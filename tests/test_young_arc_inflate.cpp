/**
 * @file test_young_arc_inflate.cpp
 * @brief Contract tests for @c network.phase_leveling_young_inflate_var_m2 — the
 *        AGE-AWARE variance inflation of freshly RE-LEVELLED phase-leveling arcs
 *        (SPEC_amb_iono_split_overcount §6.17–§6.18).
 *
 * WHY the card exists, in the one sentence the tests are shaped around: on the slipped
 * 60-station bed a detector slip response frees the arc for ONE epoch
 * (@c network.cycle_slip.relevel_free_epoch) and the next harvest re-stashes it at that
 * epoch's posterior — a level whose VALUE is code-quality (0.2–0.4 m off, the truth
 * census) but whose harvested VARIANCE is phase-STIFF, because the conditional it is
 * read from is conditioned on the station's MATURE arcs — so the arc is inconsistent
 * with geometry until ~20 min of geometry change refine it, the T4 attribution tier
 * erases it (21 erases/station-hour), and the erase–re-level treadmill sustains a
 * metre-class ZWD/clock plateau.
 *
 * Three things separate this from the neighbours falsified on the same bed the same
 * day, and each is a test below:
 *
 *  1. the population is AGE-SELECTED — only arcs young SINCE THEIR LAST RE-LEVEL, an
 *     age clock (@ref LocalCarry::relevel_s) that is deliberately NOT @c birth_s (a
 *     re-level preserves birth_s by design, and it is the re-level, not the birth, that
 *     resets what the level is worth).  Uniform floors (1e-4 / 1e-3 m²) and uniform
 *     level process noise (1e-7 / 1e-6 m²/s) were falsified — too small AND applied to
 *     every arc;
 *  2. the inflated arc STAYS INSIDE the F3 whitened block with its cross terms, applied
 *     as a CONGRUENCE scaling of its row/column of Σ.  Arm 5b (young legs kept OUT of
 *     the block) was falsified on the quiet bed's post-eclipse legs;
 *  3. the inflation starts at CODE level and DECAYS, so it is a statement about what
 *     the level is worth rather than a tuning constant.
 *
 * The congruence's three algebraic properties — exact target diagonal, PD preserved,
 * every correlation coefficient unchanged — are what make (2) safe, and they are pinned
 * here both at the helper (@ref GnssProcessor::NetworkYoungInflate::congruence) and at
 * the real seam (@c CouplingRowBuilder::buildLocalBlock over a registered
 * @c NetworkEstimator).  What a unit test cannot reach is the harvest's stamping site
 * and the PASS-B age arithmetic against a live run clock; the STAMP RULE itself is
 * factored into @ref GnssProcessor::NetworkYoungInflate::stamp so the rule — not a
 * paraphrase of it — is what the harvest calls and what these cases check.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkYoungInflate.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;
namespace YI = GnssProcessor::NetworkYoungInflate;

namespace {

// --- seam fixture ------------------------------------------------------------
//
// One registered satellite clock + phase bias, layout frozen at beginEpoch — the
// shape test_carry_zwd_in_block / test_carry_iono_in_block use.  A local copy so
// this file owns its suite name outright and no other file's layout can drift it.
struct YoungBlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1};

    YoungBlockFixture() {
        clk_col = est.registerClock("G01");
        pb_col  = est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 10);
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make() {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  /*joint=*/false, /*tu=*/false, /*shmarg=*/false,
                                  /*gauge=*/false, /*reinit=*/false, tu_reinit_hist, R};
    }
};

/// A hand-built SPD harvest block standing in for the (N_rr⁻¹) sub-matrix.
std::shared_ptr<PlHarvestBlock> makeHarvestBlock(const std::vector<double>& diag,
                                                 double offdiag) {
    auto blk = std::make_shared<PlHarvestBlock>();
    const Eigen::Index n = static_cast<Eigen::Index>(diag.size());
    blk->C = Eigen::MatrixXd::Constant(n, n, offdiag);
    for (Eigen::Index k = 0; k < n; ++k) blk->C(k, k) = diag[static_cast<std::size_t>(k)];
    blk->stamp_s = 1000.0;
    return blk;
}

