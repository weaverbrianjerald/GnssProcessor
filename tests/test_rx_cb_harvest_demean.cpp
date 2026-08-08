/**
 * @file test_rx_cb_harvest_demean.cpp
 * @brief Card S2H — the "S-consistent harvest" (docs/SPEC_level_information_budget.md
 *        §17 item 3 / §27 item 1): post-solve Λ-weighted de-mean of the harvested
 *        ABSOLUTE rx-CB realization, per family.
 *
 * The transform is one weighted mean and a subtraction, sitting on a path where a
 * wrong answer produces a plausible number rather than a crash — exactly the
 * coverage rule the LV cards set.  What is pinned here:
 *
 *   1. μ is the INFORMATION-weighted mean of the harvested ABSOLUTE values —
 *      not the arithmetic mean, and not a mean of innovations (LV-2's measured
 *      failure: de-meaning innovations pins the common mode to mean(x_lin)).
 *   2. The transformed set is zero-mean in the FOLD's own metric
 *      (Σλ·(v − μ) ≡ 0), every between-station DIFFERENCE is untouched, and the
 *      transform is IDEMPOTENT (S is a projector: a second reduction returns
 *      μ ≈ 0).
 *   3. λ ≡ const reduces to the LASER oracle case: subset mean subtracted from
 *      ALL points.  The golden GPS3710_test_PMINOLESS output is consumed
 *      directly when present (skip-gated like the other fixture tests): re-pin
 *      LASER's PMINOLESS shifts to a different datum realization, run the
 *      helper over the 6-member set, and require the PMINOLESS realization
 *      back — the S-transform's g-inverse invariance, exercised end to end.
 *   4. The exact diagonal covariance co-transform the caller applies
 *      (voters var − 1/Σλ, screened var + 1/Σλ) equals diag(S·Q·Sᵀ) — verified
 *      against a brute-force Eigen build of S.
 *   5. The robust peer screen keeps a wild cold-start realization out of the μ
 *      VOTE (SPEC §16/12.1's contamination window) while the entry is still
 *      transformed; thin epochs apply NOTHING (identity, counted) — never a
 *      frozen μ.
 *   6. Loader contract: default OFF; INERT-safe without the carry; forces the
 *      superseded LV-2/LV-5 fold-side de-mean OFF; hard errors on a degenerate
 *      screen z or vote floor (standing invariant #4 throughout).
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ===========================================================================
// The Λ-weighted mean and its identities
// ===========================================================================

TEST(RxCbHarvestDemean, MuIsInformationWeightedNotArithmetic) {
    // Two members, 9:1 in information (var 1/9 vs 1).  The arithmetic mean of
    // the values would be 0.5; only the Λ-weighted one zeroes the NET
    // common-mode force the folds exert.
    const std::vector<double> vals{0.0, 1.0};
    const std::vector<double> vars{1.0 / 9.0, 1.0};
    const auto red = rxCbHarvestMu(vals, vars, /*min_n=*/2, /*screen_z=*/1e9);
    ASSERT_TRUE(red.applied);
    EXPECT_NEAR(red.mu, 0.1, 1e-15);
    EXPECT_EQ(red.n, 2);
    EXPECT_EQ(red.n_used, 2);
    EXPECT_EQ(red.n_screened, 0);
    EXPECT_NEAR(red.sum_lam, 10.0, 1e-12);
}

