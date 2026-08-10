/**
 * @file NetworkMeasurementAdmission.hpp
 * @brief PRODUCT-REFERENCED MEASUREMENT ADMISSION — the corrupt-channel screen.
 *        Card: docs/SPEC_measurement_admission.md.
 *
 * ## The class no differential screen can see
 *
 * BSHM00ISR emits two corrupt channels on DOY-129: G07 C1C+L1C at **+198.2 km**
 * and G16 at **+99.9 km** against SP3+CLK-predicted range (0.661 / 0.333 ms of
 * light-time, ~2:1, drifting ~-4.8 m/s).  Triangulation from the prefit-census
 * card's §17 gate 6: the same G07 reads +8.0 m at DJIG (the PRODUCTS are fine);
 * eleven other BSHM satellites sit in -12.5…+19.1 m (the station clock and the
 * model are fine); G21@BSHM — also C1C-only — reads -10.1 m (single-band alone is
 * innocent).  The receiver failure mode is not determinable from the RINEX; the
 * guard must work from internal evidence.
 *
 * Because the offset rides BOTH the code and the phase of the channel, it is
 * invisible to every differential check ever run against this data — TD
 * smoothness, phase-minus-code, GF and MW all pass BY CONSTRUCTION.  The chain it
 * feeds is fully established: the state initializer faithfully seeds the corrupt
 * value → on a single-band channel the local redundancy r ≈ 0, so the error
 * vanishes into the free slant state where no residual test can reach it
 * (single-band is the AMPLIFIER, not the cause) → the parked value waits for a
 * topology valve (the 25° tie admission) → +5.7k TECU discharge, 76-minute
 * withhold era, 181 m never-healed scar.
 *
 * Defense in depth, in order of engagement:
 *   **layer 0 (this seam, the origin)** → INC-3b admission-validate (the valve,
 *   `network.iono_dw_admit_validate_k`) → the prefit fuse (symptom bound,
 *   `network.prefit_fuse_sigma`) → the SH health gate (backstop).
 *
 * ## The screen statistic — absolute, coarse, clock-free, STATE-FREE
 *
 * Per (station, satellite, band) CODE observable, every epoch products cover the
 * line of sight:
 * @f[
 *   d_i = C^\text{obs}_i - m_i, \qquad
 *   z_i = d_i - \operatorname{median}_{s \in \text{station}}
 *               \bigl(\operatorname{median}_{i \in s} d_i\bigr)
 * @f]
 * where @f$m_i@f$ is @c PppObservation::modeled — and the load-bearing property
 * of that value is that it is **state-free**.  @c PppModel::computeBaseModeledValue
 * (`src/core/model/PppModel.cpp:1466`) is exactly
 *
 *     range - c*sat_clock - c*relativistic + ZHD*mf_d + antenna + bias
 *           + shapiro + otl + apl + solid_tide   (+ pole tide + higher-order iono)
 *
 * with **no** receiver clock, **no** wet-troposphere state, **no** first-order
 * slant iono and **no** ambiguity in it.  So @f$d_i@f$ is the spec §2 statistic
 * @f$C^\text{obs} - \rho_\text{pred}(\text{SP3}) - c\,\delta t^s(\text{CLK})@f$
 * with the small deterministic corrections additionally removed — a strictly
 * TIGHTER healthy spread, at zero extra cost, with no product re-query and no new
 * dependency in this translation unit.  Recorded as an implementation deviation
 * in the card's §9.
 *
 * That state-freedom is the entire reason this screen can fire at a cell's BIRTH
 * where the prefit census structurally cannot: a prefit innovation subtracts the
 * filter state, and a correctly self-seeded birth has innovation ≈ 0 whatever the
 * observable says.  @f$d_i@f$ does not care that the cell is new.
 *
 * Deliberate design choices, each paid for by a documented field lesson:
 *
 *  - **RAW by intent.**  No tropo-wet / iono / receiver-bias modelling.  Those
 *    terms live INSIDE the healthy spread the threshold must tolerate (measured
 *    at BSHM: -12.5…+19.1 m across eleven satellites, storm conditions included).
 *    This is a structural fault detector, not a residual test — it owns the
 *    @f$10^5@f$ m regime, and Huber / the deweight gate / RAIM own the 3-10σ one.
 *  - **The station median IS the receiver clock** at this precision, so ms-scale
 *    offsets and steps cancel EXACTLY (URUM00CHN and INEG00MEX take clean 1 ms
 *    steps in the FT63 leg window; the prefit census's negative gate validated
 *    precisely this centring).  A raw-|d| screen would fire on those and find
 *    nothing else.
 *  - **The centre is a median over SATELLITES**, each satellite first reduced to
 *    the median of its own code terms.  A flat median over OBSERVABLES weights a
 *    satellite by how many bands it happens to be tracked on, so one satellite on
 *    four bands out-votes three on one and can OWN the centre — at which point
 *    the screen INVERTS (the corrupt satellite reads @f$z \approx 0@f$ and is
 *    admitted; its healthy peers read @f$z \approx |d|@f$ and are rejected) and
 *    the station guard structurally cannot catch it, because the guard fires iff
 *    healthy bands outnumber corrupt ones and the capture needs the opposite.
 *    Reducing per satellite first also makes @c adm_min_sats bound the population
 *    it names: the median's sample size IS the satellite count, exactly as §2
 *    writes it.
 *  - **CODE only, by principle.**  A phase-only constant offset is
 *    indistinguishable from an ambiguity and is therefore GAUGE, not corruption.
 *    Screening code flags the CHANNEL; the response then covers BOTH observables
 *    of that band, because a corrupt channel corrupts everything it carries.
 *  - **Band, not signal.**  The record and the response are keyed by
 *    (satellite, RINEX band digit), so C1C/C1W/L1C are one channel.  A receiver
 *    fault that puts one band 198 km out does not respect attribute letters.
 *
 * ## The three increments (one arming knob for the response)
 *
 *  - **INC-1 — admission at arc birth and at warm-resume restore.**  A channel
 *    whose |z| fails @c network.adm_bound_m at the epoch its arc would be BORN —
 *    or, on a warm resume, at the first restored epoch (a restored corrupt arc is
 *    NOT a birth, and the census card's INC-3 structurally cannot see it) — is
 *    NOT seeded: the band is withheld from the model set for that epoch.  No state
 *    exists yet, so nothing estimable is lost.  This is pure admission — the
 *    coverage-gate / anchor-hygiene hold-out precedent, not a measurement drop.
 *    The birth-vs-restore split is a LABEL: the refloat runs on EVERY rejection,
 *    because "which of the two is this?" is answered from a map that is not
 *    serialized (and whose station may have been absent, thin, or
 *    satellite-pruned on the resume epoch), while re-tapping an arc that has no
 *    carried state is a no-op.  Correctness must not ride a classification that
 *    can be wrong when the alternative is free — standing invariant #1.
 *  - **INC-2 — mid-arc onset and heal.**  An ESTABLISHED channel crossing the
 *    bound has its band withheld for the epoch AND its arc flagged for refloat
 *    (reset-over-repair: the channel's history is untrusted, and nothing bounds
 *    the error, so there is no value to correct toward — only state to discard).
 *    After @c network.adm_heal_ep consecutive in-bound epochs the band is
 *    readmitted through the normal funnel with a FRESH seed; a heal never
 *    restores the old states.  A rejected band re-tests EVERY epoch.
 *  - **INC-3 — the absolute-residual census** (@c network.adm_trace, armable
 *    without the response — measure-only).  Per epoch: the population level and
 *    the extreme of |z| into `network_epoch_status.csv`, with the argmax cell's
 *    identity into an ADMISSION-tier event.  This is the check whose absence let
 *    "raw RINEX verified clean" be honestly wrong for a month of sessions.
 *  - **INC-4 — the arc RE-ENTRY vet** (@c network.adm_revet).  The admission
 *    record outlives an arc: it is kept for @ref kAdmArcForgetEp epochs, so a
 *    channel that leaves the model set and returns hours later is still carrying
 *    `admitted = true` from before the gap and was dispatched down the
 *    ESTABLISHED path.  That is a verdict inherited from a channel which is not
 *    necessarily the one that came back.  Measured on `runs/ft63_24h_conv`:
 *    **16 of 17 mid-arc onsets on the day were preceded by an `arc_resume` within
 *    five minutes** — there were no genuine mid-arc onsets at all.  Twelve fired
 *    at the resume epoch itself (gaps of 3-2025 epochs), so only their LABEL was
 *    wrong — and one of them is G16\@BSHM at TOW 327210, the very channel §1 of
 *    the card is written around, which the gate-2 leg replay calls
 *    `adm_reject_birth` purely because its record map starts empty.  The other
 *    four were re-admitted and crossed the bound 2-8 epochs later, and those
 *    epochs are where the day's two largest iono excursions live (a00 39.7 ->
 *    59.4 TECU at G16\@BSHM, 30.8 -> 43.0 at G13\@BSHM), against a median of
 *    -0.11 TECU for the eight birth rejections.  **Refusing at admission is free;
 *    amputating an established arc is not** — standing invariant #7 at both
 *    edges.  So a return after a gap is re-screened as a BIRTH, and is then VET
 *    for @c network.adm_revet_ep epochs during which a PREFIT-census `obs_code`
 *    exceedance on the line of sight (previous epoch) refuses as surely as the
 *    bound does.  The census named all four exposed cells 1-4 epochs before the
 *    bound reached them; §1 always called the census the detection net, and this
 *    is the wire from it to the response.
 *
 * ## The station guard — never reject off a corrupted centre
 *
 * If more than @c network.adm_max_frac of a station's screened channels fail in
 * one epoch, the MEDIAN itself is untrusted (a badly wrong a-priori position, a
 * product outage, a receiver-wide fault) and no per-channel verdict derived from
 * it means anything.  The response mirrors the IF-TD reference gate's
 * incoherent-station precedent (`IfPhaseSlipDetector.cpp:454` — "fire, do NOT
 * rebase: the median is a mixture, not a common step"): take the STATION-scoped
 * conservative action and REFUSE the per-channel claim.  Concretely — hold every
 * screened band of that station for the epoch, refloat the arcs behind the hole
 * (standing invariant #1: never punch a hole in a stiff time-linked carry without
 * a wired response), emit ONE @c adm_station_event, and leave every per-channel
 * admission record untouched so the next epoch re-tests from evidence rather than
 * from a verdict this one was not entitled to make.
 *
 * ## Ordering contract
 *
 * `networkMeasurementAdmission` MUST run
 *   * AFTER the PASS-A slip scans and their union into the funnel — it INSERTS
 *     into `carry_slipped_ambs` / `carry_slipped_raw`, and every consumer
 *     (`networkAnchorHygiene`, the T5v2 deviation erase in PASS B, the C3c
 *     DD-prior build, the Card-G gap classifier) must see those insertions;
 *   * AFTER `networkAssemblyPrefilter_` — a satellite the two-station
 *     observability gate has just erased must not be screened, evented, or
 *     counted as admitted;
 *   * AFTER `networkSignalIntegrity` — see below; and
 *   * BEFORE the phase-datum census / election, the shared-column registration
 *     and PASS B.  It is **layer 0** of the card's §1 defense in depth (this
 *     seam → the INC-3b tie admission → the prefit fuse → the SH gate), and
 *     every one of those, plus the election, the prefit census and RAIM, must
 *     observe the ADMITTED set rather than the raw one.
 *
 * The window is the single point immediately after `networkSignalIntegrity`.
 * Running BEFORE the flicker classifier instead would land in the same window
 * and work, but it would corrupt that card's instrument: `modeled` is rebuilt
 * from `computeModels` every epoch, so a withhold applied first would make the
 * classifier read a band LOSS this epoch, a `band_dropped_perm` after its
 * flicker window, and a FLICKER-REGAIN on heal — manufacturing exactly the
 * events the flicker card's INC-0 census exists to MEASURE.  Running after it
 * costs nothing (this seam reads only `observed` / `modeled`, which the
 * classifier does not write) and leaves the flicker counters comparable across
 * an admission-armed leg and its control.  The two cards' refloats compose:
 * both land in the same funnel, which is a set.
 *
 * ## Scope discipline
 *
 * Default-OFF; OFF is structurally byte-identical (the seam returns on its first
 * line, allocating no map and touching no observable); and **NOT fingerprinted** —
 * a forward-only admission policy, RAIM-class, so a stored snapshot's state means
 * the same thing either way and an OFF/ON pair on ONE shared snapshot stays
 * A/B-legal.  The exemption is recorded in place at `networkSnapshotFingerprint`.
 *
 * It owns NO state: the per-channel admission records live in
 * @ref NetworkCarryState, and every counter in @ref NetworkServiceCounters.
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
struct NetworkRunContext;
struct NetworkRunTraces;
struct GnssTime;

/// Drop an admission record unseen for this many PROCESSED epochs — purely a
/// memory bound (the key set is stations x satellites x bands, ~36 k at 305
/// stations, so it never binds in practice); one full day at 30 s.  A dropped
/// record makes the channel's eventual return read as a BIRTH rather than a
/// heal-in-progress, which is the CONSERVATIVE direction (it re-screens from
/// scratch), and the drop is counted so the label shift is never silent.
inline constexpr long long kAdmArcForgetEp = 2880;

/// Per-(event, station, satellite, band) event rate limit, in seam passes.
/// Deliberately a constant rather than a knob: the INC-1/INC-2 events are
/// TRANSITIONS (a birth, an onset, a heal) and are self-limiting by construction,
/// so the only two streams that can repeat are the INC-3 census argmax and a
/// chronically-bad station's guard event.  Matches the prefit census's default
/// holdoff so the two event files read at the same cadence.
inline constexpr long long kAdmEventHoldoffEp = 20;

/**
 * @brief Cross-epoch admission record for one (station, satellite, band) channel.
 *
 * Lives in @c NetworkCarryState::adm_arc, keyed by
 * (app station index, "SAT|<band digit>").
 *
 * Keyed per BAND rather than per signal because the response covers both
 * observables of a band and because a receiver fault that displaces a channel by
 * 198 km does not respect RINEX attribute letters — C1C and C1W at one satellite
 * are one physical channel group, and admitting one while rejecting the other
 * would be a distinction the evidence cannot support.
 *
 * NOT serialized into the state snapshot.  A resumed leg's first seam pass
 * therefore sees every channel as unknown — which is precisely the case INC-1's
 * resume clause exists for: the arc's STATE was restored (possibly carrying the
 * corrupt seed) even though this observer's memory was not, so the first restored
 * epoch is screened and a failure is a @c adm_reject_resume, not a birth.
 */
