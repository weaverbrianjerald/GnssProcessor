/**
 * @file test_turbo_edit_resolver.cpp
 * @brief Unit tests for TurboEditResolver, the MW+GF joint cycle-slip repair
 *        resolver, exercised end to end through resolve() with synthetic
 *        carrier-phase epochs.
 *
 * Focus: the MW-only code-domain veto (an MW flag whose geometry-free phase is
 * affirmatively stable is classified as a code artifact and the phase reset is
 * skipped), the gap-spanning GF bridge, low-rate operation, and regression
 * coverage for the joint-solve behaviors the veto must not disturb (real-pair
 * repair, GF-only equal-slip repair, reset fallbacks, trust gates).
 *
 * All inputs are deterministic: GF noise comes from a fixed table (no RNG),
 * epochs advance at an exact cadence, and wavelengths are computed from the GPS
 * broadcast frequencies, so every threshold crossing in these tests is a designed
 * margin.
 */

#include <gtest/gtest.h>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/TurboEditResolver.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr double kC  = 299792458.0;
const double kL1 = kC / 1575.42e6;   // GPS L1 wavelength ≈ 0.19029 m
const double kL2 = kC / 1227.60e6;   // GPS L2 wavelength ≈ 0.24421 m
const double kL5 = kC / 1176.45e6;   // GPS L5 wavelength ≈ 0.25483 m

// Deterministic GF "phase noise" (metres), sub-mm scale, derived from the
// digits of π so the TD2 sign pattern is well mixed.  A strictly
// sign-alternating table makes the TD2 MAD collapse whenever the window parity
// flips (the median lands on one sign cluster), which is a test artifact, not
// receiver behavior.  TD2 of this sequence stays within about ±1 mm, far below
// the 4 cm GF detection floor (no spurious flags), yet wide enough that the
// arc's TD2 MAD-σ is non-zero, which arms the code-event affirmative-zero gate
// (3.5σ ≈ 2 mm) with about 5× margin over the event-epoch TD2 (0.3–0.4 mm).
constexpr double kGfNoiseM[16] = {
    -0.00035, -0.00005, -0.00035,  0.00005,  0.00045, -0.00025,  0.00015,  0.00005,
    -0.00015,  0.00005,  0.00035,  0.00045,  0.00025,  0.00045, -0.00015, -0.00025 };

/// Minimal phase-only observation: raw carrier phases in cycles keyed by
/// RINEX-3 code, plus the fields resolve() consults (sat, system, time,
/// elevation, LLI).
PppObservation makePhaseObs(const std::string& sat, double tow,
                            const std::map<std::string, double>& phase_cycles,
                            double elevation_deg = 45.0) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system = System::GPS;
    obs.time = GnssTime{ 2400, tow };
    obs.elevation = elevation_deg;
    obs.azimuth = 0.0;
    obs.raw_observed = phase_cycles;
    return obs;
}

/// MW pair-slip result as MwSlipDetector emits it for an uncombined pair.
MwSlipResult makeMwFlag(const std::string& sat, int dn_wl, bool wl_known = true,
                        const std::string& sig1 = "L1C",
                        const std::string& sig2 = "L2W",
                        double wl_float = std::numeric_limits<double>::quiet_NaN()) {
    MwSlipResult sr;
    sr.amb_key        = sat + "_" + sig1;
    sr.satellite      = sat;
    sr.sig1           = sig1;
    sr.sig2           = sig2;
    sr.slip_wl_cycles = dn_wl;
    sr.slip_wl_known  = wl_known;
    sr.lambda_wl      = 0.8619;
    sr.slip_wl_float  = wl_float;
    return sr;
}

/// Test harness: default GnssConfig (veto on, k=6 GF gate, 10° mask) and a
/// bare LogManager, member-ordered so the refs outlive the resolver.
struct TeHarness {
    LogManager  log_manager;
    GnssConfig  config;
    TurboEditResolver te{ config, log_manager };
};

/// Dual-frequency single-sat epoch with a flat (noise-only) GF.
/// L2W is held constant; the GF noise is injected through L1C so that
/// GF(t) = kGfNoiseM[k mod 16] + const.
std::vector<PppObservation> flatEpoch(const std::string& sat, double tow, int k,
                                      double extra_l1_cycles = 0.0,
                                      double extra_l2_cycles = 0.0) {
    const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1 + extra_l1_cycles;
    const double l2 = 200.0 + extra_l2_cycles;
    return { makePhaseObs(sat, tow, { { "L1C", l1 }, { "L2W", l2 } }) };
}

/// Roll a single-sat dual-frequency arc through n quiet epochs starting at
/// tow0; returns the tow of the next (event) epoch.  After 12 epochs the
/// arc's TD2 MAD window is mature (≥ 5 samples) and the code-event
/// affirmative-zero gate is armed.
double warmupFlat(TurboEditResolver& te, const std::string& sat,
                  int n_epochs, double interval_s, double tow0 = 100000.0) {
    for (int k = 0; k < n_epochs; ++k) {
        te.resolve({}, {}, flatEpoch(sat, tow0 + k * interval_s, k), interval_s);
    }
    return tow0 + n_epochs * interval_s;
}

/// Dual-frequency single-sat epoch whose GF noise is the π-digit table scaled
/// by noise_scale: the deterministic stand-in for the coarse-rate regime,
/// where the uncancelled iono residual inflates the arc's TD2 noise from
/// sub-mm to cm scale.  At scale 25 the TD2 MAD-σ is
/// ≈ 25 × 0.67 mm ≈ 1.7 cm: the code-event zero gate (3.5σ ≈ 5.8 cm)
/// overshoots its discrimination cap (0.5×|λ1−λ2| ≈ 2.7 cm) and the σ
/// overshoots the split gate (0.2×|λ1−λ2| ≈ 1.1 cm), so both the veto and
/// the per-signal split must stand down.
std::vector<PppObservation> noisyEpoch(const std::string& sat, double tow, int k,
                                       double noise_scale,
                                       double extra_l1_cycles = 0.0,
                                       double extra_l2_cycles = 0.0) {
    const double l1 =
        100.0 + noise_scale * kGfNoiseM[k % 16] / kL1 + extra_l1_cycles;
    const double l2 = 200.0 + extra_l2_cycles;
    return { makePhaseObs(sat, tow, { { "L1C", l1 }, { "L2W", l2 } }) };
}

