// Unit tests for the standalone NetworkCarryVce accumulator
// (include/network/carry/CarryVce.hpp), the residual-form variance-component
// estimator into which the network producer feeds carry-row innovations (M2, the
// LS-VCE carry-q monitor).  The tests drive the class directly with synthetic
// carry rows of known truth, as test_neq_accumulator and the M4 combined-sigma
// test do for their classes; there is no ApplicationNetwork coupling.
//
//   Gate 1 (recovery):  a deterministic 2-component fixture with known
//                       (floor σ0, rate q); the estimator recovers the floor
//                       within 5 % and the rate within 20 %, and yields no
//                       negative component.
//   Gate 2 (gating):    the same fixture plus an injected level shift; the
//                       gated accumulator (contaminated epochs marked dirty)
//                       stays unbiased, while the ungated one is biased high,
//                       which is the reason for the clean-epoch gate.

#include "network/carry/CarryVce.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// One synthetic clean carry row under the "config == truth" BLUE scenario, so
// the fixed-weight estimator is unbiased for the true components.  Given the
// true carry variance CV = floor + rate*gap and a data information I_data, the
// solve posterior is v = 1/(1/CV + I_data) and the residual redundancy is
// r = 1 - v/CV; the post-fit innovation is drawn ~ N(0, r*CV) (BLUE residual).
struct Row {
    double innov, gap, post_var, fold_var;
};

Row makeRow(double floor_true, double rate_true, double gap, double i_data,
            std::mt19937& gen) {
    const double cv   = floor_true + rate_true * gap;   // true carry variance (= fold, config==truth)
    const double v    = 1.0 / (1.0 / cv + i_data);       // posterior variance after the solve
    const double r    = 1.0 - v / cv;                     // partial redundancy in (0,1)
    const double sd   = std::sqrt(r * cv);                // Var(residual) = r*CV (BLUE)
    std::normal_distribution<double> nrm(0.0, sd);
    Row row;
    row.innov    = nrm(gen);
    row.gap      = gap;
    row.post_var = v;
    row.fold_var = cv;
    return row;
}

// A spread of arc gaps (s) so the Floor (cofactor 1) and Rate (cofactor gap)
// columns are separable, and a spread of data informations so redundancy varies.
const std::vector<double> kGaps   = {30.0, 60.0, 90.0, 120.0};
const std::vector<double> kIData  = {5.0e3, 2.0e4, 8.0e4};   // m^-2

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1: recover (floor, rate) from a clean 2-component fixture.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVce, RecoversTwoComponentsFromCleanFixture) {
    const double floor_true = 4.0e-5;   // m^2   (golden-class PL floor)
    const double rate_true  = 3.0e-7;   // m^2/s (nonzero so it is identifiable)

    // The a-priori seed is wrong on purpose (5x the true floor, zero rate), so
    // the test shows that the data, not the prior, drives the estimate.
    NetworkCarryVce vce(/*families=*/{"pl"}, /*window_epochs=*/0.0,
                        /*split_by_const=*/true,
                        /*seed_floor=*/2.0e-4, /*seed_rate=*/0.0);

    std::mt19937 gen(12345u);
    const int n_epochs = 3000, rows_per_epoch = 24;
    int gi = 0, di = 0;
    for (int e = 0; e < n_epochs; ++e) {
        vce.beginEpoch(/*content_time_s=*/e * 30.0, /*clean=*/true);
        for (int k = 0; k < rows_per_epoch; ++k) {
            const double gap   = kGaps[gi++ % kGaps.size()];
            const double idata = kIData[di++ % kIData.size()];
            const Row row = makeRow(floor_true, rate_true, gap, idata, gen);
            vce.addRow("pl", "G01", row.innov, row.gap, row.post_var, row.fold_var);
        }
    }

    const NetworkCarryVce::Estimate est = vce.estimate("pl", "G01");

    // Positivity guarantee (v1 clamp): neither component is negative.
    EXPECT_GT(est.sigma[NetworkCarryVce::kFloor], 0.0);
    EXPECT_GE(est.sigma[NetworkCarryVce::kRate], 0.0);
    EXPECT_EQ(est.floor_clamped, 0);   // a well-posed fixture never binds the clamp
    EXPECT_EQ(est.rate_clamped, 0);

    // Both components recovered (Monte-Carlo with 72k rows is tight; the fixed
    // seed makes the pass deterministic).
    EXPECT_NEAR(est.sigma[NetworkCarryVce::kFloor], floor_true, 0.05 * floor_true)
        << "floor sigma-hat = " << est.sigma[NetworkCarryVce::kFloor];
    EXPECT_NEAR(est.sigma[NetworkCarryVce::kRate], rate_true, 0.20 * rate_true)
        << "rate sigma-hat = " << est.sigma[NetworkCarryVce::kRate];

    // The normal matrix is well-conditioned with this gap spread ⇒ rate not frozen.
    EXPECT_FALSE(est.rate_frozen) << "cond = " << est.cond;
    EXPECT_GT(est.n_rows, 0);
}

