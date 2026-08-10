#pragma once
/**
 * @file NetworkRedundancyCensus.hpp
 * @brief Redundancy / testability census — the statistical parked-garbage
 *        monitor (docs/SPEC_redundancy_census.md).
 *
 * Measure-only, default-OFF, NOT fingerprinted (forward-only readout; stored
 * state meaning unchanged — the adaptive_iono_q / rhs_attrib precedent), so a
 * shared-snapshot replay leg with only this armed stays A/B-legal against an
 * unarmed control.  It touches no weight, no state and no decision: standing
 * invariant #6 in the strictest reading, and §7 of the spec pre-declares that
 * ANY automatic response to the cross-flag belongs to a different card.
 *
 * ## Why redundancy and not magnitude
 *
 * Era-2's parked cell carried a steady 2.6·10⁵σ common-removed prefit
 * innovation, but magnitude alone cannot separate a fault from an extreme, and
 * the two obvious hard responses are both wrong: raw magnitude is legitimate
 * (a 1 ms receiver clock step is 3·10⁵ m on every row of a station at once),
 * and excluding single-frequency cells as a class violates the standing
 * prefer-fallback directive.  Single-frequency lines of sight are valuable
 * data; their problem is not their band count, it is that under certain
 * topologies they become UNCHECKABLE.
 *
 * The principled discriminator is TESTABILITY.  Per row the local redundancy
 * number
 *
 *   @f[ r_i = 1 - w_i\,a_i^\top \mathrm{Cov}(\theta)\,a_i \in [0,1] @f]
 *
 * is the fraction of that row's information cross-checked by the rest of the
 * network.  Baarda: the minimal detectable bias scales as
 * @f$\mathrm{MDB}_i = \delta_0\,\sigma_i/\sqrt{r_i} \to \infty@f$ as
 * @f$r_i \to 0@f$.  A row with @f$r_i \approx 0@f$ is a pure STATE FEEDER: its
 * misfit is invisible to every residual-based test — post-fit RAIM, σ̂₀, robust
 * weighting — no matter how large it is.  That is exactly what the
 * (BSHM00ISR, G07) code row became when the iono-deweight gate cut the
 * station's SH ties: one observation feeding one otherwise-unconstrained slant
 * state, free to park 188 km without a single residual anywhere moving.
 *
 * ## The alarm condition is the CROSS-FLAG
 *
 *   uncheckable (@f$r_i < \varepsilon@f$)  ×  large centred innovation (@f$s_i > S@f$)
 *
 * Neither half alone suffices.  Healthy single-frequency risers are
 * uncheckable with small @f$s@f$ (fine — keep them, per the fallback
 * directive); storm extremes carry large @f$s@f$ on healthy @f$r@f$ (fine — the
 * redundant network absorbs and localizes them).  The INTERSECTION is parked
 * garbage with essentially zero false-positive surface, and it fires DURING the
 * park (era-2: 17+ epochs of warning), not at the discharge.
 *
 * ## The two censuses
 *
 *  - **INC-1, row redundancy** — per station, per data class: rows, redundancy
 *    mass @f$\sum r_i@f$, the uncheckable count, and among uncheckable rows the
 *    largest centred @f$s_i@f$ with its (station, satellite, class) identity.
 *    The cross-flag emits a `REDUNDANCY` / `uncheckable_hot_cell` event,
 *    rate-limited per cell.
 *  - **INC-2, unobservable locals** — the state-side complement: local columns
 *    whose @f$N_{rr}@f$ LDLᵀ pivot is below a relative floor, i.e. states
 *    carrying NO information this epoch.  (The one-observation-one-state case
 *    shows up in INC-1 instead, as @f$r \approx 0@f$ — the two censuses are
 *    complementary, not redundant.)  Emits `REDUNDANCY` / `unobservable_local`
 *    on the DECOUPLING edge and counts the re-coupling edge.
 *
 * ## Cost and the two-tier escalation
 *
 * A full sweep is a per-station GEMM against the datum-frame shared covariance
 * — measured at 100–300 ms/epoch on the 63-station FT63 leg, i.e. 5–20 % of
 * that tier's 1.6–1.8 s epoch.  Not free.  Hence:
 *
 *  - `network.redundancy_census_stride` (recommended 10 = one sweep per 5 min
 *    at 30 s cadence).  A park persists for many epochs, so stride-10 detection
 *    latency is minutes — acceptable for an alarm, and the cost is amortized.
 *  - `network.redundancy_escalate_sigma` — the FREE tier.  The centred prefit
 *    census is O(rows) and runs every epoch when this is armed; any station
 *    whose largest centred @f$s@f$ exceeds it gets a TARGETED single-station
 *    sweep at the next epoch (~2–5 ms).  The free tier asks "is something
 *    big?"; redundancy answers "is it checkable?" — exactly where and when it
 *    matters.
 *  - A flight-recorder trip epoch (an armed `network.rhs_attrib_tow` window)
 *    always forces a FULL sweep, so the attribution dump and the redundancy
 *    census describe the same epoch.
 *
 * ## Reading the numbers on an UNCONVERGED run (measured, FT63 cold smoke)
 *
 * Both halves of the cross-flag are structurally inflated during convergence,
 * and a reader who does not know that will mistake a healthy cold start for a
 * network full of parked garbage:
 *
 *  - **43 % of DATA rows read uncheckable** on a 10-epoch cold start (measured:
 *    15541 / 35948, mean r = 0.35).  That is not a defect — a FREE ambiguity
 *    absorbs its own arc's phase misfit exactly, which is the same property
 *    that makes the post-fit phase pool identically zero.  The number falls as
 *    ambiguities tighten.
 *  - **A freshly seeded arc reads s ≫ S.**  The seed is chosen so the arc's
 *    first phase innovation is ~0, but at a station whose receiver clock has
 *    not yet been absorbed into the linearization point the rest of that
 *    station's phase rows sit at the clock offset (measured on this fixture:
 *    FFMJ00DEU 259 km, URUM00CHN 42 km — timing-lab receivers with unsteered
 *    clocks, against a 0.23 m median across the other 61 stations).  The fresh
 *    arc is then the OUTLIER against its own station's median, at r = 0 exactly,
 *    and it cross-flags.  Measured: 40 hot cells over 11 cold epochs (0.11 % of
 *    data rows), concentrated on exactly this signature.
 *
 * Both are true statements about a converging system — the cell really is
 * uncheckable and really is far from its station's common mode — so they are
 * NOT suppressed here.  The consequence for grading is procedural: this card's
 * thresholds are calibrated for, and its field gates run on, a CONVERGED WARM
 * leg.  A cold smoke exercises the wiring, not the alarm.
 *
 * ## Deviation from the spec, recorded
 *
 * §4 has this card consume the prefit census card's INC-1 metric and WARN when
 * armed without `network.trace_prefit_census`.  That card had not landed when
 * this one was built, so the centred metric is computed HERE (identical
 * definition, same hoisted classifier, one extra O(rows) pass when both are
 * armed).  The consequence is strictly better: no INERT-guard is needed and
 * the escalation channel is live on its own.
 */

