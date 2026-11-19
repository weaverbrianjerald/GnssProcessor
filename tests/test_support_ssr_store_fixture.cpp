/**
 * @file test_support_ssr_store_fixture.cpp
 * @brief The shared SSR correction-store fixture (tests/support/SsrStoreFixture.hpp) puts the
 *        store into the state it names, and each of its steps moves exactly one quantity.
 *
 * Later user-engine tests (reactions of the real-time engine to counter, flag, generation and
 * age changes; the fixing functions) rely on the fixture to say what the store holds.  A
 * fixture that wrote a different value, moved a neighbouring signal along with the addressed
 * one or failed to remove a record would make every one of those tests check the wrong thing,
 * so each builder and each step is read back here through the store's own readers
 * (getOrbitClock, getCodeBias, hasCodeBias, getCodeBiasDiscontinuity, getClockDiscontinuity,
 * getPhaseBias, getPhaseBiasTime, getGenerationId, getSsrEpoch, getSsrIod, coverageFor,
 * censusSummary).  The expected numbers follow from the fixture's deterministic value rules
 * (PRN 5 and PRN 11 below) and from the store's freshness gate (an age equal to the limit is
 * fresh, one second more is stale).
 */

// The header under test comes first, so this file also proves it compiles on its own.
#include "support/SsrStoreFixture.hpp"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "products/SsrCorrectionStore.hpp"
#include "rtcm/RtcmTypes.hpp"

#include <string>
#include <vector>

using namespace GnssProcessor;
using namespace gnss_test;

