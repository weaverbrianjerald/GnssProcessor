/**
 * @file NetworkAmbFloat.cpp
 * @brief RF-1 step 4 — the floored birth prior and the bounded drop.
 *        @see include/filter/NetworkAmbFloat.hpp
 */
#include "filter/NetworkAmbFloat.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/StateKeySchema.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

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
    // value_abs, var_first, var_birth, birth_s, birth_ep, last_seen_s and n_folds are
    // KEPT by construction (the manager's decision of 2026-09-14, question 2).
    return true;
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

    long long n_free = 0, n_stale = 0, n_idem = 0;
    // (1) THE LATCHED FREE-EPOCH SET.  The set itself is NOT consumed — §4.1: the lane
    //     observatory, the phase-datum candidacy screen, the anchor-slip handover staging
    //     and the harvest's own statistics guards all read it, and re-pointing them is a
    //     separate change.  What changes is only the RESPONSE the set invokes.
    for (const auto& [sta, amb_key] : relevel.free_ambs) {
        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
        if (!parts) continue;
        const auto it = local_carry.find(
            std::make_tuple(sta, parts->satellite, parts->signal));
        if (it == local_carry.end()) continue;      // nothing carried ⇒ nothing to drop
        const double fl = ambFloatBirthFloor(floor_base,
                                             elevOf(sta, parts->satellite), elev_power);
        if (ambFloatDrop(it->second, fl, pl_now_s, now_ep)) ++n_free;
        else                                                ++n_idem;
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
        const double fl = ambFloatBirthFloor(floor_base,
                                             elevOf(std::get<0>(it->first), std::get<1>(it->first)),
                                             elev_power);
        if (ambFloatDrop(it->second, fl, pl_now_s, now_ep)) ++n_stale;
        else                                               ++n_idem;
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
    if (log && n > 0)
        log->debug("Network (RF-1 step 4): TOW {:.1f} — dropped {} carried prior(s) back to "
                   "their birth variance ({} free-epoch latch, {} stale gap; {} already at "
                   "birth strength).  value_abs kept; the prior still folds this epoch.",
                   tow, n, n_free, n_stale, n_idem);
}

}  // namespace GnssProcessor