/// warmupFlat with scaled GF noise (see noisyEpoch).
double warmupNoisy(TurboEditResolver& te, const std::string& sat,
                   int n_epochs, double interval_s, double noise_scale,
                   double tow0 = 100000.0) {
    for (int k = 0; k < n_epochs; ++k) {
        te.resolve({}, {},
                   noisyEpoch(sat, tow0 + k * interval_s, k, noise_scale),
                   interval_s);
    }
    return tow0 + n_epochs * interval_s;
}

// ---------------------------------------------------------------------------
// MW-only code-domain veto
// ---------------------------------------------------------------------------

// The main case: MW flags ΔN_WL = +2 (whose only GF-consistent split is the
// implausible null-line pair (9,7)) while the GF is flat.  Without the veto the
// joint solve produces a confident wrong (9,7) repair; with the veto the flag
// must be classified as a code-domain event: no repair, no reset, both signals
// adjudicated clean.
TEST(TurboEditCodeEventVeto, MwOnlyFlatGfEvenKClassifiedAsCodeEvent) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +2) }, {},
                                flatEpoch("G01", tow, 12), 1.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_EQ(r.code_events[0].satellite, "G01");
    EXPECT_EQ(r.code_events[0].sig1, "L1C");
    EXPECT_EQ(r.code_events[0].sig2, "L2W");
    EXPECT_EQ(r.code_events[0].dN_wl, 2);
    EXPECT_TRUE(r.code_events[0].wl_known);
    EXPECT_GT(r.code_events[0].zero_gate_m, 0.0);
    EXPECT_LE(std::abs(r.code_events[0].td2_m), r.code_events[0].zero_gate_m);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    ASSERT_EQ(r.clean_signals.count("G01"), 1u);
    EXPECT_EQ(r.clean_signals.at("G01").count("L1C"), 1u);
    EXPECT_EQ(r.clean_signals.at("G01").count("L2W"), 1u);
}

// Odd-k variant: ΔN_WL = +1 with flat GF has no consistent integer pair at all
// (frac ≈ 0.47; these are the resets the veto suppresses).  It must be a code
// event, and a persistent code excursion (MW re-flags the next epoch after its
// arc re-baselines) must be vetoed again: the GF arc keeps rolling through a
// veto, so the evidence stays current.
TEST(TurboEditCodeEventVeto, OddKVetoedAndPersistentExcursionVetoedAgain) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r1 = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                 flatEpoch("G01", tow, 12), 1.0);
    ASSERT_EQ(r1.code_events.size(), 1u);
    EXPECT_TRUE(r1.reset_sats.empty());

    const auto r2 = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                 flatEpoch("G01", tow + 1.0, 13), 1.0);
    ASSERT_EQ(r2.code_events.size(), 1u);
    EXPECT_TRUE(r2.reset_sats.empty());
    EXPECT_TRUE(r2.repairs.empty());
}

// A non-integer MW excursion (slip_wl_known = false) with a still phase is, if
// anything, more code-like: the veto must not require the WL integer.  Without
// the veto this case takes the unconditional !wl_known reset.
TEST(TurboEditCodeEventVeto, NonIntegerMwStepAlsoVetoed) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", 0, /*wl_known=*/false) }, {},
                                flatEpoch("G01", tow, 12), 1.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_FALSE(r.code_events[0].wl_known);
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.repairs.empty());
}

// ---------------------------------------------------------------------------
// Trust gates: every leg the veto's evidence stands on must, when absent,
// restore the pre-veto reset behavior.
// ---------------------------------------------------------------------------

// Immature arc (TD2 exists but the MAD window has < 5 samples): GF cannot
// affirm zero, so the MW flag falls through to the joint solve, whose
// frac-gate rejection (ΔN2 float ≈ 3.53 for k=1) demands the reset.
TEST(TurboEditCodeEventVeto, WarmupArcFallsBackToReset) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 5, 1.0);  // window: 3 TD2s < 5

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 5), 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// Coarse rate, quiet ionosphere: GF trust is gated on evidence, not on rate.
// A 30 s arc whose measured TD2 noise stays sub-mm (quiet iono) carries a zero
// gate far below the discrimination cap, so the veto must engage exactly as at
// 1 s.  This rate-adaptive contract is what makes TurboEdit usable on 30 s RINEX.
TEST(TurboEditCodeEventVeto, CoarseRateQuietArcStillVetoes) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 30.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 12), 30.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.repairs.empty());
}

// Coarse rate, iono-inflated arc: with the TD2 MAD at cm scale the
// affirmative-zero gate (3.5σ) overshoots the discrimination cap
// (0.5×|λ1−λ2|), so "zero" can no longer be told from a small slip and the
// veto stands down; with the σ also above the split gate, the MW mandate takes
// the conservative reset.  The noise evidence, not a fixed rate gate, produces
// this coarse-rate stand-down.
TEST(TurboEditCodeEventVeto, CoarseRateNoisyArcStandsDown) {
    TeHarness h;
    const double scale = 25.0;  // TD2 MAD-σ ≈ 1.7 cm (see noisyEpoch)
    const double tow = warmupNoisy(h.te, "G01", 12, 30.0, scale);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                noisyEpoch("G01", tow, 12, scale), 30.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// Receiver-declared loss of lock on either pair signal corroborates that
// integer continuity broke: the veto must stand down and let the caller's LLI
// mandate govern.
TEST(TurboEditCodeEventVeto, LliOnPairSignalBlocksVeto) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    auto epoch = flatEpoch("G01", tow, 12);
    epoch[0].lli_flags["L1C"] = 1;

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {}, epoch, 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// Disturbed ionosphere (cross-satellite IVI above the 6 cm gate): the GF
// "stability" of a single arc proves nothing when the constellation's TD2
// field is churning, so the flagged sat takes the reset fallback.
TEST(TurboEditCodeEventVeto, DisturbedIonoBlocksVeto) {
    TeHarness h;
    const std::vector<std::string> sats = { "G01", "G02", "G03", "G04" };
    const double interval = 1.0;
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k) {
        std::vector<PppObservation> epoch;
        for (const auto& s : sats)
            epoch.push_back(flatEpoch(s, tow0 + k * interval, k)[0]);
        h.te.resolve({}, {}, epoch, interval);
    }

    // Event epoch: G02..G04 GF jumps +8 cm (iono churn), G01 stays flat
    // with an MW flag.  Constellation IVI = median{~0, .08, .08, .08} +
    // MADσ ≥ 0.08 m > 0.06 m gate.
    std::vector<PppObservation> epoch;
    epoch.push_back(flatEpoch("G01", tow0 + 12 * interval, 12)[0]);
    for (const auto& s : { "G02", "G03", "G04" })
        epoch.push_back(flatEpoch(s, tow0 + 12 * interval, 12, 0.08 / kL1)[0]);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {}, epoch, interval);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// Off switch: estimation.cycle_slip.turbo_edit_mw_only_veto = false must
