/**
 * @file test_pivot_election.cpp
 * @brief The clock pivot's election by predicted station coverage (network.pivot_election).
 *
 * The ranking over plain scores, the predictor over synthetic circular orbits (an equatorial
 * ring with a fixed Sun, so every expected count is geometry that can be checked by hand), the
 * election rule inside NetworkSbasis (incumbent, standby, fresh election, health, failure),
 * the datum builder's counters, the per-epoch binding the two electing seams share, and the
 * loader's knob, refusals and defaults.
 */

#include <gtest/gtest.h>

#include "ConfigLoadCapture.hpp"

#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/solve/ClockDatum.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/PivotElection.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using TestSupport::contains;
using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

constexpr double kOrbitRadius = 26.56e6;   // m, a GNSS-like ring
constexpr double kOrbitPeriod = 43200.0;   // s
constexpr double kPi          = 3.14159265358979323846;
constexpr double kT0          = 2313.0 * 604800.0 + 3.0 * 86400.0;   // 2024-05-08 00:00 GPS

PivotCandidateScore score(const std::string& sat, long long samples, double umbra_in_s = -1.0,
                          bool has_orbit = true) {
    PivotCandidateScore s;
    s.sat             = sat;
    s.has_orbit       = has_orbit;
    s.station_samples = samples;
    s.umbra_in_s      = umbra_in_s;
    return s;
}

PivotRanking ranked(std::vector<PivotCandidateScore> table) {
    PivotRanking r;
    r.constellation = "G";
    r.samples       = 25;
    r.stations      = 10;
    r.table         = std::move(table);
    rankPivotCandidates(r);
    return r;
}

/// A satellite on an equatorial ring in the Earth-fixed frame, sampled every 300 s over
/// +-4 h about kT0.
std::vector<NetworkPivotCoverage::OrbitNode> ringNodes(double phi0) {
    std::vector<NetworkPivotCoverage::OrbitNode> nodes;
    for (double dt = -14400.0; dt <= 14400.0; dt += 300.0) {
        const double phi = phi0 + 2.0 * kPi * dt / kOrbitPeriod;
        nodes.push_back({kT0 + dt, Eigen::Vector3d(kOrbitRadius * std::cos(phi),
                                                   kOrbitRadius * std::sin(phi), 0.0)});
    }
    return nodes;
}

Eigen::Vector3d ringAt(double phi0, double dt) {
    const double phi = phi0 + 2.0 * kPi * dt / kOrbitPeriod;
    return Eigen::Vector3d(kOrbitRadius * std::cos(phi), kOrbitRadius * std::sin(phi), 0.0);
}

NetworkPivotCoverage::SunFn sunAlongX() {
    return [](double) { return Eigen::Vector3d(1.496e11, 0.0, 0.0); };
}

PivotCoverageParams params(double mask_deg = 10.0) {
    PivotCoverageParams p;
    p.horizon_s          = 7200.0;
    p.step_s             = 300.0;
    p.elevation_mask_deg = mask_deg;
    p.shadow_scale       = 1.0;
    return p;
}

/// One station on the equator at longitude 0, under the ring's phase 0 at kT0.
std::vector<Eigen::Vector3d> oneStationOnX() {
    return {Eigen::Vector3d(Constants::EARTH_RADIUS, 0.0, 0.0)};
}

}  // namespace

// ---- The ranking ---------------------------------------------------------------------

TEST(CoveragePivotRanking, LargestStationCountWins) {
    const PivotRanking r = ranked({score("G01", 100), score("G14", 240), score("G22", 180)});
    EXPECT_EQ(r.sat, "G14");
    EXPECT_FALSE(r.all_shadowed);
}

TEST(CoveragePivotRanking, TieGoesToTheLexicallyFirstSatellite) {
    const PivotRanking r = ranked({score("G22", 200), score("G14", 200), score("G30", 200)});
    EXPECT_EQ(r.sat, "G14");
}

TEST(CoveragePivotRanking, SatelliteWithoutAnOrbitIsNeverElected) {
    const PivotRanking r = ranked({score("G01", 0, -1.0, /*has_orbit=*/false), score("G02", 3)});
    EXPECT_EQ(r.sat, "G02");
    const PivotRanking none = ranked({score("G01", 0, -1.0, false), score("G02", 0, -1.0, false)});
    EXPECT_TRUE(none.sat.empty());
    EXPECT_EQ(none.winner(), nullptr);
}

