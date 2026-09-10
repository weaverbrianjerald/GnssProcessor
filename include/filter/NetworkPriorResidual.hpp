#pragma once
/**
 * @file NetworkPriorResidual.hpp
 * @brief PRIOR-RESIDUAL CENSUS — the stale-carry test, generalized off D5
 *        (docs/SPEC_prior_residual_census.md).
 *
 * Measure-only, default-OFF, NOT fingerprinted (forward-only readout; no stored
 * state, no solve effect — the redundancy-census / rhs_attrib precedent), so a
 * shared-snapshot replay leg with only this armed stays A/B-legal against an
 * unarmed control.  It adds NO gate, NO deweight, NO exclusion and NO refloat:
 * @c prior_residual_report_sigma is a TRACE threshold and nothing reads it back.
 *
 * ## The statistic
 *
 * Standing invariant #1 requires every zero-q / stiff time-linked quantity to
 * have a wired slip/jump/event response.  Today that layer is PER-MECHANISM (MW
 * / LLI / IF-TD on the observables, D5 on the DD chords, jump tests on the code
 * bias, @c eq34_reref on the pin displacement) and nothing tells us which of the
 * REMAINING stiff priors is carrying a stale value right now.
 *
 * NGS LASER Guide v2.7 slide 5-74 gives the general statistic.  In the GMM with
 * stochastic constraints @f$z_0 = K\xi + e_0@f$ the residual of prior
 * information and its dispersion are
 * @f[ \tilde e_0 = z_0 - K\hat\xi, \qquad
 *     \hat D\{\tilde e_0\} = \hat\sigma_0^2 P_0^{-1}(I_l + P_0 K N^{-1}K^\top)^{-1}. @f]
 * Every carried prior we fold IS a stochastic-constraint row.  Specialize to one
 * scalar constraint — prior variance @f$p@f$, data-only variance
 * @f$d = KN^{-1}K^\top@f$, posterior variance @f$v = pd/(p+d)@f$ — and
 * @f[ D\{\hat e\} = \frac{p^2}{p+d} = r\cdot\text{fold\_var},\qquad r = 1 - wv, @f]
 * which is EXACTLY the quantity @ref NetworkCarryVce::addRow already forms from
 * its @c (post_var_m2, fold_var_m2) arguments (@c NetworkCarryVce.hpp:266).
 * LASER's dispersion and our partial redundancy are the same object.  No new
 * algebra, no new solve, no covariance sweep: this card turns the POOLING of
 * those numbers into a per-row SCREEN and measures its power.
 *
 * ## The `folded` branch is load-bearing — the sign flip is exact, not a convention
 *
 * @verbatim
 * folded   (the inject predicate held; the prior entered this epoch's solve):
 *     D{e} = r*fold_var = fold_var - post_var      residual of prior information
 *     r    = clamp(1 - post_var/fold_var, [eps,1]) partial redundancy (LASER 5-74)
 * unfolded (RESPONDED refloat / FRAME reref — the inject predicate skipped it):
 *     D{e} = fold_var + post_var                   predicted residual (the D5 form)
 *
 *  s = e / sqrt(D{e})        formally standardized residual
 *  t = s / sigma0_f          EMPIRICALLY standardized residual
 * @endverbatim
 *
 * With the fold IN, @f$\mathrm{Cov}(z_0,\hat\xi) = v@f$ (the posterior used the
 * prior), so @f$D\{z_0-\hat\xi\} = p + v - 2v = p - v@f$ — algebraically
 * identical to @f$r\cdot p@f$ under any network coupling.  With the fold OUT the
 * prior and the solve are independent and the variances ADD.  Applying the
 * folded form to an unfolded row is not merely wrong, it is ANTI-CONSERVATIVE by
 * orders of magnitude: a refloated ambiguity has @f$v \gg p@f$, @f$r@f$ slams
 * into the @ref kRedundancyFloor clamp and @f$t@f$ inflates by
 * @f$\sqrt{(p+v)/(\varepsilon p)}@f$ — x3000 for a 1e-4 m² carry against a 1 m²
 * refloat.  Every RESPONDED row would then land in the top ladder bin BY
 * CONSTRUCTION and the positive-control gate would pass vacuously.  Test 10 of
 * @c tests/test_prior_residual_census.cpp keeps that as a NEGATIVE example.
 *
 * ## The critical value is EMPIRICAL, never tabular (D5's binding precedent)
 *
 * D5 (@c project_m1_card4_d5_innovation) is this exact statistic, already built,
 * for exactly one family — the invariant DD chords — and its three-cycle history
 * is this card's design constraint:
 *
 *  - v1 used the raw formal @f$\mathrm{Cov}(v)@f$; the formal sigma measured
 *    10-30x optimistic and the gate needed a hand-tuned k = 10.
 *  - v2 calibrated an LS-VCE @f$\hat\sigma_0@f$, which AUTO-DISCOVERED v1's hand
 *    tuning (em10 @f$\hat\sigma_0@f$ = 2.49 => effective k = 10.0).
 *  - the Jul-30 correction: D5 is NOT firing on slips.  2.1 % of its resets show
 *    a raw MW/GF step against 95 % on the same run's known-discontinuity
 *    control.  A SCALE calibration fixes the typical scale but CANNOT flatten a
 *    non-Gaussian tail: a 4-sigma0 gate still flags ~17 %/epoch where a clean
 *    Gaussian gives 0.006 %.
 *  - v3 corroboration suppressed 97.3 % of trips by requiring positive
 *    raw-observable evidence.
 *
 * Hence: LASER's @f$t(\alpha/2, n-q)@f$ / @f$F(\alpha,1,r)@f$ references assume
 * Gaussian errors, and on this data that assumption is measured false by three
 * orders of magnitude in the tail.  This class reports the EMPIRICAL EXCEEDANCE
 * LADDER per family (@ref ladder) — "alpha" is a quantile position, not a table
 * lookup.  Anyone who reads LASER 5-100/5-102 and reaches for a t-table on this
 * data will rebuild D5 v1.  Test 3 reproduces that failure mode in a unit test
 * so a future reader cannot mistake it for a bug.
 *
 * ## Three populations, never pooled
 *
 * The existing @ref NetworkCarryVce feed deliberately SKIPS contaminated rows.
 * That is correct for estimating a variance component and exactly backwards for
 * a detector: those skips remove precisely the rows a screen exists to find.  So
 * the census runs on EVERY qualifying row and labels it @ref
 * PriorResidualLabel::Clean / @ref PriorResidualLabel::Responded / @ref
 * PriorResidualLabel::Frame, and keeps a ladder per (family x label).  Pooling
 * them is the one thing that would make the output uninterpretable — and it is
 * the current monitor's behaviour.
 *
 * ## The rider: the constraint-aware global variance component (the honest sigma0)
 *
 * @c docs/SPEC_prior_residual_census.md §14, LASER 5-74's OTHER half.  The ladder
 * above tests each carried prior ONE AT A TIME; the rider pools the same rows
 * into the GLOBAL variance component the network has never had:
 * @f[ \hat\sigma_0^2 = \frac{\tilde e^\top P\tilde e + \tilde e_0^\top P_0\tilde e_0}
 *                           {n - m + l} @f]
 * — prior residuals in the numerator, constraints in the redundancy.  The
 * network's within-epoch @f$\hat\sigma_0@f$ is a ROBUST MEDIAN of standardized
 * DATA-row residuals (the @c assess lambda, @c NetworkSolveGlue.cpp:1029); that
 * is the right statistic for RAIM attribution and it stays untouched, but it is
 * structurally BLIND to tension absorbed by the carried priors — the largest
 * constraint population in the solve by orders of magnitude.  The rider adds two
 * running sums per side and one division:
 *
 * @verbatim
 *   data side    sum w*v^2  and  sum r_data     (the post-fit RAIM pool)
 *   prior side   sum w*e^2  and  sum r_prior    (census rows, FOLDED branch ONLY)
 *
 *   sigma0^2_global    = (sum w*v^2 + sum w*e^2) / (sum r_data + sum r_prior)
 *   prior_tension      =  sum w*e^2 / sum w*v^2       <- the headline
 * @endverbatim
 *
 * Both halves are SELF-CONSISTENT by the same identity, which is why the two
 * pools may be added at all: a data row has @f$E[w v^2] = r_i@f$ and a folded
 * prior row has @f$E[w \hat e^2] = w\cdot r\cdot p = r@f$, so each row
 * contributes its own redundancy to BOTH the numerator's expectation and the
 * denominator.  A vanishing-@f$r@f$ row (the stiffest carries) contributes ~0 to
 * each and cannot bias the ratio — the reason the @ref kRedundancyFloor clamp
 * that matters so much for @f$|t|@f$ barely matters here.
 *
 * Slide 5-85's refinement is why the FOLDED restriction is not a detail: for a
 * MINIMAL constraint set @f$n-m+l = n-\mathrm{rk}\,A@f$ — the datum pins need no
 * correction and get none (test @c Laser585MinimalConstraintEquivalence proves
 * the two denominators coincide as the pin tightens).  It is the NON-MINIMAL
 * stochastic rows — the carry families — whose omission biases the global test.
 * An UNFOLDED row is not a constraint in this epoch's solve at all and must move
 * NEITHER sum; mixing them in is the same sign error the `folded` branch above
 * documents.
 *
 * MEASURE-ONLY, and one level deeper than the census: the A4
 * @c raim_withhold_sigma0 guard, every gate and every deweight stay on the
 * MEDIAN sigma0.  Nothing reads @ref GlobalResult back.  It is a SCALE, not a
 * test statistic — it inherits the same heavy-tail reality the ladders measure.
 *
 * ## Isolation / testing
 *
 * Pure numeric leaf class in the @ref NetworkCarryVce / @c NeqAccumulator mould:
 * header-only, no @c ApplicationNetwork coupling, no @c StateManager, no I/O.
 * It takes @ref Input rows and returns ladders; @c tests/test_prior_residual_census.cpp
 * drives it directly with synthetic rows of known truth.  The per-epoch trace /
 * event / aggregate emission lives in @c filter/NetworkPriorResidual.cpp, which
 * is the only translation unit that knows about traces.
 */

