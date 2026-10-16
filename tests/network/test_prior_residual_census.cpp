/**
 * @file test_prior_residual_census.cpp
 * @brief Prior-residual census (docs/SPEC_prior_residual_census.md): the stale-carry
 *        test of D5, generalized to every carried stiff prior.
 *
 * The census applies the residual of prior information and its dispersion (LASER
 * Guide v2.7 slide 5-74) to every carried stiff prior that has no screen of its own,
 * and reports a per-family empirical exceedance ladder.  Four properties still
 * produce plausible output when they are wrong, so each has its own case:
 *
 *   1. **The dispersion identity.**  D{ê} = p²/(p+d) = r·fold_var underlies every
 *      ladder, and a wrong identity still yields plausible ladders.  Case 1 checks
 *      it against the closed form to 1e-12.
 *   2. **The `folded` branch.**  Applying the folded form to an unfolded row is
 *      anti-conservative (about ×3000 at a 1e-3 redundancy floor, about 3e6 at the
 *      census floor of 1e-9), so every Responded row lands in the top
 *      ladder bin by construction and the §9.3 positive control passes vacuously.
 *      Case 10 keeps that as a negative example: the number it asserts is the
 *      artifact, not a target.
 *   3. **The empirical critical value.**  Cases 2 and 3 are a matched pair: Gaussian
 *      input reproduces the normal tail once σ̂₀ converges, and Student-t(3) input
 *      grossly over-occupies the 4σ̂₀ bin while the geometric mean stays at 1.
 *      Case 3 reproduces the failure mode D5 measured and is expected behavior: a
 *      scale calibration fixes the typical scale and cannot flatten a non-Gaussian
 *      tail.
 *   4. **The two calibration contracts.**  Predict-then-update (case 4: a single
 *      large row does not standardize itself away) and no censoring by threshold
 *      (case 5: censoring biases σ̂₀ low, the mechanical form of the D5 v2 note).
 *
 * The remaining early cases cover the partition (case 6: the three populations are
 * neither pooled nor double-counted), the warm-up flag (case 7), the redundancy clamp
 * for the documented coupling-carry case (case 8), and structural inertness when
 * unarmed (case 9).
 *
 * ## Cases 13-18: the constraint-aware global variance component (the rider)
 *
 * docs/SPEC_prior_residual_census.md §14: the other half of LASER 5-74, which pools
 * the same rows into the global scale
 * @f$\hat\sigma_0^2 = (\tilde e^\top P\tilde e + \tilde e_0^\top P_0 \tilde e_0)
 *                     /(n-m+l)@f$.  The rider uses the census accumulator, so its
 * cases share this file and suite, which keeps one-file-one-suite ownership.
 *
 *   13. **The 5-74 identity**, against a closed form derived without the
 *       accumulator (a one-redundancy scalar net has
 *       @f$\hat\sigma_0^2 = (z-z_0)^2/(d+p)@f$: the misfit between two
 *       independent measurements of the same unknown over their combined
 *       variance).  Wrongly assembled sums leave the ladders unaffected, so no
 *       other case in this file would detect them.
 *   14. **The 5-85 equivalence.**  As the constraint tightens to a datum pin, the
 *       global denominator @f$n-m+l@f$ converges to @f$n-\mathrm{rk}\,A@f$ and
 *       the statistic to the fixed-constraint form; pins need no correction and
 *       get none.  The bias in the global test comes from the non-minimal rows.
 *   15. **Label discipline.**  An unfolded row is not a constraint in this
 *       epoch's solve and moves neither sum; the same holds for the two other
 *       skips, each counted, because a row admitted to the numerator but not the
 *       denominator biases the ratio with no other visible sign.
 *   16. **The approx flag.**  Without a Baarda r_i the fallback changes only
 *       @f$\sum r_{data}@f$ and reports that in-band.
 *   17. **Structural inertness when unarmed**, and the property the field A/B
 *       depends on: arming the rider changes no ladder count anywhere.
 *   18. The per-family split closes, and the per-epoch history is the population
 *       the run-end quantiles claim.
 *
 * Cases 19-21 cover the corroboration consensus rule, its audit columns and the
 * sensitivity column; case 22 covers the rneg re-partition.
 */

#include <gtest/gtest.h>

#include "network/qc/NetworkPriorResidual.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// A census armed on one family, with the warm-up and threshold under test control.
NetworkPriorResidual makeCensus(double min_rows = 200.0,
                                double report_sigma = 4.0,
                                double window_epochs = 0.0,   // 0 => no forgetting
                                bool corroborate = false) {
    return NetworkPriorResidual(std::vector<std::string>{"pl"}, window_epochs,
                                min_rows, report_sigma, corroborate);
}

/// One row; by default an ordinary folded row labelled Clean.
NetworkPriorResidual::Input row(double e, double fold_var, double post_var,
                                bool folded = true,
                                PriorResidualLabel lab = PriorResidualLabel::Clean) {
    NetworkPriorResidual::Input in;
    in.family   = "pl";
    in.sat      = "G01";
    in.station  = 0;
    in.e        = e;
    in.fold_var = fold_var;
    in.post_var = post_var;
    in.gap_s    = 30.0;
    in.folded   = folded;
    in.label    = lab;
    return in;
}

/// A census with the constraint-aware global variance component armed
/// (docs/SPEC_prior_residual_census.md §14).  By default the warm-up threshold is
/// out of reach: the rider's sums do not depend on the σ̂₀ calibration, and keeping
/// the ladder side raw-formal keeps the two concerns separate.
NetworkPriorResidual makeGlobalCensus(double min_rows = 1e18) {
    return NetworkPriorResidual(std::vector<std::string>{"pl"}, /*window_epochs=*/0.0,
                                min_rows, /*report_sigma=*/4.0,
                                /*corroborate=*/false, /*global_sigma0=*/true);
}

/// A scalar Gauss-Markov net with one data row and one stochastic constraint,
/// solved in closed form; the reference for the rider.
///
///   data:        z  = xi + e,   Var{e}  = d = 1/w_d
///   constraint:  z0 = xi + e0,  Var{e0} = p = 1/w_0
///
/// n = 1, m = 1, l = 1 ⇒ the LASER 5-74 redundancy n − m + l = 1.
struct ScalarNet {
    double w_d, w_0, N, xhat, v, e0, r_data, post_var, r_prior;
    ScalarNet(double d, double p, double z, double z0)
        : w_d(1.0 / d), w_0(1.0 / p), N(w_d + w_0),
          xhat((w_d * z + w_0 * z0) / N),
          v(z - xhat), e0(z0 - xhat),
          // Baarda per-row redundancy of the data row: r = w*(1/w - a N^-1 a').
          r_data(w_d * (d - 1.0 / N)),
          // The posterior variance the carry harvest would report for xi...
          post_var(1.0 / N),
          // ...from which the census forms the constraint row's partial
          // redundancy exactly as addRow does.
          r_prior(1.0 - (1.0 / N) / p) {}
};

/// Feed @p n standardized draws from @p gen as folded rows whose dispersion is
/// exactly 1 m², so `s` equals the draw and the calibration sees it unmodified.
/// fold_var = 2, post_var = 1 ⇒ r = 0.5 ⇒ D{ê} = 1.
template <typename Gen, typename Dist>
void feedUnitRows(NetworkPriorResidual& c, int n, Gen& g, Dist& d) {
    for (int i = 0; i < n; ++i) c.addRow(row(d(g), 2.0, 1.0));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. The dispersion identity, on which every other case depends.
// ---------------------------------------------------------------------------
// Hand-built scalar fold: prior variance p, data-only variance d, posterior
// v = pd/(p+d).  LASER 5-74's D̂{ẽ₀} specialized to one scalar constraint is
// r·fold_var with r = 1 − w·v; the direct algebra gives p²/(p+d); and
// fold_var − post_var gives the same value.  All three agree.
TEST(PriorResidualCensusTest, DispersionIdentityMatchesClosedForm) {
    // The cases span eight decades of p/d ratio.  The p << d cases matter most:
    // r = p/(p+d) goes to zero on the stiffest priors ({1e-4, 1.0} gives r = 1e-4),
    // which is the standing-invariant-#1 population the census targets.  A 1e-3
    // redundancy floor (the one NetworkCarryVce uses) would clamp those rows,
    // inflating their D{e} tenfold and shrinking their |t| by sqrt(10); the
    // assertion on r_clamped below guards that.  See kRedundancyFloor for why the
    // census floor is 1e-9.
    const double cases[][2] = {{1e-4, 1e-4}, {1e-4, 1.0},  {1.0, 1e-4},
                              {1e-6, 1e-2},  {25.0, 3.0},  {1e2, 1e-8},
                              {1e-6, 1.0},   {1e-8, 1.0}};   // r = 1e-6, 1e-8
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);  // stay raw-formal
    c.beginEpoch(0.0, 0.0);
    for (const auto& pd : cases) {
        const double p = pd[0], d = pd[1];
        const double v = p * d / (p + d);                 // posterior variance
        const auto st = c.addRow(row(/*e=*/0.0, /*fold_var=*/p, /*post_var=*/v));
        ASSERT_TRUE(st.accepted) << "p=" << p << " d=" << d;
        EXPECT_FALSE(st.r_clamped) << "p=" << p << " d=" << d;
        const double closed = p * p / (p + d);            // §1 direct algebra
        const double r_true = p / (p + d);
        // r = 1 - v/p is a difference of near-equal doubles, so its relative
        // precision is about eps/r.  The identity is exact in exact arithmetic;
        // this is the double-precision floor on evaluating it.  At r = 1e-6 that is
        // about 1e-9 relative; at the kRedundancyFloor of 1e-9 it is about 1e-6,
        // which is why the floor is not lower.  Well-conditioned rows (r >~ 1e-4)
        // hold the spec's 1e-12.
        const double tol = std::max(1e-12, 4.0 * 2.220446049250313e-16 / r_true);
        EXPECT_NEAR(st.disp / closed, 1.0, tol) << "p=" << p << " d=" << d;
        EXPECT_NEAR(st.disp / (p - v), 1.0, tol) << "fold_var - post_var";
        EXPECT_NEAR(st.r / r_true, 1.0, tol)
            << "partial redundancy r = 1 - w*v (p=" << p << " d=" << d << ")";
    }
}

