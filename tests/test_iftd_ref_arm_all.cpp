/**
 * @file test_iftd_ref_arm_all.cpp
 * @brief Gates for the reference-satellite slip arm on the additional IF-TD gradient
 *        chains (`network.cycle_slip.if_phase_td_ref_arm_all`,
 *        docs/SPEC_event_harness.md §22.6, REF-FAN), and for the fan-out it removes.
 *
 * ### The fan-out
 *
 * Under `if_phase_td_pairs = all` each band pair of a satellite has its own
 * between-epoch gradient chain (§14.10).  Without the knob, the reference-satellite
 * slip test (tier T1r) runs on the first chain only.  An additional chain adopts the
 * first chain's reference when that satellite is in its set and elects its own
 * otherwise; in neither case is a slip on that reference attributed.  The
 * reference's own double difference is identically 0 by construction, so the step
 * does not appear on its own rows; it appears in full on every other satellite of
 * the chain, where the pooled outlier test reads it as simultaneous slips on arcs
 * that did not slip.  The resulting false IF-TD refloats can re-realize a satellite
 * phase-bias datum network-wide (field example in §22.6).
 *
 * ### Why the arm needs the empirical gate
 *
 * Running the arm per chain through the legacy candidate condition
 * |median| > 0.5·λ_IF floods (§14.10.3): on (L2W,L5Q) the IF coefficients are
 * 12.26 / −11.26 (noise amplification 16.6) against λ_IF = 0.1247 m, so noise alone
 * clears the fixed floor (5 351 candidates and 64 fires on a quiet 70-epoch
 * 60-station null).  The knob therefore opens the arm only behind the empirical gate,
 * which scales with each chain's own dispersion, and the detector refuses
 * the combination in code as well as in the loader.
 *
 * ### Gates in this file
 *
 * - Off by default: with the knob absent the additional chains run no arm and push
 *   no reset key, and the fan-out stays on every non-reference DD.
 * - Own-elected reference: the arm runs on that chain, with that chain's dispersion
 *   state and λ_IF; the reference arc is reset and the fan-out is rebased out of
 *   every other satellite's DD.
 * - Adopted reference: the first chain's verdict is inherited, never re-derived.
 * - Second lock: armed without the empirical gate, the additional chains still run
 *   no arm, so the legacy condition never gates a wide chain.
 * - Byte identity: with `pairs = first`, and for the first chain's own numbers,
 *   both knob settings give identical results.
 * - The loader interlock (both prerequisites) and the armed load.
 *
 * All tests drive the public API with synthetic uncombined (PPP-RTK) observations.
 * The IF coefficients satisfy c1 + c2 == 1 exactly, so giving every band of a
 * satellite the same observed-minus-modeled value makes the IF OMC of every pair
 * equal that value: the quiet stream is designed for all three pairs at once, and
 * a per-band offset is the only thing that separates them.  Detectors run with
 * `setDeferFlagging(true)`, as in the network wiring; in that mode only the
 * reference arm pushes reset keys, so a nonempty return from `detect()` comes from
 * a chain's reference arm.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/IfPhaseSlipDetector.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

constexpr double kF1 = Constants::GPS_L1_FREQ;
constexpr double kF2 = Constants::GPS_L2_FREQ;
constexpr double kF5 = Constants::GPS_L5_FREQ;

/// Chain keys.  The first pair of a satellite keeps the bare system string (its
/// decisions are byte-identical to those without the knob); every additional pair
/// is "<system>|<sig1>_<sig2>".
const std::string kChain12 = "GPS";
const std::string kChain15 = "GPS|L1C_L5Q";
const std::string kChain25 = "GPS|L2W_L5Q";

/// The high-band IF coefficient c1 = fa²/(fa² − fb²): what a step on band a
/// alone does to the (a, b) iono-free combination.
double c1Of(double fa, double fb) { return (fa * fa) / (fa * fa - fb * fb); }
/// The low-band IF coefficient c2 = −fb²/(fa² − fb²).
double c2Of(double fa, double fb) { return -(fb * fb) / (fa * fa - fb * fb); }

/// Deterministic sub-millimetre wobble (m) so no chain's dispersion is
/// degenerate.  Three orders of magnitude below the ~5 cm physical floor, so it
/// cannot move a verdict.
constexpr double kWobbleM[8] = { 0.0004, -0.0003, 0.0002,  0.0005,
                                -0.0006,  0.0001, -0.0002, 0.0003 };
double wobble(std::size_t epoch, std::size_t sat) {
    return kWobbleM[(epoch * 3u + sat) % 8u];
}

using OmcState   = std::map<std::string, double>;                          ///< satellite → common IF OMC (m)
using ElevState  = std::map<std::string, double>;                          ///< satellite → elevation (deg)
using BandState  = std::map<std::string, std::vector<std::string>>;        ///< satellite → its tracked phase bands
using BandOffset = std::map<std::string, std::map<std::string, double>>;   ///< satellite → (band → extra OMC on that band alone)

/// One epoch of uncombined observations.  Each satellite carries exactly the
/// bands @p bands names for it, so a dual-band satellite contributes only to the
/// first chain and is absent from the additional ones, which then elect their own
/// reference.
std::vector<PppObservation> makeEpoch(double tow, std::size_t epoch_idx,
                                      const OmcState& omc_m,
                                      const ElevState& elev_deg,
                                      const BandState& bands,
                                      const BandOffset& extra = {}) {
    std::vector<PppObservation> epoch;
    epoch.reserve(omc_m.size());
    std::size_t k = 0;
    for (const auto& [sat, omc] : omc_m) {
        const double v = omc + wobble(epoch_idx, k++);
        PppObservation obs{};
        obs.satellite = sat;
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2400, tow };
        obs.elevation = elev_deg.at(sat);
        obs.azimuth   = 0.0;
        obs.los       = Eigen::Vector3d(0.0, 0.0, 1.0);
        obs.sat_pos   = Eigen::Vector3d::Zero();
        obs.rx_pos_linearization = Eigen::Vector3d::Zero();
        const auto eit = extra.find(sat);
        for (const auto& sig : bands.at(sat)) {
            double add = 0.0;
            if (eit != extra.end()) {
                const auto sit = eit->second.find(sig);
                if (sit != eit->second.end()) add = sit->second;
            }
            obs.modeled[sig]  = 0.0;
            obs.observed[sig] = v + add;
        }
        epoch.push_back(std::move(obs));
    }
    return epoch;
}

/// The armed gate with its default parameters (k = 6, min_n = 4,
/// thin_widen = 3, coherence = 0.5, max_consecutive = 1, no warm-up abstention).
IfPhaseSlipDetector::RefGateParams armedGate() {
    IfPhaseSlipDetector::RefGateParams p;
    p.enable = true;
    return p;
}

/// Detector under test plus the config/log it borrows.  Member order keeps the
/// references alive for the detector.
struct FanHarness {
    LogManager log_manager;
    GnssConfig config;                      // defaults: 10 deg elevation mask
    IfPhaseSlipDetector det{ config, log_manager };

    /// @param all_pairs  `if_phase_td_pairs = all`.
    /// @param ref_gate   the empirical reference gate.
    /// @param ref_arm    the arm on the additional chains (§22.6).
    FanHarness(bool all_pairs, bool ref_gate, bool ref_arm) {
        det.setDeferFlagging(true);         // the network wiring
        det.setAllPairs(all_pairs);
        if (ref_gate) det.setRefGate(armedGate());
        det.setRefArmAllChains(ref_arm);
    }

    std::vector<std::string> step(std::size_t epoch_idx, const OmcState& omc,
                                  const ElevState& elev, const BandState& bands,
                                  const BandOffset& extra = {}) {
        return det.detect(makeEpoch(30.0 * static_cast<double>(epoch_idx),
                                    epoch_idx, omc, elev, bands, extra));
    }

    /// The cached double difference of one (satellite, pair), or NaN when the
    /// pair produced nothing this epoch.
    double dd(const std::string& sat, const std::string& pair) const {
        const auto it = det.getLastIfJumpsAllPairs().find({ sat, pair });
        return (it == det.getLastIfJumpsAllPairs().end())
                   ? std::numeric_limits<double>::quiet_NaN()
                   : it->second.delta_grad_m;
    }

    /// This chain's reference-arm counters, zeroed when absent (absence means the
    /// arm never ran on that chain).
    IfPhaseSlipDetector::RefArmChainCounters chain(const std::string& key) const {
        const auto it = det.refArmChainCounters().find(key);
        return (it == det.refArmChainCounters().end())
                   ? IfPhaseSlipDetector::RefArmChainCounters{}
                   : it->second;
    }
};

/// Six GPS satellites.  G01 is dual-band (L1C+L2W only), so it is present in the
/// first chain and absent from both L5 chains, which therefore elect their own
/// reference.  G02 is the highest-elevation satellite those chains see, and both
/// it and G01 clear the 45° reference-retention bar, so the test controls which
/// satellite is the reference.
BandState ownElectedBands() {
    return { { "G01", { "L1C", "L2W" } },
             { "G02", { "L1C", "L2W", "L5Q" } },
             { "G03", { "L1C", "L2W", "L5Q" } },
             { "G04", { "L1C", "L2W", "L5Q" } },
             { "G05", { "L1C", "L2W", "L5Q" } },
             { "G06", { "L1C", "L2W", "L5Q" } } };
}

/// Every satellite triple-band: every chain contains G01, so every additional
/// chain adopts the first chain's reference.
BandState adoptedBands() {
    BandState b = ownElectedBands();
    b["G01"] = { "L1C", "L2W", "L5Q" };
    return b;
}

ElevState sixSatElevations() {
    return { { "G01", 80.0 }, { "G02", 70.0 }, { "G03", 40.0 },
             { "G04", 35.0 }, { "G05", 30.0 }, { "G06", 25.0 } };
}

OmcState sixSatOmc() {
    return { { "G01", 0.00 }, { "G02", 0.12 }, { "G03", -0.07 },
             { "G04", 0.21 }, { "G05", -0.15 }, { "G06", 0.05 } };
}

bool hasKey(const std::vector<std::string>& keys, const std::string& k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
}

/// Only the reset keys of the first chain's pair, which must not change when the
/// additional chains are armed.
std::vector<std::string> firstChainKeys(const std::vector<std::string>& keys) {
    std::vector<std::string> out;
    for (const auto& k : keys)
        if (k.size() > 8 && k.compare(k.size() - 8, 8, "_L1C_L2W") == 0) out.push_back(k);
    return out;
}

/// Bit-identity for a double difference, NaN included: the first epoch of an arc
/// has no previous gradient and is a designed NaN, so "identical" has to mean
/// identical rather than equal (`nan == nan` is false).
::testing::AssertionResult SameDd(const char* ea, const char* eb,
                                  double a, double b) {
    if ((std::isnan(a) && std::isnan(b)) || a == b) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << ea << " = " << a << " but " << eb << " = " << b;
}

// --------------------------------------------------------------- config I/O

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("reffan_cfg_" + tag + ".json");
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

/// A `network.cycle_slip` block with the three interlocked knobs.
std::string cycleSlipBlock(const char* pairs, bool ref_gate, bool ref_arm_all) {
    return std::string(
        "    \"cycle_slip\": {\n"
        "      \"if_phase_td\": true,\n"
        "      \"if_phase_td_pairs\": \"") + pairs + "\",\n"
        "      \"if_phase_td_ref_gate\": " + (ref_gate ? "true" : "false") + ",\n"
        "      \"if_phase_td_ref_arm_all\": " + (ref_arm_all ? "true" : "false") + "\n"
        "    },\n";
}

bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log,
              const std::string& tag) {
    const fs::path p = writeTempConfig(tag, networkConfig(extra));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

// ===========================================================================
// Off by default: the fan-out reproduced
// ===========================================================================

// With `pairs = all` and the empirical gate armed but the knob absent, an
// additional chain runs no reference arm: the step on its own reference stays on
// every non-reference DD of that chain (the fan-out the pooled test then attributes
// to arcs that did not slip), and no reset key names the reference.  This is the
// baseline the armed tests are compared against.
TEST(IfTdRefArmAll, DefaultOffLeavesTheFanOutOnEveryInnocentArc) {
    FanHarness h(/*all_pairs=*/true, /*ref_gate=*/true, /*ref_arm=*/false);
    const auto elev  = sixSatElevations();
    const auto bands = ownElectedBands();
    OmcState omc = sixSatOmc();

    for (std::size_t e = 0; e < 3; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());

    // A −0.4 m step on G02's L5Q alone.  G02 is the reference elected by both L5
    // chains (G01 has no L5 to be adopted from), so the (1,5) and (2,5)
    // combinations move on the reference itself while (1,2) stays quiet.
    const BandOffset slip = { { "G02", { { "L5Q", -0.4 } } } };
    const auto keys = h.step(3, omc, elev, bands, slip);

    EXPECT_TRUE(keys.empty())
        << "an additional chain has no reference arm today, so nothing is reset";

    // The fan-out: every non-reference satellite of the (1,5) chain carries the
    // reference's whole step.  The reference's own row is the structural zero.
    const double fan15 = -c2Of(kF1, kF5) * (-0.4);
    const double fan25 = -c2Of(kF2, kF5) * (-0.4);
    EXPECT_NEAR(h.dd("G02", "L1C_L5Q"), 0.0, 1e-9);
    for (const char* sat : { "G03", "G04", "G05", "G06" }) {
        EXPECT_NEAR(h.dd(sat, "L1C_L5Q"), fan15, 5e-3) << sat;
        EXPECT_NEAR(h.dd(sat, "L2W_L5Q"), fan25, 5e-2) << sat;
    }
    EXPECT_NEAR(h.dd("G03", "L1C_L2W"), 0.0, 5e-3) << "the first chain is quiet";

    // Invariant #4: the additional chains report no opportunity at all because the
    // arm did not run there.  Zero counts here mean the arm is absent, not clean.
    EXPECT_EQ(h.chain(kChain15).opportunities, 0);
    EXPECT_EQ(h.chain(kChain25).opportunities, 0);
    EXPECT_GT(h.chain(kChain12).opportunities, 0) << "the first chain's arm always runs";
}

