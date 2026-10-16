/**
 * @file NormalEquations.cpp
 * @brief Implementation of the arrowhead Schur-reduction kernel for the joint
 *        multi-station PPP-RTK network estimator (W4-S1).
 *
 * See NormalEquations.hpp for the full block structure and the
 * marginalization identities.  All operations are dense Eigen; the local blocks
 * are factorized once on entry (LDLᵀ) and the factor is reused for both the
 * Schur reduction and the per-station back-substitution.
 */

#include "network/solve/NormalEquations.hpp"

#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (parallel_carry: local GMGᵀ un-pin)
#include <Eigen/Eigenvalues>   // SelfAdjointEigenSolver (PD-safe fold INC-1c repair backstop)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>          // NaN for an out-of-range column in the stationRegression cross gather (F3)
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace GnssProcessor {

namespace {

/// Throw std::invalid_argument with a uniform "NetworkNormalEquations: ..." prefix.
[[noreturn]] void fail(const std::string& what) {
    throw std::invalid_argument("NetworkNormalEquations: " + what);
}

/// Require a square matrix of exactly @p n rows/cols.
void requireSquare(const Eigen::MatrixXd& m, Eigen::Index n, const char* name) {
    if (m.rows() != n || m.cols() != n) {
        std::ostringstream os;
        os << name << " must be " << n << "x" << n << ", got " << m.rows() << "x"
           << m.cols();
        fail(os.str());
    }
}

/// Require an (approximately) symmetric matrix.  A caller forming N = AᵀWA gets
/// symmetry to ~machine-eps; gross asymmetry signals a caller bug (e.g. only the
/// lower triangle of N_rr would otherwise be silently used by LDLᵀ).
void requireSymmetric(const Eigen::MatrixXd& m, const char* name) {
    if (m.size() == 0) {
        return; // empty block (e.g. a zero-local station) is trivially symmetric
    }
    const double asym = (m - m.transpose()).cwiseAbs().maxCoeff();
    const double scale = 1.0 + m.cwiseAbs().maxCoeff();
    if (asym > 1e-8 * scale) {
        std::ostringstream os;
        os << name << " must be symmetric (max |M - Mᵀ| = " << asym << ")";
        fail(os.str());
    }
}

/// Verify that an LDLᵀ factorization is positive-definite.  Eigen's @c info() does not
/// reject a singular or indefinite matrix: LDLᵀ factorizes it, and a later solve goes
/// through a pseudo-inverse.  A non-positive pivot, or a pivot below 1e-12 of the
/// largest, therefore throws, so a rank-deficient reduced system (an under-specified
/// S-basis datum) fails instead of returning an under-determined solution.  @p context
/// is thrown verbatim.
void requireFactorPd(const Eigen::LDLT<Eigen::MatrixXd>& ldlt,
                     const std::string& context) {
    bool ok = (ldlt.info() == Eigen::Success);
    if (ok) {
        const Eigen::VectorXd d = ldlt.vectorD();
        if (d.size() > 0) {
            const double dmax = d.cwiseAbs().maxCoeff();
            const double dmin = d.minCoeff();
            ok = (dmax > 0.0) && (dmin > 0.0) && (dmin >= 1e-12 * dmax);
        }
    }
    if (!ok) {
        fail(context);
    }
}

// --- PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md §4) --------------
// The DD-prior fold inverts M = (Pᶻ+B)⁻¹, and a corrupted carry shows up there.  The
// joint [band-1; band-2] lane block is near-singular at birth, because same-chord rows
// of the two bands are near-duplicates through shared geometry.  Operand scales range
// from 1e-4 (mature) to 1e6 (fresh), and rounding over long GEMM chains can push the
// block's smallest eigenvalues through zero.  The legacy screen responds by freezing the
// block (M = 0, posterior equals prior) with no counter.  The PD-safe screen equilibrates
// the arithmetic so such violations shrink to round-off, repairs the round-off it still
// finds, and counts both.
//
// Relative pivot floor.  Same value as the @ref requireFactorPd constant, so the DD
// fold's rank test and the shared-system rank test agree.
constexpr double kDdPivotRelTol = 1e-12;
/// Bound on the INC-1c eigenvalue repair, relative to λmax.  SPEC §4(c) requires a clamp
/// above this bound to be reported rather than hidden, and §5(a) rebirths a block that
/// still cannot factor after (c), naming a corrupt restored blob as a reachable trigger.
/// Both hold only if a clamp this large is refused: otherwise the clamp always succeeds,
/// any negative eigenvalue is rewritten to +1e-12·λmax, and the §5(a) trigger cannot be
/// reached.  Clamps within the bound are repaired and counted; a larger clamp marks the
/// block degenerate and leaves it to the bounded, logged rebirth.
constexpr double kDdRepairLoudRel = 1e-6;
/// Diagonal floor for the equilibration, relative to the largest |diagonal|.  With a
/// non-positive diagonal (the corrupt-carry case), an absolute floor would give a scale
/// factor near 1e150 and overflow the scaled matrix; a relative floor keeps S finite, so
/// the PD screen makes the decision rather than an inf.
constexpr double kDdEquilDiagFloorRel = 1e-16;

/// Explicit symmetrized inverse from a computed LDLᵀ factor.  The expressions are the
/// ones the serial fold used inline, unchanged, so the legacy path rounds identically
/// (standing invariant #6).
///
/// @p station_threads > 1 (@c network.parallel_carry) splits the identity columns across
/// threads.  Each column solve A x_j = e_j through the shared factor has no cross-column
/// arithmetic, so the split result is byte-identical to the single solve for a given
/// factor.  The factorization itself stays serial.
Eigen::MatrixXd inverseFromLdlt(const Eigen::LDLT<Eigen::MatrixXd>& ldlt, Eigen::Index K,
                                int station_threads) {
    Eigen::MatrixXd M;
#ifdef _OPENMP
    if (station_threads > 1 && K > 1) {
        M.resize(K, K);
        const int nt = static_cast<int>(
            std::min<Eigen::Index>(static_cast<Eigen::Index>(station_threads), K));
        #pragma omp parallel num_threads(nt)
        {
            const int tid = omp_get_thread_num();
            const int rnt = omp_get_num_threads();
            const Eigen::Index lo = (K * tid) / rnt;
            const Eigen::Index hi = (K * (tid + 1)) / rnt;
            if (hi > lo) {
                Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(K, hi - lo);
                for (Eigen::Index j = lo; j < hi; ++j) rhs(j, j - lo) = 1.0;
                M.middleCols(lo, hi - lo) = ldlt.solve(rhs);
            }
        }
    } else
#endif
        M = ldlt.solve(Eigen::MatrixXd::Identity(K, K));
    M = (0.5 * (M + M.transpose())).eval();
    return M;
}

/// One block's middle-inverse screen result (both arithmetic paths produce this shape).
struct DdMiddleInverse {
    Eigen::MatrixXd M;            ///< (Pᶻ+B)⁻¹, or K×K zero when the screen tripped (inert fold).
    Eigen::MatrixXd Minv;         ///< the matrix M actually inverts: Pᶻ+B, post-repair if (c) fired.
    bool   degenerate{false};     ///< the screen tripped and nothing rescued it ⇒ M = 0.
    double min_pivot_rel{0.0};    ///< d_min/d_max of the original (pre-repair) factor.
    bool   repaired{false};       ///< an eigenvalue clamp was applied (INC-1c).
    double repair_worst{0.0};     ///< |worst clamped eigenvalue| / λ_max (INC-1c).
};

/// Legacy middle inverse: the arithmetic before the PD-safe screen, unchanged.  The one
/// addition is a read of @c min_pivot_rel from the factor already computed (INC-0); the
/// decision is still exactly `info() != Success || vectorD().minCoeff() <= 0.0`.
DdMiddleInverse ddMiddleInverseLegacy(const Eigen::MatrixXd& PzB, Eigen::Index K,
                                      int station_threads) {
    DdMiddleInverse r;
    r.Minv = PzB;
    Eigen::LDLT<Eigen::MatrixXd> ldlt(PzB);   // O(K³/3) factor, kept serial for byte-identity
    if (ldlt.info() == Eigen::Success) {      // read only; does not affect the decision (INC-0)
        const Eigen::VectorXd d = ldlt.vectorD();
        const double dmax = d.cwiseAbs().maxCoeff();
        r.min_pivot_rel = (dmax > 0.0) ? (d.minCoeff() / dmax) : 0.0;
    }
    if (ldlt.info() != Eigen::Success || ldlt.vectorD().minCoeff() <= 0.0) {
        r.M = Eigen::MatrixXd::Zero(K, K);
        r.degenerate = true;
    } else {
        r.M = inverseFromLdlt(ldlt, K, station_threads);
    }
    return r;
}

/// PD-safe middle inverse (@c network.carry_invariant_pd_safe), SPEC §4(b) and (c).
///
/// (b) Equilibrate: S = diag(1/√max(A_ii, ε)) makes every pivot of Ã = S A S of order 1,
///     so the rank test `d_min ≥ 1e-12·d_max` measures conditioning rather than the 1e10
///     spread between a mature (1e-4) and a fresh (1e6) row.  The unscale M = S Ã⁻¹ S is
///     exact (A⁻¹ = S (S A S)⁻¹ S for diagonal S); only the rounding path differs.
/// (c) Repair: on a trip, clamp Ã's eigenvalues below 1e-12·λmax up to that floor and
///     refactor.  The repair is bounded and counted.  A clamp larger than
///     kDdRepairLoudRel·λmax (1e-6) is refused rather than hidden: the block is returned
///     degenerate with M = 0, @c repair_worst still records the clamp size for the
///     caller's warning, and the install rebirths the block (INC-2a).
DdMiddleInverse ddMiddleInversePdSafe(const Eigen::MatrixXd& PzB, Eigen::Index K,
                                      int station_threads) {
    DdMiddleInverse r;
    r.Minv = PzB;
    const Eigen::VectorXd dg = PzB.diagonal();
    const double dmax_abs = dg.cwiseAbs().maxCoeff();
    const double eps = (dmax_abs > 0.0) ? (kDdEquilDiagFloorRel * dmax_abs) : 1.0;
    Eigen::VectorXd s(K);
    for (Eigen::Index i = 0; i < K; ++i) s(i) = 1.0 / std::sqrt(std::max(dg(i), eps));
    Eigen::MatrixXd At = s.asDiagonal() * PzB * s.asDiagonal();
    At = (0.5 * (At + At.transpose())).eval();

    Eigen::LDLT<Eigen::MatrixXd> ldlt(At);
    bool ok = (ldlt.info() == Eigen::Success);
    if (ok) {
        const Eigen::VectorXd d = ldlt.vectorD();
        const double dmax = d.cwiseAbs().maxCoeff();
        const double dmin = d.minCoeff();
        r.min_pivot_rel = (dmax > 0.0) ? (dmin / dmax) : 0.0;
        ok = (dmax > 0.0) && (dmin > 0.0) && (dmin >= kDdPivotRelTol * dmax);
    }
    if (ok) {   // the common case: scale-following arithmetic, no repair
        const Eigen::MatrixXd Ainv = inverseFromLdlt(ldlt, K, station_threads);
        r.M = s.asDiagonal() * Ainv * s.asDiagonal();
        r.M = (0.5 * (r.M + r.M.transpose())).eval();
        return r;
    }

    // ---- (c) repair backstop: eigenvalue clamp on the equilibrated matrix -------
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(At);
    if (es.info() != Eigen::Success) {
        r.M = Eigen::MatrixXd::Zero(K, K);
        r.degenerate = true;                 // the install rebirths this block (INC-2a)
        return r;
    }
    Eigen::VectorXd lam = es.eigenvalues();
    const double lmax = lam.cwiseAbs().maxCoeff();
    if (!(lmax > 0.0)) {                     // an all-zero block carries no information
        r.M = Eigen::MatrixXd::Zero(K, K);
        r.degenerate = true;
        return r;
    }
    const double lfloor = kDdPivotRelTol * lmax;
    double worst = 0.0;
    long long nclamp = 0;
    for (Eigen::Index i = 0; i < K; ++i) {
        if (lam(i) < lfloor) {
            worst = std::max(worst, std::abs(lam(i)));
            lam(i) = lfloor;
            ++nclamp;
        }
    }
    r.repair_worst = worst / lmax;
    if (r.repair_worst > kDdRepairLoudRel) {
        // Beyond round-off.  Clamping this would turn an indefinite block into one that
        // passes the PD screen and conceal the condition the screen exists to report.
        // Refuse it and leave the block to the bounded, counted and logged rebirth (INC-2a).
        r.M = Eigen::MatrixXd::Zero(K, K);
        r.degenerate = true;
        return r;
    }
    r.repaired     = (nclamp > 0);
    Eigen::MatrixXd Arep = es.eigenvectors() * lam.asDiagonal()
                         * es.eigenvectors().transpose();
    Arep = (0.5 * (Arep + Arep.transpose())).eval();
    Eigen::LDLT<Eigen::MatrixXd> ldlt2(Arep);
    bool ok2 = (ldlt2.info() == Eigen::Success);
    if (ok2) {
        const Eigen::VectorXd d2 = ldlt2.vectorD();
        const double d2max = d2.cwiseAbs().maxCoeff();
        ok2 = (d2max > 0.0) && (d2.minCoeff() > 0.0)
              && (d2.minCoeff() >= kDdPivotRelTol * d2max);
    }
    if (!ok2) {   // inert fold; the install rebirths the block (INC-2a)
        r.M = Eigen::MatrixXd::Zero(K, K);
        r.degenerate = true;
        r.repaired = false;                  // the clamp did not rescue it; do not count it
        return r;
    }
    const Eigen::MatrixXd Ainv = inverseFromLdlt(ldlt2, K, station_threads);
    r.M = s.asDiagonal() * Ainv * s.asDiagonal();
    r.M = (0.5 * (r.M + r.M.transpose())).eval();
    // Keep the D5 cache consistent with the matrix M actually inverts: Cov(v) reads
    // fd.Minv, and handing it the pre-repair (indefinite) Pᶻ+B would make the innovation
    // covariance disagree with the fold that produced the innovation.
    const Eigen::VectorXd sinv = s.cwiseInverse();
    r.Minv = sinv.asDiagonal() * Arep * sinv.asDiagonal();
    r.Minv = (0.5 * (r.Minv + r.Minv.transpose())).eval();
    return r;
}

// --- Section timers (network.trace_section_timers, tier D0) ------------------
// RAII accumulator with no overhead when disabled.  With @p on false it takes no
// timestamp and touches nothing, so an unarmed run makes no clock reads and is
// numerically byte-identical; with @p on true it adds its scope's wall-clock ms to
// @p acc on destruction.  Used only by the instrumented kernel primitives below.
using PerfClock = std::chrono::steady_clock;

inline double perfMsSince(PerfClock::time_point t0) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t0).count();
}

