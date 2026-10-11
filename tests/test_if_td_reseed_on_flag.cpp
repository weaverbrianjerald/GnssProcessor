/**
 * @file test_if_td_reseed_on_flag.cpp
 * @brief IF-TD post-flag re-seed (`network.cycle_slip.if_phase_td_reseed_on_flag`,
 *        card ARL, slip-ladder defect 2).
 *
 * ### The defect
 *
 * A pooled IF-TD flag restarts the flagged satellite's gradient chain
 * (IfPhaseSlipDetector::resetSatellite erases its stored single difference).  The
 * epoch after the flag then has no double difference for that satellite: every
 * pool-arm rung of the slip ladder evaluates one sample fewer than the control,
 * and a second slip in that epoch (rung S7, repetition 2) passes every pre-fit
 * tier.  The re-seed keeps the flag epoch's own single difference, which already
 * contains the step, as the next epoch's origin.
 *
 * ### What is gated here
 *
 * - Detector (@c IfTdReseedOnFlag.Detector...): a reset leaves the next epoch
 *   blind and a repeated step unseen; a re-seed evaluates that epoch, reads about
 *   0 on a clean arc and the full step on a repeated one, marks exactly that one
 *   sample (IfJumpInfo::after_reseed), scopes like the reset (whole satellite, or
 *   one chain), and acts as a reset when there is nothing to keep.
 * - Driver (@c IfTdReseedOnFlag.Driver...): networkSlipIfTdDriver evaluates the
 *   post-flag sample only when the knob is armed, flags a repeated slip there,
 *   and counts both in the pair tally.
 * - Loader (@c IfTdReseedOnFlag.Config...): default off, parsed when set.
 *
 * Synthetic uncombined observations as in test_if_td_pair_width.cpp: the model is
 * 0 on every band and the observed value carries the whole OMC, so a per-band
 * offset is the only thing that separates a slip from the quiet stream.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/IfPhaseSlipDetector.hpp"
#include "slip/NetworkRawObservables.hpp"
#include "slip/NetworkSlipQc.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <Eigen/Dense>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

const std::string kP12 = "L1C_L2W";
const std::string kP15 = "L1C_L5Q";

/// The IF step of an L1-only slip of @p n cycles: c1 * n * lambda1, with
/// c1 = f1^2 / (f1^2 - f2^2) (about 2.546), so +2 cycles move the IF phase 0.969 m.
double ifStepL1(double n) {
    const double f1 = Constants::GPS_L1_FREQ, f2 = Constants::GPS_L2_FREQ;
    return f1 * f1 / (f1 * f1 - f2 * f2) * n * Constants::GPS_L1_WAVELENGTH;
}

/// Deterministic sub-millimetre wobble (m) so pooled scales are never degenerate;
/// two orders of magnitude below the ~0.107 m physical floor.
constexpr double kWobbleM[8] = { 0.0004, -0.0003, 0.0002,  0.0005,
                                -0.0006,  0.0001, -0.0002, 0.0003 };
double wobble(std::size_t epoch, std::size_t sat, std::size_t station) {
    return kWobbleM[(epoch * 3u + sat + station * 5u) % 8u];
}

using OmcState   = std::map<std::string, double>;   ///< satellite -> common OMC (m)
using ElevState  = std::map<std::string, double>;   ///< satellite -> elevation (deg)
/// satellite -> (signal -> extra observed value on that band alone, m).
using BandOffset = std::map<std::string, std::map<std::string, double>>;

/// Six GPS satellites; G01 is the highest and stays above the 45 deg retention
/// threshold, so every chain elects and keeps it as the reference.
ElevState sixSatElevations() {
    return { { "G01", 80.0 }, { "G02", 70.0 }, { "G03", 40.0 },
             { "G04", 35.0 }, { "G05", 30.0 }, { "G06", 25.0 } };
}
OmcState sixSatOmc() {
    return { { "G01", 0.00 }, { "G02", 0.12 }, { "G03", -0.07 },
             { "G04", 0.21 }, { "G05", -0.15 }, { "G06", 0.05 } };
}

/// One epoch of uncombined observations on @p bands at station index @p station.
std::vector<PppObservation> makeEpoch(std::size_t epoch_idx, std::size_t station,
                                      const std::vector<std::string>& bands,
                                      const BandOffset& extra = {}) {
    const OmcState  omc  = sixSatOmc();
    const ElevState elev = sixSatElevations();
    std::vector<PppObservation> out;
    std::size_t k = 0;
    for (const auto& [sat, v0] : omc) {
        const double v = v0 + wobble(epoch_idx, k++, station);
        PppObservation obs{};
        obs.satellite = sat;
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2313, 261000.0 + 30.0 * static_cast<double>(epoch_idx) };
        obs.elevation = elev.at(sat);
        obs.azimuth   = 0.0;
        obs.los       = Eigen::Vector3d(0.0, 0.0, 1.0);
        obs.sat_pos   = Eigen::Vector3d::Zero();
        obs.rx_pos_linearization = Eigen::Vector3d::Zero();
        const auto eit = extra.find(sat);
        for (const auto& sig : bands) {
            double add = 0.0;
            if (eit != extra.end()) {
                const auto sit = eit->second.find(sig);
                if (sit != eit->second.end()) add = sit->second;
            }
            obs.modeled[sig]  = 0.0;
            obs.observed[sig] = v + add;
        }
        out.push_back(std::move(obs));
    }
    return out;
}

/// An L1-only slip of @p cycles on @p sat, as a cumulative band offset (m).
BandOffset l1Offset(const std::string& sat, double cycles) {
    return { { sat, { { "L1C", cycles * Constants::GPS_L1_WAVELENGTH } } } };
}

/// One detector in the network wiring (deferred flagging).
struct DetHarness {
    LogManager log_manager;
    GnssConfig config;
    IfPhaseSlipDetector det{ config, log_manager };
    std::vector<std::string> bands{ "L1C", "L2W" };

    explicit DetHarness(bool all_pairs = false) {
        det.setDeferFlagging(true);
        det.setAllPairs(all_pairs);
    }
    void step(std::size_t e, const BandOffset& extra = {}) {
        det.detect(makeEpoch(e, 0, bands, extra));
    }
    const IfPhaseSlipDetector::IfJumpInfo* sample(const std::string& sat,
                                                  const std::string& pair = kP12) const {
        const auto it = det.getLastIfJumpsAllPairs().find({ sat, pair });
        return it == det.getLastIfJumpsAllPairs().end() ? nullptr : &it->second;
    }
    double dd(const std::string& sat, const std::string& pair = kP12) const {
        const auto* s = sample(sat, pair);
        return s ? s->delta_grad_m : std::numeric_limits<double>::quiet_NaN();
    }
    bool afterReseed(const std::string& sat, const std::string& pair = kP12) const {
        const auto* s = sample(sat, pair);
        return s && s->after_reseed;
    }
};

}  // namespace

// ------------------------------------------------------------------ detector

/// The defect: after a reset the next epoch has no double difference for the
/// flagged satellite, so a step repeated in that epoch is absorbed unseen (the
/// chain restarts from the post-step gradient and the following epoch reads 0).
TEST(IfTdReseedOnFlag, DetectorResetLeavesThePostFlagEpochBlind) {
    DetHarness h;
    h.step(0);
    h.step(1);
    h.step(2, l1Offset("G03", 2.0));           // first slip, flagged by the driver
    ASSERT_NEAR(h.dd("G03"), ifStepL1(2.0), 3e-3);
    h.det.resetSatellite("G03");
    h.step(3, l1Offset("G03", 4.0));           // the slip repeats
    EXPECT_TRUE(std::isnan(h.dd("G03")));      // no double difference: unseen
    EXPECT_FALSE(h.afterReseed("G03"));
    h.step(4, l1Offset("G03", 4.0));
    EXPECT_NEAR(h.dd("G03"), 0.0, 3e-3);       // and it never shows later either
}

/// A re-seed keeps the flag epoch's gradient: the next epoch has a double
/// difference, the first step is not counted again (about 0), and that sample is
/// the one marked after_reseed.
TEST(IfTdReseedOnFlag, DetectorReseedEvaluatesThePostFlagEpoch) {
    DetHarness h;
    h.step(0);
    h.step(1);
    h.step(2, l1Offset("G03", 2.0));
    ASSERT_TRUE(h.det.reseedSatellite("G03"));
    h.step(3, l1Offset("G03", 2.0));
    ASSERT_FALSE(std::isnan(h.dd("G03")));
    EXPECT_NEAR(h.dd("G03"), 0.0, 3e-3);
    EXPECT_TRUE(h.afterReseed("G03"));
    for (const char* other : { "G01", "G02", "G04", "G05", "G06" })
        EXPECT_FALSE(h.afterReseed(other)) << other;
    // The mark describes one sample only.
    h.step(4, l1Offset("G03", 2.0));
    EXPECT_FALSE(h.afterReseed("G03"));
    EXPECT_NEAR(h.dd("G03"), 0.0, 3e-3);
}

/// Rung S7: the slip repeats in the epoch after the flag.  Under the re-seed that
/// epoch reads the second step at full size.
TEST(IfTdReseedOnFlag, DetectorReseedSeesASlipRepeatedInThePostFlagEpoch) {
    DetHarness h;
    h.step(0);
    h.step(1);
    h.step(2, l1Offset("G03", 2.0));
    ASSERT_TRUE(h.det.reseedSatellite("G03"));
    h.step(3, l1Offset("G03", 4.0));
    EXPECT_NEAR(h.dd("G03"), ifStepL1(2.0), 3e-3);
    EXPECT_TRUE(h.afterReseed("G03"));
    // A second re-seed carries on from the repeated step.
    ASSERT_TRUE(h.det.reseedSatellite("G03"));
    h.step(4, l1Offset("G03", 4.0));
    EXPECT_NEAR(h.dd("G03"), 0.0, 3e-3);
    EXPECT_TRUE(h.afterReseed("G03"));
}

/// The per-chain variant keeps and marks one chain only, as resetSatelliteInState
/// restarts one chain only.
TEST(IfTdReseedOnFlag, DetectorReseedInStateMarksOnlyThatChain) {
    DetHarness h(/*all_pairs=*/true);
    h.bands = { "L1C", "L2W", "L5Q" };
    h.step(0);
    h.step(1);
    ASSERT_TRUE(h.det.reseedSatelliteInState("GPS|" + kP15, "G03"));
    h.step(2);
    EXPECT_TRUE(h.afterReseed("G03", kP15));
    EXPECT_FALSE(h.afterReseed("G03", kP12));
    EXPECT_FALSE(std::isnan(h.dd("G03", kP15)));
    EXPECT_FALSE(std::isnan(h.dd("G03", kP12)));
    // An unknown chain is a silent no-op.
    EXPECT_FALSE(h.det.reseedSatelliteInState("GPS|L1C_L9X", "G03"));
}

