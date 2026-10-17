/**
 * @file test_reinflate_never_shrinks.cpp
 * @brief Every StateManager variance re-inflation raises the variance and never shrinks
 *        it.
 *
 * The seven re-inflations (receiver code bias, inter-system bias, slant ionosphere and its
 * rate, satellite code bias, satellite phase bias, satellite clock) set the diagonal to
 * the maximum of the current and the requested variance.  Setting it to the request
 * alone would deflate a larger variance, adding information the data never supplied.
 * The row and column are zeroed (the partition is broken), the value is untouched, and
 * each re-inflation has a raised / kept counter.  Also tested: the inter-system biases a
 * clock re-reference re-inflates (a reference step takes every allocated one, observed
 * that epoch or not).
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "realtime/RealTimeEngine.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr size_t kCore = 5;   // PPP core: position (3), receiver clock, tropo
using Kind = StateManager::Reinflation;

/// Default config (PPP, six active systems: five inter-system bias states).
struct Harness {
    LogManager   log_manager;
    GnssConfig   config;
    StateManager sm{ kCore, log_manager, config };
};

/// Plant a diagonal at @p idx and a cross term with @p other.
void plant(StateManager& sm, Eigen::Index idx, double diag, Eigen::Index other, double cross,
           double value) {
    Eigen::VectorXd x = sm.getStateVector();
    Eigen::MatrixXd P = sm.getCovariance();
    x(idx)        = value;
    P(idx, idx)   = diag;
    P(idx, other) = cross;
    P(other, idx) = cross;
    sm.setStateVector(x);
    sm.setCovariance(P);
}

const StateManager::ReinflateCounter& counter(const StateManager& sm, Kind k) {
    return sm.reinflateCounters()[static_cast<std::size_t>(k)];
}

/// The partition is broken and the value untouched, whatever the diagonal became.
void expectDecoupled(const StateManager& sm, Eigen::Index idx, Eigen::Index other,
                     double value) {
    const Eigen::MatrixXd& P = sm.getCovariance();
    EXPECT_DOUBLE_EQ(P(idx, other), 0.0);
    EXPECT_DOUBLE_EQ(P(other, idx), 0.0);
    EXPECT_DOUBLE_EQ(sm.getStateVector()(idx), value);
}

}  // namespace

// Receiver code bias: a larger current variance is kept, a smaller one is raised to the
// request; each is counted once.
TEST(ReinflateNeverShrinks, CodeBiasKeepsALargerAndRaisesASmallerVariance) {
    Harness h;
    const auto a = static_cast<Eigen::Index>(h.sm.addCodeBiasState("GPS_DCB", 0.0));
    const auto b = static_cast<Eigen::Index>(h.sm.addCodeBiasState("GPS_IFB5", 0.0));

    plant(h.sm, a, 4.0, b, 1.5, 0.42);
    h.sm.reinflateCodeBiasVariance("GPS_DCB", 0.25);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(a, a), 4.0);   // kept
    expectDecoupled(h.sm, a, b, 0.42);
    EXPECT_EQ(counter(h.sm, Kind::CodeBias).kept, 1);
    EXPECT_EQ(counter(h.sm, Kind::CodeBias).raised, 0);

    plant(h.sm, a, 0.04, b, 0.01, 0.42);
    h.sm.reinflateCodeBiasVariance("GPS_DCB", 0.25);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(a, a), 0.25);  // raised
    expectDecoupled(h.sm, a, b, 0.42);
    EXPECT_EQ(counter(h.sm, Kind::CodeBias).kept, 1);
    EXPECT_EQ(counter(h.sm, Kind::CodeBias).raised, 1);
}

// A frozen (zero-diagonal) row is refused as before and counted in neither column.
TEST(ReinflateNeverShrinks, FrozenRowIsRefusedAndNotCounted) {
    Harness h;
    const auto a = static_cast<Eigen::Index>(h.sm.addCodeBiasState("GPS_DCB", 0.0));
    Eigen::MatrixXd P = h.sm.getCovariance();
    P(a, a) = 0.0;
    h.sm.setCovariance(P);
    h.sm.reinflateCodeBiasVariance("GPS_DCB", 0.25);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(a, a), 0.0);
    EXPECT_EQ(counter(h.sm, Kind::CodeBias).kept, 0);
    EXPECT_EQ(counter(h.sm, Kind::CodeBias).raised, 0);
}

// Inter-system bias: kept and raised; the non-positive request falls back to the initial
// variance, and the maximum rule applies to the fallback too.
TEST(ReinflateNeverShrinks, SystemTimeOffsetKeepsRaisesAndFallsBackUnderTheMaxRule) {
    Harness h;
    const auto gi = static_cast<Eigen::Index>(h.sm.getSystemTimeOffsetIndex("Galileo"));
    const auto ri = static_cast<Eigen::Index>(h.sm.getSystemTimeOffsetIndex("GLONASS"));

    plant(h.sm, gi, 4.0, ri, 1.5, 1.7);
    h.sm.reinflateSystemTimeOffsetVariance("Galileo", 0.25);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(gi, gi), 4.0);
    expectDecoupled(h.sm, gi, ri, 1.7);

    const double s0 = h.config.getInitialStd().system_time_offset;   // 100 m by default
    plant(h.sm, gi, 4.0, ri, 1.5, 1.7);
    h.sm.reinflateSystemTimeOffsetVariance("Galileo", 0.0);          // fallback s0^2
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(gi, gi), s0 * s0);

    plant(h.sm, gi, 2.0 * s0 * s0, ri, 1.5, 1.7);
    h.sm.reinflateSystemTimeOffsetVariance("Galileo", 0.0);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(gi, gi), 2.0 * s0 * s0);   // kept over the fallback

    EXPECT_EQ(counter(h.sm, Kind::SystemTimeOffset).kept, 2);
    EXPECT_EQ(counter(h.sm, Kind::SystemTimeOffset).raised, 1);
}

// Slant ionosphere: kept and raised.
TEST(ReinflateNeverShrinks, IonosphereKeepsAndRaises) {
    Harness h;
    const auto io = static_cast<Eigen::Index>(h.sm.addIonosphereState("G05", 0.3));
    const auto cb = static_cast<Eigen::Index>(h.sm.addCodeBiasState("GPS_DCB", 0.0));

    plant(h.sm, io, 30.0, cb, 2.0, 0.3);
    h.sm.reinflateIonosphereVariance("G05", 25.0);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(io, io), 30.0);
    expectDecoupled(h.sm, io, cb, 0.3);

    plant(h.sm, io, 0.01, cb, 0.001, 0.3);
    h.sm.reinflateIonosphereVariance("G05", 25.0);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(io, io), 25.0);

    EXPECT_EQ(counter(h.sm, Kind::Ionosphere).kept, 1);
    EXPECT_EQ(counter(h.sm, Kind::Ionosphere).raised, 1);
}

// Slant ionosphere rate (present only with estimation.enable_iono_rate): kept and raised.
TEST(ReinflateNeverShrinks, IonosphereRateKeepsAndRaises) {
    const fs::path p = gnss_test::tempDir() / "owd_reinflate_iono_rate.json";
    { std::ofstream f(p); f << "{ \"estimation\": { \"enable_iono_rate\": true } }\n"; }
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_TRUE(cfg.enableIonoRate());
    StateManager sm{ kCore, log, cfg };
    sm.addIonosphereState("G05", 0.3);
    const int r = sm.getIonosphereRateIndex("G05");
    ASSERT_GE(r, 0);
    const auto ri = static_cast<Eigen::Index>(r);
    const auto io = static_cast<Eigen::Index>(sm.getIonosphereIndex("G05"));

    plant(sm, ri, 9.0, io, 0.5, 0.001);
    sm.reinflateIonosphereRateVariance("G05", 1e-4);
    EXPECT_DOUBLE_EQ(sm.getCovariance()(ri, ri), 9.0);
    expectDecoupled(sm, ri, io, 0.001);

    plant(sm, ri, 1e-8, io, 1e-9, 0.001);
    sm.reinflateIonosphereRateVariance("G05", 1e-4);
    EXPECT_DOUBLE_EQ(sm.getCovariance()(ri, ri), 1e-4);

    EXPECT_EQ(sm.reinflateCounters()[static_cast<std::size_t>(Kind::IonosphereRate)].kept, 1);
    EXPECT_EQ(sm.reinflateCounters()[static_cast<std::size_t>(Kind::IonosphereRate)].raised, 1);
    std::error_code ec;
    fs::remove(p, ec);
}

// Satellite code bias, per state of the satellite: one state kept, the other raised.
TEST(ReinflateNeverShrinks, SatBiasPerStateKeepsAndRaises) {
    Harness h;
    const auto c1 = static_cast<Eigen::Index>(
        h.sm.addSatBiasState(StateKeys::makeSatBiasKey("G05", "C1C"), 0.2));
    const auto c2 = static_cast<Eigen::Index>(
        h.sm.addSatBiasState(StateKeys::makeSatBiasKey("G05", "C2W"), -0.1));
    plant(h.sm, c1, 40.0, c2, 3.0, 0.2);
    plant(h.sm, c2, 1.0, c1, 3.0, -0.1);
    h.sm.reinflateSatBiasVariancesForSat("G05", 25.0);
    const Eigen::MatrixXd& P = h.sm.getCovariance();
    EXPECT_DOUBLE_EQ(P(c1, c1), 40.0);
    EXPECT_DOUBLE_EQ(P(c2, c2), 25.0);
    EXPECT_DOUBLE_EQ(P(c1, c2), 0.0);
    EXPECT_DOUBLE_EQ(h.sm.getStateVector()(c1), 0.2);
    EXPECT_DOUBLE_EQ(h.sm.getStateVector()(c2), -0.1);
    EXPECT_EQ(counter(h.sm, Kind::SatBias).kept, 1);
    EXPECT_EQ(counter(h.sm, Kind::SatBias).raised, 1);
}

// Satellite phase bias, per state: one kept, one raised.
TEST(ReinflateNeverShrinks, SatPhaseBiasPerStateKeepsAndRaises) {
    Harness h;
    const auto l1 = static_cast<Eigen::Index>(
        h.sm.addSatPhaseBiasState(StateKeys::makeSatPhaseBiasKey("G05", "L1C"), 0.05));
    const auto l2 = static_cast<Eigen::Index>(
        h.sm.addSatPhaseBiasState(StateKeys::makeSatPhaseBiasKey("G05", "L2W"), -0.05));
    plant(h.sm, l1, 50.0, l2, 2.0, 0.05);
    plant(h.sm, l2, 0.5, l1, 2.0, -0.05);
    h.sm.reinflateSatPhaseBiasVariancesForSat("G05", 4.0);
    const Eigen::MatrixXd& P = h.sm.getCovariance();
    EXPECT_DOUBLE_EQ(P(l1, l1), 50.0);
    EXPECT_DOUBLE_EQ(P(l2, l2), 4.0);
    EXPECT_DOUBLE_EQ(P(l1, l2), 0.0);
    EXPECT_EQ(counter(h.sm, Kind::SatPhaseBias).kept, 1);
    EXPECT_EQ(counter(h.sm, Kind::SatPhaseBias).raised, 1);
}

// Satellite clock: kept and raised.
TEST(ReinflateNeverShrinks, SatClockKeepsAndRaises) {
    Harness h;
    const auto sc = static_cast<Eigen::Index>(
        h.sm.addSatClockState(StateKeys::makeSatClockKey("G05"), 1.0));
    const auto cb = static_cast<Eigen::Index>(h.sm.addCodeBiasState("GPS_DCB", 0.0));
    plant(h.sm, sc, 1e6, cb, 5.0, 1.0);
    h.sm.reinflateSatClockVarianceForSat("G05", 100.0);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(sc, sc), 1e6);
    expectDecoupled(h.sm, sc, cb, 1.0);
    plant(h.sm, sc, 1.0, cb, 0.5, 1.0);
    h.sm.reinflateSatClockVarianceForSat("G05", 100.0);
    EXPECT_DOUBLE_EQ(h.sm.getCovariance()(sc, sc), 100.0);
    EXPECT_EQ(counter(h.sm, Kind::SatClock).kept, 1);
    EXPECT_EQ(counter(h.sm, Kind::SatClock).raised, 1);
}

// The run-end summary names the seven re-inflations in order with both counts.
TEST(ReinflateNeverShrinks, SummaryNamesEveryReinflation) {
    StateManager::ReinflateCounters c{};
    c[static_cast<std::size_t>(Kind::CodeBias)] = {3, 2};
    c[static_cast<std::size_t>(Kind::SatClock)] = {1, 0};
    EXPECT_EQ(StateManager::reinflateSummary(c),
              "code_bias raised=3 kept=2 | isb raised=0 kept=0 | iono raised=0 kept=0 | "
              "iono_rate raised=0 kept=0 | sat_bias raised=0 kept=0 | "
              "sat_phase_bias raised=0 kept=0 | sat_clock raised=1 kept=0");
}

// A reference (GPS) re-reference re-inflates every allocated ISB, observed that epoch or
// not; a non-reference step re-inflates only its own; each ISB appears at most once.
TEST(ReinflateNeverShrinks, ReferenceStepReinflatesEveryAllocatedIsb) {
    const std::map<std::string, size_t> allocated = {{"BeiDou", 11}, {"Galileo", 10}};
    EXPECT_EQ(RealTimeEngine::isbReinflationSet(true, {}, allocated),
              (std::set<std::string>{"BeiDou", "Galileo"}));
    EXPECT_EQ(RealTimeEngine::isbReinflationSet(false, {"Galileo"}, allocated),
              (std::set<std::string>{"Galileo"}));
    EXPECT_EQ(RealTimeEngine::isbReinflationSet(true, {"Galileo"}, allocated),
              (std::set<std::string>{"BeiDou", "Galileo"}));
    EXPECT_TRUE(RealTimeEngine::isbReinflationSet(true, {}, {}).empty());   // GPS-only: none
}
