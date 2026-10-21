/**
 * @file SignalIntegrity.hpp
 * @brief Flicker and gap signal integrity for each (station, satellite) line of
 *        sight: a band-state classifier, demotion of unvetted phase, and a refloat
 *        plus re-vet when a band returns.
 *
 * ## Why this is not a detector
 *
 * Across a loss of lock the slip size is unbounded and unobservable.  The phase
 * counter is arbitrary at re-acquisition, and a proportional L1/L2 counter reset is
 * invisible to both the geometry-free and the Melbourne-Wübbena combinations.  This
 * seam therefore detects nothing.  It tracks which lines of sight can currently be
 * monitored and withholds trust from the rest, a weaker claim that can be checked.
 *
 * Every detector is blind for the first epochs after a recovery, which is exactly
 * where a slip occurs:
 *   * MW (Melbourne-Wübbena) needs a Welford history of at least two
 *     time-difference samples.
 *   * IF-TD (ionosphere-free time difference) needs three consecutive epochs.
 *   * LLI (the loss-of-lock indicator) is set at the receiver's discretion; in the
 *     design case below it flagged one slip of three.
 *   * GF (geometry-free) is not used at 30 s sampling.
 *
 * ## Design case
 *
 * G21 at MBAR00UGA (JAVAD TRE_3 DELTA), DOY 129, 19:21 to 19:31, raw RINEX.  A weak
 * semi-codeless L2W loop (S2W 28 to 39 dB) slipped by near-integer counts three
 * times:
 *   * 19:22:00, mid-arc: a GF step of -1.42 m (about -6 L2W cycles).
 *   * 19:25:30, re-entry after a one-epoch L2W outage: -1.44 m, LLI set.
 *   * 19:26:00: -0.99 m (about +4 L2W cycles).
 * Only the re-entry carried an LLI flag.  Both unflagged slips fell where MW and
 * IF-TD were blind (arc age 0 or 1 epochs).  The class is routine: 2482 (station,
 * satellite) re-entries in 24 h at 63 stations, about 1.7 per epoch.
 *
 * ## What the seam does
 *
 * One master switch, `network.signal_integrity.census`, arms four parts:
 *  - Census: each epoch, classify every present line of sight and count re-entries
 *    by outage length, band losses and regains, degraded continuations (a
 *    dual-frequency arc still contributing with fewer bands), standing
 *    single-frequency arcs, and disappearances split by whether the arc was dual.
 *    Diagnostics only.  The counts show from data whether the producer continues a
 *    degraded arc or drops it.
 *  - Classifier: @ref SignalIntegrityState.
 *  - Demotion: while a line of sight is unvetted, its phase rows fold at an
 *    inflated σ (`unvetted_sigma_scale`), or its phase observables are withheld for
 *    the epoch (`withhold_instead`).  Code rows are untouched: they are slip-immune
 *    and re-anchor the float.  Deweighting is preferred to a hard exclusion.
 *  - Regain refloat: a band that returns with no producer-registered gap refloats
 *    the ambiguities of every band on that line of sight through the slip funnel.
 *    Nothing bounds the slip, so the state is reset rather than repaired.  Full
 *    weight returns after `revet_ep` consecutive full-band epochs.
 *
 * ## Ordering contract
 *
 * `networkSignalIntegrity` has one valid position, immediately after
 * `networkAssemblyPrefilter_`:
 *   * After the PASS-A slip scans and their union into the funnel.  The regain
 *     refloat inserts into `carry_slipped_ambs` and `carry_slipped_raw`, and every
 *     consumer (`networkAnchorHygiene`, the SH deviation erase in PASS B, the
 *     DD-prior build, the gap classifier) must see those insertions.
 *   * After `networkAssemblyPrefilter_`, so the census counts the line-of-sight set
 *     that actually folds rows.  A satellite erased by the two-station
 *     observability gate is not a signal-integrity event.
 *   * Before the phase-datum census and election, and before PASS B.  A withhold
 *     prunes the station's satellite phase-bias states, so the election cannot seat
 *     a pin whose supporting row was removed.  The σ map must also exist before the
 *     parallel produce reads it.
 * The PASS-B serial pre-pass does not qualify: it runs after the election.
 *
 * The seam owns no state.  The band history lives in the Los entity
 * (`net::LosState::si_arc`), the per-epoch σ map on @ref NetStation, and every
 * counter in @ref NetworkServiceCounters.  With the master switch off the seam
 * returns at once and touches no map, row weight or observable.
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
struct NetworkRunContext;   // network/state/RunContext.hpp.  The tag must stay `struct`:
                            // MSVC mangles class and struct differently, so a mismatch
                            // gives one function two symbols.
class NetworkRunTraces;
struct GnssTime;

/**
 * @brief This epoch's band state of one (station, satellite) line of sight.
 *
 * The states are mutually exclusive, with precedence New, Reentry, FlickerRegain,
 * Degraded, then Stable.  A gap outranks a band event: an arc that was absent and
 * returns with fewer bands is a Reentry, not a Degraded continuation.  A band event
 * outranks the steady state.
 */
