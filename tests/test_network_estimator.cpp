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

#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkSbasis.hpp"

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

// RT0-3: toSolutionEpoch flags CDMA (GPS/GAL/BDS/QZSS) satellite phase biases as
// integer-preserving (signal_integer=true) and leaves GLONASS/FDMA ('R') biases
// false — truthful metadata a consumer's AR candidate selection can consult before
// FDMA (per-slot inter-frequency-bias) biases ever enter the product.
TEST(NetworkEstimatorTest, ToSolutionEpochSetsSignalIntegerForCdmaOnly) {
    NetworkEstimator est;
    const Eigen::Index g_pb = est.registerPhaseBias("G01", "L1");
    const Eigen::Index e_pb = est.registerPhaseBias("E11", "L1");
    const Eigen::Index c_pb = est.registerPhaseBias("C08", "L2");
    const Eigen::Index j_pb = est.registerPhaseBias("J02", "L1");   // QZSS / CDMA
    const Eigen::Index r_pb = est.registerPhaseBias("R05", "L1");   // GLONASS / FDMA

    Eigen::VectorXd x_s(est.numShared());
    x_s.setZero();
    x_s(g_pb) = 0.10; x_s(e_pb) = 0.20; x_s(c_pb) = 0.30; x_s(j_pb) = 0.35; x_s(r_pb) = 0.40;

    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, GnssTime{2300, 0.0});
    ASSERT_EQ(sol.sats.size(), 5u);

    auto findSat = [&](const std::string& id) -> const NetworkSatSolution& {
        for (const auto& s : sol.sats) if (s.sat == id) return s;
        ADD_FAILURE() << "sat not found: " << id;
        return sol.sats.front();
    };
    EXPECT_TRUE (findSat("G01").phase_bias.at("L1").signal_integer);  // GPS  → integer
    EXPECT_TRUE (findSat("E11").phase_bias.at("L1").signal_integer);  // GAL  → integer
    EXPECT_TRUE (findSat("C08").phase_bias.at("L2").signal_integer);  // BDS  → integer
    EXPECT_TRUE (findSat("J02").phase_bias.at("L1").signal_integer);  // QZSS → integer (CDMA)
    EXPECT_FALSE(findSat("R05").phase_bias.at("L1").signal_integer);  // GLO  → NOT integer (FDMA)
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

// The between-epoch D2 carry-forward seam (W4-S2b): sharedCovariance() exposes a
// well-formed posterior covariance, and addSharedPrior() (a diagonal prior on the
// shared border) actually moves the next epoch's estimate.  These are exactly the
// two mechanics the runNetwork() driver leans on for between-epoch continuity.
TEST(NetworkEstimatorTest, SharedCovarianceAndCarryForwardPrior) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    // Epoch 1: a plain solve, then inspect the shared covariance.
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    const Eigen::VectorXd x1 = est.solve(datum);
    const Eigen::MatrixXd cov = est.sharedCovariance();

    ASSERT_EQ(cov.rows(), est.numShared());
    ASSERT_EQ(cov.cols(), est.numShared());
    EXPECT_TRUE(cov.allFinite());
    EXPECT_NEAR((cov - cov.transpose()).cwiseAbs().maxCoeff(), 0.0, 1e-9);  // symmetric
    // The datum-pinned column is realized as a hard pin (N(col,col)=pin_scale), so
    // its covariance is 1/pin_scale — small relative to a free column, but not zero.
    EXPECT_GT(cov(clkCol[0], clkCol[0]), 0.0);
    EXPECT_GT(cov(clkCol[1], clkCol[1]), 0.0);
    EXPECT_LT(cov(clkCol[0], clkCol[0]), cov(clkCol[1], clkCol[1]));  // pinned tighter than free

    // Epoch 2: identical data, but a STRONG carry-forward prior pulls satellite
    // 1's clock toward a deliberately biased value.  The D2 seam must move it.
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    const double biased = x1(clkCol[1]) + 5.0;
    {
        NetworkEstimator::SharedPrior pr;
        pr.kind = SharedParamKind::Clock; pr.sat = t.sats[1];
        pr.value = biased; pr.info = 1e8;
        est.addSharedPrior({pr});
    }
    const Eigen::VectorXd x2 = est.solve(datum);

    EXPECT_GT(x2(clkCol[1]), x1(clkCol[1]) + 4.0);  // dominated by the strong prior
    EXPECT_NEAR(x2(clkCol[0]), 0.0, 1e-6);          // datum column still pinned
    // A prior identity-keyed to a satellite absent from the layout is ignored.
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    {
        NetworkEstimator::SharedPrior pr;  // not registered → must be a no-op
        pr.kind = SharedParamKind::Clock; pr.sat = "G99";
        pr.value = 99.0; pr.info = 1e8;
        est.addSharedPrior({pr});
    }
    const Eigen::VectorXd x3 = est.solve(datum);
    for (int s = 0; s < t.S; ++s)
        EXPECT_NEAR(x3(clkCol[s]), x1(clkCol[s]), 1e-6) << "absent-prior must be a no-op, sat " << s;
}

