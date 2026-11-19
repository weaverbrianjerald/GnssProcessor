/**
 * @file test_realtime_reaction_records.cpp
 * @brief What the user engine records when it reacts to an event of the correction
 *        stream, pinned as the code behaves today.
 *
 * The user engine answers many correction-stream events (a phase-bias counter step, a
 * bias that leaves or returns, a code-bias step, a declared level step, a change of the
 * broadcast record, an outage of the stream).  Each answer changes the filter at one edge
 * and should leave a count and an identity at both edges, so a run can show what it did.
 * These cases pin, through the engine's existing test accesses and the public interfaces
 * of the correction store, the snapshot format and the contract check:
 *   - the store inputs the admission and coverage paths read (one header per satellite, so
 *     a bias push moves the synchronisation horizon; the coverage line's 60 s window);
 *   - which reaction memories a snapshot carries across a restore, and that the format
 *     has no record for the declared level steps;
 *   - the reset, carry and translate answers to a counter step, to a bias that vanishes
 *     and returns, and to a satellite that returns on a new arc, with the counts each
 *     answer leaves;
 *   - the contract check's comparison of the code-bias pin pair, the satellite group-delay
 *     datum and the identity tokens;
 *   - the store side of a code-bias step, a code-bias flip and a declared level step;
 *   - the ephemeris jump's restart of every arc of the jumping satellite, the input that
 *     releases a held pair at the integer resolver.
 * A case whose expected count, identity or line does not exist in the code today is a
 * GTEST_SKIP that names what is missing.
 *
 * The two access structs below are the ones the engine already befriends.  Each is the
 * same definition, token for token, as in test_pb_hold_presence.cpp and
 * test_ephemeris_jump.cpp (C++ allows one class to be defined in several translation units
 * only when the definitions are identical): an edit of either struct must be made in both
 * files.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "filter/StateManager.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "realtime/RtStateSnapshot.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support/SsrStoreFixture.hpp"
#include "support/SyntheticNav.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seams this suite needs.
struct PbHoldPresenceTestAccess {
    static void initialize(RealTimeEngine& e, const SsrCorrectionStore& store) {
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
        e.ssr_store_override_ = &store;
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }
    /// One epoch of the phase-bias reaction (the hold seam, then the configured one).
    static void react(RealTimeEngine& e, const PppObservation& obs) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        ctx.modeled_obs = {obs};
        ctx.current_time = obs.time;
        e.resetOnPhaseBiasDisc(ctx);
    }
};

// House friend-shim idiom: expose only the engine seams this suite needs.
struct EphemerisJumpTestAccess {
    static void initialize(RealTimeEngine& e) {
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }
    static void setReceiver(RealTimeEngine& e, const Eigen::Vector3d& r) { e.ppp_state_.position = r; }
    /// One pass of the seam over an epoch holding @p sats, their orbits from the
    /// broadcast store unless @p live_orbit is false (an SP3-modelled epoch).
    static void react(RealTimeEngine& e, const std::vector<std::string>& sats, const GnssTime& t,
                      bool live_orbit = true) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{ raw };
        ctx.current_time = t;
        for (const auto& s : sats) {
            PppObservation o;
            o.satellite  = s;
            o.system     = System::Galileo;
            o.time       = t;
            o.live_orbit = live_orbit;
            ctx.modeled_obs.push_back(o);
        }
        e.reactToEphemerisJumps_(ctx);
    }
    static const EphemerisJumpLedger& ledger(RealTimeEngine& e) { return e.eph_jump_; }
    static long long reseeded(RealTimeEngine& e) { return e.eph_jump_reseeded_sats_; }
    static std::set<std::string> targets(const std::string& sat, bool is_pivot,
                                         const std::set<std::string>& state_sats) {
        return RealTimeEngine::ephemerisJumpTargets(sat, is_pivot, state_sats);
    }
};

namespace {

constexpr const char* kSat = "G05";
constexpr double kL1Bias = 0.25;      // phase bias of L1C (m)
constexpr double kL2Bias = -0.5;      // phase bias of L2W (m)
constexpr double kConvergedVar = 0.01;

/// The phase-bias answer the engine is configured with.
enum class Reaction { Reset, Carry, Translate };

/// signal -> (phase bias in m, discontinuity counter) of one phase-bias record.
using Biases = std::map<std::string, std::pair<double, int>>;

bool sameTime(const GnssTime& a, const GnssTime& b) { return a.week == b.week && a.tow == b.tow; }

/// A minimal valid snapshot: two core states and one ambiguity.
RtStateSnapshot smallSnapshot() {
    RtStateSnapshot s;
    s.snap_week     = 2313;
    s.snap_tow      = 261600.0;
    s.wall_utc      = "2026-11-18T00:00:00Z";
    s.fingerprint   = 0x0011223344556677ULL;
    s.core_size     = 2;
    s.state_size    = 3;
    s.generation_id = 7u;
    s.epoch_count   = 120;
    s.dyn = { { 2, "AMB", "G05_L1C" } };
    s.x = Eigen::VectorXd::Zero(3);
    s.x(0) = 1.5; s.x(1) = -2.25; s.x(2) = 12.5;
    s.P = Eigen::MatrixXd::Identity(3, 3) * 0.04;
    return s;
}

/// An S-record contract as a real-time user adopts it from the first receipt.
SbasisContract firstReceipt() {
    SbasisContract c;
    c.present = true;
    c.decl.reference_receiver_id = "AAAA";
    c.frame_id     = "IGS20";
    c.coord_epoch  = "2026.0";
    c.atx_id       = "igs20.atx";
    c.orbit_ref    = "CoM";
    c.cb_pin_pair  = { { "GPS", "C1C/C2W" } };
    c.sat_gf_datum = "broadcast_tgd";
    c.windup_mode  = "accumulated";
    return c;
}

bool mentions(const std::vector<std::string>& lines, const std::string& what) {
    for (const auto& l : lines)
        if (l.find(what) != std::string::npos) return true;
    return false;
}

class RealtimeReactionRecords : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    SsrCorrectionStore store;
    std::unique_ptr<RealTimeEngine> engine;
    int epoch = 0;

    void TearDown() override {
        LiveEphemerisStore::getInstance().clear();
        SsrCorrectionStore::getInstance().clear();
    }

    void loadConfig(const std::string& body) {
        const auto path = gnss_test::tempDir()
            / (std::string("rrr_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()
               + ".json");
        { std::ofstream out(path); out << body; }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
    }

    /// A GPS user engine whose phase-bias answer is @p r, with the bias age gate
    /// (processing.enable_ssr_bias_max_age, 60 s) on or off, holding two converged
    /// ambiguities of kSat (L1C and L2W).
    void buildPhaseEngine(Reaction r, bool age_gate) {
        const bool carry = (r == Reaction::Carry);
        const bool translate = (r == Reaction::Translate);
        std::ostringstream body;
        body << R"({
            "processing": {
                "mode": "ppp-rtk",
                "active_systems": ["GPS"],
                "enable_ssr_bias_max_age": )" << (age_gate ? "true" : "false") << R"(,
                "ssr_bias_max_age_s": 60.0,
                "ssr_phase_bias_disc_carry": )" << (carry ? "true" : "false") << R"(,
                "ssr_phase_bias_step_translate": )" << (translate ? "true" : "false") << R"(
            },
            "estimation": {
                "estimate_velocity": false,
                "estimate_phase_clock": false,
                "estimate_ifb": false,
                "estimate_sat_biases": false,
                "ambiguity_resolution": {"mode": 0},
                "between_satellite_sd": {"enabled": false}
            },
            "filter": {"initial_std": {
                "position": 1.0, "clock": 1.0, "troposphere": 0.1,
                "ionosphere": 1.0, "ambiguity": 100.0
            }}
        })";
        loadConfig(body.str());
        if (HasFatalFailure()) return;
        ASSERT_EQ(config.enableSsrBiasMaxAge(), age_gate);
        ASSERT_EQ(config.ssrPhaseBiasDiscCarry(), carry);
        ASSERT_EQ(config.ssrPhaseBiasStepTranslate(), translate);
        engine = std::make_unique<RealTimeEngine>(config, log);
        PbHoldPresenceTestAccess::initialize(*engine, store);
        states().addAmbiguity(amb("L1C"), -12.5, 0.1);
        states().addAmbiguity(amb("L2W"), 30.75, 0.1);
    }

    StateManager& states() { return PbHoldPresenceTestAccess::states(*engine); }
    static std::string amb(const std::string& sig) { return std::string(kSat) + "_" + sig; }
    GnssTime now() const { return GnssTime{2313, 261600.0 + 30.0 * epoch}; }
    double resetVar() const { return config.getAmbiguityStd() * config.getAmbiguityStd(); }

    /// One user epoch: kSat's phase-bias record (unheld, stamped now) with @p biases,
    /// then the reaction over an observation tracking @p tracked.
    void step(const Biases& biases,
              const std::vector<std::string>& tracked = {"L1C", "L2W"}) {
        SsrPhaseBias rec;
        rec.sat = kSat;
        rec.received_time = now();
        for (const auto& [sig, bd] : biases) {
            SsrPhaseBiasEntry e;
            e.bias_m = bd.first;
            e.discontinuity_counter = bd.second;
            rec.bias_per_signal[sig] = e;
        }
        SsrHeader h;
        h.epoch_time = now();
        h.system = System::GPS;
        store.pushPhaseBias(rec, h);
        PppObservation o{};
        o.satellite = kSat;
        o.system = System::GPS;
        o.time = now();
        for (const auto& sig : tracked) o.modeled[sig] = 2.2e7;
        PbHoldPresenceTestAccess::react(*engine, o);
        ++epoch;
    }

    int index(const std::string& sig) {
        const int i = states().getAmbiguityIndex(amb(sig));
        EXPECT_GE(i, 0) << sig;
        return i;
    }
    double var(const std::string& sig) { return states().getCovariance()(index(sig), index(sig)); }
    double value(const std::string& sig) { return states().getStateVector()(index(sig)); }
    std::uint64_t gen(const std::string& sig) { return states().getAmbiguityGeneration(amb(sig)); }
    bool pending(const std::string& sig) { return states().isAmbiguityPendingReseed(amb(sig)); }

    /// Put an ambiguity back to a converged variance (between two edges).
    void reconverge(const std::string& sig) {
        const int i = index(sig);
        Eigen::MatrixXd P = states().getCovariance();
        P.row(i).setZero();
        P.col(i).setZero();
        P(i, i) = kConvergedVar;
        states().setCovariance(P);
    }

    static Biases both() { return { { "L1C", { kL1Bias, 0 } }, { "L2W", { kL2Bias, 0 } } }; }
    static Biases withoutL1() { return { { "L2W", { kL2Bias, 0 } } }; }
};

} // namespace

// ===========================================================================
// Admission: the observation/correction synchronisation and skip-to-latest
// ===========================================================================

// The store keeps one header per satellite, overwritten by every record kind, and the
// synchronisation horizon is the newest of those headers.  A phase-bias push therefore
// holds the horizon at its own epoch while the satellite's clock is too old to apply.
TEST_F(RealtimeReactionRecords, SyncHorizonFollowsTheNewestHeaderOfAnyRecordKind) {
    gnss_test::SsrStoreState st = gnss_test::networkSsrState({ "G05", "G07" });
    gnss_test::ageSatellite(st, "G05", 40.0, gnss_test::SsrRecordKind::OrbitClock);
    gnss_test::ageSatellite(st, "G07", 40.0, gnss_test::SsrRecordKind::OrbitClock);
    gnss_test::writeSsrState(store, st);

    const auto newest = store.newestSsrEpoch();
    ASSERT_TRUE(newest.has_value());
    EXPECT_TRUE(sameTime(*newest, st.epoch)) << "the bias headers, not the clocks, set the horizon";
    const auto g05 = store.getSsrEpoch("G05");
    ASSERT_TRUE(g05.has_value());
    EXPECT_TRUE(sameTime(*g05, st.epoch));
    const auto clk = store.getOrbitClock("G05", st.epoch, 30.0);
    EXPECT_TRUE(!clk || !clk->has_clock) << "the 40 s old clock does not apply within 30 s";
}

TEST_F(RealtimeReactionRecords, SyncOutageAndRecoveryNeedCounts) {
    GTEST_SKIP() << "Missing counts: the observation/correction sync hold latches an outage with "
                    "one warning and clears it with no line and no count (no outage or recovery "
                    "count exists); its hold budget is measured on the wall clock, so no test can "
                    "drive it deterministically.";
}

TEST_F(RealtimeReactionRecords, KeepLatestOverwriteNeedsACount) {
    GTEST_SKIP() << "Missing count: while the sync hold waits, each new epoch overwrites the held "
                    "one with no line and no count, so a lost epoch is not recorded.";
}

TEST_F(RealtimeReactionRecords, SkipToLatestNeedsACount) {
    GTEST_SKIP() << "Missing count: skip-to-latest drops a late epoch with a debug line only; no "
                    "count of skipped epochs exists, and the lag is measured on the wall clock.";
}

// ===========================================================================
// Reaction memories across a snapshot restore
// ===========================================================================

// The three discontinuity memories (phase "sat|signal", code "CB|sat|signal", clock
// "SC|sat") share one map and travel in the D records, with the two freshness-flip
// memories beside them, so a restored user does not read the next counter as a first
// sighting.
TEST_F(RealtimeReactionRecords, SnapshotCarriesTheDiscontinuityAndFlipMemories) {
    RtStateSnapshot s = smallSnapshot();
    s.ssr_disc = { { "G05|L1C", 2 }, { "CB|G05|C1C", 1 }, { "SC|G05", 3 } };
    s.phase_bias_applied = { { "G05|L1C", true } };
    s.code_bias_applied  = { { "CB|G05|C1C", false } };
    std::ostringstream os;
    RtStateSnapshotIO::serialize(os, s);
    std::istringstream is(os.str());
    RtStateSnapshot r;
    std::string err;
    ASSERT_TRUE(RtStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_EQ(r.ssr_disc, s.ssr_disc);
    EXPECT_EQ(r.phase_bias_applied, s.phase_bias_applied);
    EXPECT_EQ(r.code_bias_applied, s.code_bias_applied);

    StateManager sm{ 5, log, config };
    sm.setSsrDiscontinuityMemory(r.ssr_disc);
    EXPECT_FALSE(sm.noteSsrDiscontinuity("CB|G05|C1C", 1)) << "an unchanged code counter is no step";
    EXPECT_TRUE(sm.noteSsrDiscontinuity("SC|G05", 4)) << "the clock counter steps against the restored value";
    EXPECT_TRUE(sm.noteSsrDiscontinuity("G05|L1C", 3)) << "the phase counter steps against the restored value";
}

// The record tags a fully populated snapshot writes.  None holds the declared level-step
// totals the engine compares against, so a restored user reads the store's totals at its
// first epoch as a first sighting, and a step made across the outage is not answered.
TEST_F(RealtimeReactionRecords, SnapshotFormatHasNoRecordForDeclaredLevelSteps) {
    RtStateSnapshot s = smallSnapshot();
    s.ssr_disc = { { "G05|L1C", 2 } };
    s.phase_bias_applied = { { "G05|L1C", true } };
    s.code_bias_applied  = { { "CB|G05|C1C", true } };
    s.init_latches = { { "isb", true } };
    std::ostringstream os;
    RtStateSnapshotIO::serialize(os, s);
    std::istringstream is(os.str());
    std::string line;
    std::getline(is, line);   // the header line
    std::set<std::string> tags;
    while (std::getline(is, line)) {
        if (line.rfind("END,", 0) == 0) break;
        tags.insert(line.substr(0, line.find(',')));
    }
    EXPECT_EQ(tags, (std::set<std::string>{ "M", "G", "K", "X", "P", "D", "F", "C", "I" }));
}

// ===========================================================================
// The clock-discontinuity reaction
// ===========================================================================

TEST_F(RealtimeReactionRecords, ClockDiscIsbEventIsRecordedBeforeItsTest) {
    GTEST_SKIP() << "Defect: the clock-discontinuity reaction records its reinflate_isb trace "
                    "event before it tests estimation.clock_disc_isb_reinflate_std > 0, so with "
                    "the std at 0 (the default) the trace names an ISB re-inflation that does not "
                    "happen, and no skipped event exists.  The reaction is a private engine member "
                    "that no existing test access reaches.";
}

// ===========================================================================
// Phase-bias counter step, reset answer
// ===========================================================================

// A counter step on one unheld signal re-floats that signal only and restarts its arc
// (a new continuity generation).  No run-end count records the reset: the hold, carry,
// translate and flip censuses all stay at zero.
TEST_F(RealtimeReactionRecords, UnheldDiscResetRefloatsOnlyTheSteppedSignal) {
    ASSERT_NO_FATAL_FAILURE(buildPhaseEngine(Reaction::Reset, /*age_gate=*/false));
    step(both());                       // first sighting of both counters: recorded only
    step(both());
    EXPECT_NEAR(var("L1C"), kConvergedVar, 1e-12) << "no step yet";
    const std::uint64_t g_l1 = gen("L1C");
    const std::uint64_t g_l2 = gen("L2W");

    step({ { "L1C", { kL1Bias, 1 } }, { "L2W", { kL2Bias, 0 } } });
    EXPECT_NEAR(var("L1C"), resetVar(), 1e-6 * resetVar());
    EXPECT_TRUE(pending("L1C"));
    EXPECT_NE(gen("L1C"), g_l1) << "the reset restarts the arc";
    EXPECT_NEAR(var("L2W"), kConvergedVar, 1e-12) << "the unstepped signal keeps its convergence";
    EXPECT_FALSE(pending("L2W"));
    EXPECT_EQ(gen("L2W"), g_l2);

    EXPECT_EQ(engine->pbHoldCounters().cells, 0u);
    EXPECT_EQ(engine->pbDiscCarryCounters().events, 0u);
    EXPECT_EQ(engine->pbStepTranslateCounters().cells, 0u);
    EXPECT_EQ(engine->rosterConsumerCounters().flips, 0);
}