// A satellite that stays out of the umbra for the whole horizon beats one with more stations
// that enters it.
TEST(CoveragePivotRanking, UmbraFreeBeatsHigherCoverageThatEntersTheUmbra) {
    const PivotRanking r = ranked({score("G05", 300, /*umbra_in_s=*/1800.0), score("G09", 120)});
    EXPECT_EQ(r.sat, "G09");
    EXPECT_FALSE(r.all_shadowed);
}

// When every candidate enters the umbra, the one that enters it last wins; coverage decides
// only between equal entries.
TEST(CoveragePivotRanking, WhenAllEnterTheUmbraTheLastEntryWins) {
    const PivotRanking r = ranked({score("G05", 300, 1800.0), score("G09", 120, 5400.0),
                                   score("G11", 500, 5400.0)});
    EXPECT_TRUE(r.all_shadowed);
    EXPECT_EQ(r.sat, "G11");
}

TEST(CoveragePivotRanking, DescriptionNamesThePickAndTheRunnerUp) {
    const PivotRanking r = ranked({score("G01", 100), score("G14", 250), score("G22", 200)});
    const std::string d = r.describe();
    EXPECT_NE(d.find("G14 elected at 10.0"), std::string::npos) << d;
    EXPECT_NE(d.find("runner-up G22"), std::string::npos) << d;
    EXPECT_NE(d.find("3 candidate(s)"), std::string::npos) << d;
}

// ---- The predictor -------------------------------------------------------------------

TEST(CoveragePivotPredictor, SampleCountFollowsHorizonAndStep) {
    PivotCoverageParams p = params();
    EXPECT_EQ(p.sampleCount(), 25u);
    p.horizon_s = 600.0; p.step_s = 300.0;
    EXPECT_EQ(p.sampleCount(), 3u);
    p.horizon_s = 0.0;
    EXPECT_EQ(p.sampleCount(), 1u);
}

TEST(CoveragePivotPredictor, InterpolatesTheOrbitBetweenNodes) {
    const NetworkPivotCoverage cov(params(), oneStationOnX(), {{"G01", ringNodes(0.3)}}, sunAlongX());
    for (double dt : {-3000.0, -1234.5, 0.0, 150.0, 2999.0, 7211.0}) {
        const auto p = cov.positionAt("G01", kT0 + dt);
        ASSERT_TRUE(p.has_value()) << dt;
        EXPECT_LT((*p - ringAt(0.3, dt)).norm(), 1.0) << "dt " << dt;
    }
}

TEST(CoveragePivotPredictor, NoPositionOutsideTheProductOrForAnUnknownSatellite) {
    const NetworkPivotCoverage cov(params(), oneStationOnX(), {{"G01", ringNodes(0.0)}}, sunAlongX());
    EXPECT_FALSE(cov.positionAt("G01", kT0 + 20000.0).has_value());
    EXPECT_FALSE(cov.positionAt("G01", kT0 - 20000.0).has_value());
    EXPECT_FALSE(cov.positionAt("G99", kT0).has_value());
}

// One station on +X. A ring satellite overhead at kT0 (phase 0) stays within about 60 degrees
// of the station for the 7200 s horizon (elevation 16.7 deg at the end), so it is counted at
// all 25 samples; the one at the antipode (phase pi) is below the horizon throughout.
TEST(CoveragePivotPredictor, CountsTheSamplesAboveTheMask) {
    const NetworkPivotCoverage cov(params(10.0), oneStationOnX(),
                                   {{"G01", ringNodes(0.0)}, {"G02", ringNodes(kPi)}}, sunAlongX());
    const PivotRanking r = cov.rank(kT0, "G", {"G01", "G02"});
    ASSERT_EQ(r.table.size(), 2u);
    EXPECT_EQ(r.samples, 25u);
    EXPECT_EQ(r.stations, 1u);
    EXPECT_TRUE(r.table[0].has_orbit);
    EXPECT_EQ(r.table[0].station_samples, 25);
    EXPECT_EQ(r.table[1].station_samples, 0);
    EXPECT_EQ(r.sat, "G01");
    EXPECT_DOUBLE_EQ(r.meanStations(r.table[0]), 1.0);
    // A 20 degree mask loses the tail of the horizon.
    const NetworkPivotCoverage high(params(20.0), oneStationOnX(), {{"G01", ringNodes(0.0)}},
                                    sunAlongX());
    const PivotRanking rh = high.rank(kT0, "G", {"G01"});
    EXPECT_LT(rh.table[0].station_samples, 25);
    EXPECT_GE(rh.table[0].station_samples, 20);
}

