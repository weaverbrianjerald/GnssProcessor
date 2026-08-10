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

#include <fmt/format.h>
#include <spdlog/spdlog.h>

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

Eigen::Index NetworkEstimator::registerVtecCoeff(int n, int m, char trig, int layer) {
    if (epoch_open_) {
        throw std::logic_error(
            "NetworkEstimator: cannot register shared parameters after "
            "beginEpoch(); finalize the layout first");
    }
    const std::string k = StateKeys::makeVtecShKey(n, m, trig, layer);
    auto it = index_.find(k);
    if (it != index_.end()) return it->second;
    const Eigen::Index col = static_cast<Eigen::Index>(columns_.size());
    index_.emplace(k, col);
    ColumnDesc d;
    d.kind = SharedParamKind::VtecSh;
    d.sh_n = n; d.sh_m = m; d.sh_trig = trig; d.sh_layer = layer;
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

Eigen::Index NetworkEstimator::vtecCoeffColumn(int n, int m, char trig, int layer) const {
    return index_.at(StateKeys::makeVtecShKey(n, m, trig, layer));
}

std::string NetworkEstimator::sharedColumnLabel(Eigen::Index col) const {
    if (col < 0 || col >= static_cast<Eigen::Index>(columns_.size())) {
        throw std::out_of_range("NetworkEstimator::sharedColumnLabel: "
                                "column index out of range");
    }
    const ColumnDesc& d = columns_[static_cast<std::size_t>(col)];
    switch (d.kind) {
        case SharedParamKind::Clock:     return "CLK:" + d.sat;
        case SharedParamKind::CodeBias:  return "CB:" + d.sat + ":" + d.signal;
        case SharedParamKind::PhaseBias: return "PB:" + d.sat + ":" + d.signal;
        case SharedParamKind::VtecSh:
            return std::string(d.sh_layer > 0 ? "SH2:" : "SH:") + d.sh_trig + ":"
                   + std::to_string(d.sh_n) + ":" + std::to_string(d.sh_m);
    }
    return "UNKNOWN:" + std::to_string(col);   // unreachable; keeps -Wreturn-type quiet
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
    // RHS-attribution recorder: a re-opened epoch (including the within-epoch
    // RAIM re-solve) must retain only the FINAL build's captures — clear the
    // list, keep the armed flag (see attribArm()).
    if (attrib_armed_) attrib_shared_.clear();
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
        // network.parallel_carry: run the fold SERIAL-over-blocks (block_threads=1) with
        // WITHIN-BLOCK per-station threading (station_threads) — the fold's dominant cost is
        // the per-station reduction, which the seam's block-parallelism (only ~few blocks)
        // and Eigen's GEMM threading both leave serial.  OFF ⇒ seam_threads_ block-parallel
        // (byte-identical), no within-block fan-out.
        if (carry_par_threads_ > 1)
            neq_.foldReducedDdPriors(dd_priors_, /*block_threads=*/1, carry_par_threads_);
        else
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
    std::size_t station, const Datum& datum, bool carry_aware) const {
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
    return neq_.reconstructStationCovariance(station, cov_free, carry_aware);
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
    const Datum& datum, bool carry_aware) const {
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
    return neq_.jointConditionalCovariance(requests, cov_free, carry_aware);
}

Eigen::VectorXd NetworkEstimator::conditionalLocalVarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Datum& datum, bool carry_aware) const {
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
    return neq_.reconstructStationCovarianceDiag(station, local_cols, cov_free,
                                                carry_aware);
}

NetworkNormalEquations::StationRegression NetworkEstimator::stationRegression(
    std::size_t station, Eigen::Index local_col, bool carry_aware) const {
    CovTimer _t(neq_);  // D0 (cov consumer)
    // Pure normals — no datum frame (M_r and (N_rr⁻¹)_{rr} are datum-independent).
    // The carry-aware form is datum-independent too: Ã⁻¹ = N_rr⁻¹ − Σ_b Y_b M_b Y_bᵀ is
    // built from the accumulated normals and the fold's own factors, not from Σ_ss.
    return neq_.stationRegression(station, local_col, carry_aware);
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
            ? StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig, p.sh_layer)
            : key_(p.sat, p.kind, p.signal);
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        delta_info(c, c) += p.info;          // diagonal info prior (D2 carry-forward)
        delta_rhs(c)     += p.info * p.value;
    }
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
    neq_.addSharedInfo(delta_info, delta_rhs);
}

