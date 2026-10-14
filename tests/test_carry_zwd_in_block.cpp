/**
 * @file test_carry_zwd_in_block.cpp
 * @brief Contract tests for @c network.carry_zwd_in_block: the B9 ZWD carry folded
 *        inside the F3 jointly-whitened station block instead of as a diagonal
 *        coupling row outside it (SPEC_amb_iono_split_overcount §7 item 17,
 *        "block all carried local columns").
 *
 * Rationale: a diagonal B9 row asserts the station's ZWD direction a second time,
 * on top of the assertion every carried ambiguity row of the same station already
 * makes implicitly.  That station-common over-count is what falsified
 * @c network.carry_zwd on the quiet 60-station truth bed.  Inside the block the
 * joint Σ carries the harvested @f$(N_{rr}^{-1})_{c\,c_j}@f$ cross terms, so the
 * overlap is subtracted exactly rather than counted twice.
 *
 * A unit test reaches the seam the card acts on
 * (@c CouplingRowBuilder::buildLocalBlock over a real registered
 * @c NetworkEstimator) and the loader's interlocks.  It does not reach the harvest
 * (which appends the ZWD column to @c cols and writes row n of
 * @c PlHarvestBlock::C) or the PASS-B driver (which pushes the @c LocalBlockCol and
 * counts @c pl_blk_zwd_fallback_diag); those are graded on the sim fixture ladder.
 * The driver's fallback contract is pinned here: a non-SPD block buffers nothing,
 * so the caller emits the B9 diagonal row instead.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkEstimator.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

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
// Mirrors test_network_coupling.cpp's BuilderFixture (one registered satellite
// clock + phase bias, layout frozen at beginEpoch).  A separate local copy, so
// this file owns its suite name and neither file's cases can change the other's
// layout.
struct ZwdBlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1};

    ZwdBlockFixture() {
        clk_col = est.registerClock("G01");
        pb_col  = est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 8);   // 8 local columns is plenty for these cases
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make() {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  /*joint=*/false, /*tu=*/false, /*shmarg=*/false,
                                  /*gauge=*/false, /*reinit=*/false, tu_reinit_hist, R};
    }
};

/// One station-epoch's harvest block over @p n columns: a hand-built SPD
/// conditional covariance standing in for the (N_rr⁻¹) sub-matrix the production
/// harvest gathers.  Diagonally dominant so every principal sub-matrix is SPD too
/// (the marginalization rule buildLocalBlock relies on).
std::shared_ptr<PlHarvestBlock> makeHarvestBlock(const std::vector<double>& diag,
                                                 double offdiag) {
    auto blk = std::make_shared<PlHarvestBlock>();
    const Eigen::Index n = static_cast<Eigen::Index>(diag.size());
    blk->C = Eigen::MatrixXd::Constant(n, n, offdiag);
    for (Eigen::Index k = 0; k < n; ++k) blk->C(k, k) = diag[static_cast<std::size_t>(k)];
    blk->stamp_s = 1000.0;
    return blk;
}

