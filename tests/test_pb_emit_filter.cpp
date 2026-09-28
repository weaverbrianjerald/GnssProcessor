/**
 * @file test_pb_emit_filter.cpp
 * @brief Gates for the PB emission-continuity filter (@ref NetworkPbEmitFilter,
 *        the smoother on the re-based emitted phase biases that M5 requires).
 *
 * The filter is driven standalone on synthetic single-key streams of known truth
 * (as for @ref NetworkEmitRebase / @ref NetworkCarryVce), and end-to-end into the
 * production emitter (@ref NetworkProductEmitter, whose 0.05 m jump test remains
 * the only, unchanged discontinuity test), so the disc-floor claim is checked
 * against the real counter.  Gates:
 *
 *   1. A quasi-static value with per-epoch noise is smoothed (the filtered value
 *      tracks the constant, the posterior σ shrinks below the seed, no resets).
 *   2. A reference-path slip flag causes a reset: the raw (stepped) value passes
 *      through unchanged (never smoothed over, invariant #1) and the cause is
 *      recorded.
 *   3. An arc gap over max_gap_s causes a re-init (the value passes through).
 *   4. A genuine innovation over k·σ̂_step causes a reset (the raw value passes
 *      through).
 *   5. End to end: on a noisy quasi-static stream the production emitter reports
 *      no discontinuity with the filter (floor cleared) but fires on most epochs
 *      without it (negative control); a genuine >0.5 m event fires exactly once
 *      on both arms, which shows the filter does not hide real events.
 *   6. The k·σ̂_step gate self-tunes: σ̂_step tracks the injected noise scale.
 *
 * v2 gate (@c Config::gate_v2: censoring-free feed, model-scaled k·√(Ppred+R),
 * seed used only as a guard; the default-off v1 path drives gates 1–6 unchanged):
 *   7. The censoring-free feed breaks the v1 gate-scale spiral (v1 σ̂ latches low
 *      and resets persist; v2 σ̂ recovers the scale and resets drop), and the
 *      n_fed_all_rows accounting shows that the innovation-gated rows are fed.
 *   8. One k=4 works at both 30 s and 300 s: a genuine event fires at both cadences.
 *   9. A sub-gate slip passes as a bounded transient toward the new level (the
 *      §3 residual-risk envelope), producing no discontinuity.
 */

#include <gtest/gtest.h>

#include "products/NetworkPbEmitFilter.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"

#include <array>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

const GnssTime kT0{2313, 259200.0};
constexpr double LAM = Constants::GPS_L1_WAVELENGTH;   // ~0.190294 m
constexpr double DT  = 30.0;                           // epoch spacing (s)

NetworkPbEmitFilter::Config defaultCfg() {
    NetworkPbEmitFilter::Config c;   // q=3e-7, k=4, seed 0.05, max_gap 600, window 60
    return c;
}

// Run the filter on one key/value at continuous time t; returns the filtered Entry.
NetworkPbEmitFilter::Entry step(NetworkPbEmitFilter& f, const std::string& sat,
                                double value, double t, bool slipped = false) {
    std::vector<NetworkPbEmitFilter::Entry> e(1);
    e[0].sat = sat; e[0].signal = "L1C"; e[0].value_m = value; e[0].slipped = slipped;
    f.filterEpoch(e, t);
    return e[0];
}