bool NetworkEstimator::denseFoldTerms_(const std::vector<SharedPrior>& keys,
                                       const Eigen::MatrixXd& cov,
                                       const std::set<std::string>& skip,
                                       std::vector<Eigen::Index>& col,
                                       std::vector<Eigen::Index>& in_idx,
                                       Eigen::MatrixXd& Lambda,
                                       Eigen::VectorXd& eta,
                                       bool clock_gauge_free,
                                       std::size_t* gauge_free_keys) const {
    if (gauge_free_keys) *gauge_free_keys = 0;
    const Eigen::Index nk = static_cast<Eigen::Index>(keys.size());
    in_idx.clear();
    col.clear();
    // Map identity keys onto this epoch's layout.  An absent key is MARGINALIZED:
    // its rows/cols of the prior COVARIANCE are dropped before the inversion.
    // (Dropping rows of Λ would instead condition on the departed key — wrong
    // statistics for every survivor; see the header contract.)  Card Q3's
    // refloat marginalizes on exactly the same footing via `skip`.
    in_idx.reserve(keys.size());
    col.reserve(keys.size());
    for (Eigen::Index i = 0; i < nk; ++i) {
        const SharedPrior& p = keys[static_cast<std::size_t>(i)];
        if (p.kind == SharedParamKind::Clock && skip.count(p.sat)) continue;  // Q3 refloat
        const std::string pkey = (p.kind == SharedParamKind::VtecSh)
            ? StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig, p.sh_layer)
            : key_(p.sat, p.kind, p.signal);
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // not in this epoch's layout → marginalized
        in_idx.push_back(i);
        col.push_back(it->second);
    }
    const Eigen::Index k = static_cast<Eigen::Index>(in_idx.size());
    if (k == 0) return false;

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
    Lambda = ldlt.solve(Eigen::MatrixXd::Identity(k, k));
    Lambda = (dinv.asDiagonal() * Lambda * dinv.asDiagonal()).eval();
    // Explicit symmetry: the kernel symmetrizes only on the post-reduce fast path;
    // the within-epoch RAIM replay re-folds this block PRE-reduce, and the two
    // paths must agree bit-for-bit.
    Lambda = (0.5 * (Lambda + Lambda.transpose())).eval();
    // Card Q3 §2.1 — marginalize the per-constellation satellite-clock GAUGE
    // direction out of the JOINT information (see addSharedPriorClockGaugeFree
    // for the algebra).  Applied here, on Λ, rather than on the covariance,
    // because "infinite prior variance along u" has no finite covariance
    // representation; the downdate is its exact information-side image.  Doing it
    // on the JOINT block is what keeps the clock↔code-bias off-diagonals the C1
    // dense carry exists to retain — only the gauge direction is removed.
    // Deliberately BEFORE the information-budget check below, so that check sees
    // the information actually folded.
    if (clock_gauge_free) {
        std::map<char, std::vector<Eigen::Index>> grp;   // constellation -> rows of Λ
        for (Eigen::Index a = 0; a < k; ++a) {
            const SharedPrior& p = keys[static_cast<std::size_t>(in_idx[static_cast<std::size_t>(a)])];
            if (p.kind == SharedParamKind::Clock && !p.sat.empty())
                grp[p.sat[0]].push_back(a);
        }
        std::vector<Eigen::Index> used;
        for (const auto& [c, rows] : grp) {
            (void)c;
            if (rows.size() < 2) continue;   // a lone carried clock IS the gauge — nothing estimable to anchor
            used.insert(used.end(), rows.begin(), rows.end());
        }
        if (!used.empty()) {
            // One column of U per constellation with >= 2 carried clocks.  Built as
            // a matrix rather than looped rank-1 downdates because the groups are
            // COUPLED through Λ (the carried posterior correlates constellations),
            // so sequential downdates would not commute and the second would be
            // taken against an already-modified Λ.
            std::vector<char> cs;
            for (const auto& [c, rows] : grp) if (rows.size() >= 2) cs.push_back(c);
            Eigen::MatrixXd U = Eigen::MatrixXd::Zero(k, static_cast<Eigen::Index>(cs.size()));
            for (std::size_t j = 0; j < cs.size(); ++j)
                for (Eigen::Index a : grp.at(cs[j]))
                    U(a, static_cast<Eigen::Index>(j)) = 1.0;
            const Eigen::MatrixXd LU = Lambda * U;          // k x m
            const Eigen::MatrixXd M  = U.transpose() * LU;  // m x m, PSD
            const Eigen::LDLT<Eigen::MatrixXd> mf(M);
            if (mf.info() == Eigen::Success && mf.vectorD().minCoeff() > 0.0) {
                Lambda -= LU * mf.solve(LU.transpose());
                Lambda = (0.5 * (Lambda + Lambda.transpose())).eval();
                if (gauge_free_keys) *gauge_free_keys = used.size();
            }
            // A singular UᵀΛU means the block already carries no information along
            // the gauge (nothing to remove) — leaving Λ untouched is the correct
            // and only safe action; the caller's counter then reads 0 for the fold.
        }
    }
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
    eta = Lambda * v;
    return true;
}

