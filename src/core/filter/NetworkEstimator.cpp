/**
 * @file NetworkEstimator.cpp
 * @brief Implementation of the single-epoch PPP-RTK network estimator (W4-S2a).
 *
 * Thin semantic layer over @ref NetworkNormalEquations: it owns the shared
 * satellite-parameter column layout, applies the explicit minimum-constraint
 * datum to the reduced shared system, and maps the result onto an emittable
 * @c NetworkSolutionEpoch.  See the header for the per-epoch sequence.
 */

#include "filter/NetworkEstimator.hpp"

#include "filter/StateKeySchema.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace GnssProcessor {

namespace {

/// Tier D0 (network.trace_section_timers): RAII wall-clock accumulator for the
/// covariance-consumer wrappers.  Timed HERE (not in the kernel) so the measured
/// span includes each wrapper's full-shared-covariance COPY + datum-column
/// zeroing — the per-call O(n_shared²) overhead that dominates when a wrapper is
/// invoked per station (e.g. the per-station carried-ambiguity harvest) and that
/// the D1 selected-inverse rewrite removes.  Zero-overhead + no clock read when
/// the kernel's timing is not armed (@ref NetworkNormalEquations::perfTiming).
struct CovTimer {
    const NetworkNormalEquations& neq;
    bool on;
    std::chrono::steady_clock::time_point t0;
    explicit CovTimer(const NetworkNormalEquations& n) : neq(n), on(n.perfTiming()) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~CovTimer() {
        if (on)
            neq.accumulateCovConsumer(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count());
    }
};

} // namespace

std::string NetworkEstimator::key_(const std::string& sat, SharedParamKind kind,
                                   const std::string& signal) {
    // Clock is signal-independent; code/phase bias are per-signal.  The kind
    // tag keeps the families disjoint in the same map.  VtecSh is keyed by
    // (n,m,trig), not (sat,signal), so it is handled by registerVtecCoeff /
    // vtecCoeffColumn directly (its key is never built from this overload).
    switch (kind) {
        case SharedParamKind::Clock:
            return sat + "|CLK";
        case SharedParamKind::CodeBias:
            return sat + "|CB|" + signal;
        case SharedParamKind::PhaseBias:
            return sat + "|PB|" + signal;
        case SharedParamKind::VtecSh:
            return sat;  // unused for VtecSh; see registerVtecCoeff
    }
    return sat; // unreachable
}

Eigen::Index NetworkEstimator::registerColumn_(const std::string& sat,
                                               SharedParamKind kind,
                                               const std::string& signal) {
    if (epoch_open_) {
        throw std::logic_error(
            "NetworkEstimator: cannot register shared parameters after "
            "beginEpoch(); finalize the layout first");
    }
    const std::string k = key_(sat, kind, signal);
    auto it = index_.find(k);
    if (it != index_.end()) {
        return it->second;
    }
    const Eigen::Index col = static_cast<Eigen::Index>(columns_.size());
    index_.emplace(k, col);
    columns_.push_back(ColumnDesc{sat, kind, signal});
    return col;
}

Eigen::Index NetworkEstimator::registerClock(const std::string& sat) {
    return registerColumn_(sat, SharedParamKind::Clock, "");
}

Eigen::Index NetworkEstimator::registerCodeBias(const std::string& sat,
                                                const std::string& signal) {
    return registerColumn_(sat, SharedParamKind::CodeBias, signal);
}

Eigen::Index NetworkEstimator::registerPhaseBias(const std::string& sat,
                                                 const std::string& signal) {
    return registerColumn_(sat, SharedParamKind::PhaseBias, signal);
}

Eigen::Index NetworkEstimator::registerVtecCoeff(int n, int m, char trig) {
    if (epoch_open_) {
        throw std::logic_error(
            "NetworkEstimator: cannot register shared parameters after "
            "beginEpoch(); finalize the layout first");
    }
    const std::string k = StateKeys::makeVtecShKey(n, m, trig);
    auto it = index_.find(k);
    if (it != index_.end()) return it->second;
    const Eigen::Index col = static_cast<Eigen::Index>(columns_.size());
    index_.emplace(k, col);
    ColumnDesc d;
    d.kind = SharedParamKind::VtecSh;
    d.sh_n = n; d.sh_m = m; d.sh_trig = trig;
    columns_.push_back(std::move(d));
    return col;
}

