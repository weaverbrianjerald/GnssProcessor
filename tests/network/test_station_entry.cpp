/**
 * @file test_station_entry.cpp
 * @brief The pin of a refused station's null directions (network.station_local_null_pin) and the cold
 *        entry of a refused station (network.station_entry_cold).
 *
 * Covered:
 *   - the keys: off by default, loaded on;
 *   - the normal equations: a station whose block holds a direction no row informs fails
 *     addStation; with one pin row along it the station is admitted, every other estimate equals
 *     the solve of the same station without the direction, and the direction stays at its
 *     linearization point;
 *   - the null pin on an assembly: one row per null direction, none touching an informed column,
 *     tagged and counted; the parked columns leave the harvest lists (a column with a post-PD
 *     carry keeps its harvest); nothing happens with the key off;
 *   - the edge step: a station refused for 25 epochs from its first epoch and then admitted gives
 *     one decoupling edge and one re-coupling edge, a first entry, 25 epochs out; a park episode
 *     opens and closes;
 *   - the cold drop: every carried state and chain memory cell of the dropped station is reset,
 *     another station's are untouched.
 */

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/NormalEquations.hpp"
#include "network/solve/StationEntry.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <tuple>
#include <vector>
#include "../support/TestTempDir.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Load the simulator's reference file with @p edit applied; a refusal returns false.
bool loadRefWith(const std::function<void(nlohmann::json&)>& edit, GnssConfig& cfg,
                 const std::string& tag) {
    const fs::path src = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / "golden_sim_refound.json";
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const fs::path p = gnss_test::tempDir() / ("gnss_sen_" + tag + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    bool ok = false;
    try { ok = cfg.loadFromJson(p, log); } catch (const std::exception&) { ok = false; }
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// Add a station to @p neq from its rows: the normal blocks A^T W A and A^T W y.
std::size_t addRows(NetworkNormalEquations& neq, const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
                    const Eigen::VectorXd& w, const Eigen::VectorXd& y,
                    const NetworkNormalEquations::StationPrior& prior = {}) {
    const Eigen::MatrixXd W = w.asDiagonal();
    const Eigen::MatrixXd N_ss = A_s.transpose() * W * A_s;
    const Eigen::MatrixXd N_sr = A_s.transpose() * W * A_r;
    const Eigen::MatrixXd N_rr = A_r.transpose() * W * A_r;
    const Eigen::VectorXd u_s = A_s.transpose() * W * y;
    const Eigen::VectorXd u_r = A_r.transpose() * W * y;
    return neq.addStation(N_ss, N_sr, N_rr, u_s, u_r, prior);
}

/// A station block of 8 rows over 2 shared and 3 local columns; local column 2 has no row.
struct ToyStation {
    Eigen::MatrixXd A_s, A_r;
    Eigen::VectorXd w, y;
    ToyStation() : A_s(8, 2), A_r(8, 3), w(8), y(8) {
        for (int r = 0; r < 8; ++r) {
            A_s(r, 0) = 1.0;
            A_s(r, 1) = std::cos(0.7 * r);
            A_r(r, 0) = 1.0 + 0.1 * r;
            A_r(r, 1) = std::sin(0.9 * r + 0.3);
            A_r(r, 2) = 0.0;                     // the rowless column
            w(r) = 1.0 + 0.05 * r;
            y(r) = 0.3 * std::sin(1.3 * r) + 0.1 * r;
        }
    }
};

/// A shared-only anchor station that makes the two shared columns estimable.
void addAnchor(NetworkNormalEquations& neq) {
    Eigen::MatrixXd A_s(6, 2), A_r(6, 1);
    Eigen::VectorXd w(6), y(6);
    for (int r = 0; r < 6; ++r) {
        A_s(r, 0) = 1.0;
        A_s(r, 1) = 0.5 * r - 1.0;
        A_r(r, 0) = 1.0;
        w(r) = 2.0;
        y(r) = 0.2 * r;
    }
    addRows(neq, A_s, A_r, w, y);
}

}  // namespace

// ===========================================================================
// The keys
// ===========================================================================

TEST(StationEntryConfig, OffByDefaultAndLoadedOn) {
    GnssConfig off, on;
    ASSERT_TRUE(loadRefWith([](nlohmann::json&) {}, off, "off"));
    EXPECT_FALSE(off.getNetworkConfig().station_local_null_pin);
    EXPECT_FALSE(off.getNetworkConfig().station_entry_cold);
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) {
        j["network"]["station_local_null_pin"] = true;
        j["network"]["station_entry_cold"] = true;
    }, on, "on"));
    EXPECT_TRUE(on.getNetworkConfig().station_local_null_pin);
    EXPECT_TRUE(on.getNetworkConfig().station_entry_cold);
}

