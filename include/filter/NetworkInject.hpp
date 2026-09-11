/**
 * @file NetworkInject.hpp
 * @brief EVENT HARNESS — the producer-side fault injector seam.
 *
 * ### Why this exists
 *
 * Standing practice on this project is to grade a mechanism by running the
 * network for hours and waiting for nature to supply an event.  That makes the
 * turnaround for a single question ("what does the re-level treadmill actually
 * cost after ONE T4 erase?") a day, and it makes the answer a mixture of the
 * event, its neighbours and the run's own ignition.  The harness inverts it:
 * converge ONCE into a state snapshot (@c network.state_snapshot_*), then resume
 * for a few tens of epochs with exactly one scripted fault armed, and difference
 * against a NULL leg resumed from the same snapshot.  The seam transient, the
 * bed and the posture are common to both legs and cancel.
 *
 * ### Where it acts, and why there
 *
 * Four of the six kinds are corruptions of the producer's INPUT and are applied
 * to a per-epoch mutable copy of the @c SyncedEpoch, at the very top of the
 * epoch body — before @c networkFastClockTick, before @c computeModels, before
 * every T0 detector, before the raw-observable pass and before assembly.  That
 * single site is sufficient because everything downstream is derived from it:
 * @c PppModel::computeModels builds BOTH @c PppObservation::observed (phase in
 * METRES, via the wavelength) and @c PppObservation::raw_observed (phase in
 * CYCLES, what MW / GF / IF-TD read) from the same @c RinexObservation
 * @c measurements entry.  Mutating one scalar there therefore moves the slip
 * detectors and the design matrix together and consistently; mutating
 * @c modeled after the fact would need a dual write, a wavelength lookup, and a
 * site inside an OpenMP region.
 *
 * Two of them, @c sat_clock_step and @c rx_clock_step, are applied at the same
 * site and this is EXACT, not an approximation.  The uncombined residual is
 * @f$ y = \tilde{L} - (\rho + c\,\delta t_r - c\,\delta t^s + \ldots) @f$, so
 * adding @f$ s @f$ metres to every code observable and @f$ s/\lambda @f$ cycles
 * to every phase observable
 *
 *   * of ONE SATELLITE at every station is adding @f$ s @f$ to the a-priori
 *     satellite clock @f$ c\,\delta t^s @f$ (@c sat_clock_step); and
 *   * of EVERY SATELLITE at ONE STATION is adding @f$ s @f$ to that station's
 *     receiver clock @f$ c\,\delta t_r @f$ (@c rx_clock_step) —
 *
 * the same scalar write swept over the other index of the same residual, which
 * is the whole duality between the two.  Both are PERSISTENT steps (the delta is
 * re-applied every epoch from @c tow on) because that is what a clock which has
 * jumped does; neither is a one-epoch spike.  Doing it here keeps the injector
 * out of @c ModelGeometry, which has no access to the run context.
 *
 * The two remaining kinds are not observation faults at all — they force a
 * DECISION the producer would otherwise make on the data — so they are staged
 * here and consumed by the seam that owns that decision:
 *
 *   * @c anchor_handover  -> @c networkElectPhaseAnchors (filter/NetworkPhaseDatum)
 *   * @c arc_erase        -> the T4 attribution refloat (filter/NetworkRaim)
 *
 * ### Byte-identity
 *
 * @c network.inject absent (or @c enable false, or an empty event list) leaves
 * @c NetworkInjectState::armed false.  @c networkInjectEpoch then returns a
 * reference to the caller's own @c SyncedEpoch — no copy, no branch taken, no
 * counter moved — and every consumer sees a null directive pointer.  The
 * manifest keys are emitted only when armed (the @c mdb precedent), so an
 * unarmed run's manifest bytes are unchanged too.
 *
 * ### Never-fired is a HARD error
 *
 * Verbatim the simulator's scripted-slip rule (@c ScriptedSlips::unfinished,
 * @c src/sim/main.cpp): an armed event that never fired fails the run at
 * run-end, by name.  A harness whose fixture silently did not exist is worse
 * than one that refuses to finish — the leg would otherwise be graded as a
 * null and reported as "the event had no effect".
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
    class PppUtils;                // common/PppUtils.hpp (held by shared_ptr: incomplete is fine)
    struct NetworkRunContext;
    class NetworkRunTraces;
    struct SyncedEpoch;            // rinex/MultiRinexSync.hpp — namespace-level

    /**
     * @brief A staged @c anchor_handover directive, consumed by the anchor election.
     *
     * Populated by @c networkInjectEpoch on the epoch the event fires and cleared
     * at the top of the NEXT epoch, so it is a strictly one-epoch object.  The
     * election reads it through a @c const pointer that is @c nullptr on every
     * unarmed run and on every epoch the event is not due.
     */
    struct InjectAnchorDirective {
        std::string sat;            ///< Satellite whose phase-anchor families are forced.
        std::string signal{ "all" };///< "all", or one RINEX-3 phase code.
        bool        cold{ false };  ///< Force the COLD (re-realize + re-reference) branch.
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
        /// Families that took the COLD pin although the directive did NOT ask for it:
        /// the successor had no ledger entry, or one too poorly determined to be a
        /// datum.  A cold outcome is a real gauge re-realization — invariant #7's
        /// re-coupling edge — so it is COUNTED here and never folded into the
        /// continuous total (docs/SPEC_event_harness.md §12.7).
        mutable int n_cold_fallback{ 0 };
        /// Families for which the ban left NO usable candidate at all, so the
        /// election pinned nobody this epoch.  The decoupling edge with no
        /// re-coupling: counted, never silent.
        mutable int n_no_successor{ 0 };
    };

    /**
     * @brief A staged @c arc_erase directive, consumed by the T4 attribution
     *        block (production modes) or by the PASS-A slip drains (@c free_epoch).
     *
     * @c mode is the DIAGNOSTIC sub-knob of Study 1 (docs/SPEC_event_harness.md
     * §9).  The erase path performs FOUR actions the detector refloat does not,
     * and the study asks which of them owns the persistent level floor an
     * unnecessary erase leaves behind:
     *
     *   | mode              | LocalCarry entry | LOS iono carry | pending-reseed | C3c queue | free-epoch latch |
     *   |-------------------|------------------|----------------|----------------|-----------|------------------|
     *   | @c erase (default)| ERASED           | co-erased      | marked         | queued    | no               |
     *   | @c erase_keep_iono| ERASED           | KEPT           | marked         | queued    | no               |
     *   | @c erase_no_reseed| ERASED           | co-erased      | NOT marked     | queued    | no               |
     *   | @c erase_no_funnel| ERASED           | co-erased      | marked         | NOT queued| no               |
     *   | @c free_epoch     | KEPT             | KEPT           | marked         | (detector)| YES              |
     *
     * Harness-only: the mode gates what the INJECTED erase does.  A real
     * attribution is untouched on every path, and an unarmed run cannot reach
     * any of it.
     */
    struct InjectArcErase {
        std::string station;          ///< Station name (roster-matched at the RAIM/drain site).
        std::string sat;              ///< Satellite id.
        std::string mode{ "erase" };  ///< See the table above; loader-defaulted to "erase".
        /// BAND SCOPE (Study 1 rung 2): the phase signals the erase/free acts on.
        /// EMPTY = every signal of that satellite, which is what BOTH production
        /// paths do and what the harness did before this field existed.  A
        /// non-empty set restricts the action, so a healthy arc can be freed on
        /// the same 2-of-3 scope a real IF-TD detector refloat uses.
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
     * Deliberately NOT part of the warm-resume fingerprint or the snapshot: a leg
     * resumes a converged state and then applies its OWN schedule, which is the
     * entire point — two legs off one snapshot differ only in this object.
     */
    struct NetworkInjectState {
        bool armed{ false };                            ///< Any event parsed and enabled.
        std::vector<NetworkInjectEvent> events;         ///< The schedule (config copy).
        std::vector<long long> n_applied;               ///< Per event: epochs it acted on.
        std::vector<bool>      onset_logged;            ///< Per event: onset row already emitted.
        std::map<std::string, long long> fired_by_kind; ///< kind -> events that fired at least once.
        /// Wavelength source for @c sat_clock_step (phase observables are in cycles).
        /// Built once at arm time; null on every unarmed run.
        std::shared_ptr<PppUtils> utils;

        /// This epoch's staged directives (cleared at the top of every epoch).
        std::vector<InjectAnchorDirective> anchor_now;
        /// PRODUCTION anchor-slip handovers (network.cycle_slip.anchor_slip_handover,
        /// docs/SPEC_event_harness.md §12.7) — staged by
        /// @ref networkStageAnchorSlipHandovers for every family whose ANCHOR arc a
        /// slip response freed this epoch.  Deliberately a SECOND vector rather than
        /// a push into @c anchor_now: the injector's firing ledger, its never-fired
        /// hard error and its INJECT event rows all iterate @c anchor_now and must
        /// keep seeing exactly the scripted directives.  Same one-epoch lifetime,
        /// cleared at the same point — but it lives on UNARMED runs too, so the
        /// clear sits ahead of @c networkInjectEpoch's armed guard.
        std::vector<InjectAnchorDirective> anchor_now_slip;
        /// This epoch's staged arc-erase directives (station, satellite, mode).
        std::vector<InjectArcErase> arc_erase_now;
        long long n_arc_erase_acted{ 0 };  ///< Chords actually refloated by injected erases.
        /// Study-1 path census: mode -> chords the injected event actually acted on.
        /// A zero here on an armed leg is invariant #4's "the wiring is inert",
        /// which is exactly the confound the sub-knobs exist to rule out.
        std::map<std::string, long long> arc_erase_mode_acted;

        /// @brief Directive covering @p amb, or @c nullptr.  Cheap on the empty path.
        ///
        /// The INJECTED schedule is consulted FIRST: a harness leg that scripts an
        /// @c anchor_handover for a family the production card also staged must get
        /// the scripted directive (its @c cold mode is the whole point of such a
        /// leg), and the election reads exactly one directive per family.
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
     * Absent / disabled / empty => @c ctx.inject.armed stays false and nothing
     * else in the harness is reachable.
     */
    void networkInjectArm(NetworkRunContext& ctx, const GnssConfig& config_,
                          LogManager& log_manager, const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief Apply this epoch's observation-side faults and stage its directives.
     *
     * @param ctx      Run context (schedule + ledger + staged directives).
     * @param log      Logger (onset lines).
     * @param traces   Trace bundle; one @c network_events.csv row per discrete action.
     * @param se_raw   The epoch as synchronised from the input files.
     * @param scratch  Caller-owned buffer that receives the mutated copy.
     * @return @p se_raw itself when the injector is unarmed or has nothing due
     *         this epoch (NO copy is taken — the byte-identical path), otherwise
     *         a reference to @p scratch holding the corrupted epoch.
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
     * @brief Run-end clause + the never-fired hard error.
     *
     * @return 0 when every armed event fired at least once (or the injector is
     *         unarmed), 1 when at least one armed event never fired — the caller
     *         (@c networkTeardown) returns it as the process exit code.
     */
    int networkInjectRunEnd(NetworkRunContext& ctx,
                            const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
