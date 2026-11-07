/**
 * @file test_network_anchor_level_hold.cpp
 * @brief The anchor-level hold of the phase datum (network/solve/AnchorLevelHold.hpp, card ALD):
 *        the level directions of a two-constellation block, the hold row, the station step on a
 *        synthetic block (the tenure scope, the carried mask, the all-stations scope), the serial
 *        tally's edges and events, and the loader's interlocks.
 */

#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/AnchorLevelHold.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"
#include "math/TimeUtils.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr double kMu2 = (154.0 / 120.0) * (154.0 / 120.0);   // L2 against L1
constexpr double kMu5 = (154.0 / 115.0) * (154.0 / 115.0);   // L5 and E5a against L1

/**
 * A synthetic two-constellation station block: three satellites per constellation, two bands
 * each.  Columns, in state order: CLK, ZWD, ISB_Galileo, then per GPS satellite (IONO, AMB
 * L1C, AMB L2W), GPS_DCB, then per Galileo satellite (IONO, AMB L1C, AMB L5Q), Galileo_DCB.
 * Rows per satellite: code band 1, code band 2, phase band 1, phase band 2.  The receiver
 * differential code bias d enters the code rows in its clock-free split (band 1: -beta d,
 * band 2: -alpha d, alpha - beta = 1, alpha = gamma beta, gamma the second band's ionospheric
 * factor), so each constellation's geometry-free level is the data-null direction
 * [iono +1, amb band 1 +1, amb band 2 +gamma, d +(gamma - 1)] on its own columns and nothing
 * on the clock, the wet delay, the inter-system bias or the other constellation.
 */
struct SyntheticBlock {
    std::vector<ParamRef> refs;      ///< Column order = state order.
    std::vector<System>   col_sys;
    std::vector<char>     col_iono;
    Eigen::MatrixXd       A;         ///< The data rows.
    Eigen::VectorXd       w;
    std::vector<RowTag>   tags;
    int clk{-1}, zwd{-1}, isb{-1}, dcb_g{-1}, dcb_e{-1};
    std::vector<int> iono_g, amb1_g, amb2_g, iono_e, amb1_e, amb5_e;
    std::vector<SatId> sats_g, sats_e;

    SyntheticBlock() {
        clk = add(rxClockRef());
        zwd = add(zwdRef());
        isb = add(isbRef(System::Galileo));
        for (const char* s : {"G01", "G02", "G03"}) {
            const SatId sat = *SatId::parse(s);
            sats_g.push_back(sat);
            iono_g.push_back(add(slantIonoRef(sat)));
            amb1_g.push_back(add(ambiguityRef(sat, *SignalId::fromObsCode(System::GPS, "L1C"))));
            amb2_g.push_back(add(ambiguityRef(sat, *SignalId::fromObsCode(System::GPS, "L2W"))));
        }
        dcb_g = add(rxCodeBiasRef(RxCbKey{System::GPS, ParamForm::Dcb, FrequencyBand::Unknown, SignalId{}, 0}));
        for (const char* s : {"E01", "E02", "E03"}) {
            const SatId sat = *SatId::parse(s);
            sats_e.push_back(sat);
            iono_e.push_back(add(slantIonoRef(sat)));
            amb1_e.push_back(add(ambiguityRef(sat, *SignalId::fromObsCode(System::Galileo, "L1C"))));
            amb5_e.push_back(add(ambiguityRef(sat, *SignalId::fromObsCode(System::Galileo, "L5Q"))));
        }
        dcb_e = add(rxCodeBiasRef(RxCbKey{System::Galileo, ParamForm::Dcb, FrequencyBand::Unknown, SignalId{}, 0}));
        const int n = static_cast<int>(refs.size());
        col_sys.assign(static_cast<std::size_t>(n), System::Unknown);
        col_iono.assign(static_cast<std::size_t>(n), 0);
        for (int c = 0; c < n; ++c) {
            const ParamRef& r = refs[static_cast<std::size_t>(c)];
            if (r.kind == NetworkParamKind::Ambiguity || r.kind == NetworkParamKind::SlantIono)
                col_sys[static_cast<std::size_t>(c)] = r.sat.sys;
            if (r.kind == NetworkParamKind::RxCodeBias) col_sys[static_cast<std::size_t>(c)] = r.sys;
            if (r.kind == NetworkParamKind::SlantIono) col_iono[static_cast<std::size_t>(c)] = 1;
        }
        A = Eigen::MatrixXd::Zero(24, n);
        w = Eigen::VectorXd::Zero(24);
        int r = 0;
        const double maps_g[] = {1.5, 2.0, 3.0};
        const double maps_e[] = {1.2, 2.5, 4.0};
        for (int k = 0; k < 3; ++k)
            satelliteRows(r, maps_g[k], false, iono_g[k], amb1_g[k], amb2_g[k], dcb_g, kMu2);
        for (int k = 0; k < 3; ++k)
            satelliteRows(r, maps_e[k], true, iono_e[k], amb1_e[k], amb5_e[k], dcb_e, kMu5);
    }

