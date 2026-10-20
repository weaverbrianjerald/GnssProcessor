/**
 * @file test_observation_equation.cpp
 * @brief The term list of a code or phase row (measurement/ObservationEquation.hpp) over the
 *        single-receiver state (measurement/UserParamView.hpp).
 *
 * Four groups:
 *  - the state keys: every family's typed identity renders the key the state manager stores
 *    the parameter under, checked against hand-written key text over every classified signal;
 *  - the term list: the terms of representative rows in the order of the sum, their columns and
 *    partials, and the predicted observable against the same sum written out in the test, bit
 *    for bit;
 *  - the refusals: a phase row without its ambiguity, and a satellite text that is not a
 *    canonical RINEX id;
 *  - the postures: whole epochs built through the model over each posture the row has a
 *    branch for, with finite rows, and the agreement of the producer's satellite states with
 *    the consumer's known SSR terms.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "common/PppUtils.hpp"
#include "common/SignalBand.hpp"
#include "filter/StateKeySchema.hpp"
#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquation.hpp"
#include "support/SameBits.hpp"
#include "measurement/UserParamView.hpp"
#include "products/ProductParser.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"
#include "support/ModelStack.hpp"
#include "support/TestTempDir.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;
using gnss_test::sameBits;
namespace K = GnssProcessor::StateKeys;

namespace {

const std::vector<System> kSystems = {System::GPS, System::Galileo, System::GLONASS,
                                      System::BeiDou, System::QZSS, System::SBAS};

/// Every (digit, attribute) the classifier maps to a known band for @p sys.
std::vector<SignalId> classifiedSignals(System sys) {
    std::vector<SignalId> out;
    for (char d = '0'; d <= '9'; ++d)
        for (char a = 'A'; a <= 'Z'; ++a)
            if (classifySignal(traitsOf(sys).name, std::string{'C', d, a}).band
                != FrequencyBand::Unknown)
                out.push_back(SignalId{sys, d, a});
    return out;
}

/// The code and phase text of a signal, written by hand.
std::string codeText(const SignalId& s)  { return std::string{'C', s.digit, s.attribute}; }
std::string phaseText(const SignalId& s) { return std::string{'L', s.digit, s.attribute}; }

// ===========================================================================
// The state keys
// ===========================================================================

TEST(UserParamKeys, SatelliteKeyedFamiliesRenderTheStoredKeys) {
    for (const System sys : kSystems) {
        const std::vector<SignalId> signals = classifiedSignals(sys);
        ASSERT_FALSE(signals.empty()) << systemName(sys);
        for (const int prn : {1, 9, 10, 32, 99}) {
            const SatId sat{sys, prn};
            const std::string s = sat.str();
            ASSERT_EQ(s.size(), 3u);
            EXPECT_EQ(userKey(slantIonoRef(sat)), s);
            EXPECT_EQ(userKey(phaseColourRef(sat)), s);
            EXPECT_EQ(userKey(satClockRef(sat)), s + "|SC");
            for (const SignalId& sig : signals) {
                EXPECT_EQ(userKey(ambiguityRef(sat, sig)), s + "_" + phaseText(sig));
                EXPECT_EQ(userKey(satCodeBiasRef(sat, sig)), s + "|" + codeText(sig));
                EXPECT_EQ(userKey(satPhaseBiasRef(sat, sig)), s + "|" + phaseText(sig) + "|PB");
                EXPECT_EQ(userKey(rxPhaseBiasRef(sys, sig)),
                          std::string(systemName(sys)) + "_RPB_" + phaseText(sig));
            }
            // An iono-free pair of the first two signals, as code and as phase.
            const SignalPair pair{signals.front(), signals.back()};
            EXPECT_EQ(userKey(ambiguityIfRef(sat, pair)),
                      s + "_" + phaseText(pair.a) + "_" + phaseText(pair.b));
            EXPECT_EQ(userKey(satCodeBiasIfRef(sat, pair)),
                      s + "|" + codeText(pair.a) + "_" + codeText(pair.b));
        }
    }
}

TEST(UserParamKeys, NarrowAndWideLaneKeysUseTheBandDigits) {
    for (const System sys : kSystems) {
        const ConstellationTraits& t = traitsOf(sys);
        const SatId sat{sys, 7};
        for (std::size_t i = 0; i < t.n_bands; ++i) {
            EXPECT_EQ(userKey(narrowLaneRef(sat, t.bands[i].band)),
                      sat.str() + "_NL_" + std::string(1, t.bands[i].digit))
                << systemName(sys);
            for (std::size_t j = 0; j < t.n_bands; ++j) {
                if (i == j) continue;
                const char a = std::min(t.bands[i].digit, t.bands[j].digit);
                const char b = std::max(t.bands[i].digit, t.bands[j].digit);
                EXPECT_EQ(userKey(wideLaneRef(sat, t.bands[i].band, t.bands[j].band)),
                          sat.str() + "_WL_" + std::string(1, a) + "_" + std::string(1, b))
                    << systemName(sys);
            }
        }
    }
}

TEST(UserParamKeys, ReceiverCodeBiasKeysRenderTheStoredKeys) {
    for (const System sys : kSystems) {
        const std::string name = systemName(sys);
        RxCbKey dcb;
        dcb.sys = sys;
        dcb.form = ParamForm::Dcb;
        EXPECT_EQ(userCodeBiasKey(dcb), name + "_DCB");
        const ConstellationTraits& t = traitsOf(sys);
        for (std::size_t i = 0; i < t.n_bands; ++i) {
            RxCbKey ifb;
            ifb.sys = sys;
            ifb.form = ParamForm::Ifb;
            ifb.band = t.bands[i].band;
            EXPECT_EQ(userCodeBiasKey(ifb), name + "_IFB" + std::string(1, t.bands[i].digit));
            for (const int k : {-7, -1, 0, 3, 6}) {
                RxCbKey slot = ifb;
                slot.form = ParamForm::GloSlotIfb;
                slot.glo_slot = k;
                EXPECT_EQ(userCodeBiasKey(slot),
                          name + "_IFB" + std::string(1, t.bands[i].digit) + "_k"
                              + (k >= 0 ? "+" : "") + std::to_string(k));
            }
        }
        for (const SignalId& sig : classifiedSignals(sys)) {
            RxCbKey isc;
            isc.sys = sys;
            isc.form = ParamForm::Isc;
            isc.signal = sig;
            EXPECT_EQ(userCodeBiasKey(isc), name + "_ISC_" + codeText(sig));
            EXPECT_EQ(userKey(rxCodeBiasRef(isc)), name + "_ISC_" + codeText(sig));
        }
    }
}

TEST(UserParamKeys, ABandTheConstellationDoesNotUseHasNoKey) {
    RxCbKey ifb;
    ifb.sys = System::GPS;
    ifb.form = ParamForm::Ifb;
    ifb.band = FrequencyBand::Unknown;
    EXPECT_TRUE(userCodeBiasKey(ifb).empty());
    EXPECT_TRUE(userKey(narrowLaneRef(SatId{System::GPS, 1}, FrequencyBand::GalE1)).empty());
}

TEST(UserParamKeys, FixedCoreAndInterSystemTermsAreNotStringKeyed) {
    EXPECT_TRUE(userKey(rxPositionRef(0)).empty());
    EXPECT_TRUE(userKey(zwdRef()).empty());
    EXPECT_TRUE(userKey(rxClockRef()).empty());
    EXPECT_TRUE(userKey(phaseClockRef()).empty());
    EXPECT_TRUE(userKey(gradientRef(true)).empty());
    EXPECT_TRUE(userKey(isbRef(System::Galileo)).empty());
}

TEST(UserParamKeys, CanonicalSatelliteTextRoundTrips) {
    for (const System sys : kSystems)
        for (int prn = 1; prn <= 99; ++prn) {
            const SatId sat{sys, prn};
            const auto back = SatId::parse(sat.str());
            ASSERT_TRUE(back.has_value());
            EXPECT_EQ(*back, sat);
            EXPECT_EQ(back->str(), sat.str());
        }
    // The spellings the equation refuses to name.
    EXPECT_NE(SatId::parse("G5")->str(), "G5");
    EXPECT_NE(SatId::parse("G 5")->str(), "G 5");
}

TEST(UserParamKeys, InterSystemBiasExistsExactlyWhereTheStateInitializerNamesOne) {
    for (const System sys : kSystems)
        EXPECT_EQ(hasInterSystemBias(sys), !StateInitializer::systemToIsbName(sys).empty())
            << systemName(sys);
    EXPECT_EQ(hasInterSystemBias(System::Unknown),
              !StateInitializer::systemToIsbName(System::Unknown).empty());
}

TEST(ParamRefFactories, EachFactorySetsOnlyTheFieldsOfItsKind) {
    const SatId sat{System::Galileo, 5};
    const SignalId sig{System::Galileo, '1', 'C'};
    const ParamRef amb = ambiguityRef(sat, sig);
    EXPECT_EQ(amb.kind, NetworkParamKind::Ambiguity);
    EXPECT_EQ(amb.form, ParamForm::None);
    EXPECT_EQ(amb.sat, sat);
    EXPECT_EQ(amb.signal, sig);
    EXPECT_EQ(amb.core, UserCore::None);
    EXPECT_NE(amb, ambiguityIfRef(sat, SignalPair{sig, sig}));
    EXPECT_NE(phaseClockRef(), rxClockRef());
    EXPECT_NE(gradientRef(true), gradientRef(false));
    EXPECT_EQ(rxCbKeyOf(rxCodeBiasRef(RxCbKey{System::GPS, ParamForm::Ifb,
                                              FrequencyBand::GpsL2, SignalId{}, 0})),
              (RxCbKey{System::GPS, ParamForm::Ifb, FrequencyBand::GpsL2, SignalId{}, 0}));
    EXPECT_FALSE(describe(amb).empty());
}

// ===========================================================================
// The fixture: a single-receiver model over synthetic observations
// ===========================================================================

constexpr const char* kSat = "G12";

using Stack = gnss_test::ModelStack;

class ObservationEquationTest : public ::testing::Test {
protected:
    LogManager log;
    Stack primary;
    GnssConfig& config = primary.config;
    SsrCorrectionStore& store = primary.store;
    std::unique_ptr<StateManager>& states = primary.states;
    std::unique_ptr<MeasurementModel>& model = primary.model;
    int epoch = 0;

    void load(const std::string& mode, const std::string& processing = "",
              const std::string& estimation = "", bool bssd = false,
              const std::string& top = "") {
        ASSERT_TRUE(primary.load(
            log, ::testing::UnitTest::GetInstance()->current_test_info()->name(), mode,
            processing, estimation, bssd, top));
    }

    /// One satellite with the given signals (see gnss_test::syntheticObservation).
    PppObservation observation(const std::string& sat, System sys,
                               const std::vector<std::string>& signals, double azimuth = 45.0,
                               double elevation = 45.0, double modeled = 2.2e7) {
        return gnss_test::syntheticObservation(sat, sys, signals, epoch, azimuth, elevation,
                                               modeled);
    }

    /// The last epoch the model built.
    EpochMeasurement last;

    /// One epoch through the model (births, the row loop, the differencing).
    EpochMeasurement update(const std::vector<PppObservation>& obs) {
        ++epoch;
        last = model->computeMeasurementUpdate(obs, states->getStateVector());
        return last;
    }

    ObservationEquation equation(const PppObservation& o, const std::string& signal,
                                 const std::map<std::string, char>& nlwl = {}) {
        return primary.equation(log, o, signal, nlwl);
    }

    void perturbStates() { primary.perturbStates(); }

    /// The effects of an equation, in order.
    static std::vector<ObsEffect> effects(const ObservationEquation& eq) {
        std::vector<ObsEffect> out;
        for (std::uint8_t i = 0; i < eq.n; ++i) out.push_back(eq.addend[i].effect);
        return out;
    }

    /// The term of one effect.
    static const ObsAddend* term(const ObservationEquation& eq, ObsEffect effect) {
        for (std::uint8_t i = 0; i < eq.n; ++i)
            if (eq.addend[i].effect == effect) return &eq.addend[i];
        return nullptr;
    }

    /// The last epoch has rows, and every entry of them is finite.
    void expectRowsBuilt() const {
        ASSERT_GT(last.innovations.size(), 0);
        EXPECT_EQ(last.H.rows(), last.innovations.size());
        EXPECT_TRUE(last.H.allFinite());
        EXPECT_TRUE(last.innovations.allFinite());
        EXPECT_TRUE(last.R_v.allFinite());
    }
};

// ===========================================================================
// The term list
// ===========================================================================

TEST_F(ObservationEquationTest, ARowIsTheSumOfItsTermsInOrderWithTheirPartials) {
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"});
    update({o});
    perturbStates();

    const Eigen::VectorXd x = states->getStateVector();
    const int iono = states->getIonosphereIndex(kSat);
    const int amb = states->getAmbiguityIndex(K::makeAmbiguityKey(kSat, "L1W"));
    ASSERT_GE(iono, 0);
    ASSERT_GE(amb, 0);

    // Phase row on L1: -alpha on the ionosphere, +1 on the ambiguity; no phase clock (off).
    const ObservationEquation eq = equation(o, "L1W");
    ASSERT_TRUE(eq.usable);
    EXPECT_EQ(effects(eq), (std::vector<ObsEffect>{
        ObsEffect::Modeled, ObsEffect::RangeCorrection, ObsEffect::ReceiverClock,
        ObsEffect::Troposphere, ObsEffect::Ionosphere, ObsEffect::SsrBias,
        ObsEffect::Ambiguity}));

    const ObsAddend* range = term(eq, ObsEffect::RangeCorrection);
    ASSERT_NE(range, nullptr);
    ASSERT_EQ(range->n_part, 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(range->part[i].column, i);
        EXPECT_EQ(range->part[i].partial, -o.los(i));
        EXPECT_EQ(range->part[i].ref, rxPositionRef(static_cast<std::uint8_t>(i)));
    }
    EXPECT_EQ(term(eq, ObsEffect::Troposphere)->part[0].partial, o.mf_w);
    EXPECT_EQ(term(eq, ObsEffect::Troposphere)->part[0].column,
              static_cast<int>(states->getTropoIndex()));
    EXPECT_EQ(term(eq, ObsEffect::Ionosphere)->part[0].partial, -1.0);   // L1: alpha = 1
    EXPECT_EQ(term(eq, ObsEffect::Ionosphere)->part[0].column, iono);
    EXPECT_EQ(term(eq, ObsEffect::Ambiguity)->part[0].partial, 1.0);
    EXPECT_EQ(term(eq, ObsEffect::Ambiguity)->part[0].column, amb);

    // The predicted observable is the same sum written out, left to right.
    const Eigen::Vector3d pos = x.segment<3>(states->getPositionIndex());
    double expected = o.modeled.at("L1W");
    expected += -o.los.dot(pos - o.rx_pos_linearization);
    expected += x(static_cast<Eigen::Index>(states->getClockIndex()));
    expected += o.mf_w * x(static_cast<Eigen::Index>(states->getTropoIndex()));
    expected += -1.0 * x(iono);
    expected += 0.0;   // no SSR bias in the store
    expected += x(amb);
    EXPECT_TRUE(sameBits(evaluate(eq), expected));

    // The dense row carries each partial at its column and nothing elsewhere.
    Eigen::VectorXd row = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(states->getStateSize()));
    scatterDense(eq, row);
    EXPECT_EQ(row(amb), 1.0);
    EXPECT_EQ(row(iono), -1.0);
    EXPECT_EQ(row(static_cast<Eigen::Index>(states->getTropoIndex())), o.mf_w);
    EXPECT_EQ((row.array() != 0.0).count(), 3 + 1 /*clock*/ + 1 /*tropo*/ + 1 + 1);

    // A code row on L2: +alpha on the ionosphere, no ambiguity.
    const ObservationEquation code = equation(o, "C2W");
    ASSERT_TRUE(code.usable);
    EXPECT_EQ(effects(code), (std::vector<ObsEffect>{
        ObsEffect::Modeled, ObsEffect::RangeCorrection, ObsEffect::ReceiverClock,
        ObsEffect::Troposphere, ObsEffect::Ionosphere, ObsEffect::SsrBias}));
    const double alpha2 = std::pow(Constants::GPS_L1_FREQ / Constants::GPS_L2_FREQ, 2);
    EXPECT_NEAR(term(code, ObsEffect::Ionosphere)->part[0].partial, alpha2, 1e-12);
}

