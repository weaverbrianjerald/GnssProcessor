/**
 * @file test_net_small_defects.cpp
 * @brief Three small network-producer defects, each pinned by its own suite.
 *
 *  - PbHoldDecommonedStepTest: the de-commoned satellite clock step the phase-bias hold records
 *    at a coupled mark takes one median per constellation, as the hold's own transfer does, not
 *    one median across GPS and Galileo.
 *  - StashHardRefloatTest: a hard clock refloat (network.clock_monitor_reinit_sigma_m <= 0)
 *    leaves a flagged satellite's clock, phase-bias and code-bias entries out of the stash;
 *    the response table counts each entry it left out, and only an entry the stash would have
 *    kept.
 *  - StashStatusSeedMirrorTest: the epoch-tail snapshots of the carry LS-VCE monitor and the
 *    prior-residual census skip every parked entry; a readmit seed that the D2 fold applied is
 *    one of them. The seeds are counted either way, and network.readmit_seed_mirrors takes
 *    them as rows at the seed gap.
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Responses.hpp"
#include "network/carry/CarryVce.hpp"
#include "network/diag/StashStatus.hpp"
#include "network/driver/Epoch.hpp"
#include "network/emit/PbHold.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/state/CarryDisposition.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("nsd_test_null", sink);
}

/// A minimal loadable network configuration with @p extra spliced into the network block.
std::string networkConfig(const std::string& extra) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"filter\": { \"process_noise\": { \"sat_code_bias_q\": 1e-4 } },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"trace_events\": true,\n")
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load @p extra into @p cfg through a temporary file.
bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log,
              const std::string& tag) {
    const fs::path p = gnss_test::tempDir() / ("nsd_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << networkConfig(extra);
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

// ---------------------------------------------------------------------------------------
// The de-commoned clock step of the phase-bias hold
// ---------------------------------------------------------------------------------------

struct StepSat {
    std::string sat;
    double      clk;
    bool        has_clock{true};
};

NetworkSolutionEpoch stepEpoch(const std::vector<StepSat>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{2313, 259200.0};
    for (const auto& x : sats) {
        NetworkSatSolution s;
        s.sat = x.sat;
        s.has_clock = x.has_clock;
        s.clock_c0_m = x.clk;
        sol.sats.push_back(s);
    }
    return sol;
}

}  // namespace

// Three GPS satellites step by 0.1, 0.3 and 0.2 m and three Galileo satellites by 0.5 m each:
// each satellite's step is read against its own constellation's median (0.2 m and 0.5 m).
// One median across all six steps would be 0.4 m and would put -0.3 m on a GPS satellite and
// +0.1 m on every Galileo satellite, which is the difference of the two common modes.
TEST(PbHoldDecommonedStepTest, TheMedianIsTakenPerConstellation) {
    const std::map<std::string, double> prev{
        {"G01", 1.0}, {"G02", 2.0}, {"G03", 3.0}, {"E11", 5.0}, {"E12", 6.0}, {"E24", 7.0}};
    const auto sol = stepEpoch({{"G01", 1.1}, {"G02", 2.3}, {"G03", 3.2},
                                {"E11", 5.5}, {"E12", 6.5}, {"E24", 7.5}});
    const auto d = pbHoldDecommonedClockStep(sol, prev);
    ASSERT_EQ(d.size(), 6u);
    EXPECT_NEAR(d.at("G01"), -0.1, 1e-12);
    EXPECT_NEAR(d.at("G02"),  0.1, 1e-12);
    EXPECT_NEAR(d.at("G03"),  0.0, 1e-12);
    EXPECT_NEAR(d.at("E11"),  0.0, 1e-12);
    EXPECT_NEAR(d.at("E12"),  0.0, 1e-12);
    EXPECT_NEAR(d.at("E24"),  0.0, 1e-12);
}

// A single constellation reads exactly as the one median did: the median of the steps removed,
// an even count taking the mean of the two middle values.
TEST(PbHoldDecommonedStepTest, OneConstellationIsTheSingleMedianStep) {
    const std::map<std::string, double> prev{{"G01", 1.0}, {"G02", 2.0}, {"G03", 3.0}, {"G04", 4.0}};
    const auto odd = pbHoldDecommonedClockStep(
        stepEpoch({{"G01", 1.1}, {"G02", 2.3}, {"G03", 3.2}}), prev);
    EXPECT_NEAR(odd.at("G01"), -0.1, 1e-12);
    EXPECT_NEAR(odd.at("G02"),  0.1, 1e-12);
    EXPECT_NEAR(odd.at("G03"),  0.0, 1e-12);
    // Steps 0.1, 0.3, 0.2 and 0.6: the middle values are 0.2 and 0.3, so the median is 0.25.
    const auto even = pbHoldDecommonedClockStep(
        stepEpoch({{"G01", 1.1}, {"G02", 2.3}, {"G03", 3.2}, {"G04", 4.6}}), prev);
    EXPECT_NEAR(even.at("G01"), -0.15, 1e-12);
    EXPECT_NEAR(even.at("G04"),  0.35, 1e-12);
}

// A satellite with no previous clock, no clock now, or a step that is not finite is absent
// and does not enter its constellation's median.
TEST(PbHoldDecommonedStepTest, SatellitesWithoutAStepAreAbsentAndDoNotMoveTheMedian) {
    const std::map<std::string, double> prev{
        {"G01", 1.0}, {"G02", 2.0}, {"G03", 3.0}, {"E11", 5.0},
        {"G05", std::numeric_limits<double>::quiet_NaN()}};
    const auto sol = stepEpoch({{"G01", 1.1}, {"G02", 2.3}, {"G03", 3.2},
                                {"G04", 9.0},                       // no previous clock
                                {"G06", 9.0, /*has_clock=*/false},  // no clock now
                                {"G05", 9.0},                       // previous clock not finite
                                {"E11", 5.5}});
    const auto d = pbHoldDecommonedClockStep(sol, prev);
    EXPECT_EQ(d.size(), 4u);
    EXPECT_EQ(d.count("G04"), 0u);
    EXPECT_EQ(d.count("G05"), 0u);
    EXPECT_EQ(d.count("G06"), 0u);
    EXPECT_NEAR(d.at("G01"), -0.1, 1e-12);   // the GPS median stays 0.2 m
    EXPECT_NEAR(d.at("E11"),  0.0, 1e-12);   // a lone Galileo satellite is its own median
}

