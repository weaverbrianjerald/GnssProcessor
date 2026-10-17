/**
 * @file test_measurement_admission.cpp
 * @brief Product-referenced measurement admission (docs/SPEC_measurement_admission.md):
 *        the corrupt-channel screen.  Covers the statistic, the admission decision
 *        at birth and at a warm-resume restore, the mid-arc onset and heal, and the
 *        station guard on an untrusted median.
 *
 * The field defect the screen targets is BSHM00ISR on DOY-129: G07 C1C+L1C reported
 * at **+198.2 km** and G16 at **+99.9 km** against SP3+CLK, with the offset on both
 * the code and the phase of the channel.  Every differential check (TD smoothness,
 * phase-minus-code, GF, MW) therefore passes by construction, and on a single-band
 * line of sight the free slant state absorbs the error exactly, so no residual test
 * can detect it.  Only an absolute, product-referenced comparison detects this
 * class, and only one with no filter state in it can detect it at birth.
 *
 * Each case below pins a decision that has no other observable:
 *
 *   1. the corrupt channel is rejected at the epoch its arc would be born, while
 *      its healthy single-band sibling at the same station (G21@BSHM) is admitted;
 *      this control makes "single-band is the amplifier, not the cause" testable;
 *   2. a station-common 1 ms offset on every satellite produces zero rejections:
 *      at this precision the median is the receiver clock, and a screen that fired
 *      on clock steps would generate false positives;
 *   3. below the median floor the station is skipped and counted, never screened
 *      off a two-value "median";
 *   4. a mid-arc onset withholds the band and refloats the arc: a hole in a stiff
 *      time-linked carry with no re-tap violates standing invariant #1;
 *   5. a heal readmits only after N consecutive clean epochs and re-taps, so it
 *      cannot restore the old state;
 *   6. a restored corrupt arc is rejected on the first post-resume epoch and is not
 *      called a birth.  The prefit census's seed-birth tripwire cannot see this
 *      case, and it is why a birth (nothing seeded) and a restore (a stiff carried
 *      chord) get different responses;
 *   7. when more than max_frac of a station's channels fail, the median is what is
 *      untrustworthy: the station is held and no per-channel claim is made;
 *   8. when disarmed the seam is structurally inert: no record map, no counter, no
 *      observable touched.
 *
 * The seam is exercised directly as a free function.  It reads `observed` /
 * `modeled` and `present[]`, and needs a real StateManager only because the
 * refloat path marks pending-reseed flags and the withhold prunes shared states.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/MeasurementAdmission.hpp"
#include "network/driver/RunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"
#include "support/NetworkTestRun.hpp"   // the run and epoch the seam's epoch form takes

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// The admission records of every station (StationState::adm_arc).
std::size_t admArcs(const NetworkRunContext& ctx) {
    std::size_t n = 0;
    const net::StationTable& t = ctx.entities.stations;
    for (auto it = t.begin(); it != t.end(); ++it) n += it->second.adm_arc.size();
    return n;
}

/// The ADMISSION event rate-limit clocks of every station (StationState::adm_event_last).
std::size_t admEvents(const NetworkRunContext& ctx) {
    std::size_t n = 0;
    const net::StationTable& t = ctx.entities.stations;
    for (auto it = t.begin(); it != t.end(); ++it) n += it->second.adm_event_last.size();
    return n;
}

constexpr std::size_t kPppCoreSize = 5;
constexpr const char* kL1 = "L1C";
constexpr const char* kL2 = "L2W";
constexpr const char* kC1 = "C1C";
constexpr const char* kC2 = "C2W";

/// A healthy station's raw term: the receiver-clock level the median removes.
constexpr double kClockLevel = 1.7e5;      // ~0.57 ms, the CAS100ATA class
/// A healthy per-satellite spread about it (tropo-wet + slant iono + multipath).
constexpr double kHealthySpread = 12.0;
/// The measured BSHM/G07 displacement.
constexpr double kCorruptG07 = 1.982e5;

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("measadm_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config with @p net_extra spliced into the
/// network block.  `phase_datum_eq34` and `carry_invariant` are on in every case:
/// the loader admits carry_invariant only on the eq34 chain datum, and without it
/// the funnel feed is inert, so a refloat assertion could pass without testing
/// anything.
std::string networkConfig(const std::string& net_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
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

std::unique_ptr<GnssConfig> loadCfg(LogManager& log, const std::string& tag,
                                    const std::string& net_extra) {
    auto cfg = std::make_unique<GnssConfig>();
    const fs::path p = writeTempConfig(tag, networkConfig(net_extra));
    const bool ok = cfg->loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    EXPECT_TRUE(ok) << "loadFromJson failed for tag " << tag;
    return cfg;
}

/// Arm the screen.  @p screen false with @p trace true is the spec's measure-only leg.
std::string armed(bool screen = true, double bound = 1000.0, int min_sats = 4,
                  int heal_ep = 5, double max_frac = 0.5, bool trace = true) {
    std::string s;
    s += std::string("    \"adm_screen\": ") + (screen ? "true" : "false") + ",\n";
    s += "    \"adm_bound_m\": " + std::to_string(bound) + ",\n";
    s += "    \"adm_min_sats\": " + std::to_string(min_sats) + ",\n";
    s += "    \"adm_heal_ep\": " + std::to_string(heal_ep) + ",\n";
    s += "    \"adm_max_frac\": " + std::to_string(max_frac) + ",\n";
    s += std::string("    \"adm_trace\": ") + (trace ? "true" : "false") + ",\n";
    return s;
}

/// Arm the re-entry vet on top of @ref armed (INC-4).
///
/// @p bound_scale defaults to **1.0 here, not to the shipped 0.2**.  Most vet cases
/// isolate one mechanism (the relabel, the gap threshold, the census trigger, the
/// hold posture), and a 200 m vet bound would also fire on the corrupt fixtures and
/// let those cases pass for the wrong reason.  The cases that exercise the bound set
/// it explicitly, which keeps the mechanism under test visible at every call site.
std::string revetArmed(int gap_ep = 1, int vet_ep = 8, bool use_census = true,
                       bool hold = false, int heal_ep = 5,
                       double bound_scale = 1.0) {
    std::string s = armed(/*screen=*/true, /*bound=*/1000.0, /*min_sats=*/4, heal_ep);
    s += "    \"adm_revet\": true,\n";
    s += "    \"adm_revet_gap_ep\": " + std::to_string(gap_ep) + ",\n";
    s += "    \"adm_revet_ep\": " + std::to_string(vet_ep) + ",\n";
    s += "    \"adm_revet_bound_scale\": " + std::to_string(bound_scale) + ",\n";
    s += std::string("    \"adm_revet_census\": ") + (use_census ? "true" : "false") + ",\n";
    s += std::string("    \"adm_revet_hold\": ") + (hold ? "true" : "false") + ",\n";
    return s;
}

GnssTime epochAt(double tow) {
    GnssTime t;
    t.week = 2314;
    t.tow  = tow;
    return t;
}