/// A carried local column pointing at @p blk index @p idx, with one PRESENT shared
/// coupling term (the G01 clock) so every row carries a real border regressor.
LocalCarry makeCarry(double value_abs, double var,
                     const std::shared_ptr<PlHarvestBlock>& blk, int idx) {
    LocalCarry lc;
    lc.value_abs   = value_abs;
    lc.var         = var;
    lc.last_seen_s = 0.0;
    lc.coupling    = { CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C',
                                     /*sh_layer=*/0, /*coeff=*/0.8, /*s_prev=*/1.5,
                                     /*p_prev=*/0.2} };
    lc.blk     = blk;
    lc.blk_idx = idx;
    return lc;
}

/// The knobs as the validated arm sets them: V0 = (0.3 m)², τ = 20 ep, ceiling 60 ep.
YI::Knobs armed(double v0 = 0.09, double tau = 20.0, int max_age = 60) {
    YI::Knobs k;
    k.var_m2 = v0; k.tau_ep = tau; k.max_age_ep = max_age;
    return k;
}

double minEig(const Eigen::MatrixXd& M) {
    return Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd>(M).eigenvalues().minCoeff();
}

Eigen::MatrixXd corrOf(const Eigen::MatrixXd& S) {
    Eigen::MatrixXd C = S;
    for (Eigen::Index i = 0; i < S.rows(); ++i)
        for (Eigen::Index j = 0; j < S.cols(); ++j)
            C(i, j) = S(i, j) / std::sqrt(S(i, i) * S(j, j));
    return C;
}

