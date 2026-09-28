/**
 * @file test_network_fast_clock.cpp
 * @brief Plumbing contracts of the split-cadence clocks-only fast pass (card FC-1, INC-0
 *        and INC-3).  docs/SPEC_fast_clock_split_cadence.md; seam
 *        filter/NetworkFastClock.hpp.
 *
 * The suite covers the wiring, not the estimation:
 *
 *   1. The config contract: defaults, the dotted/legacy key pair, and the clamps.  The
 *      run manifest echoes these defaults, so they must match the header.
 *   2. The inert-arming refusals (standing invariant #4).  The fast tier runs on epochs
 *      the full pipeline skips, so a miswired tier produces a clean log and no ticks.
 *      The loader refuses every configuration that arms the tier into a state that can
 *      never drive a tick, rather than leaving it to show up as a zero counter.
 *   3. The arming streak: @ref fastClockAdvanceArm, the one piece of logic in INC-0.  It
 *      keys on emission continuity, not solve continuity (R11), and requires two emitted
 *      epochs so that the lump rate can be formed.
 *   4. The fast wire record (INC-3): round trip, a file whose full-epoch records read
 *      the same as a pre-FC-1 file, and the consumer-side replay contract.
 *
 * The estimation gates (shadow-vs-full RMS, the six event responses, determinism) belong
 * to INC-2 and are not simulated here; a test at this level could not show that the tier
 * actually fired.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkFastClock.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("fastclk_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal loadable PPP-RTK network config.  @p net_extra is spliced into the network
// block (before "stations"), @p proc_extra into the processing block.
std::string networkConfig(const std::string& net_extra = "",
                          const std::string& proc_extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }"
        + (proc_extra.empty() ? std::string("\n") : ",\n" + proc_extra)
        + "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + net_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

// Load @p body, expecting the loader to accept it, and hand back the fast_clock block.
FastClockConfig loadFastClock(const std::string& tag, const std::string& body) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    EXPECT_TRUE(ok) << "loadFromJson failed for tag " << tag;
    return cfg.getNetworkConfig().fast_clock;
}

// A network block that arms the tier in a state that can drive ticks: decimation > 1.
std::string armedBlock(const std::string& extra_fast_clock_fields = "") {
    return "    \"epoch_decimation_n\": 2,\n"
           "    \"fast_clock\": { \"enable\": true"
           + extra_fast_clock_fields + " },\n";
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Config contract
// ---------------------------------------------------------------------------

// The card is off by default: a stock config leaves the T2e decimation hook with its
// bare `continue`, which is the precondition for the INC-0 byte-identity gate.
TEST(NetworkFastClockConfig, StockConfigLeavesTheTierCompletelyOff) {
    const FastClockConfig fc = loadFastClock("stock", networkConfig());
    EXPECT_FALSE(fc.enable);
    EXPECT_FALSE(fc.shadow);
    EXPECT_FALSE(fc.emit);
    EXPECT_FALSE(fc.trace);
    // Defaults match the header: the manifest echoes these, and a grader reads the
    // manifest to tell a split-cadence arm from a reference arm.
    EXPECT_EQ(fc.min_stations_per_sat, 3);
    EXPECT_EQ(fc.min_sats_per_station, 2);
    EXPECT_DOUBLE_EQ(fc.screen_k_sigma, 5.0);
    EXPECT_FALSE(fc.allow_single_band);
    EXPECT_DOUBLE_EQ(fc.emit_max_gap_s, 60.0);
    EXPECT_EQ(fc.trace_sat_stride, 0);
}

TEST(NetworkFastClockConfig, DottedKeysParse) {
    const FastClockConfig fc = loadFastClock(
        "dotted",
        networkConfig("    \"epoch_decimation_n\": 10,\n"
                      "    \"fast_clock\": {\n"
                      "      \"enable\": true,\n"
                      "      \"shadow\": true,\n"
                      "      \"min_stations_per_sat\": 5,\n"
                      "      \"min_sats_per_station\": 4,\n"
                      "      \"screen_k_sigma\": 3.5,\n"
                      "      \"allow_single_band\": true,\n"
                      "      \"emit_max_gap_s\": 15.0,\n"
                      "      \"trace\": true,\n"
                      "      \"trace_sat_stride\": 7\n"
                      "    },\n"));
    EXPECT_TRUE(fc.enable);
    EXPECT_TRUE(fc.shadow);
    EXPECT_EQ(fc.min_stations_per_sat, 5);
    EXPECT_EQ(fc.min_sats_per_station, 4);
    EXPECT_DOUBLE_EQ(fc.screen_k_sigma, 3.5);
    EXPECT_TRUE(fc.allow_single_band);
    EXPECT_DOUBLE_EQ(fc.emit_max_gap_s, 15.0);
    EXPECT_TRUE(fc.trace);
    EXPECT_EQ(fc.trace_sat_stride, 7);
}

// Every sub-struct knob in this project carries a flat legacy alias; the nested path must
// win when both are present (GnssConfigJsonHelpers precedence).
TEST(NetworkFastClockConfig, LegacyFlatKeysParseAndNestedWins) {
    // The legacy aliases live at the JSON root, not inside "network", so this config is
    // written out longhand rather than through the networkConfig() helper.
    LogManager log;
    GnssConfig cfg;
    const std::string body =
        std::string("{\n")
        + "  \"network_fast_clock_enable\": true,\n"
          "  \"network_fast_clock_screen_k_sigma\": 9.0,\n"
          "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"epoch_decimation_n\": 2,\n"
          "    \"fast_clock\": { \"screen_k_sigma\": 4.0 },\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
    const fs::path p = writeTempConfig("legacy_mixed", body);
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
    const FastClockConfig fc = cfg.getNetworkConfig().fast_clock;
    EXPECT_TRUE(fc.enable) << "flat legacy alias must arm the tier";
    EXPECT_DOUBLE_EQ(fc.screen_k_sigma, 4.0)
        << "the nested network.fast_clock.screen_k_sigma must beat the flat alias";
}

TEST(NetworkFastClockConfig, ClampsRepairNonsenseValues) {
    const FastClockConfig fc = loadFastClock(
        "clamps",
        networkConfig(armedBlock(", \"min_stations_per_sat\": 1"
                                 ", \"min_sats_per_station\": 0"
                                 ", \"screen_k_sigma\": -1.0"
                                 ", \"emit_max_gap_s\": -5.0"
                                 ", \"trace_sat_stride\": -3")));
    // One station cannot separate its own clock from the satellite's: the bipartite fit
    // is rank-deficient, so 2 is a structural floor.
    EXPECT_EQ(fc.min_stations_per_sat, 2);
    EXPECT_EQ(fc.min_sats_per_station, 1);
    // A non-positive k would screen every LOS and leave the tick with nothing to fit.
    EXPECT_DOUBLE_EQ(fc.screen_k_sigma, 5.0);
    EXPECT_DOUBLE_EQ(fc.emit_max_gap_s, 0.0);
    EXPECT_EQ(fc.trace_sat_stride, 0);
}

// ---------------------------------------------------------------------------
// 2. Inert-arming refusals (standing invariant #4)
// ---------------------------------------------------------------------------

// The tier is armed and the config is otherwise valid, but there is no decimated-out
// epoch for a tick to run on.  Such a run would log cleanly and drive zero ticks.
TEST(NetworkFastClockInertArming, EnableWithoutDecimationOrShadowIsDisarmed) {
    const FastClockConfig fc = loadFastClock(
        "no_decim",
        networkConfig("    \"epoch_decimation_n\": 1,\n"
                      "    \"fast_clock\": { \"enable\": true },\n"));
    EXPECT_FALSE(fc.enable)
        << "epoch_decimation_n=1 leaves no decimated-out epochs, so the tier can never "
           "drive a tick — it must be refused at load, not discovered as a zero counter";
}

// Shadow mode makes the same config valid, because it runs the tick on full epochs.
TEST(NetworkFastClockInertArming, ShadowMakesUndecimatedRunsLegitimate) {
    const FastClockConfig fc = loadFastClock(
        "shadow_no_decim",
        networkConfig("    \"epoch_decimation_n\": 1,\n"
                      "    \"fast_clock\": { \"enable\": true, \"shadow\": true },\n"));
    EXPECT_TRUE(fc.enable);
    EXPECT_TRUE(fc.shadow);
}

TEST(NetworkFastClockInertArming, ShadowWithoutEnableIsDisarmed) {
    const FastClockConfig fc = loadFastClock(
        "shadow_only",
        networkConfig("    \"fast_clock\": { \"shadow\": true },\n"));
    EXPECT_FALSE(fc.enable);
    EXPECT_FALSE(fc.shadow);
}

TEST(NetworkFastClockInertArming, EmitWithoutEnableIsDisarmed) {
    const FastClockConfig fc = loadFastClock(
        "emit_only",
        networkConfig("    \"fast_clock\": { \"emit\": true },\n"));
    EXPECT_FALSE(fc.emit);
}

// emit with no product file is the same class of defect: armed, but unable to reach a
// consumer.
TEST(NetworkFastClockInertArming, EmitWithoutProductPathIsDisarmedButTierSurvives) {
    const FastClockConfig fc = loadFastClock(
        "emit_no_path",
        networkConfig(armedBlock(", \"emit\": true")));
    EXPECT_TRUE(fc.enable) << "the tick still runs and traces";
    EXPECT_FALSE(fc.emit)  << "with no product_output_path the fast rows have nowhere to land";
}

TEST(NetworkFastClockInertArming, EmitSurvivesWhenAProductPathExists) {
    const FastClockConfig fc = loadFastClock(
        "emit_with_path",
        networkConfig("    \"product_output_path\": \"fc1_products.txt\",\n"
                      + armedBlock(", \"emit\": true")));
    EXPECT_TRUE(fc.enable);
    EXPECT_TRUE(fc.emit);
}

// ---------------------------------------------------------------------------
// 3. The arming streak: fastClockAdvanceArm
// ---------------------------------------------------------------------------

// A cold start has no anchor at all: the first emitted epoch establishes one but cannot
// arm, because the lump rate is a finite difference and needs a second.
TEST(NetworkFastClockArming, ColdStartNeedsTwoEmittedEpochs) {
    const FastClockArmDecision first = fastClockAdvanceArm(0.0, 0, 1000.0, 60.0);
    EXPECT_TRUE(first.chain_broken);
    EXPECT_EQ(first.consecutive_emitted, std::size_t{1});
    EXPECT_FALSE(first.armed);

    const FastClockArmDecision second =
        fastClockAdvanceArm(1000.0, first.consecutive_emitted, 1030.0, 60.0);
    EXPECT_FALSE(second.chain_broken);
    EXPECT_EQ(second.consecutive_emitted, std::size_t{2});
    EXPECT_TRUE(second.armed) << "two consecutive emitted epochs supply lump AND lump rate";
}

TEST(NetworkFastClockArming, StreakAccumulatesWhileEmissionIsContinuous) {
    std::size_t streak = 0;
    double prev = 0.0;
    for (int i = 0; i < 5; ++i) {
        const double now = 1000.0 + 30.0 * i;
        const FastClockArmDecision d = fastClockAdvanceArm(prev, streak, now, 60.0);
        streak = d.consecutive_emitted;
        prev = now;
    }
    EXPECT_EQ(streak, std::size_t{5});
}

// A withhold streak longer than the R11 bound breaks the chain: the consumer went without
// a full solve, so a lump rate differenced across the gap is not a valid linearization,
// and the tier re-arms from scratch instead of continuing on the stale anchor.
TEST(NetworkFastClockArming, GapBeyondTheEmissionBoundResetsTheStreak) {
    const FastClockArmDecision d = fastClockAdvanceArm(1000.0, 7, 1000.0 + 61.0, 60.0);
    EXPECT_TRUE(d.chain_broken);
    EXPECT_EQ(d.consecutive_emitted, std::size_t{1});
    EXPECT_FALSE(d.armed) << "a re-armed anchor must spend one full-solve era unarmed";
}

// The bound is inclusive: a gap of exactly emit_max_gap_s is still continuous, matching
// the emission gate's own comparison so the two cannot disagree by one epoch.
TEST(NetworkFastClockArming, GapExactlyAtTheBoundIsStillContinuous) {
    const FastClockArmDecision d = fastClockAdvanceArm(1000.0, 3, 1060.0, 60.0);
    EXPECT_FALSE(d.chain_broken);
    EXPECT_EQ(d.consecutive_emitted, std::size_t{4});
    EXPECT_TRUE(d.armed);
}

// emit_max_gap_s = 0 is the strictest posture the clamp permits: only a zero-length gap
// counts as continuous, so any real cadence re-arms every epoch and the tier never fires.
// The configuration is legal, and the decision reports the broken chain as such.
TEST(NetworkFastClockArming, ZeroBoundBreaksEveryChain) {
    const FastClockArmDecision d = fastClockAdvanceArm(1000.0, 4, 1030.0, 0.0);
    EXPECT_TRUE(d.chain_broken);
    EXPECT_FALSE(d.armed);
}

// ---------------------------------------------------------------------------
// 4. The fast wire record (INC-3)
// ---------------------------------------------------------------------------

namespace {

GnssTime tAt(double tow) { GnssTime t{}; t.week = 2313; t.tow = tow; return t; }

std::string tempProduct(const std::string& tag) {
    return (std::filesystem::temp_directory_path() / ("fc1_wire_" + tag + ".txt")).string();
}

// A minimal full epoch, so the store holds both a clock and a bias for a fast row to be
// checked against.
NetworkSolutionEpoch fullEpoch(double tow, double c0_g01, double c0_g02) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = tAt(tow);
    sol.iod_ssr = 1; sol.provider_id = 7; sol.solution_id = 2;
    NetworkSatSolution a; a.sat = "G01"; a.has_clock = true; a.clock_c0_m = c0_g01;
    a.code_bias_m["C1W"] = 0.25;
    a.phase_bias["L1C"] = NetworkPhaseBias{1.5, true, 0};
    NetworkSatSolution b; b.sat = "G02"; b.has_clock = true; b.clock_c0_m = c0_g02;
    b.code_bias_m["C1W"] = -0.10;
    sol.sats = {a, b};
    return sol;
}

} // namespace

TEST(NetworkFastClockWire, FastRecordRoundTripsAndIsTaggedFast) {
    const std::string path = tempProduct("roundtrip");
    std::filesystem::remove(path);
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(fullEpoch(259200.0, 12.5, -3.25));
        w.writeFastClock(tAt(259230.0), tAt(259200.0),
                         {{"G01", 12.61, 0.004}, {"G02", -3.19, 0.0055}});
    }
    bool ok = false;
    const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok) << "a fast record must parse cleanly, not as a malformed line";
    ASSERT_EQ(recs.size(), 2u);

    EXPECT_FALSE(recs[0].is_fast);
    ASSERT_TRUE(recs[1].is_fast);
    const NetworkProductRecord& f = recs[1];
    EXPECT_DOUBLE_EQ(f.epoch.epoch_time.tow, 259230.0);
    // The anchor is what lets a consumer reject a fast row it cannot interpret: the
    // values are a delta on that full solve and are meaningless against another.
    EXPECT_EQ(f.fast_anchor.week, 2313);
    EXPECT_DOUBLE_EQ(f.fast_anchor.tow, 259200.0);
    ASSERT_EQ(f.fast_clocks.size(), 2u);
    EXPECT_DOUBLE_EQ(f.fast_clocks.at("G01").first, 12.61);
    EXPECT_DOUBLE_EQ(f.fast_clocks.at("G01").second, 0.004);
    EXPECT_DOUBLE_EQ(f.fast_clocks.at("G02").first, -3.19);
    // A fast record carries nothing else, by construction.
    EXPECT_TRUE(f.epoch.sats.empty());
    EXPECT_FALSE(f.datum_changed);
    EXPECT_TRUE(f.retro_sats.empty());
    std::filesystem::remove(path);
}

// Both the fast marker (F) and its payload (K) use private tags: a pre-FC-1 reader skips
// unknown tags silently, so a fast record written with the C tag would have its payload
// absorbed into the preceding full epoch.  With every FC-1 record removed, the file reads
// the same as one written without them.
TEST(NetworkFastClockWire, StrippingFastRecordsLeavesThePreCardFileExactly) {
    const std::string with = tempProduct("with");
    const std::string without = tempProduct("without");
    std::filesystem::remove(with);
    std::filesystem::remove(without);
    {
        NetworkProductWriter w(with);
        w.write(fullEpoch(259200.0, 12.5, -3.25));
        w.writeFastClock(tAt(259210.0), tAt(259200.0), {{"G01", 12.55, 0.004}});
        w.writeFastClock(tAt(259220.0), tAt(259200.0), {{"G01", 12.58, 0.004}});
        w.write(fullEpoch(259230.0, 12.7, -3.10));
    }
    {
        NetworkProductWriter w(without);
        w.write(fullEpoch(259200.0, 12.5, -3.25));
        w.write(fullEpoch(259230.0, 12.7, -3.10));
    }
    bool ok_with = false, ok_without = false;
    const std::vector<NetworkProductRecord> rw = NetworkProductReader::read(with, &ok_with);
    const std::vector<NetworkProductRecord> rn =
        NetworkProductReader::read(without, &ok_without);
    EXPECT_TRUE(ok_with);
    EXPECT_TRUE(ok_without);
    std::vector<NetworkProductRecord> slow;
    for (const auto& r : rw)
        if (!r.is_fast) slow.push_back(r);
    ASSERT_EQ(slow.size(), rn.size());
    for (std::size_t i = 0; i < slow.size(); ++i) {
        EXPECT_DOUBLE_EQ(slow[i].epoch.epoch_time.tow, rn[i].epoch.epoch_time.tow);
        ASSERT_EQ(slow[i].epoch.sats.size(), rn[i].epoch.sats.size());
        for (std::size_t k = 0; k < slow[i].epoch.sats.size(); ++k) {
            EXPECT_EQ(slow[i].epoch.sats[k].sat, rn[i].epoch.sats[k].sat);
            EXPECT_DOUBLE_EQ(slow[i].epoch.sats[k].clock_c0_m,
                             rn[i].epoch.sats[k].clock_c0_m);
        }
    }
    std::filesystem::remove(with);
    std::filesystem::remove(without);
}

// The consumer contract, field by field: a fast row moves the clock value and its age
// and nothing else.  Each field checked here is one a consumer would mishandle without
// a warning if a fast tick changed it.
TEST(NetworkFastClockWire, ReplayRefreshesClockValueAndAgeOnly) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;
    NetworkProductRecord full;
    full.epoch = fullEpoch(259200.0, 12.5, -3.25);
    NetworkProductReader::replay(full, store, emitter);

    const auto before = store.getOrbitClock("G01", tAt(259200.0));
    ASSERT_TRUE(before.has_value());
    const auto disc_before = store.getClockDiscontinuity("G01");
    const double cb_before = store.getCodeBias("G01", "C1W");
    EXPECT_DOUBLE_EQ(cb_before, 0.25);

    NetworkProductRecord fast;
    fast.is_fast = true;
    fast.epoch.epoch_time = tAt(259230.0);
    fast.fast_anchor = tAt(259200.0);
    fast.fast_clocks["G01"] = {12.61, 0.004};
    NetworkProductReader::replay(fast, store, emitter);

    const auto after = store.getOrbitClock("G01", tAt(259230.0));
    ASSERT_TRUE(after.has_value()) << "the refreshed clock must be fresh at the tick epoch";
    EXPECT_NEAR(after->c0, 12.61, 1e-9) << "the clock VALUE must refresh";
    EXPECT_EQ(store.getClockDiscontinuity("G01"), disc_before)
        << "a fast row continues the same clock; advancing disc would make every "
           "consumer reset its ambiguities at the fast cadence";
    // The bias channel belongs to the slow era: same value, and not aged off when the
    // clock refresh re-stamps the shared per-satellite header.
    EXPECT_DOUBLE_EQ(store.getCodeBias("G01", "C1W"), cb_before)
        << "a fast clock refresh must not disturb the code-bias channel";
    // G02 got no fast row and is unchanged.
    const auto g2 = store.getOrbitClock("G02", tAt(259200.0));
    ASSERT_TRUE(g2.has_value());
    EXPECT_DOUBLE_EQ(g2->c0, -3.25);
}

// A fast row does not create a satellite: a consumer handed a clock with no bias set to
// go with it would build a corrected range from half a correction.
TEST(NetworkFastClockWire, FastRowForAnUnknownSatelliteIsIgnored) {
    SsrCorrectionStore store;
    NetworkProductEmitter emitter;
    NetworkProductRecord full;
    full.epoch = fullEpoch(259200.0, 12.5, -3.25);
    NetworkProductReader::replay(full, store, emitter);

    NetworkProductRecord fast;
    fast.is_fast = true;
    fast.epoch.epoch_time = tAt(259230.0);
    fast.fast_anchor = tAt(259200.0);
    fast.fast_clocks["G31"] = {99.0, 0.004};   // never seen in a full epoch
    NetworkProductReader::replay(fast, store, emitter);

    EXPECT_FALSE(store.getOrbitClock("G31", tAt(259230.0)).has_value())
        << "pushFastClock must refresh, never establish";
}

// A fast marker followed by full-epoch payload is a writer bug that would put slow-era
// values on the fast path; the reader flags it instead of absorbing it.
TEST(NetworkFastClockWire, FullEpochPayloadInsideAFastRecordIsRejected) {
    const std::string path = tempProduct("mixed");
    std::filesystem::remove(path);
    {
        std::ofstream f(path);
        f << "# gnss_network_product v1\n";
        f << "F,2313,259230,2313,259200,1\n";
        f << "K,G01,12.61,sigk=0.004\n";
        f << "C,G01,12.61\n";          // illegal inside a fast record
    }
    bool ok = true;
    const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    EXPECT_FALSE(ok) << "a C line inside a fast record must set the malformed flag";
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(recs[0].is_fast);
    EXPECT_EQ(recs[0].fast_clocks.size(), 1u);
    EXPECT_TRUE(recs[0].epoch.sats.empty()) << "the illegal C must not land anywhere";
    std::filesystem::remove(path);
}
