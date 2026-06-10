#pragma once

/**
 * @file SlipDetectorRegistry.hpp
 * @brief Factory that constructs cycle-slip detectors by kind / from config.
 *
 * The registry is the single place that knows how to build each concrete
 * @c ISlipDetector from the uniform @c (GnssConfig&, LogManager&) constructor
 * every detector shares.  It replaces the previous hard-coded ownership of a
 * lone @c MwSlipDetector inside @c CycleSlipManager with config-driven
 * selection of a detector set.
 *
 * Two layers:
 *   - @c create(kind, ...) — build one detector by kind, or nullptr if that
 *     kind is staged/unavailable in this build (GF / IF are not yet compiled).
 *   - @c createEnabled(config, ...) — build the ordered set the configuration
 *     selects (MW first as the primary, then auxiliaries), skipping kinds that
 *     are disabled or unavailable and logging each decision.
 *
 * Adding a new detector means: implement @c ISlipDetector, add its
 * @c SlipDetectorKind, register it in the creator table (SlipDetectorRegistry.cpp),
 * add its config toggle to @c GnssConfig::CycleSlipConfig, and list its .cpp in
 * CMakeLists.txt.  No call sites change — @c CycleSlipManager picks it up
 * automatically.
 */

#include "slip/ISlipDetector.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Canonical lower-case selection token for a detector kind
 *        (e.g. @c "mw", @c "graphic", @c "ewl", @c "gf", @c "if").
 */
std::string toString(SlipDetectorKind kind);

/**
 * @brief Parse a (case-insensitive) selection token into a detector kind.
 * @param token Token such as "mw", "graphic", "ewl", "gf", "if" (synonyms accepted).
 * @return The kind, or @c std::nullopt if unrecognized.
 */
std::optional<SlipDetectorKind> slipDetectorKindFromString(const std::string& token);

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
     * @return The detector, or nullptr if @p kind is not available in this
     *         build (e.g. staged GF / IF detectors excluded from compilation).
     */
    static std::unique_ptr<ISlipDetector> create(SlipDetectorKind kind,
                                                 const GnssConfig& config,
                                                 LogManager& log_manager);

    /**
     * @brief Whether a kind can be constructed in this build.
     * @param kind Detector kind.
     * @return true if a creator is registered for @p kind.
     */
    static bool isAvailable(SlipDetectorKind kind);

    /**
     * @brief All detector kinds with a registered creator (build-available).
     */
    static std::vector<SlipDetectorKind> availableKinds();

    /**
     * @brief Build the config-selected detector set.
     *
     * Order is deterministic: the MW detector first (when enabled — it is the
     * primary and the source of WL integers for AR / pivots), then the
     * auxiliary detectors in a fixed order.  A kind that is enabled in config
     * but unavailable in this build is skipped with a warning (so a config
     * referencing a staged detector degrades gracefully rather than failing).
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