// ===========================================================================
// SharedParamKind::VtecSh — global SH-VTEC coefficient columns
// ===========================================================================

// registerVtecCoeff is idempotent and disjoint from clock/bias columns;
// vtecCoeffColumn round-trips; the prior routes to the right SH column; the SH
// columns are solved from their (Kaula) prior alone when they carry no data; and
// toSolutionEpoch does NOT manufacture a per-satellite slot for an SH column.
TEST(NetworkEstimatorTest, VtecShColumnsRegisterPriorAndSkipEmit) {
    NetworkEstimator est;
    const Eigen::Index cG1 = est.registerClock("G01");
    const Eigen::Index cG2 = est.registerClock("G02");
    // Degree-1 field: (0,0,C),(1,0,C),(1,1,C),(1,1,S).
    const Eigen::Index a00 = est.registerVtecCoeff(0, 0, 'C');
    const Eigen::Index a10 = est.registerVtecCoeff(1, 0, 'C');
    const Eigen::Index a11c = est.registerVtecCoeff(1, 1, 'C');
    const Eigen::Index a11s = est.registerVtecCoeff(1, 1, 'S');
    EXPECT_EQ(est.registerVtecCoeff(1, 1, 'C'), a11c);          // idempotent
    EXPECT_EQ(est.vtecCoeffColumn(0, 0, 'C'), a00);             // round-trip
    EXPECT_EQ(est.vtecCoeffColumn(1, 1, 'S'), a11s);
    EXPECT_EQ(est.numShared(), 6);
    // Disjoint namespaces: SH columns are distinct from clock columns.
    std::set<Eigen::Index> cols{cG1, cG2, a00, a10, a11c, a11s};
    EXPECT_EQ(cols.size(), 6u);

    est.beginEpoch();
    // One station observing G01/G02 by code: P = a_rx - c_sat (A_r = rx clock).
    // The SH columns carry NO data (A_s SH entries all zero).
    Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(2, 6);
    A_s(0, cG1) = -1.0;
    A_s(1, cG2) = -1.0;
    Eigen::MatrixXd A_r(2, 1);
    A_r << 1.0, 1.0;                       // receiver clock
    Eigen::VectorXd w(2); w << 1.0, 1.0;
    Eigen::VectorXd y(2); y << 3.0, 5.0;   // arbitrary prefit residuals
    est.addStation(A_s, A_r, w, y);

    // Kaula-style prior: a₀₀ → climatology 12 TECU, the rest → 0, all tight so the
    // data-free SH columns resolve to their prior mean.
    std::vector<NetworkEstimator::SharedPrior> pr;
    auto mk = [](int n, int m, char t, double v) {
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::VtecSh; p.sh_n = n; p.sh_m = m; p.sh_trig = t;
        p.value = v; p.info = 1.0e4;
        return p;
    };
    pr.push_back(mk(0, 0, 'C', 12.0));
    pr.push_back(mk(1, 0, 'C', 0.0));
    pr.push_back(mk(1, 1, 'C', 0.0));
    pr.push_back(mk(1, 1, 'S', 0.0));
    est.addSharedPrior(pr);

    NetworkEstimator::Datum datum{ NetworkEstimator::Pin{cG1, 0.0} };  // GPS clock gauge
    const Eigen::VectorXd x = est.solve(datum);

    EXPECT_NEAR(x(a00), 12.0, 1e-3);   // data-free SH column resolves to its prior mean
    EXPECT_NEAR(x(a10), 0.0, 1e-3);
    EXPECT_NEAR(x(a11c), 0.0, 1e-3);
    EXPECT_NEAR(x(a11s), 0.0, 1e-3);

    // toSolutionEpoch must NOT create an "SH" satellite — only G01/G02 appear.
    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x, GnssTime{2313, 259200.0});
    EXPECT_EQ(sol.sats.size(), 2u);
    for (const auto& s : sol.sats)
        EXPECT_TRUE(s.sat == "G01" || s.sat == "G02") << "unexpected sat " << s.sat;
}

