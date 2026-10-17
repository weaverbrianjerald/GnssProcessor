/**
 * @file test_realtime_defects.cpp
 * @brief Real-time engine defects, driven through the engine seams.
 *
 * Coverage:
 *   - Data gap: with `processing.data_gap_tolerance_s` at 0 the gap threshold is 1.5
 *     measured epoch intervals, so a 30 s stream is not a gap at every epoch (it used to
 *     read as one, which re-seeded the receiver clock from code every epoch).  A
 *     configured tolerance is still used as given.
 *   - Staged snapshot restore: an unknown dynamic state kind is refused before any state
 *     is restored, so the cold start that follows starts from an untouched manager.
 */

#include <gtest/gtest.h>

#include "realtime/RealTimeEngine.hpp"
#include "realtime/RtStateSnapshot.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine members these cases need.
struct RealtimeDefectTestAccess {
    static void attachStateManager(RealTimeEngine& e) {
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }

    /// One call of the time-step and gap seam at @p tow (week 2313); marks the first
    /// epoch done afterwards, as the epoch driver does.
    struct Step { bool advanced; double dt; bool gap; };
    static Step step(RealTimeEngine& e, double tow) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        ctx.current_time = GnssTime{2313, tow};
        ctx.epoch_prefix = "[RT] ";
        const bool advanced = e.stepTimeAndDetectGap(ctx);
        e.first_epoch_ = false;
        return Step{advanced, ctx.dt, ctx.gap_detected};
    }

    static uint64_t liveFingerprint(const RealTimeEngine& e) { return e.rtSnapshotFingerprint_(); }
    static void stage(RealTimeEngine& e, const RtStateSnapshot& snap) {
        e.staged_rt_snap_ = std::make_unique<RtStateSnapshot>(snap);
    }
    static void apply(RealTimeEngine& e, double tow) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        ctx.current_time = GnssTime{2313, tow};
        ctx.epoch_prefix = "[RT] ";
        e.applyStagedStateSnapshot_(ctx);
    }
    static bool firstEpoch(const RealTimeEngine& e) { return e.first_epoch_; }
    static uint64_t epochCount(const RealTimeEngine& e) { return e.epoch_count_; }
};

namespace {

class RealtimeDefectTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    std::unique_ptr<RealTimeEngine> engine;

    void build(const std::string& extra_processing = "", const std::string& realtime = "") {
        const auto path = gnss_test::tempDir() / "rt_defect.json";
        {
            std::ofstream out(path);
            out << R"({
                "processing": {
                    "mode": "ppp-rtk",
                    "active_systems": ["GPS"])" << extra_processing << R"(
                },
                "estimation": {
                    "estimate_velocity": false,
                    "estimate_phase_clock": false,
                    "estimate_ifb": false,
                    "estimate_sat_biases": false,
                    "ambiguity_resolution": {"mode": 0},
                    "between_satellite_sd": {"enabled": false}
                },
                "filter": {"initial_std": {
                    "position": 1.0, "clock": 1.0, "troposphere": 0.1,
                    "ionosphere": 1.0, "ambiguity": 100.0
                }})" << realtime << R"(
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        engine = std::make_unique<RealTimeEngine>(config, log);
        RealtimeDefectTestAccess::attachStateManager(*engine);
    }
};

// ---------------------------------------------------------------------------
// Data-gap threshold
// ---------------------------------------------------------------------------

TEST_F(RealtimeDefectTest, DefaultToleranceIsOnePointFiveMeasuredIntervals) {
    build();
    ASSERT_EQ(config.getDataGapTolerance(), 0.0);
    using A = RealtimeDefectTestAccess;

    auto first = A::step(*engine, 1000.0);
    EXPECT_TRUE(first.advanced);
    EXPECT_FALSE(first.gap);                 // the first epoch is never a gap

    // The second epoch's spacing is the first measurement: judged against the fixed
    // 1 s of the unmeasured stream, as before.
    auto second = A::step(*engine, 1030.0);
    EXPECT_DOUBLE_EQ(second.dt, 30.0);
    EXPECT_TRUE(second.gap);

    // From here the 30 s cadence is the nominal interval: no gap at every epoch.
    for (int k = 2; k < 12; ++k) {
        const auto s = A::step(*engine, 1000.0 + 30.0 * k);
        EXPECT_DOUBLE_EQ(s.dt, 30.0);
        EXPECT_FALSE(s.gap) << "epoch " << k;
    }

    // A missed epoch (60 s) exceeds 1.5 intervals and is a gap; the next 30 s is not.
    const auto missed = A::step(*engine, 1000.0 + 30.0 * 11 + 60.0);
    EXPECT_DOUBLE_EQ(missed.dt, 60.0);
    EXPECT_TRUE(missed.gap);
    const auto after = A::step(*engine, 1000.0 + 30.0 * 11 + 90.0);
    EXPECT_FALSE(after.gap);
}

TEST_F(RealtimeDefectTest, OneHertzStreamKeepsItsGapsAtOnePointFiveSeconds) {
    build();
    using A = RealtimeDefectTestAccess;
    A::step(*engine, 500.0);
    for (int k = 1; k < 20; ++k) EXPECT_FALSE(A::step(*engine, 500.0 + k).gap) << k;
    const double t = 519.0;
    EXPECT_FALSE(A::step(*engine, t + 1.4).gap);     // jitter inside 1.5 s
    EXPECT_TRUE(A::step(*engine, t + 1.4 + 2.0).gap);// a lost epoch
}

TEST_F(RealtimeDefectTest, ConfiguredToleranceIsUsedAsGiven) {
    build(R"(, "data_gap_tolerance_s": 10.0)");
    ASSERT_DOUBLE_EQ(config.getDataGapTolerance(), 10.0);
    using A = RealtimeDefectTestAccess;
    A::step(*engine, 2000.0);
    for (int k = 1; k < 8; ++k) {
        const auto s = A::step(*engine, 2000.0 + 30.0 * k);   // 30 s > 10 s: every epoch is a gap
        EXPECT_TRUE(s.gap) << k;
    }
}

TEST_F(RealtimeDefectTest, DuplicateEpochDoesNotEnterTheCadence) {
    build();
    using A = RealtimeDefectTestAccess;
    A::step(*engine, 3000.0);
    A::step(*engine, 3030.0);
    EXPECT_FALSE(A::step(*engine, 3030.0).advanced);   // retransmitted epoch, dt 0
    EXPECT_FALSE(A::step(*engine, 3030.0).advanced);
    const auto next = A::step(*engine, 3060.0);
    EXPECT_TRUE(next.advanced);
    EXPECT_FALSE(next.gap);
}

// ---------------------------------------------------------------------------
// Staged snapshot restore: validate before mutating
// ---------------------------------------------------------------------------

RtStateSnapshot makeSnapshot(const RealTimeEngine& engine, StateManager& sm,
                             const std::vector<RtStateSnapshot::DynState>& dyn_in) {
    RtStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 1000.0;
    s.wall_utc  = "2026-10-17T00:00:00Z";
    s.fingerprint = RealtimeDefectTestAccess::liveFingerprint(engine);
    s.core_size = sm.getStateSize();
    s.state_size = s.core_size + dyn_in.size();
    s.generation_id = 77;
    s.epoch_count = 123;
    s.dyn = dyn_in;
    const auto n = static_cast<Eigen::Index>(s.state_size);
    s.x = Eigen::VectorXd::Constant(n, 0.25);
    s.P = Eigen::MatrixXd::Identity(n, n) * 0.04;
    return s;
}

TEST_F(RealtimeDefectTest, UnknownSnapshotKindIsRefusedBeforeAnyStateIsRestored) {
    build("", R"(, "realtime": {"state_snapshot_path": "unused.txt",
                                "state_snapshot_restore_scope": "full"})");
    ASSERT_EQ(config.getRealTimeConfig().state_snapshot_restore_scope, "full");
    using A = RealtimeDefectTestAccess;
    StateManager& sm = A::states(*engine);
    const std::size_t core = sm.getStateSize();