TEST_F(RealtimeReactionRecords, UnheldDiscResetNeedsARunEndCount) {
    GTEST_SKIP() << "Missing count: an unheld discontinuity reset writes one info line per key "
                    "but no run-end count of these resets exists, while the hold, carry and "
                    "translate answers each have one.";
}

// ===========================================================================
// A bias that vanishes: carry against reset
// ===========================================================================

// Under the carry the vanish moves the whole applied bias into the ambiguity
// (amb - (0 - bias)), keeps the variance and counts one carried flip in the carry ledger.
TEST_F(RealtimeReactionRecords, CarriedVanishShiftsByTheWholeBiasAndCountsInTheCarryLedger) {
    ASSERT_NO_FATAL_FAILURE(buildPhaseEngine(Reaction::Carry, /*age_gate=*/true));
    step(both());
    step(both());
    const double l1_before = value("L1C");
    const double l2_before = value("L2W");
    step(withoutL1());                  // L1C leaves the record; the receiver still tracks it
    EXPECT_NEAR(value("L1C"), l1_before + kL1Bias, 1e-12);
    EXPECT_NEAR(var("L1C"), kConvergedVar, 1e-12) << "the carry keeps the convergence";
    EXPECT_FALSE(pending("L1C"));
    EXPECT_DOUBLE_EQ(value("L2W"), l2_before);
    const auto& c = engine->pbDiscCarryCounters();
    EXPECT_EQ(c.flips, 1u);
    EXPECT_EQ(c.flip_carried, 1u);
    EXPECT_EQ(c.disc_bumps, 0u);
    EXPECT_EQ(c.events, 1u);
    EXPECT_TRUE(c.closureHolds());
}