#include <memory>
#include <vector>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Post-solve redundancy + unobservable-local census for this epoch.
 *
 * Call AFTER the epoch's solve (the kernel's per-station LDLᵀ factors and the
 * shared covariance must be current) from the driver's SB_FRAMEDIAG block,
 * BEFORE @c networkRhsAttribution so the trip predicate is still unconsumed.
 * The driver gates the call on @c ctx.knobs.redun_census_on, so an unarmed run
 * never enters this translation unit.
 *
 * @param ctx          Run context: the stride phase, the escalation latch, the
 *                     event rate-limit memory and every counter live here.
 * @param config_      Full configuration (reads @c network.redundancy_*).
 * @param log          Main run logger (one-shot degradation warnings).
 * @param traces       Run traces (the `network_events.csv` sink).
 * @param se           This synced epoch (week/tow stamp).
 * @param stations     Network station roster (names + StateManagers for labels).
 * @param est          The SOLVED estimator (covariance + kernel accessors).
 * @param plan_datum   This epoch's minimum-constraint datum pins — the exact
 *                     gauges, zeroed out of the covariance before any leverage
 *                     is computed.
 * @param asm_res      PASS A/B station assemblies (the row stacks).
 * @param est_index_of Station → kernel slot map (−1 = not folded this epoch).
 * @param x_s          The epoch's accepted POST-FIT shared solution.  The census
 *                     itself does not read it — @f$r_i@f$ is a property of the
 *                     geometry alone — but the BlockFormal rider that hangs off
 *                     this sweep (docs/SPEC_block_baarda.md) needs the post-fit
 *                     residual @f$y - A_s x_s - A_r x_r@f$, and the whole point of
 *                     riding this seam is that the block sweep inherits its
 *                     stride/escalation schedule instead of adding a second one.
 *                     Pass @c sg_.x_s (the FINAL accepted solution, after any
 *                     within-epoch re-solve); an empty vector disarms the rider.
 */