// ===========================================================================
// (i) Own-elected reference: tested on its own chain, through the gate
// ===========================================================================

TEST(IfTdRefArmAll, OwnElectedReferenceIsResetAndTheFanOutIsRebasedOut) {
    FanHarness h(/*all_pairs=*/true, /*ref_gate=*/true, /*ref_arm=*/true);
    const auto elev  = sixSatElevations();
    const auto bands = ownElectedBands();
    OmcState omc = sixSatOmc();

    for (std::size_t e = 0; e < 3; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());

    const BandOffset slip = { { "G02", { { "L5Q", -0.4 } } } };
    const auto keys = h.step(3, omc, elev, bands, slip);

    // The reference's own arc on each L5 chain, keyed by that chain's pair.
    EXPECT_TRUE(hasKey(keys, "G02_L1C_L5Q")) << "the (1,5) chain's reference arc";
    EXPECT_TRUE(hasKey(keys, "G02_L2W_L5Q")) << "the (2,5) chain's reference arc";
    // Nothing on the first chain, which did not move.
    EXPECT_TRUE(firstChainKeys(keys).empty());

    // The fan-out is gone: the rebase leaves only the per-satellite residual, so
    // the pooled test downstream sees noise instead of four simultaneous slips.
    for (const char* sat : { "G03", "G04", "G05", "G06" }) {
        EXPECT_NEAR(h.dd(sat, "L1C_L5Q"), 0.0, 5e-3) << sat;
        EXPECT_NEAR(h.dd(sat, "L2W_L5Q"), 0.0, 5e-2) << sat;
    }

    // The counters record which arm decided, and on how many rows the fan-out was
    // removed.
    EXPECT_EQ(h.chain(kChain15).fires_own, 1);
    EXPECT_EQ(h.chain(kChain15).fires_inherited, 0) << "no adoption: G01 has no L5";
    EXPECT_GT(h.chain(kChain15).opportunities, 0);
    EXPECT_EQ(h.chain(kChain15).candidates, 1);
    EXPECT_EQ(h.chain(kChain15).fanout_suppressed, 4) << "G03..G06";
    EXPECT_EQ(h.chain(kChain25).fires_own, 1);
    EXPECT_EQ(h.chain(kChain25).fanout_suppressed, 4);
    EXPECT_EQ(h.chain(kChain12).fires_own, 0) << "the first chain never moved";
}