struct ScopedAccum {
    bool on;
    double* acc;
    PerfClock::time_point t0;
    ScopedAccum(bool on_, double* acc_) : on(on_), acc(acc_) {
        if (on) t0 = PerfClock::now();
    }
    ~ScopedAccum() {
        if (on) *acc += perfMsSince(t0);
    }
};

// RAII un-pin of Eigen's internal threading around one dense GEMM (the fold's GMGᵀ),
// used under parallel_carry.  The saved thread count is restored on every scope exit,
// including a throw mid-GEMM, so no exit path leaves Eigen un-pinned for the next
// block's OpenMP region (same pattern as the driver-side CarryEigenScope).  With @p on
// false it makes no Eigen thread-count call, so the default path is unchanged.
struct ScopedEigenThreads {
    bool on;
    int  saved;
    ScopedEigenThreads(bool on_, int nthreads) : on(on_), saved(on_ ? Eigen::nbThreads() : 0) {
        if (on) Eigen::setNbThreads(nthreads);
    }
    ~ScopedEigenThreads() { if (on) Eigen::setNbThreads(saved); }
    ScopedEigenThreads(const ScopedEigenThreads&) = delete;
    ScopedEigenThreads& operator=(const ScopedEigenThreads&) = delete;
};

} // namespace

NetworkNormalEquations::NetworkNormalEquations(Eigen::Index n_shared)
    : n_shared_(n_shared),
      N_ss_(Eigen::MatrixXd::Zero(n_shared, n_shared)),
      u_s_(Eigen::VectorXd::Zero(n_shared)),
      reduced_N_ss_(Eigen::MatrixXd::Zero(n_shared, n_shared)),
      reduced_u_s_(Eigen::VectorXd::Zero(n_shared)) {
    if (n_shared < 0) {
        fail("n_shared must be non-negative");
    }
}

void NetworkNormalEquations::reset(Eigen::Index n_shared) {
    if (n_shared < 0) {
        fail("reset: n_shared must be non-negative");
    }
    n_shared_ = n_shared;
    // setZero(r,c) reuses the existing buffer when the size is unchanged and
    // reallocates only on a size change.  The shared-block size drifts slowly as
    // the satellite set changes (one modest matrix, not the per-station pool), so
    // this is negligible churn.
    N_ss_.setZero(n_shared, n_shared);
    u_s_.setZero(n_shared);
    reduced_N_ss_.setZero(n_shared, n_shared);
    reduced_u_s_.setZero(n_shared);
    // Keep the stations_ pool (its dense per-station buffers persist for reuse) and
    // mark every slot inactive; addStation overwrites pooled slots in place, so there
    // is no per-epoch allocate/free churn.
    n_active_ = 0;
    reduced_ = false;
    reduce_recompute_count_ = 0;
    sumzero_gauges_.clear();
    station_sumzero_.clear();
    sumzero_folded_ = false;
    dd_priors_.clear();
    station_ddprior_.clear();
    ddprior_folded_ = false;
    dd_fold_stats_ = DdFoldStats{};   // dd_fold_pd_safe_ is configuration and survives (INC-0)
}

void NetworkNormalEquations::addSharedInfo(const Eigen::MatrixXd& delta_info,
                                           const Eigen::VectorXd& delta_rhs) {
    requireSquare(delta_info, n_shared_, "addSharedInfo: delta_info");
    if (delta_rhs.size() != n_shared_) {
        fail("addSharedInfo: delta_rhs size mismatch");
    }
    N_ss_ += delta_info;
    u_s_ += delta_rhs;
    // Reduction-preserving fast path.  A shared-border increment passes through the
    // Schur reduction unchanged: the per-station subtrahends N_sr (N_rr)⁻¹ N_rs depend
    // only on N_sr and N_rr, never on N_ss_, so the reduced normals shift by the same
    // delta as the unreduced normals.  When the cached reduction is current, the delta
    // is added to it directly and it stays current.  This avoids a full re-reduction
    // (the expensive per-station loop) for a post-reduce prior such as the driver's D2
    // carry-forward, which is applied after the null-space gate's reduce() and before
    // the solve.  The increment is symmetrized (0.5·(d + dᵀ)) so the update is exact
    // for any symmetric delta and matches reduce()'s own symmetrization (a diagonal
    // carry-forward prior is already bit-symmetric; a w·HᵀH constraint increment in
    // general is not, to the last bit).
    if (reduced_) {
        reduced_N_ss_ += 0.5 * (delta_info + delta_info.transpose());
        reduced_u_s_ += delta_rhs;
    }
    // else: reduced_ is already false; the next reduce() recomputes from scratch.
}

std::size_t NetworkNormalEquations::addStation(const Eigen::MatrixXd& N_ss_r,
                                               const Eigen::MatrixXd& N_sr_r,
                                               const Eigen::MatrixXd& N_rr_r,
                                               const Eigen::VectorXd& u_s_r,
                                               const Eigen::VectorXd& u_r_r,
                                               const StationPrior& prior) {
    const Eigen::Index n_r = N_rr_r.rows();

    requireSquare(N_ss_r, n_shared_, "addStation: N_ss_r");
    if (N_sr_r.rows() != n_shared_ || N_sr_r.cols() != n_r) {
        fail("addStation: N_sr_r must be n_shared x n_local");
    }
    requireSquare(N_rr_r, n_r, "addStation: N_rr_r");
    if (u_s_r.size() != n_shared_) {
        fail("addStation: u_s_r size mismatch");
    }
    if (u_r_r.size() != n_r) {
        fail("addStation: u_r_r size mismatch");
    }
    // The normal-equation blocks are symmetric by construction (AᵀWA); reject a
    // grossly asymmetric input so a caller bug is caught here, not silently
    // half-used by the LDLᵀ factorization / final symmetrization.
    requireSymmetric(N_ss_r, "addStation: N_ss_r");
    requireSymmetric(N_rr_r, "addStation: N_rr_r");

    // --- Station prior before the PD test (network.station_prior_before_pd, RF-PD-fix)
    // The station's own diagonal prior rows are folded into N_rr / u_r before the
    // factorization below, so the positive-definiteness test judges the matrix the
    // solve will use.  The whole addStationPrior contract is validated here, because
    // a throw must leave the border untouched and this runs before the first mutation
    // of anything shared.
    const Eigen::Index n_pri = static_cast<Eigen::Index>(prior.cols.size());
    if (n_pri > 0) {
        if (prior.dNrr.size() != n_pri || prior.dur.size() != n_pri) {
            fail("addStation: prior cols / dNrr / dur size mismatch");
        }
        if (n_r == 0) {
            fail("addStation: station prior supplied but the station has no local block");
        }
        for (Eigen::Index i = 0; i < n_pri; ++i) {
            const Eigen::Index c = prior.cols[static_cast<std::size_t>(i)];
            if (c < 0 || c >= n_r) {
                fail("addStation: prior local column out of range");
            }
            if (!(prior.dNrr(i) >= 0.0)) {
                fail("addStation: prior information increment must be non-negative");
            }
            if (!std::isfinite(prior.dur(i))) {
                fail("addStation: prior RHS increment is not finite");
            }
        }
    }

    // Build and validate this station's local block before committing anything to the
    // shared border.  addStation throws on a non-positive-definite local block (a
    // station with an unobservable nuisance, e.g. a GPS-only station whose Galileo-ISB
    // column is structurally zero), and the driver drops such a station.  A station
    // that throws must leave N_ss_ and u_s_ untouched.  If N_ss_r were accumulated
    // before the throw, its shared contribution would stay in the border with no
    // compensating Schur term (the station never enters stations_, so reduce() never
    // subtracts its N_sr Nrr⁻¹ N_rs).  Its observations would then pin the shared
    // parameters they touch without the receiver-clock absorption: a GPS-only station
    // pins the GPS satellite-clock common-mode gauge, the null-space rank gate sees one
    // fewer free gauge than declared, and every epoch is withheld.  In the global
    // PPP-RTK network this produced zero emission; the bᵀWb of the dropped GPS-only
    // stations summed to the spurious GPS-common pin.
    //
    // The station is built into a reused pool slot: its dense buffers persist across
    // epochs, and the Eigen .compute() and assignments below reuse the slot's storage
    // when the size matches.  The pool grows only on a new high-water station count.
    // A station that throws below leaves n_active_ unadvanced and N_ss_/u_s_
    // untouched, so its partially written slot is overwritten by the next addStation
    // and the singular station leaves no trace.
    if (n_active_ >= stations_.size()) {
        stations_.emplace_back();
    }
    Station& st = stations_[n_active_];
    st.n_local = n_r;

    // Fold the prior into a local copy, never into the caller's block, which the
    // driver still owns (its `R` is reused for the epoch's diagnostics).  With an
    // empty prior no copy is taken and every line below reads the caller's N_rr_r,
    // so the off path is byte-identical (RF-PD-fix).
    Eigen::MatrixXd N_rr_pre;
    if (n_pri > 0) {
        N_rr_pre = N_rr_r;
        for (Eigen::Index i = 0; i < n_pri; ++i) {
            const Eigen::Index c = prior.cols[static_cast<std::size_t>(i)];
            N_rr_pre(c, c) += prior.dNrr(i);   // information is additive on the diagonal
        }
    }
    const Eigen::MatrixXd& N_rr_use = (n_pri > 0) ? N_rr_pre : N_rr_r;

    if (n_r > 0) {
        // Factorize the local block once; reduce() and reconstructStation() reuse
        // the factor.  LDLᵀ is robust for the symmetric local normals.  The caller
        // folds in the station prior so the block is full-rank, either inside
        // N_rr_r or through `prior` (RF-PD-fix), which is already in N_rr_use here
        // and therefore inside the PD test below.
        st.Nrr_ldlt.compute(N_rr_use);
        if (st.Nrr_ldlt.info() != Eigen::Success) {
            fail("addStation: LDLᵀ factorization of N_rr_r failed (non-finite?)");
        }
        // info() == Success does not exclude a singular or indefinite block: LDLᵀ
        // factorizes those too, and a back-solve through the resulting
        // pseudo-inverse corrupts the reduction, which then surfaces downstream as a
        // misleading "reduced datum" failure.  A non-positive-definite local block is
        // caught here, where the cause is clear (the station's prior is too weak or a
        // nuisance parameter is unobservable).
        const Eigen::VectorXd d = st.Nrr_ldlt.vectorD();
        const double d_min = d.minCoeff();
        const double d_max = d.cwiseAbs().maxCoeff();
        // Reference scale for the relative rank test.  Default: the largest pivot.
        // Pin-aware (setStationPdMedianScale): the median |pivot|.  A deliberate
        // large-weight gauge-pin row in the block (the prioritized phase-datum
        // anchor's 1e10 ambiguity pin) raises the largest pivot by about 10 orders
        // of magnitude, and the test would then eject a marginal station whose
        // weakest genuine parameter sits at ~1e-3 (usable).  The median is robust
        // to any minority of pin-scale pivots and still catches a singular
        // direction (a pivot many orders below block scale).
        double d_ref = d_max;
        if (station_pd_median_scale_ && d.size() > 0) {
            Eigen::VectorXd a = d.cwiseAbs();
            std::vector<double> v(a.data(), a.data() + a.size());
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                             v.end());
            d_ref = v[v.size() / 2];
        }
        if (d_min <= 0.0 || d_min < 1e-12 * d_ref) {
            // --- Null direction of a rejected block (network.trace_station_pd_null, RF-PD)
            // The error message reports only a pivot magnitude, and the caller drops
            // the whole station on a number that identifies no parameter.  When armed,
            // one SelfAdjointEigenSolver pass over the block the test just rejected
            // stores its smallest eigenpair for the caller's catch.  The LDLᵀ pivot
            // index is not used: LDLᵀ permutes (transpositionsP), so pivot k is not
            // local column k and would name the wrong parameter.  Only a rejected block
            // pays the O(n³) cost, and with the knob off the branch allocates and calls
            // nothing, so the rejection path is byte-identical.
            last_pd_failure_.valid = false;
            if (station_pd_null_trace_) {
                Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N_rr_use);
                if (es.info() == Eigen::Success && es.eigenvalues().size() > 0) {
                    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
                    const double ev_max = ev.cwiseAbs().maxCoeff();
                    const double cut    = 1e-12 * ev_max;
                    int n_below = 0;
                    for (Eigen::Index k = 0; k < ev.size(); ++k)
                        if (std::abs(ev(k)) < cut) ++n_below;
                    last_pd_failure_.valid       = true;
                    last_pd_failure_.n_local     = n_r;
                    last_pd_failure_.d_min       = d_min;
                    last_pd_failure_.d_ref       = d_ref;
                    last_pd_failure_.d_max       = d_max;
                    last_pd_failure_.ev_min      = ev(0);
                    last_pd_failure_.ev_max      = ev_max;
                    last_pd_failure_.n_eig_below = n_below;
                    last_pd_failure_.null_vec    = es.eigenvectors().col(0);
                }
            }
            std::ostringstream os;
            os << "addStation: N_rr_r is not positive-definite (min LDLᵀ pivot "
               << d_min << " vs max " << d_max
               << (station_pd_median_scale_
                       ? ", pin-aware median-scale test" : "")
               << ") — station prior too weak or an unobservable local parameter";
            fail(os.str());
        }
    }

    // Local block accepted: record it and commit this station's shared-border
    // contribution, kept in step with the Schur term reduce() will subtract.
    st.N_sr = N_sr_r;
    st.u_r = u_r_r;
    // The prior's RHS half (RF-PD-fix).  N_sr is untouched: a diagonal local prior is
    // reduction-orthogonal to the border (the addStationPrior rationale).  These are
    // the same additions, onto the same base, in the same ascending column order that
    // addStationPrior would make, so u_r is bit-identical to the prior-after result
    // for a station both orders admit.
    for (Eigen::Index i = 0; i < n_pri; ++i) {
        st.u_r(prior.cols[static_cast<std::size_t>(i)]) += prior.dur(i);
    }
    N_ss_ += N_ss_r;
    u_s_ += u_s_r;

    reduced_ = false;
    return n_active_++;
}