std::size_t NetworkEstimator::addSharedPriorDense(const std::vector<SharedPrior>& keys,
                                                  const Eigen::MatrixXd& cov,
                                                  Eigen::VectorXd* lambda_diag,
                                                  bool clock_gauge_free) {
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

    static const std::set<std::string> kNoSkip;
    std::vector<Eigen::Index> in_idx, col;
    Eigen::MatrixXd Lambda;
    Eigen::VectorXd eta;
    if (!denseFoldTerms_(keys, cov, kNoSkip, col, in_idx, Lambda, eta, clock_gauge_free))
        return 0;
    const Eigen::Index k = static_cast<Eigen::Index>(in_idx.size());

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
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
    neq_.addSharedInfo(delta_info, delta_rhs);
    return static_cast<std::size_t>(k);
}

std::size_t NetworkEstimator::addSharedPriorClockGaugeFree(
    const std::vector<SharedPrior>& priors) {
    if (priors.empty()) return 0;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);

    // Pass 1: resolve identities.  Non-clock priors accumulate exactly as
    // addSharedPrior accumulates them (same order, same arithmetic); clock
    // priors are grouped by constellation for the downdate below.
    struct ClkRow { Eigen::Index col; double info, value; };
    std::map<char, std::vector<ClkRow>> grp;
    for (const SharedPrior& p : priors) {
        if (!(p.info > 0.0)) continue;
        const std::string pkey = (p.kind == SharedParamKind::VtecSh)
            ? StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig, p.sh_layer)
            : key_(p.sat, p.kind, p.signal);
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        if (p.kind == SharedParamKind::Clock && !p.sat.empty()) {
            grp[p.sat[0]].push_back({c, p.info, p.value});
        } else {
            delta_info(c, c) += p.info;
            delta_rhs(c)     += p.info * p.value;
        }
    }

    // Pass 2: per constellation, Λ_gf = D − λλᵀ/Λ_tot with D = diag(λ), λ = info.
    // (The general Λ − Λu(uᵀΛu)⁻¹uᵀΛ specialized to a diagonal Λ and u = 1.)
    // η = Λ_gf v = λ ⊙ (v − v̄_λ): each clock is anchored to its carried value
    // MINUS the information-weighted mean of the carried values, i.e. the
    // DIFFERENCES are anchored and the gauge is left to the datum.
    std::size_t downdated = 0;
    for (const auto& [c, rows] : grp) {
        (void)c;
        if (rows.size() < 2) continue;   // a lone carried clock IS the gauge
        double tot = 0.0, wsum = 0.0;
        for (const ClkRow& r : rows) { tot += r.info; wsum += r.info * r.value; }
        if (!(tot > 0.0)) continue;
        const double vbar = wsum / tot;
        for (std::size_t a = 0; a < rows.size(); ++a) {
            delta_rhs(rows[a].col) += rows[a].info * (rows[a].value - vbar);
            for (std::size_t b = 0; b < rows.size(); ++b) {
                const double v = (a == b ? rows[a].info : 0.0)
                               - rows[a].info * rows[b].info / tot;
                delta_info(rows[a].col, rows[b].col) += v;
            }
        }
        downdated += rows.size();
    }
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
    neq_.addSharedInfo(delta_info, delta_rhs);
    return downdated;
}