// Deterministic N(0,1): Box–Muller on raw mt19937 (bit-identical across platforms,
// the same generator the self-tune gate test uses).  The estimator's χ²₁ de-bias is
// exact for Gaussian ê, so the probe noise must be Gaussian.
struct Gauss {
    std::mt19937 rng;
    explicit Gauss(unsigned seed) : rng(seed) {}
    double operator()() {
        const double u1 = (static_cast<double>(rng()) + 0.5) / 4294967296.0;
        const double u2 = (static_cast<double>(rng()) + 0.5) / 4294967296.0;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
};

// --- 1. A quasi-static value with noise is smoothed; σ shrinks; no resets --------

TEST(PbEmitFilter, SmoothsQuasiStaticStreamAndShrinksSigma) {
    NetworkPbEmitFilter f(defaultCfg());
    const double base = 0.30;
    // Deterministic sub-gate noise (|offset| <= 0.05 < the 4·σ̂_step gate).
    const std::array<double, 6> off{{0.04, -0.045, 0.05, -0.04, 0.045, -0.05}};

    double t = kT0.week * 604800.0 + kT0.tow;
    NetworkPbEmitFilter::Entry last{};
    for (int i = 0; i < 60; ++i, t += DT)
        last = step(f, "G01", base + off[i % off.size()], t);

    // Converged: the filtered value sits near the constant (noise absorbed) and the
    // posterior σ has shrunk well below the seed (multi-epoch information gain).
    EXPECT_NEAR(last.value_m, base, 0.02);
    EXPECT_LT(last.post_sigma_m, 0.03);
    double mean = 0.0, var = 0.0;
    ASSERT_TRUE(f.stateForTest("G01", "L1C", mean, var));
    EXPECT_NEAR(mean, base, 0.02);

    // Every epoch after the first sighting is smoothed: no spurious resets on noise.
    const auto& c = f.counters();
    EXPECT_EQ(c.inits, 1);
    EXPECT_EQ(c.smoothed, 59);
    EXPECT_EQ(c.resets_innovation, 0);
    EXPECT_EQ(c.resets_refpath, 0);
    EXPECT_EQ(c.resets_gap, 0);
}

// --- 2. A reference-path slip resets and passes the raw step through --------------

TEST(PbEmitFilter, ReferencePathSlipResetsAndIsNeverLaundered) {
    NetworkPbEmitFilter f(defaultCfg());
    const double base = -0.20;
    double t = kT0.week * 604800.0 + kT0.tow;
    for (int i = 0; i < 10; ++i, t += DT) step(f, "G07", base, t);   // converge on a constant

    // A +1-cycle slip on the emission-reference arc, flagged as slipped.  The filter
    // emits the raw stepped value instead of smoothing it back toward base.
    const double slipped_val = base + LAM;
    const auto e = step(f, "G07", slipped_val, t, /*slipped=*/true);
    EXPECT_EQ(e.reset_cause, NetworkPbEmitFilter::Cause::kRefPathSlip);
    EXPECT_NEAR(e.value_m, slipped_val, 1e-12);          // raw value survives → emitter jump test fires
    EXPECT_EQ(f.counters().resets_refpath, 1);

    // The filter re-seeded on the fresh value (the next clean epoch smooths from it).
    double mean = 0.0, var = 0.0;
    ASSERT_TRUE(f.stateForTest("G07", "L1C", mean, var));
    EXPECT_NEAR(mean, slipped_val, 1e-12);
}

// --- 3. An arc gap over max_gap_s re-inits ----------------------------------------

TEST(PbEmitFilter, GapBeyondMaxGapReinits) {
    NetworkPbEmitFilter f(defaultCfg());   // max_gap 600 s
    const double base = 1.10;
    double t = kT0.week * 604800.0 + kT0.tow;
    for (int i = 0; i < 5; ++i, t += DT) step(f, "E11", base, t);

    // Return after a 20-minute gap (> 600 s) at a shifted value: a first sighting,
    // not a continued arc; passed through and counted as a gap re-init.
    t += 1200.0;
    const double ret_val = base + 0.3;
    const auto e = step(f, "E11", ret_val, t);
    EXPECT_EQ(e.reset_cause, NetworkPbEmitFilter::Cause::kGap);
    EXPECT_NEAR(e.value_m, ret_val, 1e-12);
    EXPECT_EQ(f.counters().resets_gap, 1);
}

// --- 4. A genuine innovation over k·σ̂_step resets ---------------------------------

TEST(PbEmitFilter, InnovationEventResets) {
    NetworkPbEmitFilter f(defaultCfg());
    const double base = 0.00;
    double t = kT0.week * 604800.0 + kT0.tow;
    for (int i = 0; i < 20; ++i, t += DT) step(f, "G21", base, t);

    // A 0.5 m step with no slip flag, well above k·σ̂_step (k=4, σ̂_step≈seed floor).
    const auto e = step(f, "G21", base + 0.5, t);
    EXPECT_EQ(e.reset_cause, NetworkPbEmitFilter::Cause::kInnovation);
    EXPECT_NEAR(e.value_m, base + 0.5, 1e-12);           // raw value survives
    EXPECT_EQ(f.counters().resets_innovation, 1);
}

// --- 5. End to end with the production emitter: floor cleared, events preserved ---

TEST(PbEmitFilter, ClearsDiscFloorAndPreservesGenuineEventAgainstUnfilteredControl) {
    SsrCorrectionStore store_f, store_u;
    NetworkProductEmitter em_f, em_u;      // both default 0.05 m, the only disc test
    NetworkPbEmitFilter  f(defaultCfg());
    const double base = 0.30;
    // Per-epoch noise whose consecutive steps exceed 0.05 m (so the unfiltered
    // emitter false-alarms) but whose deviation from base stays sub-gate.
    const std::array<double, 6> off{{0.04, -0.045, 0.05, -0.04, 0.045, -0.05}};

    auto emitOne = [](NetworkProductEmitter& em, SsrCorrectionStore& store,
                      const GnssTime& t, double value) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        NetworkSatSolution s; s.sat = "G01"; s.has_clock = true; s.clock_c0_m = 1.0;
        s.phase_bias["L1C"] = NetworkPhaseBias{value, /*integer=*/true, /*wl=*/1};
        sol.sats.push_back(s);
        em.emitToStore(sol, store);
    };