std::size_t NetworkNormalEquations::addStationObservations(
    const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
    const Eigen::MatrixXd& W, const Eigen::VectorXd& y,
    const StationPrior& prior) {
    ScopedAccum _perf(perf_timing_, &timers_.add_station_ms);  // assembly primitive timer (D0)
    if (perf_timing_) ++timers_.add_station_calls;
    const Eigen::Index m = A_s.rows();
    const Eigen::Index n_r = A_r.cols();

    if (A_s.cols() != n_shared_) {
        fail("addStationObservations: A_s must have n_shared columns");
    }
    if (A_r.rows() != m) {
        fail("addStationObservations: A_s and A_r must have the same row count");
    }
    requireSquare(W, m, "addStationObservations: W");
    if (y.size() != m) {
        fail("addStationObservations: y size mismatch");
    }

    // Form normals with the full (possibly correlated) weight.  WA_s / WA_r are
    // computed once and reused.
    const Eigen::MatrixXd WA_s = W * A_s;          // m × n_s
    const Eigen::MatrixXd WA_r = W * A_r;          // m × n_r
    const Eigen::MatrixXd N_ss_r = A_s.transpose() * WA_s;   // n_s × n_s
    const Eigen::MatrixXd N_sr_r = A_s.transpose() * WA_r;   // n_s × n_r
    const Eigen::MatrixXd N_rr_r = A_r.transpose() * WA_r;   // n_r × n_r
    const Eigen::VectorXd u_s_r = WA_s.transpose() * y;      // n_s
    const Eigen::VectorXd u_r_r = WA_r.transpose() * y;      // n_r

    return addStation(N_ss_r, N_sr_r, N_rr_r, u_s_r, u_r_r, prior);
}

std::size_t NetworkNormalEquations::addStationObservations(
    const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
    const Eigen::VectorXd& w_diag, const Eigen::VectorXd& y,
    const StationPrior& prior) {
    ScopedAccum _perf(perf_timing_, &timers_.add_station_ms);  // assembly primitive timer (D0)
    if (perf_timing_) ++timers_.add_station_calls;
    const Eigen::Index m = A_s.rows();
    // Validate all dimensions before forming any product: on a mismatch the
    // asDiagonal products below would trip a hard Eigen assertion (Debug crash,
    // Release UB) instead of throwing std::invalid_argument.
    if (A_s.cols() != n_shared_) {
        fail("addStationObservations: A_s must have n_shared columns");
    }
    if (A_r.rows() != m) {
        fail("addStationObservations: A_s and A_r must have the same row count");
    }
    if (w_diag.size() != m) {
        fail("addStationObservations: w_diag size mismatch");
    }
    if (y.size() != m) {
        fail("addStationObservations: y size mismatch");
    }

    // Diagonal weight: form normals via the asDiagonal product without
    // materializing the m × m matrix.
    const Eigen::MatrixXd WA_s = w_diag.asDiagonal() * A_s;  // m × n_s
    const Eigen::MatrixXd WA_r = w_diag.asDiagonal() * A_r;  // m × n_r

    const Eigen::MatrixXd N_ss_r = A_s.transpose() * WA_s;
    const Eigen::MatrixXd N_sr_r = A_s.transpose() * WA_r;
    const Eigen::MatrixXd N_rr_r = A_r.transpose() * WA_r;
    const Eigen::VectorXd u_s_r = WA_s.transpose() * y;
    const Eigen::VectorXd u_r_r = WA_r.transpose() * y;

    return addStation(N_ss_r, N_sr_r, N_rr_r, u_s_r, u_r_r, prior);
}

void NetworkNormalEquations::addStationPrior(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Eigen::VectorXd& delta_Nrr_diag, const Eigen::VectorXd& delta_ur) {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::addStationPrior: "
                                "station index out of range");
    }
    const Eigen::Index k = static_cast<Eigen::Index>(local_cols.size());
    if (delta_Nrr_diag.size() != k || delta_ur.size() != k) {
        fail("addStationPrior: local_cols / delta_Nrr_diag / delta_ur size mismatch");
    }
    if (k == 0) {
        return;  // genuine no-op: leaves the cached factor and reduced_ untouched
    }
    Station& st = stations_[station];
    if (st.n_local == 0) {
        fail("addStationPrior: station has no local block to prior");
    }
    // Validate everything before mutating the cached factor: a rank-1 update is
    // applied in place, so a throw partway through would leave N_rr half-updated.
    for (Eigen::Index i = 0; i < k; ++i) {
        const Eigen::Index c = local_cols[i];
        if (c < 0 || c >= st.n_local) {
            fail("addStationPrior: local column out of range");
        }
        if (!(delta_Nrr_diag(i) >= 0.0)) {
            fail("addStationPrior: information increment must be non-negative");
        }
        // A non-finite RHS increment would corrupt u_r (and so the shared solve)
        // without tripping any PD check, because the rank-1 factor update does not
        // involve u_r.  It is rejected here, under the kernel's contract that all
        // input is validated before any mutation.
        if (!std::isfinite(delta_ur(i))) {
            fail("addStationPrior: RHS increment is not finite");
        }
    }
    // Fold each diagonal increment Λ_c into N_rr by a rank-1 LDLᵀ update
    // (N_rr ← N_rr + Λ_c·e_c·e_cᵀ) and accumulate the RHS η_c into u_r.  A
    // positive Λ_c keeps the factor positive-definite, so no PD re-check is
    // needed; a zero Λ_c is skipped (a no-op column).  The loop follows the
    // caller's (deterministic, ascending) order, so the update sequence, and with
    // it the ULP-level result, is reproducible.
    Eigen::VectorXd e_c = Eigen::VectorXd::Zero(st.n_local);
    bool any = false;
    for (Eigen::Index i = 0; i < k; ++i) {
        const Eigen::Index c = local_cols[i];
        st.u_r(c) += delta_ur(i);
        const double lambda = delta_Nrr_diag(i);
        if (lambda > 0.0) {
            e_c(c) = 1.0;
            st.Nrr_ldlt.rankUpdate(e_c, lambda);
            e_c(c) = 0.0;  // restore the scratch unit vector for the next column
            any = true;
        }
    }
    if (any) {
        reduced_ = false;  // the station's Schur subtrahend changed → reduce() must rerun
    }
}

void NetworkNormalEquations::reduce(int n_threads) {
    // Idempotency guard: the reduced form is recomputed only when a mutation
    // (addStation, addStationPrior, reset) has marked it stale.  addSharedInfo carries
    // a shared-border delta through the reduction in place (reduced_ stays true), so
    // the common driver path (gate reduce(), then the D2 carry-forward addSharedInfo,
    // then the reduce() inside solve()) runs the expensive per-station Schur loop once.
    if (reduced_) {
        return;
    }

    // Time only a real recompute; the no-op above has already returned (D0).
    ScopedAccum _perf(perf_timing_, &timers_.reduce_ms);
    if (perf_timing_) ++timers_.reduce_recomputes;

    // A recompute rebuilds the reduced system from scratch, so any previously
    // folded anchor-free sum-to-zero gauge (@ref foldReducedSumZeroGauges) is
    // dropped from it, and its cached (g, β, γ) would be stale for both the reduced
    // normals and the constrained reconstruction.  The gauges are cleared here and
    // the driver re-folds them after the reduce, as it re-applies the code-bias and
    // clock constraints on the within-epoch RAIM re-solve.
    sumzero_gauges_.clear();
    station_sumzero_.clear();
    sumzero_folded_ = false;
    // Same for the invariant-carry DD priors (@ref foldReducedDdPriors, Card 4): a
    // rebuilt reduced system drops the folded G M Gᵀ; the driver re-folds after the reduce.
    dd_priors_.clear();
    station_ddprior_.clear();
    ddprior_folded_ = false;

    // Start from the accumulated shared border (Σ_r N_ss^r + priors), then
    // subtract each station's Schur contribution N_sr (N_rr)^{-1} N_rs.
    reduced_N_ss_ = N_ss_;
    reduced_u_s_ = u_s_;

    // Per-station elimination.  Each station's subtrahend N_sr (N_rr)^{-1} N_rs is
    // independent (its own cached LDLᵀ factor, its own N_sr); only the accumulation
    // into the shared border is a reduction.  The helper computes the two local
    // solves.  The serial path fuses the product into the running subtraction
    // (bit-identical to the original serial loop); the optional parallel path
    // accumulates per-thread partials and combines them deterministically.
    auto stationSolves = [&](std::size_t i, Eigen::MatrixXd& Nrr_inv_Nrs,
                             Eigen::VectorXd& Nrr_inv_ur) -> bool {
        const Station& st = stations_[i];
        if (st.n_local == 0) {
            return false; // no nuisance block to eliminate
        }
        // N_rr^{-1} N_rs  (n_r × n_s), with N_rs = N_sr^ᵀ;  and N_rr^{-1} u_r (n_r).
        Nrr_inv_Nrs = st.Nrr_ldlt.solve(st.N_sr.transpose());
        Nrr_inv_ur  = st.Nrr_ldlt.solve(st.u_r);
        return true;
    };

#ifdef _OPENMP
    // Optional fan-out across stations.  The driver pins Eigen to a single thread
    // for the epoch, so each station's dense kernels stay serial and there is no
    // nested oversubscription.  Determinism: each thread owns a contiguous station
    // range summed in ascending index, and the per-thread partials are combined in
    // ascending thread order, so the result is reproducible for a fixed thread count.
    // It differs from the serial fold at ULP level (a different summation grouping).
    // n_threads ≤ 1 takes the exact serial path below.
    if (n_threads > 1 && n_active_ > 1) {
        const int nt = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(n_threads), n_active_));
        // Pre-size every partial to a zero block so the combine below is valid even
        // if the runtime grants fewer than nt threads (untouched slots stay zero).
        const Eigen::MatrixXd zN = Eigen::MatrixXd::Zero(n_shared_, n_shared_);
        const Eigen::VectorXd zu = Eigen::VectorXd::Zero(n_shared_);
        std::vector<Eigen::MatrixXd> part_N(nt, zN);
        std::vector<Eigen::VectorXd> part_u(nt, zu);
        #pragma omp parallel num_threads(nt)
        {
            const int tid = omp_get_thread_num();
            const int real_nt = omp_get_num_threads();
            // Contiguous static partition [lo, hi) of the active stations.
            const std::size_t chunk =
                (n_active_ + static_cast<std::size_t>(real_nt) - 1)
                / static_cast<std::size_t>(real_nt);
            const std::size_t lo =
                std::min(n_active_, static_cast<std::size_t>(tid) * chunk);
            const std::size_t hi = std::min(n_active_, lo + chunk);
            Eigen::MatrixXd accN = Eigen::MatrixXd::Zero(n_shared_, n_shared_);
            Eigen::VectorXd accu = Eigen::VectorXd::Zero(n_shared_);
            Eigen::MatrixXd Nrr_inv_Nrs;
            Eigen::VectorXd Nrr_inv_ur;
            for (std::size_t i = lo; i < hi; ++i) {
                if (stationSolves(i, Nrr_inv_Nrs, Nrr_inv_ur)) {
                    accN.noalias() += stations_[i].N_sr * Nrr_inv_Nrs;
                    accu.noalias() += stations_[i].N_sr * Nrr_inv_ur;
                }
            }
            part_N[tid] = std::move(accN);
            part_u[tid] = std::move(accu);
        }
        // Deterministic serial combine, ascending thread index.
        for (int t = 0; t < nt; ++t) {
            reduced_N_ss_.noalias() -= part_N[t];
            reduced_u_s_.noalias() -= part_u[t];
        }
    } else
