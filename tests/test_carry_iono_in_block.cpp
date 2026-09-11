/**
 * @file test_carry_iono_in_block.cpp
 * @brief Contract tests for @c network.carry_iono_in_block — the per-LOS slant-iono
 *        columns folded INSIDE the F3 jointly-whitened station block, the I half of
 *        the ZWD-in-block card (SPEC_amb_iono_split_overcount §7 item 17, §6.16.7d).
 *
 * WHY the card exists, in one sentence the tests are shaped around: a station's B
 * carried band priors already assert each LOS's iono direction implicitly, so an
 * INDEPENDENT diagonal iono prior — @c network.carry_iono, QUEUE 8(b), falsified ×4
 * — asserts it a further time (the B-fold over-count); inside the block the joint Σ
 * carries the harvested @f$(N_{rr}^{-1})_{c\,c_j}@f$ cross terms and the overlap is
 * subtracted exactly.  That is the same argument that vindicated the ZWD carry
 * (0.723 m diagonal vs 0.088 m in-block on the quiet bed).
 *
 * What a unit test can reach is the SEAM the card acts on —
 * @c CouplingRowBuilder::buildLocalBlock over a real registered
 * @c NetworkEstimator — plus the loader's interlocks and the carry map's key shape.
 * The two ends it cannot reach are the harvest (which appends the iono columns to
 * @c cols and writes their rows of @c PlHarvestBlock::C) and the PASS-B driver
 * (which computes @c var_eff = var + q·gap_ep and pushes the @c LocalBlockCol);
 * those are graded on the sim fixture ladder.  What IS pinned here is the contract
 * between them: whatever @c var_eff the driver computes lands on the block's
 * DIAGONAL and nowhere else, and the harvested cross terms are what couple the iono
 * to the rest of the station.
 *
 * ⚠ Deliberately NOT tested as a fallback: there is no diagonal iono row.  A
 * diagonal per-LOS iono prior IS the falsified vehicle, so a non-SPD block means the
 * iono carry did not act (counted @c pl_blk_iono_not_folded by the driver), and the
 * unit contract is that the failed build buffers NOTHING.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkEstimator.hpp"
#include "app/NetworkRunContext.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// --- seam fixture ------------------------------------------------------------
//
// Mirrors test_carry_zwd_in_block.cpp's ZwdBlockFixture (one registered satellite
// clock + phase bias, layout frozen at beginEpoch) — a separate local copy so this
// file owns its suite name outright and neither file's cases can drift the other's
// layout.
struct IonoBlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1};

    IonoBlockFixture() {
        clk_col = est.registerClock("G01");
        pb_col  = est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 10);   // 10 local columns is plenty for these cases
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make() {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  /*joint=*/false, /*tu=*/false, /*shmarg=*/false,
                                  /*gauge=*/false, /*reinit=*/false, tu_reinit_hist, R};
    }
};

/// One station-epoch's harvest block over @p diag.size() columns: a hand-built SPD
/// conditional covariance standing in for the (N_rr⁻¹) sub-matrix the production
/// harvest gathers.  Diagonally dominant so every principal sub-matrix is SPD too.
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