// ---------------------------------------------------------------------------
// Gate 1b: constant gaps make Floor and Rate collinear.  The estimator stays
// well-behaved instead of diverging: the rate is frozen (held near its a-priori)
// and non-negative, and the Floor is still recovered.  This exercises the
// separability guard.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVce, ConstantGapsFreezeRateButRecoverFloor) {
    const double floor_true = 4.0e-5;
    NetworkCarryVce vce({"pl"}, /*window=*/0.0, /*split=*/true,
                        /*seed_floor=*/4.0e-5, /*seed_rate=*/0.0);
    std::mt19937 gen(777u);
    int di = 0;
    for (int e = 0; e < 3000; ++e) {
        vce.beginEpoch(e * 30.0, true);
        for (int k = 0; k < 24; ++k) {
            const double idata = kIData[di++ % kIData.size()];
            const Row row = makeRow(floor_true, /*rate_true=*/0.0, /*gap=*/60.0, idata, gen);
            vce.addRow("pl", "G17", row.innov, row.gap, row.post_var, row.fold_var);
        }
    }
    const auto est = vce.estimate("pl", "G17");
    EXPECT_TRUE(est.rate_frozen) << "cond = " << est.cond;   // collinear ⇒ frozen
    EXPECT_GT(est.sigma[NetworkCarryVce::kFloor], 0.0);
    // Floor is still identifiable from the constant-gap rows.
    EXPECT_NEAR(est.sigma[NetworkCarryVce::kFloor], floor_true, 0.08 * floor_true)
        << "floor sigma-hat = " << est.sigma[NetworkCarryVce::kFloor];
    EXPECT_GE(est.sigma[NetworkCarryVce::kRate], 0.0);       // never negative
}

// ---------------------------------------------------------------------------
// Gate 2: contaminated fixture; ungated is biased high, gated stays unbiased.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVce, CleanEpochGatePreventsLevelShiftBias) {
    const double floor_true = 4.0e-5;
    const double rate_true  = 3.0e-7;
    const double shift_m    = 0.30;   // a re-level: 30 cm carry jump (many sigma)

    // Two estimators fed identical row streams; they differ only in whether the
    // contaminated epochs are marked clean.
    NetworkCarryVce ungated({"pl"}, 0.0, true, 4.0e-5, 0.0);
    NetworkCarryVce gated  ({"pl"}, 0.0, true, 4.0e-5, 0.0);

    std::mt19937 gen(24680u);
    int gi = 0, di = 0;
    const int n_epochs = 3000, rows_per_epoch = 24;
    // Every 20th epoch is a "datum reshuffle": its rows carry the level shift.
    for (int e = 0; e < n_epochs; ++e) {
        const bool dirty = (e % 20 == 0);
        ungated.beginEpoch(e * 30.0, /*clean=*/true);       // ungated trusts every epoch
        gated.beginEpoch(e * 30.0, /*clean=*/!dirty);        // gated drops dirty epochs
        for (int k = 0; k < rows_per_epoch; ++k) {
            const double gap   = kGaps[gi++ % kGaps.size()];
            const double idata = kIData[di++ % kIData.size()];
            Row row = makeRow(floor_true, rate_true, gap, idata, gen);
            if (dirty) row.innov += shift_m;                 // inject the re-level
            ungated.addRow("pl", "G01", row.innov, row.gap, row.post_var, row.fold_var);
            gated.addRow  ("pl", "G01", row.innov, row.gap, row.post_var, row.fold_var);
        }
    }

    const auto eU = ungated.estimate("pl", "G01");
    const auto eG = gated.estimate("pl", "G01");

    // Ungated: the 5% of contaminated rows carry a huge weighted residual and
    // inflate the Floor estimate far above truth.
    EXPECT_GT(eU.sigma[NetworkCarryVce::kFloor], 3.0 * floor_true)
        << "ungated floor = " << eU.sigma[NetworkCarryVce::kFloor];

    // Gated: the contaminated epochs never entered the accumulator, so the Floor
    // estimate is back at truth.
    EXPECT_NEAR(eG.sigma[NetworkCarryVce::kFloor], floor_true, 0.06 * floor_true)
        << "gated floor = " << eG.sigma[NetworkCarryVce::kFloor];

    // And the gate demonstrably matters: gated is far below ungated.
    EXPECT_LT(eG.sigma[NetworkCarryVce::kFloor], 0.5 * eU.sigma[NetworkCarryVce::kFloor]);
}

