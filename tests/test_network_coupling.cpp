// Unit tests for the conditional coupling-row seam of cards 1/1b/1d
// (filter/NetworkCoupling.hpp, split out of Application::networkPassB_ at INC-7b).
// They exercise branches the 50-station byte-gate arms do not reach even with
// coupling live: the production whitenCouplingRows kernel, its refloat skip-set path
// and non-SPD fallback, and the CouplingRowBuilder scalar / joint / departed /
// kappa-shrink / SH-marginalize branches over a real registered estimator.  The
// builder takes explicit references, so it is directly constructible without a
// TestAccess shim.

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkChainDatum.hpp"   // eq34ShiftCarry (the D4 shift through the traversal)
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkEstimator.hpp"
#include "config/GnssConfigTypes.hpp"

#include <Eigen/Dense>

#include <limits>
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

// --- whitenCouplingRows: the production kernel ------------------------------

// The whitened +1/local (Hr), M_r/shared (Hs) and RHS (z) blocks are exactly L^{-1}
// applied to the raw rows, where L L^T = R (the joint covariance).  The test
// reconstructs L and asserts L * whitened == raw to 1e-12, the numerical contract
// addStation relies on.
TEST(NetworkCouplingTest, WhitenRowsAreExactInverseCholeskyOfJointCovariance) {
    // Two locals (r0, r1) coupled to two present shared columns and sharing one
    // departed shared column W (index 0 in the M_w block): the off-diagonal
    // correlation that the scalar path drops.
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
    std::vector<Eigen::Index> kept;
    ASSERT_TRUE(whitenCouplingRows(rows, white_pw, /*skip=*/{}, ncol_r, nshared, Hr, Hs, z,
                                   kept));
    EXPECT_EQ(kept, (std::vector<Eigen::Index>{0, 1}));   // no skip set: every row, in order
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

// The refloat re-whiten path (the joint-coupling refloat block in NetworkSolveGlue.cpp)
// passes a `skip` set of local A_r columns to drop; no gate arm exercises a coupling
// refloat.  The kept rows are exactly those whose local_col is not in skip.
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
    std::vector<Eigen::Index> kept;
    ASSERT_TRUE(whitenCouplingRows(rows, white_pw, skip, 3, 0, Hr, Hs, z, kept));
    ASSERT_EQ(Hr.rows(), 2);            // rows 0 and 2 survive
    EXPECT_EQ(kept, (std::vector<Eigen::Index>{0, 2}));   // and the kernel reports them
    // Surviving rows target locals 0 and 2 (diagonal +1 after unit-var whitening).
    EXPECT_GT(Hr(0, 0), 0.0);
    EXPECT_DOUBLE_EQ(Hr(0, 1), 0.0);
    EXPECT_GT(Hr(1, 2), 0.0);
    EXPECT_DOUBLE_EQ(Hr(1, 1), 0.0);
}

// A non-SPD joint covariance (here forced with a negative var_eff, a degenerate input
// that production screens upstream, used only to reach the return path) fails the
// ridge retry and returns false, so the caller falls back to the scalar per-row
// marginalization.  No captured run has reached this fallback, so only a unit test
// covers it.
TEST(NetworkCouplingTest, WhitenNonSpdReturnsFalse) {
    std::vector<CplRowRaw> rows(1);
    rows[0].local_col = 0;
    rows[0].var_eff = -1.0;            // indefinite: R(0,0) = -1, ridge (1e-12) cannot rescue it
    rows[0].rhs = 0.0;
    std::vector<double> white_pw;
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    std::vector<Eigen::Index> kept;
    EXPECT_FALSE(whitenCouplingRows(rows, white_pw, /*skip=*/{}, 1, 0, Hr, Hs, z, kept));
}

// An empty kept-row set is the documented success case (returns true, empty outputs).
TEST(NetworkCouplingTest, WhitenEmptyIsSuccess) {
    std::vector<CplRowRaw> rows;
    std::vector<double> white_pw;
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    std::vector<Eigen::Index> kept{7};   // cleared by the call
    EXPECT_TRUE(whitenCouplingRows(rows, white_pw, /*skip=*/{}, 2, 2, Hr, Hs, z, kept));
    EXPECT_EQ(Hr.rows(), 0);
    EXPECT_TRUE(kept.empty());
}

// --- CouplingRowBuilder over a real registered estimator --------------------

