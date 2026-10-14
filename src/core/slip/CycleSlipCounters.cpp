/**
 * @file CycleSlipCounters.cpp
 * @brief The run-end table of the post-processing slip layer's counters.
 */

#include "slip/CycleSlipCounters.hpp"

#include <fmt/format.h>

namespace GnssProcessor {

std::vector<std::string> cycleSlipCounterLines(const CycleSlipCounters& counters) {
    std::vector<std::string> lines;
    lines.push_back(fmt::format("Cycle-slip counters at run end (post-processing, {} epochs):",
                                counters.epochs));
    const auto row = [&lines](const std::string& label, long long value) {
        lines.push_back(fmt::format("  {:<48}{:>10}", label, value));
    };
    for (const auto& [token, count] : counters.detections) {
        row("detections, " + token + (token == "mw" ? " (result entries)" : " (events)"), count);
    }
    row("MW mandates (ambiguity keys)", counters.mw_mandates);
    row("integer repairs (covariance kept)", counters.integer_repairs);
    row("ambiguity resets (BSSD-aware calls)", counters.ambiguity_resets);
    row("full satellite resets (satellites)", counters.full_satellite_resets);
    row("soft-LLI bridge repairs (satellites)", counters.soft_lli_bridge_repairs);
    row("suppressed MW mandates (ambiguity keys)", counters.suppressed_mandates);
    row("forced pivot handovers (constellations)", counters.forced_pivot_handovers);
    row("pivot frames repaired in place (constellations)", counters.pivot_frames_repaired_in_place);
    return lines;
}

} // namespace GnssProcessor
