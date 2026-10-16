/**
 * @file NetworkMeasurementAdmission.hpp
 * @brief Product-referenced measurement admission: the screen for corrupt channels.
 *        Card: docs/SPEC_measurement_admission.md.
 *
 * ## The fault class
 *
 * The screen targets a channel whose code and phase carry the same large offset
 * against the product-predicted range.  Reference case (DOY-129, BSHM00ISR): G07
 * C1C+L1C at +198.2 km and G16 at +99.9 km against the SP3+CLK prediction.  From
 * the prefit-census card's §17 gate 6: the same G07 reads +8.0 m at DJIG, so the
 * products are sound; eleven other BSHM satellites sit in -12.5…+19.1 m, so the
 * station clock and the model are sound; G21@BSHM, also C1C-only, reads -10.1 m,
 * so single-band tracking alone is not the cause.  The receiver failure mode
 * cannot be determined from the RINEX, so the screen works from internal evidence.
 *
 * Because the offset is common to the code and the phase of the channel, every
 * differential check passes it: TD smoothness, phase-minus-code, GF and MW all
 * cancel it by construction.  Its path into the solution: the state initializer
 * seeds the corrupt value; on a single-band channel the local redundancy is r ≈ 0,
 * so the error moves into the free slant state where no residual test reaches it
 * (single-band tracking amplifies the error but does not cause it); the parked
 * value is then released when a topology valve opens (the 25° tie admission),
 * which discharges it into the ionosphere and starts a long withhold era.
 *
 * Layers of defense, in order of engagement: this seam (layer 0, at the origin) and
 * the prefit fuse (a bound on the symptom, `network.prefit_fuse_sigma`).  The card's
 * other two layers, the INC-3b admission validation (the valve) and the SH health
 * gate (the backstop), were retired with their keys.
 *
 * ## The screen statistic: absolute, coarse, clock-free and state-free
 *
 * For each (station, satellite, band) code observable, at every epoch the products
 * cover the line of sight:
 * @f[
 *   d_i = C^\text{obs}_i - m_i, \qquad
 *   z_i = d_i - \operatorname{median}_{s \in \text{station}}
 *               \bigl(\operatorname{median}_{i \in s} d_i\bigr)
 * @f]
 * where @f$m_i@f$ is @c PppObservation::modeled.  That value is state-free: it is
 * @c PppModel::computeBaseModeledValue (`src/core/model/PppModel.cpp`) plus the
 * pole-tide and higher-order-iono terms added at its call site,
 *
 *     range - c*sat_clock - c*relativistic + ZHD*mf_d + antenna + bias
 *           + shapiro + otl + apl + solid_tide   (+ pole tide + higher-order iono)
 *
 * with no receiver clock, no wet-troposphere state, no first-order slant iono and
 * no ambiguity in it.  @f$d_i@f$ is therefore the spec §2 statistic
 * @f$C^\text{obs} - \rho_\text{pred}(\text{SP3}) - c\,\delta t^s(\text{CLK})@f$
 * with the small deterministic corrections also removed.  The healthy spread is
 * tighter, at no extra cost, with no product re-query and no new dependency in
 * this translation unit.  The deviation from the spec is recorded in the card's §9.
 *
 * Being state-free is what lets this screen act at a cell's birth, where the
 * prefit census cannot: a prefit innovation subtracts the filter state, and a
 * correctly self-seeded birth has innovation ≈ 0 whatever the observable says.
 * @f$d_i@f$ does not depend on whether the cell is new.
 *
 * Design choices, each backed by a field result recorded in the card:
 *
 *  - Raw by intent.  No wet-troposphere, iono or receiver-bias modelling.  Those
 *    terms lie inside the healthy spread the threshold must tolerate (at BSHM,
 *    -12.5…+19.1 m across eleven satellites, storm conditions included).  This is
 *    a structural fault detector for the @f$10^5@f$ m regime, not a residual test;
 *    Huber weighting and RAIM cover the 3-10σ regime.
 *  - The station median stands in for the receiver clock at this precision, so
 *    ms-scale clock offsets and steps cancel exactly (URUM00CHN and INEG00MEX take
 *    clean 1 ms steps in the FT63 leg window; the prefit census's negative gate
 *    validated this centring).  A screen on raw |d| would fire on those steps and
 *    find nothing else.
 *  - The centre is a median over satellites, each satellite first reduced to the
 *    median of its own code terms.  A flat median over observables weights a
 *    satellite by the number of bands it is tracked on, so one satellite on four
 *    bands out-votes three satellites on one band and can set the centre.  The
 *    screen then inverts: the corrupt satellite reads @f$z \approx 0@f$ and is
 *    admitted, and its healthy peers read @f$z \approx |d|@f$ and are rejected.
 *    The station guard cannot catch that case: it fires only when the failing (here
 *    the healthy) bands are the majority, and the capture needs the corrupt bands
 *    to be the majority.  Reducing per satellite first also makes @c adm_min_sats
 *    bound the population it names: the median's sample size is the satellite
 *    count, as §2 defines it.
 *  - Code only.  A constant offset on phase alone is indistinguishable from an
 *    ambiguity, so it is gauge, not corruption.  Screening the code flags the
 *    channel; the response then removes both observables of that band, because a
 *    corrupt channel corrupts everything it carries.
 *  - Band, not signal.  The record and the response are keyed by (satellite, RINEX
 *    band digit), so C1C, C1W and L1C are one channel.  A receiver fault that puts
 *    one band 198 km out is not expected to respect attribute letters.
 *
 * ## The increments (one arming knob for the response)
 *
 *  - INC-1, admission at arc birth and at warm-resume restore.  A channel whose |z|
 *    exceeds @c network.adm_bound_m at the epoch its arc would be born is not
 *    seeded: the band is withheld from the model set for that epoch.  On a warm
 *    resume the same applies at each station's first screened epoch, because a
 *    restored corrupt arc is not a birth and the census card's INC-3 cannot see
 *    it.  At a birth no state exists yet, so nothing estimable is lost; this is
 *    admission (the coverage-gate and anchor-hygiene hold-out precedent), not a
 *    measurement drop.  Birth versus restore is only a label: the refloat runs on
 *    every rejection.  The classification comes from a record map that is not
 *    serialized (and whose station may have been absent, thin or satellite-pruned
 *    on the resume epoch), so it can be wrong, while re-tapping an arc that has no
 *    carried state is a no-op.  Correctness therefore does not depend on the
 *    label (standing invariant #1).
 *  - INC-2, mid-arc onset and heal.  An established channel that crosses the bound
 *    has its band withheld for the epoch and its arc flagged for refloat (reset
 *    over repair: the channel's history is untrusted and nothing bounds the error,
 *    so there is no value to correct toward, only state to discard).  A rejected
 *    band is re-tested every epoch.  After @c network.adm_heal_ep consecutive
 *    in-bound epochs it is readmitted through the normal funnel with a fresh seed;
 *    a heal never restores the old states.
 *  - INC-3, the absolute-residual census (@c network.adm_trace; armable without
 *    the response, as a measure-only arm).  Per epoch, the median over screened
 *    stations of each station's worst |z| and the network maximum |z| go into
 *    `network_epoch_status.csv`, and every over-bound cell that no response event
 *    named this epoch gets an ADMISSION-tier `adm_census` event (rate-limited per
 *    cell).
 *  - INC-4, the arc re-entry vet (@c network.adm_revet).  An admission record
 *    outlives its arc (it is kept for @ref kAdmArcForgetEp epochs), so a channel
 *    that leaves the model set and returns later still carries `admitted = true`
 *    from before the gap and would take the established path, inheriting a verdict
 *    about a channel that is not necessarily the one that came back.  With INC-4 a
 *    return after a gap of at least @c network.adm_revet_gap_ep epochs is
 *    re-screened as a birth, and the channel is then vetted for
 *    @c network.adm_revet_ep screened epochs.  In the vet window a tighter bound
 *    applies (@c network.adm_revet_bound_scale times the admission bound), and a
 *    PREFIT-census `obs_code` exceedance on the line of sight in the previous
 *    epoch refuses the channel as the bound does.  Refusing at admission costs
 *    nothing, while amputating an established arc has a cost (standing
 *    invariant #7 at both edges).  Field basis (card §10): on the FT63 24 h day,
 *    16 of the 17 mid-arc onsets followed an `arc_resume` within five minutes.
 *    Twelve fired at the resume epoch itself (gaps of 3-2025 epochs), so only
 *    their label was wrong; the other four were readmitted and crossed the bound
 *    2-8 epochs later, the epochs of the day's two largest iono excursions.  The
 *    census named all four 1-4 epochs before the bound reached them; the vet
 *    connects that detection to the response.
 *
 * ## The station guard: no rejection from a corrupted centre
 *
 * If, in one epoch, more than @c network.adm_max_frac of the satellites that formed
 * a station's median have a failing band, the median itself is untrusted (a badly
 * wrong a-priori position, a product outage, a receiver-wide fault) and no
 * per-channel verdict derived from it means anything.  The response follows the
 * IF-TD reference gate's incoherent-station rule (`IfPhaseSlipDetector.cpp`,
 * FireNoRebase: fire without rebasing, because the median is a mixture and not a
 * common step): take the conservative action at station scope and make no
 * per-channel claim.  The seam holds every screened band of the station for the
 * epoch, refloats the arcs behind the hole (standing invariant #1: a hole in a
 * stiff time-linked carry needs a wired response), emits one
 * @c adm_station_event, and leaves every per-channel admission record untouched,
 * so the next epoch re-tests from evidence rather than from a verdict this one
 * could not support.
 *
 * ## Ordering contract
 *
 * `networkMeasurementAdmission` runs
 *   * after the PASS-A slip scans and their union into the funnel, because it
 *     inserts into `carry_slipped_ambs` / `carry_slipped_raw` and every consumer
 *     (`networkAnchorHygiene`, the T5v2 deviation erase in PASS B, the C3c
 *     DD-prior build, the Card-G gap classifier) must see those insertions;
 *   * after `networkAssemblyPrefilter_`, so a satellite the two-station
 *     observability gate has just erased is not screened, evented, or counted as
 *     admitted;
 *   * after `networkSignalIntegrity` (see below); and
 *   * before the phase-datum census and election, the shared-column registration
 *     and PASS B.  It is layer 0 of the card's §1 defense in depth (this seam, the
 *     prefit fuse), and every later layer,
 *     the election, the prefit census and RAIM observe the admitted set rather
 *     than the raw one.
 *
 * The window is the point immediately after `networkSignalIntegrity`.  Running
 * before the flicker classifier would also fall inside the window and would work,
 * but it would distort that card's instrument: `modeled` is rebuilt from
 * `computeModels` every epoch, so a withhold applied first would make the
 * classifier read a band loss this epoch, a `band_dropped_perm` after its flicker
 * window and a FLICKER-REGAIN on heal, producing the events the flicker card's
 * INC-0 census measures.  Running after it costs nothing: this seam's statistic
 * reads only the code keys of `observed` and `modeled`, and the classifier writes
 * neither (its withhold erases phase keys only).  It also keeps the flicker
 * counters comparable between an admission-armed leg and its control.  The
 * refloats of the two cards compose: both land in the same funnel, which is a set.
 *
 * ## Scope
 *
 * Off by default.  With it off the seam returns on its first line, allocates no
 * map and touches no observable, so the output is byte-identical.  Not
 * fingerprinted: it is a forward-only admission policy (RAIM-class), so a stored
 * snapshot's state means the same thing either way and an off/on pair on one
 * shared snapshot is a legal A/B.  The exemption is recorded in place at
 * `networkSnapshotFingerprint`.
 *
 * The seam owns no state: the per-channel admission records live in
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
#include "network/driver/NetworkRunTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;
struct GnssTime;

/// Drop an admission record not screened for this many processed epochs.  This is a
/// memory bound only (the key set is stations x satellites x bands, about 36 k at 305
/// stations, so it does not bind in practice); 2880 epochs is one day at 30 s.  A
/// dropped record makes the channel's eventual return read as a birth rather than a
/// heal in progress, which is the conservative direction (it re-screens from
/// scratch), and the drop is counted, so the label change is visible.
inline constexpr long long kAdmArcForgetEp = 2880;

/// Per-(event, station, satellite, band) event rate limit, in processed epochs.
/// A constant rather than a knob: the INC-1/INC-2 events are transitions (a birth,
/// an onset, a heal) and limit themselves, so the only two streams that can repeat
/// are the INC-3 `adm_census` events and a chronically bad station's guard event.
/// The value matches the prefit census's default holdoff, so the two event files
/// read at the same cadence.
inline constexpr long long kAdmEventHoldoffEp = 20;

/**
 * @brief Cross-epoch admission record for one (station, satellite, band) channel.
 *
 * Stored in @c NetworkCarryState::adm_arc, keyed by
 * (app station index, "SAT|<band digit>").
 *
 * Keyed per band rather than per signal because the response covers both
 * observables of a band, and a receiver fault that displaces a channel by 198 km
 * is not expected to respect RINEX attribute letters: C1C and C1W at one satellite
 * are one physical channel group, and the evidence cannot support admitting one
 * while rejecting the other.
 *
 * Not serialized into the state snapshot.  On a resumed leg every channel is
 * therefore unknown at its station's first screened epoch, which is the case
 * INC-1's resume clause covers: the arc's state was restored (possibly carrying
 * the corrupt seed) while this record was not, so a failure on that epoch is a
 * @c adm_reject_resume, not a birth.
 */