// The reset answer to the same vanish re-floats L1C and records the flip in the census:
// one phase flip to stale, unattributable (no declared roster), with its ARC row.
TEST_F(RealtimeReactionRecords, ResetPathRecordsAVanishInTheFlipCensus) {
    ASSERT_NO_FATAL_FAILURE(buildPhaseEngine(Reaction::Reset, /*age_gate=*/true));
    step(both());
    step(both());
    step(withoutL1());
    EXPECT_NEAR(var("L1C"), resetVar(), 1e-6 * resetVar());
    EXPECT_TRUE(pending("L1C"));
    EXPECT_NEAR(var("L2W"), kConvergedVar, 1e-12);
    const auto& r = engine->rosterConsumerCounters();
    EXPECT_EQ(r.flips, 1);
    EXPECT_EQ(r.flips_phase, 1);
    EXPECT_EQ(r.flip_to_stale, 1);
    EXPECT_EQ(r.flip_no_decl, 1);
    EXPECT_EQ(r.flip_events, 1);
    EXPECT_EQ(r.flip_suppressed, 0);
}

TEST_F(RealtimeReactionRecords, CarriedVanishIsMissingFromTheFlipCensus) {
    GTEST_SKIP() << "Defect: with processing.ssr_phase_bias_disc_carry armed, the carry answers a "
                    "freshness flip without the flip census the reset path feeds, so the vanish "
                    "that the reset path records as one flip with its ARC row and attribution "
                    "class leaves rosterConsumerCounters().flips at 0 and writes no ARC row.";
}

