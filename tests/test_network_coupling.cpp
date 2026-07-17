// Unit tests for the Card-1/1b/1d conditional coupling-row seam
// (filter/NetworkCoupling.hpp), carved out of Application::networkPassB_ at
// decomposition INC-7b.  These exercise the exact branches the 50-sta byte-gate
// arms cannot reach even when coupling is LIVE: the production whitenCouplingRows
// kernel (previously covered only by a parallel re-implementation), its refloat
// skip-set path and non-SPD fallback, and the CouplingRowBuilder scalar / joint /
// departed / kappa-shrink / SH-marginalize branches over a real registered
// estimator.  The builder-over-explicit-refs shape is directly constructible —
// no TestAccess shim required.

#include <gtest/gtest.h>

#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkEstimator.hpp"
#include "config/GnssConfigTypes.hpp"

#include <Eigen/Dense>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

// Build the joint row covariance R = diag(var_eff) + M_w diag(P_w) M_w^T that
// whitenCouplingRows factors, so a test can reconstruct L and assert the
// production whitening is exactly L^{-1} applied to the raw rows.
Eigen::MatrixXd jointCov(const std::vector<CplRowRaw>& rows,
                         const std::vector<double>& white_pw) {
    const Eigen::Index m = static_cast<Eigen::Index>(rows.size());
    const Eigen::Index nw = static_cast<Eigen::Index>(white_pw.size());
    Eigen::MatrixXd Mw = Eigen::MatrixXd::Zero(m, nw);
    Eigen::MatrixXd R  = Eigen::MatrixXd::Zero(m, m);
    for (Eigen::Index k = 0; k < m; ++k) {
        for (const auto& dc : rows[static_cast<size_t>(k)].departed)
            Mw(k, dc.first) = dc.second;
        R(k, k) = rows[static_cast<size_t>(k)].var_eff;
    }
    for (Eigen::Index j = 0; j < nw; ++j)
        R.noalias() += white_pw[static_cast<size_t>(j)] * (Mw.col(j) * Mw.col(j).transpose());
    return R;
}

}  // namespace

// --- whitenCouplingRows: the production kernel (previously only a parallel
//     re-implementation in test_network_estimator existed) -------------------

// The whitened +1/local (Hr), M_r/shared (Hs) and RHS (z) blocks must be exactly
// L^{-1} applied to the raw rows, where L L^T = R (the joint covariance).  We
// reconstruct L in the test and assert L * whitened == raw to 1e-12 — the direct
// numerical contract addStation relies on.
TEST(NetworkCouplingTest, WhitenRowsAreExactInverseCholeskyOfJointCovariance) {
    // Two locals (r0, r1) coupled to two present shared cols and sharing ONE
    // departed shared column W (index 0 in the M_w block) — the off-diagonal
    // correlation the scalar path drops.
    std::vector<CplRowRaw> rows(2);
    rows[0].local_col = 0;
    rows[0].shared = {{0, 0.7}};
    rows[0].departed = {{0, 0.5}};
    rows[0].rhs = 1.25;
    rows[0].var_eff = 0.04;
    rows[1].local_col = 1;
    rows[1].shared = {{1, -1.3}};
    rows[1].departed = {{0, -0.9}};
    rows[1].rhs = -0.5;
    rows[1].var_eff = 0.09;
    std::vector<double> white_pw = {2.5};   // P_w for the shared departed column

    const Eigen::Index ncol_r = 2, nshared = 2;
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    ASSERT_TRUE(whitenCouplingRows(rows, white_pw, /*skip=*/{}, ncol_r, nshared, Hr, Hs, z));
    ASSERT_EQ(Hr.rows(), 2);
    ASSERT_EQ(Hs.rows(), 2);

    const Eigen::MatrixXd R = jointCov(rows, white_pw);
    const Eigen::MatrixXd L = Eigen::LLT<Eigen::MatrixXd>(R).matrixL();

    // Raw (un-whitened) blocks: Hr_raw = +1 at local_col, Hs_raw = M_r at shared, z_raw = rhs.
    Eigen::MatrixXd Hr_raw = Eigen::MatrixXd::Zero(2, ncol_r);
    Eigen::MatrixXd Hs_raw = Eigen::MatrixXd::Zero(2, nshared);
    Eigen::VectorXd z_raw(2);
    for (int k = 0; k < 2; ++k) {
        Hr_raw(k, rows[k].local_col) = 1.0;
        for (const auto& sc : rows[k].shared) Hs_raw(k, sc.first) = sc.second;
        z_raw(k) = rows[k].rhs;
    }
    EXPECT_LT((L * Hr - Hr_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * Hs - Hs_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * z  - z_raw ).cwiseAbs().maxCoeff(), 1e-12);
}