/// A carried local column pointing at @p blk index @p idx, with one present shared
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
    const fs::path p = fs::temp_directory_path() / ("zwdblk_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p extra is spliced into the network
/// block (the pattern of the zwd_seam_reseed and iono-carry config tests).
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
// (1) On: k ambiguity columns plus the ZWD build a (k+1)-row whitened block whose
//     Σ carries the harvested ZWD cross terms.
// =============================================================================
//
// The direct assertion is on Σ itself (retained as R.pl_blk.Sigma): its last row
// and column are the harvest block's ZWD row, not zero, so the ZWD is coupled to
// the ambiguities.  The whitening is then checked the way test_network_coupling
// checks the ambiguity-only block: L·(emitted) == raw, i.e. the emitted rows are
// L⁻¹·[I | M | y] for this Σ.
TEST(CarryZwdInBlockTest, ZwdColumnJoinsTheWhitenedBlockAndCarriesItsHarvestedCrossTerms) {
    ZwdBlockFixture f;
    // 3 ambiguity columns (a GPS triple LOS) + the ZWD at block index 3.
    const std::vector<double> cdiag = {0.040, 0.030, 0.050, 0.0090};   // C's diagonal = each column's cond var
    const double cross = 0.0060;                                       // (N_rr⁻¹)_{c_i c_j}, all pairs
    auto blk = makeHarvestBlock(cdiag, cross);

    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    LocalCarry zw = makeCarry(0.16, cdiag[3], blk, 3);   // ZWD ≈ 16 cm wet delay

    std::vector<LocalBlockCol> bc = {
        LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
        LocalBlockCol{2, 1.0,  &a2, cdiag[2], 30.0, 1.0},
        // The ZWD column: same struct, vce_fac 1.0, is_zwd = true (telemetry only).
        LocalBlockCol{3, 0.15, &zw, cdiag[3], 30.0, 1.0, /*is_zwd=*/true},
    };

    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(bc));

    // k + 1 = 4 whitened rows, one per column, all unit weight.
    ASSERT_EQ(cb.crows.size(), 4u);
    for (const PendCplRow& row : cb.crows) {
        EXPECT_DOUBLE_EQ(row.w, 1.0);
        EXPECT_FALSE(row.local.empty());   // a block row (dense L⁻¹), not a scalar +1 row
    }
    EXPECT_EQ(cb.crows[3].local_col, 3);   // the ZWD row's primary column is the ZWD column

    // Σ: 4×4, diagonal = each column's σ² (var_eff here, no departed/tu terms),
    // off-diagonals = the harvested cross terms, the ZWD's included.
    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 4);
    ASSERT_EQ(S.cols(), 4);
    for (int k = 0; k < 4; ++k)
        EXPECT_DOUBLE_EQ(S(k, k), cdiag[static_cast<std::size_t>(k)]);
    for (int k = 0; k < 3; ++k) {
        EXPECT_DOUBLE_EQ(S(3, k), cross);   // ZWD ↔ ambiguity k, from the harvest block
        EXPECT_DOUBLE_EQ(S(k, 3), cross);   // symmetric
    }
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);           // counted exactly once
    EXPECT_EQ(f.R.pl_blk_zwd_missing_cross, 0);  // every ZWD pair had a cross term
    EXPECT_EQ(f.R.pl_blk_pair_missing, 0);
    EXPECT_EQ(f.R.pl_blk_blocks, 1);
    EXPECT_EQ(f.R.pl_blk_rows, 4);

    // The emitted rows are L⁻¹·[I | M | y] for this Σ: reconstruct L and undo it.
    const Eigen::MatrixXd L = Eigen::LLT<Eigen::MatrixXd>(S).matrixL();
    Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(4, f.R.A_r.cols());
    Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(4, f.R.A_s.cols());
    Eigen::VectorXd z(4);
    for (int k = 0; k < 4; ++k) {
        for (const auto& lc : cb.crows[static_cast<std::size_t>(k)].local) Hr(k, lc.first) = lc.second;
        for (const auto& sc : cb.crows[static_cast<std::size_t>(k)].shared) Hs(k, sc.first) = sc.second;
        z(k) = cb.crows[static_cast<std::size_t>(k)].y;
    }
    // Raw: +1 on each column, the border regressor M_r, and the RHS
    // (value_abs − x_lin) + Σ M_rj·ŝ_prev,j.  The ZWD row is built by the same
    // ingredient path as an ambiguity row, and this check pins that identity.
    Eigen::MatrixXd Hr_raw = Eigen::MatrixXd::Zero(4, f.R.A_r.cols());
    Eigen::MatrixXd Hs_raw = Eigen::MatrixXd::Zero(4, f.R.A_s.cols());
    Eigen::VectorXd z_raw(4);
    for (int k = 0; k < 4; ++k) {
        Hr_raw(k, bc[static_cast<std::size_t>(k)].col) = 1.0;
        Hs_raw(k, f.clk_col) = 0.8;
        z_raw(k) = (bc[static_cast<std::size_t>(k)].lc->value_abs
                    - bc[static_cast<std::size_t>(k)].x_lin) + 0.8 * 1.5;
    }
    EXPECT_LT((L * Hr - Hr_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * Hs - Hs_raw).cwiseAbs().maxCoeff(), 1e-12);
    EXPECT_LT((L * z  - z_raw ).cwiseAbs().maxCoeff(), 1e-12);
}

