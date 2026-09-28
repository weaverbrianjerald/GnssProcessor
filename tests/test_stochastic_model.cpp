/**
 * @file test_stochastic_model.cpp
 * @brief Unit tests for StochasticModelWeight elevation-dependent weighting.
 *
 * The elevation model amplifies the zenith variance by sin(el)^(-2·power):
 * power = code_elevation_power_ (default 1.0 -> csc²(el) on variance) for code
 * observables, and phase_elevation_power_ (default 1.0 -> csc²(el), the same as
 * code; 0.5 -> csc(el) selectable via JSON) for 'L…' observables.  A near-zero elevation (sin ≤ 1e-6) is clamped to the zenith
 * variance to avoid amplifying to infinity.
 *
 * computeElevation is private; the befriended StochasticModelTestAccess shim
 * calls it directly so the test exercises the weighting kernel itself, not the
 * configured model-type dispatch.  A default-constructed GnssConfig supplies
 * the documented code/phase powers (1.0 / 1.0).
 */

#include <gtest/gtest.h>

#include "model/StochasticModel.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <string>

namespace GnssProcessor {

// Befriended in StochasticModel.hpp: forwards to the private const
// computeElevation kernel.
struct StochasticModelTestAccess {
    static double computeElevation(const StochasticModelWeight& sm,
                                   const std::string& obs_type,
                                   double default_var, double elevation_deg) {
        return sm.computeElevation(obs_type, default_var, elevation_deg);
    }
};

namespace {

// StochasticModelWeight reads its powers from config and logs once; a default
// GnssConfig gives code_elevation_power_ = 1.0, phase_elevation_power_ = 1.0.
struct StochHarness {
    LogManager log_manager;
    GnssConfig config;
    StochasticModelWeight sm{ config, log_manager };
};

TEST(StochasticModelTest, ZenithReturnsZenithVariance) {
    StochHarness h;
    // sin(90°) = 1 -> no amplification, for any power.
    EXPECT_NEAR(StochasticModelTestAccess::computeElevation(h.sm, "C1W", 0.04, 90.0), 0.04, 1e-12);
    EXPECT_NEAR(StochasticModelTestAccess::computeElevation(h.sm, "L1C", 4.0e-4, 90.0), 4.0e-4, 1e-15);
}

TEST(StochasticModelTest, CodeAndPhaseUseCscSquaredByDefault) {
    StochHarness h;
    const double V = 0.04;
    // el = 30°, sin = 0.5.  Code power 1.0 -> /sin² = ×4.  Phase power 1.0
    // (symmetric default) -> /sin² = ×4.
    EXPECT_NEAR(StochasticModelTestAccess::computeElevation(h.sm, "C1W", V, 30.0), 4.0 * V, 1e-12);
    EXPECT_NEAR(StochasticModelTestAccess::computeElevation(h.sm, "L1C", V, 30.0), 4.0 * V, 1e-12);
    // 'D' (Doppler) / empty-first-char fall back to the code power.
    EXPECT_NEAR(StochasticModelTestAccess::computeElevation(h.sm, "D1C", V, 30.0), 4.0 * V, 1e-12);
}

TEST(StochasticModelTest, NearZeroElevationIsNotAmplified) {
    StochHarness h;
    // sin(el) ≤ 1e-6 guard returns the zenith variance unscaled (exactly).
    EXPECT_DOUBLE_EQ(StochasticModelTestAccess::computeElevation(h.sm, "C1W", 0.04, 0.0), 0.04);
    EXPECT_DOUBLE_EQ(StochasticModelTestAccess::computeElevation(h.sm, "C1W", 0.04, 5.0e-5), 0.04);
}

TEST(StochasticModelTest, VarianceIncreasesTowardHorizon) {
    StochHarness h;
    const double V = 0.04;
    const double v90 = StochasticModelTestAccess::computeElevation(h.sm, "C1W", V, 90.0);
    const double v45 = StochasticModelTestAccess::computeElevation(h.sm, "C1W", V, 45.0);
    const double v15 = StochasticModelTestAccess::computeElevation(h.sm, "C1W", V, 15.0);
    EXPECT_NEAR(v90, V, 1e-12);
    EXPECT_LT(v90, v45);
    EXPECT_LT(v45, v15);
    // Phase amplification matches code at the same elevation under the
    // symmetric default (both powers 1.0).
    const double code15 = v15;
    const double phase15 = StochasticModelTestAccess::computeElevation(h.sm, "L1C", V, 15.0);
    EXPECT_NEAR(phase15, code15, 1e-12);
}

}  // namespace
}  // namespace GnssProcessor
