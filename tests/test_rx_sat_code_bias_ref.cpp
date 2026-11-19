/**
 * @file test_rx_sat_code_bias_ref.cpp
 * @brief The typed identity of the receiver-satellite code bias (common/ParamRef.hpp): its fields,
 *        its table names, its state key and column on the single-receiver state, and the network
 *        producer's views answering "absent" for it.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "measurement/UserParamView.hpp"
#include "network/state/StationParams.hpp"
#include "support/StationParamsBuilder.hpp"

#include <Eigen/Dense>
#include <string>

using namespace GnssProcessor;
namespace K = GnssProcessor::StateKeys;

namespace {

const SatId kSat{ System::GPS, 5 };
const SignalId kC1W{ System::GPS, '1', 'W' };
const SignalId kC2W{ System::GPS, '2', 'W' };

}  // namespace

TEST(RxSatCodeBiasRefTest, TheReferenceIsASatelliteCodeBiasOfItsOwnForm) {
    const ParamRef r = rxSatCodeBiasRef(kSat, kC1W);
    EXPECT_EQ(r.kind, NetworkParamKind::SatCodeBias);
    EXPECT_EQ(r.form, ParamForm::RxSat);
    EXPECT_EQ(r.sat, kSat);
    EXPECT_EQ(r.signal, kC1W);
    EXPECT_EQ(r.core, UserCore::None);
    EXPECT_NE(r, satCodeBiasRef(kSat, kC1W));
    EXPECT_NE(r, satCodeBiasIfRef(kSat, SignalPair{ kC1W, kC2W }));
    EXPECT_NE(r, rxSatCodeBiasRef(kSat, kC2W));
    EXPECT_EQ(r, rxSatCodeBiasRef(kSat, kC1W));
}

TEST(RxSatCodeBiasRefTest, TheFormHasATableNameAndTheKindsAreUnchanged) {
    EXPECT_STREQ(paramFormName(ParamForm::RxSat), "rx_sat");
    EXPECT_STREQ(paramKindName(NetworkParamKind::SatCodeBias), "sat_code_bias");
    // No new kind: the registry's table size is the one of the network.
    EXPECT_EQ(static_cast<std::size_t>(NetworkParamKind::TropoGradient) + 1, kNetworkParamKindCount);
    const std::string text = describe(rxSatCodeBiasRef(kSat, kC1W));
    EXPECT_NE(text.find("sat_code_bias.rx_sat"), std::string::npos) << text;
    EXPECT_NE(text.find("G05"), std::string::npos) << text;
    EXPECT_NE(text.find("1W"), std::string::npos) << text;   // the signal as describe() renders it
}

TEST(RxSatCodeBiasRefTest, TheInstanceCarriesTheSatelliteAndTheSignal) {
    const ParamInstanceId id = paramInstanceOf(rxSatCodeBiasRef(kSat, kC1W), StationId{ 3 });
    EXPECT_EQ(id.cls.kind, NetworkParamKind::SatCodeBias);
    EXPECT_EQ(id.cls.form, ParamForm::RxSat);
    EXPECT_EQ(id.cls.sys, System::GPS);
    EXPECT_EQ(id.cls.signal, kC1W);
    EXPECT_EQ(id.sat, kSat);
    // It is a line of its own, apart from the plain satellite code bias.
    EXPECT_FALSE(id.cls == paramInstanceOf(satCodeBiasRef(kSat, kC1W)).cls);
}

TEST(RxSatCodeBiasRefTest, TheUserKeyIsTheFamilysKeyOverEveryGpsCodeSignal) {
    for (const char* code : { "C1C", "C1W", "C2W", "C2L", "C5Q" }) {
        const auto sig = SignalId::fromObsCode(System::GPS, code);
        ASSERT_TRUE(sig.has_value()) << code;
        EXPECT_EQ(userKey(rxSatCodeBiasRef(kSat, *sig)), K::makeRxSatCodeBiasKey("G05", code));
    }
}

TEST(RxSatCodeBiasRefTest, TheUserViewFindsTheColumnOfTheFamilyAndNotTheOneOfTheSatelliteBias) {
    LogManager log;
    GnssConfig config;
    StateManager states{ 5, log, config };
    states.addRxSatCodeBiasState("G05|C1W");
    states.addSatBiasState("G05|C2W", 0.0);
    Eigen::VectorXd x = states.getStateVector();
    x(states.getRxSatCodeBiasIndex("G05|C1W")) = 0.123;
    states.setStateVector(x);
    const Eigen::VectorXd xv = states.getStateVector();
    const UserParamView view(states, xv, states.getStateSize());

    const ParamSlot rs = view.find(rxSatCodeBiasRef(kSat, kC1W));
    EXPECT_EQ(rs.column, states.getRxSatCodeBiasIndex("G05|C1W"));
    EXPECT_EQ(rs.x, 0.123);
    // A pair with no state of this family: absent, even where a satellite code-bias state exists.
    EXPECT_LT(view.find(rxSatCodeBiasRef(kSat, kC2W)).column, 0);
    EXPECT_GE(view.find(satCodeBiasRef(kSat, kC2W)).column, 0);
    EXPECT_LT(view.find(satCodeBiasRef(kSat, kC1W)).column, 0);
}

TEST(RxSatCodeBiasRefTest, TheNetworkViewsHaveNoSuchColumn) {
    StationParams set;
    ASSERT_GE(gnss_test::bornParam(set, satCodeBiasRef(kSat, kC1W)), 0);
    const int untyped_before = set.untyped;
    // The family's form is refused by the station's set: not placed, counted as untyped, never found.
    EXPECT_LT(gnss_test::bornParam(set, rxSatCodeBiasRef(kSat, kC1W)), 0);
    EXPECT_EQ(set.untyped, untyped_before + 1);
    EXPECT_LT(findParam(set, rxSatCodeBiasRef(kSat, kC1W)), 0);
    EXPECT_GE(findParam(set, satCodeBiasRef(kSat, kC1W)), 0);
}
