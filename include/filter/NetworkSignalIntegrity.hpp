/**
 * @file NetworkSignalIntegrity.hpp
 * @brief FLICKER / GAP SIGNAL INTEGRITY — per-line-of-sight band-state
 *        classifier, degraded/unvetted phase demotion, and regain refloat + re-vet.
 *        Card: docs/SPEC_flicker_gap_integrity.md.
 *
 * ## The epistemics (why this is not a detector)
 *
 * Across any loss of lock the slip magnitude is **unbounded and unknowable**.  The
 * phase counter is arbitrary at re-acquisition, and a PROPORTIONAL L1/L2 counter
 * reset is invisible to BOTH the geometry-free and the Melbourne-Wübbena
 * combinations — there is no observable that bounds it.  So this seam does not try
 * to detect anything.  It tracks WHICH LINES OF SIGHT ARE CURRENTLY MONITORABLE and
 * withholds trust from the rest, which is a strictly weaker and strictly checkable
 * claim.
 *
 * The blind window is STRUCTURAL, not a tuning miss:
 *   * MW needs Welford history (≥ 2 time-difference samples),
 *   * IF-TD needs 3 consecutive epochs,
 *   * LLI is receiver-discretionary (the measured case below is flagged ONCE in
 *     three slips), and
 *   * GF is doctrine-barred at 30 s (`feedback_slip_detectors_by_rate`).
 * For the first epochs after ANY recovery, therefore, NOTHING can operate — and
 * that is exactly where a slip lands, because a recovery is what a slip is.
 *
 * ## The measured case
 *
 * G21 @ MBAR00UGA (JAVAD TRE_3 DELTA), DOY-129 19:21-19:31, raw RINEX:
 *
 * | time     | event                                   | GF step            | flag |
 * |----------|-----------------------------------------|--------------------|------|
 * | 19:22:00 | mid-arc slip                            | −1.42 m ≈ −6 L2W c | none |
 * | 19:25:00 | L2W missing (L1C present, healthy S1C)  | LOS dropped        | —    |
 * | 19:25:30 | re-entry                                | −1.44 m vs pre-gap | LLI  |
 * | 19:26:00 | post-re-entry slip                      | −0.99 m ≈ +4 L2W c | none |
 *
 * A weak semi-codeless L2W loop (S2W 28-39 dB) slipping near-integer counts
 * repeatedly, receiver-flagged ONCE.  Both unflagged slips fell in windows where
 * MW and IF-TD were structurally blind (arc 0-1 epochs old).  Class scale: 2482
 * (station, satellite) re-entries in ONE 24 h at 63 stations (~1.7 per epoch) —
 * this is constant weather, not a corner case.
 *
 * ## What this seam does (four increments, one master arm)
 *
 *  - **INC-0 `census`** (the master): classify every present LOS each epoch and
 *    count the BEHAVIOUR — re-entries bucketed by outage length, band losses and
 *    regains, DEGRADED continuations (a dual-born arc still contributing with fewer
 *    bands), standing single-frequency arcs, and LOS disappearances split by
 *    whether the arc was dual.  Diagnostics only; it answers the spec's open audit
 *    question with DATA rather than with a code reading.
 *  - **INC-1**: the classifier itself — @ref SignalIntegrityState.
 *  - **INC-2**: while a LOS is not STABLE its PHASE rows fold at an inflated σ
 *    (`unvetted_sigma_scale`), or — non-default — its phase observables are
 *    WITHHELD for the epoch (`withhold_instead`).  **CODE rows are untouched**:
 *    they are slip-immune and they are what re-anchors the float
 *    (`feedback_prefer_fallback_over_drop` — deweight, never hard-exclude).
 *  - **INC-3**: a band that returns without a producer-registered gap refloats the
 *    LOS's ambiguities on ALL bands through the EXISTING slip funnel
 *    (`feedback_reset_over_repair_biasless` — reset, never repair, because nothing
 *    can bound the size), and STABLE is only re-earned after `revet_ep` consecutive
 *    full-band epochs.
 *
 * ## Ordering contract
 *
 * `networkSignalIntegrity` MUST run
 *   * AFTER the PASS-A slip scans and their union into the funnel — INC-3 INSERTS
 *     into `carry_slipped_ambs` / `carry_slipped_raw`, and every consumer
 *     (`networkAnchorHygiene`, the T5v2 deviation erase in PASS B, the C3c DD-prior
 *     build, the Card-G gap classifier) must see those insertions;
 *   * AFTER `networkAssemblyPrefilter_` — the census must count the LOS set that
 *     actually folds rows, not the pre-prune set (a satellite erased by the
 *     two-station observability gate is not a signal-integrity event); and
 *   * BEFORE the phase-datum census / election and BEFORE PASS B — the WITHHOLD
 *     disposition prunes the withheld station's satellite phase-bias states so the
 *     election never elects a pin whose supporting row this seam just removed, and
 *     the INC-2 σ map must exist before the parallel produce reads it.
 *
 * That window is a single point in the driver: immediately after
 * `networkAssemblyPrefilter_`.  It is deliberately NOT the PASS-B serial pre-pass,
 * which runs AFTER the election (the eq34 hygiene card's recorded deviation #1).
 *
 * It owns NO state: the per-LOS band history lives in @ref NetworkCarryState, the
 * per-epoch σ map on @ref NetStation, and every counter in
 * @ref NetworkServiceCounters.  With `ctx.knobs.sig_integrity_on` false it returns
 * immediately, having touched nothing — no map, no row weight, no observable.
 */
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/PppObservation.hpp"
#include "filter/NetworkRunTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class NetworkRunContext;
class NetworkRunTraces;
struct GnssTime;