// restore the behavior without the veto exactly (loaded through the real JSON
// path, so a key typo in the loader fails here, not in the field).
TEST(TurboEditCodeEventVeto, ConfigKillSwitchRestoresReset) {
    namespace fs = std::filesystem;
    const fs::path cfg_path =
        fs::temp_directory_path() / "gnss_te_veto_off_test.json";
    {
        std::ofstream f(cfg_path);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_mw_only_veto": false } } })";
    }

    LogManager log_manager;
    GnssConfig config;
    ASSERT_TRUE(config.loadFromJson(cfg_path, log_manager));
    ASSERT_FALSE(config.getCycleSlipConfig().turbo_edit_mw_only_veto);

    TurboEditResolver te{ config, log_manager };
    double tow = 100000.0;
    for (int k = 0; k < 12; ++k, tow += 1.0)
        te.resolve({}, {}, flatEpoch("G01", tow, k), 1.0);

    const auto r = te.resolve({ makeMwFlag("G01", +1) }, {},
                              flatEpoch("G01", tow, 12), 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);

    std::error_code ec;
    fs::remove(cfg_path, ec);
}

// Loader defaults and the legacy flat-key alias: the veto must be on when
// the key is absent (turbo_edit alone opted the user into the trust-gated
// machinery), and the snake_case alias must reach the same field.
TEST(TurboEditCodeEventVeto, LoaderDefaultOnAndFlatAlias) {
    namespace fs = std::filesystem;
    LogManager log_manager;

    {
        const fs::path p = fs::temp_directory_path() / "gnss_te_veto_default_test.json";
        { std::ofstream f(p); f << R"({ "estimation": { "cycle_slip": { "turbo_edit": true } } })"; }
        GnssConfig config;
        ASSERT_TRUE(config.loadFromJson(p, log_manager));
        EXPECT_TRUE(config.getCycleSlipConfig().turbo_edit_mw_only_veto);
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = fs::temp_directory_path() / "gnss_te_veto_alias_test.json";
        { std::ofstream f(p); f << R"({ "cycle_slip_turbo_edit_mw_only_veto": false })"; }
        GnssConfig config;
        ASSERT_TRUE(config.loadFromJson(p, log_manager));
        EXPECT_FALSE(config.getCycleSlipConfig().turbo_edit_mw_only_veto);
        std::error_code ec; fs::remove(p, ec);
    }
}

// Before initialize(), the no-argument LogManager accessor must degrade to a
// transient console logger, not throw: GnssConfig::loadFromJson's catch
// handler logs through it, so a throw here would escape the handler that
// reports the original failure.
TEST(LogManagerFallback, UninitializedDefaultLoggerDoesNotThrow) {
    LogManager log_manager;
    std::shared_ptr<spdlog::logger> logger;
    EXPECT_NO_THROW(logger = log_manager.getLogger());
    ASSERT_NE(logger, nullptr);
    EXPECT_NO_THROW(logger->info("fallback logger is usable"));
}

// ---------------------------------------------------------------------------
// Atomicity: contradictory evidence on another pair withdraws the veto.
// ---------------------------------------------------------------------------

// Triple-signal sat where the L1C/L2W pair earns a veto (MW flag, flat GF)
// while a half-cycle artifact on L5Q trips both L5 pairs' GF gates with no
// affirmative MW-clean evidence (→ reset mandate).  The satellite-level reset
// must win atomically: the code event is withdrawn and the staged clean
// adjudication suppressed, so the caller cannot skip the reset this
// resolution demands.
//
// Five quiet background satellites keep the constellation IVI calm at the
// event epoch (the pool median stays at noise scale despite G01's two ~13 cm
// TD2s); without them the IVI gate trips, the veto never fires, and the
// withdrawal assertions are vacuous.  Phase 1 (MW flag alone) shows the veto
// is live in this configuration; phase 2 adds the conflicting L5 flags and
// pins the withdrawal.
TEST(TurboEditCodeEventVeto, ConflictingPairResetWithdrawsCodeEvent) {
    TeHarness h;
    const double interval = 1.0;
    const double tow0 = 100000.0;
    const std::vector<std::string> bg = { "G02", "G03", "G04", "G05", "G06" };

    auto buildEpoch = [&](double tow, int k, double l5_extra_cycles) {
        const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
        std::vector<PppObservation> epoch{ makePhaseObs(
            "G01", tow, { { "L1C", l1 },
                          { "L2W", 200.0 },
                          { "L5Q", 300.0 + l5_extra_cycles } }) };
        for (const auto& s : bg)
            epoch.push_back(flatEpoch(s, tow, k)[0]);
        return epoch;
    };

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, buildEpoch(tow0 + k * interval, k, 0.0), interval);

    // Phase 1: MW-only flag, everything flat → the veto must fire here,
    // or phase 2 would pass for the wrong reason.
    const auto r1 = h.te.resolve({ makeMwFlag("G01", +1, true, "L1C", "L2W") }, {},
                                 buildEpoch(tow0 + 12 * interval, 12, 0.0),
                                 interval);
    ASSERT_EQ(r1.code_events.size(), 1u);
    ASSERT_TRUE(r1.reset_sats.empty());

    // Phase 2: L5Q shifts by half a cycle (GF(L1,L5) and GF(L2,L5) jump
    // ~12.7 cm, exceeding their gates, no MW-clean affirmation → reset
    // mandates) while MW again flags only the flat L1C/L2W pair.
    const auto r2 = h.te.resolve({ makeMwFlag("G01", +1, true, "L1C", "L2W") }, {},
                                 buildEpoch(tow0 + 13 * interval, 13, 0.5),
                                 interval);

    EXPECT_EQ(r2.reset_sats.count("G01"), 1u);
    EXPECT_EQ(r2.reset_sats.size(), 1u);   // background sats untouched
    EXPECT_TRUE(r2.code_events.empty());   // withdrawal executed
    EXPECT_TRUE(r2.repairs.empty());
    EXPECT_EQ(r2.clean_signals.count("G01"), 0u);
}

