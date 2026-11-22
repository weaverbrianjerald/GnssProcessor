/**
 * @file test_station_memory.cpp
 * @brief The station memory section of the snapshot (network/state/StationMemory.hpp): the wind-up
 *        accumulator's export and restore, the section's text in the snapshot, its fingerprint, the
 *        loader's interlocks, and the write and the restore through a run.
 *
 * What these cases pin:
 *   - an exported accumulator restored into a fresh instance reads back bit for bit (radians and
 *     continuous seconds, negative and multi-turn angles included);
 *   - a restored entry continues the unwrap at the next epoch exactly as the uninterrupted
 *     instance does, where a fresh instance restarts a whole turn away once the arc has passed
 *     half a cycle (the defect the section repairs);
 *   - a stale restored entry, past the continuity horizon, restarts and enters the restart ledger,
 *     as the uninterrupted instance does, where a fresh instance seeds silently;
 *   - the snapshot carries the section verbatim, and nothing without it; a cut section fails;
 *   - the section's fingerprint follows the wind-up keys, and the snapshot's own fingerprint is the
 *     same with the keys on or off;
 *   - both keys default off, and each needs a snapshot path and the accumulated wind-up;
 *   - the write and the restore through a run, with each refusal counted.
 */

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/slip/GfArcJump.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationMemory.hpp"
#include "products/NetworkStateSnapshot.hpp"

#include "../support/ModelStack.hpp"
#include "../support/NetworkTestRun.hpp"

using namespace GnssProcessor;

namespace {

using Eigen::Vector3d;

constexpr double kL1 = Constants::GPS_L1_WAVELENGTH;
constexpr double kDeg = Constants::PI / 180.0;
constexpr double kAu = 1.495978707e11;
constexpr double kGpsRadius = 26560000.0;
constexpr int    kWeek = 2300;
constexpr double kTow0 = 100000.0;
constexpr double kEpoch = 30.0;
constexpr double kRateDegPerS = 10.0 / 30.0;   // the Sun turns 10 degrees per epoch about the nadir: -1/36 cycle

/// A station at 40 N, 105 W and a GPS satellite at elevation 45, azimuth 120, the Sun turning in the
/// satellite's transverse plane (the geometry of the wind-up continuity tests).
struct Scene {
    Vector3d rx_pos, sat_pos, a1, a2;

    Scene() {
        const double lat = 40.0 * kDeg, lon = -105.0 * kDeg;
        rx_pos = Coordinates::llhToEcef(lat, lon, 1650.0);
        const Vector3d north(-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat));
        const Vector3d east(-std::sin(lon), std::cos(lon), 0.0);
        const Vector3d up(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
        const double el = 45.0 * kDeg, az = 120.0 * kDeg;
        const Vector3d los = std::cos(el) * (std::cos(az) * north + std::sin(az) * east) + std::sin(el) * up;
        const double b = 2.0 * rx_pos.dot(los);
        const double c = rx_pos.squaredNorm() - kGpsRadius * kGpsRadius;
        sat_pos = rx_pos + 0.5 * (-b + std::sqrt(b * b - 4.0 * c)) * los;
        const Vector3d ez = -sat_pos.normalized();
        a1 = ez.cross(rx_pos).normalized();
        a2 = ez.cross(a1);
    }

    Vector3d sunAt(double dt_s) const {
        const double th = kRateDegPerS * dt_s * kDeg;
        return sat_pos + kAu * (std::cos(th) * a1 + std::sin(th) * a2);
    }

