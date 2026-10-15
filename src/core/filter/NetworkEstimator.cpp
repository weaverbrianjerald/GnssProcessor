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

#include "common/GnssIdentity.hpp"
#include "filter/StateKeySchema.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>      // the held solve's NaN / infinity sentinels (card FCF)
#include <stdexcept>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunGroups.hpp"
#include <memory>

namespace GnssProcessor {

namespace {

/// RAII wall-clock accumulator for the covariance-consumer wrappers
/// (network.trace_section_timers, tier D0).  The timer sits in the wrapper, not in
/// the kernel, so the measured span includes each wrapper's copy of the full shared
/// covariance and its datum-column zeroing: an O(n_shared²) overhead per call that
/// dominates when a wrapper runs once per station (e.g. the per-station
/// carried-ambiguity harvest), and that the D1 selected-inverse rewrite removes.
/// When the kernel's timing is off (@ref NetworkNormalEquations::perfTiming), no
/// clock is read and the timer costs nothing.
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

std::string NetworkEstimator::priorKey_(const SharedPrior& p) {
    if (p.kind == SharedParamKind::VtecSh)
        return StateKeys::makeVtecShKey(p.sh_n, p.sh_m, p.sh_trig, p.sh_layer);
    if (p.hist) return "HIST|" + std::to_string(p.hist_idx);   // card SCL2 X2: a shared-history column
    if (p.clone) return cloneKey_(p.kind, p.sat, p.signal);   // card SCL clock, card BCL bias clone
    return key_(p.sat, p.kind, p.signal);
}

std::string NetworkEstimator::cloneKey_(SharedParamKind kind, const std::string& sat,
                                        const std::string& signal) {
    switch (kind) {
        case SharedParamKind::PhaseBias: return sat + "|PBC|" + signal;   // card BCL
        case SharedParamKind::CodeBias:  return sat + "|CBC|" + signal;   // card BCL
        default:                         return sat + "|CLKC";            // card SCL
    }
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

Eigen::Index NetworkEstimator::registerClockClone(const std::string& sat) {
    if (epoch_open_) {
        throw std::logic_error(
            "NetworkEstimator: cannot register shared parameters after "
            "beginEpoch(); finalize the layout first");
    }
    const std::string k = cloneKey_(SharedParamKind::Clock, sat, "");   // disjoint from the clock's "sat|CLK"
    auto it = index_.find(k);
    if (it != index_.end()) return it->second;
    const Eigen::Index col = static_cast<Eigen::Index>(columns_.size());
    index_.emplace(k, col);
    ColumnDesc d;
    d.sat   = sat;
    d.kind  = SharedParamKind::Clock;
    d.clone = true;
    columns_.push_back(std::move(d));
    ++n_clones_;
    return col;
}

Eigen::Index NetworkEstimator::registerClone(SharedParamKind kind, const std::string& sat,
                                             const std::string& signal) {
    if (kind == SharedParamKind::Clock) return registerClockClone(sat);
    if (kind != SharedParamKind::PhaseBias && kind != SharedParamKind::CodeBias)
        throw std::invalid_argument("NetworkEstimator::registerClone: a clone is of a clock, a "
                                    "phase bias or a code bias");
    if (epoch_open_) {
        throw std::logic_error(
            "NetworkEstimator: cannot register shared parameters after "
            "beginEpoch(); finalize the layout first");
    }
    const std::string k = cloneKey_(kind, sat, signal);   // disjoint from the bias's own key
    auto it = index_.find(k);
    if (it != index_.end()) return it->second;
    const Eigen::Index col = static_cast<Eigen::Index>(columns_.size());
    index_.emplace(k, col);
    ColumnDesc d;
    d.sat    = sat;
    d.kind   = kind;
    d.signal = signal;
    d.clone  = true;
    columns_.push_back(std::move(d));
    ++n_clones_;
    return col;
}

Eigen::Index NetworkEstimator::registerHistoryColumn(int j) {
    if (epoch_open_) {
        throw std::logic_error(
            "NetworkEstimator: cannot register shared parameters after "
            "beginEpoch(); finalize the layout first");
    }
    const std::string k = "HIST|" + std::to_string(j);   // disjoint from every satellite key
    auto it = index_.find(k);
    if (it != index_.end()) return it->second;
    const Eigen::Index col = static_cast<Eigen::Index>(columns_.size());
    index_.emplace(k, col);
    ColumnDesc d;
    d.kind     = SharedParamKind::Clock;
    d.hist     = true;
    d.hist_idx = j;
    columns_.push_back(std::move(d));
    ++n_hist_;
    return col;
}

Eigen::Index NetworkEstimator::historyColumn(int j) const {
    return index_.at("HIST|" + std::to_string(j));
}

bool NetworkEstimator::isHistoryColumn(Eigen::Index col) const {
    return col >= 0 && col < static_cast<Eigen::Index>(columns_.size())
        && columns_[static_cast<std::size_t>(col)].hist;
}

Eigen::Index NetworkEstimator::clockCloneColumn(const std::string& sat) const {
    return index_.at(cloneKey_(SharedParamKind::Clock, sat, ""));
}

Eigen::Index NetworkEstimator::cloneColumn(SharedParamKind kind, const std::string& sat,
                                           const std::string& signal) const {
    return index_.at(cloneKey_(kind, sat, signal));
}

bool NetworkEstimator::isClockClone(Eigen::Index col) const {
    return col >= 0 && col < static_cast<Eigen::Index>(columns_.size())
        && columns_[static_cast<std::size_t>(col)].clone;
}

bool NetworkEstimator::sharedColumnIdentity(Eigen::Index col, SharedParamKind& kind,
                                            std::string& sat, std::string& signal) const {
    if (col < 0 || col >= static_cast<Eigen::Index>(columns_.size())) return false;
    const ColumnDesc& d = columns_[static_cast<std::size_t>(col)];
    if (d.clone || d.hist || d.kind == SharedParamKind::VtecSh) return false;
    kind   = d.kind;
    sat    = d.sat;
    signal = d.signal;
    return true;
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
    if (d.clone) {   // card SCL: a clock clone; card BCL: a bias clone
        if (d.kind == SharedParamKind::PhaseBias) return "PBC:" + d.sat + ":" + d.signal;
        if (d.kind == SharedParamKind::CodeBias)  return "CBC:" + d.sat + ":" + d.signal;
        return "CLKC:" + d.sat;
    }
    if (d.hist) return "HIST:" + std::to_string(d.hist_idx);   // card SCL2 X2: a shared-history column
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
    // Clear the shared-column layout for a new epoch.  index_ (a map of a few
    // hundred nodes) and columns_ are small, so their per-epoch churn is negligible
    // next to the per-station Schur blocks, which the retained neq_ pool reuses.
    // neq_ is not reconstructed here; beginEpoch resets it.  The flags are cleared
    // so a fresh register → beginEpoch sequence is valid.
    index_.clear();
    columns_.clear();
    n_clones_ = 0;              // card SCL: the clones are per-epoch columns too
    n_hist_   = 0;              // card SCL2 X2: and so are the history columns
    shared_cov_.resize(0, 0);   // match a freshly-constructed estimator exactly
    phase_sumzero_gauges_.clear();  // the ensemble datum is per-epoch (Fix A)
    dd_priors_.clear();             // the DD priors are per-epoch too (Card 4, M1)
    // The contribution log is per epoch (card NOM N2a): read by the epoch tail, which runs
    // before the next epoch's reset.
    contrib_log_.clear();
    contrib_kind_        = ContributionKind::Unset;
    contrib_row_kinds_   = nullptr;
    contrib_prior_kinds_ = nullptr;
    contrib_replay_      = false;
    epoch_open_ = false;
    solved_ = false;
}

void NetworkEstimator::beginEpoch() {
    // Reuse the kernel's retained per-station allocations.  Constructing a fresh
    // NetworkNormalEquations would free and reallocate every station block each
    // epoch, which adds up to significant memory churn in the network producer.
    neq_.reset(numShared());
    // The DD priors are strictly per-epoch (the z-store sets them again each epoch
    // from the carried DDs).  Clearing them at the epoch boundary means an epoch
    // that sets none cannot re-fold a stale block: the off path stays
    // byte-identical, and carry_invariant tolerates an epoch with no carry
    // available (Card 4, M1).
    dd_priors_.clear();
    // RHS-attribution recorder: a re-opened epoch (including the within-epoch RAIM
    // re-solve) keeps only the captures of the final build, so the list is cleared
    // and the armed flag is kept (see attribArm()).
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
                                         const Eigen::VectorXd& y,
                                         const NetworkNormalEquations::StationPrior& prior) {
    requireAccumulating_();
    return neq_.addStationObservations(A_s, A_r, W, y, prior);
}

std::size_t NetworkEstimator::addStation(const Eigen::MatrixXd& A_s,
                                         const Eigen::MatrixXd& A_r,
                                         const Eigen::VectorXd& w_diag,
                                         const Eigen::VectorXd& y,
                                         const NetworkNormalEquations::StationPrior& prior) {
    requireAccumulating_();
    return neq_.addStationObservations(A_s, A_r, w_diag, y, prior);
}

Eigen::VectorXd NetworkEstimator::solve(const Datum& datum) {
    if (!epoch_open_) {
        throw std::logic_error("NetworkEstimator::solve: call beginEpoch() first");
    }
    neq_.reduce();
    // Fold the anchor-free sum-to-zero phase-bias gauges onto the reduced system
    // (Fix A).  The fold is idempotent within one reduce, because the null-space
    // gate path may already have folded them.  Empty on the anchor / off path:
    // no-op, byte-identical.
    if (!phase_sumzero_gauges_.empty()) {
        neq_.foldReducedSumZeroGauges(phase_sumzero_gauges_);
    }
    // Fold the invariant-carry DD priors onto the reduced system (idempotent within
    // the reduce; Card 4, M1).  The DD fold and the sum-to-zero gauge fold are both
    // post-reduce Woodbury folds on the unmodified per-station N_rr, so composing
    // them over coupled local ambiguity columns is inexact: the cross term is
    // dropped and the solution is biased with nothing to flag it.  The two are
    // mutually exclusive by design.  Under carry_invariant the DD priors supersede
    // the ensemble phase datum (spec D6, its gauge off), and the phase datum is
    // realized instead by the pre-reduce tree pins (addStationPrior, folded into
    // N_rr before the reduce, which composes exactly with this fold).  The check
    // below throws rather than return a biased solution.
    if (!dd_priors_.empty()) {
        if (!phase_sumzero_gauges_.empty()) {
            throw std::logic_error(
                "NetworkEstimator::solve: the Card-4 DD priors and the sum-to-zero "
                "phase-bias ensemble gauge are mutually exclusive (both are post-reduce "
                "Woodbury folds on the same local block; composing them is inexact). Under "
                "carry_invariant the ensemble datum is superseded — realize the phase datum "
                "with the pre-reduce tree pins (addStationPrior) instead.");
        }
        // network.parallel_carry: run the fold serially over blocks (block_threads=1)
        // with per-station threading inside each block (station_threads).  The fold's
        // dominant cost is the per-station reduction, which neither the seam's block
        // parallelism (there are only a few blocks) nor Eigen's GEMM threading spreads
        // across cores.  When off, the fold runs block-parallel over seam_threads_
        // (byte-identical) with no within-block fan-out.
        if (carry_par_threads_ > 1)
            neq_.foldReducedDdPriors(dd_priors_, /*block_threads=*/1, carry_par_threads_);
        else
            neq_.foldReducedDdPriors(dd_priors_, seam_threads_);
    }

    // Apply the explicit minimum-constraint datum to a copy of the reduced shared
    // system (the kernel's W5 seam), so the kernel's own reduced normals stay
    // unmodified for reconstructStation().  The pins are applyDatumPins (card FCF moved
    // them there verbatim, so the detached held solve pins exactly as this solve does).
    Eigen::MatrixXd N = neq_.reducedSharedNormals();
    Eigen::VectorXd u = neq_.reducedSharedRhs();
    applyDatumPins(N, u, datum);

    // Solve the datum-constrained reduced system and recover the shared covariance
    // from the same LDLᵀ factor: the kernel factorizes the constrained N once and
    // fills shared_cov_ (the inverse of the datum-constrained reduced normals), so N
    // is not factorized a second time here.  shared_cov_ feeds the driver's
    // between-epoch carry-forward prior (its diagonal; W4-S2b D2) and producer-side
    // network AR (the full block, via conditionalLocalCovariance).
    const Eigen::VectorXd x_s = neq_.solveSharedConstrained(N, u, &shared_cov_);

    solved_ = true;
    // The clock pivots of the primary build's last solve (card NOM N2a); a replay's
    // solve uses the same datum and is counted with the replay.
    if (!contrib_replay_) {
        contrib_log_.pins   = static_cast<long long>(datum.size());
        contrib_log_.solved = true;
    }
    return x_s;
}

void NetworkEstimator::applyDatumPins(Eigen::MatrixXd& N, Eigen::VectorXd& u,
                                      const Datum& datum) {
    // Moved verbatim out of solve() (card FCF); the one change is the range bound,
    // N.rows() for numShared(), which are equal at every call (solve() pins the reduced
    // normals of the open epoch, which beginEpoch sized to numShared()).
    //
    // Each gauge is realized as an exact hard pin (Dirichlet elimination): its known
    // contribution moves to the RHS, then its row and column are zeroed and the pinned
    // diagonal is set so x_col = value.
    for (const Pin& p : datum) {
        if (p.column < 0 || p.column >= N.rows()) {
            throw std::out_of_range("NetworkEstimator::solve: datum pin column "
                                    "out of range");
        }
    }
    // The pinned diagonal is set to the reduced normals' own scale (max |diag N|,
    // at least 1) rather than 1.0, so the pinned LDLᵀ pivots neither dominate nor
    // distort the spectrum the positive-definiteness guard inspects: an unfixed
    // exact gauge still shows up as a ~0 relative pivot whatever the network's
    // weighting magnitude.  Setting N(col,col)=s and u(col)=s·value keeps
    // x_col=value exact for any s>0.  A large-weight pseudo-observation would
    // instead inflate the maximum pivot, making the relative-pivot guard's
    // threshold too lenient and hiding a real remaining rank deficiency.  Full
    // rank-versus-declared-S-basis-dimension validation is W5's null-space gate;
    // this guard catches the exact-deficiency (under-specified datum) case.
    const double pin_scale =
        N.size() > 0 ? std::max(1.0, N.diagonal().cwiseAbs().maxCoeff()) : 1.0;

    // (1) Move known pin terms to the RHS using the original reduced normals.
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
}

NetworkEstimator::HeldSolve NetworkEstimator::solveHeldDetached(
    const Datum& datum, const Eigen::VectorXd& x_float,
    const std::vector<std::vector<NetworkNormalEquations::CycleRow>>& blocks) const {
    // Preconditions, read from the kernel's own state: its reduced normals are those of a
    // float solve, and nothing else is folded into them that the held solve would have to
    // undo.  (The request lists dd_priors_ / phase_sumzero_gauges_ may be changed after
    // the solve; what was folded is what the kernel holds.)
    if (!solved_) {
        throw std::logic_error("NetworkEstimator::solveHeldDetached: requires a prior solve()");
    }
    if (!neq_.isReduced()) {
        throw std::logic_error("NetworkEstimator::solveHeldDetached: the kernel's reduction is "
                               "not current (the cached system did not produce the float)");
    }
    if (neq_.numDdPriorBlocks() > 0 || neq_.numSumZeroGauges() > 0) {
        throw std::logic_error(
            "NetworkEstimator::solveHeldDetached: the float solve folded Card-4 DD priors or "
            "sum-to-zero phase gauges; the detached fold reads the unfolded station factors "
            "and composing the two is inexact (the fixed-cycle loader refuses both)");
    }
    if (x_float.size() != numShared()) {
        throw std::invalid_argument("NetworkEstimator::solveHeldDetached: x_float size mismatch");
    }
    const Eigen::MatrixXd& N0 = neq_.reducedSharedNormals();
    const Eigen::VectorXd& u0 = neq_.reducedSharedRhs();
    constexpr double kInf = std::numeric_limits<double>::infinity();
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    // The held solves are not the float path's: the kernel's section timers are put back on
    // every exit (a throw included), so the D0 run-end line and the driver's per-bracket
    // accounting count the float solves alone.  The fixed-cycle seam times itself.
    struct KernelTimersRestore {
        const NetworkNormalEquations& kernel;
        const NetworkNormalEquations::SectionTimers saved;
        ~KernelTimersRestore() { kernel.restoreSectionTimers(saved); }
    } const timers_restore{neq_, neq_.sectionTimers()};

    HeldSolve h;
    h.x_s = x_float;   // the contract: x_float, bitwise, unless the held estimate is accepted

    // (1) Float reproduction: the normwise backward error of x_float on the pinned
    // unfolded copy.  For the float this cache produced it is round-off whatever the
    // conditioning (a residual relative to |u| alone grows with cond(N) when the float
    // increment lies in weakly determined directions); a cache that no longer produced
    // x_float (a later re-solve, a post-solve shared fold) leaves it far above round-off.
    if (numShared() > 0) {
        if (!x_float.allFinite()) {
            h.float_repro_rel = kInf;
        } else {
            Eigen::MatrixXd Np = N0;
            Eigen::VectorXd up = u0;
            applyDatumPins(Np, up, datum);
            const double r_inf = (Np * x_float - up).lpNorm<Eigen::Infinity>();
            const double n_inf = Np.cwiseAbs().rowwise().sum().maxCoeff();   // ||N_p||_inf
            const double den   = n_inf * x_float.lpNorm<Eigen::Infinity>()
                               + up.lpNorm<Eigen::Infinity>();
            h.float_repro_rel = (den > 0.0) ? r_inf / den : (r_inf > 0.0 ? kInf : 0.0);
            if (!std::isfinite(h.float_repro_rel)) h.float_repro_rel = kInf;
        }
    }

    const std::size_t nb = blocks.size();
    h.v.resize(nb);
    h.kappa.resize(nb);
    h.chi2.assign(nb, 0.0);
    h.chi2_expect.assign(nb, 0.0);
    h.degenerate.assign(nb, 0);
    h.dependent.assign(nb, 0);
    if (nb == 0) return h;   // nothing to fold: the float, unchanged

    // (2) The fold operands, from the kernel's cached per-station factors (read only).
    const NetworkNormalEquations::DetachedFold fold = neq_.foldCycleRowsDetached(blocks);
    int n_folded = 0;
    for (std::size_t b = 0; b < nb; ++b) {
        const NetworkNormalEquations::DetachedFoldBlock& fb = fold.blocks[b];
        h.degenerate[b] = fb.degenerate ? 1 : 0;
        h.dependent[b]  = fb.dependent ? 1 : 0;
        h.factor_nnz += fb.factor_nnz;
        // v = z_free - G^T x_float - zhat: the float cycle value minus its target.
        h.v[b] = fb.z_free - fb.G.transpose() * x_float - fb.zhat;
        if (fb.factor) ++n_folded;
        if (fb.degenerate) h.chi2[b] = h.chi2_expect[b] = kNaN;
    }
    if (n_folded == 0) return h;

    // One held solve: fold the increments onto a copy, pin it with the same datum, and
    // solve once; the held covariance with it, the pinned columns exact (zero), as the
    // statistics' means read it.
    auto heldSolve = [&](const Eigen::MatrixXd& dN, const Eigen::VectorXd& du,
                         Eigen::MatrixXd& cov) {
        Eigen::MatrixXd N = N0 + dN;
        Eigen::VectorXd u = u0 + du;
        applyDatumPins(N, u, datum);
        Eigen::VectorXd x = neq_.solveSharedConstrained(N, u, &cov);
        for (const Pin& p : datum) {
            cov.row(p.column).setZero();
            cov.col(p.column).setZero();
        }
        return x;
    };
    // A block's kappa against a held estimate: A^-1 (z_free - G^T x - zhat).
    auto kappaAt = [](const NetworkNormalEquations::DetachedFoldBlock& fb,
                      const Eigen::VectorXd& x) {
        return NetworkNormalEquations::solveFoldBlock(fb, fb.z_free - fb.G.transpose() * x - fb.zhat);
    };
    // A block's share of tr(S^-1 P^z) under the held covariance cov:
    // sum_k pz_k (S^-1)_kk with (S^-1)_kk = (A^-1)_kk - x_k' cov x_k, x_k the k-th row of
    // X = A^-1 G^T (the Woodbury form of S = A + G' Sigma_float G; A is block diagonal over
    // the blocks, so the rows of the joint X are the blocks' rows).
    auto holdTrace = [&](std::size_t b, const Eigen::MatrixXd& cov) {
        const NetworkNormalEquations::DetachedFoldBlock& fb = fold.blocks[b];
        const Eigen::VectorXd q = (fb.X * cov).cwiseProduct(fb.X).rowwise().sum();
        double tr = 0.0;
        for (Eigen::Index k = 0; k < q.size(); ++k)
            tr += blocks[b][static_cast<std::size_t>(k)].pz_m2 * (fb.ainv_diag(k) - q(k));
        return tr;
    };

    // (3) Every block folded together: the held estimate and its covariance.
    Eigen::MatrixXd cov_held;
    const Eigen::VectorXd x_held = heldSolve(fold.dN, fold.du, cov_held);

    // (4) kappa = S^-1 v over every folded row, S = Var(float cycles) + P^z, so v^T kappa
    // is the joint chi-square statistic, of mean K - tr(S^-1 P^z) under right targets.  A
    // block's own statistic is v_b^T kappa_b only when no other block folds (the float
    // cycles of two blocks are correlated through the shared border); otherwise it comes
    // from a held solve of that block alone, and so does its mean.
    for (std::size_t b = 0; b < nb; ++b) {
        const NetworkNormalEquations::DetachedFoldBlock& fb = fold.blocks[b];
        if (!fb.factor) continue;
        const double K_b = static_cast<double>(h.v[b].size());
        h.kappa[b] = kappaAt(fb, x_held);
        const double share = h.v[b].dot(h.kappa[b]);
        const double share_tr = holdTrace(b, cov_held);
        h.chi2_joint        += share;
        h.chi2_joint_expect += K_b - share_tr;
        h.dof_joint         += static_cast<long long>(h.v[b].size());
        if (n_folded == 1) {
            h.chi2[b]        = share;
            h.chi2_expect[b] = K_b - share_tr;
        } else {
            Eigen::MatrixXd cov_b;
            const Eigen::VectorXd x_b = heldSolve(fb.dN, fb.du, cov_b);
            h.chi2[b]        = h.v[b].dot(kappaAt(fb, x_b));
            h.chi2_expect[b] = K_b - holdTrace(b, cov_b);
        }
    }

    // (5) Acceptance: a finite held estimate on a reproduced float.  Only then is x_s the
    // held estimate; otherwise it stays x_float, bitwise.
    // (The checks are explicit: Eigen's maxCoeff may skip a NaN.)
    const bool finite = x_held.allFinite();
    if (numShared() > 0) {
        h.max_shift_m = (finite && x_float.allFinite())
                      ? (x_held - x_float).lpNorm<Eigen::Infinity>() : kInf;
    }
    h.ok = finite && (h.float_repro_rel <= kHeldFloatReproTol);
    if (h.ok) h.x_s = x_held;
    return h;
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
    CovTimer _t(neq_);  // timed as a covariance consumer (D0)
    return neq_.reconstructStation(station, x_s);
}

void NetworkEstimator::zeroPhaseGaugeColumns(Eigen::MatrixXd& cov) const {
    // Each folded ensemble sum-to-zero gauge fixes a satellite phase-bias shared
    // column after the reduce (Fix A).  That column is the gauge direction, so its
    // slot of shared_cov_ carries the fold's artificial variance and cross terms.
    // It is treated as an exact datum column (row and column zeroed), like the
    // datum-pin zeroing.  Empty on the anchor / off path: byte-identical no-op.
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
    CovTimer _t(neq_);  // timed as a covariance consumer, shared_cov_ copy included (D0)
    // Cov_free = shared_cov_ with the datum-pinned columns treated as exact
    // (variance 0), not as carrying the Dirichlet pin's artificial 1/pin_scale.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    // The ensemble phase-bias gauges are exact datum columns too (Fix A).
    zeroPhaseGaugeColumns(cov_free);
    return neq_.reconstructStationCovariance(station, cov_free, carry_aware);
}

Eigen::MatrixXd NetworkEstimator::datumFrameSharedCov(const Datum& datum) const {
    CovTimer _t(neq_);  // timed: the n_shared² shared_cov_ copy and gauge zeroing (D0)
    // Cov_free = shared_cov_ with the datum-pinned columns treated as exact (variance
    // 0), as in conditionalLocalCovariance, so a carried-DD readout uses the
    // datum-as-known covariance (the DD is gauge-free, hence datum-invariant).  Under
    // carry_invariant the ensemble phase gauge is off (mutually exclusive with the DD
    // priors), so zeroPhaseGaugeColumns is a no-op there; it is kept for symmetry
    // with the other consumers.
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
    CovTimer _t(neq_);  // timed as a covariance consumer, shared_cov_ copy included (D0)
    // Cov_free = shared_cov_ with the datum-pinned columns treated as exact
    // (variance 0), as in conditionalLocalCovariance, so the between-station DD
    // covariance is differenced in the datum-as-known frame.
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

std::vector<Eigen::MatrixXd> NetworkEstimator::blockConditionalCovariances(
    const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
    const Datum& datum, const std::vector<std::vector<int>>& blocks) const {
    CovTimer _t(neq_);
    // The same datum-as-known frame as jointConditionalCovariance.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    zeroPhaseGaugeColumns(cov_free);
    return neq_.blockConditionalCovariances(requests, cov_free, blocks);
}

Eigen::VectorXd NetworkEstimator::conditionalLocalVarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols,
    const Datum& datum, bool carry_aware) const {
    CovTimer _t(neq_);  // timed as a covariance consumer, shared_cov_ copy included (D0)
    // Same datum-as-known frame as conditionalLocalCovariance: the pinned shared
    // columns are exact (variance 0) and do not carry the Dirichlet pin's 1/pin_scale.
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    // The ensemble phase-bias gauges are exact datum columns too (Fix A).  Without
    // this, the folded gauge-direction variance inflates the carried-ambiguity
    // conditional variance; the phase-leveling carry then goes inert, the ensemble
    // reference churns, and the solution runs away (see zeroPhaseGaugeColumns).
    zeroPhaseGaugeColumns(cov_free);
    return neq_.reconstructStationCovarianceDiag(station, local_cols, cov_free,
                                                carry_aware);
}

Eigen::VectorXd NetworkEstimator::pureConditionalLocalVarianceDiag(
    std::size_t station, const std::vector<Eigen::Index>& local_cols) const {
    CovTimer _t(neq_);  // timed as a covariance consumer (D0)
    // Border-exact: the entire shared border is treated as known (Σ_ss ≡ 0), so
    // this is diag(N_rr⁻¹) restricted to local_cols, i.e. the local column's
    // uncertainty relative to the shared frame, with no border variance
    // propagated in.  The anchor-handover ledger re-validation
    // (SPEC_pivot_visibility_gauge_defect §5.09.1) needs this form: the shared
    // phase-bias column's own uncertainty moves the pin target and the realized
    // frame together, so it is common-mode in the handover step, and a gate on the
    // border-inclusive marginal measures the wrong quantity in both directions (it
    // refuses settled successors whose family bias is loose, and it can pass a
    // target that lands a large spike; the spec records the measured cases, and
    // §5.0 has the spike case).
    const Eigen::MatrixXd cov_zero =
        Eigen::MatrixXd::Zero(shared_cov_.rows(), shared_cov_.cols());
    return neq_.reconstructStationCovarianceDiag(station, local_cols, cov_zero,
                                                 /*carry_aware=*/false);
}

NetworkNormalEquations::StationRegression NetworkEstimator::stationRegression(
    std::size_t station, Eigen::Index local_col, bool carry_aware,
    const std::vector<Eigen::Index>* cross_cols) const {
    CovTimer _t(neq_);  // timed as a covariance consumer (D0)
    // Pure normals with no datum frame (M_r and (N_rr⁻¹)_{rr} are datum-independent).
    // The carry-aware form is datum-independent too: Ã⁻¹ = N_rr⁻¹ − Σ_b Y_b M_b Y_bᵀ is
    // built from the accumulated normals and the fold's own factors, not from Σ_ss.
    return neq_.stationRegression(station, local_col, carry_aware, cross_cols);
}

Eigen::VectorXd NetworkEstimator::rowRedundancy(
    std::size_t station, const Eigen::MatrixXd& A_s_rows,
    const Eigen::MatrixXd& A_r_rows, const Eigen::VectorXd& w_rows,
    const Datum& datum) const {
    CovTimer _t(neq_);  // timed as a covariance consumer, shared_cov_ copy included (D0)
    // Datum-as-known frame: the pinned shared columns are exact gauges (variance 0).
    Eigen::MatrixXd cov_free = shared_cov_;
    for (const auto& pin : datum) {
        if (pin.column >= 0 && pin.column < cov_free.rows()) {
            cov_free.row(pin.column).setZero();
            cov_free.col(pin.column).setZero();
        }
    }
    // The ensemble phase-bias gauges are exact datum columns too (Fix A).
    zeroPhaseGaugeColumns(cov_free);
    return neq_.rowRedundancy(station, A_s_rows, A_r_rows, w_rows, cov_free);
}

const Eigen::MatrixXd& NetworkEstimator::reducedSharedNormals() {
    neq_.reduce();
    // Fold the anchor-free phase-bias gauges before the null-space gate inspects
    // the reduced normals, so the phase gauge reads as fixed (Fix A).  The gate's
    // declared dimension then stays equal to the clock deficiency count, as in the
    // anchor mode, whose local pins were pre-eliminated.  Idempotent with the
    // solve()-side fold.
    if (!phase_sumzero_gauges_.empty()) {
        neq_.foldReducedSumZeroGauges(phase_sumzero_gauges_);
    }
    return neq_.reducedSharedNormals();
}

void NetworkEstimator::addSharedPrior(const std::vector<SharedPrior>& priors) {
    const ContributionKind ck = takeContributionKind_();
    const std::vector<ContributionKind>* prior_kinds = contrib_prior_kinds_;
    contrib_prior_kinds_ = nullptr;
    if (priors.empty()) return;
    const bool per_prior_kinds = prior_kinds && prior_kinds->size() == priors.size();
    // Card NOM step N2b: with a kind per prior and the capture armed, each kind's part of
    // the RHS increment is accumulated beside it (the fold itself is unchanged).
    const bool split = attrib_armed_ && per_prior_kinds;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    const std::vector<ContributionKind> korder =
        split ? kindsInOrder_(*prior_kinds) : std::vector<ContributionKind>{};
    std::vector<Eigen::VectorXd> kpart(korder.size(), Eigen::VectorXd::Zero(n));
    for (std::size_t j = 0; j < priors.size(); ++j) {
        const SharedPrior& p = priors[j];
        if (!(p.info > 0.0)) continue;
        const std::string pkey = priorKey_(p);   // VtecSh, a clock clone (card SCL), or key_
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        delta_info(c, c) += p.info;          // diagonal info prior (D2 carry-forward)
        delta_rhs(c)     += p.info * p.value;
        if (split) {
            const auto ki = std::find(korder.begin(), korder.end(), (*prior_kinds)[j]);
            kpart[static_cast<std::size_t>(ki - korder.begin())](c) += p.info * p.value;
        }
    }
    if (split) {
        for (std::size_t k = 0; k < korder.size(); ++k) noteCapture_(korder[k], kpart[k]);
    } else {
        noteCapture_(ck, delta_rhs);
    }
    neq_.addSharedInfo(delta_info, delta_rhs);
    if (per_prior_kinds) noteFoldsPerKind_(*prior_kinds);
    else                 noteFold_(ck, static_cast<long long>(priors.size()));
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
    // Map identity keys onto this epoch's layout.  An absent key is marginalized:
    // its rows and columns of the prior covariance are dropped before the
    // inversion.  (Dropping rows of Λ would instead condition on the departed key,
    // giving wrong statistics for every survivor; see the header contract.)  The
    // Card Q3 refloat marginalizes on the same footing via `skip`.
    in_idx.reserve(keys.size());
    col.reserve(keys.size());
    for (Eigen::Index i = 0; i < nk; ++i) {
        const SharedPrior& p = keys[static_cast<std::size_t>(i)];
        // Q3 refloat.  A clock clone of the same satellite is kept (card SCL): the rows
        // still reference it, and the refloat frees the clock, not its previous value.
        if (p.kind == SharedParamKind::Clock && !p.clone && skip.count(p.sat)) continue;
        const std::string pkey = priorKey_(p);   // VtecSh, a clock clone (card SCL), or key_
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
    const double kappa = dense_fold_kappa_;   // network.dense_fold_kappa (default 1e-6)
    const auto pdOk = [kappa](const Eigen::LDLT<Eigen::MatrixXd>& f) {
        if (f.info() != Eigen::Success) return false;
        const Eigen::VectorXd dv = f.vectorD();
        const double dmax = dv.maxCoeff();
        const double dmin = dv.minCoeff();
        // κ = 1e-6, much tighter than the kernel's 1e-12 solve guard on purpose:
        // this factor is inverted and folded as information, so an admitted
        // condition number becomes information amplification on the shared
        // normals (Λ_aa = 1/(P_aa·(1−R²_a))).  Without a bound, a near-gauge
        // carried direction (multiple correlation R² → 1) can inflate the normals'
        // max diagonal so far that the constrained solve's own relative-pivot guard
        // (1e-12·dmax) rises above real weakly-observed columns and throws.  A
        // solve throw skips the epoch before the stash, so the identical block
        // would re-fold every epoch (a persistent skip).  Rejecting here routes the
        // block to the driver's diagonal fallback instead (bounded at the legacy
        // 1/var_eff), which stashes a fresh block at the next emit, so the
        // condition clears without intervention.
        return dmin > 0.0 && dmin >= kappa * dmax;
    };
    Eigen::LDLT<Eigen::MatrixXd> ldlt(C);
    if (!pdOk(ldlt)) {
        // One ridge retry: +1e-9 on the unit diagonal inflates every prior variance
        // by about one part in 1e9 of its value.  That is statistically negligible,
        // and it is numerically decisive when a correlation approaches ±1
        // (near-gauge posterior directions).
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
    // the within-epoch RAIM replay re-folds this block before the reduce, and the
    // two paths must agree bit-for-bit.
    Lambda = (0.5 * (Lambda + Lambda.transpose())).eval();
    // Marginalize the per-constellation satellite-clock gauge direction out of the
    // joint information (Card Q3 §2.1; see addSharedPriorClockGaugeFree for the
    // algebra).  This is applied on Λ rather than on the covariance, because
    // "infinite prior variance along u" has no finite covariance representation;
    // the downdate is its exact information-side image.  Applying it to the joint
    // block keeps the clock↔code-bias off-diagonals that the C1 dense carry exists
    // to retain; only the gauge direction is removed.  It runs before the
    // information-budget check below, so that check sees the information actually
    // folded.
    if (clock_gauge_free) {
        std::map<char, std::vector<Eigen::Index>> grp;   // constellation letter -> rows of Λ
        for (Eigen::Index a = 0; a < k; ++a) {
            const SharedPrior& p = keys[static_cast<std::size_t>(in_idx[static_cast<std::size_t>(a)])];
            if (p.kind == SharedParamKind::Clock && !p.clone && !p.sat.empty())   // a clone is not a clock of the gauge (card SCL)
                grp[letterOfSat(p.sat)].push_back(a);
        }
        std::vector<Eigen::Index> used;
        for (const auto& [c, rows] : grp) {
            (void)c;
            if (rows.size() < 2) continue;   // a lone carried clock is the gauge; nothing estimable to anchor
            used.insert(used.end(), rows.begin(), rows.end());
        }
        if (!used.empty()) {
            // One column of U per constellation with >= 2 carried clocks.  It is
            // built as a matrix rather than as looped rank-1 downdates because the
            // groups are coupled through Λ (the carried posterior correlates
            // constellations): sequential downdates would not commute, and the
            // second would be taken against an already-modified Λ.
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
            // the gauge (nothing to remove).  Leaving Λ untouched is then the correct
            // and only safe action; the caller's counter reads 0 for the fold.
        }
    }
    // Absolute folded-information budget, the second half of the conditioning
    // guard: the κ check above bounds the relative amplification, and this bounds
    // the absolute injected stiffness, which is what the downstream solve guard
    // sees.  1e13 m⁻² keeps the constrained solve's relative-pivot floor
    // (1e-12·dmax) at or below ~10 m⁻², under the weakest real reduced pivots (a
    // single-station low-elevation riser is O(1–100)).  Only a near-singular
    // carried block combined with an extreme zero-q variance (~1e-9 m²) can exceed
    // it; with the B4 carry floors armed (the recommended C1 operating point) it is
    // unreachable.  For a PSD matrix the max diagonal bounds max |Λ_ij|.
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
    const ContributionKind ck = takeContributionKind_();
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
    noteCapture_(ck, delta_rhs);
    neq_.addSharedInfo(delta_info, delta_rhs);
    noteFold_(ck, static_cast<long long>(nk));
    return static_cast<std::size_t>(k);
}

std::size_t NetworkEstimator::addCloneWalk(const std::vector<CloneWalk>& walks, double sign) {
    const ContributionKind ck = takeContributionKind_();
    if (walks.empty()) return 0;
    // Like withdrawSharedPrior, the call may follow solve(): card Q3's refloat withdraws a
    // factor and, when its re-solve throws, folds it back before restoring the solve.
    if (!epoch_open_) {
        throw std::logic_error("NetworkEstimator::addCloneWalk: call beginEpoch() first");
    }
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    std::size_t applied = 0;
    for (const CloneWalk& w : walks) {
        if (!(w.q > 0.0) || !std::isfinite(w.q) || !std::isfinite(w.mean)) continue;
        const auto ic = index_.find(key_(w.sat, w.kind, w.signal));   // card BCL: a bias walk too
        const auto ik = index_.find(cloneKey_(w.kind, w.sat, w.signal));
        if (ic == index_.end() || ik == index_.end()) continue;
        const Eigen::Index c = ic->second, k = ik->second;
        const double info = sign / w.q;
        if (!w.ref.empty()) {
            // Card SCL B2: c - k + r = mean across a dense-transformed handover,
            // a = e_c - e_k + e_r, information a a'/q.
            const auto ir = index_.find(cloneKey_(SharedParamKind::Clock, w.ref, ""));
            if (ir == index_.end() || ir->second == k) continue;
            const Eigen::Index idx[3] = {c, k, ir->second};
            const double coef[3] = {1.0, -1.0, 1.0};
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) delta_info(idx[a], idx[b]) += info * coef[a] * coef[b];
                delta_rhs(idx[a]) += info * coef[a] * w.mean;
            }
            ++applied;
            continue;
        }
        // The pseudo-observation c - k = mean with weight 1/q: (e_c - e_k)(e_c - e_k)'/q.
        delta_info(c, c) += info;
        delta_info(k, k) += info;
        delta_info(c, k) -= info;
        delta_info(k, c) -= info;
        delta_rhs(c) += info * w.mean;
        delta_rhs(k) -= info * w.mean;
        ++applied;
    }
    if (applied == 0) return 0;
    noteCapture_(ck, delta_rhs);
    neq_.addSharedInfo(delta_info, delta_rhs);
    noteFold_(ck, static_cast<long long>(walks.size()));
    return applied;
}

std::size_t NetworkEstimator::addCloneWalkJoint(const std::vector<CloneWalk>& walks,
                                                const Eigen::MatrixXd& Qw, double sign) {
    if (Qw.size() == 0) return addCloneWalk(walks, sign);   // takes the kind itself
    const ContributionKind ck = takeContributionKind_();
    if (walks.empty()) return 0;
    if (!epoch_open_) {
        throw std::logic_error("NetworkEstimator::addCloneWalkJoint: call beginEpoch() first");
    }
    const Eigen::Index m = static_cast<Eigen::Index>(walks.size());
    if (Qw.rows() != m || Qw.cols() != m) {
        throw std::runtime_error("NetworkEstimator::addCloneWalkJoint: Qw is not walks x walks");
    }
    // The rows a_i of the walks that apply (addCloneWalk's skip rules), as sparse index lists.
    struct Row { Eigen::Index w; std::vector<std::pair<Eigen::Index, double>> t; };
    std::vector<Row> rows;
    rows.reserve(walks.size());
    for (Eigen::Index i = 0; i < m; ++i) {
        const CloneWalk& w = walks[static_cast<std::size_t>(i)];
        if (!std::isfinite(w.mean)) continue;
        const auto ic = index_.find(key_(w.sat, w.kind, w.signal));   // card BCL: a bias walk too
        const auto ik = index_.find(cloneKey_(w.kind, w.sat, w.signal));
        if (ic == index_.end() || ik == index_.end()) continue;
        Row r;
        r.w = i;
        r.t = {{ic->second, 1.0}, {ik->second, -1.0}};
        if (!w.ref.empty()) {
            const auto ir = index_.find(cloneKey_(SharedParamKind::Clock, w.ref, ""));
            if (ir == index_.end() || ir->second == ik->second) continue;
            r.t.push_back({ir->second, 1.0});
        }
        rows.push_back(std::move(r));
    }
    if (rows.empty()) return 0;
    const Eigen::Index k = static_cast<Eigen::Index>(rows.size());
    Eigen::MatrixXd Qk(k, k);
    Eigen::VectorXd mean(k);
    for (Eigen::Index a = 0; a < k; ++a) {
        mean(a) = walks[static_cast<std::size_t>(rows[static_cast<std::size_t>(a)].w)].mean;
        for (Eigen::Index b = 0; b < k; ++b)
            Qk(a, b) = 0.5 * (Qw(rows[static_cast<std::size_t>(a)].w, rows[static_cast<std::size_t>(b)].w)
                            + Qw(rows[static_cast<std::size_t>(b)].w, rows[static_cast<std::size_t>(a)].w));
    }
    // Correlation scaling before the factor: the walks' variances span decades.
    Eigen::VectorXd d(k);
    for (Eigen::Index a = 0; a < k; ++a) {
        if (!(Qk(a, a) > 0.0) || !std::isfinite(Qk(a, a)))
            throw std::runtime_error("NetworkEstimator::addCloneWalkJoint: non-positive walk variance");
        d(a) = 1.0 / std::sqrt(Qk(a, a));
    }
    const Eigen::MatrixXd C = d.asDiagonal() * Qk * d.asDiagonal();
    const Eigen::LDLT<Eigen::MatrixXd> f(C);
    if (f.info() != Eigen::Success || !f.isPositive()
        || !(f.vectorD().minCoeff() > 1e-12 * f.vectorD().cwiseAbs().maxCoeff()))
        throw std::runtime_error("NetworkEstimator::addCloneWalkJoint: Qw is not positive definite");
    // W = Qk^-1 = D C^-1 D; information A'WA and right-hand side A'W mean, through the rows.
    const Eigen::MatrixXd W = d.asDiagonal() * f.solve(Eigen::MatrixXd::Identity(k, k)) * d.asDiagonal();
    const Eigen::VectorXd Wm = W * mean;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    for (Eigen::Index a = 0; a < k; ++a) {
        for (const auto& [ca, va] : rows[static_cast<std::size_t>(a)].t) {
            delta_rhs(ca) += sign * va * Wm(a);
            for (Eigen::Index b = 0; b < k; ++b)
                for (const auto& [cb, vb] : rows[static_cast<std::size_t>(b)].t)
                    delta_info(ca, cb) += sign * va * W(a, b) * vb;
        }
    }
    delta_info = (0.5 * (delta_info + delta_info.transpose())).eval();
    noteCapture_(ck, delta_rhs);
    neq_.addSharedInfo(delta_info, delta_rhs);
    noteFold_(ck, static_cast<long long>(walks.size()));
    return static_cast<std::size_t>(k);
}

bool NetworkEstimator::cloneConditional(const Datum& datum, Eigen::MatrixXd& K,
                                        Eigen::MatrixXd& Bc) const {
    if (n_clones_ + n_hist_ == 0 || !solved_) return false;   // card SCL2 X2: Y = clones and history
    const Eigen::MatrixXd& J = neq_.reducedSharedNormals();
    const Eigen::Index n = numShared();
    if (J.rows() != n || J.cols() != n) return false;
    // A datum-pinned column is exactly known in the solve's frame (the Dirichlet pin), so
    // the conditional mean of the clones does not move with it: no coefficient.
    std::vector<Eigen::Index> cc, ss;
    std::vector<char> pinned(static_cast<std::size_t>(n), 0);
    for (const Pin& p : datum)
        if (p.column >= 0 && p.column < n) pinned[static_cast<std::size_t>(p.column)] = 1;
    for (Eigen::Index c = 0; c < n; ++c) {
        if (columns_[static_cast<std::size_t>(c)].clone
            || columns_[static_cast<std::size_t>(c)].hist) cc.push_back(c);   // card SCL2 X2: the history joins Y
        else if (!pinned[static_cast<std::size_t>(c)]) ss.push_back(c);
    }
    const Eigen::Index nc = static_cast<Eigen::Index>(cc.size());
    if (nc == 0) return false;
    Eigen::MatrixXd Jcc(nc, nc);
    for (Eigen::Index a = 0; a < nc; ++a)
        for (Eigen::Index b = 0; b < nc; ++b)
            Jcc(a, b) = 0.5 * (J(cc[a], cc[b]) + J(cc[b], cc[a]));
    // Correlation scaling, as the dense fold inverts its block: a clone tied to its clock
    // by the walk factor and to tens of rows can carry information far above another
    // clone's.
    Eigen::VectorXd dinv(nc);
    for (Eigen::Index a = 0; a < nc; ++a) {
        if (!(Jcc(a, a) > 0.0) || !std::isfinite(Jcc(a, a))) return false;
        dinv(a) = 1.0 / std::sqrt(Jcc(a, a));
    }
    const Eigen::MatrixXd Cn = dinv.asDiagonal() * Jcc * dinv.asDiagonal();
    const Eigen::LDLT<Eigen::MatrixXd> f(Cn);
    if (f.info() != Eigen::Success || !f.isPositive()) return false;
    const Eigen::VectorXd dv = f.vectorD();
    if (!(dv.minCoeff() > 1e-14 * dv.cwiseAbs().maxCoeff())) return false;
    Eigen::MatrixXd B = dinv.asDiagonal() * f.solve(Eigen::MatrixXd::Identity(nc, nc))
                        * dinv.asDiagonal();
    B = (0.5 * (B + B.transpose())).eval();
    Eigen::MatrixXd Jcs = Eigen::MatrixXd::Zero(nc, n);
    for (Eigen::Index a = 0; a < nc; ++a)
        for (const Eigen::Index s : ss) Jcs(a, s) = J(cc[a], s);
    Eigen::MatrixXd Kout = -B * Jcs;
    if (!B.allFinite() || !Kout.allFinite()) return false;
    K  = std::move(Kout);
    Bc = std::move(B);
    return true;
}

std::size_t NetworkEstimator::addSharedPriorClockGaugeFree(
    const std::vector<SharedPrior>& priors) {
    const ContributionKind ck = takeContributionKind_();
    const std::vector<ContributionKind>* prior_kinds = contrib_prior_kinds_;
    contrib_prior_kinds_ = nullptr;
    if (priors.empty()) return 0;
    const bool per_prior_kinds = prior_kinds && prior_kinds->size() == priors.size();
    // Card NOM step N2b: with a kind per prior and the capture armed, each kind's part of
    // the RHS increment is accumulated beside it (the fold itself is unchanged).  A
    // downdated group's increment is Λ_gf v, so prior b's part is column b of Λ_gf times
    // its value v_b, which lands on every column of its group.
    const bool split = attrib_armed_ && per_prior_kinds;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    const std::vector<ContributionKind> korder =
        split ? kindsInOrder_(*prior_kinds) : std::vector<ContributionKind>{};
    std::vector<Eigen::VectorXd> kpart(korder.size(), Eigen::VectorXd::Zero(n));
    const auto partOf = [&](std::size_t j) -> Eigen::VectorXd& {
        const auto ki = std::find(korder.begin(), korder.end(), (*prior_kinds)[j]);
        return kpart[static_cast<std::size_t>(ki - korder.begin())];
    };

    // Pass 1: resolve identities.  Non-clock priors accumulate exactly as
    // addSharedPrior accumulates them (same order, same arithmetic); clock
    // priors are grouped by constellation for the downdate below.
    struct ClkRow { Eigen::Index col; double info, value; std::size_t j; };   // j: the prior's index in priors
    std::map<char, std::vector<ClkRow>> grp;
    for (std::size_t j = 0; j < priors.size(); ++j) {
        const SharedPrior& p = priors[j];
        if (!(p.info > 0.0)) continue;
        const std::string pkey = priorKey_(p);   // VtecSh, a clock clone (card SCL), or key_
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        if (p.kind == SharedParamKind::Clock && !p.clone && !p.sat.empty()) {   // a clone folds plainly (card SCL)
            grp[letterOfSat(p.sat)].push_back({c, p.info, p.value, j});
        } else {
            delta_info(c, c) += p.info;
            delta_rhs(c)     += p.info * p.value;
            if (split) partOf(j)(c) += p.info * p.value;
        }
    }

    // Pass 2: per constellation, Λ_gf = D − λλᵀ/Λ_tot with D = diag(λ), λ = info.
    // (The general Λ − Λu(uᵀΛu)⁻¹uᵀΛ specialized to a diagonal Λ and u = 1.)
    // η = Λ_gf v = λ ⊙ (v − v̄_λ): each clock is anchored to its carried value
    // minus the information-weighted mean of the carried values, i.e. the
    // differences are anchored and the gauge is left to the datum.
    std::size_t downdated = 0;
    for (const auto& [c, rows] : grp) {
        (void)c;
        if (rows.size() < 2) continue;   // a lone carried clock is the gauge
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
                if (split) partOf(rows[b].j)(rows[a].col) += v * rows[b].value;
            }
        }
        downdated += rows.size();
    }
    if (split) {
        for (std::size_t k = 0; k < korder.size(); ++k) noteCapture_(korder[k], kpart[k]);
    } else {
        noteCapture_(ck, delta_rhs);
    }
    neq_.addSharedInfo(delta_info, delta_rhs);
    if (per_prior_kinds) noteFoldsPerKind_(*prior_kinds);
    else                 noteFold_(ck, static_cast<long long>(priors.size()));
    return downdated;
}

std::size_t NetworkEstimator::refoldSharedPriorClockGaugeFreeDropping(
    const std::vector<SharedPrior>& priors, const std::set<std::string>& drop_sats,
    bool restore) {
    const ContributionKind ck = takeContributionKind_();
    if (priors.empty() || drop_sats.empty()) return 0;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);