// ===========================================================================
// A bias that vanishes and returns: translate
// ===========================================================================

// The step translation has no applied value on one side of a freshness flip, so it takes
// the flip, in either direction, as a declared first sighting and resets the ambiguity.
TEST_F(RealtimeReactionRecords, TranslateTakesEveryFreshnessFlipAsAFirstSightingReset) {
    ASSERT_NO_FATAL_FAILURE(buildPhaseEngine(Reaction::Translate, /*age_gate=*/true));
    step(both());
    step(both());
    step(withoutL1());                  // fresh -> stale
    EXPECT_NEAR(var("L1C"), resetVar(), 1e-6 * resetVar());
    EXPECT_TRUE(pending("L1C"));
    const auto& c = engine->pbStepTranslateCounters();
    EXPECT_EQ(c.reset_first_sight, 1u);
    EXPECT_EQ(c.declared_seen, 1u);

    reconverge("L1C");
    step(both());                       // stale -> fresh
    EXPECT_NEAR(var("L1C"), resetVar(), 1e-6 * resetVar());
    EXPECT_EQ(c.reset_first_sight, 2u);
    EXPECT_EQ(c.declared_seen, 2u);
    EXPECT_EQ(c.translate_declared, 0u);
    EXPECT_EQ(c.translate_undeclared, 0u);
    EXPECT_TRUE(c.closureHolds());
    EXPECT_NEAR(var("L2W"), kConvergedVar, 1e-12) << "L2W never flipped";
}

