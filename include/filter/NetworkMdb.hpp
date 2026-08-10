#pragma once
/**
 * @file NetworkMdb.hpp
 * @brief Baarda reliability numbers (MDB) on the network path —
 *        docs/SPEC_network_mdb_reliability.md.
 *
 * Measure-only, default-OFF, OFF byte-identical, fingerprint-SAFE (a forward-only
 * readout that changes no stored-state meaning — the adaptive_iono_q /
 * rhs_attrib / redundancy-census precedent), so a shared-snapshot replay leg with
 * only this armed stays A/B-legal against an unarmed control.  Nothing here
 * writes a weight, a state or a decision: §9 of the spec pre-declares that ANY
 * gate, deweight or admission wired to MDB belongs to a different card.
 *
 * ## Why the number
 *
 * The redundancy census counts UNCHECKABLE rows (@f$r_i < \varepsilon@f$) and
 * RAIM standardizes residuals, but nothing on the network path answered the
 * service question in FAULT UNITS: how large a fault could hide in this row
 * right now?  Baarda's minimal detectable bias is that number, per row, in
 * metres —
 *
 *   @f[ \mathrm{MDB}_i = \frac{\sigma_i\,\delta_0}{\sqrt{r_i}},
 *       \qquad \delta_0 = k + \Phi^{-1}(0.80), @f]
 *
 * and it costs one division wherever @f$r_i@f$ already exists.  This card is a
 * PORT, not a build: @c KalmanFilter::computeResidualQc_ has computed exactly
 * this quartet on the PPP path since the data-snooping work, and the DIA catalog
 * recorded the network side as unported.  Both paths now call
 * @c filter/BaardaReliability.hpp, so "one convention, two paths" is a
 * structural property rather than a claim.
 *
 * ## The two honesty rules (both inherited from measured history)
 *
 *  1. **Formal MDB inherits the formal-σ optimism.**  D5 measured the formal
 *     @f$\sigma@f$ 10–30x optimistic on this data; MDB scales LINEARLY with
 *     @f$\sigma@f$, so a formal MDB is optimistic by the same factor.  Every
 *     trace row therefore carries BOTH `mdb_formal` and
 *     @f$\texttt{mdb\_scaled} = \texttt{mdb\_formal}\times\hat\sigma_{0,f}@f$,
 *     the per-family empirical inflation measured by the P1-4 post-fit
 *     @f$\hat\sigma_0@f$ pass (@c network.trace_postfit_sigma0).  When that pass
 *     is not armed — or its pool for this family is empty, which is the NORMAL
 *     state of the PHASE pool under free ambiguities — the scaled columns are
 *     BLANK and `sigma0_src` reads `absent`.  They are never silently filled
 *     with the formal value, and never with 1.0: quoting `mdb_formal` alone to a
 *     consumer is repeating D5 v1, and a scaled column that secretly equals the
 *     formal one would make that indistinguishable from a calibrated answer.
 *     The MIRROR-IMAGE trap has its own tag: a @f$\hat\sigma_{0,f} < 1@f$ makes
 *     the scaled MDB SMALLER than the formal one, which reads as a TIGHTER
 *     guarantee — and on the phase family it is the exact opposite, because that
 *     pool's post-fit dispersion is near zero precisely BECAUSE each phase row
 *     is fitted exactly by its own free ambiguity, which is the same fact as
 *     "these rows carry no redundancy".  Measured on the FT63 cold smoke:
 *     @f$\hat\sigma_{0,\text{phase}} = 0.041@f$ pulls the phase median MDB from
 *     4.58 m to 0.19 m on rows that are 78 % UNCHECKABLE.  Such bins are
 *     REPORTED, never clamped (the prior-residual census's rule — the deflation
 *     IS the measurement) and tagged `*_deflating`, so a reader keying on
 *     `sigma0_src` cannot mistake one for a calibrated tightening.
 *  2. **MDB is a property of the GEOMETRY, not a test.**  Its value is making
 *     the census's "uncheckable" binary QUANTITATIVE and the RAIM thresholds
 *     interpretable.  The `mdb_event_m` threshold is a REPORTING threshold; no
 *     decision reads it.
 *
 * ## Where the rows come from
 *
 * Two feed sites, both loops that already hold @f$r_i@f$ — no new sweep, no new
 * covariance product:
 *
 *  - the **RAIM pool** (`network.raim_redundancy_normalize`, B2) — carried-
 *    ambiguity PHASE rows, every warm epoch, @c NetworkRaim.cpp;
 *  - the **redundancy-census sweep** (`network.redundancy_census_stride` /
 *    `_escalate_sigma`) — every DATA row of a swept station, at the census's own
 *    stride and escalation, @c NetworkRedundancyCensus.cpp.
 *
 * A run may arm either or both; the loader HARD-ERRORS when neither is armed,
 * because there is then no @f$r_i@f$ to divide by and the card would report a
 * clean zero-row run that reads exactly like a genuine negative result.
 *
 * ## The uncheckable JOIN, and the threshold that is NOT the same threshold
 *
 * Spec §INC-1 asks that the rows at @f$\infty@f$ reconcile against the census's
 * `redun_uncheckable` population — one number, two instruments.  They are
 * governed by DIFFERENT thresholds and that distinction is load-bearing:
 *
 *  - MDB is @f$\infty@f$ at @f$r_i \le@f$ @ref kBaardaRedundancyFloor (1e-6) —
 *    the arithmetic floor, where the division itself stops meaning anything;
 *  - the census calls a row UNCHECKABLE at @f$r_i <@f$ `network.redundancy_eps`
 *    (0.05 by default) — a POLICY floor, four decades looser.
 *
 * So @f$n_\infty \subseteq n_\text{uncheckable}@f$ and an equality assertion
 * between them would be wrong.  What this seam actually reconciles is the pair
 * of numbers that MUST agree exactly: over the census-fed rows it independently
 * recounts the DATA rows and the rows below `redundancy_eps`, and compares them
 * against the census's own `ep_redun_rows` / `ep_uncheckable_rows` for the same
 * epoch.  A mismatch there is a bug in one of the two instruments and is
 * reported as such; the @f$\infty@f$ count is reported separately as the strict
 * subset it is.
 *
 * ## Cost
 *
 * One division, one compare and a @c push_back per row in loops that already
 * run, plus one @c nth_element per non-empty (constellation x class) bin per
 * epoch.  The RAIM pool is small (carried-ambiguity phase rows only); the census
 * sweep is large but STRIDED, and its own cost is 100–300 ms/epoch at 63
 * stations, so the rider is amortized well under the §3 budget.  @ref
 * NetworkMdbTallies::wall_ms times the FLUSH only — the per-row cost rides the
 * host loops and is not separable (the prior-residual census records the same
 * limitation for the same reason).
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
#include "filter/NetworkRowClassify.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

// ---------------------------------------------------------------------------
//  Constellation binning
// ---------------------------------------------------------------------------

/// Constellation bins: G R E C J I S plus one catch-all.  A satellite id whose
/// system letter this build does not know is BINNED, never dropped — an
/// unnamed row still counts, and a silently discarded one would make the join
/// against the census fail for a reason nobody could see.
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

/// Where a row was fed from — reported separately so "the census swept and MDB
/// saw nothing" and "the RAIM pool was empty" are distinguishable at run end.
enum class NetMdbSource : unsigned char { Raim = 0, Census = 1 };

// ---------------------------------------------------------------------------
//  Per-epoch accumulation
// ---------------------------------------------------------------------------

/**
 * @brief One (constellation x row class) distribution for one epoch.
 *
 * @c mdb holds the FINITE values only; the infinite ones are counted in
 * @c n_inf and deliberately kept out of every order statistic, because a median
 * over a pool containing @f$\infty@f$ is either meaningless or silently
 * censored depending on the sort. Reporting them as a separate count is the
 * only honest option.
 */