    GnssTime t = kT0;
    double tc = kT0.week * 604800.0 + kT0.tow;
    const int N = 40;
    for (int i = 0; i < N; ++i) {
        const double raw = base + off[i % off.size()];
        const auto e = step(f, "G01", raw, tc);
        emitOne(em_f, store_f, t, e.value_m);   // filtered arm
        emitOne(em_u, store_u, t, raw);         // unfiltered control
        t.tow += DT; tc += DT;
    }

    // Floor cleared: the filtered arm is silent across the noisy window; the
    // unfiltered control false-alarms on most epochs.
    EXPECT_EQ(em_f.discontinuityCounter("G01", "L1C"), 0);
    EXPECT_GE(em_u.discontinuityCounter("G01", "L1C"), 20);
    const int disc_f_pre = em_f.discontinuityCounter("G01", "L1C");
    const int disc_u_pre = em_u.discontinuityCounter("G01", "L1C");

    // A genuine +0.6 m event fires exactly once on both arms.
    const double event_val = base + 0.6;
    const auto ee = step(f, "G01", event_val, tc);
    EXPECT_EQ(ee.reset_cause, NetworkPbEmitFilter::Cause::kInnovation);
    emitOne(em_f, store_f, t, ee.value_m);
    emitOne(em_u, store_u, t, event_val);
    EXPECT_EQ(em_f.discontinuityCounter("G01", "L1C"), disc_f_pre + 1);
    EXPECT_EQ(em_u.discontinuityCounter("G01", "L1C"), disc_u_pre + 1);

    // The filtered arm carried the raw event value onto the wire (not smoothed).
    const auto pb = store_f.getPhaseBias("G01", "L1C");
    ASSERT_TRUE(pb.has_value());
    EXPECT_NEAR(pb->bias_m, event_val, 1e-9);
}

// --- 6. The calibrated gate self-tunes to the noise scale -------------------------