// ===========================================================================
// The normal equations: the park decouples the column exactly
// ===========================================================================

TEST(StationNullPin, TheParkedColumnIsDecoupledExactly) {
    const ToyStation t;
    // Without the park the block fails the positive-definiteness test.
    {
        NetworkNormalEquations neq(2);
        addAnchor(neq);
        EXPECT_THROW(addRows(neq, t.A_s, t.A_r, t.w, t.y), std::exception);
    }
    // With the null pin: one row along the null direction (here the rowless column) at the
    // linearization point, of a small weight.
    NetworkNormalEquations parked(2);
    addAnchor(parked);
    Eigen::MatrixXd A_s2(9, 2), A_r2(9, 3);
    Eigen::VectorXd w2(9), y2(9);
    A_s2 << t.A_s, Eigen::RowVector2d::Zero();
    A_r2 << t.A_r, Eigen::RowVector3d(0.0, 0.0, 1.0);
    w2 << t.w, 1e-3;
    y2 << t.y, 0.0;
    std::size_t ip = 0;
    ASSERT_NO_THROW(ip = addRows(parked, A_s2, A_r2, w2, y2));
    const Eigen::VectorXd xs_p = parked.solveShared();
    const Eigen::VectorXd xr_p = parked.reconstructStation(ip, xs_p);
    // The same station without the column.
    NetworkNormalEquations ref(2);
    addAnchor(ref);
    const std::size_t ir = addRows(ref, t.A_s, t.A_r.leftCols(2), t.w, t.y);
    const Eigen::VectorXd xs_r = ref.solveShared();
    const Eigen::VectorXd xr_r = ref.reconstructStation(ir, xs_r);
    ASSERT_EQ(xs_p.size(), xs_r.size());
    for (Eigen::Index k = 0; k < xs_p.size(); ++k) EXPECT_NEAR(xs_p(k), xs_r(k), 1e-12);
    EXPECT_NEAR(xr_p(0), xr_r(0), 1e-12);
    EXPECT_NEAR(xr_p(1), xr_r(1), 1e-12);
    EXPECT_NEAR(xr_p(2), 0.0, 1e-15);   // the parked column stays at its linearization point
}

// ===========================================================================
// The park on an assembly
// ===========================================================================

