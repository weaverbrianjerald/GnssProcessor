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

#include <algorithm>
#include <stdexcept>

namespace GnssProcessor {

std::string NetworkEstimator::key_(const std::string& sat, SharedParamKind kind,
                                   const std::string& signal) {
    // Clock is signal-independent; code/phase bias are per-signal.  The kind
    // tag keeps the three families disjoint in the same map.
    switch (kind) {
        case SharedParamKind::Clock:
            return sat + "|CLK";
        case SharedParamKind::CodeBias:
            return sat + "|CB|" + signal;
        case SharedParamKind::PhaseBias:
            return sat + "|PB|" + signal;
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

void NetworkEstimator::reset() {
    // Clear the shared-column layout for a new epoch.  index_ (a few-hundred-node
    // map) and columns_ are small; their per-epoch churn is negligible next to the
    // per-station Schur blocks, which the retained neq_ pool (reset in beginEpoch)
    // now reuses.  Flags are cleared so a fresh register → beginEpoch sequence is
    // valid; neq_ itself is NOT reconstructed here (beginEpoch reuses it).
    index_.clear();
    columns_.clear();
    shared_cov_.resize(0, 0);   // match a freshly-constructed estimator exactly
    epoch_open_ = false;
    solved_ = false;
}

void NetworkEstimator::beginEpoch() {
    // Reuse the kernel's retained per-station allocations rather than constructing
    // a fresh NetworkNormalEquations (which would free + reallocate every station
    // block each epoch — the memory-churn the network producer accumulates).
    neq_.reset(numShared());
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
    neq_.reduce();

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

    const Eigen::VectorXd x_s = neq_.solveSharedConstrained(N, u);

    // Cache the shared covariance (inverse of the datum-constrained reduced
    // normals) for the driver's between-epoch carry-forward prior (W4-S2b D2).
    // N is full rank here (the datum removed the deficiency, as solveShared-
    // Constrained already verified), so the symmetric inverse is well-defined.
    shared_cov_ = N.ldlt().solve(
        Eigen::MatrixXd::Identity(numShared(), numShared()));

    solved_ = true;
    return x_s;
}

Eigen::VectorXd NetworkEstimator::reconstructStation(
    std::size_t station, const Eigen::VectorXd& x_s) const {
    return neq_.reconstructStation(station, x_s);
}

const Eigen::MatrixXd& NetworkEstimator::reducedSharedNormals() {
    neq_.reduce();
    return neq_.reducedSharedNormals();
}

void NetworkEstimator::addSharedPrior(const std::vector<SharedPrior>& priors) {
    if (priors.empty()) return;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    for (const SharedPrior& p : priors) {
        if (!(p.info > 0.0)) continue;
        const auto it = index_.find(key_(p.sat, p.kind, p.signal));
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        delta_info(c, c) += p.info;          // diagonal info prior (D2 carry-forward)
        delta_rhs(c)     += p.info * p.value;
    }
    neq_.addSharedInfo(delta_info, delta_rhs);
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
                                                       int solution_id) const {
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
        NetworkSatSolution& s = slotFor(d.sat);
        switch (d.kind) {
            case SharedParamKind::Clock:
                s.clock_c0_m = x_s(c);
                s.has_clock = true;
                break;
            case SharedParamKind::CodeBias:
                s.code_bias_m[d.signal] = x_s(c);
                break;
            case SharedParamKind::PhaseBias:
                s.phase_bias[d.signal].bias_m = x_s(c);
                break;
        }
    }
    return sol;
}

} // namespace GnssProcessor
