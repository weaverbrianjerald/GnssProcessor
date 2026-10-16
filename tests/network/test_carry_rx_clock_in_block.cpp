/**
 * @file test_carry_rx_clock_in_block.cpp
 * @brief Contract tests for @c network.carry_rx_clock_in_block: the station receiver
 *        clock (and its ISB columns) folded inside the F3 jointly-whitened station
 *        block as a random walk about a carried drift.  The @f$\rho@f$-direction
 *        sibling of @c carry_zwd_in_block and @c carry_iono_in_block
 *        (SPEC_amb_iono_split_overcount §7 item 17).
 *
 * Purpose: on the network producer the per-station receiver clock is a per-epoch
 * local column, a column of @f$A_r@f$ eliminated by the Schur reduction with no
 * temporal prior (@c filter.process_noise.clock_bias_q is inert on this path).  It
 * therefore absorbs whatever station-common error the levels and the ZWD do not.
 * Carrying it puts a prior on that direction.  The prior belongs inside the block
 * because the clock is the most station-common direction there is: a diagonal prior
 * on it would be the @f$n_{LOS}@f$ over-count that falsified the diagonal ZWD carry
 * (0.723 m against 0.088 m in-block on the quiet bed).
 *
 * The drift is the main design decision, and half of these cases cover it.  The
 * producer never writes its solution back into the station StateManager, so
 * @c x_lin(clock) is constant over the run and @f$x_r@f$ carries the entire clock,
 * including a ~0.3 m/s ramp (0.29994 m/s on the sim truth bed, against 0.299 m of
 * random-walk jitter), i.e. 9 m per 30 s epoch.  A bias-only random walk would inject
 * a mean error of ~30 σ every epoch (asserted below), which is why the carried state
 * is the (bias, drift) pair.
 *
 * A unit test reaches the seam the rx-clock carry acts on (@c CouplingRowBuilder::buildLocalBlock
 * over a real registered @c NetworkEstimator), the shared (bias, drift) kernel both
 * edges call (@c rxClockPredict / @c rxClockRateUpdate), the carry map's key shape,
 * and the loader's interlocks.  It does not reach the harvest (which appends the clock
 * columns to @c cols and writes their rows of @c PlHarvestBlock::C) or the PASS-B
 * driver's walk of @c getClockIndex / @c getSystemTimeOffsetStates; those are graded
 * on the sim fixture ladder.
 *
 * There is no diagonal clock row as a fallback, so none is tested.  A non-SPD block
 * means the clock carry did not act (counted in @c pl_blk_rxclk_not_folded), and the
 * contract is that the failed build buffers nothing.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/carry/Coupling.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/RunContext.hpp"
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
// Mirrors test_carry_zwd_in_block.cpp / test_carry_iono_in_block.cpp (one registered
// satellite clock + phase bias, layout frozen at beginEpoch).  It is a separate local
// copy so that this file owns its suite name and no file's cases can change another
// file's layout.
struct RxClkBlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    net::SatSignalTable tu_reinit_hist;   // the reinit history
    StationAssembly R;
    Eigen::Index clk_col{-1}, pb_col{-1};

    RxClkBlockFixture() {
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
                                  /*tu=*/false, /*gauge=*/false, /*reinit=*/false,
                                  tu_reinit_hist, R};
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

// --- the sim truth bed's measured receiver-clock model -----------------------
//
// runs/simnet60_quiet_wu93_24h/<sta>/truth_clock.csv, 2879 epochs at 30 s: a white
// random walk of 0.2988 m per epoch (lag-1 correlation −0.004) about a linear drift of
// 0.29994 m/s.  The defaults are set from these numbers, so they are recorded here to
// allow the tuning to be re-derived.
constexpr double kSimDriftMps   = 0.29994;   // m/s  ⇒ 8.998 m per 30 s epoch
constexpr double kSimRwSigmaM   = 0.2988;    // m per 30 s epoch
constexpr double kEpochS        = 30.0;

// --- loader fixture ----------------------------------------------------------

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("rxclkblk_cfg_" + tag + ".json");
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

