// Trace-stream LIFECYCLE tests for NetworkRunTraces (include/logging/NetworkRunTraces.hpp).
//
// The property under test is not what a stream writes but how it is OPENED.  A
// network trace must be truncated once at run start and written through a single
// owned handle for the rest of the run, so that re-running a config into an
// already-used log directory REPLACES the artifact.  The two MW/WL diagnostic
// dumps did not obey that: networkInstallCarry re-opened them per epoch in
// std::ios::app and nothing ever truncated them, so consecutive runs into one
// directory silently concatenated — and an A/B pair whose two arms had been
// re-run unequal numbers of times showed a pure-artifact row-count divergence
// that reads exactly like a real D5 divergence (hit during FC-1 INC-1 validation,
// 2026-07-30).  That is a false-positive generator for the whole byte-identity /
// A-B methodology, since network.mw_wl.d5_dump is an ordinary diagnostic arm.
//
//   Gate 1 (truncate)  — a second openMwWlDumps over the same directory REPLACES
//                        both files: one header, only the second run's rows.
//   Gate 2 (arming)    — an unarmed dump opens NOTHING and writes NOTHING (the
//                        default-OFF byte-identical contract); a file left over
//                        from an earlier armed run is NOT touched by an OFF run,
//                        which is what makes an absent file honest evidence that
//                        the tier did not run.
//   Gate 3 (headers)   — the emitted headers are the exact column lists the
//                        offline graders parse (scripts/grade_slip_agreement.py
//                        --d5, scripts/grade_lane_trace.py).  Byte-pinned here so
//                        a column edit cannot land without updating the readers.

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

// The trailing zhat..dw_w block is the churn card's algebra dump (2026-08-02): it takes
// std_innov apart into the disagreement (v) and the formal claim about it (var_v), which
// is the only way to tell an over-optimistic Cov(v) from a genuinely large innovation.
// warmup_suppressed (RS-2b, SPEC_event_harness §21.6) is APPENDED after it: 1 where the
// trip fell inside the post-resume D5 warm-up and was counted without being acted on, so
// a grader can separate "the gate saw N" from "the gate did N" without the run log.
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
/// state into the next one (the very defect being tested is cross-run leakage).
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

// Gate 1: the defect itself.  Two runs into the same directory must leave the
// second run's file, not the concatenation of both.
TEST_F(NetworkRunTracesTest, ReopeningTheSameLogDirTruncatesTheMwWlDumps) {
    const auto log = nullLogger();
    {   // "run 1" — three D5 rows and two lane rows
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(true, true, log);
        for (int i = 0; i < 3; ++i) t.d5_flags_trace << "run1_d5_" << i << '\n';
        for (int i = 0; i < 2; ++i) t.wl_lane_trace << "run1_lane_" << i << '\n';
        t.closeAll();
    }
    {   // "run 2" — same directory, ONE row each
        NetworkRunTraces t(dir_);
        t.openMwWlDumps(true, true, log);
        t.d5_flags_trace << "run2_d5_0\n";
        t.wl_lane_trace << "run2_lane_0\n";
        t.closeAll();
    }

    const auto d5   = readLines(dir_ / "network_d5_flags.csv");
    const auto lane = readLines(dir_ / "network_wl_lane_trace.csv");

    // Exactly one header + exactly run 2's rows.  Under the append defect these
    // were 1+3+1 (with a repeated header) and 1+2+1.
    ASSERT_EQ(d5.size(), 2u) << "D5 dump was appended to, not truncated";
    EXPECT_EQ(d5[0], kD5Header);
    EXPECT_EQ(d5[1], "run2_d5_0");

    ASSERT_EQ(lane.size(), 2u) << "lane trace was appended to, not truncated";
    EXPECT_EQ(lane[0], kLaneHeader);
    EXPECT_EQ(lane[1], "run2_lane_0");

    // No line other than the first may be a header — a header MID-file is the
    // signature of the per-epoch re-open, and it is what made the graders carry
    // run-segmentation logic.
    for (std::size_t i = 1; i < d5.size(); ++i)
        EXPECT_NE(d5[i].rfind("week,", 0), 0u) << "header repeated at D5 line " << i;
    for (std::size_t i = 1; i < lane.size(); ++i)
        EXPECT_NE(lane[i].rfind("week,", 0), 0u) << "header repeated at lane line " << i;
}

// Gate 2: OFF opens nothing, and OFF does not disturb an existing artifact.
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

    // Arming ONE dump must not open the other — the two toggles are independent
    // (d5_dump rides the innovation gate, lane_trace rides the WL lane), and a
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
        t.closeAll();          // armed, zero flags — the honest "ran, found none"
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