void networkRedundancyCensus(NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             NetworkRunTraces& traces, const SyncedEpoch& se,
                             const std::vector<NetStation>& stations,
                             const NetworkEstimator& est,
                             const NetworkEstimator::Datum& plan_datum,
                             const std::vector<StationAssembly>& asm_res,
                             const std::vector<int>& est_index_of,
                             const Eigen::VectorXd& x_s);

// ---------------------------------------------------------------------------
//  Testable pieces (unit-test seam; no run state, no I/O)
// ---------------------------------------------------------------------------

/// Minimum rows a centring pool needs before its median is trusted.  Below
/// this the pool is SKIPPED for the epoch and counted: centring on a median of
/// two rows manufactures excursions instead of removing a common mode.
inline constexpr int kCensusMinPoolRows = 4;

/**
 * @brief One station's centred prefit census for one epoch.
 *
 * `s[row]` is NaN wherever the metric does not apply — a non-DATA row (pin or
 * coupling), a data row whose centring pool was skipped for want of population,
 * or a row with a non-finite innovation / non-positive weight.
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
 * @brief Compute one station's centred prefit census (the cross-flag's
 *        magnitude half).
 *
 * Per station, per row type t ∈ {code, phase, other}: m_t = median of that
 * type's innovations, then s_i = |y_i − m_t|·√w_i.  This is effectively a
 * between-satellite single difference — receiver-clock-free by construction, so
 * a whole-station 1 ms clock step cancels to ≈ 0 while a single parked cell
 * keeps its full magnitude.  `iono_sh_row` rows carry no receiver clock and are
 * standardized RAW (no centring).
 *
 * O(rows) plus one `nth_element` per non-empty pool — it rides any existing row
 * walk for free.
 *
 * @param cls            Per-row classes from the hoisted classifier (INC-0);
 *                       must be sized to @c R.y.size().
 * @param R              The station assembly.
 * @param min_pool_rows  Minimum population for a pool's median to be trusted.
 */
StationPrefitCensus stationPrefitCensus(const std::vector<NetRowClass>& cls,
                                        const StationAssembly& R,
                                        int min_pool_rows = kCensusMinPoolRows);

/**
 * @brief THE CROSS-FLAG — the card's whole decision, in one place.
 *
 * Neither half alone is a finding.  A healthy single-frequency riser is
 * uncheckable with a small @p s (keep it — the prefer-fallback directive); a
 * storm extreme carries a large @p s on a healthy @p r (the redundant network
 * absorbs and localizes it).  Only the INTERSECTION is parked garbage.
 *
 * @param r          The row's local redundancy number, in [0,1].
 * @param s          Its CENTRED standardized prefit deviation (NaN ⇒ never flag:
 *                   the magnitude half was not measurable this epoch).
 * @param eps        Uncheckable threshold (@c network.redundancy_eps).
 * @param hot_sigma  Magnitude threshold (@c network.redundancy_hot_sigma).
 */
inline bool isUncheckableHotCell(double r, double s, double eps, double hot_sigma) {
    return std::isfinite(s) && r < eps && s > hot_sigma;
}

/**
 * @brief Does this pass take a FULL sweep?
 *
 * @param ep     Zero-based census pass index (the stride phase, so pass 0 always
 *               sweeps — a run must not have to wait `stride` epochs for its
 *               first measurement).
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
 * The threshold is RELATIVE to the station's own median pivot
 * (`eps · median(pivots)`), because @f$N_{rr}@f$'s absolute scale is set by the
 * measurement weights and varies by decades between an ambiguity column and a
 * ZWD column.  A non-positive or non-finite pivot is always flagged.
 *
 * @note The FIRABLE WINDOW is bounded from below by the kernel itself.
 *       `NetworkNormalEquations::addStation` REJECTS a station whose smallest
 *       pivot falls below @f$10^{-12}@f$ times the block's reference scale, so a
 *       truly singular local block never reaches a solve (and therefore never
 *       reaches this census).  INC-2 can only ever fire in
 *       @f$[10^{-12},\ \varepsilon_\text{pivot})\times@f$ median — four decades
 *       wide at the 1e-8 default, and EMPTY if the knob is set at or below
 *       1e-12.  The loader warns about that case; do not read a quiet INC-2 as
 *       "no orphaned states" without checking the knob first.
 *
 * @param pivots  Per-local-column LDLᵀ pivots in ORIGINAL column order
 *                (@c NetworkNormalEquations::stationLocalPivots).
 * @param eps     Relative floor (@c network.redundancy_pivot_eps).
 * @return Indices of the flagged columns, ascending.
 */
std::vector<Eigen::Index> unobservableLocalColumns(const Eigen::VectorXd& pivots,
                                                   double eps);

} // namespace GnssProcessor
