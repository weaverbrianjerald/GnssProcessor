/**
 * @file NetworkAnchorHygiene.cpp
 * @brief eq34 reattach hygiene: arc ageing, eligibility verdicts and the INC-2
 *        bridge lifecycle.  The header describes the problem, the design and the
 *        ordering contract.
 */

#include "filter/NetworkAnchorHygiene.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkResponses.hpp"   // prmRespond / ResponseSite (card PRM phase 3a)
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

/// Arc ages are a monotone counter compared against a small threshold.  They are
/// clamped so a multi-day run cannot overflow and the map's values stay readable in
/// a debug dump.  The cap is far above any plausible min_arc_ep + ramp.
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
    (void)config_;   // datum modes come from ctx.knobs; config_ serves the ledger switches below
    hyg = AnchorHygiene{};   // fully overwritten every epoch (caller storage is reused)

    const bool   on          = ctx.knobs.anchor_hygiene_on;
    const int    min_arc_ep  = ctx.knobs.eq34_min_arc_ep;
    const bool   bridge_on   = ctx.knobs.pin_bridge_on;
    const int    bridge_max  = ctx.knobs.pin_bridge_max_ep;
    if (!on) return;   // off: no map, no bridge, no counter (structurally inert)

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
        const auto a = StateKeys::parseAmbiguityKey(amb);
        return a ? a->satellite : amb;
    };

    // --- (1) Age this epoch's anchorable arcs -----------------------------
    // Rebuilt wholesale: an arc absent this epoch does not appear in the new map, so
    // absence resets its age.  This is the conservative direction: a satellite
    // returning after a gap must re-earn eligibility on its new arc.  A slipped arc is
    // entered at age 0 rather than dropped, so it counts as present but fresh; either
    // way it cannot host a reattach.
    //
    // The two slip feeds are unioned because their gates are disjoint:
    // carry_slipped_ambs is the DD slip funnel (carry_invariant), and the
    // pending-reseed flag is the phase-leveling refloat (carry_warm).  Reading only
    // one would leave a configuration running the other with no invariant-#1 response
    // (the loader warns when neither is live).
    const bool resume_seed = ctx.carry.anchor_age_resume_seed;
    ctx.carry.anchor_age_resume_seed = false;   // one-shot: the first live epoch only
    const int seed_age = resume_seed ? std::max(min_arc_ep, 1) : 1;
    // The arc ledger's admitted age is the quantity this map counts, and the audit
    // assigns this container the reader role
    // (`runs/_msc/sid_audit_report_2026-09-22.md` E.2: "age is ep - ep_state_changed").
    // S2 compares the two and counts disagreements; B2 takes the ledger's answer
    // (Card SID phase 4).  Both are reachable only while this seam runs, which needs
    // network.eq34_anchor_min_arc_ep, the pin bridge or the fresh-pin ramp armed.
    // On a posture with none of them the class has a zero denominator, and the
    // run-end table reports dead wiring rather than agreement.
    const bool sid_age_read = ctx.roster.active && config_.networkLedgerScreens();
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
            const auto a = StateKeys::parseAmbiguityKey(amb);
            if (!a) continue;
            const long long led_age = anchorLedgerArcAge(
                ctx.arc_ledger, ArcLedger::Key{ si, a->satellite, a->signal }, sid_now_ep);
            if (led_age < 0) { ++sid_ctr.age_no_ledger; continue; }
            ++sid_ctr.age_compared;
            const int led_clamped = static_cast<int>(
                std::min<long long>(led_age, kArcAgeCap));
            if (led_clamped != counted) ++sid_ctr.age_disagree;
            // With the ledger anchor switch on, the container takes the ledger's age
            // where the ledger has one (B2).  An arc the ledger has never admitted
            // keeps the counted value; the alternative would seed a fresh age on an
            // arc the estimator has carried since before the ledger started observing it.
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
    // Everything below is decided per (sat,signal) key of the candidate union, using
    // three predicates evaluated once each:
    //   incumbent_live   - the current host is present and still tracks the key, so
    //                      the election keeps it: not a reattach.
    //   handover_pending - the key had a prior realization (an incumbent, an open
    //                      bridge, a hold, or a fresh emitted value) and no live
    //                      incumbent: the key must move.  This is the only case the
    //                      seam acts on; a first sighting has no level to preserve
    //                      and no frame to protect.
    //   eligible_exists  - at least one tracking arc has reached min_arc_ep.
    const bool datum_retired = ctx.carry.last_datum_changed;
    // Warm conditioning (spec addendum).  A cold pin does harm in proportion to the
    // warm memory it contradicts, for example a realization the network emitted
    // seconds earlier.  A full cold start, a restart, or a return from an outage
    // longer than the horizon has no such memory; there the ungated behaviour (pin
    // the cold arc, then converge) is correct, and blocking would only extend the
    // phase-bias outage by min_arc_ep epochs.  The block is therefore conditioned on
    // the age of the key's last emitted realization
    // (network.eq34_prior_realization_max_gap_s).
    const double now_s = TimeUtils::continuousGpsSeconds(t);
    const double prior_horizon_s = ctx.knobs.prior_realization_max_gap_s;
    auto priorFresh = [&](const std::string& amb) {
        if (!(prior_horizon_s > 0.0)) return true;   // conditioning off: every key is fresh, so the block always applies
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
        // A bridged or held-out key has no host, so it is absent from anchor_incumbent
        // while bridged or held out.  The test "was this key anchored before" therefore
        // includes both states.  Otherwise, on the epoch a bridge is dropped or the
        // epoch after a hold-out, the key would read as a first sighting (never gated)
        // and the election would pin it on the cold host it was protected from one
        // epoch earlier.
        const auto br = bridges.find(amb);
        const bool was_bridged = br != bridges.end();
        const bool was_blocked = ctx.carry.anchor_blocked.count(amb) != 0;
        // `prior_fresh` covers the case anchor_incumbent cannot: a key absent for a few
        // epochs drops out of the incumbent map (each election replaces it wholesale),
        // so on its return it would read as a first sighting.  That is the shape of the
        // failure this seam guards: a satellite rising after a gap onto a fresh arc
        // while the network emitted its previous realization shortly before.
        const bool prior_fresh = priorFresh(amb);
        const bool handover_pending =
            (has_incumbent || was_bridged || was_blocked || prior_fresh) && !incumbent_live;

        // (2a) An open bridge: apply the invariant-#1 event responses (slip, datum,
        // stale) first, then the eligible-host test ("healed").  A slip on a key that has
        // also regained an eligible host is counted as a slip drop; the held level is
        // invalid either way, and the run-end counters then record the cause.
        bool reopen_barred = false;
        if (was_bridged) {
            const char* drop = nullptr;
            if (slipped_any)            { drop = "slip";  ++ctr.hyg.bridge_drop_slip; }
            else if (datum_retired)     { drop = "datum"; ++ctr.hyg.bridge_drop_datum;
                                          // Card PRM phase 3a: the bridge erased below.
                                          prmRespond(ctr.prm_resp,
                                                     ResponseSite::PbPinBridgeDiscard); }
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
                // path an eligible host exists, so the election reattaches normally.
                // On slip, datum or stale, the held value is invalid while the key may
                // still lack an eligible host.  Re-opening a bridge on the same
                // (slipped, retired or expired) level would bypass the staleness bound
                // and the invariant-#1 response, so reopening is barred this epoch and
                // the key goes to the INC-1 eligibility verdict instead.
                reopen_barred = std::string(drop) != "healed";
            } else {
                hyg.bridged.insert(amb);
                continue;   // still bridged: the election must not pin it
            }
        }

        const bool cold_hosts_only = min_arc_ep > 0 && !eligible_exists;
        if (!handover_pending) {
            // Incumbent live, or a first sighting with no warm memory.  The second case
            // is the restart path, one of the two places a cold pin is allowed, and it
            // is counted so that it stays visible.
            if (cold_hosts_only && !prior_fresh)
                ++ctr.hyg.reattach_cold_allowed_no_prior;
            ctx.carry.anchor_blocked.erase(amb);
            continue;
        }

        // (2b) A reattach is required.  Bridge if armed and applicable; else block if
        // INC-1 says no host has converged; else let the election pick (it prefers an
        // eligible host by construction).  The bridge is also warm-conditioned: with no
        // fresh realization there is no level worth holding, and pb_level_prev would be
        // stale or absent.
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
                ctx.carry.anchor_blocked.erase(amb);   // a bridge supersedes a hold-out
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
            // The warm memory has expired or never existed, so a cold pin contradicts
            // nothing and is allowed; blocking would extend a restart's phase-bias
            // outage by min_arc_ep epochs.  Each such pin is counted.
            ++ctr.hyg.reattach_cold_allowed_no_prior;
            ctx.carry.anchor_blocked.erase(amb);
        } else if (cold_hosts_only) {
            // Hold the key out, and keep holding until a host converges.  The hold is
            // entered once (counted as one refused reattach) and its duration is
            // counted separately in key-epochs: one key blocked for 300 epochs and 300
            // keys blocked once give the same single count, and the duration counter
            // separates them.  Only the entry emits an event row, so the events CSV
            // records transitions rather than one row per epoch.
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
            // election re-hosts normally.  The release is traced so the hold's duration
            // can be read from network_events.csv without differencing counters.
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
    // the coverage gate, or a station roster change).  A held value for a key the solve
    // no longer contains has no meaning, so the bridge is dropped now rather than left
    // to age out.
    for (auto it = bridges.begin(); it != bridges.end();) {
        if (cnd.union_keys.count(it->first)) { ++it; continue; }
        ++ctr.hyg.bridge_drop_leftset;
        emit("pin_bridge_drop", satOf(it->first), "",
             static_cast<double>(it->second.folds),
             static_cast<double>(bridge_max), "left_set", it->first);
        it = bridges.erase(it);
    }

    // --- (3) Realize the block: the key leaves the anchored union ---------
    // The same disposition as the coverage gate (NetworkPhaseDatum
    // networkBuildPhaseDatumCandidates): erase from union_keys so no shared column is
    // registered and no election can pin it, then prune each tracking station's
    // phase-bias state so the per-station column sets stay consistent with the gated
    // union.  The key's phase rows still serve the satellite clock (each station's
    // float ambiguity absorbs the bias, as for a non-shared signal), and the emitter
    // sees a per-key absence (staleness / first-sighting semantics on return).
    if (!blocked.empty()) {
        for (const auto& amb : blocked) cnd.union_keys.erase(amb);
        for (std::size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || !stations[i].sm) continue;
            StateManager& sm = *stations[i].sm;
            std::set<std::string> keep_pb;
            bool drop_any = false;
            for (const auto& kv : sm.getSatPhaseBiasStates()) {
                const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                if (p && blocked.count(StateKeys::makeAmbiguityKey(p->satellite, p->signal))) {
                    drop_any = true;
                    // The parameter inventory's absorbed instance (card NOM N1b).
                    if (stations[i].mm)
                        stations[i].mm->noteAbsorbedSharedState(
                            StateContainer::SatPhaseBiasStates, kv.first,
                            AbsorbedReason::AnchorHygieneBlock);
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
