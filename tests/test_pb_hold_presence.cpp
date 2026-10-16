/**
 * @file test_pb_hold_presence.cpp
 * @brief Presence edges of a held phase bias at the consumer.
 *
 * Under the producer's hold the hold seam claims every key whose store
 * entry carries a generation, and the three phase-bias reactions used to skip a
 * claimed key entirely.  A held bias that appears on a phase row the consumer is
 * already using, or leaves it, steps the applied correction by the whole bias; the
 * field wire does this at the L1W family (G32 L1W appearing at 92.4 m), and with no
 * reaction the consumer's Kalman peel either catches the row or resets the wrong
 * satellites.  These cases drive the real reaction seam (RealTimeEngine::
 * resetOnPhaseBiasDisc, which runs the hold seam first) across both edges, with the
 * knob off (the card-PBH dispatch, nothing answered) and on (both edges answered).
 */

#include <gtest/gtest.h>

#include "realtime/RealTimeEngine.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seams this card needs.
struct PbHoldPresenceTestAccess {
    static void initialize(RealTimeEngine& e, const SsrCorrectionStore& store) {
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
        e.ssr_store_override_ = &store;
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }
    /// One epoch of the phase-bias reaction (the hold seam, then the configured one).
    static void react(RealTimeEngine& e, const PppObservation& obs) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        ctx.modeled_obs = {obs};
        ctx.current_time = obs.time;
        e.resetOnPhaseBiasDisc(ctx);
    }
};

namespace {

constexpr const char* kSat = "G32";
constexpr double kHeldBias = 92.409344;   // the field value of G32 L1W

class PbHoldPresenceTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    SsrCorrectionStore store;
    std::unique_ptr<RealTimeEngine> engine;
    int epoch = 0;

    void build(bool presence_flip, bool carry = false) {
        const auto path = std::filesystem::temp_directory_path()
            / (std::string("pbf_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()
               + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {
                    "mode": "ppp-rtk",
                    "active_systems": ["GPS"],
                    "enable_ssr_bias_max_age": true,
                    "ssr_bias_max_age_s": 60.0,
                    "ssr_phase_bias_disc_carry": )" << (carry ? "true" : "false") << R"(,
                    "ssr_pb_hold_presence_flip": )" << (presence_flip ? "true" : "false") << R"(
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
        ASSERT_TRUE(config.enableSsrBiasMaxAge());
        ASSERT_EQ(config.ssrPbHoldPresenceFlip(), presence_flip);
        engine = std::make_unique<RealTimeEngine>(config, log);
        PbHoldPresenceTestAccess::initialize(*engine, store);
        // A converged L1W ambiguity that absorbed the missing bias, and its L2W twin.
        states().addAmbiguity(std::string(kSat) + "_L1W", -33.56, 0.1);
        states().addAmbiguity(std::string(kSat) + "_L2W", 76.75, 0.1);
    }

    StateManager& states() { return PbHoldPresenceTestAccess::states(*engine); }

    GnssTime now() const { return GnssTime{2313, 261600.0 + 30.0 * epoch}; }

    /// Push this epoch's phase-bias record for kSat: L2W always, unheld; L1W only
    /// when @p l1w_present, held at generation @p gen.
    void push(bool l1w_present, std::uint32_t gen = 1) {
        SsrPhaseBias rec;
        rec.sat = kSat;
        rec.received_time = now();   // the staleness gate reads the record's own stamp
        SsrPhaseBiasEntry l2;
        l2.bias_m = -0.914672;
        rec.bias_per_signal["L2W"] = l2;
        if (l1w_present) {
            SsrPhaseBiasEntry l1;
            l1.bias_m = kHeldBias;
            l1.hold_generation = gen;
            rec.bias_per_signal["L1W"] = l1;
        }
        SsrHeader h;
        h.epoch_time = now();
        h.system = System::GPS;
        store.pushPhaseBias(rec, h);
    }

    /// One consumer epoch: the record, then the reaction seam.
    void step(bool l1w_present, std::uint32_t gen = 1) {
        push(l1w_present, gen);
        PppObservation o{};
        o.satellite = kSat;
        o.system = System::GPS;
        o.time = now();
        o.modeled["L1W"] = 2.2e7;
        o.modeled["L2W"] = 2.2e7;
        PbHoldPresenceTestAccess::react(*engine, o);
        ++epoch;
    }

    double var(const std::string& sig) {
        const int i = states().getAmbiguityIndex(std::string(kSat) + "_" + sig);
        EXPECT_GE(i, 0);
        return states().getCovariance()(i, i);
    }
    double value(const std::string& sig) {
        const int i = states().getAmbiguityIndex(std::string(kSat) + "_" + sig);
        EXPECT_GE(i, 0);
        return states().getStateVector()(i);
    }
    /// Put the L1W ambiguity back to a converged variance (between two edges).
    void reconverge() {
        const int i = states().getAmbiguityIndex(std::string(kSat) + "_L1W");
        Eigen::MatrixXd P = states().getCovariance();
        P.row(i).setZero();
        P.col(i).setZero();
        P(i, i) = 0.01;
        states().setCovariance(P);
    }
};

// The card-PBH dispatch: a claimed key skips every reaction, so neither the
// appearance nor the departure of the held bias touches the ambiguity.
TEST_F(PbHoldPresenceTest, KnobOffAnswersNeitherEdge) {
    build(/*presence_flip=*/false);
    for (int i = 0; i < 3; ++i) step(false);   // L1W used without a bias
    step(true);                                // the held bias appears: a 92 m step
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12) << "the card-PBH dispatch leaves the edge unanswered";
    for (int i = 0; i < 2; ++i) step(true);
    step(false);                               // and leaves
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12);
    EXPECT_NEAR(var("L2W"), 0.01, 1e-12);
    const auto& c = engine->pbHoldPresenceCounters();
    EXPECT_EQ(c.appear + c.vanish, 0u) << "the counters stay silent with the knob off";
}

// Armed, both edges take the A5 reset, on the flipped signal only.
TEST_F(PbHoldPresenceTest, KnobOnResetsTheFlippedSignalAtBothEdges) {
    build(/*presence_flip=*/true);
    const double reset_var = config.getAmbiguityStd() * config.getAmbiguityStd();
    for (int i = 0; i < 3; ++i) step(false);
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12) << "no edge yet: nothing is reset";
    step(true);                                             // appear
    EXPECT_NEAR(var("L1W"), reset_var, 1e-6 * reset_var) << "the appearance re-floats L1W";
    EXPECT_NEAR(var("L2W"), 0.01, 1e-12) << "the unflipped L2W keeps its convergence";
    reconverge();
    for (int i = 0; i < 2; ++i) step(true);                 // held, constant: no edge
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12) << "a held bias inside its generation is no edge";
    step(false);                                            // vanish
    EXPECT_NEAR(var("L1W"), reset_var, 1e-6 * reset_var) << "the departure re-floats L1W";
    const auto& c = engine->pbHoldPresenceCounters();
    EXPECT_EQ(c.appear, 1u);
    EXPECT_EQ(c.vanish, 1u);
    EXPECT_EQ(c.suppressed, 0u);
}