/**
 * @brief This epoch's band state of one (station, satellite) line of sight.
 *
 * The five states are mutually exclusive and are decided in the order listed: a
 * gap dominates a band event (an arc that was ABSENT and returns with fewer bands
 * is a REENTRY, not a DEGRADED continuation), and a band event dominates the
 * steady state.
 */
enum class SignalIntegrityState {
    Stable,        ///< Full reference band set for @c revet_ep consecutive epochs — the only state that carries full phase weight.
    New,           ///< First sighting: no prior record at all.  Unvetted by construction (nothing can have monitored it).
    Reentry,       ///< Absent for >= 1 processed epoch and now present.  The classic loss-of-lock recovery.
    FlickerRegain, ///< A reference band was missing while the LOS itself stayed PRESENT, and has now returned inside @c flicker_max_ep — the case no producer-registered gap exists for.
    Degraded       ///< Currently contributing with FEWER than its reference band set (a band is missing but still inside the flicker window).
};

/// Human-readable tag for @ref SignalIntegrityState (events / logs / tests).
const char* signalIntegrityStateName(SignalIntegrityState s);

/**
 * @brief Cross-epoch record for one (station, satellite) line of sight.
 *
 * Lives in @c NetworkCarryState::si_arc, keyed by (app station index, satellite id).
 * Deliberately keyed per LOS rather than per (satellite, signal) arc: across a loss
 * of lock EVERY band's counter is arbitrary at once (the measured G21 case carries
 * LLI=1 on both), so demoting one band while trusting the other would be exactly the
 * absence-of-evidence claim this card exists to refuse.  The per-band bookkeeping it
 * still needs — which reference band is missing, and since when — lives inside the
 * record.
 *
 * NOT serialized into the state snapshot.  A resumed leg's first epoch therefore
 * sees every LOS as a first sighting and re-vets it, which is the CONSERVATIVE
 * direction: a resume IS a window in which no time-difference detector can operate,
 * so demoting through it is correct rather than merely tolerable.  (Contrast the
 * eq34 arc-age map, whose resume rule had to SEED mature because blocking every
 * reattach for min_arc_ep epochs would have been an active harm.)
 */
