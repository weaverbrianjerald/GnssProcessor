/**
 * @file test_pb_emit_filter.cpp
 * @brief Gates for the PB emission-continuity filter (@ref NetworkPbEmitFilter,
 *        the M5-prerequisite smoother on the re-based emitted phase biases).
 *
 * The leaf is driven standalone on synthetic single-key streams of known truth
 * (the @ref NetworkEmitRebase / @ref NetworkCarryVce precedent), and end-to-end
 * into the PRODUCTION emitter (@ref NetworkProductEmitter, whose 0.05 m jump test
 * stays the SOLE, unchanged discontinuity authority) so the disc-floor claim is
 * exercised against the real counter.  Gates:
 *
 *   1. A quasi-static value + per-epoch noise is smoothed (the filtered value
 *      tracks the constant, the posterior σ shrinks below the seed, no resets).
 *   2. A reference-path slip flag ⇒ RESET: the raw (stepped) value passes through
 *      untouched (NEVER laundered — invariant #1) and the cause is recorded.
 *   3. An arc gap over max_gap_s ⇒ re-init (value passes through).
 *   4. A genuine innovation over k·σ̂_step ⇒ reset (raw value passes through).
 *   5. END-TO-END: on a noisy quasi-static stream the PRODUCTION emitter is
 *      SILENT with the filter (floor cleared) but fires on nearly every epoch
 *      UNFILTERED (negative control); a genuine >0.5 m event fires EXACTLY ONCE
 *      on both arms (preserved 1:1 — the anti-launder falsifier).
 *   6. The k·σ̂_step gate self-tunes: σ̂_step tracks the injected noise scale.
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

// Run the filter on ONE key/value at continuous time t; returns the filtered Entry.
NetworkPbEmitFilter::Entry step(NetworkPbEmitFilter& f, const std::string& sat,
                                double value, double t, bool slipped = false) {
    std::vector<NetworkPbEmitFilter::Entry> e(1);
    e[0].sat = sat; e[0].signal = "L1C"; e[0].value_m = value; e[0].slipped = slipped;
    f.filterEpoch(e, t);
    return e[0];
}

// --- 1. A quasi-static value + noise is smoothed; σ shrinks; no resets -----------

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

    // Every epoch after the first sighting smoothed — no spurious resets on noise.
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

    // A +1-cycle slip on the emission-reference arc, FLAGGED slipped.  The filter
    // must emit the raw stepped value (not smooth it back toward base).
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
    // not a continued arc — passed through, counted as a gap re-init.
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

    // A 0.5 m step with NO slip flag — well above k·σ̂_step (k=4, σ̂_step≈seed floor).
    const auto e = step(f, "G21", base + 0.5, t);
    EXPECT_EQ(e.reset_cause, NetworkPbEmitFilter::Cause::kInnovation);
    EXPECT_NEAR(e.value_m, base + 0.5, 1e-12);           // raw value survives
    EXPECT_EQ(f.counters().resets_innovation, 1);
}

// --- 5. END-TO-END with the PRODUCTION emitter: floor cleared, events preserved ---

TEST(PbEmitFilter, ClearsDiscFloorAndPreservesGenuineEventAgainstUnfilteredControl) {
    SsrCorrectionStore store_f, store_u;
    NetworkProductEmitter em_f, em_u;      // both default 0.05 m — the SOLE disc authority
    NetworkPbEmitFilter  f(defaultCfg());
    const double base = 0.30;
    // Per-epoch noise whose CONSECUTIVE steps exceed 0.05 m (so the unfiltered
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

    // A genuine +0.6 m event: fires EXACTLY ONCE on BOTH arms (preserved 1:1).
    const double event_val = base + 0.6;
    const auto ee = step(f, "G01", event_val, tc);
    EXPECT_EQ(ee.reset_cause, NetworkPbEmitFilter::Cause::kInnovation);
    emitOne(em_f, store_f, t, ee.value_m);
    emitOne(em_u, store_u, t, event_val);
    EXPECT_EQ(em_f.discontinuityCounter("G01", "L1C"), disc_f_pre + 1);
    EXPECT_EQ(em_u.discontinuityCounter("G01", "L1C"), disc_u_pre + 1);

    // The filtered arm carried the raw event value onto the wire (not laundered).
    const auto pb = store_f.getPhaseBias("G01", "L1C");
    ASSERT_TRUE(pb.has_value());
    EXPECT_NEAR(pb->bias_m, event_val, 1e-9);
}

// --- 6. The calibrated gate self-tunes to the noise scale -------------------------

TEST(PbEmitFilter, StepSigmaSelfTunesToNoiseScale) {
    // window_epochs=0 ⇒ accumulate the whole run (deterministic, data-dominated).
    // The probe noise MUST be Gaussian: the estimator's kChi2GeoMean de-bias is
    // the geo-mean of χ²₁, exact for ê~N(0,σ) — a ±s square wave (zero log-spread)
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

}  // namespace
}  // namespace GnssProcessor
