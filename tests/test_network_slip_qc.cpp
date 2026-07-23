/**
 * @file test_network_slip_qc.cpp
 * @brief First dedicated unit coverage of the shared RAW slip-observable seam
 *        (slip/NetworkRawObservables) that Cards L and G of the 2026-07-21
 *        slip-QC hardening decide on — docs/SPEC_slip_qc_hardening.md §3
 *        (MUST M6) and §5 (MUST G1).
 *
 * Three layers are exercised, bottom-up:
 *
 *   1. The pure helpers — @c networkRobustStepScale (1.4826·median|x|) and
 *      @c networkLliCredibilityStep (the two-state hysteresis).  Pure
 *      functions, so the state machine is pinned without a station.
 *   2. @c networkComputeRawObservables — primary-pair selection (the two
 *      lowest DISTINCT bands whose phase AND matching "C"+tail code are both
 *      present), the MW / GF / phase-minus-code arithmetic and the wide-lane
 *      wavelength the MW floor is expressed in.
 *   3. @c networkQualifyRawObservables — the epoch-to-epoch adjudication:
 *      LLI credibility (chatty vs quiet station), corroboration by a real
 *      wide-lane step, and the Card-G arc-resumption instrument.
 *
 * Every satellite here is synthesized from a textbook geometry
 *   L_i = (R − I_i)/λ_i + N_i  [cycles],  C_i = R + I_i  [m],  I_i = I_1(λ_i/λ_1)²
 * under which the Melbourne-Wübbena combination collapses EXACTLY to
 * λ_WL·(N_1 − N_2): geometry, clocks and the first-order ionosphere all cancel.
 * A wide-lane slip injected as ΔN therefore appears as a step of exactly
 * ΔN_WL·λ_WL metres — a designed margin against the 0.5-cycle physical floor,
 * not a derived one.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "slip/NetworkRawObservables.hpp"

#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
// Synthetic geometry (GPS L1 / L2 / L5)
// ---------------------------------------------------------------------------

const double kL1 = Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ;  // ≈ 0.190294 m
const double kL2 = Constants::SPEED_OF_LIGHT / Constants::GPS_L2_FREQ;  // ≈ 0.244210 m
const double kL5 = Constants::SPEED_OF_LIGHT / Constants::GPS_L5_FREQ;  // ≈ 0.254828 m

/// Wide-lane wavelength of the L1/L2 primary pair: 1/(1/λ1 − 1/λ2) ≈ 0.8619 m.
const double kLambdaWl = 1.0 / (1.0 / kL1 - 1.0 / kL2);

constexpr double kRangeM  = 22'000'000.0;   ///< Nominal geometric range.
constexpr double kIonoL1M = 5.0;            ///< First-order L1 ionospheric delay.

/// Append one band's raw phase (cycles) and matching code (metres), built from
/// the designed geometry above.  @p code_bias_m shifts the code only (a benign
/// multipath-like wobble), which moves MW without touching GF.
void addBand(PppObservation& obs, const std::string& phase_sig, double lambda_m,
             double amb_cyc, double code_bias_m = 0.0) {
    const double ratio = lambda_m / kL1;
    const double iono  = kIonoL1M * ratio * ratio;
    obs.raw_observed[phase_sig] = (kRangeM - iono) / lambda_m + amb_cyc;
    obs.raw_observed["C" + phase_sig.substr(1)] = kRangeM + iono + code_bias_m;
}

/// Dual-band (L1C + L2W) GPS satellite with matching codes.
PppObservation makeDualObs(const std::string& sat, double n1_cyc, double n2_cyc,
                           bool lli = false, double code_bias_m = 0.0,
                           double elev_deg = 45.0) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system    = System::GPS;
    obs.time      = GnssTime{ 2400, 0.0 };
    obs.elevation = elev_deg;
    addBand(obs, "L1C", kL1, n1_cyc, code_bias_m);
    addBand(obs, "L2W", kL2, n2_cyc, code_bias_m);
    if (lli) { obs.lli_flags["L1C"] = 1; obs.lli_flags["L2W"] = 1; }
    return obs;
}

/// One epoch of one station, wrapped for the seam's per-station containers.
struct RawHarness {
    LogManager                  log_manager;
    PppUtils                    utils{ log_manager };
    SlipQcParams                params;
    std::vector<RawSlipHistory> hist;
    RawSlipEpoch                epoch;
    SlipQcCounters              counters;
    long long                   epoch_idx{ 0 };

    /// Compute + qualify one processed epoch, exactly as the run loop does.
    void run(const std::vector<std::vector<PppObservation>>& modeled) {
        const int n = static_cast<int>(modeled.size());
        epoch.reset(static_cast<std::size_t>(n));
        const std::vector<bool> present(static_cast<std::size_t>(n), true);
        networkComputeRawObservables(modeled, present, n, utils, epoch);
        networkQualifyRawObservables(modeled, present, n, epoch_idx, params,
                                     hist, epoch, counters);
        ++epoch_idx;
    }

    /// This epoch's verdict for one (station, satellite).
    const RawSlipVerdict& verdict(std::size_t sta, const std::string& sat) const {
        return epoch.verdict.at(sta).at(sat);
    }
};

/// Locate a satellite's row in a station's raw-observable list.
const RawSlipObs* findRow(const std::vector<RawSlipObs>& rows, const std::string& sat) {
    for (const auto& r : rows) if (r.satellite == sat) return &r;
    return nullptr;
}

// ===========================================================================
// networkRobustStepScale
// ===========================================================================

TEST(SlipQcRobustScale, EmptySampleHasNoEmpiricalInformation) {
    // 0 is the "no information yet" sentinel: the caller then falls back to
    // the PHYSICAL floor rather than to a stochastic-model sigma (rule 4).
    std::vector<double> empty;
    EXPECT_DOUBLE_EQ(networkRobustStepScale(empty), 0.0);
}

TEST(SlipQcRobustScale, OddSampleIsMadToSigmaTimesTheMedian) {
    std::vector<double> v{ 0.3, 0.1, 0.2 };
    EXPECT_DOUBLE_EQ(networkRobustStepScale(v), 1.4826 * 0.2);
}

TEST(SlipQcRobustScale, EvenSampleTakesTheUpperMiddleOrderStatistic) {
    // nth_element at size/2 — the file's convention, deliberately NOT the
    // averaging median of RobustStats::median.  Pinned so a future "cleanup"
    // that swaps implementations has to notice the scale would change.
    std::vector<double> v{ 4.0, 1.0, 3.0, 2.0 };
    EXPECT_DOUBLE_EQ(networkRobustStepScale(v), 1.4826 * 3.0);
}

TEST(SlipQcRobustScale, SingleElementSample) {
    std::vector<double> v{ 0.05 };
    EXPECT_DOUBLE_EQ(networkRobustStepScale(v), 1.4826 * 0.05);
}

// ===========================================================================
// networkLliCredibilityStep — the two-state hysteresis
// ===========================================================================

TEST(SlipQcCredibility, ArmingGateFreezesTheVerdict) {
    SlipQcParams p;
    p.chatter_min_epochs = 3;
    // Below the arming gate NOTHING moves — neither a single chattering epoch
    // demoting a station nor a single quiet epoch rehabilitating one.
    EXPECT_TRUE(networkLliCredibilityStep(true, 1.0, 2, p));
    EXPECT_FALSE(networkLliCredibilityStep(false, 0.0, 2, p));
}

TEST(SlipQcCredibility, DemotesOnlyAboveTheHighThreshold) {
    SlipQcParams p;   // hi = 0.6, lo = 0.3, min_epochs = 1
    EXPECT_FALSE(networkLliCredibilityStep(true, 0.95, 5, p));  // implausible
    EXPECT_TRUE(networkLliCredibilityStep(true, 0.61, 5, p) == false);
    EXPECT_TRUE(networkLliCredibilityStep(true, 0.60, 5, p));   // boundary: strict >
    EXPECT_TRUE(networkLliCredibilityStep(true, 0.45, 5, p));   // inside the band
    EXPECT_TRUE(networkLliCredibilityStep(true, 0.05, 5, p));
}

TEST(SlipQcCredibility, PromotesOnlyBelowTheLowThreshold) {
    SlipQcParams p;
    EXPECT_FALSE(networkLliCredibilityStep(false, 0.45, 5, p));  // hysteresis band
    EXPECT_FALSE(networkLliCredibilityStep(false, 0.30, 5, p));  // boundary: strict <
    EXPECT_TRUE(networkLliCredibilityStep(false, 0.29, 5, p));
    EXPECT_TRUE(networkLliCredibilityStep(false, 0.00, 5, p));
}

TEST(SlipQcCredibility, TheBandIsStickyInBothDirections) {
    SlipQcParams p;
    // The SAME fraction inside (lo, hi) leaves either state untouched — that
    // is the whole point of the hysteresis, and it is why the machine never
    // has a third "ignore" state to fall into.
    for (const double frac : { 0.31, 0.45, 0.59 }) {
        EXPECT_TRUE(networkLliCredibilityStep(true, frac, 5, p)) << frac;
        EXPECT_FALSE(networkLliCredibilityStep(false, frac, 5, p)) << frac;
    }
}

// ===========================================================================
// networkComputeRawObservables
// ===========================================================================

// Primary-pair selection and the raw arithmetic, over a two-station epoch
// covering all four shapes the seam must handle: a triple-band satellite (the
// two LOWEST distinct bands win), a dual-band satellite, a single-band
// satellite (dual == false but l1_c1_m still meaningful), and a phase with no
// matching code (no row at all).
TEST(SlipQcRawObservables, PrimaryPairSelectionAndCombinations) {
    LogManager log_manager;
    PppUtils   utils{ log_manager };

    // Station 0 --------------------------------------------------------
    // G01: L1C/L2W/L5Q all coded -> the two lowest distinct bands (1, 2) win.
    PppObservation g01{};
    g01.satellite = "G01";
    g01.system    = System::GPS;
    g01.elevation = 55.0;
    addBand(g01, "L1C", kL1, 1234.0);
    addBand(g01, "L2W", kL2, 987.0);
    addBand(g01, "L5Q", kL5, 555.0);

    // G02: single band -> not dual, but the phase-minus-code is still real.
    PppObservation g02{};
    g02.satellite = "G02";
    g02.system    = System::GPS;
    g02.elevation = 20.0;
    addBand(g02, "L1C", kL1, 4321.0);

    // G03: phase with NO matching code -> contributes no row whatsoever.
    PppObservation g03{};
    g03.satellite = "G03";
    g03.system    = System::GPS;
    g03.elevation = 33.0;
    g03.raw_observed["L1C"] = 1.0e8;
    g03.raw_observed["L2W"] = 0.9e8;

    // Station 1 --------------------------------------------------------
    const PppObservation g10 = makeDualObs("G10", 100.0, 60.0);

    const std::vector<std::vector<PppObservation>> modeled = { { g01, g02, g03 },
                                                               { g10 } };
    const std::vector<bool> present{ true, true };
    RawSlipEpoch ep;
    ep.reset(2);
    networkComputeRawObservables(modeled, present, 2, utils, ep);

    ASSERT_TRUE(ep.computed);
    ASSERT_EQ(ep.rows.size(), 2u);
    EXPECT_EQ(ep.rows[0].size(), 2u) << "G03 has no coded phase and must not appear";
    EXPECT_EQ(findRow(ep.rows[0], "G03"), nullptr);

    // --- G01: the two lowest DISTINCT bands, not merely the first two keys.
    const RawSlipObs* r1 = findRow(ep.rows[0], "G01");
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r1->sig1, "L1C");
    EXPECT_EQ(r1->sig2, "L2W");
    EXPECT_TRUE(r1->dual);
    EXPECT_DOUBLE_EQ(r1->elev_deg, 55.0);
    EXPECT_NEAR(r1->lambda_wl_m, kLambdaWl, 1e-12);
    EXPECT_NEAR(r1->lambda_wl_m, 0.8619, 1e-4);   // the documented GPS L1/L2 value

    // MW collapses to λ_WL·(N1 − N2) exactly under this geometry.
    EXPECT_NEAR(r1->mw_m, kLambdaWl * (1234.0 - 987.0), 1e-6);
    // GF = (I2 − I1) + λ1·N1 − λ2·N2.
    const double iono2 = kIonoL1M * (kL2 / kL1) * (kL2 / kL1);
    EXPECT_NEAR(r1->gf_m, (iono2 - kIonoL1M) + kL1 * 1234.0 - kL2 * 987.0, 1e-6);
    // Phase-minus-code on the primary signal = λ1·N1 − 2·I1.
    EXPECT_NEAR(r1->l1_c1_m, kL1 * 1234.0 - 2.0 * kIonoL1M, 1e-6);
    EXPECT_EQ(r1->lli1, 0);
    EXPECT_EQ(r1->lli2, 0);

    // --- G02: single band.
    const RawSlipObs* r2 = findRow(ep.rows[0], "G02");
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r2->sig1, "L1C");
    EXPECT_TRUE(r2->sig2.empty());
    EXPECT_FALSE(r2->dual);
    EXPECT_DOUBLE_EQ(r2->mw_m, 0.0);
    EXPECT_DOUBLE_EQ(r2->gf_m, 0.0);
    EXPECT_DOUBLE_EQ(r2->lambda_wl_m, 0.0);
    EXPECT_TRUE(std::isfinite(r2->l1_c1_m));
    EXPECT_NEAR(r2->l1_c1_m, kL1 * 4321.0 - 2.0 * kIonoL1M, 1e-6);

    // --- Station 1 is independent and complete.
    ASSERT_EQ(ep.rows[1].size(), 1u);
    EXPECT_EQ(ep.rows[1][0].satellite, "G10");
    EXPECT_TRUE(ep.rows[1][0].dual);
    EXPECT_NEAR(ep.rows[1][0].mw_m, kLambdaWl * (100.0 - 60.0), 1e-6);
}

// The LLI bytes ride through unmodified per band — the raw evidence Card L
// qualifies (it never rewrites it).
TEST(SlipQcRawObservables, LliBytesArePassedThroughPerBand) {
    LogManager log_manager;
    PppUtils   utils{ log_manager };

    PppObservation obs = makeDualObs("G05", 10.0, 7.0);
    obs.lli_flags["L1C"] = 1;    // loss of lock on the primary only
    obs.lli_flags["L2W"] = 0;

    const std::vector<std::vector<PppObservation>> modeled = { { obs } };
    RawSlipEpoch ep;
    ep.reset(1);
    networkComputeRawObservables(modeled, std::vector<bool>{ true }, 1, utils, ep);

    ASSERT_EQ(ep.rows[0].size(), 1u);
    EXPECT_EQ(ep.rows[0][0].lli1, 1);
    EXPECT_EQ(ep.rows[0][0].lli2, 0);
}

// ===========================================================================
// networkQualifyRawObservables — Card L (M6)
// ===========================================================================

// The audit's four JPN receivers assert LLI on essentially every tracked
// satellite every epoch with zero observable jump; a quiet station asserts it
// on one.  The credibility state must separate them PER STATION — demoting the
// chatty one (so an uncorroborated assertion there can be suppressed) while
// leaving LLI decisive at the quiet one (M1: the G23@GAMB class of catches
// where MW and IF genuinely cannot see the slip).
TEST(SlipQcQualify, ChattyStationDemotesWhileQuietStationStaysCredible) {
    RawHarness h;
    h.params.lli_corroborate = true;

    for (int e = 0; e < 3; ++e) {
        const double wob = 0.002 * static_cast<double>(e % 2);
        std::vector<PppObservation> chatty, quiet;
        // Station 0: six satellites, LLI on ALL of them, observables flat.
        for (int s = 1; s <= 6; ++s)
            chatty.push_back(makeDualObs("G0" + std::to_string(s), 100.0 + s,
                                         60.0 + s, /*lli=*/true, wob));
        // Station 1: eight satellites, LLI on exactly one, observables flat.
        for (int s = 1; s <= 8; ++s)
            quiet.push_back(makeDualObs("G1" + std::to_string(s), 200.0 + s,
                                        120.0 + s, /*lli=*/s == 3, wob));
        h.run({ chatty, quiet });
    }

    // --- chatty station: demoted, and demoted from its own evidence.
    EXPECT_DOUBLE_EQ(h.epoch.lli_frac[0], 1.0);
    EXPECT_NEAR(h.hist[0].lli_frac_ewma, 1.0, 1e-12);
    EXPECT_EQ(h.epoch.lli_credible[0], 0);
    EXPECT_FALSE(h.hist[0].lli_credible);
    EXPECT_EQ(h.hist[0].lli_demotions, 1);      // one transition, then latched
    EXPECT_EQ(h.hist[0].lli_promotions, 0);

    // ...and the observables are flat, so nothing corroborates those
    // assertions: this is exactly the (demoted, unstepped) pair the drain
    // reads to suppress an LLI-only refloat.
    for (int s = 1; s <= 6; ++s) {
        const auto& v = h.verdict(0, "G0" + std::to_string(s));
        EXPECT_TRUE(v.measured);
        EXPECT_FALSE(v.stepped) << "G0" << s;
    }
    EXPECT_TRUE(h.epoch.stepped.empty());

    // --- quiet station: credibility intact, so LLI stays decisive there.
    EXPECT_NEAR(h.epoch.lli_frac[1], 0.125, 1e-12);
    EXPECT_EQ(h.epoch.lli_credible[1], 1);
    EXPECT_TRUE(h.hist[1].lli_credible);
    EXPECT_EQ(h.hist[1].lli_demotions, 0);

    // Counters are per (station, epoch) and must not be cross-contaminated.
    EXPECT_EQ(h.counters.lli_demoted_epochs, 3);   // station 0, three epochs
    EXPECT_EQ(h.counters.lli_credible_epochs, 3);  // station 1, three epochs
}