namespace {

// ---------------------------------------------------------------------------------------
// The hard clock refloat at the stash
// ---------------------------------------------------------------------------------------

/// A solved shared estimate over clocks G01 and G02, code biases G01 and G02 on C1C and phase
/// biases on L1C, with a prior on every column (so the stash reads a full-rank solve).
struct StashFixture {
    LogManager                       logman;
    GnssConfig                       cfg;
    std::unique_ptr<NetworkTestRun>  fx;
    Eigen::VectorXd                  x_s;
    NetworkSbasis::Plan              plan;
    std::vector<std::string>         clocks{"G01", "G02"};
    std::set<std::string>            phase_keys;
    std::vector<std::pair<std::string, std::string>> code_keys{{"G01", "C1C"}, {"G02", "C1C"}};

    explicit StashFixture(const std::string& extra, const std::string& tag) {
        EXPECT_TRUE(loadWith(extra, cfg, logman, tag));
        const fs::path dir = gnss_test::tempDir() / ("nsd_stash_" + tag);
        fs::create_directories(dir);
        fx = std::make_unique<NetworkTestRun>(cfg, nullLogger(), dir);
        NetworkEstimator& est = fx->est();
        std::vector<NetworkEstimator::SharedPrior> pri;
        const auto prior = [&](SharedParamKind kind, const std::string& sat,
                               const std::string& sig, double value) {
            NetworkEstimator::SharedPrior p;
            p.kind = kind; p.sat = sat; p.signal = sig; p.value = value; p.info = 100.0;
            pri.push_back(p);
        };
        double v = 0.1;
        for (const std::string& sat : clocks) {
            est.registerClock(sat);
            prior(SharedParamKind::Clock, sat, "", v);
            v += 0.1;
        }
        for (const auto& [sat, sig] : code_keys) {
            est.registerCodeBias(sat, sig);
            prior(SharedParamKind::CodeBias, sat, sig, v);
            v += 0.1;
        }
        for (const std::string& sat : clocks) {
            est.registerPhaseBias(sat, "L1C");
            prior(SharedParamKind::PhaseBias, sat, "L1C", v);
            phase_keys.insert(StateKeys::makeSatPhaseBiasKey(sat, "L1C"));
            v += 0.1;
        }
        est.beginEpoch();
        est.addSharedPrior(pri);
        x_s = est.solve({});
    }

