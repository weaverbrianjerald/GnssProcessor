#pragma once


#include "StateManager.hpp"
#include "PppModel.hpp"
#include "GnssConfig.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include <Eigen/Dense>
#include <vector>
#include <string>
#include <set>

namespace GnssProcessor {

    /**
     * @file MeasurementModel.hpp
     * @brief Builds measurement vectors, design matrices and covariance used by the Kalman filter.
     *
     * The MeasurementModel is responsible for converting a set of PPP observations
     * (code and phase) together with the current PPP state into an @c EpochMeasurement
     * suitable for Kalman filter processing. It queries the high-level @c PppModel for
     * corrected modeled values and diagnostics, applies observation selection and masking,
     * assembles the innovation vector, design matrix H and measurement noise R, and provides
     * validation helpers for outlier and quality checks.
     *
     * The implementation is decomposed into focused helpers:
     *  - State pre-pass:  ensureRequiredStates()
     *  - Filtering:       isUsableMeasurement()
     *  - Jacobian:        buildJacobianRow()
 *  - Initialization:  initializeInterSystemBiases(), initializePhaseClockOffset(), initializeCodeBiases()
     */
    class MeasurementModel {
    public:
        /**
         * @brief Construct a MeasurementModel.
         *
         * The model stores references to a live @c StateManager and @c LogManager,
         * and a const reference to the configured @c PppModel and @c GnssConfig.
         * The constructor should initialize any helpers (e.g. @c PppUtils) using the
         * provided @c LogManager.
         *
         * @param state_manager Reference to the Kalman filter state manager (owned externally).
         * @param ppp_model     Const reference to the PPP model that computes corrections and modeled observables.
         * @param config        Const reference to processing configuration.
         * @param log_manager   Reference to the logging manager used for diagnostics.
         */
        MeasurementModel(StateManager& state_manager,
            const PppModel& ppp_model,
            const GnssConfig& config,
            LogManager& log_manager);

        /**
         * @brief Compute the measurement update for a single epoch.
         */
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(const std::vector<PppObservation>& observations,
            const Eigen::VectorXd& current_state,
            const std::set<std::string>& excluded_measurements = {});

        /**
         * @brief Validate an @c EpochMeasurement before Kalman processing.
         */
        bool validateMeasurementUpdate(const EpochMeasurement& measurement) const;

        /**
         * @brief Set the body-to-ECEF attitude matrix for tightly-coupled Jacobians.
         *
         * In tightly-coupled GNSS/INS mode the measurement Jacobian includes
         * attitude-error and accelerometer-bias partials for Doppler rows.
         * Call this once per epoch (after IMU mechanization) before
         * computeMeasurementUpdate().
         *
         * @param C_b_e Current body-to-ECEF rotation matrix.
         * @param f_b_corrected Bias-corrected specific force in body frame (m/s²).
         */
        void setImuState(const Eigen::Matrix3d& C_b_e,
                         const Eigen::Vector3d& f_b_corrected);

        /**
         * @brief Get the current BSSD pivot satellite for each constellation.
         *
         * Returns an empty map when BSSD is not active or before the first
         * epoch has been processed.
         *
         * @return Map from constellation name (e.g. "GPS") to pivot satellite
         *         identifier (e.g. "G01").
         */
        const std::map<std::string, std::string>& getBssdPivotSatellites() const {
            return bssd_pivot_sat_;
        }

    private:
        StateManager& state_manager_;    ///< Reference to the state manager (non-owning).
        const PppModel& ppp_model_;      ///< Reference to the PPP modeling component (non-owning).
        const GnssConfig& config_;       ///< Processing configuration (read-only).
        LogManager& log_manager_;        ///< Reference to the logging manager (non-owning).
        PppUtils ppp_utils_;             ///< Utility helpers (frequency/wavelength helpers) initialized with the logger.

        bool isb_initialized_ = false;  ///< Flag to track if ISB has been initialized
        bool phase_clock_initialized_ = false; ///< Flag to track if phase clock offset has been seeded
        bool code_bias_initialized_ = false; ///< Flag to track if DCB/IFB states have been seeded

        /// Tracked BSSD pivot satellite per constellation (persists across epochs).
        /// Used to detect pivot handovers and re-reference SD ambiguity states.
        std::map<std::string, std::string> bssd_pivot_sat_;

        // Tightly-coupled IMU state (set per-epoch via setImuState())
        Eigen::Matrix3d tc_C_b_e_ = Eigen::Matrix3d::Identity(); ///< Body-to-ECEF rotation.
        Eigen::Vector3d tc_f_b_ = Eigen::Vector3d::Zero();       ///< Bias-corrected specific force (body frame).

