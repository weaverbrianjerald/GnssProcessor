#pragma once
/**
 * @file Mdb.hpp
 * @brief Baarda reliability numbers (MDB) on the network path;
 *        docs/SPEC_redundancy_census.md.
 *
 * Measure-only and off by default; with it off, the output is byte-identical.
 * Fingerprint-safe: a forward-only readout that changes the meaning of no stored
 * state (as rhs_attrib and the redundancy census are), so a
 * shared-snapshot replay leg with only this armed remains a legal A/B against an
 * unarmed control.  Nothing here writes a weight, a state or a decision; §9 of the
 * spec assigns any gate, deweight or admission driven by MDB to a separate card.
 *
 * ## Purpose
 *
 * The redundancy census counts uncheckable rows (@f$r_i < \varepsilon@f$) and RAIM
 * standardizes residuals; neither states, in fault units, how large a fault could
 * go undetected in a given row.  Baarda's minimal detectable bias is that number,
 * per row, in metres:
 *
 *   @f[ \mathrm{MDB}_i = \frac{\sigma_i\,\delta_0}{\sqrt{r_i}},
 *       \qquad \delta_0 = k + \Phi^{-1}(0.80), @f]
 *
 * and it costs one division wherever @f$r_i@f$ already exists.  The computation is a
 * port: @c KalmanFilter::computeResidualQc_ computes the same quantities on the PPP
 * path, and both paths call @c filter/BaardaReliability.hpp, so the two share one
 * convention by construction.
 *
 * ## Two reporting rules
 *
 *  1. **Formal MDB inherits the optimism of the formal σ.**  Card D5 measured the
 *     formal @f$\sigma@f$ 10–30x optimistic on this data; MDB scales linearly with
 *     @f$\sigma@f$, so a formal MDB is optimistic by the same factor.  Every trace
 *     row therefore carries both `mdb_formal` and
 *     @f$\texttt{mdb\_scaled} = \texttt{mdb\_formal}\times\hat\sigma_{0,f}@f$, the
 *     per-family empirical inflation measured by the P1-4 post-fit
 *     @f$\hat\sigma_0@f$ pass (@c network.trace_postfit_sigma0).  When that pass is
 *     not armed, or its pool for this family is empty (the normal state of the
 *     phase pool under free ambiguities), the scaled columns are blank and
 *     `sigma0_src` reads `absent`.  They are never filled with the formal value or
 *     with 1.0: a scaled column equal to the formal one would be indistinguishable
 *     from a calibrated answer.
 *     The opposite case is tagged as well.  A @f$\hat\sigma_{0,f} < 1@f$ makes the
 *     scaled MDB smaller than the formal one, which reads as a tighter guarantee.
 *     On the phase family the meaning is the reverse: that pool's post-fit
 *     dispersion is near zero because each phase row is fitted exactly by its own
 *     free ambiguity, which is the same fact as "these rows carry no redundancy".
 *     Such bins are reported, never clamped (the prior-residual census's rule: the
 *     deflation is the measurement), and tagged `*_deflating`, so a reader keying
 *     on `sigma0_src` cannot mistake one for a calibrated tightening.
 *  2. **MDB is a property of the geometry, not a test.**  It turns the census's
 *     binary "uncheckable" into a quantity and makes the RAIM thresholds
 *     interpretable.  `mdb_event_m` is a reporting threshold; no decision reads it.
 *
 * ## Row sources
 *
 * Two feed sites, both in loops that already hold @f$r_i@f$ (no new sweep, no new
 * covariance product):
 *
 *  - the **RAIM pool** (`network.raim_redundancy_normalize` on a live network.raim,
 *    B2): carried-ambiguity phase rows, every warm epoch, @c Raim.cpp;
 *  - the **redundancy-census sweep** (`network.redundancy_census_stride` /
 *    `_escalate_sigma`): every data row of a swept station, at the census's own
 *    stride and escalation, @c RedundancyCensus.cpp.
 *
 * A run may arm either or both.  The loader throws when neither is armed, because
 * there is then no @f$r_i@f$ to divide by and the run would report zero rows in a
 * form indistinguishable from a genuine negative result.
 *
 * ## The uncheckable join, and its two thresholds
 *
 * Spec §INC-1 asks that the rows at @f$\infty@f$ reconcile against the census's
 * `redun_uncheckable` population.  The two are governed by different thresholds:
 *
 *  - MDB is @f$\infty@f$ at @f$r_i \le@f$ @ref kBaardaRedundancyFloor (1e-6), the
 *    arithmetic floor below which the division has no meaning;
 *  - the census calls a row uncheckable at @f$r_i <@f$ `network.redundancy_eps`
 *    (0.05 by default), a policy floor four decades looser.
 *
 * So @f$n_\infty \subseteq n_\text{uncheckable}@f$, and an equality assertion
 * between them would be wrong.  The seam reconciles the pair of numbers that must
 * agree exactly: over the census-fed rows it independently recounts the data rows
 * and the rows below `redundancy_eps`, and compares them with the census's own
 * `ep_redun_rows` / `ep_uncheckable_rows` for the same epoch.  A mismatch is a bug
 * in one of the two instruments and is reported as such; the @f$\infty@f$ count is
 * reported separately as a subset.
 *
 * ## Cost
 *
 * One division, one compare and a @c push_back per row in loops that already run,
 * plus one @c nth_element per non-empty (constellation x class) bin per epoch.  The
 * RAIM pool is small (carried-ambiguity phase rows only); the census sweep is large
 * but strided, and its own cost is 100–300 ms/epoch at 63 stations, so the added
 * cost stays well under the §3 budget.  @ref NetworkMdbTallies::wall_ms times the
 * flush only; the per-row cost is inside the host loops and cannot be separated
 * (the prior-residual census has the same limitation for the same reason).
 *
 * Seam contract per CLAUDE.md: free functions over the run context, owning no
 * state (the epoch accumulator is a member of @c NetworkRunContext::carry and
 * every counter a member of @c ctx.counters).
 */

