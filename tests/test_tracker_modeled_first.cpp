/**
 * @file test_tracker_modeled_first.cpp
 * @brief The arc-level tracker's band signal taken from the station's canonical
 *        latch (
 *        docs/refound/GRK_gap_reentry_key_mismatch_2026-09-22.md section 3).
 *
 * The case under test is a key mismatch, not a wrong number.  The arc-level
 * tracker builds its primary pair from `raw_observed` as the lexically lowest
 * phase per band that has a matching code, so a station reporting L1C and L1W
 * tracks band 1 as L1C.  The carried ambiguity at that station is keyed on the
 * station's canonical band-1 signal, which the priority latch makes L1W.  The
 * gap re-entry response then looks the carry up under L1C, finds nothing, and
 * the stale band-1 level folds across the break.
 *
 * Two parts are exercised, both over synthetic values with no station, no state
 * manager and no solve:
 *
 *  - the selection rule in `networkComputeRawObservables`, driven with a latch
 *    lookup bound the way the run loop binds one over the stations' PppModels.
 *    The source is the latch and not the epoch's admitted row set
 *    (`PppObservation::observed`): a returning arc's phase can be withheld from
 *    the admitted set for its first epochs, and that is the re-entry epoch the
 *    tracker has to name correctly;
 *  - the `gap_reentry_unreachable` counter in `networkSlipDrainGapReentry`,
 *    which counts only the arcs the response could have acted on.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/slip/SlipQc.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
// Synthetic geometry (GPS L1 / L2 / L5), the shape test_gap_reentry_per_signal
// uses.  Duplicated on purpose: neither file's fixture may be changed under the
// other.
// ---------------------------------------------------------------------------

const double kL1 = Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ;
const double kL2 = Constants::SPEED_OF_LIGHT / Constants::GPS_L2_FREQ;
const double kL5 = Constants::SPEED_OF_LIGHT / Constants::GPS_L5_FREQ;

constexpr double kRangeM  = 22'000'000.0;   ///< Nominal geometric range.
constexpr double kIonoL1M = 5.0;            ///< First-order L1 ionospheric delay.

/// First-order ionospheric delay at a band, in metres.
double ionoAt(double lambda_m) {
    const double ratio = lambda_m / kL1;
    return kIonoL1M * ratio * ratio;
}

/// One RAW band: a phase (cycles) and its matching code (metres).
struct Raw {
    std::string sig;        ///< Phase signal name ("L1C").
    double      lambda_m;   ///< Its wavelength.
    double      amb_cyc{0}; ///< Its ambiguity (cycles).
    bool        code{true}; ///< Emit the matching "C"+tail code.
};

/// A GPS satellite reporting @p raw, with @p admitted naming the signals the
/// epoch's admitted row set carries (@c PppObservation::observed).  Independent
/// of the latch the harness holds: the admitted set is per epoch and a returning
/// arc's phase can be missing from it, which is the case the tracker must still
/// name correctly.
PppObservation makeObs(const std::string& sat, const std::vector<Raw>& raw,
                       const std::vector<std::string>& admitted) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system    = System::GPS;
    obs.time      = GnssTime{ 2400, 0.0 };
    obs.elevation = 45.0;
    for (const auto& b : raw) {
        const double iono = ionoAt(b.lambda_m);
        obs.raw_observed[b.sig] = (kRangeM - iono) / b.lambda_m + b.amb_cyc;
        if (b.code) obs.raw_observed["C" + b.sig.substr(1)] = kRangeM + iono;
    }
    for (const auto& m : admitted) {
        const auto it = obs.raw_observed.find(m);
        if (it != obs.raw_observed.end()) obs.observed[m] = it->second;
        const auto cit = obs.raw_observed.find("C" + m.substr(1));
        if (cit != obs.raw_observed.end()) obs.observed["C" + m.substr(1)] = cit->second;
    }
    return obs;
}

/// One station's compute pass, driven the way the run loop drives it, with the
/// station's canonical phase latch supplied as the run loop supplies it.
struct TrackerHarness {
    LogManager     log_manager;
    PppUtils       utils{ log_manager };
    RawSlipEpoch   epoch;
    SlipQcCounters counters;
    /// Band digit to latched GPS phase signal.  A band absent from the map is a
    /// cell the latch does not know, which the tracker reads as "change nothing".
    std::map<char, std::string> latch;
    /// False reproduces the pre-card call shape: no lookup bound at all.
    bool bind_latch{ true };

    /// @param modeled_first  Gap re-entry key knob state for this pass.
    void run(const std::vector<PppObservation>& obs, bool modeled_first) {
        const std::vector<std::vector<PppObservation>> modeled{ obs };
        epoch.reset(1);
        const std::vector<bool> present{ true };
        const TrackerLatchLookup lookup =
            [this](int sta, const std::string& system, char band) -> std::string {
                EXPECT_EQ(sta, 0);
                EXPECT_EQ(system, std::string("GPS"));
                const auto it = latch.find(band);
                return it == latch.end() ? std::string{} : it->second;
            };
        networkComputeRawObservables(modeled, present, 1, utils, epoch,
                                     /*per_signal=*/false, modeled_first, &counters,
                                     bind_latch ? &lookup : nullptr);
    }

    /// This epoch's row for one satellite, or null.
    const RawSlipObs* row(const std::string& sat) const {
        for (const auto& r : epoch.rows.at(0))
            if (r.satellite == sat) return &r;
        return nullptr;
    }
};

