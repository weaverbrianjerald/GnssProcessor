
#pragma once

/**
 * @file StateInitializer.hpp
 * @brief First-epoch state seeding for inter-system biases, phase clock, code
 *        biases, and the (network) cold receiver clock.
 *
 * StateInitializer encapsulates the one-shot initialization logic that runs
 * on the first epoch to seed ISB, phase clock offset, receiver DCB/IFB, and
 * — on the network path, opt-in — the cold receiver clock, from code and
 * phase observation residuals.  Extracting this from MeasurementModel keeps
 * the per-epoch hot path readable and isolates the initialization-specific
 * helpers (computeRawResidual, resolveReceiverBias, computeIonosphereSeed,
 * computeMedian) in one place.
 */

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Forward declarations
namespace GnssProcessor {
class PppModel;
}

namespace GnssProcessor {

/**
 * @class StateInitializer
 * @brief Seeds first-epoch filter states from observation residuals.
 *
 * Owns the initialization flags and provides four entry points:
 *   - initializeInterSystemBiases()
 *   - initializePhaseClockOffset()
 *   - initializeCodeBiases()
 *   - seedColdReceiverClock()   (network cold-start clock leak; opt-in)
 *
 * Also exposes helpers used by MeasurementModel during the state pre-pass
 * and Jacobian assembly (computeIonosphereSeed, resolveReceiverBias,
 * computeRawResidual).
 */
class StateInitializer {
public:
    StateInitializer(const GnssConfig& config,
                     const PppModel& ppp_model,
                     StateManager& state_manager,
                     LogManager& log_manager);

    // -------------------------------------------------------------------
    // First-epoch initialization entry points
    // -------------------------------------------------------------------

    /**
     * @brief Initialize inter-system biases from first-epoch code observations.
     *
     * Computes the median code residual per system relative to GPS.
     *
     * @return @c true if initialization completed (GPS reference present, so the
     *         ISB seeds are final); @c false if it bailed because no GPS code
     *         observation was available this epoch.  The caller must only latch
     *         "ISB initialized" on @c true — otherwise a run whose GPS joins
     *         after the first estimating epoch marks the bias done-but-unseeded
     *         and never retries, pinning every non-GPS ISB at 0 for the session.
     */
    bool initializeInterSystemBiases(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix);

    /**
     * @brief Seed the phase clock offset from the codephase residual difference.
     */
    void initializePhaseClockOffset(const std::vector<PppObservation>& observations,
                                     const std::string& epoch_prefix);

    /**
     * @brief Seed DCB/IFB states from cross-band code residual differences.
     *
     * Must be called after ensureRequiredStates() has created the code-bias
     * states and after ionosphere states have been seeded.
     */
    void initializeCodeBiases(const std::vector<PppObservation>& observations,
                               const std::string& epoch_prefix);

    /**
     * @brief Pre-seed a genuinely COLD receiver clock from GPS code residuals.
     *
     * Writes the median GPS code residual — the GPS-referenced receiver clock
     * (ISB carries the inter-system offsets, so GPS-only is the correct datum)
     * — into @c x(getClockIndex()).  On the PPP-RTK NETWORK path nothing else
     * pre-solves the receiver clock before @c ensureRequiredStates first seeds
     * ambiguities, so without this the whole free-running clock offset leaks
     * uniformly into every cold ambiguity seed (see
     * @c GnssConfig::networkColdClockSeed).  Call BEFORE the ambiguity-seed
     * loop (and after the iono seed, so @c computeRawResidual can remove the
     * seeded slant iono from the code residual) and ONCE per station.
     *
     * Self-limiting: only acts while the clock is genuinely cold (state == 0),
     * so it never overwrites a clock already seeded by the PPP path's
     * @c StateManager::syncPppStateToStateVector.
     *
     * @return @c true when the pre-seed is settled and the caller should latch
     *         it (clock seeded from GPS code, OR already non-cold ⇒ nothing to
     *         do); @c false when the clock is cold but NO GPS code observation
     *         was available this epoch (retry next epoch — do NOT latch, mirror
     *         of @ref initializeInterSystemBiases's GPS-reference contract).
     */
    bool seedColdReceiverClock(const std::vector<PppObservation>& observations,
                                const std::string& epoch_prefix);

