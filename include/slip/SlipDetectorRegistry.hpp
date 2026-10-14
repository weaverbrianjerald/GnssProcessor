#pragma once

/**
 * @file SlipDetectorRegistry.hpp
 * @brief Factory that constructs cycle-slip detectors by kind or from the config.
 *
 * The registry is the single place that builds each concrete user-side
 * @c ISlipDetector from the @c (GnssConfig&, LogManager&) constructor that every
 * detector shares; @c CycleSlipManager obtains its detector set from
 * @c createEnabled.  Registered today: MW, GRAPHIC and EWL.
 *
 * Two layers:
 *   - @c create(kind, ...): build one detector by kind, or nullptr if no creator
 *     is registered for that kind.  GF is staged (its .cpp is not compiled); IF is
 *     compiled but built directly by the network path, not through the registry.
 *   - @c createEnabled(config, ...): build the ordered set the configuration
 *     selects (MW first as the primary, then the auxiliaries), skipping kinds that
 *     are disabled or unavailable and logging each decision.
 *
 * Adding a detector:
 *   1. Implement @c ISlipDetector and add its @c SlipDetectorKind.
 *   2. Register it in the creator table (SlipDetectorRegistry.cpp).
 *   3. Add its config toggle to @c CycleSlipConfig and add the kind to the
 *      selection list in @c createEnabled.
 *   4. List its .cpp in CMakeLists.txt.
 * No call site in @c CycleSlipManager changes.
 */

#include "slip/ISlipDetector.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Canonical lower-case token for a detector kind, as printed by the
 *        registry's selection log and the auxiliary-reset log lines.
 * @param kind Detector kind.
 * @return @c "mw", @c "graphic", @c "ewl", @c "gf" or @c "if" (@c "unknown"
 *         for a value outside the enumeration).
 */
std::string toString(SlipDetectorKind kind);

/**
 * @class SlipDetectorRegistry
 * @brief Static factory for @c ISlipDetector instances.
 *
 * Stateless: all methods are static and consult a process-wide creator table
 * (a Meyers singleton, mirroring @c baselineFixturePresets).  Detectors are
 * returned as @c std::unique_ptr<ISlipDetector>, matching the codebase's
 * owner-holds-unique_ptr convention.
 */
class SlipDetectorRegistry {
public:
    /// Creator signature: every detector takes the uniform 2-arg constructor.
    using Creator =
        std::function<std::unique_ptr<ISlipDetector>(const GnssConfig&, LogManager&)>;

    /**
     * @brief Construct a single detector by kind.
     * @param kind        Detector to build.
     * @param config      Processing configuration.
     * @param log_manager Logging facility.
     * @return The detector, or nullptr if no creator is registered for @p kind
     *         (the staged, uncompiled GF detector, or the IF detector, which the
     *         network path builds directly).
     */
    static std::unique_ptr<ISlipDetector> create(SlipDetectorKind kind,
                                                 const GnssConfig& config,
                                                 LogManager& log_manager);

    /**
     * @brief Build the config-selected detector set.
     *
     * Order is deterministic: the MW detector first when enabled (it is the
     * primary and the source of WL integers for AR and pivots), then the
     * auxiliary detectors in a fixed order.  A kind that is enabled in the config
     * but has no registered creator is skipped with a warning, so a config that
     * selects a staged detector still runs instead of failing.  The warning
     * reads "staged/uncompiled" for both kinds without a creator; that is true
     * of GF only, since IF is compiled and built by the network path alone.
     *
     * @param config      Processing configuration (queried via getCycleSlipConfig()).
     * @param log_manager Logging facility.
     * @return Owned detectors in run order (never contains nullptr).
     */
    static std::vector<std::unique_ptr<ISlipDetector>> createEnabled(
        const GnssConfig& config, LogManager& log_manager);

private:
    /// Process-wide kind→creator table (Meyers singleton).
    static const std::map<SlipDetectorKind, Creator>& creators();
};

} // namespace GnssProcessor
