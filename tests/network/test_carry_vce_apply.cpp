// Unit tests for NetworkCarryVceApply (M2 v2, docs/SPEC_carry_vce_apply.md): the
// LASER variance-factor iteration (NGS LASER Guide v2.7 slides 5-93..5-95; Koch 1999
// sections 3.6.1-3.6.2) driven over the M2 monitor.
//
// Each gate drives the two standalone classes directly with synthetic carry rows of
// known truth (as test_network_carry_vce and test_neq_accumulator do); there is no
// ApplicationNetwork coupling.  The closed loop is reproduced: each epoch folds at
// fold_var = hand_floor * alpha2, feeds the monitor both that used value and the hand
// value, then steps the factor once.  This is the producer's ordering
// (SB_STASH_STATUS, after every feed, before the next fold).
//
// The rows follow the scalar carry model the iteration is derived for (since
// 2026-10-07; the earlier feed drew the residual with a variance that ignored the
// factor and fixed the redundancy at 0.5, which is why it could not see the missing
// redundancy division): a prior of true variance a*p folded at f = alpha2*p against
// data of variance d gives posterior v = f d/(f + d), redundancy r = f/(f + d) and a
// residual of variance r^2 (a p + d).  The factor's fixed point is then alpha2 = a.
//
//   Gate 1 (convergence)   : truth 25x the hand floor gives alpha2 -> 25 and the
//                            re-standardized sigma_hat_post -> 1 (LASER 5-95).
//   Gate 2 (inflate-only)  : truth 0.1x the floor keeps alpha2 at exactly 1.
//   Gate 3 (clamps)        : slew, ceiling, freeze-on-dirty and warm-up each
//                            engage and release, each with an edge.
//   Gate 4 (isolation)     : applying 'pl' leaves pb/cb and other constellations
//                            at exactly 1.0 (bit-identical fold variance).
//   Gate 5 (anti-ratchet)  : an event era inflates alpha2 and the window's
//                            forgetting brings it back down; the return is
//                            checked against the closed form.
//   Gate 6 (off identity)  : unconfigured and empty-family instances are unity.
//   Gate 7 (monitor delta) : the hand_fold_var accumulator added to
//                            NetworkCarryVce is a geometric mean and defaults to
//                            the fold value, so every pre-v2 call site is unchanged;
//                            the redundancy-corrected channel reads ê²/r.
//   Gate 8 (stiff prior)   : data 10x weaker than the hand floor, truth 25x: the
//                            factor still reaches 25, where the uncorrected target
//                            had no fixed point above 1.
//   Gate 9 (no redundancy) : rows with r <= floor or r < 0 are left out of the target
//                            and of the warm-up count, and are counted.

#include "network/carry/NetworkCarryVce.hpp"
#include "network/carry/NetworkCarryVceApply.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr double kHandFloor = 4.0e-5;   // the FT63 phase-leveling floor (m^2)
constexpr int    kRowsPerEpoch = 60;    // one epoch's clean carry rows for one group
constexpr double kWindow = 60.0;        // monitor half-life (epochs); the shipped default
constexpr double kDataVar = kHandFloor; // default data variance: as informative as the floor

/// One epoch of the producer's closed loop for a single (family, constellation),
/// on the scalar carry model.
///
/// `prior_var` is the true variance of the carried prior (a times the hand floor).
/// The fold uses the hand variance scaled by the live factor, as the injectors do;
/// the epoch's data determine the state with variance `data_var`.  The residual
/// ê = prior − solved = r·(prior − data) then has variance r²·(prior_var + data_var)
/// with r = f/(f + d), and the posterior variance is v = f·d/(f + d).  Both the used
/// and the hand fold variance reach the monitor.
void feedEpoch(NetworkCarryVce& mon, const NetworkCarryVceApply& apply,
               const std::string& family, const std::string& sat,
               double prior_var, double hand_var, std::mt19937& gen,
               int n_rows = kRowsPerEpoch, double data_var = kDataVar) {
    const int slot = NetworkCarryVceApply::familySlot(family);
    const double fold_var = hand_var * apply.factor(slot, sat);
    const double r        = fold_var / (fold_var + data_var);
    const double post_var = fold_var * data_var / (fold_var + data_var);
    std::normal_distribution<double> nrm(0.0, r * std::sqrt(prior_var + data_var));
    for (int i = 0; i < n_rows; ++i)
        mon.addRow(family, sat, nrm(gen), /*gap_s=*/60.0, post_var, fold_var, hand_var);
}

