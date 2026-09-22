/**
 * @file NetworkAnchorHygiene.cpp
 * @brief eq34 reattach hygiene — arc ageing, eligibility verdicts and the INC-2
 *        bridge lifecycle.  See the header for the defect, the design and the
 *        ordering contract.
 */

#include "filter/NetworkAnchorHygiene.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"   // GnssTime (event stamps)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace GnssProcessor {

namespace {

/// Arc ages are a MONOTONE counter compared against a small threshold; clamp them so
/// a multi-day run cannot overflow and so the map's values stay human-readable in a
/// debug dump.  Far above any plausible min_arc_ep + ramp.
constexpr int kArcAgeCap = 1'000'000;

} // namespace

void networkAnchorHygiene(NetworkRunContext& ctx,
                          const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces,
                          const GnssTime& t,
                          std::vector<NetStation>& stations,
                          const std::vector<bool>& present,
                          const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                          PhaseDatumCandidates& cnd,
                          AnchorHygiene& hyg) {
    (void)config_;   // mode predicates are already resolved into ctx.knobs
    hyg = AnchorHygiene{};   // fully overwritten every epoch (caller storage is reused)

    const bool   on          = ctx.knobs.anchor_hygiene_on;
    const int    min_arc_ep  = ctx.knobs.eq34_min_arc_ep;
    const bool   bridge_on   = ctx.knobs.pin_bridge_on;
    const int    bridge_max  = ctx.knobs.pin_bridge_max_ep;
    if (!on) return;   // OFF: no map, no bridge, no counter — structurally inert

    auto& age_map  = ctx.carry.anchor_arc_age;
    auto& bridges  = ctx.carry.pin_bridge;
    auto& levels   = ctx.carry.pb_level_prev;
    auto& ctr      = ctx.counters;
    ++ctr.hyg.epochs;

    hyg.active            = true;
    hyg.min_arc_ep        = min_arc_ep;
    hyg.fresh_pin_scale   = ctx.knobs.fresh_pin_scale;
    hyg.fresh_pin_ramp_ep = ctx.knobs.fresh_pin_ramp_ep;

    auto emit = [&](const char* event, const std::string& sat,
                    const std::string& station, std::optional<double> statistic,
                    std::optional<double> threshold, const char* action,
                    const std::string& detail) {
        traces.emitEvent(t, "PHASE_DATUM", event, sat, station, statistic, threshold,
                         action, detail);
    };
    auto satOf = [](const std::string& amb) {
        const auto us = amb.find('_');
        return us == std::string::npos ? amb : amb.substr(0, us);
    };

    // --- (1) Age this epoch's anchorable arcs -----------------------------
    // Rebuilt wholesale: an arc absent this epoch simply does not appear in the new
    // map, so absence resets its age — the conservative direction (a returning arc
    // must re-earn eligibility, which is exactly the 2138-epoch-gap case that broke
    // DOY-129).  A slipped arc is entered at age 0 rather than dropped, so it is
    // counted as present-but-fresh; either way it cannot host a reattach.
    //
    // Slip feeds are UNIONED because they have disjoint gates: carry_slipped_ambs is
    // the Card-4 funnel (carry_invariant), the pending-reseed flag is the
    // phase-leveling refloat (carry_warm).  Reading only one would leave a stack
    // running the other with no invariant-#1 response at all (the loader warns when
    // NEITHER is live).
    const bool resume_seed = ctx.carry.anchor_age_resume_seed;
    ctx.carry.anchor_age_resume_seed = false;   // one-shot: the first live epoch only
    const int seed_age = resume_seed ? std::max(min_arc_ep, 1) : 1;
    // Card SID phase 4 (S2 / B2): the ledger's ADMITTED age is the same quantity
    // this map counts, and the audit's disposition for the container is READER
    // (`runs/_msc/sid_audit_report_2026-09-22.md` E.2: "age is ep - ep_state_changed").
    // S2 COMPARES the two and counts the disagreement; B2 takes the ledger's answer.
    // ⚠ Both are reachable only while this seam runs at all, which needs
    // network.eq34_anchor_min_arc_ep, the pin bridge or the fresh-pin ramp armed —
    // so on a posture with none of them the class has a ZERO DENOMINATOR and the
    // run-end table says dead wiring rather than agreement.
    const bool sid_age_read = ctx.roster.armed && config_.networkLedgerScreens();
    const bool sid_age_use  = sid_age_read && config_.networkLedgerAnchor();
    const long long sid_now_ep = ctx.arc_ledger.epochIndex();
    auto& sid_ctr = ctx.counters.sid;
    std::map<std::pair<int, std::string>, int> new_age;
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (!present[i] || !stations[i].sm) continue;
        const int si = static_cast<int>(i);
        for (const auto& [amb, pbkey] : cnd.usable_keys[i]) {
            (void)pbkey;
            const bool slipped = carry_slipped_ambs.count({si, amb}) != 0
                              || stations[i].sm->isAmbiguityPendingReseed(amb);
            if (slipped) {
                new_age[{si, amb}] = 0;   // refloated ambiguity: fresh by definition
                continue;
            }
            const auto prev = age_map.find({si, amb});
            const int counted = prev == age_map.end()
                ? seed_age
                : std::min(prev->second + 1, kArcAgeCap);
            new_age[{si, amb}] = counted;
            if (!sid_age_read) continue;
            const std::size_t us = amb.find('_');
            if (us == std::string::npos || us == 0 || us + 1 >= amb.size()) continue;
            const long long led_age = anchorLedgerArcAge(
                ctx.arc_ledger,
                ArcLedger::Key{ si, amb.substr(0, us), amb.substr(us + 1) },
                sid_now_ep);
            if (led_age < 0) { ++sid_ctr.age_no_ledger; continue; }
            ++sid_ctr.age_compared;
            const int led_clamped = static_cast<int>(
                std::min<long long>(led_age, kArcAgeCap));
            if (led_clamped != counted) ++sid_ctr.age_disagree;
            // B2: the container becomes a READER.  Taken only where the ledger can
            // answer; an arc it has never admitted keeps the counted value, which is
            // the conservative direction (the alternative would seed a fresh age on
            // an arc the estimator has been carrying since before the observatory).
            if (sid_age_use) {
                new_age[{si, amb}] = led_clamped;
                ++sid_ctr.age_from_ledger;
            }
        }
    }
    ctr.hyg.arcs_tracked += static_cast<long long>(new_age.size());
    age_map.swap(new_age);
    hyg.age = &age_map;
    if (resume_seed)
        log->info("Network (eq34 hygiene): warm resume — seeded {} arc age(s) at {} "
                  "epoch(s) (arcs behind a snapshot are mature by definition; the age "
                  "map is not serialized).", age_map.size(), seed_age);

    // --- (2) Per-key verdicts + bridge lifecycle --------------------------
    // Everything below is decided per (sat,signal) key of the CANDIDATE UNION, using
    // three predicates evaluated once each:
    //   incumbent_live  — the current host is present and still tracks the key, so
    //                     the election will simply keep it: NOT a reattach.
    //   handover_pending— an incumbent EXISTS but is not live: the key must move, and
    //                     this is the only situation the card acts on (a first
    //                     sighting has no level to preserve and no frame to protect).
    //   eligible_exists — at least one tracking arc has cleared min_arc_ep.
    const bool datum_retired = ctx.carry.last_datum_changed;
    // WARM CONDITIONING (spec addendum).  A cold pin is dangerous only in proportion to
    // the warm memory it has to fight: the DOY-129 break was a cold host contradicting a
    // realization the network had been emitting seconds earlier.  A full cold start, a
    // restart, or a return from an outage longer than the horizon has no such memory —
    // there the legacy behaviour (pin the cold arc, converge) is CORRECT, and blocking
    // would merely extend the phase-bias outage by min_arc_ep epochs.  So the block is
    // conditioned on the age of the key's last EMITTED realization.
    const double now_s = TimeUtils::continuousGpsSeconds(t);
    const double prior_horizon_s = ctx.knobs.prior_realization_max_gap_s;
    auto priorFresh = [&](const std::string& amb) {
        if (!(prior_horizon_s > 0.0)) return true;   // conditioning disabled ⇒ always block
        const auto it = ctx.carry.emitted_anchor_s.find(amb);
        if (it == ctx.carry.emitted_anchor_s.end()) return false;   // never emitted
        const double age = now_s - it->second;
        return age >= 0.0 && age <= prior_horizon_s;
    };
    std::set<std::string> blocked;    // keys leaving the anchored set this epoch
    for (const auto& [amb, pbkey] : cnd.union_keys) {
        (void)pbkey;
        const auto inc = ctx.carry.anchor_incumbent.find(amb);
        const bool has_incumbent = inc != ctx.carry.anchor_incumbent.end();
        bool incumbent_live = false;
        bool eligible_exists = false;
        bool slipped_any = false;
        std::size_t best_i = static_cast<std::size_t>(-1);
        int best_age = -1;
        for (std::size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || !cnd.usable_keys[i].count(amb)) continue;
            const int a = hyg.arcAge(i, amb);
            if (a > best_age) { best_age = a; best_i = i; }
            if (a >= min_arc_ep) eligible_exists = true;
            if (has_incumbent && stations[i].name == inc->second) incumbent_live = true;
            if (a == 0
                && (carry_slipped_ambs.count({static_cast<int>(i), amb}) != 0
                    || (stations[i].sm
                        && stations[i].sm->isAmbiguityPendingReseed(amb))))
                slipped_any = true;
        }
        // A bridged or held-out key has NO host, so it drops out of anchor_incumbent
        // while it is bridged / held out.  "Was this key anchored before?" must
        // therefore include BOTH states — otherwise the epoch a bridge is dropped, or
        // the epoch after a hold-out, the key would read as a FIRST SIGHTING (which is
        // deliberately never gated) and the election would happily pin it on the very
        // cold host it was just protected from, one epoch later.
        const auto br = bridges.find(amb);
        const bool was_bridged = br != bridges.end();
        const bool was_blocked = ctx.carry.anchor_blocked.count(amb) != 0;
        // `prior_fresh` also carries the case anchor_incumbent CANNOT: a key absent for
        // a few epochs drops out of the incumbent map entirely (it is replaced wholesale
        // by each election), so on its return it would read as a first sighting — which
        // is exactly the DOY-129 shape, a satellite rising after a gap onto a fresh arc
        // while the network was still emitting its old realization moments before.
        const bool prior_fresh = priorFresh(amb);
        const bool handover_pending =
            (has_incumbent || was_bridged || was_blocked || prior_fresh) && !incumbent_live;

        // (2a) An OPEN bridge: apply the invariant-#1 event response FIRST, then the
        // heal test.  Order matters — a slip on a key that ALSO regained an eligible
        // host must be charged to the slip (the level is poison either way, but the
        // counters are how the field run is read).
        bool reopen_barred = false;
        if (was_bridged) {
            const char* drop = nullptr;
            if (slipped_any)            { drop = "slip";  ++ctr.hyg.bridge_drop_slip; }
            else if (datum_retired)     { drop = "datum"; ++ctr.hyg.bridge_drop_datum; }
            else if (br->second.folds >= bridge_max)
                                        { drop = "stale"; ++ctr.hyg.bridge_drop_stale; }
            else if (eligible_exists && min_arc_ep > 0)
                                        { drop = "healed"; ++ctr.hyg.bridge_drop_healed; }
            if (drop) {
                emit("pin_bridge_drop", satOf(amb), "",
                     static_cast<double>(br->second.folds),
                     static_cast<double>(bridge_max), drop, amb);
                bridges.erase(br);
                // A dropped bridge leaves the key to the logic below.  On the "healed"
                // path that is exactly what we want — an eligible host exists, so the
                // election reattaches normally.  On slip / datum / stale the HELD
                // VALUE is what became invalid, not the situation: re-opening a bridge
                // on the same (now poisoned, retired or time-expired) level would make
                // the staleness bound and the invariant-#1 response cosmetic, so the
                // key falls through to the INC-1 hold-out instead.
                reopen_barred = std::string(drop) != "healed";
            } else {
                hyg.bridged.insert(amb);
                continue;   // still bridged: the election must not pin it
            }
        }

        const bool cold_hosts_only = min_arc_ep > 0 && !eligible_exists;
        if (!handover_pending) {
            // Incumbent live, or a genuine first sighting with no warm memory.  The
            // second case is the RESTART path and must be visible rather than silent:
            // it is the one place a cold pin is deliberately allowed.
            if (cold_hosts_only && !prior_fresh)
                ++ctr.hyg.reattach_cold_allowed_no_prior;
            ctx.carry.anchor_blocked.erase(amb);
            continue;
        }

        // (2b) A reattach is required.  Bridge if armed and applicable; else block if
        // INC-1 says no host has converged; else let the election pick (it prefers an
        // eligible host by construction).  The bridge is warm-conditioned too — with no
        // fresh realization there is no level worth holding (and pb_level_prev would be
        // stale or absent anyway).
        const bool want_bridge = bridge_on && !reopen_barred && prior_fresh
                                 && (min_arc_ep <= 0 || !eligible_exists);
        if (want_bridge) {
            const auto lv = levels.find(amb);
            if (lv != levels.end() && std::isfinite(lv->second)) {
                NetworkCarryState::PinBridge nb;
                nb.value      = lv->second;
                nb.folds      = 0;
                nb.opened_tow = t.tow;
                bridges.emplace(amb, nb);
                hyg.bridged.insert(amb);
                ctx.carry.anchor_blocked.erase(amb);   // a bridge SUPERSEDES a hold-out
                ++ctr.hyg.bridges_opened;
                emit("pin_bridge_open", satOf(amb),
                     has_incumbent ? inc->second : std::string{},
                     lv->second, ctx.knobs.pin_bridge_sigma, "bridge",
                     amb + " held at last solved level");
                continue;
            }
            // No harvested level (the key has never been solved, or the last kept
            // epoch did not carry it).  There is nothing to hold, so fall through:
            // block if INC-1 demands it, otherwise let the election proceed.
            log->debug("Network (eq34 hygiene): {} wants a bridge but has no solved "
                       "level yet — falling back to the eligibility verdict.", amb);
        }

        if (cold_hosts_only && !prior_fresh) {
            // The warm memory has expired (or never existed): a cold pin has nothing
            // left to contradict, so allow it — a restart must not extend its own
            // phase-bias outage by min_arc_ep epochs.  Counted, never silent.
            ++ctr.hyg.reattach_cold_allowed_no_prior;
            ctx.carry.anchor_blocked.erase(amb);
        } else if (cold_hosts_only) {
            // HOLD OUT — and keep holding until a host converges.  The hold is entered
            // once (counted as one refused reattach) and its DURATION is counted
            // separately: a guard that blocks one key for 300 epochs and one that
            // blocks 300 keys once are very different field events, and a single
            // counter cannot tell them apart.  Only the entry emits an event row, so
            // the events CSV records transitions rather than a per-epoch flood.
            blocked.insert(amb);
            ++ctr.hyg.blocked_key_epochs;
            if (ctx.carry.anchor_blocked.insert(amb).second) {
                ++ctr.hyg.reattach_blocked_cold;
                emit("reattach_blocked_cold", satOf(amb),
                     best_i == static_cast<std::size_t>(-1) ? std::string{}
                                                            : stations[best_i].name,
                     best_age < 0 ? std::optional<double>{}
                                  : std::optional<double>(static_cast<double>(best_age)),
                     static_cast<double>(min_arc_ep), "hold_out",
                     amb + " from " + (has_incumbent ? inc->second : std::string("-")));
            }
        } else if (ctx.carry.anchor_blocked.erase(amb)) {
            // A converged host appeared (or INC-1 is off): the hold releases and the
            // election re-hosts normally.  Traced so the hold's DURATION is readable
            // from network_events.csv without differencing counters.
            ++ctr.hyg.blocks_released;
            emit("reattach_unblocked", satOf(amb),
                 best_i == static_cast<std::size_t>(-1) ? std::string{}
                                                        : stations[best_i].name,
                 best_age < 0 ? std::optional<double>{}
                              : std::optional<double>(static_cast<double>(best_age)),
                 static_cast<double>(min_arc_ep), "release",
                 amb + " eligible host available");
        }
    }

    // (2c) Bridges whose key left this epoch's shared set entirely (the satellite set,
    // the coverage gate, or a station roster change).  Held values in a set the solve
    // no longer contains are meaningless — drop rather than let them age out.
    for (auto it = bridges.begin(); it != bridges.end();) {
        if (cnd.union_keys.count(it->first)) { ++it; continue; }
        ++ctr.hyg.bridge_drop_leftset;
        emit("pin_bridge_drop", satOf(it->first), "",
             static_cast<double>(it->second.folds),
             static_cast<double>(bridge_max), "left_set", it->first);
        it = bridges.erase(it);
    }

    // --- (3) Realize the BLOCK: the key leaves the anchored union ---------
    // Byte-for-byte the coverage gate's disposition (NetworkPhaseDatum
    // networkBuildPhaseDatumCandidates): erase from union_keys so no shared column is
    // registered and no election can pin it, then prune each tracking station's
    // phase-bias state so the per-station column sets stay consistent with the gated
    // union.  Its phase rows still serve the satellite clock — each station's float
    // ambiguity absorbs the bias, exactly as a legacy non-shared signal always has —
    // and the emitter sees a clean per-key absence (staleness / first-sighting
    // semantics on return).
    if (!blocked.empty()) {
        for (const auto& amb : blocked) cnd.union_keys.erase(amb);
        for (std::size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || !stations[i].sm) continue;
            StateManager& sm = *stations[i].sm;
            std::set<std::string> keep_pb;
            bool drop_any = false;
            for (const auto& kv : sm.getSatPhaseBiasStates()) {
                const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                if (p && blocked.count(p->satellite + "_" + p->signal)) {
                    drop_any = true;
                    continue;
                }
                keep_pb.insert(kv.first);
            }
            if (drop_any) sm.pruneSatPhaseBiasStatesExcept(keep_pb);
        }
        log->debug("Network (eq34 hygiene): {} key(s) held out of the anchored set — "
                   "every candidate host younger than {} epoch(s).",
                   blocked.size(), min_arc_ep);
    }
}

} // namespace GnssProcessor