// With the Sun on +X, a ring satellite is in the cylindrical umbra from the phase where it is
// behind the Earth within the Earth's radius of the axis: pi - asin(R_e / r), about 2.90 rad.
// Starting at phase pi - 0.5 it gets there after about 1.8 ks; one at phase 0 does not within
// the 7.2 ks horizon.
TEST(CoveragePivotPredictor, FindsTheUmbraEntryInsideTheHorizon) {
    const NetworkPivotCoverage cov(params(), oneStationOnX(),
                                   {{"G01", ringNodes(kPi - 0.5)}, {"G02", ringNodes(0.0)}},
                                   sunAlongX());
    const PivotRanking r = cov.rank(kT0, "G", {"G01", "G02"});
    ASSERT_EQ(r.table.size(), 2u);
    EXPECT_TRUE(r.table[0].entersUmbra());
    EXPECT_GT(r.table[0].umbra_in_s, 1500.0);
    EXPECT_LT(r.table[0].umbra_in_s, 2100.0);
    EXPECT_FALSE(r.table[1].entersUmbra());
    EXPECT_EQ(r.sat, "G02");   // out of the umbra for the horizon, though G01 is the nearer the Sun now
}

TEST(CoveragePivotPredictor, MemoGivesTheSameRankingToTheSecondCaller) {
    const NetworkPivotCoverage cov(params(), oneStationOnX(), {{"G01", ringNodes(0.0)}}, sunAlongX());
    const PivotRanking a = cov.rank(kT0, "G", {"G01"});
    const PivotRanking b = cov.rank(kT0, "G", {"G01"});
    EXPECT_EQ(a.sat, b.sat);
    ASSERT_NE(cov.last("G"), nullptr);
    EXPECT_EQ(cov.last("G")->sat, "G01");
    EXPECT_EQ(cov.last("E"), nullptr);
}

// ---- The election rule ---------------------------------------------------------------

namespace {

/// A ranker that records its calls and returns a fixed answer.
struct Probe {
    std::string answer;
    mutable int calls{0};
    mutable std::vector<std::string> last_pool;
    NetworkSbasis::PivotRanker ranker() const {
        return [this](const std::vector<std::string>& pool) {
            ++calls;
            last_pool = pool;
            return answer;
        };
    }
};

}  // namespace

TEST(CoveragePivotRule, RegisteredHealthyIncumbentStaysAndTheRankerIsNotCalled) {
    Probe probe{"G03"};
    const auto ranker = probe.ranker();
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02", "G03"}, "", "G02", nullptr, &ranker);
    EXPECT_EQ(p.sat, "G02");
    EXPECT_FALSE(p.ranked);
    EXPECT_EQ(probe.calls, 0);
}

TEST(CoveragePivotRule, DeclaredPinStillWins) {
    Probe probe{"G03"};
    const auto ranker = probe.ranker();
    const std::set<std::string> bad = {"G01"};
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02", "G03"}, "G01", "G02", &bad, &ranker);
    EXPECT_EQ(p.sat, "G01");
    EXPECT_EQ(probe.calls, 0);
}

TEST(CoveragePivotRule, NoIncumbentElectsByTheRanker) {
    Probe probe{"G03"};
    const auto ranker = probe.ranker();
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02", "G03"}, "", "", nullptr, &ranker);
    EXPECT_EQ(p.sat, "G03");
    EXPECT_TRUE(p.ranked);
    EXPECT_FALSE(p.rank_failed);
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.last_pool, (std::vector<std::string>{"G01", "G02", "G03"}));
}

// The incumbent left the registered set: a fresh election, not the lexical first.
TEST(CoveragePivotRule, AbsentIncumbentForcesAFreshElection) {
    Probe probe{"G03"};
    const auto ranker = probe.ranker();
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02", "G03"}, "", "G09", nullptr, &ranker);
    EXPECT_EQ(p.sat, "G03");
    EXPECT_TRUE(p.ranked);
}

