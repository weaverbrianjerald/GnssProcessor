/**
 * @file test_signal_integrity.cpp
 * @brief Tests for flicker/gap signal integrity (docs/SPEC_flicker_gap_integrity.md):
 *        the per-line-of-sight band-state classifier, the demotion of unvetted phase,
 *        the regain refloat and the re-vet window.
 *
 * The tests target failures that produce no other symptom.  The motivating field case
 * (G21 at MBAR00UGA, DOY-129 19:22-19:26) is a semi-codeless L2W loop slipping ~6 and
 * ~4 cycles with no receiver flag, in windows where MW (needs Welford history), IF-TD
 * (needs three consecutive epochs) and GF (not used at 30 s by policy) are all blind.
 * No detector observes the event; the card is bookkeeping over presence, and
 * bookkeeping that does nothing looks the same as bookkeeping that works.  Each case
 * below pins a decision that has no other observable:
 *
 *   1. a band that vanishes while the line of sight stays present is a flicker, and its
 *      return (FlickerRegain) refloats every band of that LOS, not only the one that
 *      flickered, because across a loss of lock every band's cycle count is arbitrary;
 *   2. the demoted window runs from the transition epoch through the epoch that
 *      completes the vet (K = 3 gives four demoted epochs), so the vet ends when MW and
 *      IF-TD are live again rather than one epoch before;
 *   3. code rows are never touched: they are slip-immune and they re-anchor the float,
 *      so demoting them would defeat the recovery the demotion protects;
 *   4. a band gone longer than the flicker window leaves the reference set; otherwise a
 *      permanently single-frequency arc would stay demoted for the rest of its life
 *      (the standing single-frequency policy is deferred, spec section 7);
 *   5. a re-entry after a real outage is asserted with a counter and not acted on, and
 *      the census separates an ever-dual arc running single-frequency (the audit
 *      answer) from one that was born single-frequency;
 *   6. off is structurally inert: no band map, no sigma map, no counter, and an
 *      observation set the seam has not touched.
 *
 * The classifier is called directly as a free function: it reads `modeled[i]`,
 * `present[]` and the config.  Real StateManagers are used only because the refloat
 * path marks pending-reseed flags and the WITHHOLD path prunes phase-bias states.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/assembly/SignalIntegrity.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"
#include "support/NetworkTestRun.hpp"   // the run and epoch the seam's epoch form takes

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr std::size_t kPppCoreSize = 5;
constexpr const char* kL1 = "L1C";
constexpr const char* kL2 = "L2W";
constexpr const char* kC1 = "C1C";
constexpr const char* kC2 = "C2W";

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("siginteg_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config, with @p net_extra spliced into the
/// network block.  `carry_invariant` is on in every case so the INC-3 funnel feed is
/// live; without it the loader warns and the refloat degrades to a counter, which is
/// a separate (separately asserted) behaviour.
std::string networkConfig(const std::string& net_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          // carry_invariant is the funnel feed INC-3 rides, and the loader only
          // admits it on the eq34 chain datum (the DD cycle basis is defined there).
          "    \"phase_datum_eq34\": true,\n"
          "    \"carry_invariant\": true,\n"
        + net_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load a config through the real loader (so every INERT guard and clamp runs) and
/// return the owned GnssConfig.
std::unique_ptr<GnssConfig> loadCfg(LogManager& log, const std::string& tag,
                                    const std::string& net_extra) {
    auto cfg = std::make_unique<GnssConfig>();
    const fs::path p = writeTempConfig(tag, networkConfig(net_extra));
    const bool ok = cfg->loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    EXPECT_TRUE(ok) << "loadFromJson failed for tag " << tag;
    return cfg;
}

/// Arm the block.  @p scale <= 1 with @p refloat false is the spec's census-only leg.
std::string armed(double scale = 10.0, int revet = 3, int flicker = 5,
                  bool refloat = true, bool withhold = false,
                  bool resume_vet = true) {
    std::string s = "    \"signal_integrity\": {\n";
    s += "      \"census\": true,\n";
    s += "      \"unvetted_sigma_scale\": " + std::to_string(scale) + ",\n";
    s += "      \"revet_ep\": " + std::to_string(revet) + ",\n";
    s += "      \"flicker_max_ep\": " + std::to_string(flicker) + ",\n";
    s += std::string("      \"refloat_on_regain\": ") + (refloat ? "true" : "false") + ",\n";
    s += std::string("      \"resume_warmup_vet\": ") + (resume_vet ? "true" : "false") + ",\n";
    s += std::string("      \"withhold_instead\": ") + (withhold ? "true" : "false") + "\n";
    s += "    },\n";
    return s;
}

GnssTime epochAt(double tow) {
    GnssTime t;
    t.week = 2314;
    t.tow  = tow;
    return t;
}

/// One satellite's observation with exactly the listed phase bands present.  Code is
/// present on both bands in every scenario; it is the control for "code rows
/// untouched".
PppObservation obsWith(const std::string& sat,
                       const std::vector<std::string>& phase_bands,
                       int lli_bits = 0) {
    PppObservation o;
    o.satellite = sat;
    o.elevation = 45.0;
    o.observed[kC1] = 2.0e7;      // code is always present on both bands
    o.observed[kC2] = 2.0e7;
    o.raw_observed[kC1] = 2.0e7;
    o.raw_observed[kC2] = 2.0e7;
    for (const auto& b : phase_bands) {
        o.observed[b] = 1.0e8;
        o.raw_observed[b] = 1.0e8;
        if (lli_bits) o.lli_flags[b] = lli_bits;
    }
    return o;
}

/// One station with a real StateManager, plus the per-epoch scaffolding the seam takes.
/// The run (its context, stations and slip-QC epoch index) is the fixture's; the funnel and
/// the modelled rows are carried across steps here and handed to each epoch.
struct Rig {
    std::unique_ptr<NetworkTestRun> fx;
    std::vector<NetStation>& stations;
    std::vector<bool> present{true};
    std::vector<std::vector<PppObservation>> modeled{1};
    std::set<std::pair<int, std::string>> slipped, slipped_raw;
    long long ep = 0;

    Rig(LogManager& log, const GnssConfig& cfg)
        : fx(std::make_unique<NetworkTestRun>(cfg)), stations(fx->setup().stations) {
        stations.resize(1);
        stations[0].name = "AAAA";
        stations[0].sm = std::make_unique<StateManager>(kPppCoreSize, log, cfg);
    }

    /// Run one epoch with the given per-satellite band sets.
    void step(NetworkRunContext& ctx, const GnssConfig& cfg,
              const std::vector<PppObservation>& obs) {
        (void)ctx; (void)cfg;   // the fixture's (the test binds ctx to it); carry_invariant comes from cfg
        modeled[0] = obs;
        NetworkEpoch& e = fx->newEpoch(epochAt(1000.0 + 30.0 * static_cast<double>(ep)));
        fx->run().engines.slip_qc.epoch_idx = ep;
        e.clock.carry_warm = true;
        e.pass_a.present = present;
        e.pass_a.modeled = modeled;
        e.funnel.carry_slipped_ambs = slipped;
        e.funnel.carry_slipped_raw = slipped_raw;
        networkSignalIntegrity(fx->run(), e, e.pass_a, e.funnel);
        modeled = e.pass_a.modeled;
        slipped = e.funnel.carry_slipped_ambs;
        slipped_raw = e.funnel.carry_slipped_raw;
        ++ep;
    }

    /// This epoch's published phase sigma scale for @p sat (1.0 = full weight).
    double scaleFor(const std::string& sat) const {
        const auto it = stations[0].phase_sigma_scale.find(sat);
        return it == stations[0].phase_sigma_scale.end() ? 1.0 : it->second;
    }
    bool funnelHas(const std::string& sat, const std::string& sig) const {
        return slipped.count({0, StateKeys::makeAmbiguityKey(sat, sig)}) != 0;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. The measured shape: a band flickers out and back while the LOS never leaves
// ---------------------------------------------------------------------------
//
// The G21 at MBAR case: L2W gone for one epoch while L1C and a valid S1C stay present.
// No producer-registered gap exists for it, so nothing downstream treats it as a
// recovery; MW and GF both need the same dual pair on both sides of a step, and the
// missing band breaks that.  The regain therefore refloats both bands: in the field
// case the following epoch carried a +4-cycle L2W slip that no detector flagged, and
// an L1 LLI at the same re-entry shows that the L1 cycle count moved too.  Refloating
// only the flickered band would treat absence of evidence as evidence of continuity.
TEST(NetworkSignalIntegrity, FlickeredBandReturnsAndRefloatsEveryBandOfTheLineOfSight) {
    LogManager lm;
    auto cfg = loadCfg(lm, "flicker", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Five full-band epochs (1 + K + 1): the birth epoch is a transition and
    // contributes no vet sample, three vet epochs complete the vet, and one more
    // epoch follows, so the arc is converged.  A never-converged arc is exempt from
    // demotion (case 9), so these epochs carry full weight throughout.
    for (int k = 0; k < 5; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    ASSERT_DOUBLE_EQ(1.0, r.scaleFor("G21")) << "a settled dual arc must carry full phase weight";
    ASSERT_TRUE(r.slipped.empty()) << "a quiet arc must not touch the slip funnel";

    // L2W drops out; L1C and both codes stay.
    r.step(ctx, *cfg, {obsWith("G21", {kL1})});
    EXPECT_GT(r.scaleFor("G21"), 1.0) << "a degraded LOS must not carry full phase weight";
    EXPECT_EQ(1, ctx.counters.sig.band_lost);
    EXPECT_EQ(0, ctx.counters.sig.los_vanished) << "the LOS never left — this is a BAND event";

    // L2W returns inside the flicker window.
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    EXPECT_EQ(1, ctx.counters.sig.band_regain);
    EXPECT_EQ(1, ctx.counters.sig.refloat_regain);
    EXPECT_TRUE(r.funnelHas("G21", kL2)) << "the flickered band must refloat";
    EXPECT_TRUE(r.funnelHas("G21", kL1))
        << "the SURVIVING band must refloat too: across a loss of lock every band's "
           "counter is arbitrary at once, and nothing can bound the sizes";
    // The regain is a raw observable event (a band vanished and came back), so the
    // Iono-2 v3k erase-policy split erases this LOS's GM deviation rather than keeping
    // (D5) a level carried across a loss of lock.
    EXPECT_EQ(r.slipped, r.slipped_raw)
        << "a regain must land in the RAW provenance set, not only in the funnel";
}

// ---------------------------------------------------------------------------
// 2. The vet window ends exactly when monitoring begins
// ---------------------------------------------------------------------------
//
// K = 3 follows from the detectors: MW needs two time-difference samples and IF-TD
// needs three consecutive epochs, so at K = 3 both are live for at least one epoch
// before the phase returns to full weight.  That requires the demoted window to run
// from the transition epoch through the epoch that completes the vet, four epochs
// in all, matching the spec's §6D arithmetic for the 19:25:30 regain ("demoted phase
// through 19:27:00").  Counting the transition epoch as vet sample #1 would end the
// window one epoch early, on the epoch where the field case's +4-cycle slip landed.
TEST(NetworkSignalIntegrity, DemotionSpansTheTransitionEpochThroughVetCompletion) {
    LogManager lm;
    auto cfg = loadCfg(lm, "vet", armed(/*scale=*/10.0, /*revet=*/3));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 5; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    ASSERT_DOUBLE_EQ(1.0, r.scaleFor("G21"));

    r.step(ctx, *cfg, {obsWith("G21", {kL1})});             // band lost   (transition)
    EXPECT_GT(r.scaleFor("G21"), 1.0) << "degraded epoch";
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});        // regain      (transition, vet 0)
    EXPECT_DOUBLE_EQ(10.0, r.scaleFor("G21")) << "the regain epoch itself is demoted";
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});        // vet 0 -> 1
    EXPECT_DOUBLE_EQ(10.0, r.scaleFor("G21"));
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});        // vet 1 -> 2
    EXPECT_DOUBLE_EQ(10.0, r.scaleFor("G21"));
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});        // vet 2 -> 3 (completes)
    EXPECT_DOUBLE_EQ(10.0, r.scaleFor("G21"))
        << "the epoch that COMPLETES the vet is still demoted — full weight resumes after it";
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    EXPECT_DOUBLE_EQ(1.0, r.scaleFor("G21")) << "vetted: MW and IF-TD are both live by now";
    EXPECT_EQ(2, ctx.counters.sig.vet_completions) << "one at birth, one after the regain";
}

