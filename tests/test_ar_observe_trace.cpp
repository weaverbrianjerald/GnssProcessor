/**
 * @file test_ar_observe_trace.cpp
 * @brief The two trace files of the observe-mode search: their headers (against the frozen
 *        consumer grader), the row formatters, the ENU errors, the run-end line and the
 *        network run's writer.
 */

#include <gtest/gtest.h>

#include "network/diag/RunTraces.hpp"
#include "realtime/ArObserve.hpp"
#include "support/TestTempDir.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == ',' || ch == '\n') { out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

ArFixRow sampleRow() {
    ArFixRow r;
    r.system = 'G';
    r.signal = "L1C";
    r.sat = "G05";
    r.ref_sat = "G01";
    r.cmp = 1;
    r.float_cyc = -39.9876543;
    r.cond_sigma_cyc = 0.0123456;
    r.fixed_int = -40;
    r.determined = true;
    r.kept = true;
    r.gen_sat = 7;
    r.gen_ref = 9;
    return r;
}

ArEpochSummary sampleSummary() {
    ArEpochSummary s;
    s.n_entries = 4;
    s.n_pairs = 2;
    s.n_wl_det = 2;
    s.n_band_det = 4;
    s.z_fixed = 4;
    s.z_total = 4;
    s.ratio = 12.3456;
    s.p_s = 0.9999;
    return s;
}

TEST(ArObserveTraceTest, FixHeaderIsTheFrozenGradersHeader) {
    // The consumer grader (scripts/consumer_ar_grade.py, FIX_HEADER) refuses a file whose
    // columns differ; this reads its tuple, so the writer and the grader cannot drift apart.
    const std::string path = std::string(GNSS_PROJECT_ROOT) + "/scripts/consumer_ar_grade.py";
    std::ifstream in(path);
    if (!in) GTEST_SKIP() << "scripts/consumer_ar_grade.py is not in this tree";
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const auto start = text.find("FIX_HEADER = (");
    ASSERT_NE(start, std::string::npos);
    const auto end = text.find(')', start);
    ASSERT_NE(end, std::string::npos);
    const std::string block = text.substr(start, end - start);
    std::vector<std::string> grader;
    const std::regex quoted("\"([a-z_]+)\"");          // a named regex: the iterator may not bind a temporary
    for (std::sregex_iterator it(block.begin(), block.end(), quoted), last; it != last; ++it)
        grader.push_back((*it)[1].str());
    ASSERT_EQ(grader.size(), 18u);
    EXPECT_EQ(split(kArFixTraceHeader), grader);
}

TEST(ArObserveTraceTest, EpochHeaderNamesTheCountsAndSixErrors) {
    const std::vector<std::string> expected = {
        "week", "tow", "n_pairs", "n_wl_det", "n_band_det",
        "fix_e", "fix_n", "fix_u", "float_e", "float_n", "float_u" };
    EXPECT_EQ(split(kArEpochTraceHeader), expected);
    const std::string h = kArEpochTraceHeader;
    EXPECT_EQ(h.back(), '\n');
}

TEST(ArObserveTraceTest, FixRowWritesEveryColumnOfTheHeader) {
    const std::string line = formatArFixRow(2313, 261030.0, sampleRow(), sampleSummary());
    EXPECT_EQ(line, "2313,261030.000,G,L1C,G05,G01,1,-39.987654,0.012346,-40,1,1,4,4,12.346,"
                    "0.999900,7,9\n");
    EXPECT_EQ(split(line).size(), split(kArFixTraceHeader).size());
}

TEST(ArObserveTraceTest, FixRowFlagsAreZeroAndOneAndAbsentTokensAreZero) {
    ArFixRow r = sampleRow();
    r.determined = false;
    r.kept = false;
    r.cmp = 0;
    r.gen_sat = 0;
    r.gen_ref = 0;
    r.signal = "WL";
    const std::vector<std::string> f = split(formatArFixRow(2313, 261060.5, r, sampleSummary()));
    ASSERT_EQ(f.size(), 18u);
    EXPECT_EQ(f[1], "261060.500");
    EXPECT_EQ(f[3], "WL");
    EXPECT_EQ(f[6], "0");     // cmp
    EXPECT_EQ(f[10], "0");    // determined
    EXPECT_EQ(f[11], "0");    // kept
    EXPECT_EQ(f[16], "0");    // gen_sat
    EXPECT_EQ(f[17], "0");    // gen_ref
}

TEST(ArObserveTraceTest, EpochRowCarriesTheCountsAndTheEnuErrors) {
    ArEpochErrors e;
    e.fix_enu = Eigen::Vector3d(0.0123, -0.0456, 0.0789);
    e.float_enu = Eigen::Vector3d(0.1, -0.2, 0.3);
    EXPECT_EQ(formatArEpochRow(2313, 261030.0, sampleSummary(), e),
              "2313,261030.000,2,2,4,0.0123,-0.0456,0.0789,0.1000,-0.2000,0.3000\n");
}

