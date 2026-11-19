/**
 * @file test_rx_sat_code_bias_learning.cpp
 * @brief When a receiver-satellite code bias counts as learned, and the store a run leaves
 *        (filter/RxSatCodeBiasLearning.hpp).
 *
 * The quadratic form v' P v of a state against the mean of a set is checked against the vector
 * written out; the learned test on hand-made covariances (a pair with a free common part is
 * learned, an untouched prior state is not); the store builder on a small run (zero-mean values,
 * sigma, the carry-over of entries the run never saw); and the per-epoch event on a state manager.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/RxSatCodeBiasLearning.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kPrior = 0.15;   // the default filter.initial_std.rx_sat_code_bias

/// A symmetric positive-definite matrix that is not diagonal, deterministic.
Eigen::MatrixXd someCovariance(int n) {
    Eigen::MatrixXd A(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) A(i, j) = std::sin(1.3 * i + 0.7 * j + 0.1) + (i == j ? 1.5 : 0.0);
    return A * A.transpose() + 0.05 * Eigen::MatrixXd::Identity(n, n);
}

/// P = common * 11' + diag(d), on n states: a free common part that every state shares.
Eigen::MatrixXd withFreeCommonPart(int n, double common, double d) {
    return Eigen::MatrixXd::Constant(n, n, common) + d * Eigen::MatrixXd::Identity(n, n);
}

}  // namespace

TEST(RxSatCodeBiasLearningTest, TheDeviationVarianceIsTheQuadraticFormOfTheVectorItNames) {
    const Eigen::MatrixXd P = someCovariance(7);
    const std::vector<Eigen::Index> set = { 1, 2, 4, 5 };
    for (const Eigen::Index i : { 0, 1, 4, 6 }) {   // in the set and outside it
        Eigen::VectorXd v = Eigen::VectorXd::Zero(7);
        v(i) += 1.0;
        for (const Eigen::Index k : set) v(k) -= 1.0 / static_cast<double>(set.size());
        EXPECT_NEAR(deviationVariance(P, i, set), v.dot(P * v), 1e-12) << "state " << i;
    }
    // An empty set subtracts nothing; the set of the state alone leaves nothing.
    EXPECT_DOUBLE_EQ(deviationVariance(P, 3, {}), P(3, 3));
    EXPECT_NEAR(deviationVariance(P, 3, { 3 }), 0.0, 1e-14);
}

TEST(RxSatCodeBiasLearningTest, ACommonShiftOfEveryStateDoesNotChangeTheDeviation) {
    // The free common part of a group is the receiver code bias: it drops out of the deviation.
    const std::vector<Eigen::Index> set = { 0, 1, 2 };
    const double d = 0.002;
    for (const double common : { 0.0, 1.0, 400.0, 1.0e6 })
        EXPECT_NEAR(deviationVariance(withFreeCommonPart(3, common, d), 0, set), d * (1.0 - 1.0 / 3.0), 1e-6)
            << "common " << common;
}

TEST(RxSatCodeBiasLearningTest, ThePairWithAFreeCommonPartIsLearnedAndAnUntouchedPairIsNot) {
    // Two states whose sum is free (variance 1) and whose difference is known to 1 mm: learned.
    Eigen::MatrixXd tight = Eigen::MatrixXd::Constant(2, 2, 1.0);
    tight(0, 0) = tight(1, 1) = 1.0 + 1.0e-6;
    const std::vector<Eigen::Index> pair = { 0, 1 };
    const double v_tight = deviationVariance(tight, 0, pair);
    EXPECT_NEAR(v_tight, 0.5e-6, 1e-12);          // var((x0 - x1) / 2) = 2e-6 / 4
    EXPECT_TRUE(isLearnedDeviation(v_tight, kPrior));
    EXPECT_LT(std::sqrt(v_tight), 0.01 * kPrior);

    // The same two states at their prior, independent: the deviation has half the prior variance.
    const Eigen::MatrixXd prior = (kPrior * kPrior) * Eigen::MatrixXd::Identity(2, 2);
    const double v_prior = deviationVariance(prior, 0, pair);
    EXPECT_NEAR(v_prior, 0.5 * kPrior * kPrior, 1e-15);
    EXPECT_FALSE(isLearnedDeviation(v_prior, kPrior));

    // The bound is (0.5 * sigma)^2, inclusive.
    EXPECT_TRUE(isLearnedDeviation(0.25 * kPrior * kPrior, kPrior));
    EXPECT_FALSE(isLearnedDeviation(0.25 * kPrior * kPrior * (1.0 + 1e-9), kPrior));
}

TEST(RxSatCodeBiasLearningTest, TheGroupsAreOneConstellationAndOneCodeSignal) {
    const std::map<std::string, std::size_t> states = {
        { "G05|C1W", 5 }, { "G07|C1W", 6 }, { "G05|C2W", 7 }, { "E11|C1C", 8 }, { "E12|C1C", 9 },
        { "garbage", 10 } };
    const auto groups = rxSatCodeBiasGroups(states);
    ASSERT_EQ(groups.size(), 3u);
    EXPECT_EQ(groups.at("G|C1W").size(), 2u);
    EXPECT_EQ(groups.at("G|C2W").size(), 1u);
    EXPECT_EQ(groups.at("E|C1C").size(), 2u);
    EXPECT_EQ(groups.at("G|C1W")[0].first, "G05|C1W");
    EXPECT_EQ(groups.at("G|C1W")[0].second, 5);
}

TEST(RxSatCodeBiasLearningTest, OnlyAGroupOfTwoOrMoreCanHaveLearnedStates) {
    const std::map<std::string, std::size_t> states = {
        { "G05|C1W", 0 }, { "G07|C1W", 1 }, { "G09|C1W", 2 }, { "G05|C2W", 3 } };
    Eigen::MatrixXd P = Eigen::MatrixXd::Zero(4, 4);
    P.topLeftCorner(3, 3) = withFreeCommonPart(3, 100.0, 1.0e-3);   // the C1W group: all learned
    P(3, 3) = 1.0e-8;                                               // the lone C2W state is tight, alone
    const std::set<std::string> learned = learnedKeys(P, states, kPrior);
    EXPECT_EQ(learned, (std::set<std::string>{ "G05|C1W", "G07|C1W", "G09|C1W" }));
}

TEST(RxSatCodeBiasLearningTest, AnUntouchedStateAmongLearnedOnesSpoilsTheGroupMean) {
    // Three states share a free common part; the fourth, at its prior, does not: against the mean
    // of the four, none passes.  The test is on the group, as the data can only know the group.
    const std::map<std::string, std::size_t> states = {
        { "G01|C1W", 0 }, { "G02|C1W", 1 }, { "G03|C1W", 2 }, { "G04|C1W", 3 } };
    Eigen::MatrixXd P = Eigen::MatrixXd::Zero(4, 4);
    P.topLeftCorner(3, 3) = withFreeCommonPart(3, 100.0, 1.0e-3);
    P(3, 3) = kPrior * kPrior;
    EXPECT_TRUE(learnedKeys(P, states, kPrior).empty());
}

TEST(RxSatCodeBiasLearningTest, TheStoreHoldsTheLearnedStatesAtZeroMeanWithTheirSigma) {
    // Three GPS C1W states with a free common part (the receiver code bias, offset 5 m), one lone
    // GPS C2W state, and an Galileo group of two states left at the prior.
    const std::map<std::string, std::size_t> states = {
        { "G01|C1W", 0 }, { "G02|C1W", 1 }, { "G03|C1W", 2 }, { "G01|C2W", 3 },
        { "E11|C1C", 4 }, { "E12|C1C", 5 } };
    Eigen::MatrixXd P = Eigen::MatrixXd::Zero(6, 6);
    P.topLeftCorner(3, 3) = withFreeCommonPart(3, 25.0, 4.0e-6);
    P(3, 3) = 1.0e-8;
    P(4, 4) = P(5, 5) = kPrior * kPrior;
    Eigen::VectorXd x = Eigen::VectorXd::Zero(6);
    x << 5.30, 5.10, 5.20, 0.4, 0.05, -0.03;
    const GnssTime end{ 2418, 85000.0 };
    const RxSatCodeBiasIdentity identity{ "TRIMBLE ALLOY", "5.56", "COD0MGXFIN" };

    RxSatCodeBiasBuildCounts counts;
    const RxSatCodeBiasStore s = buildRxSatCodeBiasStore(x, P, states, kPrior, nullptr, identity, end, &counts);

    EXPECT_EQ(s.identity.receiver_type, "TRIMBLE ALLOY");
    EXPECT_EQ(counts.written, 3u);
    EXPECT_EQ(counts.carried_over, 0u);
    ASSERT_EQ(s.entries.size(), 3u);   // the lone state and the untouched pair are not written
    double sum = 0.0;
    for (const char* key : { "G01|C1W", "G02|C1W", "G03|C1W" }) {
        ASSERT_TRUE(s.entries.count(key)) << key;
        const RxSatCodeBiasEntry& e = s.entries.at(key);
        sum += e.value_m;
        EXPECT_EQ(e.updated.week, 2418);
        EXPECT_EQ(e.updated.tow, 85000.0);
        // Sigma: the deviation from the mean of the learned set, floor 1 mm.
        EXPECT_NEAR(e.sigma_m, std::sqrt(4.0e-6 * (1.0 - 1.0 / 3.0)), 1e-9) << key;
    }
    // Values: the state minus the mean of the learned states; the common 5 m is gone.
    EXPECT_NEAR(s.entries.at("G01|C1W").value_m, 0.10, 1e-12);
    EXPECT_NEAR(s.entries.at("G02|C1W").value_m, -0.10, 1e-12);
    EXPECT_NEAR(s.entries.at("G03|C1W").value_m, 0.0, 1e-12);
    EXPECT_NEAR(sum, 0.0, 1e-12);
    EXPECT_EQ(s.entries.at("G01|C1W").satellite, "G01");
    EXPECT_EQ(s.entries.at("G01|C1W").signal, "C1W");
}

TEST(RxSatCodeBiasLearningTest, ASigmaBelowTheFloorIsWrittenAtTheFloor) {
    const std::map<std::string, std::size_t> states = { { "G01|C1W", 0 }, { "G02|C1W", 1 } };
    Eigen::MatrixXd P = Eigen::MatrixXd::Constant(2, 2, 1.0);
    P(0, 0) = P(1, 1) = 1.0 + 1.0e-10;   // difference known to a micrometre
    const Eigen::VectorXd x = (Eigen::VectorXd(2) << 0.2, 0.1).finished();
    const RxSatCodeBiasStore s =
        buildRxSatCodeBiasStore(x, P, states, kPrior, nullptr, RxSatCodeBiasIdentity{}, GnssTime{ 1, 0.0 });
    ASSERT_EQ(s.entries.size(), 2u);
    for (const auto& [key, e] : s.entries) EXPECT_EQ(e.sigma_m, kRxSatCodeBiasMinStoredSigmaM) << key;
}

TEST(RxSatCodeBiasLearningTest, EntriesTheRunNeverSawAreCarriedOverAndTheOthersAreRewritten) {
    const std::map<std::string, std::size_t> states = {
        { "G01|C1W", 0 }, { "G02|C1W", 1 },     // learned: rewritten
        { "G05|C2W", 2 }, { "G06|C2W", 3 } };   // seen, at the prior: not learned, dropped
    Eigen::MatrixXd P = Eigen::MatrixXd::Zero(4, 4);
    P.topLeftCorner(2, 2) = withFreeCommonPart(2, 9.0, 1.0e-5);
    P(2, 2) = P(3, 3) = kPrior * kPrior;
    const Eigen::VectorXd x = (Eigen::VectorXd(4) << 1.07, 1.01, 0.3, -0.2).finished();

    RxSatCodeBiasStore previous;
    previous.identity = RxSatCodeBiasIdentity{ "TRIMBLE ALLOY", "5.56", "COD0MGXFIN" };
    const GnssTime old_time{ 2400, 1000.0 };
    previous.entries["G01|C1W"] = RxSatCodeBiasEntry{ "G01", "C1W", 0.9, 0.2, old_time };      // seen, rewritten
    previous.entries["G05|C2W"] = RxSatCodeBiasEntry{ "G05", "C2W", 0.4, 0.1, old_time };      // seen, dropped
    previous.entries["G30|C1W"] = RxSatCodeBiasEntry{ "G30", "C1W", -0.0321, 0.0044, old_time }; // never seen

    RxSatCodeBiasBuildCounts counts;
    const RxSatCodeBiasIdentity identity{ "TRIMBLE ALLOY", "5.56", "COD0MGXFIN" };
    const RxSatCodeBiasStore s =
        buildRxSatCodeBiasStore(x, P, states, kPrior, &previous, identity, GnssTime{ 2418, 5.0 }, &counts);

    EXPECT_EQ(counts.written, 2u);
    EXPECT_EQ(counts.carried_over, 1u);
    ASSERT_EQ(s.entries.size(), 3u);
    // The unseen entry keeps its sigma and its epoch; its value moves only by the group's one shift.
    const RxSatCodeBiasEntry& kept = s.entries.at("G30|C1W");
    EXPECT_EQ(kept.sigma_m, 0.0044);
    EXPECT_EQ(kept.updated.week, 2400);
    EXPECT_EQ(kept.updated.tow, 1000.0);
    // The seen and learned one has this run's epoch, and every distance to the carried-over entry
    // is the one the values had before the re-centring.
    EXPECT_EQ(s.entries.at("G01|C1W").updated.week, 2418);
    EXPECT_NEAR(s.entries.at("G01|C1W").value_m - kept.value_m, 1.07 - (-0.0321), 1e-12);
    EXPECT_NEAR(s.entries.at("G02|C1W").value_m - kept.value_m, 1.01 - (-0.0321), 1e-12);
    EXPECT_NEAR(s.entries.at("G01|C1W").value_m - s.entries.at("G02|C1W").value_m, 0.06, 1e-12);
    // One shift for the whole group: the file holds zero mean over the written and the carried entries.
    EXPECT_NEAR(s.entries.at("G01|C1W").value_m + s.entries.at("G02|C1W").value_m + kept.value_m, 0.0, 1e-12);
    // The seen one that is not learned is gone: the run found its old value unproven.
    EXPECT_EQ(s.entries.count("G05|C2W"), 0u);
}

TEST(RxSatCodeBiasLearningTest, AGroupWithNoWrittenEntryKeepsItsCarriedValuesAsTheyAre) {
    const std::map<std::string, std::size_t> states = {
        { "G01|C1W", 0 }, { "G02|C1W", 1 } };
    Eigen::MatrixXd P = withFreeCommonPart(2, 9.0, 1.0e-5);
    const Eigen::VectorXd x = (Eigen::VectorXd(2) << 1.07, 1.01).finished();
    RxSatCodeBiasStore previous;
    previous.entries["E11|C1C"] = RxSatCodeBiasEntry{ "E11", "C1C", 0.0123, 0.003, GnssTime{ 1, 1.0 } };
    previous.entries["E12|C1C"] = RxSatCodeBiasEntry{ "E12", "C1C", 0.0300, 0.003, GnssTime{ 1, 1.0 } };
    const RxSatCodeBiasStore s = buildRxSatCodeBiasStore(
        x, P, states, kPrior, &previous, RxSatCodeBiasIdentity{}, GnssTime{ 2, 2.0 });
    EXPECT_EQ(s.entries.at("E11|C1C").value_m, 0.0123);
    EXPECT_EQ(s.entries.at("E12|C1C").value_m, 0.0300);
    EXPECT_NEAR(s.entries.at("G01|C1W").value_m + s.entries.at("G02|C1W").value_m, 0.0, 1e-12);
}

TEST(RxSatCodeBiasLearningTest, AnUntouchedRunWritesNothingButWhatItCarries) {
    const std::map<std::string, std::size_t> states = { { "G01|C1W", 0 }, { "G02|C1W", 1 } };
    const Eigen::MatrixXd P = (kPrior * kPrior) * Eigen::MatrixXd::Identity(2, 2);
    const Eigen::VectorXd x = Eigen::VectorXd::Zero(2);
    RxSatCodeBiasStore previous;
    previous.entries["G30|C1W"] = RxSatCodeBiasEntry{ "G30", "C1W", 0.01, 0.002, GnssTime{ 1, 1.0 } };
    RxSatCodeBiasBuildCounts counts;
    const RxSatCodeBiasStore s = buildRxSatCodeBiasStore(
        x, P, states, kPrior, &previous, RxSatCodeBiasIdentity{}, GnssTime{ 2, 2.0 }, &counts);
    EXPECT_EQ(counts.written, 0u);
    EXPECT_EQ(counts.carried_over, 1u);
    EXPECT_EQ(s.entries.size(), 1u);
}

TEST(RxSatCodeBiasLearningTest, TheEventIsCountedOnTheFirstEpochTheTestPassesAndNotAgain) {
    LogManager lm;
    GnssConfig cfg;
    StateManager sm{ 5, lm, cfg };
    sm.addRxSatCodeBiasState("G01|C1W");
    sm.addRxSatCodeBiasState("G02|C1W");
    sm.addRxSatCodeBiasState("G09|C2W");   // alone in its group

    // At the prior nothing is learned.
    EXPECT_EQ(noteLearnedRxSatCodeBiases(sm, kPrior), 0u);
    EXPECT_EQ(sm.rxSatCodeBiasCounters().learned, 0);

    // The data tighten the difference of the pair and leave their sum free.
    const Eigen::Index i1 = sm.getRxSatCodeBiasIndex("G01|C1W");
    const Eigen::Index i2 = sm.getRxSatCodeBiasIndex("G02|C1W");
    Eigen::MatrixXd P = sm.getCovariance();
    P(i1, i1) = P(i2, i2) = 4.0;
    P(i1, i2) = P(i2, i1) = 4.0 - 1.0e-6;
    sm.setCovariance(P);
    EXPECT_EQ(noteLearnedRxSatCodeBiases(sm, kPrior), 2u);
    EXPECT_TRUE(sm.isRxSatCodeBiasLearned("G01|C1W"));
    EXPECT_TRUE(sm.isRxSatCodeBiasLearned("G02|C1W"));
    EXPECT_FALSE(sm.isRxSatCodeBiasLearned("G09|C2W"));
    EXPECT_EQ(sm.rxSatCodeBiasCounters().learned, 2);

    // The next epoch: still learned, no new event.
    EXPECT_EQ(noteLearnedRxSatCodeBiases(sm, kPrior), 0u);
    EXPECT_EQ(sm.rxSatCodeBiasCounters().learned, 2);

    // A reset of one satellite takes it back to the prior, so it is not learned until the data return.
    sm.resetRxSatCodeBiasStatesToPrior("G01");
    EXPECT_EQ(noteLearnedRxSatCodeBiases(sm, kPrior), 0u);
    EXPECT_FALSE(sm.isRxSatCodeBiasLearned("G01|C1W"));
    EXPECT_EQ(sm.rxSatCodeBiasCounters().reset, 1);
}

TEST(RxSatCodeBiasLearningTest, TheRunEndLineNamesEveryCounter) {
    StateManager::RxSatCodeBiasCounters c;
    c.born = 64;
    c.born_from_store = 10;
    c.learned = 40;
    c.reset = 3;
    c.reset_satellites = 2;
    c.written = 38;
    c.carried_over = 5;
    c.store_ignored = 1;
    EXPECT_EQ(rxSatCodeBiasRunEndLine(c, 64, 41),
              "receiver-satellite code bias: states=64 learned_now=41 | born=64 (from the store 10) "
              "learned=40 reset=3 (satellites 2) written=38 carried_over=5 store_ignored=1");
}
