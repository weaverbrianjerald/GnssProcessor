/**
 * @file test_disc_mark_scope.cpp
 * @brief `network.disc_mark_scope` (`sat` | `channel`): with `channel`, a
 *        per-satellite discontinuity mark bumps only the generation of the channel
 *        that moved.
 *
 * In the `sat` scope, `markSatDiscontinuity` puts a satellite into one
 * `forced_bump_sats_` set, and `satForced()` is read by the clock, code-bias and
 * phase-bias emit blocks alike.  A clock-only guard (the Q3 anchor decouple, the
 * CLK-R step test) therefore also declares a phase-bias discontinuity that did not
 * happen; 117 of 390 declarations on RF-0hv were of this kind (CST §7.1).
 *
 * The tests lock:
 *   - under the default `sat` scope a mark bumps all three generations, whatever
 *     channel mask the call site passes (byte-identical to the build without the scope);
 *   - under `channel` a clock-only mark leaves the phase and code generations
 *     untouched, and a code-bias mark leaves the clock and phase untouched;
 *   - two sites marking the same satellite in one epoch combine their masks by OR;
 *   - the re-coupling edge: the emitter's own per-(sat, signal) phase jump test
 *     still declares a real phase-bias step under a clock-only mark, so the
 *     narrowed scope does not replace a false declaration with a missed one;
 *   - the suppression census: one MARK_SCOPE-bound event per (sat, channel) per
 *     emit (not per signal), with the mask the marking sites asked for, and the
 *     applied-mark tally per channel;
 *   - the pending-mark state round-trips its channel mask, and a legacy
 *     (2-field `efs,`) snapshot still restores;
 *   - the config parse (`sat`, `channel`, and fallback on an unknown value) and the
 *     S-contract token `discscope=channel`, with the S line byte-identical when off.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

const GnssTime kMsT{ 2313, 259200.0 };

using Chan = NetworkProductEmitter::DiscChannel;

/// Two GPS satellites carrying clock + one code bias + one phase bias each; the
/// phase bias of G32 is settable so the jump test can be exercised.
NetworkSolutionEpoch msEpoch(const GnssTime& t, double g32_pb = 0.10) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    for (const char* s : { "G32", "G12" }) {
        NetworkSatSolution ns;
        ns.sat = s; ns.has_clock = true; ns.clock_c0_m = 1.0;
        ns.code_bias_m["C1W"] = 0.2;
        ns.phase_bias["L1W"]  = NetworkPhaseBias{ (std::string(s) == "G32") ? g32_pb : 0.10,
                                                  true, 1 };
        sol.sats.push_back(ns);
    }
    return sol;
}

/// An emitter with the clock channel on, in the requested scope.
NetworkProductEmitter makeEmitter(bool channel_scope) {
    return NetworkProductEmitter(0.05, /*emit_clock_disc=*/true, /*staleness=*/0.0,
                                 /*code_bias_jump_test=*/false,
                                 /*clock_disc_on_set_change=*/false,
                                 /*disc_threshold_frac_cycles=*/0.0,
                                 /*disc_mark_scope_channel=*/channel_scope);
}

