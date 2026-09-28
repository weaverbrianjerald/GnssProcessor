#pragma once
/**
 * @file Namespaces.hpp
 * @brief Lightweight namespace grouping and aliases for incremental refactor.
 *
 * Provides `GnssProcessor::Model`, `GnssProcessor::Atmosphere`, `GnssProcessor::IO`
 * and `GnssProcessor::Filter` as grouping namespaces through using-declarations, so
 * types can be migrated into nested namespaces step by step.
 *
 * Aliases are added or removed here as types move into real nested namespaces.
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
 * Re-exports model types from the top-level `GnssProcessor` namespace, giving a
 * single place to migrate them into a true nested namespace during incremental
 * refactor.  `Geometry` is already declared in `GnssProcessor::Model`
 * (model/ModelGeometry.hpp); its using-declaration here is redundant but harmless.
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
 * Re-exports the ionosphere and troposphere types to ease migration.
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
 * Re-exports the Kalman filter, state manager, WLS estimator and MW slip detector
 * types to support incremental namespace migration.
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
 * Holds aliases intended for internal implementation only, not for the public
 * API.  Contents are kept private to the translation unit where possible.
 */
namespace internal {
    // Implementation-only aliases go here; currently empty.
}

} // namespace GnssProcessor