/// One satellite's observation.  @p offsets maps an observable key to the raw term
/// d = observed - modeled it should carry; the modeled side is a fixed plausible
/// range, so the test pins the difference, which is the entire statistic.
PppObservation obsWith(const std::string& sat,
                       const std::vector<std::string>& keys, double d_term) {
    PppObservation o;
    o.satellite = sat;
    o.elevation = 45.0;
    o.range     = 2.2e7;
    for (const auto& k : keys) {
        const double modeled = (k[0] == 'L') ? 2.2e7 + 137.0 : 2.2e7;
        o.modeled[k]  = modeled;
        o.observed[k] = modeled + d_term;
        o.raw_observed[k] = o.observed[k];
    }
    return o;
}

/// A healthy dual-band satellite: the station clock level plus a per-satellite
/// spread well inside the bound.
PppObservation healthy(const std::string& sat, double spread,
                       double clock = kClockLevel) {
    return obsWith(sat, {kC1, kC2, kL1, kL2}, clock + spread);
}

/// One station with a real StateManager plus the per-epoch scaffolding the seam
/// takes.  Two stations so the second can stay absent (present[] gating).
/// The run (its context, stations and slip-QC epoch index) is the fixture's; the funnel and the
/// modelled rows are carried across steps here and handed to each epoch.
struct Rig {
    std::unique_ptr<NetworkTestRun> fx;
    std::vector<NetStation>& stations;
    std::vector<bool> present{true, false};
    std::vector<std::vector<PppObservation>> modeled{2};
    std::set<std::pair<int, std::string>> slipped, slipped_raw;
    long long ep = 0;

    Rig(LogManager& log, const GnssConfig& cfg)
        : fx(std::make_unique<NetworkTestRun>(cfg)), stations(fx->setup().stations) {
        stations.resize(2);
        stations[0].name = "AAAA";
        stations[0].sm = std::make_unique<StateManager>(kPppCoreSize, log, cfg);
        stations[1].name = "BBBB";
        stations[1].sm = std::make_unique<StateManager>(kPppCoreSize, log, cfg);
    }

    /// Run one epoch with the given station-0 observation set.
    ///
    /// `n_loop_epochs` is advanced first, as the driver does before either this seam
    /// or the prefit census runs.  Both read that counter, and the census-freshness
    /// test is `flag_ep == n_loop_epochs - 1` (INC-4), so a rig that left it at zero
    /// would make every flag look fresh and leave the staleness gate untested.
    void step(NetworkRunContext& ctx, const GnssConfig& cfg,
              const std::vector<PppObservation>& obs) {
        (void)cfg;   // the fixture's; carry_invariant comes from it (ctx is the fixture's context)
        ++ctx.counters.n_loop_epochs;
        modeled[0] = obs;
        NetworkEpoch& e = fx->newEpoch(epochAt(1000.0 + 30.0 * static_cast<double>(ep)));
        fx->run().engines.slip_qc.epoch_idx = ep;
        e.clock.carry_warm = true;
        e.pass_a.present = present;
        e.pass_a.modeled = modeled;
        e.funnel.carry_slipped_ambs = slipped;
        e.funnel.carry_slipped_raw = slipped_raw;
        networkMeasurementAdmission(fx->run(), e, e.pass_a, e.funnel, e.events);
        modeled = e.pass_a.modeled;
        slipped = e.funnel.carry_slipped_ambs;
        slipped_raw = e.funnel.carry_slipped_raw;
        ++ep;
    }

    /// Publish a prefit-census `obs_code` flag for @p sat as the census would have
    /// written it at the end of the previous epoch; that is the only stamp the vet
    /// honours (INC-4).  Call immediately before the `step` that should read it.
    void censusFlag(NetworkRunContext& ctx, const std::string& sat) const {
        net::assignLosFlags(ctx.entities, &net::LosState::prefit_code_flag, {{0, sat}});
        ctx.carry.prefit_code_flag_ep =
            static_cast<long long>(ctx.counters.n_loop_epochs);
    }

    /// Does station 0's observation for @p sat still carry @p key?
    bool has(const std::string& sat, const std::string& key) const {
        for (const auto& o : modeled[0])
            if (o.satellite == sat) return o.observed.count(key) != 0;
        return false;
    }
    bool funnelHas(const std::string& sat, const std::string& sig) const {
        return slipped.count({0, StateKeys::makeAmbiguityKey(sat, sig)}) != 0;
    }
};

/// Eleven healthy satellites plus the BSHM channel shapes: G07 is C1C+L1C only
/// (single-band, the amplifier) and corrupt, and G21 is also C1C+L1C only but
/// healthy (the innocence control).  The corrupt G16 channel is not modelled.
std::vector<PppObservation> bshmEpoch(double g07 = kCorruptG07) {
    std::vector<PppObservation> v;
    const char* healthy_sats[] = {"G01", "G03", "G05", "G08", "G09",
                                  "G11", "G13", "G17", "G22", "G28", "G30"};
    double spread = -12.5;
    for (const char* s : healthy_sats) { v.push_back(healthy(s, spread)); spread += 3.0; }
    v.push_back(obsWith("G07", {kC1, kL1}, kClockLevel + g07));
    v.push_back(obsWith("G21", {kC1, kL1}, kClockLevel - 10.1));   // healthy single-band
    return v;
}

/// The same epoch with @p sat not tracked at all: the satellite has set, or the
/// two-station observability gate erased it.  This is what an arc gap looks like to
/// the admission seam: the channel is not among the observations, so its record is
/// left untouched and `last_ep` stops advancing.
std::vector<PppObservation> bshmEpochWithout(const std::string& sat,
                                             double g07 = kCorruptG07) {
    std::vector<PppObservation> v = bshmEpoch(g07);
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const PppObservation& o) { return o.satellite == sat; }),
            v.end());
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The measured shape: rejected at birth, healthy single-band sibling admitted
// ---------------------------------------------------------------------------
TEST(MeasurementAdmission, CorruptChannelRejectedAtBirthWhileHealthySiblingsAdmit) {
    LogManager lm;
    auto cfg = loadCfg(lm, "birth", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    r.step(ctx, *cfg, bshmEpoch());

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.reject_birth) << "exactly the one corrupt channel must be rejected";
    EXPECT_EQ(0, a.reject_resume) << "a cold first pass is a BIRTH, never a restore";
    EXPECT_EQ(0, a.station_events)
        << "one bad channel in thirteen is far under adm_max_frac — the median stands";

    EXPECT_FALSE(r.has("G07", kC1)) << "the corrupt band's CODE must leave the model set";
    EXPECT_FALSE(r.has("G07", kL1))
        << "and its PHASE with it: the offset rides BOTH observables of the channel, "
           "which is exactly why no differential test sees it";

    // The innocence control.  G21 is single-band too; excluding single-frequency
    // cells as a class would discard a good satellite to catch a bad one.
    EXPECT_TRUE(r.has("G21", kC1)) << "a healthy single-band channel must be admitted";
    EXPECT_TRUE(r.has("G21", kL1));
    EXPECT_TRUE(r.has("G01", kC1));
    EXPECT_TRUE(r.has("G01", kL2));

    // The refloat runs on every rejection, birth included.  At a true birth it is a
    // no-op (no chord, no deviation, no allocated ambiguity), which is why it can be
    // unconditional and why correctness does not depend on the birth-vs-restore
    // classification (see ResumeRestoredCorruptArc… below).
    EXPECT_GT(a.refloat_ambs, 0)
        << "every rejection re-taps: the classification that would justify skipping "
           "it is derived from a map that is not serialized and can be wrong";
    // Eleven dual-band satellites (two channels each) plus G21's single band.
    EXPECT_EQ(24, a.births);
    EXPECT_EQ(23, a.admits_birth);
    EXPECT_GT(a.max_z, 1.9e5);
    EXPECT_EQ("AAAA|G07|1", a.max_z_id) << "the census must NAME the channel it found";
}

