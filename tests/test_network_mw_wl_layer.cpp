/**
 * @file test_network_mw_wl_layer.cpp
 * @brief Gate tests for the MW-seeded wide-lane pseudo-observation layer (M5 card WL-1,
 *        with the WL-2 and UWL-1 extensions).
 *
 * The layer holds the card's new math.  From the carry driver's fundamental-cycle
 * chord basis and a per-arc MW resolver it emits WL rows:
 *   terms:  +c/λ1 on the L1 ambiguity column, −c/λ2 on the L2 column (per cycle arc);
 *   z    :  Σ c·MW̄/λ_WL   (measured wide-lane double difference, cycles);
 *   R    :  max(Σ c²·var/λ_WL², sigma_floor²).
 * These gates check the signs, the λ scaling, the R propagation and floor, the
 * eligibility classification (with counters), the max-rows cap, and the grouping per
 * carry block (so the WL rows of one component fold jointly).  A resolver stub feeds
 * synthetic ArcData, so the tests are pure and deterministic (no estimator, no
 * detector): the SPEC §7.3 "unit battery" for the row builder.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "filter/NetworkMwWlLayer.hpp"

using namespace GnssProcessor;
using Block = NetworkNormalEquations::DdPriorBlock;
using Term  = NetworkNormalEquations::DdPriorBlock::Term;

namespace {

// One cycle arc's ground-truth contribution, keyed by (est_station, band-1 col).
struct ArcKeyT {
    std::size_t  station;
    Eigen::Index col1;
    bool operator<(const ArcKeyT& o) const {
        return std::tie(station, col1) < std::tie(o.station, o.col1);
    }
};

// Build a resolver over a fixed (station,col1) -> ArcData table.
NetworkMwWlLayer::ArcResolver
tableResolver(const std::map<ArcKeyT, NetworkMwWlLayer::ArcData>& tbl) {
    return [tbl](std::size_t st, Eigen::Index col,
                 NetworkMwWlLayer::ArcData& out) -> bool {
        const auto it = tbl.find({st, col});
        if (it == tbl.end()) return false;   // not reverse-mappable
        out = it->second;
        return true;
    };
}

// A fully-eligible primary-L1 arc with the given band-1 col, sat mean, variance.
NetworkMwWlLayer::ArcData eligibleArc(Eigen::Index col1, Eigen::Index col2,
                                      double mean_m, double var_m2,
                                      long long count,
                                      double l1 = 0.2, double l2 = 0.25,
                                      double lwl = 0.8) {
    NetworkMwWlLayer::ArcData a;
    a.col1 = col1; a.col2 = col2;
    a.lambda1 = l1; a.lambda2 = l2; a.lambda_wl = lwl;
    a.mw_zd_mean_m = mean_m; a.mw_var_m2 = var_m2; a.mw_count = count;
    a.is_primary_sig1 = true; a.have_mw = true;
    return a;
}

// A single 4-arc double-difference cycle on band-1 columns {0,1} of stations {0,1}:
//   (N_{0,a} − N_{0,b}) − (N_{1,a} − N_{1,b}),  coeffs +1,−1,−1,+1.
std::vector<Term> ddCycleRow() {
    return { Term{0, 0, +1.0}, Term{0, 1, -1.0}, Term{1, 0, -1.0}, Term{1, 1, +1.0} };
}

Block oneRowBlock(std::vector<Term> row) {
    Block b;
    b.rows.push_back(std::move(row));
    return b;   // Pz/zhat are set by the carry driver in reality; the layer only reads rows.
}

double termCoeff(const std::vector<Term>& terms, std::size_t st, Eigen::Index col) {
    double s = 0.0;
    for (const auto& t : terms) if (t.station == st && t.local_col == col) s += t.coeff;
    return s;
}

}  // namespace

// OFF path: enable=false ⇒ no blocks, no counter movement (byte-identical producer).
TEST(NetworkMwWlLayer, DisabledReturnsNoBlocks) {
    NetworkMwWlLayer::Config cfg;   // enable defaults false
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.cycles_seen, 0);
    EXPECT_EQ(ctr.rows_built, 0);
}

// The core algebra: signs (+c/λ1 on L1, −c/λ2 on L2), z = Σ c·MW̄/λ_WL, R propagation.
TEST(NetworkMwWlLayer, SingleDdCycleRowConstruction) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.min_arc_epochs = 10; cfg.sigma_floor_cyc = 0.05;
    NetworkMwWlLayer layer(cfg);

    // Means chosen so z = (0.8 − 0 − 0 + 0.8)/0.8 = 2.0 cyc; var 0.01 each.
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    tbl[{0, 0}] = eligibleArc(0, 100, 0.80, 0.01, 30);
    tbl[{0, 1}] = eligibleArc(1, 101, 0.00, 0.01, 30);
    tbl[{1, 0}] = eligibleArc(0, 100, 0.00, 0.01, 30);
    tbl[{1, 1}] = eligibleArc(1, 101, 0.80, 0.01, 30);

    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].rows.size(), 1u);
    const auto& terms = out[0].rows[0];
    EXPECT_EQ(terms.size(), 8u);            // 2 cross-band terms per cycle arc

    // Signs & λ scaling: +c/λ1 on L1 col, −c/λ2 on L2 col (λ1=0.2 ⇒ 1/λ1=5; λ2=0.25 ⇒ 4).
    EXPECT_NEAR(termCoeff(terms, 0, 0),   +5.0, 1e-12);   // st0 sat-a L1, c=+1
    EXPECT_NEAR(termCoeff(terms, 0, 100), -4.0, 1e-12);   // st0 sat-a L2
    EXPECT_NEAR(termCoeff(terms, 0, 1),   -5.0, 1e-12);   // st0 sat-b L1, c=−1
    EXPECT_NEAR(termCoeff(terms, 0, 101), +4.0, 1e-12);   // st0 sat-b L2
    EXPECT_NEAR(termCoeff(terms, 1, 0),   -5.0, 1e-12);   // st1 sat-a L1, c=−1
    EXPECT_NEAR(termCoeff(terms, 1, 100), +4.0, 1e-12);
    EXPECT_NEAR(termCoeff(terms, 1, 1),   +5.0, 1e-12);   // st1 sat-b L1, c=+1
    EXPECT_NEAR(termCoeff(terms, 1, 101), -4.0, 1e-12);

    ASSERT_EQ(out[0].zhat.size(), 1);
    EXPECT_NEAR(out[0].zhat(0), 2.0, 1e-12);               // measured WL DD (cycles)
    // R = Σ c²·var/λ_WL² = 4·0.01/0.64 = 0.0625 (> floor 0.0025).
    ASSERT_EQ(out[0].Pz.rows(), 1); ASSERT_EQ(out[0].Pz.cols(), 1);
    EXPECT_NEAR(out[0].Pz(0, 0), 0.0625, 1e-12);
    EXPECT_EQ(ctr.cycles_seen, 1);
    EXPECT_EQ(ctr.rows_built, 1);
    EXPECT_NEAR(ctr.sum_R, 0.0625, 1e-12);
}

// A lucky-tight arc must not fold a near-exact constraint: R is floored at sigma_floor².
TEST(NetworkMwWlLayer, RFloorApplied) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.min_arc_epochs = 10; cfg.sigma_floor_cyc = 0.10;   // floor² = 0.01
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.0, 1e-9, 30);   // ~zero variance
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].Pz(0, 0), 0.01, 1e-12);   // floored, not ~0
}

// Eligibility classification + counters (invariant #4).
TEST(NetworkMwWlLayer, YoungArcSkipped) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true; cfg.min_arc_epochs = 10;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, /*count=*/5);  // < min
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.cycles_seen, 1);
    EXPECT_EQ(ctr.skip_young_arc, 1);
    EXPECT_EQ(ctr.rows_built, 0);
}