#include "filter/NetworkCarryVce.hpp"

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
    Clean     = 0,  ///< Nothing fired.  A high |t| here is a MISS — the standing-invariant-#1 gap this card exists to find.
    Responded = 1,  ///< An independent mechanism already fired (pending reseed / slip refloat / D5 / RAIM exclusion / rx-CB innovation withhold).  THE POSITIVE CONTROL: a statistic with power must show a shifted |t| distribution here.
    Frame     = 2   ///< A datum handover / eq34 re-reference / inflated-q reinit / phase hold.  A frame step, not noise — and an invariant-#7 re-coupling edge whose |t| measures what the edge actually moved.
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
 * @brief INC-3 raw-observable verdict joined onto a census row.
 *
 * Deliberately the SAME tri-state the slip-QC pass already adjudicates
 * (@ref RawSlipVerdict): absence of evidence is never evidence of a slip, and
 * never evidence of its absence either.
 */
enum class PriorResidualCorrob : int {
    NoEvidence = 0,  ///< No adjacent-epoch predecessor row (first sighting / gap / single band), or the slip-QC pass did not run.
    Flat       = 1,  ///< A step COULD be measured and the raw MW/GF level did NOT move.  THE B-CELL: on the DD family this measured 97 %.
    Stepped    = 2   ///< The raw MW/GF level moved beyond the empirical gate — positive raw-observable evidence.
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
 * @brief The raw-observable evidence backing ONE census row's verdict.
 *
 * A carried prior's corroboration DOMAIN is the set of (station, satellite)
 * pairs whose raw MW/GF observables can witness a discontinuity in it:
 *
 *  - `pl` — a per-(station, satellite) ambiguity: the domain is ONE pair.
 *  - `zwd`, `rxcb` — RECEIVER-level: the domain is that station's satellites.
 *  - `pb`, `cb` — SATELLITE-level (no station): the domain is the stations that
 *    measured that satellite this epoch.
 */
struct PriorResidualEvidence {
    int n_measured = 0;  ///< domain members with an adjacent-epoch predecessor (a step COULD be seen).
    int n_stepped  = 0;  ///< of those, members whose raw level moved beyond the empirical gate.
};

/**
 * @brief THE CONSENSUS RULE — a multi-member domain votes; it does not OR.
 *
 * ## Why an OR is wrong, and why it is wrong in a ROSTER-DEPENDENT way
 *
 * v1 resolved a multi-member domain by asking "did ANY member step?".  That is
 * defensible at 63 stations and DEGENERATE at 303, measured on the same data the
 * same day: `cb` rows reading `raw_stepped` went 29 % → **70 %** and the b-cell
 * (the card's headline INC-3 number) collapsed 57 % → **21 %**.  Nothing about
 * the physics changed between those runs.  With three hundred receivers,
 * essentially every satellite has *some* station stepping in *any* epoch, so
 * @f$P(\text{any of } N) = 1-(1-p)^N \to 1@f$ and the join stops discriminating.
 * A statistic whose value depends on how many receivers happen to be in the
 * roster is not measuring the satellite.
 *
 * ## The rule, and why it needs no threshold
 *
 * The discontinuities these priors can suffer are COMMON-MODE OVER THEIR DOMAIN
 * BY CONSTRUCTION, which is what makes a vote the physically correct reduction
 * rather than a tuning choice:
 *
 *  - a satellite code-/phase-bias jump enters MW (which carries the satellite
 *    code AND phase bias) and GF at **every** station tracking that satellite;
 *  - a receiver reset or firmware bias jump moves **every** satellite at that
 *    station.
 *
 * A single member stepping is therefore evidence about THAT ARC — a receiver-side
 * slip on one line of sight — and is precisely the noise the OR was integrating
 * over an ever-growing domain.  So:
 *
 * @verbatim
 *   n_measured == 0             -> NoEvidence   (nothing could be seen)
 *   n_measured == 1             -> the member's own verdict  (pl: unchanged)
 *   n_measured >= 2             -> Stepped iff 2*n_stepped > n_measured
 *                                  (STRICT majority), else Flat
 * @endverbatim
 *
 * No threshold, no tuning, roster-independent in expectation, and it degenerates
 * EXACTLY to v1's per-arc behaviour on a one-member domain — so `pl`, the family
 * the rule was never wrong for, is bit-for-bit unaffected.
 *
 * ## Read the b-cell with the audit columns, not on its own
 *
 * This rule can only move rows Stepped → Flat, and the b-cell counts `Flat`
 * among flagged rows, so it can only RAISE the number the card reports as its
 * finding.  That is a correctness fix, not evidence — and it would be circular to
 * present the resulting b-cell without showing how much the rule moved.  Hence
 * @ref Ladder::n_corrob_or_stepped (what the saturating OR would have said) and
 * @ref Ladder::sum_corrob_domain (the mean domain size) are carried beside every
 * contingency table, in the trace and in the run-end line.  A b-cell quoted
 * without them is not auditable.
 */
inline PriorResidualCorrob priorResidualConsensus(const PriorResidualEvidence& ev) {
    if (ev.n_measured <= 0) return PriorResidualCorrob::NoEvidence;
    if (ev.n_measured == 1)
        return ev.n_stepped > 0 ? PriorResidualCorrob::Stepped
                                : PriorResidualCorrob::Flat;
    return (2 * ev.n_stepped > ev.n_measured) ? PriorResidualCorrob::Stepped
                                              : PriorResidualCorrob::Flat;
}

/// v1's saturating OR — retained ONLY as the audit baseline the run-end line
/// reports the consensus against.  Never used for a contingency cell.
inline PriorResidualCorrob priorResidualOrVerdict(const PriorResidualEvidence& ev) {
    if (ev.n_measured <= 0) return PriorResidualCorrob::NoEvidence;
    return ev.n_stepped > 0 ? PriorResidualCorrob::Stepped
                            : PriorResidualCorrob::Flat;
}

/**
 * @brief Per-row prior-residual census over the carried stiff priors (v1).
 *
 * One instance per producer run, mirroring @ref NetworkCarryVce's lifetime and
 * ownership (a run-TU local, threaded by reference into the feed seams — see the
 * "engine objects stay run-TU locals" note in @c app/NetworkRunContext.hpp).
 */
class NetworkPriorResidual {
public:
    // ---------------------------------------------------------------------
    //  The exceedance ladder — INC-1, the card's headline product
    // ---------------------------------------------------------------------
    /// Number of ladder rungs.
    static constexpr int kLadderBins = 11;