TEST_F(ObservationEquationTest, GradientPhaseClockAndInterSystemBiasKeepTheirGrouping) {
    load("ppp-rtk", "", R"(, "estimate_tropo_gradients": true, "estimate_phase_clock": true)");
    const PppObservation gps = observation(kSat, System::GPS, {"C1W", "L1W"});
    const PppObservation gal = observation("E05", System::Galileo, {"C1C", "L1C"}, 120.0, 33.0);
    update({gps, gal});
    perturbStates();
    ASSERT_TRUE(states->hasTropoGradients());
    ASSERT_TRUE(states->hasPhaseClock());
    ASSERT_NE(states->getSystemTimeOffsetIndex("Galileo"), std::numeric_limits<size_t>::max());

    const Eigen::VectorXd x = states->getStateVector();
    const ObservationEquation eq = equation(gal, "L1C");
    ASSERT_TRUE(eq.usable);
    EXPECT_EQ(effects(eq), (std::vector<ObsEffect>{
        ObsEffect::Modeled, ObsEffect::RangeCorrection, ObsEffect::ReceiverClock,
        ObsEffect::PhaseClock, ObsEffect::Troposphere, ObsEffect::TropoGradient,
        ObsEffect::SystemTimeOffset, ObsEffect::Ionosphere, ObsEffect::SsrBias,
        ObsEffect::Ambiguity}));

    // The gradient term keeps its factor outside the sum: mf_grad * (g_n cos + g_e sin).
    const double el = gal.elevation * Constants::PI / 180.0;
    const double az = gal.azimuth * Constants::PI / 180.0;
    const double mf_grad = 1.0 / (std::sin(el) * std::tan(el) + 0.0032);
    const double gn = x(static_cast<Eigen::Index>(states->getTropoGradientNorthIndex()));
    const double ge = x(static_cast<Eigen::Index>(states->getTropoGradientEastIndex()));
    ObsTermValues terms;
    const double predicted = evaluate(eq, &terms);
    EXPECT_TRUE(sameBits(terms[ObsEffect::TropoGradient],
                         mf_grad * (gn * std::cos(az) + ge * std::sin(az))));
    EXPECT_TRUE(sameBits(terms[ObsEffect::PhaseClock],
                         x(static_cast<Eigen::Index>(states->getPhaseClockIndex()))));
    EXPECT_TRUE(sameBits(terms[ObsEffect::SystemTimeOffset],
                         x(static_cast<Eigen::Index>(states->getSystemTimeOffsetIndex("Galileo")))));
    EXPECT_EQ(term(eq, ObsEffect::TropoGradient)->scale, mf_grad);

    // The Jacobian entries of the gradient are scale * partial.
    Eigen::VectorXd row = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(states->getStateSize()));
    scatterDense(eq, row);
    EXPECT_TRUE(sameBits(row(static_cast<Eigen::Index>(states->getTropoGradientNorthIndex())),
                         mf_grad * std::cos(az)));
    EXPECT_TRUE(sameBits(row(static_cast<Eigen::Index>(states->getTropoGradientEastIndex())),
                         mf_grad * std::sin(az)));
    EXPECT_TRUE(std::isfinite(predicted));

    // The reference constellation has no inter-system bias term.
    EXPECT_EQ(term(equation(gps, "L1W"), ObsEffect::SystemTimeOffset), nullptr);
}

