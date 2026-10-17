/**
 * @file test_innovation_screen_engine.cpp
 * @brief The consumer's pre-update innovation screen on the engine side: the config
 *        knobs and their closed-loop derivation, the innovation covariance built from
 *        the state manager's predicted covariance, and the decisions applied to the
 *        exclusion set and the ambiguity states.
 *
 * The measurement is built by hand (design matrix, noise, labels) over a real
 * StateManager, so the covariance the screen reads is the manager's own. The seam that
 * builds the measurement through the measurement model is exercised by the end-to-end
 * replays; here the knob-off contract (nothing built, nothing counted) is pinned.
 */

#include <gtest/gtest.h>

#include "realtime/RealTimeEngine.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seams this suite needs.
struct InnovationScreenTestAccess {
    static void initialize(RealTimeEngine& e) {
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }
    static void configure(RealTimeEngine& e) { e.configureInnovationScreen_(); }
    static InnovationScreen::Outcome evaluate(RealTimeEngine& e, const EpochMeasurement& mu) {
        e.configureInnovationScreen_();
        return e.evaluateInnovationScreen_(mu);
    }
    static void apply(RealTimeEngine& e, const InnovationScreen::Outcome& out,
                      std::set<std::string>& excluded) {
        e.applyInnovationScreen_(out, excluded);
    }
    /// The whole seam on an empty epoch: its return value, the measurement size it left
    /// in the context and the exclusion set.
    static bool screenEmptyEpoch(RealTimeEngine& e, std::set<std::string>& excluded,
                                 Eigen::Index& n_innovations) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{ raw };
        const bool built = e.screenInnovations_(ctx, excluded);
        n_innovations = ctx.meas_update.innovations.size();
        return built;
    }
};

namespace {

constexpr int kSats = 8;

/// Loads a processing-only config with the screen knobs spliced in.
class InnovationScreenEngineTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    std::unique_ptr<RealTimeEngine> engine;
    std::mt19937 rng{ 17 };

    void build(const std::string& screen_keys) {
        const auto path = gnss_test::tempDir() / "innovation_screen.json";
        {
            std::ofstream out(path);
            out << R"({
                "processing": {
                    "mode": "ppp-rtk",
                    "active_systems": ["GPS"],)" << screen_keys << R"(
                    "enable_ssr_bias_max_age": true
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
                    "position": 10.0, "clock": 100.0, "troposphere": 0.1,
                    "ionosphere": 1.0, "ambiguity": 100.0
                }}
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        engine = std::make_unique<RealTimeEngine>(config, log);
        InnovationScreenTestAccess::initialize(*engine);
        for (int s = 0; s < kSats; ++s)
            states().addAmbiguity(ambKey(s), 0.0, 0.02);
        // The predicted covariance of a consumer in steady state: a clock that moves
        // by hundreds of metres between epochs, loose position, tight ambiguities.
        Eigen::MatrixXd P = Eigen::MatrixXd::Zero(states().getStateSize(), states().getStateSize());
        P(states().getClockIndex(), states().getClockIndex()) = 30000.0;
        for (int i = 0; i < 3; ++i) P(i, i) = 100.0;
        const size_t tr = states().getTropoIndex();
        P(tr, tr) = 0.01;
        for (int s = 0; s < kSats; ++s) {
            const int i = states().getAmbiguityIndex(ambKey(s));
            P(i, i) = 4.0e-4;
        }
        states().setCovariance(P);
    }

    StateManager& states() { return InnovationScreenTestAccess::states(*engine); }

    static std::string satName(int s) {
        char b[8];
        std::snprintf(b, sizeof(b), "G%02d", s + 1);
        return b;
    }
    static std::string ambKey(int s) { return satName(s) + "_L1W"; }