#include <cstddef>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "filter/BaardaReliability.hpp"
#include "network/assembly/RowClassify.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp, included in the .cpp only

// ---------------------------------------------------------------------------
//  Constellation binning
// ---------------------------------------------------------------------------

/// Constellation bins: G R E C J I S plus one catch-all.  A satellite id whose
/// system letter this build does not know goes to the catch-all bin and is never
/// dropped, so the join against the census still counts it.
inline constexpr int kNetMdbConstellations = 8;

/// @return Bin index of a RINEX system letter (7 = unknown/other).
inline constexpr int netMdbConstIndex(char sys) {
    return sys == 'G' ? 0 : sys == 'R' ? 1 : sys == 'E' ? 2 : sys == 'C' ? 3
         : sys == 'J' ? 4 : sys == 'I' ? 5 : sys == 'S' ? 6 : 7;
}

/// @return The wire letter of bin @p i ('?' for the catch-all).  Never null.
inline constexpr char netMdbConstChar(int i) {
    return i == 0 ? 'G' : i == 1 ? 'R' : i == 2 ? 'E' : i == 3 ? 'C'
         : i == 4 ? 'J' : i == 5 ? 'I' : i == 6 ? 'S' : '?';
}

/// Feed site of a row.  Counted separately so that "the census swept and MDB saw
/// nothing" and "the RAIM pool was empty" are distinguishable at run end.
enum class NetMdbSource : unsigned char { Raim = 0, Census = 1 };

// ---------------------------------------------------------------------------
//  Per-epoch accumulation
// ---------------------------------------------------------------------------

/**
 * @brief One (constellation x row class) distribution for one epoch.
 *
 * @c mdb holds the finite values only.  Infinite ones are counted in @c n_inf and
 * kept out of every order statistic, because a median over a pool containing
 * @f$\infty@f$ is either meaningless or silently censored depending on the sort;
 * they are reported as a separate count instead.
 */