    // -------------------------------------------------------------------
    // Initialization flags (queried/set by MeasurementModel orchestrator)
    // -------------------------------------------------------------------

    bool isIsbInitialized()        const { return isb_initialized_; }
    bool isPhaseClockInitialized() const { return phase_clock_initialized_; }
    bool isCodeBiasInitialized()   const { return code_bias_initialized_; }
    bool isClockSeedInitialized()  const { return clock_seed_initialized_; }

    void markIsbInitialized()        { isb_initialized_ = true; }
    void markPhaseClockInitialized() { phase_clock_initialized_ = true; }
    void markCodeBiasInitialized()   { code_bias_initialized_ = true; }
    void markClockSeedInitialized()  { clock_seed_initialized_ = true; }

    // -------------------------------------------------------------------
    // Shared helpers (used by MeasurementModel for state pre-pass / Jacobian)
    // -------------------------------------------------------------------

    /**
     * @brief Resolved receiver-bias state for one observation signal.
     */
    struct ReceiverBiasEntry {
        std::string state_key;   /**< Key into code_bias_indices_ (e.g. "G_DCB" or "G_IFB5"). */
        double      coefficient; /**< Jacobian coefficient for the state.  NaN ? no bias modelled. */
    };

    /**
     * @brief Determine the receiver-bias state and coefficient for a signal.
     */
    ReceiverBiasEntry resolveReceiverBias(const std::string& obs_signal,
                                           System system,
                                           const std::string& satellite) const;

    /**
     * @brief Compute a "raw" residual for an observation/ref_type pair.
     *
     * residual = observed - modeled - rx_clk - mf_w*zwd - isb - tropo_grad
     *          - iono - code_bias - sat_bias
     */
    double computeRawResidual(const PppObservation& obs,
                               const std::string& ref_type,
                               const Eigen::VectorXd& x) const;

    /**
     * @brief Compute ionosphere seed from dual-frequency code pseudoranges.
     *
     * Only code signals with valid (non-zero, in-product) BIA coverage are
     * used.  `(P - mod)` for an uncovered signal carries the raw satellite
     * code bias, which would pollute the iono seed by tens of metres.
     * Returns std::nullopt when no usable pair of bands is available — the
     * caller should fall back to seed=0 with an inflated initial variance
     * so the filter doesn't trust the zero too tightly.
     */
    std::optional<double> computeIonosphereSeed(const PppObservation& obs) const;

    // -------------------------------------------------------------------
    // Card LV-1 — bias-free per-LOS iono seeding
    // (docs/SPEC_level_information_budget.md)
    // -------------------------------------------------------------------

    /**
     * @brief Per-station, per-epoch inputs for the LV-1 seed reframe.
     *
     * Installed by the NETWORK producer immediately before the PASS-B state
     * pre-pass (@c MeasurementModel::computeMeasurementUpdate) and CONSUMED ONCE
     * — the pre-pass disarms it — so a stale frame can never be applied to a
     * later epoch's allocation.
     */
    struct IonoSeedFrame {
        /// satellite → the field's slant value at THAT line of sight's pierce
        /// point, in metres of L1 slant delay
        /// (@f$\sum_l M_l(el)\cdot\text{tecu\_to\_L1m}\cdot \mathrm{SH}_l(IPP_l)@f$).
        /// Built by the producer from the LAG-1 solved SH field, or from the a₀₀
        /// climatology prior mean before the first solve.
        std::map<std::string, double> field_slant_m;
        /// Minimum usable raw seeds at this station-epoch for the median offset.
        int min_sats = 5;
        /// Station name — used once, for the cold-start offset log line.
        std::string station;
    };

    /// LV-1 run tallies (standing invariant #4: an armed-but-inert reframe must
    /// never read as working).  Summed over stations at the run-end readout.
    struct IonoSeedFrameStats {
        long long reframed = 0;      ///< seeds born in the field's frame.
        long long fallback_raw = 0;  ///< no station offset (< min_sats) ⇒ RAW seed.
        long long field_missing = 0; ///< offset OK but no field value for that LOS ⇒ RAW seed.
        long long offsets = 0;       ///< station-epochs with a usable median offset.
        double last_offset_m = 0.0;  ///< the most recent median offset (m of L1 slant GF).
    };

