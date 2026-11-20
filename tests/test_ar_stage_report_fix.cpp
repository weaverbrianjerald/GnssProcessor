/**
 * @file test_ar_stage_report_fix.cpp
 * @brief Unit tests of stage S9 of the WL-constrained user resolver,
 *        @c AmbiguityResolver::reportFix_: the summary and QBCOND lines of a full fix, a partial
 *        fix and a refused partial search.
 *
 * The lines are read by the grading scripts (QBCOND, the summary's ratio and P_s), so their
 * text is pinned exactly, numbers included:
 *   - a full fix writes the "WL-constrained AR succeeded" line and QBCOND path=full;
 *   - a partial fix writes the "Partial AR (WL/NL) succeeded" line with kept/total and
 *     QBCOND path=partial;
 *   - a refused partial search writes the "no fixable subset" line and QBCOND path=reject
 *     with literal zeros for the after-fix values;
 *   - the two ratios of the QBCOND line are 0 when the before-fix value is 0.
 *
 * Suite name @c ArStageReportFix is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "support/ArWlcStageAccess.hpp"

#include <string>

using namespace GnssProcessor;
using gnss_test::ArStageRig;
using Access = ArWlcStageTestAccess;

namespace {

AmbiguityResolverConfig cfg() {
    AmbiguityResolverConfig c;
    c.hold_variance = 1e-7;
    return c;
}

QbCond cond(double c, double lo, double hi) {
    QbCond q;
    q.cond = c;
    q.sig_lo = lo;
    q.sig_hi = hi;
    return q;
}

/// The message part of each captured line (the text after the level tag), joined by newlines.
std::string messages(const std::string& text) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // spdlog ends lines with CR LF on Windows
        const std::size_t tag = line.find("] ", line.find("[info]"));
        out += (tag == std::string::npos ? line : line.substr(tag + 2)) + "\n";
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return out;
}

TEST(ArStageReportFix, AFullFixWritesTheSummaryAndTheFullConditioningLine) {
    ArStageRig rig(cfg());
    Access::report(*rig.resolver, Access::Path::Full, 7, 7, 12.3456, 0.999876, 0.0421,
                   cond(1.5e3, 0.04, 0.6), cond(3.0e3, 0.02, 0.6));
    EXPECT_EQ(messages(rig.logText()),
              "WL-constrained AR succeeded (Kalman pseudo-obs, R=0.0000 m\xC2\xB2): 7 NL pairs fixed, "
              "ratio=12.35, P_s=0.99988, max|frac|=0.042 cyc\n"
              "QBCOND path=full n_nl=7 nfix=7 R=1.000e-07 pre_cond=1.5000e+03 pre_sig_lo=4.0000e-02 "
              "pre_sig_hi=6.0000e-01 post_cond=3.0000e+03 post_sig_lo=2.0000e-02 post_sig_hi=6.0000e-01 "
              "cond_ratio=2.0000e+00 siglo_ratio=5.0000e-01 ratio=12.35\n");
}

TEST(ArStageReportFix, APartialFixWritesKeptOverTotalAndThePartialConditioningLine) {
    ArStageRig rig(cfg());
    Access::report(*rig.resolver, Access::Path::Partial, 9, 4, 6.5, 0.9995, 0.1,
                   cond(10.0, 0.1, 1.0), cond(20.0, 0.05, 1.0));
    EXPECT_EQ(messages(rig.logText()),
              "Partial AR (WL/NL) succeeded (Kalman pseudo-obs, R=0.0000 m\xC2\xB2): 4/9 NL pairs fixed, "
              "ratio=6.50, P_s=0.99950, max|frac|=0.100 cyc\n"
              "QBCOND path=partial n_nl=9 nfix=4 R=1.000e-07 pre_cond=1.0000e+01 pre_sig_lo=1.0000e-01 "
              "pre_sig_hi=1.0000e+00 post_cond=2.0000e+01 post_sig_lo=5.0000e-02 post_sig_hi=1.0000e+00 "
              "cond_ratio=2.0000e+00 siglo_ratio=5.0000e-01 ratio=6.50\n");
}

TEST(ArStageReportFix, ARefusedPartialSearchWritesLiteralZerosAfterTheFix) {
    ArStageRig rig(cfg());
    Access::report(*rig.resolver, Access::Path::Reject, 5, 0, 2.25, 0.0, 0.0,
                   cond(10.0, 0.1, 1.0), QbCond{});
    EXPECT_EQ(messages(rig.logText()),
              "Partial AR (WL/NL): no fixable subset found (5 NL candidates); staying float\n"
              "QBCOND path=reject n_nl=5 nfix=0 R=1.000e-07 pre_cond=1.0000e+01 pre_sig_lo=1.0000e-01 "
              "pre_sig_hi=1.0000e+00 post_cond=0 post_sig_lo=0 post_sig_hi=0 cond_ratio=0 siglo_ratio=0 "
              "ratio=2.25\n");
}

TEST(ArStageReportFix, TheConditioningRatiosAreZeroWhenTheValueBeforeTheFixIsZero) {
    ArStageRig rig(cfg());
    Access::report(*rig.resolver, Access::Path::Full, 1, 1, 8.0, 1.0, 0.0, QbCond{}, cond(2.0, 0.5, 1.0));
    const std::string m = messages(rig.logText());
    EXPECT_NE(m.find("cond_ratio=0.0000e+00 siglo_ratio=0.0000e+00"), std::string::npos) << m;
}

} // namespace
