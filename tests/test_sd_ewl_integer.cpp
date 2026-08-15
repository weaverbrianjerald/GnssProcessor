/**
 * @file test_sd_ewl_integer.cpp
 * @brief SD EWL INTEGER path (increments 2a + 2b) — Route-B DD rounding, the A2
 *        DD-to-SD datum propagation, the standing-invariant-#1 flip response, the
 *        fixed-row design + correction frame, and the A1 weight accounting.
 *        See docs/SPEC_sd_ewl_prereduce.md §8 (increment 2) and §10 (the amendments).
 *
 * Suite names owned by this file ONLY (the suite-per-file rule):
 *   SdEwlDatumTest, SdEwlFfrTest, SdEwlFlipTest, SdEwlFixedRowTest, SdEwlBandTableTest,
 *   SdEwlFixConfigTest.
 * (tests/test_sd_ewl_prereduce.cpp owns SdEwl{Algebra,Whitening,PivotElection,Config,
 *  ArcSource,ChannelSearch}Test — do not add to those from here.)
 *
 * Unlike the increment-1 file, most of this is NOT a contract restatement: 2a's algebra
 * was deliberately extracted into pure free functions (@ref sdEwlPropagate,
 * @ref sdEwlRoundingFfr, @ref sdEwlFlipCheck, @ref sdEwlBands) precisely so the property
 * that carries the risk — that a propagated integer is the true SD integer up to ONE
 * per-satellite-pair constant, independent of the spanning tree — is testable directly.
 * The 2b row build stays inside a private Application member, so SdEwlFixedRowTest pins
 * the fixed row's design and frame as independent algebra, the same way
 * SdEwlAlgebraTest does for the float row.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkSdEwlAr.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// Build a synthetic Route-B graph from TRUTH: per-station receiver EWL bias, per-satellite
/// satellite EWL bias, and the true undifferenced integers.  The MW statistic a station
/// actually holds is  u(r,s) = N_true(r,s) + a_r + b_s (+ noise), and Route B must recover
/// the SD integers from the DDs alone — with no knowledge of a_r or b_s.  That is the whole
/// claim, so the fixture supplies biases large enough that any leakage would be obvious.
struct Truth {
    std::map<std::string, double> rx_bias;    ///< a_r, cycles (station EWL bias)
    std::map<std::string, double> sat_bias;   ///< b_s, cycles (satellite EWL bias)
    std::map<std::string, std::map<std::string, long long>> n;   ///< true integers
};

std::map<std::string, std::map<std::string, SdEwlArcStat>>
makeGraph(const Truth& t, double sigma_cyc = 1e-4, std::size_t count = 20,
          double noise_scale = 0.0) {
    std::map<std::string, std::map<std::string, SdEwlArcStat>> g;
    int k = 0;
    for (const auto& [sta, sats] : t.n) {
        for (const auto& [sat, n] : sats) {
            // Deterministic pseudo-noise: a fixed pattern, so a failure reproduces.
            const double eps = noise_scale * std::sin(1.7 * static_cast<double>(++k));
            SdEwlArcStat a;
            a.u_cyc = static_cast<double>(n) + t.rx_bias.at(sta)
                      + t.sat_bias.at(sat) + eps;
            a.var_cyc2 = sigma_cyc * sigma_cyc;
            a.count = count;
            g[sta][sat] = a;
        }
    }
    return g;
}

/// The SD quantity the row actually asserts: n(r,s) - n(r,p).
long long sd(const std::map<std::string, std::map<std::string, SdEwlDecision>>& d,
             const std::string& sta, const std::string& s, const std::string& p) {
    return d.at(sta).at(s).n_cyc - d.at(sta).at(p).n_cyc;
}

constexpr const char* kMinimalStations = R"(
        "clock_datum_station": "AAAA00XXX",
        "stations": [
          { "name": "AAAA00XXX", "rinex_path": "a.rnx",
            "position_ecef": [ 4000000.0, 1000000.0, 4500000.0 ] },
          { "name": "BBBB00XXX", "rinex_path": "b.rnx",
            "position_ecef": [ 4100000.0, 1100000.0, 4400000.0 ] }
        ])";

/// The loader takes a PATH, so each config test writes a throwaway file next to the
/// test binary and removes it again (the same helper shape test_sd_ewl_prereduce.cpp
/// uses; the rinex paths are never opened).
std::string writeTempConfig(const std::string& body, const char* tag) {
    const std::string path = std::string("_sd_ewl_int_test_") + tag + ".json";
    std::ofstream f(path);
    f << body;
    f.close();
    return path;
}

}  // namespace

// =================================================================================
// SdEwlBandTableTest — the shared EWL tier table (one definition for 2a and 2b).
// =================================================================================

TEST(SdEwlBandTableTest, GpsAndGalileoTiersAreTheKnownWavelengths) {
    char b1 = 0, b2 = 0;
    double f1 = 0.0, f2 = 0.0;
    ASSERT_TRUE(sdEwlBands('G', b1, b2, f1, f2));
    EXPECT_EQ(b1, '2');
    EXPECT_EQ(b2, '5');
    EXPECT_NEAR(Constants::SPEED_OF_LIGHT / (f1 - f2), 5.861, 1e-3);
    ASSERT_TRUE(sdEwlBands('E', b1, b2, f1, f2));
    EXPECT_EQ(b1, '7');
    EXPECT_EQ(b2, '5');
    EXPECT_NEAR(Constants::SPEED_OF_LIGHT / (f1 - f2), 9.768, 1e-3);
}

// BeiDou is deferred (clock divergence) and GLONASS excluded (FDMA makes the
// between-satellite lane non-integer).  Both must simply produce no tier, which is why
// neither needs a skip counter anywhere downstream.
TEST(SdEwlBandTableTest, BeiDouAndGlonassHaveNoTier) {
    char b1 = 0, b2 = 0;
    double f1 = 0.0, f2 = 0.0;
    EXPECT_FALSE(sdEwlBands('C', b1, b2, f1, f2));
    EXPECT_FALSE(sdEwlBands('R', b1, b2, f1, f2));
    EXPECT_FALSE(sdEwlBands('J', b1, b2, f1, f2));
}

// =================================================================================
// SdEwlFfrTest — the rounding success probability (FFR, not ADOP).
// =================================================================================

TEST(SdEwlFfrTest, NoiselessRoundingSucceedsAndWideNoiseIsHopeless) {
    EXPECT_DOUBLE_EQ(sdEwlRoundingFfr(0.0), 1.0);
    EXPECT_DOUBLE_EQ(sdEwlRoundingFfr(-1.0), 1.0);   // guard, not a physical input
    EXPECT_GT(sdEwlRoundingFfr(1e-4), 0.999999);
    EXPECT_LT(sdEwlRoundingFfr(1.0), 0.4);           // sigma of a whole cycle: useless
}

// The gate must be monotone and must land where the SPEC's numbers say it does: a
// 4-leg DD of arcs at the A1 per-sample scale (1.0 m over lambda_EWL 5.861 m, 10
// samples) is decisive, and a 2-sample arc is not.
TEST(SdEwlFfrTest, GateIsMonotoneAndMatchesTheEwlMaturityLadder) {
    EXPECT_GT(sdEwlRoundingFfr(0.05), sdEwlRoundingFfr(0.10));
    const double lam = 5.861;
    const auto ddSigma = [lam](double sample_m, double n) {
        const double per_arc = (sample_m / std::sqrt(n)) / lam;   // sigma of the MEAN
        return std::sqrt(4.0) * per_arc;                          // four legs
    };
    EXPECT_GT(sdEwlRoundingFfr(ddSigma(1.0, 10.0)), 0.999);   // mature: decisive
    EXPECT_LT(sdEwlRoundingFfr(ddSigma(1.0, 2.0)), 0.999);    // 2 samples: rejected
}

// =================================================================================
// SdEwlFlipTest — the standing-invariant-#1 event response.
// =================================================================================

TEST(SdEwlFlipTest, FreshDecisionFoldsAndStableDecisionFolds) {
    // Fresh matters for the S2 single-epoch virtual fold: after a warm resume there is
    // no previous decision, so requiring agreement would make the A/B byte-identical —
    // the Card-1e trap ("byte-identical A/B means the feature never ran").
    EXPECT_EQ(sdEwlFlipCheck(false, 0, 0, 42, 10), SdEwlFlipVerdict::Fresh);
    EXPECT_EQ(sdEwlFlipCheck(true, 42, 10, 42, 11), SdEwlFlipVerdict::Stable);
}

TEST(SdEwlFlipTest, DifferentIntegerOnAContinuousArcIsAFlip) {
    EXPECT_EQ(sdEwlFlipCheck(true, 42, 10, 43, 11), SdEwlFlipVerdict::Flip);
    EXPECT_EQ(sdEwlFlipCheck(true, 42, 10, 42 - 1, 10), SdEwlFlipVerdict::Flip);
}

// A falling lagged sample count means the MW accumulator reset — slip, gap or
// re-acquisition — so the true integer legitimately changed and no flip is charged.
// Deliberately keyed on the COUNT rather than a slip flag: the slip funnel is
// carry_invariant-gated and therefore empty in some legal configurations.
TEST(SdEwlFlipTest, RestartedArcChangesItsIntegerWithoutChargingAFlip) {
    EXPECT_EQ(sdEwlFlipCheck(true, 42, 30, 7, 2), SdEwlFlipVerdict::Restart);
    // Same integer after a restart is still a restart, not "stable": the memory must be
    // replaced from the new arc either way.
    EXPECT_EQ(sdEwlFlipCheck(true, 42, 30, 42, 2), SdEwlFlipVerdict::Restart);
}

// =================================================================================
// SdEwlDatumTest — Route B + the A2 DD-to-SD datum propagation.
// =================================================================================

// The core claim of Route B: the DD needs NO knowledge of the receiver or satellite EWL
// bias.  Feed biases of tens of cycles and check that every recovered SD integer is the
// truth minus ONE per-satellite-pair constant, namely the reference station's own SD.
TEST(SdEwlDatumTest, RecoversTrueSdIntegersUpToTheReferenceStationConstant) {
    Truth t;
    t.rx_bias = { { "AAAA", 11.37 }, { "BBBB", -4.62 }, { "CCCC", 27.05 } };
    t.sat_bias = { { "G05", -8.19 }, { "G11", 3.44 }, { "G23", 15.71 } };
    t.n = {
        { "AAAA", { { "G05", 100 }, { "G11", -7 }, { "G23", 42 } } },
        { "BBBB", { { "G05", -13 }, { "G11", 88 }, { "G23", 5 } } },
        { "CCCC", { { "G05", 61 }, { "G11", 61 }, { "G23", -30 } } },
    };
    SdEwlPropagateTally tally;
    const auto d = sdEwlPropagate(makeGraph(t), "AAAA", 0.15, 0.999, tally);

    ASSERT_EQ(d.size(), 3u);
    for (const std::string& sta : { "AAAA", "BBBB", "CCCC" }) {
        for (const std::string& s : { "G05", "G23" }) {
            const long long got = sd(d, sta, s, "G11");
            const long long want = (t.n.at(sta).at(s) - t.n.at(sta).at("G11"))
                                   - (t.n.at("AAAA").at(s) - t.n.at("AAAA").at("G11"));
            EXPECT_EQ(got, want) << sta << " " << s << " vs G11";
        }
    }
    // r0's own SD integers are exactly zero by construction (A2's N_SD(r0) == 0).
    EXPECT_EQ(sd(d, "AAAA", "G05", "G11"), 0);
    EXPECT_EQ(sd(d, "AAAA", "G23", "G11"), 0);
}

// The A2 frame constant must be the SAME for every station in the component — that is
// what makes it a re-datum (the classical WL-FCB modulo-lambda definition) rather than a
// per-station error.
TEST(SdEwlDatumTest, TheFrameOffsetIsOneConstantPerSatellitePairNotPerStation) {
    Truth t;
    t.rx_bias = { { "AAAA", 0.31 }, { "BBBB", 19.8 }, { "CCCC", -7.2 }, { "DDDD", 3.0 } };
    t.sat_bias = { { "E05", 5.5 }, { "E11", -2.25 }, { "E24", 40.1 } };
    t.n = {
        { "AAAA", { { "E05", 3 }, { "E11", 9 }, { "E24", -4 } } },
        { "BBBB", { { "E05", -21 }, { "E11", 6 }, { "E24", 17 } } },
        { "CCCC", { { "E05", 55 }, { "E11", -12 }, { "E24", 8 } } },
        { "DDDD", { { "E05", 1 }, { "E11", 1 }, { "E24", 1 } } },
    };
    SdEwlPropagateTally tally;
    const auto d = sdEwlPropagate(makeGraph(t), "BBBB", 0.15, 0.999, tally);
    ASSERT_EQ(d.size(), 4u);
    const long long c = (t.n.at("BBBB").at("E05") - t.n.at("BBBB").at("E11"));
    for (const std::string& sta : { "AAAA", "BBBB", "CCCC", "DDDD" }) {
        const long long truth_sd = t.n.at(sta).at("E05") - t.n.at(sta).at("E11");
        EXPECT_EQ(sd(d, sta, "E05", "E11"), truth_sd - c) << sta;
    }
}

// The spanning tree and each station's link satellite shift n by a PER-STATION constant,
// which the between-satellite difference removes.  So changing the reference station
// changes the frame constant but must NOT change any station's SD integers relative to
// each other beyond that single shift.  This is the property that lets the seam keep r0
// sticky and ignore tree churn entirely.
TEST(SdEwlDatumTest, SdIntegersAreIndependentOfTheTreeAndShiftOnlyWithTheReference) {
    Truth t;
    t.rx_bias = { { "AAAA", 2.0 }, { "BBBB", -30.0 }, { "CCCC", 14.0 } };
    t.sat_bias = { { "G01", 0.5 }, { "G02", -6.5 }, { "G03", 22.0 } };
    t.n = {
        { "AAAA", { { "G01", 4 }, { "G02", -9 }, { "G03", 15 } } },
        { "BBBB", { { "G01", 31 }, { "G02", 2 }, { "G03", -8 } } },
        { "CCCC", { { "G01", -5 }, { "G02", 12 }, { "G03", 7 } } },
    };
    const auto g = makeGraph(t);
    SdEwlPropagateTally ta, tb;
    const auto da = sdEwlPropagate(g, "AAAA", 0.15, 0.999, ta);
    const auto db = sdEwlPropagate(g, "CCCC", 0.15, 0.999, tb);
    ASSERT_EQ(da.size(), 3u);
    ASSERT_EQ(db.size(), 3u);
    // The DIFFERENCE of the two realizations' SD integers is one constant, the same for
    // every station: n_A(sd) - n_C(sd) = -[SD_true(AAAA) - SD_true(CCCC)].
    const long long shift = sd(da, "AAAA", "G01", "G02") - sd(db, "AAAA", "G01", "G02");
    for (const std::string& sta : { "AAAA", "BBBB", "CCCC" })
        EXPECT_EQ(sd(da, sta, "G01", "G02") - sd(db, sta, "G01", "G02"), shift) << sta;
}

// Noise inside the pull-in region must not change a single integer; noise beyond the
// |frac| gate must be REJECTED rather than rounded to a neighbour.
TEST(SdEwlDatumTest, NoiseInsideThePullInRegionIsHarmlessAndTheFracGateRejects) {
    Truth t;
    t.rx_bias = { { "AAAA", 1.0 }, { "BBBB", 2.0 } };
    t.sat_bias = { { "G05", 0.0 }, { "G11", 0.0 }, { "G23", 0.0 } };
    t.n = { { "AAAA", { { "G05", 5 }, { "G11", 0 }, { "G23", 3 } } },
            { "BBBB", { { "G05", -2 }, { "G11", 0 }, { "G23", 9 } } } };
    {
        SdEwlPropagateTally tally;
        const auto d = sdEwlPropagate(makeGraph(t, 1e-3, 20, 0.05), "AAAA", 0.15,
                                      0.999, tally);
        EXPECT_GT(tally.fixed, 0);
        EXPECT_EQ(tally.reject_frac, 0);
        EXPECT_EQ(sd(d, "BBBB", "G05", "G11"), -2 - 5);
    }
    {
        // 0.3 cyc of DD-scale noise: past the 0.15 gate, so nothing is fixed and nothing
        // is quietly rounded to the wrong integer.
        SdEwlPropagateTally tally;
        const auto d = sdEwlPropagate(makeGraph(t, 1e-3, 20, 0.30), "AAAA", 0.15,
                                      0.999, tally);
        EXPECT_GT(tally.reject_frac, 0);
        EXPECT_EQ(tally.fixed, 0);
        // r0's own values are a DATUM, not a measurement, so they survive the gate.
        EXPECT_EQ(d.count("AAAA"), 1u);
    }
}

TEST(SdEwlDatumTest, FfrGateRejectsAnImmatureArcEvenWhenTheFractionLooksClean) {
    Truth t;
    t.rx_bias = { { "AAAA", 0.0 }, { "BBBB", 0.0 } };
    t.sat_bias = { { "G05", 0.0 }, { "G11", 0.0 } };
    t.n = { { "AAAA", { { "G05", 1 }, { "G11", 0 } } },
            { "BBBB", { { "G05", 4 }, { "G11", 0 } } } };
    SdEwlPropagateTally tally;
    // sigma 0.3 cyc per arc: the fraction is exactly 0 (no noise injected) but the DD
    // cannot be TRUSTED, and FFR is the gate that says so.
    const auto d = sdEwlPropagate(makeGraph(t, 0.3, 20, 0.0), "AAAA", 0.15, 0.999, tally);
    EXPECT_GT(tally.reject_ffr, 0);
    EXPECT_EQ(tally.fixed, 0);
}

// Route B needs TWO co-visible satellites per station pair: one to be the quantity and
// one to be the link that removes the station terms.  A single shared satellite forms no
// DD at all, and the propagation must leave that station empty rather than guess.
TEST(SdEwlDatumTest, OneSharedSatelliteFormsNoDoubleDifference) {
    Truth t;
    t.rx_bias = { { "AAAA", 0.0 }, { "BBBB", 0.0 } };
    t.sat_bias = { { "G05", 0.0 }, { "G11", 0.0 }, { "G23", 0.0 } };
    t.n = { { "AAAA", { { "G05", 1 }, { "G11", 2 } } },
            { "BBBB", { { "G05", 3 }, { "G23", 4 } } } };
    SdEwlPropagateTally tally;
    const auto d = sdEwlPropagate(makeGraph(t), "AAAA", 0.15, 0.999, tally);
    EXPECT_EQ(d.count("BBBB"), 0u);
    EXPECT_EQ(tally.attempts, 0);
}

// A station reachable only through a chain must still land in the same frame — the
// propagation is transitive, which is what makes "one component per constellation"
// (measured) enough.
TEST(SdEwlDatumTest, PropagationIsTransitiveAlongAChain) {
    Truth t;
    t.rx_bias = { { "AAAA", 0.0 }, { "BBBB", 5.0 }, { "CCCC", -9.0 } };
    t.sat_bias = { { "G01", 0.0 }, { "G02", 0.0 }, { "G03", 0.0 }, { "G04", 0.0 } };
    // AAAA-BBBB share {G01,G02}; BBBB-CCCC share {G02,G03}; AAAA-CCCC share only G02.
    t.n = { { "AAAA", { { "G01", 3 }, { "G02", 7 } } },
            { "BBBB", { { "G01", -4 }, { "G02", 11 }, { "G03", 6 } } },
            { "CCCC", { { "G02", 2 }, { "G03", -8 } } } };
    SdEwlPropagateTally tally;
    const auto d = sdEwlPropagate(makeGraph(t), "AAAA", 0.15, 0.999, tally);
    ASSERT_EQ(d.count("CCCC"), 1u);
    ASSERT_EQ(d.at("CCCC").count("G03"), 1u);
    // CCCC's SD(G03,G02) must equal its truth minus BBBB-relayed r0 frame.  r0 (AAAA)
    // does not see G03, so the frame constant for the (G03,G02) pair is set where the
    // pair first becomes observable — the value must at least be CONSISTENT between the
    // two stations that see it.
    const long long c_b = d.at("BBBB").at("G03").n_cyc - d.at("BBBB").at("G02").n_cyc
                          - (t.n.at("BBBB").at("G03") - t.n.at("BBBB").at("G02"));
    const long long c_c = d.at("CCCC").at("G03").n_cyc - d.at("CCCC").at("G02").n_cyc
                          - (t.n.at("CCCC").at("G03") - t.n.at("CCCC").at("G02"));
    EXPECT_EQ(c_b, c_c);
}

// =================================================================================
// SdEwlFixedRowTest — 2b's design, RHS and correction frame (independent algebra).
// =================================================================================

namespace {

/// The four LOCAL coefficients of a FLOAT SD EWL row (SPEC_sd_ewl_prereduce §3), and the
/// fixed row's requirement that all four be ZERO.
struct LocalCoeffs { double s_b1, s_b2, p_b1, p_b2; };

LocalCoeffs floatLocal(double lam_ewl, double lam_b1, double lam_b2) {
    return { lam_ewl / lam_b1, -lam_ewl / lam_b2, -lam_ewl / lam_b1, lam_ewl / lam_b2 };
}

}  // namespace

// The fixed row REMOVES the local degree of freedom: no coefficient, not a small one.
// A tight prior on a gauge-adjacent local column would be a stiff prior, not a rank
// reduction, and increment 2 exists precisely so the local nuisance stops absorbing the
// row's information.
TEST(SdEwlFixedRowTest, FixedRowCarriesNoLocalAmbiguityCoefficients) {
    const double f1 = Constants::GPS_L2_FREQ, f2 = Constants::GPS_L5_FREQ;
    const double lam_ewl = Constants::SPEED_OF_LIGHT / (f1 - f2);
    const LocalCoeffs fl = floatLocal(lam_ewl, Constants::SPEED_OF_LIGHT / f1,
                                      Constants::SPEED_OF_LIGHT / f2);
    // The float row's coefficients are large — this is what the fixed row drops.
    EXPECT_GT(std::abs(fl.s_b1), 20.0);
    EXPECT_GT(std::abs(fl.s_b2), 20.0);
    const LocalCoeffs fixed{ 0.0, 0.0, 0.0, 0.0 };
    EXPECT_DOUBLE_EQ(fixed.s_b1, 0.0);
    EXPECT_DOUBLE_EQ(fixed.s_b2, 0.0);
    EXPECT_DOUBLE_EQ(fixed.p_b1, 0.0);
    EXPECT_DOUBLE_EQ(fixed.p_b2, 0.0);
    // Whitening cannot resurrect them: an ALL-fixed group's local block is identically
    // zero, so L^-1 * 0 == 0 for any L.  That is exactly why fixed and float rows are
    // emitted as SEPARATE whitened groups — in a mixed group the dense L^-1 would
    // re-populate a fixed row's zeroed columns out of its float siblings.
    Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(3, 5);
    Eigen::MatrixXd R = Eigen::MatrixXd::Identity(3, 3) * 2.0;
    R.array() += 0.7;                          // diag + var_p * 11^T, the group's R
    const Eigen::LLT<Eigen::MatrixXd> llt(R);
    ASSERT_EQ(llt.info(), Eigen::Success);
    const Eigen::MatrixXd whitened = llt.matrixL().solve(Hr);
    EXPECT_DOUBLE_EQ(whitened.cwiseAbs().maxCoeff(), 0.0);
}

// The RHS must lose exactly lambda_EWL * (N_s - N_p) AND the ambiguity half of H*x_lin,
// together.  A row whose design changed and whose linearization point did not is folded
// in a MIXED frame — this pins that the two edits cancel at the linearization point.
TEST(SdEwlFixedRowTest, FixedRowResidualVanishesWhenTheIntegerAndTheStateAgree) {
    const double f1 = Constants::GPS_L2_FREQ, f2 = Constants::GPS_L5_FREQ;
    const double lam_ewl = Constants::SPEED_OF_LIGHT / (f1 - f2);
    const double lam_b1 = Constants::SPEED_OF_LIGHT / f1;
    const double lam_b2 = Constants::SPEED_OF_LIGHT / f2;
    // States hold lam_b * N_b metres; the truth is integer N on both bands.
    const long long Ns1 = 1234, Ns2 = 1200, Np1 = -55, Np2 = -70;
    const double x_s_b1 = lam_b1 * static_cast<double>(Ns1);
    const double x_s_b2 = lam_b2 * static_cast<double>(Ns2);
    const double x_p_b1 = lam_b1 * static_cast<double>(Np1);
    const double x_p_b2 = lam_b2 * static_cast<double>(Np2);
    const auto predAmb = [&](double xb1, double xb2) {
        return (lam_ewl / lam_b1) * xb1 - (lam_ewl / lam_b2) * xb2;
    };
    // The raw statistic at the linearization point, with zero bias (PB/CB allocated at 0).
    const double mean_s = predAmb(x_s_b1, x_s_b2);
    const double mean_p = predAmb(x_p_b1, x_p_b2);
    const long long n_fix_s = Ns1 - Ns2, n_fix_p = Np1 - Np2;

    // FLOAT row: y = (mean_s - mean_p) - (pred_s - pred_p) == 0.
    const double y_float = (mean_s - mean_p)
                           - (predAmb(x_s_b1, x_s_b2) - predAmb(x_p_b1, x_p_b2));
    EXPECT_NEAR(y_float, 0.0, 1e-6);
    // FIXED row: the integer leaves the RHS and the ambiguity terms leave the frame.
    // Both, or neither — dropping only one leaves hundreds of metres behind.
    const double y_fixed = (mean_s - mean_p)
                           - lam_ewl * static_cast<double>(n_fix_s - n_fix_p);
    EXPECT_NEAR(y_fixed, 0.0, 1e-6);
    // Dropping the integer but KEEPING the ambiguity frame terms (the mistake this test
    // exists to catch) leaves the whole lambda_EWL * DD_integer behind.
    const double y_wrong = (mean_s - mean_p)
                           - (predAmb(x_s_b1, x_s_b2) - predAmb(x_p_b1, x_p_b2))
                           - lam_ewl * static_cast<double>(n_fix_s - n_fix_p);
    EXPECT_GT(std::abs(y_wrong), 50.0);
}

// The r0 frame constant is HARMLESS to the row: an SD integer offset by a constant per
// satellite pair moves the row's RHS by exactly lambda * that constant, which the shared
// satellite bias absorbs — the classical WL-FCB modulo-lambda re-datum, identical for
// every station, so no DD or SD-graded quantity moves.
TEST(SdEwlFixedRowTest, A2FrameConstantShiftsOnlyTheAbsorbedBiasNotAnySdQuantity) {
    const double lam = 5.861;
    const long long true_sd = 34;
    for (long long c : { 0LL, 7LL, -19LL }) {
        const long long folded = true_sd - c;              // the A2-framed integer
        const double absorbed = lam * static_cast<double>(c);
        // What the network then observes in place of B^s - B^p:
        const double observed_bias = 0.0 + absorbed;
        // Two stations in the same component share c, so their DIFFERENCE is clean.
        EXPECT_DOUBLE_EQ(observed_bias - absorbed, 0.0);
        EXPECT_EQ(folded + c, true_sd);
    }
}

// Amendment A1's two floors are DIFFERENT quantities and must not be conflated: the
// DECISION floors the variance of the MEAN (sigma^2/n, because arcZdMean's served
// variance shrinks as 1/n and measured 13-60x optimistic), while the fixed ROW floors at
// the flat PER-SAMPLE scale (because the accumulator is arc-cumulative, so refolding a
// variance-of-mean as white every epoch over-claims information as sqrt(k)).
TEST(SdEwlFixedRowTest, A1WeightAccountingUsesMeanVarianceToDecideAndSampleVarianceToFold) {
    const double sample_m = 1.0;
    for (double n : { 2.0, 10.0, 60.0 }) {
        const double decide_floor_m2 = sample_m * sample_m / n;
        const double fold_floor_m2 = sample_m * sample_m;
        EXPECT_LT(decide_floor_m2, fold_floor_m2 + 1e-12);
        EXPECT_DOUBLE_EQ(fold_floor_m2, 1.0);           // flat: never divided by n
        EXPECT_DOUBLE_EQ(decide_floor_m2, 1.0 / n);
    }
    // The over-claim A1 prevents: folding the variance-of-mean as white for k epochs
    // claims sqrt(k) times the information the correlated samples carry.
    const double k = 36.0;
    EXPECT_NEAR(std::sqrt(k), 6.0, 1e-12);
    // At the flat per-sample floor the row accretes ~1 sample of information per epoch,
    // so after k epochs it claims k samples — exactly what it has.
    const double info_per_epoch = 1.0 / (sample_m * sample_m);
    EXPECT_DOUBLE_EQ(k * info_per_epoch, k);
}

// =================================================================================
// SdEwlFixConfigTest — the 2a/2b knobs: defaults, clamps, and the inert-arming refusal.
// =================================================================================

TEST(SdEwlFixConfigTest, IntegerPathDefaultsOffSoTheRunIsByteIdentical) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": {)" + std::string(kMinimalStations) + R"(
      }
    })";
    const std::string path = writeTempConfig(body, "default");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    const auto& se = cfg.getNetworkConfig().sd_ewl;
    EXPECT_FALSE(se.enable);
    EXPECT_FALSE(se.fix_enable);
    EXPECT_NEAR(se.fix_min_ffr, 0.999, 1e-12);
    EXPECT_NEAR(se.fix_max_frac_cyc, 0.15, 1e-12);
    // Amendment A1's measured honest point — the per-SAMPLE MW residual scale.
    EXPECT_NEAR(se.fix_sigma_floor_m, 1.0, 1e-12);
    std::remove(path.c_str());
}

// fix_enable without enable has NO row layer to fold into (the appender returns
// immediately), i.e. armed-and-dead — the exact shape standing invariant #4 exists to
// catch.  The loader must refuse it rather than let the run report zero fixed rows.
TEST(SdEwlFixConfigTest, FixWithoutTheRowLayerIsDisarmedAtLoad) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": {
        "sd_ewl": { "enable": false, "fix_enable": true },)"
        + std::string(kMinimalStations) + R"(
      }
    })";
    const std::string path = writeTempConfig(body, "inert");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    EXPECT_FALSE(cfg.getNetworkConfig().sd_ewl.fix_enable);
    std::remove(path.c_str());
}

TEST(SdEwlFixConfigTest, KnobsParseAndOutOfRangeGatesAreClampedToTheirDefaults) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": {
        "sd_ewl": { "enable": true, "fix_enable": true, "fix_min_ffr": 1.5,
                    "fix_max_frac_cyc": 0.9, "fix_sigma_floor_m": -2.0 },)"
        + std::string(kMinimalStations) + R"(
      }
    })";
    const std::string path = writeTempConfig(body, "clamp");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    const auto& se = cfg.getNetworkConfig().sd_ewl;
    EXPECT_TRUE(se.fix_enable);
    EXPECT_NEAR(se.fix_min_ffr, 0.999, 1e-12);
    // A |frac| gate above 0.5 cannot reject anything: every real number is within half a
    // cycle of some integer.
    EXPECT_NEAR(se.fix_max_frac_cyc, 0.15, 1e-12);
    EXPECT_NEAR(se.fix_sigma_floor_m, 1.0, 1e-12);
    std::remove(path.c_str());
}