/// The field's ten stations: both band-1 codes reported, the station latched on
/// the semi-codeless pair (L1W / L2W), which is what the carry is keyed on.
std::vector<Raw> bothBand1Codes() {
    return { { "L1C", kL1, 100.0 }, { "L1W", kL1, 140.0 },
             { "L2W", kL2, 60.0 } };
}

/// The latch of those stations.
std::map<char, std::string> wLatch() { return { { '1', "L1W" }, { '2', "L2W" } }; }

// ===========================================================================
// (a) OFF: the existing lexical rule, pinned
// ===========================================================================

// Byte-identity control (standing invariant #6).  With the knob off, a station
// reporting L1C and L1W tracks band 1 as L1C whatever it latched: the mismatch
// the gap re-entry key fix addresses, and the behaviour of the unarmed path.
TEST(TrackerModeledFirstTest, OffTracksTheLexicallyLowestBandOneSignal) {
    TrackerHarness h;
    h.latch = wLatch();
    h.run({ makeObs("G13", bothBand1Codes(), { "L1W", "L2W" }) }, /*modeled_first=*/false);

    const RawSlipObs* r = h.row("G13");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sig1, "L1C");
    EXPECT_EQ(r->sig2, "L2W");
    EXPECT_TRUE(r->dual);
}

// ===========================================================================
// (b) ON: the latched signal, and (e) the mismatch counter's two readings
// ===========================================================================

// Armed, the tracker names band 1 by the latched signal, so it agrees with the
// key the carried ambiguity uses.  Band 2 is unaffected (one signal reported,
// and it is the latched one).
TEST(TrackerModeledFirstTest, OnTracksTheLatchedBandOneSignal) {
    TrackerHarness h;
    h.latch = wLatch();
    h.run({ makeObs("G13", bothBand1Codes(), { "L1W", "L2W" }) }, /*modeled_first=*/true);

    const RawSlipObs* r = h.row("G13");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sig1, "L1W");
    EXPECT_EQ(r->sig2, "L2W");
    EXPECT_TRUE(r->dual);
}

// The source is the latch, not the epoch's admitted row set.  Admission and
// readmission can withhold a returning arc's phase from the admitted set for its
// first epochs, and that is the re-entry epoch the tracker has to name correctly.
// Here L1W is reported with its code but absent from the admitted set, and it is
// still chosen.
TEST(TrackerModeledFirstTest, OnUsesTheLatchEvenWhenTheAdmittedRowSetOmitsIt) {
    TrackerHarness h;
    h.latch = wLatch();
    // The admitted set carries only the fallback band-1 signal.
    const PppObservation obs = makeObs("G13", bothBand1Codes(), { "L1C", "L2W" });
    ASSERT_EQ(obs.observed.count("L1W"), 0u);
    ASSERT_EQ(obs.raw_observed.count("L1W"), 1u);
    h.run({ obs }, /*modeled_first=*/true);

    const RawSlipObs* r = h.row("G13");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sig1, "L1W");
    EXPECT_EQ(r->sig2, "L2W");
    // The off arm on the same observation still reads the cell as a mismatch,
    // so the counter does not measure the admitted set either.
    TrackerHarness off;
    off.latch = wLatch();
    off.run({ obs }, /*modeled_first=*/false);
    EXPECT_EQ(off.counters.gap_tracker_key_mismatch, 1);
}