TEST(NetworkMwWlLayer, MissingBandSkipped) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);
    tbl[{1, 1}].col2 = -1;   // one arc lacks its L2 column
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.cycles_seen, 1);
    EXPECT_EQ(ctr.skip_missing_band, 1);
}

TEST(NetworkMwWlLayer, MissingMwSkipped) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);
    tbl[{0, 0}].have_mw = false;   // channel mismatch / pivot missing
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.cycles_seen, 1);
    EXPECT_EQ(ctr.skip_missing_mw, 1);
}

TEST(NetworkMwWlLayer, NotPrimarySkipped) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}}) {
            auto a = eligibleArc(c, c + 100, 0.5, 0.01, 30);
            a.is_primary_sig1 = false;   // an L2 / L1W / L5 carry group cycle
            tbl[{s, c}] = a;
        }
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.cycles_seen, 0);       // a non-primary cycle is not "seen"
    EXPECT_EQ(ctr.skip_not_primary, 1);
}

TEST(NetworkMwWlLayer, BadLambdaSkipped) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);
    tbl[{0, 1}].lambda_wl = 0.0;   // non-positive wavelength
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.skip_bad_lambda, 1);
}

TEST(NetworkMwWlLayer, UnresolvableTermSkipsMissingBand) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;   // empty ⇒ every term fails to resolve
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.skip_missing_band, 1);
    EXPECT_EQ(ctr.cycles_seen, 0);
}