namespace {

constexpr double kMaxAge = 60.0;
constexpr double kTol    = 1e-12;
const std::vector<std::string> kSats{"G05", "E11"};

// The builder writes one orbit and one network clock per satellite with the values of the
// deterministic rules, both applied on their own (a network clock needs no orbit of its IOD-SSR).
TEST(SupportSsrStoreFixture, NetworkStateWritesOrbitAndClock) {
    const SsrStoreState st = networkSsrState(kSats);
    SsrCorrectionStore store;
    writeSsrState(store, st);

    EXPECT_EQ(store.size(), 2u);
    const auto g = store.getOrbitClock("G05", st.epoch, kMaxAge);
    ASSERT_TRUE(g.has_value());
    EXPECT_TRUE(g->has_orbit);
    EXPECT_TRUE(g->has_clock);
    EXPECT_NEAR(g->c0, 1.5, kTol);
    EXPECT_NEAR(g->delta_rac.x(), 0.05, kTol);
    EXPECT_NEAR(g->delta_rac.y(), 0.10, kTol);
    EXPECT_NEAR(g->delta_rac.z(), -0.025, kTol);
    EXPECT_EQ(g->iode, -1);
    EXPECT_EQ(g->epoch_time, st.epoch);
    const auto e = store.getOrbitClock("E11", st.epoch, kMaxAge);
    ASSERT_TRUE(e.has_value());
    EXPECT_NEAR(e->c0, 2.1, kTol);

    EXPECT_EQ(store.getSsrIod("G05"), 1);
    ASSERT_TRUE(store.getSsrEpoch("G05").has_value());
    EXPECT_EQ(*store.getSsrEpoch("G05"), st.epoch);
    EXPECT_EQ(store.getClockDiscontinuity("G05"), std::optional<int>(0));
    EXPECT_FALSE(store.getGenerationId().has_value());

    const auto cov = store.coverageFor(kSats, st.epoch, kMaxAge);
    EXPECT_EQ(cov.total, 2);
    EXPECT_EQ(cov.orbit, 2);
    EXPECT_EQ(cov.clock, 2);
    EXPECT_EQ(cov.code_bias, 2);
    EXPECT_EQ(cov.phase_bias, 2);
    EXPECT_EQ(store.censusSummary(), "E[o1 c1 cb1 pb1] G[o1 c1 cb1 pb1]");
}

// Code biases per signal of the constellation's default pair, with tracked counters at 0.
TEST(SupportSsrStoreFixture, NetworkStateWritesCodeBiasesWithCounters) {
    const SsrStoreState st = networkSsrState(kSats);
    SsrCorrectionStore store;
    writeSsrState(store, st);

    EXPECT_NEAR(store.getCodeBias("G05", "C1C"), 0.30, kTol);
    EXPECT_NEAR(store.getCodeBias("G05", "C2W"), 0.80, kTol);
    EXPECT_NEAR(store.getCodeBias("E11", "C1C"), 0.36, kTol);
    EXPECT_NEAR(store.getCodeBias("E11", "C5Q"), 0.86, kTol);
    EXPECT_TRUE(store.hasCodeBias("E11", "C5Q", st.epoch, kMaxAge));
    EXPECT_FALSE(store.hasCodeBias("E11", "C2W"));
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G05", "C1C"), std::optional<int>(0));
    EXPECT_EQ(store.getCodeBiasDiscontinuity("E11", "C5Q", st.epoch, kMaxAge), std::optional<int>(0));
}

// Phase biases carry both integer flags set, counters at 0, no hold, stamped at the epoch.
TEST(SupportSsrStoreFixture, NetworkStateWritesIntegerPhaseBiases) {
    const SsrStoreState st = networkSsrState(kSats);
    SsrCorrectionStore store;
    writeSsrState(store, st);

    const auto l1 = store.getPhaseBias("G05", "L1C", st.epoch, kMaxAge);
    ASSERT_TRUE(l1.has_value());
    EXPECT_NEAR(l1->bias_m, 0.035, kTol);
    EXPECT_TRUE(l1->signal_integer);
    EXPECT_EQ(l1->wide_lane_integer, 1);
    EXPECT_EQ(l1->discontinuity_counter, 0);
    EXPECT_EQ(l1->hold_generation, 0u);
    const auto l2 = store.getPhaseBias("G05", "L2W");
    ASSERT_TRUE(l2.has_value());
    EXPECT_NEAR(l2->bias_m, 0.055, kTol);
    const auto e5 = store.getPhaseBias("E11", "L5Q");
    ASSERT_TRUE(e5.has_value());
    EXPECT_NEAR(e5->bias_m, 0.061, kTol);
    EXPECT_FALSE(store.getPhaseBias("E11", "L2W").has_value());
    ASSERT_TRUE(store.getPhaseBiasTime("G05").has_value());
    EXPECT_EQ(*store.getPhaseBiasTime("G05"), st.epoch);
}

// The float builder clears both flags on every phase signal and keeps the biases.
TEST(SupportSsrStoreFixture, FloatStateClearsBothIntegerFlags) {
    SsrCorrectionStore store;
    writeSsrState(store, floatSsrState(kSats));
    for (const auto& [sat, sig] : std::vector<std::pair<std::string, std::string>>{
             {"G05", "L1C"}, {"G05", "L2W"}, {"E11", "L1C"}, {"E11", "L5Q"}}) {
        const auto pb = store.getPhaseBias(sat, sig);
        ASSERT_TRUE(pb.has_value()) << sat << " " << sig;
        EXPECT_FALSE(pb->signal_integer) << sat << " " << sig;
        EXPECT_EQ(pb->wide_lane_integer, 0) << sat << " " << sig;
    }
    EXPECT_NEAR(store.getPhaseBias("G05", "L1C")->bias_m, 0.035, kTol);
}

// The held builder stamps the hold generation on every phase signal and the generation id on
// the store.
TEST(SupportSsrStoreFixture, HeldStateCarriesHoldAndGeneration) {
    SsrCorrectionStore store;
    writeSsrState(store, heldSsrState(kSats, 3, 4));
    EXPECT_EQ(store.getGenerationId(), std::optional<uint32_t>(3u));
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->hold_generation, 4u);
    EXPECT_EQ(store.getPhaseBias("E11", "L5Q")->hold_generation, 4u);
}

