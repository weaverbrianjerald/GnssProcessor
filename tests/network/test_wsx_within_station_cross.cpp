/**
 * @file test_wsx_within_station_cross.cpp
 * @brief Unit gates of card WSX (@c network.carry_coupling_exact_cross): the exact time
 *        update's bracket between two carried rows of a station, added in the station's
 *        whitened local block (docs/refound/WSX_within_station_cross_terms_2026-09-30.md).
 *
 * 1. With the knob, every pair of rows that both took the exact route gets
 *    @f$M_i^\top B M_k@f$ (@ref GnssProcessor::couplingExactCross over the rows'
 *    @ref GnssProcessor::couplingExactRow) added to @f$\Sigma_{ik}@f$ on top of the
 *    harvested conditional covariance; a row on card 1d's route (harvested before the
 *    stash) adds nothing; the diagonal is unchanged; the counters count it.
 * 2. Without it, @f$\Sigma@f$ is the per-row form, bitwise.
 * 3. The loader refuses the knob without the exact update.
 *
 * The comparison of the two forms against the exact batch is
 * @c CouplingExactUpdateTest.WithinStationCrossTermsAgainstTheBatch.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/carry/NetworkCoupling.hpp"
#include "network/carry/NetworkCouplingExact.hpp"
#include "network/solve/NetworkEstimator.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kStash = 1000.0;

/// Three registered border columns (G01 clock, G01 L1C phase bias, G02 clock), the layout
/// frozen at beginEpoch, and the exact epoch over the same three keys stashed at kStash.
struct WsxFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    net::SatSignalTable tu_reinit_hist;   // the reinit history (card NOM step N4d)
    StationAssembly R;
    CouplingExactEpoch ep;
    std::vector<std::string> keys;

    WsxFixture() {
        const Eigen::Index c1 = est.registerClock("G01");
        const Eigen::Index p1 = est.registerPhaseBias("G01", "L1C");
        const Eigen::Index c2 = est.registerClock("G02");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 10);
        R.w.resize(0);
        R.y.resize(0);
        keys = {sharedCarryKey(SharedParamKind::Clock, "G01", ""),
                sharedCarryKey(SharedParamKind::PhaseBias, "G01", "L1C"),
                sharedCarryKey(SharedParamKind::Clock, "G02", "")};
        ep.stash_s = kStash;
        ep.P.resize(3, 3);
        ep.P << 0.20, 0.15, 0.05,
                0.15, 0.18, 0.04,
                0.05, 0.04, 0.12;
        for (int p = 0; p < 3; ++p) ep.p_of_key[keys[static_cast<std::size_t>(p)]] = p;
        ep.s_of_p   = {0, 1, 2};
        ep.p_of_s   = {0, 1, 2};
        ep.route_p  = std::vector<int>(3, static_cast<int>(CouplingExactDRoute::Present));
        ep.clk_p    = {1, 0, 1};
        ep.cc_p     = {'G', 'G', 'G'};
        ep.col_of_s = {c1, p1, c2};
        ep.q_s.resize(3);
        ep.q_s << 0.03, 0.0, 0.03;   // a walk on both clocks, large enough to be seen
        ep.shat_s.resize(3);
        ep.shat_s << 1.5, 0.3, -0.7;
        EXPECT_TRUE(couplingExactFactor(ep));
    }

    CouplingRowBuilder make(bool cross) {
        CouplingRowBuilder cb{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                              /*tu=*/false, /*gauge=*/false, /*reinit=*/false,
                              tu_reinit_hist, R};
        cb.cxu       = &ep;
        cb.cxu_cross = cross;
        return cb;
    }
};

/// A hand-built SPD harvest block standing in for the (N_rr^-1) sub-matrix.
std::shared_ptr<PlHarvestBlock> harvestBlock(const std::vector<double>& diag, double off) {
    auto blk = std::make_shared<PlHarvestBlock>();
    const Eigen::Index n = static_cast<Eigen::Index>(diag.size());
    blk->C = Eigen::MatrixXd::Constant(n, n, off);
    for (Eigen::Index k = 0; k < n; ++k) blk->C(k, k) = diag[static_cast<std::size_t>(k)];
    blk->stamp_s = kStash;
    return blk;
}

/// A carried ambiguity with coupling terms on the three keys (coefficients @p m), seen at
/// @p seen_s (kStash: the exact route; earlier: card 1d's route).
LocalCarry carry(const WsxFixture& f, const std::vector<double>& m, double seen_s,
                 const std::shared_ptr<PlHarvestBlock>& blk, int idx) {
    LocalCarry lc;
    lc.value_abs   = 2.0 + 0.1 * idx;
    lc.var         = blk->C(idx, idx);
    lc.last_seen_s = seen_s;
    const SharedParamKind kinds[3] = {SharedParamKind::Clock, SharedParamKind::PhaseBias,
                                      SharedParamKind::Clock};
    const char* sats[3] = {"G01", "G01", "G02"};
    const char* sigs[3] = {"", "L1C", ""};
    for (int p = 0; p < 3; ++p)
        lc.coupling.push_back(CoupledShared{kinds[p], sats[p], sigs[p], 0, 0, 'C', 0,
                                            m[static_cast<std::size_t>(p)], f.ep.shat_s(p),
                                            f.ep.P(p, p)});
    lc.blk     = blk;
    lc.blk_idx = idx;
    return lc;
}

