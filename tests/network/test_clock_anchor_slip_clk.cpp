/**
 * @file test_clock_anchor_slip_clk.cpp
 * @brief The satellite clock event (`network.cycle_slip.if_phase_td_sat_clock_event`) in the
 *        clock anchor: `EpochEvents::Slip::sat_clock_step` read by the fold decision and the
 *        guard.
 *
 * The IF-TD pools name the satellites whose clock stepped, in PASS A, before the carry is
 * folded.  An ANCHORED such satellite therefore folds LOOSE (the legacy increment, as a stale
 * carry does), so the stiff prior never meets the step and nothing has to be withdrawn and
 * re-solved; the guard then declares the decoupling edge with reason `slip_clk`.
 *
 * Covered:
 *   - the fold decision: an anchored clock with the event flag is LOOSE with `decoupled` and
 *     `slip_clk` set and the legacy increment, an unflagged one is ANCHORED, a LOOSE one is
 *     unchanged; the commit records `slip_clk_pending` (not `stale_pending`), and the next
 *     epoch's begin clears it;
 *   - the border decision (`carryFoldDecide`, `carryFoldVarEff`) reads the event map and
 *     hands the flag to the fold's variance;
 *   - the guard decouples the satellite with reason `slip_clk`: counted in
 *     `cta_trip_slip_clk` and in neither the statistical counters nor the backstop's, wire
 *     marked, recorded in `clk_anchor.stepped`, and the epoch is NOT re-solved; the first
 *     solve already follows the data, where the same carry folded anchored is pulled by the
 *     prior and tripped on the statistic;
 *   - an empty event map trips nothing and records nothing (the knob-off path), and a carried
 *     satellite that is not in the map is not touched;
 *   - the backstop: an anchored satellite whose fold did not read the decision is tripped in
 *     the test (`cta_trip_slip_clk_late`) and re-solved;
 *   - a LOOSE satellite in the map has its in-band run reset instead of re-coupling;
 *   - a statistical trip is recorded in `stepped` as well, and `clockAnchorBeginEpoch` clears
 *     it with `tripped`.
 *
 * The fixture is the toy network of test_clock_anchor.cpp: rows are (station, satellite) pairs
 * of a clock-only observable y = clock + receiver clock.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "config/GnssConfig.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/state/CarryDisposition.hpp"
#include "network/solve/Estimator.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& network_extra) {
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
        "    \"process_noise\": {\n"
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
    const fs::path p = gnss_test::tempDir() / ("cta_slipclk_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// A loaded GnssConfig, so a NetworkRunContext carries the knob snapshot a real run would take.
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

const char* const kArmed =
    "    \"clock_temporal_anchor\": true,\n"
    "    \"clock_anchor_q\": 1e-5,\n"
    "    \"clock_anchor_var_floor_m2\": 1e-6,\n"
    "    \"clock_anchor_max_gap_s\": 120.0,\n"
    "    \"clock_anchor_k\": 6.0,\n"
    "    \"clock_anchor_abs_m\": 2.0,\n"
    "    \"clock_anchor_sigma_floor_m\": 0.02,\n"
    "    \"clock_anchor_recouple_epochs\": 3,\n";

struct ToyNet {
    int R = 5, S = 4;
    std::vector<std::string> sats{"G01", "G02", "G03", "G04"};
    std::vector<double> rx{0.7, -1.3, 0.2, 2.1, -0.4};
};

/// Register the shared layout, add every station, and solve.
Eigen::VectorXd solveEpoch(NetworkEstimator& est, const ToyNet& t,
                           const std::vector<double>& sat_clk,
                           const NetworkEstimator::Datum& datum,
                           const std::vector<NetworkEstimator::SharedPrior>& priors) {
    est.beginEpoch();
    for (int r = 0; r < t.R; ++r) {
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(t.S, t.S);
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(t.S, 1);
        Eigen::VectorXd w(t.S), y(t.S);
        for (int s = 0; s < t.S; ++s) {
            A_s(s, s) = 1.0;
            A_r(s, 0) = 1.0;
            w(s)      = 100.0;
            y(s)      = sat_clk[static_cast<std::size_t>(s)] + t.rx[static_cast<std::size_t>(r)];
        }
        est.addStation(A_s, A_r, w, y);
    }
    if (!priors.empty()) est.addSharedPrior(priors);
    return est.solve(datum);
}

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("cta_slipclk_test", sink);
}

/// One guarded epoch of the toy network, carried values folded for `carried`, the guard run
/// with `events`.
struct GuardRun {
    Eigen::VectorXd x;
    EpochWorkspace  ws;
};

GuardRun guardedEpoch(NetworkRunContext& ctx, const GnssConfig& cfg, NetworkRunTraces& traces,
                      NetworkEstimator& est, const ToyNet& t, const std::vector<double>& sat_clk,
                      const NetworkSbasis::Plan& plan,
                      const std::map<std::string, double>& carried, double now_s, double dt,
                      const EpochEvents& events, const std::set<std::string>& fold_slip) {
    GuardRun out;
    clockAnchorBeginEpoch(ctx);
    std::vector<NetworkEstimator::SharedPrior> priors;
    for (const auto& [sat, value] : carried) {
        // The fold as the border fold makes it: the decision (with the event flag for the
        // satellites in `fold_slip`), then the commit.
        const ClockAnchorFold f = clockAnchorFoldDecide(ctx, sat, /*prev_var=*/1e-4, dt, now_s,
                                                        0.0, fold_slip.count(sat) > 0);
        clockAnchorFoldCommit(ctx, sat, f, now_s, value);
        NetworkEstimator::SharedPrior p;
        p.kind  = SharedParamKind::Clock;
        p.sat   = sat;
        p.value = value;
        p.info  = 1.0 / f.var_eff;
        priors.push_back(p);
    }
    networkCapturePriors(out.ws.cap_priors, out.ws.cap_prior_kinds, priors,
                         ContributionKind::BorderCarryDiagonal);
    out.x = solveEpoch(est, t, sat_clk, plan.shared_datum, priors);
    std::vector<std::string> reg(t.sats.begin(), t.sats.end());
    GnssTime tm; tm.week = 2300; tm.tow = now_s;
    networkClockAnchorGuard(ctx, cfg, nullLogger(), traces, tm, est,
                            plan, out.x, out.ws, reg, {}, false, {}, events);
    return out;
}

