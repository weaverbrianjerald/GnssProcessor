#pragma once

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
         * @brief Index of the receiver velocity state within the state vector.
         *
         * Velocity occupies three consecutive elements (Vx, Vy, Vz).
         * Only present when velocity estimation is enabled.
         * @return Index of the first velocity element, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getVelocityIndex() const;

        /**
         * @brief Check if velocity estimation is active.
         * @return True if velocity states exist in the state vector.
         */
        bool hasVelocity() const;

        /**
         * @brief Index of the receiver clock bias state within the state vector.
         * @return Index of the clock bias state.
         */
        size_t getClockIndex() const;

        /**
         * @brief Index of the receiver phase clock offset state within the state vector.
         *
         * When separate phase clock estimation is enabled, phase measurements use
         * (code_clock + phase_clock_offset). This returns the index of the phase
         * clock offset state.
         *
         * @return Index of the phase clock offset state, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getPhaseClockIndex() const;

        /**
         * @brief Check if separate phase clock estimation is active.
         * @return True if a phase clock offset state exists in the state vector.
         */
        bool hasPhaseClock() const;

        /**
         * @brief Index of the tropospheric zenith wet delay (ZWD) state.
         * @return Index of the troposphere state.
         */
        size_t getTropoIndex() const;

        /**
         * @brief Index of the ionospheric zenith delay state (PPP-RTK only).
         * @return Index of the ionosphere state, or an index >= state size if not present.
         * @deprecated Use getIonosphereIndex(satellite) for per-satellite iono states.
         */
        size_t getIonoIndex() const;

        /**
         * @brief Lookup the per-satellite ionosphere state index by satellite ID.
         *
         * In PPP-RTK uncombined mode each satellite has its own slant ionosphere
         * state managed dynamically (like ambiguities). Keys are satellite IDs
         * (e.g. "G01").
         *
         * @param satellite Satellite identifier.
         * @return Zero-based index of the ionosphere state, or -1 if not present.
         */
        int getIonosphereIndex(const std::string& satellite) const;

        /**
         * @brief Get reference to the internal per-satellite ionosphere index map.
         * @return Map from satellite ID to state index.
         */
        const std::map<std::string, size_t>& getIonosphereStates() const;

        /**
         * @brief Add a new per-satellite ionosphere state and return its index.
         *
         * The ionosphere state is appended to the state vector and covariance.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param initial_value Initial slant ionosphere delay in meters.
         * @return Index assigned to the newly-created ionosphere state.
         */
        size_t addIonosphereState(const std::string& satellite, double initial_value);

        /**
         * @brief Remove all per-satellite ionosphere states except those listed in keep_keys.
         *
         * Convenience to prune stale iono states for satellites no longer visible.
         *
         * @param keep_keys Set of satellite IDs to keep.
         */
        void pruneIonosphereStatesExcept(const std::set<std::string>& keep_keys);

        /**
         * @brief Store the last-known elevation (degrees) for a per-satellite iono state.
         *
         * Used by the process noise model to apply elevation-dependent spectral
         * density scaling: q_slant = q_zenith / sin²(el).
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param elevation_deg Elevation in degrees.
         */
        void setIonosphereElevation(const std::string& satellite, double elevation_deg);

        /**
         * @brief Get the stored per-satellite ionosphere elevation map.
         * @return Map from satellite ID to last-known elevation in degrees.
         */
        const std::map<std::string, double>& getIonosphereElevations() const;

        /**
         * @brief Mark a set of satellite ionosphere states as frozen (BSSD pivots).
         *
         * Frozen states receive zero process noise to prevent unbounded
         * variance growth on unobservable pivot ionosphere states.
         *
         * @param sats Set of satellite IDs whose iono states are frozen.
         */
        void setFrozenIonosphereSatellites(const std::set<std::string>& sats);

        /**
         * @brief Get the set of frozen ionosphere satellites.
         * @return Set of satellite IDs.
         */
        const std::set<std::string>& getFrozenIonosphereSatellites() const;

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
         * @brief Lookup a receiver code bias (IFB) state index by key.
         *
         * Keys have the format "system_signal" (e.g. "GPS_C2W", "Galileo_C5Q").
         * Only non-reference code signals have a code bias state; the reference
         * signal's hardware delay is absorbed by the receiver clock / ISB.
         *
         * @param key Code bias key.
         * @return Zero-based state index, or -1 if not present.
         */
        int getCodeBiasIndex(const std::string& key) const;

        /**
         * @brief Add a new receiver code bias state and return its index.
         *
         * The state is appended to the state vector and covariance with the
         * configured initial variance.
         *
         * @param key Unique code bias key (e.g. "GPS_C2W").
         * @param initial_value Initial bias value in meters (typically 0.0).
         * @return Index assigned to the newly-created code bias state.
         */
        size_t addCodeBiasState(const std::string& key, double initial_value);

        /**
         * @brief Remove all code bias states except those listed in keep_keys.
         *
         * Prunes stale code bias states for signals no longer observed.
         *
         * @param keep_keys Set of code bias keys to keep.
         */
        void pruneCodeBiasStatesExcept(const std::set<std::string>& keep_keys);

        /**
         * @brief Get reference to the internal code bias index map.
         * @return Map from code bias key to state index.
         */
        const std::map<std::string, size_t>& getCodeBiasStates() const;

        // -------------------------------------------------------------------
        // Per-satellite signal bias states  (uncorrected satellite HW delay)
        // -------------------------------------------------------------------

        /**
         * @brief Lookup a per-satellite signal bias state index.
         *
         * Keys have the format "satellite|signal" (e.g. "E11|C7Q").
         *
         * @param key Satellite bias key.
         * @return Zero-based state index, or -1 if not present.
         */
        int getSatBiasIndex(const std::string& key) const;

        /**
         * @brief Add a new per-satellite signal bias state and return its index.
         *
         * @param key Unique key "satellite|signal".
         * @param initial_value Initial bias value in meters (typically 0.0).
         * @return Index assigned to the newly-created state.
         */
        size_t addSatBiasState(const std::string& key, double initial_value);

        /**
         * @brief Remove all satellite bias states except those listed in keep_keys.
         *
         * @param keep_keys Set of keys to keep.
         */
        void pruneSatBiasStatesExcept(const std::set<std::string>& keep_keys);

        /**
         * @brief Get reference to the internal satellite bias index map.
         * @return Map from key to state index.
         */
        const std::map<std::string, size_t>& getSatBiasStates() const;

        // -------------------------------------------------------------------
        // Relaxed ambiguity support
        // -------------------------------------------------------------------

        /**
         * @brief Mark an ambiguity key as "relaxed" (non-zero process noise).
         *
         * Relaxed ambiguities receive the phase_bias process noise instead
         * of the standard (typically zero) ambiguity process noise.  This
         * allows the ambiguity to slowly track elevation-dependent residuals
         * from missing receiver ATX or BIA data.
         *
         * @param amb_key Ambiguity key (e.g. "G01_L1W_L5Q").
         */
        void markAmbiguityRelaxed(const std::string& amb_key);

        /**
         * @brief Check whether an ambiguity key is marked as relaxed.
         * @param amb_key Ambiguity key.
         * @return True if the key is in the relaxed set.
         */
        bool isAmbiguityRelaxed(const std::string& amb_key) const;

        /**
         * @brief Clear all relaxed-ambiguity marks.
         *
         * Called at the start of each epoch before ensureRequiredStates()
         * rebuilds the set from current observations.
         */
        void clearRelaxedAmbiguities();

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

        // -------------------------------------------------------------------
        // IMU bias states (GNSS/INS integration)
        // -------------------------------------------------------------------

        /**
         * @brief Check if IMU bias states are present in the state vector.
         * @return True if accelerometer and gyroscope bias states exist.
         */
        bool hasImuBiases() const;

        /**
         * @brief Index of the first accelerometer bias state (ba_x).
         *
         * Accelerometer biases occupy 3 consecutive elements (ba_x, ba_y, ba_z).
         * @return Index, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getAccelBiasIndex() const;

        /**
         * @brief Index of the first gyroscope bias state (bg_x).
         *
         * Gyroscope biases occupy 3 consecutive elements (bg_x, bg_y, bg_z).
         * @return Index, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getGyroBiasIndex() const;

        /**
         * @brief Index of the first attitude error state (dpsi_x).
         *
         * Attitude errors occupy 3 consecutive elements (dpsi_x, dpsi_y, dpsi_z).
         * Only present when IMU integration is enabled.
         * @return Index, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getAttitudeIndex() const;


    private:
        size_t core_state_size_;                    //!< Number of core states (e.g. 5 or 6, +2 if gradients, +1 if phase clock, +3 if velocity).
        size_t base_core_size_;                     //!< Base core size without inter-system time offsets.
        size_t velocity_idx_;                       //!< Index of velocity Vx state (max if disabled).
        size_t phase_clock_idx_;                    //!< Index of phase clock offset (max if disabled).
        size_t gradient_north_idx_;                 //!< Index of tropo north gradient (max if disabled).
        size_t gradient_east_idx_;                  //!< Index of tropo east gradient (max if disabled).
        size_t attitude_idx_;                       //!< Index of attitude error dpsi_x (max if disabled).
        size_t accel_bias_idx_;                     //!< Index of accelerometer bias ba_x (max if disabled).
        size_t gyro_bias_idx_;                      //!< Index of gyroscope bias bg_x (max if disabled).
        std::vector<std::string> systems_with_time_offsets_; //!< Systems with time offset states (excludes GPS).
        std::map<std::string, size_t> system_time_offset_indices_; //!< Map system name -> state index.
        std::map<std::string, size_t> ambiguity_indices_; //!< Map ambiguity key -> state index.
        std::set<std::string> relaxed_ambiguities_; //!< Ambiguity keys with non-zero process noise (incomplete product coverage).
        std::map<std::string, size_t> code_bias_indices_; //!< Map code bias key -> state index (PPP-RTK uncombined).
        std::map<std::string, size_t> sat_bias_indices_; //!< Map "satellite|signal" -> state index (uncorrected sat HW bias).
        std::map<std::string, size_t> ionosphere_indices_; //!< Map satellite ID -> per-satellite iono state index (PPP-RTK).
        std::map<std::string, double> ionosphere_elevations_; //!< Map satellite ID -> last-known elevation in degrees (PPP-RTK).
        std::set<std::string> frozen_ionosphere_sats_; //!< Satellites whose iono state is frozen (BSSD pivot).
        GnssTime state_epoch_time_;                 //!< Epoch time associated with the stored state.
        Eigen::VectorXd state_vector_;              //!< Filter state vector (core + ambiguities).
        Eigen::MatrixXd covariance_;                //!< State covariance matrix.
        LogManager& log_manager_;                   //!< Reference to logging manager.
        const GnssConfig& config_;                  //!< Read-only configuration reference.
        std::map<std::string, double> meas_var_scale_; //!< Per-measurement variance scale map (key -> multiplier).

        // -------------------------------------------------------------------
        // Internal state vector / covariance manipulation helpers
        // -------------------------------------------------------------------

        /**
         * @brief Append one state to the end of the state vector and covariance.
         *
         * Resizes both, copies existing data, sets the new value and diagonal variance.
         *
         * @param initial_value Value for the new state element.
         * @param initial_variance Diagonal covariance entry for the new state.
         * @return Index of the newly appended state.
         */
        size_t appendState(double initial_value, double initial_variance);

        /**
         * @brief Remove a single row/column from the state vector and covariance.
         *
         * After removal, all entries in ambiguity_indices_, code_bias_indices_ and
         * ionosphere_indices_ with index > rem_idx are decremented by one.
         *
         * @param rem_idx Zero-based index to remove (must be >= core_state_size_).
         */
        void removeStateAtIndex(size_t rem_idx);
    };

} // namespace GnssProcessor