Eigen::Index NetworkEstimator::clockColumn(const std::string& sat) const {
    return index_.at(key_(sat, SharedParamKind::Clock, ""));
}

Eigen::Index NetworkEstimator::codeBiasColumn(const std::string& sat,
                                              const std::string& signal) const {
    return index_.at(key_(sat, SharedParamKind::CodeBias, signal));
}

Eigen::Index NetworkEstimator::phaseBiasColumn(const std::string& sat,
                                               const std::string& signal) const {
    return index_.at(key_(sat, SharedParamKind::PhaseBias, signal));
}

Eigen::Index NetworkEstimator::vtecCoeffColumn(int n, int m, char trig) const {
    return index_.at(StateKeys::makeVtecShKey(n, m, trig));
}

void NetworkEstimator::reset() {
    // Clear the shared-column layout for a new epoch.  index_ (a few-hundred-node
    // map) and columns_ are small; their per-epoch churn is negligible next to the
    // per-station Schur blocks, which the retained neq_ pool (reset in beginEpoch)
    // now reuses.  Flags are cleared so a fresh register → beginEpoch sequence is
    // valid; neq_ itself is NOT reconstructed here (beginEpoch reuses it).
    index_.clear();
    columns_.clear();
    shared_cov_.resize(0, 0);   // match a freshly-constructed estimator exactly
    phase_sumzero_gauges_.clear();  // Fix A: the ensemble datum is per-epoch
    dd_priors_.clear();             // Card 4 (M1): the DD priors are per-epoch too
    epoch_open_ = false;
    solved_ = false;
}

void NetworkEstimator::beginEpoch() {
    // Reuse the kernel's retained per-station allocations rather than constructing
    // a fresh NetworkNormalEquations (which would free + reallocate every station
    // block each epoch — the memory-churn the network producer accumulates).
    neq_.reset(numShared());
    // Card 4 (M1): the DD priors are strictly per-epoch (the z-store re-sets them
    // each epoch from the carried DDs).  Clear at the epoch boundary so an epoch
    // that sets none cannot re-fold a stale block — keeps the OFF path byte-identical
    // and makes carry_invariant robust to an epoch with no carry available.
    dd_priors_.clear();
    epoch_open_ = true;
    solved_ = false;
}

void NetworkEstimator::requireAccumulating_() const {
    if (!epoch_open_) {
        throw std::logic_error("NetworkEstimator::addStation: call beginEpoch() first");
    }
    if (solved_) {
        throw std::logic_error(
            "NetworkEstimator::addStation: epoch already solved; call beginEpoch() "
            "to start a new epoch before adding more stations");
    }
}

std::size_t NetworkEstimator::addStation(const Eigen::MatrixXd& A_s,
                                         const Eigen::MatrixXd& A_r,
                                         const Eigen::MatrixXd& W,
                                         const Eigen::VectorXd& y) {
    requireAccumulating_();
    return neq_.addStationObservations(A_s, A_r, W, y);
}

std::size_t NetworkEstimator::addStation(const Eigen::MatrixXd& A_s,
                                         const Eigen::MatrixXd& A_r,
                                         const Eigen::VectorXd& w_diag,
                                         const Eigen::VectorXd& y) {
    requireAccumulating_();
    return neq_.addStationObservations(A_s, A_r, w_diag, y);
}

