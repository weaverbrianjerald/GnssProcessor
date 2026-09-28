/**
 * @file test_rx_cb_carry.cpp
 * @brief Unit gates for the receiver code-bias carry's inject-time decision
 *        (@ref rxCbInjectGate, docs/SPEC_rx_cb_carry.md).
 *
 * The gate is the read-only half of the carry's event response: the produce
 * phase is parallel, so a breach is answered by withholding the fold, and the
 * serial harvest then overwrites the stale entry (an implicit drop).  These
 * tests cover the decision table: gap staleness, RW variance inflation, the
 * k·sigma innovation gate with its variance guard, and NaN/degenerate inputs.
 */

#include <gtest/gtest.h>

#include "filter/NetworkRunTypes.hpp"

#include <cmath>
#include <limits>

using namespace GnssProcessor;

namespace {

// Nominal fold: fresh carry, small innovation; the variance inflates by q·gap.
TEST(RxCbInjectGate, FoldsWithInflatedVariance) {
    double var_eff = 0.0;
    const auto act = rxCbInjectGate(/*gap=*/30.0, /*max_gap=*/900.0,
                                    /*var=*/1e-4, /*q=*/1e-9, /*innov=*/0.01,
                                    /*k=*/10.0, /*guard=*/0.25, var_eff);
    EXPECT_EQ(act, RxCbInjectAction::Fold);
    EXPECT_NEAR(var_eff, 1e-4 + 1e-9 * 30.0, 1e-18);
}

// A stale entry (gap beyond the housekeeping horizon) is skipped, not folded.
TEST(RxCbInjectGate, SkipsStaleGap) {
    double var_eff = -1.0;
    EXPECT_EQ(rxCbInjectGate(901.0, 900.0, 1e-4, 1e-9, 0.0, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipGap);
}

// The innovation gate: |innov| just under k·sqrt(var_eff+guard) folds, just
// over withholds.  With var_eff ~ 1e-4 and guard 0.25 the threshold is
// dominated by the guard: 10*sqrt(0.2501) ~ 5.001 m, the scale of a receiver reset.
TEST(RxCbInjectGate, InnovationGateBoundary) {
    double var_eff = 0.0;
    const double thr = 10.0 * std::sqrt(1e-4 + 0.25);
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, 1e-4, 1e-9, thr * 0.999, 10.0, 0.25, var_eff),
              RxCbInjectAction::Fold);
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, 1e-4, 1e-9, thr * 1.001, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipInnov);
    // Sign-symmetric.
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, 1e-4, 1e-9, -thr * 1.001, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipInnov);
}

// Degenerate variances never fold: zero/negative, NaN, and runaway values all
// resolve to SkipGap (the conservative answer), without <cmath> in the header.
TEST(RxCbInjectGate, DegenerateVarianceIsSkipped) {
    double var_eff = 0.0;
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, 0.0, 0.0, 0.0, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipGap);
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, -1.0, 0.0, 0.0, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipGap);
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, std::numeric_limits<double>::quiet_NaN(),
                             0.0, 0.0, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipGap);
    EXPECT_EQ(rxCbInjectGate(0.0, 900.0, 1e31, 0.0, 0.0, 10.0, 0.25, var_eff),
              RxCbInjectAction::SkipGap);
}

// Negative gap (clock jitter across the epoch snap) must not deflate the
// variance: q·gap is clamped at zero, so var_eff == var.
TEST(RxCbInjectGate, NegativeGapDoesNotDeflate) {
    double var_eff = 0.0;
    const auto act = rxCbInjectGate(-5.0, 900.0, 4e-4, 1e-3, 0.0, 10.0, 0.25, var_eff);
    EXPECT_EQ(act, RxCbInjectAction::Fold);
    EXPECT_DOUBLE_EQ(var_eff, 4e-4);
}

}  // namespace