// The decoded builder reproduces what a wire stream can carry: no code counters, no generation,
// orbits on a broadcast IODE, and clocks that wait for their orbit.
TEST(SupportSsrStoreFixture, DecodedStateMatchesWireLimits) {
    SsrStoreState st = decodedSsrState(kSats, 50);
    SsrCorrectionStore store;
    writeSsrState(store, st);

    EXPECT_FALSE(store.getCodeBiasDiscontinuity("G05", "C1C").has_value());
    EXPECT_NEAR(store.getCodeBias("G05", "C1C"), 0.30, kTol);
    EXPECT_FALSE(store.getGenerationId().has_value());
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->wide_lane_integer, 0);
    EXPECT_TRUE(store.getPhaseBias("G05", "L1C")->signal_integer);
    const auto oc = store.getOrbitClock("G05", st.epoch, kMaxAge);
    ASSERT_TRUE(oc.has_value());
    EXPECT_TRUE(oc->has_clock);
    EXPECT_EQ(oc->iode, 50);
    EXPECT_EQ(oc->clock_iode, 50);

    // Without its orbit the decoded clock is withheld, so the satellite has no correction.
    st.sats.at("G05").has_orbit = false;
    writeSsrState(store, st);
    EXPECT_FALSE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
    EXPECT_TRUE(store.getOrbitClock("E11", st.epoch, kMaxAge).has_value());
}

// The empty builder leaves the store empty, and writing it clears earlier content.
TEST(SupportSsrStoreFixture, EmptyStateClearsTheStore) {
    SsrCorrectionStore store;
    writeSsrState(store, networkSsrState(kSats, 9));
    writeSsrState(store, emptySsrState());
    EXPECT_TRUE(store.isEmpty());
    EXPECT_FALSE(store.getGenerationId().has_value());
    EXPECT_FALSE(store.newestSsrEpoch().has_value());
}

// Each discontinuity step moves only its own counter.
TEST(SupportSsrStoreFixture, DiscontinuityStepsMoveOneCounter) {
    SsrStoreState st = networkSsrState(kSats);
    SsrCorrectionStore store;

    EXPECT_EQ(bumpPhaseDiscontinuity(st, "G05", "L2W"), 1);
    EXPECT_EQ(bumpCodeDiscontinuity(st, "E11", "C5Q"), 1);
    EXPECT_EQ(bumpClockDiscontinuity(st, "G05"), 1);
    EXPECT_EQ(bumpClockDiscontinuity(st, "G05"), 2);
    writeSsrState(store, st);

    EXPECT_EQ(store.getPhaseBias("G05", "L2W")->discontinuity_counter, 1);
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->discontinuity_counter, 0);
    EXPECT_EQ(store.getPhaseBias("E11", "L5Q")->discontinuity_counter, 0);
    EXPECT_EQ(store.getCodeBiasDiscontinuity("E11", "C5Q"), std::optional<int>(1));
    EXPECT_EQ(store.getCodeBiasDiscontinuity("E11", "C1C"), std::optional<int>(0));
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G05", "C1C"), std::optional<int>(0));
    EXPECT_EQ(store.getClockDiscontinuity("G05"), std::optional<int>(2));
    EXPECT_EQ(store.getClockDiscontinuity("E11"), std::optional<int>(0));
}

// An untracked code counter (a decoded stream) becomes tracked at 1 on its first bump.
TEST(SupportSsrStoreFixture, UntrackedCodeCounterStartsAtOne) {
    SsrStoreState st = decodedSsrState({"G05"});
    EXPECT_EQ(bumpCodeDiscontinuity(st, "G05", "C2W"), 1);
    SsrCorrectionStore store;
    writeSsrState(store, st);
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G05", "C2W"), std::optional<int>(1));
    EXPECT_FALSE(store.getCodeBiasDiscontinuity("G05", "C1C").has_value());
}

// The flag steps change one entry's flag and nothing else.
TEST(SupportSsrStoreFixture, FlagStepsChangeOneEntry) {
    SsrStoreState st = networkSsrState(kSats);
    EXPECT_FALSE(flipSignalInteger(st, "G05", "L1C"));
    EXPECT_EQ(flipWideLaneInteger(st, "E11", "L5Q"), 0);
    EXPECT_EQ(setWideLaneInteger(st, "G05", "L2W", 2), 2);
    SsrCorrectionStore store;
    writeSsrState(store, st);

    EXPECT_FALSE(store.getPhaseBias("G05", "L1C")->signal_integer);
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->wide_lane_integer, 1);
    EXPECT_TRUE(store.getPhaseBias("G05", "L2W")->signal_integer);
    EXPECT_EQ(store.getPhaseBias("G05", "L2W")->wide_lane_integer, 2);
    EXPECT_EQ(store.getPhaseBias("E11", "L5Q")->wide_lane_integer, 0);
    EXPECT_TRUE(store.getPhaseBias("E11", "L5Q")->signal_integer);
    EXPECT_EQ(store.getPhaseBias("E11", "L1C")->wide_lane_integer, 1);

    // Flipping back restores the flag.
    EXPECT_TRUE(flipSignalInteger(st, "G05", "L1C"));
    writeSsrState(store, st);
    EXPECT_TRUE(store.getPhaseBias("G05", "L1C")->signal_integer);
}

