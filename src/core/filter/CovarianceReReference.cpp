/**
 * @file CovarianceReReference.cpp
 * @brief Implementation of the shared covariance re-reference primitives.
 *
 * @c applyBssd is the verbatim transform previously inlined in
 * @c PivotManager::applyCovarianceReReference; @c enforcePsd is the verbatim
 * conditioning guard previously inlined in the DD handover path
 * (@c DdPivotManager).  Both behaviours are preserved exactly — only their home
 * moved, so the network producer can reuse them without copy-paste drift.
 */

#include "filter/CovarianceReReference.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Cholesky>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {
namespace CovReReference {

void applyBssd(const std::vector<Eigen::Index>& group_indices,
               Eigen::Index pivot_idx,
               Eigen::MatrixXd& P) {
    // Row operations: T * P
    for (const Eigen::Index gi : group_indices) {
        if (gi == pivot_idx) continue;
        P.row(gi) -= P.row(pivot_idx);
    }
    // Column operations: (T * P) * T'
    for (const Eigen::Index gi : group_indices) {
        if (gi == pivot_idx) continue;
        P.col(gi) -= P.col(pivot_idx);
    }
    // Freeze the pivot row/col
    P.row(pivot_idx).setZero();
    P.col(pivot_idx).setZero();
}

double enforcePsd(Eigen::MatrixXd& P,
                  LogManager& log_manager,
                  const std::string& context) {
    // Symmetrize defensively, then check PSD via LDLT.  Symmetrization alone
    // fixes asymmetry from FP rounding; it does NOT guarantee PSD if P was
    // already ill-conditioned or a near-rank-deficient pivot row crept through.
    // Log when the LDLT reports any non-positive pivot so the issue surfaces in
    // logs rather than poisoning the filter silently.
    //
    // .eval() is load-bearing: the in-place form `P = 0.5 * (P + P.transpose())`
    // ALIASES — element (i,j) depends on input (j,i), but Eigen overwrites in
    // storage order, so P(0,1) reads the already-symmetrized P(1,0) and the
    // upper triangle is silently corrupted.  At DD/user scale the induced
    // asymmetry is second-order small (P is already near-symmetric) so it was
    // masked; across network-scale handovers it would accumulate.  Materializing
    // the symmetric result into a temporary first makes the transform exact.
    P = (0.5 * (P + P.transpose())).eval();

    Eigen::LDLT<Eigen::MatrixXd> ldlt(P);
    const auto& d = ldlt.vectorD();
    const double min_d = (d.size() > 0) ? d.minCoeff() : 0.0;
    if (ldlt.info() != Eigen::Success || min_d <= 0.0) {
        log_manager.getLogger("main_log")->warn(
            "{}: P not PSD after T·P·Tᵀ (min LDLT diagonal = {:.3e}); "
            "applying epsilon regularization",
            context, min_d);
        const double eps = std::max(1e-12, std::abs(min_d) + 1e-9);
        P.diagonal().array() += eps;
    }
    return min_d;
}

} // namespace CovReReference
} // namespace GnssProcessor