    /// A measurement of kSats satellites, one code and one phase row each, with
    /// innovations drawn from the model's own covariance.
    EpochMeasurement measurement(bool differenced_labels = false) {
        const size_t n = states().getStateSize();
        const Eigen::Index m = 2 * kSats;
        EpochMeasurement mu;
        mu.H = Eigen::MatrixXd::Zero(m, static_cast<Eigen::Index>(n));
        mu.R_v.resize(m);
        for (int s = 0; s < kSats; ++s) {
            const double az = 6.283185307 * s / kSats;
            const double el = 0.35 + 1.0 * ((s * 3) % kSats) / kSats;
            const Eigen::Vector3d u(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az),
                                    std::sin(el));
            for (int k = 0; k < 2; ++k) {
                const Eigen::Index r = 2 * s + k;
                mu.H.block(r, 0, 1, 3) = u.transpose();
                mu.H(r, static_cast<Eigen::Index>(states().getClockIndex())) = 1.0;
                if (k == 1) mu.H(r, states().getAmbiguityIndex(ambKey(s))) = 1.0;
                mu.R_v(r) = (k == 0) ? 0.25 : 9.0e-6;
                mu.satellite_ids.push_back(differenced_labels ? satName(s) + "-G01" : satName(s));
                mu.signal_types.push_back(k == 0 ? "C1W" : "L1W");
            }
        }
        const Eigen::MatrixXd P = states().getCovariance();
        Eigen::MatrixXd S = mu.H * P * mu.H.transpose();
        S.diagonal() += mu.R_v;
        Eigen::LLT<Eigen::MatrixXd> llt(S);
        std::normal_distribution<double> nd(0.0, 1.0);
        Eigen::VectorXd z(m);
        for (Eigen::Index i = 0; i < m; ++i) z(i) = nd(rng);
        mu.innovations = llt.matrixL() * z;
        return mu;
    }

    static Eigen::Index phaseRow(int s) { return 2 * s + 1; }
    static Eigen::Index codeRow(int s) { return 2 * s; }

    double ambVar(int s) {
        const int i = states().getAmbiguityIndex(ambKey(s));
        return states().getCovariance()(i, i);
    }
};

// The knob off: the seam builds nothing and counts nothing, so the update loop runs
// exactly as it did before the screen existed.
TEST_F(InnovationScreenEngineTest, KnobOffBuildsNothingAndCountsNothing) {
    build("");
    EXPECT_FALSE(config.innovationScreen());
    EXPECT_FALSE(engine->innovationScreenArmed());
    std::set<std::string> excluded;
    Eigen::Index n_innovations = -1;
    EXPECT_FALSE(InnovationScreenTestAccess::screenEmptyEpoch(*engine, excluded, n_innovations));
    EXPECT_EQ(n_innovations, 0);
    EXPECT_TRUE(excluded.empty());
    const auto& c = engine->innovationScreenCounters();
    EXPECT_EQ(c.epochs, 0u);
    EXPECT_EQ(c.skipped, 0u);
}

TEST_F(InnovationScreenEngineTest, DefaultsOfTheKnobs) {
    build("\"innovation_screen\": true,");
    EXPECT_TRUE(config.innovationScreen());
    EXPECT_DOUBLE_EQ(config.getInnovationScreenK(), 6.0);
    EXPECT_EQ(config.getInnovationScreenMode(), "act");
    EXPECT_EQ(config.getInnovationScreenRows(), "all");
    EXPECT_DOUBLE_EQ(config.getInnovationScreenMaxFraction(), 0.25);
    EXPECT_EQ(config.getInnovationScreenRefloatEp(), 3);
    EXPECT_DOUBLE_EQ(config.getInnovationScreenPhaseFloorM(), 0.3);
    EXPECT_DOUBLE_EQ(config.getInnovationScreenCodeFloorM(), 1.0);
}