TEST(CoveragePivotRule, UnhealthyIncumbentIsReplacedAndTheRankerSeesOnlyHealthySatellites) {
    Probe probe{"G03"};
    const auto ranker = probe.ranker();
    const std::set<std::string> bad = {"G01", "G02"};
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02", "G03", "G04"}, "", "G01", &bad, &ranker);
    EXPECT_EQ(p.sat, "G03");
    EXPECT_TRUE(p.ranked);
    EXPECT_TRUE(p.health_skipped);
    EXPECT_FALSE(p.health_fallback);
    EXPECT_EQ(probe.last_pool, (std::vector<std::string>{"G03", "G04"}));
}

TEST(CoveragePivotRule, NoHealthySatelliteRanksAmongAllAndIsCounted) {
    Probe probe{"G02"};
    const auto ranker = probe.ranker();
    const std::set<std::string> bad = {"G01", "G02"};
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02"}, "", "", &bad, &ranker);
    EXPECT_EQ(p.sat, "G02");
    EXPECT_TRUE(p.health_fallback);
    EXPECT_FALSE(p.health_skipped);
    EXPECT_EQ(probe.last_pool, (std::vector<std::string>{"G01", "G02"}));
}

TEST(CoveragePivotRule, StandbyHoldsUntilItLeavesOrTurnsUnhealthy) {
    Probe probe{"G04"};
    const auto ranker = probe.ranker();
    const std::vector<std::string> sats = {"G01", "G02", "G03", "G04"};
    const auto kept = NetworkSbasis::electClockPivot(sats, "", "", nullptr, &ranker, "G03");
    EXPECT_EQ(kept.sat, "G03");
    EXPECT_TRUE(kept.standby_kept);
    EXPECT_EQ(probe.calls, 0);
    // An incumbent that is registered and healthy comes before the standby.
    const auto inc = NetworkSbasis::electClockPivot(sats, "", "G02", nullptr, &ranker, "G03");
    EXPECT_EQ(inc.sat, "G02");
    EXPECT_FALSE(inc.standby_kept);
    // The standby turned unhealthy: elected afresh.
    const std::set<std::string> bad = {"G03"};
    const auto fresh = NetworkSbasis::electClockPivot(sats, "", "", &bad, &ranker, "G03");
    EXPECT_EQ(fresh.sat, "G04");
    EXPECT_TRUE(fresh.ranked);
    // The standby left the registered set: elected afresh.
    const auto gone = NetworkSbasis::electClockPivot({"G01", "G04"}, "", "", nullptr, &ranker, "G03");
    EXPECT_EQ(gone.sat, "G04");
    EXPECT_TRUE(gone.ranked);
}

TEST(CoveragePivotRule, UnusableRankerAnswerLeavesTheFirstCandidateAndIsCounted) {
    Probe empty{""};
    const auto r1 = empty.ranker();
    const auto none = NetworkSbasis::electClockPivot({"G01", "G02"}, "", "", nullptr, &r1);
    EXPECT_EQ(none.sat, "G01");
    EXPECT_TRUE(none.rank_failed);
    EXPECT_FALSE(none.ranked);
    Probe stranger{"G77"};
    const auto r2 = stranger.ranker();
    const auto foreign = NetworkSbasis::electClockPivot({"G01", "G02"}, "", "", nullptr, &r2);
    EXPECT_EQ(foreign.sat, "G01");
    EXPECT_TRUE(foreign.rank_failed);
}

// ---- The datum builder ---------------------------------------------------------------

TEST(CoveragePivotDatum, ElectionIsPerConstellationAndCountedInThePlan) {
    NetworkEstimator est;
    for (const char* s : {"G01", "G02", "G14", "E02", "E03", "E11"}) est.registerClock(s);
    NetworkSbasis sb;
    SbasisDeclaration decl;
    const std::vector<std::string> sats = {"G01", "G02", "G14", "E02", "E03", "E11"};
    std::map<std::string, std::vector<std::string>> seen;
    NetworkSbasis::PivotElection election;
    election.rank = [&seen](const std::string& c, const std::vector<std::string>& cands) {
        seen[c] = cands;
        return c == "G" ? std::string("G14") : std::string("E11");
    };
    const std::map<std::string, std::string> incumbent;   // ignition
    const std::set<std::string> bad = {"G01"};
    const auto plan = sb.buildSharedDatum(est, sats, decl, &incumbent, &bad, &election);
    EXPECT_EQ(plan.clock_pivot.at("G"), "G14");
    EXPECT_EQ(plan.clock_pivot.at("E"), "E11");
    EXPECT_EQ(plan.pivot_ranked, 2);
    EXPECT_EQ(plan.pivot_rank_failures, 0);
    EXPECT_EQ(plan.pivot_standby_kept, 0);
    EXPECT_EQ(plan.pivot_ranked_consts, (std::set<std::string>{"E", "G"}));
    EXPECT_EQ(plan.pivot_health_skips, 1);   // G01 passed over
    EXPECT_EQ(seen.at("G"), (std::vector<std::string>{"G02", "G14"}));
    EXPECT_EQ(plan.declared_dimension, 2);
}