struct NetworkMdbBin {
    std::vector<double> mdb;        ///< finite MDB values, metres, unsorted.
    long long n_rows = 0;           ///< rows binned (finite + infinite).
    long long n_inf = 0;            ///< …of which uncheckable (r <= 1e-6 ⇒ MDB = ∞).
    long long n_over = 0;           ///< …of which finite and above network.mdb_event_m.
    double    worst = 0.0;          ///< largest finite MDB in the bin (metres).
    double    worst_r = 0.0;        ///< its local redundancy.
    double    worst_sigma = 0.0;    ///< its a-priori σ (metres).
    std::string worst_station;      ///< its station.
    std::string worst_sat;          ///< its satellite ("" when unlabelled).

    /// Clear the counters and the value pool, keeping the pool's capacity (the
    /// bin is refilled every census sweep; releasing the buffer would make the
    /// feed allocate tens of thousands of doubles per sweep).
    void reset();
};

/// One event candidate: the worst finite row of one (station x class) block.
struct NetworkMdbCandidate {
    std::string station;
    std::string sat;
    int    cls = 0;            ///< NetRowClass as an int (data classes only).
    double mdb = 0.0;          ///< metres.
    double r = 0.0;            ///< local redundancy of that row.
    double sigma = 0.0;        ///< its a-priori σ (metres).
};

/**
 * @brief The epoch's MDB accumulation, shared by both feed sites.
 *
 * Lives in @c NetworkRunContext::carry.  The driver does not clear it; it is
 * stamped instead.  The RAIM feed runs in SB_RAIM and the census feed in
 * SB_FRAMEDIAG, and a feed whose @p tow differs from the stamp resets first.  The
 * accumulator therefore stays correct on an epoch that fed rows and never reached
 * the flush (a withhold, a solve failure), where a driver-side clear would carry
 * the previous epoch's rows into the next one's aggregates.
 */
struct NetworkMdbEpoch {
    double tow = -1.0;               ///< stamp of the epoch currently accumulating.
    bool   fed_raim = false;         ///< the RAIM pool contributed this epoch.
    bool   fed_census = false;       ///< the census sweep contributed this epoch.
    NetworkMdbBin bin[kNetMdbConstellations][kNetDataClasses];
    std::vector<NetworkMdbCandidate> cand;   ///< per (station x class) worst finite rows.
    long long rows_raim = 0;         ///< rows binned from the RAIM pool this epoch.
    long long rows_census = 0;       ///< rows binned from the census sweep this epoch.
    // --- the census join (see the threshold note in the file header) -------
    long long census_rows = 0;       ///< data rows fed from the census sweep.
    long long census_unchk = 0;      ///< …with r < network.redundancy_eps (must equal the census's own count).
    long long skipped_weight = 0;    ///< rows dropped for a non-positive / non-finite weight.

    /// Reset every bin, the candidate list and the join counters, and re-stamp.
    void reset(double new_tow);
};

/**
 * @brief Run-total MDB telemetry (standing invariant #4).
 *
 * "Armed, ran, all rows checkable" must be distinguishable from "armed, never
 * ran".  The denominators (passes, epochs, rows, and rows per source) therefore
 * come before any finding, and the run-end line states which of the two a quiet
 * run is.
 */
