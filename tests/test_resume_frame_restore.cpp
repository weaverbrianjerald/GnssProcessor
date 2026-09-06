/**
 * @file test_resume_frame_restore.cpp
 * @brief Contract tests for the RESUME FRAME RESTORE card
 *        (SPEC_synthetic_minimum_network §18.10): the snapshot format-v2 frame
 *        records (PA/PT phase-anchor incumbent + sticky pin target, CA clock-anchor
 *        per-satellite state, FA/FP/FE CLK-R held frame), their backward
 *        compatibility with v1 files, and the loader's side of the two knobs
 *        (@c network.resume_frame_restore, @c network.resume_slip_warmup_ep).
 *
 * What is locked here: (1) a snapshot carrying frame rows round-trips them
 * exactly and is written under the v2 header; (2) a snapshot carrying NONE is
 * written under the v1 header byte-for-byte (the unconditional capture cannot
 * change a run that has nothing to capture); (3) a v1 file parses with empty
 * frame maps (an old snapshot restores today's cold-pin behaviour); (4) the
 * CLK-R engine exports and restores its frame state exactly; (5) the knobs
 * default OFF / 0, a negative warm-up is rejected, and the warn-inert posture
 * (no snapshot path) still loads.  The seam mechanics themselves (the election
 * keeping its incumbents, the anchored first fold, the absent 1(d) erase) are
 * graded on the .ep1440 branch legs, not here.
 */
#include <gtest/gtest.h>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "products/NetworkClockEmitRebase.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

NetworkStateSnapshot baseSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 302400.0;
    s.wall_utc  = "2026-09-02T00:00:00Z";
    s.generation_id = 1399161629u;
    s.config_fingerprint = 0x35860c5f45b8a004ull;
    s.n_emitted = 1440;
    s.prev_week = 2313;
    s.prev_tow  = 302400.0;
    return s;
}