// Unknown sampling interval (interval_s <= 0, e.g. a RINEX file without an
// INTERVAL header) does not affect the evidence gates: the arc's measured TD2
// noise carries the rate information.  With quiet 30 s spacing the veto
// engages; the iono-inflated variant is covered by CoarseRateNoisyArcStandsDown
// (the noise, not the spacing, governs).
TEST(TurboEditCodeEventVeto, UnknownIntervalNoiseGovernsNotSpacing) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 30.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 12), /*interval_s=*/0.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_TRUE(r.reset_sats.empty());
}

// The same at high-rate spacing, pinning that an unknown declared interval
// never blocks the veto on a quiet arc.
TEST(TurboEditCodeEventVeto, UnknownIntervalHighRateSpacingKeepsVeto) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 12), /*interval_s=*/0.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_TRUE(r.reset_sats.empty());
}

// Post-slip cooldown: right after a genuine repair re-seeds the GF chain,
// the first epoch with a fresh TD2 still carries cooldown_remaining == 1,
// and the veto must stand down there (the echo window is not affirmative
// evidence), falling back to the frac-gate reset.
TEST(TurboEditCodeEventVeto, CooldownEpochBlocksVeto) {
    TeHarness h;
    double tow = warmupFlat(h.te, "G01", 12, 1.0);

    // Genuine (1,0) slip → repair → did_slip: chain re-seeds, cooldown = 3.
    const auto r0 = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                 flatEpoch("G01", tow, 12, /*extra_l1=*/1.0), 1.0);
    ASSERT_EQ(r0.repairs.size(), 1u);
    tow += 1.0;

    // Two warm-up epochs at the slipped level (count 0→1→2, cooldown 3→2→1).
    for (int k = 13; k <= 14; ++k, tow += 1.0)
        h.te.resolve({}, {}, flatEpoch("G01", tow, k, 1.0), 1.0);

    // First epoch with a valid post-slip TD2: cooldown_remaining == 1.
    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 15, 1.0), 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// An MW mandate on a pair the resolver could not evaluate (fresh arc, no TD2
// yet) must leave the resolution empty: no reset_sats entry, no code event, no
// clean signals.  The caller honors the unadjudicated mandate; the resolver
// must neither veto nor duplicate it.
TEST(TurboEditCodeEventVeto, UncachedPairMandateLeavesResolverSilent) {
    TeHarness h;

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", 100000.0, 0), 1.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.clean_signals.empty());
}

// The veto must work identically on an L1/L5-family pair: in that band family
// the null line sits at ΔN1 ≈ 3.95k and even a k=1 flag has a GF-quiet (4,3)
// pair at 3.3 mm, so the magnitude prior (not the GF geometry) carries the
// classification.
TEST(TurboEditCodeEventVeto, L1L5FamilyPairVetoed) {
    TeHarness h;
    const double tow0 = 100000.0;
    auto l15Epoch = [&](double tow, int k) {
        const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
        return std::vector<PppObservation>{ makePhaseObs(
            "G01", tow, { { "L1C", l1 }, { "L5Q", 300.0 } }) };
    };
    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, l15Epoch(tow0 + k, k), 1.0);

    // The L1/L5 null-line premise itself: the (4,3) pair's GF jump is below
    // the noise (≈3.3 mm), as the comment above states.
    EXPECT_LT(std::abs(4.0 * kL1 - 3.0 * kL5), 0.004);

    const auto r = h.te.resolve(
        { makeMwFlag("G01", +1, true, "L1C", "L5Q") }, {},
        l15Epoch(tow0 + 12, 12), 1.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_EQ(r.code_events[0].sig2, "L5Q");
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.repairs.empty());
}

// ---------------------------------------------------------------------------
// Gap-spanning GF bridge
// ---------------------------------------------------------------------------