    int add(const ParamRef& r) { refs.push_back(r); return static_cast<int>(refs.size()) - 1; }

    void satelliteRows(int& r, double m, bool galileo, int iono, int amb1, int amb2, int dcb,
                       double gamma) {
        const double alpha = gamma / (gamma - 1.0), beta = 1.0 / (gamma - 1.0);
        const double w_code = 1.0 / (0.3 * 0.3), w_phase = 1.0 / (0.003 * 0.003);
        const auto common = [&](int row) {
            A(row, clk) = 1.0;
            A(row, zwd) = m;
            if (galileo) A(row, isb) = 1.0;
        };
        common(r); A(r, iono) = 1.0;    A(r, dcb) = -beta;  w(r) = w_code;  tags.push_back(RowTag{ContributionKind::DataCode, {}, {}});  ++r;
        common(r); A(r, iono) = gamma;  A(r, dcb) = -alpha; w(r) = w_code;  tags.push_back(RowTag{ContributionKind::DataCode, {}, {}});  ++r;
        common(r); A(r, iono) = -1.0;   A(r, amb1) = 1.0;   w(r) = w_phase; tags.push_back(RowTag{ContributionKind::DataPhase, {}, {}}); ++r;
        common(r); A(r, iono) = -gamma; A(r, amb2) = 1.0;   w(r) = w_phase; tags.push_back(RowTag{ContributionKind::DataPhase, {}, {}}); ++r;
    }

    /// The expected unit level direction of a constellation.
    Eigen::VectorXd expected(System sys) const {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(refs.size()));
        const bool g = sys == System::GPS;
        const double gamma = g ? kMu2 : kMu5;
        for (int c : (g ? iono_g : iono_e)) v(c) = 1.0;
        for (int c : (g ? amb1_g : amb1_e)) v(c) = 1.0;
        for (int c : (g ? amb2_g : amb5_e)) v(c) = gamma;
        v(g ? dcb_g : dcb_e) = gamma - 1.0;
        return v / v.norm();
    }

    /// A parameter set with these columns at the given linearization points (slot k = column k).
    StationParams params(double x0) const {
        StationParams set;
        for (std::size_t k = 0; k < refs.size(); ++k) {
            LocalSlot slot;
            slot.ref = refs[k];
            slot.x_lin = x0 + 0.1 * static_cast<double>(k);
            addParam(set, slot);
        }
        return set;
    }
};

std::string networkConfig(const std::string& network_extra) {
    return std::string(R"({
        "processing": {"mode": "ppp-rtk-network", "active_systems": ["GPS", "Galileo"],
                       "signal_priorities": {"GPS": "WC", "Galileo": "QX"}},
        "network": {"clock_datum_station": "AAAA")") + network_extra + R"(,
                    "stations": [{"name": "AAAA", "position_ecef": [6378137.0, 0.0, 0.0]},
                                 {"name": "BBBB", "position_ecef": [0.0, 6378137.0, 0.0]}]}
    })";
}

/// Loads @p body as a configuration; @p tag names the temporary file.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    const fs::path p = gnss_test::tempDir() / ("ald_hold_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

LocalCarry carryAt(double value, double now_s) {
    LocalCarry lc;
    lc.value_abs = value;
    lc.var = 1e-4;
    lc.last_seen_s = now_s;
    return lc;
}