// The arm on an additional chain uses that chain's own dispersion state and λ_IF,
// not the first chain's.  (L2W,L5Q) has |c2| = 11.26 against 1.26 on (L1C,L5Q), so
// the same L5 step is about 9× larger there.  Each chain's EWMA advances on its own
// samples, so the empirical bar tracks each combination's own noise.
TEST(IfTdRefArmAll, EachChainJudgesOnItsOwnScaleAndWavelength) {
    FanHarness h(/*all_pairs=*/true, /*ref_gate=*/true, /*ref_arm=*/true);
    const auto elev  = sixSatElevations();
    const auto bands = ownElectedBands();
    OmcState omc = sixSatOmc();

    // Six quiet epochs so each chain has folded its own samples into its own
    // ref_scale EWMA before anything is judged.
    for (std::size_t e = 0; e < 6; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());
    EXPECT_EQ(h.chain(kChain15).opportunities, h.chain(kChain25).opportunities)
        << "same family, same denominator";
    EXPECT_EQ(h.chain(kChain15).candidates, 0) << "the quiet stream is under every floor";
    EXPECT_EQ(h.chain(kChain25).candidates, 0)
        << "the WIDE combination must not be a candidate on noise — that is §14.10.3";

    // A step just over the (1,5) floor (0.5·λ_IF = 54.5 mm), and 9× larger on (2,5).
    const double d = -0.05;                                  // m on L5Q alone
    EXPECT_GT(std::abs(c2Of(kF1, kF5) * d), 0.5 * 0.1089);
    const BandOffset slip = { { "G02", { { "L5Q", d } } } };
    const auto keys = h.step(6, omc, elev, bands, slip);
    EXPECT_TRUE(hasKey(keys, "G02_L1C_L5Q"));
    EXPECT_TRUE(hasKey(keys, "G02_L2W_L5Q"));
    EXPECT_EQ(h.chain(kChain15).fires_own, 1);
    EXPECT_EQ(h.chain(kChain25).fires_own, 1);
}