// Per-signal dropout, modelled on the COVL145 G03 event: L2W drops for several
// epochs while L1C survives, and on return L2W has re-locked 3 cycles low
// (across-gap GF jump +3λ₂ ≈ +0.733 m).  The MW arc spans the dropout and flags
// with the integer withheld (locked-arc TD-confirm contract) but a float estimate
// available.  With the lattice-aware candidate window (±3) the instantaneous
// float alone is ambiguous (k = 1, 3, 5 all fit +3λ₂ within the fractional gate;
// the null-line spacing is 2 in k), so the resolver defers to the WL-settle
// estimate.  Once the post-restart MW mean settles, the solve re-runs and
// dispatches (dN1 = 0, dN2 = -3) as: L2W moot (its ambiguity was pruned and
// reseeded on return, so the slip is already absorbed), and L1C clean (mandate
// suppressed; the converged ambiguity survives).  No resets, no repairs.
TEST(TurboEditGapBridge, PerSignalDropoutMootAttribution) {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "gnss_te_moot_settle_test.json";
    {
        std::ofstream f(p);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_bridge_lli": true } } })";
    }
    LogManager log_manager;
    GnssConfig config;
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    { std::error_code ec; fs::remove(p, ec); }
    TurboEditResolver te{ config, log_manager };
    te.setMaxGapSeconds(1.5);  // production RT tolerance (1.5 × 1 Hz)
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);

    // 4 epochs with L2W absent.  L1C keeps the satellite observed, so these
    // epochs occur: L2W's continuity breaks, and the state side prunes and
    // reseeds its ambiguity.
    for (int k = 12; k < 16; ++k) {
        const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
        te.resolve({}, {}, { makePhaseObs("G01", tow0 + k, { { "L1C", l1 } }) },
                   1.0);
    }

    // Return epoch: L2W re-locked 3 cycles low → GF jumps +3λ₂.  MW flags with
    // unknown integer but float ≈ +3.2 WL cycles, which is lattice-ambiguous,
    // so the adjudication must defer (nothing actioned, sat pending).
    const auto r1 = te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", /*float=*/3.2) },
        {},
        flatEpoch("G01", tow0 + 16, 16, /*extra_l1=*/0.0, /*extra_l2=*/-3.0),
        1.0);
    EXPECT_EQ(r1.pending_sats.count("G01"), 1u);
    EXPECT_TRUE(r1.repairs.empty());
    EXPECT_TRUE(r1.moot_signals.empty());
    EXPECT_TRUE(r1.reset_sats.empty());
    EXPECT_TRUE(r1.code_events.empty());

    // Settled across-gap WL evidence arrives: k = +3 selected directly →
    // moot attribution for the reseeded L2W, clean for the continuous L1C.
    std::map<std::string, MwSlipDetector::AcrossGapWlEvidence> ev;
    ev["G01_L1C_L2W"] = { /*float=*/2.98, /*post_count=*/12,
                          /*sigma_mean=*/0.05, /*stable=*/true };
    const auto r2 = te.resolve(
        {}, {},
        flatEpoch("G01", tow0 + 17, 1, /*extra_l1=*/0.0, /*extra_l2=*/-3.0),
        1.0, ev);

    ASSERT_EQ(r2.moot_signals.count("G01"), 1u);
    EXPECT_EQ(r2.moot_signals.at("G01").count("L2W"), 1u);
    ASSERT_EQ(r2.clean_signals.count("G01"), 1u);
    EXPECT_EQ(r2.clean_signals.at("G01").count("L1C"), 1u);
    EXPECT_TRUE(r2.repairs.empty());
    EXPECT_TRUE(r2.reset_sats.empty());
    EXPECT_TRUE(r2.code_events.empty());
    EXPECT_TRUE(r2.pending_sats.empty());
}

// Full receiver gap (no epochs at all for 8 s): nothing was pruned, so a
// re-locked L2W carries a surviving ambiguity that needs the integer repair;
// continuity tracking must dispatch it to repairs, not moot.  Here the MW
// integer is known (k = +3).
TEST(TurboEditGapBridge, ReceiverGapRepairAttribution) {
    TeHarness h;
    h.te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);

    // 8 s of silence (no resolve calls), then L2W returns 3 cycles low.
    const double tow_ret = tow0 + 11 + 8.0;
    const auto r = h.te.resolve(
        { makeMwFlag("G01", +3, /*wl_known=*/true) }, {},
        flatEpoch("G01", tow_ret, 12, /*extra_l1=*/0.0, /*extra_l2=*/-3.0),
        1.0);

    ASSERT_EQ(r.repairs.size(), 1u);
    EXPECT_EQ(r.repairs[0].signal, "L2W");
    EXPECT_EQ(r.repairs[0].dN, -3);
    ASSERT_EQ(r.clean_signals.count("G01"), 1u);
    EXPECT_EQ(r.clean_signals.at("G01").count("L1C"), 1u);
    EXPECT_TRUE(r.moot_signals.empty());
    EXPECT_TRUE(r.reset_sats.empty());
}

// Flat across the gap: an MW flag with an unmoved GF level is the
// re-acquisition DLL transient, so it is a bridged code event and nothing is
// reset or repaired.
TEST(TurboEditGapBridge, FlatAcrossGapCodeEvent) {
    TeHarness h;
    h.te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);
    for (int k = 12; k < 16; ++k) {
        const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
        h.te.resolve({}, {}, { makePhaseObs("G01", tow0 + k, { { "L1C", l1 } }) },
                     1.0);
    }

    const auto r = h.te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", /*float=*/3.2) },
        {},
        flatEpoch("G01", tow0 + 16, 16), 1.0);

    ASSERT_EQ(r.code_events.size(), 1u);
    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.moot_signals.empty());
    EXPECT_TRUE(r.reset_sats.empty());
}

// Triple-signal single-sat epoch: L1C carries the GF noise, L2W and L2L
// are constant unless offset.  GF(L1C/L2L) stays flat throughout (the
// sibling arc).  Omitting L2W (have_l2w=false) models its per-signal outage
// while the satellite stays observed.
std::vector<PppObservation> flatEpoch3(const std::string& sat, double tow, int k,
                                       bool have_l2w = true,
                                       double extra_l2w_cycles = 0.0) {
    const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
    std::map<std::string, double> phases{ { "L1C", l1 }, { "L2L", 250.0 } };
    if (have_l2w) phases["L2W"] = 200.0 + extra_l2w_cycles;
    return { makePhaseObs(sat, tow, phases) };
}