// ---------------------------------------------------------------------------
// 2. The negative gate: a station-common 1 ms step rejects nothing
// ---------------------------------------------------------------------------
//
// Receiver clocks take clean 1 ms steps in the field (URUM00CHN and INEG00MEX in
// the FT63 leg window), putting |d| ~ 3e5 m on every row of the station for one
// epoch.  At this precision the station median is the receiver clock, so the step
// cancels exactly.  A raw-|d| screen would fire on every such step; this case
// checks that the statistic is centred.  The prefit census uses the same centring
// and the same negative gate.
TEST(MeasurementAdmission, StationCommonClockStepProducesZeroRejections) {
    LogManager lm;
    auto cfg = loadCfg(lm, "clockstep", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    constexpr double kOneMs = 2.99792458e5;   // one millisecond of light time
    std::vector<PppObservation> v;
    double spread = -12.5;
    for (const char* s : {"G01", "G03", "G05", "G08", "G09", "G11"}) {
        v.push_back(healthy(s, spread, kClockLevel + kOneMs));
        spread += 5.0;
    }
    r.step(ctx, *cfg, v);

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(0, a.reject_birth);
    EXPECT_EQ(0, a.over_bound) << "the whole station moved together; nothing is an outlier";
    EXPECT_EQ(0, a.withheld_channels);
    EXPECT_EQ(0, a.station_events);
    EXPECT_LT(a.max_z, 100.0) << "after centring, only the healthy per-satellite spread survives";
    for (const char* s : {"G01", "G03", "G05", "G08", "G09", "G11"}) {
        EXPECT_TRUE(r.has(s, kC1)) << s;
        EXPECT_TRUE(r.has(s, kL2)) << s;
    }
}

// ---------------------------------------------------------------------------
// 3. Below the median floor the station is skipped, not screened
// ---------------------------------------------------------------------------
//
// A median of two or three values can be the outlier it is meant to remove.  A
// screen that rejects off an untrusted centre is worse than no screen, so below the
// floor the station is skipped and counted rather than screened against a centre
// of zero.
TEST(MeasurementAdmission, ThinStationIsSkippedAndCountedNeverScreened) {
    LogManager lm;
    auto cfg = loadCfg(lm, "thin", armed(/*screen=*/true, /*bound=*/1000.0,
                                         /*min_sats=*/4));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Three satellites, one of them corrupt by 198 km.  With n < 4 nothing happens.
    std::vector<PppObservation> v{healthy("G01", -5.0), healthy("G03", 4.0),
                                  obsWith("G07", {kC1, kL1}, kClockLevel + kCorruptG07)};
    r.step(ctx, *cfg, v);

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.skips_thin);
    EXPECT_EQ(0, a.station_epochs);
    EXPECT_EQ(0, a.channels) << "a skipped station screens no channel at all";
    EXPECT_EQ(0, a.reject_birth);
    EXPECT_TRUE(r.has("G07", kC1)) << "no verdict may be reached from an untrusted centre";
    EXPECT_TRUE(admArcs(ctx) == 0) << "and no admission record may be created";
}

// ---------------------------------------------------------------------------
// 4. A mid-arc onset withholds the band and refloats the arc
// ---------------------------------------------------------------------------
//
// Standing invariant #1: a zero-q or stiff time-linked quantity needs a wired
// slip/jump response before a hole is made in it.  A withhold is such a hole, and
// the invariant-DD chord is keyed independently of the StateManager, so only an
// explicit re-tap discards it.  (The invariant carry's slip hole without a response cost
// 10.9 cyc/ep in the field.)
TEST(MeasurementAdmission, MidArcOnsetWithholdsTheBandAndRefloatsTheArc) {
    LogManager lm;
    auto cfg = loadCfg(lm, "onset", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Three clean epochs establish the arc.
    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    ASSERT_EQ(0, ctx.counters.adm.reject_birth);
    ASSERT_TRUE(r.slipped.empty()) << "a quiet screen must not touch the slip funnel";
    ASSERT_TRUE(r.has("G07", kC1));

    // The channel goes bad mid-arc.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.onsets);
    EXPECT_EQ(0, a.reject_birth) << "an ESTABLISHED channel going bad is an onset, not a birth";
    EXPECT_FALSE(r.has("G07", kC1));
    EXPECT_FALSE(r.has("G07", kL1));
    EXPECT_TRUE(r.funnelHas("G07", kL1))
        << "the arc behind the hole must be re-tapped: nothing bounds the error on a "
           "corrupt channel, so there is no value to correct toward — only state to discard";
    EXPECT_EQ(r.slipped, r.slipped_raw)
        << "a withheld band is a RAW observable event, so the Iono-2 v3k erase policy "
           "must ERASE this line of sight's deviation rather than D5-KEEP it";
    EXPECT_GT(a.refloat_reseeds, 0) << "both refloat feeds are written and counted apart";
}

// ---------------------------------------------------------------------------
// 5. A heal readmits only after N clean epochs, and never restores the old state
// ---------------------------------------------------------------------------
TEST(MeasurementAdmission, HealReadmitsAfterNCleanEpochsWithAFreshSeed) {
    LogManager lm;
    auto cfg = loadCfg(lm, "heal", armed(/*screen=*/true, /*bound=*/1000.0,
                                         /*min_sats=*/4, /*heal_ep=*/3));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    r.step(ctx, *cfg, bshmEpoch());                       // rejected at birth
    ASSERT_EQ(1, ctx.counters.adm.reject_birth);
    r.slipped.clear();
    r.slipped_raw.clear();

    // Two clean epochs are not enough: the band stays out and keeps re-testing.
    for (int k = 0; k < 2; ++k) {
        r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
        EXPECT_FALSE(r.has("G07", kC1)) << "readmitted too early at k=" << k;
    }
    EXPECT_EQ(0, ctx.counters.adm.heals);
    EXPECT_EQ(2, ctx.counters.adm.heal_progress);
    EXPECT_EQ(2, ctx.counters.adm.rejects_held)
        << "a held rejection emits no transition event, so its DURATION lives here";

    // The third completes the heal: the band is admitted in this epoch, and the arc
    // is re-tapped so that no old state comes back with it.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    EXPECT_EQ(1, ctx.counters.adm.heals);
    EXPECT_TRUE(r.has("G07", kC1)) << "a heal ADMITS — it must not withhold its own epoch";
    EXPECT_TRUE(r.has("G07", kL1));
    EXPECT_TRUE(r.funnelHas("G07", kL1))
        << "a heal never restores the old states: the invariant-DD chord is keyed "
           "independently of the StateManager, so only an explicit re-tap discards it";
}

// ---------------------------------------------------------------------------
// 6. A warm-resume restore is not a birth
// ---------------------------------------------------------------------------
//
// The record map is not serialized, so after a resume every channel looks new,
// but its estimator state crossed the snapshot boundary and may carry the corrupt
// seed.  The prefit census's seed-birth tripwire cannot see this case, and the two
// entry points get different responses: a birth has nothing to refloat, while a
// restore has a stiff carried chord that would otherwise carry across the hole.
TEST(MeasurementAdmission, ResumeRestoredCorruptArcIsRejectedAsARestoreAndRefloats) {
    LogManager lm;
    auto cfg = loadCfg(lm, "resume", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    ctx.carry.resume.adm = true;   // what a successful warm resume latches
    r.step(ctx, *cfg, bshmEpoch());

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.reject_resume);
    EXPECT_EQ(0, a.reject_birth) << "a restored corrupt arc is NOT a birth";
    EXPECT_EQ(1, a.resume_passes);
    EXPECT_FALSE(r.has("G07", kC1));
    EXPECT_TRUE(r.funnelHas("G07", kL1))
        << "unlike a birth, a restore HAS carried state — invariant #1 requires the re-tap";
    EXPECT_TRUE(ctx.carry.resume.adm)
        << "the RUN flag persists — it is the per-station set below that is consumed";
    EXPECT_EQ(1u, net::present(ctx.entities.stations, &net::StationState::adm_resume_seen))
        << "exactly the one screened station has had its restore epoch";

    // A second corrupt channel appearing later is an ordinary birth again.
    auto v = bshmEpoch(/*g07=*/8.0);
    v.push_back(obsWith("G16", {kC1}, kClockLevel + 9.75e4));
    r.step(ctx, *cfg, v);
    EXPECT_EQ(1, ctx.counters.adm.reject_birth);
    EXPECT_EQ(1, ctx.counters.adm.reject_resume) << "the restore rule must not persist";
}

// ---------------------------------------------------------------------------
// 7. Too many failures ⇒ the median is untrusted: hold the station, claim nothing
// ---------------------------------------------------------------------------
//
// Mirrors the IF-TD reference gate's incoherent-station verdict: when a family
// disagrees with its own centre, the centre is what is untrustworthy, so the
// response acts at the family's scope and makes no per-member claim.
TEST(MeasurementAdmission, StationGuardHoldsEverythingAndRejectsNothingIndividually) {
    LogManager lm;
    auto cfg = loadCfg(lm, "staguard", armed(/*screen=*/true, /*bound=*/1000.0,
                                             /*min_sats=*/4, /*heal_ep=*/5,
                                             /*max_frac=*/0.5));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Six satellites; four of them displaced in different directions, so the median
    // is a mixture rather than a common step.  4 of 6 channels fail, > 0.5.
    std::vector<PppObservation> v{
        obsWith("G01", {kC1, kL1}, kClockLevel + 0.0),
        obsWith("G03", {kC1, kL1}, kClockLevel + 5.0),
        obsWith("G05", {kC1, kL1}, kClockLevel + 4.0e4),
        obsWith("G08", {kC1, kL1}, kClockLevel - 6.0e4),
        obsWith("G09", {kC1, kL1}, kClockLevel + 9.0e4),
        obsWith("G11", {kC1, kL1}, kClockLevel - 3.0e4)};
    r.step(ctx, *cfg, v);

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.station_events);
    EXPECT_EQ(0, a.reject_birth)
        << "no per-channel verdict may be claimed off a corrupted centre";
    EXPECT_EQ(0, a.onsets);
    EXPECT_EQ(6, a.station_held_channels) << "hold ALL of the station's screened bands";
    for (const auto& o : r.modeled[0])
        EXPECT_TRUE(o.observed.empty()) << o.satellite << " should be held";
    EXPECT_TRUE(admArcs(ctx) == 0)
        << "and no admission record may be written from an epoch whose centre was "
           "untrusted — the next epoch must re-test from evidence";
    EXPECT_GT(a.refloat_ambs, 0)
        << "a station-wide hole still needs its arcs re-tapped (invariant #1)";
}

