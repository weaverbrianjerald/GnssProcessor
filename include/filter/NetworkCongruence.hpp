#pragma once
/**
 * @file NetworkCongruence.hpp
 * @brief The STATION CONGRUENCE screen — coordinate-fault detection in position
 *        space (docs/SPEC_station_congruence.md), INC-1..3: the statistic and its
 *        bookkeeping.
 *
 * Measure-only, default-OFF, OFF byte-identical, fingerprint-SAFE.  Nothing here
 * moves, deweights or re-estimates a station coordinate; SPEC §8 row 1
 * pre-declares that ANY response — and re-coordinating a station is a DATUM act
 * under standing invariant #7, with both edges to bound — belongs to its own
 * card.
 *
 * ## The one fault class no armed tier can see
 *
 * Station coordinates are HELD in the network producer.  A wrong held coordinate
 * — a survey blunder, an ARP/eccentricity mistake, monument motion — projects
 * into EVERY row of that station through the line-of-sight geometry, coherently,
 * and every armed tier is blind to it by construction:
 *
 *  - the prefit census's median polish removes the station's common effect, which
 *    is exactly the part a coordinate error mostly is;
 *  - per-row w-tests (@ref NetworkPriorResidual, RAIM) see only the crumbs left
 *    after the receiver clock, the ZWD and the ambiguities have absorbed what
 *    they can;
 *  - the (station, satellite) block test (@ref NetworkBlockBaarda) pools ONE line
 *    of sight, and a coordinate error is spread across ALL of them — its
 *    per-block share is small by construction;
 *  - the measurement-admission screen tests OBSERVATIONS, not geometry.
 *
 * JPLM's 6.10 cm ARP error is the measured precedent on this very network
 * (docs/SPEC_antenna_eccentricity_arp.md).  It was found by hand.  The ARP-frame
 * card fixed that INSTANCE; this card is the instrument for the CLASS.
 *
 * The screen estimates, from the station's own post-fit residuals, the 3D
 * coordinate offset that would best explain them, and tests it against zero —
 * with its formal detectability (a per-station, per-direction coordinate MDB)
 * reported alongside, because on a `carry_invariant` lineage the honest answer is
 * "this screen cannot see below X cm in Up", and that number is a DELIVERABLE,
 * not an embarrassment (SPEC §1, §7.3).
 *
 * ## The statistic
 *
 * For station @f$i@f$ on a swept epoch let @f$B@f$ be its `obs_code` + `obs_phase`
 * rows (@ref NetRowClass 0–1; an SH↔slant tie carries no position dependence),
 * @f$\tilde e_B@f$ the post-fit residuals, @f$P_B=\mathrm{diag}(w)@f$,
 * @f$Q_{\tilde e,B}@f$ the @f$b\times b@f$ residual cofactor
 * (@ref NetworkNormalEquations::blockResidualCofactor — the SAME kernel the block
 * card uses, taking an arbitrary row set of ONE station; @f$b@f$ here is 30–200,
 * not that card's @f$\le 8@f$), and @f$H_B@f$ the @f$b\times 3@f$ position
 * Jacobian in ENU:
 * @f[
 *   M = H_B^\top P_B\,Q_{\tilde e,B}\,P_B H_B, \qquad
 *   \hat\delta = M^{+} H_B^\top P_B \tilde e_B, \qquad
 *   T_i = \frac{\hat\delta^\top M \hat\delta}{q\,\hat\sigma_{0}^2},
 * @f]
 * @f[
 *   \mathrm{MDB}_k = \delta_0\,\hat\sigma_{0}\,\sqrt{(M^{+})_{kk}} \quad\text{(m)} .
 * @f]
 * This is Baarda's multi-dimensional (DIA) form with the alternative "an arbitrary
 * 3-vector bias on the station's coordinates": the general
 * @f$\tilde e^\top Q_y^{-1}C(C^\top Q_y^{-1}Q_{\tilde e}Q_y^{-1}C)^{-1}C^\top
 * Q_y^{-1}\tilde e@f$ with @f$C = H_B@f$ and a diagonal @f$Q_y^{-1}=P@f$.
 * Multi-dimensional outlier form per Snow & Schaffrin (2003); the congruence
 * question itself ("has this point moved relative to its held position") is
 * Caspary (2000), which the NGS LASER Guide carries in its own reference list.
 *
 * ### THREE things about this recipe are load-bearing
 *
 * 1. **@f$Q_{\tilde e,B}@f$ is never INVERTED here.**  The block card has an
 *    UNCHECKABLE population because @f$T_B@f$ needs @f$Q^{-1}@f$; this card's
 *    contraction @f$H^\top P\,Q\,P H@f$ is FORWARD, so a rank-deficient block —
 *    the structural state of a cold start, where every free ambiguity zeroes its
 *    own arc's phase residuals — costs nothing and needs no special case.  The
 *    degeneracy that DOES matter is pushed into a @f$3\times 3@f$ eigenproblem
 *    where it can be named per direction.  That is a genuine simplification over
 *    the sibling card, not an oversight.
 * 2. **@f$M@f$ is eigen-guarded, never blind-inverted.**  With a per-epoch station
 *    clock and a ZWD estimated, the Up direction's information can collapse
 *    (clock and troposphere absorb vertical almost exactly).  A near-null
 *    eigenvalue reports that direction UNOBSERVABLE with @f$\mathrm{MDB}=+\infty@f$
 *    — @ref BaardaReliability.hpp's convention, where infinity is a CATEGORY and
 *    not a large finite number a consumer could act on — and @f$\hat\delta@f$ and
 *    @f$T@f$ are formed in the observable subspace with @f$q = \mathrm{rank}@f$
 *    degrees of freedom.  The absorption is thereby MEASURED, not worked around.
 *    (SPEC §2 writes the denominator as @f$3\hat\sigma_0^2@f$; @f$q@f$ is the same
 *    number whenever the geometry is full-rank and the only honest generalization
 *    when it is not — a @f$T@f$ divided by 3 on a rank-2 station would read 1/3
 *    too quiet forever.  @ref CongruenceSolve::dof carries @f$q@f$ so a reader can
 *    always tell which regime a row is in.)
 * 3. **Phase rows need no special casing.**  Under `carry_invariant` their
 *    @f$Q_{\tilde e}@f$ rows are @f$\approx 0@f$ (a free ambiguity fits its own
 *    arc exactly), so they contribute @f$\approx@f$ nothing to @f$M@f$ and the
 *    screen is honestly CODE-grade — expect a dm-class MDB.  On a leveled lineage
 *    the phase tier engages and the MDB drops toward cm-class.  The MDB table says
 *    which regime the run was in; nothing is tuned to hide the difference.
 *
 * ## THE SIGN CONVENTION, pinned
 *
 * @f$\hat\delta@f$ is the **error in the HELD coordinate**: positive East means
 * the held coordinate sits @f$\hat\delta_E@f$ metres too far East of where the
 * data puts the antenna.  SUBTRACT it to correct the station.
 *
 * The derivation, because a sign error here is a diagnostic that confidently
 * points the wrong way.  The measurement model linearizes about the held position
 * and writes @f$y \approx H_{pos}\,\Delta@f$ with @f$\Delta = p_{true}-p_{held}@f$
 * the correction to the linearization point (@c MeasurementModel::buildJacobianRow
 * sets that partial to @f$-\hat u@f$ for every code and phase row, and
 * @c StationAssembly::row_pos_enu carries it verbatim).  The reported quantity is
 * @f$d = -\Delta = p_{held}-p_{true}@f$, so this class uses
 * @f$H_B = -\,@f$`row_pos_enu`.  A @f$+5@f$ cm East perturbation INJECTED into a
 * held coordinate therefore reads @f$\hat\delta_E \approx +5@f$ cm — which is what
 * SPEC §6.1 and the §7.2 perturbed arm both assert, and what
 * `tests/test_station_congruence.cpp` pins.  @f$M@f$, @f$T@f$ and every MDB are
 * invariant to that overall sign; only @f$\hat\delta@f$ is not.
 *
 * ## Reading the ladder — @f$t_i@f$ IS A VARIANCE RATIO, NOT A SIGMA
 *
 * The rungs are shared with the prior-residual and block-Baarda censuses
 * (@ref NetworkPriorResidual::ladder) so one reader handles all three files.
 * **The UNIT is not the same.**  Those cards' @f$|t|@f$ is a standardized
 * residual, so rung 4 means "four sigma".  Here @f$T_i@f$ carries the @f$1/q@f$,
 * so @f$E[t_i]=1@f$ under @f$H_0@f$ for every @f$q@f$ and rung 4 means "this
 * station's coordinates carry four times the tension the model allows".
 *
 * ## @f$\hat\sigma_0@f$ — ONE global family, and that is the point
 *
 * @ref NetworkCarryVce, predict-then-update, never censored, with the block card's
 * two corrections applied verbatim because they are not optional for @f$b>1@f$:
 * the accumulator inverts the geometric mean of a @f$\chi^2_1@f$ variate
 * (@c kChi2GeoMean), so a @f$\chi^2_q@f$ statistic must be fed as its
 * @f$\chi^2_1@f$-EQUIVALENT via @ref blockChi2GeoMean, and @c seed_floor_m2 must
 * be passed as 1.0 (the 0.0 default silently becomes 1e-6, i.e.
 * @f$\hat\sigma_0=10^{-3}@f$ on an unwarmed family and every @f$t_i@f$ inflated by
 * @f$10^6@f$).
 *
 * The partition is ONE NETWORK-WIDE family, deliberately.  The block card splits
 * by constellation because its statistic is per-LOS; this one pools a whole
 * station.  A PER-STATION @f$\hat\sigma_0@f$ would let the faulty station
 * calibrate its own fault away — the screen would converge to "everything is
 * normal" precisely on the station it exists to find.  A per-constellation split
 * is not available either: the statistic spans every constellation the station
 * tracks by construction.
 *
 * ## Isolation / testing
 *
 * Pure numeric leaf in the @ref NetworkCarryVce / @ref NetworkBlockBaarda mould:
 * header-only, no ApplicationNetwork coupling, no StateManager, no I/O.  The
 * statistic itself (@ref congruenceStatistic) is a free function over dense
 * matrices so `tests/test_station_congruence.cpp` can drive it against an
 * independent dense-GLS oracle; the per-epoch trace / event / aggregate emission
 * lives in @c filter/NetworkCongruence.cpp, the only translation unit here that
 * knows about traces.
 */