// The refloat re-whiten path (ApplicationNetwork.cpp:4338) passes a `skip` set of
// local A_r columns to drop; NO gate arm exercises a coupling refloat.  The kept
// rows must be exactly those whose local_col is not in skip.
TEST(NetworkCouplingTest, WhitenSkipSetDropsRefloatedRows) {
    std::vector<CplRowRaw> rows(3);
    for (int k = 0; k < 3; ++k) {
        rows[static_cast<size_t>(k)].local_col = k;
        rows[static_cast<size_t>(k)].var_eff = 0.05;
        rows[static_cast<size_t>(k)].rhs = 0.1 * (k + 1);
    }
    std::vector<double> white_pw;   // no departed columns
    std::set<Eigen::Index> skip = {1};   // refloat the middle local

    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    ASSERT_TRUE(whitenCouplingRows(rows, white_pw, skip, 3, 0, Hr, Hs, z));
    ASSERT_EQ(Hr.rows(), 2);            // rows 0 and 2 survive
    // Surviving rows target locals 0 and 2 (diagonal +1 after unit-var whitening).
    EXPECT_GT(Hr(0, 0), 0.0);
    EXPECT_DOUBLE_EQ(Hr(0, 1), 0.0);
    EXPECT_GT(Hr(1, 2), 0.0);
    EXPECT_DOUBLE_EQ(Hr(1, 1), 0.0);
}

// A non-SPD joint covariance (here forced with a negative var_eff — a degenerate
// input production screens upstream, used only to reach the return path) must
// fail the ridge retry and return false, so the caller falls back to the scalar
// per-row marginalization.  This fallback counter has never been non-zero in any
// captured run, so only a unit test reaches it.
TEST(NetworkCouplingTest, WhitenNonSpdReturnsFalse) {
    std::vector<CplRowRaw> rows(1);
    rows[0].local_col = 0;
    rows[0].var_eff = -1.0;            // indefinite: R(0,0) = -1, ridge (1e-12) cannot rescue it
    rows[0].rhs = 0.0;
    std::vector<double> white_pw;
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    EXPECT_FALSE(whitenCouplingRows(rows, white_pw, /*skip=*/{}, 1, 0, Hr, Hs, z));
}

// An empty kept-row set is the documented success case (returns true, empty outputs).
TEST(NetworkCouplingTest, WhitenEmptyIsSuccess) {
    std::vector<CplRowRaw> rows;
    std::vector<double> white_pw;
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    EXPECT_TRUE(whitenCouplingRows(rows, white_pw, /*skip=*/{}, 2, 2, Hr, Hs, z));
    EXPECT_EQ(Hr.rows(), 0);
}

// --- CouplingRowBuilder over a real registered estimator --------------------

namespace {

// Minimal fixture: an estimator with one satellite clock + phase bias registered,
// plus the config/scratch a CouplingRowBuilder needs.  buildCplRow(col=0, ...)
// targets local A_r column 0.
struct BuilderFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    std::map<std::pair<SharedParamKind, std::string>, double> tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1}, sh_col{-1};

    BuilderFixture() {
        clk_col = est.registerClock("G01");
        pb_col  = est.registerPhaseBias("G01", "L1C");
        sh_col  = est.registerVtecCoeff(1, 0, 'C');   // present SH column (SH-marginalize test)
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 4);   // 4 local columns for the test
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make(bool joint, bool tu, bool shmarg) {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  joint, tu, shmarg, tu_reinit_hist, R};
    }
};

}  // namespace

