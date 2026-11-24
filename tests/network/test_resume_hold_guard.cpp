/**
 * @file test_resume_hold_guard.cpp
 * @brief The resume hold guard (network/state/ResumeHoldGuard.hpp) and the loader's tie between the chain restore and
 *        the station memory restore.
 *
 * What these cases pin:
 *   - the loader refuses network.resume_chain_restore under accumulated wind-up without
 *     network.resume_station_memory_restore, and accepts it with it, or without accumulated wind-up;
 *   - a chain section written with held ring-fix links and restored while the station memory section is absent
 *     releases every restored hold before the first epoch, stamped at the snapshot's epoch, counted by constellation
 *     and declared by one RESUME event; the links stay in the table, unheld;
 *   - nothing is released when the station memory section was restored, when no chain section was restored, or under
 *     fractional wind-up.
 */

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Dense>

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "corrections/GnssCorrections.hpp"
#include "math/Coordinates.hpp"
#include "network/ar/RingFix.hpp"
#include "network/state/Entities.hpp"
#include "network/driver/Run.hpp"
#include "network/state/ChainMemory.hpp"
#include "network/state/ResumeHoldGuard.hpp"
#include "network/state/RunContext.hpp"

#include "../support/NetworkTestRun.hpp"

using namespace GnssProcessor;

