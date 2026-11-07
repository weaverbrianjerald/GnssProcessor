#pragma once
/**
 * @file Vcf.hpp
 * @brief The per-class variance factor of the network producer's observation rows.
 *
 * Every code row is weighted at the a priori code sigma and every phase row at the a priori
 * phase sigma (filter.measurement_noise, the elevation law), and nothing in the producer
 * estimates how far the data depart from that model.  This file measures it, per class of
 * observation row, from the post-fit residuals and their local redundancies, and keeps an
 * exponentially weighted average per class.
 *
 * ## The statistics
 *
 * For a row i with post-fit residual @f$v_i@f$ (observed minus predicted), information weight
 * @f$w_i = 1/\sigma_i^2@f$ and local redundancy @f$r_i = 1 - w_i a_i^\top \mathrm{Cov}\,a_i@f$
 * (the redundancy census's kernel, with the carried priors and the datum drawn), per class
 * @f$c@f$ and swept epoch:
 *
 *   @f[ f_c = \frac{\sum_{i\in c} w_i v_i^2}{\sum_{i\in c} r_i}, \qquad
 *       g_c = \Big(1.4826\,\mathrm{med}_{i\in c}\,|v_i|\sqrt{w_i/r_i}\Big)^2 . @f]
 *
 * @f$f_c@f$ is the redundancy-weighted variance factor: for a correctly weighted model
 * @f$E[v^\top W v] = \sigma_0^2 \sum_i r_i@f$, exactly when every class shares one factor
 * (per class it is the simplified variance-component estimator, the first step of LS-VCE
 * without the cross-class trace terms).  It is a second moment and inherits every tail.
 * @f$g_c@f$ is its robust companion: @f$|v_i|\sqrt{w_i/r_i}@f$ is the w-test statistic in a
 * priori units, of unit variance under a calibrated model, so @f$g_c@f$ estimates the same
 * factor under Gaussian noise and one station's gross channel does not move it.  A factor is a
 * variance ratio; its square root is the scale (a sigma ratio).
 *
 * ## Classes
 *
 * Code and phase observation rows by constellation and signal ("G C1C", "E L5Q", ...), the
 * aggregates "code" and "phase" over every constellation, and two counted classes that sit
 * outside the nominal model: "code_companion" (the second code channel's companion and fallback
 * rows, weighted at sigma times network.second_code_sigma_scale) and "phase_second" (a second
 * phase channel).  Rows on a line of sight carrying a deweight this epoch (the unvetted-LOS phase
 * scale, the gross-LOS code scale) are counted and left out of every pool.
 *
 * ## Identifiability
 *
 * A class is identifiable when its redundancy per row is at least @ref kVcfMinRedundancyPerRow
 * and it pooled at least @ref kVcfMinClassRows rows.  That is necessary, not sufficient, on
 * phase: with a free slant ionosphere per line of sight the geometry-free direction of a phase
 * pair has no redundancy, and the coloured part of the phase noise is absorbed by the carried
 * ambiguities, so a phase factor is a measurement only where a bed with a known phase law
 * confirms it.
 *
 * ## Host and cost
 *
 * The rows are fed by the redundancy-census sweep (network.redundancy_census_stride), which
 * already holds every swept row's redundancy and, with the block census armed, its residual:
 * no second sweep and no second covariance product.  The per-row work is a few branches, two
 * map lookups and two additions.  The flush (@ref networkVcfFlush) runs once per epoch after the
 * census.
 *
 * ## Stage 2: the applying scales
 *
 * With network.vcf_apply the robust aggregate code EWMA becomes the URA's scale, and with
 * network.vcf_apply_phase as well the robust aggregate phase EWMA divides each statistic of the
 * within-epoch and the post-fit RAIM pools (@ref networkVcfApplyScale).  A scale is 1 (the a
 * priori model) until its class is identifiable and has @ref kVcfApplyMinSweeps sweeps.  The
 * solve's weights and the carried variances are never touched.
 */

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "network/assembly/Contributions.hpp"   // RowTag, ContributionKind

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct NetworkRunEndView;       // network/runend/RunEnd.hpp

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

/// Rows a class must pool on a sweep before its factor enters the EWMA (and before it can be
/// identifiable).  Below it the trace row is still written, flagged unidentifiable.
inline constexpr long long kVcfMinClassRows = 50;
/// Redundancy per row below which a class is flagged unidentifiable: its residuals then read
/// mostly the redundancy structure rather than the noise.
inline constexpr double kVcfMinRedundancyPerRow = 0.1;
/// Local redundancy at or below which a row is uncheckable: it enters neither the robust pool
/// nor (being fully absorbed, with v = 0) moves the weighted sums.
inline constexpr double kVcfRedundancyFloor = 1e-6;
/// Sweeps a class's EWMA must hold before stage 2 applies its scale.
inline constexpr long long kVcfApplyMinSweeps = 3;

// ---------------------------------------------------------------------------
//  Classes
// ---------------------------------------------------------------------------

