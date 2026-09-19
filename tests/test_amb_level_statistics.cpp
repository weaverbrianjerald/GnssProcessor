/**
 * @file test_amb_level_statistics.cpp
 * @brief ALC-STAT: production-detector null experiments and nuisance witnesses.
 *
 * Fixed seeds, independent observation-level noise, unchanged n=20/k=4/floors.
 * These characterize assumptions; they do not calibrate field false alarms.
 */
#include <gtest/gtest.h>
#include "realtime/AmbLevelCheck.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <utility>

using GnssProcessor::AmbLevelCheck;

namespace {
constexpr double f1 = 1575.42e6, f2 = 1227.60e6;
constexpr double lambda = 299792458.0 / (f1 - f2);
constexpr int n = 20;

std::pair<double, double> levels(double p1, double p2, double c1, double c2) {
    return {AmbLevelCheck::wlLevelMinusState(f1, f2, lambda, p1, p2, c1, c2),
            AmbLevelCheck::ifLevelMinusState(f1, f2, p1, p2, c1, c2)};
}

AmbLevelCheck detector() {
    AmbLevelCheck d;
    AmbLevelCheck::Config cfg;
    cfg.mode = AmbLevelCheck::Mode::Observe;
    d.configure(cfg);
    return d;
}

// Each band has stationary AR(1) code noise with marginal sigma 0.5 m.
// Phase noise is independent white noise with sigma 0.005 m. No state error.
struct Noise {
    std::mt19937 rng;
    std::normal_distribution<double> normal{0.0, 1.0};
    double phi, c1{}, c2{};
    Noise(unsigned seed, double correlation) : rng(seed), phi(correlation) {}
    void start() { c1 = 0.5 * normal(rng); c2 = 0.5 * normal(rng); }
    std::pair<double, double> next() {
        const double p1 = 0.005 * normal(rng), p2 = 0.005 * normal(rng);
        const auto v = levels(p1, p2, c1, c2);
        const double scale = 0.5 * std::sqrt(1.0 - phi * phi);
        c1 = phi * c1 + scale * normal(rng);
        c2 = phi * c2 + scale * normal(rng);
        return v;
    }
};

struct Census {
    uint64_t windows{}, wl{}, ifd{}, either{}, both{}, floors{};
    double wlMeanSq{}, wlSigmaSq{}, ifMeanSq{}, ifSigmaSq{};
};

Census independentWindows(double phi) {
    Noise noise(20260917, phi);
    auto d = detector();
    Census c;
    for (int trial = 0; trial < 250000; ++trial) {
        d.restart("G12");
        noise.start(); // independent, stationary start for every window
        AmbLevelCheck::Verdict v;
        for (int j = 0; j < n; ++j) {
            const auto sample = noise.next();
            v = d.push("G12", sample.first, sample.second);
        }
        EXPECT_TRUE(v.tested);
        const bool wl = std::abs(v.wl_mean) > 4 * v.wl_sigma;
        const bool ifd = std::abs(v.if_mean) > 4 * v.if_sigma;
        ++c.windows;
        c.wl += wl; c.ifd += ifd; c.both += wl && ifd;
        c.either += v.test != AmbLevelCheck::Test::None;
        c.floors += v.wl_sigma == 0.02 || v.if_sigma == 0.05;
        c.wlMeanSq += v.wl_mean * v.wl_mean;
        c.wlSigmaSq += v.wl_sigma * v.wl_sigma;
        c.ifMeanSq += v.if_mean * v.if_mean;
        c.ifSigmaSq += v.if_sigma * v.if_sigma;
    }
    EXPECT_EQ(c.either, c.wl + c.ifd - c.both);
    std::cout << "ALC_STAT {\"kind\":\"independent\",\"phi\":" << phi
              << ",\"windows\":" << c.windows << ",\"wl\":" << c.wl
              << ",\"if\":" << c.ifd << ",\"either\":" << c.either
              << ",\"both\":" << c.both << ",\"floored_windows\":" << c.floors
              << ",\"wl_variance_ratio\":" << c.wlMeanSq / c.wlSigmaSq
              << ",\"if_variance_ratio\":" << c.ifMeanSq / c.ifSigmaSq << "}\n";
    return c;
}

AmbLevelCheck::Verdict constantWindow(double p1, double p2, double c1, double c2) {
    auto d = detector();
    const auto sample = levels(p1, p2, c1, c2);
    AmbLevelCheck::Verdict v;
    for (int j = 0; j < n; ++j) v = d.push("G12", sample.first, sample.second);
    return v;
}
} // namespace

