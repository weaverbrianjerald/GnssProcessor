/**
 * @file Inject.hpp
 * @brief Event harness: the producer-side fault injector seam.
 *
 * ### Purpose
 *
 * Grading a mechanism by running the network for hours until the data supply an
 * event makes one question (for example the re-level cost of a single RAIM
 * attribution erase)
 * take a day, and mixes the event's effect with its neighbours and the run's own
 * ignition.  The harness instead converges once into a state snapshot
 * (@c network.state_snapshot_*), resumes for a few tens of epochs with exactly
 * one scripted fault armed, and differences the result against a null leg
 * resumed from the same snapshot.  The seam transient, the bed and the posture
 * are common to both legs and cancel.
 *
 * ### Where each kind acts
 *
 * Ten kinds exist; the two newest (@c sat_health, @c station_step) and the
 * outage scopes are described in the section "Scenario kinds" below.  Of the first
 * eight, four are corruptions of the producer's input (@c slip,
 * @c outage, @c sat_clock_step, @c rx_clock_step) and are applied to a per-epoch
 * mutable copy of the @c SyncedEpoch at the top of the epoch body: before
 * @c computeModels, every slip detector, the raw-observable pass and assembly.  One
 * site suffices because everything downstream derives from it:
 * @c PppModel::computeModels builds both
 * @c PppObservation::observed (phase in metres, via the wavelength) and
 * @c PppObservation::raw_observed (phase in cycles, read by MW, GF and IF-TD)
 * from the same @c RinexObservation @c measurements entry.  Changing one scalar
 * there therefore moves the slip detectors and the design matrix together and
 * consistently; changing @c modeled afterwards would need a dual write, a
 * wavelength lookup and a site inside an OpenMP region.
 *
 * For @c sat_clock_step and @c rx_clock_step, applying the step at this site is
 * exact, not an approximation.  The uncombined residual is
 * @f$ y = \tilde{L} - (\rho + c\,\delta t_r - c\,\delta t^s + \ldots) @f$, so
 * adding @f$ s @f$ metres to every code observable and @f$ s/\lambda @f$ cycles
 * to every phase observable
 *
 *   * of one satellite at every station is adding @f$ s @f$ to the a-priori
 *     satellite clock @f$ c\,\delta t^s @f$ (@c sat_clock_step), and
 *   * of every satellite at one station is adding @f$ s @f$ to that station's
 *     receiver clock @f$ c\,\delta t_r @f$ (@c rx_clock_step).
 *
 * The two are the same scalar write swept over the other index of the same
 * residual.  Both are persistent steps (the delta is re-applied every epoch from
 * @c tow on), as for a clock that has jumped; neither is a one-epoch spike.
 * Applying them here keeps the injector out of @c ModelGeometry, which has no
 * access to the run context.
 *
 * Three kinds are not observation faults: they force a decision the producer
 * would otherwise make on the data, or plant a state the producer would otherwise
 * hold, so they are staged here and consumed by the seam that owns it:
 *
 *   * @c anchor_handover   -> @c networkElectPhaseAnchors (network/solve/PhaseDatum.hpp)
 *   * @c arc_erase         -> the RAIM attribution refloat (network/qc/Raim.hpp), or the
 *                             PASS-A slip drain for mode @c free_epoch
 *   * @c fixed_cycle_plant -> the fixed-cycle seam's ledger (network/ar/FixedCycles.hpp):
 *                             a wrong integer planted on one arc
 *
 * The eighth, @c oracle_pin (simulation only), is not handled by this seam
 * beyond the arming summary: network/slip/OraclePin.hpp reads it from the config
 * and feeds @c NetworkInjectState::n_applied itself.
 *
 * ### Scenario kinds: scoped outages, a health flip, a coordinate step
 *
 * Three more shapes complete the fault matrix of card SMX
 * (docs/refound/SMX_scenario_matrix_2026-10-24.md):
 *
 *   * @c outage takes two scopes beyond one satellite at one station.  A
 *     @c signal list ("L1C;L2W") drops only those signals (each phase code with
 *     the code, Doppler and strength observables of its band and tracking
 *     attribute) while the satellite's other signals track through: the
 *     single-signal loss of lock of card L5B's MAJU00MHL event.  @c sat "all"
 *     drops every satellite of the station (a whole-station outage).  An optional
 *     @c delta_cycles is added to every dropped phase observable from the end of
 *     the window on (the re-acquisition's new integer), and @c flag_lli raises
 *     LLI 0x1 on them at the first epoch back.  Both act on this same site.
 *   * @c sat_health flags one satellite's broadcast health unhealthy from
 *     @c tow, for @c duration_s or to the run end.  The span is written into the
 *     run's health series (@c NetworkSetupResult::sat_health) at arm time, where
 *     the readers of @c network.sat_health ("pivot") and
 *     @c network.pivot_election query it.  The "exclude" door ran at setup on the
 *     broadcast as parsed, so under "exclude" this seam removes the satellite's
 *     observations at every station, the consumer included, while the flag is in
 *     force, exactly as the door would have.  With no reader armed the flag is
 *     unread: the event still fires (the flag is in force) and the arming line
 *     and the run-end clause say that nothing reads it.
 *   * @c station_step moves one roster station's held coordinate by
 *     (@c east_m, @c north_m, @c up_m) in its local frame at the first epoch at or
 *     after @c tow, and moves it back after @c duration_s when that is positive.
 *     The held coordinate is pinned in the solve (@c StationSplit), so the step
 *     reaches the rows through @c NetStation::ecef, @c PppState::position (read
 *     by @c computeModels) and the position slots' linearization points, all
 *     three written together here, before the epoch's models are computed.  The
 *     observations are unchanged, so a step of +d is the antenna moving by -d;
 *     card PLANT's planted offsets are the same fault from the first epoch.
 *
 * Each of the three is an event at both edges: an @c INJECT row at onset and a
 * @c released row at the closing edge (the outage's return, the flag's clear,
 * the coordinate's return), and the run-end clause counts both.
 *
 * ### Byte-identity
 *
 * With @c network.inject absent, @c enable false or an empty event list,
 * @c NetworkInjectState::armed stays false.  @c networkInjectEpoch then returns
 * a reference to the caller's own @c SyncedEpoch (no copy, no counter moved),
 * and every consumer sees a null directive pointer.  The manifest carries the
 * inject keys only when @c network.inject.enable is set (as for @c mdb), so a
 * run without @c network.inject has unchanged manifest bytes.
 *
 * ### An event that never fires is a hard error
 *
 * Same rule as the simulator's scripted slips (@c ScriptedSlips::unfinished,
 * @c src/sim/main.cpp): an armed event that never fired fails the run at run
 * end, by name.  Otherwise a leg whose fixture silently did not take effect
 * would be graded as a null and reported as "the event had no effect".
 */