// ===========================================================================
// (ii) Adopted reference: the first chain's verdict, inherited
// ===========================================================================

TEST(IfTdRefArmAll, AdoptedReferenceInheritsTheFirstChainsFire) {
    const auto elev  = sixSatElevations();
    const auto bands = adoptedBands();          // every chain contains G01
    // A step on G01's L1C alone: it moves (1,2) by c1 = 2.546 and (1,5) by
    // c1 = 2.261, and leaves (2,5), which does not contain L1C, unchanged.
    const BandOffset slip = { { "G01", { { "L1C", 0.2 } } } };

    std::vector<std::string> keys_off, keys_on;
    double fan_off = 0.0, fan_on = 0.0;
    IfPhaseSlipDetector::RefArmChainCounters c15_on{}, c25_on{}, c15_off{};

    for (const bool arm : { false, true }) {
        FanHarness h(/*all_pairs=*/true, /*ref_gate=*/true, /*ref_arm=*/arm);
        OmcState omc = sixSatOmc();
        for (std::size_t e = 0; e < 3; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());
        auto k = h.step(3, omc, elev, bands, slip);
        (arm ? keys_on : keys_off) = k;
        (arm ? fan_on : fan_off)   = h.dd("G03", "L1C_L5Q");
        if (arm) { c15_on = h.chain(kChain15); c25_on = h.chain(kChain25); }
        else     { c15_off = h.chain(kChain15); }
    }

    // Off: the first chain fires on its own reference, and the (1,5) chain, which
    // adopted that reference, is not told.  The step stays on every one of its
    // non-reference DDs.
    EXPECT_TRUE(hasKey(keys_off, "G01_L1C_L2W"));
    EXPECT_FALSE(hasKey(keys_off, "G01_L1C_L5Q"));
    EXPECT_NEAR(fan_off, -c1Of(kF1, kF5) * 0.2, 5e-3);
    EXPECT_EQ(c15_off.opportunities, 0) << "no arm ran on the adopting chain";

    // On: the verdict is inherited, the (1,5) reference arc is reset with this
    // chain's key, and the rebase by this chain's own median removes the fan-out
    // from the other arcs.
    EXPECT_TRUE(hasKey(keys_on, "G01_L1C_L2W")) << "the first chain is unchanged";
    EXPECT_TRUE(hasKey(keys_on, "G01_L1C_L5Q"));
    EXPECT_TRUE(hasKey(keys_on, "G01_L2W_L5Q"));
    EXPECT_NEAR(fan_on, 0.0, 5e-3);
    EXPECT_EQ(c15_on.fires_inherited, 1);
    EXPECT_EQ(c15_on.fires_own, 0) << "an adopted reference is never re-tested";
    EXPECT_EQ(c15_on.candidates, 1) << "(1,5) contains L1C, so its own median moved too";
    EXPECT_EQ(c15_on.fanout_suppressed, 5) << "G02..G06 — every satellite but the reference";

    // The cost of unconditional inheritance: (2,5) does not contain L1C, so its own
    // combination never moved and its candidate counter stays 0, yet it still
    // resets the reference arc.  The run-end gap (inherited fires > candidates)
    // counts these resets.
    EXPECT_EQ(c25_on.fires_inherited, 1);
    EXPECT_EQ(c25_on.candidates, 0);
    EXPECT_EQ(c25_on.fanout_suppressed, 0) << "nothing to de-implicate there";
}

