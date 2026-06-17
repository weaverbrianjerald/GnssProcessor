#pragma once

/**
 * @file AidingSourceRegistry.hpp
 * @brief Static factory that constructs aiding sources by kind / from config.
 *
 * The navigation-aiding analog of @c SlipDetectorRegistry.  The registry is the
 * single place that knows how to build each concrete @c IAidingSource from the
 * uniform @c (const GnssConfig&, LogManager&) constructor every source shares.
 *
 * Two layers:
 *   - @c create(kind, ...) — build one source by kind, or nullptr if that kind
 *     is staged/unavailable in this build (e.g. Vision is not yet compiled).
 *   - @c createEnabled(config, ...) — build the ordered set the configuration
 *     selects (IMU first as the primary, then auxiliaries), skipping kinds that
 *     are disabled or unavailable and logging each decision.
 *
 * Adding a new source means: implement @c IAidingSource, add its
 * @c AidingSourceKind, register it in the creator table
 * (AidingSourceRegistry.cpp), add its toggle to @c GnssConfig::AidingConfig,
 * and list its .cpp in CMakeLists.txt.  No call sites change —
 * @c AidingSourceManager picks it up automatically.
 *
 * Note: no @c TestAccess friend is declared here.  Following the tree's
 * convention, @c TestAccess shims exist only on numeric kernels (Mlambda,
 * WlsEstimator, StochasticModel); the registry's selection API
 * (@c createEnabled / @c create / @c isAvailable / @c availableKinds) is
 * already public, so registry selection is fully testable through it.
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
     * Order is deterministic: the IMU first (the primary — it owns the INS
     * error states the TC Jacobian and gap-bridging depend on), then
     * ZUPT / NHC / WheelOdom / Baro / Vision.  A kind enabled in config but
     * unavailable in this build is skipped with a warning (so a config
     * referencing a staged source degrades gracefully rather than failing).
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