/// One station over a configuration that arms the hold, with the synthetic block as its slot
/// and every level column carried at its linearization point plus a known offset.
struct HoldFixture {
    SyntheticBlock                       blk;
    GnssConfig                           cfg;
    std::unique_ptr<NetworkTestRun>      run;
    std::unique_ptr<GlobalIonosphereMap> gim;
    StationAssembly                      slot;
    double                               now_s{0.0};
    static constexpr double kOffset = 0.25;   ///< carried value minus linearization point, every column

    explicit HoldFixture(const std::string& network_extra, bool carry_galileo = false) {
        EXPECT_TRUE(loadInto(network_extra.size() > 40 ? "all" : "h1", networkConfig(network_extra), cfg));
        run = std::make_unique<NetworkTestRun>(cfg);
        run->setup().stations.resize(1);
        station().name = "AAAA";
        station().params = blk.params(5.0);
        NetworkEpoch& e = run->newEpoch(GnssTime{2313, 259230.0});
        now_s = TimeUtils::continuousGpsSeconds(e.se.time);
        e.clock = EpochClock{30.0, now_s, true};
        run->ctx().counters.n_loop_epochs = 7;
        const int n = static_cast<int>(blk.refs.size());
        for (int k = 0; k < n; ++k) slot.h2local[k] = k;
        slot.A_r = blk.A;
        slot.A_s = Eigen::MatrixXd::Zero(24, 2);
        slot.w = blk.w;
        slot.y = Eigen::VectorXd::Zero(24);
        slot.row_tags = blk.tags;
        slot.n_data_rows = 24;
        // The carries: every GPS level column (and Galileo's when asked), at x_lin + kOffset.
        carryConstellation(blk.sats_g, blk.iono_g, blk.amb1_g, blk.amb2_g, blk.dcb_g);
        if (carry_galileo) carryConstellation(blk.sats_e, blk.iono_e, blk.amb1_e, blk.amb5_e, blk.dcb_e);
    }

    void carryConstellation(const std::vector<SatId>& sats, const std::vector<int>& iono,
                            const std::vector<int>& amb1, const std::vector<int>& amb2, int dcb) {
        net::NetworkEntities& ent = run->ctx().entities;
        const StationParams& set = station().params;
        for (std::size_t k = 0; k < sats.size(); ++k) {
            net::put(ent.los, LosId{StationId{0}, sats[k]}, &net::LosState::iono_blk) =
                carryAt(slotAt(set, iono[k]).x_lin + kOffset, now_s);
            for (int c : {amb1[k], amb2[k]}) {
                const ParamRef& r = slotAt(set, c).ref;
                net::put(ent.arcs, ArcId{StationId{0}, r.sat, r.signal}, &net::ArcState::amb) =
                    carryAt(slotAt(set, c).x_lin + kOffset, now_s);
            }
        }
        net::put(ent.stations, StationId{0}, &net::StationState::signals,
                 keyText(slotAt(set, dcb).ref), &net::StationSignalState::rx_cb) =
            carryAt(slotAt(set, dcb).x_lin + kOffset, now_s);
    }

    NetStation& station() { return run->setup().stations[0]; }

    StationBuild build() {
        NetworkEpoch& e = run->ep();
        return StationBuild{run->ctx(), run->config(), run->est(), run->setup(), e.se, e.clock,
                            e.pass_a, e.events, gim, 0, station(), slot};
    }
};

const std::string kHoldOn = R"(, "phase_datum_prioritized": true, "phase_datum_anchor_level_hold": true)";
const std::string kHoldAll = kHoldOn + R"(, "phase_datum_anchor_level_hold_all_stations": true)";

}  // namespace

// ---- The directions -----------------------------------------------------------------------

