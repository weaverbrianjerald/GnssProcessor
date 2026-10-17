/**
 * @file test_amb_level_arc.cpp
 * @brief ALC windows cannot span ambiguity resets hidden by the subsequent KF update.
 *
 * The fixture runs the real measurement builder (including pending-ambiguity
 * reseeding), Kalman update, and post-update ALC seam.  Synthetic modeled GPS
 * observations avoid product files while preserving the production ordering.
 */

#include <gtest/gtest.h>

#include "realtime/RealTimeEngine.hpp"
#include "filter/StateKeySchema.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seams this regression needs.
struct AmbLevelArcTestAccess {
    static void initialize(RealTimeEngine& e, const SsrCorrectionStore& store) {
        e.product_parser_ = std::make_unique<ProductParser>(e.config_, e.log_manager_);
        e.rinex_parser_ = std::make_unique<RinexParser>(
            e.config_, e.log_manager_, *e.product_parser_);
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
        e.ppp_model_ = std::make_unique<PppModel>(
            e.config_, *e.product_parser_, e.log_manager_, *e.rinex_parser_, *e.state_manager_);
        e.meas_model_ = std::make_unique<MeasurementModel>(
            *e.state_manager_, *e.ppp_model_, e.config_, e.log_manager_);
        e.meas_model_->setSsrStore(&store);
        e.kalman_filter_ = std::make_unique<KalmanFilter>(
            *e.state_manager_, e.config_, e.log_manager_, *e.ppp_model_);
        e.mw_slip_detector_ = std::make_unique<MwSlipDetector>(e.config_, e.log_manager_);

        auto x = e.state_manager_->getStateVector();
        x.segment<3>(e.state_manager_->getPositionIndex()) = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        e.state_manager_->setStateVector(x);
    }

    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }
    static const AmbLevelCheck& check(const RealTimeEngine& e) { return e.amb_level_check_; }

    static void step(RealTimeEngine& e, std::vector<PppObservation> observations) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        ctx.modeled_obs = std::move(observations);
        ctx.current_time = ctx.modeled_obs.front().time;
        e.runMeasurementUpdate(ctx);
        ASSERT_TRUE(ctx.update_applied);
        ASSERT_GT(ctx.meas_update.innovations.size(), 0);
        // A successful update genuinely contracted at least one phase state;
        // the original variance guard cannot detect an arc boundary this way.
        for (const auto& o : ctx.modeled_obs) {
            for (const auto& [signal, value] : o.modeled) {
                if (signal.empty() || signal[0] != 'L') continue;
                const auto key = StateKeys::makeAmbiguityKey(o.satellite, signal);
                const int idx = e.state_manager_->getAmbiguityIndex(key);
                ASSERT_GE(idx, 0);
                EXPECT_FALSE(e.state_manager_->isAmbiguityPendingReseed(key));
                EXPECT_LT(e.state_manager_->getStateVariance(idx),
                          std::pow(e.config_.getAmbiguityStd(), 2));
            }
        }
        e.checkAmbiguityLevel_(ctx);
    }

    static void noUpdate(RealTimeEngine& e) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        e.checkAmbiguityLevel_(ctx);
    }

    static void disturb(RealTimeEngine& e, const std::string& sat) {
        e.noteAmbLevelDisturbed_(sat);
    }
};

} // namespace GnssProcessor

using namespace GnssProcessor;

namespace {

constexpr int kWindow = 4;
constexpr const char* kSat = "G12";
constexpr const char* kAmb1 = "G12_L1W";
constexpr const char* kAmb2 = "G12_L2W";

class AmbLevelArcTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    SsrCorrectionStore store;
    std::unique_ptr<RealTimeEngine> engine;
    int epoch = 0;