fs::path writeMsConfig(const std::string& tag, const std::string& scope_json) {
    const fs::path p = gnss_test::tempDir() / ("ms_disc_mark_scope_" + tag + ".json");
    std::ofstream f(p);
    f << "{\n"
         "  \"processing\": {\n"
         "    \"mode\": \"ppp-rtk-network\",\n"
         "    \"active_systems\": [ \"GPS\" ],\n"
         "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
         "  },\n"
         "  \"network\": {\n"
         "    \"clock_datum_station\": \"AAAA\",\n"
      << scope_json <<
         "    \"stations\": [\n"
         "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
         "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
         "    ]\n"
         "  }\n"
         "}\n";
    f.close();
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// The scope itself.
// ---------------------------------------------------------------------------
TEST(DiscMarkScopeTest, SatScopeBumpsAllThreeWhateverMaskIsPassed) {
    SsrCorrectionStore store;
    NetworkProductEmitter e = makeEmitter(/*channel_scope=*/false);
    e.emitToStore(msEpoch(kMsT), store);                    // first sight: records only

    GnssTime t = kMsT; t.tow += 30.0;
    e.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);   // a narrow mask
    e.emitToStore(msEpoch(t), store);
    // Under the legacy scope all three generations step anyway, byte-identical to a
    // build without the scope; that is what makes the default safe.
    EXPECT_EQ(e.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(e.codeBiasDiscontinuityCounter("G32", "C1W"), 1);
    EXPECT_EQ(e.discontinuityCounter("G32", "L1W"), 1);
    EXPECT_TRUE(e.markScopeEvents().empty());               // nothing is censused there
    EXPECT_EQ(e.markChannelTally().clk, 0);
    EXPECT_FALSE(e.discMarkScopeChannel());
}

TEST(DiscMarkScopeTest, ChannelScopeBumpsOnlyTheMarkedChannel) {
    SsrCorrectionStore store;
    NetworkProductEmitter e = makeEmitter(/*channel_scope=*/true);
    ASSERT_TRUE(e.discMarkScopeChannel());
    e.emitToStore(msEpoch(kMsT), store);

    // A clock-only mark, as the Q3 anchor decouple issues.
    GnssTime t = kMsT; t.tow += 30.0;
    e.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);
    e.emitToStore(msEpoch(t), store);
    EXPECT_EQ(e.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(e.codeBiasDiscontinuityCounter("G32", "C1W"), 0);   // not declared
    EXPECT_EQ(e.discontinuityCounter("G32", "L1W"), 0);           // not declared (the empty declarations of CST §7.1)
    EXPECT_EQ(e.clockDiscontinuityCounter("G12"), 0);             // and still per-satellite

    // A code-bias-only mark, as cb_excursion_reinit issues.
    t.tow += 30.0;
    e.markSatDiscontinuity({ "G12" }, Chan::kDiscChanCodeBias);
    e.emitToStore(msEpoch(t), store);
    EXPECT_EQ(e.codeBiasDiscontinuityCounter("G12", "C1W"), 1);
    EXPECT_EQ(e.clockDiscontinuityCounter("G12"), 0);
    EXPECT_EQ(e.discontinuityCounter("G12", "L1W"), 0);
}

TEST(DiscMarkScopeTest, TwoSitesMarkingOneSatelliteOrTheirMasks) {
    SsrCorrectionStore store;
    NetworkProductEmitter e = makeEmitter(true);
    e.emitToStore(msEpoch(kMsT), store);

    GnssTime t = kMsT; t.tow += 30.0;
    e.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);       // site A: clock
    e.markSatDiscontinuity({ "G32" }, Chan::kDiscChanCodeBias);    // site B: code bias
    e.emitToStore(msEpoch(t), store);
    EXPECT_EQ(e.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(e.codeBiasDiscontinuityCounter("G32", "C1W"), 1);
    EXPECT_EQ(e.discontinuityCounter("G32", "L1W"), 0);            // neither site named phase

    // The mask is consumed with the mark: the next emit declares nothing.
    t.tow += 30.0;
    e.emitToStore(msEpoch(t), store);
    EXPECT_EQ(e.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(e.codeBiasDiscontinuityCounter("G32", "C1W"), 1);
}

TEST(DiscMarkScopeTest, AnUnmigratedCallerStillGetsTheFullSatelliteScope) {
    // The default argument, which a replay's R record and any un-migrated call site
    // use, declares all three channels even under `channel`; otherwise a replay would
    // under-declare without any signal.
    SsrCorrectionStore store;
    NetworkProductEmitter e = makeEmitter(true);
    e.emitToStore(msEpoch(kMsT), store);
    GnssTime t = kMsT; t.tow += 30.0;
    e.markSatDiscontinuity({ "G32" });                             // no channels named
    e.emitToStore(msEpoch(t), store);
    EXPECT_EQ(e.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(e.codeBiasDiscontinuityCounter("G32", "C1W"), 1);
    EXPECT_EQ(e.discontinuityCounter("G32", "L1W"), 1);
    EXPECT_TRUE(e.markScopeEvents().empty());                      // nothing was suppressed
}

// ---------------------------------------------------------------------------
// The re-coupling edge: the jump test is still the backstop.
// ---------------------------------------------------------------------------
TEST(DiscMarkScopeTest, PhaseJumpTestStillDeclaresUnderAClockOnlyMark) {
    SsrCorrectionStore store;
    NetworkProductEmitter e = makeEmitter(true);
    e.emitToStore(msEpoch(kMsT, /*g32_pb=*/0.10), store);

    // A clock-only mark and a real 1.409 m phase-bias step (0.10 to 1.509 m) on the
    // same satellite, the pattern seen at TOW 276810.  The mark does not declare
    // phase; the emitter's own jump test does, so the declaration survives the
    // narrowed scope.
    GnssTime t = kMsT; t.tow += 30.0;
    e.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);
    e.emitToStore(msEpoch(t, /*g32_pb=*/1.509), store);
    EXPECT_EQ(e.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(e.discontinuityCounter("G32", "L1W"), 1) << "the jump test is the backstop";
    // G12's phase did not move and nothing marked it: no declaration at all.
    EXPECT_EQ(e.discontinuityCounter("G12", "L1W"), 0);
}

// ---------------------------------------------------------------------------
// The census: counters + one event per (sat, channel).
// ---------------------------------------------------------------------------
TEST(DiscMarkScopeTest, SuppressedMarksAreCountedOncePerSatAndChannel) {
    SsrCorrectionStore store;
    NetworkProductEmitter e = makeEmitter(true);
    // Two code signals and two phase signals, so a per-signal census would double up.
    auto ep = [](const GnssTime& t) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        NetworkSatSolution ns;
        ns.sat = "G32"; ns.has_clock = true; ns.clock_c0_m = 1.0;
        ns.code_bias_m["C1W"] = 0.2;
        ns.code_bias_m["C2W"] = 0.4;
        ns.phase_bias["L1W"]  = NetworkPhaseBias{ 0.1, true, 1 };
        ns.phase_bias["L2W"]  = NetworkPhaseBias{ 0.3, true, 1 };
        sol.sats.push_back(ns);
        return sol;
    };
    e.emitToStore(ep(kMsT), store);

    GnssTime t = kMsT; t.tow += 30.0;
    e.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);
    e.emitToStore(ep(t), store);

    const auto& ev = e.markScopeEvents();
    ASSERT_EQ(ev.size(), 2u) << "one row per (sat, suppressed channel), not per signal";
    std::set<unsigned> suppressed;
    for (const auto& x : ev) {
        EXPECT_EQ(x.sat, "G32");
        EXPECT_EQ(x.marked, static_cast<unsigned>(Chan::kDiscChanClock));
        suppressed.insert(static_cast<unsigned>(x.suppressed));
    }
    EXPECT_EQ(suppressed.count(static_cast<unsigned>(Chan::kDiscChanCodeBias)), 1u);
    EXPECT_EQ(suppressed.count(static_cast<unsigned>(Chan::kDiscChanPhaseBias)), 1u);
    // The applied tally: the clock once (per satellite), code and phase never.
    EXPECT_EQ(e.markChannelTally().clk, 1);
    EXPECT_EQ(e.markChannelTally().cb, 0);
    EXPECT_EQ(e.markChannelTally().pb, 0);

    // Per-emit scratch: the next emit starts clean.
    t.tow += 30.0;
    e.emitToStore(ep(t), store);
    EXPECT_TRUE(e.markScopeEvents().empty());
    EXPECT_EQ(e.markChannelTally().clk, 0);

    EXPECT_STREQ(NetworkProductEmitter::discChannelName(Chan::kDiscChanClock), "clk");
    EXPECT_STREQ(NetworkProductEmitter::discChannelName(Chan::kDiscChanCodeBias), "cb");
    EXPECT_STREQ(NetworkProductEmitter::discChannelName(Chan::kDiscChanPhaseBias), "pb");
}

// ---------------------------------------------------------------------------
// The pending-mark state round-trips its mask.
// ---------------------------------------------------------------------------
TEST(DiscMarkScopeTest, PendingMarkStateRoundTripsTheChannelMaskAndAcceptsLegacy) {
    // A mark not yet consumed by an emit is pending state.  A resume restores it with
    // its channel mask; otherwise a narrow mark would come back as a wide one.
    NetworkProductEmitter src = makeEmitter(true);
    src.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);
    std::ostringstream os;
    src.serializeState(os);
    const std::string blob = os.str();
    EXPECT_NE(blob.find("efs,G32,1"), std::string::npos) << blob;

    NetworkProductEmitter dst = makeEmitter(true);
    std::istringstream is(blob);
    ASSERT_TRUE(dst.restoreState(is));
    SsrCorrectionStore store;
    dst.emitToStore(msEpoch(kMsT), store);                 // first sight records baselines
    GnssTime t = kMsT; t.tow += 30.0;
    // The restored mark lands on the first emit after the restore, which consumed it
    // above.  The serialized mask is checked through the blob (`efs,G32,1`); here a
    // clock-only mark is re-issued to check the channel scope on the restored emitter.
    dst.markSatDiscontinuity({ "G32" }, Chan::kDiscChanClock);
    dst.emitToStore(msEpoch(t), store);
    EXPECT_EQ(dst.clockDiscontinuityCounter("G32"), 1);
    EXPECT_EQ(dst.discontinuityCounter("G32", "L1W"), 0);

    // A legacy (satellite-scope) snapshot writes 2 fields and must still restore.
    NetworkProductEmitter legacy_src = makeEmitter(false);
    legacy_src.markSatDiscontinuity({ "G12" });
    std::ostringstream los;
    legacy_src.serializeState(los);
    EXPECT_NE(los.str().find("efs,G12\n"), std::string::npos) << los.str();
    NetworkProductEmitter legacy_dst = makeEmitter(true);
    std::istringstream lis(los.str());
    EXPECT_TRUE(legacy_dst.restoreState(lis));
}