/// With no stored gradient to keep (an earlier reset erased it) the re-seed acts
/// as a reset: false is returned, the next epoch has no double difference and no
/// sample is marked.
TEST(IfTdReseedOnFlag, DetectorReseedWithNothingToKeepActsAsAReset) {
    DetHarness h;
    h.step(0);
    h.step(1);
    h.det.resetSatellite("G03");
    EXPECT_FALSE(h.det.reseedSatellite("G03"));
    h.step(2);
    EXPECT_TRUE(std::isnan(h.dd("G03")));
    EXPECT_FALSE(h.afterReseed("G03"));
    // A reset after a re-seed drops the mark with the gradient.
    h.step(3);
    ASSERT_TRUE(h.det.reseedSatellite("G04"));
    h.det.resetSatellite("G04");
    h.step(4);
    EXPECT_TRUE(std::isnan(h.dd("G04")));
    EXPECT_FALSE(h.afterReseed("G04"));
}

/// Re-seeding the chain reference takes the reset's reference handling: the
/// reference is elected again at the next epoch, and every satellite keeps a
/// finite double difference through the re-election.
TEST(IfTdReseedOnFlag, DetectorReseededReferenceIsReElected) {
    DetHarness h;
    h.step(0);
    h.step(1);
    ASSERT_TRUE(h.sample("G01") && h.sample("G01")->is_reference);
    EXPECT_TRUE(h.det.reseedSatellite("G01"));
    h.step(2);
    ASSERT_NE(h.sample("G01"), nullptr);
    EXPECT_TRUE(h.sample("G01")->is_reference);   // highest, so elected again
    for (const char* sat : { "G01", "G02", "G03", "G04", "G05", "G06" })
        EXPECT_FALSE(std::isnan(h.dd(sat))) << sat;
}