#endif
    {
        // Serial fold over the active pool slots [0, n_active_); slots beyond that
        // are retained allocations from a previous, larger epoch.  Fused
        // product-subtract in ascending index, bit-identical to the original serial loop.
        Eigen::MatrixXd Nrr_inv_Nrs;
        Eigen::VectorXd Nrr_inv_ur;
        for (std::size_t i = 0; i < n_active_; ++i) {
            if (stationSolves(i, Nrr_inv_Nrs, Nrr_inv_ur)) {
                reduced_N_ss_.noalias() -= stations_[i].N_sr * Nrr_inv_Nrs;
                reduced_u_s_.noalias()  -= stations_[i].N_sr * Nrr_inv_ur;
            }
        }
    }

    // The Schur complement of a symmetric matrix is symmetric; enforce it
    // explicitly to remove floating-point asymmetry before the solve.  The result
    // is written into a temporary and then assigned, to avoid aliasing.
    reduced_N_ss_ = (0.5 * (reduced_N_ss_ + reduced_N_ss_.transpose())).eval();

    reduced_ = true;
    ++reduce_recompute_count_;  // reduce-once invariant instrumentation (this is a real recompute)
}

void NetworkNormalEquations::foldReducedSumZeroGauges(
    const std::vector<SumZeroGauge>& gauges) {
    if (!reduced_) {
        reduce();  // need the reduced system + the cached per-station factors
    }
    // Idempotent within one reduce: the fold mutates reduced_N_ss_ in place, and
    // both the null-space gate (via reducedSharedNormals) and the solve trigger it,
    // so a second call must not add g gᵀ/β again.  A reduce() recompute or reset()
    // clears the flag.
    if (sumzero_folded_) {
        return;
    }
    sumzero_gauges_.clear();
    station_sumzero_.assign(n_active_, {});
    sumzero_folded_ = true;
    if (gauges.empty() || n_shared_ == 0) {
        return;
    }

    // Cache N_rr⁻¹ u_r per station lazily: a station appears in one gauge per
    // (sat, signal) it tracks, so the u_r solve would otherwise repeat ~15×.  The
    // per-column solve n_r = N_rr⁻¹ e_col is genuinely per (station, gauge).
    std::vector<bool> have_ur(n_active_, false);
    std::vector<Eigen::VectorXd> nrr_inv_ur(n_active_);

    for (const auto& gz : gauges) {
        Eigen::VectorXd g = Eigen::VectorXd::Zero(n_shared_);
        double beta = 0.0, gamma = 0.0;
        for (const auto& [si, col] : gz.terms) {
            if (si >= n_active_) {
                throw std::out_of_range("NetworkNormalEquations::foldReducedSumZeroGauges: "
                                        "station index out of range");
            }
            const Station& st = stations_[si];
            if (col < 0 || col >= st.n_local) {
                fail("foldReducedSumZeroGauges: local column out of range");
            }
            Eigen::VectorXd e = Eigen::VectorXd::Zero(st.n_local);
            e(col) = 1.0;
            const Eigen::VectorXd n_r = st.Nrr_ldlt.solve(e);  // N_rr⁻¹ e_col
            beta += n_r(col);                                  // (N_rr⁻¹)_{col,col} > 0
            g.noalias() += st.N_sr * n_r;                      // N_sr n_r
            if (!have_ur[si]) {
                nrr_inv_ur[si] = st.Nrr_ldlt.solve(st.u_r);
                have_ur[si] = true;
            }
            gamma += nrr_inv_ur[si](col);                      // (N_rr⁻¹ u_r)_{col}
        }
        if (!(beta > 0.0) || !std::isfinite(beta) || !std::isfinite(gamma)) {
            continue;  // degenerate gauge (no valid terms): skip
        }
        // Schur-projected rank-1 fold: Ñ_ss += g gᵀ/β,  ũ_s += g γ/β.
        const double inv_beta = 1.0 / beta;
        reduced_N_ss_.noalias() += (g * g.transpose()) * inv_beta;
        reduced_u_s_.noalias() += g * (gamma * inv_beta);
        const std::size_t gidx = sumzero_gauges_.size();
        FoldedSumZeroGauge fg;
        fg.terms = gz.terms;
        fg.g = std::move(g);
        fg.beta = beta;
        fg.gamma = gamma;
        for (const auto& [si, col] : fg.terms)
            station_sumzero_[si].emplace_back(gidx, col);
        sumzero_gauges_.push_back(std::move(fg));
    }
    // Symmetrize the rank-1 accumulation (g gᵀ is symmetric to ULP; keep the
    // solve's factorization clean, matching reduce()'s own symmetrization).
    reduced_N_ss_ = (0.5 * (reduced_N_ss_ + reduced_N_ss_.transpose())).eval();
}

