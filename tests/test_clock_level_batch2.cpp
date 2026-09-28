/**
 * @file test_clock_level_batch2.cpp
 * @brief Card CLV2 (docs/refound/CLV2_clock_level_batch2_2026-09-25.md): the free
 *        same-band column's own re-base (R-A2) and the declared level steps a consumer
 *        answers.
 *
 * R-A2 moves a free code-bias column (GPS C1W, Galileo C1X) by its own step, through the
 * same carried references as batch 1's follow share, and a column under its own filter no
 * longer follows the level.  The emitter accumulates the declared steps (the C and B
 * records' lvl= tokens) in the SSR store, which a consumer differences like a
 * discontinuity counter.
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkClockLevel.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace GnssProcessor {
namespace {

CarryPrior prior(SharedParamKind kind, const std::string& sat, const std::string& sig,
                 double value) {
    CarryPrior cp{};
    cp.kind   = kind;
    cp.sat    = sat;
    cp.signal = sig;
    cp.value  = value;
    cp.var    = 1e-4;
    return cp;
}

CoupledShared term(SharedParamKind kind, const std::string& sat, const std::string& sig,
                   double s_prev) {
    CoupledShared cs;
    cs.kind   = kind;
    cs.sat    = sat;
    cs.signal = sig;
    cs.coeff  = 0.5;
    cs.s_prev = s_prev;
    cs.p_prev = 1e-4;
    return cs;
}

}  // namespace

// A direct step moves the column's carried value, its coupling s_prev, its time-linked
// datum reference and its pending B-record declaration, with no level shift at all.
TEST(ClockLevelFreeColumn, DirectStepMovesTheColumnWithoutALevelShift) {
    NetworkCarryState carry;
    carry.prev_carry = {prior(SharedParamKind::Clock, "G01", "", 0.8),
                        prior(SharedParamKind::CodeBias, "G01", "C1C", 1.5),
                        prior(SharedParamKind::CodeBias, "G01", "C1W", 1.9),
                        prior(SharedParamKind::CodeBias, "G02", "C1W", -0.3)};
    LocalCarry rx{0.0, 1e-4, 0.0};
    rx.coupling = {term(SharedParamKind::CodeBias, "G01", "C1W", 1.9)};
    carry.rx_cb_carry[{3, "C1W"}] = rx;
    carry.code_bias_prev_["G|C1W"]["G01"] = 1.9;
    carry.code_bias_prev_["G|C1W"]["G02"] = -0.3;

    const std::map<std::pair<std::string, std::string>, double> direct = {
        {{"G01", "C1W"}, 0.12}, {{"G02", "C1W"}, -0.12}};
    std::map<std::pair<std::string, std::string>, double> moved;
    const long long n = clockLevelShiftCarry(carry, {}, nullptr, &moved, &direct, nullptr);
    EXPECT_EQ(n, 5);   // two carried columns, one coupling term, two datum references
    EXPECT_DOUBLE_EQ(carry.prev_carry[0].value, 0.8) << "no level shift, the clock stays";
    EXPECT_DOUBLE_EQ(carry.prev_carry[1].value, 1.5) << "the pinned signal stays";
    EXPECT_DOUBLE_EQ(carry.prev_carry[2].value, 1.9 + 0.12);
    EXPECT_DOUBLE_EQ(carry.prev_carry[3].value, -0.3 - 0.12);
    EXPECT_DOUBLE_EQ(carry.rx_cb_carry.at({3, "C1W"}).coupling[0].s_prev, 1.9 + 0.12);
    EXPECT_DOUBLE_EQ(carry.code_bias_prev_.at("G|C1W").at("G01"), 1.9 + 0.12);
    EXPECT_DOUBLE_EQ(carry.code_bias_prev_.at("G|C1W").at("G02"), -0.3 - 0.12);
    EXPECT_DOUBLE_EQ(carry.clk_level.sat.at("G01").cb_pending.at("C1W"), 0.12);
    EXPECT_DOUBLE_EQ(carry.clk_level.sat.at("G02").cb_pending.at("C1W"), -0.12);
    EXPECT_EQ(moved.size(), 2u);
}

// A column under its own filter (held) does not follow the level; an unheld column still
// takes the follow share; a held column with a direct step takes the step alone.
TEST(ClockLevelFreeColumn, HeldColumnDoesNotFollowTheLevel) {
    NetworkCarryState carry;
    carry.prev_carry = {prior(SharedParamKind::Clock, "G01", "", 0.0),
                        prior(SharedParamKind::Clock, "G02", "", 0.0),
                        prior(SharedParamKind::CodeBias, "G01", "C1W", 1.0),
                        prior(SharedParamKind::CodeBias, "G02", "C1W", 2.0)};
    carry.code_bias_prev_["G|C1W"]["G01"] = 1.0;
    carry.code_bias_prev_["G|C1W"]["G02"] = 2.0;
    const double d = 0.5, share = 0.4;
    std::map<std::string, ClockLevelCbFollow> follow;
    follow["G01"] = ClockLevelCbFollow{share, "C1C", "C2W"};
    follow["G02"] = ClockLevelCbFollow{share, "C1C", "C2W"};
    const std::map<std::pair<std::string, std::string>, double> direct = {
        {{"G01", "C1W"}, 0.07}};
    const std::set<std::pair<std::string, std::string>> held = {{"G01", "C1W"}};
    std::map<std::pair<std::string, std::string>, double> moved;
    clockLevelShiftCarry(carry, {{"G01", d}, {"G02", -d}}, &follow, &moved, &direct, &held);
    EXPECT_DOUBLE_EQ(carry.prev_carry[0].value, d);
    EXPECT_DOUBLE_EQ(carry.prev_carry[1].value, -d);
    EXPECT_DOUBLE_EQ(carry.prev_carry[2].value, 1.0 + 0.07) << "held: its own step, no follow";
    EXPECT_DOUBLE_EQ(carry.prev_carry[3].value, 2.0 - share * d) << "unheld: the follow";
    EXPECT_DOUBLE_EQ(moved.at({"G01", "C1W"}), 0.07);
    EXPECT_DOUBLE_EQ(moved.at({"G02", "C1W"}), -share * d);

    // With neither a shift nor a direct step nothing moves.
    NetworkCarryState c2;
    c2.prev_carry = {prior(SharedParamKind::CodeBias, "G01", "C1W", 1.0)};
    const std::map<std::pair<std::string, std::string>, double> none;
    EXPECT_EQ(clockLevelShiftCarry(c2, {}, nullptr, nullptr, &none, &held), 0);
    EXPECT_DOUBLE_EQ(c2.prev_carry[0].value, 1.0);
}

// The store accumulates each satellite's declared steps and forgets them on clear().
TEST(DeclaredLevelSteps, StoreAccumulatesPerSatelliteAndSignal) {
    SsrCorrectionStore store;
    EXPECT_FALSE(store.getDeclaredLevelSteps("G05").has_value());
    store.addDeclaredLevelSteps("G05", 0.30, {});
    store.addDeclaredLevelSteps("G05", -0.10, {{"C1W", 0.05}});
    store.addDeclaredLevelSteps("G05", 0.0, {{"C1W", 0.02}, {"C1X", 1.0}});
    const auto d = store.getDeclaredLevelSteps("G05");
    ASSERT_TRUE(d.has_value());
    EXPECT_DOUBLE_EQ(d->clock_m, 0.20);
    EXPECT_DOUBLE_EQ(d->bias_m.at("C1W"), 0.07);
    EXPECT_DOUBLE_EQ(d->bias_m.at("C1X"), 1.0);
    EXPECT_EQ(d->n, 3);
    EXPECT_FALSE(store.getDeclaredLevelSteps("G06").has_value());
    store.clear();
    EXPECT_FALSE(store.getDeclaredLevelSteps("G05").has_value());
}

// The emitter hands the store exactly the declared steps of an epoch, and nothing for a
// satellite without one (an unarmed wire leaves the store untouched).
TEST(DeclaredLevelSteps, EmitterFeedsTheStoreOnlyDeclaredSteps) {
    NetworkProductEmitter emitter;
    SsrCorrectionStore store;
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{2313, 259230.0};
    NetworkSatSolution a;
    a.sat = "G05";
    a.has_clock = true;
    a.clock_c0_m = 1.0;
    a.level_step_m = 0.25;
    a.cb_level_step_m["C1W"] = 0.04;
    NetworkSatSolution b;
    b.sat = "G07";
    b.has_clock = true;
    b.clock_c0_m = 2.0;
    sol.sats = {a, b};
    emitter.emitToStore(sol, store);
    const auto d = store.getDeclaredLevelSteps("G05");
    ASSERT_TRUE(d.has_value());
    EXPECT_DOUBLE_EQ(d->clock_m, 0.25);
    EXPECT_DOUBLE_EQ(d->bias_m.at("C1W"), 0.04);
    EXPECT_FALSE(store.getDeclaredLevelSteps("G07").has_value());
}

}  // namespace GnssProcessor
