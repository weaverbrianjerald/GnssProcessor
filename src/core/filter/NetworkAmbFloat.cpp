/**
 * @file NetworkAmbFloat.cpp
 * @brief RF-1 step 4 — the floored birth prior and the bounded drop.
 *        @see include/filter/NetworkAmbFloat.hpp
 */
#include "filter/NetworkAmbFloat.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/NetworkCoupling.hpp"          // rebaseCouplingPhaseBiasSprev (the D4 s_prev walk)
#include "filter/NetworkHandoverStransform.hpp" // kStsExactTolM (one exactness tolerance)
#include "filter/StateKeySchema.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace GnssProcessor {

double ambFloatBirthFloor(double base_m2, double elev_deg, double elev_power) {
    if (!(base_m2 > 0.0)) return 0.0;
    if (!std::isfinite(elev_deg) || elev_deg <= 0.0) return base_m2;   // unavailable ⇒ zenith
    // sigma^2(e) = sigma^2_zen / sin^{2p}(e), exactly the code row weight's mapping.  The
    // sine is clamped away from 0 so a grazing / below-horizon elevation cannot produce an
    // infinite floor; 5 degrees is well below every mask this producer runs.
    const double s = std::max(std::sin(elev_deg * 3.14159265358979323846 / 180.0),
                              std::sin(5.0 * 3.14159265358979323846 / 180.0));
    const double scaled = base_m2 / std::pow(s, 2.0 * elev_power);
    return std::max(base_m2, std::isfinite(scaled) ? scaled : base_m2);
}

bool ambFloatDrop(LocalCarry& lc, double floor_m2, double now_s, long long now_ep) {
    // The restored prior: what the arc was worth at birth, never tighter than the
    // posture's floor.  max() over all three so a drop can only LOOSEN — an arc whose
    // var_birth was recorded before the floor was armed (or on a warm-resume-restored
    // entry, where var_birth is 0 = unknown) still gets the floor.
    const double target = std::max({lc.var_birth, lc.var_first, floor_m2});
    if (!(target > 0.0)) return false;          // nothing usable to restore
    if (lc.var >= target) {
        // Already at or below birth strength — an idempotent re-drop inside one epoch
        // (two detectors on the same arc), or an arc that never converged.  The age
        // clock still restarts: the LEVEL is still doubted even if the variance is not.
        lc.relevel_s  = now_s;
        lc.dropped_ep = now_ep;
        // RF-3a A2: the WALK metric's population is "every drop that doubted the level",
        // which includes this one — the level is re-determined either way, and an OFF run
        // has to report the walk it suffers whether or not the variance moved.
        lc.drop_walk_ph = 1;
        return false;
    }
    lc.var = target;
    // The border regression and the block cross terms belong to the OLD value: a
    // regression harvested against a level the response has just doubted would keep
    // injecting that level's relationship to the shared border at full strength.
    // Clearing blk also makes every pair with a sibling read as a pair-miss this epoch,
    // which is counted — the decoupling edge, visible (invariant #7).
    lc.coupling.clear();
    lc.blk.reset();
    lc.blk_idx = -1;
    lc.relevel_s  = now_s;
    lc.dropped_ep = now_ep;
    // RF-3a A2: arm the WALK metric.  The next harvest of this arc reads the phase,
    // records |value_abs(after) - value_abs(before)| and advances it; the fold after
    // that records the residual walk.  Measure-only.
    lc.drop_walk_ph = 1;
    // value_abs, var_first, var_birth, birth_s, birth_ep, last_seen_s and n_folds are
    // KEPT by construction (the manager's decision of 2026-09-14, question 2).
    return true;
}

double ambFloatDropFloorNow(double drop_floor_m2, long long now_ep, int after_ep) {
    if (!(drop_floor_m2 > 0.0)) return 0.0;      // nothing to gate
    if (after_ep <= 0)          return drop_floor_m2;   // no gate: RF-3a's behaviour
    return (now_ep >= static_cast<long long>(after_ep)) ? drop_floor_m2 : 0.0;
}

AmbDropOutcome ambFloatDropFloored(LocalCarry& lc, double birth_floor_m2,
                                   double drop_floor_m2, double now_s,
                                   long long now_ep) {
    AmbDropOutcome out;
    // What the arc would have been restored to WITHOUT the drop-only floor.  Read
    // before the call, because the call overwrites lc.var.
    const double without_drop = std::max({lc.var_birth, lc.var_first, birth_floor_m2});
    out.from_drop_floor = (drop_floor_m2 > 0.0) && (drop_floor_m2 > without_drop);
    out.acted = ambFloatDrop(lc, std::max(birth_floor_m2, drop_floor_m2), now_s, now_ep);
    // An arc already looser than every floor is not evidence that the drop floor did
    // anything: report the floor as the SOURCE only when the variance actually moved
    // to it.  (Without this an idempotent re-drop would inflate the counter.)
    if (!out.acted) out.from_drop_floor = false;
    return out;
}