TEST(StationNullPin, PinsTheNullDirectionsAndLeavesTheHarvest) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) { j["network"]["station_local_null_pin"] = true; },
                            cfg, "park"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.local_null_pin);
    NetStation st;
    st.name = "AAAA";
    StationAssembly R;
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Ones(4, 2);
    R.A_r = Eigen::MatrixXd::Zero(4, 5);
    R.A_r.col(0).setOnes();                 // a column with rows
    R.w = Eigen::VectorXd::Ones(4);
    R.y = Eigen::VectorXd::Zero(4);
    R.row_tags.assign(4, RowTag{ContributionKind::DataCode, {}, {}});
    R.n_data_rows = 4;
    // column 1: a pre-PD prior; column 2: a post-PD carry only; columns 3 and 4: nothing.
    R.prior_cols = {1};
    R.prior_dNrr = Eigen::VectorXd::Constant(1, 4.0);
    R.prior_dur = Eigen::VectorXd::Constant(1, 0.5);
    R.prior_tags = {RowTag{ContributionKind::PriorRxCbDatumPrePd, {}, {}}};
    R.pl_cb_datum_rows = 1;                 // the builder's own count of that datum row
    R.carry_cols = {2};
    R.carry_dNrr = Eigen::VectorXd::Constant(1, 9.0);
    R.carry_dur = Eigen::VectorXd::Zero(1);
    R.carry_tags = {RowTag{ContributionKind::PriorRxCbCarry, {}, {}}};
    R.pl_rpb = {std::make_tuple(std::string("Galileo_RPB_L5Q"), Eigen::Index{3}, 0.0),
                std::make_tuple(std::string("Galileo_RPB_L1C"), Eigen::Index{0}, 0.0)};
    R.pl_cb = {std::make_tuple(std::string("Galileo_ISC_C5Q"), Eigen::Index{4}, 0.0)};
    const Eigen::MatrixXd A_r0 = R.A_r;

    // The test's block has the null space span{e2, e3, e4} (the post-PD carry is not in it).
    EXPECT_EQ(networkStationNullPin(ctx, st, R), 3);
    ASSERT_EQ(R.A_r.rows(), 7);
    ASSERT_EQ(R.A_s.rows(), 7);
    EXPECT_TRUE(R.A_r.topRows(4).isApprox(A_r0));
    EXPECT_TRUE(R.A_s.bottomRows(3).isZero(0.0));
    for (Eigen::Index r = 4; r < 7; ++r) {
        EXPECT_NEAR(R.A_r.row(r).norm(), 1.0, 1e-12);
        EXPECT_NEAR(R.A_r(r, 0), 0.0, 1e-12);   // no pin touches an informed column
        EXPECT_NEAR(R.A_r(r, 1), 0.0, 1e-12);
        EXPECT_DOUBLE_EQ(R.y(r), 0.0);
        EXPECT_GT(R.w(r), 0.0);
        EXPECT_EQ(R.row_tags[static_cast<std::size_t>(r)].kind, ContributionKind::LocalNullPin);
    }
    EXPECT_EQ(R.null_pin_rows, 3);
    EXPECT_EQ(R.prior_cols.size(), 1u);      // the prior channel is not touched
    // The parked columns left the harvest lists (the carried column keeps its harvest).
    ASSERT_EQ(R.parked_cols.size(), 2u);
    EXPECT_EQ(R.parked_cols[0], 3);
    EXPECT_EQ(R.parked_cols[1], 4);
    ASSERT_EQ(R.pl_rpb.size(), 1u);
    EXPECT_EQ(std::get<1>(R.pl_rpb[0]), 0);
    EXPECT_TRUE(R.pl_cb.empty());
    EXPECT_EQ(ctx.counters.sen_park_station_epochs, 1);
    EXPECT_EQ(ctx.counters.sen_null_pin_rows, 3);
    // The tag check closes over the new rows.
    const StationTagCheck chk = checkStationTags(R);
    EXPECT_EQ(chk.untagged, 0);
    EXPECT_EQ(chk.mismatches, 0);
}

namespace {

/// An assembly over 2 shared and 3 local columns whose local columns 1 and 2 enter every row
/// together (a single-frequency constellation's phase bias and ionosphere shift together), so
/// v = (0, 1, -1)/sqrt(2) is a null direction of the local block that no row sees; @p eps > 0
/// separates the two columns slightly in one row, which makes the direction weak, not null.
StationAssembly nullBlock(double eps) {
    StationAssembly R;
    R.ok = true;
    const int m = 8;
    R.A_s.resize(m, 2);
    R.A_r.resize(m, 3);
    R.w.resize(m);
    R.y.resize(m);
    for (int r = 0; r < m; ++r) {
        R.A_s(r, 0) = 1.0;
        R.A_s(r, 1) = std::cos(0.7 * r);
        R.A_r(r, 0) = 1.0 + 0.1 * r;
        const double c = std::sin(0.9 * r + 0.3);
        R.A_r(r, 1) = c;
        R.A_r(r, 2) = c;
        R.w(r) = 1.0 + 0.05 * r;
        R.y(r) = 0.3 * std::sin(1.3 * r) + 0.1 * r;
    }
    R.A_r(0, 2) += eps;
    R.row_tags.assign(static_cast<std::size_t>(m), RowTag{ContributionKind::DataCode, {}, {}});
    R.n_data_rows = m;
    R.pl_rpb = {std::make_tuple(std::string("Galileo_RPB_L1C"), Eigen::Index{1}, 0.0)};
    R.pl_iono = {std::make_tuple(std::string("E07"), Eigen::Index{2}, 0.0)};
    R.pl_cb = {std::make_tuple(std::string("GPS_DCB"), Eigen::Index{0}, 0.0)};
    return R;
}

}  // namespace

