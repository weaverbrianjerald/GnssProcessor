/**
 * @file ResumeHoldGuard.cpp
 * @brief The resume's hold guard: the release of restored ring-fix holds without the wind-up accumulators, and its
 *        seam event.  The mechanism is described in the header.
 */

#include "network/state/ResumeHoldGuard.hpp"

#include "config/GnssConfig.hpp"
#include "network/ar/RingFix.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/state/RunContext.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

namespace {

/// The station memory's state behind a release, by the guard's code (1 refused, 2 absent, 3 not restored).
const char* holdGuardWhy(long long code) {
    return code == 1 ? "refused" : (code == 2 ? "absent" : "not restored");
}

/// The released holds by constellation, "G:n,E:n,other:n".
std::string holdGuardSystems(const StateSnapshotCounters& c) {
    return fmt::format("G:{},E:{},other:{}", c.resume_hold_guard_released_gps, c.resume_hold_guard_released_gal,
                       c.resume_hold_guard_released - c.resume_hold_guard_released_gps
                           - c.resume_hold_guard_released_gal);
}

}  // namespace

void networkResumeHoldGuardRelease(NetworkRun& run) {
    NetworkRingFix* rf = run.ctx.carry.ring_fix.get();   // written: the restored links' holds
    auto& counters = run.ctx.counters;                     // written: the guard's counters
    const double snap_s = run.ctx.carry.resume.snap_s;
    const bool accumulated = run.config.accumulatePhaseWindUp();
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    if (rf == nullptr || !accumulated || counters.chain_restore_restored == 0 || counters.station_memory_restored > 0)
        return;
    // The chain section brought back held integers in the writer's frame, and the wind-up accumulators did not come
    // back: each accumulator restarts at this run's first raw angle, so a hold on an arc whose accumulator had passed
    // half a cycle sits one whole turn off.  Every restored hold is released; the links re-enter through the gate.
    const long long why = counters.station_memory_refused > 0 ? 1 : (counters.station_memory_absent > 0 ? 2 : 3);
    std::map<char, long long> by_system;
    for (auto& [id, L] : rf->links) {
        if (!L.held) continue;
        ringFixUnholdLink(L, snap_s, RingFixUnholdKind::Release);
        ++counters.resume_hold_guard_released;
        ++by_system[id.sat.empty() ? '?' : id.sat[0]];
    }
    counters.resume_hold_guard_released_gps = by_system['G'];
    counters.resume_hold_guard_released_gal = by_system['E'];
    counters.resume_hold_guard_why = why;
    const long long n = counters.resume_hold_guard_released;
    const std::string systems = holdGuardSystems(counters);
    log->warn("Network (resume hold guard): the chain memory section was restored but the station memory section was "
              "{}: every wind-up accumulator restarts at this run's first epoch, so {} restored ring-fix hold(s) ({}) "
              "are RELEASED before the first epoch; the links re-enter through the ring fix's gate.",
              holdGuardWhy(why), n, systems);
}

void networkResumeHoldGuardSeamEvent(NetworkRun& run, const NetworkEpoch& ep) {
    auto& counters = run.ctx.counters;   // written: the event's count
    NetworkRunTraces& traces = run.traces;
    const GnssTime& t = ep.se.time;
    // ---- End of the bindings ----
    if (counters.resume_hold_guard_why == 0 || counters.resume_hold_guard_events_written > 0) return;
    // The release is a decoupling edge (standing invariant 7): one event at the first resumed epoch with how many
    // restored holds were released and why; each link's re-coupling is the ring fix's own gate and its HOLD event.
    traces.emitEvent(t, "RESUME", "hold_guard_release", "", "",
                     static_cast<double>(counters.resume_hold_guard_released), std::nullopt, "released",
                     fmt::format("holds_released={};by_system={};station_memory={};the links re-enter through the "
                                 "ring fix's gate",
                                 counters.resume_hold_guard_released, holdGuardSystems(counters),
                                 holdGuardWhy(counters.resume_hold_guard_why)));
    ++counters.resume_hold_guard_events_written;
}

}  // namespace GnssProcessor