Eigen::VectorXd NetworkEstimator::solve(const Datum& datum) {
    if (!epoch_open_) {
        throw std::logic_error("NetworkEstimator::solve: call beginEpoch() first");
    }
    neq_.reduce(reduce_threads_);
    // Fix A: fold the anchor-free sum-to-zero phase-bias gauges onto the reduced
    // system (idempotent within the reduce — the gate path may have folded them
    // already).  Empty on the anchor / OFF path → no-op, byte-identical.
    if (!phase_sumzero_gauges_.empty()) {
        neq_.foldReducedSumZeroGauges(phase_sumzero_gauges_);
    }
    // Card 4 (M1): fold the invariant-carry DD priors onto the reduced system
    // (idempotent within the reduce).  The DD fold and the sum-to-zero gauge fold are
    // BOTH post-reduce Woodbury folds on the pristine per-station N_rr, so composing
    // them over coupled local ambiguity columns is INEXACT (drops the cross term) — a
    // silent bias.  They are mutually exclusive by design: under carry_invariant the DD
    // priors SUPERSEDE the ensemble phase datum (spec D6, its gauge OFF), and the phase
    // datum is instead the PRE-REDUCE tree pins (addStationPrior, folded into N_rr before
    // the reduce → exact composition with this fold).  Fail fast rather than bias.
    if (!dd_priors_.empty()) {
        if (!phase_sumzero_gauges_.empty()) {
            throw std::logic_error(
                "NetworkEstimator::solve: the Card-4 DD priors and the sum-to-zero "
                "phase-bias ensemble gauge are mutually exclusive (both are post-reduce "
                "Woodbury folds on the same local block; composing them is inexact). Under "
                "carry_invariant the ensemble datum is superseded — realize the phase datum "
                "with the pre-reduce tree pins (addStationPrior) instead.");
        }
        neq_.foldReducedDdPriors(dd_priors_, seam_threads_);
    }

    // Apply the explicit minimum-constraint datum to a COPY of the reduced
    // shared system (the kernel's W5 seam), so the kernel's own reduced normals
    // stay pristine for reconstructStation().  Realize each gauge as an EXACT
    // hard pin (Dirichlet elimination): move its known contribution to the RHS,
    // then zero its row/column and set the pinned diagonal so x_col = value.
    Eigen::MatrixXd N = neq_.reducedSharedNormals();
    Eigen::VectorXd u = neq_.reducedSharedRhs();
    for (const Pin& p : datum) {
        if (p.column < 0 || p.column >= numShared()) {
            throw std::out_of_range("NetworkEstimator::solve: datum pin column "
                                    "out of range");
        }
    }
    // The pinned diagonal is set COMMENSURATE with the reduced normals' own scale
    // (not 1.0) so the pinned LDLᵀ pivots neither dominate nor distort the
    // spectrum the positive-definiteness guard inspects: an unfixed exact gauge
    // then still surfaces as a ~0 RELATIVE pivot regardless of the network's
    // weighting magnitude.  Setting N(col,col)=s and u(col)=s·value keeps
    // x_col=value exact for any s>0.  (A large-weight pseudo-obs would instead
    // inflate the max pivot, making the relative-pivot guard's threshold too
    // lenient and masking a genuine remaining rank deficiency.)  Rigorous
    // rank-vs-declared-S-basis-dimension validation is W5's null-space gate;
    // this guard catches the exact-deficiency (under-specified datum) case.
    const double pin_scale =
        N.size() > 0 ? std::max(1.0, N.diagonal().cwiseAbs().maxCoeff()) : 1.0;

    // (1) Move known pin terms to the RHS using the ORIGINAL reduced normals.
    for (const Pin& p : datum) {
        u -= N.col(p.column) * p.value;
    }
    // (2) Decouple the pinned variables, then fix x_col = value exactly.
    for (const Pin& p : datum) {
        N.row(p.column).setZero();
        N.col(p.column).setZero();
    }
    for (const Pin& p : datum) {
        N(p.column, p.column) = pin_scale;
        u(p.column) = pin_scale * p.value;
    }

    // Solve the datum-constrained reduced system AND recover the shared covariance
    // from the SAME LDLᵀ factor: the kernel factorizes the constrained N once and
    // fills shared_cov_ (the inverse of the datum-constrained reduced normals), so
    // this caller no longer refactors N a second time.  shared_cov_ feeds the
    // driver's between-epoch carry-forward prior (W4-S2b D2; its diagonal) and
    // producer-side network AR (the full block via conditionalLocalCovariance).
    const Eigen::VectorXd x_s = neq_.solveSharedConstrained(N, u, &shared_cov_);

    solved_ = true;
    return x_s;
}