// ---------------------------------------------------------------------------
// Robust (geometric-mean) floor: recovers the innovation variance and is
// insensitive to a heavy outlier tail.  That property makes it the primary field
// statistic; the arithmetic mean is dominated by rare frame-churn re-levels.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVce, RobustFloorRecoversVarianceAndResistsOutliers) {
    const double s_true = 5.0e-4;   // m^2, the true innovation variance (sigma ~2.2 cm)

    NetworkCarryVce clean({"pl"}, 0.0, true, 4.0e-5, 0.0);
    NetworkCarryVce tailed({"pl"}, 0.0, true, 4.0e-5, 0.0);
    std::mt19937 gen(9090u);
    std::normal_distribution<double> nrm(0.0, std::sqrt(s_true));
    const int N = 60000;
    for (int i = 0; i < N; ++i) {
        // Both estimators see the same clean draw; on 1 % of rows `tailed` gets a
        // 0.5 m innovation instead (a re-level; ~22 sigma, 500x the variance).
        const double e = nrm(gen);
        // fold_var/post_var are arbitrary-but-valid; they do not enter robust_floor.
        clean.beginEpoch(i * 1.0, true);
        tailed.beginEpoch(i * 1.0, true);
        clean.addRow("pl", "G01", e, 60.0, 1.0e-5, 4.0e-5);
        const double e_t = (i % 100 == 0) ? 0.5 : e;
        tailed.addRow("pl", "G01", e_t, 60.0, 1.0e-5, 4.0e-5);
    }
    const auto ec = clean.estimate("pl", "G01");
    const auto et = tailed.estimate("pl", "G01");

    // Clean: the geometric-mean estimator recovers the true innovation variance.
    EXPECT_NEAR(ec.robust_floor, s_true, 0.05 * s_true)
        << "robust_floor = " << ec.robust_floor;

    // Tailed: 1% of 0.5 m re-levels barely move the robust floor (<2x)...
    EXPECT_LT(et.robust_floor, 2.0 * s_true)
        << "robust_floor with outliers = " << et.robust_floor;
    // ...while the arithmetic-mean floor is inflated by them (here ~8x) and ends
    // up several times the robust floor.  That is why the robust statistic is the
    // field primary and the mean is the event-inclusive companion.
    EXPECT_GT(et.sigma[NetworkCarryVce::kFloor], 3.0 * s_true)
        << "mean floor with outliers = " << et.sigma[NetworkCarryVce::kFloor];
    EXPECT_GT(et.sigma[NetworkCarryVce::kFloor], 3.0 * et.robust_floor)
        << "mean " << et.sigma[NetworkCarryVce::kFloor]
        << " should dwarf robust " << et.robust_floor;
}

// ---------------------------------------------------------------------------
// Component-list parsing (the network.carry_vce_components knob).
// ---------------------------------------------------------------------------
TEST(NetworkCarryVce, ParsesComponentList) {
    auto a = NetworkCarryVce::parseComponents("pl,pb,cb");
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[0], "pl"); EXPECT_EQ(a[1], "pb"); EXPECT_EQ(a[2], "cb");
    auto b = NetworkCarryVce::parseComponents("  pl ");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], "pl");
    auto c = NetworkCarryVce::parseComponents("");
    EXPECT_TRUE(c.empty());
}

}  // namespace GnssProcessor