    // -------------------------------------------------------------------
    // RS-9 — the code-bias LINEARIZATION POINT restore
    // (docs/SPEC_event_harness.md §21.18)
    // -------------------------------------------------------------------

    /**
     * @brief Install the writer's code-bias linearization points for this
     *        station, to be used INSTEAD of @ref initializeCodeBiases' one-shot
     *        median seed on the station's first processed epoch.
     *
     * WHY.  @ref initializeCodeBiases seeds @c x(cb) ONCE, from ONE epoch of
     * code, behind the one-shot @c isCodeBiasInitialized() latch, and nothing
     * ever writes it back.  That value is a pure LINEARIZATION POINT for the
     * solve (a code bias enters linearly, so shifting it is an exact solve
     * no-op — project_network_xlin_invariance), but it is also the reference the
     * network producer's rx-CB carry inject gate measures its innovation
     * against (@c cb_innov_raw = @c value_abs − @c x_lin(cb), the 5.13 m bar at
     * @c ApplicationNetworkAssembly.cpp:3954).  On a warm resume that reference
     * is re-derived from the RESUME epoch's code while the carry's numerator is
     * restored from the snapshot, so 17 of 61 field stations sat outside the bar
     * every epoch of a leg against the continuous run's 7 (§21.18(a)/(b)) and
     * lost their receiver-DCB memory for the whole leg.  Restoring the frame the
     * gate judges against is the re-coupling edge RS-6 left open (invariant #7).
     *
     * Contract: a key PRESENT here is seeded to @p overrides' value and counted
     * in @ref codeBiasSeedOverrideApplied; a key the samples loop reaches that is
     * NOT present falls back to today's median seed and is counted in
     * @ref codeBiasSeedOverrideUncovered.  Install BEFORE the station's first
     * @c computeMeasurementUpdate; it is read only under the one-shot latch, so
     * a later install is inert by construction.
     *
     * @param overrides  CB state key ("GPS_DCB", "GPS_IFB5", …) → the writer's
     *                   @c x_lin for that state, in metres.
     * @param applied    Optional out-slot incremented per covered key seeded.
     * @param uncovered  Optional out-slot incremented per uncovered key.
     *
     * @note The two out-slots are RAW pointers on purpose: the network producer
     *       runs @c computeMeasurementUpdate inside an OpenMP parallel-for over
     *       stations, so each station is handed a pointer to its OWN element of
     *       a driver-owned, never-resized vector — distinct objects, no race and
     *       no atomics.  The driver sums them serially at the run-end readout.
     */
    void setCodeBiasSeedOverride(std::map<std::string, double> overrides,
                                 long long* applied = nullptr,
                                 long long* uncovered = nullptr) {
        code_bias_seed_override_ = std::move(overrides);
        cb_seed_override_applied_ = applied;
        cb_seed_override_uncovered_ = uncovered;
    }
    /// Entries installed by @ref setCodeBiasSeedOverride (0 = nothing restored).
    std::size_t codeBiasSeedOverrideSize() const { return code_bias_seed_override_.size(); }

    /**
     * @brief The RS-9 seed decision for ONE code-bias key.
     *
     * STATIC and free of member state on purpose: the rule is what a unit test
     * must be able to pin, and constructing a @c StateInitializer drags in
     * @c PppModel → @c ProductParser + @c RinexParser, a chain no test at this
     * tier builds.  @ref initializeCodeBiases calls exactly this.
     *
     * @param overrides  The installed map (empty ⇒ always @c std::nullopt, which
     *                   is the whole pre-card path).
     * @param key        CB state key being seeded.
     * @return The writer's @c x_lin when the key is covered by a FINITE entry;
     *         @c std::nullopt ⇒ UNCOVERED, and the caller takes today's
     *         one-epoch median seed.  A non-finite entry is treated as uncovered
     *         rather than trusted — the parser already refuses one, so this is
     *         belt-and-braces against a future producer of the map.
     */
    static std::optional<double> resolveCodeBiasSeedOverride(
        const std::map<std::string, double>& overrides, const std::string& key) {
        if (overrides.empty()) return std::nullopt;
        const auto it = overrides.find(key);
        if (it == overrides.end() || !std::isfinite(it->second)) return std::nullopt;
        return it->second;
    }