void NetworkEstimator::addStationPrior(std::size_t station,
                                       const std::vector<Eigen::Index>& local_cols,
                                       const Eigen::VectorXd& delta_Nrr_diag,
                                       const Eigen::VectorXd& delta_ur) {
    if (!epoch_open_) {
        throw std::logic_error("NetworkEstimator::addStationPrior: call beginEpoch() first");
    }
    if (solved_) {
        throw std::logic_error(
            "NetworkEstimator::addStationPrior: epoch already solved; the carried "
            "prior must be injected before solve() (a local-block change must pass "
            "through the Schur reduction)");
    }
    neq_.addStationPrior(station, local_cols, delta_Nrr_diag, delta_ur);
}

Eigen::VectorXd NetworkEstimator::reconstructStation(
    std::size_t station, const Eigen::VectorXd& x_s) const {
    CovTimer _t(neq_);  // D0 (cov consumer)
    return neq_.reconstructStation(station, x_s);
}

void NetworkEstimator::zeroPhaseGaugeColumns(Eigen::MatrixXd& cov) const {
    // Fix A: each folded ensemble sum-to-zero gauge fixes a satellite phase-bias
    // shared column POST-reduce; that column is the gauge direction, so its slot of
    // shared_cov_ carries the fold's artificial variance/cross terms.  Treat it as an
    // exact datum column (zero row + col), mirroring the datum-pin zeroing.  Empty on
    // the anchor / OFF path → byte-identical no-op.
    for (const auto& gz : phase_sumzero_gauges_) {
        const Eigen::Index c = gz.shared_col;
        if (c >= 0 && c < cov.rows() && c < cov.cols()) {
            cov.row(c).setZero();
            cov.col(c).setZero();
        }
    }
}

Eigen::MatrixXd NetworkEstimator::conditionalLocalCovariance(
    std::size_t station, const Datum& datum) const {
    CovTimer _t(neq_);  // D0 (cov consumer — includes the shared_cov_ copy below)
    // Cov_free = shared_cov_ with the datum-pinned columns treated as EXACT
    // (variance 0), not as carrying the Dirichlet pin's artificial 1/pin_scale.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    // Fix A: the ensemble phase-bias gauges are exact datum columns too.
    zeroPhaseGaugeColumns(cov_free);
    return neq_.reconstructStationCovariance(station, cov_free);
}

Eigen::MatrixXd NetworkEstimator::datumFrameSharedCov(const Datum& datum) const {
    CovTimer _t(neq_);  // D0 (the n_shared² shared_cov_ copy + gauge zeroing)
    // Cov_free = shared_cov_ with the datum-pinned columns treated as EXACT (variance
    // 0), mirroring conditionalLocalCovariance so a carried-DD readout is the datum-as-
    // known covariance (the DD is gauge-free ⇒ datum-invariant).  Under carry_invariant
    // the ensemble phase gauge is OFF (mutually exclusive with the DD priors), so
    // zeroPhaseGaugeColumns is a no-op there; kept for symmetry with the other consumers.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    zeroPhaseGaugeColumns(cov_free);
    return cov_free;
}

Eigen::MatrixXd NetworkEstimator::ddPriorPosteriorCovariance(
    std::size_t b, const Datum& datum) const {
    return neq_.ddPriorPosteriorCovariance(b, datumFrameSharedCov(datum));
}

void NetworkEstimator::ddPriorInnovation(
    std::size_t b, const Eigen::VectorXd& x_s, const Datum& datum,
    Eigen::VectorXd& v, Eigen::MatrixXd& cov_v) const {
    neq_.ddPriorInnovation(b, x_s, datumFrameSharedCov(datum), v, cov_v);
}