NetworkCarryVce makeMonitor(std::vector<std::string> families) {
    return NetworkCarryVce(std::move(families), kWindow, /*split_by_const=*/true,
                           /*seed_floor=*/kHandFloor, /*seed_rate=*/0.0);
}

const NetworkCarryVceApply::Group& group(const NetworkCarryVceApply& a,
                                         const std::string& key) {
    const auto it = a.groups().find(key);
    EXPECT_NE(it, a.groups().end()) << "no group '" << key << "'";
    return it->second;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1: the factor iteration converges on a 25x-optimistic hand floor, and the
// LASER convergence readout sigma_hat_post reaches 1.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, FactorConvergesToTheMeasuredRatio) {
    std::mt19937 gen(20260804u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, /*slew=*/1.05, /*max=*/1000.0, /*min_rows=*/200);
    ASSERT_TRUE(apply.enabled());

    const double truth = 25.0 * kHandFloor;
    // ln(25)/ln(1.05) = 66 slew-limited epochs from unity, plus the monitor's own
    // window lag; 400 epochs leaves a wide margin without hiding a failure to move.
    for (int ep = 0; ep < 400; ++ep) {
        mon.beginEpoch(/*content_time_s=*/ep * 60.0, /*clean=*/true);
        feedEpoch(mon, apply, "pl", "G01", truth, kHandFloor, gen);
        apply.stepEpoch(mon, /*clean=*/true);
    }

    const auto& g = group(apply, "pl|G");
    EXPECT_NEAR(g.target, 25.0, 25.0 * 0.10);
    EXPECT_NEAR(g.alpha2, 25.0, 25.0 * 0.10);
    // The slide's convergence criterion: the variance of unit weight of the
    // group, under the weights the solve actually used, is 1.
    EXPECT_NEAR(g.sigma_hat_post, 1.0, 0.05);
    EXPECT_GT(g.n_steps, 300);
    // The factor moved through the limiter on the way up and was released at the
    // top; a run that is still slew-clipped at the end has not converged.
    EXPECT_GT(g.n_slew_up, 50);
    EXPECT_EQ(g.at_ceiling, false);
    EXPECT_EQ(g.at_unit, false);

    // The fold site sees the factor, and only for the armed family.
    EXPECT_NEAR(apply.factor(NetworkCarryVceApply::kPl, "G01"), g.alpha2, 1e-12);
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPb, "G01"), 1.0);
}

// ---------------------------------------------------------------------------
// Gate 2: inflate-only.  A hand floor that is already conservative is never
// tightened.  Deflation below it is the lock-in direction (standing invariant #1;
// the DOY-129 pivot-CB ratchet is an instance), and the clamp excludes it by
// construction.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, NeverDeflatesBelowTheHandFloor) {
    std::mt19937 gen(4242u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, 1000.0, 200);

    const double truth = 0.1 * kHandFloor;      // the hand floor is already 10x too loose
    // Data much stronger than the prior, so that at alpha2 = 1 the target,
    // (a p + d)/(p + d), reads close to a.
    const double data_var = 0.1 * kHandFloor;
    for (int ep = 0; ep < 300; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", truth, kHandFloor, gen, kRowsPerEpoch, data_var);
        apply.stepEpoch(mon, true);
        // Checked at every epoch, not only at the end: the factor is never below 1.
        ASSERT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPl, "G01"), 1.0);
    }
    const auto& g = group(apply, "pl|G");
    EXPECT_DOUBLE_EQ(g.alpha2, 1.0);
    EXPECT_DOUBLE_EQ(g.alpha2_max, 1.0);
    EXPECT_TRUE(g.at_unit);
    EXPECT_GT(g.n_unit_ep, 200);
    // The target itself is below 1, so the clamp refuses it; the measurement does
    // see the low variance.  (The target is about 0.18; the tolerance is wide because
    // the geometric mean is noisy at these row counts and the assertion is on the
    // direction, not the value.)
    EXPECT_LT(g.target, 0.5);
    EXPECT_LT(g.sigma_hat_post, 0.5);
}