// Field regression shape (G03 at week 2420 TOW 109772, covl145): the MW
// locked-arc TD-confirmation delivers its across-gap flag only after the GF
// chain has rebuilt and owns its own (flat, gap-blind) TD2, and the
// instantaneous re-acquisition float is NL-code-biased (25.75 against the true
// k = +28).  The bias is large enough that a ±1 candidate window excludes the
// true integer and yields a confident wrong repair ((−9, −35) instead of
// (0, −28)).  The sibling L1C/L2L arc runs flat and unbroken through the L2W
// outage, affirming dN(L1C) = 0; with the wide candidate window that
// affirmation must prune every nonzero-dN1 lattice candidate and select k = +28.
TEST(TurboEditGapBridge, SiblingAffirmationSelectsTrueKOverBiasedFloat) {
    TeHarness h;
    h.te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, flatEpoch3("G01", tow0 + k, k), 1.0);

    // The state side holds L2W's ambiguity across the outage (the gap-hold /
    // LLI machinery), so the bridge must dispatch a repair, not a moot.
    h.te.markHeldAcrossLli("G01", "L2W");

    // 2-epoch L2W outage; L1C and L2L stay continuous (their GF chain never
    // breaks: the affirming sibling).
    for (int k = 12; k < 14; ++k)
        h.te.resolve({}, {}, flatEpoch3("G01", tow0 + k, k, /*have_l2w=*/false),
                     1.0);

    // L2W returns 28 cycles low; two quiet epochs (MW TD-confirmation lag).
    for (int k = 14; k < 16; ++k)
        h.te.resolve({}, {},
                     flatEpoch3("G01", tow0 + k, k, true, /*extra_l2w=*/-28.0),
                     1.0);

    // MW finally flags with the integer withheld and the float biased 2.25
    // WL low (re-acquisition NL-code transient).  ±3 window admits the
    // true k = +28; the sibling affirmation kills k = 26 / 24 / 30 (all
    // demand dN1 ≠ 0 on the affirmed-still L1C) → unique → repair.
    const auto r = h.te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", /*float=*/25.75) },
        {},
        flatEpoch3("G01", tow0 + 16, 16, true, /*extra_l2w=*/-28.0),
        1.0);

    EXPECT_TRUE(r.code_events.empty());
    ASSERT_EQ(r.repairs.size(), 1u);
    EXPECT_EQ(r.repairs[0].signal, "L2W");
    EXPECT_EQ(r.repairs[0].dN, -28);
    ASSERT_EQ(r.clean_signals.count("G01"), 1u);
    EXPECT_EQ(r.clean_signals.at("G01").count("L1C"), 1u);
    EXPECT_TRUE(r.moot_signals.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.pending_sats.empty());
}

// Without a sibling the same biased float is lattice-ambiguous.  With the
// WL-settle deferral disabled (the default: turbo_edit_bridge_lli off) the
// resolver must take the conservative reset, never a window-relative "unique"
// guess.
TEST(TurboEditGapBridge, AmbiguousFloatWithoutSiblingOrSettleResets) {
    TeHarness h;
    h.te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);

    // Full receiver gap (ambiguities survive), L2W re-locks 28 cycles low.
    const double tow_ret = tow0 + 11 + 2.0;
    const auto r = h.te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", /*float=*/25.75) },
        {},
        flatEpoch("G01", tow_ret, 12, /*extra_l1=*/0.0, /*extra_l2=*/-28.0),
        1.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.pending_sats.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// With the deferral enabled the ambiguous case waits (holds kept, mandate
// suppressed) and repairs once the across-gap WL estimate settles.
TEST(TurboEditGapBridge, AmbiguousFloatDefersThenSettlesToRepair) {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "gnss_te_settle_repair_test.json";
    {
        std::ofstream f(p);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_bridge_lli": true } } })";
    }
    LogManager log_manager;
    GnssConfig config;
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    { std::error_code ec; fs::remove(p, ec); }
    TurboEditResolver te{ config, log_manager };
    te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);

    // Full receiver gap; L2W re-locks 28 cycles low; flag with biased float.
    const double tow_ret = tow0 + 11 + 2.0;
    const auto r1 = te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", /*float=*/25.75) },
        {},
        flatEpoch("G01", tow_ret, 12, 0.0, -28.0), 1.0);
    EXPECT_EQ(r1.pending_sats.count("G01"), 1u);
    EXPECT_TRUE(r1.repairs.empty());
    EXPECT_TRUE(r1.reset_sats.empty());
    EXPECT_TRUE(r1.code_events.empty());

    // Not settled yet → still pending.
    const auto r2 = te.resolve(
        {}, {}, flatEpoch("G01", tow_ret + 1, 13, 0.0, -28.0), 1.0);
    EXPECT_EQ(r2.pending_sats.count("G01"), 1u);
    EXPECT_TRUE(r2.repairs.empty());

    // Settled at +27.99 → k = +28 selected directly → repair (full receiver
    // gap: nothing was pruned, both signals continuous → genuine repair).
    std::map<std::string, MwSlipDetector::AcrossGapWlEvidence> ev;
    ev["G01_L1C_L2W"] = { 27.99, 15, 0.04, true };
    const auto r3 = te.resolve(
        {}, {}, flatEpoch("G01", tow_ret + 2, 14, 0.0, -28.0), 1.0, ev);
    ASSERT_EQ(r3.repairs.size(), 1u);
    EXPECT_EQ(r3.repairs[0].signal, "L2W");
    EXPECT_EQ(r3.repairs[0].dN, -28);
    ASSERT_EQ(r3.clean_signals.count("G01"), 1u);
    EXPECT_EQ(r3.clean_signals.at("G01").count("L1C"), 1u);
    EXPECT_TRUE(r3.pending_sats.empty());
    EXPECT_TRUE(r3.reset_sats.empty());
}

// A deferral that never settles must terminate in the conservative reset at
// the configured cap, so a satellite cannot stay pending indefinitely.
TEST(TurboEditGapBridge, DeferralTimeoutFallsBackToReset) {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "gnss_te_settle_timeout_test.json";
    {
        std::ofstream f(p);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_bridge_lli": true,
                "turbo_edit_wl_settle_max_s": 3.0 } } })";
    }
    LogManager log_manager;
    GnssConfig config;
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    { std::error_code ec; fs::remove(p, ec); }
    ASSERT_DOUBLE_EQ(config.getCycleSlipConfig().turbo_edit_wl_settle_max_s, 3.0);
    TurboEditResolver te{ config, log_manager };
    te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);

    const double tow_ret = tow0 + 11 + 2.0;
    const auto r1 = te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", /*float=*/25.75) },
        {},
        flatEpoch("G01", tow_ret, 12, 0.0, -28.0), 1.0);
    EXPECT_EQ(r1.pending_sats.count("G01"), 1u);

    // No evidence ever arrives; the cap (3 s) expires within a few epochs.
    bool reset_seen = false;
    for (int k = 1; k <= 6 && !reset_seen; ++k) {
        const auto r = te.resolve(
            {}, {}, flatEpoch("G01", tow_ret + k, 12 + k, 0.0, -28.0), 1.0);
        if (r.reset_sats.count("G01")) {
            reset_seen = true;
            EXPECT_TRUE(r.pending_sats.empty());
            EXPECT_TRUE(r.repairs.empty());
        }
    }
    EXPECT_TRUE(reset_seen);
}