namespace {

// Minimal fixture: an estimator with one satellite clock + phase bias registered,
// plus the config/scratch a CouplingRowBuilder needs.  buildCplRow(col=0, ...)
// targets local A_r column 0.
struct BuilderFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1}, sh_col{-1}, clk2_col{-1};

    /// @param second_clock register a second G-constellation clock (G02) before
    ///        beginEpoch().  The card-1f cases need a present same-constellation
    ///        recipient for the truncated column's coefficient; the layout is frozen at
    ///        beginEpoch(), so it cannot be added afterwards.  The default (false)
    ///        leaves the layout of every other case unchanged.
    explicit BuilderFixture(bool second_clock = false) {
        clk_col = est.registerClock("G01");
        if (second_clock) clk2_col = est.registerClock("G02");
        pb_col  = est.registerPhaseBias("G01", "L1C");
        sh_col  = est.registerVtecCoeff(1, 0, 'C');   // present SH column (SH-marginalize test)
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 4);   // 4 local columns for the test
        R.w.resize(0);
        R.y.resize(0);
    }

    // `gauge` / `reinit` default to false, so every case that does not set them runs the
    // card 1f/1g code paths with those cards off.  Those cases, left unmodified, are the
    // proof that the cards change nothing when off.
    CouplingRowBuilder make(bool joint, bool tu, bool shmarg,
                            bool gauge = false, bool reinit = false) {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  joint, tu, shmarg, gauge, reinit, tu_reinit_hist, R};
    }
};

}  // namespace

// Scalar path: a coupling to a present shared column produces one PendCplRow with
// the shared term kept as an A_s coefficient and w = 1/var_eff.
TEST(NetworkCouplingTest, BuildScalarPresentColumnMakesPendRow) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 2.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', /*sh_layer=*/0,
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

// A coupling to a shared parameter not registered this epoch is departed: on the
// scalar path it is marginalized into the row variance (sigma^2 += coeff^2 P_prev)
// and counted as a departed term.
TEST(NetworkCouplingTest, BuildScalarDepartedColumnMarginalizesIntoVariance) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G99", "", 0, 0, 'C', /*sh_layer=*/0,
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

// Border time update (card 1d): a present column is kappa-shrunk (coeff * P/(P+q*dt))
// and the row variance inflated; the shrink is counted.
TEST(NetworkCouplingTest, BuildScalarTimeUpdateShrinksPresentColumn) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', /*sh_layer=*/0,
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

// --- Truncated-row gauge defect and its repair (card 1f) ---------------------
//
// A harvested coupling row is gauge-invariant through Mᵀg_s = −g_r(c): its
// per-(constellation,kind) coefficient sum is whatever the harvest regression made it,
// and it has to stay there.  Deleting a departed column's coefficient moves that sum by
// −M_dep, which turns the row into a high-weight observation of the current datum gauge
// pinned at the old realization.
//
// These four cases pin the contract of the fix: the family sum is held at the harvest
// value, not set to zero (§13.2: a Galileo ambiguity row measures Σ_E = −1 exactly, so
// zeroing the sum would itself inject a gauge observation).

// The defect itself, with the card off: the surviving coefficients sum to the harvest
// sum minus the departed coefficient.  This is the pre-registered "before" measurement.
TEST(NetworkCouplingTest, GaugeProjectOffLeavesTruncatedFamilySumShort) {
    BuilderFixture f(/*second_clock=*/true);   // a present recipient exists
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = {
        CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', 0, 0.5, 1.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G02", "", 0, 0, 'C', 0, 0.3, 2.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G99", "", 0, 0, 'C', 0, 0.2, 3.0, 0.4},  // departed
    };
    auto cb = f.make(/*joint=*/false, /*tu=*/false, /*shmarg=*/false, /*gauge=*/false);
    cb.buildCplRow(0, 0.0, lc, /*var_eff=*/0.01, /*gap=*/30.0);

    ASSERT_EQ(cb.crows.size(), 1u);
    double sum = 0.0;
    for (const auto& sc : cb.crows[0].shared) sum += sc.second;
    EXPECT_NEAR(sum, 0.8, 1e-12);          // harvest sum 1.0 minus the departed 0.2
    EXPECT_EQ(f.R.cpl_gauge_held, 0);      // with the card off: no repair, no counter movement
    EXPECT_EQ(f.R.cpl_gauge_orphan, 0);
}