    // A valid iono row first, then a kind written by a newer build.
    const auto snap = makeSnapshot(*engine, sm, { { core, "ION", "G01" },
                                                  { core + 1, "NEWKIND", "X1" } });
    A::stage(*engine, snap);
    A::apply(*engine, 1030.0);

    const auto& st = engine->stateSnapshotRestoreStatus();
    EXPECT_EQ(st.outcome, RealTimeEngine::RtSnapshotRestoreStatus::Outcome::Discarded);
    EXPECT_NE(st.reason.find("unknown dynamic state kind"), std::string::npos) << st.reason;

    // Nothing was restored: the cold start proceeds from the manager as it was.
    EXPECT_EQ(sm.getStateSize(), core);
    EXPECT_TRUE(sm.getIonosphereStates().empty());
    EXPECT_TRUE(sm.getAmbiguities().empty());
    EXPECT_TRUE(A::firstEpoch(*engine));
    EXPECT_EQ(A::epochCount(*engine), 0u);
}

TEST_F(RealtimeDefectTest, KnownKindsStillRestoreInFullScope) {
    build("", R"(, "realtime": {"state_snapshot_path": "unused.txt",
                                "state_snapshot_restore_scope": "full"})");
    using A = RealtimeDefectTestAccess;
    StateManager& sm = A::states(*engine);
    const std::size_t core = sm.getStateSize();

    const auto snap = makeSnapshot(*engine, sm, { { core, "ION", "G01" },
                                                  { core + 1, "AMB", "G01_L1C" } });
    A::stage(*engine, snap);
    A::apply(*engine, 1030.0);

    const auto& st = engine->stateSnapshotRestoreStatus();
    ASSERT_EQ(st.outcome, RealTimeEngine::RtSnapshotRestoreStatus::Outcome::Restored) << st.reason;
    EXPECT_EQ(st.scope, "full");
    EXPECT_EQ(sm.getStateSize(), core + 2);
    EXPECT_EQ(sm.getIonosphereStates().count("G01"), 1u);
    EXPECT_EQ(sm.getAmbiguities().count("G01_L1C"), 1u);
    EXPECT_FALSE(A::firstEpoch(*engine));
    EXPECT_EQ(A::epochCount(*engine), 123u);
}

} // namespace
} // namespace GnssProcessor