// ---------------------------------------------------------------------------
// Config + the S contract.
// ---------------------------------------------------------------------------
TEST(DiscMarkScopeTest, ConfigParsesSatAndChannelAndFallsBackOnUnknown) {
    {
        GnssConfig d;
        EXPECT_EQ(d.getNetworkConfig().disc_mark_scope, "sat");
    }
    {
        LogManager log; GnssConfig cfg;
        const fs::path p = writeMsConfig("channel", "    \"disc_mark_scope\": \"channel\",\n");
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_EQ(cfg.getNetworkConfig().disc_mark_scope, "channel");
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log; GnssConfig cfg;
        const fs::path p = writeMsConfig("sat", "    \"disc_mark_scope\": \"sat\",\n");
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_EQ(cfg.getNetworkConfig().disc_mark_scope, "sat");
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log; GnssConfig cfg;
        const fs::path p = writeMsConfig("bogus", "    \"disc_mark_scope\": \"per_signal\",\n");
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_EQ(cfg.getNetworkConfig().disc_mark_scope, "sat")
            << "an unknown scope falls back to the legacy one, never to a narrower one";
        std::error_code ec; fs::remove(p, ec);
    }
}

TEST(DiscMarkScopeTest, SLineDeclaresTheScopeAndOffIsByteIdentical) {
    const fs::path with_p    = gnss_test::tempDir() / "ms_discscope_with.txt";
    const fs::path without_p = gnss_test::tempDir() / "ms_discscope_without.txt";
    std::error_code ec; fs::remove(with_p, ec); fs::remove(without_p, ec);

    SbasisContract base;
    base.decl.reference_receiver_id = "MBAR00UGA";
    base.disc_threshold_m           = 0.05;
    base.present                    = true;
    SbasisContract armed = base;
    armed.disc_mark_scope_channel = true;

    { NetworkProductWriter w(without_p.string(), base);  ASSERT_TRUE(w.good()); }
    { NetworkProductWriter w(with_p.string(),    armed); ASSERT_TRUE(w.good()); }

    const auto pick = [](const fs::path& p, const char* prefix) {
        std::ifstream f(p);
        std::string ln;
        while (std::getline(f, ln))
            if (ln.rfind(prefix, 0) == 0) return ln;
        return std::string{};
    };
    const std::string a = pick(without_p, "S,"), b = pick(with_p, "S,");
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_EQ(a.find("discscope="), std::string::npos)
        << "an OFF producer writes the pre-MS S line byte-for-byte";
    EXPECT_EQ(b, a + ",discscope=channel");

    // The legend is grammar-complete by contract and names the field in both files.
    // Pinning it here keeps the only products.txt difference on v1-header lineages a
    // documented one.
    const std::string legend = pick(without_p, "# S,");
    EXPECT_NE(legend.find("[,discscope=channel]"), std::string::npos) << legend;
    EXPECT_EQ(pick(with_p, "# S,"), legend);

    bool ok = false;
    SbasisContract parsed_on, parsed_off;
    NetworkProductReader::read(with_p.string(),    &parsed_on,  &ok);  ASSERT_TRUE(ok);
    NetworkProductReader::read(without_p.string(), &parsed_off, &ok);  ASSERT_TRUE(ok);
    EXPECT_TRUE(parsed_on.disc_mark_scope_channel);
    EXPECT_FALSE(parsed_off.disc_mark_scope_channel);
    // The flag reaches the replay emitter's construction, although the R record cannot
    // carry the channel set (see the field's Doxygen).
    EXPECT_TRUE(NetworkProductReader::makeReplayEmitter(parsed_on).discMarkScopeChannel());
    EXPECT_FALSE(NetworkProductReader::makeReplayEmitter(parsed_off).discMarkScopeChannel());
    fs::remove(with_p, ec); fs::remove(without_p, ec);
}

}  // namespace GnssProcessor
