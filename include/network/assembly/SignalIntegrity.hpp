/**
 * @file SignalIntegrity.hpp
 * @brief Flicker/gap signal integrity: a per-line-of-sight band-state classifier,
 *        demotion of degraded or unvetted phase, and refloat plus re-vet on a band
 *        regain.  Spec: docs/SPEC_flicker_gap_integrity.md.
 *
 * ## Why this is not a detector
 *
 * Across a loss of lock the slip magnitude is unbounded and cannot be observed.  The
 * phase counter is arbitrary at re-acquisition, and a proportional L1/L2 counter
 * reset is invisible to both the geometry-free and the Melbourne-Wübbena
 * combinations, so no observable bounds it.  This seam therefore detects nothing.
 * It tracks which lines of sight are currently monitorable and withholds trust from
 * the rest, which is a weaker claim and one that can be checked.
 *
 * The blind window is structural, not a tuning gap:
 *   * MW needs Welford history (at least 2 time-difference samples),
 *   * IF-TD needs 3 consecutive epochs,
 *   * LLI is set at the receiver's discretion (the design case below is flagged on
 *     one of three slips), and
 *   * GF is not used at 30 s (`feedback_slip_detectors_by_rate`).
 * For the first epochs after any recovery no detector can operate, and a recovery
 * is exactly where a slip occurs.
 *
 * ## Design case
 *
 * G21 at MBAR00UGA (JAVAD TRE_3 DELTA), DOY-129 19:21-19:31, raw RINEX (spec §1): a
 * weak semi-codeless L2W loop (S2W 28-39 dB) slipped by near-integer counts three
 * times: a GF step of −1.42 m (about −6 L2W cycles) mid-arc at 19:22:00, −1.44 m
 * across a one-epoch L2W outage (re-entry at 19:25:30, LLI set), and −0.99 m
 * (about +4 L2W cycles) at 19:26:00.  Only the re-entry carried an LLI flag.  Both
 * unflagged slips fell where MW and IF-TD were blind (arc 0-1 epochs old).  The
 * class is routine: 2482 (station, satellite) re-entries in 24 h at 63 stations,
 * about 1.7 per epoch.
 *
 * ## What this seam does (four increments under one master switch)
 *
 *  - INC-0, `census` (the master switch): classify every present LOS each epoch and
 *    count re-entries bucketed by outage length, band losses and regains, degraded
 *    continuations (a dual-born arc still contributing with fewer bands), standing
 *    single-frequency arcs, and LOS disappearances split by whether the arc was
 *    dual.  Diagnostics only; the counts answer the spec's audit question from data
 *    rather than from a reading of the code.
 *  - INC-1: the classifier, @ref SignalIntegrityState.
 *  - INC-2: while a LOS is not vetted, its phase rows fold at an inflated σ
 *    (`unvetted_sigma_scale`) or, optionally, its phase observables are withheld for
 *    the epoch (`withhold_instead`).  Code rows are untouched: they are slip-immune
 *    and re-anchor the float (`feedback_prefer_fallback_over_drop`: deweight rather
 *    than hard-exclude).
 *  - INC-3: a band that returns without a producer-registered gap refloats the LOS's
 *    ambiguities on all bands through the existing slip funnel
 *    (`feedback_reset_over_repair_biasless`: reset rather than repair, since nothing
 *    bounds the size), and full weight returns only after `revet_ep` consecutive
 *    full-band epochs.
 *
 * ## Ordering contract
 *
 * `networkSignalIntegrity` runs
 *   * after the PASS-A slip scans and their union into the funnel: INC-3 inserts into
 *     `carry_slipped_ambs` / `carry_slipped_raw`, and every consumer
 *     (`networkAnchorHygiene`, the T5v2 deviation erase in PASS B, the C3c DD-prior
 *     build, the gap classifier) must see those insertions;
 *   * after `networkAssemblyPrefilter_`: the census counts the LOS set that actually
 *     folds rows, not the pre-prune set (a satellite erased by the two-station
 *     observability gate is not a signal-integrity event); and
 *   * before the phase-datum census and election and before PASS B: the WITHHOLD
 *     disposition prunes the withheld station's satellite phase-bias states so the
 *     election cannot elect a pin whose supporting row this seam removed, and the
 *     INC-2 σ map must exist before the parallel produce reads it.
 *
 * That window is a single point in the driver, immediately after
 * `networkAssemblyPrefilter_`.  It is not the PASS-B serial pre-pass, which runs
 * after the election (the eq34 hygiene spec's recorded deviation #1).
 *
 * The seam owns no state: the per-LOS band history lives in @ref NetworkCarryState,
 * the per-epoch σ map on @ref NetStation, and every counter in
 * @ref NetworkServiceCounters.  With `ctx.knobs.sig_integrity_on` false it returns
 * immediately and touches no map, row weight or observable.
 */
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/PppObservation.hpp"
#include "network/driver/RunTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp defines it as a struct; the tag
                            // must match, or MSVC mangles the same free function two ways
                            // depending on which header a translation unit saw first
