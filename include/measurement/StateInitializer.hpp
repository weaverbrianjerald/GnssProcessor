
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
};

} // namespace GnssProcessor
