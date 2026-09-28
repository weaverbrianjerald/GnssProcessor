/**
 * @file CovarianceReReference.cpp
 * @brief Implementation of the shared covariance re-reference primitives.
 *
 * @c applyBssd is the transform used by
 * @c PivotManager::applyCovarianceReReference; @c enforcePsd is the conditioning
 * guard used by the DD handover path (@c DdPivotManager).  Keeping one copy of
 * each lets a network producer reuse them without a diverging duplicate.
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
    // Symmetrize, then check definiteness via LDLT.  Symmetrization removes
    // asymmetry from FP rounding; it does not make P PSD if P was already
    // ill-conditioned or a near-rank-deficient pivot row came through.  Any
    // non-positive LDLT pivot is logged and ridged below.
    //
    // .eval() is required: the in-place form `P = 0.5 * (P + P.transpose())`
    // aliases.  Element (i,j) depends on input (j,i), but Eigen overwrites in
    // storage order, so P(0,1) reads the already-symmetrized P(1,0) and the
    // upper triangle is wrong.  At DD/user scale the error is second-order (P is
    // already near-symmetric); across network-scale handovers it would
    // accumulate.  Evaluating the symmetric result into a temporary first makes
    // the symmetrization exact.
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