struct MeasurementAdmissionArc {
    /// True while the channel is in the model set (it passed its last verdict, or it
    /// has healed).  Set false by a birth, re-entry or onset rejection and by the
    /// strict re-entry hold (@c network.adm_revet_hold), and true again when
    /// @c clean_run reaches @c network.adm_heal_ep.
    bool      admitted  = false;
    /// The heal counter: consecutive in-bound epochs accumulated while rejected.
    /// Reset to 0 by any failing epoch.  Not advanced on a station-guard epoch,
    /// because a verdict from an untrusted centre is not evidence in either direction.
    int       clean_run = 0;
    /// Processed-epoch index of the last epoch this channel was screened
    /// (-1 = never).  Drives @ref kAdmArcForgetEp and, under
    /// @c network.adm_revet, the re-entry test: the gap since the previous
    /// screened epoch is the only evidence this seam has that the channel present
    /// now may not be the one its stored verdict describes.
    long long last_ep   = -1;
    /// Screened epochs left in the INC-4 re-entry vet window (0 = not vetting).  Set
    /// to @c network.adm_revet_ep on a re-entry, on a heal, and on any refusal of an
    /// admitted channel on the established path while @c network.adm_revet is on;
    /// decremented once per screened epoch on which the channel is admitted.
    ///
    /// While positive, the refusal test is widened: the tighter re-entry bound
    /// (@c network.adm_revet_bound_scale) applies, and a PREFIT-census `obs_code`
    /// exceedance on the line of sight refuses as the |z| bound does.  The window
    /// does not by itself withhold anything: a clean re-entry is admitted on its
    /// return epoch as before (unless @c network.adm_revet_hold is on), so the
    /// increment leaves healthy re-entries unchanged on that epoch.
    int       vet_left  = 0;
};