struct NetworkMdbTallies {
    long long passes = 0;            ///< flush invocations (the "did the wiring execute" denominator).
    long long epochs = 0;            ///< …that carried at least one row.
    long long rows = 0;              ///< rows binned over the run, both sources.
    long long rows_raim = 0;         ///< …fed by the RAIM pool.
    long long rows_census = 0;       ///< …fed by the census sweep.
    long long rows_inf = 0;          ///< …uncheckable (MDB = ∞).
    long long rows_over = 0;         ///< …finite and above network.mdb_event_m.
    long long rows_skipped = 0;      ///< rows dropped for an unusable weight (counted, never silent).
    long long events = 0;            ///< MDB events emitted (after the per-cell holdoff).
    long long events_held = 0;       ///< …suppressed by the holdoff.
    long long bins_scaled = 0;       ///< trace rows that carried a σ̂₀ empirical scale.
    long long bins_unscaled = 0;     ///< …that reported `absent` (no formal/scaled margin is measured there).
    long long bins_deflating = 0;    ///< …of the scaled ones, those with σ̂₀ < 1, whose scaled MDB is smaller than the formal one.  Reported, never clamped, and not quotable as a tightened guarantee.
    double    min_scale = 1.0;       ///< smallest σ̂₀ applied (the deepest deflation).
    bool      min_scale_seen = false;///< has @ref min_scale ever been written?
    std::string min_scale_id;        ///< its "const/class @tow".
    double    max_mdb = 0.0;         ///< largest finite MDB over the run (metres).
    std::string max_mdb_id;          ///< its "STATION/SAT/class @tow r=… σ=…".
    double    min_mdb = 0.0;         ///< smallest finite MDB (the best-checked row in the network).
    bool      min_seen = false;      ///< has @ref min_mdb ever been written?
    double    max_scale = 0.0;       ///< largest σ̂₀ applied (the measured formal/scaled margin).
    long long join_checks = 0;       ///< epochs on which the census join could be evaluated.
    long long join_mismatch = 0;     ///< …on which the two instruments disagreed.
    std::string join_first_mismatch; ///< the first disagreement, verbatim.
    double    wall_ms = 0.0;         ///< run-total wall time inside the flush (the per-row cost is inside the host loops).
};

// ---------------------------------------------------------------------------
//  The hot-loop feeder
// ---------------------------------------------------------------------------

/**
 * @brief Per-station MDB feed: constructed once per station inside a host loop
 *        that already holds @f$r_i@f$, fed once per row, finished once.
 *
 * A small value type with an inline @ref add: the census feed site walks tens of
 * thousands of rows per sweep, so a per-row map lookup or string build would
 * exceed the §3 cost budget on its own.  The per-row work is a few branches, one
 * division and one @c push_back; the only string work is the rare assignment when
 * a row becomes the new worst of its class.
 */
class NetworkMdbStationFeed {
public:
    /**
     * @param acc         The epoch accumulator (re-stamped here if @p tow is new).
     * @param tow         This epoch's TOW, the accumulator's stamp.
     * @param delta_0     @c baardaDelta0 of the run's data-snooping critical value.
     * @param event_m     `network.mdb_event_m`, the reporting threshold, metres.
     * @param station     Station name (borrowed; must outlive the feed).
     * @param src         Which feed site this is.
     * @param redun_eps   `network.redundancy_eps`, for the census join recount
     *                    (ignored unless @p src is @c NetMdbSource::Census).
     */
    NetworkMdbStationFeed(NetworkMdbEpoch& acc, double tow, double delta_0,
                          double event_m, const std::string& station,
                          NetMdbSource src, double redun_eps)
        : acc_(acc), delta_0_(delta_0), event_m_(event_m), station_(station),
          src_(src), redun_eps_(redun_eps) {
        if (acc_.tow != tow) acc_.reset(tow);
        if (src == NetMdbSource::Raim) acc_.fed_raim = true;
        else                           acc_.fed_census = true;
    }

