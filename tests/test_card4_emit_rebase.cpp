/**
 * @file test_card4_emit_rebase.cpp
 * @brief Card 4 (M1) phase D — Gate 5 (EmitRebaseInteger) for the emit-layer
 *        frame re-base (docs/SPEC_card4_invariant_carry.md §10 (gate 5 block)).
 *
 * Drives the PRODUCTION re-base (`NetworkEmitRebase`, the exact object
 * ApplicationNetwork runs before every emit) into the PRODUCTION emitter
 * (`NetworkProductEmitter`, whose 0.05 m phase-bias jump test remains the sole
 * discontinuity authority), on hand-built solution epochs so the assertions are
 * isolated from network-solve noise.  The gate asserts:
 *
 *   1. A forced datum re-realization (the same double differences realized in a
 *      different S-basis frame: a per-satellite integer×λ + one common fractional
 *      δ per component) ⇒ emitted per-key steps EXACTLY 0 after re-basing and the
 *      disc counter silent.
 *   2. NEGATIVE control — re-base disabled ⇒ the integer-class (≥1 λ) step is
 *      visible on the same assertion (a numeric fail, not a throw).
 *   3. A slip on the emission-reference (spanning-tree) path ⇒ the integer step
 *      SURVIVES the re-base + the disc fires (the re-base must NOT launder genuine
 *      events — standing invariant #1), while a clean peer stays silent.
 *   4. Independent components carry independent δ (no cross-component leakage) and
 *      a first sighting is a genuine event the re-base passes through untouched.
 */

#include <gtest/gtest.h>

#include "products/NetworkEmitRebase.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

const GnssTime kT0{2313, 259200.0};
constexpr double LAM = Constants::GPS_L1_WAVELENGTH;   // ~0.190294 m (well above the 0.05 m gate)

// One (const,signal)=GPS/L1C component of re-base entries from (sat, bias) pairs.
std::vector<NetworkEmitRebase::Entry> makeEntries(
    const std::vector<std::pair<std::string, double>>& sats,
    const std::set<std::string>& slipped = {}, int component = 1) {
    std::vector<NetworkEmitRebase::Entry> v;
    for (const auto& [sat, b] : sats) {
        NetworkEmitRebase::Entry e;
        e.sat = sat; e.signal = "L1C"; e.component = component;
        e.lambda_m = LAM; e.bias_m = b;
        e.slipped = slipped.count(sat) != 0;
        v.push_back(e);
    }
    return v;
}

// A NetworkSolutionEpoch of L1C phase biases from (sat, bias) pairs.
NetworkSolutionEpoch makeEpoch(const GnssTime& t,
                               const std::vector<std::pair<std::string, double>>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    for (const auto& [sat, b] : sats) {
        NetworkSatSolution s; s.sat = sat; s.has_clock = true; s.clock_c0_m = 1.0;
        s.phase_bias["L1C"] = NetworkPhaseBias{b, /*integer=*/true, /*wl=*/1};
        sol.sats.push_back(s);
    }
    return sol;
}

// --- 1. The re-base arithmetic: integer×λ + common fractional zero the step ------

TEST(Card4EmitRebase, IntegerAndCommonFractionalZeroTheStep) {
    NetworkEmitRebase rb;
    const double b1 = -0.70, b2 = 0.25;

    // Frame N is a first sighting for both keys ⇒ passes through raw (seeds the frame).
    auto eN = makeEntries({{"G01", b1}, {"G02", b2}});
    rb.rebaseEpoch(eN);
    EXPECT_NEAR(eN[0].bias_m, b1, 1e-12);
    EXPECT_NEAR(eN[1].bias_m, b2, 1e-12);

    // Frame N+1: the SAME estimable solution re-realized — per-sat integer + one
    // common fractional δ for the component.  Verify it really is the same DD first:
    const int k1 = 3, k2 = -2; const double delta = 0.037;
    const double raw1 = b1 + k1 * LAM + delta, raw2 = b2 + k2 * LAM + delta;
    const double dd_move = (raw1 - raw2) - (b1 - b2);                 // = (k1-k2)*λ exactly
    EXPECT_NEAR(dd_move - std::round(dd_move / LAM) * LAM, 0.0, 1e-9); // δ cancels; integer·λ only

    auto e1 = makeEntries({{"G01", raw1}, {"G02", raw2}});
    rb.rebaseEpoch(e1);
    EXPECT_NEAR(e1[0].bias_m, b1, 1e-9);   // re-based exactly back onto frame N
    EXPECT_NEAR(e1[1].bias_m, b2, 1e-9);

    EXPECT_EQ(rb.counters().integer_cycles, 5);   // |3| + |-2|
    EXPECT_EQ(rb.counters().keys_rebased, 2);
    double lv = 0.0;
    ASSERT_TRUE(rb.lastEmittedForTest("G01", "L1C", lv));
    EXPECT_NEAR(lv, b1, 1e-9);
}

