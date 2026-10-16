#pragma once
/**
 * @file PriorResidual.hpp
 * @brief Prior-residual census: the stale-carry test of the D5 innovation gate,
 *        generalized to every carried stiff prior (docs/SPEC_prior_residual_census.md).
 *
 * Measure-only and off by default.  The census is not fingerprinted: it is a
 * forward-only readout with no stored state and no effect on the solve (the
 * redundancy-census and rhs_attrib precedent), so a replay leg from a shared
 * snapshot with only the census armed remains a valid A/B against an unarmed
 * control.  It adds no gate, deweight, exclusion or refloat;
 * @c prior_residual_report_sigma is a trace threshold that nothing reads back.
 *
 * ## The statistic
 *
 * Standing invariant #1 requires every zero-q or stiff time-linked quantity to
 * have a wired slip, jump or event response.  Those responses exist per
 * mechanism (MW, LLI and IF-TD on the observables, D5 on the DD chords, jump
 * tests on the code bias, @c eq34_reref on the pin displacement), and none of
 * them reports which of the remaining stiff priors is carrying a stale value at
 * a given epoch.
 *
 * NGS LASER Guide v2.7 slide 5-74 gives the general statistic.  In the GMM with
 * stochastic constraints @f$z_0 = K\xi + e_0@f$, the residual of prior
 * information and its dispersion are
 * @f[ \tilde e_0 = z_0 - K\hat\xi, \qquad
 *     \hat D\{\tilde e_0\} = \hat\sigma_0^2 P_0^{-1}(I_l + P_0 K N^{-1}K^\top)^{-1}. @f]
 * Every carried prior that is folded into the solve is a stochastic-constraint
 * row.  For one scalar constraint with prior variance @f$p@f$, data-only variance
 * @f$d = KN^{-1}K^\top@f$ and posterior variance @f$v = pd/(p+d)@f$,
 * @f[ D\{\hat e\} = \frac{p^2}{p+d} = r\cdot\text{fold\_var},\qquad r = 1 - wv, @f]
 * with @f$w = 1/\text{fold\_var}@f$.  This @f$r@f$ is the partial redundancy that
 * @ref NetworkCarryVce::addRow already forms from its @c (post_var_m2, fold_var_m2)
 * arguments (@c CarryVce.hpp:266): LASER's dispersion and the VCE's
 * partial redundancy are the same quantity.  The census needs no new algebra, no
 * new solve and no covariance sweep.  Where the VCE pools these numbers into a
 * variance component, the census tests them row by row and measures the power of
 * that screen.
 *
 * ## The `folded` flag selects the dispersion; the sign flip is exact, not a convention
 *
 * @verbatim
 * folded   (the inject predicate held; the prior entered this epoch's solve):
 *     D{e} = r*fold_var = fold_var - post_var      residual of prior information
 *     r    = clamp(1 - post_var/fold_var, [eps,1]) partial redundancy (LASER 5-74)
 * unfolded (RESPONDED refloat or FRAME reref; the inject predicate skipped it):
 *     D{e} = fold_var + post_var                   predicted residual (the D5 form)
 *
 *  s = e / sqrt(D{e})        formally standardized residual
 *  t = s / sigma0_f          empirically standardized residual
 * @endverbatim
 *
 * With the fold in, @f$\mathrm{Cov}(z_0,\hat\xi) = v@f$ (the posterior used the
 * prior), so @f$D\{z_0-\hat\xi\} = p + v - 2v = p - v@f$, which equals
 * @f$r\cdot p@f$ under any network coupling.  With the fold out, the prior and
 * the solve are independent and the variances add.  The folded form applied to
 * an unfolded row is anti-conservative by orders of magnitude: a refloated
 * ambiguity has @f$v \gg p@f$, so @f$r@f$ is clamped at @ref kRedundancyFloor
 * (@f$\varepsilon@f$) and @f$|t|@f$ inflates by
 * @f$\sqrt{(p+v)/(\varepsilon p)}@f$, about 3.2e6 for a 1e-4 m² carry against a
 * 1 m² refloat.  Every RESPONDED row would then land in the top ladder bin by
 * construction, and the positive-control gate would pass without measuring
 * anything.  Test 10 of @c tests/network/test_prior_residual_census.cpp keeps this case
 * as a negative example.
 *
 * ## The critical value is empirical, not tabular (the D5 precedent)
 *
 * D5 (@c project_m1_card4_d5_innovation) applies this statistic to one family,
 * the invariant DD chords, and its three versions set the design constraints of
 * this census:
 *
 *  - v1 standardized by the raw formal @f$\mathrm{Cov}(v)@f$.  The formal sigma
 *    was 10-30x optimistic, and the gate needed a hand-tuned k = 10.
 *  - v2 calibrated an LS-VCE @f$\hat\sigma_0@f$, which reproduced that hand
 *    tuning (em10: @f$\hat\sigma_0@f$ = 2.49, so its k = 4 gate sits at an
 *    effective k = 10.0).
 *  - D5 does not fire on slips: 2.1 % of its resets show a raw MW/GF step,
 *    against 95 % on the known-discontinuity control of the same run.  A scale
 *    calibration fixes the typical scale but cannot flatten a non-Gaussian tail:
 *    a 4-sigma0 gate still flags about 17 % per epoch, where a Gaussian gives
 *    0.006 %.
 *  - v3 required positive raw-observable evidence (corroboration), which
 *    suppressed 97.3 % of trips.
 *
 * LASER's @f$t(\alpha/2, n-q)@f$ and @f$F(\alpha,1,r)@f$ critical values assume
 * Gaussian errors; on this data the measured tail exceeds the Gaussian one by
 * three orders of magnitude.  This class therefore reports an empirical
 * exceedance ladder per family (@ref ladder), on which "alpha" is a quantile
 * position, not a table lookup.  A t-table critical value (LASER 5-100/5-102)
 * applied to this data reproduces D5 v1.  Test 3 reproduces that failure mode,
 * so that it is recognized as a property of the data and not taken for a bug.
 *
 * ## Three populations, never pooled
 *
 * The @ref NetworkCarryVce feed deliberately skips contaminated rows.  That is
 * correct for estimating a variance component and backwards for a detector,
 * because the skipped rows are the ones a screen exists to find.  The census
 * therefore runs on every qualifying row, labels it
 * @ref PriorResidualLabel::Clean, @ref PriorResidualLabel::Responded or
 * @ref PriorResidualLabel::Frame, and keeps one ladder per (family x label).
 * Pooling the three populations, as the NetworkCarryVce monitor pools the rows
 * it admits, would make the output uninterpretable.
 *
 * ## The rider: a constraint-aware global variance component
 *
 * The rider is the other half of LASER 5-74 (see
 * @c docs/SPEC_prior_residual_census.md §14 for its design).  The ladder tests
 * each carried prior on its own; the rider pools the same rows into a global
 * variance component, with the prior residuals in the numerator and the
 * constraints in the redundancy:
 * @f[ \hat\sigma_0^2 = \frac{\tilde e^\top P\tilde e + \tilde e_0^\top P_0\tilde e_0}
 *                           {n - m + l} @f]
 * The network's within-epoch @f$\hat\sigma_0@f$ is a robust median of
 * standardized data-row residuals (the @c assess lambda in
 * @c SolveGlue.cpp).  It is the right statistic for RAIM attribution and
 * stays unchanged, but by construction it cannot see tension absorbed by the
 * carried priors, which are the largest constraint population in the solve by
 * orders of magnitude.  The rider adds two running sums per side and one
 * division:
 *
 * @verbatim
 *   data side    sum w*v^2  and  sum r_data     (the post-fit RAIM pool)
 *   prior side   sum w*e^2  and  sum r_prior    (census rows, folded branch only)
 *
 *   sigma0^2_global    = (sum w*v^2 + sum w*e^2) / (sum r_data + sum r_prior)
 *   prior_tension      =  sum w*e^2 / sum w*v^2       <- the headline
 * @endverbatim
 *
 * The two pools may be added because both follow the same identity: a data row
 * has @f$E[w v^2] = r_i@f$ and a folded prior row has
 * @f$E[w \hat e^2] = w\cdot r\cdot p = r@f$, so each row contributes its own
 * redundancy both to the expectation of the numerator and to the denominator.
 * A row with vanishing @f$r@f$ (the stiffest carries) contributes about 0 to
 * each and cannot bias the ratio.  The @ref kRedundancyFloor clamp, which
 * matters for @f$|t|@f$, therefore barely matters here.
 *
 * Slide 5-85 is the reason for the restriction to folded rows.  For a minimal
 * constraint set, @f$n-m+l = n-\mathrm{rk}\,A@f$: the datum pins need no
 * correction and get none (test @c Laser585MinimalConstraintEquivalence checks
 * that the two denominators coincide as the pin tightens).  Omitting the
 * non-minimal stochastic rows, the carry families, is what biases the global
 * test.  An unfolded row is not a constraint in this epoch's solve and moves
 * neither sum; admitting it would repeat the sign error described under the
 * `folded` branch above.
 *
 * The rider is also measure-only: the A4 @c raim_withhold_sigma0 guard and every
 * gate and deweight stay on the median sigma0, and nothing reads
 * @ref GlobalResult back.  sigma0_global is a scale, not a test statistic, and
 * it inherits the same heavy tail the ladders measure.
 *
 * ## Isolation and testing
 *
 * Pure numeric leaf class in the @ref NetworkCarryVce / @c NeqAccumulator mould:
 * header-only, no @c ApplicationNetwork coupling, no @c StateManager, no I/O.
 * It takes @ref Input rows and returns ladders; @c tests/network/test_prior_residual_census.cpp
 * drives it directly with synthetic rows of known truth.  The per-epoch trace,
 * event and aggregate emission lives in @c network/qc/PriorResidual.cpp, the
 * only translation unit that knows about traces.
 */

