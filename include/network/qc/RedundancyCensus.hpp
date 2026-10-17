#pragma once
/**
 * @file RedundancyCensus.hpp
 * @brief Redundancy and testability census: a statistical monitor for rows whose
 *        misfit no residual test can see (docs/SPEC_redundancy_census.md).
 *
 * Measure-only and off by default.  It is not fingerprinted: it is a forward-only
 * readout and does not change the meaning of stored state (the same treatment as
 * rhs_attrib), so a shared-snapshot replay leg with only this
 * armed stays A/B-comparable against an unarmed control.  It changes no weight,
 * state or decision (standing invariant #6), and §7 of the spec assigns any
 * automatic response to the cross-flag to a separate change.
 *
 * ## Why redundancy and not magnitude
 *
 * Magnitude alone cannot separate a fault from an extreme, and the two obvious
 * hard responses are both wrong.  A large raw innovation can be legitimate: a 1 ms
 * receiver clock step is 3·10⁵ m on every row of a station at once.  Excluding
 * single-frequency cells as a class violates the standing prefer-fallback
 * directive; single-frequency lines of sight are valuable data, and their problem
 * is not their band count but that some topologies leave them uncheckable.
 *
 * The discriminator used here is testability.  Per row, the local redundancy
 * number
 *
 *   @f[ r_i = 1 - w_i\,a_i^\top \mathrm{Cov}(\theta)\,a_i \in [0,1] @f]
 *
 * is the fraction of that row's information cross-checked by the rest of the
 * network.  Per Baarda, the minimal detectable bias
 * @f$\mathrm{MDB}_i = \delta_0\,\sigma_i/\sqrt{r_i} \to \infty@f$ as
 * @f$r_i \to 0@f$.  A row with @f$r_i \approx 0@f$ only feeds a state: its misfit
 * is invisible to every residual-based test (post-fit RAIM, σ̂₀, robust
 * weighting) however large it is.  The typical case is a code row that becomes
 * the only observation of an otherwise unconstrained slant-iono state once its
 * station has no SH ties; the state can then absorb an
 * arbitrarily large error without any residual moving.
 *
 * ## The alarm condition is the cross-flag
 *
 *   uncheckable (@f$r_i < \varepsilon@f$)  ×  large centred innovation (@f$s_i > S@f$)
 *
 * Neither half alone suffices.  Healthy single-frequency risers are uncheckable
 * with small @f$s@f$ and are kept, per the fallback directive.  Storm extremes
 * carry large @f$s@f$ on healthy @f$r@f$, and the redundant network absorbs and
 * localizes them.  The intersection identifies a parked error with a very small
 * false-positive surface, and it fires for as long as the cell stays parked,
 * before the discharge.
 *
 * ## The two censuses
 *
 *  - **Row redundancy (INC-1):** per station and data class, the row count, the
 *    redundancy mass @f$\sum r_i@f$, the uncheckable count, and among uncheckable
 *    rows the largest centred @f$s_i@f$ with its (station, satellite, class)
 *    identity.  The cross-flag emits a `REDUNDANCY` / `uncheckable_hot_cell`
 *    event, rate-limited per cell.
 *  - **Unobservable locals (INC-2):** the state-side complement.  Local columns
 *    whose @f$N_{rr}@f$ LDLᵀ pivot is below a relative floor carry no information
 *    this epoch.  The one-observation-one-state case appears in INC-1 instead, as
 *    @f$r \approx 0@f$, so the two censuses are complementary.  Emits
 *    `REDUNDANCY` / `unobservable_local` when a column enters the set (the
 *    decoupling edge) and counts the exit (the re-coupling edge).
 *
 * ## Cost and the two-tier escalation
 *
 * A full sweep is a per-station GEMM against the datum-frame shared covariance,
 * measured at 100–300 ms/epoch on the 63-station FT63 tier (5–20 % of its
 * 1.6–1.8 s epoch).  The cost is controlled by two tiers:
 *
 *  - `network.redundancy_census_stride` (recommended 10, one sweep per 5 min at
 *    30 s cadence).  A park persists for many epochs, so a detection latency of
 *    minutes is acceptable for an alarm, and the cost is amortized.
 *  - `network.redundancy_escalate_sigma`, the cheap tier.  The centred prefit
 *    census is O(rows) and runs every epoch when this is armed; any station whose
 *    largest centred @f$s@f$ exceeds it gets a targeted single-station sweep at
 *    the next epoch (about 2–5 ms).  The cheap tier detects a large deviation;
 *    the redundancy sweep then tests whether that row is checkable.
 *  - A flight-recorder trip epoch (an armed `network.rhs_attrib_tow` window)
 *    always forces a full sweep, so the attribution dump and the redundancy
 *    census describe the same epoch.
 *
 * ## Reading the numbers on an unconverged run
 *
 * Both halves of the cross-flag are structurally inflated during convergence, so
 * a healthy cold start can look like a network full of parked errors:
 *
 *  - **Many data rows read uncheckable.**  A 10-epoch FT63 cold start measured
 *    43 % (mean r = 0.35).  A free ambiguity absorbs its own arc's phase misfit
 *    exactly, the same property that makes the post-fit phase pool identically
 *    zero.  The fraction falls as the ambiguities tighten.
 *  - **A freshly seeded arc reads s ≫ S.**  The seed makes the arc's first phase
 *    innovation about 0, but at a station whose receiver clock is not yet
 *    absorbed into the linearization point the station's other phase rows sit at
 *    the clock offset (tens to hundreds of km for receivers with unsteered
 *    clocks).  The fresh arc is then the outlier against its station's median,
 *    at r = 0 exactly, and it cross-flags.  On the same cold start this gave
 *    0.11 % of data rows as hot cells.
 *
 * Both statements are true of a converging system (the cell is uncheckable and
 * is far from its station's common mode), so they are not suppressed here.  The
 * thresholds are calibrated for, and the field gates run on, a converged warm
 * leg; a cold smoke exercises the wiring, not the alarm.
 *
 * ## Deviation from the spec
 *
 * §4 has the redundancy census consume the INC-1 metric of the prefit census and warn
 * when armed without `network.trace_prefit_census`.  The centred metric is
 * computed here instead (identical definition, same hoisted classifier, one
 * extra O(rows) pass when both are armed), so no inert-guard is needed and the
 * escalation channel works without the prefit census.
 */