// ---------------------------------------------------------------------------
// 8. Disarmed, the seam is structurally inert
// ---------------------------------------------------------------------------
TEST(MeasurementAdmission, DisarmedRunTouchesNothing) {
    LogManager lm;
    auto cfg = loadCfg(lm, "off", "");     // no adm_* keys at all
    ASSERT_FALSE(cfg->getNetworkConfig().adm_screen);
    ASSERT_FALSE(cfg->getNetworkConfig().adm_trace);
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();
    ASSERT_FALSE(ctx.knobs.adm_on);

    r.step(ctx, *cfg, bshmEpoch());

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(0, a.epochs) << "the seam must return on its first line";
    EXPECT_EQ(0, a.channels);
    EXPECT_EQ(0, a.over_bound);
    EXPECT_TRUE(admArcs(ctx) == 0) << "no record map may be allocated";
    EXPECT_EQ(admEvents(ctx), 0u);
    EXPECT_DOUBLE_EQ(-1.0, r.fx->ep().events.admission.adm_ep_tow) << "and no epoch-status column stamped";
    EXPECT_TRUE(r.slipped.empty());
    // Every observable of the corrupt channel is still there: disarmed means the
    // screen makes no decision, not that it admits.
    EXPECT_TRUE(r.has("G07", kC1));
    EXPECT_TRUE(r.has("G07", kL1));
    EXPECT_TRUE(r.has("G21", kC1));
}

// ---------------------------------------------------------------------------
// 9. Measure-only: the census sees everything and changes nothing
// ---------------------------------------------------------------------------
//
// §7 gate 5 runs the full day with adm_trace on and the screen off, to give era-1
// its first absolute scan.  That arm is solve-inert while still enumerating the
// over-bound channels.
TEST(MeasurementAdmission, MeasureOnlyCensusCountsWithoutActing) {
    LogManager lm;
    auto cfg = loadCfg(lm, "census", armed(/*screen=*/false));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();
    ASSERT_TRUE(ctx.knobs.adm_on);
    ASSERT_FALSE(ctx.knobs.adm_screen);

    // A second, healthy station so the two census columns are distinguishable: one
    // is a population statistic over stations, the other the network extreme.
    r.present[1] = true;
    r.modeled[1] = {healthy("G01", -5.0), healthy("G03", 2.0), healthy("G05", 6.0),
                    healthy("G08", -8.0), healthy("G09", 9.0)};
    r.step(ctx, *cfg, bshmEpoch());

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.over_bound) << "the census must still FIND the corrupt channel";
    EXPECT_GT(a.max_z, 1.9e5);
    EXPECT_EQ("AAAA|G07|1", a.max_z_id);
    EXPECT_EQ(0, a.withheld_channels);
    EXPECT_EQ(0, a.reject_birth);
    EXPECT_TRUE(r.has("G07", kC1)) << "measure-only may not remove an observable";
    EXPECT_TRUE(admArcs(ctx) == 0)
        << "and may not build admission records there is no response to use";
    EXPECT_DOUBLE_EQ(1000.0, r.fx->ep().events.admission.adm_ep_tow)
        << "the per-epoch census columns are stamped even measure-only";
    EXPECT_GT(r.fx->ep().events.admission.ep_adm_net_worst_z, 1.9e5);
    EXPECT_LT(r.fx->ep().events.admission.ep_adm_sta_worst_z, r.fx->ep().events.admission.ep_adm_net_worst_z)
        << "the two columns are a POPULATION and an EXTREME, not the same number: "
           "a single maximum cannot make the separation claim falsifiable";
}