    /// Install this epoch's frame (consumed once by the next state pre-pass).
    void setIonoSeedFrame(IonoSeedFrame frame) {
        iono_seed_frame_ = std::move(frame);
        iono_seed_frame_armed_ = true;
    }
    /// Drop any installed frame (the pre-pass calls this after consuming it).
    void clearIonoSeedFrame() { iono_seed_frame_armed_ = false; }
    bool ionoSeedFrameArmed() const { return iono_seed_frame_armed_; }
    const IonoSeedFrame& ionoSeedFrame() const { return iono_seed_frame_; }
    const IonoSeedFrameStats& ionoSeedFrameStats() const { return iono_seed_stats_; }

    /**
     * @brief The station-common content of this epoch's raw iono seeds.
     *
     * The MEDIAN of @p seeds — robust to a single bad channel, which a mean is
     * not; the geometry-free code seed of one mis-tracked satellite can be tens
     * of metres.  Physically this is (receiver GF hardware bias ⊕ the station's
     * median slant ionosphere).
     *
     * @param seeds    Raw per-satellite seeds (REORDERED in place by the median).
     * @param min_sats Minimum sample count; below it there is no robust estimate.
     * @return The median, or @c std::nullopt when @c seeds.size() < min_sats.
     */
    static std::optional<double> stationSeedOffset(std::vector<double>& seeds,
                                                    int min_sats);

    /**
     * @brief Gather this station's raw per-satellite seeds and reduce to the
     *        median offset.
     *
     * Runs over ALL of @p observations (not only the ones being allocated), so a
     * single mid-run re-birth still gets the full-station statistic.  The
     * @c |seed| < 200 m sanity check inside @ref computeIonosphereSeed screens
     * each contribution, so a garbage channel is rejected BEFORE it can move the
     * median.
     *
     * Not const: it tallies the outcome and emits the ONE cold-start log line
     * carrying the station's offset (a direct estimate of that receiver's GF
     * hardware bias — cross-checkable against @c scripts/extcb_gim_verdict.py).
     */
    std::optional<double> computeStationSeedOffset(
        const std::vector<PppObservation>& observations, int min_sats);

    /**
     * @brief Apply the installed frame to one raw seed (and tally the outcome).
     *
     *   seed' = raw − station_offset + field_slant(IPP)
     *
     * Falls back to @p raw_seed (counted) when the station offset is absent
     * (< @c min_sats usable seeds) or the frame carries no field value for this
     * satellite — a handful of bias-in lines of sight cannot move the field.
     *
     * @param satellite       The LOS's satellite id.
     * @param raw_seed        The seed @ref computeIonosphereSeed returned.
     * @param station_offset  The station median from @ref computeStationSeedOffset.
     * @return The seed to allocate the state with.
     */
    double applyIonoSeedFrame(const std::string& satellite, double raw_seed,
                               const std::optional<double>& station_offset);

    /**
     * @brief Map a System enum to its constellation name for ISB lookup.
     */
    static std::string systemToIsbName(System system);

    /**
     * @brief Map a System enum to the single-char BIA clock reference identifier.
     */
    static char systemToClockRefChar(System system);

private:
    const GnssConfig& config_;
    const PppModel& ppp_model_;
    StateManager& state_manager_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;

    bool isb_initialized_ = false;
    bool phase_clock_initialized_ = false;
    bool code_bias_initialized_ = false;
    bool clock_seed_initialized_ = false;

    // Card LV-1 state.  Disarmed by default, so a PPP run (and any network run
    // with network.iono_seed_field_frame off) never enters the reframe path.
    IonoSeedFrame      iono_seed_frame_{};
    bool               iono_seed_frame_armed_ = false;
    bool               iono_seed_offset_logged_ = false;   ///< cold-start one-shot log latch.
    IonoSeedFrameStats iono_seed_stats_{};

    // RS-9 state (§21.18).  Empty by default, so every run that does not arm
    // network.resume_rxcb_lin_restore takes the pre-card path with no branch
    // taken and no counter touched.
    std::map<std::string, double> code_bias_seed_override_{};
    long long* cb_seed_override_applied_   = nullptr;  ///< station-owned out-slot (see setCodeBiasSeedOverride).
    long long* cb_seed_override_uncovered_ = nullptr;  ///< ditto.
};

} // namespace GnssProcessor