// ---------------------------------------------------------------------------
// 2. Gaussian input: the ladder is unbiased when the Gaussian assumption holds.
// ---------------------------------------------------------------------------
// This is the control for §2's claim that the field data does not satisfy the
// assumption: if the ladder over-occupied on clean Gaussian input, case 3's
// over-occupancy would be an artifact of this class rather than of the data.
TEST(PriorResidualCensusTest, GaussianRowsReproduceTheNormalTail) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1000.0);
    std::mt19937 g(20260804u);
    std::normal_distribution<double> nd(0.0, 1.0);

    // Epoch 0 calibrates (every row raw-formal at σ̂₀ = 1, which is also the truth).
    c.beginEpoch(0.0, 0.0);
    feedUnitRows(c, 200000, g, nd);
    // Epoch 1 is measured against the σ̂₀ frozen at epoch start, now converged.
    c.beginEpoch(30.0, 30.0);
    ASSERT_TRUE(c.sigma0Warm(0));
    EXPECT_NEAR(c.sigma0(0), 1.0, 0.02);
    const int kN = 400000;
    feedUnitRows(c, kN, g, nd);

    const auto& L = c.epochLadder(0, PriorResidualLabel::Clean);
    ASSERT_EQ(L.n_rows, kN);
    EXPECT_NEAR(L.geoT2(), 1.0, 0.05);
    // Two-sided normal tails: 31.73 % / 4.55 % / 0.270 % / 0.00633 %.
    EXPECT_NEAR(L.frac(0), 0.31731, 0.01);     // |t| > 1
    EXPECT_NEAR(L.frac(1), 0.04550, 0.004);    // |t| > 2
    EXPECT_NEAR(L.frac(2), 0.00270, 0.001);    // |t| > 3
    // |t| > 4 expects ~25 rows in 4e5.  Loose bounds (this is a Poisson count),
    // but three orders of magnitude clear of case 3's heavy tail.
    EXPECT_LT(L.frac(3), 3.0e-4);
    EXPECT_EQ(L.bins[NetworkPriorResidual::kLadderBins - 1], 0);  // nothing past 200σ
}

// ---------------------------------------------------------------------------
// 3. The D5 failure mode, reproduced (expected behavior, not a defect).
// ---------------------------------------------------------------------------
// Student-t(3) input: the geometric-mean σ̂₀ calibration converges (geo(t²) ≈ 1,
// so the typical scale is right) while the 4σ̂₀ bin is over-occupied by more than
// two orders of magnitude against the Gaussian tail.  D5 measured the same in the
// field: a 4·σ̂₀ gate still flagged about 17 % per epoch where a clean 4σ Gaussian
// gives 0.006 %.  A scale calibration cannot flatten a non-Gaussian tail, which is
// why the census reports the empirical ladder and does not use LASER's t/F
// critical values.
TEST(PriorResidualCensusTest, HeavyTailOverOccupiesTheLadderWhileGeoMeanStaysAtOne) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1000.0);
    std::mt19937 g(20260804u);
    std::student_t_distribution<double> td(3.0);

    c.beginEpoch(0.0, 0.0);
    feedUnitRows(c, 200000, g, td);
    c.beginEpoch(30.0, 30.0);
    ASSERT_TRUE(c.sigma0Warm(0));
    const int kN = 400000;
    feedUnitRows(c, kN, g, td);

    const auto& L = c.epochLadder(0, PriorResidualLabel::Clean);
    ASSERT_EQ(L.n_rows, kN);
    // The calibration fixes the typical scale...
    EXPECT_NEAR(L.geoT2(), 1.0, 0.10);
    // ...and the tail is still over-occupied by at least 100×.
    EXPECT_GT(L.frac(3), 100.0 * 6.33e-5) << "|t| > 4 must be >=100x the Gaussian tail";
    EXPECT_GT(L.bins[7], 0) << "t(3) reaches |t| > 20; a Gaussian sample never would";
}

// ---------------------------------------------------------------------------
// 4. Predict-then-update: a row does not standardize itself away.
// ---------------------------------------------------------------------------
TEST(PriorResidualCensusTest, HugeRowDoesNotCalibrateAgainstItself) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/100.0);
    std::mt19937 g(7u);
    std::normal_distribution<double> nd(0.0, 1.0);
    c.beginEpoch(0.0, 0.0);
    feedUnitRows(c, 5000, g, nd);

    c.beginEpoch(30.0, 30.0);
    const double sigma0_frozen = c.sigma0(0);
    ASSERT_TRUE(c.sigma0Warm(0));
    // One row 500σ out.  If σ̂₀ were re-estimated per row (or later within the
    // epoch), this row would standardize itself toward 1.
    const auto st = c.addRow(row(/*e=*/500.0, 2.0, 1.0));
    ASSERT_TRUE(st.accepted);
    EXPECT_DOUBLE_EQ(st.sigma0, sigma0_frozen);
    EXPECT_GT(std::abs(st.t), 400.0);
    EXPECT_TRUE(st.reported);
    // The frozen σ̂₀ also stays unchanged for the rest of this epoch.
    const auto st2 = c.addRow(row(/*e=*/1.0, 2.0, 1.0));
    EXPECT_DOUBLE_EQ(st2.sigma0, sigma0_frozen);
    // The row does reach the calibration for the next epoch (no censoring by size).
    c.beginEpoch(60.0, 60.0);
    EXPECT_GT(c.sigma0(0), sigma0_frozen);
}

// ---------------------------------------------------------------------------
// 5. No censoring by threshold (the D5 v2 note, stated mechanically).
// ---------------------------------------------------------------------------
// Above-threshold rows are fed into σ̂₀ because censoring them biases σ̂₀ low, and
// under misspecification the flagged rows are the calibration signal.  A low σ̂₀
// inflates every |t|, so a censoring implementation would create the
// over-occupancy the census measures.
TEST(PriorResidualCensusTest, CensoringAboveThresholdRowsBiasesSigma0Low) {
    NetworkPriorResidual full = makeCensus(/*min_rows=*/100.0);
    NetworkPriorResidual cens = makeCensus(/*min_rows=*/100.0);
    std::mt19937 g(11u);
    std::student_t_distribution<double> td(3.0);   // needs a real tail to censor

    full.beginEpoch(0.0, 0.0);
    cens.beginEpoch(0.0, 0.0);
    for (int i = 0; i < 50000; ++i) {
        const double s = td(g);
        auto r_full = row(s, 2.0, 1.0);            // D{ê} = 1 ⇒ e == s
        auto r_cens = r_full;
        if (std::abs(s) > 4.0) r_cens.calibrate = false;   // the censoring under test
        full.addRow(r_full);
        cens.addRow(r_cens);
    }
    full.beginEpoch(30.0, 30.0);
    cens.beginEpoch(30.0, 30.0);
    ASSERT_TRUE(full.sigma0Warm(0));
    ASSERT_TRUE(cens.sigma0Warm(0));
    EXPECT_LT(cens.sigma0(0), full.sigma0(0))
        << "censoring the tail must bias sigma0 LOW (and every |t| high)";
    // The exclusion is counted (n_no_sigma0), so it can be audited.
    EXPECT_GT(cens.runLadder(0, PriorResidualLabel::Clean).n_no_sigma0, 0);
    EXPECT_EQ(full.runLadder(0, PriorResidualLabel::Clean).n_no_sigma0, 0);
}

