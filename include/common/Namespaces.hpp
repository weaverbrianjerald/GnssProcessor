#pragma once
/**
 * @file Namespaces.hpp
 * @brief Lightweight namespace grouping and aliases for incremental refactor.
 *
 * Provide `GnssProcessor::Model`, `GnssProcessor::IO`, `GnssProcessor::Filter`
 * as grouping namespaces via type aliases to allow stepwise migration.
 *
 * Add or remove aliases as you move types into actual nested namespaces.
 */

#include "model/PppModel.hpp"
#include "model/ModelGeometry.hpp"
#include "model/Antenna.hpp"
#include "model/Planetary.hpp"

#include "rinex/RinexParser.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductCache.hpp"
#include "rtcm/RtcmParser.hpp"

#include "filter/KalmanFilter.hpp"
#include "filter/StateManager.hpp"
#include "math/WlsEstimator.hpp"
#include "slip/MwSlipDetector.hpp"

namespace GnssProcessor {

/**
 * @namespace GnssProcessor::Model
 * @brief Grouping aliases for model-related public types.
 *
 * This namespace re-exports model types from the top-level `GnssProcessor`
 * to provide a single place to migrate types into a true nested namespace
 * during incremental refactor.
 *
 * Included types:
 * - PppModel
 * - Geometry
 * - Antenna
 * - Planetary
 */
namespace Model {
    using ::GnssProcessor::PppModel;
    using ::GnssProcessor::Model::Geometry;
    using ::GnssProcessor::Antenna;
    using ::GnssProcessor::Planetary;
}

/**
 * @namespace GnssProcessor::Atmosphere
 * @brief Grouping aliases for atmospheric model types.
 *
 * Re-exports ionosphere and troposphere related types to ease migration.
 */
namespace Atmosphere {
    using ::GnssProcessor::Ionosphere;
    using ::GnssProcessor::Troposphere;
}

/**
 * @namespace GnssProcessor::IO
 * @brief Grouping aliases for input/output and parser types.
 *
 * Re-exports parser classes so callers can be migrated to `GnssProcessor::IO::*`
 * without changing type names across the codebase immediately.
 */
namespace IO {
    using ::GnssProcessor::RinexParser;
    using ::GnssProcessor::ProductParser;
    using ::GnssProcessor::ProductCache;
    using ::GnssProcessor::RtcmParser;
}

/**
 * @namespace GnssProcessor::Filter
 * @brief Grouping aliases for filtering, estimation and state management.
 *
 * Contains Kalman, WLS and other estimator/state manager types to support
 * incremental namespace migration.
 */
namespace Filter {
    using ::GnssProcessor::KalmanFilter;
    using ::GnssProcessor::StateManager;
    using ::GnssProcessor::WlsEstimator;
    using ::GnssProcessor::MwSlipDetector;
}

/**
 * @namespace GnssProcessor::internal
 * @brief Implementation-only aliases.
 *
 * Use this namespace for aliases intended only for internal implementation
 * and not for the public API. Keep contents private to the translation unit
 * where possible.
 */
namespace internal {
    // Add implementation-only aliases as you create them.
}

} // namespace GnssProcessor