/// The posture this feature is validated on: the leveling carry as the live vehicle,
/// folded as conditional coupling rows, with F3 armed.
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
// (1) On: k ambiguities + the ZWD + the receiver clock + an ISB build one
//     (k+1+2)-row whitened block whose Σ carries the harvested clock cross terms.
// =============================================================================
//
// The main claim: the receiver clock is coupled to the station's ambiguities
// (and to the ZWD, and to its ISB) through the harvested (N_rr⁻¹) terms rather than
// treated as independent.  Σ is checked directly, then the whitening is verified the
// way test_network_coupling checks an ambiguity-only block: L·(emitted) == raw.
TEST(CarryRxClockInBlockTest, ClockAndIsbJoinTheWhitenedBlockAndCarryTheirHarvestedCrossTerms) {
    RxClkBlockFixture f;
    // 3 ambiguity columns + the ZWD + the receiver clock + one ISB.  Block index order
    // mirrors the harvest's: ambiguities, iono (none here), clock family, ZWD last,
    // because the ZWD-in-block code computes its own row as C.rows()−1 and needs that slot.
    // Strictly diagonally dominant so Σ is PD by inspection: every off-diagonal is the
    // same harvested cross term and 5×cross < the smallest diagonal.  (A carried receiver
    // clock is well determined, σ ~10 cm against the station's whole code+phase set, so
    // a small clock diagonal is the physical case, not a convenience.)
    const std::vector<double> cdiag = {0.040, 0.030, 0.050, 0.0100, 0.0120, 0.0075};
    const double cross = 0.0010;                       // (N_rr⁻¹)_{c_i c_j}, all pairs
    auto blk = makeHarvestBlock(cdiag, cross);

    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    LocalCarry ck = makeCarry(29988.0, cdiag[3], blk, 3);   // ~100 µs of receiver clock
    LocalCarry ib = makeCarry(8.00,    cdiag[4], blk, 4);   // the sim's GPS→Galileo ISB
    LocalCarry zw = makeCarry(0.16,    cdiag[5], blk, 5);

    std::vector<LocalBlockCol> bc = {
        LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
        LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
        LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
        LocalBlockCol{3, 0.0,   &ck, cdiag[3], 30.0, 1.0, false, false,
                      /*is_rx_clk=*/true},
        LocalBlockCol{4, 0.0,   &ib, cdiag[4], 30.0, 1.0, false, false,
                      /*is_rx_clk=*/true, /*is_rx_isb=*/true},
        LocalBlockCol{5, 0.15,  &zw, cdiag[5], 30.0, 1.0, /*is_zwd=*/true},
    };

    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(bc));

    ASSERT_EQ(cb.crows.size(), 6u);
    for (const PendCplRow& row : cb.crows) {
        EXPECT_DOUBLE_EQ(row.w, 1.0);
        EXPECT_FALSE(row.local.empty());   // a block row (dense L⁻¹), not a scalar +1 row
    }
    EXPECT_EQ(cb.crows[3].local_col, 3);   // the clock row's primary column is its own
    EXPECT_EQ(cb.crows[4].local_col, 4);

    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 6);
    for (int k = 0; k < 6; ++k)
        EXPECT_DOUBLE_EQ(S(k, k), cdiag[static_cast<std::size_t>(k)]);
    // The clock family is coupled to every column the harvest saw: the ambiguities (the
    // station-common overlap this mechanism subtracts), the ZWD, and each other.
    for (int k = 0; k < 6; ++k)
        for (int j : {3, 4}) {
            if (k == j) continue;
            EXPECT_DOUBLE_EQ(S(j, k), cross);
            EXPECT_DOUBLE_EQ(S(k, j), cross);
        }
    EXPECT_EQ(f.R.pl_blk_rxclk_cols, 2);          // clock + ISB, counted exactly once each
    EXPECT_EQ(f.R.pl_blk_rxclk_isb_cols, 1);      // …of which one is the ISB (a sub-count)
    EXPECT_EQ(f.R.pl_blk_rxclk_missing_cross, 0);
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);            // the neighbouring column families are untouched
    EXPECT_EQ(f.R.pl_blk_iono_cols, 0);
    EXPECT_EQ(f.R.pl_blk_pair_missing, 0);
    EXPECT_EQ(f.R.pl_blk_blocks, 1);
    EXPECT_EQ(f.R.pl_blk_rows, 6);

    // The emitted rows are L⁻¹·[I | M | y] for this Σ: reconstruct L and undo it.  The
    // clock rows are built by the same ingredient path as an ambiguity row; the clock is
    // treated as another carried local column, and this identity checks that.
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
    EXPECT_LT((L * Hr - Hr_raw).cwiseAbs().maxCoeff(), 1e-9);
    EXPECT_LT((L * Hs - Hs_raw).cwiseAbs().maxCoeff(), 1e-9);
    EXPECT_LT((L * z  - z_raw ).cwiseAbs().maxCoeff(), 1e-9);
}

// is_rx_clk / is_rx_isb affect telemetry only.  If either entered the algebra, the
// clock would no longer be an ordinary carried local column and the exactness claim
// would need a new proof, so this pins that the whitened output is bit-identical with
// the flags set and cleared.
TEST(CarryRxClockInBlockTest, IsRxClkFlagChangesTelemetryOnlyNotTheAlgebra) {
    const std::vector<double> cdiag = {0.040, 0.0025};
    auto blk = makeHarvestBlock(cdiag, 0.0050);
    LocalCarry a0 = makeCarry(2.0,     cdiag[0], blk, 0);
    LocalCarry ck = makeCarry(29988.0, cdiag[1], blk, 1);

    RxClkBlockFixture flag_f, noflag_f;
    auto flag   = flag_f.make();
    auto noflag = noflag_f.make();
    ASSERT_TRUE(flag.buildLocalBlock(
        {LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
         LocalBlockCol{1, 0.0, &ck, cdiag[1], 30.0, 1.0, false, false, true}}));
    ASSERT_TRUE(noflag.buildLocalBlock(
        {LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
         LocalBlockCol{1, 0.0, &ck, cdiag[1], 30.0, 1.0, false, false, false}}));
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
    EXPECT_EQ(flag_f.R.pl_blk_rxclk_cols, 1);     // the only difference
    EXPECT_EQ(noflag_f.R.pl_blk_rxclk_cols, 0);
    EXPECT_EQ(flag_f.R.pl_blk_rxclk_isb_cols, 0); // …and the ISB sub-count stays 0 (no ISB flag)
}