#include <memory>
#include <vector>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/assembly/RowClassify.hpp"
#include "network/driver/RunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"
#include <string>

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

struct NetworkRunContext;   // network/state/RunContext.hpp; included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp; included in the .cpp only

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Post-solve redundancy and unobservable-local census for this epoch.
 *
 * Call after the epoch's solve (the kernel's per-station LDLᵀ factors and the
 * shared covariance must be current), from the driver's SB_FRAMEDIAG block and
 * before @c networkRhsAttribution, so the trip predicate is still unconsumed.
 * The driver gates the call on @c ctx.knobs.redun_census_on, so an unarmed run
 * never enters this translation unit.
 *
 * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log),
 *            run.setup.stations (stations), run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
 *           ep.solve.plan.shared_datum (plan_datum), ep.assembly.asm_res (asm_res),
 *           ep.assembly.est_index_of (est_index_of), ep.solve.x_s (x_s).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: Run context: holds the stride phase, the escalation latch, the event
 *     rate-limit memory and every counter.
 *   - @p config_ = run.config: Full configuration (reads @c network.redundancy_*).
 *   - @p log = run.log: Main run logger (one-shot degradation warnings).
 *   - @p traces = run.traces: Run traces (the `network_events.csv` sink).
 *   - @p se = ep.se: This synced epoch (week/tow stamp).
 *   - @p stations = run.setup.stations: Network station roster (names and StateManagers for
 *     labels).
 *   - @p est = run.engines.est: The solved estimator (covariance and kernel accessors).
 *   - @p plan_datum = ep.solve.plan.shared_datum: This epoch's minimum-constraint datum pins:
 *     the exact gauges, zeroed out of the covariance before any leverage is computed.
 *   - @p asm_res = ep.assembly.asm_res: PASS A/B station assemblies (the row stacks).
 *   - @p est_index_of = ep.assembly.est_index_of: Station → kernel slot map (−1 = not folded
 *     this epoch).
 *   - @p x_s = ep.solve.x_s: The epoch's accepted post-fit shared solution. The census itself
 *     does not read it (@f$r_i@f$ depends on the geometry alone), but the block-Baarda
 *     (BlockFormal) and congruence riders on this sweep (docs/SPEC_redundancy_census.md) need
 *     the post-fit residual @f$y - A_s x_s - A_r x_r@f$. The census forms it once per swept
 *     station and passes it to both, and the riders inherit the census's stride and escalation
 *     schedule instead of adding their own. Pass @c sg_.x_s (the final accepted solution,
 *     after any within-epoch re-solve); an empty vector disarms both riders.  (sg_ was
 *     the driver's solve result until the epoch object took it over; the binding above reads it as
 *     ep.solve.x_s.)
 * @param events The epoch's events; writes census (the epoch-status scalars and the hits).
 */