// ---------------------------------------------------------------------------
// 6. The partition: three populations, neither pooled nor double-counted.
// ---------------------------------------------------------------------------
TEST(PriorResidualCensusTest, LabelLaddersPartitionTheRowsExactly) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);
    c.beginEpoch(0.0, 0.0);
    const int kClean = 37, kResp = 11, kFrame = 5;
    for (int i = 0; i < kClean; ++i)
        c.addRow(row(0.1 * i, 2.0, 1.0, true, PriorResidualLabel::Clean));
    for (int i = 0; i < kResp; ++i)
        c.addRow(row(0.1 * i, 1e-4, 1.0, false, PriorResidualLabel::Responded));
    for (int i = 0; i < kFrame; ++i)
        c.addRow(row(0.1 * i, 2.0, 1.0, false, PriorResidualLabel::Frame));

    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_rows, kClean);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Responded).n_rows, kResp);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Frame).n_rows, kFrame);
    EXPECT_EQ(c.epochRows(), kClean + kResp + kFrame);
    EXPECT_EQ(c.epochRowsResp(), kResp);
    EXPECT_EQ(c.epochRowsFrame(), kFrame);
    EXPECT_EQ(c.runRows(0), kClean + kResp + kFrame);
    // Only the Clean rows took the folded branch here.
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_folded, kClean);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Responded).n_folded, 0);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Frame).n_folded, 0);
    // A new epoch clears the per-epoch ladders and keeps the run ones.
    c.beginEpoch(30.0, 30.0);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_rows, 0);
    EXPECT_EQ(c.runRows(0), kClean + kResp + kFrame);
}

// ---------------------------------------------------------------------------
// 7. Warm-up: below min_rows, t equals s and the row is flagged raw-formal.
// ---------------------------------------------------------------------------
// Standardizing by a seed value without a flag would make a cold ladder
// indistinguishable from a calibrated one; the trace's `sigma0_warm` column
// exists to keep them apart.
TEST(PriorResidualCensusTest, BelowWarmUpReportsRawFormalAndFlagsIt) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1000.0);
    std::mt19937 g(3u);
    std::normal_distribution<double> nd(0.0, 3.0);   // true sigma 3, not 1

    c.beginEpoch(0.0, 0.0);
    feedUnitRows(c, 100, g, nd);                     // far below min_rows
    c.beginEpoch(30.0, 30.0);
    EXPECT_FALSE(c.sigma0Warm(0));
    EXPECT_DOUBLE_EQ(c.sigma0(0), 1.0);
    const auto st = c.addRow(row(/*e=*/6.0, 2.0, 1.0));
    ASSERT_TRUE(st.accepted);
    EXPECT_TRUE(st.raw_formal);
    EXPECT_DOUBLE_EQ(st.t, st.s);
    EXPECT_DOUBLE_EQ(st.sigma0, 1.0);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_raw_formal, 1);

    // Past the warm-up the same row standardizes by the measured scale (~3).
    for (int e = 1; e <= 12; ++e) {
        c.beginEpoch(30.0 * e, 30.0 * e);
        feedUnitRows(c, 200, g, nd);
    }
    c.beginEpoch(1000.0, 1000.0);
    ASSERT_TRUE(c.sigma0Warm(0));
    EXPECT_NEAR(c.sigma0(0), 3.0, 0.4);
    const auto st2 = c.addRow(row(/*e=*/6.0, 2.0, 1.0));
    EXPECT_FALSE(st2.raw_formal);
    EXPECT_LT(std::abs(st2.t), std::abs(st2.s));
}

// ---------------------------------------------------------------------------
// 8. The r clamp: the documented coupling-carry case, not a negative variance.
// ---------------------------------------------------------------------------
// Under network.carry_conditional_coupling the harvested post_var is a
// conditional variance that can legitimately exceed the prior (NetworkCarryVce
// documents this at its own clamp).  fold_var − post_var would then be negative
// and √D{ê} a NaN, so the census clamps r, as NetworkCarryVce does, but to its
// own floor of 1e-9 (kRedundancyFloor) instead of 1e-3.
TEST(PriorResidualCensusTest, PostVarAboveFoldVarClampsInsteadOfGoingNegative) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);
    c.beginEpoch(0.0, 0.0);
    const double fold_var = 1e-4, post_var = 3e-4;      // conditional > prior
    const auto st = c.addRow(row(/*e=*/1e-3, fold_var, post_var));
    ASSERT_TRUE(st.accepted);
    EXPECT_TRUE(st.r_clamped);
    EXPECT_TRUE(st.r_negative) << "post_var > fold_var must be counted SEPARATELY "
                                  "from a merely stiff prior — one is a valid "
                                  "statistic with vanishing power, the other is a "
                                  "prior/posterior pair that does not describe one fold";
    EXPECT_GT(st.disp, 0.0);
    EXPECT_TRUE(std::isfinite(st.s));
    EXPECT_NEAR(st.r, 1e-9, 1e-21);                     // the numerical [eps,1] floor
    EXPECT_NEAR(st.disp, 1e-9 * fold_var, 1e-24);
    // With the rneg re-partition (case 22) the row is counted in the rneg class,
    // not in the label ladder: its |t| is not a standardized residual, so it is
    // out of every bin, out of sum_log_t2 and out of the worst-|t| line.  The
    // clamp itself is unchanged, and that is what this case pins.
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_r_clamped, 0);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_r_negative, 0);
    EXPECT_EQ(c.rnegEpochLadder(0, PriorResidualLabel::Clean).n_rows, 1);
    EXPECT_EQ(c.rnegEpochLadder(0, PriorResidualLabel::Clean).n_r_clamped, 1);
    EXPECT_EQ(c.rnegEpochLadder(0, PriorResidualLabel::Clean).n_r_negative, 1);
    // A stiff prior (r = 1e-6, three decades under NetworkCarryVce's 1e-3 floor)
    // passes through unclamped; that is the purpose of the tighter floor, and
    // clamping it would hide a stale stiff carry.
    const double p = 1e-6, d = 1.0, v = p * d / (p + d);
    const auto stiff = c.addRow(row(/*e=*/1e-3, p, v));
    EXPECT_FALSE(stiff.r_clamped);
    EXPECT_FALSE(stiff.r_negative);
    EXPECT_NEAR(stiff.r / (p / (p + d)), 1.0, 1e-9);
    // The upper end of the clamp too: a zero posterior gives r = 1 exactly.
    const auto st2 = c.addRow(row(/*e=*/1e-3, fold_var, 0.0));
    EXPECT_FALSE(st2.r_clamped);
    EXPECT_NEAR(st2.r, 1.0, 1e-15);
    EXPECT_NEAR(st2.disp, fold_var, 1e-18);
}

// ---------------------------------------------------------------------------
// 9. An unarmed census is structurally inert.
// ---------------------------------------------------------------------------
TEST(PriorResidualCensusTest, UnarmedCensusIsStructurallyInert) {
    NetworkPriorResidual off(std::vector<std::string>{}, 60.0);
    EXPECT_FALSE(off.enabled());
    EXPECT_FALSE(off.wants("pl"));
    EXPECT_EQ(off.familyIndex("pl"), -1);
    off.beginEpoch(0.0, 0.0);
    const auto st = off.addRow(row(1e6, 1e-6, 1e-9));
    EXPECT_FALSE(st.accepted);
    EXPECT_EQ(st.family, -1);
    EXPECT_EQ(off.epochs(), 0) << "an unarmed census must not even count epochs";
    EXPECT_EQ(off.runRowsTotal(), 0);
    EXPECT_EQ(off.epochRows(), 0);
    EXPECT_TRUE(off.hits().empty());

    // An armed census still ignores a family it was not configured for.
    NetworkPriorResidual on = makeCensus();
    on.beginEpoch(0.0, 0.0);
    auto other = row(1e6, 1e-6, 1e-9);
    other.family = "cb";
    EXPECT_FALSE(on.addRow(other).accepted);
    EXPECT_EQ(on.epochRows(), 0);
}

// ---------------------------------------------------------------------------
// 10. The branch test, with the wrong-branch artifact kept as a negative example.
// ---------------------------------------------------------------------------
// The sign flip follows from the covariance, not from a convention.  With the
// fold in, Cov(z₀, ξ̂) = post_var, so D{z₀ − ξ̂} = fold_var + post_var − 2·post_var.
// With the fold out, prior and solve are independent and the variances add.
//
// The second half is the failure mode: a refloat-scale row (post_var =
// 1e4·fold_var) sent through the folded branch has its r clamped to ε and its |t|
// inflated (about ×3000 at a 1e-3 floor, far more at the census floor; see
// below).  If the field wiring did that, every Responded row would land in the
// top ladder bin by construction, and §9 gate 3 (the positive control) would pass
// vacuously.
TEST(PriorResidualCensusTest, FoldedAndUnfoldedBranchesAndTheClampArtifact) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);
    c.beginEpoch(0.0, 0.0);
    const double fold_var = 4e-4, post_var = 1e-4, e = 0.01;

    const auto fold = c.addRow(row(e, fold_var, post_var, /*folded=*/true));
    const auto free = c.addRow(row(e, fold_var, post_var, /*folded=*/false));
    ASSERT_TRUE(fold.accepted);
    ASSERT_TRUE(free.accepted);
    EXPECT_NEAR(fold.disp, fold_var - post_var, 1e-18);
    EXPECT_NEAR(free.disp, fold_var + post_var, 1e-18);
    EXPECT_GT(std::abs(fold.s), std::abs(free.s))
        << "the folded form is the TIGHTER null — that is why using it on an "
           "unfolded row is anti-conservative";

    // The artifact.  A refloated ambiguity: 1e-4 m² carry against a 1 m² refloat.
    const double refloat_fold = 1e-4, refloat_post = 1.0;   // = 1e4 x fold_var
    const auto correct = c.addRow(row(e, refloat_fold, refloat_post, /*folded=*/false,
                                      PriorResidualLabel::Responded));
    const auto wrong   = c.addRow(row(e, refloat_fold, refloat_post, /*folded=*/true,
                                      PriorResidualLabel::Responded));
    ASSERT_TRUE(correct.accepted);
    ASSERT_TRUE(wrong.accepted);
    EXPECT_TRUE(wrong.r_clamped);
    EXPECT_FALSE(correct.r_clamped);
    EXPECT_TRUE(wrong.r_negative);
    // The spec quotes about ×3000, computed at NetworkCarryVce's eps = 1e-3.  The
    // census floors at 1e-9 (see kRedundancyFloor; a 1e-3 floor would truncate
    // stiff priors, case 1), so the artifact is far larger here:
    // sqrt((p+v)/(eps*p)) = sqrt(1.0001 / 1e-13) ~ 3.2e6.  The number is the
    // artifact's size, not a target; it is asserted so that a Responded ladder
    // pinned at the top bin can be recognized by this signature.
    const double ratio = std::abs(wrong.t) / std::abs(correct.t);
    EXPECT_NEAR(ratio / std::sqrt((refloat_fold + refloat_post)
                                  / (1e-9 * refloat_fold)), 1.0, 1e-6);
    EXPECT_GT(ratio, 1e6);
}

