/**
 * @file test_network_iftd_sat_centre.cpp
 * @brief The IF-TD two-way centring (`network.cycle_slip.if_phase_td_sat_centre`): the pure
 *        fit `ifTdSatCentreFit`, the centred value `ifTdSatCentred`, the reference re-test
 *        `ifTdSatCentreRefStep`, and the driver on eight stations with real detectors.
 *
 * The between-epoch IF phase double difference of satellite s at station i is
 * dd_i(s) = de_s - de_ref(i) + slip + noise, where de is the increment of the a priori clock's
 * error.  With the broadcast clock as the a priori, de_s reaches centimetres, and one network
 * median per pair cannot remove it.  The centring fits dd = overall + row_i + col_s by median
 * polish and tests the residual.
 *
 * Covered:
 *   - a satellite's common step becomes its column and leaves no residual;
 *   - a slip at one station stays in that sample's residual;
 *   - a station-wide shift (its reference moved) becomes the station's row;
 *   - a satellite with fewer non-reference samples than the minimum has no column;
 *   - reference samples are not fitted and centre to 0;
 *   - the reference re-test removes the clock part of the gate's step, keeps a slip, and is
 *     untested without the reference's column;
 *   - the fit does not depend on the order of the samples; an all-thin pool leaves raw values;
 *   - the driver: a common step of one satellite flags nowhere (it flags at every station with
 *     the knob off), a station slip on top of it is still a slip, a step of the reference
 *     satellite of two stations is kept there by the re-test, and the loader keeps the knob
 *     off by default and replaces a bad minimum.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/slip/SlipQc.hpp"
#include "network/state/StationParams.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/IfPhaseSlipDetector.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

/// A deterministic +-3 mm scatter for cell (i, j).
double wobble(int i, int j) {
    static constexpr double w[7] = { 0.0011, -0.0024, 0.0030, -0.0006, 0.0017, -0.0029, 0.0004 };
    return w[(i * 3 + j * 5) % 7];
}

/// A pool of @p n_sta stations x the satellites G02..G07 (G01 is every station's reference),
/// each sample the scatter plus @p add of its (station, satellite).
std::vector<IfTdCentreSample> pool(int n_sta,
                                   const std::map<std::pair<int, std::string>, double>& add = {}) {
    std::vector<IfTdCentreSample> out;
    for (int i = 0; i < n_sta; ++i) {
        out.push_back({ i, "G01", 0.0, true });
        for (int j = 2; j <= 7; ++j) {
            const std::string sat = "G0" + std::to_string(j);
            double v = wobble(i, j);
            const auto it = add.find({ i, sat });
            if (it != add.end()) v += it->second;
            out.push_back({ i, sat, v, false });
        }
    }
    return out;
}

std::map<std::pair<int, std::string>, double> satStep(int n_sta, const std::string& sat, double m) {
    std::map<std::pair<int, std::string>, double> a;
    for (int i = 0; i < n_sta; ++i) a[{ i, sat }] = m;
    return a;
}

double residualOf(const IfTdSatCentre& fit, const std::vector<IfTdCentreSample>& p, int sta,
                  const std::string& sat) {
    for (const auto& s : p)
        if (s.sta == sta && s.sat == sat) return ifTdSatCentred(fit, s);
    ADD_FAILURE() << "no sample " << sta << " " << sat;
    return 0.0;
}

}  // namespace

TEST(IfTdSatCentre, ACommonSatelliteStepBecomesItsColumnAndLeavesNoResidual) {
    const auto p = pool(10, satStep(10, "G04", 0.12));
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    ASSERT_EQ(fit.col.count("G04"), 1u);
    EXPECT_NEAR(fit.col.at("G04") - fit.col.at("G03"), 0.12, 0.006);
    for (int i = 0; i < 10; ++i) EXPECT_LT(std::abs(residualOf(fit, p, i, "G04")), 0.006) << i;
}

TEST(IfTdSatCentre, ASlipAtOneStationStaysInThatSamplesResidual) {
    auto add = satStep(10, "G04", 0.12);
    add[{ 3, "G05" }] = 0.20;
    const auto p = pool(10, add);
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    EXPECT_NEAR(residualOf(fit, p, 3, "G05"), 0.20, 0.008);
    for (int i = 0; i < 10; ++i)
        if (i != 3) EXPECT_LT(std::abs(residualOf(fit, p, i, "G05")), 0.008) << i;
}

TEST(IfTdSatCentre, AStationWideShiftBecomesTheStationsRow) {
    std::map<std::pair<int, std::string>, double> add;
    for (int j = 2; j <= 7; ++j) add[{ 5, "G0" + std::to_string(j) }] = -0.08;
    const auto p = pool(10, add);
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    const int r5 = ifTdSatCentreRow(IfTdCentreSample{ 5, "G02", 0.0, false });
    const int r4 = ifTdSatCentreRow(IfTdCentreSample{ 4, "G02", 0.0, false });
    ASSERT_EQ(fit.row.count(r5), 1u);
    EXPECT_NEAR(fit.row.at(r5) - fit.row.at(r4), -0.08, 0.006);
    for (int j = 2; j <= 7; ++j)
        EXPECT_LT(std::abs(residualOf(fit, p, 5, "G0" + std::to_string(j))), 0.006) << j;
}

TEST(IfTdSatCentre, ASatelliteBelowTheMinimumHasNoColumn) {
    auto p = pool(10);
    for (int i = 0; i < 3; ++i) p.push_back({ i, "G09", 0.05 + wobble(i, 9), false });
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    EXPECT_EQ(fit.col.count("G09"), 0u);
    EXPECT_EQ(fit.n_obs.at("G09"), 3);
    // Station centring only: dd - overall - row.
    const IfTdCentreSample s{ 1, "G09", 0.05 + wobble(1, 9), false };
    EXPECT_NEAR(ifTdSatCentred(fit, s), s.dd - fit.overall - fit.row.at(ifTdSatCentreRow(s)), 1e-12);
    EXPECT_GT(ifTdSatCentred(fit, s), 0.04) << "the thin satellite's own step is not removed";
}

TEST(IfTdSatCentre, ReferenceSamplesAreNotFittedAndCentreToZero) {
    const auto p = pool(8);
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    EXPECT_EQ(fit.col.count("G01"), 0u);
    EXPECT_EQ(fit.n_obs.count("G01"), 0u);
    EXPECT_EQ(ifTdSatCentred(fit, IfTdCentreSample{ 0, "G01", 0.0, true }), 0.0);
}

TEST(IfTdSatCentre, TheReferenceReTestRemovesTheClockPartAndKeepsASlip) {
    // G03 is a reference elsewhere; here it is a non-reference satellite at ten stations,
    // and its clock moved by +0.10 m.  A station that took G03 as its reference saw every
    // other double difference move by -0.10 m: the gate's step.
    const auto p = pool(10, satStep(10, "G03", 0.10));
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    const std::vector<std::string> others = { "G02", "G04", "G05", "G06", "G07" };
    const auto clock_only = ifTdSatCentreRefStep(fit, -0.10, "G03", others);
    ASSERT_TRUE(clock_only.has_value());
    EXPECT_LT(std::abs(*clock_only), 0.006) << "the step is the reference's clock";
    const auto with_slip = ifTdSatCentreRefStep(fit, -0.10 - 0.107, "G03", others);
    ASSERT_TRUE(with_slip.has_value());
    EXPECT_NEAR(*with_slip, -0.107, 0.006) << "a (1,1) slip of the reference remains";
}

TEST(IfTdSatCentre, TheReferenceReTestIsUntestedWithoutTheReferencesColumn) {
    const auto p = pool(10);
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    EXPECT_FALSE(ifTdSatCentreRefStep(fit, -0.10, "G01", { "G02", "G03" }).has_value());
    EXPECT_FALSE(ifTdSatCentreRefStep(fit, -0.10, "G03", { "G09" }).has_value())
        << "none of the station's satellites has a column";
}

TEST(IfTdSatCentre, TwoConstellationsUnderOnePairLabelKeepTheirOwnRows) {
    // Station 0's GPS chain moved by +0.06 (its reference), its Galileo chain did not: one
    // station row for both would split the difference and leave 3 cm in every sample.
    std::vector<IfTdCentreSample> p;
    for (int i = 0; i < 8; ++i)
        for (int j = 2; j <= 6; ++j) {
            const double shift = (i == 0) ? 0.06 : 0.0;
            p.push_back({ i, "G0" + std::to_string(j), shift + wobble(i, j), false });
            p.push_back({ i, "E1" + std::to_string(j), wobble(i, j + 1), false });
        }
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    for (const auto& s : p)
        if (s.sta == 0) EXPECT_LT(std::abs(ifTdSatCentred(fit, s)), 0.008) << s.sat;
}

TEST(IfTdSatCentre, TheFitDoesNotDependOnTheOrderOfTheSamples) {
    auto add = satStep(9, "G06", -0.07);
    add[{ 2, "G03" }] = 0.15;
    auto p = pool(9, add);
    const IfTdSatCentre a = ifTdSatCentreFit(p, 4);
    std::reverse(p.begin(), p.end());
    std::rotate(p.begin(), p.begin() + 11, p.end());
    const IfTdSatCentre b = ifTdSatCentreFit(p, 4);
    EXPECT_DOUBLE_EQ(a.overall, b.overall);
    EXPECT_EQ(a.row, b.row);
    EXPECT_EQ(a.col, b.col);
}

TEST(IfTdSatCentre, AnAllThinPoolLeavesTheRawValues) {
    const auto p = pool(3, satStep(3, "G04", 0.12));
    const IfTdSatCentre fit = ifTdSatCentreFit(p, 4);
    EXPECT_TRUE(fit.col.empty());
    EXPECT_TRUE(fit.row.empty());
    EXPECT_EQ(fit.overall, 0.0);
    for (const auto& s : p) EXPECT_EQ(ifTdSatCentred(fit, s), s.is_ref ? 0.0 : s.dd);
}

// --- the driver ------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

constexpr int kStations = 8;
const std::string kPair = "L1C_L2W";

std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>(
        "test_iftd_sat_centre", std::make_shared<spdlog::sinks::null_sink_mt>());
}

constexpr double kWobbleM[8] = { 0.0004, -0.0003, 0.0002,  0.0005,
                                -0.0006,  0.0001, -0.0002, 0.0003 };

using BandOffset = std::map<std::string, std::map<std::string, double>>;

/// One epoch of L1C / L2W observations at station @p station.  Stations 0 and 1 see G01
/// highest (their chain's reference), the others G02, so G01 is a non-reference satellite at
/// six stations and has a column.
std::vector<PppObservation> makeEpoch(std::size_t epoch_idx, std::size_t station,
                                      const BandOffset& common, const BandOffset& local) {
    const bool g01_high = station < 2;
    const std::vector<std::pair<std::string, std::pair<double, double>>> sats = {
        { "G01", { 0.00, g01_high ? 80.0 : 60.0 } }, { "G02", { 0.12, g01_high ? 70.0 : 82.0 } },
        { "G03", { -0.07, 40.0 } }, { "G04", { 0.21, 35.0 } }, { "G05", { -0.15, 30.0 } },
        { "G06", { 0.05, 25.0 } } };
    std::vector<PppObservation> out;
    std::size_t k = 0;
    for (const auto& [sat, ve] : sats) {
        const double v = ve.first + kWobbleM[(epoch_idx * 3u + k++ + station * 5u) % 8u];
        PppObservation obs{};
        obs.satellite = sat;
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2313, 261000.0 + 30.0 * static_cast<double>(epoch_idx) };
        obs.elevation = ve.second;
        obs.azimuth   = 0.0;
        obs.los       = Eigen::Vector3d(0.0, 0.0, 1.0);
        obs.sat_pos   = Eigen::Vector3d::Zero();
        obs.rx_pos_linearization = Eigen::Vector3d::Zero();
        for (const std::string& sig : { std::string("L1C"), std::string("L2W") }) {
            double add = 0.0;
            for (const BandOffset* b : { &common, &local }) {
                const auto eit = b->find(sat);
                if (eit == b->end()) continue;
                const auto sit = eit->second.find(sig);
                if (sit != eit->second.end()) add += sit->second;
            }
            obs.modeled[sig]  = 0.0;
            obs.observed[sig] = v + add;
        }
        out.push_back(std::move(obs));
    }
    return out;
}

/// The satellite's clock moved by @p m metres: the same offset on both bands.
BandOffset clockStep(const std::string& sat, double m) {
    return { { sat, { { "L1C", m }, { "L2W", m } } } };
}

/// An L1-only slip of @p cycles.
BandOffset l1Slip(const std::string& sat, double cycles) {
    return { { sat, { { "L1C", cycles * Constants::GPS_L1_WAVELENGTH } } } };
}

std::string iftdConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"cycle_slip\": { \"if_phase_td\": true" + extra + " },\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg,
                LogManager& log) {
    const fs::path p = gnss_test::tempDir() / ("iftd_sat_centre_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// Eight stations with deferred IF-TD detectors and the driver's buffers, cold carry (a flag
/// is diagnostic).  The centring sink is passed only when @p on, as the PASS A orchestrator
/// passes it only when the knob is armed.
struct CentreDriverHarness {
    LogManager                 log_manager;
    GnssConfig                 config;
    std::vector<NetStation>    stations;
    std::vector<bool>          present = std::vector<bool>(kStations, true);
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    FlagRate                   fr;
    SyncedEpoch                se;
    NetworkRunTraces           traces{ gnss_test::tempDir() / "gnss_iftd_sat_centre_unused" };
    std::shared_ptr<spdlog::logger> log{ quietLogger() };
    IfTdPairTallies            tally;
    bool                       on;
    SlipQcCounters             counters;
    std::size_t                resets{ 0 };   ///< Reference reset keys the detectors returned.

    explicit CentreDriverHarness(bool on_) : on(on_) {
        const bool ok = loadConfig(on ? "on" : "off",
                                   iftdConfig(on ? ", \"if_phase_td_sat_centre\": true"
                                                 : std::string{}),
                                   config, log_manager);
        EXPECT_TRUE(ok);
        for (int i = 0; i < kStations; ++i) {
            stations.emplace_back();
            NetStation& st = stations.back();
            st.name = "STA" + std::to_string(i);
            st.params = startStationParams(config, Eigen::Vector3d::Zero());
            st.if_slip = std::make_unique<IfPhaseSlipDetector>(config, log_manager);
            st.if_slip->setDeferFlagging(true);
        }
        se.time = GnssTime{ 2313, 261000.0 };
    }

    /// One epoch: `common` reaches every station, `local` only station @p local_sta.
    void epoch(std::size_t e, const BandOffset& common = {}, const BandOffset& local = {},
               int local_sta = 2) {
        se.time.tow = 261000.0 + 30.0 * static_cast<double>(e);
        PassABuffers pa(stations.size());
        for (int i = 0; i < kStations; ++i) {
            pa.sta_if_resets[i] = stations[i].if_slip->detect(
                makeEpoch(e, static_cast<std::size_t>(i), common,
                          i == local_sta ? local : BandOffset{}));
            resets += pa.sta_if_resets[i].size();
        }
        IfTdSatCentreSink sink{ config.getNetworkConfig().slip_qc.if_phase_td_sat_centre_min_stations,
                                counters };
        networkSlipIfTdDriver(pa, config, stations, present, /*i_lo=*/0, kStations,
                              /*carry_invariant=*/false, /*carry_warm=*/false,
                              carry_slipped_ambs, /*trace_flag_rates=*/false, fr, se,
                              traces, log, /*refsat_mark_sats=*/nullptr,
                              /*relevel=*/nullptr, /*bridge_local_carry=*/nullptr,
                              /*bridge_cnt=*/nullptr, /*relevel_pend=*/nullptr,
                              /*raw_epoch=*/nullptr, /*raw_hist=*/nullptr,
                              /*oracle=*/nullptr, /*common_band=*/nullptr, &tally,
                              /*sat_clock=*/nullptr, on ? &sink : nullptr);
    }
    const IfTdPairTally& T() { return tally[kPair]; }
};

}  // namespace