Eigen::MatrixXd NetworkEstimator::jointConditionalCovariance(
    const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
    const Datum& datum) const {
    CovTimer _t(neq_);  // D0 (cov consumer — includes the shared_cov_ copy below)
    // Cov_free = shared_cov_ with the datum-pinned columns treated as EXACT
    // (variance 0), mirroring conditionalLocalCovariance so the between-station
    // DD covariance is differenced in the datum-as-known frame.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    zeroPhaseGaugeColumns(cov_free);
    return neq_.jointConditionalCovariance(requests, cov_free);
}

Eigen::VectorXd NetworkEstimator::conditionalLocalVarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Datum& datum) const {
    CovTimer _t(neq_);  // D0 (cov consumer — includes the shared_cov_ copy below)
    // Same datum-as-known frame as conditionalLocalCovariance: the pinned shared
    // columns are EXACT (variance 0), not carrying the Dirichlet pin's 1/pin_scale.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    // Fix A: the ensemble phase-bias gauges are exact datum columns too.  Without
    // this the folded gauge-direction variance inflates the carried ambiguity
    // conditional variance → the phase-leveling carry goes inert → the ensemble
    // reference churns → runaway (see zeroPhaseGaugeColumns).
    zeroPhaseGaugeColumns(cov_free);
    return neq_.reconstructStationCovarianceDiag(station, local_cols, cov_free);
}

NetworkNormalEquations::StationRegression NetworkEstimator::stationRegression(
    std::size_t station, Eigen::Index local_col) const {
    CovTimer _t(neq_);  // D0 (cov consumer)
    // Pure normals — no datum frame (M_r and (N_rr⁻¹)_{rr} are datum-independent).
    return neq_.stationRegression(station, local_col);
}

Eigen::VectorXd NetworkEstimator::rowRedundancy(
    std::size_t station, const Eigen::MatrixXd& A_s_rows,
    const Eigen::MatrixXd& A_r_rows, const Eigen::VectorXd& w_rows,
    const Datum& datum) const {
    CovTimer _t(neq_);  // D0 (cov consumer — includes the shared_cov_ copy below)
    // Datum-as-known frame: the pinned shared columns are EXACT gauges (variance 0).
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    // Fix A: the ensemble phase-bias gauges are exact datum columns too.
    zeroPhaseGaugeColumns(cov_free);
    return neq_.rowRedundancy(station, A_s_rows, A_r_rows, w_rows, cov_free);
}

const Eigen::MatrixXd& NetworkEstimator::reducedSharedNormals() {
    neq_.reduce(reduce_threads_);
    // Fix A: fold the anchor-free phase-bias gauges BEFORE the null-space gate
    // inspects the reduced normals, so the phase gauge reads as fixed (the gate's
    // declared dimension stays = the clock deficiency count, as in the anchor mode
    // whose local pins were pre-eliminated).  Idempotent with the solve()-side fold.
    if (!phase_sumzero_gauges_.empty()) {
        neq_.foldReducedSumZeroGauges(phase_sumzero_gauges_);
    }
    return neq_.reducedSharedNormals();
}

void NetworkEstimator::addSharedPrior(const std::vector<SharedPrior>& priors) {
    if (priors.empty()) return;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    for (const SharedPrior& p : priors) {
        if (!(p.info > 0.0)) continue;
        const std::string pkey = (p.kind == SharedParamKind::VtecSh)
            ? StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig)
            : key_(p.sat, p.kind, p.signal);
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        delta_info(c, c) += p.info;          // diagonal info prior (D2 carry-forward)
        delta_rhs(c)     += p.info * p.value;
    }
    neq_.addSharedInfo(delta_info, delta_rhs);
}

