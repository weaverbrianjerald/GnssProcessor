/**
 * @file test_resume_datum_restore.cpp
 * @brief Contract tests for the RS-3 DATUM-CONTINUITY card
 *        (docs/SPEC_event_harness.md §21.7): the snapshot's format-forward datum
 *        records (@c EQD / @c EQP / @c EQS / @c EQT / @c EQE / @c EQR for the eq34
 *        chain datum's last-epoch spanning tree, @c CBP / @c CBS for the time-linked
 *        code-bias datum memory), their backward compatibility with every earlier
 *        file, and the loader's side of the two knobs
 *        (@c network.state_snapshot_datum_capture, @c network.resume_datum_restore).
 *
 * What is locked here: (1) every map round-trips EXACTLY — including a key holding a
 * '|' (the eq34 "<station>|<cons>|<signal>" receiver key) and a full station name, and
 * doubles at full precision; (2) a snapshot carrying NONE of the rows is written
 * byte-for-byte as before and parses back with every container empty, which is the
 * "reader that finds no such rows behaves exactly as today" contract and the OFF-path
 * byte-identity lock; (3) the knobs default OFF, @c resume_datum_restore hard-requires
 * @c resume_frame_restore, and the warn-inert postures still load; (4) a malformed or
 * non-finite record is a HARD parse failure, never a silent partial restore.
 *
 * The seam mechanics themselves — the election's sticky seed continuing a RESTORED
 * tree, and @c code_bias_datum_resumed_ staying false so a re-seed bumps only where it
 * genuinely re-seeds — are graded on the ep0604 resume legs, not here.
 */
#include <gtest/gtest.h>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"
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
    s.snap_tow  = 277290.00000810623;
    s.wall_utc  = "2026-09-12T16:56:14Z";
    s.generation_id = 0u;
    s.config_fingerprint = 0x545b8bdd2b638231ull;
    s.n_emitted = 604;
    s.prev_week = 2313;
    s.prev_tow  = 277290.0;
    return s;
}

/// A datum-bearing snapshot shaped like the field seed's: a two-signal GPS group, a
/// receiver key carrying '|' and a full 9-character RINEX station name, one interior
/// satellite pinned at two different stations (the per-station key the pin target MUST
/// keep), and a code-bias family with two satellites.
NetworkStateSnapshot datumSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.eq34_datum_rx["G|L1C"] = "GOPE00CZE";
    s.eq34_datum_rx["G|L2W"] = "WTZR00DEU";
    s.eq34_rx_parent["GOPE00CZE|G|L1C"] = "G24";
    s.eq34_rx_parent["WTZR00DEU|G|L2W"] = "G07";
    s.eq34_sat_root["G24_L1C"] = "GOPE00CZE";
    s.eq34_sat_root["G07_L2W"] = "WTZR00DEU";
    // Same ambiguity, two stations, two targets — the collision the per-station key
    // exists to prevent (an amb-only key measured a 4e8-cycle blow-up).
    s.eq34_prev_target[{3, "G24_L1C"}]  = -118371.98765432109;
    s.eq34_prev_target[{17, "G24_L1C"}] = 44021.0;
    s.eq34_prev_el[{3, "G24"}]  = 61.284999999999997;
    s.eq34_prev_el[{17, "G07"}] = 12.5;
    s.eq34_retained_edges.emplace(3, "G24_L1C");
    s.eq34_retained_edges.emplace(17, "G07_L2W");
    s.code_bias_prev["G|C1W"]["G24"] = -1.2345678901234567;
    s.code_bias_prev["G|C1W"]["G07"] = 0.5;
    s.code_bias_prev["G|C2W"]["G24"] = 3.75e-3;
    s.code_bias_sat_seen.insert("G24");
    s.code_bias_sat_seen.insert("G07");
    s.code_bias_sat_seen.insert("G12");
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("resumedatum_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
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

/// Round-trip a snapshot through serialize + parse, returning the text it wrote.
std::string roundTrip(const NetworkStateSnapshot& s, NetworkStateSnapshot& out) {
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    std::string err;
    std::istringstream is(text);
    EXPECT_TRUE(NetworkStateSnapshotIO::parse(is, out, &err)) << err;
    return text;
}

}  // namespace