    /// The stash for the epoch, flagging @p flagged as the clock monitor's refloat set.
    void stash(const std::set<std::string>& flagged) {
        networkStashCarry(fx->ctx(), cfg, fx->traces(), GnssTime{2313, 259230.0}, fx->est(),
                          x_s, plan, nullptr, nullptr, flagged, clocks, phase_keys, code_keys,
                          EpochEvents{}, nullLogger());
    }

    long long count(ResponseSite s) const {
        return fx->ctx().counters.prm_resp[static_cast<int>(s)];
    }

    /// The stashed entries of @p sat, per kind.
    int entries(const std::string& sat, SharedParamKind kind) const {
        int n = 0;
        for (const CarryPrior& cp : fx->ctx().carry.prev_carry)
            if (cp.sat == sat && cp.kind == kind) ++n;
        return n;
    }
};

const char* kHardMonitor =
    "    \"estimate_sat_code_bias\": true,\n"
    "    \"clock_monitor\": true,\n"
    "    \"clock_monitor_reinit_sigma_m\": 0.0,\n";

const char* kSoftMonitor =
    "    \"estimate_sat_code_bias\": true,\n"
    "    \"clock_monitor\": true,\n"
    "    \"clock_monitor_reinit_sigma_m\": 30.0,\n";

}  // namespace

// The hard refloat leaves the flagged satellite's three entries out of the stash and each is
// counted at its own site; the other satellite is stashed and counts nothing.
TEST(StashHardRefloatTest, EachEntryLeftOutIsCountedAtItsOwnSite) {
    StashFixture f(kHardMonitor, "hard");
    ASSERT_TRUE(f.fx->ctx().knobs.clk_mon_on);
    ASSERT_FALSE(f.fx->ctx().knobs.clk_mon_reinit_sig > 0.0);
    f.stash({"G01"});
    EXPECT_EQ(f.count(ResponseSite::ClkHardRefloat), 1);
    EXPECT_EQ(f.count(ResponseSite::PbHardRefloat), 1);
    EXPECT_EQ(f.count(ResponseSite::CbHardRefloat), 1);
    EXPECT_EQ(f.entries("G01", SharedParamKind::Clock), 0);
    EXPECT_EQ(f.entries("G01", SharedParamKind::PhaseBias), 0);
    EXPECT_EQ(f.entries("G01", SharedParamKind::CodeBias), 0);
    EXPECT_EQ(f.entries("G02", SharedParamKind::Clock), 1);
    EXPECT_EQ(f.entries("G02", SharedParamKind::PhaseBias), 1);
    EXPECT_EQ(f.entries("G02", SharedParamKind::CodeBias), 1);
}

// An epoch with no flagged satellite counts nothing, and a flagged satellite counts once per
// epoch it is flagged.
TEST(StashHardRefloatTest, NothingIsCountedWithoutAFlagAndEachFlaggedEpochCounts) {
    StashFixture f(kHardMonitor, "hard_flags");
    f.stash({});
    EXPECT_EQ(f.count(ResponseSite::ClkHardRefloat), 0);
    EXPECT_EQ(f.count(ResponseSite::PbHardRefloat), 0);
    EXPECT_EQ(f.count(ResponseSite::CbHardRefloat), 0);
    f.stash({"G01", "G02"});
    EXPECT_EQ(f.count(ResponseSite::ClkHardRefloat), 2);
    f.stash({"G02"});
    EXPECT_EQ(f.count(ResponseSite::ClkHardRefloat), 3);
    EXPECT_EQ(f.count(ResponseSite::PbHardRefloat), 3);
    EXPECT_EQ(f.count(ResponseSite::CbHardRefloat), 3);
}

// A datum-pinned column is never stashed, so a flag on its satellite leaves nothing out: the
// pinned clock is not counted, and the same satellite's phase and code biases still are.
TEST(StashHardRefloatTest, ADatumPinnedColumnIsNotAnEntryTheRefloatLeavesOut) {
    StashFixture f(kHardMonitor, "hard_pinned");
    f.plan.shared_datum.push_back({f.fx->est().clockColumn("G01"), 0.0});
    f.stash({"G01"});
    EXPECT_EQ(f.count(ResponseSite::ClkHardRefloat), 0);
    EXPECT_EQ(f.count(ResponseSite::PbHardRefloat), 1);
    EXPECT_EQ(f.count(ResponseSite::CbHardRefloat), 1);
}