// Rows of one carry block group into one WL block (jointly foldable); distinct carry
// blocks yield distinct WL blocks (column-disjoint ⇒ exact composition).
TEST(NetworkMwWlLayer, RowsGroupedByCarryBlock) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);

    // Block A has two identical cycles; block B has one.  ⇒ WL block 0 has K=2, WL block 1 K=1.
    Block a; a.rows.push_back(ddCycleRow()); a.rows.push_back(ddCycleRow());
    Block b = oneRowBlock(ddCycleRow());
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ a, b }, tableResolver(tbl), ctr);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].rows.size(), 2u);
    EXPECT_EQ(out[0].Pz.rows(), 2);            // K×K within the block
    EXPECT_EQ(out[0].Pz.cols(), 2);
    // Identical cycles share every arc ⇒ fully correlated: the off-diagonal equals
    // the (un-floored) shared-arc variance Σ c²·var/λ_WL² = 4·0.01/0.64 = 0.0625,
    // the same as the diagonal (review F2: shared-arc covariance, not diagonal Pz).
    EXPECT_NEAR(out[0].Pz(0, 0), 0.0625, 1e-12);
    EXPECT_NEAR(out[0].Pz(0, 1), 0.0625, 1e-12);
    EXPECT_NEAR(out[0].Pz(1, 0), 0.0625, 1e-12);
    EXPECT_EQ(out[1].rows.size(), 1u);
    EXPECT_EQ(ctr.rows_built, 3);
    EXPECT_EQ(ctr.cycles_seen, 3);
}

