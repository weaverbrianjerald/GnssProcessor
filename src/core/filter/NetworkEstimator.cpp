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
#include <stdexcept>

namespace GnssProcessor {

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
    neq_.reduce(reduce_threads_);

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
    return neq_.reconstructStation(station, x_s);
}

Eigen::MatrixXd NetworkEstimator::conditionalLocalCovariance(
    std::size_t station, const Datum& datum) const {
    // Cov_free = shared_cov_ with the datum-pinned columns treated as EXACT
    // (variance 0), not as carrying the Dirichlet pin's artificial 1/pin_scale.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    return neq_.reconstructStationCovariance(station, cov_free);
}

Eigen::VectorXd NetworkEstimator::conditionalLocalVarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Datum& datum) const {
    // Same datum-as-known frame as conditionalLocalCovariance: the pinned shared
    // columns are EXACT (variance 0), not carrying the Dirichlet pin's 1/pin_scale.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    return neq_.reconstructStationCovarianceDiag(station, local_cols, cov_free);
}

Eigen::VectorXd NetworkEstimator::rowRedundancy(
    std::size_t station, const Eigen::MatrixXd& A_s_rows,
    const Eigen::MatrixXd& A_r_rows, const Eigen::VectorXd& w_rows,
    const Datum& datum) const {
    // Datum-as-known frame: the pinned shared columns are EXACT gauges (variance 0).
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    return neq_.rowRedundancy(station, A_s_rows, A_r_rows, w_rows, cov_free);
}

const Eigen::MatrixXd& NetworkEstimator::reducedSharedNormals() {
    neq_.reduce(reduce_threads_);
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
                npb.signal_integer = !d.sat.empty() && d.sat[0] != 'R';
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
