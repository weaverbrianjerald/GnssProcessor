/**
 * @file RowTrace.cpp
 * @brief The decomposition trace of a code or phase row (see measurement/RowTrace.hpp).
 */

#include "measurement/RowTrace.hpp"

#include "common/GnssTypes.hpp"      // classifyMeasurement
#include "logging/LogManager.hpp"

namespace GnssProcessor {

namespace {

/// The logger of the row traces.  The name is built once: a row asks for the logger per call and
/// a literal longer than the small-string size would allocate each time.
const std::string& rowLoggerName() {
    static const std::string name = "measurement_model";
    return name;
}

}  // namespace

bool wantsRowDecomposition(LogManager& log, const PppObservation& obs) {
    return !obs.satellite.empty()
        && log.getLogger(rowLoggerName())->should_log(spdlog::level::trace);
}

void traceRowDecomposition(LogManager& log, const PppObservation& obs, const std::string& ref_type,
                           double predicted, const ObsTermValues& terms) {
    auto lg = log.getLogger(rowLoggerName());
    const double observed_val = obs.observed.count(ref_type) ? obs.observed.at(ref_type) : 0.0;
    if (classifyMeasurement(ref_type) == MeasurementType::Phase) {
        lg->trace("PHASE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                  "zd_innov={:+.3f}m | rx_clk={:+.3f} phase_clk={:+.3f} "
                  "iono={:+.3f} ssr_pbias={:+.3f} sat_pbias={:+.3f} "
                  "sat_clk={:+.3f} amb={:+.3f}",
                  obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                  observed_val - predicted, terms[ObsEffect::ReceiverClock],
                  terms[ObsEffect::PhaseClock], terms[ObsEffect::Ionosphere],
                  terms[ObsEffect::SsrBias], terms[ObsEffect::SatellitePhaseBias],
                  terms[ObsEffect::SatelliteClock], terms[ObsEffect::Ambiguity]);
    } else {
        lg->trace("CODE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                  "zd_innov={:+.3f}m | rx_clk={:+.3f} iono={:+.3f} ssr_cbias={:+.3f} "
                  "sat_bias={:+.3f} bia_cbias={:+.3f}",
                  obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                  observed_val - predicted, terms[ObsEffect::ReceiverClock],
                  terms[ObsEffect::Ionosphere], terms[ObsEffect::SsrBias],
                  terms[ObsEffect::SatelliteCodeBias], terms[ObsEffect::ReceiverCodeBias]);
    }
}

} // namespace GnssProcessor