// ---------------------------------------------------------------------------
// 3. Only phase is demoted; code is never touched
// ---------------------------------------------------------------------------
//
// The demotion keeps a phase that cannot be monitored from corrupting the shared
// clock, bias and iono states at full weight.  Code is slip-immune, and it is the
// only input that re-anchors the float to a bounded error (tens of cycles) after a
// recovery; deweighting it would remove the recovery mechanism along with the hazard.
// The map is keyed by satellite and the phase/code split happens at the fold on the
// row's own signal type, so this test pins the contract the fold depends on: the
// published key is the bare satellite id, never a signal-qualified one.
TEST(NetworkSignalIntegrity, DemotionMapIsKeyedBySatelliteSoOnlyPhaseRowsCanMatch) {
    LogManager lm;
    auto cfg = loadCfg(lm, "codesafe", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 5; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    r.step(ctx, *cfg, {obsWith("G21", {kL1})});        // converged, then degraded
    const auto& m = r.stations[0].phase_sigma_scale;
    ASSERT_EQ(1u, m.size());
    EXPECT_EQ("G21", m.begin()->first)
        << "the key must be the bare satellite id — the fold selects phase rows by "
           "em.signal_types[r][0] == 'L', so a signal-qualified key would match nothing";
    EXPECT_DOUBLE_EQ(10.0, m.begin()->second);
    // The observations themselves are untouched under the default (deweight)
    // disposition: both codes and the surviving phase are still there for PASS B.
    EXPECT_EQ(1u, r.modeled[0][0].observed.count(kC1));
    EXPECT_EQ(1u, r.modeled[0][0].observed.count(kC2));
    EXPECT_EQ(1u, r.modeled[0][0].observed.count(kL1));
}

// ---------------------------------------------------------------------------
// 4. A band gone longer than the flicker window leaves the reference set
// ---------------------------------------------------------------------------
//
// Otherwise an arc that permanently lost a band would stay Degraded, and so demoted,
// for the rest of its life.  That is the standing single-frequency policy question
// the spec defers until after the census (§7), and this card does not answer it by
// default.  The arc instead re-vets on its reduced set and returns to full weight,
// while `ever_dual` keeps it visible in the audit census.
TEST(NetworkSignalIntegrity, BandGoneBeyondTheFlickerWindowReducesTheReferenceSet) {
    LogManager lm;
    auto cfg = loadCfg(lm, "perm", armed(/*scale=*/10.0, /*revet=*/2, /*flicker=*/2));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 4; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});  // 1 + K + 1, K = 2
    ASSERT_DOUBLE_EQ(1.0, r.scaleFor("G21"));

    // L2W gone for good.  The window is inclusive (a band is still a flicker
    // candidate while `now - missing_since <= flicker_max_ep`), so with
    // flicker_max_ep = 2 the reference set is reduced on the fourth missing epoch.
    for (int k = 0; k < 4; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1})});
    EXPECT_EQ(1, ctx.counters.sig.band_dropped_perm);
    EXPECT_EQ(0, ctx.counters.sig.band_regain);

    // Now it re-vets on L1C alone and returns to full weight (the reduction epoch is
    // itself a transition, so 1 + K more epochs).
    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1})});
    EXPECT_DOUBLE_EQ(1.0, r.scaleFor("G21"))
        << "a genuinely single-frequency arc must re-stabilise, not stay demoted forever";
    // …but the census still records that it was dual.  This counter is the spec's
    // INC-0 audit answer: an ever-dual arc contributing single-frequency.
    EXPECT_GT(ctx.counters.sig.sf_continuation_epochs, 0);
    EXPECT_EQ(0, ctx.counters.sig.standing_sf_arcs)
        << "this arc was born dual — it must not be counted in the born-single class";
}

