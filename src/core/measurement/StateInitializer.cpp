/**
 * @file StateInitializer.cpp
 * @brief The construction and the system helpers of the first-epoch seeding latches (see
 *        measurement/StateInitializer.hpp; the seeds themselves are in measurement/Seeds.cpp
 *        and the receiver code-bias resolver is in measurement/ReceiverBias.cpp).
 */

#include "measurement/StateInitializer.hpp"
#include "model/PppModel.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "measurement/UserParamView.hpp"   // userCodeBiasKey
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

// ===========================================================================
// Construction
// ===========================================================================

StateInitializer::StateInitializer(const GnssConfig& config,
                                   const PppModel& ppp_model,
                                   LogManager& log_manager)
    : resolver_(config, ppp_model, log_manager) {
}

// ===========================================================================
// System enum helpers
// ===========================================================================

std::string StateInitializer::systemToIsbName(System system) {
    // The time reference has no ISB state; an unknown system has none either.
    if (isTimeReference(system) || system == System::Unknown) return "";
    return systemName(system);
}

char StateInitializer::systemToClockRefChar(System system) {
    return ReceiverBiasResolver::systemToClockRefChar(system);
}

} // namespace GnssProcessor
