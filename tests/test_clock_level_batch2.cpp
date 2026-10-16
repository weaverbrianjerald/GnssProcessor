/**
 * @file test_clock_level_batch2.cpp
 * @brief Card CLV2 (docs/refound/CLV2_clock_level_batch2_2026-09-25.md): the declared
 *        level steps a consumer answers.
 *
 * The emitter accumulates the declared steps (the C and B records' lvl= tokens) in the
 * SSR store, which a consumer differences like a discontinuity counter.  The producer's
 * re-base that wrote them was removed; a replay of a wire it wrote still declares them.
 */
#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace GnssProcessor {

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