    /// The accumulated wind-up (metres) for @p sat at @p dt_s.
    double call(GnssCorrections& gc, const std::string& sat, double dt_s) const {
        return gc.computePhaseWindUp(sat, System::GPS, rx_pos, sat_pos, kL1, GnssTime{ kWeek, kTow0 + dt_s },
                                     sunAt(dt_s), Vector3d::Zero(), /*accumulate=*/true, /*max_gap_s=*/300.0);
    }
};

double continuousSeconds(double dt_s) { return kWeek * 604800.0 + kTow0 + dt_s; }

/// A loadable two-station network configuration with @p network_extra spliced into "network" and
/// @p estimation_extra into "estimation"; the loader's text is kept.
struct Loaded {
    bool ok{false};
    GnssConfig config;
    std::string text;
};

Loaded loadConfig(const std::string& tag, const std::string& network_extra, const std::string& estimation_extra) {
    Loaded out;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("station_memory_cfg_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "config.json";
    {
        std::ofstream f(path);
        f << "{\n"
             "  \"processing\": {\n"
             "    \"mode\": \"ppp-rtk-network\",\n"
             "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
             "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
             "  },\n"
             "  \"estimation\": {\n"
          << estimation_extra
          << "    \"estimate_velocity\": false\n"
             "  },\n"
             "  \"network\": {\n"
             "    \"clock_datum_station\": \"AAAA\",\n"
          << network_extra
          << "    \"stations\": [\n"
             "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
             "    ]\n"
             "  }\n"
             "}\n";
    }
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    LogManager log;
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto cfg_log = log.getLogger("gnss_config");
    auto main_log = log.getLogger();
    cfg_log->sinks().push_back(sink);
    main_log->sinks().push_back(sink);
    out.ok = out.config.loadFromJson(path, log);
    cfg_log->flush();
    main_log->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    return out;
}

const char* kAccumulate = "    \"accumulate_phase_windup\": true,\n";
const char* kBothKeys =
    "    \"state_snapshot_path\": \"snap.txt\",\n"
    "    \"state_snapshot_station_memory_capture\": true,\n"
    "    \"resume_station_memory_restore\": true,\n";

/// The snapshot's own fingerprint a run of @p cfg computes (two stations, the first the pivot).
std::uint64_t resumeFingerprint(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

bool contains(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

}  // namespace

// ---- The accumulator's export and restore ------------------------------------------------------------

TEST(StationMemoryRecords, ExportAndRestoreRoundTripBitForBit) {
    LogManager log;
    GnssCorrections a(log);
    const std::vector<GnssCorrections::WindUpRecord> in{
        { "E04", -13.82937261810345, 1399165230.0012, 1399161600.0012 },
        { "G01", 0.1, 1399165230.0, 1399165230.0 },
        { "G30", 25.13274122871834, 1399165200.5, 1399150000.25 },
    };
    a.restoreWindUpRecords(in);
    const std::vector<GnssCorrections::WindUpRecord> out = a.windUpRecords();
    ASSERT_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(out[i].sat, in[i].sat);
        EXPECT_EQ(out[i].angle_rad, in[i].angle_rad);
        EXPECT_EQ(out[i].epoch_s, in[i].epoch_s);
        EXPECT_EQ(out[i].start_epoch_s, in[i].start_epoch_s);
    }
    // Restoring replaces the whole accumulator.
    a.restoreWindUpRecords({ in[1] });
    ASSERT_EQ(a.windUpRecords().size(), 1u);
    EXPECT_EQ(a.windUpRecords()[0].sat, "G01");
}

TEST(StationMemoryRecords, ARestoredEntryContinuesTheUnwrapWhereAFreshOneTurnsAway) {
    const Scene sc;
    LogManager log;
    GnssCorrections writer(log);
    // 21 epochs: the Sun turns 200 degrees, the accumulator falls 0.556 cycle past its start.
    for (int k = 0; k <= 20; ++k) sc.call(writer, "G30", k * kEpoch);
    GnssCorrections resumed(log);
    resumed.restoreWindUpRecords(writer.windUpRecords());
    GnssCorrections fresh(log);
    for (int k = 21; k <= 40; ++k) {
        const double w = sc.call(writer, "G30", k * kEpoch);
        const double r = sc.call(resumed, "G30", k * kEpoch);
        const double f = sc.call(fresh, "G30", k * kEpoch);
        EXPECT_EQ(r, w) << "epoch " << k;
        const double turns = (f - w) / kL1;
        EXPECT_NEAR(turns, std::round(turns), 1e-9) << "epoch " << k;
        EXPECT_NE(std::lround(turns), 0) << "epoch " << k;
    }
    EXPECT_TRUE(resumed.wuRestarts().empty());
}

TEST(StationMemoryRecords, AStaleRestoredEntryRestartsIntoTheLedgerAsTheWriterDoes) {
    const Scene sc;
    LogManager log;
    GnssCorrections writer(log);
    for (int k = 0; k <= 5; ++k) sc.call(writer, "G30", k * kEpoch);
    GnssCorrections resumed(log);
    resumed.restoreWindUpRecords(writer.windUpRecords());
    GnssCorrections fresh(log);
    // The satellite returns 600 s after its last epoch, past the 300 s horizon.
    const double back = 5 * kEpoch + 600.0;
    sc.call(writer, "G30", back);
    sc.call(resumed, "G30", back);
    sc.call(fresh, "G30", back);
    ASSERT_EQ(writer.wuRestarts().count("G30"), 1u);
    ASSERT_EQ(resumed.wuRestarts().count("G30"), 1u);
    EXPECT_EQ(resumed.wuRestarts().at("G30").gap_s, writer.wuRestarts().at("G30").gap_s);
    EXPECT_EQ(resumed.wuRestarts().at("G30").epoch_s, continuousSeconds(back));
    EXPECT_TRUE(fresh.wuRestarts().empty());
    ASSERT_EQ(resumed.windUpRecords().size(), 1u);
    EXPECT_EQ(resumed.windUpRecords()[0].start_epoch_s, continuousSeconds(back));
}

// ---- The section in the snapshot -------------------------------------------------------------------

TEST(StationMemorySection, SnapshotCarriesTheSectionVerbatimAndNothingWithoutIt) {
    NetworkStateSnapshot snap;
    snap.snap_week = 2313;
    snap.snap_tow = 279150.0;
    snap.n_emitted = 665;
    snap.config_fingerprint = 0xc5b5df834d185ac8ULL;
    std::ostringstream plain;
    NetworkStateSnapshotIO::serialize(plain, snap);
    EXPECT_EQ(plain.str().find("STMEM,"), std::string::npos);

    snap.chain_memory = "CHAIN,v2,0123456789abcdef\nGD,E04,-0.4188\nCHEND\n";
    snap.station_memory = "STMEM,v2,0123456789abcdef\nWU,AAAA,G30,-3.4906585039886591,1399165230,1399161600\nSMEND\n";
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, snap);
    std::istringstream is(os.str());
    NetworkStateSnapshot back;
    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, back, &err)) << err;
    EXPECT_EQ(back.station_memory, snap.station_memory);
    EXPECT_EQ(back.chain_memory, snap.chain_memory);

    std::string cut = os.str();
    const auto at = cut.find("SMEND\n");
    ASSERT_NE(at, std::string::npos);
    cut.erase(at, 6);
    std::istringstream is2(cut);
    NetworkStateSnapshot back2;
    EXPECT_FALSE(NetworkStateSnapshotIO::parse(is2, back2, &err));
}