    void SetUp() override {
        const auto path = gnss_test::tempDir()
            / (std::string("alc_arc_") + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {
                    "mode": "ppp-rtk",
                    "active_systems": ["GPS"],
                    "amb_level_check": true,
                    "amb_level_check_window_ep": 4,
                    "amb_level_check_mode": "observe"
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
                }}
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        ASSERT_TRUE(config.ambLevelCheck());
        ASSERT_TRUE(config.isUncombinedEnabled());
        ASSERT_EQ(config.getAmbLevelCheckWindowEp(), kWindow);
        engine = std::make_unique<RealTimeEngine>(config, log);
        AmbLevelArcTestAccess::initialize(*engine, store);
    }

    PppObservation observation(double l1_ambiguity = 12.0,
                               const std::string& l2_signal = "L2W",
                               bool paired_code = true) {
        PppObservation o{};
        o.satellite = kSat;
        o.system = System::GPS;
        o.time = GnssTime{2418, 1000.0 + 30.0 * epoch++};
        o.rx_pos_linearization = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        o.los = Eigen::Vector3d(1.0, 1.0, 1.0).normalized();
        o.range = 2.2e7;
        o.sat_pos = o.rx_pos_linearization + o.range * o.los;
        o.elevation = 45.0;
        o.azimuth = 45.0;
        o.mf_w = std::sqrt(2.0);
        o.mf_d = std::sqrt(2.0);
        const auto add = [&](const std::string& signal, double ambiguity) {
            o.modeled[signal] = o.range;
            o.observed[signal] = o.range + ambiguity;
            o.variances[signal] = signal[0] == 'L' ? 0.000025 : 0.25;
        };
        add("C1W", 0.0);
        add("L1W", l1_ambiguity);
        add(l2_signal, 7.0);
        if (paired_code) add("C" + l2_signal.substr(1), 0.0);
        return o;
    }

    void step(double ambiguity = 12.0) {
        AmbLevelArcTestAccess::step(*engine, {observation(ambiguity)});
    }
    void fillOldWindow() {
        for (int i = 0; i < kWindow - 1; ++i) step();
        ASSERT_EQ(check().depth(kSat), kWindow - 1u);
        ASSERT_EQ(check().counters().checks, 0u);
    }
    StateManager& states() { return AmbLevelArcTestAccess::states(*engine); }
    const AmbLevelCheck& check() { return AmbLevelArcTestAccess::check(*engine); }
};

TEST_F(AmbLevelArcTest, ResetReseedAndMeasurementUpdateCannotHideTheArcBoundary) {
    fillOldWindow();
    const auto old_generation = states().getAmbiguityGeneration(kAmb1);
    ASSERT_NE(old_generation, 0u);
    ASSERT_TRUE(states().resetAmbiguity(kAmb1, config.getAmbiguityStd()));
    ASSERT_TRUE(states().isAmbiguityPendingReseed(kAmb1));
    ASSERT_NE(states().getAmbiguityGeneration(kAmb1), old_generation);
    EXPECT_DOUBLE_EQ(states().getStateVariance(states().getAmbiguityIndex(kAmb1)),
                     std::pow(config.getAmbiguityStd(), 2));

    // No explicit engine disturbance notification: this is the slipped-arc
    // path the old post-update variance test missed.  The builder reseeds to
    // the new observation, then the actual KF contracts P below reset_var.
    step(15.0);
    EXPECT_NEAR(states().getStateVector()(states().getAmbiguityIndex(kAmb1)), 15.0, 1e-6);
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
    EXPECT_EQ(check().counters().restarts, 1u);
    for (int i = 1; i < kWindow - 1; ++i) {
        step(15.0);
        EXPECT_EQ(check().counters().checks, 0u);
    }
    step(15.0);
    EXPECT_EQ(check().depth(kSat), kWindow);
    EXPECT_EQ(check().counters().checks, 1u);
    EXPECT_EQ(check().counters().trips_wl + check().counters().trips_if, 0u);
}