// ---------------------------------------------------------------------------
// Gate 3a: the slew limiter binds on the way up and releases at the target.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, SlewLimiterBindsThenReleases) {
    std::mt19937 gen(7u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, /*slew=*/1.05, 1000.0, /*min_rows=*/200);

    const double truth = 100.0 * kHandFloor;
    std::vector<double> a2;
    for (int ep = 0; ep < 40; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", truth, kHandFloor, gen);
        apply.stepEpoch(mon, true);
        a2.push_back(apply.factor(NetworkCarryVceApply::kPl, "G01"));
    }
    // Warm-up (min_rows 200 at 60 rows/epoch) holds the first three epochs at 1.
    // The target is far above reach, so from index 6 on every step is exactly the
    // limiter ratio.
    EXPECT_DOUBLE_EQ(a2.front(), 1.0);
    for (std::size_t i = 6; i < a2.size(); ++i)
        EXPECT_NEAR(a2[i] / a2[i - 1], 1.05, 1e-9)
            << "step " << i << " was not slew-limited";
    EXPECT_LT(a2.back(), 100.0);   // 40 epochs cannot reach 100x at 5 %/epoch

    // Continue: once the factor reaches the target, the limiter stops clipping.
    const long long clipped_at_40 = group(apply, "pl|G").n_slew_up;
    for (int ep = 40; ep < 400; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", truth, kHandFloor, gen);
        apply.stepEpoch(mon, true);
    }
    const auto& g = group(apply, "pl|G");
    EXPECT_GT(g.n_slew_up, clipped_at_40);
    EXPECT_LT(g.n_slew_up, 200);   // released well before the run ended
    EXPECT_NEAR(g.alpha2, 100.0, 100.0 * 0.12);
}

// ---------------------------------------------------------------------------
// Gate 3b: the ceiling clamp; entry and exit are each an evented edge (invariant #7).
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, CeilingClampEngagesAndReleasesWithEdges) {
    std::mt19937 gen(99u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, /*max=*/5.0, /*min_rows=*/200);
    std::vector<NetworkCarryVceApply::Edge> edges;

    for (int ep = 0; ep < 200; ++ep) {          // truth far above the ceiling
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", 500.0 * kHandFloor, kHandFloor, gen);
        apply.stepEpoch(mon, true, &edges);
    }
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPl, "G01"), 5.0);
    EXPECT_TRUE(group(apply, "pl|G").at_ceiling);
    EXPECT_GT(group(apply, "pl|G").n_ceiling_ep, 100);

    int enter = 0, exit = 0;
    for (const auto& e : edges)
        if (std::string(e.what) == "ceiling") (e.entered ? enter : exit) += 1;
    EXPECT_EQ(enter, 1) << "the ceiling entry must be evented exactly once";
    EXPECT_EQ(exit, 0);

    // With the truth well below the ceiling, the factor comes back off it.
    for (int ep = 200; ep < 700; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", 2.0 * kHandFloor, kHandFloor, gen);
        apply.stepEpoch(mon, true, &edges);
    }
    EXPECT_FALSE(group(apply, "pl|G").at_ceiling);
    EXPECT_LT(apply.factor(NetworkCarryVceApply::kPl, "G01"), 5.0);
    exit = 0;
    for (const auto& e : edges)
        if (std::string(e.what) == "ceiling" && !e.entered) ++exit;
    EXPECT_EQ(exit, 1) << "the ceiling RELEASE is an edge too (standing invariant #7)";
}

