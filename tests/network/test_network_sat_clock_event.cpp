/**
 * @file test_network_sat_clock_event.cpp
 * @brief The IF-TD satellite clock event rule (`network.cycle_slip.if_phase_td_sat_clock_event`):
 *        the pure classifier `ifTdSatClockClassify` and the keep test `ifTdSatClockKeeps`.
 *
 * A satellite clock step enters the between-epoch IF phase difference of the satellite at
 * every station that does not take it as the chain's reference, with one magnitude; a cycle
 * slip is station-local.  The rule reads the satellite's flagged residuals r = dd - med:
 * with F the flagged set, m its median and d = 1.4826 MAD about m, the satellite is a clock
 * event iff |F| >= min_stations, |F| >= ceil(min_fraction * n_obs) and d <= coherence * |m|.
 *
 * Covered:
 *   - 16 stations all flagged near +0.50 m with small scatter: an event, step about 0.5,
 *     every flagged sample kept;
 *   - 3 of 16 flagged (a few slips): no event;
 *   - 10 of 16 flagged at fraction 0.75: no event; the same count passes at fraction 0.5;
 *   - magnitudes spread from 0.1 to 0.9 m: no event (slips of mixed size);
 *   - a coherent 0.5 m set plus one station at 0.88 m: an event, and that station is not kept
 *     (it is a slip as well);
 *   - mixed signs: no event;
 *   - the keep test at the edge of the threshold, just inside and just outside;
 *   - the result does not depend on the order of the samples, and an unflagged set is empty.
 *
 * And the driver (`IfTdSatClockDriver`), on six stations with real detectors: a step of one
 * satellite seen at every station is kept at all six (no detector reset, so the next epoch
 * evaluates it), its step reaches the sink, and a station that slipped as well is still a slip;
 * with no sink the flags reset the chains as before; the loader keeps the knob off by default,
 * parses the three parameters and replaces a bad value by its default.
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

constexpr double kThr = 0.10;   // a typical per-sample threshold (m)

/// One sample of the satellite at station @p sta with residual @p r against threshold @p thr;
/// flagged exactly as the pooled test flags it.
IfTdSatClockSample sample(int sta, double r, double thr = kThr) {
    IfTdSatClockSample s;
    s.sta     = sta;
    s.r       = r;
    s.thr     = thr;
    s.flagged = std::abs(r) > thr;
    return s;
}

/// @p n stations with residuals @p centre + a small deterministic scatter (+-1 cm).
std::vector<IfTdSatClockSample> coherentSet(int n, double centre) {
    std::vector<IfTdSatClockSample> v;
    for (int k = 0; k < n; ++k)
        v.push_back(sample(k, centre + 0.01 * (((k * 7) % 5) - 2) / 2.0));
    return v;
}

constexpr int    kMinStations = 4;
constexpr double kMinFraction = 0.75;
constexpr double kCoherence   = 0.25;

IfTdSatClockVerdict classify(const std::vector<IfTdSatClockSample>& v) {
    return ifTdSatClockClassify(v, kMinStations, kMinFraction, kCoherence);
}

}  // namespace

TEST(IfTdSatClockEvent, SixteenStationsAtOneStepAreAnEventAndEveryFlagIsKept) {
    const auto v = coherentSet(16, 0.50);
    const IfTdSatClockVerdict out = classify(v);
    EXPECT_TRUE(out.event);
    EXPECT_EQ(out.n_obs, 16);
    EXPECT_EQ(out.n_flag, 16);
    EXPECT_NEAR(out.step_m, 0.50, 0.01);
    EXPECT_LT(out.disp_m, kCoherence * std::abs(out.step_m));
    for (const auto& s : v)
        EXPECT_TRUE(ifTdSatClockKeeps(out, s.r, s.thr)) << "station " << s.sta;
}

TEST(IfTdSatClockEvent, ThreeOfSixteenFlaggedIsNotAnEvent) {
    // Three slips of the same size at three stations: not enough stations (and far below the
    // fraction), so each is a slip at its own station.
    auto v = coherentSet(16, 0.0);
    for (auto& s : v) { s.r = 0.002 * (s.sta % 3); s.flagged = false; }
    for (int k : {2, 7, 11}) { v[static_cast<std::size_t>(k)].r = 0.50; v[static_cast<std::size_t>(k)].flagged = true; }
    const IfTdSatClockVerdict out = classify(v);
    EXPECT_FALSE(out.event);
    EXPECT_EQ(out.n_flag, 3);
    EXPECT_FALSE(ifTdSatClockKeeps(out, 0.50, kThr)) << "no event: nothing is kept";
}

TEST(IfTdSatClockEvent, TenOfSixteenFlaggedFailsTheFractionAtThreeQuarters) {
    auto v = coherentSet(16, 0.0);
    for (auto& s : v) { s.r = 0.003; s.flagged = false; }
    for (int k = 0; k < 10; ++k) { v[static_cast<std::size_t>(k)].r = 0.50 + 0.004 * k; v[static_cast<std::size_t>(k)].flagged = true; }
    // ceil(0.75 * 16) = 12 > 10.
    const IfTdSatClockVerdict strict = ifTdSatClockClassify(v, kMinStations, 0.75, kCoherence);
    EXPECT_FALSE(strict.event);
    EXPECT_EQ(strict.n_flag, 10);
    // The same set at a fraction of 0.5 (8 needed) is an event: the flags are coherent.
    const IfTdSatClockVerdict loose = ifTdSatClockClassify(v, kMinStations, 0.5, kCoherence);
    EXPECT_TRUE(loose.event);
    EXPECT_NEAR(loose.step_m, 0.518, 0.01);
}

TEST(IfTdSatClockEvent, TheFractionCeilingIsNotPushedUpByRounding) {
    // 0.7 * 10 is 7.000000000000001 in floating point: seven flags must suffice.
    std::vector<IfTdSatClockSample> v;
    for (int k = 0; k < 10; ++k) v.push_back(sample(k, k < 7 ? 0.50 : 0.001));
    const IfTdSatClockVerdict out = ifTdSatClockClassify(v, 4, 0.7, kCoherence);
    EXPECT_EQ(out.n_flag, 7);
    EXPECT_TRUE(out.event);
}

TEST(IfTdSatClockEvent, MinimumStationCountIsEnforcedBesideTheFraction) {
    // Three stations, all flagged at one step: the fraction is met (3 of 3) but four stations are
    // required.
    std::vector<IfTdSatClockSample> v;
    for (int k = 0; k < 3; ++k) v.push_back(sample(k, 0.50));
    EXPECT_FALSE(classify(v).event);
    EXPECT_TRUE(ifTdSatClockClassify(v, 3, kMinFraction, kCoherence).event);
}

TEST(IfTdSatClockEvent, MagnitudesSpreadFromTenToNinetyCentimetresAreNotAnEvent) {
    std::vector<IfTdSatClockSample> v;
    for (int k = 0; k < 16; ++k) v.push_back(sample(k, 0.11 + 0.8 * k / 15.0));
    const IfTdSatClockVerdict out = classify(v);
    EXPECT_EQ(out.n_flag, 16);
    EXPECT_FALSE(out.event) << "slips of mixed size are not one common step (d "
                            << out.disp_m << " vs |m| " << std::abs(out.step_m) << ")";
    EXPECT_GT(out.disp_m, kCoherence * std::abs(out.step_m));
}

TEST(IfTdSatClockEvent, ACoherentSetWithOneStationFurtherOutIsAnEventAndThatStationIsASlipToo) {
    auto v = coherentSet(15, 0.50);
    v.push_back(sample(15, 0.88));
    const IfTdSatClockVerdict out = classify(v);
    EXPECT_TRUE(out.event);
    EXPECT_NEAR(out.step_m, 0.50, 0.01);
    EXPECT_FALSE(ifTdSatClockKeeps(out, 0.88, kThr)) << "|0.88 - 0.50| = 0.38 m exceeds its threshold";
    // The other fifteen are kept.
    int kept = 0;
    for (const auto& s : v)
        if (s.flagged && ifTdSatClockKeeps(out, s.r, s.thr)) ++kept;
    EXPECT_EQ(kept, 15);
}

TEST(IfTdSatClockEvent, MixedSignsAreNotAnEvent) {
    std::vector<IfTdSatClockSample> v;
    for (int k = 0; k < 16; ++k) v.push_back(sample(k, k % 2 == 0 ? 0.50 : -0.50));
    const IfTdSatClockVerdict out = classify(v);
    EXPECT_EQ(out.n_flag, 16);
    EXPECT_FALSE(out.event) << "a step of one sign at half the stations and the other sign at the rest "
                               "is not one common step";
}

TEST(IfTdSatClockEvent, KeepIsDecidedByTheDistanceToTheStepAgainstTheSamplesOwnThreshold) {
    const auto v = coherentSet(16, 0.50);
    const IfTdSatClockVerdict out = classify(v);
    ASSERT_TRUE(out.event);
    const double thr = 0.08;
    EXPECT_TRUE(ifTdSatClockKeeps(out, out.step_m + 0.0799, thr));
    EXPECT_TRUE(ifTdSatClockKeeps(out, out.step_m - 0.0799, thr));
    EXPECT_FALSE(ifTdSatClockKeeps(out, out.step_m + 0.0801, thr));
    EXPECT_FALSE(ifTdSatClockKeeps(out, out.step_m - 0.0801, thr));
    // Without an event, the same distance keeps nothing.
    IfTdSatClockVerdict none = out;
    none.event = false;
    EXPECT_FALSE(ifTdSatClockKeeps(none, out.step_m, thr));
}

TEST(IfTdSatClockEvent, TheVerdictDoesNotDependOnTheOrderOfTheSamples) {
    auto v = coherentSet(16, 0.50);
    v.push_back(sample(16, 0.001));   // one unflagged station among them
    const IfTdSatClockVerdict a = classify(v);
    std::reverse(v.begin(), v.end());
    const IfTdSatClockVerdict b = classify(v);
    EXPECT_EQ(a.event, b.event);
    EXPECT_EQ(a.n_flag, b.n_flag);
    EXPECT_EQ(a.n_obs, b.n_obs);
    EXPECT_DOUBLE_EQ(a.step_m, b.step_m);
    EXPECT_DOUBLE_EQ(a.disp_m, b.disp_m);
}

TEST(IfTdSatClockEvent, NothingFlaggedGivesAnEmptyVerdict) {
    std::vector<IfTdSatClockSample> v;
    for (int k = 0; k < 16; ++k) v.push_back(sample(k, 0.002 * (k % 3)));
    const IfTdSatClockVerdict out = classify(v);
    EXPECT_FALSE(out.event);
    EXPECT_EQ(out.n_flag, 0);
    EXPECT_EQ(out.n_obs, 16);
    EXPECT_DOUBLE_EQ(out.step_m, 0.0);
    EXPECT_FALSE(classify({}).event);
}

// ===========================================================================
// The driver, on six stations with real deferred IF-TD detectors
// ===========================================================================
//
// Synthetic uncombined observations as in test_if_td_reseed_on_flag.cpp: the model is 0 on
// every band and the observed value carries the whole OMC.  A step of the satellite's clock
// is the same offset on both bands at every station (the IF combination sums to one, so it
// moves the IF phase by exactly that amount); a slip is an offset on one band of one station.

namespace {

namespace fs = std::filesystem;

constexpr int kStations = 6;
const std::string kPair = "L1C_L2W";

std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>(
        "test_sat_clock_event", std::make_shared<spdlog::sinks::null_sink_mt>());
}

constexpr double kWobbleM[8] = { 0.0004, -0.0003, 0.0002,  0.0005,
                                -0.0006,  0.0001, -0.0002, 0.0003 };

using BandOffset = std::map<std::string, std::map<std::string, double>>;

/// One epoch of observations on L1C and L2W at station @p station; G01 is the highest
/// satellite and stays every chain's reference.
std::vector<PppObservation> makeEpoch(std::size_t epoch_idx, std::size_t station,
                                      const BandOffset& common, const BandOffset& local) {
    const std::vector<std::pair<std::string, std::pair<double, double>>> sats = {
        { "G01", { 0.00, 80.0 } }, { "G02", { 0.12, 70.0 } }, { "G03", { -0.07, 40.0 } },
        { "G04", { 0.21, 35.0 } }, { "G05", { -0.15, 30.0 } }, { "G06", { 0.05, 25.0 } } };
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
    const fs::path p = gnss_test::tempDir() / ("sat_clock_event_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// Six stations with deferred IF-TD detectors and the driver's buffers, cold carry (a flag is
/// diagnostic, so no parameter-set write is involved).  The event sink is passed only when
/// @p sink_on, as the PASS A orchestrator passes it only when the knob is armed.
struct ClockDriverHarness {
    LogManager                 log_manager;
    GnssConfig                 config;
    std::vector<NetStation>    stations;
    std::vector<bool>          present = std::vector<bool>(kStations, true);
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    FlagRate                   fr;
    SyncedEpoch                se;
    NetworkRunTraces           traces{ gnss_test::tempDir() / "gnss_sat_clock_event_unused" };
    std::shared_ptr<spdlog::logger> log{ quietLogger() };
    IfTdPairTallies            tally;
    bool                       sink_on;
    std::map<std::string, double> step_m;
    SlipQcCounters             counters;

    explicit ClockDriverHarness(bool sink_on_) : sink_on(sink_on_) {
        const bool ok = loadConfig(sink_on ? "on" : "off",
                                   iftdConfig(sink_on ? ", \"if_phase_td_sat_clock_event\": true"
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

    /// One epoch: every station's detector runs (`common` reaches every station, `local` only
    /// station 0), then the pooled driver decides.  The step map is the epoch's, so it starts empty.
    void epoch(std::size_t e, const BandOffset& common = {}, const BandOffset& local = {}) {
        se.time.tow = 261000.0 + 30.0 * static_cast<double>(e);
        step_m.clear();
        PassABuffers pa(stations.size());
        for (int i = 0; i < kStations; ++i)
            pa.sta_if_resets[i] = stations[i].if_slip->detect(
                makeEpoch(e, static_cast<std::size_t>(i), common, i == 0 ? local : BandOffset{}));
        IfTdSatClockSink sink{ step_m, counters };
        networkSlipIfTdDriver(pa, config, stations, present, /*i_lo=*/0, kStations,
                              /*carry_invariant=*/false, /*carry_warm=*/false,
                              carry_slipped_ambs, /*trace_flag_rates=*/false, fr, se,
                              traces, log, /*refsat_mark_sats=*/nullptr,
                              /*relevel=*/nullptr, /*bridge_local_carry=*/nullptr,
                              /*bridge_cnt=*/nullptr, /*relevel_pend=*/nullptr,
                              /*raw_epoch=*/nullptr, /*raw_hist=*/nullptr,
                              /*oracle=*/nullptr, /*common_band=*/nullptr, &tally,
                              sink_on ? &sink : nullptr);
    }
    const IfTdPairTally& T() { return tally[kPair]; }
};

}  // namespace

