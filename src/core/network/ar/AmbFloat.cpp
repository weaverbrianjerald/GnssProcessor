/**
 * @file AmbFloat.cpp
 * @brief Float ambiguity vehicle: the floored birth prior, the bounded drop, and the
 *        declared anchor S-transform (an exact change of reference basis when the
 *        phase-datum anchor station of a satellite signal changes).
 * @see include/network/ar/AmbFloat.hpp
 */
#include "network/ar/AmbFloat.hpp"
#include "network/driver/Epoch.hpp"          // NetworkEpoch
#include "network/driver/Run.hpp"            // NetworkRun
#include "network/driver/SetupResult.hpp"    // run.setup.stations
#include "network/assembly/Responses.hpp"   // networkResponseLatchTally
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/diag/AmbStateTrace.hpp"
#include "network/carry/Coupling.hpp"          // phaseBiasShiftOf (the phase-bias shift rule)
#include "network/solve/HandoverStransform.hpp" // kStsExactTolM (one exactness tolerance)
#include "network/state/Entities.hpp"          // net::arcKey, net::satSignalKey, net::satKey
#include "network/diag/RunTraces.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <cstddef>
#include <memory>
#include <vector>

namespace GnssProcessor {

double ambFloatBirthFloor(double base_m2, double elev_deg, double elev_power) {
    if (!(base_m2 > 0.0)) return 0.0;
    if (!std::isfinite(elev_deg) || elev_deg <= 0.0) return base_m2;   // elevation unavailable: use the zenith value
    // sigma^2(e) = sigma^2_zen / sin^{2p}(e), the mapping of the code row weights.
    // The sine is clamped at sin(5 deg) so a grazing elevation cannot give an unbounded
    // floor. 5 degrees is well below every elevation mask this producer runs.
    const double s = std::max(std::sin(elev_deg * 3.14159265358979323846 / 180.0),
                              std::sin(5.0 * 3.14159265358979323846 / 180.0));
    const double scaled = base_m2 / std::pow(s, 2.0 * elev_power);
    return std::max(base_m2, std::isfinite(scaled) ? scaled : base_m2);
}

bool ambFloatDrop(LocalCarry& lc, double floor_m2, double now_s, long long now_ep,
                  bool decouple_always, bool* decoupled_idem) {
    if (decoupled_idem) *decoupled_idem = false;
    // Variance to restore: the arc's birth variance, never tighter than the floor.
    // Taking the max of all three means a drop can only loosen the prior. An arc whose
    // var_birth was recorded before the floor was armed, or a warm-resume entry
    // (var_birth = 0 means unknown), still gets the floor.
    const double target = std::max({lc.var_birth, lc.var_first, floor_m2});
    if (!(target > 0.0)) return false;          // nothing usable to restore
    if (lc.var >= target) {
        // Already at or below birth strength: a repeated drop within one epoch (two
        // detectors on the same arc), or an arc that never converged. The age clock
        // still restarts, because the level is doubted even if the variance is not.
        lc.relevel_s  = now_s;
        lc.dropped_ep = now_ep;
        // Walk metric: how far the level moves in the first folds after a drop
        // (measure-only). It counts every drop that doubted the level, including this
        // one. The level is re-determined either way, and a run with the drop floor off
        // still reports the walk it suffers.
        lc.drop_walk_ph = 1;
        // The regression and the block row belong to the doubted value whether or not
        // the variance moved. An arc that slips one epoch after its birth is already at
        // birth strength. Without this it keeps folding its pre-slip value with full
        // cross terms and a full border regression. Under a consistent time update the
        // station's ionosphere-free ties are then phase-tight, and the stale value is
        // pushed into the satellite clocks (seen as a jump at the first fold, field
        // station BSHM00ISR, G07 L2W). Resetting blk leaves the siblings the principal
        // submatrix of their harvest, which is their exact marginal. Opt-in
        // (network.amb_float_drop_decouple_always); false keeps the previous behaviour
        // byte for byte.
        if (decouple_always && (!lc.coupling.empty() || lc.blk)) {
            lc.coupling.clear();
            lc.blk.reset();
            lc.blk_idx = -1;
            if (decoupled_idem) *decoupled_idem = true;
        }
        return false;
    }
    lc.var = target;
    // The border regression and the block cross terms belong to the old value. A
    // regression harvested against a level the response has just doubted would keep
    // injecting that level's relationship to the shared border at full strength.
    // Clearing blk also means the arc's pairs with its siblings (the other arcs of the
    // joint block) find no cross term this epoch. That is counted as a pair miss, so the
    // decoupling is visible.
    lc.coupling.clear();
    lc.blk.reset();
    lc.blk_idx = -1;
    lc.relevel_s  = now_s;
    lc.dropped_ep = now_ep;
    // Arm the walk metric (drop_walk_ph = 1). The next harvest of this arc records
    // |value_abs(after) - value_abs(before)| and advances the phase. The harvest after
    // that records the residual walk.
    lc.drop_walk_ph = 1;
    // Kept on purpose: value_abs, var_first, var_birth, birth_s, birth_ep, last_seen_s
    // and n_folds. The drop lowers confidence in the level, not the identity of the arc.
    // Keeping value_abs costs little, because a prior at birth variance is dominated by
    // the epoch's data, and birth_s and birth_ep keep the age gates meaningful.
    return true;
}

double ambFloatDropFloorNow(double drop_floor_m2, long long now_ep, int after_ep) {
    if (!(drop_floor_m2 > 0.0)) return 0.0;      // nothing to gate
    if (after_ep <= 0)          return drop_floor_m2;   // no gate: the floor applies from the first epoch
    return (now_ep >= static_cast<long long>(after_ep)) ? drop_floor_m2 : 0.0;
}

AmbDropOutcome ambFloatDropFloored(LocalCarry& lc, double birth_floor_m2,
                                   double drop_floor_m2, double now_s,
                                   long long now_ep, bool decouple_always) {
    AmbDropOutcome out;
    // What the arc would be restored to without the drop-only floor. Computed before
    // the call, because the call overwrites lc.var.
    const double without_drop = std::max({lc.var_birth, lc.var_first, birth_floor_m2});
    out.from_drop_floor = (drop_floor_m2 > 0.0) && (drop_floor_m2 > without_drop);
    out.acted = ambFloatDrop(lc, std::max(birth_floor_m2, drop_floor_m2), now_s, now_ep,
                             decouple_always, &out.decoupled_idem);
    // An arc already looser than every floor is no evidence that the drop floor acted.
    // Report the floor as the source only when the variance actually moved to it, so an
    // idempotent re-drop does not inflate the counter.
    if (!out.acted) out.from_drop_floor = false;
    return out;
}

void networkAmbFloatApplyDrops(
    NetworkRunContext& ctx, const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    const std::vector<NetStation>& stations,
    const std::vector<std::map<std::string, double>>& elev_of,
    long long week, double tow, double pl_now_s, EpochEvents& events) {
    net::NetworkEntities& entities = ctx.entities;   // written: stations
    if (!ctx.knobs.amb_float_kf) return;            // the drop is the float vehicle's response
    auto& relevel     = events.slip.relevel;
    auto& counters    = ctx.counters;
    const double floor_base = ctx.knobs.amb_birth_var_floor;
    // The drop-only floor (network.amb_float_drop_var_m2). It is elevation-mapped like
    // the birth floor: ambFloatBirthFloor supplies the mapping, not the knob value.
    const double drop_base_knob = ctx.knobs.amb_drop_var_floor;
    const double elev_power = config_.getCodeElevationPower();
    const double max_gap_s  = ctx.knobs.pl_max_gap_s;
    const long long now_ep  = static_cast<long long>(counters.n_loop_epochs);

    const auto elevOf = [&](int sta, const std::string& sat) -> double {
        if (sta < 0 || sta >= static_cast<int>(elev_of.size())) return -1.0;
        const auto it = elev_of[static_cast<std::size_t>(sta)].find(sat);
        return it == elev_of[static_cast<std::size_t>(sta)].end() ? -1.0 : it->second;
    };
    const auto note = [&](const std::tuple<int, std::string, std::string>& key,
                          const LocalCarry& lc, const char* cause) {
        if (!ctx.knobs.trace_amb_state) return;
        const int sta = std::get<0>(key);
        ambStateNoteDrop(ctx, week, tow,
                         (sta >= 0 && sta < static_cast<int>(stations.size()))
                             ? stations[static_cast<std::size_t>(sta)].name : std::string{},
                         std::get<1>(key), std::get<2>(key), lc, cause);
    };

    long long n_free = 0, n_stale = 0, n_idem = 0, n_dropfloor = 0, n_pregate = 0;
    // Drop floor in force this epoch: 0 while the warm-up gate
    // (network.amb_float_drop_floor_after_ep) is closed, so the restored variance
    // excludes the drop-only floor. Resolved once per pass because the gate depends on
    // the epoch, not on the line of sight.
    const double drop_base = ambFloatDropFloorNow(drop_base_knob, now_ep,
                                                  ctx.knobs.amb_drop_floor_after_ep);
    const bool   pre_gate  = (drop_base_knob > 0.0) && !(drop_base > 0.0);
    // Set by dropAt: true when an idempotent drop (variance already at birth strength)
    // still cleared the arc's regression and block row
    // (network.amb_float_drop_decouple_always). That is the second way a drop changes
    // the carried prior. Count-only: the loops below read it next to dropAt's return
    // value, which it does not change.
    bool last_decoupled_idem = false;
    // One lambda, so the two populations below cannot take the drop at different
    // floors. Both floors are mapped at this line of sight's elevation.
    const auto dropAt = [&](LocalCarry& lc, int sta, const std::string& sat) {
        const double e = elevOf(sta, sat);
        const AmbDropOutcome o = ambFloatDropFloored(
            lc, ambFloatBirthFloor(floor_base, e, elev_power),
            ambFloatBirthFloor(drop_base,  e, elev_power), pl_now_s, now_ep,
            ctx.knobs.amb_drop_decouple_always);
        if (o.decoupled_idem) ++counters.n_amb_drop_decoupled_idem;
        last_decoupled_idem = o.decoupled_idem;
        if (o.from_drop_floor) ++n_dropfloor;
        if (pre_gate)          ++n_pregate;
        return o.acted;
    };
    // Cause bits of the two re-entry responses (gap re-entry and wind-up restart). A
    // latch counts as a re-entry only when its recorded cause is non-zero and has no
    // other bit. A slip detector's bit, a RAIM (receiver autonomous integrity
    // monitoring) attribution bit, an unknown bit, or no recorded cause counts as the
    // slip row. A slip injected for testing is tagged with the RAIM attribution bit
    // (kCauseT4Attrib), so it also counts as a slip.
    constexpr unsigned kReentryBits =
        RelevelEpochState::kCauseGap | RelevelEpochState::kCauseWindup;
    // (1) Free-epoch latch: the arcs the slip detectors flagged this epoch. The set is
    //     left in place because the phase-datum candidacy screen, the anchor-slip
    //     handover staging and the harvest's statistics guards also read it. Only the
    //     response to a flag changes here: a bounded drop instead of skipping the inject.
    for (const auto& [sta, amb_key] : relevel.free_ambs) {
        const std::optional<ArcId> arc = net::arcKey(sta, amb_key);
        if (!arc) continue;
        // With network.phase_datum_held_basis (the held S-basis, the reference basis of
        // the phase datum), every flag stamps the arc with this epoch, whether or not it
        // has a carry to drop. The datum election reads the stamp: its quiet window
        // refuses the S-basis to a recently flagged candidate, and its sticky branch
        // deposes a flagged basis holder. The stamp is written even without a carry,
        // because a flagged arc with no carry is still not a usable datum.
        if (ctx.knobs.pd_held_basis)
            net::putAmbFlagEp(entities, sta, amb_key) = now_ep;
        const auto it = net::get(ctx.entities.arcs, *arc, &net::ArcState::amb);
        if (it == nullptr) continue;      // nothing carried, nothing to drop
        const bool acted = dropAt((*it), sta, net::satKey(arc->sat));
        if (acted) ++n_free;
        else       ++n_idem;
        // Tally a drop that changed the carried prior (the variance moved, or the
        // idempotent drop decoupled the arc), split by what latched the arc
        // (kReentryBits). The two rows sum to n_free plus this loop's idempotent
        // decouples.
        if (acted || last_decoupled_idem) {
            const auto cz = relevel.free_cause.find({sta, amb_key});
            const unsigned cbits = (cz == relevel.free_cause.end()) ? 0u : cz->second;
            const bool reentry = cbits != 0u && (cbits & ~kReentryBits) == 0u;
            prmRespond(counters.prm_resp, reentry ? ResponseSite::AmbDropLatchReentry
                                                  : ResponseSite::AmbDropLatchSlip);
        }
        note(net::arcTuple(*arc), (*it), AmbDropCause::kReLevelFree);
    }
    // (2) Gap cliff: an arc whose sighting gap already exceeds the horizon
    //     (phase_leveling_max_gap_s). On the leveling vehicle the inject skips such an
    //     arc and the harvest re-stashes a level drawn from one epoch of code. The
    //     post-harvest prune cannot see these arcs, because the harvest refreshes
    //     last_seen_s first. The prune keeps its own population (arcs that are gone),
    //     which is still erased. This loop does not test presence, so an absent arc past
    //     the horizon is also dropped and counted here before the prune erases it.
    net::forEach(ctx.entities.arcs, &net::ArcState::amb, [&](const ArcId& id, LocalCarry& lc) {
        const double gap = pl_now_s - lc.last_seen_s;
        if (!(gap > max_gap_s)) return;
        const bool acted = dropAt(lc, id.station.index, net::satKey(id.sat));
        if (acted) ++n_stale;
        else       ++n_idem;
        // Tally a drop that changed the carried prior (as in loop (1)).
        if (acted || last_decoupled_idem)
            prmRespond(counters.prm_resp, ResponseSite::AmbDropStaleGap);
        note(net::arcTuple(id), lc, AmbDropCause::kStaleGap);
    });

    const long long n = n_free + n_stale;
    if (n > 0) {
        counters.n_amb_drop += n;
        ++counters.n_amb_drop_epochs;
    }
    counters.n_amb_drop_relevel_free += n_free;
    counters.n_amb_drop_stale_gap    += n_stale;
    counters.n_amb_drop_idempotent   += n_idem;
    counters.n_amb_drop_floored      += n_dropfloor;
    counters.n_amb_drop_pre_gate     += n_pregate;
    if (log && n > 0)
        log->debug("Network (RF-1 step 4): TOW {:.1f} — dropped {} carried prior(s) back to "
                   "their birth variance ({} free-epoch latch, {} stale gap; {} already at "
                   "birth strength).  value_abs kept; the prior still folds this epoch.",
                   tow, n, n_free, n_stale, n_idem);
}

void declaredHandoverShiftCarry(NetworkCarryState& carry, net::NetworkEntities& entities,
                                    const std::string& sat,
                                const std::string& sig, double delta,
                                long long& n_shift, long long& n_pb, long long& n_cpl,
                                CarrySiteCounts* counts, bool complete) {
    n_shift = n_pb = n_cpl = 0;
    // Phase-bias shift of this (satellite, signal) family, from the shared rule
    // phaseBiasShiftOf (the rule eq34ShiftCarry in ChainDatum.cpp also applies).
    const std::map<std::pair<std::string, std::string>, double> shift_sat{{{sat, sig}, +delta}};
    // With network.carry_rebase_complete, the remaining carried references join the
    // shift: the gross line-of-sight screen's held ambiguity (shifted like the ambiguity), the
    // last solved phase bias and an open pin bridge (shifted like the carried phase
    // bias), and the terms of every other coupling map on the family.
    forEachCarriedReference(
        carry, entities, carryMechanismSiteMask(CarryMechanism::DeclaredHandover, complete),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::PhaseBias || r.sat != sat || r.signal != sig)
                return false;
            switch (r.site) {
                case CarrySite::AmbiguityValue:
                    *r.value -= delta;             // variances untouched: an exact basis change
                    ++n_shift;
                    return true;
                case CarrySite::HoldAmbiguityValue:   // network.carry_rebase_complete
                    *r.value -= delta;             // the held ambiguity, as the ambiguity
                    return true;
                case CarrySite::PrevCarry:
                    *r.value += delta;             // pinning N_{r_in} = 0 (r_in: incoming anchor) means b'_s = b_s + Δ
                    ++n_pb;
                    return true;
                case CarrySite::PbLevelPrev:          // network.carry_rebase_complete
                case CarrySite::PinBridge:
                    *r.value += delta;             // the last solved phase bias and the bridge, as the phase bias
                    return true;
                default: {                         // a coupling term on the family's phase bias
                    const double s = phaseBiasShiftOf(shift_sat, r.sat, r.signal);
                    if (s == 0.0) return false;
                    *r.value += s;
                    ++n_cpl;
                    return true;
                }
            }
        },
        counts);
}

