/**
 * @file test_consumer_trace.cpp
 * @brief Pins the schema of `logs/closed_loop_trace.csv` and the formal-sigma
 *        rotation written into it.
 *
 * The header, the row formatter and the two helpers are pure functions, so the
 * column set every grader depends on can be tested without an engine or a file:
 * the eighteen columns in order (the first fourteen unchanged, the four sigma
 * columns appended), a data line with as many fields as the header, the bytes of
 * the first fourteen fields as both writers produced them before the sigma columns
 * existed, the AR gate labels, and the rotation of an ECEF covariance into the
 * local frame in which the error is written.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include "realtime/ConsumerTrace.hpp"

#include "filter/AmbiguityResolver.hpp"
#include "math/Coordinates.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

/// Split a CSV line on ',' and keep empty fields.
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\n') { cur.push_back(c); }
    }
    out.push_back(cur);
    return out;
}

/// A row with every field set to a value whose formatting is checkable.
ConsumerTraceRow sampleRow() {
    ConsumerTraceRow r;
    r.week = 2313;
    r.tow = 259230.0;
    r.n_sats = 20;
    r.err_3d_m = 0.70914;
    r.err_enu_m = Eigen::Vector3d(-0.19384, 0.56451, 0.38291);
    r.sig_enu_m = Eigen::Vector3d(0.12346, 0.23456, 0.54321);
    r.sig_3d_m = 0.6;
    return r;
}

} // namespace

TEST(ConsumerTraceSchema, HeaderHasEighteenColumnsWithSigmaAppended) {
    const std::vector<std::string> cols = splitCsv(kConsumerTraceHeader);
    const std::vector<std::string> want = {
        "week", "tow", "n_sats", "ar_ran", "ar_success", "num_fixed", "num_candidates",
        "ratio", "pos_err_3d_m", "east_m", "north_m", "up_m", "ar_gate", "p_succ",
        "sig_e_m", "sig_n_m", "sig_u_m", "sig_3d_m"};
    EXPECT_EQ(cols, want);
    EXPECT_EQ(cols.size(), kConsumerTraceColumns);
    const std::string h = kConsumerTraceHeader;
    ASSERT_FALSE(h.empty());
    EXPECT_EQ(h.back(), '\n');
}

TEST(ConsumerTraceSchema, FirstFourteenColumnsKeepTheirFormerBytes) {
    // The line both writers produced for this epoch before the sigma columns: the
    // first row of a control census replay (AR not run).
    AmbiguityResolution ar;
    ConsumerTraceRow r = sampleRow();
    fillConsumerTraceAr(r, /*ar_ran=*/false, ar);
    const std::string line = formatConsumerTraceRow(r);
    const std::string legacy =
        "2313,259230.000,20,0,0,0,0,0.00,0.7091,-0.1938,0.5645,0.3829,not_run,0.000000";
    ASSERT_GE(line.size(), legacy.size());
    EXPECT_EQ(line.substr(0, legacy.size()), legacy);
    EXPECT_EQ(line, legacy + ",0.1235,0.2346,0.5432,0.6000\n");
    EXPECT_EQ(splitCsv(line).size(), kConsumerTraceColumns);
}

TEST(ConsumerTraceSchema, ArColumnsFollowTheEngineResult) {
    AmbiguityResolution ar;
    ar.success = true;
    ar.num_fixed = 14;
    ar.num_candidates = 17;
    ar.ratio = 4.257;
    ar.p_succ = 0.9991234;
    ConsumerTraceRow r = sampleRow();
    fillConsumerTraceAr(r, /*ar_ran=*/true, ar);
    const std::vector<std::string> f = splitCsv(formatConsumerTraceRow(r));
    ASSERT_EQ(f.size(), kConsumerTraceColumns);
    EXPECT_EQ(f[3], "1");
    EXPECT_EQ(f[4], "1");
    EXPECT_EQ(f[5], "14");
    EXPECT_EQ(f[6], "17");
    EXPECT_EQ(f[7], "4.26");
    EXPECT_EQ(f[12], "fixed");
    EXPECT_EQ(f[13], "0.999123");

    // The WL-only path's ratio sentinel is written as it always was.
    ar.wl_only_fix = true;
    ar.ratio = std::numeric_limits<double>::infinity();
    fillConsumerTraceAr(r, true, ar);
    const std::vector<std::string> g = splitCsv(formatConsumerTraceRow(r));
    EXPECT_EQ(g[7], "inf");
    EXPECT_EQ(g[12], "wl_only");
}