struct MeasurementAdmissionArc {
    /// The channel is currently IN the model set (it passed its last verdict, or
    /// it has healed).  False from the moment a birth/onset rejection lands until
    /// @c clean_run reaches @c network.adm_heal_ep.
    bool      admitted  = false;
    /// Consecutive in-bound epochs accumulated while REJECTED — the heal counter.
    /// Reset to 0 by any failing epoch.  Never advanced on a station-guard epoch:
    /// a verdict from an untrusted centre is not evidence in either direction.
    int       clean_run = 0;
    /// Processed-epoch index of the last epoch this channel was screened
    /// (-1 = never).  Drives @ref kAdmArcForgetEp — and, under
    /// @c network.adm_revet, the RE-ENTRY test: the gap since the previous
    /// screened epoch is the only evidence this seam has that the channel it is
    /// looking at is not the one whose verdict it is holding.
    long long last_ep   = -1;
    /// INC-4: screened epochs the re-entry VET window stays open for
    /// (0 = not vetting).  Set to @c network.adm_revet_ep on a re-entry and on any
    /// refusal inside the window, decremented once per screened epoch.
    ///
    /// While positive, the refusal test is WIDENED — a PREFIT-census `obs_code`
    /// exceedance on the line of sight refuses as surely as the |z| bound does.  It
    /// does not by itself withhold anything: a clean re-entry is admitted on its
    /// return epoch exactly as before, which is what makes the increment inert on
    /// the ~4650 healthy re-entries a 63-station day carries.
    int       vet_left  = 0;
};