TEST_F(RealtimeReactionRecords, TranslateFlipNeedsItsOwnCountAndRecord) {
    GTEST_SKIP() << "Missing count: the step translation folds a freshness flip into "
                    "declared_seen and reset_first_sight, so a first sighting and a flip share "
                    "one count; no flip count of its own exists, and the flip census and its ARC "
                    "row are not fed.";
}

// ===========================================================================
// A satellite that returns on a new arc: carry
// ===========================================================================

// The carry ledger keeps a key's last applied value across a gap.  A satellite that sets,
// loses its ambiguity and returns on a new arc with a counter step is shifted by the whole
// bias change of the gap, which the new arc never saw.
TEST_F(RealtimeReactionRecords, CarryShiftsAReturningArcByTheBiasChangeAcrossTheGap) {
    ASSERT_NO_FATAL_FAILURE(buildPhaseEngine(Reaction::Carry, /*age_gate=*/false));
    const Biases before = { { "L1C", { kL1Bias, 0 } } };
    step(before, { "L1C" });
    step(before, { "L1C" });

    epoch += 10;                        // the satellite is below the horizon
    ASSERT_TRUE(states().removeAmbiguity(amb("L1C")));
    states().addAmbiguity(amb("L1C"), 3.0, 0.1);   // the new arc
    step({ { "L1C", { kL1Bias + 0.60, 1 } } }, { "L1C" });

    EXPECT_NEAR(value("L1C"), 3.0 - 0.60, 1e-12);
    EXPECT_NEAR(var("L1C"), kConvergedVar, 1e-12);
    const auto& c = engine->pbDiscCarryCounters();
    EXPECT_EQ(c.disc_bumps, 1u);
    EXPECT_EQ(c.disc_carried, 1u);
    EXPECT_EQ(c.reset_first_sight, 0u);
    EXPECT_TRUE(c.closureHolds());
}