    /**
     * @brief Feed one row.
     *
     * @param sat  Satellite id of the row ("" when the row carries no labelled
     *             satellite column; binned under the catch-all constellation).
     * @param cls  Row class; a non-data class is ignored (the MDB of a pivot pin
     *             says nothing about data).
     * @param w    The row's information weight, @f$1/\sigma^2@f$.
     * @param r    Its local redundancy, in [0,1].
     */
    void add(const std::string& sat, NetRowClass cls, double w, double r) {
        const int ci = dataClassIndex(cls);
        if (ci < 0) return;                       // pins / coupling rows: not data
        if (src_ == NetMdbSource::Census) {
            ++acc_.census_rows;                   // the join recount comes before
            if (r < redun_eps_) ++acc_.census_unchk;   // any usability screen, so
        }                                         // both instruments see one domain
        const double sigma = baardaSigmaFromWeight(w);
        if (!std::isfinite(sigma) || !std::isfinite(r)) {
            ++acc_.skipped_weight;
            return;
        }
        const int si = sat.empty() ? (kNetMdbConstellations - 1)
                                   : netMdbConstIndex(sat[0]);
        NetworkMdbBin& b = acc_.bin[si][ci];
        ++b.n_rows;
        if (src_ == NetMdbSource::Raim) ++acc_.rows_raim; else ++acc_.rows_census;
        const double m = baardaMinimalDetectableBias(sigma, r, delta_0_);
        if (!std::isfinite(m)) { ++b.n_inf; return; }
        b.mdb.push_back(m);
        if (m > event_m_) ++b.n_over;
        if (m > b.worst) {
            b.worst = m; b.worst_r = r; b.worst_sigma = sigma;
            b.worst_station = station_; b.worst_sat = sat;
        }
        Worst& cw = worst_[static_cast<std::size_t>(ci)];
        if (m > cw.mdb) { cw.mdb = m; cw.r = r; cw.sigma = sigma; cw.sat = sat; }
    }

    /**
     * @brief Commit this station's per-class worst finite rows as event
     *        candidates.
     *
     * The event stream carries the worst row per (station x class), not every row.
     * The spec's domain is "finite rows above `mdb_event_m`", and on this data most
     * code rows qualify, so a per-row stream would be tens of thousands of lines
     * per epoch, a second copy of the trace rather than a reporting channel.  The
     * full population still reaches the reader: @c n_over counts every row above
     * the threshold in its bin, and the bin's `worst_*` columns name the
     * network-wide worst row exactly.
     */
    void finish();

private:
    struct Worst { double mdb = 0.0, r = 0.0, sigma = 0.0; std::string sat; };

    NetworkMdbEpoch& acc_;
    double delta_0_;
    double event_m_;
    const std::string& station_;
    NetMdbSource src_;
    double redun_eps_;
    Worst worst_[kNetDataClasses];
};

// ---------------------------------------------------------------------------
//  Seams
// ---------------------------------------------------------------------------

struct NetworkRun;              // network/driver/Run.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // network/driver/Epoch.hpp (card NOM3 step EP1)

/**
 * @brief Aggregate this epoch's MDB rows, write the trace, emit the reporting
 *        events, reconcile against the redundancy census, and reset (INC-1).
 *
 * Call from SB_FRAMEDIAG after @c networkPostfitSigma0 (whose per-family
 * @f$\hat\sigma_0@f$ this reads for the scaled variant) and after
 * @c networkRedundancyCensus (whose per-epoch row counts the join compares
 * against).  Called earlier, the scale would be the previous epoch's and the join
 * would compare against a stale sweep.
 *
 * The scaled aggregates are computed as @f$\hat\sigma_{0,f}\times@f$ the formal
 * order statistic.  This is exact: @f$\hat\sigma_{0,f}@f$ is a positive per-family
 * constant, and a positive scaling commutes with the median and with any
 * quantile.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: Run context: the epoch accumulator, the holdoff memory and every
 *     counter live here.
 *   - @p config_ = run.config: Live config (reads @c network.mdb_* and
 *     @c network.redundancy_eps).
 *   - @p log = run.log: Main run logger (one-shot join-mismatch warning).
 *   - @p traces = run.traces: Owns @c network_mdb.csv and the @c network_events.csv sink.
 *   - @p se = ep.se: This epoch (week/tow stamp).
 */
