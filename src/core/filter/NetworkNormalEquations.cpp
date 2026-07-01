/**
 * @file NetworkNormalEquations.cpp
 * @brief Implementation of the arrowhead Schur-reduction kernel for the joint
 *        multi-station PPP-RTK network estimator (W4-S1).
 *
 * See NetworkNormalEquations.hpp for the full block structure and the
 * marginalization identities.  All operations are dense Eigen; the local blocks
 * are factorized once on entry (LDLᵀ) and the factor is reused for both the
 * Schur reduction and the per-station back-substitution.
 */

#include "filter/NetworkNormalEquations.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

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

/// Verify an LDLᵀ factorization is positive-definite.  Eigen's @c info() only
/// flags non-finite input — LDLᵀ happily factorizes a singular / indefinite
/// matrix and would then back-solve through a pseudo-inverse.  Catch a
/// non-positive or vanishing relative pivot so a rank-deficient reduced system
/// (an under-specified S-basis datum) errors loudly instead of silently
/// emitting an under-determined solution.  @p context is thrown verbatim.
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
    // Retain the stations_ pool (its dense per-station buffers persist for reuse);
    // just mark every slot inactive.  This is what eliminates the per-epoch
    // allocate/free churn — addStation overwrites pooled slots in place.
    n_active_ = 0;
    reduced_ = false;
    reduce_recompute_count_ = 0;
}

void NetworkNormalEquations::addSharedInfo(const Eigen::MatrixXd& delta_info,
                                           const Eigen::VectorXd& delta_rhs) {
    requireSquare(delta_info, n_shared_, "addSharedInfo: delta_info");
    if (delta_rhs.size() != n_shared_) {
        fail("addSharedInfo: delta_rhs size mismatch");
    }
    N_ss_ += delta_info;
    u_s_ += delta_rhs;
    // Reduction-preserving fast path.  A shared-border increment carries through
    // the Schur reduction UNCHANGED: the per-station subtrahends N_sr (N_rr)^{-1}
    // N_rs depend only on N_sr / N_rr (never on N_ss_), so the reduced normals
    // shift by exactly the same delta as the pre-reduction normals.  When the
    // cached reduction is already current we therefore fold the delta straight
    // into it and KEEP it current — sparing a full re-reduction (the expensive
    // per-station loop) for a post-reduce prior such as the driver's D2
    // carry-forward, which is applied AFTER the null-space gate's reduce() and
    // before the solve.  Symmetrize the increment (0.5*(d + dᵀ)) so the update is
    // exact for ANY symmetric delta, matching reduce()'s own symmetrization (a
    // diagonal carry-forward prior is already bit-symmetric; a w·HᵀH constraint
    // increment, in general, is not to the last bit).
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
                                               const Eigen::VectorXd& u_r_r) {
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

    // Build and VALIDATE this station's local block BEFORE committing anything to
    // the shared border.  addStation throws on a non-positive-definite local block
    // (a station with an unobservable nuisance — e.g. a GPS-only station whose
    // Galileo-ISB column is structurally zero), and the driver drops such a
    // station.  A thrown station MUST leave N_ss_/u_s_ untouched: if we accumulated
    // N_ss_r up front (before the throw) its shared contribution would stay in the
    // border with NO compensating Schur term (the station is never pushed to
    // stations_, so reduce() never subtracts its N_sr Nrr⁻¹ N_rs).  Its
    // observations would then pin the shared parameters they touch with the
    // receiver-clock absorption MISSING — a GPS-only station phantom-pins the GPS
    // satellite-clock common-mode gauge, so the null-space rank gate sees one
    // fewer free gauge than declared and WITHHOLDS every epoch (this was the global
    // PPP-RTK network zero-emit: the two dropped GPS-only stations' bᵀWb summed to
    // exactly the spurious GPS-common pin).
    //
    // Build into a REUSED pool slot (its dense buffers persist across epochs — the
    // Eigen .compute()/assignments below reuse the slot's storage when the size
    // matches).  Grow the pool only on a new high-water station count.  A station
    // that throws below leaves n_active_ unadvanced and N_ss_/u_s_ untouched, so
    // its partially-written slot is simply overwritten by the next addStation —
    // the same drop-on-singular-station semantics as the old push_back path.
    if (n_active_ >= stations_.size()) {
        stations_.emplace_back();
    }
    Station& st = stations_[n_active_];
    st.n_local = n_r;

    if (n_r > 0) {
        // Factorize the local block once; reused by reduce() and
        // reconstructStation().  LDLᵀ is robust for the symmetric local normals;
        // the caller folds in the station prior so the block is full-rank.
        st.Nrr_ldlt.compute(N_rr_r);
        if (st.Nrr_ldlt.info() != Eigen::Success) {
            fail("addStation: LDLᵀ factorization of N_rr_r failed (non-finite?)");
        }
        // info()==Success only rules out non-finite input — LDLᵀ also factorizes
        // singular / indefinite blocks and would then back-solve through a
        // pseudo-inverse, corrupting the reduction and surfacing far downstream
        // as a misleading "reduced datum" failure.  Catch a non-positive-definite
        // local block here, where the diagnosis is unambiguous (the station's
        // prior is too weak / a nuisance parameter is unobservable).
        const Eigen::VectorXd d = st.Nrr_ldlt.vectorD();
        const double d_min = d.minCoeff();
        const double d_max = d.cwiseAbs().maxCoeff();
        if (d_min <= 0.0 || d_min < 1e-12 * d_max) {
            std::ostringstream os;
            os << "addStation: N_rr_r is not positive-definite (min LDLᵀ pivot "
               << d_min << " vs max " << d_max
               << ") — station prior too weak or an unobservable local parameter";
            fail(os.str());
        }
    }

    // Local block accepted — record it + commit this station's shared-border
    // contribution (kept in lock-step with the Schur term reduce() will subtract).
    st.N_sr = N_sr_r;
    st.u_r = u_r_r;
    N_ss_ += N_ss_r;
    u_s_ += u_s_r;

    reduced_ = false;
    return n_active_++;
}