// ===========================================================================
// NetworkSbasis (W5) — minimum-constraint datum builder + null-space rank gate
// ===========================================================================

// The shared datum is ONE satellite-clock pin per constellation (the clock gauge
// per timing system).  The phase-bias gauge is per (sat,signal) and is fixed by
// LOCAL pivot-receiver ambiguity pins, so it does NOT appear in the shared datum.
TEST(NetworkSbasisTest, BuildsOneClockPinPerConstellation) {
    NetworkEstimator est;
    est.registerClock("G01"); est.registerClock("G02");
    est.registerClock("E11"); est.registerClock("E12");
    est.registerPhaseBias("G01", "L1");  // phase columns coexist in the layout

    NetworkSbasis sb;
    SbasisDeclaration decl;
    const auto plan = sb.buildSharedDatum(est, {"G01", "G02", "E11", "E12"}, decl);

    EXPECT_EQ(plan.declared_dimension, 2);          // GPS + Galileo => 2 clock pins
    ASSERT_EQ(plan.shared_datum.size(), 2u);
    EXPECT_EQ(plan.clock_pivot.at("G"), "G01");     // first registered per constellation
    EXPECT_EQ(plan.clock_pivot.at("E"), "E11");

    std::set<Eigen::Index> cols;
    for (const auto& p : plan.shared_datum) { cols.insert(p.column); EXPECT_EQ(p.value, 0.0); }
    EXPECT_TRUE(cols.count(est.clockColumn("G01")));
    EXPECT_TRUE(cols.count(est.clockColumn("E11")));

    // A declared override picks a different pivot satellite for that constellation.
    decl.clock_pivot_sat["G"] = "G02";
    const auto plan2 = sb.buildSharedDatum(est, {"G01", "G02", "E11", "E12"}, decl);
    EXPECT_EQ(plan2.clock_pivot.at("G"), "G02");
    EXPECT_TRUE([&]{ for (const auto& p : plan2.shared_datum)
                       if (p.column == est.clockColumn("G02")) return true; return false; }());
}

// RTA-1: sticky clock-pivot selection.  Passing the driver's emit-baseline map
// keeps the incumbent pivot as long as it is still registered — a lexically
// LOWER satellite rising no longer flips the datum (the constellation-wide
// consumer reset the legacy per-epoch lexical pick forced).  Passing nullptr
// (the default) preserves the legacy lexical selection byte-identically.
TEST(NetworkSbasisTest, StickyPivotKeepsIncumbentWhenLexicallyLowerRises) {
    NetworkEstimator est;
    est.registerClock("G01"); est.registerClock("G03"); est.registerClock("G05");
    est.registerClock("E11"); est.registerClock("E12");

    NetworkSbasis sb;
    SbasisDeclaration decl;
    const std::vector<std::string> sats{"G01", "G03", "G05", "E11", "E12"};
    // Incumbent G05 (e.g. it was the first sighting); G01/G03 have since risen.
    const std::map<std::string, std::string> incumbent{{"G", "G05"}};

    // Sticky OFF (nullptr): legacy lexical pick — the datum flips to G01.
    const auto off = sb.buildSharedDatum(est, sats, decl);
    EXPECT_EQ(off.clock_pivot.at("G"), "G01");

    // Sticky ON: the incumbent survives the rise; the pin lands on ITS column.
    const auto on = sb.buildSharedDatum(est, sats, decl, &incumbent);
    EXPECT_EQ(on.clock_pivot.at("G"), "G05");
    EXPECT_TRUE([&]{ for (const auto& p : on.shared_datum)
                       if (p.column == est.clockColumn("G05")) return true; return false; }());
    // A constellation with NO incumbent entry still falls back to the lexical pick.
    EXPECT_EQ(on.clock_pivot.at("E"), "E11");
    EXPECT_EQ(on.declared_dimension, 2);
}