struct SignalIntegrityArc {
    /// The vetted reference PHASE signal set ("L1C", "L2W", …).  Re-established
    /// wholesale on a first sighting and on a re-entry; GROWN when a new band
    /// appears; REDUCED when a band has been gone longer than @c flicker_max_ep, so
    /// a genuinely single-frequency arc re-stabilises on its reduced set instead of
    /// being demoted forever (spec §7 defers the standing-SF policy to after the
    /// census).
    std::set<std::string> ref_bands;
    /// PHASE signals present at the last epoch this LOS was seen.
    std::set<std::string> bands;
    /// Reference band -> the processed-epoch index it first went missing while the
    /// LOS itself stayed present.  A band's return from here is a FLICKER-REGAIN.
    std::map<std::string, long long> missing_since;
    /// Processed-epoch index of the last epoch this LOS was present (-1 = never).
    long long last_seen_ep = -1;
    /// Consecutive FULL-BAND epochs accumulated since the last transition.  The
    /// demotion tests `vet < revet_ep` BEFORE this epoch's increment, so the
    /// transition epoch itself is demoted and full weight resumes the epoch AFTER
    /// the vet completes (the spec's §6D "demoted through 19:27:00 (K=3)" arithmetic).
    int  vet = 0;
    /// This LOS has been observed with >= 2 phase bands at some point — the latch
    /// that separates "degraded mid-arc" (the hazard) from "born single-frequency"
    /// (a different, deferred policy question).
    bool ever_dual = false;
    /// This arc has completed a re-vet at least once in this run, i.e. it has
    /// CONVERGED and its carry now holds something a slip could poison.
    ///
    /// The demotion is gated on it (spec ADDENDUM, Aug-1): a phase row's damage
    /// potential is its weight times the TIGHTNESS of the state it updates, and a
    /// never-converged arc has none — its ambiguity is diffuse (σ 1000 m), it has no
    /// accumulated carry chord, and "slip at epoch 1" is not even defined.  The
    /// priors ARE the demotion there, so vetting a brand-new arc buys nothing and
    /// costs convergence.  This is what keeps a COLD START exempt by principle
    /// rather than by a special case: at cold start every arc is un-converged, so
    /// nothing is demoted, while mid-run the same rule still exempts a genuinely
    /// rising satellite.  (The opposite case — a WARM RESUME, where carries are
    /// tight but detector state is gone — cannot be seen per-arc at all and is
    /// handled by @c NetworkCarryState::si_resume_warmup_ep.)
    bool ever_stable = false;
    /// One-shot: this arc has already been counted into @c standing_sf_arcs.
    bool sf_counted = false;
    /// One-shot per DEGRADED window: the entry event has been emitted, so the events
    /// CSV records transitions rather than one row per degraded epoch.
    bool degraded_open = false;
};