// --- loader fixture ----------------------------------------------------------

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("ionoblk_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p extra splices into the network block.
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

/// The posture the card is validated on: the leveling carry as the live vehicle,
/// folded as Card-1 conditional coupling rows, with F3 armed.
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
// (1) ON: k ambiguities + the ZWD + m iono columns build ONE (k+1+m)-row whitened
//     block whose Σ carries the HARVESTED iono cross terms.
// =============================================================================
//
// This is the card's whole claim in one case: the per-LOS iono is coupled to the
// station's ambiguities (and to the ZWD, and to each other) through the harvested
// (N_rr⁻¹) terms rather than asserted independently.  Σ is checked directly, then
// the whitening is verified the way test_network_coupling checks an ambiguity-only
// block: L·(emitted) == raw.
TEST(CarryIonoInBlockTest, IonoColumnsJoinTheWhitenedBlockAndCarryTheirHarvestedCrossTerms) {
    IonoBlockFixture f;
    // 3 ambiguity columns (a GPS triple on one LOS) + the ZWD + 2 per-LOS iono columns.
    // Block index order mirrors the harvest's: ambiguities, then iono, then the ZWD
    // last (the ZWD card computes its own row as C.rows()−1 and must keep that slot).
    const std::vector<double> cdiag = {0.040, 0.030, 0.050, 0.0120, 0.0090, 0.0075};
    const double cross = 0.0060;                       // (N_rr⁻¹)_{c_i c_j}, all pairs
    auto blk = makeHarvestBlock(cdiag, cross);

    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    LocalCarry i0 = makeCarry(3.10, cdiag[3], blk, 3);   // 3.10 m of L1 slant delay
    LocalCarry i1 = makeCarry(5.40, cdiag[4], blk, 4);
    LocalCarry zw = makeCarry(0.16, cdiag[5], blk, 5);

    std::vector<LocalBlockCol> bc = {
        LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
        LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
        LocalBlockCol{3, 3.05,  &i0, cdiag[3], 30.0, 1.0, /*is_zwd=*/false, /*is_iono=*/true},
        LocalBlockCol{4, 5.20,  &i1, cdiag[4], 30.0, 1.0, /*is_zwd=*/false, /*is_iono=*/true},
        LocalBlockCol{5, 0.15,  &zw, cdiag[5], 30.0, 1.0, /*is_zwd=*/true},
    };

    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(bc));

    // k + m + 1 = 6 whitened rows, one per column, all unit weight.
    ASSERT_EQ(cb.crows.size(), 6u);
    for (const PendCplRow& row : cb.crows) {
        EXPECT_DOUBLE_EQ(row.w, 1.0);
        EXPECT_FALSE(row.local.empty());   // a BLOCK row (dense L⁻¹), not a scalar +1 row
    }
    EXPECT_EQ(cb.crows[3].local_col, 3);   // each iono row's PRIMARY column is its own
    EXPECT_EQ(cb.crows[4].local_col, 4);

    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 6);
    ASSERT_EQ(S.cols(), 6);
    for (int k = 0; k < 6; ++k)
        EXPECT_DOUBLE_EQ(S(k, k), cdiag[static_cast<std::size_t>(k)]);
    // The iono columns are coupled to EVERYTHING the harvest saw: the ambiguities
    // (the B-fold overlap this card subtracts), the ZWD, and each other.
    for (int k = 0; k < 6; ++k)
        for (int j : {3, 4}) {
            if (k == j) continue;
            EXPECT_DOUBLE_EQ(S(j, k), cross);
            EXPECT_DOUBLE_EQ(S(k, j), cross);
        }
    EXPECT_EQ(f.R.pl_blk_iono_cols, 2);           // counted exactly once per column
    EXPECT_EQ(f.R.pl_blk_iono_missing_cross, 0);
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);            // the ρ half is untouched by this card
    EXPECT_EQ(f.R.pl_blk_pair_missing, 0);
    EXPECT_EQ(f.R.pl_blk_blocks, 1);
    EXPECT_EQ(f.R.pl_blk_rows, 6);

    // The emitted rows are L⁻¹·[I | M | y] for this Σ: reconstruct L and undo it.
    // The iono rows are built by the SAME ingredient path as an ambiguity row — that
    // identity ("just another carried local column") is the point.
    const Eigen::MatrixXd L = Eigen::LLT<Eigen::MatrixXd>(S).matrixL();
    Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(6, f.R.A_r.cols());
    Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(6, f.R.A_s.cols());
    Eigen::VectorXd z(6);
    for (int k = 0; k < 6; ++k) {
        for (const auto& lc : cb.crows[static_cast<std::size_t>(k)].local) Hr(k, lc.first) = lc.second;
        for (const auto& sc : cb.crows[static_cast<std::size_t>(k)].shared) Hs(k, sc.first) = sc.second;
        z(k) = cb.crows[static_cast<std::size_t>(k)].y;
    }
    Eigen::MatrixXd Hr_raw = Eigen::MatrixXd::Zero(6, f.R.A_r.cols());
    Eigen::MatrixXd Hs_raw = Eigen::MatrixXd::Zero(6, f.R.A_s.cols());
    Eigen::VectorXd z_raw(6);
    for (int k = 0; k < 6; ++k) {
        Hr_raw(k, bc[static_cast<std::size_t>(k)].col) = 1.0;
        Hs_raw(k, f.clk_col) = 0.8;
        z_raw(k) = (bc[static_cast<std::size_t>(k)].lc->value_abs
                    - bc[static_cast<std::size_t>(k)].x_lin) + 0.8 * 1.5;
    }
    EXPECT_LT((L * Hr - Hr_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * Hs - Hs_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * z  - z_raw ).cwiseAbs().maxCoeff(), 1e-12);
}