EpochEvents clockEvent(const std::string& sat, double step_m) {
    EpochEvents ev;
    ev.slip.sat_clock_step[sat] = step_m;
    return ev;
}

/// The toy network with the anchor armed, three satellites carried at truth and anchored from
/// an earlier fold (as the guard's own tests set it).
struct Fixture {
    Loaded            L;
    NetworkRunContext ctx;
    fs::path          dir;
    NetworkRunTraces  traces;
    ToyNet            t;
    NetworkEstimator  est;
    NetworkSbasis::Plan plan;
    std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
    std::map<std::string, double> carried{{"G02", 1.0}, {"G03", -0.5}, {"G04", 2.0}};

    explicit Fixture(const std::string& tag, bool anchored = true)
        : L(tag, kArmed), ctx(L.cfg, L.cfg.getNetworkConfig()),
          dir(gnss_test::tempDir() / ("cta_slipclk_traces_" + tag)),
          traces((fs::create_directories(dir), dir)) {
        for (const auto& s : t.sats) est.registerClock(s);
        plan.shared_datum = {{est.clockColumn(t.sats[0]), 0.0}};
        clockAnchorBeginEpoch(ctx);
        for (const auto& [sat, v] : carried) {
            clockAnchorFoldVariance(ctx, sat, 1e-4, 30.0, 1000.0, v);
            ctx.carry.clk_anchor.sat[sat].anchored = anchored;
        }
    }
    ~Fixture() { std::error_code ec; fs::remove_all(dir, ec); }

    /// One epoch: the guard reads `events`; the fold takes the event flag for `fold_slip`.
    GuardRun epoch(const EpochEvents& events, const std::set<std::string>& fold_slip = {},
                   double now_s = 1030.0) {
        return guardedEpoch(ctx, L.cfg, traces, est, t, truth, plan, carried, now_s, 30.0, events,
                            fold_slip);
    }
    /// The event as the pipeline delivers it: the map names @p sat and the fold reads it.
    GuardRun eventEpoch(const std::string& sat, double step_m) {
        return epoch(clockEvent(sat, step_m), {sat});
    }
};

}  // namespace

// --- the fold decision ----------------------------------------------------