// =============================================================================
// (2) Off: the block is k rows and no clock column exists.  Like the iono-in-block carry and
//     unlike the ZWD-in-block carry, there is no diagonal replacement row.
// =============================================================================
TEST(CarryRxClockInBlockTest, KnobOffLeavesKBlockRowsAndNoClockColumnAtAll) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);

    RxClkBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock({LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
                                    LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
                                    LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0}}));
    ASSERT_EQ(cb.crows.size(), 3u);                 // k rows, not k+1
    EXPECT_EQ(f.R.pl_blk.Sigma.rows(), 3);
    EXPECT_EQ(f.R.pl_blk_rxclk_cols, 0);
    EXPECT_EQ(f.R.pl_blk_rxclk_isb_cols, 0);
    EXPECT_EQ(f.R.pl_blk_rxclk_missing_cross, 0);
    EXPECT_EQ(f.R.pl_blk_rxclk_not_folded, 0);
    EXPECT_TRUE(f.R.pl_rxclk_blk_cols.empty());
    EXPECT_TRUE(f.R.pl_rxclk.empty());
}

// =============================================================================
// (3) q inflation lands on the clock diagonal only, and PD is preserved.
// =============================================================================
//
// The driver computes var_eff = var + q·gap_ep + rate_var·gap² (rxClockPredict) and
// passes it as LocalBlockCol::var_eff.  The contract pinned here is where it lands:
// on Σ's diagonal for that column and nowhere else.  The off-diagonals stay the
// harvested cross terms (a random walk has no cross term) and the ambiguity rows'
// entries are bitwise unchanged.
TEST(CarryRxClockInBlockTest, QInflationTouchesTheClockDiagonalOnlyAndPreservesPd) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.0100, 0.0120};
    const double cross = 0.0005;
    auto blk = makeHarvestBlock(cdiag, cross);
    LocalCarry a0 = makeCarry(2.00,    cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25,    cdiag[1], blk, 1);
    LocalCarry ck = makeCarry(29988.0, cdiag[2], blk, 2);
    LocalCarry ib = makeCarry(8.00,    cdiag[3], blk, 3);

    const double q = 0.09;        // the loader default, m² per epoch
    const double gap_ep = 1.0;

    RxClkBlockFixture base_f, infl_f;
    auto base = base_f.make();
    auto infl = infl_f.make();
    ASSERT_TRUE(base.buildLocalBlock(
        {LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
         LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
         LocalBlockCol{2, 0.0,  &ck, cdiag[2], 30.0, 1.0, false, false, true},
         LocalBlockCol{3, 0.0,  &ib, cdiag[3], 30.0, 1.0, false, false, true, true}}));
    ASSERT_TRUE(infl.buildLocalBlock(
        {LocalBlockCol{0, 0.5,  &a0, cdiag[0], 30.0, 1.0},
         LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
         LocalBlockCol{2, 0.0,  &ck, cdiag[2] + q * gap_ep, 30.0, 1.0,
                       false, false, true},
         LocalBlockCol{3, 0.0,  &ib, cdiag[3] + q * gap_ep, 30.0, 1.0,
                       false, false, true, true}}));

    const Eigen::MatrixXd& Sb = base_f.R.pl_blk.Sigma;
    const Eigen::MatrixXd& Si = infl_f.R.pl_blk.Sigma;
    ASSERT_EQ(Sb.rows(), 4);
    ASSERT_EQ(Si.rows(), 4);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const bool clk_diag = (r == c) && (r >= 2);
            if (clk_diag) EXPECT_DOUBLE_EQ(Si(r, c), Sb(r, c) + q * gap_ep);
            else          EXPECT_EQ(Si(r, c), Sb(r, c));   // bitwise: nothing else moved
        }
    EXPECT_EQ(Eigen::LLT<Eigen::MatrixXd>(Sb).info(), Eigen::Success);
    EXPECT_EQ(Eigen::LLT<Eigen::MatrixXd>(Si).info(), Eigen::Success);
    EXPECT_GT(Si(2, 2), Sb(2, 2));       // strictly weaker, never stronger
    EXPECT_GT(Si(3, 3), Sb(3, 3));
}

// Gap scaling: q is per epoch, so a four-epoch gap lands 4q on the diagonal.
TEST(CarryRxClockInBlockTest, GapScalingIsLinearInEpochsOnTheClockDiagonal) {
    const std::vector<double> cdiag = {0.040, 0.0025};
    auto blk = makeHarvestBlock(cdiag, 0.0020);
    LocalCarry a0 = makeCarry(2.00,    cdiag[0], blk, 0);
    LocalCarry ck = makeCarry(29988.0, cdiag[1], blk, 1);
    const double q = 0.09;

    auto sigmaFor = [&](double gap_ep) {
        auto fx = std::make_unique<RxClkBlockFixture>();
        auto cb = fx->make();
        EXPECT_TRUE(cb.buildLocalBlock(
            {LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
             LocalBlockCol{1, 0.0, &ck, cdiag[1] + q * gap_ep, 30.0 * gap_ep, 1.0,
                           false, false, true}}));
        const double s = fx->R.pl_blk.Sigma(1, 1);
        EXPECT_DOUBLE_EQ(fx->R.pl_blk.Sigma(0, 1), 0.0020);   // cross never scales
        return s;
    };
    const double s1 = sigmaFor(1.0);
    const double s4 = sigmaFor(4.0);
    EXPECT_DOUBLE_EQ(s1, cdiag[1] + q);
    EXPECT_DOUBLE_EQ(s4, cdiag[1] + 4.0 * q);
    EXPECT_NEAR((s4 - cdiag[1]) / (s1 - cdiag[1]), 4.0, 1e-12);
}