#include "network/carry/CarryVce.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

/**
 * @brief Which independent mechanism (if any) had already fired on this carried
 *        prior at this epoch.  Ladders are kept per (family x label).
 */
enum class PriorResidualLabel : int {
    Clean     = 0,  ///< Nothing fired.  A high |t| here is a miss: a stale stiff prior with no wired response, the standing invariant #1 gap the census looks for.
    Responded = 1,  ///< An independent mechanism already fired (pending reseed, slip refloat, D5, RAIM exclusion, rx-CB innovation withhold).  The positive control: a statistic with power must show a shifted |t| distribution here.
    Frame     = 2   ///< A datum handover, eq34 or anchored-election re-reference, inflated-q reinit or Q3 anchor refloat.  A frame step, not noise, and an invariant #7 re-coupling edge whose |t| measures how much the edge moves.
};
inline constexpr int kPriorResidualLabels = 3;

/// Human-readable label name (trace / log column).
inline const char* priorResidualLabelName(PriorResidualLabel l) {
    switch (l) {
        case PriorResidualLabel::Clean:     return "CLEAN";
        case PriorResidualLabel::Responded: return "RESPONDED";
        case PriorResidualLabel::Frame:     return "FRAME";
    }
    return "?";
}

/**
 * @brief Raw-observable verdict joined onto a census row (INC-3).
 *
 * The same tri-state the slip-QC pass already adjudicates (@ref RawSlipVerdict):
 * absence of evidence is evidence neither of a slip nor of its absence.
 */
enum class PriorResidualCorrob : int {
    NoEvidence = 0,  ///< No adjacent-epoch predecessor row (first sighting, gap or single band), or the slip-QC pass did not run.
    Flat       = 1,  ///< A step could be measured and the raw MW/GF level did not move.  On a flagged row this is the b-cell; on the DD family it measured 97 %.
    Stepped    = 2   ///< The raw MW/GF level moved beyond the empirical gate: positive raw-observable evidence.
};
inline constexpr int kPriorResidualCorrobs = 3;

/// Human-readable corroboration name (trace / log column).
inline const char* priorResidualCorrobName(PriorResidualCorrob c) {
    switch (c) {
        case PriorResidualCorrob::NoEvidence: return "no_evidence";
        case PriorResidualCorrob::Flat:       return "raw_flat";
        case PriorResidualCorrob::Stepped:    return "raw_stepped";
    }
    return "?";
}

/**
 * @brief The raw-observable evidence behind one census row's verdict.
 *
 * The corroboration domain of a carried prior is the set of (station, satellite)
 * pairs whose raw MW/GF observables can show a discontinuity in it:
 *
 *  - `pl`: a per-(station, satellite) ambiguity; the domain is that one pair.
 *  - `zwd`, `rxcb`: receiver-level; the domain is that station's satellites.
 *  - `pb`, `cb`: satellite-level (no station); the domain is the stations that
 *    measured that satellite this epoch.
 */
struct PriorResidualEvidence {
    int n_measured = 0;  ///< domain members with an adjacent-epoch predecessor (a step could be seen).
    int n_stepped  = 0;  ///< of those, members whose raw level moved beyond the empirical gate.
};

/**
 * @brief Consensus rule: a multi-member domain votes instead of taking the OR.
 *
 * ## Why an OR depends on the roster
 *
 * The first version (v1) resolved a multi-member domain by asking whether any
 * member stepped.  That works at 63 stations and degenerates at 303: on the same
 * data, the `cb` rows reading `raw_stepped` rose from 29 % to 70 % and the b-cell
 * (the headline INC-3 number) fell from 57 % to 21 %, with no change in the
 * physics.  With three hundred receivers nearly every satellite has some station
 * stepping in any epoch, so @f$P(\text{any of } N) = 1-(1-p)^N \to 1@f$ and the
 * join stops discriminating.  A statistic whose value depends on how many
 * receivers are in the roster does not measure the satellite.
 *
 * ## The rule, and why it needs no threshold
 *
 * The discontinuities these priors can suffer are common-mode over their domain
 * by construction, which makes a vote the physically correct reduction rather
 * than a tuning choice:
 *
 *  - a satellite code- or phase-bias jump enters MW (which carries both the
 *    satellite code and phase bias) and GF at every station tracking that
 *    satellite;
 *  - a receiver reset or firmware bias jump moves every satellite at that
 *    station.
 *
 * A single member stepping is therefore evidence about that arc (a receiver-side
 * slip on one line of sight), and it is the noise the OR accumulated over a
 * domain that grows with the roster.  The rule:
 *
 * @verbatim
 *   n_measured == 0             -> NoEvidence   (nothing could be seen)
 *   n_measured == 1             -> the member's own verdict  (pl: unchanged)
 *   n_measured >= 2             -> Stepped iff 2*n_stepped > n_measured
 *                                  (strict majority), else Flat
 * @endverbatim
 *
 * It has no threshold and no tuning, is roster-independent in expectation, and
 * reduces exactly to v1's per-arc behaviour on a one-member domain, so `pl` (the
 * family on which the OR was never wrong) is bit-for-bit unaffected.
 *
 * ## Read the b-cell with the audit columns
 *
 * The rule can only move rows from Stepped to Flat, and the b-cell counts `Flat`
 * among flagged rows, so the rule can only raise the b-cell, the census's
 * reported finding.  The rule is a correctness fix, not evidence, and the
 * resulting b-cell means something only next to a measure of how much the rule
 * moved.  @ref Ladder::n_corrob_or_stepped (what the saturating OR would have
 * returned) and @ref Ladder::sum_corrob_domain (the mean domain size) are
 * therefore carried beside every contingency table, in the trace and in the
 * run-end line.  A b-cell quoted without them is not auditable.
 */
inline PriorResidualCorrob priorResidualConsensus(const PriorResidualEvidence& ev) {
    if (ev.n_measured <= 0) return PriorResidualCorrob::NoEvidence;
    if (ev.n_measured == 1)
        return ev.n_stepped > 0 ? PriorResidualCorrob::Stepped
                                : PriorResidualCorrob::Flat;
    return (2 * ev.n_stepped > ev.n_measured) ? PriorResidualCorrob::Stepped
                                              : PriorResidualCorrob::Flat;
}

/// v1's saturating OR, kept only as the audit baseline against which the run-end
/// line reports the consensus.  Never used for a contingency cell.
inline PriorResidualCorrob priorResidualOrVerdict(const PriorResidualEvidence& ev) {
    if (ev.n_measured <= 0) return PriorResidualCorrob::NoEvidence;
    return ev.n_stepped > 0 ? PriorResidualCorrob::Stepped
                            : PriorResidualCorrob::Flat;
}

/**
 * @brief Per-row prior-residual census over the carried stiff priors (v1).
 *
 * One instance per producer run, with the same lifetime and ownership as
 * @ref NetworkCarryVce: an engine the run owns (run.engines.prires, network/driver/Run.hpp;
 * a run-TU local passed by reference until the per-epoch seam refactor; see the note on engine
 * objects in the @c network/state/RunContext.hpp banner).
 */
class NetworkPriorResidual {
public:
    // ---------------------------------------------------------------------
    //  The exceedance ladder: the census's headline product (INC-1)
    // ---------------------------------------------------------------------
    /// Number of ladder rungs.
    static constexpr int kLadderBins = 11;

    /**
     * @brief The fixed |t| ladder.
     *
     * Fixed rather than configurable, and approximately log-spaced above 4.  The
     * ladder answers what a k*sigma0 gate on a family would cost before any
     * response is wired to it, and the answer is comparable across families and
     * runs only if the rungs never move.  A family whose 4-sigma0 bin holds 17 %
     * per epoch has the heavy tail D5 had.
     */
    static const std::array<double, kLadderBins>& ladder() {
        static const std::array<double, kLadderBins> kL{
            {1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 20.0, 50.0, 100.0, 200.0}};
        return kL;
    }

    /// Index of the ladder rung the field gates quote (|t| > 4).
    static constexpr int kLadder4Sigma = 3;

