#pragma once

/**
 * @file IonoVariabilityMonitor.hpp
 * @brief Per-satellite ionospheric disturbance monitor that drives the
 *        adaptive ionospheric process noise for PPP-RTK.
 *
 * The monitor forms, per satellite and per epoch, the geometry-free (GF)
 * carrier-phase combination of the primary low-band pair:
 *
 *   GF(t) = L1(t, m) - L2(t, m)
 *
 * and its second-order time difference:
 *
 *   TD2(t) = GF(t) - 2*GF(t-1) + GF(t-2)
 *
 * The first difference cancels geometry, clock, and troposphere; the
 * second difference additionally cancels the constant carrier-phase
 * ambiguity and any constant/linear ionospheric trend, leaving a proxy
 * for ionospheric (TEC) acceleration plus measurement noise.  Cycle slips
 * appear as isolated single-epoch outliers and are absorbed by a robust
 * (MAD) statistic.
 *
 * Per satellite the monitor compares a recent-window MAD-sigma of TD2 to
 * a baseline-window MAD-sigma.  The squared ratio, clamped to
 * [1, max_multiplier], scales the static ionospheric process-noise
 * spectral density (ProcessNoise::ionosphere).  Using the ratio
 * self-normalizes: the slowly varying geometric (obliquity-curvature)
 * component sits in both windows and cancels, so quiet conditions yield a
 * multiplier near 1 at any elevation and the existing 1/sin^2(el)
 * obliquity scaling is not double-counted.  The resulting per-satellite
 * zenith process noise is exposed via processNoiseOverrides() for
 * StateManager::setIonoProcessNoiseOverrides().
 *
 * This is a measurement-domain monitor: it owns per-arc GF history only,
 * never filter state.  The GF math is adapted from the (staged,
 * uncompiled) GfSlipDetector.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @class IonoVariabilityMonitor
     * @brief Tracks per-satellite GF phase second-difference variability
     *        and produces per-satellite ionospheric process-noise overrides.
     */
    class IonoVariabilityMonitor {
    public:
        /**
         * @brief Construct the monitor.
         * @param config      Processing configuration (supplies AdaptiveIonoQConfig
         *                    and the baseline ProcessNoise::ionosphere).
         * @param log_manager Logging facility.
         */
        IonoVariabilityMonitor(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Process one epoch of modeled observations and refresh the
         *        per-satellite process-noise override map.
         *
         * Call once per epoch, before the Kalman prediction step that
         * consumes the overrides.
         *
         * @param observations Per-satellite modeled observations for the epoch.
         */
        void update(const std::vector<PppObservation>& observations);

        /**
         * @brief Per-satellite zenith ionospheric process-noise overrides
         *        (m^2/s) from the most recent update().
         *
         * Feed directly to StateManager::setIonoProcessNoiseOverrides().
         *
         * @return Map from satellite ID to zenith process-noise variance rate.
         */
        const std::map<std::string, double>& processNoiseOverrides() const {
            return overrides_;
        }

        /**
         * @brief Per-satellite dimensionless variability multipliers from the
         *        most recent update() — the clamped recent/baseline TD2
         *        MAD-sigma ratio squared, before any q scaling.
         *
         * For consumers whose noise base is NOT ProcessNoise::ionosphere
         * (e.g. the network T5v2 GM deviation innovation), which scale their
         * own base by this factor instead of adopting the zenith q override.
         *
         * @return Map from satellite ID to multiplier in [1, max_multiplier].
         */
        const std::map<std::string, double>& multipliers() const {
            return multipliers_;
        }

        /**
         * @brief Clear all GF arc history for a satellite (e.g. on a slip).
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite);

        /**
         * @brief Enable CSV tracing of the per-satellite GF diagnostics.
         * @param log_dir Directory where the trace file is created.
         * @return true if the trace file was opened.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

        /// @brief Whether CSV tracing is currently active.
        bool isTracingEnabled() const { return trace_file_.is_open(); }

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;

        /**
         * @brief Per-satellite GF arc state (one tracked pair per satellite).
         */
        struct GfArc {
            double prev_gf{ 0.0 };          ///< GF at epoch t-1 (m).
            double prev_prev_gf{ 0.0 };     ///< GF at epoch t-2 (m).
            double last_epoch_s{ 0.0 };     ///< Continuous GPS seconds of last update.
            std::size_t count{ 0 };         ///< Number of GF samples on this arc.
            std::deque<double> td2_window;  ///< Rolling TD2 history (m), length <= baseline_window.
        };

        /// Per-(satellite + signal-pair) GF arc state.  Key: "G01_L1C_L2W".
        std::map<std::string, GfArc> arcs_;

        /// Per-satellite zenith process-noise overrides (m^2/s) from last update().
        std::map<std::string, double> overrides_;

        /// Per-satellite dimensionless multipliers from last update() (see multipliers()).
        std::map<std::string, double> multipliers_;

        /// Maximum inter-epoch gap (seconds) before an arc is reset.
        static constexpr double kMaxGapSeconds = 120.0;

        /// Floor on the baseline MAD-sigma (m) guarding the ratio denominator.
        static constexpr double kBaselineSigmaFloorM = 1.0e-4;

        /**
         * @brief Select a satellite's primary dual-frequency phase pair
         *        (the two lowest RINEX bands present).
         * @return true if at least two phase bands were found.
         */
        bool selectPrimaryPair(const PppObservation& obs,
                               std::string& sig1, std::string& sig2) const;

        /**
         * @brief Compute the geometry-free phase combination L1 - L2 (m).
         * @return GF in meters, or NaN if either signal is unavailable.
         */
        double computeGfMeters(const PppObservation& obs,
                               const std::string& sig1,
                               const std::string& sig2) const;

        /**
         * @brief Robust (MAD-derived) sigma of a value set.
         * @return Gaussian-equivalent sigma, or NaN if fewer than 5 samples.
         */
        static double computeMadSigma(const std::vector<double>& values);

        /// Output stream for the optional CSV trace.
        mutable std::ofstream trace_file_;

        /// Epoch time of the current update() call (for tracing).
        GnssTime current_epoch_;
    };

} // namespace GnssProcessor
