/**
 * @file test_stream_filed_counters.cpp
 * @brief A file reader emits each epoch once and takes the file's discontinuity counters
 *        before the records are pushed.
 *
 * A product file written with @c network.serialize_disc_counters carries the producer's
 * post-emit counters on every C, B and P record.  @c NetworkProductReader::replay hands them
 * to @c NetworkProductEmitter::emitToStore, which runs its own rules on every key and then
 * takes the file's value.  The earlier reader emitted the epoch, installed the file's
 * counters into the emitter and emitted the epoch a second time; these cases pin that the
 * single emission leaves the same store and emitter counters on a stream without declared
 * level steps, and that each value that differs from the emitter's own rules is listed with
 * its class.
 */

#include "../support/SolutionEpochBuilder.hpp"

#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using gnss_test::SolutionEpochBuilder;
using Cls = NetworkProductEmitter::FiledDisagreementClass;

/// An emitter with the clock channel, as a producer that serializes clock counters builds it.
NetworkProductEmitter clockEmitter() {
    return NetworkProductEmitter(NetworkProductEmitter::kDefaultDiscThresholdM, /*emit_clock_disc=*/true);
}

/// A record of the builder's epoch carrying the builder's counters, as the parser returns it.
NetworkProductRecord recordOf(const SolutionEpochBuilder& b) {
    NetworkProductRecord rec;
    rec.epoch = b.epoch();
    for (const auto& s : rec.epoch.sats) {
        if (s.has_clock) rec.serialized_disc.clock[s.sat] = b.clockDiscOf(s.sat);
        for (const auto& [sig, v] : s.code_bias_m) {
            (void)v;
            rec.serialized_disc.code[s.sat + "|" + sig] = b.codeDiscOf(s.sat, sig);
        }
        for (const auto& [sig, v] : s.phase_bias) {
            (void)v;
            rec.serialized_disc.phase[s.sat + "|" + sig] = b.phaseDiscOf(s.sat, sig);
        }
    }
    return rec;
}

/// Everything the store answers for the default epoch's satellites and signals.
std::string dump(const SsrCorrectionStore& st) {
    std::string o;
    for (const char* sat : {"G05", "G12", "E11"}) {
        o += fmt::format("{} cd {} ", sat, st.getClockDiscontinuity(sat).value_or(-1));
        for (const char* sig : {"C1C", "C2W", "C5Q"})
            if (st.hasCodeBias(sat, sig))
                o += fmt::format("{} {} {} ", sig, st.getCodeBias(sat, sig),
                                 st.getCodeBiasDiscontinuity(sat, sig).value_or(-1));
        for (const char* sig : {"L1C", "L2W", "L5Q"})
            if (const auto pb = st.getPhaseBias(sat, sig))
                o += fmt::format("{} {} {} {} {} ", sig, pb->bias_m, pb->signal_integer ? 1 : 0,
                                 pb->wide_lane_integer, pb->discontinuity_counter);
        if (const auto lv = st.getDeclaredLevelSteps(sat)) o += fmt::format("lv {} {} ", lv->clock_m, lv->n);
        o += "\n";
    }
    return o;
}

/// Four epochs with value jumps the emitter declares and file counters that disagree with it in
/// both directions, on every channel; no declared level step.
std::vector<NetworkProductRecord> sequence() {
    std::vector<NetworkProductRecord> v;
    SolutionEpochBuilder b;
    v.push_back(recordOf(b));
    // Epoch 2: G05 L1C jumps 0.25 m (the emitter steps it; the file does not), the file steps
    // G12 L2W (flag down) and E11 L1C (flag up) and the G05 C1C code and E11 clock counters.
    b.advance(30.0).phaseBias("G05", "L1C", 0.2921).phaseDisc("G12", "L2W", 1).phaseDisc("E11", "L1C", 1)
        .codeDisc("G05", "C1C", 1).clockDisc("E11", 1);
    v.push_back(recordOf(b));
    // Epoch 3: G12 L2W's flag rises and the file steps it; the emitter has nothing to step.
    b.advance(30.0).signalInteger("G12", "L2W", true).phaseDisc("G12", "L2W", 2);
    v.push_back(recordOf(b));
    // Epoch 4: nothing moves; the counters stay where the file put them.
    b.advance(30.0);
    v.push_back(recordOf(b));
    return v;
}

