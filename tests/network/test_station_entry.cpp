/**
 * @file test_station_entry.cpp
 * @brief The park of a rowless local column (network.station_rowless_column_park) and the cold
 *        entry of a refused station (network.station_entry_cold).
 *
 * Covered:
 *   - the keys: off by default, loaded on;
 *   - the normal equations: a station whose block holds a column with no row fails addStation;
 *     with the park's unit prior on that column it is admitted and every other estimate equals
 *     the solve of the same station without the column, while the parked column stays at its
 *     linearization point;
 *   - the park on an assembly: the rowless column is parked (pre-PD entry, tag, counters), a
 *     column with a row, a pre-PD prior or a post-PD carry is not, the parked column leaves the
 *     harvest lists, and nothing happens with the key off;
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
    neq.addStation(A_s, A_r, w, y);
}

}  // namespace

// ===========================================================================
// The keys
// ===========================================================================

TEST(StationEntryConfig, OffByDefaultAndLoadedOn) {
    GnssConfig off, on;
    ASSERT_TRUE(loadRefWith([](nlohmann::json&) {}, off, "off"));
    EXPECT_FALSE(off.getNetworkConfig().station_rowless_column_park);
    EXPECT_FALSE(off.getNetworkConfig().station_entry_cold);
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) {
        j["network"]["station_rowless_column_park"] = true;
        j["network"]["station_entry_cold"] = true;
    }, on, "on"));
    EXPECT_TRUE(on.getNetworkConfig().station_rowless_column_park);
    EXPECT_TRUE(on.getNetworkConfig().station_entry_cold);
}

// ===========================================================================
// The normal equations: the park decouples the column exactly
// ===========================================================================

TEST(StationRowlessPark, TheParkedColumnIsDecoupledExactly) {
    const ToyStation t;
    // Without the park the block fails the positive-definiteness test.
    {
        NetworkNormalEquations neq(2);
        addAnchor(neq);
        EXPECT_THROW(neq.addStation(t.A_s, t.A_r, t.w, t.y), std::exception);
    }
    // With the park's unit prior at the linearization point.
    NetworkNormalEquations parked(2);
    addAnchor(parked);
    NetworkNormalEquations::StationPrior pin;
    pin.cols = {2};
    pin.dNrr = Eigen::VectorXd::Constant(1, 1.0);
    pin.dur = Eigen::VectorXd::Zero(1);
    std::size_t ip = 0;
    ASSERT_NO_THROW(ip = parked.addStation(t.A_s, t.A_r, t.w, t.y, pin));
    const Eigen::VectorXd xs_p = parked.solveShared();
    const Eigen::VectorXd xr_p = parked.reconstructStation(ip, xs_p);
    // The same station without the column.
    NetworkNormalEquations ref(2);
    addAnchor(ref);
    const std::size_t ir = ref.addStation(t.A_s, t.A_r.leftCols(2), t.w, t.y);
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

TEST(StationRowlessPark, ParksOnlyTheRowlessColumnAndLeavesTheHarvest) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) { j["network"]["station_rowless_column_park"] = true; },
                            cfg, "park"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.rowless_park);
    NetStation st;
    st.name = "AAAA";
    StationAssembly R;
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Ones(4, 2);
    R.A_r = Eigen::MatrixXd::Zero(4, 5);
    R.A_r.col(0).setOnes();                 // a column with rows
    // column 1: a pre-PD prior; column 2: a post-PD carry; columns 3 and 4: nothing.
    R.prior_cols = {1};
    R.prior_dNrr = Eigen::VectorXd::Constant(1, 4.0);
    R.prior_dur = Eigen::VectorXd::Constant(1, 0.5);
    R.prior_tags = {RowTag{ContributionKind::PriorRxCbDatumPrePd, {}, {}}};
    R.carry_cols = {2};
    R.carry_dNrr = Eigen::VectorXd::Constant(1, 9.0);
    R.carry_dur = Eigen::VectorXd::Zero(1);
    R.carry_tags = {RowTag{ContributionKind::PriorRxCbCarry, {}, {}}};
    R.pl_rpb = {std::make_tuple(std::string("Galileo_RPB_L5Q"), Eigen::Index{3}, 0.0),
                std::make_tuple(std::string("Galileo_RPB_L1C"), Eigen::Index{0}, 0.0)};
    R.pl_cb = {std::make_tuple(std::string("Galileo_ISC_C5Q"), Eigen::Index{4}, 0.0)};

    EXPECT_EQ(networkStationRowlessPark(ctx, st, R), 2);
    ASSERT_EQ(R.prior_cols.size(), 3u);
    EXPECT_EQ(R.prior_cols[0], 1);
    EXPECT_EQ(R.prior_cols[1], 3);
    EXPECT_EQ(R.prior_cols[2], 4);
    EXPECT_DOUBLE_EQ(R.prior_dNrr(0), 4.0);   // the existing entry is kept
    EXPECT_DOUBLE_EQ(R.prior_dur(0), 0.5);
    EXPECT_DOUBLE_EQ(R.prior_dNrr(1), 1.0);
    EXPECT_DOUBLE_EQ(R.prior_dur(1), 0.0);
    EXPECT_EQ(R.prior_tags[1].kind, ContributionKind::PriorRowlessPark);
    EXPECT_EQ(R.prior_tags[2].kind, ContributionKind::PriorRowlessPark);
    ASSERT_EQ(R.parked_cols.size(), 2u);
    EXPECT_EQ(R.parked_cols[0], 3);
    EXPECT_EQ(R.parked_cols[1], 4);
    // The parked columns left the harvest lists; the column with rows stayed.
    ASSERT_EQ(R.pl_rpb.size(), 1u);
    EXPECT_EQ(std::get<1>(R.pl_rpb[0]), 0);
    EXPECT_TRUE(R.pl_cb.empty());
    EXPECT_EQ(ctx.counters.sen_park_station_epochs, 1);
    EXPECT_EQ(ctx.counters.sen_park_columns, 2);
    // The tag check closes over the new kind.
    const StationTagCheck chk = checkStationTags(R);
    EXPECT_EQ(chk.untagged, 0);
}

TEST(StationRowlessPark, OffLeavesTheAssemblyUntouched) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json&) {}, cfg, "park_off"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    NetStation st;
    StationAssembly R;
    R.ok = true;
    R.A_r = Eigen::MatrixXd::Zero(3, 2);
    R.A_r.col(0).setOnes();
    EXPECT_EQ(networkStationRowlessPark(ctx, st, R), 0);
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
