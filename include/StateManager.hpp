#pragma once
#ifndef STATEMANAGER_HPP
#define STATEMANAGER_HPP

#include "PppTypes.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>

namespace GnssProcessor {

    class StateManager {
    public:
        // Constructor: Initializes StateManager with core state size (e.g., 5 for PPP, 6 for PPP-RTK)
        StateManager(size_t core_state_size);

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

        // Adds an ambiguity state and returns its index
        size_t addAmbiguity(const std::string& key, double initial_value);

    private:
        size_t core_state_size_;                    // Core state size (e.g., 5 for PPP, 6 for PPP-RTK)
        std::map<std::string, size_t> ambiguity_indices_; // Maps ambiguity keys to state indices
        GnssTime state_epoch_time_;                 // Current state epoch time
        Eigen::VectorXd state_vector_;              // Current state vector
        Eigen::MatrixXd covariance_;                // Current covariance matrix
    };

} // namespace GnssProcessor

#endif // STATEMANAGER_HPP