// (c) The rule follows the latch; it is not a fixed preference for the
// semi-codeless signal.  The same raw stream at a station latched on L1C tracks
// L1C when armed, and the two arms then agree.
TEST(TrackerModeledFirstTest, OnFollowsTheLatchRatherThanAFixedPreference) {
    TrackerHarness on;
    TrackerHarness off;
    on.latch = off.latch = { { '1', "L1C" }, { '2', "L2W" } };
    const std::vector<PppObservation> ep{
        makeObs("G13", bothBand1Codes(), { "L1C", "L2W" }) };
    on.run(ep, /*modeled_first=*/true);
    off.run(ep, /*modeled_first=*/false);

    const RawSlipObs* a = on.row("G13");
    const RawSlipObs* b = off.row("G13");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->sig1, "L1C");
    EXPECT_EQ(a->sig1, b->sig1);
    EXPECT_EQ(a->sig2, b->sig2);
    // No cell differs, in either arm: the latch names what the lexical rule
    // picked, on both bands.
    EXPECT_EQ(on.counters.gap_tracker_key_mismatch, 0);
    EXPECT_EQ(off.counters.gap_tracker_key_mismatch, 0);
}

// (d) A band the latch does not name keeps the lexical pick.  The dual posture's
// band 5 is present in raw_observed and absent from the latch, and the fallback
// is what keeps the pair formable there.
TEST(TrackerModeledFirstTest, OnFallsBackToTheLexicalRuleOnAnUnlatchedBand) {
    // Band 1 is not reported at all, so the two lowest distinct bands are 2 and
    // 5, and only band 2 is latched.
    const std::vector<Raw> raw{ { "L2W", kL2, 60.0 },
                                { "L5Q", kL5, 40.0 }, { "L5X", kL5, 55.0 } };
    TrackerHarness h;
    h.latch = { { '2', "L2W" } };
    h.run({ makeObs("G13", raw, { "L2W" }) }, /*modeled_first=*/true);

    const RawSlipObs* r = h.row("G13");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sig1, "L2W");
    EXPECT_EQ(r->sig2, "L5Q");        // the lexically lowest band-5 phase
    EXPECT_TRUE(r->dual);
    // The unlatched band contributes no mismatch cell either: the counter is
    // defined only where the latch names a signal tracked this epoch.
    EXPECT_EQ(h.counters.gap_tracker_key_mismatch, 0);
}

// (e) The counter reads 1 in case (a) and 0 in case (b), on the same stream.
// It counts the population the response cannot reach, so it is readable from an
// unarmed run (standing invariant #4) and reads 0 when armed.
TEST(TrackerModeledFirstTest, MismatchCounterCountsTheOffCellAndNotTheOnOne) {
    TrackerHarness off;
    TrackerHarness on;
    off.latch = on.latch = wLatch();
    const std::vector<PppObservation> ep{
        makeObs("G13", bothBand1Codes(), { "L1W", "L2W" }) };
    off.run(ep, /*modeled_first=*/false);
    on.run(ep, /*modeled_first=*/true);

    EXPECT_EQ(off.counters.gap_tracker_key_mismatch, 1);
    EXPECT_EQ(on.counters.gap_tracker_key_mismatch, 0);
}

// The counter is knob-independent telemetry and accumulates over epochs and
// satellites rather than reporting one cell per run.  The latch is per station,
// so a satellite that does not report the latched band-1 signal contributes no
// cell at all; that separates the counter from a row count.
TEST(TrackerModeledFirstTest, MismatchCounterAccumulatesPerSatelliteAndEpoch) {
    TrackerHarness off;
    off.latch = wLatch();
    const std::vector<PppObservation> ep{
        makeObs("G13", bothBand1Codes(), { "L1W", "L2W" }),
        makeObs("G14", bothBand1Codes(), { "L1W", "L2W" }),
        makeObs("G15", { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } },
                { "L1C", "L2W" }) };
    off.run(ep, /*modeled_first=*/false);
    EXPECT_EQ(off.counters.gap_tracker_key_mismatch, 2);
    off.run(ep, /*modeled_first=*/false);
    EXPECT_EQ(off.counters.gap_tracker_key_mismatch, 4);
}