/// The exact row the builder forms for @p m (every term on a stashed dense key).
CouplingExactRow exactRow(const WsxFixture& f, const std::vector<double>& m) {
    std::vector<std::pair<int, double>> t;
    for (int p = 0; p < 3; ++p) t.emplace_back(p, m[static_cast<std::size_t>(p)]);
    return couplingExactRow(f.ep, t);
}

const std::vector<std::vector<double>> kM = {
    {-1.2, 0.9, 0.1}, {0.2, -0.3, -1.1}, {-0.6, 0.5, -0.4}, {-0.9, 0.8, 0.0}};
const std::vector<double> kDiag = {0.040, 0.030, 0.050, 0.045};
constexpr double kOff = 0.006;

/// Build the four-column block (rows 0-2 exact, row 3 harvested before the stash) and
/// return Σ.
Eigen::MatrixXd buildSigma(WsxFixture& f, bool cross) {
    auto blk = harvestBlock(kDiag, kOff);
    std::vector<LocalCarry> lcs;
    for (int i = 0; i < 4; ++i)
        lcs.push_back(carry(f, kM[static_cast<std::size_t>(i)], i < 3 ? kStash : kStash - 30.0,
                            blk, i));
    auto cb = f.make(cross);
    std::vector<LocalBlockCol> cols;
    for (int i = 0; i < 4; ++i)
        cols.push_back(LocalBlockCol{i, 0.1 * i, &lcs[static_cast<std::size_t>(i)],
                                     kDiag[static_cast<std::size_t>(i)], 30.0, 1.0});
    EXPECT_TRUE(cb.buildLocalBlock(cols));
    return f.R.pl_blk.Sigma;
}

std::string wsxConfig(bool exact, bool cross) {
    const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_coupling_exact_update\": " + b(exact) + ",\n"
        "    \"carry_coupling_exact_cross\": " + b(cross) + ",\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool wsxLoads(bool exact, bool cross, const char* tag, bool* armed = nullptr) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / (std::string("wsx_load_") + tag + ".json");
    { std::ofstream f(p); f << wsxConfig(exact, cross); }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    if (armed) *armed = cfg.networkCarryCouplingExactCross();
    return ok;
}

}  // namespace

// The knob adds M_i'BM_k between the exact rows and nothing with the card 1d row; the
// diagonal and the harvested term are untouched.
TEST(WsxWithinStationCrossTest, TheBlockTakesTheBracketBetweenExactRowsOnly) {
    WsxFixture off, on;
    const Eigen::MatrixXd S0 = buildSigma(off, false);
    const Eigen::MatrixXd S1 = buildSigma(on, true);
    ASSERT_EQ(S0.rows(), 4);
    ASSERT_EQ(S1.rows(), 4);
    for (int i = 0; i < 4; ++i) EXPECT_DOUBLE_EQ(S1(i, i), S0(i, i)) << "diagonal " << i;
    std::vector<CouplingExactRow> xr;
    for (int i = 0; i < 3; ++i) {
        xr.push_back(exactRow(on, kM[static_cast<std::size_t>(i)]));
        ASSERT_TRUE(xr.back().ok);
    }
    double largest = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int k = i + 1; k < 4; ++k) {
            const double expect = (k < 3) ? couplingExactCross(on.ep, xr[static_cast<std::size_t>(i)],
                                                               xr[static_cast<std::size_t>(k)])
                                          : 0.0;
            EXPECT_NEAR(S1(i, k) - S0(i, k), expect, 1e-15) << "pair " << i << "," << k;
            EXPECT_DOUBLE_EQ(S1(i, k), S1(k, i));
            largest = std::max(largest, std::abs(expect));
        }
    EXPECT_GT(largest, 1e-4) << "the fixture's walk should make the bracket visible";
    EXPECT_EQ(on.R.cxu.cross_blocks, 1);
    EXPECT_EQ(on.R.cxu.cross_pairs, 3);
    EXPECT_EQ(on.R.cxu.cross_pairs_skipped, 3);   // the three pairs with the card 1d row
    long long n = 0;
    for (int b = 0; b < CouplingExactCounters::kCrossRhoBins; ++b) n += on.R.cxu.cross_rho_hist[0][b];
    EXPECT_EQ(n, 3);
    EXPECT_GT(on.R.cxu.cross_abs_sum, 0.0);
    EXPECT_EQ(off.R.cxu.cross_pairs, 0);
    EXPECT_EQ(on.R.cxu.rows_exact, 3);
    EXPECT_EQ(on.R.cxu.rows_stale, 1);
}

// Off, Σ is the per-row form: the harvested term off the diagonal, bitwise.
TEST(WsxWithinStationCrossTest, OffTheBlockIsThePerRowForm) {
    WsxFixture f;
    const Eigen::MatrixXd S = buildSigma(f, false);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            if (i != k) EXPECT_DOUBLE_EQ(S(i, k), kOff) << i << "," << k;
}

TEST(WsxWithinStationCrossTest, LoaderRequiresTheExactUpdate) {
    bool armed = false;
    EXPECT_TRUE(wsxLoads(true, true, "ok", &armed));
    EXPECT_TRUE(armed);
    EXPECT_TRUE(wsxLoads(true, false, "off", &armed));
    EXPECT_FALSE(armed);
    EXPECT_FALSE(wsxLoads(false, true, "noexact")) << "it adds the exact rule's bracket";
}
