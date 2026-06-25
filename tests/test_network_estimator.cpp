/**
 * @file test_network_estimator.cpp
 * @brief Synthetic-truth recovery tests for the single-epoch network estimator
 *        (W4-S2a) — the plan's "synthetic-network simulation (known truth)"
 *        verification milestone.
 *
 * A noise-free, zero-atmosphere, single-frequency network is generated from
 * prescribed truth (per-satellite clock + phase bias; per-station clock +
 * float-ambiguity lumps).  Under the explicit minimum-constraint datum — a
 * SHARED pin on the reference satellite's clock (c_{s0}=0, exercising the
 * solveSharedConstrained S-basis seam) plus LOCAL pins on the reference
 * receiver's ambiguities (M_{0,s}=0, folded into that station's rows) — the
 * estimator must recover the satellite-side parameters EXACTLY up to that
 * declared S-transform:
 *     c_s   -> c_s   - c_{s0}
 *     b_s   -> b_s   + M_{0,s}     (the pivot-receiver ambiguity lump, Odijk Eq. 4)
 *     a_r   -> a_r   - c_{s0}
 *     M_{r,s} -> M_{r,s} - M_{0,s}
 *
 * Observation model (geometry/atmosphere removed; single frequency so the
 * satellite code bias folds into the clock):
 *     code  P_{r,s} = a_r - c_s
 *     phase Φ_{r,s} = a_r - c_s + b_s + M_{r,s}
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <random>
#include <string>
#include <vector>

#include "filter/NetworkEstimator.hpp"

namespace GnssProcessor {
namespace {

struct Truth {
    int R, S;
    std::vector<double> a;                 // a_r        [R]
    std::vector<double> c;                 // c_s        [S]
    std::vector<double> b;                 // b_s        [S]
    std::vector<std::vector<double>> M;    // M_{r,s}    [R][S]
    std::vector<std::string> sats;         // RINEX ids  [S]
};

Truth makeTruth(unsigned seed, int R, int S) {
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> clk(-10.0, 10.0);
    std::uniform_real_distribution<double> bias(-1.0, 1.0);
    std::uniform_real_distribution<double> amb(-5.0, 5.0);
    Truth t;
    t.R = R; t.S = S;
    for (int r = 0; r < R; ++r) t.a.push_back(clk(gen));
    for (int s = 0; s < S; ++s) {
        t.c.push_back(clk(gen));
        t.b.push_back(bias(gen));
        char id[8];
        std::snprintf(id, sizeof(id), "G%02d", s + 1);
        t.sats.push_back(id);
    }
    t.M.assign(R, std::vector<double>(S));
    for (int r = 0; r < R; ++r)
        for (int s = 0; s < S; ++s) t.M[r][s] = amb(gen);
    return t;
}

// Assemble one station's pre-formed design rows.  The pivot receiver (r==0)
// also gets S tight-prior rows pinning its ambiguities M_{0,s}=0 (the local
// half of the datum).
void buildStation(const Truth& t, int r, NetworkEstimator& est,
                  const std::vector<Eigen::Index>& clkCol,
                  const std::vector<Eigen::Index>& pbCol,
                  Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                  Eigen::VectorXd& y) {
    const int S = t.S;
    const Eigen::Index n_local = 1 + S;          // [a_r, M_{r,0..S-1}]
    const bool pivot = (r == 0);
    const Eigen::Index m = 2 * S + (pivot ? S : 0);
    A_s = Eigen::MatrixXd::Zero(m, est.numShared());
    A_r = Eigen::MatrixXd::Zero(m, n_local);
    w = Eigen::VectorXd::Ones(m);
    y = Eigen::VectorXd::Zero(m);

    Eigen::Index row = 0;
    for (int s = 0; s < S; ++s) {
        // code: P = a_r - c_s
        A_s(row, clkCol[s]) = -1.0;
        A_r(row, 0) = 1.0;
        y(row) = t.a[r] - t.c[s];
        ++row;
        // phase: Φ = a_r - c_s + b_s + M_{r,s}
        A_s(row, clkCol[s]) = -1.0;
        A_s(row, pbCol[s]) = 1.0;
        A_r(row, 0) = 1.0;
        A_r(row, 1 + s) = 1.0;
        y(row) = t.a[r] - t.c[s] + t.b[s] + t.M[r][s];
        ++row;
    }
    if (pivot) {
        for (int s = 0; s < S; ++s) {
            A_r(row, 1 + s) = 1.0;   // pin M_{0,s} = 0
            y(row) = 0.0;
            w(row) = 1e10;
            ++row;
        }
    }
}

// Headline: the estimator recovers the satellite-side truth up to the S-transform.
TEST(NetworkEstimatorTest, RecoversSyntheticTruthUpToSTransform) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    ASSERT_EQ(est.numShared(), 2 * t.S);

    est.beginEpoch();
    std::vector<std::size_t> ids;
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        ids.push_back(est.addStation(A_s, A_r, w, y));
    }

    // Datum: SHARED pin on the reference satellite's clock (c_{s0}=0).
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);

    const double c0 = t.c[0];
    for (int s = 0; s < t.S; ++s) {
        EXPECT_NEAR(x_s(clkCol[s]), t.c[s] - c0, 1e-6) << "sat clock " << s;
        EXPECT_NEAR(x_s(pbCol[s]), t.b[s] + t.M[0][s], 1e-6) << "sat phase bias " << s;
    }
    EXPECT_NEAR(x_s(clkCol[0]), 0.0, 1e-6);  // datum: reference sat clock pinned

    for (int r = 0; r < t.R; ++r) {
        const Eigen::VectorXd xr = est.reconstructStation(ids[r], x_s);
        EXPECT_NEAR(xr(0), t.a[r] - c0, 1e-6) << "rx clock " << r;
        for (int s = 0; s < t.S; ++s)
            EXPECT_NEAR(xr(1 + s), t.M[r][s] - t.M[0][s], 1e-6)
                << "ambiguity r=" << r << " s=" << s;
    }
    // Pivot receiver's ambiguities are exactly the datum (zero).
    const Eigen::VectorXd x0 = est.reconstructStation(ids[0], x_s);
    for (int s = 0; s < t.S; ++s) EXPECT_NEAR(x0(1 + s), 0.0, 1e-6);
}

// Multi-pin datum: a second, independent gauge (an UNOBSERVED satellite phase
// bias whose column is zero in every station) is rank-deficient on its own and
// must be fixed by its own pin.  Exercises the multi-pin hard-pin elimination
// (RHS moves across several pins) and confirms the extra pin does not perturb
// the recovered satellite-side truth.
TEST(NetworkEstimatorTest, MultiPinDatumWithIndependentGauge) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    // A ghost satellite observed by nobody → its column is zero everywhere.
    const Eigen::Index ghostCol = est.registerPhaseBias("G99", signal);

    est.beginEpoch();
    std::vector<std::size_t> ids;
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);  // sizes A_s to numShared → ghost col is 0
        ids.push_back(est.addStation(A_s, A_r, w, y));
    }

    const double ghost_value = -0.123;
    const NetworkEstimator::Datum datum = {
        {est.clockColumn(t.sats[0]), 0.0},
        {ghostCol, ghost_value},
    };
    const Eigen::VectorXd x_s = est.solve(datum);

    EXPECT_NEAR(x_s(ghostCol), ghost_value, 1e-9);  // the independent gauge is fixed exactly
    const double c0 = t.c[0];
    for (int s = 0; s < t.S; ++s) {                  // and the real solution is unperturbed
        EXPECT_NEAR(x_s(clkCol[s]), t.c[s] - c0, 1e-6) << "sat clock " << s;
        EXPECT_NEAR(x_s(pbCol[s]), t.b[s] + t.M[0][s], 1e-6) << "sat phase bias " << s;
    }
}

// The shared solution maps onto an emittable NetworkSolutionEpoch correctly.
TEST(NetworkEstimatorTest, ToSolutionEpochMapsColumns) {
    NetworkEstimator est;
    const Eigen::Index g1_clk = est.registerClock("G01");
    const Eigen::Index g1_pb = est.registerPhaseBias("G01", "L1");
    const Eigen::Index g1_cb = est.registerCodeBias("G01", "C1C");
    const Eigen::Index e11_clk = est.registerClock("E11");

    Eigen::VectorXd x_s(est.numShared());
    x_s(g1_clk) = 1.5;
    x_s(g1_pb) = -0.3;
    x_s(g1_cb) = 0.07;
    x_s(e11_clk) = 2.25;

    const GnssTime epoch{2300, 345600.0};
    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, epoch, /*iod=*/4);

    EXPECT_EQ(sol.epoch_time.week, 2300);
    EXPECT_EQ(sol.iod_ssr, 4);
    ASSERT_EQ(sol.sats.size(), 2u);

    // Find G01 (order is first-seen, so index 0).
    const NetworkSatSolution& g1 = sol.sats[0];
    EXPECT_EQ(g1.sat, "G01");
    EXPECT_TRUE(g1.has_clock);
    EXPECT_NEAR(g1.clock_c0_m, 1.5, 1e-12);
    ASSERT_EQ(g1.phase_bias.count("L1"), 1u);
    EXPECT_NEAR(g1.phase_bias.at("L1").bias_m, -0.3, 1e-12);
    ASSERT_EQ(g1.code_bias_m.count("C1C"), 1u);
    EXPECT_NEAR(g1.code_bias_m.at("C1C"), 0.07, 1e-12);

    const NetworkSatSolution& e11 = sol.sats[1];
    EXPECT_EQ(e11.sat, "E11");
    EXPECT_NEAR(e11.clock_c0_m, 2.25, 1e-12);
    EXPECT_TRUE(e11.phase_bias.empty());
}