TEST(PbEmitFilter, StepSigmaSelfTunesToNoiseScale) {
    // window_epochs=0 ⇒ accumulate the whole run (deterministic, data-dominated).
    // The probe noise must be Gaussian: the estimator's kChi2GeoMean de-bias is
    // the geo-mean of χ²₁, exact for ê~N(0,σ); a ±s square wave (zero log-spread)
    // reads 1/√kChi2GeoMean ≈ 1.89× high by construction.  Box–Muller on raw
    // mt19937 draws keeps the sequence bit-deterministic across platforms.
    auto run = [](double s) {
        NetworkPbEmitFilter::Config c = defaultCfg();
        c.window_epochs = 0.0;
        NetworkPbEmitFilter f(c);
        std::mt19937 rng(20260714u);
        auto gauss = [&rng]() {
            const double u1 = (static_cast<double>(rng()) + 0.5) / 4294967296.0;
            const double u2 = (static_cast<double>(rng()) + 0.5) / 4294967296.0;
            return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        };
        const double base = 0.10;
        double t = kT0.week * 604800.0 + kT0.tow;
        step(f, "G01", base, t);                       // seed μ at base (first sighting)
        t += DT;
        for (int i = 0; i < 200; ++i, t += DT)
            step(f, "G01", base + s * gauss(), t);
        return f.stepSigmaForTest("G");
    };

    const double sig_low  = run(0.03);
    const double sig_high = run(0.12);
    EXPECT_LT(sig_low, sig_high);            // tracks the injected scale
    EXPECT_NEAR(sig_low, 0.03, 0.015);       // converges near the low scale
    EXPECT_GT(sig_high, 0.08);               // climbed well above the low run
}

// --- v2 (a): the censoring-free feed breaks the gate-scale spiral ----------------

TEST(PbEmitFilter, V2CensoringFreeFeedBreaksTheGateScaleSpiral) {
    // A quasi-static value with a heavy-tailed per-epoch step: a small Gaussian core
    // (0.015 m) plus a 0.12 m step every 4th epoch (the "moderate step at gate
    // scale" class).  With a tight seed (0.02 ⇒ initial gate 0.08 m) the 0.12 m
    // steps start above the gate.  v1 withholds every rejected row from the
    // estimator, so the robust σ̂_step latches to the 0.015 m core, the gate stays
    // near 0.06 m, and the moderate steps are rejected indefinitely (the documented
    // spiral; the same censored-estimator fixed point occurs in the field at 300 s
    // through forgetting and pooling).  v2 feeds the innovation-gated rows, so
    // σ̂_step recovers the mixture scale, the gate opens past 0.12 m, and the
    // resets drop.
    const double base = 0.30, core = 0.015, spike = 0.12;
    auto drive = [&](NetworkPbEmitFilter& f) {
        Gauss g(20260714u);
        double t = kT0.week * 604800.0 + kT0.tow;
        for (int i = 0; i < 400; ++i, t += DT) {
            double v = base + core * g();
            if (i % 4 == 2) v += ((i / 4) % 2 == 0 ? +spike : -spike);  // 0.12 m every 4th ep
            step(f, "G01", v, t);
        }
    };
    NetworkPbEmitFilter::Config c1 = defaultCfg();
    c1.window_epochs = 0.0;   // accumulate the whole run (deterministic)
    c1.meas_sigma_m  = 0.02;  // tight seed ⇒ the 0.12 m steps start above the gate
    NetworkPbEmitFilter::Config c2 = c1; c2.gate_v2 = true;

    NetworkPbEmitFilter fv1(c1), fv2(c2);
    drive(fv1); drive(fv2);
    const auto& n1 = fv1.counters();
    const auto& n2 = fv2.counters();
    const double s1 = fv1.stepSigmaForTest("G");
    const double s2 = fv2.stepSigmaForTest("G");

    // v1: σ̂_step censored low (latched to the 0.015 m core); v2: recovers the scale.
    EXPECT_LT(s1, 0.030);
    EXPECT_GT(s2, 0.035);
    EXPECT_GT(s2, s1);

    // v1 keeps rejecting the moderate steps (100 in 400 epochs); v2 resets only during
    // warm-up.
    EXPECT_GE(n1.resets_innovation, 80);
    EXPECT_LE(n2.resets_innovation, 25);
    EXPECT_LT(n2.resets_innovation, n1.resets_innovation);

    // The censoring-free feed accounting: v1 feeds smoothed rows only; v2 also feeds
    // the innovation-gated rows (n_fed_all_rows telemetry).
    EXPECT_EQ(n1.fed_rows, n1.smoothed);
    EXPECT_EQ(n2.fed_rows, n2.smoothed + n2.resets_innovation);
    EXPECT_GT(n2.fed_rows, n1.fed_rows);
}