void NetworkNormalEquations::foldReducedDdPriors(
    const std::vector<DdPriorBlock>& blocks, int n_threads, int station_threads) {
    if (!reduced_) {
        reduce();  // need the reduced system + the cached per-station factors
    }
    // Idempotent within one reduce (mirrors foldReducedSumZeroGauges): the fold
    // mutates reduced_N_ss_ in place, so a second call must not double-add G M Gᵀ.
    if (ddprior_folded_) {
        return;
    }
    dd_priors_.clear();
    station_ddprior_.assign(n_active_, {});
    ddprior_folded_ = true;
    dd_fold_stats_ = DdFoldStats{};   // per fold, so a RAIM re-solve overwrites it (INC-0)
    // Roll the per-block PD-screen reads up into the fold's diagnostics (INC-0).  Both
    // fold paths call this (the deterministic-parallel one returns early), so the two
    // cannot disagree about what the epoch's screen saw.
    auto tallyDdFoldStats = [this]() {
        DdFoldStats st;
        for (const FoldedDdPrior& f : dd_priors_) {
            const Eigen::Index K = f.Pz.rows();
            if (K == 0) continue;   // chordless slot; never reached a factorization
            ++st.blocks;
            st.rows += static_cast<long long>(K);
            if (f.degenerate) {
                ++st.degen_blocks;
                st.degen_rows += static_cast<long long>(K);
            }
            if (f.repaired) {
                ++st.repairs;
                st.repaired_rows += static_cast<long long>(K);
            }
            st.min_pivot_rel = std::min(st.min_pivot_rel, f.min_pivot_rel);
            st.repair_worst  = std::max(st.repair_worst, f.repair_worst);
        }
        dd_fold_stats_ = st;
    };
    if (blocks.empty() || n_shared_ == 0) {
        return;
    }

    // ---- Deterministic-parallel fold (network.parallel_seam) --------------------
    // Every block's per-(const, signal) algebra and the operands of its rank-K Woodbury
    // contribution (dN = G M Gᵀ, du = G M (z_free − ẑ)) are computed in parallel into
    // pre-sized per-block slots; the contributions are then accumulated serially in
    // block-index order below.  The per-block compute follows the serial fold exactly
    // (same operations, same sorted by_station reduction order) and the accumulation
    // order is the same, so the reduced system is bit-identical to the serial path
    // (n_threads ≤ 1, with station_threads ≤ 1) for any thread count, unlike the
    // FP-reassociating threaded reduce().  The serial fold below (the default) does not
    // depend on this path; keep the two block-algebra bodies in step.
    if (n_threads > 1) {
        const std::size_t NB = blocks.size();

        // (0, serial) Validate every block up front so all throws stay on the serial
        // thread (an exception escaping the OpenMP region is UB), and precompute
        // N_rr⁻¹ u_r for every referenced station.  This removes the cross-block cache
        // race; the value is a deterministic per-station solve, so precomputing it is
        // byte-identical to the serial path's lazy fill.
        std::vector<bool> have_ur(n_active_, false);
        std::vector<Eigen::VectorXd> nrr_inv_ur(n_active_);
        for (const auto& blk : blocks) {
            const Eigen::Index K = static_cast<Eigen::Index>(blk.rows.size());
            if (blk.Pz.rows() != K || blk.Pz.cols() != K || blk.zhat.size() != K) {
                fail("foldReducedDdPriors: Pz/zhat dimension mismatch with the row count");
            }
            for (Eigen::Index k = 0; k < K; ++k) {
                for (const auto& t : blk.rows[static_cast<std::size_t>(k)]) {
                    if (t.station >= n_active_) {
                        throw std::out_of_range("NetworkNormalEquations::foldReducedDdPriors: "
                                                "station index out of range");
                    }
                    if (t.local_col < 0 || t.local_col >= stations_[t.station].n_local) {
                        fail("foldReducedDdPriors: local column out of range");
                    }
                    if (!have_ur[t.station]) {
                        nrr_inv_ur[t.station] =
                            stations_[t.station].Nrr_ldlt.solve(stations_[t.station].u_r);
                        have_ur[t.station] = true;
                    }
                }
            }
        }

        // Per-block slots (separate vectors, using the fold's own private types as the
        // serial path does; no local struct at function scope).  Each block writes only
        // its own index.  The slots hold the per-block G, M and z_free, not the
        // materialized G M Gᵀ: the serial merge below recomputes the product with the
        // same fused Eigen expression the serial fold uses, so the += rounds
        // bit-identically.  Pre-forming dN = (G M Gᵀ).eval() and adding it instead
        // drifts by a ULP at large n_shared, because Eigen's blocked GEMM rounds a
        // beta = 0 product-then-add differently from the beta = 1 fused accumulate, and
        // the network's ill-conditioning amplifies that over the carry-forward epochs
        // (the threaded reduce() failure mode).  Only the block-local work (the
        // per-station B/G/z_free solves and the K×K middle inverse) runs in the
        // parallel region.
        std::vector<FoldedDdPrior>   fd_slot(NB);
        std::vector<std::vector<std::pair<std::size_t, std::vector<DdStationTerm>>>> terms_slot(NB);
        std::vector<char> has_merge(NB, 0);         // K > 0 ⇒ G/M/terms carry a contribution

        // (1, parallel over block index) The driver pins Eigen to one thread for the
        // whole epoch, so these dense kernels do not nest threads.  Each iteration writes
        // only its own slot index; nrr_inv_ur and stations_ are read-only here.  No throw
        // can escape (all validation ran serially above).
        #pragma omp parallel for schedule(dynamic) num_threads(n_threads)
        for (long long bi = 0; bi < static_cast<long long>(NB); ++bi) {
            const std::size_t bidx = static_cast<std::size_t>(bi);
            const auto& blk = blocks[bidx];
            const Eigen::Index K = static_cast<Eigen::Index>(blk.rows.size());
            fd_slot[bidx].zhat = blk.zhat;
            fd_slot[bidx].Pz   = blk.Pz;
            if (K == 0) {
                continue;   // chordless component → inert slot, no merge (index preserved)
            }
            std::map<std::size_t, std::vector<DdStationTerm>> by_station;
            for (Eigen::Index k = 0; k < K; ++k) {
                for (const auto& t : blk.rows[static_cast<std::size_t>(k)]) {
                    by_station[t.station].push_back({bidx, k, t.local_col, t.coeff});
                }
            }
            Eigen::MatrixXd B = Eigen::MatrixXd::Zero(K, K);
            Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n_shared_, K);
            Eigen::VectorXd z_free = Eigen::VectorXd::Zero(K);
            for (const auto& [si, terms] : by_station) {
                const Station& st = stations_[si];
                Eigen::MatrixXd CrT = Eigen::MatrixXd::Zero(st.n_local, K);
                for (const auto& t : terms) {
                    CrT(t.col, t.row) += t.coeff;
                }
                const Eigen::MatrixXd Y = st.Nrr_ldlt.solve(CrT);
                B.noalias()      += CrT.transpose() * Y;
                G.noalias()      += st.N_sr * Y;
                z_free.noalias() += CrT.transpose() * nrr_inv_ur[si];
            }
            Eigen::MatrixXd PzB = blk.Pz + B;
            PzB = (0.5 * (PzB + PzB.transpose())).eval();
            // The product-form posterior readback (Pᶻ − PᶻMPᶻ ≡ PᶻMB) multiplies through
            // the data information B (PD-safe INC-1a).  B is cached only when armed, so
            // the off path keeps its exact allocation footprint.
            if (dd_fold_pd_safe_) fd_slot[bidx].B = B;
            // station_threads is 1 here on purpose: the within-block column split is a
            // serial-fold option only (the blocks already run one per thread).
            DdMiddleInverse mi = dd_fold_pd_safe_
                                     ? ddMiddleInversePdSafe(PzB, K, 1)
                                     : ddMiddleInverseLegacy(PzB, K, 1);
            fd_slot[bidx].Minv         = std::move(mi.Minv);
            fd_slot[bidx].degenerate   = mi.degenerate;
            fd_slot[bidx].min_pivot_rel = mi.min_pivot_rel;
            fd_slot[bidx].repaired     = mi.repaired;
            fd_slot[bidx].repair_worst = mi.repair_worst;
            fd_slot[bidx].G      = std::move(G);
            fd_slot[bidx].M      = std::move(mi.M);
            fd_slot[bidx].z_free = std::move(z_free);
            terms_slot[bidx].reserve(by_station.size());
            for (auto& [si, terms] : by_station) {
                terms_slot[bidx].emplace_back(si, std::move(terms));
            }
            has_merge[bidx] = 1;
        }

        // (2, serial in block-index order) Accumulate with the same fused Eigen
        // expression as the serial fold's Woodbury step below.  The operands (the
        // parallel-computed G, M and z_free) are bit-identical to the serial fold's and
        // the += sequence is the same, so the reduced system is bit-identical.
        dd_priors_.reserve(NB);
        for (std::size_t b = 0; b < NB; ++b) {
            FoldedDdPrior& fd = fd_slot[b];
            if (has_merge[b]) {
                reduced_N_ss_.noalias() += fd.G * fd.M * fd.G.transpose();
                reduced_u_s_.noalias()  += fd.G * (fd.M * (fd.z_free - fd.zhat));
                for (auto& [si, terms] : terms_slot[b]) {
                    auto& dst = station_ddprior_[si];
                    dst.insert(dst.end(), terms.begin(), terms.end());
                }
            }
            dd_priors_.push_back(std::move(fd));
        }
        reduced_N_ss_ = (0.5 * (reduced_N_ss_ + reduced_N_ss_.transpose())).eval();
        tallyDdFoldStats();   // INC-0
        return;
    }

    // ---- Serial fold (n_threads ≤ 1, the default; the byte-identical off path) ---
    // Cache N_rr⁻¹ u_r per station (reused across every block/row that touches it).
    std::vector<bool> have_ur(n_active_, false);
    std::vector<Eigen::VectorXd> nrr_inv_ur(n_active_);

    for (const auto& blk : blocks) {
        const Eigen::Index K = static_cast<Eigen::Index>(blk.rows.size());
        if (blk.Pz.rows() != K || blk.Pz.cols() != K || blk.zhat.size() != K) {
            fail("foldReducedDdPriors: Pz/zhat dimension mismatch with the row count");
        }
        // Every input block occupies exactly one slot (bidx == input index), so a
        // chordless (K=0) or degenerate (non-PD) block stays index-aligned for the
        // z-store's per-block posterior readback (ddPriorPosteriorMean/numDdPriorBlocks).
        const std::size_t bidx = dd_priors_.size();
        FoldedDdPrior fd;
        fd.zhat = blk.zhat;
        fd.Pz = blk.Pz;
        if (K == 0) {
            dd_priors_.push_back(std::move(fd));   // chordless component → inert slot
            continue;
        }

        // Group the block's terms by station so each station's C_{L,r}ᵀ (n_local × K)
        // is assembled once and solved against the cached N_rr factor in one shot.
        std::map<std::size_t, std::vector<DdStationTerm>> by_station;
        for (Eigen::Index k = 0; k < K; ++k) {
            for (const auto& t : blk.rows[static_cast<std::size_t>(k)]) {
                if (t.station >= n_active_) {
                    throw std::out_of_range("NetworkNormalEquations::foldReducedDdPriors: "
                                            "station index out of range");
                }
                if (t.local_col < 0 || t.local_col >= stations_[t.station].n_local) {
                    fail("foldReducedDdPriors: local column out of range");
                }
                by_station[t.station].push_back({bidx, k, t.local_col, t.coeff});
            }
        }

        Eigen::MatrixXd B = Eigen::MatrixXd::Zero(K, K);
        Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n_shared_, K);
        Eigen::VectorXd z_free = Eigen::VectorXd::Zero(K);
        {   // section timer for the per-station reduction loop, the dominant fold cost (P0b)
            ScopedAccum _fl(perf_timing_, &timers_.fold_loop_ms);
            // Flatten by_station (ascending station index, std::map order) and precompute
            // N_rr⁻¹ u_r for every touched station serially.  The value is a deterministic
            // per-station solve, so hoisting it out of any parallel region removes the
            // lazy-fill race and is byte-identical to the serial lazy fill.
            std::vector<std::pair<std::size_t, const std::vector<DdStationTerm>*>> st_list;
            st_list.reserve(by_station.size());
            for (const auto& kv : by_station) {
                if (!have_ur[kv.first]) {
                    nrr_inv_ur[kv.first] =
                        stations_[kv.first].Nrr_ldlt.solve(stations_[kv.first].u_r);
                    have_ur[kv.first] = true;
                }
                st_list.emplace_back(kv.first, &kv.second);
            }
            auto stationTerm = [&](std::size_t idx, Eigen::MatrixXd& Bacc,
                                   Eigen::MatrixXd& Gacc, Eigen::VectorXd& zacc) {
                const std::size_t si = st_list[idx].first;
                const Station& st = stations_[si];
                Eigen::MatrixXd CrT = Eigen::MatrixXd::Zero(st.n_local, K);  // C_{L,r}ᵀ
                for (const auto& t : *st_list[idx].second) CrT(t.col, t.row) += t.coeff;
                const Eigen::MatrixXd Y = st.Nrr_ldlt.solve(CrT);            // N_rr⁻¹ C_{L,r}ᵀ
                Bacc.noalias() += CrT.transpose() * Y;                       // C_{L,r} N_rr⁻¹ C_{L,r}ᵀ
                Gacc.noalias() += st.N_sr * Y;                              // N_sr N_rr⁻¹ C_{L,r}ᵀ
                zacc.noalias() += CrT.transpose() * nrr_inv_ur[si];         // C_{L,r} N_rr⁻¹ u_r
            };
#ifdef _OPENMP
            if (station_threads > 1 && st_list.size() > 1) {
                // Under parallel_carry, per-thread partials over contiguous station ranges
                // are combined in ascending thread order.  The result is reproducible for a
                // given thread count and differs from serial at ULP level: the same
                // relaxation as in reduce(), which the toggle opts into.
                const int nt = static_cast<int>(std::min<std::size_t>(
                    static_cast<std::size_t>(station_threads), st_list.size()));
                std::vector<Eigen::MatrixXd> Bp(nt, Eigen::MatrixXd::Zero(K, K));
                std::vector<Eigen::MatrixXd> Gp(nt, Eigen::MatrixXd::Zero(n_shared_, K));
                std::vector<Eigen::VectorXd> zp(nt, Eigen::VectorXd::Zero(K));
                #pragma omp parallel num_threads(nt)
                {
                    const int tid = omp_get_thread_num();
                    const int rnt = omp_get_num_threads();
                    const std::size_t n = st_list.size();
                    const std::size_t lo = (n * static_cast<std::size_t>(tid))
                                           / static_cast<std::size_t>(rnt);
                    const std::size_t hi = (n * static_cast<std::size_t>(tid + 1))
                                           / static_cast<std::size_t>(rnt);
                    for (std::size_t idx = lo; idx < hi; ++idx)
                        stationTerm(idx, Bp[tid], Gp[tid], zp[tid]);
                }
                for (int t = 0; t < nt; ++t) {   // deterministic ascending combine
                    B.noalias()      += Bp[t];
                    G.noalias()      += Gp[t];
                    z_free.noalias() += zp[t];
                }
            } else
#endif
            {
                for (std::size_t idx = 0; idx < st_list.size(); ++idx)
                    stationTerm(idx, B, G, z_free);
            }
        }

        // Middle inverse M = (Pᶻ + B)⁻¹ (PD: SPD prior plus PSD B).  A degenerate block
        // (non-PD, e.g. a malformed prior) folds inertly (M = 0): a no-op that keeps its
        // index slot rather than corrupting the reduced system or shifting the keys.
        // The inert branch freezes the block, so it is counted (INC-0), avoided under
        // the PD-safe screen where the violation is numerical (INC-1), and re-coupled by
        // the install's block rebirth where it is not (INC-2a).  The column-parallel
        // inverse (parallel_carry) is in @ref inverseFromLdlt.
        Eigen::MatrixXd M;
        {   // section timer for the K×K middle inverse, LDLT factor plus explicit inverse (P0b)
            ScopedAccum _fm(perf_timing_, &timers_.fold_minv_ms);
            Eigen::MatrixXd PzB = blk.Pz + B;
            PzB = (0.5 * (PzB + PzB.transpose())).eval();
            // Cache the data information B for the product-form posterior
            // (Pᶻ − PᶻMPᶻ ≡ PᶻMB), PD-safe INC-1a.  Only when armed, so the off path's
            // allocation footprint is unchanged.
            if (dd_fold_pd_safe_) fd.B = B;
            DdMiddleInverse mi = dd_fold_pd_safe_
                                     ? ddMiddleInversePdSafe(PzB, K, station_threads)
                                     : ddMiddleInverseLegacy(PzB, K, station_threads);
            // Cache M⁻¹ = Pᶻ+B for the D5 innovation covariance readout; post-repair
            // when the INC-1c clamp fired, so Cov(v) and the fold agree.
            fd.Minv          = std::move(mi.Minv);
            fd.degenerate    = mi.degenerate;
            fd.min_pivot_rel = mi.min_pivot_rel;
            fd.repaired      = mi.repaired;
            fd.repair_worst  = mi.repair_worst;
            M = std::move(mi.M);
        }

        // Joint rank-K Woodbury fold onto the reduced shared system (a no-op when M=0).
        {   // Section timer for the Woodbury GMGᵀ fold (P0b).  Under parallel_carry, G·M·Gᵀ
            // is a large dense GEMM (n_shared×K · K×K · K×n_shared) that threads well in
            // Eigen, so Eigen is un-pinned locally to spread it across cores.  This step is
            // outside any OpenMP region (the per-station loop and column-solve regions
            // closed above), so there is no nesting.  parallel_carry already gives up serial
            // bit-identity in the per-station reduction, so the GEMM reassociation adds no
            // further serial/parallel difference.  ScopedEigenThreads restores the pin on
            // every exit, including a throw, so the next block's OpenMP loop runs with
            // Eigen pinned again.
            ScopedAccum _fg(perf_timing_, &timers_.fold_gmg_ms);
            ScopedEigenThreads _et(station_threads > 1, station_threads);
            reduced_N_ss_.noalias() += G * M * G.transpose();
            reduced_u_s_.noalias()  += G * (M * (z_free - blk.zhat));
        }

        fd.G = std::move(G);
        fd.M = std::move(M);
        fd.z_free = std::move(z_free);
        for (const auto& [si, terms] : by_station) {
            auto& dst = station_ddprior_[si];
            dst.insert(dst.end(), terms.begin(), terms.end());
        }
        dd_priors_.push_back(std::move(fd));
    }
    reduced_N_ss_ = (0.5 * (reduced_N_ss_ + reduced_N_ss_.transpose())).eval();
    tallyDdFoldStats();   // INC-0
}

bool NetworkNormalEquations::ddPriorBlockDegenerate(std::size_t b) const {
    if (b >= dd_priors_.size()) {
        throw std::out_of_range("NetworkNormalEquations::ddPriorBlockDegenerate: "
                                "block index out of range");
    }
    return dd_priors_[b].degenerate;
}

double NetworkNormalEquations::ddPriorBlockMinPivotRel(std::size_t b) const {
    if (b >= dd_priors_.size()) {
        throw std::out_of_range("NetworkNormalEquations::ddPriorBlockMinPivotRel: "
                                "block index out of range");
    }
    return dd_priors_[b].min_pivot_rel;
}

Eigen::VectorXd NetworkNormalEquations::ddPriorPosteriorMean(
    std::size_t block_index, const Eigen::VectorXd& x_s) const {
    if (block_index >= dd_priors_.size()) {
        throw std::out_of_range("NetworkNormalEquations::ddPriorPosteriorMean: "
                                "block index out of range");
    }
    if (x_s.size() != n_shared_) {
        fail("ddPriorPosteriorMean: x_s size mismatch");
    }
    const FoldedDdPrior& fd = dd_priors_[block_index];
    // ẑ_post = ẑ + Pᶻ M (z_free − Gᵀ x_s − ẑ): the prior shrunk toward the data DD.
    const Eigen::VectorXd innov = fd.z_free - fd.G.transpose() * x_s - fd.zhat;
    return fd.zhat + fd.Pz * (fd.M * innov);
}