TEST_F(ObservationEquationTest, NarrowAndWideLaneAmbiguityKeepTheirFactorOutsideTheSum) {
    load("ppp-rtk", "", R"(, "state_ambiguity_basis": "nl_wl")");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"});
    update({o});
    perturbStates();
    const int nl = states->getAmbiguityIndex(K::makeNlAmbiguityKey(kSat, '1'));
    const int wl = states->getAmbiguityIndex(K::makeWlAmbiguityKey(kSat, '1', '2'));
    ASSERT_GE(nl, 0);
    ASSERT_GE(wl, 0);
    const std::map<std::string, char> ref_band{{kSat, '1'}};
    const Eigen::VectorXd x = states->getStateVector();
    const PppUtils utils(log);
    const double lam1 = utils.getWavelength(System::GPS, "L1W", kSat);
    const double lam2 = utils.getWavelength(System::GPS, "L2W", kSat);

    // The reference band: lambda * NL.
    const ObservationEquation e1 = equation(o, "L1W", ref_band);
    ASSERT_TRUE(e1.usable);
    ObsTermValues t1;
    evaluate(e1, &t1);
    EXPECT_TRUE(sameBits(t1[ObsEffect::Ambiguity], lam1 * x(nl)));
    EXPECT_EQ(term(e1, ObsEffect::Ambiguity)->n_part, 1);

    // The other band: lambda * (NL - s * WL), s = +1 when the reference band is the lower one.
    const ObservationEquation e2 = equation(o, "L2W", ref_band);
    ASSERT_TRUE(e2.usable);
    ObsTermValues t2;
    evaluate(e2, &t2);
    EXPECT_TRUE(sameBits(t2[ObsEffect::Ambiguity], lam2 * (x(nl) - 1.0 * x(wl))));
    Eigen::VectorXd row = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(states->getStateSize()));
    scatterDense(e2, row);
    EXPECT_TRUE(sameBits(row(nl), lam2));
    EXPECT_TRUE(sameBits(row(wl), -lam2 * 1.0));
}

