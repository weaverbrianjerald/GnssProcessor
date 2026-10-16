#pragma once

/**
 * @file KalmanKernels.hpp
 * @brief @internal The sparse design-matrix products of KalmanFilter::update.
 *
 * H is stored dense (m x n) but each row holds only about five non-zeros out of
 * about 350 columns (geometry, one ambiguity, perhaps tropo and ionosphere), so the
 * dense products H*P, HP*H' and K*H spend O(n^2 m) operations on zeros.  These kernels
 * build a per-row (column, value) list once per update and walk it instead, producing
 * the same dense result the dense product would, element by element in the same order.
 *
 * Stateless free functions, defined in src/core/filter/KalmanKernels.cpp beside the
 * other algebra of the filter (KalmanFilter::spliceAndPropagate and
 * KalmanFilter::josephPseudoUpdate).  Included only by
 * src/core/filter/Kalman*.cpp.
 */

#include <Eigen/Dense>
#include <vector>

namespace GnssProcessor {
namespace kalman {

    /**
     * @brief The non-zeros of one row of H: column indices and their values.
     *
     * Entries with |value| <= 1e-12 are dropped (the noise floor of zero-initialized
     * blocks); the order is the column order of the dense row.
     */
    struct SparseHRow {
        std::vector<int>    cols;
        std::vector<double> vals;
    };

    /**
     * @brief Build the per-row non-zero lists of H.
     * @param H Dense design matrix (m x n).
     * @return One SparseHRow per row of H, in row order.
     */
    std::vector<SparseHRow> buildSparseH(const Eigen::MatrixXd& H);

    /**
     * @brief HP = H * P (m x n), accumulated row by row over the non-zeros of H.
     * @param Hs Sparse rows of H (from buildSparseH).
     * @param P  State covariance (n x n).
     * @return The dense product H * P.
     */
    Eigen::MatrixXd sparseHP(const std::vector<SparseHRow>& Hs,
                              const Eigen::MatrixXd& P);

    /**
     * @brief HP * H' (m x m, symmetric), the upper triangle formed and mirrored.
     * @param HP Dense H * P (m x n), from sparseHP.
     * @param Hs Sparse rows of H.
     * @return The dense product HP * H'.
     */
    Eigen::MatrixXd sparseHPHt(const Eigen::MatrixXd& HP,
                                const std::vector<SparseHRow>& Hs);

    /**
     * @brief K * H (n x n): for each non-zero of row i of H, add value * K.col(i)
     *        into the matching column of the output.
     * @param K  Gain (n x m).
     * @param Hs Sparse rows of H.
     * @param n  State dimension.
     * @return The dense product K * H.
     */
    Eigen::MatrixXd sparseKH(const Eigen::MatrixXd& K,
                              const std::vector<SparseHRow>& Hs,
                              Eigen::Index n);

    /**
     * @brief H * v (m), one sparse row-wise dot product per row.
     * @param Hs Sparse rows of H.
     * @param v  Vector of the state dimension (n).
     * @return The dense product H * v.
     */
    Eigen::VectorXd sparseHv(const std::vector<SparseHRow>& Hs,
                              const Eigen::VectorXd& v);

} // namespace kalman
} // namespace GnssProcessor