// Armed: the departed coefficient is redistributed over the present same-constellation
// clocks, so the family sum returns to its harvest value.  The RHS follows the corrected
// coefficients, which keeps the row's prediction at ŝ_prev unchanged.
TEST(NetworkCouplingTest, GaugeProjectHoldsTruncatedFamilySumAtHarvestValue) {
    BuilderFixture f(/*second_clock=*/true);
    LocalCarry lc;
    lc.value_abs = 5.0;
    lc.last_seen_s = 0.0;
    lc.coupling = {
        CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', 0, 0.5, 1.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G02", "", 0, 0, 'C', 0, 0.3, 2.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G99", "", 0, 0, 'C', 0, 0.2, 3.0, 0.4},  // departed
    };
    auto cb = f.make(/*joint=*/false, /*tu=*/false, /*shmarg=*/false, /*gauge=*/true);
    cb.buildCplRow(0, 1.0, lc, /*var_eff=*/0.01, /*gap=*/30.0);

    ASSERT_EQ(cb.crows.size(), 1u);
    const PendCplRow& row = cb.crows[0];
    ASSERT_EQ(row.shared.size(), 2u);
    double sum = 0.0;
    for (const auto& sc : row.shared) sum += sc.second;
    EXPECT_NEAR(sum, 1.0, 1e-12);          // harvest sum restored (0.5 + 0.3 + 0.2)
    // The 0.2 deficit splits evenly over the two present recipients.
    EXPECT_NEAR(row.shared[0].second, 0.6, 1e-12);
    EXPECT_NEAR(row.shared[1].second, 0.4, 1e-12);
    // y = (value_abs − x_lin) + Σ corrected_coeff · s_prev, with the departed term's
    // own ŝ_prev absent (it was marginalized into the variance).
    EXPECT_NEAR(row.y, (5.0 - 1.0) + 0.6 * 1.0 + 0.4 * 2.0, 1e-12);
    // The projection leaves sigma^2 unchanged; the departed marginalization still applies.
    EXPECT_NEAR(row.w, 1.0 / (0.01 + 0.2 * 0.2 * 0.4), 1e-12);
    EXPECT_EQ(f.R.cpl_gauge_held, 1);
    EXPECT_EQ(f.R.cpl_gauge_orphan, 0);
}

// No present same-constellation recipient: the deficit has nowhere to go.  The row is
// left as it is and counted (invariant #4: the miss is reported, not partially fixed).
TEST(NetworkCouplingTest, GaugeProjectCountsOrphanWhenWholeFamilyDeparted) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = {
        CoupledShared{SharedParamKind::Clock, "G98", "", 0, 0, 'C', 0, 0.4, 1.0, 0.1},  // departed
        CoupledShared{SharedParamKind::Clock, "G99", "", 0, 0, 'C', 0, 0.2, 1.0, 0.1},  // departed
    };
    auto cb = f.make(false, false, false, /*gauge=*/true);
    cb.buildCplRow(0, 0.0, lc, /*var_eff=*/0.01, /*gap=*/30.0);

    ASSERT_EQ(cb.crows.size(), 1u);
    EXPECT_TRUE(cb.crows[0].shared.empty());
    EXPECT_EQ(f.R.cpl_gauge_held, 0);
    EXPECT_EQ(f.R.cpl_gauge_orphan, 1);
}

// The card is a strict generalization of card 1d: with nothing truncated, the armed row
// is bit-identical to the unarmed one (the deficit map is seeded with nothing).
TEST(NetworkCouplingTest, GaugeProjectIsIdenticalWhenNothingTruncated) {
    BuilderFixture f(/*second_clock=*/true);
    LocalCarry lc;
    lc.value_abs = 2.0;
    lc.last_seen_s = 0.0;
    lc.coupling = {
        CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', 0, 0.5, 1.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G02", "", 0, 0, 'C', 0, 0.3, 2.0, 0.2},
    };
    auto off = f.make(false, false, false, /*gauge=*/false);
    off.buildCplRow(0, 0.5, lc, 0.01, 30.0);
    BuilderFixture g(/*second_clock=*/true);
    auto on = g.make(false, false, false, /*gauge=*/true);
    on.buildCplRow(0, 0.5, lc, 0.01, 30.0);

    ASSERT_EQ(off.crows.size(), 1u);
    ASSERT_EQ(on.crows.size(), 1u);
    ASSERT_EQ(off.crows[0].shared.size(), on.crows[0].shared.size());
    for (size_t i = 0; i < off.crows[0].shared.size(); ++i) {
        EXPECT_EQ(off.crows[0].shared[i].first, on.crows[0].shared[i].first);
        EXPECT_DOUBLE_EQ(off.crows[0].shared[i].second, on.crows[0].shared[i].second);
    }
    EXPECT_DOUBLE_EQ(off.crows[0].y, on.crows[0].y);
    EXPECT_DOUBLE_EQ(off.crows[0].w, on.crows[0].w);
    EXPECT_EQ(g.R.cpl_gauge_held, 0);
}