/**
 * @brief Run tallies for the card (standing invariant #4 — counters or it didn't
 *        happen).  Embedded in @c NetworkServiceCounters as `sig`.
 *
 * @c epochs and @c los_epochs are the DENOMINATORS.  An armed run reporting 0
 * classifier passes never ran the census at all — which is NOT the same as "the
 * census ran and the network was quiet", and no amount of clean-looking log output
 * would reveal the difference.
 *
 * The census block (INC-0) is deliberately split so it can be read against the
 * Card-G resumption totals as a cross-check (spec §6C): @c reentries must track
 * `SlipQcCounters::gap_resumptions` on the same run, because both count the same
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
    long long reentry_gap_1 = 0;      ///< …of those, a ONE-epoch outage (the flicker-adjacent class).
    long long reentry_gap_2_5 = 0;    ///< …2-5 epochs.
    long long reentry_gap_gt5 = 0;    ///< …more than 5.
    long long band_lost = 0;          ///< a reference band went missing while the LOS stayed PRESENT.
    long long band_regain = 0;        ///< …and returned inside flicker_max_ep (FLICKER-REGAIN).
    long long band_dropped_perm = 0;  ///< …did NOT return inside the window: the reference set was REDUCED.
    long long band_gained = 0;        ///< a band outside the reference set appeared (a rising third frequency, or a re-established band past the window).
    long long degraded_epochs = 0;    ///< LOS-epochs contributing with FEWER than the reference band set.

    // --- INC-0 census: the audit question (spec §1 INC-0 behaviour inconsistency) ---
    /// LOS-epochs on which an EVER-DUAL arc contributed with fewer than two phase
    /// bands.  **This is the audit answer.**  A large number means the producer
    /// CONTINUES a degraded arc single-frequency (an unmonitorable phase at full
    /// weight, no gap registered) — the worst case the spec named.  Near zero
    /// alongside a large @c los_vanished_dual means it drops the LOS instead.
    long long sf_continuation_epochs = 0;
    long long los_vanished = 0;       ///< a LOS present at the previous epoch is absent now, at a station that IS present (so this is a LOS event, not a station outage).
    long long los_vanished_dual = 0;  ///< …of those, arcs that were DUAL at their last epoch — the "dropped on band loss" candidate class.
    long long standing_sf_arcs = 0;   ///< DISTINCT arcs never once observed with >= 2 phase bands (the BSHM/G07 class).
    long long standing_sf_epochs = 0; ///< LOS-epochs contributed by those arcs.
    long long no_phase_epochs = 0;    ///< LOS-epochs with CODE only (no phase observable at all).

    // --- INC-2: the demotion ---
    long long unvetted_los_epochs = 0;  ///< LOS-epochs inside the vet window (the demotion's opportunity count).
    /// …of those, LOS-epochs EXEMPTED because the arc has never converged in this run
    /// (spec ADDENDUM): no warm carry exists for a slip to poison, so the arc's own
    /// diffuse priors already are the demotion.  Expected to DOMINATE on a cold-start
    /// leg — every arc is un-converged for its first 1 + revet_ep epochs — and to be
    /// small in steady state, where only genuinely rising satellites qualify.  Read it
    /// against `new_arcs`, not absolutely.
    long long birth_exempt_los_epochs = 0;
    long long resume_warmup_los_epochs = 0;  ///< LOS-epochs demoted by the RESUME WARMUP override rather than by their own band state.
    long long demoted_los_epochs = 0;   ///< …with a σ scale actually published for the epoch.
    long long demoted_rows = 0;         ///< PHASE rows folded at the demoted weight (summed from the per-station counters at teardown — the ROW-level proof the map was READ, not merely written).
    long long withheld_los_epochs = 0;  ///< …withheld instead (phase observables removed for the epoch).
    long long withheld_rows = 0;        ///< phase observables removed.
    long long withheld_pb_pruned = 0;   ///< satellite phase-bias states pruned alongside a withhold so the election cannot elect a pin with no supporting row.

    // --- INC-3: the refloat + the re-vet ---
    long long refloat_regain = 0;         ///< LOS refloats driven by a FLICKER-REGAIN (the EVENT count).
    /// …ambiguity keys actually INSERTED into the Card-4 slip funnel.  Kept separate
    /// from the event count because the two feeds have disjoint gates
    /// (`carry_invariant` here, `carry_warm` for @c refloat_regain_reseeds): on the
    /// FT63 lineage phase-leveling is OFF, so only this one can fire, and reporting
    /// a single fused number is exactly limitation L3 that the slip-QC card had to
    /// go back and split.
    long long refloat_regain_ambs = 0;
    long long refloat_regain_reseeds = 0; ///< …ambiguity keys marked pending-reseed (the phase-leveling feed).
    /// Re-entries whose ambiguity keys were ALREADY in the slip funnel / marked
    /// pending-reseed when this seam ran — i.e. a PASS-A detector (MW or LLI) had
    /// caught the recovery.  The spec's premise ("true REENTRY already refloats")
    /// predicts this dominates; it is asserted here rather than assumed.
    long long reentry_already_refloated = 0;
    long long reentry_unrefloated = 0;    ///< …and the complement.  A LARGE value FALSIFIES the premise and is the headline number of the §6C census leg — this card counts it and deliberately does NOT act on it (spec §2 INC-3a: "no change, but assert it with a counter").
    /// Re-entries whose StateManager ambiguity state SURVIVED the outage.  Expected
    /// ZERO: `MeasurementModel::ensureRequiredStates` prunes every ambiguity whose
    /// phase observable is absent, unconditionally and with no grace epoch, so an
    /// absent arc's state is gone by construction and the arc re-enters
    /// code-anchored-fresh AT THE STATE LEVEL.  Nonzero would mean that prune did
    /// not fire and the whole re-entry premise needs re-deriving.  (What CAN bridge
    /// an outage is the phase-leveling / invariant-DD CARRY, one level up — that is
    /// Card G's instrument and this card does not duplicate it.)
    long long reentry_amb_state_live = 0;
    /// LLI bit 0 asserted on a RE-ENTRY epoch (spec §2 INC-3c audit).
    long long lli_at_reentry = 0;
    /// …of those, whose ambiguity key reached a refloat funnel.  Structurally at
    /// risk of being BELOW @c lli_at_reentry: the MW/LLI drain matches an assertion
    /// against `StateManager::getAmbiguities()`, and a re-entering arc's ambiguity
    /// has just been pruned — so the drain can find nothing to mark even though the
    /// receiver reported loss of lock, while the invariant-DD CHORD for that arc
    /// (keyed independently of the StateManager) is still carried.  A gap between
    /// these two counters is that hole, measured.
    long long lli_at_reentry_funnelled = 0;
    long long vet_completions = 0;        ///< LOS re-vets completed (a LOS returned to full phase weight).
};

/**
 * @brief Classify this epoch's lines of sight, publish the INC-2 demotion, and run
 *        the INC-3 regain refloat.  See the file banner for the ordering contract.
 *
 * @param ctx                [in/out] band-state map, counters, knobs.
 * @param config_            read for nothing but the doxygen contract (modes are
 *                           already resolved into `ctx.knobs`) — kept in the
 *                           signature for seam-contract uniformity.
 * @param log                producer logger (debug / warn).
 * @param traces             SIGNAL-tier `trace_events` rows.
 * @param t                  this epoch's time (event stamps).
 * @param epoch_idx          the monotonic PROCESSED-epoch index.  Deliberately the
 *                           SAME counter Card G measures gaps in
 *                           (`SlipQcChannel::epoch_idx`), so the two instruments'
 *                           re-entry totals are directly comparable — which is the
 *                           spec's §6C acceptance test.
 * @param stations           [in/out] non-const: the σ map is republished per station,
 *                           and a WITHHELD key's phase-bias state is pruned exactly
 *                           as the coverage gate prunes a thin key's.
 * @param present            this epoch's presence mask.
 * @param modeled            [in/out] per-station observations.  Read for the band
 *                           census; under `withhold_instead` an unvetted LOS's PHASE
 *                           entries are ERASED from `observed` (never from
 *                           `raw_observed`, which the slip detectors own).
 * @param carry_slipped_ambs [in/out] the epoch's slip funnel — read to recognise an
 *                           already-refloated arc, written by INC-3.
 * @param carry_slipped_raw  [in/out] the RAW-detector provenance subset (Iono-2 v3k).
 *                           A regain is a RAW observable event — a band physically
 *                           vanished and came back — so it belongs here too, or the
 *                           deviation carry would D5-KEEP a level across a loss of
 *                           lock.
 * @param carry_invariant    the Card-4 funnel is live (gates the funnel insertion).
 * @param carry_warm         the phase-leveling carry is warm (gates pending-reseed).
 */
void networkSignalIntegrity(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            NetworkRunTraces& traces,
                            const GnssTime& t,
                            long long epoch_idx,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            std::vector<std::vector<PppObservation>>& modeled,
                            std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::pair<int, std::string>>& carry_slipped_raw,
                            bool carry_invariant, bool carry_warm);

/**
 * @brief The PHASE signal set of one observation ("L…" keys of @c observed).
 *
 * Exposed for the unit suite (and used by the seam itself) so "which bands does this
 * LOS contribute this epoch" has exactly ONE definition.  Reads @c observed — the
 * SELECTED set that becomes rows — never @c raw_observed, which carries every
 * tracked channel whether or not the filter uses it.
 */
std::set<std::string> signalIntegrityPhaseBands(const PppObservation& obs);

} // namespace GnssProcessor