TEST_F(InnovationScreenEngineTest, OutOfRangeKnobsFallBackToTheDefaults) {
    build("\"innovation_screen\": true, \"innovation_screen_k\": -2.0, "
          "\"innovation_screen_mode\": \"sometimes\", \"innovation_screen_rows\": \"all-of-it\", "
          "\"innovation_screen_max_fraction\": 3.0, \"innovation_screen_refloat_ep\": 0, "
          "\"innovation_screen_phase_floor_m\": -1.0, \"innovation_screen_code_floor_m\": -1.0,");
    EXPECT_DOUBLE_EQ(config.getInnovationScreenK(), 6.0);
    EXPECT_EQ(config.getInnovationScreenMode(), "act");
    EXPECT_EQ(config.getInnovationScreenRows(), "all");
    EXPECT_DOUBLE_EQ(config.getInnovationScreenMaxFraction(), 0.25);
    EXPECT_EQ(config.getInnovationScreenRefloatEp(), 3);
    EXPECT_DOUBLE_EQ(config.getInnovationScreenPhaseFloorM(), 0.3);
    EXPECT_DOUBLE_EQ(config.getInnovationScreenCodeFloorM(), 1.0);
}

// The formal phase noise is millimetres; a healthy consumer's innovations spread over
// decimetres. Without the model-error floor a 0.35 m deviation of one phase row is
// "gross"; with the default floor it is not, and a metre-level one still is.
TEST_F(InnovationScreenEngineTest, WithoutTheFloorADecimetreDeviationIsFlagged) {
    build("\"innovation_screen\": true, \"innovation_screen_phase_floor_m\": 0.0, "
          "\"innovation_screen_code_floor_m\": 0.0,");
    EpochMeasurement mu = measurement();
    mu.innovations(phaseRow(4)) += 0.35;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0].sat, "G05");
}

TEST_F(InnovationScreenEngineTest, TheDefaultFloorKeepsADecimetreDeviationQuietAndStillFindsAMetre) {
    build("\"innovation_screen\": true,");
    {
        EpochMeasurement mu = measurement();
        mu.innovations(phaseRow(4)) += 0.35;
        EXPECT_TRUE(InnovationScreenTestAccess::evaluate(*engine, mu).events.empty());
    }
    {
        EpochMeasurement mu = measurement();
        mu.innovations(phaseRow(4)) += 4.0;
        const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
        ASSERT_EQ(out.events.size(), 1u);
        EXPECT_EQ(out.events[0].sat, "G05");
    }
}

// The 92 m phase step on a tight ambiguity, in a consumer whose clock alone is 173 m wide.
TEST_F(InnovationScreenEngineTest, AGrossPhaseRowIsHeldOutAndNamedByItsKey) {
    build("\"innovation_screen\": true,");
    EpochMeasurement mu = measurement();
    mu.innovations(phaseRow(4)) += 92.409;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0].kind, InnovationScreen::EventKind::Exclude);
    EXPECT_EQ(out.events[0].sat, "G05");
    EXPECT_EQ(out.events[0].signal, "L1W");
    EXPECT_GT(std::fabs(out.events[0].w), 100.0);

    std::set<std::string> excluded;
    InnovationScreenTestAccess::apply(*engine, out, excluded);
    EXPECT_EQ(excluded, std::set<std::string>{ "G05_L1W" });
    EXPECT_NEAR(ambVar(4), 4.0e-4, 1e-12) << "an exclusion does not touch the ambiguity";
}

// The screen reads the manager's predicted covariance: the same innovation on a loose
// ambiguity is not an error, because the state has no prior to contradict.
TEST_F(InnovationScreenEngineTest, ALooseAmbiguityCarriesNoPriorToContradict) {
    build("\"innovation_screen\": true,");
    Eigen::MatrixXd P = states().getCovariance();
    const int i = states().getAmbiguityIndex(ambKey(4));
    P(i, i) = 1.0e4;
    states().setCovariance(P);
    EpochMeasurement mu = measurement();
    mu.innovations(phaseRow(4)) += 92.409;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    EXPECT_TRUE(out.events.empty());
    EXPECT_FALSE(out.changesUpdate());
}