    /**
     * @brief The fixed |t| ladder.
     *
     * FIXED, not configurable, and deliberately log-spaced past 4: the question
     * this answers is "what would a k*sigma0 gate on this family actually COST,
     * before anyone wires a response to it?", and that is only comparable across
     * families and across runs if the rungs never move.  A family whose 4-sigma0
     * bin holds 17 %/epoch is telling you exactly what D5's did.
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
        long long n_raw_formal= 0;   ///< of those, rows reported RAW FORMAL (sigma0 below warm-up, or a non-reconstructible fold variance) — t == s, flagged.
        long long n_r_clamped = 0;   ///< folded rows whose partial redundancy hit the numerical [eps,1] floor — its |t| is TRUNCATED (read low), never inflated.
        /// Rows with post_var > fold_var, i.e. r < 0 — the documented
        /// coupling-carry case (NetworkCarryVce.hpp:245), where the harvested
        /// posterior is a CONDITIONAL variance that can legitimately exceed the
        /// prior.  A prior/posterior pair that does not describe ONE fold.
        ///
        /// ⚠ ALWAYS 0 on a CLEAN/RESPONDED/FRAME ladder since the rneg
        /// RE-PARTITION: such rows no longer enter the label ladders at all
        /// (see @ref rnegRunLadder).  The column is retained so existing
        /// readers of network_prior_residual.csv keep parsing; the population
        /// is in the `rneg_*` columns at the END of the row.  On an rneg
        /// ladder it equals @ref n_rows by construction.
        long long n_r_negative = 0;
        long long n_no_sigma0 = 0;   ///< rows deliberately withheld from the sigma0 calibration (null dispersion undefined — the ONE admissible censoring).
        std::array<long long, kLadderBins> bins{};  ///< CUMULATIVE exceedance: bins[k] = #rows with |t| > ladder()[k].
        double    sum_log_t2  = 0.0; ///< sum log(t^2) for the tail-robust geometric mean.
        long long n_geo       = 0;   ///< rows in @ref sum_log_t2.
        // --- worst row's FULL identity (invariant #4: a finding needs a name) --
        double      max_abs_t = 0.0; ///< largest |t| seen.
        double      max_e     = 0.0; ///< its innovation (m).
        double      max_disp  = 0.0; ///< its D{e} (m^2).
        double      max_fold_var = 0.0;
        double      max_post_var = 0.0;
        double      max_gap_s = 0.0;
        /// Largest |ê| (metres) seen by this ladder — the RNEG class's headline,
        /// because |t| is exactly the quantity that is not meaningful there:
        /// its dispersion is formed from a (prior, posterior) pair that does not
        /// describe one fold, so `r` slams into @ref kRedundancyFloor and |t|
        /// inflates by √((p+v)/(εp)).  Kept on every ladder (it costs one
        /// compare) so the two classes are read the same way, but ONLY the rneg
        /// readout quotes it.  The identity fields below follow the |ê| winner
        /// on an rneg ladder and the |t| winner elsewhere — see @ref keepsMaxByE.
        double      max_abs_e = 0.0;
        double      max_tow   = -1.0;///< its epoch (GPS TOW s); < 0 = never set.
        int         max_station = -1;///< its app station index (-1 = shared / none).
        std::string max_sat;         ///< its satellite id (empty = none).
        std::string max_signal;      ///< its signal / key (empty = none).
        const char* max_mech = "";   ///< the mechanism tag that labelled it.
        /// INC-3 contingency table: [0] = |t| > report_sigma, [1] = |t| <= it;
        /// columns follow @ref PriorResidualCorrob.  All-zero unless corroboration
        /// is armed.  The b-cell (row 0, Flat) is the number that matters.
        std::array<std::array<long long, kPriorResidualCorrobs>, 2> contingency{};
        /// INC-3 AUDIT (see @ref priorResidualConsensus).  The consensus rule can
        /// only move rows Stepped -> Flat and therefore only RAISE the b-cell, so
        /// a b-cell presented without these two is not auditable.
        long long n_corrob_or_stepped = 0;  ///< rows v1's saturating OR would have called Stepped (>= the consensus count, by construction).
        long long n_corrob_stepped    = 0;  ///< rows the CONSENSUS called Stepped — the number actually used in @ref contingency.
        long long sum_corrob_domain   = 0;  ///< sum of domain sizes (n_measured); / n_rows = the MEAN domain, the quantity that used to grow with the roster.
        long long n_corrob_rows       = 0;  ///< rows carrying any evidence at all (the denominator for the mean domain).
        /// The HIGHEST stepped fraction any domain reached, over rows carrying
        /// evidence.  This is the SENSITIVITY number, and it is what turns "the
        /// consensus fired N times" from an unfalsifiable statement into a
        /// measured one: the rule's bar is 0.5, so a run whose maximum fraction
        /// is (say) 0.08 tells you the bar was never approached — the rule is not
        /// merely strict there, it is OUT OF RANGE of anything the data
        /// produced, and a zero consensus count is uninformative about whether a
        /// real common-mode event would have been caught.  A maximum near or
        /// above 0.5 says the bar sits inside the observed distribution and the
        /// count means what it appears to mean.
        double    max_corrob_frac     = 0.0;
        /// True on the RNEG ladders: the identity fields track the largest |ê|
        /// rather than the largest |t|.  Set once at construction of the family
        /// state and never by a row, so the two classes cannot swap ordering
        /// mid-run.
        bool      keepsMaxByE = false;

        /// @return chi2-corrected geometric mean of t^2 (1.0 = the empirical
        /// scale calibration has converged).  Test 3's point: this can sit at 1
        /// while the 4-sigma bin is grossly over-occupied — a scale calibration
        /// cannot flatten a non-Gaussian tail.
        double geoT2() const {
            if (n_geo <= 0) return 0.0;
            return std::exp(sum_log_t2 / static_cast<double>(n_geo)) / kChi2GeoMean;
        }
        /// Mean corroboration-domain size (0 when the join was not armed).  The
        /// roster-dependence detector: this is what grew 1 -> ~300 on the shared
        /// families and made v1's OR saturate.
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
        /// Clear every accumulator.  @ref keepsMaxByE is CLASS IDENTITY, not
        /// content, so it survives — an epoch reset that flipped an rneg ladder
        /// back to |t| ordering would silently change what its identity columns
        /// mean from epoch 2 onward.
        void reset() { const bool by_e = keepsMaxByE; *this = Ladder{}; keepsMaxByE = by_e; }
    };

    /// What @ref addRow computed for one row (returned so the caller can trace
    /// it; the caller is NEVER expected to act on it — measure-only).
    struct RowStat {
        bool   accepted   = false;  ///< The row entered a ladder (family requested + inputs finite).
        bool   folded     = false;  ///< Which dispersion branch was used.
        bool   raw_formal = false;  ///< t == s, because sigma0 is below warm-up or the fold variance was not reconstructible.
        bool   r_clamped  = false;  ///< The partial redundancy hit the numerical [eps,1] floor (|t| truncated LOW).
        bool   r_negative = false;  ///< post_var > fold_var (r < 0 before the floor) — the coupling-carry case, not a stiff prior.
        bool   reported   = false;  ///< |t| exceeded @ref reportSigma (a TRACE threshold — nothing acts on it).
        double disp       = 0.0;    ///< D{e} (m^2).
        double r          = 0.0;    ///< partial redundancy (folded branch only; 0 on the `+` branch).
        double s          = 0.0;    ///< formally standardized residual.
        double t          = 0.0;    ///< empirically standardized residual.
        double sigma0     = 1.0;    ///< the per-family sigma0 actually used (frozen at @ref beginEpoch).
        int    family     = -1;     ///< resolved family index (-1 = not requested).
        PriorResidualCorrob corrob_used = PriorResidualCorrob::NoEvidence;  ///< INC-3 verdict joined (NoEvidence when corroboration is off).
    };

    /// One row of the census, as handed over by a feed site.
    struct Input {
        std::string_view family;     ///< "pl" / "pb" / "cb" / "zwd" / "rxcb"; ignored when not requested.
        std::string_view sat;        ///< satellite id ("G10"); empty for a station-level prior.
        std::string_view signal;     ///< signal / carry key; empty when not applicable.
        int    station   = -1;       ///< app station index; -1 for a SHARED (satellite-level) prior.
        double e         = 0.0;      ///< the residual of prior information, ê = carried prior − solved (m).
        double fold_var  = 0.0;      ///< the prior variance the mechanism ACTUALLY used, or would have used (m^2).
        double post_var  = 0.0;      ///< the posterior variance of that state after the solve (m^2).
        double gap_s     = 0.0;      ///< arc / carry gap the fold variance was inflated over (s).
        bool   folded    = true;     ///< Did the prior enter this epoch's solve?  DECIDES THE DISPERSION BRANCH — see the file banner.
        PriorResidualLabel label = PriorResidualLabel::Clean;
        /// Feed this row's `s` into the per-family sigma0 accumulator?  TRUE for
        /// every ordinary row INCLUDING above-threshold ones — D5 v2's note
        /// ("feeding above-threshold rows into sigma0 is NECESSARY, not merely
        /// safe; censoring biases sigma0 low, and under misspecification the
        /// flags ARE the calibration signal") applies unchanged.  The ONE
        /// admissible false is a row whose NULL DISPERSION IS UNDEFINED (a
        /// `phase_hold` pinned posterior; a fold variance the site could not
        /// reconstruct) — excluded because the null does not exist, never
        /// because the value is large.
        bool   calibrate = true;
        /// The fold variance is a NOMINAL recompute the mechanism did not
        /// actually use.  Reports the row RAW FORMAL (t == s, flagged) rather
        /// than standardizing it by a variance that never existed, and implies
        /// @c calibrate = false.
        bool   var_nominal = false;
        PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;  ///< INC-3; resolved by @ref setCorroborator when armed.
        const char* mech = "";       ///< mechanism tag for the trace ("reseed", "eq34_reref", "inflate_q", "phase_hold", "handover", "gap", ...).
    };

    /// A row that exceeded @ref reportSigma, buffered for the emission seam.
    /// Bounded per epoch (@ref kMaxHitsPerEpoch) — a census must never be able
    /// to allocate an unbounded per-epoch buffer on a bad day.
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

    /// Per-epoch buffer cap for @ref Hit (beyond it rows are still counted in
    /// every ladder — only their INDIVIDUAL identity is dropped, and the drop is
    /// counted in @ref hitsDropped so a truncated event stream never reads as a
    /// quiet epoch).
    static constexpr std::size_t kMaxHitsPerEpoch = 512;

    // ---------------------------------------------------------------------
    //  The constraint-aware global variance component
    //  (docs/SPEC_prior_residual_census.md §14) — the rider on this accumulator
    // ---------------------------------------------------------------------

    /**
     * @brief The four sums LASER 5-74's global form needs, plus the exclusion
     *        counts that make them auditable.
     *
     * Two of the three exclusion counters exist because the alternative is a
     * SILENTLY biased ratio: a row admitted to the numerator but not the
     * denominator (or vice versa) shifts @f$\hat\sigma_0^2@f$ with nothing in
     * the output saying so.  Every skip is counted and surfaced as a flag bit.
     */
    struct GlobalSums {
        // --- data side: the post-fit RAIM pool ------------------------------
        double    wv2     = 0.0;   ///< Σ w·v² over the visited data rows (each row counted ONCE — see @ref addDataRow).
        double    r_data  = 0.0;   ///< Σ r_i over those same rows (Baarda per-row redundancy, or 1 when unavailable).
        long long n_data  = 0;     ///< rows behind the two sums above.
        long long n_data_approx = 0; ///< of those, rows counted with r ≡ 1 because `network.raim_redundancy_normalize` is off — there is NO honest Σr_data then, and the flag says so in-band.
        // --- prior side: the census's own folded rows -----------------------
        double    we2     = 0.0;   ///< Σ w·ê² = Σ ê²/fold_var (LASER's ẽ₀ᵀP₀ẽ₀).
        double    r_prior = 0.0;   ///< Σ r over those same rows (the partial redundancy the census already forms).
        long long n_prior = 0;     ///< rows behind the two sums above.
        long long n_prior_unfolded = 0;///< census rows SKIPPED: the prior did not enter this epoch's solve, so it is not a constraint and moves neither sum.
        long long n_prior_nominal  = 0;///< census rows SKIPPED: their fold variance is a nominal recompute the mechanism never used, so w is a fiction (the `phase_hold` population).
        long long n_prior_rneg     = 0;///< census rows SKIPPED: post_var > fold_var, i.e. a (prior, posterior) pair that does not describe ONE fold.  Its numerator is real and its redundancy is not, and adding one without the other is exactly the bias this card exists to remove.
        void reset() { *this = GlobalSums{}; }
    };