// ===========================================================================
// The refusals
// ===========================================================================

TEST_F(ObservationEquationTest, APhaseRowWithoutItsAmbiguityIsUnusable) {
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    // No epoch has run: the model has no ambiguity (and no ionosphere) state yet.
    const ObservationEquation phase = equation(o, "L1W");
    EXPECT_FALSE(phase.usable);
    EXPECT_EQ(phase.fault, ObsFault::NoAmbiguity);
    // A code row needs none.
    EXPECT_TRUE(equation(o, "C1W").usable);
}

TEST_F(ObservationEquationTest, ASatelliteTextThatIsNotCanonicalCannotBeNamed) {
    load("ppp-rtk");
    PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});
    o.satellite = "G12 ";   // not the text the states were born under
    const ObservationEquation eq = equation(o, "C1W");
    EXPECT_FALSE(eq.usable);
    EXPECT_EQ(eq.fault, ObsFault::UnmappedIdentity);
}

TEST_F(ObservationEquationTest, ADopplerRowHasNoEquation) {
    load("ppp-rtk");
    PppObservation o = observation(kSat, System::GPS, {"C1W"});
    o.modeled["D1C"] = 0.0;
    const ObservationEquation eq = equation(o, "D1C");
    EXPECT_FALSE(eq.usable);
    EXPECT_EQ(eq.fault, ObsFault::NotCodeOrPhase);
}

