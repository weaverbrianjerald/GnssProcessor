#pragma once
#ifndef STATEMANAGER_HPP
#define STATEMANAGER_HPP

/**
 * @file StateManager.hpp
 * @brief Manages the Kalman filter state vector, covariance and indexed states.
 *
 * The StateManager provides accessors and mutators for the filter state
 * vector, covariance, process noise / transition helpers and ambiguity
 * bookkeeping. It also maintains per-measurement variance scaling used for
 * persistent reweighting of observations (e.g. code noise inflation).
 *
 * The class is intentionally lightweight in the header; implementations live
 * in the corresponding source file. Doxygen comments describe the contract
 * and side-effects of each public API.
 */

#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <set>

namespace GnssProcessor {

    class LogManager; // forward-declare

    /**
     * @brief Container and accessor for Kalman filter states and related utilities.
     *
     * StateManager owns the state epoch time, state vector and covariance
     * matrix. It also maps ambiguity keys to indices within the state vector
     * and maintains measurement variance scales for persistent reweighting.
     *
     * The class exposes helpers used by the Kalman filter implementation:
     * transition and process-noise matrix providers, index getters for core
     * states (position, clock, troposphere, ionosphere) and ambiguity
     * management routines.
     */
    class StateManager {
    public:
        /**
         * @brief Construct a StateManager.
         *
         * @param core_state_size Number of core states (e.g. 5 for PPP, 6 for PPP-RTK).
         * @param log_manager Reference to a LogManager for diagnostic logging.
         * @param config Read-only reference to GnssConfig containing runtime parameters.
         */
        StateManager(size_t core_state_size, LogManager& log_manager, const GnssConfig& config);

        /**
         * @brief Get the epoch time associated with the current state.
         * @return Current state epoch as a GnssTime.
         */
        GnssTime getStateEpochTime() const;

        /**
         * @brief Set the epoch time for the current state.
         * @param time Epoch time to assign to the state.
         */
        void setStateEpochTime(const GnssTime& time);

        /**
         * @brief Replace the internal state vector.
         * @param state New state vector. Size must match current state size.
         */
        void setStateVector(const Eigen::VectorXd& state);

        /**
         * @brief Replace the internal covariance matrix.
         * @param covariance New covariance matrix. Must be square with dimension equal to state size.
         */
        void setCovariance(const Eigen::MatrixXd& covariance);

        /**
         * @brief Get the total number of states (core + ambiguity states).
         * @return Total state size.
         */
        size_t getStateSize() const;

        /**
         * @brief Get a copy of the current state vector.
         * @return State vector as Eigen::VectorXd.
         */
        Eigen::VectorXd getStateVector() const;

        /**
         * @brief Get a copy of the current covariance matrix.
         * @return Covariance matrix as Eigen::MatrixXd.
         */
        Eigen::MatrixXd getCovariance() const;

        /**
         * @brief Build or retrieve the process noise matrix for prediction.
         *
         * Implementations typically use configuration parameters and the time
         * step \p dt to produce a state-dimensioned process noise matrix Q.
         *
         * @param dt Time interval in seconds for which Q is requested.
         * @return Process noise matrix Q with dimensions (state_size x state_size).
         */
        Eigen::MatrixXd getProcessNoiseMatrix(double dt) const;

        /**
         * @brief Get the discrete-time state transition matrix for interval dt.
         *
         * The returned matrix F should satisfy x_k+1 = F * x_k (prior to adding process noise).
         *
         * @param dt Time interval in seconds.
         * @return Transition matrix F (state_size x state_size).
         */
        Eigen::MatrixXd getTransitionMatrix(double dt) const;

        /**
         * @brief Index of the position state within the state vector.
         *
         * Position is expected to occupy three consecutive elements (x,y,z).
         * @return Index of the first position element.
         */
        size_t getPositionIndex() const;

        /**
         * @brief Index of the receiver clock bias state within the state vector.
         * @return Index of the clock bias state.
         */
        size_t getClockIndex() const;

        /**
         * @brief Index of the tropospheric zenith wet delay (ZWD) state.
         * @return Index of the troposphere state.
         */
        size_t getTropoIndex() const;

        /**
         * @brief Index of the ionospheric zenith delay state (PPP-RTK only).
         * @return Index of the ionosphere state, or an index >= state size if not present.
         */
        size_t getIonoIndex() const;

        /**
         * @brief Index of the tropospheric north gradient state.
         * @return Index of Gn state, or std::numeric_limits<size_t>::max() if gradients disabled.
         */
        size_t getTropoGradientNorthIndex() const;

        /**
         * @brief Index of the tropospheric east gradient state.
         * @return Index of Ge state, or std::numeric_limits<size_t>::max() if gradients disabled.
         */
        size_t getTropoGradientEastIndex() const;

        /**
         * @brief Check if troposphere gradient estimation is active.
         * @return True if gradient states exist in the state vector.
         */
        bool hasTropoGradients() const;

        /**
         * @brief Get the index of the inter-system time bias for a given system.
         *
         * Inter-system time biases are estimated relative to the reference system (GPS).
         * This returns the state index for the specified system's time offset.
         *
         * @param system_name Name of the GNSS system (e.g., "Galileo", "GLONASS", "BeiDou").
         * @return Index of the system time bias state, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getSystemTimeOffsetIndex(const std::string& system_name) const;

        /**
         * @brief Get the number of inter-system time bias states.
         *
         * This is equal to the number of active GNSS systems minus one (GPS is the reference).
         *
         * @return Number of inter-system time bias states.
         */
        size_t getNumSystemTimeOffsets() const;