    /// One (family x label) exceedance ladder.  O(1) per row: no row storage,
    /// no quantile sketch, forgetting-compatible.
    struct Ladder {
        long long n_rows      = 0;   ///< rows folded into this ladder.
        long long n_folded    = 0;   ///< of those, rows on the `r*fold_var` (folded) branch.
        long long n_raw_formal= 0;   ///< of those, rows reported raw formal (t == s, flagged) because sigma0 is below warm-up or the fold variance is not reconstructible.
        long long n_r_clamped = 0;   ///< folded rows whose partial redundancy was clamped to the numerical [eps,1] range.  On a label ladder the clamp truncates |t| (reads low) and never inflates it; every row of an rneg ladder is clamped up from r < 0, and its |t| is inflated (see @ref rnegRunLadder).
        /// Rows with post_var > fold_var, i.e. r < 0: the coupling-carry case
        /// documented on @ref NetworkCarryVce::addRow (CarryVce.hpp:266),
        /// where the harvested posterior is a conditional variance that can
        /// legitimately exceed the prior.  Such a prior/posterior pair does not
        /// describe one fold.
        ///
        /// Always 0 on a CLEAN, RESPONDED or FRAME ladder: these rows enter the
        /// rneg ladders instead (see @ref rnegRunLadder).  The column is kept so
        /// that existing readers of network_prior_residual.csv still parse; the
        /// population is reported in the `rneg_*` columns at the end of the row.
        /// On an rneg ladder it equals @ref n_rows by construction.
        long long n_r_negative = 0;
        long long n_no_sigma0 = 0;   ///< rows withheld from the sigma0 calibration because their null dispersion is undefined (the only admissible censoring).
        std::array<long long, kLadderBins> bins{};  ///< cumulative exceedance: bins[k] = number of rows with |t| > ladder()[k].
        double    sum_log_t2  = 0.0; ///< sum log(t^2) for the tail-robust geometric mean.
        long long n_geo       = 0;   ///< rows in @ref sum_log_t2.
        // --- the worst row's full identity (invariant #4: a finding needs a name) --
        double      max_abs_t = 0.0; ///< largest |t| seen (on an rneg ladder, the |t| of the largest-|ê| row).
        double      max_e     = 0.0; ///< its innovation (m).
        double      max_disp  = 0.0; ///< its D{e} (m^2).
        double      max_fold_var = 0.0;
        double      max_post_var = 0.0;
        double      max_gap_s = 0.0;
        /// Largest |ê| (metres) seen by this ladder.  It is the headline of the
        /// rneg class, where |t| is not meaningful: the dispersion is formed from
        /// a (prior, posterior) pair that does not describe one fold, so `r` is
        /// clamped at @ref kRedundancyFloor and |t| inflates by √((p+v)/(εp)).
        /// Kept on every ladder (it costs one compare) so that both classes are
        /// read the same way, but only the rneg readout quotes it.  The identity
        /// fields (every max_* field except this one) follow the |ê| winner on an
        /// rneg ladder and the |t| winner elsewhere (see @ref keepsMaxByE).
        double      max_abs_e = 0.0;
        double      max_tow   = -1.0;///< its epoch (GPS TOW s); < 0 = never set.
        int         max_station = -1;///< its app station index (-1 = shared / none).
        std::string max_sat;         ///< its satellite id (empty = none).
        std::string max_signal;      ///< its signal / key (empty = none).
        const char* max_mech = "";   ///< the mechanism tag that labelled it.
        /// Contingency table (INC-3): row [0] = |t| > report_sigma, row [1] =
        /// |t| <= report_sigma; columns follow @ref PriorResidualCorrob.  All zero
        /// unless corroboration is armed.  The b-cell (row 0, Flat) is the
        /// finding the census reports.
        std::array<std::array<long long, kPriorResidualCorrobs>, 2> contingency{};
        /// Audit of the corroboration join (INC-3; see @ref priorResidualConsensus).
        /// The consensus rule can only move rows from Stepped to Flat, and so can
        /// only raise the b-cell; a b-cell presented without these counts is not
        /// auditable.
        long long n_corrob_or_stepped = 0;  ///< rows v1's saturating OR would have called Stepped (>= the consensus count by construction when a resolver is installed).
        long long n_corrob_stepped    = 0;  ///< rows whose joined verdict is Stepped (the consensus when a resolver is installed); the verdict used in @ref contingency.
        long long sum_corrob_domain   = 0;  ///< sum of domain sizes (n_measured).  Divided by @ref n_corrob_rows it gives the mean domain size (@ref meanCorrobDomain), the quantity that grows with the roster.
        long long n_corrob_rows       = 0;  ///< rows carrying any evidence at all (the denominator for the mean domain).
        /// The highest stepped fraction (n_stepped / n_measured) any domain
        /// reached, over rows carrying evidence.  This is the sensitivity number,
        /// and it makes a consensus count falsifiable.  The rule's bar is 0.5, so
        /// a run whose maximum fraction is, say, 0.08 never approached the bar:
        /// the data produced nothing within the rule's range, and a zero
        /// consensus count says nothing about whether a real common-mode event
        /// would have been caught.  A maximum near or above 0.5 places the bar
        /// inside the observed distribution, and the count then means what it
        /// appears to mean.
        double    max_corrob_frac     = 0.0;
        /// True on the rneg ladders: the identity fields track the largest |ê|
        /// rather than the largest |t|.  Set once, when the family state is
        /// constructed, and never by a row, so the two classes cannot swap
        /// ordering mid-run.
        bool      keepsMaxByE = false;

        /// @return chi2-corrected geometric mean of t^2 (1.0 = the empirical
        /// scale calibration has converged).  This can sit at 1 while the
        /// 4-sigma bin is heavily over-occupied, because a scale calibration
        /// cannot flatten a non-Gaussian tail (test 3).
        double geoT2() const {
            if (n_geo <= 0) return 0.0;
            return std::exp(sum_log_t2 / static_cast<double>(n_geo)) / kChi2GeoMean;
        }
        /// Mean corroboration-domain size (0 when the join was not armed).  It
        /// detects roster dependence: on the shared families it grows from 1 to
        /// about 300 with the roster, which is what saturated v1's OR.
        double meanCorrobDomain() const {
            return n_corrob_rows > 0 ? static_cast<double>(sum_corrob_domain)
                                     / static_cast<double>(n_corrob_rows)
                                     : 0.0;
        }
        /// Fraction of rows above ladder rung @p k (0 when empty).
        double frac(int k) const {
            if (n_rows <= 0 || k < 0 || k >= kLadderBins) return 0.0;
            return static_cast<double>(bins[static_cast<std::size_t>(k)])
                 / static_cast<double>(n_rows);
        }
        /// Clear every accumulator.  @ref keepsMaxByE is class identity, not
        /// content, so it survives the reset: an epoch reset that returned an
        /// rneg ladder to |t| ordering would change the meaning of its identity
        /// columns from epoch 2 onward, with nothing in the output to show it.
        void reset() { const bool by_e = keepsMaxByE; *this = Ladder{}; keepsMaxByE = by_e; }
    };

    /// What @ref addRow computed for one row, returned so the caller can trace
    /// it.  The census is measure-only: the caller does not act on it.
    struct RowStat {
        bool   accepted   = false;  ///< The row entered a ladder (family requested + inputs finite).
        bool   folded     = false;  ///< Which dispersion branch was used.
        bool   raw_formal = false;  ///< t == s, because sigma0 is below warm-up or the fold variance was not reconstructible.
        bool   r_clamped  = false;  ///< The partial redundancy was clamped to the numerical [eps,1] range.  For 0 <= r < eps this truncates |t| (reads low); an r_negative row is also clamped, and its |t| inflates.
        bool   r_negative = false;  ///< post_var > fold_var (r < 0 before the floor): the coupling-carry case, not a stiff prior.
        bool   reported   = false;  ///< |t| exceeded @ref reportSigma (a trace threshold; nothing acts on it).
        double disp       = 0.0;    ///< D{e} (m^2).
        double r          = 0.0;    ///< partial redundancy (folded branch only; 0 on the `+` branch).
        double s          = 0.0;    ///< formally standardized residual.
        double t          = 0.0;    ///< empirically standardized residual.
        double sigma0     = 1.0;    ///< the per-family sigma0 actually used (frozen at @ref beginEpoch).
        int    family     = -1;     ///< resolved family index (-1 = not requested).
        PriorResidualCorrob corrob_used = PriorResidualCorrob::NoEvidence;  ///< raw-observable verdict joined to the row (INC-3); NoEvidence when corroboration is off.
    };

    /// One row of the census, as handed over by a feed site.
    struct Input {
        std::string_view family;     ///< "pl" / "pb" / "cb" / "zwd" / "rxcb"; ignored when not requested.
        std::string_view sat;        ///< satellite id ("G10"); empty for a station-level prior.
        std::string_view signal;     ///< signal / carry key; empty when not applicable.
        int    station   = -1;       ///< app station index; -1 for a shared (satellite-level) prior.
        double e         = 0.0;      ///< the residual of prior information, ê = carried prior − solved (m).
        double fold_var  = 0.0;      ///< the prior variance the mechanism actually used, or would have used (m^2).
        double post_var  = 0.0;      ///< the posterior variance of that state after the solve (m^2).
        double gap_s     = 0.0;      ///< arc / carry gap the fold variance was inflated over (s).
        bool   folded    = true;     ///< Did the prior enter this epoch's solve?  Selects the dispersion branch (see the file banner).
        PriorResidualLabel label = PriorResidualLabel::Clean;
        /// Feed this row's `s` into the per-family sigma0 accumulator?  True for
        /// every ordinary row, including above-threshold ones.  D5 v2's rule
        /// applies unchanged: above-threshold rows have to feed sigma0, because
        /// censoring biases sigma0 low and, under misspecification, the flagged
        /// rows are the calibration signal.  The only admissible false is a row
        /// whose null dispersion is undefined (a `cta_refloat` clock the anchor
        /// guard un-folded, or a fold variance the site could not reconstruct):
        /// it is excluded because the null does not exist, never because the
        /// value is large.
        bool   calibrate = true;
        /// The fold variance is a nominal recompute that the mechanism did not
        /// use.  The row is reported raw formal (t == s, flagged) instead of
        /// being standardized by a variance that was never applied, and
        /// @c calibrate = false is implied.
        bool   var_nominal = false;
        PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;  ///< caller-supplied raw-observable verdict (INC-3); when corroboration is armed and @ref setCorroborator installed a resolver, the resolver's consensus replaces it.
        const char* mech = "";       ///< mechanism tag for the trace ("reseed", "eq34_reref", "inflate_q", "cta_refloat", "handover", "gap", ...).
    };