// The is_iono flag is TELEMETRY ONLY.  If it ever became load-bearing in the algebra
// the iono would stop being "just another carried local column" and the exactness
// claim would need re-proving — so pin that the whitened output is bit-identical
// with the flag set and cleared.
TEST(CarryIonoInBlockTest, IsIonoFlagChangesTelemetryOnlyNotTheAlgebra) {
    const std::vector<double> cdiag = {0.040, 0.0120};
    auto blk = makeHarvestBlock(cdiag, 0.0050);
    LocalCarry a0 = makeCarry(2.0,  cdiag[0], blk, 0);
    LocalCarry i0 = makeCarry(3.10, cdiag[1], blk, 1);

    IonoBlockFixture flag_f, noflag_f;
    auto flag   = flag_f.make();
    auto noflag = noflag_f.make();
    ASSERT_TRUE(flag.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                      LocalBlockCol{1, 3.05, &i0, cdiag[1], 30.0, 1.0,
                                                    false, true}}));
    ASSERT_TRUE(noflag.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                        LocalBlockCol{1, 3.05, &i0, cdiag[1], 30.0, 1.0,
                                                      false, false}}));
    ASSERT_EQ(flag.crows.size(), noflag.crows.size());
    for (std::size_t k = 0; k < flag.crows.size(); ++k) {
        EXPECT_EQ(flag.crows[k].w, noflag.crows[k].w);
        EXPECT_EQ(flag.crows[k].y, noflag.crows[k].y);
        EXPECT_EQ(flag.crows[k].local_col, noflag.crows[k].local_col);
        ASSERT_EQ(flag.crows[k].local.size(), noflag.crows[k].local.size());
        for (std::size_t j = 0; j < flag.crows[k].local.size(); ++j) {
            EXPECT_EQ(flag.crows[k].local[j].first, noflag.crows[k].local[j].first);
            EXPECT_EQ(flag.crows[k].local[j].second, noflag.crows[k].local[j].second);
        }
    }
    EXPECT_EQ(flag_f.R.pl_blk_iono_cols, 1);     // the ONLY difference
    EXPECT_EQ(noflag_f.R.pl_blk_iono_cols, 0);
}

// =============================================================================
// (2) OFF: the block is k rows and no iono column exists — and, unlike the ZWD,
//     there is NO diagonal replacement row.
// =============================================================================
TEST(CarryIonoInBlockTest, KnobOffLeavesKBlockRowsAndNoIonoColumnAtAll) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);

    IonoBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
                                    LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                    LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0}}));
    ASSERT_EQ(cb.crows.size(), 3u);                 // k rows, NOT k+m
    EXPECT_EQ(f.R.pl_blk.Sigma.rows(), 3);
    EXPECT_EQ(f.R.pl_blk_iono_cols, 0);
    EXPECT_EQ(f.R.pl_blk_iono_missing_cross, 0);
    EXPECT_EQ(f.R.pl_blk_iono_not_folded, 0);
    EXPECT_TRUE(f.R.pl_iono_blk_cols.empty());
}

