#pragma once

#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "TimeUtils.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <optional>

namespace GnssProcessor {

    class PppModel; // Avoid including PppModel.hpp

    class StateManager {
    public:
        // Constructor
        explicit StateManager(const GnssConfig& config);

        // Get state vector size
        int getStateSize() const { return state_vector_.size(); }

        // Access state vector
        Eigen::VectorXd getStateVector() const { return state_vector_; }
        void setStateVector(const Eigen::VectorXd& state);

        // Access covariance matrix
        Eigen::MatrixXd getCovariance() const { return covariance_; }
        void setCovariance(const Eigen::MatrixXd& cov);

        // Get process noise matrix for time step dt
        Eigen::MatrixXd getProcessNoiseMatrix(double dt) const;

        // Manage ambiguities
        std::map<std::string, int> getAmbiguityIndices() const { return ambiguity_indices_; }
        std::optional<int> getAmbiguityIndex(const std::string& key) const;
        void addAmbiguity(const std::string& key, double value);
        void removeAmbiguity(const std::string& key);

        // State parameter indices
        int getPositionIndex() const { return 0; }
        int getClockIndex() const { return 3; }
        int getTropoIndex() const { return 4; }
        int getIonoIndex() const { return 5; }

        // Epoch time management
        GnssTime getStateEpochTime() const { return state_epoch_time_; }
        void setStateEpochTime(const GnssTime& time) { state_epoch_time_ = time; }

    private:
        const GnssConfig& config_; // Configuration reference
        LogManager log_manager_; // Logging support
        Eigen::VectorXd state_vector_; // State vector [x, y, z, clock, tropo, iono, ambiguities]
        Eigen::MatrixXd covariance_; // State covariance matrix
        std::map<std::string, int> ambiguity_indices_; // Ambiguity key -> state index
        int next_index_; // Next available state index for ambiguities
        GnssTime state_epoch_time_; // Current state epoch time
    };

} // namespace GnssProcessor

//#endif // STATEMANAGER_HPP