// --- loader fixture ----------------------------------------------------------

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("yngarc_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The posture the card is validated on (the F3 golden's leveling vehicle).
const char* kF3Posture =
    "    \"phase_leveling\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"phase_leveling_local_block\": true,\n";

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

// =============================================================================
// (1) THE STAMP.  relevel_s is the age clock, and it moves on exactly two events.
// =============================================================================

// A CREATED entry — a first stash, or the re-stash after a T4 attribution erase / gap
// prune DELETED the entry (which is precisely why the erase needs no clause of its own:
// it deletes, so the next harvest creates).  Its level came from one epoch of code.
TEST(YoungArcInflateTest, StampIsSetWhenTheEntryIsCreated) {
    EXPECT_DOUBLE_EQ(YI::stamp(/*prev=*/0.0, /*created=*/true, /*freed=*/false,
                               /*now_s=*/1'234'567.0),
                     1'234'567.0);
}

// A FREED arc — it was in this epoch's latched relevel.free_ambs set, so THIS harvest
// re-levelled it from one epoch of code even though the entry already existed.
TEST(YoungArcInflateTest, StampIsSetWhenTheArcWasFreedThisEpoch) {
    EXPECT_DOUBLE_EQ(YI::stamp(/*prev=*/1'000'000.0, /*created=*/false, /*freed=*/true,
                               /*now_s=*/1'234'567.0),
                     1'234'567.0);
}

// Every OTHER re-stash PRESERVES it — otherwise the age would reset every epoch and no
// arc could ever mature out of the inflation.
TEST(YoungArcInflateTest, StampIsPreservedOnAnOrdinaryRestash) {
    EXPECT_DOUBLE_EQ(YI::stamp(/*prev=*/1'000'000.0, /*created=*/false, /*freed=*/false,
                               /*now_s=*/1'234'567.0),
                     1'000'000.0);
}

TEST(YoungArcInflateTest, StampIsSetWhenBothEventsCoincide) {
    EXPECT_DOUBLE_EQ(YI::stamp(0.0, /*created=*/true, /*freed=*/true, 500.0), 500.0);
}

// =============================================================================
// (2) THE DECAY.  V_add = V0·exp(−age/τ), age in LOOP EPOCHS.
// =============================================================================

TEST(YoungArcInflateTest, VarAddStartsAtV0AndEFoldsEveryTau) {
    const YI::Knobs k = armed(0.09, 20.0, 60);
    const double now = 100'000.0, dt = 30.0;

    const auto a0 = YI::varAdd(k, /*relevel_s=*/now, now, dt);          // age 0
    EXPECT_TRUE(a0.inflated);
    EXPECT_NEAR(a0.age_ep, 0.0, 1e-12);
    EXPECT_NEAR(a0.var_add, 0.09, 1e-15);

    const auto a1 = YI::varAdd(k, now - 20.0 * dt, now, dt);            // age τ
    EXPECT_TRUE(a1.inflated);
    EXPECT_NEAR(a1.age_ep, 20.0, 1e-12);
    EXPECT_NEAR(a1.var_add, 0.09 / std::exp(1.0), 1e-15);

    const auto a2 = YI::varAdd(k, now - 40.0 * dt, now, dt);            // age 2τ
    EXPECT_NEAR(a2.var_add, 0.09 / std::exp(2.0), 1e-15);
    // Strictly decreasing in age — the whole point is that the arc EARNS its stiffness
    // back rather than being handed a fixed floor.
    EXPECT_GT(a0.var_add, a1.var_add);
    EXPECT_GT(a1.var_add, a2.var_add);
}

// The ceiling is a REFUSAL, and it is COUNTED (invariant #3: measure the deferral).
TEST(YoungArcInflateTest, NothingIsAddedBeyondTheAgeCeilingAndTheRefusalIsFlagged) {
    const YI::Knobs k = armed(0.09, 20.0, 60);
    const double now = 100'000.0, dt = 30.0;

    const auto in  = YI::varAdd(k, now - 60.0 * dt, now, dt);      // exactly at the ceiling
    EXPECT_TRUE(in.inflated);
    EXPECT_FALSE(in.over_age);

    const auto out = YI::varAdd(k, now - 60.5 * dt, now, dt);      // just past it
    EXPECT_FALSE(out.inflated);
    EXPECT_TRUE(out.over_age);
    EXPECT_DOUBLE_EQ(out.var_add, 0.0);
}

// relevel_s == 0 is UNSTAMPED — a warm-resume-restored entry (the field is not
// snapshotted, by the birth_s rule) or a pre-card lineage.  It reads as MATURE: no
// inflation, and NOT counted as an age refusal, so the age-skip counter stays a
// statement about real arcs.
TEST(YoungArcInflateTest, AnUnstampedEntryIsMatureAndIsNotCountedAsAnAgeRefusal) {
    const auto a = YI::varAdd(armed(), /*relevel_s=*/0.0, /*now_s=*/100'000.0, 30.0);
    EXPECT_FALSE(a.inflated);
    EXPECT_FALSE(a.over_age);
    EXPECT_DOUBLE_EQ(a.var_add, 0.0);
}

// The age unit is LOOP EPOCHS, taken from the run's own observed spacing — the same
// wall-clock gap is a young arc at 30 s cadence and a mature one at 1 s.
TEST(YoungArcInflateTest, AgeIsCountedInLoopEpochsNotSeconds) {
    const YI::Knobs k = armed(0.09, 20.0, 60);
    const double now = 100'000.0, dt_s = 300.0;   // 5 minutes since the re-level

    const auto slow = YI::varAdd(k, now - dt_s, now, /*epoch_s=*/30.0);   // 10 epochs
    const auto fast = YI::varAdd(k, now - dt_s, now, /*epoch_s=*/1.0);    // 300 epochs
    EXPECT_NEAR(slow.age_ep, 10.0, 1e-12);
    EXPECT_TRUE(slow.inflated);
    EXPECT_NEAR(fast.age_ep, 300.0, 1e-12);
    EXPECT_FALSE(fast.inflated);      // past the 60-epoch ceiling at 1 s cadence
    EXPECT_TRUE(fast.over_age);
}

// An unknowable epoch length (first epoch, implausible spacing) must never become a
// division by zero or a guessed nominal rate — it inflates nothing.
TEST(YoungArcInflateTest, AnUnknownEpochLengthInflatesNothing) {
    const YI::Knobs k = armed();
    for (double dt : {0.0, -30.0, std::numeric_limits<double>::quiet_NaN()}) {
        const auto a = YI::varAdd(k, 99'000.0, 100'000.0, dt);
        EXPECT_FALSE(a.inflated);
        EXPECT_DOUBLE_EQ(a.var_add, 0.0);
    }
}

// A stamp in the future (a clock that went backwards) is refused rather than producing
// a negative age that would read as "inflate at V0".
TEST(YoungArcInflateTest, ANegativeAgeIsRefused) {
    const auto a = YI::varAdd(armed(), /*relevel_s=*/100'030.0, /*now_s=*/100'000.0, 30.0);
    EXPECT_FALSE(a.inflated);
    EXPECT_FALSE(a.over_age);
}

// OFF is OFF on all three shape knobs, including the degenerate shapes the loader also
// refuses — belt and suspenders, since a config path is not the only caller.
TEST(YoungArcInflateTest, TheKnobIsOffAtV0ZeroAndOnEveryDegenerateShape) {
    const double now = 100'000.0;
    EXPECT_FALSE(armed(0.0).on());
    EXPECT_FALSE(armed(0.09, 0.0).on());
    EXPECT_FALSE(armed(0.09, 20.0, 0).on());
    for (const YI::Knobs& k : {armed(0.0), armed(0.09, 0.0), armed(0.09, 20.0, 0)}) {
        const auto a = YI::varAdd(k, now - 30.0, now, 30.0);
        EXPECT_FALSE(a.inflated);
        EXPECT_FALSE(a.over_age);
        EXPECT_DOUBLE_EQ(a.var_add, 0.0);
    }
    EXPECT_TRUE(armed().on());
}

// =============================================================================
// (3) THE CONGRUENCE.  Σ ← DΣD: exact target diagonal, PD preserved, correlations
//     EXACTLY unchanged.  This is what lets the arc stay INSIDE the block.
// =============================================================================

TEST(YoungArcInflateTest, CongruenceInflatesTheTargetDiagonalExactlyAndPreservesPd) {
    Eigen::MatrixXd S(4, 4);
    S << 0.040, 0.006, 0.005, 0.004,
         0.006, 0.030, 0.007, 0.003,
         0.005, 0.007, 0.050, 0.006,
         0.004, 0.003, 0.006, 0.012;
    ASSERT_GT(minEig(S), 0.0);
    const Eigen::MatrixXd S0 = S;

    const double add = 0.09;
    const std::vector<double> va = {0.0, add, 0.0, 0.0};
    EXPECT_EQ(YI::congruence(S, va), 1u);

    EXPECT_NEAR(S(1, 1), S0(1, 1) + add, 1e-15);      // exactly Σ_jj + var_add
    // Untouched columns keep their marginals BITWISE — the inflation is local.
    for (int k : {0, 2, 3}) EXPECT_DOUBLE_EQ(S(k, k), S0(k, k));
    // Off-diagonals of the target column scaled by f; everything else bitwise.
    const double f = std::sqrt((S0(1, 1) + add) / S0(1, 1));
    for (int k : {0, 2, 3}) {
        EXPECT_NEAR(S(1, k), S0(1, k) * f, 1e-15);
        EXPECT_NEAR(S(k, 1), S0(k, 1) * f, 1e-15);
    }
    EXPECT_DOUBLE_EQ(S(0, 2), S0(0, 2));
    EXPECT_DOUBLE_EQ(S(0, 3), S0(0, 3));
    EXPECT_DOUBLE_EQ(S(2, 3), S0(2, 3));
    // Symmetric, and still PD (Sylvester: D is diagonal and strictly positive).
    EXPECT_LT((S - S.transpose()).cwiseAbs().maxCoeff(), 1e-18);
    EXPECT_GT(minEig(S), 0.0);
}

// THE property that makes it a congruence and not a diagonal bump: a freshly
// re-levelled arc is not LESS coupled to its station's other columns, it is worth less
// on its own.  Every correlation coefficient must survive to machine precision.
TEST(YoungArcInflateTest, CongruencePreservesEveryCorrelationCoefficient) {
    Eigen::MatrixXd S(4, 4);
    S << 0.040, 0.006, 0.005, 0.004,
         0.006, 0.030, 0.007, 0.003,
         0.005, 0.007, 0.050, 0.006,
         0.004, 0.003, 0.006, 0.012;
    const Eigen::MatrixXd C0 = corrOf(S);

    const std::vector<double> va = {0.09, 0.0, 0.02, 0.0};   // TWO inflated columns
    EXPECT_EQ(YI::congruence(S, va), 2u);
    EXPECT_LT((corrOf(S) - C0).cwiseAbs().maxCoeff(), 1e-14);
    EXPECT_GT(minEig(S), 0.0);

    // …and the contrast: adding the same amounts to the DIAGONAL alone does move them.
    Eigen::MatrixXd D(4, 4);
    D << 0.040, 0.006, 0.005, 0.004,
         0.006, 0.030, 0.007, 0.003,
         0.005, 0.007, 0.050, 0.006,
         0.004, 0.003, 0.006, 0.012;
    D(0, 0) += 0.09; D(2, 2) += 0.02;
    EXPECT_GT((corrOf(D) - C0).cwiseAbs().maxCoeff(), 1e-3);
}

// Two inflated columns must compose as D Σ D exactly, including the (j,k) entry that
// takes BOTH factors — the loop applies row and column scalings independently, so this
// is the case a naive implementation gets wrong.
TEST(YoungArcInflateTest, CongruenceMatchesDSigmaDForOverlappingColumns) {
    Eigen::MatrixXd S(3, 3);
    S << 0.040, 0.006, 0.005,
         0.006, 0.030, 0.007,
         0.005, 0.007, 0.050;
    const Eigen::MatrixXd S0 = S;
    const std::vector<double> va = {0.09, 0.02, 0.0};
    ASSERT_EQ(YI::congruence(S, va), 2u);

    Eigen::VectorXd d(3);
    for (int j = 0; j < 3; ++j)
        d(j) = (va[static_cast<std::size_t>(j)] > 0.0)
                   ? std::sqrt((S0(j, j) + va[static_cast<std::size_t>(j)]) / S0(j, j))
                   : 1.0;
    const Eigen::MatrixXd want = d.asDiagonal() * S0 * d.asDiagonal();
    EXPECT_LT((S - want).cwiseAbs().maxCoeff(), 1e-18);
}

// OFF path: not one write to Σ.  This is the byte-identity contract at the algebra
// level — the driver's OFF path never produces a positive var_add at all.
TEST(YoungArcInflateTest, CongruenceWithNoPositiveAdditionTouchesNothing) {
    Eigen::MatrixXd S(3, 3);
    S << 0.040, 0.006, 0.005,
         0.006, 0.030, 0.007,
         0.005, 0.007, 0.050;
    const Eigen::MatrixXd S0 = S;
    EXPECT_EQ(YI::congruence(S, {0.0, 0.0, 0.0}), 0u);
    EXPECT_EQ(S, S0);
    EXPECT_EQ(YI::congruence(S, {-1.0, 0.0, 0.0}), 0u);   // a negative addition is refused
    EXPECT_EQ(S, S0);
    EXPECT_EQ(YI::congruence(S, {0.09, 0.0}), 0u);        // size mismatch ⇒ no-op
    EXPECT_EQ(S, S0);
}

// =============================================================================
// (4) THE SEAM.  LocalBlockCol::var_add reaches Σ through buildLocalBlock, the arc
//     STAYS IN the block, and the whitening still reproduces L⁻¹·[I | M | y].
// =============================================================================

TEST(YoungArcInflateTest, VarAddInflatesItsBlockColumnInsideTheWhitenedBlock) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    const double cross = 0.0060;
    auto blk = makeHarvestBlock(cdiag, cross);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);   // the freshly re-levelled arc
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);

    const double add = 0.09;
    YoungBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({
        LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0, false, false, add},
        LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
    }));

    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 3);
    EXPECT_NEAR(S(1, 1), cdiag[1] + add, 1e-15);         // the target, exactly
    EXPECT_DOUBLE_EQ(S(0, 0), cdiag[0]);                 // the mature arcs, bitwise
    EXPECT_DOUBLE_EQ(S(2, 2), cdiag[2]);
    EXPECT_DOUBLE_EQ(S(0, 2), cross);                    // their mutual term, bitwise
    const double fj = std::sqrt((cdiag[1] + add) / cdiag[1]);
    EXPECT_NEAR(S(0, 1), cross * fj, 1e-15);             // still COUPLED, just looser
    EXPECT_NEAR(S(1, 2), cross * fj, 1e-15);
    EXPECT_GT(minEig(S), 0.0);
    EXPECT_EQ(f.R.pl_blk_yng_scaled, 1);                 // counted (invariant #4)
    EXPECT_EQ(f.R.pl_blk_rows, 3);                       // the arc is IN the block, not out

    // The emitted rows are still exactly L⁻¹·[I | M | y] for the SCALED Σ.
    const Eigen::MatrixXd L = Eigen::LLT<Eigen::MatrixXd>(S).matrixL();
    Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(3, f.R.A_r.cols());
    Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(3, f.R.A_s.cols());
    Eigen::VectorXd z(3);
    for (int k = 0; k < 3; ++k) {
        for (const auto& lc : cb.crows[static_cast<std::size_t>(k)].local) Hr(k, lc.first) = lc.second;
        for (const auto& sc : cb.crows[static_cast<std::size_t>(k)].shared) Hs(k, sc.first) = sc.second;
        z(k) = cb.crows[static_cast<std::size_t>(k)].y;
    }
    Eigen::MatrixXd Hr_raw = Eigen::MatrixXd::Zero(3, f.R.A_r.cols());
    Eigen::MatrixXd Hs_raw = Eigen::MatrixXd::Zero(3, f.R.A_s.cols());
    Eigen::VectorXd z_raw(3);
    const double xlin[3] = {0.5, -0.25, 1.0};
    const LocalCarry* lcs[3] = {&a0, &a1, &a2};
    for (int k = 0; k < 3; ++k) {
        Hr_raw(k, k) = 1.0;
        Hs_raw(k, f.clk_col) = 0.8;
        z_raw(k) = (lcs[k]->value_abs - xlin[k]) + 0.8 * 1.5;
    }
    EXPECT_LT((L * Hr - Hr_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * Hs - Hs_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * z  - z_raw ).cwiseAbs().maxCoeff(), 1e-12);
}

