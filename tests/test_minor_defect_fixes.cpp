/**
 * @file test_minor_defect_fixes.cpp
 * @brief Regression tests for four small defects found by the comment audit.
 *
 *  - The configuration summary printed every PPP-RTK mode as "Baseline-Multi"; the
 *    loader and the summary now print one label per mode from one function.
 *  - Ionosphere::computeKlobucharDelay indexed the coefficient vector without a size
 *    check; without the 8 broadcast coefficients it returns 0 m.
 *  - Application declared its GIM after the models that keep a pointer to it, so the
 *    map was destroyed first; it is declared before them.
 *  - The raw-observable tracker picks the lexically lowest phase of a band; the pick
 *    rests on a stable sort, not on an unstable one that happens to keep the order.
 */

#include <gtest/gtest.h>

#include "app/Application.hpp"
#include "atmosphere/Ionosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/ConfigLoadCapture.hpp"
#include "network/slip/RawObservables.hpp"
#include "products/ProductCache.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Reads the addresses of the private members that decide teardown order (a friend of
/// Application).  Members of one access section sit at increasing addresses in
/// declaration order, and a member declared later is destroyed earlier.
struct ApplicationMemberOrderProbe {
    static std::uintptr_t gim(const Application& a)          { return reinterpret_cast<std::uintptr_t>(&a.gim_); }
    static std::uintptr_t pppModel(const Application& a)     { return reinterpret_cast<std::uintptr_t>(&a.ppp_model_); }
    static std::uintptr_t refPppModel(const Application& a)  { return reinterpret_cast<std::uintptr_t>(&a.ref_ppp_model_); }
};