TEST(ConsumerTraceSchema, GateLabelsByReason) {
    AmbiguityResolution ar;
    EXPECT_EQ(consumerArGateLabel(false, ar), "not_run");
    ar.gate_reason = 1; EXPECT_EQ(consumerArGateLabel(true, ar), "disabled");
    ar.gate_reason = 2; EXPECT_EQ(consumerArGateLabel(true, ar), "min_epochs");
    ar.gate_reason = 3; ar.gate_sigma_3d = 0.1234;
    EXPECT_EQ(consumerArGateLabel(true, ar), "sigma_gate(0.123m)");
    ar.gate_reason = 4; EXPECT_EQ(consumerArGateLabel(true, ar), "no_candidates");
    ar.gate_reason = 5; EXPECT_EQ(consumerArGateLabel(true, ar), "wl_guard");
    ar.gate_reason = 0; ar.success = false; ar.ratio = 1.7;
    EXPECT_EQ(consumerArGateLabel(true, ar), "ratio_battery");
    ar.ratio = 0.0;
    EXPECT_EQ(consumerArGateLabel(true, ar), "mlambda_fail");
}

TEST(ConsumerTraceSchema, SigmaRotatesIntoTheErrorFrame) {
    // A covariance built in the local frame at a mid-latitude site and rotated to
    // ECEF must come back as the same east, north and up sigmas.
    Coordinates::Llh ref;
    ref.lat_rad = 49.144 * kDegToRad;
    ref.lon_rad = 12.879 * kDegToRad;
    ref.h_m = 666.0;
    const Eigen::Matrix3d r = Coordinates::enuToEcefMatrix(ref.lat_rad, ref.lon_rad);
    const Eigen::Vector3d sig(0.02, 0.03, 0.07);
    const Eigen::Matrix3d p_enu = sig.cwiseAbs2().asDiagonal();
    const Eigen::Matrix3d p_ecef = r * p_enu * r.transpose();
    const Eigen::Vector3d got = enuSigmaFromEcefCovariance(p_ecef, ref);
    EXPECT_NEAR(got.x(), 0.02, 1e-12);
    EXPECT_NEAR(got.y(), 0.03, 1e-12);
    EXPECT_NEAR(got.z(), 0.07, 1e-12);
    // The 3D sigma is the root of the trace, the same in either frame.
    EXPECT_NEAR(sigma3dFromCovariance(p_ecef), std::sqrt(0.02 * 0.02 + 0.03 * 0.03 + 0.07 * 0.07),
                1e-12);

    // The same frame as the error: a displacement of one formal sigma along each
    // local axis, rotated to ECEF, reads back through ecefToEnu on that axis.
    const Eigen::Vector3d d_ecef = r * Eigen::Vector3d(0.0, 0.0, got.z());
    const Eigen::Vector3d d_enu = Coordinates::ecefToEnu(d_ecef, ref);
    EXPECT_NEAR(d_enu.z(), 0.07, 1e-12);
    EXPECT_NEAR(d_enu.x(), 0.0, 1e-12);
}

TEST(ConsumerTraceSchema, NegativeVarianceWritesZeroNotNan) {
    Coordinates::Llh ref;
    Eigen::Matrix3d p = Eigen::Matrix3d::Zero();
    p(0, 0) = -1e-9;
    const Eigen::Vector3d got = enuSigmaFromEcefCovariance(p, ref);
    EXPECT_TRUE(std::isfinite(got.x()));
    EXPECT_TRUE(std::isfinite(got.y()));
    EXPECT_TRUE(std::isfinite(got.z()));
    EXPECT_EQ(sigma3dFromCovariance(-Eigen::Matrix3d::Identity()), 0.0);
}