// OFF at the seam: var_add 0 on every column must be bit-identical to the pre-card
// block — same Σ, same rows, same counters, and pl_blk_yng_scaled untouched.
TEST(YoungArcInflateTest, ZeroVarAddIsBitIdenticalToTheUninflatedBlock) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);

    YoungBlockFixture off_f, base_f;
    auto off  = off_f.make();
    auto base = base_f.make();
    ASSERT_TRUE(off.buildLocalBlock({
        LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0, false, false, 0.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0, false, false, 0.0},
        LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0, false, false, 0.0},
    }));
    ASSERT_TRUE(base.buildLocalBlock({          // the pre-card 6-element brace init
        LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
        LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
    }));
    EXPECT_EQ(off_f.R.pl_blk.Sigma, base_f.R.pl_blk.Sigma);
    ASSERT_EQ(off.crows.size(), base.crows.size());
    for (std::size_t k = 0; k < off.crows.size(); ++k) {
        EXPECT_EQ(off.crows[k].y, base.crows[k].y);
        EXPECT_EQ(off.crows[k].w, base.crows[k].w);
        ASSERT_EQ(off.crows[k].local.size(), base.crows[k].local.size());
        for (std::size_t j = 0; j < off.crows[k].local.size(); ++j)
            EXPECT_EQ(off.crows[k].local[j].second, base.crows[k].local[j].second);
    }
    EXPECT_EQ(off_f.R.pl_blk_yng_scaled, 0);
    EXPECT_EQ(base_f.R.pl_blk_yng_scaled, 0);
}