// --- v2 (b): one k works at both 30 s and 300 s (the purpose of the card) ---------

TEST(PbEmitFilter, V2GenuineEventFiresAtBothCadencesWithOneK) {
    // Same config (k=4 default, default seed), v2 on: a genuine 0.6 m event fires at
    // both 30 s and 300 s cadence.  The model-scaled gate k·√(Ppred+R) carries dt
    // through the prior's q·gap term, so no per-cadence retune is needed.
    auto eventCause = [](double dt) {
        NetworkPbEmitFilter::Config c = defaultCfg();
        c.gate_v2 = true; c.window_epochs = 0.0;
        NetworkPbEmitFilter f(c);
        Gauss g(4242u);
        const double base = 0.0, sigma_step = 0.06;
        double t = kT0.week * 604800.0 + kT0.tow;
        for (int i = 0; i < 150; ++i, t += dt)          // warm σ̂_step to ~0.06 m
            step(f, "G05", base + sigma_step * g(), t);
        return step(f, "G05", base + 0.6, t).reset_cause;  // genuine 0.6 m event
    };
    EXPECT_EQ(eventCause(30.0),  NetworkPbEmitFilter::Cause::kInnovation);
    EXPECT_EQ(eventCause(300.0), NetworkPbEmitFilter::Cause::kInnovation);
}

// --- v2 (§3): a sub-gate slip passes as a bounded transient toward the new level

TEST(PbEmitFilter, V2SubGateSlipSmoothsThroughBoundedNotLaundered) {
    // Residual-risk envelope (§3): an unflagged step below k·σ̂_step (the ~2σ–kσ
    // miss band, e.g. a single-cycle L1 step the upstream funnel missed) does not
    // reset, so no discontinuity reaches the consumer, and is not snapped back: the
    // filter tracks toward the true new level over ~1/K epochs (a bounded, sub-gate
    // transient), as the header documents for v2 and for v1 as designed.
    NetworkPbEmitFilter::Config c = defaultCfg();
    c.gate_v2 = true; c.window_epochs = 0.0;
    NetworkPbEmitFilter f(c);
    Gauss g(7u);
    const double base = 0.0, sigma_step = 0.06;
    double t = kT0.week * 604800.0 + kT0.tow;
    for (int i = 0; i < 150; ++i, t += DT)              // warm σ̂_step to ~0.06 m
        step(f, "G09", base + sigma_step * g(), t);

    const double sig  = f.stepSigmaForTest("G");
    const double sub  = 0.15;                            // in the (2σ, kσ) miss band
    ASSERT_GT(sub, 2.0 * sig);
    ASSERT_LT(sub, 4.0 * sig);                           // sub-gate (gate ≈ 4σ̂ ≈ 0.24 m)
    const double newlvl = base + sub;
    const long long resets0 = f.counters().resets_innovation;
    double pre_mean = 0.0, pre_var = 0.0;
    ASSERT_TRUE(f.stateForTest("G09", "L1C", pre_mean, pre_var));  // ≈base ± warmup residual

    const auto e0 = step(f, "G09", newlvl, t); t += DT;  // the missed step: no disc
    EXPECT_NE(e0.reset_cause, NetworkPbEmitFilter::Cause::kInnovation);
    EXPECT_LT(e0.value_m, newlvl);                       // smoothed toward, not snapped to raw
    EXPECT_GT(e0.value_m, pre_mean);                     // ... and moving toward the new level

    for (int i = 0; i < 500; ++i, t += DT)               // hold the new level, clean
        step(f, "G09", newlvl, t);

    // No discontinuity through the whole transient (the consumer sees none), and the
    // filter converged up to the true new level (not pulled back to base, and not
    // past it).
    EXPECT_EQ(f.counters().resets_innovation, resets0);
    double mean = 0.0, var = 0.0;
    ASSERT_TRUE(f.stateForTest("G09", "L1C", mean, var));
    EXPECT_GT(mean, base + 0.5 * sub);                   // tracked most of the way up
    EXPECT_LE(mean, newlvl + 1e-6);                      // bounded: no overshoot
}

}  // namespace
}  // namespace GnssProcessor