// The is_zwd flag is telemetry only.  If it entered the algebra, the ZWD would no
// longer be an ordinary carried local column and the exactness claim would need a
// new proof, so the whitened output is pinned bit-identical with the flag set and
// cleared.
TEST(CarryZwdInBlockTest, IsZwdFlagChangesTelemetryOnlyNotTheAlgebra) {
    const std::vector<double> cdiag = {0.040, 0.0090};
    auto blk = makeHarvestBlock(cdiag, 0.0050);
    LocalCarry a0 = makeCarry(2.0, cdiag[0], blk, 0);
    LocalCarry zw = makeCarry(0.16, cdiag[1], blk, 1);

    ZwdBlockFixture flag_f, noflag_f;
    auto flag   = flag_f.make();
    auto noflag = noflag_f.make();
    ASSERT_TRUE(flag.buildLocalBlock({LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
                                      LocalBlockCol{1, 0.15, &zw, cdiag[1], 30.0, 1.0, true}}));
    ASSERT_TRUE(noflag.buildLocalBlock({LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
                                        LocalBlockCol{1, 0.15, &zw, cdiag[1], 30.0, 1.0, false}}));
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
    EXPECT_EQ(flag_f.R.pl_blk_zwd_cols, 1);     // the only difference
    EXPECT_EQ(noflag_f.R.pl_blk_zwd_cols, 0);
}

// =============================================================================
// (2) Off: the block is k rows and the ZWD is the diagonal B9 row, with the exact
//     ingredients of the unarmed path.
// =============================================================================
//
// Byte-identical ingredients are asserted bitwise (EXPECT_EQ on doubles): the off-path
// ZWD row's weight, RHS, primary column and
// border coefficient equal what buildCplRow emits standalone, and the row is a
// scalar row (empty `local`), not a block row.
TEST(CarryZwdInBlockTest, KnobOffLeavesKBlockRowsAndTheZwdAsTheDiagonalB9Row) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};   // 3 ambiguities; ZWD not in C
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    // Off: the ZWD carry is harvested without a block (the unarmed
    // `harvestCoupling(est_idx, R.pl_zwd.col, nullptr, nullptr)` call).
    LocalCarry zw = makeCarry(0.16, 0.0090, /*blk=*/nullptr, /*idx=*/-1);
    const double zvar_eff = 0.0090 + 1e-9 * 30.0;   // var + zwd_q·gap, the B9 expression

    ZwdBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
                                    LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                    LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0}}));
    ASSERT_EQ(cb.crows.size(), 3u);                 // k rows, not k+1
    EXPECT_EQ(f.R.pl_blk.Sigma.rows(), 3);
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 0);
    EXPECT_EQ(f.R.pl_blk_zwd_missing_cross, 0);

    // Then the B9 diagonal row, as ApplicationNetworkAssembly emits it.
    cb.buildCplRow(/*col=*/3, /*x_lin_val=*/0.15, zw, zvar_eff, /*gap=*/30.0);
    ASSERT_EQ(cb.crows.size(), 4u);
    const PendCplRow& zrow = cb.crows[3];
    EXPECT_TRUE(zrow.local.empty());                // scalar row: the implicit +1 at local_col
    EXPECT_EQ(zrow.local_col, 3);
    EXPECT_EQ(zrow.w, 1.0 / zvar_eff);              // bitwise: the legacy B9 expression
    EXPECT_EQ(zrow.y, (0.16 - 0.15) + 0.8 * 1.5);
    ASSERT_EQ(zrow.shared.size(), 1u);
    EXPECT_EQ(zrow.shared[0].first, f.clk_col);
    EXPECT_EQ(zrow.shared[0].second, 0.8);

    // The same row built by a builder that never saw a block is bit-identical, so
    // the fallback below degrades exactly to the unarmed B9 row.
    ZwdBlockFixture g;
    auto solo = g.make();
    solo.buildCplRow(/*col=*/3, /*x_lin_val=*/0.15, zw, zvar_eff, /*gap=*/30.0);
    ASSERT_EQ(solo.crows.size(), 1u);
    EXPECT_EQ(solo.crows[0].w, zrow.w);
    EXPECT_EQ(solo.crows[0].y, zrow.y);
    EXPECT_EQ(solo.crows[0].local_col, zrow.local_col);
    EXPECT_EQ(solo.crows[0].shared[0].second, zrow.shared[0].second);
}

