/**
 * @file test_jump_gap_fitter.cpp
 * @brief Unit tests for JumpGapFitter, the pure two-sided trend+step fitter
 *        behind the non-causal smoother cycle-slip bridge.
 *
 * All inputs are deterministic closed-form lines with an injected step, so the
 * tolerances cover floating-point rounding only, not a statistical margin.
 */
#include <gtest/gtest.h>

#include "slip/JumpGapFitter.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace GnssProcessor {
namespace {

// Build a window of `n` samples at `t0 + k*dt`, k=0..n-1, with
// y = intercept + slope*t  (intercept/slope in absolute time, not centred).
JumpGapFitter::Window makeLine(double t0, double dt, int n, double intercept,
                               double slope) {
    JumpGapFitter::Window w;
    for (int k = 0; k < n; ++k) {
        const double t = t0 + k * dt;
        w.t_s.push_back(t);
        w.y_m.push_back(intercept + slope * t);
    }
    return w;
}

TEST(JumpGapFitter, RecoversStepAndSlopeExactly) {
    const double tb = 100.0;
    // Pre: y = 2 + 0.1 t over t in [90,98]; Post: same line + 0.862 step.
    const auto pre = makeLine(90.0, 2.0, 5, /*intercept=*/2.0, /*slope=*/0.1);
    auto post = makeLine(100.0, 2.0, 5, 2.0, 0.1);
    for (double& y : post.y_m) y += 0.862;

    JumpGapFitter fitter;
    const auto r = fitter.fitStep(pre, post, tb);

    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.n_pre, 5);
    EXPECT_EQ(r.n_post, 5);
    EXPECT_NEAR(r.pre_slope, 0.1, 1e-9);
    EXPECT_NEAR(r.post_slope, 0.1, 1e-9);
    // Level at boundary: 2 + 0.1*100 = 12.0 (pre), +0.862 (post).
    EXPECT_NEAR(r.pre_level_m, 12.0, 1e-6);
    EXPECT_NEAR(r.post_level_m, 12.862, 1e-6);
    EXPECT_NEAR(r.step_m, 0.862, 1e-6);
    EXPECT_NEAR(r.rms_resid_m, 0.0, 1e-9);  // perfect lines ⇒ zero residual
}

TEST(JumpGapFitter, RecoversIndependentPerSideSlopes) {
    const double tb = 100.0;
    // Pre: slope 0.10, level 12.0 at the boundary (y = 2 + 0.1 t at t=100).
    const auto pre = makeLine(90.0, 2.0, 6, /*intercept=*/2.0, /*slope=*/0.10);
    const double pre_level_at_tb = 12.0;
    const double intended_step = -1.5;
    const double post_slope = 0.05;  // deliberately different from pre
    // Post: built directly about the boundary with a clean intended step so
    // the step is independent of the (differing) slope.
    JumpGapFitter::Window post;
    for (int k = 0; k < 6; ++k) {
        const double t = tb + k * 2.0;
        post.t_s.push_back(t);
        post.y_m.push_back(pre_level_at_tb + intended_step + post_slope * (t - tb));
    }

    JumpGapFitter fitter;
    const auto r = fitter.fitStep(pre, post, tb);

    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.pre_slope, 0.10, 1e-9);
    EXPECT_NEAR(r.post_slope, post_slope, 1e-9);
    EXPECT_NEAR(r.step_m, intended_step, 1e-6);
}

TEST(JumpGapFitter, FailsWhenSideHasTooFewPoints) {
    const double tb = 100.0;
    const auto pre = makeLine(94.0, 2.0, 3, 2.0, 0.1);   // only 3 < default 4
    const auto post = makeLine(100.0, 2.0, 5, 2.0, 0.1);

    JumpGapFitter fitter;  // min_window_points default 4
    const auto r = fitter.fitStep(pre, post, tb);
    EXPECT_FALSE(r.ok);
}

TEST(JumpGapFitter, HonorsCustomMinWindowPoints) {
    const double tb = 100.0;
    const auto pre = makeLine(96.0, 2.0, 2, 2.0, 0.1);
    const auto post = makeLine(100.0, 2.0, 2, 2.0, 0.1);

    JumpGapFitter fitter;
    fitter.min_window_points = 2;
    const auto r = fitter.fitStep(pre, post, tb);
    EXPECT_TRUE(r.ok);  // exactly meets the relaxed minimum
}

TEST(JumpGapFitter, FailsOnNonFiniteInput) {
    const double tb = 100.0;
    auto pre = makeLine(90.0, 2.0, 5, 2.0, 0.1);
    pre.y_m[2] = std::numeric_limits<double>::quiet_NaN();
    const auto post = makeLine(100.0, 2.0, 5, 2.0, 0.1);

    JumpGapFitter fitter;
    const auto r = fitter.fitStep(pre, post, tb);
    EXPECT_FALSE(r.ok);
}

TEST(JumpGapFitter, FailsOnLengthMismatch) {
    JumpGapFitter::Window pre;
    pre.t_s = {90.0, 92.0, 94.0, 96.0};
    pre.y_m = {1.0, 2.0, 3.0};  // shorter than t_s
    const auto post = makeLine(100.0, 2.0, 5, 2.0, 0.1);

    JumpGapFitter fitter;
    const auto r = fitter.fitStep(pre, post, 100.0);
    EXPECT_FALSE(r.ok);
}

TEST(JumpGapFitter, WellConditionedAtRawGpsSecondScale) {
    // Continuous-GPS-second magnitudes (~1.4e9) must not break the centred
    // normal equations.
    const double tb = 1.4e9 + 100.0;
    const auto pre = makeLine(1.4e9 + 90.0, 2.0, 5, /*intercept=*/0.0, 0.1);
    auto post = makeLine(1.4e9 + 100.0, 2.0, 5, 0.0, 0.1);
    for (double& y : post.y_m) y += 0.5;

    JumpGapFitter fitter;
    const auto r = fitter.fitStep(pre, post, tb);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.step_m, 0.5, 1e-4);
    EXPECT_NEAR(r.pre_slope, 0.1, 1e-6);
}

}  // namespace
}  // namespace GnssProcessor
