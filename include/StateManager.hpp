#pragma once
#ifndef STATEMANAGER_HPP
#define STATEMANAGER_HPP

#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>

namespace GnssProcessor {

    class LogManager; // forward-declare

    class StateManager {
    public:
        // Constructor: Initializes StateManager with core state size (e.g., 5 for PPP, 6 for PPP-RTK)
        StateManager(size_t core_state_size, LogManager& log_manager, const GnssConfig& config);

        // Returns the current state epoch time
        GnssTime getStateEpochTime() const;

        // Sets the state epoch time
        void setStateEpochTime(const GnssTime& time);

        // Sets the state vector
        void setStateVector(const Eigen::VectorXd& state);

        // Sets the covariance matrix
        void setCovariance(const Eigen::MatrixXd& covariance);

        // Returns the total state size (core + ambiguities)
        size_t getStateSize() const;

        // Returns the state vector
        Eigen::VectorXd getStateVector() const;

        // Returns the covariance matrix
        Eigen::MatrixXd getCovariance() const;

        // Returns the process noise matrix for prediction
        Eigen::MatrixXd getProcessNoiseMatrix(double dt) const;

        // Returns the state transition matrix
        Eigen::MatrixXd getTransitionMatrix(double dt) const;

        // Returns the index of the position state (x, y, z)
        size_t getPositionIndex() const;

        // Returns the index of the clock bias state
        size_t getClockIndex() const;

        // Returns the index of the tropospheric zenith delay state
        size_t getTropoIndex() const;

        // Returns the index of the ionospheric zenith delay state (PPP-RTK only)
        size_t getIonoIndex() const;

        // Returns the index of an ambiguity state by key, or -1 if not found
        int getAmbiguityIndex(const std::string& key) const;

        const std::map<std::string, size_t>& getAmbiguities() const;

        // Adds an ambiguity state and returns its index
        size_t addAmbiguity(const std::string& key, double initial_value);

        // NEW: Reset (or create then reset) a float ambiguity state in meters.
        // - Sets value=0
        // - Zeros cross-covariances
        // - Sets diagonal variance = std_m^2
        // Returns true if reset was applied.
        bool resetAmbiguity(const std::string& key, double std_m);

        void updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const;

        //void syncStateVectorToPppState(PppState& ppp_state) const;
        void syncPppStateToStateVector(const PppState& ppp_state);

        // Persistent per-measurement variance scaling (e.g., for code noise inflation)
        void setMeasurementVarianceScale(const std::string& sat, const std::string& signal_type, double scale);
        double getMeasurementVarianceScale(const std::string& sat, const std::string& signal_type) const;
        void clearMeasurementVarianceScales();

        // Convenience: inflate (multiply) the current scale by a factor
        void inflateMeasurementVarianceScale(const std::string& sat, const std::string& signal_type, double factor);

    private:
        size_t core_state_size_;                    // Core state size (e.g., 5 for PPP, 6 for PPP-RTK)
        std::map<std::string, size_t> ambiguity_indices_; // Maps ambiguity keys to state indices
        GnssTime state_epoch_time_;                 // Current state epoch time
        Eigen::VectorXd state_vector_;              // Current state vector
        Eigen::MatrixXd covariance_;                // Current covariance matrix
        LogManager& log_manager_;                   // Logger manager
        const GnssConfig& config_;
        std::map<std::string, double> meas_var_scale_; // key: "G01_C1C" or "G01_C1C_C2W", value: multiplier >= 1.0
    };

} // namespace GnssProcessor

#endif // STATEMANAGER_HPP