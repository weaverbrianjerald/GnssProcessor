#pragma once
/**
 * @file Congruence.hpp
 * @brief Station congruence screen: detection of station-coordinate faults in
 *        position space (docs/SPEC_redundancy_census.md, CG-INC-1..3).  This header
 *        holds the statistic and its bookkeeping.
 *
 * Measure-only, off by default and fingerprint-safe; with it off, the output is
 * byte-identical.  Nothing here moves, deweights or re-estimates a station
 * coordinate.  SPEC §8 row 1 assigns any response to a separate change, because
 * re-coordinating a station is a datum act under standing invariant #7, with both
 * edges to bound.
 *
 * ## The fault class the other tiers cannot see
 *
 * Station coordinates are held fixed in the network producer.  An error in a held
 * coordinate (a survey blunder, an ARP or eccentricity mistake, monument motion)
 * projects coherently into every row of that station through the line-of-sight
 * geometry, and each armed tier is blind to it by construction:
 *
 *  - the prefit census's median polish removes the station's common effect, which
 *    is most of what a coordinate error is;
 *  - per-row w-tests (@ref NetworkPriorResidual, RAIM) see only what remains after
 *    the receiver clock, the ZWD and the ambiguities have absorbed what they can;
 *  - the (station, satellite) block test (@ref NetworkBlockBaarda) pools one line
 *    of sight, while a coordinate error is spread across all of them, so its
 *    per-block share is small;
 *  - the measurement-admission screen tests observations, not geometry.
 *
 * The 6.10 cm ARP error at JPLM is a measured instance on this network
 * (docs/SPEC_antenna_eccentricity_arp.md).  The ARP-frame fix resolved that instance;
 * this screen addresses the class.
 *
 * The screen estimates, from the station's own post-fit residuals, the 3D
 * coordinate offset that best explains them and tests it against zero.  It also
 * reports the formal detectability (a per-station, per-direction coordinate MDB),
 * because on a `carry_invariant` lineage the screen cannot see below some number of
 * centimetres in Up, and that bound is part of the output (SPEC §1, §7.3).
 *
 * ## The statistic
 *
 * For station @f$i@f$ on a swept epoch let @f$B@f$ be its `obs_code` + `obs_phase`
 * rows (@ref NetRowClass 0–1; an SH↔slant tie has no position dependence),
 * @f$\tilde e_B@f$ the post-fit residuals, @f$P_B=\mathrm{diag}(w)@f$,
 * @f$Q_{\tilde e,B}@f$ the @f$b\times b@f$ residual cofactor
 * (@ref NetworkNormalEquations::blockResidualCofactor, the kernel the block test
 * uses, applied here to an arbitrary row set of one station; @f$b@f$ is 30–200
 * here, against @f$\le 8@f$ in the block test), and @f$H_B@f$ the @f$b\times 3@f$
 * position Jacobian in ENU:
 * @f[
 *   M = H_B^\top P_B\,Q_{\tilde e,B}\,P_B H_B, \qquad
 *   \hat\delta = M^{+} H_B^\top P_B \tilde e_B, \qquad
 *   T_i = \frac{\hat\delta^\top M \hat\delta}{q\,\hat\sigma_{0}^2},
 * @f]
 * @f[
 *   \mathrm{MDB}_k = \delta_0\,\hat\sigma_{0}\,\sqrt{(M^{+})_{kk}} \quad\text{(m)} .
 * @f]
 * This is Baarda's multi-dimensional (DIA) form with the alternative hypothesis "an
 * arbitrary 3-vector bias on the station's coordinates": the general
 * @f$\tilde e^\top Q_y^{-1}C(C^\top Q_y^{-1}Q_{\tilde e}Q_y^{-1}C)^{-1}C^\top
 * Q_y^{-1}\tilde e@f$ with @f$C = H_B@f$ and a diagonal @f$Q_y^{-1}=P@f$.
 * Multi-dimensional outlier form per Snow & Schaffrin (2003); the congruence
 * question itself ("has this point moved relative to its held position") is
 * Caspary (2000), which the NGS LASER Guide carries in its own reference list.
 *
 * ### Three properties of this recipe
 *
 * 1. **@f$Q_{\tilde e,B}@f$ is never inverted.**  The block test's statistic
 *    @f$T_B@f$ needs @f$Q^{-1}@f$ and therefore has an uncheckable population; the
 *    contraction @f$H^\top P\,Q\,P H@f$ here is forward only.  A rank-deficient
 *    block (the normal state at a cold start, where every free ambiguity zeroes its
 *    own arc's phase residuals) needs no special case.  The degeneracy that matters
 *    moves into a @f$3\times 3@f$ eigenproblem, where it can be named per direction.
 * 2. **@f$M@f$ is eigen-guarded, never inverted directly.**  With a per-epoch
 *    station clock and a ZWD estimated, the information in Up can collapse (clock
 *    and troposphere absorb the vertical almost exactly).  A near-null eigenvalue
 *    reports that direction as unobservable with @f$\mathrm{MDB}=+\infty@f$
 *    (@ref BaardaReliability.hpp's convention: infinity is a category, not a large
 *    finite number a consumer could act on), and @f$\hat\delta@f$ and @f$T@f$ are
 *    formed in the observable subspace with @f$q = \mathrm{rank}@f$ degrees of
 *    freedom, so the absorption is measured.  (SPEC §2 writes the denominator as
 *    @f$3\hat\sigma_0^2@f$; @f$q@f$ equals 3 when the geometry is full-rank and is
 *    the correct generalization when it is not: dividing by 3 on a rank-2 station
 *    would leave @f$T@f$ one third too small.  @ref CongruenceSolve::dof carries
 *    @f$q@f$, so each row states its regime.)
 * 3. **Phase rows need no special case.**  Under `carry_invariant` their
 *    @f$Q_{\tilde e}@f$ rows are @f$\approx 0@f$ (a free ambiguity fits its own
 *    arc exactly), so they contribute almost nothing to @f$M@f$ and the screen is
 *    code-grade, with a dm-class MDB expected.  On a leveled lineage the phase rows
 *    contribute and the MDB drops toward cm-class.  The MDB table shows which regime
 *    the run was in.
 *
 * ## Sign convention
 *
 * @f$\hat\delta@f$ is the error in the held coordinate: positive East means the
 * held coordinate sits @f$\hat\delta_E@f$ metres East of where the data puts the
 * antenna.  Subtract it to correct the station.
 *
 * Derivation.  The measurement model linearizes about the held position and writes
 * @f$y \approx H_{pos}\,\Delta@f$ with @f$\Delta = p_{true}-p_{held}@f$ the
 * correction to the linearization point (@c MeasurementModel::buildJacobianRow
 * sets that partial to @f$-\hat u@f$ for every code and phase row, and
 * @c StationAssembly::row_pos_enu carries it unchanged).  The reported quantity is
 * @f$d = -\Delta = p_{held}-p_{true}@f$, so this class uses
 * @f$H_B = -\,@f$`row_pos_enu`.  A @f$+5@f$ cm East perturbation injected into a
 * held coordinate therefore reads @f$\hat\delta_E \approx +5@f$ cm, as SPEC §6.1
 * and the §7.2 perturbed arm assert and `tests/network/test_station_congruence.cpp` checks.
 * @f$M@f$, @f$T@f$ and every MDB are invariant to the overall sign; only
 * @f$\hat\delta@f$ depends on it.
 *
 * ## Reading the ladder: @f$t_i@f$ is a variance ratio, not a sigma
 *
 * The rungs are shared with the prior-residual and block-Baarda censuses
 * (@ref NetworkPriorResidual::ladder) so one reader handles all three files, but
 * the unit differs.  In those censuses @f$|t|@f$ is a standardized residual, so
 * rung 4 means four sigma.  Here @f$T_i@f$ includes the @f$1/q@f$, so
 * @f$E[t_i]=1@f$ under @f$H_0@f$ for every @f$q@f$, and rung 4 means the station's
 * coordinates carry four times the tension the model allows.
 *
 * ## @f$\hat\sigma_0@f$: one network-wide family
 *
 * @ref NetworkCarryVce, predict-then-update, never censored, with the block test's
 * two corrections applied unchanged because both are required for @f$b>1@f$:
 * the accumulator inverts the geometric mean of a @f$\chi^2_1@f$ variate
 * (@c kChi2GeoMean), so a @f$\chi^2_q@f$ statistic is fed as its
 * @f$\chi^2_1@f$-equivalent via @ref blockChi2GeoMean; and @c seed_floor_m2 is
 * passed as 1.0 (the 0.0 default becomes 1e-6, which gives
 * @f$\hat\sigma_0=10^{-3}@f$ on an unwarmed family and inflates every @f$t_i@f$ by
 * @f$10^6@f$).
 *
 * The family is a single network-wide partition by design.  The block test splits
 * by constellation because its statistic is per-LOS; this one pools a whole
 * station.  A per-station @f$\hat\sigma_0@f$ would let a faulty station calibrate
 * its own fault away, so the screen would read normal on the station it is meant to
 * find.  A per-constellation split is not available either, because the statistic
 * spans every constellation the station tracks.
 *
 * ## Isolation and testing
 *
 * Pure numeric leaf in the style of @ref NetworkCarryVce and @ref NetworkBlockBaarda:
 * header-only, no ApplicationNetwork coupling, no StateManager, no I/O.  The
 * statistic (@ref congruenceStatistic) is a free function over dense matrices, so
 * `tests/network/test_station_congruence.cpp` drives it against an independent dense-GLS
 * oracle.  The per-epoch trace, event and aggregate emission lives in
 * @c network/qc/Congruence.cpp, the only translation unit here that knows about
 * traces.
 */