TEST(StationNullPin, ANullDirectionIsPinnedAndNothingElseMoves) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) { j["network"]["station_local_null_pin"] = true; },
                            cfg, "nullblock"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    NetStation st;
    st.name = "PALM";
    StationAssembly R = nullBlock(0.0);
    // Refused as built.
    {
        NetworkNormalEquations neq(2);
        addAnchor(neq);
        EXPECT_THROW(addRows(neq, R.A_s, R.A_r, R.w, R.y), std::exception);
    }
    ASSERT_EQ(networkStationNullPin(ctx, st, R), 1);
    EXPECT_EQ(R.null_pin_rows, 1);
    // The pin row lies along the null direction (0, 1, -1)/sqrt(2), up to its sign.
    const Eigen::RowVectorXd pin = R.A_r.row(R.A_r.rows() - 1);
    EXPECT_NEAR(pin(0), 0.0, 1e-12);
    EXPECT_NEAR(std::abs(pin(1)), 1.0 / std::sqrt(2.0), 1e-12);
    EXPECT_NEAR(pin(1) + pin(2), 0.0, 1e-12);
    NetworkNormalEquations pinned(2);
    addAnchor(pinned);
    std::size_t ip = 0;
    ASSERT_NO_THROW(ip = addRows(pinned, R.A_s, R.A_r, R.w, R.y));
    const Eigen::VectorXd xs_p = pinned.solveShared();
    const Eigen::VectorXd xr_p = pinned.reconstructStation(ip, xs_p);
    // The same problem with the direction removed: columns 1 and 2 merged into one.
    const StationAssembly R0 = nullBlock(0.0);
    Eigen::MatrixXd A_r_merged(R0.A_r.rows(), 2);
    A_r_merged.col(0) = R0.A_r.col(0);
    A_r_merged.col(1) = R0.A_r.col(1);
    NetworkNormalEquations merged(2);
    addAnchor(merged);
    const std::size_t im = addRows(merged, R0.A_s, A_r_merged, R0.w, R0.y);
    const Eigen::VectorXd xs_m = merged.solveShared();
    const Eigen::VectorXd xr_m = merged.reconstructStation(im, xs_m);
    for (Eigen::Index k = 0; k < xs_p.size(); ++k) EXPECT_NEAR(xs_p(k), xs_m(k), 1e-10);   // the border
    EXPECT_NEAR(xr_p(0), xr_m(0), 1e-10);                       // the other local column
    EXPECT_NEAR(xr_p(1) + xr_p(2), xr_m(1), 1e-10);             // the observable sum
    EXPECT_NEAR(xr_p(1) - xr_p(2), 0.0, 1e-10);                 // the pinned direction at its linearization point
    // The direction's columns left the harvest; the other column stayed.
    EXPECT_EQ(R.parked_cols, (std::vector<Eigen::Index>{1, 2}));
    EXPECT_TRUE(R.pl_rpb.empty());
    EXPECT_TRUE(R.pl_iono.empty());
    ASSERT_EQ(R.pl_cb.size(), 1u);
}

