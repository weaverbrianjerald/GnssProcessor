/**
 * @file CovarianceReReference.hpp
 * @brief Shared covariance S-transform primitives for pivot re-referencing.
 *
 * Two pieces are kept here so the pivot-handover paths do not duplicate them:
 *
 *   - @c applyBssd       the BSSD / PPP-RTK single-pivot subtract-and-freeze
 *                        T·P·Tᵀ transform (@c PivotManager::applyCovarianceReReference
 *                        delegates to it).
 *   - @c enforcePsd      the post-transform conditioning guard (symmetrize, LDLT
 *                        minimum-pivot check, epsilon ridge), called by the DD
 *                        handover path (@c DdPivotManager).
 *
 * They are also intended for the minimum-constraint S-system network producer;
 * no network-producer code calls either one in the current tree.
 *
 * The DD transform: the baseline DD handover is not a subtract-and-freeze on a
 * single pivot.  It negates the new-pivot row/col and subtracts it from the
 * others (a different S-transform).  Only the @c enforcePsd guard is shared with
 * DD; its transform stays in @c DdPivotManager.
 *
 * Guarding (docs/SPEC_sbasis_realization_contract.md §F8.7): the single-receiver
 * user BSSD path (@c PivotManager) runs without the guard on purpose.  At user
 * scale the groups are small (~8–12 states) and handovers are rare, so FP error
 * in T·P·Tᵀ does not accumulate.  A minimum-constraint network producer, whose
 * handover groups span every station's ambiguity column and fire every broadcast
 * interval, has to call @c enforcePsd after the transform.  Keeping the guard a
 * separate, opt-in call leaves the validated user-path numerics byte-identical.
 */

#pragma once

#include <Eigen/Dense>
#include <string>
#include <vector>

namespace GnssProcessor {

class LogManager;

namespace CovReReference {

/**
 * @brief BSSD / PPP-RTK single-pivot covariance re-reference (in place).
 *
 * Applies the S-transform T·P·Tᵀ that re-references a per-constellation group
 * onto @p pivot_idx: for every non-pivot group member subtract the pivot
 * row/col, then freeze (zero) the pivot row and column.  In matrix terms:
 *   - non-pivot i:  P'(i,:) = P(i,:) − P(pivot,:),  P'(:,i) = P(:,i) − P(:,pivot)
 *   - pivot:        P'(pivot,:) = 0,                 P'(:,pivot) = 0
 *
 * @param group_indices All state indices in the group (including the pivot).
 * @param pivot_idx     The pivot's state index.
 * @param[in,out] P     Covariance matrix, modified in place.
 */
void applyBssd(const std::vector<Eigen::Index>& group_indices,
               Eigen::Index pivot_idx,
               Eigen::MatrixXd& P);

/**
 * @brief Post-transform PSD conditioning guard (in place).
 *
 * Symmetrizes @p P (removing FP asymmetry from the T·P·Tᵀ row/col passes), then
 * factors it with LDLT.  If the LDLT fails or any diagonal pivot is
 * non-positive, logs a warning under @p context (on the "main_log" logger) and
 * adds an epsilon ridge to the diagonal, so the filter does not continue with a
 * covariance that is not positive definite.
 *
 * @param[in,out] P       Covariance matrix, modified in place.
 * @param log_manager     Logger source for the warning.
 * @param context         Short label naming the call site (e.g. "DD pivot
 *                        handover") used in the warning.
 * @return The minimum LDLT diagonal of the pre-ridge matrix.  A value ≤ 0 means
 *         the matrix was singular or indefinite before the ridge; it can serve
 *         as a product-quality / SISA-degradation flag for network handovers.
 */
double enforcePsd(Eigen::MatrixXd& P,
                  LogManager& log_manager,
                  const std::string& context);

} // namespace CovReReference
} // namespace GnssProcessor