TEST_F(RealtimeReactionRecords, CrossArcShiftNeedsACount) {
    GTEST_SKIP() << "Missing count: neither the carry nor the translate ledger forgets a key, and "
                    "no count or identity records a shift of an ambiguity that was added after "
                    "the ledger's last entry for its key.";
}

TEST_F(RealtimeReactionRecords, PartnerRefloatHeldPairEraseNeedsALineAndACount) {
    GTEST_SKIP() << "Missing line and count: with refloat_partner_signal armed, a carried shift "
                    "restarts the whole satellite and the integer resolver forgets its held pairs "
                    "with no line and no count (only the partner refloat itself is logged).";
}

// ===========================================================================
// The stream's contract fingerprint
// ===========================================================================

// The contract check itself reports a change of the code-bias pin pair against an
// expectation adopted from an earlier receipt.
TEST_F(RealtimeReactionRecords, ContractCheckReportsACodeBiasPinChange) {
    const SbasisContract expected = firstReceipt();
    SbasisContract second = firstReceipt();
    second.cb_pin_pair["GPS"] = "C1W/C2W";
    std::vector<std::string> mismatches;
    EXPECT_FALSE(NetworkProductReader::checkContract(second, expected, &mismatches));
    EXPECT_TRUE(mentions(mismatches, "cb_pin_pair[GPS]")) << ::testing::PrintToString(mismatches);
}

// The satellite group-delay datum has no reader: a change of it alone passes the check.
TEST_F(RealtimeReactionRecords, ContractCheckIgnoresTheSatelliteGroupDelayDatum) {
    const SbasisContract expected = firstReceipt();
    SbasisContract second = firstReceipt();
    second.sat_gf_datum = "zero";
    std::vector<std::string> mismatches;
    EXPECT_TRUE(NetworkProductReader::checkContract(second, expected, &mismatches))
        << ::testing::PrintToString(mismatches);
    EXPECT_TRUE(mismatches.empty());
}

TEST_F(RealtimeReactionRecords, RealtimeTailNeverChecksACodeBiasPinOnlyChange) {
    GTEST_SKIP() << "Missing check: the real-time tail re-runs the contract check only when its "
                    "fingerprint of the S record changes, and the fingerprint leaves out cbpin, "
                    "gfpin, sat_gf_datum, the pbhold tokens and three disc tokens, so a receipt "
                    "that changes only the code-bias pin pair is never compared.  The fingerprint "
                    "is local to the real-time driver and no mismatch count exists.";
}

// Frame, coordinate epoch, antenna file and orbit reference are compared only against a
// non-empty expectation: a user without typed pins compares them with its first receipt
// (a mid-stream change is reported) and never with its own configuration.
TEST_F(RealtimeReactionRecords, IdentityTokensAreComparedOnlyAgainstANonEmptyExpectation) {
    const SbasisContract adopted = firstReceipt();
    SbasisContract moved = firstReceipt();
    moved.frame_id = "ITRF2014";
    std::vector<std::string> mismatches;
    EXPECT_FALSE(NetworkProductReader::checkContract(moved, adopted, &mismatches));
    EXPECT_TRUE(mentions(mismatches, "frame_id")) << ::testing::PrintToString(mismatches);

    SbasisContract no_pins = firstReceipt();
    no_pins.frame_id.clear();
    no_pins.coord_epoch.clear();
    no_pins.atx_id.clear();
    no_pins.orbit_ref.clear();
    SbasisContract other = firstReceipt();
    other.frame_id    = "ITRF2014";
    other.coord_epoch = "2010.0";
    other.atx_id      = "igs14.atx";
    other.orbit_ref   = "APC";
    mismatches.clear();
    EXPECT_TRUE(NetworkProductReader::checkContract(other, no_pins, &mismatches))
        << ::testing::PrintToString(mismatches);
}

TEST_F(RealtimeReactionRecords, FirstReceiptIdentityLineIsMissing) {
    GTEST_SKIP() << "Missing line: no line at the first receipt names the frame, coordinate "
                    "epoch, antenna file and orbit reference of the stream beside the user's own "
                    "antenna file; the verified-contract line names only the reference receiver, "
                    "the a-priori product, the generation id and the wind-up mode.";
}

// ===========================================================================
// The coverage line
// ===========================================================================

// The per-epoch coverage line counts records with the store's 60 s default window, while
// the clock is applied within processing.ssr_max_age_s (30 s): a 45 s old clock is
// "in store" on the line and not applied.
TEST_F(RealtimeReactionRecords, CoverageCountsAClockTheApplyWindowRefuses) {
    gnss_test::SsrStoreState st = gnss_test::networkSsrState({ "G05" });
    gnss_test::ageSatellite(st, "G05", 45.0, gnss_test::SsrRecordKind::OrbitClock);
    gnss_test::writeSsrState(store, st);

    const auto cov = store.coverageFor({ "G05" }, st.epoch);
    EXPECT_EQ(cov.total, 1);
    EXPECT_EQ(cov.clock, 1);
    EXPECT_EQ(cov.orbit, 1);
    const auto clk = store.getOrbitClock("G05", st.epoch, 30.0);
    EXPECT_TRUE(!clk || !clk->has_clock);
    EXPECT_EQ(store.coverageFor({ "G05" }, st.epoch, 30.0).clock, 0)
        << "the window is the only difference";
}