// -------------------------------------------------------------------- driver

namespace {

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_if_td_reseed", std::make_shared<spdlog::sinks::null_sink_mt>());
}

/// A minimal loadable network config with the IF-TD tier armed, the re-seed
/// optional.
std::string iftdConfig(bool reseed) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"cycle_slip\": { \"if_phase_td\": true"
        + (reseed ? ", \"if_phase_td_reseed_on_flag\": true" : "")
        + " },\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg,
                LogManager& log) {
    const fs::path p = fs::temp_directory_path() / ("iftd_reseed_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

constexpr int kStations = 6;

/// Six stations with deferred IF-TD detectors and the driver's buffers, cold carry
/// (a flag is diagnostic, so no StateManager write is involved).  The traces
/// object's streams stay closed, so event rows are no-ops.
struct DriverHarness {
    LogManager                 log_manager;
    GnssConfig                 config;
    std::vector<NetStation>    stations;
    std::vector<bool>          present = std::vector<bool>(kStations, true);
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    FlagRate                   fr;
    SyncedEpoch                se;
    NetworkRunTraces           traces{ fs::temp_directory_path() / "gnss_iftd_reseed_test_unused" };
    std::shared_ptr<spdlog::logger> log{ nullLogger() };
    IfTdPairTallies            tally;

    explicit DriverHarness(bool reseed) {
        const bool ok = loadConfig(reseed ? "on" : "off", iftdConfig(reseed), config,
                                   log_manager);
        EXPECT_TRUE(ok);
        for (int i = 0; i < kStations; ++i) {
            stations.emplace_back();
            NetStation& st = stations.back();
            st.name = "STA" + std::to_string(i);
            st.sm = std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config);
            st.if_slip = std::make_unique<IfPhaseSlipDetector>(config, log_manager);
            st.if_slip->setDeferFlagging(true);
        }
        se.time = GnssTime{ 2313, 261000.0 };
    }

    /// One epoch: every station's detector runs (station 0 carries @p extra), then
    /// the pooled driver decides.
    void epoch(std::size_t e, const BandOffset& extra = {}) {
        se.time.tow = 261000.0 + 30.0 * static_cast<double>(e);
        PassABuffers pa(stations.size());
        for (int i = 0; i < kStations; ++i)
            pa.sta_if_resets[i] = stations[i].if_slip->detect(
                makeEpoch(e, static_cast<std::size_t>(i), { "L1C", "L2W" },
                          i == 0 ? extra : BandOffset{}));
        networkSlipIfTdDriver(pa, config, stations, present, /*i_lo=*/0, kStations,
                              /*carry_invariant=*/false, /*carry_warm=*/false,
                              carry_slipped_ambs, /*trace_flag_rates=*/false, fr, se,
                              traces, log, /*refsat_mark_sats=*/nullptr,
                              /*relevel=*/nullptr, /*bridge_local_carry=*/nullptr,
                              /*bridge_cnt=*/nullptr, /*relevel_pend=*/nullptr,
                              /*raw_epoch=*/nullptr, /*raw_hist=*/nullptr,
                              /*oracle=*/nullptr, /*common_band=*/nullptr, &tally);
    }
    const IfTdPairTally& T() { return tally[kP12]; }
};

/// Runs the S7 shape: an L1 slip of +2 at epoch 2 and, when @p repeat, again at
/// epoch 3 (the post-flag epoch), then two clean epochs.
void runS7Shape(DriverHarness& h, bool repeat) {
    h.epoch(0);
    h.epoch(1);
    h.epoch(2, l1Offset("G03", 2.0));
    h.epoch(3, l1Offset("G03", repeat ? 4.0 : 2.0));
    h.epoch(4, l1Offset("G03", repeat ? 4.0 : 2.0));
    h.epoch(5, l1Offset("G03", repeat ? 4.0 : 2.0));
}

}  // namespace

