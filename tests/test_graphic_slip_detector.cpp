/**
 * @file test_graphic_slip_detector.cpp
 * @brief GraphicSlipDetector on a moving satellite: the default test on |dG| and
 *        the trend-centred test selected by estimation.cycle_slip.graphic_trend_centred.
 *
 * The between-epoch GRAPHIC difference dG holds the satellite's range rate times
 * the epoch spacing: hundreds of metres for a moving satellite.  The detector's
 * threshold is a MAD spread about the window median, so a test of the raw |dG|
 * against it fires as soon as the window gives a threshold.  That is the default
 * (it keeps the output of every run that selects the detector).  The trend-centred
 * mode tests the change of dG from one epoch to the next, measured from its window
 * median.
 *
 * The stream is synthetic: a quadratic range (a constant range rate plus a
 * constant range acceleration), code and phase noise from a fixed-seed generator,
 * and an optional integer-cycle phase jump.  Code noise is 0.1 m and phase noise
 * 3 mm, in the range of a modern receiver; the detector's own floor and its MAD
 * scale are untouched.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/GraphicSlipDetector.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

const double kLambdaL1 = Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ;   // 0.190294 m

constexpr double kRange0M      = 22'000'000.0;   ///< Range at the first epoch.
constexpr double kRangeRateMps = -650.0;         ///< A fast-moving satellite.
constexpr double kRangeAccMps2 = 0.15;           ///< Its range acceleration.
constexpr double kCodeSigmaM   = 0.10;
constexpr double kPhaseSigmaM  = 0.003;
constexpr double kEpochS       = 1.0;

/// A deterministic one-satellite, one-band observation stream.
class Stream {
public:
    explicit Stream(unsigned seed = 20261017u) : rng_(seed) {}

    /// One epoch k (seconds since the stream start), with @p slip_cycles of integer
    /// phase jump added from @p slip_from_epoch on (cycles; 0 for none).
    PppObservation epoch(long k, long slip_from_epoch = -1, double slip_cycles = 0.0) {
        return epochAt(static_cast<double>(k) * kEpochS, k, slip_from_epoch, slip_cycles);
    }

    /// An epoch at an explicit time (for a data gap), phase jump as in @ref epoch.
    PppObservation epochAt(double t_s, long k, long slip_from_epoch, double slip_cycles) {
        const double range = kRange0M + kRangeRateMps * t_s + 0.5 * kRangeAccMps2 * t_s * t_s;
        const double jump  = (slip_from_epoch >= 0 && k >= slip_from_epoch) ? slip_cycles : 0.0;
        PppObservation obs{};
        obs.satellite = "G07";
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2400, 100000.0 + t_s };
        obs.elevation = 45.0;
        obs.raw_observed["C1C"] = range + code_(rng_);
        obs.raw_observed["L1C"] = (range + phase_(rng_)) / kLambdaL1 + 1234.0 + jump;
        return obs;
    }

private:
    std::mt19937 rng_;
    std::normal_distribution<double> code_{ 0.0, kCodeSigmaM };
    std::normal_distribution<double> phase_{ 0.0, kPhaseSigmaM };
};

/// Load a GnssConfig through the real JSON path with the GRAPHIC detector selected,
/// with or without the trend-centred statistic.
void loadGraphicConfig(GnssConfig& config, LogManager& log_manager, bool trend_centred) {
    namespace fs = std::filesystem;
    const fs::path p = gnss_test::tempDir() /
        (trend_centred ? "gnss_graphic_centred.json" : "gnss_graphic_default.json");
    {
        std::ofstream f(p);
        f << "{ \"estimation\": { \"cycle_slip\": { \"graphic\": true"
          << (trend_centred ? ", \"graphic_trend_centred\": true" : "")
          << " } } }";
    }
    EXPECT_TRUE(config.loadFromJson(p, log_manager));
    std::error_code ec;
    fs::remove(p, ec);
}

/// The detector under test (6 sigma gate, 0.5 m floor) on a config that selects the
/// GRAPHIC detector, in the default mode or the trend-centred mode.
struct Fixture {
    LogManager log_manager;
    GnssConfig config;
    std::unique_ptr<GraphicSlipDetector> detector;

    explicit Fixture(bool trend_centred) {
        loadGraphicConfig(config, log_manager, trend_centred);
        detector = std::make_unique<GraphicSlipDetector>(config, log_manager);
    }
};

/// Fixture for the trend-centred mode.
struct CentredFixture : Fixture {
    CentredFixture() : Fixture(true) {}
};

/// Fixture for the default mode.
struct DefaultFixture : Fixture {
    DefaultFixture() : Fixture(false) {}
};

/// Lines of a text file.
std::vector<std::string> readLines(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line); ) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

/// Number of comma-separated fields of a CSV line.
std::size_t fieldCount(const std::string& line) {
    std::size_t n = 1;
    for (const char c : line) n += (c == ',');
    return n;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// ---------------------------------------------------------------------------
// Trend-centred mode (graphic_trend_centred = true)
// ---------------------------------------------------------------------------

// A satellite whose range rate is -650 m/s and whose range accelerates: dG is -650 m
// every epoch.  The raw |dG| test fires as soon as the window gives a threshold; the
// centred statistic stays inside it.
TEST(GraphicSlipDetector, MovingSatelliteWithoutSlipRaisesNothing) {
    CentredFixture f;
    Stream s;
    std::size_t flagged = 0;
    for (long k = 0; k < 400; ++k)
        flagged += f.detector->detect(std::vector<PppObservation>{ s.epoch(k) }).size();
    EXPECT_EQ(flagged, 0u);
}

// A jump of 40 cycles is 3.8 m of GRAPHIC: well above the 6 sigma gate on the centred
// statistic.  It is flagged once, at its own epoch.  The magnitude is only good to about
// a cycle (the statistic carries ~0.12 m of noise, 2 cycles peak to peak), so it is
// compared with a tolerance.
TEST(GraphicSlipDetector, SlipOnMovingSatelliteIsFlaggedOnceWithItsMagnitude) {
    CentredFixture f;
    Stream s;
    constexpr long kSlipEpoch = 150;
    std::vector<long> hits;
    GraphicSlipResult hit;
    for (long k = 0; k < 400; ++k) {
        const auto r = f.detector->detect(
            std::vector<PppObservation>{ s.epoch(k, kSlipEpoch, 40.0) });
        for (const auto& x : r) { hits.push_back(k); hit = x; }
    }
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], kSlipEpoch);
    EXPECT_EQ(hit.satellite, "G07");
    EXPECT_EQ(hit.signal, "L1C");
    EXPECT_EQ(hit.amb_key, "G07_L1C");
    EXPECT_NEAR(hit.slip_cycles, 40, 6);
}

// The sign of the jump survives the centring.
TEST(GraphicSlipDetector, NegativeSlipKeepsItsSign) {
    CentredFixture f;
    Stream s;
    constexpr long kSlipEpoch = 90;
    GraphicSlipResult hit;
    std::size_t n = 0;
    for (long k = 0; k < 200; ++k)
        for (const auto& x : f.detector->detect(
                 std::vector<PppObservation>{ s.epoch(k, kSlipEpoch, -35.0) })) {
            hit = x;
            ++n;
        }
    ASSERT_EQ(n, 1u);
    EXPECT_NEAR(hit.slip_cycles, -35, 6);
}

// A gap longer than the arc limit restarts the arc, and the range moved by tens of
// kilometres across it: neither the second-difference history nor the first
// difference may carry over (that would flag the gap itself).
TEST(GraphicSlipDetector, DataGapRestartsTheArcWithoutAFalseSlip) {
    CentredFixture f;
    Stream s;
    std::size_t flagged = 0;
    long k = 0;
    for (; k < 100; ++k)
        flagged += f.detector->detect(std::vector<PppObservation>{ s.epoch(k) }).size();
    const double gap_s = 300.0;   // above kMaxGapSeconds (120 s)
    for (long j = 0; j < 100; ++j, ++k)
        flagged += f.detector->detect(std::vector<PppObservation>{
            s.epochAt(static_cast<double>(k) * kEpochS + gap_s, k, -1, 0.0) }).size();
    EXPECT_EQ(flagged, 0u);
}

// resetSatellite drops the arc; the next epochs start a fresh one and a slip after the
// window refills is still found.
TEST(GraphicSlipDetector, ResetSatelliteStartsAFreshArc) {
    CentredFixture f;
    Stream s;
    long k = 0;
    for (; k < 60; ++k) f.detector->detect(std::vector<PppObservation>{ s.epoch(k) });
    f.detector->resetSatellite("G07");
    std::size_t n = 0;
    for (; k < 160; ++k)
        n += f.detector->detect(std::vector<PppObservation>{ s.epoch(k, 130, 40.0) }).size();
    EXPECT_EQ(n, 1u);
}

// The centred trace ends with the tested statistic.
TEST(GraphicSlipDetector, CentredTraceEndsWithTheTestedStatistic) {
    namespace fs = std::filesystem;
    const fs::path dir = gnss_test::tempDir() / "graphic_trace_centred";
    {
        CentredFixture f;
        ASSERT_TRUE(f.detector->enableTracing(dir));
        Stream s;
        for (long k = 0; k < 12; ++k)
            f.detector->detect(std::vector<PppObservation>{ s.epoch(k) });
    }   // the detector closes the trace
    const auto lines = readLines(dir / "graphic_slip_trace.csv");
    ASSERT_EQ(lines.size(), 13u);
    EXPECT_TRUE(endsWith(lines[0], ",is_slip,resid_m")) << lines[0];
    for (const auto& line : lines)
        EXPECT_EQ(fieldCount(line), 15u) << line;
}

// ---------------------------------------------------------------------------
// Default mode (graphic_trend_centred absent or false)
// ---------------------------------------------------------------------------

// The default tests |dG| itself.  On a moving satellite it exceeds the threshold at the
// first epoch the window gives one (arc epoch 6, the seventh epoch of the stream), the
// arc is reset, and the same happens again every seven epochs.  This is the behaviour a
// run that selects the detector keeps unless it sets graphic_trend_centred.
TEST(GraphicSlipDetector, DefaultModeFlagsAMovingSatelliteAtArcEpochSix) {
    DefaultFixture f;
    Stream s;
    std::vector<long> hits;
    for (long k = 0; k < 30; ++k)
        if (!f.detector->detect(std::vector<PppObservation>{ s.epoch(k) }).empty())
            hits.push_back(k);
    EXPECT_EQ(hits, (std::vector<long>{ 6, 13, 20, 27 }));
}

// The default trace keeps its fourteen columns: no resid_m, nan in the missing sigma and
// threshold of the arc's first row, and the -1 sentinel on the later rows until the
// window gives a threshold.
TEST(GraphicSlipDetector, DefaultTraceKeepsItsColumnsAndSentinels) {
    namespace fs = std::filesystem;
    const fs::path dir = gnss_test::tempDir() / "graphic_trace_default";
    {
        DefaultFixture f;
        ASSERT_TRUE(f.detector->enableTracing(dir));
        Stream s;
        for (long k = 0; k < 3; ++k)
            f.detector->detect(std::vector<PppObservation>{ s.epoch(k) });
    }
    const auto lines = readLines(dir / "graphic_slip_trace.csv");
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0],
              "week,tow_s,satellite,system,elevation_deg,signal,graphic_m,dg_m,"
              "mad_sigma_m,threshold_m,arc_count,slip_cycles,fractional,is_slip");
    for (const auto& line : lines)
        EXPECT_EQ(fieldCount(line), 14u) << line;
    EXPECT_TRUE(endsWith(lines[1], ",nan,nan,nan,1,0,nan,0")) << lines[1];
    EXPECT_TRUE(endsWith(lines[2], ",-1.000000,-1.000000,1,0,0.000,0")) << lines[2];
}

// ---------------------------------------------------------------------------
// The key
// ---------------------------------------------------------------------------

TEST(GraphicSlipDetector, TrendCentredKeyDefaultsOffAndLoadsFromJson) {
    {
        LogManager log_manager;
        GnssConfig config;
        loadGraphicConfig(config, log_manager, false);
        EXPECT_TRUE(config.getCycleSlipConfig().graphic);
        EXPECT_FALSE(config.getCycleSlipConfig().graphic_trend_centred);
    }
    {
        LogManager log_manager;
        GnssConfig config;
        loadGraphicConfig(config, log_manager, true);
        EXPECT_TRUE(config.getCycleSlipConfig().graphic);
        EXPECT_TRUE(config.getCycleSlipConfig().graphic_trend_centred);
    }
    {
        const GnssConfig config;   // no file loaded
        EXPECT_FALSE(config.getCycleSlipConfig().graphic_trend_centred);
    }
}

}  // namespace
}  // namespace GnssProcessor