TEST(StationNullPin, ABlockWithoutANullDirectionTakesTheUnchangedPath) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) { j["network"]["station_local_null_pin"] = true; },
                            cfg, "nonull"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    NetStation st;
    // A full-rank block and a weak direction just above the bar (columns 1 and 2 separated by
    // 1e-3 in one row): no row, no parked column, no counter; the weak direction keeps its
    // columns in the harvest, so its values are carried with their own large variance.
    for (const double eps : {0.5, 1e-3}) {
        StationAssembly R = nullBlock(eps);
        const Eigen::Index rows0 = R.A_r.rows();
        EXPECT_EQ(networkStationNullPin(ctx, st, R), 0) << eps;
        EXPECT_EQ(R.A_r.rows(), rows0) << eps;
        EXPECT_EQ(R.null_pin_rows, 0) << eps;
        EXPECT_TRUE(R.parked_cols.empty()) << eps;
        EXPECT_EQ(R.pl_rpb.size(), 1u) << eps;
        EXPECT_EQ(R.pl_iono.size(), 1u) << eps;
    }
    EXPECT_EQ(ctx.counters.sen_park_station_epochs, 0);
    EXPECT_EQ(ctx.counters.sen_null_pin_rows, 0);
}

TEST(StationNullPin, ADirectionCoupledToTheBorderIsNotPinned) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) { j["network"]["station_local_null_pin"] = true; },
                            cfg, "coupled"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    NetStation st;
    // Local column 2 equals column 1 plus a tiny share of the shared column 1: the local block's
    // direction (0, 1, -1) is below the bar, but its residue A_r v lies along a shared column, so
    // a pin would hand the border information (a datum direction of the network).
    StationAssembly R = nullBlock(0.0);
    for (Eigen::Index r = 0; r < R.A_r.rows(); ++r) R.A_r(r, 2) += 1e-7 * R.A_s(r, 1);
    const Eigen::Index rows0 = R.A_r.rows();
    EXPECT_EQ(networkStationNullPin(ctx, st, R), 0);
    EXPECT_EQ(R.A_r.rows(), rows0);
    EXPECT_EQ(ctx.counters.sen_null_coupled_skipped, 1);
    EXPECT_TRUE(R.parked_cols.empty());
}

TEST(StationNullPin, OffLeavesTheAssemblyUntouched) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json&) {}, cfg, "park_off"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    NetStation st;
    StationAssembly R;
    R.ok = true;
    R.A_r = Eigen::MatrixXd::Zero(3, 2);
    R.A_r.col(0).setOnes();
    EXPECT_EQ(networkStationNullPin(ctx, st, R), 0);
    EXPECT_TRUE(R.prior_cols.empty());
    EXPECT_TRUE(R.parked_cols.empty());
    EXPECT_EQ(ctx.counters.sen_park_columns, 0);
}

// ===========================================================================
// The edge step
// ===========================================================================

TEST(StationEntryStep, ARefusalFromTheFirstEpochThenAReturn) {
    net::StationEntryRecord rec;
    int refusals = 0;
    for (int k = 0; k < 25; ++k) {
        const StationEntryEdges e =
            networkStationEntryStep(rec, false, true, 0, "", 259200.0 + 30.0 * k, "RPB");
        refusals += e.refused ? 1 : 0;
        EXPECT_FALSE(e.returned);
    }
    EXPECT_EQ(refusals, 1);
    EXPECT_EQ(rec.out_epochs, 25);
    EXPECT_DOUBLE_EQ(rec.refused_at_tow, 259200.0);
    EXPECT_EQ(rec.family, "RPB");
    const StationEntryEdges back = networkStationEntryStep(rec, true, false, 0, "", 259950.0, "");
    EXPECT_TRUE(back.returned);
    EXPECT_TRUE(back.first_entry);
    EXPECT_EQ(back.out_epochs, 25);
    EXPECT_TRUE(rec.ever_in_solve);
    // A later refusal and return is not a first entry.
    rec.out_epochs = 0;
    networkStationEntryStep(rec, false, true, 0, "", 260000.0, "");
    EXPECT_EQ(rec.family, "unnamed");
    const StationEntryEdges again = networkStationEntryStep(rec, true, false, 0, "", 260030.0, "");
    EXPECT_TRUE(again.returned);
    EXPECT_FALSE(again.first_entry);
    EXPECT_EQ(again.out_epochs, 1);
}