// ---------------------------------------------------------------------------
// 5. A real outage is a re-entry: asserted, bucketed, and not acted on
// ---------------------------------------------------------------------------
//
// The spec's premise is that a true re-entry already refloats through the funnel
// (Card G measured "0 bridged"), so this card's instruction is "no change, but assert
// it with a counter".  Acting anyway would push ~1.7 extra funnel entries per epoch
// into a channel the Iono-2 v3k erase policy is tuned on, so the card measures the
// premise and leaves the decision to a later card.  The gap bucketing makes the
// measurement comparable to Card G's own re-entry totals.
TEST(NetworkSignalIntegrity, ReentryIsAssertedAndBucketedButNeverRefloated) {
    LogManager lm;
    auto cfg = loadCfg(lm, "reentry", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 4; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    ASSERT_TRUE(r.slipped.empty());

    r.step(ctx, *cfg, {});                                  // the whole LOS is gone
    EXPECT_EQ(1, ctx.counters.sig.los_vanished);
    EXPECT_EQ(1, ctx.counters.sig.los_vanished_dual)
        << "it was DUAL when it left — the class the INC-0 audit weighs";

    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2}, /*lli_bits=*/1)});
    EXPECT_EQ(1, ctx.counters.sig.reentries);
    EXPECT_EQ(1, ctx.counters.sig.reentry_gap_1);
    EXPECT_EQ(1, ctx.counters.sig.lli_at_reentry);
    EXPECT_TRUE(r.slipped.empty())
        << "spec section 2 INC-3a: a true re-entry is ASSERTED, not acted on";
    EXPECT_EQ(1, ctx.counters.sig.reentry_unrefloated)
        << "nothing had marked it, and that is the number the field report carries";
    EXPECT_GT(r.scaleFor("G21"), 1.0) << "a re-entered LOS is unvetted like any other";
}