#include "filter/BaardaReliability.hpp"   // delta_0 and the +infinity convention (the single reliability convention in this tree)
#include "network/qc/BlockBaarda.hpp"  // blockChi2GeoMean / kBlockChi2GeoMean1: the chi2_b -> chi2_1 rescale
#include "network/carry/CarryVce.hpp"
#include "network/qc/PriorResidual.hpp" // the shared ladder rungs
#include "network/assembly/RowClassify.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace GnssProcessor {

class NetworkNormalEquations;

// ===========================================================================
//  Conventions fixed as constants (SPEC §5 lists three knobs, none of them these)
// ===========================================================================

/// Relative eigenvalue floor on @f$M@f$: a direction whose eigenvalue is more than
/// ten decades below the largest carries no information at double precision and is
/// reported as unobservable rather than inverted.  Same decade, for the same
/// reason, as @ref kBlockCofactorRcondEps.
inline constexpr double kCongruenceEigenRelEps = 1e-10;

/// How much of an ENU axis may lie in @f$M@f$'s null space before that axis's MDB
/// is reported as @f$+\infty@f$.  @f$e_k^\top\delta@f$ is estimable only when
/// @f$e_k\in\mathrm{range}(M)@f$; this is that test with a numerical tolerance on
/// the squared null-space projection (the eigenvectors are orthonormal, so the
/// quantity is a fraction in @f$[0,1]@f$).  1e-6 means an axis tilted more than
/// 0.1 % (1e-3 in amplitude) into a collapsed direction cannot be separated from
/// it, and a finite MDB there would be the false number SPEC §10 refuses to print.
inline constexpr double kCongruenceAxisNullTol = 1e-6;

/// @f$\hat\sigma_0@f$ forgetting half-life, in swept epochs.  Not a knob (SPEC §5
/// lists three).  The value is D5 v2's, shared with the prior-residual and
/// block-Baarda censuses so the three censuses warm up identically.
inline constexpr double kCongruenceWindowEpochs = 60.0;

/// Station-epochs a family needs before @f$\hat\sigma_0@f$ is used.  D5's value,
/// in this screen's unit.  Below it every station reports raw formal
/// (@f$t_i \equiv T_i@f$) and flags that in its own column.
inline constexpr double kCongruenceWarmupSamples = 200.0;

/// Per-(station × label) cap on retained @f$\hat\delta@f$ / MDB samples.  The
/// median/MAD pair needs the samples; the cap bounds the memory at 48 B × 2048 ≈
/// 98 kB per station-label (about 60 MB at 305 stations with both labels full;
/// allocated only when armed).  Overflow is counted and reported at run end, so a
/// truncated median is not read as covering the whole run (the no-silent-caps
/// rule).
inline constexpr std::size_t kCongruenceMaxSamples = 2048;