// The generation steps reach the store's generation id, and 0 reads as none.
TEST(SupportSsrStoreFixture, GenerationSteps) {
    SsrStoreState st = networkSsrState(kSats, 7);
    SsrCorrectionStore store;
    writeSsrState(store, st);
    EXPECT_EQ(store.getGenerationId(), std::optional<uint32_t>(7u));
    EXPECT_EQ(bumpGeneration(st), 8u);
    writeSsrState(store, st);
    EXPECT_EQ(store.getGenerationId(), std::optional<uint32_t>(8u));
    setGeneration(st, 0);
    writeSsrState(store, st);
    EXPECT_FALSE(store.getGenerationId().has_value());
}

// The hold-generation steps move one signal's hold and leave the others unheld.
TEST(SupportSsrStoreFixture, HoldGenerationSteps) {
    SsrStoreState st = networkSsrState(kSats);
    EXPECT_EQ(bumpHoldGeneration(st, "G05", "L1C"), 1u);
    EXPECT_EQ(setHoldGeneration(st, "E11", "L5Q", 6), 6u);
    SsrCorrectionStore store;
    writeSsrState(store, st);
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->hold_generation, 1u);
    EXPECT_EQ(store.getPhaseBias("G05", "L2W")->hold_generation, 0u);
    EXPECT_EQ(store.getPhaseBias("E11", "L5Q")->hold_generation, 6u);
    EXPECT_EQ(store.getPhaseBias("E11", "L1C")->hold_generation, 0u);
}

// A value step moves the bias and leaves the counter and flags alone (an undeclared step).
TEST(SupportSsrStoreFixture, PhaseBiasValueStepKeepsCounters) {
    SsrStoreState st = networkSsrState(kSats);
    EXPECT_NEAR(stepPhaseBias(st, "G05", "L1C", 0.30), 0.335, kTol);
    SsrCorrectionStore store;
    writeSsrState(store, st);
    const auto pb = store.getPhaseBias("G05", "L1C");
    ASSERT_TRUE(pb.has_value());
    EXPECT_NEAR(pb->bias_m, 0.335, kTol);
    EXPECT_EQ(pb->discontinuity_counter, 0);
    EXPECT_TRUE(pb->signal_integer);
    EXPECT_NEAR(store.getPhaseBias("G05", "L2W")->bias_m, 0.055, kTol);
}

// Aging a satellite stamps its records that many seconds before the epoch: fresh at the limit,
// stale one second past it for the gated readers, still present for the ungated ones.
TEST(SupportSsrStoreFixture, AgeStepMakesRecordsStale) {
    SsrStoreState st = networkSsrState(kSats);
    SsrCorrectionStore store;

    ageSatellite(st, "G05", 60.0);
    writeSsrState(store, st);
    EXPECT_TRUE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
    EXPECT_TRUE(store.getPhaseBias("G05", "L1C", st.epoch, kMaxAge).has_value());

    ageSatellite(st, "G05", 61.0);
    writeSsrState(store, st);
    EXPECT_FALSE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
    EXPECT_FALSE(store.getPhaseBias("G05", "L1C", st.epoch, kMaxAge).has_value());
    EXPECT_EQ(store.getCodeBias("G05", "C1C", st.epoch, kMaxAge), 0.0);
    EXPECT_FALSE(store.hasCodeBias("G05", "C1C", st.epoch, kMaxAge));
    EXPECT_TRUE(store.getPhaseBias("G05", "L1C").has_value());
    EXPECT_NEAR(store.getCodeBias("G05", "C1C"), 0.30, kTol);
    EXPECT_EQ(*store.getPhaseBiasTime("G05"), ssrShift(st.epoch, -61.0));
    EXPECT_EQ(*store.getSsrEpoch("G05"), ssrShift(st.epoch, -61.0));
    EXPECT_EQ(*store.newestSsrEpoch(), st.epoch);   // E11 stays fresh

    const auto cov = store.coverageFor(kSats, st.epoch, kMaxAge);
    EXPECT_EQ(cov.orbit, 1);
    EXPECT_EQ(cov.clock, 1);
    EXPECT_EQ(cov.code_bias, 1);
    EXPECT_EQ(cov.phase_bias, 1);
}