// The soft reinit keeps the flagged satellite's entries (marked for the reinit variance) and
// the hard-refloat sites stay silent.
TEST(StashHardRefloatTest, TheSoftReinitStashesTheEntriesAndCountsNoHardRefloat) {
    StashFixture f(kSoftMonitor, "soft");
    ASSERT_GT(f.fx->ctx().knobs.clk_mon_reinit_sig, 0.0);
    f.stash({"G01"});
    EXPECT_EQ(f.count(ResponseSite::ClkHardRefloat), 0);
    EXPECT_EQ(f.count(ResponseSite::PbHardRefloat), 0);
    EXPECT_EQ(f.count(ResponseSite::CbHardRefloat), 0);
    int flagged = 0;
    for (const CarryPrior& cp : f.fx->ctx().carry.prev_carry)
        if (cp.sat == "G01" && cp.inflate_q) ++flagged;
    EXPECT_EQ(flagged, 3);
}

// The declaration: each hard-refloat site is armed exactly where the stash can leave an entry
// out (the clock monitor's flags with a reinit sigma that is not positive, on a carried kind),
// and never beside the soft reinit that replaces it.
TEST(StashHardRefloatTest, TheSitesAreArmedWhereTheStashCanLeaveAnEntryOut) {
    GnssConfig cfg;
    NetworkKnobs hard{};
    hard.clk_mon_on = true;
    hard.clk_mon_reinit_sig = 0.0;
    EXPECT_TRUE(responseSiteArmed(ResponseSite::ClkHardRefloat, hard, cfg));
    EXPECT_TRUE(responseSiteArmed(ResponseSite::PbHardRefloat, hard, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::ClkReinit, hard, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::PbReinit, hard, cfg));
    NetworkKnobs soft = hard;
    soft.clk_mon_reinit_sig = 30.0;
    EXPECT_FALSE(responseSiteArmed(ResponseSite::ClkHardRefloat, soft, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::PbHardRefloat, soft, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::CbHardRefloat, soft, cfg));
    // No flags, no entry left out.
    NetworkKnobs none{};
    none.clk_mon_reinit_sig = 0.0;
    EXPECT_FALSE(responseSiteArmed(ResponseSite::ClkHardRefloat, none, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::PbHardRefloat, none, cfg));
    // The clock rides the oscillator under the clock model, so it is not stashed; the clock
    // model's fault test still flags the phase biases.
    NetworkKnobs model{};
    model.clk_model_on = true;
    model.clk_mon_reinit_sig = 0.0;
    EXPECT_FALSE(responseSiteArmed(ResponseSite::ClkHardRefloat, model, cfg));
    EXPECT_TRUE(responseSiteArmed(ResponseSite::PbHardRefloat, model, cfg));
    // The phase biases are not stashed under the DD carry.
    NetworkKnobs dd = hard;
    dd.carry_invariant = true;
    EXPECT_FALSE(responseSiteArmed(ResponseSite::PbHardRefloat, dd, cfg));
}

// ---------------------------------------------------------------------------------------
// The stash-status mirrors and the parked readmit seeds
// ---------------------------------------------------------------------------------------

// The key defaults to false, neither golden arms it, and the loader reads it from the network
// block into the knob snapshot.
TEST(StashStatusSeedMirrorTest, TheKeyDefaultsOffAndNeitherGoldenArmsIt) {
    for (const char* bed : {"field", "sim"}) {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
                           (std::string("golden_") + bed + "_refound.json");
        ASSERT_TRUE(cfg.loadFromJson(p, log)) << bed;
        EXPECT_FALSE(cfg.networkReadmitSeedMirrors()) << bed;
        EXPECT_FALSE(NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig()).readmit_seed_mirrors) << bed;
    }
    LogManager logman;
    GnssConfig absent, armed;
    ASSERT_TRUE(loadWith("", absent, logman, "key_absent"));
    EXPECT_FALSE(absent.networkReadmitSeedMirrors());
    ASSERT_TRUE(loadWith("    \"readmit_seed_mirrors\": true,\n", armed, logman, "key_armed"));
    EXPECT_TRUE(armed.networkReadmitSeedMirrors());
    EXPECT_TRUE(NetworkKnobs::snapshot(armed, armed.getNetworkConfig()).readmit_seed_mirrors);
}