/// The single @f$\hat\sigma_0@f$ family name.  See the file banner: a per-station
/// split would let a faulty station standardize its own fault away.
inline constexpr const char* kCongruenceFamily = "net";

// ===========================================================================
//  The statistic (pure; no run state, no I/O)
// ===========================================================================

/// Datum-frame label of one station-epoch.  Separate from
/// @ref PriorResidualLabel on purpose: its @c Responded tier is an arc-level
/// estimator response with no station-level meaning, and a third ladder here would
/// always be empty.  SPEC §3 (INC-2) specifies this split, and standing invariant
/// #7 motivates it: a datum re-realization is an event at both edges, so its
/// station-epochs are counted separately rather than dropped or pooled.
enum class CongruenceLabel : unsigned char { Clean = 0, Frame = 1 };

/// Number of @ref CongruenceLabel values.
inline constexpr int kCongruenceLabels = 2;

/// Stable wire name of a @ref CongruenceLabel.  Never null.
inline const char* congruenceLabelName(CongruenceLabel l) {
    return (l == CongruenceLabel::Frame) ? "FRAME" : "CLEAN";
}

/**
 * @brief One station-epoch's coordinate-offset estimate, its test statistic and
 *        its per-direction detectability.
 *
 * Every value here is raw (no @f$\hat\sigma_0@f$ applied).  The empirical
 * rescaling belongs to @ref NetworkCongruence, because a feed site that
 * standardized its own input could standardize a fault away.
 */
struct CongruenceSolve {
    bool ok = false;            ///< a statistic was formed (at least one observable direction).
    int  b = 0;                 ///< rows contracted.
    int  dof = 0;               ///< @f$q@f$: observable directions retained (0..3).
    Eigen::Vector3d delta = Eigen::Vector3d::Zero();  ///< @f$\hat\delta@f$ (m, ENU): the error in the held coordinate (see the sign convention in the file banner).
    double T_raw = 0.0;         ///< @f$\hat\delta^\top M\hat\delta / q@f$: per dof, before @f$\hat\sigma_0@f$ is applied.
    /// Formal per-axis coordinate MDB (m), @f$\delta_0\sqrt{(M^{+})_{kk}}@f$, in
    /// ENU order.  @c +infinity marks a direction that is not estimable; it is a
    /// category, never a clamp (@ref BaardaReliability.hpp).  The empirical variant
    /// is this times @f$\hat\sigma_0@f$ and is reported separately, never folded in
    /// here, for the reason the MDB analysis records: an MDB that is already inflated
    /// hides how optimistic the formal model is.
    Eigen::Vector3d mdb =
        Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());
    Eigen::Vector3d eval = Eigen::Vector3d::Zero();   ///< eigenvalues of @f$M@f$, ascending (units 1/m²).
    Eigen::Vector3d null_dir = Eigen::Vector3d::Zero();///< ENU components of the smallest-eigenvalue direction, i.e. the direction of the absorption.
};

/**
 * @brief Form one station's coordinate-offset estimate, test statistic and MDB.
 *
 * Pure: no run state, no I/O, no config.  Every guard is explicit so a caller
 * cannot get a plausible number out of a degenerate input.
 *
 * @param Hd        @f$b\times 3@f$ position Jacobian **in the reported sign
 *                  convention**, i.e. @f$\partial y/\partial d@f$ with
 *                  @f$d = p_{held}-p_{true}@f$.  Callers holding
 *                  @c StationAssembly::row_pos_enu (which is
 *                  @f$\partial y/\partial\Delta@f$) negate it first; see the
 *                  sign convention in the file banner.
 * @param w         per-row information weights @f$1/\sigma^2@f$ (length @c b).
 * @param Q         @f$b\times b@f$ residual cofactor
 *                  (@ref NetworkNormalEquations::blockResidualCofactor).  Used
 *                  forward only and never inverted, so a rank-deficient @p Q is
 *                  admissible (and common).
 * @param e         post-fit residuals @f$y - A_s x_s - A_r x_r@f$ (length @c b).
 * @param delta_0   Baarda non-centrality from @ref baardaDelta0.
 * @param eig_eps   relative eigenvalue floor (@ref kCongruenceEigenRelEps).
 * @param axis_tol  null-projection tolerance (@ref kCongruenceAxisNullTol).
 * @return The solve; @c ok is false when the inputs are unusable or @f$M@f$ has
 *         no observable direction at all.
 */
inline CongruenceSolve congruenceStatistic(
    const Eigen::MatrixXd& Hd, const Eigen::VectorXd& w,
    const Eigen::MatrixXd& Q, const Eigen::VectorXd& e, double delta_0,
    double eig_eps = kCongruenceEigenRelEps,
    double axis_tol = kCongruenceAxisNullTol) {
    CongruenceSolve out;
    const Eigen::Index b = Hd.rows();
    if (b <= 0 || Hd.cols() != 3 || w.size() != b || e.size() != b
        || Q.rows() != b || Q.cols() != b) return out;
    if (!Hd.allFinite() || !w.allFinite() || !e.allFinite() || !Q.allFinite())
        return out;
    out.b = static_cast<int>(b);

    // G = P H (b x 3).  Both the information matrix and the numerator are
    // contractions of it, so it is formed once.
    const Eigen::MatrixXd G = w.asDiagonal() * Hd;
    Eigen::Matrix3d M = G.transpose() * Q * G;
    // Q is symmetric by construction but the triple product is symmetric only in
    // exact arithmetic, and the self-adjoint eigensolver below reads a single
    // triangle, so an asymmetry would be discarded without notice.  Symmetrize.
    M = (0.5 * (M + M.transpose())).eval();
    if (!M.allFinite()) return out;
    const Eigen::Vector3d g = G.transpose() * e;
    if (!g.allFinite()) return out;

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(M);
    if (es.info() != Eigen::Success) return out;
    out.eval = es.eigenvalues();                 // ascending (Eigen's contract)
    const Eigen::Matrix3d V = es.eigenvectors();
    const double lmax = out.eval(2);
    if (!(lmax > 0.0) || !std::isfinite(lmax)) return out;
    const double cut = eig_eps * lmax;

    // Pseudo-inverse over the observable subspace only.  Directions under the cut
    // are dropped rather than regularized: a ridge would produce a finite MDB in a
    // direction the data does not constrain, and this screen must not report one.
    Eigen::Matrix3d Mp = Eigen::Matrix3d::Zero();
    int q = 0;
    for (int j = 0; j < 3; ++j) {
        const double lj = out.eval(j);
        if (!(lj > cut) || !std::isfinite(lj)) continue;
        Mp.noalias() += (V.col(j) * V.col(j).transpose()) / lj;
        ++q;
    }
    out.dof = q;
    if (q == 0) return out;

    out.delta = Mp * g;
    // delta' M delta == g' Mp M Mp g == g' Mp g exactly (Mp M Mp = Mp on the
    // retained subspace); the quadratic form is written out so the identity is
    // visible rather than assumed.
    const double quad = out.delta.dot(M * out.delta);
    if (!(quad >= 0.0) || !std::isfinite(quad)) return out;
    out.T_raw = quad / static_cast<double>(q);

    for (int k = 0; k < 3; ++k) {
        double null_frac = 0.0;
        for (int j = 0; j < 3; ++j) {
            const double lj = out.eval(j);
            if ((lj > cut) && std::isfinite(lj)) continue;   // observable direction
            null_frac += V(k, j) * V(k, j);
        }
        const double vkk = Mp(k, k);
        out.mdb(k) = (null_frac > axis_tol || !(vkk > 0.0) || !std::isfinite(vkk))
                         ? std::numeric_limits<double>::infinity()
                         : delta_0 * std::sqrt(vkk);
    }

    // The weakest direction, for the readout.  Sign-normalized on its largest
    // component so the same geometry always reports the same vector: an
    // eigenvector's sign is arbitrary, and a sign that flips between epochs would
    // look like motion.
    out.null_dir = V.col(0);
    Eigen::Index kmax = 0;
    out.null_dir.cwiseAbs().maxCoeff(&kmax);
    if (out.null_dir(kmax) < 0.0) out.null_dir = -out.null_dir;

    out.ok = true;
    return out;
}