TEST(NetworkAnchorLevelHoldTest, EachConstellationHasOneUnitLevelDirectionOnItsOwnColumns) {
    const SyntheticBlock b;
    const LevelDirections d = anchorLevelDirections(b.A, b.w, b.col_sys, b.col_iono,
                                                    {System::GPS, System::Galileo});
    EXPECT_EQ(d.n_null, 2);
    ASSERT_EQ(d.found.size(), 2u);
    EXPECT_TRUE(d.refused.empty());
    for (const LevelDirections::Entry& e : d.found) {
        const Eigen::VectorXd want = b.expected(e.sys);
        EXPECT_NEAR(e.c.norm(), 1.0, 1e-12);
        EXPECT_LT((e.c - want).norm(), 1e-6) << systemName(e.sys);
        EXPECT_LT(e.off_sys_max, 1e-7) << systemName(e.sys);
        EXPECT_EQ(e.c(b.clk), 0.0);
        EXPECT_EQ(e.c(b.zwd), 0.0);
        EXPECT_EQ(e.c(b.isb), 0.0);
        // Data-null: every data row is blind to it.
        EXPECT_LT((b.A * e.c).cwiseAbs().maxCoeff(), 1e-9) << systemName(e.sys);
        // The sign: the slant-ionosphere entries positive, the second band's ambiguity by the
        // ionospheric factor, the differential bias by (gamma - 1).
        const bool g = e.sys == System::GPS;
        const int iono = g ? b.iono_g[0] : b.iono_e[0];
        const int amb2 = g ? b.amb2_g[0] : b.amb5_e[0];
        EXPECT_GT(e.c(iono), 0.0);
        EXPECT_NEAR(e.c(amb2) / e.c(iono), g ? kMu2 : kMu5, 1e-9);
        EXPECT_NEAR(e.c(g ? b.dcb_g : b.dcb_e) / e.c(iono), (g ? kMu2 : kMu5) - 1.0, 1e-9);
    }
}

TEST(NetworkAnchorLevelHoldTest, OnlyTheWantedConstellationsAreSplitOut) {
    const SyntheticBlock b;
    const LevelDirections d = anchorLevelDirections(b.A, b.w, b.col_sys, b.col_iono, {System::Galileo});
    EXPECT_EQ(d.n_null, 2);
    ASSERT_EQ(d.found.size(), 1u);
    EXPECT_EQ(d.found[0].sys, System::Galileo);
    EXPECT_LT((d.found[0].c - b.expected(System::Galileo)).norm(), 1e-6);
}

TEST(NetworkAnchorLevelHoldTest, ASecondNullDirectionInsideAConstellationRefusesItAsNotUnique) {
    SyntheticBlock b;
    // Without G03's code rows its slant ionosphere and ambiguities are undetermined among
    // themselves: a second GPS null direction, so GPS has no unique level; Galileo is untouched.
    b.w(8) = 0.0;
    b.w(9) = 0.0;
    const LevelDirections d = anchorLevelDirections(b.A, b.w, b.col_sys, b.col_iono,
                                                    {System::GPS, System::Galileo});
    EXPECT_EQ(d.n_null, 3);
    ASSERT_EQ(d.found.size(), 1u);
    EXPECT_EQ(d.found[0].sys, System::Galileo);
    ASSERT_EQ(d.refused.size(), 1u);
    EXPECT_EQ(d.refused[0].first, System::GPS);
    EXPECT_EQ(d.refused[0].second, LevelHoldSkip::NotUnique);
}

TEST(NetworkAnchorLevelHoldTest, NoColumnsAndNoDataAreRefusedByName) {
    const SyntheticBlock b;
    const LevelDirections none = anchorLevelDirections(b.A, Eigen::VectorXd::Zero(24), b.col_sys,
                                                       b.col_iono, {System::GPS});
    ASSERT_EQ(none.refused.size(), 1u);
    EXPECT_EQ(none.refused[0].second, LevelHoldSkip::NoDataInformation);
    const LevelDirections bds = anchorLevelDirections(b.A, b.w, b.col_sys, b.col_iono, {System::BeiDou});
    ASSERT_EQ(bds.refused.size(), 1u);
    EXPECT_EQ(bds.refused[0].second, LevelHoldSkip::NoColumns);
    EXPECT_STREQ(levelHoldSkipName(LevelHoldSkip::NoColumns), "no_columns");
    EXPECT_STREQ(levelHoldSkipName(LevelHoldSkip::NotUnique), "not_unique");
}

// ---- The row --------------------------------------------------------------------------------

