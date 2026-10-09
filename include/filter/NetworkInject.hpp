/**
 * @file NetworkInject.hpp
 * @brief Event harness: the producer-side fault injector seam.
 *
 * ### Purpose
 *
 * Grading a mechanism by running the network for hours until the data supply an
 * event makes one question (for example the re-level cost of a single T4 erase)
 * take a day, and mixes the event's effect with its neighbours and the run's own
 * ignition.  The harness instead converges once into a state snapshot
 * (@c network.state_snapshot_*), resumes for a few tens of epochs with exactly
 * one scripted fault armed, and differences the result against a null leg
 * resumed from the same snapshot.  The seam transient, the bed and the posture
 * are common to both legs and cancel.
 *
 * ### Where each kind acts
 *
 * Seven kinds exist.  Four are corruptions of the producer's input (@c slip,
 * @c outage, @c sat_clock_step, @c rx_clock_step) and are applied to a per-epoch
 * mutable copy of the @c SyncedEpoch at the top of the epoch body: before
 * @c networkFastClockTick, @c computeModels, every T0 detector, the
 * raw-observable pass and assembly.  One site suffices because everything
 * downstream derives from it: @c PppModel::computeModels builds both
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
 * Two kinds are not observation faults: they force a decision the producer
 * would otherwise make on the data, so they are staged here and consumed by the
 * seam that owns that decision:
 *
 *   * @c anchor_handover  -> @c networkElectPhaseAnchors (filter/NetworkPhaseDatum)
 *   * @c arc_erase        -> the T4 attribution refloat (filter/NetworkRaim), or the
 *                            PASS-A slip drain for mode @c free_epoch
 *
 * The seventh, @c oracle_pin (simulation only), is not handled by this seam
 * beyond the arming summary: filter/NetworkOraclePin reads it from the config
 * and feeds @c NetworkInjectState::n_applied itself.
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
        /// serve as a datum.  A cold outcome is a real gauge re-realization (the
        /// re-coupling edge of invariant #7), so it is counted here and never added
        /// to the continuous total (docs/SPEC_event_harness.md §12.7).
        mutable int n_cold_fallback{ 0 };
        /// Families for which the ban left no usable candidate, so the election
        /// pinned no station this epoch.  This is a decoupling edge with no
        /// re-coupling, and it is counted.
        mutable int n_no_successor{ 0 };
        /// Cold anchor-fallback readback (network.phase_datum_candidacy_usable,
        /// docs/SPEC_event_harness.md §15): higher-ranked candidates the
        /// usable-first pass skipped across this directive's families, and the first
        /// station it skipped.  Both stay 0 / empty with the knob off (the pass is
        /// not entered), so the PHASE_ANCHOR handover row is unchanged on an
        /// unarmed run.
        mutable int n_cand_skipped{ 0 };
        mutable std::string cand_first_unusable;
    };

    /**
     * @brief A staged @c arc_erase directive, consumed by the T4 attribution
     *        block (erase modes) or by the PASS-A slip drains (@c free_epoch).
     *
     * @c mode is the diagnostic sub-knob of Study 1 (docs/SPEC_event_harness.md
     * §9).  The erase path performs four actions the detector refloat does not,
     * and the study asks which of them causes the persistent level floor that an
     * unnecessary erase leaves behind:
     *
     *   | mode              | LocalCarry entry | LOS iono carry | pending-reseed | C3c queue | free-epoch latch |
     *   |-------------------|------------------|----------------|----------------|-----------|------------------|
     *   | @c erase (default)| erased           | co-erased      | marked         | queued    | no               |
     *   | @c erase_keep_iono| erased           | kept           | marked         | queued    | no               |
     *   | @c erase_no_reseed| erased           | co-erased      | not marked     | queued    | no               |
     *   | @c erase_no_funnel| erased           | co-erased      | marked         | not queued| no               |
     *   | @c free_epoch     | kept             | kept           | marked         | (detector)| yes              |
     *
     * Harness only: the mode changes what the injected erase does.  A real
     * attribution is unchanged on every path, and an unarmed run cannot reach
     * any of it.
     */
    struct InjectArcErase {
        std::string station;          ///< Station name (roster-matched at the RAIM/drain site).
        std::string sat;              ///< Satellite id.
        std::string mode{ "erase" };  ///< See the table above; loader-defaulted to "erase".
        /// Band scope (Study 1 rung 2): the phase signals the erase or free acts on.
        /// Empty means every signal of that satellite, which is what both production
        /// paths do.  A non-empty set restricts the action, so a healthy arc can be
        /// freed on the same 2-of-3 scope a real IF-TD detector refloat uses.
        std::set<std::string> signals;
        std::size_t ev_index{ 0 };    ///< Index into @c NetworkInjectState::events (ledger updates).
        /// @brief True when @p sig is in this directive's band scope.
        bool coversSignal(const std::string& sig) const {
            return signals.empty() || signals.count(sig) != 0;
        }
        /// @brief True for every mode that runs the T4 erase path (i.e. not @c free_epoch).
        bool isErase() const { return mode != "free_epoch"; }
    };

    /**
     * @brief Run-scope state of the injector: the schedule, its firing ledger and
     *        the one-epoch directives other seams consume.
     *
     * Lives in @c NetworkRunContext (the anti-regrowth rule for carried state).
     * It is not part of the warm-resume fingerprint or the snapshot: a leg resumes
     * a converged state and then applies its own schedule, so two legs off one
     * snapshot differ only in this object.
     */
    struct NetworkInjectState {
        bool armed{ false };                            ///< Any event parsed and enabled.
        std::vector<NetworkInjectEvent> events;         ///< The schedule (config copy).
        std::vector<long long> n_applied;               ///< Per event: epochs it acted on.
        std::vector<bool>      onset_logged;            ///< Per event: onset row already emitted.
        std::map<std::string, long long> fired_by_kind; ///< kind -> events that fired at least once.
        /// Wavelength source for the clock-step kinds (phase observables are in
        /// cycles).  Built once at arm time; null on every unarmed run.
        std::shared_ptr<PppUtils> utils;

        /// This epoch's staged directives (cleared at the top of every epoch).
        std::vector<InjectAnchorDirective> anchor_now;
        /// Production anchor-slip handovers (network.cycle_slip.anchor_slip_handover,
        /// docs/SPEC_event_harness.md §12.7), staged by
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
        long long n_arc_erase_acted{ 0 };  ///< Chords actually refloated by injected erases.
        /// Study-1 path census: mode -> chords the injected event actually acted on.
        /// A zero here on an armed leg means the wiring is inert (invariant #4),
        /// which is the confound the sub-knobs exist to rule out.
        std::map<std::string, long long> arc_erase_mode_acted;

        /// @brief Directive covering @p amb, or @c nullptr.  Cheap on the empty path.
        ///
        /// The injected schedule is consulted first: a harness leg that scripts an
        /// @c anchor_handover for a family the production card also staged gets the
        /// scripted directive (its @c cold mode is what such a leg tests), and the
        /// election reads exactly one directive per family.
        const InjectAnchorDirective* anchorFor(const std::string& amb) const {
            for (const auto& d : anchor_now)
                if (d.matches(amb)) return &d;
            for (const auto& d : anchor_now_slip)
                if (d.matches(amb)) return &d;
            return nullptr;
        }
        /// @brief True when at least one event fired at least once.
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
     *
     * Absent, disabled or empty => @c ctx.inject.armed stays false and nothing
     * else in the harness is reachable.
     */
    void networkInjectArm(NetworkRunContext& ctx, const GnssConfig& config_,
                          LogManager& log_manager, const std::shared_ptr<spdlog::logger>& log);

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
     * @return 0 when every armed event fired at least once (or the injector is
     *         unarmed), 1 when at least one armed event never fired; the caller
     *         (the run-end table's row, the overload below) returns it as its verdict,
     *         which @c networkTeardown makes the process exit code.
     */
    int networkInjectRunEnd(const NetworkRunContext& ctx,
                            const std::shared_ptr<spdlog::logger>& log);

    // ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the event harness (fault injection).
     *
     * The table's row for the event harness: its clause, and the hard error for a schedule
     * that never fired; the report is the overload above, which a unit test calls directly.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (filter/NetworkRunEnd.hpp).
     * @return the injection verdict: non-zero when an armed schedule never fired, else 0.
     */
    int networkInjectRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