/// Median of @p v.  Reorders @p v (partial sort).  NaN for an empty pool.
inline double congruenceMedian(std::vector<double>& v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const std::size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2), v.end());
    const double hi = v[n / 2];
    if (n % 2 == 1) return hi;
    // Even n: the lower middle is the max of the left partition after nth_element.
    const double lo = *std::max_element(v.begin(),
                                        v.begin() + static_cast<std::ptrdiff_t>(n / 2));
    return 0.5 * (lo + hi);
}

/// Median absolute deviation of @p v about @p med, scaled to a Gaussian sigma
/// (× 1.4826).  The D-family robust convention.  SPEC §3 (INC-2) makes the
/// median/MAD pair the reported uncertainty because no @f$\sqrt N@f$ reduction
/// applies: D1 measured @f$K_{eff}(30)\approx 2@f$ on this network, so per-epoch
/// @f$\hat\delta@f$ samples do not average as white noise.
inline double congruenceMad(const std::vector<double>& v, double med) {
    if (v.empty() || !std::isfinite(med))
        return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> d;
    d.reserve(v.size());
    for (double x : v) if (std::isfinite(x)) d.push_back(std::abs(x - med));
    if (d.empty()) return std::numeric_limits<double>::quiet_NaN();
    return 1.4826 * congruenceMedian(d);
}

// ===========================================================================
//  The accumulator
// ===========================================================================

/**
 * @brief Per-station congruence ladders, @f$\hat\delta@f$ / MDB accumulation and
 *        the INC-3 separability table.  One instance per producer run.
 *
 * Like @ref NetworkBlockBaarda (and unlike @ref NetworkPriorResidual, a run-TU
 * local) this lives in @c NetworkCarryState, because its feed site is inside the
 * redundancy-census seam rather than in the driver.
 */
class NetworkCongruence {
public:
    /// Ladder rungs are shared with the prior-residual and block-Baarda censuses
    /// so the three censuses' CSVs have one shape and one reader.  The unit
    /// differs (see "Reading the ladder" in the file banner), which matters when
    /// comparing occupancies.
    static constexpr int kLadderBins   = NetworkPriorResidual::kLadderBins;
    static constexpr int kLadder4Sigma = NetworkPriorResidual::kLadder4Sigma;
    static const std::array<double, kLadderBins>& ladder() {
        return NetworkPriorResidual::ladder();
    }

    /// One (station × label) exceedance ladder over @f$t_i@f$.  O(1) per fold.
    struct Ladder {
        long long n_epochs     = 0;  ///< station-epochs folded into this ladder.
        long long n_rows       = 0;  ///< data rows they contracted.
        long long n_degenerate = 0;  ///< …with @f$q<3@f$: at least one direction unobservable.
        long long n_null       = 0;  ///< …with @f$q=0@f$: no direction observable, so no statistic (counted, never inverted).
        long long n_raw_formal = 0;  ///< …reported against @f$\hat\sigma_0=1@f$ because the family had not warmed up (@f$t_i\equiv T_i@f$; flagged in the output).
        long long n_reported   = 0;  ///< …with @f$t_i>@f$ report threshold.
        std::array<long long, kLadderBins> bins{};  ///< cumulative exceedance: bins[k] = #station-epochs with t_i > ladder()[k].
        double    sum_log_t    = 0.0;///< sum of the log of the calibrated @f$\chi^2_1@f$-equivalent (see @ref geoT), not of @f$t_i@f$.
        long long n_geo        = 0;  ///< station-epochs in @ref sum_log_t.
        /// Separability table (INC-3).  Rows: [0] = @f$t_i>@f$ threshold, [1] =
        /// below.  Columns: [0] = block-Baarda flagged no line of sight of this
        /// station this epoch, [1] = it flagged at least one.  All-zero unless the
        /// block census is armed; an unarmed run has no verdict to join, which is
        /// different from a negative verdict and is reported as such.
        std::array<std::array<long long, 2>, 2> contingency{};

        // --- identity of the worst station-epoch (invariant #4: a finding needs a name)
        double max_t   = 0.0;   ///< largest @f$t_i@f$ seen.
        double max_T   = 0.0;   ///< its raw @f$T_i@f$ (before @f$\hat\sigma_0@f$).
        double max_tow = -1.0;  ///< its TOW.
        Eigen::Vector3d max_delta = Eigen::Vector3d::Zero();  ///< its @f$\hat\delta@f$ (m, ENU).
        int    max_dof = 0;     ///< its observable dimension.