// ---------------------------------------------------------------------------
// 11. The corroboration contingency table; the b-cell is the reported number (INC-3).
// ---------------------------------------------------------------------------
// Not in the spec's numbered list, but the join is a v1 deliverable (D5 went three
// cycles without this measurement), so the wiring has its own case: the resolver
// is consulted per row, the split lands on the report threshold, and the cells
// close.
TEST(PriorResidualCensusTest, CorroborationJoinFillsTheContingencyTable) {
    NetworkPriorResidual c(std::vector<std::string>{"pl"}, /*window*/ 0.0,
                           /*min_rows=*/1e18, /*report_sigma=*/4.0,
                           /*corroborate=*/true);
    // Resolver keyed off the satellite id so the test controls each row's cell.
    // One-member domains, so priorResidualConsensus reduces to the member's own
    // verdict and this case tests the table rather than the rule (the rule has
    // its own cases, 19 and 20).
    c.setCorroborator([](std::string_view, int, std::string_view sat) {
        if (sat == "G01") return PriorResidualEvidence{1, 1};   // Stepped
        if (sat == "G02") return PriorResidualEvidence{1, 0};   // Flat
        return PriorResidualEvidence{0, 0};                     // NoEvidence
    });
    c.beginEpoch(0.0, 0.0);
    // D{ê} = 1 here, and the census is raw-formal (σ̂₀ = 1), so e equals |t|.
    const char* sats[] = {"G01", "G02", "G03"};
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 5; ++i) {          // above the 4σ report threshold
            auto r = row(10.0, 2.0, 1.0);  r.sat = sats[k];  c.addRow(r);
        }
        for (int i = 0; i < 7; ++i) {          // at or below it
            auto r = row(0.5, 2.0, 1.0);   r.sat = sats[k];  c.addRow(r);
        }
    }
    const auto& L = c.epochLadder(0, PriorResidualLabel::Clean);
    EXPECT_EQ(L.contingency[0][static_cast<int>(PriorResidualCorrob::Stepped)], 5);
    EXPECT_EQ(L.contingency[0][static_cast<int>(PriorResidualCorrob::Flat)], 5);
    EXPECT_EQ(L.contingency[0][static_cast<int>(PriorResidualCorrob::NoEvidence)], 5);
    EXPECT_EQ(L.contingency[1][static_cast<int>(PriorResidualCorrob::Stepped)], 7);
    EXPECT_EQ(L.contingency[1][static_cast<int>(PriorResidualCorrob::Flat)], 7);
    EXPECT_EQ(L.contingency[1][static_cast<int>(PriorResidualCorrob::NoEvidence)], 7);
    long long tot = 0;
    for (int r = 0; r < 2; ++r)
        for (int k = 0; k < kPriorResidualCorrobs; ++k) tot += L.contingency[r][k];
    EXPECT_EQ(tot, L.n_rows) << "the table must close over every row";
    // An unarmed join leaves the table empty rather than defaulting a column.
    NetworkPriorResidual plain = makeCensus(/*min_rows=*/1e18);
    plain.beginEpoch(0.0, 0.0);
    plain.addRow(row(10.0, 2.0, 1.0));
    const auto& P = plain.epochLadder(0, PriorResidualLabel::Clean);
    for (int r = 0; r < 2; ++r)
        for (int k = 0; k < kPriorResidualCorrobs; ++k)
            EXPECT_EQ(P.contingency[r][k], 0);
}

// ---------------------------------------------------------------------------
// 12. The bounded identity buffer never costs a ladder count.
// ---------------------------------------------------------------------------
// Standing invariant #4: a truncated event stream does not make a busy epoch read
// as a quiet one.  The cap thins identities only; ladder counts stay complete.
TEST(PriorResidualCensusTest, IdentityBufferCapNeverThinsTheLadder) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18, /*report_sigma=*/4.0);
    c.beginEpoch(0.0, 0.0);
    const int kN = static_cast<int>(NetworkPriorResidual::kMaxHitsPerEpoch) + 250;
    for (int i = 0; i < kN; ++i) c.addRow(row(10.0, 2.0, 1.0));
    EXPECT_EQ(c.hits().size(), NetworkPriorResidual::kMaxHitsPerEpoch);
    EXPECT_EQ(c.hitsDropped(), 250);
    EXPECT_EQ(c.epochReported(), kN);
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).bins[3], kN);
    // …and the buffer resets per epoch.
    c.beginEpoch(30.0, 30.0);
    EXPECT_TRUE(c.hits().empty());
    EXPECT_EQ(c.hitsDropped(), 0);
}

// ===========================================================================
//  The constraint-aware global variance component
//  (docs/SPEC_prior_residual_census.md §14): the other half of LASER 5-74
// ===========================================================================

// ---------------------------------------------------------------------------
// 13. The 5-74 identity, on which the rider depends.
// ---------------------------------------------------------------------------
// sigma0^2 = (v'Pv + e0'P0e0)/(n − m + l): prior residuals in the numerator,
// constraints in the redundancy.  Checked against a closed form derived without
// the accumulator: for a one-redundancy scalar net the variance factor is the
// squared misfit between two independent measurements of the same unknown over
// their combined variance, (z − z0)^2/(d + p).  Wrongly assembled sums would
// leave the ladders unaffected, so no other case in this file would detect them.
TEST(PriorResidualCensusTest, Laser574GlobalIdentityMatchesClosedForm) {
    // Spread over eight decades of p/d, i.e. from a loose carry to a stiff one.
    const double cases[][4] = {
        //   d       p       z      z0
        {  1.0,   1.0,    0.30, -0.20},
        {  1e-4,  1e-2,   0.01,  0.03},
        {  1e-2,  1e-6,  -0.05, -0.049},
        {  4.0,   0.25,   1.50,  0.75},
        {  1e-6,  1e-6,   1e-3,  2e-3},
    };
    for (const auto& cs : cases) {
        const double d = cs[0], p = cs[1], z = cs[2], z0 = cs[3];
        const ScalarNet n(d, p, z, z0);
        NetworkPriorResidual c = makeGlobalCensus();
        ASSERT_TRUE(c.globalOn());
        c.beginEpoch(0.0, 0.0);
        c.addDataRow(n.w_d, n.v, n.r_data, /*r_is_approx=*/false);
        const auto st = c.addRow(row(n.e0, /*fold_var=*/p, /*post_var=*/n.post_var));
        ASSERT_TRUE(st.accepted);
        ASSERT_FALSE(st.r_clamped) << "d=" << d << " p=" << p;

        const auto g = c.epochGlobal();
        ASSERT_TRUE(g.formed);
        EXPECT_EQ(g.flags, 0) << "a complete epoch degrades in no way";

        // (a) the numerator halves, each against its own definition
        EXPECT_NEAR(g.s.wv2 / (n.w_d * n.v * n.v), 1.0, 1e-12);
        EXPECT_NEAR(g.s.we2 / (n.w_0 * n.e0 * n.e0), 1.0, 1e-12);
        // (b) The redundancy.  Slide 5-74's n − m + l = 1 − 1 + 1 = 1 is the sum
        //     of the two per-row redundancies, which is what allows the two
        //     pools to be added.
        EXPECT_NEAR(g.s.r_data + g.s.r_prior, 1.0, 1e-12)
            << "sum of partial redundancies must equal n - m + l (d=" << d
            << " p=" << p << ")";
        // (c) the statistic, against the independent closed form.  The tolerance
        //     is conditioning-aware for the same reason as case 1's, and the
        //     conditioning here is that of the residuals, not the data: on a stiff
        //     prior the solution sits almost on z0, so e0 = z0 − xi_hat is a
        //     difference of near-equal doubles, while the closed form reaches the
        //     same number through (z − z0).  The two computations differ at the
        //     cancellation level, so no tighter tolerance can be resolved.
        //     Well-conditioned nets hold the spec's 1e-12.
        constexpr double kEps = 2.220446049250313e-16;
        const double cond = (std::abs(z)  + std::abs(n.xhat)) / std::abs(n.v)
                          + (std::abs(z0) + std::abs(n.xhat)) / std::abs(n.e0);
        const double tol = std::max(1e-12, 40.0 * kEps * cond);
        EXPECT_NEAR(g.sigma0_global_v / ((z - z0) * (z - z0) / (d + p)), 1.0, tol)
            << "sigma0^2_global vs (z-z0)^2/(d+p)  (d=" << d << " p=" << p << ")";
        EXPECT_DOUBLE_EQ(g.sigma0_global, std::sqrt(g.sigma0_global_v));
        // (d) the headline ratio.  w_0*e0^2 / (w_d*v^2) = w_d/w_0 = p/d exactly:
        //     on this net "how much of the tension lives in the carry" is the
        //     variance ratio, and a tension of 1 means prior and data disagree in
        //     equal measure.
        ASSERT_TRUE(g.tension_defined);
        EXPECT_NEAR(g.prior_tension / (p / d), 1.0, tol);
        // (e) each side is self-consistent: E[w*resid^2] = r for both pools,
        //     which is why a vanishing-r row cannot bias the ratio.
        EXPECT_NEAR((g.s.wv2 / g.s.r_data) / (g.s.we2 / g.s.r_prior), 1.0, 1e-9)
            << "the two pools must imply the SAME variance factor on a net with "
               "one degree of freedom";
    }
}

