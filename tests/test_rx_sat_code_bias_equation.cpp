/**
 * @file test_rx_sat_code_bias_equation.cpp
 * @brief The receiver-satellite code-bias term of the observation equation, and the birth of the
 *        states it reads.
 *
 * Built through the single-receiver model over synthetic observations (support/ModelStack.hpp), with
 * the published satellite bias supplied by an SSR record, which is how a signal counts as
 * covered in a unit test.  The term is +1 on an uncombined raw code row when the column exists.
 * Nothing is added on a phase row or an iono-free pair, with the key off, for a constellation
 * outside the configured list, or for a signal whose published bias is missing (that pair keeps
 * its satellite code-bias state).
 */

#include <gtest/gtest.h>

#include "common/ParamRef.hpp"
#include "filter/RxSatCodeBiasStore.hpp"
#include "filter/StateKeySchema.hpp"
#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquation.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "support/ModelStack.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace K = GnssProcessor::StateKeys;

namespace {

constexpr const char* kKeyOn = R"(, "estimate_rx_sat_code_bias": true)";

class RxSatCodeBiasEquationTest : public ::testing::Test {
protected:
    LogManager log;
    gnss_test::ModelStack stack;
    int epoch = 0;

    StateManager& states() { return *stack.states; }

    void load(const std::string& estimation, const std::string& mode = "ppp-rtk",
              const std::string& processing = "") {
        ASSERT_TRUE(stack.load(log, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                               mode, processing, estimation, false));
    }

    PppObservation observation(const std::string& sat, System sys,
                               const std::vector<std::string>& signals) {
        return gnss_test::syntheticObservation(sat, sys, signals, epoch);
    }

    /// The published satellite code bias of the listed signals, as an SSR record.
    void cover(const std::string& sat, const std::vector<std::string>& signals) {
        SsrHeader header;
        SsrCodeBias cb;
        cb.sat = sat;
        for (const std::string& s : signals) cb.bias_per_signal[s] = 0.1;
        cb.received_time = GnssTime{ 2418, 1000.0 };
        stack.store.pushCodeBias(cb, header);
    }

    EpochMeasurement update(const std::vector<PppObservation>& obs) {
        ++epoch;
        return stack.model->computeMeasurementUpdate(obs, states().getStateVector());
    }

    static const ObsAddend* term(const ObservationEquation& eq, ObsEffect effect) {
        return findTerm(eq, effect);
    }
};

}  // namespace

TEST_F(RxSatCodeBiasEquationTest, ACoveredUncombinedGpsCodeRowHasThePlusOneTermOnItsOwnColumn) {
    load(kKeyOn);
    cover("G12", { "C1W", "C2W" });
    const PppObservation o = observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" });
    update({ o });

    const int c1 = states().getRxSatCodeBiasIndex("G12|C1W");
    const int c2 = states().getRxSatCodeBiasIndex("G12|C2W");
    ASSERT_GE(c1, 0);
    ASSERT_GE(c2, 0);
    EXPECT_NE(c1, c2);
    EXPECT_EQ(states().getRxSatCodeBiasStates().size(), 2u);

    // Give the states values, so that the term's value is visible.
    Eigen::VectorXd x = states().getStateVector();
    x(c1) = 0.37;
    x(c2) = -0.21;
    states().setStateVector(x);

    const ObservationEquation eq = stack.equation(log, o, "C1W");
    ASSERT_TRUE(eq.usable);
    const ObsAddend* t = term(eq, ObsEffect::ReceiverSatelliteCodeBias);
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->n_part, 1);
    EXPECT_EQ(t->part[0].ref, rxSatCodeBiasRef(*SatId::parse("G12"), SignalId{ System::GPS, '1', 'W' }));
    EXPECT_EQ(t->part[0].partial, 1.0);
    EXPECT_EQ(t->part[0].column, c1);
    EXPECT_EQ(termValue(eq, *t), 0.37);

    // The Jacobian row has +1 on the pair's column and nothing on the other signal's.
    Eigen::VectorXd row = Eigen::VectorXd::Zero(states().getStateSize());
    scatterDense(eq, row);
    EXPECT_EQ(row(c1), 1.0);
    EXPECT_EQ(row(c2), 0.0);

    // The sum order: after the slant ionosphere (and the satellite code bias), before the SSR term.
    std::vector<ObsEffect> effects;
    for (std::uint8_t i = 0; i < eq.n; ++i) effects.push_back(eq.addend[i].effect);
    EXPECT_EQ(effects, (std::vector<ObsEffect>{
        ObsEffect::Modeled, ObsEffect::RangeCorrection, ObsEffect::ReceiverClock,
        ObsEffect::Troposphere, ObsEffect::Ionosphere, ObsEffect::ReceiverSatelliteCodeBias,
        ObsEffect::SsrBias }));
    EXPECT_STREQ(obsEffectName(ObsEffect::ReceiverSatelliteCodeBias), "receiver_satellite_code_bias");
}

