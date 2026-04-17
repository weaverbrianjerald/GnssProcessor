#pragma once




#pragma once

#include "StateManager.hpp"
#include "PppModel.hpp"
#include "GnssConfig.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include "BssdPivotManager.hpp"
#include "StateInitializer.hpp"
#include "BssdTransform.hpp"
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
     * Heavy-lifting sub-tasks are delegated to focused components:
     *  - StateInitializer:  first-epoch ISB, phase clock, and code bias seeding.
     *  - BssdPivotManager:  pivot selection, handover, and state re-referencing.
     *  - BssdTransform:     between-satellite single-difference S-transformation.
     */
    class MeasurementModel {
    public:
        /**
         * @brief Construct a MeasurementModel.
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
         * @param C_b_e Current body-to-ECEF rotation matrix.
         * @param f_b_corrected Bias-corrected specific force in body frame (m/s²).
         */
        void setImuState(const Eigen::Matrix3d& C_b_e,
                         const Eigen::Vector3d& f_b_corrected);

        /**
         * @brief Get the current BSSD pivot satellite for each constellation.
         */
        const std::map<std::string, std::string>& getBssdPivotSatellites() const {
            return pivot_manager_.getPivotSatellites();
        }

        /// Access the pivot manager (for external components that need pivot info).
        BssdPivotManager& getPivotManager() { return pivot_manager_; }
        const BssdPivotManager& getPivotManager() const { return pivot_manager_; }

        /// Provide the MW detector to the pivot manager for quality-based scoring.
        void setMwDetector(const MwSlipDetector* mw) { pivot_manager_.setMwDetector(mw); }

        /// Access the state initializer (for shared helpers like resolveReceiverBias).
        StateInitializer& getStateInitializer() { return state_initializer_; }
        const StateInitializer& getStateInitializer() const { return state_initializer_; }

    private:
        // -------------------------------------------------------------------
        // Core dependencies (non-owning references)
        // -------------------------------------------------------------------

        StateManager& state_manager_;
        const PppModel& ppp_model_;
        const GnssConfig& config_;
        LogManager& log_manager_;

        // -------------------------------------------------------------------
        // Delegate components
        // -------------------------------------------------------------------

        /// First-epoch state seeding (ISB, phase clock, DCB/IFB).
        StateInitializer state_initializer_;

        /// BSSD pivot satellite manager (selection, handover, re-referencing).
        BssdPivotManager pivot_manager_;

        /// BSSD S-transformation (differencing operator).
        BssdTransform bssd_transform_;

        // -------------------------------------------------------------------
        // Per-instance helpers
        // -------------------------------------------------------------------

        PppUtils ppp_utils_;  ///< Frequency/wavelength helpers.

        /// Epoch at which the BSSD pivot was last selected.  Used to
        /// prevent re-running pivot handover during measurement rebuilds
        /// within the same epoch (outlier-triggered rebuilds must reuse
        /// the original pivot to avoid cascading SD-ambiguity invalidation).
        GnssTime last_pivot_epoch_{};

        /// Per-constellation set of frequency bands covered by the BSSD pivot.
        /// Populated at the start of each computeMeasurementUpdate() call.
        /// When BSSD is active, measurements on bands not covered by the
        /// pivot are excluded (they would pass through as undifferenced
        /// singletons with zeroed clock columns, corrupting the solution).
        /// Key: constellation (e.g. "GPS"), Value: set of bands (e.g. {"L1","L2","C1","C2"}).
        mutable std::map<std::string, std::set<std::string>> bssd_pivot_bands_;

        // Tightly-coupled IMU state (set per-epoch via setImuState())
        Eigen::Matrix3d tc_C_b_e_ = Eigen::Matrix3d::Identity();
        Eigen::Vector3d tc_f_b_ = Eigen::Vector3d::Zero();

        // -------------------------------------------------------------------
        // Product coverage checks
        // -------------------------------------------------------------------

        /**
         * @brief Check whether both component signals of an IF combination
         *        have BIA product coverage.
         */
        bool ifCombHasFullBia(const std::string& satellite,
                              const std::string& if_key) const;

        /**
         * @brief Check whether both component signals of an IF combination
         *        have receiver antenna ATX coverage (PCO/PCV).
         */
        bool ifCombHasFullRxAtx(const std::string& if_key, System system) const;

        // -------------------------------------------------------------------
        // State pre-pass
        // -------------------------------------------------------------------

        /**
         * @brief Ensure that all ambiguity, ionosphere, and code-bias states
         *        required by the current epoch exist in the StateManager.
         *
         * Also prunes stale states that are no longer needed.
         */
        void ensureRequiredStates(const std::vector<PppObservation>& observations,
            const std::string& epoch_prefix);

        // -------------------------------------------------------------------
        // Measurement filtering
        // -------------------------------------------------------------------

        /**
         * @brief Determine whether a given observation/ref_type pair is usable.
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
         */
        bool buildJacobianRow(const PppObservation& obs,
            const std::string& ref_type,
            const Eigen::VectorXd& x,
            size_t state_size,
            Eigen::VectorXd& jacobian,
            double& predicted) const;

        // -------------------------------------------------------------------
        // BSSD delegation
        // -------------------------------------------------------------------

        void handleBssdPivotHandover(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix);

        void freezePivotAmbiguityCovariance();

        void freezePivotIonoAndBiasCovariance();
    };

} // namespace GnssProcessor