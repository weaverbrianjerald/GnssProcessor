#pragma once

/**
 * @file EwlSlipDetector.hpp
 * @brief Extra-Wide-Lane (EWL) cycle slip detector for 3+ frequency GNSS.
 *
 * The EWL combination uses two spectrally close frequencies (e.g. L2-L5
 * for GPS, E5a-E5b for Galileo) to form a Melbourne-Wübbena observable
 * with a very long wavelength:
 *
 *   MW_EWL = (f_i·L_i - f_j·L_j)/(f_i - f_j)
 *            - (f_i·P_i + f_j·P_j)/(f_i + f_j)
 *
 * where (f_i, f_j) are two non-L1 frequencies, e.g.:
 *   GPS:     L2 (1227.60 MHz) & L5 (1176.45 MHz)  -> λ_EWL ≈ 5.861 m
 *   Galileo: E5a (1176.45 MHz) & E5b (1207.14 MHz) -> λ_EWL ≈ 9.768 m
 *
 * Consequences of the long EWL wavelength:
 *   - Code noise of ~0.3 m produces only ~0.05 EWL-cycle noise.
 *   - The EWL integer ambiguity can be resolved in a single epoch.
 *   - Integer cycle slips are detected and sized with high confidence.
 *
 * Combined with the standard WL (L1-L2) from MwSlipDetector, the EWL
 * provides a second independent WL constraint:
 *
 *   dN_WL  = dN_1 - dN_2   (from MW L1-L2)
 *   dN_EWL = dN_2 - dN_5   (from MW EWL L2-L5)
 *
 * Two constraints on three unknowns; one further constraint (code-phase
 * divergence, or rounding the L1 ambiguity after the WL+EWL narrowing)
 * would determine dN_1, dN_2 and dN_5 individually.  This detector only
 * detects and sizes the EWL slip.  CycleSlipManager treats its events as
 * band-difference events and resets both ambiguities of the pair; it does
 * not apply an integer repair.
 *
 * Usage:
 *   - Satellites with two or more non-L1 bands carrying code and phase:
 *     EWL slip detection and sizing.
 *   - Dual-frequency (L1 plus one other band): EWL is not available; MW
 *     alone handles WL slip detection, with reset as the fallback.
 *
 * References:
 *   Geng & Bock (2013), "Triple-frequency GPS precise point positioning
 *   with rapid ambiguity resolution", J. Geodesy, 87(5):449-460.
 *
 *   Li et al. (2015), "Three-carrier ambiguity resolution: distance-
 *   independent performance demonstrated using semi-generated triple-
 *   frequency GPS signals", GPS Solutions, 19(2):177-187.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
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
     * @brief Result of EWL slip detection for a single frequency pair.
     */
    struct EwlSlipResult {
        std::string satellite;         ///< Satellite identifier (e.g. "G01").
        std::string sig1;              ///< Higher-freq signal of the EWL pair (e.g. "L2W").
        std::string sig2;              ///< Lower-freq signal of the EWL pair (e.g. "L5Q").
        std::string amb_key;           ///< Ambiguity key (e.g. "G01_L2W").
        int    slip_ewl_cycles{ 0 };   ///< EWL slip ΔN_EWL in cycles, rounded; set even when @c slip_ewl_known is false.
        bool   slip_ewl_known{ false };///< True if slip_ewl_cycles is nonzero with fractional residual < kMaxFractional (0.35).
        double lambda_ewl{ 0.0 };      ///< EWL wavelength (meters) for this pair.
        double fractional{ 0.0 };      ///< Fractional residual from rounding (quality metric).
    };

    /**
     * @class EwlSlipDetector
     * @brief Detects and sizes cycle slips via Extra-Wide-Lane MW combinations.
     *
     * For each satellite with 3+ frequencies, the detector forms EWL MW
     * combinations using non-L1 frequency pairs (e.g. L2-L5, E5a-E5b).
     * The very long EWL wavelength (~5-10 m) makes cycle slips trivially
     * detectable from code-level noise.
     *
     * The detector maintains per-(satellite, pair) arc statistics with a
     * Hatch-filter mean and MAD-based adaptive threshold, following the
     * same architecture as MwSlipDetector.
     */
    class EwlSlipDetector : public ISlipDetector {
    public:
        /**
         * @brief Construct a new EwlSlipDetector.
         * @param config Configuration providing thresholds and processing options.
         * @param log_manager Log manager for diagnostic messages.
         */
        EwlSlipDetector(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Detect EWL cycle slips for the provided epoch observations.
         *
         * Iterates observations above the elevation mask and returns one
         * result per EWL pair on which a slip is flagged; the pair's arc is
         * then reset.  Each result carries the EWL slip rounded to integer
         * cycles and whether that integer is reliable.
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @return std::vector<EwlSlipResult> Slip results for affected pairs.
         */
        std::vector<EwlSlipResult> detect(const std::vector<PppObservation>& observations);

        // --- ISlipDetector contract -------------------------------------

        /**
         * @brief Detect slips for one epoch (ISlipDetector entry point).
         *
         * Wraps the native per-pair @c detect overload and projects each
         * @c EwlSlipResult onto a @c SlipGranularity::BandDifference event
         * (@f$\Delta N_{EWL}=\Delta N_1-\Delta N_2@f$ on the EWL band pair).
         *
         * @param context Epoch inputs (only @c observations is consulted).
         * @return Unified band-difference slip events.
         */
        std::vector<SlipEvent> detect(const SlipEpochContext& context) override;

        /// @copydoc ISlipDetector::kind
        SlipDetectorKind kind() const override { return SlipDetectorKind::ExtraWideLane; }

        /// @copydoc ISlipDetector::name
        std::string name() const override { return "ExtraWideLane"; }

        /**
         * @brief Clear EWL arc history for a specific satellite.
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite) override;

        /**
         * @brief Enable CSV tracing for EWL diagnostics.
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened.
         */
        bool enableTracing(const std::filesystem::path& log_dir) override;

        /**
         * @brief Check if tracing is currently enabled.
         */
        bool isTracingEnabled() const override { return trace_file_.is_open(); }

        /**
         * @brief Provide satellite bias access for EWL bias correction.
         * @param corrections Pointer to GnssCorrections (may be nullptr to disable).
         */
        void setCorrections(const GnssCorrections* corrections) override { corrections_ = corrections; }

        /**
         * @brief Get all currently locked EWL integer ambiguities.
         *
         * Returns a snapshot of every arc that has a locked EWL integer value.
         * Key format: "G01_L2W_L5Q".
         *
         * @return Map of stats_key -> rounded EWL integer.
         */
        std::map<std::string, int> getResolvedEwlAmbiguities() const;

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;
        const GnssCorrections* corrections_{ nullptr };

        /**
         * @brief Per-(satellite, pair) EWL arc statistics.
         *
         * Maintains a Hatch-filtered mean and MAD-based adaptive threshold,
         * analogous to the MW WL detector.
         */
        struct EwlArcStats {
            double mean{ 0.0 };            ///< Running (Hatch) mean of EWL MW values (meters).
            double M2{ 0.0 };              ///< Welford accumulator for variance (m²); not updated by the current implementation.
            size_t count{ 0 };             ///< Number of EWL samples accumulated.
            double prev_ewl{ 0.0 };        ///< Previous epoch EWL value for differencing (meters).
            double last_epoch_s{ 0.0 };    ///< Continuous GPS seconds of last update.
            std::deque<double> diff_window; ///< Rolling window of epoch-to-epoch diffs (meters).
            bool   bia_corrected{ false }; ///< True if satellite BIA bias is removed.
            int    rounded_ewl{ 0 };       ///< Rounded EWL integer once locked.
            bool   ewl_locked{ false };    ///< True once the EWL integer has been locked.
            double lambda_ewl{ 0.0 };      ///< EWL wavelength (meters), cached for the threshold floor.
        };

        /** Per-(satellite + pair) arc statistics.
         *  Key: "G01_L2W_L5Q" */
        std::map<std::string, EwlArcStats> ewl_stats_;

        /** Maximum number of recent values for MAD computation. */
        static constexpr size_t kWindowSize = 30;

        /** Maximum effective count for Hatch mean. */
        static constexpr size_t kMaxHatchCount = 50;

        /** Minimum number of epoch-to-epoch diffs before the adaptive threshold
         *  is defined (no slip is flagged before that), and minimum arc count
         *  for locking the EWL integer without BIA correction. */
        static constexpr size_t kMinEpochsForAdaptive = 3;

        /** kσ multiplier for the adaptive threshold, from
         *  @c GnssConfig::getCycleSlipConfig().ewl_sigma_multiplier (default 5.0).
         *  The EWL is so long-wavelength that the per-epoch noise in EWL cycles
         *  is very small (~0.05); 5.0 gives tight detection while avoiding false
         *  alarms. */
        double sigma_multiplier_;

        /** Minimum slip threshold in EWL cycles.
         *  With ~0.05-cycle noise, a floor of 0.5 cycles catches all
         *  integer slips including ±1 cycle. */
        static constexpr double kMinThresholdCycles = 0.5;

        /** Maximum fractional cycle residual to accept the integer estimate. */
        static constexpr double kMaxFractional = 0.35;

        /** Maximum inter-epoch gap (seconds) before arc reset. */
        static constexpr double kMaxGapSeconds = 120.0;

        /**
         * @brief A non-L1 dual-frequency pair for EWL formation.
         */
        struct EwlPair {
            std::string phase1;  ///< Phase signal on higher freq (e.g. "L2W").
            std::string phase2;  ///< Phase signal on lower freq (e.g. "L5Q").
            std::string code1;   ///< Code signal on higher freq (e.g. "C2W").
            std::string code2;   ///< Code signal on lower freq (e.g. "C5Q").
        };

        /**
         * @brief Collect viable EWL pairs from an observation.
         *
         * Groups code and phase signals by RINEX band number, then forms
         * all non-L1 dual-frequency pairs (band >= 2) that have both code
         * and phase on each band.
         *
         * @param obs Observation to inspect.
         * @return Vector of viable EWL pairs (may be empty for dual-freq).
         */
        std::vector<EwlPair> collectEwlPairs(const PppObservation& obs) const;

        /**
         * @brief Compute the EWL wavelength in meters.
         */
        double computeEwlWavelength(const PppObservation& obs,
                                     const std::string& sig1,
                                     const std::string& sig2) const;

        /**
         * @brief Compute the MW observable for an EWL pair in meters.
         *
         * Same formula as the standard MW but applied to non-L1 frequencies:
         *   MW_EWL = (f1·L1 - f2·L2)/(f1-f2) - (f1·P1 + f2·P2)/(f1+f2)
         */
        double computeEwlMwMeters(const PppObservation& obs,
                                   const EwlPair& pair) const;

        /**
         * @brief Compute the satellite EWL MW bias from BIA entries.
         *
         * @param obs Observation for satellite/system/time lookup.
         * @param pair EWL pair.
         * @param[out] bias_m Computed satellite EWL MW bias in meters.
         * @return True if all BIA entries were found.
         */
        bool computeSatEwlBias(const PppObservation& obs,
                                const EwlPair& pair,
                                double& bias_m) const;

        /**
         * @brief Compute MAD-derived sigma from a diff window.
         */
        static double computeMadSigma(const std::deque<double>& window);

        /**
         * @brief Compute adaptive threshold from arc stats.
         */
        double computeAdaptiveThreshold(const EwlArcStats& stats) const;

        /** Output stream for EWL trace CSV file. */
        mutable std::ofstream trace_file_;

        /** Current epoch time for tracing. */
        mutable GnssTime current_epoch_;
    };

} // namespace GnssProcessor