// Beyond the bridge cap the behavior without the bridge must return exactly: full
// arc wipe, the post-gap pair is uncached, and the resolver stays silent
// (the caller's own mandate resets).
TEST(TurboEditGapBridge, GapBeyondCapLeavesResolverSilent) {
    TeHarness h;
    h.te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);

    const double tow_ret = tow0 + 11 + 20.0;  // > 15 s cap
    const auto r = h.te.resolve(
        { makeMwFlag("G01", +3) }, {},
        flatEpoch("G01", tow_ret, 12, 0.0, -3.0), 1.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.moot_signals.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.clean_signals.empty());
}

// No WL information at all (integer withheld and no float): the bridge cannot
// solve the 2×2, because the GF equation alone is degenerate along the
// null-line lattice.  The resolver stays silent and the caller's mandate
// fires, exactly as without the bridge.
TEST(TurboEditGapBridge, NoWlInformationStaysSilent) {
    TeHarness h;
    h.te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;

    for (int k = 0; k < 12; ++k)
        h.te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);
    for (int k = 12; k < 16; ++k) {
        const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
        h.te.resolve({}, {}, { makePhaseObs("G01", tow0 + k, { { "L1C", l1 } }) },
                     1.0);
    }

    const auto r = h.te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W") }, {},
        flatEpoch("G01", tow0 + 16, 16, 0.0, -3.0), 1.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.moot_signals.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
}

// Bridge disabled (cap = 0) through the real config loader: the behavior
// without the bridge, in full.
TEST(TurboEditGapBridge, ConfigDisableRestoresSilence) {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "gnss_te_bridge_off_test.json";
    {
        std::ofstream f(p);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_max_gap_s": 0.0 } } })";
    }
    LogManager log_manager;
    GnssConfig config;
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    ASSERT_EQ(config.getCycleSlipConfig().turbo_edit_max_gap_s, 0.0);

    TurboEditResolver te{ config, log_manager };
    te.setMaxGapSeconds(1.5);
    const double tow0 = 100000.0;
    for (int k = 0; k < 12; ++k)
        te.resolve({}, {}, flatEpoch("G01", tow0 + k, k), 1.0);
    for (int k = 12; k < 16; ++k) {
        const double l1 = 100.0 + kGfNoiseM[k % 16] / kL1;
        te.resolve({}, {}, { makePhaseObs("G01", tow0 + k, { { "L1C", l1 } }) }, 1.0);
    }

    const auto r = te.resolve(
        { makeMwFlag("G01", 0, /*wl_known=*/false, "L1C", "L2W", 3.2) }, {},
        flatEpoch("G01", tow0 + 16, 16, 0.0, -3.0), 1.0);

    EXPECT_TRUE(r.moot_signals.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.repairs.empty());

    std::error_code ec;
    fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// Behaviors the veto must not disturb (regression)
// ---------------------------------------------------------------------------

// The window between the affirmative-zero gate (3.5×MAD ≈ 2.3 mm on this
// harness noise) and the GF detection floor (0.04 m): a genuine (4,3)
// L1/L2 pair jumps GF by 4λ1−3λ2 ≈ +2.85 cm: quiet enough that gf_exceeds
// stays false and the veto eligibility check is entered, yet 12× the zero
// gate, so the veto must decline and the joint solve must deliver the (4,3)
// repair.  Pins the gate-tighter-than-floor property
// and the no-absolute-floor constraint: any widening of the zero gate
// toward the detection floor converts this repair into a veto and fails
// here.
TEST(TurboEditJointSolve, SmallPairBetweenZeroGateAndFloorStillRepairs) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 12, /*extra_l1=*/4.0,
                                          /*extra_l2=*/3.0), 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    ASSERT_EQ(r.repairs.size(), 2u);
    std::map<std::string, int> by_sig;
    for (const auto& rep : r.repairs) by_sig[rep.signal] = rep.dN;
    EXPECT_EQ(by_sig["L1C"], 4);
    EXPECT_EQ(by_sig["L2W"], 3);
}

// A real (1,0) slip: MW k=+1 with the matching GF jump of +λ1 ≈ +19 cm is
// far outside the affirmative-zero gate, so the joint solve must still
// deliver the targeted repair with the partner adjudicated clean.
TEST(TurboEditJointSolve, RealPairStillRepairs) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +1) }, {},
                                flatEpoch("G01", tow, 12, /*extra_l1=*/1.0), 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    ASSERT_EQ(r.repairs.size(), 1u);
    EXPECT_EQ(r.repairs[0].satellite, "G01");
    EXPECT_EQ(r.repairs[0].signal, "L1C");
    EXPECT_EQ(r.repairs[0].dN, 1);
    ASSERT_EQ(r.clean_signals.count("G01"), 1u);
    EXPECT_EQ(r.clean_signals.at("G01").count("L2W"), 1u);
}

// MW's dN1 == dN2 blind spot: a (1,1) equal slip jumps GF by λ1−λ2 ≈
// −5.4 cm with no MW flag.  With affirmative MW cleanliness supplied, the
// GF-only split must still repair both signals (and the veto, which keys
// on MW flags, must stay out of the way).
TEST(TurboEditJointSolve, GfOnlyEqualSlipStillRepairs) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 1.0);

    const auto r = h.te.resolve({}, { "G01_L1C_L2W" },
                                flatEpoch("G01", tow, 12, /*extra_l1=*/1.0,
                                          /*extra_l2=*/1.0), 1.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    ASSERT_EQ(r.repairs.size(), 2u);
    std::map<std::string, int> by_sig;
    for (const auto& rep : r.repairs) by_sig[rep.signal] = rep.dN;
    EXPECT_EQ(by_sig["L1C"], 1);
    EXPECT_EQ(by_sig["L2W"], 1);
}