TEST_F(ObservationEquationTest, AMissingModeledValueThrows) {
    load("ppp-rtk");
    PppObservation o = observation(kSat, System::GPS, {"C1W"});
    o.modeled.erase("C1W");
    EXPECT_THROW(equation(o, "C1W"), std::out_of_range);
}

// ===========================================================================
// The rows the model builds in each posture
// ===========================================================================

TEST_F(ObservationEquationTest, PerBandUncombinedBuildsItsRows) {
    load("ppp-rtk");
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
                observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0)});
        perturbStates();
    }
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, MultiSystemWithGradientsAndPhaseClockBuildsItsRows) {
    load("ppp-rtk", "", R"(, "estimate_tropo_gradients": true, "estimate_phase_clock": true)");
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
                observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0)});
        perturbStates();
    }
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, NlWlBasisBuildsItsRows) {
    load("ppp-rtk", "", R"(, "state_ambiguity_basis": "nl_wl")");
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})});
        perturbStates();
    }
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, IonoFreeCombinationBuildsItsRows) {
    load("ppp", R"(, "uncombined_enabled": false)");
    ASSERT_FALSE(config.isUncombinedEnabled());
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W_C2W", "L1W_L2W"})});
        perturbStates();
    }
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, SatelliteCodeBiasStatesBuildItsRows) {
    load("ppp-rtk", "", R"(, "estimate_sat_biases": true)");
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})});
        perturbStates();
    }
    ASSERT_GE(states->getSatBiasIndex(std::string(kSat) + "|C1W"), 0);
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, SatelliteCodeBiasOfAnIonoFreePairBuildsItsRows) {
    load("ppp", R"(, "uncombined_enabled": false)", R"(, "estimate_sat_biases": true)");
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W_C2W", "L1W_L2W"})});
        perturbStates();
    }
    ASSERT_GE(states->getSatBiasIndex(std::string(kSat) + "|C1W_C2W"), 0);
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, BetweenSatelliteDifferencingBuildsItsRows) {
    load("ppp-rtk", "", "", /*bssd=*/true);
    for (int e = 0; e < 3; ++e) {
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
                observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
                observation("G20", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 300.0, 40.0)});
        perturbStates();
    }
    expectRowsBuilt();
}