        /**
         * @brief Get the list of systems that have time offset states.
         *
         * Returns the system names (excluding GPS, which is the reference) that have
         * inter-system time bias states in the filter.
         *
         * @return Vector of system names with time offset states.
         */
        const std::vector<std::string>& getSystemsWithTimeOffsets() const;

        /**
         * @brief Lookup the ambiguity state index by key.
         *
         * Keys typically combine satellite identifier and signal (e.g. "G01_L1").
         *
         * @param key Ambiguity key.
         * @return Zero-based index of the ambiguity state, or -1 if not present.
         */
        int getAmbiguityIndex(const std::string& key) const;

        /**
         * @brief Get reference to the internal ambiguity map.
         * @return Map from ambiguity key to state index.
         */
        const std::map<std::string, size_t>& getAmbiguities() const;

        /**
         * @brief Add a new ambiguity state and return its index.
         *
         * The ambiguity is appended to the state vector and covariance.
         *
         * @param key Unique ambiguity key.
         * @param initial_value Initial state value in meters.
         * @return Index assigned to the newly-created ambiguity state.
         */
        size_t addAmbiguity(const std::string& key, double initial_value);

        /**
         * @brief Reset (or create then reset) a float ambiguity state.
         *
         * Behaviour:
         *  - If the ambiguity key exists, the state's value is set to zero.
         *  - Cross-covariances for that ambiguity row/column are zeroed.
         *  - The ambiguity diagonal variance is set to (std_m)^2.
         *  - If the ambiguity key does not exist, a new ambiguity state is created and initialized accordingly.
         *
         * This method is intended for ambiguity reinitialization after a detected phase outlier.
         *
         * @param key Ambiguity key.
         * @param std_m Standard deviation in meters to set on the ambiguity diagonal.
         * @return true if a reset (or creation+reset) was applied, false otherwise.
         */
        bool resetAmbiguity(const std::string& key, double std_m);

        /**
         * @brief Compute an approximate ambiguity seed value from an observation.
         *
         * The seed is computed as:
         *   seed = observed - modeled - rx_clock - (mf_w * zwd) - isb - tropo_grad
         *
         * where rx_clock, zwd, inter-system time offset (isb) and tropospheric
         * gradient contribution are read from the supplied state vector at the
         * standard core-state indices.  This centralizes the formula used when
         * creating new ambiguities and when re-seeding ambiguities that were
         * reset to zero (e.g. after cycle-slip or outlier detection).
         *
         * @param obs        The PppObservation containing observed/modeled values and wet mapping factor.
         * @param ref_type   The iono-free key into obs.observed / obs.modeled (e.g. "L1C_L2W").
         * @param x          State vector from which rx clock, ZWD, ISB and gradients are read.
         * @return Approximate ambiguity value in meters, or 0.0 on failure.
         */
        double computeAmbiguitySeed(const PppObservation& obs,
                                    const std::string& ref_type,
                                    const Eigen::VectorXd& x) const;

        /**
         * @brief Update a PppState object from a raw state-vector representation.
         *
         * Copies relevant core states and ambiguities into the provided \p ppp_state.
         * This function is a lightweight translator used to expose the internal
         * state to higher-level models.
         *
         * @param current_state State vector to interpret.
         * @param ppp_state Output PppState that will receive values and covariance.
         * @param state_manager Reference to the manager (kept for compatibility with callers).
         */
        void updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const;

        /**
         * @brief Synchronize values from a PppState into the internal state vector.
         *
         * Writes core-state values (position, clock, tropo, iono) and known ambiguities
         * from \p ppp_state into the internal state vector and covariance where applicable.
         *
         * @param ppp_state Source PppState containing updated values.
         */
        void syncPppStateToStateVector(const PppState& ppp_state);

        /**
         * @brief Remove an ambiguity state by key.
         *
         * The ambiguity state and its associated covariance are removed from the filter.
         *
         * @param key Ambiguity key to remove.
         * @return true if the ambiguity was found and removed, false if not present.
         */
        bool removeAmbiguity(const std::string& key);

        /**
         * @brief Remove all ambiguity states except those listed in keep_keys.
         *
         * Convenience to prune stale ambiguities when the caller knows exactly which
         * ambiguity keys must remain (e.g. from the current epoch measurement set).
         *
         * @param keep_keys Set of ambiguity keys to keep.
         */
        void pruneAmbiguitiesExcept(const std::set<std::string>& keep_keys);

    private:
        size_t core_state_size_;                    //!< Number of core states (e.g. 5 or 6, +2 if gradients).
        size_t base_core_size_;                     //!< Base core size without inter-system time offsets.
        size_t gradient_north_idx_;                 //!< Index of tropo north gradient (max if disabled).
        size_t gradient_east_idx_;                  //!< Index of tropo east gradient (max if disabled).
        std::vector<std::string> systems_with_time_offsets_; //!< Systems with time offset states (excludes GPS).
        std::map<std::string, size_t> system_time_offset_indices_; //!< Map system name -> state index.
        std::map<std::string, size_t> ambiguity_indices_; //!< Map ambiguity key -> state index.
        GnssTime state_epoch_time_;                 //!< Epoch time associated with the stored state.
        Eigen::VectorXd state_vector_;              //!< Filter state vector (core + ambiguities).
        Eigen::MatrixXd covariance_;                //!< State covariance matrix.
        LogManager& log_manager_;                   //!< Reference to logging manager.
        const GnssConfig& config_;                  //!< Read-only configuration reference.
        std::map<std::string, double> meas_var_scale_; //!< Per-measurement variance scale map (key -> multiplier).
    };

} // namespace GnssProcessor

#endif // STATEMANAGER_HPP