TEST(NetworkAnchorLevelHoldTest, TheRowIsTheDirectionOnTheCarriedColumnsAimedAtTheCarriedLevel) {
    const SyntheticBlock b;
    const Eigen::VectorXd c = b.expected(System::GPS);
    const Eigen::Index n = c.size();
    std::vector<char> carried(static_cast<std::size_t>(n), 0);
    Eigen::VectorXd x_prev = Eigen::VectorXd::Zero(n), x_lin = Eigen::VectorXd::Zero(n);
    double want_prev = 0.0, want_lin = 0.0;
    int want_cols = 0;
    for (Eigen::Index k = 0; k < n; ++k) {
        if (c(k) == 0.0 || k == b.amb1_g[0]) continue;   // G01 L1C pinned: left free
        carried[static_cast<std::size_t>(k)] = 1;
        x_prev(k) = 10.0 + static_cast<double>(k);
        x_lin(k) = 9.5 + 0.5 * static_cast<double>(k);
        want_prev += c(k) * x_prev(k);
        want_lin += c(k) * x_lin(k);
        ++want_cols;
    }
    const LevelHoldRow row = anchorLevelHoldRow(c, carried, x_prev, x_lin);
    EXPECT_EQ(row.n_cols, want_cols);
    EXPECT_NEAR(row.level_prev, want_prev, 1e-12);
    EXPECT_NEAR(row.level_lin, want_lin, 1e-12);
    EXPECT_NEAR(row.y, want_prev - want_lin, 1e-12);
    EXPECT_EQ(row.a_r(b.amb1_g[0]), 0.0);
    EXPECT_EQ(row.a_r(b.clk), 0.0);
    EXPECT_EQ(row.a_r(b.iono_g[1]), c(b.iono_g[1]));
    EXPECT_LT(row.a_r.norm(), 1.0);   // restricted, not re-normalized
}

// ---- The station step -----------------------------------------------------------------------

TEST(NetworkAnchorLevelHoldTest, DuringTenureOneRowHoldsTheCarriedLevelBesideThePins) {
    HoldFixture f(kHoldOn);
    ASSERT_TRUE(f.run->ctx().knobs.pd_anchor_level_hold);
    EXPECT_FALSE(f.run->ctx().knobs.pd_anchor_level_hold_all);
    // The station anchors G01 L1C: tenure of GPS, none of Galileo.
    f.station().pinned_amb_targets["G01_L1C"] = 0.0;
    // G02 L1C pending a re-seed, G03 L2W dropped this epoch: both left free.
    markPendingReseed(f.station().params, SatSignalId{f.blk.sats_g[1], *SignalId::fromObsCode(System::GPS, "L1C")});
    {
        const ParamRef& r = slotAt(f.station().params, f.blk.amb2_g[2]).ref;
        LocalCarry* lc = net::get(f.run->ctx().entities.arcs, ArcId{StationId{0}, r.sat, r.signal}, &net::ArcState::amb);
        ASSERT_NE(lc, nullptr);
        lc->dropped_ep = 7;
    }
    StationBuild b = f.build();
    EXPECT_EQ(networkAnchorLevelHoldRows(b), StationStep::Next);

    ASSERT_EQ(f.slot.level_hold.size(), 1u) << "Galileo is not anchored: no record of it under H1";
    const LevelHoldRecord& rec = f.slot.level_hold[0];
    EXPECT_EQ(rec.sys, System::GPS);
    EXPECT_TRUE(rec.tenure);
    EXPECT_EQ(rec.n_fam, 1);
    EXPECT_TRUE(rec.held);
    EXPECT_EQ(rec.skip, LevelHoldSkip::None);
    EXPECT_EQ(rec.n_null, 2);
    EXPECT_EQ(rec.n_excl_pinned, 1);
    EXPECT_EQ(rec.n_excl_dropped, 2);
    EXPECT_EQ(rec.n_excl_uncarried, 0);
    EXPECT_EQ(rec.n_amb, 3);
    EXPECT_EQ(rec.n_iono, 3);
    EXPECT_EQ(rec.n_rxcb, 1);
    EXPECT_EQ(rec.n_cols, 7);
    EXPECT_LT(rec.off_sys_max, 1e-7);
    // Every carried column sits kOffset above its linearization point, so the innovation is
    // kOffset times the sum of the direction's carried entries.
    const Eigen::VectorXd c = f.blk.expected(System::GPS);
    double sum = 0.0;
    // Carried: the three slant ionospheres, G01 L2W, G02 L2W, G03 L1C and the DCB.
    for (int k : {f.blk.iono_g[0], f.blk.iono_g[1], f.blk.iono_g[2], f.blk.amb2_g[0], f.blk.amb2_g[1],
                  f.blk.amb1_g[2], f.blk.dcb_g})
        sum += c(k);
    EXPECT_NEAR(rec.y, HoldFixture::kOffset * sum, 1e-9);
    EXPECT_NEAR(rec.level_prev - rec.level_lin, rec.y, 1e-12);

    // The block: one row appended after the data rows, a_s zero, the pins' weight, the
    // direction on the carried columns only, tagged with the hold's kind.
    ASSERT_EQ(f.slot.A_r.rows(), 25);
    ASSERT_EQ(f.slot.A_s.rows(), 25);
    EXPECT_EQ(f.slot.level_hold_rows, 1);
    EXPECT_EQ(f.slot.row_tags.size(), 25u);
    EXPECT_EQ(f.slot.row_tags.back().kind, ContributionKind::AnchorLevelHold);
    EXPECT_EQ(f.slot.w(24), kAnchorLevelHoldWeight);
    EXPECT_NEAR(f.slot.y(24), rec.y, 1e-12);
    EXPECT_EQ(f.slot.A_s.row(24).cwiseAbs().maxCoeff(), 0.0);
    EXPECT_EQ(f.slot.A_r(24, f.blk.amb1_g[0]), 0.0);   // pinned
    EXPECT_EQ(f.slot.A_r(24, f.blk.amb1_g[1]), 0.0);   // pending re-seed
    EXPECT_EQ(f.slot.A_r(24, f.blk.amb2_g[2]), 0.0);   // dropped this epoch
    EXPECT_EQ(f.slot.A_r(24, f.blk.clk), 0.0);
    EXPECT_EQ(f.slot.A_r(24, f.blk.iono_e[0]), 0.0);
    EXPECT_NEAR(f.slot.A_r(24, f.blk.iono_g[1]), c(f.blk.iono_g[1]), 1e-6);
    EXPECT_NEAR(f.slot.A_r(24, f.blk.dcb_g), c(f.blk.dcb_g), 1e-6);
    EXPECT_EQ(f.slot.np, 0) << "the hold is not a pin of the total_pins ledger";
}