TEST_F(ObservationEquationTest, ProducerSatelliteStatesBuildItsRows) {
    // The network producer's satellite phase bias, receiver phase bias, satellite clock and
    // phase colour: columns the single-receiver pre-pass does not create, added by hand.
    load("ppp-rtk", "", R"(, "estimate_sat_phase_biases": true, "estimate_sat_clocks": true)");
    config.setProcessingMode(ProcessingMode::PPP_RTK_Network);
    update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})});
    states->addSatPhaseBiasState(K::makeSatPhaseBiasKey(kSat, "L1W"), 0.31);
    states->addSatPhaseBiasState(K::makeSatPhaseBiasKey(kSat, "L2W"), -0.17);
    states->addReceiverPhaseBiasState(K::makeReceiverPhaseBiasKey("GPS", "L1W"), 0.05);
    states->addSatClockState(K::makeSatClockKey(kSat), 1.9);
    for (int e = 0; e < 3; ++e) {
        perturbStates();
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})});
    }
    expectRowsBuilt();
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"});
    const ObservationEquation eq = equation(o, "L1W");
    ASSERT_TRUE(eq.usable);
    const ObsAddend* spb = term(eq, ObsEffect::SatellitePhaseBias);
    ASSERT_NE(spb, nullptr);
    EXPECT_EQ(spb->part[0].partial, kSatBiasSign);
    const ObsAddend* clk = term(eq, ObsEffect::SatelliteClock);
    ASSERT_NE(clk, nullptr);
    EXPECT_EQ(clk->part[0].partial, kSatClockPartial);
    // The network producer takes no SSR bias term.
    EXPECT_EQ(term(eq, ObsEffect::SsrBias), nullptr);
}