// A persistent step is re-floated on the third consecutive epoch, through the reset path,
// and is no longer held out.
TEST_F(InnovationScreenEngineTest, APersistentPhaseStepIsRefloatedAndReturnsToTheUpdate) {
    build("\"innovation_screen\": true,");
    const double reset_var = config.getAmbiguityStd() * config.getAmbiguityStd();
    std::set<std::string> excluded;
    for (int e = 0; e < 2; ++e) {
        EpochMeasurement mu = measurement();
        mu.innovations(phaseRow(4)) += 92.409;
        InnovationScreenTestAccess::apply(*engine,
            InnovationScreenTestAccess::evaluate(*engine, mu), excluded);
        EXPECT_EQ(excluded.count("G05_L1W"), 1u);
        EXPECT_NEAR(ambVar(4), 4.0e-4, 1e-12);
    }
    excluded.clear();   // the loop's exclusion set is per epoch
    EpochMeasurement mu = measurement();
    mu.innovations(phaseRow(4)) += 92.409;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    ASSERT_EQ(out.refloat.size(), 1u);
    InnovationScreenTestAccess::apply(*engine, out, excluded);
    EXPECT_TRUE(excluded.empty()) << "the re-floated row returns to the update";
    EXPECT_NEAR(ambVar(4), reset_var, 1e-6 * reset_var);
    EXPECT_TRUE(states().isAmbiguityPendingReseed(ambKey(4)))
        << "the ambiguity is re-seeded when the measurement is built";
    const auto& c = engine->innovationScreenCounters();
    EXPECT_EQ(c.refloated, 1u);
    EXPECT_EQ(c.episodes, 1u);
    EXPECT_TRUE(c.closureHolds());
}

// Observe mode: the same events and counters, and no write anywhere.
TEST_F(InnovationScreenEngineTest, ObserveModeCountsAndWritesNothing) {
    build("\"innovation_screen\": true, \"innovation_screen_mode\": \"observe\", "
          "\"innovation_screen_refloat_ep\": 1,");
    EpochMeasurement mu = measurement();
    mu.innovations(phaseRow(4)) += 92.409;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    EXPECT_FALSE(out.acted);
    EXPECT_FALSE(out.changesUpdate());
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0].kind, InnovationScreen::EventKind::Refloat);
    std::set<std::string> excluded;
    InnovationScreenTestAccess::apply(*engine, out, excluded);
    EXPECT_TRUE(excluded.empty());
    EXPECT_NEAR(ambVar(4), 4.0e-4, 1e-12);
    EXPECT_FALSE(states().isAmbiguityPendingReseed(ambKey(4)));
    EXPECT_EQ(engine->innovationScreenCounters().flagged, 1u);
}

// The row class: the other kind takes no part in the screen.
TEST_F(InnovationScreenEngineTest, ThePhaseClassIgnoresAGrossCodeRow) {
    build("\"innovation_screen\": true, \"innovation_screen_rows\": \"phase\",");
    EpochMeasurement mu = measurement();
    mu.innovations(codeRow(2)) += 90.0;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    EXPECT_TRUE(out.events.empty());
    EXPECT_EQ(engine->innovationScreenCounters().rows, static_cast<uint64_t>(kSats));
}

TEST_F(InnovationScreenEngineTest, TheCodeClassFindsAGrossCodeRow) {
    build("\"innovation_screen\": true, \"innovation_screen_rows\": \"code\",");
    EpochMeasurement mu = measurement();
    mu.innovations(codeRow(2)) += 90.0;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0].signal, "C1W");
    std::set<std::string> excluded;
    InnovationScreenTestAccess::apply(*engine, out, excluded);
    EXPECT_EQ(excluded, std::set<std::string>{ "G03_C1W" });
}

// Differenced rows ("<sat>-<pivot>") are not a single-satellite quantity; the screen
// leaves a differenced consumer alone and counts the calls it skipped.
TEST_F(InnovationScreenEngineTest, DifferencedRowsAreNeverScreened) {
    build("\"innovation_screen\": true,");
    EpochMeasurement mu = measurement(/*differenced_labels=*/true);
    mu.innovations(phaseRow(4)) += 92.409;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    EXPECT_TRUE(out.events.empty());
    EXPECT_EQ(engine->innovationScreenCounters().skipped, 1u);
    EXPECT_EQ(engine->innovationScreenCounters().epochs, 0u);
}