// ---------------------------------------------------------------------------
// 6. A born-single arc is counted apart from a degraded one
// ---------------------------------------------------------------------------
//
// The BSHM/G07 class: an arc never observed dual.  Folding it into the degraded count
// would inflate the audit answer with arcs that never degraded, and the two have
// opposite policy consequences: a degraded arc is a hazard this card demotes, while a
// standing single-frequency arc is a deferred policy question (spec §7) and must not
// be demoted permanently by default.
TEST(NetworkSignalIntegrity, StandingSingleFrequencyArcsAreCountedSeparatelyAndStabilise) {
    LogManager lm;
    auto cfg = loadCfg(lm, "standingsf", armed(/*scale=*/10.0, /*revet=*/2));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 4; ++k) r.step(ctx, *cfg, {obsWith("G07", {kL1})});
    EXPECT_EQ(1, ctx.counters.sig.standing_sf_arcs);
    EXPECT_EQ(4, ctx.counters.sig.standing_sf_epochs);
    EXPECT_EQ(0, ctx.counters.sig.sf_continuation_epochs)
        << "never dual ⇒ never a DEGRADED continuation";
    EXPECT_DOUBLE_EQ(1.0, r.scaleFor("G07"))
        << "a born-single arc stabilises on its own reference set — permanent demotion "
           "is a deferred policy question, not this card's default";
}