long long networkAmbFloatDeclaredHandover(
    NetworkRunContext& ctx,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    const std::vector<NetStation>& stations,
    const std::vector<AnchorHandoverEvent>& handovers,
    long long week, double tow,
    std::set<std::pair<std::string, std::string>>& handled_out) {
    net::NetworkEntities& entities = ctx.entities;   // written
    if (!ctx.knobs.amb_float_kf || !ctx.knobs.amb_float_declared_handover) return 0;
    if (handovers.empty()) return 0;
    auto& counters    = ctx.counters;

    long long n_shift_total = 0;
    for (const AnchorHandoverEvent& h : handovers) {
        const std::optional<SatSignalId> family = net::satSignalKey(h.amb);
        if (!family) continue;
        const std::string sat = net::satKey(family->sat);
        const std::string sig = net::signalKey(family->signal);
        // Only a continuous handover (the successor was elected with a carried level)
        // has a Δ. A cold re-realization has no usable carried level, so there is no new
        // basis to express the family in. Its erase stands, and it is counted as refused
        // rather than transformed with Δ = 0, which would leave every carry in the
        // retired frame without an erase. An incoming station missing from the roster is
        // counted the same way.
        if (!h.continuous) { ++counters.n_amb_sts_refused_cold; continue; }
        int to_idx = -1;
        for (std::size_t s = 0; s < stations.size(); ++s)
            if (stations[s].name == h.to) { to_idx = static_cast<int>(s); break; }
        if (to_idx < 0) { ++counters.n_amb_sts_refused_cold; continue; }
        const auto in_it = net::getArc(ctx.entities.arcs, std::make_tuple(to_idx, sat, sig), &net::ArcState::amb);
        if (in_it == nullptr || !std::isfinite(in_it->value_abs)) {
            // The handover record says continuous, but the incoming anchor holds no live
            // carry this epoch. This is the same "no Δ" case, counted separately because
            // the record and the carry disagree, which is not a cold election.
            ++counters.n_amb_sts_refused_no_level;
            continue;
        }
        const double delta = in_it->value_abs;
        if (!(std::abs(delta) > 0.0)) {
            // Already in the new basis (Δ = 0): the transform is the identity. Mark the
            // family handled (the caller does not erase it) and continue. This is the
            // common case when the same station re-pins itself.
            handled_out.insert({sat, sig});
            ++counters.n_amb_sts_identity;
            continue;
        }
        // ---- The transform.  Three references on this (satellite, signal) family move
        //      together: every carried ambiguity of the family by -Δ, and the satellite
        //      phase bias by the opposite shift (pinning N_{r_in} = 0 means
        //      b'_s = b_s + Δ), along with its carried prior and every coupling s_prev
        //      that references it. Moving only some of them injects the frame step into
        //      the rows as bias. eq34ShiftCarry in ChainDatum.cpp moves the ambiguity and
        //      the coupling terms together for the same reason.
        CarrySiteCounts prm_cc;
        long long n_shift = 0, n_pb = 0, n_cpl = 0;
        declaredHandoverShiftCarry(ctx.carry, entities, sat, sig, delta, n_shift, n_pb, n_cpl, &prm_cc,
                                   ctx.knobs.rebase_complete);
        ++counters.prm_ref_calls[static_cast<int>(CarryMechanism::DeclaredHandover)];
        carryTallyMoved(counters.prm_ref_moved, CarryMechanism::DeclaredHandover, prm_cc);
        // Count a held emission of the family that stays in the old frame (the phase-bias
        // hold is exempt from the shift). Measure-only.
        if (delta != 0.0
            && net::getSig(ctx.entities.sat_signals, net::holdKey(*family), &net::SatSignalState::pb_hold))
            ++counters.prm_pbhold_unmoved_at_shift;
        // ---- Exactness self-check (the same test networkHandoverStransform makes with
        //      selfcheck_ok).
        //      The incoming anchor's column now reads 0, which is what its w = 1e10 pin
        //      asserts.
        const double x_post = in_it->value_abs;
        const bool ok = std::abs(x_post) <= kStsExactTolM;
        if (!ok) ++counters.n_amb_sts_selfcheck_fail;
        n_shift_total += n_shift;
        ++counters.n_amb_stransform;
        counters.n_amb_stransform_cols += n_shift;
        handled_out.insert({sat, sig});
        // ---- One event row per family, with identities. A transform reported only as a
        //      total would be a silent re-referencing that the topology trace
        //      (network.trace_topology) would have to reconstruct.
        traces.emitEvent(GnssTime{static_cast<int>(week), tow}, "STRANSFORM",
                         ok ? "sts_applied" : "sts_selfcheck_fail",
                         h.amb, h.to, delta, std::nullopt, "s_transform",
                         fmt::format("from={};cols={};pb={};cpl={};x_post={:.3e}",
                                     h.from.empty() ? std::string("(none)") : h.from,
                                     n_shift, n_pb, n_cpl, x_post));
        if (!ok && log)
            log->warn("Network (RF-1 step 5): declared anchor S-transform on {} -> {} "
                      "(Δ = {:.6f} m) left the incoming anchor's carried level at {:.3e} m, "
                      "above the {:.0e} m exactness tolerance.  The carry is now in a frame "
                      "that is NOT the pinned one — read this as a transform defect, not as "
                      "noise.", h.amb, h.to, delta, x_post, kStsExactTolM);
    }
    if (log && n_shift_total > 0)
        log->debug("Network (RF-1 step 5): TOW {:.1f} — {} declared anchor S-transform(s) "
                   "shifted {} carried level(s); variances untouched.",
                   tow, counters.n_amb_stransform, n_shift_total);
    return n_shift_total;
}

