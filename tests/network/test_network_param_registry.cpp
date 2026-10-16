/**
 * @file test_network_param_registry.cpp
 * @brief Card PRM phase 1: the parameter registry built from the committed goldens.
 *
 * The registry is the single process-noise source of the border kinds, so its values
 * must be the doubles the folds used before it existed (exact equality with the knob
 * snapshot, the byte-identity contract), and its descriptors must state the goldens'
 * armed model: the satellite clock anchored at card Q3's rate and loose at the legacy
 * rate, the phase and code biases at their pinned rates, the float ambiguities in the
 * station's whitened local block, the receiver code bias carried in that block (card EXL),
 * and the wet delay, slant ionosphere and receiver clock re-estimated every epoch.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "network/state/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/NetworkParamRegistry.hpp"
#include "logging/LogManager.hpp"

using namespace GnssProcessor;

namespace {

bool loadGolden(const std::string& bed, GnssConfig& cfg) {
    LogManager log;
    const std::filesystem::path p =
        std::filesystem::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
        ("golden_" + bed + "_refound.json");
    return cfg.loadFromJson(p, log);
}

/// The armed model both goldens share (one posture, two beds).
void expectGoldenModel(const std::string& bed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
    const NetworkConfig& nc = cfg.getNetworkConfig();
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);

    const ParamSpec& clk = r.spec(NetworkParamKind::SatClock);
    EXPECT_TRUE(clk.carried);
    EXPECT_DOUBLE_EQ(clk.q, 1e-3);
    EXPECT_EQ(clk.q_rule, ParamQRule::AnchorElapsed);
    EXPECT_DOUBLE_EQ(clk.q_anchored, 2.9e-8);
    EXPECT_DOUBLE_EQ(clk.var_floor_m2, 1e-5);
    EXPECT_EQ(clk.route, ParamRoute::DenseBorder);

    const ParamSpec& pb = r.spec(NetworkParamKind::SatPhaseBias);
    EXPECT_TRUE(pb.carried);
    EXPECT_DOUBLE_EQ(pb.q, 1e-12);
    EXPECT_EQ(pb.q_rule, ParamQRule::RandomWalk);
    EXPECT_TRUE(std::isnan(pb.q_anchored));
    EXPECT_EQ(pb.route, ParamRoute::DenseBorder);

    const ParamSpec& cb = r.spec(NetworkParamKind::SatCodeBias);
    EXPECT_TRUE(cb.carried);
    EXPECT_DOUBLE_EQ(cb.q, 1e-10);
    EXPECT_EQ(cb.route, ParamRoute::DenseBorder);

    EXPECT_FALSE(r.spec(NetworkParamKind::VtecSh).estimated);   // free slant, no SH
    EXPECT_EQ(r.spec(NetworkParamKind::VtecSh).route, ParamRoute::NotCarried);

    const ParamSpec& amb = r.spec(NetworkParamKind::Ambiguity);
    EXPECT_TRUE(amb.carried);
    EXPECT_DOUBLE_EQ(amb.q, 0.0);                     // no ambiguity process noise
    EXPECT_EQ(amb.route, ParamRoute::LocalBlock);     // the float vehicle's joint block

    EXPECT_FALSE(r.spec(NetworkParamKind::Zwd).carried);
    EXPECT_EQ(r.spec(NetworkParamKind::Zwd).route, ParamRoute::NotCarried);
    EXPECT_FALSE(r.spec(NetworkParamKind::SlantIono).carried);
    EXPECT_FALSE(r.spec(NetworkParamKind::RxClock).carried);
    EXPECT_FALSE(r.spec(NetworkParamKind::RxPhaseBias).estimated);

    const ParamSpec& rxcb = r.spec(NetworkParamKind::RxCodeBias);
    EXPECT_TRUE(rxcb.carried);
    EXPECT_EQ(rxcb.route, ParamRoute::LocalBlock);   // in the station block since card EXL

    EXPECT_FALSE(r.spec(NetworkParamKind::PhaseColour).estimated);   // card LGM, off
    EXPECT_EQ(r.spec(NetworkParamKind::PhaseColour).route, ParamRoute::NotCarried);
}

} // namespace

TEST(NetworkParamRegistry, SimGoldenStatesTheArmedModel) { expectGoldenModel("sim"); }

TEST(NetworkParamRegistry, FieldGoldenStatesTheArmedModel) { expectGoldenModel("field"); }

// The byte-identity contract of phase 1: every border q the folds, the mirror, the anchor
// and the coupling builder now read from the registry is the exact double they read from
// the knob snapshot before.
TEST(NetworkParamRegistry, BorderQIsTheKnobSnapshotsDouble) {
    for (const std::string bed : {"sim", "field"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        EXPECT_EQ(r.q(SharedParamKind::Clock), k.q_clock) << bed;
        EXPECT_EQ(r.q(SharedParamKind::PhaseBias), k.q_pb) << bed;
        EXPECT_EQ(r.q(SharedParamKind::CodeBias), k.q_cb) << bed;
    }
}

TEST(NetworkParamRegistry, TableHasOneLinePerKindAndTheJsonNamesEveryKind) {
    GnssConfig cfg;
    ASSERT_TRUE(loadGolden("sim", cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
    const auto t = r.modelTable();
    ASSERT_EQ(t.size(), kNetworkParamKindCount + 1);   // header + one line per kind
    const std::string j = r.modelJson();
    EXPECT_EQ(j.rfind("{\"record\":\"param_model\"", 0), 0u);
    for (std::size_t i = 0; i < kNetworkParamKindCount; ++i) {
        const auto kind = static_cast<NetworkParamKind>(i);
        EXPECT_NE(t[i + 1].find(paramKindName(kind)), std::string::npos);
        EXPECT_NE(j.find(std::string("\"kind\":\"") + paramKindName(kind) + "\""),
                  std::string::npos);
    }
}

TEST(NetworkParamRegistry, BorderKindMapping) {
    EXPECT_EQ(paramKindOf(SharedParamKind::Clock), NetworkParamKind::SatClock);
    EXPECT_EQ(paramKindOf(SharedParamKind::PhaseBias), NetworkParamKind::SatPhaseBias);
    EXPECT_EQ(paramKindOf(SharedParamKind::CodeBias), NetworkParamKind::SatCodeBias);
    EXPECT_EQ(paramKindOf(SharedParamKind::VtecSh), NetworkParamKind::VtecSh);
}