enum class SignalIntegrityState {
    /// Continuous, every reference band present, no regain this epoch.  Full phase
    /// weight also needs the vet count to reach @c revet_ep, and a band gain or a
    /// permanent band drop resets that count (see @ref SignalIntegrityArc::vet).
    Stable,
    /// First sighting, with no prior record.  Unvetted, but not demoted unless a
    /// resume warm-up is active, because the arc has never converged.
    New,
    /// Absent for at least one processed epoch and now present: the usual
    /// loss-of-lock recovery.
    Reentry,
    /// A reference band went missing while the line of sight stayed present, and
    /// returned within @c flicker_max_ep.  The producer registers no gap for this case.
    FlickerRegain,
    /// Contributing with fewer bands than its reference set, with the missing band
    /// still inside the flicker window.
    Degraded
};

/// Lowercase tag for @ref SignalIntegrityState, used in events, logs and tests.
const char* signalIntegrityStateName(SignalIntegrityState s);

/**
 * @brief Cross-epoch record for one (station, satellite) line of sight.
 *
 * Stored as `net::LosState::si_arc` in the Los entity, keyed by `LosId`.  The record
 * is per line of sight rather than per (satellite, signal) arc: a loss of lock
 * leaves every band's counter arbitrary at once (in the design case both bands
 * carry LLI = 1), so trusting one band while demoting the other would treat absence
 * of evidence as evidence.  The per-band bookkeeping that is still needed (which
 * reference band is missing, and since when) lives inside the record.
 *
 * The record is not saved in the state snapshot, so a resumed leg's first epoch sees
 * every line of sight as a first sighting and re-vets it.  That is the conservative
 * direction: no time-difference detector can operate across a resume.  The anchor
 * arc-age map takes the opposite rule and seeds arcs as mature on resume, because
 * blocking every reattach for `eq34_anchor_min_arc_ep` epochs would do harm.
 */