// The knob decides the signal, not the bands: the pair is the two lowest
// distinct bands in both arms, and every other field of the row is formed from
// the chosen signals alone.
TEST(TrackerModeledFirstTest, TheBandPairIsTheSameInBothArms) {
    const std::vector<Raw> raw{ { "L1C", kL1, 100.0 }, { "L1W", kL1, 140.0 },
                                { "L2W", kL2, 60.0 }, { "L5Q", kL5, 40.0 } };
    TrackerHarness on;
    TrackerHarness off;
    on.latch = off.latch = wLatch();
    const std::vector<PppObservation> ep{ makeObs("G13", raw, { "L1W", "L2W" }) };
    on.run(ep, /*modeled_first=*/true);
    off.run(ep, /*modeled_first=*/false);

    const RawSlipObs* a = on.row("G13");
    const RawSlipObs* b = off.row("G13");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->sig1[1], b->sig1[1]);   // band 1 in both
    EXPECT_EQ(a->sig2[1], b->sig2[1]);   // band 2 in both, not band 5
    EXPECT_EQ(a->sig1, "L1W");
    EXPECT_EQ(b->sig1, "L1C");
    EXPECT_EQ(a->sig2, "L2W");
    EXPECT_EQ(b->sig2, "L2W");
    // Different band-1 ambiguities, so the two arms' observables differ: the
    // knob is not a relabelling.
    EXPECT_NE(a->l1_c1_m, b->l1_c1_m);
}

// A raw phase without a matching code is not a tracker candidate in either arm,
// the latched one included: the observables need the code.
TEST(TrackerModeledFirstTest, ALatchedPhaseWithoutACodeIsNotChosen) {
    const std::vector<Raw> raw{ { "L1C", kL1, 100.0, true },
                                { "L1W", kL1, 140.0, false },   // no C1W
                                { "L2W", kL2, 60.0, true } };
    TrackerHarness h;
    h.latch = wLatch();
    h.run({ makeObs("G13", raw, { "L1W", "L2W" }) }, /*modeled_first=*/true);

    const RawSlipObs* r = h.row("G13");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sig1, "L1C");      // the only band-1 candidate
    EXPECT_EQ(r->sig2, "L2W");
    EXPECT_EQ(h.counters.gap_tracker_key_mismatch, 0);
}

// With no lookup bound (the call shape without a latch), the tracker stays on
// the lexical rule in both knob states and the counter stays at 0.  An unarmed
// field run whose mismatch count reads 0 on a mixed-signal network is therefore
// unwired, not quiet (standing invariant #4).
TEST(TrackerModeledFirstTest, NoLatchLookupLeavesTheLexicalRule) {
    TrackerHarness h;
    h.bind_latch = false;
    h.latch = wLatch();
    h.run({ makeObs("G13", bothBand1Codes(), { "L1W", "L2W" }) }, /*modeled_first=*/true);

    const RawSlipObs* r = h.row("G13");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sig1, "L1C");
    EXPECT_EQ(h.counters.gap_tracker_key_mismatch, 0);
}

// The counters sink is optional: a caller that passes none still gets the
// selection rule, so call sites that pass no counters still compile.
TEST(TrackerModeledFirstTest, TheCountersSinkIsOptional) {
    LogManager   log_manager;
    PppUtils     utils{ log_manager };
    RawSlipEpoch epoch;
    const std::vector<std::vector<PppObservation>> modeled{
        { makeObs("G13", bothBand1Codes(), { "L1W", "L2W" }) } };
    epoch.reset(1);
    const std::vector<bool> present{ true };
    const TrackerLatchLookup lookup =
        [](int, const std::string&, char band) -> std::string {
            return band == '1' ? "L1W" : (band == '2' ? "L2W" : std::string{});
        };
    networkComputeRawObservables(modeled, present, 1, utils, epoch,
                                 /*per_signal=*/false, /*tracker_modeled_first=*/true,
                                 /*cnt=*/nullptr, &lookup);
    ASSERT_EQ(epoch.rows.at(0).size(), 1u);
    EXPECT_EQ(epoch.rows.at(0)[0].sig1, "L1W");
}