TEST(ParkedCarrySection, SnapshotCarriesParkedCarriesExactlyAndNothingWithoutThem) {
    NetworkStateSnapshot snap;
    snap.snap_week = 2313;
    snap.snap_tow = 270000.0;
    snap.n_emitted = 361;
    std::ostringstream plain;
    NetworkStateSnapshotIO::serialize(plain, snap);
    EXPECT_EQ(plain.str().find("\nDP,"), std::string::npos);
    snap.parked_carry.push_back({ 0, "G25", "", -0.6117512931251, 2.6461e-03, 1399161600.0012 });
    snap.parked_carry.push_back({ 2, "G25", "C1W", 3.0088031275, 3.4386290001e-04, 1399161630.5 });
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, snap);
    std::istringstream is(os.str());
    NetworkStateSnapshot back;
    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, back, &err)) << err;
    ASSERT_EQ(back.parked_carry.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(back.parked_carry[i].kind, snap.parked_carry[i].kind);
        EXPECT_EQ(back.parked_carry[i].sat, snap.parked_carry[i].sat);
        EXPECT_EQ(back.parked_carry[i].signal, snap.parked_carry[i].signal);
        EXPECT_EQ(back.parked_carry[i].value, snap.parked_carry[i].value);
        EXPECT_EQ(back.parked_carry[i].var, snap.parked_carry[i].var);
        EXPECT_EQ(back.parked_carry[i].t_park_s, snap.parked_carry[i].t_park_s);
    }
    // A malformed DP line fails the parse (the cold-start signal).
    std::string bad = os.str();
    const auto at = bad.find("DP,2,G25,C1W,");
    ASSERT_NE(at, std::string::npos);
    bad.replace(at, 13, "DP,2,G25,");
    std::istringstream is2(bad);
    NetworkStateSnapshot back2;
    EXPECT_FALSE(NetworkStateSnapshotIO::parse(is2, back2, &err));
}