namespace {

CarryPrior codeBiasPrior(const std::string& sat, double value, double var) {
    CarryPrior cp;
    cp.kind = SharedParamKind::CodeBias;
    cp.sat = sat;
    cp.signal = "C1C";
    cp.value = value;
    cp.var = var;
    return cp;
}

/// A parked entry whose fold decision is @p d, with the seed gap of a Seeded one.
CarryPrior parked(CarryPrior cp, CarryDisposition d, double gap_s) {
    cp.t_park_s = 1000.0;
    cp.fold.disposition = d;
    cp.fold.seed_gap_s = (d == CarryDisposition::Seeded) ? gap_s : 0.0;
    return cp;
}

}  // namespace

// The two helpers the mirrors read: only a parked entry the fold applied is a seed, and its
// interval is the gap since the park; every other entry keeps the step, clamped at zero.
TEST(StashStatusSeedMirrorTest, OnlyAParkedEntryTheFoldAppliedIsASeed) {
    CarryPrior live = codeBiasPrior("G01", 0.1, 1e-2);
    live.fold.disposition = CarryDisposition::Folded;
    EXPECT_FALSE(carryFoldedSeed(live));
    EXPECT_DOUBLE_EQ(carryMirrorGapS(live, 30.0), 30.0);
    EXPECT_DOUBLE_EQ(carryMirrorGapS(live, -5.0), 0.0);

    const CarryPrior seed = parked(codeBiasPrior("E03", 0.1, 1e-2), CarryDisposition::Seeded, 600.0);
    EXPECT_TRUE(carryFoldedSeed(seed));
    EXPECT_DOUBLE_EQ(carryMirrorGapS(seed, 30.0), 600.0);

    // Parked and not applied: the column is absent or past the horizon.
    EXPECT_FALSE(carryFoldedSeed(parked(codeBiasPrior("G05", 0.1, 1e-2), CarryDisposition::Parked, 0.0)));
    // A seed the fold retired or screened is not one the fold applied.
    EXPECT_FALSE(carryFoldedSeed(parked(codeBiasPrior("G05", 0.1, 1e-2), CarryDisposition::Retired, 0.0)));
    EXPECT_FALSE(carryFoldedSeed(parked(codeBiasPrior("G05", 0.1, 1e-2), CarryDisposition::Screened, 0.0)));
    // A live entry the fold labelled Seeded is not parked.
    CarryPrior odd = codeBiasPrior("G05", 0.1, 1e-2);
    odd.fold.disposition = CarryDisposition::Seeded;
    EXPECT_FALSE(carryFoldedSeed(odd));
}

namespace {

/// One epoch through networkStashStatus with a live code-bias prior (G01), an applied seed
/// (E03), and a parked entry the fold did not apply (G05, column absent), the monitor and the
/// census armed on the code-bias family.
struct SeedMirrorRun {
    LogManager                       logman;
    GnssConfig                       cfg;
    std::unique_ptr<NetworkTestRun>  fx;
    NetworkEpoch*                    ep{nullptr};