    /// A row that exceeded @ref reportSigma, buffered for the emission seam.
    /// The buffer is bounded per epoch (@ref kMaxHitsPerEpoch), so an epoch
    /// with many flagged rows cannot grow it without limit.
    struct Hit {
        int         family_idx = -1;
        PriorResidualLabel label = PriorResidualLabel::Clean;
        int         station = -1;
        std::string sat;
        std::string signal;
        double      e = 0.0, fold_var = 0.0, post_var = 0.0, gap_s = 0.0;
        double      disp = 0.0, r = 0.0, s = 0.0, t = 0.0, sigma0 = 1.0;
        bool        folded = false, raw_formal = false, r_clamped = false, r_negative = false;
        PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;
        const char* mech = "";
    };

    /// Per-epoch buffer cap for @ref Hit.  Rows beyond it are still counted in
    /// every ladder; only their individual identity is dropped, and each drop is
    /// counted in @ref hitsDropped so that a truncated event stream does not read
    /// as a quiet epoch.
    static constexpr std::size_t kMaxHitsPerEpoch = 512;

    // ---------------------------------------------------------------------
    //  The constraint-aware global variance component: the rider on this
    //  accumulator (docs/SPEC_prior_residual_census.md §14)
    // ---------------------------------------------------------------------

    /**
     * @brief The four sums LASER 5-74's global form needs, plus the exclusion
     *        counts that make them auditable.
     *
     * Two of the three exclusion counters exist because the alternative is a
     * biased ratio that nothing reports: a row admitted to the numerator but not
     * the denominator (or the reverse) shifts @f$\hat\sigma_0^2@f$ with nothing
     * in the output to show it.  Every skip is counted and reported in the
     * run-end readout; the rneg skip also sets @ref kGlobalRnegDrop, and the
     * r ≡ 1 data fallback sets @ref kGlobalApprox.
     */
    struct GlobalSums {
        // --- data side: the post-fit RAIM pool ------------------------------
        double    wv2     = 0.0;   ///< Σ w·v² over the visited data rows (each row counted once; see @ref addDataRow).
        double    r_data  = 0.0;   ///< Σ r_i over those same rows (Baarda per-row redundancy, or 1 when unavailable).
        long long n_data  = 0;     ///< rows behind the two sums above.
        long long n_data_approx = 0; ///< of those, rows counted with r ≡ 1 because no per-row redundancy was available (`network.raim_redundancy_normalize` off, or no redundancy vector for the station).  Σr_data is then partly a row count, and @ref kGlobalApprox marks it in-band.
        // --- prior side: the census's own folded rows -----------------------
        double    we2     = 0.0;   ///< Σ w·ê² = Σ ê²/fold_var (LASER's ẽ₀ᵀP₀ẽ₀).
        double    r_prior = 0.0;   ///< Σ r over those same rows (the partial redundancy the census already forms).
        long long n_prior = 0;     ///< rows behind the two sums above.
        long long n_prior_unfolded = 0;///< census rows skipped: the prior did not enter this epoch's solve, so it is not a constraint and moves neither sum.
        long long n_prior_nominal  = 0;///< census rows skipped: the fold variance is a nominal recompute the mechanism never used, so w = 1/fold_var does not describe the fold (the `cta_refloat` and rx-CB `gap` populations).
        long long n_prior_rneg     = 0;///< census rows skipped: post_var > fold_var, a (prior, posterior) pair that does not describe one fold.  The numerator term is real and the redundancy is not; admitting one without the other is the bias the rider exists to remove.
        void reset() { *this = GlobalSums{}; }
    };

    /// In-band degradation bits on @ref GlobalResult::flags.  Standing invariant
    /// #4's convention: a degraded number is marked as degraded in the same row
    /// in which it is written.
    enum GlobalFlag : int {
        kGlobalApprox     = 1,  ///< ≥1 data row entered with r ≡ 1: Σr_data is then partly a row count, not a redundancy, and σ̂₀_global reads low.
        kGlobalNoData     = 2,  ///< zero data rows (RAIM did not run this epoch): the number is prior-only and is not the LASER global statistic.
        kGlobalNoPrior    = 4,  ///< zero folded census rows: the number is data-only, the quantity the rider exists to correct.
        kGlobalDegenerate = 8,  ///< Σr ≤ 0 or non-finite ⇒ σ̂₀²_global is not formed; the column is blank, never 0.
        kGlobalRnegDrop   = 16  ///< ≥1 folded row dropped for post_var > fold_var (@ref GlobalSums::n_prior_rneg).
    };

    /// The formed statistic for one accumulation window (an epoch, a family's
    /// prior half, or the whole run).  Pure function of a @ref GlobalSums.
    struct GlobalResult {
        bool   formed = false;        ///< σ̂₀²_global is defined (Σr > 0 and finite).
        double sigma0_global   = 0.0; ///< σ̂₀, the square root; this is what the column carries (comparable to the median σ̂₀ it is measured against).
        double sigma0_global_v = 0.0; ///< σ̂₀², the variance component itself.
        double prior_tension   = 0.0; ///< Σwê² / Σwv²: how much of the epoch's tension lives in the carry.  The headline readout.
        bool   tension_defined = false;///< false when there is no data side to divide by (the census never writes ∞).
        int    flags = 0;             ///< @ref GlobalFlag bitmask.
        GlobalSums s{};               ///< the sums it was formed from.
    };

    /// Form the global variance component from a set of sums.  Static and pure:
    /// the unit test calls it on hand-built sums, and the epoch / run / family
    /// readouts below are the same three lines applied to different windows.
    static GlobalResult computeGlobal(const GlobalSums& s) {
        GlobalResult g;
        g.s = s;
        const double num = s.wv2 + s.we2;
        const double den = s.r_data + s.r_prior;
        if (s.n_data_approx > 0)     g.flags |= kGlobalApprox;
        if (s.n_data  == 0)          g.flags |= kGlobalNoData;
        if (s.n_prior == 0)          g.flags |= kGlobalNoPrior;
        if (s.n_prior_rneg > 0)      g.flags |= kGlobalRnegDrop;
        if (den > 0.0 && std::isfinite(den) && std::isfinite(num) && num >= 0.0) {
            g.sigma0_global_v = num / den;
            g.sigma0_global   = std::sqrt(g.sigma0_global_v);
            g.formed          = std::isfinite(g.sigma0_global);
        }
        if (!g.formed) g.flags |= kGlobalDegenerate;
        if (s.wv2 > 0.0 && std::isfinite(s.wv2) && std::isfinite(s.we2)) {
            g.prior_tension   = s.we2 / s.wv2;
            g.tension_defined = std::isfinite(g.prior_tension);
        }
        return g;
    }

    /// Per-epoch (σ̂₀_global, prior_tension) history cap.  262144 epochs is about
    /// 91 days at 30 s.  Past the cap a sample is dropped and counted
    /// (@ref globalSamplesDropped), so a run-end median cannot silently become a
    /// median of the first N epochs.
    static constexpr std::size_t kMaxGlobalSamples = 262144;

    /**
     * @brief Construct the census.
     * @param families      families to accumulate (@c NetworkCarryVce::parseComponents
     *                      of @c network.prior_residual_families).  Empty ⇒ inert.
     * @param window_epochs sigma0 exponential-forgetting half-life in epochs
     *                      (<= 0 ⇒ whole-run accumulation).
     * @param min_rows      sigma0 warm-up: below this effective row count the
     *                      family reports t == s and flags it (200, as in D5 v2).
     * @param report_sigma  per-row trace/event threshold in sigma0 units.  A
     *                      reporting threshold; nothing in the solve reads it.
     * @param corroborate   join every row to the raw-observable verdict (INC-3).
     * @param global_sigma0 The constraint-aware global variance component rider
     *                      (@c network.prior_residual_global_sigma0).  When off,
     *                      the four sums are never touched, @ref addDataRow is a
     *                      single bool test, and no column exists.
     */
    NetworkPriorResidual(std::vector<std::string> families,
                         double window_epochs,
                         double min_rows      = 200.0,
                         double report_sigma  = 4.0,
                         bool   corroborate   = false,
                         bool   global_sigma0 = false)
        : families_(std::move(families)),
          min_rows_(min_rows > 0.0 ? min_rows : 0.0),
          report_sigma_(report_sigma > 0.0 ? report_sigma : 0.0),
          corroborate_(corroborate),
          global_on_(global_sigma0),
          // The sigma0 accumulator is built the way D5 v2 builds its own (the
          // inflation_->addRow feed in InvariantCarryDriver.cpp): a
          // NetworkCarryVce fed the standardized value with unit fold variance,
          // so Estimate::robust_floor reads back as sigma0^2.
          // split_by_const = false: the ladder's unit is the family, and a
          // per-constellation split would quarter every population and delay
          // warm-up fourfold for no question the census asks.  seed_floor = 1,
          // so a family with no rows reads sigma0 = 1.
          inflation_(families_, window_epochs, /*split_by_const=*/false,
                     /*seed_floor_m2=*/1.0, /*seed_rate_m2ps=*/0.0) {
        state_.resize(families_.size());
        for (auto& st : state_) {
            st.sigma0 = 1.0;
            // The rneg class orders its identity columns by |ê| (metres), not by
            // |t| (see rnegRunLadder).  Set once, here, and preserved by
            // Ladder::reset(), so the ordering cannot change mid-run.
            for (auto& l : st.rneg_epoch) l.keepsMaxByE = true;
            for (auto& l : st.rneg_run)   l.keepsMaxByE = true;
        }
    }