TEST(StationEntryStep, AParkEpisodeOpensAndCloses) {
    net::StationEntryRecord rec;
    const StationEntryEdges a = networkStationEntryStep(rec, true, false, 1, "RPB", 0.0, "");
    EXPECT_TRUE(a.park_opened);
    const StationEntryEdges b = networkStationEntryStep(rec, true, false, 1, "RPB", 30.0, "");
    EXPECT_FALSE(b.park_opened);
    EXPECT_EQ(rec.park_epochs, 2);
    const StationEntryEdges c = networkStationEntryStep(rec, true, false, 0, "", 60.0, "");
    EXPECT_TRUE(c.park_closed);
    EXPECT_EQ(c.park_epochs, 2);
    EXPECT_EQ(rec.park_epochs, 0);
}

// ===========================================================================
// The cold drop
// ===========================================================================

TEST(StationColdDrop, DropsTheStationsCarriesAndChainMemoryOnly) {
    net::NetworkEntities e;
    const LocalCarry lc{1.0, 0.01, 100.0};
    for (int s = 0; s < 2; ++s) {
        const StationId sid{s};
        net::put(e.stations, sid, &net::StationState::zwd) = lc;
        net::put(e.stations, sid, &net::StationState::signals, std::string("Galileo_RPB_L1C"),
                 &net::StationSignalState::rpb) = lc;
        net::put(e.stations, sid, &net::StationState::signals, std::string(""),
                 &net::StationSignalState::rx_clk) = lc;
        net::put(e.stations, sid, &net::StationState::signals, std::string("E|L1C"),
                 &net::StationSignalState::eq34_rx_parent) = std::string("E11");
        net::putLos(e, s, "E11", &net::LosState::iono_blk) = lc;
        net::putArcAmb(e, s, "E11_L1C", &net::ArcState::amb) = lc;
        net::putArcAmb(e, s, "E11_L1C", &net::ArcState::eq34_prev_target) = 0.0;
    }
    net::putSig(e, "E11_L1C", &net::SatSignalState::anchor_incumbent) = std::string("AAAA");
    net::putSig(e, "E12_L1C", &net::SatSignalState::anchor_incumbent) = std::string("BBBB");

    const StationColdDrop d = networkStationColdDrop(e, 0, "AAAA");
    EXPECT_EQ(d.carries, 5);       // zwd, rpb, rx_clk, iono_blk, amb
    EXPECT_EQ(d.chain_cells, 3);   // the parent edge, the pin target, E11's incumbent
    EXPECT_EQ(net::get(e.stations, StationId{0}, &net::StationState::zwd), nullptr);
    EXPECT_EQ(net::getLos(e.los, 0, "E11", &net::LosState::iono_blk), nullptr);
    EXPECT_EQ(net::getArcAmb(e.arcs, 0, "E11_L1C", &net::ArcState::amb), nullptr);
    EXPECT_EQ(net::getSig(e.sat_signals, "E11_L1C", &net::SatSignalState::anchor_incumbent), nullptr);
    // The other station and the other satellite are untouched.
    EXPECT_NE(net::get(e.stations, StationId{1}, &net::StationState::zwd), nullptr);
    EXPECT_NE(net::getLos(e.los, 1, "E11", &net::LosState::iono_blk), nullptr);
    EXPECT_NE(net::getArcAmb(e.arcs, 1, "E11_L1C", &net::ArcState::amb), nullptr);
    EXPECT_NE(net::getArcAmb(e.arcs, 1, "E11_L1C", &net::ArcState::eq34_prev_target), nullptr);
    EXPECT_NE(net::getSig(e.sat_signals, "E12_L1C", &net::SatSignalState::anchor_incumbent), nullptr);
}