// --- 2. Re-base ON keeps the emitter disc silent across datum churn --------------

TEST(Card4EmitRebase, RebaseKeepsDiscSilentAcrossDatumChurn) {
    SsrCorrectionStore store;
    NetworkProductEmitter em;                 // default 0.05 m threshold
    NetworkEmitRebase rb;
    const double b1 = -0.70, b2 = 0.25;

    auto emit = [&](const GnssTime& t,
                    const std::vector<std::pair<std::string, double>>& sats,
                    const std::set<std::string>& slip = {}) {
        auto ent = makeEntries(sats, slip);
        rb.rebaseEpoch(ent);                  // PRODUCTION re-base
        std::map<std::string, double> out;
        for (const auto& e : ent) out[e.sat] = e.bias_m;
        std::vector<std::pair<std::string, double>> reb;
        for (const auto& [s, raw] : sats) { (void)raw; reb.push_back({s, out[s]}); }
        em.emitToStore(makeEpoch(t, reb), store);   // PRODUCTION emitter (disc authority)
    };

    GnssTime t = kT0;
    emit(t, {{"G01", b1}, {"G02", b2}});
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 0);   // first sight

    t.tow += 30.0;
    emit(t, {{"G01", b1 + 3 * LAM + 0.037}, {"G02", b2 - 2 * LAM + 0.037}});
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 0);   // datum churn absorbed → silent
    EXPECT_EQ(em.discontinuityCounter("G02", "L1C"), 0);

    auto pb = store.getPhaseBias("G01", "L1C");
    ASSERT_TRUE(pb.has_value());
    EXPECT_NEAR(pb->bias_m, b1, 1e-9);                     // the wire carries the re-based value
}

// --- 3. NEGATIVE control: no re-base ⇒ the integer-class step fires disc ----------

TEST(Card4EmitRebase, NoRebaseIntegerStepFiresDisc) {
    SsrCorrectionStore store;
    NetworkProductEmitter em;
    const double b1 = -0.70, b2 = 0.25;

    em.emitToStore(makeEpoch(kT0, {{"G01", b1}, {"G02", b2}}), store);
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 0);

    GnssTime t = kT0; t.tow += 30.0;
    // The identical churned frame, emitted RAW (re-base disabled = the OFF/A-B control).
    em.emitToStore(makeEpoch(t, {{"G01", b1 + 3 * LAM + 0.037}, {"G02", b2 - 2 * LAM + 0.037}}),
                   store);
    EXPECT_GE(em.discontinuityCounter("G01", "L1C"), 1);   // 3λ+δ ≈ 0.61 m ≫ 0.05 m
    EXPECT_GE(em.discontinuityCounter("G02", "L1C"), 1);   // 2λ-δ ≈ 0.34 m ≫ 0.05 m
}

// --- 4. Genuine event: a reference-path slip survives + fires disc ----------------