TEST_F(RxSatCodeBiasEquationTest, ThereIsNoTermOnAPhaseRowOrAnIonosphereFreePair) {
    load(kKeyOn);
    cover("G12", { "C1W", "C2W" });
    const PppObservation o = observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" });
    update({ o });
    ASSERT_GE(states().getRxSatCodeBiasIndex("G12|C1W"), 0);
    // The modeled value of the pair's label, for the equation of a pair row.
    const PppObservation op = observation("G12", System::GPS, { "C1W_C2W", "L1W_L2W" });

    EXPECT_EQ(term(stack.equation(log, o, "L1W"), ObsEffect::ReceiverSatelliteCodeBias), nullptr);
    EXPECT_EQ(term(stack.equation(log, o, "L2W"), ObsEffect::ReceiverSatelliteCodeBias), nullptr);
    // A pair row, though the columns of its members exist.
    const ObservationEquation pair = stack.equation(log, op, "C1W_C2W");
    EXPECT_EQ(term(pair, ObsEffect::ReceiverSatelliteCodeBias), nullptr);
    EXPECT_NE(term(stack.equation(log, o, "C1W"), ObsEffect::ReceiverSatelliteCodeBias), nullptr);
    // No pair has a state of its own: the family's keys are raw code signals.
    for (const auto& [key, idx] : states().getRxSatCodeBiasStates())
        EXPECT_EQ(key.find('_'), std::string::npos) << key;
}

TEST_F(RxSatCodeBiasEquationTest, WithTheKeyOffThereIsNoStateNoTermAndNoColumn) {
    load("");
    cover("G12", { "C1W", "C2W" });
    const PppObservation o = observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" });
    const EpochMeasurement m = update({ o });

    EXPECT_FALSE(stack.config.estimateRxSatCodeBias());
    EXPECT_TRUE(states().getRxSatCodeBiasStates().empty());
    EXPECT_EQ(states().rxSatCodeBiasCounters().born, 0);
    EXPECT_EQ(m.H.cols(), static_cast<Eigen::Index>(states().getStateSize()));
    // Even a state placed by hand is not read: the term is gated by the key.
    states().addRxSatCodeBiasState("G12|C1W");
    EXPECT_EQ(term(stack.equation(log, o, "C1W"), ObsEffect::ReceiverSatelliteCodeBias), nullptr);
}

TEST_F(RxSatCodeBiasEquationTest, AConstellationOutsideTheListGetsNoState) {
    load(kKeyOn);   // the default list is GPS
    cover("G12", { "C1W" });
    cover("E11", { "C1C" });
    const PppObservation g = observation("G12", System::GPS, { "C1W", "L1W" });
    const PppObservation e = observation("E11", System::Galileo, { "C1C", "L1C" });
    update({ g, e });
    EXPECT_GE(states().getRxSatCodeBiasIndex("G12|C1W"), 0);
    EXPECT_LT(states().getRxSatCodeBiasIndex("E11|C1C"), 0);
    EXPECT_EQ(term(stack.equation(log, e, "C1C"), ObsEffect::ReceiverSatelliteCodeBias), nullptr);
}

TEST_F(RxSatCodeBiasEquationTest, TheListNamesTheConstellationsAsTheCodeSpellsThem) {
    load(std::string(kKeyOn) + R"(, "rx_sat_code_bias_systems": ["Galileo"])");
    ASSERT_EQ(stack.config.getRxSatCodeBiasSystems(), (std::vector<std::string>{ "Galileo" }));
    cover("G12", { "C1W" });
    cover("E11", { "C1C" });
    const PppObservation g = observation("G12", System::GPS, { "C1W", "L1W" });
    const PppObservation e = observation("E11", System::Galileo, { "C1C", "L1C" });
    update({ g, e });
    EXPECT_LT(states().getRxSatCodeBiasIndex("G12|C1W"), 0);
    EXPECT_GE(states().getRxSatCodeBiasIndex("E11|C1C"), 0);
    EXPECT_NE(term(stack.equation(log, e, "C1C"), ObsEffect::ReceiverSatelliteCodeBias), nullptr);
}

