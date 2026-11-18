// Unit tests of the near-null guard's verdicts (include/network/ar/RingFix.hpp, network.ring_fix.nn_guard): a held chord
// whose code residuals, centred on its satellite's family and then its receiver, sit at k near-null steps with a small
// ionosphere-free deviation is flagged; a chord's own code bias, half a step, a small group or a satellite-wide shift is
// not.

#include "network/ar/RingFix.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

// The Galileo E1/E5a step on the code's geometry-free combination, 4 lambda5 - 3 lambda1 (m).
constexpr double kL1 = 0.190293672798365;
constexpr double kL5 = 0.254828049;
constexpr double kStep = 4.0 * kL5 - 3.0 * kL1;

std::string satName(int s) { return "E" + std::string(s < 10 ? "0" : "") + std::to_string(s); }

// Five receivers by five satellites, every chord clean (small, distinct deviations), then the given changes.
std::vector<RingFixNearNullIn> grid(int n_rx = 5, int n_sat = 5) {
    std::vector<RingFixNearNullIn> in;
    for (int r = 0; r < n_rx; ++r)
        for (int s = 1; s <= n_sat; ++s) {
            RingFixNearNullIn c;
            c.id = {r, satName(s), "L1C", "L5Q"};
            c.step_m = kStep;
            c.gf_m = 0.01 * ((r * 7 + s * 3) % 5 - 2);
            c.if_m = 0.02 * ((r * 3 + s * 5) % 5 - 2);
            in.push_back(c);
        }
    return in;
}

const RingFixNearNullVerdict* verdictOf(const std::vector<RingFixNearNullVerdict>& v, int station, const std::string& sat) {
    for (const RingFixNearNullVerdict& x : v)
        if (x.id.station == station && x.id.sat == sat) return &x;
    return nullptr;
}

RingFixNearNullIn& chord(std::vector<RingFixNearNullIn>& in, int station, const std::string& sat) {
    for (RingFixNearNullIn& c : in)
        if (c.id.station == station && c.id.sat == sat) return c;
    throw std::runtime_error("no such chord");
}

}  // namespace

TEST(RingFixNearNullGuard, OneStepHighIsFlaggedWithKPlusOne) {
    std::vector<RingFixNearNullIn> in = grid();
    chord(in, 3, "E05").gf_m += kStep;
    const std::vector<RingFixNearNullVerdict> v = ringFixNearNullVerdicts(in, 0.12, 0.30);
    ASSERT_EQ(v.size(), in.size());
    const RingFixNearNullVerdict* x = verdictOf(v, 3, "E05");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->act);
    EXPECT_EQ(x->k, 1);
    EXPECT_NEAR(x->dev_gf_m, kStep, 0.05);
    int acted = 0;
    for (const RingFixNearNullVerdict& y : v) acted += y.act ? 1 : 0;
    EXPECT_EQ(acted, 1);
}

TEST(RingFixNearNullGuard, TwoStepsLowIsFlaggedWithKMinusTwo) {
    std::vector<RingFixNearNullIn> in = grid();
    chord(in, 1, "E02").gf_m -= 2.0 * kStep;
    const std::vector<RingFixNearNullVerdict> v = ringFixNearNullVerdicts(in, 0.12, 0.30);
    const RingFixNearNullVerdict* x = verdictOf(v, 1, "E02");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->act);
    EXPECT_EQ(x->k, -2);
}

TEST(RingFixNearNullGuard, ALargeIonosphereFreeDeviationIsRefused) {
    // A chord's own code bias shows in both combinations: a step-sized geometry-free deviation with 0.6 m
    // ionosphere-free is refused.
    std::vector<RingFixNearNullIn> in = grid();
    chord(in, 2, "E03").gf_m += kStep;
    chord(in, 2, "E03").if_m += 0.60;
    const std::vector<RingFixNearNullVerdict> v = ringFixNearNullVerdicts(in, 0.12, 0.30);
    const RingFixNearNullVerdict* x = verdictOf(v, 2, "E03");
    ASSERT_NE(x, nullptr);
    EXPECT_FALSE(x->act);
    EXPECT_TRUE(x->refused_if);
    EXPECT_EQ(x->k, 1);
}

TEST(RingFixNearNullGuard, HalfAStepOrAnUnfitDeviationIsNotFlagged) {
    std::vector<RingFixNearNullIn> in = grid();
    chord(in, 0, "E01").gf_m += 0.18;   // under half a step (0.224 m), clear of the clean chords' spread: k = 0
    chord(in, 4, "E04").gf_m += 0.30;   // k = 1, but 0.15 m from the step: outside the tolerance
    const std::vector<RingFixNearNullVerdict> v = ringFixNearNullVerdicts(in, 0.12, 0.30);
    const RingFixNearNullVerdict* a = verdictOf(v, 0, "E01");
    const RingFixNearNullVerdict* b = verdictOf(v, 4, "E04");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->k, 0);
    EXPECT_FALSE(a->act);
    EXPECT_EQ(b->k, 1);
    EXPECT_FALSE(b->act);
    EXPECT_FALSE(b->refused_if);
}

TEST(RingFixNearNullGuard, ASatelliteWideShiftIsAGaugeAndNotFlagged) {
    std::vector<RingFixNearNullIn> in = grid();
    for (int r = 0; r < 5; ++r) chord(in, r, "E05").gf_m += kStep;
    for (const RingFixNearNullVerdict& x : ringFixNearNullVerdicts(in, 0.12, 0.30)) EXPECT_FALSE(x.act);
}

TEST(RingFixNearNullGuard, SmallGroupsGetNoVerdict) {
    // Three receivers: every satellite group has 3 chords (fewer than 4), so no chord gets a verdict.
    std::vector<RingFixNearNullIn> in = grid(3, 5);
    chord(in, 1, "E02").gf_m += kStep;
    EXPECT_TRUE(ringFixNearNullVerdicts(in, 0.12, 0.30).empty());
    // A receiver with fewer than 3 grouped chords gets no verdict either.
    std::vector<RingFixNearNullIn> in2 = grid(5, 2);
    EXPECT_TRUE(ringFixNearNullVerdicts(in2, 0.12, 0.30).empty());
}