    /// In-band degradation bits on @ref GlobalResult::flags.  Standing invariant
    /// #4's convention: a degraded number SAYS SO in the same row it is written,
    /// never silently.
    enum GlobalFlag : int {
        kGlobalApprox     = 1,  ///< ≥1 data row entered with r ≡ 1: Σr_data is a ROW COUNT, not a redundancy, and σ̂₀_global reads LOW.
        kGlobalNoData     = 2,  ///< zero data rows (RAIM did not run this epoch): the number is PRIOR-ONLY and is not the LASER global.
        kGlobalNoPrior    = 4,  ///< zero folded census rows: the number is DATA-ONLY, i.e. exactly the quantity the card exists to correct.
        kGlobalDegenerate = 8,  ///< Σr ≤ 0 or non-finite ⇒ σ̂₀²_global NOT FORMED; the column is blank, never 0.
        kGlobalRnegDrop   = 16  ///< ≥1 folded row dropped for post_var > fold_var (@ref GlobalSums::n_prior_rneg).
    };

    /// The formed statistic for one accumulation window (an epoch, a family's
    /// prior half, or the whole run).  Pure function of a @ref GlobalSums.
    struct GlobalResult {
        bool   formed = false;        ///< σ̂₀²_global is defined (Σr > 0 and finite).
        double sigma0_global   = 0.0; ///< σ̂₀ — the SQRT, which is what the column carries (comparable to the median σ̂₀ it is measured against).
        double sigma0_global_v = 0.0; ///< σ̂₀² — the variance component itself.
        double prior_tension   = 0.0; ///< Σwê² / Σwv² — "how much of the epoch's tension lives in the carry".  THE HEADLINE.
        bool   tension_defined = false;///< false when there is no data side to divide by (∞ is not a number this writes).
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