// Scalar path: a coupling to a PRESENT shared column produces one PendCplRow with
// the shared term kept as an A_s coefficient and w = 1/var_eff.
TEST(NetworkCouplingTest, BuildScalarPresentColumnMakesPendRow) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 2.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C',
                                  /*coeff=*/0.8, /*s_prev=*/1.5, /*p_prev=*/0.2} };
    auto cb = f.make(/*joint=*/false, /*tu=*/false, /*shmarg=*/false);
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.5, lc, /*var_eff=*/0.04, /*gap=*/30.0);

    ASSERT_EQ(cb.crows.size(), 1u);
    ASSERT_TRUE(cb.craw.empty());
    const PendCplRow& row = cb.crows[0];
    EXPECT_EQ(row.local_col, 0);
    ASSERT_EQ(row.shared.size(), 1u);
    EXPECT_EQ(row.shared[0].first, f.clk_col);
    EXPECT_DOUBLE_EQ(row.shared[0].second, 0.8);          // present coeff unchanged (tu off)
    EXPECT_DOUBLE_EQ(row.w, 1.0 / 0.04);                  // no departed/tu inflation
    // y = (value_abs - x_lin) + coeff * s_prev
    EXPECT_DOUBLE_EQ(row.y, (2.0 - 0.5) + 0.8 * 1.5);
    EXPECT_EQ(f.R.cpl_terms_present, 1);
    EXPECT_EQ(f.R.cpl_terms_departed, 0);
}

// A coupling to a shared parameter NOT registered this epoch is DEPARTED: on the
// scalar path it is marginalized into the row variance (sigma^2 += coeff^2 P_prev)
// and counted as a departed term.
TEST(NetworkCouplingTest, BuildScalarDepartedColumnMarginalizesIntoVariance) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G99", "", 0, 0, 'C',
                                  /*coeff=*/0.5, /*s_prev=*/3.0, /*p_prev=*/9.0} };  // G99 not registered
    auto cb = f.make(false, false, false);
    cb.buildCplRow(/*col=*/1, /*x_lin_val=*/0.0, lc, /*var_eff=*/0.01, /*gap=*/30.0);

    ASSERT_EQ(cb.crows.size(), 1u);
    const PendCplRow& row = cb.crows[0];
    EXPECT_TRUE(row.shared.empty());                       // departed → no present A_s term
    EXPECT_DOUBLE_EQ(row.w, 1.0 / (0.01 + 0.5 * 0.5 * 9.0)); // sigma^2 = var_eff + coeff^2 P_prev
    EXPECT_EQ(f.R.cpl_terms_departed, 1);
    EXPECT_EQ(f.R.cpl_terms_present, 0);
}

// Card 1d border time update: a present column is kappa-shrunk (coeff * P/(P+q*dt))
// and the row variance inflated; the shrink is counted.
TEST(NetworkCouplingTest, BuildScalarTimeUpdateShrinksPresentColumn) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C',
                                  /*coeff=*/1.0, /*s_prev=*/0.0, /*p_prev=*/1.0} };
    const double q_clock = 1e-4, gap = 30.0;
    auto cb = f.make(/*joint=*/false, /*tu=*/true, /*shmarg=*/false);
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.0, lc, /*var_eff=*/0.01, gap);

    ASSERT_EQ(cb.crows.size(), 1u);
    ASSERT_EQ(cb.crows[0].shared.size(), 1u);
    const double qj = q_clock * gap;
    const double kap = 1.0 / (1.0 + qj);                  // P/(P+q*dt), P=1
    // A single clock term is the only member of its (constellation,kind) group, so
    // tuGaugeRestore restores it fully back to the unshrunk coefficient.
    EXPECT_NEAR(cb.crows[0].shared[0].second, 1.0, 1e-12);
    EXPECT_EQ(f.R.cpl_terms_shrunk, 1);
    EXPECT_GT(cb.crows[0].shared[0].second, kap);          // restored above the raw kappa-shrink
}