TEST(RxCbHarvestDemean, TransformedSetIsZeroMeanInTheFoldMetricAndKeepsDifferences) {
    const std::vector<double> vals{-1.20, 0.35, 2.75, -0.40, 5.00, 0.05, -3.10, 0.90};
    const std::vector<double> vars{0.01, 0.04, 0.25, 0.0025, 1.0, 1.0 / 64.0, 1.0 / 9.0, 1.0 / 36.0};
    const auto red = rxCbHarvestMu(vals, vars, 2, 1e9);
    ASSERT_TRUE(red.applied);

    // Σλ·(v − μ) ≡ 0: the carried set exerts zero net force on the common mode.
    double sum_lam_resid = 0.0;
    for (size_t i = 0; i < vals.size(); ++i)
        sum_lam_resid += (1.0 / vars[i]) * (vals[i] - red.mu);
    EXPECT_NEAR(sum_lam_resid, 0.0, 1e-9 * red.sum_lam);

    // Between-station differences are untouched by a common shift.
    for (size_t i = 1; i < vals.size(); ++i)
        EXPECT_DOUBLE_EQ((vals[i] - red.mu) - (vals[0] - red.mu), vals[i] - vals[0]);

    // Idempotence (S² = S): reducing the transformed set returns μ ≈ 0.
    std::vector<double> transformed(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) transformed[i] = vals[i] - red.mu;
    const auto red2 = rxCbHarvestMu(transformed, vars, 2, 1e9);
    ASSERT_TRUE(red2.applied);
    EXPECT_NEAR(red2.mu, 0.0, 1e-12);
}

// A uniform shift of every value moves μ by exactly that shift and leaves the
// transformed set unchanged — the transform output is invariant to the input's
// datum realization (the property the LASER oracle exercises with real data).
TEST(RxCbHarvestDemean, OutputIsInvariantToTheInputRealization) {
    const std::vector<double> vals{0.2, -0.5, 1.1, 0.0, -0.8};
    const std::vector<double> vars{0.09, 0.01, 0.16, 0.04, 0.25};
    const double shift = 37.5;   // an arbitrary different realization
    std::vector<double> shifted(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) shifted[i] = vals[i] + shift;

    const auto a = rxCbHarvestMu(vals, vars, 2, 1e9);
    const auto b = rxCbHarvestMu(shifted, vars, 2, 1e9);
    ASSERT_TRUE(a.applied);
    ASSERT_TRUE(b.applied);
    EXPECT_NEAR(b.mu - a.mu, shift, 1e-9);
    for (size_t i = 0; i < vals.size(); ++i)
        EXPECT_NEAR(shifted[i] - b.mu, vals[i] - a.mu, 1e-9);
}

// ===========================================================================
// The LASER golden oracle (skip-gated on the fixture, like FixtureSmokeTest)
// ===========================================================================

// estimated_coordinates.csv layout (verified against the golden output):
// Number(0), ID(1), X(2), Y(3), Z(4), SD X/Y/Z(5-7), rho(8-10), SD ENU(11-13),
// rho ENU(14-16), shift X(17), shift Y(18), shift Z(19), shift ENU(20-22).
std::map<std::string, std::array<double, 3>> readLaserShifts(const fs::path& csv) {
    std::map<std::string, std::array<double, 3>> out;
    std::ifstream f(csv);
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            const auto b = tok.find_first_not_of(" \t\r");
            const auto e = tok.find_last_not_of(" \t\r");
            fields.push_back(b == std::string::npos ? "" : tok.substr(b, e - b + 1));
        }
        if (fields.size() < 20 || fields[1].empty()) continue;
        try {
            out[fields[1]] = {std::stod(fields[17]), std::stod(fields[18]),
                              std::stod(fields[19])};
        } catch (...) { continue; }
    }
    return out;
}