TEST(CoveragePivotDatum, HeldPicksSkipTheRankerAndAreCounted) {
    NetworkEstimator est;
    for (const char* s : {"G01", "G02", "G14"}) est.registerClock(s);
    NetworkSbasis sb;
    SbasisDeclaration decl;
    int calls = 0;
    NetworkSbasis::PivotElection election;
    election.rank = [&calls](const std::string&, const std::vector<std::string>&) {
        ++calls;
        return std::string("G14");
    };
    const std::map<std::string, std::string> standby = {{"G", "G02"}};
    election.standby = &standby;
    const std::map<std::string, std::string> none;
    const auto held = sb.buildSharedDatum(est, {"G01", "G02", "G14"}, decl, &none, nullptr, &election);
    EXPECT_EQ(held.clock_pivot.at("G"), "G02");
    EXPECT_EQ(held.pivot_standby_kept, 1);
    EXPECT_EQ(held.pivot_ranked, 0);
    EXPECT_EQ(calls, 0);
    const std::map<std::string, std::string> incumbent = {{"G", "G01"}};
    const auto inc = sb.buildSharedDatum(est, {"G01", "G02", "G14"}, decl, &incumbent, nullptr, &election);
    EXPECT_EQ(inc.clock_pivot.at("G"), "G01");
    EXPECT_EQ(inc.pivot_standby_kept, 0);
}

// Without an election the plan is the plan it was.
TEST(CoveragePivotDatum, NoElectionLeavesTheLexicalRule) {
    NetworkEstimator est;
    for (const char* s : {"G01", "G02"}) est.registerClock(s);
    NetworkSbasis sb;
    SbasisDeclaration decl;
    const auto plan = sb.buildSharedDatum(est, {"G01", "G02"}, decl);
    EXPECT_EQ(plan.clock_pivot.at("G"), "G01");
    EXPECT_EQ(plan.pivot_ranked, 0);
    EXPECT_TRUE(plan.pivot_ranked_consts.empty());
}

// ---- The per-epoch binding -----------------------------------------------------------

TEST(CoveragePivotEpoch, OffGivesNoElection) {
    const LoadedConfig r = loadNetworkConfigCapturing("piv_off", "");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPivotElection(), 0);
    NetworkRunContext ctx(r.config, r.config.getNetworkConfig());
    NetworkSetupResult setup;
    const NetworkPivotElectionEpoch e(ctx, setup, GnssTime{2313, 3.0 * 86400.0});
    EXPECT_EQ(e.get(), nullptr);
}

TEST(CoveragePivotEpoch, OnBindsThePredictorAndTheStandbyRows) {
    const LoadedConfig r = loadNetworkConfigCapturing(
        "piv_on", "    \"sticky_pivot\": true,\n    \"pivot_election\": \"coverage\",\n");
    ASSERT_TRUE(r.ok) << r.text;
    NetworkRunContext ctx(r.config, r.config.getNetworkConfig());
    NetworkSetupResult setup;
    setup.pivot_coverage = std::make_unique<NetworkPivotCoverage>(
        params(), oneStationOnX(),
        std::map<std::string, std::vector<NetworkPivotCoverage::OrbitNode>>{
            {"G01", ringNodes(0.0)}, {"G02", ringNodes(kPi)}},
        sunAlongX());
    net::NetworkEntities& entities = ctx.entities;
    net::mergeLetterMap(entities, &net::ConstellationState::elected_pivot,
                        std::map<std::string, std::string>{{"G", "G02"}});

    const NetworkPivotElectionEpoch e(ctx, setup, GnssTime{2313, 3.0 * 86400.0});
    ASSERT_NE(e.get(), nullptr);
    ASSERT_NE(e.get()->standby, nullptr);
    EXPECT_EQ(e.get()->standby->at("G"), "G02");
    EXPECT_EQ(e.get()->rank("G", {"G01", "G02"}), "G01");   // G01 is the one over the station
    // Both seams of one epoch share the ranking.
    ASSERT_NE(setup.pivot_coverage->last("G"), nullptr);
    EXPECT_EQ(setup.pivot_coverage->last("G")->sat, "G01");

    // With no predictor the ranker names nobody, so the rule falls back (counted).
    NetworkSetupResult bare;
    const NetworkPivotElectionEpoch e2(ctx, bare, GnssTime{2313, 3.0 * 86400.0});
    ASSERT_NE(e2.get(), nullptr);
    EXPECT_TRUE(e2.get()->rank("G", {"G01"}).empty());
}