// ---------------------------------------------------------------------------
// 10. The centre is a median over satellites, not over observables
// ---------------------------------------------------------------------------
//
// A flat median over code observables weights a satellite by how many bands it is
// tracked on.  Three single-band satellites against one four-band satellite put
// four of the seven samples on one line of sight, so a corrupt four-band satellite
// sets the centre: it reads z ~ 0 and is admitted, while its three healthy peers
// read z ~ |d| and are rejected.  The station guard cannot catch that inversion,
// because it needs healthy bands to outnumber corrupt ones and the capture needs
// the opposite.  The configuration is reachable with the repository's shipped
// network configs (GPS+Galileo, frequency_mode "all", channel_mode "all").
TEST(MeasurementAdmission, OneMultiBandSatelliteCannotCaptureTheStationCentre) {
    LogManager lm;
    auto cfg = loadCfg(lm, "capture", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Three healthy single-band GPS satellites (the BSHM shape) + one corrupt
    // four-band Galileo satellite.  Observables: 3 + 4 = 7; satellites: 4.
    const double corrupt = 2.0e5;
    std::vector<PppObservation> v{
        obsWith("G07", {kC1, kL1}, kClockLevel + 1.0),
        obsWith("G16", {kC1, kL1}, kClockLevel - 2.0),
        obsWith("G21", {kC1, kL1}, kClockLevel + 3.0),
        obsWith("E11", {"C1C", "C5Q", "C7Q", "C6C", "L1C"}, kClockLevel + corrupt)};
    r.step(ctx, *cfg, v);

    const auto& a = ctx.counters.adm;
    // The four corrupt bands are rejected; the three healthy ones are not.
    for (const char* s : {"G07", "G16", "G21"})
        EXPECT_TRUE(r.has(s, kC1))
            << s << " is healthy and must survive: a satellite tracked on more bands "
                    "must not be able to out-vote its peers for the centre";
    for (const char* k : {"C1C", "C5Q", "C7Q", "C6C"})
        EXPECT_FALSE(r.has("E11", k)) << k << " is corrupt and must be withheld";
    EXPECT_EQ(4, a.reject_birth) << "exactly the four corrupt bands";
    EXPECT_EQ(3, a.admits_birth) << "exactly the three healthy channels";
    // n_sats is the satellite count, so adm_min_sats bounds the population it names.
    EXPECT_EQ(0, a.skips_thin) << "four satellites clears the 4-satellite floor";
    // The guard does not fire: one bad satellite in four is not a bad centre, even
    // though it holds four of the station's seven bands.  A band-denominated
    // guard would fire here (4/7 > 0.5) and refuse three sound per-channel verdicts.
    EXPECT_EQ(0, a.station_events)
        << "the guard's denominator is the population that formed the centre";
}

// ---------------------------------------------------------------------------
// 11. The warm-resume label is per station, not a global one-shot
// ---------------------------------------------------------------------------
//
// The seam screens the stations that are present and above the satellite floor.
// A station that is absent, thin, or whose satellite the two-station observability
// gate erased on the resume epoch does not have the restore rule consumed on its
// behalf; otherwise every later first sighting there would read as a birth and a
// restored corrupt arc would be mislabelled.
TEST(MeasurementAdmission, ResumeRestoreLabelIsPerStationNotAGlobalOneShot) {
    LogManager lm;
    auto cfg = loadCfg(lm, "resume2", armed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    ctx.carry.resume.adm = true;

    // Epoch 0: station 1 is absent (present[1] stays false), so only station 0 is
    // screened.  A global one-shot would be consumed here.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    ASSERT_EQ(0, ctx.counters.adm.reject_resume);
    ASSERT_EQ(0, ctx.counters.adm.reject_birth);

    // Epoch 1: station 1 appears for the first time, carrying a restored corrupt
    // arc.  Its first screened epoch still takes the restore label.
    r.present[1] = true;
    r.modeled[1] = {healthy("G01", -5.0), healthy("G03", 2.0), healthy("G05", 6.0),
                    healthy("G08", -8.0),
                    obsWith("G07", {kC1, kL1}, kClockLevel + kCorruptG07)};
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));

    EXPECT_EQ(1, ctx.counters.adm.reject_resume)
        << "station BBBB's first screened epoch is its RESTORE epoch, even though "
           "another station consumed the run's first seam pass";
    EXPECT_EQ(0, ctx.counters.adm.reject_birth);

    // Station 0 has already had its restore epoch, so a new corrupt channel there
    // now is an ordinary birth.
    auto v = bshmEpoch(/*g07=*/8.0);
    v.push_back(obsWith("G16", {kC1}, kClockLevel + 9.75e4));
    r.modeled[1].clear();
    r.present[1] = false;
    r.step(ctx, *cfg, v);
    EXPECT_EQ(1, ctx.counters.adm.reject_birth);
    EXPECT_EQ(1, ctx.counters.adm.reject_resume) << "the restore label does not persist";
}

// ---------------------------------------------------------------------------
// 12. The statistic itself: state-free, and band identity is the RINEX digit
// ---------------------------------------------------------------------------
//
// Pinned directly because every case above depends on it and none of them would
// fail clearly if it drifted.  `observed - modeled` lets the screen fire at a
// cell's birth: `modeled` carries the geometric range, the satellite clock and the
// deterministic corrections and no filter state.  A prefit innovation, by
// contrast, is driven to ~0 by a correctly self-seeded birth whatever the
// observable says.
TEST(MeasurementAdmission, StatisticAndBandIdentityAreWellDefined) {
    EXPECT_EQ('1', measurementAdmissionBand("C1C"));
    EXPECT_EQ('2', measurementAdmissionBand("L2W"));
    EXPECT_EQ('5', measurementAdmissionBand("C5Q"));
    EXPECT_EQ('\0', measurementAdmissionBand("C1C_C2W"))
        << "an iono-free combination has no single band to withhold";
    EXPECT_EQ('\0', measurementAdmissionBand(""));
    EXPECT_EQ('\0', measurementAdmissionBand("CXC"));

    PppObservation o = obsWith("G07", {kC1}, 1234.5);
    double d = 0.0;
    ASSERT_TRUE(measurementAdmissionD(o, kC1, d));
    EXPECT_NEAR(1234.5, d, 1e-6);
    EXPECT_FALSE(measurementAdmissionD(o, kC2, d)) << "absent observable";
    o.modeled.erase(kC1);
    EXPECT_FALSE(measurementAdmissionD(o, kC1, d))
        << "an observable with no modeled counterpart carries no statistic and must "
           "never enter a median";
}

// ===========================================================================
// The arc re-entry vet (§10, INC-4)
// ===========================================================================
//
// The admission record outlives the arc: it is kept for kAdmArcForgetEp (2880)
// epochs, so a channel that leaves the model set and comes back still carries
// `admitted = true` from before the gap.  Without the vet it is dispatched down the
// established path, and a bad re-entry is reported as a mid-arc onset (in a 24 h
// FT63 run, 16 of 17 `adm_onset` events followed an `arc_resume` within five
// minutes).
//
// The costs are asymmetric: a birth rejection moved a00 by a median of -0.11 TECU
// (max +0.32) over eight events, while re-entries that were re-admitted and caught
// only 2-8 epochs later coincided with the day's two largest iono excursions.
// Refusing at admission costs little; removing an established arc does not.

