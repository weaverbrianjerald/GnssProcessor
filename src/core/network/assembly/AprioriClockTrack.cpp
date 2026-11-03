/**
 * @file AprioriClockTrack.cpp
 * @brief The broadcast a priori clock tracked by the network's own phase (see AprioriClockTrack.hpp).
 */

#include "network/assembly/AprioriClockTrack.hpp"

#include "common/GnssIdentity.hpp"   // obsTypeOf
#include "common/PppObservation.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace GnssProcessor {

AprioriTrackEpoch aprioriClockTrackApply(const std::map<std::string, double>& columns,
                                         std::map<std::string, double>& track,
                                         std::map<std::string, int>& paused,
                                         std::vector<std::vector<PppObservation>>& modeled) {
    AprioriTrackEpoch out;
    for (const auto& [sat, col] : columns) {
        const double inc = -col;
        const double a = std::abs(inc);
        const std::size_t bin = a < 0.005 ? 0 : a < 0.01 ? 1 : a < 0.02 ? 2 : a < 0.05 ? 3 : a < 0.10 ? 4 : 5;
        ++out.inc_hist[bin];
        const auto it = track.find(sat);
        if (it == track.end()) {
            track.emplace(sat, inc);
            out.started.push_back(sat);
        } else {
            it->second += inc;
            const auto pit = paused.find(sat);
            if (pit != paused.end() && pit->second > 0) out.resumed.emplace(sat, pit->second);
        }
        paused[sat] = 0;
        ++out.applied;
    }
    for (auto& [sat, n] : paused)
        if (!columns.count(sat)) { ++n; ++out.paused; }
    for (const auto& [sat, u] : track) out.max_abs_track_m = std::max(out.max_abs_track_m, std::abs(u));
    for (auto& station : modeled)
        for (auto& obs : station) {
            const auto it = track.find(obs.satellite);
            if (it == track.end() || it->second == 0.0) continue;
            // Code and phase carry the satellite clock; a Doppler entry is a range rate.
            for (auto& [sig, v] : obs.modeled) {
                const ObsType kind = obsTypeOf(sig);
                if (kind != ObsType::Code && kind != ObsType::Phase) continue;
                v -= it->second;
                ++out.rows_moved;
            }
        }
    return out;
}

void networkAprioriClockTrack(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a) {
    // ---- From the run and the epoch ----
    // Reads bind through `ep`; writes go through the output parameters and are marked
    // `// written`.
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    const GnssTime& t = ep.se.time;
    const std::map<std::string, double>& columns = ep.events.slip.sat_centre_col;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    // ---- End of the bindings ----
    if (!ctx.knobs.apriori_track_on) return;   // OFF: no track, no moved row, no event

    net::NetworkEntities& entities = ctx.entities;   // written: sats
    std::map<std::string, double> track = net::satMap(ctx.entities.sats, &net::SatelliteState::apriori_track_m);
    std::map<std::string, int> paused = net::satMap(ctx.entities.sats, &net::SatelliteState::apriori_track_paused);
    const AprioriTrackEpoch rec = aprioriClockTrackApply(columns, track, paused, modeled);
    net::assignSat(entities, &net::SatelliteState::apriori_track_m, track);
    net::assignSat(entities, &net::SatelliteState::apriori_track_paused, paused);

    auto& c = ctx.counters;
    ++c.brd_track_epochs;
    c.brd_track_applied += rec.applied;
    c.brd_track_paused += rec.paused;
    c.brd_track_started += static_cast<long long>(rec.started.size());
    c.brd_track_resumed += static_cast<long long>(rec.resumed.size());
    c.brd_track_rows_moved += rec.rows_moved;
    for (std::size_t k = 0; k < rec.inc_hist.size(); ++k) c.brd_track_inc_hist[k] += rec.inc_hist[k];
    c.brd_track_max_abs_m = std::max(c.brd_track_max_abs_m, rec.max_abs_track_m);
    for (const auto& sat : rec.started)
        traces.emitEvent(t, "BRD_TRACK", "track_start", sat, "", std::nullopt, std::nullopt,
                         "track", fmt::format("u={:.4f}", track[sat]));
    for (const auto& [sat, n] : rec.resumed)
        traces.emitEvent(t, "BRD_TRACK", "track_resume", sat, "", static_cast<double>(n), std::nullopt,
                         "track", fmt::format("paused={};u={:.4f}", n, track[sat]));
}

} // namespace GnssProcessor