// =============================================================================
// (4) Absolute-value carry: the RHS is (value_abs − x_lin), so a moved
//     linearization point changes η by exactly −Δx_lin and nothing else.
// =============================================================================
//
// For the receiver clock this frame does not normally move: the producer never writes
// its solution back, so x_lin(clock) is constant over the run.  The test moves it
// anyway, because that constancy is a property of the current driver, not a guarantee.
// A later change that re-seeds the clock (or a warm resume that restores a state vector)
// must not silently re-interpret the carry.
TEST(CarryRxClockInBlockTest, CarriedClockValueIsAbsoluteSoEtaTracksAMovedXLin) {
    const std::vector<double> cdiag = {0.040, 0.0025};
    auto blk = makeHarvestBlock(cdiag, 0.0020);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    const double value_abs = 29988.0;                       // x_lin + x_r, absolute
    LocalCarry ck = makeCarry(value_abs, cdiag[1], blk, 1);

    auto rhsFor = [&](double x_lin_now) {
        auto fx = std::make_unique<RxClkBlockFixture>();
        auto cb = fx->make();
        EXPECT_TRUE(cb.buildLocalBlock(
            {LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
             LocalBlockCol{1, x_lin_now, &ck, cdiag[1], 30.0, 1.0,
                           false, false, true}}));
        const Eigen::MatrixXd L =
            Eigen::LLT<Eigen::MatrixXd>(fx->R.pl_blk.Sigma).matrixL();
        Eigen::VectorXd z(2);
        z(0) = cb.crows[0].y;
        z(1) = cb.crows[1].y;
        const Eigen::VectorXd raw = L * z;
        return raw(1);
    };
    const double x0 = 0.0, x1 = 29900.0;   // e.g. a cold_clock_seed landing mid-run
    const double r0 = rhsFor(x0);
    const double r1 = rhsFor(x1);
    EXPECT_NEAR(r0, (value_abs - x0) + 0.8 * 1.5, 1e-6);
    EXPECT_NEAR(r1, (value_abs - x1) + 0.8 * 1.5, 1e-9);
    EXPECT_NEAR(r1 - r0, -(x1 - x0), 1e-6);
    EXPECT_GT(std::abs(r1 - r0), 1e-6);    // it moved: not a stored correction
}

// =============================================================================
// (5) Block failure ⇒ nothing buffered.  There is no diagonal clock row to fall
//     back to, so the outcome is "not folded", and the driver counts it.
// =============================================================================
TEST(CarryRxClockInBlockTest, NonSpdBlockBuffersNothingAndTheClockSimplyDoesNotFold) {
    const std::vector<double> cdiag = {0.040, 0.0025};
    auto blk = makeHarvestBlock(cdiag, /*offdiag=*/0.50);   // indefinite Σ
    LocalCarry a0 = makeCarry(2.00,    cdiag[0], blk, 0);
    LocalCarry ck = makeCarry(29988.0, cdiag[1], blk, 1);

    RxClkBlockFixture f;
    auto cb = f.make();
    const bool ok = cb.buildLocalBlock(
        {LocalBlockCol{0, 0.5, &a0, cdiag[0], 30.0, 1.0},
         LocalBlockCol{1, 0.0, &ck, cdiag[1], 30.0, 1.0, false, false, true}});
    EXPECT_FALSE(ok);                      // driver: ++pl_blk_fallback, clock not folded
    EXPECT_TRUE(cb.crows.empty());         // nothing buffered ⇒ no double inject
    EXPECT_EQ(f.R.pl_blk_blocks, 0);
    EXPECT_EQ(f.R.pl_blk_rows, 0);
    // The clock column was admitted (it passed the scalar screen) even though the
    // factorization then failed.  The counter records the admission so that the
    // not-folded rate can be read against the admission rate.
    EXPECT_EQ(f.R.pl_blk_rxclk_cols, 1);
}

