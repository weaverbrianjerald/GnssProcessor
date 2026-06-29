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

#include <sstream>
#include <stdexcept>
#include <string>

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
}

void NetworkNormalEquations::addSharedInfo(const Eigen::MatrixXd& delta_info,
                                           const Eigen::VectorXd& delta_rhs) {
    requireSquare(delta_info, n_shared_, "addSharedInfo: delta_info");
    if (delta_rhs.size() != n_shared_) {
        fail("addSharedInfo: delta_rhs size mismatch");
    }
    N_ss_ += delta_info;
    u_s_ += delta_rhs;
    reduced_ = false;
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

void NetworkNormalEquations::reduce() {
    // Start from the accumulated shared border (Σ_r N_ss^r + priors), then
    // subtract each station's Schur contribution N_sr (N_rr)^{-1} N_rs.
    reduced_N_ss_ = N_ss_;
    reduced_u_s_ = u_s_;

    // Iterate only the ACTIVE pool slots [0, n_active_) — slots beyond that are
    // retained allocations from a previous (larger) epoch, not part of this solve.
    for (std::size_t i = 0; i < n_active_; ++i) {
        const Station& st = stations_[i];
        if (st.n_local == 0) {
            continue; // no nuisance block to eliminate
        }
        // N_rr^{-1} N_rs  (n_r × n_s), with N_rs = N_sr^ᵀ.
        const Eigen::MatrixXd Nrr_inv_Nrs = st.Nrr_ldlt.solve(st.N_sr.transpose());
        // N_rr^{-1} u_r   (n_r).
        const Eigen::VectorXd Nrr_inv_ur = st.Nrr_ldlt.solve(st.u_r);

        reduced_N_ss_.noalias() -= st.N_sr * Nrr_inv_Nrs;
        reduced_u_s_.noalias() -= st.N_sr * Nrr_inv_ur;
    }

    // The Schur complement of a symmetric matrix is symmetric; enforce it
    // explicitly to clean up floating-point asymmetry before the solve (avoids
    // the aliasing pitfall — write into a temporary, then assign).
    reduced_N_ss_ = (0.5 * (reduced_N_ss_ + reduced_N_ss_.transpose())).eval();

    reduced_ = true;
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
    const Eigen::VectorXd& constrained_u_s) const {
    requireSquare(constrained_N_ss, n_shared_,
                  "solveSharedConstrained: constrained_N_ss");
    if (constrained_u_s.size() != n_shared_) {
        fail("solveSharedConstrained: constrained_u_s size mismatch");
    }
    if (n_shared_ == 0) {
        return Eigen::VectorXd();
    }
    Eigen::LDLT<Eigen::MatrixXd> ldlt(constrained_N_ss);
    requireFactorPd(ldlt,
                    "solveSharedConstrained: constrained shared block is not "
                    "positive-definite (the applied S-basis constraint did not "
                    "remove the datum deficiency)");
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

Eigen::Index NetworkNormalEquations::numLocal(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::numLocal: "
                                "station index out of range");
    }
    return stations_[station].n_local;
}

} // namespace GnssProcessor