/// A step of one satellite's clock, the same at every station: the six flags form an event, all six
/// arcs are kept (no detector reset), and the step reaches the sink.  The flag-rate tallies still
/// count the six flags.  The epoch after the step evaluates the satellite at every station, which a
/// reset would have left blind.
TEST(IfTdSatClockDriver, AStepSeenAtEveryStationIsKeptAtAllOfThem) {
    ClockDriverHarness h(/*sink_on=*/true);
    h.epoch(0);
    h.epoch(1);
    h.epoch(2, clockStep("G03", 0.50));
    EXPECT_EQ(h.counters.if_td_clock_events, 1);
    EXPECT_EQ(h.counters.if_td_clock_flagged, 6);
    EXPECT_EQ(h.counters.if_td_clock_kept, 6);
    EXPECT_EQ(h.counters.if_td_clock_refsat_kept, 0);
    ASSERT_EQ(h.step_m.count("G03"), 1u);
    EXPECT_NEAR(std::abs(h.step_m.at("G03")), 0.50, 0.01);
    EXPECT_EQ(h.step_m.size(), 1u);
    EXPECT_EQ(h.T().flagged, 6) << "the flag tally is counted before the rule";
    EXPECT_EQ(h.T().refloated, 0) << "and no arc was refloated";
    h.epoch(3, clockStep("G03", 0.50));
    EXPECT_EQ(h.T().evaluated, 3 * kStations * 6) << "no chain was restarted";
    EXPECT_EQ(h.T().flagged, 6) << "the stepped clock reads clean in the next epoch";
    EXPECT_TRUE(h.step_m.empty());
}