    explicit SeedMirrorRun(bool mirrors, const std::string& tag) {
        std::string extra =
            "    \"estimate_sat_code_bias\": true,\n"
            "    \"readmit_seed_horizon_s\": 3900.0,\n"
            "    \"carry_vce\": true,\n"
            "    \"carry_vce_components\": \"cb\",\n"
            "    \"prior_residual_census\": true,\n"
            "    \"prior_residual_families\": \"cb\",\n";
        extra += mirrors ? "    \"readmit_seed_mirrors\": true,\n"
                         : "    \"readmit_seed_mirrors\": false,\n";
        EXPECT_TRUE(loadWith(extra, cfg, logman, tag));
        const fs::path dir = gnss_test::tempDir() / ("nsd_mirror_" + tag);
        fs::create_directories(dir);
        fx = std::make_unique<NetworkTestRun>(cfg, nullLogger(), dir);
        NetworkRunContext& ctx = fx->ctx();
        EXPECT_EQ(ctx.knobs.readmit_seed_mirrors, mirrors);
        EXPECT_TRUE(ctx.knobs.vce_wants_cb);
        EXPECT_TRUE(ctx.knobs.prires_cb);

        // The solve: G01 and E03 have columns; G05 is absent from the layout.  The posterior
        // variance (1/1000) is below every fold variance the carry gives its prior.
        NetworkEstimator& est = fx->est();
        est.registerCodeBias("G01", "C1C");
        est.registerCodeBias("E03", "C1C");
        std::vector<NetworkEstimator::SharedPrior> pri;
        for (const auto& [sat, v] : {std::pair<const char*, double>{"G01", 0.25},
                                     std::pair<const char*, double>{"E03", 0.36}}) {
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::CodeBias; p.sat = sat; p.signal = "C1C";
            p.value = v; p.info = 1000.0;
            pri.push_back(p);
        }
        est.beginEpoch();
        est.addSharedPrior(pri);
        const Eigen::VectorXd x = est.solve({});

        ctx.carry.prev_carry = {
            [] { CarryPrior c = codeBiasPrior("G01", 0.20, 1e-2);
                 c.fold.disposition = CarryDisposition::Folded; return c; }(),
            parked(codeBiasPrior("E03", 0.30, 1e-2), CarryDisposition::Seeded, 600.0),
            parked(codeBiasPrior("G05", 0.40, 1e-2), CarryDisposition::Parked, 0.0)};

        const GnssTime t{2313, 259230.0};
        const double now_s = TimeUtils::continuousGpsSeconds(t);
        fx->run().engines.carry_vce.beginEpoch(now_s, /*clean=*/true);
        fx->run().engines.prires.beginEpoch(now_s, t.tow);
        ep = &fx->newEpoch(t);
        ep->solve.x_s = x;
        ep->clock.dt = 30.0;
        ep->columns.registered_code_keys = {{"G01", "C1C"}, {"E03", "C1C"}};
        networkStashStatus(fx->run(), *ep, ep->events);
    }

    long long vceRows(const std::string& sat) {
        return fx->run().engines.carry_vce.estimate("cb", sat).n_rows;
    }
};

}  // namespace

// Knob off: the seed is skipped by both mirrors, as before, and counted by each; the live prior
// is the only row.
TEST(StashStatusSeedMirrorTest, OffTheSeedIsSkippedByBothMirrorsAndCounted) {
    SeedMirrorRun r(/*mirrors=*/false, "off");
    const auto& C = r.fx->ctx().counters;
    EXPECT_EQ(C.n_readmit_mirror_seed_vce_cb, 1);
    EXPECT_EQ(C.n_readmit_mirror_seed_census_cb, 1);
    EXPECT_EQ(C.n_readmit_mirror_seed_census_clk, 0);
    EXPECT_EQ(r.vceRows("G01"), 1);
    EXPECT_EQ(r.vceRows("E03"), 0);
    EXPECT_EQ(r.ep->events.status.ep_prires_rows, 1);
}

// Knob on: the same seed is a row of the monitor and of the census, the parked entry the fold
// did not apply still is not, and the counts are the same as with the knob off.
TEST(StashStatusSeedMirrorTest, OnBothMirrorsTakeTheSeedAsARow) {
    SeedMirrorRun r(/*mirrors=*/true, "on");
    const auto& C = r.fx->ctx().counters;
    EXPECT_EQ(C.n_readmit_mirror_seed_vce_cb, 1);
    EXPECT_EQ(C.n_readmit_mirror_seed_census_cb, 1);
    EXPECT_EQ(C.n_readmit_mirror_seed_census_clk, 0);
    EXPECT_EQ(r.vceRows("G01"), 1);
    EXPECT_EQ(r.vceRows("E03"), 1);
    EXPECT_EQ(r.ep->events.status.ep_prires_rows, 2);
    // The fold variance of each row is the carried variance plus q over the row's own
    // interval: the 30 s step for the live prior, the 600 s gap since the park for the seed.
    const double q = r.fx->ctx().params.q(SharedParamKind::CodeBias);
    const double floor = r.fx->ctx().knobs.cb_var_floor;
    ASSERT_GT(q * 600.0, 1e-3) << "the process noise must make the seed gap distinguishable from the step";
    auto& vce = r.fx->run().engines.carry_vce;
    EXPECT_NEAR(vce.estimate("cb", "G01").hand_fold_var, std::max(1e-2 + q * 30.0, floor), 1e-12);
    EXPECT_NEAR(vce.estimate("cb", "E03").hand_fold_var, std::max(1e-2 + q * 600.0, floor), 1e-12);
}

}  // namespace GnssProcessor