/**
 * @brief Run tallies for the card (standing invariant #4 — counters or it did not
 *        happen).  Embedded in @c NetworkServiceCounters as `adm`.
 *
 * @c epochs, @c station_epochs and @c channels are the DENOMINATORS.  An armed run
 * reporting zero screened channels never ran the screen at all, which is NOT the
 * same as "the screen ran and every channel was healthy" — and no amount of clean
 * log output distinguishes them.
 *
 * The reject counts are split by ENTRY POINT (birth / resume-restore / mid-arc
 * onset) rather than fused, because the three answer different questions: a birth
 * rejection says the channel was never admissible, a resume rejection says a
 * corrupt arc was carried across a snapshot boundary, and an onset says a
 * previously-good channel went bad — which is the only one of the three that
 * needs a refloat, and the only one whose absence would falsify the card's
 * "the offset is present from the arc's first epoch" finding.
 */
struct NetworkMeasurementAdmissionTallies {
    // --- denominators -------------------------------------------------------
    long long epochs = 0;             ///< seam passes (0 while armed ⇒ the wiring never executed).
    long long station_epochs = 0;     ///< (station, epoch) blocks with a usable median.
    long long los_epochs = 0;         ///< (station, satellite, epoch) lines of sight visited.
    long long channels = 0;           ///< (station, satellite, band, epoch) CODE channels screened.
    long long skips_thin = 0;         ///< station-epochs SKIPPED: fewer than adm_min_sats satellites carried a finite d, so the median is not a centre.
    long long skips_nonfinite = 0;    ///< code observables skipped: non-finite observed or modeled value (never allowed to poison a median).
    long long arcs_forgotten = 0;     ///< admission records dropped after a very long absence (memory bound, not a physical event).
    long long resume_passes = 0;      ///< seam passes that ran under the warm-resume RESTORE rule (expected 0 or 1 per run).