// ---------------------------------------------------------------------------
// 7. The WITHHOLD disposition removes phase observables, never row weights
// ---------------------------------------------------------------------------
//
// A zero-weight phase row would still contribute its live H column to the local
// block, so its ambiguity column would enter A_r carrying no information and trip the
// per-station positive-definiteness guard.  That would turn a per-arc demotion into a
// whole-station exclusion, the hard drop the fallback-over-drop rule forbids.
// Removing the observable instead makes the column all-zero, which the existing
// dead-column scan drops.  Code observables stay untouched.
TEST(NetworkSignalIntegrity, WithholdRemovesPhaseObservablesAndKeepsEveryCodeObservable) {
    LogManager lm;
    auto cfg = loadCfg(lm, "withhold",
                       armed(/*scale=*/10.0, /*revet=*/3, /*flicker=*/5,
                             /*refloat=*/true, /*withhold=*/true));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 5; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    r.step(ctx, *cfg, {obsWith("G21", {kL1})});        // converged, then degraded
    const auto& o = r.modeled[0][0];
    EXPECT_EQ(0u, o.observed.count(kL1));
    EXPECT_EQ(1u, o.observed.count(kC1)) << "code rows are what re-anchor the float";
    EXPECT_EQ(1u, o.observed.count(kC2));
    EXPECT_EQ(1u, o.raw_observed.count(kL1))
        << "raw_observed belongs to the slip detectors and must keep the true tracking state";
    EXPECT_EQ(1, ctx.counters.sig.withheld_rows);
    EXPECT_TRUE(r.stations[0].phase_sigma_scale.empty())
        << "withhold and deweight are alternatives — never both on one LOS";
}