        // -------------------------------------------------------------------
        // Utility helpers
        // -------------------------------------------------------------------

        /**
         * @brief Map a System enum to its constellation name for ISB lookup.
         *
         * Returns an empty string for GPS (reference system) and "Unknown"
         * for unrecognized values.
         */
        static std::string systemToIsbName(System system);

        /**
         * @brief Map a System enum to the single-char system identifier used by the BIA clock reference map.
         */
        static char systemToClockRefChar(System system);

        /**
         * @brief Resolved receiver-bias state for one observation signal.
         *
         * Returned by resolveReceiverBias() to tell the Jacobian builder
         * which code-bias state key to use and with what coefficient.
         */
        struct ReceiverBiasEntry {
            std::string state_key;   /**< Key into code_bias_indices_ (e.g. "G_DCB" or "G_IFB5"). */
            double      coefficient; /**< Jacobian coefficient for the state.  NaN ? no bias modelled. */
        };

        /**
         * @brief Determine the receiver-bias state and coefficient for a signal.
         *
         * For signals on one of the two clock-reference bands the method
         * returns the per-system DCB state with the GF-IF coefficient ?_j.
         * For signals on any other band it returns a per-system-per-band IFB
         * state with coefficient 1.0 (the total receiver delay for that band
         * relative to the IF clock is an independent free parameter).
         *
         * @param obs_signal  Signal key (e.g. "C1W", "L5Q").
         * @param system      GNSS system enum.
         * @param satellite   Satellite id (for GLONASS frequency slot lookup).
         * @return ReceiverBiasEntry with state_key and coefficient.
         *         coefficient is NaN when no bias can be modelled.
         */
        ReceiverBiasEntry resolveReceiverBias(const std::string& obs_signal,
                                               System system,
                                               const std::string& satellite) const;

        /**
         * @brief Check whether both component signals of an IF combination
         *        have BIA product coverage.
         *
         * For a key like "C1W_C5Q" this checks hasBiasEntry for both
         * "C1W" and "C5Q".  Returns true for non-IF keys (no underscore)
         * and when both components have coverage.
         *
         * @param satellite Satellite identifier.
         * @param if_key    IF combination key (e.g. "C1W_C5Q").
         * @return True if both component signals have BIA coverage.
         */
        bool ifCombHasFullBia(const std::string& satellite,
                              const std::string& if_key) const;

        /**
         * @brief Check whether both component signals of an IF combination
         *        have receiver antenna ATX coverage (PCO/PCV).
         *
         * For a key like "L1W_L5Q" this checks hasReceiverAtxEntry for
         * both "L1W" and "L5Q".  Returns true for non-IF keys.
         *
         * @param if_key  IF combination key (e.g. "L1W_L5Q").
         * @param system  GNSS system.
         * @return True if both component signals have receiver ATX coverage.
         */
        bool ifCombHasFullRxAtx(const std::string& if_key, System system) const;

        /**
         * @brief Compute a "raw" residual for an observation/ref_type pair.
         *
         * residual = observed - modeled - rx_clk - mf_w*zwd - isb - tropo_grad
         *          - iono - code_bias
         *
         * In uncombined mode the per-satellite ionosphere and receiver
         * code-bias (DCB/IFB) contributions are subtracted so that the
         * returned residual is free of all currently-estimated states.
         *
         * Used by initializeInterSystemBiases(), initializePhaseClockOffset(),
         * and initializeCodeBiases() to avoid duplicating the residual formula.
         */
        double computeRawResidual(const PppObservation& obs,
            const std::string& ref_type,
            const Eigen::VectorXd& x) const;

        // -------------------------------------------------------------------
        // State pre-pass
        // -------------------------------------------------------------------

        /**
         * @brief Ensure that all ambiguity, ionosphere, and code-bias states
         *        required by the current epoch exist in the StateManager.
         *
         * Also prunes stale states that are no longer needed.
         *
         * @param observations Epoch observations.
         * @param epoch_prefix Log prefix string.
         */
        void ensureRequiredStates(const std::vector<PppObservation>& observations,
            const std::string& epoch_prefix);

        /**
         * @brief Compute ionosphere seed from dual-frequency code pseudoranges.
         *
         * Uses the geometry-free code combination P1-P2 to estimate the slant
         * ionospheric delay at the ionosphere state reference frequency
         * (GPS L1).  Follows cssrlib: I = (P1 - P2) / (1 - (f1/f2)^2).
         *
         * @param obs  Observation record with code measurements in meters.
         * @return Estimated slant iono delay in meters at the reference
         *         frequency, or 0.0 if dual-frequency codes are unavailable.
         */
        double computeIonosphereSeed(const PppObservation& obs) const;