struct NetworkMdbBin {
    std::vector<double> mdb;        ///< finite MDB values, metres, unsorted.
    long long n_rows = 0;           ///< rows binned (finite + infinite).
    long long n_inf = 0;            ///< …of which UNCHECKABLE (r <= 1e-6 ⇒ MDB = ∞).
    long long n_over = 0;           ///< …of which FINITE and above network.mdb_event_m.
    double    worst = 0.0;          ///< largest FINITE MDB in the bin (metres).
    double    worst_r = 0.0;        ///< its local redundancy.
    double    worst_sigma = 0.0;    ///< its a-priori σ (metres).
    std::string worst_station;      ///< its station.
    std::string worst_sat;          ///< its satellite ("" when unlabelled).

    /// Clear the counters and the value pool, KEEPING the pool's capacity (the
    /// bin is refilled every census sweep; releasing the buffer would make the
    /// rider allocate tens of thousands of doubles per sweep).
    void reset();
};

/// One event candidate — the worst FINITE row of one (station x class) block.
struct NetworkMdbCandidate {
    std::string station;
    std::string sat;
    int    cls = 0;            ///< NetRowClass as an int (DATA classes only).
    double mdb = 0.0;          ///< metres.
    double r = 0.0;            ///< local redundancy of that row.
    double sigma = 0.0;        ///< its a-priori σ (metres).
};