TEST(ParkedCarryConfig, KeysDefaultOffAndNeedAPathAndTheReadmissionSeed) {
    const char* park = "    \"state_snapshot_park_capture\": true,\n    \"resume_park_restore\": true,\n";
    const Loaded def = loadConfig("park_def", "    \"readmit_seed_horizon_s\": 3900,\n", "");
    ASSERT_TRUE(def.ok) << def.text;
    EXPECT_FALSE(def.config.networkStateSnapshotParkCapture());
    EXPECT_FALSE(def.config.networkResumeParkRestore());
    const Loaded noseed = loadConfig("park_noseed", std::string("    \"state_snapshot_path\": \"snap.txt\",\n") + park, "");
    ASSERT_TRUE(noseed.ok) << noseed.text;
    EXPECT_FALSE(noseed.config.networkStateSnapshotParkCapture());
    EXPECT_FALSE(noseed.config.networkResumeParkRestore());
    EXPECT_TRUE(contains(noseed.text, "readmit_seed_horizon_s"));
    const Loaded nopath = loadConfig("park_nopath", std::string("    \"readmit_seed_horizon_s\": 3900,\n") + park, "");
    ASSERT_TRUE(nopath.ok) << nopath.text;
    EXPECT_FALSE(nopath.config.networkResumeParkRestore());
    const Loaded both = loadConfig("park_both", std::string("    \"state_snapshot_path\": \"snap.txt\",\n"
                                                            "    \"readmit_seed_horizon_s\": 3900,\n") + park, "");
    ASSERT_TRUE(both.ok) << both.text;
    EXPECT_TRUE(both.config.networkStateSnapshotParkCapture());
    EXPECT_TRUE(both.config.networkResumeParkRestore());
}

// ---- The fingerprint and the loader -----------------------------------------------------------------

TEST(StationMemoryFingerprint, FollowsTheWindUpKeysAndLeavesTheSnapshotFingerprint) {
    const Loaded base = loadConfig("fp_base", kBothKeys, kAccumulate);
    const Loaded gap = loadConfig("fp_gap", kBothKeys, std::string(kAccumulate) + "    \"phase_windup_max_gap_s\": 120.0,\n");
    const Loaded other = loadConfig("fp_other", std::string(kBothKeys) + "    \"ring_fix\": { \"wl_frac_max\": 0.2 },\n",
                                    kAccumulate);
    const Loaded off = loadConfig("fp_off", "    \"state_snapshot_path\": \"snap.txt\",\n", kAccumulate);
    ASSERT_TRUE(base.ok) << base.text;
    ASSERT_TRUE(gap.ok) << gap.text;
    ASSERT_TRUE(other.ok) << other.text;
    ASSERT_TRUE(off.ok) << off.text;
    EXPECT_NE(networkStationMemoryFingerprint(base.config), networkStationMemoryFingerprint(gap.config));
    EXPECT_EQ(networkStationMemoryFingerprint(base.config), networkStationMemoryFingerprint(other.config));
    EXPECT_EQ(resumeFingerprint(base.config), resumeFingerprint(off.config));
}

