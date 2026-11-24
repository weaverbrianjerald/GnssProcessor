/**
 * @file test_station_entry.cpp
 * @brief The single-band receiver phase-bias park (network.station_single_band_park) and the cold
 *        entry of a refused station (network.station_entry_cold).
 *
 * Covered:
 *   - the keys: off by default, loaded on;
 *   - the single-band test: read on the phase rows that enter the solve (a code row and an
 *     excluded row do not make a dual-band line of sight); the return drops the carries of the
 *     whole free direction (the held receiver phase biases, the constellation's inter-system
 *     offset and slant ionospheres) at that station only;
 *   - why holding moves nothing else: on blocks with a free direction, a held or pinned column of
 *     it leaves every other estimate, local and border, equal to the problem without it, and two
 *     held values give the same border to rounding; the measure-only diagnostic counts the free
 *     direction and changes nothing, and counts none on a full-rank block or a weak direction
 *     just above the bar;
 *   - the edge step: a station refused for 25 epochs from its first epoch and then admitted gives
 *     one decoupling edge and one re-coupling edge, a first entry, 25 epochs out; a park episode
 *     opens, gathers its keys and closes;
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
#include "network/state/StationParams.hpp"

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
    // The loader's default: the two keys left out of the file.  The simulator reference file itself arms
    // station_single_band_park since 2026-11-23 (the author's "Yes, arm all four"), so the default is read with
    // the keys erased, not from the file as it stands.
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) {
        j["network"].erase("station_single_band_park");
        j["network"].erase("station_entry_cold");
    }, off, "off"));
    EXPECT_FALSE(off.getNetworkConfig().station_single_band_park);
    EXPECT_FALSE(off.getNetworkConfig().station_entry_cold);
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) {
        j["network"]["station_single_band_park"] = true;
        j["network"]["station_entry_cold"] = true;
    }, on, "on"));
    EXPECT_TRUE(on.getNetworkConfig().station_single_band_park);
    EXPECT_TRUE(on.getNetworkConfig().station_entry_cold);
}

// ===========================================================================
// The normal equations: the park decouples the column exactly
// ===========================================================================

TEST(StationNullDirection, APinAlongARowlessColumnMovesNothingElse) {
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
// The single-band test and the return
// ===========================================================================

TEST(StationSingleBandPark, ReadsTheRowsThatEnterTheSolve) {
    // G01 phase on L1C and L2W (dual-band); E02 and E07 phase on L1C only, E07 also code on
    // C5Q (a code row does not make a dual-band phase line of sight).
    std::vector<std::string> sats = {"G01", "G01", "E02", "E07", "E07"};
    std::vector<std::string> sigs = {"L1C", "L2W", "L1C", "L1C", "C5Q"};
    EXPECT_EQ(networkSingleBandConstellations(sats, sigs, {}), (std::set<System>{System::Galileo}));
    // E02 gains L5Q: Galileo is dual-band.
    sats.push_back("E02");
    sigs.push_back("L5Q");
    EXPECT_TRUE(networkSingleBandConstellations(sats, sigs, {}).empty());
    // The same L5Q row excluded at zero weight does not count (it does not enter the solve).
    std::vector<char> excluded(sats.size(), 0);
    excluded.back() = 1;
    EXPECT_EQ(networkSingleBandConstellations(sats, sigs, excluded),
              (std::set<System>{System::Galileo}));
    // GPS loses L2W: both single-band.
    excluded[1] = 1;
    EXPECT_EQ(networkSingleBandConstellations(sats, sigs, excluded),
              (std::set<System>{System::GPS, System::Galileo}));
}

TEST(StationSingleBandPark, TheReturnRebornsTheWholeDirectionLoose) {
    net::NetworkEntities e;
    const LocalCarry lc{1.0, 0.01, 100.0};
    const StationId sid{0};
    net::put(e.stations, sid, &net::StationState::signals, std::string("Galileo_RPB_L1C"),
             &net::StationSignalState::rpb) = lc;
    net::put(e.stations, sid, &net::StationState::signals, std::string("Galileo"),
             &net::StationSignalState::rx_clk) = lc;              // the inter-system offset
    net::put(e.stations, sid, &net::StationState::signals, std::string(""),
             &net::StationSignalState::rx_clk) = lc;              // the receiver clock (kept)
    net::put(e.stations, sid, &net::StationState::signals, std::string("GPS_RPB_L1C"),
             &net::StationSignalState::rpb) = lc;                 // GPS (kept)
    net::putLos(e, 0, "E02", &net::LosState::iono_blk) = lc;
    net::putLos(e, 0, "G05", &net::LosState::iono_blk) = lc;      // GPS (kept)
    net::putLos(e, 1, "E02", &net::LosState::iono_blk) = lc;      // another station (kept)
    EXPECT_EQ(networkStationSingleBandReturn(e, 0, {"Galileo_RPB_L1C"}), 3);
    EXPECT_EQ(net::get(e.stations, sid, &net::StationState::signals, std::string("Galileo_RPB_L1C"),
                       &net::StationSignalState::rpb), nullptr);
    EXPECT_EQ(net::get(e.stations, sid, &net::StationState::signals, std::string("Galileo"),
                       &net::StationSignalState::rx_clk), nullptr);
    EXPECT_EQ(net::getLos(e.los, 0, "E02", &net::LosState::iono_blk), nullptr);
    EXPECT_NE(net::get(e.stations, sid, &net::StationState::signals, std::string(""),
                       &net::StationSignalState::rx_clk), nullptr);
    EXPECT_NE(net::get(e.stations, sid, &net::StationState::signals, std::string("GPS_RPB_L1C"),
                       &net::StationSignalState::rpb), nullptr);
    EXPECT_NE(net::getLos(e.los, 0, "G05", &net::LosState::iono_blk), nullptr);
    EXPECT_NE(net::getLos(e.los, 1, "E02", &net::LosState::iono_blk), nullptr);
}

// ===========================================================================
// Why the single-band hold moves nothing else: a block with a null direction
// ===========================================================================

namespace {

/// An assembly over 2 shared and 3 local columns whose local columns 1 and 2 enter every row
/// together (a single-band constellation's phase bias and ionosphere shift together), so
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
    return R;
}

}  // namespace

TEST(StationNullDirection, AHoldOnOneColumnOfTheNullDirectionMovesNothingElse) {
    // The declared rule holds one column of the free direction (here column 1) at a value; every
    // other estimate, local and border, equals the problem with the direction removed.
    StationAssembly R = nullBlock(0.0);
    {
        NetworkNormalEquations neq(2);
        addAnchor(neq);
        EXPECT_THROW(addRows(neq, R.A_s, R.A_r, R.w, R.y), std::exception);
    }
    NetworkNormalEquations held(2);
    addAnchor(held);
    NetworkNormalEquations::StationPrior hold;
    hold.cols = {1};
    hold.dNrr = Eigen::VectorXd::Constant(1, 1.0);
    hold.dur = Eigen::VectorXd::Constant(1, 0.25);   // held at 0.25 from the linearization point
    std::size_t ih = 0;
    ASSERT_NO_THROW(ih = addRows(held, R.A_s, R.A_r, R.w, R.y, hold));
    const Eigen::VectorXd xs_h = held.solveShared();
    const Eigen::VectorXd xr_h = held.reconstructStation(ih, xs_h);
    Eigen::MatrixXd A_r_merged(R.A_r.rows(), 2);
    A_r_merged.col(0) = R.A_r.col(0);
    A_r_merged.col(1) = R.A_r.col(1);
    NetworkNormalEquations merged(2);
    addAnchor(merged);
    const std::size_t im = addRows(merged, R.A_s, A_r_merged, R.w, R.y);
    const Eigen::VectorXd xs_m = merged.solveShared();
    const Eigen::VectorXd xr_m = merged.reconstructStation(im, xs_m);
    for (Eigen::Index k = 0; k < xs_h.size(); ++k) EXPECT_NEAR(xs_h(k), xs_m(k), 1e-10);   // the border
    EXPECT_NEAR(xr_h(0), xr_m(0), 1e-10);                     // the other local column
    EXPECT_NEAR(xr_h(1) + xr_h(2), xr_m(1), 1e-10);           // the observable sum
    EXPECT_NEAR(xr_h(1), 0.25, 1e-10);                        // the held column at its value
}

TEST(StationNullDirection, TwoHeldValuesGiveTheSameBorder) {
    // HeldAtSeed: the held column leaves the block and enters every row as a constant.  Held at
    // 0 or at 0.5 m, the border solution is the same to rounding; only the other column of the
    // free direction moves, by the held offset.
    const StationAssembly R = nullBlock(0.0);
    Eigen::VectorXd xs[2], xr[2];
    const double held[2] = {0.0, 0.5};
    for (int k = 0; k < 2; ++k) {
        Eigen::MatrixXd A_r_kept(R.A_r.rows(), 2);
        A_r_kept.col(0) = R.A_r.col(0);
        A_r_kept.col(1) = R.A_r.col(2);
        const Eigen::VectorXd y = R.y - R.A_r.col(1) * held[k];
        NetworkNormalEquations neq(2);
        addAnchor(neq);
        const std::size_t i = addRows(neq, R.A_s, A_r_kept, R.w, y);
        xs[k] = neq.solveShared();
        xr[k] = neq.reconstructStation(i, xs[k]);
    }
    for (Eigen::Index j = 0; j < xs[0].size(); ++j) EXPECT_NEAR(xs[0](j), xs[1](j), 1e-12);
    EXPECT_NEAR(xr[0](0), xr[1](0), 1e-12);                      // a column outside the direction
    EXPECT_NEAR(xr[1](1) - xr[0](1), -0.5, 1e-12);                // the other column takes the offset
}

TEST(StationNullDirection, TheDiagnosticCountsAndChangesNothing) {
    GnssConfig cfg;
    ASSERT_TRUE(loadRefWith([](nlohmann::json& j) { j["network"]["station_single_band_park"] = true; },
                            cfg, "diag"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    NetStation st;
    const StationAssembly null_block = nullBlock(0.0);
    StationAssembly R = null_block;
    EXPECT_EQ(networkStationNullDiagnostic(ctx, st, R), 1);
    EXPECT_TRUE(R.A_r.isApprox(null_block.A_r));
    EXPECT_EQ(R.A_r.rows(), null_block.A_r.rows());
    EXPECT_TRUE(R.prior_cols.empty());
    // A full-rank block and a weak direction just above the bar count nothing.
    for (const double eps : {0.5, 1e-3}) {
        const StationAssembly W = nullBlock(eps);
        EXPECT_EQ(networkStationNullDiagnostic(ctx, st, W), 0) << eps;
    }
    EXPECT_EQ(ctx.counters.sen_null_diag_directions, 1);
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
    const StationEntryEdges a =
        networkStationEntryStep(rec, true, false, 1, "RPB", 0.0, "", {"Galileo_RPB_L1C"});
    EXPECT_TRUE(a.park_opened);
    const StationEntryEdges b = networkStationEntryStep(rec, true, false, 1, "RPB", 30.0, "");
    EXPECT_FALSE(b.park_opened);
    EXPECT_EQ(rec.park_epochs, 2);
    const StationEntryEdges c = networkStationEntryStep(rec, true, false, 0, "", 60.0, "");
    EXPECT_TRUE(c.park_closed);
    EXPECT_EQ(c.park_epochs, 2);
    EXPECT_EQ(c.unparked_keys, (std::vector<std::string>{"Galileo_RPB_L1C"}));
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