// ---------------------------------------------------------------------------
// Low-rate (30 s RINEX) operation: GF trust is evidence-gated, not
// rate-gated.  Quiet ionosphere → the full repair machinery engages at
// 30 s; iono-inflated arcs → MW-flagged sats demote to the conservative
// reset and unattributable GF-only transients are ignored (KF backstop).
// ---------------------------------------------------------------------------

// The main low-rate case: a large dual-signal slip ((10,7), ΔN_WL=+3, GF
// jump ≈ +19.3 cm) at 30 s sampling under quiet ionosphere must split and
// repair exactly as at 1 s: the measured sub-mm TD2 noise passes the
// split gate regardless of the declared interval.
TEST(TurboEditLowRate, QuietIonoLargeSlipRepairsAt30s) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 30.0);

    const auto r = h.te.resolve({ makeMwFlag("G01", +3) }, {},
                                flatEpoch("G01", tow, 12, /*extra_l1=*/10.0,
                                          /*extra_l2=*/7.0), 30.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    ASSERT_EQ(r.repairs.size(), 2u);
    std::map<std::string, int> by_sig;
    for (const auto& rep : r.repairs) by_sig[rep.signal] = rep.dN;
    EXPECT_EQ(by_sig["L1C"], 10);
    EXPECT_EQ(by_sig["L2W"], 7);
}

// The same large slip on an iono-inflated arc (TD2 MAD-σ ≈ 1.7 cm, a float
// ΔN2 noise of about 0.3 cycles): the split gate must refuse the per-signal
// attribution and the MW mandate must take the conservative reset.  Without
// the gate, the contaminated solve can round to a wrong integer pair with high
// confidence; the gate prevents that.
TEST(TurboEditLowRate, IonoInflatedArcMwSlipFallsBackToReset) {
    TeHarness h;
    const double scale = 25.0;
    const double tow = warmupNoisy(h.te, "G01", 12, 30.0, scale);

    const auto r = h.te.resolve(
        { makeMwFlag("G01", +3) }, {},
        noisyEpoch("G01", tow, 12, scale, /*extra_l1=*/10.0, /*extra_l2=*/7.0),
        30.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

// A GF-only exceedance on a split-incapable arc carries no MW mandate and
// cannot be attributed to per-signal integers, so it must be ignored, not
// reset: at 30 s a single-satellite iono transient (equatorial bubble, TEC
// gradient) routinely trips the GF gate without any slip, and resetting on
// each one would discard converged precision without cause.  A genuine equal
// slip here is left to the KF phase-innovation backstop.
TEST(TurboEditLowRate, GfOnlyExceedanceOnCoarseArcIgnored) {
    TeHarness h;
    const double scale = 25.0;
    const double tow = warmupNoisy(h.te, "G01", 12, 30.0, scale);

    // (3,3)-equivalent GF jump (−16.2 cm) with MW silent and no MW-clean
    // affirmation; without the gate this takes the !wl_known blanket reset.
    const auto r = h.te.resolve(
        {}, {},
        noisyEpoch("G01", tow, 12, scale, /*extra_l1=*/3.0, /*extra_l2=*/3.0),
        30.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.clean_signals.empty());
}

// MW's blind-spot coverage survives the rate expansion: a (1,1) equal slip
// at 30 s on a quiet arc, with MW affirmatively clean, still splits via
// the GF-only path (k=0): the |λ1−λ2| quantum is detectable because the
// 4 cm floor sits below it and the quiet-arc σ passes the split gate.
TEST(TurboEditLowRate, QuietIonoGfOnlyEqualSlipRepairsAt30s) {
    TeHarness h;
    const double tow = warmupFlat(h.te, "G01", 12, 30.0);

    const auto r = h.te.resolve({}, { "G01_L1C_L2W" },
                                flatEpoch("G01", tow, 12, /*extra_l1=*/1.0,
                                          /*extra_l2=*/1.0), 30.0);

    EXPECT_TRUE(r.code_events.empty());
    EXPECT_TRUE(r.reset_sats.empty());
    ASSERT_EQ(r.repairs.size(), 2u);
    std::map<std::string, int> by_sig;
    for (const auto& rep : r.repairs) by_sig[rep.signal] = rep.dN;
    EXPECT_EQ(by_sig["L1C"], 1);
    EXPECT_EQ(by_sig["L2W"], 1);
}

// The split gate is a config lever (estimation.cycle_slip.
// turbo_edit_split_max_sigma), loaded through the real JSON path: an
// extreme tightening (0.001 cycles ≈ 0.05 mm, below even the harness's
// sub-mm noise) must convert a quiet-arc 1 s repair into the conservative
// reset, which shows the key reaches the gate.
TEST(TurboEditLowRate, SplitGateConfigLeverReachesTheSolve) {
    namespace fs = std::filesystem;
    const fs::path p =
        fs::temp_directory_path() / "gnss_te_split_gate_test.json";
    {
        std::ofstream f(p);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_split_max_sigma": 0.001 } } })";
    }
    LogManager log_manager;
    GnssConfig config;
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    { std::error_code ec; fs::remove(p, ec); }
    ASSERT_DOUBLE_EQ(config.getCycleSlipConfig().turbo_edit_split_max_sigma,
                     0.001);

    TurboEditResolver te{ config, log_manager };
    double tow = 100000.0;
    for (int k = 0; k < 12; ++k, tow += 1.0)
        te.resolve({}, {}, flatEpoch("G01", tow, k), 1.0);

    // A real (1,0) slip that the default gate repairs (RealPairStillRepairs).
    const auto r = te.resolve({ makeMwFlag("G01", +1) }, {},
                              flatEpoch("G01", tow, 12, /*extra_l1=*/1.0), 1.0);

    EXPECT_TRUE(r.repairs.empty());
    EXPECT_EQ(r.reset_sats.count("G01"), 1u);
}

}  // namespace
}  // namespace GnssProcessor