// ---------------------------------------------------------------------------
// Gate 3c: freeze on a contaminated epoch and during warm-up, both flagged.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, FreezesOnDirtyEpochsAndDuringWarmup) {
    std::mt19937 gen(1234u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, 1000.0, /*min_rows=*/200);
    std::vector<NetworkCarryVceApply::Edge> edges;

    // --- warm-up: 60 rows/epoch, so epochs 1-3 are below 200 effective rows.
    for (int ep = 0; ep < 3; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", 25.0 * kHandFloor, kHandFloor, gen);
        apply.stepEpoch(mon, true, &edges);
        EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPl, "G01"), 1.0);
        EXPECT_EQ(group(apply, "pl|G").freeze, NetworkCarryVceApply::Freeze::kWarmup);
    }
    EXPECT_GE(group(apply, "pl|G").n_frozen, 3);
    EXPECT_EQ(group(apply, "pl|G").n_steps, 0);

    for (int ep = 3; ep < 20; ++ep) {           // out of warm-up, factor climbing
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", 25.0 * kHandFloor, kHandFloor, gen);
        apply.stepEpoch(mon, true, &edges);
    }
    ASSERT_EQ(group(apply, "pl|G").freeze, NetworkCarryVceApply::Freeze::kNone);
    const double held = apply.factor(NetworkCarryVceApply::kPl, "G01");
    ASSERT_GT(held, 1.0);

    // --- contaminated epoch: the monitor drops the rows and the factor holds.
    // A datum handover's re-levels are a frame step, not carry noise; a factor
    // that absorbed them would inflate on a datum change.
    mon.beginEpoch(20 * 60.0, /*clean=*/false);
    feedEpoch(mon, apply, "pl", "G01", 1.0e6 * kHandFloor, kHandFloor, gen);
    apply.stepEpoch(mon, /*clean=*/false, &edges);
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPl, "G01"), held);
    EXPECT_EQ(group(apply, "pl|G").freeze, NetworkCarryVceApply::Freeze::kEpochDirty);

    int dirty_enter = 0;
    for (const auto& e : edges)
        if (std::string(e.what) == "freeze" && e.entered
            && std::string(e.reason) == "epoch_dirty") ++dirty_enter;
    EXPECT_EQ(dirty_enter, 1);

    // A clean epoch releases the freeze (the other edge).
    mon.beginEpoch(21 * 60.0, true);
    feedEpoch(mon, apply, "pl", "G01", 25.0 * kHandFloor, kHandFloor, gen);
    apply.stepEpoch(mon, true, &edges);
    EXPECT_EQ(group(apply, "pl|G").freeze, NetworkCarryVceApply::Freeze::kNone);
    int release = 0;
    for (const auto& e : edges)
        if (std::string(e.what) == "freeze" && !e.entered
            && std::string(e.reason) == "epoch_dirty") ++release;
    EXPECT_EQ(release, 1);
}

// ---------------------------------------------------------------------------
// Gate 4: no cross-contamination.  An unarmed family, and an armed family's other
// constellation, return exactly 1.0, so the fold variance there is bit-identical
// to an apply-off run (spec section 6.4; the D5 isolation assert).
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, ScalesOnlyTheArmedFamilyAndConstellation) {
    std::mt19937 gen(555u);
    NetworkCarryVce mon = makeMonitor({"pl", "pb", "cb"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, 1000.0, 200);

    for (int ep = 0; ep < 300; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", 25.0 * kHandFloor, kHandFloor, gen);
        feedEpoch(mon, apply, "pl", "E11",  4.0 * kHandFloor, kHandFloor, gen);
        feedEpoch(mon, apply, "pb", "G01", 900.0 * kHandFloor, kHandFloor, gen);
        feedEpoch(mon, apply, "cb", "G01", 900.0 * kHandFloor, kHandFloor, gen);
        apply.stepEpoch(mon, true);
    }
    // pl is split per constellation and the two are independent estimates.
    EXPECT_NEAR(apply.factor(NetworkCarryVceApply::kPl, "G01"), 25.0, 3.0);
    EXPECT_NEAR(apply.factor(NetworkCarryVceApply::kPl, "E11"),  4.0, 0.6);
    // pb / cb are monitored but not applied: bit-identical unity.
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPb, "G01"), 1.0);
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kCb, "G01"), 1.0);
    EXPECT_EQ(apply.groups().count("pb|G"), 0u);
    EXPECT_EQ(apply.groups().count("cb|G"), 0u);
    // A constellation with no accumulator at all is unity, not a fallback to a
    // sibling's factor (no pooling, as in the D5 population).
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPl, "C22"), 1.0);
    EXPECT_DOUBLE_EQ(apply.factor(NetworkCarryVceApply::kPl, std::string()), 1.0);
}