TEST_F(RxSatCodeBiasEquationTest, AnUncoveredSignalKeepsItsSatelliteCodeBiasStateAndNeverHasBoth) {
    load(std::string(kKeyOn) + R"(, "estimate_sat_biases": true)");
    cover("G12", { "C1W" });   // C2W has no published bias
    const PppObservation o = observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" });
    update({ o });
    update({ observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" }) });

    // Covered C1W: the receiver-satellite state; uncovered C2W: the satellite code-bias state.
    EXPECT_GE(states().getRxSatCodeBiasIndex("G12|C1W"), 0);
    EXPECT_LT(states().getSatBiasIndex("G12|C1W"), 0);
    EXPECT_GE(states().getSatBiasIndex("G12|C2W"), 0);
    EXPECT_LT(states().getRxSatCodeBiasIndex("G12|C2W"), 0);
    for (const auto& [key, idx] : states().getRxSatCodeBiasStates())
        EXPECT_LT(states().getSatBiasIndex(key), 0) << key;

    const PppObservation now = observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" });
    const ObservationEquation c1 = stack.equation(log, now, "C1W");
    const ObservationEquation c2 = stack.equation(log, now, "C2W");
    EXPECT_NE(term(c1, ObsEffect::ReceiverSatelliteCodeBias), nullptr);
    EXPECT_EQ(term(c1, ObsEffect::SatelliteCodeBias), nullptr);
    EXPECT_EQ(term(c2, ObsEffect::ReceiverSatelliteCodeBias), nullptr);
    EXPECT_NE(term(c2, ObsEffect::SatelliteCodeBias), nullptr);
}

TEST_F(RxSatCodeBiasEquationTest, AStateIsBornOnceAndNeverPrunedWhenItsSatelliteLeaves) {
    load(kKeyOn);
    cover("G12", { "C1W" });
    cover("G15", { "C1W" });
    const auto both = [&] {
        return std::vector<PppObservation>{
            observation("G12", System::GPS, { "C1W", "L1W" }),
            observation("G15", System::GPS, { "C1W", "L1W" }) };
    };
    update(both());
    EXPECT_EQ(states().rxSatCodeBiasCounters().born, 2);
    Eigen::VectorXd x = states().getStateVector();
    x(states().getRxSatCodeBiasIndex("G12|C1W")) = 0.19;
    states().setStateVector(x);

    // G12 sets: its ionosphere, ambiguity and the rest are pruned, its code-bias deviation is not.
    for (int k = 0; k < 3; ++k) update({ observation("G15", System::GPS, { "C1W", "L1W" }) });
    EXPECT_EQ(states().getIonosphereIndex("G12"), -1);
    ASSERT_GE(states().getRxSatCodeBiasIndex("G12|C1W"), 0);
    EXPECT_EQ(states().getStateVector()(states().getRxSatCodeBiasIndex("G12|C1W")), 0.19);
    // It rises again: the same state, no second birth.
    update(both());
    EXPECT_EQ(states().rxSatCodeBiasCounters().born, 2);
    EXPECT_EQ(states().getStateVector()(states().getRxSatCodeBiasIndex("G12|C1W")), 0.19);
    EXPECT_EQ(states().getRxSatCodeBiasStates().size(), 2u);
}

TEST_F(RxSatCodeBiasEquationTest, AStoredEntryIsTheValueAndAgedVarianceOfTheBornState) {
    load(kKeyOn);
    cover("G12", { "C1W", "C2W" });
    RxSatCodeBiasStore store;
    RxSatCodeBiasEntry e;
    e.satellite = "G12";
    e.signal    = "C1W";
    e.value_m   = 0.052;
    e.sigma_m   = 0.01;
    e.updated   = GnssTime{ 2417, 602200.0 };   // one hour before the epoch below
    store.entries["G12|C1W"] = e;
    stack.model->setRxSatCodeBiasStore(&store);
    states().setStateEpochTime(GnssTime{ 2418, 1000.0 });

    update({ observation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" }) });
    const int c1 = states().getRxSatCodeBiasIndex("G12|C1W");
    const int c2 = states().getRxSatCodeBiasIndex("G12|C2W");
    ASSERT_GE(c1, 0);
    ASSERT_GE(c2, 0);
    EXPECT_DOUBLE_EQ(states().getStateVector()(c1), 0.052);
    const double q = stack.config.getProcessNoise().rx_sat_code_bias;
    // The variance is the entry's sigma squared grown by q over the 3600 s of its age.
    EXPECT_NEAR(states().getStateVariance(c1), 0.01 * 0.01 + q * 3600.0, 1e-15);
    // No entry for C2W: the prior.
    EXPECT_EQ(states().getStateVector()(c2), 0.0);
    EXPECT_EQ(states().rxSatCodeBiasCounters().born, 2);
    EXPECT_EQ(states().rxSatCodeBiasCounters().born_from_store, 1);
}
