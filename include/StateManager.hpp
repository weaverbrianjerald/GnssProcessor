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

        // Sets the previous state vector
        //void setStateVectorPrevious(const Eigen::VectorXd& state);

        // Sets the previous covariance matrix
        //void setCovariancePrevious(const Eigen::MatrixXd& covariance);

        // Returns the total state size (core + ambiguities)
        size_t getStateSize() const;

        // Returns the state vector
        Eigen::VectorXd getStateVector() const;

        // Returns the state vector
        //Eigen::VectorXd getStateVectorPrevious() const;

        // Returns the covariance matrix
        Eigen::MatrixXd getCovariance() const;

        // Returns the previous covariance matrix
        //Eigen::MatrixXd getCovariancePrevious() const;

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

        void updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const;

    private:
        size_t core_state_size_;                    // Core state size (e.g., 5 for PPP, 6 for PPP-RTK)
        std::map<std::string, size_t> ambiguity_indices_; // Maps ambiguity keys to state indices
        GnssTime state_epoch_time_;                 // Current state epoch time
        Eigen::VectorXd state_vector_;              // Current state vector
        Eigen::MatrixXd covariance_;                // Current covariance matrix
        Eigen::VectorXd state_vector_prev_;         // Previous state vector
        Eigen::MatrixXd covariance_prev_;           // Previous covariance matrix
    };

} // namespace GnssProcessor

#endif // STATEMANAGER_HPP