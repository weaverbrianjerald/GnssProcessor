/**
 * @file test_band_scoped_response.cpp
 * @brief §Z BAND-SCOPED responses — the two keep-one rules and their config gate.
 *
 * (docs/SPEC_event_harness.md §Z.)  Study 1 measured that freeing EVERY band of a
 * line of sight is what buys the persistent ~0.45-cycle level floor an erase or a
 * whole-satellite refloat leaves behind: with no band pinned, (ρ, I, N₁..N₃)
 * re-draw together from one epoch of code.  Restrict either path to 2 of 3 bands
 * and the floor drops 9–29×.  Both production paths now have a knob that keeps
 * the least-suspect band pinned, and both decision RULES are pure functions —
 * which is what this file exercises:
 *
 *  * `networkRaimEraseBandScope` (T4 attribution erase, `network.raim_erase_band_scope`)
 *    — 3-band / 2-band / 1-band cells, the kept band is the smallest |r|, the
 *    all-over-gate identity, and the no-band-over-gate edge (a median-polish
 *    INTERACTION flag whose per-signal residuals all sit under k·σ̂₀ must still
 *    produce a response, never a silent skip).
 *  * `networkSlipBandScope` (detector refloats, `network.cycle_slip.refloat_band_scope`)
 *    — an IF-TD pair on a 3-band arc is a NO-OP (the third band already anchors the
 *    LOS), the same pair on a 2-band arc is the all-band FALLBACK, the MW/GF
 *    per-band decomposition picks the quieter band, and a single-band arc is never
 *    spared.
 *  * The loader — both knobs default to the pre-knob behaviour, a typo falls back
 *    loudly to it, and OFF leaves the enums at their byte-identical defaults.
 *
 * Structural only (the suite does not validate estimation accuracy); the floors
 * themselves are graded by the harness legs the SPEC records.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkRaim.hpp"
#include "logging/LogManager.hpp"
#include "slip/NetworkRawObservables.hpp"
#include "slip/NetworkSlipQc.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// GPS L1/L2 constants, exactly as `networkComputeRawObservables` forms them.
constexpr double kLam1  = 0.19029367279836487;   // L1 wavelength (m)
constexpr double kLam2  = 0.24421021342456825;   // L2 wavelength (m)
// Derived, not transcribed: the scope recovers λ2 from (λ1, λ_WL), so the test's
// two wavelengths must be EXACTLY consistent or the round trip carries a rounding
// residual that has nothing to do with the rule under test.
constexpr double kLamWl = kLam1 * kLam2 / (kLam2 - kLam1);   // 1/(1/λ1 − 1/λ2) (m)

/// A dual-band raw row for (sig1, sig2) with the wavelengths above.
RawSlipObs dualRow(const std::string& sat, const std::string& s1,
                   const std::string& s2) {
    RawSlipObs r;
    r.satellite   = sat;
    r.sig1        = s1;
    r.sig2        = s2;
    r.dual        = true;
    r.lam1_m      = kLam1;
    r.lambda_wl_m = kLamWl;
    return r;
}

/// A measured verdict whose (ΔMW, ΔGF) encode the given per-band cycle slips —
/// the exact inverse of the decomposition the scope applies, so a test can state
/// "band 1 slipped by dn1 cycles" and read back that the scope agreed.
RawSlipVerdict verdictFor(double dn1_cyc, double dn2_cyc) {
    RawSlipVerdict v;
    v.measured = true;
    v.stepped  = true;
    v.d_mw     = (dn1_cyc - dn2_cyc) * kLamWl;      // λ_WL · Δ(N1 − N2)
    v.d_gf     = kLam1 * dn1_cyc - kLam2 * dn2_cyc; // λ1ΔN1 − λ2ΔN2
    return v;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("bscope_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// Minimum loadable PPP-RTK network config; @p extra is spliced into the network
/// section verbatim (must end with a comma when non-empty).
std::string networkConfig(const std::string& extra) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"STA1\",\n")
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"STA1\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"STA2\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log,
              const std::string& tag) {
    const fs::path p = writeTempConfig(tag, networkConfig(extra));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// T4 attribution erase — network.raim_erase_band_scope
// ---------------------------------------------------------------------------

TEST(BandScopedEraseRule, ThreeBandKeepsTheSmallestResidual) {
    // L2W is the quietest band of the cell, so it is the one that stays pinned as
    // the LOS's level anchor; the other two are over the gate and are freed.
    const std::vector<std::pair<std::string, double>> cell = {
        {"L1C", 8.0}, {"L2W", 1.2}, {"L5Q", 6.0}};
    const auto d = networkRaimEraseBandScope(cell, /*bar=*/4.0);
    ASSERT_TRUE(d.act);
    EXPECT_FALSE(d.single_band);
    EXPECT_EQ(d.keep, "L2W");
    EXPECT_FALSE(d.kept_over_gate);
    EXPECT_EQ(d.erase, (std::set<std::string>{"L1C", "L5Q"}));
}