// A first chain that does not fire hands down nothing, and the additional chains
// behave as with the knob off.  An adopted reference is never tested on its own
// chain, so no fire means no action, not a local test instead.
TEST(IfTdRefArmAll, NoFirstChainFireMeansNoInheritance) {
    FanHarness h(/*all_pairs=*/true, /*ref_gate=*/true, /*ref_arm=*/true);
    const auto elev  = sixSatElevations();
    const auto bands = adoptedBands();
    OmcState omc = sixSatOmc();

    for (std::size_t e = 0; e < 4; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());

    // A step on a non-reference satellite: no chain's reference moved.
    const BandOffset slip = { { "G04", { { "L5Q", -0.4 } } } };
    EXPECT_TRUE(h.step(4, omc, elev, bands, slip).empty());
    EXPECT_EQ(h.chain(kChain15).fires_inherited, 0);
    EXPECT_EQ(h.chain(kChain15).fires_own, 0);
    EXPECT_EQ(h.chain(kChain25).fires_inherited, 0);
    // The per-satellite step is left for the pooled arm downstream, untouched.
    EXPECT_NEAR(h.dd("G04", "L1C_L5Q"), c2Of(kF1, kF5) * (-0.4), 5e-3);
}

// ===========================================================================
// The second lock: the legacy condition never gates a wide chain
// ===========================================================================

