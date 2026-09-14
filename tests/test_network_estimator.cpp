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

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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

// Fix A — anchor-free ENSEMBLE phase-bias datum: instead of pinning ONE anchor
// receiver's ambiguity per satellite (M_{0,s}=0), fix the phase-bias↔ambiguity
// gauge by a SUM-TO-ZERO over the receivers (Σ_r M_{r,s}=0), folded through the
// kernel's Schur-projected rank-1 correction (foldReducedSumZeroGauges).  The
// estimable satellite phase bias then references the receiver ENSEMBLE MEAN
// (b_s + mean_r M_{r,s}), the ambiguities are M_{r,s} − mean_r (summing to zero),
// and the between-receiver differences equal the truth — a pure S-transform of
// the anchor datum (same estimable quantities, different gauge).  No pivot, no
// pin rows in any station's block.
TEST(NetworkEstimatorTest, SumZeroPhaseDatumRecoversEnsembleSTransform) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }

    est.beginEpoch();
    std::vector<std::size_t> ids;
    for (int r = 0; r < t.R; ++r) {
        // Every station symmetric — NO pivot pin rows (the ensemble datum needs none).
        const int S = t.S;
        const Eigen::Index n_local = 1 + S;      // [a_r, M_{r,0..S-1}]
        const Eigen::Index m = 2 * S;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_local);
        Eigen::VectorXd w = Eigen::VectorXd::Ones(m);
        Eigen::VectorXd y = Eigen::VectorXd::Zero(m);
        Eigen::Index row = 0;
        for (int s = 0; s < S; ++s) {
            A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0;
            y(row) = t.a[r] - t.c[s]; ++row;
            A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
            A_r(row, 0) = 1.0; A_r(row, 1 + s) = 1.0;
            y(row) = t.a[r] - t.c[s] + t.b[s] + t.M[r][s]; ++row;
        }
        ids.push_back(est.addStation(A_s, A_r, w, y));
    }

    // Ensemble phase-bias datum: one Σ_r M_{r,s}=0 gauge per satellite, over every
    // receiver's ambiguity to that satellite (local column 1+s in each station).
    std::vector<NetworkNormalEquations::SumZeroGauge> gauges(t.S);
    for (int s = 0; s < t.S; ++s)
        for (int r = 0; r < t.R; ++r)
            gauges[s].terms.emplace_back(ids[r], static_cast<Eigen::Index>(1 + s));
    est.setPhaseSumZeroGauges(gauges);

    // Clock datum unchanged: a SHARED pin on the reference satellite clock.
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);

    const double c0 = t.c[0];
    for (int s = 0; s < t.S; ++s) {
        double meanM = 0.0;
        for (int r = 0; r < t.R; ++r) meanM += t.M[r][s];
        meanM /= t.R;
        EXPECT_NEAR(x_s(clkCol[s]), t.c[s] - c0, 1e-6) << "sat clock " << s;
        EXPECT_NEAR(x_s(pbCol[s]), t.b[s] + meanM, 1e-6)   // ENSEMBLE-mean reference
            << "sat phase bias " << s;
    }

    std::vector<Eigen::VectorXd> xr(t.R);
    for (int r = 0; r < t.R; ++r) xr[r] = est.reconstructStation(ids[r], x_s);
    for (int s = 0; s < t.S; ++s) {
        double meanM = 0.0, sum = 0.0;
        for (int r = 0; r < t.R; ++r) meanM += t.M[r][s];
        meanM /= t.R;
        for (int r = 0; r < t.R; ++r) {
            EXPECT_NEAR(xr[r](1 + s), t.M[r][s] - meanM, 1e-6)
                << "ambiguity r=" << r << " s=" << s;
            sum += xr[r](1 + s);
        }
        EXPECT_NEAR(sum, 0.0, 1e-6) << "sum-to-zero gauge, sat " << s;
        // Pure S-transform: between-receiver differences match the truth exactly.
        for (int r = 1; r < t.R; ++r)
            EXPECT_NEAR(xr[r](1 + s) - xr[0](1 + s), t.M[r][s] - t.M[0][s], 1e-6)
                << "DD-invariant r=" << r << " s=" << s;
    }
}

// Card 4 (M1) phase C — gate 4 (DatumFreedom) through the production NetworkEstimator
// API: a folded rank-K DD prior yields a DD ambiguity estimate INVARIANT to the shared
// datum choice.  Same network + sum-to-zero phase gauge + DD prior, solved under two
// different clock datums (pin sat0 vs sat1) ⇒ identical reconstructed AMBIGUITIES (the
// receiver-clock column legitimately shifts with the datum; the ambiguities do not) and
// an identical DD posterior.  Exercises setDdPriors → solve → reconstruct →
// ddPriorPosteriorMean end-to-end.
TEST(NetworkEstimatorTest, DdPriorDatumFreedomThroughEstimator) {
    Truth t = makeTruth(/*seed=*/321, /*R=*/4, /*S=*/4);
    for (auto& mrow : t.M) for (auto& v : mrow) v = std::round(v);   // integer ambiguities
    const std::string signal = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    est.beginEpoch();
    std::vector<std::size_t> ids;
    for (int r = 0; r < t.R; ++r) {
        const int S = t.S;
        const Eigen::Index n_local = 1 + S;   // [a_r, M_{r,0..S-1}]
        const Eigen::Index m = 2 * S;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_local);
        Eigen::VectorXd w = Eigen::VectorXd::Ones(m), y = Eigen::VectorXd::Zero(m);
        Eigen::Index row = 0;
        for (int s = 0; s < S; ++s) {
            A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0;
            y(row) = t.a[r] - t.c[s]; ++row;
            A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
            A_r(row, 0) = 1.0; A_r(row, 1 + s) = 1.0;
            y(row) = t.a[r] - t.c[s] + t.b[s] + t.M[r][s]; ++row;
        }
        ids.push_back(est.addStation(A_s, A_r, w, y));
    }
    // Phase datum: PIN receiver 0's ambiguities to 0 (the anchor/tree pins) via
    // addStationPrior — a PRE-reduce local prior that composes EXACTLY with the
    // post-reduce DD fold (the fold reads the already-pinned N_rr).  NOT the
    // sum-to-zero ensemble gauge: that is itself a post-reduce fold and is mutually
    // exclusive with the DD priors (NetworkEstimator::solve throws if both are set).
    {
        std::vector<Eigen::Index> pin_cols(t.S);
        for (int s = 0; s < t.S; ++s) pin_cols[s] = 1 + s;
        est.addStationPrior(ids[0], pin_cols, Eigen::VectorXd::Constant(t.S, 1e10),
                            Eigen::VectorXd::Zero(t.S));   // M_{0,s} = 0
    }

    // A DD prior between receivers {0,1} and satellites {0,1} (ambiguity col 1+s).
    using Term = NetworkNormalEquations::DdPriorBlock::Term;
    NetworkNormalEquations::DdPriorBlock blk;
    blk.rows = {{Term{ids[0], 1, +1.0}, Term{ids[0], 2, -1.0},
                 Term{ids[1], 1, -1.0}, Term{ids[1], 2, +1.0}}};
    blk.Pz = (Eigen::MatrixXd(1, 1) << 0.05).finished();
    blk.zhat = (Eigen::VectorXd(1) << 0.0).finished();   // pull the DD toward 0
    est.setDdPriors({blk});

    const NetworkEstimator::Datum datumA = {{est.clockColumn(t.sats[0]), 0.0}};
    const Eigen::VectorXd xsA = est.solve(datumA);
    std::vector<Eigen::VectorXd> xrA(t.R);
    for (int r = 0; r < t.R; ++r) xrA[r] = est.reconstructStation(ids[r], xsA);
    const Eigen::VectorXd zA = est.ddPriorPosteriorMean(0, xsA);

    const NetworkEstimator::Datum datumB = {{est.clockColumn(t.sats[1]), 0.0}};
    const Eigen::VectorXd xsB = est.solve(datumB);   // re-solve, DIFFERENT datum
    std::vector<Eigen::VectorXd> xrB(t.R);
    for (int r = 0; r < t.R; ++r) xrB[r] = est.reconstructStation(ids[r], xsB);
    const Eigen::VectorXd zB = est.ddPriorPosteriorMean(0, xsB);

    for (int r = 0; r < t.R; ++r)
        EXPECT_LT((xrA[r].tail(t.S) - xrB[r].tail(t.S)).cwiseAbs().maxCoeff(), 1e-7)
            << "station " << r << " ambiguities must be invariant to the clock datum";
    const double ddA = xrA[0](1) - xrA[0](2) - xrA[1](1) + xrA[1](2);
    EXPECT_NEAR(zA(0), ddA, 1e-7) << "posterior == DD of the reconstructed ambiguities";
    EXPECT_NEAR(zA(0), zB(0), 1e-7) << "DD posterior must be datum-invariant";
    EXPECT_EQ(est.numDdPriorBlocks(), 1u);
}

// The DD priors and the sum-to-zero ensemble gauge are mutually exclusive (both are
// post-reduce Woodbury folds on the same local block; composing them drops the cross
// term and silently biases the solve).  Setting both must fail fast at solve().
TEST(NetworkEstimatorTest, DdPriorAndSumZeroGaugeAreMutuallyExclusive) {
    Truth t = makeTruth(/*seed=*/7, /*R=*/3, /*S=*/3);
    const std::string signal = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        est.registerPhaseBias(t.sats[s], signal);
    }
    est.beginEpoch();
    std::vector<std::size_t> ids;
    for (int r = 0; r < t.R; ++r) {
        const int S = t.S;
        const Eigen::Index n_local = 1 + S, m = 2 * S;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_local);
        Eigen::VectorXd w = Eigen::VectorXd::Ones(m), y = Eigen::VectorXd::Zero(m);
        Eigen::Index row = 0;
        for (int s = 0; s < S; ++s) {
            A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0; ++row;
            A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0; A_r(row, 1 + s) = 1.0; ++row;
        }
        ids.push_back(est.addStation(A_s, A_r, w, y));
    }
    std::vector<NetworkNormalEquations::SumZeroGauge> gauges(1);
    for (int r = 0; r < t.R; ++r)
        gauges[0].terms.emplace_back(ids[r], static_cast<Eigen::Index>(1));
    est.setPhaseSumZeroGauges(gauges);
    using Term = NetworkNormalEquations::DdPriorBlock::Term;
    NetworkNormalEquations::DdPriorBlock blk;
    blk.rows = {{Term{ids[0], 1, +1.0}, Term{ids[1], 1, -1.0}}};
    blk.Pz = (Eigen::MatrixXd(1, 1) << 0.05).finished();
    blk.zhat = Eigen::VectorXd::Zero(1);
    est.setDdPriors({blk});
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
    EXPECT_THROW(est.solve(datum), std::logic_error);
}

// ---------------------------------------------------------------------------
// Eq-34 integer-recovery gate (S-system theory reference §6, docs/
// SPEC_sbasis_realization_contract.md §E).  A network phase-bias datum yields an
// INTEGER-fixable product for a held-out PPP-RTK user IFF the emitted per-
// satellite datum reference has INTEGER between-satellite differences.
//
// The user applies the emitted bias δ̃_s to its phase; its float ambiguity
// absorbs  M_u,s − ref_s  with  ref_s = δ̃_s − b_s  (the physical satellite bias
// b_s cancels between the observation's true bias and the emitted product).  The
// user's fixable between-satellite single-difference is
//     (M_u,s − M_u,p) − (ref_s − ref_p).
// With integer user ambiguities this is integer IFF  D_sp = ref_s − ref_p  is an
// integer for every satellite pair (s,p).
//
//   CC-R  (single pivot receiver 0, M_{0,s}=0 pin):  ref_s = M_{0,s}  (integer)
//         ⇒ D_sp = M_{0,s} − M_{0,p} integer ⇒ user DD integer ⇒ FIXABLE.
//   Ensemble (Σ_r M_{r,s}=0):  ref_s = mean_r M_{r,s}
//         ⇒ D_sp = mean_r(M_{r,s} − M_{r,p}) = k_sp / R  ⇒ NON-integer
//         ⇒ user DD non-integer ⇒ NOT FIXABLE (Odijk CC-S "not directly integer").
//
// This is the decisive gate the theory reference mandates before trusting the
// ensemble datum: churn suppression via the ensemble CANNOT unlock integer AR,
// because the ensemble product is structurally non-integer (a mean of integers).
TEST(NetworkEstimatorTest, Eq34UserIntegerRecovery_CCRIntegerEnsembleFractional) {
    // Integer network ambiguities.  In real GNSS M_{r,s} = integer + a receiver-
    // common fractional phase bias; here pure integers so the ONLY non-integer
    // source under test is the datum's own averaging (not receiver biases).
    Truth t = makeTruth(/*seed=*/99, /*R=*/5, /*S=*/6);
    for (auto& row : t.M)
        for (auto& v : row) v = std::round(v);

    const std::string signal = "L1";
    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };

    // ---- CC-R solve: pivot receiver 0 pins M_{0,s}=0 (buildStation pivot rows) ----
    std::vector<double> ref_ccr(t.S);
    {
        NetworkEstimator est;
        std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
        for (int s = 0; s < t.S; ++s) {
            clkCol[s] = est.registerClock(t.sats[s]);
            pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
        }
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r;
            Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
        const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
        const Eigen::VectorXd x_s = est.solve(datum);
        for (int s = 0; s < t.S; ++s)
            ref_ccr[s] = x_s(pbCol[s]) - t.b[s];   // ref_s = δ̃_s − b_s
    }

    // ---- Ensemble solve: Σ_r M_{r,s}=0, no pivot pins ----
    std::vector<double> ref_ens(t.S);
    {
        NetworkEstimator est;
        std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
        for (int s = 0; s < t.S; ++s) {
            clkCol[s] = est.registerClock(t.sats[s]);
            pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
        }
        est.beginEpoch();
        std::vector<std::size_t> ids;
        for (int r = 0; r < t.R; ++r) {
            const int S = t.S;
            const Eigen::Index n_local = 1 + S;
            const Eigen::Index m = 2 * S;
            Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, est.numShared());
            Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_local);
            Eigen::VectorXd w = Eigen::VectorXd::Ones(m);
            Eigen::VectorXd y = Eigen::VectorXd::Zero(m);
            Eigen::Index row = 0;
            for (int s = 0; s < S; ++s) {
                A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0;
                y(row) = t.a[r] - t.c[s]; ++row;
                A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
                A_r(row, 0) = 1.0; A_r(row, 1 + s) = 1.0;
                y(row) = t.a[r] - t.c[s] + t.b[s] + t.M[r][s]; ++row;
            }
            ids.push_back(est.addStation(A_s, A_r, w, y));
        }
        std::vector<NetworkNormalEquations::SumZeroGauge> gauges(t.S);
        for (int s = 0; s < t.S; ++s)
            for (int r = 0; r < t.R; ++r)
                gauges[s].terms.emplace_back(ids[r], static_cast<Eigen::Index>(1 + s));
        est.setPhaseSumZeroGauges(gauges);
        const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
        const Eigen::VectorXd x_s = est.solve(datum);
        for (int s = 0; s < t.S; ++s)
            ref_ens[s] = x_s(pbCol[s]) - t.b[s];
    }

    // ---- The gate: D_sp = ref_s − ref_p must be integer for the user to fix ----
    bool ens_has_fraction = false;
    for (int s = 0; s < t.S; ++s) {
        for (int p = 0; p < t.S; ++p) {
            const double d_ccr = ref_ccr[s] - ref_ccr[p];
            const double d_ens = ref_ens[s] - ref_ens[p];
            EXPECT_LT(fracDist(d_ccr), 1e-6)
                << "CC-R between-sat reference D(" << s << "," << p << ")="
                << d_ccr << " must be integer (user DD fixable)";
            if (fracDist(d_ens) > 1e-3) ens_has_fraction = true;
        }
    }
    // Ensemble: at least one between-sat reference is a NON-integer k/R fraction
    // ⇒ the held-out user's DD is non-integer ⇒ the product is NOT integer-fixable.
    EXPECT_TRUE(ens_has_fraction)
        << "Ensemble datum should yield a non-integer between-satellite reference "
           "(mean-of-integers / R) — the structural obstruction to user integer AR.";

    // The ensemble reference is exactly the receiver mean (reproduces the k/R form).
    for (int s = 0; s < t.S; ++s) {
        double meanM = 0.0;
        for (int r = 0; r < t.R; ++r) meanM += t.M[r][s];
        meanM /= t.R;
        EXPECT_NEAR(ref_ens[s], meanM, 1e-6) << "ensemble ref = receiver mean, sat " << s;
    }
}

// ===========================================================================
// Card 3 — Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN phase-bias datum.
//
// Unlike the fixtures above (which lump the receiver fractional bias into the
// ambiguity M_{r,s}), the eq34 model gives the receiver fractional phase bias
// its OWN estimable state δ_r and keeps the ambiguity a true integer z_{r,s}:
//     code  P_{r,s} = a_r − c_s
//     phase Φ_{r,s} = a_r − c_s + b_s + δ_r + z_{r,s}   (z ∈ ℤ, δ_r fractional)
// The datum is realized by a SHARED clock pin (c_{s0}=0) + ONE datum-receiver
// RPB pin (δ_{r1}=0) + a spanning tree of ambiguity pins over the bipartite
// (receiver × satellite) observation graph.  Then EVERY emitted between-
// satellite reference difference is an integer — including for satellites the
// datum receiver never sees — because each chain hop contributes only an
// integer while the SAME common receiver fractional δ_{r1} rides every ref_s
// and cancels in the difference.
// ===========================================================================
namespace {

struct Eq34Truth {
    int R, S;
    std::vector<double> a, c, b, d;         // rx clk, sat clk, sat pbias, rx frac
    std::vector<std::vector<int>> z;        // integer ambiguities [R][S]
    std::vector<std::string> sats;
    std::vector<std::set<int>> vis;         // vis[r] = sats station r sees
};

// One spanning-tree ambiguity pin: drive station r's ambiguity for sat s to the
// integer target (cycles, λ≡1 in these synthetic units).
struct AmbPin { int r; int s; double target; };

Eq34Truth makeEq34Truth() {
    Eq34Truth t;
    t.R = 4; t.S = 5;
    t.a = {3.0, -2.0, 5.0, 1.0};
    t.c = {7.0, -4.0, 2.0, 9.0, -1.0};
    t.b = {0.10, -0.20, 0.35, 0.05, -0.15};   // physical satellite phase bias
    t.d = {0.30, -0.40, 0.15, 0.45};           // receiver fractional bias (all ≠ 0)
    t.sats = {"G01", "G02", "G03", "G04", "G05"};
    const int zz[4][5] = {{ 2, -3,  1,  4, -2},
                          { 5,  1, -4,  2,  3},
                          {-1,  2,  6, -5,  1},
                          { 3, -2,  4,  1, -6}};
    t.z.assign(4, std::vector<int>(5, 0));
    for (int r = 0; r < 4; ++r) for (int s = 0; s < 5; ++s) t.z[r][s] = zz[r][s];
    // No receiver sees all sats; r0 (a natural datum receiver) misses s3,s4.
    t.vis = { {0, 1, 2}, {2, 3}, {3, 4}, {0, 4} };
    return t;
}

// Solve one epoch under the eq34 chain datum: clock pin c_{s0}=0 + r1's RPB pin
// (δ_{r1}=0) + the given spanning-tree ambiguity pins.  Only present stations
// (present==nullptr ⇒ all) and their visible sats contribute.  Returns
// ref[s] = x_s(pbCol[s]) − b[s] (the emitted between-satellite frame reference),
// or NaN for a satellite no present station sees.
std::vector<double> solveEq34Chain(const Eq34Truth& t, int r1,
                                   const std::vector<AmbPin>& pins,
                                   const std::vector<bool>* present = nullptr) {
    const std::string signal = "L1";
    auto here = [&](int r) { return !present || (*present)[r]; };

    // Register only satellites some present station sees (a phantom all-zero
    // shared column would trip the estimator's own gauge bookkeeping).
    std::set<int> seen;
    for (int r = 0; r < t.R; ++r) if (here(r))
        for (int s : t.vis[r]) seen.insert(s);

    NetworkEstimator est;
    std::map<int, Eigen::Index> clkCol, pbCol;
    for (int s : seen) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
    }

    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        if (!here(r)) continue;
        std::vector<int> vs(t.vis[r].begin(), t.vis[r].end());  // sorted
        const int nv = static_cast<int>(vs.size());
        std::map<int, int> ambCol;                       // sat -> local col
        for (int k = 0; k < nv; ++k) ambCol[vs[k]] = 2 + k;  // [0]=a_r [1]=δ_r [2+]=z
        std::vector<AmbPin> myPins;
        for (const auto& p : pins) if (p.r == r && t.vis[r].count(p.s)) myPins.push_back(p);
        const bool rpbPin = (r == r1);
        const int nrows = 2 * nv + static_cast<int>(myPins.size()) + (rpbPin ? 1 : 0);
        const int nloc  = 2 + nv;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(nrows, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(nrows, nloc);
        Eigen::VectorXd w   = Eigen::VectorXd::Ones(nrows);
        Eigen::VectorXd y   = Eigen::VectorXd::Zero(nrows);
        int row = 0;
        for (int s : vs) {
            // code: P = a_r − c_s
            A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0;
            y(row) = t.a[r] - t.c[s]; ++row;
            // phase: Φ = a_r − c_s + b_s + δ_r + z_{r,s}
            A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
            A_r(row, 0) = 1.0;             // a_r
            A_r(row, 1) = 1.0;             // δ_r (RPB split state)
            A_r(row, ambCol[s]) = 1.0;     // z_{r,s}
            y(row) = t.a[r] - t.c[s] + t.b[s] + t.d[r] + static_cast<double>(t.z[r][s]);
            ++row;
        }
        for (const auto& p : myPins) {     // spanning-tree ambiguity pins
            A_r(row, ambCol[p.s]) = 1.0; y(row) = p.target; w(row) = 1e10; ++row;
        }
        if (rpbPin) {                       // r1's RPB pin: δ_{r1}=0
            A_r(row, 1) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row;
        }
        est.addStation(A_s, A_r, w, y);
    }

    // The clock datum must pin a PRESENT satellite's clock; s0 is seen by r0 and
    // r3 in this fixture, so pin s0 when present else the lowest seen sat.
    const int clk_pin_sat = seen.count(0) ? 0 : *seen.begin();
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[clk_pin_sat]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);

    std::vector<double> ref(t.S, std::numeric_limits<double>::quiet_NaN());
    for (int s : seen) ref[s] = x_s(pbCol[s]) - t.b[s];
    return ref;
}

}  // namespace

// GATE 1: the eq34 chain datum yields an INTEGER user double difference for
// EVERY satellite pair — including pairs whose chain paths differ and pairs
// involving satellites the datum receiver never sees — despite a non-zero
// receiver fractional bias d_r.  Negative control in the same fixture: the
// prioritized per-satellite anchor election yields a fractional cross-anchor
// pair (the structural obstruction the chain datum removes).
TEST(NetworkEstimatorTest, Eq34ChainDatumUserDdInteger) {
    const Eq34Truth t = makeEq34Truth();
    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };

    // Spanning tree rooted at r1 = station 0 (BFS): r0-{s0,s1,s2}, s0-r3, s2-r1,
    // r3-s4, r1-s3, s4-r2.  8 edges over 9 vertices + 1 RPB pin = 9 = R + S.
    const std::vector<AmbPin> pins = {
        {0, 0, 0.0}, {0, 1, 0.0}, {0, 2, 0.0},   // r1's child edges
        {3, 0, 0.0},                              // s0 -> r3
        {1, 2, 0.0},                              // s2 -> r1
        {3, 4, 0.0},                              // r3 -> s4
        {1, 3, 0.0},                              // r1 -> s3
        {2, 4, 0.0},                              // s4 -> r2
    };
    // Rank bookkeeping (D3): #pins == n_c + m_c = R + S for the single component.
    EXPECT_EQ(static_cast<int>(pins.size()) + 1 /*RPB*/, t.R + t.S);

    const std::vector<double> ref = solveEq34Chain(t, /*r1=*/0, pins);
    for (int s = 0; s < t.S; ++s) ASSERT_TRUE(std::isfinite(ref[s])) << "sat " << s;

    // Every between-satellite reference difference is an integer (the common
    // receiver fractional δ_{r1} rides every ref_s and cancels).
    for (int s = 0; s < t.S; ++s)
        for (int p = 0; p < t.S; ++p)
            EXPECT_LT(fracDist(ref[s] - ref[p]), 1e-6)
                << "chain-datum D(" << s << "," << p << ")=" << (ref[s] - ref[p])
                << " must be an integer (user DD fixable)";

    // Every ref_s carries the SAME common fractional (its distance to the nearest
    // integer equals that of ref_0) — the mechanism behind the integer DDs.
    const double frac0 = ref[0] - std::round(ref[0]);
    for (int s = 1; s < t.S; ++s) {
        double frac = ref[s] - std::round(ref[s]);
        // fold to (−0.5, 0.5] and compare to frac0 modulo 1.
        double diff = std::fmod(frac - frac0, 1.0);
        if (diff > 0.5) diff -= 1.0; if (diff < -0.5) diff += 1.0;
        EXPECT_LT(std::abs(diff), 1e-6) << "sat " << s << " frac " << frac << " vs " << frac0;
    }

    // ---- Negative control: prioritized per-satellite anchor election ----
    // Lumped legacy model M_{r,s}=d_r+z_{r,s} (no RPB), anchors: s0-2->r0,
    // s3->r1, s4->r2.  ref_s = M_{anchor(s),s}, so a cross-anchor pair carries
    // d_{anchor(s)} − d_{anchor(p)} ∉ ℤ.
    std::vector<double> ref_prio(t.S, 0.0);
    {
        const std::string signal = "L1";
        const int anchorOf[5] = {0, 0, 0, 1, 2};
        NetworkEstimator est;
        std::map<int, Eigen::Index> clkCol, pbCol;
        for (int s = 0; s < t.S; ++s) {
            clkCol[s] = est.registerClock(t.sats[s]);
            pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
        }
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            std::vector<int> vs(t.vis[r].begin(), t.vis[r].end());
            const int nv = static_cast<int>(vs.size());
            std::map<int, int> ambCol;
            for (int k = 0; k < nv; ++k) ambCol[vs[k]] = 1 + k;   // [0]=a_r [1+]=M
            std::vector<int> anchoredHere;
            for (int s : vs) if (anchorOf[s] == r) anchoredHere.push_back(s);
            const int nrows = 2 * nv + static_cast<int>(anchoredHere.size());
            const int nloc  = 1 + nv;
            Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(nrows, est.numShared());
            Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(nrows, nloc);
            Eigen::VectorXd w   = Eigen::VectorXd::Ones(nrows);
            Eigen::VectorXd y   = Eigen::VectorXd::Zero(nrows);
            int row = 0;
            for (int s : vs) {
                A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0;
                y(row) = t.a[r] - t.c[s]; ++row;
                A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
                A_r(row, 0) = 1.0; A_r(row, ambCol[s]) = 1.0;   // M lumps δ_r + z
                y(row) = t.a[r] - t.c[s] + t.b[s] + t.d[r] + static_cast<double>(t.z[r][s]);
                ++row;
            }
            for (int s : anchoredHere) {   // pin M_{anchor,s}=0
                A_r(row, ambCol[s]) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row;
            }
            est.addStation(A_s, A_r, w, y);
        }
        const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
        const Eigen::VectorXd x_s = est.solve(datum);
        for (int s = 0; s < t.S; ++s) ref_prio[s] = x_s(pbCol[s]) - t.b[s];
    }
    bool prio_has_fraction = false;
    for (int s = 0; s < t.S; ++s)
        for (int p = 0; p < t.S; ++p)
            if (fracDist(ref_prio[s] - ref_prio[p]) > 1e-3) prio_has_fraction = true;
    EXPECT_TRUE(prio_has_fraction)
        << "prioritized anchor election must yield >= 1 fractional cross-anchor pair "
           "(the obstruction the eq34 chain datum removes).";
}