// =============================================================================
// (3) q inflation lands on the IONO DIAGONAL ONLY, and PD is preserved.
// =============================================================================
//
// The driver computes var_eff = var + q·gap_ep and hands it over as
// LocalBlockCol::var_eff.  The contract this pins is where that lands: Σ's diagonal
// for that column and NOWHERE else — the off-diagonals stay the harvested cross
// terms (a per-column random walk has no cross term) and the ambiguity rows' entries
// are BITWISE unchanged.  Raising a diagonal of a PD matrix is PD-preserving, so the
// inflated block must still factorize; both are asserted.
TEST(CarryIonoInBlockTest, QInflationTouchesTheIonoDiagonalOnlyAndPreservesPd) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.0120, 0.0090};
    const double cross = 0.0060;
    auto blk = makeHarvestBlock(cdiag, cross);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry i0 = makeCarry(3.10, cdiag[2], blk, 2);
    LocalCarry i1 = makeCarry(5.40, cdiag[3], blk, 3);

    const double q = 2.5e-3;      // the loader default, m² at L1 per epoch
    const double gap_ep = 1.0;

    IonoBlockFixture base_f, infl_f;
    auto base = base_f.make();
    auto infl = infl_f.make();
    ASSERT_TRUE(base.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                      LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                      LocalBlockCol{2, 3.05, &i0, cdiag[2], 30.0, 1.0, false, true},
                                      LocalBlockCol{3, 5.20, &i1, cdiag[3], 30.0, 1.0, false, true}}));
    ASSERT_TRUE(infl.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                      LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                      LocalBlockCol{2, 3.05, &i0, cdiag[2] + q * gap_ep,
                                                    30.0, 1.0, false, true},
                                      LocalBlockCol{3, 5.20, &i1, cdiag[3] + q * gap_ep,
                                                    30.0, 1.0, false, true}}));

    const Eigen::MatrixXd& Sb = base_f.R.pl_blk.Sigma;
    const Eigen::MatrixXd& Si = infl_f.R.pl_blk.Sigma;
    ASSERT_EQ(Sb.rows(), 4);
    ASSERT_EQ(Si.rows(), 4);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const bool iono_diag = (r == c) && (r >= 2);
            if (iono_diag) EXPECT_DOUBLE_EQ(Si(r, c), Sb(r, c) + q * gap_ep);
            else           EXPECT_EQ(Si(r, c), Sb(r, c));   // bitwise: nothing else moved
        }
    // PD preserved: the inflated Σ factorizes, and so does the base.
    EXPECT_EQ(Eigen::LLT<Eigen::MatrixXd>(Sb).info(), Eigen::Success);
    EXPECT_EQ(Eigen::LLT<Eigen::MatrixXd>(Si).info(), Eigen::Success);
    // The inflated prior is strictly WEAKER on the iono directions, never stronger.
    EXPECT_GT(Si(2, 2), Sb(2, 2));
    EXPECT_GT(Si(3, 3), Sb(3, 3));
}

// Gap scaling: q is PER EPOCH, so a four-epoch gap must land 4q on the diagonal —
// four times the inflation of a one-epoch gap, measured from the same base.
TEST(CarryIonoInBlockTest, GapScalingIsLinearInEpochsOnTheIonoDiagonal) {
    const std::vector<double> cdiag = {0.040, 0.0120};
    auto blk = makeHarvestBlock(cdiag, 0.0050);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry i0 = makeCarry(3.10, cdiag[1], blk, 1);
    const double q = 2.5e-3;

    auto sigmaFor = [&](double gap_ep) {
        auto fx = std::make_unique<IonoBlockFixture>();
        auto cb = fx->make();
        EXPECT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
                                        LocalBlockCol{1, 3.05, &i0, cdiag[1] + q * gap_ep,
                                                      30.0 * gap_ep, 1.0, false, true}}));
        const double s = fx->R.pl_blk.Sigma(1, 1);
        const double off = fx->R.pl_blk.Sigma(0, 1);
        EXPECT_DOUBLE_EQ(off, 0.0050);   // the cross term never scales with the gap
        return s;
    };
    const double s1 = sigmaFor(1.0);
    const double s4 = sigmaFor(4.0);
    EXPECT_DOUBLE_EQ(s1, cdiag[1] + q);
    EXPECT_DOUBLE_EQ(s4, cdiag[1] + 4.0 * q);
    EXPECT_NEAR((s4 - cdiag[1]) / (s1 - cdiag[1]), 4.0, 1e-12);
}

