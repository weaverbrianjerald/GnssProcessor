/**
 * @file test_ring_fix_window.cpp
 * @brief The windowed residual monitor of held ring-fix links (network.ring_fix.window_monitor; measure only).
 *
 * RingFixWindow: the receiver classes; the residual at the trace's resolution; the reference d; the class scale
 * without the tested link and its count rule; the trip epoch of each fixed form (O2b, O1, O2a) on a constant residual;
 * the scaled sums' trip, the frozen scale during an episode and the unscaled count; the closes at the hold's end and at
 * a new hold; the station share.
 */
#include "network/ar/RingFix.hpp"
#include "network/ar/RingFixWindow.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kE1 = 0.19029367279836487;   // Galileo E1 (m)
constexpr double kE5a = 0.25482804879085386;  // Galileo E5a (m)

RingFixWindowObs obs(int station, const std::string& sat, double r, bool have_r = true, double hold_s = 0.0) {
    RingFixWindowObs o;
    o.id = RingFixLinkId{station, sat, "L1C", "L5Q"};
    o.n1 = 5;
    o.n2 = 3;
    o.hold_s = hold_s;
    o.lambda1 = kE1;
    o.lambda2 = kE5a;
    o.have_r = have_r;
    o.r = r;
    return o;
}

/// The epoch of the first open edge of @p form for a single link with a constant residual, or -1.
int firstOpen(RingFixWindowForm form, double r, int epochs) {
    RingFixWindowMonitor W;
    W.rx_class = {"SIM"};
    for (int k = 0; k < epochs; ++k) {
        const std::vector<RingFixWindowEdge> e = ringFixWindowStep(W, {obs(0, "E11", r)}, 30.0 * k, 30.0, nullptr);
        for (const RingFixWindowEdge& x : e)
            if (x.open && x.form == form) return k;
    }
    return -1;
}

}  // namespace

TEST(RingFixWindow, ReceiverClassesKeepATypeSharedByThreeElseTheManufacturer) {
    const std::vector<std::string> c =
        ringFixWindowRxClasses({"SEPT POLARX5", "SEPT POLARX5", "SEPT POLARX5", "TRIMBLE ALLOY", "", "TRIMBLE NETR9"});
    EXPECT_EQ(c, (std::vector<std::string>{"SEPT POLARX5", "SEPT POLARX5", "SEPT POLARX5", "TRIMBLE", "unknown",
                                           "TRIMBLE"}));
}

TEST(RingFixWindow, ResolutionAndReference) {
    EXPECT_DOUBLE_EQ(ringFixWindowResolution(0.0123456), 0.01235);
    EXPECT_DOUBLE_EQ(ringFixWindowResolution(-0.0071249), -0.00712);
    // Half of 13.5/108.94 of the Galileo E1/E5a footprint (108.94 mm): 6.75 mm.
    EXPECT_NEAR(ringFixWindowReference(kE1, kE5a), 0.00675, 2e-6);
}

TEST(RingFixWindow, ClassScaleLeavesOutTheTestedLinkAndNeedsThreeOthers) {
    std::deque<std::tuple<double, int, double>> buf;
    std::map<int, int> mem;
    const double vals[3][3] = {{0.001, -0.001, 0.002}, {0.000, 0.003, -0.002}, {-0.001, 0.001, 0.000}};
    for (int link = 0; link < 3; ++link)
        for (double v : vals[link]) { buf.emplace_back(0.0, link, v); ++mem[link]; }
    for (int k = 0; k < 3; ++k) { buf.emplace_back(0.0, 9, 1.0); ++mem[9]; }   // the tested link: large
    double s = 0.0;
    int n = 0;
    ASSERT_TRUE(ringFixWindowScale(buf, mem, 9, s, n));
    EXPECT_EQ(n, 3);
    // The others' nine values: median 0.000; |deviations| median 0.001; scale 1.4826 mm.
    EXPECT_NEAR(s, 1.4826e-3, 1e-12);
    // Link 0 tested: the others are links 1, 2 and 9 (three): formed; with link 9 gone, two: not formed.
    EXPECT_TRUE(ringFixWindowScale(buf, mem, 0, s, n));
    mem.erase(9);
    std::deque<std::tuple<double, int, double>> buf2;
    for (const auto& e : buf) if (std::get<1>(e) != 9) buf2.push_back(e);
    EXPECT_FALSE(ringFixWindowScale(buf2, mem, 0, s, n));
    EXPECT_EQ(n, 2);
}

TEST(RingFixWindow, FixedFormsTripAtTheirEpochOnAConstantResidual) {
    // O2b: S+ grows by r - k = 4.5 mm an epoch and passes 0.4 m-epochs at the 89th row (index 88).
    EXPECT_EQ(firstOpen(RingFixWindowForm::O2b, 0.0105, 200), 88);
    EXPECT_EQ(firstOpen(RingFixWindowForm::O2b, 0.006, 200), -1);
    // O2a: the 40th consecutive row at 4 mm or more (index 39); 3.9 mm never.
    EXPECT_EQ(firstOpen(RingFixWindowForm::O2a, 0.005, 100), 39);
    EXPECT_EQ(firstOpen(RingFixWindowForm::O2a, 0.0039, 100), -1);
    // O1: valid once the hold spans 40 min less one epoch (row 80, index 79, 80 rows in the window).
    EXPECT_EQ(firstOpen(RingFixWindowForm::O1, 0.009, 200), 79);
    EXPECT_EQ(firstOpen(RingFixWindowForm::O1, 0.0079, 200), -1);
}