class NetworkRunTraces;
struct GnssTime;

/**
 * @brief This epoch's band state of one (station, satellite) line of sight.
 *
 * The five states are mutually exclusive.  Precedence: New, then Reentry, then
 * FlickerRegain, then Degraded, else Stable.  A gap dominates a band event (an arc
 * that was absent and returns with fewer bands is a Reentry, not a Degraded
 * continuation), and a band event dominates the steady state.
 */
enum class SignalIntegrityState {
    Stable,        ///< Continuous, every reference band present, no regain this epoch.  A band gain or a permanent band drop still resets the vet, so full phase weight also requires the vet count to have reached @c revet_ep (see @ref SignalIntegrityArc::vet).
    New,           ///< First sighting: no prior record.  Unvetted by construction (nothing has monitored it), but exempt from demotion unless a resume warm-up is active, because the arc has never converged.
    Reentry,       ///< Absent for >= 1 processed epoch and now present: the usual loss-of-lock recovery.
    FlickerRegain, ///< A reference band was missing while the LOS itself stayed present, and has now returned within @c flicker_max_ep.  No producer-registered gap exists for this case.
    Degraded       ///< Contributing with fewer bands than its reference set (a band is missing but still within the flicker window).
};

/// Human-readable tag for @ref SignalIntegrityState (events / logs / tests).
const char* signalIntegrityStateName(SignalIntegrityState s);

/**
 * @brief Cross-epoch record for one (station, satellite) line of sight.
 *
 * Lives in @c NetworkCarryState::si_arc, keyed by (app station index, satellite id).
 * Keyed per LOS rather than per (satellite, signal) arc because across a loss of lock
 * every band's counter is arbitrary at once (the G21 design case carries LLI=1 on
 * both bands), so demoting one band while trusting the other would treat absence of
 * evidence as evidence.  The per-band bookkeeping still needed (which reference band
 * is missing, and since when) lives inside the record.
 *
 * Not serialized into the state snapshot.  A resumed leg's first epoch therefore sees
 * every LOS as a first sighting and re-vets it.  That is the conservative direction:
 * a resume is a window in which no time-difference detector can operate.  (The eq34
 * arc-age map takes the opposite resume rule and seeds arcs as mature, because
 * blocking every reattach for min_arc_ep epochs would do active harm.)
 */