/// Unarmed, the epoch after a flag evaluates one sample fewer (the slip ladder's
/// 116 628 against 116 629), and armed it does not.  A clean post-flag epoch adds
/// no flag.
TEST(IfTdReseedOnFlag, DriverEvaluatesThePostFlagEpochOnlyWhenArmed) {
    DriverHarness off(/*reseed=*/false), on(/*reseed=*/true);
    runS7Shape(off, /*repeat=*/false);
    runS7Shape(on, /*repeat=*/false);
    // Epochs 1..5 carry double differences: 5 epochs x 6 stations x 6 satellites.
    EXPECT_EQ(on.T().evaluated, 5 * kStations * 6);
    EXPECT_EQ(off.T().evaluated, on.T().evaluated - 1);
    EXPECT_EQ(off.T().flagged, 1);
    EXPECT_EQ(on.T().flagged, 1);
    EXPECT_EQ(on.T().reseeded, 1);
    EXPECT_EQ(on.T().reseed_nothing_kept, 0);
    EXPECT_EQ(on.T().post_flag_evaluated, 1);
    EXPECT_EQ(on.T().post_flag_flagged, 0);
    // The unarmed tally never moves the re-seed counters.
    EXPECT_EQ(off.T().reseeded, 0);
    EXPECT_EQ(off.T().post_flag_evaluated, 0);
}