/// An anchored clock with the event flag folds LOOSE at the legacy increment, flagged
/// decoupled and slip_clk, with no floor; without the flag it is ANCHORED, and a LOOSE clock
/// is the same with or without it.
TEST(ClockAnchorSlipClk, TheFoldTakesAnAnchoredClockLooseWhenTheEventIsRead) {
    Loaded L("decide", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-4, 30.0, 1000.0, 0.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    clockAnchorFoldVariance(ctx, "G02", 1e-4, 30.0, 1000.0, 0.0);   // stays LOOSE
    const double q_legacy = ctx.params.q(SharedParamKind::Clock);

    const ClockAnchorFold plain = clockAnchorFoldDecide(ctx, "G01", 1e-4, 30.0, 1030.0);
    EXPECT_TRUE(plain.anchored);
    EXPECT_FALSE(plain.decoupled);
    EXPECT_FALSE(plain.slip_clk);

    const ClockAnchorFold ev = clockAnchorFoldDecide(ctx, "G01", 1e-4, 30.0, 1030.0, 0.0, true);
    EXPECT_FALSE(ev.anchored);
    EXPECT_TRUE(ev.decoupled);
    EXPECT_TRUE(ev.slip_clk);
    EXPECT_FALSE(ev.floor_hit);
    EXPECT_DOUBLE_EQ(ev.inc_m2, q_legacy * 30.0);
    EXPECT_DOUBLE_EQ(ev.rate_m2ps, q_legacy);
    EXPECT_DOUBLE_EQ(ev.var_eff, 1e-4 + q_legacy * 30.0);
    EXPECT_GT(ev.var_eff, 10.0 * plain.var_eff) << "the prior is loosened, not tightened";

    const ClockAnchorFold loose = clockAnchorFoldDecide(ctx, "G02", 1e-4, 30.0, 1030.0, 0.0, true);
    EXPECT_FALSE(loose.anchored);
    EXPECT_FALSE(loose.decoupled);
    EXPECT_FALSE(loose.slip_clk) << "a LOOSE clock has no edge to declare";
    EXPECT_DOUBLE_EQ(loose.var_eff, ev.var_eff);

    // The decision writes nothing: the state still says ANCHORED and nothing is pending.
    EXPECT_TRUE(ctx.carry.clk_anchor.sat.at("G01").anchored);
    EXPECT_FALSE(ctx.carry.clk_anchor.sat.at("G01").slip_clk_pending);
}

/// The commit records slip_clk_pending (and not stale_pending) and counts a LOOSE fold; the
/// next epoch's begin clears it.
TEST(ClockAnchorSlipClk, TheCommitMarksTheEdgePendingForTheGuard) {
    Loaded L("commit", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    clockAnchorBeginEpoch(ctx);
    clockAnchorFoldVariance(ctx, "G01", 1e-4, 30.0, 1000.0, 0.0);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    clockAnchorBeginEpoch(ctx);
    const ClockAnchorFold f = clockAnchorFoldDecide(ctx, "G01", 1e-4, 30.0, 1030.0, 0.0, true);
    clockAnchorFoldCommit(ctx, "G01", f, 1030.0, 0.25);
    const ClockAnchorSat& st = ctx.carry.clk_anchor.sat.at("G01");
    EXPECT_TRUE(st.slip_clk_pending);
    EXPECT_FALSE(st.stale_pending);
    EXPECT_TRUE(st.folded);
    EXPECT_FALSE(st.folded_anchored);
    EXPECT_TRUE(st.anchored) << "the guard owns the state transition";
    EXPECT_EQ(ctx.counters.cta_folded_loose, 2);   // the first fold and this one
    EXPECT_EQ(ctx.counters.cta_folded_anchored, 0);
    clockAnchorBeginEpoch(ctx);
    EXPECT_FALSE(ctx.carry.clk_anchor.sat.at("G01").slip_clk_pending);
}

/// The border decision made once before PASS B reads the event map, and the fold's variance
/// carries the flag: LOOSE with no floor, as the diagonal and the dense fold both form it.
TEST(ClockAnchorSlipClk, TheBorderDecisionReadsTheEventMap) {
    Loaded L("border", kArmed);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    NetworkEstimator est;
    est.registerClock("G03");
    ctx.carry.clk_anchor.sat["G03"].anchored = true;
    ctx.carry.clk_anchor.sat["G03"].t_last   = 1000.0;
    CarryPrior cp;
    cp.kind = SharedParamKind::Clock; cp.sat = "G03"; cp.value = -0.5; cp.var = 1e-4;
    const double dt = 30.0, now = 1030.0;
    const double q_legacy = ctx.params.q(SharedParamKind::Clock);

    EpochEvents none;
    const CarryFoldDecision d0 = carryFoldDecide(ctx, est, cp, dt, now, now, none);
    EXPECT_EQ(d0.disposition, CarryDisposition::Anchored);
    EXPECT_TRUE(d0.anchored);
    EXPECT_FALSE(d0.slip_clk);

    const EpochEvents ev = clockEvent("G03", 0.5);
    const CarryFoldDecision d1 = carryFoldDecide(ctx, est, cp, dt, now, now, ev);
    EXPECT_EQ(d1.disposition, CarryDisposition::Loose);
    EXPECT_FALSE(d1.anchored);
    EXPECT_TRUE(d1.decoupled);
    EXPECT_TRUE(d1.slip_clk);
    EXPECT_DOUBLE_EQ(d1.q_applied_m2, q_legacy * dt);
    EXPECT_DOUBLE_EQ(d1.rate_m2ps, q_legacy);

    ClockAnchorFold af;
    const double var_eff = carryFoldVarEff(ctx, cp, d1, cp.var, /*dense=*/true, &af);
    EXPECT_DOUBLE_EQ(var_eff, cp.var + q_legacy * dt);
    EXPECT_TRUE(af.slip_clk);
    EXPECT_FALSE(af.anchored);
    EXPECT_FALSE(af.floor_hit);

    // An event for another satellite changes nothing for this one.
    const EpochEvents other = clockEvent("G04", 0.5);
    EXPECT_TRUE(carryFoldDecide(ctx, est, cp, dt, now, now, other).anchored);
}

// --- the guard ------------------------------------------------------------

/// G03's carried value is 3 m off the data (its clock stepped) and the pools read the event.
/// The fold takes G03 LOOSE, so the first solve follows the data; the guard declares the edge
/// with reason slip_clk, counts it apart, marks the wire and records the satellite for the
/// hold, and re-solves nothing.  The control, with the same carry folded anchored, is pulled
/// by the prior and tripped on the statistic.
TEST(ClockAnchorSlipClk, AFoldedLooseEventSatelliteIsDecoupledByTheGuardWithoutAReSolve) {
    Fixture f("event");
    ASSERT_TRUE(f.ctx.knobs.cta_on);
    ASSERT_TRUE(f.ctx.knobs.cta_resolve);
    f.carried["G03"] = -3.5;
    const GuardRun run = f.eventEpoch("G03", 3.0);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk, 1);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk_late, 0) << "the fold read the decision";
    EXPECT_EQ(f.ctx.counters.cta_trip_nis + f.ctx.counters.cta_trip_abs, 0);
    EXPECT_EQ(f.ctx.counters.cta_resolves, 0) << "nothing was folded stiff, so nothing is withdrawn";
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at("G02").anchored) << "a neighbour is not touched";
    EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at("G04").anchored);
    EXPECT_TRUE(net::hasSat(f.ctx.entities.sats, "G03", net::Disc::ClockAnchor)) << "wire-marked";
    EXPECT_EQ(f.ctx.counters.cta_marks, 1);
    EXPECT_EQ(f.ctx.carry.clk_anchor.stepped, (std::set<std::string>{"G03"}));
    EXPECT_EQ(f.ctx.carry.clk_anchor.tripped.count("G03"), 1u);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at("G03").slip_clk_pending)
        << "the pending mark stays until the next epoch's begin";
    // The first solve follows the data: G03 relative to G02 is within the loosened prior's
    // pull of the truth (0.19 m for these weights), not the 3 m the carry is off.
    const double rel = run.x(f.est.clockColumn("G03")) - run.x(f.est.clockColumn("G02"));
    EXPECT_NEAR(rel, f.truth[2] - f.truth[1], 0.25);

    Fixture c("event_control");
    c.carried["G03"] = -3.5;
    const GuardRun ctl = c.epoch(EpochEvents{});
    EXPECT_GE(c.ctx.counters.cta_trip_nis + c.ctx.counters.cta_trip_abs, 1)
        << "folded anchored, the same carry is tripped on the statistic";
    EXPECT_EQ(c.ctx.counters.cta_trip_slip_clk, 0);
    EXPECT_EQ(c.ctx.counters.cta_resolves, 1) << "and needs the withdraw-and-re-solve";
    (void)ctl;
}