TEST(RxCbHarvestDemean, LaserGps3710PminolessOracle) {
    const fs::path csv =
        "C:/LASER/test/GPS3710_test_PMINOLESS/Output_PMINOLESS/estimated_coordinates.csv";
    if (!fs::exists(csv))
        GTEST_SKIP() << "LASER oracle fixture not present on this machine";

    const auto shifts = readLaserShifts(csv);
    ASSERT_EQ(shifts.size(), 9u) << "oracle CSV parse drift — expected 9 points";

    const std::vector<std::string> members{"DG6515", "DH3860", "DI2250",
                                           "DI3430", "DL2742", "DQ7578"};
    const std::vector<std::string> all{"DG6515", "DH3860", "DI2250", "DI3430",
                                       "DL2742", "DQ7578", "DS1630", "MO0576",
                                       "MO0983"};
    for (const auto& id : all) ASSERT_TRUE(shifts.count(id)) << id;

    // LASER's PMINOLESS shifts are zero-mean over the 6-member set (its own
    // summary asserts the sums).  Re-pin them to a DIFFERENT realization of the
    // same solution (an RLESS-style pin on DG6515), run the helper over the
    // member set with equal weights (the LASER case), apply μ to ALL points,
    // and require the PMINOLESS realization back — g-inverse invariance of the
    // S-transform, on the golden data, per component.
    for (int c = 0; c < 3; ++c) {
        const double pin = shifts.at("DG6515")[c];
        std::vector<double> member_vals, member_vars;
        for (const auto& id : members) {
            member_vals.push_back(shifts.at(id)[c] - pin);
            member_vars.push_back(1.0);   // unit weights = the LASER transform
        }
        const auto red = rxCbHarvestMu(member_vals, member_vars, 6, 1e9);
        ASSERT_TRUE(red.applied);
        EXPECT_EQ(red.n_used, 6);
        double member_mean = 0.0;
        for (const auto& id : all) {
            const double repinned = shifts.at(id)[c] - pin;
            const double recovered = repinned - red.mu;
            // 5e-5 m: the CSV is 4-decimal (0.1 mm), so the member mean of the
            // golden shifts is bounded by the rounding, not by the algebra.
            EXPECT_NEAR(recovered, shifts.at(id)[c], 5e-5)
                << id << " component " << c;
        }
        for (const auto& id : members)
            member_mean += (shifts.at(id)[c] - pin - red.mu);
        EXPECT_NEAR(member_mean / 6.0, 0.0, 5e-5);
    }
}

// ===========================================================================
// The covariance co-transform's closed form vs a brute-force S·Q·Sᵀ
// ===========================================================================

// The caller applies var − 1/Σλ to voters and var + 1/Σλ to screened
// non-voters.  That closed form must BE diag(S·Q·Sᵀ) for diagonal Q and
// S = I − 1·wᵀ (w = λ/Σλ over the voters, zero for non-voters) — a wrong sign
// or a wrong denominator here yields plausible variances and a silently
// mis-weighted next fold.
TEST(RxCbHarvestDemean, VarianceCoTransformMatchesBruteForceSQSt) {
    const std::vector<double> vars{0.01, 0.04, 0.25, 0.0025, 0.09, 4.0};
    const std::vector<bool> votes{true, true, true, true, true, false};
    const int n = static_cast<int>(vars.size());

    double sum_lam = 0.0;
    for (int i = 0; i < n; ++i)
        if (votes[i]) sum_lam += 1.0 / vars[i];

    Eigen::MatrixXd S = Eigen::MatrixXd::Identity(n, n);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c)
            if (votes[c]) S(r, c) -= (1.0 / vars[c]) / sum_lam;
    const Eigen::MatrixXd Q =
        Eigen::Map<const Eigen::VectorXd>(vars.data(), n).asDiagonal();
    const Eigen::MatrixXd SQSt = S * Q * S.transpose();

    for (int i = 0; i < n; ++i) {
        const double closed_form =
            votes[i] ? vars[i] - 1.0 / sum_lam : vars[i] + 1.0 / sum_lam;
        EXPECT_NEAR(SQSt(i, i), closed_form, 1e-12) << "entry " << i;
    }
    // S is a projector on the voting subspace: S·S = S.
    EXPECT_NEAR(((S * S) - S).norm(), 0.0, 1e-12);
}

// ===========================================================================
// The robust peer screen and the thin-set stand-down
// ===========================================================================