// ---------------------------------------------------------------------------
// 8. The census-only leg classifies and counts, and changes nothing at all
// ---------------------------------------------------------------------------
//
// Spec §6C runs an INC-0 leg before any action is armed.  With no separate "demote"
// knob, that leg is expressed as census + unvetted_sigma_scale <= 1 + no refloat, and
// this test checks that the combination gives full telemetry with no mechanism active.
TEST(NetworkSignalIntegrity, CensusOnlyLegCountsWithoutChangingAnyWeightOrFunnel) {
    LogManager lm;
    auto cfg = loadCfg(lm, "censusonly",
                       armed(/*scale=*/1.0, /*revet=*/3, /*flicker=*/5, /*refloat=*/false));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 4; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    r.step(ctx, *cfg, {obsWith("G21", {kL1})});          // band lost
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});     // regain

    EXPECT_EQ(6, ctx.counters.sig.epochs);
    EXPECT_EQ(6, ctx.counters.sig.los_epochs);
    EXPECT_EQ(1, ctx.counters.sig.band_lost);
    EXPECT_EQ(1, ctx.counters.sig.band_regain) << "the census still classifies the flicker";
    EXPECT_EQ(0, ctx.counters.sig.refloat_regain) << "…and does not act on it";
    EXPECT_TRUE(r.slipped.empty());
    EXPECT_TRUE(r.stations[0].phase_sigma_scale.empty());
    EXPECT_EQ(0, ctx.counters.sig.demoted_los_epochs);
}

// ---------------------------------------------------------------------------
// 9. A never-converged arc is exempt, which also exempts cold start
// ---------------------------------------------------------------------------
//
// A phase row's potential for damage is its weight times the tightness of the state
// it updates.  A new arc has none: its ambiguity is diffuse (σ 1000 m) and it has no
// accumulated carry chord, so its own priors already act as the demotion and "slip at
// epoch 1" is not a defined event.  Vetting it would cost convergence and gain
// nothing.  Expressing the rule as "the arc has never completed a re-vet" exempts cold
// start without a special case (at cold start every arc is un-converged), and also
// exempts a satellite rising mid-run.  The census still counts such arcs, so the
// exemption stays visible.
TEST(NetworkSignalIntegrity, NeverConvergedArcIsExemptFromDemotionButStillCensused) {
    LogManager lm;
    auto cfg = loadCfg(lm, "birth", armed(/*scale=*/10.0, /*revet=*/3));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // The whole birth window: transition epoch + K vet epochs, none demoted.
    for (int k = 0; k < 4; ++k) {
        r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
        EXPECT_DOUBLE_EQ(1.0, r.scaleFor("G21"))
            << "epoch " << k << ": a never-converged arc must not be demoted";
    }
    EXPECT_EQ(4, ctx.counters.sig.unvetted_los_epochs) << "…but it IS counted as unvetted";
    EXPECT_EQ(4, ctx.counters.sig.birth_exempt_los_epochs) << "…and the exemption is visible";
    EXPECT_EQ(0, ctx.counters.sig.demoted_los_epochs);
    EXPECT_EQ(1, ctx.counters.sig.vet_completions);

    // Now it has converged, so it has something to protect: a band loss demotes.
    r.step(ctx, *cfg, {obsWith("G21", {kL1})});
    EXPECT_DOUBLE_EQ(10.0, r.scaleFor("G21"))
        << "once the arc has a warm carry, the same transition DOES demote";
    EXPECT_EQ(1, ctx.counters.sig.demoted_los_epochs);
}