/**
 * @brief Run tallies for the card (standing invariant #4: run-end counters are the
 *        on-path telemetry).  Embedded in @c NetworkServiceCounters as `adm`.
 *
 * @c epochs, @c station_epochs and @c channels are the denominators.  An armed run
 * that reports zero screened channels never ran the screen, which is not the same
 * as a run in which the screen ran and every channel was healthy; log output alone
 * cannot distinguish the two.
 *
 * Rejections are counted per entry point (birth, resume restore, mid-arc onset)
 * rather than fused, because the three answer different questions: a birth
 * rejection says the channel was never admissible, a resume rejection says a
 * corrupt arc was carried across a snapshot boundary, and an onset says a
 * previously good channel went bad.  Only an onset needs a refloat (at a birth the
 * refloat is a no-op), and only onsets can falsify the card's finding that the
 * offset is present from the arc's first epoch.  Re-entries (INC-4) have their own
 * counters.
 */
struct NetworkMeasurementAdmissionTallies {
    // --- denominators -------------------------------------------------------
    long long epochs = 0;             ///< seam passes (0 while armed means the wiring never executed).
    long long station_epochs = 0;     ///< (station, epoch) blocks with a usable median.
    long long los_epochs = 0;         ///< (station, satellite, epoch) lines of sight visited.
    long long channels = 0;           ///< (station, satellite, band, epoch) code channels screened.
    long long skips_thin = 0;         ///< station-epochs skipped: fewer than adm_min_sats satellites had a finite d, so the median is not a usable centre.
    long long skips_nonfinite = 0;    ///< code observables skipped: no modeled value, or a non-finite difference (kept out of every median).
    long long arcs_forgotten = 0;     ///< admission records dropped after a very long absence (memory bound, not a physical event).
    long long resume_passes = 0;      ///< 1 on a warm-resume run (counted on its first seam pass), otherwise 0.