// --- Reinit route independent of the kappa knob (card 1g) --------------------
//
// A column re-realized since this row's harvest carries an unknown frame step in its
// stashed ŝ_prev.  With card 1g on, such a column takes the departed route whether or
// not carry_coupling_time_update is on.  With card 1g and the time update both off, the
// retired-frame value folds at full weight on the plain card-1 path (the legacy case).
TEST(NetworkCouplingTest, ReinitSourcesRouteToDepartedWithTimeUpdateOff) {
    BuilderFixture f;
    // A (kind,sat,signal) mark, as the phase-anchor / eq34 re-reference feeds write it.
    f.tu_reinit_hist[{SharedParamKind::PhaseBias, "G01", "L1C"}] = 100.0;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 50.0;            // harvested before the re-realization at t=100
    lc.coupling = { CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C', 0,
                                  /*coeff=*/0.7, /*s_prev=*/9.0, /*p_prev=*/0.25} };

    // kappa off, card 1g off: the stale column folds as a full-weight present term.
    auto legacy = f.make(/*joint=*/false, /*tu=*/false, /*shmarg=*/false,
                         /*gauge=*/false, /*reinit=*/false);
    legacy.buildCplRow(0, 0.0, lc, 0.01, 30.0);
    ASSERT_EQ(legacy.crows.size(), 1u);
    EXPECT_EQ(legacy.crows[0].shared.size(), 1u);
    EXPECT_EQ(f.R.cpl_terms_present, 1);

    // kappa still off, card 1g on: it takes the departed route (kappa = 0).
    BuilderFixture g;
    g.tu_reinit_hist[{SharedParamKind::PhaseBias, "G01", "L1C"}] = 100.0;
    auto fixed = g.make(false, /*tu=*/false, false, /*gauge=*/false, /*reinit=*/true);
    fixed.buildCplRow(0, 0.0, lc, 0.01, 30.0);
    ASSERT_EQ(fixed.crows.size(), 1u);
    EXPECT_TRUE(fixed.crows[0].shared.empty());
    EXPECT_EQ(g.R.cpl_terms_departed, 1);
    EXPECT_NEAR(fixed.crows[0].w, 1.0 / (0.01 + 0.7 * 0.7 * 0.25), 1e-12);
}

// The clock-QC feeds write the satellite-wide wildcard (empty signal), which matches
// every signal of that satellite, so the widened key still covers the legacy feed.
TEST(NetworkCouplingTest, ReinitSatWideWildcardMatchesAnySignal) {
    BuilderFixture f;
    f.tu_reinit_hist[{SharedParamKind::PhaseBias, "G01", std::string{}}] = 100.0;
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 50.0;
    lc.coupling = { CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C', 0,
                                  0.7, 9.0, 0.25} };
    auto cb = f.make(false, /*tu=*/false, false, /*gauge=*/false, /*reinit=*/true);
    cb.buildCplRow(0, 0.0, lc, 0.01, 30.0);
    ASSERT_EQ(cb.crows.size(), 1u);
    EXPECT_TRUE(cb.crows[0].shared.empty());   // wildcard matched the L1C term
    EXPECT_EQ(f.R.cpl_terms_departed, 1);
}