struct SignalIntegrityArc {
    /// The vetted reference phase signal set ("L1C", "L2W", …).  Re-established
    /// wholesale on a first sighting and on a re-entry; grown when a new band
    /// appears; reduced when a band has been gone longer than @c flicker_max_ep, so
    /// a single-frequency arc re-stabilises on its reduced set instead of staying
    /// demoted (spec §7 defers the standing single-frequency policy until after the
    /// census).
    std::set<std::string> ref_bands;
    /// Phase signals present at the last epoch this LOS was seen.
    std::set<std::string> bands;
    /// Reference band -> the processed-epoch index at which it went missing while the
    /// LOS itself stayed present.  A band's return from this map is a FlickerRegain.
    std::map<std::string, long long> missing_since;
    /// Processed-epoch index of the last epoch this LOS was present (-1 = never).
    long long last_seen_ep = -1;
    /// Consecutive full-band epochs accumulated since the last transition.  The
    /// demotion tests `vet < revet_ep` before this epoch's increment, so the
    /// transition epoch itself is demoted and full weight resumes the epoch after the
    /// vet completes (the spec's §6D arithmetic: demoted through 19:27:00 at K=3).
    int  vet = 0;
    /// Latch: this LOS has been observed with >= 2 phase bands at some point.  It
    /// separates "degraded mid-arc" (the hazard) from "born single-frequency" (a
    /// separate, deferred policy question).
    bool ever_dual = false;
    /// This arc has completed a re-vet at least once in this run, i.e. it has
    /// converged and its carry holds a value a slip could corrupt.
    ///
    /// The demotion is gated on this flag (spec ADDENDUM).  The damage a phase row
    /// can do scales with its weight and with how tight the state it updates is.  A
    /// never-converged arc has a diffuse ambiguity (σ 1000 m) and no accumulated
    /// carry chord, and a slip at its first epoch is not defined; its priors already
    /// act as the demotion, so vetting it only slows convergence.  This makes a cold
    /// start exempt by rule rather than by a special case: at cold start no arc has
    /// converged, so nothing is demoted, while mid-run the same rule exempts a rising
    /// satellite.  The opposite case, a warm resume with tight carries and no
    /// detector state, cannot be seen per arc and is handled by
    /// @c NetworkCarryState::si_resume_warmup_ep.
    bool ever_stable = false;
    /// One-shot: this arc has already been counted into @c standing_sf_arcs.
    bool sf_counted = false;
    /// One-shot per degraded window: the entry event has been emitted, so the events
    /// CSV records transitions rather than one row per degraded epoch.
    bool degraded_open = false;
};

/**
 * @brief Run tallies for the seam (standing invariant #4: run-end counters are the
 *        telemetry of the armed path).  Embedded in @c NetworkServiceCounters as `sig`.
 *
 * @c epochs and @c los_epochs are the denominators.  An armed run that reports 0
 * classifier passes never ran the census, which differs from a census that ran on a
 * quiet network; log output alone cannot show the difference.
 *
 * The census block (INC-0) is split so it can be read against the gap-classifier resumption
 * totals as a cross-check (spec §6C): @c reentries should track
 * `SlipQcCounters::gap_resumptions` on the same run, since both count the same
 * physical event from independent state.
 */
struct NetworkSignalIntegrityTallies {
    // --- denominators ---
    long long epochs = 0;             ///< classifier passes run.
    long long los_epochs = 0;         ///< (station, satellite) LOS-epochs classified.
    long long arcs_forgotten = 0;     ///< records dropped after a very long absence (memory bound, not a physical event).

    // --- INC-0 census: the transition ledger ---
    long long new_arcs = 0;           ///< first sightings (no prior record).
    long long reentries = 0;          ///< absent >= 1 processed epoch, now present.
    long long reentry_gap_1 = 0;      ///< …of those, a one-epoch outage (the class next to flicker).
    long long reentry_gap_2_5 = 0;    ///< …2-5 epochs.
    long long reentry_gap_gt5 = 0;    ///< …more than 5.
    long long band_lost = 0;          ///< a reference band went missing while the LOS stayed present.
    long long band_regain = 0;        ///< …and returned within flicker_max_ep (FlickerRegain).
    long long band_dropped_perm = 0;  ///< …did not return within the window, so the reference set was reduced.
    long long band_gained = 0;        ///< a band outside the reference set appeared (a rising third frequency, or a band returning after the window).
    long long degraded_epochs = 0;    ///< LOS-epochs contributing with fewer bands than the reference set.

    // --- INC-0 census: the audit question (spec §1 INC-0 behaviour inconsistency) ---
    /// LOS-epochs on which an ever-dual arc contributed with fewer than two phase
    /// bands.  This is the audit answer.  A large value means the producer continues
    /// a degraded arc single-frequency (an unmonitorable phase at full weight, no gap
    /// registered), the worst case the spec names.  A value near zero alongside a
    /// large @c los_vanished_dual means the producer drops the LOS instead.
    long long sf_continuation_epochs = 0;
    long long los_vanished = 0;       ///< a LOS present at the previous epoch is absent now, at a station that is present (a LOS event, not a station outage).
    long long los_vanished_dual = 0;  ///< …of those, arcs that were dual at their last epoch: the "dropped on band loss" candidate class.
    long long standing_sf_arcs = 0;   ///< distinct arcs never observed with >= 2 phase bands (the BSHM/G07 class).
    long long standing_sf_epochs = 0; ///< LOS-epochs contributed by those arcs.
    long long no_phase_epochs = 0;    ///< LOS-epochs with code only (no phase observable).