    /// Per-epoch (σ̂₀_global, prior_tension) history cap.  262144 epochs is ~91
    /// days at 30 s; past it the sample is DROPPED and COUNTED, so a run-end
    /// median can never quietly become a median of the first N epochs.
    static constexpr std::size_t kMaxGlobalSamples = 262144;

    /**
     * @brief Construct the census.
     * @param families      families to accumulate (@c NetworkCarryVce::parseComponents
     *                      of @c network.prior_residual_families).  Empty ⇒ inert.
     * @param window_epochs sigma0 exponential-forgetting half-life in epochs
     *                      (<= 0 ⇒ whole-run accumulation).
     * @param min_rows      sigma0 warm-up: below this effective row count the
     *                      family reports t == s and SAYS SO (D5 v2's 200).
     * @param report_sigma  per-row trace/event threshold in sigma0 units.  A
     *                      REPORTING threshold — nothing in the solve reads it.
     * @param corroborate   INC-3: join every row to the raw-observable verdict.
     * @param global_sigma0 The constraint-aware global variance component rider
     *                      (@c network.prior_residual_global_sigma0).  OFF ⇒ the
     *                      four sums are never touched, @ref addDataRow is a
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
          // The sigma0 accumulator, EXACTLY as D5 v2 does it (verbatim precedent:
          // NetworkInvariantCarryDriver.cpp:1453-1455) — a NetworkCarryVce fed the
          // STANDARDIZED value with unit fold variance, so Estimate::robust_floor
          // reads back as sigma0^2.  split_by_const = FALSE: the ladder's unit is
          // the FAMILY, and a per-constellation split would quarter every
          // population and delay warm-up fourfold for no question anyone is
          // asking.  seed_floor = 1 so a family with no rows reads sigma0 = 1.
          inflation_(families_, window_epochs, /*split_by_const=*/false,
                     /*seed_floor_m2=*/1.0, /*seed_rate_m2ps=*/0.0) {
        state_.resize(families_.size());
        for (auto& st : state_) {
            st.sigma0 = 1.0;
            // The RNEG class orders its identity columns by |ê| (metres), not by
            // |t| — see rnegRunLadder.  Stamped ONCE, here, and preserved by
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
    /// Is the constraint-aware global variance component armed?  The ONE
    /// predicate the RAIM data-side feed tests per row.
    bool   globalOn()      const { return global_on_ && enabled(); }

    /**
     * @brief Install the INC-3 raw-observable verdict resolver.
     *
     * Injected rather than plumbed so the accumulator stays a pure numeric leaf
     * (its unit test installs its own, or none) and so the five feed sites do
     * not each have to grow a @c SlipQcChannel parameter.  Called at most once
     * per run; never called when @c corroborate is off.
     *
     * @param fn (family, station index, satellite) -> verdict.  @p station is -1
     *           for a SHARED prior, in which case the resolver is expected to OR
     *           over the stations that saw that satellite this epoch.
     */
    void setCorroborator(
        std::function<PriorResidualEvidence(std::string_view, int, std::string_view)> fn) {
        corrob_fn_ = std::move(fn);
    }

    /**
     * @brief Advance to a new epoch: FREEZE this epoch's per-family sigma0, clear
     *        the per-epoch ladders, and apply forgetting to the calibration.
     *
     * PREDICT-THEN-UPDATE, the first of D5 v2's two load-bearing ordering
     * contracts: every row of epoch N is standardized against the
     * through-epoch-(N-1) sigma0 frozen here, and the rows themselves only reach
     * the accumulator afterwards.  Otherwise a row calibrates against itself and
     * a single huge row standardizes itself away (test 4).
     *
     * The census passes @c clean = true to the calibration UNCONDITIONALLY,
     * unlike @ref NetworkCarryVce::beginEpoch: a datum-changed epoch is exactly a
     * FRAME population to MEASURE, not one to discard.
     *
     * @param content_time_s epoch content time (continuous GPS s).
     * @param tow            epoch GPS TOW (s) — stamps the worst-row identity.
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
            // DELIBERATELY NOT clamped to [1, cap] the way D5's gate clamps it.
            // D5 may only RAISE its threshold because it ACTS; a census that
            // clamped would report a calibrated-optimistic family (sigma0 < 1) as
            // if it were calibrated, which is the measurement.
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
     * reconstruct, no sweep — @c post_var and @c fold_var are already in hand at
     * every feed site, which is what separates this card's cost from the
     * redundancy census's measured 6.1 ms per station-sweep.
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
            disp = in.fold_var + in.post_var;            // independent prior and solve: variances ADD
        }
        if (!(disp > 0.0) || !std::isfinite(disp)) return out;

        FamilyState& st = state_[static_cast<std::size_t>(fi)];
        const double s = in.e / std::sqrt(disp);
        if (!std::isfinite(s)) return out;
        // A row whose fold variance is a nominal recompute the mechanism did not
        // use has no trustworthy scale, so it is reported RAW FORMAL and kept out
        // of the calibration (its `s` would corrupt sigma0 for every other row).
        const bool raw_formal = !st.warm || in.var_nominal;
        const double sigma0   = raw_formal ? 1.0 : st.sigma0;
        const double t        = s / sigma0;
        const double at       = std::abs(t);

        // --- the ladder (INC-1), re-partitioned by r_negative -----------------
        // An r_negative row's |t| is NOT a standardized residual: its dispersion
        // came from a (prior, posterior) pair that does not describe one fold,
        // so r hits kRedundancyFloor and |t| inflates by √((p+v)/(εp)) — the
        // x3000 artifact the file banner already names for the wrong-branch
        // case, arriving here through a different door.  Measured on
        // runs/fldf3_pl2: 927 of the 957 rows above |t| = 1e4 are r_negative,
        // and the run-end "worst |t| of the run" read 2.5e8 off one of them.
        // The σ̂₀_global path has always dropped them (kGlobalRnegDrop); the
        // ladder now does the same, into its OWN class rather than into the
        // bin-counted CLEAN/RESPONDED/FRAME population, so both are readable.
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
        // sum_log_t2 / n_geo are the geometric mean of t² — the SCALE-calibration
        // convergence readout.  An rneg row's t² is not on that scale, so it
        // accumulates in the rneg class's own sums (where geoT2 is reported as a
        // property of that class) and never in the label class's.
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
            // The RULE lives in priorResidualConsensus (pure, unit-tested); the
            // resolver only gathers the DOMAIN COUNTS.  Separating them is what
            // makes the roster-independence claim checkable without a network.
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

        // The identity of the worst row.  The ORDERING key differs by class:
        // |t| on a label ladder (the statistic), |ê| on an rneg ladder (metres —
        // the only quantity there that means anything, since the dispersion is
        // undefined).  Both extrema are tracked on both classes so the columns
        // line up; only the ordering key differs, and it is fixed per ladder.
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

        // --- the sigma0 calibration (UPDATE, after the standardization) -------
        // NEVER censor by threshold.  The only rows withheld are those whose null
        // dispersion is undefined; they are counted so the exclusion is auditable.
        if (in.calibrate && !in.var_nominal) {
            inflation_.addRow(families_[static_cast<std::size_t>(fi)], std::string(),
                              s, /*gap_s=*/0.0, /*post_var=*/0.0, /*fold_var=*/1.0);
        } else {
            ++L.n_no_sigma0; ++R.n_no_sigma0;
        }