// The inflation LOOSENS: the whitened row's information about its own column must go
// DOWN, which is the entire mechanical claim ("the level is worth less than the stiff
// harvested variance says").  1/L_kk is the row's own-column coefficient.
TEST(YoungArcInflateTest, InflationLoosensTheArcsOwnWhitenedRow) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);

    const auto ownCoeff = [&](double add) {
        YoungBlockFixture f;
        auto cb = f.make();
        EXPECT_TRUE(cb.buildLocalBlock({
            LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
            LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0, false, false, add},
            LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
        }));
        // Row 1's coefficient on its OWN local column (block column 1 = A_r column 1).
        for (const auto& lc : cb.crows[1].local)
            if (lc.first == 1) return std::abs(lc.second);
        return 0.0;
    };
    EXPECT_LT(ownCoeff(0.09), ownCoeff(0.0));
}

// A column so heavily inflated that it dwarfs the block must still factorize — the
// congruence cannot break PD, so there is no "inflate too hard and lose the block"
// failure mode to guard against elsewhere.
TEST(YoungArcInflateTest, AHugeInflationStillFactorizes) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);

    YoungBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({
        LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0, false, false, 1.0e4},
        LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
    }));
    EXPECT_GT(minEig(f.R.pl_blk.Sigma), 0.0);
    EXPECT_EQ(f.R.pl_blk_rows, 3);
}