/// A step of one satellite's clock seen at every station: one network median leaves it in every
/// sample and flags it at all eight stations; the centring makes it the satellite's column and
/// flags nothing.
TEST(IfTdSatCentreDriver, ACommonStepOfOneSatelliteFlagsNowhere) {
    CentreDriverHarness off(/*on=*/false), on(/*on=*/true);
    for (CentreDriverHarness* h : { &off, &on }) {
        h->epoch(0);
        h->epoch(1);
        h->epoch(2, clockStep("G04", 0.50));
    }
    EXPECT_EQ(off.T().flagged, kStations) << "the one-way centre flags the step everywhere";
    EXPECT_EQ(on.T().flagged, 0);
    EXPECT_EQ(on.counters.if_td_centre_flags_removed, kStations);
    EXPECT_EQ(on.counters.if_td_centre_flags_added, 0);
    EXPECT_EQ(on.counters.if_td_centre_pools, 2) << "one pool per epoch with a previous one";
    EXPECT_GE(on.counters.if_td_centre_col_hist[5], 1) << "the 0.5 m column";
    EXPECT_NEAR(on.counters.if_td_centre_col_max_m, 0.50, 0.08);
    EXPECT_EQ(off.counters.if_td_centre_pools, 0) << "nothing runs with the knob off";
}