TEST(NetworkAnchorLevelHoldTest, WithoutTenureAndWithoutTheAllStationsScopeNothingIsBuilt) {
    HoldFixture f(kHoldOn);
    StationBuild b = f.build();
    EXPECT_EQ(networkAnchorLevelHoldRows(b), StationStep::Next);
    EXPECT_TRUE(f.slot.level_hold.empty());
    EXPECT_EQ(f.slot.A_r.rows(), 24);
    EXPECT_EQ(f.slot.level_hold_rows, 0);
}

TEST(NetworkAnchorLevelHoldTest, TheAllStationsScopeHoldsEveryConstellationWithACarriedLevel) {
    HoldFixture f(kHoldAll, /*carry_galileo=*/true);
    ASSERT_TRUE(f.run->ctx().knobs.pd_anchor_level_hold_all);
    StationBuild b = f.build();
    EXPECT_EQ(networkAnchorLevelHoldRows(b), StationStep::Next);
    ASSERT_EQ(f.slot.level_hold.size(), 2u);
    EXPECT_EQ(f.slot.level_hold_rows, 2);
    EXPECT_EQ(f.slot.A_r.rows(), 26);
    for (const LevelHoldRecord& rec : f.slot.level_hold) {
        EXPECT_FALSE(rec.tenure);
        EXPECT_TRUE(rec.held);
        EXPECT_EQ(rec.n_cols, 10);   // three slant ionospheres, six ambiguities, the DCB: nothing pinned or dropped
        EXPECT_EQ(rec.n_excl_pinned, 0);
        EXPECT_EQ(rec.n_excl_dropped, 0);
    }
    EXPECT_EQ(f.slot.level_hold[0].sys, System::Galileo);   // letter order
    EXPECT_EQ(f.slot.level_hold[1].sys, System::GPS);
}