    // --- INC-1 admission ----------------------------------------------------
    long long births = 0;             ///< channel-epochs with no prior record — the admission opportunity count.
    long long admits_birth = 0;       ///< …admitted (the overwhelming majority; the denominator the two rejects are read against).
    long long reject_birth = 0;       ///< …REJECTED at the epoch the arc would have been born.
    long long reject_resume = 0;      ///< …rejected on the first restored epoch of a warm resume (a restored corrupt arc, not a birth).

    // --- INC-4 the re-entry vet ---------------------------------------------
    // The onset counter above is the one this increment exists to empty.  On the
    // FT63 24 h day, 16 of its 17 hits were re-entries wearing an onset's label; a
    // run with INC-4 armed should move essentially all of them into @c reentries and
    // leave @c onsets holding only channels that genuinely went bad while present.
    // That migration is the increment's primary field readout, which is why the two
    // are counted apart rather than fused.
    long long reentries = 0;          ///< channel-epochs re-screened as a birth because the channel had been ABSENT for at least adm_revet_gap_ep screened epochs.
    long long admits_reentry = 0;     ///< …admitted on the return epoch (the overwhelming majority; no withhold, no refloat, no solve change).
    long long reject_reentry = 0;     ///< …REFUSED on the return epoch — the twelve of the FT63 day that were previously mislabelled as onsets.
    long long revet_window_ep = 0;    ///< channel-epochs of ADMITTED operation inside an open vet window — i.e. epochs that actually spent vet budget.  A WITHHELD epoch is deliberately not counted and does not tick the window down: it is not evidence of recovery, and letting a rejected channel serve out its window in absentia would return it UNVETTED.  Under the default posture this is greater than @ref revet_census_reads by exactly one per re-entry (the return epoch is admitted but takes the birth-equivalent path, which asks the census nothing — and could not usefully, since a re-entering arc's fresh ambiguity makes the census defer it as a birth on exactly that epoch).  Under @c adm_revet_hold the return epoch is withheld instead, so that identity does not hold there.
    long long revet_bound_refusals = 0;   ///< refusals made by the TIGHTER re-entry bound (@c network.adm_revet_bound_scale) that @c adm_bound_m would not have made — at a return epoch or inside a vet window.  Counted apart from @ref revet_census_refusals because the two reach DISJOINT cells: the bound catches large-on-arrival (G16 at 670.3 m, G22 at 347.6 m), the census catches slow-ramp-small-on-arrival (G13 at 81.5 m, which no absolute bound can separate from the 61.9 m healthy tail).
    long long revet_census_reads = 0;     ///< consultations ATTEMPTED — vet-window channel-epochs on the established path with the census trigger armed.  The denominator without which @ref revet_census_absent cannot be read: `absent == reads` means the walk is off, `absent == 0` means every consultation had fresh evidence to weigh.
    long long revet_census_refusals = 0;  ///< …that REFUSED, and that the |z| bound alone would NOT have refused this epoch — the whole value of the census wire, isolated so it can be zero and be seen to be zero.
    long long revet_census_absent = 0;    ///< …that found no fresh flag set (walk off, or its stamp was not the previous loop epoch).  Invariant #4: a consultation that never happened is not a clean bill of health.
    long long revet_hold_ep = 0;      ///< channel-epochs withheld by the STRICT posture (network.adm_revet_hold) on a clean re-entry.
    long long revet_expired = 0;      ///< vet windows that ran to completion — the channel served its full window and returned to ordinary established-arc screening.  A window RE-ARMED by a refusal counts here only once it later runs out.