// Joint path: the same projection contract, on the card-1b branch.
TEST(NetworkCouplingTest, GaugeProjectHoldsFamilySumOnJointPath) {
    BuilderFixture f(/*second_clock=*/true);
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = {
        CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', 0, 0.5, 1.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G02", "", 0, 0, 'C', 0, 0.3, 2.0, 0.2},
        CoupledShared{SharedParamKind::Clock, "G99", "", 0, 0, 'C', 0, 0.2, 3.0, 0.4},
    };
    auto cb = f.make(/*joint=*/true, /*tu=*/false, /*shmarg=*/false, /*gauge=*/true);
    cb.buildCplRow(0, 0.0, lc, 0.01, 30.0);

    ASSERT_EQ(cb.craw.size(), 1u);
    double sum = 0.0;
    for (const auto& sc : cb.craw[0].shared) sum += sc.second;
    EXPECT_NEAR(sum, 1.0, 1e-12);
    ASSERT_EQ(cb.craw[0].departed.size(), 1u);   // still jointly marginalized
    EXPECT_EQ(f.R.cpl_gauge_held, 1);
}

// Joint path: the same present coupling produces a CplRowRaw (not a PendCplRow),
// carrying its shared terms for the group whitening.
TEST(NetworkCouplingTest, BuildJointPresentColumnMakesRawRow) {
    BuilderFixture f;
    LocalCarry lc;
    lc.value_abs = 1.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C', /*sh_layer=*/0,
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

// SH marginalize (card 1d-SH): with carry_coupling_sh_marginalize a VtecSh coupling is
// forced onto the departed (joint M_w) route even though its column is present.
TEST(NetworkCouplingTest, BuildJointShMarginalizeRoutesVtecToDeparted) {
    BuilderFixture f;   // SH column (1,0,'C') registered present in the fixture
    LocalCarry lc;
    lc.value_abs = 0.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::VtecSh, "", "", 1, 0, 'C',
                                  /*sh_layer=*/0,
                                  /*coeff=*/0.3, /*s_prev=*/0.0, /*p_prev=*/0.5} };
    auto cb = f.make(/*joint=*/true, /*tu=*/false, /*shmarg=*/true);
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.0, lc, /*var_eff=*/0.02, /*gap=*/30.0);

    ASSERT_EQ(cb.craw.size(), 1u);
    EXPECT_TRUE(cb.craw[0].shared.empty());                // SH term not kept present
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
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', /*sh_layer=*/0,
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
    lc.coupling = { CoupledShared{SharedParamKind::PhaseBias, "G01", "L1C", 0, 0, 'C', /*sh_layer=*/0,
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

// --- phaseBiasShiftOf: the eq34 D4 §14 re-base, through the traversal ---------
// CoupledShared::s_prev and CarryPrior::value are the same number for the same
// identity, so the D4 v2-shift that applies `cp.value += s` applies the same `+s` to
// every matching coupling term and to nothing else (card PRM phase 2: one rule,
// phaseBiasShiftOf, applied to every reference eq34ShiftCarry walks).

// Selectivity: only the PhaseBias entry whose (sat, signal) is in shift_sat
// moves, by exactly +s, and only s_prev moves (coeff / p_prev untouched).
TEST(NetworkCouplingTest, Eq34RebaseShiftsOnlyMatchingPhaseBiasSprev) {
    NetworkCarryState carry;
    LocalCarry lc{0.0, 1e-4, 100.0};
    lc.coupling = {
        CoupledShared{SharedParamKind::PhaseBias, "G05", "L1C", 0, 0, 'C', 0, 0.25, 1.5, 0.01},
        CoupledShared{SharedParamKind::PhaseBias, "G07", "L1C", 0, 0, 'C', 0, 0.25, 1.5, 0.01},  // other sat
        CoupledShared{SharedParamKind::PhaseBias, "G05", "L2W", 0, 0, 'C', 0, 0.25, 1.5, 0.01},  // other signal
        CoupledShared{SharedParamKind::Clock,     "G05", "",    0, 0, 'C', 0, 0.50, 3.0, 0.02},  // clock, shifted sat
        CoupledShared{SharedParamKind::CodeBias,  "G05", "L1C", 0, 0, 'C', 0, 0.10, -2.0, 0.03}, // code bias, same key
        CoupledShared{SharedParamKind::VtecSh,    "",    "",    2, 1, 'S', 0, 0.05, 4.0, 0.04},  // SH coefficient
    };
    carry.zwd_carry[0] = lc;
    const std::map<std::pair<std::string, std::string>, double> shift{{{"G05", "L1C"}, 0.75}};

    long long n_values = 0, n_terms = 0;
    eq34ShiftCarry(carry, shift, {}, {}, n_values, n_terms);
    EXPECT_EQ(n_terms, 1);
    EXPECT_EQ(n_values, 0);
    const auto& cpl = carry.zwd_carry[0].coupling;
    EXPECT_DOUBLE_EQ(cpl[0].s_prev, 2.25);   // 1.5 + 0.75, same sign as cp.value += s
    EXPECT_DOUBLE_EQ(cpl[0].coeff, 0.25);
    EXPECT_DOUBLE_EQ(cpl[0].p_prev, 0.01);
    EXPECT_DOUBLE_EQ(cpl[1].s_prev, 1.5);
    EXPECT_DOUBLE_EQ(cpl[2].s_prev, 1.5);
    EXPECT_DOUBLE_EQ(cpl[3].s_prev, 3.0);
    EXPECT_DOUBLE_EQ(cpl[4].s_prev, -2.0);
    EXPECT_DOUBLE_EQ(cpl[5].s_prev, 4.0);
}

// Guards mirror the prev_carry walk: a zero or non-finite shift is skipped (not
// applied, not counted), and empty inputs move nothing.
TEST(NetworkCouplingTest, Eq34RebaseSkipsZeroAndNonFiniteShifts) {
    const std::map<std::pair<std::string, std::string>, double> shift{
        {{"G05", "L1C"}, 0.0},
        {{"G07", "L1C"}, std::numeric_limits<double>::quiet_NaN()},
        {{"G09", "L1C"}, -0.5},
    };
    EXPECT_EQ(phaseBiasShiftOf(shift, "G05", "L1C"), 0.0);
    EXPECT_EQ(phaseBiasShiftOf(shift, "G07", "L1C"), 0.0);
    EXPECT_EQ(phaseBiasShiftOf(shift, "G09", "L1C"), -0.5);
    EXPECT_EQ(phaseBiasShiftOf(shift, "G09", "L2W"), 0.0);

    NetworkCarryState carry;
    LocalCarry lc{0.0, 1e-4, 100.0};
    lc.coupling = {
        CoupledShared{SharedParamKind::PhaseBias, "G05", "L1C", 0, 0, 'C', 0, 0.25, 1.5, 0.01},
        CoupledShared{SharedParamKind::PhaseBias, "G07", "L1C", 0, 0, 'C', 0, 0.25, 1.5, 0.01},
        CoupledShared{SharedParamKind::PhaseBias, "G09", "L1C", 0, 0, 'C', 0, 0.25, 1.5, 0.01},
    };
    carry.zwd_carry[0] = lc;
    long long n_values = 0, n_terms = 0;
    eq34ShiftCarry(carry, shift, {}, {}, n_values, n_terms);
    EXPECT_EQ(n_terms, 1);
    const auto& cpl = carry.zwd_carry[0].coupling;
    EXPECT_DOUBLE_EQ(cpl[0].s_prev, 1.5);
    EXPECT_DOUBLE_EQ(cpl[1].s_prev, 1.5);
    EXPECT_DOUBLE_EQ(cpl[2].s_prev, 1.0);

    NetworkCarryState empty_carry;
    eq34ShiftCarry(empty_carry, shift, {}, {}, n_values, n_terms);
    EXPECT_EQ(n_values + n_terms, 0);
    eq34ShiftCarry(carry, {}, {}, {}, n_values, n_terms);
    EXPECT_EQ(n_values + n_terms, 0);
    EXPECT_DOUBLE_EQ(cpl[2].s_prev, 1.0);    // the empty shift touched nothing
}

// --- Re-admission re-coupling edge (network.readmit_coupling_marg_ep) --------------
//
// A satellite inside its post-re-admission window takes the departed route even
// though its column is present and bound: the row drops the coefficient and adds
// M²·P_prev to σ² (the q=∞ marginalization), so no carried row conditions on a
// column whose realization is still settling after its cold re-registration.
// Outside the window (expired, another satellite, null map) the row is the legacy
// present-term row bit-for-bit, and an SH column (no satellite) is never diverted.
// The joint path (card 1b) routes the same term to an M_w departed column.
TEST(NetworkCouplingTest, ReadmitWindowDivertsThePresentColumnAndExpires) {
    LocalCarry lc;
    lc.value_abs = 2.0;
    lc.last_seen_s = 0.0;
    lc.coupling = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', 0,
                                  /*coeff=*/0.8, /*s_prev=*/1.5, /*p_prev=*/0.2} };
    const double var = 0.01;

    BuilderFixture off_f;
    auto off = off_f.make(/*joint=*/false, /*tu=*/false, /*shmarg=*/false);
    off.buildCplRow(/*col=*/0, /*x_lin_val=*/0.5, lc, var, 30.0);
    ASSERT_EQ(off.crows.size(), 1u);
    ASSERT_EQ(off.crows[0].shared.size(), 1u);
    EXPECT_EQ(off.crows[0].w, 1.0 / var);
    EXPECT_EQ(off_f.R.cpl_readmit_diverted, 0);

    // Inside the window: loop epoch 10 < expiry 12.
    std::map<std::string, long long> marg{{"G01", 12}};
    BuilderFixture in_f;
    auto in = in_f.make(false, false, false);
    in.readmit_marg = &marg;
    in.loop_ep      = 10;
    in.buildCplRow(0, 0.5, lc, var, 30.0);
    ASSERT_EQ(in.crows.size(), 1u);
    EXPECT_TRUE(in.crows[0].shared.empty()) << "the present column must be dropped";
    EXPECT_DOUBLE_EQ(in.crows[0].w, 1.0 / (var + 0.8 * 0.8 * 0.2));
    EXPECT_DOUBLE_EQ(in.crows[0].y, 2.0 - 0.5) << "no s_prev term without the coefficient";
    EXPECT_EQ(in_f.R.cpl_readmit_diverted, 1);
    EXPECT_EQ(in_f.R.cpl_terms_departed, 1);
    EXPECT_EQ(in_f.R.cpl_terms_present, 0);

    // Expired (loop epoch == expiry): the legacy row again.
    BuilderFixture ex_f;
    auto ex = ex_f.make(false, false, false);
    ex.readmit_marg = &marg;
    ex.loop_ep      = 12;
    ex.buildCplRow(0, 0.5, lc, var, 30.0);
    ASSERT_EQ(ex.crows.size(), 1u);
    EXPECT_EQ(ex.crows[0].w, off.crows[0].w);
    EXPECT_EQ(ex.crows[0].y, off.crows[0].y);
    ASSERT_EQ(ex.crows[0].shared.size(), 1u);
    EXPECT_EQ(ex_f.R.cpl_readmit_diverted, 0);

    // Another satellite's window: untouched.
    std::map<std::string, long long> other{{"G02", 100}};
    BuilderFixture ot_f;
    auto ot = ot_f.make(false, false, false);
    ot.readmit_marg = &other;
    ot.loop_ep      = 10;
    ot.buildCplRow(0, 0.5, lc, var, 30.0);
    ASSERT_EQ(ot.crows.size(), 1u);
    EXPECT_EQ(ot.crows[0].w, off.crows[0].w);
    EXPECT_EQ(ot_f.R.cpl_readmit_diverted, 0);

    // An SH column has no satellite: never diverted, even by an empty-sat entry.
    LocalCarry lsh;
    lsh.value_abs = 1.0;
    lsh.last_seen_s = 0.0;
    lsh.coupling = { CoupledShared{SharedParamKind::VtecSh, "", "", 1, 0, 'C', 0,
                                   /*coeff=*/0.3, /*s_prev=*/2.0, /*p_prev=*/0.05} };
    std::map<std::string, long long> empty_sat{{"", 100}};
    BuilderFixture sh_f;
    auto sh = sh_f.make(false, false, false);
    sh.readmit_marg = &empty_sat;
    sh.loop_ep      = 1;
    sh.buildCplRow(0, 0.0, lsh, var, 30.0);
    ASSERT_EQ(sh.crows.size(), 1u);
    EXPECT_EQ(sh.crows[0].shared.size(), 1u);
    EXPECT_EQ(sh_f.R.cpl_readmit_diverted, 0);

    // Joint path (card 1b): the diverted term becomes an M_w departed column.
    BuilderFixture jf;
    auto jb = jf.make(/*joint=*/true, false, false);
    jb.readmit_marg = &marg;
    jb.loop_ep      = 10;
    jb.buildCplRow(0, 0.5, lc, var, 30.0);
    ASSERT_EQ(jb.craw.size(), 1u);
    EXPECT_TRUE(jb.craw[0].shared.empty());
    ASSERT_EQ(jb.craw[0].departed.size(), 1u);
    EXPECT_EQ(jf.R.cpl_readmit_diverted, 1);
}