TEST(ArObserveTraceTest, EnuErrorsAreAgainstTheTruthInItsLocalFrame) {
    // Truth on the equator at the prime meridian: up is +x, east is +y, north is +z.
    const Eigen::Vector3d truth(6378137.0, 0.0, 0.0);
    const Coordinates::Llh llh = Coordinates::ecefToLlh(truth);
    ArEpochSummary s;
    s.fix_ecef = truth + Eigen::Vector3d(0.01, 0.02, 0.03);      // up 0.01, east 0.02, north 0.03
    s.float_ecef = truth + Eigen::Vector3d(-0.1, 0.4, -0.2);     // up -0.1, east 0.4, north -0.2
    const ArEpochErrors e = arEpochErrors(s, truth, llh);
    EXPECT_NEAR(e.fix_enu.x(), 0.02, 1e-9);
    EXPECT_NEAR(e.fix_enu.y(), 0.03, 1e-9);
    EXPECT_NEAR(e.fix_enu.z(), 0.01, 1e-9);
    EXPECT_NEAR(e.float_enu.x(), 0.4, 1e-9);
    EXPECT_NEAR(e.float_enu.y(), -0.2, 1e-9);
    EXPECT_NEAR(e.float_enu.z(), -0.1, 1e-9);
}

TEST(ArObserveTraceTest, TheNetworkRunsWriterWritesOneEpochRowAndOneRowPerSearchRow) {
    const std::filesystem::path dir = gnss_test::tempDir() / "ar_observe_writer";
    std::filesystem::create_directories(dir);
    const GnssTime t{ 2313, 261030.0 };
    ArEpochErrors err;
    err.fix_enu = Eigen::Vector3d(0.01, 0.02, 0.03);
    err.float_enu = Eigen::Vector3d(0.1, 0.2, 0.3);

    ArObserveEpoch epoch;
    epoch.valid = true;
    epoch.summary = sampleSummary();
    epoch.rows = { sampleRow(), sampleRow() };
    epoch.rows[1].signal = "WL";

    {
        NetworkRunTraces traces(dir);
        // The streams are closed (the search or its trace is off): nothing is written.
        EXPECT_EQ(traces.writeConsumerArObserve(t, epoch, err), 0u);

        traces.cl_ar_fix_trace.open(dir / "closed_loop_ar_fixes.csv");
        traces.cl_ar_epoch_trace.open(dir / "closed_loop_ar_epoch.csv");
        ASSERT_TRUE(traces.cl_ar_fix_trace.is_open());
        ASSERT_TRUE(traces.cl_ar_epoch_trace.is_open());
        traces.cl_ar_fix_trace << kArFixTraceHeader;
        traces.cl_ar_epoch_trace << kArEpochTraceHeader;
        EXPECT_EQ(traces.writeConsumerArObserve(t, epoch, err), 2u);

        // An epoch whose search stage did not run writes nothing, not even an epoch row.
        ArObserveEpoch skipped;
        EXPECT_EQ(traces.writeConsumerArObserve(GnssTime{ 2313, 261060.0 }, skipped, err), 0u);

        // A gated epoch has no rows but still writes its epoch row.
        ArObserveEpoch gated;
        gated.valid = true;
        gated.summary.gated = true;
        EXPECT_EQ(traces.writeConsumerArObserve(GnssTime{ 2313, 261090.0 }, gated, err), 0u);
        traces.closeAll();
    }
    auto lines = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::vector<std::string> out;
        std::string line;
        while (std::getline(in, line)) out.push_back(line);
        return out;
    };
    const auto fixes = lines(dir / "closed_loop_ar_fixes.csv");
    ASSERT_EQ(fixes.size(), 3u);                        // header and two rows
    EXPECT_EQ(fixes[1], "2313,261030.000,G,L1C,G05,G01,1,-39.987654,0.012346,-40,1,1,4,4,12.346,0.999900,7,9");
    EXPECT_EQ(split(fixes[2])[3], "WL");
    const auto epochs = lines(dir / "closed_loop_ar_epoch.csv");
    ASSERT_EQ(epochs.size(), 3u);                       // header, the searched epoch, the gated epoch
    EXPECT_EQ(epochs[1], "2313,261030.000,2,2,4,0.0100,0.0200,0.0300,0.1000,0.2000,0.3000");
    EXPECT_EQ(split(epochs[2])[1], "261090.000");
}

TEST(ArObserveTraceTest, RunEndLineNamesTheTotalsTheSearchAndTheGate) {
    ArObserveCounters c;
    c.epochs = 100;
    c.epochs_searched = 90;
    c.epochs_accepted = 40;
    c.candidates = 1000;
    c.eligible = 800;
    c.gated_out = 200;
    c.sd_pairs = 700;
    c.band_rows = 1400;
    c.wl_rows = 700;
    c.determined = 900;
    c.kept = 800;
    c.band_kept = 500;
    c.wl_kept = 300;
    const std::string line = formatArObserveSummary(c, "end of replay", "decorrelated_partial", 0.05, true);
    EXPECT_NE(line.find("AR observe (end of replay, search 'decorrelated_partial'"), std::string::npos);
    EXPECT_NE(line.find("chain gate on"), std::string::npos);
    EXPECT_NE(line.find("epochs 100 / searched 90 / accepted 40"), std::string::npos);
    EXPECT_NE(line.find("eligible 800 / gated out 200"), std::string::npos);
    EXPECT_NE(line.find("SD pairs 700, band rows 1400 + wide-lane rows 700"), std::string::npos);
    EXPECT_NE(line.find("determined 900, kept 800 (band 500 / wide lane 300)"), std::string::npos);
    EXPECT_NE(line.find("closure HOLDS"), std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos);
    c.eligible = 700;                                            // a candidate neither admitted nor refused
    EXPECT_NE(formatArObserveSummary(c, "closed loop", "full", 0.05, false).find("closure BROKEN"),
              std::string::npos);
}

} // namespace