TEST(NetworkAnchorLevelHoldTest, AConstellationWithNoCarriedColumnIsRecordedAndHeldNowhere) {
    HoldFixture f(kHoldAll, /*carry_galileo=*/false);
    StationBuild b = f.build();
    EXPECT_EQ(networkAnchorLevelHoldRows(b), StationStep::Next);
    ASSERT_EQ(f.slot.level_hold.size(), 2u);
    EXPECT_EQ(f.slot.level_hold_rows, 1);
    const LevelHoldRecord& e = f.slot.level_hold[0];
    EXPECT_EQ(e.sys, System::Galileo);
    EXPECT_FALSE(e.held);
    EXPECT_EQ(e.skip, LevelHoldSkip::NoCarriedColumn);
    EXPECT_EQ(e.n_excl_uncarried, 10);
}

// ---- The tally ------------------------------------------------------------------------------

TEST(NetworkAnchorLevelHoldTest, TheTallyRaisesTheHoldsEdgesAsEventsAndKeepsTheMemo) {
    HoldFixture f(kHoldOn);
    NetworkRunContext& ctx = f.run->ctx();
    NetworkRunTraces& traces = f.run->traces();
    traces.events_trace.open(traces.events_trace_path);
    ASSERT_TRUE(traces.events_trace.is_open());
    std::vector<StationAssembly> slots(1);
    const auto record = [](bool held, bool tenure, LevelHoldSkip skip) {
        LevelHoldRecord r;
        r.sys = System::GPS; r.tenure = tenure; r.n_fam = tenure ? 2 : 0; r.held = held; r.skip = skip;
        r.n_cols = held ? 5 : 0; r.n_amb = held ? 3 : 0; r.n_iono = held ? 1 : 0; r.n_rxcb = held ? 1 : 0;
        r.level_prev = 12.5; r.level_lin = 12.0; r.y = 0.5; r.n_null = 2;
        return r;
    };
    // Epoch 1: the hold starts.
    slots[0].ok = true;
    slots[0].level_hold = {record(true, true, LevelHoldSkip::None)};
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    const AnchorLevelHoldCounters& C = ctx.counters;
    EXPECT_EQ(C.alh_epochs, 1);
    EXPECT_EQ(C.alh_rows, 1);
    EXPECT_EQ(C.alh_station_epochs, 1);
    EXPECT_EQ(C.alh_starts, 1);
    EXPECT_EQ(C.alh_ends, 0);
    EXPECT_EQ(C.alh_cols, 5);
    EXPECT_EQ(C.alh_rows_by_sys.at('G'), 1);
    EXPECT_DOUBLE_EQ(C.alh_y_max, 0.5);
    EXPECT_EQ(C.alh_y_max_station, "AAAA");
    {
        const net::LevelHoldMemo* m = nullptr;
        const auto it = ctx.entities.stations.find(StationId{0});
        ASSERT_NE(it, ctx.entities.stations.end());
        m = &it->second.level_hold.at(System::GPS);
        EXPECT_TRUE(m->held);
        EXPECT_TRUE(m->tenure);
        EXPECT_EQ(m->held_epochs, 1);
        EXPECT_DOUBLE_EQ(m->level, 12.5);
    }
    // Epoch 2: held again, no edge.
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    EXPECT_EQ(C.alh_starts, 1);
    EXPECT_EQ(C.alh_ends, 0);
    EXPECT_EQ(C.alh_rows, 2);
    // Epoch 3: tenure, but no carried column: the hold ends with that reason.
    slots[0].level_hold = {record(false, true, LevelHoldSkip::NoCarriedColumn)};
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    EXPECT_EQ(C.alh_ends, 1);
    EXPECT_EQ(C.alh_skip_no_carried, 1);
    // Epoch 4: held again (direction found); epoch 5: the station anchors nothing (no record):
    // the hold ends by tenure.
    slots[0].level_hold = {record(true, true, LevelHoldSkip::None)};
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    EXPECT_EQ(C.alh_starts, 2);
    slots[0].level_hold.clear();
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    EXPECT_EQ(C.alh_ends, 2);
    EXPECT_EQ(C.alh_handover_epochs, 0);
    traces.events_trace.close();
    std::ifstream in(traces.events_trace_path);
    std::string line, text;
    int starts = 0, ends = 0;
    while (std::getline(in, line)) {
        text += line + "\n";
        if (line.find(",ANCHOR_LEVEL,hold_start,,AAAA,") != std::string::npos) ++starts;
        if (line.find(",ANCHOR_LEVEL,hold_end,,AAAA,") != std::string::npos) ++ends;
    }
    EXPECT_EQ(starts, 2) << text;
    EXPECT_EQ(ends, 2) << text;
    EXPECT_NE(text.find("reason=tenure;"), std::string::npos) << text;
    EXPECT_NE(text.find("reason=no_carried_column;"), std::string::npos) << text;
    EXPECT_NE(text.find("reason=direction_found;"), std::string::npos) << text;
    EXPECT_NE(text.find("reason=tenure_end;"), std::string::npos) << text;
}

