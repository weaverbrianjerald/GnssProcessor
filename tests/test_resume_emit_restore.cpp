/**
 * @file test_resume_emit_restore.cpp
 * @brief Contract tests for the emit-layer continuity card RS-5
 *        (docs/SPEC_event_harness.md §21.10): the snapshot's format-forward
 *        emit-layer records (@c ER: the emission frame of the Card-4 eq34 emit
 *        re-base), the engine's own export/restore, and the loader's side of the
 *        two knobs (@c network.state_snapshot_emit_capture,
 *        @c network.resume_emit_restore).
 *
 * The tests lock:
 *   (a) the engine's state round-trips through the snapshot text, with a
 *       '|'-bearing key and full-precision doubles, since the keys are
 *       "sat|signal";
 *   (b) a restored @ref NetworkEmitRebase re-bases its next epoch as the engine it
 *       was exported from does (the continuity claim: run N epochs on A, export,
 *       restore into B, feed both the same next epoch);
 *   (c) a snapshot with no @c ER rows restores nothing (an older file keeps the
 *       cold-start behaviour, and the container stays empty);
 *   (d) with the write knob off none of the rows is written, so the file matches
 *       the format before card RS-5 byte for byte; arming it adds only the rows
 *       and the END line count they advance.
 *
 * The seam mechanics (the emitted level continuing across a warm resume) are
 * graded on the harness legs, not here.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/emit/NetworkEmitRebase.hpp"
#include "products/NetworkStateSnapshot.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kLamL1 = 0.19029367279836487;   // GPS L1 wavelength (m)

NetworkStateSnapshot baseSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 277290.0;
    s.wall_utc  = "2026-09-12T00:00:00Z";
    s.generation_id = 1399161629u;
    s.config_fingerprint = 0x545b8bdd2b638231ull;
    s.n_emitted = 604;
    s.prev_week = 2313;
    s.prev_tow  = 277290.0;
    return s;
}

/// A snapshot carrying one of each emit-layer record, with values chosen so a
/// shortest-round-trip decimal differs from a rounded one.
NetworkStateSnapshot emitSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.emit_rebase_last["G01|L1C"] = -18.410123456789012;
    s.emit_rebase_last["E11|L5Q"] = 3.0517578125e-05;
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("resumeemit_cfg_" + tag + ".json");
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

/// Drive @p eng for one epoch over two GPS L1 keys at the given biases.
std::vector<NetworkEmitRebase::Entry> rebaseStep(NetworkEmitRebase& eng,
                                                 double b1, double b2) {
    std::vector<NetworkEmitRebase::Entry> ents(2);
    ents[0].sat = "G01"; ents[0].signal = "L1C";
    ents[1].sat = "G02"; ents[1].signal = "L1C";
    for (auto& e : ents) { e.component = 0; e.lambda_m = kLamL1; }
    ents[0].bias_m = b1;
    ents[1].bias_m = b2;
    eng.rebaseEpoch(ents);
    return ents;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) round trip through the snapshot text
// ---------------------------------------------------------------------------

TEST(ResumeEmitRestore, EmitRecordsRoundTripExactly) {
    const NetworkStateSnapshot s = emitSnapshot();
    ASSERT_TRUE(s.hasEmitState());
    // The emit rows use the existing v1 header (as in RS-3): on their own they
    // neither claim frame state nor move the file to v2/v3.
    ASSERT_FALSE(s.hasFrameState());
    ASSERT_FALSE(s.hasCouplingState());

    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u) << text.substr(0, 60);
    // The '|' in the key survives the comma-split format verbatim.
    EXPECT_NE(text.find("ER,G01|L1C,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    ASSERT_EQ(r.emit_rebase_last.size(), 2u);
    // Full precision: the shortest round-trippable decimal is written so the value
    // comes back bit for bit, because the restored value is the frame the next
    // epoch differences against.  EXPECT_DOUBLE_EQ checks it to within 4 ULP.
    EXPECT_DOUBLE_EQ(r.emit_rebase_last.at("G01|L1C"), -18.410123456789012);
    EXPECT_DOUBLE_EQ(r.emit_rebase_last.at("E11|L5Q"), 3.0517578125e-05);
    EXPECT_TRUE(r.hasEmitState());
}

/// strtod accepts a non-finite value, which would then corrupt every re-based
/// emitted bias for that key.  As in card CBP, it is a hard parse failure (the
/// caller's documented cold-start signal), never a silent partial restore.
TEST(ResumeEmitRestore, NonFiniteEmitFieldIsAHardParseFailure) {
    NetworkStateSnapshot s = baseSnapshot();
    s.emit_rebase_last["G01|L1C"] = 1.0;
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    std::string text = os.str();
    const std::size_t pos = text.find("ER,G01|L1C,");
    ASSERT_NE(pos, std::string::npos);
    const std::size_t eol = text.find('\n', pos);
    text.replace(pos, eol - pos, "ER,G01|L1C,nan");

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    EXPECT_NE(err.find("ER"), std::string::npos) << err;
}

// ---------------------------------------------------------------------------
// (b) the re-base engine continues across the export/restore
// ---------------------------------------------------------------------------

TEST(ResumeEmitRestore, EmitRebaseRestoredEngineRebasesTheNextEpochIdentically) {
    NetworkEmitRebase eng;
    // Three epochs of a two-satellite GPS L1 component whose raw solve frame walks
    // by a common fractional plus a per-satellite integer, the churn the engine
    // exists to remove, so `last_emit_` accumulates a real frame.
    rebaseStep(eng, 10.0, 20.0);
    rebaseStep(eng, 10.0 + 0.03 + 2 * kLamL1, 20.0 + 0.03 - 1 * kLamL1);
    const auto ep3 = rebaseStep(eng, 10.0 + 0.07 - 3 * kLamL1, 20.0 + 0.07 + 5 * kLamL1);
    // The re-base holds the emitted values on the first epoch's frame: at epoch 3
    // both the integer steps and the common 0.07 m drift are absent (10.0 and 20.0
    // to 1e-9).
    EXPECT_NEAR(ep3[0].bias_m, 10.0, 1e-9);
    EXPECT_NEAR(ep3[1].bias_m, 20.0, 1e-9);

    const auto st = eng.exportState();
    ASSERT_EQ(st.size(), 2u);
    double v = 0.0;
    ASSERT_TRUE(eng.lastEmittedForTest("G01", "L1C", v));
    EXPECT_DOUBLE_EQ(st.at("G01|L1C"), v);

    // A fresh engine (the resumed run) restored from the export re-bases the next
    // epoch as the original does (EXPECT_DOUBLE_EQ); an unrestored one passes the
    // raw solve-frame value through instead, which is the field seam of §21.10 of
    // SPEC_event_harness in miniature.
    NetworkEmitRebase resumed, cold;
    resumed.restoreState(st);
    const double n1 = 10.0 + 0.11 + 1 * kLamL1, n2 = 20.0 + 0.11 - 2 * kLamL1;
    const auto a = rebaseStep(eng,     n1, n2);
    const auto b = rebaseStep(resumed, n1, n2);
    const auto c = rebaseStep(cold,    n1, n2);
    EXPECT_DOUBLE_EQ(b[0].bias_m, a[0].bias_m);
    EXPECT_DOUBLE_EQ(b[1].bias_m, a[1].bias_m);
    EXPECT_GT(std::abs(c[0].bias_m - a[0].bias_m), 0.1)
        << "a cold-started re-base must NOT reproduce the continuous frame";
    // Counters are run telemetry, not frame state: the restore leaves them at zero.
    EXPECT_EQ(resumed.counters().epochs, 1);
    EXPECT_EQ(eng.counters().epochs, 4);
}

// ---------------------------------------------------------------------------
// (c) / (d) absence and the write knob
// ---------------------------------------------------------------------------

/// A file with no emit-layer rows restores nothing: an old snapshot (or a writer
/// without the capture knob) leaves the engine cold-starting as it does without a
/// restore.
TEST(ResumeEmitRestore, SnapshotWithoutEmitRowsRestoresNothing) {
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasEmitState());
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.find("\nER,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_TRUE(r.emit_rebase_last.empty());
    EXPECT_FALSE(r.hasEmitState());

    // An engine driven for one epoch exports its two keys.  restoreState is not
    // called here, so the claim that restoring an empty export is a no-op (not a
    // wipe of a live frame) is not asserted by this test.
    NetworkEmitRebase reb;
    rebaseStep(reb, 10.0, 20.0);
    const auto before = reb.exportState();
    ASSERT_EQ(before.size(), 2u);
}

/// The driver gates the capture on the write knob, so with it off the containers
/// stay empty and the file the same snapshot produces is byte-identical to the
/// format before card RS-5 (standing invariant #6, at the format level).
TEST(ResumeEmitRestore, WriteKnobOffEmitsNoneOfTheRows) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nocapture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_emit_capture);

    // With the knob off the driver writes a snapshot whose emit containers were
    // never filled.  It is compared against the same snapshot with one ER entry
    // filled.
    const NetworkStateSnapshot off = baseSnapshot();
    std::ostringstream a;
    NetworkStateSnapshotIO::serialize(a, off);
    NetworkStateSnapshot on = baseSnapshot();
    on.emit_rebase_last["G01|L1C"] = 1.0;
    std::ostringstream b;
    NetworkStateSnapshotIO::serialize(b, on);
    ASSERT_NE(a.str(), b.str());
    // Line-wise the differences are the one ER row and the END line count it
    // advances; nothing else in the file moves when the capture is armed.
    const auto lines = [](const std::string& t) {
        std::vector<std::string> v;
        std::istringstream is(t);
        for (std::string ln; std::getline(is, ln); ) v.push_back(ln);
        return v;
    };
    const auto la = lines(a.str()), lb = lines(b.str());
    ASSERT_EQ(la.size() + 1u, lb.size());
    std::vector<std::string> diff;
    std::size_t ia = 0;
    for (const auto& ln : lb) {
        if (ia < la.size() && la[ia] == ln) { ++ia; continue; }
        diff.push_back(ln);
    }
    EXPECT_EQ(ia + 1u, la.size()) << "only the END line may also differ";
    ASSERT_EQ(diff.size(), 2u);
    EXPECT_EQ(diff[0], "ER,G01|L1C,1");
    EXPECT_EQ(diff[1].rfind("END,", 0), 0u) << diff[1];
}

TEST(ResumeEmitRestoreConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_emit_capture);
    EXPECT_FALSE(cfg.networkResumeEmitRestore());
}

TEST(ResumeEmitRestoreConfig, CaptureKnobParsesAndDoesNotArmTheReadSide) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_emit_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_emit_capture);
    EXPECT_FALSE(cfg.networkResumeEmitRestore());
}

/// The read knob is only meaningful when the realization frame underneath it also
/// continues, as with the prerequisite of @c resume_datum_restore.
TEST(ResumeEmitRestoreConfig, RestoreWithoutFrameRestoreIsRejected) {
    // The loader turns its own throw into a failed load (loadFromJson catches and
    // returns false), the form in which every other restore prerequisite is asserted.
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("noframe",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_emit_restore\": true,\n"), cfg));
}

TEST(ResumeEmitRestoreConfig, ArmedPostureLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_frame_capture\": true,\n"
                      "    \"state_snapshot_emit_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_emit_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_emit_capture);
    EXPECT_TRUE(cfg.networkResumeEmitRestore());
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
}

/// Armed with no snapshot path, the loader warns and still loads (inert, not
/// fatal), as it does for every other resume-response knob.
TEST(ResumeEmitRestoreConfig, ArmedWithoutASnapshotPathStillLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inert",
        networkConfig("    \"state_snapshot_emit_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_emit_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeEmitRestore());
}