TEST(Card4EmitRebase, ReferencePathSlipSurvivesAndFiresDisc) {
    SsrCorrectionStore store;
    NetworkProductEmitter em;
    NetworkEmitRebase rb;
    const double b1 = -0.70, b2 = 0.25;

    auto emit = [&](const GnssTime& t,
                    const std::vector<std::pair<std::string, double>>& sats,
                    const std::set<std::string>& slip = {}) {
        auto ent = makeEntries(sats, slip);
        rb.rebaseEpoch(ent);
        std::map<std::string, double> out;
        for (const auto& e : ent) out[e.sat] = e.bias_m;
        std::vector<std::pair<std::string, double>> reb;
        for (const auto& [s, raw] : sats) { (void)raw; reb.push_back({s, out[s]}); }
        em.emitToStore(makeEpoch(t, reb), store);
    };

    GnssTime t = kT0;
    emit(t, {{"G01", b1}, {"G02", b2}});             // seed
    t.tow += 30.0;
    emit(t, {{"G01", b1}, {"G02", b2}});             // steady
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 0);

    // G01 slips +1 cycle on its emission-reference (tree) arc — flagged slipped.  The
    // re-base must leave its integer step intact (k_s=0), NOT launder it.
    t.tow += 30.0;
    emit(t, {{"G01", b1 + 1 * LAM}, {"G02", b2}}, /*slip=*/{"G01"});
    EXPECT_GE(em.discontinuityCounter("G01", "L1C"), 1);   // step survived → disc fired
    EXPECT_EQ(em.discontinuityCounter("G02", "L1C"), 0);   // clean peer stays silent
    EXPECT_GE(rb.counters().slips_preserved, 1);

    // The emitted G01 value stepped by ~1 cycle (not laundered back to b1).
    auto pb = store.getPhaseBias("G01", "L1C");
    ASSERT_TRUE(pb.has_value());
    EXPECT_NEAR(pb->bias_m, b1 + LAM, 1e-9);
}

// --- 5. Independent components carry independent δ (no cross-leak) ----------------

TEST(Card4EmitRebase, IndependentComponentsDoNotLeakDelta) {
    NetworkEmitRebase rb;
    // Two components (ids 1 and 2), two sats each.  Seed the frame.
    std::vector<NetworkEmitRebase::Entry> e0 = {
        {"G01", "L1C", 1, LAM, -0.70, false}, {"G02", "L1C", 1, LAM, 0.25, false},
        {"G05", "L1C", 2, LAM, 0.10, false},  {"G06", "L1C", 2, LAM, -0.33, false}};
    rb.rebaseEpoch(e0);

    // Component 1 churns by common +0.04 (± integers); component 2 by common -0.02.
    std::vector<NetworkEmitRebase::Entry> e1 = {
        {"G01", "L1C", 1, LAM, -0.70 + 0.04 + 1 * LAM, false},
        {"G02", "L1C", 1, LAM,  0.25 + 0.04 - 1 * LAM, false},
        {"G05", "L1C", 2, LAM,  0.10 - 0.02 + 2 * LAM, false},
        {"G06", "L1C", 2, LAM, -0.33 - 0.02,           false}};
    rb.rebaseEpoch(e1);

    EXPECT_NEAR(e1[0].bias_m, -0.70, 1e-9);   // each re-bases to ITS OWN frame-N value
    EXPECT_NEAR(e1[1].bias_m,  0.25, 1e-9);   // component 2's -0.02 does not leak here
    EXPECT_NEAR(e1[2].bias_m,  0.10, 1e-9);
    EXPECT_NEAR(e1[3].bias_m, -0.33, 1e-9);
    EXPECT_EQ(rb.counters().components, 4);   // 2 per epoch × 2 epochs
}

// --- 6. A first sighting is a genuine event: passed through, disc silent once -----

TEST(Card4EmitRebase, FirstSightingIsPassedThroughAndFirstSightSilent) {
    SsrCorrectionStore store;
    NetworkProductEmitter em;
    NetworkEmitRebase rb;

    // A single fresh key with no baseline re-bases against nothing (δ=0, k=0).
    auto ent = makeEntries({{"G09", 1.234}});
    rb.rebaseEpoch(ent);
    EXPECT_NEAR(ent[0].bias_m, 1.234, 1e-12);   // birth passes through untouched
    EXPECT_EQ(rb.counters().integer_cycles, 0);

    em.emitToStore(makeEpoch(kT0, {{"G09", 1.234}}), store);
    EXPECT_EQ(em.discontinuityCounter("G09", "L1C"), 0);   // first sight is silent by construction
}

}  // namespace
}  // namespace GnssProcessor
