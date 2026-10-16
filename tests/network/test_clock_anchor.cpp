/**
 * @file test_clock_anchor.cpp
 * @brief Tests for the guarded temporal clock anchor: the fold-side variance
 *        decision, the per-satellite elapsed-time staleness guard, the
 *        LOOSE/ANCHORED state machine, the guard's in-epoch refloat and
 *        re-solve, the gauge-free fold, and the loader contract (
 *        docs/SPEC_clock_temporal_anchor.md).
 *
 * Properties pinned here:
 *
 *  1. With the anchor off, the fold variance is the legacy expression
 *     `cp.var + sat_clock_q·dt`, bit for bit.  The fold seam runs in every
 *     network run, so the card's byte-identity claim depends on it.
 *  2. A satellite starts LOOSE and becomes ANCHORED only after the guard has
 *     validated it.  Stiffness never precedes the evidence for it; this is the
 *     re-coupling rule of invariant #7, applied to a first sighting and to a
 *     warm resume alike.
 *  3. The inflation uses the per-satellite elapsed time, not the epoch dt.
 *     Every skip path advances the driver's dt while the carry survives, so a
 *     withheld era can leave a carry many minutes old while dt reads 30 s.
 *     Both the inflation over the true elapsed time and the staleness
 *     fallback are pinned.
 *  4. The variance floor binds (the cap from assessment deviation #1).
 *  5. The guard recovers a genuine step within the same epoch.  A satellite
 *     whose true clock jumps under a stiff anchor is pulled toward the prior;
 *     the guard detects it on the standardized predicted residual, withdraws
 *     the prior and re-solves, and the result matches the solve that never
 *     folded the prior to rounding.  This is adapt-and-emit: the epoch is
 *     corrected, not only annotated.
 *  6. Both edges: the decoupling edge marks the wire and returns the satellite
 *     to LOOSE; the re-coupling edge re-engages only after the configured run
 *     of consecutive in-band epochs, and does not mark.
 *  7. The per-constellation common mode is removed, so a whole-constellation
 *     gauge re-realization does not trip a single satellite (invariant #5).
 *  8. Loader contract: defaults off, every knob parses, and the knob snapshot
 *     holds the values a run would use.
 *  9. The gauge-free fold (card Q3 §2.1) carries no information along the
 *     per-constellation clock common direction.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "config/GnssConfig.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/solve/Estimator.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ---------------------------------------------------------------------------
// Config fixtures
// ---------------------------------------------------------------------------

fs::path writeTempConfig(const std::string& tag, const std::string& network_extra,
                         const std::string& filter_extra = "") {
    const std::string body = std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"filter\": {\n"
        "    \"process_noise\": {\n" + filter_extra +
        "      \"sat_clock_q\": 1e-3\n"
        "    }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    const fs::path p = fs::temp_directory_path() / ("cta_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// A loaded GnssConfig plus its NetworkConfig, so a NetworkRunContext can be
/// built with the knob snapshot a real run would take.
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    explicit Loaded(const std::string& tag, const std::string& extra) {
        path = writeTempConfig(tag, extra);
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

/// The armed knob block used by every mechanics case below.  q = 1e-5 gives a
/// √(q·Δt) = 1.7 cm walk at 30 s (the FC-1 physical floor), which makes the
/// anchor much stiffer than the 1e-3 legacy carry it replaces.
const char* const kArmed =
    "    \"clock_temporal_anchor\": true,\n"
    "    \"clock_anchor_q\": 1e-5,\n"
    "    \"clock_anchor_var_floor_m2\": 1e-6,\n"
    "    \"clock_anchor_max_gap_s\": 120.0,\n"
    "    \"clock_anchor_k\": 6.0,\n"
    "    \"clock_anchor_abs_m\": 2.0,\n"
    "    \"clock_anchor_sigma_floor_m\": 0.02,\n"
    "    \"clock_anchor_recouple_epochs\": 3,\n";

// ---------------------------------------------------------------------------
// A minimal but genuinely solvable network: rows are (station, satellite) pairs
// of a clock-only observable  y = δt^s + δt_r.  Shared columns are the satellite
// clocks; local columns the receiver clocks.  One satellite clock is pinned, so
// the system has full rank and every satellite clock is data-supported, which is
// the population on which the guard's refloat is admissible.
// ---------------------------------------------------------------------------

struct ToyNet {
    int R = 5, S = 4;
    std::vector<std::string> sats{"G01", "G02", "G03", "G04"};
    std::vector<double> rx{0.7, -1.3, 0.2, 2.1, -0.4};
};

/// Register the shared layout, add every station, and solve.
Eigen::VectorXd solveEpoch(NetworkEstimator& est, const ToyNet& t,
                           const std::vector<double>& sat_clk,
                           const NetworkEstimator::Datum& datum,
                           const std::vector<NetworkEstimator::SharedPrior>* priors) {
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(t.S, t.S);
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(t.S, 1);
        Eigen::VectorXd w(t.S), y(t.S);
        for (int s = 0; s < t.S; ++s) {
            A_s(s, s) = 1.0;
            A_r(s, 0) = 1.0;
            w(s)      = 100.0;                        // σ = 0.1 m per row
            y(s)      = sat_clk[static_cast<std::size_t>(s)]
                      + t.rx[static_cast<std::size_t>(r)];
        }
        est.addStation(A_s, A_r, w, y);
    }
    if (priors && !priors->empty()) est.addSharedPrior(*priors);
    return est.solve(datum);
}

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("cta_test", sink);
}

}  // namespace

// ===========================================================================
// 1–4: the fold-side decision
// ===========================================================================

TEST(ClockAnchorFold, OffPathIsTheLegacyExpressionExactly) {
    Loaded L("off", "");
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ASSERT_FALSE(ctx.knobs.cta_on);
    const double prev_var = 3.7e-3, dt = 30.0;
    const auto f = clockAnchorFoldVariance(ctx, "G01", prev_var, dt, 1000.0, 0.5);
    EXPECT_EQ(f.var_eff, prev_var + ctx.knobs.q_clock * dt);   // bit equality, not NEAR
    EXPECT_FALSE(f.anchored);
    EXPECT_FALSE(f.floor_hit);
    EXPECT_TRUE(ctx.carry.clk_anchor.sat.empty());   // nothing allocated when OFF
    EXPECT_EQ(ctx.counters.cta_folded_anchored, 0);
}

TEST(ClockAnchorFold, BornLooseAndMustEarnTheAnchor) {
    Loaded L("born", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.cta_on);
    clockAnchorBeginEpoch(ctx);
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1000.0, 0.5);
    // A first sighting folds at the legacy rate: the stiffness never precedes the
    // evidence for it.
    EXPECT_EQ(f.var_eff, 1e-3 + ctx.knobs.q_clock * 30.0);
    EXPECT_FALSE(f.anchored);
    EXPECT_EQ(ctx.counters.cta_folded_loose, 1);
    EXPECT_EQ(ctx.counters.cta_folded_anchored, 0);
    const auto& st = ctx.carry.clk_anchor.sat.at("G01");
    EXPECT_TRUE(st.folded);
    EXPECT_FALSE(st.folded_anchored);
    EXPECT_DOUBLE_EQ(st.t_last, 1000.0);
    EXPECT_DOUBLE_EQ(st.last_value, 0.5);
}

TEST(ClockAnchorFold, AnchoredFoldUsesPerSatelliteElapsedTime) {
    Loaded L("dtsat", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1000.0, 0.0);   // seed t_last
    ctx.carry.clk_anchor.sat["G01"].anchored = true;

    // The driver's dt says 30 s (every skip path advances prev_tow), but this
    // satellite's carry is 90 s old.  The anchor inflates over 90 s.
    clockAnchorBeginEpoch(ctx);
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1090.0, 0.0);
    EXPECT_TRUE(f.anchored);
    EXPECT_DOUBLE_EQ(f.var_eff, 1e-3 + ctx.knobs.cta_q * 90.0);
    EXPECT_EQ(ctx.counters.cta_folded_anchored, 1);
}

TEST(ClockAnchorFold, StaleCarryFallsBackToLooseAndFlagsTheEdge) {
    Loaded L("stale", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1000.0, 0.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;

    // 300 s exceeds the 120 s bound.  Folding a stiff prior across that gap is
    // the failure invariant #1 describes, so the anchor is not used and the edge
    // is flagged for the guard to emit an event and mark the wire.
    clockAnchorBeginEpoch(ctx);
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1300.0, 0.0);
    EXPECT_FALSE(f.anchored);
    EXPECT_TRUE(f.decoupled);
    EXPECT_TRUE(ctx.carry.clk_anchor.sat.at("G01").stale_pending);
    EXPECT_EQ(f.var_eff, 1e-3 + ctx.knobs.q_clock * 30.0);   // the legacy carry bounds it
}

TEST(ClockAnchorFold, VarianceFloorBinds) {
    Loaded L("floor", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-9, 30.0, 1000.0, 0.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    clockAnchorBeginEpoch(ctx);
    // At Δt = 30 s, prev_var 1e-9 + q·Δt 3e-4 = 3.0e-4 is above the 1e-6 floor,
    // so the floor would not bind.  A Δt of 0.01 s gives 1.01e-7, below it.
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-9, 30.0, 1000.01, 0.0);
    EXPECT_TRUE(f.floor_hit);
    EXPECT_DOUBLE_EQ(f.var_eff, ctx.knobs.cta_var_floor);
    EXPECT_EQ(ctx.counters.cta_floor_hits, 1);
}

/// Readmission continuity (network.readmit_seed_horizon_s): a parked pre-gap
/// realization that seeds a re-registered column is stale only beyond the seed
/// horizon.  A 3300 s eclipse gap (27.5 times the 120 s staleness bound) folds
/// ANCHORED over the true gap (prev_var + cta_q·gap), flags no edge, and leaves the
/// guard to re-validate it.  The unseeded call at the same gap takes the
/// stale/decouple path (the case above), so the two folds differ only through the
/// bound.  Beyond the horizon a seed is stale like any other carry, and a LOOSE seed
/// inflates over the true gap, not over the dt.
TEST(ClockAnchorFold, SeededReturnFoldsAnchoredOverTheTrueGap) {
    Loaded L("seed", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1000.0, 0.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;

    clockAnchorBeginEpoch(ctx);
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 4300.0, 0.0,
                                           /*seed_horizon_s=*/3600.0);
    EXPECT_TRUE(f.anchored);
    EXPECT_FALSE(f.decoupled);
    EXPECT_FALSE(ctx.carry.clk_anchor.sat.at("G01").stale_pending);
    EXPECT_DOUBLE_EQ(f.var_eff, 1e-3 + ctx.knobs.cta_q * 3300.0);
    EXPECT_EQ(ctx.counters.cta_folded_anchored, 1);

    // Beyond the seed horizon (3700 s since the fold above): stale like any carry.
    clockAnchorBeginEpoch(ctx);
    const auto g = clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 8000.0, 0.0, 3600.0);
    EXPECT_FALSE(g.anchored);
    EXPECT_TRUE(g.decoupled);
    EXPECT_TRUE(ctx.carry.clk_anchor.sat.at("G01").stale_pending);

    // A LOOSE seed inflates over the true gap, not over the driver's dt.
    ctx.carry.clk_anchor.sat["G02"].t_last   = 1000.0;
    ctx.carry.clk_anchor.sat["G02"].anchored = false;
    clockAnchorBeginEpoch(ctx);
    const auto h = clockAnchorFoldVariance(ctx, "G02", 1e-3, 30.0, 4300.0, 0.0, 3600.0);
    EXPECT_FALSE(h.anchored);
    EXPECT_FALSE(h.decoupled);
    EXPECT_DOUBLE_EQ(h.var_eff, 1e-3 + ctx.knobs.q_clock * 3300.0);
}