std::size_t NetworkNormalEquations::addStationObservations(
    const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
    const Eigen::MatrixXd& W, const Eigen::VectorXd& y) {
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

    return addStation(N_ss_r, N_sr_r, N_rr_r, u_s_r, u_r_r);
}

std::size_t NetworkNormalEquations::addStationObservations(
    const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
    const Eigen::VectorXd& w_diag, const Eigen::VectorXd& y) {
    const Eigen::Index m = A_s.rows();
    // Validate ALL dimensions before forming any product — the asDiagonal
    // products below would otherwise trip a hard Eigen assertion (Debug crash /
    // Release UB) on a mismatch instead of throwing a clean std::invalid_argument.
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

    return addStation(N_ss_r, N_sr_r, N_rr_r, u_s_r, u_r_r);
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
    // Validate EVERYTHING before mutating the cached factor: a rank-1 update is
    // applied in place, so a throw partway through would leave N_rr half-updated.
    for (Eigen::Index i = 0; i < k; ++i) {
        const Eigen::Index c = local_cols[i];
        if (c < 0 || c >= st.n_local) {
            fail("addStationPrior: local column out of range");
        }
        if (!(delta_Nrr_diag(i) >= 0.0)) {
            fail("addStationPrior: information increment must be non-negative");
        }
        // A non-finite RHS increment would silently poison u_r (and thus the shared
        // solve) without tripping any PD check — the rank-1 factor update is
        // unaffected by u_r.  Reject it here, where "validate EVERYTHING before
        // mutating" is the kernel's contract.
        if (!std::isfinite(delta_ur(i))) {
            fail("addStationPrior: RHS increment is not finite");
        }
    }
    // Fold each diagonal increment Λ_c into N_rr via a rank-1 LDLᵀ update
    // (N_rr ← N_rr + Λ_c·e_c·e_cᵀ), and accumulate the RHS η_c into u_r.  A
    // positive Λ_c keeps the factor positive-definite, so no PD re-check is
    // needed; a zero Λ_c is skipped (a no-op column).  Iterate in the caller's
    // (deterministic, ascending) order so the update sequence — and thus the
    // ULP-level result — is reproducible.
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
    // (addStation / addSharedInfo) has marked it stale.  addSharedInfo carries a
    // shared-border delta through the reduction in place (keeping reduced_ true),
    // so the common driver path — gate reduce() → D2 carry-forward addSharedInfo →
    // solve() reduce() — runs the expensive per-station Schur loop ONCE, not twice.
    if (reduced_) {
        return;
    }

    // Start from the accumulated shared border (Σ_r N_ss^r + priors), then
    // subtract each station's Schur contribution N_sr (N_rr)^{-1} N_rs.
    reduced_N_ss_ = N_ss_;
    reduced_u_s_ = u_s_;

    // Per-station elimination.  Each station's subtrahend N_sr (N_rr)^{-1} N_rs is
    // independent (its own cached LDLᵀ factor, its own N_sr) — only the accumulation
    // into the shared border is a reduction.  The helper computes the two local
    // solves; the serial path then FUSES the product into the running subtraction
    // (bit-identical to the historical loop), while the optional parallel path
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
    // for the epoch, so each station's dense kernels stay serial — no nested
    // oversubscription.  Determinism: each thread owns a CONTIGUOUS station range
    // summed in ascending index, and the per-thread partials are combined in
    // ascending thread order, so the result is reproducible for a fixed thread
    // count (differing from the serial fold only at ULP — a different summation
    // grouping).  n_threads ≤ 1 takes the exact serial path below.
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
        // Serial fold over the ACTIVE pool slots [0, n_active_) — slots beyond that
        // are retained allocations from a previous (larger) epoch.  Fused
        // product-subtract in ascending index — BIT-IDENTICAL to the historical loop.
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
    // explicitly to clean up floating-point asymmetry before the solve (avoids
    // the aliasing pitfall — write into a temporary, then assign).
    reduced_N_ss_ = (0.5 * (reduced_N_ss_ + reduced_N_ss_.transpose())).eval();

    reduced_ = true;
    ++reduce_recompute_count_;  // reduce-once invariant instrumentation (this is a real recompute)
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
    // Factorize the datum-constrained reduced normals ONCE and reuse the factor for
    // both the point solve and (optionally) the formal covariance — the inverse of
    // the same matrix — so the caller need not refactor it.  The PD guard below is
    // the single rank-deficiency safety net (an under-specified datum throws here).
    Eigen::LDLT<Eigen::MatrixXd> ldlt(constrained_N_ss);
    requireFactorPd(ldlt,
                    "solveSharedConstrained: constrained shared block is not "
                    "positive-definite (the applied S-basis constraint did not "
                    "remove the datum deficiency)");
    if (cov_out) {
        *cov_out = ldlt.solve(Eigen::MatrixXd::Identity(n_shared_, n_shared_));
    }
    return ldlt.solve(constrained_u_s);
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
    return st.Nrr_ldlt.solve(rhs);
}

Eigen::MatrixXd NetworkNormalEquations::reconstructStationCovariance(
    std::size_t station, const Eigen::MatrixXd& cov_free_xs) const {
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
    return 0.5 * (cov + cov.transpose());  // symmetrize numerical drift
}

Eigen::VectorXd NetworkNormalEquations::reconstructStationCovarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Eigen::MatrixXd& cov_free_xs) const {
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
    // One local solve per requested column — no full N_rr⁻¹ (the serial-path cost).
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
    }
    return out;
}

Eigen::Index NetworkNormalEquations::numLocal(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::numLocal: "
                                "station index out of range");
    }
    return stations_[station].n_local;
}

} // namespace GnssProcessor