/**
 * @brief The epoch's MDB accumulation, shared by both feed sites.
 *
 * Lives in @c NetworkRunContext::carry.  It is STAMPED rather than cleared by
 * the driver: the RAIM feed happens in SB_RAIM and the census feed in
 * SB_FRAMEDIAG, so a feed whose @p tow differs from the stamp resets first.
 * That makes the accumulator correct even on an epoch that fed rows and never
 * reached the flush (a withhold, a solve failure), where a driver-side clear
 * would have leaked the previous epoch's rows into the next one's aggregates.
 */
struct NetworkMdbEpoch {
    double tow = -1.0;               ///< stamp of the epoch currently accumulating.
    bool   fed_raim = false;         ///< the RAIM pool contributed this epoch.
    bool   fed_census = false;       ///< the census sweep contributed this epoch.
    NetworkMdbBin bin[kNetMdbConstellations][kNetDataClasses];
    std::vector<NetworkMdbCandidate> cand;   ///< per (station x class) worst finite rows.
    long long rows_raim = 0;         ///< rows binned from the RAIM pool this epoch.
    long long rows_census = 0;       ///< rows binned from the census sweep this epoch.
    // --- the census join (see the header's threshold note) ------------------
    long long census_rows = 0;       ///< DATA rows fed FROM the census sweep.
    long long census_unchk = 0;      ///< …with r < network.redundancy_eps (must equal the census's own count).
    long long skipped_weight = 0;    ///< rows dropped for a non-positive / non-finite weight.

    /// Reset every bin, the candidate list and the join counters, and re-stamp.
    void reset(double new_tow);
};

/**
 * @brief Run-total MDB telemetry (standing invariant #4).
 *
 * The failure this card must be able to DISPROVE is "armed, ran, all rows
 * checkable" being reported identically to "armed, never ran".  Hence the
 * denominators — passes, epochs, rows, and rows PER SOURCE — come before any
 * finding, and the run-end line names which of the two states a quiet run is in.
 */