TEST(ClockAnchorFold, BeginEpochClearsTheFoldFlagsButKeepsTheState) {
    Loaded L("begin", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1000.0, 4.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    clockAnchorBeginEpoch(ctx);
    const auto& st = ctx.carry.clk_anchor.sat.at("G01");
    EXPECT_FALSE(st.folded);          // a satellite absent this epoch folds nothing…
    EXPECT_TRUE(st.anchored);         // …but does not lose the anchor it earned
    EXPECT_DOUBLE_EQ(st.t_last, 1000.0);
}

// ===========================================================================
// 5–7: the guard
// ===========================================================================

namespace {

/// Drive one guarded epoch: fold the anchor priors for `anchored_sats`, solve,
/// and run the guard.  Returns the (possibly re-solved) shared solution.
struct GuardRun {
    Eigen::VectorXd x;
    EpochWorkspace  ws;
};

GuardRun guardedEpoch(NetworkRunContext& ctx, const GnssConfig& cfg,
                      NetworkRunTraces& traces,
                      NetworkEstimator& est, const ToyNet& t,
                      const std::vector<double>& sat_clk,
                      const NetworkSbasis::Plan& plan,
                      const std::map<std::string, double>& carried,
                      double now_s, double dt) {
    GuardRun out;
    clockAnchorBeginEpoch(ctx);
    std::vector<NetworkEstimator::SharedPrior> priors;
    for (const auto& [sat, value] : carried) {
        const auto f = clockAnchorFoldVariance(ctx, sat, /*prev_var=*/1e-4, dt, now_s, value);
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::Clock;
        p.sat  = sat;
        p.value = value;
        p.info  = 1.0 / f.var_eff;
        priors.push_back(p);
    }
    // The injector's capture, mirrored, with its kinds.
    networkCapturePriors(out.ws.cap_priors, out.ws.cap_prior_kinds, priors,
                         ContributionKind::BorderCarryDiagonal);
    out.x = solveEpoch(est, t, sat_clk, plan.shared_datum, &priors);
    std::vector<std::string> reg(t.sats.begin(), t.sats.end());
    GnssTime tm; tm.week = 2300; tm.tow = now_s;
    networkClockAnchorGuard(ctx, cfg, nullLogger(), traces, tm, est,
                            plan, out.x, out.ws, reg, {}, false, {}, EpochEvents{});
    return out;
}

}  // namespace

TEST(ClockAnchorGuard, GenuineStepIsDetectedRefloatedAndValueCorrectedInEpoch) {
    Loaded L("guard", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.cta_on);
    ASSERT_TRUE(ctx.knobs.cta_resolve);
    const fs::path dir = fs::temp_directory_path() / "cta_guard_traces";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);

    ToyNet t;
    NetworkEstimator est;
    for (const auto& s : t.sats) est.registerClock(s);
    NetworkSbasis::Plan plan;
    plan.shared_datum = {{est.clockColumn(t.sats[0]), 0.0}};

    // Truth: G03's clock steps by 3 m between the carried level and this epoch.
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    std::map<std::string, double> carried{{"G02", 1.0}, {"G03", -3.5}, {"G04", 2.0}};

    // Seed t_last and set every carried satellite ANCHORED.  In a run the state
    // is reached through re-validation; setting it directly isolates the guard.
    clockAnchorBeginEpoch(ctx);
    for (const auto& [sat, v] : carried) {
        clockAnchorFoldVariance(ctx, sat, 1e-4, 30.0, 1000.0, v);
        ctx.carry.clk_anchor.sat[sat].anchored = true;
    }

    // The reference: the same epoch solved with G03's prior never folded.
    NetworkEstimator ref;
    for (const auto& s : t.sats) ref.registerClock(s);
    NetworkSbasis::Plan rplan;
    rplan.shared_datum = {{ref.clockColumn(t.sats[0]), 0.0}};
    std::vector<NetworkEstimator::SharedPrior> ref_priors;
    for (const auto& [sat, v] : carried) {
        if (sat == "G03") continue;
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::Clock; p.sat = sat; p.value = v;
        p.info = 1.0 / (1e-4 + ctx.knobs.cta_q * 30.0);
        ref_priors.push_back(p);
    }
    const Eigen::VectorXd x_ref = solveEpoch(ref, t, truth, rplan.shared_datum, &ref_priors);

    const auto run = guardedEpoch(ctx, L.cfg, traces, est, t, truth, plan, carried, 1030.0, 30.0);

    // The guard fired on G03 …
    EXPECT_GE(ctx.counters.cta_trip_nis + ctx.counters.cta_trip_abs, 1);
    EXPECT_EQ(ctx.counters.cta_resolves, 1);
    EXPECT_EQ(ctx.counters.cta_resolve_failed, 0);
    EXPECT_FALSE(ctx.carry.clk_anchor.sat.at("G03").anchored);       // decoupled
    EXPECT_TRUE(net::hasSat(ctx.entities.sats, "G03", net::Disc::ClockAnchor));  // wire-marked
    EXPECT_EQ(ctx.counters.cta_marks, 1);
    // … and the emitted value follows the data, not the stale anchor.  Without
    // the re-solve the epoch would publish a clock pulled most of the 3 m toward
    // the carried value (adapt-and-emit).
    const Eigen::Index c3 = est.clockColumn("G03");
    EXPECT_NEAR(run.x(c3), x_ref(ref.clockColumn("G03")), 1e-6);
    EXPECT_NEAR(run.x(c3) - run.x(est.clockColumn("G02")),
                truth[2] - truth[1], 1e-3);
    // The withdrawn prior is gone from the RAIM-replay capture, so a later
    // within-epoch re-solve cannot silently re-inject it.
    for (const auto& p : run.ws.cap_priors)
        EXPECT_NE(p.sat, std::string("G03"));
    // The capture's kinds are erased in step, so the replay still
    // names one kind per captured prior.
    EXPECT_EQ(run.ws.cap_priors.size(), 2u);
    EXPECT_EQ(run.ws.cap_prior_kinds.size(), run.ws.cap_priors.size());
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST(ClockAnchorGuard, WholeConstellationCommonModeDoesNotTrip) {
    Loaded L("common", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    const fs::path dir = fs::temp_directory_path() / "cta_common_traces";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);

    ToyNet t;
    NetworkEstimator est;
    for (const auto& s : t.sats) est.registerClock(s);
    NetworkSbasis::Plan plan;
    plan.shared_datum = {{est.clockColumn(t.sats[0]), 0.0}};

    // Every satellite's carried level is displaced by the same 5 m: a gauge
    // re-realization, not per-satellite content.  The guard removes the
    // per-constellation median (invariant #5); without that, every satellite
    // would trip.
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    std::map<std::string, double> carried{
        {"G02", 1.0 - 5.0}, {"G03", -0.5 - 5.0}, {"G04", 2.0 - 5.0}};
    clockAnchorBeginEpoch(ctx);
    for (const auto& [sat, v] : carried) {
        clockAnchorFoldVariance(ctx, sat, 1e-4, 30.0, 1000.0, v);
        ctx.carry.clk_anchor.sat[sat].anchored = true;
    }
    guardedEpoch(ctx, L.cfg, traces, est, t, truth, plan, carried, 1030.0, 30.0);
    EXPECT_EQ(ctx.counters.cta_trip_nis, 0);
    EXPECT_EQ(ctx.counters.cta_trip_abs, 0);
    EXPECT_EQ(ctx.counters.cta_marks, 0);
    for (const auto& [sat, v] : carried) {
        (void)v;
        EXPECT_TRUE(ctx.carry.clk_anchor.sat.at(sat).anchored) << sat;
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST(ClockAnchorGuard, RecouplingNeedsTheConsecutiveInBandRunAndDoesNotMark) {
    Loaded L("recouple", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ASSERT_EQ(ctx.knobs.cta_recouple_ep, 3);
    const fs::path dir = fs::temp_directory_path() / "cta_recouple_traces";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);

    ToyNet t;
    NetworkEstimator est;
    for (const auto& s : t.sats) est.registerClock(s);
    NetworkSbasis::Plan plan;
    plan.shared_datum = {{est.clockColumn(t.sats[0]), 0.0}};
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    std::map<std::string, double> carried{{"G02", 1.0}, {"G03", -0.5}, {"G04", 2.0}};

    // Every satellite starts LOOSE, the same state a decoupled satellite is left
    // in.  It re-engages only after the full run of in-band epochs.
    double now = 1000.0;
    for (int ep = 1; ep <= 3; ++ep) {
        guardedEpoch(ctx, L.cfg, traces, est, t, truth, plan, carried, now, 30.0);
        now += 30.0;
        const bool expect_anchored = (ep >= 3);
        EXPECT_EQ(ctx.carry.clk_anchor.sat.at("G03").anchored, expect_anchored)
            << "after epoch " << ep;
    }
    EXPECT_EQ(ctx.counters.cta_recouples, 3);   // one per carried satellite
    EXPECT_EQ(ctx.counters.cta_marks, 0);       // a re-coupling edge is not wire-marked
    std::error_code ec; fs::remove_all(dir, ec);
}

// ===========================================================================
// 8: loader contract
// ===========================================================================

TEST(ClockAnchorLoader, DefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", "");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkClockTemporalAnchor());
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorQ(), 1e-4);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorVarFloorM2(), 0.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorMaxGapS(), 120.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorK(), 6.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorAbsM(), 2.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorSigmaFloorM(), 0.02);
    EXPECT_EQ(cfg.networkClockAnchorRecoupleEpochs(), 4);
    EXPECT_TRUE(cfg.networkClockAnchorMark());
    EXPECT_TRUE(cfg.networkClockAnchorResolve());
    std::error_code ec; fs::remove(p, ec);
}

TEST(ClockAnchorLoader, EveryKnobParses) {
    LogManager log;
    const fs::path p = writeTempConfig("parse",
        "    \"clock_temporal_anchor\": true,\n"
        "    \"clock_anchor_q\": 3e-5,\n"
        "    \"clock_anchor_var_floor_m2\": 4e-4,\n"
        "    \"clock_anchor_max_gap_s\": 90.0,\n"
        "    \"clock_anchor_k\": 5.5,\n"
        "    \"clock_anchor_abs_m\": 1.5,\n"
        "    \"clock_anchor_sigma_floor_m\": 0.03,\n"
        "    \"clock_anchor_recouple_epochs\": 6,\n"
        "    \"clock_anchor_mark\": false,\n"
        "    \"clock_anchor_resolve\": false,\n");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkClockTemporalAnchor());
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorQ(), 3e-5);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorVarFloorM2(), 4e-4);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorMaxGapS(), 90.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorK(), 5.5);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorAbsM(), 1.5);
    EXPECT_DOUBLE_EQ(cfg.networkClockAnchorSigmaFloorM(), 0.03);
    EXPECT_EQ(cfg.networkClockAnchorRecoupleEpochs(), 6);
    EXPECT_FALSE(cfg.networkClockAnchorMark());
    EXPECT_FALSE(cfg.networkClockAnchorResolve());
    std::error_code ec; fs::remove(p, ec);
}