// Armed with the card-CC carry, the edge translates the ambiguity by the applied
// step instead of resetting it: amb - 92.4 at the appearance, back at the departure.
TEST_F(PbHoldPresenceTest, KnobOnWithCarryTranslatesByTheWholeBias) {
    build(/*presence_flip=*/true, /*carry=*/true);
    for (int i = 0; i < 3; ++i) step(false);
    const double before = value("L1W");
    step(true);
    EXPECT_NEAR(value("L1W"), before - kHeldBias, 1e-9) << "the carry absorbs the applied step";
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12) << "and keeps the convergence";
    step(true);
    step(false);
    EXPECT_NEAR(value("L1W"), before, 1e-9) << "the departure carries it back";
    const auto& c = engine->pbHoldPresenceCounters();
    EXPECT_EQ(c.appear, 1u);
    EXPECT_EQ(c.vanish, 1u);
}

// The hold's own reaction is unchanged with the knob on: a generation change
// re-floats once through the hold seam and is not answered a second time downstream.
TEST_F(PbHoldPresenceTest, KnobOnLeavesTheGenerationRefloatToTheHold) {
    build(/*presence_flip=*/true);
    const double reset_var = config.getAmbiguityStd() * config.getAmbiguityStd();
    step(true, 1);                       // first sighting on the first epoch: no prior row
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12) << "a first sighting of a new row is no edge";
    step(true, 1);
    EXPECT_NEAR(var("L1W"), 0.01, 1e-12);
    step(true, 2);                       // the producer re-levelled the key
    EXPECT_NEAR(var("L1W"), reset_var, 1e-6 * reset_var) << "the hold seam re-floats it";
    const auto& c = engine->pbHoldPresenceCounters();
    EXPECT_EQ(c.appear + c.vanish, 0u) << "a generation change is not a presence edge";
    EXPECT_EQ(engine->pbHoldCounters().refloats, 1u);
}

} // namespace
} // namespace GnssProcessor