TEST(NetworkAnchorLevelHoldTest, AHandoverIsAnEndAndAStartOfTheSameConstellationInOneEpoch) {
    HoldFixture f(kHoldOn);
    NetworkRunContext& ctx = f.run->ctx();
    NetworkRunTraces& traces = f.run->traces();
    traces.events_trace.open(traces.events_trace_path);
    f.run->setup().stations.resize(2);
    f.run->setup().stations[1].name = "BBBB";
    std::vector<StationAssembly> slots(2);
    slots[0].ok = slots[1].ok = true;
    LevelHoldRecord held;
    held.sys = System::GPS; held.tenure = true; held.n_fam = 1; held.held = true; held.n_cols = 4;
    slots[0].level_hold = {held};
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    slots[0].level_hold.clear();   // AAAA handed its families over
    slots[1].level_hold = {held};  // BBBB took them
    networkAnchorLevelHoldTally(ctx, traces, f.run->se(), f.run->setup().stations, slots);
    const AnchorLevelHoldCounters& C = ctx.counters;
    EXPECT_EQ(C.alh_starts, 2);
    EXPECT_EQ(C.alh_ends, 1);
    EXPECT_EQ(C.alh_handover_epochs, 1);
    traces.events_trace.close();
    std::ifstream in(traces.events_trace_path);
    std::string line, text;
    while (std::getline(in, line)) text += line + "\n";
    EXPECT_NE(text.find(",ANCHOR_LEVEL,hold_handover,,,1.000000,,handover,sys=G;from=AAAA;to=BBBB"),
              std::string::npos) << text;
}

// ---- The loader -----------------------------------------------------------------------------

TEST(NetworkAnchorLevelHoldTest, TheLoaderRefusesTheHoldOffTheAnchorElectionAndTheScopeAlone) {
    GnssConfig ok;
    EXPECT_TRUE(loadInto("on", networkConfig(kHoldOn), ok));
    EXPECT_TRUE(ok.networkPhaseDatumAnchorLevelHold());
    EXPECT_FALSE(ok.networkPhaseDatumAnchorLevelHoldAllStations());
    GnssConfig off;
    EXPECT_TRUE(loadInto("off", networkConfig(R"(, "phase_datum_prioritized": true)"), off));
    EXPECT_FALSE(off.networkPhaseDatumAnchorLevelHold());
    GnssConfig legacy;
    EXPECT_FALSE(loadInto("legacy", networkConfig(R"(, "phase_datum_anchor_level_hold": true)"), legacy))
        << "the legacy pivot datum elects no anchor";
    GnssConfig eq34;
    EXPECT_FALSE(loadInto("eq34", networkConfig(kHoldOn + R"(, "phase_datum_eq34": true)"), eq34))
        << "the eq34 chain elects no anchor";
    GnssConfig alone;
    EXPECT_FALSE(loadInto("alone", networkConfig(R"(, "phase_datum_prioritized": true, "phase_datum_anchor_level_hold_all_stations": true)"), alone))
        << "the scope widens the hold and builds nothing on its own";
}

TEST(NetworkAnchorLevelHoldTest, TheKnobsResolveOnlyOnTheAnchorElection) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("knobs", networkConfig(kHoldAll), cfg));
    NetworkTestRun fx(cfg);
    EXPECT_TRUE(fx.ctx().knobs.pd_anchor_level_hold);
    EXPECT_TRUE(fx.ctx().knobs.pd_anchor_level_hold_all);
    GnssConfig plain;
    ASSERT_TRUE(loadInto("plain", networkConfig(R"(, "phase_datum_prioritized": true)"), plain));
    NetworkTestRun fy(plain);
    EXPECT_FALSE(fy.ctx().knobs.pd_anchor_level_hold);
    EXPECT_FALSE(fy.ctx().knobs.pd_anchor_level_hold_all);
}