void networkAmbFloatApplyDrops(
    NetworkRunContext& ctx, const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    const std::vector<NetStation>& stations,
    const std::vector<std::map<std::string, double>>& elev_of,
    long long week, double tow, double pl_now_s) {
    // --- alias preamble (the seam contract) -------------------------------------
    if (!ctx.knobs.amb_float_kf) return;            // the vehicle owns this response
    auto& local_carry = ctx.carry.local_carry;
    auto& relevel     = ctx.carry.relevel;
    auto& counters    = ctx.counters;
    const double floor_base = ctx.knobs.amb_birth_var_floor;
    // RF-3a A1: the DROP-only floor, elevation-mapped with exactly the same mapping the
    // birth floor uses (@ref ambFloatBirthFloor is the mapping, not the knob).
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
    // RF-3c: the drop floor IN FORCE this epoch — 0 while the warm-up gate is closed,
    // which is today's RF-3a-free quantity.  Resolved once per pass because the gate's
    // abscissa is the epoch, not the line of sight.
    const double drop_base = ambFloatDropFloorNow(drop_base_knob, now_ep,
                                                  ctx.knobs.amb_drop_floor_after_ep);
    const bool   pre_gate  = (drop_base_knob > 0.0) && !(drop_base > 0.0);
    // RF-3a A1: one lambda so the two populations below cannot take the drop at
    // different floors.  Both floors are mapped at THIS line of sight's elevation.
    const auto dropAt = [&](LocalCarry& lc, int sta, const std::string& sat) {
        const double e = elevOf(sta, sat);
        const AmbDropOutcome o = ambFloatDropFloored(
            lc, ambFloatBirthFloor(floor_base, e, elev_power),
            ambFloatBirthFloor(drop_base,  e, elev_power), pl_now_s, now_ep);
        if (o.from_drop_floor) ++n_dropfloor;
        if (pre_gate)          ++n_pregate;
        return o.acted;
    };
    // (1) THE LATCHED FREE-EPOCH SET.  The set itself is NOT consumed — §4.1: the lane
    //     observatory, the phase-datum candidacy screen, the anchor-slip handover staging
    //     and the harvest's own statistics guards all read it, and re-pointing them is a
    //     separate change.  What changes is only the RESPONSE the set invokes.
    for (const auto& [sta, amb_key] : relevel.free_ambs) {
        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
        if (!parts) continue;
        // RF-2 items 3 + 4: every screen flag stamps the arc, whether or not it has a
        // carry to drop.  The election's quiet window reads it to refuse a
        // recently-flagged candidate the S-basis, and the sticky branch reads it to
        // DEPOSE a flagged basis holder — which is why the stamp must land even when
        // there is no carry here (a flagged arc with no carry is still not a datum).
        if (ctx.knobs.pd_held_basis)
            ctx.carry.amb_flag_ep[{sta, amb_key}] = now_ep;
        const auto it = local_carry.find(
            std::make_tuple(sta, parts->satellite, parts->signal));
        if (it == local_carry.end()) continue;      // nothing carried ⇒ nothing to drop
        if (dropAt(it->second, sta, parts->satellite)) ++n_free;
        else                                           ++n_idem;
        note(it->first, it->second, AmbDropCause::kReLevelFree);
    }
    // (2) THE GAP CLIFF.  An arc PRESENT this epoch whose sighting gap already exceeds the
    //     horizon: on the leveling vehicle the inject simply skipped it and the harvest
    //     re-stashed a level drawn from one epoch of code (SPEC_withhold_era_release §8 —
    //     "the cliff").  The post-harvest prune cannot see these, because the harvest
    //     refreshes last_seen_s first; the prune's own population (arcs genuinely gone)
    //     stays an ERASE, which is §3.4's one surviving erase case.
    for (auto it = local_carry.begin(); it != local_carry.end(); ++it) {
        const double gap = pl_now_s - it->second.last_seen_s;
        if (!(gap > max_gap_s)) continue;
        if (dropAt(it->second, std::get<0>(it->first), std::get<1>(it->first))) ++n_stale;
        else                                                                    ++n_idem;
        note(it->first, it->second, AmbDropCause::kStaleGap);
    }

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

long long networkAmbFloatDeclaredHandover(
    NetworkRunContext& ctx,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    const std::vector<NetStation>& stations,
    const std::vector<AnchorHandoverEvent>& handovers,
    long long week, double tow,
    std::set<std::pair<std::string, std::string>>& handled_out) {
    // --- alias preamble (the seam contract) -------------------------------------
    if (!ctx.knobs.amb_float_kf || !ctx.knobs.amb_float_declared_handover) return 0;
    if (handovers.empty()) return 0;
    auto& local_carry = ctx.carry.local_carry;
    auto& prev_carry  = ctx.carry.prev_carry;
    auto& counters    = ctx.counters;

    long long n_shift_total = 0;
    for (const AnchorHandoverEvent& h : handovers) {
        const auto parts = StateKeys::parseAmbiguityKey(h.amb);
        if (!parts) continue;
        const std::string& sat = parts->satellite;
        const std::string& sig = parts->signal;
        // SCOPE: only a CONTINUOUS handover has a Δ.  A cold re-realization elected a
        // successor with no usable carried level, so there is no new basis to express
        // the family in — its erase stands, and it is counted as the refused population
        // rather than silently transformed with Δ = 0 (which would leave every carry in
        // the RETIRED frame, the worst of both).
        if (!h.continuous) { ++counters.n_amb_sts_refused_cold; continue; }
        int to_idx = -1;
        for (std::size_t s = 0; s < stations.size(); ++s)
            if (stations[s].name == h.to) { to_idx = static_cast<int>(s); break; }
        if (to_idx < 0) { ++counters.n_amb_sts_refused_cold; continue; }
        const auto in_it = local_carry.find(std::make_tuple(to_idx, sat, sig));
        if (in_it == local_carry.end() || !std::isfinite(in_it->second.value_abs)) {
            // The ledger said continuous but the incoming anchor holds no live carry
            // THIS epoch — the same "no Δ" case, named separately because it is a
            // ledger/carry disagreement rather than a cold election.
            ++counters.n_amb_sts_refused_no_level;
            continue;
        }
        const double delta = in_it->second.value_abs;
        if (!(std::abs(delta) > 0.0)) {
            // Already in the new basis (Δ = 0) — the transform is the identity, so take
            // ownership of the family (no erase) and move on.  This is the common case
            // when the same station re-pins itself.
            handled_out.insert({sat, sig});
            ++counters.n_amb_sts_identity;
            continue;
        }
        // ---- THE TRANSFORM.  All three references on this identity move TOGETHER.
        long long n_shift = 0;
        for (auto& [key, lc] : local_carry) {
            if (std::get<1>(key) != sat || std::get<2>(key) != sig) continue;
            lc.value_abs -= delta;          // variances UNTOUCHED — an exact basis change
            ++n_shift;
        }
        // The satellite PHASE BIAS absorbs the opposite shift: pinning N_{r_in} = 0 means
        // b'_s = b_s + Δ.  Its carried prior and every coupling s_prev that references it
        // must move with it, or the frame step is injected as row bias
        // (NetworkChainDatum.cpp:852-866, the filed latent defect).
        const std::map<std::pair<std::string, std::string>, double> shift_sat{
            {{sat, sig}, +delta}};
        long long n_pb = 0, n_cpl = 0;
        for (auto& cp : prev_carry) {
            if (cp.kind != SharedParamKind::PhaseBias) continue;
            if (cp.sat != sat || cp.signal != sig) continue;
            cp.value += delta;
            ++n_pb;
        }
        for (auto& kv : local_carry)
            n_cpl += rebaseCouplingPhaseBiasSprev(kv.second.coupling, shift_sat);
        // ---- THE EXACTNESS SELF-CHECK (the sts_selfcheck_ok pattern).  The incoming
        //      anchor's column must now read 0, which is what its w = 1e10 pin asserts.
        const double x_post = in_it->second.value_abs;
        const bool ok = std::abs(x_post) <= kStsExactTolM;
        if (!ok) ++counters.n_amb_sts_selfcheck_fail;
        n_shift_total += n_shift;
        ++counters.n_amb_stransform;
        counters.n_amb_stransform_cols += n_shift;
        handled_out.insert({sat, sig});
        // ---- BOTH EDGES, with identities (invariant #7).  A transform logged only as a
        //      total is exactly the silent re-referencing the FT63 topology ledger exists
        //      to recover.
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

}  // namespace GnssProcessor
