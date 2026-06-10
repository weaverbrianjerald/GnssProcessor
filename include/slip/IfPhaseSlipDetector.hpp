#pragma once

/**
* @file IfPhaseSlipDetector.hpp
* @brief Between-satellite, between-epoch ionosphere-free phase slip detector.
*
* @note STATUS: STAGED / INCOMPLETE — not compiled or wired into the pipeline.
*       This detector is intentionally excluded from the build (see
*       CMakeLists.txt) and has no instantiation path in Application. It is
*       kept (not abandoned) for possible future completion. Key limitation:
*       the position-uncertainty gating below only works for a STATIC site
*       with a precise a priori position; it is not yet usable in the general
*       (kinematic / pre-convergence) case. The `enable_if_slip_detector`
*       config toggle is currently inert. For active slip detection see
*       slip/MwSlipDetector and slip/GraphicSlipDetector.
*
* Implements the double-differenced IF phase OMC (observed-minus-computed)
* approach for sensitive cycle slip detection.  The model subtraction is
* essential: it removes the satellite-specific geometric range (~20,000 km)
* and satellite clock, which change by meters per epoch and do NOT cancel
* in between-satellite differencing of raw observables.  After model
* subtraction the OMC contains only receiver clock, ZWD, ISB, ambiguity
* and noise  all of which DO cancel in the double difference.
*
* Algorithm:
*   1. Computes per-satellite IF phase OMC = observed_IF - modeled_IF.
*   2. Selects a high-elevation reference satellite per constellation.
*   3. Forms between-satellite single differences (gradients):
*          grad(sat) = OMC(sat) - OMC(ref)
*      Cancels rx_clk, ISB, phase_clk (common-mode).
*   4. Forms between-epoch double differences:
*          dd(sat) = grad(sat, t) - grad(sat, t-1)
*      Cancels ZWD*mf_w (slowly varying) and constant ambiguity.
*   5. Applies MAD-based adaptive + fixed (IF wavelength) outlier thresholds.
*
* Unlike the MW detector (which is code-noise limited at ~0.3 WL cycles),
* this detector operates on carrier phase precision (~mm) and can reliably
* detect slips of 1-2 IF cycles that the MW and post-fit residual checks miss.
*
* Position-uncertainty gating:
*   The between-satellite single difference does NOT perfectly cancel the
*   receiver position contribution  a position error ?pos produces a
*   differential geometric residual of ~(e_sat - e_ref) · ?pos, which can
*   reach meters during pre-convergence.  To avoid false slip flags the
*   detector accepts an optional 3D position sigma (from the filter
*   covariance) and suppresses outlier flagging when the position is
*   uncertain.  Gradient storage continues so that the detector resumes
*   seamlessly once the position converges.
*
* Ported from stochastic_model_phase_v03.m / compute_if_phase_diff().
*/

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Dense>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @class IfPhaseSlipDetector
     * @brief Detects cycle slips via between-satellite, between-epoch IF phase OMC differences.
     *
     * The detector works per constellation: it computes the IF phase OMC
     * (observed - modeled) to remove the large satellite-specific geometric
     * range and satellite clock, then forms between-satellite single
     * differences (removing common receiver clock, ISB, phase clock), then
     * between-epoch double differences (removing constant ambiguities and
     * slowly-varying ZWD).  A clean double difference should be near zero;
     * a cycle slip appears as a jump of ~N * lambda_IF.
     *
     * When the estimated position is uncertain (pre-convergence), the
     * differential geometric error (e_sat - e_ref) · ?pos can exceed the
     * IF wavelength, causing false slip detections.  The detector therefore
     * accepts a 3D position sigma and gates outlier flagging accordingly.
     */
    class IfPhaseSlipDetector {
    public:
        /**
         * @brief Construct a new IfPhaseSlipDetector.
         * @param config Configuration providing thresholds and processing options.
         * @param log_manager Log manager used to emit diagnostic messages.
         */
        IfPhaseSlipDetector(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Detect IF phase cycle slips for the provided epoch observations.
         *
         * Processes observations grouped by constellation. Returns a vector of
         * ambiguity keys that should be reset (format: "G01_L1C_L2W").
         *
         * The IF phase OMC (observed_IF - modeled_IF) is used. The modeled
         * value removes the large satellite-specific geometric range and
         * satellite clock that change by meters per epoch. After model
         * subtraction, the OMC contains only receiver clock, ZWD*mf_w,
         * ISB, phase clock, ambiguity and noise:
         *   - Between-satellite diff cancels rx_clk, ISB, phase_clk.
         *   - Between-epoch diff cancels ZWD*mf_w and ambiguity.
         * This makes the detector independent of Kalman filter state.
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @param position_sigma_3d Optional 3D position uncertainty (meters).
         *        When > kPositionSigmaThresholdM the detector stores gradients
         *        but suppresses slip flagging to avoid false detections from
         *        differential geometric error during pre-convergence.
         *        Pass 0.0 or omit to always run full detection (backward compatible).
         * @return std::vector<std::string> Vector of ambiguity keys to reset.
         */
        std::vector<std::string> detect(const std::vector<PppObservation>& observations,
            double position_sigma_3d = 0.0,
            const Eigen::Vector3d& receiver_position = Eigen::Vector3d::Zero());

        /**
         * @brief Clear IF phase history for a specific satellite.
         *
         * Call this when an ambiguity is reset by an external mechanism so the
         * detector starts a fresh arc on the next epoch.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite);

        /**
         * @brief Clear all internal state (all constellations).
         *
         * Called after a complete filter reset.
         */
        void resetAll();

        /**
         * @brief IF phase jump information for one satellite from the last detect() call.
         */
        struct IfJumpInfo {
            double delta_grad_m{ 0.0 };  ///< Between-epoch DD IF phase jump (meters).
            double lambda_if_m{ 0.0 };   ///< IF wavelength (meters).
            double elevation_deg{ 0.0 };  ///< Satellite elevation (degrees) at detection epoch.
            bool   valid{ false };        ///< True if a finite jump was computed.
        };

        /**
         * @brief Retrieve the IF phase jump for a satellite from the last detect() call.
         *
         * Returns the between-epoch, between-satellite double-differenced IF
         * phase jump (in meters) that was computed during the most recent
         * call to detect().  This is the quantity denoted delta_grad in the
         * internal processing.  The value is relative to the constellation's
         * reference satellite and is valid only when IfJumpInfo::valid is true.
         *
         * @param satellite  Satellite identifier (e.g. "G01").
         * @return IfJumpInfo  Phase jump info (valid=false if not available).
         */
        IfJumpInfo getLastIfJump(const std::string& satellite) const;

        /**
         * @brief Enable CSV tracing for IF phase slip diagnostics.
         *
         * Opens or creates "if_slip_trace.csv" in the specified directory.
         *
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened, false otherwise.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

        /**
         * @brief Check if tracing is currently enabled.
         * @return true if the trace file is open and writable.
         */
        bool isTracingEnabled() const { return trace_file_.is_open(); }

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;

        /** Minimum elevation (degrees) for a satellite to be chosen as reference. */
        static constexpr double kRefElevThreshDeg = 45.0;

        /** Number of MAD-sigmas for the adaptive outlier threshold. */
        static constexpr double kOutlierSigmaMultiplier = 8.0;

        /** Maximum number of iterative outlier removal passes. */
        static constexpr size_t kMaxOutlierPasses = 10;

        /**
         * @brief 3D position sigma threshold (meters) for gating slip detection.
         *
         * When the caller-provided position_sigma_3d exceeds this value, the
         * detector still computes and stores between-satellite gradients (so
         * that between-epoch differences are available once the position
         * converges) but suppresses outlier flagging.
         *
         * Rationale: a satellite pair separated by ~60° in the sky and a
         * position error of ?pos produces a differential OMC of roughly
         * |?pos| * sin(60°) ? 0.87 * |?pos|.  For GPS L1/L2 the IF
         * wavelength is ~10.7 cm, so even a 0.5 m position uncertainty
         * can produce ~0.43 m of differential error  several IF cycles.
         * A threshold of 0.5 m provides a conservative margin.
         */
        static constexpr double kPositionSigmaThresholdM = 0.5;

        /**
         * @brief Per-constellation persistent state across epochs.
         */
        struct ConstellationState {
            /** Previous epoch between-satellite IF OMC gradients keyed by satellite ID. */
            std::map<std::string, double> prev_grad;

            /** Previous epoch LOS unit vectors keyed by satellite ID (for geometry correction). */
            std::map<std::string, Eigen::Vector3d> prev_los;

            /** Receiver position at the previous epoch (ECEF meters). */
            Eigen::Vector3d prev_position{ Eigen::Vector3d::Zero() };

            /** True if prev_position has been set at least once. */
            bool has_prev_position{ false };

            /** Current reference satellite ID for this constellation. */
            std::string ref_satellite;

            /** Current reference satellite elevation (degrees). */
            double ref_elevation{ 0.0 };
        };

        /** Per-constellation state keyed by system string (e.g. "GPS"). */
        std::map<std::string, ConstellationState> constellation_state_;

        /** Cached per-satellite IF jump from the most recent detect() call.
         *  Populated at the end of detect() so that getLastIfJump() can
         *  return values without re-computing. */
        std::map<std::string, IfJumpInfo> last_if_jumps_;

        /** @brief Per-satellite data collected during observation grouping. */
        struct SatData {
            std::string satellite;
            std::string phase_if_key;
            std::string sig1, sig2;
            double phi_if_omc;
            double elevation;
            double lambda_if;
            System system;
            Eigen::Vector3d los{ Eigen::Vector3d::Zero() }; ///< LOS unit vector for geometry correction.
        };

        bool findPhaseIfLabel(const PppObservation& obs, std::string& phase_if_key) const;
        bool splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const;
        double computeIfWavelengthMeters(const PppObservation& obs,
            const std::string& sig1, const std::string& sig2) const;

        std::map<std::string, double> checkOutliers(
            const std::map<std::string, double>& values,
            const std::string& ref_satellite,
            double lambda_if) const;

        /** @brief Compute median of a vector (sorts in-place). */
        static double computeMedian(std::vector<double>& values);

        /**
         * @brief Group observations by constellation and extract IF phase OMC data.
         * @param observations Epoch observations.
         * @return Map of system string -> vector of SatData.
         */
        std::map<std::string, std::vector<SatData>> groupByConstellation(
            const std::vector<PppObservation>& observations) const;

        /**
         * @brief Select or validate the reference satellite for a constellation.
         * @param sat_vec  Satellite data for this constellation.
         * @param cs       Persistent constellation state (ref is updated in-place).
         * @return True if the reference changed this epoch.
         */
        bool selectReference(const std::vector<SatData>& sat_vec, ConstellationState& cs) const;

        /**
         * @brief Process double differences for one constellation and flag slips.
         *
         * Handles reference-change correction, reference-slip check, and
         * non-reference outlier detection. Appends flagged keys to reset_keys.
         *
         * @param sat_vec         Satellite data for this constellation.
         * @param sys_str         System string (for logging).
         * @param cs              Persistent constellation state.
         * @param grad_phi_if     Current-epoch between-satellite gradients.
         * @param lambda_if       Median IF wavelength for this constellation.
         * @param ref_changed     Whether the reference satellite changed.
         * @param position_sigma_3d 3D position sigma (meters).
         * @param[out] reset_keys Ambiguity keys to reset are appended here.
         */
        std::map<std::string, double> processDoubleDifferences(
            const std::vector<SatData>& sat_vec,
            const std::string& sys_str,
            ConstellationState& cs,
            const std::map<std::string, double>& grad_phi_if,
            double lambda_if,
            bool ref_changed,
            double position_sigma_3d,
            const Eigen::Vector3d& delta_pos,
            std::vector<std::string>& reset_keys);

        /**
         * @brief Write a single trace CSV record.
         */
        void writeTraceRecord(const SatData& sd, const std::string& sys_str,
            double grad, double delta_grad,
            const std::string& ref_sat, bool ref_changed,
            double lambda_if, bool is_slip,
            const std::string& slip_reason,
            double position_sigma_3d, bool pos_gated) const;

        /** Output stream for IF slip trace CSV file. */
        mutable std::ofstream trace_file_;

        /** Current epoch time for tracing. */
        mutable GnssTime current_epoch_;
    };

} // namespace GnssProcessor