// ---------------------------------------------------------------------------
// 10. A warm resume vets every arc for the re-vet window
// ---------------------------------------------------------------------------
//
// The per-arc rule cannot see this case: a resumed run starts with an empty band map,
// so every arc looks like a first sighting and would be exempted by case 9, although
// the snapshot has restored tight carries.  No detector state is serialized (neither
// MW Welford counts nor IF-TD arc history), so in the first epochs every detector in
// the network is blind while the state is warm: the flicker hazard at network scale.
// The override is therefore carried independently of the arcs and expires on its own.
TEST(NetworkSignalIntegrity, WarmResumeVetsEveryArcForTheRevetWindowThenExpires) {
    LogManager lm;
    auto cfg = loadCfg(lm, "resume", armed(/*scale=*/10.0, /*revet=*/3));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();
    ASSERT_TRUE(ctx.knobs.sig_resume_warmup);

    // What networkTryWarmResume latches on a successful resume.
    ctx.carry.si_resume_warmup_ep = ctx.knobs.sig_revet_ep;

    for (int k = 0; k < 3; ++k) {
        r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
        EXPECT_DOUBLE_EQ(10.0, r.scaleFor("G21"))
            << "resume epoch " << k << ": tight carries, no detector history";
    }
    EXPECT_EQ(3, ctx.counters.sig.resume_warmup_los_epochs);
    EXPECT_EQ(0, ctx.carry.si_resume_warmup_ep) << "the override expires on its own";

    // Past the window the arc falls back to the ordinary birth exemption: it has not
    // yet converged in this run, so it is not demoted.
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    EXPECT_DOUBLE_EQ(1.0, r.scaleFor("G21"));
}

// ---------------------------------------------------------------------------
// 11. Disarmed is structurally inert
// ---------------------------------------------------------------------------
//
// Invariant #6 requires byte-identity by structure, not by a numerically neutral
// value: with the card off no band map is allocated, no sigma map is published, no
// counter moves and no observable is touched, so the assembly reads the pre-card
// `em.R_v.cwiseInverse()` with no additional floating-point operations.  The action
// knobs are set on purpose: the loader disarms them when the census master is off,
// rather than letting them act behind a classifier that never ran.
TEST(NetworkSignalIntegrity, DisarmedIsStructurallyInertEvenWithActionKnobsSet) {
    LogManager lm;
    auto cfg = loadCfg(lm, "off",
                       "    \"signal_integrity\": { \"census\": false, "
                       "\"unvetted_sigma_scale\": 10.0, \"refloat_on_regain\": true },\n");
    ASSERT_FALSE(cfg->getNetworkConfig().signal_integrity.census);
    EXPECT_DOUBLE_EQ(1.0, cfg->getNetworkConfig().signal_integrity.unvetted_sigma_scale)
        << "the INERT guard must disarm the action, not merely ignore it";
    EXPECT_FALSE(cfg->getNetworkConfig().signal_integrity.refloat_on_regain);

    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();
    ASSERT_FALSE(ctx.knobs.sig_integrity_on);

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});
    r.step(ctx, *cfg, {obsWith("G21", {kL1})});
    r.step(ctx, *cfg, {obsWith("G21", {kL1, kL2})});

    EXPECT_TRUE((net::present(ctx.entities.los, &net::LosState::si_arc) == 0)) << "no band history allocated";
    EXPECT_TRUE(r.stations[0].phase_sigma_scale.empty()) << "no sigma map published";
    EXPECT_EQ(0, r.stations[0].si_rows_demoted);
    EXPECT_EQ(0, ctx.counters.sig.epochs) << "not one counter moved";
    EXPECT_EQ(0, ctx.counters.sig.los_epochs);
    EXPECT_TRUE(r.slipped.empty());
    EXPECT_TRUE(r.slipped_raw.empty());
    EXPECT_EQ(1u, r.modeled[0][0].observed.count(kL1)) << "no observable touched";
    EXPECT_EQ(1u, r.modeled[0][0].observed.count(kL2));
}
