/**
 * @file ObsEquationCheckRunEnd.cpp
 * @brief The run-end report of the observation-equation check (see
 *        network/runend/ObsEquationCheckRunEnd.hpp).
 */

#include "network/runend/ObsEquationCheckRunEnd.hpp"

#include "config/GnssConfig.hpp"
#include "measurement/ObservationEquationCheck.hpp"
#include "network/runend/RunEnd.hpp"

#include <spdlog/spdlog.h>

namespace GnssProcessor {

int networkObsEquationCheckRunEnd(const NetworkRunEndView& v) {
    const GnssConfig& config = v.config;
    const auto& log = v.log;

    if (!config.obsEquationCheck()) return 0;
    const std::vector<std::string> lines = obsEquationCheckReport();
    if (lines.empty()) {
        log->info("obs_equation_check: no row was compared");
        return 0;
    }
    for (const std::string& line : lines) log->info("{}", line);
    return 0;
}

} // namespace GnssProcessor