// GATE 3: datum-receiver handover — electing a DIFFERENT datum receiver r1'
// still yields integer between-satellite DDs (the common fractional cancels),
// though the absolute references move by a common fractional + integers.
TEST(NetworkEstimatorTest, Eq34DatumReceiverHandoverBetweenSatInteger) {
    const Eq34Truth t = makeEq34Truth();
    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };

    const std::vector<AmbPin> pins0 = {
        {0, 0, 0.0}, {0, 1, 0.0}, {0, 2, 0.0}, {3, 0, 0.0},
        {1, 2, 0.0}, {3, 4, 0.0}, {1, 3, 0.0}, {2, 4, 0.0},
    };
    // Spanning tree rooted at r1' = station 3 (BFS): r3-{s0,s4}, s0-r0, s4-r2,
    // r0-{s1,s2}, r2-s3, s2-r1.  8 edges + 1 RPB pin = 9.
    const std::vector<AmbPin> pins3 = {
        {3, 0, 0.0}, {3, 4, 0.0}, {0, 0, 0.0}, {2, 4, 0.0},
        {0, 1, 0.0}, {0, 2, 0.0}, {2, 3, 0.0}, {1, 2, 0.0},
    };
    const std::vector<double> ref0 = solveEq34Chain(t, /*r1=*/0, pins0);
    const std::vector<double> ref3 = solveEq34Chain(t, /*r1=*/3, pins3);

    for (int s = 0; s < t.S; ++s) {
        ASSERT_TRUE(std::isfinite(ref0[s])) << "s=" << s;
        ASSERT_TRUE(std::isfinite(ref3[s])) << "s=" << s;
    }
    // Both datums give integer between-satellite DDs.
    for (int s = 0; s < t.S; ++s)
        for (int p = 0; p < t.S; ++p) {
            EXPECT_LT(fracDist(ref0[s] - ref0[p]), 1e-6) << "r1=0 D(" << s << "," << p << ")";
            EXPECT_LT(fracDist(ref3[s] - ref3[p]), 1e-6) << "r1=3 D(" << s << "," << p << ")";
        }
    // The absolute frame moved (a different common fractional d_{r1}); at least
    // one satellite's reference differs by a non-integer between the two datums.
    bool moved = false;
    for (int s = 0; s < t.S; ++s)
        if (fracDist(ref0[s] - ref3[s]) > 1e-3) moved = true;
    EXPECT_TRUE(moved) << "handover to a different datum receiver should move the "
                          "absolute frame by a common fractional.";
}

// GATE 2: a tree-edge REATTACH (satellite re-pinned at a different station,
// same datum receiver) steps the emitted satellite phase bias by an EXACT
// integer number of cycles — the frame's common fractional δ_{r1} is unchanged,
// so only an integer moves.  (Pin-to-round, a driver feature, would then pick
// the |·|-minimizing integer; at the estimator level we pin target 0 and check
// the step is integral.)
TEST(NetworkEstimatorTest, Eq34ReattachStepsByExactInteger) {
    const Eq34Truth t = makeEq34Truth();
    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };

    // Datum receiver r1 = station 0 in BOTH solves.  s3 (index 3) is tracked by
    // station 1 AND station 2; the two spanning trees differ ONLY in s3's edge.
    const std::vector<AmbPin> pinsA = {   // s3 pinned at station 1
        {0, 0, 0.0}, {0, 1, 0.0}, {0, 2, 0.0}, {3, 0, 0.0},
        {1, 2, 0.0}, {3, 4, 0.0}, {1, 3, 0.0}, {2, 4, 0.0},
    };
    const std::vector<AmbPin> pinsB = {   // s3 REATTACHED to station 2
        {0, 0, 0.0}, {0, 1, 0.0}, {0, 2, 0.0}, {3, 0, 0.0},
        {1, 2, 0.0}, {3, 4, 0.0}, {2, 3, 0.0}, {2, 4, 0.0},
    };
    const std::vector<double> refA = solveEq34Chain(t, /*r1=*/0, pinsA);
    const std::vector<double> refB = solveEq34Chain(t, /*r1=*/0, pinsB);

    // The reattached satellite's emitted bias stepped by an exact integer.
    EXPECT_LT(fracDist(refB[3] - refA[3]), 1e-6)
        << "reattach step " << (refB[3] - refA[3]) << " must be an integer number of cycles.";
    // Between-satellite DDs remain integer in both frames.
    for (int s = 0; s < t.S; ++s)
        for (int p = 0; p < t.S; ++p) {
            EXPECT_LT(fracDist(refA[s] - refA[p]), 1e-6) << "A D(" << s << "," << p << ")";
            EXPECT_LT(fracDist(refB[s] - refB[p]), 1e-6) << "B D(" << s << "," << p << ")";
        }
}

// GATE 4: component-split integrity — when a cut station leaves and the graph
// splits, within-component DDs stay integer while a cross-component pair is
// fractional (the accepted, unavoidable limitation: no data connects them).
TEST(NetworkEstimatorTest, Eq34ComponentSplitLocalIntegrity) {
    // Two disjoint components on the SAME signal: {r0,r1} see {s0,s1};
    // {r2,r3} see {s2,s3}.  No station bridges them.
    Eq34Truth t;
    t.R = 4; t.S = 4;
    t.a = {3.0, -2.0, 5.0, 1.0};
    t.c = {7.0, -4.0, 2.0, 9.0};
    t.b = {0.10, -0.20, 0.35, 0.05};
    t.d = {0.30, -0.40, 0.15, 0.45};
    t.sats = {"G01", "G02", "G03", "G04"};
    const int zz[4][4] = {{2, -3, 0, 0}, {5, 1, 0, 0}, {0, 0, 6, -5}, {0, 0, 4, 1}};
    t.z.assign(4, std::vector<int>(4, 0));
    for (int r = 0; r < 4; ++r) for (int s = 0; s < 4; ++s) t.z[r][s] = zz[r][s];
    t.vis = { {0, 1}, {0, 1}, {2, 3}, {2, 3} };

    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };
    // Component A: r1=0 (RPB pin), tree r0-s0, r0-s1, s0-r1.  Component B: r1=2
    // (RPB pin), tree r2-s2, r2-s3, s2-r3.
    const std::vector<AmbPin> pins = {
        {0, 0, 0.0}, {0, 1, 0.0}, {1, 0, 0.0},   // component A (root r0)
        {2, 2, 0.0}, {2, 3, 0.0}, {3, 2, 0.0},   // component B (root r2)
    };
    // Both roots pin their RPB.  solveEq34Chain pins only r1; component B's root
    // (r2) needs its RPB pinned too, so drive a second solve is unnecessary — we
    // fold r2's RPB pin by treating the whole network as one solve with TWO RPB
    // pins.  Build inline to pin both roots.
    const std::string signal = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
    }
    const int rootOf[4] = {0, 0, 2, 2};   // datum receiver per station's component
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        std::vector<int> vs(t.vis[r].begin(), t.vis[r].end());
        const int nv = static_cast<int>(vs.size());
        std::map<int, int> ambCol;
        for (int k = 0; k < nv; ++k) ambCol[vs[k]] = 2 + k;
        std::vector<AmbPin> myPins;
        for (const auto& p : pins) if (p.r == r) myPins.push_back(p);
        const bool rpbPin = (rootOf[r] == r);
        const int nrows = 2 * nv + static_cast<int>(myPins.size()) + (rpbPin ? 1 : 0);
        const int nloc  = 2 + nv;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(nrows, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(nrows, nloc);
        Eigen::VectorXd w   = Eigen::VectorXd::Ones(nrows);
        Eigen::VectorXd y   = Eigen::VectorXd::Zero(nrows);
        int row = 0;
        for (int s : vs) {
            A_s(row, clkCol[s]) = -1.0; A_r(row, 0) = 1.0;
            y(row) = t.a[r] - t.c[s]; ++row;
            A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
            A_r(row, 0) = 1.0; A_r(row, 1) = 1.0; A_r(row, ambCol[s]) = 1.0;
            y(row) = t.a[r] - t.c[s] + t.b[s] + t.d[r] + static_cast<double>(t.z[r][s]);
            ++row;
        }
        for (const auto& p : myPins) { A_r(row, ambCol[p.s]) = 1.0; y(row) = p.target; w(row) = 1e10; ++row; }
        if (rpbPin) { A_r(row, 1) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
        est.addStation(A_s, A_r, w, y);
    }
    // One clock pin per component (each component's clock gauge is independent).
    const NetworkEstimator::Datum datum = {
        {est.clockColumn(t.sats[0]), 0.0},   // component A gauge
        {est.clockColumn(t.sats[2]), 0.0},   // component B gauge
    };
    const Eigen::VectorXd x_s = est.solve(datum);
    std::vector<double> ref(t.S);
    for (int s = 0; s < t.S; ++s) ref[s] = x_s(pbCol[s]) - t.b[s];

    // Within-component DDs are integer …
    EXPECT_LT(fracDist(ref[0] - ref[1]), 1e-6) << "component A within-DD";
    EXPECT_LT(fracDist(ref[2] - ref[3]), 1e-6) << "component B within-DD";
    // … a cross-component pair is fractional (different, uncoupled r1 fractionals).
    const bool cross_fractional =
        fracDist(ref[0] - ref[2]) > 1e-3 || fracDist(ref[0] - ref[3]) > 1e-3 ||
        fracDist(ref[1] - ref[2]) > 1e-3 || fracDist(ref[1] - ref[3]) > 1e-3;
    EXPECT_TRUE(cross_fractional)
        << "cross-component DDs must be fractional (no data connects the two "
           "datum receivers — the accepted eq34 limitation).";
}

// ===========================================================================
// Card 3 / I1 — SHIFT-WALK (carry S-transformation) unit test.
//
// The experimental `phase_datum_eq34_carry_shift` walk claims that when the
// chain datum re-pins a tree edge, the carried quantities can be transformed
// EXACTLY into the new emission frame instead of erased.  It is an
// alternating-sign forward pass over the spanning tree (parents strictly before
// children; the datum receiver's RPB is pinned so its shift is 0):
//     Δb_s     = −Δδ_parent − pinmove(edge)     (satellite child of a receiver)
//     Δδ_r     = −Δb_parent − pinmove(edge)     (receiver child of a satellite)
//     Δz_{r,s} = −Δb_s − Δδ_r                    (every carried ambiguity)
// where `pinmove` is this pin's displacement of its pinned float vs the frame it
// last sat in.  It FIELD-FAILED (B8': 86 cyc/ep injected error).  These tests
// decide, in a controlled noise-free box, whether the ALGEBRA is exact — so the
// field failure is a model-coupling defect (iono / weak δ split feeding a wrong
// carried float), NOT a bookkeeping bug in the walk itself.
//
// Setup: the code datum (clock pin) is held fixed across the two epochs, so a_r
// and c_s are invariant and cancel — φ_{r,s} := Φ_{r,s} − (a_r − c_s) =
// b_s + δ_r + z_{r,s}.  A spanning tree over the connected bipartite graph, plus
// one RPB pin, makes the phase subsystem square and full rank, so exact tree
// propagation IS the estimator's solution (no Eigen solve needed here — GATE 1-4
// above already exercise NetworkEstimator; these target the walk arithmetic).
//
// KEY FACT that shapes the negative results: in a NOISE-FREE box every floated
// (non-tree) ambiguity lands on an exact integer (the frame's single common
// fractional δ_{r1} rides b_s and δ_r and cancels in z = φ − b − δ).  So a
// FAITHFUL carry ⇒ pin-to-round reproduces the true integer ⇒ pinmove 0 ⇒ the
// reattach is a perfect no-op (I1b).  The shift only moves — and only mis-fires
// (I1c) — when the CARRIED float has drifted off its true value, which is
// exactly what phase-leveling with iono/δ coupling does in the field.
//
// SCOPE / what these tests do and do NOT cover.  The production carry walk lives
// in two [&]-capturing lambdas inside the per-epoch election loop of
// ApplicationNetwork.cpp (recordEdge + the vshift forward pass) — there is no
// callable seam to drive from a unit test.  `shiftWalk`/`propagateChain` below
// are therefore an INDEPENDENT reference reimplementation, kept byte-faithful to
// the production recurrence.  These tests FALSIFY/CONFIRM the ALGEBRA (the
// roadmap I1 goal) and stand as an executable reference to diff production
// against — they do NOT auto-detect a regression in the shipped walk (a sign
// flip or mis-key in production would leave them green), and they deliberately
// do not exercise production's pin-to-round target derivation (they supply the
// already-rounded target), its per-satellite eq34_sat_root root gating (I1d
// blanket-flags on a single-component handover), or the station-keyed maps whose
// mis-keying was the "collision class bit twice" field bug.  Wiring the shipped
// lambdas to a testable seam is a separate (src/-touching) follow-up.
// ===========================================================================
namespace {

// One directed tree edge pinning ambiguity z_{r,s}=target.  sat_is_child mirrors
// the production recordEdge flag: true = the satellite attaches under receiver r;
// false = the receiver attaches under satellite s.  Edges are listed in
// attachment order (each edge's parent already attached or is the root).
struct ChainEdge { bool sat_is_child; int r; int s; double target; };

// A chain datum: root receiver r1 (δ_{r1}=0) + its spanning-tree edges.
struct ChainTree { int r1; std::vector<ChainEdge> edges; };

// φ_{r,s} = b_s + δ_r + z_{r,s}  (code removed; exact in the noise-free box).
inline double eq34Phi(const Eq34Truth& t, int r, int s) {
    return t.b[s] + t.d[r] + static_cast<double>(t.z[r][s]);
}

// Full eq34 state under a chain datum, by exact tree propagation.
struct Eq34FullState {
    std::vector<double> b;                       // [S] emitted sat phase-bias frame
    std::vector<double> d;                       // [R] receiver fractional δ
    std::map<std::pair<int, int>, double> z;     // (r,s) -> ambiguity, all visible
};

Eq34FullState propagateChain(const Eq34Truth& t, const ChainTree& tree) {
    Eq34FullState st;
    st.b.assign(t.S, std::numeric_limits<double>::quiet_NaN());
    st.d.assign(t.R, std::numeric_limits<double>::quiet_NaN());
    st.d[tree.r1] = 0.0;                          // RPB pin δ_{r1}=0
    for (const auto& e : tree.edges) {
        st.z[{e.r, e.s}] = e.target;              // hard ambiguity pin
        if (e.sat_is_child)                       // parent = receiver e.r (δ known)
            st.b[e.s] = eq34Phi(t, e.r, e.s) - st.d[e.r] - e.target;
        else                                      // parent = satellite e.s (b known)
            st.d[e.r] = eq34Phi(t, e.r, e.s) - st.b[e.s] - e.target;
    }
    // Floated (non-tree) ambiguities: z = φ − b − δ (integer in the noise-free box).
    for (int r = 0; r < t.R; ++r)
        for (int s : t.vis[r])
            if (!st.z.count({r, s}))
                st.z[{r, s}] = eq34Phi(t, r, s) - st.b[s] - st.d[r];
    return st;
}

// Result of the alternating-sign walk over tree B relative to incumbent tree A.
struct ShiftWalk {
    std::map<int, double> db, dd;            // Δb per sat idx, Δδ per rx idx (0 if unknown)
    std::map<int, bool>   db_known, dd_known;
    long long             n_unknown = 0;     // edges whose child was flagged unknown
};

// Independent reference reimplementation of the production carry S-transformation
// walk (ApplicationNetwork.cpp recordEdge + the vshift forward pass) on explicit
// trees — a parallel copy for algebra falsification, NOT a call into production
// (see the SCOPE note above).  `carry` supplies the value each floated ambiguity
// is believed to sit at when a re-pin lands at a NEW station — in the estimator
// this is local_carry (phase-leveling); here we pass it in so a test can feed
// either the faithful float (state_A.z) or a drifted one.
// `seed_state` (null = INC-2 off) is the SEED-ROUND frame-continuous re-host
// (docs/SPEC_eq34_seed_round.md §8.5 INC-2): where a re-pin lands at a new station
// with NO carry, the station's own ambiguity STATE supplies the pre-pin value, so
// the step is computable and the edge stays shiftable even with shift_on false.
ShiftWalk shiftWalk(const ChainTree& treeA, const ChainTree& treeB,
                    const std::map<std::pair<int, int>, double>& carry,
                    bool shift_on,
                    const std::map<std::pair<int, int>, double>* seed_state = nullptr) {
    // Incumbent maps from tree A (mirror anchor_incumbent / eq34_rx_parent /
    // eq34_sat_root / eq34_prev_target).
    std::map<int, int>    A_sat_parent;                 // sat -> its parent station
    std::map<int, int>    A_rx_parent;                  // rx  -> its parent sat
    std::map<std::pair<int, int>, double> A_target;     // (r,s) -> tree-A pin target
    for (const auto& e : treeA.edges) {
        A_target[{e.r, e.s}] = e.target;
        if (e.sat_is_child) A_sat_parent[e.s] = e.r;
        else                A_rx_parent[e.r] = e.s;
    }
    const int rootA = treeA.r1;   // every tree-A sat's recorded component root

    ShiftWalk sw;
    sw.dd[treeB.r1] = 0.0;        // datum receiver: RPB pinned 0 in both epochs
    sw.dd_known[treeB.r1] = true;

    for (const auto& e : treeB.edges) {
        // --- pinmove + known, mirroring production recordEdge (reference copy) ---
        double pinmove = 0.0;
        bool   known = true;
        bool   edge_changed;
        if (e.sat_is_child) {
            auto it = A_sat_parent.find(e.s);
            edge_changed = (it != A_sat_parent.end() && it->second != e.r);
            // Root handover: the sat's recorded root differs from this epoch's r1
            // ⇒ the frame steps by the datum receivers' fractional δ difference,
            // not an integer, not computable here.
            if (rootA != treeB.r1) known = false;
        } else {
            auto it = A_rx_parent.find(e.r);
            edge_changed = (it != A_rx_parent.end() && it->second != e.s);
        }
        bool shiftable = false;                         // INC-2: framed re-host
        if (edge_changed) {
            auto cv = carry.find({e.r, e.s});           // carry at the NEW station
            if (cv != carry.end()) pinmove = e.target - cv->second;
            else if (seed_state) {                      // INC-2: the state itself
                auto sx = seed_state->find({e.r, e.s});
                if (sx != seed_state->end()) {
                    pinmove = e.target - sx->second;
                    shiftable = true;
                } else known = false;                   // no state ⇒ cold re-host
            }
            else known = false;                         // re-pin with no carry
        } else {
            auto pt = A_target.find({e.r, e.s});        // same station: exact target diff
            pinmove = e.target - (pt != A_target.end() ? pt->second : 0.0);
        }
        // --- alternating-sign forward pass (vshift[v] = −vshift[parent] − pinmove) ---
        const int   parent_idx = e.sat_is_child ? e.r : e.s;
        const auto& pk_map = e.sat_is_child ? sw.dd_known : sw.db_known;
        const auto& pv_map = e.sat_is_child ? sw.dd : sw.db;
        const bool  pk = pk_map.count(parent_idx) ? pk_map.at(parent_idx) : false;
        const double pv = pv_map.count(parent_idx) ? pv_map.at(parent_idx) : 0.0;
        const bool  k = known && pk && (shift_on || pinmove == 0.0 || shiftable);
        const double s = k ? (-pv - pinmove) : 0.0;
        if (e.sat_is_child) { sw.db[e.s] = s; sw.db_known[e.s] = k; }
        else                { sw.dd[e.r] = s; sw.dd_known[e.r] = k; }
        if (!k) ++sw.n_unknown;
    }
    return sw;
}

// Assert state_A + shift == state_B on every KNOWN vertex + every ambiguity whose
// endpoints are both known.  Returns the count of checked cells (so a test can
// assert the check was non-vacuous).
int expectShiftReproduces(const Eq34Truth& t, const Eq34FullState& A,
                          const Eq34FullState& B, const ShiftWalk& sw, double tol) {
    int checked = 0;
    for (int s = 0; s < t.S; ++s) {
        if (!std::isfinite(A.b[s]) || !std::isfinite(B.b[s])) continue;
        if (!(sw.db_known.count(s) ? sw.db_known.at(s) : false)) continue;
        EXPECT_NEAR(A.b[s] + sw.db.at(s), B.b[s], tol) << "b_s reproduce, sat " << s;
        ++checked;
    }
    for (int r = 0; r < t.R; ++r) {
        if (!std::isfinite(A.d[r]) || !std::isfinite(B.d[r])) continue;
        if (!(sw.dd_known.count(r) ? sw.dd_known.at(r) : false)) continue;
        EXPECT_NEAR(A.d[r] + sw.dd.at(r), B.d[r], tol) << "delta_r reproduce, rx " << r;
        ++checked;
    }
    for (int r = 0; r < t.R; ++r)
        for (int s : t.vis[r]) {
            const bool sk = sw.db_known.count(s) ? sw.db_known.at(s) : false;
            const bool rk = sw.dd_known.count(r) ? sw.dd_known.at(r) : false;
            if (!sk || !rk) continue;
            const double dz = -sw.db.at(s) - sw.dd.at(r);       // Δz = −Δb_s − Δδ_r
            EXPECT_NEAR(A.z.at({r, s}) + dz, B.z.at({r, s}), tol)
                << "z reproduce, (" << r << "," << s << ")";
            ++checked;
        }
    return checked;
}

// Tree A of the makeEq34Truth() fixture, rooted at r1=0, s3 pinned at station 1
// (BFS attachment order; parents strictly precede children).  8 edges + RPB = 9.
ChainTree eq34TreeA() {
    return {0, {
        {true, 0, 0, 0.0}, {true, 0, 1, 0.0}, {true, 0, 2, 0.0},  // R0 -> S0,S1,S2
        {false, 3, 0, 0.0},                                       // S0 -> R3
        {false, 1, 2, 0.0},                                       // S2 -> R1
        {true, 3, 4, 0.0},                                        // R3 -> S4
        {true, 1, 3, 0.0},                                        // R1 -> S3
        {false, 2, 4, 0.0},                                       // S4 -> R2
    }};
}

}  // namespace

