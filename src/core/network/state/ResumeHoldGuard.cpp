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
#include "model/PppModel.hpp"
#include "network/driver/RunTypes.hpp"   // NetStation (the models)
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

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

void networkResumeSeedRestartArm(NetworkRun& run) {
    std::vector<NetStation>& stations = run.setup.stations;   // written: each model's seed rule
    auto& counters = run.ctx.counters;                         // written: the step's counters
    const net::ArcTable& arc_rows = run.ctx.entities.arcs;
    const bool accumulated = run.config.accumulatePhaseWindUp();
    const bool armed = run.config.networkResumeStationMemoryRestore();
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    const bool missing = counters.station_memory_refused > 0 || counters.station_memory_absent > 0;
    if (!accumulated || !armed || counters.station_memory_restored > 0 || !missing) return;
    // The resumed run holds no wind-up accumulator: each satellite's first epoch seeds it at the raw angle where the
    // uninterrupted run continued its unwrapped angle, the step of an accumulator restart.  Each station's model
    // records that seed in its restart ledger, and the restart response (network.cycle_slip.windup_restart_refloat)
    // refloats the arcs' carries as it does for every restart: one rule for one event.
    long long carries = 0;
    net::forEach(arc_rows, &net::ArcState::amb, [&](const ArcId&, const LocalCarry&) { ++carries; });
    long long n = 0;
    for (NetStation& st : stations) {
        if (!st.model) continue;
        st.model->setWindUpSeedAsRestart(true);
        ++n;
    }
    counters.resume_seed_restart_stations = n;
    counters.resume_seed_amb_carries = carries;
    counters.resume_seed_why = counters.station_memory_refused > 0 ? 1 : 2;
    log->warn("Network (resume seed restart): the station memory section was {}: no wind-up accumulator came back, so "
              "the first seed of every satellite at {} station(s) enters the restart ledger and the restart response "
              "refloats the arcs' carries ({} ambiguity carr(y|ies) restored){}.",
              holdGuardWhy(counters.resume_seed_why), n, carries,
              run.config.networkCycleSlipWindupRestartRefloat()
                  ? std::string()
                  : std::string("; network.cycle_slip.windup_restart_refloat is OFF, so nothing responds"));
}

void networkResumeHoldGuardSeamEvent(NetworkRun& run, const NetworkEpoch& ep) {
    auto& counters = run.ctx.counters;   // written: the event's count
    NetworkRunTraces& traces = run.traces;
    const GnssTime& t = ep.se.time;
    // ---- End of the bindings ----
    if (counters.resume_seed_why != 0 && counters.resume_seed_events_written == 0) {
        // The seed rule is a decoupling edge too: one event with the population the restart response refloats; each
        // refloat is the response's own T0_windup row (detail "resume seed without the accumulators").
        traces.emitEvent(t, "RESUME", "seed_restart_arm", "", "", static_cast<double>(counters.resume_seed_amb_carries),
                         std::nullopt, "armed",
                         fmt::format("stations={};amb_carries_restored={};station_memory={};each satellite's first "
                                     "seed enters the restart ledger and its arcs are refloated by the restart response",
                                     counters.resume_seed_restart_stations, counters.resume_seed_amb_carries,
                                     holdGuardWhy(counters.resume_seed_why)));
        ++counters.resume_seed_events_written;
    }
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