// =============================================================================
// (5) Loader interlocks.
// =============================================================================

TEST(YoungArcInflateTest, DefaultIsOffOnAnArmedLevelingPosture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(kF3Posture), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.0);
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateTauEp(), 20.0);
    EXPECT_EQ(cfg.networkPhaseLevelingYoungInflateMaxAgeEp(), 60);
    EXPECT_TRUE(cfg.networkPhaseLeveling());       // the vehicle IS armed…
}

TEST(YoungArcInflateTest, ArmedHappyPathKeepsTheValueAndTheShapeDefaults) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"),
                         cfg));
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.09);
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateTauEp(), 20.0);
    EXPECT_EQ(cfg.networkPhaseLevelingYoungInflateMaxAgeEp(), 60);
}

TEST(YoungArcInflateTest, ExplicitShapeKnobsOverrideTheDefaults) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("shape",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"phase_leveling_young_inflate_var_m2\": 0.01,\n"
                                         "    \"phase_leveling_young_inflate_tau_ep\": 8.5,\n"
                                         "    \"phase_leveling_young_inflate_max_age_ep\": 40,\n"),
                         cfg));
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.01);
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateTauEp(), 8.5);
    EXPECT_EQ(cfg.networkPhaseLevelingYoungInflateMaxAgeEp(), 40);
}