std::size_t NetworkEstimator::refoldSharedPriorClockGaugeFreeDropping(
    const std::vector<SharedPrior>& priors, const std::set<std::string>& drop_sats,
    bool restore) {
    if (priors.empty() || drop_sats.empty()) return 0;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);

    struct ClkRow { Eigen::Index col; double info, value; bool drop; };
    std::map<char, std::vector<ClkRow>> grp;
    for (const SharedPrior& p : priors) {
        if (p.kind != SharedParamKind::Clock || p.sat.empty() || !(p.info > 0.0)) continue;
        const auto it = index_.find(key_(p.sat, p.kind, p.signal));
        if (it == index_.end()) continue;
        grp[p.sat[0]].push_back({it->second, p.info, p.value, drop_sats.count(p.sat) > 0});
    }
    // The gauge-free block is NOT diagonal, so a per-key subtraction (the plain
    // withdrawSharedPrior) would leave the rest of the group folded against a
    // stale weighted mean.  Rebuild both blocks and fold the difference — the
    // exact analogue of the dense path's marginalized re-fold.
    const double sgn = restore ? -1.0 : +1.0;
    const auto accumulate = [&](const std::vector<ClkRow>& rows, bool keep_dropped,
                                double s) {
        std::vector<const ClkRow*> use;
        for (const ClkRow& r : rows)
            if (keep_dropped || !r.drop) use.push_back(&r);
        if (use.size() < 2) return;
        double tot = 0.0, wsum = 0.0;
        for (const ClkRow* r : use) { tot += r->info; wsum += r->info * r->value; }
        if (!(tot > 0.0)) return;
        const double vbar = wsum / tot;
        for (std::size_t a = 0; a < use.size(); ++a) {
            delta_rhs(use[a]->col) += s * use[a]->info * (use[a]->value - vbar);
            for (std::size_t b = 0; b < use.size(); ++b) {
                const double v = (a == b ? use[a]->info : 0.0)
                               - use[a]->info * use[b]->info / tot;
                delta_info(use[a]->col, use[b]->col) += s * v;
            }
        }
    };
    std::size_t dropped = 0;
    for (const auto& [c, rows] : grp) {
        (void)c;
        std::size_t nd = 0;
        for (const ClkRow& r : rows) if (r.drop) ++nd;
        if (nd == 0) continue;
        accumulate(rows, /*keep_dropped=*/true,  -sgn);   // remove the block as folded
        accumulate(rows, /*keep_dropped=*/false, +sgn);   // re-fold it without the refloated keys
        dropped += nd;
    }
    if (dropped == 0) return 0;
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
    neq_.addSharedInfo(delta_info, delta_rhs);
    return dropped;
}

void NetworkEstimator::withdrawSharedPrior(const std::vector<SharedPrior>& priors) {
    if (priors.empty()) return;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    // Mirror addSharedPrior's accumulation exactly, negated: the withdrawal must
    // be the arithmetic inverse of the fold, not an independent recomputation.
    for (const SharedPrior& p : priors) {
        if (!(p.info > 0.0)) continue;
        const std::string pkey = (p.kind == SharedParamKind::VtecSh)
            ? StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig, p.sh_layer)
            : key_(p.sat, p.kind, p.signal);
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        delta_info(c, c) -= p.info;
        delta_rhs(c)     -= p.info * p.value;
    }
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
    neq_.addSharedInfo(delta_info, delta_rhs);
}

