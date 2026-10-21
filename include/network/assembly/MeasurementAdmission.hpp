/**
 * @file MeasurementAdmission.hpp
 * @brief Product-referenced measurement admission: an absolute screen for corrupt
 *        code and phase channels.
 *
 * ## The fault class
 *
 * Some receivers emit a channel whose code and phase carry the same large offset
 * against the range predicted from the precise products (SP3 orbits and CLK clocks).
 * Reference case (day of year 129, station BSHM00ISR): G07 C1C+L1C at +198.2 km and
 * G16 at +99.9 km. The evidence that the fault sits in the channel and nowhere else:
 *
 *  - G07 reads +8.0 m at DJIG, so the products are sound.
 *  - Eleven other BSHM satellites sit in -12.5 to +19.1 m, so the station clock and
 *    the model are sound.
 *  - G21 at BSHM, also C1C-only, reads -10.1 m, so single-band tracking alone is not
 *    the cause.
 *
 * The RINEX does not reveal the receiver failure mode, so the screen works from
 * internal evidence.
 *
 * Because the offset is common to the code and the phase of the channel, every
 * differential check passes it: time-difference (TD) smoothness, phase minus code,
 * geometry-free (GF) and Melbourne-Wubbena (MW) combinations all cancel it by
 * construction. Its path into the solution:
 *
 *  1. The birth seeds of the channel's states take the corrupt value.
 *  2. On a single-band channel the local redundancy is r ≈ 0, so the error moves
 *     into the free slant ionosphere state, where no residual test reaches it.
 *     (Single-band tracking amplifies the error but does not cause it.)
 *  3. The parked value is released later, when a gate that ties the channel's slant
 *     ionosphere to a wider constraint opens (for example, admission of the
 *     elevation-limited ionosphere tie rows). The stiff parked error is then
 *     discharged into the ionosphere and starts a long run of withheld epochs.
 *
 * This seam is the first layer of defense and acts at the origin. The second layer
 * is the prefit fuse (`network.prefit_fuse_sigma`), a bound on the symptom.
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
 * where @f$m_i@f$ is @c PppObservation::modeled. That value is state-free: it is
 * @c PppModel::computeBaseModeledValue (`src/core/model/PppModel.cpp`) plus the
 * pole-tide and higher-order-iono terms added at its call site,
 *
 *     range - c*sat_clock - c*relativistic + ZHD*mf_d + antenna + bias
 *           + shapiro + otl + apl + solid_tide   (+ pole tide + higher-order iono)
 *
 * where ZHD is the zenith hydrostatic troposphere delay, mf_d its mapping function,
 * and otl and apl are ocean-tide and atmospheric-pressure loading. There is no
 * receiver clock, no wet-troposphere state, no first-order slant iono and
 * no ambiguity in it. Hence @f$d_i@f$ is
 * @f$C^\text{obs} - \rho_\text{pred}(\text{SP3}) - c\,\delta t^s(\text{CLK})@f$
 * with the small deterministic corrections also removed. That tightens the healthy
 * spread at no extra cost: no product is queried again and this file takes no new
 * dependency.
 *
 * Being state-free is what lets this screen act at a cell's birth, where the prefit
 * census (`network/qc/PrefitCensus.hpp`) cannot: a prefit innovation subtracts the
 * filter state, and a correctly self-seeded birth has innovation ≈ 0 whatever the
 * observable says. @f$d_i@f$ does not depend on whether the cell is new.
 *
 * Design choices, each backed by a field result:
 *
 *  - Raw by intent. No wet-troposphere, iono or receiver-bias modelling. Those terms
 *    lie inside the healthy spread the threshold must tolerate (at BSHM, -12.5 to
 *    +19.1 m across eleven satellites, storm conditions included). This is a
 *    structural fault detector for the @f$10^5@f$ m regime, not a residual test.
 *    Huber weighting and RAIM (receiver autonomous integrity monitoring) cover the
 *    3 to 10 sigma regime.
 *  - The station median stands in for the receiver clock at this precision, so
 *    millisecond clock offsets and steps cancel exactly. (Stations URUM00CHN and
 *    INEG00MEX take clean 1 ms steps on the 63-station, 24 h field day used to
 *    develop the screen, and the prefit census confirmed that this centring leaves
 *    them clean.) A screen on raw |d| would fire on every such step.
 *  - The centre is a median over satellites, each satellite first reduced to the
 *    median of its own code terms. A flat median over observables weights a
 *    satellite by the number of bands it is tracked on, so one satellite on four
 *    bands out-votes three satellites on one band and can set the centre. The
 *    screen then inverts: the corrupt satellite reads @f$z \approx 0@f$ and is
 *    admitted, and its healthy peers read @f$z \approx |d|@f$ and are rejected.
 *    The station guard cannot catch this case. At the default @c adm_max_frac of 0.5
 *    it fires only when failing satellites outnumber passing ones, while the capture
 *    needs the corrupt satellites to outnumber the healthy ones, and in the inverted
 *    screen the failing satellites are the healthy ones.
 *    Reducing per satellite first also makes @c adm_min_sats bound the population it
 *    names: the median's sample size is the satellite count.
 *  - Code only. A constant offset on phase alone is indistinguishable from an
 *    ambiguity, so it is gauge, not corruption. Screening the code flags the
 *    channel; the response then removes both observables of that band, because a
 *    corrupt channel corrupts everything it carries.
 *  - Band, not signal. The record and the response are keyed by (satellite, RINEX
 *    band digit), so C1C, C1W and L1C are one channel. A receiver fault that puts
 *    one band 198 km out is not expected to respect attribute letters.
 *
 * ## The four parts
 *
 * `network.adm_screen` arms the response (parts 1 and 2). Part 4 additionally needs
 * `network.adm_revet` (off by default, ignored unless `adm_screen` is on).
 * `network.adm_trace` arms the census (part 3) and can run alone as a measure-only
 * arm; `adm_screen` implies it.
 *
 * A rejection withholds the band's observables for the epoch and refloats the arcs
 * behind the hole. To refloat an arc is to discard its carried ambiguity and level
 * and re-seed it, by inserting its key into the slip funnel
 * (@c NetworkSlipFunnel) and marking it pending-reseed.
 *
 *  1. Admission at arc birth and at warm-resume restore. A channel whose |z| exceeds
 *     @c network.adm_bound_m at the epoch its arc would be born is not seeded. On a
 *     warm resume the same applies at each station's first screened epoch, because
 *     a restored corrupt arc is not a birth and the prefit census cannot see it. At
 *     a birth no state exists yet, so nothing estimable is lost: this is admission,
 *     not a measurement drop. Birth versus restore is only a label, because the
 *     refloat runs on every rejection. The label comes from a record map that is
 *     not serialized, so it can be wrong (the station may have been absent, thin or
 *     satellite-pruned on the resume epoch), and refloating an arc that has no carried
 *     state is a no-op. Without the unconditional refloat, a restored arc
 *     mislabelled as a birth would keep its stiff carried value across the hole.
 *  2. Mid-arc onset and heal. An established channel that crosses the bound has its
 *     band withheld for the epoch and its arcs refloated. This is reset over repair:
 *     the channel's history is untrusted and nothing bounds the error, so there is
 *     no value to correct toward, only state to discard. A rejected band is
 *     re-tested every epoch. After @c network.adm_heal_ep consecutive in-bound
 *     epochs it is readmitted with a fresh seed; a heal never restores the old
 *     states.
 *  3. The absolute-residual census (@c network.adm_trace). Per epoch, the median
 *     over screened stations of each station's worst |z| and the network maximum
 *     |z| go into `network_epoch_status.csv`, and every over-bound cell that no
 *     response event named this epoch gets an ADMISSION-tier `adm_census` event
 *     (rate-limited per cell).
 *  4. The arc re-entry vet (@c network.adm_revet). An admission record outlives its
 *     arc (it is kept for @ref kAdmArcForgetEp epochs), so a channel that leaves the
 *     model set and returns later still carries `admitted = true` from before the
 *     gap and would take the established path. It would inherit a verdict about a
 *     channel that is not necessarily the one that came back. With the vet, a
 *     return after a gap of at least @c network.adm_revet_gap_ep epochs is
 *     re-screened as a birth, and the channel is then vetted for
 *     @c network.adm_revet_ep screened epochs. In the vet window a tighter bound
 *     applies (@c network.adm_revet_bound_scale times the admission bound), and a
 *     prefit-census `obs_code` exceedance on the line of sight in the previous
 *     epoch refuses the channel as the bound does (@c network.adm_revet_census).
 *     Refusing at admission costs nothing, while amputating an established arc has
 *     a cost.
 *
 *     Field basis: on the 63-station field day, 16 of the 17 mid-arc onsets
 *     followed an `arc_resume` within five minutes. Twelve fired at the resume
 *     epoch itself (gaps of 3 to 2025 epochs), so only their label was wrong. The
 *     other four were readmitted and crossed the bound 2 to 8 epochs later, during
 *     the day's two largest ionosphere excursions. The census named all four 1 to
 *     4 epochs before the bound reached them; the vet connects that detection to
 *     the response.
 *
 * ## The station guard: no rejection from a corrupted centre
 *
 * With `network.adm_screen` armed, if in one epoch more than @c network.adm_max_frac
 * of the satellites that formed a station's median have a failing band, the median
 * itself is untrusted (a badly wrong a-priori position, a product outage, a
 * receiver-wide fault) and no per-channel verdict derived from it means anything.
 *
 * The response follows the incoherent-station rule of the ionosphere-free
 * time-difference (IF-TD) reference gate (`RefGateVerdict::FireNoRebase` in
 * `src/core/slip/IfPhaseSlipDetector.cpp`: fire without rebasing, because the
 * median is a mixture and not a common step). It takes the conservative action at
 * station scope and makes no per-channel claim:
 *
 *  - every screened band of the station is held for the epoch;
 *  - the arcs behind the holes are refloated (a hole in a stiff time-linked carry
 *    needs a wired response, or the carried value warps the solution);
 *  - one @c adm_station_event is emitted;
 *  - every per-channel admission record is left untouched, so the next epoch
 *    re-tests from evidence rather than from a verdict this one could not support.
 *
 * ## Ordering contract
 *
 * `networkMeasurementAdmission` runs
 *   * after the PASS A slip scans (the per-epoch slip detection) and their union
 *     into the funnel, because it inserts into `carry_slipped_ambs` /
 *     `carry_slipped_raw` and every consumer must see those insertions:
 *     `networkAnchorHygiene`, the deviation erase policy in PASS B (the per-station
 *     row build), `networkBuildCarryDdPriors` and the gap classifier;
 *   * after `networkAssemblyPrefilter_`, so a satellite the two-station
 *     observability gate has just erased is not screened, evented, or counted as
 *     admitted;
 *   * after `networkSignalIntegrity` (see below); and
 *   * before the phase-datum census and election, the shared-column registration
 *     and PASS B. Every later stage (the election, the prefit census and RAIM)
 *     therefore observes the admitted set rather than the raw one.
 *
 * Running before `networkSignalIntegrity` would also be valid, but it would distort
 * that instrument. `modeled` is rebuilt from `computeModels` every epoch, so a
 * withhold applied first would make the signal-integrity classifier read a band
 * loss this epoch, a permanent band drop (`band_dropped_perm`) once its flicker
 * window passed, and a flicker regain (@c SignalIntegrityState::FlickerRegain) on
 * heal. Those are the events its census exists to measure.
 *
 * Running after it costs nothing: this seam's statistic reads only the code keys of
 * `observed` and `modeled`, and the classifier writes neither (its withhold erases
 * phase keys only). It also keeps the signal-integrity counters comparable between
 * an admission-armed run and its control. The refloats of the two screens compose:
 * both land in the same funnel, which is a set.
 *
 * ## Scope
 *
 * Off by default. With it off the seam returns on its first line, allocates no map
 * and touches no observable, so the output is byte-identical. Not fingerprinted: it
 * is a forward-only admission policy (RAIM-class), so a stored snapshot's state
 * means the same thing either way and an off/on pair on one shared snapshot is a
 * legal A/B comparison. The exemption is recorded in place at
 * `networkSnapshotFingerprint`.
 *
 * The seam owns no state: the per-channel admission records live in
 * @c net::StationState::adm_arc (`network/state/Entities.hpp`), and every counter
 * in @ref NetworkServiceCounters.
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
struct NetworkRunContext;
struct NetworkRunTraces;
struct GnssTime;

/// Drop an admission record not screened for this many processed epochs.  This is a
/// memory bound only (the key set is stations x satellites x bands, about 36 k at 305
/// stations, so it does not bind in practice); 2880 epochs is one day at 30 s.  A
/// dropped record makes the channel's eventual return read as a birth rather than a
/// heal in progress, which is the conservative direction (it re-screens from
/// scratch).  The drop is counted (@c arcs_forgotten), so the label change is visible.
inline constexpr long long kAdmArcForgetEp = 2880;

/// Per-(event, station, satellite, band) event rate limit, in processed epochs.
/// A constant rather than a knob: birth, onset and heal events are transitions and
/// limit themselves, so the only two streams that can repeat are the `adm_census`
/// events and a chronically bad station's guard event.  The value matches the prefit
/// census's default holdoff, so the two event files read at the same cadence.
inline constexpr long long kAdmEventHoldoffEp = 20;

/**
 * @brief Cross-epoch admission record for one (station, satellite, band) channel.
 *
 * Stored in @c net::StationState::adm_arc (`network/state/Entities.hpp`) under the
 * key "SAT|<band digit>" within the station.
 *
 * Keyed per band rather than per signal because the response covers both
 * observables of a band, and a receiver fault that displaces a channel by 198 km
 * is not expected to respect RINEX attribute letters: C1C and C1W at one satellite
 * are one physical channel group, and the evidence cannot support admitting one
 * while rejecting the other.
 *
 * Not serialized into the state snapshot.  On a resumed leg every channel is
 * therefore unknown at its station's first screened epoch.  That is the case the
 * warm-resume clause of the admission covers: the arc's state was restored
 * (possibly carrying the corrupt seed) while this record was not, so a failure on
 * that epoch is an @c adm_reject_resume, not a birth.
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
    /// Screened epochs left in the re-entry vet window (0 = not vetting).  Set to
    /// @c network.adm_revet_ep on a re-entry, on a heal, and on any refusal of an
    /// admitted channel on the established path while @c network.adm_revet is on;
    /// decremented once per screened epoch on which the channel is admitted.
    ///
    /// While positive, the refusal test is widened: the tighter re-entry bound
    /// (@c network.adm_revet_bound_scale) applies, and a prefit-census `obs_code`
    /// exceedance on the line of sight refuses as the |z| bound does.  The window
    /// does not by itself withhold anything: a clean re-entry is admitted on its
    /// return epoch as before (unless @c network.adm_revet_hold is on), so the
    /// vet leaves healthy re-entries unchanged on that epoch.
    int       vet_left  = 0;
};

/**
 * @brief Run tallies for the admission seam.  Embedded in
 *        @c NetworkServiceCounters as `adm`.
 *
 * Run-end counters are the telemetry that shows a code path ran.  @c epochs,
 * @c station_epochs and @c channels are the denominators.  An armed run that
 * reports zero screened channels never ran the screen, which is not the same as a
 * run in which the screen ran and every channel was healthy; log output alone
 * cannot distinguish the two.
 *
 * Rejections are counted per entry point (birth, resume restore, mid-arc onset)
 * rather than fused, because the three answer different questions: a birth
 * rejection says the channel was never admissible, a resume rejection says a
 * corrupt arc was carried across a snapshot boundary, and an onset says a
 * previously good channel went bad.  Only an onset needs a refloat (at a birth the
 * refloat is a no-op), and only onsets can contradict the field finding that the
 * offset is present from the arc's first epoch.  Re-entries have their own
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

    // --- admission at birth and warm-resume restore -------------------------
    long long births = 0;             ///< channel-epochs with no prior record: the admission-opportunity count (admits_birth + reject_birth + reject_resume).
    long long admits_birth = 0;       ///< of the births, those admitted (the large majority; the denominator the two rejection counts are read against).
    long long reject_birth = 0;       ///< of the births, those rejected at the epoch the arc would have been born.
    long long reject_resume = 0;      ///< of the births, those rejected at a station's first screened epoch of a warm resume (a restored corrupt arc, not a birth).

    // --- re-entry vet -------------------------------------------------------
    // Without the vet, a channel that returns after a gap takes the established path
    // and a failure there is counted in @c onsets; on the 63-station field day, 16
    // of the 17 onsets were such re-entries.  With the vet armed they move to
    // @c reentries, and @c onsets holds channels that went bad while present plus
    // refusals inside a vet window.  That shift is the vet's main field readout, so
    // the two are counted separately.
    long long reentries = 0;          ///< channel-epochs re-screened as a birth because the channel had not been screened for at least adm_revet_gap_ep epochs (admits_reentry + reject_reentry + revet_hold_ep).
    long long admits_reentry = 0;     ///< of the re-entries, those admitted on the return epoch (the large majority; no withhold and no refloat on that epoch).
    /// Of the re-entries, those refused on the return epoch.  On the field day these
    /// were twelve re-entries that the established path counts as onsets when the
    /// vet is off.
    long long reject_reentry = 0;
    /// Channel-epochs of admitted operation inside an open vet window (epochs that
    /// spent vet budget).  A withheld epoch is not counted and does not tick the
    /// window down: it is not evidence of recovery, and a rejected channel that
    /// served out its window while withheld would return unvetted.  With the census
    /// trigger armed, this exceeds @ref revet_census_reads by one for each admitted
    /// epoch that opened a window (an admitted re-entry or a heal consults no
    /// census) and falls short of it by one for each established-path refusal inside
    /// a window (a census read on an epoch that ends withheld).
    long long revet_window_ep = 0;
    /// Refusals made by the tighter re-entry bound (@c network.adm_revet_bound_scale)
    /// that @c adm_bound_m would not have made, at a return epoch or inside a vet
    /// window.  When the bound and the census both fire, the refusal counts here.
    /// Counted apart from @ref revet_census_refusals because the two reach disjoint
    /// cells.  The bound catches channels that are large on arrival (G16 at 670.3 m,
    /// G22 at 347.6 m, both at BSHM00ISR).  The census catches slow ramps that are
    /// small on arrival (G13 at 81.5 m), which no absolute bound separates from the
    /// 61.9 m healthy tail.
    long long revet_bound_refusals = 0;
    /// Consultations attempted: vet-window channel-epochs on the established path
    /// with the census trigger armed.  The denominator for @ref revet_census_absent:
    /// `absent == reads` means the census walk is off, `absent == 0` means every
    /// consultation had fresh evidence to weigh.
    long long revet_census_reads = 0;
    /// Consultations that refused where neither @c adm_bound_m nor the tighter vet
    /// bound would have refused this epoch: the value the census trigger adds,
    /// isolated so that a zero is visible.
    long long revet_census_refusals = 0;
    /// Consultations that found no fresh flag set (the census walk is off, or its
    /// stamp was not the previous loop epoch).  A consultation that did not happen is
    /// not a clean result.
    long long revet_census_absent = 0;
    long long revet_hold_ep = 0;      ///< channel-epochs withheld by the strict posture (network.adm_revet_hold) on a clean re-entry.
    long long revet_expired = 0;      ///< vet windows that ran to completion: the channel served its full window and returned to ordinary established-arc screening.  A window re-armed by a refusal counts here only once it later runs out.

    // --- mid-arc onset and heal ---------------------------------------------
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
    long long withheld_cb_pruned = 0; ///< satellite code-bias (observable-specific bias, OSB) states pruned, so the >= 2-station registration gate is not satisfied by a station whose row this seam removed.
    long long withheld_clk_pruned = 0;///< satellite clock states pruned at a station left with no observable at all for that satellite (the same reason, for the clock column's union).
    long long refloat_ambs = 0;       ///< ambiguity keys newly inserted into the invariant-carry slip funnel (only with carry_invariant).
    long long refloat_reseeds = 0;    ///< ambiguity keys marked pending-reseed (the phase-leveling feed; only with carry_warm).

    // --- census -------------------------------------------------------------
    long long events = 0;             ///< ADMISSION-tier events emitted.
    long long events_held = 0;        ///< events suppressed by the per-cell holdoff.
    double    max_z = 0.0;            ///< run maximum |z| (m).
    double    max_z_tow = -1.0;       ///< GPS time of week (TOW) of that epoch (s; -1 = none yet).
    std::string max_z_id;             ///< its "STATION|SAT|band".
    long long over_bound = 0;         ///< channel-epochs with |z| over adm_bound_m, whether or not the response is armed: the measure-only census's headline.
};

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochEvents;             // network/driver/Epoch.hpp
struct EpochPassA;              // network/driver/Epoch.hpp
struct NetworkSlipFunnel;       // network/slip/SlipFunnel.hpp

/**
 * @brief Screen this epoch's code observables against the products, withhold the
 *        channels that fail, heal the ones that recover, and census the rest.
 *        The file banner describes the statistic and the ordering contract.
 *
 * Returns at once unless the screen or the census is armed (`network.adm_screen`
 * or `network.adm_trace`).  Unit tests call this form through
 * tests/support/NetworkTestRun.hpp.
 *
 * @param run The run (network/driver/Run.hpp).
 *            Written: @c run.ctx (admission records, counters, and the per-epoch
 *            census values the epoch-status writer reads), @c run.traces
 *            (ADMISSION-tier `trace_events` rows) and @c run.setup.stations (a
 *            withheld band's satellite phase-bias and code-bias states, and the
 *            clock state of a satellite left with no observable, are pruned as the
 *            coverage gate prunes a thin key's).
 *            Read: @c run.log (debug logger), @c run.ctx.knobs, and
 *            @c run.engines.slip_qc.epoch_idx (the monotonic processed-epoch index,
 *            the same counter the gap instrumentation and the signal-integrity
 *            classifier use, so their per-epoch totals are directly comparable).
 * @param ep The epoch (network/driver/Epoch.hpp), read only.  Read:
 *           @c ep.se.time (event stamps), @c ep.pass_a.present (which stations are
 *           present) and @c ep.clock.carry_warm (the phase-leveling carry is warm,
 *           which gates the pending-reseed refloat).
 * @param pass_a The epoch's PASS A values.  Written: @c pass_a.modeled, the
 *               per-station observations.  Read for the statistic; on a rejection
 *               the failing band's entries are erased from `observed`, never from
 *               `raw_observed`, which the slip detectors own and which must keep
 *               reporting the true tracking state.
 * @param funnel The epoch's slip funnel, written by every withhold (rejection,
 *               hold, station guard) and by a heal.  @c carry_slipped_ambs gets
 *               the refloat keys when the invariant carry is live.
 *               @c carry_slipped_raw is the raw-detector provenance subset: a
 *               withheld band is a raw observable event (the channel leaves the
 *               model set), so it is recorded there too; otherwise the deviation
 *               carry would keep a level across the hole this seam made.
 * @param events The epoch's events.  Written: @c events.admission.
 */