TEST_F(ObservationEquationTest, NetworkPostureWithReceiverPhaseBiasAndPhaseColourBuildsItsRows) {
    // A loadable network posture with the eq34 receiver phase bias and the phase-colour
    // state: the network loader forces the satellite clock and phase bias on, and these two
    // keys add the receiver phase bias to the satellite phase-bias term and the colour term.
    const std::string text = R"({
        "processing": {"mode": "ppp-rtk-network", "active_systems": ["GPS", "Galileo"],
                       "signal_priorities": {"GPS": "WC", "Galileo": "CB"}},
        "network": {
            "clock_datum_station": "AAAA",
            "ambiguity_vehicle": "float_kf",
            "phase_leveling": true,
            "carry_conditional_coupling": true,
            "shared_carry_full_cov": true,
            "phase_datum_eq34": true,
            "phase_colour_state": true,
            "stations": [
                {"name": "AAAA", "position_ecef": [-2493304.99, -4655214.79, 3565497.67]},
                {"name": "BBBB", "position_ecef": [-2281621.75, -1453585.08, 5756964.89]}
            ]
        }
    })";
    ASSERT_TRUE(primary.loadText(log, "network_posture", text));
    ASSERT_EQ(config.getProcessingMode(), ProcessingMode::PPP_RTK_Network);
    ASSERT_TRUE(config.networkPhaseDatumEq34());
    ASSERT_TRUE(config.getNetworkConfig().phase_colour_state);
    ASSERT_TRUE(config.estimateSatPhaseBiases());
    ASSERT_TRUE(config.estimateSatClocks());

    // The columns the network's shared admission would add, added by hand.
    update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})});
    states->addSatPhaseBiasState(K::makeSatPhaseBiasKey(kSat, "L1W"), 0.31);
    states->addSatPhaseBiasState(K::makeSatPhaseBiasKey(kSat, "L2W"), -0.17);
    states->addReceiverPhaseBiasState(K::makeReceiverPhaseBiasKey("GPS", "L1W"), 0.05);
    states->addSatClockState(K::makeSatClockKey(kSat), 1.9);
    states->addPhaseColourState(kSat, 0.0001);
    for (int e = 0; e < 3; ++e) {
        perturbStates();
        update({observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})});
    }
    expectRowsBuilt();

    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"});
    const ObservationEquation eq = equation(o, "L1W");
    ASSERT_TRUE(eq.usable);
    // The receiver phase bias joins the satellite phase bias in one term; the colour is +1.
    const ObsAddend* spb = term(eq, ObsEffect::SatellitePhaseBias);
    ASSERT_NE(spb, nullptr);
    ASSERT_EQ(spb->n_part, 2);
    EXPECT_EQ(spb->part[1].ref, rxPhaseBiasRef(System::GPS, SignalId{System::GPS, '1', 'W'}));
    EXPECT_EQ(spb->part[1].partial, 1.0);
    const ObsAddend* colour = term(eq, ObsEffect::PhaseColour);
    ASSERT_NE(colour, nullptr);
    EXPECT_EQ(colour->part[0].partial, 1.0);
    // The receiver phase bias exists for L1 only here, so L2 carries the satellite term alone.
    const ObsAddend* spb2 = term(equation(o, "L2W"), ObsEffect::SatellitePhaseBias);
    ASSERT_NE(spb2, nullptr);
    EXPECT_EQ(spb2->n_part, 1);
}

// ===========================================================================
// Producer and consumer agree on the satellite terms
// ===========================================================================