// =============================================================================
// (4) ABSOLUTE-value carry: the RHS is (value_abs − x_lin), so a moved
//     linearization point changes η by exactly −Δx_lin and nothing else.
// =============================================================================
//
// This is the frame decision the card makes explicit: network_ionosphere.csv writes
// x_r ALONE and is frame-broken, so the carry stashes x_lin + x_r.  If the carry
// held a correction instead, the same stored number would mean a different absolute
// ionosphere after the linearization point moved — and the test below would show the
// RHS unchanged across the move, which is exactly the defect.
TEST(CarryIonoInBlockTest, CarriedIonoValueIsAbsoluteSoEtaTracksAMovedXLin) {
    const std::vector<double> cdiag = {0.040, 0.0120};
    auto blk = makeHarvestBlock(cdiag, 0.0050);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    // ONE carried absolute slant iono, harvested last epoch as x_lin + x_r.
    const double value_abs = 3.10;
    LocalCarry i0 = makeCarry(value_abs, cdiag[1], blk, 1);

    auto rhsFor = [&](double x_lin_now) {
        auto fx = std::make_unique<IonoBlockFixture>();
        auto cb = fx->make();
        EXPECT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
                                        LocalBlockCol{1, x_lin_now, &i0, cdiag[1],
                                                      30.0, 1.0, false, true}}));
        // Undo the whitening to read the RAW row RHS of the iono row.
        const Eigen::MatrixXd L =
            Eigen::LLT<Eigen::MatrixXd>(fx->R.pl_blk.Sigma).matrixL();
        Eigen::VectorXd z(2);
        z(0) = cb.crows[0].y;
        z(1) = cb.crows[1].y;
        const Eigen::VectorXd raw = L * z;
        return raw(1);
    };
    const double x0 = 3.05, x1 = 3.42;             // the state re-linearized by +0.37 m
    const double r0 = rhsFor(x0);
    const double r1 = rhsFor(x1);
    // η_raw = (value_abs − x_lin) + M·ŝ_prev, so the move shows up exactly once.
    EXPECT_NEAR(r0, (value_abs - x0) + 0.8 * 1.5, 1e-12);
    EXPECT_NEAR(r1, (value_abs - x1) + 0.8 * 1.5, 1e-12);
    EXPECT_NEAR(r1 - r0, -(x1 - x0), 1e-12);
    EXPECT_GT(std::abs(r1 - r0), 1e-6);            // it MOVED: not a stored correction
}

// =============================================================================
// (5) Block failure ⇒ nothing buffered.  There is no diagonal iono row to fall
//     back to, so "not folded" is the honest outcome and the driver counts it.
// =============================================================================
TEST(CarryIonoInBlockTest, NonSpdBlockBuffersNothingAndTheIonoSimplyDoesNotFold) {
    // A cross term larger than the geometric mean of the diagonals makes Σ
    // indefinite; the ridge retry cannot rescue it.
    const std::vector<double> cdiag = {0.040, 0.0120};
    auto blk = makeHarvestBlock(cdiag, /*offdiag=*/0.50);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry i0 = makeCarry(3.10, cdiag[1], blk, 1);

    IonoBlockFixture f;
    auto cb = f.make();
    const bool ok = cb.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                        LocalBlockCol{1, 3.05, &i0, cdiag[1], 30.0, 1.0,
                                                      false, true}});
    EXPECT_FALSE(ok);                      // driver: ++R.pl_blk_fallback, iono NOT folded
    EXPECT_TRUE(cb.crows.empty());         // nothing buffered ⇒ no double inject
    EXPECT_EQ(f.R.pl_blk_blocks, 0);
    EXPECT_EQ(f.R.pl_blk_rows, 0);
    // The iono column was ADMITTED (it passed the scalar screen) even though the
    // factorization then failed — the counter must say so, otherwise the not-folded
    // rate cannot be read against the admission rate.
    EXPECT_EQ(f.R.pl_blk_iono_cols, 1);
}