// Two cycles of one component sharing exactly one arc: the block covariance is that
// arc's contribution g² = c_i·c_j·var/λ_WL² (signs included); disjoint cycles in the
// same block stay uncorrelated (review F2 gate).
TEST(NetworkMwWlLayer, SharedArcCovarianceExact) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.sigma_floor_cyc = 1e-6;   // floor out of the way
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1, 2})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}, std::size_t{2}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);   // var 0.01, λ_WL 0.8

    // cycle1 arcs: (0,0)+ (0,1)- (1,0)- (1,1)+ ; cycle2 arcs: (0,0)+ (0,2)- (2,0)- (2,2)+
    // shared arc = (0,0) with coeff +1 in both ⇒ cov = 0.01/0.64 = 0.015625.
    Block blk;
    blk.rows.push_back({ Term{0, 0, +1.0}, Term{0, 1, -1.0}, Term{1, 0, -1.0}, Term{1, 1, +1.0} });
    blk.rows.push_back({ Term{0, 0, +1.0}, Term{0, 2, -1.0}, Term{2, 0, -1.0}, Term{2, 2, +1.0} });
    // cycle3 shares no arc with cycle1 or cycle2: arcs (1,2)+ (2,1)- on disjoint cols.
    blk.rows.push_back({ Term{1, 2, +1.0}, Term{2, 1, -1.0} });
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ blk }, tableResolver(tbl), ctr);
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].rows.size(), 3u);
    EXPECT_NEAR(out[0].Pz(0, 1), 0.015625, 1e-12);   // one shared arc, ++ signs
    EXPECT_NEAR(out[0].Pz(1, 0), 0.015625, 1e-12);   // symmetric
    EXPECT_NEAR(out[0].Pz(0, 2), 0.0, 1e-15);        // disjoint ⇒ uncorrelated
    EXPECT_NEAR(out[0].Pz(1, 2), 0.0, 1e-15);
    // Sign flip: shared arc entering with opposite signs ⇒ negative covariance.
    Block blk2;
    blk2.rows.push_back({ Term{0, 0, +1.0}, Term{0, 1, -1.0}, Term{1, 0, -1.0}, Term{1, 1, +1.0} });
    blk2.rows.push_back({ Term{0, 0, -1.0}, Term{0, 2, +1.0}, Term{2, 0, +1.0}, Term{2, 2, -1.0} });
    NetworkMwWlLayer::Counters ctr2;
    auto out2 = layer.build({ blk2 }, tableResolver(tbl), ctr2);
    ASSERT_EQ(out2.size(), 1u);
    EXPECT_NEAR(out2[0].Pz(0, 1), -0.015625, 1e-12);
}

// The kernel solves corrections about x_lin, so the absolute MW measurement is
// re-expressed by subtracting the row functional at the linearization point:
// z = Σc·MW̄/λ_WL − Σ[(c/λ1)x1 − (c/λ2)x2] (review F5 gate).
TEST(NetworkMwWlLayer, LinearizationFrameSubtraction) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true; cfg.sigma_floor_cyc = 1e-6;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.8, 0.01, 30);   // λ1=0.2, λ2=0.25, λwl=0.8
    // Distinct linearization values per arc (meters).
    tbl[{0, 0}].x_lin1 = 2.0;  tbl[{0, 0}].x_lin2 = 1.0;
    tbl[{0, 1}].x_lin1 = -1.0; tbl[{0, 1}].x_lin2 = 0.5;
    tbl[{1, 0}].x_lin1 = 0.4;  tbl[{1, 0}].x_lin2 = -0.25;
    tbl[{1, 1}].x_lin1 = 0.0;  tbl[{1, 1}].x_lin2 = 0.0;
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    ASSERT_EQ(out.size(), 1u);
    // z_abs = Σ c·MW̄/λwl = (+1−1−1+1)·(0.8/0.8) = 0.
    // lin part = Σ c·(x1/λ1 − x2/λ2):
    //   (0,0)+ : 2.0/0.2 − 1.0/0.25   = 10 − 4    =  6
    //   (0,1)− : −(−1.0/0.2 − 0.5/0.25) = −(−5−2) =  7
    //   (1,0)− : −(0.4/0.2 − (−0.25)/0.25) = −(2+1) = −3
    //   (1,1)+ : 0
    // z = 0 − (6 + 7 − 3) = −10.
    EXPECT_NEAR(out[0].zhat(0), -10.0, 1e-12);
}

// A cycle mixing BIA-corrected and raw satellite-bias frames is skipped (the biases
// would not cancel) and counted; a cycle in either uniform frame passes (review F3 gate).
TEST(NetworkMwWlLayer, MixedBiasFrameSkipped) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    for (Eigen::Index c : {0, 1})
        for (std::size_t s : {std::size_t{0}, std::size_t{1}})
            tbl[{s, c}] = eligibleArc(c, c + 100, 0.5, 0.01, 30);
    tbl[{1, 1}].bia_corrected = true;   // one arc in the other frame
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.skip_mixed_frame, 1);
    EXPECT_EQ(ctr.rows_built, 0);
    // Uniform BIA-corrected frame passes.
    for (auto& kv : tbl) kv.second.bia_corrected = true;
    NetworkMwWlLayer::Counters ctr2;
    auto out2 = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr2);
    EXPECT_EQ(out2.size(), 1u);
    EXPECT_EQ(ctr2.skip_mixed_frame, 0);
}

