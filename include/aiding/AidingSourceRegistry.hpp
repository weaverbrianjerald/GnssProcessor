#pragma once

/**
 * @file AidingSourceRegistry.hpp
 * @brief Static factory that constructs aiding sources by kind or from config.
 *
 * The navigation-aiding analog of @c SlipDetectorRegistry.  The registry is the
 * one place that knows how to build each concrete @c IAidingSource from the
 * uniform @c (const GnssConfig&, LogManager&) constructor that every source shares.
 *
 * Two layers:
 *   - @c create(kind, ...): build one source by kind, or nullptr if that kind
 *     is staged or unavailable in this build (e.g. Vision is not compiled).
 *   - @c createEnabled(config, ...): build the ordered set the configuration
 *     selects (IMU first as the primary, then auxiliaries), skipping kinds that
 *     are disabled or unavailable; each built source is logged at info level
 *     and each unavailable one with a warning.
 *
 * Adding a new source: implement @c IAidingSource, add its
 * @c AidingSourceKind (with its tokens in @c toString / @c aidingKindFromString),
 * register it in the creator table and in the selection list of
 * @c createEnabled (AidingSourceRegistry.cpp), add its toggle to @c AidingConfig
 * (config/GnssConfigTypes.hpp), and list its .cpp in CMakeLists.txt.
 * @c AidingSourceManager builds its set through @c createEnabled, so its call
 * sites do not change.
 *
 * No @c TestAccess friend is declared here.  Such friends exist on classes whose
 * private state tests must reach (e.g. ImuAidingSource, KalmanFilter,
 * WlsEstimator, StochasticModel); the registry's selection API
 * (@c createEnabled / @c create / @c isAvailable / @c availableKinds) is
 * public, so registry selection is testable through it.
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "aiding/IAidingSource.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace GnssProcessor {

/**
 * @class AidingSourceRegistry
 * @brief Static factory for @c IAidingSource instances.
 *
 * Stateless: all methods are static and consult a process-wide creator table
 * (a Meyers singleton, mirroring @c SlipDetectorRegistry).  Sources are
 * returned as @c std::unique_ptr<IAidingSource>, matching the codebase's
 * owner-holds-unique_ptr convention.
 */
class AidingSourceRegistry {
public:
    /// Creator signature: every source takes the uniform 2-arg constructor.
    using Creator =
        std::function<std::unique_ptr<IAidingSource>(const GnssConfig&, LogManager&)>;

    /**
     * @brief Construct a single source by kind.
     * @param kind        Source to build.
     * @param config      Processing configuration.
     * @param log_manager Logging facility.
     * @return The source, or nullptr if @p kind is not available in this build
     *         (e.g. the staged Vision source excluded from compilation).
     */
    static std::unique_ptr<IAidingSource> create(AidingSourceKind kind,
                                                 const GnssConfig& config,
                                                 LogManager& log_manager);

    /// @brief Whether a kind can be constructed in this build.
    static bool isAvailable(AidingSourceKind kind);

    /// @brief All kinds with a registered creator (build-available).
    static std::vector<AidingSourceKind> availableKinds();

    /**
     * @brief Build the config-selected source set.
     *
     * The order is fixed: the IMU first (the primary, which owns the INS error
     * states that the TC Jacobian and gap-bridging depend on), then
     * ZUPT / NHC / WheelOdom / Baro / Vision.  A kind enabled in config but
     * unavailable in this build is skipped with a warning, so a config that
     * selects a staged source still runs instead of failing.
     *
     * @param config      Processing configuration (queried via getAidingConfig()).
     * @param log_manager Logging facility.
     * @return Owned sources in run order (never contains nullptr).
     */
    static std::vector<std::unique_ptr<IAidingSource>> createEnabled(
        const GnssConfig& config, LogManager& log_manager);

private:
    /// Process-wide kind->creator table (Meyers singleton).
    static const std::map<AidingSourceKind, Creator>& creators();
};

} // namespace GnssProcessor