#include "filter/BaardaReliability.hpp"   // delta_0 + the +infinity convention — ONE reliability convention in this tree
#include "filter/NetworkBlockBaarda.hpp"  // blockChi2GeoMean / kBlockChi2GeoMean1 — the chi2_b -> chi2_1 rescale a sibling card paid for
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkPriorResidual.hpp" // the shared ladder rungs
#include "filter/NetworkRowClassify.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
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

namespace GnssProcessor {

class NetworkNormalEquations;

// ===========================================================================
//  Conventions frozen as constants (SPEC §5 lists exactly three knobs)
// ===========================================================================

/// Relative eigenvalue floor on @f$M@f$: a direction whose eigenvalue is more than
/// ten decades below the largest carries no information at double precision and is
/// reported UNOBSERVABLE rather than inverted through.  Same decade, and the same
/// reasoning, as @ref kBlockCofactorRcondEps.
inline constexpr double kCongruenceEigenRelEps = 1e-10;

/// How much of an ENU axis may lie in @f$M@f$'s null space before that axis's MDB
/// is reported as @f$+\infty@f$.  @f$e_k^\top\delta@f$ is estimable only when
/// @f$e_k\in\mathrm{range}(M)@f$; this is that test with a numerical tolerance on
/// the squared null-space projection (the eigenvectors are orthonormal, so the
/// quantity is a fraction in @f$[0,1]@f$).  1e-6 ⇒ an axis more than 0.1 % tilted
/// into a collapsed direction cannot be separated from it, and a finite number
/// there would be exactly the lie SPEC §10 refuses to print.
inline constexpr double kCongruenceAxisNullTol = 1e-6;

/// @f$\hat\sigma_0@f$ forgetting half-life, in swept epochs.  NOT a knob: SPEC §5
/// lists three, and this shares D5 v2's value with the prior-residual and
/// block-Baarda censuses so all three cards' warm-up behaviour is one number.
inline constexpr double kCongruenceWindowEpochs = 60.0;

/// Station-epochs a family needs before @f$\hat\sigma_0@f$ is trusted.  D5's
/// value, in this card's unit.  Below it every station reports RAW FORMAL
/// (@f$t_i \equiv T_i@f$) and SAYS SO in its own column.
inline constexpr double kCongruenceWarmupSamples = 200.0;

/// Per-(station × label) cap on retained @f$\hat\delta@f$ / MDB samples.  The
/// median/MAD pair needs the samples; this bounds the memory at ~48 B × 2048 ≈
/// 98 kB per station-label (30 MB at 305 stations, armed only).  Overflow is
/// COUNTED and reported at run end — a silently truncated median reads as
/// "the whole run" when it is not (the no-silent-caps rule).
inline constexpr std::size_t kCongruenceMaxSamples = 2048;

/// The single @f$\hat\sigma_0@f$ family name.  See the header rationale: a
/// per-station split would let a faulty station standardize its own fault away.
inline constexpr const char* kCongruenceFamily = "net";

// ===========================================================================
//  The statistic (pure; no run state, no I/O)
// ===========================================================================

/// Datum-frame label of one station-epoch.  Deliberately NOT
/// @ref PriorResidualLabel: the RESPONDED tier there is an ARC-level estimator
/// response and has no station-level meaning, and an always-empty third ladder is
/// noise a reader has to learn to ignore.  SPEC §3 (INC-2) asks for exactly this
/// split, and standing invariant #7 is why it exists: a datum re-realization is an
/// event at both edges, so its station-epochs are counted apart rather than
/// dropped or silently pooled.
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
 * Everything here is RAW — no @f$\hat\sigma_0@f$ has been applied — because the
 * empirical rescaling is @ref NetworkCongruence's job and a feed site that
 * standardized its own input could standardize a fault away.
 */
struct CongruenceSolve {
    bool ok = false;            ///< a statistic was formed (at least one observable direction).
    int  b = 0;                 ///< rows contracted.
    int  dof = 0;               ///< @f$q@f$ — observable directions retained (0..3).
    Eigen::Vector3d delta = Eigen::Vector3d::Zero();  ///< @f$\hat\delta@f$ (m, ENU) — the HELD-COORDINATE ERROR (see the file banner's sign section).
    double T_raw = 0.0;         ///< @f$\hat\delta^\top M\hat\delta / q@f$ — per-dof, PRE-@f$\hat\sigma_0@f$.
    /// FORMAL per-axis coordinate MDB (m), @f$\delta_0\sqrt{(M^{+})_{kk}}@f$, in
    /// ENU order.  @c +infinity marks a direction that is not estimable — a
    /// CATEGORY, never a clamp (@ref BaardaReliability.hpp).  The EMPIRICAL variant
    /// is this times @f$\hat\sigma_0@f$ and is reported separately, never folded in
    /// here, for the reason the MDB card records: a silently pre-inflated MDB makes
    /// the formal model's optimism unauditable.
    Eigen::Vector3d mdb =
        Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());
    Eigen::Vector3d eval = Eigen::Vector3d::Zero();   ///< eigenvalues of @f$M@f$, ASCENDING (units 1/m²).
    Eigen::Vector3d null_dir = Eigen::Vector3d::Zero();///< ENU components of the SMALLEST-eigenvalue direction — which way the absorption points.
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
 *                  @f$\partial y/\partial\Delta@f$) must NEGATE it; see the file
 *                  banner.
 * @param w         per-row information weights @f$1/\sigma^2@f$ (length @c b).
 * @param Q         @f$b\times b@f$ residual cofactor
 *                  (@ref NetworkNormalEquations::blockResidualCofactor).  Used
 *                  FORWARD only — never inverted, so a rank-deficient @p Q is
 *                  admissible and common.
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
    // exact arithmetic, and the eigensolver below is a SelfAdjoint one that reads
    // a single triangle — an asymmetry would be silently discarded rather than
    // diagnosed.  Symmetrize.
    M = (0.5 * (M + M.transpose())).eval();
    if (!M.allFinite()) return out;
    const Eigen::Vector3d g = G.transpose() * e;
    if (!g.allFinite()) return out;

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(M);
    if (es.info() != Eigen::Success) return out;
    out.eval = es.eigenvalues();                 // ASCENDING (Eigen's contract)
    const Eigen::Matrix3d V = es.eigenvectors();
    const double lmax = out.eval(2);
    if (!(lmax > 0.0) || !std::isfinite(lmax)) return out;
    const double cut = eig_eps * lmax;

    // The pseudo-inverse over the OBSERVABLE subspace only.  Directions under the
    // cut are dropped rather than regularized: a ridge would manufacture a finite
    // MDB in a direction the data does not constrain, which is the one number this
    // card must never print.
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
    // component so the same geometry always reports the same vector — an
    // eigenvector's sign is arbitrary and a flapping one would look like motion.
    out.null_dir = V.col(0);
    Eigen::Index kmax = 0;
    out.null_dir.cwiseAbs().maxCoeff(&kmax);
    if (out.null_dir(kmax) < 0.0) out.null_dir = -out.null_dir;

    out.ok = true;
    return out;
}