// The loader disarms the knob without `if_phase_td_ref_gate`; this test covers the
// detector's own refusal, which protects a caller that bypasses the loader.
// Without the empirical gate the legacy |median| > 0.5·λ_IF condition would be the
// decision, and on (L2W,L5Q) (noise amplification 16.6 against λ_IF = 0.1247 m)
// noise meets that condition (§14.10.3: 5 351 candidates and 64 fires on a quiet
// null).
TEST(IfTdRefArmAll, ArmedWithoutTheEmpiricalGateTheAdditionalChainsStillDoNotRun) {
    FanHarness h(/*all_pairs=*/true, /*ref_gate=*/false, /*ref_arm=*/true);
    const auto elev  = sixSatElevations();
    const auto bands = ownElectedBands();
    OmcState omc = sixSatOmc();

    for (std::size_t e = 0; e < 3; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());
    const BandOffset slip = { { "G02", { { "L5Q", -0.4 } } } };
    const auto keys = h.step(3, omc, elev, bands, slip);

    EXPECT_TRUE(keys.empty()) << "no arm on an additional chain without the gate";
    EXPECT_EQ(h.chain(kChain15).opportunities, 0);
    EXPECT_EQ(h.chain(kChain25).opportunities, 0);
    EXPECT_EQ(h.chain(kChain15).fires_own, 0);
    EXPECT_EQ(h.chain(kChain25).fires_own, 0);
    // The fan-out is still present, as with the knob off.
    EXPECT_NEAR(h.dd("G03", "L1C_L5Q"), -c2Of(kF1, kF5) * (-0.4), 5e-3);
}