void networkRedundancyCensus(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events);

// ---------------------------------------------------------------------------
//  Testable pieces (unit-test seam; no run state, no I/O)
// ---------------------------------------------------------------------------

/// Minimum rows a centring pool needs before its median is used.  Below this the
/// pool is skipped for the epoch and counted: centring on the median of two rows
/// creates excursions instead of removing a common mode.
inline constexpr int kCensusMinPoolRows = 4;

/**
 * @brief One station's centred prefit census for one epoch.
 *
 * `s[row]` is NaN wherever the metric does not apply: a non-data row (pin or
 * coupling), a data row whose class is outside the class mask, a data row whose
 * centring pool was skipped for low population, or a row with a non-finite innovation
 * or non-positive weight.
 */
struct StationPrefitCensus {
    std::vector<double> s;             ///< centred standardized residual per row (NaN = n/a).
    double med[3]{0.0, 0.0, 0.0};      ///< per-pool median of y (code, phase, other).
    int    n[3]{0, 0, 0};              ///< per-pool row population.
    int    n_pool_skipped{0};          ///< pools present but below @ref kCensusMinPoolRows.
    int    n_data_rows{0};             ///< data rows carrying a finite @c s.
    double max_s{0.0};                 ///< largest finite @c s over data rows.
    Eigen::Index max_row{-1};          ///< row index of @ref max_s (−1 = none).
};

/**
 * @brief Compute one station's centred prefit census (the magnitude half of the
 *        cross-flag).
 *
 * Per station and per row type t ∈ {code, phase, other}: m_t is the median of that
 * type's innovations, and s_i = |y_i − m_t|·√w_i.  This acts as a between-satellite
 * single difference and is free of the receiver clock, so a whole-station 1 ms
 * clock step cancels to about 0 while a single parked cell keeps its full
 * magnitude.  `iono_sh_row` rows carry no receiver clock and are standardized raw
 * (not centred).
 *
 * Cost: O(rows) plus one `nth_element` per non-empty pool, small enough to run
 * alongside any existing row walk.
 *
 * @param cls            Per-row classes from the hoisted classifier (INC-0);
 *                       must be sized to @c R.y.size().
 * @param R              The station assembly.
 * @param min_pool_rows  Minimum population for a pool's median to be used.
 * @param class_mask     Data classes the statistic is reported for, one bit per class
 *                       (@c dataClassBit).  The cross-flag passes the prefit census's
 *                       event-class mask (@c network.prefit_event_classes), so a class
 *                       that census keeps out of its events, phase by default, is not
 *                       cross-flagged here either.  The centring pools still use every
 *                       data row; a masked row only gets no @c s.  The default covers
 *                       every class.
 */