// max_rows_per_epoch keeps the lowest-R (most informative) rows; the rest are counted.
TEST(NetworkMwWlLayer, MaxRowsCapKeepsLowestR) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.min_arc_epochs = 10; cfg.sigma_floor_cyc = 1e-6;  // floor out of the way
    cfg.max_rows_per_epoch = 1;
    NetworkMwWlLayer layer(cfg);

    // Two blocks referencing distinct columns, so a single resolver table serves both:
    // block "lowR" (cols {0,1}, var 0.01) and block "hiR" (cols {2,3}, var 1.0).
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    tbl[{0, 0}] = eligibleArc(0, 100, 0.5, 0.01, 30);
    tbl[{0, 1}] = eligibleArc(1, 101, 0.5, 0.01, 30);
    tbl[{1, 0}] = eligibleArc(0, 100, 0.5, 0.01, 30);
    tbl[{1, 1}] = eligibleArc(1, 101, 0.5, 0.01, 30);
    tbl[{0, 2}] = eligibleArc(2, 102, 0.5, 1.00, 30);
    tbl[{0, 3}] = eligibleArc(3, 103, 0.5, 1.00, 30);
    tbl[{1, 2}] = eligibleArc(2, 102, 0.5, 1.00, 30);
    tbl[{1, 3}] = eligibleArc(3, 103, 0.5, 1.00, 30);

    Block lowR = oneRowBlock({ Term{0, 0, +1.0}, Term{0, 1, -1.0}, Term{1, 0, -1.0}, Term{1, 1, +1.0} });
    Block hiR  = oneRowBlock({ Term{0, 2, +1.0}, Term{0, 3, -1.0}, Term{1, 2, -1.0}, Term{1, 3, +1.0} });

    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ hiR, lowR }, tableResolver(tbl), ctr);   // note: hi first
    // Only the low-R row survives (1 WL block, 1 row) and the high-R row is capped.
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].rows.size(), 1u);
    EXPECT_LT(out[0].Pz(0, 0), 0.1);        // it's the low-R row
    EXPECT_EQ(ctr.rows_built, 1);
    EXPECT_EQ(ctr.rows_capped, 1);
    EXPECT_EQ(ctr.cycles_seen, 2);
}

// A chordless (empty) carry row is a no-op (mirrors the kernel's inert degenerate slot).
TEST(NetworkMwWlLayer, EmptyCarryRowIgnored) {
    NetworkMwWlLayer::Config cfg; cfg.enable = true;
    NetworkMwWlLayer layer(cfg);
    Block b; b.rows.push_back({});   // empty cycle
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    NetworkMwWlLayer::Counters ctr;
    auto out = layer.build({ b }, tableResolver(tbl), ctr);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(ctr.cycles_seen, 0);
}

// =========================================================================
// FFR integer-hard fixing and the FixedEwlRow exposure the WL phase bridge keys
// off (M5 card WL-2, SPEC §3.2/§3.4).
// =========================================================================