TEST(BandScopedEraseRule, TwoBandStillSparesOne) {
    const std::vector<std::pair<std::string, double>> cell = {
        {"L1C", 9.0}, {"L2W", 5.0}};
    const auto d = networkRaimEraseBandScope(cell, 4.0);
    ASSERT_TRUE(d.act);
    EXPECT_EQ(d.keep, "L2W");
    EXPECT_EQ(d.erase, (std::set<std::string>{"L1C"}));
    // Both bands are over the gate, so the pin is the LEAST-BAD one — the identity
    // that keeps the "we knowingly kept suspect level" population visible.
    EXPECT_TRUE(d.kept_over_gate);
}

TEST(BandScopedEraseRule, SingleBandCellIsSkippedNotErased) {
    // One band: there is nothing to spare, and freeing it is exactly the all-band
    // case the scope exists to prevent.  `erase` empty ⇒ the caller skips.
    const auto d = networkRaimEraseBandScope({{"L1C", 99.0}}, 4.0);
    ASSERT_TRUE(d.act);
    EXPECT_TRUE(d.single_band);
    EXPECT_EQ(d.keep, "L1C");
    EXPECT_TRUE(d.erase.empty());
}

TEST(BandScopedEraseRule, NoBandOverGateStillErasesAllButThePin) {
    // The median-polish INTERACTION flagged this cell while every per-signal
    // residual sits under k·σ̂₀.  Absence of per-band evidence must NOT suppress
    // the response (standing invariant #1) — erase everything but the pin.
    const std::vector<std::pair<std::string, double>> cell = {
        {"L1C", 1.0}, {"L2W", 2.0}, {"L5Q", 3.0}};
    const auto d = networkRaimEraseBandScope(cell, 4.0);
    ASSERT_TRUE(d.act);
    EXPECT_EQ(d.keep, "L1C");
    EXPECT_FALSE(d.kept_over_gate);
    EXPECT_EQ(d.erase, (std::set<std::string>{"L2W", "L5Q"}));
}

TEST(BandScopedEraseRule, EmptyCellDoesNotAct) {
    // No attribution rows for this (station, satellite) this epoch ⇒ the caller
    // falls through to the unscoped whole-arc erase rather than refusing it.
    const auto d = networkRaimEraseBandScope({}, 4.0);
    EXPECT_FALSE(d.act);
    EXPECT_TRUE(d.keep.empty());
}

TEST(BandScopedEraseRule, TieBreaksOnTheLexicallyLowestSignal) {
    const std::vector<std::pair<std::string, double>> cell = {
        {"L5Q", 2.0}, {"L1C", 2.0}, {"L2W", 9.0}};
    const auto d = networkRaimEraseBandScope(cell, 4.0);
    EXPECT_EQ(d.keep, "L1C");   // deterministic across pool orderings
}

// ---------------------------------------------------------------------------
// Detector refloats — network.cycle_slip.refloat_band_scope
// ---------------------------------------------------------------------------

TEST(BandScopedRefloatRule, IfTdPairOnThreeBandArcIsANoOp) {
    // L5Q is not implicated, so the arc already keeps a level anchor: the scope
    // must not perturb the pre-knob instruction stream.
    const auto row = dualRow("G10", "L1C", "L2W");
    const auto vd  = verdictFor(/*dn1=*/10.0, /*dn2=*/8.0);
    const auto d = networkSlipBandScope({"L1C", "L2W", "L5Q"}, {"L1C", "L2W"}, {},
                                        &row, &vd);
    EXPECT_FALSE(d.scoped);
    EXPECT_TRUE(d.keep.empty());
}

TEST(BandScopedRefloatRule, IfTdPairOnTwoBandArcIsTheAllBandFallback) {
    // The pair IS the arc.  ΔN1 = 10, ΔN2 = 2 ⇒ L2W is the quieter band and is the
    // one that stays pinned.  This is the documented case where a truly-slipped
    // band can stay pinned; the counter names it.
    const auto row = dualRow("G10", "L1C", "L2W");
    const auto vd  = verdictFor(10.0, 2.0);
    const auto d = networkSlipBandScope({"L1C", "L2W"}, {"L1C", "L2W"}, {},
                                        &row, &vd);
    ASSERT_TRUE(d.scoped);
    EXPECT_TRUE(d.measured);
    EXPECT_EQ(d.keep, "L2W");
    EXPECT_NEAR(d.keep_dn_cyc, 2.0, 1e-5);
}