StationPrefitCensus stationPrefitCensus(const std::vector<NetRowClass>& cls,
                                        const StationAssembly& R,
                                        int min_pool_rows = kCensusMinPoolRows,
                                        unsigned class_mask = ~0u);

/**
 * @brief The cross-flag: the census's single decision.
 *
 * Neither half alone is a finding.  A healthy single-frequency riser is
 * uncheckable with a small @p s (kept, per the prefer-fallback directive); a
 * storm extreme carries a large @p s on a healthy @p r (the redundant network
 * absorbs and localizes it).  Only the intersection marks a parked error.
 *
 * @param r          The row's local redundancy number, in [0,1].
 * @param s          Its centred standardized prefit deviation (NaN ⇒ never flag:
 *                   the magnitude half was not measurable this epoch).
 * @param eps        Uncheckable threshold (@c network.redundancy_eps).
 * @param hot_sigma  Magnitude threshold (@c network.redundancy_hot_sigma).
 */
inline bool isUncheckableHotCell(double r, double s, double eps, double hot_sigma) {
    return std::isfinite(s) && r < eps && s > hot_sigma;
}

/**
 * @brief Whether this pass takes a full sweep.
 *
 * @param ep     Zero-based census pass index (the stride phase), so pass 0 always
 *               sweeps and a run does not wait `stride` epochs for its first
 *               measurement.
 * @param stride @c network.redundancy_census_stride (≤ 0 ⇒ escalation-only).
 * @param trip   A flight-recorder trip epoch, which always forces a full sweep
 *               so the attribution dump and the census describe the same epoch.
 */
inline bool censusSweepsThisEpoch(long long ep, int stride, bool trip) {
    return trip || (stride > 0 && (ep % stride) == 0);
}

/**
 * @brief Flag the local columns of @p pivots that carry no information.
 *
 * The threshold is relative to the station's own median pivot
 * (`eps · median(pivots)`), because the absolute scale of @f$N_{rr}@f$ is set by
 * the measurement weights and differs by decades between an ambiguity column and
 * a ZWD column.  A non-positive or non-finite pivot is always flagged.
 *
 * @note The kernel bounds the window in which this can fire from below.
 *       `NetworkNormalEquations::addStation` rejects a station whose smallest
 *       pivot falls below @f$10^{-12}@f$ times the block's reference scale, so a
 *       truly singular local block never reaches a solve or this census.  INC-2
 *       can therefore fire only in @f$[10^{-12},\ \varepsilon_\text{pivot})\times@f$
 *       median: four decades wide at the 1e-8 default, and empty if the knob is
 *       set at or below 1e-12.  The loader warns in that case; a zero INC-2 count
 *       means "no orphaned states" only when the knob is above 1e-12.
 *
 * @param pivots  Per-local-column LDLᵀ pivots in original column order
 *                (@c NetworkNormalEquations::stationLocalPivots).
 * @param eps     Relative floor (@c network.redundancy_pivot_eps).
 * @return Indices of the flagged columns, ascending.
 */