TEST_F(ObservationEquationTest, ProducerStatesAndConsumerKnownTermsGiveTheSamePrediction) {
    // Producer: the satellite code bias b, phase bias p and clock c are states.  The modeled
    // value is small so that the two orders of summation agree to far better than 1e-9 m.
    load("ppp-rtk", "", R"(, "estimate_sat_biases": true, "estimate_sat_phase_biases": true,
                           "estimate_sat_clocks": true)");
    config.setProcessingMode(ProcessingMode::PPP_RTK_Network);
    const PppObservation base =
        observation(kSat, System::GPS, {"C1W", "L1W"}, 45.0, 45.0, 1234.5678);
    update({base});
    const double b = 1.37, p = -0.62, c = 2.41;
    const std::string bias_key = std::string(kSat) + "|C1W";
    const std::string phase_key = K::makeSatPhaseBiasKey(kSat, "L1W");
    const std::string clock_key = K::makeSatClockKey(kSat);
    if (states->getSatBiasIndex(bias_key) < 0) states->addSatBiasState(bias_key, 0.0);
    states->addSatPhaseBiasState(phase_key, 0.0);
    states->addSatClockState(clock_key, 0.0);
    primary.perturbStates();
    Eigen::VectorXd x = states->getStateVector();
    x(states->getSatBiasIndex(bias_key)) = b;
    x(states->getSatPhaseBiasIndex(phase_key)) = p;
    x(states->getSatClockIndex(clock_key)) = c;
    states->setStateVector(x);

    const ObservationEquation code_eq = equation(base, "C1W");
    const ObservationEquation phase_eq = equation(base, "L1W");
    ASSERT_TRUE(code_eq.usable);
    ASSERT_TRUE(phase_eq.usable);
    ASSERT_NE(term(code_eq, ObsEffect::SatelliteCodeBias), nullptr);
    ASSERT_NE(term(phase_eq, ObsEffect::SatellitePhaseBias), nullptr);
    ASSERT_NE(term(phase_eq, ObsEffect::SatelliteClock), nullptr);
    const double code_producer = evaluate(code_eq);
    const double phase_producer = evaluate(phase_eq);

    // Consumer: a stack without the satellite states, on the same values of every other
    // column; the biases come from the SSR store with the shared sign, and the clock is
    // inside the modeled value as kSatClockPartial times c.
    Stack consumer;
    ASSERT_TRUE(consumer.load(log, "consumer", "ppp-rtk", "", "", false));
    ASSERT_FALSE(consumer.config.estimateSatBiases());
    PppObservation known = base;
    for (auto& [signal, modeled] : known.modeled)
        modeled = base.modeled.at(signal) + kSatClockPartial * c;
    consumer.model->computeMeasurementUpdate({known}, consumer.states->getStateVector());
    Eigen::VectorXd xc = consumer.states->getStateVector();
    for (Eigen::Index i = 0; i < 5; ++i) xc(i) = x(i);
    xc(consumer.states->getIonosphereIndex(kSat)) = x(states->getIonosphereIndex(kSat));
    const std::string amb_key = K::makeAmbiguityKey(kSat, "L1W");
    xc(consumer.states->getAmbiguityIndex(amb_key)) = x(states->getAmbiguityIndex(amb_key));
    consumer.states->setStateVector(xc);

    SsrHeader header;
    SsrCodeBias cb;
    cb.sat = kSat;
    cb.bias_per_signal["C1W"] = b;
    cb.received_time = base.time;
    consumer.store.pushCodeBias(cb, header);
    SsrPhaseBias pb;
    pb.sat = kSat;
    SsrPhaseBiasEntry entry;
    entry.bias_m = p;
    pb.bias_per_signal["L1W"] = entry;
    pb.received_time = base.time;
    consumer.store.pushPhaseBias(pb, header);

    const ObservationEquation code_known = consumer.equation(log, known, "C1W");
    const ObservationEquation phase_known = consumer.equation(log, known, "L1W");
    ASSERT_TRUE(code_known.usable);
    ASSERT_TRUE(phase_known.usable);
    EXPECT_NEAR(evaluate(code_known), code_producer, 1e-9);
    EXPECT_NEAR(evaluate(phase_known), phase_producer, 1e-9);

    // The producer's partial and the consumer's multiplier are one constant.
    EXPECT_EQ(term(code_eq, ObsEffect::SatelliteCodeBias)->part[0].partial, kSatBiasSign);
    EXPECT_EQ(term(code_known, ObsEffect::SsrBias)->constant, kSatBiasSign * b);
    EXPECT_EQ(term(phase_eq, ObsEffect::SatellitePhaseBias)->part[0].partial, kSatBiasSign);
    EXPECT_EQ(term(phase_known, ObsEffect::SsrBias)->constant, kSatBiasSign * p);
    EXPECT_EQ(term(phase_eq, ObsEffect::SatelliteClock)->part[0].partial, kSatClockPartial);
}

}  // namespace