// ===========================================================================
// gap_reentry_unreachable: the carried signal is tracked this epoch
// ===========================================================================

/// One station, one satellite, one shifted re-entry on the tracker's signals,
/// with the leveling carry keyed on other signals of the same two bands.
/// Nothing is opened for writing: the traces object is constructed over a
/// scratch path and its event stream stays closed, so emitEvent is a no-op.
struct UnreachableHarness {
    LogManager              log_manager;
    GnssConfig              config;
    std::vector<NetStation> stations;
    std::vector<bool>       present{ true };
    std::set<std::pair<int, std::string>> carry_slipped_ambs;
    RawSlipEpoch            raw;
    SlipQcParams            params;
    SlipQcCounters          counters;
    RelevelEpochState       relevel;
    std::map<std::tuple<int, std::string, std::string>, RelevelPend> pends;
    net::ArcTable  local_carry;
    double                  pl_now_s{ 270390.0 };
    double                  pl_max_gap_s{ 900.0 };
    SyncedEpoch             se;
    NetworkRunTraces        traces{ std::filesystem::temp_directory_path()
                                    / "gnss_tracker_modeled_first_test_unused" };
    std::shared_ptr<spdlog::logger> log{ std::make_shared<spdlog::logger>(
        "test_tracker_modeled_first", std::make_shared<spdlog::sinks::null_sink_mt>()) };

    UnreachableHarness() {
        stations.emplace_back();
        stations.front().name = "BSHM00ISR";
        stations.front().sm =
            std::make_unique<StateManager>(/*core_state_size=*/5, log_manager, config);
        se.time = GnssTime{ 2313, 270390.0 };
        raw.reset(1);
        params.gap_reentry = true;
        // The carried keys: the station's canonical signals, which are not the
        // ones the tracker named on the return epoch.
        addCarry("L1C");
        addCarry("L2W");
    }
    void addCarry(const char* sig, double age_s = 210.0) {
        LocalCarry lc{};
        lc.var         = 1.0e-5;
        lc.last_seen_s = pl_now_s - age_s;
        net::putArcIn(local_carry, std::make_tuple(0, std::string("G14"), std::string(sig)), &net::ArcState::amb) = lc;
    }
    /// The tracker's pair at re-entry, and the signals actually tracked now.
    void addResumption(const std::vector<std::string>& tracked_now) {
        GapResumption g;
        g.station    = 0;
        g.satellite  = "G14";
        g.sig1       = "L1L";
        g.sig2       = "L2L";
        g.gap_epochs = 7;
        g.measurable = true;
        g.shifted    = true;
        g.d_mw       = -1250.98;
        g.d_gf       = -276.67;
        raw.resumptions.push_back(std::move(g));
        RawSlipObs r;
        r.satellite = "G14";
        r.sig1      = "L1L";
        r.sig2      = "L2L";
        r.dual      = true;
        for (const auto& s : tracked_now) {
            RawSigObs so;
            so.sig = s;
            r.sigs.push_back(std::move(so));
        }
        raw.rows.at(0).push_back(std::move(r));
    }
    /// A per-signal return of @p signal while the satellite's row survived, with
    /// @p tracked_now the signals present this epoch.
    void addSigResumption(const std::string& signal, const std::string& partner,
                          const std::vector<std::string>& tracked_now) {
        SigResumption g;
        g.station         = 0;
        g.satellite       = "G14";
        g.signal          = signal;
        g.partner         = partner;
        g.partner_through = true;
        g.gap_epochs      = 7;
        g.measurable      = true;
        g.shifted         = true;
        g.d_gf            = -2.5;
        g.gate_gf         = 0.04;
        raw.sig_resumptions.push_back(std::move(g));
        RawSlipObs r;
        r.satellite = "G14";
        r.sig1      = "L1C";
        r.sig2      = "L2W";
        r.dual      = true;
        for (const auto& s : tracked_now) {
            RawSigObs so;
            so.sig = s;
            r.sigs.push_back(std::move(so));
        }
        raw.rows.at(0).push_back(std::move(r));
    }
    void run() {
        networkSlipDrainGapReentry(stations, present, /*i_lo=*/0, /*n_sta_total=*/1,
                                   /*carry_invariant=*/false, /*carry_warm=*/true,
                                   carry_slipped_ambs, raw, params, local_carry,
                                   pl_now_s, pl_max_gap_s, counters, se, traces, log,
                                   &relevel, &pends);
    }
    void runPerSignal() {
        params.gap_reentry_per_signal = true;
        networkSlipDrainGapReentryPerSignal(stations, present, /*i_lo=*/0,
                                            /*n_sta_total=*/1,
                                            /*carry_invariant=*/false, /*carry_warm=*/true,
                                            carry_slipped_ambs, raw, params, local_carry,
                                            pl_now_s, pl_max_gap_s, counters, se, traces,
                                            log, &relevel, &pends);
    }
};