std::vector<Eigen::Index> unobservableLocalColumns(const Eigen::VectorXd& pivots,
                                                   double eps);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the redundancy and testability census.
 *
 * The redundancy and testability census: sweeps, data rows, uncheckable rows and the
 * cross-flag, the INC-2 pivot floor, the closure bound and the escalation path.
 *
 * Gate: returns 0 at once unless `ctx.knobs.redun_census_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkRedundancyCensusRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of RedundancyCensus, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RedundancyCensusCounters {

    // --- redundancy / testability census ---
    // Standing invariant #4 in its statistical form.  The failure mode this census
    // must be able to disprove is "the sweep ran and found nothing", which cannot be
    // told apart from "the sweep never ran" without denominators, so the passes, the
    // sweeps and the rows are reported before any finding.  `redun_max_cold_*` exists
    // for the opposite reason: the prefer-fallback directive says uncheckable but quiet
    // rows (for example single-frequency rising satellites) must be counted and never
    // evented, and keeping the worst one makes that a checkable claim rather than an
    // assertion.
    long long redun_passes = 0;             ///< seam invocations (the denominator for everything else).
    long long redun_sweeps = 0;             ///< epochs on which at least one station was swept.
    long long redun_sweeps_full = 0;        ///< of those, stride/trip full sweeps.
    long long redun_sweeps_targeted = 0;    ///< of those, escalation-driven single-station sweeps.
    long long redun_sweeps_trip = 0;        ///< of the full sweeps, those forced by a flight-recorder trip epoch.
    long long redun_station_sweeps = 0;     ///< (station, epoch) pairs swept: the real cost unit.
    long long redun_escalate_flagged = 0;   ///< (station, epoch) latches raised by the free tier: the escalation's opportunity count.
    long long redun_escalations = 0;        ///< targeted sweeps taken from those latches.  Necessarily 0 under stride 1 (the scheduled sweep already covered every station), so it must never be read as "the free tier found nothing"; redun_escalate_flagged answers that.
    long long redun_rows = 0;               ///< data rows examined in sweeps.
    long long redun_rows_all = 0;           ///< all rows examined (the closure domain: pins and coupling rows carry leverage too).
    long long redun_uncheckable = 0;        ///< data rows with r_i < eps (uncheckable; counted, not acted on).
    long long redun_uncheckable_hot = 0;    ///< of those, rows that also exceeded the magnitude threshold: the cross-flag.
    long long redun_unchk_class_skipped = 0; ///< of those, rows whose class is outside network.prefit_event_classes: counted, never cross-flagged.
    long long redun_events = 0;             ///< events emitted (after the rate limit): hot cells and unobservable locals.
    long long redun_pool_skips = 0;         ///< (station, epoch, pool) centring pools skipped for population < 4.
    long long redun_r_clamped = 0;          ///< rows whose r_i hit the kernel's [0,1] clamp at 0 (leverage >= 1/w).
    double    redun_mass = 0.0;             ///< sum of r_i over data rows: the redundancy mass of what was swept.
    long long redun_pivot_cols = 0;         ///< local columns examined by the INC-2 pivot census.
    long long redun_pivot_flagged = 0;      ///< of those, columns below the relative pivot floor (column-epochs).
    long long redun_unobs_entered = 0;      ///< decoupling edges: (station, key) first appearances, each evented.
    long long redun_unobs_cleared = 0;      ///< re-coupling edges: (station, key) that regained information.
    bool      redun_closure_any = false;    ///< at least one closure sample exists.
    double    redun_closure_min_slack = 0.0;///< worst (smallest) n_free − trace(H); must stay >= 0.
    long long redun_closure_neg = 0;        ///< sweeps whose slack went negative (a covariance/row-stack defect, not a data finding).
    double    redun_max_hot_s = 0.0;        ///< largest cross-flagged centred deviation over the run.
    std::string redun_max_hot_id;           ///< its (station/sat/class @tow) identity.
    double    redun_max_cold_s = 0.0;       ///< largest centred deviation on an uncheckable row that stayed below the cross-flag.
    std::string redun_max_cold_id;          ///< its (station/sat/class @tow) identity.
    double    redun_wall_ms = 0.0;          ///< run-total wall time inside the census seam (the cost claim).

    // Redundancy / testability census: the stride phase and event clock, and the
    // one-shot warn latches (observer state of a measure-only instrument).
    long long redun_epochs = 0;            ///< census seam passes: the stride phase counter and the event-holdoff clock.
    bool      redun_cov_warned    = false; ///< one-shot: armed with no solved covariance to read.
    bool      redun_closure_warned = false;///< one-shot: trace(H) exceeded the free-parameter bound.
};

} // namespace GnssProcessor
