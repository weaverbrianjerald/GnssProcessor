#pragma once

#include "StateManager.hpp"
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "PppModel.hpp"
#include <MeasurementModel.hpp>
#include <Constants.hpp>
#include <Eigen/Dense>
#include <map>
#include <deque>
#include <string>
#include <vector>

namespace GnssProcessor {

    class KalmanFilter {
    public:
        // Constructor
        KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model);
        void initialize(); // Added to resolve missing member error
        void predict(double dt); // Added to declare predict method
        void update(const EpochMeasurement& measurement, PppState& ppp_state);
        Eigen::VectorXd getStateEstimate() const;

    private:
        // Time update step
        void timeUpdate(const GnssTime& new_time);

        // Cycle-slip detection methods
        bool detectCycleSlip(const EpochMeasurement& measurement, const std::string& sat_id,
            const std::string& type, double innovation, double variance) const;
        bool detectGeometryFreeCycleSlip(const PppObservation& obs, double& gf_residual) const;
        bool detectTimeSeriesCycleSlip(const PppObservation& obs, double gf_residual, double& deviation) const;

        // Robust estimation
        Eigen::VectorXd computeRobustWeights(const Eigen::VectorXd& innovations,
            const Eigen::VectorXd& variances) const;

        // Ambiguity resolution methods
        bool resolveAmbiguities(PppState& ppp_state, Eigen::VectorXd& state_vector,
            Eigen::MatrixXd& covariance);
        bool resolvePartialAmbiguities(PppState& ppp_state, Eigen::VectorXd& state_vector,
            Eigen::MatrixXd& covariance, const Eigen::MatrixXd& H,
            const Eigen::VectorXd& z, const Eigen::VectorXd& R_v);
        void decorrelateAmbiguities(Eigen::MatrixXd& Q, Eigen::MatrixXd& Z, Eigen::VectorXd& a);
        Eigen::VectorXi searchIntegerAmbiguities(const Eigen::VectorXd& float_amb,
            const Eigen::MatrixXd& Q_amb, double& best_ratio);
        bool validateFixedAmbiguities(const Eigen::VectorXd& fixed_amb,
            const std::vector<std::string>& amb_keys,
            const Eigen::MatrixXd& H, const Eigen::VectorXd& z,
            const Eigen::VectorXd& R_v) const;

        // Geometry-free history for cycle-slip detection
        struct GfHistory {
            std::deque<double> gf_values; // GF combination values
            std::deque<GnssTime> timestamps; // Corresponding times
            static constexpr size_t WINDOW_SIZE = 10; // Number of epochs in moving window
            static constexpr double TIME_GAP_THRESHOLD = 30.0; // Max time gap (seconds)
        };

        // Member variables
        StateManager& state_manager_;
        const GnssConfig& config_;
        LogManager& log_manager_; // Logging support
        const PppModel& ppp_model_;
        Eigen::MatrixXd R_; // SRIF information matrix
        Eigen::VectorXd x_; // SRIF state vector
        mutable std::map<std::string, GfHistory> gf_histories_; // Geometry-free history per satellite-frequency pair
        EpochMeasurement last_measurement_;

        // Constants
        static constexpr double HUBER_K = 1.345; // Huber tuning constant
        static constexpr double CYCLE_SLIP_THRESHOLD = 0.5; // Cycle-slip threshold (meters)
        static constexpr double GF_CYCLE_SLIP_THRESHOLD = 0.1; // Geometry-free threshold (meters)
        static constexpr double TS_CYCLE_SLIP_THRESHOLD = 3.0; // Time-series threshold (standard deviations)
        static constexpr double RATIO_TEST_THRESHOLD = 3.0; // Ratio test threshold for ambiguity validation
        static constexpr int MAX_SEARCH_CANDIDATES = 100; // Max candidates in LAMBDA search
        static constexpr double RESIDUAL_THRESHOLD = 0.05; // Residual threshold for validation (meters)
        static constexpr double VARIANCE_THRESHOLD = 0.01; // Variance threshold for partial fixing (m^2)
        static constexpr double MIN_FIX_FRACTION = 0.5; // Minimum fraction of ambiguities to fix
    };

} // namespace GnssProcessor

//#endif // KALMANFILTER_HPP