// Both directions on one fixture.  Absent: the station tracks only L1L / L2L /
// L5Q this epoch, so the carried L1C / L2W arcs have no row at all and their
// return belongs to the per-signal tier, not to a response this tier declined.
// Present: L1C is tracked with its matching code alongside L1L, so the PASS-B
// inject folds that carry across the break while the response looks the band
// up under L1L and finds nothing.  Both arms have the same two no-carry arcs, so
// only the unreachable count differs.
TEST(TrackerModeledFirstTest, UnreachableCountsOnlyATrackedCarriedSignal) {
    UnreachableHarness absent;
    absent.addResumption({ "L1L", "L2L", "L5Q" });
    absent.run();
    EXPECT_EQ(absent.counters.gap_reentry_no_carry, 2);
    EXPECT_EQ(absent.counters.gap_reentry_unreachable, 0);

    UnreachableHarness present;
    present.addResumption({ "L1C", "L1L", "L2L" });
    present.run();
    EXPECT_EQ(present.counters.gap_reentry_no_carry, 2);
    // L1C is tracked, so the L1L arc counts; L2W is not, so the L2L arc does not.
    EXPECT_EQ(present.counters.gap_reentry_unreachable, 1);
}

// The presence list is optional: a posture without the per-signal tier gives the
// responder no signal set, and the counter then keeps its broader definition
// (every no-carry arc) rather than reporting zero.  Same fixture with no
// per-signal list.
TEST(TrackerModeledFirstTest, NoPerSignalListKeepsTheBroaderUnreachableCount) {
    UnreachableHarness h;
    h.addResumption({});          // row present, per-signal list empty
    h.run();
    EXPECT_EQ(h.counters.gap_reentry_no_carry, 2);
    EXPECT_EQ(h.counters.gap_reentry_unreachable, 2);
}

// The per-signal tier never feeds the counter.  Its key is the returning signal,
// so the tracker and the carry cannot name the band differently there.  A return
// on L2L at a station carrying L2W correctly finds no carry under L2L, and the
// modelled L2W carry it would otherwise be scored against was tracked through
// the break, so the case is not the key mismatch of the gap re-entry key fix.
TEST(TrackerModeledFirstTest, PerSignalTierNeverFeedsTheUnreachableCount) {
    UnreachableHarness h;
    // The carried, modelled band-2 signal is L2W, and it is tracked right now.
    h.addSigResumption("L2L", "L1C", { "L1C", "L2W", "L2L" });
    h.runPerSignal();
    EXPECT_EQ(h.counters.gap_sig.verdicts, 1);
    EXPECT_EQ(h.counters.gap_sig.no_carry, 1);   // no carry under L2L: correct
    EXPECT_EQ(h.counters.gap_reentry_unreachable, 0);

    // The row-level tier on the same shape does count it, so the exclusion is a
    // tier rule rather than a silenced counter.
    UnreachableHarness row;
    row.addResumption({ "L1C", "L1L", "L2L" });
    row.run();
    EXPECT_EQ(row.counters.gap_reentry_unreachable, 1);
}

}  // namespace
}  // namespace GnssProcessor