// RTA-1: when the incumbent SETS, a new pivot is selected (existing lexical
// fallback) — and after the emit-baseline advances, the NEW pivot is the sticky
// incumbent: the OLD one returning must NOT flip the datum back.
TEST(NetworkSbasisTest, StickyPivotReselectsOnLossAndDoesNotFlipBack) {
    NetworkEstimator est;
    est.registerClock("G03"); est.registerClock("G05"); est.registerClock("G07");

    NetworkSbasis sb;
    SbasisDeclaration decl;

    // Epoch N: incumbent G05 has set — re-select (lexical fallback picks G03).
    std::map<std::string, std::string> incumbent{{"G", "G05"}};
    const auto n = sb.buildSharedDatum(est, {"G03", "G07"}, decl, &incumbent);
    EXPECT_EQ(n.clock_pivot.at("G"), "G03");

    // The driver's emit-baseline advances to the emitted plan (emit site, P0-1).
    incumbent["G"] = n.clock_pivot.at("G");

    // Epoch N+1: the old incumbent G05 returns — the NEW incumbent stays pivot.
    const auto n1 = sb.buildSharedDatum(est, {"G03", "G05", "G07"}, decl, &incumbent);
    EXPECT_EQ(n1.clock_pivot.at("G"), "G03");
}

// RTA-1: the config-pinned satellite (network.clock_datum_satellite) has
// priority in BOTH modes — including over a sticky incumbent (a returning
// pinned satellite deliberately reclaims the datum; that flip-back is user
// intent).  When the pinned satellite is absent, sticky mode falls back to the
// incumbent and legacy mode to the lexical pick.
TEST(NetworkSbasisTest, PinnedSatellitePriorityRespectedInBothModes) {
    NetworkEstimator est;
    est.registerClock("G01"); est.registerClock("G03"); est.registerClock("G05");

    NetworkSbasis sb;
    SbasisDeclaration decl;
    decl.clock_pivot_sat["G"] = "G05";
    const std::map<std::string, std::string> incumbent{{"G", "G03"}};
    const std::vector<std::string> sats{"G01", "G03", "G05"};

    // Pinned present: it wins over BOTH the lexical pick and the incumbent.
    EXPECT_EQ(sb.buildSharedDatum(est, sats, decl).clock_pivot.at("G"), "G05");
    EXPECT_EQ(sb.buildSharedDatum(est, sats, decl, &incumbent).clock_pivot.at("G"), "G05");

    // Pinned absent this epoch: sticky keeps the incumbent, legacy goes lexical.
    decl.clock_pivot_sat["G"] = "G09";   // not in the registered set
    EXPECT_EQ(sb.buildSharedDatum(est, sats, decl).clock_pivot.at("G"), "G01");
    EXPECT_EQ(sb.buildSharedDatum(est, sats, decl, &incumbent).clock_pivot.at("G"), "G03");
}

// The SVD gate must count near-zero singular values == declared S-basis dimension.
TEST(NetworkSbasisTest, NullSpaceGateCountsDeficiencies) {
    NetworkSbasis sb;

    // Rank-2 of 3: one exact gauge (zero eigenvalue).
    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(3, 3);
    N(0, 0) = 10.0; N(1, 1) = 5.0; N(2, 2) = 0.0;
    EXPECT_NO_THROW(sb.assertNullSpaceRank(N, 1));
    EXPECT_THROW(sb.assertNullSpaceRank(N, 0), std::runtime_error);  // under-counts
    EXPECT_THROW(sb.assertNullSpaceRank(N, 2), std::runtime_error);  // over-counts

    // Full rank => zero deficiencies.
    const Eigen::MatrixXd F = Eigen::MatrixXd::Identity(4, 4) * 7.0;
    EXPECT_NO_THROW(sb.assertNullSpaceRank(F, 0));
    EXPECT_THROW(sb.assertNullSpaceRank(F, 1), std::runtime_error);
}