struct SignalIntegrityArc {
    /// The vetted reference phase signals ("L1C", "L2W", ...).  Reset to the present
    /// bands on a first sighting and on a re-entry, grown when a new band appears,
    /// and reduced when a band has been gone longer than @c flicker_max_ep.  The
    /// reduction lets a single-frequency arc re-stabilise on its smaller set instead
    /// of staying demoted.  The policy for standing single-frequency arcs is open.
    std::set<std::string> ref_bands;
    /// Phase signals present at the last epoch this line of sight was seen.
    std::set<std::string> bands;
    /// Each missing reference band, mapped to the processed-epoch index at which it
    /// went missing while the line of sight stayed present.  A band's return from
    /// this map is a FlickerRegain.
    std::map<std::string, long long> missing_since;
    /// Processed-epoch index of the last epoch this line of sight was present
    /// (-1 = never).
    long long last_seen_ep = -1;
    /// Consecutive full-band epochs since the last transition.  The demotion tests
    /// `vet < revet_ep` before this epoch's increment, so the transition epoch is
    /// demoted and full weight resumes the epoch after the vet completes.  With
    /// revet_ep = 3 and 30 s epochs, a transition at 19:25:30 stays demoted through
    /// 19:27:00.
    int  vet = 0;
    /// Latch: seen with two or more phase bands at some point.  Separates "degraded
    /// mid-arc" (the hazard) from "born single-frequency" (a separate, open policy
    /// question).
    bool ever_dual = false;
    /// Set once the arc has completed a re-vet in this run: it has converged, and its
    /// carry holds a value a slip could corrupt.
    ///
    /// The demotion is gated on this flag.  The damage a phase row can do scales with
    /// its weight and with how tight the state it updates is.  A never-converged arc
    /// has a diffuse ambiguity (σ 1000 m) and no accumulated carry chord, and a slip
    /// at its first epoch is undefined.  Its priors already act as the demotion, so
    /// vetting it only slows convergence.  A cold start is therefore exempt by rule,
    /// not by a special case: no arc has converged yet, so nothing is demoted, and
    /// mid-run the same rule exempts a rising satellite.  The opposite case, a warm
    /// resume with tight carries and no detector state, is invisible per arc and is
    /// handled by @c NetworkCarryState::si_resume_warmup_ep.
    bool ever_stable = false;
    /// One-shot: this arc has already been counted into @c standing_sf_arcs.
    bool sf_counted = false;
    /// One-shot per degraded window: the entry event has been emitted, so the events
    /// CSV records transitions rather than one row per degraded epoch.
    bool degraded_open = false;
};

/**
 * @brief Run tallies for the seam, embedded in @c NetworkServiceCounters as `sig`.
 *
 * Run-end counters are the telemetry of the armed path.  @c epochs and
 * @c los_epochs are the denominators: an armed run that reports zero classifier
 * passes never ran the census, which log output alone cannot tell apart from a
 * census on a quiet network.
 *
 * @c reentries cross-checks against `SlipQcCounters::gap_resumptions` on the same
 * run: both count the same physical event from independent state.
 *
 * Counts named "epochs" are line-of-sight epochs unless stated otherwise.
 */
struct NetworkSignalIntegrityTallies {
    // --- Denominators ---
    long long epochs = 0;             ///< Classifier passes run.
    long long los_epochs = 0;         ///< Line-of-sight epochs classified.
    long long arcs_forgotten = 0;     ///< Records dropped after a very long absence (a memory bound, not a physical event).

    // --- Census: transitions ---
    long long new_arcs = 0;           ///< First sightings.
    long long reentries = 0;          ///< Absent for at least one processed epoch, now present.
    long long reentry_gap_1 = 0;      ///< Re-entries after a one-epoch outage (the class next to flicker).
    long long reentry_gap_2_5 = 0;    ///< Re-entries after 2 to 5 epochs.
    long long reentry_gap_gt5 = 0;    ///< Re-entries after more than 5 epochs.
    long long band_lost = 0;          ///< A reference band went missing while the line of sight stayed present.
    long long band_regain = 0;        ///< A missing band returned within flicker_max_ep (a FlickerRegain).
    long long band_dropped_perm = 0;  ///< A missing band stayed away past the window, so the reference set was reduced.
    long long band_gained = 0;        ///< A band outside the reference set appeared (a rising third frequency, or a band returning after the window).
    long long degraded_epochs = 0;    ///< Epochs contributing with fewer bands than the reference set.