// ---------------------------------------------------------------------------
// Gate 5: anti-ratchet round trip.  An event era inflates the factor, and the
// monitor window's forgetting brings it back down, so there is no lock-in.
//
// The return rate follows from the window arithmetic, not from a tuning choice.
// With phi = 0.5^(1/W) and an era of E epochs at a truth ratio R, k epochs after
// the era
//
//     target(k) = R ^ ( phi^k * (1 - phi^E) )
//
// because the accumulator is a forgetting-weighted mean of log(e^2) and the era's
// share of the total weight is phi^k(1-phi^E).  For R=25, E=50, W=60 that is 1.42
// at two windows and 1.09 at four, so the spec's "within 10 % in ~2 windows" gives
// the order of magnitude and about four windows is the closed-form figure.  The
// test asserts the closed form, so a change to the forgetting shows up here as a
// mismatch rather than as a retuned constant.  The rows carry data 100x stronger
// than the hand floor (r near 1), where the redundancy-corrected target reads the
// prior ratio directly, so the closed form is the window's alone.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, EventEraInflatesAndTheWindowGivesItBack) {
    std::mt19937 gen(31415u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, 1000.0, 200);

    int ep = 0;
    const auto run = [&](int n, double ratio) {
        for (int i = 0; i < n; ++i, ++ep) {
            mon.beginEpoch(ep * 60.0, true);
            feedEpoch(mon, apply, "pl", "G01", ratio * kHandFloor, kHandFloor, gen,
                      kRowsPerEpoch, /*data_var=*/0.01 * kHandFloor);
            apply.stepEpoch(mon, true);
        }
    };

    run(300, 1.0);                                   // settle at the hand floor
    const double pre = apply.factor(NetworkCarryVceApply::kPl, "G01");
    EXPECT_NEAR(pre, 1.0, 0.25);

    run(50, 25.0);                                   // the event era
    const double peak = apply.factor(NetworkCarryVceApply::kPl, "G01");
    EXPECT_GT(peak, 3.0) << "the factor did not respond to the era at all";

    run(120, 1.0);                                   // two windows of quiet
    const double two_win = apply.factor(NetworkCarryVceApply::kPl, "G01");
    EXPECT_LT(two_win, peak) << "the factor RATCHETED: it never came back down";

    run(130, 1.0);                                   // out to ~four windows
    const double four_win = apply.factor(NetworkCarryVceApply::kPl, "G01");
    EXPECT_LT(four_win, two_win);

    const double phi   = std::pow(0.5, 1.0 / kWindow);
    const double share = std::pow(phi, 250.0) * (1.0 - std::pow(phi, 50.0));
    const double closed_form = std::pow(25.0, share);          // ~1.08 at k = 250
    EXPECT_NEAR(four_win, closed_form, 0.25);
    EXPECT_LT(four_win, 1.10 * std::max(pre, 1.0) + 0.15);
    EXPECT_GE(four_win, 1.0);                                  // still inflate-only
}

// ---------------------------------------------------------------------------
// Gate 6: off is structurally unity.  The fold sites are not conditional on the
// knob; they multiply by factor() unconditionally, so this test is the
// byte-identity argument for the whole card.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, UnarmedInstancesAreExactlyUnity) {
    NetworkCarryVceApply fresh;
    EXPECT_FALSE(fresh.enabled());
    for (int f = 0; f < NetworkCarryVceApply::kNumFamilies; ++f)
        for (const char* s : {"G01", "E11", "C22", "R07", "J03"})
            EXPECT_DOUBLE_EQ(fresh.factor(f, std::string(s)), 1.0);

    NetworkCarryVceApply empty;
    empty.configure({}, 1.05, 1000.0, 200);
    EXPECT_FALSE(empty.enabled());
    EXPECT_DOUBLE_EQ(empty.factor(NetworkCarryVceApply::kPl, "G01"), 1.0);

    // An unknown family name never silently arms something else.
    NetworkCarryVceApply bogus;
    bogus.configure({"zwd"}, 1.05, 1000.0, 200);
    EXPECT_FALSE(bogus.enabled());
    EXPECT_EQ(NetworkCarryVceApply::familySlot("zwd"), -1);
    EXPECT_EQ(NetworkCarryVceApply::familySlot("pl"), NetworkCarryVceApply::kPl);
    // Spec section 3: the apply never reaches the Card-4 D5 instance, which pools
    // under the family name "dd" and already standardizes by its own sigma_hat_0;
    // scaling it would scale twice.  That instance is a separate NetworkCarryVce
    // owned by NetworkInvariantCarryDriver, so the apply cannot see it.  "dd"
    // having no slot is a second, structural lock (and the loader rejects it,
    // since it is not a carry_vce_components name).
    EXPECT_EQ(NetworkCarryVceApply::familySlot("dd"), -1);
    NetworkCarryVceApply dd;
    dd.configure({"dd"}, 1.05, 1000.0, 200);
    EXPECT_FALSE(dd.enabled());

    // stepEpoch on an unarmed instance is a no-op even with a live monitor.
    std::mt19937 gen(8u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    for (int e = 0; e < 50; ++e) {
        mon.beginEpoch(e * 60.0, true);
        feedEpoch(mon, empty, "pl", "G01", 25.0 * kHandFloor, kHandFloor, gen);
        empty.stepEpoch(mon, true);
    }
    EXPECT_EQ(empty.epochs(), 0);
    EXPECT_TRUE(empty.groups().empty());
    EXPECT_DOUBLE_EQ(empty.factor(NetworkCarryVceApply::kPl, "G01"), 1.0);
}