/// A station that slipped as well as the clock moving is still a slip.
TEST(IfTdSatCentreDriver, AStationSlipOnTopOfTheStepIsStillASlip) {
    CentreDriverHarness h(/*on=*/true);
    h.epoch(0);
    h.epoch(1);
    h.epoch(2, clockStep("G04", 0.50), l1Slip("G04", 2.0), /*local_sta=*/5);
    EXPECT_EQ(h.T().flagged, 1);
}

/// A step of G01, the reference at stations 0 and 1 and a non-reference satellite at the other
/// six: the gate fires at stations 0 and 1, and the re-test finds the step in G01's column and
/// keeps both arcs.
TEST(IfTdSatCentreDriver, AStepOfTwoStationsReferenceIsKeptByTheReTest) {
    CentreDriverHarness h(/*on=*/true);
    h.epoch(0);
    h.epoch(1);
    const std::size_t before = h.resets;
    h.epoch(2, clockStep("G01", 0.50));
    EXPECT_EQ(h.resets - before, 2u) << "the gate fired at the two stations whose reference moved";
    EXPECT_EQ(h.counters.if_td_centre_refsat_tested, 2);
    EXPECT_EQ(h.counters.if_td_centre_refsat_kept, 2);
    EXPECT_EQ(h.counters.if_td_centre_refsat_refloated, 0);
    EXPECT_EQ(h.T().flagged, 0) << "and G01 does not flag at the six stations where it is not the reference";
}