    // --- Census: does the producer continue or drop a degraded arc? ---
    /// Epochs on which an arc that was once dual-frequency contributed with fewer
    /// than two phase bands.  A large value means the producer continues a degraded
    /// arc single-frequency, the worst case: unmonitorable phase at full weight with
    /// no gap registered.  A value near zero alongside a large @c los_vanished_dual
    /// means the producer drops the line of sight instead.
    long long sf_continuation_epochs = 0;
    long long los_vanished = 0;       ///< Present at the previous epoch and absent now, at a station that is present (a line-of-sight event, not a station outage).
    long long los_vanished_dual = 0;  ///< Of those, arcs that were dual-frequency at their last epoch: the "dropped on band loss" class.
    long long standing_sf_arcs = 0;   ///< Distinct arcs never seen with two or more phase bands.
    long long standing_sf_epochs = 0; ///< Epochs contributed by those arcs.
    long long no_phase_epochs = 0;    ///< Epochs with code only, no phase observable.

    // --- Demotion ---
    long long unvetted_los_epochs = 0;  ///< Epochs inside the vet window (the demotion's opportunities).
    /// Of those, epochs exempt because the arc has never converged in this run.  No
    /// warm carry exists for a slip to corrupt, and the arc's diffuse priors already
    /// act as the demotion.  Expected to dominate on a cold-start leg, where every
    /// arc is unconverged for its first 1 + revet_ep epochs, and to be small in
    /// steady state, where only rising satellites qualify.  Read it against
    /// @c new_arcs.
    long long birth_exempt_los_epochs = 0;
    long long resume_warmup_los_epochs = 0;  ///< Epochs demoted only by the resume warm-up: the arc itself has never converged and would be exempt.
    long long demoted_los_epochs = 0;   ///< Epochs with a σ scale published.
    long long demoted_rows = 0;         ///< Phase rows folded at the demoted weight, summed from the stations at run end (proof that the σ map was read, not only written).
    long long withheld_los_epochs = 0;  ///< Epochs withheld instead (phase observables removed).
    long long withheld_rows = 0;        ///< Phase observables removed.
    long long withheld_pb_pruned = 0;   ///< Satellite phase-bias states pruned with a withhold, so the election cannot seat a pin with no supporting row.

    // --- Regain refloat and re-vet ---
    long long refloat_regain = 0;         ///< Line-of-sight refloats driven by a FlickerRegain (the event count).
    /// Ambiguity keys inserted into the invariant-carry slip funnel.  Counted apart
    /// from @c refloat_regain_reseeds because the two feeds have independent gates
    /// (`carry_invariant` here, `carry_warm` there).  Without phase leveling only
    /// this feed can fire.
    long long refloat_regain_ambs = 0;
    long long refloat_regain_reseeds = 0; ///< Ambiguity keys marked pending-reseed (the phase-leveling feed).
    /// Re-entries whose ambiguity keys were already in the slip funnel or marked
    /// pending-reseed when this seam ran, meaning a PASS-A detector (MW or LLI)
    /// caught the recovery.  The design assumes this case dominates; the counter
    /// measures it rather than assuming it.
    long long reentry_already_refloated = 0;
    /// The complement: re-entries no detector refloated.  A large value contradicts
    /// the assumption above.  The seam counts these and does not act on them.
    long long reentry_unrefloated = 0;
    /// Re-entries whose ambiguity state in the parameter set survived the outage.
    /// Expected zero: `networkBirthLocalStates` prunes every ambiguity whose phase
    /// observable is absent, with no grace epoch, except keys held by the soft-LLI /
    /// gap bridge.  An absent arc therefore re-enters fresh and code-anchored.  A
    /// nonzero value means that prune did not fire, or a held key bridged the
    /// outage.  The carry one level up (phase leveling, invariant DD) can bridge an
    /// outage; the gap classifier measures that, and this tally does not duplicate it.
    long long reentry_amb_state_live = 0;
    /// Re-entry epochs with LLI bit 0 set on a phase observable.
    long long lli_at_reentry = 0;
    /// Of those, re-entries whose ambiguity key reached a refloat funnel.  This can
    /// fall below @c lli_at_reentry by construction.  The MW/LLI drain matches an LLI
    /// against the parameter set's ambiguities, and a re-entering arc's ambiguity has
    /// just been pruned, so the drain marks nothing even though the receiver reported
    /// loss of lock.  The invariant-DD chord for that arc, keyed independently of the
    /// parameter set, is still carried.  The gap between the two counters measures
    /// that hole.
    long long lli_at_reentry_funnelled = 0;
    long long vet_completions = 0;        ///< Re-vets completed (a line of sight returned to full phase weight).
};

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochPassA;              // network/driver/Epoch.hpp
struct NetworkSlipFunnel;       // network/slip/SlipFunnel.hpp