Eigen::MatrixXd NetworkNormalEquations::ddPriorPosteriorCovariance(
    std::size_t block_index, const Eigen::MatrixXd& cov_free_xs) const {
    if (block_index >= dd_priors_.size()) {
        throw std::out_of_range("NetworkNormalEquations::ddPriorPosteriorCovariance: "
                                "block index out of range");
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("ddPriorPosteriorCovariance: cov_free_xs size mismatch");
    }
    const FoldedDdPrior& fd = dd_priors_[block_index];
    const Eigen::Index K = fd.Pz.rows();
    if (K == 0) {
        return Eigen::MatrixXd();            // chordless slot → empty
    }
    // Pᶻ_post = (Pᶻ − Pᶻ M Pᶻ) + (Pᶻ M Gᵀ) Σ_ss (G M Pᶻ).  An inert degenerate block
    // has M = 0 ⇒ Pᶻ_post = Pᶻ (the untouched prior; no information was folded).
    const Eigen::MatrixXd PzM = fd.Pz * fd.M;                 // Pᶻ M            (K×K)
    Eigen::MatrixXd cov;
    if (dd_fold_pd_safe_ && !fd.degenerate && fd.B.rows() == K && fd.B.cols() == K) {
        // --- Product form of the same quantity (PD-safe INC-1a) ------------------
        // Pᶻ − PᶻMPᶻ = PᶻM[(Pᶻ+B) − Pᶻ] = PᶻMB exactly.  The left form subtracts two
        // operands of order 1e6 to produce a mature row of order 1e-4.  That
        // cancellation, accumulated over long GEMM chains and the W·Pᶻ·Wᵀ integer
        // re-expression, can push the smallest eigenvalues of the near-singular joint
        // lane block through zero.  The right form's operands follow the scale of the
        // result: 1e-4·(1/B)·B.
        //
        // Not applied to a degenerate block: with M forced to 0 the identity's premise
        // (M = (Pᶻ+B)⁻¹) is false, and the subtractive form still gives the inert-fold
        // result (the posterior equals the untouched prior).
        cov = fd.Pz * (fd.M * fd.B);                          // Pᶻ M B          (K×K)
        cov = (0.5 * (cov + cov.transpose())).eval();         // sym(·): the exact symmetry
    } else {
        cov = fd.Pz - PzM * fd.Pz;                            // conditional-given-x_s
    }
    // Coupling through the shared-parameter uncertainty: the DD depends on x_s via
    // −Pᶻ M Gᵀ, so add (Pᶻ M Gᵀ) Σ_ss (Pᶻ M Gᵀ)ᵀ.  Empty on a zero shared block.
    if (n_shared_ > 0) {
        const Eigen::MatrixXd PzMGt = PzM * fd.G.transpose(); // Pᶻ M Gᵀ         (K×n_s)
        cov.noalias() += PzMGt * cov_free_xs * PzMGt.transpose();
    }
    cov = (0.5 * (cov + cov.transpose())).eval();             // symmetrize round-off
    return cov;
}

void NetworkNormalEquations::ddPriorInnovation(
    std::size_t block_index, const Eigen::VectorXd& x_s,
    const Eigen::MatrixXd& cov_free_xs, Eigen::VectorXd& v, Eigen::MatrixXd& cov_v) const {
    if (block_index >= dd_priors_.size()) {
        throw std::out_of_range("NetworkNormalEquations::ddPriorInnovation: "
                                "block index out of range");
    }
    if (x_s.size() != n_shared_) {
        fail("ddPriorInnovation: x_s size mismatch");
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("ddPriorInnovation: cov_free_xs size mismatch");
    }
    const FoldedDdPrior& fd = dd_priors_[block_index];
    const Eigen::Index K = fd.Pz.rows();
    if (K == 0) {                            // chordless slot → no innovation
        v.resize(0);
        cov_v.resize(0, 0);
        return;
    }
    // v = z_free − Gᵀ x_s − ẑ (== the `innov` of ddPriorPosteriorMean): the data-implied
    // DD given the solved shared estimate, minus the carried prior.
    v = fd.z_free - fd.G.transpose() * x_s - fd.zhat;
    // Cov(v) = (Pᶻ + B) − Gᵀ Σ_ss G = M⁻¹ − Gᵀ Σ_ss G.  Subtracting the border term is
    // exact (x_s is influenced by the prior); it gives Pᶻ M Cov(v) M Pᶻ = Pᶻ − Pᶻ⁺.
    cov_v = fd.Minv;
    if (n_shared_ > 0) {
        cov_v.noalias() -= fd.G.transpose() * cov_free_xs * fd.G;
    }
    cov_v = (0.5 * (cov_v + cov_v.transpose())).eval();       // symmetrize round-off
}

Eigen::VectorXd NetworkNormalEquations::solveShared() {
    if (!reduced_) {
        reduce();
    }
    if (n_shared_ == 0) {
        return Eigen::VectorXd();
    }
    Eigen::LDLT<Eigen::MatrixXd> ldlt(reduced_N_ss_);
    requireFactorPd(ldlt,
                    "solveShared: reduced shared block is not positive-definite "
                    "(rank-deficient datum? apply the S-basis constraint first)");
    return ldlt.solve(reduced_u_s_);
}

Eigen::VectorXd NetworkNormalEquations::solveSharedConstrained(
    const Eigen::MatrixXd& constrained_N_ss,
    const Eigen::VectorXd& constrained_u_s,
    Eigen::MatrixXd* cov_out) const {
    requireSquare(constrained_N_ss, n_shared_,
                  "solveSharedConstrained: constrained_N_ss");
    if (constrained_u_s.size() != n_shared_) {
        fail("solveSharedConstrained: constrained_u_s size mismatch");
    }
    if (n_shared_ == 0) {
        if (cov_out) *cov_out = Eigen::MatrixXd();
        return Eigen::VectorXd();
    }
    // Factorize the datum-constrained reduced normals once and reuse the factor for
    // the point solve and, optionally, the formal covariance (the inverse of the same
    // matrix), so the caller need not refactor it.  The PD guard below is the single
    // rank-deficiency safety net (an under-specified datum throws here).
    //
    // Timing (D0): the factorization and point solve accrue to solve_ms; the explicit
    // full inverse (the direct cost that the D1 selected-inverse option addresses)
    // accrues to cov_inverse_ms.  The two sub-timers are disjoint, so the profiler
    // shows how much of the "solve" is the covariance the consumers then propagate.
    const bool pt = perf_timing_;
    if (pt) ++timers_.solve_calls;
    PerfClock::time_point t_fac;
    if (pt) t_fac = PerfClock::now();
    Eigen::LDLT<Eigen::MatrixXd> ldlt(constrained_N_ss);
    requireFactorPd(ldlt,
                    "solveSharedConstrained: constrained shared block is not "
                    "positive-definite (the applied S-basis constraint did not "
                    "remove the datum deficiency)");
    if (pt) timers_.solve_ms += perfMsSince(t_fac);
    if (cov_out) {
        ScopedAccum _perf(pt, &timers_.cov_inverse_ms);
        *cov_out = ldlt.solve(Eigen::MatrixXd::Identity(n_shared_, n_shared_));
    }
    PerfClock::time_point t_sol;
    if (pt) t_sol = PerfClock::now();
    Eigen::VectorXd x_s = ldlt.solve(constrained_u_s);
    if (pt) timers_.solve_ms += perfMsSince(t_sol);
    return x_s;
}

Eigen::VectorXd NetworkNormalEquations::reconstructStation(
    std::size_t station, const Eigen::VectorXd& x_s) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::reconstructStation: "
                                "station index out of range");
    }
    if (x_s.size() != n_shared_) {
        fail("reconstructStation: x_s size mismatch");
    }
    const Station& st = stations_[station];
    if (st.n_local == 0) {
        return Eigen::VectorXd();
    }
    // x_r = N_rr^{-1} (u_r − N_rs x_s),  N_rs = N_sr^ᵀ.
    const Eigen::VectorXd rhs = st.u_r - st.N_sr.transpose() * x_s;
    Eigen::VectorXd x_r = st.Nrr_ldlt.solve(rhs);
    applySumZeroCorrection(station, x_s, x_r);
    applyDdPriorCorrection(station, x_s, x_r);
    return x_r;
}

NetworkNormalEquations::StationReconstructionParts
NetworkNormalEquations::reconstructStationParts(std::size_t station,
                                                const Eigen::VectorXd& x_s) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::reconstructStationParts: "
                                "station index out of range");
    }
    if (x_s.size() != n_shared_) {
        fail("reconstructStationParts: x_s size mismatch");
    }
    StationReconstructionParts p;
    p.x_r = reconstructStation(station, x_s);
    const Station& st = stations_[station];
    if (st.n_local == 0) return p;
    p.free     = st.Nrr_ldlt.solve(st.u_r);
    p.feedback = -st.Nrr_ldlt.solve(st.N_sr.transpose() * x_s);
    p.sumzero  = Eigen::VectorXd::Zero(st.n_local);
    p.ddprior  = Eigen::VectorXd::Zero(st.n_local);
    applySumZeroCorrection(station, x_s, p.sumzero);
    applyDdPriorCorrection(station, x_s, p.ddprior);
    return p;
}

void NetworkNormalEquations::applySumZeroCorrection(std::size_t station,
                                                    const Eigen::VectorXd& x_s,
                                                    Eigen::VectorXd& x_r) const {
    const Station& st = stations_[station];
    // Anchor-free sum-to-zero gauge correction (@ref foldReducedSumZeroGauges):
    // the fold constrained the reduced solve, so the matching constrained locals
    // are x_r −= n_r·(γ − gᵀ x_s)/β for each gauge this station is a term of
    // (n_r = N_rr⁻¹ e_col; γ − gᵀ x_s = the total unconstrained ambiguity sum).
    // Only the gauge (common-shift) direction changes, so every estimable
    // (between-receiver, between-satellite) quantity is unchanged.  The list is
    // empty on the anchor path and with the gauge off, which stay byte-identical.
    if (!station_sumzero_.empty()) {
        Eigen::VectorXd e = Eigen::VectorXd::Zero(st.n_local);
        for (const auto& [gidx, col] : station_sumzero_[station]) {
            const FoldedSumZeroGauge& fg = sumzero_gauges_[gidx];
            const double s_unc = fg.gamma - fg.g.dot(x_s);
            e(col) = 1.0;
            const Eigen::VectorXd n_r = st.Nrr_ldlt.solve(e);
            e(col) = 0.0;
            x_r.noalias() -= n_r * (s_unc / fg.beta);
        }
    }
}

void NetworkNormalEquations::applyDdPriorCorrection(std::size_t station,
                                                    const Eigen::VectorXd& x_s,
                                                    Eigen::VectorXd& x_r) const {
    const Station& st = stations_[station];
    // DD-prior correction (@ref foldReducedDdPriors, Card 4): the constrained locals
    // are x_r −= N_rr⁻¹ C_{L,r}ᵀ κ, with κ = M(z_free − Gᵀ x_s − ẑ) shared across this
    // station's terms of each block.  Only the DD (cycle) directions change.  With no
    // DD priors folded the list is empty and the reconstruction is byte-identical to
    // the one without the correction.
    if (station < station_ddprior_.size() && !station_ddprior_[station].empty()) {
        std::vector<Eigen::VectorXd> kappa(dd_priors_.size());
        std::vector<bool> have_kappa(dd_priors_.size(), false);
        Eigen::VectorXd e = Eigen::VectorXd::Zero(st.n_local);
        for (const auto& t : station_ddprior_[station]) {
            if (!have_kappa[t.block]) {
                const FoldedDdPrior& fd = dd_priors_[t.block];
                kappa[t.block] = fd.M * (fd.z_free - fd.G.transpose() * x_s - fd.zhat);
                have_kappa[t.block] = true;
            }
            e(t.col) += t.coeff * kappa[t.block](t.row);
        }
        x_r.noalias() -= st.Nrr_ldlt.solve(e);
    }
}

// ---------------------------------------------------------------------------------
// Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md §5a).
//
// The fold is the Schur complement of  Ã⁻¹ = N_rr⁻¹ − Σ_b Y_b M_b Y_bᵀ  (Y_b = N_rr⁻¹C_bᵀ),
// so a consistent readback subtracts the same rank-K term the reduced shared normals
// were built from.  This is the single implementation of that term; every armed
// readback routes through it, so the four cannot diverge.
// ---------------------------------------------------------------------------------
void NetworkNormalEquations::carryCovCorrection(
    const std::vector<std::size_t>& sta, const std::vector<Eigen::VectorXd>& n,
    Eigen::MatrixXd& local_corr, Eigen::MatrixXd& m_corr) const {
    const Eigen::Index k = static_cast<Eigen::Index>(n.size());
    local_corr.setZero(k, k);
    m_corr.setZero(n_shared_, k);
    if (k == 0 || dd_priors_.empty()) {
        return;
    }
    // One sparse pass over the requests fills every touched block's Y columns:
    //   Ycols_b(:, i) = C_{b, sta[i]} n_i   (zero column where block b misses station i).
    // station_ddprior_ is the same (block, row, local_col, coeff) list that the mean path
    // walks in reconstructStation, so the two readbacks agree on which block owns which
    // column.
    std::vector<Eigen::MatrixXd> Ycols(dd_priors_.size());
    for (Eigen::Index i = 0; i < k; ++i) {
        const std::size_t s = sta[static_cast<std::size_t>(i)];
        if (s >= station_ddprior_.size()) continue;
        for (const DdStationTerm& t : station_ddprior_[s]) {
            const FoldedDdPrior& fd = dd_priors_[t.block];
            const Eigen::Index K = fd.Pz.rows();
            if (K == 0 || fd.M.rows() != K) continue;   // chordless / never factored
            Eigen::MatrixXd& Y = Ycols[t.block];
            if (Y.size() == 0) Y.setZero(K, k);
            Y(t.row, i) += t.coeff * n[static_cast<std::size_t>(i)](t.col);
        }
    }
    // One GEMM per touched block.  A degenerate block carries M = 0 (the inert fold), so
    // it contributes exactly zero here without a branch.
    for (std::size_t b = 0; b < dd_priors_.size(); ++b) {
        const Eigen::MatrixXd& Y = Ycols[b];
        if (Y.size() == 0) continue;
        const FoldedDdPrior& fd = dd_priors_[b];
        const Eigen::MatrixXd W = fd.M * Y;                 // K × k
        local_corr.noalias() += Y.transpose() * W;          // k × k
        if (n_shared_ > 0) m_corr.noalias() += fd.G * W;    // n_shared × k
    }
}