TEST(StationMemoryConfig, KeysDefaultOffAndEachNeedsAPathAndTheAccumulator) {
    const Loaded def = loadConfig("cfg_def", "", kAccumulate);
    ASSERT_TRUE(def.ok) << def.text;
    EXPECT_FALSE(def.config.networkStateSnapshotStationMemoryCapture());
    EXPECT_FALSE(def.config.networkResumeStationMemoryRestore());
    // Without a snapshot path: both disarmed with a warning.
    const Loaded nopath = loadConfig("cfg_nopath",
                                     "    \"state_snapshot_station_memory_capture\": true,\n"
                                     "    \"resume_station_memory_restore\": true,\n",
                                     kAccumulate);
    ASSERT_TRUE(nopath.ok) << nopath.text;
    EXPECT_FALSE(nopath.config.networkStateSnapshotStationMemoryCapture());
    EXPECT_FALSE(nopath.config.networkResumeStationMemoryRestore());
    EXPECT_TRUE(contains(nopath.text, "state_snapshot_station_memory_capture"));
    // Without the accumulated wind-up: disarmed with a warning (fractional mode keeps no accumulator).
    const Loaded frac = loadConfig("cfg_frac", kBothKeys, "");
    ASSERT_TRUE(frac.ok) << frac.text;
    EXPECT_FALSE(frac.config.networkStateSnapshotStationMemoryCapture());
    EXPECT_FALSE(frac.config.networkResumeStationMemoryRestore());
    EXPECT_TRUE(contains(frac.text, "accumulate_phase_windup"));
    const Loaded both = loadConfig("cfg_both", kBothKeys, kAccumulate);
    ASSERT_TRUE(both.ok) << both.text;
    EXPECT_TRUE(both.config.networkStateSnapshotStationMemoryCapture());
    EXPECT_TRUE(both.config.networkResumeStationMemoryRestore());
}

// ---- The write and the restore through a run ----------------------------------------------------------

namespace {

/// A run whose stations each hold a real model, with a logger that keeps what it prints.
struct StationRun {
    LogManager                      lm;
    std::vector<std::unique_ptr<gnss_test::ModelStack>> stacks;
    std::ostringstream              text;
    std::shared_ptr<spdlog::logger> log;
    std::unique_ptr<NetworkTestRun> fx;

    StationRun(const std::string& tag, const GnssConfig& cfg, const std::vector<std::string>& names,
               bool accumulate) {
        log = std::make_shared<spdlog::logger>("station_memory_" + tag,
                                               std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        log->set_level(spdlog::level::info);
        fx = std::make_unique<NetworkTestRun>(cfg, log);
        for (const std::string& name : names) {
            auto stack = std::make_unique<gnss_test::ModelStack>();
            const bool ok = stack->load(lm, tag + "_" + name, "ppp-rtk", "",
                                        accumulate ? ", \"accumulate_phase_windup\": true" : "", false);
            EXPECT_TRUE(ok);
            fx->setup().stations.emplace_back();
            fx->setup().stations.back().name = name;
            fx->setup().stations.back().model = std::move(stack->ppp);
            stacks.push_back(std::move(stack));
        }
    }
    PppModel& model(std::size_t i) { return *fx->setup().stations[i].model; }
};

}  // namespace

TEST(StationMemoryRestore, ASectionWithItsOwnFingerprintIsRestoredAndConsumed) {
    const Loaded cfg = loadConfig("res_ok", kBothKeys, kAccumulate);
    ASSERT_TRUE(cfg.ok) << cfg.text;
    StationRun writer("w_ok", cfg.config, { "AAAA", "BBBB" }, true);
    const std::vector<GnssCorrections::WindUpRecord> a{ { "G30", -3.4906585039886591, 1399165230.0, 1399161600.0 },
                                                        { "E04", 12.566370614359172, 1399165200.0, 1399150000.5 } };
    const std::vector<GnssCorrections::WindUpRecord> b{ { "G01", 0.25, 1399165230.0, 1399165230.0 } };
    ASSERT_TRUE(writer.model(0).restoreWindUpRecords(a));
    ASSERT_TRUE(writer.model(1).restoreWindUpRecords(b));
    long long n = 0;
    const std::string text = networkStationMemoryWrite(writer.fx->run(), n);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(text.substr(0, text.find('\n')),
              fmt::format("STMEM,v2,{:016x}", networkStationMemoryFingerprint(cfg.config)));

    StationRun leg("r_ok", cfg.config, { "BBBB", "AAAA" }, true);   // another roster order: matched by name
    leg.fx->ctx().carry.resume.station_memory = text;
    networkStationMemoryRestore(leg.fx->run());
    EXPECT_TRUE(leg.fx->ctx().carry.resume.station_memory.empty());
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_restored, 1);
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_records_restored, 3);
    const auto got_a = leg.model(1).windUpRecords();
    ASSERT_EQ(got_a.size(), 2u);
    EXPECT_EQ(got_a[0].sat, "E04");   // sorted by satellite
    EXPECT_EQ(got_a[0].angle_rad, a[1].angle_rad);
    EXPECT_EQ(got_a[0].start_epoch_s, a[1].start_epoch_s);
    EXPECT_EQ(got_a[1].angle_rad, a[0].angle_rad);
    EXPECT_EQ(got_a[1].epoch_s, a[0].epoch_s);
    ASSERT_EQ(leg.model(0).windUpRecords().size(), 1u);
    EXPECT_EQ(leg.model(0).windUpRecords()[0].angle_rad, 0.25);
    EXPECT_TRUE(contains(leg.text.str(), "the station memory section restored at the resume"));
}