TEST(RxCbHarvestDemean, PeerScreenKeepsAColdStartRealizationOutOfTheVote) {
    // Ten settled receivers within ±1 m and one 200 km-class cold-start
    // realization (SPEC §16/12.1's contamination window).  A naive Λ-mean is
    // dragged by kilometres; the screen must exclude the wild vote while the
    // entry still gets transformed by the caller.
    std::vector<double> vals{0.2, -0.5, 1.1, 0.0, -0.8, 0.4, -0.1, 0.7, -0.3, 0.5};
    std::vector<double> vars(vals.size(), 0.01);
    const auto clean = rxCbHarvestMu(vals, vars, 2, 8.0);
    ASSERT_TRUE(clean.applied);

    vals.push_back(2.0e5);
    vars.push_back(0.01);
    const auto screened = rxCbHarvestMu(vals, vars, 2, 8.0);
    ASSERT_TRUE(screened.applied);
    EXPECT_EQ(screened.n_screened, 1);
    EXPECT_EQ(screened.n_used, static_cast<int>(vals.size()) - 1);
    EXPECT_NEAR(screened.mu, clean.mu, 1e-9);

    // With the screen effectively disabled the same contaminator drags μ by
    // ~2e4 m — what this guard exists to prevent.
    const auto unscreened = rxCbHarvestMu(vals, vars, 2, 1e12);
    ASSERT_TRUE(unscreened.applied);
    EXPECT_GT(std::abs(unscreened.mu - clean.mu), 1.0e4);
}

TEST(RxCbHarvestDemean, FlatFamilyScreensNobody) {
    // Identical values ⇒ MAD 0 ⇒ the median-formal-σ floor is what stands; a
    // zero scale would screen everyone and stand the transform down forever.
    const std::vector<double> vals(12, 2.5);
    const std::vector<double> vars(12, 0.04);
    const auto red = rxCbHarvestMu(vals, vars, 2, 8.0);
    ASSERT_TRUE(red.applied);
    EXPECT_EQ(red.n_screened, 0);
    EXPECT_EQ(red.n_used, 12);
    EXPECT_NEAR(red.mu, 2.5, 1e-12);
}

TEST(RxCbHarvestDemean, ThinSetAppliesNothing) {
    // Below the vote floor the transform stands down ENTIRELY — identity, not a
    // frozen μ (there is no held frame to re-assert; the caller counts it).
    const std::vector<double> vals{0.3, -0.3};
    const std::vector<double> vars{0.01, 0.01};
    const auto thin = rxCbHarvestMu(vals, vars, /*min_n=*/8, 8.0);
    EXPECT_FALSE(thin.applied);
    EXPECT_EQ(thin.n, 2);
    EXPECT_DOUBLE_EQ(thin.mu, 0.0);

    // The floor counts VOTES, not entries: 8 entries with one screened is 7
    // votes, and a floor of 8 stands down.
    std::vector<double> vals8{0.1, 0.2, -0.1, 0.0, 0.15, -0.2, 0.05, 5.0e5};
    std::vector<double> vars8(8, 0.01);
    const auto screened_thin = rxCbHarvestMu(vals8, vars8, 8, 8.0);
    EXPECT_EQ(screened_thin.n_screened, 1);
    EXPECT_FALSE(screened_thin.applied);
}

TEST(RxCbHarvestDemean, DegenerateInputsAreSafe) {
    // Empty family.
    const auto empty = rxCbHarvestMu({}, {}, 1, 8.0);
    EXPECT_FALSE(empty.applied);
    EXPECT_EQ(empty.n, 0);

    // Unusable entries (non-finite value, non-positive var) neither vote nor
    // count; an all-unusable family applies nothing.
    const double nan = std::nan("");
    const auto junk = rxCbHarvestMu({nan, 1.0, 0.5}, {0.01, -1.0, 0.0}, 1, 8.0);
    EXPECT_FALSE(junk.applied);
    EXPECT_EQ(junk.n, 0);

    // A single member at floor 1 is the degenerate pin: μ = the value, the
    // transformed entry is exactly zero — valid, and visible as n_used == 1.
    const auto single = rxCbHarvestMu({0.42}, {0.01}, 1, 8.0);
    ASSERT_TRUE(single.applied);
    EXPECT_EQ(single.n_used, 1);
    EXPECT_DOUBLE_EQ(single.mu, 0.42);
}