// The same refusal on the adopted path: an inherited verdict on a wide chain is
// also refused without the gate, so neither path can reopen §14.10.3.
TEST(IfTdRefArmAll, InheritanceIsAlsoRefusedWithoutTheEmpiricalGate) {
    FanHarness h(/*all_pairs=*/true, /*ref_gate=*/false, /*ref_arm=*/true);
    const auto elev  = sixSatElevations();
    const auto bands = adoptedBands();
    OmcState omc = sixSatOmc();

    for (std::size_t e = 0; e < 3; ++e) EXPECT_TRUE(h.step(e, omc, elev, bands).empty());
    const auto keys = h.step(3, omc, elev, bands,
                             BandOffset{ { "G01", { { "L1C", 0.2 } } } });
    EXPECT_TRUE(hasKey(keys, "G01_L1C_L2W")) << "the legacy first-chain arm still acts";
    EXPECT_FALSE(hasKey(keys, "G01_L1C_L5Q"));
    EXPECT_FALSE(hasKey(keys, "G01_L2W_L5Q"));
    EXPECT_EQ(h.chain(kChain15).fires_inherited, 0);
}

// ===========================================================================
// Byte identity: the first chain and `pairs = first`
// ===========================================================================

// With one chain per satellite there is no additional chain, so the knob is a
// structural no-op: identical reset keys and identical double differences,
// sample for sample, on the slip epoch and on the quiet ones.
TEST(IfTdRefArmAll, PairsFirstIsUntouched) {
    const auto elev  = sixSatElevations();
    const auto bands = adoptedBands();
    const BandOffset slip = { { "G01", { { "L1C", 0.2 } } } };

    std::vector<std::vector<std::string>> keys[2];
    std::vector<double> dds[2];
    for (int arm = 0; arm < 2; ++arm) {
        FanHarness h(/*all_pairs=*/false, /*ref_gate=*/true, /*ref_arm=*/arm == 1);
        OmcState omc = sixSatOmc();
        for (std::size_t e = 0; e < 6; ++e) {
            keys[arm].push_back(h.step(e, omc, elev, bands, e >= 3 ? slip : BandOffset{}));
            for (const char* s : { "G01", "G02", "G03", "G04", "G05", "G06" })
                dds[arm].push_back(h.dd(s, "L1C_L2W"));
        }
    }
    EXPECT_EQ(keys[0], keys[1]);
    ASSERT_EQ(dds[0].size(), dds[1].size());
    for (std::size_t i = 0; i < dds[0].size(); ++i)
        EXPECT_PRED_FORMAT2(SameDd, dds[0][i], dds[1][i]) << "sample " << i;
}