// A dense noise matrix (as a differenced build supplies) is honoured in S.
TEST_F(InnovationScreenEngineTest, ADenseNoiseMatrixIsUsedInTheInnovationCovariance) {
    build("\"innovation_screen\": true,");
    EpochMeasurement mu = measurement();
    // Inflate the phase noise of G05 enormously through R_full: the same 92 m is then
    // inside its own measurement noise and must not be flagged.
    mu.R_full = Eigen::MatrixXd(mu.R_v.asDiagonal());
    mu.R_full(phaseRow(4), phaseRow(4)) = 1.0e6;
    mu.innovations(phaseRow(4)) += 92.409;
    const auto out = InnovationScreenTestAccess::evaluate(*engine, mu);
    EXPECT_TRUE(out.events.empty());
}

// The closed-loop harness key and its five parameters reach the derived consumer; an
// out-of-range parameter falls back; the base config's own processing key is kept.
TEST(InnovationScreenConfig, TheClosedLoopKeyIsDerivedIntoTheConsumer) {
    LogManager log;
    const auto path = gnss_test::tempDir() / "innovation_screen_cl.json";
    {
        std::ofstream f(path);
        f << R"({
          "processing": { "mode": "ppp-rtk-network", "active_systems": ["GPS"],
                          "signal_priorities": { "GPS": "WC" } },
          "network": {
            "clock_datum_station": "AAAA",
            "closed_loop": { "innovation_screen": true, "innovation_screen_k": 5.0,
                             "innovation_screen_mode": "observe",
                             "innovation_screen_rows": "phase",
                             "innovation_screen_max_fraction": 0.4,
                             "innovation_screen_refloat_ep": 5,
                             "innovation_screen_phase_floor_m": 0.2,
                             "innovation_screen_code_floor_m": 2.5 },
            "stations": [
              { "name": "AAAA", "position_ecef": [ -2493304.99, -4655214.79, 3565497.67 ] },
              { "name": "BBBB", "position_ecef": [ -2281621.75, -1453585.08, 5756964.89 ] }
            ]
          }
        })";
    }
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, log));
    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_FALSE(cfg.innovationScreen()) << "the producer's own config is untouched";
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_TRUE(consumer.innovationScreen());
    EXPECT_DOUBLE_EQ(consumer.getInnovationScreenK(), 5.0);
    EXPECT_EQ(consumer.getInnovationScreenMode(), "observe");
    EXPECT_EQ(consumer.getInnovationScreenRows(), "phase");
    EXPECT_DOUBLE_EQ(consumer.getInnovationScreenMaxFraction(), 0.4);
    EXPECT_EQ(consumer.getInnovationScreenRefloatEp(), 5);
    EXPECT_DOUBLE_EQ(consumer.getInnovationScreenPhaseFloorM(), 0.2);
    EXPECT_DOUBLE_EQ(consumer.getInnovationScreenCodeFloorM(), 2.5);
}

TEST(InnovationScreenConfig, WithoutTheKeyTheDerivedConsumerIsUnchanged) {
    LogManager log;
    const auto path = gnss_test::tempDir() / "innovation_screen_cl_off.json";
    {
        std::ofstream f(path);
        f << R"({
          "processing": { "mode": "ppp-rtk-network", "active_systems": ["GPS"],
                          "signal_priorities": { "GPS": "WC" } },
          "network": {
            "clock_datum_station": "AAAA",
            "stations": [
              { "name": "AAAA", "position_ecef": [ -2493304.99, -4655214.79, 3565497.67 ] },
              { "name": "BBBB", "position_ecef": [ -2281621.75, -1453585.08, 5756964.89 ] }
            ]
          }
        })";
    }
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, log));
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(consumer.innovationScreen());
    EXPECT_DOUBLE_EQ(consumer.getInnovationScreenK(), 6.0);
    EXPECT_EQ(consumer.getInnovationScreenRefloatEp(), 3);
}

} // namespace
} // namespace GnssProcessor
