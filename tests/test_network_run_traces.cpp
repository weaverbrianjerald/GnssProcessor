// Trace-stream lifecycle tests for NetworkRunTraces (include/logging/NetworkRunTraces.hpp).
//
// The property under test is how a stream is opened, not what it writes.  A network
// trace is truncated once at run start and written through a single owned handle for
// the rest of the run, so re-running a config into an already-used log directory
// replaces the artifact.  The two MW/WL diagnostic dumps are the case covered here:
// re-opened per epoch in std::ios::app with no truncation, consecutive runs into one
// directory would concatenate, and an A/B pair whose arms had been re-run a different
// number of times would show a row-count divergence that looks like a real D5
// divergence (FC-1 INC-1).  Since network.mw_wl.d5_dump is an ordinary diagnostic
// arm, that would produce false positives in byte-identity and A/B comparisons.
//
//   Gate 1 (truncate):  a second openMwWlDumps over the same directory replaces
//                       both files: one header, only the second run's rows.
//   Gate 2 (arming):    an unarmed dump opens and writes nothing (the default-off
//                       byte-identical contract), and an off run leaves a file from
//                       an earlier armed run untouched, so an absent file shows that
//                       the tier did not run.
//   Gate 3 (headers):   the emitted headers are the exact column lists the offline
//                       graders parse (scripts/grade_slip_agreement.py --d5,
//                       scripts/grade_lane_trace.py).  They are pinned byte for byte
//                       here, so a column edit fails this test until the readers
//                       and the expected header are updated together.

#include "logging/NetworkRunTraces.hpp"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// The trailing zhat..dw_w block is the churn card's algebra dump: it splits std_innov
// into the disagreement (v) and its formal variance (var_v), which separates an
// over-optimistic Cov(v) from a large innovation.  warmup_suppressed (RS-2b,
// SPEC_event_harness §21.6) is appended after it: 1 where the trip fell inside the
// post-resume D5 warm-up and was counted without being acted on, so a grader can tell
// trips seen from trips acted on without the run log.
const char* const kD5Header =
    "week,tow,group,chord_u,chord_v,station,amb_key,std_innov,"
    "ewl_stasat,hold_chord,injected_prev,raw_stepped,"
    "zhat,v,var_v,sigma0,z_post,dw_w,warmup_suppressed";
const char* const kLaneHeader =
    "week,tow,chord_u,chord_v,station,sat,lane_cyc,frac_cyc,w,decided,held,injected";

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_network_run_traces", std::make_shared<spdlog::sinks::null_sink_mt>());
}

std::vector<std::string> readLines(const std::filesystem::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

/// A scratch log directory that cleans itself up, so a failing test cannot leak
/// state into the next one (the property under test is the absence of cross-run leakage).
class NetworkRunTracesTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path()
             / ("gnss_run_traces_test_"
                + std::to_string(
                      ::testing::UnitTest::GetInstance()->random_seed())
                + "_"
                + ::testing::UnitTest::GetInstance()
                      ->current_test_info()->name());
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir_;
};

// Gate 1: two runs into the same directory leave the second run's file, not the
// concatenation of both.
TEST_F(NetworkRunTracesTest, ReopeningTheSameLogDirTruncatesTheMwWlDumps) {
    const auto log = nullLogger();
    {   // "run 1": three D5 rows and two lane rows
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(true, true, log);
        for (int i = 0; i < 3; ++i) t.d5_flags_trace << "run1_d5_" << i << '\n';
        for (int i = 0; i < 2; ++i) t.wl_lane_trace << "run1_lane_" << i << '\n';
        t.closeAll();
    }
    {   // "run 2": same directory, one row each
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(true, true, log);
        t.d5_flags_trace << "run2_d5_0\n";
        t.wl_lane_trace << "run2_lane_0\n";
        t.closeAll();
    }

    const auto d5   = readLines(dir_ / "network_d5_flags.csv");
    const auto lane = readLines(dir_ / "network_wl_lane_trace.csv");

    // One header plus run 2's rows only.  Append-mode opening would give 1+3+1
    // lines (with a repeated header) and 1+2+1.
    ASSERT_EQ(d5.size(), 2u) << "D5 dump was appended to, not truncated";
    EXPECT_EQ(d5[0], kD5Header);
    EXPECT_EQ(d5[1], "run2_d5_0");

    ASSERT_EQ(lane.size(), 2u) << "lane trace was appended to, not truncated";
    EXPECT_EQ(lane[0], kLaneHeader);
    EXPECT_EQ(lane[1], "run2_lane_0");

    // Only the first line is a header.  A header mid-file indicates a per-epoch
    // re-open, which forces the graders to carry run-segmentation logic.
    for (std::size_t i = 1; i < d5.size(); ++i)
        EXPECT_NE(d5[i].rfind("week,", 0), 0u) << "header repeated at D5 line " << i;
    for (std::size_t i = 1; i < lane.size(); ++i)
        EXPECT_NE(lane[i].rfind("week,", 0), 0u) << "header repeated at lane line " << i;
}

// Gate 2: an off run opens nothing and does not disturb an existing artifact.
TEST_F(NetworkRunTracesTest, UnarmedMwWlDumpsOpenNothing) {
    const auto log = nullLogger();
    {
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(false, false, log);
        EXPECT_FALSE(t.d5_flags_trace.is_open());
        EXPECT_FALSE(t.wl_lane_trace.is_open());
        t.closeAll();
    }
    EXPECT_FALSE(std::filesystem::exists(dir_ / "network_d5_flags.csv"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "network_wl_lane_trace.csv"));

    // Arming one dump does not open the other.  The two toggles are independent
    // (d5_dump belongs to the innovation gate, lane_trace to the WL lane), and a
    // spurious empty file would misreport which tier ran.
    {
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(true, false, log);
        EXPECT_TRUE(t.d5_flags_trace.is_open());
        EXPECT_FALSE(t.wl_lane_trace.is_open());
        t.closeAll();
    }
    EXPECT_TRUE(std::filesystem::exists(dir_ / "network_d5_flags.csv"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "network_wl_lane_trace.csv"));

    // A subsequent OFF run leaves the armed run's file exactly as it was, rather
    // than truncating it to a bare header.
    const auto before = readLines(dir_ / "network_d5_flags.csv");
    {
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(false, false, log);
        t.closeAll();
    }
    EXPECT_EQ(readLines(dir_ / "network_d5_flags.csv"), before);
}

// Gate 3: an armed-but-silent run yields a header-only file, and the header is
// byte-exactly what the offline graders expect.
TEST_F(NetworkRunTracesTest, ArmedDumpsWriteTheGraderHeadersEvenWithNoRows) {
    const auto log = nullLogger();
    {
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(true, true, log);
        t.closeAll();          // armed, zero flags: "ran, found none"
    }
    const auto d5   = readLines(dir_ / "network_d5_flags.csv");
    const auto lane = readLines(dir_ / "network_wl_lane_trace.csv");
    ASSERT_EQ(d5.size(), 1u);
    ASSERT_EQ(lane.size(), 1u);
    EXPECT_EQ(d5[0], kD5Header);
    EXPECT_EQ(lane[0], kLaneHeader);
}

}  // namespace
}  // namespace GnssProcessor