    // --- INC-1 admission ----------------------------------------------------
    long long births = 0;             ///< channel-epochs with no prior record: the admission-opportunity count (admits_birth + reject_birth + reject_resume).
    long long admits_birth = 0;       ///< …admitted (the large majority; the denominator the two rejection counts are read against).
    long long reject_birth = 0;       ///< …rejected at the epoch the arc would have been born.
    long long reject_resume = 0;      ///< …rejected at a station's first screened epoch of a warm resume (a restored corrupt arc, not a birth).

    // --- INC-4 the re-entry vet ---------------------------------------------
    // Without INC-4, a channel that returns after a gap takes the established path
    // and a failure there is counted in @c onsets; on the FT63 24 h day, 16 of the 17
    // onsets were such re-entries.  With INC-4 armed they move to @c reentries, and
    // @c onsets holds channels that went bad while present plus refusals inside a
    // vet window.  That shift is the increment's main field readout, so the two are
    // counted separately.
    long long reentries = 0;          ///< channel-epochs re-screened as a birth because the channel had not been screened for at least adm_revet_gap_ep epochs (admits_reentry + reject_reentry + revet_hold_ep).
    long long admits_reentry = 0;     ///< …admitted on the return epoch (the large majority; no withhold and no refloat on that epoch).
    long long reject_reentry = 0;     ///< …refused on the return epoch (on the FT63 day, twelve re-entries that the established path counts as onsets without INC-4).
    long long revet_window_ep = 0;    ///< channel-epochs of admitted operation inside an open vet window, i.e. epochs that spent vet budget.  A withheld epoch is not counted and does not tick the window down: it is not evidence of recovery, and a rejected channel that served out its window while withheld would return unvetted.  With the census trigger armed, this exceeds @ref revet_census_reads by one for each admitted epoch that opened a window (an admitted re-entry or a heal: those paths consult no census, and on the return epoch the census defers a re-entering arc's fresh ambiguity as a birth anyway), and falls short of it by one for each established-path refusal inside a window (a census read on an epoch that ends withheld).
    long long revet_bound_refusals = 0;   ///< refusals made by the tighter re-entry bound (@c network.adm_revet_bound_scale) that @c adm_bound_m would not have made, at a return epoch or inside a vet window; when the bound and the census both fire, the refusal counts here.  Counted apart from @ref revet_census_refusals because the two reach disjoint cells: the bound catches channels large on arrival (G16 at 670.3 m, G22 at 347.6 m), the census catches slow ramps small on arrival (G13 at 81.5 m, which no absolute bound separates from the 61.9 m healthy tail).
    long long revet_census_reads = 0;     ///< consultations attempted: vet-window channel-epochs on the established path with the census trigger armed.  The denominator for @ref revet_census_absent: `absent == reads` means the walk is off, `absent == 0` means every consultation had fresh evidence to weigh.
    long long revet_census_refusals = 0;  ///< …that refused where neither @c adm_bound_m nor the tighter vet bound would have refused this epoch: the value the census wire adds, isolated so that a zero is visible.
    long long revet_census_absent = 0;    ///< …that found no fresh flag set (walk off, or its stamp was not the previous loop epoch).  Invariant #4: a consultation that did not happen is not a clean result.
    long long revet_hold_ep = 0;      ///< channel-epochs withheld by the strict posture (network.adm_revet_hold) on a clean re-entry.
    long long revet_expired = 0;      ///< vet windows that ran to completion: the channel served its full window and returned to ordinary established-arc screening.  A window re-armed by a refusal counts here only once it later runs out.