// =============================================================================
// (3) Block failure ⇒ the diagonal fallback, with nothing buffered first.
// =============================================================================
//
// The driver's contract on a non-SPD block is to emit the per-column rows instead
// and count the fallback.  For the ZWD that means the B9 diagonal row.  The part a
// unit test can pin is that a failed build leaves `crows` untouched: partial rows
// buffered there would make the caller's fallback inject every column twice.
TEST(CarryZwdInBlockTest, NonSpdBlockBuffersNothingAndTheZwdTakesTheDiagonalFallback) {
    // A cross term larger than the geometric mean of the diagonals makes Σ
    // indefinite; the ridge retry cannot rescue it.
    const std::vector<double> cdiag = {0.040, 0.0090};
    auto blk = makeHarvestBlock(cdiag, /*offdiag=*/0.50);
    LocalCarry a0 = makeCarry(2.0,  cdiag[0], blk, 0);
    LocalCarry zw = makeCarry(0.16, cdiag[1], blk, 1);

    ZwdBlockFixture f;
    auto cb = f.make();
    const bool ok = cb.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                        LocalBlockCol{1, 0.15, &zw, cdiag[1], 30.0, 1.0, true}});
    EXPECT_FALSE(ok);                      // driver: ++R.pl_blk_fallback, per-column route
    EXPECT_TRUE(cb.crows.empty());         // nothing buffered ⇒ no double inject
    EXPECT_EQ(f.R.pl_blk_blocks, 0);
    EXPECT_EQ(f.R.pl_blk_rows, 0);
    // The ZWD column was admitted to the block (it passed the scalar screen) even
    // though the factorization then failed.  The counter records the admission, so
    // the fallback rate can be read against the admission rate.
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);

    // The caller's fallback: the ambiguity per-column row and the B9 diagonal ZWD
    // row.  Both are ordinary scalar rows, the same as the unarmed B9 path.
    const double zvar_eff = cdiag[1] + 1e-9 * 30.0;
    cb.buildCplRow(/*col=*/0, /*x_lin_val=*/0.5,  a0, cdiag[0], /*gap=*/30.0);
    cb.buildCplRow(/*col=*/1, /*x_lin_val=*/0.15, zw, zvar_eff, /*gap=*/30.0);
    ASSERT_EQ(cb.crows.size(), 2u);
    EXPECT_TRUE(cb.crows[0].local.empty());
    EXPECT_TRUE(cb.crows[1].local.empty());
    EXPECT_EQ(cb.crows[1].w, 1.0 / zvar_eff);
}

// =============================================================================
// (4) A missing ZWD cross term is counted separately, and the block still builds.
// =============================================================================
//
// The NaN-pair rule: a pair whose two entries were not stashed at the same harvest
// (pointer-unequal PlHarvestBlock, or one entry with no block at all, e.g. a ZWD
// restored on a warm resume) has no valid cross term, so it is set to zero, not
// estimated.  Σ stays block-diagonal by harvest stamp and therefore PD.  The ZWD's
// share of pl_blk_pair_missing is counted on its own, so a carried ZWD coupled to
// nothing shows up directly rather than inside the aggregate.
TEST(CarryZwdInBlockTest, ZwdWithoutAHarvestBlockZeroesItsPairsAndIsCountedSeparately) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    LocalCarry zw = makeCarry(0.16, 0.0090, /*blk=*/nullptr, /*idx=*/-1);   // restored / failed harvest

    ZwdBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
                                    LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                    LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
                                    LocalBlockCol{3, 0.15,  &zw, 0.0090,   30.0, 1.0, true}}));
    ASSERT_EQ(cb.crows.size(), 4u);            // the block still builds: 4 rows
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);
    EXPECT_EQ(f.R.pl_blk_zwd_missing_cross, 3);  // ZWD ↔ each of the 3 ambiguities
    EXPECT_EQ(f.R.pl_blk_pair_missing, 3);       // the ZWD's share is the whole total here

    // Σ: the ambiguity block keeps its harvested cross terms; every ZWD pair is 0.
    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 4);
    EXPECT_DOUBLE_EQ(S(0, 1), 0.0060);
    EXPECT_DOUBLE_EQ(S(3, 3), 0.0090);
    for (int k = 0; k < 3; ++k) {
        EXPECT_DOUBLE_EQ(S(3, k), 0.0);
        EXPECT_DOUBLE_EQ(S(k, 3), 0.0);
    }

    // A ZWD stashed at a different harvest (a second block object) falls under the
    // same rule through pointer inequality rather than a null pointer.
    auto other = makeHarvestBlock({0.0090}, 0.0);
    LocalCarry zw2 = makeCarry(0.16, 0.0090, other, 0);
    ZwdBlockFixture g;
    auto cb2 = g.make();
    ASSERT_TRUE(cb2.buildLocalBlock({LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
                                     LocalBlockCol{1, 0.15, &zw2, 0.0090, 30.0, 1.0, true}}));
    EXPECT_EQ(g.R.pl_blk_zwd_missing_cross, 1);
    EXPECT_DOUBLE_EQ(g.R.pl_blk.Sigma(0, 1), 0.0);
}