    struct ClkRow { Eigen::Index col; double info, value; bool drop; };
    std::map<char, std::vector<ClkRow>> grp;
    for (const SharedPrior& p : priors) {
        if (p.kind != SharedParamKind::Clock || p.clone || p.sat.empty() || !(p.info > 0.0)) continue;
        const auto it = index_.find(key_(p.sat, p.kind, p.signal));
        if (it == index_.end()) continue;
        grp[letterOfSat(p.sat)].push_back({it->second, p.info, p.value, drop_sats.count(p.sat) > 0});
    }
    // The gauge-free block is not diagonal, so a per-key subtraction (the plain
    // withdrawSharedPrior) would leave the rest of the group folded against a
    // stale weighted mean.  Both blocks are rebuilt and the difference is folded,
    // the exact analogue of the dense path's marginalized re-fold.
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
    noteCapture_(ck, delta_rhs);
    neq_.addSharedInfo(delta_info, delta_rhs);
    noteFold_(ck, static_cast<long long>(priors.size()));
    return dropped;
}

void NetworkEstimator::withdrawSharedPrior(const std::vector<SharedPrior>& priors) {
    const ContributionKind ck = takeContributionKind_();
    if (priors.empty()) return;
    const Eigen::Index n = numShared();
    Eigen::MatrixXd delta_info = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd delta_rhs  = Eigen::VectorXd::Zero(n);
    // Mirror addSharedPrior's accumulation exactly, negated: the withdrawal must
    // be the arithmetic inverse of the fold, not an independent recomputation.
    for (const SharedPrior& p : priors) {
        if (!(p.info > 0.0)) continue;
        const std::string pkey = priorKey_(p);   // VtecSh, a clock clone (card SCL), or key_
        const auto it = index_.find(pkey);
        if (it == index_.end()) continue;   // parameter not in this epoch's layout
        const Eigen::Index c = it->second;
        delta_info(c, c) -= p.info;
        delta_rhs(c)     -= p.info * p.value;
    }
    noteCapture_(ck, delta_rhs);
    neq_.addSharedInfo(delta_info, delta_rhs);
    noteFold_(ck, static_cast<long long>(priors.size()));
}

