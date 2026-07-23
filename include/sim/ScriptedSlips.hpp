#pragma once

/**
 * @file ScriptedSlips.hpp
 * @brief Deterministic ("gotcha") cycle-slip schedule for the exam synthesizer.
 *
 * A @c ScriptedSlipSchedule replays a list of hand-authored @c ScriptedSlip
 * events (from @c SimConfig) as the synthesizer walks the arc.  It is layered ON
 * TOP of the random slip model and shares NONE of its state: this class contains
 * no RNG, performs no I/O, and pulls in no product dependencies, so the CTest
 * battery can cover it product-free.  @c runSynth calls @c onEpoch once per
 * (visible sat, band) per epoch, AFTER the random-slip block, and applies the
 * returned deltas to the running phase ambiguity — see SPEC_scripted_slips.md §4.
 *
 * The RNG invariant that makes the byte-identity / additivity gates hold lives in
 * the CALLER (runSynth): this class only decides WHICH deltas fire WHEN; it never
 * touches @c slip_last_epoch or the random slip/multipath streams.
 */

#include <string>
#include <vector>

#include "sim/SimConfig.hpp" // ScriptedSlip

namespace GnssProcessor {
namespace sim {

/**
 * @brief Stateful replay of a scripted cycle-slip list over an arc.
 *
 * Trigger semantics (per event):
 *  - Form A (@c epoch > 0): starts when @c epoch_counter == @c epoch exactly (so
 *    the satellite must be visible at that precise epoch, else the event never
 *    fires — a hard error surfaced by @c unfinished()).
 *  - Form B (elevation window): starts at the FIRST call with
 *    @c epoch_counter >= @c not_before_epoch and @c elev_lo_deg <= elev <
 *    @c elev_hi_deg.  The window selects the START only.
 *
 * Once started, the same @c delta_cycles is applied on each subsequent VISIBLE
 * epoch for that (sat, signal) — consecutive when the satellite stays up,
 * stalling across a mask gap — until @c repeat repetitions have been applied.
 */
class ScriptedSlipSchedule {
public:
    /// Build a schedule from a (validated) event list; the order is preserved and
    /// used as the reported @c event_index.
    explicit ScriptedSlipSchedule(const std::vector<ScriptedSlip>& events);

    /// One firing of one event this epoch.
    struct Fired {
        int  delta_cycles; ///< Signed integer jump to add to this (sat,signal) ambiguity.
        bool lli;          ///< Whether this repetition requests LLI=1.
        int  event_index;  ///< Index into the constructor's event list (provenance).
    };

    /**
     * @brief Advance every event matching (@p sat, @p phase_code) for this epoch.
     * @param sat            Satellite id, e.g. "G05".
     * @param phase_code     Phase RINEX code, e.g. "L2W".
     * @param epoch_counter  1-based epoch index of the current epoch.
     * @param elev_deg       Satellite elevation this epoch (deg).
     * @return One @c Fired per event that applies a repetition this epoch (usually
     *         zero or one; same-key same-epoch events each contribute an entry, so
     *         the caller sums the deltas and ORs the LLI flags).
     */
    std::vector<Fired> onEpoch(const std::string& sat, const std::string& phase_code,
                               int epoch_counter, double elev_deg);

    /// Human-readable descriptions of events that never fired or ended a run
    /// mid-@c repeat (one string per problem; empty when the schedule completed).
    std::vector<std::string> unfinished() const;

    /// True when there are no scripted events at all (the byte-identity case).
    bool empty() const { return events_.empty(); }

private:
    struct EventState {
        ScriptedSlip ev;         ///< The immutable event definition.
        bool         started{ false }; ///< Trigger satisfied at some prior/this call.
        int          applied{ 0 };     ///< Repetitions applied so far (0..repeat).
    };
    std::vector<EventState> events_;
};

} // namespace sim
} // namespace GnssProcessor
