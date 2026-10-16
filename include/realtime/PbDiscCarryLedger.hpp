/**
 * @file PbDiscCarryLedger.hpp
 * @brief The consumer's declared-step ambiguity carry: the per-key applied-bias
 *        ledger, the carry-vs-reset decision and its run-end counters.
 *
 * A network-product consumer's phase row is
 *
 *     predicted = … + ssr_bias_term + amb_term        (both signs +1;
 *                                                       MeasurementModel.cpp)
 *
 * so when the satellite phase bias that the model applies moves by
 * Δ = PB_new − PB_old, the float ambiguity in the new datum is exactly amb − Δ:
 * a translation of one state, with the covariance untouched and no re-seed.
 * The step is either a declared re-realization (an IGS-SSR discontinuity-counter
 * bump) or the A5 freshness flip (applied ↔ not applied, with 0 standing for
 * "not applied").  The reset reaction (@c RealTimeEngine::resetOnPhaseBiasDisc)
 * instead zeroes the ambiguity's covariance row and column and re-seeds it from
 * one epoch of code on every declared step the consumer observes
 * (SPEC_event_harness §24.2).
 *
 * This class owns the two things the carry needs that the seam does not already
 * hold: the per-key record of the value the model applied at the last update
 * (0.0 = not applied or absent; presence of the key = recorded), and the
 * counters.  The decision and the state mutation live in @ref react, so every
 * refusal branch is unit-testable against a bare @c StateManager; the engine
 * seam (@c RealTimeEngine::carryOnPhaseBiasDisc_, RealTimeEngineSsrBreaks.cpp)
 * computes the inputs, calls it, and logs.
 *
 * A refusal takes the existing reset (@c StateManager::resetAmbiguity, the same
 * call the reset reaction makes), each reason with its own counter:
 *   - no recorded value for the key (first sighting; also each key's first
 *     event after a warm restore, because RT snapshot v1 carries no value map);
 *   - the state ambiguity basis is NL/WL (the shift would need the basis
 *     transform, which is out of scope);
 *   - the consumer is BSSD (its ambiguity states are between-satellite
 *     differences, not the undifferenced value the datum step acts on);
 *   - the ambiguity key is not in the state (mirrors resetAmbiguity's guard).
 *
 * Counter closure (standing invariant #4): every event is exactly one of
 * carried-disc, carried-flip or one reset reason, so
 * @c disc_carried + flip_carried + Σ reset_* == events.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

class StateManager;

/// Applied-bias ledger, carry-vs-reset decision and counters.
class PbDiscCarryLedger {
public:
    /// What @ref react did for one (key, epoch) event.
    enum class Action {
        None,             ///< No bump and no flip this epoch; nothing to do.
        CarriedDisc,      ///< Disc-counter bump: ambiguity translated by −Δ.
        CarriedFlip,      ///< Freshness flip only (no bump): translated by −Δ.
        ResetFirstSight,  ///< No recorded applied value for the key → existing reset.
        ResetBasis,       ///< NL/WL state basis → existing reset.
        ResetBssd,        ///< BSSD consumer → existing reset.
        ResetUnknownKey   ///< Ambiguity key not in the state → existing reset (a no-op there too).
    };

    /// Run-end counters (all monotone; the invariant-#4 readout).
    struct Counters {
        uint64_t disc_bumps{ 0 };         ///< Disc-counter changes seen (noteSsrDiscontinuity true).
        uint64_t flips{ 0 };              ///< A5 freshness flips seen (applied ↔ not applied).
        uint64_t events{ 0 };             ///< (key, epoch) pairs with a bump, a flip or both; the closure denominator.
        uint64_t disc_carried{ 0 };       ///< Events with a bump that were carried.
        uint64_t flip_carried{ 0 };       ///< Flip-only events that were carried.
        uint64_t reset_first_sight{ 0 };  ///< Refused: no recorded applied value.
        uint64_t reset_basis{ 0 };        ///< Refused: NL/WL ambiguity basis.
        uint64_t reset_bssd{ 0 };         ///< Refused: BSSD consumer.
        uint64_t reset_unknown_key{ 0 };  ///< Refused: ambiguity key not in the state.

        /// Σ of the four reset counters.
        uint64_t resets() const {
            return reset_first_sight + reset_basis + reset_bssd + reset_unknown_key;
        }
        /// The closure identity: carried + resets == events.
        bool closureHolds() const { return disc_carried + flip_carried + resets() == events; }
    };

    /// Outcome of one @ref react call.
    struct Decision {
        Action action{ Action::None };
        double delta_m{ 0.0 };   ///< Δ = applied_now − applied_prev (m); valid when carried.
    };

    /**
     * @brief Decide and apply the reaction for one phase key this epoch.
     *
     * @param sm             The consumer's state manager (mutated: shift or reset).
     * @param key            Ledger key ("sat|signal", e.g. "G24|L5X").
     * @param amb_key        Ambiguity state key ("sat_signal", e.g. "G24_L5X").
     * @param bump           The disc counter changed this epoch (noteSsrDiscontinuity).
     * @param flip           The A5 applied flag changed this epoch (fresh ↔ stale).
     * @param applied_now_m  The bias value the model applies in this epoch (0 = not applied).
     * @param basis_nlwl     The state ambiguity basis is NL/WL.
     * @param bssd           The consumer is BSSD (between-satellite SD in the filter).
     * @param reset_std_m    The reset std the existing reaction uses (ambiguity_std).
     * @return The action taken and, when carried, Δ.
     *
     * A bump coinciding with a flip on the same key is one event and one
     * translation (Δ already includes both), counted as a disc carry.
     */
    Decision react(StateManager& sm, const std::string& key, const std::string& amb_key,
                   bool bump, bool flip, double applied_now_m,
                   bool basis_nlwl, bool bssd, double reset_std_m);

    /**
     * @brief Update site: record the value the model applies this epoch.
     *
     * Call after @ref react for the key in the same epoch: the reaction's Δ is
     * taken against the previous record, and the next epoch's Δ has to be taken
     * against the value the model applied in this epoch.  The engine calls it at
     * the tail of the per-key loop in @c carryOnPhaseBiasDisc_, before the
     * measurement update that applies this value.
     */
    void record(const std::string& key, double applied_now_m) { applied_m_[key] = applied_now_m; }

    /// The recorded applied value for @p key, if any.
    std::optional<double> recorded(const std::string& key) const {
        const auto it = applied_m_.find(key);
        if (it == applied_m_.end()) return std::nullopt;
        return it->second;
    }

    /// Number of keys with a recorded value.
    std::size_t size() const { return applied_m_.size(); }

    const Counters& counters() const { return ctr_; }

    /// Stable name for logs / tests.
    static const char* actionName(Action a);

private:
    std::map<std::string, double> applied_m_;   ///< key → applied bias value at the last record (m).
    Counters ctr_{};
};

} // namespace GnssProcessor
