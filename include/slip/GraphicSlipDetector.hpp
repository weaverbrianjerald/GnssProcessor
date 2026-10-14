#pragma once

/**
 * @file GraphicSlipDetector.hpp
 * @brief GRAPHIC (GRoup And PHase Ionospheric Correction) per-signal cycle slip detector.
 *
 * The GRAPHIC combination averages the code and phase measurements on the same
 * frequency.  The first-order ionospheric delay enters code and phase with
 * opposite signs and cancels, leaving an observable whose noise is about half
 * the code noise:
 *
 *     GRAPHIC_i(t) = ( P_i(t) + lambda_i * L_i(t) ) / 2
 *
 * This observable contains the geometric range, clocks, troposphere, half the
 * code multipath, and a constant ambiguity term (lambda_i * N_i / 2).  The
 * between-epoch first difference eliminates the constant ambiguity:
 *
 *     dG_i(t) = GRAPHIC_i(t) - GRAPHIC_i(t-1)
 *
 * A cycle slip of dN cycles on signal i produces a jump of dN * lambda_i / 2.
 * With code noise of ~0.3 m, GRAPHIC noise is ~0.15 m and dG noise ~0.21 m,
 * so a 0.6 m (about 3 sigma) jump corresponds to ~6.3 L1 cycles or ~4.9 L2
 * cycles.
 *
 * Properties:
 *   - Works per signal (single frequency); no dual-frequency requirement.
 *   - Ionosphere-free to first order, so scintillation does not contaminate it.
 *   - The between-epoch difference does not remove the change in geometric
 *     range and clocks over the interval.  The detector compares |dG| directly
 *     with the threshold and applies no geometry or clock prediction.
 *
 * The detector maintains per-(satellite, signal) arc statistics with a
 * MAD-based adaptive threshold.  When a slip is detected, the integer slip
 * magnitude is estimated by rounding 2*dG/lambda.
 *
 * References:
 *   Yunck (1993), "Coping with the atmosphere and ionosphere in precise
 *   satellite and ground positioning", IAG Symposia.
 *   Banville & Langley (2013), "Mitigating the impact of ionospheric cycle
 *   slips in GNSS observations", J. Geodesy, 87(2):179-193.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/ISlipDetector.hpp"
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Result of GRAPHIC slip detection for a single signal.
     */
    struct GraphicSlipResult {
        std::string satellite;         ///< Satellite identifier (e.g. "G01").
        std::string signal;            ///< Phase signal that slipped (e.g. "L2W").
        std::string amb_key;           ///< Ambiguity key to reset (e.g. "G01_L2W").
        int    slip_cycles{ 0 };       ///< Estimated integer slip magnitude (cycles).
        bool   slip_known{ false };    ///< True if slip_cycles is a reliable integer.
        double fractional{ 0.0 };      ///< Fractional residual from rounding (quality metric).
    };

    /**
     * @class GraphicSlipDetector
     * @brief Detects per-signal cycle slips via between-epoch GRAPHIC differencing.
     *
     * For each signal with matched code+phase on the same band, the detector
     * forms the GRAPHIC combination and monitors its between-epoch change.
     * A MAD-based adaptive threshold flags outliers as slips, and the slip
     * magnitude is estimated by rounding.
     */
    class GraphicSlipDetector : public ISlipDetector {
    public:
        /**
         * @brief Construct a new GraphicSlipDetector.
         * @param config Configuration providing thresholds.
         * @param log_manager Log manager for diagnostic messages.
         */
        GraphicSlipDetector(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Detect GRAPHIC cycle slips for the provided epoch observations.
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @return Vector of per-signal slip results.
         */
        std::vector<GraphicSlipResult> detect(const std::vector<PppObservation>& observations);

        // --- ISlipDetector contract -------------------------------------

        /**
         * @brief Detect slips for one epoch (ISlipDetector entry point).
         *
         * Wraps the native per-signal @c detect overload and projects each
         * @c GraphicSlipResult onto a @c SlipGranularity::PerSignal event.
         *
         * @param context Epoch inputs (only @c observations is consulted).
         * @return Unified per-signal slip events.
         */
        std::vector<SlipEvent> detect(const SlipEpochContext& context) override;

        /// @copydoc ISlipDetector::kind
        SlipDetectorKind kind() const override { return SlipDetectorKind::Graphic; }

        /// @copydoc ISlipDetector::name
        std::string name() const override { return "GRAPHIC"; }

        /**
         * @brief Clear GRAPHIC arc history for a specific satellite.
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite) override;

        /**
         * @brief Enable CSV tracing for GRAPHIC diagnostics.
         *
         * No driver calls this, so the GRAPHIC trace is never opened.  A plain
         * method since the virtual left @c ISlipDetector (card UOM step U5m-a).
         *
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;

        /**
         * @brief Per-(satellite, signal) GRAPHIC arc statistics.
         */
        struct GraphicArcStats {
            double prev_graphic{ 0.0 };      ///< GRAPHIC value at epoch t-1 (meters).
            double last_epoch_s{ 0.0 };      ///< Continuous GPS seconds of last update.
            size_t count{ 0 };               ///< Number of epochs in this arc.
            std::deque<double> diff_window;  ///< Rolling window of dG values for MAD.
        };

        /** Per-(satellite + signal) arc statistics.
         *  Key: "G01_L1C" */
        std::map<std::string, GraphicArcStats> graphic_stats_;

        /** Maximum number of recent dG values for MAD computation. */
        static constexpr size_t kWindowSize = 30;

        /** Minimum epochs before adaptive threshold is used. */
        static constexpr size_t kMinEpochsForAdaptive = 5;

        /** kσ multiplier for the adaptive threshold, from
         *  @c GnssConfig::getCycleSlipConfig().graphic_sigma_multiplier (default 6.0). */
        double sigma_multiplier_;

        /** Minimum slip threshold in meters.
         *  Code noise of ~0.3 m gives GRAPHIC noise of ~0.15 m (0.3/2), and
         *  between-epoch differencing multiplies by sqrt(2), giving ~0.21 m for dG.
         *  The 0.5 m floor corresponds to a slip of ~5.3 L1 cycles (2*0.5/lambda_L1). */
        static constexpr double kMinThresholdM = 0.50;

        /** Maximum fractional cycle residual to accept the integer estimate. */
        static constexpr double kMaxFractional = 0.35;

        /** Maximum inter-epoch gap (seconds) before arc reset. */
        static constexpr double kMaxGapSeconds = 120.0;

        /**
         * @brief Matched code/phase signal pair on the same frequency band.
         */
        struct SignalPair {
            std::string phase;  ///< Phase signal (e.g. "L1C").
            std::string code;   ///< Code signal (e.g. "C1C").
        };

        /**
         * @brief Collect all viable code+phase pairs from an observation.
         */
        std::vector<SignalPair> collectSignalPairs(const PppObservation& obs) const;

        /**
         * @brief Compute GRAPHIC value = (P + lambda*L) / 2 in meters.
         */
        double computeGraphic(const PppObservation& obs,
                              const std::string& phase_sig,
                              const std::string& code_sig) const;

        /**
         * @brief Compute MAD-derived sigma from a diff window.
         */
        static double computeMadSigma(const std::deque<double>& window);

        /**
         * @brief Compute adaptive threshold from arc stats.
         */
        double computeAdaptiveThreshold(const GraphicArcStats& stats) const;

        /** Output stream for GRAPHIC trace CSV file. */
        mutable std::ofstream trace_file_;

        /** Current epoch time for tracing. */
        mutable GnssTime current_epoch_;
    };

} // namespace GnssProcessor
