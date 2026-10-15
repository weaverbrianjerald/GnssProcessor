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

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include <memory>

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

    net::SatSignalTable& sigs = ctx.entities.sat_signals;   // pin_bridge, pb_level_prev, anchor_incumbent (N4d)
    using SG = net::SatSignalState;
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
            const int* prev = net::getArcAmb(ctx.entities.arcs, si, amb, &net::ArcState::anchor_arc_age);
            const int counted = prev == nullptr
                ? seed_age
                : std::min(*prev + 1, kArcAgeCap);
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
    net::assignArcAmb(ctx.entities, &net::ArcState::anchor_arc_age, new_age);   // rebuilt wholesale
    hyg.age = &ctx.entities.arcs;
    if (resume_seed)
        log->info("Network (eq34 hygiene): warm resume — seeded {} arc age(s) at {} "
                  "epoch(s) (arcs behind a snapshot are mature by definition; the age "
                  "map is not serialized).", new_age.size(), seed_age);

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
        const double* emitted_s =
            net::getSig(ctx.entities.sat_signals, amb, &net::SatSignalState::emitted_anchor_s);
        if (emitted_s == nullptr) return false;   // never emitted
        const double age = now_s - *emitted_s;
        return age >= 0.0 && age <= prior_horizon_s;
    };
    std::set<std::string> blocked;    // keys leaving the anchored set this epoch
    for (const auto& [amb, pbkey] : cnd.union_keys) {
        (void)pbkey;
        const std::string* inc = net::getSig(sigs, amb, &SG::anchor_incumbent);
        const bool has_incumbent = inc != nullptr;
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
            if (has_incumbent && stations[i].name == *inc) incumbent_live = true;
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
        net::PinBridge* br = net::getSig(sigs, amb, &SG::pin_bridge);
        const bool was_bridged = br != nullptr;
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
            else if (br->folds >= bridge_max)
                                        { drop = "stale"; ++ctr.hyg.bridge_drop_stale; }
            else if (eligible_exists && min_arc_ep > 0)
                                        { drop = "healed"; ++ctr.hyg.bridge_drop_healed; }
            if (drop) {
                emit("pin_bridge_drop", satOf(amb), "",
                     static_cast<double>(br->folds),
                     static_cast<double>(bridge_max), drop, amb);
                net::dropSig(sigs, amb, &SG::pin_bridge);
                br = nullptr;
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
            const double* lv = net::getSig(sigs, amb, &SG::pb_level_prev);
            if (lv && std::isfinite(*lv)) {
                net::PinBridge nb;
                nb.value      = *lv;
                nb.folds      = 0;
                nb.opened_tow = t.tow;
                net::emplaceSig(ctx.entities, amb, &SG::pin_bridge, nb);
                hyg.bridged.insert(amb);
                ctx.carry.anchor_blocked.erase(amb);   // a bridge supersedes a hold-out
                ++ctr.hyg.bridges_opened;
                emit("pin_bridge_open", satOf(amb),
                     has_incumbent ? *inc : std::string{},
                     *lv, ctx.knobs.pin_bridge_sigma, "bridge",
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
                     amb + " from " + (has_incumbent ? *inc : std::string("-")));
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
    net::eraseIf(sigs, &SG::pin_bridge, [&](const SatSignalId& id, const net::PinBridge& b) {
        const std::string key = net::ambKey(id);
        if (cnd.union_keys.count(key)) return false;
        ++ctr.hyg.bridge_drop_leftset;
        emit("pin_bridge_drop", satOf(key), "",
             static_cast<double>(b.folds),
             static_cast<double>(bridge_max), "left_set", key);
        return true;
    });

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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: phase-anchor hygiene: handover, cold fallback, PHASE_ANCHOR re-reference, eq34.
int networkAnchorHygieneRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // --- Two-edge continuous handover run-end telemetry --------------------------
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.1).  The denominator is
    // `reattaches`, the number of gauge changes the run made at all, which is counted
    // whether or not the fix is armed: a run with zero handovers says nothing about the
    // mechanism (invariant #4).  The two edges are then split: `handover_continuous`
    // counts the frame-preserving handovers and `handover_cold` the unavoidable
    // re-realizations.  A cold handover is not a failure; it reports a frame step the
    // consumer has to be told about.
    if (ctx.knobs.pd_handover_continuous) {
        const auto& h = ctx.counters.hyg;
        const long long total = h.handover_continuous + h.handover_cold;
        log->info("Network (PB-GAUGE HANDOVER, two-edge): {} anchor handover(s) over the "
                  "run — {} made CONTINUOUS (pinned at the successor's own value in the "
                  "outgoing frame; largest |target| {:.4f} m, which is the size of the "
                  "frame step that was AVOIDED) and {} COLD (no ledger entry for the "
                  "successor — a fresh arc or the first epoch — pinned at 0, so the "
                  "frame DID step there); {} of the cold were REFUSED ledger values "
                  "(conditional variance above the {} m² ceiling — the invariant-#7 "
                  "re-validation, not a miss).",
                  h.reattaches, h.handover_continuous, h.handover_max_abs_m,
                  h.handover_cold, h.handover_refused_var,
                  ctx.knobs.pd_handover_max_var);
        if (ctx.knobs.pd_handover_max_var_ratio > 0.0)
            log->info("Network (PB-GAUGE HANDOVER, ratio ceiling): {} ledger value(s) REFUSED by "
                      "the honest-sigma-scale test var <= {} x var_first (a subset of the cold "
                      "count, disjoint from the {} absolute-ceiling refusals; an unknown "
                      "var_first counts here).",
                      h.handover_refused_ratio, ctx.knobs.pd_handover_max_var_ratio,
                      h.handover_refused_var);
        if (h.reattaches > 0 && total == 0)
            log->warn("Network (PB-GAUGE HANDOVER): DEAD WIRING — {} handover(s) were "
                      "elected but NONE reached the retarget block.  The knob reports "
                      "ARMED and every pin still went in at 0, so every one of those "
                      "gauge changes re-realized the frame.",
                      h.reattaches);
        else if (total > 0 && h.handover_continuous == 0)
            log->warn("Network (PB-GAUGE HANDOVER): {} handover(s) and NOT ONE was "
                      "continuous — every successor was missing from the ledger.  That "
                      "is the harvest not running (the census `qc_amb` is empty, or the "
                      "solve reconstruction failed), not a property of the data.",
                      total);
        else if (h.reattaches == 0)
            log->info("Network (PB-GAUGE HANDOVER): ZERO handovers this run — the anchor "
                      "set never moved, so this arm exercises the mechanism NOT AT ALL.  "
                      "That is a quiet window, not evidence the fix works.");
    }

    // --- Cold anchor-fallback run-end telemetry (docs/SPEC_event_harness.md §15) --
    // Three populations together describe what the pass did (invariant #4):
    // usable_elected (the pass elects a usable candidate), rank_changed (that candidate
    // is not the station the coverage sweep takes first; the population the pass
    // targets), and no_usable_fallback (no candidate is usable, so the sweeps' cold path
    // runs).  The four skip counters partition the skipped total by the reason for each
    // refusal, which separates "the ledger is thin" from "the maturity bar is too tight".
    if (ctx.knobs.pd_candidacy_usable) {
        const auto& h = ctx.counters.hyg;
        const long long entered = h.candidacy_usable_elected
                                + h.candidacy_no_usable_fallback;
        log->info("Network (COLD ANCHOR-FALLBACK): the usable-first candidacy pass ran on "
                  "{} handover election(s) — {} elected a candidate whose carry is USABLE "
                  "(of which {} were NOT the station today's sweep would have taken: the "
                  "cold re-realizations this card removed) and {} found NO usable "
                  "candidate and fell through to today's sweeps (the honest cold path).  "
                  "{} (candidate, family) pair(s) were skipped: {} no ledger entry, {} "
                  "refused by the variance/ratio ceiling, {} freed this epoch, {} younger "
                  "than {} epochs.",
                  entered, h.candidacy_usable_elected, h.candidacy_rank_changed,
                  h.candidacy_no_usable_fallback, h.candidacy_skipped_unusable,
                  h.candidacy_skip_no_entry, h.candidacy_skip_var_refused,
                  h.candidacy_skip_freed, h.candidacy_skip_young,
                  ctx.knobs.pd_candidacy_min_age_ep);
        if (entered == 0)
            log->warn("Network (COLD ANCHOR-FALLBACK): ARMED but the pass never ran — no "
                      "election this run reached a handover with an incumbent to hand "
                      "over FROM.  The knob is inert on this window; nothing it reports "
                      "is evidence either way (invariant #4).");
        else if (h.candidacy_rank_changed == 0)
            log->info("Network (COLD ANCHOR-FALLBACK): the pass ran on {} election(s) and "
                      "changed NOT ONE of them — every usable candidate was already the "
                      "station today's coverage sweep takes first.  Byte-identical to OFF "
                      "on this window by construction, not by luck.",
                      entered);
        // Satellite clause, iteration 2 (docs/SPEC_event_harness.md §15.8).  Rule (c):
        // the distinct-anchor census is not recomputed here; it is read from the
        // always-on counters the storm grade also reads (networkSplitReferencedSats, run
        // every epoch whatever the knobs), so the clause and the grade agree by
        // construction.
        if (ctx.knobs.pd_candidacy_satellite) {
            const auto& C = ctx.counters;
            const double ep = C.n_cb_split_epochs > 0
                            ? static_cast<double>(C.n_cb_split_epochs) : 1.0;
            log->info("Network (COLD ANCHOR-FALLBACK, SATELLITE clause): {} satellite-epoch(s) "
                      "resolved jointly over {} family(ies) — {} decided by the held-host "
                      "preference (a PARTIAL handover co-located on the station already "
                      "anchoring the satellite's other families), {} where the family-only "
                      "rule would have picked a DIFFERENT station (the split references this "
                      "clause removed), {} pinned at the joint host through the COLD path (a "
                      "frame step, not a split), {} the joint host does not TRACK (the only "
                      "residual split this clause can leave), and {} satellite-epoch(s) with "
                      "no usable candidate for ANY mover.  Resulting SPLIT-REFERENCED census "
                      "(the storm grade's own numbers): mean {:.2f} / max {} satellites of "
                      "{:.1f} anchored, over {} epoch(s).",
                      h.sat_clause_satellites, h.sat_clause_families,
                      h.sat_clause_colocated, h.sat_clause_split_avoided,
                      h.sat_clause_cold_colocated, h.sat_clause_host_untracked,
                      h.sat_clause_no_usable,
                      static_cast<double>(C.n_cb_split_sats_sum) / ep,
                      C.n_cb_split_sats_max,
                      static_cast<double>(C.n_cb_split_multi_sum) / ep,
                      C.n_cb_split_epochs);
            if (h.sat_clause_families > 0 && h.sat_clause_split_avoided == 0)
                log->info("Network (COLD ANCHOR-FALLBACK, SATELLITE clause): the clause ran on "
                          "{} family(ies) and the family-only rule would have chosen the SAME "
                          "station every time — this window had no split to avoid, which is "
                          "not evidence the clause works.",
                          h.sat_clause_families);
            if (h.sat_clause_host_untracked > 0)
                log->warn("Network (COLD ANCHOR-FALLBACK, SATELLITE clause): {} family(ies) "
                          "could not be co-located because the joint host does not track their "
                          "key — those are the ONLY families that can still split their "
                          "satellite's reference.",
                          h.sat_clause_host_untracked);
        }
    }

    // --- PHASE_ANCHOR carry re-reference run-end telemetry (queue item 1(d)) ------
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  As in the handover
    // block above, `reattaches` is the denominator (invariant #4), and an armed run
    // whose elections handed over while the re-reference set never populated is
    // reported as dead wiring.
    if (ctx.knobs.pa_carry_reref) {
        const auto& h = ctx.counters.hyg;
        log->info("Network (PHASE_ANCHOR carry reref, 1(d)): {} marked "
                  "(sat,signal) family-epoch(s) — handovers plus incumbent-less "
                  "pins (first sightings / bridge exits; no-ops without a "
                  "surviving carry) — marginalized for one epoch over the run "
                  "({} anchor handover(s) elected); {} phase-leveling local "
                  "carry(ies) erased.",
                  ctx.carry.pa_reref_total, h.reattaches,
                  ctx.carry.pa_reref_erase_total);
        if (h.reattaches > 0 && ctx.carry.pa_reref_total == 0)
            log->warn("Network (PHASE_ANCHOR carry reref): DEAD WIRING — {} "
                      "handover(s) were elected but the re-reference set never "
                      "populated, so every carried PB prior still folded in its "
                      "retired frame (the exact §5.07 defect this knob exists to "
                      "close).",
                      h.reattaches);
        else if (h.reattaches == 0)
            log->info("Network (PHASE_ANCHOR carry reref): ZERO handovers this run — "
                      "the mechanism was never exercised.  A quiet window, not "
                      "evidence.");
    }

    // --- eq34 reattach hygiene run-end telemetry -------------------------------
    // (docs/SPEC_eq34_reattach_hygiene.md).  An armed run whose gate never ran is a
    // different failure from one that ran and found nothing to block (invariant #4), so
    // the denominators (`epochs`, `arcs_tracked`) are reported first and an all-zero
    // feed gets a warning rather than looking quiet.
    if (ctx.knobs.anchor_hygiene_on) {
        const auto& h = ctx.counters.hyg;
        log->info("Network (eq34 hygiene): {} election pass(es), {} arc-epoch(s) aged; "
                  "{} reattach(es) performed, {} BLOCKED cold (min_arc_ep {}) held for "
                  "{} key-epoch(s), {} released on a converged host; {} unavoidable "
                  "fresh pin(s) taken for connectivity, {} sigma-ramped pin(s).",
                  h.epochs, h.arcs_tracked, h.reattaches, h.reattach_blocked_cold,
                  ctx.knobs.eq34_min_arc_ep, h.blocked_key_epochs, h.blocks_released,
                  h.cold_fallback_pins, h.fresh_pin_ramps);
        log->info("Network (eq34 hygiene): {} cold key-epoch(s) ALLOWED with no fresh "
                  "prior realization (warm-conditioning horizon {:.0f} s) — the "
                  "restart / long-outage path, where a cold pin has no warm memory to "
                  "fight and blocking would only extend the outage.  A cold-start run "
                  "is EXPECTED to show a large number here and zero blocks.",
                  h.reattach_cold_allowed_no_prior,
                  ctx.knobs.prior_realization_max_gap_s);
        if (h.reattach_blocked_cold > 0 && h.blocks_released == 0)
            log->warn("Network (eq34 hygiene): {} hold(s) entered and NONE released — "
                      "every held-out key stayed out for the whole run.  That is only "
                      "correct if those arcs never reached {} continuous epochs; "
                      "otherwise the release path is not firing and the guard is "
                      "silently withholding columns.",
                      h.reattach_blocked_cold, ctx.knobs.eq34_min_arc_ep);
        if (ctx.knobs.pin_bridge_on)
            log->info("Network (eq34 hygiene INC-2): {} bridge(s) opened, {} row-fold(s) "
                      "(sigma {:g} m, margin {:.1f} decades over the rank-gate cutoff, "
                      "max {} epoch(s) each); dropped slip {} / datum {} / stale {} / "
                      "left_set {}, healed {}.",
                      h.bridges_opened, h.bridge_folds, ctx.knobs.pin_bridge_sigma,
                      h.bridge_margin_decades, ctx.knobs.pin_bridge_max_ep,
                      h.bridge_drop_slip, h.bridge_drop_datum, h.bridge_drop_stale,
                      h.bridge_drop_leftset, h.bridge_drop_healed);
        if (h.epochs == 0)
            log->warn("Network (eq34 hygiene): ARMED but the seam ran on ZERO epochs — "
                      "the wiring never executed (invariant #4).  Check that a "
                      "prioritized / eq34 phase datum is actually active.");
        else if (h.arcs_tracked == 0)
            log->warn("Network (eq34 hygiene): ARMED and the seam ran, but it aged ZERO "
                      "arcs — every eligibility answer was the default, so the gate is "
                      "INERT despite looking live (invariant #4).");
        else if (h.reattaches == 0 && h.reattach_blocked_cold == 0)
            log->info("Network (eq34 hygiene): no reattach occurred this run — the "
                      "guard had zero opportunity, which is NOT evidence that it works "
                      "(grade it on a run that contains a handover).");
        if (ctx.knobs.pin_bridge_on && h.bridges_opened == 0)
            log->warn("Network (eq34 hygiene INC-2): ARMED but ZERO bridges opened — "
                      "either no handover needed one, or no solved level was ever "
                      "harvested (pb_level_prev empty).  With {} blocked-cold "
                      "reattach(es) recorded the second is the likely cause.",
                      h.reattach_blocked_cold);
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