TEST(ClockAnchorLoader, ClockModelInterlockDisarmsTheAnchor) {
    // Under the two-state oscillator the clock is never stashed to the D2 carry,
    // so the anchor has nothing to fold.  The knob snapshot resolves that once,
    // so no downstream site reads an armed flag that cannot act.
    LogManager log;
    const fs::path p = writeTempConfig("interlock",
        std::string(kArmed) + "    \"clock_model\": true,\n");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkClockTemporalAnchor());   // the user's request stands…
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EXPECT_TRUE(ctx.knobs.clk_model_on);
    EXPECT_FALSE(ctx.knobs.cta_on);                  // …but the run cannot honour it
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// 9: the gauge-free fold (card Q3 §2.1)
// ===========================================================================
//
// The property is algebraic and can be checked exactly: after the downdate the
// folded prior carries no information along the per-constellation clock common
// direction, and the information it carries on the differences is unchanged.
// Both are checked against a solve rather than against Λ, because "no
// information along u" has an operational meaning the algebra alone does not
// show: shifting every carried clock of a constellation by a common amount
// leaves the solution unchanged.

namespace {

/// Solve one epoch with satellite-clock priors folded raw or gauge-free.
Eigen::VectorXd solveWithClockPriors(NetworkEstimator& est, const ToyNet& t,
                                     const std::vector<double>& sat_clk,
                                     const NetworkEstimator::Datum& datum,
                                     const std::map<std::string, double>& carried,
                                     double var, bool gauge_free,
                                     std::size_t* downdated = nullptr) {
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(t.S, t.S);
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(t.S, 1);
        Eigen::VectorXd w(t.S), y(t.S);
        for (int s = 0; s < t.S; ++s) {
            A_s(s, s) = 1.0; A_r(s, 0) = 1.0; w(s) = 100.0;
            y(s) = sat_clk[static_cast<std::size_t>(s)] + t.rx[static_cast<std::size_t>(r)];
        }
        est.addStation(A_s, A_r, w, y);
    }
    std::vector<NetworkEstimator::SharedPrior> priors;
    for (const auto& kv : carried) {
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::Clock; p.sat = kv.first; p.value = kv.second;
        p.info = 1.0 / var;
        priors.push_back(p);
    }
    if (gauge_free) {
        const std::size_t n = est.addSharedPriorClockGaugeFree(priors);
        if (downdated) *downdated = n;
    } else {
        est.addSharedPrior(priors);
    }
    return est.solve(datum);
}

}  // namespace