Eigen::MatrixXd NetworkNormalEquations::reconstructStationCovariance(
    std::size_t station, const Eigen::MatrixXd& cov_free_xs, bool carry_aware) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::reconstructStationCovariance: "
                                "station index out of range");
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("reconstructStationCovariance: cov_free_xs size mismatch");
    }
    const Station& st = stations_[station];
    if (st.n_local == 0) {
        return Eigen::MatrixXd();
    }
    // Cov(x_r) = N_rr^{-1} + M · Cov_free · Mᵀ,  with M = N_rr^{-1} N_rs and
    // N_rs = N_sr^ᵀ.  Both N_rr^{-1} and M reuse the cached LDLᵀ factor.
    const Eigen::MatrixXd M = st.Nrr_ldlt.solve(st.N_sr.transpose());  // n_local × n_shared
    const Eigen::MatrixXd Nrr_inv = st.Nrr_ldlt.solve(
        Eigen::MatrixXd::Identity(st.n_local, st.n_local));
    Eigen::MatrixXd cov = Nrr_inv + M * cov_free_xs * M.transpose();
    cov = (0.5 * (cov + cov.transpose())).eval();  // symmetrize numerical drift
    if (!carry_aware || dd_priors_.empty()) {
        return cov;
    }
    // Armed: the same quantity over Ã⁻¹.  Column i of N_rr⁻¹ is n_i = N_rr⁻¹e_i, so the
    // whole-station form is the batched correction with one request per local column.
    std::vector<std::size_t>     sta(static_cast<std::size_t>(st.n_local), station);
    std::vector<Eigen::VectorXd> n(static_cast<std::size_t>(st.n_local));
    for (Eigen::Index c = 0; c < st.n_local; ++c) n[static_cast<std::size_t>(c)] = Nrr_inv.col(c);
    Eigen::MatrixXd local_corr, m_corr;
    carryCovCorrection(sta, n, local_corr, m_corr);
    ++carry_cov_stats_.calls;
    carry_cov_stats_.rows += static_cast<long long>(st.n_local);
    const Eigen::MatrixXd Mreg = (n_shared_ > 0) ? Eigen::MatrixXd(M - m_corr.transpose())
                                                 : Eigen::MatrixXd(M);
    Eigen::MatrixXd out = Nrr_inv - local_corr;
    if (n_shared_ > 0) out.noalias() += Mreg * cov_free_xs * Mreg.transpose();
    out = (0.5 * (out + out.transpose())).eval();
    // Refuse rather than clamp, and refuse the whole matrix, so the returned matrix stays
    // one consistent model rather than a mixture of two (§5b).
    long long bad = 0;
    for (Eigen::Index c = 0; c < st.n_local; ++c)
        if (!(out(c, c) > 0.0) || !std::isfinite(out(c, c))) ++bad;
    if (bad > 0) {
        carry_cov_stats_.nonpos_rows += bad;
        ++carry_cov_stats_.refused_calls;
        return cov;
    }
    for (Eigen::Index c = 0; c < st.n_local; ++c) tallyCarryRatio(out(c, c), cov(c, c));
    carry_cov_stats_.corrected_rows += static_cast<long long>(st.n_local);
    return out;
}

Eigen::VectorXd NetworkNormalEquations::reconstructStationCovarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Eigen::MatrixXd& cov_free_xs, bool carry_aware) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::reconstructStationCovarianceDiag: "
                                "station index out of range");
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("reconstructStationCovarianceDiag: cov_free_xs size mismatch");
    }
    const Station& st = stations_[station];
    const Eigen::Index k = static_cast<Eigen::Index>(local_cols.size());
    if (st.n_local == 0 || k == 0) {
        return Eigen::VectorXd();
    }
    // Per-column diagonal of Cov(x_r) = N_rr⁻¹ + M Cov_free Mᵀ, M = N_rr⁻¹ N_rs:
    //   (N_rr⁻¹)_{cc} = e_cᵀ N_rr⁻¹ e_c = n_c(c),   n_c = N_rr⁻¹ e_c
    //   (M Cov_free Mᵀ)_{cc} = m_cᵀ Cov_free m_c,   m_c = N_sr n_c  (row c of M, transposed)
    // One local solve per requested column; no full N_rr⁻¹ is formed (the serial-path cost).
    const bool armed = carry_aware && !dd_priors_.empty();
    std::vector<Eigen::VectorXd> nvec;
    std::vector<std::size_t>     sta;
    if (armed) {
        nvec.resize(static_cast<std::size_t>(k));
        sta.assign(static_cast<std::size_t>(k), station);
    }
    Eigen::VectorXd out(k);
    Eigen::VectorXd e_c = Eigen::VectorXd::Zero(st.n_local);
    for (Eigen::Index i = 0; i < k; ++i) {
        const Eigen::Index c = local_cols[i];
        if (c < 0 || c >= st.n_local) {
            fail("reconstructStationCovarianceDiag: local column out of range");
        }
        e_c(c) = 1.0;
        const Eigen::VectorXd n_c = st.Nrr_ldlt.solve(e_c);  // N_rr⁻¹ e_c (n_local)
        e_c(c) = 0.0;
        double var = n_c(c);                                 // (N_rr⁻¹)_{cc}
        if (n_shared_ > 0) {
            const Eigen::VectorXd m_c = st.N_sr * n_c;        // N_sr n_c (n_shared)
            var += m_c.dot(cov_free_xs * m_c);                // m_cᵀ Cov_free m_c
        }
        out(i) = var;
        if (armed) nvec[static_cast<std::size_t>(i)] = n_c;
    }
    if (!armed) {
        return out;
    }
    // Armed: the same per-column quantity over Ã⁻¹, one batched correction for all k
    // columns.  The fallback here is per column (unlike the matrix-valued forms): each
    // variance is a standalone number, so refusing one does not make the others
    // inconsistent.
    Eigen::MatrixXd local_corr, m_corr;
    carryCovCorrection(sta, nvec, local_corr, m_corr);
    ++carry_cov_stats_.calls;
    carry_cov_stats_.rows += static_cast<long long>(k);
    for (Eigen::Index i = 0; i < k; ++i) {
        const Eigen::VectorXd& n_c = nvec[static_cast<std::size_t>(i)];
        double var = n_c(local_cols[i]) - local_corr(i, i);
        if (n_shared_ > 0) {
            const Eigen::VectorXd m_c = st.N_sr * n_c - m_corr.col(i);
            var += m_c.dot(cov_free_xs * m_c);
        }
        if (var > 0.0 && std::isfinite(var)) {
            tallyCarryRatio(var, out(i));
            out(i) = var;
            ++carry_cov_stats_.corrected_rows;
        } else {
            ++carry_cov_stats_.nonpos_rows;   // refused, not clamped: carry-blind value kept (§5b)
        }
    }
    return out;
}

NetworkNormalEquations::StationRegression
NetworkNormalEquations::stationRegression(std::size_t station,
                                          Eigen::Index local_col,
                                          bool carry_aware,
                                          const std::vector<Eigen::Index>* cross_cols) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::stationRegression: "
                                "station index out of range");
    }
    const Station& st = stations_[station];
    if (local_col < 0 || local_col >= st.n_local) {
        fail("stationRegression: local column out of range");
    }
    // One local solve n_c = N_rr⁻¹ e_r gives both outputs (the same primitive
    // reconstructStationCovarianceDiag uses per column):
    //   cond_var = (N_rr⁻¹)_{rr} = n_c(r)                pure conditional variance
    //   row      = M_rᵀ = N_sr n_c = N_sr N_rr⁻¹ e_r     regression on the border
    StationRegression out;
    Eigen::VectorXd e_c = Eigen::VectorXd::Zero(st.n_local);
    e_c(local_col) = 1.0;
    const Eigen::VectorXd n_c = st.Nrr_ldlt.solve(e_c);   // N_rr⁻¹ e_r (n_local)
    out.cond_var = n_c(local_col);                        // (N_rr⁻¹)_{rr}
    out.row = (n_shared_ > 0) ? Eigen::VectorXd(st.N_sr * n_c)   // N_sr N_rr⁻¹ e_r (n_shared)
                              : Eigen::VectorXd();
    if (!carry_aware || dd_priors_.empty()) {
        // Cross covariance for network.phase_leveling_local_block (F3): the same n_c read
        // at the station's other carried columns gives (N_rr⁻¹)_{r c_j}, the conditional
        // cross covariance.  Plain-normals path only: the carry-aware correction below is
        // per column and would leave these uncorrected (the header explains why the two
        // must not be mixed).
        if (cross_cols) {
            const Eigen::Index k = static_cast<Eigen::Index>(cross_cols->size());
            out.cross.resize(k);
            for (Eigen::Index j = 0; j < k; ++j) {
                const Eigen::Index c = (*cross_cols)[static_cast<std::size_t>(j)];
                out.cross(j) = (c >= 0 && c < st.n_local)
                                   ? n_c(c)
                                   : std::numeric_limits<double>::quiet_NaN();
            }
        }
        return out;
    }
    // Armed: both outputs move to Ã⁻¹ together.  A pseudo-observation row whose
    // coefficients came from one model and whose variance came from another would be
    // worse than either, so the refusal is joint.
    Eigen::MatrixXd local_corr, m_corr;
    carryCovCorrection({station}, {n_c}, local_corr, m_corr);
    ++carry_cov_stats_.calls;
    ++carry_cov_stats_.rows;
    const double cond_var = out.cond_var - local_corr(0, 0);
    if (!(cond_var > 0.0) || !std::isfinite(cond_var)) {
        ++carry_cov_stats_.nonpos_rows;
        return out;                                       // refuse rather than clamp (§5b)
    }
    tallyCarryRatio(cond_var, out.cond_var);
    ++carry_cov_stats_.corrected_rows;
    out.cond_var = cond_var;
    if (n_shared_ > 0) out.row -= m_corr.col(0);
    return out;
}

std::vector<Eigen::MatrixXd> NetworkNormalEquations::blockConditionalCovariances(
    const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
    const Eigen::MatrixXd& cov_free_xs,
    const std::vector<std::vector<int>>& blocks) const {
    const Eigen::Index k = static_cast<Eigen::Index>(requests.size());
    std::vector<Eigen::MatrixXd> out;
    out.reserve(blocks.size());
    if (k == 0) {
        for (const auto& b : blocks) out.emplace_back(Eigen::MatrixXd::Zero(
            static_cast<Eigen::Index>(b.size()), static_cast<Eigen::Index>(b.size())));
        return out;
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("blockConditionalCovariances: cov_free_xs size mismatch");
    }
    // n_c = N_rr⁻¹ e_c per request, and M = [N_sr n_c] (shared x k), as in
    // jointConditionalCovariance; Cov_free M is then one product.
    std::vector<Eigen::VectorXd> n(static_cast<std::size_t>(k));
    Eigen::MatrixXd M(n_shared_, k);
    for (Eigen::Index i = 0; i < k; ++i) {
        const std::size_t s = requests[static_cast<std::size_t>(i)].first;
        const Eigen::Index c = requests[static_cast<std::size_t>(i)].second;
        if (s >= n_active_) {
            throw std::out_of_range("NetworkNormalEquations::blockConditionalCovariances: "
                                    "station index out of range");
        }
        const Station& st = stations_[s];
        if (c < 0 || c >= st.n_local) fail("blockConditionalCovariances: local column out of range");
        Eigen::VectorXd e_c = Eigen::VectorXd::Zero(st.n_local);
        e_c(c) = 1.0;
        n[static_cast<std::size_t>(i)] = st.Nrr_ldlt.solve(e_c);
        if (n_shared_ > 0) M.col(i) = st.N_sr * n[static_cast<std::size_t>(i)];
    }
    const Eigen::MatrixXd CM = (n_shared_ > 0) ? Eigen::MatrixXd(cov_free_xs * M)
                                               : Eigen::MatrixXd();
    for (const auto& b : blocks) {
        const Eigen::Index m = static_cast<Eigen::Index>(b.size());
        Eigen::MatrixXd C(m, m);
        for (Eigen::Index x = 0; x < m; ++x) {
            const int ia = b[static_cast<std::size_t>(x)];
            for (Eigen::Index y = x; y < m; ++y) {
                const int ib = b[static_cast<std::size_t>(y)];
                double v = (n_shared_ > 0) ? M.col(ia).dot(CM.col(ib)) : 0.0;
                if (requests[static_cast<std::size_t>(ia)].first == requests[static_cast<std::size_t>(ib)].first)
                    v += n[static_cast<std::size_t>(ia)](requests[static_cast<std::size_t>(ib)].second);
                C(x, y) = v;
                C(y, x) = v;
            }
        }
        out.push_back(std::move(C));
    }
    return out;
}