// =============================================================================
// (6) A clock entry with no harvest block zeroes its pairs and is counted separately.
// =============================================================================
//
// Counting the clock's share separately from pl_blk_pair_missing makes visible the
// case where the carried clock is coupled to nothing, which amounts to a diagonal
// prior on the most station-common direction.
TEST(CarryRxClockInBlockTest, ClockWithoutAHarvestBlockZeroesItsPairsAndIsCountedSeparately) {
    const std::vector<double> cdiag = {0.040, 0.030, 0.050};
    auto blk = makeHarvestBlock(cdiag, 0.0060);
    LocalCarry a0 = makeCarry(2.00, cdiag[0], blk, 0);
    LocalCarry a1 = makeCarry(2.25, cdiag[1], blk, 1);
    LocalCarry a2 = makeCarry(1.75, cdiag[2], blk, 2);
    LocalCarry ck = makeCarry(29988.0, 0.0025, /*blk=*/nullptr, /*idx=*/-1);

    RxClkBlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(
        {LocalBlockCol{0, 0.5,   &a0, cdiag[0], 30.0, 1.0},
         LocalBlockCol{1, -0.25, &a1, cdiag[1], 30.0, 1.0},
         LocalBlockCol{2, 1.0,   &a2, cdiag[2], 30.0, 1.0},
         LocalBlockCol{3, 0.0,   &ck, 0.0025,   30.0, 1.0, false, false, true}}));
    ASSERT_EQ(cb.crows.size(), 4u);               // the block still builds
    EXPECT_EQ(f.R.pl_blk_rxclk_cols, 1);
    EXPECT_EQ(f.R.pl_blk_rxclk_missing_cross, 3); // clock ↔ each of the 3 ambiguities
    EXPECT_EQ(f.R.pl_blk_pair_missing, 3);

    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    EXPECT_DOUBLE_EQ(S(0, 1), 0.0060);
    EXPECT_DOUBLE_EQ(S(3, 3), 0.0025);
    for (int k = 0; k < 3; ++k) {
        EXPECT_DOUBLE_EQ(S(3, k), 0.0);
        EXPECT_DOUBLE_EQ(S(k, 3), 0.0);
    }

    // A (clock, zwd) pair counts in both splits.  Neither counter partitions
    // pl_blk_pair_missing, so the splits do not sum to it.
    auto other = makeHarvestBlock({0.0090}, 0.0);
    LocalCarry zw = makeCarry(0.16, 0.0090, other, 0);   // different harvest ⇒ pointer-unequal
    RxClkBlockFixture g;
    auto cb2 = g.make();
    ASSERT_TRUE(cb2.buildLocalBlock(
        {LocalBlockCol{0, 0.0,  &ck, 0.0025, 30.0, 1.0, false, false, true},
         LocalBlockCol{1, 0.15, &zw, 0.0090, 30.0, 1.0, true}}));
    EXPECT_EQ(g.R.pl_blk_pair_missing, 1);
    EXPECT_EQ(g.R.pl_blk_rxclk_missing_cross, 1);
    EXPECT_EQ(g.R.pl_blk_zwd_missing_cross, 1);
    EXPECT_DOUBLE_EQ(g.R.pl_blk.Sigma(0, 1), 0.0);
}

// =============================================================================
// (7) The drift: the main design decision, tested at the kernel both edges
//     share (rxClockPredict / rxClockRateUpdate).
// =============================================================================

// With the sim bed's measured clock model, a bias-only random walk (carrying the clock
// like the ZWD) is ~30 σ off every epoch, while the (bias, drift) prediction is within
// 1 σ.  If this case fails because the drift term was dropped, the design is wrong, not
// the test.
TEST(CarryRxClockInBlockTest, PureRandomWalkIsThirtySigmaWrongOnThisDriftButTheDriftedOneIsNot) {
    const double q = 0.09;                       // σ 0.30 m per 30 s epoch
    const double var_prev = 1e-4;                // the clock is very well determined
    const double truth_step = kSimDriftMps * kEpochS;   // 8.998 m of real motion

    // (a) bias-only: rate = 0, and no rate variance to pay for it.
    const RxClkPred rw = rxClockPredict(/*value_abs=*/0.0, var_prev, /*rate=*/0.0,
                                        /*rate_var=*/0.0, kEpochS, 1.0, q);
    const double rw_sigma = (truth_step - rw.mean) / std::sqrt(rw.var);
    EXPECT_GT(rw_sigma, 25.0);                   // measured ≈ 30 σ
    EXPECT_LT(std::sqrt(rw.var), 0.35);          // …and it *claims* 0.30 m of accuracy

    // (b) the carried pair, drift converged: the prediction lands within the jitter.
    const double rate_var = 1e-6;                // a converged rate (σ 1 mm/s)
    const RxClkPred dp = rxClockPredict(0.0, var_prev, kSimDriftMps, rate_var,
                                        kEpochS, 1.0, q);
    EXPECT_NEAR(dp.mean, truth_step, 1e-9);
    EXPECT_LT(std::abs(truth_step - dp.mean) / std::sqrt(dp.var), 1.0);
    // The drift's own uncertainty is included in the injected variance.
    EXPECT_GT(dp.var, rw.var);
    EXPECT_NEAR(dp.var - rw.var, rate_var * kEpochS * kEpochS, 1e-15);
}

// An unknown drift injects a nearly free prior, never a stiff wrong one.  With the
// default rate_var0 = 1 m²/s², a 30 s prediction carries σ ≈ 30 m, about three times
// the per-epoch clock motion, so the prior constrains almost nothing until the rate
// filter has seen one increment.  This is the cold-start quarantine.
TEST(CarryRxClockInBlockTest, UnknownDriftInjectsANearFreePriorNotAStiffWrongOne) {
    const double q = 0.09, var_prev = 1e-4, rate_var0 = 1.0;
    const RxClkPred cold = rxClockPredict(0.0, var_prev, 0.0, rate_var0, kEpochS, 1.0, q);
    EXPECT_NEAR(std::sqrt(cold.var), 30.0, 0.1);
    // The real motion is within a third of a sigma, so the cold prior exerts almost no
    // pull on the clock.
    EXPECT_LT((kSimDriftMps * kEpochS) / std::sqrt(cold.var), 0.35);
}