// There is no carried leveling column to inflate without the carry itself.
TEST(YoungArcInflateTest, DisabledWhenPhaseLevelingIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nolevel",
                         networkConfig("    \"carry_conditional_coupling\": true,\n"
                                       "    \"shared_carry_full_cov\": true,\n"
                                       "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLeveling());
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.0);
}

// Card 4's invariant DD carry supersedes the leveling ambiguity inject outright
// (do_amb_carry is false), so riding it would be ARMED AND INERT — invariant #4.
TEST(YoungArcInflateTest, DisabledUnderCarryInvariant) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("ci",
                         networkConfig(std::string(kF3Posture)
                                       // carry_invariant is only DEFINED on the eq34
                                       // chain datum, so the posture must be legal
                                       // before the interlock under test can fire.
                                       + "    \"phase_datum_eq34\": true,\n"
                                         "    \"carry_invariant\": true,\n"
                                         "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryInvariant());
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.0);
}

// F3 is deliberately NOT required: off the block the inflation is a plain
// var_eff += var_add on the per-column diagonal row, and that route is wired.
TEST(YoungArcInflateTest, StaysArmedWithoutTheF3Block) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nof3",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLevelingLocalBlock());
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.09);
}

TEST(YoungArcInflateTest, NegativeVarIsARefusedConfig) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("negv",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"phase_leveling_young_inflate_var_m2\": -0.01,\n"),
                          cfg));
}

TEST(YoungArcInflateTest, NonPositiveTauIsARefusedConfig) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zerotau",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"
                                          "    \"phase_leveling_young_inflate_tau_ep\": 0.0,\n"),
                          cfg));
}

TEST(YoungArcInflateTest, NonPositiveMaxAgeIsARefusedConfig) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zeroage",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"
                                          "    \"phase_leveling_young_inflate_max_age_ep\": 0,\n"),
                          cfg));
}
