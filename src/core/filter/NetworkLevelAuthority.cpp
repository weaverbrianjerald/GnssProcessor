/**
 * @file NetworkLevelAuthority.cpp
 * @brief LV-6 census implementation; see the header and
 *        `docs/SPEC_level_authority_internal.md` §3/§5.
 */

#include "filter/NetworkLevelAuthority.hpp"

#include "app/NetworkRunContext.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "filter/NetworkRobustStats.hpp"
#include "slip/NetworkRawObservables.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkLevelAuthorityCensus(
    NetworkRunContext& ctx, const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces,
    const SyncedEpoch& se, const std::vector<NetStation>& stations,
    const std::vector<std::vector<PppObservation>>& modeled,
    const std::vector<bool>& present) {
    (void)log;
    if (!ctx.knobs.lvlauth_on) return;

    // Shell convention: the same shell as the SH tie rows (layer 0, the dominant
    // shell on dual-layer postures).  A different shell height would make the
    // comparison of L_cs with a00 invalid by construction.
    const auto& shc = config_.getGlobalVtecShConfig();
    const double Re = shc.earth_radius_km * 1000.0;
    const double shell_m = shc.layer_height_km * 1000.0;   // layer 0 = the dominant shell
    constexpr double kC = 299792458.0;       // m/s
    constexpr double kIonoK = 40.3e16;       // m·Hz²/TECU

    // Tier-1 inputs: per station, (vertical TECU, weight) over the mature LOS of
    // the current epoch.  The modeled rows are looked up in the CS map; the map
    // itself is not iterated, because it is never pruned and keeps entries for
    // satellites that have left the tracked set.
    std::vector<std::vector<std::pair<double, double>>> sta_rows(stations.size());
    long long immature = 0;
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (i >= present.size() || !present[i]) continue;
        for (const auto& o : modeled[i]) {
            const auto it = ctx.carry.iono_cs.find({static_cast<int>(i), o.satellite});
            if (it == ctx.carry.iono_cs.end()) continue;
            const IonoCsState& cs = it->second;
            if (cs.n <= 0 || !(cs.lam1_m > 0.0) || !std::isfinite(cs.val)) continue;
            if (cs.n < ctx.knobs.lvlauth_min_n) { ++immature; continue; }
            const IppGeometry g =
                computeIppGeometry(stations[i].ecef, o.sat_pos, Re, shell_m);
            if (!(g.mapping > 0.0)) continue;
            // val is slant metres at the pair's primary band: STEC[TECU] =
            // val·f1²/40.3e16 with f1 = c/λ1; VTEC = STEC / M.
            const double f1 = kC / cs.lam1_m;
            const double v_tecu = cs.val * f1 * f1 / kIonoK / g.mapping;
            const double w = cs.var > 1e-12 ? 1.0 / cs.var : 1.0e12;
            if (!std::isfinite(v_tecu)) continue;
            sta_rows[i].emplace_back(v_tecu, w);
            // Publish this row's pierce point and weight so an offline reference
            // field (GIM in the field, truth in sim) can be reduced through the
            // same sampling (LV-6 G2, §4).  Per §5, a planetary-mean grade with
            // unmatched sampling measures the roster's geography, and re-deriving
            // the LOS set offline (all visible LOS instead of mature CS-GF only)
            // would reintroduce that term on a smaller scale.  The row is written
            // before the min_los gate: every row that entered tier 1 is dumped,
            // even when its station later fails to qualify for tier 2, so the
            // reference side applies the same gate itself.
            if (ctx.knobs.lvlauth_los_dump && traces.level_authority_los_trace.is_open())
                traces.level_authority_los_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << stations[i].name << ',' << o.satellite << ','
                    << fmt::format("{:.6f}", g.ipp_lat_rad * 180.0 / Constants::PI) << ','
                    << fmt::format("{:.6f}", g.ipp_lon_rad * 180.0 / Constants::PI) << ','
                    << fmt::format("{:.6f}", g.mapping) << ','
                    << fmt::format("{:.4f}", g.elevation_rad * 180.0 / Constants::PI) << ','
                    << fmt::format("{:.6f}", v_tecu) << ','
                    << fmt::format("{:.6e}", w) << ',' << cs.n << '\n';
        }
    }
    ctx.counters.n_lvlauth_immature += immature;

    const auto red = levelAuthorityReduce(sta_rows, ctx.knobs.lvlauth_min_los);
    auto& out = traces.level_authority_trace;
    if (!red) {
        // "Unavailable is not 0": no row this epoch, only the counter moves.
        return;
    }
    ++ctx.counters.n_lvlauth_epochs;
    ctx.counters.lvlauth_sum += red->level_tecu;
    if (red->level_tecu < ctx.counters.lvlauth_min) ctx.counters.lvlauth_min = red->level_tecu;
    if (red->level_tecu > ctx.counters.lvlauth_max) ctx.counters.lvlauth_max = red->level_tecu;

    if (out.is_open()) {
        // FLEET row, with the SH a00 in the last column when there is one, so the
        // in-run comparison needs no join.
        out << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
            << ",FLEET,," << fmt::format("{:.4f}", red->level_tecu) << ','
            << fmt::format("{:.4f}", red->mad_tecu) << ',' << red->n_sta << ','
            << red->n_los << ',' << immature << ',';
        // The a00 field is written only when the SH channel is estimated, and is
        // blank otherwise: sh_gate_last_a00 is initialized to 0.0, which cannot be
        // told apart from a real near-zero level.  With SH on it holds the last
        // finite a00 recorded in the SH gate of an earlier epoch's solve (0.0
        // before the first one).
        if (config_.estimateNetworkIonoSh() &&
            std::isfinite(ctx.carry.sh_gate_last_a00))
            out << fmt::format("{:.4f}", ctx.carry.sh_gate_last_a00);
        out << '\n';
        // STA rows: the per-station tier-1 means, which are the d_r,GF-class
        // diagnostic and the sampling-matched grading population for G2.
        for (std::size_t i = 0; i < sta_rows.size(); ++i) {
            const auto& rows = sta_rows[i];
            if (static_cast<int>(rows.size()) < ctx.knobs.lvlauth_min_los) continue;
            double sw = 0.0, swv = 0.0;
            for (const auto& [v, w] : rows) { sw += w; swv += w * v; }
            out << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                << ",STA," << stations[i].name << ','
                << fmt::format("{:.4f}", swv / sw) << ",,,"
                << rows.size() << ",,\n";
        }
    }
}

}  // namespace GnssProcessor