// I1a — EXACTNESS under a same-station re-pin (pin-to-round lands on a different
// integer).  Tree topology unchanged; edge S2->R1's target bumped +1 cycle.  The
// walk must propagate the alternating-sign step through depth (Δδ_1=−1 ⇒ Δb_3=+1
// ⇒ floated Δz=∓1) and reproduce the re-solved state EXACTLY.  The default-OFF
// behavior (shift_on=false) must instead flag the affected subtree unknown.
TEST(NetworkEstimatorTest, Eq34ShiftWalkReproducesRepinExactly) {
    const Eq34Truth t = makeEq34Truth();
    const ChainTree A = eq34TreeA();
    ChainTree B = A;
    B.edges[4].target = 1.0;   // S2 -> R1 (z_{1,2}) re-pinned 0 -> 1 (same station)

    const Eq34FullState sA = propagateChain(t, A);
    const Eq34FullState sB = propagateChain(t, B);

    // Shift ON: exact reproduction, and the step genuinely propagated (Δδ_1, Δb_3
    // are the −1/+1 the algebra predicts, not zero).
    const ShiftWalk on = shiftWalk(A, B, sA.z, /*shift_on=*/true);
    ASSERT_TRUE(on.dd_known.at(1)) << "rx1 must be a known (shiftable) vertex";
    ASSERT_TRUE(on.db_known.at(3)) << "sat3 must be a known (shiftable) vertex";
    EXPECT_NEAR(on.dd.at(1), -1.0, 1e-9) << "Δδ_1 = −Δb_2 − pinmove = −0 − 1";
    EXPECT_NEAR(on.db.at(3), +1.0, 1e-9) << "Δb_3 = −Δδ_1 − pinmove = +1 − 0";
    const int checked = expectShiftReproduces(t, sA, sB, on, 1e-9);
    EXPECT_GE(checked, 10) << "reproduction check must be non-vacuous";

    // Shift OFF (default): a non-zero pin displacement is treated as unknown, so
    // the re-pinned edge's subtree (rx1 and its child sat3) is flagged for erase.
    const ShiftWalk off = shiftWalk(A, B, sA.z, /*shift_on=*/false);
    EXPECT_FALSE(off.dd_known.at(1)) << "shift OFF must NOT shift rx1 (erase path)";
    EXPECT_FALSE(off.db_known.at(3)) << "shift OFF must NOT shift sat3 (erase path)";
    EXPECT_GT(off.n_unknown, 0);
}

// I1b — a CLEAN reattach with a FAITHFUL carry is a perfect no-op.  s3 moves from
// station 1 to station 2; the carry equals the estimator's true float there, so
// pin-to-round reproduces the integer it already sits at (pinmove 0) and the
// emission frame does not move.  Every vertex stays known; state is unchanged.
TEST(NetworkEstimatorTest, Eq34ShiftWalkCleanReattachIsNoOp) {
    const Eq34Truth t = makeEq34Truth();
    const ChainTree A = eq34TreeA();
    const Eq34FullState sA = propagateChain(t, A);

    // Reattach s3: drop R1->S3, attach R2->S3 (R2 already lit via S4->R2), pinning
    // z_{2,3} to round(true float there) — the pin-to-round production does.
    const double carry23 = sA.z.at({2, 3});
    ChainTree B{0, {
        {true, 0, 0, 0.0}, {true, 0, 1, 0.0}, {true, 0, 2, 0.0},
        {false, 3, 0, 0.0}, {false, 1, 2, 0.0}, {true, 3, 4, 0.0},
        {false, 2, 4, 0.0},                              // S4 -> R2
        {true, 2, 3, std::round(carry23)},               // R2 -> S3 (reattached)
    }};
    const Eq34FullState sB = propagateChain(t, B);

    const ShiftWalk sw = shiftWalk(A, B, sA.z, /*shift_on=*/true);
    EXPECT_EQ(sw.n_unknown, 0) << "a faithful-carry reattach must not erase anything";
    const int checked = expectShiftReproduces(t, sA, sB, sw, 1e-9);
    EXPECT_GE(checked, 10);
    // The frame genuinely did not move (clean integer handover).
    for (int s = 0; s < t.S; ++s) EXPECT_NEAR(sw.db.at(s), 0.0, 1e-9) << "sat " << s;
}

// I1c — the B8 FAILURE, reproduced and localized.  Same reattach as I1b, but the
// carried float has DRIFTED by ε off its true value (what phase-leveling does).
// pin-to-round still lands on the true integer, so the estimator's actual state
// is UNCHANGED (Δb_3 = 0) — but the walk computes pinmove from the drifted carry
// and predicts Δb_3 = +ε.  The shift therefore injects exactly the carry error.
// This establishes the CONDITIONAL: the alternating-sign arithmetic is exact
// given a faithful carry (I1b), and any carry drift ε propagates verbatim into
// the frame.  That is consistent with the B8 field failure being carry-driven
// (phase-leveling / iono / weak-δ coupling feeding a drifted float) — but this
// box does not by itself rule out the other B8 candidates (sat-pass churn, the
// station-keyed map collisions), so read it as mechanism-plausible, not as a
// closed attribution of the field failure.
TEST(NetworkEstimatorTest, Eq34ShiftWalkCarryDriftInjectsError) {
    const Eq34Truth t = makeEq34Truth();
    const ChainTree A = eq34TreeA();
    const Eq34FullState sA = propagateChain(t, A);

    const double true_float = sA.z.at({2, 3});          // exact integer in the box
    const double eps = 0.23;                            // leveling drift on the carry
    std::map<std::pair<int, int>, double> drifted = sA.z;
    drifted[{2, 3}] = true_float + eps;

    const double target = std::round(true_float + eps); // == true_float (|eps|<0.5)
    ChainTree B{0, {
        {true, 0, 0, 0.0}, {true, 0, 1, 0.0}, {true, 0, 2, 0.0},
        {false, 3, 0, 0.0}, {false, 1, 2, 0.0}, {true, 3, 4, 0.0},
        {false, 2, 4, 0.0}, {true, 2, 3, target},
    }};
    const Eq34FullState sB = propagateChain(t, B);       // estimator sees target only

    const ShiftWalk sw = shiftWalk(A, B, drifted, /*shift_on=*/true);
    ASSERT_TRUE(sw.db_known.at(3));
    EXPECT_NEAR(std::round(true_float), true_float, 1e-9)
        << "box invariant: the true floated carry is an exact integer";
    // Estimator truth: the frame did NOT move (pinned to the integer it sat at).
    EXPECT_NEAR(sB.b[3] - sA.b[3], 0.0, 1e-9);
    // The walk, trusting the drifted carry, predicts a non-zero step == the drift…
    EXPECT_NEAR(sw.db.at(3), eps, 1e-9) << "injected step equals the carry drift";
    // …so shift-applied b_3 mismatches the re-solved b_3 by exactly ε.
    EXPECT_NEAR((sA.b[3] + sw.db.at(3)) - sB.b[3], eps, 1e-9)
        << "carry drift ε propagates verbatim into the shifted frame (the B8 defect)";
}

// I1d — ROOT-HANDOVER negative control.  Electing a different datum receiver
// (r1: 0 -> 3) steps the whole frame by the receivers' common FRACTIONAL δ
// difference — not expressible as any integer chain transform.  The walk must
// refuse: every sat is flagged unknown (erase), never shifted.  We also confirm
// the refusal is NECESSARY — the true frame step is genuinely non-integer.
TEST(NetworkEstimatorTest, Eq34ShiftWalkRootHandoverIsUnknown) {
    const Eq34Truth t = makeEq34Truth();
    const ChainTree A = eq34TreeA();
    // Tree B rooted at r1'=3 (BFS: R3->{S0,S4}, S0->R0, S4->R2, R0->{S1,S2},
    // R2->S3, S2->R1) — the GATE-3 handover tree, as ChainEdges.
    const ChainTree B{3, {
        {true, 3, 0, 0.0}, {true, 3, 4, 0.0},
        {false, 0, 0, 0.0}, {false, 2, 4, 0.0},
        {true, 0, 1, 0.0}, {true, 0, 2, 0.0},
        {true, 2, 3, 0.0}, {false, 1, 2, 0.0},
    }};
    const Eq34FullState sA = propagateChain(t, A);
    const Eq34FullState sB = propagateChain(t, B);

    const ShiftWalk sw = shiftWalk(A, B, sA.z, /*shift_on=*/true);
    // Every satellite is flagged unknown (recorded root != new r1) ⇒ erase, not shift.
    for (int s = 0; s < t.S; ++s)
        EXPECT_FALSE(sw.db_known.count(s) && sw.db_known.at(s))
            << "sat " << s << " must be unknown on a root handover";
    EXPECT_GT(sw.n_unknown, 0);

    // The refusal is necessary: the true frame step carries a common fractional
    // (δ_{3} − δ_{0} in tree A's frame), so at least one sat's Δb is non-integer —
    // no integer walk could have reproduced it.
    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };
    bool some_fractional = false;
    for (int s = 0; s < t.S; ++s)
        if (std::isfinite(sA.b[s]) && std::isfinite(sB.b[s]) &&
            fracDist(sB.b[s] - sA.b[s]) > 1e-3)
            some_fractional = true;
    EXPECT_TRUE(some_fractional)
        << "root handover must move the frame by a non-integer common fractional "
           "(the reason the walk is right to erase rather than shift).";
}

// I1e — SEED-ROUND INC-2, the FRAME-CONTINUOUS RE-HOST (docs/SPEC_eq34_seed_round.md
// §8.5).  The carry_invariant posture has NO phase-leveling carry at all, so every
// re-host of a tree pin hits the "re-pin with no carry" arm: unknown step ⇒ the
// (sat,σ) is re-referenced and its carried phase-bias prior is marginalized for the
// epoch.  INC-2 supplies the missing pre-pin value from the new host's OWN ambiguity
// state, which makes the step exactly computable — and the walk then reproduces the
// re-solved frame with the global carry-shift knob still OFF.
//
// Non-vacuous by construction: the re-host is pinned one cycle off the float it sits
// at, so pinmove = +1 and the (pinmove == 0) door in the walk is shut.  Read this
// box together with I1c: the ε that made carry_shift inject error in the field is the
// CARRY's drift off the estimator, and INC-2's input is the estimator's own state, so
// that term is structurally absent — the residual risk INC-2's A/B measures is a
// different one (whether the real solve puts the pin displacement into b_s alone, as
// the chain algebra assumes, or spreads it across the weakly-held δ split).
TEST(NetworkEstimatorTest, Eq34ShiftWalkSeedRoundRehostFramesWithShiftOff) {
    const Eq34Truth t = makeEq34Truth();
    const ChainTree A = eq34TreeA();
    const Eq34FullState sA = propagateChain(t, A);

    // Reattach s3 from station 1 to station 2 (as I1b), but pin it a cycle off the
    // value the state holds there — a genuine frame step, not a clean handover.
    const double target23 = std::round(sA.z.at({2, 3})) + 1.0;
    const ChainTree B{0, {
        {true, 0, 0, 0.0}, {true, 0, 1, 0.0}, {true, 0, 2, 0.0},
        {false, 3, 0, 0.0}, {false, 1, 2, 0.0}, {true, 3, 4, 0.0},
        {false, 2, 4, 0.0},
        {true, 2, 3, target23},                       // R2 -> S3 (re-hosted)
    }};
    const Eq34FullState sB = propagateChain(t, B);
    const std::map<std::pair<int, int>, double> no_carry;   // carry_invariant: none

    // Production TODAY (INC-2 off): no carry at the new station ⇒ unknown ⇒ the
    // sat is re-referenced (PB prior marginalized) even though nothing about the
    // step is actually unknowable.
    const ShiftWalk off = shiftWalk(A, B, no_carry, /*shift_on=*/false);
    EXPECT_FALSE(off.db_known.count(3) && off.db_known.at(3))
        << "without INC-2 a carry-less re-host must stay on the erase/reref path";
    EXPECT_GT(off.n_unknown, 0);

    // INC-2: the state at the new host IS the pre-pin value, so the step is exact.
    const ShiftWalk inc2 = shiftWalk(A, B, no_carry, /*shift_on=*/false, &sA.z);
    ASSERT_TRUE(inc2.db_known.at(3)) << "INC-2 must frame the re-hosted sat";
    EXPECT_NEAR(inc2.db.at(3), -1.0, 1e-9)
        << "Δb_3 = −Δδ_2 − pinmove; δ_2 does not move and the pin steps +1 cycle";
    const int checked = expectShiftReproduces(t, sA, sB, inc2, 1e-9);
    EXPECT_GE(checked, 10) << "reproduction check must be non-vacuous";

    // A re-host INC-2 cannot frame (the state does not exist yet — the fold-deferred
    // cold arm) keeps the legacy refusal: the exemption is per-edge, never blanket.
    std::map<std::pair<int, int>, double> partial = sA.z;
    partial.erase({2, 3});
    const ShiftWalk cold = shiftWalk(A, B, no_carry, /*shift_on=*/false, &partial);
    EXPECT_FALSE(cold.db_known.count(3) && cold.db_known.at(3))
        << "no state at the new host ⇒ cold re-host ⇒ unknown, exactly as before";
}

// ===========================================================================
// I2 — extend-only membership (network.phase_datum_eq34_extend_only, de Jonge
// §3.5).  Production retains an incumbent tree edge's sticky identity across a
// momentary row gap while its phase-leveling carry is still alive (age-stamped
// to phase_leveling_max_gap_s), so a returning arc re-pins to its pre-gap
// integer (frame step ≈ 0) instead of re-leveling from a first-sighting 0.  The
// mirror below reproduces ONLY the retention decision (carry-alive ⇒ keep); the
// frame CONSEQUENCE of that decision is checked against the independent
// propagateChain oracle, so the gates are not self-referential.
// ===========================================================================
namespace {

// (r,s) pin target to carry into next epoch + the set kept across a gap.
struct RetainResult {
    std::map<std::pair<int, int>, double> targets;   // edge -> pin target next epoch
    std::set<std::pair<int, int>>         retained;  // edges kept across the gap
};

// Mirror of ApplicationNetwork.cpp's extend-only retention: an incumbent edge
// not seen this epoch keeps its identity iff its carry is alive; else it drops
// (a reroute erases the carry; a gap past the horizon prunes it) and re-floats.
template <class CarryAlive>
RetainResult retainIdentity(
    const std::map<std::pair<int, int>, double>& incumbentTargets,
    const std::set<std::pair<int, int>>&         seenThisEpoch,
    CarryAlive carryAlive) {
    RetainResult out;
    for (const auto& kv : incumbentTargets) {
        const std::pair<int, int>& rs = kv.first;
        if (seenThisEpoch.count(rs)) { out.targets[rs] = kv.second; continue; }  // live
        if (carryAlive(rs.first, rs.second)) {          // absent, carry alive ⇒ retain
            out.targets[rs] = kv.second;
            out.retained.insert(rs);
        }
        // else: carry gone ⇒ dropped ⇒ first sighting on return
    }
    return out;
}

}  // namespace

// I2a — a returning arc (row gap, carry alive) re-pins to its pre-gap integer,
// so the emitted between-satellite frame does NOT step; a wholesale replace
// (first-sighting target 0) re-levels it by that integer — the silent
// re-reference that poisons every peer station's surviving carry.
TEST(NetworkEstimatorTest, Eq34ExtendOnlyGapReturnStepsZero) {
    const Eq34Truth t = makeEq34Truth();
    ChainTree A = eq34TreeA();
    A.edges[6].target = 1.0;                     // pre-gap: R1->S3 pinned to integer 1
    const Eq34FullState sA = propagateChain(t, A);

    std::map<std::pair<int, int>, double> incumbent;   // mirror eq34_prev_target
    for (const auto& e : A.edges) incumbent[{e.r, e.s}] = e.target;

    // GAP epoch: S3 absent everywhere; its phase-leveling carry is still alive.
    std::set<std::pair<int, int>> seen;
    for (const auto& e : A.edges) if (e.s != 3) seen.insert({e.r, e.s});
    const RetainResult ret =
        retainIdentity(incumbent, seen, [](int, int) { return true; });
    ASSERT_TRUE(ret.retained.count({1, 3}))
        << "a carry-alive gapped edge must retain its identity";
    EXPECT_NEAR(ret.targets.at({1, 3}), 1.0, 1e-12)
        << "the retained pin target is the pre-gap integer";

    // RESUME: S3 back at R1; pin-to-round of the surviving carry reproduces the
    // retained integer ⇒ the tree and the emitted frame are unchanged (step 0).
    ChainTree resume = A;
    resume.edges[6].target = ret.targets.at({1, 3});
    const Eq34FullState sR = propagateChain(t, resume);
    EXPECT_NEAR(sR.b[3] - sA.b[3], 0.0, 1e-9)
        << "returning arc re-pins to its pre-gap integer ⇒ emitted b_3 does not step";

    // Contrast — wholesale replace forgets the identity ⇒ first-sighting 0 ⇒ b_3
    // silently re-levels by the full integer.
    ChainTree wholesale = A;
    wholesale.edges[6].target = 0.0;
    const Eq34FullState sW = propagateChain(t, wholesale);
    EXPECT_NEAR(sW.b[3] - sA.b[3], 1.0, 1e-9)
        << "wholesale replace re-levels b_3 by the pre-gap integer (step != 0)";
}

// I2b — a gap LONGER than the carry horizon (phase_leveling_max_gap_s) drops the
// identity, so the return is a genuine first sighting (target 0, frame re-levels)
// — correct, because the peer carries are gone by then too.
TEST(NetworkEstimatorTest, Eq34ExtendOnlyBeyondHorizonFirstSighting) {
    const Eq34Truth t = makeEq34Truth();
    ChainTree A = eq34TreeA();
    A.edges[6].target = 1.0;
    const Eq34FullState sA = propagateChain(t, A);

    std::map<std::pair<int, int>, double> incumbent;
    for (const auto& e : A.edges) incumbent[{e.r, e.s}] = e.target;
    std::set<std::pair<int, int>> seen;
    for (const auto& e : A.edges) if (e.s != 3) seen.insert({e.r, e.s});

    const RetainResult ret =
        retainIdentity(incumbent, seen, [](int, int) { return false; });  // carry pruned
    EXPECT_TRUE(ret.retained.empty())
        << "an edge absent past the carry horizon must NOT be retained";
    EXPECT_EQ(ret.targets.count({1, 3}), 0u)
        << "its identity is dropped ⇒ the return is a genuine first sighting";

    ChainTree resume = A;
    resume.edges[6].target = ret.targets.count({1, 3}) ? ret.targets.at({1, 3}) : 0.0;
    const Eq34FullState sR = propagateChain(t, resume);
    EXPECT_NEAR(sR.b[3] - sA.b[3], 1.0, 1e-9)
        << "beyond the horizon the returning arc re-floats from a first-sighting 0";
}

// I2c — a gap FOLLOWED BY A REROUTE (the arc returns at a DIFFERENT station with
// no carry there) must flag a re-reference: with the incumbent identity retained
// the resume is an edge change with no carry ⇒ unknown ⇒ erase (correct — the
// emitted b_s stepped by a full integer, so the peer leveling carries are stale).
// A wholesale replace would have silently dropped the incumbent and left the
// reroute unflagged — the "gap+reroute now flags a reref" gate.
TEST(NetworkEstimatorTest, Eq34ExtendOnlyGapRerouteFlagsReref) {
    const Eq34Truth t = makeEq34Truth();
    const ChainTree A = eq34TreeA();                 // S3 incumbent under R1 (edge 6)
    const ChainTree B{0, {                            // resume: S3 rerouted to R2
        {true, 0, 0, 0.0}, {true, 0, 1, 0.0}, {true, 0, 2, 0.0},
        {false, 3, 0, 0.0}, {false, 1, 2, 0.0}, {true, 3, 4, 0.0},
        {false, 2, 4, 0.0}, {true, 2, 3, 0.0},        // R2->S3 (was R1->S3)
    }};
    const Eq34FullState sA = propagateChain(t, A);
    const Eq34FullState sB = propagateChain(t, B);

    std::map<std::pair<int, int>, double> carry = sA.z;  // faithful floats…
    carry.erase({2, 3});                                 // …but none at the reroute station

    const ShiftWalk with_retain = shiftWalk(A, B, carry, /*shift_on=*/false);
    EXPECT_GT(with_retain.n_unknown, 0)
        << "gap+reroute with the incumbent retained flags a reref (no carry at R2)";

    // Necessity — with a COMMON root the reroute's receiver fractional cancels and
    // b_3 steps by an exact NON-ZERO INTEGER (the chain-path difference; here −13
    // cycles).  There is no carry at R2 to pin-to-round against, so the step is real
    // ⇒ the peer stations' leveling carries for S3 go stale by that integer ⇒ the
    // erase/reref is correct.  (This is also the Card-3 integer-user-DD property in
    // miniature: the fractional never leaks, so a re-reference is only ever integer.)
    const double step = sB.b[3] - sA.b[3];
    auto fracDist = [](double v) { return std::abs(v - std::round(v)); };
    EXPECT_GT(std::abs(step), 0.5)
        << "reroute steps b_3 by a full cycle ⇒ peer carries go stale ⇒ erase, not shift";
    EXPECT_LT(fracDist(step), 1e-6)
        << "common root ⇒ the step is an exact integer (the receiver fractional cancels)";

    // Contrast — had the incumbent been dropped (wholesale), the same resume edge
    // is a fresh first sighting (no edge change) and the reroute goes unflagged.
    ChainTree A_dropped = A;
    A_dropped.edges.erase(A_dropped.edges.begin() + 6);  // S3 not in the incumbent tree
    const ShiftWalk wholesale = shiftWalk(A_dropped, B, carry, /*shift_on=*/false);
    EXPECT_LT(wholesale.n_unknown, with_retain.n_unknown)
        << "the reref surfaces only once the incumbent identity is retained (I2)";
}

// jointConditionalCovariance (the between-station DD ambiguity covariance
// primitive for producer-side network AR) must match a dense brute-force full
// inverse of the bordered normal system conditioned on the datum.  This is the
// risk-#1 correctness check from docs/network_ambiguity_resolution_design.md §7:
// getting the conditional covariance wrong feeds LAMBDA a wrong Q → spurious
// fixes.  Exercises cross-station blocks (the term reconstructStationCovariance
// does not carry) and same-station off-diagonals.
TEST(NetworkEstimatorTest, JointConditionalCovarianceMatchesBruteForce) {
    const Truth t = makeTruth(/*seed=*/2025, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
    }
    const int nS = static_cast<int>(est.numShared());

    est.beginEpoch();
    std::vector<Eigen::MatrixXd> As(t.R), Ar(t.R);
    std::vector<Eigen::VectorXd> Wv(t.R);
    std::vector<std::size_t> ids(t.R);
    for (int r = 0; r < t.R; ++r) {
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, As[r], Ar[r], w, y);
        Wv[r] = w;
        ids[r] = est.addStation(As[r], Ar[r], w, y);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};
    (void)est.solve(datum);

    // --- Brute force: assemble the full bordered N over θ = [x_s ; locals...] ---
    std::vector<int> off(t.R);
    int total = nS;
    for (int r = 0; r < t.R; ++r) { off[r] = total; total += static_cast<int>(Ar[r].cols()); }
    Eigen::MatrixXd Nfull = Eigen::MatrixXd::Zero(total, total);
    for (int r = 0; r < t.R; ++r) {
        const int nl = static_cast<int>(Ar[r].cols());
        const Eigen::MatrixXd Wm  = Wv[r].asDiagonal();
        const Eigen::MatrixXd WAs = Wm * As[r];
        const Eigen::MatrixXd WAr = Wm * Ar[r];
        Nfull.block(0, 0, nS, nS)            += As[r].transpose() * WAs;
        Nfull.block(0, off[r], nS, nl)       += As[r].transpose() * WAr;
        Nfull.block(off[r], 0, nl, nS)       += Ar[r].transpose() * WAs;
        Nfull.block(off[r], off[r], nl, nl)  += Ar[r].transpose() * WAr;
    }
    // Condition on the datum clock (shared col p) as KNOWN: drop its row/col, invert.
    const int p = static_cast<int>(est.clockColumn(t.sats[0]));
    std::vector<int> freeIdx;
    for (int i = 0; i < total; ++i) if (i != p) freeIdx.push_back(i);
    const int nf = static_cast<int>(freeIdx.size());
    Eigen::MatrixXd Nf(nf, nf);
    for (int a = 0; a < nf; ++a)
        for (int b = 0; b < nf; ++b) Nf(a, b) = Nfull(freeIdx[a], freeIdx[b]);
    const Eigen::MatrixXd Cfull = Nf.inverse();
    std::vector<int> gToFree(total, -1);
    for (int a = 0; a < nf; ++a) gToFree[freeIdx[a]] = a;

    // --- Requests: non-pivot ambiguities across stations (local col 1+s per sat) ---
    const std::vector<std::pair<std::size_t, Eigen::Index>> req = {
        {ids[1], 1 + 0}, {ids[1], 1 + 2}, {ids[2], 1 + 0}, {ids[3], 1 + 3}, {ids[2], 1 + 4},
    };
    const Eigen::MatrixXd J = est.jointConditionalCovariance(req, datum);
    ASSERT_EQ(J.rows(), static_cast<Eigen::Index>(req.size()));
    ASSERT_EQ(J.cols(), static_cast<Eigen::Index>(req.size()));
    for (std::size_t a = 0; a < req.size(); ++a) {
        for (std::size_t b = 0; b < req.size(); ++b) {
            const int ga = off[req[a].first] + static_cast<int>(req[a].second);
            const int gb = off[req[b].first] + static_cast<int>(req[b].second);
            const double expect = Cfull(gToFree[ga], gToFree[gb]);
            EXPECT_NEAR(J(a, b), expect, 1e-4)
                << "joint cov entry (" << a << "," << b << ")"
                << (req[a].first == req[b].first ? " [same-station]" : " [cross-station]");
        }
    }
    // Symmetric by construction.
    EXPECT_LT((J - J.transpose()).cwiseAbs().maxCoeff(), 1e-12);
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
// addSharedPriorDense — the C1 full-covariance carry seam
// ===========================================================================