        /// Fraction of station-epochs above ladder rung @p k (0 when empty).
        double frac(int k) const {
            if (n_epochs <= 0 || k < 0 || k >= kLadderBins) return 0.0;
            return static_cast<double>(bins[static_cast<std::size_t>(k)])
                 / static_cast<double>(n_epochs);
        }
        /// Tail-robust calibration indicator: the geometric mean of the
        /// @f$\chi^2_1@f$-equivalent, standardized statistic, divided by its null
        /// value.  1.0 means the scale has converged, for any mix of @f$q@f$.  The
        /// geometric mean of @f$t_i@f$ itself is not used because its null value
        /// @f$2e^{\psi(q/2)}/q@f$ depends on @f$q@f$, which makes it unreadable
        /// across a population that mixes rank-3 and rank-2 stations.
        double geoT() const {
            if (n_geo <= 0) return 0.0;
            return std::exp(sum_log_t / static_cast<double>(n_geo)) / kBlockChi2GeoMean1;
        }
        void reset() { *this = Ladder{}; }
    };

    /// One retained station-epoch, for the median/MAD readout (INC-2).  48 bytes.
    struct Sample {
        Eigen::Vector3d d   = Eigen::Vector3d::Zero();  ///< @f$\hat\delta@f$ (m, ENU).
        Eigen::Vector3d mdb =
            Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());  ///< formal MDB (m, ENU); @c +inf entries are counted, never averaged.
    };

    /// One station's run state.
    struct StationState {
        std::array<Ladder, kCongruenceLabels>              run{};
        std::array<std::vector<Sample>, kCongruenceLabels> samples{};
        std::array<long long, kCongruenceLabels>           dropped{{0, 0}};  ///< samples past @ref kCongruenceMaxSamples; counted and reported.
    };

    /// One station-epoch, as the feed site presents it.  Raw: no
    /// @f$\hat\sigma_0@f$ applied, so the feed site cannot standardize against the
    /// epoch it is contributing to.
    struct Input {
        int             station = -1;      ///< station roster index (identity only).
        CongruenceSolve solve{};           ///< the statistic (@ref congruenceStatistic).
        int             n_code = 0;        ///< code rows contracted (composition, for the run-end readout).
        int             n_phase = 0;       ///< phase rows contracted.
        CongruenceLabel label = CongruenceLabel::Clean;  ///< assigned at the feed site; this class never labels.
        bool            block_known = false;  ///< the block census was armed, so its verdict exists (INC-3).
        bool            block_flagged = false;///< the block census flagged >= 1 LOS of this station this epoch (INC-3).
    };

    /// What @ref addStation did, for the caller's per-epoch counters and events.
    struct StationStat {
        bool   accepted   = false;  ///< entered a ladder.
        bool   degenerate = false;  ///< @f$q<3@f$.
        bool   null_geom  = false;  ///< @f$q=0@f$: no statistic could be formed.
        bool   raw_formal = false;  ///< the family had not warmed up: @f$\hat\sigma_0=1@f$.
        bool   reported   = false;  ///< @f$t_i@f$ cleared the report threshold.
        double t          = 0.0;    ///< @f$t_i@f$.
        double sigma0     = 1.0;    ///< the frozen @f$\hat\sigma_0@f$ this station-epoch was scaled by.
    };

    /**
     * @param armed         whether the screen runs at all (an unarmed instance is
     *                      inert and allocates nothing).
     * @param window_epochs @f$\hat\sigma_0@f$ forgetting window
     *                      (@ref kCongruenceWindowEpochs).
     * @param min_samples   warm-up bar in station-epochs
     *                      (@ref kCongruenceWarmupSamples).
     * @param report_sigma  @c network.congruence_report_sigma: the trace / event
     *                      threshold on @f$t_i@f$.  Affects reporting only.
     */
    explicit NetworkCongruence(bool armed = false,
                               double window_epochs = kCongruenceWindowEpochs,
                               double min_samples = kCongruenceWarmupSamples,
                               double report_sigma = 4.0)
        : armed_(armed),
          // >= 0, not > 0: 0 is a valid request for "no warm-up bar, calibrate
          // from the first station-epoch".  Only a negative value is replaced by
          // the default.
          min_samples_(min_samples >= 0.0 ? min_samples : kCongruenceWarmupSamples),
          report_sigma_(report_sigma > 0.0 ? report_sigma : 4.0),
          // A single family (see the file banner).  split_by_const = false: there
          // is no constellation axis here, since the statistic spans every
          // constellation the station tracks.  seed_floor_m2 = 1.0 so an unwarmed
          // family reads sigma0 = 1 exactly (0.0 would become 1e-6).
          inflation_(armed ? std::vector<std::string>{kCongruenceFamily}
                           : std::vector<std::string>{},
                     window_epochs, /*split_by_const=*/false,
                     /*seed_floor_m2=*/1.0, /*seed_rate_m2ps=*/0.0) {}

    /// @return Whether the screen is armed (an unarmed instance is inert).
    bool   enabled() const      { return armed_; }
    double reportSigma() const  { return report_sigma_; }

    /**
     * @brief Predict step: freeze this epoch's @f$\hat\sigma_0@f$ and clear the
     *        per-epoch counters.
     *
     * The order is part of the contract: the estimate (through the previous epoch)
     * is read before @ref NetworkCarryVce::beginEpoch decays the accumulator, so a
     * station is never standardized by a calibration it contributed to.  @c clean
     * is always true: a datum-changed epoch is a Frame population to be measured,
     * not dropped, and the label split exists to keep it separate.
     */
    void beginEpoch(double content_time_s, double tow) {
        if (!armed_) return;
        last_tow_ = tow;
        ++n_epochs_;
        const NetworkCarryVce::Estimate e =
            inflation_.estimate(kCongruenceFamily, std::string());
        // Test n_eff, not robust_floor > 0: an empty bucket returns seed_floor_,
        // so robust_floor does not indicate whether data exists.  The explicit
        // n_eff > 0 keeps a min_samples = 0 family from reading warm before the
        // first station-epoch has been folded.
        if (e.n_eff > 0.0 && e.n_eff >= min_samples_ && e.robust_floor > 0.0
            && std::isfinite(e.robust_floor)) {
            sigma0_ = std::sqrt(e.robust_floor);
            warm_   = true;
        } else {
            sigma0_ = 1.0;        // raw-formal: t_i == T_i, and every row says so
            warm_   = false;
        }
        n_eff_ = e.n_eff;
        ep_stations_ = ep_degenerate_ = ep_reported_ = 0;
        ep_max_t_ = 0.0;
        inflation_.beginEpoch(content_time_s, /*clean=*/true);
    }

    /**
     * @brief Update step: fold one station-epoch.
     * @return What happened, for the caller's events and counters.
     */
    StationStat addStation(const Input& in) {
        StationStat out;
        if (!armed_ || in.station < 0) return out;
        StationState& S = state_[in.station];
        Ladder& L = S.run[static_cast<std::size_t>(in.label)];

        // A null-geometry station-epoch is counted with its identity and never
        // inverted; it is the position-space analogue of the block census's
        // uncheckable cell.  It indicates wiring or absorption, not a finding, and
        // is kept distinct from "swept and quiet" (invariant #4).
        if (!in.solve.ok || in.solve.dof <= 0) {
            ++L.n_epochs; ++L.n_null;
            L.n_rows += in.solve.b;
            if (in.solve.dof < 3) ++L.n_degenerate;
            ++ep_stations_;
            out.accepted = true;
            out.null_geom = true;
            out.degenerate = true;
            return out;
        }
        if (!(in.solve.T_raw >= 0.0) || !std::isfinite(in.solve.T_raw)) return out;

        // --- uncensored calibration feed ------------------------------------
        // The calibration is fed before any threshold is consulted, with the
        // chi2_q -> chi2_1 rescaling from the block test: NetworkCarryVce inverts
        // the geometric mean of a chi2_1 variate, and T_raw*q ~ sigma0^2 chi2_q,
        // so feeding sqrt(T_raw) directly would bias sigma0 high by up to 1.57x
        // in sigma (2.46x in variance, at q = 3), and the bias would change with
        // the share of rank-2 stations.
        const double u2 = in.solve.T_raw * static_cast<double>(in.solve.dof)
                        * (kBlockChi2GeoMean1 / blockChi2GeoMean(in.solve.dof));
        if (u2 > 0.0 && std::isfinite(u2))
            inflation_.addRow(kCongruenceFamily, std::string(), std::sqrt(u2),
                              /*gap_s=*/0.0, /*post_var=*/0.0, /*fold_var=*/1.0);

        const double s2 = sigma0_ * sigma0_;
        out.sigma0     = sigma0_;
        out.raw_formal = !warm_;
        out.t          = (s2 > 0.0) ? in.solve.T_raw / s2 : in.solve.T_raw;
        out.accepted   = true;
        out.degenerate = in.solve.dof < 3;

        ++L.n_epochs;
        L.n_rows += in.solve.b;
        if (out.degenerate)  ++L.n_degenerate;
        if (out.raw_formal)  ++L.n_raw_formal;

        const auto& lad = ladder();
        for (int k = 0; k < kLadderBins; ++k) {
            if (!(out.t > lad[static_cast<std::size_t>(k)])) break;  // ascends: first miss ends it
            ++L.bins[static_cast<std::size_t>(k)];
        }
        const double un = (s2 > 0.0) ? u2 / s2 : u2;
        if (un > 0.0 && std::isfinite(un)) {
            L.sum_log_t += std::log(un);
            ++L.n_geo;
        }

        out.reported = out.t > report_sigma_;
        if (out.reported) { ++L.n_reported; ++ep_reported_; }

        // Separability table (INC-3): a coordinate fault is coherent across all
        // lines of sight and should not excite the per-LOS block tier, while a
        // single-LOS fault should excite that tier and not this one.  The 2x2
        // measures that partition instead of assuming it.
        if (in.block_known) {
            ++L.contingency[out.reported ? 0u : 1u][in.block_flagged ? 1u : 0u];
            ++sep_known_;
        }

        if (out.t > L.max_t) {
            L.max_t = out.t;   L.max_T = in.solve.T_raw;
            L.max_tow = last_tow_;
            L.max_delta = in.solve.delta;
            L.max_dof = in.solve.dof;
        }

        // Retain the sample (INC-2).  Median/MAD needs the samples, since a running
        // scalar can produce neither, and the arithmetic mean is the statistic
        // that D1's K_eff ~ 2 rules out here.
        std::vector<Sample>& smp = S.samples[static_cast<std::size_t>(in.label)];
        if (smp.size() < kCongruenceMaxSamples) {
            Sample s;
            s.d = in.solve.delta;
            s.mdb = in.solve.mdb;
            smp.push_back(s);
        } else {
            ++S.dropped[static_cast<std::size_t>(in.label)];
        }

        n_code_  += in.n_code;
        n_phase_ += in.n_phase;
        ++ep_stations_;
        if (out.degenerate) ++ep_degenerate_;
        if (out.t > ep_max_t_) ep_max_t_ = out.t;
        return out;
    }

    // --- readout ----------------------------------------------------------
    double    sigma0() const      { return sigma0_; }
    bool      sigma0Warm() const  { return warm_; }
    double    sigma0Neff() const  { return n_eff_; }
    long long epochs() const      { return n_epochs_; }
    long long epochStations() const   { return ep_stations_; }
    long long epochDegenerate() const { return ep_degenerate_; }
    long long epochReported() const   { return ep_reported_; }
    double    epochMaxT() const   { return ep_max_t_; }
    double    lastTow() const     { return last_tow_; }
    long long runCodeRows() const { return n_code_; }
    long long runPhaseRows() const{ return n_phase_; }
    long long separabilityKnown() const { return sep_known_; }

    /// Stations that have been folded at least once, in ascending roster order
    /// (std::map, so the run-end table is reproducible across A/B arms).
    const std::map<int, StationState>& stations() const { return state_; }

    /// @return Station @p sta's ladder for @p l, or a static empty one.
    const Ladder& runLadder(int sta, CongruenceLabel l) const {
        static const Ladder kEmpty{};
        const auto it = state_.find(sta);
        if (it == state_.end()) return kEmpty;
        return it->second.run[static_cast<std::size_t>(l)];
    }

    /// Station-epochs folded for @p sta over both labels.
    long long stationEpochs(int sta) const {
        long long n = 0;
        for (int l = 0; l < kCongruenceLabels; ++l)
            n += runLadder(sta, static_cast<CongruenceLabel>(l)).n_epochs;
        return n;
    }

    /// Network-wide 2x2 separability table, summed over stations and labels.
    std::array<std::array<long long, 2>, 2> separability() const {
        std::array<std::array<long long, 2>, 2> c{};
        for (const auto& kv : state_)
            for (int l = 0; l < kCongruenceLabels; ++l)
                for (int r = 0; r < 2; ++r)
                    for (int b = 0; b < 2; ++b)
                        c[static_cast<std::size_t>(r)][static_cast<std::size_t>(b)] +=
                            kv.second.run[static_cast<std::size_t>(l)]
                                .contingency[static_cast<std::size_t>(r)]
                                            [static_cast<std::size_t>(b)];
        return c;
    }

    /// Samples past the cap, over every station and label.  Reported at run end so
    /// a truncated median never reads as a whole-run one.
    long long droppedSamples() const {
        long long n = 0;
        for (const auto& kv : state_)
            for (int l = 0; l < kCongruenceLabels; ++l)
                n += kv.second.dropped[static_cast<std::size_t>(l)];
        return n;
    }

    /// Per-component median and MAD of @f$\hat\delta@f$ for one (station, label).
    /// NaN components when the pool is empty.
    struct DeltaStats {
        long long n = 0;
        Eigen::Vector3d med = Eigen::Vector3d::Constant(std::numeric_limits<double>::quiet_NaN());
        Eigen::Vector3d mad = Eigen::Vector3d::Constant(std::numeric_limits<double>::quiet_NaN());
    };
    DeltaStats deltaStats(int sta, CongruenceLabel l) const {
        DeltaStats o;
        const auto it = state_.find(sta);
        if (it == state_.end()) return o;
        const std::vector<Sample>& smp = it->second.samples[static_cast<std::size_t>(l)];
        o.n = static_cast<long long>(smp.size());
        if (smp.empty()) return o;
        for (int k = 0; k < 3; ++k) {
            std::vector<double> v;
            v.reserve(smp.size());
            for (const Sample& s : smp)
                if (std::isfinite(s.d(k))) v.push_back(s.d(k));
            const double m = congruenceMedian(v);
            o.med(k) = m;
            o.mad(k) = congruenceMad(v, m);
        }
        return o;
    }

    /// Per-component median MDB for one (station, label) over the finite samples,
    /// with the @f$+\infty@f$ population counted separately.  A median over a pool
    /// containing infinity is either meaningless or censored without notice,
    /// depending on the sort, so the unobservable epochs are reported as their own
    /// count (the rule the MDB analysis follows).
    struct MdbStats {
        long long n = 0;
        Eigen::Vector3d med = Eigen::Vector3d::Constant(std::numeric_limits<double>::quiet_NaN());
        std::array<long long, 3> n_inf{{0, 0, 0}};
    };
    MdbStats mdbStats(int sta, CongruenceLabel l) const {
        MdbStats o;
        const auto it = state_.find(sta);
        if (it == state_.end()) return o;
        const std::vector<Sample>& smp = it->second.samples[static_cast<std::size_t>(l)];
        o.n = static_cast<long long>(smp.size());
        if (smp.empty()) return o;
        for (int k = 0; k < 3; ++k) {
            std::vector<double> v;
            v.reserve(smp.size());
            for (const Sample& s : smp) {
                if (std::isfinite(s.mdb(k))) v.push_back(s.mdb(k));
                else ++o.n_inf[static_cast<std::size_t>(k)];
            }
            o.med(k) = congruenceMedian(v);
        }
        return o;
    }