// One increment collapses the rate prior: K ≈ 1, so the estimate jumps essentially to
// the observed rate and its variance to the increment's own noise.  This is why the
// cold prior above costs exactly one epoch.
TEST(CarryRxClockInBlockTest, RateFilterConvergesToTheObservedDriftAfterOneIncrement) {
    const double q = 0.09, var = 1e-4, dt = kEpochS;
    double rate = 0.0, rate_var = 1.0;                 // the uninformative prior
    const double obs_rate = kSimDriftMps;
    const double r_rate = (var + var + q) / (dt * dt); // ≈ 1.0e-4 m²/s²
    ASSERT_TRUE(rxClockRateUpdate(rate, rate_var, obs_rate, r_rate, /*rate_q_ep=*/1e-10));
    EXPECT_NEAR(rate, obs_rate, 1e-4);                 // K ≈ 0.9999
    EXPECT_LT(rate_var, 2.0 * r_rate);
    // …and the prior it now injects is the RW jitter, not 30 m.
    const RxClkPred warm = rxClockPredict(0.0, var, rate, rate_var, dt, 1.0, q);
    EXPECT_LT(std::sqrt(warm.var), 0.45);
    EXPECT_GT(std::sqrt(warm.var), kSimRwSigmaM);      // never tighter than the true q

    // A second increment tightens it further and tracks a genuinely changing drift.
    double rate2 = rate, rv2 = rate_var;
    ASSERT_TRUE(rxClockRateUpdate(rate2, rv2, obs_rate + 0.01, r_rate, 1e-10));
    EXPECT_LT(rv2, rate_var);
    EXPECT_GT(rate2, rate);
    EXPECT_LT(rate2, obs_rate + 0.01);                 // …but does not chase it in one step

    // Degenerate inputs are rejected and leave the state unchanged.
    double r3 = rate2, v3 = rv2;
    EXPECT_FALSE(rxClockRateUpdate(r3, v3, std::numeric_limits<double>::quiet_NaN(),
                                   r_rate, 1e-10));
    EXPECT_EQ(r3, rate2);
    EXPECT_EQ(v3, rv2);
}

// The clock-jump gate (invariant #1's wired response, and the only jump detector for
// the network's receiver clock).  A 1 ms receiver step trips it by orders of
// magnitude; ordinary random-walk jitter does not trip it.
TEST(CarryRxClockInBlockTest, JumpGateCatchesAMillisecondStepAndIgnoresOrdinaryJitter) {
    const double q = 0.09, var = 1e-4, k = 10.0;       // the defaults
    const double rate_var = 1e-6;                      // a converged drift
    const RxClkPred p = rxClockPredict(/*value_abs=*/0.0, var, kSimDriftMps, rate_var,
                                       kEpochS, 1.0, q);
    const double sigma = std::sqrt(p.var);

    // (a) an ordinary epoch: real motion + one RW increment of jitter.
    const double quiet = kSimDriftMps * kEpochS + kSimRwSigmaM;
    EXPECT_LT(std::abs(quiet - p.mean), k * sigma);
    // …and even a 3-sigma jitter epoch stays well inside the gate.
    EXPECT_LT(std::abs((kSimDriftMps * kEpochS + 3.0 * kSimRwSigmaM) - p.mean), k * sigma);

    // (b) a 1 ms receiver clock step = 299 792.458 m.  Field receivers do this.
    const double jumped = kSimDriftMps * kEpochS + 299792.458;
    EXPECT_GT(std::abs(jumped - p.mean), k * sigma);
    EXPECT_GT(std::abs(jumped - p.mean) / sigma, 1e5);   // it is not marginal

    // (c) the gate scales with the gate constant, so a lineage can loosen it.  A tiny
    //     k would erase the carry on ordinary jitter, which is why the loader refuses
    //     k <= 0.
    EXPECT_LT(std::abs(quiet - p.mean), 1000.0 * sigma);
    EXPECT_GT(std::abs(quiet - p.mean), 0.0);
}

// A gap is not a jump: over a long gap the prediction band grows with the gap squared
// (the drift's uncertainty) plus q per epoch, so a station that drifted for ten epochs
// is not mistaken for a stepped receiver.  This keeps the gap response (drop) and the
// jump response (erase + rebirth) from firing on each other's events.
TEST(CarryRxClockInBlockTest, PredictionBandGrowsWithTheGapSoAGapIsNotReadAsAJump) {
    const double q = 0.09, var = 1e-4, rate_var = 1e-6;
    const RxClkPred one = rxClockPredict(0.0, var, kSimDriftMps, rate_var, kEpochS, 1.0, q);
    const RxClkPred ten = rxClockPredict(0.0, var, kSimDriftMps, rate_var,
                                         10.0 * kEpochS, 10.0, q);
    EXPECT_GT(ten.var, one.var);
    EXPECT_NEAR(ten.mean, 10.0 * one.mean, 1e-9);            // the mean extrapolates…
    // …and the variance carries both terms: 10q from the bias walk and 100·rate_var·Δt²
    // from the drift.
    EXPECT_NEAR(ten.var, var + 10.0 * q + rate_var * 100.0 * kEpochS * kEpochS, 1e-12);
    // Ten epochs of real random walk is ~√10 σ, well inside a 10 σ gate.
    EXPECT_LT(std::sqrt(10.0) * kSimRwSigmaM, 10.0 * std::sqrt(ten.var));
}