// A dense prior whose covariance is exactly diagonal must reproduce the scalar
// addSharedPrior path (same information, different plumbing).
TEST(NetworkEstimatorTest, DensePriorWithDiagonalCovMatchesScalarPath) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    std::vector<NetworkEstimator::SharedPrior> priors(2);
    priors[0].kind = SharedParamKind::Clock;
    priors[0].sat = t.sats[1];
    priors[0].value = 3.0;
    priors[0].info = 1.0 / 0.25;
    priors[1].kind = SharedParamKind::PhaseBias;
    priors[1].sat = t.sats[2];
    priors[1].signal = signal;
    priors[1].value = -0.4;
    priors[1].info = 1.0 / 0.04;
    Eigen::MatrixXd cov = Eigen::MatrixXd::Zero(2, 2);
    cov(0, 0) = 0.25;
    cov(1, 1) = 0.04;

    auto runEpoch = [&](bool dense) {
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
        if (dense) EXPECT_EQ(est.addSharedPriorDense(priors, cov), 2u);
        else       est.addSharedPrior(priors);
        return est.solve(datum);
    };
    const Eigen::VectorXd x_diag  = runEpoch(false);
    const Eigen::VectorXd x_dense = runEpoch(true);
    ASSERT_EQ(x_diag.size(), x_dense.size());
    EXPECT_LT((x_diag - x_dense).cwiseAbs().maxCoeff(), 1e-9);
}

// ---------------------------------------------------------------------------
// Card Q3 (docs/SPEC_clock_temporal_anchor.md §2.4) — the post-solve refloat
// primitives.  Both exist so an epoch can be RE-SOLVED with one carried prior
// withdrawn, which is what turns the clock anchor's guard from an annotation
// into a correction (adapt-and-EMIT).  Their whole contract is exactness: the
// withdrawal must leave the system in the state it would have been in had the
// prior never been folded, or the "corrected" epoch is corrected to the wrong
// thing.  Both cases therefore compare against a never-folded reference rather
// than against a tolerance on the delta.
// ---------------------------------------------------------------------------

TEST(NetworkEstimatorTest, WithdrawSharedPriorIsTheExactInverseOfTheFold) {
    const Truth t = makeTruth(/*seed=*/7717, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    NetworkEstimator::SharedPrior pr;
    pr.kind = SharedParamKind::Clock;
    pr.sat  = t.sats[2];
    pr.value = 4.25;          // deliberately far off, so a residue would show
    pr.info  = 1.0 / 4e-4;    // the anchor's own scale (σ = 2 cm)

    auto addStations = [&]() {
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
    };

    addStations();                       // reference: the prior is never folded
    const Eigen::VectorXd x_free = est.solve(datum);

    addStations();                       // fold it, solve, then withdraw + re-solve
    est.addSharedPrior({pr});
    const Eigen::VectorXd x_prior = est.solve(datum);
    EXPECT_GT((x_prior - x_free).cwiseAbs().maxCoeff(), 0.1)
        << "the prior must actually have moved the solve, or the test proves nothing";
    est.withdrawSharedPrior({pr});
    const Eigen::VectorXd x_back = est.solve(datum);
    EXPECT_LT((x_back - x_free).cwiseAbs().maxCoeff(), 1e-9);

    // …and a second solve after the withdrawal must not drift (the kernel's
    // reduced form stays current through the shared-border fast path — this is
    // what makes the in-epoch re-solve cheap enough to do at all).
    const Eigen::VectorXd x_again = est.solve(datum);
    EXPECT_LT((x_again - x_back).cwiseAbs().maxCoeff(), 1e-12);
}

TEST(NetworkEstimatorTest, RefoldDenseDroppingClocksEqualsMarginalizedFold) {
    const Truth t = makeTruth(/*seed=*/8821, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    // A CORRELATED block — clock ↔ phase bias, the coupling the C1 dense carry
    // exists to retain.  Correlation is what makes "drop the row/col of the
    // covariance" (marginalize) differ from "drop the row/col of Λ" (condition),
    // so a diagonal block would not test the property that matters.
    std::vector<NetworkEstimator::SharedPrior> keys(3);
    keys[0].kind = SharedParamKind::Clock;      keys[0].sat = t.sats[1]; keys[0].value = 1.5;
    keys[1].kind = SharedParamKind::PhaseBias;  keys[1].sat = t.sats[1]; keys[1].signal = signal;
    keys[1].value = -0.3;
    keys[2].kind = SharedParamKind::Clock;      keys[2].sat = t.sats[2]; keys[2].value = -2.0;
    Eigen::MatrixXd cov(3, 3);
    cov << 4e-4,  1.2e-4, 0.0,
           1.2e-4, 9e-4,  0.0,
           0.0,    0.0,   4e-4;

    auto addStations = [&]() {
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
    };

    // Reference: fold the block with sat[1]'s CLOCK already marginalized out.
    std::vector<NetworkEstimator::SharedPrior> kept{keys[1], keys[2]};
    Eigen::MatrixXd kept_cov(2, 2);
    kept_cov << cov(1, 1), cov(1, 2),
                cov(2, 1), cov(2, 2);
    addStations();
    est.addSharedPriorDense(kept, kept_cov);
    const Eigen::VectorXd x_ref = est.solve(datum);

    // The card's path: fold the FULL block, solve, then re-fold it marginalized.
    addStations();
    est.addSharedPriorDense(keys, cov);
    const Eigen::VectorXd x_full = est.solve(datum);
    EXPECT_GT((x_full - x_ref).cwiseAbs().maxCoeff(), 1e-3)
        << "the dropped clock prior must actually have mattered";
    EXPECT_EQ(est.refoldSharedPriorDenseDroppingClocks(keys, cov, {t.sats[1]}), 1u);
    const Eigen::VectorXd x_drop = est.solve(datum);
    EXPECT_LT((x_drop - x_ref).cwiseAbs().maxCoeff(), 1e-9);

    // The restore branch puts the epoch back exactly — the path taken when the
    // re-solve throws and the anchored solution must survive.
    est.refoldSharedPriorDenseDroppingClocks(keys, cov, {t.sats[1]}, /*restore=*/true);
    const Eigen::VectorXd x_restored = est.solve(datum);
    EXPECT_LT((x_restored - x_full).cwiseAbs().maxCoeff(), 1e-9);

    // Dropping nothing is an algebraic no-op (both Λ built by the same path).
    EXPECT_EQ(est.refoldSharedPriorDenseDroppingClocks(keys, cov, {}), 0u);
    EXPECT_EQ(est.refoldSharedPriorDenseDroppingClocks(keys, cov, {"G99"}), 0u);
    const Eigen::VectorXd x_noop = est.solve(datum);
    EXPECT_LT((x_noop - x_full).cwiseAbs().maxCoeff(), 1e-12);
}

// Absent-key policy: a key not in the layout is MARGINALIZED — its rows/cols of
// the prior covariance are dropped BEFORE inversion.  With a strong correlation
// to a present key, marginalizing leaves 1/P(0,0) of information on the
// survivor; row-deleting Λ instead would inject the far larger CONDITIONAL
// information 1/(P(0,0)·(1−ρ²)).  This is the load-bearing statistics contract.
TEST(NetworkEstimatorTest, DensePriorMarginalizesAbsentKeys) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    std::vector<NetworkEstimator::SharedPrior> keys(2);
    keys[0].kind = SharedParamKind::Clock; keys[0].sat = t.sats[1]; keys[0].value = 2.0;
    keys[1].kind = SharedParamKind::Clock; keys[1].sat = "G99";     keys[1].value = 9.0;
    Eigen::MatrixXd P(2, 2);
    P << 0.25, 0.2475,
         0.2475, 0.25;   // ρ = 0.99 with the absent key

    auto addStations = [&]() {
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
    };

    est.beginEpoch();
    addStations();
    Eigen::VectorXd lam;
    const std::size_t kept = est.addSharedPriorDense(keys, P, &lam);
    EXPECT_EQ(kept, 1u);
    ASSERT_EQ(lam.size(), 2);
    EXPECT_NEAR(lam(0), 1.0 / 0.25, 1e-7);  // marginal info, NOT 1/(0.25·(1−0.99²)) ≈ 201
    EXPECT_EQ(lam(1), 0.0);                 // absent key → dropped
    const Eigen::VectorXd x_dense = est.solve(datum);

    // Reference: the marginal scalar prior through the legacy diagonal path.
    est.beginEpoch();
    addStations();
    {
        NetworkEstimator::SharedPrior pr = keys[0];
        pr.info = 1.0 / 0.25;
        est.addSharedPrior({pr});
    }
    const Eigen::VectorXd x_ref = est.solve(datum);
    EXPECT_LT((x_dense - x_ref).cwiseAbs().maxCoeff(), 1e-9);
}

// Deterministic measurement noise on the DATA rows (the pivot's 1e10 ambiguity
// pin rows stay exact).  Noise makes the two epochs' information genuinely
// distinct — noise-free consistent data would satisfy ANY prior weighting and
// the batch-vs-recursive comparison below would lose its discrimination.
void addDataNoise(Eigen::VectorXd& y, int S, unsigned seed) {
    std::mt19937 gen(seed);
    std::normal_distribution<double> n01(0.0, 1.0);
    for (int s = 0; s < S; ++s) {
        y(2 * s)     += 0.10 * n01(gen);   // code row
        y(2 * s + 1) += 0.01 * n01(gen);   // phase row
    }
}

// THE C1 exactness property: with q = 0 and the FULL posterior covariance
// carried, the two-epoch recursion equals the single batch solve stacking both
// epochs' station data (locals per-epoch separate, shared border + datum
// common) — the information-form KF is exact recursive least squares, and the
// identical-value datum pins telescope through the Dirichlet elimination.  The
// legacy DIAGONAL carry discards the off-diagonal information and must
// measurably deviate.  This is the property that motivates the entire card.
TEST(NetworkEstimatorTest, DenseCarryRecursiveEqualsBatch) {
    const Truth t1 = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    Truth t2 = t1;   // SAME shared truth (c_s, b_s); new receiver clocks + arcs
    {
        std::mt19937 gen(777);
        std::uniform_real_distribution<double> clk(-10.0, 10.0);
        std::uniform_real_distribution<double> amb(-5.0, 5.0);
        for (int r = 0; r < t2.R; ++r) {
            t2.a[r] = clk(gen);
            for (int s = 0; s < t2.S; ++s) t2.M[r][s] = amb(gen);
        }
    }
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t1.S), pbCol(t1.S);
    for (int s = 0; s < t1.S; ++s) {
        clkCol[s] = est.registerClock(t1.sats[s]);
        pbCol[s] = est.registerPhaseBias(t1.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t1.sats[0]), 0.0}};

    // Pre-build every noisy station block ONCE: the recursive and batch paths
    // must consume bit-identical inputs.
    struct Blk { Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y; };
    std::vector<Blk> ep1(static_cast<std::size_t>(t1.R)), ep2(static_cast<std::size_t>(t2.R));
    for (int r = 0; r < t1.R; ++r) {
        buildStation(t1, r, est, clkCol, pbCol,
                     ep1[r].A_s, ep1[r].A_r, ep1[r].w, ep1[r].y);
        addDataNoise(ep1[r].y, t1.S, 100u + static_cast<unsigned>(r));
        buildStation(t2, r, est, clkCol, pbCol,
                     ep2[r].A_s, ep2[r].A_r, ep2[r].w, ep2[r].y);
        addDataNoise(ep2[r].y, t2.S, 200u + static_cast<unsigned>(r));
    }

    // Epoch 1: solve, stash the value + FULL covariance over the free columns.
    est.beginEpoch();
    for (auto& b : ep1) est.addStation(b.A_s, b.A_r, b.w, b.y);
    const Eigen::VectorXd x1 = est.solve(datum);
    const Eigen::MatrixXd P1 = est.sharedCovariance();

    std::vector<NetworkEstimator::SharedPrior> keys;
    std::vector<Eigen::Index> kcol;
    for (int s = 0; s < t1.S; ++s) {
        if (s != 0) {   // clkCol[0] is the datum pin — never carried
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::Clock; p.sat = t1.sats[s];
            p.value = x1(clkCol[s]);
            keys.push_back(p); kcol.push_back(clkCol[s]);
        }
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::PhaseBias; p.sat = t1.sats[s]; p.signal = signal;
        p.value = x1(pbCol[s]);
        keys.push_back(p); kcol.push_back(pbCol[s]);
    }
    const Eigen::Index k = static_cast<Eigen::Index>(keys.size());
    Eigen::MatrixXd Pk(k, k);
    for (Eigen::Index a = 0; a < k; ++a)
        for (Eigen::Index b = 0; b < k; ++b)
            Pk(a, b) = P1(kcol[static_cast<std::size_t>(a)], kcol[static_cast<std::size_t>(b)]);

    // Epoch 2, FULL covariance carried (q = 0): exact recursive least squares.
    est.beginEpoch();
    for (auto& b : ep2) est.addStation(b.A_s, b.A_r, b.w, b.y);
    EXPECT_EQ(est.addSharedPriorDense(keys, Pk), static_cast<std::size_t>(k));
    const Eigen::VectorXd x2_dense = est.solve(datum);

    // Epoch 2, DIAGONAL only: the legacy D2 approximation.
    est.beginEpoch();
    for (auto& b : ep2) est.addStation(b.A_s, b.A_r, b.w, b.y);
    {
        std::vector<NetworkEstimator::SharedPrior> dp = keys;
        for (Eigen::Index a = 0; a < k; ++a)
            dp[static_cast<std::size_t>(a)].info = 1.0 / Pk(a, a);
        est.addSharedPrior(dp);
    }
    const Eigen::VectorXd x2_diag = est.solve(datum);

    // Batch reference: one joint solve over both epochs' station blocks.
    NetworkEstimator estb;
    for (int s = 0; s < t1.S; ++s) {
        estb.registerClock(t1.sats[s]);
        estb.registerPhaseBias(t1.sats[s], signal);
    }
    estb.beginEpoch();
    for (auto& b : ep1) estb.addStation(b.A_s, b.A_r, b.w, b.y);
    for (auto& b : ep2) estb.addStation(b.A_s, b.A_r, b.w, b.y);
    const Eigen::VectorXd xb = estb.solve(datum);

    const double dense_err = (x2_dense - xb).cwiseAbs().maxCoeff();
    const double diag_err  = (x2_diag  - xb).cwiseAbs().maxCoeff();
    EXPECT_LT(dense_err, 1e-6) << "full-cov carry must reproduce the batch solve";
    EXPECT_GT(diag_err, 1e-7) << "diagonal carry unexpectedly matches batch — "
                                 "the fixture lost its discrimination";
    const double floor_err = (dense_err > 1e-12) ? dense_err : 1e-12;
    EXPECT_GT(diag_err, 100.0 * floor_err)
        << "the dense carry should be orders of magnitude closer to batch";
}