// ---- Epoch entry point: the bounded drop for one epoch ----
void networkAmbFloatDropEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events) {
    // ---- From the run and the epoch (reads bind through `ep`; writes are marked `// written`) ----
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkSetupResult& setup = run.setup;
    auto& stations = setup.stations;
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const SyncedEpoch& se = ep.se;
    const std::vector<std::vector<PppObservation>>& modeled = ep.pass_a.modeled;
    const double pl_now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    // Bounded drop, applied serially: after PASS A's slip scans (the per-station scan
    // pass) have latched relevel.free_ambs, and before the parallel PASS B inject (the
    // per-station assembly) reads the carry. By the time a station's produce step sees an
    // arc, the drop has happened, so the inject folds the arc at birth strength instead
    // of skipping it. No-op unless the vehicle is float_kf. `modeled` carries this
    // epoch's line-of-sight elevations, which map the birth floor; the map is built only
    // when the floor is armed.
    std::vector<std::map<std::string, double>> amb_elev_of;
    if (ctx.knobs.amb_birth_floor_on) {
        amb_elev_of.resize(modeled.size());
        for (std::size_t mi = 0; mi < modeled.size(); ++mi)
            for (const auto& mo : modeled[mi])
                amb_elev_of[mi][mo.satellite] = mo.elevation;
    }
    // Tally the latch per cause before the drop reads it.
    networkResponseLatchTally(ctx, events);
    networkAmbFloatApplyDrops(ctx, config_, log, stations, amb_elev_of,
                              se.time.week, se.time.tow, pl_now_s, events);
}

