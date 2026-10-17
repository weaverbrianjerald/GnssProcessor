/**
 * @file test_graphic_slip_detector.cpp
 * @brief GraphicSlipDetector on a moving satellite: the threshold and the tested
 *        statistic are centred the same way.
 *
 * The between-epoch GRAPHIC difference dG holds the satellite's range rate times
 * the epoch spacing: hundreds of metres for a moving satellite.  The detector's
 * threshold is a MAD spread about the window median, so a test of the raw |dG|
 * against it fires on every epoch once the window fills.  The detector tests the
 * change of dG from one epoch to the next, measured from its window median.
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

#include <cmath>
#include <cstddef>
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

/// The detector under test with the default configuration (6 sigma gate, 0.5 m floor).
struct Fixture {
    LogManager log_manager;
    GnssConfig config;
    GraphicSlipDetector detector{ config, log_manager };
};

// A satellite whose range rate is -650 m/s and whose range accelerates: dG is -650 m
// every epoch.  The raw |dG| test fired on every epoch once the window filled; the
// centred statistic stays inside the threshold.
TEST(GraphicSlipDetector, MovingSatelliteWithoutSlipRaisesNothing) {
    Fixture f;
    Stream s;
    std::size_t flagged = 0;
    for (long k = 0; k < 400; ++k)
        flagged += f.detector.detect(std::vector<PppObservation>{ s.epoch(k) }).size();
    EXPECT_EQ(flagged, 0u);
}

// A jump of 40 cycles is 3.8 m of GRAPHIC: well above the 6 sigma gate on the centred
// statistic.  It is flagged once, at its own epoch.  The magnitude is only good to about
// a cycle (the statistic carries ~0.12 m of noise, 2 cycles peak to peak), so it is
// compared with a tolerance.
TEST(GraphicSlipDetector, SlipOnMovingSatelliteIsFlaggedOnceWithItsMagnitude) {
    Fixture f;
    Stream s;
    constexpr long kSlipEpoch = 150;
    std::vector<long> hits;
    GraphicSlipResult hit;
    for (long k = 0; k < 400; ++k) {
        const auto r = f.detector.detect(
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
    Fixture f;
    Stream s;
    constexpr long kSlipEpoch = 90;
    GraphicSlipResult hit;
    std::size_t n = 0;
    for (long k = 0; k < 200; ++k)
        for (const auto& x : f.detector.detect(
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
    Fixture f;
    Stream s;
    std::size_t flagged = 0;
    long k = 0;
    for (; k < 100; ++k)
        flagged += f.detector.detect(std::vector<PppObservation>{ s.epoch(k) }).size();
    const double gap_s = 300.0;   // above kMaxGapSeconds (120 s)
    for (long j = 0; j < 100; ++j, ++k)
        flagged += f.detector.detect(std::vector<PppObservation>{
            s.epochAt(static_cast<double>(k) * kEpochS + gap_s, k, -1, 0.0) }).size();
    EXPECT_EQ(flagged, 0u);
}

// resetSatellite drops the arc; the next epochs start a fresh one and a slip after the
// window refills is still found.
TEST(GraphicSlipDetector, ResetSatelliteStartsAFreshArc) {
    Fixture f;
    Stream s;
    long k = 0;
    for (; k < 60; ++k) f.detector.detect(std::vector<PppObservation>{ s.epoch(k) });
    f.detector.resetSatellite("G07");
    std::size_t n = 0;
    for (; k < 160; ++k)
        n += f.detector.detect(std::vector<PppObservation>{ s.epoch(k, 130, 40.0) }).size();
    EXPECT_EQ(n, 1u);
}

}  // namespace
}  // namespace GnssProcessor