TEST(ClockAnchorSlipClk, AnEmptyEventMapTripsNothingAndRecordsNothing) {
    Fixture f("empty");
    f.epoch(EpochEvents{});
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk, 0);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk_late, 0);
    EXPECT_EQ(f.ctx.counters.cta_trip_nis + f.ctx.counters.cta_trip_abs, 0);
    EXPECT_EQ(f.ctx.counters.cta_marks, 0);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.stepped.empty());
    EXPECT_TRUE(f.ctx.carry.clk_anchor.tripped.empty());
    for (const auto& [sat, v] : f.carried) {
        (void)v;
        EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at(sat).anchored) << sat;
    }
}

/// A satellite the event map names but that is not carried this epoch has no fold and no row:
/// no edge, no trip.
TEST(ClockAnchorSlipClk, ASatelliteWithNoStandingPriorIsNotTouched) {
    Fixture f("noprior");
    f.carried.erase("G03");   // not carried this epoch, so not folded
    f.eventEpoch("G03", 0.5);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk, 0);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk_late, 0);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.stepped.empty());
}

/// The backstop: an anchored satellite whose fold did not read the decision (the event map
/// names it, the fold took no flag) is tripped in the test on the first pass, with its own
/// counter, and re-solved like a statistical trip.
TEST(ClockAnchorSlipClk, AnAnchoredEventSatelliteTheFoldMissedIsTrippedByTheBackstop) {
    Fixture f("late");
    f.epoch(clockEvent("G03", -0.5));   // the map names G03; the fold is given no flag
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk_late, 1);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk, 0);
    EXPECT_EQ(f.ctx.counters.cta_trip_nis + f.ctx.counters.cta_trip_abs, 0)
        << "not counted as a statistical trip";
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_TRUE(net::hasSat(f.ctx.entities.sats, "G03", net::Disc::ClockAnchor));
    EXPECT_EQ(f.ctx.counters.cta_resolves, 1);
    EXPECT_EQ(f.ctx.carry.clk_anchor.stepped, (std::set<std::string>{"G03"}));
}