// (a) Every map round-trips EXACTLY, keys with '|' and station names included.
TEST(ResumeDatumRestore, DatumRecordsRoundTripExactly) {
    const NetworkStateSnapshot s = datumSnapshot();
    ASSERT_TRUE(s.hasDatumState());
    NetworkStateSnapshot r;
    const std::string text = roundTrip(s, r);

    // The wire shapes, spelled out — a silent field-order change would otherwise only
    // surface on a cross-build resume.
    EXPECT_NE(text.find("EQD,G|L1C,GOPE00CZE\n"), std::string::npos) << text;
    EXPECT_NE(text.find("EQP,GOPE00CZE|G|L1C,G24\n"), std::string::npos) << text;
    EXPECT_NE(text.find("EQS,G24_L1C,GOPE00CZE\n"), std::string::npos) << text;
    EXPECT_NE(text.find("EQT,3,G24_L1C,-118371.98765432108\n"), std::string::npos) << text;
    EXPECT_NE(text.find("EQT,17,G24_L1C,44021\n"), std::string::npos) << text;
    EXPECT_NE(text.find("EQE,3,G24,61.285\n"), std::string::npos) << text;
    EXPECT_NE(text.find("EQR,17,G07_L2W\n"), std::string::npos) << text;
    EXPECT_NE(text.find("CBP,G|C1W,G24,-1.2345678901234567\n"), std::string::npos) << text;
    EXPECT_NE(text.find("CBS,G12\n"), std::string::npos) << text;

    EXPECT_EQ(r.eq34_datum_rx, s.eq34_datum_rx);
    EXPECT_EQ(r.eq34_rx_parent, s.eq34_rx_parent);
    EXPECT_EQ(r.eq34_sat_root, s.eq34_sat_root);
    EXPECT_EQ(r.eq34_retained_edges, s.eq34_retained_edges);
    EXPECT_EQ(r.code_bias_sat_seen, s.code_bias_sat_seen);
    // Doubles compared exactly: fmt's shortest round-trippable form must survive
    // strtod bit-for-bit, or a restored pin target re-levels a whole component.
    ASSERT_EQ(r.eq34_prev_target.size(), 2u);
    EXPECT_DOUBLE_EQ(r.eq34_prev_target.at({3, "G24_L1C"}), -118371.98765432109);
    EXPECT_DOUBLE_EQ(r.eq34_prev_target.at({17, "G24_L1C"}), 44021.0);
    ASSERT_EQ(r.eq34_prev_el.size(), 2u);
    EXPECT_DOUBLE_EQ(r.eq34_prev_el.at({3, "G24"}), 61.284999999999997);
    ASSERT_EQ(r.code_bias_prev.size(), 2u);
    EXPECT_DOUBLE_EQ(r.code_bias_prev.at("G|C1W").at("G24"), -1.2345678901234567);
    EXPECT_DOUBLE_EQ(r.code_bias_prev.at("G|C1W").at("G07"), 0.5);
    EXPECT_DOUBLE_EQ(r.code_bias_prev.at("G|C2W").at("G24"), 3.75e-3);
    EXPECT_TRUE(r.hasDatumState());
}

// The datum rows ride the EXISTING header literal (the optional-var_first precedent),
// so an RS-3 A/B can diff two snapshots and see only the new rows.
TEST(ResumeDatumRestore, DatumRecordsDoNotAdvanceTheHeader) {
    NetworkStateSnapshot s = datumSnapshot();
    ASSERT_FALSE(s.hasFrameState());
    ASSERT_FALSE(s.hasCouplingState());
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    EXPECT_EQ(os.str().rfind("# gnss_network_state_snapshot v1\n", 0), 0u)
        << os.str().substr(0, 60);

    // ...and they compose with the v2 frame group without disturbing its selector.
    s.anchor_incumbent["G24_L1C"] = "GOPE00CZE";
    ASSERT_TRUE(s.hasFrameState());
    std::ostringstream os2;
    NetworkStateSnapshotIO::serialize(os2, s);
    EXPECT_EQ(os2.str().rfind("# gnss_network_state_snapshot v2\n", 0), 0u)
        << os2.str().substr(0, 60);
}

// (b) + (c) A snapshot WITHOUT the rows is written byte-for-byte as before and parses
// back with every datum container empty — the "restores nothing" contract, which is
// also the write-knob-off byte-identity lock (the driver leaves the containers empty
// unless network.state_snapshot_datum_capture is armed).
TEST(ResumeDatumRestore, NoDatumRowsWhenUnpopulatedAndRestoresNothing) {
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasDatumState());
    NetworkStateSnapshot r;
    const std::string text = roundTrip(s, r);

    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u);
    for (const char* tag : {"\nEQD,", "\nEQP,", "\nEQS,", "\nEQT,",
                            "\nEQE,", "\nEQR,", "\nCBP,", "\nCBS,"})
        EXPECT_EQ(text.find(tag), std::string::npos) << tag;

    EXPECT_TRUE(r.eq34_datum_rx.empty());
    EXPECT_TRUE(r.eq34_rx_parent.empty());
    EXPECT_TRUE(r.eq34_sat_root.empty());
    EXPECT_TRUE(r.eq34_prev_target.empty());
    EXPECT_TRUE(r.eq34_prev_el.empty());
    EXPECT_TRUE(r.eq34_retained_edges.empty());
    EXPECT_TRUE(r.code_bias_prev.empty());
    EXPECT_TRUE(r.code_bias_sat_seen.empty());
    EXPECT_FALSE(r.hasDatumState());
}

