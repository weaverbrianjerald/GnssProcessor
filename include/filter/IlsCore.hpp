/**
 * @file IlsCore.hpp
 * @brief Pure integer-least-squares (MLAMBDA) core, decoupled from StateManager.
 *
 * Free functions for integer ambiguity resolution that operate only on
 * @c (â cycles, Q cycles²): the Q = Lᵀ·D·L factorization, MLAMBDA lattice
 * reduction, the Schnorr-Euchner integer least-squares search, the bootstrap
 * (FFR) success rate, and the acceptance battery with optional partial-AR
 * backward elimination.
 *
 * These routines do not depend on @c StateManager, @c KalmanFilter,
 * @c GnssConfig or logging.  The kernels and @c selectFixableSubset are shared
 * by the single-station user path (@c AmbiguityResolver) and the PPP-RTK
 * network producer path (@c NetworkEstimator / @c runNetwork); @c ilsFix is
 * called by the network path, while @c AmbiguityResolver runs the same gate
 * sequence inline.  Callers own all state-vector assembly (metres to cycles
 * scaling, per-pair λ lookups, DD formation) and the application of the
 * returned integer fix; this header owns only the integer search and its
 * acceptance gates.
 *
 * Units: MLAMBDA runs in cycles.  On metre-domain floats it rounds to
 * integer-metre values that have no carrier-phase meaning and gives ratios
 * near 1.0 whatever the float quality.  Convert per pair (÷λ) before calling.
 *
 * References:
 *   [1] P.J.G. Teunissen, "The least-squares ambiguity decorrelation
 *       adjustment", J. Geodesy, 70, 65-82, 1995.
 *   [2] X.-W. Chang, X. Yang, T. Zhou, "MLAMBDA: A modified LAMBDA method
 *       for integer least-squares estimation", J. Geodesy, 79, 552-565, 2005.
 */
#pragma once

#include <Eigen/Dense>
#include <vector>