// =============================================================================
// (6) An iono entry with no harvest block zeroes its pairs, is counted separately,
//     and the block still builds.
// =============================================================================
//
// The NaN-pair rule: a pair whose two entries were not stashed at the SAME harvest
// (pointer-unequal PlHarvestBlock, or one entry with no block at all) has no valid
// cross term, so it is ZEROED, never guessed.  Σ stays block-diagonal by harvest
// stamp and therefore PD.  Splitting the iono's share out of pl_blk_pair_missing is
// what makes "the carried iono is not actually coupled to anything" — i.e. silently
// back to a diagonal prior, the falsified vehicle — visible instead of hidden.
TEST(CarryIonoInBlockTest, IonoWithoutAHarvestBlockZeroesItsPairsAndIsCountedSeparately) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    LocalCarry i0 = makeCarry(3.10, 0.0120, /*blk=*/nullptr, /*idx=*/-1);   // failed harvest

    IonoBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
                                    LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                    LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
                                    LocalBlockCol{3, 3.05,  &i0, 0.0120,   30.0, 1.0,
                                                  false, true}}));
    ASSERT_EQ(cb.crows.size(), 4u);              // the block STILL builds — 4 rows
    EXPECT_EQ(f.R.pl_blk_iono_cols, 1);
    EXPECT_EQ(f.R.pl_blk_iono_missing_cross, 3); // iono ↔ each of the 3 ambiguities
    EXPECT_EQ(f.R.pl_blk_pair_missing, 3);

    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 4);
    EXPECT_DOUBLE_EQ(S(0, 1), 0.0060);
    EXPECT_DOUBLE_EQ(S(3, 3), 0.0120);
    for (int k = 0; k < 3; ++k) {
        EXPECT_DOUBLE_EQ(S(3, k), 0.0);
        EXPECT_DOUBLE_EQ(S(k, 3), 0.0);
    }

    // A pair that is BOTH (iono, zwd) counts in BOTH splits — neither counter claims
    // to partition pl_blk_pair_missing, and a reader must not assume they sum.
    auto other = makeHarvestBlock({0.0090}, 0.0);
    LocalCarry zw = makeCarry(0.16, 0.0090, other, 0);   // different harvest ⇒ pointer-unequal
    IonoBlockFixture g;
    auto cb2 = g.make();
    ASSERT_TRUE(cb2.buildLocalBlock({LocalBlockCol{0, 3.05, &i0, 0.0120, 30.0, 1.0, false, true},
                                     LocalBlockCol{1, 0.15, &zw, 0.0090, 30.0, 1.0, true}}));
    EXPECT_EQ(g.R.pl_blk_pair_missing, 1);
    EXPECT_EQ(g.R.pl_blk_iono_missing_cross, 1);
    EXPECT_EQ(g.R.pl_blk_zwd_missing_cross, 1);
    EXPECT_DOUBLE_EQ(g.R.pl_blk.Sigma(0, 1), 0.0);
}

// =============================================================================
// (7) The T4 erase edge: the key shape NetworkRaim uses must address exactly one
//     (station, satellite) entry of the in-block map.
// =============================================================================
//
// Invariant #1's wired response for this zero-lag carry: a T4/consensus arc refloat
// erases the LOS's in-block iono carry, because the T4 statistic reads level error
// and carried-iono error as a SUM and cannot separate them.  The seam itself lives
// in NetworkRaim; what is testable — and what would actually break silently — is the
// KEY SHAPE: {station index, satellite}, per-satellite and NOT per-signal, so one
// erase clears the LOS once no matter how many bands tripped, and a sibling
// satellite (or the same satellite at another station) survives.
TEST(CarryIonoInBlockTest, T4EraseKeyClearsExactlyOneLosAndSparesSiblings) {
    NetworkCarryState carry;
    auto put = [&](int sta, const std::string& sat, double v) {
        carry.iono_blk_carry[{sta, sat}] = LocalCarry{v, 0.0120, 100.0};
    };
    put(0, "G01", 3.10);
    put(0, "G02", 4.20);
    put(1, "G01", 2.90);
    ASSERT_EQ(carry.iono_blk_carry.size(), 3u);

    // The NetworkRaim T4 site's expression, verbatim in shape: erase({i, pa.sat}).
    const std::size_t erased = carry.iono_blk_carry.erase({0, std::string("G01")});
    EXPECT_EQ(erased, 1u);                                   // one counted erase
    EXPECT_EQ(carry.iono_blk_carry.count({0, std::string("G01")}), 0u);
    EXPECT_EQ(carry.iono_blk_carry.count({0, std::string("G02")}), 1u);  // sibling sat
    EXPECT_EQ(carry.iono_blk_carry.count({1, std::string("G01")}), 1u);  // other station
    // Idempotent across the per-signal loop: a second band's erase counts nothing.
    EXPECT_EQ(carry.iono_blk_carry.erase({0, std::string("G01")}), 0u);
    // The legacy diagonal map is a DIFFERENT map — the two vehicles never share state.
    EXPECT_TRUE(carry.iono_carry.empty());
}

// =============================================================================
// (8) Loader interlocks.
// =============================================================================

TEST(CarryIonoInBlockTest, DefaultIsOffOnAnArmedF3Posture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(kF3Posture), cfg));
    EXPECT_FALSE(cfg.networkCarryIonoInBlock());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());   // the vehicle IS armed…
    EXPECT_FALSE(cfg.networkCarryIono());                // …and the legacy carry is off
}