    /// @return true when at least one family is requested (the census is live).
    bool enabled() const { return !families_.empty(); }
    /// @return the requested family list (run-end "armed but never ran" check).
    const std::vector<std::string>& families() const { return families_; }
    /// @return the resolved family index, or -1 when not requested.
    int familyIndex(std::string_view f) const {
        for (std::size_t i = 0; i < families_.size(); ++i)
            if (families_[i] == f) return static_cast<int>(i);
        return -1;
    }
    /// @return true when @p f is one of the requested families.
    bool wants(std::string_view f) const { return familyIndex(f) >= 0; }
    double reportSigma() const { return report_sigma_; }
    double minRows()     const { return min_rows_; }
    bool   corroborating() const { return corroborate_; }
    /// Is the constraint-aware global variance component armed?  This is the
    /// only predicate that gates the RAIM data-side feed (read once, tested per row).
    bool   globalOn()      const { return global_on_ && enabled(); }

    /**
     * @brief Install the raw-observable evidence resolver (INC-3).
     *
     * Injected rather than plumbed, so the accumulator stays a pure numeric leaf
     * (its unit test installs its own, or none) and the five feed sites do not
     * each need a @c SlipQcChannel parameter.  Called at most once per run;
     * never called when @c corroborate is off.
     *
     * @param fn (family, station index, satellite) -> @ref PriorResidualEvidence,
     *           the domain counts for that prior.  @p station is -1 for a shared
     *           prior, in which case the resolver counts over the stations that
     *           measured that satellite this epoch.  The resolver only gathers
     *           the counts; @ref addRow reduces them with
     *           @ref priorResidualConsensus.
     */
    void setCorroborator(
        std::function<PriorResidualEvidence(std::string_view, int, std::string_view)> fn) {
        corrob_fn_ = std::move(fn);
    }

    /**
     * @brief Advance to a new epoch: freeze this epoch's per-family sigma0, clear
     *        the per-epoch ladders, and apply forgetting to the calibration.
     *
     * Predict-then-update, the first of the two ordering contracts D5 v2 depends
     * on: every row of epoch N is standardized against the sigma0 through epoch
     * N-1 frozen here, and the rows themselves reach the accumulator only
     * afterwards.  Otherwise a row calibrates against itself, and a single huge
     * row standardizes itself away (test 4).
     *
     * The census always passes @c clean = true to the calibration's
     * @ref NetworkCarryVce::beginEpoch, where false would make the VCE ignore
     * every row of the epoch: a datum-changed epoch is a FRAME population to
     * measure, not one to discard.
     *
     * @param content_time_s epoch content time (continuous GPS s).
     * @param tow            epoch GPS TOW (s); stamps the worst-row identity.
     */
    void beginEpoch(double content_time_s, double tow) {
        if (!enabled()) return;
        last_tow_ = tow;
        ++n_epochs_;
        for (std::size_t f = 0; f < state_.size(); ++f) {
            FamilyState& st = state_[f];
            const NetworkCarryVce::Estimate e = inflation_.estimate(families_[f], std::string());
            // Warm-up: below min_rows the family reports t == s and says so in
            // the output column, rather than silently standardizing by a seed.
            if (e.n_eff >= min_rows_ && e.robust_floor > 0.0
                && std::isfinite(e.robust_floor)) {
                st.sigma0 = std::sqrt(e.robust_floor);
                st.warm   = st.sigma0 > 0.0 && std::isfinite(st.sigma0);
                if (!st.warm) st.sigma0 = 1.0;
            } else {
                st.sigma0 = 1.0;
                st.warm   = false;
            }
            // Not clamped to [1, cap] the way D5's gate clamps it.  D5 may only
            // raise its threshold because it acts on the result; a census that
            // clamped would report a family with sigma0 < 1 as if it were
            // calibrated, and sigma0 < 1 is itself the measurement.
            st.n_eff  = e.n_eff;
            st.n_cal_rows = e.n_rows;
            for (auto& l : st.epoch)      l.reset();
            for (auto& l : st.rneg_epoch) l.reset();
            st.g_epoch.reset();
        }
        global_epoch_.reset();
        hits_.clear();
        hits_dropped_ = 0;
        ep_rows_ = ep_rows_resp_ = ep_rows_frame_ = ep_reported_ = 0;
        ep_rows_rneg_ = 0;
        ep_max_abs_t_ = 0.0;
        ep_rneg_max_abs_e_ = 0.0;
        inflation_.beginEpoch(content_time_s, /*clean=*/true);
    }

