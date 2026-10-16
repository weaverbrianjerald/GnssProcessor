/**
 * @file KalmanResidualQc.cpp
 * @brief The statistical QC of one update: the Baarda kernel (redundancy, w-test, MDB,
 *        variance-factor sums) and its fill of the epoch measurement.
 *
 * KalmanFilter::residualQcKernel is pure and static (unit-tested in
 * tests/test_residual_qc_wtest.cpp); KalmanFilter::computeResidualQc_ applies it to an
 * EpochMeasurement.  Neither touches the state.
 */

#include "filter/KalmanFilter.hpp"
#include "filter/BaardaReliability.hpp"   // δ₀ / MDB, shared with the network path
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Statistical QC: a-posteriori variance factor + Baarda data-snooping
    // ===================================================================

    void KalmanFilter::computeResidualQc_(
        EpochMeasurement& meas, const Eigen::MatrixXd& R,
        const Eigen::VectorXd& sr_diag, const Eigen::MatrixXd* Sr_full) const {

        const Eigen::Index m = meas.residuals.size();
        if (m == 0) {
            meas.variance_factor = 1.0;
            meas.variance_dof    = 0.0;
            meas.residual_redundancy.resize(0);
            meas.residual_wtest.resize(0);
            meas.residual_mdb.resize(0);
            return;
        }

        // Baarda non-centrality δ₀ at the conventional 80% test power
        // (β=0.20 → Φ⁻¹(0.80)=0.8416); δ₀≈4.13 for the default k=3.29.  σ₀²=1
        // since R is in absolute units, so MDBᵢ = σᵢ·δ₀/√rᵢ.
        //
        // δ₀, the r-floor (1e-6) and the r→0 ⇒ ∞ sentinel come from
        // filter/BaardaReliability.hpp, so the network path's MDB is
        // commensurable with this one by construction rather than by a second
        // copy of the formula.
        const double delta_0 = baardaDelta0(config_.dataSnoopingCritical());
        const ResidualQcStats q =
            residualQcKernel(meas.residuals, R, sr_diag, Sr_full, delta_0);

        meas.residual_redundancy = q.redundancy;
        meas.residual_wtest      = q.wtest;
        meas.residual_mdb        = q.mdb;
        meas.variance_dof        = q.dof;
        meas.variance_factor     =
            (q.dof > 1e-9 && std::isfinite(q.wssr)) ? (q.wssr / q.dof) : 1.0;
    }

    KalmanFilter::ResidualQcStats KalmanFilter::residualQcKernel(
        const Eigen::VectorXd& v, const Eigen::MatrixXd& R,
        const Eigen::VectorXd& sr_diag, const Eigen::MatrixXd* Sr_full,
        double delta_0) {

        const Eigen::Index m = v.size();
        constexpr double kSrFloor  = 1e-12;   // guard /√Sr

        Eigen::VectorXd redundancy(m);
        Eigen::VectorXd wtest(m);
        Eigen::VectorXd mdb(m);
        double wssr = 0.0;   // Σ vᵀR⁻¹v  (weighted post-fit residual SSR)
        double dof  = 0.0;   // Σ rᵢ      (effective redundancy / degrees of freedom)

        bool use_full = (Sr_full != nullptr &&
                         Sr_full->rows() == m && Sr_full->cols() == m &&
                         R.rows() == m && R.cols() == m);

        // P = R⁻¹, formed only for the correlation-aware branch.  An R that is not
        // positive definite has no weight matrix, so that branch falls back to the
        // diagonal one rather than testing against an invented P.
        Eigen::MatrixXd Rinv;
        if (use_full) {
            const Eigen::LDLT<Eigen::MatrixXd> R_ldlt(R);
            use_full = (R_ldlt.info() == Eigen::Success) && R_ldlt.isPositive();
            if (use_full) {
                Rinv = R_ldlt.solve(Eigen::MatrixXd::Identity(m, m));
                use_full = Rinv.allFinite();
            }
        }

        if (use_full) {
            // Correlation-aware path (off-diagonal R kernel, item #13A).  With the
            // weight matrix P = R⁻¹ and the residual cofactor Q_v = Sr:
            //   redundancy matrix  Q_v·P = Sr·R⁻¹   → rᵢ = (Sr R⁻¹)ᵢᵢ, dof = tr
            //   w-test (Baarda)    wᵢ = (P v)ᵢ / √(P Q_v P)ᵢᵢ
            //   MDB                MDBᵢ = δ₀ / √(P Q_v P)ᵢᵢ
            //   wssr               = vᵀ P v
            // In a Kalman update P·v = S⁻¹·z and P·Q_v·P = S⁻¹, so wᵢ is the
            // innovation w-test (S⁻¹z)ᵢ/√(S⁻¹)ᵢᵢ.  Until 2026-10-07 this branch used
            // Sr⁻¹ in place of P: that statistic is N(0,1) under H₀ but tests the
            // direction S·R⁻¹·eᵢ rather than observation i, so it lost power against
            // a blunder in row i and disagreed with the diagonal branch even for a
            // diagonal R (Snow, Adjustment Computations, ch. 9; LASER 5-97..5-100).
            const Eigen::MatrixXd Red = (*Sr_full) * Rinv;
            const Eigen::MatrixXd PQP = Rinv * (*Sr_full) * Rinv;
            const Eigen::VectorXd Pv  = Rinv * v;
            wssr = v.dot(Pv);
            for (Eigen::Index i = 0; i < m; ++i) {
                double r_i = Red(i, i);
                r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;
                redundancy(i) = r_i;
                dof += r_i;
                // wᵢ and MDBᵢ are written through the row's own σ so their guards
                // match the diagonal branch: uᵢ = Rᵢᵢ·(P v)ᵢ (m) and
                // sᵢ = Rᵢᵢ²·(P Q_v P)ᵢᵢ (m²) are exactly vᵢ and Sr_ii when R is
                // diagonal, and r̃ᵢ = Rᵢᵢ·(P Q_v P)ᵢᵢ is then rᵢ.
                const double Rii = R(i, i);
                const double pqp = PQP(i, i);
                const double u_i = Rii * Pv(i);
                const double s_i = Rii * Rii * pqp;
                wtest(i) = (Rii > 0.0 && std::isfinite(u_i) && std::isfinite(s_i))
                    ? u_i / std::sqrt(std::max(s_i, kSrFloor)) : 0.0;
                const double sigma_i = (Rii > 0.0) ? std::sqrt(Rii) : 0.0;
                const double r_mdb   = (Rii > 0.0 && std::isfinite(pqp)) ? Rii * pqp : 0.0;
                mdb(i) = baardaMinimalDetectableBias(sigma_i, r_mdb, delta_0);
            }
        } else {
            // Diagonal (uncorrelated) path, the common PPP case.
            for (Eigen::Index i = 0; i < m; ++i) {
                const double Rii  = (i < R.rows() && R(i, i) > 0.0) ? R(i, i) : 0.0;
                const double srii = (i < sr_diag.size()) ? sr_diag(i) : 0.0;
                const double v_i  = v(i);
                double r_i = (Rii > 0.0) ? srii / Rii : 0.0;
                r_i = std::isfinite(r_i) ? std::min(std::max(r_i, 0.0), 1.0) : 0.0;
                redundancy(i) = r_i;
                dof += r_i;
                if (Rii > 0.0 && std::isfinite(v_i)) wssr += (v_i * v_i) / Rii;
                // Baarda w-test: the studentized residual v/√Sr, not floored at R.
                const double srii_pos = std::max(srii, kSrFloor);
                wtest(i) = std::isfinite(v_i) ? v_i / std::sqrt(srii_pos) : 0.0;
                const double sigma_i = (Rii > 0.0) ? std::sqrt(Rii) : 0.0;
                mdb(i) = baardaMinimalDetectableBias(sigma_i, r_i, delta_0);
            }
        }

        ResidualQcStats out;
        out.redundancy = std::move(redundancy);
        out.wtest      = std::move(wtest);
        out.mdb        = std::move(mdb);
        out.wssr       = wssr;
        out.dof        = dof;
        return out;
    }

} // namespace GnssProcessor