private:
    bool   armed_;
    double min_samples_;
    double report_sigma_;
    NetworkCarryVce inflation_;
    std::map<int, StationState> state_;

    double    sigma0_ = 1.0;   ///< frozen at beginEpoch (through the previous epoch).
    bool      warm_   = false;
    double    n_eff_  = 0.0;
    double    last_tow_ = -1.0;
    long long n_epochs_ = 0;
    long long ep_stations_ = 0, ep_degenerate_ = 0, ep_reported_ = 0;
    double    ep_max_t_ = 0.0;
    long long n_code_ = 0, n_phase_ = 0;
    long long sep_known_ = 0;
};

// ===========================================================================
//  The sweep + emission (network/qc/Congruence.cpp)
// ===========================================================================

/// One reportable station-epoch, held until the emission seam writes its rows.
/// Every station-epoch accepted by @ref NetworkCongruence::addStation (null
/// geometry included) produces one, unlike the block test's `hits`, which holds
/// only the flagged ones: the per-station trace row is this screen's primary
/// product (SPEC §7.3's MDB table is built from it), and the event stream is a
/// rate-limited alarm on top of it.
struct CongruenceHit {
    int             station = -1;
    CongruenceSolve solve{};
    int             n_code = 0, n_phase = 0;
    double          t = 0.0;        ///< @f$t_i@f$.
    double          sigma0 = 1.0;
    bool            raw_formal = false;
    bool            reported = false;
    bool            null_geom = false;
    CongruenceLabel label = CongruenceLabel::Clean;
    bool            block_known = false;
    bool            block_flagged = false;
};

