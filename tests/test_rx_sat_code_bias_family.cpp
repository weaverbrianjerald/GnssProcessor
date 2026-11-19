/**
 * @file test_rx_sat_code_bias_family.cpp
 * @brief The receiver-satellite code-bias state family in StateManager: its table row, birth, process
 *        noise, lifetime, reset to the prior, learned mark and run-start table line.
 *
 * The family is declared once in filter/StateFamily.hpp.  These cases pin the properties that
 * the row declares and that the generic family tests do not name: the states are never pruned,
 * never decoupled by a gap, never skipped for a frozen pivot and never serialized by the RT
 * snapshot, and one call returns the states of a satellite to the prior.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateFamily.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kPriorSigma = 0.15;   // filter.initial_std.rx_sat_code_bias, default
constexpr double kQ = 1.0e-10;         // filter.process_noise.rx_sat_code_bias_q, default
constexpr double kDt = 30.0;

struct Harness {
    LogManager   log_manager;
    GnssConfig   config;
    StateManager sm{ 5, log_manager, config };
};

/// A configuration loaded from @p json through the loader.
GnssConfig loadConfig(const std::string& json, const std::string& tag) {
    namespace fs = std::filesystem;
    const fs::path dir = gnss_test::tempDir() / ("rscb_family_" + tag);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "config.json";
    { std::ofstream out(file); out << json; }
    LogManager log;
    GnssConfig cfg;
    EXPECT_TRUE(cfg.loadFromJson(file.string(), log)) << tag;
    fs::remove_all(dir, ec);
    return cfg;
}

}  // namespace

TEST(RxSatCodeBiasFamilyTest, TheRowDeclaresTheFamilyThatTheCardDescribes) {
    const StateFamilySpec& s = specOf(StateFamily::RxSatCodeBias);
    EXPECT_EQ(static_cast<std::size_t>(StateFamily::RxSatCodeBias) + 1, kStateFamilyCount);
    EXPECT_STREQ(s.label_prefix, "RSCB:");
    EXPECT_EQ(s.snapshot_tag, nullptr);
    EXPECT_EQ(s.reinflation, kNoReinflation);
    EXPECT_FALSE(s.gap_decouple);
    EXPECT_EQ(s.sat_of_key, SatOfKey::None);       // no frozen-pivot Q skip
    EXPECT_EQ(s.q_rule, QRule::RandomWalk);
    EXPECT_STREQ(s.q_key, "rx_sat_code_bias_q");
    EXPECT_STREQ(s.initial_std_key, "rx_sat_code_bias");
    EXPECT_TRUE(s.never_pruned);
    EXPECT_FALSE(s.prune_cache);
    EXPECT_EQ(s.birth, BirthGate::RxSatCodeBias);
    // Every other family is pruned.
    for (const StateFamilySpec& other : kStateFamilies)
        if (other.family != StateFamily::RxSatCodeBias) EXPECT_FALSE(other.never_pruned) << other.name;
}

TEST(RxSatCodeBiasFamilyTest, ABornStateHasValueZeroAndThePriorVariance) {
    Harness h;
    const std::size_t core = h.sm.getStateSize();
    const std::size_t idx = h.sm.addRxSatCodeBiasState("G05|C1W");
    EXPECT_EQ(idx, core);
    EXPECT_EQ(h.sm.getRxSatCodeBiasIndex("G05|C1W"), static_cast<int>(core));
    EXPECT_EQ(h.sm.getRxSatCodeBiasIndex("G05|C2W"), -1);
    EXPECT_EQ(h.sm.getStateSize(), core + 1);
    EXPECT_EQ(h.sm.getStateVector()(static_cast<Eigen::Index>(idx)), 0.0);
    EXPECT_DOUBLE_EQ(h.sm.getStateVariance(static_cast<int>(idx)), kPriorSigma * kPriorSigma);
    EXPECT_EQ(h.sm.getStateLabels()[idx], "RSCB:G05|C1W");
    EXPECT_EQ(&h.sm.indicesOf(StateFamily::RxSatCodeBias), &h.sm.getRxSatCodeBiasStates());

    const auto& c = h.sm.rxSatCodeBiasCounters();
    EXPECT_EQ(c.born, 1);
    EXPECT_EQ(c.born_from_store, 0);
    // A second call for the same key is idempotent and counts nothing.
    EXPECT_EQ(h.sm.addRxSatCodeBiasState("G05|C1W"), idx);
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().born, 1);
    EXPECT_EQ(h.sm.getStateSize(), core + 1);
}

TEST(RxSatCodeBiasFamilyTest, AStateBornFromTheStoreTakesItsValueAndVarianceAndIsCounted) {
    Harness h;
    const std::size_t idx = h.sm.addRxSatCodeBiasStateFromStore("G05|C1W", 0.0421, 4.0e-6);
    EXPECT_DOUBLE_EQ(h.sm.getStateVector()(static_cast<Eigen::Index>(idx)), 0.0421);
    EXPECT_DOUBLE_EQ(h.sm.getStateVariance(static_cast<int>(idx)), 4.0e-6);
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().born, 1);
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().born_from_store, 1);
    // A non-positive variance falls back to the prior rather than creating a state that takes no data.
    const std::size_t idx2 = h.sm.addRxSatCodeBiasStateFromStore("G07|C1W", 0.01, 0.0);
    EXPECT_DOUBLE_EQ(h.sm.getStateVariance(static_cast<int>(idx2)), kPriorSigma * kPriorSigma);
}

TEST(RxSatCodeBiasFamilyTest, EveryStateTakesTheRandomWalkOnEveryPredictFrozenPivotIncluded) {
    Harness h;
    h.sm.addRxSatCodeBiasState("G05|C1W");
    h.sm.addRxSatCodeBiasState("G05|C2W");
    h.sm.addRxSatCodeBiasState("G17|C1W");   // a satellite that is not in any epoch's view
    h.sm.setFrozenPivotSatellites({ "G05" });
    const Eigen::MatrixXd Q = h.sm.getProcessNoiseMatrix(kDt);
    for (const auto& [key, idx] : h.sm.getRxSatCodeBiasStates())
        EXPECT_DOUBLE_EQ(Q(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx)), kQ * kDt) << key;
    // And the q the loader reads is the table's field.
    EXPECT_DOUBLE_EQ(h.config.getProcessNoise().*specOf(StateFamily::RxSatCodeBias).q_psd, kQ);
}

TEST(RxSatCodeBiasFamilyTest, NoPruneAndNoGapEditTouchesTheFamily) {
    Harness h;
    h.sm.addIonosphereState("G05", 0.0);
    h.sm.addAmbiguity("G05_L1C", 0.0);
    h.sm.addSatBiasState("G09|C1W", 0.0);
    h.sm.addRxSatCodeBiasState("G05|C1W");
    h.sm.addRxSatCodeBiasState("G17|C2W");
    Eigen::MatrixXd P = h.sm.getCovariance();
    const auto i = static_cast<Eigen::Index>(h.sm.getRxSatCodeBiasIndex("G05|C1W"));
    P(0, i) = 0.25;
    P(i, 0) = 0.25;
    h.sm.setCovariance(P);

    // Every prune of every other family, with an empty keep set (no satellite in view).
    h.sm.pruneAmbiguitiesExcept({});
    h.sm.pruneIonosphereStatesExcept({});
    h.sm.pruneSatBiasStatesExcept({});
    h.sm.pruneCodeBiasStatesExcept({});
    ASSERT_EQ(h.sm.getRxSatCodeBiasStates().size(), 2u);
    const auto j = static_cast<Eigen::Index>(h.sm.getRxSatCodeBiasIndex("G05|C1W"));
    EXPECT_EQ(h.sm.getStateLabels()[static_cast<std::size_t>(j)], "RSCB:G05|C1W");

    // The index map was shifted by the removals, and a gap leaves the cross-covariance with the position.
    h.sm.zeroPositionCrossCovariancesToSatStates();
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(0, j), 0.25);
}

TEST(RxSatCodeBiasFamilyTest, ResetReturnsEveryStateOfOneSatelliteToThePriorAndZeroesItsCrossCovariances) {
    Harness h;
    h.sm.addAmbiguity("G05_L1C", 0.0);
    h.sm.addRxSatCodeBiasState("G05|C1W");
    h.sm.addRxSatCodeBiasState("G05|C2W");
    h.sm.addRxSatCodeBiasState("G07|C1W");
    const auto idx = [&](const std::string& key) {
        return static_cast<Eigen::Index>(h.sm.getRxSatCodeBiasIndex(key));
    };
    const Eigen::Index n = static_cast<Eigen::Index>(h.sm.getStateSize());

    // A dense, positive-definite covariance and values off zero.
    Eigen::MatrixXd P = Eigen::MatrixXd::Constant(n, n, 0.002);
    for (Eigen::Index k = 0; k < n; ++k) P(k, k) = 0.04 + 0.001 * static_cast<double>(k);
    h.sm.setCovariance(P);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);
    x(idx("G05|C1W")) = 0.31;
    x(idx("G05|C2W")) = -0.22;
    x(idx("G07|C1W")) = 0.07;
    h.sm.setStateVector(x);
    ASSERT_TRUE(h.sm.markRxSatCodeBiasLearned("G05|C1W"));
    ASSERT_TRUE(h.sm.markRxSatCodeBiasLearned("G07|C1W"));

    EXPECT_EQ(h.sm.resetRxSatCodeBiasStatesToPrior("G05"), 2u);

    const Eigen::VectorXd xr = h.sm.getStateVector();
    const Eigen::MatrixXd Pr = h.sm.getCovariance();
    for (const char* key : { "G05|C1W", "G05|C2W" }) {
        const Eigen::Index i = idx(key);
        EXPECT_EQ(xr(i), 0.0) << key;
        EXPECT_DOUBLE_EQ(Pr(i, i), kPriorSigma * kPriorSigma) << key;
        for (Eigen::Index k = 0; k < n; ++k)
            if (k != i) {
                EXPECT_EQ(Pr(i, k), 0.0) << key << " row " << k;
                EXPECT_EQ(Pr(k, i), 0.0) << key << " col " << k;
            }
        EXPECT_FALSE(h.sm.isRxSatCodeBiasLearned(key)) << key;
    }
    // The other satellite and every other state are untouched.
    EXPECT_EQ(xr(idx("G07|C1W")), 0.07);
    EXPECT_EQ(Pr(idx("G07|C1W"), idx("G07|C1W")), P(idx("G07|C1W"), idx("G07|C1W")));
    const Eigen::Index amb = static_cast<Eigen::Index>(h.sm.getAmbiguityIndex("G05_L1C"));
    EXPECT_EQ(Pr(amb, idx("G07|C1W")), 0.002);
    EXPECT_TRUE(h.sm.isRxSatCodeBiasLearned("G07|C1W"));

    const auto& c = h.sm.rxSatCodeBiasCounters();
    EXPECT_EQ(c.reset, 2);
    EXPECT_EQ(c.reset_satellites, 1);
    // A satellite without states resets nothing and counts nothing.
    EXPECT_EQ(h.sm.resetRxSatCodeBiasStatesToPrior("G99"), 0u);
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().reset, 2);
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().reset_satellites, 1);
}

TEST(RxSatCodeBiasFamilyTest, TheLearnedMarkIsCountedOnceAndClearedByAReset) {
    Harness h;
    h.sm.addRxSatCodeBiasState("G05|C1W");
    EXPECT_FALSE(h.sm.markRxSatCodeBiasLearned("G99|C1W"));   // no such state
    EXPECT_TRUE(h.sm.markRxSatCodeBiasLearned("G05|C1W"));
    EXPECT_FALSE(h.sm.markRxSatCodeBiasLearned("G05|C1W"));   // already marked
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().learned, 1);
    EXPECT_TRUE(h.sm.isRxSatCodeBiasLearned("G05|C1W"));
    h.sm.resetRxSatCodeBiasStatesToPrior("G05");
    EXPECT_FALSE(h.sm.isRxSatCodeBiasLearned("G05|C1W"));
    EXPECT_TRUE(h.sm.markRxSatCodeBiasLearned("G05|C1W"));    // learned again after the reset
    EXPECT_EQ(h.sm.rxSatCodeBiasCounters().learned, 2);
}

TEST(RxSatCodeBiasFamilyTest, TheRtSnapshotDoesNotSerializeTheFamily) {
    Harness h;
    const std::size_t core = h.sm.getStateSize();
    EXPECT_FALSE(h.sm.restoreDynamicState(StateFamily::RxSatCodeBias, "G05|C1W", 0.0, core));
    EXPECT_EQ(h.sm.getStateSize(), core);
}

TEST(RxSatCodeBiasFamilyTest, TheRunStartTableShowsTheRowAsNotBornUntilTheKeyIsOnAndUncombined) {
    // Default configuration: the row is listed and not born.
    Harness h;
    const auto lines = h.sm.familiesAsArmed();
    ASSERT_EQ(lines.size(), 1 + kStateFamilyCount);
    const std::string& off = lines[1 + static_cast<std::size_t>(StateFamily::RxSatCodeBias)];
    EXPECT_NE(off.find("receiver-satellite code bias:"), std::string::npos) << off;
    EXPECT_NE(off.find("born no"), std::string::npos) << off;
    EXPECT_NE(off.find("label RSCB:"), std::string::npos) << off;
    EXPECT_NE(off.find("snapshot none"), std::string::npos) << off;
    EXPECT_NE(off.find("never pruned"), std::string::npos) << off;
    EXPECT_EQ(off.find("gap decouple"), std::string::npos) << off;
    EXPECT_EQ(off.find("frozen-pivot Q skip"), std::string::npos) << off;
    EXPECT_NE(off.find("filter.initial_std.rx_sat_code_bias = 0.15"), std::string::npos) << off;
    EXPECT_NE(off.find("random walk, filter.process_noise.rx_sat_code_bias_q = 1e-10"), std::string::npos) << off;

    // Key on, uncombined PPP: born.
    LogManager lm;
    const GnssConfig on = loadConfig(
        R"({"processing": {"mode": "PPP", "uncombined_enabled": true},
            "estimation": {"estimate_rx_sat_code_bias": true}})", "on");
    ASSERT_TRUE(on.estimateRxSatCodeBias());
    StateManager sm_on{ 5, lm, on };
    const auto on_lines = sm_on.familiesAsArmed();
    const std::string& line = on_lines[1 + static_cast<std::size_t>(StateFamily::RxSatCodeBias)];
    EXPECT_NE(line.find("born yes"), std::string::npos) << line;
}