// Aging one record kind leaves the others fresh.
TEST(SupportSsrStoreFixture, AgeStepByRecordKind) {
    SsrStoreState st = networkSsrState({"G05"});
    ageSatellite(st, "G05", 120.0, SsrRecordKind::PhaseBias);
    SsrCorrectionStore store;
    writeSsrState(store, st);
    EXPECT_TRUE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
    EXPECT_TRUE(store.hasCodeBias("G05", "C1C", st.epoch, kMaxAge));
    EXPECT_FALSE(store.getPhaseBias("G05", "L1C", st.epoch, kMaxAge).has_value());

    ageSatellite(st, "G05", 0.0);
    ageSatellite(st, "G05", 90.0, SsrRecordKind::OrbitClock);
    writeSsrState(store, st);
    EXPECT_FALSE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
    EXPECT_TRUE(store.hasCodeBias("G05", "C1C", st.epoch, kMaxAge));
    EXPECT_TRUE(store.getPhaseBias("G05", "L1C", st.epoch, kMaxAge).has_value());
}

// Withdrawing removes every record of the satellite; restoring brings it back with the counters
// it had when it left.
TEST(SupportSsrStoreFixture, WithdrawAndRestoreSatellite) {
    SsrStoreState st = networkSsrState(kSats);
    bumpPhaseDiscontinuity(st, "G05", "L1C");
    bumpClockDiscontinuity(st, "G05");
    SsrCorrectionStore store;

    ASSERT_TRUE(withdrawSatellite(st, "G05"));
    writeSsrState(store, st);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_FALSE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
    EXPECT_FALSE(store.getPhaseBias("G05", "L1C").has_value());
    EXPECT_FALSE(store.hasCodeBias("G05", "C1C"));
    EXPECT_FALSE(store.getClockDiscontinuity("G05").has_value());
    EXPECT_EQ(store.getSsrIod("G05"), -1);
    EXPECT_TRUE(store.getOrbitClock("E11", st.epoch, kMaxAge).has_value());

    ASSERT_TRUE(restoreSatellite(st, "G05"));
    writeSsrState(store, st);
    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->discontinuity_counter, 1);
    EXPECT_EQ(store.getClockDiscontinuity("G05"), std::optional<int>(1));
    EXPECT_TRUE(store.getOrbitClock("G05", st.epoch, kMaxAge).has_value());
}

// Advancing the epoch re-stamps every record at the new epoch with its age kept.
TEST(SupportSsrStoreFixture, AdvanceEpochRestampsRecords) {
    SsrStoreState st = networkSsrState(kSats);
    ageSatellite(st, "E11", 10.0, SsrRecordKind::PhaseBias);
    const GnssTime t0 = st.epoch;
    EXPECT_EQ(advanceSsrEpoch(st, 30.0), ssrShift(t0, 30.0));
    SsrCorrectionStore store;
    writeSsrState(store, st);
    EXPECT_EQ(*store.getPhaseBiasTime("G05"), ssrShift(t0, 30.0));
    EXPECT_EQ(*store.getPhaseBiasTime("E11"), ssrShift(t0, 20.0));
    EXPECT_EQ(*store.newestSsrEpoch(), ssrShift(t0, 30.0));
    EXPECT_EQ(store.getOrbitClock("G05", st.epoch, kMaxAge)->epoch_time, ssrShift(t0, 30.0));
}