TEST(RingFixWindow, ScaledSumsTripFreezeTheScaleAndCountUnscaled) {
    RingFixWindowMonitor W;
    W.rx_class = {"SIM"};
    // Four links of one station class: three at +1, -1 and 0 mm, the first two swapping each epoch (scale 1.4826 mm),
    // the fourth at 20 mm.
    int open_epoch = -1;
    double scale_at_open = 0.0;
    for (int k = 0; k < 60; ++k) {
        const double a = (k % 2 == 0) ? 0.001 : -0.001;
        std::vector<RingFixWindowObs> held = {obs(0, "E01", a), obs(0, "E02", -a), obs(0, "E03", 0.0),
                                              obs(0, "E04", 0.020)};
        std::vector<RingFixWindowRow> rows;
        const std::vector<RingFixWindowEdge> e = ringFixWindowStep(W, held, 30.0 * k, 30.0, &rows);
        for (const RingFixWindowEdge& x : e)
            if (x.open && x.form == RingFixWindowForm::ScaledStation && x.id.sat == "E04" && open_epoch < 0) {
                open_epoch = k;
                scale_at_open = x.scale;
                EXPECT_EQ(x.scale_n, 3);
            }
        if (open_epoch >= 0 && k > open_epoch) {
            // Frozen: the class is in an episode, so every link keeps its scale.
            EXPECT_DOUBLE_EQ(rows[3].scale[0], scale_at_open);
        }
    }
    ASSERT_GE(open_epoch, 0);
    // (20 - 6.75) mm / 1.4826 mm = 8.94 scales an epoch: above 95 at the 11th row (index 10).
    EXPECT_EQ(open_epoch, 10);
    // The network class has the same four links (one receiver class): formed for all of them.
    EXPECT_EQ(W.unscaled[0], 0);
    // A lone link of another station has no station class (unscaled) but joins the network class.
    ringFixWindowStep(W, {obs(1, "E05", 0.0), obs(0, "E01", 0.0), obs(0, "E02", 0.0), obs(0, "E03", 0.0),
                          obs(0, "E04", 0.0)}, 1800.0, 30.0, nullptr);
    EXPECT_EQ(W.unscaled[0], 1);
}

TEST(RingFixWindow, EpisodesCloseAtTheHoldsEndAndAtANewHold) {
    RingFixWindowMonitor W;
    W.rx_class = {"SIM"};
    for (int k = 0; k < 45; ++k) ringFixWindowStep(W, {obs(0, "E11", 0.005)}, 30.0 * k, 30.0, nullptr);
    ASSERT_EQ(W.opened[static_cast<std::size_t>(RingFixWindowForm::O2a)], 1);
    // A new hold of the link (another decision time): its open episodes close with cause new_hold, its sums restart.
    std::vector<RingFixWindowEdge> e = ringFixWindowStep(W, {obs(0, "E11", 0.0, true, 1350.0)}, 1350.0, 30.0, nullptr);
    ASSERT_FALSE(e.empty());
    EXPECT_FALSE(e[0].open);
    EXPECT_STREQ(e[0].cause, "new_hold");
    EXPECT_EQ(W.links.at(e[0].id).o2a_run, 0);
    // Not held: the link is dropped; a second episode closed by "end".
    for (int k = 46; k < 92; ++k) ringFixWindowStep(W, {obs(0, "E11", 0.005, true, 1350.0)}, 30.0 * k, 30.0, nullptr);
    e = ringFixWindowStep(W, {}, 30.0 * 92, 30.0, nullptr);
    ASSERT_FALSE(e.empty());
    EXPECT_STREQ(e[0].cause, "end");
    EXPECT_TRUE(W.links.empty());
    long long opened = 0, closed = 0;
    for (std::size_t f = 0; f < kRingFixWindowFormCount; ++f) { opened += W.opened[f]; closed += W.closed[f]; }
    EXPECT_EQ(opened, closed);
}

TEST(RingFixWindow, StationShareCountsNeighboursWithEnoughOverlap) {
    RingFixWindowMonitor W;
    W.rx_class = {"SIM", "SIM"};
    std::vector<RingFixWindowRow> rows;
    for (int k = 0; k < 70; ++k) {
        // At station 0: E11 is tested; E01 and E02 sit at 5 mm, E03 at 1 mm; E04 joins late (under 30 min).
        std::vector<RingFixWindowObs> held = {obs(0, "E01", 0.005), obs(0, "E02", -0.005), obs(0, "E03", 0.001),
                                              obs(0, "E11", 0.0), obs(1, "E11", 0.006)};
        if (k >= 40) held.push_back(obs(0, "E04", 0.010, true, 1200.0));
        ringFixWindowStep(W, held, 30.0 * k, 30.0, &rows);
    }
    // rows[3] is station 0's E11: neighbours with 60 rows or more: E01, E02, E03 (E04 has 30); two at 4 mm or more.
    EXPECT_EQ(rows[3].n_sta, 3);
    EXPECT_NEAR(rows[3].share_sta, 2.0 / 3.0, 1e-12);
    // Its satellite neighbour: E11 at station 1 (6 mm): share 1.
    EXPECT_EQ(rows[3].n_sat, 1);
    EXPECT_DOUBLE_EQ(rows[3].share_sat, 1.0);
}