// M6's third case: LLI accompanied by a REAL wide-lane step.  The step is what
// makes the assertion actionable regardless of any station-level credibility
// state (M3 — the OWMG00NZL re-lock class must never be vetoed).
TEST(SlipQcQualify, LliWithWideLaneStepIsCorroborated) {
    RawHarness h;
    h.params.lli_corroborate = true;

    const auto build = [](double n1_bump, bool lli_on_g01) {
        std::vector<PppObservation> sta;
        sta.push_back(makeDualObs("G01", 100.0 + n1_bump, 60.0, lli_on_g01, 0.001));
        for (int s = 2; s <= 5; ++s)
            sta.push_back(makeDualObs("G0" + std::to_string(s), 100.0 + s,
                                      60.0 + s, false, 0.001));
        return sta;
    };

    h.run({ build(0.0, false) });                    // epoch 0: establish levels
    h.run({ build(2.0, true) });                     // epoch 1: +2 WL cycles + LLI

    const auto& v = h.verdict(0, "G01");
    EXPECT_TRUE(v.measured);
    EXPECT_TRUE(v.mw_step);
    EXPECT_TRUE(v.stepped);
    // The step is the designed 2 wide-lane cycles, and it clears the gate by
    // ~4x — the gate itself is the 0.5-cycle physical floor because the
    // station's own empirical scale is far tighter.
    EXPECT_NEAR(v.d_mw, 2.0 * kLambdaWl, 1e-4);
    EXPECT_NEAR(v.gate_mw, h.params.mw_floor_cyc * kLambdaWl, 1e-6);
    EXPECT_GT(std::abs(v.d_mw), v.gate_mw);
    EXPECT_EQ(h.epoch.stepped.count({ 0, "G01" }), 1u);

    // The quiet satellites at the same station stay clean — a real step must
    // not drag the empirical scale up and mask its neighbours.
    for (int s = 2; s <= 5; ++s) {
        const auto& q = h.verdict(0, "G0" + std::to_string(s));
        EXPECT_TRUE(q.measured);
        EXPECT_FALSE(q.stepped) << "G0" << s;
    }
    EXPECT_EQ(h.epoch.stepped.size(), 1u);
}