/// The kind of an observation-row class.
enum class VcfKind : unsigned char {
    Code = 0,        ///< Code rows of the latched code signal (ContributionKind::DataCode).
    Phase,           ///< Phase rows of the latched phase signal (ContributionKind::DataPhase).
    CodeCompanion,   ///< The second code channel's companion and fallback rows (counted, outside the nominal model).
    PhaseSecond,     ///< A second phase channel's rows (counted, outside the nominal model).
};

/// @return The trace word of @p k ("code", "phase", "code_companion", "phase_second").
const char* vcfKindName(VcfKind k);

/**
 * @brief The class kind of an observation row's contribution kind.
 * @param k The row's contribution kind.
 * @param[out] out The class kind, written when the row is an observation row.
 * @return false for any row that is not an observation (pins, priors, coupling rows).
 */
bool vcfKindOf(ContributionKind k, VcfKind& out);

/**
 * @brief The key of a per-signal class.
 * @param sys    The RINEX constellation letter ('G', 'E', ...).
 * @param signal The observation code ("C1C", "L5Q").
 * @return "G C1C".
 */
std::string vcfSignalClassKey(char sys, const std::string& signal);

// ---------------------------------------------------------------------------
//  Per-epoch accumulation
// ---------------------------------------------------------------------------

/// One class's sums over one swept epoch.
struct NetworkVcfClassAcc {
    VcfKind   kind = VcfKind::Code;
    char      sys = '*';            ///< constellation letter, '*' for an aggregate.
    std::string signal;             ///< observation code, "" for an aggregate.
    long long n = 0;                ///< rows pooled (w > 0, finite residual, not deweighted).
    long long n_dw = 0;             ///< rows left out for a line-of-sight deweight this epoch.
    long long n_unchk = 0;          ///< pooled rows with r <= kVcfRedundancyFloor (in the sums, not in the robust pool).
    double    sum_wv2 = 0.0;        ///< sum of w v^2 over the pooled rows.
    double    sum_r = 0.0;          ///< sum of r over the pooled rows.
    std::vector<double> z;          ///< |v| sqrt(w / r) over the pooled rows with r > floor (unsorted).
};

/**
 * @brief The epoch's accumulation, filled by the census sweep and drained by the flush.
 *
 * Stamped rather than cleared by the driver: a feed whose TOW differs from the stamp resets it
 * first, so an epoch that fed rows and never reached the flush does not leak into the next.
 */
struct NetworkVcfEpoch {
    double tow = -1.0;              ///< stamp of the epoch currently accumulating.
    bool   fed = false;             ///< the census sweep contributed this epoch.
    std::map<std::string, NetworkVcfClassAcc> cls;   ///< per-signal classes and the aggregates, by key.
    long long rows_skipped = 0;     ///< observation rows dropped for a non-positive weight or a non-finite value.
    long long stations = 0;         ///< stations fed this epoch.

    /// Clear every class and re-stamp.
    void reset(double new_tow);
};

/// One class's run state: its EWMAs and the material of the run-end table.
struct NetworkVcfClassState {
    VcfKind   kind = VcfKind::Code;
    char      sys = '*';
    std::string signal;
    double    ewma_f = 0.0;         ///< EWMA of the redundancy-weighted factor.
    double    ewma_g = 0.0;         ///< EWMA of the robust factor.
    long long sweeps = 0;           ///< sweeps that entered the EWMA (n >= kVcfMinClassRows).
    long long sweeps_seen = 0;      ///< sweeps that pooled at least one row.
    long long rows = 0;             ///< rows pooled over the run.
    long long rows_dw = 0;          ///< rows left out for a deweight over the run.
    double    rho_last = 0.0;       ///< redundancy per row on the last sweep that entered the EWMA.
    bool      identifiable_last = false;   ///< identifiability on that sweep.
    std::map<int, std::vector<double>> hour_f;   ///< hour (1-based, from the first sweep) -> per-sweep f.
    std::map<int, std::vector<double>> hour_g;   ///< hour -> per-sweep g.
    std::map<int, double> hour_ewma_g;           ///< hour -> ewma_g after the last sweep of the hour.
    std::map<int, double> hour_ewma_f;           ///< hour -> ewma_f after the last sweep of the hour.
};

/**
 * @brief The variance-factor run state: the epoch accumulator and every class's EWMA.
 *
 * Lives in @c NetworkRunContext::carry.  Cold-start: a warm-resumed leg re-seeds its EWMAs
 * (stage 2 then applies the a priori scale until @ref kVcfApplyMinSweeps sweeps).
 */
struct NetworkVcfState {
    NetworkVcfEpoch ep;                                   ///< this epoch's accumulation.
    std::map<std::string, NetworkVcfClassState> classes;  ///< by class key.
    double first_sweep_s = -1.0;                          ///< continuous time of the first sweep (hour anchor).
};

