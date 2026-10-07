/**
 * @file NetworkBrdcWire.cpp
 * @brief Card NBP's emit seam: C_wire = C_est + O with iode=k (see NetworkBrdcWire.hpp).
 */

#include "filter/NetworkBrdcWire.hpp"

#include "app/NetworkRunContext.hpp"
#include "app/NetworkSetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "products/BrdcClockApriori.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace GnssProcessor {

void networkBrdcWire(NetworkRunContext& ctx,
                     const GnssConfig& config_,
                     const std::shared_ptr<spdlog::logger>& log,
                     NetworkRunTraces& traces,
                     NetworkSetupResult& setup,
                     const SyncedEpoch& se,
                     NetworkSolutionEpoch& sol) {
    (void)config_;
    (void)log;
    if (!ctx.knobs.nbp_on || !setup.brdc_apriori) return;
    // --- Alias preamble ---
    const BrdcClockApriori& apriori = *setup.brdc_apriori;
    auto& c = ctx.counters;
    auto& trace = traces.brdc_offset_trace;

    const auto rows = apriori.stampWire(sol, se.time, &LiveEphemerisStore::getInstance(),
                                        ctx.carry.nbp_prev_iode);
    ++c.nbp_epochs;
    for (const auto& r : rows) {
        if (r.iode < 0) {
            ++c.nbp_unstamped;
        } else {
            const bool first = (c.nbp_stamped == 0);
            ++c.nbp_stamped;
            if (r.held) ++c.nbp_held; else ++c.nbp_not_held;
            if (r.switched) ++c.nbp_switches;
            c.nbp_records.emplace(r.sat, r.iode);
            c.nbp_offset_min_m = first ? r.offset_m : std::min(c.nbp_offset_min_m, r.offset_m);
            c.nbp_offset_max_m = first ? r.offset_m : std::max(c.nbp_offset_max_m, r.offset_m);
        }
        if (trace)
            trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ',' << r.sat
                  << ',' << r.iode << ',' << fmt::format("{:.6f}", r.c_est_m) << ','
                  << fmt::format("{:.6f}", r.offset_m) << ','
                  << fmt::format("{:.6f}", r.c_est_m + r.offset_m) << ','
                  << fmt::format("{:.6f}", r.brdc_m) << ',' << (r.held ? 1 : 0) << ','
                  << (r.switched ? 1 : 0) << '\n';
    }
}

} // namespace GnssProcessor