TEST(ClockAnchorGaugeFree, CommonShiftOfEveryCarriedClockDoesNotMoveTheSolution) {
    ToyNet t;
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    std::map<std::string, double> carried{{"G02", 1.0}, {"G03", -0.5}, {"G04", 2.0}};
    std::map<std::string, double> shifted;
    for (const auto& kv : carried) shifted[kv.first] = kv.second + 5.0;   // a pure gauge move

    NetworkEstimator a, b, c;
    for (auto* e : {&a, &b, &c}) for (const auto& s : t.sats) e->registerClock(s);
    const NetworkEstimator::Datum da = {{a.clockColumn(t.sats[0]), 0.0}};
    const NetworkEstimator::Datum db = {{b.clockColumn(t.sats[0]), 0.0}};
    const NetworkEstimator::Datum dc = {{c.clockColumn(t.sats[0]), 0.0}};

    std::size_t n_gf = 0;
    const Eigen::VectorXd x_gf  = solveWithClockPriors(a, t, truth, da, carried, 4e-4, true, &n_gf);
    const Eigen::VectorXd x_gfs = solveWithClockPriors(b, t, truth, db, shifted, 4e-4, true);
    const Eigen::VectorXd x_raw = solveWithClockPriors(c, t, truth, dc, shifted, 4e-4, false);

    EXPECT_EQ(n_gf, 3u);                                   // all three downdated
    EXPECT_LT((x_gf - x_gfs).cwiseAbs().maxCoeff(), 1e-9);  // the gauge move has no effect
    // …and the same shift moves a raw fold.  Without this check the test above
    // would also pass on a prior that does nothing at all.
    EXPECT_GT((x_gf - x_raw).cwiseAbs().maxCoeff(), 0.1);
}

