/**
 * @file PbDiscCarryLedger.hpp
 * @brief Card CC (A): the consumer's DECLARED-STEP ambiguity carry — the
 *        per-key applied-bias ledger, the carry-vs-reset decision and its
 *        run-end counters.
 *
 * A network-product consumer's phase row is
 *
 *     predicted = … + ssr_bias_term + amb_term        (both signs +1;
 *                                                       MeasurementModel.cpp)
 *
 * so when the satellite phase bias the model APPLIES moves by Δ = PB_new − PB_old
 * — a declared re-realization (IGS-SSR discontinuity-counter bump) or the A5
 * freshness flip (applied ↔ not applied, with 0 for "not applied") — the float
 * ambiguity in the new datum is EXACTLY amb − Δ: a translation of one state,
 * covariance untouched, no re-seed.  The pre-card reaction
 * (@c RealTimeEngine::resetOnPhaseBiasDisc) threw that ambiguity away instead —
 * covariance row/column zeroed, re-seeded from one epoch of code — on every
 * declared step the consumer observed (29 per 6 h on the field seed,
 * SPEC_event_harness §24.2).
 *
 * This class owns the two things the carry needs and the seam does not already
 * have: the per-key RECORD of the value the model applied at the last update
 * (0.0 = not applied / absent; presence = recorded), and the counters.  The
 * decision + state mutation live in @ref react so every refusal branch is
 * unit-testable against a bare @c StateManager; the engine seam
 * (@c RealTimeEngine::carryOnPhaseBiasDisc_, RealTimeEngineSsrBreaks.cpp) only
 * computes the inputs, calls it, and logs.
 *
 * Refusals take the EXISTING reset (@c StateManager::resetAmbiguity — the same
 * call the pre-card reaction makes), each with its own counter:
 *   - no recorded value for the key (first sighting; also every key's first
 *     event after a warm restore — the RT snapshot v1 carries no value map);
 *   - the state ambiguity basis is NL/WL (the shift would need the basis
 *     transform — out of scope);
 *   - the consumer is BSSD (its ambiguity states are between-satellite
 *     differences, not the undifferenced value the datum step lands on);
 *   - the ambiguity key is not in the state (mirrors resetAmbiguity's guard).
 *
 * Counter closure (standing invariant #4): every event is exactly one of
 * carried-disc / carried-flip / one reset reason, so
 * @c disc_carried + flip_carried + Σ reset_* == events.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

class StateManager;

/// Card CC (A): applied-bias ledger + carry-vs-reset decision + counters.
class PbDiscCarryLedger {
public:
    /// What @ref react did for one (key, epoch) event.
    enum class Action {
        None,             ///< No bump and no flip this epoch — nothing to do.
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
        uint64_t events{ 0 };             ///< (key, epoch) with a bump and/or a flip — the closure denominator.
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
     * @brief Decide and APPLY the reaction for one phase key this epoch.
     *
     * @param sm             The consumer's state manager (mutated: shift or reset).
     * @param key            Ledger key ("sat|signal", e.g. "G24|L5X").
     * @param amb_key        Ambiguity state key ("sat_signal", e.g. "G24_L5X").
     * @param bump           The disc counter changed this epoch (noteSsrDiscontinuity).
     * @param flip           The A5 applied flag changed this epoch (fresh ↔ stale).
     * @param applied_now_m  The bias value the model applies THIS epoch (0 = not applied).
     * @param basis_nlwl     The state ambiguity basis is NL/WL.
     * @param bssd           The consumer is BSSD (between-satellite SD in the filter).
     * @param reset_std_m    The reset std the existing reaction uses (ambiguity_std).
     * @return The action taken and, when carried, Δ.
     *
     * A bump coinciding with a flip on the same key is ONE event and one
     * translation (Δ already includes both), counted as a disc carry.
     */
    Decision react(StateManager& sm, const std::string& key, const std::string& amb_key,
                   bool bump, bool flip, double applied_now_m,
                   bool basis_nlwl, bool bssd, double reset_std_m);

    /**
     * @brief The UPDATE SITE: record the value the model applies this epoch.
     *
     * Must run AFTER @ref react for the key in the same epoch (the reaction's Δ is
     * against the previous record; the next epoch's Δ must be against the value
     * the model actually applied this epoch).  The engine calls it at the tail
     * of the per-key loop in @c carryOnPhaseBiasDisc_, before the measurement
     * update that applies exactly this value.
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