// FIREWALL at the GATE (design §4 / §9(d)): turning the carried-ambiguity prior ON
// must NOT change the null-space gate's withhold decision.  A carry on a station's
// LOCAL ambiguity cannot fill a SHARED gauge the data leaves free, so a marginal /
// under-declared epoch still WITHHOLDS with the carry on.  Here two shared columns
// are forced co-linear (an exact analogue of an unpinned per-constellation clock
// datum), giving one shared gauge; the gate's pass/throw decision is asserted to be
// identical with the carry OFF and ON.
TEST(NetworkSbasisTest, CarryDoesNotChangeGateDecision_Firewall) {
    NetworkSbasis sb;
    std::mt19937 gen(20260630);
    auto rnd = [&](Eigen::Index r, Eigen::Index c) {
        std::uniform_real_distribution<double> d(-1.0, 1.0);
        Eigen::MatrixXd M(r, c);
        for (Eigen::Index a = 0; a < r; ++a)
            for (Eigen::Index b = 0; b < c; ++b) M(a, b) = d(gen);
        return M;
    };
    const Eigen::Index n_shared = 3, nl = 3, m = 8;
    struct S { Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y; };
    std::vector<S> sd;
    for (int s = 0; s < 2; ++s) {
        S st;
        st.A_s = rnd(m, n_shared);
        st.A_s.col(1) = st.A_s.col(0);          // cols 0,1 identical → one shared gauge (e0−e1)
        st.A_r = rnd(m, nl);
        st.w = Eigen::VectorXd::Constant(m, 1.0);
        st.y = rnd(m, 1);
        sd.push_back(std::move(st));
    }
    auto build = [&](NetworkNormalEquations& neq, bool with_carry) {
        for (const auto& s : sd) {
            const Eigen::MatrixXd WA_s = s.w.asDiagonal() * s.A_s;
            const Eigen::MatrixXd WA_r = s.w.asDiagonal() * s.A_r;
            Eigen::MatrixXd N_rr = s.A_r.transpose() * WA_r
                + 1e-2 * Eigen::MatrixXd::Identity(nl, nl);  // PD local block
            neq.addStation(s.A_s.transpose() * WA_s, s.A_s.transpose() * WA_r, N_rr,
                           WA_s.transpose() * s.y, WA_r.transpose() * s.y);
        }
        if (with_carry) {
            Eigen::VectorXd dN(2); dN << 4.0, 2.0;
            Eigen::VectorXd du(2); du << 1.0, -1.0;
            neq.addStationPrior(0, {0, 2}, dN, du);   // prior on station 0's local ambiguities
        }
        neq.reduce();
    };
    NetworkNormalEquations off(n_shared), on(n_shared);
    build(off, false);
    build(on, true);

    // Declared dimension = 1 (the single shared gauge): both pass.
    EXPECT_NO_THROW(sb.assertNullSpaceRank(off.reducedSharedNormals(), 1));
    EXPECT_NO_THROW(sb.assertNullSpaceRank(on.reducedSharedNormals(), 1));
    // Under-declared datum (0): BOTH still withhold (throw).  The carry did not mask
    // the gauge — the firewall holds with the carry on.
    EXPECT_THROW(sb.assertNullSpaceRank(off.reducedSharedNormals(), 0), std::runtime_error);
    EXPECT_THROW(sb.assertNullSpaceRank(on.reducedSharedNormals(), 0), std::runtime_error);
}

// End-to-end: NetworkSbasis builds the clock datum, the gate sees exactly one
// residual deficiency (single constellation) after the local ambiguity pins, and
// the solve recovers the synthetic truth up to that S-transform.
TEST(NetworkSbasisTest, DrivesSingleConstellationRecoveryAndGate) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    std::vector<std::string> clk_sats;
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
        clk_sats.push_back(t.sats[s]);
    }

    est.beginEpoch();
    std::vector<std::size_t> ids;
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);  // pivot r=0 folds local amb pins
        ids.push_back(est.addStation(A_s, A_r, w, y));
    }

    NetworkSbasis sb;
    SbasisDeclaration decl;
    const auto plan = sb.buildSharedDatum(est, clk_sats, decl);
    EXPECT_EQ(plan.declared_dimension, 1);   // one constellation => one clock pin

    // Gate: after the local ambiguity pins are folded + Schur-eliminated, the
    // reduced shared normals carry exactly the one clock deficiency.
    EXPECT_NO_THROW(sb.assertNullSpaceRank(est.reducedSharedNormals(),
                                           plan.declared_dimension));

    const Eigen::VectorXd x_s = est.solve(plan.shared_datum);
    const double c0 = t.c[0];   // pivot = first registered sat = G01 = sats[0]
    for (int s = 0; s < t.S; ++s) {
        EXPECT_NEAR(x_s(clkCol[s]), t.c[s] - c0, 1e-6) << "sat clock " << s;
        EXPECT_NEAR(x_s(pbCol[s]), t.b[s] + t.M[0][s], 1e-6) << "sat phase bias " << s;
    }
    EXPECT_NEAR(x_s(clkCol[0]), 0.0, 1e-6);  // pivot clock pinned to the datum
}