// Card 1 (network.carry_conditional_coupling): the CONDITIONAL local carry.  The
// sequential recursion with {dense border carry + per-column conditional coupling
// rows} must reproduce the BATCH solve for the shared border AND the carried LOCAL
// ambiguities — where the legacy diagonal (marginal) carry does NOT.  This test
// FAILING with the diagonal carry and PASSING with the coupling rows is the whole
// point of the card.
//
// Model (chosen so the coupling row is EXACT): shared per satellite = clock c_s +
// phase bias b_s; local per station = ambiguity M_{r,s}, one per satellite.  Each
// M_{r,s} appears in exactly ONE phase row (Φ = −c_s + b_s + M_{r,s}); a code row
// P = −c_s makes c_s observable.  So the local block N_rr is DIAGONAL → the carried
// ambiguities are conditionally INDEPENDENT given the shared border, the regime a
// per-column coupling row carries exactly.  Datum: pin c_0 (shared) + M_{0,s}=0
// (pivot local).  Two epochs, SAME truth, DIFFERENT noise, zero process noise → an
// exact recursive-least-squares recursion.
TEST(NetworkEstimatorTest, ConditionalCarryRecursiveEqualsBatch) {
    const int R = 3, S = 4;   // station 0 = pivot (M_{0,s}=0); 1,2 carry their M
    const std::string signal = "L1";

    std::mt19937 gen(4242);
    std::uniform_real_distribution<double> clk(-10.0, 10.0), bias(-1.0, 1.0), amb(-5.0, 5.0);
    std::vector<std::string> sats(static_cast<std::size_t>(S));
    std::vector<double> c(static_cast<std::size_t>(S)), b(static_cast<std::size_t>(S));
    std::vector<std::vector<double>> M(static_cast<std::size_t>(R), std::vector<double>(static_cast<std::size_t>(S)));
    for (int s = 0; s < S; ++s) {
        char id[8]; std::snprintf(id, sizeof(id), "G%02d", s + 1);
        sats[static_cast<std::size_t>(s)] = id;
        c[static_cast<std::size_t>(s)] = clk(gen);
        b[static_cast<std::size_t>(s)] = bias(gen);
    }
    for (int r = 0; r < R; ++r)
        for (int s = 0; s < S; ++s) M[static_cast<std::size_t>(r)][static_cast<std::size_t>(s)] = amb(gen);

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(static_cast<std::size_t>(S)), pbCol(static_cast<std::size_t>(S));
    for (int s = 0; s < S; ++s) {
        clkCol[static_cast<std::size_t>(s)] = est.registerClock(sats[static_cast<std::size_t>(s)]);
        pbCol[static_cast<std::size_t>(s)]  = est.registerPhaseBias(sats[static_cast<std::size_t>(s)], signal);
    }
    const Eigen::Index nshared = est.numShared();
    const NetworkEstimator::Datum datum = {{est.clockColumn(sats[0]), 0.0}};

    struct Blk { Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y; };
    // Build station r's block for one epoch (local block = [M_{r,0..S-1}]).
    auto build = [&](int r, unsigned seed) {
        const bool pivot = (r == 0);
        const Eigen::Index m = 2 * S + (pivot ? S : 0);
        Blk blk;
        blk.A_s = Eigen::MatrixXd::Zero(m, nshared);
        blk.A_r = Eigen::MatrixXd::Zero(m, S);
        blk.w   = Eigen::VectorXd::Ones(m);
        blk.y   = Eigen::VectorXd::Zero(m);
        std::mt19937 g(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        Eigen::Index row = 0;
        for (int s = 0; s < S; ++s) {
            const auto su = static_cast<std::size_t>(s);
            // code: P = -c_s  (σ = 0.5 m)
            blk.A_s(row, clkCol[su]) = -1.0;
            blk.w(row) = 4.0;
            blk.y(row) = -c[su] + 0.5 * nd(g);
            ++row;
            // phase: Φ = -c_s + b_s + M_{r,s}  (σ = 0.1 m)
            blk.A_s(row, clkCol[su]) = -1.0;
            blk.A_s(row, pbCol[su])  = 1.0;
            blk.A_r(row, s)          = 1.0;
            blk.w(row) = 100.0;
            blk.y(row) = -c[su] + b[su] + M[static_cast<std::size_t>(r)][su] + 0.1 * nd(g);
            ++row;
        }
        if (pivot)
            for (int s = 0; s < S; ++s) { blk.A_r(row, s) = 1.0; blk.w(row) = 1e10; blk.y(row) = 0.0; ++row; }
        return blk;
    };

    std::vector<Blk> ep1(static_cast<std::size_t>(R)), ep2(static_cast<std::size_t>(R));
    for (int r = 0; r < R; ++r) {
        ep1[static_cast<std::size_t>(r)] = build(r, 1000u + static_cast<unsigned>(r));
        ep2[static_cast<std::size_t>(r)] = build(r, 2000u + static_cast<unsigned>(r));
    }

    // --- Epoch 1: solve; stash the dense border cov + per-carried-ambiguity
    //     {value, regression M_r, conditional variance, marginal variance}. ------
    est.beginEpoch();
    std::vector<std::size_t> stidx(static_cast<std::size_t>(R));
    for (int r = 0; r < R; ++r)
        stidx[static_cast<std::size_t>(r)] = est.addStation(ep1[static_cast<std::size_t>(r)].A_s,
            ep1[static_cast<std::size_t>(r)].A_r, ep1[static_cast<std::size_t>(r)].w,
            ep1[static_cast<std::size_t>(r)].y);
    const Eigen::VectorXd x1 = est.solve(datum);
    const Eigen::MatrixXd P1 = est.sharedCovariance();

    // Border dense carry over the free shared columns (clkCol[1..], pbCol[0..]).
    std::vector<NetworkEstimator::SharedPrior> keys;
    std::vector<Eigen::Index> kcol;
    for (int s = 0; s < S; ++s) {
        const auto su = static_cast<std::size_t>(s);
        if (s != 0) {
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::Clock; p.sat = sats[su]; p.value = x1(clkCol[su]);
            keys.push_back(p); kcol.push_back(clkCol[su]);
        }
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::PhaseBias; p.sat = sats[su]; p.signal = signal; p.value = x1(pbCol[su]);
        keys.push_back(p); kcol.push_back(pbCol[su]);
    }
    const Eigen::Index k = static_cast<Eigen::Index>(keys.size());
    Eigen::MatrixXd Pk(k, k);
    for (Eigen::Index a = 0; a < k; ++a)
        for (Eigen::Index bb = 0; bb < k; ++bb)
            Pk(a, bb) = P1(kcol[static_cast<std::size_t>(a)], kcol[static_cast<std::size_t>(bb)]);

    // Per-carried-ambiguity harvest (non-pivot stations 1..R-1, all S columns).
    struct Carry { int r; Eigen::Index col; double value; double cond_var; double marg_var; Eigen::VectorXd Mr; };
    std::vector<Carry> carries;
    const Eigen::Index pin0 = est.clockColumn(sats[0]);   // the datum-pinned shared column
    for (int r = 1; r < R; ++r) {
        const std::size_t si = stidx[static_cast<std::size_t>(r)];
        const Eigen::VectorXd x1r = est.reconstructStation(si, x1);
        std::vector<Eigen::Index> cols(static_cast<std::size_t>(S));
        for (int s = 0; s < S; ++s) cols[static_cast<std::size_t>(s)] = s;
        const Eigen::VectorXd marg = est.conditionalLocalVarianceDiag(si, cols, datum);
        for (int s = 0; s < S; ++s) {
            const auto reg = est.stationRegression(si, s);
            carries.push_back(Carry{r, s, x1r(s), reg.cond_var,
                                    marg(s), reg.row});
        }
    }

    // Append one conditional coupling row per carried ambiguity to a copy of ep2.
    auto with_coupling = [&](std::vector<Blk> blks) {
        for (const Carry& cr : carries) {
            Blk& blk = blks[static_cast<std::size_t>(cr.r)];
            const Eigen::Index m0 = blk.A_s.rows();
            blk.A_s.conservativeResize(m0 + 1, Eigen::NoChange); blk.A_s.bottomRows(1).setZero();
            blk.A_r.conservativeResize(m0 + 1, Eigen::NoChange); blk.A_r.bottomRows(1).setZero();
            blk.w.conservativeResize(m0 + 1);
            blk.y.conservativeResize(m0 + 1);
            blk.A_r(m0, cr.col) = 1.0;
            double yv = cr.value;                       // (r̂_prev − r_lin), r_lin = 0
            for (Eigen::Index j = 0; j < nshared; ++j) {
                if (j == pin0) continue;                // FREE-column restriction
                const double mj = cr.Mr(j);
                blk.A_s(m0, j) = mj;
                yv += mj * x1(j);                       // + M_rj · ŝ_prev,j
            }
            blk.w(m0) = 1.0 / cr.cond_var;
            blk.y(m0) = yv;
        }
        return blks;
    };
    // The legacy diagonal (marginal) carry, for the discrimination check.
    auto with_diagonal = [&](std::vector<Blk> blks) { return blks; };

    // --- Epoch 2 (COUPLING rows + dense border): the rigorous recursion. --------
    std::vector<Blk> ep2c = with_coupling(ep2);
    est.beginEpoch();
    std::vector<std::size_t> stidx2(static_cast<std::size_t>(R));
    for (int r = 0; r < R; ++r)
        stidx2[static_cast<std::size_t>(r)] = est.addStation(ep2c[static_cast<std::size_t>(r)].A_s,
            ep2c[static_cast<std::size_t>(r)].A_r, ep2c[static_cast<std::size_t>(r)].w,
            ep2c[static_cast<std::size_t>(r)].y);
    EXPECT_EQ(est.addSharedPriorDense(keys, Pk), static_cast<std::size_t>(k));
    const Eigen::VectorXd x2_coupled = est.solve(datum);
    std::vector<Eigen::VectorXd> loc_coupled(static_cast<std::size_t>(R));
    for (int r = 1; r < R; ++r)
        loc_coupled[static_cast<std::size_t>(r)] =
            est.reconstructStation(stidx2[static_cast<std::size_t>(r)], x2_coupled);

    // --- Epoch 2 (DIAGONAL marginal carry + dense border): the legacy path. -----
    std::vector<Blk> ep2d = with_diagonal(ep2);
    est.beginEpoch();
    std::vector<std::size_t> stidxd(static_cast<std::size_t>(R));
    for (int r = 0; r < R; ++r)
        stidxd[static_cast<std::size_t>(r)] = est.addStation(ep2d[static_cast<std::size_t>(r)].A_s,
            ep2d[static_cast<std::size_t>(r)].A_r, ep2d[static_cast<std::size_t>(r)].w,
            ep2d[static_cast<std::size_t>(r)].y);
    for (const Carry& cr : carries) {
        const std::size_t si = stidxd[static_cast<std::size_t>(cr.r)];
        Eigen::VectorXd dN(1), du(1);
        dN(0) = 1.0 / cr.marg_var;                 // MARGINAL variance (the double-count)
        du(0) = (1.0 / cr.marg_var) * cr.value;
        est.addStationPrior(si, {cr.col}, dN, du);
    }
    est.addSharedPriorDense(keys, Pk);
    const Eigen::VectorXd x2_diag = est.solve(datum);
    std::vector<Eigen::VectorXd> loc_diag(static_cast<std::size_t>(R));
    for (int r = 1; r < R; ++r)
        loc_diag[static_cast<std::size_t>(r)] = est.reconstructStation(stidxd[static_cast<std::size_t>(r)], x2_diag);

    // --- Batch: each station added ONCE with BOTH epochs' rows stacked (its M
    //     persists; the shared border persists) — the joint reference. ----------
    NetworkEstimator estb;
    for (int s = 0; s < S; ++s) {
        estb.registerClock(sats[static_cast<std::size_t>(s)]);
        estb.registerPhaseBias(sats[static_cast<std::size_t>(s)], signal);
    }
    estb.beginEpoch();
    std::vector<std::size_t> bidx(static_cast<std::size_t>(R));
    for (int r = 0; r < R; ++r) {
        const Blk& a = ep1[static_cast<std::size_t>(r)];
        const Blk& bB = ep2[static_cast<std::size_t>(r)];
        const Eigen::Index m1 = a.A_s.rows(), m2 = bB.A_s.rows();
        Blk cb;
        cb.A_s.resize(m1 + m2, nshared); cb.A_s << a.A_s, bB.A_s;
        cb.A_r.resize(m1 + m2, S);       cb.A_r << a.A_r, bB.A_r;
        cb.w.resize(m1 + m2);            cb.w   << a.w,   bB.w;
        cb.y.resize(m1 + m2);            cb.y   << a.y,   bB.y;
        bidx[static_cast<std::size_t>(r)] = estb.addStation(cb.A_s, cb.A_r, cb.w, cb.y);
    }
    const Eigen::VectorXd xb = estb.solve(datum);

    // --- Assertions -----------------------------------------------------------
    const double shared_coupled = (x2_coupled - xb).cwiseAbs().maxCoeff();
    const double shared_diag    = (x2_diag    - xb).cwiseAbs().maxCoeff();
    double local_coupled = 0.0, local_diag = 0.0;
    for (int r = 1; r < R; ++r) {
        const Eigen::VectorXd lb = estb.reconstructStation(bidx[static_cast<std::size_t>(r)], xb);
        local_coupled = std::max(local_coupled,
            (loc_coupled[static_cast<std::size_t>(r)] - lb).cwiseAbs().maxCoeff());
        local_diag = std::max(local_diag,
            (loc_diag[static_cast<std::size_t>(r)] - lb).cwiseAbs().maxCoeff());
    }
    EXPECT_LT(shared_coupled, 1e-9)
        << "conditional coupling rows must reproduce the batch shared solve";
    EXPECT_LT(local_coupled, 1e-9)
        << "conditional coupling rows must reproduce the batch LOCAL (ambiguity) solve";
    EXPECT_GT(shared_diag + local_diag, 1e-6)
        << "the diagonal (marginal) carry should NOT match batch — the fixture lost its "
           "discrimination (double-counting the shared border via the marginal variance)";
}

// ── Card 1c ──────────────────────────────────────────────────────────────────
// Numerical trace of the between-epoch conditional-coupling carry (Card 1,
// network.carry_conditional_coupling) over an 8-epoch SURVIVING arc.  Field fact
// (memory project_network_ddfrac_rootcause_shvtec): arcs survive 16-43 epochs,
// dep_var≈0 (clock_model off ⇒ M_w empty, Card-1b joint whitening a no-op), yet the
// DD formal σ stays FLAT at var_eff≈5.6 cyc rather than contracting ~1/√k.  This test
// drives the PRODUCTION harvest→inject path each epoch — solve → stationRegression
// (cond_var + M_r) + conditionalLocalVarianceDiag → inject a coupling row next epoch,
// plus the C1 dense-border carry — and PRINTS, per epoch for the tracked ambiguity:
// the injected row σ² (last epoch's q_cond), the harvested q_cond, the M_r
// coefficients, the jointConditionalCovariance σ (the dd_sigma path), and the value
// error vs truth.  The σ(k) trajectory is read directly from the table.
//
// Geometry: 2 stations, 2 satellites.  Gauge = datum pins sat-0's clock (clk0) + station 0
// pins sat-0's phase bias (pb0=0) — so BOTH stations' sat-1 ambiguities stay FREE and carried,
// letting the DD N_{1,1}−N_{0,1} cancel the shared clk1/pb1 EXACTLY (the field dd_sigma
// contrast).  The FREE carried border the arc regresses onto is exactly {clk_1 (LARGE per-epoch
// q ⇒ loosely carried), pb_1 (small q ⇒ tightly carried)} — the card's "1 clock-like + 1
// bias-like column".  Two σ's are traced: the single-ambiguity conditional σ (carries the
// shared-border propagation) and the border-cancelling DD σ.  Per station per sat: a phase row
// (σ 0.002 m; amb + iono + clock + bias) and a code row (σ 0.3 m; iono + clock).  Zero
// ambiguity process noise (constant arc).  The `correlated_iono` variant (the branch-(a)
// probe) ties BOTH stations' ionos to ONE carried SH-like shared column with a
// representativeness σ — the correlated per-epoch iono error hypothesised to be the
// field floor.  PURE DIAGNOSTIC: asserts only that the harvest stayed finite/positive
// (green in every branch); the verdict is read from the printed table.
TEST(NetworkEstimatorTest, ConditionalCarryContractionTrace) {
    struct Blk { Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y; };
    const int NEP = 8;
    const double sig_ph = 0.002, sig_cd = 0.3;
    // clk1 is confounded with common-mode iono in this 2-sat toy (a single free satellite's
    // clock is unobservable), so it is carried with a SMALL q that holds its variance at a
    // stable nonzero floor (~seed 1 m²) — that floor is what the single-ambiguity σ can never
    // beat, while the between-station DD cancels clk1 and pb1 exactly.  pb1 = the small-q
    // carried bias.  (A truly large-q free clock would diverge here; the field breaks the
    // clock/iono null with many satellites + the SH-VTEC constraint, which this toy omits.)
    const double q_clk = 1e-3, q_pb = 1e-8;
    const double pb_true[2]   = { 0.37, -0.52 };
    const double N_true[2][2] = { {0.0, 0.0}, {2.3, -4.1} };  // gauge: pivot r0 ambiguities = 0

    // Card 1d: `fixed` arms inject each coupling row with the border TIME UPDATE
    // (per-column κ shrink + M²κq inflation, q ladder = the border carry's own
    // {q_clk, q_pb, q_sh}) — the docs/SPEC_card1_coupling_carry.md fix.
    struct Trace { std::vector<double> single_sig, dd_sig, err_ratio; };
    auto run_trace = [&](bool correlated_iono, bool fixed) -> Trace {
        NetworkEstimator est;
        const Eigen::Index clk0 = est.registerClock("G01");
        const Eigen::Index clk1 = est.registerClock("G02");
        const Eigen::Index pb0  = est.registerPhaseBias("G01", "L1");
        const Eigen::Index pb1  = est.registerPhaseBias("G02", "L1");
        const Eigen::Index shc  = correlated_iono ? est.registerClock("SH99") : Eigen::Index(-1);
        const Eigen::Index nshared = est.numShared();
        const NetworkEstimator::Datum datum = {{clk0, 0.0}};
        const Eigen::Index clkc[2] = { clk0, clk1 };
        const Eigen::Index pbc[2]  = { pb0, pb1 };
        // per-station local layout: 0,1 = ambiguities N_{r,0},N_{r,1}; 2,3 = ionos I_{r,0},I_{r,1}
        auto ncol = [](int s){ return static_cast<Eigen::Index>(s); };
        auto icol = [](int s){ return static_cast<Eigen::Index>(2 + s); };
        const Eigen::Index NL = 4;

        std::mt19937 gen(90210u + (correlated_iono ? 7u : 0u));
        std::normal_distribution<double> ndph(0.0, sig_ph), ndcd(0.0, sig_cd);
        std::normal_distribution<double> ndI(0.0, 5.0), nd1(0.0, 1.0), ndrep(0.0, 0.1);
        // Model-CONSISTENT truths (mirroring the Card-1c falsifier convention,
        // card1c_falsify.py): clk0 ≡ 0 (the datum), clk1 = a genuine RW(q_clk)
        // seeded N(0,1) (exactly the border carry's model), pb constant.  The SH
        // truth stays WHITE N(0,25) against the border's RW q_sh=5 — Opus's probe
        // mismatch, kept DELIBERATELY: it is the representativeness error the σ
        // trajectory is graded under.  With consistent truths the dd_val_err/σ
        // ratio is a meaningful consistency metric (formal σ itself is truth-
        // independent, so the printed σ trajectories are unchanged).
        double clk1_true = nd1(gen);

        struct Cpl { Eigen::Index col; double value; double cond_var; Eigen::VectorXd Mr; };
        std::vector<std::vector<Cpl>> carry(2);          // per-station coupling rows (empty at ep0)
        Eigen::VectorXd prev_xs;                         // last epoch's shared solution (ŝ_prev RHS)
        Eigen::VectorXd prev_pdiag;                      // Card 1d: last epoch's posterior border variances (κ inputs)

        // Gauge: datum pins clk0 (reference clock); station 0 pins pb0=0 (reference bias) with a
        // tight row.  This leaves BOTH stations' sat-1 ambiguities FREE and carried, so the DD
        // N_{1,1}−N_{0,1} cancels the shared clk1/pb1 EXACTLY (the field dd_sigma contrast) — the
        // decisive check that the single-ambiguity plateau is the shared-border-propagation floor.
        //
        // Dense border carry = {clk1 (large q), pb1 (small q, the carried bias) (+SH)}.  Seeded
        // with a weak diagonal prior so epoch 0 is full-rank (the free sat's clock/bias share the
        // classic null until the small-q bias is carried); rebuilt from posterior + q each epoch.
        std::vector<NetworkEstimator::SharedPrior> bkeys;
        std::vector<Eigen::Index> bcol;
        auto addKey = [&](SharedParamKind kind, const std::string& sat, const std::string& sig, Eigen::Index col){
            NetworkEstimator::SharedPrior p; p.kind = kind; p.sat = sat; p.signal = sig; p.value = 0.0;
            bkeys.push_back(p); bcol.push_back(col);
        };
        addKey(SharedParamKind::Clock, "G02", "", clk1);
        addKey(SharedParamKind::PhaseBias, "G02", "L1", pb1);
        if (correlated_iono) addKey(SharedParamKind::Clock, "SH99", "", shc);
        const Eigen::Index k = static_cast<Eigen::Index>(bcol.size());
        // Epoch-0 seed prior: TIGHT (variance 1 m²) on every free border column so the
        // uncarried (clk1,pb1) gauge null is PD for the first solve.  From epoch 1 the q
        // inflation (clk1 += q_clk = 1e4) loosens the clock to the intended large-q regime;
        // the DD cancels clk1, so its epoch-0 seed does not bias the tracked contrast.
        // The SH seed matches its white-truth variance (25 m²) so epoch 0 is honest —
        // the probe's mismatch is the RW q=5 CARRY, not the seed.
        Eigen::MatrixXd Pk = Eigen::MatrixXd::Identity(k, k);
        if (correlated_iono) Pk(k - 1, k - 1) = 25.0;   // bcol.back() == shc

        std::vector<double> single_traj, dd_traj, err_ratio;
        std::printf("\n=== Card 1c contraction trace | correlated_iono=%d fixed=%d | DD sat1 truth N_{1,1}-N_{0,1}=%+.3f m ===\n",
                    correlated_iono ? 1 : 0, fixed ? 1 : 0, N_true[1][1] - N_true[0][1]);
        std::printf(" ep | inj_sig2   q_cond(N11) | Mr[clk1  pb1    sh  ] |  single_sig  |   DD_sig    | DD_val_err\n");

        for (int ep = 0; ep < NEP; ++ep) {
            if (ep > 0) clk1_true += std::sqrt(q_clk) * nd1(gen);
            const double clk[2] = { 0.0, clk1_true };
            double I[2][2];
            for (int r = 0; r < 2; ++r) for (int s = 0; s < 2; ++s) I[r][s] = ndI(gen);
            const double sh_true = correlated_iono ? ndI(gen) : 0.0;

            auto build = [&](int r) {
                const bool ref = (r == 0);                       // station 0 also pins pb0 = 0 (bias gauge)
                const Eigen::Index nrow = 4 + (correlated_iono ? 2 : 0) + (ref ? 1 : 0);
                Blk b; b.A_s = Eigen::MatrixXd::Zero(nrow, nshared); b.A_r = Eigen::MatrixXd::Zero(nrow, NL);
                b.w = Eigen::VectorXd::Ones(nrow); b.y = Eigen::VectorXd::Zero(nrow);
                Eigen::Index row = 0;
                for (int s = 0; s < 2; ++s) {
                    // code: P = -clk_s + I_{r,s}   (σ 0.3)
                    b.A_s(row, clkc[s]) = -1.0; b.A_r(row, icol(s)) = 1.0;
                    b.w(row) = 1.0 / (sig_cd * sig_cd);
                    b.y(row) = -clk[s] + I[r][s] + ndcd(gen); ++row;
                    // phase: Φ = -clk_s + pb_s + N_{r,s} - I_{r,s}   (σ 0.002)
                    b.A_s(row, clkc[s]) = -1.0; b.A_s(row, pbc[s]) = 1.0;
                    b.A_r(row, ncol(s)) = 1.0; b.A_r(row, icol(s)) = -1.0;
                    b.w(row) = 1.0 / (sig_ph * sig_ph);
                    b.y(row) = -clk[s] + pb_true[s] + N_true[r][s] - I[r][s] + ndph(gen); ++row;
                }
                if (correlated_iono) {
                    // Pin I_{r,s} = map_r·SH + rep.  The mapping map_r DIFFERS per station (the
                    // field's SH→slant map at each station's pierce point), so SH does NOT cancel
                    // in the between-station DD (unlike the sat clock) — the ambiguity DD inherits
                    // (map_1−map_0)·SH.  SH is carried with a LARGE q (re-applied ~white against a
                    // representativeness σ), so its posterior variance never converges to 0 → a
                    // PERSISTENT floor on the DD σ.  This is the project_network_ddfrac_rootcause_shvtec
                    // SH-VTEC iono-error mechanism.
                    const double map_r = (r == 0) ? 1.0 : 1.4;
                    for (int s = 0; s < 2; ++s) {
                        b.A_r(row, icol(s)) = 1.0; b.A_s(row, shc) = -map_r;
                        b.w(row) = 1.0 / (0.3 * 0.3);           // representativeness σ 0.3 m (loose pin)
                        b.y(row) = (I[r][s] - map_r * sh_true) + ndrep(gen); ++row;
                    }
                }
                if (ref) { b.A_s(row, pb0) = 1.0; b.w(row) = 1e10; b.y(row) = 0.0; ++row; }  // pin pb0 = 0
                return b;
            };
            std::vector<Blk> blk = { build(0), build(1) };

            // Inject BOTH stations' coupling rows from last epoch's harvest.
            double inj_sig2_11 = -1.0;
            for (int r = 0; r < 2; ++r)
                for (const Cpl& cr : carry[static_cast<std::size_t>(r)]) {
                    if (r == 1 && cr.col == ncol(1)) inj_sig2_11 = cr.cond_var;
                    Blk& b = blk[static_cast<std::size_t>(r)];
                    const Eigen::Index m0 = b.A_s.rows();
                    b.A_s.conservativeResize(m0 + 1, Eigen::NoChange); b.A_s.bottomRows(1).setZero();
                    b.A_r.conservativeResize(m0 + 1, Eigen::NoChange); b.A_r.bottomRows(1).setZero();
                    b.w.conservativeResize(m0 + 1); b.y.conservativeResize(m0 + 1);
                    b.A_r(m0, cr.col) = 1.0;
                    double yv = cr.value;                               // (r̂_prev − r_lin), r_lin = 0
                    double tu = 0.0;                                    // Card 1d: Σ M²·κ·q inflation
                    for (Eigen::Index j = 0; j < nshared; ++j) {
                        if (j == clk0 || j == pb0) continue;            // FREE-column restriction (both pinned)
                        const double mj = cr.Mr(j);
                        if (fixed) {
                            // Border time update: same per-column q the Pk rebuild applies below.
                            const double qj = (j == clk1) ? q_clk : (j == pb1) ? q_pb
                                            : (j == shc)  ? 5.0   : 0.0;
                            const double pj = (prev_pdiag.size() > j) ? prev_pdiag(j) : 0.0;
                            if (qj > 0.0 && pj > 0.0) {
                                const double kap = pj / (pj + qj);
                                b.A_s(m0, j) = kap * mj; yv += kap * mj * prev_xs(j);
                                tu += mj * mj * kap * qj;
                                continue;
                            }
                        }
                        b.A_s(m0, j) = mj; yv += mj * prev_xs(j);       // + M_rj · ŝ_prev,j
                    }
                    b.w(m0) = 1.0 / (cr.cond_var + tu); b.y(m0) = yv;
                }

            est.beginEpoch();
            const std::size_t s0 = est.addStation(blk[0].A_s, blk[0].A_r, blk[0].w, blk[0].y);
            const std::size_t s1 = est.addStation(blk[1].A_s, blk[1].A_r, blk[1].w, blk[1].y);
            est.addSharedPriorDense(bkeys, Pk);
            const Eigen::VectorXd xs = est.solve(datum);

            // Single-ambiguity σ (includes shared-border propagation) vs the between-station DD σ
            // N_{1,1}−N_{0,1} (cancels the shared clk1/pb1 — the field dd_sigma contrast).
            const Eigen::MatrixXd J1 = est.jointConditionalCovariance({{s1, ncol(1)}}, datum);
            const double single_sig = (J1.rows() == 1 && J1(0, 0) > 0.0) ? std::sqrt(J1(0, 0)) : -1.0;
            const Eigen::MatrixXd J2 =
                est.jointConditionalCovariance({{s0, ncol(1)}, {s1, ncol(1)}}, datum);
            double dd_sig = -1.0;
            if (J2.rows() == 2) {
                const double vdd = J2(0, 0) + J2(1, 1) - 2.0 * J2(0, 1);   // g = [+1,-1]
                dd_sig = (vdd > 0.0) ? std::sqrt(vdd) : -1.0;
            }

            // Harvest BOTH stations' ambiguities → next epoch's coupling rows (production path).
            const Eigen::VectorXd x0 = est.reconstructStation(s0, xs);
            const Eigen::VectorXd x1 = est.reconstructStation(s1, xs);
            const std::vector<Eigen::Index> ambcols = { ncol(0), ncol(1) };
            (void)est.conditionalLocalVarianceDiag(s1, ambcols, datum);   // exercise the marginal harvest too
            std::vector<std::vector<Cpl>> next(2);
            double q_cond_11 = -1.0; Eigen::VectorXd Mr_11 = Eigen::VectorXd::Zero(nshared);
            for (int r = 0; r < 2; ++r) {
                const std::size_t sid = (r == 0) ? s0 : s1;
                const Eigen::VectorXd& xl = (r == 0) ? x0 : x1;
                for (int s = 0; s < 2; ++s) {
                    const auto reg = est.stationRegression(sid, ncol(s));
                    next[static_cast<std::size_t>(r)].push_back(Cpl{ ncol(s), xl(ncol(s)), reg.cond_var, reg.row });
                    if (r == 1 && s == 1) { q_cond_11 = reg.cond_var; Mr_11 = reg.row; }
                }
            }
            const double dd_val_err = (x1(ncol(1)) - x0(ncol(1))) - (N_true[1][1] - N_true[0][1]);

            // Rebuild the dense border {clk1, pb1 (+SH)} from the posterior + q inflation.
            const Eigen::MatrixXd P = est.sharedCovariance();
            for (std::size_t a = 0; a < bkeys.size(); ++a) bkeys[a].value = xs(bcol[a]);
            for (Eigen::Index a = 0; a < k; ++a)
                for (Eigen::Index bb = 0; bb < k; ++bb) Pk(a, bb) = P(bcol[a], bcol[bb]);
            for (Eigen::Index a = 0; a < k; ++a)
                Pk(a, a) += (bcol[a] == clk1) ? q_clk : (bcol[a] == shc ? 5.0 : q_pb);
            carry = std::move(next);
            prev_xs = xs;
            prev_pdiag = P.diagonal();   // Card 1d: κ inputs for the next inject

            const double mr_sh = (shc >= 0) ? Mr_11(shc) : 0.0;
            std::printf("  %d | %.3e %.4e | %+7.3f %+6.3f %+6.3f | %11.4e | %11.4e | %+9.4f\n",
                        ep, inj_sig2_11, q_cond_11, Mr_11(clk1), Mr_11(pb1), mr_sh,
                        single_sig, dd_sig, dd_val_err);
            single_traj.push_back(single_sig); dd_traj.push_back(dd_sig);
            err_ratio.push_back((dd_sig > 0.0) ? std::abs(dd_val_err) / dd_sig : -1.0);
            EXPECT_TRUE(std::isfinite(q_cond_11) && q_cond_11 > 0.0)
                << "harvest produced a non-finite/non-positive conditional variance at ep " << ep;
            EXPECT_TRUE(std::isfinite(single_sig) && std::isfinite(dd_sig))
                << "jointConditionalCovariance non-finite at ep " << ep;
        }
        auto ratio = [&](const std::vector<double>& v) {
            return (v.size() == static_cast<std::size_t>(NEP) && v.front() > 0 && v.back() > 0)
                       ? v.front() / v.back() : 0.0;
        };
        std::printf(" contraction σ(0)/σ(7):  single=%.2f   DD=%.2f   (1/√8 accumulation ⇒ ~%.2f)\n",
                    ratio(single_traj), ratio(dd_traj), std::sqrt(static_cast<double>(NEP)));
        return Trace{single_traj, dd_traj, err_ratio};
    };

    const Trace plain     = run_trace(false, false);
    const Trace corr      = run_trace(true,  false);
    const Trace plain_fix = run_trace(false, true);
    const Trace corr_fix  = run_trace(true,  true);
    (void)plain;

    // ── Card 1d assertions (the unfixed arms above stay pure diagnostics) ──────
    auto maxr = [](const std::vector<double>& v) {
        double m = 0.0; for (double x : v) m = std::max(m, x); return m;
    };
    auto contraction = [&](const std::vector<double>& v) {
        return (!v.empty() && v.front() > 0 && v.back() > 0) ? v.front() / v.back() : 0.0;
    };
    // The asymmetric correlated-iono probe (map 1.0 vs 1.4) leaks the stale-row
    // defect into the DD (the "σ-incest" ±5 m, 30-50× overconfident signature) —
    // the discrimination arm.  The fix must keep the SAME probe honest.
    EXPECT_GT(maxr(corr.err_ratio), 4.0)
        << "unfixed correlated-iono arm should be grossly overconfident (the Card-1c "
           "defect) — the fixture lost its discrimination";
    EXPECT_LT(maxr(corr_fix.err_ratio), 3.0)
        << "Card 1d: fixed correlated-iono arm must stay consistent (|err|/sigma <= 3)";
    EXPECT_LT(maxr(plain_fix.err_ratio), 3.0)
        << "Card 1d: fixed plain arm must stay consistent (|err|/sigma <= 3)";
    // Honest accumulation: the DD σ keeps contracting under the fix (no incest
    // floor, no divergence) in both geometries.
    EXPECT_GT(contraction(plain_fix.dd_sig), 1.8)
        << "Card 1d: fixed plain arm DD sigma should contract ~1/sqrt(k)";
    EXPECT_GT(contraction(corr_fix.dd_sig), 1.3)
        << "Card 1d: fixed correlated-iono arm DD sigma should still contract";
}

// ── Card 1d ──────────────────────────────────────────────────────────────────
// Border TIME UPDATE of the coupling rows (network.carry_coupling_time_update).
// ConditionalCarryRecursiveEqualsBatch above passes only because its shared
// border is STATIC between epochs (zero process noise) — the exact test gap that
// shipped the Card-1c defect.  Here the border MOVES: c_{s,2} = c_{s,1} + w,
// w ~ N(0, q) with the card-spec LARGE q (1e4 m², a free-running clock), and the
// recursion equals batch only when each injected coupling row is re-independenced
// against the moved border: per-column shrink κ_j = P_prev/(P_prev + q) applied
// to the coefficient AND its ŝ_prev RHS term, plus the row-variance inflation
// M²·κ_j·q = Mᵀ(P⁻¹+Q⁻¹)⁻¹M.  Geometry chosen so the scalar per-column κ is
// EXACT: ONE carried station (no cross-station conditional correlation to drop),
// shared = one clock per satellite ONLY, each ambiguity couples to exactly one
// clock, and different satellites never share a row or a local — so the border
// posterior is diagonal across the coupled columns and each row has a single
// coupled column.  The batch reference is hand-built per satellite over
// (c₁, c₂, M₀, M₁) with an explicit link row c₂−c₁ ~ N(0,q).  The RAW (unfixed
// Card-1) recursion must FAIL against the moving border — that failing arm IS
// the Card-1c defect.
namespace card1d {

struct Blk { Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y; };

// Coupling-row construction mode for epoch 2.
enum class Mode {
    Raw,       ///< unfixed Card 1: full-strength harvested row, σ² = cond_var
    Fixed,     ///< Card 1d: κ-shrunk coefficients + M²κq inflation
    Departed   ///< full marginalization: coupled terms dropped, σ² += M²·P_prev
};

constexpr int R = 2, S = 3;   // station 0 = pivot (M₀ₛ pinned), station 1 carried
constexpr double W_CD = 4.0, W_PH = 100.0, W_PIN = 1e10;

struct Fixture {
    std::vector<std::string> sats;
    std::vector<double> c1, c2;              // per-sat truth clock, epochs 1 / 2
    double M[R][S];                          // truth ambiguities (row 0 = gauge 0)
    double q;                                // clock process noise between epochs (m²)

    explicit Fixture(double q_carry, double truth_drift_sigma, unsigned seed = 777u)
            : q(q_carry) {
        std::mt19937 gen(seed);
        std::uniform_real_distribution<double> clk(-10.0, 10.0), amb(-5.0, 5.0);
        std::normal_distribution<double> nd(0.0, 1.0);
        sats.resize(S);
        c1.resize(static_cast<std::size_t>(S));
        c2.resize(static_cast<std::size_t>(S));
        for (int s = 0; s < S; ++s) {
            char id[8]; std::snprintf(id, sizeof(id), "G%02d", s + 1);
            sats[static_cast<std::size_t>(s)] = id;
            c1[static_cast<std::size_t>(s)] = clk(gen);
            c2[static_cast<std::size_t>(s)] =
                c1[static_cast<std::size_t>(s)] + truth_drift_sigma * nd(gen);
        }
        for (int s = 0; s < S; ++s) { M[0][s] = 0.0; M[1][s] = amb(gen); }
    }

    // One station-epoch block: per sat a code row P = −c_s (σ 0.5) and a phase
    // row Φ = −c_s + M_{r,s} (σ 0.1); the pivot adds M₀ₛ = 0 pins (w 1e10).
    Blk build(int r, const std::vector<double>& c, unsigned seed,
              const std::vector<Eigen::Index>& clkCol, Eigen::Index nshared) const {
        const bool pivot = (r == 0);
        const Eigen::Index m = 2 * S + (pivot ? S : 0);
        Blk b;
        b.A_s = Eigen::MatrixXd::Zero(m, nshared);
        b.A_r = Eigen::MatrixXd::Zero(m, S);
        b.w   = Eigen::VectorXd::Ones(m);
        b.y   = Eigen::VectorXd::Zero(m);
        std::mt19937 g(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        Eigen::Index row = 0;
        for (int s = 0; s < S; ++s) {
            const auto su = static_cast<std::size_t>(s);
            b.A_s(row, clkCol[su]) = -1.0;
            b.w(row) = W_CD;
            b.y(row) = -c[su] + 0.5 * nd(g); ++row;
            b.A_s(row, clkCol[su]) = -1.0; b.A_r(row, s) = 1.0;
            b.w(row) = W_PH;
            b.y(row) = -c[su] + M[r][s] + 0.1 * nd(g); ++row;
        }
        if (pivot)
            for (int s = 0; s < S; ++s) { b.A_r(row, s) = 1.0; b.w(row) = W_PIN; b.y(row) = 0.0; ++row; }
        return b;
    }
};

// Harvested carry of one epoch-1 ambiguity of the carried station.
struct Carry {
    Eigen::Index col;      ///< local (A_r) column = satellite index
    double value;          ///< reconstructed absolute value
    double cond_var;       ///< pure conditional variance (N_rr⁻¹)_cc
    Eigen::VectorXd Mr;    ///< regression on the shared border (spans nshared)
};

// Append one coupling row per carry to the carried station's epoch-2 block,
// built per `mode` (κ uses the harvest-epoch border variances `pdiag` and the
// per-column process noise `q`).
inline void appendCplRows(Blk& blk, const std::vector<Carry>& carries,
                          const Eigen::VectorXd& x1, const Eigen::VectorXd& pdiag,
                          double q, Mode mode) {
    for (const Carry& cr : carries) {
        const Eigen::Index m0 = blk.A_s.rows();
        blk.A_s.conservativeResize(m0 + 1, Eigen::NoChange); blk.A_s.bottomRows(1).setZero();
        blk.A_r.conservativeResize(m0 + 1, Eigen::NoChange); blk.A_r.bottomRows(1).setZero();
        blk.w.conservativeResize(m0 + 1);
        blk.y.conservativeResize(m0 + 1);
        blk.A_r(m0, cr.col) = 1.0;
        double yv = cr.value;                       // (r̂_prev − r_lin), r_lin = 0
        double sig2 = cr.cond_var;
        for (Eigen::Index j = 0; j < cr.Mr.size(); ++j) {
            const double mj = cr.Mr(j);
            if (mj == 0.0) continue;
            switch (mode) {
                case Mode::Raw:
                    blk.A_s(m0, j) = mj; yv += mj * x1(j);
                    break;
                case Mode::Fixed: {
                    const double kap = pdiag(j) / (pdiag(j) + q);   // κ = 1 exactly at q = 0
                    blk.A_s(m0, j) = kap * mj; yv += kap * mj * x1(j);
                    sig2 += mj * mj * kap * q;
                    break;
                }
                case Mode::Departed:
                    sig2 += mj * mj * pdiag(j);
                    break;
            }
        }
        blk.w(m0) = 1.0 / sig2;
        blk.y(m0) = yv;
    }
}

// Run epoch 1 (harvest) + epoch 2 (inject per `mode` + dense border carry
// inflated by q) through NetworkEstimator; return the epoch-2 shared solution,
// the carried station's locals, and the locals' joint covariance.
struct EpochTwo {
    Eigen::VectorXd x_s;      ///< shared solution (clock columns)
    Eigen::VectorXd x_loc;    ///< carried station's locals (ambiguities)
    Eigen::MatrixXd P_loc;    ///< their joint (marginal) covariance
    Eigen::MatrixXd P_s;      ///< shared covariance
};

inline EpochTwo runRecursion(const Fixture& fx, Mode mode) {
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(static_cast<std::size_t>(S));
    for (int s = 0; s < S; ++s)
        clkCol[static_cast<std::size_t>(s)] = est.registerClock(fx.sats[static_cast<std::size_t>(s)]);
    const Eigen::Index nshared = est.numShared();
    const NetworkEstimator::Datum datum;   // clocks absolutely observed — no pin needed

    // Epoch 1: solve + harvest.
    est.beginEpoch();
    Blk b0 = fx.build(0, fx.c1, 11u, clkCol, nshared);
    Blk b1 = fx.build(1, fx.c1, 22u, clkCol, nshared);
    const std::size_t s0 = est.addStation(b0.A_s, b0.A_r, b0.w, b0.y);
    const std::size_t s1 = est.addStation(b1.A_s, b1.A_r, b1.w, b1.y);
    (void)s0;
    const Eigen::VectorXd x1 = est.solve(datum);
    const Eigen::MatrixXd P1 = est.sharedCovariance();
    const Eigen::VectorXd x1r = est.reconstructStation(s1, x1);
    std::vector<Carry> carries;
    for (int s = 0; s < S; ++s) {
        const auto reg = est.stationRegression(s1, s);
        carries.push_back(Carry{s, x1r(s), reg.cond_var, reg.row});
    }
    std::vector<NetworkEstimator::SharedPrior> keys(static_cast<std::size_t>(S));
    Eigen::MatrixXd Pk(S, S);
    for (int a = 0; a < S; ++a) {
        keys[static_cast<std::size_t>(a)].kind  = SharedParamKind::Clock;
        keys[static_cast<std::size_t>(a)].sat   = fx.sats[static_cast<std::size_t>(a)];
        keys[static_cast<std::size_t>(a)].value = x1(clkCol[static_cast<std::size_t>(a)]);
        for (int bb = 0; bb < S; ++bb)
            Pk(a, bb) = P1(clkCol[static_cast<std::size_t>(a)], clkCol[static_cast<std::size_t>(bb)]);
    }
    Pk += fx.q * Eigen::MatrixXd::Identity(S, S);   // the border carry's time update

    // Epoch 2: fresh data + coupling rows (per mode) + dense border carry.
    est.beginEpoch();
    Blk c0 = fx.build(0, fx.c2, 33u, clkCol, nshared);
    Blk c1b = fx.build(1, fx.c2, 44u, clkCol, nshared);
    appendCplRows(c1b, carries, x1, P1.diagonal(), fx.q, mode);
    est.addStation(c0.A_s, c0.A_r, c0.w, c0.y);
    const std::size_t t1 = est.addStation(c1b.A_s, c1b.A_r, c1b.w, c1b.y);
    EXPECT_EQ(est.addSharedPriorDense(keys, Pk), static_cast<std::size_t>(S));
    EpochTwo out;
    const Eigen::VectorXd xs_full = est.solve(datum);
    out.x_loc = est.reconstructStation(t1, xs_full);
    std::vector<std::pair<std::size_t, Eigen::Index>> pairs;
    for (int s = 0; s < S; ++s) pairs.emplace_back(t1, static_cast<Eigen::Index>(s));
    out.P_loc = est.jointConditionalCovariance(pairs, datum);
    // Re-index the shared solution/covariance to satellite order (clkCol[s] need
    // not equal s in general).
    const Eigen::MatrixXd Ps_full = est.sharedCovariance();
    out.x_s.resize(S);
    out.P_s.resize(S, S);
    for (int a = 0; a < S; ++a) {
        out.x_s(a) = xs_full(clkCol[static_cast<std::size_t>(a)]);
        for (int bb = 0; bb < S; ++bb)
            out.P_s(a, bb) = Ps_full(clkCol[static_cast<std::size_t>(a)],
                                     clkCol[static_cast<std::size_t>(bb)]);
    }
    return out;
}

// Hand-built exact batch per satellite: unknowns z = [c₁, c₂, M₀, M₁], rows =
// both epochs' code/phase + BOTH epochs' pivot pins + the link c₂−c₁ ~ N(0,q).
// Noise realizations MUST match runRecursion's builds (same seeds), so the two
// see identical data.
struct Batch { double c2, m1; double var_c2, var_m1; };

inline Batch runBatch(const Fixture& fx, int s) {
    // Reproduce the per-row noise stream: build() draws 2 gaussians per sat in
    // sat order from one generator per (station, epoch) — replay them all.
    auto noise = [&](unsigned seed) {
        std::mt19937 g(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        std::vector<std::pair<double,double>> v(static_cast<std::size_t>(S));
        for (int k = 0; k < S; ++k) { const double a = nd(g); const double b = nd(g); v[static_cast<std::size_t>(k)] = {a, b}; }
        return v;
    };
    const auto n0e1 = noise(11u), n1e1 = noise(22u), n0e2 = noise(33u), n1e2 = noise(44u);
    const auto su = static_cast<std::size_t>(s);

    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(4, 4);
    Eigen::VectorXd u = Eigen::VectorXd::Zero(4);
    auto addRow = [&](const Eigen::Vector4d& a, double w, double y) {
        N += w * a * a.transpose();
        u += w * a * y;
    };
    // z = [c1, c2, M0, M1]
    for (int ep = 0; ep < 2; ++ep) {
        const double c = (ep == 0) ? fx.c1[su] : fx.c2[su];
        const int ci = ep;   // c1 or c2 column
        const auto& np = (ep == 0) ? n0e1 : n0e2;   // pivot noise
        const auto& nc = (ep == 0) ? n1e1 : n1e2;   // carried-station noise
        // pivot: code, phase (M0), pin
        addRow(Eigen::Vector4d((ci == 0 ? -1 : 0), (ci == 1 ? -1 : 0), 0, 0), W_CD,
               -c + 0.5 * np[su].first);
        addRow(Eigen::Vector4d((ci == 0 ? -1 : 0), (ci == 1 ? -1 : 0), 1, 0), W_PH,
               -c + fx.M[0][s] + 0.1 * np[su].second);
        addRow(Eigen::Vector4d(0, 0, 1, 0), W_PIN, 0.0);
        // carried station: code, phase (M1)
        addRow(Eigen::Vector4d((ci == 0 ? -1 : 0), (ci == 1 ? -1 : 0), 0, 0), W_CD,
               -c + 0.5 * nc[su].first);
        addRow(Eigen::Vector4d((ci == 0 ? -1 : 0), (ci == 1 ? -1 : 0), 0, 1), W_PH,
               -c + fx.M[1][s] + 0.1 * nc[su].second);
    }
    addRow(Eigen::Vector4d(-1, 1, 0, 0), 1.0 / fx.q, 0.0);   // link c2 − c1 ~ N(0, q)
    const Eigen::MatrixXd Ninv = N.inverse();
    const Eigen::VectorXd z = Ninv * u;
    return Batch{z(1), z(3), Ninv(1, 1), Ninv(3, 3)};
}

}  // namespace card1d

// Card 1d gate: with the LARGE-q moving clock border, the κ-fixed recursion
// equals the hand-built batch (values AND formal variances) while the raw
// Card-1 recursion fails grossly; a moderate q exercises κ away from both
// limits.  See docs/SPEC_card1_coupling_carry.md.
TEST(NetworkEstimatorTest, ConditionalCarryTimeUpdateRecursiveEqualsBatch) {
    using namespace card1d;
    for (const double q : {1e4, 1.0}) {
        const Fixture fx(q, std::sqrt(q));
        const EpochTwo fixed = runRecursion(fx, Mode::Fixed);
        const EpochTwo raw   = runRecursion(fx, Mode::Raw);
        double fixed_err = 0.0, raw_err = 0.0;
        for (int s = 0; s < S; ++s) {
            const Batch bt = runBatch(fx, s);
            fixed_err = std::max(fixed_err, std::abs(fixed.x_s(s) - bt.c2));
            fixed_err = std::max(fixed_err, std::abs(fixed.x_loc(s) - bt.m1));
            raw_err = std::max(raw_err, std::abs(raw.x_s(s) - bt.c2));
            raw_err = std::max(raw_err, std::abs(raw.x_loc(s) - bt.m1));
            EXPECT_NEAR(fixed.P_s(s, s), bt.var_c2, 1e-6 * bt.var_c2)
                << "q=" << q << " sat " << s << ": fixed recursion clock variance != batch";
            EXPECT_NEAR(fixed.P_loc(s, s), bt.var_m1, 1e-6 * bt.var_m1)
                << "q=" << q << " sat " << s << ": fixed recursion ambiguity variance != batch";
        }
        EXPECT_LT(fixed_err, 1e-8)
            << "q=" << q << ": κ-fixed recursion must equal the batch solve";
        if (q == 1e4) {
            EXPECT_GT(raw_err, 0.05)
                << "raw Card-1 rows should fail against the large-q moving border "
                   "(the Card-1c defect) — the fixture lost its discrimination";
            EXPECT_GT(raw_err, 1e3 * std::max(fixed_err, 1e-12))
                << "the fixed recursion should be orders of magnitude closer to batch";
        }
    }
}

// Card 1d limits: q = 0 must reproduce the raw (unfixed) rows EXACTLY (κ = 1,
// zero inflation — guards the OFF-adjacent behavior), and q → ∞ must reproduce
// the departed-column marginalization (κ = 0, σ² += M²·P_prev) — the fix's two
// analytic limits.
TEST(NetworkEstimatorTest, ConditionalCarryTimeUpdateLimits) {
    using namespace card1d;
    {   // q = 0: κ-fixed construction == raw construction, bit-for-bit behavior
        const Fixture fx(0.0, 1.0);
        const EpochTwo fixed = runRecursion(fx, Mode::Fixed);
        const EpochTwo raw   = runRecursion(fx, Mode::Raw);
        EXPECT_LT((fixed.x_s - raw.x_s).cwiseAbs().maxCoeff(), 1e-13)
            << "q=0: the κ formula must degenerate to the unfixed rows exactly";
        EXPECT_LT((fixed.x_loc - raw.x_loc).cwiseAbs().maxCoeff(), 1e-13);
        EXPECT_LT((fixed.P_loc - raw.P_loc).cwiseAbs().maxCoeff(), 1e-13);
    }
    {   // q → ∞ (1e30): κ-fixed == full departed-column marginalization
        const Fixture fx(1e30, 1.0);   // truth drift kept moderate — equivalence is data-identical
        const EpochTwo fixed = runRecursion(fx, Mode::Fixed);
        const EpochTwo dep   = runRecursion(fx, Mode::Departed);
        EXPECT_LT((fixed.x_s - dep.x_s).cwiseAbs().maxCoeff(), 1e-9)
            << "q→∞: the κ formula must reproduce the departed-column branch";
        EXPECT_LT((fixed.x_loc - dep.x_loc).cwiseAbs().maxCoeff(), 1e-9);
        EXPECT_LT((fixed.P_loc - dep.P_loc).cwiseAbs().maxCoeff(), 1e-9);
    }
}

// Card 1b (network.carry_coupling_joint_whiten): when >=2 of a station's carried
// ambiguities regress on the SAME shared column that has DEPARTED the layout, that
// column must be marginalized JOINTLY — the coupling rows' noise covariance
// R = diag(var_eff) + M_w·diag(P_w)·M_wᵀ — not per-row into a scalar.  This test
// isolates the mathematical claim on a minimal system (two locals r0,r1 + one
// departed shared W): the joint covariance equals the exact batch marginal of W, so
// the recursive posterior matches the batch; the Card-1 per-row SCALAR variance is
// exactly the DIAGONAL of that joint covariance (it drops the between-row correlation
// the shared departed column induces) and does NOT match.  Self-contained linear
// algebra — validates the mechanism the driver's whitenCouplingRows() implements.
TEST(NetworkEstimatorTest, JointWhitenCouplingMarginalizationEqualsBatch) {
    using Eigen::MatrixXd;
    using Eigen::Vector2d;

    // Epoch-1 information over θ = [r0, r1, W] accumulated from a small obs set: a
    // "phase" obs couples each local r_i to the shared W; a direct obs on each local
    // and a prior on W keep the 3×3 information SPD.
    const double M0 = 0.7, M1 = -1.3;
    MatrixXd Lam1 = MatrixXd::Zero(3, 3);
    auto addObs = [&](double a_r0, double a_r1, double a_W, double var) {
        Eigen::Vector3d a(a_r0, a_r1, a_W);
        Lam1 += (a * a.transpose()) / var;
    };
    addObs(1.0, 0.0, M0, 0.01);   // phase: r0 + M0·W
    addObs(0.0, 1.0, M1, 0.01);   // phase: r1 + M1·W
    addObs(1.0, 0.0, 0.0, 1.0);   // direct r0
    addObs(0.0, 1.0, 0.0, 1.0);   // direct r1
    addObs(0.0, 0.0, 1.0, 100.0); // W prior (a-priori var 100)

    const MatrixXd Sig1 = Lam1.inverse();            // epoch-1 covariance
    const MatrixXd Srr  = Sig1.topLeftCorner(2, 2);  // marginal (r0,r1) cov (the batch's epoch-1 marginal)
    const Vector2d SrW  = Sig1.block<2, 1>(0, 2);
    const double   SWW  = Sig1(2, 2);                 // W marginal variance = P_w
    const Vector2d Mr   = SrW / SWW;                  // regression of (r0,r1) on W
    const MatrixXd Scond = Srr - SWW * (Mr * Mr.transpose());  // conditional cov (var_eff diagonal)

    // JOINT whitening covariance R = diag(var_eff) + M_w·P_w·M_wᵀ, with M_w=[Mr0;Mr1],
    // var_eff_i = Scond(i,i).  By the marginalization identity it equals the marginal Srr.
    MatrixXd Rjoint = SWW * (Mr * Mr.transpose());
    for (int i = 0; i < 2; ++i) Rjoint(i, i) += Scond(i, i);
    EXPECT_LT((Rjoint - Srr).cwiseAbs().maxCoeff(), 1e-12)
        << "joint departed-column marginalization must equal the exact batch marginal cov";

    // Card-1 SCALAR path: σ²_i = var_eff_i + M_i²·P_w = Srr(i,i) — a DIAGONAL R that
    // discards the off-diagonal M0·M1·P_w the two rows share through W.
    MatrixXd Rscalar = MatrixXd::Zero(2, 2);
    for (int i = 0; i < 2; ++i) Rscalar(i, i) = Scond(i, i) + SWW * Mr(i) * Mr(i);

    // Epoch-2 information on the LOCALS only (W has departed — not touched).  The
    // recursive posterior = epoch-1 marginal r-info (R⁻¹) + epoch-2 info; the batch =
    // full (r0,r1,W) info + epoch-2 r-info, marginalized over W.  They agree iff the
    // marginalization is exact (joint), and epoch-2's r-only update commutes with it.
    MatrixXd I2 = MatrixXd::Zero(2, 2);
    I2(0, 0) = 1.0 / 0.02;
    I2(1, 1) = 1.0 / 0.03;

    MatrixXd Lam_full = Lam1;
    Lam_full.topLeftCorner(2, 2) += I2;
    const MatrixXd batch_rr = Lam_full.inverse().topLeftCorner(2, 2);

    const MatrixXd cov_joint  = (Rjoint.inverse()  + I2).inverse();
    const MatrixXd cov_scalar = (Rscalar.inverse() + I2).inverse();

    EXPECT_LT((cov_joint - batch_rr).cwiseAbs().maxCoeff(), 1e-9)
        << "joint whitening of the shared departed column reproduces the batch (r0,r1) "
           "posterior covariance — the σ the scalar path flattens";
    EXPECT_GT((cov_scalar - batch_rr).cwiseAbs().maxCoeff(), 1e-6)
        << "the per-row scalar (diagonal) marginalization must NOT match batch — dropping "
           "the between-row correlation is exactly the Card-1 inert-carry flaw Card 1b fixes";
}

// Input validation + numerical rejection: bad dims throw invalid_argument, an
// un-begun epoch throws logic_error, an indefinite or negative-variance block
// throws runtime_error (after the ridge retry) — and a rejected fold leaves the
// epoch solvable (the throw happens before any kernel mutation).
TEST(NetworkEstimatorTest, DensePriorInputValidation) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/3, /*S=*/4);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    std::vector<NetworkEstimator::SharedPrior> one(1);
    one[0].kind = SharedParamKind::Clock; one[0].sat = t.sats[1]; one[0].value = 1.0;

    // Dimension mismatch → invalid_argument (checked before the epoch guard).
    EXPECT_THROW(est.addSharedPriorDense(one, Eigen::MatrixXd::Identity(2, 2)),
                 std::invalid_argument);
    // No open epoch → logic_error (the NEW overload is guarded; the legacy
    // diagonal addSharedPrior deliberately is not).
    EXPECT_THROW(est.addSharedPriorDense(one, Eigen::MatrixXd::Identity(1, 1)),
                 std::logic_error);

    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }

    std::vector<NetworkEstimator::SharedPrior> two(2);
    two[0].kind = SharedParamKind::Clock; two[0].sat = t.sats[1]; two[0].value = 1.0;
    two[1].kind = SharedParamKind::Clock; two[1].sat = t.sats[2]; two[1].value = 2.0;
    Eigen::MatrixXd indef(2, 2);
    indef << 1.0, 2.0,
             2.0, 1.0;   // eigenvalues {3, −1}: |ρ| > 1, unfixable by the ridge
    EXPECT_THROW(est.addSharedPriorDense(two, indef), std::runtime_error);
    Eigen::MatrixXd negvar(2, 2);
    negvar << -1.0, 0.0,
              0.0, 1.0;
    EXPECT_THROW(est.addSharedPriorDense(two, negvar), std::runtime_error);

    // A rejected fold must not have touched the kernel: the epoch still solves.
    const Eigen::VectorXd x = est.solve(datum);
    EXPECT_TRUE(x.allFinite());
}