    /**
     * @brief Fold one carried prior's residual of prior information into the census.
     *
     * O(1): a divide, a log and two counter bumps.  No covariance access, no
     * reconstruct and no sweep: @c post_var and @c fold_var are already in hand
     * at every feed site, which keeps the census far cheaper than the redundancy
     * census (measured at 6.1 ms per station sweep).
     */
    RowStat addRow(const Input& in) {
        RowStat out;
        if (!enabled()) return out;
        const int fi = familyIndex(in.family);
        if (fi < 0) return out;
        out.family = fi;
        if (!std::isfinite(in.e) || !std::isfinite(in.fold_var)
            || !std::isfinite(in.post_var) || !(in.fold_var > 0.0)
            || !(in.post_var >= 0.0))
            return out;                                  // not measurable this epoch

        // --- the dispersion, by the `folded` branch --------------------------
        double disp, r = 0.0;
        bool   r_clamped = false, r_negative = false;
        if (in.folded) {
            r = 1.0 - in.post_var / in.fold_var;         // = 1 - w*v, LASER 5-74
            r_negative = r < 0.0;                        // coupling-carry conditional > prior
            if (r < kRedundancyFloor) { r = kRedundancyFloor; r_clamped = true; }
            if (r > 1.0)              { r = 1.0;              r_clamped = true; }
            disp = r * in.fold_var;                      // == fold_var - post_var (exactly, unclamped)
        } else {
            disp = in.fold_var + in.post_var;            // independent prior and solve: variances add
        }
        if (!(disp > 0.0) || !std::isfinite(disp)) return out;

        FamilyState& st = state_[static_cast<std::size_t>(fi)];
        const double s = in.e / std::sqrt(disp);
        if (!std::isfinite(s)) return out;
        // A row whose fold variance is a nominal recompute the mechanism did not
        // use has no trustworthy scale, so it is reported raw formal and kept out
        // of the calibration (its `s` would corrupt sigma0 for every other row).
        const bool raw_formal = !st.warm || in.var_nominal;
        const double sigma0   = raw_formal ? 1.0 : st.sigma0;
        const double t        = s / sigma0;
        const double at       = std::abs(t);

        // --- the ladder (INC-1), partitioned by r_negative --------------------
        // An r_negative row's |t| is not a standardized residual: its dispersion
        // comes from a (prior, posterior) pair that does not describe one fold,
        // so r is clamped at kRedundancyFloor and |t| inflates by √((p+v)/(εp)),
        // the artifact the file banner computes for the wrong-branch case,
        // reached here by another path.  The σ̂₀_global path drops these rows
        // (kGlobalRnegDrop); the ladder routes them to a class of their own,
        // apart from the bin-counted CLEAN/RESPONDED/FRAME population, so both
        // stay readable (rnegRunLadder gives the measured effect).
        const bool to_rneg = r_negative;
        Ladder& L = to_rneg ? st.rneg_epoch[static_cast<std::size_t>(in.label)]
                            : st.epoch[static_cast<std::size_t>(in.label)];
        Ladder& R = to_rneg ? st.rneg_run[static_cast<std::size_t>(in.label)]
                            : st.run[static_cast<std::size_t>(in.label)];
        const auto& lad = ladder();
        for (int k = 0; k < kLadderBins; ++k) {
            if (!(at > lad[static_cast<std::size_t>(k)])) break;   // ladder ascends: first miss ends it
            ++L.bins[static_cast<std::size_t>(k)];
            ++R.bins[static_cast<std::size_t>(k)];
        }
        // sum_log_t2 / n_geo form the geometric mean of t², the convergence
        // readout of the scale calibration.  An rneg row's t² is not on that
        // scale, so it accumulates in the rneg class's own sums (where geoT2 is
        // reported as a property of that class) and never in the label class's.
        const double t2 = t * t;
        const double lt = std::log(t2 > kTinyT2 ? t2 : kTinyT2);
        L.sum_log_t2 += lt; ++L.n_geo;
        R.sum_log_t2 += lt; ++R.n_geo;
        ++L.n_rows; ++R.n_rows;
        if (in.folded)      { ++L.n_folded; ++R.n_folded; }
        if (raw_formal)     { ++L.n_raw_formal; ++R.n_raw_formal; }
        if (r_clamped)      { ++L.n_r_clamped; ++R.n_r_clamped; }
        if (r_negative)     { ++L.n_r_negative; ++R.n_r_negative; }

        const bool reported = report_sigma_ > 0.0 && at > report_sigma_;
        if (corroborate_) {
            // The rule lives in priorResidualConsensus (pure, unit-tested); the
            // resolver only gathers the domain counts.  The separation makes the
            // roster-independence claim testable without a network.
            PriorResidualCorrob c = in.corrob;
            if (corrob_fn_) {
                const PriorResidualEvidence ev =
                    corrob_fn_(in.family, in.station, in.sat);
                c = priorResidualConsensus(ev);
                if (ev.n_measured > 0) {
                    L.sum_corrob_domain += ev.n_measured;
                    R.sum_corrob_domain += ev.n_measured;
                    ++L.n_corrob_rows; ++R.n_corrob_rows;
                    const double frac = static_cast<double>(ev.n_stepped)
                                      / static_cast<double>(ev.n_measured);
                    if (frac > L.max_corrob_frac) L.max_corrob_frac = frac;
                    if (frac > R.max_corrob_frac) R.max_corrob_frac = frac;
                    if (priorResidualOrVerdict(ev) == PriorResidualCorrob::Stepped) {
                        ++L.n_corrob_or_stepped; ++R.n_corrob_or_stepped;
                    }
                }
            }
            if (c == PriorResidualCorrob::Stepped) {
                ++L.n_corrob_stepped; ++R.n_corrob_stepped;
            }
            const std::size_t row = reported ? 0u : 1u;
            ++L.contingency[row][static_cast<std::size_t>(c)];
            ++R.contingency[row][static_cast<std::size_t>(c)];
            out.corrob_used = c;
        }

        // The identity of the worst row.  The ordering key differs by class:
        // |t| on a label ladder (the statistic), |ê| on an rneg ladder (metres,
        // the only meaningful quantity there, since the dispersion is
        // undefined).  The |ê| extremum is tracked on both classes so the
        // columns line up; every other max_* field follows the ordering key,
        // which is fixed per ladder.
        const double ae = std::abs(in.e);
        auto keepMax = [&](Ladder& lg) {
            const bool win = lg.keepsMaxByE ? (ae > lg.max_abs_e) : (at > lg.max_abs_t);
            if (ae > lg.max_abs_e) lg.max_abs_e = ae;   // the extremum, on both classes
            if (!win) return;
            lg.max_abs_t = at;      lg.max_e = in.e;   lg.max_disp = disp;
            lg.max_fold_var = in.fold_var; lg.max_post_var = in.post_var;
            lg.max_gap_s = in.gap_s; lg.max_tow = last_tow_;
            lg.max_station = in.station;
            lg.max_sat.assign(in.sat.begin(), in.sat.end());
            lg.max_signal.assign(in.signal.begin(), in.signal.end());
            lg.max_mech = in.mech ? in.mech : "";
        };
        keepMax(L); keepMax(R);

        // --- the sigma0 calibration (update, after the standardization) -------
        // Rows are never censored by threshold.  The only rows withheld are those
        // whose null dispersion is undefined; they are counted so the exclusion
        // is auditable.
        if (in.calibrate && !in.var_nominal) {
            inflation_.addRow(families_[static_cast<std::size_t>(fi)], std::string(),
                              s, /*gap_s=*/0.0, /*post_var=*/0.0, /*fold_var=*/1.0);
        } else {
            ++L.n_no_sigma0; ++R.n_no_sigma0;
        }

        // --- per-epoch aggregates + the bounded identity buffer ---------------
        // ep_rows_ is the total (label classes + rneg), so the epoch-status row
        // closes against the ladder counts.  ep_max_abs_t_, from which the
        // `prires_max_t` column and the run-end "worst |t|" line are built,
        // excludes the rneg class for the reason given at the ladder above.
        ++ep_rows_;
        if (in.label == PriorResidualLabel::Responded) ++ep_rows_resp_;
        if (in.label == PriorResidualLabel::Frame)     ++ep_rows_frame_;
        if (to_rneg) {
            ++ep_rows_rneg_;
            if (ae > ep_rneg_max_abs_e_) ep_rneg_max_abs_e_ = ae;
        } else if (at > ep_max_abs_t_) {
            ep_max_abs_t_ = at;
        }
        if (reported) {
            ++ep_reported_;
            ++n_reported_run_;
            if (hits_.size() < kMaxHitsPerEpoch) {
                Hit h;
                h.family_idx = fi;   h.label = in.label;  h.station = in.station;
                h.sat.assign(in.sat.begin(), in.sat.end());
                h.signal.assign(in.signal.begin(), in.signal.end());
                h.e = in.e; h.fold_var = in.fold_var; h.post_var = in.post_var;
                h.gap_s = in.gap_s; h.disp = disp; h.r = r; h.s = s; h.t = t;
                h.sigma0 = sigma0;  h.folded = in.folded;
                h.raw_formal = raw_formal; h.r_clamped = r_clamped;
                h.r_negative = r_negative;
                h.corrob = out.corrob_used; h.mech = in.mech ? in.mech : "";
                hits_.push_back(std::move(h));
            } else {
                ++hits_dropped_;
            }
        }

        // --- the constraint-aware global variance component (the rider) -------
        // docs/SPEC_prior_residual_census.md §14.2.  Folded rows only: an
        // unfolded prior is not a stochastic constraint in this epoch's solve, so
        // it is in neither LASER's ẽ₀ᵀP₀ẽ₀ nor its `l`.  Three skips, each
        // counted (a one-sided row would bias the ratio with nothing to show it):
        //  * unfolded:    not a constraint this epoch;
        //  * var_nominal: the fold variance is a recompute the mechanism never
        //                 used, so w = 1/fold_var does not describe the fold and
        //                 both sums would be wrong (this is also the `cta_refloat`
        //                 population, whose null dispersion is undefined);
        //  * r_negative:  post_var > fold_var is a (prior, posterior) pair that
        //                 does not describe one fold.  Its w·ê² is a real
        //                 weighted residual and its redundancy is not, and
        //                 admitting the numerator without the denominator is the
        //                 bias the rider exists to remove.
        // A row that is only floored (0 <= r < ε, the stiffest carries) is kept:
        // it contributes about 0 to both sums by construction, since E[w·ê²] = r.
        if (global_on_) {
            GlobalSums& fe = st.g_epoch;
            GlobalSums& fr = st.g_run;
            if (!in.folded) {
                ++global_epoch_.n_prior_unfolded; ++global_run_.n_prior_unfolded;
                ++fe.n_prior_unfolded;            ++fr.n_prior_unfolded;
            } else if (in.var_nominal) {
                ++global_epoch_.n_prior_nominal;  ++global_run_.n_prior_nominal;
                ++fe.n_prior_nominal;             ++fr.n_prior_nominal;
            } else if (r_negative) {
                ++global_epoch_.n_prior_rneg;     ++global_run_.n_prior_rneg;
                ++fe.n_prior_rneg;                ++fr.n_prior_rneg;
            } else {
                const double q = in.e * in.e / in.fold_var;   // w·ê², w = P₀ = 1/fold_var
                if (std::isfinite(q)) {
                    global_epoch_.we2 += q;      global_run_.we2 += q;
                    fe.we2 += q;                 fr.we2 += q;
                    global_epoch_.r_prior += r;  global_run_.r_prior += r;
                    fe.r_prior += r;             fr.r_prior += r;
                    ++global_epoch_.n_prior;     ++global_run_.n_prior;
                    ++fe.n_prior;                ++fr.n_prior;
                }
            }
        }

        out.accepted = true;  out.folded = in.folded;   out.raw_formal = raw_formal;
        out.r_clamped = r_clamped; out.r_negative = r_negative; out.reported = reported;
        out.disp = disp; out.r = r; out.s = s; out.t = t; out.sigma0 = sigma0;
        return out;
    }