    // --- INC-2 onset / heal -------------------------------------------------
    long long onsets = 0;             ///< established, admitted channels refused (withheld and refloated): |z| over the bound, or, inside a vet window, the vet bound or the census trigger.
    long long heals = 0;              ///< channels readmitted after adm_heal_ep consecutive in-bound epochs.
    long long rejects_held = 0;       ///< channel-epochs held while already rejected (the duration readout; no transition event).
    long long heal_progress = 0;      ///< channel-epochs that advanced a heal counter without completing it.

    // --- the station guard --------------------------------------------------
    long long station_events = 0;     ///< (station, epoch) blocks where more than adm_max_frac of the satellites that formed the median had a failing band, so the median is untrusted.
    long long station_held_channels = 0;  ///< channel-epochs held by a station event (no individual verdict was claimed for any of them).

    // --- the response, at the observable level ------------------------------
    long long withheld_channels = 0;  ///< channel-epochs whose observables were removed from the model set (all causes).
    long long withheld_obs = 0;       ///< individual observable keys erased (code and phase of the withheld band): row-level evidence that the response reached the data.
    long long withheld_pb_pruned = 0; ///< satellite phase-bias states pruned alongside a withhold, so the election cannot seat a pin with no supporting row.
    long long withheld_cb_pruned = 0; ///< satellite code-bias (OSB) states pruned, so the >= 2-station registration gate is not satisfied by a station whose row this seam removed.
    long long withheld_clk_pruned = 0;///< satellite clock states pruned at a station left with no observable at all for that satellite (the same reason, for the clock column's union).
    long long refloat_ambs = 0;       ///< ambiguity keys newly inserted into the Card-4 slip funnel (only with carry_invariant).
    long long refloat_reseeds = 0;    ///< ambiguity keys marked pending-reseed (the phase-leveling feed; only with carry_warm).