// ---------------------------------------------------------------------------
// 13. A re-entry after a gap is screened as a birth, not inherited as an onset
// ---------------------------------------------------------------------------
//
// In this sub-class the screen fires at the right epoch with the right response,
// and only the label differs.  The onset label claims "a previously good channel
// went bad", which the evidence does not support, and the same cell in a leg replay
// whose record map starts empty is called `adm_reject_birth`.  The vet makes the
// classification independent of how long the run has been going.
TEST(MeasurementAdmission, ReEntryAfterAGapIsScreenedAsABirthNotAnOnset) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_birth", revetArmed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Three clean epochs establish G07 as an admitted, healthy arc.
    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    ASSERT_EQ(0, ctx.counters.adm.reject_birth);
    ASSERT_EQ(0, ctx.counters.adm.reentries);

    // It disappears for two epochs (the satellite set, or the observability gate
    // erased it).  Its record survives; its arc does not.
    for (int k = 0; k < 2; ++k) r.step(ctx, *cfg, bshmEpochWithout("G07"));
    ASSERT_EQ(0, ctx.counters.adm.reentries) << "an ABSENT channel is not screened at all";

    // It returns corrupt.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.reentries) << "exactly the returning channel is re-screened";
    EXPECT_EQ(1, a.reject_reentry);
    EXPECT_EQ(0, a.onsets)
        << "the onset counter is what this increment exists to empty — an arc that "
           "was ABSENT did not 'go bad while present'";
    EXPECT_EQ(0, a.reject_birth) << "a re-entry is not a birth either; the three "
                                    "entry points stay mutually exclusive";
    EXPECT_EQ(0, a.admits_reentry);
    EXPECT_FALSE(r.has("G07", kC1)) << "the response is unchanged — this is a relabel";
    EXPECT_FALSE(r.has("G07", kL1));
    EXPECT_TRUE(r.funnelHas("G07", kL1))
        << "and the refloat still runs: the record survived a boundary the ARC did "
           "not, so a stiff carried chord could be riding the hole (invariant #1)";
    EXPECT_TRUE(r.has("G21", kC1)) << "the healthy single-band control is untouched";
}

// ---------------------------------------------------------------------------
// 14. The control: with the vet disarmed the same sequence is still an onset (INC-4)
// ---------------------------------------------------------------------------
//
// This pins the mislabel itself.  It is the only case in the file that asserts
// behaviour the design considers wrong; it checks that disarming the vet reproduces
// the pre-change classification exactly.
TEST(MeasurementAdmission, DisarmedVetStillInheritsTheStaleVerdictAsAnOnset) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_off", armed());   // INC-4 absent entirely
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    for (int k = 0; k < 2; ++k) r.step(ctx, *cfg, bshmEpochWithout("G07"));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.onsets) << "the pre-change reading, reproduced exactly";
    EXPECT_EQ(0, a.reentries) << "and not one INC-4 counter moves when it is disarmed";
    EXPECT_EQ(0, a.reject_reentry);
    EXPECT_EQ(0, a.revet_window_ep);
    EXPECT_EQ(0, a.revet_census_absent);
    EXPECT_FALSE(r.has("G07", kC1)) << "the RESPONSE was never the defect";
}

// ---------------------------------------------------------------------------
// 15. A continuously-present channel going bad is still a genuine onset
// ---------------------------------------------------------------------------
//
// The vet does not empty the onset counter by absorbing genuine onsets.  A channel
// that never left the screened set and then crosses the bound is what `adm_onset`
// is for, and it is still counted there.
TEST(MeasurementAdmission, ContinuouslyPresentChannelGoingBadIsStillAnOnset) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_cont", revetArmed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 4; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.onsets);
    EXPECT_EQ(0, a.reentries) << "gap 0 is not a re-entry at any threshold";
    EXPECT_EQ(0, a.reject_reentry);
    EXPECT_FALSE(r.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 16. A gap under adm_revet_gap_ep is not a re-entry
// ---------------------------------------------------------------------------
TEST(MeasurementAdmission, GapBelowTheThresholdIsNotAReEntry) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_gap", revetArmed(/*gap_ep=*/3));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    for (int k = 0; k < 2; ++k) r.step(ctx, *cfg, bshmEpochWithout("G07"));   // gap 2 < 3
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(0, a.reentries) << "the threshold must actually bind";
    EXPECT_EQ(1, a.onsets);
    EXPECT_FALSE(r.has("G07", kC1)) << "the channel is still refused, by the other path";
}

// ---------------------------------------------------------------------------
// 17. The census trigger refuses inside the vet window where the bound would admit
// ---------------------------------------------------------------------------
//
// Isolates what the cross-seam census input adds.  The census statistic is
// standardized (|y - m| sqrt(w)), so it resolves a ramping corruption while it is
// still under this screen's absolute metre bound (at G16@BSHM it read s = 1439.6
// on a raw y = 975.5 m one epoch before |z| reached 1000).  Here the channel stays
// well inside the bound at every epoch, so the bound alone cannot refuse it and
// any refusal is attributable to the census.
TEST(MeasurementAdmission, CensusFlagRefusesInsideTheVetWindowWhereTheBoundWouldAdmit) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_census", revetArmed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));

    // It returns inside the bound and is admitted provisionally: no withhold, no
    // refloat.  The four exposed cells of the FT63 day were in this state.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/300.0));
    ASSERT_EQ(1, ctx.counters.adm.reentries);
    ASSERT_EQ(1, ctx.counters.adm.admits_reentry);
    ASSERT_EQ(0, ctx.counters.adm.reject_reentry);
    ASSERT_TRUE(r.has("G07", kC1)) << "a clean re-entry must be admitted, not held";
    const long long refloats_before = ctx.counters.adm.refloat_ambs;

    // Next epoch the census names the cell.  |z| is still 300 m, far under the bound.
    r.censusFlag(ctx, "G07");
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/300.0));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.revet_census_refusals)
        << "the bound could not have made this refusal — |z| = 300 m against a "
           "1000 m bound — so it is the census's, and it is counted as such";
    EXPECT_EQ(1, a.onsets) << "mechanically it IS a mid-arc withhold; the counter is "
                              "honest about that while the event names the trigger";
    EXPECT_EQ(0, a.revet_census_absent) << "the flag carried the previous epoch's stamp";
    EXPECT_FALSE(r.has("G07", kC1)) << "and the response actually reached the data";
    EXPECT_FALSE(r.has("G07", kL1));
    EXPECT_GT(a.refloat_ambs, refloats_before)
        << "a mid-arc withhold refloats — the arc was admitted for an epoch, so "
           "there IS state behind the hole this time (invariant #1)";
}