// =============================================================================
// (8) The carry map's key shape: {station index, kind}, with "" the primary clock
//     and the system name for each ISB.
// =============================================================================
//
// A key-shape error would fail silently.  The jump response erases {i, ""} and leaves
// the station's ISBs in place (an ISB does not step when the clock does: the receiver's
// step is common to every system, so the GPS-referenced ISB is unchanged), and the gap
// prune addresses the whole family per station.
TEST(CarryRxClockInBlockTest, CarryKeyAddressesTheClockAndEachIsbSeparately) {
    NetworkCarryState carry;
    net::NetworkEntities ent;   // the station carries
    auto stash = [&](int sta, const std::string& kind, double v) {
        net::put(ent.stations, StationId{sta}, &net::StationState::signals, kind,
                 &net::StationSignalState::rx_clk) = LocalCarry{v, 1e-4, 100.0};
    };
    stash(0, "",         29988.0);
    stash(0, "Galileo",      8.0);
    stash(1, "",         29991.0);
    ASSERT_EQ(net::present(ent.stations, &net::StationState::signals,
                           &net::StationSignalState::rx_clk), 3u);

    // The jump site's expression, verbatim in shape.
    const std::size_t erased = net::drop(ent.stations, StationId{0}, &net::StationState::signals,
                                         std::string(), &net::StationSignalState::rx_clk);
    EXPECT_EQ(erased, 1u);
    EXPECT_EQ(static_cast<std::size_t>(net::get(ent.stations, StationId{0}, &net::StationState::signals,
                                                std::string(), &net::StationSignalState::rx_clk) != nullptr), 0u);
    EXPECT_EQ(static_cast<std::size_t>(net::get(ent.stations, StationId{0}, &net::StationState::signals,
                                                std::string("Galileo"), &net::StationSignalState::rx_clk) != nullptr), 1u);  // ISB survives
    EXPECT_EQ(static_cast<std::size_t>(net::get(ent.stations, StationId{1}, &net::StationState::signals,
                                                std::string(), &net::StationSignalState::rx_clk) != nullptr), 1u);           // other station
    EXPECT_EQ(net::drop(ent.stations, StationId{0}, &net::StationState::signals, std::string(),
                        &net::StationSignalState::rx_clk), 0u);           // idempotent

    // The drift map is keyed identically but populated only for the primary clock.
    const std::pair<int, std::string> k1{1, std::string()};
    net::put(ent.stations, StationId{k1.first}, &net::StationState::signals, k1.second,
             &net::StationSignalState::rx_clk_rate).rate = kSimDriftMps;
    EXPECT_EQ(static_cast<std::size_t>(net::get(ent.stations, StationId{1}, &net::StationState::signals,
                                                std::string("Galileo"), &net::StationSignalState::rx_clk_rate) != nullptr), 0u);
    EXPECT_DOUBLE_EQ(net::put(ent.stations, StationId{k1.first}, &net::StationState::signals, k1.second,
                              &net::StationSignalState::rx_clk_rate).rate, kSimDriftMps);
    // A fresh entry means "no rate yet": rate 0 with a negative variance sentinel, so
    // the injector substitutes carry_rx_clock_rate_var0 rather than a stiff zero.
    const auto fresh = net::put(ent.stations, StationId{0}, &net::StationState::signals, std::string(),
                                &net::StationSignalState::rx_clk_rate);
    EXPECT_DOUBLE_EQ(fresh.rate, 0.0);
    EXPECT_LT(fresh.rate_var, 0.0);
    EXPECT_EQ(fresh.n_incr, 0);
    EXPECT_LT(fresh.prev_s, 0.0);

    // The neighbouring carries are separate maps; no vehicle shares state.
    EXPECT_TRUE((net::present(ent.los, &net::LosState::iono_blk) == 0));
    EXPECT_EQ(net::present(ent.stations, &net::StationState::zwd), 0u);
}

// =============================================================================
// (9) Loader interlocks.
// =============================================================================

TEST(CarryRxClockInBlockTest, DefaultIsOffOnAnArmedF3Posture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(kF3Posture), cfg));
    EXPECT_FALSE(cfg.networkCarryRxClockInBlock());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());   // the vehicle is armed…
}