TEST(ClockAnchorGaugeFree, DifferenceInformationSurvivesAndALoneClockFoldsNothing) {
    ToyNet t;
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    // G03 is carried 1 m off truth and the others at truth.  That is a
    // difference, not the gauge, so a gauge-free anchor still pulls it.
    std::map<std::string, double> carried{{"G02", 1.0}, {"G03", 0.5}, {"G04", 2.0}};
    NetworkEstimator a;
    for (const auto& s : t.sats) a.registerClock(s);
    const NetworkEstimator::Datum da = {{a.clockColumn(t.sats[0]), 0.0}};
    const Eigen::VectorXd x = solveWithClockPriors(a, t, truth, da, carried, 4e-4, true);
    EXPECT_GT(x(a.clockColumn("G03")) - truth[2], 0.3);

    // A constellation contributing a single carried clock is entirely gauge: it
    // has no estimable content to anchor, so nothing is folded.
    NetworkEstimator b;
    for (const auto& s : t.sats) b.registerClock(s);
    const NetworkEstimator::Datum db = {{b.clockColumn(t.sats[0]), 0.0}};
    std::size_t n_one = 0;
    const std::map<std::string, double> lone{{"G03", 99.0}};
    const Eigen::VectorXd x_one =
        solveWithClockPriors(b, t, truth, db, lone, 4e-4, true, &n_one);
    EXPECT_EQ(n_one, 0u);
    EXPECT_NEAR(x_one(b.clockColumn("G03")), truth[2], 1e-6)
        << "a lone carried clock IS the gauge — a 99 m carried value must not move it";
}