// ---- Run-end reports ----

// Run-end report: the ambiguity vehicle and the joint-block fallback bar. PD means
// positive definite: a joint block that is not PD falls back to per-column rows.
int networkAmbVehicleRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    int amb_block_rc = 0;   // verdict of the joint-block fallback bar, this report's return value

    // Ambiguity-vehicle readout. Printed only when the vehicle is non-default or the
    // amb-state trace is armed, so the output of a run with neither is unchanged. The
    // mechanism counters (drops, declared S-transforms and the columns they move,
    // joint-block fallbacks) are printed even at 0: a float_kf run with 0 drops means
    // the wiring is broken, not that the run was quiet.
    if (ctx.knobs.amb_float_kf || config_.getNetworkConfig().trace_amb_state)
        log->info("Network (RF-1 ambiguity vehicle): vehicle=\"{}\"{}, birth_var_floor={} "
                  "m^2, declared_handover={}.  Mechanism counters: {} drop(s) "
                  "over {} epoch(s), {} declared S-transform(s) moving {} column(s), {} "
                  "joint-block fallback(s).",
                  ctx.knobs.amb_float_kf ? "float_kf" : "level",
                  ctx.knobs.pl_blk_from_vehicle
                      ? " — the joint block was STRUCTURAL (step 3)"
                      : " — the zero-q scalar carry",
                  ctx.knobs.amb_birth_var_floor,
                  ctx.knobs.amb_float_declared_handover,
                  ctx.counters.n_amb_drop, ctx.counters.n_amb_drop_epochs,
                  ctx.counters.n_amb_stransform, ctx.counters.n_amb_stransform_cols,
                  ctx.counters.n_amb_block_fallback);
    // Drop inventory by cause, and the evidence that the birth floor acted. A float_kf
    // run with 0 drops means the wiring is broken, not that the run was quiet, so 0 logs
    // at warn level, with the population that would have been dropped named beside it.
    if (ctx.knobs.amb_float_kf && ctx.knobs.amb_birth_var_floor > 0.0) {
        const auto& C = ctx.counters;
        log->log(C.n_amb_drop > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (RF-1 step 4 bounded drop): {} drop(s) over {} epoch(s) — {} "
                 "free-epoch latch, {} stale gap, {} T4 attribution, {} T3 consensus; {} "
                 "call(s) found the arc already at or below birth strength.  Birth floor "
                 "{} m^2 at zenith, elevation-mapped at the code weights' power: {} birth "
                 "stash(es) were RAISED by it.  The ERASE survives for the gap prune "
                 "({} entr(ies)) and re-admission — 'this is a different arc'.  0 drops "
                 "under an armed vehicle is a MISWIRE, not a quiet run: compare against "
                 "the {} free-epoch latch(es) and {} inject-site stale skip(s) the "
                 "detectors and the cliff actually raised.",
                 C.n_amb_drop, C.n_amb_drop_epochs, C.n_amb_drop_relevel_free,
                 C.n_amb_drop_stale_gap, C.n_amb_drop_t4, C.n_amb_drop_t3,
                 C.n_amb_drop_idempotent, ctx.knobs.amb_birth_var_floor,
                 C.n_amb_birth_floored, C.n_cr_gap_prune,
                 C.n_relevel_free_skips, C.n_cr_inject_stale);
    }
    // Drops that found the arc at birth strength and still decoupled it.
    if (ctx.knobs.amb_drop_decouple_always)
        log->info("Network (drop decouple): {} drop(s) found the arc already at "
                  "birth strength and still cleared its border regression and block row "
                  "(network.amb_float_drop_decouple_always).",
                  ctx.counters.n_amb_drop_decoupled_idem);
    // Inventory of the drop-only floor and the walk it targets. Printed with the floor
    // on or off: a run with the floor off reports the walk it has, which gives the armed
    // run's number its meaning. The two fold buckets separate the correction absorbed by
    // the first fold after a drop from the walk that follows it. A stiff restored prior
    // lets a 40 m level error through at a few metres per epoch for tens of epochs,
    // while a floored one takes it in at once.
    if (ctx.knobs.amb_float_kf) {
        const auto& C = ctx.counters;
        auto stat = [](std::vector<double> v) {
            struct { double max; double med; std::size_t n; } s{0.0, 0.0, v.size()};
            if (v.empty()) return s;
            s.max = *std::max_element(v.begin(), v.end());
            const std::size_t h = v.size() / 2;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(h), v.end());
            s.med = v[h];
            return s;
        };
        const auto w1 = stat(C.amb_walk_fold1_m);
        const auto w2 = stat(C.amb_walk_fold2_m);
        // The two populations of the drop-floor warm-up gate, printed with the gate on or
        // off so that a run with no gate says so.
        log->info("Network (RF-3c drop-floor warm-up gate): "
                  "network.amb_float_drop_floor_after_ep = {} ({}).  {} drop(s) were taken "
                  "BEFORE the gate opened and restored max(var_birth, var_first, birth "
                  "floor) — the convergence population the floor is deliberately NOT a "
                  "statement about.  {} drop(s) took the floor.",
                  ctx.knobs.amb_drop_floor_after_ep,
                  ctx.knobs.amb_drop_floor_after_ep > 0
                      ? "ARMED — processed epochs, the gross-LOS warmup's abscissa"
                      : "OFF (no gate: the floor applies from epoch 1)",
                  C.n_amb_drop_pre_gate, C.n_amb_drop_floored);
        log->info("Network (RF-3a drop-only floor): network.amb_float_drop_var_m2 = {} m^2 "
                  "at zenith ({}), elevation-mapped at the code weights' power beside the "
                  "birth floor {} m^2.  {} of {} drop(s) had their restored variance come "
                  "FROM the drop floor.  WALK (measure-only, both knob states): the FIRST "
                  "fold after a drop absorbed max {:.3f} m / median {:.3f} m over {} "
                  "measured drop(s); the SECOND fold absorbed max {:.3f} m / median "
                  "{:.3f} m over {}.  A drop floor of 0 with a large second-fold walk is "
                  "the re-coupling edge this drop-only floor names: the restored prior is too stiff "
                  "for a level the response has just doubted.",
                  ctx.knobs.amb_drop_var_floor,
                  ctx.knobs.amb_drop_var_floor > 0.0 ? "ARMED" : "OFF (today's behaviour)",
                  ctx.knobs.amb_birth_var_floor, C.n_amb_drop_floored, C.n_amb_drop,
                  w1.max, w1.med, w1.n, w2.max, w2.med, w2.n);
    }
    // Inventory of the declared anchor S-transform, with the refused population beside
    // it. The split shows the limit of the transform: a cold handover has no Δ, so its
    // family keeps the erase. How often that happens depends on the datum election
    // policy.
    if (ctx.knobs.amb_float_kf && ctx.knobs.amb_float_declared_handover) {
        const auto& C = ctx.counters;
        log->log(C.n_amb_stransform > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (RF-1 step 5 declared anchor S-transform): {} transform(s) "
                 "shifted {} carried level(s) with variances untouched, {} identity "
                 "(Δ = 0, already in the new basis).  REFUSED: {} cold election(s) (no "
                 "carried level for the successor ⇒ no new basis) and {} ledger/carry "
                 "disagreement(s) — those families kept the network-wide ERASE, which is "
                 "the {} carr(ies) the anchor re-reference still destroyed.  Exactness "
                 "self-check FAILED on {} — a transform defect, never noise.  0 "
                 "transforms under an armed knob means every handover this run was cold: "
                 "read the election, not this line.",
                 C.n_amb_stransform, C.n_amb_stransform_cols, C.n_amb_sts_identity,
                 C.n_amb_sts_refused_cold, C.n_amb_sts_refused_no_level,
                 ctx.counters.pa_reref_erase_total, C.n_amb_sts_selfcheck_fail);
    }
    // Joint-block fallback bar. A fallback rate above 1 % is a run-end failure, not a
    // note. Under float_kf a block that is not symmetric positive definite (SPD) sends
    // those station-epochs back to the per-column route the vehicle replaces. A run that
    // fell back on more than 1 % of its attempted station-epochs therefore did not run
    // the vehicle its manifest claims, and must not be graded as if it had. A failure
    // sets amb_block_rc, which the run-end walk folds into the run's return code.
    if (ctx.knobs.pl_blk_from_vehicle) {
        const long long attempted = ctx.counters.n_pl_blk_blocks
                                  + ctx.counters.n_pl_blk_fallback;
        const double rate = attempted > 0
            ? static_cast<double>(ctx.counters.n_pl_blk_fallback) / static_cast<double>(attempted)
            : 0.0;
        if (attempted == 0) {
            log->error("Network (RF-1 step 3) RUN-END FAILURE: ambiguity_vehicle = float_kf "
                       "but ZERO joint blocks were attempted over the whole run — the fold "
                       "never took the vehicle's route, so this run carries today's "
                       "over-count under a float_kf manifest (standing invariant #4).  Check "
                       "carry_conditional_coupling, the carry warm-up, and whether any "
                       "station ever had an injectable carried column.");
            amb_block_rc = 1;
        } else if (rate > 0.01) {
            log->error("Network (RF-1 step 3) RUN-END FAILURE: the joint local block fell "
                       "back to per-column rows on {} of {} attempted station-epochs "
                       "({:.2f} %) — above the 1 % bar.  A fallback is a VEHICLE CHANGE on "
                       "that station-epoch (design risk #3), so above the bar the arm is a "
                       "MIXTURE of the two vehicles and its delta is not attributable.  Do "
                       "not grade this run.",
                       ctx.counters.n_pl_blk_fallback, attempted, 100.0 * rate);
            amb_block_rc = 1;
        } else {
            log->info("Network (RF-1 step 3): joint local block folded on {} of {} attempted "
                      "station-epochs; {} PD fallback(s) ({:.3f} %, bar 1 %).",
                      ctx.counters.n_pl_blk_blocks, attempted,
                      ctx.counters.n_pl_blk_fallback, 100.0 * rate);
        }
    }
    return amb_block_rc;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
