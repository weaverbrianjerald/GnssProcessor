/**
 * @file test_network_live_record.cpp
 * @brief The live record (network/diag/LiveRecord.hpp): its JSON writer, the header and epoch
 *        lines, and the order in which the notes and the observer write an epoch's line (a
 *        skipped epoch written at the next epoch's first note, a late note as a second line,
 *        a pending line at close, nothing while closed).
 */

#include "network/diag/LiveRecord.hpp"

#include "network/driver/RunTypes.hpp"   // EpochStatusRow

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// The lines of a text file.
std::vector<std::string> readLines(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

/// A fresh file path under the temporary directory.
fs::path freshPath(const std::string& tag) {
    const fs::path dir = fs::temp_directory_path() / "network_live_record_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path p = dir / (tag + ".jsonl");
    fs::remove(p, ec);
    return p;
}

GnssTime at(double tow) { return GnssTime{2419, tow}; }

EpochStatusRow statusWith(int n_present, int n_clock) {
    EpochStatusRow r;
    r.n_present = n_present;
    r.n_clock_sats = n_clock;
    return r;
}

/// A line's values with one present station, one absent, and one satellite without a clock.
LiveEpochValues sampleValues() {
    LiveEpochValues v;
    v.s0_code = 0.28812;
    v.s0_phase = 0.17549;
    LiveStationValues a;
    a.present = true;
    a.code_rms_m = 0.25114;
    a.phase_rms_m = 0.001023;
    a.n_sats = 12;
    a.code_scale = 0.2911;
    a.max_norm = 4.123;
    a.zwd_m = 0.14321;
    a.clk_sd_m = 0.0081;
    v.stations = {a, LiveStationValues{}};
    LiveSatelliteValues s;
    s.sat = "G05";
    s.code_rms_m = 0.231;
    s.n_rx = 40;
    v.satellites = {s};
    return v;
}

}  // namespace