namespace {

// ---------------------------------------------------------------------------
// Processing-mode labels
// ---------------------------------------------------------------------------

TEST(ProcessingModeLabel, EveryModeHasItsOwnName) {
    EXPECT_STREQ(processingModeLabel(ProcessingMode::PPP),             "PPP");
    EXPECT_STREQ(processingModeLabel(ProcessingMode::PPP_RTK),         "PPP-RTK");
    EXPECT_STREQ(processingModeLabel(ProcessingMode::BaselineSingle),  "Baseline-Single");
    EXPECT_STREQ(processingModeLabel(ProcessingMode::BaselineMulti),   "Baseline-Multi");
    EXPECT_STREQ(processingModeLabel(ProcessingMode::PPP_RTK_Network), "PPP-RTK-Network");
    EXPECT_STREQ(processingModeLabel(ProcessingMode::PPP_RTK_User),    "PPP-RTK-User");
}

// The summary line ("Loaded configuration: mode=...") of a network config names the
// network mode, as the "Processing mode" line above it does.
TEST(ProcessingModeLabel, SummaryLineNamesTheNetworkMode) {
    const TestSupport::LoadedConfig r = TestSupport::loadNetworkConfigCapturing("mode_label", "");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(TestSupport::contains(r.text, "Processing mode: PPP-RTK-Network")) << r.text;
    EXPECT_TRUE(TestSupport::contains(r.text, "Loaded configuration: mode=PPP-RTK-Network")) << r.text;
    EXPECT_FALSE(TestSupport::contains(r.text, "mode=Baseline-Multi")) << r.text;
}

// ---------------------------------------------------------------------------
// Klobuchar delay without coefficients
// ---------------------------------------------------------------------------

/// A RINEX 3 navigation header carrying the GPS Klobuchar lines named by @p which
/// ("AB" both, "A" alpha only, "" none).
std::filesystem::path writeNavHeader(const std::string& tag, const std::string& which) {
    const std::filesystem::path p = gnss_test::tempDir() / ("klobuchar_" + tag + ".rnx");
    std::ofstream f(p);
    f << "     3.04           N: GNSS NAV DATA    G: GPS              RINEX VERSION / TYPE\n";
    if (which.find('A') != std::string::npos)
        f << "GPSA   1.1176E-08 -7.4506E-09 -5.9605E-08  1.1921E-07       IONOSPHERIC CORR\n";
    if (which.find('B') != std::string::npos)
        f << "GPSB   1.1674E+05 -2.2938E+05 -1.3107E+05  1.0486E+06       IONOSPHERIC CORR\n";
    f << "                                                            END OF HEADER\n";
    return p;
}

/// The Klobuchar delay (m, L1) for a near-zenith satellite at local early afternoon,
/// from a receiver built on @p nav (an empty path for no navigation file).
double klobucharL1Delay(const std::filesystem::path& nav, double* l2_over_l1 = nullptr) {
    ProductCache::getInstance().clearCache();   // the Ionosphere reads the cache first
    LogManager log;
    GnssConfig config;
    if (!nav.empty()) config.setNavigationPath(nav);
    ProductParser parser(config, log);
    RinexParser rinex(config, log, parser);
    const Ionosphere ion(config, log, parser, rinex);

    const Eigen::Vector3d rx(-2493304.99, -4655214.79, 3565497.67);
    const Eigen::Vector3d sat = rx + 2.0e7 * rx.normalized();
    const GnssTime t{ 2400, 78736.0 };   // about 14:00 local at this longitude
    const double l1 = ion.computeKlobucharDelay(t, rx, sat, Constants::GPS_L1_FREQ);
    if (l2_over_l1 != nullptr) {
        const double l2 = ion.computeKlobucharDelay(t, rx, sat, Constants::GPS_L2_FREQ);
        *l2_over_l1 = l2 / l1;
    }
    return l1;
}

TEST(KlobucharSizeGuard, NoCoefficientsGiveZeroDelay) {
    EXPECT_DOUBLE_EQ(klobucharL1Delay({}), 0.0);
}

// One of the two header lines is not a model: the loader keeps neither and the
// evaluation answers 0 m instead of reading past the vector.
TEST(KlobucharSizeGuard, AlphaWithoutBetaGivesZeroDelay) {
    const auto nav = writeNavHeader("alpha_only", "A");
    EXPECT_DOUBLE_EQ(klobucharL1Delay(nav), 0.0);
    std::error_code ec;
    std::filesystem::remove(nav, ec);
}

// Control: the full set evaluates, scaled to L2 by (f1/f2)^2.
TEST(KlobucharSizeGuard, FullSetGivesTheModelDelay) {
    const auto nav = writeNavHeader("full", "AB");
    double ratio = 0.0;
    const double l1 = klobucharL1Delay(nav, &ratio);
    EXPECT_GT(l1, 0.5);
    EXPECT_LT(l1, 30.0);
    const double f = Constants::GPS_L1_FREQ / Constants::GPS_L2_FREQ;
    EXPECT_NEAR(ratio, f * f, 1e-9);
    std::error_code ec;
    std::filesystem::remove(nav, ec);
}

// ---------------------------------------------------------------------------
// Application teardown order
// ---------------------------------------------------------------------------

// Members are destroyed in reverse declaration order.  The two PPP models keep a
// non-owning pointer to the GIM, so the GIM is declared above both.
TEST(ApplicationTeardownOrder, GimOutlivesTheModelsThatPointToIt) {
    const Application app;
    EXPECT_LT(ApplicationMemberOrderProbe::gim(app), ApplicationMemberOrderProbe::pppModel(app));
    EXPECT_LT(ApplicationMemberOrderProbe::gim(app), ApplicationMemberOrderProbe::refPppModel(app));
}

// ---------------------------------------------------------------------------
// Same-band signal pick of the raw-observable tracker
// ---------------------------------------------------------------------------

const double kL1M = Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ;
const double kL2M = Constants::SPEED_OF_LIGHT / Constants::GPS_L2_FREQ;
const double kL5M = Constants::SPEED_OF_LIGHT / Constants::GPS_L5_FREQ;

void addSignal(PppObservation& obs, const std::string& phase, double lambda_m, double n_cyc) {
    constexpr double kRangeM = 22'000'000.0;
    obs.raw_observed[phase] = kRangeM / lambda_m + n_cyc;
    obs.raw_observed["C" + phase.substr(1)] = kRangeM;
}

// A band with several tracking modes, every phase with its code: the tracker names the
// lexically lowest phase of each of the two lowest bands, whatever else is reported.
TEST(RawObservablesSameBand, LexicallyLowestPhaseOfEachBandIsNamed) {
    LogManager log_manager;
    PppUtils utils{ log_manager };

    PppObservation g01{};
    g01.satellite = "G01";
    g01.system    = System::GPS;
    g01.elevation = 50.0;
    for (const char* s : { "L1C", "L1S", "L1L", "L1X", "L1P", "L1W" }) addSignal(g01, s, kL1M, 10.0);
    for (const char* s : { "L2C", "L2L", "L2S", "L2X", "L2P", "L2W" }) addSignal(g01, s, kL2M, 20.0);
    for (const char* s : { "L5I", "L5Q", "L5X" })                      addSignal(g01, s, kL5M, 30.0);

    const std::vector<std::vector<PppObservation>> modeled = { { g01 } };
    const std::vector<bool> present{ true };
    RawSlipEpoch ep;
    ep.reset(1);
    networkComputeRawObservables(modeled, present, 1, utils, ep);

    ASSERT_TRUE(ep.computed);
    ASSERT_EQ(ep.rows.size(), 1u);
    ASSERT_EQ(ep.rows[0].size(), 1u);
    EXPECT_EQ(ep.rows[0][0].sig1, "L1C");
    EXPECT_EQ(ep.rows[0][0].sig2, "L2C");
    EXPECT_TRUE(ep.rows[0][0].dual);
}

// When the lexically lowest phase of a band has no code, the next one of that band
// is named; the band is not skipped.
TEST(RawObservablesSameBand, PhaseWithoutCodeIsPassedOver) {
    LogManager log_manager;
    PppUtils utils{ log_manager };

    PppObservation g02{};
    g02.satellite = "G02";
    g02.system    = System::GPS;
    g02.elevation = 50.0;
    addSignal(g02, "L1W", kL1M, 10.0);
    addSignal(g02, "L1X", kL1M, 10.0);
    g02.raw_observed["L1C"] = 1.0e8;   // a phase with no matching C1C
    addSignal(g02, "L2W", kL2M, 20.0);

    const std::vector<std::vector<PppObservation>> modeled = { { g02 } };
    const std::vector<bool> present{ true };
    RawSlipEpoch ep;
    ep.reset(1);
    networkComputeRawObservables(modeled, present, 1, utils, ep);

    ASSERT_EQ(ep.rows.size(), 1u);
    ASSERT_EQ(ep.rows[0].size(), 1u);
    EXPECT_EQ(ep.rows[0][0].sig1, "L1W");
    EXPECT_EQ(ep.rows[0][0].sig2, "L2W");
}

}  // namespace
}  // namespace GnssProcessor