// ---------------------------------------------------------------------------
// 18. A stale census flag is refused as evidence and counted
// ---------------------------------------------------------------------------
//
// Invariant #4 applied to a cross-seam read: a flag set in an earlier epoch names
// cells that may since have healed, and acting on it would mean acting on evidence
// the vet does not have.  "The census said nothing" and "the census was not
// consulted" stay distinguishable, because only the first is a clean run.
TEST(MeasurementAdmission, StaleCensusFlagIsIgnoredAndCounted) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_stale", revetArmed());
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/300.0));       // re-entry, admitted, vet open
    ASSERT_EQ(1, ctx.counters.adm.admits_reentry);

    // A flag naming the cell, but stamped several epochs ago: the shape a run with
    // the prefit walk switched off part-way through would produce.
    net::assignLosFlags(ctx.entities, &net::LosState::prefit_code_flag, {{0, std::string("G07")}});
    ctx.carry.prefit_code_flag_ep = 0;
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/300.0));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(0, a.revet_census_refusals) << "stale evidence is not evidence";
    EXPECT_EQ(0, a.onsets);
    EXPECT_GT(a.revet_census_absent, 0)
        << "and the consultation that did not happen is on the record, not silent";
    EXPECT_TRUE(r.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 19. The vet window expires and the census stops being consulted
// ---------------------------------------------------------------------------
//
// The window is bounded on purpose: once a channel has served it, it is an ordinary
// established arc again and the |z| bound is the only test it answers to.  Without
// this, one census flag would refuse a cell at any distance from its re-entry.
TEST(MeasurementAdmission, VetWindowExpiresAndStopsConsultingTheCensus) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_expire", revetArmed(/*gap_ep=*/1, /*vet_ep=*/2));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));         // re-entry: vet_left = 2 -> 1
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));         // vet_left = 1 -> 0, expired
    ASSERT_GE(ctx.counters.adm.revet_expired, 1);
    ASSERT_EQ(2, ctx.counters.adm.revet_window_ep)
        << "coverage runs from the return epoch through return + (adm_revet_ep - 1)";
    // One consultation, not two.  The return epoch is inside the window but takes
    // the birth-equivalent path, which does not consult the census; it could not do
    // so usefully, because a re-entering arc's ambiguity is fresh and the census
    // defers it as a birth on that epoch.  `revet_census_reads` is the denominator
    // that makes this readable; without it, `absent < revet_window_ep` would look
    // like a partial failure.
    ASSERT_EQ(1, ctx.counters.adm.revet_census_reads);
    ASSERT_EQ(1, ctx.counters.adm.revet_census_absent)
        << "no flag has been published in this test yet — the shape a run with the "
           "prefit walk switched off produces on EVERY consultation, and what the "
           "run-end dead-wiring warn keys on (absent == reads)";
    const long long absent_before = ctx.counters.adm.revet_census_absent;
    const long long reads_before  = ctx.counters.adm.revet_census_reads;

    // The census now names the cell, but the window is closed.
    r.censusFlag(ctx, "G07");
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(0, a.revet_census_refusals) << "an expired window consults nothing";
    EXPECT_EQ(0, a.onsets);
    EXPECT_EQ(reads_before, a.revet_census_reads)
        << "an expired window must not even ASK — the counter that proves it";
    EXPECT_EQ(absent_before, a.revet_census_absent)
        << "and so it cannot count a failed consultation it never intended to make";
    EXPECT_EQ(2, a.revet_window_ep) << "nor does the window re-open on its own";
    EXPECT_TRUE(r.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 20. The strict posture withholds a clean re-entry until the heal path readmits
// ---------------------------------------------------------------------------
//
// Off by default, per the prefer-fallback directive: it withholds every re-entry
// (~4650 on a 63-station day), and the population it delays is dominated by
// legitimately rising satellites.  It is the one posture that closes the exposure
// window outright, so it is tested for correctness and measurability.
TEST(MeasurementAdmission, StrictHoldWithholdsACleanReEntryThenHealsIt) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_hold",
                       revetArmed(/*gap_ep=*/1, /*vet_ep=*/8, /*use_census=*/true,
                                  /*hold=*/true, /*heal_ep=*/2));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));

    // Returns clean and is still held, because one in-bound epoch is thin evidence
    // about a channel that has been away.  G16@BSHM and G13@BSHM both passed the
    // bound on their return epoch and crossed it 2 and 5 epochs later.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    EXPECT_EQ(1, ctx.counters.adm.reentries);
    EXPECT_EQ(1, ctx.counters.adm.revet_hold_ep);
    EXPECT_EQ(0, ctx.counters.adm.admits_reentry) << "the strict posture admits nothing "
                                                     "on the return epoch";
    EXPECT_EQ(0, ctx.counters.adm.reject_reentry) << "nor does it claim the channel FAILED";
    EXPECT_FALSE(r.has("G07", kC1));

    // Two clean epochs re-earn it through the ordinary heal path.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    EXPECT_EQ(1, ctx.counters.adm.heals) << "the hold must be a delay, not a death "
                                            "sentence — prefer fallback over dropping";
    EXPECT_TRUE(r.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 21. A heal opens a vet window of its own
// ---------------------------------------------------------------------------
//
// A heal is a re-entry: the same arc coming back with a fresh seed after an
// absence from the model set.  Without its own window, the path that puts a
// previously rejected channel back into the solve would be the only one that puts
// it back unvetted, which is where an unvetted return matters most.
TEST(MeasurementAdmission, HealOpensAVetWindowOfItsOwn) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_heal",
                       revetArmed(/*gap_ep=*/1, /*vet_ep=*/8, /*use_census=*/true,
                                  /*hold=*/false, /*heal_ep=*/2));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    // Rejected at birth, then two clean epochs heal it.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));
    ASSERT_EQ(1, ctx.counters.adm.reject_birth);
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    ASSERT_EQ(1, ctx.counters.adm.heals);
    ASSERT_TRUE(r.has("G07", kC1));

    // The census names it while the post-heal window is open.
    r.censusFlag(ctx, "G07");
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));

    EXPECT_EQ(1, ctx.counters.adm.revet_census_refusals)
        << "a freshly healed channel is vetted like any other re-entry";
    EXPECT_FALSE(r.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 22. The tighter vet bound refuses on arrival a re-entry that the outer bound
//     would have admitted
// ---------------------------------------------------------------------------
//
// The mechanism the §10.10 measurement supports; it covers the case the census
// cannot reach.  G16@BSHM arrives at 670.3 m on its return epoch and crosses the
// 1000 m bound two epochs later, by which time it has been folded at full weight
// and a00 has moved 39.7 -> 58.8 TECU.  A 200 m vet bound refuses it on arrival.
//
// The fixture has that shape: a channel that returns at 670 m, well inside
// `adm_bound_m` and well outside 0.2 × it.
TEST(MeasurementAdmission, TighterVetBoundRefusesALargeOnArrivalReEntry) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_vbound",
                       revetArmed(/*gap_ep=*/1, /*vet_ep=*/8, /*use_census=*/true,
                                  /*hold=*/false, /*heal_ep=*/5,
                                  /*bound_scale=*/0.2));   // 200 m
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));

    // Returns at 670 m, the measured G16 arrival.  Inside 1000, outside 200.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/670.3));

    const auto& a = ctx.counters.adm;
    EXPECT_EQ(1, a.reentries);
    EXPECT_EQ(1, a.reject_reentry) << "refused on the RETURN epoch — the whole point; "
                                      "a00 never gets the chance to move";
    EXPECT_EQ(1, a.revet_bound_refusals)
        << "and attributed to the vet bound, because |z| = 670 m is well inside "
           "adm_bound_m and that bound could not have made this call";
    EXPECT_EQ(0, a.revet_census_refusals) << "no census flag was published here";
    EXPECT_EQ(0, a.admits_reentry);
    EXPECT_EQ(0, a.onsets);
    EXPECT_FALSE(r.has("G07", kC1));
    EXPECT_FALSE(r.has("G07", kL1));
    EXPECT_TRUE(r.funnelHas("G07", kL1)) << "invariant #1 still applies to a re-entry";

    // A true birth at the same magnitude is not held to the tighter bound: a first
    // sighting has no history to be suspicious about, and the rising-satellite
    // population is largest there.
    LogManager lm2;
    auto cfg2 = loadCfg(lm2, "revet_vbound_birth",
                        revetArmed(1, 8, true, false, 5, /*bound_scale=*/0.2));
    NetworkRunContext ctx2(*cfg2, cfg2->getNetworkConfig());
    Rig r2(lm2, *cfg2);
    r2.step(ctx2, *cfg2, bshmEpoch(/*g07=*/670.3));
    EXPECT_EQ(0, ctx2.counters.adm.reject_birth)
        << "the tighter bound is a RE-ENTRY rule; §10.10's population is cells within "
           "8 epochs of an arc_resume and nothing licenses extending it to a birth";
    EXPECT_EQ(0, ctx2.counters.adm.revet_bound_refusals);
    EXPECT_TRUE(r2.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 23. The tighter bound stays live for the whole vet window
// ---------------------------------------------------------------------------
//
// Not only on the return epoch: §10.10's measured population is "within 8 epochs of
// an arc_resume", which is the window, and it holds no healthy cells over 50 m, so
// holding the whole window to 200 m stays within the measurement.  It also catches
// a shape that neither bound catches on the return epoch: a channel that arrives
// clean and ramps through 200 m before it ramps through 1000.
TEST(MeasurementAdmission, TighterVetBoundStaysLiveAcrossTheWindowThenLifts) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_vwindow",
                       revetArmed(/*gap_ep=*/1, /*vet_ep=*/3, /*use_census=*/false,
                                  /*hold=*/false, /*heal_ep=*/5,
                                  /*bound_scale=*/0.2));
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));      // returns clean: vet_left 3 -> 2
    ASSERT_EQ(1, ctx.counters.adm.admits_reentry);
    ASSERT_TRUE(r.has("G07", kC1));

    // Ramps to 500 m inside the window: over 200, under 1000.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/500.0));
    EXPECT_EQ(1, ctx.counters.adm.revet_bound_refusals)
        << "the window is a scope, not a single epoch";
    EXPECT_EQ(1, ctx.counters.adm.onsets);
    EXPECT_FALSE(r.has("G07", kC1));

    // Outside any vet window, 500 m is an ordinary in-bound value; otherwise one
    // re-entry would hold a cell to 200 m forever.  The second rig checks this on
    // an established channel that never had a gap.
    LogManager lm2;
    auto cfg2 = loadCfg(lm2, "revet_vlift",
                        revetArmed(1, 1, false, false, /*heal_ep=*/1,
                                   /*bound_scale=*/0.2));
    NetworkRunContext ctx2(*cfg2, cfg2->getNetworkConfig());
    Rig r2(lm2, *cfg2);
    for (int k = 0; k < 6; ++k) r2.step(ctx2, *cfg2, bshmEpoch(/*g07=*/8.0));
    ASSERT_EQ(0, ctx2.counters.adm.reentries) << "no gap, so no window ever opened";
    r2.step(ctx2, *cfg2, bshmEpoch(/*g07=*/500.0));
    EXPECT_EQ(0, ctx2.counters.adm.revet_bound_refusals)
        << "an established channel outside any vet window answers to adm_bound_m only";
    EXPECT_EQ(0, ctx2.counters.adm.onsets);
    EXPECT_TRUE(r2.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 24. A channel refused by the vet bound heals against the vet bound
// ---------------------------------------------------------------------------
//
// Pins the §10.11 field defect.  A heal path that tests the absolute bound lets a
// channel held out at 200 m accumulate "clean" epochs at anything under 1000 and be
// readmitted above the bound that refused it (G22@BSHM: refused at z = 347.6 m,
// healed at 782.2 m, refused again at 888.5 m, with the readmission folded into
// a00).  The heal therefore tests the bound that made the refusal.
//
// Second, a withheld epoch spends no vet budget; otherwise a channel held out
// would serve its window while absent and return unvetted, the state the vet
// exists to prevent (INC-4).
TEST(MeasurementAdmission, VetBoundRefusalHealsAgainstTheVetBoundNotTheOuterBound) {
    LogManager lm;
    auto cfg = loadCfg(lm, "revet_healbound",
                       revetArmed(/*gap_ep=*/1, /*vet_ep=*/2, /*use_census=*/false,
                                  /*hold=*/false, /*heal_ep=*/2,
                                  /*bound_scale=*/0.2));   // 200 m
    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/347.6));       // the G22 arrival, refused
    ASSERT_EQ(1, ctx.counters.adm.reject_reentry);
    ASSERT_EQ(1, ctx.counters.adm.revet_bound_refusals);
    ASSERT_FALSE(r.has("G07", kC1));

    // Two epochs at 782 m: under adm_bound_m, far over the vet bound.  A heal tested
    // against the outer bound would complete here, and with `adm_revet_ep` = 2 a
    // countdown that ran while the channel was withheld would also have expired.
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/782.2));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/782.2));
    EXPECT_EQ(0, ctx.counters.adm.heals)
        << "782 m is not clean for a channel this seam refused at 347 m";
    EXPECT_FALSE(r.has("G07", kC1)) << "and it must still be out of the model set";
    EXPECT_EQ(0, ctx.counters.adm.revet_expired)
        << "a withheld epoch spends no vet budget — otherwise the channel serves "
           "its window in absentia and comes back unvetted";

    // A channel that recovers under the vet bound is readmitted on schedule.  The
    // hold is a delay, not a permanent exclusion (prefer fallback over dropping).
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/12.0));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/12.0));
    EXPECT_EQ(1, ctx.counters.adm.heals);
    EXPECT_TRUE(r.has("G07", kC1));
}