// ---------------------------------------------------------------------------
// Gate 7: the additive change to NetworkCarryVce.  hand_fold_var is the geometric
// mean of the hand fold variance over the same rows, and it defaults to the fold
// variance, so every pre-v2 caller (D5's instance, the block-Baarda and
// prior-residual sigma0 feeds, all of which pass fold_var = 1.0) is unchanged.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, MonitorTracksTheHandFoldVarianceSeparately) {
    NetworkCarryVce mon = makeMonitor({"pl"});
    mon.beginEpoch(0.0, true);
    // Two hand variances, one decade apart; the geometric mean is their sqrt product.
    for (int i = 0; i < 100; ++i) {
        mon.addRow("pl", "G01", 1.0e-3, 60.0, 1.0e-5, /*fold=*/8.0e-5, /*hand=*/1.0e-5);
        mon.addRow("pl", "G01", 1.0e-3, 60.0, 1.0e-5, /*fold=*/8.0e-4, /*hand=*/1.0e-4);
    }
    const auto e = mon.estimate("pl", "G01");
    EXPECT_NEAR(e.hand_fold_var, std::sqrt(1.0e-5 * 1.0e-4), 1.0e-12);
    // robust_floor is not affected by the new channel and stays independent of
    // alpha2; that independence makes the factor loop a tracker rather than a
    // feedback loop (see NetworkCarryVceApply.hpp).
    EXPECT_NEAR(e.robust_floor, 1.0e-6 / 0.2807297418, 1.0e-12);
    // The redundancy-corrected channel: r = 1 - v/f is 0.875 and 0.9875 on the two
    // row kinds, every row qualifies, and ê²/r is averaged geometrically.
    const double rbar = std::sqrt(0.875 * 0.9875);
    EXPECT_NEAR(e.rbar, rbar, 1.0e-12);
    EXPECT_NEAR(e.n_eff_r, e.n_eff, 1.0e-12);
    EXPECT_NEAR(e.robust_var_r, 1.0e-6 / rbar / 0.2807297418, 1.0e-12);
    EXPECT_NEAR(e.hand_fold_var_r, std::sqrt(1.0e-5 * 1.0e-4), 1.0e-12);
    EXPECT_NEAR(e.robust_floor, e.rbar * e.robust_var_r, 1.0e-12);

    // Omitting the hand value reproduces the pre-v2 behaviour exactly.
    NetworkCarryVce legacy = makeMonitor({"dd"});
    legacy.beginEpoch(0.0, true);
    for (int i = 0; i < 10; ++i)
        legacy.addRow("dd", "G01", 2.0, 0.0, 0.0, /*fold_var=*/1.0);
    EXPECT_DOUBLE_EQ(legacy.estimate("dd", "G01").hand_fold_var, 1.0);
}