// ---- The loader ----------------------------------------------------------------------

TEST(CoveragePivotConfig, DefaultsAreOffWithTheStatedHorizonAndStep) {
    const LoadedConfig r = loadNetworkConfigCapturing("piv_default", "");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPivotElection(), 0);
    EXPECT_DOUBLE_EQ(r.config.networkPivotElectionHorizonS(), 7200.0);
    EXPECT_DOUBLE_EQ(r.config.networkPivotElectionStepS(), 300.0);
    NetworkRunContext ctx(r.config, r.config.getNetworkConfig());
    EXPECT_EQ(ctx.knobs.pivot_election, 0);
}

TEST(CoveragePivotConfig, CoverageIsReadIntoTheKnobs) {
    const LoadedConfig r = loadNetworkConfigCapturing(
        "piv_cov",
        "    \"sticky_pivot\": true,\n"
        "    \"pivot_election\": \"coverage\",\n"
        "    \"pivot_election_horizon_s\": 3600,\n"
        "    \"pivot_election_step_s\": 120,\n");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPivotElection(), 1);
    NetworkRunContext ctx(r.config, r.config.getNetworkConfig());
    EXPECT_EQ(ctx.knobs.pivot_election, 1);
    EXPECT_DOUBLE_EQ(ctx.knobs.pivot_election_horizon_s, 3600.0);
    EXPECT_DOUBLE_EQ(ctx.knobs.pivot_election_step_s, 120.0);
    EXPECT_TRUE(contains(r.text, "clock pivot election by coverage ON")) << r.text;
}

TEST(CoveragePivotConfig, RefusesWithoutTheStickyPivot) {
    const LoadedConfig r = loadNetworkConfigCapturing(
        "piv_nosticky", "    \"pivot_election\": \"coverage\",\n");
    EXPECT_FALSE(r.ok);
}

TEST(CoveragePivotConfig, RefusesAnUnknownModeAndAHorizonBelowTheStep) {
    const LoadedConfig bad_mode = loadNetworkConfigCapturing(
        "piv_mode", "    \"sticky_pivot\": true,\n    \"pivot_election\": \"best\",\n");
    EXPECT_FALSE(bad_mode.ok);
    const LoadedConfig bad_span = loadNetworkConfigCapturing(
        "piv_span",
        "    \"sticky_pivot\": true,\n"
        "    \"pivot_election\": \"coverage\",\n"
        "    \"pivot_election_horizon_s\": 100,\n"
        "    \"pivot_election_step_s\": 300,\n");
    EXPECT_FALSE(bad_span.ok);
}

// The fingerprint carries the election, so a snapshot written with it cannot resume without.
TEST(CoveragePivotConfig, FingerprintDiffersWithTheElection) {
    const LoadedConfig off = loadNetworkConfigCapturing("piv_fp_off", "    \"sticky_pivot\": true,\n");
    const LoadedConfig on  = loadNetworkConfigCapturing(
        "piv_fp_on", "    \"sticky_pivot\": true,\n    \"pivot_election\": \"coverage\",\n");
    ASSERT_TRUE(off.ok && on.ok);
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    NetworkRunContext c_off(off.config, off.config.getNetworkConfig());
    NetworkRunContext c_on(on.config, on.config.getNetworkConfig());
    c_off.carry.snap_path = "state_snapshot.snap";
    c_on.carry.snap_path  = "state_snapshot.snap";
    networkSnapshotFingerprint(c_off, off.config, st);
    networkSnapshotFingerprint(c_on, on.config, st);
    EXPECT_NE(c_off.carry.live_fingerprint, c_on.carry.live_fingerprint);
}

}  // namespace GnssProcessor