TEST(StationMemoryRestore, TheGfArcsAndTheSlipEpochCounterRoundTripExactly) {
    const Loaded cfg = loadConfig("res_gf", std::string(kBothKeys) + "    \"cycle_slip\": { \"gf_arc_jump\": true },\n",
                                  kAccumulate);
    ASSERT_TRUE(cfg.ok) << cfg.text;
    StationRun writer("w_gf", cfg.config, { "AAAA", "BBBB" }, true);
    SlipQcChannel& wq = writer.fx->run().engines.slip_qc;
    wq.params.gf_jump.armed = true;
    wq.hist.resize(2);
    wq.epoch_idx = 361;
    GfArcJumpArc a;
    a.last_epoch = 361;
    a.last_t_s = 1399170000.0012;
    a.sig1 = "L1C";
    a.sig2 = "L2W";
    a.gf_m = -3.0517578125e-05 + 12.25;
    a.has_ref = true;
    a.ref_step_m = -0.00123456789;
    a.ref_span_s = 30.0;
    a.flag_run = 2;
    a.clean_r = { 0.001, -0.0025, 3.3e-4 };
    wq.hist[1].gf_arcs["G05"] = a;
    GfArcJumpArc b;   // a fresh arc: no reference, no residuals
    b.last_epoch = 360;
    b.last_t_s = 1399169970.0;
    b.sig1 = "L1C";
    b.sig2 = "L5Q";
    wq.hist[0].gf_arcs["E11"] = b;
    long long n = 0;
    const std::string text = networkStationMemoryWrite(writer.fx->run(), n);
    EXPECT_EQ(n, 2);

    StationRun leg("r_gf", cfg.config, { "BBBB", "AAAA" }, true);   // another roster order: matched by name
    SlipQcChannel& lq = leg.fx->run().engines.slip_qc;
    lq.hist.resize(2);
    leg.fx->ctx().carry.resume.station_memory = text;
    networkStationMemoryRestore(leg.fx->run());
    EXPECT_EQ(lq.epoch_idx, 361);
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_gf_arcs_restored, 2);
    ASSERT_EQ(lq.hist[0].gf_arcs.count("G05"), 1u);   // BBBB is index 0 in the leg
    const GfArcJumpArc& r = lq.hist[0].gf_arcs.at("G05");
    EXPECT_EQ(r.last_epoch, a.last_epoch);
    EXPECT_EQ(r.last_t_s, a.last_t_s);
    EXPECT_EQ(r.sig1, a.sig1);
    EXPECT_EQ(r.sig2, a.sig2);
    EXPECT_EQ(r.gf_m, a.gf_m);
    EXPECT_EQ(r.has_ref, a.has_ref);
    EXPECT_EQ(r.ref_step_m, a.ref_step_m);
    EXPECT_EQ(r.ref_span_s, a.ref_span_s);
    EXPECT_EQ(r.flag_run, a.flag_run);
    EXPECT_EQ(r.clean_r, a.clean_r);
    ASSERT_EQ(lq.hist[1].gf_arcs.count("E11"), 1u);
    EXPECT_FALSE(lq.hist[1].gf_arcs.at("E11").has_ref);
    EXPECT_TRUE(lq.hist[1].gf_arcs.at("E11").clean_r.empty());
    EXPECT_EQ(lq.hist[1].gf_arcs.at("E11").sig2, "L5Q");
}

