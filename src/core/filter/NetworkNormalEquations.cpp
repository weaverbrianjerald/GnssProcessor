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
#include <chrono>
#include <cmath>
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

// --- Tier D0 section timers (network.trace_section_timers) -------------------
// A zero-overhead-when-disabled RAII accumulator: when @p on is false it records
// no timestamp and touches nothing (so an unarmed run makes NO clock reads and is
// numerically byte-identical), and when true it adds its scope's wall-clock ms to
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
    sumzero_gauges_.clear();
    station_sumzero_.clear();
    sumzero_folded_ = false;
    dd_priors_.clear();
    station_ddprior_.clear();
    ddprior_folded_ = false;
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
        // Reference scale for the relative rank test.  Legacy: the LARGEST pivot.
        // Pin-aware (setStationPdMedianScale): the MEDIAN |pivot| — a deliberate
        // huge-weight gauge-pin row in the block (the prioritized phase-datum
        // anchor's 1e10 ambiguity pin) otherwise inflates the bar by ~10 orders
        // and ejects a healthy marginal station whose weakest genuine parameter
        // sits at a usable ~1e-3 (the TWTF/VOIM every-epoch non-PD class).  The
        // median is robust to any minority of pin-scale pivots yet still catches
        // a truly singular direction (a pivot many orders below block scale).
        double d_ref = d_max;
        if (station_pd_median_scale_ && d.size() > 0) {
            Eigen::VectorXd a = d.cwiseAbs();
            std::vector<double> v(a.data(), a.data() + a.size());
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                             v.end());
            d_ref = v[v.size() / 2];
        }
        if (d_min <= 0.0 || d_min < 1e-12 * d_ref) {
            std::ostringstream os;
            os << "addStation: N_rr_r is not positive-definite (min LDLᵀ pivot "
               << d_min << " vs max " << d_max
               << (station_pd_median_scale_
                       ? ", pin-aware median-scale test" : "")
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
    ScopedAccum _perf(perf_timing_, &timers_.add_station_ms);  // D0 (assembly primitive)
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

    return addStation(N_ss_r, N_sr_r, N_rr_r, u_s_r, u_r_r);
}