// ===========================================================================
// Code-bias step, code-bias flip, declared level step: the store side
// ===========================================================================

// The code-bias reaction reads the ungated counter of a (satellite, code) pair: a network
// record carries it and a step of it is read back; a decoded stream carries none, so it
// can never drive the reaction.  The engine keys its memory "CB|sat|code".
TEST_F(RealtimeReactionRecords, CodeBiasCounterComesOnlyFromANetworkRecord) {
    gnss_test::SsrStoreState st = gnss_test::networkSsrState({ "G05" });
    gnss_test::writeSsrState(store, st);
    ASSERT_EQ(store.getCodeBiasDiscontinuity("G05", "C1C"), std::optional<int>(0));
    StateManager sm{ 5, log, config };
    EXPECT_FALSE(sm.noteSsrDiscontinuity("CB|G05|C1C", 0)) << "first sighting: recorded only";

    gnss_test::bumpCodeDiscontinuity(st, "G05", "C1C");
    gnss_test::writeSsrState(store, st);
    const auto c = store.getCodeBiasDiscontinuity("G05", "C1C");
    ASSERT_EQ(c, std::optional<int>(1));
    EXPECT_TRUE(sm.noteSsrDiscontinuity("CB|G05|C1C", *c));
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G05", "C2W"), std::optional<int>(0))
        << "the other code signal did not step";

    gnss_test::writeSsrState(store, gnss_test::decodedSsrState({ "G05" }));
    EXPECT_FALSE(store.getCodeBiasDiscontinuity("G05", "C1C").has_value());
}

TEST_F(RealtimeReactionRecords, CodeBiasDiscEngineCaseNeedsACount) {
    GTEST_SKIP() << "Missing count: the code-bias discontinuity reaction writes an info line and "
                    "a trace event but no run-end count of its events; its effect shows only in "
                    "the state manager's raised and kept totals, which every code-bias "
                    "re-inflation shares.  The reaction is also a private engine member that no "
                    "existing test access reaches.";
}

// A code bias that ages out of the gated window is the flip's only edge: the ungated
// counter the discontinuity path reads does not move, so only the freshness memory sees
// the step of the applied bias.
TEST_F(RealtimeReactionRecords, AgedCodeBiasMovesOnlyTheFreshnessEdge) {
    constexpr double kGatedAgeS = 120.0;   // ssr_bias_max_age_s 60 times the hold factor 2
    gnss_test::SsrStoreState st = gnss_test::networkSsrState({ "G05" });
    gnss_test::writeSsrState(store, st);
    EXPECT_TRUE(store.hasCodeBias("G05", "C1C", st.epoch, kGatedAgeS));

    gnss_test::ageSatellite(st, "G05", 150.0, gnss_test::SsrRecordKind::CodeBias);
    gnss_test::writeSsrState(store, st);
    EXPECT_FALSE(store.hasCodeBias("G05", "C1C", st.epoch, kGatedAgeS)) << "fresh -> stale";
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G05", "C1C"), std::optional<int>(0))
        << "the ungated counter is unchanged";

    gnss_test::ageSatellite(st, "G05", 0.0, gnss_test::SsrRecordKind::CodeBias);
    gnss_test::writeSsrState(store, st);
    EXPECT_TRUE(store.hasCodeBias("G05", "C1C", st.epoch, kGatedAgeS)) << "stale -> fresh";
    EXPECT_EQ(store.getCodeBiasDiscontinuity("G05", "C1C"), std::optional<int>(0));
}

// The declared level steps are running totals per satellite (clock, and per code signal),
// which the engine differences between epochs; a clear forgets them.
TEST_F(RealtimeReactionRecords, DeclaredLevelStepsAreRunningTotalsPerSatellite) {
    store.addDeclaredLevelSteps("G05", 0.10, { { "C1C", 0.30 } });
    store.addDeclaredLevelSteps("G05", -0.04, { { "C1C", 0.05 }, { "C2W", -0.20 } });
    const auto d = store.getDeclaredLevelSteps("G05");
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(d->clock_m, 0.06, 1e-12);
    EXPECT_NEAR(d->bias_m.at("C1C"), 0.35, 1e-12);
    EXPECT_NEAR(d->bias_m.at("C2W"), -0.20, 1e-12);
    EXPECT_EQ(d->n, 2);
    EXPECT_FALSE(store.getDeclaredLevelSteps("G07").has_value()) << "no step declared for G07";
    store.clear();
    EXPECT_FALSE(store.getDeclaredLevelSteps("G05").has_value());
}