TEST(ClockAnchorGaugeFree, RefoldDroppingRebuildsTheMarginalizedGroupExactly) {
    ToyNet t;
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    const std::map<std::string, double> carried{{"G02", 1.0}, {"G03", 0.5}, {"G04", 2.0}};
    std::vector<NetworkEstimator::SharedPrior> priors;
    for (const auto& kv : carried) {
        NetworkEstimator::SharedPrior p;
        p.kind = SharedParamKind::Clock; p.sat = kv.first; p.value = kv.second;
        p.info = 1.0 / 4e-4;
        priors.push_back(p);
    }
    std::map<std::string, double> without = carried;
    without.erase("G03");

    NetworkEstimator a;
    for (const auto& s : t.sats) a.registerClock(s);
    const NetworkEstimator::Datum da = {{a.clockColumn(t.sats[0]), 0.0}};
    const Eigen::VectorXd x_ref = solveWithClockPriors(a, t, truth, da, without, 4e-4, true);

    NetworkEstimator b;
    for (const auto& s : t.sats) b.registerClock(s);
    const NetworkEstimator::Datum db = {{b.clockColumn(t.sats[0]), 0.0}};
    const Eigen::VectorXd x_full = solveWithClockPriors(b, t, truth, db, carried, 4e-4, true);
    EXPECT_GT((x_full - x_ref).cwiseAbs().maxCoeff(), 1e-3)
        << "dropping G03 must actually matter, or the comparison is vacuous";
    // A per-key subtraction cannot do this: removing one key moves the group's
    // information-weighted mean, so every survivor's anchor target changes too.
    EXPECT_EQ(b.refoldSharedPriorClockGaugeFreeDropping(priors, {"G03"}), 1u);
    const Eigen::VectorXd x_drop = b.solve(db);
    EXPECT_LT((x_drop - x_ref).cwiseAbs().maxCoeff(), 1e-9);

    b.refoldSharedPriorClockGaugeFreeDropping(priors, {"G03"}, /*restore=*/true);
    const Eigen::VectorXd x_back = b.solve(db);
    EXPECT_LT((x_back - x_full).cwiseAbs().maxCoeff(), 1e-9);
}

TEST(ClockAnchorLoader, GaugeFreeDefaultsOffAndParses) {
    LogManager log;
    {
        const fs::path p = writeTempConfig("gfdef", "");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkClockAnchorGaugeFree());
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("gfon",
            std::string(kArmed) + "    \"clock_anchor_gauge_free\": true,\n");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkClockAnchorGaugeFree());
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        EXPECT_TRUE(ctx.knobs.cta_gauge_free);
        std::error_code ec; fs::remove(p, ec);
    }
}