/// Run-total tallies (standing invariant #4).  Without denominators, "the screen
/// ran and found nothing" cannot be told apart from "the screen never ran", so
/// passes, station sweeps, rows and the degenerate population are reported before
/// any finding.
struct NetworkCongruenceTallies {
    long long passes         = 0;  ///< emission-seam invocations (did the wiring execute at all).
    long long epochs         = 0;  ///< …that carried at least one station.
    long long station_sweeps = 0;  ///< per-station congruence sweeps performed.
    long long rows           = 0;  ///< data rows contracted.
    long long no_geom_rows   = 0;  ///< code and phase rows dropped for a missing, zero or non-finite position partial (e.g. a station assembled before the screen was armed), a non-positive weight or a non-finite residual.
    long long no_resid       = 0;  ///< stations skipped because the post-fit residual could not be formed.
    long long no_rows        = 0;  ///< stations skipped because no row carried geometry.
    long long solve_failed   = 0;  ///< stations whose residual cofactor could not be formed, or whose solve addStation did not accept.  A q = 0 station-epoch is not counted here; it is accepted and counted as null geometry.
    long long degenerate     = 0;  ///< station-epochs with q < 3 (at least one direction unobservable).
    long long reported       = 0;  ///< station-epochs over the report threshold.
    long long events         = 0;  ///< event rows emitted (rate-limited).
    double    wall_ms        = 0.0;///< run-total wall inside the sweep + emission.
    double    max_t          = 0.0;///< worst @f$t_i@f$ of the run.
    std::string max_id;            ///< its identity, "#<roster index> d=(E,N,U) m ENU q=… @tow …".
};

struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp, included in the .cpp only

/**
 * @brief Sweep one station's coordinate congruence (the costly tier of INC-1).
 *
 * Called from the redundancy census's swept branch, never from its free tier, so
 * the stride, targeted-escalation and trip schedule is inherited unchanged and
 * this screen adds no scheduler of its own (SPEC §3).  Everything it needs is
 * already in scope there: the kernel, the station's slot, the hoisted datum-frame
 * covariance, the row classes from the census's classifier, and the post-fit
 * residual that the census forms once for this screen and the block rider.
 *
 * Cost: one @f$b\times b@f$ cofactor over the station's data rows, one
 * @f$b\times 3@f$ contraction and one @f$3\times 3@f$ eigensolve.  The cofactor
 * is of the same order as the @c rowRedundancy call the census already made for
 * the same station (both are dominated by the @f$N_{rr}^{-1}N_{sr}^\top@f$ solve
 * and the @f$n_{shared}^2@f$ Gram), which keeps SPEC §4's budget ("≤ 2× the
 * redundancy sweep it rides") within reach.  The sweep is timed here, and
 * @c tallies.wall_ms is the figure to check against that budget.
 *
 * @param acc            The run accumulator (already @ref NetworkCongruence::beginEpoch'd).
 * @param kernel         Solved arrowhead kernel.
 * @param slot           This station's kernel slot (@c est_index_of, never the roster index).
 * @param station_index  Roster index, for identity only.
 * @param R              The station assembly (row stack + @c row_pos_enu).
 * @param cls            Per-row classes from the census's classifier.
 * @param resid          post-fit residuals @f$y - A_s x_s - A_r x_r@f$, in @c R.y's row order.
 * @param cov_free       Datum-frame shared covariance, built once by the census.
 * @param delta_0        Baarda non-centrality (@ref baardaDelta0) for the MDB.
 * @param datum_changed  This epoch re-realized the datum, so the station-epoch is
 *                       labelled Frame (invariant #7).
 * @param block_known    The block census was armed, so the INC-3 join has a verdict.
 * @param block_flagged  The block census flagged at least one LOS of this station
 *                       this epoch.
 * @param tallies        [in/out] run totals.
 * @param hits           [out] this epoch's station records, appended.
 */
void networkCongruenceSweepStation(NetworkCongruence& acc,
                                   const NetworkNormalEquations& kernel,
                                   std::size_t slot, int station_index,
                                   const StationAssembly& R,
                                   const std::vector<NetRowClass>& cls,
                                   const Eigen::VectorXd& resid,
                                   const Eigen::MatrixXd& cov_free,
                                   double delta_0, bool datum_changed,
                                   bool block_known, bool block_flagged,
                                   NetworkCongruenceTallies& tallies,
                                   std::vector<CongruenceHit>& hits);

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Per-epoch emission: the per-station trace rows and the rate-limited
 *        event identities.
 *
 * Called from the driver's SB_FRAMEDIAG block after @c networkRedundancyCensus,
 * which fed the accumulator this epoch.  Gated on @c ctx.knobs.cg_on, so an
 * unarmed run never enters this TU.
 *
 * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log),
 *            run.setup.stations (stations).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx.
 *   - @p config_ = run.config.
 *   - @p log = run.log.
 *   - @p traces = run.traces.
 *   - @p se = ep.se.
 *   - @p stations = run.setup.stations.
 */
void networkCongruenceEmit(NetworkRun& run, const NetworkEpoch& ep);

/// Run-end readout (invariant #4).  Denominators first; then the MDB table (SPEC
/// §7.3's service deliverable: the smallest coordinate fault this network can see,
/// per station and per direction); then the ladders; then the INC-3 2×2 table.
void networkCongruenceRunEnd(const NetworkRunContext& ctx,
                             const std::shared_ptr<spdlog::logger>& log,
                             const std::vector<NetStation>& stations);

/**
 * @brief The predict half of the station-congruence census: sigma0 through the previous epoch
 *        is frozen before the census's rider feeds this epoch's stations, so a station cannot
 *        standardize its own coordinate fault away.
 *
 * Under the congruence screen (ctx.knobs.cg_on), its first line.  The body is the frozen
 * driver text, moved by scripts/nom_ep2_cut.py.
 *
 * @param run The run: ctx.carry.cg (written).
 * @param ep  The epoch, read: clock.pl_now_s, se.time.tow.
 */
void networkCongruencePredict(NetworkRun& run, const NetworkEpoch& ep);
// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the station congruence screen.
 *
 * The table's row for the station congruence screen; the report is the overload above.
 *
 * Gate: prints under `ctx.knobs.cg_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkCongruenceRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- PASS B station steps, declarations (generated by scripts/nom_pb1b_cut.py) ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp

/**
 * @brief The station congruence screen's per-row position partials (CG-INC-0).
 *
 * Step `congruence_geometry` of kStationBuildSteps (network/assembly/StationBuild.hpp); its kinds:
 * none (it records only). It returns at once unless `congruence_rows && em.H.rows() > 0 &&
 * st.ecef.norm() > 1.0`. Records: row_pos_enu. The frozen text of networkPassB_'s produce loop
 * below its bindings.
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkCongruenceRowGeometry(StationBuild& build);

// ---- End of the PASS B station steps, declarations ----

/// @brief Run-scope counters of Congruence, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct CongruenceCounters {

    /// Station congruence screen tallies.  Named `cg` on the same convention as `bb`:
    /// `ctx.counters.cg` is the run-total POD and `ctx.carry.cg` is the accumulator that
    /// produced it.  It counts station-epochs, where `bb` counts blocks and the
    /// redundancy census's `redun_*` block counts rows: three denominators that are not
    /// interchangeable and must never be divided into one another.
    NetworkCongruenceTallies cg;
};

}  // namespace GnssProcessor
