/**
 * @file PbHoldLedger.hpp
 * @brief Consumer reaction to a held satellite phase bias: the per-key generation
 *        memory, the ignore-or-refloat decision and its run-end counters.
 *
 * A producer running @c network.emit_pb_hold freezes the emitted phase bias of
 * each (satellite, signal) at the first value of its generation and keeps
 * estimating behind it, so on the wire @c bias_m is constant within a generation
 * rather than a per-epoch estimate.  That changes the meaning of both signals the
 * consumer reactions without a hold key on:
 *
 *  - The discontinuity counter no longer implies a value step, so a counter change
 *    is consumed and ignored.  It is still consumed so that the state manager's
 *    per-key memory advances; otherwise a key that later leaves the hold would
 *    produce a spurious reset from a stale comparison.
 *  - The hold generation becomes the declaration.  A change of a previously seen
 *    nonzero generation means the producer re-levelled the key, and the consumer
 *    re-floats that ambiguity through the existing reset path.
 *
 * A first sighting is not a change: there is no prior consumer state that could
 * have drifted against the new level, and re-floating on it would discard a valid
 * ambiguity at every satellite rise.
 *
 * The decision is kept separate from the engine seam
 * (@c RealTimeEngine::reactToPbHold_, RealTimeEngineSsrBreaks.cpp), as in cards CC
 * and PBT: the seam computes the inputs, calls @ref react, and performs the reset
 * and the logging, so every branch is unit-testable without a live engine.  The
 * ledger modifies only its own memory and counters.
 *
 * Counter closure (standing invariant #4): every visited cell takes exactly one of
 * @c first_sight / @c refloats / @c held, so those three sum to @c cells.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

/// Hold-generation memory, the ignore-or-refloat decision and counters.
class PbHoldLedger {
public:
    /// What @ref react decided for one (key, epoch) cell.
    enum class Action {
        NotHeld,     ///< @c hold_generation == 0: not a held entry; the caller's normal path runs.
        FirstSight,  ///< First held entry for this key: adopt the generation, react to nothing.
        Held,        ///< Same generation as last epoch: the value has not moved; any counter change is ignored.
        Refloat      ///< The generation changed: re-float this ambiguity through the existing reset path.
    };

    /// Run-end counters (all monotone; the invariant #4 readout).
    struct Counters {
        std::uint64_t cells{ 0 };         ///< Held (key, epoch) cells visited; the closure denominator.
        std::uint64_t keys{ 0 };          ///< Distinct keys ever seen held.
        std::uint64_t first_sight{ 0 };   ///< Cells that were a key's first held entry.
        std::uint64_t held{ 0 };          ///< Cells inside a generation (the value is a constant).
        std::uint64_t refloats{ 0 };      ///< Generation changes that re-floated an ambiguity.
        std::uint64_t disc_ignored{ 0 };  ///< Disc-counter changes suppressed because the value could not have stepped.

        /// The closure identity: every visited cell took exactly one branch.
        bool closureHolds() const { return first_sight + held + refloats == cells; }
    };

    /// Outcome of one @ref react call.
    struct Decision {
        Action        action{ Action::NotHeld };
        std::uint32_t previous_generation{ 0 };  ///< The generation held before this cell (0 = none).
    };

    /**
     * @brief Decide the reaction for one phase key this epoch and adopt its generation.
     *
     * @param key         Ledger key ("sat|signal", e.g. "G24|L1W").
     * @param generation  @c SsrPhaseBiasEntry::hold_generation of the entry the
     *                    consumer is about to apply.  0 means the producer is not
     *                    holding this key; the call is then a no-op returning
     *                    @ref Action::NotHeld, which keeps every unheld product
     *                    (RTCM streams, files written before card PBH, runs with the
     *                    hold off) on the unchanged consumer path.
     * @param disc_changed Whether the discontinuity counter changed this epoch.  Only
     *                    counted, never acted on, for a held key; ignored for an
     *                    unheld one.
     * @return The action and the generation this key carried before the call.
     *
     * @note Not idempotent: it adopts the generation, so a key must be visited at
     *       most once per epoch.  The engine guarantees this by recording every key
     *       it handles and skipping it in the three downstream reactions.
     */
    Decision react(const std::string& key, std::uint32_t generation, bool disc_changed) {
        Decision d;
        if (generation == 0) return d;   // not held: unchanged consumer path
        ++ctr_.cells;
        if (disc_changed) ++ctr_.disc_ignored;
        const auto it = gen_.find(key);
        if (it == gen_.end()) {
            ++ctr_.keys;
            ++ctr_.first_sight;
            d.action = Action::FirstSight;
            gen_.emplace(key, generation);
            return d;
        }
        d.previous_generation = it->second;
        if (it->second != generation) {
            ++ctr_.refloats;
            d.action = Action::Refloat;
        } else {
            ++ctr_.held;
            d.action = Action::Held;
        }
        it->second = generation;
        return d;
    }

    /// The generation recorded for @p key, if the key has ever been seen held.
    std::optional<std::uint32_t> recorded(const std::string& key) const {
        const auto it = gen_.find(key);
        if (it == gen_.end()) return std::nullopt;
        return it->second;
    }

    /// Number of keys with a recorded generation.
    std::size_t size() const { return gen_.size(); }

    const Counters& counters() const { return ctr_; }

    /// Stable lowercase name for logs and tests.
    static const char* actionName(Action a) {
        switch (a) {
            case Action::NotHeld:    return "not_held";
            case Action::FirstSight: return "first_sight";
            case Action::Held:       return "held";
            case Action::Refloat:    return "refloat";
        }
        return "unknown";
    }

private:
    std::map<std::string, std::uint32_t> gen_;  ///< key -> last hold generation seen.
    Counters                             ctr_;
};

}  // namespace GnssProcessor