TEST(StationMemoryRestore, RefusalsAreCountedAndTheResumeGoesOn) {
    const Loaded cfg = loadConfig("res_ref", kBothKeys, kAccumulate);
    const Loaded other = loadConfig("res_ref_other", kBothKeys,
                                    std::string(kAccumulate) + "    \"phase_windup_max_gap_s\": 120.0,\n");
    ASSERT_TRUE(cfg.ok) << cfg.text;
    ASSERT_TRUE(other.ok) << other.text;
    StationRun writer("w_ref", cfg.config, { "AAAA" }, true);
    ASSERT_TRUE(writer.model(0).restoreWindUpRecords({ { "G30", 1.5, 1399165230.0, 1399161600.0 } }));
    long long n = 0;
    std::string text = networkStationMemoryWrite(writer.fx->run(), n);

    // Another wind-up configuration: the whole section is refused.
    StationRun refused("r_fp", other.config, { "AAAA" }, true);
    refused.fx->ctx().carry.resume.station_memory = text;
    networkStationMemoryRestore(refused.fx->run());
    EXPECT_EQ(refused.fx->ctx().counters.station_memory_refused, 1);
    EXPECT_EQ(refused.fx->ctx().counters.station_memory_restored, 0);
    EXPECT_TRUE(refused.model(0).windUpRecords().empty());
    EXPECT_TRUE(contains(refused.text.str(), "is REFUSED"));

    // Another build's header: refused.
    StationRun old_build("r_old", cfg.config, { "AAAA" }, true);
    old_build.fx->ctx().carry.resume.station_memory = "STMEM,v1,0123456789abcdef\nWU,AAAA,G30,1.5,1399165230,1399161600\nSMEND\n";
    networkStationMemoryRestore(old_build.fx->run());
    EXPECT_EQ(old_build.fx->ctx().counters.station_memory_refused, 1);
    EXPECT_TRUE(old_build.model(0).windUpRecords().empty());

    // A station not in the run, a malformed line, and a station in fractional mode: each refused by reason.
    const std::string header = text.substr(0, text.find('\n') + 1);
    const std::string mixed = header + "WU,AAAA,G30,1.5,1399165230,1399161600\n"
                                       "WU,ZZZZ,G05,0.5,1399165230,1399161600\n"
                                       "WU,AAAA,G07,1.5x,1399165230,1399161600\n"
                                       "WU,FRAC,G09,0.5,1399165230,1399161600\n"
                                       "SMEND\n";
    StationRun leg("r_mix", cfg.config, { "AAAA" }, true);
    auto frac = std::make_unique<gnss_test::ModelStack>();
    ASSERT_TRUE(frac->load(leg.lm, "r_mix_frac", "ppp-rtk", "", "", false));
    leg.fx->setup().stations.emplace_back();
    leg.fx->setup().stations.back().name = "FRAC";
    leg.fx->setup().stations.back().model = std::move(frac->ppp);
    leg.fx->ctx().carry.resume.station_memory = mixed;
    networkStationMemoryRestore(leg.fx->run());
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_restored, 1);
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_records_restored, 1);
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_refused_station, 1);
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_refused_malformed, 1);
    EXPECT_EQ(leg.fx->ctx().counters.station_memory_refused_fractional, 1);
    ASSERT_EQ(leg.model(0).windUpRecords().size(), 1u);
    EXPECT_EQ(leg.model(0).windUpRecords()[0].sat, "G30");

    // Nothing staged: a no-op that says nothing.
    StationRun idle("r_idle", cfg.config, { "AAAA" }, true);
    networkStationMemoryRestore(idle.fx->run());
    EXPECT_EQ(idle.fx->ctx().counters.station_memory_restored + idle.fx->ctx().counters.station_memory_refused, 0);
    EXPECT_FALSE(contains(idle.text.str(), "station memory"));
}