Eigen::MatrixXd NetworkNormalEquations::jointConditionalCovariance(
    const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
    const Eigen::MatrixXd& cov_free_xs, bool carry_aware) const {
    const Eigen::Index k = static_cast<Eigen::Index>(requests.size());
    if (k == 0) {
        return Eigen::MatrixXd();
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("jointConditionalCovariance: cov_free_xs size mismatch");
    }
    // Per request: n_c = N_rr⁻¹ e_c (in its station's local space) and
    // m_c = N_sr n_c (shared space).  One cached-factor solve each.
    std::vector<Eigen::VectorXd> n(k), m(k);
    std::vector<std::size_t>     sta(k);
    std::vector<Eigen::Index>    col(k);
    for (Eigen::Index i = 0; i < k; ++i) {
        const std::size_t s = requests[i].first;
        const Eigen::Index c = requests[i].second;
        if (s >= n_active_) {
            throw std::out_of_range("NetworkNormalEquations::jointConditionalCovariance: "
                                    "station index out of range");
        }
        const Station& st = stations_[s];
        if (c < 0 || c >= st.n_local) {
            fail("jointConditionalCovariance: local column out of range");
        }
        Eigen::VectorXd e_c = Eigen::VectorXd::Zero(st.n_local);
        e_c(c) = 1.0;
        n[i] = st.Nrr_ldlt.solve(e_c);                       // N_rr⁻¹ e_c
        m[i] = (n_shared_ > 0) ? Eigen::VectorXd(st.N_sr * n[i])  // N_sr N_rr⁻¹ e_c
                               : Eigen::VectorXd();
        sta[i] = s;
        col[i] = c;
    }
    // Cov_free · m_b (shared propagation), precomputed once per request.
    std::vector<Eigen::VectorXd> cm(k);
    for (Eigen::Index b = 0; b < k; ++b) {
        cm[b] = (n_shared_ > 0) ? Eigen::VectorXd(cov_free_xs * m[b]) : Eigen::VectorXd();
    }
    Eigen::MatrixXd cov(k, k);
    for (Eigen::Index a = 0; a < k; ++a) {
        for (Eigen::Index b = a; b < k; ++b) {
            double v = (n_shared_ > 0) ? m[a].dot(cm[b]) : 0.0;  // m_aᵀ Cov_free m_b
            if (sta[a] == sta[b]) {
                v += n[a](col[b]);  // (N_rr⁻¹)_{c_a,c_b}; same local space only
            }
            cov(a, b) = v;
            cov(b, a) = v;
        }
    }
    if (!carry_aware || dd_priors_.empty()) {
        return cov;
    }
    // Armed: the same joint quantity over Ã⁻¹.  The local term now survives across
    // stations: N_rr⁻¹ is block-diagonal and Ã⁻¹ is not, and a DD chord spans two
    // stations by construction, so this cross-station term is what gives a
    // between-station DD σ its meaning.
    Eigen::MatrixXd local_corr, m_corr;
    carryCovCorrection(sta, n, local_corr, m_corr);
    ++carry_cov_stats_.calls;
    carry_cov_stats_.rows += static_cast<long long>(k);
    std::vector<Eigen::VectorXd> m_dd(k), cm_dd(k);
    for (Eigen::Index i = 0; i < k; ++i) {
        m_dd[i] = (n_shared_ > 0) ? Eigen::VectorXd(m[i] - m_corr.col(i)) : Eigen::VectorXd();
    }
    for (Eigen::Index b = 0; b < k; ++b) {
        cm_dd[b] = (n_shared_ > 0) ? Eigen::VectorXd(cov_free_xs * m_dd[b]) : Eigen::VectorXd();
    }
    Eigen::MatrixXd out(k, k);
    for (Eigen::Index a = 0; a < k; ++a) {
        for (Eigen::Index b = a; b < k; ++b) {
            double v = (n_shared_ > 0) ? m_dd[a].dot(cm_dd[b]) : 0.0;
            if (sta[a] == sta[b]) v += n[a](col[b]);
            v -= local_corr(a, b);
            out(a, b) = v;
            out(b, a) = v;
        }
    }
    // Refuse the whole matrix, as §5b requires.  A k×k matrix with some entries from
    // Ã⁻¹ and some from N_rr⁻¹ is not the covariance of anything, and the DD variance
    // gᵀQg its consumers form would mix the two models with no indication.
    long long bad = 0;
    for (Eigen::Index a = 0; a < k; ++a)
        if (!(out(a, a) > 0.0) || !std::isfinite(out(a, a))) ++bad;
    if (bad > 0) {
        carry_cov_stats_.nonpos_rows += bad;
        ++carry_cov_stats_.refused_calls;
        return cov;
    }
    for (Eigen::Index a = 0; a < k; ++a) tallyCarryRatio(out(a, a), cov(a, a));
    carry_cov_stats_.corrected_rows += static_cast<long long>(k);
    return out;
}

Eigen::VectorXd NetworkNormalEquations::rowRedundancy(
    std::size_t station, const Eigen::MatrixXd& A_s_rows,
    const Eigen::MatrixXd& A_r_rows, const Eigen::VectorXd& w_rows,
    const Eigen::MatrixXd& cov_free_xs) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::rowRedundancy: "
                                "station index out of range");
    }
    const Eigen::Index m = A_s_rows.rows();
    if (A_r_rows.rows() != m || w_rows.size() != m) {
        fail("rowRedundancy: row count mismatch across A_s/A_r/w");
    }
    if (A_s_rows.cols() != n_shared_) {
        fail("rowRedundancy: A_s column count != n_shared");
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("rowRedundancy: cov_free_xs size mismatch");
    }
    const Station& st = stations_[station];
    if (A_r_rows.cols() != st.n_local) {
        fail("rowRedundancy: A_r column count != station n_local");
    }
    // r_i = 1 − w_i · a_iᵀ Cov(θ) a_i,  a_i = [as_i | ar_i].
    // a_iᵀ Cov a_i = (as_i − Mᵀ ar_i)ᵀ Cov_free (as_i − Mᵀ ar_i) + ar_iᵀ N_rr⁻¹ ar_i,
    //   M = N_rr⁻¹ N_srᵀ  (n_local × n_shared).  Cov_free is the datum-frame shared cov.
    Eigen::VectorXd r(m);
    const bool has_local = st.n_local > 0;
    const Eigen::MatrixXd M = (has_local && n_shared_ > 0)
        ? Eigen::MatrixXd(st.Nrr_ldlt.solve(st.N_sr.transpose()))  // n_local × n_shared
        : Eigen::MatrixXd();
    for (Eigen::Index i = 0; i < m; ++i) {
        const Eigen::VectorXd as_i = A_s_rows.row(i).transpose();      // n_shared
        double lev;
        if (has_local) {
            const Eigen::VectorXd ar_i   = A_r_rows.row(i).transpose(); // n_local
            const Eigen::VectorXd nrr_ar = st.Nrr_ldlt.solve(ar_i);     // N_rr⁻¹ ar_i
            double lev_local  = ar_i.dot(nrr_ar);                       // ar_iᵀ N_rr⁻¹ ar_i
            double lev_shared = 0.0;
            if (n_shared_ > 0) {
                const Eigen::VectorXd g = as_i - M.transpose() * ar_i;  // as_i − Mᵀ ar_i
                lev_shared = g.dot(cov_free_xs * g);
            }
            lev = lev_shared + lev_local;
        } else {
            lev = (n_shared_ > 0) ? as_i.dot(cov_free_xs * as_i) : 0.0;
        }
        double r_i = 1.0 - w_rows(i) * lev;
        r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;  // r_i ∈ [0,1]
        r(i) = r_i;
    }
    return r;
}

Eigen::MatrixXd NetworkNormalEquations::blockResidualCofactor(
    std::size_t station, const Eigen::MatrixXd& A_s_rows,
    const Eigen::MatrixXd& A_r_rows, const Eigen::VectorXd& w_rows,
    const Eigen::MatrixXd& cov_free_xs) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::blockResidualCofactor: "
                                "station index out of range");
    }
    const Eigen::Index b = A_s_rows.rows();
    if (A_r_rows.rows() != b || w_rows.size() != b) {
        fail("blockResidualCofactor: row count mismatch across A_s/A_r/w");
    }
    if (A_s_rows.cols() != n_shared_) {
        fail("blockResidualCofactor: A_s column count != n_shared");
    }
    if (cov_free_xs.rows() != n_shared_ || cov_free_xs.cols() != n_shared_) {
        fail("blockResidualCofactor: cov_free_xs size mismatch");
    }
    const Station& st = stations_[station];
    if (A_r_rows.cols() != st.n_local) {
        fail("blockResidualCofactor: A_r column count != station n_local");
    }
    if (b == 0) {
        return Eigen::MatrixXd();
    }
    // L_ij = a_iᵀ Cov(θ) a_j: rowRedundancy's leverage, evaluated off the diagonal
    // as well.  Same marginalization, same frame; the loop there is the i = j case
    // of the two Gram products here.  Costs one N_rr solve for the b right-hand
    // sides plus one for M = N_rr⁻¹ N_srᵀ, both through the cached factor.
    Eigen::MatrixXd L;
    const bool has_local = st.n_local > 0;
    if (has_local) {
        const Eigen::MatrixXd nrr_ar = st.Nrr_ldlt.solve(A_r_rows.transpose());  // n_local × b
        L.noalias() = A_r_rows * nrr_ar;                     // A_r N_rr⁻¹ A_rᵀ
        if (n_shared_ > 0) {
            const Eigen::MatrixXd M = st.Nrr_ldlt.solve(st.N_sr.transpose());  // n_local × n_shared
            const Eigen::MatrixXd G = A_s_rows - A_r_rows * M;                 // b × n_shared
            L.noalias() += G * cov_free_xs * G.transpose();
        }
    } else if (n_shared_ > 0) {
        L.noalias() = A_s_rows * cov_free_xs * A_s_rows.transpose();
    } else {
        L = Eigen::MatrixXd::Zero(b, b);
    }
    // Q = P⁻¹ − L.  The two Gram products are symmetric only in exact arithmetic,
    // and the consumer factorizes this matrix to form ẽᵀQ⁻¹ẽ, where a half-ulp
    // asymmetry gives a wrong quadratic form with no diagnostic; hence the
    // symmetrization.  Not clamped: a fully leveraged block must come back singular
    // (first @note in the header).
    Eigen::MatrixXd Q(b, b);
    for (Eigen::Index i = 0; i < b; ++i) {
        for (Eigen::Index j = i; j < b; ++j) {
            const double lev = 0.5 * (L(i, j) + L(j, i));
            const double v = (i == j) ? (1.0 / w_rows(i) - lev) : -lev;
            Q(i, j) = v;
            Q(j, i) = v;
        }
    }
    return Q;
}

Eigen::Index NetworkNormalEquations::numLocal(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::numLocal: "
                                "station index out of range");
    }
    return stations_[station].n_local;
}

const Eigen::MatrixXd& NetworkNormalEquations::stationCrossBlock(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::stationCrossBlock: "
                                "station index out of range");
    }
    return stations_[station].N_sr;
}

const Eigen::VectorXd& NetworkNormalEquations::stationLocalRhs(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::stationLocalRhs: "
                                "station index out of range");
    }
    return stations_[station].u_r;
}

Eigen::MatrixXd NetworkNormalEquations::stationLocalNormals(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::stationLocalNormals: "
                                "station index out of range");
    }
    const Station& st = stations_[station];
    if (st.n_local == 0) return Eigen::MatrixXd(0, 0);
    return st.Nrr_ldlt.reconstructedMatrix();
}

Eigen::VectorXd NetworkNormalEquations::stationLocalPivots(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::stationLocalPivots: "
                                "station index out of range");
    }
    const Station& st = stations_[station];
    if (st.n_local == 0) return Eigen::VectorXd();
    const Eigen::VectorXd d = st.Nrr_ldlt.vectorD();
    // Undo Eigen's symmetric pivoting.  Applying the transposition sequence to
    // the identity labelling gives perm[k] = the original column at permuted
    // slot k (the same k-th/indices[k] swap order LDLT::solve uses), so the
    // pivot D(k) belongs to original column perm[k].
    const auto& tr = st.Nrr_ldlt.transpositionsP();
    std::vector<Eigen::Index> perm(static_cast<std::size_t>(st.n_local));
    for (Eigen::Index k = 0; k < st.n_local; ++k) perm[static_cast<std::size_t>(k)] = k;
    for (Eigen::Index k = 0; k < st.n_local; ++k) {
        const Eigen::Index j = static_cast<Eigen::Index>(tr.coeff(k));
        if (j >= 0 && j < st.n_local)
            std::swap(perm[static_cast<std::size_t>(k)], perm[static_cast<std::size_t>(j)]);
    }
    Eigen::VectorXd out(st.n_local);
    for (Eigen::Index k = 0; k < st.n_local && k < d.size(); ++k)
        out(perm[static_cast<std::size_t>(k)]) = d(k);
    return out;
}

} // namespace GnssProcessor