    // --- INC-2 onset / heal -------------------------------------------------
    long long onsets = 0;             ///< ESTABLISHED channels crossing the bound (withheld + refloated).
    long long heals = 0;              ///< channels readmitted after adm_heal_ep consecutive in-bound epochs.
    long long rejects_held = 0;       ///< channel-epochs held while ALREADY rejected (the duration readout; no event, by design).
    long long heal_progress = 0;      ///< channel-epochs that advanced a heal counter without completing it.

    // --- the station guard --------------------------------------------------
    long long station_events = 0;     ///< (station, epoch) blocks where > adm_max_frac of screened channels failed ⇒ the MEDIAN is untrusted.
    long long station_held_channels = 0;  ///< channel-epochs held by a station event (no individual verdict was claimed for any of them).

    // --- the response, at the observable level ------------------------------
    long long withheld_channels = 0;  ///< channel-epochs whose observables were removed from the model set (all causes).
    long long withheld_obs = 0;       ///< individual observable keys erased (CODE + PHASE of the withheld band) — the ROW-level proof the response reached the data.
    long long withheld_pb_pruned = 0; ///< satellite phase-bias states pruned alongside a withhold so the election cannot seat a pin with no supporting row.
    long long withheld_cb_pruned = 0; ///< satellite CODE-bias (OSB) states pruned so the >= 2-station registration gate is not phantom-supported by a station whose row this seam removed.
    long long withheld_clk_pruned = 0;///< satellite CLOCK states pruned at a station left with NO observable at all for that satellite (the same reason, for the clock column's union).
    long long refloat_ambs = 0;       ///< ambiguity keys INSERTED into the Card-4 slip funnel (gated on carry_invariant).
    long long refloat_reseeds = 0;    ///< ambiguity keys marked pending-reseed (the phase-leveling feed; gated on carry_warm).