namespace GnssProcessor {
namespace ils {

// ===========================================================================
// MLAMBDA numerical kernels (pure)
// ===========================================================================

/**
 * @brief Factorization Q = Lᵀ·D·L of a positive-definite matrix, D = diag(d).
 *
 * Eliminates from the last row upward, as in the MLAMBDA reference code, so
 * the factor is Lᵀ·D·L rather than L·D·Lᵀ.
 *
 * @param Q  Input covariance matrix (n x n).
 * @param L  Output unit lower-triangular matrix.
 * @param d  Output diagonal vector.
 * @return True on success, false if a pivot d_i ≤ 0 (@p Q not positive definite).
 */
bool ldlDecompose(const Eigen::MatrixXd& Q,
                  Eigen::MatrixXd& L, Eigen::VectorXd& d);

/**
 * @brief MLAMBDA lattice reduction (decorrelation).
 * @param[in,out] L  Lower-triangular matrix (modified in place).
 * @param[in,out] d  Diagonal vector (modified in place).
 * @param[out]    Z  Integer (unimodular) transformation matrix.
 */
void reduction(Eigen::MatrixXd& L, Eigen::VectorXd& d, Eigen::MatrixXd& Z);

/**
 * @brief Integer least-squares search (modified Schnorr-Euchner).
 * @param L   Reduced lower-triangular matrix.
 * @param d   Reduced diagonal vector.
 * @param zs  Float ambiguity in Z-transformed space.
 * @param zn  Output: best @p m integer candidates (n x m).
 * @param sq  Output: squared distances for each candidate (ascending).
 * @param m   Number of candidates to find (default 2 for the ratio test).
 * @return Number of candidates found.
 */
int search(const Eigen::MatrixXd& L, const Eigen::VectorXd& d,
           const Eigen::VectorXd& zs,
           Eigen::MatrixXd& zn, Eigen::VectorXd& sq, int m = 2);

/**
 * @brief Full MLAMBDA: decompose, reduce, search, back-transform.
 * @param a     Float ambiguity vector (cycles).
 * @param Q     Ambiguity covariance matrix (cycles²).
 * @param afix  Output: best integer candidate (cycles).
 * @param s     Output: squared distances [s1, s2] for the ratio test.
 * @param d_out Optional output: post-reduction diagonal D (one variance
 *              per decorrelated ambiguity), fed to
 *              @c computeBootstrapSuccessRate.  Pass nullptr to skip.
 * @return True on success (PD covariance and ≥2 candidates found).
 */
bool mlambda(const Eigen::VectorXd& a, const Eigen::MatrixXd& Q,
             Eigen::VectorXd& afix, Eigen::VectorXd& s,
             Eigen::VectorXd* d_out = nullptr);

/**
 * @brief Bootstrap-rounding success rate for an ILS problem (FFR gate).
 *
 *     P_s,boot = Π_i  erf( 1 / (2·√(2 · d_i)) )
 *
 * A lower bound on the ILS success rate; used as a fixed-failure-rate
 * acceptance criterion.
 *
 * @param d Post-reduction diagonal D of Q = Lᵀ·D·L (positive entries).
 * @return Bootstrap success rate ∈ [0, 1].  Zero if any d_i ≤ 0 or empty.
 */
double computeBootstrapSuccessRate(const Eigen::VectorXd& d);

// ===========================================================================
// Acceptance gates + fixing
// ===========================================================================

/**
 * @brief Acceptance-gate parameters for @c ilsFix / @c selectFixableSubset.
 *
 * Mirrors the relevant fields of @c AmbiguityResolverConfig so the user path
 * and the network path drive the identical battery.  Network defaults are
 * stricter (see @c docs/network_ambiguity_resolution_design.md §5):
 * @c min_success_rate=0.9999, @c nl_max_per_pair_frac=0.10,
 * @c enable_partial_ar=true.
 */
struct IlsGateConfig {
    double ratio_threshold      = 3.0;   ///< Minimum ratio test (s2/s1) for acceptance.
    double ratio_upper_bound    = 1e4;   ///< Reject fixes whose ratio exceeds this (degenerate Q).
    double min_success_rate     = 0.999; ///< Minimum bootstrap success rate (FFR).
    double nl_max_per_pair_frac  = 0.15; ///< Max |float − fixed| (cycles) per entry.
    bool   enable_partial_ar    = false; ///< Retry on fixable subsets when the full set fails.
};

/// Why the full-set fix failed its acceptance battery (for caller diagnostics).
enum class IlsFailReason {
    None,           ///< Full set passed all gates.
    MlambdaFailed,  ///< Covariance not PD or <2 candidates found.
    BadDistances,   ///< s1 ≤ 0 or s2 ≤ 0.
    RatioLow,       ///< ratio < ratio_threshold.
    RatioHigh,      ///< ratio > ratio_upper_bound (degenerate covariance).
    Bootstrap,      ///< bootstrap P_s < min_success_rate.
    FracCap         ///< worst |float − fixed| > nl_max_per_pair_frac.
};

/**
 * @brief Outcome of an integer fix attempt (full set, or partial subset).
 *
 * @c afix is in cycles, ordered to match @c kept (which indexes into the
 * caller's input set).  For a full-set fix @c kept is 0..n-1 and
 * @c partial=false.  The @c full_* fields capture the full-set evaluation
 * even when a partial subset was ultimately accepted, so the caller can log
 * the full-set attempt faithfully.
 */
struct IlsResult {
    bool ok = false;                ///< A fix (full or partial) was accepted.
    bool partial = false;           ///< Accepted via subset (kept.size() < n).
    std::vector<int> kept;          ///< Indices (into the input set) that were fixed.
    Eigen::VectorXd afix;           ///< Integer fix (cycles), in @c kept order.
    double ratio = 0.0;             ///< Ratio of the accepted fix.
    double p_succ = 0.0;            ///< Bootstrap success rate of the accepted fix.
    double max_frac = 0.0;          ///< Worst |float − fixed| (cycles) of the accepted fix.

    // Full-set evaluation diagnostics (valid regardless of partial outcome).
    double full_s1 = 0.0;           ///< Best squared distance of the full-set search.
    double full_s2 = 0.0;           ///< Second-best squared distance of the full-set search.
    double full_ratio = 0.0;        ///< Full-set ratio (s2/s1).
    double full_p_succ = 0.0;       ///< Full-set bootstrap rate (0 if not reached).
    double full_max_frac = 0.0;     ///< Full-set worst |float − fixed| (0 if not reached).
    int    full_worst_idx = -1;     ///< Index of the worst-fractional entry (-1 if not reached).
    IlsFailReason full_fail = IlsFailReason::None;  ///< Why the full set failed (None if it passed).