TEST(BandScopedRefloatRule, DecompositionPicksTheQuieterBandEitherWay) {
    // Same arc, slip on the OTHER band: the rule must follow the evidence, not the
    // signal ordering.
    const auto row = dualRow("G10", "L1C", "L2W");
    const auto vd  = verdictFor(1.0, 12.0);
    const auto d = networkSlipBandScope({"L1C", "L2W"}, {"L1C", "L2W"}, {},
                                        &row, &vd);
    ASSERT_TRUE(d.scoped);
    EXPECT_EQ(d.keep, "L1C");
    EXPECT_NEAR(d.keep_dn_cyc, 1.0, 1e-5);
}

TEST(BandScopedRefloatRule, WholeSatelliteLliKeepsAMeasuredBandOverAnUnmeasuredOne) {
    // LLI on all three bands (a receiver loss of lock).  The raw row only carries
    // the primary pair, so L5Q has NO evidence — and an unmeasured band must never
    // out-rank a measured one, however quiet it looks.
    const auto row = dualRow("G10", "L1C", "L2W");
    const auto vd  = verdictFor(9.0, 0.05);
    const auto d = networkSlipBandScope({"L1C", "L2W", "L5Q"}, {"L1C", "L2W", "L5Q"}, {},
                                        &row, &vd);
    ASSERT_TRUE(d.scoped);
    EXPECT_TRUE(d.measured);
    EXPECT_EQ(d.keep, "L2W");
}

TEST(BandScopedRefloatRule, NoRawEvidenceFallsBackToTheLowestBand) {
    // A gap, a changed pair, or no raw row at all: deterministic lexical fallback
    // (also the shortest wavelength — the smallest metric error per kept cycle).
    const auto d = networkSlipBandScope({"L1C", "L2W", "L5Q"}, {"L1C", "L2W", "L5Q"}, {},
                                        nullptr, nullptr);
    ASSERT_TRUE(d.scoped);
    EXPECT_FALSE(d.measured);
    EXPECT_EQ(d.keep, "L1C");
}

TEST(BandScopedRefloatRule, PairChangedRowIsRefusedAsEvidence) {
    // The struct's own contract: a step formed on a DIFFERENT lane must not be
    // converted into cycles of this one.
    const auto row = dualRow("G10", "L1C", "L2W");
    auto vd = verdictFor(9.0, 0.05);
    vd.pair_changed = true;
    const auto d = networkSlipBandScope({"L1C", "L2W"}, {"L1C", "L2W"}, {},
                                        &row, &vd);
    ASSERT_TRUE(d.scoped);
    EXPECT_FALSE(d.measured);
    EXPECT_EQ(d.keep, "L1C");   // lexical fallback, not the (refused) evidence
}

TEST(BandScopedRefloatRule, UnmeasuredVerdictIsRefusedAsEvidence) {
    const auto row = dualRow("G10", "L1C", "L2W");
    RawSlipVerdict vd;            // measured = false (first sighting / gap)
    const auto d = networkSlipBandScope({"L1C", "L2W"}, {"L1C", "L2W"}, {},
                                        &row, &vd);
    ASSERT_TRUE(d.scoped);
    EXPECT_FALSE(d.measured);
    EXPECT_EQ(d.keep, "L1C");
}

TEST(BandScopedRefloatRule, AlreadyFreedBandsCountTowardCoveringTheArc) {
    // THE DEFECT THE HARNESS FOUND.  An LLI on all three bands makes the MW/LLI
    // drain keep L1C; the IF-TD driver then implicates (L1C, L2W), which does NOT
    // cover a 3-band arc — so per-drain the scope is a no-op there and the arc
    // ends the epoch with EVERY band freed, byte-identical to an unscoped run
    // (measured: evleg_z_slip3_on == evleg_z_slip3_off).  With the epoch's freed
    // set in hand the second drain sees the arc covered and pins the survivor.
    const auto d = networkSlipBandScope({"L1C", "L2W", "L5Q"},   // live
                                        {"L1C", "L2W"},          // this drain
                                        {"L2W", "L5Q"},          // already freed
                                        nullptr, nullptr);
    ASSERT_TRUE(d.scoped);
    EXPECT_EQ(d.keep, "L1C");   // the only band still pinned
}