// The conditioning guards (review hardening): a PD-but-near-singular carried
// correlation (relative pivot < 1e-6 even after the ridge retry) and an absolute
// folded-information overflow (Λ_aa > 1e13 1/m²) are both REJECTED as
// runtime_error — the driver then degrades to the legacy diagonal fold for the
// epoch instead of injecting amplified information that would trip the
// constrained solve's own relative-pivot guard downstream (where a solve-throw
// skips the epoch BEFORE the stash and the identical block re-folds forever).
TEST(NetworkEstimatorTest, DensePriorRejectsNearSingularAndOverStiffBlocks) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/3, /*S=*/4);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }

    std::vector<NetworkEstimator::SharedPrior> two(2);
    two[0].kind = SharedParamKind::Clock; two[0].sat = t.sats[1]; two[0].value = 1.0;
    two[1].kind = SharedParamKind::Clock; two[1].sat = t.sats[2]; two[1].value = 2.0;

    // (a) PD in exact arithmetic but near-singular: ρ = 1 − 5e-9 ⇒ the
    // correlation factor's relative pivot ≈ 1e-8, under the 1e-6 acceptance even
    // after the +1e-9 ridge.  Folding it would amplify information ~1e8-fold.
    Eigen::MatrixXd near_sing(2, 2);
    const double rho = 1.0 - 5e-9;
    near_sing << 1.0, rho,
                 rho, 1.0;
    EXPECT_THROW(est.addSharedPriorDense(two, near_sing), std::runtime_error);

    // (b) Absolute folded-information budget: healthy correlation (identity) but
    // an extreme 1e-14 m² variance ⇒ Λ_aa = 1e14 > the 1e13 1/m² budget.
    Eigen::MatrixXd overstiff = Eigen::MatrixXd::Zero(2, 2);
    overstiff(0, 0) = 1e-14;
    overstiff(1, 1) = 1e-14;
    EXPECT_THROW(est.addSharedPriorDense(two, overstiff), std::runtime_error);

    // Both rejections precede any kernel mutation — the epoch still solves.
    const Eigen::VectorXd x = est.solve(datum);
    EXPECT_TRUE(x.allFinite());
}