    // --- INC-2: the demotion ---
    long long unvetted_los_epochs = 0;  ///< LOS-epochs inside the vet window (the demotion's opportunity count).
    /// …of those, LOS-epochs exempted because the arc has never converged in this run
    /// (spec ADDENDUM): no warm carry exists for a slip to corrupt, so the arc's
    /// diffuse priors already act as the demotion.  Expected to dominate on a
    /// cold-start leg, where every arc is unconverged for its first 1 + revet_ep
    /// epochs, and to be small in steady state, where only rising satellites qualify.
    /// Read it against `new_arcs`, not in absolute terms.
    long long birth_exempt_los_epochs = 0;
    long long resume_warmup_los_epochs = 0;  ///< LOS-epochs demoted by the resume warm-up override rather than by their own band state.
    long long demoted_los_epochs = 0;   ///< …with a σ scale published for the epoch.
    long long demoted_rows = 0;         ///< phase rows folded at the demoted weight (summed from the per-station counters at teardown; the row-level evidence that the map was read, not only written).
    long long withheld_los_epochs = 0;  ///< …withheld instead (phase observables removed for the epoch).
    long long withheld_rows = 0;        ///< phase observables removed.
    long long withheld_pb_pruned = 0;   ///< satellite phase-bias states pruned alongside a withhold so the election cannot elect a pin with no supporting row.

    // --- INC-3: the refloat and the re-vet ---
    long long refloat_regain = 0;         ///< LOS refloats driven by a FlickerRegain (the event count).
    /// …ambiguity keys inserted into the invariant-carry slip funnel.  Kept separate from the
    /// event count because the two feeds have disjoint gates (`carry_invariant`
    /// here, `carry_warm` for @c refloat_regain_reseeds); on a lineage without
    /// phase leveling only this feed can fire.
    long long refloat_regain_ambs = 0;
    long long refloat_regain_reseeds = 0; ///< …ambiguity keys marked pending-reseed (the phase-leveling feed).
    /// Re-entries whose ambiguity keys were already in the slip funnel or marked
    /// pending-reseed when this seam ran, i.e. a PASS-A detector (MW or LLI) caught
    /// the recovery.  The spec's premise ("true REENTRY already refloats") predicts
    /// this dominates; the counter measures it instead of assuming it.
    long long reentry_already_refloated = 0;
    long long reentry_unrefloated = 0;    ///< …and the complement.  A large value falsifies the premise and is the headline number of the §6C census leg; this seam counts it and does not act on it (spec §2 INC-3a: "no change, but assert it with a counter").
    /// Re-entries whose parameter-set ambiguity state survived the outage.  Expected
    /// zero: `networkBirthLocalStates` prunes every ambiguity whose
    /// phase observable is absent, with no grace epoch, except keys held by the
    /// soft-LLI / gap bridge.  An absent arc's state is therefore gone and the arc
    /// re-enters code-anchored and fresh at the state level.  A nonzero value means
    /// that prune did not fire (or a held key bridged the outage) and the re-entry
    /// premise needs re-deriving.  (What can bridge an outage is the phase-leveling /
    /// invariant-DD carry one level up; that is the gap classifier's instrument and this tally
    /// does not duplicate it.)
    long long reentry_amb_state_live = 0;
    /// LLI bit 0 asserted on a re-entry epoch (spec §2 INC-3c audit).
    long long lli_at_reentry = 0;
    /// …of those, whose ambiguity key reached a refloat funnel.  This can fall below
    /// @c lli_at_reentry by construction: the MW/LLI drain matches an assertion
    /// against the parameter set's ambiguities, and a re-entering arc's ambiguity
    /// has just been pruned, so the drain finds nothing to mark even though the
    /// receiver reported loss of lock, while the invariant-DD chord for that arc
    /// (keyed independently of the parameter set) is still carried.  The difference
    /// between the two counters measures that hole.
    long long lli_at_reentry_funnelled = 0;
    long long vet_completions = 0;        ///< LOS re-vets completed (a LOS returned to full phase weight).
};

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochPassA;              // network/driver/Epoch.hpp
struct NetworkSlipFunnel;       // network/driver/Epoch.hpp