    // --- INC-3 census -------------------------------------------------------
    long long events = 0;             ///< ADMISSION-tier events emitted.
    long long events_held = 0;        ///< …suppressed by the per-cell holdoff.
    double    max_z = 0.0;            ///< run maximum |z| (m).
    double    max_z_tow = -1.0;       ///< its epoch.
    std::string max_z_id;             ///< its "STATION|SAT|band".
    long long over_bound = 0;         ///< channel-epochs with |z| over the bound, WHETHER OR NOT the response was armed — the measure-only census's headline.
};

/**
 * @brief Screen this epoch's CODE observables against the products, withhold the
 *        channels that fail, heal the ones that recover, and census the rest.
 *        See the file banner for the statistic and the ordering contract.
 *
 * @param ctx                [in/out] admission records, counters, knobs, the
 *                           per-epoch census scalars the epoch-status writer picks up.
 * @param config_            read for nothing but the doxygen contract (every mode
 *                           predicate is already resolved into `ctx.knobs`) — kept
 *                           in the signature for seam-contract uniformity.
 * @param log                producer logger (debug / warn).
 * @param traces             ADMISSION-tier `trace_events` rows.
 * @param t                  this epoch's time (event stamps).
 * @param epoch_idx          the monotonic PROCESSED-epoch index.  Deliberately the
 *                           SAME counter Card G and the flicker classifier use, so
 *                           the three instruments' per-epoch totals are directly
 *                           comparable.
 * @param stations           [in/out] non-const: a withheld band's satellite
 *                           phase-bias state is pruned exactly as the coverage gate
 *                           prunes a thin key's.
 * @param present            this epoch's presence mask.
 * @param modeled            [in/out] per-station observations.  Read for the
 *                           statistic; on a rejection the failing band's entries are
 *                           ERASED from `observed` (never from `raw_observed`, which
 *                           the slip detectors own and which must keep reporting the
 *                           true tracking state).
 * @param carry_slipped_ambs [in/out] the epoch's slip funnel — written by the onset,
 *                           heal and station-guard responses.
 * @param carry_slipped_raw  [in/out] the RAW-detector provenance subset (Iono-2 v3k).
 *                           A withheld band is a RAW observable event — the channel
 *                           physically leaves the model set — so it belongs here too,
 *                           or the deviation carry would D5-KEEP a level across the
 *                           hole this seam just punched.
 * @param carry_invariant    the Card-4 funnel is live (gates the funnel insertion).
 * @param carry_warm         the phase-leveling carry is warm (gates pending-reseed).
 */
void networkMeasurementAdmission(NetworkRunContext& ctx,
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
 * @brief The RINEX-3 band digit of an observable key ("C1C" -> '1'), or 0 when the
 *        key is not a plain 3-character RINEX-3 code.
 *
 * Exposed for the unit suite (and used by the seam itself) so "which band does this
 * observable belong to" has exactly ONE definition.  Combination keys produced by
 * the iono-free path (e.g. "C1C_C2W") are deliberately rejected: this card screens
 * the uncombined producer's raw channels, and an IF combination has no single band
 * to withhold.
 */
char measurementAdmissionBand(const std::string& key);

/**
 * @brief The admission statistic's raw per-observable term
 *        @f$d = C^\text{obs} - m@f$, or @c std::nullopt when either side is
 *        missing or non-finite.
 *
 * Exposed so the unit suite pins the statistic itself rather than only its
 * consequences, and so the "state-free" property has one testable definition.
 */
bool measurementAdmissionD(const PppObservation& obs, const std::string& code_key,
                           double& d_out);

} // namespace GnssProcessor