// Joint path: the same present coupling produces a CplRowRaw (not a PendCplRow),
// carrying its shared terms for the group whitening.
TEST(NetworkCouplingTest, BuildJointPresentColumnMakesRawRow) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 1.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C',
                                  /*coeff=*/-0.6, /*s_prev=*/0.4, /*p_prev=*/0.1} };
    auto cb = f.make(/*joint=*/true, /*tu=*/false, /*shmarg=*/false);
    cb.buildCplRow(/*col=*/2, /*x_lin_val=*/0.0, lc, /*var_eff=*/0.02, /*gap=*/30.0);

    ASSERT_EQ(cb.craw.size(), 1u);
    ASSERT_TRUE(cb.crows.empty());
    EXPECT_EQ(cb.craw[0].local_col, 2);
    ASSERT_EQ(cb.craw[0].shared.size(), 1u);
    EXPECT_EQ(cb.craw[0].shared[0].first, f.pb_col);
    EXPECT_TRUE(cb.craw[0].departed.empty());
    EXPECT_EQ(f.R.cpl_terms_present, 1);
}

// Card 1d-SH marginalize: with carry_coupling_sh_marginalize a VtecSh coupling is
// forced onto the departed (joint M_w) route even though its column is present.
TEST(NetworkCouplingTest, BuildJointShMarginalizeRoutesVtecToDeparted) {
    BuilderFixture f;   // SH column (1,0,'C') registered present in the fixture
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::VtecSh, "", "", 1, 0, 'C',
                                  /*coeff=*/0.3, /*s_prev=*/0.0, /*p_prev=*/0.5} };
    auto cb = f.make(/*joint=*/true, /*tu=*/false, /*shmarg=*/true);
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.0, lc, /*var_eff=*/0.02, /*gap=*/30.0);

    ASSERT_EQ(cb.craw.size(), 1u);
    EXPECT_TRUE(cb.craw[0].shared.empty());                // SH term NOT kept present
    ASSERT_EQ(cb.craw[0].departed.size(), 1u);             // routed to the joint M_w block
    EXPECT_EQ(f.R.cpl_terms_departed, 1);
}

// appendCouplingRows (scalar) scatters the buffered PendCplRows into R.A_s/A_r/w/y
// as a trailing block and records the refloat handles.
TEST(NetworkCouplingTest, AppendScalarScattersRowsIntoStationBlock) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 2.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C',
                                  0.8, 1.5, 0.2} };
    auto cb = f.make(/*joint=*/false, false, false);
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.5, lc, /*var_eff=*/0.04, /*gap=*/30.0);
    cb.appendCouplingRows();

    ASSERT_EQ(f.R.A_s.rows(), 1);
    ASSERT_EQ(f.R.A_r.rows(), 1);
    EXPECT_EQ(f.R.cpl_row_start, 0);
    ASSERT_EQ(f.R.cpl_row_local_cols.size(), 1u);
    EXPECT_EQ(f.R.cpl_row_local_cols[0], 0);
    EXPECT_DOUBLE_EQ(f.R.A_r(0, 0), 1.0);                  // +1 on the carried local
    EXPECT_DOUBLE_EQ(f.R.A_s(0, f.clk_col), 0.8);          // M_r on the shared border
    EXPECT_DOUBLE_EQ(f.R.w(0), 1.0 / 0.04);
    EXPECT_TRUE(f.R.carry_cols.empty());                  // scalar coupling bypasses addStationPrior
}

// appendCouplingRows (joint) whitens the buffered CplRowRaw group and retains the
// pre-whitening ingredients (cpl_raw / cpl_white_pw) for the refloat re-whiten.
TEST(NetworkCouplingTest, AppendJointWhitensAndRetainsIngredients) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 1.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C',
                                  -0.6, 0.4, 0.1} };
    auto cb = f.make(/*joint=*/true, false, false);
    cb.buildCplRow(/*col=*/2, /*x_lin_val=*/0.0, lc, /*var_eff=*/0.02, /*gap=*/30.0);
    cb.appendCouplingRows();

    ASSERT_EQ(f.R.A_s.rows(), 1);
    EXPECT_EQ(f.R.cpl_row_start, 0);
    EXPECT_FALSE(f.R.cpl_joint_fallback);
    ASSERT_EQ(f.R.cpl_raw.size(), 1u);                    // ingredients retained for refloat
    EXPECT_DOUBLE_EQ(f.R.w(0), 1.0);                      // unit-information whitened row
    EXPECT_DOUBLE_EQ(f.R.A_r(0, 2), 1.0 / std::sqrt(0.02)); // L^{-1} of a 1x1 R=var_eff
}