// Empty key set and all-absent key sets are EXACT no-ops (no kernel mutation),
// mirroring the legacy absent-key skip.
TEST(NetworkEstimatorTest, DensePriorEmptyAndAbsentKeysAreExactNoOps) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/3, /*S=*/4);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s] = est.registerPhaseBias(t.sats[s], signal);
    }
    const NetworkEstimator::Datum datum = {{est.clockColumn(t.sats[0]), 0.0}};

    auto solveEpoch = [&](int mode) {   // 0 = no call, 1 = empty keys, 2 = absent key
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
        if (mode == 1) {
            EXPECT_EQ(est.addSharedPriorDense({}, Eigen::MatrixXd(0, 0)), 0u);
        } else if (mode == 2) {
            std::vector<NetworkEstimator::SharedPrior> ghost(1);
            ghost[0].kind = SharedParamKind::Clock; ghost[0].sat = "G99";
            ghost[0].value = 42.0;
            Eigen::VectorXd lam;
            EXPECT_EQ(est.addSharedPriorDense(ghost, Eigen::MatrixXd::Identity(1, 1), &lam), 0u);
            EXPECT_EQ(lam(0), 0.0);
        }
        return est.solve(datum);
    };
    const Eigen::VectorXd x_plain  = solveEpoch(0);
    const Eigen::VectorXd x_empty  = solveEpoch(1);
    const Eigen::VectorXd x_absent = solveEpoch(2);
    EXPECT_EQ((x_empty - x_plain).cwiseAbs().maxCoeff(), 0.0);
    EXPECT_EQ((x_absent - x_plain).cwiseAbs().maxCoeff(), 0.0);
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

// Iono-2 dual-layer: the second shell registers under its own "SH2" keys — the
// same (n,m,trig) coexists on both layers as DISTINCT columns, the per-layer
// prior routing lands on the right one, the layer-0 API keeps its legacy
// defaulted-argument behavior, and labels disambiguate the shells.
TEST(NetworkEstimatorTest, VtecShLayer2ColumnsRegisterDistinctWithPerLayerPriors) {
    NetworkEstimator est;
    const Eigen::Index cG1 = est.registerClock("G01");
    const Eigen::Index a00_l1 = est.registerVtecCoeff(0, 0, 'C');       // layer 0 (legacy)
    const Eigen::Index a00_l2 = est.registerVtecCoeff(0, 0, 'C', 1);    // layer 1
    const Eigen::Index a10_l2 = est.registerVtecCoeff(1, 0, 'C', 1);
    EXPECT_NE(a00_l1, a00_l2);                                   // distinct columns
    EXPECT_EQ(est.registerVtecCoeff(0, 0, 'C', 1), a00_l2);      // idempotent per layer
    EXPECT_EQ(est.vtecCoeffColumn(0, 0, 'C'), a00_l1);           // default = layer 0
    EXPECT_EQ(est.vtecCoeffColumn(0, 0, 'C', 1), a00_l2);
    EXPECT_EQ(est.numShared(), 4);
    EXPECT_EQ(est.sharedColumnLabel(a00_l1), "SH:C:0:0");
    EXPECT_EQ(est.sharedColumnLabel(a00_l2), "SH2:C:0:0");

    est.beginEpoch();
    Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(1, 4);
    A_s(0, cG1) = -1.0;
    Eigen::MatrixXd A_r(1, 1); A_r << 1.0;
    Eigen::VectorXd w(1); w << 1.0;
    Eigen::VectorXd y(1); y << 2.0;
    est.addStation(A_s, A_r, w, y);

    // Per-layer a00 anchors: layer 0 → 12 TECU, layer 1 → 3 TECU.  sh_layer in
    // the SharedPrior identity must route each to ITS shell's column.
    std::vector<NetworkEstimator::SharedPrior> pr;
    auto mk = [](int n, int m, char t, int layer, double v) {
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::VtecSh; p.sh_n = n; p.sh_m = m; p.sh_trig = t;
        p.sh_layer = layer; p.value = v; p.info = 1.0e4;
        return p;
    };
    pr.push_back(mk(0, 0, 'C', 0, 12.0));
    pr.push_back(mk(0, 0, 'C', 1, 3.0));
    pr.push_back(mk(1, 0, 'C', 1, 0.0));
    est.addSharedPrior(pr);

    NetworkEstimator::Datum datum{ NetworkEstimator::Pin{cG1, 0.0} };
    const Eigen::VectorXd x = est.solve(datum);
    EXPECT_NEAR(x(a00_l1), 12.0, 1e-3);
    EXPECT_NEAR(x(a00_l2), 3.0, 1e-3);
    EXPECT_NEAR(x(a10_l2), 0.0, 1e-3);
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

// W6 IF-pin reference-pair selection must be a DETERMINISTIC function of the
// satellite's registered signal SET — independent of the order the signals are
// passed in.  The emitted (δtˢ, dˢ) split is anchored to this pair, so an
// order-dependent same-band swap (C1C ↔ C1W) would silently re-datum the clock
// by the satellite's inter-channel bias — the bug selectIfPinPair fixes.
TEST(NetworkSbasisTest, SelectIfPinPairIsDeterministicInSignalSet) {
    using Pair = std::pair<std::string, std::string>;
    // GPS carrier frequencies by RINEX band digit (L1 / L2 / L5); '9' (below) is
    // an unavailable-frequency stand-in (freqOf -> 0).
    auto gpsFreq = [](const std::string& s) -> double {
        if (s.size() < 2) return 0.0;
        switch (s[1]) {
            case '1': return 1575.42e6;
            case '2': return 1227.60e6;
            case '5': return 1176.45e6;
            default:  return 0.0;
        }
    };

    // Same-band channels tie-break on the FULL signal name (C1C < C1W), and the
    // result is invariant to input order — three permutations, one answer.
    const Pair expected{ "C1C", "C2W" };
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C1W", "C2W" }, gpsFreq), expected);
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C2W", "C1W", "C1C" }, gpsFreq), expected);
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1W", "C2W", "C1C" }, gpsFreq), expected);

    // `second` skips a same-FREQUENCY co-channel (C1L is L1 like C1C) and takes the
    // first genuinely distinct frequency.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1L", "C1C", "C2W" }, gpsFreq),
              (Pair{ "C1C", "C2W" }));

    // Two LOWEST distinct frequencies: L2 wins over L5 (first = L1, second = the
    // first distinct frequency = L2), regardless of where L5 sorts.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C5Q", "C2W", "C1C" }, gpsFreq),
              (Pair{ "C1C", "C2W" }));

    // Single distinct frequency (both L1) => no second signal (degenerate pin).
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1W", "C1C" }, gpsFreq),
              (Pair{ "C1C", std::string{} }));

    // A signal whose frequency is unavailable (freqOf == 0) is never chosen as the
    // distinct second; with no other real band there is no second.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair({ "C1C", "C9X" }, gpsFreq),
              (Pair{ "C1C", std::string{} }));

    // Empty set => empty pair.
    EXPECT_EQ(NetworkSbasis::selectIfPinPair(std::vector<std::string>{}, gpsFreq),
              (Pair{ std::string{}, std::string{} }));
}

// Card 2 (network.code_datum_time_linked): the pure time-linked sum-to-zero target.
// Veterans (present satellites WITH a previous solved value) span the row and are
// referenced to themselves (RHS Σ b̂_prev), so a membership change never moves the
// target.  A channel with NO veteran falls back to the legacy all-present Σ = 0 seed
// and flags reseed so the driver disc-bumps the re-reference.
TEST(NetworkSbasisTest, CodeS2zTimeLinkVeteranReferenceAndReseed) {
    const std::vector<std::string> present = { "G01", "G02", "G03" };

    // All veterans: row spans all three, RHS = Σ prev, no reseed.
    {
        const std::map<std::string, double> prev =
            { { "G01", 0.3 }, { "G02", -0.1 }, { "G03", 0.5 } };
        const auto tl = NetworkSbasis::codeS2zTimeLink(present, prev);
        EXPECT_FALSE(tl.reseed);
        EXPECT_EQ(tl.jac_sats, present);
        EXPECT_NEAR(tl.rhs, 0.3 - 0.1 + 0.5, 1e-12);
    }
    // Mixed (G02 fresh): the row spans the VETERANS {G01,G03} only — the target is the
    // veteran sum, so the new satellite takes its level from the network + this row.
    {
        const std::map<std::string, double> prev = { { "G01", 0.3 }, { "G03", 0.5 } };
        const auto tl = NetworkSbasis::codeS2zTimeLink(present, prev);
        EXPECT_FALSE(tl.reseed);
        EXPECT_EQ(tl.jac_sats, (std::vector<std::string>{ "G01", "G03" }));
        EXPECT_NEAR(tl.rhs, 0.8, 1e-12);
    }
    // No veteran: fall back to the all-present Σ = 0 seed, reseed flagged.
    {
        const auto tl = NetworkSbasis::codeS2zTimeLink(present, {});
        EXPECT_TRUE(tl.reseed);
        EXPECT_EQ(tl.jac_sats, present);
        EXPECT_NEAR(tl.rhs, 0.0, 1e-12);
    }
}

// Card 2: set-change CONTINUITY of the sum-to-zero code-bias datum, solved through the
// real estimator.  A channel of satellite code biases is observable only up to a common
// LEVEL — each observation is (d_r + b_s), the receiver code bias d_r absorbing the
// level, so the sum-to-zero row is the gauge that fixes it.  When a satellite LEAVES
// between epochs, the legacy zero target (Σ b = 0 over the CURRENT set) STEPS every
// surviving bias by the departed bias / (m−1) (a common-mode shift); the time-linked
// veteran target (Σ_vet b = Σ b̂_prev) reproduces the previous epoch EXACTLY.  This is
// the falsifiable core of the card — the same layout either way (rank unchanged).
TEST(NetworkEstimatorTest, CodeDatumTimeLinkedSetChangeContinuity) {
    const std::string sig = "C1W";
    const std::vector<std::string> sats = { "G01", "G02", "G03", "G04" };
    const std::vector<double>      O    = { 12.3, 12.9, 11.7, 13.4 };  // observed d_r + b_s

    // Solve one epoch over `present` (indices into sats) with a sum-to-zero code-bias
    // gauge at RHS `z_target`; return the solved b_s keyed by satellite.
    auto solveEpoch = [&](const std::vector<int>& present, double z_target) {
        NetworkEstimator est;
        std::map<std::string, Eigen::Index> cb;
        for (int i : present) cb[sats[static_cast<std::size_t>(i)]] =
            est.registerCodeBias(sats[static_cast<std::size_t>(i)], sig);
        const Eigen::Index nshared = est.numShared();
        est.beginEpoch();
        // One station: m code observations P_s = d_r + b_s (local d_r absorbs the level).
        const Eigen::Index m = static_cast<Eigen::Index>(present.size());
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, nshared);
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, 1);        // receiver code bias d_r
        Eigen::VectorXd w   = Eigen::VectorXd::Ones(m);
        Eigen::VectorXd y   = Eigen::VectorXd::Zero(m);
        for (Eigen::Index k = 0; k < m; ++k) {
            const int i = present[static_cast<std::size_t>(k)];
            A_s(k, cb[sats[static_cast<std::size_t>(i)]]) = 1.0;
            A_r(k, 0)                                     = 1.0;
            y(k)                                          = O[static_cast<std::size_t>(i)];
        }
        est.addStation(A_s, A_r, w, y);
        // Sum-to-zero gauge over the present code biases at RHS z_target (weight 1e3 —
        // the driver's kCodeBiasGaugeWeight).
        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, nshared);
        for (int i : present) H(0, cb[sats[static_cast<std::size_t>(i)]]) = 1.0;
        Eigen::VectorXd z(1); z(0) = z_target;
        est.addSharedConstraintRows(H, z, 1.0e3);
        const Eigen::VectorXd x = est.solve(NetworkEstimator::Datum{});   // soft s2z pins the level
        std::map<std::string, double> out;
        for (int i : present) out[sats[static_cast<std::size_t>(i)]] = x(cb[sats[static_cast<std::size_t>(i)]]);
        return out;
    };

    // Epoch 1: all four satellites, legacy Σ = 0.
    const auto b1 = solveEpoch({ 0, 1, 2, 3 }, 0.0);
    const std::vector<int> surv = { 0, 1, 2 };   // Epoch 2: G04 leaves.

    // OLD (toggle off): Σ = 0 over the survivors → every survivor steps by the SAME
    // common-mode shift (the mean change).
    const auto b2_old = solveEpoch(surv, 0.0);
    // NEW (toggle on): Σ_vet = Σ b̂_prev over the survivors → survivors continuous.
    double z_vet = 0.0;
    for (int i : surv) z_vet += b1.at(sats[static_cast<std::size_t>(i)]);
    const auto b2_new = solveEpoch(surv, z_vet);

    double step_min = 1e300, step_max = -1e300;
    for (int i : surv) {
        const std::string& s = sats[static_cast<std::size_t>(i)];
        const double step = b2_old.at(s) - b1.at(s);
        step_min = std::min(step_min, step);
        step_max = std::max(step_max, step);
        // Time-linked: reproduces epoch 1 exactly.
        EXPECT_NEAR(b2_new.at(s), b1.at(s), 1e-6)
            << "time-linked datum must hold " << s << " continuous across the set change";
    }
    // Legacy: a visible, uniform (common-mode) step — the very wander the card cures.
    EXPECT_GT(std::abs(step_min), 1e-3) << "legacy zero-target datum should step the survivors";
    EXPECT_NEAR(step_min, step_max, 1e-9) << "the legacy step is a pure common-mode shift";
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

// SUM-TO-ZERO clock datum (network.clock_datum_sum_to_zero): buildSharedDatum emits
// one all-ones gauge row per constellation over its OWN clock columns and NO pivot /
// hard pin — the anchor-free analogue of the code-bias per-(const,signal) sum-to-zero.
TEST(NetworkSbasisTest, SumToZeroDatumBuildsGaugeRowsNoPivot) {
    NetworkEstimator est;
    est.registerClock("G01"); est.registerClock("G02");
    est.registerClock("E11"); est.registerClock("E12");
    est.registerPhaseBias("G01", "L1");   // a non-clock column that must stay 0 in the rows

    NetworkSbasis sb;
    SbasisDeclaration decl;
    decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
    decl.clock_pivot_sat["G"] = "G02";    // a stray config pin must be IGNORED (no pivot exists)

    const auto plan = sb.buildSharedDatum(est, {"G01", "G02", "E11", "E12"}, decl);

    EXPECT_TRUE(plan.shared_datum.empty());          // no hard pins
    EXPECT_TRUE(plan.clock_pivot.empty());           // no pivot satellite
    EXPECT_EQ(plan.declared_dimension, 0);           // gauges folded before the gate → full rank
    EXPECT_GT(plan.clock_constraint_weight, 0.0);
    ASSERT_EQ(plan.clock_constraint_H.rows(), 2);    // GPS + Galileo
    EXPECT_EQ(plan.clock_constraint_H.cols(), est.numShared());
    ASSERT_EQ(plan.clock_constraint_z.size(), 2);
    EXPECT_TRUE(plan.clock_constraint_z.isZero());

    // Each row is all-ones over exactly its constellation's clock columns and zero
    // everywhere else (the other constellation's clocks + the phase-bias column).
    for (Eigen::Index r = 0; r < plan.clock_constraint_H.rows(); ++r) {
        const Eigen::VectorXd row = plan.clock_constraint_H.row(r);
        const bool is_gps = row(est.clockColumn("G01")) == 1.0;
        if (is_gps) {
            EXPECT_EQ(row(est.clockColumn("G02")), 1.0);
            EXPECT_EQ(row(est.clockColumn("E11")), 0.0);
            EXPECT_EQ(row(est.clockColumn("E12")), 0.0);
        } else {
            EXPECT_EQ(row(est.clockColumn("E11")), 1.0);
            EXPECT_EQ(row(est.clockColumn("E12")), 1.0);
            EXPECT_EQ(row(est.clockColumn("G02")), 0.0);
        }
        EXPECT_EQ(row(est.phaseBiasColumn("G01", "L1")), 0.0);  // non-clock column untouched
        EXPECT_EQ(row.sum(), 2.0);                              // exactly two ones per row
    }
}

// End-to-end: the SUM-TO-ZERO datum recovers the SAME estimable solution as the pin
// datum (a pure S-transform).  Both are solved on the same synthetic truth; the
// between-satellite clock DIFFERENCES and the phase biases match, the sum-to-zero
// clocks sum to zero (each == truth − constellation mean), and the full-rank gate
// (declared_dimension 0) passes.
TEST(NetworkSbasisTest, SumToZeroDatumRecoversSTransformAndGate) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/4, /*S=*/5);
    const std::string signal = "L1";

    auto buildAndSolve = [&](bool sum_to_zero, std::vector<Eigen::Index>& clkCol,
                             std::vector<Eigen::Index>& pbCol) -> Eigen::VectorXd {
        NetworkEstimator est;
        clkCol.assign(t.S, 0); pbCol.assign(t.S, 0);
        std::vector<std::string> clk_sats;
        for (int s = 0; s < t.S; ++s) {
            clkCol[s] = est.registerClock(t.sats[s]);
            pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
            clk_sats.push_back(t.sats[s]);
        }
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s, A_r; Eigen::VectorXd w, y;
            buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);  // pivot r=0 folds local amb pins
            est.addStation(A_s, A_r, w, y);
        }
        NetworkSbasis sb;
        SbasisDeclaration decl;
        if (sum_to_zero)
            decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
        const auto plan = sb.buildSharedDatum(est, clk_sats, decl);
        if (plan.clock_constraint_H.rows() > 0)   // sum-to-zero: fold the gauge rows
            est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                        plan.clock_constraint_weight);
        EXPECT_EQ(plan.declared_dimension, sum_to_zero ? 0 : 1);
        EXPECT_NO_THROW(sb.assertNullSpaceRank(est.reducedSharedNormals(),
                                               plan.declared_dimension));
        return est.solve(plan.shared_datum);      // empty datum in sum-to-zero mode
    };

    std::vector<Eigen::Index> clkP, pbP, clkZ, pbZ;
    const Eigen::VectorXd x_pin = buildAndSolve(false, clkP, pbP);
    const Eigen::VectorXd x_z   = buildAndSolve(true,  clkZ, pbZ);

    // Sum-to-zero: Σ satellite clocks == 0, and each clock == truth − constellation mean.
    double mean_c = 0.0; for (double c : t.c) mean_c += c; mean_c /= t.S;
    double sum_z = 0.0;
    for (int s = 0; s < t.S; ++s) {
        EXPECT_NEAR(x_z(clkZ[s]), t.c[s] - mean_c, 1e-6) << "sum-to-zero clock " << s;
        sum_z += x_z(clkZ[s]);
    }
    EXPECT_NEAR(sum_z, 0.0, 1e-6);

    // S-transform equivalence: between-satellite clock differences are identical to
    // the pin datum, and the phase-bias gauge (fixed by the LOCAL pivot pins) is
    // untouched by the clock-datum choice.
    for (int s = 0; s < t.S; ++s) {
        EXPECT_NEAR(x_z(clkZ[s]) - x_z(clkZ[0]),
                    x_pin(clkP[s]) - x_pin(clkP[0]), 1e-6) << "delta-clock " << s;
        EXPECT_NEAR(x_z(pbZ[s]), x_pin(pbP[s]), 1e-6) << "phase bias parity " << s;
        EXPECT_NEAR(x_z(pbZ[s]), t.b[s] + t.M[0][s], 1e-6) << "phase bias truth " << s;
    }
}

// BROADCAST-ANCHORED clock datum (network.clock_datum_broadcast_anchored,
// brdc_rts RT): buildSharedDatum emits ONE single-entry soft anchor row per
// registered clock satellite (Δδtˢ → 0 = its own broadcast a-priori) at
// information 1/σ² — the anchors ARE the gauge.  Like sum-to-zero there is no
// pivot and no hard pin (declared dimension 0, gauges folded before the rank
// gate); UNLIKE sum-to-zero the rows are per-SATELLITE, so membership changes
// cause no redistribution wander of the absolute clock-product frame.  This
// test pins the exact Plan shape the driver folds: row count, row order
// (constellation groups in map order, lexical satellite order within), the
// single-1.0 column per row, zero targets, and the 1/σ² weight.
TEST(NetworkSbasisTest, BroadcastAnchoredPlanShape) {
    NetworkEstimator est;
    est.registerClock("G01"); est.registerClock("G02");
    est.registerClock("E01");
    est.registerPhaseBias("G01", "L1");   // a non-clock column that must stay 0 in the rows

    NetworkSbasis sb;
    SbasisDeclaration decl;
    decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
    // decl.broadcast_anchor_sigma_m keeps the declared default of 3.0 m.

    const auto plan = sb.buildSharedDatum(est, {"G01", "G02", "E01"}, decl);

    EXPECT_TRUE(plan.shared_datum.empty());          // no hard pins — the soft anchors are the datum
    EXPECT_TRUE(plan.clock_pivot.empty());           // no pivot satellite → no handover seam ever
    EXPECT_EQ(plan.declared_dimension, 0);           // anchors close every clock gauge pre-gate
    EXPECT_DOUBLE_EQ(plan.clock_constraint_weight, 1.0 / 9.0);   // 1/σ² at σ = 3 m

    // One row per registered clock SATELLITE (not per constellation): 3 sats.
    ASSERT_EQ(plan.clock_constraint_H.rows(), 3);
    EXPECT_EQ(plan.clock_constraint_H.cols(), est.numShared());
    ASSERT_EQ(plan.clock_constraint_z.size(), 3);
    EXPECT_TRUE(plan.clock_constraint_z.isZero());   // target = own broadcast a-priori = 0

    // Rows are grouped by constellation (lexical constellation order: E before
    // G) with lexical satellite order within each group — E01, G01, G02.
    const std::vector<std::string> expected_order{"E01", "G01", "G02"};
    for (Eigen::Index r = 0; r < plan.clock_constraint_H.rows(); ++r) {
        const Eigen::VectorXd row = plan.clock_constraint_H.row(r);
        const std::string& sat = expected_order[static_cast<std::size_t>(r)];
        EXPECT_EQ(row(est.clockColumn(sat)), 1.0) << "row " << r << " must anchor " << sat;
        EXPECT_EQ(row.sum(), 1.0);                              // exactly ONE entry per row
        EXPECT_EQ(row(est.phaseBiasColumn("G01", "L1")), 0.0);  // non-clock column untouched
    }
}

// The broadcast-anchored datum has NO pivot, so BOTH pivot-selection inputs —
// the config pin (network.clock_datum_satellite → decl.clock_pivot_sat) and
// the RTA-1 sticky incumbent map — must be completely inert: the plan is
// IDENTICAL with and without them.  Guards against a future refactor threading
// pivot state into the anchor rows, which would silently reintroduce the
// handover seam this datum exists to eliminate.
TEST(NetworkSbasisTest, BroadcastAnchoredIgnoresPivotOverridesAndSticky) {
    NetworkEstimator est;
    est.registerClock("G01"); est.registerClock("G02");
    est.registerClock("E01");

    NetworkSbasis sb;
    const std::vector<std::string> sats{"G01", "G02", "E01"};

    SbasisDeclaration base;
    base.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
    const auto ref = sb.buildSharedDatum(est, sats, base);

    SbasisDeclaration pinned = base;
    pinned.clock_pivot_sat["G"] = "G02";   // stray config pin — meaningless here
    const std::map<std::string, std::string> incumbent{{"G", "G02"}, {"E", "E01"}};
    const auto plan = sb.buildSharedDatum(est, sats, pinned, &incumbent);

    // Field-by-field identity with the un-pinned / non-sticky baseline.
    EXPECT_TRUE(plan.shared_datum.empty());
    EXPECT_TRUE(plan.clock_pivot.empty());
    EXPECT_EQ(plan.declared_dimension, ref.declared_dimension);
    EXPECT_EQ(plan.clock_constraint_weight, ref.clock_constraint_weight);
    ASSERT_EQ(plan.clock_constraint_H.rows(), ref.clock_constraint_H.rows());
    ASSERT_EQ(plan.clock_constraint_H.cols(), ref.clock_constraint_H.cols());
    EXPECT_TRUE(plan.clock_constraint_H == ref.clock_constraint_H);   // bit-identical rows
    EXPECT_TRUE(plan.clock_constraint_z == ref.clock_constraint_z);
}