std::size_t NetworkEstimator::refoldSharedPriorDenseDroppingClocks(
    const std::vector<SharedPrior>& keys, const Eigen::MatrixXd& cov,
    const std::set<std::string>& drop_sats, bool restore, bool clock_gauge_free) {
    const Eigen::Index nk = static_cast<Eigen::Index>(keys.size());
    if (cov.rows() != nk || cov.cols() != nk) {
        throw std::invalid_argument(
            "NetworkEstimator::refoldSharedPriorDenseDroppingClocks: cov must be "
            "keys.size() square");
    }
    if (nk == 0 || drop_sats.empty()) return 0;

    static const std::set<std::string> kNoSkip;
    std::vector<Eigen::Index> old_in, old_col, new_in, new_col;
    Eigen::MatrixXd L_old, L_new;
    Eigen::VectorXd e_old, e_new;
    // Both blocks are built BEFORE anything is folded, so a throw from either
    // leaves the shared system untouched (the caller's restore path is then a
    // plain "keep the pre-withdraw solution").
    const bool had_old = denseFoldTerms_(keys, cov, kNoSkip, old_col, old_in, L_old, e_old,
                                         clock_gauge_free);
    if (!had_old) return 0;
    const bool has_new = denseFoldTerms_(keys, cov, drop_sats, new_col, new_in, L_new, e_new,
                                         clock_gauge_free);
    const std::size_t dropped = old_in.size() - (has_new ? new_in.size() : 0u);
    if (dropped == 0) return 0;

    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    const auto accumulate = [&](const std::vector<Eigen::Index>& c,
                                const Eigen::MatrixXd& L, const Eigen::VectorXd& e,
                                double sign) {
        const Eigen::Index k = static_cast<Eigen::Index>(c.size());
        for (Eigen::Index a = 0; a < k; ++a) {
            delta_rhs(c[static_cast<std::size_t>(a)]) += sign * e(a);
            for (Eigen::Index b = 0; b < k; ++b)
                delta_info(c[static_cast<std::size_t>(a)], c[static_cast<std::size_t>(b)])
                    += sign * L(a, b);
        }
    };
    const double sgn = restore ? -1.0 : +1.0;   // restore folds the inverse delta
    accumulate(old_col, L_old, e_old, -sgn);           // remove the block as folded
    if (has_new) accumulate(new_col, L_new, e_new, +sgn);  // re-fold the marginalized block
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
    neq_.addSharedInfo(delta_info, delta_rhs);
    return dropped;
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
    if (attrib_armed_) attrib_shared_.push_back({attrib_tag_, delta_rhs});
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


NetworkEstimator::EstimabilityDump NetworkEstimator::dumpEstimabilitySpectrum(
        double tow, int declared_dim,
        const std::vector<std::string>& col_desc,
        const std::shared_ptr<spdlog::logger>& log) {
    // DG5: the machine-readable mirror is FILLED FROM the same locals the warn
    // lines format, inside the same loops and in the same (largest-ev-first)
    // order — so a network_estimability.csv row and its log line can never
    // disagree, and the log text stays byte-identical (the estim golden arm).
    EstimabilityDump out;
    const Eigen::MatrixXd& Nss = reducedSharedNormals();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(
        Nss, Eigen::ComputeEigenvectors);
    if (es.info() == Eigen::Success) {
        const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
        const Eigen::Index k = ev.size();
        const double smax = (k > 0) ? ev(k - 1) : 0.0;  // largest = last
        // DG2: the RESOLVED tolerance — the dump must describe the decision the
        // gate is about to make, so it reads the same member the gate call site
        // and the drop-and-heal read (never the kNullSpaceTol default directly).
        const double cutoff = rank_gate_tol_ * std::max(smax, 1.0);
        int below = 0;
        for (Eigen::Index i = 0; i < k; ++i) if (ev(i) < cutoff) ++below;
        // Smallest ntail eigenvalues, printed largest-first so the very
        // smallest (the gauge freedoms) sit at the end of the list.
        const Eigen::Index ntail = std::min<Eigen::Index>(14, k);
        std::string tail;
        for (Eigen::Index i = ntail - 1; i >= 0; --i)
            tail += fmt::format("{}{:.3e}", (i == ntail - 1 ? "" : ", "), ev(i));
        log->warn("Network[estimability]: TOW {:.1f} reduced_N_ss {}x{}, "
                  "declared_dim={}, ev_max={:.3e}, cutoff={:.3e}, {} ev<cutoff; "
                  "smallest {} ev = [{}]",
                  tow, Nss.rows(), Nss.cols(), declared_dim,
                  smax, cutoff, below, ntail, tail);
        out.valid        = true;
        out.tow          = tow;
        out.n_shared     = static_cast<int>(Nss.rows());
        out.declared_dim = declared_dim;
        out.ev_max       = smax;
        out.cutoff       = cutoff;
        out.n_below      = below;
        // Name the parameters spanning each near-zero (unpinned-gauge)
        // direction, printed largest-first to mirror the tail line above.
        const Eigen::MatrixXd& V = es.eigenvectors();
        const Eigen::Index ndump = std::min<Eigen::Index>(below + 2, k);
        out.dirs.reserve(static_cast<size_t>(std::max<Eigen::Index>(ndump, 0)));
        for (Eigen::Index j = ndump - 1; j >= 0; --j) {
            const Eigen::VectorXd v = V.col(j).cwiseAbs();
            std::vector<Eigen::Index> idx(v.size());
            for (Eigen::Index t = 0; t < v.size(); ++t) idx[t] = t;
            std::partial_sort(idx.begin(),
                              idx.begin() + std::min<Eigen::Index>(5, v.size()), idx.end(),
                              [&](Eigen::Index a, Eigen::Index b){ return v(a) > v(b); });
            std::string top;
            EstimabilityDirection row;
            row.ev = ev(j);
            for (int t = 0; t < 5 && t < v.size(); ++t) {
                const Eigen::Index c = idx[t];
                const std::string label = (c < static_cast<Eigen::Index>(col_desc.size())
                                           && !col_desc[c].empty())
                                              ? col_desc[c] : fmt::format("col{}", c);
                top += fmt::format("{}{}={:.2f}", (t ? ", " : ""), label, v(c));
                row.top.emplace_back(label, v(c));
            }
            log->warn("Network[estimability]:   ev={:.3e} dir spanned by [{}]", ev(j), top);
            out.dirs.push_back(std::move(row));
        }
    }
    return out;
}

} // namespace GnssProcessor
