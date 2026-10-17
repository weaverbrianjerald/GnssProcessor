/**
 * @file test_fix_and_hold.cpp
 * @brief PPP fix-and-hold (@c ar_mode 3): @c AmbiguityResolver::holdAmbiguities
 *        has to hold the ambiguities the accepted fix constrained, and only those.
 *
 * The PPP callers (Application::resolveEpochAmbiguities_ and the real-time
 * estimate seam) install @c AmbiguityResolution::fixed_state and
 * @c fixed_covariance into the filter and then call @c holdAmbiguities with the
 * same resolution.  The hold used to rebuild the double-difference pairs from the
 * state alone, with no observations.  A satellite without an observation has
 * elevation 0, and the 20 degree @c elevation_mask_ar then removed every pair, so
 * the call returned without a single pseudo-observation.
 *
 * The first group drives the production chain (resolve, install, hold) on a
 * synthetic float state with the default mask and pins that the hold changes the
 * covariance of every fixed pair and of nothing else.  The second group calls the
 * hold on hand-built resolutions to pin the gates and the row selection.
 *
 * Suite name @c PppFixAndHoldTest is owned solely by this file.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// PPP core size used by Application::run (position x3, clock, troposphere).
constexpr size_t kPppCoreSize = 5;

/// Converged float: position variance per axis (3D sigma 0.035 m, inside the
/// 0.05 m AR sigma gate) and ambiguity variance (4 mm)^2.
constexpr double kPosVar = 4.0e-4;
constexpr double kAmbVar = 1.6e-5;

/// One synthetic GPS satellite on L1C.
struct SatSpec {
    const char* name;
    double      elevation_deg;
    int         integer;        ///< Known DD integer (cycles) against the pivot.
    double      frac_cycles;    ///< Float error (cycles) on top of the integer.
};

/// The pivot (highest elevation) is G01.  G05 sits below the default 20 degree AR
/// mask, so it must neither enter the fix nor be held.
const SatSpec kSats[] = {
    { "G01", 80.0,  0,  0.000 },
    { "G02", 45.0,  3,  0.010 },
    { "G03", 30.0, -2, -0.020 },
    { "G04", 60.0,  5,  0.015 },
    { "G05", 15.0,  7,  0.030 },
};

/// Variance of the double difference of two states.
double ddVariance(const Eigen::MatrixXd& P, int i, int ref) {
    return P(i, i) + P(ref, ref) - 2.0 * P(i, ref);
}

class PppFixAndHoldTest : public ::testing::Test {
protected:
    LogManager   log_;
    GnssConfig   config_;
    PppUtils     utils_{ log_ };
    StateManager sm_{ kPppCoreSize, log_, config_ };
    std::vector<PppObservation> obs_;

    /// Seed the five satellites.  @p g04_frac_cycles and @p g04_var_m2 replace G04's
    /// float error and variance, so a test can make that one pair unfixable.
    void build(double g04_frac_cycles, double g04_var_m2 = kAmbVar) {
        // A common fractional offset on every satellite of the signal: the
        // per-satellite floats are not integer, only their differences are.
        const double common_offset_cycles = 0.37;
        for (const auto& s : kSats) {
            PppObservation o{};
            o.satellite = s.name;
            o.system    = System::GPS;
            o.time      = GnssTime{ 2313, 259200.0 };
            o.elevation = s.elevation_deg;
            obs_.push_back(o);

            const double frac = (std::string(s.name) == "G04") ? g04_frac_cycles : s.frac_cycles;
            const double lam = utils_.getWavelength(System::GPS, "L1C", s.name);
            ASSERT_GT(lam, 0.0);
            sm_.addAmbiguity(std::string(s.name) + "_L1C",
                             lam * (common_offset_cycles + 100.0 + s.integer + frac));
        }
        const std::size_t n = sm_.getStateSize();
        Eigen::MatrixXd P = Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(n),
                                                      static_cast<Eigen::Index>(n));
        for (int d = 0; d < 3; ++d) P(d, d) = kPosVar;
        for (const auto& kv : sm_.getAmbiguities())
            P(static_cast<Eigen::Index>(kv.second), static_cast<Eigen::Index>(kv.second)) = kAmbVar;
        P(sm_.getAmbiguityIndex("G04_L1C"), sm_.getAmbiguityIndex("G04_L1C")) = g04_var_m2;
        sm_.setCovariance(P);
    }

    /// Production defaults for everything the fix gates read, except the two
    /// convergence guards of a single-shot call and the mode.
    AmbiguityResolverConfig arConfig(int ar_mode) const {
        AmbiguityResolverConfig cfg;
        cfg.ar_mode           = ar_mode;
        cfg.min_epochs        = 0;
        cfg.ratio_upper_bound = 1e12;   // the ratio gate is not under test
        return cfg;
    }

    int idx(const char* sat) const { return sm_.getAmbiguityIndex(std::string(sat) + "_L1C"); }

    /// What the PPP callers do with a successful fix, before the hold.
    void installFix(const AmbiguityResolution& r) {
        sm_.setStateVector(r.fixed_state);
        sm_.setCovariance(r.fixed_covariance);
    }

    /// A successful resolution whose fixed state is the current state with
    /// @p shift_m added to the ambiguity at @p shifted_idx, constraining @p rows.
    AmbiguityResolution handBuilt(const std::vector<AmbiguityResolution::FixedDdRow>& rows,
                                  int shifted_idx = -1, double shift_m = 0.0) const {
        AmbiguityResolution r;
        r.success         = true;
        r.fixed_state     = sm_.getStateVector();
        r.fixed_covariance = sm_.getCovariance();
        if (shifted_idx >= 0) r.fixed_state(shifted_idx) += shift_m;
        r.fixed_dd_rows   = rows;
        return r;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The production chain, default mask
// ---------------------------------------------------------------------------

// The default mask is 20 degrees: the hold has to reach the fixed pairs under it.
TEST_F(PppFixAndHoldTest, HoldTightensEveryFixedPairAndLeavesTheMaskedSatelliteAlone) {
    build(0.015);
    AmbiguityResolver resolver(arConfig(3), log_);
    ASSERT_DOUBLE_EQ(resolver.getConfig().elevation_mask_ar, 20.0);

    const AmbiguityResolution res = resolver.resolve(sm_, obs_);
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.num_fixed, 3) << "G02, G03, G04 against G01; G05 is below the mask";
    ASSERT_EQ(res.fixed_dd_rows.size(), 3u);
    installFix(res);

    const Eigen::MatrixXd P_fix = sm_.getCovariance();
    const Eigen::VectorXd x_fix = sm_.getStateVector();

    resolver.holdAmbiguities(sm_, res);

    const Eigen::MatrixXd P_held = sm_.getCovariance();
    const Eigen::VectorXd x_held = sm_.getStateVector();

    // Each fixed pair carries a second pseudo-observation, so its DD variance falls.
    const int ref = idx("G01");
    for (const char* sat : { "G02", "G03", "G04" }) {
        EXPECT_LT(ddVariance(P_held, idx(sat), ref), ddVariance(P_fix, idx(sat), ref) * 0.999)
            << sat << " was not held";
    }
    // The state already sits on the fix, so the hold moves no mean.
    EXPECT_LT((x_held - x_fix).cwiseAbs().maxCoeff(), 1e-9);

    // The masked satellite is no candidate: its variance and its covariance with
    // every other state are bit-identical across the hold.
    const int masked = idx("G05");
    for (Eigen::Index k = 0; k < P_fix.rows(); ++k) {
        EXPECT_EQ(P_held(masked, k), P_fix(masked, k)) << "column " << k;
    }
}

// A partial fix constrains a subset: the candidate it left out stays float.  Pinning
// the left-out pair at the hold variance would freeze an unconverged ambiguity.
TEST_F(PppFixAndHoldTest, PartialFixHoldsOnlyTheKeptSubset) {
    // G04 is loose (sigma 4 cm, 0.2 cycle) and 0.3 cycle off: the full set fails the
    // bootstrap gate and the subset search drops the least precise ambiguity.
    build(0.30, 1.6e-3);
    AmbiguityResolverConfig cfg = arConfig(3);
    cfg.enable_partial_ar = true;
    AmbiguityResolver resolver(cfg, log_);

    const AmbiguityResolution res = resolver.resolve(sm_, obs_);
    ASSERT_TRUE(res.success);
    ASSERT_EQ(res.num_fixed, 2) << "G02 and G03; G04 is dropped by the subset search";
    ASSERT_EQ(res.fixed_dd_rows.size(), 2u);
    installFix(res);

    const Eigen::MatrixXd P_fix = sm_.getCovariance();
    resolver.holdAmbiguities(sm_, res);
    const Eigen::MatrixXd P_held = sm_.getCovariance();

    const int ref = idx("G01");
    for (const char* sat : { "G02", "G03" }) {
        EXPECT_LT(ddVariance(P_held, idx(sat), ref), ddVariance(P_fix, idx(sat), ref) * 0.999)
            << sat << " was not held";
    }
    const int left_out = idx("G04");
    EXPECT_EQ(P_held(left_out, left_out), P_fix(left_out, left_out));
}

// ---------------------------------------------------------------------------
// The gates and the row selection, on hand-built resolutions
// ---------------------------------------------------------------------------

// A hold of one row pulls that DD onto the fixed value and touches no other state.
TEST_F(PppFixAndHoldTest, HeldRowMovesTowardTheFixedValueAndNoOtherAmbiguity) {
    build(0.015);
    AmbiguityResolver resolver(arConfig(3), log_);
    const int g01 = idx("G01"), g02 = idx("G02"), g03 = idx("G03");
    const double shift_m = 0.02;   // the fixed DD sits 2 cm above the float DD
    const AmbiguityResolution res = handBuilt({ { g02, g01 } }, g02, shift_m);

    const Eigen::VectorXd x0 = sm_.getStateVector();
    const Eigen::MatrixXd P0 = sm_.getCovariance();
    resolver.holdAmbiguities(sm_, res);
    const Eigen::VectorXd x1 = sm_.getStateVector();
    const Eigen::MatrixXd P1 = sm_.getCovariance();

    // A pseudo-observation of variance R on a DD of prior variance V moves the DD
    // by V / (V + R) of the innovation.
    const double v_dd = ddVariance(P0, g02, g01);
    const double gain = v_dd / (v_dd + resolver.getConfig().hold_variance);
    const double moved = (x1(g02) - x1(g01)) - (x0(g02) - x0(g01));
    EXPECT_NEAR(moved, gain * shift_m, 1e-12);
    EXPECT_NEAR(ddVariance(P1, g02, g01), v_dd * (1.0 - gain), 1e-12);

    // A satellite outside the row keeps its mean and its covariance.
    EXPECT_EQ(x1(g03), x0(g03));
    EXPECT_EQ(P1(g03, g03), P0(g03, g03));
    EXPECT_EQ(P1(g03, g01), P0(g03, g01));
}

// A fix that constrained no single-signal DD (the WL-only path) holds nothing.
TEST_F(PppFixAndHoldTest, EmptyRowsHoldNothing) {
    build(0.015);
    AmbiguityResolver resolver(arConfig(3), log_);
    const AmbiguityResolution res = handBuilt({}, idx("G02"), 0.02);

    const Eigen::VectorXd x0 = sm_.getStateVector();
    const Eigen::MatrixXd P0 = sm_.getCovariance();
    resolver.holdAmbiguities(sm_, res);
    EXPECT_EQ(sm_.getStateVector(), x0);
    EXPECT_EQ(sm_.getCovariance(), P0);
}

// The hold is the fix-and-hold mode: every other AR mode leaves the filter alone.
TEST_F(PppFixAndHoldTest, OtherArModesHoldNothing) {
    build(0.015);
    const Eigen::VectorXd x0 = sm_.getStateVector();
    const Eigen::MatrixXd P0 = sm_.getCovariance();
    for (const int mode : { 0, 1, 2 }) {
        AmbiguityResolver resolver(arConfig(mode), log_);
        const AmbiguityResolution res = handBuilt({ { idx("G02"), idx("G01") } }, idx("G02"), 0.02);
        resolver.holdAmbiguities(sm_, res);
        EXPECT_EQ(sm_.getStateVector(), x0) << "ar_mode " << mode;
        EXPECT_EQ(sm_.getCovariance(), P0) << "ar_mode " << mode;
    }
}

// A resolution that did not succeed carries no fix to hold.
TEST_F(PppFixAndHoldTest, FailedResolutionHoldsNothing) {
    build(0.015);
    AmbiguityResolver resolver(arConfig(3), log_);
    AmbiguityResolution res = handBuilt({ { idx("G02"), idx("G01") } }, idx("G02"), 0.02);
    res.success = false;

    const Eigen::VectorXd x0 = sm_.getStateVector();
    const Eigen::MatrixXd P0 = sm_.getCovariance();
    resolver.holdAmbiguities(sm_, res);
    EXPECT_EQ(sm_.getStateVector(), x0);
    EXPECT_EQ(sm_.getCovariance(), P0);
}

// A row naming a state outside the filter or outside the fixed state is skipped, and
// the valid rows beside it are still held.
TEST_F(PppFixAndHoldTest, OutOfRangeRowsAreSkipped) {
    build(0.015);
    AmbiguityResolver resolver(arConfig(3), log_);
    const int g01 = idx("G01"), g02 = idx("G02");
    const int n = static_cast<int>(sm_.getStateSize());
    const AmbiguityResolution res =
        handBuilt({ { n + 3, g01 }, { g02, -1 }, { g02, g01 } }, g02, 0.02);

    const Eigen::MatrixXd P0 = sm_.getCovariance();
    resolver.holdAmbiguities(sm_, res);
    EXPECT_LT(ddVariance(sm_.getCovariance(), g02, g01), ddVariance(P0, g02, g01));
}