// The time shift crosses week boundaries in both directions, and a record aged across one is
// still graded by its true age.
TEST(SupportSsrStoreFixture, ShiftCrossesWeekBoundary) {
    const GnssTime fwd = ssrShift(GnssTime{2300, 604790.0}, 20.0);
    EXPECT_EQ(fwd.week, 2301);
    EXPECT_DOUBLE_EQ(fwd.tow, 10.0);
    const GnssTime back = ssrShift(GnssTime{2301, 5.0}, -10.0);
    EXPECT_EQ(back.week, 2300);
    EXPECT_DOUBLE_EQ(back.tow, 604795.0);

    SsrStoreState st = networkSsrState({"G05"}, 0, GnssTime{2301, 30.0});
    ageSatellite(st, "G05", 45.0);
    SsrCorrectionStore store;
    writeSsrState(store, st);
    EXPECT_EQ(*store.getPhaseBiasTime("G05"), (GnssTime{2300, 604785.0}));
    EXPECT_TRUE(store.getPhaseBias("G05", "L1C", st.epoch, kMaxAge).has_value());
    EXPECT_FALSE(store.getPhaseBias("G05", "L1C", st.epoch, 44.0).has_value());
}

// Writing one satellite overwrites its records and keeps the store's diagnostics; writing the
// whole state clears the diagnostics and keeps the configuration.
TEST(SupportSsrStoreFixture, SatelliteWriteKeepsDiagnosticsStateWriteClears) {
    SsrStoreState st = networkSsrState(kSats, 5);
    SsrCorrectionStore store;
    store.setCodeBiasBandFallback(true);
    writeSsrState(store, st);

    // A C1X lookup next to a C1C bias is an attribute mismatch the store counts.
    store.getCodeBias("G05", "C1X");
    ASSERT_EQ(store.biasAttributeMismatchCounts().count("G05|C1X|C"), 1u);

    bumpPhaseDiscontinuity(st, "G05", "L1C");
    ASSERT_TRUE(writeSsrSatellite(store, st, "G05"));
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->discontinuity_counter, 1);
    EXPECT_EQ(store.biasAttributeMismatchCounts().count("G05|C1X|C"), 1u);
    EXPECT_EQ(store.getGenerationId(), std::optional<uint32_t>(5u));
    EXPECT_EQ(store.size(), 2u);

    writeSsrState(store, st);
    EXPECT_TRUE(store.biasAttributeMismatchCounts().empty());
    EXPECT_TRUE(store.codeBiasBandFallback());
    EXPECT_EQ(store.getPhaseBias("G05", "L1C")->discontinuity_counter, 1);
}

// The default signal sets follow the constellation.
TEST(SupportSsrStoreFixture, DefaultSignalsFollowConstellation) {
    const SsrSatSpec g = networkSsrSatellite("G05");
    const SsrSatSpec e = networkSsrSatellite("E11");
    const SsrSatSpec c = networkSsrSatellite("C20");
    EXPECT_EQ(g.code.count("C2W"), 1u);
    EXPECT_EQ(g.phase.count("L2W"), 1u);
    EXPECT_EQ(e.code.count("C5Q"), 1u);
    EXPECT_EQ(e.phase.count("L5Q"), 1u);
    EXPECT_EQ(c.code.count("C6I"), 1u);
    EXPECT_EQ(c.phase.count("L2I"), 1u);
    EXPECT_EQ(g.code.size(), 2u);
    EXPECT_EQ(g.phase.size(), 2u);
}

// A step on a name the state does not hold is a mistake in the test: reported, and nothing moves.
TEST(SupportSsrStoreFixture, UnknownNamesAreReported) {
    SsrStoreState st = networkSsrState(kSats);
    int r = 0;
    EXPECT_NONFATAL_FAILURE(r = bumpPhaseDiscontinuity(st, "G99", "L1C"), "G99");
    EXPECT_EQ(r, -1);
    EXPECT_NONFATAL_FAILURE(r = bumpCodeDiscontinuity(st, "G05", "C5Q"), "C5Q");
    EXPECT_EQ(r, -1);
    bool ok = true;
    EXPECT_NONFATAL_FAILURE(ok = restoreSatellite(st, "E11"), "E11");
    EXPECT_FALSE(ok);
    EXPECT_EQ(st.sats.size(), 2u);
    EXPECT_EQ(st.sats.at("G05").phase.at("L1C").discontinuity, 0);
}

}  // namespace