std::size_t NetworkEstimator::addSharedPriorDense(const std::vector<SharedPrior>& keys,
                                                  const Eigen::MatrixXd& cov,
                                                  Eigen::VectorXd* lambda_diag) {
    const Eigen::Index nk = static_cast<Eigen::Index>(keys.size());
    if (cov.rows() != nk || cov.cols() != nk) {
        throw std::invalid_argument(
            "NetworkEstimator::addSharedPriorDense: cov must be keys.size() square");
    }
    if (lambda_diag) *lambda_diag = Eigen::VectorXd::Zero(nk);
    if (!epoch_open_ || solved_) {
        throw std::logic_error(
            "NetworkEstimator::addSharedPriorDense: call between beginEpoch() and "
            "solve()");
    }
    if (nk == 0) return 0;

    // Map identity keys onto this epoch's layout.  An absent key is MARGINALIZED:
    // its rows/cols of the prior COVARIANCE are dropped before the inversion.
    // (Dropping rows of Λ would instead condition on the departed key — wrong
    // statistics for every survivor; see the header contract.)
    std::vector<Eigen::Index> in_idx;   // position in `keys`
    std::vector<Eigen::Index> col;      // shared-border column
    in_idx.reserve(keys.size());
    col.reserve(keys.size());
    for (Eigen::Index i = 0; i < nk; ++i) {
        const SharedPrior& p = keys[static_cast<std::size_t>(i)];
        const std::string pkey = (p.kind == SharedParamKind::VtecSh)
            ? StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig)
            : key_(p.sat, p.kind, p.signal);
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // not in this epoch's layout → marginalized
        in_idx.push_back(i);
        col.push_back(it->second);
    }
    const Eigen::Index k = static_cast<Eigen::Index>(in_idx.size());
    if (k == 0) return 0;

    // Subset + symmetrize P, then invert in correlation scaling: P = D·C·D with
    // D = diag(√P_ii) ⇒ Λ = D⁻¹·C⁻¹·D⁻¹.  C carries a unit diagonal, so the LDLT
    // sees an O(1)-scaled matrix even when the block mixes stiff zero-q phase-bias
    // carries (~1e-9 m²) with loose clock carries (~1e-2 m²).
    Eigen::MatrixXd P(k, k);
    Eigen::VectorXd v(k), dinv(k);
    for (Eigen::Index a = 0; a < k; ++a) {
        v(a) = keys[static_cast<std::size_t>(in_idx[a])].value;
        for (Eigen::Index b = 0; b < k; ++b)
            P(a, b) = 0.5 * (cov(in_idx[a], in_idx[b]) + cov(in_idx[b], in_idx[a]));
    }
    if (!P.allFinite() || !v.allFinite()) {
        throw std::runtime_error(
            "NetworkEstimator::addSharedPriorDense: non-finite carried value or "
            "covariance in the retained block");
    }
    for (Eigen::Index a = 0; a < k; ++a) {
        if (!(P(a, a) > 0.0)) {
            throw std::runtime_error(
                "NetworkEstimator::addSharedPriorDense: non-positive prior variance "
                "on a retained key (caller must pre-screen var_eff > 0)");
        }
        dinv(a) = 1.0 / std::sqrt(P(a, a));
    }
    Eigen::MatrixXd C = dinv.asDiagonal() * P * dinv.asDiagonal();
    const auto pdOk = [](const Eigen::LDLT<Eigen::MatrixXd>& f) {
        if (f.info() != Eigen::Success) return false;
        const Eigen::VectorXd dv = f.vectorD();
        const double dmax = dv.maxCoeff();
        const double dmin = dv.minCoeff();
        // κ = 1e-6, deliberately MUCH tighter than the kernel's 1e-12 solve guard:
        // this factor gets INVERTED and folded as information, so an admitted
        // condition number becomes information AMPLIFICATION on the shared
        // normals (Λ_aa = 1/(P_aa·(1−R²_a))).  Un-bounded, a near-gauge carried
        // direction (multiple correlation R² → 1) can inflate the normals' max
        // diagonal so far that the constrained solve's own relative-pivot guard
        // (1e-12·dmax) rises above genuine weakly-observed columns and throws —
        // and a solve-throw skips the epoch BEFORE the stash, so the identical
        // block would re-fold every epoch (persistent skip).  Rejecting here
        // routes the block to the driver's diagonal fallback instead (bounded at
        // the legacy 1/var_eff), which re-stashes fresh next emit — self-healing.
        return dmin > 0.0 && dmin >= 1e-6 * dmax;
    };
    Eigen::LDLT<Eigen::MatrixXd> ldlt(C);
    if (!pdOk(ldlt)) {
        // One ridge retry: +1e-9 on the unit diagonal ≈ inflating every prior
        // variance by one part in 1e9 — statistically negligible, numerically
        // decisive when a correlation brushes ±1 (near-gauge posterior directions).
        C.diagonal().array() += 1e-9;
        ldlt.compute(C);
        if (!pdOk(ldlt)) {
            throw std::runtime_error(
                "NetworkEstimator::addSharedPriorDense: carried covariance block is "
                "not positive-definite after the ridge retry — fall back to the "
                "diagonal carry for this epoch");
        }
    }
    Eigen::MatrixXd Lambda = ldlt.solve(Eigen::MatrixXd::Identity(k, k));
    Lambda = (dinv.asDiagonal() * Lambda * dinv.asDiagonal()).eval();
    // Explicit symmetry: the kernel symmetrizes only on the post-reduce fast path;
    // the within-epoch RAIM replay re-folds this block PRE-reduce, and the two
    // paths must agree bit-for-bit.
    Lambda = (0.5 * (Lambda + Lambda.transpose())).eval();
    // Absolute folded-information budget — the second half of the conditioning
    // guard (the κ check above bounds the RELATIVE amplification; this bounds the
    // ABSOLUTE injected stiffness, which is what the downstream solve guard sees).
    // 1e13 m⁻² keeps the constrained solve's relative-pivot floor (1e-12·dmax)
    // at or below ~10 m⁻² — under the weakest genuine reduced pivots (a single-
    // station low-elevation riser is O(1–100)).  Only a near-singular carried
    // block combined with an extreme zero-q variance (~1e-9 m²) can exceed it;
    // with the B4 carry floors armed (the recommended C1 operating point) it is
    // unreachable.  Max diagonal bounds max |Λ_ij| for a PSD matrix.
    static constexpr double kMaxFoldedInfo = 1e13;  // 1/m²
    if (Lambda.diagonal().maxCoeff() > kMaxFoldedInfo) {
        throw std::runtime_error(
            "NetworkEstimator::addSharedPriorDense: folded information exceeds the "
            "conditioning budget (near-singular carried block) — fall back to the "
            "diagonal carry for this epoch");
    }
    const Eigen::VectorXd eta = Lambda * v;

    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    for (Eigen::Index a = 0; a < k; ++a) {
        delta_rhs(col[static_cast<std::size_t>(a)]) += eta(a);
        for (Eigen::Index b = 0; b < k; ++b)
            delta_info(col[static_cast<std::size_t>(a)],
                       col[static_cast<std::size_t>(b)]) += Lambda(a, b);
        if (lambda_diag) (*lambda_diag)(in_idx[static_cast<std::size_t>(a)]) = Lambda(a, a);
    }
    neq_.addSharedInfo(delta_info, delta_rhs);
    return static_cast<std::size_t>(k);
}