TEST(NetworkLiveRecord, JsonStringEscapesQuotesBackslashesAndControls) {
    EXPECT_EQ(liveJsonString("plain"), "\"plain\"");
    EXPECT_EQ(liveJsonString("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(liveJsonString("x\ny\tz"), "\"x\\ny\\tz\"");
    EXPECT_EQ(liveJsonString(std::string("\x01", 1)), "\"\\u0001\"");
    EXPECT_EQ(liveJsonString("\xcf\x83" "0"), "\"\xcf\x83" "0\"");   // UTF-8 passes through
}

TEST(NetworkLiveRecord, CutKeepsWholeUtf8Characters) {
    EXPECT_EQ(liveCutUtf8("abcdef", 10), "abcdef");
    EXPECT_EQ(liveCutUtf8("abcdef", 3), "abc");
    // "a" then the two bytes of sigma: a cut after 2 bytes would split it, so it stops at 1.
    EXPECT_EQ(liveCutUtf8("a\xcf\x83" "b", 2), "a");
    EXPECT_EQ(liveCutUtf8("a\xcf\x83" "b", 3), "a\xcf\x83");
}

TEST(NetworkLiveRecord, HeaderNamesTheStationsAndTheFields) {
    const std::string h = formatLiveHeader({{"ABMF00GLP", 16.262321, -61.527531}, {"WTZZ00DEU", 49.144, 12.879}});
    EXPECT_EQ(h,
              "{\"kind\":\"header\",\"format\":\"netview-live/1\",\"stations\":["
              "{\"name\":\"ABMF00GLP\",\"lat\":16.26232,\"lon\":-61.52753},"
              "{\"name\":\"WTZZ00DEU\",\"lat\":49.144,\"lon\":12.879}],"
              "\"rx_fields\":[\"code_rms_m\",\"phase_rms_m\",\"n_sats\",\"code_scale\",\"max_norm\",\"zwd_m\",\"clk_sd_m\"],"
              "\"sat_fields\":[\"code_rms_m\",\"n_rx\",\"clk_m\",\"disc\"]}");
}

TEST(NetworkLiveRecord, EpochLineCarriesEveryPartAndNullsWhatIsAbsent) {
    LiveEpochLine L;
    L.t = at(345630.0);
    L.disposition = "emitted";
    L.n_present = 56;
    L.n_clock_sats = 61;
    L.user = std::array<double, 4>{0.01234, 0.0041, -0.00624, 0.00981};
    L.wall_ms = 812.04;
    L.values = sampleValues();
    L.events.push_back({"T1_if_slip", "slip", "G05", "ABMF00GLP", "reset", "k=4 \"q\"", 1.234567891, std::nullopt});
    EXPECT_EQ(formatLiveLine(L),
              "{\"t\":[2419,345630],\"disp\":\"emitted\",\"npres\":56,\"nclk\":61,\"s0\":[0.2881,0.1755],"
              "\"user\":[0.0123,0.0041,-0.0062,0.0098],\"wall_ms\":812,"
              "\"rx\":[[0.2511,0.00102,12,0.291,4.12,0.1432,0.0081],null],"
              "\"sat\":{\"G05\":[0.231,40,null,null]},"
              "\"ev\":[[\"T1_if_slip\",\"slip\",\"G05\",\"ABMF00GLP\",\"reset\",\"k=4 \\\"q\\\"\",1.234568,null]]}");
}

TEST(NetworkLiveRecord, SkippedLineHasOnlyItsNotes) {
    LiveEpochLine L;
    L.t = at(345660.5);
    L.disposition = "sync_skip";
    EXPECT_EQ(formatLiveLine(L), "{\"t\":[2419,345660.5],\"disp\":\"sync_skip\"}");
}

TEST(NetworkLiveRecord, NotesAndObserverWriteOneLinePerEpochInOrder) {
    const fs::path p = freshPath("order");
    NetworkLiveRecord rec;
    rec.open(p, {{"ABMF00GLP", 16.0, -61.0}, {"WTZZ00DEU", 49.0, 12.0}}, nullptr);
    ASSERT_TRUE(rec.on());

    // Epoch 1: status, an event, then the observer.
    rec.noteStatus(at(30.0), "emitted", statusWith(2, 30));
    rec.noteEvent(at(30.0), "T3_raim", "flag", "G05", "ABMF00GLP", 5.1, 4.0, "refloat", "");
    rec.writeEpoch(at(30.0), sampleValues());
    // Epoch 2 ends early (no observer call): written at epoch 3's first note.
    rec.noteStatus(at(60.0), "sync_skip", EpochStatusRow{});
    rec.noteEvent(at(60.0), "GATE", "skip", "", "", std::nullopt, std::nullopt, "skip", "no stations");
    EXPECT_EQ(readLines(p).size(), 2u) << "the skipped epoch waits for a later note";
    rec.noteUser(at(90.0), 0.02, Eigen::Vector3d(0.01, -0.01, 0.015));
    rec.noteStatus(at(90.0), "emitted", statusWith(2, 31));
    rec.writeEpoch(at(90.0), sampleValues());
    // A note after its epoch's line is written becomes a second line of that epoch.
    rec.noteEvent(at(90.0), "CHAIN", "late", "E11", "", std::nullopt, std::nullopt, "note", "");
    rec.close();
    EXPECT_FALSE(rec.on());

    const std::vector<std::string> lines = readLines(p);
    ASSERT_EQ(lines.size(), 5u);
    EXPECT_EQ(lines[0].rfind("{\"kind\":\"header\"", 0), 0u);
    EXPECT_EQ(lines[1].rfind("{\"t\":[2419,30],\"disp\":\"emitted\",\"npres\":2,\"nclk\":30,\"s0\":", 0), 0u);
    EXPECT_NE(lines[1].find("\"ev\":[[\"T3_raim\",\"flag\",\"G05\",\"ABMF00GLP\",\"refloat\",\"\",5.1,4]]"),
              std::string::npos);
    EXPECT_EQ(lines[1].find("wall_ms"), std::string::npos) << "the first epoch line has no previous line";
    EXPECT_EQ(lines[2], "{\"t\":[2419,60],\"disp\":\"sync_skip\","
                        "\"ev\":[[\"GATE\",\"skip\",\"\",\"\",\"skip\",\"no stations\",null,null]]}");
    EXPECT_EQ(lines[3].rfind("{\"t\":[2419,90],\"disp\":\"emitted\",\"npres\":2,\"nclk\":31,\"s0\":", 0), 0u);
    EXPECT_NE(lines[3].find("\"user\":[0.02,0.01,-0.01,0.015]"), std::string::npos);
    EXPECT_NE(lines[3].find("\"wall_ms\":"), std::string::npos);
    EXPECT_EQ(lines[4], "{\"t\":[2419,90],\"ev\":[[\"CHAIN\",\"late\",\"E11\",\"\",\"note\",\"\",null,null]]}");
    EXPECT_EQ(rec.lines_written, 4);
}

TEST(NetworkLiveRecord, ClosedRecordIgnoresEveryNote) {
    NetworkLiveRecord rec;
    EXPECT_FALSE(rec.on());
    rec.noteStatus(at(30.0), "emitted", statusWith(2, 30));
    rec.noteEvent(at(30.0), "T3_raim", "flag", "G05", "ABMF00GLP", 5.1, 4.0, "refloat", "");
    rec.noteUser(at(30.0), 0.02, Eigen::Vector3d::Zero());
    rec.writeEpoch(at(30.0), sampleValues());
    rec.close();
    EXPECT_FALSE(rec.pending.has_value());
    EXPECT_EQ(rec.lines_written, 0);
}

TEST(NetworkLiveRecord, LongDetailIsCut) {
    const fs::path p = freshPath("detail");
    NetworkLiveRecord rec;
    rec.open(p, {}, nullptr);
    rec.noteEvent(at(30.0), "T1_if_slip", "slip", "G05", "X", std::nullopt, std::nullopt, "reset",
                  std::string(500, 'x'));
    ASSERT_TRUE(rec.pending.has_value());
    ASSERT_EQ(rec.pending->events.size(), 1u);
    EXPECT_EQ(rec.pending->events[0].detail.size(), kLiveDetailBytes);
    rec.close();
}