TEST(CarryIonoInBlockTest, DisabledWhenTheF3BlockIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("noblock",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"carry_conditional_coupling\": true,\n"
                                       "    \"shared_carry_full_cov\": true,\n"
                                       "    \"carry_iono_in_block\": true,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLevelingLocalBlock());
    EXPECT_FALSE(cfg.networkCarryIonoInBlock());
}

TEST(CarryIonoInBlockTest, DisabledWhenPhaseLevelingIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nolevel",
                         networkConfig("    \"carry_conditional_coupling\": true,\n"
                                       "    \"shared_carry_full_cov\": true,\n"
                                       "    \"phase_leveling_local_block\": true,\n"
                                       "    \"carry_iono_in_block\": true,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLeveling());
    EXPECT_FALSE(cfg.networkCarryIonoInBlock());
}

// carry_invariant is a different carry VEHICLE and makes F3 structurally inert, so
// riding it is the armed-and-inert failure invariant #4 forbids.
TEST(CarryIonoInBlockTest, DisabledUnderCarryInvariant) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("ci",
                         networkConfig(std::string(kF3Posture)
                                       // carry_invariant is only DEFINED on the eq34 chain
                                       // datum (the loader hard-errors otherwise), so the
                                       // posture has to be legal before the interlock under
                                       // test can be the thing that fires.
                                       + "    \"carry_invariant\": true,\n"
                                         "    \"phase_datum_eq34\": true,\n"
                                         "    \"carry_iono_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryInvariant());
    EXPECT_FALSE(cfg.networkCarryIonoInBlock());
}

// F3 armed but structurally unable to build a block (no conditional coupling rows)
// is the same failure: the iono columns would have no block to join.
TEST(CarryIonoInBlockTest, DisabledWhenF3CannotBuildABlockOnThisPosture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inertf3",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"phase_leveling_local_block\": true,\n"
                                       "    \"carry_iono_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());        // armed…
    EXPECT_FALSE(cfg.networkCarryConditionalCoupling());      // …but inert
    EXPECT_FALSE(cfg.networkCarryIonoInBlock());
}

// THE MUTEX, and it is a HARD ERROR rather than a warn-and-disable: carry_iono is
// not a different setting of this mechanism, it is the DIAGONAL vehicle this card
// replaces (QUEUE 8(b), falsified ×4).  Running both would assert each LOS's iono
// direction a third time and no run-end counter could tell the vehicles apart.
TEST(CarryIonoInBlockTest, CoArmingTheLegacyDiagonalCarryIonoIsARefusedConfig) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("coarm",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"carry_iono\": true,\n"
                                          "    \"carry_iono_in_block\": true,\n"),
                          cfg));
}

TEST(CarryIonoInBlockTest, ArmedHappyPathResolvesQAndTheGapSentinel) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_iono_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryIonoInBlock());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());
    EXPECT_FALSE(cfg.networkCarryIono());                    // the legacy vehicle stays off
    // q default: m² at L1 PER EPOCH (σ ≈ 5 cm/epoch), NOT m²/s like zwd_carry_q.
    EXPECT_DOUBLE_EQ(cfg.networkCarryIonoInBlockQM2(), 2.5e-3);
    // The gap sentinel resolves to the leveling gap, so the iono carry can never
    // outlive the block it rides.
    EXPECT_DOUBLE_EQ(cfg.networkCarryIonoInBlockMaxGapS(),
                     cfg.networkPhaseLevelingMaxGapS());
    // And it does NOT subject the iono columns to the F3 young-column age gate.
    EXPECT_EQ(cfg.networkPhaseLevelingLocalBlockMinAgeEp(), 0);
}

TEST(CarryIonoInBlockTest, ExplicitQAndGapOverrideTheDefaults) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("override",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_iono_in_block\": true,\n"
                                         "    \"carry_iono_in_block_q_m2\": 0.01,\n"
                                         "    \"carry_iono_in_block_max_gap_s\": 240.0,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryIonoInBlock());
    EXPECT_DOUBLE_EQ(cfg.networkCarryIonoInBlockQM2(), 0.01);
    EXPECT_DOUBLE_EQ(cfg.networkCarryIonoInBlockMaxGapS(), 240.0);
    EXPECT_NE(cfg.networkCarryIonoInBlockMaxGapS(), cfg.networkPhaseLevelingMaxGapS());
}

TEST(CarryIonoInBlockTest, NegativeQIsARefusedConfig) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("negq",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"carry_iono_in_block\": true,\n"
                                          "    \"carry_iono_in_block_q_m2\": -1.0,\n"),
                          cfg));
}
