/**
 * @file test_phase_colour_state.cpp
 * @brief Card LGM: the per-line-of-sight phase nuisance state's column family, its loader
 *        interlocks and its prior law (docs/refound/LGM_los_phase_nuisance_2026-10-01.md
 *        section 4.1).
 *
 * The state is a band-common, phase-only Gauss-Markov column per (station, satellite).
 * These cases guard the pieces that do not need a network run: the StateManager family
 * (allocation, idempotence, labels, index bookkeeping under removal, pruning, the recorded
 * elevation), the loader's refusals and acceptance on the committed sim golden, the knob
 * snapshot's stationary variance law, and the registry's new kind.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "network/state/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/NetworkParamRegistry.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

using namespace GnssProcessor;

namespace {

constexpr size_t kCore = 5;
constexpr double kPi = 3.14159265358979323846;

bool hasLabel(const StateManager& sm, const std::string& label) {
    for (const auto& l : sm.getStateLabels())
        if (l == label) return true;
    return false;
}

/// Load the committed sim golden with @p edit applied to its parsed JSON.
bool loadGoldenWith(const std::function<void(nlohmann::json&)>& edit, GnssConfig& cfg,
                    const std::string& tag) {
    const std::filesystem::path src = std::filesystem::path(GNSS_PROJECT_ROOT) / "config" /
                                      "golden" / "golden_sim_refound.json";
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("gnss_lgm_" + tag + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return ok;
}

} // namespace

TEST(PhaseColourState, StateFamilyAllocatesLabelsAndPrunes) {
    LogManager log;
    GnssConfig cfg;
    StateManager sm(kCore, log, cfg);
    const size_t n0 = sm.getStateSize();
    const size_t a = sm.addPhaseColourState("G05", 2.5e-5);
    const size_t b = sm.addPhaseColourState("G12", 2.5e-5);
    EXPECT_EQ(sm.addPhaseColourState("G05", 9.0), a);           // idempotent
    EXPECT_EQ(sm.getPhaseColourIndex("G05"), static_cast<int>(a));
    EXPECT_EQ(sm.getPhaseColourIndex("G12"), static_cast<int>(b));
    EXPECT_EQ(sm.getPhaseColourIndex("G30"), -1);
    EXPECT_EQ(sm.getStateSize(), n0 + 2);
    EXPECT_DOUBLE_EQ(sm.getStateVector()(static_cast<Eigen::Index>(a)), 0.0);   // seeded 0
    EXPECT_TRUE(hasLabel(sm, "PCOL:G05"));
    EXPECT_TRUE(hasLabel(sm, "PCOL:G12"));

    EXPECT_TRUE(std::isnan(sm.getPhaseColourElevation("G05")));
    sm.setPhaseColourElevation("G05", 37.5);
    EXPECT_DOUBLE_EQ(sm.getPhaseColourElevation("G05"), 37.5);

    // Pruning G05 (the lower index) shifts G12 down by one and forgets G05's elevation.
    sm.prunePhaseColourStatesExcept({"G12"});
    EXPECT_EQ(sm.getPhaseColourIndex("G05"), -1);
    EXPECT_EQ(sm.getPhaseColourIndex("G12"), static_cast<int>(a));
    EXPECT_EQ(sm.getStateSize(), n0 + 1);
    EXPECT_TRUE(std::isnan(sm.getPhaseColourElevation("G05")));
    EXPECT_FALSE(hasLabel(sm, "PCOL:G05"));
}

TEST(PhaseColourState, RemovingAnotherStateBelowShiftsTheColourIndex) {
    LogManager log;
    GnssConfig cfg;
    StateManager sm(kCore, log, cfg);
    const size_t n0 = sm.getStateSize();
    sm.addReceiverPhaseBiasState("GPS_RPB_L1C", 0.0);           // a lower dynamic index
    const size_t pc = sm.addPhaseColourState("G07", 2.5e-5);
    sm.pruneReceiverPhaseBiasStatesExcept({});
    EXPECT_EQ(sm.getPhaseColourIndex("G07"), static_cast<int>(pc) - 1);
    EXPECT_EQ(sm.getStateSize(), n0 + 1);
}

TEST(PhaseColourState, LoaderAcceptsTheGoldenPostureAndRefusesTheLevelVehicle) {
    GnssConfig on;
    ASSERT_TRUE(loadGoldenWith([](nlohmann::json& j) {
        j["network"]["phase_colour_state"] = true;
        j["network"]["phase_colour_tau_s"] = 200.0;
    }, on, "on"));
    EXPECT_TRUE(on.getNetworkConfig().phase_colour_state);
    EXPECT_DOUBLE_EQ(on.getNetworkConfig().phase_colour_tau_s, 200.0);
    EXPECT_DOUBLE_EQ(on.getNetworkConfig().phase_colour_sigma_zen_m, 0.005);   // default

    GnssConfig level;
    EXPECT_FALSE(loadGoldenWith([](nlohmann::json& j) {
        j["network"]["phase_colour_state"] = true;
        j["network"]["ambiguity_vehicle"] = "level";
        j["network"]["phase_datum_held_basis"] = false;
    }, level, "level"));

    GnssConfig bad_tau;
    EXPECT_FALSE(loadGoldenWith([](nlohmann::json& j) {
        j["network"]["phase_colour_state"] = true;
        j["network"]["phase_colour_tau_s"] = 0.0;
    }, bad_tau, "badtau"));

    GnssConfig off;
    ASSERT_TRUE(loadGoldenWith([](nlohmann::json&) {}, off, "off"));
    EXPECT_FALSE(off.getNetworkConfig().phase_colour_state);
}

TEST(PhaseColourState, StationaryVarianceFollowsTheCappedElevationLaw) {
    GnssConfig cfg;
    ASSERT_TRUE(loadGoldenWith([](nlohmann::json& j) {
        j["network"]["phase_colour_state"] = true;
    }, cfg, "law"));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    EXPECT_TRUE(k.pc_on);
    const double s0 = 0.005;
    EXPECT_NEAR(k.pcStationaryVar(90.0), s0 * s0, 1e-18);
    const double s30 = s0 / std::sin(30.0 * kPi / 180.0);
    EXPECT_NEAR(k.pcStationaryVar(30.0), s30 * s30, 1e-15);
    const double s20 = s0 / std::sin(20.0 * kPi / 180.0);
    EXPECT_NEAR(k.pcStationaryVar(10.0), s20 * s20, 1e-15);      // below the floor
    EXPECT_NEAR(k.pcStationaryVar(std::nan("")), s20 * s20, 1e-15);   // unknown: the floor

    const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
    const ParamSpec& pc = r.spec(NetworkParamKind::PhaseColour);
    EXPECT_TRUE(pc.carried);
    EXPECT_EQ(pc.q_rule, ParamQRule::GaussMarkov);
    EXPECT_DOUBLE_EQ(pc.q, s0 * s0);
    EXPECT_DOUBLE_EQ(pc.tau_s, 300.0);
    EXPECT_EQ(pc.route, ParamRoute::LocalBlock);
}