NetworkStateSnapshot frameSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.anchor_incumbent["G01_L1C"] = "S30000SIM";
    s.anchor_incumbent["E02_L5Q"] = "S07000SIM";
    s.anchor_pin_target["E02_L5Q"] = -28.981234567;   // a continuous-handover target
    NetworkStateSnapshot::ClockAnchorSat g01;
    g01.t_last = 1400000000.0 + 302400.0; g01.last_value = -0.9718706; g01.last_var_eff = 1.7e-5;
    g01.in_band = 0; g01.anchored = true;
    NetworkStateSnapshot::ClockAnchorSat g12;
    g12.t_last = 1400000000.0 + 302370.0; g12.last_value = 5.04; g12.last_var_eff = 0.08;
    g12.in_band = 2; g12.anchored = false;
    s.clock_anchor["G01"] = g01;
    s.clock_anchor["G12"] = g12;
    s.clk_frame_acc['G'] = 1.2178912315598864;
    s.clk_frame_acc['E'] = 0.2675;
    s.clk_frame_prev["G01"] = {0.0, 302400.0};
    s.clk_frame_prev["G12"] = {-5.04, 302400.0};
    s.clk_frame_epochs = 1440;
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("resumeframe_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

TEST(ResumeFrameRestoreTest, FrameRecordsRoundTripUnderV2Header) {
    const NetworkStateSnapshot s = frameSnapshot();
    ASSERT_TRUE(s.hasFrameState());
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v2\n", 0), 0u) << text.substr(0, 60);
    EXPECT_NE(text.find("PA,G01_L1C,S30000SIM\n"), std::string::npos);
    EXPECT_NE(text.find("PT,E02_L5Q,"), std::string::npos);
    EXPECT_NE(text.find("CA,G01,"), std::string::npos);
    EXPECT_NE(text.find("FA,G,"), std::string::npos);
    EXPECT_NE(text.find("FP,G12,"), std::string::npos);
    EXPECT_NE(text.find("FE,1440\n"), std::string::npos);

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_EQ(r.anchor_incumbent, s.anchor_incumbent);
    ASSERT_EQ(r.anchor_pin_target.size(), 1u);
    EXPECT_DOUBLE_EQ(r.anchor_pin_target.at("E02_L5Q"), -28.981234567);
    ASSERT_EQ(r.clock_anchor.size(), 2u);
    EXPECT_DOUBLE_EQ(r.clock_anchor.at("G01").t_last, s.clock_anchor.at("G01").t_last);
    EXPECT_DOUBLE_EQ(r.clock_anchor.at("G01").last_value, -0.9718706);
    EXPECT_DOUBLE_EQ(r.clock_anchor.at("G01").last_var_eff, 1.7e-5);
    EXPECT_TRUE(r.clock_anchor.at("G01").anchored);
    EXPECT_EQ(r.clock_anchor.at("G12").in_band, 2);
    EXPECT_FALSE(r.clock_anchor.at("G12").anchored);
    EXPECT_DOUBLE_EQ(r.clock_anchor.at("G12").last_var_eff, 0.08);
    ASSERT_EQ(r.clk_frame_acc.size(), 2u);
    EXPECT_DOUBLE_EQ(r.clk_frame_acc.at('G'), 1.2178912315598864);
    EXPECT_DOUBLE_EQ(r.clk_frame_acc.at('E'), 0.2675);
    ASSERT_EQ(r.clk_frame_prev.size(), 2u);
    EXPECT_DOUBLE_EQ(r.clk_frame_prev.at("G12").first, -5.04);
    EXPECT_DOUBLE_EQ(r.clk_frame_prev.at("G12").second, 302400.0);
    EXPECT_EQ(r.clk_frame_epochs, 1440);
    EXPECT_TRUE(r.hasFrameState());
}

/// §18.12 (2a): the dense D2 covariance side-car (QK keys + QP lower-triangle rows)
/// and the OPTIONAL L var_first field round-trip exactly and put the file under the
/// v3 header on their own; an L row without the field parses with var_first = 0 (a
/// pre-ratio file); a block missing a row is a HARD parse failure, never a partial
/// restore.
TEST(ResumeFrameRestoreTest, DenseCarryCovAndVarFirstRoundTripUnderV3) {
    NetworkStateSnapshot s = baseSnapshot();
    s.local_carry[{3, "G01", "L1C"}] = NetworkStateSnapshot::Local{12.5, 2.2e-3, 302400.0, 0.81};
    s.local_carry[{4, "E02", "L5Q"}] = NetworkStateSnapshot::Local{-3.0, 1.0e-2, 302370.0};
    s.carry_cov_keys = {"G01|CLK", "G01|CB|C1C", "E02|PB|L5Q"};
    s.carry_cov_rows = {{1.0e-4}, {2.0e-5, 3.0e-4}, {-1.0e-6, 4.0e-6, 5.0e-4}};
    ASSERT_TRUE(s.hasCouplingState());
    ASSERT_FALSE(s.hasFrameState());
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v3\n", 0), 0u) << text.substr(0, 60);
    EXPECT_NE(text.find("L,3,G01,L1C,12.5,0.0022,302400,0.81\n"), std::string::npos) << text;
    EXPECT_NE(text.find("L,4,E02,L5Q,-3,0.01,302370\n"), std::string::npos)
        << "an unknown var_first writes the legacy 6-value L row";
    EXPECT_NE(text.find("QK,0,G01|CLK\n"), std::string::npos);
    EXPECT_NE(text.find("QK,2,E02|PB|L5Q\n"), std::string::npos);
    EXPECT_NE(text.find("QP,0,1,0.0001\n"), std::string::npos);
    EXPECT_NE(text.find("QP,2,3,-1e-06,4e-06,0.0005\n"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_EQ(r.carry_cov_keys, s.carry_cov_keys);
    ASSERT_EQ(r.carry_cov_rows.size(), 3u);
    ASSERT_EQ(r.carry_cov_rows[1].size(), 2u);
    EXPECT_DOUBLE_EQ(r.carry_cov_rows[1][0], 2.0e-5);
    EXPECT_DOUBLE_EQ(r.carry_cov_rows[2][2], 5.0e-4);
    EXPECT_DOUBLE_EQ(r.local_carry.at({3, "G01", "L1C"}).var_first, 0.81);
    EXPECT_DOUBLE_EQ(r.local_carry.at({3, "G01", "L1C"}).var, 2.2e-3);
    EXPECT_DOUBLE_EQ(r.local_carry.at({4, "E02", "L5Q"}).var_first, 0.0);
    EXPECT_TRUE(r.hasCouplingState());

    // A block missing one row is refused WHOLE.
    NetworkStateSnapshot bad = s;
    bad.carry_cov_rows.pop_back();
    std::ostringstream ob;
    NetworkStateSnapshotIO::serialize(ob, bad);
    NetworkStateSnapshot rb;
    std::istringstream ib(ob.str());
    EXPECT_FALSE(NetworkStateSnapshotIO::parse(ib, rb, &err));
    EXPECT_NE(err.find("QK/QP"), std::string::npos) << err;
}

TEST(ResumeFrameRestoreTest, NoFrameStateWritesV1HeaderByteIdentical) {
    // The unconditional capture must not change a snapshot that has nothing to
    // capture: same header, same bytes as the pre-card writer.
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasFrameState());
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u);
    EXPECT_EQ(text.find("\nPA,"), std::string::npos);
    EXPECT_EQ(text.find("\nPT,"), std::string::npos);
    EXPECT_EQ(text.find("\nCA,"), std::string::npos);
    EXPECT_EQ(text.find("\nFA,"), std::string::npos);
    EXPECT_EQ(text.find("\nFP,"), std::string::npos);
    EXPECT_EQ(text.find("\nFE,"), std::string::npos);
    // §18.12: the format-v3 Card-1 coupling records belong in the same absence
    // list — the coupling capture is knob-gated and its containers stay
    // default-constructed, so hasCouplingState() is false, the three-way header
    // selector still yields v1, the four emit loops iterate zero times and the
    // END,<n> trailer is unchanged.  This is the OFF-path byte-identity lock.
    EXPECT_EQ(text.find("\nQS,"), std::string::npos);
    EXPECT_EQ(text.find("\nQL,"), std::string::npos);
    EXPECT_EQ(text.find("\nQZ,"), std::string::npos);
    EXPECT_EQ(text.find("\nQR,"), std::string::npos);
}

TEST(ResumeFrameRestoreTest, V1FileParsesWithEmptyFrameMaps) {
    // An old (pre-card) snapshot restores today's cold-pin behaviour: every
    // frame map empty, hasFrameState() false — the armed knob logs INERT.
    const NetworkStateSnapshot s = baseSnapshot();
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(os.str());
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_TRUE(r.anchor_incumbent.empty());
    EXPECT_TRUE(r.anchor_pin_target.empty());
    EXPECT_TRUE(r.clock_anchor.empty());
    EXPECT_TRUE(r.clk_frame_acc.empty());
    EXPECT_TRUE(r.clk_frame_prev.empty());
    EXPECT_EQ(r.clk_frame_epochs, 0);
    EXPECT_FALSE(r.hasFrameState());
    EXPECT_EQ(r.n_emitted, 1440u);
}

TEST(ResumeFrameRestoreTest, MalformedFrameRecordsFailParse) {
    const NetworkStateSnapshot s = frameSnapshot();
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    std::string text = os.str();
    // Corrupt one CA line's arity: the strict parser must refuse (cold start),
    // never restore a half-read anchor state.
    const auto pos = text.find("CA,G01,");
    ASSERT_NE(pos, std::string::npos);
    const auto eol = text.find('\n', pos);
    text.replace(pos, eol - pos, "CA,G01,1.0,2.0");
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    EXPECT_NE(err.find("CA"), std::string::npos) << err;
}

TEST(ResumeFrameRestoreTest, ClockEmitRebaseFrameStateExportsAndRestoresExactly) {
    NetworkClockEmitRebase::Config cfg;
    cfg.frame_hold = true;
    cfg.jump_test  = false;
    cfg.min_common = 1;
    NetworkClockEmitRebase eng(cfg);
    // Two epochs of a two-sat GPS constellation: the second epoch's common step
    // (+0.5 m on both) drains into A_G; per-sat baselines advance.
    std::vector<NetworkClockEmitRebase::Entry> ep;
    NetworkClockEmitRebase::Entry a, b;
    a.sat = "G01"; a.c0_m = 1.0;
    b.sat = "G02"; b.c0_m = 2.0;
    std::set<char> re;
    ep = {a, b};
    eng.rebaseEpoch(ep, 1000.0, re);
    a.c0_m = 1.5; b.c0_m = 2.5;
    ep = {a, b};
    eng.rebaseEpoch(ep, 1030.0, re);
    ASSERT_EQ(eng.counters().epochs, 2);
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.5, 1e-12);

    const auto fs = eng.exportFrameState();
    EXPECT_EQ(fs.epochs, 2);
    ASSERT_EQ(fs.acc.size(), 1u);
    EXPECT_NEAR(fs.acc.at('G'), 0.5, 1e-12);
    ASSERT_EQ(fs.prev.size(), 2u);
    EXPECT_NEAR(fs.prev.at("G01").first, 1.0, 1e-12);   // held = c0 - A_G
    EXPECT_DOUBLE_EQ(fs.prev.at("G01").second, 1030.0);

    // A fresh engine (a resumed run) restored from the export continues the
    // frame: same accumulator, same baselines, epochs > 0 (the F1 seam rule,
    // keyed on epochs == 0, does not fire).
    NetworkClockEmitRebase fresh(cfg);
    EXPECT_EQ(fresh.counters().epochs, 0);
    fresh.restoreFrameState(fs);
    EXPECT_EQ(fresh.counters().epochs, 2);
    EXPECT_NEAR(fresh.frameAccForTest('G'), 0.5, 1e-12);
    double v = 0.0;
    ASSERT_TRUE(fresh.prevEmittedForTest("G02", v));
    EXPECT_NEAR(v, 2.0, 1e-12);
    // The next epoch on the restored engine equals the next epoch on the
    // original: a further common +0.25 m drains into A_G on both.
    a.c0_m = 1.75; b.c0_m = 2.75;
    std::vector<NetworkClockEmitRebase::Entry> ep1 = {a, b}, ep2 = {a, b};
    std::set<char> r1, r2;
    eng.rebaseEpoch(ep1, 1060.0, r1);
    fresh.rebaseEpoch(ep2, 1060.0, r2);
    EXPECT_NEAR(eng.frameAccForTest('G'), fresh.frameAccForTest('G'), 1e-12);
    EXPECT_NEAR(ep1[0].c0_m, ep2[0].c0_m, 1e-12);
    EXPECT_NEAR(ep1[1].c0_m, ep2[1].c0_m, 1e-12);
}

