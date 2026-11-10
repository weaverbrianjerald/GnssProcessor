/**
 * @file test_rt_state_snapshot_latches.cpp
 * @brief Unit gates for the initializer's seed latches in the RT filter-state snapshot
 *        (card CRS2, docs/record/refound/CRS2_resume_hold_and_latches_2026-11-06.md): the @c I
 *        record round-trips, an unknown or duplicated latch fails the strict parse, a
 *        file without the rows restores nothing, the scope rule of the restore, and the
 *        loader's side of the two keys (@c realtime.state_snapshot_latch_capture,
 *        @c realtime.state_snapshot_latch_restore).
 *
 * The engine-level halves (the latches set before the first processed epoch, the seeds
 * not re-run) need a full engine and product stream, so they are graded on the harness
 * legs: card CRS2 section 5.  The format contract and the scope rule are pinned here.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "realtime/RtStateSnapshot.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal valid snapshot: two core states, no dynamic state.
RtStateSnapshot makeSnap() {
    RtStateSnapshot s;
    s.snap_week  = 2313;
    s.snap_tow   = 275580.0;
    s.wall_utc   = "2026-11-06T00:00:00Z";
    s.fingerprint = 0x30d7d945a4f6f13fULL;
    s.core_size  = 2;
    s.state_size = 2;
    s.generation_id = 1399161629u;
    s.epoch_count   = 485;
    s.x = Eigen::VectorXd::Zero(2);
    s.x(0) = 1.5; s.x(1) = -2.25;
    s.P = Eigen::MatrixXd::Identity(2, 2) * 0.04;
    s.ssr_disc = { { "G01|L1C", 3 } };
    return s;
}

std::string serialized(const RtStateSnapshot& s) {
    std::ostringstream os;
    RtStateSnapshotIO::serialize(os, s);
    return os.str();
}

bool parses(const std::string& text, RtStateSnapshot& out, std::string* err = nullptr) {
    std::istringstream is(text);
    return RtStateSnapshotIO::parse(is, out, err);
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("rtlatch_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

std::string realtimeConfig(const std::string& rt_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  },\n"
          "  \"realtime\": {\n"
        + rt_extra +
          "    \"output_type\": \"console\"\n"
          "  }\n"
          "}\n";
}

}  // namespace

TEST(RtStateSnapshotLatches, LatchRowsRoundTripInTheWritersOrder) {
    RtStateSnapshot s = makeSnap();
    s.init_latches = { { "isb", true }, { "phase_clock", false },
                       { "code_bias", true }, { "clock_seed", false } };
    const std::string text = serialized(s);
    // Written in the writer's order (the StateInitializer's), as 0/1 flags, before END.
    const std::size_t i_isb = text.find("\nI,isb,1\n");
    const std::size_t i_pc  = text.find("\nI,phase_clock,0\n");
    const std::size_t i_cb  = text.find("\nI,code_bias,1\n");
    const std::size_t i_cs  = text.find("\nI,clock_seed,0\n");
    ASSERT_NE(i_isb, std::string::npos) << text;
    ASSERT_NE(i_pc, std::string::npos) << text;
    ASSERT_NE(i_cb, std::string::npos) << text;
    ASSERT_NE(i_cs, std::string::npos) << text;
    EXPECT_LT(i_isb, i_pc);
    EXPECT_LT(i_pc, i_cb);
    EXPECT_LT(i_cb, i_cs);
    EXPECT_EQ(text.rfind("# gnss_rt_state_snapshot v1\n", 0), 0u) << "the rows sit under the v1 header";

    RtStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parses(text, r, &err)) << err;
    EXPECT_EQ(r.init_latches, s.init_latches);
    EXPECT_EQ(r.ssr_disc, s.ssr_disc);
}

TEST(RtStateSnapshotLatches, FileWithoutLatchRowsRestoresNothing) {
    const RtStateSnapshot s = makeSnap();
    const std::string text = serialized(s);
    EXPECT_EQ(text.find("\nI,"), std::string::npos) << text;
    RtStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parses(text, r, &err)) << err;
    EXPECT_TRUE(r.init_latches.empty());
    // Nothing to apply under either scope: the restore is inert on an older file.
    EXPECT_TRUE(rtSnapshotLatchesToApply(r.init_latches, false).empty());
    EXPECT_TRUE(rtSnapshotLatchesToApply(r.init_latches, true).empty());
}

TEST(RtStateSnapshotLatches, UnknownOrDuplicateLatchIsRejected) {
    RtStateSnapshot s = makeSnap();
    s.init_latches = { { "isb", true } };
    const std::string good = serialized(s);
    RtStateSnapshot r;
    ASSERT_TRUE(parses(good, r));
    // An unknown latch name.
    {
        std::string t = good;
        t.insert(t.find("I,isb,1"), "I,ambiguity,1\n");
        std::string err;
        EXPECT_FALSE(parses(t, r, &err));
        EXPECT_NE(err.find("ambiguity"), std::string::npos) << err;
    }
    // A duplicated latch (the END count is regenerated so the duplicate is what fails).
    {
        std::string t = good;
        t.insert(t.find("I,isb,1"), "I,isb,0\n");
        const auto pos = t.rfind("END,");
        unsigned long long n = std::stoull(t.substr(pos + 4));
        t = t.substr(0, pos) + "END," + std::to_string(n + 1) + "\n";
        std::string err;
        EXPECT_FALSE(parses(t, r, &err));
        EXPECT_NE(err.find("duplicate"), std::string::npos) << err;
    }
    // A flag that is not 0/1.
    {
        std::string t = good;
        const auto pos = t.find("I,isb,1");
        t.replace(pos, 7, "I,isb,2");
        EXPECT_FALSE(parses(t, r));
    }
}

/// The scope rule: every latch the file holds true is set, except that the code-bias
/// latch is set under the full scope only, because the core scope re-creates the receiver
/// code-bias states per epoch and they need their first-epoch seed.
TEST(RtStateSnapshotLatches, ScopeRuleKeepsTheCodeBiasSeedUnderTheCoreScope) {
    const std::map<std::string, bool> all = { { "isb", true }, { "phase_clock", true },
                                              { "code_bias", true }, { "clock_seed", true } };
    EXPECT_EQ(rtSnapshotLatchesToApply(all, true),
              (std::vector<std::string>{ "isb", "phase_clock", "code_bias", "clock_seed" }));
    EXPECT_EQ(rtSnapshotLatchesToApply(all, false),
              (std::vector<std::string>{ "isb", "phase_clock", "clock_seed" }));
    // A latch the writer held clear stays clear: the seed runs as on the writer.
    const std::map<std::string, bool> some = { { "isb", true }, { "phase_clock", false },
                                               { "code_bias", true }, { "clock_seed", false } };
    EXPECT_EQ(rtSnapshotLatchesToApply(some, true), (std::vector<std::string>{ "isb", "code_bias" }));
    EXPECT_EQ(rtSnapshotLatchesToApply(some, false), (std::vector<std::string>{ "isb" }));
    // The order is the writer's, whatever the map's.
    const std::map<std::string, bool> two = { { "clock_seed", true }, { "isb", true } };
    EXPECT_EQ(rtSnapshotLatchesToApply(two, false), (std::vector<std::string>{ "isb", "clock_seed" }));
}

TEST(RtStateSnapshotLatchesConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", realtimeConfig(""), cfg));
    EXPECT_FALSE(cfg.getRealTimeConfig().state_snapshot_latch_capture);
    EXPECT_FALSE(cfg.getRealTimeConfig().state_snapshot_latch_restore);
}

TEST(RtStateSnapshotLatchesConfig, BothKeysParse) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        realtimeConfig("    \"state_snapshot_path\": \"consumer_snapshot.txt\",\n"
                       "    \"state_snapshot_restore_scope\": \"full\",\n"
                       "    \"state_snapshot_latch_capture\": true,\n"
                       "    \"state_snapshot_latch_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getRealTimeConfig().state_snapshot_latch_capture);
    EXPECT_TRUE(cfg.getRealTimeConfig().state_snapshot_latch_restore);
    EXPECT_EQ(cfg.getRealTimeConfig().state_snapshot_restore_scope, "full");
}