/// The same step with no sink (the knob off): the six flags restart their chains as before, and the
/// flag tally is the same as the armed run's.
TEST(IfTdSatClockDriver, WithNoSinkTheFlagsResetTheirChainsAsBefore) {
    ClockDriverHarness off(/*sink_on=*/false), on(/*sink_on=*/true);
    for (ClockDriverHarness* h : {&off, &on}) {
        h->epoch(0);
        h->epoch(1);
        h->epoch(2, clockStep("G03", 0.50));
        h->epoch(3, clockStep("G03", 0.50));
    }
    EXPECT_EQ(off.T().flagged, on.T().flagged);
    EXPECT_EQ(off.T().evaluated, on.T().evaluated - 6) << "the six restarted chains skip one epoch";
    EXPECT_EQ(off.counters.if_td_clock_events, 0);
    EXPECT_EQ(off.counters.if_td_clock_kept, 0);
    EXPECT_TRUE(off.step_m.empty());
}

/// A station that slipped as well as the clock moving is still a slip: it is not kept and its
/// chain restarts, while the other five arcs are kept.
TEST(IfTdSatClockDriver, AStationThatSlippedAsWellTakesTheOrdinaryPath) {
    ClockDriverHarness h(/*sink_on=*/true);
    h.epoch(0);
    h.epoch(1);
    h.epoch(2, clockStep("G03", 0.50), l1Slip("G03", 2.0));
    EXPECT_EQ(h.counters.if_td_clock_events, 1);
    EXPECT_EQ(h.counters.if_td_clock_flagged, 6);
    EXPECT_EQ(h.counters.if_td_clock_kept, 5);
    ASSERT_EQ(h.step_m.count("G03"), 1u);
    EXPECT_NEAR(std::abs(h.step_m.at("G03")), 0.50, 0.01);
    h.epoch(3, clockStep("G03", 0.50), l1Slip("G03", 2.0));
    EXPECT_EQ(h.T().evaluated, 3 * kStations * 6 - 1) << "only the slipped station's chain restarted";
}

