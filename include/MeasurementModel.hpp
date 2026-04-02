#pragma once
#ifndef MEASUREMENTMODEL_HPP
#define MEASUREMENTMODEL_HPP

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
     * Typical responsibilities:
     *  - Filter observations by elevation, signal priorities and configuration.
     *  - Compute per-observation modeled values and residuals using @c PppModel.
     *  - Assemble innovations, H matrix and measurement covariance for the Kalman filter.
     *  - Provide validation and basic reweighting / persistent variance scaling via @c StateManager.
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
         *
         * This routine is read-only with respect to the Kalman state (it does not modify
         * the @c StateManager). It consumes the provided list of parsed @c PppObservation
         * and the current state vector, queries the @c PppModel for modeled values,
         * and assembles an @c EpochMeasurement containing:
         *  - innovations (observed - modeled)
         *  - design matrix H
         *  - measurement noise vector R_v (per-measurement variance)
         *  - diagnostic fields (satellite ids, signal types, residuals, covariance, etc.)
         *
         * The produced @c EpochMeasurement is intended for immediate validation and use
         * by the Kalman filter update step.
         *
         * @param observations   Vector of parsed and pre-filtered observations for the epoch.
         * @param current_state  Current Kalman state (state vector) used to compute geometry-dependent terms.
         * @return An @c EpochMeasurement populated with innovations, H, R_v and ancillary diagnostics.
         */
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(const std::vector<PppObservation>& observations,
            const Eigen::VectorXd& current_state);

        /**
         * @brief Validate an @c EpochMeasurement before Kalman processing.
         *
         * Performs quality checks on the assembled measurement such as:
         *  - Sufficient number of valid measurements after masking.
         *  - Percentage of bad residuals below configured threshold.
         *  - Residuals within acceptable bounds (using configured residual threshold).
         *
         * If validation fails, callers may skip the update or trigger reweighting/persistent scaling.
         *
         * @param measurement The epoch measurement to validate.
         * @return True if the measurement passes validation and is suitable for a Kalman update; false otherwise.
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

        /**
         * @brief Initialize inter-system biases from first-epoch code observations.
         *
         * Computes approximate ISB by comparing mean code residuals between GPS and
         * other constellations. Only called once at the first epoch.
         */
        void initializeInterSystemBiases(const std::vector<PppObservation>& observations,
                                          const std::string& epoch_prefix);

        /**
         * @brief Seed the phase clock offset from the code-phase residual difference.
         *
         * Computes the mean difference between code and phase iono-free residuals
         * at the first epoch. This difference captures the receiver's code-phase
         * bias (hardware delay mismatch) and is used to constrain the phase clock
         * state so the filter does not have to converge from zero.
         *
         * Only called once at the first epoch when phase clock estimation is enabled.
         */
        void initializePhaseClockOffset(const std::vector<PppObservation>& observations,
                                         const std::string& epoch_prefix);
    };

} // namespace GnssProcessor

#endif // MEASUREMENTMODEL_HPP