// Registration is idempotent and rejected once the epoch is open.
TEST(NetworkEstimatorTest, LayoutIdempotentAndFrozenAfterBeginEpoch) {
    NetworkEstimator est;
    const Eigen::Index a = est.registerClock("G01");
    const Eigen::Index b = est.registerClock("G01");  // same column
    EXPECT_EQ(a, b);
    EXPECT_EQ(est.numShared(), 1);

    est.beginEpoch();
    EXPECT_THROW(est.registerClock("G02"), std::logic_error);
}

// An under-specified datum (leaves a rank deficiency) surfaces as a clean throw.
TEST(NetworkEstimatorTest, UnderSpecifiedDatumThrows) {
    const Truth t = makeTruth(/*seed=*/7, /*R=*/3, /*S=*/4);
    const std::string signal = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    // No datum at all → the reduced shared clock datum is still free → singular.
    EXPECT_THROW(est.solve({}), std::invalid_argument);
}

// PARTIAL under-specification: a datum that fixes the clock gauge but leaves an
// independent exact gauge (the unobserved ghost satellite) unpinned must still
// throw — the positive-definiteness guard catches the remaining exact deficiency
// even amongst other (well-conditioned) pins, regardless of weighting scale.
TEST(NetworkEstimatorTest, PartialUnderSpecifiedDatumThrows) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    est.registerPhaseBias("G99", signal);  // ghost gauge, observed by nobody

    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    // Pin ONLY the clock gauge; leave the ghost gauge unfixed → still singular.
    EXPECT_THROW(est.solve({{est.clockColumn(t.sats[0]), 0.0}}), std::invalid_argument);
}

// Adding a station after solve() is rejected (the accumulation is frozen until
// the next beginEpoch()), so a stale post-solve estimate cannot be silently
// corrupted.
TEST(NetworkEstimatorTest, AddStationAfterSolveThrows) {
    const Truth t = makeTruth(/*seed=*/11, /*R=*/3, /*S=*/4);
    const std::string signal = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    est.beginEpoch();
    std::vector<std::pair<Eigen::MatrixXd, Eigen::MatrixXd>> blocks;
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    est.solve({{est.clockColumn(t.sats[0]), 0.0}});

    Eigen::MatrixXd A_s, A_r;
    Eigen::VectorXd w, y;
    buildStation(t, 1, est, clkCol, pbCol, A_s, A_r, w, y);
    EXPECT_THROW(est.addStation(A_s, A_r, w, y), std::logic_error);
    // A fresh epoch re-opens accumulation.
    est.beginEpoch();
    EXPECT_NO_THROW(est.addStation(A_s, A_r, w, y));
}

} // namespace
} // namespace GnssProcessor
