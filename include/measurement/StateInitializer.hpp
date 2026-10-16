
#pragma once

/**
 * @file StateInitializer.hpp
 * @brief First-epoch state seeding for inter-system biases, phase clock, code
 *        biases, and the (network) cold receiver clock.
 *
 * StateInitializer holds the one-shot initialization logic that runs on the
 * first epoch and seeds, from code and phase observation residuals, the ISB,
 * the phase clock offset, the receiver DCB/IFB and (on the network path, opt-in)
 * the cold receiver clock.  Keeping it out of MeasurementModel keeps the
 * per-epoch hot path readable and gathers the initialization helpers
 * (computeRawResidual, resolveReceiverBias, computeIonosphereSeed,
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
     *         ISB seeds are final); @c false if no GPS code observation was
     *         available this epoch.  Latch "ISB initialized" on @c true only.
     *         Latching on @c false would mark the bias done without a seed when
     *         GPS joins after the first estimating epoch, and every non-GPS ISB
     *         would then stay at 0 for the session.
     */
    bool initializeInterSystemBiases(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix);

    /**
     * @brief Seed the phase clock offset from the code-minus-phase residual difference.
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
     * @brief Pre-seed a cold receiver clock from GPS code residuals.
     *
     * Writes the median GPS code residual, which is the GPS-referenced receiver
     * clock, into @c x(getClockIndex()).  GPS alone is the correct datum because
     * the ISB states carry the inter-system offsets.  On the PPP-RTK network path
     * nothing else pre-solves the receiver clock before @c ensureRequiredStates
     * first seeds ambiguities; without this seed the whole free-running clock
     * offset enters every cold ambiguity seed uniformly (see
     * @c GnssConfig::networkColdClockSeed).  Call it once per station, before the
     * ambiguity-seed loop and after the iono seed (so @c computeRawResidual can
     * remove the seeded slant iono from the code residual).
     *
     * Acts only while the clock is cold (|state| at most 1e-6 m), so it never
     * overwrites a clock already seeded by the PPP path's
     * @c StateManager::syncPppStateToStateVector.
     *
     * @return @c true when the pre-seed is settled and the caller should latch it
     *         (clock seeded from GPS code, or already not cold, or no clock state
     *         in this configuration); @c false when the clock is cold and no GPS
     *         code observation was available this epoch.  On @c false, do not
     *         latch; retry next epoch (the same contract as
     *         @ref initializeInterSystemBiases).
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
        std::string state_key;   /**< Key into code_bias_indices_ (e.g. "GPS_DCB" or "GPS_IFB5"). */
        double      coefficient; /**< Jacobian coefficient for the state.  NaN means no bias is modelled. */
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
     * code bias, which would bias the iono seed by tens of metres.
     * Returns the first seed from a pair of distinct bands that is finite and
     * below 200 m in magnitude, or std::nullopt when no such pair exists.  On
     * std::nullopt the caller falls back to seed = 0 with an inflated initial
     * variance, so the filter does not weight the zero too tightly.
     */
    std::optional<double> computeIonosphereSeed(const PppObservation& obs) const;

    // -------------------------------------------------------------------
    // Code-bias linearization point restore (RS-9)
    // (docs/SPEC_event_harness.md §21.18)
    // -------------------------------------------------------------------

    /**
     * @brief Install the writer's code-bias linearization points for this
     *        station, used in place of @ref initializeCodeBiases' one-shot
     *        median seed on the station's first processed epoch.
     *
     * Rationale.  @ref initializeCodeBiases seeds @c x(cb) once, from one epoch
     * of code, behind the one-shot @c isCodeBiasInitialized() latch, and nothing
     * writes it back.  For the solve that value is only a linearization point (a
     * code bias enters linearly, so shifting it is an exact solve no-op;
     * project_network_xlin_invariance).  It is also the reference the network
     * producer's rx-CB carry inject gate measures its innovation against
     * (@c cb_innov = @c value_abs − @c x_lin(cb), against the 5.13 m bar of
     * the @c rxCbInjectGate call in @c ApplicationNetworkAssembly.cpp).  On a warm
     * resume that reference is re-derived from the resume epoch's code while the
     * carry's numerator is restored from the snapshot, so stations can sit outside
     * the bar for a whole leg and lose their receiver-DCB memory
     * (§21.18(a)/(b)).  Restoring the reference the gate compares against covers
     * the re-coupling edge of the warm resume (invariant #7) that RS-6 left open.
     *
     * Contract: a key present here is seeded to @p overrides' value and counted in
     * @c applied.  A key the loop reaches that is absent takes the usual median
     * seed and is not counted.  The RL rows pair 1:1 with the RC rows, so an
     * absent key is one with no rx-CB carry, whose @c x_lin the inject gate never
     * reads (a field station holds @c GPS_DCB, which is carried, and
     * @c GPS_IFB5, which is not).  @c uncovered counts the case that matters: a
     * key that has a carry and whose restored reference could not be applied (a
     * non-finite value, or a state index this run does not hold).  It should read
     * 0 (invariant #4).
     *
     * Install before the station's first @c computeMeasurementUpdate.  The map is
     * read only under the one-shot latch, so a later install has no effect.
     *
     * @param overrides  CB state key ("GPS_DCB", "GPS_IFB5", …) → the writer's
     *                   @c x_lin for that state, in metres.
     * @param applied    Optional out-slot incremented per covered key seeded.
     * @param uncovered  Optional out-slot incremented per covered key that could
     *                   not be seeded (see above); never for an absent key.
     *
     * @note The two out-slots are raw pointers because the network producer runs
     *       @c computeMeasurementUpdate inside an OpenMP parallel-for over
     *       stations.  Each station receives a pointer to its own element of a
     *       driver-owned vector that is never resized, so the objects are
     *       distinct and need no atomics.  The driver sums them serially at the
     *       run-end readout.
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
     * @brief The RS-9 seed decision for one code-bias key.
     *
     * Static and free of member state so that a unit test can pin the rule:
     * constructing a @c StateInitializer requires @c PppModel, which pulls in
     * @c ProductParser and @c RinexParser, and no test at this tier builds that
     * chain.  @ref initializeCodeBiases calls this function.
     *
     * @param overrides  The installed map (empty ⇒ always @c std::nullopt, which
     *                   is the path taken when RS-9 is not armed).
     * @param key        CB state key being seeded.
     * @return The writer's @c x_lin when the key is covered by a finite entry;
     *         @c std::nullopt otherwise, and the caller takes the one-epoch
     *         median seed.  A non-finite entry is refused.  The parser already
     *         refuses one; this check guards any other producer of the map.  Use
     *         @ref codeBiasSeedOverrideCovers to tell the two @c nullopt cases
     *         apart: an absent key has no carry (nothing reads its reference),
     *         while a present but refused key is the counted failure.
     */
    static std::optional<double> resolveCodeBiasSeedOverride(
        const std::map<std::string, double>& overrides, const std::string& key) {
        if (overrides.empty()) return std::nullopt;
        const auto it = overrides.find(key);
        if (it == overrides.end() || !std::isfinite(it->second)) return std::nullopt;
        return it->second;
    }

    /**
     * @brief Whether the installed map carries a reference for @p key at all.
     *
     * Defines the population @c uncovered is measured against.  The RL rows pair
     * 1:1 with the RC rows, so presence in the map means the key has an rx-CB
     * carry entry, and therefore that the inject gate differences something
     * against this key's @c x_lin.  A key the seeding loop reaches that is absent
     * here is not a miss: it is a code-bias family that nothing carries.
     */
    static bool codeBiasSeedOverrideCovers(const std::map<std::string, double>& overrides,
                                           const std::string& key) {
        return overrides.find(key) != overrides.end();
    }

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

    // State of the RS-9 code-bias restore (§21.18).  Empty by default, so a run that does not arm
    // network.resume_rxcb_lin_restore takes the median-seed path, with no override
    // branch taken and no counter touched.
    std::map<std::string, double> code_bias_seed_override_{};
    long long* cb_seed_override_applied_   = nullptr;  ///< station-owned out-slot (see setCodeBiasSeedOverride).
    long long* cb_seed_override_uncovered_ = nullptr;  ///< station-owned out-slot (see setCodeBiasSeedOverride).
};

} // namespace GnssProcessor
