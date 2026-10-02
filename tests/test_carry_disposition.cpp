/**
 * @file test_carry_disposition.cpp
 * @brief Card PRM phase 1b: the carried border's fold disposition, decided once before PASS B
 *        (filter/NetworkCarryDisposition), and the Q3 anchor's decide / commit split.
 *
 * The decision must be the fold's: every clause in the fold's order, the variance the fold
 * forms bit for bit on the variance the caller holds, and the anchor's bookkeeping advanced
 * only by the commit.  The byte-identity A/B of the phase (scripts/prm_ab.py, 6 h goldens
 * across the clock-pivot handover, and the program, scalar and diagonal postures) is the
 * end-to-end gate; these cases pin each rule on its own.
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkCarryDisposition.hpp"
#include "filter/NetworkClockAnchor.hpp"
#include "filter/NetworkEstimator.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A two-station GPS network config with the Q3 anchor, READMIT and the B4 phase-bias floor
/// armed (the clauses the decision distinguishes).  `extra` is spliced into "network".
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    bool       ok{false};
    Loaded(const std::string& tag, const std::string& extra = "") {
        const std::string body =
            "{\n"
            "  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
            "                    \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
            "  \"estimation\": { \"estimate_ifb\": true },\n"
            "  \"filter\": { \"process_noise\": { \"sat_clock_q\": 1e-3 } },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            "    \"clock_temporal_anchor\": true,\n"
            "    \"clock_anchor_q\": 2.9e-8,\n"
            "    \"clock_anchor_var_floor_m2\": 1e-5,\n"
            "    \"clock_anchor_max_gap_s\": 120.0,\n"
            "    \"readmit_seed_horizon_s\": 3900.0,\n"
            "    \"shared_carry_var_floor_pb_m2\": 1e-6,\n" + extra +
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        path = fs::temp_directory_path() / ("prm1b_cfg_" + tag + ".json");
        { std::ofstream f(path); f << body; }
        ok = cfg.loadFromJson(path, log);
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

CarryPrior entry(SharedParamKind kind, const std::string& sat, const std::string& sig,
                 double value, double var) {
    CarryPrior cp;
    cp.kind = kind; cp.sat = sat; cp.signal = sig; cp.value = value; cp.var = var;
    return cp;
}

/// G01 and G02 registered with a clock, one phase bias and one code bias each; G03 absent.
void layout(NetworkEstimator& est) {
    for (const char* s : {"G01", "G02"}) {
        est.registerClock(s);
        est.registerPhaseBias(s, "L1C");
        est.registerCodeBias(s, "C1C");
    }
}

}  // namespace

// Every clause in the fold's order, with the dispositions the card names.
TEST(CarryDisposition, DecideFollowsTheFoldsClauses) {
    Loaded L("clauses");
    ASSERT_TRUE(L.ok);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.cta_on);
    ASSERT_GT(ctx.knobs.readmit_seed_horizon_s, 0.0);
    NetworkEstimator est;
    layout(est);
    const double dt = 30.0, now = 100000.0;
    const auto dec = [&](const CarryPrior& cp) {
        return carryFoldDecide(ctx, est, cp, dt, now, now);
    };

    CarryPrior sh = entry(SharedParamKind::VtecSh, "", "", 1.0, 1e-2);
    EXPECT_EQ(dec(sh).disposition, CarryDisposition::Held);

    CarryPrior pb = entry(SharedParamKind::PhaseBias, "G01", "L1C", 0.1, 1e-4);
    CarryFoldDecision d = dec(pb);
    EXPECT_EQ(d.disposition, CarryDisposition::Folded);
    EXPECT_EQ(d.q_applied_m2, ctx.params.q(SharedParamKind::PhaseBias) * dt);
    EXPECT_FALSE(d.departed);
    EXPECT_EQ(d.var_decided_m2, pb.var);
    EXPECT_EQ(d.value_decided_m, pb.value);

    ctx.carry.eq34_reref.insert({"G01", "L1C"});
    EXPECT_EQ(dec(pb).disposition, CarryDisposition::Reref);
    ctx.carry.eq34_reref.clear();
    ctx.carry.phase_anchor_reref.insert({"G01", "L1C"});
    EXPECT_EQ(dec(pb).disposition, CarryDisposition::Reref);
    ctx.carry.phase_anchor_reref.clear();

    // A clock seen for the first time is LOOSE under the armed anchor; an anchored one is
    // ANCHORED at the anchor's increment.
    CarryPrior ck = entry(SharedParamKind::Clock, "G01", "", 0.5, 1e-3);
    d = dec(ck);
    EXPECT_EQ(d.disposition, CarryDisposition::Loose);
    EXPECT_EQ(d.q_applied_m2, ctx.params.q(SharedParamKind::Clock) * dt);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    ctx.carry.clk_anchor.sat["G01"].t_last   = now - dt;
    d = dec(ck);
    EXPECT_EQ(d.disposition, CarryDisposition::Anchored);
    EXPECT_TRUE(d.anchored);
    EXPECT_EQ(d.q_applied_m2, ctx.knobs.cta_q * dt);
    // Stale beyond the anchor's gap: decoupled, LOOSE.
    ctx.carry.clk_anchor.sat["G01"].t_last = now - 600.0;
    d = dec(ck);
    EXPECT_EQ(d.disposition, CarryDisposition::Loose);
    EXPECT_TRUE(d.decoupled);

    // The clock monitor's reinit wins over the anchor.
    ck.inflate_q = true;
    EXPECT_EQ(dec(ck).disposition, CarryDisposition::Reinit);
    ck.inflate_q = false;

    // READMIT: a parked entry with its column absent is PARKED; with the column back inside
    // the horizon it is SEEDED at q over the gap since the park; past the horizon PARKED.
    CarryPrior gone = entry(SharedParamKind::CodeBias, "G03", "C1C", 0.2, 1e-3);
    gone.t_park_s = now - 600.0;
    EXPECT_EQ(dec(gone).disposition, CarryDisposition::Parked);
    CarryPrior back = entry(SharedParamKind::CodeBias, "G02", "C1C", 0.2, 1e-3);
    back.t_park_s = now - 600.0;
    d = dec(back);
    EXPECT_EQ(d.disposition, CarryDisposition::Seeded);
    EXPECT_EQ(d.seed_gap_s, 600.0);
    EXPECT_EQ(d.q_applied_m2, ctx.params.q(SharedParamKind::CodeBias) * 600.0);
    back.t_park_s = now - 5000.0;
    EXPECT_EQ(dec(back).disposition, CarryDisposition::Parked);

    // A foldable entry whose column left the layout is decided and flagged departed.
    CarryPrior dep = entry(SharedParamKind::PhaseBias, "G03", "L1C", 0.1, 1e-4);
    d = dec(dep);
    EXPECT_EQ(d.disposition, CarryDisposition::Folded);
    EXPECT_TRUE(d.departed);
}

// The variance the fold forms: the decision finished on the caller's variance reproduces the
// fold's expression bit for bit, the B4 floor and the anchor's floor included.
TEST(CarryDisposition, VarEffIsTheFoldsExpression) {
    Loaded L("vareff");
    ASSERT_TRUE(L.ok);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    NetworkEstimator est;
    layout(est);
    std::mt19937 rng(7u);
    std::uniform_real_distribution<double> lv(-9.0, -2.0), ldt(0.0, 3.0);
    const double now = 200000.0;
    for (int i = 0; i < 400; ++i) {
        const double var = std::pow(10.0, lv(rng));
        const double dt  = std::pow(10.0, ldt(rng));
        // Phase bias: cp.var + q*dt, floored.
        CarryPrior pb = entry(SharedParamKind::PhaseBias, "G01", "L1C", 0.1, var);
        const CarryFoldDecision dp = carryFoldDecide(ctx, est, pb, dt, now, now);
        double want = pb.var + ctx.params.q(SharedParamKind::PhaseBias) * std::max(0.0, dt);
        want = std::max(want, ctx.knobs.pb_var_floor);
        EXPECT_EQ(carryFoldVarEff(ctx, pb, dp, pb.var, true), want);
        // Clock: the anchor's rule written out, on a fresh state each time (anchored half the
        // time; an anchored carry older than the anchor's 120 s gap decouples to LOOSE).
        ctx.carry.clk_anchor.sat.clear();
        const bool anchored = (i % 2) != 0;
        if (anchored) {
            ctx.carry.clk_anchor.sat["G02"].anchored = true;
            ctx.carry.clk_anchor.sat["G02"].t_last   = now - dt;
        }
        CarryPrior ck = entry(SharedParamKind::Clock, "G02", "", 0.3, var);
        const CarryFoldDecision dc = carryFoldDecide(ctx, est, ck, dt, now, now);
        ClockAnchorFold af;
        const double got = carryFoldVarEff(ctx, ck, dc, ck.var, true, &af);
        const double dt_sat = now - (now - dt);              // the anchor's own elapsed time
        const bool held = anchored && !(dt_sat > ctx.knobs.cta_max_gap_s);
        double want_c = held ? ck.var + ctx.knobs.cta_q * dt_sat
                             : ck.var + ctx.params.q(SharedParamKind::Clock) * std::max(0.0, dt);
        const bool floor_hit = held && want_c < ctx.knobs.cta_var_floor;
        if (floor_hit) want_c = ctx.knobs.cta_var_floor;
        EXPECT_EQ(got, want_c) << "i=" << i;
        EXPECT_EQ(af.var_eff, want_c);
        EXPECT_EQ(af.anchored, held);
        EXPECT_EQ(af.floor_hit, floor_hit);
        EXPECT_EQ(dc.decoupled, anchored && !held);
    }
}

// A re-expression between the decision and the fold (the handover S-transform adds to the
// carried variance) changes the base, not the decision: finishing the earlier decision on the
// new variance equals deciding afresh on it, with and without the anchor's floor binding.
TEST(CarryDisposition, AReexpressedVarianceKeepsTheDecision) {
    Loaded L("reexpr");
    ASSERT_TRUE(L.ok);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    NetworkEstimator est;
    layout(est);
    const double now = 300000.0, dt = 30.0;
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    ctx.carry.clk_anchor.sat["G01"].t_last   = now - dt;
    for (const double v0 : {1e-7, 4e-6, 1e-3}) {          // under, near and over the 1e-5 floor
        for (const double dv : {0.0, 1.1244e-3, 4.4e-4}) {
            CarryPrior ck = entry(SharedParamKind::Clock, "G01", "", 0.4, v0);
            const CarryFoldDecision d0 = carryFoldDecide(ctx, est, ck, dt, now, now);
            ck.var += dv;                                    // the S-transform's v^2 var_q
            const CarryFoldDecision d1 = carryFoldDecide(ctx, est, ck, dt, now, now);
            EXPECT_EQ(d0.disposition, d1.disposition);
            EXPECT_EQ(d0.q_applied_m2, d1.q_applied_m2);
            EXPECT_EQ(carryFoldVarEff(ctx, ck, d0, ck.var, true),
                      carryFoldVarEff(ctx, ck, d1, ck.var, true)) << v0 << " + " << dv;
        }
    }
}

// The anchor's decide / at / commit in the six states the fold distinguishes, against the
// anchor's rule written out: the decision alone advances nothing, the commit stamps the
// satellite's last fold and counts it, and clockAnchorFoldVariance is the two together.
TEST(CarryDisposition, AnchorDecideAndCommitFollowTheRule) {
    Loaded La("anchor_a"), Lb("anchor_b");
    ASSERT_TRUE(La.ok && Lb.ok);
    NetworkRunContext a(La.cfg, La.cfg.getNetworkConfig());   // decide + at + commit
    NetworkRunContext b(Lb.cfg, Lb.cfg.getNetworkConfig());   // clockAnchorFoldVariance
    const double q = a.params.q(SharedParamKind::Clock), qa = a.knobs.cta_q;
    const double fl = a.knobs.cta_var_floor;
    ASSERT_GT(fl, 0.0);
    long long n_anch = 0, n_loose = 0, n_floor = 0;
    const auto step = [&](const std::string& sat, double var, double dt, double now,
                          double seed, double want, bool want_anch, bool want_floor,
                          bool want_dec) {
        const ClockAnchorSat before = a.carry.clk_anchor.sat.count(sat)
                                          ? a.carry.clk_anchor.sat[sat] : ClockAnchorSat{};
        const ClockAnchorFold dd = clockAnchorFoldDecide(a, sat, var, dt, now, seed);
        const ClockAnchorSat mid = a.carry.clk_anchor.sat.count(sat)
                                       ? a.carry.clk_anchor.sat[sat] : ClockAnchorSat{};
        EXPECT_EQ(mid.t_last, before.t_last);              // the decision writes nothing
        EXPECT_EQ(mid.anchored, before.anchored);
        EXPECT_EQ(mid.stale_pending, before.stale_pending);
        clockAnchorBeginEpoch(a);
        const ClockAnchorFold fa = clockAnchorFoldAt(a, dd, var);
        clockAnchorFoldCommit(a, sat, fa, now, 0.1);
        clockAnchorBeginEpoch(b);
        const ClockAnchorFold fb = clockAnchorFoldVariance(b, sat, var, dt, now, 0.1, seed);
        EXPECT_EQ(dd.var_eff, want) << sat << " at " << now;   // bit equality with the rule
        EXPECT_EQ(fa.var_eff, want);
        EXPECT_EQ(fb.var_eff, want);
        EXPECT_EQ(fa.anchored, want_anch);
        EXPECT_EQ(fa.floor_hit, want_floor);
        EXPECT_EQ(fa.decoupled, want_dec);
        EXPECT_EQ(fb.anchored, want_anch);
        EXPECT_EQ(fb.decoupled, want_dec);
        n_anch += want_anch ? 1 : 0;
        n_loose += want_anch ? 0 : 1;
        n_floor += want_floor ? 1 : 0;
        for (NetworkRunContext* c : {&a, &b}) {
            const ClockAnchorSat& st = c->carry.clk_anchor.sat[sat];
            EXPECT_EQ(st.t_last, now);
            EXPECT_EQ(st.last_value, 0.1);
            EXPECT_EQ(st.last_var_eff, want);
            EXPECT_TRUE(st.folded);
            EXPECT_EQ(st.folded_anchored, want_anch);
            EXPECT_EQ(st.stale_pending, want_dec);
            EXPECT_EQ(c->counters.cta_folded_anchored, n_anch);
            EXPECT_EQ(c->counters.cta_folded_loose, n_loose);
            EXPECT_EQ(c->counters.cta_floor_hits, n_floor);
        }
    };
    const auto anchor = [&](const std::string& sat) {
        a.carry.clk_anchor.sat[sat].anchored = true;
        b.carry.clk_anchor.sat[sat].anchored = true;
    };
    // First sighting: LOOSE at the legacy rate over the driver's step.
    step("G01", 1e-3, 30.0, 1000.0, 0.0, 1e-3 + q * 30.0, false, false, false);
    anchor("G01");
    // Anchored, 30 s after the last fold: the anchor's rate over the satellite's own gap.
    step("G01", 1e-3, 30.0, 1030.0, 0.0, 1e-3 + qa * 30.0, true, false, false);
    // Anchored at the consistency floor.
    step("G01", 1e-7, 30.0, 1060.0, 0.0, fl, true, true, false);
    // Anchored and stale (340 s > the 120 s gap): decoupled, LOOSE over the driver's step.
    step("G01", 1e-3, 30.0, 1400.0, 0.0, 1e-3 + q * 30.0, false, false, true);
    anchor("G01");
    // A re-admission seed 3500 s after the last fold, inside the 3900 s horizon: anchored
    // over the true gap.
    step("G01", 1e-3, 30.0, 4900.0, 3900.0, 1e-3 + qa * 3500.0, true, false, false);
    // No elapsed time on a first sighting: LOOSE, no inflation.
    step("G02", 2e-3, 0.0, 5000.0, 0.0, 2e-3 + q * 0.0, false, false, false);
}

// The pass decides every entry, parked ones included, and writes nothing else: a record left
// over from an earlier epoch (a parked copy keeps the record of the fold that last saw it)
// is overwritten, and the anchor's state does not advance.
TEST(CarryDisposition, ThePassOverwritesEveryEntryAndNothingElse) {
    Loaded L("pass");
    ASSERT_TRUE(L.ok);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    NetworkEstimator est;
    layout(est);
    GnssTime t;
    t.week = 2313;
    t.tow  = 259230.0;
    const double now = TimeUtils::continuousGpsSeconds(t);
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    ctx.carry.clk_anchor.sat["G01"].t_last   = now - 30.0;
    CarryPrior ck = entry(SharedParamKind::Clock, "G01", "", 0.5, 1e-4);
    CarryPrior parked = entry(SharedParamKind::Clock, "G03", "", 0.5, 1e-4);
    parked.t_park_s = now - 300.0;
    parked.fold.disposition = CarryDisposition::Anchored;    // stale record from a past epoch
    ctx.carry.prev_carry = {ck, parked};
    const ClockAnchorSat before = ctx.carry.clk_anchor.sat["G01"];
    networkCarryDecide(ctx, est, t, 30.0);
    EXPECT_EQ(ctx.carry.prev_carry[0].fold.disposition, CarryDisposition::Anchored);
    EXPECT_EQ(ctx.carry.prev_carry[1].fold.disposition, CarryDisposition::Parked);
    EXPECT_EQ(ctx.carry.clk_anchor.sat["G01"].t_last, before.t_last);
    EXPECT_EQ(ctx.carry.clk_anchor.sat.count("G03"), 0u);
    EXPECT_EQ(ctx.counters.cta_folded_anchored, 0LL);
    EXPECT_EQ(ctx.counters.prm_decide_epochs, 1LL);
}