TEST(BandScopedRefloatRule, AlreadyFreedBandIsNeverChosenAsTheSurvivor) {
    // L2W is the quiet band by raw evidence, but an earlier drain already freed
    // it — an un-freeing does not exist, so the pin must fall on a survivor.
    const auto row = dualRow("G10", "L1C", "L2W");
    const auto vd  = verdictFor(/*dn1=*/9.0, /*dn2=*/0.05);
    const auto d = networkSlipBandScope({"L1C", "L2W"}, {"L1C"}, {"L2W"},
                                        &row, &vd);
    ASSERT_TRUE(d.scoped);
    EXPECT_EQ(d.keep, "L1C");
    EXPECT_TRUE(d.measured);
    EXPECT_NEAR(d.keep_dn_cyc, 9.0, 1e-5);
}

TEST(BandScopedRefloatRule, EverythingAlreadyFreedIsTooLateNotScoped) {
    // The arc lost its anchor before this drain ran.  Nothing to do — but it is
    // COUNTED, because a nonzero tally means some freeing site upstream is not
    // routed through the scope (invariant #4).
    const auto d = networkSlipBandScope({"L1C", "L2W"}, {"L1C", "L2W"},
                                        {"L1C", "L2W"}, nullptr, nullptr);
    EXPECT_FALSE(d.scoped);
    EXPECT_TRUE(d.too_late);
    EXPECT_TRUE(d.keep.empty());
}

TEST(BandScopedRefloatRule, PartialPriorFreeStillLeavesAnAnchorAndIsANoOp) {
    // L5Q freed earlier, this drain implicates only L1C: L2W is untouched by
    // both, so the arc keeps an anchor with no help and the scope must not act.
    const auto d = networkSlipBandScope({"L1C", "L2W", "L5Q"}, {"L1C"}, {"L5Q"},
                                        nullptr, nullptr);
    EXPECT_FALSE(d.scoped);
    EXPECT_FALSE(d.too_late);
}

TEST(BandScopedRefloatRule, SingleBandArcIsNeverSpared) {
    // One band: no LOS anchor exists to preserve, and refusing a detector's
    // positive slip evidence would leave the step standing with no observable able
    // to catch it again.  The caller frees it exactly as an unscoped run does.
    const auto d = networkSlipBandScope({"L1C"}, {"L1C"}, {},
                                        nullptr, nullptr);
    EXPECT_FALSE(d.scoped);
    EXPECT_TRUE(d.single_band);
    EXPECT_TRUE(d.keep.empty());
}

TEST(BandScopedRefloatRule, EmptyArcIsANoOp) {
    const auto d = networkSlipBandScope({}, {"L1C"}, {},
                                        nullptr, nullptr);
    EXPECT_FALSE(d.scoped);
    EXPECT_FALSE(d.single_band);
}

// ---------------------------------------------------------------------------
// The config gate — default OFF is the pre-knob behaviour
// ---------------------------------------------------------------------------

TEST(BandScopedConfig, DefaultsAreTheUnscopedPaths) {
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, lm, "def"));
    EXPECT_EQ(cfg.getNetworkConfig().raim_erase_band_scope,
              NetworkRaimEraseBandScope::All);
    EXPECT_EQ(cfg.getNetworkConfig().cycle_slip_refloat_band_scope,
              NetworkRefloatBandScope::All);
}

TEST(BandScopedConfig, BothKnobsArm) {
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"raim_erase_band_scope\": \"min_keep_one\",\n"
                         "    \"cycle_slip\": { \"refloat_band_scope\": \"implicated\" },\n",
                         cfg, lm, "arm"));
    EXPECT_EQ(cfg.getNetworkConfig().raim_erase_band_scope,
              NetworkRaimEraseBandScope::MinKeepOne);
    EXPECT_EQ(cfg.getNetworkConfig().cycle_slip_refloat_band_scope,
              NetworkRefloatBandScope::Implicated);
}

TEST(BandScopedConfig, TypoFallsBackToTheUnscopedPathNotToSilence) {
    // A misspelled scope must not silently arm (or disarm) a carry-shaping
    // response.  The loader logs an error and takes the pre-knob default.
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"raim_erase_band_scope\": \"keep_one\",\n"
                         "    \"cycle_slip\": { \"refloat_band_scope\": \"pair\" },\n",
                         cfg, lm, "typo"));
    EXPECT_EQ(cfg.getNetworkConfig().raim_erase_band_scope,
              NetworkRaimEraseBandScope::All);
    EXPECT_EQ(cfg.getNetworkConfig().cycle_slip_refloat_band_scope,
              NetworkRefloatBandScope::All);
}
