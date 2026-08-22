/**
 * @file NetworkLevelAuthority.cpp
 * @brief LV-6 census implementation — see the header and
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

    // Shell convention: identical to the SH tie rows (layer 0 = the dominant
    // shell on dual-layer postures) — a different shell height would make the
    // L_cs vs a00 comparison invalid by construction.
    const auto& shc = config_.getGlobalVtecShConfig();
    const double Re = shc.earth_radius_km * 1000.0;
    const double shell_m = shc.layer_height_km * 1000.0;   // layer 0 = the dominant shell
    constexpr double kC = 299792458.0;       // m/s
    constexpr double kIonoK = 40.3e16;       // m·Hz²/TECU

    // Tier-1 inputs: per station, (vertical TECU, weight) over THIS epoch's
    // mature LOS.  Join modeled rows against the CS map — never iterate the
    // map (deliberately unpruned; stale entries live forever by design).
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
            // val is slant metres at the PAIR's primary band: STEC[TECU] =
            // val·f1²/40.3e16 with f1 = c/λ1; VTEC = STEC / M.
            const double f1 = kC / cs.lam1_m;
            const double v_tecu = cs.val * f1 * f1 / kIonoK / g.mapping;
            const double w = cs.var > 1e-12 ? 1.0 / cs.var : 1.0e12;
            if (!std::isfinite(v_tecu)) continue;
            sta_rows[i].emplace_back(v_tecu, w);
            // LV-6 G2 (§4): publish THIS row's pierce point and weight so an
            // offline reference field (GIM in the field, truth in sim) can be
            // reduced through the identical sampling.  §5 killed the naive
            // planetary-mean grade because an unmatched sampling term measures
            // the roster's geography; re-deriving the LOS set offline would
            // reintroduce it in miniature (all-visible vs mature-CS-GF-only).
            // Written BEFORE the min_los gate — a row that entered tier 1 is
            // dumped even if its station later fails to qualify for tier 2, so
            // the reference can apply the identical gate itself.
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
        // FLEET row.  a00 sampled when the SH channel has one (sentinel NaN →
        // blank field) so the in-run comparison needs no join.
        out << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
            << ",FLEET,," << fmt::format("{:.4f}", red->level_tecu) << ','
            << fmt::format("{:.4f}", red->mad_tecu) << ',' << red->n_sta << ','
            << red->n_los << ',' << immature << ',';
        // a00 column only when the SH channel exists — its 0.0 initializer is
        // otherwise indistinguishable from a real near-zero level.
        if (config_.estimateNetworkIonoSh() &&
            std::isfinite(ctx.carry.sh_gate_last_a00))
            out << fmt::format("{:.4f}", ctx.carry.sh_gate_last_a00);
        out << '\n';
        // STA rows: the per-station tier-1 means — the d_r,GF-class
        // diagnostic, and G2's sampling-matched grading population.
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