namespace {

struct Loaded {
    bool ok{false};
    GnssConfig config;
    std::string text;
};

/// Loads a two-station network configuration with @p estimation spliced into "estimation" and @p network into
/// "network"; the loader's refusal is a thrown exception, caught here as not ok.
Loaded loadGuardConfig(const std::string& tag, const std::string& estimation, const std::string& network) {
    Loaded out;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("resume_hold_guard_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "config.json";
    {
        std::ofstream f(path);
        f << "{\n  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\", \"Galileo\" ] },\n"
             "  \"estimation\": {\n"
          << estimation
          << "    \"estimate_troposphere\": true\n  },\n"
             "  \"network\": {\n    \"clock_datum_station\": \"AAAA\",\n"
             "    \"phase_leveling\": true,\n    \"shared_carry_full_cov\": true,\n"
             "    \"carry_conditional_coupling\": true,\n    \"ambiguity_vehicle\": \"float_kf\",\n"
             "    \"phase_datum_eq34\": true,\n"
             "    \"state_snapshot_path\": \"snap.txt\",\n"
             "    \"state_snapshot_frame_capture\": true,\n    \"state_snapshot_datum_capture\": true,\n"
             "    \"resume_frame_restore\": true,\n    \"resume_datum_restore\": true,\n"
          << network
          << "    \"stations\": [ { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] } ]\n"
             "  }\n}\n";
    }
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    LogManager log;
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    log.getLogger("gnss_config")->sinks().push_back(sink);
    log.getLogger()->sinks().push_back(sink);
    try {
        out.ok = out.config.loadFromJson(path, log);
    } catch (const std::exception& e) {
        out.ok = false;
        oss << e.what();
    }
    log.getLogger("gnss_config")->flush();
    log.getLogger()->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    std::filesystem::remove_all(dir, ec);
    return out;
}

const char* kAccumulated = "    \"accumulate_phase_windup\": true,\n";
const char* kChain = "    \"state_snapshot_chain_capture\": true,\n    \"resume_chain_restore\": true,\n";
const char* kStationMemory =
    "    \"state_snapshot_station_memory_capture\": true,\n    \"resume_station_memory_restore\": true,\n";

bool contains(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

/// A run with a logger that keeps what it prints, and a ring fix holding three links and leaving one unheld.
struct GuardRun {
    std::ostringstream              text;
    std::shared_ptr<spdlog::logger> log;
    std::unique_ptr<NetworkTestRun> fx;

    GuardRun(const std::string& tag, const GnssConfig& cfg) {
        log = std::make_shared<spdlog::logger>("resume_hold_guard_" + tag,
                                               std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        log->set_level(spdlog::level::info);
        fx = std::make_unique<NetworkTestRun>(cfg, log);
        for (const char* name : {"AAAA", "BBBB"}) {
            fx->setup().stations.emplace_back();
            fx->setup().stations.back().name = name;
        }
        fx->ctx().carry.ring_fix = std::make_unique<NetworkRingFix>();
        NetworkRingFix& rf = *fx->ctx().carry.ring_fix;
        for (const auto& [sta, sat, s1, s2, held] :
             {std::tuple{0, "G05", "L1C", "L2W", true}, std::tuple{1, "G05", "L1C", "L2W", true},
              std::tuple{1, "E11", "L1C", "L5Q", true}, std::tuple{0, "E11", "L1C", "L5Q", false}}) {
            RingFixLink& L = rf.links[RingFixLinkId{sta, sat, s1, s2}];
            L.held = held;
            L.n1 = 7;
            L.n2 = -3;
            L.released_s = -1.0;
        }
        fx->ctx().carry.resume.snap_s = 1399165200.0;
    }
    NetworkRingFix& rf() { return *fx->ctx().carry.ring_fix; }
    long long held() {
        long long n = 0;
        for (const auto& [id, L] : rf().links) n += L.held ? 1 : 0;
        return n;
    }
};

}  // namespace

// ---- The loader ------------------------------------------------------------------------------------

TEST(ResumeHoldGuardConfig, TheChainRestoreUnderAccumulatedWindUpRequiresTheStationMemory) {
    const Loaded refused = loadGuardConfig("refused", kAccumulated, kChain);
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(contains(refused.text, "network.resume_chain_restore requires network.resume_station_memory_restore"))
        << refused.text;
    const Loaded tied = loadGuardConfig("tied", kAccumulated, std::string(kChain) + kStationMemory);
    ASSERT_TRUE(tied.ok) << tied.text;
    EXPECT_TRUE(tied.config.networkResumeChainRestore());
    EXPECT_TRUE(tied.config.networkResumeStationMemoryRestore());
    const Loaded fractional = loadGuardConfig("fractional", "", kChain);
    ASSERT_TRUE(fractional.ok) << fractional.text;
    EXPECT_TRUE(fractional.config.networkResumeChainRestore());
    const Loaded neither = loadGuardConfig("neither", kAccumulated, "");
    ASSERT_TRUE(neither.ok) << neither.text;
}

// ---- The release at the seam --------------------------------------------------------------------------

TEST(ResumeHoldGuardRelease, RestoredHoldsAreReleasedWhenTheStationMemorySectionIsAbsent) {
    const Loaded cfg = loadGuardConfig("rel", kAccumulated, std::string(kChain) + kStationMemory);
    ASSERT_TRUE(cfg.ok) << cfg.text;
    // The writer's chain section with the three held links, restored by a leg whose station memory section is absent.
    GuardRun writer("w", cfg.config);
    const std::string section = networkChainMemoryWrite(writer.fx->run());
    GuardRun leg("r", cfg.config);
    for (auto& [id, L] : leg.rf().links) L.held = false;   // the leg's own ring fix before the restore
    leg.fx->ctx().carry.resume.chain_memory = section;
    networkChainMemoryRestore(leg.fx->run());
    ASSERT_EQ(leg.fx->ctx().counters.chain_restore_restored, 1);
    ASSERT_EQ(leg.held(), 3);
    leg.fx->ctx().counters.station_memory_absent = 1;      // what the warm resume counted: no station memory section
    networkResumeHoldGuardRelease(leg.fx->run());
    const auto& C = leg.fx->ctx().counters;
    EXPECT_EQ(C.resume_hold_guard_released, 3);
    EXPECT_EQ(C.resume_hold_guard_released_gps, 2);
    EXPECT_EQ(C.resume_hold_guard_released_gal, 1);
    EXPECT_EQ(C.resume_hold_guard_why, 2);
    EXPECT_EQ(leg.held(), 0);
    EXPECT_EQ(leg.rf().links.size(), 4u);   // released, not erased
    for (const auto& [id, L] : leg.rf().links)
        if (!(id.station == 0 && id.sat == "E11")) EXPECT_EQ(L.released_s, 1399165200.0) << id.sat;
    EXPECT_TRUE(contains(leg.text.str(), "3 restored ring-fix hold(s) (G:2,E:1,other:0) are RELEASED"));
    // A second call finds nothing held.
    networkResumeHoldGuardRelease(leg.fx->run());
    EXPECT_EQ(C.resume_hold_guard_released, 3);
}

TEST(ResumeHoldGuardRelease, NothingIsReleasedWithTheStationMemoryWithoutAChainSectionOrUnderFractionalWindUp) {
    const Loaded acc = loadGuardConfig("noop", kAccumulated, std::string(kChain) + kStationMemory);
    ASSERT_TRUE(acc.ok) << acc.text;
    {
        GuardRun leg("sm", acc.config);
        leg.fx->ctx().counters.chain_restore_restored = 1;
        leg.fx->ctx().counters.station_memory_restored = 1;
        networkResumeHoldGuardRelease(leg.fx->run());
        EXPECT_EQ(leg.held(), 3);
        EXPECT_EQ(leg.fx->ctx().counters.resume_hold_guard_why, 0);
    }
    {
        GuardRun leg("nochain", acc.config);
        leg.fx->ctx().counters.station_memory_absent = 1;
        networkResumeHoldGuardRelease(leg.fx->run());
        EXPECT_EQ(leg.held(), 3);
        EXPECT_EQ(leg.fx->ctx().counters.resume_hold_guard_why, 0);
    }
    {
        const Loaded frac = loadGuardConfig("frac", "", kChain);
        ASSERT_TRUE(frac.ok) << frac.text;
        GuardRun leg("frac", frac.config);
        leg.fx->ctx().counters.chain_restore_restored = 1;
        networkResumeHoldGuardRelease(leg.fx->run());
        EXPECT_EQ(leg.held(), 3);
    }
    {
        GuardRun leg("refused", acc.config);
        leg.fx->ctx().counters.chain_restore_restored = 1;
        leg.fx->ctx().counters.station_memory_refused = 1;
        networkResumeHoldGuardRelease(leg.fx->run());
        EXPECT_EQ(leg.held(), 0);
        EXPECT_EQ(leg.fx->ctx().counters.resume_hold_guard_why, 1);
    }
}

// ---- Step (b'): the first wind-up seeds of a resumed run without its accumulators ------------------------

namespace {

/// One accumulated wind-up call for @p sat at @p dt_s (a station at 40 N, a GPS satellite overhead, a fixed Sun).
double windUpCall(GnssCorrections& gc, const std::string& sat, double dt_s) {
    const Eigen::Vector3d rx = Coordinates::llhToEcef(40.0 * Constants::PI / 180.0, -105.0 * Constants::PI / 180.0, 1650.0);
    const Eigen::Vector3d sv = rx + 20200000.0 * rx.normalized() + Eigen::Vector3d(1.0e6, 2.0e6, 0.0);
    const Eigen::Vector3d sun = sv + 1.495978707e11 * Eigen::Vector3d(0.6, 0.8, 0.0);
    return gc.computePhaseWindUp(sat, System::GPS, rx, sv, Constants::GPS_L1_WAVELENGTH,
                                 GnssTime{2300, 100000.0 + dt_s}, sun, Eigen::Vector3d::Zero(), /*accumulate=*/true,
                                 /*max_gap_s=*/300.0);
}

}  // namespace

TEST(ResumeSeedRestart, AFirstSeedEntersTheRestartLedgerOnlyWhenSet) {
    LogManager log;
    GnssCorrections plain(log);
    windUpCall(plain, "G30", 0.0);
    EXPECT_TRUE(plain.wuRestarts().empty());   // a seed is silent by default
    GnssCorrections resumed(log);
    resumed.setSeedAsRestart(true);
    windUpCall(resumed, "G30", 0.0);
    windUpCall(resumed, "G07", 0.0);
    ASSERT_EQ(resumed.wuRestarts().size(), 2u);
    const GnssCorrections::WuRestart& r = resumed.wuRestarts().at("G30");
    EXPECT_TRUE(r.resume_seed);
    EXPECT_EQ(r.gap_s, 0.0);
    EXPECT_EQ(r.epoch_s, 2300 * 604800.0 + 100000.0);
    // Each satellite seeds once: the next epochs unwrap and add nothing.
    resumed.clearWuRestarts();
    windUpCall(resumed, "G30", 30.0);
    windUpCall(resumed, "G07", 30.0);
    EXPECT_TRUE(resumed.wuRestarts().empty());
}

TEST(ResumeSeedRestart, TheGuardSetsTheRuleOnlyWhenTheStationMemorySectionIsAbsentOrRefused) {
    const Loaded acc = loadGuardConfig("seed", kAccumulated, std::string(kChain) + kStationMemory);
    ASSERT_TRUE(acc.ok) << acc.text;
    const auto putCarries = [](GuardRun& r) {
        net::putArcAmb(r.fx->ctx().entities, 0, "G05_L1C", &net::ArcState::amb);
        net::putArcAmb(r.fx->ctx().entities, 1, "E11_L5Q", &net::ArcState::amb);
    };
    {
        GuardRun leg("seed_abs", acc.config);
        putCarries(leg);
        leg.fx->ctx().counters.station_memory_absent = 1;
        networkResumeSeedRestartArm(leg.fx->run());
        const auto& C = leg.fx->ctx().counters;
        EXPECT_EQ(C.resume_seed_why, 2);
        EXPECT_EQ(C.resume_seed_amb_carries, 2);
        EXPECT_EQ(C.resume_seed_restart_stations, 0);   // the test stations carry no model
        EXPECT_TRUE(contains(leg.text.str(), "the station memory section was absent"));
    }
    {
        GuardRun leg("seed_ref", acc.config);
        putCarries(leg);
        leg.fx->ctx().counters.station_memory_refused = 1;
        networkResumeSeedRestartArm(leg.fx->run());
        EXPECT_EQ(leg.fx->ctx().counters.resume_seed_why, 1);
    }
    {
        GuardRun leg("seed_ok", acc.config);
        putCarries(leg);
        leg.fx->ctx().counters.station_memory_restored = 1;
        networkResumeSeedRestartArm(leg.fx->run());
        EXPECT_EQ(leg.fx->ctx().counters.resume_seed_why, 0);
    }
    {
        GuardRun leg("seed_cold", acc.config);   // no resume: no section counted at all
        networkResumeSeedRestartArm(leg.fx->run());
        EXPECT_EQ(leg.fx->ctx().counters.resume_seed_why, 0);
    }
    {
        const Loaded frac = loadGuardConfig("seed_frac", "", kChain);
        ASSERT_TRUE(frac.ok) << frac.text;
        GuardRun leg("seed_frac", frac.config);
        leg.fx->ctx().counters.station_memory_absent = 1;
        networkResumeSeedRestartArm(leg.fx->run());
        EXPECT_EQ(leg.fx->ctx().counters.resume_seed_why, 0);
    }
}