TEST(AmbLevelStatistics, IndependentNullIsStudentTAndCorrelatedCodeInflatesRejections) {
    const auto iid = independentWindows(0.0);
    // Two-sided t(19) tail at 4, independently integrated in alc_statistics_audit.py.
    constexpr double p = 0.000766192337228655;
    const double expected = iid.windows * p;
    const double tolerance = 6 * std::sqrt(iid.windows * p * (1 - p));
    EXPECT_NEAR(static_cast<double>(iid.wl), expected, tolerance);
    EXPECT_NEAR(static_cast<double>(iid.ifd), expected, tolerance);
    EXPECT_NEAR(iid.wlMeanSq / iid.wlSigmaSq, 1.0, 0.03);
    EXPECT_NEAR(iid.ifMeanSq / iid.ifSigmaSq, 1.0, 0.03);
    const auto medium = independentWindows(0.5);
    const auto strong = independentWindows(0.9);
    EXPECT_GT(medium.either, 10 * iid.either);
    EXPECT_GT(strong.either, 3 * medium.either);
    EXPECT_GT(strong.ifMeanSq / strong.ifSigmaSq, 10.0);
}

TEST(AmbLevelStatistics, StreamingCensusIncludesOverlapAndTripClears) {
    for (double phi : {0.0, 0.5, 0.9}) {
        Noise noise(17092026, phi);
        noise.start();
        auto d = detector();
        constexpr uint64_t samples = 1000000;
        for (uint64_t j = 0; j < samples; ++j) {
            const auto sample = noise.next();
            d.push("G12", sample.first, sample.second);
        }
        const auto c = d.counters();
        const auto trips = c.trips_wl + c.trips_if;
        // Every completed refill suppresses n-1 checks. A final trip may leave
        // fewer than n-1 samples available for its refill.
        EXPECT_EQ(c.samples, samples);
        EXPECT_LE(c.checks, samples - (n - 1) * trips);
        EXPECT_GE(c.checks, samples - (n - 1) * (trips + 1));
        EXPECT_GT(trips, 0u);
        std::cout << "ALC_STAT {\"kind\":\"stream\",\"phi\":" << phi
                  << ",\"samples\":" << samples << ",\"checks\":" << c.checks
                  << ",\"wl\":" << c.trips_wl << ",\"if\":" << c.trips_if << "}\n";
    }
}

TEST(AmbLevelStatistics, CorrectAmbiguitiesWithCodeOffsetAreNotAnAmbiguityNull) {
    // True ambiguity errors are zero; a common +0.5 m CODE-only nuisance remains.
    const auto code = constantWindow(0, 0, 0.5, 0.5);
    const auto ambiguity = constantWindow(-0.5, -0.5, 0, 0);
    EXPECT_EQ(code.test, AmbLevelCheck::Test::Wl);
    EXPECT_DOUBLE_EQ(code.wl_mean, ambiguity.wl_mean);
    EXPECT_DOUBLE_EQ(code.if_mean, ambiguity.if_mean);
    // Both explanations are observationally identical to this detector.
    EXPECT_NEAR(code.if_mean, -0.5, 1e-14);
}

TEST(AmbLevelStatistics, PhaseOnlyClockSurvivesWhileCommonGeometryCancels) {
    const auto common = constantWindow(0.5, 0.5, 0.5, 0.5);
    EXPECT_EQ(common.test, AmbLevelCheck::Test::None);
    const auto phaseClock = constantWindow(0.5, 0.5, 0, 0);
    EXPECT_EQ(phaseClock.test, AmbLevelCheck::Test::Wl);
    EXPECT_NEAR(phaseClock.wl_mean, 0.5 / lambda, 1e-14);
    EXPECT_NEAR(phaseClock.if_mean, 0.5, 1e-14);
}
