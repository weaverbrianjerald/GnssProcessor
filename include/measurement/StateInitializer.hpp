
#pragma once

/**
 * @file StateInitializer.hpp
 * @brief The one-shot latches of the first-epoch state seeding, the receiver code-bias
 *        resolver, and the rule that picks a restored code-bias linearization point.
 *
 * The values of the seeds (inter-system biases, phase clock, receiver DCB/IFB, the cold
 * receiver clock, the slant ionosphere, the ambiguities) are pure functions of the observation
 * equation in measurement/Seeds.hpp; the single-receiver model applies them to its
 * @c StateManager (MeasurementModelSeeds.cpp).  StateInitializer keeps what is not a function
 * of the observations and the parameters: which seeds have run (the latches), which receiver
 * code-bias state and coefficient a signal maps to (@ref resolveReceiverBias), the rule that
 * decides whether a restored linearization point replaces the median code-bias seed
 * (@ref resolveCodeBiasSeedOverride), and the frequency helper the seeds scale the ionosphere
 * by.  It holds no state vector.  The single-receiver model never receives a restore: the
 * network producer's warm resume installs its points on the station's parameter set
 * (@c StationParams::rx_cb_restore, network/state/StationParams.hpp).
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "common/ParamRef.hpp"
#include "common/PppTypes.hpp"
#include "common/PppUtils.hpp"
#include "measurement/ReceiverBias.hpp"
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
 * and the frequency helper of the seeds; its static rules serve the user-side and the network
 * code-bias seeds alike.
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

    /// The receiver code-bias parameter of one code signal (measurement/ReceiverBias.hpp).
    using ReceiverBiasEntry = GnssProcessor::ReceiverBiasEntry;

    /**
     * @brief Determine the receiver-bias state and coefficient for a signal.
     *
     * @param obs_signal  The code signal ("C1W").
     * @param system      Its constellation.
     * @param satellite   The satellite (selects the GLONASS channel).
     * @param clock_refs  The .BIA product's satellite clock-reference observables per
     *                    constellation letter: the bands the receiver DCB is referenced to.
     *                    A constellation without an entry has no DCB or IFB key.
     * @return The state key and coefficient; a NaN coefficient and no key when the signal has
     *         no receiver-bias state.
     */
    ReceiverBiasEntry resolveReceiverBias(const std::string& obs_signal,
                                           System system,
                                           const std::string& satellite,
                                           const ClockRefMap& clock_refs) const {
        return resolver_.resolveReceiverBias(obs_signal, system, satellite, clock_refs);
    }

    /// The resolver itself (what the observation equation holds a pointer to).
    const ReceiverBiasResolver& resolver() const { return resolver_; }

    /**
     * @brief The frequency helper the seeds scale the slant ionosphere by.
     *
     * Its GLONASS channels are the default table, never the receiver's (nothing seeds
     * them), which is the number the seeds have always used; the model's own helper
     * (seeded from the RINEX header) belongs to the Jacobian row.
     *
     * @return The helper.
     */
    const PppUtils& frequencyHelper() const { return resolver_.frequencyHelper(); }

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
    ReceiverBiasResolver resolver_;

    bool isb_initialized_ = false;
    bool phase_clock_initialized_ = false;
    bool code_bias_initialized_ = false;
    bool clock_seed_initialized_ = false;
};

} // namespace GnssProcessor
