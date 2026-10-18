
#pragma once

/**
 * @file StateInitializer.hpp
 * @brief The one-shot latches of the first-epoch state seeding, the receiver code-bias
 *        resolver, and the warm resume's code-bias restore.
 *
 * The values of the seeds (inter-system biases, phase clock, receiver DCB/IFB, the cold
 * receiver clock, the slant ionosphere, the ambiguities) are pure functions of the observation
 * equation in measurement/Seeds.hpp; the single-receiver model applies them to its
 * @c StateManager (MeasurementModelSeeds.cpp).  StateInitializer keeps what is not a function
 * of the observations and the parameters: which seeds have run (the latches), which receiver
 * code-bias state and coefficient a signal maps to (@ref resolveReceiverBias), the
 * linearization points a warm resume installs in place of the median code-bias seed, and the
 * frequency helper the seeds scale the ionosphere by.  It holds no state vector.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "common/ParamRef.hpp"
#include "common/PppTypes.hpp"
#include "common/PppUtils.hpp"
#include <cmath>
#include <map>
#include <optional>
#include <string>

// Forward declarations
namespace GnssProcessor {
class PppModel;
}

namespace GnssProcessor {

/**
 * @class StateInitializer
 * @brief The latches of the first-epoch seeding and the receiver code-bias resolver.
 *
 * Owns the initialization flags, the resolver used by the Jacobian row and the code-bias seed,
 * the code-bias restore of a warm resume, and the frequency helper of the seeds.
 */
class StateInitializer {
public:
    StateInitializer(const GnssConfig& config,
                     const PppModel& ppp_model,
                     LogManager& log_manager);

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
        RxCbKey     key{};       /**< The same state as a typed key (valid when @ref has_key). */
        bool        has_key{false}; /**< True exactly when @ref state_key is non-empty. */
    };

    /**
     * @brief Determine the receiver-bias state and coefficient for a signal.
     */
    ReceiverBiasEntry resolveReceiverBias(const std::string& obs_signal,
                                           System system,
                                           const std::string& satellite) const;

    // -------------------------------------------------------------------
    // Code-bias linearization point restore (RS-9)
    // (docs/SPEC_event_harness.md §21.18)
    // -------------------------------------------------------------------

    /**
     * @brief Install the writer's code-bias linearization points for this
     *        station, used in place of the one-shot median seed
     *        (@c seedCodeBias) on the station's first processed epoch.
     *
     * Rationale.  The code-bias seed (@c MeasurementModel::initializeCodeBiases)
     * seeds @c x(cb) once, from one epoch
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

    /// The installed linearization points by key text (empty: none).
    const std::map<std::string, double>& codeBiasSeedOverride() const {
        return code_bias_seed_override_;
    }

    /**
     * @brief Counts the outcome of one covered key of the restore.
     *
     * The seeding function decides which keys the installed map applies to
     * (@c seedCodeBias); the model reports each to the station's out-slots here.
     *
     * @param applied True when the key was seeded from the map, false when the map covered
     *                it but its value could not be used.
     */
    void noteCodeBiasRestore(bool applied) {
        long long* slot = applied ? cb_seed_override_applied_ : cb_seed_override_uncovered_;
        if (slot != nullptr) ++*slot;
    }

    /**
     * @brief The frequency helper the seeds scale the slant ionosphere by.
     *
     * Its GLONASS channels are the default table, never the receiver's (nothing seeds
     * them), which is the number the seeds have always used; the model's own helper
     * (seeded from the RINEX header) belongs to the Jacobian row.
     *
     * @return The helper.
     */
    const PppUtils& frequencyHelper() const { return ppp_utils_; }

    /**
     * @brief The RS-9 seed decision for one code-bias key.
     *
     * Static and free of member state so that a unit test can pin the rule:
     * constructing a @c StateInitializer requires @c PppModel, which pulls in
     * @c ProductParser and @c RinexParser, and no test at this tier builds that
     * chain.  @c seedCodeBias calls this function.
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