// An anchor σ of zero, negative, or non-finite makes the 1/σ² row weight (and
// hence the gauge) undefined — buildSharedDatum must throw loudly rather than
// return a datum-less plan the downstream solve would silently least-norm.
TEST(NetworkSbasisTest, BroadcastAnchoredThrowsOnInvalidSigma) {
    NetworkEstimator est;
    est.registerClock("G01");

    NetworkSbasis sb;
    SbasisDeclaration decl;
    decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;

    decl.broadcast_anchor_sigma_m = 0.0;   // 1/σ² → ∞: gauge undefined
    EXPECT_THROW(sb.buildSharedDatum(est, {"G01"}, decl), std::runtime_error);

    decl.broadcast_anchor_sigma_m = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(sb.buildSharedDatum(est, {"G01"}, decl), std::runtime_error);

    decl.broadcast_anchor_sigma_m = -3.0;  // negative σ is equally meaningless
    EXPECT_THROW(sb.buildSharedDatum(est, {"G01"}, decl), std::runtime_error);

    // The declared default (3.0 m) stays valid — the guard must not overfire.
    decl.broadcast_anchor_sigma_m = 3.0;
    EXPECT_NO_THROW(sb.buildSharedDatum(est, {"G01"}, decl));
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

// ===========================================================================
// INC-8: buildCodeBiasDatumRows — the W6 satellite code-bias (OSB) datum row
// builder relocated VERBATIM out of the network run loop.  Pure over const
// estimator queries; every carry / scratch / latch is an explicit reference,
// so the tests own them all directly.
// ===========================================================================

namespace {

// GPS carrier frequency by RINEX band digit (L1 / L2 / L5), in the
// buildCodeBiasDatumRows (sat, signal) oracle shape; unknown band -> 0
// ("frequency unavailable").
double gpsCodeFreq(const std::string& /*sat*/, const std::string& sig) {
    if (sig.size() < 2) return 0.0;
    switch (sig[1]) {
        case '1': return 1575.42e6;
        case '2': return 1227.60e6;
        case '5': return 1176.45e6;
        default:  return 0.0;
    }
}

// Owns the builder's five side channels + knob set and clears the per-epoch
// scratch outs before each call, exactly as the driver contract requires
// ("cleared by caller").  The emitEvent stub counts firings and records the
// last (tier, event, sat, detail) for the re-latch assertions.
/// RF-0c (network.sat_gf_datum) off for every case in this file: these lock the INC-8
/// IF-pin / sum-to-zero shape, which the RF-0c row sits beside without changing.
const std::map<std::string, double> kNoSatGfTargets{};

struct CbDatumRowsHarness {
    using Pair = std::pair<std::string, std::string>;

    // knobs
    bool cb_time_linked{false};
    bool clock_signal_basis_hold{false};
    bool emit_clock_disc{false};
    bool gf_datum{false};          // slant-posture GF pin rows (default OFF = legacy)
    // Card-2 carry (const in)
    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::map<std::string, Pair>                          code_bias_ifpin_pair;
    std::set<std::string>                                code_bias_sat_seen;
    bool                                                 code_bias_datum_resumed{false};
    // side channels (read/write latch + per-epoch scratch + pending discs)
    std::map<std::string, Pair> held_clock_ref;
    std::map<std::string, Pair> this_epoch_if_pin_sel;
    std::map<std::string, Pair> this_epoch_if_pin_real;   // S-contract §1.5 declaration record
    std::set<std::string>       this_epoch_gf_pin_real;   // sats that took a GF datum row
    // FAMILY-GF: the partitioned-secondary-family verdict fed to the builder, and the
    // per-satellite secondary pair it records back.  Empty set => not one extra row, which
    // is the byte-identity control every other case in this fixture runs under.
    std::set<std::string>       gf_family_constellations;
    std::map<std::string, Pair> this_epoch_family_gf_pin_real;
    std::map<std::string, Pair> this_epoch_cb_ifpin;
    std::set<std::string>       this_epoch_cb_reseed;
    std::set<std::string>       basis_relatch_disc_pending;
    // emitEvent stub
    int         events{0};
    std::string last_tier, last_event, last_sat, last_detail;

    NetworkSbasis::CodeBiasDatumRows build(
        const NetworkEstimator& est,
        const std::vector<Pair>& registered_code_keys) {
        this_epoch_if_pin_sel.clear();
        this_epoch_if_pin_real.clear();
        this_epoch_gf_pin_real.clear();
        this_epoch_family_gf_pin_real.clear();
        this_epoch_cb_ifpin.clear();
        this_epoch_cb_reseed.clear();
        return NetworkSbasis::buildCodeBiasDatumRows(
            est, registered_code_keys, gpsCodeFreq, cb_time_linked,
            clock_signal_basis_hold, emit_clock_disc, gf_datum,
            NetworkSatGfDatum::NoneMode, kNoSatGfTargets,
            gf_family_constellations, code_bias_prev,
            code_bias_ifpin_pair, code_bias_sat_seen, code_bias_datum_resumed,
            held_clock_ref, this_epoch_if_pin_sel, this_epoch_if_pin_real,
            this_epoch_gf_pin_real, this_epoch_family_gf_pin_real,
            this_epoch_cb_ifpin,
            this_epoch_cb_reseed, basis_relatch_disc_pending,
            [this](const char* tier, const char* event, const std::string& sat,
                   const std::string& /*station*/, std::optional<double> /*stat*/,
                   std::optional<double> /*thr*/, const char* /*action*/,
                   const std::string& detail) {
                ++events;
                last_tier   = tier;
                last_event  = event;
                last_sat    = sat;
                last_detail = detail;
            });
    }
};

} // namespace

// Legacy two-band shape (all knobs off, empty carries): 2 sats x {C1C, C2W}
// yields 2 IF pins + 2 sum-to-zero rows in the CONTRACTUAL order — IF pins in
// census FIRST-SEEN order (not lexical, not column order), then s2z rows in
// sorted "<constellation>|<signal>" channel-key order — with the
// ionosphere-free (clock-preserving) weights and all-zero targets.
TEST(NetworkSbasisTest, BuildCodeBiasDatumRowsLegacyTwoBand) {
    NetworkEstimator est;
    est.registerCodeBias("G01", "C1C");
    est.registerCodeBias("G01", "C2W");
    est.registerCodeBias("G02", "C1C");
    est.registerCodeBias("G02", "C2W");

    // Census order deliberately G02-first (differs from both registration and
    // lexical order) so a row/ordering mixup cannot pass.
    const std::vector<CbDatumRowsHarness::Pair> keys{
        {"G02", "C1C"}, {"G02", "C2W"}, {"G01", "C1C"}, {"G01", "C2W"}};

    CbDatumRowsHarness h;
    const auto out = h.build(est, keys);

    ASSERT_EQ(out.H.rows(), 4);              // 2 IF pins + 2 s2z channels
    EXPECT_EQ(out.H.cols(), est.numShared());
    ASSERT_EQ(out.z.size(), 4);
    EXPECT_TRUE(out.z.isZero());             // legacy: all-zero targets
    EXPECT_EQ(out.n_if_pin_sats, 2);
    EXPECT_EQ(out.n_s2z_channels, 2);

    const double f1 = gpsCodeFreq("G", "C1C"), f2 = gpsCodeFreq("G", "C2W");
    const double den = f1 * f1 - f2 * f2;
    const double a1 = f1 * f1 / den;         // ionosphere-free (clock-preserving)
    const double a2 = -f2 * f2 / den;        //   weights: a1 + a2 = 1

    // Rows 0/1: IF pins, census first-seen order (G02 then G01), exactly two
    // entries each (the |.|-sum pins "zero everywhere else").
    const std::vector<std::string> census{"G02", "G01"};
    for (int r = 0; r < 2; ++r) {
        const std::string& sat = census[static_cast<std::size_t>(r)];
        EXPECT_DOUBLE_EQ(out.H(r, est.codeBiasColumn(sat, "C1C")), a1) << "IF row " << r;
        EXPECT_DOUBLE_EQ(out.H(r, est.codeBiasColumn(sat, "C2W")), a2) << "IF row " << r;
        EXPECT_NEAR(out.H.row(r).cwiseAbs().sum(), std::abs(a1) + std::abs(a2), 1e-12);
    }
    // Rows 2/3: per-(constellation,signal) sum-to-zero in sorted channel-key
    // order ("G|C1C" then "G|C2W"), all-ones over the carrying satellites.
    EXPECT_DOUBLE_EQ(out.H(2, est.codeBiasColumn("G01", "C1C")), 1.0);
    EXPECT_DOUBLE_EQ(out.H(2, est.codeBiasColumn("G02", "C1C")), 1.0);
    EXPECT_DOUBLE_EQ(out.H.row(2).cwiseAbs().sum(), 2.0);
    EXPECT_DOUBLE_EQ(out.H(3, est.codeBiasColumn("G01", "C2W")), 1.0);
    EXPECT_DOUBLE_EQ(out.H(3, est.codeBiasColumn("G02", "C2W")), 1.0);
    EXPECT_DOUBLE_EQ(out.H.row(3).cwiseAbs().sum(), 2.0);
}

// A satellite with a single DISTINCT frequency (two same-band channels) has no
// IF split to pin — its row degenerates to the all-ones gauge over its
// registered signals (the code-bias common mode is still fixed), zero RHS.
TEST(NetworkSbasisTest, BuildCodeBiasDatumRowsDegenerateSingleFreq) {
    NetworkEstimator est;
    est.registerCodeBias("G03", "C1C");
    est.registerCodeBias("G03", "C1W");   // same carrier (L1) as C1C

    CbDatumRowsHarness h;
    const auto out = h.build(est, {{"G03", "C1C"}, {"G03", "C1W"}});

    ASSERT_EQ(out.H.rows(), 3);              // 1 degenerate IF pin + 2 s2z channels
    ASSERT_EQ(out.z.size(), 3);
    EXPECT_TRUE(out.z.isZero());
    EXPECT_EQ(out.n_if_pin_sats, 1);
    EXPECT_EQ(out.n_s2z_channels, 2);

    // Row 0: all-ones over the satellite's signals (no IF weights).
    EXPECT_DOUBLE_EQ(out.H(0, est.codeBiasColumn("G03", "C1C")), 1.0);
    EXPECT_DOUBLE_EQ(out.H(0, est.codeBiasColumn("G03", "C1W")), 1.0);
    EXPECT_DOUBLE_EQ(out.H.row(0).cwiseAbs().sum(), 2.0);
}

// Card 2 (network.code_datum_time_linked): both row families pin the CHANGE of
// the datum.  With the stored pair intact and both signals veteran, the IF row's
// RHS is the previous solved IF combination and each s2z row's RHS the veteran
// sum; a stored-pair flip re-seeds the IF target to 0 and records the satellite
// in this_epoch_cb_reseed iff it has a prior kept epoch (code_bias_sat_seen_) —
// a genuine first appearance is exempt.
TEST(NetworkSbasisTest, BuildCodeBiasDatumRowsTimeLinked) {
    NetworkEstimator est;
    est.registerCodeBias("G01", "C1C");
    est.registerCodeBias("G01", "C2W");
    const std::vector<CbDatumRowsHarness::Pair> keys{{"G01", "C1C"}, {"G01", "C2W"}};

    const double f1 = gpsCodeFreq("G", "C1C"), f2 = gpsCodeFreq("G", "C2W");
    const double den = f1 * f1 - f2 * f2;
    const double a1 = f1 * f1 / den, a2 = -f2 * f2 / den;
    const double v1 = 0.4, v2 = -0.2;        // previous kept-epoch solved biases

    CbDatumRowsHarness h;
    h.cb_time_linked = true;
    h.code_bias_prev["G|C1C"]["G01"] = v1;   // carry keyed "<constellation>|<signal>" -> sat
    h.code_bias_prev["G|C2W"]["G01"] = v2;
    h.code_bias_ifpin_pair["G01"]    = {"C1C", "C2W"};
    h.code_bias_sat_seen.insert("G01");

    // (a) Pair unchanged + both prevs: IF RHS = a1 v1 + a2 v2; veteran s2z RHS;
    //     this epoch's pair recorded, no re-seed.
    auto out = h.build(est, keys);
    ASSERT_EQ(out.z.size(), 3);              // [IF G01, s2z G|C1C, s2z G|C2W]
    EXPECT_NEAR(out.z(0), a1 * v1 + a2 * v2, 1e-12);
    EXPECT_NEAR(out.z(1), v1, 1e-12);
    EXPECT_NEAR(out.z(2), v2, 1e-12);
    EXPECT_TRUE(h.this_epoch_cb_reseed.empty());
    ASSERT_EQ(h.this_epoch_cb_ifpin.count("G01"), 1u);
    EXPECT_EQ(h.this_epoch_cb_ifpin.at("G01"),
              (CbDatumRowsHarness::Pair{"C1C", "C2W"}));

    // (b) Stored pair flipped: the IF target re-seeds to 0 (a pair change
    //     re-references the clock<->code split even with prior values) and the
    //     SEEN satellite is recorded for the disc bump.  The s2z targets are
    //     untouched — their veterans did not change.
    h.code_bias_ifpin_pair["G01"] = {"C1C", "C5Q"};
    out = h.build(est, keys);
    EXPECT_EQ(out.z(0), 0.0);
    EXPECT_NEAR(out.z(1), v1, 1e-12);
    EXPECT_NEAR(out.z(2), v2, 1e-12);
    EXPECT_EQ(h.this_epoch_cb_reseed.count("G01"), 1u);
    EXPECT_EQ(h.this_epoch_cb_ifpin.at("G01"),
              (CbDatumRowsHarness::Pair{"C1C", "C2W"}));  // this epoch's selection, re-recorded

    // (c) Same flip but the satellite has NO prior kept epoch (not seen, not
    //     resumed): the re-seed is a first appearance — no disc-bump candidate.
    h.code_bias_sat_seen.clear();
    out = h.build(est, keys);
    EXPECT_EQ(out.z(0), 0.0);
    EXPECT_TRUE(h.this_epoch_cb_reseed.empty());
}

// network.clock_signal_basis_hold: the satellite's first-latched reference pair
// is HELD while both signals stay registered — even when a fresh selection
// would prefer a newly-risen lower band — and RE-LATCHED (one
// "CLK_BASIS"/"relatch" event; per-sat disc pending iff emit_clock_disc) only
// when a held signal leaves the >=2-station set.
TEST(NetworkSbasisTest, BuildCodeBiasDatumRowsHoldLatchRelatch) {
    using Pair = CbDatumRowsHarness::Pair;
    for (const bool emit_disc : {false, true}) {
        NetworkEstimator est;
        est.registerCodeBias("G01", "C1C");
        est.registerCodeBias("G01", "C2W");
        est.registerCodeBias("G01", "C5Q");

        CbDatumRowsHarness h;
        h.clock_signal_basis_hold = true;
        h.emit_clock_disc         = emit_disc;

        // Epoch 1 (LATCH): only L2+L5 registered -> (C2W, C5Q) latched silently.
        h.build(est, {{"G01", "C2W"}, {"G01", "C5Q"}});
        EXPECT_EQ(h.events, 0) << "emit_disc=" << emit_disc;
        ASSERT_EQ(h.held_clock_ref.count("G01"), 1u);
        EXPECT_EQ(h.held_clock_ref.at("G01"), (Pair{"C2W", "C5Q"}));

        // Epoch 2 (HOLD): C1C rises — the fresh selection would be (C1C, C2W),
        // but both held signals are still registered, so the IF row stays on
        // the held L2/L5 pair and the counting stub does NOT fire.
        auto out = h.build(est, {{"G01", "C1C"}, {"G01", "C2W"}, {"G01", "C5Q"}});
        EXPECT_EQ(h.events, 0) << "emit_disc=" << emit_disc;
        EXPECT_EQ(h.held_clock_ref.at("G01"), (Pair{"C2W", "C5Q"}));
        const double fa = gpsCodeFreq("G", "C2W"), fb = gpsCodeFreq("G", "C5Q");
        const double den = fa * fa - fb * fb;
        EXPECT_DOUBLE_EQ(out.H(0, est.codeBiasColumn("G01", "C2W")),  fa * fa / den);
        EXPECT_DOUBLE_EQ(out.H(0, est.codeBiasColumn("G01", "C5Q")), -fb * fb / den);
        EXPECT_DOUBLE_EQ(out.H(0, est.codeBiasColumn("G01", "C1C")), 0.0);

        // Epoch 3 (RE-LATCH): held C2W leaves the registered set -> adopt the
        // fresh (C1C, C5Q) pair; exactly one CLK_BASIS/relatch event; the
        // per-sat disc goes pending iff the clock-disc channel is armed.
        h.build(est, {{"G01", "C1C"}, {"G01", "C5Q"}});
        EXPECT_EQ(h.events, 1) << "emit_disc=" << emit_disc;
        EXPECT_EQ(h.last_tier, "CLK_BASIS");
        EXPECT_EQ(h.last_event, "relatch");
        EXPECT_EQ(h.last_sat, "G01");
        EXPECT_EQ(h.held_clock_ref.at("G01"), (Pair{"C1C", "C5Q"}));
        EXPECT_EQ(h.basis_relatch_disc_pending.count("G01"), emit_disc ? 1u : 0u);
    }
}

// Hold OFF + emit_clock_disc: this epoch's pin selection is recorded in the
// this_epoch_if_pin_sel scratch for the emit-site compare — but ONLY for a real
// two-frequency IF pair (a single-frequency satellite has no clock<->code split
// to re-reference, so it must stay out of the disc channel).
TEST(NetworkSbasisTest, BuildCodeBiasDatumRowsHoldOffDiscScratch) {
    NetworkEstimator est;
    est.registerCodeBias("G01", "C1C");
    est.registerCodeBias("G01", "C2W");
    est.registerCodeBias("G02", "C1C");   // single-frequency satellite

    CbDatumRowsHarness h;
    h.emit_clock_disc = true;             // hold stays OFF
    const auto out = h.build(
        est, {{"G01", "C1C"}, {"G01", "C2W"}, {"G02", "C1C"}});

    ASSERT_EQ(h.this_epoch_if_pin_sel.size(), 1u);
    EXPECT_EQ(h.this_epoch_if_pin_sel.at("G01"),
              (CbDatumRowsHarness::Pair{"C1C", "C2W"}));
    EXPECT_EQ(h.this_epoch_if_pin_sel.count("G02"), 0u);
    // The hold latch is neither read nor written on the OFF path.
    EXPECT_TRUE(h.held_clock_ref.empty());
    EXPECT_EQ(out.n_if_pin_sats, 2);      // both sats still get a pin row
    EXPECT_EQ(out.n_s2z_channels, 2);     // G|C1C (2 sats) + G|C2W (1 sat)
    // The DECLARATION record is populated regardless of the disc channel — same
    // pair, but a different map with a different purpose (see the next test).
    ASSERT_EQ(h.this_epoch_if_pin_real.size(), 1u);
    EXPECT_EQ(h.this_epoch_if_pin_real.at("G01"),
              (CbDatumRowsHarness::Pair{"C1C", "C2W"}));
}

// S-contract §1.5: the DECLARATION record (this_epoch_if_pin_real /
// this_epoch_gf_pin_real) must be independent of BOTH the disc channel and the
// basis hold — it is what cbpin= / gfpin= are built from, and a product-contract
// declaration must never depend on a QC-channel knob.
//
// This is the regression guard for the measured field defect: the cbpin fill
// originally read this_epoch_if_pin_sel, which is gated
// `!clock_signal_basis_hold && emit_clock_disc`.  network.emit_clock_disc
// defaults false and appears nowhere in the ft63 -> pc305 field chain, so every
// field posture-C product declared no pin at all and the truth-grader (which
// REFUSES rather than assume a pair) could not transform its B records — while
// the sim arms, which arm the knob, declared fine.
//
// The knobs below are the field posture exactly: hold ON, disc channel OFF, GF
// datum armed.  The declaration must survive; the disc scratch must stay empty
// (that map drives per-satellite discontinuity bumps, so widening it would be a
// WIRE change, not a declaration change).
TEST(NetworkSbasisTest, BuildCodeBiasDatumRowsDeclarationRecordIsUngated) {
    NetworkEstimator est;
    est.registerCodeBias("G01", "C1C");
    est.registerCodeBias("G01", "C2W");
    est.registerCodeBias("G01", "C5Q");   // third band: the estimable content
    est.registerCodeBias("G02", "C1C");   // single-frequency: no IF pin, no GF row

    CbDatumRowsHarness h;
    h.clock_signal_basis_hold = true;     // the hold branch records no selection...
    h.emit_clock_disc         = false;    // ...and the disc scratch is gated off
    h.gf_datum                = true;     // slant posture
    const auto out = h.build(
        est, {{"G01", "C1C"}, {"G01", "C2W"}, {"G01", "C5Q"}, {"G02", "C1C"}});

    // The disc scratch stays EMPTY — the QC channel is untouched by this change.
    EXPECT_TRUE(h.this_epoch_if_pin_sel.empty());
    // The declaration record carries the HELD pair anyway.
    ASSERT_EQ(h.this_epoch_if_pin_real.size(), 1u);
    EXPECT_EQ(h.this_epoch_if_pin_real.at("G01"),
              (CbDatumRowsHarness::Pair{"C1C", "C2W"}));
    EXPECT_EQ(h.this_epoch_if_pin_real.count("G02"), 0u);   // no dual-freq pin
    EXPECT_EQ(h.held_clock_ref.at("G01"),
              (CbDatumRowsHarness::Pair{"C1C", "C2W"}));    // hold really did latch
    // The GF record names the satellites that took the second row — the same set
    // the gfpin= unanimity rule tallies over.
    EXPECT_EQ(h.this_epoch_gf_pin_real, (std::set<std::string>{"G01"}));
    EXPECT_EQ(out.n_gf_pin_sats, 1);
    EXPECT_EQ(out.n_gf_only_pair, 0);     // G01 has C5Q outside the pinned pair

    // GF datum OFF: the pair is still declared, the GF set is empty (so no gfpin=).
    CbDatumRowsHarness h0;
    h0.clock_signal_basis_hold = true;
    h0.gf_datum                = false;
    const auto out0 = h0.build(
        est, {{"G01", "C1C"}, {"G01", "C2W"}, {"G01", "C5Q"}, {"G02", "C1C"}});
    EXPECT_EQ(h0.this_epoch_if_pin_real.size(), 1u);
    EXPECT_TRUE(h0.this_epoch_gf_pin_real.empty());
    EXPECT_EQ(out0.n_gf_pin_sats, 0);
}

// ---------------------------------------------------------------------------
// INC-8: dumpEstimabilitySpectrum — one-shot estimability diagnostic relocated
// out of the run loop.  Smoke over a real registered/added system: no throw,
// and the "Network[estimability]" warn lines land in the sink with the
// driver-format labels when col_desc is supplied and the "colN" fallback when
// it is empty.
// ---------------------------------------------------------------------------
TEST(NetworkEstimatorTest, DumpEstimabilitySpectrumSmoke) {
    const Truth t = makeTruth(/*seed=*/2024, /*R=*/3, /*S=*/3);
    const std::string signal = "L1";

    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(t.S), pbCol(t.S);
    for (int s = 0; s < t.S; ++s) {
        clkCol[s] = est.registerClock(t.sats[s]);
        pbCol[s]  = est.registerPhaseBias(t.sats[s], signal);
    }
    // Driver-format labels ("CLK|sat" / "PB|sat|sig" — deliberately NOT
    // sharedColumnLabel), indexed by shared column.
    std::vector<std::string> col_desc(static_cast<std::size_t>(est.numShared()));
    for (int s = 0; s < t.S; ++s) {
        col_desc[static_cast<std::size_t>(clkCol[s])] = "CLK|" + t.sats[s];
        col_desc[static_cast<std::size_t>(pbCol[s])]  = "PB|" + t.sats[s] + "|" + signal;
    }

    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        buildStation(t, r, est, clkCol, pbCol, A_s, A_r, w, y);  // pivot r=0 folds amb pins
        est.addStation(A_s, A_r, w, y);
    }
    // One constellation, no clock pin folded -> exactly one near-zero (clock
    // gauge) direction for the dump to name; declared_dim = 1 matches it.

    // (i) full col_desc: the gauge direction is named with the driver labels.
    std::ostringstream oss_full;
    auto log_full = std::make_shared<spdlog::logger>(
        "estim_dump_full", std::make_shared<spdlog::sinks::ostream_sink_mt>(oss_full));
    EXPECT_NO_THROW(est.dumpEstimabilitySpectrum(3600.0, 1, col_desc, log_full));
    const std::string text_full = oss_full.str();
    EXPECT_NE(text_full.find("estimability"), std::string::npos);
    EXPECT_NE(text_full.find("CLK|G01"), std::string::npos)
        << "gauge direction should be named via col_desc";

    // (ii) empty col_desc: every spanning parameter falls back to "colN".
    std::ostringstream oss_bare;
    auto log_bare = std::make_shared<spdlog::logger>(
        "estim_dump_bare", std::make_shared<spdlog::sinks::ostream_sink_mt>(oss_bare));
    EXPECT_NO_THROW(est.dumpEstimabilitySpectrum(3600.0, 1,
                                                 std::vector<std::string>{}, log_bare));
    const std::string text_bare = oss_bare.str();
    EXPECT_NE(text_bare.find("estimability"), std::string::npos);
    EXPECT_NE(text_bare.find("col0"), std::string::npos)
        << "missing labels should fall back to colN";
}

} // namespace
} // namespace GnssProcessor
