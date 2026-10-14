#pragma once

/**
 * @file StateManager.hpp
 * @brief Kalman filter state vector, covariance and indexed states.
 *
 * Accessors and mutators for the state vector and covariance, the fixed core
 * layout (position, clock, troposphere and the optional core states), the
 * key->index maps of the dynamic state families (declared in
 * `filter/StateFamily.hpp`), the process noise and transition matrices, and the
 * ambiguity bookkeeping.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateFamily.hpp"
#include <Eigen/Dense>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <set>

namespace GnssProcessor {

    class LogManager; // forward-declare

    /**
     * @brief Owns the Kalman filter's state and the key→index maps over it.
     *
     * Holds the state epoch time, state vector and covariance.  The fixed core
     * block (position, clock, ZWD and the optional velocity, clock rate, phase
     * clock, gradients, IMU and inter-system offsets) is laid out once by the
     * constructor; the dynamic families (ambiguities, slant ionosphere and the
     * other rows of @ref kStateFamilies) each keep a key->index map that grows
     * and shrinks at run time.  Also provides the transition and process-noise
     * matrices, the core-state index getters and the per-family add, prune,
     * reset and re-inflation routines.
     */
    class StateManager {
    public:
        /**
         * @brief Construct a StateManager.
         *
         * @param core_state_size Core-state count supplied by the caller.  Only baseline
         *        mode reads it, as the size of the fixed core (3 = position; the optional
         *        differential ZWD is appended after it).  The other modes derive the core
         *        layout from @p config and ignore this value.
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
         * @brief Get the total number of states (the core block plus every
         *        dynamic family's map, @ref kStateFamilies).
         * @return Total state size.
         */
        size_t getStateSize() const;

        /**
         * @brief Build a stable human-readable label for every state index.
         *
         * One string per state index (size == getStateSize()), unioning
         * synthesized labels for the fixed-core block (position, clock, ZWD,
         * velocity, clock-rate, phase-clock, tropo gradients, IMU
         * attitude/biases, differential ZWD, inter-system time/rate offsets)
         * with the keys of every dynamic family, each behind its family's label
         * prefix (@ref kStateFamilies: slant ionosphere, ionosphere rate,
         * ambiguity, receiver code bias, satellite code bias, satellite phase
         * bias, receiver phase bias, phase colour, satellite clock).
         *
         * The dynamic keys come from StateKeySchema, so they stay stable across
         * epochs even as rows are appended or pruned and the raw indices shift.
         * That is what makes them the right labels for combining normal
         * equations across epochs or sessions (NeqAccumulator), where indexing
         * by raw position would be wrong.  Uncovered indices fall back to
         * "STATE_<i>".
         *
         * @return Stable labels, indexed by state position.
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
         * @brief Build the process noise matrix for prediction.
         *
         * Built from @c filter.process_noise and the time step \p dt, one block
         * per state family (motion, receiver clock, phase clock, troposphere,
         * slant ionosphere, receiver code bias, satellite code bias, satellite
         * phase bias, satellite clock, inter-system offsets, ambiguities); every
         * entry is written by exactly one block.  Per-satellite states of a
         * frozen BSSD pivot (@ref setFrozenPivotSatellites) get no process noise.
         * In baseline mode only the motion block, the ambiguities and the
         * optional differential ZWD are filled.
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
         * Allocated for PPP and PPP-RTK in non-BSSD modes.  Under BSSD the SD
         * transform differences the receiver clock out: it is unobservable to
         * code and phase measurements and would only drift on process noise, so
         * it is not allocated.  Baseline (DD) modes also omit it because DD
         * cancels the receiver clock identically.
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
         * in the transition matrix (a continuous white-noise acceleration model,
         * as for the position/velocity block).  Present only when velocity
         * estimation is enabled (the gate that also opens Doppler processing) and
         * BSSD is off.
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
         * @c std::numeric_limits<size_t>::max() when not present: in PPP and
         * PPP-RTK modes, and in baseline mode with the flag off, where the wet
         * delay is assumed to cancel in BRSD.
         */
        size_t getDifferentialTropoIndex() const;

        /**
         * @brief Check whether the differential ZWD state is active.
         */
        bool hasDifferentialTropo() const;

        /**
         * @brief Index of a single ionospheric zenith delay state, which no mode allocates.
         * @return Always @c std::numeric_limits<size_t>::max(): no such state is
         *         allocated, and the ionosphere is carried in per-satellite states.
         * @deprecated Use getIonosphereIndex(satellite) for per-satellite iono states.
         */
        size_t getIonoIndex() const;

        /**
         * @brief Lookup the per-satellite ionosphere state index by satellite ID.
         *
         * With uncombined processing (@c processing.uncombined_enabled, in any
         * mode) each satellite has its own slant ionosphere state managed
         * dynamically (like ambiguities). Keys are satellite IDs (e.g. "G01").
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
         * @brief Lookup the per-satellite iono-rate (Idot_s) state index.
         *
         * Present only when @c GnssConfig::enableIonoRate() is on (constant-
         * velocity slant-iono model).  The rate is a companion to the level
         * state @c getIonosphereIndex, keyed by the same satellite ID, added
         * and pruned in lockstep with it.  Unobserved directly (H=0); driven by
         * the F-coupling into the level state.
         *
         * @param satellite Satellite identifier.
         * @return Zero-based index of the iono-rate state, or -1 if not present.
         */
        int getIonosphereRateIndex(const std::string& satellite) const;

        /**
         * @brief Get reference to the internal per-satellite iono-rate index map.
         * @return Map from satellite ID to state index (empty unless enableIonoRate).
         */
        const std::map<std::string, size_t>& getIonosphereRateStates() const;

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
         * Preserves the state value, decorrelates by zeroing the iono row/column,
         * then raises the diagonal to @p new_variance: the new diagonal is the larger
         * of the current and the requested variance, so a re-inflation never shrinks
         * a variance (lowering it would inject information; card OWD).  The
         * cycle-slip layer calls this after a sat-wide ambiguity reset to break the
         * stale iono/sat-bias partition: the pre-reset pair may have absorbed slant
         * iono non-uniquely, and without enough variance to re-track after the
         * reseed, that partition persists for the rest of the arc.
         *
         * Refuses a frozen (pivot) row, identified by a diagonal ≤ 0: the BSSD
         * pivot's iono state is held at (0, 0) and is left undisturbed.
         *
         * @param satellite Satellite identifier.  No-op when it has no iono state.
         * @param new_variance Requested diagonal (m²), a floor the current variance is
         *                  raised to; falls back to @c initial_std.ionosphere² when
         *                  non-positive.
         */
        void reinflateIonosphereVariance(const std::string& satellite,
                                          double new_variance);

        /**
         * @brief Re-inflate a per-satellite iono-rate (Idot_s) state on a slip.
         *
         * The constant-velocity companion of @c reinflateIonosphereVariance, with
         * the same semantics: the rate value is preserved (the physical slant-iono
         * ramp is continuous across a phase-lock loss, like the level), the
         * cross-covariance partition is broken (row/column zeroed) and the diagonal
         * is raised to @p new_variance, so the filter can re-learn the ramp after
         * the ambiguity reseed.  No-op when @c enableIonoRate is off or no rate
         * state exists.  Refuses a frozen (pivot) row (diagonal ≤ 0).
         *
         * @param satellite Satellite identifier.
         * @param new_variance Requested diagonal (m²/s²), a floor the current variance
         *                  is raised to (never lowered; card OWD).  Falls back to
         *                  @c initial_std.ionosphere_rate² when non-positive.
         */
        void reinflateIonosphereRateVariance(const std::string& satellite,
                                             double new_variance);

        /**
         * @brief Re-inflate a receiver code-bias (DCB/IFB) state's variance.
         *
         * The code-bias analog of @c reinflateIonosphereVariance.  When a consumed
         * satellite code-bias datum re-references (the producer's per-(const,signal)
         * sum-to-zero level steps as satellites rise and set), the receiver code-bias
         * state that absorbs the common-mode step is freed to take it in one epoch
         * instead of following it through a near-frozen random walk.  Zeroes the
         * row/col and raises the diagonal to the request (the larger of the current
         * and the requested variance, so a re-inflation never shrinks; card OWD)
         * while preserving the value: the hardware delay is unchanged, only the
         * gauge it is referenced to has stepped.
         *
         * @param key Receiver code-bias key (e.g. "GPS_DCB", "GPS_IFB5").  No-op
         *            when no such state exists.
         * @param new_variance Requested diagonal (m²), a floor; falls back to
         *            @c initial_std.code_bias² when non-positive.  Refused on a
         *            frozen row (P_diag ≤ 0), which protects any BSSD-held or
         *            GLONASS per-slot column; the network path does not target those.
         */
        void reinflateCodeBiasVariance(const std::string& key,
                                       double new_variance);

        /**
         * @brief Re-inflate an inter-system time-offset (ISB) state's variance.
         *
         * The clock-datum analog of @c reinflateCodeBiasVariance.  A clock-datum
         * re-reference steps every satellite clock of one constellation by a common
         * constant.  The receiver clock absorbs the reference constellation's step; a
         * non-reference constellation's step is differential and lands on the ISB
         * state, whose near-frozen random walk cannot follow a jump in one epoch, so
         * the step biases every observation of that constellation until the ISB
         * re-converges.  Zeroes the row/col and raises the diagonal to the request
         * (the larger of the current and the requested variance; card OWD) while
         * preserving the value: the inter-system hardware offset is unchanged, only
         * the clock gauge has stepped.
         *
         * @param system_name Constellation name as used by the ISB key map
         *            (e.g. "Galileo", "GLONASS"; @c StateInitializer::systemToIsbName).
         *            No-op when no such state exists, as for the reference
         *            constellation.
         * @param new_variance Requested diagonal (m²), a floor; falls back to
         *            @c initial_std.system_time_offset² when non-positive.
         *            Refused on a frozen row (P_diag ≤ 0).
         */
        void reinflateSystemTimeOffsetVariance(const std::string& system_name,
                                               double new_variance);

        /// The seven variance re-inflations, in the order of the run-end line (card OWD, O2).
        /// Declared with the family table (`filter/StateFamily.hpp`), where each dynamic
        /// family names its slot (@ref StateFamilySpec::reinflation) and the run-end names
        /// are @ref kReinflationNames.
        using Reinflation = GnssProcessor::Reinflation;

        /// One re-inflation's counts: states whose diagonal became the request (the request
        /// was at least the current variance) and states that kept a larger current variance
        /// (where writing the request would have lowered it).  A refused (frozen) row counts
        /// in neither (card OWD, O2).
        struct ReinflateCounter {
            long long raised = 0;   ///< request >= current: the diagonal became the request
            long long kept   = 0;   ///< current > request: the diagonal kept its value
        };

        /// The counters of the seven re-inflations, indexed by @ref Reinflation (card OWD, O2).
        using ReinflateCounters =
            std::array<ReinflateCounter, static_cast<std::size_t>(Reinflation::Count)>;

        /// This state manager's re-inflation counters since construction (card OWD, O2).
        /// @return The counters, indexed by @ref Reinflation.
        const ReinflateCounters& reinflateCounters() const { return reinflate_counters_; }

        /**
         * @brief Format re-inflation counters as the run-end summary (card OWD, O2).
         *
         * One format everywhere (the consumer, the producer summed over its stations, the
         * PPP run), parsed by the card's graders:
         * "code_bias raised=N kept=M | isb raised=N kept=M | iono ... | iono_rate ... |
         * sat_bias ... | sat_phase_bias ... | sat_clock raised=N kept=M".
         *
         * @param counters One state manager's counters, or a sum of several.
         * @return The formatted summary.
         */
        static std::string reinflateSummary(const ReinflateCounters& counters);

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
         * @brief Store the last-known azimuth (degrees) for a per-satellite iono state.
         *
         * Kept alongside the elevation map for offline diagnostics: the
         * ionosphere trace logger reads it to emit a sky-plot-ready (el, az)
         * pair per row without the original observation being passed through
         * to the logger.  The process-noise model does not use it (it needs
         * only elevation), so the convention is best-effort, last observation
         * wins, as for setIonosphereElevation.
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
         * The BSSD S-transform freezes the pivot's per-(sat, signal) states
         * (ionosphere, ambiguities and sat-biases) at value=0, variance=0.
         * This set tells @c getProcessNoiseMatrix() to skip Q for every
         * per-satellite state of these satellites (ionosphere and iono rate,
         * ambiguities, satellite code and phase biases, satellite clock), so
         * process noise cannot grow their variance past the
         * disturbance-detection threshold, which would trigger a spurious pivot
         * handover every epoch.  @c getTransitionMatrix() also skips their
         * iono level/rate coupling.  Real cycle slips on the pivot's
         * observation are caught separately by the phase-only pivot-slip
         * detector in @c KalmanFilter::validatePostFitResiduals and routed
         * through PivotManager's @c forced_handover_requests_.
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
         * @brief Read-only view of the inter-system time-offset (ISB) key map
         *        (system name → state index).
         *
         * Used by the RT warm-resume seam response: the ISB is the stiffest core
         * state (near-zero random walk), so a resume across a producer frame
         * re-realization re-inflates every registered ISB via
         * @ref reinflateSystemTimeOffsetVariance, which needs the full map rather
         * than a per-name probe.
         */
        const std::map<std::string, size_t>& getSystemTimeOffsetStates() const {
            return system_time_offset_indices_;
        }

        /**
         * @brief Get the inter-system clock-rate offset state index for a system.
         *
         * Mirrors @c getSystemTimeOffsetIndex but for the rate (ISBR).  The
         * total clock-rate observed on a non-GPS Doppler measurement is
         * @f$ \dot{\delta t}_r + \dot{\mathrm{ISB}}_\mathrm{sys} @f$: the
         * GPS-referenced clock-rate plus this per-system rate offset.  Present
         * only when velocity estimation is enabled and BSSD is off.
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
         * @brief Read the continuity generation of a live ambiguity.
         *
         * Birth, reset/reseed request, explicit translation, restore and rename
         * assign a new nonzero generation. Ordinary filter updates and clearing
         * a pending-reseed mark leave it unchanged. Generations are unique for
         * this manager's lifetime, including after removal and rebirth of a key.
         * This metadata survives covariance contraction after a reset; it does
         * not alter the estimated state and is not serialized in snapshots.
         *
         * @param key Ambiguity key.
         * @return Generation, or zero if the key is absent.
         */
        uint64_t getAmbiguityGeneration(const std::string& key) const;

        /**
         * @brief Mark a discontinuous ambiguity edit made on external x/P copies.
         *
         * KF recovery and slip repairs use this when they apply the edit
         * themselves. Changes metadata only, without setting pending reseed.
         *
         * @param key Existing ambiguity key.
         * @return True if the key exists and its generation was advanced.
         */
        bool noteAmbiguityDiscontinuity(const std::string& key);

        /**
         * @brief Add a new ambiguity state and return its index.
         *
         * Appends a row to the state vector and covariance.
         *
         * @param key Unique ambiguity key.
         * @param initial_value Initial state value (m).
         * @param initial_std_override Initial standard deviation (m); a positive
         *        value replaces the global @c GnssConfig::getAmbiguityStd().
         * @return Index of the new ambiguity state.
         */
        size_t addAmbiguity(const std::string& key, double initial_value,
                             double initial_std_override = -1.0);

        /**
         * @brief Reset an existing float ambiguity state.
         *
         * Zeroes the row/column cross-covariances, sets the diagonal to std_m²,
         * and marks the key pending re-seed.  The value is left unchanged;
         * ensureRequiredStates overwrites it with a geometry-based seed. Unknown
         * keys are refused. Advances the key's continuity generation.
         *
         * Intended for ambiguity reinitialization after a detected phase outlier.
         *
         * @param key Ambiguity key.
         * @param std_m Standard deviation (m) for the ambiguity diagonal.
         * @return true if a reset was applied.
         */
        bool resetAmbiguity(const std::string& key, double std_m);

        /**
         * @brief Translate a float ambiguity state by @p delta_m, value only
         *        (the consumer's declared-step carry; card CC).
         *
         * The consumer's phase row is @c predicted = … + ssr_bias_term + amb_term
         * with both signs +1, so when the applied satellite phase bias moves by
         * Δ (a declared re-realization: disc-counter bump or A5 freshness flip)
         * the ambiguity in the new datum is exactly @c amb − Δ.  This primitive
         * applies that translation: @c state_vector_(idx) += delta_m.  The
         * covariance is untouched (no row/column zeroing, no re-inflation) and
         * the key is not marked pending re-seed, so the ambiguity keeps every
         * cross-covariance it has converged; a reset would discard them.  A
         * nonzero @p delta_m advances the key's continuity generation.
         *
         * Unknown keys are refused as by @ref resetAmbiguity's guard (false plus
         * a debug line): slip detectors and the SSR datum-break reactions
         * iterate raw signals the state may not hold.
         *
         * @param key     Ambiguity key ("G24_L5X").
         * @param delta_m Translation added to the state value (metres); the
         *                caller passes −Δ for an applied-bias step of +Δ.
         * @return true iff the state existed and was translated.
         */
        bool shiftAmbiguity(const std::string& key, double delta_m);

        /**
         * @brief Record an SSR phase-bias discontinuity counter and report a bump.
         *
         * The IGS-SSR discontinuity counter increments whenever a satellite phase
         * bias jumps, breaking the integer property of that signal's ambiguity.
         * The caller queries the SSR store per (sat, signal) and passes the counter
         * here; this tracks the last-seen value and reports any change, not only an
         * increase, because the field is 4 bits and wraps.  A reported change means
         * the ambiguity should be reset.  A first sighting records and returns false.
         *
         * @param key     Stable per-(sat, signal) key (e.g. "G01|L1C").
         * @param counter Current discontinuity counter from the SSR phase bias.
         * @return true iff the counter changed since the last recorded value.
         */
        bool noteSsrDiscontinuity(const std::string& key, int counter);

        /**
         * @brief Read-only view of the SSR discontinuity-counter memory
         *        (per-"sat|signal" last-seen counters behind
         *        @ref noteSsrDiscontinuity).
         *
         * Serialized by the RT filter-state snapshot so a warm-resumed engine
         * keeps its counter baselines: a producer disc bump during the outage
         * then fires the existing ambiguity-reset reaction at the first
         * post-resume sighting, instead of being recorded as a first sighting
         * with no reset (the consumer-gap silent-frame-swap class).
         */
        const std::map<std::string, int>& getSsrDiscontinuityMemory() const {
            return last_ssr_discontinuity_;
        }

        /**
         * @brief Replace the SSR discontinuity-counter memory wholesale (RT
         *        warm resume only; see @ref getSsrDiscontinuityMemory).
         */
        void setSsrDiscontinuityMemory(std::map<std::string, int> mem) {
            last_ssr_discontinuity_ = std::move(mem);
        }

        /**
         * @brief The key->index map of one dynamic state family.
         *
         * The families and their properties are declared in @ref kStateFamilies
         * (`filter/StateFamily.hpp`); code that visits every family loops over that
         * table and reads each map through this accessor, instead of naming the
         * maps one by one.
         *
         * @param family The family.
         * @return The family's map (key -> state index); the same object the
         *         family's named getter returns (e.g. @ref getAmbiguities).
         */
        const std::map<std::string, size_t>& indicesOf(StateFamily family) const;

        /**
         * @brief Re-create one dynamic state at its serialized index and value
         *        (RT warm resume).
         *
         * Appends a row with @p value at placeholder unit variance (the resume
         * overwrites the whole covariance via @ref setCovariance once the layout
         * is rebuilt) and registers @p key in the map of @p family.  Only the
         * families with an RT snapshot tag (@ref StateFamilySpec::snapshot_tag)
         * can be restored; any other family is refused.
         *
         * Bypasses the semantic add* functions on purpose: their creation side
         * effects (elevation-scaled seeds, the enableIonoRate paired-rate append,
         * duplicate-key short-circuits) would make the rebuilt layout differ from
         * the serialized one wherever an in-run prune had interleaved the original
         * indices.
         *
         * @param family      Which dynamic map owns the key.
         * @param key         The map key, verbatim from the snapshot.
         * @param value       Serialized state value.
         * @param expect_idx  The index this row held at serialization; the append
         *                    must land exactly there (the caller replays rows in
         *                    ascending index order onto a core-only manager).
         * @return true if the row landed at @p expect_idx and the key was newly
         *         registered.  A refusal leaves the manager unchanged: the landing
         *         index is checked before the append, because getStateSize() is
         *         map-derived and an unregistered appended row would desynchronize
         *         it from the actual vector length.
         */
        bool restoreDynamicState(StateFamily family, const std::string& key,
                                 double value, size_t expect_idx);

        /**
         * @brief Compute an approximate ambiguity seed value from an observation.
         *
         * The seed is computed as:
         *   seed = observed - modeled - rx_clock - (mf_w * zwd) - isb - tropo_grad - iono
         *
         * where rx_clock (plus the phase-clock offset when estimated), zwd, the
         * inter-system time offset (isb) and the tropospheric gradient contribution
         * are read from the supplied state vector at the standard core-state
         * indices.  In uncombined mode, for a phase key of a satellite with a
         * slant-iono state I_s, iono = −α·I_s with α = (f_L1/f)² for the key's
         * band; otherwise iono = 0.  This centralizes the formula used when creating new
         * ambiguities and when re-seeding ambiguities after cycle-slip or outlier
         * detection.
         *
         * @param obs        The PppObservation containing observed/modeled values and wet mapping factor.
         * @param ref_type   The key into obs.observed / obs.modeled: an iono-free pair
         *                   (e.g. "L1C_L2W"), or a single signal in uncombined mode (e.g. "L1C").
         * @param x          State vector from which rx clock, ZWD, ISB, gradients and iono are read.
         * @return Approximate ambiguity value in meters, or 0.0 on failure.
         */
        double computeAmbiguitySeed(const PppObservation& obs,
                                    const std::string& ref_type,
                                    const Eigen::VectorXd& x) const;

        /**
         * @brief Update a PppState object from a raw state-vector representation.
         *
         * Copies position, clock, ZWD and, when present, velocity, phase clock and
         * troposphere gradients; the inter-system time offsets; the ambiguities; and
         * in PPP-RTK mode the slant-iono and iono-rate states into \p ppp_state.
         * A lightweight translator that exposes the internal state to higher-level
         * models; it copies values only, not covariance.
         *
         * @param current_state State vector to interpret.
         * @param ppp_state Output PppState that receives the values.
         * @param state_manager Manager that supplies the core-state indices and the
         *        ambiguity map for reading \p current_state; the ionosphere and ISB
         *        maps come from this object.
         */
        void updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const;

        /**
         * @brief Synchronize values from a PppState into the internal state vector.
         *
         * Writes the position, the clock (when allocated), the ZWD and, when
         * present, the velocity, phase clock and troposphere gradients; in
         * @c ProcessingMode::PPP_RTK also the slant-iono and iono-rate values of
         * satellites that have a state; and the inter-system time offsets.  Values
         * only: the ambiguities and the covariance are not written.
         *
         * @param ppp_state Source PppState containing updated values.
         */
        void syncPppStateToStateVector(const PppState& ppp_state);

        /**
         * @brief Lookup a receiver code bias (IFB) state index by key.
         *
         * Keys follow StateKeySchema: "<system>_IFB<band>", "<system>_DCB", the
         * per-FCN-slot GLONASS form "GLONASS_IFB<band>_k<±slot>", or the
         * inter-signal form "<system>_ISC_<signal>" (e.g. "GPS_IFB2", "GPS_DCB",
         * "GPS_ISC_C1W").  Only non-reference code signals have a code bias
         * state; the reference signal's hardware delay is absorbed by the
         * receiver clock / ISB.
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
         * @param key Unique code bias key (e.g. "GPS_IFB2").
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
         * Called by the stuck-bias detector when a (sat, signal) code row has been
         * outlier-rejected for @c estimation.stuck_code_bias.min_streak
         * consecutive epochs, or its EWMA mean has drifted past
         * @c estimation.stuck_code_bias.bias_floor_m.  Either way the bias has
         * over-converged on a wrong value, and the reset breaks the closed loop
         * (innovation → outlier-reject → bias never updates).
         *
         * Operationally:
         *  - x(idx) ← @p new_value, typically the current value plus the EWMA
         *    residual mean, i.e. the estimate that recent residuals imply.
         *  - P row and column at idx ← 0, since the state's correlations are not
         *    trusted after the reset.
         *  - P(idx, idx) ← @p new_variance, inflated enough to absorb new
         *    innovations (typically cold-start @c initial_std.sat_bias²).
         *
         * Refuses a frozen (pivot) row, identified by a diagonal ≤ 0: PivotManager
         * holds the BSSD pivot's sat-bias at (0, 0), so this logs a warning and
         * leaves the pivot constraint intact.
         *
         * @param key Existing key "satellite|signal".  No-op when unknown.
         * @param new_value Replacement value (m).
         * @param new_variance Replacement diagonal (m²); falls back to
         *            @c initial_std.sat_bias² when non-positive.
         */
        void resetSatBiasState(const std::string& key,
                                double new_value,
                                double new_variance);

        /**
         * @brief In-place variance re-inflation of all sat-bias states
         *        for one satellite.
         *
         * Preserves the values, decorrelates each affected row/column, then raises
         * each diagonal to @p new_variance (the larger of the current and the requested
         * variance per state; card OWD).  Rows with a zero diagonal (frozen pivot) are
         * skipped, preserving the BSSD invariant.
         *
         * The cycle-slip layer calls this alongside @c reinflateIonosphereVariance
         * after a sat-wide ambiguity reset: the bias may have absorbed iono leakage
         * beforehand, and the extra variance gives the filter room to re-partition.
         * The value is kept: hardware delays do not change, so the converged value
         * remains the best estimate of the pure-bias component, and only the
         * cross-state correlation needs breaking.
         *
         * @param satellite Satellite identifier.  No-op when it has no sat-bias states.
         * @param new_variance Requested diagonal (m²), a floor per state; falls back to
         *                  @c initial_std.sat_bias² when non-positive.
         */
        void reinflateSatBiasVariancesForSat(const std::string& satellite,
                                               double new_variance);

        /**
         * @brief Remove all satellite bias states except those listed in keep_keys.
         *
         * Pruned keys are stashed in the internal @c SatBiasCache (value, diagonal
         * variance, drop time), so a re-acquisition inside the configured window
         * can recover the converged estimate via @c tryRestoreSatBias.
         *
         * @param keep_keys Keys to keep.
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
         *    forward by @c process_noise.sat_bias_q × @c gap_s: a warm
         *    restart that preserves converged precision.
         *  - Between @c warm_age_s and @c max_age_s
         *    (@c is_warm == false), @c variance is the cold-start value
         *    @c initial_std.sat_bias²: the cache supplies a better seed
         *    than WLS, but the variance is reset.
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
         * From the cached entry's age @c gap_s = @c now − @c drop_time:
         *  - ≤ @c warm_age_s: a warm restoration, cached variance aged forward by
         *    @c sat_bias_q × @c gap_s.
         *  - ≤ @c max_age_s: a cold-value restoration, cached value at
         *    @c initial_std.sat_bias² variance.
         *  - beyond that: the entry is evicted and @c std::nullopt returned.
         *
         * Otherwise a pure lookup: the entry stays in the cache, and the caller
         * decides whether to consume it (@c consumeSatBiasCacheEntry).
         *
         * @param key  Sat-bias key "satellite|signal".
         * @param now  Current epoch time.
         * @return Restoration parameters, or @c std::nullopt if none is usable.
         */
        std::optional<SatBiasRestoration>
        tryRestoreSatBias(const std::string& key, const GnssTime& now);

        /**
         * @brief Drop a cache entry after the caller has restored it
         *        into an active state.
         *
         * Called by @c MeasurementModel once a lookup has been applied via
         * @c addSatBiasStateWithVariance.  Keeps the invariant that a key is in
         * either the active state vector or the cache, never both.
         *
         * @param key Sat-bias key to drop from the cache.
         */
        void consumeSatBiasCacheEntry(const std::string& key);

        // -------------------------------------------------------------------
        // Satellite phase-bias states (PPP_RTK_Network producer).
        //
        // Explicit per-(satellite, signal) satellite phase-bias states (Odijk
        // Eq. 4; each carries the network pivot-receiver ambiguity lump).  They
        // mirror the satellite code-bias block above but live in a separate map
        // (@c sat_phase_bias_indices_) keyed "<sat>|<signal>|PB".  The network
        // estimator (the shared satellite spine) creates and owns them; every
        // other mode leaves them empty.  The code-bias block's warm-restart
        // cache is not mirrored yet: the network's cross-epoch restart semantics
        // are decided with the estimator (W4), not by the single-receiver
        // arc-continuity cache.
        // -------------------------------------------------------------------

        /// @brief Index of a satellite phase-bias state, or -1 if absent.
        int getSatPhaseBiasIndex(const std::string& key) const;

        /// @brief Read-only map "satellite|signal|PB" -> state index.
        const std::map<std::string, size_t>& getSatPhaseBiasStates() const;

        /// @brief Append a satellite phase-bias state at cold-start variance
        ///        (@c initial_std.sat_phase_bias²).  Idempotent if the key
        ///        already exists.  Returns the state index.
        size_t addSatPhaseBiasState(const std::string& key, double initial_value);

        /// @brief Re-inflate the variances of all phase-bias states for a
        ///        satellite (value preserved, since the phase hardware delay is
        ///        constant), skipping frozen-pivot rows.  Each diagonal is raised
        ///        to @p new_variance, never lowered (card OWD).
        /// @param satellite    Satellite identifier.
        /// @param new_variance Requested diagonal (m²), a floor per state; falls back
        ///                     to @c initial_std.sat_phase_bias² when non-positive.
        void reinflateSatPhaseBiasVariancesForSat(const std::string& satellite,
                                                  double new_variance);

        /// @brief Prune satellite phase-bias states whose keys are not in
        ///        @p keep_keys (no warm-restart cache yet; see W4).
        void pruneSatPhaseBiasStatesExcept(const std::set<std::string>& keep_keys);

        // -------------------------------------------------------------------
        // Receiver phase-bias states (network.phase_datum_eq34; card 3).
        //
        // Per-(receiver, system, signal) receiver phase-bias split state
        // δ_{r,σ} (Odijk 2016 §4/§6; de Jonge 1998).  With its own estimable
        // state, the receiver fractional phase bias does not lump into the
        // emitted satellite phase bias, so the Eq-34 single-receiver / chain
        // datum can keep the emitted between-satellite reference
        // integer-consistent across all satellites.  Keyed
        // "<system>_RPB_<signal>" in the separate map
        // @c receiver_phase_bias_indices_ (the "_RPB_" infix keeps the keys
        // disjoint from the receiver code-bias family).  These states are local
        // (each is observed by one station) and are re-estimated fresh each
        // epoch as the receiver-side wander sink, with no process noise and no
        // cross-epoch carry.  In this split (Hou-Zhang), the integers stay
        // time-linked through the ambiguity carry and δ_{r,σ} does not.  Unused
        // in every non-network mode.
        // -------------------------------------------------------------------

        /// @brief Index of a receiver phase-bias state, or -1 if absent.
        int getReceiverPhaseBiasIndex(const std::string& key) const;

        /// @brief Read-only map "system_RPB_signal" -> state index.
        const std::map<std::string, size_t>& getReceiverPhaseBiasStates() const;

        /// @brief Append a receiver phase-bias state at cold-start variance
        ///        (@c initial_std.sat_phase_bias²).  Idempotent if the key
        ///        already exists.  Returns the state index.
        size_t addReceiverPhaseBiasState(const std::string& key, double initial_value);

        /// @brief Prune receiver phase-bias states whose keys are not in
        ///        @p keep_keys (no warm-restart cache: the states are re-estimated
        ///        fresh).
        void pruneReceiverPhaseBiasStatesExcept(const std::set<std::string>& keep_keys);

        // -------------------------------------------------------------------
        // Phase-colour states (network.phase_colour_state; card LGM,
        // docs/refound/LGM_los_phase_nuisance_2026-10-01.md section 4.1).
        //
        // One band-common, phase-only Gauss-Markov nuisance state per satellite of this
        // station: the line of sight's time-correlated phase error, which enters every
        // phase row of the satellite with +1 and no code row.  Keyed by the bare
        // satellite id in the separate map @c phase_colour_indices_ (labelled "PCOL:"),
        // seeded 0 and never written back by the network, so the linearization point
        // stays 0 and the solved local correction is the estimate itself.  The network
        // carries the estimate between epochs (NetworkCarryState::pc_carry); the
        // StateManager holds only the column.  Unused in every non-network mode.
        // -------------------------------------------------------------------

        /// @brief Index of a satellite's phase-colour state, or -1 if absent.
        int getPhaseColourIndex(const std::string& satellite) const;

        /// @brief Read-only map satellite -> phase-colour state index.
        const std::map<std::string, size_t>& getPhaseColourStates() const;

        /// @brief Append a phase-colour state for @p satellite at value 0 and variance
        ///        @p initial_var_m2.  Idempotent if the satellite already has one.
        /// @return The state index.
        size_t addPhaseColourState(const std::string& satellite, double initial_var_m2);

        /// @brief Prune the phase-colour states of satellites not in @p keep_sats.
        void prunePhaseColourStatesExcept(const std::set<std::string>& keep_sats);

        /// @brief Record the satellite's elevation (deg) for the state's prior law.
        void setPhaseColourElevation(const std::string& satellite, double elevation_deg);

        /// @brief Last recorded elevation (deg) of the satellite's phase-colour state,
        ///        or NaN when none was recorded.
        double getPhaseColourElevation(const std::string& satellite) const;

        // -------------------------------------------------------------------
        // Satellite clock states (PPP_RTK_Network producer).
        //
        // Explicit per-satellite clock-correction states (Δδtˢ in metres,
        // relative to the a-priori product clock already removed in obs.modeled)
        // kept in a separate map (@c sat_clock_indices_) keyed "<sat>|SC".  The
        // satellite orbit stays fixed from the precise/real-time product; only
        // the clock is estimated, to keep consistency with the chosen S-system
        // datum.  The network estimator (the shared satellite spine) creates and
        // owns these states; every other mode leaves them empty.  There is one
        // clock per satellite, not per signal, so the key has no signal field.
        // As for the phase-bias block, the warm-restart cache is not mirrored
        // yet; network cross-epoch restart is decided with the estimator.
        // -------------------------------------------------------------------

        /// @brief Index of a satellite clock state, or -1 if absent.
        int getSatClockIndex(const std::string& key) const;

        /// @brief Read-only map "satellite|SC" -> state index.
        const std::map<std::string, size_t>& getSatClockStates() const;

        /// @brief Append a satellite clock state at cold-start variance
        ///        (@c initial_std.sat_clock²).  Idempotent if the key already
        ///        exists.  Returns the state index.
        size_t addSatClockState(const std::string& key, double initial_value);

        /// @brief Re-inflate the variance of a satellite's clock state, skipping
        ///        a frozen-pivot row.  Value is preserved.  The diagonal is raised to
        ///        @p new_variance, never lowered (card OWD).
        /// @param satellite    Satellite identifier.
        /// @param new_variance Requested diagonal (m²), a floor; falls back to
        ///                     @c initial_std.sat_clock² when non-positive.
        void reinflateSatClockVarianceForSat(const std::string& satellite,
                                             double new_variance);

        /// @brief Prune satellite clock states whose keys are not in
        ///        @p keep_keys (no warm-restart cache yet; see W4).
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
         * row indices stay in place and only the key strings change to
         * reflect the new pivot.  The caller transforms the state value at
         * the row index separately (T·x); this method only re-binds the key
         * in @c ambiguity_indices_ and advances its continuity generation.
         * No covariance compaction; no index shifting.  Returns false if the
         * old key is not present or the new key already exists (the caller
         * resolves the collision).
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
         * @param keep_keys   Set of ambiguity keys to keep.
         * @param removed_out Optional sink; the keys this call removes are appended
         *                    to it.  The prune itself does not depend on the sink,
         *                    which only lets the arc ledger record that a column
         *                    disappeared inside a gap, so the arc's next re-entry can
         *                    state that it returns to a re-created column.  Null (the
         *                    default) records nothing and costs nothing (card SID
         *                    phase 3, R3b).
         */
        void pruneAmbiguitiesExcept(const std::set<std::string>& keep_keys,
                                    std::vector<std::string>* removed_out = nullptr);

        /**
         * @brief Arm the prune's key recorder (card SID phase 3, R3b).
         *
         * Off by default, so a run that does not ask for it allocates and copies
         * nothing. The network producer arms it on every run, with the card SID
         * roster and arc ledger, which are built on every network run since card NOM
         * step N3a, and drains @ref takePrunedAmbiguities once per epoch.
         */
        void setRecordPrunedAmbiguities(bool on) { record_pruned_ambiguities_ = on; }

        /**
         * @brief The sink @c MeasurementModel passes to @ref pruneAmbiguitiesExcept.
         *
         * Null unless @ref setRecordPrunedAmbiguities armed the recorder, so the
         * call site expresses the recording and the state manager owns the buffer.
         */
        std::vector<std::string>* ambiguityPruneSink() {
            return record_pruned_ambiguities_ ? &pruned_ambiguities_ : nullptr;
        }

        /// @brief Take and clear the keys pruned since the last drain.
        std::vector<std::string> takePrunedAmbiguities() {
            std::vector<std::string> out;
            out.swap(pruned_ambiguities_);
            return out;
        }

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
        // Covariance edits used by @c Application::handleGap when an
        // inter-epoch outage exceeds @c GnssConfig::getDataGapTolerance.
        // The process noise from @c getProcessNoiseMatrix suits short in-arc
        // dt steps, but adding noise cannot remove the satellite-specific
        // cross-covariances accumulated before the gap, which no longer
        // describe the current geometry.  Removing them takes explicit edits
        // to the off-diagonal blocks, which these helpers make.
        // -------------------------------------------------------------------

        /**
         * @brief Zero cross-covariances between position and per-satellite states.
         *
         * Sets P(pos_i, j) = P(j, pos_i) = 0 for j ∈ {ionosphere, iono rate,
         * ambiguity, satellite bias} and i ∈ {x,y,z}.  Leaves the position
         * diagonal P(pos,pos) and the cross-covariances to core states (clock,
         * clock-rate, troposphere, velocity, IMU biases) untouched: those
         * represent physical correlations that survive an outage, while
         * satellite-specific correlations do not.
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
        size_t core_state_size_;                    //!< Number of core states: position 3, clock 1 (not under BSSD), ZWD 1, then the optional velocity 3, clock rate 1, phase clock 1, gradients 2, IMU 9, one ISB and one ISBR per non-GPS system; baseline: position 3 (+1 differential ZWD).
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
        std::map<std::string, size_t> system_time_rate_offset_indices_; //!< Map system name -> ISBR state index (clock-rate inter-system offset; allocated when velocity is estimated and BSSD is off).
        std::map<std::string, size_t> ambiguity_indices_; //!< Map ambiguity key -> state index.
        /// Recorder for the keys @ref pruneAmbiguitiesExcept removes (card SID phase 3,
        /// R3b).  Off by default; only the three recorder accessors touch these members.
        bool record_pruned_ambiguities_{false};
        std::vector<std::string> pruned_ambiguities_;
        std::map<std::string, uint64_t> ambiguity_generations_; //!< Continuity generation for each live ambiguity.
        uint64_t next_ambiguity_generation_{ 0 }; //!< Monotone across removals; zero denotes a missing key.
        std::set<std::string> unresolvable_ambiguities_; //!< Ambiguity keys excluded from integer AR (incomplete product coverage).
        std::set<std::string> pending_reseed_ambiguities_; //!< Ambiguity keys awaiting re-seeding after reset.
        std::map<std::string, size_t> code_bias_indices_; //!< Map code bias key -> state index (uncombined processing with estimate_ifb).
        std::map<std::string, size_t> sat_bias_indices_; //!< Map "satellite|signal" -> state index (uncorrected sat HW bias).
        std::map<std::string, SatBiasCacheEntry> sat_bias_cache_; //!< Pruned sat-bias snapshots awaiting potential re-acquisition.
        std::map<std::string, size_t> sat_phase_bias_indices_; //!< Map "satellite|signal|PB" -> state index (PPP_RTK_Network satellite phase bias).
        std::map<std::string, size_t> receiver_phase_bias_indices_; //!< Map "system_RPB_signal" -> state index (eq34 receiver phase-bias split δ_{r,σ}; local, re-estimated fresh each epoch; card 3).
        std::map<std::string, size_t> phase_colour_indices_;  //!< Map satellite -> phase-colour state index (card LGM; network only).
        std::map<std::string, double> phase_colour_elevations_; //!< Map satellite -> last elevation (deg) of its phase-colour state (card LGM).
        std::map<std::string, size_t> sat_clock_indices_;      //!< Map "satellite|SC" -> state index (PPP_RTK_Network satellite clock).
        std::map<std::string, size_t> ionosphere_indices_; //!< Map satellite ID -> per-satellite iono state index (uncombined processing).
        std::map<std::string, size_t> iono_rate_indices_; //!< Map satellite ID -> per-satellite iono-rate (Idot_s) state index; populated only when enableIonoRate() (constant-velocity model).
        std::map<std::string, double> ionosphere_elevations_; //!< Map satellite ID -> last-known elevation in degrees (uncombined processing).
        std::map<std::string, double> ionosphere_azimuths_;   //!< Map satellite ID -> last-known azimuth in degrees (uncombined processing, diagnostics only).
        std::set<std::string> frozen_pivot_sats_; //!< BSSD pivot satellites: their iono, iono-rate, ambiguity, satellite code-bias, phase-bias and clock states get no Q.
        std::map<std::string, double> amb_q_overrides_; //!< Per-(amb_key) ambiguity process noise (m²/s); replaces process_noise.ambiguity for those keys when building Q.
        std::map<std::string, double> iono_q_overrides_; //!< Per-satellite zenith iono process noise (m^2/s); replaces process_noise.ionosphere for those sats when building Q.
        GnssTime state_epoch_time_;                 //!< Epoch time associated with the stored state.
        Eigen::VectorXd state_vector_;              //!< Filter state vector (core block + every dynamic family).
        Eigen::MatrixXd covariance_;                //!< State covariance matrix.
        LogManager& log_manager_;                   //!< Reference to logging manager.
        const GnssConfig& config_;                  //!< Read-only configuration reference.
        bool is_baseline_{ false };                 //!< True when configured for baseline (DD) mode: no clock/tropo/iono/system-offset states.
        std::map<std::string, int> last_ssr_discontinuity_; //!< Last-seen SSR phase-bias discontinuity counter per "sat|signal" key.
        ReinflateCounters reinflate_counters_{};    //!< Raised / kept counts per re-inflation (card OWD, O2).

        /**
         * @brief The rule every re-inflation shares (card OWD, O2).
         *
         * Zeroes row and column @p idx (the value is untouched) and writes the larger of
         * @p current_diag and @p requested on the diagonal, so a re-inflation never shrinks a
         * variance (writing the request over a larger variance would inject information the
         * data never supplied).  Counts the state under @p kind as raised (request ≥
         * current) or kept (current > request).
         *
         * @param idx          State index (validated by the caller, not frozen).
         * @param current_diag The current diagonal (> 0).
         * @param requested    The requested variance (> 0, after the caller's fallback).
         * @param kind         Which re-inflation, for the counters.
         * @return The diagonal written.
         */
        double reinflateDiag_(Eigen::Index idx, double current_diag, double requested,
                              Reinflation kind);

        // -------------------------------------------------------------------
        // Internal add and cache helpers (called only by this class)
        // -------------------------------------------------------------------

        /**
         * @brief Append a satellite phase-bias state with an explicit variance.
         *
         * The body of @ref addSatPhaseBiasState, which passes the cold-start
         * variance.  Idempotent if the key already exists.
         *
         * @param key              Key "satellite|signal|PB".
         * @param initial_value    Initial value (m).
         * @param initial_variance Initial diagonal (m²); a non-positive value falls
         *                         back to @c initial_std.sat_phase_bias².
         * @return The state index.
         */
        size_t addSatPhaseBiasStateWithVariance(const std::string& key,
                                                double initial_value,
                                                double initial_variance);

        /**
         * @brief Append a satellite clock state with an explicit variance.
         *
         * The body of @ref addSatClockState, which passes the cold-start variance.
         * Idempotent if the key already exists.
         *
         * @param key              Key "satellite|SC".
         * @param initial_value    Initial value (m).
         * @param initial_variance Initial diagonal (m²); a non-positive value falls
         *                         back to @c initial_std.sat_clock².
         * @return The state index.
         */
        size_t addSatClockStateWithVariance(const std::string& key,
                                            double initial_value,
                                            double initial_variance);

        /**
         * @brief Drop cache entries older than @c sat_bias_cache.max_age_s
         *        relative to @c now.
         *
         * Idempotent; called on each sat-bias prune so the cache does not
         * accumulate entries for a satellite/signal that never reacquires.
         *
         * @param now Current epoch time.
         */
        void evictExpiredSatBiasCache(const GnssTime& now);

        // -------------------------------------------------------------------
        // Construction steps (StateManagerLayout.cpp)
        //
        // The constructor lays out the baseline core and returns, or lays out the
        // PPP / PPP-RTK core, seeds its variances and logs the layout, in that
        // order.
        // -------------------------------------------------------------------

        /**
         * @brief Baseline (DD) mode: the core is position (3) plus the optional
         *        differential ZWD; allocates x and P, seeds their variances and
         *        logs the layout.
         *
         * Reads the constructor argument (kept in @c base_core_size_) as the
         * position block size; clears the inter-system offset maps.
         */
        void layoutBaseline_();

        /**
         * @brief PPP / PPP-RTK mode: assigns the core indices (position, clock when
         *        BSSD is off, ZWD, then velocity and clock rate, phase clock,
         *        gradients, IMU, one ISB and one ISBR per non-GPS system) and
         *        sets @c core_state_size_.
         */
        void layoutCore_();

        /**
         * @brief Allocates x (zero) and P (identity) at the core size and writes
         *        each allocated core state's initial variance from
         *        @c filter.initial_std (the IMU states from the IMU configuration).
         */
        void seedCoreVariances_();

        /**
         * @brief Logs the core layout (sizes per block, the BSSD note and the
         *        systems with an inter-system offset) on the "state_manager" logger.
         */
        void logLayout_();

        // -------------------------------------------------------------------
        // The blocks of the process noise matrix (StateManagerProcessNoise.cpp)
        //
        // getProcessNoiseMatrix calls one function per block, in this order.
        // Each writes its own entries of Q (no entry is written twice), so the
        // order of the calls does not change Q.
        // -------------------------------------------------------------------

        /**
         * @brief Q block of the position and velocity states.
         *
         * With velocity: the continuous white-noise acceleration (CWNA) block
         * driven by @c pn.acceleration_psd (dt^3/3, dt^2/2, dt with the pos/vel
         * cross terms).  Without: a position random walk @c pn.position * dt.
         *
         * @param Q  The process noise matrix being built (state-sized, zero where unwritten).
         * @param pn The configured process noise (@c filter.process_noise).
         * @param dt Time step in seconds (> 0).
         */
        void addMotionQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the baseline (DD) mode: the ambiguities and the
         *        optional differential ZWD.
         *
         * The ambiguity random walk takes the per-key overrides and, unlike
         * @ref addAmbiguityQ_, skips no frozen pivot (baseline has no BSSD pivot).
         *
         * @param Q          The process noise matrix being built.
         * @param pn         The configured process noise.
         * @param dt         Time step in seconds.
         * @param state_size The state size Q was allocated with.
         */
        void addBaselineQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt,
                           size_t state_size) const;

        /**
         * @brief Q block of the receiver clock: the CWNA clock/clock-rate pair when
         *        the clock rate is estimated, else a clock random walk.
         *
         * Writes nothing under BSSD, where the constructor allocates neither state.
         *
         * @param Q  The process noise matrix being built.
         * @param pn The configured process noise.
         * @param dt Time step in seconds.
         */
        void addReceiverClockQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the phase clock offset: a random walk, none under BSSD.
         * @param Q  The process noise matrix being built.
         * @param pn The configured process noise.
         * @param dt Time step in seconds.
         */
        void addPhaseClockQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the troposphere: the ZWD random walk and, when
         *        estimated, the two gradient random walks.
         * @param Q  The process noise matrix being built.
         * @param pn The configured process noise.
         * @param dt Time step in seconds.
         */
        void addTroposphereQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the slant ionosphere and its rate.
         *
         * Applied only in @c ProcessingMode::PPP_RTK (card UOM defect S1).  Per
         * satellite: the zenith spectral density (or its adaptive override) over
         * max(sin^2 el, 0.01), as a random walk, or as the CWNA level/rate pair when
         * the rate state exists.  Frozen pivots get none.
         *
         * @param Q  The process noise matrix being built.
         * @param pn The configured process noise.
         * @param dt Time step in seconds.
         */
        void addIonosphereQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the receiver code biases: a random walk; under BSSD only
         *        the GLONASS per-slot IFB keys keep it.
         * @param Q  The process noise matrix being built.
         * @param pn The configured process noise.
         * @param dt Time step in seconds.
         */
        void addReceiverCodeBiasQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of one satellite signal family (satellite code bias, phase
         *        bias, clock): the random walk of the family's table row,
         *        @c pn.*q_psd * dt, none for a state whose satellite is a frozen pivot.
         *
         * One function for the three blocks that were written three times (card UOM
         * step U6e); the field and the satellite rule come from @ref kStateFamilies.
         *
         * @param Q      The process noise matrix being built.
         * @param family The family (its row has @c QRule::RandomWalk).
         * @param pn     The configured process noise.
         * @param dt     Time step in seconds.
         */
        void addSatelliteRandomWalkQ_(Eigen::MatrixXd& Q, StateFamily family,
                                      const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the inter-system time offsets: the CWNA ISB/ISBR pairs
         *        when the rate offsets exist, else an ISB random walk.
         * @param Q  The process noise matrix being built.
         * @param pn The configured process noise.
         * @param dt Time step in seconds.
         */
        void addSystemTimeOffsetQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt) const;

        /**
         * @brief Q block of the ambiguities: the random walk @c pn.ambiguity (or the
         *        per-key override); a frozen pivot's ambiguities get none.
         * @param Q          The process noise matrix being built.
         * @param pn         The configured process noise.
         * @param dt         Time step in seconds.
         * @param state_size The state size Q was allocated with.
         */
        void addAmbiguityQ_(Eigen::MatrixXd& Q, const ProcessNoise& pn, double dt,
                            size_t state_size) const;

        /**
         * @brief Whether a key's satellite is a frozen BSSD pivot
         *        (@ref setFrozenPivotSatellites), read by the family's satellite rule
         *        (@ref StateFamilySpec::sat_of_key).
         *
         * The process-noise and transition builders skip such states.
         *
         * @param family The family of @p key.
         * @param key    A key of that family.
         * @return True when the key names a satellite and it is a frozen pivot.
         */
        bool isFrozenPivotKey_(StateFamily family, const std::string& key) const;

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
         * After removal, every entry with index > rem_idx in the map of every
         * dynamic family (@ref kStateFamilies) is decremented by one.  The new
         * size comes from getStateSize(), which counts the map entries, so the
         * caller erases the removed row's map entry after this call, not before.
         *
         * @param rem_idx Zero-based index to remove (must be >= core_state_size_).
         */
        void removeStateAtIndex(size_t rem_idx);

        /**
         * @brief Mutable access to the key->index map of one dynamic family.
         * @param family The family.
         * @return The family's map; the only place that names the maps by family.
         */
        std::map<std::string, size_t>& indicesOf_(StateFamily family);
    };

} // namespace GnssProcessor
