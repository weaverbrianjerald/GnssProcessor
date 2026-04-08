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
     *  - Initialization:  initializeInterSystemBiases(), initializePhaseClockOffset()
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

    private:
        StateManager& state_manager_;    ///< Reference to the state manager (non-owning).
        const PppModel& ppp_model_;      ///< Reference to the PPP modeling component (non-owning).
        const GnssConfig& config_;       ///< Processing configuration (read-only).
        LogManager& log_manager_;        ///< Reference to the logging manager (non-owning).
        PppUtils ppp_utils_;             ///< Utility helpers (frequency/wavelength helpers) initialized with the logger.

        bool isb_initialized_ = false;  ///< Flag to track if ISB has been initialized
        bool phase_clock_initialized_ = false; ///< Flag to track if phase clock offset has been seeded

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
         * @brief Compute a "raw" residual for an observation/ref_type pair.
         *
         * residual = observed - modeled - rx_clk - mf_w*zwd - isb - tropo_grad
         *
         * Used by initializeInterSystemBiases() and initializePhaseClockOffset()
         * to avoid duplicating the residual formula.
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

        // -------------------------------------------------------------------
        // Between-satellite single difference
        // -------------------------------------------------------------------

        /**
         * @brief Apply between-satellite single-difference (BSSD) S-transformation.
         */
        void applyBetweenSatelliteSingleDifference(EpochMeasurement& update,
                                                    const std::string& epoch_prefix);
    };

} // namespace GnssProcessor