// A cycle whose statistic DD z_abs = Σc·MW̄/λ_WL sits on an integer with a tight σ
// folds integer-hard (Pz = fix_weight²) and is exposed as a FixedEwlRow (block, row,
// integer) so the bridge can pair it back to its carry chord.
TEST(NetworkMwWlLayer, FfrFixExposesFixedRow) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.min_arc_epochs = 10; cfg.sigma_floor_cyc = 0.05;
    cfg.ffr_fix = true; cfg.fix_sigma_cyc = 0.10; cfg.fix_frac_cyc = 0.20;
    cfg.fix_weight_sigma_cyc = 0.01;
    NetworkMwWlLayer layer(cfg);
    // z_abs = (2.4 − 0 − 0 + 0)/0.8 = 3.0 (frac 0); σ_row = √(4·1e-6/0.64) ≈ 0.0025 < 0.10.
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    tbl[{0, 0}] = eligibleArc(0, 100, 2.40, 1e-6, 30);
    tbl[{0, 1}] = eligibleArc(1, 101, 0.00, 1e-6, 30);
    tbl[{1, 0}] = eligibleArc(0, 100, 0.00, 1e-6, 30);
    tbl[{1, 1}] = eligibleArc(1, 101, 0.00, 1e-6, 30);
    NetworkMwWlLayer::Counters ctr;
    std::vector<NetworkMwWlLayer::FixedEwlRow> fixed;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr, &fixed);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].Pz(0, 0), 1e-4, 1e-12);   // fix_weight² (integer-hard), not the float R
    EXPECT_NEAR(out[0].zhat(0), 3.0, 1e-12);      // round(z_abs) − z_lin(=0)
    EXPECT_EQ(ctr.rows_fixed, 1);
    ASSERT_EQ(fixed.size(), 1u);
    EXPECT_EQ(fixed[0].block, 0u);
    EXPECT_EQ(fixed[0].row, 0u);
    EXPECT_EQ(fixed[0].integer, 3);
}

// A cycle whose fractional part exceeds fix_frac_cyc stays float: a row is built,
// but no FixedEwlRow is exposed (the bridge uses only FFR-fixed cycles).
TEST(NetworkMwWlLayer, FfrUnfixedExposesNothing) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.min_arc_epochs = 10; cfg.ffr_fix = true;
    cfg.fix_sigma_cyc = 0.10; cfg.fix_frac_cyc = 0.20;
    NetworkMwWlLayer layer(cfg);
    // z_abs = 2.72/0.8 = 3.4 (frac 0.4 > 0.20 ⇒ not fixed) with a tight σ.
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    tbl[{0, 0}] = eligibleArc(0, 100, 2.72, 1e-6, 30);
    tbl[{0, 1}] = eligibleArc(1, 101, 0.00, 1e-6, 30);
    tbl[{1, 0}] = eligibleArc(0, 100, 0.00, 1e-6, 30);
    tbl[{1, 1}] = eligibleArc(1, 101, 0.00, 1e-6, 30);
    NetworkMwWlLayer::Counters ctr;
    std::vector<NetworkMwWlLayer::FixedEwlRow> fixed;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr, &fixed);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(ctr.rows_built, 1);
    EXPECT_EQ(ctr.rows_fixed, 0);
    EXPECT_TRUE(fixed.empty());
}

// The WL-1 float lane (ffr_fix=false) never fixes and never exposes a FixedEwlRow,
// even when a fixed_rows sink is supplied (byte-identical WL-1 behaviour).
TEST(NetworkMwWlLayer, FloatLaneExposesNothing) {
    NetworkMwWlLayer::Config cfg;
    cfg.enable = true; cfg.min_arc_epochs = 10;   // ffr_fix defaults false
    NetworkMwWlLayer layer(cfg);
    std::map<ArcKeyT, NetworkMwWlLayer::ArcData> tbl;
    tbl[{0, 0}] = eligibleArc(0, 100, 2.40, 1e-6, 30);
    tbl[{0, 1}] = eligibleArc(1, 101, 0.00, 1e-6, 30);
    tbl[{1, 0}] = eligibleArc(0, 100, 0.00, 1e-6, 30);
    tbl[{1, 1}] = eligibleArc(1, 101, 0.00, 1e-6, 30);
    NetworkMwWlLayer::Counters ctr;
    std::vector<NetworkMwWlLayer::FixedEwlRow> fixed;
    auto out = layer.build({ oneRowBlock(ddCycleRow()) }, tableResolver(tbl), ctr, &fixed);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(ctr.rows_fixed, 0);
    EXPECT_TRUE(fixed.empty());
}
