/**
 * @file test_clock_anchor.cpp
 * @brief Card Q3 (docs/SPEC_clock_temporal_anchor.md) — the guarded temporal
 *        clock anchor: the fold-side variance decision, the per-satellite
 *        elapsed-time staleness guard, the LOOSE/ANCHORED state machine, the
 *        guard's in-epoch refloat + re-solve, and the loader contract.
 *
 * What is pinned here, and why each one is worth a case:
 *
 *  1. **OFF is the legacy expression, exactly.**  The fold seam is on the hot
 *     path of every network run; if its OFF branch is not bit-identical to
 *     `cp.var + sat_clock_q·dt`, the card's whole byte-identity claim is void.
 *  2. **A satellite is born LOOSE and must EARN the anchor.**  The stiffness
 *     must never precede the evidence — that is the standing-invariant-#7
 *     re-coupling rule applied to first sighting and to a warm resume alike.
 *  3. **The inflation basis is the PER-SATELLITE elapsed time, not the epoch
 *     dt.**  Every skip path advances the driver's dt while the carry survives,
 *     so this is the difference between folding a 30 s-old value and folding a
 *     95-minute-old one at 30 s stiffness (DOY-129 withheld 278 consecutive
 *     epochs).  Both the correct inflation and the staleness fallback are pinned.
 *  4. **The variance floor binds** (assessment deviation #1's cap).
 *  5. **The guard recovers a genuine step in the SAME epoch.**  A satellite
 *     whose true clock jumps under a stiff anchor is dragged by the prior; the
 *     guard must detect it on the standardized predicted residual, withdraw the
 *     prior, re-solve, and land within rounding of the solve that never folded
 *     it — that IS adapt-and-EMIT, and it is the difference between correcting
 *     the epoch and merely annotating it.
 *  6. **Both edges.**  The decoupling edge marks the wire and drops the
 *     satellite to LOOSE; the re-coupling edge re-engages only after the
 *     configured consecutive in-band run, and does NOT mark.
 *  7. **The per-constellation common mode is removed** — a whole-constellation
 *     gauge re-realization must not trip a single satellite (invariant #5).
 *  8. Loader contract: defaults OFF, every knob parses, the values a run would
 *     actually use.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkClockAnchor.hpp"
#include "filter/NetworkEstimator.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
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

/// The armed knob block used by every mechanics case below.  q = 1e-5 targets a
/// √(q·Δt) = 1.7 cm walk at 30 s — the FC-1 physical floor — which makes the
/// anchor decisively stiffer than the 1e-3 legacy carry it replaces.
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
// the system has full rank and every satellite clock is data-supported — which
// is exactly the population the guard's refloat is admissible on.
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
    EXPECT_EQ(f.var_eff, prev_var + ctx.knobs.q_clock * dt);   // BIT equality, not NEAR
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
    // First sighting folds at the LEGACY rate — the stiffness never precedes the
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

    // The DRIVER's dt says 30 s (every skip path advances prev_tow); this
    // satellite's carry is actually 90 s old.  The anchor must inflate over 90.
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

    // 300 s > the 120 s bound: folding a stiff prior here is the invariant-#1
    // catastrophe, so the anchor must NOT be used and the edge must be flagged
    // for the guard to event and mark.
    clockAnchorBeginEpoch(ctx);
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-3, 30.0, 1300.0, 0.0);
    EXPECT_FALSE(f.anchored);
    EXPECT_TRUE(f.decoupled);
    EXPECT_TRUE(ctx.carry.clk_anchor.sat.at("G01").stale_pending);
    EXPECT_EQ(f.var_eff, 1e-3 + ctx.knobs.q_clock * 30.0);   // the LEGACY carry bounds it
}

TEST(ClockAnchorFold, VarianceFloorBinds) {
    Loaded L("floor", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-9, 30.0, 1000.0, 0.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    clockAnchorBeginEpoch(ctx);
    // prev_var 1e-9 + q·Δt 3e-4 = 3.0e-4 … which is ABOVE the 1e-6 floor, so it
    // does not bind; drive it below with a tiny Δt to prove the cap is live.
    const auto f = clockAnchorFoldVariance(ctx, "G01", 1e-9, 30.0, 1000.01, 0.0);
    EXPECT_TRUE(f.floor_hit);
    EXPECT_DOUBLE_EQ(f.var_eff, ctx.knobs.cta_var_floor);
    EXPECT_EQ(ctx.counters.cta_floor_hits, 1);
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
    out.ws.cap_priors = priors;   // the injector's capture, mirrored
    out.x = solveEpoch(est, t, sat_clk, plan.shared_datum, &priors);
    std::vector<std::string> reg(t.sats.begin(), t.sats.end());
    GnssTime tm; tm.week = 2300; tm.tow = now_s;
    networkClockAnchorGuard(ctx, cfg, nullLogger(), traces, tm, est,
                            plan, out.x, out.ws, reg, {}, false, {});
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

    // Truth: G03's clock STEPS by 3 m between the carried level and this epoch.
    const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    std::map<std::string, double> carried{{"G02", 1.0}, {"G03", -3.5}, {"G04", 2.0}};

    // Seed t_last and hand every carried satellite the anchor (in a run it would
    // have earned it through the re-validation; here we grant it so the test
    // isolates the guard).
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
    EXPECT_TRUE(ctx.carry.clk_anchor_disc_pending.count("G03") > 0);  // wire-marked
    EXPECT_EQ(ctx.counters.cta_marks, 1);
    // … and the EMITTED value is the data's, not the stale anchor's.  This is
    // the whole point of adapt-and-EMIT: without the re-solve the epoch would
    // publish a clock dragged most of 3 m toward the carried value.
    const Eigen::Index c3 = est.clockColumn("G03");
    EXPECT_NEAR(run.x(c3), x_ref(ref.clockColumn("G03")), 1e-6);
    EXPECT_NEAR(run.x(c3) - run.x(est.clockColumn("G02")),
                truth[2] - truth[1], 1e-3);
    // The withdrawn prior is gone from the RAIM-replay capture, so a later
    // within-epoch re-solve cannot silently re-inject it.
    for (const auto& p : run.ws.cap_priors)
        EXPECT_NE(p.sat, std::string("G03"));
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

    // Every satellite's carried level is displaced by the SAME 5 m — a gauge
    // re-realization, not per-satellite content.  Removing the per-constellation
    // median is standing invariant #5; without it this trips the whole fleet.
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

    // Every satellite starts LOOSE (born LOOSE — the state a decoupled satellite
    // is left in).  It must serve the full in-band run before re-engaging.
    double now = 1000.0;
    for (int ep = 1; ep <= 3; ++ep) {
        guardedEpoch(ctx, L.cfg, traces, est, t, truth, plan, carried, now, 30.0);
        now += 30.0;
        const bool expect_anchored = (ep >= 3);
        EXPECT_EQ(ctx.carry.clk_anchor.sat.at("G03").anchored, expect_anchored)
            << "after epoch " << ep;
    }
    EXPECT_EQ(ctx.counters.cta_recouples, 3);   // one per carried satellite
    EXPECT_EQ(ctx.counters.cta_marks, 0);       // a re-coupling edge is NOT wire-marked
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
    // so the anchor has nothing to fold.  The knob snapshot resolves that ONCE so
    // no downstream site can read an armed flag that cannot act.
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