    /**
     * @brief Fold one data row into the data side of the global variance component.
     *
     * The rider's other half (docs/SPEC_prior_residual_census.md §14.2).  Called
     * from the post-fit RAIM Pass-1 row walk, which already holds @p w and, with
     * `network.raim_redundancy_normalize`, the per-row Baarda redundancy @p r, so
     * this costs a multiply and two adds and needs no covariance access, no
     * reconstruct and no sweep.
     *
     * The pool is the RAIM pool, not the whole system.  That walk visits the
     * phase rows of carried ambiguities at the stations RAIM walks (the pivot
     * station is skipped unless `network.phase_datum_prioritized` is on); it does
     * not visit the code rows, the constraint rows or the stations RAIM skips.
     * @f$\sum r_{data}@f$ here is therefore not @f$n - \mathrm{rk}\,A@f$ of the
     * network, and @f$\hat\sigma_0^2@f$ is a variance component over
     * (RAIM phase rows ∪ census prior rows).  Every readout says so; it is not
     * the system-wide σ̂₀.
     *
     * Call once per row.  The RAIM walk is keyed on ambiguity columns, so a row
     * touched by two carried ambiguities is visited twice; the caller keeps the
     * de-duplication mask (as it does for the MDB rider).  Double counting would
     * inflate both sums and leave the ratio plausible.
     *
     * @param w           the row weight (1/σ², already in hand as `R.w(row)`).
     * @param v           the post-fit residual v = y − Ax̂ (m).
     * @param r           per-row Baarda redundancy r_i ∈ [0,1]; pass 1.0 with
     *                    @p r_is_approx when no redundancy source is armed.
     * @param r_is_approx @p r is the r ≡ 1 fallback, not a computed redundancy.
     *                    Raises @ref kGlobalApprox for the epoch and the run:
     *                    Σr_data then counts rows, the denominator is too large,
     *                    and σ̂₀_global reads low.
     */
    void addDataRow(double w, double v, double r, bool r_is_approx) {
        if (!global_on_) return;
        if (!std::isfinite(w) || !std::isfinite(v) || !std::isfinite(r)) return;
        if (!(w > 0.0)) return;
        // A redundancy outside [0,1] is not a redundancy: a Baarda r_i can leave
        // the interval only through floating-point noise near its ends, and
        // clamping keeps the denominator monotone in the row count.
        const double rr = (r < 0.0) ? 0.0 : (r > 1.0 ? 1.0 : r);
        const double q  = w * v * v;
        if (!std::isfinite(q)) return;
        global_epoch_.wv2 += q;      global_run_.wv2 += q;
        global_epoch_.r_data += rr;  global_run_.r_data += rr;
        ++global_epoch_.n_data;      ++global_run_.n_data;
        if (r_is_approx) { ++global_epoch_.n_data_approx; ++global_run_.n_data_approx; }
    }

    // ---------------------------------------------------------------------
    //  Readout
    // ---------------------------------------------------------------------
    /// This epoch's ladder for one (family index, label).
    const Ladder& epochLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].epoch[static_cast<std::size_t>(l)];
    }
    /// The run-cumulative ladder for one (family index, label).
    const Ladder& runLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].run[static_cast<std::size_t>(l)];
    }
    /// This epoch's rneg ladder for one (family index, label).
    const Ladder& rnegEpochLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].rneg_epoch[static_cast<std::size_t>(l)];
    }
    /**
     * @brief The run-cumulative rneg ladder for one (family index, label).
     *
     * The rows whose posterior exceeds their prior (post_var > fold_var, so
     * r < 0 before the numerical floor).  They are a real population, the
     * documented coupling-carry case in which the harvested posterior is a
     * conditional variance, but their |t| is not a standardized residual: the
     * dispersion `r·fold_var` is formed from a (prior, posterior) pair that does
     * not describe one fold, so `r` is clamped at @ref kRedundancyFloor and |t|
     * inflates by @f$\sqrt{(p+v)/(\varepsilon p)}@f$.  Pooled with the
     * CLEAN/RESPONDED/FRAME rows, they put an artifact at the top of every ladder
     * and of the run-end "worst |t|" line: on one field run they were 927 of the
     * 957 rows above |t| = 1e4, with a worst |t| of 2.5e8
     * (docs/cards/raim-qc/RNEG-REPARTITION.md).  The σ̂₀_global rider drops them
     * for the same reason (@ref kGlobalRnegDrop), and the label ladders exclude
     * them.
     *
     * Read this class on @ref Ladder::max_abs_e (metres), not on
     * @ref Ladder::max_abs_t: its @ref Ladder::keepsMaxByE is set, so its
     * identity columns follow the |ê| winner.
     */
    const Ladder& rnegRunLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].rneg_run[static_cast<std::size_t>(l)];
    }
    /// rneg rows of this family over the run, summed across labels.
    long long rnegRunRows(int f) const {
        long long n = 0;
        for (const auto& l : state_[static_cast<std::size_t>(f)].rneg_run) n += l.n_rows;
        return n;
    }
    /// The sigma0 frozen for this epoch (1.0 while below warm-up).
    double sigma0(int f) const { return state_[static_cast<std::size_t>(f)].sigma0; }
    /// Is that sigma0 an actual calibration (true) or the warm-up fallback (false)?
    bool   sigma0Warm(int f) const { return state_[static_cast<std::size_t>(f)].warm; }
    /// Forgetting-weighted rows behind the calibration.
    double sigma0Neff(int f) const { return state_[static_cast<std::size_t>(f)].n_eff; }
    /// Undecayed rows fed to the calibration over the run.
    long long sigma0Rows(int f) const { return state_[static_cast<std::size_t>(f)].n_cal_rows; }

    /// Rows of this family over the run, summed across labels (invariant #4:
    /// 0 must read as "ran, saw nothing", never as "never ran").  Includes the
    /// rneg class: a family whose rows were all r_negative still ran, and
    /// reporting it as having seen zero rows would send a reader to check the
    /// carry's arming knob for a condition of the covariance, not the wiring.
    long long runRows(int f) const {
        long long n = 0;
        for (const auto& l : state_[static_cast<std::size_t>(f)].run)      n += l.n_rows;
        for (const auto& l : state_[static_cast<std::size_t>(f)].rneg_run) n += l.n_rows;
        return n;
    }
    /// Rows of the whole census over the run.
    long long runRowsTotal() const {
        long long n = 0;
        for (std::size_t f = 0; f < state_.size(); ++f) n += runRows(static_cast<int>(f));
        return n;
    }
    /// Epochs @ref beginEpoch has advanced (the seam-ran denominator).
    long long epochs() const { return n_epochs_; }
    /// Above-threshold rows over the run.
    long long reportedRows() const { return n_reported_run_; }

    // --- this epoch's aggregates (the epoch-status columns) ------------------
    long long epochRows()      const { return ep_rows_; }
    long long epochRowsResp()  const { return ep_rows_resp_; }
    long long epochRowsFrame() const { return ep_rows_frame_; }
    long long epochReported()  const { return ep_reported_; }
    /// The worst |t| this epoch, excluding the rneg class (see @ref rnegRunLadder).
    double    epochMaxAbsT()   const { return ep_max_abs_t_; }
    /// This epoch's rneg rows (already included in @ref epochRows).
    long long epochRowsRneg()  const { return ep_rows_rneg_; }
    /// The worst |ê| (m) among this epoch's rneg rows, their readable extremum.
    double    epochRnegMaxAbsE() const { return ep_rneg_max_abs_e_; }

    /// Above-threshold rows buffered this epoch (identities for the event tier).
    const std::vector<Hit>& hits() const { return hits_; }
    /// Rows that exceeded the threshold but overflowed @ref kMaxHitsPerEpoch.
    long long hitsDropped() const { return hits_dropped_; }

    // --- the constraint-aware global variance component ----------------------
    /// This epoch's raw sums (both sides).
    const GlobalSums& globalEpochSums() const { return global_epoch_; }
    /// The run-cumulative sums (both sides).
    const GlobalSums& globalRunSums()   const { return global_run_; }
    /// One family's prior-side contribution this epoch (data half always zero).
    const GlobalSums& familyGlobalEpoch(int f) const {
        return state_[static_cast<std::size_t>(f)].g_epoch;
    }
    /// One family's prior-side contribution over the run (data half always zero).
    const GlobalSums& familyGlobalRun(int f) const {
        return state_[static_cast<std::size_t>(f)].g_run;
    }
    /// σ̂₀_global + prior tension for this epoch (the epoch-status columns).
    GlobalResult epochGlobal() const { return computeGlobal(global_epoch_); }
    /// The run-pooled variance component: Σ over every epoch's rows, not a mean
    /// of per-epoch ratios.  The two differ, and this is the estimator.
    GlobalResult runGlobal()   const { return computeGlobal(global_run_); }

    /**
     * @brief Record this epoch's (σ̂₀_global, prior_tension) into the run history.
     *
     * Called once per drained epoch by the emission seam, after every feed.
     * Kept separate from @ref beginEpoch on purpose: the run sums accumulate for
     * every epoch that fed rows (so a withheld epoch is never lost from the
     * pooled estimate), while the per-epoch quantiles below cover the epochs
     * that were actually written out, the population a reader of
     * `network_epoch_status.csv` can check them against.
     */
    void recordEpochGlobal() {
        if (!global_on_) return;
        const GlobalResult g = epochGlobal();
        ++n_global_epochs_;
        if (g.flags & kGlobalApprox)     ++n_global_approx_;
        if (g.flags & kGlobalNoData)     ++n_global_no_data_;
        if (g.flags & kGlobalNoPrior)    ++n_global_no_prior_;
        if (g.flags & kGlobalDegenerate) ++n_global_degenerate_;
        if (!g.formed) return;
        if (hist_s0g_.size() >= kMaxGlobalSamples) { ++hist_dropped_; return; }
        hist_s0g_.push_back(g.sigma0_global);
        if (g.tension_defined) hist_tension_.push_back(g.prior_tension);
    }

    /// Drained epochs the rider formed a result for (formed or not).
    long long globalEpochs()           const { return n_global_epochs_; }
    long long globalEpochsApprox()     const { return n_global_approx_; }
    long long globalEpochsNoData()     const { return n_global_no_data_; }
    long long globalEpochsNoPrior()    const { return n_global_no_prior_; }
    long long globalEpochsDegenerate() const { return n_global_degenerate_; }
    /// Per-epoch samples retained for the quantiles (and those dropped to the cap).
    std::size_t globalSampleCount()    const { return hist_s0g_.size(); }
    std::size_t tensionSampleCount()   const { return hist_tension_.size(); }
    long long   globalSamplesDropped() const { return hist_dropped_; }
    /// Quantile @p q ∈ [0,1] of the per-epoch σ̂₀_global (NaN when empty).
    double globalQuantile(double q)  const { return quantile_(hist_s0g_, q); }
    /// Quantile @p q ∈ [0,1] of the per-epoch prior-tension ratio (NaN when empty).
    double tensionQuantile(double q) const { return quantile_(hist_tension_, q); }