void NetworkEstimator::addSharedConstraintRows(const Eigen::MatrixXd& H,
                                               const Eigen::VectorXd& z,
                                               double weight) {
    if (H.cols() != numShared() || H.rows() != z.size()) {
        throw std::invalid_argument(
            "NetworkEstimator::addSharedConstraintRows: dimension mismatch");
    }
    if (H.rows() == 0 || !(weight > 0.0)) return;
    const Eigen::MatrixXd delta_info = weight * (H.transpose() * H);
    const Eigen::VectorXd delta_rhs  = weight * (H.transpose() * z);
    neq_.addSharedInfo(delta_info, delta_rhs);
}

NetworkSolutionEpoch NetworkEstimator::toSolutionEpoch(const Eigen::VectorXd& x_s,
                                                       const GnssTime& epoch_time,
                                                       int iod_ssr, int provider_id,
                                                       int solution_id,
                                                       bool ambiguity_datum_ensemble) const {
    if (x_s.size() != numShared()) {
        throw std::invalid_argument(
            "NetworkEstimator::toSolutionEpoch: x_s size != numShared()");
    }
    NetworkSolutionEpoch sol;
    sol.epoch_time = epoch_time;
    sol.iod_ssr = iod_ssr;
    sol.provider_id = provider_id;
    sol.solution_id = solution_id;

    // Group shared columns by satellite, preserving first-seen sat order.
    std::map<std::string, std::size_t> sat_slot;  // sat → index into sol.sats
    auto slotFor = [&](const std::string& sat) -> NetworkSatSolution& {
        auto it = sat_slot.find(sat);
        if (it == sat_slot.end()) {
            sat_slot.emplace(sat, sol.sats.size());
            NetworkSatSolution s;
            s.sat = sat;
            sol.sats.push_back(std::move(s));
            return sol.sats.back();
        }
        return sol.sats[it->second];
    };

    for (Eigen::Index c = 0; c < numShared(); ++c) {
        const ColumnDesc& d = columns_[static_cast<std::size_t>(c)];
        // The global SH-VTEC coefficients are network-wide, not per-satellite, so
        // they are NOT mapped into the per-satellite NetworkSolutionEpoch here.
        // The driver reads them straight from x_s via vtecCoeffColumn for the trace;
        // their product serialization (IGS-SSR VTEC) is a later stage.
        if (d.kind == SharedParamKind::VtecSh) continue;
        NetworkSatSolution& s = slotFor(d.sat);
        switch (d.kind) {
            case SharedParamKind::Clock:
                s.clock_c0_m = x_s(c);
                s.has_clock = true;
                break;
            case SharedParamKind::CodeBias:
                s.code_bias_m[d.signal] = x_s(c);
                break;
            case SharedParamKind::PhaseBias: {
                NetworkPhaseBias& npb = s.phase_bias[d.signal];
                npb.bias_m = x_s(c);
                // RT0-3: flag the bias as integer-preserving for CDMA constellations
                // (GPS/GAL/BDS/QZSS).  Its uncombined satellite phase bias carries the
                // pivot-receiver ambiguity lump (Odijk Eq. 4) but preserves the integer
                // property a consumer's AR candidate selection can exploit.  GLONASS/FDMA
                // ('R') biases fold in a per-slot inter-frequency bias and are NOT
                // integer-recoverable, so they stay false.  Truthful metadata: no consumer
                // reads it yet, but it is cheap insurance before FDMA biases enter the
                // product (a consumer must not treat an 'R' bias as an integer candidate).
                // T3a: under the anchor-free ensemble datum the bias references the
                // receivers' float-ambiguity mean (Odijk Eq. 34 non-integer), so it is
                // NOT an AR candidate regardless of constellation.  Otherwise — the
                // pivot-receiver anchor datum OR the Card-3 Eq-34 single-receiver-chain
                // datum (network.phase_datum_eq34), which is mutually exclusive with the
                // ensemble so it passes false here — the emitted bias carries only
                // integer lumps and preserves the integer property for CDMA sats.
                npb.signal_integer = !ambiguity_datum_ensemble
                                     && !d.sat.empty() && d.sat[0] != 'R';
                break;
            }
            case SharedParamKind::VtecSh:
                break;  // handled above
        }
    }
    return sol;
}

NetworkEstimator::PerConstWithholdDecision NetworkEstimator::decidePerConstWithhold(
    const std::map<char, PerConstSigma0>& stats, double ceiling, int min_rows) {
    // RTA-10 pure publish/withhold policy (see the header for the semantics).
    // Publishable = clean by its OWN σ̂0_c under the SAME ceiling AND judgeable
    // (enough rows).  Every input key lands in exactly one output set.
    PerConstWithholdDecision dec;
    for (const auto& [cst, st] : stats) {
        if (st.sigma0_c <= ceiling && st.rows >= min_rows) dec.publish.insert(cst);
        else                                               dec.withhold.insert(cst);
    }
    return dec;
}

} // namespace GnssProcessor