        // --- per-epoch aggregates + the bounded identity buffer ---------------
        // ep_rows_ stays the TOTAL (label classes + rneg) so the epoch-status
        // row still closes against the ladder counts; ep_max_abs_t_ — which the
        // `prires_max_t` column and the run-end "worst |t|" line are built from
        // — excludes the rneg class, for the reason at the ladder above.
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
        // docs/SPEC_prior_residual_census.md §14.2.  FOLDED rows only: an unfolded
        // prior is not a stochastic constraint in THIS epoch's solve, so it is
        // neither in LASER's ẽ₀ᵀP₀ẽ₀ nor in its `l`.  Three skips, each counted
        // (a silently one-sided row biases the ratio with nothing saying so):
        //  * unfolded   — not a constraint this epoch;
        //  * var_nominal— the fold variance is a recompute the mechanism never
        //                 used, so w = 1/fold_var is a fiction and BOTH sums
        //                 would be wrong (this is also the `phase_hold`
        //                 population, whose null dispersion is undefined);
        //  * r_negative — post_var > fold_var is a (prior, posterior) pair that
        //                 does not describe one fold.  Its w·ê² is a real
        //                 weighted residual and its redundancy is not, and
        //                 admitting the numerator without the denominator is
        //                 precisely the bias this card exists to remove.
        // The merely r-FLOORED row (0 < r < ε, the stiffest carries) is KEPT: it
        // contributes ~0 to both sums by construction, because E[w·ê²] = r.
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
     * @brief Fold one DATA row into the global variance component's data side.
     *
     * The rider's other half (docs/SPEC_prior_residual_census.md §14.2).  Called
     * from the post-fit RAIM Pass-1 row walk, which already holds @p w and — with
     * `network.raim_redundancy_normalize` — the per-row Baarda redundancy
     * @p r, so this costs a multiply and two adds and needs no covariance access,
     * no reconstruct and no sweep.
     *
     * ⚠ THE POOL IS THE RAIM POOL, NOT THE WHOLE SYSTEM.  That walk visits the
     * PHASE rows of CARRIED AMBIGUITIES at non-pivot stations — not the code
     * rows, not the constraint rows, not the stations RAIM skips.  So
     * @f$\sum r_{data}@f$ here is NOT @f$n - \mathrm{rk}\,A@f$ of the network,
     * and @f$\hat\sigma_0^2@f$ is a variance component over
     * (RAIM phase rows ∪ census prior rows).  Every readout says so; quoting it
     * as the system-wide σ̂₀ would be wrong.
     *
     * ⚠ ONE ROW, ONE CALL.  The RAIM walk is keyed on AMBIGUITY COLUMNS, so a row
     * touched by two carried ambiguities is visited twice; the caller carries the
     * de-duplication mask (the same reason the MDB rider carries one).  Double
     * counting would inflate BOTH sums and leave the ratio plausible.
     *
     * @param w           the row weight (1/σ², already in hand as `R.w(row)`).
     * @param v           the post-fit residual v = y − Ax̂ (m).
     * @param r           per-row Baarda redundancy r_i ∈ [0,1]; pass 1.0 with
     *                    @p r_is_approx when no redundancy source is armed.
     * @param r_is_approx @p r is the r ≡ 1 fallback, not a computed redundancy.
     *                    Raises @ref kGlobalApprox for the epoch and the run:
     *                    Σr_data is then a ROW COUNT, the denominator is too
     *                    large and σ̂₀_global reads LOW.
     */
    void addDataRow(double w, double v, double r, bool r_is_approx) {
        if (!global_on_) return;
        if (!std::isfinite(w) || !std::isfinite(v) || !std::isfinite(r)) return;
        if (!(w > 0.0)) return;
        // A redundancy outside [0,1] is not a redundancy — a Baarda r_i can only
        // leave the interval through floating-point noise near its ends, and
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
    /// This epoch's RNEG ladder for one (family index, label).
    const Ladder& rnegEpochLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].rneg_epoch[static_cast<std::size_t>(l)];
    }
    /**
     * @brief The run-cumulative RNEG ladder for one (family index, label).
     *
     * The rows whose POSTERIOR EXCEEDS THEIR PRIOR (post_var > fold_var, so
     * r < 0 before the numerical floor).  They are a real population — the
     * documented coupling-carry case, where the harvested posterior is a
     * CONDITIONAL variance — but their |t| is not a standardized residual: the
     * dispersion `r·fold_var` is formed from a (prior, posterior) pair that does
     * not describe one fold, so `r` slams into @ref kRedundancyFloor and |t|
     * inflates by @f$\sqrt{(p+v)/(\varepsilon p)}@f$.  Pooling them with the
     * CLEAN/RESPONDED/FRAME rows put an artifact at the top of every ladder and
     * of the run-end "worst |t|" line (measured on `runs/fldf3_pl2`: 927 of the
     * 957 rows above |t| = 1e4, headline 2.5e8), which is why the σ̂₀_global
     * rider has always dropped them (@ref kGlobalRnegDrop) and why the ladder
     * now does too.
     *
     * Read this class on @ref Ladder::max_abs_e (metres), never on
     * @ref Ladder::max_abs_t — its @ref Ladder::keepsMaxByE is set, so its
     * identity columns follow the |ê| winner.
     */
    const Ladder& rnegRunLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].rneg_run[static_cast<std::size_t>(l)];
    }
    /// RNEG rows of this family over the run, summed across labels.
    long long rnegRunRows(int f) const {
        long long n = 0;
        for (const auto& l : state_[static_cast<std::size_t>(f)].rneg_run) n += l.n_rows;
        return n;
    }
    /// The sigma0 FROZEN for this epoch (1.0 while below warm-up).
    double sigma0(int f) const { return state_[static_cast<std::size_t>(f)].sigma0; }
    /// Is that sigma0 an actual calibration (true) or the warm-up fallback (false)?
    bool   sigma0Warm(int f) const { return state_[static_cast<std::size_t>(f)].warm; }
    /// Forgetting-weighted rows behind the calibration.
    double sigma0Neff(int f) const { return state_[static_cast<std::size_t>(f)].n_eff; }
    /// Undecayed rows fed to the calibration over the run.
    long long sigma0Rows(int f) const { return state_[static_cast<std::size_t>(f)].n_cal_rows; }

    /// Rows of this family over the run, summed across labels (invariant #4:
    /// 0 must read as "ran, saw nothing", never as "never ran").  Includes the
    /// RNEG class: a family whose rows were ALL r_negative still RAN, and
    /// reporting it as "saw ZERO rows" would send a reader to check the carry's
    /// arming knob for a condition that is about the covariance, not the wiring.
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
    /// The worst |t| this epoch, EXCLUDING the RNEG class (see @ref rnegRunLadder).
    double    epochMaxAbsT()   const { return ep_max_abs_t_; }
    /// This epoch's RNEG rows (already included in @ref epochRows).
    long long epochRowsRneg()  const { return ep_rows_rneg_; }
    /// The worst |ê| (m) among this epoch's RNEG rows — their readable extremum.
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
    /// One family's PRIOR-side contribution this epoch (data half always zero).
    const GlobalSums& familyGlobalEpoch(int f) const {
        return state_[static_cast<std::size_t>(f)].g_epoch;
    }
    /// One family's PRIOR-side contribution over the run (data half always zero).
    const GlobalSums& familyGlobalRun(int f) const {
        return state_[static_cast<std::size_t>(f)].g_run;
    }
    /// σ̂₀_global + prior tension for THIS epoch (the epoch-status columns).
    GlobalResult epochGlobal() const { return computeGlobal(global_epoch_); }
    /// The run-POOLED variance component (Σ over every epoch's rows, not a mean
    /// of per-epoch ratios — the two differ, and this is the estimator).
    GlobalResult runGlobal()   const { return computeGlobal(global_run_); }

    /**
     * @brief Record this epoch's (σ̂₀_global, prior_tension) into the run history.
     *
     * Called ONCE per DRAINED epoch by the emission seam, after every feed.
     * Deliberately separate from @ref beginEpoch: the run SUMS accumulate for
     * every epoch that fed rows (so a withheld epoch is never lost from the
     * pooled estimate), while the per-epoch QUANTILES below are over the epochs
     * that were actually written out — which is the population a reader of
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
        /// The RNEG class: rows whose posterior EXCEEDS their prior
        /// (post_var > fold_var ⇒ r < 0 before the floor).  Parallel to
        /// @ref epoch / @ref run and keyed by the same label, but kept OUT of
        /// them — see @ref rnegRunLadder for why.
        std::array<Ladder, kPriorResidualLabels> rneg_epoch{};
        std::array<Ladder, kPriorResidualLabels> rneg_run{};
        double    sigma0 = 1.0;    ///< frozen at beginEpoch (through the PREVIOUS epoch).
        bool      warm   = false;  ///< the calibration cleared min_rows.
        double    n_eff  = 0.0;
        long long n_cal_rows = 0;
        GlobalSums g_epoch{};      ///< this epoch's PRIOR-side contribution (the §6 per-family split).
        GlobalSums g_run{};        ///< the run's PRIOR-side contribution.
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
     * @brief NUMERICAL floor on the partial redundancy.
     *
     * DELIBERATELY 10⁶ times tighter than @c NetworkCarryVce::kRedundancyFloor
     * (1e-3), and that difference is load-bearing rather than an oversight.
     *
     * The two use @f$r@f$ for different things.  The VCE uses it as a WEIGHT in
     * a pooled variance component, where a 1e-3 floor is a harmless regularizer
     * that merely keeps the mean defined.  The census uses it to form the NULL
     * DISPERSION of one row, and
     * @f[ r = 1 - v/p = \frac{p}{p+d} @f]
     * goes to zero exactly when the prior is much TIGHTER than the data — i.e.
     * on the stiffest, least checkable carries, which is precisely the standing-
     * invariant-#1 population this card exists to inventory.  A 1e-3 floor would
     * therefore inflate @f$D\{\hat e\}@f$ — and shrink @f$|t|@f$ by
     * @f$\sqrt{\varepsilon/r}@f$ — on exactly the rows that matter most, hiding
     * a stale stiff carry behind a clamp.  (Caught by test 1: a perfectly
     * ordinary p = 1e-4 m² carry against d = 1 m² of data has r = 1e-4, an order
     * of magnitude under the VCE's floor.)
     *
     * 1e-9 is a floating-point limit, not a modelling choice: @f$1 - v/p@f$ is a
     * difference of near-equal doubles, so its RELATIVE precision is
     * @f$\sim arepsilon_{	ext{mach}}/r@f$ — measured 6e-11 at r = 1e-6 and
     * 7e-9 at r = 1e-8 (test 1).  At the floor itself only ~6 significant digits
     * survive, and below it the value is noise, so nothing is gained by going
     * lower and a genuine reading is lost by stopping higher.  The consequence
     * for @f$|t|@f$ is half that relative error, i.e. irrelevant at census
     * resolution.  When the floor binds the row is FLAGGED (@ref
     * RowStat::r_clamped, @ref Ladder::n_r_clamped) so the truncation is
     * auditable instead of silent.
     */
    static constexpr double kRedundancyFloor = 1e-9;
    /// Floor on t^2 inside the geometric-mean log.
    static constexpr double kTinyT2 = 1e-30;
    /// exp(E[log chi2_1]) — see @c NetworkCarryVce::kChi2GeoMean.
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
    long long ep_rows_rneg_ = 0;        ///< of ep_rows_, the RNEG class (post_var > fold_var).
    double    ep_max_abs_t_ = 0.0;      ///< EXCLUDES the RNEG class.
    double    ep_rneg_max_abs_e_ = 0.0; ///< worst |ê| (m) among this epoch's RNEG rows.
    double    last_tow_     = -1.0;
    // --- the constraint-aware global variance component ----------------------
    GlobalSums global_epoch_{};   ///< this epoch's four sums, both sides.
    GlobalSums global_run_{};     ///< the run-cumulative four sums, both sides.
    std::vector<double> hist_s0g_;      ///< per-epoch σ̂₀_global (run-end median / p95).
    std::vector<double> hist_tension_;  ///< per-epoch Σwê²/Σwv².
    long long hist_dropped_ = 0;        ///< samples past @ref kMaxGlobalSamples — a truncated history must never read as a shorter run.
    long long n_global_epochs_ = 0, n_global_approx_ = 0, n_global_no_data_ = 0,
              n_global_no_prior_ = 0, n_global_degenerate_ = 0;
};