void networkMeasurementAdmission(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                                 NetworkSlipFunnel& funnel, EpochEvents& events);

/**
 * @brief The RINEX-3 band digit of an observable key ("C1C" -> '1'), or 0 when the
 *        key is not a plain 3-character RINEX-3 code with a band digit 1-9.
 *
 * Exposed for the unit suite (and used by the seam itself) so the band of an
 * observable has one definition.  Combination keys produced by the iono-free path
 * (e.g. "C1C_C2W") are rejected on purpose: this seam screens the uncombined
 * producer's raw channels, and an IF combination has no single band to withhold.
 *
 * @param key Observable key, e.g. "C1C" or "L2W".
 * @return The band digit character, or 0 when @p key has no single band.
 */
char measurementAdmissionBand(const std::string& key);

/**
 * @brief The admission statistic's raw per-observable term
 *        @f$d = C^\text{obs} - m@f$.  Returns false, leaving @p d_out unchanged,
 *        when either side is missing or the difference is non-finite.
 *
 * Exposed so the unit suite pins the statistic itself rather than only its
 * consequences, and so the state-free property has one testable definition.
 *
 * @param obs      One line of sight's observation, with `observed` and `modeled`.
 * @param code_key Code observable key to difference, e.g. "C1C".
 * @param d_out    Receives @f$d@f$ in metres on success.
 * @return True when @p d_out was set.
 */
bool measurementAdmissionD(const PppObservation& obs, const std::string& code_key,
                           double& d_out);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the product-referenced measurement admission.
 *
 * Prints the denominators, the census separation, the response and the warnings.
 * Returns 0 at once unless `ctx.knobs.adm_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkMeasurementAdmissionRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of MeasurementAdmission, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct MeasurementAdmissionCounters {
    // Run tallies of product-referenced measurement admission; every field is
    // documented on the struct.  Bumped only when ctx.knobs.adm_on, and reported by
    // networkMeasurementAdmissionRunEnd.  Named `adm`, and not `admission`, on
    // purpose: `ctx.admission` is the code-bias admission census
    // (@ref NetworkBiasAdmission, network/assembly/BiasAdmission.hpp), a different
    // mechanism, and the two are easy to confuse.
    NetworkMeasurementAdmissionTallies adm;
};

} // namespace GnssProcessor