#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/GnssConfigTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    class LogManager;
    class PppUtils;                // common/PppUtils.hpp (held by shared_ptr, so incomplete is fine)
    struct NetworkRunContext;
    struct NetworkSetupResult;     // network/driver/SetupResult.hpp (stations, broadcast health)
    class NetworkRunTraces;
    struct SyncedEpoch;            // rinex/MultiRinexSync.hpp, namespace scope

    /**
     * @brief A staged @c anchor_handover directive, consumed by the anchor election.
     *
     * Filled by @c networkInjectEpoch on the epoch the event fires and cleared at
     * the top of the next epoch, so it lives for exactly one epoch.  The election
     * reads it through a @c const pointer that is @c nullptr on every unarmed run
     * and on every epoch the event is not due.
     */
    struct InjectAnchorDirective {
        std::string sat;            ///< Satellite whose phase-anchor families are forced.
        std::string signal{ "all" };///< "all", or one RINEX-3 phase code.
        bool        cold{ false };  ///< Force the cold branch (re-realize and re-reference).
        /// @brief True when @p amb ("<sat>_<signal>") is in this directive's scope.
        /// @param amb Ambiguity key: the satellite id, an underscore, then the phase signal code.
        /// @return True when @p amb names this directive's satellite and the directive's signal is
        ///         empty or "all" or equals the key's signal; false otherwise.
        bool matches(const std::string& amb) const {
            if (amb.size() <= sat.size() || amb.compare(0, sat.size(), sat) != 0
                || amb[sat.size()] != '_')
                return false;
            return signal.empty() || signal == "all"
                   || amb.compare(sat.size() + 1, std::string::npos, signal) == 0;
        }
        // --- filled in by the election, read back by the injector for its event row ---
        mutable std::vector<std::string> retired;  ///< Incumbent station names retired this epoch.
        mutable std::vector<std::string> elected;  ///< Successor station names elected this epoch.
        mutable int n_families{ 0 };               ///< Families the directive actually touched.
        mutable int n_forced_cold{ 0 };            ///< Families whose continuous branch was refused.
        /// Families that took the cold pin although the directive did not ask for
        /// it: the successor had no ledger entry, or one too poorly determined to
        /// serve as a datum.  A cold outcome is a real gauge re-realization, so it
        /// is counted here and never added to the continuous total.
        mutable int n_cold_fallback{ 0 };
        /// Families for which the ban left no usable candidate, so the election
        /// pinned no station this epoch.  This is a decoupling edge with no
        /// re-coupling, and it is counted.
        mutable int n_no_successor{ 0 };
        /// Cold anchor-fallback readback (network.phase_datum_candidacy_usable):
        /// higher-ranked candidates the usable-first pass skipped across this
        /// directive's families, and the first station it skipped.  Both stay 0 / empty with the knob off (the pass is
        /// not entered), so the PHASE_ANCHOR handover row is unchanged on an
        /// unarmed run.
        mutable int n_cand_skipped{ 0 };
        mutable std::string cand_first_unusable;   ///< First station the usable-first pass skipped (empty when none).
    };

    /**
     * @brief A staged @c arc_erase directive, consumed by the RAIM attribution
     *        block (erase modes) or by the PASS-A slip drains (@c free_epoch).
     *
     * @c mode is a diagnostic sub-knob.  The erase path performs actions the
     * detector refloat does not, and the modes ask which of them causes the
     * persistent level floor that an unnecessary erase leaves behind:
     *
     *   | mode              | LocalCarry entry  | LOS iono carry | pending-reseed | free-epoch latch |
     *   |-------------------|-------------------|----------------|----------------|------------------|
     *   | @c erase (default)| erased or dropped | co-erased      | marked         | no               |
     *   | @c erase_no_reseed| erased or dropped | co-erased      | not marked     | no               |
     *   | @c free_epoch     | kept              | kept           | marked         | yes              |
     *
     * @c erase_keep_iono and @c erase_no_funnel are still accepted but act as
     * @c erase: the diagonal slant ionosphere carry the first kept and the refloat
     * queue the second withheld no longer exist, and the in-block iono carry, when
     * armed, is co-erased in every erase mode.
     *
     * Harness only: the mode changes what the injected erase does.  A real
     * attribution is unchanged on every path, and an unarmed run cannot reach
     * any of it.
     */
    struct InjectArcErase {
        std::string station;          ///< Station name (roster-matched at the RAIM/drain site).
        std::string sat;              ///< Satellite id.
        std::string mode{ "erase" };  ///< See the table above; loader-defaulted to "erase".
        /// Band scope: the phase signals the erase or free acts on.
        /// Empty means every signal of that satellite, which is what both production
        /// paths do.  A non-empty set restricts the action, so a healthy arc can be
        /// freed on the same 2-of-3 scope a real IF-TD detector refloat uses.
        std::set<std::string> signals;
        std::size_t ev_index{ 0 };    ///< Index into @c NetworkInjectState::events (ledger updates).
        /// @brief True when @p sig is in this directive's band scope.
        /// @param sig RINEX-3 phase signal code.
        /// @return True when @c signals is empty (every signal of the satellite) or contains @p sig.
        bool coversSignal(const std::string& sig) const {
            return signals.empty() || signals.count(sig) != 0;
        }
        /// @brief True for every mode that runs the RAIM attribution erase path (i.e. not @c free_epoch).
        /// @return True unless @c mode is "free_epoch".
        bool isErase() const { return mode != "free_epoch"; }
    };

    /**
     * @brief A staged @c fixed_cycle_plant directive, consumed by the
     *        fixed-cycle seam (network/ar/FixedCycles.hpp).
     *
     * Staged at every epoch from its @c tow until it fires: it fires at the first epoch at which
     * the ledger of fixed cycles holds the arc, where every row's integer is shifted by
     * @c delta times the row's coefficient on the arc (a wrong integer planted in the ledger,
     * for its float re-validation to find).  An arc never held leaves the event unfired, which
     * the run end reports as a hard error.
     */
    struct InjectFixedCyclePlant {
        std::string station;          ///< Station name (roster-matched by the seam).
        std::string sat;              ///< Satellite id.
        std::string signal;           ///< RINEX-3 phase code of the arc's band.
        long long   delta{ 0 };       ///< The integer shift (cycles).
        std::size_t ev_index{ 0 };    ///< Index into @c NetworkInjectState::events (ledger updates).
    };

    /**
     * @brief Run-scope state of the injector: the schedule, its firing ledger and
     *        the one-epoch directives other seams consume.
     *
     * Lives in @c NetworkRunContext.  It is not part of the warm-resume fingerprint or the snapshot: a leg resumes
     * a converged state and then applies its own schedule, so two legs off one
     * snapshot differ only in this object.
     */
    struct NetworkInjectState {
        bool armed{ false };                            ///< Any event parsed and enabled.
        std::vector<NetworkInjectEvent> events;         ///< The schedule (config copy).
        std::vector<long long> n_applied;               ///< Per event: epochs it acted on.
        std::vector<bool>      onset_logged;            ///< Per event: onset row already emitted.
        /// Per event: the closing-edge row already emitted (an outage's return, a health
        /// flag's clear, a coordinate's return).
        std::vector<bool>      release_logged;
        std::vector<long long> n_window;                ///< Per event: outage epochs that dropped something.
        /// Per event: the outage's signal scope (phase codes); empty = the whole record.
        std::vector<std::set<std::string>> signal_scope;
        std::map<std::string, long long> fired_by_kind; ///< kind -> events that fired at least once.

        /// The run's setup (non-owning; it outlives the epoch loop), for the two kinds
        /// that act on it: @c station_step moves a roster station's held coordinate and
        /// @c sat_health writes the broadcast health series.  Null when the caller passed
        /// none (a unit test of the other kinds).
        NetworkSetupResult* setup{ nullptr };
        /// Per event: the station_step's roster index (-1 until it fires), the ECEF shift
        /// it applied (m), and the coordinate and position linearization points it found
        /// (x, y, z each), which the closing edge restores to the bit.
        std::vector<int> station_index;
        std::vector<std::array<double, 3>> station_shift;   ///< Per event: the ECEF shift applied (x, y, z; m).
        std::vector<std::array<double, 6>> station_orig;    ///< Per event: the coordinate then the position linearization point found (x, y, z each), restored at the closing edge.
        /// The readers of the broadcast health series on this run ("exclude", "pivot",
        /// "pivot_election", comma-joined), or "none" when nothing reads a sat_health flip.
        std::string health_readers{ "none" };
        bool health_exclude{ false };          ///< network.sat_health "exclude": this seam removes observations.
        long long n_health_obs_removed{ 0 };   ///< Observations removed under an injected flag ("exclude").
        long long n_health_spans{ 0 };         ///< Unhealthy spans written into the health series.
        long long n_station_steps{ 0 };        ///< Held-coordinate steps applied.
        long long n_station_returns{ 0 };      ///< Held-coordinate steps moved back.
        long long n_outage_signal_obs{ 0 };    ///< Observables dropped by signal-scoped outages.
        long long n_outage_station_recs{ 0 };  ///< Records dropped by whole-station outages.
        long long n_outage_return_obs{ 0 };    ///< Returned phase observables shifted or flagged.
        /// Wavelength source for the clock-step kinds (phase observables are in
        /// cycles).  Built once at arm time; null on every unarmed run.
        std::shared_ptr<PppUtils> utils;

        /// This epoch's staged directives (cleared at the top of every epoch).
        std::vector<InjectAnchorDirective> anchor_now;
        /// Production anchor-slip handovers (network.cycle_slip.anchor_slip_handover),
        /// staged by
        /// @ref networkStageAnchorSlipHandovers for every family whose anchor arc a
        /// slip response freed this epoch.  Kept in a second vector rather than
        /// pushed into @c anchor_now because the injector's firing ledger, its
        /// never-fired error and its INJECT event rows iterate @c anchor_now and
        /// must see only the scripted directives.  Same one-epoch lifetime and
        /// cleared at the same point, but it is used on unarmed runs too, so the
        /// clear sits ahead of @c networkInjectEpoch's armed guard.
        std::vector<InjectAnchorDirective> anchor_now_slip;
        /// This epoch's staged arc-erase directives (station, satellite, mode).
        std::vector<InjectArcErase> arc_erase_now;
        /// This epoch's staged fixed-cycle plants, re-staged until they fire.
        std::vector<InjectFixedCyclePlant> fixed_cycle_plant_now;
        long long n_arc_erase_acted{ 0 };  ///< Chords actually refloated by injected erases.
        /// Path census: mode -> chords the injected event actually acted on.
        /// A zero here on an armed leg means the wiring is inert, which is the
        /// confound the sub-knobs exist to rule out.
        std::map<std::string, long long> arc_erase_mode_acted;

        /// @brief Directive covering @p amb, or @c nullptr.  Cheap on the empty path.
        ///
        /// The injected schedule is consulted first: a harness leg that scripts an
        /// @c anchor_handover for a family the production path also staged gets the
        /// scripted directive (its @c cold mode is what such a leg tests), and the
        /// election reads exactly one directive per family.
        ///
        /// @param amb Ambiguity key: the satellite id, an underscore, then the phase signal code.
        /// @return Pointer to the first matching staged directive (scripted first, then production),
        ///         or null when none covers @p amb.  Valid until the directives are cleared at the top
        ///         of the next epoch.
        const InjectAnchorDirective* anchorFor(const std::string& amb) const {
            for (const auto& d : anchor_now)
                if (d.matches(amb)) return &d;
            for (const auto& d : anchor_now_slip)
                if (d.matches(amb)) return &d;
            return nullptr;
        }
        /// @brief True when at least one event fired at least once.
        /// @return True when some event's applied-epoch count is above zero; false for an unarmed or unfired schedule.
        bool anyFired() const {
            for (long long n : n_applied) if (n > 0) return true;
            return false;
        }
    };

    /**
     * @brief Arm the injector from config (once, at run start).
     *
     * @param ctx      Run context; @c ctx.inject is filled in.
     * @param config_      The run's config (reads @c network.inject).
     * @param log_manager  Owner of the @c PppUtils the wavelength lookup needs.
     * @param log          Logger for the arming summary.
     * @param setup        The run's setup, kept for @c station_step (the roster) and
     *                     @c sat_health (the health series, into which each flip's span
     *                     is written here); null when the schedule has neither kind.
     *
     * Absent, disabled or empty => @c ctx.inject.armed stays false and nothing
     * else in the harness is reachable.
     */
    void networkInjectArm(NetworkRunContext& ctx, const GnssConfig& config_,
                          LogManager& log_manager, const std::shared_ptr<spdlog::logger>& log,
                          NetworkSetupResult* setup = nullptr);

    /**
     * @brief Apply this epoch's observation-side faults and stage its directives.
     *
     * @param ctx      Run context (schedule, ledger and staged directives).
     * @param log      Logger (onset lines).
     * @param traces   Trace bundle; one @c network_events.csv row per discrete action.
     * @param se_raw   The epoch as synchronised from the input files.
     * @param scratch  Caller-owned buffer that receives the mutated copy.
     * @return @p se_raw itself when the injector is unarmed or has no
     *         observation-side event due this epoch (no copy is taken; this is the
     *         byte-identical path), otherwise a reference to @p scratch holding the
     *         corrupted epoch.
     *
     * @note @c scratch must outlive the returned reference for the whole epoch
     *       body; declare it immediately before the call.
     */
    const SyncedEpoch& networkInjectEpoch(NetworkRunContext& ctx,
                                                 const std::shared_ptr<spdlog::logger>& log,
                                                 NetworkRunTraces& traces,
                                                 const SyncedEpoch& se_raw,
                                                 SyncedEpoch& scratch);

    /**
     * @brief Run-end summary and the never-fired hard error.
     *
     * @param ctx  The run context: the injector's schedule, per-event fire counts and counters
     *             are read.
     * @param log  The network logger the summary (and the never-fired error) is written to.
     * @return 0 when every armed event fired at least once (or the injector is
     *         unarmed), 1 when at least one armed event never fired; the caller
     *         (the run-end table's row, the overload below) returns it as its verdict,
     *         which @c networkTeardown makes the process exit code.
     */
    int networkInjectRunEnd(const NetworkRunContext& ctx,
                            const std::shared_ptr<spdlog::logger>& log);

    // ---- Run-end reports ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the event harness (fault injection).
     *
     * The table's row for the event harness: its clause, and the hard error for a schedule
     * that never fired; it calls the overload above, which a unit test calls directly.
     *
     * Gate: none of its own: the row calls the report above on every run, and that report
     * applies its own.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return the injection verdict: non-zero when an armed schedule never fired, else 0.
     */
    int networkInjectRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
