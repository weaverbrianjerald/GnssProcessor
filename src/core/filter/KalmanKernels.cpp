/**
 * @file KalmanKernels.cpp
 * @brief The algebra kernels of the KalmanFilter: the sparse design-matrix products of
 *        update(), the aided-predict splice and the dense Joseph-form update.
 *
 * Stateless functions only.  The sparse-H products are declared in
 * include/filter/KalmanKernels.hpp (namespace GnssProcessor::kalman); spliceAndPropagate
 * and josephPseudoUpdate are static members of KalmanFilter (include/filter/KalmanFilter.hpp).
 */

#include "filter/KalmanKernels.hpp"
#include "filter/KalmanFilter.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <vector>

namespace GnssProcessor {
namespace kalman {

    // Sparse-H helpers.
    //
    // H is stored dense (~m × n) but each row holds only ~5 non-zeros out of
    // ~350 columns (geometry + ambiguity + maybe tropo / iono), so dense
    // `H·P`, `HP·Hᵀ` and `K·H` waste O(n²·m) ops on zeros.  These build a
    // per-row (column, value) list once per epoch and walk it instead,
    // producing the same dense result the dense matmul would.
    //
    // The 1e-12 threshold separates real geometry partials from the FP noise
    // of zero-initialized blocks.

    constexpr double kHSparseThreshold = 1e-12;

    /// Build per-row (col, val) lists of all non-zeros in H.
    std::vector<SparseHRow> buildSparseH(const Eigen::MatrixXd& H) {
        std::vector<SparseHRow> out(static_cast<size_t>(H.rows()));
        for (Eigen::Index i = 0; i < H.rows(); ++i) {
            auto& r = out[static_cast<size_t>(i)];
            r.cols.reserve(8);
            r.vals.reserve(8);
            for (Eigen::Index j = 0; j < H.cols(); ++j) {
                const double v = H(i, j);
                if (std::abs(v) > kHSparseThreshold) {
                    r.cols.push_back(static_cast<int>(j));
                    r.vals.push_back(v);
                }
            }
        }
        return out;
    }

    /// HP = H · P  (m×n).  Row-by-row sparse accumulation.
    Eigen::MatrixXd sparseHP(const std::vector<SparseHRow>& Hs,
                              const Eigen::MatrixXd& P) {
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        const Eigen::Index n = P.cols();
        Eigen::MatrixXd out = Eigen::MatrixXd::Zero(m, n);
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            const size_t nnz = r.cols.size();
            for (size_t k = 0; k < nnz; ++k) {
                out.row(i).noalias() += r.vals[k] * P.row(r.cols[k]);
            }
        }
        return out;
    }

    /// S_HHt = HP · Hᵀ  (m×m, symmetric).  Column-walk per pair.
    Eigen::MatrixXd sparseHPHt(const Eigen::MatrixXd& HP,
                                const std::vector<SparseHRow>& Hs) {
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        Eigen::MatrixXd out(m, m);
        for (Eigen::Index i = 0; i < m; ++i) {
            for (Eigen::Index j = i; j < m; ++j) {
                const auto& rj = Hs[static_cast<size_t>(j)];
                double s = 0.0;
                const size_t nnz = rj.cols.size();
                for (size_t k = 0; k < nnz; ++k) {
                    s += HP(i, rj.cols[k]) * rj.vals[k];
                }
                out(i, j) = s;
                if (i != j) out(j, i) = s;
            }
        }
        return out;
    }

    /// K·H (n×n).  K is n×m, H is m×n.  For each non-zero in row i of H,
    /// add val * K.col(i) into the corresponding column of the output.
    Eigen::MatrixXd sparseKH(const Eigen::MatrixXd& K,
                              const std::vector<SparseHRow>& Hs,
                              Eigen::Index n) {
        Eigen::MatrixXd out = Eigen::MatrixXd::Zero(n, n);
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            const size_t nnz = r.cols.size();
            for (size_t k = 0; k < nnz; ++k) {
                out.col(r.cols[k]).noalias() += r.vals[k] * K.col(i);
            }
        }
        return out;
    }

    /// H·v  (m,).  Sparse row-wise dot.
    Eigen::VectorXd sparseHv(const std::vector<SparseHRow>& Hs,
                              const Eigen::VectorXd& v) {
        const Eigen::Index m = static_cast<Eigen::Index>(Hs.size());
        Eigen::VectorXd out(m);
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto& r = Hs[static_cast<size_t>(i)];
            double s = 0.0;
            const size_t nnz = r.cols.size();
            for (size_t k = 0; k < nnz; ++k) {
                s += r.vals[k] * v(r.cols[k]);
            }
            out(i) = s;
        }
        return out;
    }

} // namespace kalman

    // ===================================================================
    // Aided-predict splice
    // ===================================================================

    void KalmanFilter::spliceAndPropagate(Eigen::VectorXd& x, Eigen::MatrixXd& P,
                                          Eigen::MatrixXd& Q_full, std::size_t state_size,
                                          const std::vector<PredictContribution>& contributions) {
        // Identity-seeded transition (F_full = I), the strapdown INS convention:
        // the means of states no contribution owns (non-INS states) are not
        // propagated.
        Eigen::MatrixXd F_full = Eigen::MatrixXd::Identity(state_size, state_size);

        for (const auto& c : contributions) {
            if (!c.valid) continue;

            // Nominal write-backs (pos/vel/att-zero for the IMU), out-of-range
            // / sentinel indices skipped.
            for (size_t k = 0; k < c.nominal_indices.size(); ++k) {
                const size_t idx = c.nominal_indices[k];
                if (idx < state_size)
                    x(static_cast<Eigen::Index>(idx)) =
                        c.nominal_values(static_cast<Eigen::Index>(k));
            }

            // Write the contribution's Phi/Q over the full-state indices it
            // owns.  This is assignment, not accumulation, so the baseline
            // process noise on owned rows is not counted twice.
            const size_t n = c.indices.size();
            for (size_t i = 0; i < n; ++i) {
                if (c.indices[i] >= state_size) continue;
                const auto ri = static_cast<Eigen::Index>(c.indices[i]);
                for (size_t j = 0; j < n; ++j) {
                    if (c.indices[j] >= state_size) continue;
                    const auto cj = static_cast<Eigen::Index>(c.indices[j]);
                    F_full(ri, cj) = c.Phi(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
                    Q_full(ri, cj) = c.Q(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
                }
            }
        }

        P = F_full * P * F_full.transpose() + Q_full;
        P = 0.5 * (P + P.transpose());
    }

    // ===================================================================
    // Dense Joseph-form update
    // ===================================================================

    void KalmanFilter::josephPseudoUpdate(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                          const Eigen::MatrixXd& H, const Eigen::VectorXd& innovation,
                                          const Eigen::MatrixXd& R,
                                          Eigen::VectorXd& x_out, Eigen::MatrixXd& P_out) {
        const Eigen::Index m = H.rows();
        const Eigen::Index n = P.rows();
        const Eigen::MatrixXd PHt = P * H.transpose();
        const Eigen::MatrixXd S   = H * PHt + R;
        const Eigen::MatrixXd K   = PHt * S.ldlt().solve(Eigen::MatrixXd::Identity(m, m));
        const Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(n, n) - K * H;

        const Eigen::VectorXd x_new = x + K * innovation;
        Eigen::MatrixXd P_new = IKH * P * IKH.transpose() + K * R * K.transpose();
        P_new = (0.5 * (P_new + P_new.transpose())).eval();

        x_out = x_new;
        P_out = P_new;
    }

} // namespace GnssProcessor