// A step is only ever formed against the IMMEDIATELY-adjacent processed epoch:
// on the first sighting there is no predecessor, so `measured` is false and
// `stepped` is false.  Absence of evidence is never evidence of a slip.
TEST(SlipQcQualify, FirstSightingIsUnmeasurableNotClean) {
    RawHarness h;
    h.params.lli_corroborate = true;
    h.run({ { makeDualObs("G01", 100.0, 60.0, /*lli=*/true) } });

    const auto& v = h.verdict(0, "G01");
    EXPECT_FALSE(v.measured);
    EXPECT_FALSE(v.stepped);
    EXPECT_TRUE(h.epoch.stepped.empty());
}

// ===========================================================================
// networkQualifyRawObservables — Card G (G1a)
// ===========================================================================

// The instrument the spec asks for BEFORE any mechanism: an arc that
// disappears for an epoch and comes back with a shifted raw level is the
// poison signature (anything that BRIDGED the break is now carrying a stale
// value); an arc that comes back clean is the benign case.  Both are counted,
// separately.
TEST(SlipQcQualify, GapResumptionSeparatesShiftedFromCleanReturns) {
    RawHarness h;
    h.params.gap_instrument = true;

    // G01 gaps and returns SHIFTED by +2 wide-lane cycles; G02 gaps and
    // returns clean; G03/G04 track continuously and supply the station's
    // empirical scale.
    const auto epoch0 = std::vector<PppObservation>{
        makeDualObs("G01", 100.0, 60.0, false, 0.001),
        makeDualObs("G02", 200.0, 150.0, false, 0.001),
        makeDualObs("G03", 300.0, 210.0, false, 0.001),
        makeDualObs("G04", 400.0, 260.0, false, 0.001) };
    const auto epoch1 = std::vector<PppObservation>{     // G01, G02 absent
        makeDualObs("G03", 300.0, 210.0, false, 0.002),
        makeDualObs("G04", 400.0, 260.0, false, 0.002) };
    const auto epoch2 = std::vector<PppObservation>{
        // G01 returns +2 wide-lane cycles high on an otherwise untouched code
        // level, so its level shift across the break is exactly 2·λ_WL.
        makeDualObs("G01", 102.0, 60.0, false, 0.001),
        // G02 returns at the same ambiguity but with the epoch's code wobble:
        // a few millimetres of MW change, i.e. a CLEAN return.
        makeDualObs("G02", 200.0, 150.0, false, 0.003),
        makeDualObs("G03", 300.0, 210.0, false, 0.003),
        makeDualObs("G04", 400.0, 260.0, false, 0.003) };

    h.run({ epoch0 });
    h.run({ epoch1 });
    h.run({ epoch2 });

    ASSERT_EQ(h.epoch.resumptions.size(), 2u);
    const GapResumption* g01 = nullptr;
    const GapResumption* g02 = nullptr;
    for (const auto& g : h.epoch.resumptions) {
        if (g.satellite == "G01") g01 = &g;
        if (g.satellite == "G02") g02 = &g;
    }
    ASSERT_NE(g01, nullptr);
    ASSERT_NE(g02, nullptr);

    EXPECT_EQ(g01->station, 0);
    EXPECT_EQ(g01->sig1, "L1C");
    EXPECT_EQ(g01->sig2, "L2W");
    EXPECT_GE(g01->gap_epochs, 1);
    EXPECT_EQ(g01->gap_epochs, 1);
    EXPECT_TRUE(g01->measurable);
    EXPECT_TRUE(g01->shifted);
    EXPECT_NEAR(g01->d_mw, 2.0 * kLambdaWl, 1e-4);

    EXPECT_EQ(g02->gap_epochs, 1);
    EXPECT_TRUE(g02->measurable);
    EXPECT_FALSE(g02->shifted);

    // Run-end telemetry (invariant 4): both resumptions counted, only the
    // level-shifted one flagged.
    EXPECT_EQ(h.counters.gap_resumptions, 2);
    EXPECT_EQ(h.counters.gap_shifted, 1);
    EXPECT_EQ(h.counters.gap_unmeasurable, 0);

    // A resumption is NOT a step: the predecessor is not adjacent, so the
    // shifted arc's own verdict stays unmeasurable and cannot poison the
    // per-station noise scale.
    EXPECT_FALSE(h.verdict(0, "G01").measured);
    EXPECT_FALSE(h.verdict(0, "G01").stepped);
}

// The instrument is default-OFF: with gap_instrument false the same stream
// produces no resumptions and no counters (spec G2 / rule 1).
TEST(SlipQcQualify, GapInstrumentIsInertWhenDisarmed) {
    RawHarness h;                       // params.gap_instrument == false
    ASSERT_FALSE(h.params.gap_instrument);

    h.run({ { makeDualObs("G01", 100.0, 60.0), makeDualObs("G03", 300.0, 210.0) } });
    h.run({ { makeDualObs("G03", 300.0, 210.0) } });
    h.run({ { makeDualObs("G01", 102.0, 60.0), makeDualObs("G03", 300.0, 210.0) } });

    EXPECT_TRUE(h.epoch.resumptions.empty());
    EXPECT_EQ(h.counters.gap_resumptions, 0);
    EXPECT_EQ(h.counters.gap_shifted, 0);
}

} // namespace
} // namespace GnssProcessor