/// Rung S7, repetition 2: the slip repeated in the post-flag epoch passes the
/// unarmed driver and is flagged by the armed one, which re-seeds again.
TEST(IfTdReseedOnFlag, DriverFlagsASlipRepeatedInThePostFlagEpoch) {
    DriverHarness off(/*reseed=*/false), on(/*reseed=*/true);
    runS7Shape(off, /*repeat=*/true);
    runS7Shape(on, /*repeat=*/true);
    EXPECT_EQ(off.T().flagged, 1);              // repetition 2 unseen
    EXPECT_EQ(on.T().flagged, 2);               // repetition 2 flagged
    EXPECT_EQ(on.T().post_flag_flagged, 1);
    EXPECT_EQ(on.T().post_flag_evaluated, 2);   // epoch 3 (flagged) and epoch 4 (clean)
    EXPECT_EQ(on.T().reseeded, 2);
    EXPECT_EQ(on.T().evaluated, 5 * kStations * 6);
}

// -------------------------------------------------------------------- loader

TEST(IfTdReseedOnFlag, ConfigDefaultsOff) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", iftdConfig(/*reseed=*/false), cfg, log));
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.if_phase_td_reseed_on_flag);
}

TEST(IfTdReseedOnFlag, ConfigParsesTheKnob) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("armed", iftdConfig(/*reseed=*/true), cfg, log));
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.if_phase_td_reseed_on_flag);
    EXPECT_TRUE(cfg.networkCycleSlipIfPhaseTd());
}

}  // namespace GnssProcessor