        // -------------------------------------------------------------------
        // Measurement filtering
        // -------------------------------------------------------------------

        /**
         * @brief Determine whether a given observation/ref_type pair is usable.
         *
         * Checks signal type (C/L), combination mode (iono-free vs uncombined),
         * availability of modeled data, and exclusion list.
         */
        bool isUsableMeasurement(const PppObservation& obs,
            const std::string& ref_type,
            const std::set<std::string>& excluded,
            const std::string& epoch_prefix) const;

        // -------------------------------------------------------------------
        // Jacobian + innovation assembly
        // -------------------------------------------------------------------

        /**
         * @brief Build one Jacobian row and compute the predicted observable.
         *
         * Fills in the position, clock, phase-clock, tropo, tropo-gradient,
         * ISB, ionosphere, code-bias, and ambiguity entries for a single
         * measurement row.
         *
         * @param obs          Observation record.
         * @param ref_type     Signal key (e.g. "C1W_C2W" or "L1C").
         * @param x            Current state vector.
         * @param state_size   Total state size.
         * @param[out] jacobian  Populated Jacobian row (sized to state_size).
         * @param[out] predicted Predicted observable value at the current state.
         * @return true if the row was successfully built; false to skip this measurement.
         */
        bool buildJacobianRow(const PppObservation& obs,
            const std::string& ref_type,
            const Eigen::VectorXd& x,
            size_t state_size,
            Eigen::VectorXd& jacobian,
            double& predicted) const;

        // -------------------------------------------------------------------
        // First-epoch initializers
        // -------------------------------------------------------------------

        /**
         * @brief Initialize inter-system biases from first-epoch code observations.
         */
        void initializeInterSystemBiases(const std::vector<PppObservation>& observations,
                                          const std::string& epoch_prefix);

        /**
         * @brief Seed the phase clock offset from the code-phase residual difference.
         */
        void initializePhaseClockOffset(const std::vector<PppObservation>& observations,
                                         const std::string& epoch_prefix);

        /**
         * @brief Seed DCB/IFB states from cross-band code residual differences.
         *
         * Uses the first reference band as an anchor and computes the median
         * bias estimate across satellites for each DCB/IFB state.  Must be
         * called after ensureRequiredStates() has created the code-bias
         * states and after the ionosphere states have been seeded.
         */
        void initializeCodeBiases(const std::vector<PppObservation>& observations,
                                   const std::string& epoch_prefix);

        // -------------------------------------------------------------------
        // Between-satellite single difference
        // -------------------------------------------------------------------

        /**
         * @brief Detect and handle BSSD pivot satellite handovers.
         *
         * Uses sticky pivot selection with elevation hysteresis: the current
         * pivot is retained while it remains above bssd_pivot_min_elevation.
         *
         * When the pivot's ambiguity has been disturbed (cycle slip or outlier
         * reset), the handler selects the best healthy (undisturbed) satellite
         * as a replacement and applies a lossless T P T' re-referencing to
         * preserve all converged filter information.  Only the disturbed
         * pivot's own ambiguity is affected.  If no healthy replacement
         * exists, the pivot is re-zeroed in place as a last resort.
         *
         * Must be called **before** innovations are computed so the corrected
         * state vector flows into the measurement equations.
         */
        void handleBssdPivotHandover(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix);

        /**
         * @brief Apply between-satellite single-difference (BSSD) S-transformation.
         */
        void applyBetweenSatelliteSingleDifference(EpochMeasurement& update,
                                                    const std::string& epoch_prefix);

        /**
         * @brief Freeze pivot ambiguity covariances to zero.
         *
         * The BSSD pivot's H-column is zeroed in the S-transformation, so
         * the pivot receives no measurement update and its variance can never
         * converge.  This helper explicitly sets its covariance row/col to
         * zero after every epoch so that the disturbance detector does not
         * confuse the never-converging initial variance with a cycle-slip
         * reset.
         */
        void freezePivotAmbiguityCovariance();

        /**
         * @brief Freeze pivot ionosphere covariances to zero.
         *
         * Under BSSD the pivot satellite's ionosphere column is zeroed in
         * the S-transformation (absorbed into the eliminated clock).
         * Without freezing, the ionosphere variance grows without bound
         * via process noise, degrading convergence through stale
         * cross-covariances.
         */
        void freezePivotIonosphereCovariance();
    };

} // namespace GnssProcessor