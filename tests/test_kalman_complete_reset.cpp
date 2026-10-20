/**
 * @file test_kalman_complete_reset.cpp
 * @brief The complete reset restores the slant-ionosphere variances on every run that carries them.
 *
 * KalmanFilter::resetFilterCovariance zeroes the covariance and re-seeds each family's
 * diagonal. The slant-ionosphere states (and, under enable_iono_rate, the rate states)
 * are re-seeded on every run that carries them (GnssConfig::carriesSlantIonosphere);
 * before that predicate the reset tested PPP-RTK mode alone, so a PPP run with
 * uncombined processing left every ionosphere variance at zero, the value the filter
 * reads as a frozen pivot. The reset is reached through a friend shim, since the
 * post-fit screen that triggers it needs a full measurement chain.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/KalmanFilter.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <fstream>
#include <string>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {

/// Reaches KalmanFilter's private complete reset (house XxxTestAccess friend-shim idiom).
struct KalmanFilterResetTestAccess {
    static void reset(KalmanFilter& kf, Eigen::MatrixXd& P) { kf.resetFilterCovariance(P, ""); }
};

namespace {

/// A configuration loaded from @p json through the loader, from a file in a directory
/// unique to this process and test.
GnssConfig loadConfig(const std::string& json) {
    namespace fs = std::filesystem;
    const fs::path dir = gnss_test::tempDir() / "complete_reset";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "config.json";
    { std::ofstream out(file); out << json; }
    LogManager log;
    GnssConfig cfg;
    EXPECT_TRUE(cfg.loadFromJson(file.string(), log));
    fs::remove_all(dir, ec);
    return cfg;
}

/// The chain a KalmanFilter needs, built without any input file.
struct FilterChain {
    LogManager     log;
    GnssConfig     cfg;
    ProductParser  products;
    RinexParser    rinex;
    StateManager   states;
    PppModel       model;
    KalmanFilter   filter;

    explicit FilterChain(GnssConfig c)
        : cfg(std::move(c)), products(cfg, log), rinex(cfg, log, products),
          states(5, log, cfg), model(cfg, products, log, rinex),
          filter(states, cfg, log, model) {}
};

}  // namespace

TEST(KalmanCompleteResetTest, RestoresTheIonosphereVarianceInEveryModeThatCarriesTheStates) {
    const char* const modes[] = {
        R"({"processing": {"mode": "PPP", "uncombined_enabled": true}})",
        R"({"processing": {"mode": "PPP-RTK"}})",
        R"({"processing": {"mode": "PPP-RTK-User"}})",
    };
    for (const char* json : modes) {
        SCOPED_TRACE(json);
        FilterChain c(loadConfig(json));
        ASSERT_TRUE(c.cfg.carriesSlantIonosphere());
        const size_t g05 = c.states.addIonosphereState("G05", 1.0);
        const size_t g12 = c.states.addIonosphereState("G12", 1.0);

        Eigen::MatrixXd P = c.states.getCovariance();
        KalmanFilterResetTestAccess::reset(c.filter, P);

        const double sd  = c.cfg.getInitialStd().ionosphere;
        const double var = sd * sd;
        ASSERT_GT(var, 0.0);
        EXPECT_DOUBLE_EQ(P(static_cast<Eigen::Index>(g05), static_cast<Eigen::Index>(g05)), var);
        EXPECT_DOUBLE_EQ(P(static_cast<Eigen::Index>(g12), static_cast<Eigen::Index>(g12)), var);
        // The reset zeroes the covariance first: no cross term survives.
        EXPECT_EQ(P(static_cast<Eigen::Index>(g05), static_cast<Eigen::Index>(g12)), 0.0);
    }
}

TEST(KalmanCompleteResetTest, RestoresTheIonosphereRateVarianceToo) {
    FilterChain c(loadConfig(
        R"({"processing": {"mode": "PPP", "uncombined_enabled": true},
            "estimation": {"enable_iono_rate": true}})"));
    ASSERT_TRUE(c.cfg.carriesSlantIonosphere());
    ASSERT_TRUE(c.cfg.enableIonoRate());
    const size_t level = c.states.addIonosphereState("G05", 1.0);
    const int rate_i = c.states.getIonosphereRateIndex("G05");
    ASSERT_GE(rate_i, 0);

    Eigen::MatrixXd P = c.states.getCovariance();
    KalmanFilterResetTestAccess::reset(c.filter, P);

    const double lsd = c.cfg.getInitialStd().ionosphere;
    const double rsd = c.cfg.getInitialStd().ionosphere_rate;
    ASSERT_GT(rsd, 0.0);
    EXPECT_DOUBLE_EQ(P(static_cast<Eigen::Index>(level), static_cast<Eigen::Index>(level)), lsd * lsd);
    EXPECT_DOUBLE_EQ(P(rate_i, rate_i), rsd * rsd);
}

}  // namespace GnssProcessor