/// The same reference step plus an L1 slip of the reference at station 0: station 0 refloats,
/// station 1 keeps.
TEST(IfTdSatCentreDriver, AReferenceThatAlsoSlippedIsRefloated) {
    CentreDriverHarness h(/*on=*/true);
    h.epoch(0);
    h.epoch(1);
    h.epoch(2, clockStep("G01", 0.50), l1Slip("G01", 2.0), /*local_sta=*/0);
    EXPECT_EQ(h.counters.if_td_centre_refsat_tested, 2);
    EXPECT_EQ(h.counters.if_td_centre_refsat_kept, 1);
    EXPECT_EQ(h.counters.if_td_centre_refsat_refloated, 1);
}

// --- the loader ------------------------------------------------------------------------

TEST(IfTdSatCentreDriver, ConfigDefaultsOff) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", iftdConfig(""), cfg, log));
    const SlipQcConfig& sq = cfg.getNetworkConfig().slip_qc;
    EXPECT_FALSE(sq.if_phase_td_sat_centre);
    EXPECT_EQ(sq.if_phase_td_sat_centre_min_stations, 4);
}

TEST(IfTdSatCentreDriver, ConfigParsesTheKnobAndReplacesABadMinimum) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("armed", iftdConfig(
        ", \"if_phase_td_sat_centre\": true, \"if_phase_td_sat_centre_min_stations\": 6"), cfg, log));
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.if_phase_td_sat_centre);
    EXPECT_EQ(cfg.getNetworkConfig().slip_qc.if_phase_td_sat_centre_min_stations, 6);
    GnssConfig bad;
    ASSERT_TRUE(loadConfig("bad", iftdConfig(
        ", \"if_phase_td_sat_centre\": true, \"if_phase_td_sat_centre_min_stations\": 2"), bad, log));
    EXPECT_EQ(bad.getNetworkConfig().slip_qc.if_phase_td_sat_centre_min_stations, 4);
}