// ---------------------------------------------------------------------------
// 14. The 5-85 equivalence: a minimal constraint needs no correction.
// ---------------------------------------------------------------------------
// Slide 5-85: for a minimal constraint set n − m + l = n − rk A.  As the
// stochastic constraint's variance goes to zero (a datum pin), the global form
// converges to the fixed-constraint statistic e'Pe/(n − rk A); here 1 data row,
// 0 free unknowns, so redundancy 1 and sigma0^2 = w_d (z − z0)^2.  This is why
// the census corrects the non-minimal stochastic rows (the carry families) and
// leaves the datum pins alone: a pin's contribution vanishes from numerator and
// denominator together.
TEST(PriorResidualCensusTest, Laser585MinimalConstraintEquivalence) {
    const double d = 1.0, z = 0.42, z0 = -0.17;
    const double fixed = (z - z0) * (z - z0) / d;   // e'Pe/(n − rk A), rk A = 0
    double prev_err = 1e9;
    for (const double p : {1e-2, 1e-4, 1e-6, 1e-8}) {
        const ScalarNet n(d, p, z, z0);
        NetworkPriorResidual c = makeGlobalCensus();
        c.beginEpoch(0.0, 0.0);
        c.addDataRow(n.w_d, n.v, n.r_data, false);
        const auto st = c.addRow(row(n.e0, p, n.post_var));
        ASSERT_TRUE(st.accepted);
        // The pin is not clamped away: r_prior ~ p/d = 1e-8 at the tightest,
        // still a decade above the numerical floor.  (This is why that floor is
        // 1e-9 and not NetworkCarryVce's 1e-3.)
        EXPECT_FALSE(st.r_clamped) << "p=" << p;
        const auto g = c.epochGlobal();
        ASSERT_TRUE(g.formed);
        // The denominators coincide: n − m + l -> n − rk A = 1.
        EXPECT_NEAR(g.s.r_data + g.s.r_prior, 1.0, 1e-12);
        EXPECT_NEAR(g.s.r_data, 1.0, 2.0 * p / d) << "the data row absorbs the whole "
                                                     "redundancy as the pin tightens";
        const double err = std::abs(g.sigma0_global_v / fixed - 1.0);
        EXPECT_LT(err, prev_err) << "convergence must be MONOTONE in p (p=" << p << ")";
        prev_err = err;
    }
    EXPECT_LT(prev_err, 1e-6)
        << "at p = 1e-8 the stochastic-constraint form must agree with the "
           "fixed-constraint one to 1e-6 — LASER 5-85's claim, measured";
}

// ---------------------------------------------------------------------------
// 15. Label and branch discipline: three skips, each counted.
// ---------------------------------------------------------------------------
// An unfolded row is not a stochastic constraint in this epoch's solve, so it is
// in neither e0'P0e0 nor l.  A nominal-variance row has no real weight w.  An
// r_negative row is a (prior, posterior) pair that does not describe one fold:
// its numerator is real and its redundancy is not, and admitting one without the
// other is the bias the rider exists to remove.  Every skip is counted and
// reported, because a one-sided row shifts the ratio with no other visible sign.
TEST(PriorResidualCensusTest, UnfoldedAndUnusableRowsMoveNeitherGlobalSum) {
    NetworkPriorResidual c = makeGlobalCensus();
    c.beginEpoch(0.0, 0.0);
    c.addDataRow(4.0, 0.5, 0.75, false);            // Σwv² = 1, Σr_data = 0.75
    c.addRow(row(/*e=*/0.02, /*fold_var=*/4e-4, /*post_var=*/1e-4));  // the only counted prior row
    const auto base = c.epochGlobal();
    ASSERT_EQ(base.s.n_prior, 1);
    const double we2_0 = base.s.we2, rp_0 = base.s.r_prior;
    EXPECT_NEAR(we2_0, 0.02 * 0.02 / 4e-4, 1e-15);
    EXPECT_NEAR(rp_0, 0.75, 1e-15);

    // (a) an unfolded Responded refloat, the population whose folded-form misuse
    //     inflates |t| by ~3e6 in case 10.  Here it enters neither sum.
    c.addRow(row(1.0, 1e-4, 1.0, /*folded=*/false, PriorResidualLabel::Responded));
    // (b) a nominal fold variance (the cta_refloat population).
    auto nom = row(1.0, 4e-4, 1e-4);  nom.var_nominal = true;  nom.calibrate = false;
    c.addRow(nom);
    // (c) post_var > fold_var: the coupling-carry pair.
    c.addRow(row(1.0, 1e-4, 3e-4));

    const auto g = c.epochGlobal();
    EXPECT_EQ(g.s.n_prior, 1)          << "no skipped row may enter the count";
    EXPECT_DOUBLE_EQ(g.s.we2, we2_0)   << "no skipped row may enter the numerator";
    EXPECT_DOUBLE_EQ(g.s.r_prior, rp_0)<< "no skipped row may enter the redundancy";
    EXPECT_EQ(g.s.n_prior_unfolded, 1);
    EXPECT_EQ(g.s.n_prior_nominal, 1);
    EXPECT_EQ(g.s.n_prior_rneg, 1);
    EXPECT_TRUE(g.flags & NetworkPriorResidual::kGlobalRnegDrop)
        << "an r_negative drop must be visible in-band, not only in a counter";
    EXPECT_FALSE(g.flags & NetworkPriorResidual::kGlobalNoPrior);
    // The ladder side, by contrast, saw all four rows: the census tests every row
    // and the rider pools only the rows the identity admits.  That asymmetry is by
    // design, and it is why the two share one class but have two readouts.
    //
    // With the rneg re-partition (case 22) the two readouts also agree on row (c)
    // (post_var > fold_var): the ladder side puts it in the rneg class, and the
    // rider drops it (kGlobalRnegDrop).  Rows (a) unfolded and (b) var_nominal
    // remain ordinary label rows: the rider skips them because they are not
    // stochastic constraints in this epoch's solve, which says nothing about
    // whether their |t| is a valid statistic.  The total stays at four.
    long long label_rows = 0, rneg_rows = 0;
    for (int li = 0; li < kPriorResidualLabels; ++li) {
        const auto lab = static_cast<PriorResidualLabel>(li);
        label_rows += c.epochLadder(0, lab).n_rows;
        rneg_rows  += c.rnegEpochLadder(0, lab).n_rows;
    }
    EXPECT_EQ(label_rows, 3);
    EXPECT_EQ(rneg_rows, 1);
    EXPECT_EQ(label_rows + rneg_rows, 4);
    EXPECT_EQ(c.epochRows(), 4) << "the epoch row count is the TOTAL, so it still closes";
    EXPECT_EQ(c.epochRowsRneg(), 1);
}

