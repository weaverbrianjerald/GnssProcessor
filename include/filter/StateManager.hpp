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

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
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
         * @brief Index of the differential ZWD state (baseline mode only).
         *
         * Estimable when @c BaselineConfig::estimate_differential_tropo is on.
         * The state represents Δzwd = ZWD_rover − ZWD_reference; phase DD H
         * rows write @c (mf_w_i − mf_w_p) into this column.  Returns
         * @c std::numeric_limits<size_t>::max() when not present (which is
         * the case in PPP / PPP-RTK modes, or in baseline mode with the flag
         * off — wet delay is then assumed to cancel in BRSD).
         */
        size_t getDifferentialTropoIndex() const;

        /**
         * @brief Check whether the differential ZWD state is active.
         */
        bool hasDifferentialTropo() const;

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
         * @param std_scale Multiplier applied to @c initial_std.ionosphere when
         *        forming the initial variance.  Defaults to 1.0 (use the
         *        configured std as-is).  Pass a value > 1 when the initial
         *        value is unreliable (e.g. seed=0 because no BIA-covered
         *        code pair was available) so the filter doesn't trust the
         *        zero too tightly and converges quickly to the true iono.
         *        The obliquity (1/sin² el) scaling is applied on top of this.
         * @return Index assigned to the newly-created ionosphere state.
         */
        size_t addIonosphereState(const std::string& satellite, double initial_value,
                                  double std_scale = 1.0);

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
         * density scaling: q_slant = q_zenith / sin�(el).
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
         * @brief Mark a set of satellites as BSSD pivots (state-freeze).
         *
         * The pivot's per-(sat, signal) states — ionosphere, ambiguities,
         * and sat-biases — are frozen at value=0, variance=0 by the BSSD
         * S-transform.  This set tells @c getProcessNoiseMatrix() to skip
         * Q for those states so process noise can't grow their variance
         * past the disturbance-detection threshold (which would trigger a
         * spurious pivot handover every epoch).  Real cycle slips on the
         * pivot's *observation* are caught separately by the phase-only
         * pivot-slip detector in @c KalmanFilter::validatePostFitResiduals
         * and routed through @c forced_handover_requests_.
         *
         * @param sats Set of pivot satellite IDs.
         */
        void setFrozenPivotSatellites(const std::set<std::string>& sats);

        /**
         * @brief Get the set of BSSD pivot satellites whose states are frozen.
         * @return Set of satellite IDs.
         */
        const std::set<std::string>& getFrozenPivotSatellites() const;

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
         *  - Cross-covariances for that ambiguity row/column are zeroed.
         *  - The ambiguity diagonal variance is set to (std_m)^2.
         *  - The ambiguity value is left unchanged (it will be overwritten
         *    by the geometry-based seed in ensureRequiredStates).
         *  - The key is marked as pending re-seed.
         *  - If the ambiguity key does not exist, a new state is created.
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
         * creating new ambiguities and when re-seeding ambiguities after
         * cycle-slip or outlier detection.
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
        // Unresolvable ambiguity support
        // -------------------------------------------------------------------

        /**
         * @brief Mark an ambiguity as unresolvable (excluded from integer AR).
         *
         * Ambiguities missing BIA phase bias or receiver ATX coverage
         * absorb a time-constant satellite/receiver bias into their
         * float value.  They cannot be resolved to the correct integer,
         * so they are excluded from the LAMBDA search.  Process noise
         * remains zero (the absorbed bias is constant).
         *
         * @param amb_key Ambiguity key (e.g. "G01_L1W_L5Q").
         */
        void markAmbiguityUnresolvable(const std::string& amb_key);

        /**
         * @brief Check whether an ambiguity key is marked as unresolvable.
         * @param amb_key Ambiguity key.
         * @return True if the key is in the unresolvable set.
         */
        bool isAmbiguityUnresolvable(const std::string& amb_key) const;

        /**
         * @brief Clear all unresolvable-ambiguity marks.
         *
         * Called at the start of each epoch before ensureRequiredStates()
         * rebuilds the set from current observations.
         */
        void clearUnresolvableAmbiguities();

        /**
         * @brief Check whether an ambiguity is awaiting re-seeding.
         *
         * After resetAmbiguity() the ambiguity is marked pending so that
         * ensureRequiredStates() can compute a proper geometry-based seed
         * before the next KF update.
         *
         * @param amb_key Ambiguity key.
         * @return True if the key needs re-seeding.
         */
        bool isAmbiguityPendingReseed(const std::string& amb_key) const;

        /**
         * @brief Clear a single pending-reseed mark after the seed is applied.
         * @param amb_key Ambiguity key.
         */
        void clearPendingReseed(const std::string& amb_key);

        /**
         * @brief Mark an ambiguity as pending re-seed without modifying state.
         *
         * Used when the caller manages the state vector and covariance
         * externally (e.g., during a pivot handover that operates on local
         * copies of x and P).  ensureRequiredStates() will recompute the
         * seed on the next call.
         *
         * @param amb_key Ambiguity key.
         */
        void markAmbiguityPendingReseed(const std::string& amb_key);

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
         * @brief Rename an ambiguity key without touching the state vector.
         *
         * Used by the DD baseline pivot-handover S-transform: when the
         * pivot for a constellation changes, the existing DD ambiguity
         * row indices stay put — only the key strings need to change to
         * reflect the new pivot.  The state value at the row index is
         * separately transformed by the caller (T·x); this method just
         * re-binds the key in @c ambiguity_indices_.  No covariance
         * compaction; no index shifting.  Returns false if the old key
         * is not present or the new key already exists (caller should
         * resolve the collision).
         *
         * @param old_key  Existing ambiguity key in the indices map.
         * @param new_key  Target key (must not already exist).
         * @return true on successful rename, false otherwise.
         */
        bool renameAmbiguity(const std::string& old_key, const std::string& new_key);

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
        size_t differential_tropo_idx_;             //!< Index of baseline-mode differential ZWD state (max if disabled).
        std::vector<std::string> systems_with_time_offsets_; //!< Systems with time offset states (excludes GPS).
        std::map<std::string, size_t> system_time_offset_indices_; //!< Map system name -> state index.
        std::map<std::string, size_t> ambiguity_indices_; //!< Map ambiguity key -> state index.
        std::set<std::string> unresolvable_ambiguities_; //!< Ambiguity keys excluded from integer AR (incomplete product coverage).
        std::set<std::string> pending_reseed_ambiguities_; //!< Ambiguity keys awaiting re-seeding after reset.
        std::map<std::string, size_t> code_bias_indices_; //!< Map code bias key -> state index (PPP-RTK uncombined).
        std::map<std::string, size_t> sat_bias_indices_; //!< Map "satellite|signal" -> state index (uncorrected sat HW bias).
        std::map<std::string, size_t> ionosphere_indices_; //!< Map satellite ID -> per-satellite iono state index (PPP-RTK).
        std::map<std::string, double> ionosphere_elevations_; //!< Map satellite ID -> last-known elevation in degrees (PPP-RTK).
        std::set<std::string> frozen_pivot_sats_; //!< BSSD pivot satellites: iono, ambiguity, and sat-bias states are frozen (Q=0).
        GnssTime state_epoch_time_;                 //!< Epoch time associated with the stored state.
        Eigen::VectorXd state_vector_;              //!< Filter state vector (core + ambiguities).
        Eigen::MatrixXd covariance_;                //!< State covariance matrix.
        LogManager& log_manager_;                   //!< Reference to logging manager.
        const GnssConfig& config_;                  //!< Read-only configuration reference.
        bool is_baseline_{ false };                 //!< True when configured for baseline (DD) mode: no clock/tropo/iono/system-offset states.
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