// =============================================================================
// (5) Loader interlocks, including the negative control: carry_zwd off ⇒ nothing.
// =============================================================================

TEST(CarryZwdInBlockTest, DefaultIsOffOnAnArmedF3Posture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(kF3Posture), cfg));
    EXPECT_FALSE(cfg.networkCarryZwdInBlock());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());   // the vehicle is armed…
    EXPECT_FALSE(cfg.networkCarryZwd());                 // …and B9 still defaults off
}

// Negative control: the sub-knob never creates a carry.  With carry_zwd off there
// is nothing to put in the block, so an armed sub-knob is disabled.  Armed-and-inert
// is not allowed, so a run-end "0 ZWD columns" means the mechanism found nothing.
TEST(CarryZwdInBlockTest, DisabledWhenCarryZwdIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nozwd",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_zwd\": false,\n"
                                         "    \"carry_zwd_in_block\": true,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkCarryZwdInBlock());
    EXPECT_FALSE(cfg.networkCarryZwd());
}

TEST(CarryZwdInBlockTest, DisabledWhenTheF3BlockIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("noblock",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"carry_conditional_coupling\": true,\n"
                                       "    \"shared_carry_full_cov\": true,\n"
                                       "    \"carry_zwd\": true,\n"
                                       "    \"carry_zwd_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryZwd());                   // the B9 carry itself is untouched…
    EXPECT_FALSE(cfg.networkPhaseLevelingLocalBlock());
    EXPECT_FALSE(cfg.networkCarryZwdInBlock());           // …the sub-knob is disabled
}

// An F3 vehicle that is armed but structurally unable to build a block (no
// conditional coupling rows) is the same armed-and-inert case: the ZWD column
// would have no block to join.
TEST(CarryZwdInBlockTest, DisabledWhenF3CannotBuildABlockOnThisPosture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inertf3",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"phase_leveling_local_block\": true,\n"
                                       "    \"carry_zwd\": true,\n"
                                       "    \"carry_zwd_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());        // armed…
    EXPECT_FALSE(cfg.networkCarryConditionalCoupling());      // …but inert
    EXPECT_FALSE(cfg.networkCarryZwdInBlock());
}

TEST(CarryZwdInBlockTest, ArmedHappyPathSurvivesLoadAndLeavesQAtItsDefault) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_zwd\": true,\n"
                                         "    \"carry_zwd_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryZwdInBlock());
    EXPECT_TRUE(cfg.networkCarryZwd());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());
    // The card changes the vehicle, not the process-noise model.
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 1e-9);
}

// The ledger's recommend branch (SPEC_amb_iono_split_overcount §6.16.7d arm F2):
// carry_zwd on, F3 on and the sub-knob off is the one combination the ledger calls a
// configuration error rather than a conservative choice, because the diagonal ZWD row
// inside an F3 station is the falsified double count.  The loader reports it and
// changes nothing: this test pins that the advisory has no side effect and does not
// act as a fourth disabling interlock alongside the ones above.
TEST(CarryZwdInBlockTest, DiagonalVehicleUnderF3IsAdvisedAgainstButNotAltered) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("recommend",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_zwd\": true,\n"
                                         "    \"carry_zwd_in_block\": false,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryZwd());                   // the diagonal B9 carry stands…
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());    // …on an armed F3 vehicle…
    EXPECT_FALSE(cfg.networkCarryZwdInBlock());           // …and nothing is auto-armed.
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 1e-9);
}