// ---------------------------------------------------------------------------
// 16. The approx fallback: degraded, flagged, and biased in one known direction.
// ---------------------------------------------------------------------------
// Without network.raim_redundancy_normalize there is no true Σr_data.  The
// r == 1 fallback leaves the numerator untouched and inflates the denominator, so
// sigma0_global reads low.  A reader who knows the direction can still use it,
// which is why the flag is in-band per epoch rather than a footnote.
TEST(PriorResidualCensusTest, ApproxRedundancyFlagChangesOnlyTheDenominator) {
    NetworkPriorResidual real = makeGlobalCensus();
    NetworkPriorResidual appx = makeGlobalCensus();
    real.beginEpoch(0.0, 0.0);
    appx.beginEpoch(0.0, 0.0);
    const double rs[] = {0.10, 0.35, 0.80, 0.02};
    for (int i = 0; i < 4; ++i) {
        const double w = 2.0 + i, v = 0.01 * (i + 1);
        real.addDataRow(w, v, rs[i], /*r_is_approx=*/false);
        appx.addDataRow(w, v, 1.0,  /*r_is_approx=*/true);
    }
    real.addRow(row(0.02, 4e-4, 1e-4));
    appx.addRow(row(0.02, 4e-4, 1e-4));
    const auto gr = real.epochGlobal();
    const auto ga = appx.epochGlobal();

    EXPECT_DOUBLE_EQ(ga.s.wv2, gr.s.wv2) << "the numerator must be untouched";
    EXPECT_DOUBLE_EQ(ga.s.we2, gr.s.we2);
    EXPECT_DOUBLE_EQ(ga.s.r_prior, gr.s.r_prior);
    EXPECT_EQ(ga.s.n_data, 4);
    EXPECT_EQ(ga.s.n_data_approx, 4);
    EXPECT_EQ(gr.s.n_data_approx, 0);
    EXPECT_NEAR(ga.s.r_data, 4.0, 1e-15) << "r == 1 makes Sum-r a ROW COUNT";
    EXPECT_NEAR(gr.s.r_data, 0.10 + 0.35 + 0.80 + 0.02, 1e-15);
    EXPECT_TRUE(ga.flags & NetworkPriorResidual::kGlobalApprox);
    EXPECT_FALSE(gr.flags & NetworkPriorResidual::kGlobalApprox);
    EXPECT_LT(ga.sigma0_global, gr.sigma0_global)
        << "the fallback over-counts the redundancy, so the degraded number is "
           "LOW — a known direction, not an unknown error";
    // The other two in-band states: an epoch with no data rows is prior-only
    // (kGlobalNoData), and one with no folded priors is data-only (kGlobalNoPrior,
    // the quantity the rider exists to correct).
    NetworkPriorResidual pr = makeGlobalCensus();
    pr.beginEpoch(0.0, 0.0);
    pr.addRow(row(0.02, 4e-4, 1e-4));
    EXPECT_TRUE(pr.epochGlobal().flags & NetworkPriorResidual::kGlobalNoData);
    NetworkPriorResidual dt = makeGlobalCensus();
    dt.beginEpoch(0.0, 0.0);
    dt.addDataRow(2.0, 0.01, 0.5, false);
    EXPECT_TRUE(dt.epochGlobal().flags & NetworkPriorResidual::kGlobalNoPrior);
    // An epoch with no rows at all forms no number (blank, not zero).
    NetworkPriorResidual none = makeGlobalCensus();
    none.beginEpoch(0.0, 0.0);
    const auto gn = none.epochGlobal();
    EXPECT_FALSE(gn.formed);
    EXPECT_TRUE(gn.flags & NetworkPriorResidual::kGlobalDegenerate);
    EXPECT_FALSE(gn.tension_defined);
}

// ---------------------------------------------------------------------------
// 17. The rider is structurally inert when off and changes no ladder when on.
// ---------------------------------------------------------------------------
// The second half is the property the field A/B depends on: arming the rider
// changes no census count anywhere.  If it did, the census's own gates would
// stop being comparable across arms without any sign of it.
TEST(PriorResidualCensusTest, GlobalRiderIsInertOffAndLadderNeutralOn) {
    NetworkPriorResidual off = makeCensus(/*min_rows=*/1e18);
    EXPECT_FALSE(off.globalOn());
    off.beginEpoch(0.0, 0.0);
    off.addDataRow(1e6, 1e3, 0.5, false);            // must be a no-op
    off.addRow(row(0.02, 4e-4, 1e-4));
    const auto go = off.epochGlobal();
    EXPECT_FALSE(go.formed);
    EXPECT_EQ(go.s.n_data, 0);
    EXPECT_EQ(go.s.n_prior, 0);
    EXPECT_DOUBLE_EQ(go.s.wv2, 0.0);
    EXPECT_DOUBLE_EQ(go.s.we2, 0.0);
    EXPECT_EQ(off.globalEpochs(), 0);
    off.recordEpochGlobal();
    EXPECT_EQ(off.globalEpochs(), 0) << "an unarmed rider must not even count epochs";
    EXPECT_EQ(off.globalSampleCount(), 0u);

    // Same row stream through both arms: every ladder count must match exactly.
    NetworkPriorResidual on = makeGlobalCensus();
    NetworkPriorResidual ref = makeCensus(/*min_rows=*/1e18);
    for (int ep = 0; ep < 3; ++ep) {
        on.beginEpoch(30.0 * ep, 30.0 * ep);
        ref.beginEpoch(30.0 * ep, 30.0 * ep);
        on.addDataRow(3.0, 0.02, 0.4, false);
        for (int i = 0; i < 9; ++i) {
            const auto lab = static_cast<PriorResidualLabel>(i % kPriorResidualLabels);
            auto r = row(0.001 * (i + 1), 4e-4, 1e-4, (i % 2) == 0, lab);
            if (i == 4) r.var_nominal = true;        // the nominal-variance skip
            if (i == 6) r.post_var = 9e-4;           // the r_negative skip (i even ⇒ folded)
            on.addRow(r);
            ref.addRow(r);
        }
    }
    for (int li = 0; li < kPriorResidualLabels; ++li) {
        const auto lab = static_cast<PriorResidualLabel>(li);
        const auto& A = on.runLadder(0, lab);
        const auto& B = ref.runLadder(0, lab);
        EXPECT_EQ(A.n_rows, B.n_rows);
        EXPECT_EQ(A.n_folded, B.n_folded);
        EXPECT_EQ(A.n_raw_formal, B.n_raw_formal);
        EXPECT_EQ(A.n_r_clamped, B.n_r_clamped);
        EXPECT_EQ(A.n_r_negative, B.n_r_negative);
        EXPECT_EQ(A.n_no_sigma0, B.n_no_sigma0);
        EXPECT_DOUBLE_EQ(A.max_abs_t, B.max_abs_t);
        EXPECT_DOUBLE_EQ(A.sum_log_t2, B.sum_log_t2);
        for (int k = 0; k < NetworkPriorResidual::kLadderBins; ++k)
            EXPECT_EQ(A.bins[k], B.bins[k]) << "label " << li << " rung " << k;
    }
    EXPECT_DOUBLE_EQ(on.sigma0Neff(0), ref.sigma0Neff(0));
}

// ---------------------------------------------------------------------------
// 18. The per-family split closes, and the history is what the quantiles claim.
// ---------------------------------------------------------------------------
// The split is the column that reconciles with M2's per-family robust sigma-hat
// to about a factor of 2; a split that did not sum to the total would make that
// reconciliation meaningless.  The history is the population the run-end median
// and p95 are quoted over, and it holds exactly the drained epochs.
TEST(PriorResidualCensusTest, GlobalPerFamilySplitClosesAndHistoryTracksDrainedEpochs) {
    NetworkPriorResidual c(std::vector<std::string>{"pl", "cb"}, /*window*/ 0.0,
                           /*min_rows=*/1e18, /*report_sigma=*/4.0,
                           /*corroborate=*/false, /*global_sigma0=*/true);
    for (int ep = 0; ep < 4; ++ep) {
        c.beginEpoch(30.0 * ep, 30.0 * ep);
        c.addDataRow(2.0, 0.01 * (ep + 1), 0.5, false);
        auto a = row(0.002 * (ep + 1), 4e-4, 1e-4);
        auto b = row(0.003 * (ep + 1), 1e-3, 2e-4);  b.family = "cb";
        c.addRow(a);
        c.addRow(b);
        // Only three of the four epochs are drained (one stands for an epoch
        // withheld between the census's beginEpoch and the emission seam).
        if (ep != 2) c.recordEpochGlobal();
    }
    const auto G = c.runGlobal();
    EXPECT_EQ(G.s.n_prior, 8);
    EXPECT_EQ(G.s.n_data, 4);
    const auto& F0 = c.familyGlobalRun(0);
    const auto& F1 = c.familyGlobalRun(1);
    EXPECT_EQ(F0.n_prior + F1.n_prior, G.s.n_prior) << "the split must close";
    EXPECT_NEAR(F0.we2 + F1.we2, G.s.we2, 1e-15 * G.s.we2);
    EXPECT_NEAR(F0.r_prior + F1.r_prior, G.s.r_prior, 1e-12);
    EXPECT_DOUBLE_EQ(F0.wv2, 0.0) << "the data side is global, never per-family";
    EXPECT_DOUBLE_EQ(F1.wv2, 0.0);
    // The run sums include the undrained epoch (its rows are real), while the
    // per-epoch history covers only the drained ones.  The two populations differ
    // by design, and the run-end line states this.
    EXPECT_EQ(c.globalEpochs(), 3);
    EXPECT_EQ(c.globalSampleCount(), 3u);
    EXPECT_EQ(c.globalSamplesDropped(), 0);
    EXPECT_GT(c.globalQuantile(0.50), 0.0);
    EXPECT_LE(c.globalQuantile(0.05), c.globalQuantile(0.95));
    EXPECT_LE(c.tensionQuantile(0.05), c.tensionQuantile(0.95));
    // The per-epoch global sums were reset between epochs.
    const auto ge = c.epochGlobal();
    EXPECT_EQ(ge.s.n_prior, 2);
    EXPECT_EQ(ge.s.n_data, 1);
}