// ---------------------------------------------------------------------------
// 25. The vet needs the screen: arming it measure-only is refused at load
// ---------------------------------------------------------------------------
//
// A vet is an admission verdict, and the measure-only arm makes none.  Following the
// signal-integrity block's inert-guard precedent, the loader disables the knob with
// a message rather than accepting it, because a knob that reads on in the manifest
// and does nothing in the run gives live counters over dead wiring.
TEST(MeasurementAdmission, VetIsRefusedWithoutTheScreen) {
    LogManager lm;
    std::string s = armed(/*screen=*/false, /*bound=*/1000.0, /*min_sats=*/4,
                          /*heal_ep=*/5, /*max_frac=*/0.5, /*trace=*/true);
    s += "    \"adm_revet\": true,\n";
    auto cfg = loadCfg(lm, "revet_noscreen", s);
    EXPECT_FALSE(cfg->getNetworkConfig().adm_revet)
        << "the loader must refuse a vet with nothing to vet FOR";

    Rig r(lm, *cfg);
    NetworkRunContext& ctx = r.fx->ctx();
    EXPECT_FALSE(ctx.knobs.adm_revet)
        << "and the predicate must re-impose it rather than trust load order";

    for (int k = 0; k < 3; ++k) r.step(ctx, *cfg, bshmEpoch(/*g07=*/8.0));
    r.step(ctx, *cfg, bshmEpochWithout("G07"));
    r.step(ctx, *cfg, bshmEpoch(/*g07=*/kCorruptG07));
    EXPECT_EQ(0, ctx.counters.adm.reentries);
    EXPECT_TRUE(r.has("G07", kC1)) << "measure-only stays measure-only";
}