// ---------------------------------------------------------------------------
// Gate 8: the redundancy belongs in the target (2026-10-07).  The prior is stiff
// against its data (d = 10 p) and the truth is 25x the hand floor.  LASER's factor
// iteration divides the group's residual square sum by its redundancy, and with
// that division the factor reaches 25.  The pre-2026-10-07 target,
// robust_floor / hand_fold_var, is E{ê²}/p = r²(a p + d)/p: from alpha2 = 1 it reads
// 35/121 = 0.29, and with d > a p / 4 it has no fixed point above 1, so the factor
// never left unity in exactly the stiff-carry case the iteration exists for.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, RecoversTheTrueFactorWhenThePriorIsStiff) {
    std::mt19937 gen(20261007u);
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, 1000.0, 200);

    const double data_var = 10.0 * kHandFloor;
    for (int ep = 0; ep < 600; ++ep) {
        mon.beginEpoch(ep * 60.0, true);
        feedEpoch(mon, apply, "pl", "G01", 25.0 * kHandFloor, kHandFloor, gen,
                  kRowsPerEpoch, data_var);
        apply.stepEpoch(mon, true);
    }
    const auto& g = group(apply, "pl|G");
    EXPECT_NEAR(g.alpha2, 25.0, 25.0 * 0.12);
    EXPECT_NEAR(g.sigma_hat_post, 1.0, 0.10);
    // At alpha2 = 25 the partial redundancy is 25/35.
    EXPECT_NEAR(g.rbar, 25.0 / 35.0, 0.05);
    // The uncorrected readback still under-reads the factor (about r²·35 = 17.9).
    EXPECT_LT(g.robust_floor / g.hand_fold_var, 0.8 * 25.0);
}

// ---------------------------------------------------------------------------
// Gate 9: rows without redundancy carry no information on the factor.  A row with
// r at or below the monitor's floor, or with r < 0 (the coupled ambiguity carry
// harvests a conditional variance that can exceed the prior), is left out of the
// target and of the warm-up count, while the uncorrected channel still sees it.
// ---------------------------------------------------------------------------
TEST(NetworkCarryVceApplyTest, RowsWithoutRedundancyAreLeftOutOfTheTarget) {
    NetworkCarryVce mon = makeMonitor({"pl"});
    NetworkCarryVceApply apply;
    apply.configure({"pl"}, 1.05, 1000.0, /*min_rows=*/200);

    const double fold = kHandFloor;
    const auto feed = [&](int ep) {
        mon.beginEpoch(ep * 60.0, true);
        for (int i = 0; i < kRowsPerEpoch; ++i) {
            mon.addRow("pl", "G01", 1.0e-3, 60.0, 0.5 * fold, fold, fold);          // r = 0.5
            mon.addRow("pl", "G01", 1.0, 60.0, 1.25 * fold, fold, fold);            // r = -0.25
            mon.addRow("pl", "G01", 1.0, 60.0, (1.0 - 1.0e-4) * fold, fold, fold);  // r = 1e-4
        }
        apply.stepEpoch(mon, true);
    };

    // 180 rows an epoch pass min_rows in the second epoch, but only the 60 with
    // redundancy count, so the group stays in warm-up for three epochs.
    for (int ep = 0; ep < 3; ++ep) {
        feed(ep);
        EXPECT_EQ(group(apply, "pl|G").freeze, NetworkCarryVceApply::Freeze::kWarmup)
            << "epoch " << ep;
    }
    const auto e = mon.estimate("pl", "G01");
    EXPECT_GT(e.n_eff, 200.0);
    EXPECT_LT(e.n_eff_r, 200.0);
    EXPECT_NEAR(e.n_eff_r, e.n_eff / 3.0, 1e-9);
    // The target reads the r = 0.5 rows alone: ê²/r = 2e-6 m².
    EXPECT_NEAR(e.robust_var_r, 2.0e-6 / 0.2807297418, 1e-15);
    EXPECT_NEAR(e.rbar, 0.5, 1e-12);
    EXPECT_NEAR(e.hand_fold_var_r, fold, 1e-15);
    // The uncorrected channel sees every row, the 1 m residuals included.
    EXPECT_GT(e.robust_floor, 1.0e-3);

    // The fourth epoch passes min_rows on the rows with redundancy, and the factor
    // steps against the target formed from them (below 1, so it stays at unity).
    feed(3);
    const auto& g = group(apply, "pl|G");
    EXPECT_EQ(g.freeze, NetworkCarryVceApply::Freeze::kNone);
    EXPECT_EQ(g.n_steps, 1);
    EXPECT_NEAR(g.target, 2.0e-6 / 0.2807297418 / fold, 1e-9);
    EXPECT_DOUBLE_EQ(g.alpha2, 1.0);
}

}  // namespace GnssProcessor