// ---------------------------------------------------------------------------
// 19. The consensus rule, and the roster dependence it removes.
// ---------------------------------------------------------------------------
// v1 resolved a multi-member corroboration domain with an OR ("did any member
// step?").  The OR saturates as the domain grows, because P(any of N steps) =
// 1-(1-p)^N -> 1.  On identical data, going from 63 to 303 stations moved the
// raw_stepped share of `cb` rows from 29 % to 70 % and the b-cell (the INC-3
// headline number) from 57 % to 21 %.  A statistic that depends on how many
// receivers are in the roster does not measure the satellite.
//
// The replacement is a vote, not a threshold, and it follows the physics: the
// discontinuities these priors can suffer are common-mode over their domain by
// construction (a satellite bias jump enters MW/GF at every station tracking it;
// a receiver reset moves every satellite at that station).
TEST(PriorResidualCensusTest, ConsensusRuleReplacesTheRosterDependentOr) {
    using E = PriorResidualEvidence;
    // Nothing measurable ⇒ NoEvidence, whatever the domain size.
    EXPECT_EQ(priorResidualConsensus(E{0, 0}), PriorResidualCorrob::NoEvidence);
    // A one-member domain is the `pl` case and is unchanged from v1: the
    // consensus reduces to the member's own verdict.
    EXPECT_EQ(priorResidualConsensus(E{1, 0}), PriorResidualCorrob::Flat);
    EXPECT_EQ(priorResidualConsensus(E{1, 1}), PriorResidualCorrob::Stepped);
    for (int stepped = 0; stepped <= 1; ++stepped)
        EXPECT_EQ(priorResidualConsensus(E{1, stepped}),
                  priorResidualOrVerdict(E{1, stepped}))
            << "the consensus must be a NO-OP on a one-member domain";
    // Multi-member: strict majority.  A tie is not a consensus.
    EXPECT_EQ(priorResidualConsensus(E{2, 1}), PriorResidualCorrob::Flat);
    EXPECT_EQ(priorResidualConsensus(E{2, 2}), PriorResidualCorrob::Stepped);
    EXPECT_EQ(priorResidualConsensus(E{4, 2}), PriorResidualCorrob::Flat);
    EXPECT_EQ(priorResidualConsensus(E{4, 3}), PriorResidualCorrob::Stepped);
    // The main property: with the per-arc step rate fixed at 20 % and the domain
    // grown from 10 to 1000, the OR saturates to Stepped and the consensus stays
    // Flat.  This reproduces the 63 -> 303 station collapse described above.
    for (int n : {10, 30, 63, 150, 303, 1000}) {
        const E ev{n, n / 5};                       // 20 % of members stepped
        EXPECT_EQ(priorResidualOrVerdict(ev), PriorResidualCorrob::Stepped)
            << "the OR saturates at domain " << n;
        EXPECT_EQ(priorResidualConsensus(ev), PriorResidualCorrob::Flat)
            << "the consensus must be roster-INDEPENDENT at domain " << n;
    }
    // A genuine common-mode event still reads Stepped at every domain size.
    for (int n : {2, 10, 303}) {
        const E ev{n, n};                           // every member stepped
        EXPECT_EQ(priorResidualConsensus(ev), PriorResidualCorrob::Stepped)
            << "a satellite-side / receiver-side event is common-mode by "
               "construction and must survive the vote at domain " << n;
    }
}

// ---------------------------------------------------------------------------
// 20. The corroboration audit columns close, and the rule can only raise the b-cell.
// ---------------------------------------------------------------------------
// Relative to the OR, the consensus can only move rows Stepped -> Flat, so it can
// only raise the b-cell, which is the number the census reports as its finding.
// Reporting that without showing how much the rule moved would be circular, hence
// the three audit columns (n_corrob_rows, n_corrob_or_stepped, n_corrob_stepped).
// This case pins the arithmetic those columns satisfy.
TEST(PriorResidualCensusTest, CorroborationAuditColumnsCloseAndBoundTheBCell) {
    NetworkPriorResidual c(std::vector<std::string>{"pl"}, /*window*/ 0.0,
                           /*min_rows=*/1e18, /*report_sigma=*/4.0,
                           /*corroborate=*/true);
    // A saturating domain: 100 members, 20 stepped.  OR says Stepped, consensus
    // says Flat, on every row.
    c.setCorroborator([](std::string_view, int, std::string_view) {
        return PriorResidualEvidence{100, 20};
    });
    c.beginEpoch(0.0, 0.0);
    const int kHi = 9, kLo = 11;
    for (int i = 0; i < kHi; ++i) c.addRow(row(10.0, 2.0, 1.0));   // above 4-sigma
    for (int i = 0; i < kLo; ++i) c.addRow(row(0.5, 2.0, 1.0));    // below
    const auto& L = c.epochLadder(0, PriorResidualLabel::Clean);

    EXPECT_EQ(L.n_corrob_rows, kHi + kLo);
    EXPECT_DOUBLE_EQ(L.meanCorrobDomain(), 100.0);
    EXPECT_EQ(L.n_corrob_or_stepped, kHi + kLo) << "v1's OR would have saturated";
    EXPECT_EQ(L.n_corrob_stepped, 0)            << "the consensus refuses a 20 % minority";
    // Every flagged row therefore lands in the b-cell rather than the a-cell, and
    // the audit columns above show that the rule, not the data, put them there.
    EXPECT_EQ(L.contingency[0][static_cast<int>(PriorResidualCorrob::Flat)], kHi);
    EXPECT_EQ(L.contingency[0][static_cast<int>(PriorResidualCorrob::Stepped)], 0);
    EXPECT_EQ(L.contingency[1][static_cast<int>(PriorResidualCorrob::Flat)], kLo);
    // The consensus count can never exceed the OR count: the rule is one-way.
    EXPECT_LE(L.n_corrob_stepped, L.n_corrob_or_stepped);

    // A one-member domain leaves both counts equal (the no-op case).
    NetworkPriorResidual solo(std::vector<std::string>{"pl"}, 0.0, 1e18, 4.0, true);
    solo.setCorroborator([](std::string_view, int, std::string_view) {
        return PriorResidualEvidence{1, 1};
    });
    solo.beginEpoch(0.0, 0.0);
    solo.addRow(row(10.0, 2.0, 1.0));
    const auto& S = solo.epochLadder(0, PriorResidualLabel::Clean);
    EXPECT_DOUBLE_EQ(S.meanCorrobDomain(), 1.0);
    EXPECT_EQ(S.n_corrob_or_stepped, S.n_corrob_stepped);
    EXPECT_EQ(S.contingency[0][static_cast<int>(PriorResidualCorrob::Stepped)], 1);
}

// ---------------------------------------------------------------------------
// 21. The sensitivity column, which makes a null consensus count falsifiable.
// ---------------------------------------------------------------------------
// "The consensus fired 0 times" reads the same whether no common-mode event
// occurred or the rule could never have seen one.  max_corrob_frac separates the
// two readings: it is the highest stepped fraction any domain reached, against
// the rule's strict-majority bar of 0.5.  When a family stays far below the bar
// (as cb did on the FT63 smoke), its zero consensus count says nothing about the
// rule's power, and the run-end line states that instead of presenting the zero
// as a result.
TEST(PriorResidualCensusTest, MaxCorroborationFractionMakesTheNullFalsifiable) {
    auto runWith = [](int n_measured, int n_stepped) {
        NetworkPriorResidual c(std::vector<std::string>{"pl"}, 0.0, 1e18, 4.0, true);
        c.setCorroborator([n_measured, n_stepped](std::string_view, int, std::string_view) {
            return PriorResidualEvidence{n_measured, n_stepped};
        });
        c.beginEpoch(0.0, 0.0);
        c.addRow(row(10.0, 2.0, 1.0));
        return c.runLadder(0, PriorResidualLabel::Clean).max_corrob_frac;
    };
    EXPECT_NEAR(runWith(100, 2),  0.02, 1e-12) << "far below the bar ⇒ null is uninformative";
    EXPECT_NEAR(runWith(100, 20), 0.20, 1e-12);
    EXPECT_NEAR(runWith(4, 2),    0.50, 1e-12) << "a tie touches the bar without clearing it";
    EXPECT_NEAR(runWith(4, 3),    0.75, 1e-12);
    EXPECT_NEAR(runWith(0, 0),    0.00, 1e-12) << "no evidence contributes no fraction";

    // It is a maximum over rows, not a mean: one high-fraction domain among many
    // low ones still sets the value, so the sensitivity claim is not diluted.
    NetworkPriorResidual c(std::vector<std::string>{"pl"}, 0.0, 1e18, 4.0, true);
    int call = 0;
    c.setCorroborator([&call](std::string_view, int, std::string_view) {
        return (++call == 50) ? PriorResidualEvidence{10, 9}
                              : PriorResidualEvidence{10, 0};
    });
    c.beginEpoch(0.0, 0.0);
    for (int i = 0; i < 200; ++i) c.addRow(row(10.0, 2.0, 1.0));
    const auto& L = c.runLadder(0, PriorResidualLabel::Clean);
    EXPECT_NEAR(L.max_corrob_frac, 0.9, 1e-12);
    EXPECT_EQ(L.n_corrob_stepped, 1) << "and exactly that one row clears the majority";
    EXPECT_DOUBLE_EQ(L.meanCorrobDomain(), 10.0);
}

