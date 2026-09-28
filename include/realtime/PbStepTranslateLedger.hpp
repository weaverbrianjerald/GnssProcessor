/**
 * @file PbStepTranslateLedger.hpp
 * @brief Consumer-side translation of every phase-bias product step, declared or
 *        not, into the float ambiguity (card PBT (A)).  Holds the per-key
 *        applied-value ledger, the translate / skip / refuse decision, and its
 *        counters.
 *
 * A network-product consumer's phase row is
 *
 *     predicted = … + ssr_bias_term + amb_term        (both signs +1;
 *                                                       MeasurementModel.cpp:1624)
 *
 * so a change in the satellite phase bias the model applies between two
 * consecutive consumer epochs,
 *
 *     Δ = PB_applied(k) − PB_applied(k−1),
 *
 * carries no information about the receiver's position.  The phase model stays
 * consistent if and only if the (sat, signal) float ambiguity absorbs it exactly:
 * amb ← amb − Δ, covariance untouched.  This holds whether or not an `R` record
 * declared the step, and it holds for a move common to every tracked satellite,
 * which is then absorbed uniformly with no de-commoning and no change to the wire.
 *
 * Card CC (@ref PbDiscCarryLedger) performs the same translation, but only on the
 * cells the producer declared.  Two populations measured on the RF-0hv field
 * lineage fall outside that gate:
 *   - 451 of 545 de-commoned phase-bias steps > 0.10 m are undeclared and land in
 *     live consumer ambiguities (EV6 / JT §4);
 *   - 117 of 390 declared cell-epochs are empty (< 0.02 m of PB motion), because
 *     `markSatDiscontinuity` bumps all three channel generations at once
 *     (CST §7.1 / card MS).
 * One rule covers both: translate on the step, not on the declaration.
 *
 * Scope.  A key with no phase bias applied at k−1 (first sighting, re-admission)
 * or none at k (an A5 freshness flip out of the applied set) has no Δ to
 * translate.  If the cell is declared, it takes the reset of the existing reaction
 * (@c RealTimeEngine::resetOnPhaseBiasDisc), made here through the same
 * @c StateManager::resetAmbiguity call; either way it is counted.  The three
 * structural refusals (@c NL/WL state basis, a BSSD consumer, an ambiguity key
 * absent from the state) are handled the same way: a declared cell falls back to
 * that reset, as card CC's refusals do, while an undeclared cell, on which the
 * reset path does nothing, stays a no-op with a counter and is never reset.
 *
 * Inside the eps band (@c pb_step_translate_eps_m, default 5 mm) the filter
 * absorbs the move as noise and nothing is written, so an empty declaration costs
 * nothing.
 *
 * Counter closure (standing invariant #4): every visited (key, epoch) cell takes
 * exactly one outcome, so @ref Counters::closureHolds sums to @c cells.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

class StateManager;

/// Applied-value ledger, translate decision and counters (card PBT (A)).
class PbStepTranslateLedger {
public:
    /// What @ref react did for one (key, epoch) cell.
    enum class Action {
        None,                 ///< No usable pair and no declaration: nothing to do (the key is only recorded).
        TranslateDeclared,    ///< |Δ| > eps and an R record declared it: ambiguity translated by −Δ.
        TranslateUndeclared,  ///< |Δ| > eps with no declaration: ambiguity translated by −Δ (the EV6 population).
        SkippedEps,           ///< |Δ| ≤ eps: the filter absorbs it as noise (an empty declaration lands here).
        ResetFirstSight,      ///< Declared, but no phase bias applied at k−1 or at k: the existing reset.
        ResetBasis,           ///< Declared, NL/WL state basis: the existing reset.
        ResetBssd,            ///< Declared, BSSD consumer: the existing reset.
        ResetUnknownKey,      ///< Declared, ambiguity key not in the state: the existing reset (a no-op there too).
        RefusedUndeclared     ///< An undeclared step blocked by a refusal class: no-op (the reset path does nothing here either).
    };

    /// Run-end counters (all monotone; the invariant-#4 readout).
    struct Counters {
        uint64_t cells{ 0 };                  ///< (key, epoch) cells visited; the closure denominator.
        uint64_t declared_seen{ 0 };          ///< Cells carrying a disc-counter bump and/or an A5 flip.
        uint64_t translate_declared{ 0 };     ///< n_pb_translate_declared.
        uint64_t translate_undeclared{ 0 };   ///< n_pb_translate_undeclared.
        double   sum_abs_translate_m{ 0.0 };  ///< sum_abs_translate_m: Σ|Δ| over every applied translation (m).
        double   max_abs_translate_m{ 0.0 };  ///< Largest single |Δ| translated (m).
        uint64_t skipped_eps{ 0 };            ///< n_pb_translate_skipped_eps (|Δ| ≤ eps).
        uint64_t skipped_eps_declared{ 0 };   ///< Of those, cells an R record declared: the empty-declaration population.
        uint64_t reset_first_sight{ 0 };      ///< Declared with no applied value at k−1 or k: existing reset.
        uint64_t reset_basis{ 0 };            ///< Declared, refused: NL/WL basis.
        uint64_t reset_bssd{ 0 };             ///< Declared, refused: BSSD consumer.
        uint64_t reset_unknown_key{ 0 };      ///< Declared, refused: ambiguity key not in the state.
        uint64_t refused_undeclared{ 0 };     ///< Undeclared step blocked by a refusal class (no-op).
        uint64_t no_pair{ 0 };                ///< Undeclared cell with no usable (k−1, k) applied pair.

        /// Σ of the four declared-reset counters.
        uint64_t resets() const {
            return reset_first_sight + reset_basis + reset_bssd + reset_unknown_key;
        }
        /// The closure identity: every visited cell took exactly one outcome.
        bool closureHolds() const {
            return translate_declared + translate_undeclared + skipped_eps + resets()
                       + refused_undeclared + no_pair
                   == cells;
        }
    };

    /// Outcome of one @ref react call.
    struct Decision {
        Action action{ Action::None };
        double delta_m{ 0.0 };   ///< Δ = applied_now − applied_prev (m); valid when translated or skipped.
    };

    /**
     * @brief Decide and apply the reaction for one phase key this epoch.
     *
     * @param sm             The consumer's state manager (mutated: shift or reset).
     * @param key            Ledger key ("sat|signal", e.g. "G32|L1W").
     * @param amb_key        Ambiguity state key ("sat_signal", e.g. "G32_L1W").
     * @param declared       An R record declared this cell (disc-counter bump and/or A5 flip).
     * @param applied_now    The model applies a phase bias this epoch.
     * @param applied_now_m  The value it applies (m); meaningless when @p applied_now is false.
     * @param basis_nlwl     The state ambiguity basis is NL/WL.
     * @param bssd           The consumer is BSSD (between-satellite SD in the filter).
     * @param eps_m          Translate only when |Δ| exceeds this (m).
     * @param reset_std_m    The reset std the existing reaction uses (ambiguity_std).
     * @return The action taken and, where meaningful, Δ.
     *
     * One translation per (key, epoch): the caller visits each key once, and this
     * function makes at most one @c StateManager::shiftAmbiguity call, the same
     * value-only call card CC's carry makes (RealTimeEngineSsrBreaks.cpp).
     */
    Decision react(StateManager& sm, const std::string& key, const std::string& amb_key,
                   bool declared, bool applied_now, double applied_now_m,
                   bool basis_nlwl, bool bssd, double eps_m, double reset_std_m);

    /**
     * @brief Update site: record what the model applies this epoch.
     *
     * Call after @ref react for the key in the same epoch.  This epoch's Δ is taken
     * against the previous record, and the next epoch's Δ must be taken against the
     * value that the measurement update following the reaction applies.
     */
    void record(const std::string& key, bool applied_now, double applied_now_m) {
        applied_[key] = Applied{ applied_now, applied_now_m };
    }

    /// The recorded applied value for @p key, if the key was recorded and a bias was applied.
    std::optional<double> recorded(const std::string& key) const {
        const auto it = applied_.find(key);
        if (it == applied_.end() || !it->second.applied) return std::nullopt;
        return it->second.value_m;
    }

    /// Number of keys with a record (applied or not).
    std::size_t size() const { return applied_.size(); }

    const Counters& counters() const { return ctr_; }

    /// Stable name for logs / the CST trace / tests.
    static const char* actionName(Action a);

private:
    struct Applied {
        bool   applied{ false };   ///< A phase bias was applied at the recorded epoch.
        double value_m{ 0.0 };     ///< The value applied (m); 0 when @c applied is false.
    };
    std::map<std::string, Applied> applied_;   ///< key → what the model applied at the last record.
    Counters ctr_{};
};

} // namespace GnssProcessor