    // Decorrelated partial fix (@ref ilsFixDecorrelatedPartial only; 0 elsewhere).
    int    z_fixed = 0;             ///< Decorrelated components fixed (0 = none accepted).
    int    z_total = 0;             ///< Decorrelated components available (the input dimension).
    /// Floats conditioned on the fixed decorrelated components (cycles, input order),
    /// and their covariance (cycles²).  Empty unless @c z_fixed > 0.  A caller forms
    /// any combination's conditional value and variance from them (card ARL: the
    /// wide lane and the narrow lane of an L1/L2 pair).
    Eigen::VectorXd a_cond;
    Eigen::MatrixXd Q_cond;
};

struct PartialFix {
    bool success = false;        ///< True if a fixable subset (≥1 entry) was found.
    std::vector<int> kept;       ///< Indices (into the input set) that survived and were fixed.
    Eigen::VectorXd afix_cycles; ///< Integer fix (cycles) for the kept entries, in @c kept order.
    double ratio = 0.0;          ///< Ratio test value of the accepted subset.
    double p_s = 0.0;            ///< Bootstrap success rate of the accepted subset.
    double max_frac = 0.0;       ///< Worst |float − fixed| (cycles) over the accepted subset.
};
/**
 * @brief Greedy backward-elimination partial ambiguity resolution.
 *
 * Starting from the full set, repeatedly runs @c mlambda on the current
 * subset and applies the acceptance battery of the full-set fix: ratio (lower
 * and upper bound), bootstrap success rate, and per-entry |float − fixed| cap.
 * On a failure one entry is dropped (the one with the largest |float − fixed|
 * when the fractional cap is the failing gate, otherwise the one with the
 * largest formal float variance) and the search repeats, down to a single
 * entry.  The accepted subset has passed the whole battery on its own
 * sub-block, so it fixes fewer ambiguities than the full set but meets the
 * same acceptance standard.
 *
 * @param y_cycles  Float ambiguity vector (cycles).
 * @param Q_cycles  Covariance of @p y_cycles (cycles²); symmetric PD.
 * @param gates     Acceptance-gate parameters.
 * @return PartialFix; @c success=false if no subset down to size 1 passes.
 */
PartialFix selectFixableSubset(const Eigen::VectorXd& y_cycles,
                               const Eigen::MatrixXd& Q_cycles,
                               const IlsGateConfig& gates);

/**
 * @brief Fix integer ambiguities from a float estimate and covariance.
 *
 * Runs MLAMBDA on @p a_hat_cycles / @p Q_cycles and applies the full-set
 * acceptance battery (ratio lower/upper → bootstrap FFR → per-entry frac
 * cap).  When the full set fails a gate and @c gates.enable_partial_ar is
 * set, it falls back to @c selectFixableSubset; when MLAMBDA itself fails or
 * the squared distances are not positive, it stays float with no fallback.
 * Pure: no logging and no state mutation.  The returned @c IlsResult carries
 * every value a caller needs to apply the fix and to log the attempt.
 *
 * @param a_hat_cycles Float ambiguity vector (cycles).
 * @param Q_cycles     Covariance (cycles²); symmetric PD.
 * @param gates        Acceptance-gate parameters.
 * @return IlsResult; @c ok=true when a full or partial fix was accepted.
 */
IlsResult ilsFix(const Eigen::VectorXd& a_hat_cycles,
                 const Eigen::MatrixXd& Q_cycles,
                 const IlsGateConfig& gates);

/**
 * @brief Partial fix in the decorrelated space, then the single-signal integers it determines.
 *
 * Card ARL (the integer-ambiguity ladder).  With a free slant ionosphere the
 * uncombined L1/L2 double differences are each imprecise (about 0.3 cycle) while
 * combinations of a pair (the wide lane, the ionosphere-free part) are precise, so
 * a subset chosen entry by entry (@ref selectFixableSubset) finds nothing and the
 * per-entry fraction cap rejects correct fixes.  This routine works on the
 * decorrelated ambiguities instead:
 *
 *  1. Q = Lᵀ·D·L and the MLAMBDA reduction give z = Zᵀ·a with conditional
 *     variances d.  The search fixes z from the last component down, so the
 *     trailing components are fixed first.
 *  2. The largest trailing set whose bootstrap success rate Π erf(1/(2√(2 d_i)))
 *     is at least @c gates.min_success_rate is searched (its covariance is the
 *     trailing block of Lᵀ·D·L, i.e. the trailing L and d).  If its ratio test
 *     fails, the set is shrunk by one component and searched again.
 *  3. The floats are conditioned on the fixed components:
 *     ǎ = â − Q_az·Q_zz⁻¹·(ẑ − ž),  Q_ǎ = Q − Q_az·Q_zz⁻¹·Q_za.
 *  4. An input entry is reported fixed (in @c kept, with @c afix its integer)
 *     when its conditional sigma is at most @p determined_sigma_cyc and its
 *     conditional value lies within @c gates.nl_max_per_pair_frac of an integer:
 *     the fixed combinations determine that single-signal ambiguity.
 *
 * @c z_fixed / @c z_total report the decorrelated fix; @c ok is true when at
 * least one single-signal ambiguity is determined.  Pure: no logging, no state.
 *
 * @param a_hat_cycles         Float ambiguity vector (cycles).
 * @param Q_cycles             Covariance (cycles²); symmetric PD.
 * @param gates                min_success_rate, ratio_threshold, ratio_upper_bound and
 *                             nl_max_per_pair_frac are used; enable_partial_ar is not.
 * @param determined_sigma_cyc Conditional sigma (cycles) at or below which a
 *                             single-signal ambiguity counts as determined.
 * @return IlsResult (see above).
 */
IlsResult ilsFixDecorrelatedPartial(const Eigen::VectorXd& a_hat_cycles,
                                    const Eigen::MatrixXd& Q_cycles,
                                    const IlsGateConfig& gates,
                                    double determined_sigma_cyc);

} // namespace ils
} // namespace GnssProcessor