// ---------------------------------------------------------------------------
// 22. The rneg re-partition: a row whose posterior exceeds its prior is not a
//     standardized residual and does not enter the label ladder.
// ---------------------------------------------------------------------------
// post_var > fold_var makes r < 0 before the numerical floor: the documented
// coupling-carry case, where the harvested posterior is a conditional variance
// that can legitimately exceed the prior.  Its D{ê} is then not the dispersion
// of one fold, so `r` is clamped to kRedundancyFloor and |t| inflates by
// sqrt((p+v)/(eps*p)): the same artifact the file header names for the
// wrong-branch case (case 10), reached by a different path.  Left in the label
// ladder, such rows made up almost all rows above |t| = 1e4 in a field run and
// set the run-end worst-|t| line.
//
// The sigma0_global rider drops them (kGlobalRnegDrop).  This is the ladder side
// of the same decision: they get their own class, keyed by the same label, and
// touch nothing in the Clean/Responded/Frame bins.
TEST(PriorResidualCensusTest, RNegativeRowsAreClassedApartAndMoveNoLadderStatistic) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);   // raw-formal: t == s
    c.beginEpoch(0.0, 0.0);

    // An ordinary population first.
    const int kClean = 20;
    for (int i = 0; i < kClean; ++i) c.addRow(row(0.05 * (i + 1), 2.0, 1.0));
    const auto snapshot = c.epochLadder(0, PriorResidualLabel::Clean);
    ASSERT_EQ(snapshot.n_rows, kClean);
    ASSERT_GT(snapshot.max_abs_t, 0.0);
    const double t_before   = snapshot.max_abs_t;
    const double geo_before = snapshot.geoT2();
    std::array<long long, NetworkPriorResidual::kLadderBins> bins_before{};
    for (int k = 0; k < NetworkPriorResidual::kLadderBins; ++k)
        bins_before[static_cast<std::size_t>(k)] = snapshot.bins[static_cast<std::size_t>(k)];

    // Then the r-negative row: a stiff 1e-5 m^2 prior whose posterior came back at
    // 1.0 m^2 (post_var > fold_var), carrying a 3 m residual.  Pooled into the
    // label ladder, this row alone would set the top bin and the worst-|t| line.
    const auto st = c.addRow(row(3.0, 1e-5, 1.0));
    ASSERT_TRUE(st.accepted);
    ASSERT_TRUE(st.r_negative) << "the fixture must actually build an r<0 row";
    ASSERT_TRUE(st.r_clamped)  << "r<0 is below the floor by construction";
    EXPECT_GT(std::abs(st.t), 1e3) << "and its |t| really is the artifact scale";

    // --- the label ladder is untouched, in every statistic --------------------
    const auto& L = c.epochLadder(0, PriorResidualLabel::Clean);
    EXPECT_EQ(L.n_rows, kClean)          << "the rneg row must not be counted here";
    EXPECT_DOUBLE_EQ(L.max_abs_t, t_before);
    EXPECT_DOUBLE_EQ(L.geoT2(), geo_before) << "sum_log_t2 must not have moved";
    for (int k = 0; k < NetworkPriorResidual::kLadderBins; ++k)
        EXPECT_EQ(L.bins[static_cast<std::size_t>(k)],
                  bins_before[static_cast<std::size_t>(k)]) << "bin " << k;
    // The legacy column is retained for reader compatibility and now reads 0:
    // the population lives in the rneg class, not "of those" rows.
    EXPECT_EQ(L.n_r_negative, 0);

    // --- the rneg class carries it, keyed on |e| in metres --------------------
    const auto& N = c.rnegEpochLadder(0, PriorResidualLabel::Clean);
    EXPECT_EQ(N.n_rows, 1);
    EXPECT_TRUE(N.keepsMaxByE) << "the class orders its identity by |e|, not |t|";
    EXPECT_DOUBLE_EQ(N.max_abs_e, 3.0);
    EXPECT_DOUBLE_EQ(N.max_e, 3.0);
    EXPECT_DOUBLE_EQ(N.max_fold_var, 1e-5);
    EXPECT_DOUBLE_EQ(N.max_post_var, 1.0);
    EXPECT_DOUBLE_EQ(N.max_gap_s, 30.0);
    EXPECT_EQ(N.max_sat, "G01");
    EXPECT_EQ(N.max_station, 0);
    // Its |t| is still recorded (the trace column shows the artifact-scale value)
    // but is not the ordering key.
    EXPECT_NEAR(N.max_abs_t, std::abs(st.t), 1e-6);

    // --- the epoch-status scalars --------------------------------------------
    // ep_rows_ stays the total so the row count still closes; the |t| scalar
    // that feeds prires_max_t and the run-end line excludes the class.
    EXPECT_EQ(c.epochRows(), kClean + 1);
    EXPECT_EQ(c.epochRowsRneg(), 1);
    EXPECT_DOUBLE_EQ(c.epochMaxAbsT(), t_before);
    EXPECT_DOUBLE_EQ(c.epochRnegMaxAbsE(), 3.0);
    // A family whose rows were all rneg still ran; runRows counts them, so the
    // run-end readout does not report the family as unarmed.
    EXPECT_EQ(c.runRows(0), kClean + 1);
    EXPECT_EQ(c.rnegRunRows(0), 1);
}

// The class is ordered by |e|, not |t|, which keeps it readable: the floor
// inflates |t| more on a stiffer prior, so a small residual on a stiff prior can
// outrank a large residual on a loose prior in |t|.  A second rneg row with a
// larger |e| but a smaller |t| takes the identity.
TEST(PriorResidualCensusTest, RNegClassIdentityFollowsTheLargestResidualNotTheLargestT) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);
    c.beginEpoch(0.0, 0.0);

    // Row A: small |e|, very stiff prior ⇒ enormous |t|.
    auto a = row(0.5, 1e-8, 1.0);
    a.sat = "GAA";
    const auto sa = c.addRow(a);
    ASSERT_TRUE(sa.r_negative);

    // Row B: much larger |e|, looser prior ⇒ smaller |t| than A.
    auto b = row(9.0, 1e-2, 1.0);
    b.sat = "GBB";
    const auto sb = c.addRow(b);
    ASSERT_TRUE(sb.r_negative);
    ASSERT_LT(std::abs(sb.t), std::abs(sa.t)) << "the fixture must invert the two orders";

    const auto& N = c.rnegEpochLadder(0, PriorResidualLabel::Clean);
    EXPECT_EQ(N.n_rows, 2);
    EXPECT_DOUBLE_EQ(N.max_abs_e, 9.0);
    EXPECT_EQ(N.max_sat, "GBB") << "the identity follows |e|, so B wins";
    EXPECT_NEAR(N.max_abs_t, std::abs(sb.t), 1e-6) << "and its |t| is B's, not A's";

    // The epoch scalar is the same extremum.
    EXPECT_DOUBLE_EQ(c.epochRnegMaxAbsE(), 9.0);
    // Nothing reached the label ladder at all.
    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Clean).n_rows, 0);
    EXPECT_DOUBLE_EQ(c.epochMaxAbsT(), 0.0);
}

// Negative control for the re-partition: an unfolded row takes the `+` branch
// (D = fold_var + post_var), where post_var > fold_var is ordinary and r is not
// formed.  Such a row stays in its label ladder; diverting it would empty the
// Responded population, which is the positive control the census is graded on
// (spec §9.3).
TEST(PriorResidualCensusTest, UnfoldedRowsWithLargePosteriorsStayInTheirLabelLadder) {
    NetworkPriorResidual c = makeCensus(/*min_rows=*/1e18);
    c.beginEpoch(0.0, 0.0);
    // Same variances as the r-negative row above, but unfolded (a refloated prior
    // that did not enter this epoch's solve).
    const auto st = c.addRow(row(3.0, 1e-5, 1.0, /*folded=*/false,
                                 PriorResidualLabel::Responded));
    ASSERT_TRUE(st.accepted);
    EXPECT_FALSE(st.r_negative) << "r is not formed on the unfolded branch";
    EXPECT_NEAR(st.disp, 1e-5 + 1.0, 1e-15);

    EXPECT_EQ(c.epochLadder(0, PriorResidualLabel::Responded).n_rows, 1);
    EXPECT_EQ(c.rnegEpochLadder(0, PriorResidualLabel::Responded).n_rows, 0);
    EXPECT_EQ(c.epochRowsRneg(), 0);
    EXPECT_GT(c.epochMaxAbsT(), 0.0);
}