/// A slip at one station is no event: nothing is classified, and the flag takes the ordinary path.
TEST(IfTdSatClockDriver, ASingleStationSlipIsNotAClockEvent) {
    ClockDriverHarness h(/*sink_on=*/true);
    h.epoch(0);
    h.epoch(1);
    h.epoch(2, {}, l1Slip("G03", 2.0));
    EXPECT_EQ(h.T().flagged, 1);
    EXPECT_EQ(h.counters.if_td_clock_events, 0);
    EXPECT_EQ(h.counters.if_td_clock_kept, 0);
    EXPECT_TRUE(h.step_m.empty());
}

// --- the loader ----------------------------------------------------------

TEST(IfTdSatClockDriver, ConfigDefaultsOffWithTheDocumentedParameters) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", iftdConfig(""), cfg, log));
    const auto& sq = cfg.getNetworkConfig().slip_qc;
    EXPECT_FALSE(sq.if_phase_td_sat_clock_event);
    EXPECT_EQ(sq.if_phase_td_sat_clock_min_stations, 4);
    EXPECT_DOUBLE_EQ(sq.if_phase_td_sat_clock_min_fraction, 0.75);
    EXPECT_DOUBLE_EQ(sq.if_phase_td_sat_clock_coherence, 0.25);
    EXPECT_FALSE(cfg.getNetworkConfig().emit_pb_hold_trip_generation);
}