std::size_t NetworkEstimator::refoldSharedPriorDenseDroppingClocks(
    const std::vector<SharedPrior>& keys, const Eigen::MatrixXd& cov,
    const std::set<std::string>& drop_sats, bool restore, bool clock_gauge_free) {
    const ContributionKind ck = takeContributionKind_();
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
    // Both blocks are built before anything is folded, so a throw from either
    // leaves the shared system untouched (the caller's restore path then keeps the
    // pre-withdraw solution).
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
    noteCapture_(ck, delta_rhs);
    neq_.addSharedInfo(delta_info, delta_rhs);
    noteFold_(ck, static_cast<long long>(nk));
    return dropped;
}

void NetworkEstimator::addSharedConstraintRows(const Eigen::MatrixXd& H,
                                               const Eigen::VectorXd& z,
                                               double weight) {
    const ContributionKind ck = takeContributionKind_();
    const std::vector<ContributionKind>* row_kinds = contrib_row_kinds_;
    contrib_row_kinds_ = nullptr;
    if (H.cols() != numShared() || H.rows() != z.size()) {
        throw std::invalid_argument(
            "NetworkEstimator::addSharedConstraintRows: dimension mismatch");
    }
    if (H.rows() == 0 || !(weight > 0.0)) return;
    const Eigen::MatrixXd delta_info = weight * (H.transpose() * H);
    const Eigen::VectorXd delta_rhs  = weight * (H.transpose() * z);
    const bool per_row_kinds =
        row_kinds && static_cast<Eigen::Index>(row_kinds->size()) == H.rows();
    if (attrib_armed_) {
        if (per_row_kinds) {
            // Card NOM step N2b: one capture per kind present in the rows, in the order a
            // kind first appears, each the rows of that kind (their sum is delta_rhs).
            for (const ContributionKind k : kindsInOrder_(*row_kinds)) {
                Eigen::VectorXd part = Eigen::VectorXd::Zero(H.cols());
                for (Eigen::Index r = 0; r < H.rows(); ++r)
                    if ((*row_kinds)[static_cast<std::size_t>(r)] == k)
                        part.noalias() += (weight * z(r)) * H.row(r).transpose();
                noteCapture_(k, part);
            }
        } else {
            noteCapture_(ck, delta_rhs);
        }
    }
    neq_.addSharedInfo(delta_info, delta_rhs);
    // Card NOM N2a: one record per kind present in the rows (in the order a kind first
    // appears), when the site named a kind per row; else one record under the call's kind.
    if (per_row_kinds) noteFoldsPerKind_(*row_kinds);
    else               noteFold_(ck, static_cast<long long>(H.rows()));
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
        // they are not mapped into the per-satellite NetworkSolutionEpoch here.
        // The driver reads them from x_s via vtecCoeffColumn for the trace; their
        // product serialization (IGS-SSR VTEC) is a later stage.
        if (d.kind == SharedParamKind::VtecSh) continue;
        // A clock clone (card SCL) is the previous epoch's clock carried inside the border
        // for one epoch: never a product, and never the satellite's clock.
        if (d.clone) continue;
        if (d.hist) continue;   // card SCL2 X2: a shared-history column, the same
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
                // Flag the bias as integer-preserving for CDMA constellations
                // (GPS/GAL/BDS/QZSS; RT0-3).  Its uncombined satellite phase bias
                // carries the pivot-receiver ambiguity lump (Odijk Eq. 4) but keeps
                // the integer property that a consumer's AR candidate selection can
                // use.  GLONASS/FDMA ('R') biases include a per-slot inter-frequency
                // bias and are not integer-recoverable, so they stay false; a
                // consumer must not treat an 'R' bias as an integer candidate.  A
                // consumer reads the flag only with
                // network.require_signal_integer_for_ar (default off).  Under the
                // pivot-receiver anchor datum, the prioritized anchor or the Card-3
                // Eq-34 single-receiver-chain datum (network.phase_datum_eq34) the
                // emitted bias carries only integer lumps and keeps the integer
                // property for CDMA satellites.
                npb.signal_integer = !d.sat.empty() && systemOfSat(d.sat) != System::GLONASS;
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
    // Pure publish/withhold policy (RTA-10; see the header for the semantics).  A
    // constellation is publishable when it is clean by its own σ̂0_c under the
    // common ceiling and judgeable (enough rows).  Every input key lands in exactly
    // one output set.
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
    // The machine-readable mirror is filled from the same locals the warn lines
    // format, inside the same loops and in the same (largest-ev-first) order, so a
    // network_estimability.csv row and its log line cannot disagree, and the log
    // text stays byte-identical (the estim golden arm) (DG5).
    EstimabilityDump out;
    const Eigen::MatrixXd& Nss = reducedSharedNormals();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(
        Nss, Eigen::ComputeEigenvectors);
    if (es.info() == Eigen::Success) {
        const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
        const Eigen::Index k = ev.size();
        const double smax = (k > 0) ? ev(k - 1) : 0.0;  // largest = last
        // The resolved tolerance (DG2): the dump describes the decision the gate is
        // about to make, so it reads the same member the gate call site and the
        // drop-and-heal read, never the kNullSpaceTol default directly.
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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the observability rank gate and the datum-direction ratio.
int networkRankGateRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const auto& n_emitted = v.ctx.counters.n_emitted;

    // Rank-gate calibration telemetry (card DG6).  Printed only when the tolerance is set
    // to a non-default value, so a default run's log text is byte-identical (DG5: with
    // the trace on, the new file is the only artifact that differs).  The same numbers
    // reach every trace-armed run through the RUN record of network_estimability.csv, so
    // the control arm still gets them.  An armed calibration whose counters never move is
    // a miswire, not a quiet run (invariant #4).
    if (config_.networkRankGateTol() != NetworkEstimator::kNullSpaceTol)
        log->info("Network (Card DG): rank gate evaluated on {} epoch(s) at tol={:g} — {} "
                  "emitted, {} withheld UNDER-specified (observed > declared: a missing "
                  "datum constraint), {} withheld OVER-specified (observed < declared: a "
                  "declared gauge stopped being counted).  Observed near-null count over "
                  "the run: min {}, max {} (declared dim is the target).",
                  ctx.counters.n_gate_evaluated, config_.networkRankGateTol(),
                  n_emitted, ctx.counters.n_gate_withheld_under,
                  ctx.counters.n_gate_withheld_over,
                  ctx.counters.nullspace_obs_min, ctx.counters.nullspace_obs_max);
    // An over-specified withhold has two causes with opposite fixes.  A tolerance set
    // below the config's own gauge depth is fixed by re-calibrating.  A pre-gate fold that
    // deposits information along a declared gauge direction moves that direction up into
    // the real-parameter layer, and no tolerance recovers it: a cutoff below the filled
    // value still does not count it as gauge, and a cutoff above it starts counting real
    // parameter directions as gauge.  (On simnet24: gauge depth 1e-11, carry-filled
    // direction 9.43e-3, weakest real phase-bias layer 1.25e-2, so no cutoff meets the
    // placement rule's 2 decades on either side.)  The invariant-#7 ratio separates the
    // two cases, so the advice below branches on it.
    if (ctx.counters.n_gate_withheld_over > 0) {
        if (ctx.counters.ddi_over_breach > 0)
            log->warn("Network (Card DG): {} of the {} OVER-specified withhold(s) landed on an "
                      "epoch where a DECLARED datum direction was already carrying information "
                      "(invariant-#7 ratio >= 1; run max R={:.3e} on {} at TOW {:.1f}).  Those "
                      "are NOT a mis-calibrated tolerance and re-calibrating cannot fix them: "
                      "the direction has moved up into the real-parameter layer, so every "
                      "cutoff below it still fails to count it as gauge and every cutoff above "
                      "it starts counting real directions as gauge.  ATTRIBUTE THE FOLD "
                      "instead.  ⚠ The ratio is measured on the reduced normals AS THE GATE "
                      "SEES THEM, i.e. AFTER the station blocks (which carry the Card-1 "
                      "coupling rows), the clock-constraint rows, the SH prior, "
                      "the pin bridge and any SBC rows — but BEFORE the D2/C1 dense carry fold "
                      "and the clock-osc prior, which are applied later and are therefore NOT "
                      "observable in this number.  So the candidates it CAN indict are the "
                      "Card-1 coupling rows (a truncated family sum — arm "
                      "network.carry_coupling_gauge_project) and the pre-gate datum/constraint "
                      "rows.  The ddi_breach events in network_events.csv name the direction and "
                      "the epoch.",
                      ctx.counters.ddi_over_breach, ctx.counters.n_gate_withheld_over,
                      ctx.counters.ddi_ratio_max, ctx.counters.ddi_ratio_max_dir,
                      ctx.counters.ddi_ratio_max_tow);
        if (ctx.counters.n_gate_withheld_over > ctx.counters.ddi_over_breach)
            log->info("Network (Card DG): {} OVER-specified withhold(s) occurred with every "
                      "declared datum direction still free (invariant-#7 ratio < 1).  THOSE are "
                      "the calibration case — the tolerance sits below this config's own gauge "
                      "depth, so RE-CALIBRATE rather than chasing the datum.  [Placement rule: "
                      "cutoff >= 2 decades above the gauge depth and >= 2 decades below the "
                      "weakest-real layer, read off this config's own network_estimability.csv.]",
                      ctx.counters.n_gate_withheld_over - ctx.counters.ddi_over_breach);
    }
    // Invariant #7's own readout, independent of whether the gate ever withheld: a run
    // whose declared directions all sit decades below the cutoff has a genuinely free
    // datum, and that is worth stating positively rather than inferring from silence.
    if (ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs == 0)
        log->warn("Network (invariant #7): the ratio was reached on {} epoch(s) but sampled "
                  "ZERO directions — every declared group was below two registered clocks, or "
                  "its columns fell outside the reduced normals.  The instrument is INERT: "
                  "'0 breaches' says nothing about this run's datum.",
                  ctx.counters.ddi_epochs);
    if (ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs > 0)
        log->info("Network (invariant #7): datum-direction information ratio measured on {} "
                  "epoch(s), {} (epoch x direction) samples — R range [{:.3e}, {:.3e}], max on "
                  "{} at TOW {:.1f}; {} epoch(s) breached R >= 1 across {} edge(s).  R << 1 "
                  "means the datum still owns its gauge directions; R >= 1 means something "
                  "folded PRE-gate is pinning one.",
                  ctx.counters.ddi_epochs, ctx.counters.ddi_dirs,
                  ctx.counters.ddi_ratio_min, ctx.counters.ddi_ratio_max,
                  ctx.counters.ddi_ratio_max_dir.empty() ? "(none)"
                                                         : ctx.counters.ddi_ratio_max_dir,
                  ctx.counters.ddi_ratio_max_tow,
                  ctx.counters.ddi_breach_epochs, ctx.counters.ddi_breach_edges);
    return 0;
}

// Run-end report: the carry-aware covariance mode.
int networkCarryAwareCovarianceRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkEstimator& est = v.run.engines.est;

    // --- Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md §4) -----
    // Printed on every carry_invariant run, armed or not: a σ published without its
    // covariance mode cannot be read, and the run that most needs to be told its σ is
    // carry-blind is the one that did not arm network.carry_aware_covariance.
    if (!ctx.knobs.carry_invariant) return 0;
    const auto& cc = est.carryCovStats();
    if (ctx.knobs.carry_aware_covariance) {
        const double mean_ratio = (cc.ratio_n > 0)
            ? cc.ratio_sum / static_cast<double>(cc.ratio_n) : 1.0;
        log->info("Network (carry-aware covariance): ARMED — {} readback call(s) over "
                  "{} requested (station,column) entr(ies); {} corrected, {} REFUSED "
                  "for a non-positive corrected variance ({} matrix call(s) fell back "
                  "wholesale).  Variance ratio corrected/blind: min {:.6g}, mean "
                  "{:.6g}, max {:.6g}.  dd_sigma_cyc and the AR probe's Q_cyc are the "
                  "estimator's own uncertainty on this run.",
                  cc.calls, cc.rows, cc.corrected_rows, cc.nonpos_rows,
                  cc.refused_calls, cc.ratio_min, mean_ratio, cc.ratio_max);
        if (cc.calls == 0)
            log->warn("Network (carry-aware covariance): ARMED but ZERO readback calls "
                      "reached the correction — no consumer asked, or every epoch was "
                      "chordless.  The wiring never executed (invariant #4); nothing "
                      "in this run is evidence about the card.");
        else if (cc.ratio_n > 0 && cc.ratio_min >= 1.0)
            log->warn("Network (carry-aware covariance): ARMED and the correction moved "
                      "NOTHING (min ratio {:.6g} >= 1).  The carry can only ADD "
                      "information, so a readback it never tightens means the rank-K "
                      "term reached no requested column — dead wiring, not a quiet "
                      "run.", cc.ratio_min);
        if (cc.nonpos_rows > 0)
            log->warn("Network (carry-aware covariance): {} corrected variance(s) came "
                      "out NON-POSITIVE and were refused (never clamped).  This is not "
                      "a readback bug: the FOLD composes one Woodbury update per block "
                      "against the pristine N_rr⁻¹, which is exact only when no station "
                      "is touched by two blocks — and a block is one connected "
                      "component of ONE (const,signal) group.  This counter is the "
                      "first measurement anywhere of that cross-block approximation "
                      "error.  It is a result about the SOLVE; open it its own card.",
                      cc.nonpos_rows);
    } else {
        log->info("Network (carry-aware covariance): OFF — dd_sigma_cyc and the AR "
                  "probe's Q_cyc are CARRY-BLIND on this run (Cov(x_r) from the "
                  "prior-free N_rr⁻¹ while the shared block was carry-aware, i.e. the "
                  "covariance of no estimator).  Read them as an epoch-local float "
                  "bound only; in particular a uniform med|frac| is the EXPECTED "
                  "reading at these σ and is NOT evidence about integer information.");
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