    // --- INC-3 census -------------------------------------------------------
    long long events = 0;             ///< ADMISSION-tier events emitted.
    long long events_held = 0;        ///< …suppressed by the per-cell holdoff.
    double    max_z = 0.0;            ///< run maximum |z| (m).
    double    max_z_tow = -1.0;       ///< TOW of that epoch (s; -1 = none yet).
    std::string max_z_id;             ///< its "STATION|SAT|band".
    long long over_bound = 0;         ///< channel-epochs with |z| over adm_bound_m, whether or not the response is armed: the measure-only census's headline.
};

struct NetworkRun;              // network/driver/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)
struct EpochEvents;             // network/driver/NetworkEpoch.hpp
struct EpochPassA;              // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)
struct NetworkSlipFunnel;       // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief Screen this epoch's code observables against the products, withhold the
 *        channels that fail, heal the ones that recover, and census the rest.
 *        The file banner describes the statistic and the ordering contract.
 *
 * Epoch form (card NOM3 step EP1); the explicit form and its forwarding adapter
 * retired in card NOM3 step EP2 (the unit tests call this form through
 * tests/support/NetworkTestRun.hpp).
 *
 * @param run The run (network/driver/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces), run.setup.stations (stations). Read: run.config (config_),
 *            run.log (log), run.engines.slip_qc.epoch_idx (epoch_idx),
 *            run.ctx.knobs.carry_invariant (carry_invariant).
 * @param ep The epoch (network/driver/NetworkEpoch.hpp), read only. Bindings: ep.se.time (t),
 *           ep.pass_a.present (present), ep.clock.carry_warm (carry_warm).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: [in/out] admission records, counters, knobs, and the per-epoch census scalars the epoch-status writer reads.
 *   - @p config_ = run.config: unused (every mode predicate is already resolved into `ctx.knobs`); kept in the signature for seam-contract uniformity.
 *   - @p log = run.log: producer logger (debug).
 *   - @p traces = run.traces: ADMISSION-tier `trace_events` rows.
 *   - @p t = ep.se.time: this epoch's time (event stamps).
 *   - @p epoch_idx = run.engines.slip_qc.epoch_idx: the monotonic processed-epoch index.  The same counter Card G and the flicker classifier use, so the three instruments' per-epoch totals are directly comparable.
 *   - @p stations = run.setup.stations: [in/out] non-const: a withheld band's satellite phase-bias and code-bias states, and the clock state of a satellite left with no observable, are pruned as the coverage gate prunes a thin key's.
 *   - @p present = ep.pass_a.present: this epoch's presence mask.
 *   - @p modeled = pass_a.modeled: [in/out] per-station observations.  Read for the statistic; on a rejection the failing band's entries are erased from `observed`, never from `raw_observed`, which the slip detectors own and which must keep reporting the true tracking state.
 *   - @p carry_slipped_ambs = funnel.carry_slipped_ambs: [in/out] the epoch's slip funnel, written by every withhold (rejection, hold, station guard) and by a heal.
 *   - @p carry_slipped_raw = funnel.carry_slipped_raw: [in/out] the raw-detector provenance subset (Iono-2 v3k). A withheld band is a raw observable event (the channel leaves the model set), so it is recorded here too; otherwise the deviation carry would D5-keep a level across the hole this seam made.
 *   - @p carry_invariant = run.ctx.knobs.carry_invariant: the Card-4 funnel is live (gates the funnel insertion).
 *   - @p carry_warm = ep.clock.carry_warm: the phase-leveling carry is warm (gates pending-reseed).
 * @param events The epoch's events; written: admission.
 */