TEST(ResumeFrameRestoreConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkResumeFrameRestore());
    EXPECT_EQ(cfg.networkResumeSlipWarmupEp(), 0);
    // The write-side capture is OFF too: a default snapshot stays format v1.
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_frame_capture);
}

TEST(ResumeFrameRestoreConfig, CaptureKnobParses) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_frame_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_frame_capture);
    EXPECT_FALSE(cfg.networkResumeFrameRestore());   // write side does not arm the read side
}

TEST(ResumeFrameRestoreConfig, ArmedPostureLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_slip_warmup_ep\": 5,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
    EXPECT_EQ(cfg.networkResumeSlipWarmupEp(), 5);
}

TEST(ResumeFrameRestoreConfig, NegativeWarmupIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("negative_warmup",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_slip_warmup_ep\": -1,\n"), cfg));
}

TEST(ResumeFrameRestoreConfig, ArmedWithoutSnapshotPathIsWarnInert) {
    // Wiring-inert, not illegal: the loader warns and the run proceeds.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("no_path",
        networkConfig("    \"resume_frame_restore\": true,\n"
                      "    \"resume_slip_warmup_ep\": 3,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
    EXPECT_EQ(cfg.networkResumeSlipWarmupEp(), 3);
}

// --- READMIT continuity (E2 seed-first re-admission + the block re-coupling edge) ---
// The loader side of the two knobs: default OFF / 0 (the byte-identity contract),
// both parse, negative values are refused.  The seam mechanics (parking at a
// departure, the seed at re-registration, the coupling window) are graded on the
// sim bed's eclipse schedule, not here.

TEST(ReadmitContinuityConfig, DefaultsOffAndKnobsParse) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("readmit_default", networkConfig(), cfg));
    EXPECT_EQ(cfg.networkReadmitSeedHorizonS(), 0.0);
    EXPECT_EQ(cfg.networkReadmitCouplingMargEp(), 0);
    GnssConfig on;
    ASSERT_TRUE(loadInto("readmit_on",
        networkConfig("    \"clock_temporal_anchor\": true,\n"
                      "    \"readmit_seed_horizon_s\": 3600.0,\n"
                      "    \"readmit_coupling_marg_ep\": 20,\n"), on));
    EXPECT_DOUBLE_EQ(on.networkReadmitSeedHorizonS(), 3600.0);
    EXPECT_EQ(on.networkReadmitCouplingMargEp(), 20);
}

TEST(ReadmitContinuityConfig, NegativeValuesAreRejected) {
    GnssConfig a;
    EXPECT_FALSE(loadInto("readmit_neg_ep",
        networkConfig("    \"readmit_coupling_marg_ep\": -1,\n"), a));
    GnssConfig b;
    EXPECT_FALSE(loadInto("readmit_neg_h",
        networkConfig("    \"readmit_seed_horizon_s\": -5.0,\n"), b));
}
