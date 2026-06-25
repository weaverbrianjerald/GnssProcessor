/**
 * @file CovarianceReReference.hpp
 * @brief Shared covariance S-transform primitives for pivot re-referencing.
 *
 * Two pieces are genuinely common across the pivot-handover paths and the
 * minimum-constraint S-system network producer, so they live here rather than
 * being duplicated:
 *
 *   - @c applyBssd       — the BSSD / PPP-RTK single-pivot subtract-and-freeze
 *                          T·P·Tᵀ transform (extracted from
 *                          @c PivotManager::applyCovarianceReReference, which
 *                          now delegates to it).
 *   - @c enforcePsd      — the post-transform conditioning guard (symmetrize +
 *                          LDLT min-pivot check + epsilon ridge), extracted
 *                          from the DD handover path (@c DdPivotManager).
 *
 * NOTE on the DD transform: the baseline DD handover is NOT a subtract-and-freeze
 * on a single pivot — it negates the new-pivot row/col and subtracts from the
 * others (a different S-transform).  Only the @c enforcePsd guard is shared with
 * DD; its transform stays in @c DdPivotManager.
 *
 * NOTE on guarding (conformance doc §8.7): the single-receiver user BSSD path
 * (@c PivotManager) deliberately runs UNGUARDED — at user scale the groups are
 * small (~8–12 states) and handovers are rare, so FP error in T·P·Tᵀ does not
 * accumulate.  The minimum-constraint NETWORK producer, whose handover groups
 * span every station's ambiguity column and fire every broadcast interval, MUST
 * call @c enforcePsd after the transform.  Keeping the guard a separate, opt-in
 * call preserves the validated user-path numerics byte-for-byte while making the
 * network path safe.
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
 * row/col, then freeze (zero) the pivot row and column.  Mathematically:
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
 * Symmetrizes @p P (fixing FP asymmetry from the T·P·Tᵀ row/col passes), then
 * LDLT-checks for positive semi-definiteness.  If the LDLT fails or any
 * diagonal pivot is non-positive, logs a warning under @p context (on the
 * "main_log" logger) and adds an epsilon ridge to the diagonal so the filter is
 * not silently poisoned by a non-PSD covariance.
 *
 * @param[in,out] P       Covariance matrix, modified in place.
 * @param log_manager     Logger source for the non-PSD warning.
 * @param context         Short label naming the call site (e.g. "DD pivot
 *                        handover", "network pivot handover") used in the warning.
 * @return The minimum LDLT diagonal of the pre-ridge matrix.  A value ≤ 0
 *         indicates the matrix was not PSD before the ridge — useful as a
 *         product-quality / SISA-degradation flag for network handovers.
 */
double enforcePsd(Eigen::MatrixXd& P,
                  LogManager& log_manager,
                  const std::string& context);

} // namespace CovReReference
} // namespace GnssProcessor