// ---------------------------------------------------------------------------
//  The emission seam (filter/NetworkPriorResidual.cpp)
// ---------------------------------------------------------------------------

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp    — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only
struct SyncedEpoch;         // rinex/MultiRinexSync.hpp     — included in the .cpp only
struct NetStation;          // filter/NetworkRunTypes.hpp   — included in the .cpp only

/**
 * @brief Drain this epoch's census into the traces, the events and the counters.
 *
 * Call ONCE per censused epoch, AFTER every family's feed and BEFORE the
 * epoch-status row is written — i.e. from @ref networkStashStatus, between the
 * pb/cb feed and the L1 trace block.  That is the only point at which all five
 * feeds have run and the epoch-status row has not yet been emitted; hosting it
 * in the driver instead would publish the census columns one epoch late.  (The
 * same reason @ref networkStashCarry is called from that seam rather than the
 * driver.)
 *
 * Writes: the per-(family x label) ladder rows of `network_prior_residual.csv`,
 * the PRIORRES-tier rows of `network_events.csv` (rate-limited per cell by
 * @c network.prior_residual_event_holdoff_ep), the @c ctx.carry.ep_prires_*
 * epoch-status scalars, and the @c ctx.counters.prires_* run totals.  Reads
 * nothing the solve can see; changes nothing.
 *
 * @param ctx      Run context (event-holdoff memory, epoch scalars, counters).
 * @param config_  Full configuration (@c network.prior_residual_*).
 * @param log      Main run logger (one-shot degradation warnings).
 * @param traces   Run traces (the CSV + event sinks).
 * @param se       This synced epoch (week/tow stamp).
 * @param stations Station roster — resolves a row's station INDEX to its name.
 * @param prires   The accumulator, drained (its per-epoch buffer is consumed).
 */
void networkPriorResidualEmit(NetworkRunContext& ctx, const GnssConfig& config_,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkRunTraces& traces, const SyncedEpoch& se,
                              const std::vector<NetStation>& stations,
                              NetworkPriorResidual& prires);

/**
 * @brief Run-end readout of the census (called from @c NetworkTeardown).
 *
 * Standing invariant #4, ladder edition: a family with @c n_rows = 0 must read
 * as "ran, saw nothing" and NOT as "never ran", so this reports the epoch
 * denominator FIRST and the ladders against it.
 */
void networkPriorResidualReport(const NetworkRunContext& ctx,
                                const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations,
                                const NetworkPriorResidual& prires);

}  // namespace GnssProcessor