// A pre-RS-3 (v1/v2/v3) file is still a legal input: the restore finds no rows, both
// halves report INERT, and the resume behaves exactly as it does today.
TEST(ResumeDatumRestore, PreCardFileParsesWithEmptyDatumMaps) {
    const std::string v1 =
        "# gnss_network_state_snapshot v1\n"
        "M,2313,277290,2026-09-12T16:56:14Z,7,545b8bdd2b638231\n"
        "N,604,0,0,0,0,0,0,3,2313,277290,277290,277200\n"
        "U,1,12,1,259230\n"
        "L,3,G24,L1C,12.5,0.0022,277290\n"
        "END,4\n";
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(v1);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_FALSE(r.hasDatumState());
    EXPECT_EQ(r.n_emitted, 604);
    EXPECT_EQ(r.local_carry.size(), 1u);
}

// A malformed or non-finite datum record is a HARD parse failure — the caller treats
// that as the cold-start signal, which is byte-identical to today, rather than
// resuming on half a spanning tree or a poisoned code-bias baseline.
TEST(ResumeDatumRestore, MalformedDatumRecordsAreHardParseFailures) {
    const std::string head =
        "# gnss_network_state_snapshot v1\n"
        "M,2313,277290,2026-09-12T16:56:14Z,7,545b8bdd2b638231\n"
        "N,604,0,0,0,0,0,0,3,2313,277290,277290,277200\n"
        "U,1,12,1,259230\n";
    struct Case { const char* row; const char* want; };
    const Case cases[] = {
        {"EQD,G|L1C\n",                  "EQD"},   // missing value
        {"EQP,GOPE00CZE|G|L1C,\n",       "EQP"},   // empty satellite
        {"EQS,,GOPE00CZE\n",             "EQS"},   // empty key
        {"EQT,3,G24_L1C,notanumber\n",   "EQT"},   // unparseable target
        {"EQE,x,G24,61.285\n",           "EQE"},   // unparseable station index
        {"EQR,3\n",                      "EQR"},   // missing ambiguity
        {"CBP,G|C1W,G24,nan\n",          "CBP"},   // non-finite previous value
        {"CBS,\n",                       "CBS"},   // empty satellite
    };
    for (const Case& c : cases) {
        const std::string text = head + c.row + "END,4\n";
        NetworkStateSnapshot r;
        std::string err;
        std::istringstream is(text);
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err)) << c.row;
        EXPECT_NE(err.find(c.want), std::string::npos) << c.row << " -> " << err;
    }
}

// The loader: both knobs default OFF, and the read knob hard-requires the frame
// restore (anchor_incumbent is the same tree's satellite side).
TEST(ResumeDatumRestore, KnobsDefaultOffAndReadRequiresFrameRestore) {
    GnssConfig def;
    ASSERT_TRUE(loadInto("def", networkConfig(), def));
    EXPECT_FALSE(def.getNetworkConfig().state_snapshot_datum_capture);
    EXPECT_FALSE(def.networkResumeDatumRestore());

    GnssConfig both;
    ASSERT_TRUE(loadInto("both", networkConfig(
        "    \"state_snapshot_path\": \"snap.txt\",\n"
        "    \"state_snapshot_datum_capture\": true,\n"
        "    \"state_snapshot_frame_capture\": true,\n"
        "    \"resume_frame_restore\": true,\n"
        "    \"resume_datum_restore\": true,\n"), both));
    EXPECT_TRUE(both.getNetworkConfig().state_snapshot_datum_capture);
    EXPECT_TRUE(both.networkResumeDatumRestore());

    // resume_datum_restore without resume_frame_restore is refused.  The loader turns
    // its own throw into a FAILED LOAD (loadFromJson catches and returns false), which
    // is how every coupling-restore prerequisite is asserted too.
    GnssConfig bad;
    EXPECT_FALSE(loadInto("bad", networkConfig(
        "    \"state_snapshot_path\": \"snap.txt\",\n"
        "    \"resume_datum_restore\": true,\n"), bad));
}

// Both warn-inert postures (armed with no snapshot path) still LOAD — they are
// warnings, not errors, exactly like the frame-capture pattern they mirror.
TEST(ResumeDatumRestore, ArmedWithoutSnapshotPathStillLoads) {
    GnssConfig w;
    ASSERT_TRUE(loadInto("warn", networkConfig(
        "    \"state_snapshot_datum_capture\": true,\n"
        "    \"resume_frame_restore\": true,\n"
        "    \"resume_datum_restore\": true,\n"), w));
    EXPECT_TRUE(w.getNetworkConfig().state_snapshot_datum_capture);
    EXPECT_TRUE(w.networkResumeDatumRestore());
}