/// Run-scope counters (a base of NetworkServiceCounters).
struct VcfCounters {
    long long vcf_passes = 0;       ///< flush invocations (the "did the wiring execute" denominator).
    long long vcf_sweeps = 0;       ///< …that carried rows (census sweeps).
    long long vcf_rows = 0;         ///< observation rows pooled over the run, every class kind.
    long long vcf_rows_dw = 0;      ///< rows left out for a line-of-sight deweight.
    long long vcf_rows_unchk = 0;   ///< pooled rows at r <= kVcfRedundancyFloor.
    long long vcf_rows_skipped = 0; ///< rows dropped for an unusable weight or value.
    long long vcf_no_resid = 0;     ///< swept stations whose residual could not be formed.
    bool vcf_we_logged = false;     ///< one-shot: the within-epoch reader logged its scale.
    bool vcf_pf_logged = false;     ///< one-shot: the post-fit reader logged its scale.
    bool vcf_ura_logged = false;    ///< one-shot: the URA reader logged its scale.
    long long vcf_we_applied = 0;   ///< within-epoch assessments that read a scale other than 1.
    long long vcf_pf_applied = 0;   ///< post-fit passes that read a scale other than 1.
    long long vcf_ura_applied = 0;  ///< URA EWMA updates fed the code scale.
};

// ---------------------------------------------------------------------------
//  The hot-loop feeder
// ---------------------------------------------------------------------------

/**
 * @brief Per-station feed: constructed once per swept station inside the census walk, fed once
 *        per row.
 */
class NetworkVcfStationFeed {
public:
    /**
     * @param acc The epoch accumulator (re-stamped here when @p tow is new).
     * @param tow This epoch's TOW, the accumulator's stamp.
     */
    NetworkVcfStationFeed(NetworkVcfEpoch& acc, double tow);

    /**
     * @brief Feed one row of the station block.
     * @param tag        The row's contribution tag (kind, satellite, signal).
     * @param w          Its information weight 1/sigma^2.
     * @param v          Its post-fit residual, metres.
     * @param r          Its local redundancy in [0, 1].
     * @param deweighted The row's line of sight carries a deweight this epoch.
     */
    void add(const RowTag& tag, double w, double v, double r, bool deweighted);

private:
    NetworkVcfEpoch& acc_;
};

// ---------------------------------------------------------------------------
//  Testable pieces (no run state, no I/O)
// ---------------------------------------------------------------------------

/// The statistics of one class on one sweep.
struct NetworkVcfFactors {
    double f = 0.0;            ///< sum w v^2 / sum r (0 when sum r is 0).
    double g = 0.0;            ///< (1.4826 median z)^2 (0 for an empty robust pool).
    double rho = 0.0;          ///< sum r / n.
    bool   have_f = false;     ///< sum r > 0.
    bool   have_g = false;     ///< the robust pool is non-empty.
    bool   enough = false;     ///< n >= kVcfMinClassRows.
    bool   identifiable = false;   ///< enough and rho >= kVcfMinRedundancyPerRow.
};

/**
 * @brief The factors of one class's accumulation.
 * @param a The class's sums; its robust pool is reordered in place (nth_element).
 * @return The two factors, the redundancy per row and the flags.  The median is the upper
 *         median for even n, the convention of the other robust statistics of the producer.
 */
NetworkVcfFactors networkVcfClassFactors(NetworkVcfClassAcc& a);

/**
 * @brief One EWMA step.
 * @param prev   The previous average.
 * @param x      The new value.
 * @param alpha  The weight of the new value, in (0, 1].
 * @param seeded false on the first value, which seeds the average.
 * @return The new average.
 */
double networkVcfEwma(double prev, double x, double alpha, bool seeded);

/**
 * @brief The stage-2 scale of one aggregate kind: sqrt of its robust EWMA when the class is
 *        identifiable and has @ref kVcfApplyMinSweeps sweeps, else 1.
 * @param st   The run state.
 * @param kind @c VcfKind::Code or @c VcfKind::Phase (any other kind returns 1).
 * @return The scale, a sigma ratio.
 */
double networkVcfApplyScale(const NetworkVcfState& st, VcfKind kind);

// ---------------------------------------------------------------------------
//  Seams
// ---------------------------------------------------------------------------

/**
 * @brief Form this epoch's per-class factors, update the EWMAs, write the trace and reset.
 *
 * An observer: call from SB_FRAMEDIAG after @c networkRedundancyCensus, whose sweep feeds the
 * accumulator.  An epoch the census did not sweep carries no rows and only counts a pass.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx
 *            (ctx), run.traces (traces). Read: run.config (config_), run.log (log).
 * @param ep  The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se).
 *
 * The bindings of the definition:
 *   - @c ctx = run.ctx: the accumulator and the EWMAs (ctx.carry.vcf), the counters.
 *   - @c traces = run.traces: owns @c network_vcf.csv.
 *   - @c se = ep.se: this epoch (week and TOW stamp).
 */
void networkVcfFlush(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Run-end report: the per-class variance factors.
 *
 * Gate: prints under `ctx.knobs.vcf_on`.  The denominators first (passes, sweeps, rows), then
 * one line per class: the final EWMAs of both statistics as factor and scale, the hourly medians
 * of the robust and the weighted factors, the redundancy per row and the identifiability, and
 * the stability of the aggregate code factor by hour (the stage-2 precondition).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkVcfRunEnd(const NetworkRunEndView& v);

}  // namespace GnssProcessor