void networkMdbFlush(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Run-end MDB telemetry (standing invariant #4).
 *
 * The body lives next to the aggregation that produces the numbers rather than in
 * the teardown TU, so the numbers and the text that interprets them stay together.
 * Called by its row of the run-end table (the overload taking the view, below) under
 * @c ctx.knobs.mdb_on.
 */
void networkMdbRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                      const std::shared_ptr<spdlog::logger>& log);

// ---------------------------------------------------------------------------
//  Testable pieces (unit-test seam; no run state, no I/O)
// ---------------------------------------------------------------------------

/// Order statistics of one finite MDB pool.  All three are 0 for an empty pool;
/// the caller reports the pool's population alongside, so an empty bin cannot be
/// read as a bin of zeros.
struct NetworkMdbStats { double med = 0.0, p95 = 0.0, max = 0.0; };

/// The empirical inflation available for one data class and, when it is not
/// available, which of the four reasons applies.  @c have == false means the
/// scaled columns are blank; @c src is the reason string written to the trace.
struct NetworkMdbScale {
    bool   have = false;
    double sigma0 = 1.0;
    const char* src = "absent";
};

/**
 * @brief Resolve @f$\hat\sigma_{0,f}@f$ for a data class from the per-epoch scalars
 *        of the P1-4 post-fit pass.
 *
 * Four reasons for an absent scale, each written distinctly rather than as a
 * single blank:
 *  - `absent_no_pf`: the P1-4 pass is not armed (`network.trace_postfit_sigma0`);
 *  - `absent_no_pool_for_class`: @c obs_other / @c iono_sh_row have no pool by
 *    construction (an SH tie row's misfit is a known artifact: the single-layer
 *    field leaves 70 to 90 % of them above 3σ, and pooling it would dominate the
 *    statistic);
 *  - `absent_pool_empty`: the class's pool saw no rows this epoch;
 *  - `absent_sigma0_zero`: @f$\hat\sigma_0@f$ is zero (or not finite).  Zero is the
 *    normal state of the phase pool under free ambiguities, since each phase row
 *    is fitted exactly by its own ambiguity.  Scaling by it would publish every
 *    phase MDB as 0 m ("a fault of any size is detectable here"), the reverse of
 *    the truth, since those rows carry no redundancy.  A zero is therefore treated
 *    as absent, never as a scale.
 *
 * A scale that is small but nonzero has the same problem to a lesser degree.  It
 * returns @c have == true (the deflation is the measurement and is reported) with
 * @c src tagged `postfit_code_deflating` / `postfit_phase_deflating`.
 *
 * @param cls           Data class index (@c NetRowClass as an int).
 * @param pf_rows       @c ctx.carry.ep_pf_rows (0 ⇒ the pass did not run).
 * @param pf_code_rows  @c ctx.carry.ep_pf_code_rows.
 * @param pf_phase_rows @c ctx.carry.ep_pf_phase_rows.
 * @param s0_code       @c ctx.carry.ep_sigma0_pf_code.
 * @param s0_phase      @c ctx.carry.ep_sigma0_pf_phase.
 */
NetworkMdbScale networkMdbFamilyScale(int cls, long long pf_rows,
                                      long long pf_code_rows, long long pf_phase_rows,
                                      double s0_code, double s0_phase);

/**
 * @brief Median / 95th percentile / maximum of a finite MDB pool.
 *
 * @param v  The pool (reordered in place by @c nth_element; not sorted).
 * @return The three order statistics.  The median is the upper median for even
 *         n.  The p95 uses the nearest-rank convention @f$\lceil 0.95 n\rceil@f$,
 *         so a single-element pool reports that element rather than
 *         interpolating.
 */
NetworkMdbStats networkMdbStats(std::vector<double>& v);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: Baarda MDB reliability.
 *
 * The table's row for the Baarda MDB reliability aggregates; the report is the overload
 * above.
 *
 * Gate: prints under `ctx.knobs.mdb_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkMdbRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