TEST(IfTdSatClockDriver, ConfigParsesTheKnobAndItsParameters) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("armed", iftdConfig(
        ", \"if_phase_td_sat_clock_event\": true, \"if_phase_td_sat_clock_min_stations\": 6,"
        " \"if_phase_td_sat_clock_min_fraction\": 0.5, \"if_phase_td_sat_clock_coherence\": 0.4"),
        cfg, log));
    const auto& sq = cfg.getNetworkConfig().slip_qc;
    EXPECT_TRUE(sq.if_phase_td_sat_clock_event);
    EXPECT_EQ(sq.if_phase_td_sat_clock_min_stations, 6);
    EXPECT_DOUBLE_EQ(sq.if_phase_td_sat_clock_min_fraction, 0.5);
    EXPECT_DOUBLE_EQ(sq.if_phase_td_sat_clock_coherence, 0.4);
}

TEST(IfTdSatClockDriver, ConfigReplacesABadParameterByItsDefault) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("bad", iftdConfig(
        ", \"if_phase_td_sat_clock_event\": true, \"if_phase_td_sat_clock_min_stations\": 1,"
        " \"if_phase_td_sat_clock_min_fraction\": 1.5, \"if_phase_td_sat_clock_coherence\": 0"),
        cfg, log));
    const auto& sq = cfg.getNetworkConfig().slip_qc;
    EXPECT_EQ(sq.if_phase_td_sat_clock_min_stations, 4);
    EXPECT_DOUBLE_EQ(sq.if_phase_td_sat_clock_min_fraction, 0.75);
    EXPECT_DOUBLE_EQ(sq.if_phase_td_sat_clock_coherence, 0.25);
}