TEST_F(RealtimeReactionRecords, RefusedLevelStepNeedsAReadoutAndAnIdentity) {
    GTEST_SKIP() << "Missing readout and identity: a refused declared level step (no ionosphere "
                    "state, a frozen row, or no ambiguity) increments a private count that is "
                    "printed only in the run-end line and has no accessor, and no line or event "
                    "names the refused satellite.";
}

// ===========================================================================
// Ephemeris jump with a held pair
// ===========================================================================

// A 100 ns clock change of E04's broadcast record (about 30 m of modelled range) re-seeds
// E04: every ambiguity of E04 is reset and takes a new continuity generation, which is the
// signal on which the integer resolver forgets a held pair.  E05 keeps its arcs.
TEST_F(RealtimeReactionRecords, EphemerisJumpRestartsEveryArcOfTheJumpingSatellite) {
    loadConfig(R"({
        "processing": {
            "mode": "ppp-rtk",
            "active_systems": ["GPS", "Galileo"],
            "ephemeris_jump_reseed": true
        },
        "estimation": {
            "estimate_velocity": false,
            "estimate_phase_clock": false,
            "estimate_ifb": false,
            "estimate_sat_biases": true,
            "ambiguity_resolution": {"mode": 0},
            "between_satellite_sd": {"enabled": false}
        },
        "filter": {"initial_std": {
            "position": 10.0, "clock": 100.0, "troposphere": 0.1,
            "ionosphere": 1.0, "ambiguity": 100.0, "sat_bias": 10.0
        }}
    })");
    ASSERT_TRUE(config.ephemerisJumpReseed());
    engine = std::make_unique<RealTimeEngine>(config, log);
    EphemerisJumpTestAccess::initialize(*engine);
    StateManager& sm = EphemerisJumpTestAccess::states(*engine);

    const double toc_s = gnss_test::syntheticNavDayStart() + 7200.0;
    const GnssTime t0 = gnss_test::syntheticNavTime(toc_s);
    const GnssTime t1 = gnss_test::syntheticNavTime(toc_s + 1.0);
    const BroadcastEphemeris e04 = gnss_test::galileoNav(4, toc_s).build();
    const BroadcastEphemeris e05 = gnss_test::galileoNav(5, toc_s).build();
    BroadcastEphemeris e04_moved = e04;
    e04_moved.kepler.af0 += 1.0e-7;
    const auto at_t0 = LiveEphemerisStore::evaluateRecord(e04, t0);
    ASSERT_TRUE(at_t0.has_value());
    EphemerisJumpTestAccess::setReceiver(*engine, at_t0->position.normalized() * 6371000.0);

    const std::vector<std::string> keys = { "E04_L1C", "E04_L5Q", "E05_L1C" };
    sm.addIonosphereState("E04", -266.0);
    sm.addIonosphereState("E05", -266.0);
    for (const auto& k : keys) sm.addAmbiguity(k, -270.0, 0.02);
    LiveEphemerisStore::getInstance().clear();
    SsrCorrectionStore::getInstance().clear();

    auto& eph = LiveEphemerisStore::getInstance();
    eph.insert(e04, /*from_igs=*/false);
    eph.insert(e05, /*from_igs=*/true);
    EphemerisJumpTestAccess::react(*engine, { "E04", "E05" }, t0);   // first sightings
    std::map<std::string, std::uint64_t> gen_before;
    for (const auto& k : keys) {
        gen_before[k] = sm.getAmbiguityGeneration(k);
        EXPECT_FALSE(sm.isAmbiguityPendingReseed(k)) << k;
    }

    eph.insert(e04_moved, /*from_igs=*/true);   // same issue of data, the IGS record wins
    EphemerisJumpTestAccess::react(*engine, { "E04", "E05" }, t1);

    const auto& c = EphemerisJumpTestAccess::ledger(*engine).counters();
    EXPECT_EQ(c.jumps, 1u);
    EXPECT_NEAR(c.max_abs_jump_m, Constants::SPEED_OF_LIGHT * 1.0e-7, 1e-4);
    EXPECT_EQ(EphemerisJumpTestAccess::reseeded(*engine), 1);
    const double reset_var = config.getAmbiguityStd() * config.getAmbiguityStd();
    for (const char* k : { "E04_L1C", "E04_L5Q" }) {
        EXPECT_TRUE(sm.isAmbiguityPendingReseed(k)) << k;
        EXPECT_NE(sm.getAmbiguityGeneration(k), gen_before[k]) << k << ": the arc restarts";
        EXPECT_NEAR(sm.getStateVariance(sm.getAmbiguityIndex(k)), reset_var, 1e-6 * reset_var) << k;
    }
    EXPECT_FALSE(sm.isAmbiguityPendingReseed("E05_L1C"));
    EXPECT_EQ(sm.getAmbiguityGeneration("E05_L1C"), gen_before["E05_L1C"]) << "E05 keeps its arc";
}

} // namespace GnssProcessor