struct NetworkMdbTallies {
    long long passes = 0;            ///< flush invocations (the "did the wiring execute" denominator).
    long long epochs = 0;            ///< …that carried at least one row.
    long long rows = 0;              ///< rows binned over the run, both sources.
    long long rows_raim = 0;         ///< …fed by the RAIM pool.
    long long rows_census = 0;       ///< …fed by the census sweep.
    long long rows_inf = 0;          ///< …UNCHECKABLE (MDB = ∞).
    long long rows_over = 0;         ///< …FINITE and above network.mdb_event_m.
    long long rows_skipped = 0;      ///< rows dropped for an unusable weight (counted, never silent).
    long long events = 0;            ///< MDB events emitted (post per-cell holdoff).
    long long events_held = 0;       ///< …suppressed by the holdoff.
    long long bins_scaled = 0;       ///< trace rows that carried a σ̂₀ empirical scale.
    long long bins_unscaled = 0;     ///< …that reported `absent` (the honesty margin is unmeasurable there).
    long long bins_deflating = 0;    ///< …of the scaled ones, those whose σ̂₀ < 1, i.e. whose SCALED MDB is SMALLER than the formal one.  Reported, never clamped — but never quotable as a tightened guarantee either.
    double    min_scale = 1.0;       ///< smallest σ̂₀ applied — the deepest deflation.
    bool      min_scale_seen = false;///< has @ref min_scale ever been written?
    std::string min_scale_id;        ///< its "const/class @tow".
    double    max_mdb = 0.0;         ///< largest FINITE MDB over the run (metres).
    std::string max_mdb_id;          ///< its "STATION/SAT/class @tow r=… σ=…".
    double    min_mdb = 0.0;         ///< smallest FINITE MDB — the best-checked row in the network.
    bool      min_seen = false;      ///< has @ref min_mdb ever been written?
    double    max_scale = 0.0;       ///< largest σ̂₀ applied — the measured formal/scaled honesty margin.
    long long join_checks = 0;       ///< epochs on which the census join could be evaluated.
    long long join_mismatch = 0;     ///< …on which the two instruments disagreed.
    std::string join_first_mismatch; ///< the first disagreement, verbatim.
    double    wall_ms = 0.0;         ///< run-total wall inside the FLUSH (the per-row cost rides the host loops).
};

// ---------------------------------------------------------------------------
//  The hot-loop feeder
// ---------------------------------------------------------------------------

/**
 * @brief Per-station MDB feed — constructed once per station inside a host loop
 *        that already holds @f$r_i@f$, fed once per row, finished once.
 *
 * Deliberately a tiny value type with an inline @ref add: the census feed site
 * walks tens of thousands of rows per sweep, so a per-row map lookup or string
 * build would blow the §3 cost budget on its own.  Everything per row is a
 * couple of branches, one division and one @c push_back; the only string work
 * is the rare "this row is the new worst of its class" assignment.
 */
