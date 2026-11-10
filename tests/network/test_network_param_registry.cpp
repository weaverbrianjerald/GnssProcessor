/**
 * @file test_network_param_registry.cpp
 * @brief The parameter registry built from the committed goldens.
 *
 * The registry is the single process-noise source of the border kinds, so its values
 * must be the doubles the folds used before it existed (exact equality with the knob
 * snapshot, the byte-identity contract), and its descriptors must state the goldens'
 * armed model: the satellite clock anchored at the clock anchor's rate and loose at the legacy
 * rate, the phase and code biases at their pinned rates, the float ambiguities in the
 * station's whitened local block, the receiver code bias, the wet delay and the per-LOS
 * phase colour carried in that block (the wet delay and the colour armed on 2026-11-09,
 * card ZTL section 8), and the slant ionosphere and receiver clock re-estimated every
 * epoch.  A golden with those two keys removed re-estimates the wet delay and has no
 * colour column.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/ParamRegistry.hpp"
#include "logging/LogManager.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

bool loadGolden(const std::string& bed, GnssConfig& cfg) {
    LogManager log;
    const std::filesystem::path p =
        std::filesystem::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
        ("golden_" + bed + "_refound.json");
    return cfg.loadFromJson(p, log);
}

/// The golden of @p bed with the wet-delay link and the phase colour state removed (the
/// posture before 2026-11-09), loaded into @p cfg.
bool loadGoldenPairOff(const std::string& bed, GnssConfig& cfg) {
    const std::filesystem::path src = std::filesystem::path(GNSS_PROJECT_ROOT) / "config" /
                                      "golden" / ("golden_" + bed + "_refound.json");
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    for (const char* key : {"carry_zwd", "carry_zwd_in_block", "zwd_carry_q", "phase_colour_state"})
        j["network"].erase(key);
    const std::filesystem::path p = gnss_test::tempDir() / ("registry_pair_off_" + bed + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return ok;
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

    const ParamSpec& zwd = r.spec(NetworkParamKind::Zwd);   // the wet delay's time link
    EXPECT_TRUE(zwd.carried);
    EXPECT_DOUBLE_EQ(zwd.q, 5e-8);
    EXPECT_EQ(zwd.q_rule, ParamQRule::RandomWalk);
    EXPECT_EQ(zwd.route, ParamRoute::LocalBlock);         // in the station block
    EXPECT_FALSE(r.spec(NetworkParamKind::SlantIono).carried);
    EXPECT_FALSE(r.spec(NetworkParamKind::RxClock).carried);
    EXPECT_FALSE(r.spec(NetworkParamKind::RxPhaseBias).estimated);

    const ParamSpec& rxcb = r.spec(NetworkParamKind::RxCodeBias);
    EXPECT_TRUE(rxcb.carried);
    EXPECT_EQ(rxcb.route, ParamRoute::LocalBlock);   // in the station block

    const ParamSpec& pc = r.spec(NetworkParamKind::PhaseColour);   // the per-LOS phase colour
    EXPECT_TRUE(pc.estimated);
    EXPECT_TRUE(pc.carried);
    EXPECT_EQ(pc.q_rule, ParamQRule::GaussMarkov);
    EXPECT_DOUBLE_EQ(pc.q, 0.005 * 0.005);                // stationary variance at the zenith
    EXPECT_DOUBLE_EQ(pc.tau_s, 300.0);
    EXPECT_EQ(pc.route, ParamRoute::LocalBlock);
}

} // namespace

TEST(NetworkParamRegistry, SimGoldenStatesTheArmedModel) { expectGoldenModel("sim"); }

TEST(NetworkParamRegistry, FieldGoldenStatesTheArmedModel) { expectGoldenModel("field"); }

// The off path: with the two keys removed the wet delay is re-estimated every epoch and there
// is no colour column, on both beds.
TEST(NetworkParamRegistry, GoldensWithThePairOffReestimateTheWetDelay) {
    for (const std::string bed : {"sim", "field"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGoldenPairOff(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        EXPECT_FALSE(r.spec(NetworkParamKind::Zwd).carried) << bed;
        EXPECT_EQ(r.spec(NetworkParamKind::Zwd).q_rule, ParamQRule::None) << bed;
        EXPECT_EQ(r.spec(NetworkParamKind::Zwd).route, ParamRoute::NotCarried) << bed;
        EXPECT_FALSE(r.spec(NetworkParamKind::PhaseColour).estimated) << bed;
        EXPECT_EQ(r.spec(NetworkParamKind::PhaseColour).route, ParamRoute::NotCarried) << bed;
        EXPECT_TRUE(r.spec(NetworkParamKind::Ambiguity).carried) << bed;   // the vehicle stays
    }
}

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