std::size_t NetworkNormalEquations::addStationObservations(
    const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
    const Eigen::VectorXd& w_diag, const Eigen::VectorXd& y) {
    ScopedAccum _perf(perf_timing_, &timers_.add_station_ms);  // D0 (assembly primitive)
    if (perf_timing_) ++timers_.add_station_calls;
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

    // D0: time only a REAL recompute (the idempotent no-op above already returned).
    ScopedAccum _perf(perf_timing_, &timers_.reduce_ms);
    if (perf_timing_) ++timers_.reduce_recomputes;

    // A recompute rebuilds the reduced system from scratch, so any previously
    // folded anchor-free sum-to-zero gauge (@ref foldReducedSumZeroGauges) is
    // dropped from it — its cached (g,β,γ) would be stale for both the reduced
    // normals and the constrained reconstruction.  Clear them; the driver
    // re-folds after the reduce (mirrors the code-bias / clock-constraint
    // re-apply on the within-epoch RAIM re-solve).
    sumzero_gauges_.clear();
    station_sumzero_.clear();
    sumzero_folded_ = false;
    // Same for the Card-4 DD priors (@ref foldReducedDdPriors): a rebuilt reduced
    // system drops the folded G M Gᵀ; the driver re-folds after the reduce.
    dd_priors_.clear();
    station_ddprior_.clear();
    ddprior_folded_ = false;

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

void NetworkNormalEquations::foldReducedSumZeroGauges(
    const std::vector<SumZeroGauge>& gauges) {
    if (!reduced_) {
        reduce();  // need the reduced system + the cached per-station factors
    }
    // Idempotent within one reduce: the fold mutates reduced_N_ss_ in place, and
    // both the null-space gate (via reducedSharedNormals) and the solve trigger it,
    // so a second call must NOT double-add g gᵀ/β.  A reduce() recompute / reset()
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

    // Cache N_rr⁻¹ u_r per station lazily — a station appears in one gauge per
    // (sat,signal) it tracks, so the u_r solve would otherwise repeat ~15×.  The
    // per-column solve n_r = N_rr⁻¹ e_col is genuinely per (station,gauge).
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
            continue;  // degenerate gauge (no valid terms) — skip
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
    const std::vector<DdPriorBlock>& blocks) {
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
    if (blocks.empty() || n_shared_ == 0) {
        return;
    }

    // Cache N_rr⁻¹ u_r per station (reused across every block/row that touches it).
    std::vector<bool> have_ur(n_active_, false);
    std::vector<Eigen::VectorXd> nrr_inv_ur(n_active_);

    for (const auto& blk : blocks) {
        const Eigen::Index K = static_cast<Eigen::Index>(blk.rows.size());
        if (blk.Pz.rows() != K || blk.Pz.cols() != K || blk.zhat.size() != K) {
            fail("foldReducedDdPriors: Pz/zhat dimension mismatch with the row count");
        }
        // EVERY input block occupies exactly one slot (bidx == input index), so a
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
        for (const auto& [si, terms] : by_station) {
            const Station& st = stations_[si];
            Eigen::MatrixXd CrT = Eigen::MatrixXd::Zero(st.n_local, K);  // C_{L,r}ᵀ
            for (const auto& t : terms) {
                CrT(t.col, t.row) += t.coeff;
            }
            const Eigen::MatrixXd Y = st.Nrr_ldlt.solve(CrT);   // N_rr⁻¹ C_{L,r}ᵀ
            B.noalias() += CrT.transpose() * Y;                 // C_{L,r} N_rr⁻¹ C_{L,r}ᵀ
            G.noalias() += st.N_sr * Y;                         // N_sr N_rr⁻¹ C_{L,r}ᵀ
            if (!have_ur[si]) {
                nrr_inv_ur[si] = st.Nrr_ldlt.solve(st.u_r);
                have_ur[si] = true;
            }
            z_free.noalias() += CrT.transpose() * nrr_inv_ur[si];  // C_{L,r} N_rr⁻¹ u_r
        }

        // Middle inverse M = (Pᶻ + B)⁻¹ (PD: SPD prior + PSD B).  A degenerate block
        // (non-PD, e.g. a malformed prior) folds INERTLY (M=0) — a no-op that keeps its
        // index slot rather than corrupting the reduced system or shifting the keys.
        Eigen::MatrixXd PzB = blk.Pz + B;
        PzB = (0.5 * (PzB + PzB.transpose())).eval();
        Eigen::LDLT<Eigen::MatrixXd> ldlt(PzB);
        Eigen::MatrixXd M;
        if (ldlt.info() != Eigen::Success || ldlt.vectorD().minCoeff() <= 0.0) {
            M = Eigen::MatrixXd::Zero(K, K);
        } else {
            M = ldlt.solve(Eigen::MatrixXd::Identity(K, K));
            M = (0.5 * (M + M.transpose())).eval();
        }

        // Joint rank-K Woodbury fold onto the reduced shared system (a no-op when M=0).
        reduced_N_ss_.noalias() += G * M * G.transpose();
        reduced_u_s_.noalias()  += G * (M * (z_free - blk.zhat));

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
    // has M=0 ⇒ Pᶻ_post = Pᶻ (the untouched prior — no information was folded).
    const Eigen::MatrixXd PzM = fd.Pz * fd.M;                 // Pᶻ M            (K×K)
    Eigen::MatrixXd cov = fd.Pz - PzM * fd.Pz;                // conditional-given-x_s
    // Coupling through the shared-parameter uncertainty: the DD depends on x_s via
    // −Pᶻ M Gᵀ, so add (Pᶻ M Gᵀ) Σ_ss (Pᶻ M Gᵀ)ᵀ.  Empty on a zero shared block.
    if (n_shared_ > 0) {
        const Eigen::MatrixXd PzMGt = PzM * fd.G.transpose(); // Pᶻ M Gᵀ         (K×n_s)
        cov.noalias() += PzMGt * cov_free_xs * PzMGt.transpose();
    }
    cov = (0.5 * (cov + cov.transpose())).eval();             // symmetrize round-off
    return cov;
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
    //
    // D0: the factorization + point solve accrue to solve_ms; the explicit full
    // inverse (the D1 selected-inverse lever's direct cost) accrues to
    // cov_inverse_ms.  Two disjoint sub-timers, so the profiler sees how much of the
    // "solve" is actually the covariance the consumers below then propagate.
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

    // Anchor-free sum-to-zero gauge correction (@ref foldReducedSumZeroGauges):
    // the fold constrained the reduced solve, so the matching CONSTRAINED locals
    // are x_r −= n_r·(γ − gᵀ x_s)/β for each gauge this station is a term of
    // (n_r = N_rr⁻¹ e_col; γ − gᵀ x_s = the total unconstrained ambiguity sum).
    // Touches only the gauge (common-shift) direction, so every estimable
    // (between-receiver, between-satellite) quantity is unchanged.  Empty on the
    // anchor / OFF path → byte-identical.
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

    // Card-4 DD-prior correction (@ref foldReducedDdPriors): the constrained locals
    // are x_r −= N_rr⁻¹ C_{L,r}ᵀ κ, with κ = M(z_free − Gᵀ x_s − ẑ) shared across this
    // station's terms of each block.  Touches only the DD (cycle) directions; empty
    // on the OFF path → byte-identical to the pre-Card-4 reconstruction.
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
    return x_r;
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

NetworkNormalEquations::StationRegression
NetworkNormalEquations::stationRegression(std::size_t station,
                                          Eigen::Index local_col) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::stationRegression: "
                                "station index out of range");
    }
    const Station& st = stations_[station];
    if (local_col < 0 || local_col >= st.n_local) {
        fail("stationRegression: local column out of range");
    }
    // One local solve n_c = N_rr⁻¹ e_r delivers BOTH outputs (the same primitive
    // reconstructStationCovarianceDiag uses per column):
    //   cond_var = (N_rr⁻¹)_{rr} = n_c(r)                — pure conditional variance
    //   row      = M_rᵀ = N_sr n_c = N_sr N_rr⁻¹ e_r     — regression on the border.
    StationRegression out;
    Eigen::VectorXd e_c = Eigen::VectorXd::Zero(st.n_local);
    e_c(local_col) = 1.0;
    const Eigen::VectorXd n_c = st.Nrr_ldlt.solve(e_c);   // N_rr⁻¹ e_r (n_local)
    out.cond_var = n_c(local_col);                        // (N_rr⁻¹)_{rr}
    out.row = (n_shared_ > 0) ? Eigen::VectorXd(st.N_sr * n_c)   // N_sr N_rr⁻¹ e_r (n_shared)
                              : Eigen::VectorXd();
    return out;
}

Eigen::MatrixXd NetworkNormalEquations::jointConditionalCovariance(
    const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
    const Eigen::MatrixXd& cov_free_xs) const {
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
                v += n[a](col[b]);  // (N_rr⁻¹)_{c_a,c_b} — same local space only
            }
            cov(a, b) = v;
            cov(b, a) = v;
        }
    }
    return cov;
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

Eigen::Index NetworkNormalEquations::numLocal(std::size_t station) const {
    if (station >= n_active_) {
        throw std::out_of_range("NetworkNormalEquations::numLocal: "
                                "station index out of range");
    }
    return stations_[station].n_local;
}

} // namespace GnssProcessor