// ===========================================================================
// Loader contract (standing invariant #4)
// ===========================================================================

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("rx_cb_harvest_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

TEST(RxCbHarvestDemean, DefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkRxCbHarvestDemean());
    EXPECT_DOUBLE_EQ(cfg.networkRxCbHarvestScreenZ(), 8.0);
    std::error_code ec; fs::remove(p, ec);
}

// No carry ⇒ no harvested set to transform ⇒ forced off, never armed-but-inert.
TEST(RxCbHarvestDemean, ForcedOffWithoutTheCarry) {
    LogManager log;
    const fs::path p = writeTempConfig("nocarry",
        networkConfig("    \"rx_cb_harvest_demean\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_FALSE(cfg.networkCarryRxCodeBias());
    EXPECT_FALSE(cfg.networkRxCbHarvestDemean());
    std::error_code ec; fs::remove(p, ec);
}

TEST(RxCbHarvestDemean, SurvivesWithTheCarry) {
    LogManager log;
    const fs::path p = writeTempConfig("carry",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_harvest_demean\": true,\n"
                      "    \"rx_cb_harvest_screen_z\": 6.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.networkCarryRxCodeBias());
    EXPECT_TRUE(cfg.networkRxCbHarvestDemean());
    EXPECT_DOUBLE_EQ(cfg.networkRxCbHarvestScreenZ(), 6.0);
    std::error_code ec; fs::remove(p, ec);
}

// The fold-side de-mean (LV-2, and its LV-5 rider) is a SUPERSEDED mechanism on
// the same channel: composing it with the harvest transform would de-mean a set
// that is already zero-mean-by-construction and re-pin the mode to the frozen
// seed frame.  Arming both must resolve to the harvest alone, loudly.
TEST(RxCbHarvestDemean, ForcesTheSupersededFoldDemeanOff) {
    LogManager log;
    const fs::path p = writeTempConfig("supersede",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_harvest_demean\": true,\n"
                      "    \"rx_cb_carry_demean\": true,\n"
                      "    \"rx_cb_carry_demean_within\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRxCbHarvestDemean());
    EXPECT_FALSE(cfg.networkRxCbCarryDemean());
    EXPECT_FALSE(cfg.networkRxCbCarryDemeanWithin());
    std::error_code ec; fs::remove(p, ec);
}

// LV-2 and LV-4 keep working alone — the supersession must not leak onto
// postures that never armed the harvest.
TEST(RxCbHarvestDemean, DoesNotDisturbLv2WhenNotArmed) {
    LogManager log;
    const fs::path p = writeTempConfig("lv2_alone",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_carry_demean\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkRxCbHarvestDemean());
    EXPECT_TRUE(cfg.networkRxCbCarryDemean());
    std::error_code ec; fs::remove(p, ec);
}

TEST(RxCbHarvestDemean, RejectsANonPositiveScreenZ) {
    LogManager log;
    const fs::path p = writeTempConfig("badz",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_harvest_demean\": true,\n"
                      "    \"rx_cb_harvest_screen_z\": 0.0,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// The vote floor is the shared LV knob, but LV-2's >= 1 validation only runs
// when LV-2 is armed — the S2H-only posture must reject a degenerate floor
// itself rather than run with "min 0 votes".
TEST(RxCbHarvestDemean, RejectsADegenerateVoteFloor) {
    LogManager log;
    const fs::path p = writeTempConfig("badfloor",
        networkConfig("    \"code_datum_time_linked\": true,\n"
                      "    \"carry_rx_code_bias\": true,\n"
                      "    \"rx_cb_harvest_demean\": true,\n"
                      "    \"rx_cb_carry_demean_min_stations\": 0,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

}  // namespace