void networkMeasurementAdmission(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                                 NetworkSlipFunnel& funnel, EpochEvents& events);

/**
 * @brief The RINEX-3 band digit of an observable key ("C1C" -> '1'), or 0 when the
 *        key is not a plain 3-character RINEX-3 code with a band digit 1-9.
 *
 * Exposed for the unit suite (and used by the seam itself) so the band of an
 * observable has one definition.  Combination keys produced by the iono-free path
 * (e.g. "C1C_C2W") are rejected on purpose: this card screens the uncombined
 * producer's raw channels, and an IF combination has no single band to withhold.
 */
char measurementAdmissionBand(const std::string& key);

/**
 * @brief The admission statistic's raw per-observable term
 *        @f$d = C^\text{obs} - m@f$.  Returns false, leaving @p d_out unchanged,
 *        when either side is missing or the difference is non-finite.
 *
 * Exposed so the unit suite pins the statistic itself rather than only its
 * consequences, and so the state-free property has one testable definition.
 */
bool measurementAdmissionD(const PppObservation& obs, const std::string& code_key,
                           double& d_out);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the product-referenced measurement admission.
 *
 * The product-referenced measurement admission screen: denominators, the census
 * separation, the response and the warnings.
 *
 * Gate: returns 0 at once unless `ctx.knobs.adm_on`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkMeasurementAdmissionRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