// ---------------------------------------------------------------------------
// RTA-10: per-constellation σ̂0 carve-out decision (network.per_const_withhold).
// decidePerConstWithhold is PURE policy — rows + σ̂0_c per constellation, the A4
// withhold ceiling, and the judgeability floor in; publish/withhold partition
// out — so it is tested exhaustively here while the driver only maps the sets
// onto the emission (RT0-5-style prune).
// ---------------------------------------------------------------------------

namespace {
NetworkEstimator::PerConstSigma0 pcs(double sigma0_c, int rows) {
    NetworkEstimator::PerConstSigma0 s;
    s.sigma0_c = sigma0_c;
    s.rows     = rows;
    return s;
}
} // namespace

// Every constellation individually clean and judgeable ⇒ all publish, none
// withheld (the driver then publishes in full and counts the DIFFUSE edge).
TEST(NetworkEstimatorTest, PerConstWithholdAllCleanPublishesAll) {
    std::map<char, NetworkEstimator::PerConstSigma0> stats{
        {'G', pcs(1.2, 120)}, {'E', pcs(2.5, 80)}, {'R', pcs(5.9, 40)}};
    const auto dec = NetworkEstimator::decidePerConstWithhold(stats, /*ceiling=*/6.0,
                                                              /*min_rows=*/20);
    EXPECT_EQ(dec.publish, (std::set<char>{'G', 'E', 'R'}));
    EXPECT_TRUE(dec.withhold.empty());
}

// One dirty constellation ⇒ exactly it is withheld; the clean ones publish
// (localized fault degrades scope, not availability).
TEST(NetworkEstimatorTest, PerConstWithholdOneDirtyIsolated) {
    std::map<char, NetworkEstimator::PerConstSigma0> stats{
        {'G', pcs(1.1, 200)}, {'E', pcs(1.4, 90)}, {'C', pcs(37.0, 60)}};
    const auto dec = NetworkEstimator::decidePerConstWithhold(stats, 6.0, 20);
    EXPECT_EQ(dec.publish, (std::set<char>{'G', 'E'}));
    EXPECT_EQ(dec.withhold, (std::set<char>{'C'}));
}

// Every constellation dirty ⇒ empty publish set — the driver's signal that NO
// healthy subset exists and the whole-epoch withhold applies unchanged.  An
// EMPTY input (no judged constellations at all) yields the same empty publish.
TEST(NetworkEstimatorTest, PerConstWithholdAllDirtyYieldsEmptyPublish) {
    std::map<char, NetworkEstimator::PerConstSigma0> stats{
        {'G', pcs(9.0, 200)}, {'E', pcs(6.01, 90)}};
    const auto dec = NetworkEstimator::decidePerConstWithhold(stats, 6.0, 20);
    EXPECT_TRUE(dec.publish.empty());
    EXPECT_EQ(dec.withhold, (std::set<char>{'G', 'E'}));

    const auto empty_dec = NetworkEstimator::decidePerConstWithhold({}, 6.0, 20);
    EXPECT_TRUE(empty_dec.publish.empty());
    EXPECT_TRUE(empty_dec.withhold.empty());
}

// Boundary semantics: rows == min_rows is judgeable (publishes); one fewer row
// is UNJUDGEABLE and withheld even with a spotless σ̂0_c.  Ceiling is inclusive
// (σ̂0_c == ceiling publishes; the smallest exceedance withholds), matching the
// global withhold's strict '>' trip.  Zero-row entries are always withheld.
TEST(NetworkEstimatorTest, PerConstWithholdMinRowsAndCeilingBoundaries) {
    std::map<char, NetworkEstimator::PerConstSigma0> stats{
        {'G', pcs(1.0, 20)},    // exactly min_rows, clean         -> publish
        {'E', pcs(1.0, 19)},    // one row short, clean            -> withhold (unjudgeable)
        {'C', pcs(6.0, 100)},   // exactly the ceiling             -> publish (inclusive)
        {'R', pcs(6.0001, 100)},// smallest exceedance             -> withhold
        {'J', pcs(1.0, 0)}};    // no rows at all                  -> withhold
    const auto dec = NetworkEstimator::decidePerConstWithhold(stats, 6.0, 20);
    EXPECT_EQ(dec.publish, (std::set<char>{'G', 'C'}));
    EXPECT_EQ(dec.withhold, (std::set<char>{'E', 'R', 'J'}));
    // Every input key lands in exactly one output set.
    EXPECT_EQ(dec.publish.size() + dec.withhold.size(), stats.size());
}

} // namespace
} // namespace GnssProcessor