// Under `pairs = all` the first chain is also unchanged: it already runs the arm,
// and the knob does not alter the decisions of the default path.  Its reset keys
// and every one of its double differences are compared bit for bit across the knob.
TEST(IfTdRefArmAll, TheFirstChainIsUntouchedUnderPairsAll) {
    const auto elev  = sixSatElevations();
    const auto bands = adoptedBands();
    const BandOffset slip = { { "G01", { { "L1C", 0.2 } } } };

    std::vector<std::vector<std::string>> keys[2];
    std::vector<double> dds[2];
    for (int arm = 0; arm < 2; ++arm) {
        FanHarness h(/*all_pairs=*/true, /*ref_gate=*/true, /*ref_arm=*/arm == 1);
        OmcState omc = sixSatOmc();
        for (std::size_t e = 0; e < 6; ++e) {
            keys[arm].push_back(firstChainKeys(h.step(e, omc, elev, bands,
                                                      e >= 3 ? slip : BandOffset{})));
            for (const char* s : { "G01", "G02", "G03", "G04", "G05", "G06" })
                dds[arm].push_back(h.dd(s, "L1C_L2W"));
        }
    }
    EXPECT_EQ(keys[0], keys[1]);
    ASSERT_EQ(dds[0].size(), dds[1].size());
    for (std::size_t i = 0; i < dds[0].size(); ++i)
        EXPECT_PRED_FORMAT2(SameDd, dds[0][i], dds[1][i]) << "sample " << i;
}

// ===========================================================================
// The loader interlock
// ===========================================================================

TEST(IfTdRefArmAll, LoaderDisarmsWithoutPairsAll) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith(cycleSlipBlock("first", /*ref_gate=*/true, /*ref_arm_all=*/true),
                         cfg, log, "nopairs"));
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.ref_gate);
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.ref_arm_all)
        << "with one chain per satellite there is no ADDITIONAL chain to arm";
}

TEST(IfTdRefArmAll, LoaderDisarmsWithoutTheEmpiricalGate) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith(cycleSlipBlock("all", /*ref_gate=*/false, /*ref_arm_all=*/true),
                         cfg, log, "nogate"));
    EXPECT_EQ(cfg.getNetworkConfig().cycle_slip_if_phase_td_pairs,
              NetworkIfPhaseTdPairs::All);
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.ref_arm_all)
        << "the LEGACY candidate condition must never gate a wide chain (§14.10.3)";
}

TEST(IfTdRefArmAll, LoaderArmsWithBothPrerequisites) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith(cycleSlipBlock("all", /*ref_gate=*/true, /*ref_arm_all=*/true),
                         cfg, log, "armed"));
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.ref_arm_all);
}

TEST(IfTdRefArmAll, LoaderDefaultIsOff) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith(cycleSlipBlock("all", /*ref_gate=*/true, /*ref_arm_all=*/false),
                         cfg, log, "default"));
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.ref_arm_all);

    // Also off when the key is absent from the JSON, not only when written false.
    GnssConfig cfg2; LogManager log2;
    ASSERT_TRUE(loadWith("    \"cycle_slip\": { \"if_phase_td\": true,"
                         " \"if_phase_td_pairs\": \"all\","
                         " \"if_phase_td_ref_gate\": true },\n",
                         cfg2, log2, "absent"));
    EXPECT_FALSE(cfg2.getNetworkConfig().slip_qc.ref_arm_all);
}

}  // namespace GnssProcessor