// The single emission leaves the store, epoch by epoch, and the emitter's counters as the
// earlier reader left them (emit, install the file's counters, emit again).  A reader that
// took the file's counters after pushing the records would leave the emitter's own counters
// in the store and fail here.
TEST(StreamFiledCounters, OneEmissionLeavesTheStoreOfTheEarlierInstallAndReEmit) {
    const std::vector<NetworkProductRecord> recs = sequence();
    SsrCorrectionStore    now_store, then_store;
    NetworkProductEmitter now  = clockEmitter();
    NetworkProductEmitter then = clockEmitter();
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const NetworkProductRecord& rec = recs[i];
        NetworkProductReader::replay(rec, now_store, now);
        then.emitToStore(rec.epoch, then_store);
        then.adoptDiscontinuityCounters(rec.serialized_disc.phase, rec.serialized_disc.code,
                                        rec.serialized_disc.clock);
        then.emitToStore(rec.epoch, then_store);
        EXPECT_EQ(dump(now_store), dump(then_store)) << "epoch " << i;
        for (const auto& [key, v] : rec.serialized_disc.phase) {
            const std::string sat = key.substr(0, 3), sig = key.substr(4);
            EXPECT_EQ(now.discontinuityCounter(sat, sig), v) << key << " at epoch " << i;
        }
    }
}

// Each value the file holds against the emitter's own rules is listed once, with its class,
// at the epoch it differs, and the run totals add up.
TEST(StreamFiledCounters, EveryDisagreementIsListedWithItsClass) {
    const std::vector<NetworkProductRecord> recs = sequence();
    SsrCorrectionStore    store;
    NetworkProductEmitter em = clockEmitter();
    std::vector<std::vector<std::string>> seen;
    for (const NetworkProductRecord& rec : recs) {
        NetworkProductReader::replay(rec, store, em);
        std::vector<std::string> e;
        for (const auto& d : em.lastFiledDisagreements())
            e.push_back(fmt::format("{} {} {} {}", NetworkProductEmitter::filedDisagreementClassName(d.cls), d.key,
                                    d.filed, d.own));
        seen.push_back(e);
    }
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_TRUE(seen[0].empty());
    const std::vector<std::string> epoch2{
        "clock E11 1 0",                   // the E11 clock counter, file ahead
        "phase flag up E11|L1C 1 0",       // E11 L1C, flag up, file ahead
        "code G05|C1C 1 0",                // the G05 C1C code counter, file ahead
        "phase replay step only G05|L1C 0 1",   // the 0.25 m jump the file did not declare
        "phase flag down G12|L2W 1 0",     // G12 L2W, flag down, file ahead
    };
    std::vector<std::string> got2 = seen[1], want2 = epoch2;
    std::sort(got2.begin(), got2.end());
    std::sort(want2.begin(), want2.end());
    EXPECT_EQ(got2, want2);
    EXPECT_EQ(seen[2], std::vector<std::string>{ "phase flag rise G12|L2W 2 1" });
    EXPECT_TRUE(seen[3].empty());

    const auto& t = em.filedDisagreementTotals();
    EXPECT_EQ(t[static_cast<std::size_t>(Cls::PhaseFlagDown)], 1);
    EXPECT_EQ(t[static_cast<std::size_t>(Cls::PhaseFlagRise)], 1);
    EXPECT_EQ(t[static_cast<std::size_t>(Cls::PhaseFlagUp)], 1);
    EXPECT_EQ(t[static_cast<std::size_t>(Cls::PhaseReplayAhead)], 1);
    EXPECT_EQ(t[static_cast<std::size_t>(Cls::Code)], 1);
    EXPECT_EQ(t[static_cast<std::size_t>(Cls::Clock)], 1);
}

// A record without counters, and the producer's own call, leave the list empty: the emitter's
// counters are its own.
TEST(StreamFiledCounters, WithoutFileCountersNothingIsListed) {
    std::vector<NetworkProductRecord> recs = sequence();
    SsrCorrectionStore    store;
    NetworkProductEmitter em = clockEmitter();
    for (NetworkProductRecord& rec : recs) {
        rec.serialized_disc = {};
        NetworkProductReader::replay(rec, store, em);
        EXPECT_TRUE(em.lastFiledDisagreements().empty());
    }
    EXPECT_EQ(em.discontinuityCounter("G05", "L1C"), 1);   // the emitter's own value jump
    EXPECT_EQ(em.discontinuityCounter("G12", "L2W"), 0);
}

}  // namespace
}  // namespace GnssProcessor
