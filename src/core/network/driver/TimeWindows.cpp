/**
 * @file TimeWindows.cpp
 * @brief The value in force of each time window of the network producer and the run-start
 *        table "Network (time windows as armed)" (network/driver/TimeWindows.hpp).
 */

#include "network/driver/TimeWindows.hpp"

#include "config/GnssConfig.hpp"
#include "network/driver/Run.hpp"   // NetworkRun

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// The printed name of a unit.
const char* unitName(NetworkWindowUnit u) {
    switch (u) {
        case NetworkWindowUnit::Seconds:  return "s";
        case NetworkWindowUnit::Epochs:   return "epochs";
        case NetworkWindowUnit::PerEpoch: return "per epoch";
    }
    return "?";
}

/// The printed name of a counting clock.
const char* clockName(NetworkWindowClock c) {
    return c == NetworkWindowClock::Presence ? "presence" : "decision";
}

/// A value in a window's unit back in seconds (per second for a count per epoch) at the reference interval.
double toSeconds(NetworkWindowUnit unit, double value) {
    return unit == NetworkWindowUnit::Epochs   ? value * kNetworkReferenceIntervalS
         : unit == NetworkWindowUnit::PerEpoch ? value / kNetworkReferenceIntervalS
                                               : value;
}

/// A value in a window's unit, printed without trailing zeros (a count as a whole number).
std::string printValue(NetworkWindowUnit unit, double value) {
    return unit == NetworkWindowUnit::Seconds ? fmt::format("{:g}", value)
                                              : fmt::format("{}", networkTimeWindowRound(value));
}

}  // namespace

double networkTimeWindowInForce(NetworkTimeWindow w, const GnssConfig& config) {
    const NetworkConfig& nc = config.getNetworkConfig();
    switch (w) {
        case NetworkTimeWindow::ClockAnchorMaxGap:   return config.networkClockAnchorMaxGapS();
        case NetworkTimeWindow::RingFixMaxDecisions: return nc.ring_fix.max_decisions_per_epoch;
        case NetworkTimeWindow::RingFixSettledWait:  return nc.ring_fix.settled_wait_emissions;
        case NetworkTimeWindow::RingFixNnGuardMin:   return nc.ring_fix.nn_guard_min_epochs;
        case NetworkTimeWindow::ArSuspect:           return nc.ambiguity_resolution.suspect_epochs;
        case NetworkTimeWindow::PhaseLevelingWarmup: return config.networkPhaseLevelingWarmupEpochs();
        case NetworkTimeWindow::ClockAnchorRecouple: return config.networkClockAnchorRecoupleEpochs();
        case NetworkTimeWindow::PhaseLevelingMaxGap: return config.networkPhaseLevelingMaxGapS();
        case NetworkTimeWindow::PhaseColourMaxGap:   return nc.phase_colour_max_gap_s;
        case NetworkTimeWindow::ReadmitSeedHorizon:  return config.networkReadmitSeedHorizonS();
        case NetworkTimeWindow::RingFixGap:
        case NetworkTimeWindow::RingFixPersist:
        case NetworkTimeWindow::RingFixMonitor:
        case NetworkTimeWindow::ReadmitMinGap:
        case NetworkTimeWindow::RxCbMaxGap:
        case NetworkTimeWindow::ReadmitVerify:
        case NetworkTimeWindow::RingFixWindow:
        case NetworkTimeWindow::ClockEmitRebaseGap:
        case NetworkTimeWindow::Count:
            break;
    }
    return networkTimeWindowDefault(w);   // a constant row
}

std::vector<std::string> networkTimeWindowsTable(const GnssConfig& config) {
    std::vector<std::string> lines;
    lines.push_back(fmt::format(
        "{} windows; an epoch count resolves at the reference interval of {:g} s (not the sampling "
        "interval); presence = epochs in which it was observed, decision = solves that decide",
        kNetworkTimeWindowCount, kNetworkReferenceIntervalS));
    lines.push_back(fmt::format("{:<32} {:<9} {:<10} {:>8} {:>10}  {}",
                                "window", "clock", "unit", "value", "s at ref", "key"));
    for (const NetworkTimeWindowRow& r : kNetworkTimeWindows) {
        const double in_force = networkTimeWindowInForce(r.id, config);
        const double dflt     = networkTimeWindowDefault(r.id);
        const bool   keyed    = r.config_key[0] != '\0';
        std::string line = fmt::format("{:<32} {:<9} {:<10} {:>8} {:>10}  {}",
                                       r.name, clockName(r.clock), unitName(r.unit),
                                       printValue(r.unit, in_force),
                                       fmt::format("{:g}", toSeconds(r.unit, in_force)),
                                       keyed ? r.config_key : "constant");
        if (keyed && in_force != dflt)
            line += fmt::format(" (default {})", printValue(r.unit, dflt));
        lines.push_back(std::move(line));
    }
    return lines;
}

void networkTimeWindowsLog(const NetworkRun& run) {
    for (const std::string& line : networkTimeWindowsTable(run.config))
        run.log->info("Network (time windows as armed): {}", line);
}

}  // namespace GnssProcessor