private:
    struct FamilyState {
        std::array<Ladder, kPriorResidualLabels> epoch{};
        std::array<Ladder, kPriorResidualLabels> run{};
        /// The rneg class: rows whose posterior exceeds their prior
        /// (post_var > fold_var ⇒ r < 0 before the floor).  Parallel to
        /// @ref epoch / @ref run and keyed by the same label, but kept out of
        /// them (see @ref rnegRunLadder for why).
        std::array<Ladder, kPriorResidualLabels> rneg_epoch{};
        std::array<Ladder, kPriorResidualLabels> rneg_run{};
        double    sigma0 = 1.0;    ///< frozen at beginEpoch (through the previous epoch).
        bool      warm   = false;  ///< the calibration cleared min_rows.
        double    n_eff  = 0.0;
        long long n_cal_rows = 0;
        GlobalSums g_epoch{};      ///< this epoch's prior-side contribution (the §6 per-family split).
        GlobalSums g_run{};        ///< the run's prior-side contribution.
    };

    /// Nearest-rank quantile over an unsorted sample (copies; run-end only).
    static double quantile_(const std::vector<double>& v, double q) {
        if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
        std::vector<double> s = v;
        const double qq = (q < 0.0) ? 0.0 : (q > 1.0 ? 1.0 : q);
        std::size_t k = static_cast<std::size_t>(qq * static_cast<double>(s.size() - 1) + 0.5);
        if (k >= s.size()) k = s.size() - 1;
        std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k), s.end());
        return s[k];
    }

    /**
     * @brief Numerical floor on the partial redundancy.
     *
     * 10⁶ times tighter than @c NetworkCarryVce::kRedundancyFloor (1e-3), on
     * purpose: the difference is load-bearing.
     *
     * The two classes use @f$r@f$ for different things.  The VCE uses it as a
     * weight in a pooled variance component, where a 1e-3 floor is a harmless
     * regularizer that keeps the mean defined.  The census uses it to form the
     * null dispersion of one row, and
     * @f[ r = 1 - v/p = \frac{p}{p+d} @f]
     * goes to zero when the prior is much tighter than the data, that is, on the
     * stiffest and least checkable carries: the standing invariant #1 population
     * the census inventories.  A 1e-3 floor would inflate @f$D\{\hat e\}@f$ and
     * shrink @f$|t|@f$ by @f$\sqrt{\varepsilon/r}@f$ on the rows that matter
     * most, hiding a stale stiff carry behind the clamp.  Test 1 covers this: an
     * ordinary p = 1e-4 m² carry against d = 1 m² of data has r = 1e-4, an order
     * of magnitude under the VCE's floor.
     *
     * 1e-9 is a floating-point limit, not a modelling choice: @f$1 - v/p@f$ is a
     * difference of near-equal doubles, so its relative precision is
     * @f$\sim \varepsilon_{\text{mach}}/r@f$ (measured 6e-11 at r = 1e-6 and
     * 7e-9 at r = 1e-8, test 1).  At the floor itself about 6 significant digits
     * survive, and one more is lost per decade below it, so a lower floor would
     * admit steadily less precise readings and a higher one would discard
     * genuine ones.  The effect on @f$|t|@f$ is half that relative error, which
     * is negligible at census resolution.  When the floor binds, the row is
     * flagged (@ref RowStat::r_clamped, @ref Ladder::n_r_clamped), so the
     * truncation is auditable.
     */
    static constexpr double kRedundancyFloor = 1e-9;
    /// Floor on t^2 inside the geometric-mean log.
    static constexpr double kTinyT2 = 1e-30;
    /// exp(E[log chi2_1]); see @c NetworkCarryVce::kChi2GeoMean.
    static constexpr double kChi2GeoMean = 0.2807297418;

    std::vector<std::string> families_;
    double  min_rows_;
    double  report_sigma_;
    bool    corroborate_;
    bool    global_on_ = false;   ///< the constraint-aware global variance-component rider.
    NetworkCarryVce inflation_;   ///< the per-family sigma0^2 accumulator (D5 v2's mechanism).
    std::function<PriorResidualEvidence(std::string_view, int, std::string_view)> corrob_fn_;
    std::vector<FamilyState> state_;
    std::vector<Hit> hits_;
    long long hits_dropped_ = 0;
    long long n_epochs_     = 0;
    long long n_reported_run_ = 0;
    long long ep_rows_ = 0, ep_rows_resp_ = 0, ep_rows_frame_ = 0, ep_reported_ = 0;
    long long ep_rows_rneg_ = 0;        ///< of ep_rows_, the rneg class (post_var > fold_var).
    double    ep_max_abs_t_ = 0.0;      ///< excludes the rneg class.
    double    ep_rneg_max_abs_e_ = 0.0; ///< worst |ê| (m) among this epoch's rneg rows.
    double    last_tow_     = -1.0;
    // --- the constraint-aware global variance component ----------------------
    GlobalSums global_epoch_{};   ///< this epoch's four sums, both sides.
    GlobalSums global_run_{};     ///< the run-cumulative four sums, both sides.
    std::vector<double> hist_s0g_;      ///< per-epoch σ̂₀_global (run-end median / p95).
    std::vector<double> hist_tension_;  ///< per-epoch Σwê²/Σwv².
    long long hist_dropped_ = 0;        ///< samples past @ref kMaxGlobalSamples, counted so that a truncated history does not read as a shorter run.
    long long n_global_epochs_ = 0, n_global_approx_ = 0, n_global_no_data_ = 0,
              n_global_no_prior_ = 0, n_global_degenerate_ = 0;
};

// ---------------------------------------------------------------------------
//  The emission seam (network/qc/PriorResidual.cpp)
// ---------------------------------------------------------------------------

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp    (included in the .cpp only)
struct EpochEvents;         // network/driver/Epoch.hpp
struct NetworkRunTraces;    // network/diag/RunTraces.hpp (included in the .cpp only)
struct SyncedEpoch;         // rinex/MultiRinexSync.hpp     (included in the .cpp only)
struct NetStation;          // network/driver/RunTypes.hpp   (included in the .cpp only)

/**
 * @brief Drain this epoch's census into the traces, the events and the counters.
 *
 * Call once per censused epoch, after every family's feed and before the
 * epoch-status row is written: from @ref networkStashStatus, between the pb/cb
 * feed and the L1 trace block.  That is the only point at which all five feeds
 * have run and the epoch-status row has not yet been emitted; called from the
 * driver instead, it would publish the census columns one epoch late.  (For the
 * same reason @ref networkStashCarry is called from that seam rather than from
 * the driver.)
 *
 * Writes the per-(family x label) ladder rows of `network_prior_residual.csv`,
 * the PRIORRES-tier rows of `network_events.csv` (rate-limited per cell by
 * @c network.prior_residual_event_holdoff_ep), the @c ctx.carry.ep_prires_*
 * epoch-status scalars and the @c ctx.counters.prires_* run totals.  It reads
 * nothing the solve can see and changes nothing.
 *
 * @param ctx      Run context (event-holdoff memory, epoch scalars, counters).
 * @param config_  Full configuration (@c network.prior_residual_*).
 * @param log      Main run logger (one-shot degradation warnings).
 * @param traces   Run traces (the CSV + event sinks).
 * @param se       This synced epoch (week/tow stamp).
 * @param stations Station roster; resolves a row's station index to its name.
 * @param prires   The accumulator, drained (its per-epoch buffer is consumed).
 * @param events   The epoch's events; written: status (the census scalars the epoch-status row reads).
 */
void networkPriorResidualEmit(NetworkRunContext& ctx, const GnssConfig& config_,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkRunTraces& traces, const SyncedEpoch& se,
                              const std::vector<NetStation>& stations,
                              NetworkPriorResidual& prires, EpochEvents& events);

/**
 * @brief Run-end readout of the census (called by its row of the run-end table, the
 *        overload taking the view below, under @c ctx.knobs.prires_on).
 *
 * Standing invariant #4 applied to the ladders: a family with @c n_rows = 0
 * must read as "ran, saw nothing" and not as "never ran", so the readout
 * reports the epoch denominator first and the ladders against it.
 */
void networkPriorResidualRunEnd(const NetworkRunContext& ctx,
                                const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations,
                                const NetworkPriorResidual& prires);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the prior-residual census.
 *
 * The table's row for the prior-residual census; the report is the overload above.
 *
 * Gate: prints under `ctx.knobs.prires_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkPriorResidualRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

struct NetworkRun;

/**
 * @brief Installs the prior-residual census's corroboration resolver when corroboration is armed.
 *
 * A census arm.  The resolver reads run.engines.slip_qc's evidence per epoch.  Setup builder
 *: called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.engines.prires (its corroborator).
 */
void networkPriorResidualArm(NetworkRun& run);

}  // namespace GnssProcessor