TEST(CarryRxClockInBlockTest, DisabledWhenTheF3BlockIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("noblock",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"carry_conditional_coupling\": true,\n"
                                       "    \"shared_carry_full_cov\": true,\n"
                                       "    \"carry_rx_clock_in_block\": true,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLevelingLocalBlock());
    EXPECT_FALSE(cfg.networkCarryRxClockInBlock());
}

TEST(CarryRxClockInBlockTest, DisabledWhenPhaseLevelingIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nolevel",
                         networkConfig("    \"carry_conditional_coupling\": true,\n"
                                       "    \"shared_carry_full_cov\": true,\n"
                                       "    \"phase_leveling_local_block\": true,\n"
                                       "    \"carry_rx_clock_in_block\": true,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLeveling());
    EXPECT_FALSE(cfg.networkCarryRxClockInBlock());
}

// carry_invariant is a different carry vehicle and makes F3 structurally inert, so
// arming the clock carry on it would be the armed-and-inert failure of invariant #4.
TEST(CarryRxClockInBlockTest, DisabledUnderCarryInvariant) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("ci",
                         networkConfig(std::string(kF3Posture)
                                       // carry_invariant is defined only on the eq34 chain
                                       // datum (the loader hard-errors otherwise), so the
                                       // posture is made legal first; the interlock under
                                       // test is then what fires.
                                       + "    \"carry_invariant\": true,\n"
                                         "    \"phase_datum_eq34\": true,\n"
                                         "    \"carry_rx_clock_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryInvariant());
    EXPECT_FALSE(cfg.networkCarryRxClockInBlock());
}

// A posture with F3 armed but unable to build a block (no conditional coupling rows) is
// the same failure: the clock column would have no block to join.
TEST(CarryRxClockInBlockTest, DisabledWhenF3CannotBuildABlockOnThisPosture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inertf3",
                         networkConfig("    \"phase_leveling\": true,\n"
                                       "    \"phase_leveling_local_block\": true,\n"
                                       "    \"carry_rx_clock_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());        // armed…
    EXPECT_FALSE(cfg.networkCarryConditionalCoupling());      // …but inert
    EXPECT_FALSE(cfg.networkCarryRxClockInBlock());
}

TEST(CarryRxClockInBlockTest, ArmedHappyPathResolvesTheVarianceLadderAndTheGapSentinel) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_rx_clock_in_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryRxClockInBlock());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());
    // q default: m² per epoch (σ 0.30 m/epoch, the sim bed's measured RW), not m²/s
    // as for zwd_carry_q.
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockQM2(), 0.09);
    EXPECT_NEAR(std::sqrt(cfg.networkCarryRxClockQM2()), kSimRwSigmaM, 2e-3);
    // The gap sentinel resolves to the leveling gap, so the clock carry can never
    // outlive the block it rides.
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockMaxGapS(), cfg.networkPhaseLevelingMaxGapS());
    // The drift ladder.
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockRateVar0(), 1.0);
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockRateQ(), 1e-10);
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockJumpK(), 10.0);
    // The ISB sub-knob is on by default: leaving the ISB white while carrying the clock
    // would move the unconstrained direction to the ISB column.
    EXPECT_TRUE(cfg.networkCarryRxClockIsb());
}

TEST(CarryRxClockInBlockTest, ExplicitLadderValuesOverrideTheDefaults) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("override",
                         networkConfig(std::string(kF3Posture)
                                       + "    \"carry_rx_clock_in_block\": true,\n"
                                         "    \"carry_rx_clock_q_m2\": 0.25,\n"
                                         "    \"carry_rx_clock_max_gap_s\": 240.0,\n"
                                         "    \"carry_rx_clock_rate_var0_m2ps2\": 4.0,\n"
                                         "    \"carry_rx_clock_rate_q_m2ps2\": 1e-8,\n"
                                         "    \"carry_rx_clock_jump_k\": 6.0,\n"
                                         "    \"carry_rx_clock_isb\": false,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkCarryRxClockInBlock());
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockQM2(), 0.25);
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockMaxGapS(), 240.0);
    EXPECT_NE(cfg.networkCarryRxClockMaxGapS(), cfg.networkPhaseLevelingMaxGapS());
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockRateVar0(), 4.0);
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockRateQ(), 1e-8);
    EXPECT_DOUBLE_EQ(cfg.networkCarryRxClockJumpK(), 6.0);
    EXPECT_FALSE(cfg.networkCarryRxClockIsb());
}

// The variance ladder all lands on a Cholesky diagonal, so a negative value would look
// like a data problem several seams away from the typo that caused it.  Refused at load.
TEST(CarryRxClockInBlockTest, NegativeVarianceLadderValuesAreRefusedConfigs) {
    for (const char* bad : {"    \"carry_rx_clock_q_m2\": -1.0,\n",
                            "    \"carry_rx_clock_rate_var0_m2ps2\": -1.0,\n",
                            "    \"carry_rx_clock_rate_q_m2ps2\": -1e-9,\n",
                            "    \"carry_rx_clock_max_gap_s\": -30.0,\n"}) {
        GnssConfig cfg;
        EXPECT_FALSE(loadInto("negladder",
                              networkConfig(std::string(kF3Posture)
                                            + "    \"carry_rx_clock_in_block\": true,\n"
                                            + bad),
                              cfg)) << "accepted: " << bad;
    }
}

// jump_k = 0 would erase the carry every epoch (|innov| > 0 always), leaving an armed
// but permanently inert channel, so the loader refuses it instead of logging it.
TEST(CarryRxClockInBlockTest, ZeroJumpGateIsARefusedConfig) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zerok",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"carry_rx_clock_in_block\": true,\n"
                                          "    \"carry_rx_clock_jump_k\": 0.0,\n"),
                          cfg));
}

// The ladder is validated even on a disabled arm, so a typo is not hidden by an
// interlock that happens to fire first.
TEST(CarryRxClockInBlockTest, TheLadderIsValidatedEvenWhenTheKnobIsOff) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("offbadq",
                          networkConfig(std::string(kF3Posture)
                                        + "    \"carry_rx_clock_q_m2\": -1.0,\n"),
                          cfg));
}