/// Median of @p v (MUTATES @p v by partial sort).  NaN for an empty pool.
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
/// (× 1.4826).  The D-family robust convention; SPEC §3 (INC-2) makes the
/// median/MAD pair the reported uncertainty precisely BECAUSE there is no
/// @f$\sqrt N@f$ claim to make — D1 measured @f$K_{eff}(30)\approx 2@f$ on this
/// network, so per-epoch @f$\hat\delta@f$ samples do not average as white.
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
    /// Ladder rungs are SHARED with the prior-residual and block-Baarda censuses
    /// so the three cards' CSVs have one shape and one reader.  Read the unit
    /// warning in the file banner before comparing occupancies.
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
        long long n_null       = 0;  ///< …with @f$q=0@f$: NO direction observable, so no statistic at all (counted, never inverted).
        long long n_raw_formal = 0;  ///< …reported against @f$\hat\sigma_0=1@f$ because the family had not warmed up.  @f$t_i\equiv T_i@f$; flagged, never hidden.
        long long n_reported   = 0;  ///< …with @f$t_i>@f$ report threshold.
        std::array<long long, kLadderBins> bins{};  ///< CUMULATIVE exceedance: bins[k] = #station-epochs with t_i > ladder()[k].
        double    sum_log_t    = 0.0;///< sum log of the CALIBRATED @f$\chi^2_1@f$-equivalent (see @ref geoT), not of @f$t_i@f$.
        long long n_geo        = 0;  ///< station-epochs in @ref sum_log_t.
        /// INC-3 separability: [0] = @f$t_i>@f$ threshold, [1] = below; columns
        /// [0] = block-Baarda flagged NO line of sight of this station this epoch,
        /// [1] = it flagged at least one.  All-zero unless the block census is
        /// armed (an unarmed run has no verdict to join, which is DIFFERENT from a
        /// negative one and is reported as such).
        std::array<std::array<long long, 2>, 2> contingency{};

        // --- worst station-epoch's FULL identity (invariant #4: a finding needs a name)
        double max_t   = 0.0;   ///< largest @f$t_i@f$ seen.
        double max_T   = 0.0;   ///< its raw @f$T_i@f$ (pre-@f$\hat\sigma_0@f$).
        double max_tow = -1.0;  ///< its TOW.
        Eigen::Vector3d max_delta = Eigen::Vector3d::Zero();  ///< its @f$\hat\delta@f$ (m, ENU).
        int    max_dof = 0;     ///< its observable dimension.

        /// Fraction of station-epochs above ladder rung @p k (0 when empty).
        double frac(int k) const {
            if (n_epochs <= 0 || k < 0 || k >= kLadderBins) return 0.0;
            return static_cast<double>(bins[static_cast<std::size_t>(k)])
                 / static_cast<double>(n_epochs);
        }
        /// Tail-robust CALIBRATION indicator: the geometric mean of the
        /// @f$\chi^2_1@f$-equivalent, standardized statistic.  **1.0 means the
        /// scale has converged**, for any mix of @f$q@f$.  Deliberately NOT the
        /// geometric mean of @f$t_i@f$, whose null value @f$2e^{\psi(q/2)}/q@f$ is
        /// @f$q@f$-dependent and therefore unreadable across a population that
        /// mixes rank-3 and rank-2 stations.
        double geoT() const {
            if (n_geo <= 0) return 0.0;
            return std::exp(sum_log_t / static_cast<double>(n_geo)) / kBlockChi2GeoMean1;
        }
        void reset() { *this = Ladder{}; }
    };

    /// One retained station-epoch, for the INC-2 median/MAD readout.  48 bytes.
    struct Sample {
        Eigen::Vector3d d   = Eigen::Vector3d::Zero();  ///< @f$\hat\delta@f$ (m, ENU).
        Eigen::Vector3d mdb =
            Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());  ///< FORMAL MDB (m, ENU); @c +inf entries are counted, never averaged.
    };

    /// One station's run state.
    struct StationState {
        std::array<Ladder, kCongruenceLabels>              run{};
        std::array<std::vector<Sample>, kCongruenceLabels> samples{};
        std::array<long long, kCongruenceLabels>           dropped{{0, 0}};  ///< samples past @ref kCongruenceMaxSamples — COUNTED, never silent.
    };

    /// One station-epoch, as the feed site presents it.  RAW: no
    /// @f$\hat\sigma_0@f$ applied, so the feed site cannot standardize against the
    /// epoch it is contributing to.
    struct Input {
        int             station = -1;      ///< station roster index (identity only).
        CongruenceSolve solve{};           ///< the statistic (@ref congruenceStatistic).
        int             n_code = 0;        ///< code rows contracted (composition, for the run-end readout).
        int             n_phase = 0;       ///< phase rows contracted.
        CongruenceLabel label = CongruenceLabel::Clean;  ///< assigned at the FEED SITE — this class never labels.
        bool            block_known = false;  ///< INC-3: the block census was armed, so its verdict exists.
        bool            block_flagged = false;///< INC-3: it flagged >= 1 LOS of THIS station THIS epoch.
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
     * @param report_sigma  @c network.congruence_report_sigma — the trace / event
     *                      threshold on @f$t_i@f$.  REPORTING ONLY.
     */
    explicit NetworkCongruence(bool armed = false,
                               double window_epochs = kCongruenceWindowEpochs,
                               double min_samples = kCongruenceWarmupSamples,
                               double report_sigma = 4.0)
        : armed_(armed),
          // >= 0, not > 0: 0 is a LEGITIMATE request for "no warm-up bar,
          // calibrate from the first station-epoch".  Only a NEGATIVE value is a
          // mistake worth substituting a default for.
          min_samples_(min_samples >= 0.0 ? min_samples : kCongruenceWarmupSamples),
          report_sigma_(report_sigma > 0.0 ? report_sigma : 4.0),
          // ONE family (see the file banner).  split_by_const = FALSE: there is no
          // constellation axis here — the statistic spans every constellation the
          // station tracks.  seed_floor_m2 = 1.0 so an unwarmed family reads
          // sigma0 = 1 EXACTLY (0.0 silently becomes 1e-6).
          inflation_(armed ? std::vector<std::string>{kCongruenceFamily}
                           : std::vector<std::string>{},
                     window_epochs, /*split_by_const=*/false,
                     /*seed_floor_m2=*/1.0, /*seed_rate_m2ps=*/0.0) {}

    /// @return Whether the screen is armed (an unarmed instance is inert).
    bool   enabled() const      { return armed_; }
    double reportSigma() const  { return report_sigma_; }

    /**
     * @brief PREDICT — freeze this epoch's @f$\hat\sigma_0@f$ and clear the
     *        per-epoch counters.
     *
     * Order is the contract: the estimate is READ (through the PREVIOUS epoch)
     * before @ref NetworkCarryVce::beginEpoch decays the accumulator, so a station
     * is never standardized by a calibration it contributed to.  @c clean is
     * unconditionally true — a datum-changed epoch is a FRAME population to
     * MEASURE, not one to drop; that is what the label split is for.
     */
    void beginEpoch(double content_time_s, double tow) {
        if (!armed_) return;
        last_tow_ = tow;
        ++n_epochs_;
        const NetworkCarryVce::Estimate e =
            inflation_.estimate(kCongruenceFamily, std::string());
        // n_eff, NOT robust_floor > 0: an empty bucket returns seed_floor_, so
        // robust_floor is never a proxy for "has data".  The explicit n_eff > 0 is
        // what keeps a min_samples = 0 family from reading "warm" before a single
        // station-epoch has been folded.
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
     * @brief UPDATE — fold one station-epoch.
     * @return What happened, for the caller's events and counters.
     */
    StationStat addStation(const Input& in) {
        StationStat out;
        if (!armed_ || in.station < 0) return out;
        StationState& S = state_[in.station];
        Ladder& L = S.run[static_cast<std::size_t>(in.label)];

        // A NULL-geometry station-epoch is counted with its identity and never
        // inverted through — the position-space analogue of the block census's
        // UNCHECKABLE cell.  It is a WIRING/absorption reading, not a finding, and
        // conflating it with "swept and quiet" is the failure invariant #4 names.
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

        // --- NEVER CENSOR ---------------------------------------------------
        // The calibration is fed BEFORE any threshold is consulted, and with the
        // chi2_q -> chi2_1 rescaling the block card paid for: NetworkCarryVce
        // inverts the geometric mean of a chi2_1 variate, and T_raw*q ~ sigma0^2
        // chi2_q, so feeding sqrt(T_raw) directly would bias sigma0 by up to 1.8x
        // in sigma and the bias would MOVE with how many stations were rank-2.
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

        // INC-3, the card's contingency deliverable: a coordinate fault is
        // ALL-LOS coherent and should NOT excite the per-LOS block tier, while a
        // single-LOS fault should excite that tier and not this one.  The 2x2 is
        // how that partition gets MEASURED from day one instead of asserted.
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

        // INC-2: the retained sample.  Median/MAD needs the samples; a running
        // scalar cannot produce either, and the arithmetic mean is exactly the
        // statistic D1's K_eff ~ 2 says not to trust here.
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

    /// Stations that have been folded at least once, in ASCENDING roster order
    /// (std::map — the run-end table has to be reproducible for an A/B to mean
    /// anything).
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

    /// Per-component median MDB for one (station, label) over the FINITE samples,
    /// with the @f$+\infty@f$ population counted separately.  A median over a pool
    /// containing infinity is either meaningless or silently censored depending on
    /// the sort, so the unobservable epochs are reported as their own COUNT — the
    /// same rule the MDB card follows.
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

    double    sigma0_ = 1.0;   ///< frozen at beginEpoch (through the PREVIOUS epoch).
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
//  The sweep + emission (filter/NetworkCongruence.cpp)
// ===========================================================================

/// One reportable station-epoch, held until the emission seam writes its rows.
/// Every swept station produces one of these — unlike the block card's `hits`,
/// which holds only the loud ones — because the per-station TRACE row IS this
/// card's headline product (SPEC §7.3's MDB table is built from it) and the event
/// stream is only the rate-limited alarm on top.
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

/// Run-total tallies.  Standing invariant #4 in its statistical form: the failure
/// this card must be able to DISPROVE is "the screen ran and found nothing", which
/// is indistinguishable from "the screen never ran" without denominators — so
/// passes, station sweeps, rows and the DEGENERATE population are reported before
/// any finding is.
struct NetworkCongruenceTallies {
    long long passes         = 0;  ///< emission-seam invocations (did the wiring execute at all).
    long long epochs         = 0;  ///< …that carried at least one station.
    long long station_sweeps = 0;  ///< per-station congruence sweeps performed.
    long long rows           = 0;  ///< data rows contracted.
    long long no_geom_rows   = 0;  ///< swept data rows dropped for want of a usable position partial (an SH tie row, or a station assembled before the screen was armed).
    long long no_resid       = 0;  ///< stations skipped because the post-fit residual could not be formed.
    long long no_rows        = 0;  ///< stations skipped because NO row carried geometry.
    long long solve_failed   = 0;  ///< stations whose 3x3 information matrix produced no statistic at all.
    long long degenerate     = 0;  ///< station-epochs with q < 3 (at least one direction unobservable).
    long long reported       = 0;  ///< station-epochs over the report threshold.
    long long events         = 0;  ///< event rows emitted (rate-limited).
    double    wall_ms        = 0.0;///< run-total wall inside the sweep + emission.
    double    max_t          = 0.0;///< worst @f$t_i@f$ of the run.
    std::string max_id;            ///< its identity, "STATION dEN U=… @tow …".
};

struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Sweep one station's coordinate congruence — INC-1's costly tier.
 *
 * Called from the redundancy census's SWEPT branch (never from its free tier), so
 * the stride / targeted-escalation / trip schedule is inherited exactly and this
 * card adds no scheduler of its own (SPEC §3).  Everything it needs is already in
 * scope there: the kernel, the station's slot, the hoisted datum-frame covariance,
 * the row classes the census's classifier already produced, and the post-fit
 * residual the block card's rider already forms.
 *
 * Cost: ONE @f$b\times b@f$ cofactor over the station's data rows, one
 * @f$b\times 3@f$ contraction and one @f$3\times 3@f$ eigensolve.  The cofactor
 * is the same order as the @c rowRedundancy call the census already made for the
 * same station (both are dominated by the @f$N_{rr}^{-1}N_{sr}^\top@f$ solve and
 * the @f$n_{shared}^2@f$ Gram), which is what puts SPEC §4's "≤ 2× the redundancy
 * sweep it rides" budget within reach; @c tallies.wall_ms is the number that has
 * to be checked against it, which is why the sweep is timed here rather than
 * trusted.
 *
 * @param acc            The run accumulator (already @ref NetworkCongruence::beginEpoch'd).
 * @param kernel         Solved arrowhead kernel.
 * @param slot           This station's kernel slot (@c est_index_of, never the roster index).
 * @param station_index  Roster index, for identity only.
 * @param R              The station assembly (row stack + @c row_pos_enu).
 * @param cls            Per-row classes from the census's classifier.
 * @param resid          POST-FIT residuals @f$y - A_s x_s - A_r x_r@f$, in @c R.y's row order.
 * @param cov_free       Datum-frame shared covariance, built once by the census.
 * @param delta_0        Baarda non-centrality (@ref baardaDelta0) for the MDB.
 * @param datum_changed  This epoch re-realized the datum ⇒ FRAME (invariant #7).
 * @param block_known    The block census was armed, so INC-3's join has a verdict.
 * @param block_flagged  It flagged at least one LOS of THIS station THIS epoch.
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

/**
 * @brief Per-epoch emission — the per-station trace rows and the rate-limited
 *        event identities.
 *
 * Called from the driver's SB_FRAMEDIAG block AFTER @c networkRedundancyCensus
 * (which is what fed the accumulator this epoch).  Gated on
 * @c ctx.knobs.cg_on, so an unarmed run never enters this TU.
 */
void networkCongruenceEmit(NetworkRunContext& ctx, const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           NetworkRunTraces& traces, const SyncedEpoch& se,
                           const std::vector<NetStation>& stations);

/// Run-end readout (invariant #4).  Denominators first, then THE MDB TABLE —
/// SPEC §7.3's service deliverable: the smallest coordinate fault this network can
/// see, per station, per direction — then the ladders, then INC-3's 2×2.
void networkCongruenceReport(const NetworkRunContext& ctx,
                             const std::shared_ptr<spdlog::logger>& log,
                             const std::vector<NetStation>& stations);

}  // namespace GnssProcessor