/**
 * @brief Classify this epoch's lines of sight, publish the INC-2 demotion, and run
 *        the INC-3 regain refloat.  See the file banner for the ordering contract.
 *
 * Epoch form; the explicit form and its forwarding adapter
 * retired (the unit tests call this form through
 * tests/support/NetworkTestRun.hpp).
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces), run.setup.stations (stations). Read: run.config (config_),
 *            run.log (log), run.engines.slip_qc.epoch_idx (epoch_idx),
 *            run.ctx.knobs.carry_invariant (carry_invariant).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se.time (t),
 *           ep.pass_a.present (present), ep.clock.carry_warm (carry_warm).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: [in/out] band-state map, counters, knobs.
 *   - @p config_ = run.config: unused: every mode is already resolved into `ctx.knobs`.  Kept in the signature for seam-contract uniformity.
 *   - @p log = run.log: producer logger (debug / warn).
 *   - @p traces = run.traces: SIGNAL-tier `trace_events` rows.
 *   - @p t = ep.se.time: this epoch's time (event stamps).
 *   - @p epoch_idx = run.engines.slip_qc.epoch_idx: the monotonic processed-epoch index.  It is the same counter the gap classifier measures gaps in (`SlipQcChannel::epoch_idx`), so the two instruments' re-entry totals are directly comparable (the spec's §6C acceptance test).
 *   - @p stations = run.setup.stations: [in/out] the σ map is republished per station, and a withheld key's phase-bias state is pruned the same way the coverage gate prunes a thin key's.
 *   - @p present = ep.pass_a.present: this epoch's presence mask.
 *   - @p modeled = pass_a.modeled: [in/out] per-station observations.  Read for the band census; under `withhold_instead` an unvetted LOS's phase entries are erased from `observed` (never from `raw_observed`, which the slip detectors own).
 *   - @p carry_slipped_ambs = funnel.carry_slipped_ambs: [in/out] the epoch's slip funnel: read to recognise an already-refloated arc, written by INC-3.
 *   - @p carry_slipped_raw = funnel.carry_slipped_raw: [in/out] the raw-detector provenance subset (Iono-2 v3k). A regain is a raw observable event (a band vanished and came back), so it is inserted here too; otherwise the deviation carry would D5-KEEP a level across a loss of lock.
 *   - @p carry_invariant = run.ctx.knobs.carry_invariant: the invariant-carry funnel is live (gates the funnel insertion).
 *   - @p carry_warm = ep.clock.carry_warm: the phase-leveling carry is warm (gates pending-reseed).
 */
void networkSignalIntegrity(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                            NetworkSlipFunnel& funnel);

/**
 * @brief The phase signal set of one observation (the "L…" keys of @c observed).
 *
 * Exposed for the unit suite and used by the seam itself, so "which bands does this
 * LOS contribute this epoch" has one definition.  Reads @c observed (the selected set
 * that becomes rows), never @c raw_observed, which carries every tracked channel
 * whether or not the filter uses it.
 */
std::set<std::string> signalIntegrityPhaseBands(const PppObservation& obs);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the flicker and gap signal integrity.
 *
 * The flicker and gap signal-integrity classifier: arcs, re-entries, band losses, the
 * INC-0 audit answer, the INC-2 vetting and the INC-3 refloats. The demoted-row count is
 * summed from the stations into a local copy of the tallies (it was summed into the run
 * context).
 *
 * Gate: returns 0 at once unless `ctx.knobs.sig_integrity_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSignalIntegrityRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of SignalIntegrity, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct SignalIntegrityCounters {
    // Run tallies of flicker/gap signal integrity; every field is documented on the
    // struct.  Bumped only when ctx.knobs.sig_integrity_on, and reported by
    // networkSignalIntegrityRunEnd (network/assembly/SignalIntegrity.cpp).  `demoted_rows`
    // stays 0 here: the report sums the per-station NetStation::si_rows_demoted counters
    // into its own copy (the fold runs inside the parallel produce, so the row tally
    // cannot live here).
    NetworkSignalIntegrityTallies sig;
};

} // namespace GnssProcessor