/**
 * @brief Classify this epoch's lines of sight, publish the demotion, and refloat any
 *        line of sight whose band flickered back.  The file banner states the
 *        ordering contract.
 *
 * Returns at once unless `ctx.knobs.sig_integrity_on`.
 *
 * @param run    The run.  Writes the band records and counters (`run.ctx`) and the
 *               SIGNAL-tier `trace_events` rows (`run.traces`).  Republishes each
 *               station's σ map and, under a withhold, prunes the withheld
 *               phase-bias states the way the coverage gate prunes a thin key's
 *               (`run.setup.stations`).  Reads the logger, the `carry_invariant`
 *               knob (gates the funnel insertion) and the processed-epoch index
 *               `run.engines.slip_qc.epoch_idx`.  The gap classifier counts gaps in
 *               the same index, so the two re-entry totals compare directly.
 * @param ep     The epoch, read only: its time (event stamps), the presence mask
 *               `pass_a.present`, and `clock.carry_warm` (gates pending-reseed).
 * @param pass_a [in/out] Its `modeled` per-station observations are read for the
 *               band census.  Under `withhold_instead`, an unvetted line of sight's
 *               phase entries are erased from `observed`, never from `raw_observed`,
 *               which the slip detectors own.
 * @param funnel [in/out] The epoch's slip funnel.  `carry_slipped_ambs` is read to
 *               recognise an arc already refloated, and the regain refloat inserts
 *               into it.  The refloat also inserts into `carry_slipped_raw`, the
 *               raw-detector subset: a band that vanished and returned is a raw
 *               observable event, and without the entry the deviation carry would
 *               keep its level across a loss of lock.
 */
void networkSignalIntegrity(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                            NetworkSlipFunnel& funnel);

/**
 * @brief The phase signals of one observation (the "L" keys of @c observed).
 *
 * Shared by the seam and the unit tests, so "which bands this line of sight
 * contributes this epoch" has one definition.  Reads @c observed, the selected set
 * that becomes rows, never @c raw_observed, which holds every tracked channel
 * whether or not the filter uses it.
 *
 * @param obs One satellite's observation at one station.
 * @return The phase observable codes present in @c obs.observed.
 */
std::set<std::string> signalIntegrityPhaseBands(const PppObservation& obs);

// ---- Run-end report ----

struct NetworkRunEndView;

/**
 * @brief Run-end report of the flicker and gap signal integrity: arcs, re-entries,
 *        band losses, the census, the demotion and the refloats.
 *
 * The demoted-row count is summed from the stations into a local copy of the
 * tallies.  Returns 0 at once unless `ctx.knobs.sig_integrity_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSignalIntegrityRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end report ----

/// @brief Run-scope counters of the signal-integrity seam, a base of
///        NetworkServiceCounters (network/state/RunContext.hpp).
struct SignalIntegrityCounters {
    // Bumped only when ctx.knobs.sig_integrity_on, and reported by
    // networkSignalIntegrityRunEnd.  `sig.demoted_rows` stays 0 here: the phase rows
    // fold inside the parallel produce, so each station counts them in
    // NetStation::si_rows_demoted and the report sums those into its own copy.
    NetworkSignalIntegrityTallies sig;
};

} // namespace GnssProcessor
