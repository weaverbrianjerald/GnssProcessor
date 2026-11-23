/**
 * @file test_rt_state_snapshot_unrecorded.cpp
 * @brief Unit gates for the @c U record of the RT filter-state snapshot (card USC,
 *        docs/cards/user-ar/USC-USER-SATELLITE-WITHOUT-RECORD.md): the dropped set of the rule for
 *        satellites without their stream records, with the drop times, and the satellites seen
 *        with their records round-trip; a file without the rule's content keeps its bytes; a
 *        malformed or duplicated @c U line fails the strict parse.
 */

#include <gtest/gtest.h>

#include "realtime/RtStateSnapshot.hpp"

#include <cmath>
#include <sstream>
#include <string>

using namespace GnssProcessor;

namespace {

/// A minimal valid snapshot: two core states, no dynamic state.
RtStateSnapshot makeSnap() {
    RtStateSnapshot s;
    s.snap_week   = 2313;
    s.snap_tow    = 268350.0;
    s.wall_utc    = "2026-11-22T00:00:00Z";
    s.fingerprint = 0x30d7d945a4f6f13fULL;
    s.core_size   = 2;
    s.state_size  = 2;
    s.generation_id = 1u;
    s.epoch_count   = 305;
    s.x = Eigen::VectorXd::Zero(2);
    s.P = Eigen::MatrixXd::Identity(2, 2) * 0.04;
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

}  // namespace

TEST(RtStateSnapshotUnrecorded, URowsRoundTrip) {
    RtStateSnapshot s = makeSnap();
    s.unrec_dropped  = { { "E25", 1399170720.0 }, { "G12", std::nan("") } };
    s.unrec_recorded = { "E25", "G01", "G12" };
    const std::string text = serialized(s);
    EXPECT_NE(text.find("U,E25,1,1399170720\n"), std::string::npos);
    EXPECT_NE(text.find("U,G12,1,nan\n"), std::string::npos);
    EXPECT_NE(text.find("U,G01,0\n"), std::string::npos);
    RtStateSnapshot back;
    std::string err;
    ASSERT_TRUE(parses(text, back, &err)) << err;
    ASSERT_EQ(back.unrec_dropped.size(), 2u);
    EXPECT_DOUBLE_EQ(back.unrec_dropped.at("E25"), 1399170720.0);
    EXPECT_TRUE(std::isnan(back.unrec_dropped.at("G12")));
    EXPECT_EQ(back.unrec_recorded, s.unrec_recorded);
}

TEST(RtStateSnapshotUnrecorded, FileWithoutTheRuleKeepsItsBytes) {
    const RtStateSnapshot s = makeSnap();
    const std::string text = serialized(s);
    EXPECT_EQ(text.find("\nU,"), std::string::npos);
    RtStateSnapshot back;
    ASSERT_TRUE(parses(text, back));
    EXPECT_TRUE(back.unrec_dropped.empty());
    EXPECT_TRUE(back.unrec_recorded.empty());
    EXPECT_EQ(serialized(back).substr(serialized(back).find('\n')),
              text.substr(text.find('\n')));
}

TEST(RtStateSnapshotUnrecorded, MalformedOrDuplicatedURowsAreRejected) {
    RtStateSnapshot s = makeSnap();
    s.unrec_recorded = { "G01" };
    const std::string good = serialized(s);
    RtStateSnapshot back;
    std::string bad = good;
    bad.replace(bad.find("U,G01,0"), 7, "U,G01,2");
    EXPECT_FALSE(parses(bad, back));
    bad = good;
    bad.replace(bad.find("U,G01,0"), 7, "U,G01,1,x");
    EXPECT_FALSE(parses(bad, back));
    // A duplicated row (the END count adjusted so only the duplicate can fail).
    s.unrec_recorded = { "G01", "G02" };
    std::string dup = serialized(s);
    dup.replace(dup.find("U,G02,0"), 7, "U,G01,0");
    EXPECT_FALSE(parses(dup, back));
}