class NetworkMdbStationFeed {
public:
    /**
     * @param acc         The epoch accumulator (re-stamped here if @p tow is new).
     * @param tow         This epoch's TOW — the accumulator's stamp.
     * @param delta_0     @c baardaDelta0 of the run's data-snooping critical value.
     * @param event_m     `network.mdb_event_m` — the REPORTING threshold, metres.
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
     *             satellite column — binned under the catch-all constellation).
     * @param cls  Row class; a non-DATA class is ignored (a pivot pin's MDB is
     *             not a statement about data).
     * @param w    The row's information weight, @f$1/\sigma^2@f$.
     * @param r    Its local redundancy, in [0,1].
     */
    void add(const std::string& sat, NetRowClass cls, double w, double r) {
        const int ci = dataClassIndex(cls);
        if (ci < 0) return;                       // pins / coupling rows: not data
        if (src_ == NetMdbSource::Census) {
            ++acc_.census_rows;                   // the join recount — BEFORE any
            if (r < redun_eps_) ++acc_.census_unchk;   // usability screen, so the
        }                                         // two instruments see one domain
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
     * @brief Commit this station's per-class worst FINITE rows as event
     *        candidates.
     *
     * The event stream is per (station x class) worst row rather than per row.
     * The spec's domain is "finite rows above `mdb_event_m`", and on this data
     * that is MOST code rows — a genuinely per-row stream would be tens of
     * thousands of lines per epoch, which is not a reporting channel, it is a
     * second copy of the trace.  The full population still reaches the reader:
     * @c n_over counts every row above the threshold in its bin, and the bin's
     * own `worst_*` columns name the network-wide worst exactly.
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

/**
 * @brief INC-1: aggregate this epoch's MDB rows, write the trace, emit the
 *        reporting events, reconcile against the redundancy census, reset.
 *
 * Call from SB_FRAMEDIAG AFTER @c networkPostfitSigma0 (whose per-family
 * @f$\hat\sigma_0@f$ this reads for the scaled variant) and AFTER
 * @c networkRedundancyCensus (whose per-epoch row counts the join compares
 * against).  Both orderings are load-bearing: read earlier and the scale would
 * silently be the PREVIOUS epoch's, and the join would compare against a stale
 * sweep.
 *
 * The scaled aggregates are computed as @f$\hat\sigma_{0,f}\times@f$ the FORMAL
 * order statistic, which is exact — @f$\hat\sigma_{0,f}@f$ is a positive
 * per-family constant, and a positive scaling commutes with the median and with
 * any quantile.
 *
 * @param ctx      Run context: the epoch accumulator, the holdoff memory and
 *                 every counter live here.
 * @param config_  Live config (reads @c network.mdb_* and
 *                 @c network.redundancy_eps).
 * @param log      Main run logger (one-shot join-mismatch warning).
 * @param traces   Owns @c network_mdb.csv and the @c network_events.csv sink.
 * @param se       This epoch (week/tow stamp).
 */
void networkMdbFlush(NetworkRunContext& ctx, const GnssConfig& config_,
                     const std::shared_ptr<spdlog::logger>& log,
                     NetworkRunTraces& traces, const SyncedEpoch& se);

/**
 * @brief Run-end MDB telemetry (standing invariant #4).
 *
 * Body lives next to the aggregation that produced it rather than in the
 * teardown TU, so the numbers and the sentences that interpret them cannot
 * drift apart.  Called from @c networkTeardown under @c ctx.knobs.mdb_on.
 */
void networkMdbReport(NetworkRunContext& ctx, const GnssConfig& config_,
                      const std::shared_ptr<spdlog::logger>& log);

// ---------------------------------------------------------------------------
//  Testable pieces (unit-test seam; no run state, no I/O)
// ---------------------------------------------------------------------------

/// Order statistics of one finite MDB pool.  All three are 0 for an empty pool
/// — the caller reports the pool's population alongside, so an empty bin can
/// never be read as a bin of zeros.
struct NetworkMdbStats { double med = 0.0, p95 = 0.0, max = 0.0; };

/// The empirical inflation available for one DATA class, and — when it is NOT
/// available — WHICH of the four reasons applies.  @c have == false means the
/// scaled columns are BLANK; @c src is the reason string that goes on the wire.
struct NetworkMdbScale {
    bool   have = false;
    double sigma0 = 1.0;
    const char* src = "absent";
};

/**
 * @brief Resolve @f$\hat\sigma_{0,f}@f$ for a DATA class from the P1-4 post-fit
 *        pass's per-epoch scalars.
 *
 * Four ways this legitimately comes back ABSENT, all distinguished on the wire
 * rather than collapsed into a single blank:
 *  - `absent_no_pf` — the P1-4 pass is not armed (`network.trace_postfit_sigma0`);
 *  - `absent_no_pool_for_class` — @c obs_other / @c iono_sh_row have no pool
 *    there by construction (an SH tie row's misfit is a known artifact, see
 *    NetworkIonoDeweightGate.hpp, and pooling it would swamp the statistic);
 *  - `absent_pool_empty` — the class's pool saw no rows this epoch;
 *  - `absent_sigma0_zero` — @f$\hat\sigma_0 = 0@f$, which is the NORMAL state of
 *    the PHASE pool under free ambiguities: each phase row is fitted exactly by
 *    its own ambiguity.  Scaling by that zero would publish every phase MDB as
 *    0 m — "a fault of any size is detectable here" — the exact inversion of the
 *    truth, since those rows carry no redundancy at all.  That is why a zero is
 *    treated as ABSENT and never as a scale.
 *
 * A scale that is merely SMALL rather than zero is the same trap one step back,
 * so it is @c have == true (the deflation is the measurement and is reported)
 * with @c src tagged `postfit_code_deflating` / `postfit_phase_deflating`.
 *
 * @param cls           DATA class index (@c NetRowClass as an int).
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
 * @param v  The pool (REORDERED in place — @c nth_element, not a sort).
 * @return The three order statistics.  The p95 uses the nearest-rank
 *         convention @f$\lceil 0.95 n\rceil@f$, so a single-element pool
 *         reports that element rather than interpolating against nothing.
 */
NetworkMdbStats networkMdbStats(std::vector<double>& v);

}  // namespace GnssProcessor