TEST_F(AmbLevelArcTest, ResetSurvivesNoUpdateAndAnEpochMissingThePairedCode) {
    fillOldWindow();
    ASSERT_TRUE(states().resetAmbiguity(kAmb1, config.getAmbiguityStd()));
    AmbLevelArcTestAccess::noUpdate(*engine);
    EXPECT_EQ(check().counters().samples, kWindow - 1u);
    AmbLevelArcTestAccess::step(*engine, {observation(15.0, "L2W", false)});
    EXPECT_EQ(check().counters().samples, kWindow - 1u);
    EXPECT_FALSE(states().isAmbiguityPendingReseed(kAmb1));

    step(15.0);
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
    EXPECT_EQ(check().counters().restarts, 1u);
}

TEST_F(AmbLevelArcTest, PendingReseedWithoutVarianceInflationRestartsTheWindow) {
    fillOldWindow();
    states().markAmbiguityPendingReseed(kAmb1);
    EXPECT_LT(states().getStateVariance(states().getAmbiguityIndex(kAmb1)),
              std::pow(config.getAmbiguityStd(), 2));
    step(15.0);
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
    EXPECT_EQ(check().counters().restarts, 1u);
}

TEST_F(AmbLevelArcTest, RemovingAndRecreatingTheSameKeyStartsAFreshWindow) {
    fillOldWindow();
    const auto old_generation = states().getAmbiguityGeneration(kAmb1);
    ASSERT_TRUE(states().removeAmbiguity(kAmb1));
    EXPECT_EQ(states().getAmbiguityGeneration(kAmb1), 0u);
    step(15.0); // MeasurementModel adds the same key again, then KF contracts P.
    EXPECT_NE(states().getAmbiguityGeneration(kAmb1), old_generation);
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
}

TEST_F(AmbLevelArcTest, ChangingThePairedSignalStartsAFreshWindow) {
    fillOldWindow();
    AmbLevelArcTestAccess::step(*engine, {observation(12.0, "L2X")});
    EXPECT_GE(states().getAmbiguityIndex("G12_L2X"), 0);
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
    EXPECT_EQ(check().counters().restarts, 1u);
}

TEST_F(AmbLevelArcTest, OrdinaryUpdatesAndObservationGapsPreserveTheWindow) {
    fillOldWindow();
    const auto generation = states().getAmbiguityGeneration(kAmb1);
    AmbLevelArcTestAccess::noUpdate(*engine);
    AmbLevelArcTestAccess::step(*engine, {observation(12.0, "L2W", false)});
    EXPECT_EQ(check().depth(kSat), kWindow - 1u);
    EXPECT_EQ(states().getAmbiguityGeneration(kAmb1), generation);
    step();
    EXPECT_EQ(check().depth(kSat), kWindow);
    EXPECT_EQ(check().counters().checks, 1u);
    EXPECT_EQ(check().counters().restarts, 0u);
}

TEST_F(AmbLevelArcTest, ExplicitDisturbanceRestartsImmediatelyEvenWithoutAnUpdate) {
    fillOldWindow();
    AmbLevelArcTestAccess::disturb(*engine, kSat);
    EXPECT_EQ(check().depth(kSat), 0u);
    AmbLevelArcTestAccess::noUpdate(*engine);
    step();
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
    EXPECT_EQ(check().counters().restarts, 1u);
}

TEST_F(AmbLevelArcTest, ValueTranslationRestartsButZeroTranslationDoesNot) {
    fillOldWindow();
    const auto generation = states().getAmbiguityGeneration(kAmb1);
    ASSERT_TRUE(states().shiftAmbiguity(kAmb1, 0.0));
    EXPECT_EQ(states().getAmbiguityGeneration(kAmb1), generation);
    ASSERT_TRUE(states().shiftAmbiguity(kAmb1, 3.0));
    EXPECT_NE(states().getAmbiguityGeneration(kAmb1), generation);
    step(15.0);
    EXPECT_EQ(check().depth(kSat), 1u);
    EXPECT_EQ(check().counters().checks, 0u);
    EXPECT_EQ(check().counters().restarts, 1u);
}

} // namespace