/// A LOOSE satellite is not tripped (there is nothing to decouple): its in-band run is reset, so
/// the re-coupling edge cannot complete while the slip layer reports a step, and the same
/// satellite without the event re-couples.
TEST(ClockAnchorSlipClk, ALooseSatelliteInTheEventMapHasItsInBandRunReset) {
    {
        Fixture f("loose_forced", /*anchored=*/false);
        f.ctx.carry.clk_anchor.sat["G03"].in_band = 2;   // one more in-band epoch re-couples
        f.eventEpoch("G03", 0.5);
        const auto& st = f.ctx.carry.clk_anchor.sat.at("G03");
        EXPECT_EQ(st.in_band, 0);
        EXPECT_FALSE(st.anchored);
        EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk, 0) << "a LOOSE satellite is not a trip";
        EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk_late, 0);
        EXPECT_EQ(f.ctx.counters.cta_marks, 0);
        EXPECT_TRUE(f.ctx.carry.clk_anchor.stepped.empty());
    }
    {
        Fixture f("loose_free", /*anchored=*/false);
        f.ctx.carry.clk_anchor.sat["G03"].in_band = 2;
        f.epoch(EpochEvents{});
        EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at("G03").anchored) << "the control re-couples";
    }
}

/// A genuine statistical trip lands in `stepped` too (the hold treats the two alike), and the
/// next epoch's begin clears it with the trip set.
TEST(ClockAnchorSlipClk, AStatisticalTripIsRecordedAndTheNextEpochClearsIt) {
    Fixture f("stat");
    f.truth = {0.0, 1.0, -0.5 + 3.0, 2.0};   // G03's clock really steps by 3 m
    f.epoch(EpochEvents{});
    EXPECT_GE(f.ctx.counters.cta_trip_nis + f.ctx.counters.cta_trip_abs, 1);
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk, 0) << "no event map, so not an event trip";
    EXPECT_EQ(f.ctx.counters.cta_trip_slip_clk_late, 0);
    EXPECT_EQ(f.ctx.carry.clk_anchor.stepped.count("G03"), 1u);
    clockAnchorBeginEpoch(f.ctx);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.stepped.empty());
    EXPECT_TRUE(f.ctx.carry.clk_anchor.tripped.empty());
}
