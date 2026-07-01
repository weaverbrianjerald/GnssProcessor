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
#include <optional>
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
         * @brief Build a stable human-readable label for every state index.
         *
         * Returns one string per state vector index (size == getStateSize()),
         * unioning synthesized labels for the fixed-core block (position,
         * clock, ZWD, velocity, clock-rate, phase-clock, tropo gradients, IMU
         * attitude/biases, differential ZWD, inter-system time/rate offsets)
         * with the category-prefixed keys of the dynamic key→index maps
         * (ambiguities, per-sat iono, receiver code biases, satellite code /
         * phase biases).  The dynamic keys come straight from
         * StateKeySchema, so they are stable across epochs even as state rows
         * are appended or pruned (which shifts the raw indices).  This is the
         * label set used to combine normal equations across epochs / sessions
         * (see NeqAccumulator) where indexing by raw position would be
         * incorrect.  Any index not otherwise covered falls back to
         * "STATE_<i>".
         *
         * @return Vector of stable labels, indexed by state position.
         */
        std::vector<std::string> getStateLabels() const;

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
         * @brief Read a single diagonal entry of the covariance matrix.
         *
         * Cheap accessor for variance lookups that would otherwise require
         * copying the entire covariance via @c getCovariance().  Used by
         * components that need per-state variance for scoring (e.g. the
         * pivot-fitness check in PivotManager) without paying the O(n²)
         * copy cost on every query.
         *
         * @param idx Zero-based state index.
         * @return Variance @c covariance_(idx, idx), or 0.0 if @p idx is
         *         out of range (caller is expected to guard).
         */
        double getStateVariance(int idx) const;

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
         *
         * Allocated for PPP and PPP-RTK in non-BSSD modes.  Under BSSD the
         * receiver clock is differenced out by the SD transform — it is
         * unobservable to code/phase measurements and would only drift on
         * process noise, so allocation is skipped to keep the state vector
         * tight.  Baseline (DD) modes also skip it because DD cancels the
         * receiver clock identically.
         *
         * @return Index of the clock bias state, or
         *         @c std::numeric_limits<size_t>::max() if not present.
         */
        size_t getClockIndex() const;

        /**
         * @brief Check if the receiver clock-bias state is active.
         * @return True when a clock-bias state exists in the state vector.
         */
        bool hasClock() const;

        /**
         * @brief Index of the receiver clock-rate state within the state vector.
         *
         * The clock-rate state holds @f$ c \cdot d\delta t_r/dt @f$ in m/s and is
         * the GPS-system reference rate.  Doppler measurements drive it directly
         * (Jacobian coefficient +1), and it propagates the clock-bias state via
         * @f$ \delta t_r(k+1) = \delta t_r(k) + dt \cdot \dot{\delta t}_r(k) @f$
         * in the transition matrix (continuous white-noise acceleration model
         * mirrors the position/velocity block).  Present only when velocity
         * estimation is enabled — same gate that opens Doppler processing.
         *
         * @return Index, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getClockRateIndex() const;

        /**
         * @brief Check if the receiver clock-rate state is active.
         * @return True if a clock-rate state exists in the state vector.
         */
        bool hasClockRate() const;

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
         * @brief In-place variance re-inflation of an existing iono state.
         *
         * Preserves the current state value, decorrelates by zeroing the
         * iono row/column, then sets the diagonal to @p new_variance.
         * Used by the cycle-slip orchestration layer after a sat-wide
         * phase ambiguity reset to break the stale iono/sat-bias
         * partition: the pre-reset (iono, sat-bias) pair may have
         * jointly absorbed slant iono in a non-unique way, and unless
         * the iono state regains enough variance to re-track real
         * iono after the ambiguity reseed, the partition stays baked
         * in for the rest of the arc.
         *
         * Refuses to operate on a frozen (pivot) row, identified by
         * a zero diagonal — the BSSD pivot's iono state is held at
         * (0, 0) and must not be disturbed.
         *
         * @param satellite Satellite identifier.  No-op (debug log)
         *                  when no iono state exists for @p satellite.
         * @param new_variance Replacement diagonal (m²).  Falls back
         *                  to @c initial_std.ionosphere² when
         *                  non-positive.
         */
        void reinflateIonosphereVariance(const std::string& satellite,
                                          double new_variance);

        /**
         * @brief Re-inflate a receiver code-bias (DCB/IFB) state's variance.
         *
         * The network code-bias analog of @c reinflateIonosphereVariance: when
         * a consumed satellite code-bias datum re-references (the producer's
         * per-(const,signal) sum-to-zero level steps as satellites rise/set),
         * the receiver code-bias state that absorbs that per-signal common-mode
         * step must be freed to re-absorb it in a single epoch instead of
         * dragging the step through its near-frozen random walk.  Breaks the
         * cross-covariance (zeroes the row/col) and sets the diagonal, while
         * PRESERVING the state value — the hardware delay itself did not change,
         * only the gauge it is referenced to.
         *
         * @param key Receiver code-bias state key (e.g. "GPS_DCB", "GPS_IFB5").
         *            No-op (debug log) when no such state exists.
         * @param new_variance Replacement diagonal (m²).  Falls back to
         *            @c initial_std.code_bias² when non-positive.  Refused
         *            (warn) on a frozen row (P_diag ≤ 0), defending any
         *            BSSD-held / GLONASS per-slot column the network path
         *            should never target.
         */
        void reinflateCodeBiasVariance(const std::string& key,
                                       double new_variance);

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
         * @brief Store the last-known azimuth (degrees) for a per-satellite iono state.
         *
         * Kept alongside the elevation map for offline diagnostics: it lets
         * the ionosphere trace logger emit a sky-plot-ready (el, az) pair per
         * row without having to plumb the original observation through to
         * the logger.  Not used by the process-noise model (which only needs
         * elevation), so the convention is "best-effort, last observation
         * wins" — same as setIonosphereElevation.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param azimuth_deg Azimuth in degrees (0 = North, increasing East).
         */
        void setIonosphereAzimuth(const std::string& satellite, double azimuth_deg);

        /**
         * @brief Get the stored per-satellite ionosphere azimuth map.
         * @return Map from satellite ID to last-known azimuth in degrees.
         */
        const std::map<std::string, double>& getIonosphereAzimuths() const;

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
         * @brief Set per-ambiguity-key process noise overrides.
         *
         * Replaces the default @c process_noise.ambiguity value for the
         * specific ambiguity keys in @p overrides during the next Q
         * matrix build.  Keys not in the map use the default.  Use to
         * loosen process noise for specific ambiguities (e.g. signals
         * whose ATX correction was frequency-fallback-substituted) so
         * the filter can absorb the small modeling error without
         * leaking it into other states.
         *
         * Pass an empty map to clear all overrides.
         *
         * @param overrides Map from ambiguity key (e.g. "G05_L5X") to
         *                  process noise variance rate (m²/s).
         */
        void setAmbiguityProcessNoiseOverrides(const std::map<std::string, double>& overrides);

        /**
         * @brief Set per-satellite ionospheric process-noise overrides.
         *
         * Replaces the default @c process_noise.ionosphere zenith spectral
         * density for the specific satellites in @p overrides during the
         * next Q matrix build.  Satellites not in the map use the default.
         * The obliquity factor 1/sin^2(el) and the dt scaling are still
         * applied on top of the overridden value.
         *
         * Used by the adaptive ionospheric process-noise scheme
         * (@c IonoVariabilityMonitor) to loosen per-satellite iono process
         * noise during ionospheric disturbance.  Pass an empty map to
         * clear all overrides.
         *
         * @param overrides Map from satellite ID (e.g. "G05") to the zenith
         *                  ionosphere process-noise variance rate (m^2/s).
         */
        void setIonoProcessNoiseOverrides(const std::map<std::string, double>& overrides);

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
         * Allocated for PPP / PPP-RTK in non-BSSD modes.  Under BSSD the ISB
         * is differenced out alongside the receiver clock (same-system SD
         * cancels both identically), so the per-system ISB states are not
         * allocated and this returns the sentinel.
         *
         * @param system_name Name of the GNSS system (e.g., "Galileo", "GLONASS", "BeiDou").
         * @return Index of the system time bias state, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getSystemTimeOffsetIndex(const std::string& system_name) const;

        /**
         * @brief Get the inter-system clock-rate offset state index for a system.
         *
         * Mirrors @c getSystemTimeOffsetIndex but for the rate (ISBR).  The
         * total clock-rate observed on a non-GPS Doppler measurement is
         * @f$ \dot{\delta t}_r + \dot{\mathrm{ISB}}_\mathrm{sys} @f$ — the GPS-
         * referenced clock-rate plus this per-system rate offset.  Present
         * only when velocity estimation is enabled.
         *
         * @param system_name Name of the GNSS system (e.g., "Galileo", "GLONASS").
         * @return Index, or std::numeric_limits<size_t>::max() if not present.
         */
        size_t getSystemTimeRateOffsetIndex(const std::string& system_name) const;

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
         * Initial variance is the square of @p initial_std_override when
         * positive; otherwise @c GnssConfig::getAmbiguityStd() is used.
         *
         * @param key Unique ambiguity key.
         * @param initial_value Initial state value in meters.
         * @param initial_std_override Per-call override for the initial
         *        standard deviation (m).  Pass a positive value to
         *        replace the global @c getAmbiguityStd default; pass
         *        a non-positive value (default) to use the global.
         * @return Index assigned to the newly-created ambiguity state.
         */
        size_t addAmbiguity(const std::string& key, double initial_value,
                             double initial_std_override = -1.0);

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
         * @brief Record an SSR phase-bias discontinuity counter and report a bump.
         *
         * The IGS-SSR phase-bias discontinuity counter increments whenever the
         * satellite phase bias for a signal jumps, which breaks the integer
         * property of that signal's ambiguity.  The caller queries the SSR
         * store per (sat, signal) and passes the counter here; this method
         * tracks the last-seen value per @p key and returns true exactly when
         * the counter CHANGED from a previously-recorded value (any change, to
         * handle the 4-bit field's wrap), signalling that the corresponding
         * ambiguity should be reset.  The first sighting records and returns false.
         *
         * @param key     Stable per-(sat, signal) key (e.g. "G01|L1C").
         * @param counter Current discontinuity counter from the SSR phase bias.
         * @return true iff the counter changed since the last recorded value.
         */
        bool noteSsrDiscontinuity(const std::string& key, int counter);

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
         * Variance defaults to @c initial_std.sat_bias² (cold start).  Use
         * @c addSatBiasStateWithVariance to specify an explicit variance,
         * e.g. when restoring from the @c SatBiasCache.
         *
         * @param key Unique key "satellite|signal".
         * @param initial_value Initial bias value in meters (typically 0.0).
         * @return Index assigned to the newly-created state.
         */
        size_t addSatBiasState(const std::string& key, double initial_value);

        /**
         * @brief Add a sat-bias state with an explicit initial variance.
         *
         * Mirrors @c addSatBiasState but lets the caller override the
         * default cold-start variance.  Used by the cache-restore path
         * to seed a re-acquired (sat, signal) with the aged variance
         * preserved from before the visibility gap.
         *
         * @param key Unique key "satellite|signal".
         * @param initial_value Initial bias value (meters).
         * @param initial_variance Initial diagonal P entry (m²).  Must be > 0.
         * @return Index assigned to the newly-created state.
         */
        size_t addSatBiasStateWithVariance(const std::string& key,
                                            double initial_value,
                                            double initial_variance);

        /**
         * @brief In-place reset of an existing sat-bias state.
         *
         * Used by the Kalman filter's stuck-bias-detector when a
         * (sat, signal) code row has been outlier-rejected for
         * @c estimation.stuck_code_bias.min_streak consecutive epochs
         * or its EWMA mean has drifted past
         * @c estimation.stuck_code_bias.bias_floor_m.  In both cases
         * the bias state has over-converged around a value that no
         * longer matches the truth and the closed loop
         * (innovation → outlier-reject → bias never updates) needs
         * to be broken.
         *
         * Operationally:
         *  - x(idx) ← @p new_value (typically the current value
         *    plus the EWMA residual mean — snap to the corrected
         *    estimate signaled by recent residuals).
         *  - P column and row at @c idx ← 0 (covariance
         *    decorrelation; the state's relationship to other
         *    states is no longer trusted after the reset).
         *  - P(idx, idx) ← @p new_variance (inflate to a value that
         *    lets the filter absorb new innovations — typically
         *    cold-start variance @c initial_std.sat_bias²).
         *
         * Refuses to operate on a frozen (pivot) row, identified by
         * a zero diagonal: the BSSD pivot's sat-bias state is held
         * at (0, 0) by @c PivotManager and must not be disturbed.
         * Logs a warning in that case rather than silently mangling
         * the pivot constraint.
         *
         * @param key Existing key "satellite|signal".  No-op
         *            (debug log) when the key is unknown.
         * @param new_value Replacement value (m).
         * @param new_variance Replacement diagonal (m²).  Falls back
         *            to @c initial_std.sat_bias² when non-positive.
         */
        void resetSatBiasState(const std::string& key,
                                double new_value,
                                double new_variance);

        /**
         * @brief In-place variance re-inflation of all sat-bias states
         *        for one satellite.
         *
         * Preserves the current state values, decorrelates each
         * affected row/column, then sets each diagonal to
         * @p new_variance.  Skips any row whose current diagonal is
         * zero (frozen pivot) to preserve the BSSD invariant.
         *
         * Used by the cycle-slip orchestration layer alongside
         * @c reinflateIonosphereVariance after a sat-wide phase
         * ambiguity reset: the bias state may have absorbed iono
         * leakage pre-reset, and re-inflating its variance gives the
         * filter room to re-partition.  The bias state's value is
         * intentionally left in place — hardware delays don't
         * change, so the converged value is still the best estimate
         * of the pure-bias component; only the cross-state
         * correlation needs to be broken.
         *
         * @param satellite Satellite identifier.  No-op when the sat
         *                  has no sat-bias states.
         * @param new_variance Replacement diagonal (m²).  Falls back
         *                  to @c initial_std.sat_bias² when
         *                  non-positive.
         */
        void reinflateSatBiasVariancesForSat(const std::string& satellite,
                                               double new_variance);

        /**
         * @brief Remove all satellite bias states except those listed in keep_keys.
         *
         * Pruned (sat, signal) keys are stashed into the internal
         * @c SatBiasCache (value + diagonal variance + drop time) so that
         * a subsequent re-acquisition within the configured window can
         * recover the converged estimate via @c tryRestoreSatBias.
         *
         * @param keep_keys Set of keys to keep.
         */
        void pruneSatBiasStatesExcept(const std::set<std::string>& keep_keys);

        /**
         * @brief Get reference to the internal satellite bias index map.
         * @return Map from key to state index.
         */
        const std::map<std::string, size_t>& getSatBiasStates() const;

        /**
         * @brief Snapshot of a previously-pruned sat-bias state, kept in
         *        the cache so a near-term re-acquisition can skip the
         *        cold-start re-seed.
         */
        struct SatBiasCacheEntry {
            double   value;       ///< Bias value at drop time (m).
            double   variance;    ///< Diagonal P at drop time (m²).
            GnssTime drop_time;   ///< Epoch when the state was pruned.
        };

        /**
         * @brief Result of a successful @c tryRestoreSatBias lookup.
         *
         * @c value is always the cached bias value.  @c variance is
         * the diagonal P the caller should seed the new state with:
         *  - Within @c sat_bias_cache.warm_age_s of the drop time
         *    (@c is_warm == true), @c variance is the cached value aged
         *    forward by @c process_noise.sat_bias_q × @c gap_s — a
         *    "warm restart" that preserves converged precision.
         *  - Between @c warm_age_s and @c max_age_s
         *    (@c is_warm == false), @c variance is the cold-start value
         *    @c initial_std.sat_bias² — the cache supplies a better
         *    seed than WLS but the variance is reset.
         *
         * Beyond @c max_age_s, @c tryRestoreSatBias returns
         * @c std::nullopt and the caller falls back to a WLS seed.
         */
        struct SatBiasRestoration {
            double value;     ///< Cached bias value (m).
            double variance;  ///< Variance to seed the new state with (m²).
            double gap_s;     ///< Elapsed seconds from drop to restore.
            bool   is_warm;   ///< True for warm restart, false for cold-value restart.
        };

        /**
         * @brief Look up a cached sat-bias entry for re-acquisition.
         *
         * Computes the elapsed seconds @c gap_s = @c now − @c drop_time
         * for the cached entry (if any), then:
         *  - @c gap_s ≤ @c warm_age_s : returns a warm restoration
         *    (cached variance aged forward by @c sat_bias_q × @c gap_s).
         *  - @c warm_age_s < @c gap_s ≤ @c max_age_s : returns a
         *    cold-value restoration (cached value, @c initial_std.sat_bias²
         *    variance).
         *  - @c gap_s > @c max_age_s : evicts the entry and returns
         *    @c std::nullopt.
         *
         * Pure lookup — the cache entry is left in place so the caller
         * can decide whether to consume it (see
         * @c consumeSatBiasCacheEntry).
         *
         * @param key  Sat-bias key "satellite|signal".
         * @param now  Current epoch time.
         * @return Restoration parameters, or @c std::nullopt if no usable entry exists.
         */
        std::optional<SatBiasRestoration>
        tryRestoreSatBias(const std::string& key, const GnssTime& now);

        /**
         * @brief Drop a cache entry after the caller has restored it
         *        into an active state.
         *
         * Called by @c MeasurementModel once a cache lookup has been
         * applied via @c addSatBiasStateWithVariance.  Keeps the cache
         * coherent: a key is either in the active state vector or in
         * the cache, never both.
         *
         * @param key Sat-bias key to drop from the cache.
         */
        void consumeSatBiasCacheEntry(const std::string& key);

        /**
         * @brief Drop cache entries older than @c sat_bias_cache.max_age_s
         *        relative to @c now.
         *
         * Idempotent.  Called opportunistically (e.g. on each prune) to
         * keep the cache from accumulating ancient entries when a
         * satellite/signal never reacquires.
         *
         * @param now Current epoch time.
         */
        void evictExpiredSatBiasCache(const GnssTime& now);

        /**
         * @brief Read-only access to the sat-bias cache (for diagnostics).
         * @return Map from key to cached entry.
         */
        const std::map<std::string, SatBiasCacheEntry>& getSatBiasCache() const {
            return sat_bias_cache_;
        }

        // -------------------------------------------------------------------
        // Satellite PHASE-bias states (PPP_RTK_Network producer).
        //
        // Explicit per-(satellite, signal) satellite phase-bias states (Odijk
        // Eq. 4 — each carries the network pivot-receiver ambiguity lump).
        // Mirror the satellite CODE-bias block above but live in a SEPARATE map
        // (@c sat_phase_bias_indices_) keyed "<sat>|<signal>|PB".  Created and
        // owned by the network estimator (the shared satellite spine); inert in
        // every other mode.  The warm-restart cache the code-bias block carries
        // is deliberately NOT mirrored yet — the network's cross-epoch restart
        // semantics are decided alongside the estimator (W4), not the
        // single-receiver arc-continuity cache.
        // -------------------------------------------------------------------

        /// @brief Index of a satellite phase-bias state, or -1 if absent.
        int getSatPhaseBiasIndex(const std::string& key) const;

        /// @brief Read-only map "satellite|signal|PB" -> state index.
        const std::map<std::string, size_t>& getSatPhaseBiasStates() const;

        /// @brief Append a satellite phase-bias state at cold-start variance
        ///        (@c initial_std.sat_phase_bias²).  Idempotent if the key
        ///        already exists.  Returns the state index.
        size_t addSatPhaseBiasState(const std::string& key, double initial_value);

        /// @brief Append a satellite phase-bias state with an explicit variance
        ///        (e.g. a warm restart).  Non-positive variance falls back to
        ///        @c initial_std.sat_phase_bias².
        size_t addSatPhaseBiasStateWithVariance(const std::string& key,
                                                double initial_value,
                                                double initial_variance);

        /// @brief Reset a satellite phase-bias state (decorrelate row/col, seed a
        ///        fresh value+variance).  Refuses to disturb a frozen-pivot row
        ///        (diag == 0), preserving the S-basis datum.
        void resetSatPhaseBiasState(const std::string& key,
                                    double new_value,
                                    double new_variance);

        /// @brief Re-inflate the variances of all phase-bias states for a
        ///        satellite (value preserved — phase HW delay is constant),
        ///        skipping frozen-pivot rows.
        void reinflateSatPhaseBiasVariancesForSat(const std::string& satellite,
                                                  double new_variance);

        /// @brief Prune satellite phase-bias states whose keys are not in
        ///        @p keep_keys (no warm-restart cache yet — see W4).
        void pruneSatPhaseBiasStatesExcept(const std::set<std::string>& keep_keys);

        // -------------------------------------------------------------------
        // Satellite CLOCK states (PPP_RTK_Network producer).
        //
        // Explicit per-satellite clock-correction states (Δδtˢ in metres,
        // relative to the a-priori product clock already removed in obs.modeled)
        // kept in a SEPARATE map (@c sat_clock_indices_) keyed "<sat>|SC".  The
        // satellite ORBIT stays fixed from the precise/real-time product; only
        // the clock is estimated, to maintain consistency with the chosen
        // S-system datum.  Created/owned by the network estimator (the shared
        // satellite spine); inert in every other mode.  Per-satellite (one clock
        // per satellite, NOT per signal), so there is no signal field.  Like the
        // phase-bias block, the warm-restart cache is deliberately NOT mirrored
        // yet — network cross-epoch restart is decided alongside the estimator.
        // -------------------------------------------------------------------

        /// @brief Index of a satellite clock state, or -1 if absent.
        int getSatClockIndex(const std::string& key) const;

        /// @brief Read-only map "satellite|SC" -> state index.
        const std::map<std::string, size_t>& getSatClockStates() const;

        /// @brief Append a satellite clock state at cold-start variance
        ///        (@c initial_std.sat_clock²).  Idempotent if the key already
        ///        exists.  Returns the state index.
        size_t addSatClockState(const std::string& key, double initial_value);

        /// @brief Append a satellite clock state with an explicit variance
        ///        (e.g. a warm restart / carried-forward prior).  Non-positive
        ///        variance falls back to @c initial_std.sat_clock².
        size_t addSatClockStateWithVariance(const std::string& key,
                                            double initial_value,
                                            double initial_variance);

        /// @brief Reset a satellite clock state (decorrelate row/col, seed a
        ///        fresh value+variance).  Refuses to disturb a frozen-pivot row
        ///        (diag == 0), preserving the S-basis datum.
        void resetSatClockState(const std::string& key,
                                double new_value,
                                double new_variance);

        /// @brief Re-inflate the variance of a satellite's clock state, skipping
        ///        a frozen-pivot row.  Value is preserved.
        void reinflateSatClockVarianceForSat(const std::string& satellite,
                                             double new_variance);

        /// @brief Prune satellite clock states whose keys are not in
        ///        @p keep_keys (no warm-restart cache yet — see W4).
        void pruneSatClockStatesExcept(const std::set<std::string>& keep_keys);

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

        // -------------------------------------------------------------------
        // Gap-recovery helpers
        //
        // Surgical covariance edits used by @c Application::handleGap when
        // an inter-epoch outage exceeds @c GnssConfig::getDataGapTolerance.
        // The diagonal-only process-noise injection in
        // @c getProcessNoiseMatrix is the right model for short, in-arc
        // dt steps but is structurally unable to express "the satellite-
        // specific cross-covariances we built up before the gap no longer
        // describe the current geometry" — that requires explicitly
        // touching the off-diagonal blocks, which these helpers do.
        // -------------------------------------------------------------------

        /**
         * @brief Zero cross-covariances between position and per-satellite states.
         *
         * Sets P(pos_i, j) = P(j, pos_i) = 0 for j ∈ {ionosphere,
         * ambiguity, satellite bias} and i ∈ {x,y,z}.  Leaves the
         * position diagonal P(pos,pos) and cross-covariances to core
         * states (clock, clock-rate, troposphere, velocity, IMU biases)
         * untouched — those represent physically real correlations that
         * survive an outage; sat-specific correlations do not.
         *
         * Called on @c gap_detected for static-short and all other gap
         * recovery branches.
         */
        void zeroPositionCrossCovariancesToSatStates();

        /**
         * @brief Zero cross-covariances between clock-bias and per-satellite states.
         *
         * Mirrors @c zeroPositionCrossCovariancesToSatStates for the
         * receiver clock-bias row/column.  Used after WLS clock seeding
         * (which jolts the value) so the stale clock↔sat-state
         * correlations don't reroute the next measurement residual
         * along pre-gap error directions.
         */
        void zeroClockCrossCovariancesToSatStates();

        /**
         * @brief Add @p sigma_m² to each diagonal element of the position block.
         *
         * Used by @c handleGap to loosen the position prior across a
         * gap in modes where position can drift (kinematic-no-IMU) or
         * where the pre-gap converged variance is too tight to allow
         * the filter to react to new evidence (static-long).
         *
         * @param sigma_m Standard-deviation increment in meters.
         */
        void inflatePositionDiagonal(double sigma_m);

        /**
         * @brief Set velocity value to zero and inflate diagonal to @p sigma_mps².
         *
         * For kinematic-no-IMU cold-start: with no inertial bridging
         * the velocity estimate at the post-gap epoch carries no
         * information and is safest as a loose zero prior.
         *
         * @param sigma_mps Standard deviation in m/s.  No-op when
         *        @c hasVelocity() is false.
         */
        void resetVelocityToLoose(double sigma_mps);

        /**
         * @brief Zero the satellite-bias cache.
         *
         * The cache stores (value, variance, drop-time) for sat-bias
         * states that were pruned, so a near-term re-acquisition can
         * warm-restart.  This method discards the cache entirely.
         * Use only when the pre-gap hardware-bias estimates are
         * deemed untrustworthy (e.g., long gap on a kinematic platform
         * where the receiver may have power-cycled).
         */
        void clearSatBiasCache();


    private:
        size_t core_state_size_;                    //!< Number of core states (e.g. 5 or 6, +2 if gradients, +1 if phase clock, +3 if velocity).
        size_t base_core_size_;                     //!< Base core size without inter-system time offsets.
        size_t clock_idx_;                          //!< Index of receiver clock-bias state (max if BSSD / baseline).
        size_t tropo_idx_;                          //!< Index of troposphere ZWD state (max in baseline).
        size_t velocity_idx_;                       //!< Index of velocity Vx state (max if disabled).
        size_t clock_rate_idx_;                     //!< Index of receiver clock-rate state (max if disabled).
        size_t phase_clock_idx_;                    //!< Index of phase clock offset (max if disabled).
        size_t gradient_north_idx_;                 //!< Index of tropo north gradient (max if disabled).
        size_t gradient_east_idx_;                  //!< Index of tropo east gradient (max if disabled).
        size_t attitude_idx_;                       //!< Index of attitude error dpsi_x (max if disabled).
        size_t accel_bias_idx_;                     //!< Index of accelerometer bias ba_x (max if disabled).
        size_t gyro_bias_idx_;                      //!< Index of gyroscope bias bg_x (max if disabled).
        size_t differential_tropo_idx_;             //!< Index of baseline-mode differential ZWD state (max if disabled).
        std::vector<std::string> systems_with_time_offsets_; //!< Systems with time offset states (excludes GPS).
        std::map<std::string, size_t> system_time_offset_indices_; //!< Map system name -> state index.
        std::map<std::string, size_t> system_time_rate_offset_indices_; //!< Map system name -> ISBR state index (clock-rate inter-system offset; active when velocity is estimated).
        std::map<std::string, size_t> ambiguity_indices_; //!< Map ambiguity key -> state index.
        std::set<std::string> unresolvable_ambiguities_; //!< Ambiguity keys excluded from integer AR (incomplete product coverage).
        std::set<std::string> pending_reseed_ambiguities_; //!< Ambiguity keys awaiting re-seeding after reset.
        std::map<std::string, size_t> code_bias_indices_; //!< Map code bias key -> state index (PPP-RTK uncombined).
        std::map<std::string, size_t> sat_bias_indices_; //!< Map "satellite|signal" -> state index (uncorrected sat HW bias).
        std::map<std::string, SatBiasCacheEntry> sat_bias_cache_; //!< Pruned sat-bias snapshots awaiting potential re-acquisition.
        std::map<std::string, size_t> sat_phase_bias_indices_; //!< Map "satellite|signal|PB" -> state index (PPP_RTK_Network satellite phase bias).
        std::map<std::string, size_t> sat_clock_indices_;      //!< Map "satellite|SC" -> state index (PPP_RTK_Network satellite clock).
        std::map<std::string, size_t> ionosphere_indices_; //!< Map satellite ID -> per-satellite iono state index (PPP-RTK).
        std::map<std::string, double> ionosphere_elevations_; //!< Map satellite ID -> last-known elevation in degrees (PPP-RTK).
        std::map<std::string, double> ionosphere_azimuths_;   //!< Map satellite ID -> last-known azimuth in degrees (PPP-RTK, diagnostics only).
        std::set<std::string> frozen_pivot_sats_; //!< BSSD pivot satellites: iono, ambiguity, and sat-bias states are frozen (Q=0).
        std::map<std::string, double> amb_q_overrides_; //!< Per-(amb_key) ambiguity process noise (m²/s); replaces process_noise.ambiguity for those keys when building Q.
        std::map<std::string, double> iono_q_overrides_; //!< Per-satellite zenith iono process noise (m^2/s); replaces process_noise.ionosphere for those sats when building Q.
        GnssTime state_epoch_time_;                 //!< Epoch time associated with the stored state.
        Eigen::VectorXd state_vector_;              //!< Filter state vector (core + ambiguities).
        Eigen::MatrixXd covariance_;                //!< State covariance matrix.
        LogManager& log_manager_;                   //!< Reference to logging manager.
        const GnssConfig& config_;                  //!< Read-only configuration reference.
        bool is_baseline_{ false };                 //!< True when configured for baseline (DD) mode: no clock/tropo/iono/system-offset states.
        std::map<std::string, double> meas_var_scale_; //!< Per-measurement variance scale map (key -> multiplier).
        std::map<std::string, int> last_ssr_discontinuity_; //!< Last-seen SSR phase-bias discontinuity counter per "sat|signal" key.

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