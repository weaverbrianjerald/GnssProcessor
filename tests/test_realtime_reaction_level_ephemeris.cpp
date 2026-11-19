/**
 * @file test_realtime_reaction_level_ephemeris.cpp
 * @brief The user engine's reactions to four correction-stream events, pinned as the code
 *        behaves today at both moments: when the reaction starts and when the state is
 *        trusted again.
 *
 * The events are a change of the broadcast record that cannot be tested (the old record is
 * outside its fit window), the satellite phase bias dropping its integer flag on a held
 * signal and returning it, and a satellite leaving the precise products and returning.  A
 * declared level step with a held pair is in the list too; its case is skipped (see it).
 *
 * Each case drives production code through a seam the neighbouring suites already use: the
 * broadcast-jump seam of the engine through the friend shim of test_ephemeris_jump.cpp,
 * the measurement model's state pre-pass through computeMeasurementUpdate, and the
 * eligibility rule through its public free function.  Where the event needs a counter or an
 * identity the code does not keep, the case is a skip that names what is missing, so the
 * gap stays visible in the test list.
 */

#include <gtest/gtest.h>

#include "realtime/EphemerisJumpLedger.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "measurement/RowRules.hpp"
#include "common/Constants.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#include "support/ModelStack.hpp"     // synthetic observations and the single-receiver model stack
#include "support/TestTempDir.hpp"    // per-process, per-test scratch directory

namespace GnssProcessor {

// The engine's broadcast-jump seam is private; RealTimeEngine.hpp befriends this type,
// which tests/test_ephemeris_jump.cpp defines.  A class defined in two translation units
// must be token for token identical, so this definition repeats that one exactly and must
// change only together with it.
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

// ===========================================================================
// Broadcast records: a real Galileo I/NAV set (E04, IODnav 51), the record the
// ephemeris-jump suite uses, so the geometry is known to evaluate.
// ===========================================================================

constexpr double kPi = Constants::PI;
const GnssTime kToe{2420, 106800.0};

BroadcastEphemeris e04() {
    BroadcastEphemeris eph;
    eph.satellite = "E04";
    eph.system    = System::Galileo;
    eph.iode      = 51;
    KeplerianEphemeris& k = eph.kepler;
    k.toe = kToe;
    k.toc = kToe;
    k.sqrt_a    = 5440.607572555542;
    k.e         = 0.00040867005009204149;
    k.m0        = 0.67652543960139155 * kPi;
    k.i0        = 0.30822082795202732 * kPi;
    k.omega     = -0.1767868883907795 * kPi;
    k.omega0    = -0.77682787552475929 * kPi;
    k.omega_dot = -1.8628725229064003e-09 * kPi;
    k.idot      = 3.205968823749572e-11 * kPi;
    k.delta_n   = 1.0739995559561066e-09 * kPi;
    k.cuc = -2.1960586309432983e-06;
    k.cus = 4.6659260988235474e-06;
    k.crc = 238.90625;
    k.crs = -45.8125;
    k.cic = -2.7939677238464355e-08;
    k.cis = 6.5192580223083496e-08;
    k.af0 = -0.00015625293599441648;
    k.af1 = -8.9244167611468583e-12;
    k.af2 = 0.0;
    return eph;
}

/// The next issue three hours later (IODnav 69).  The Galileo fit window is two hours, so at
/// the new record's epoch the old one cannot be evaluated and the change is undecided.
BroadcastEphemeris e04ThreeHoursLater() {
    BroadcastEphemeris eph = e04();
    eph.iode = 69;
    eph.kepler.toe.tow += 3 * 3600.0;
    eph.kepler.toc.tow += 3 * 3600.0;
    return eph;
}

const GnssTime kLater{2420, kToe.tow + 3 * 3600.0};

/// A ground point directly below the satellite at @p t.
Eigen::Vector3d receiverBelow(const BroadcastEphemeris& eph, const GnssTime& t) {
    const auto s = LiveEphemerisStore::evaluateRecord(eph, t);
    return s->position.normalized() * 6371000.0;
}

/// A consumer engine with the broadcast-jump response armed and E04 converged under the
/// first record: tight ionosphere, satellite-bias and ambiguity states.
struct EphemerisEngine {
    LogManager log;
    GnssConfig config;
    std::unique_ptr<RealTimeEngine> engine;

    static constexpr double kIonoVar = 0.09;      // (0.3 m)^2
    static constexpr double kSatBiasVar = 6.25e-4; // (0.025 m)^2
    static constexpr double kAmbVar = 4.0e-4;      // (0.02 m)^2

    ~EphemerisEngine() {
        LiveEphemerisStore::getInstance().clear();
        SsrCorrectionStore::getInstance().clear();
    }

    bool build() {
        const auto path = gnss_test::tempDir() / "reaction_level_ephemeris_engine.json";
        {
            std::ofstream out(path);
            out << R"({
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
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!loaded || !config.ephemerisJumpReseed()) return false;
        engine = std::make_unique<RealTimeEngine>(config, log);
        EphemerisJumpTestAccess::initialize(*engine);
        EphemerisJumpTestAccess::setReceiver(*engine, receiverBelow(e04(), kToe));

        states().addIonosphereState("E04", -266.0);
        states().addSatBiasStateWithVariance("E04|C5Q", 10.0, 1.0);
        states().addAmbiguity("E04_L1C", -270.0, 0.02);
        Eigen::MatrixXd P = states().getCovariance();
        P(iono(), iono()) = kIonoVar;
        P(satBias(), satBias()) = kSatBiasVar;
        P(amb(), amb()) = kAmbVar;
        states().setCovariance(P);
        LiveEphemerisStore::getInstance().clear();
        SsrCorrectionStore::getInstance().clear();
        return true;
    }

    StateManager& states() { return EphemerisJumpTestAccess::states(*engine); }
    int iono() { return states().getIonosphereIndex("E04"); }
    int satBias() { return states().getSatBiasIndex("E04|C5Q"); }
    int amb() { return states().getAmbiguityIndex("E04_L1C"); }
    double var(int idx) { return states().getStateVariance(idx); }
    const EphemerisJumpLedger::Counters& counters() {
        return EphemerisJumpTestAccess::ledger(*engine).counters();
    }

    /// One epoch of the seam with @p rec inserted as the IGS-stream record first.
    void epoch(const BroadcastEphemeris& rec, const GnssTime& t) {
        LiveEphemerisStore::getInstance().insert(rec, /*from_igs=*/true);
        EphemerisJumpTestAccess::react(*engine, {"E04"}, t);
    }
};

} // namespace

// ===========================================================================
// A declared level step with a held pair
// ===========================================================================

// The reaction to a declared lvl= step inflates the covariance along the phase-blind
// direction and leaves every held integer in place.  A case at both moments needs a count
// of the held pairs a step leaves in place, which the engine does not keep, and the
// reaction is reached only from processEpoch: no test seam of today's suites calls it.
TEST(RealtimeReactionLevelEphemeris, ADeclaredLevelStepOnAHeldPairIsNotCounted) {
    GTEST_SKIP() << "Missing: no counter or identity of the held pairs that a declared level "
                    "step leaves in place (the reaction keeps only events, inflated, refused "
                    "and sigma totals), and no test seam reaches the level-step reaction "
                    "outside processEpoch.";
}

// ===========================================================================
// An undecided change of the broadcast record
// ===========================================================================

// Start of the reaction: the new issue arrives after a gap longer than the old record's fit
// window.  The change is counted as undecided and nothing else happens: no state of the
// satellite is re-inflated, reset or marked, and the ambiguity keeps its continuity
// generation.  An undecided change treated as a jump, or left uncounted, fails this case.
TEST(RealtimeReactionLevelEphemeris, AnUndecidedEphemerisChangeIsCountedAndLeavesEveryState) {
    EphemerisEngine s;
    ASSERT_TRUE(s.build());
    s.epoch(e04(), kToe);                         // first sighting: recorded only
    EXPECT_EQ(s.counters().record_changes, 0u);

    const double iono_x = s.states().getStateVector()(s.iono());
    const double amb_x  = s.states().getStateVector()(s.amb());
    const std::uint64_t gen = s.states().getAmbiguityGeneration("E04_L1C");
    ASSERT_NE(gen, 0u);

    s.epoch(e04ThreeHoursLater(), kLater);        // the new issue, old record out of window

    const auto& c = s.counters();
    EXPECT_EQ(c.record_changes, 1u);
    EXPECT_EQ(c.undecided, 1u);
    EXPECT_EQ(c.jumps, 0u);
    EXPECT_EQ(c.below_threshold, 0u);
    EXPECT_EQ(c.source_switches, 0u);
    EXPECT_TRUE(c.closureHolds());
    EXPECT_EQ(EphemerisJumpTestAccess::reseeded(*s.engine), 0);

    EXPECT_DOUBLE_EQ(s.var(s.iono()), EphemerisEngine::kIonoVar);
    EXPECT_DOUBLE_EQ(s.var(s.satBias()), EphemerisEngine::kSatBiasVar);
    EXPECT_DOUBLE_EQ(s.var(s.amb()), EphemerisEngine::kAmbVar);
    EXPECT_DOUBLE_EQ(s.states().getStateVector()(s.iono()), iono_x);
    EXPECT_DOUBLE_EQ(s.states().getStateVector()(s.amb()), amb_x);
    EXPECT_FALSE(s.states().isAmbiguityPendingReseed("E04_L1C"));
    EXPECT_EQ(s.states().getAmbiguityGeneration("E04_L1C"), gen)
        << "an undecided change does not open a new ambiguity arc";

    // The next epoch on the same record is no change at all: the undecided change is
    // never tested later.
    s.epoch(e04ThreeHoursLater(), GnssTime{2420, kLater.tow + 30.0});
    EXPECT_EQ(s.counters().record_changes, 1u);
    EXPECT_EQ(s.counters().undecided, 1u);
    EXPECT_DOUBLE_EQ(s.var(s.amb()), EphemerisEngine::kAmbVar);
}

// End of the reaction: the record of the undecided change becomes the reference at once,
// untested.  A later refresh of the same issue whose semi-major axis is 0.02 m^1/2 off (a
// range change of about 215 m) is then decided against it and re-seeds the satellite.  A
// ledger that kept the old record as its reference after an undecided change would find
// this change undecided as well and fail the case.
TEST(RealtimeReactionLevelEphemeris, AfterAnUndecidedChangeTheUntestedRecordIsTheReference) {
    EphemerisEngine s;
    ASSERT_TRUE(s.build());
    s.epoch(e04(), kToe);
    s.epoch(e04ThreeHoursLater(), kLater);
    ASSERT_EQ(s.counters().undecided, 1u);

    BroadcastEphemeris refreshed = e04ThreeHoursLater();   // same issue, same toe
    refreshed.kepler.sqrt_a += 0.02;
    s.epoch(refreshed, GnssTime{2420, kLater.tow + 30.0});

    const auto& c = s.counters();
    EXPECT_EQ(c.record_changes, 2u);
    EXPECT_EQ(c.undecided, 1u);
    EXPECT_EQ(c.jumps, 1u);
    EXPECT_TRUE(c.closureHolds());
    EXPECT_GT(c.max_abs_jump_m, 150.0);
    EXPECT_EQ(c.max_jump_satellite, "E04");
    EXPECT_EQ(EphemerisJumpTestAccess::reseeded(*s.engine), 1);
    EXPECT_GT(s.var(s.iono()), 150.0 * 150.0);
    EXPECT_GT(s.var(s.satBias()), 150.0 * 150.0);
    EXPECT_TRUE(s.states().isAmbiguityPendingReseed("E04_L1C"));
    EXPECT_GT(s.var(s.amb()), 150.0 * 150.0);
}

// The undecided change is named only in an info line; the run-end counters hold a number
// and no satellite.
TEST(RealtimeReactionLevelEphemeris, AnUndecidedChangeNamesNoSatelliteInTheCounters) {
    GTEST_SKIP() << "Missing: the broadcast-jump counters carry no satellite identity for an "
                    "undecided change (only the largest decided jump is named), and no count "
                    "of the states an undecided change leaves untouched.";
}

// ===========================================================================
// The signal integer flag falling from 1 to 0 on a held signal
// ===========================================================================

namespace {

/// The single-receiver model stack of a consumer derived from a network configuration, so
/// that network.require_signal_integer_for_ar (a network-section key) reaches it.  An
/// undifferenced consumer, with eligibility following the applied stream bias
/// (bias_coverage_requires_apply), so the flag is the only thing that changes.
struct ConsumerStack {
    LogManager log;
    GnssConfig config;
    SsrCorrectionStore store;
    ClockRefMap clock_refs;   // empty: no .BIA clock reference, as the seeds suite
    std::unique_ptr<ProductParser> products;
    std::unique_ptr<RinexParser> rinex;
    std::unique_ptr<StateManager> states;
    std::unique_ptr<PppModel> ppp;
    std::unique_ptr<MeasurementModel> model;
    int epoch = 0;

    bool build() {
        const auto path = gnss_test::tempDir() / "reaction_level_ephemeris_consumer.json";
        {
            std::ofstream out(path);
            out << R"({
              "processing": { "mode": "ppp-rtk-network", "active_systems": ["GPS"],
                              "signal_priorities": { "GPS": "WC" },
                              "enable_ssr_bias_max_age": true, "ssr_bias_max_age_s": 60.0 },
              "estimation": { "ambiguity_resolution": { "elevation_mask": 20.0 } },
              "model_corrections": { "bias_coverage_requires_apply": true },
              "network": {
                "clock_datum_station": "AAAA",
                "require_signal_integer_for_ar": true,
                "closed_loop": { "consumer_undifferenced": true },
                "stations": [
                  { "name": "AAAA", "position_ecef": [ -2493304.99, -4655214.79, 3565497.67 ] },
                  { "name": "BBBB", "position_ecef": [ -2281621.75, -1453585.08, 5756964.89 ] }
                ]
              }
            })";
        }
        GnssConfig base;
        const bool loaded = base.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!loaded) return false;
        config = base.deriveClosedLoopConsumerConfig();
        products = std::make_unique<ProductParser>(config, log);
        rinex = std::make_unique<RinexParser>(config, log, *products);
        states = std::make_unique<StateManager>(5, log, config);
        ppp = std::make_unique<PppModel>(config, *products, log, *rinex);
        model = std::make_unique<MeasurementModel>(*states, *ppp, config, log);
        model->setSsrStore(&store);
        model->setClockRefs(&clock_refs);
        auto x = states->getStateVector();
        x.segment<3>(states->getPositionIndex()) = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        states->setStateVector(x);
        return true;
    }

    GnssTime now() const { return GnssTime{2418, 1000.0 + 30.0 * epoch}; }

    /// This epoch's phase biases of @p sat on L1W and L2W, L1W integer-grade per @p l1w_integer.
    void pushBiases(const std::string& sat, bool l1w_integer) {
        SsrPhaseBias rec;
        rec.sat = sat;
        rec.received_time = now();   // the staleness gate reads the record's own stamp
        for (const char* sig : {"L1W", "L2W"}) {
            SsrPhaseBiasEntry e;
            e.bias_m = 0.1;
            e.signal_integer = (std::string(sig) == "L1W") ? l1w_integer : true;
            rec.bias_per_signal[sig] = e;
        }
        SsrHeader h;
        h.epoch_time = now();
        h.system = System::GPS;
        store.pushPhaseBias(rec, h);
    }

    /// One epoch of the state pre-pass (births, prunes, eligibility marks) for G12 and G15.
    void update(bool g12_l1w_integer) {
        ++epoch;
        pushBiases("G12", g12_l1w_integer);
        pushBiases("G15", true);
        const std::vector<PppObservation> obs = {
            gnss_test::syntheticObservation("G12", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, epoch),
            gnss_test::syntheticObservation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, epoch,
                                            200.0, 60.0),
        };
        model->computeMeasurementUpdate(obs, states->getStateVector());
    }

    bool eligible(const std::string& sat, const std::string& signal) const {
        return phaseBiasCoveredForAr(RowProducts{config, *ppp, store}, sat, signal, now());
    }
};

} // namespace

// Both moments of the edge.  When the flag falls the held key leaves the integer search
// (marked unresolvable) and nothing else happens: its value, its held variance and its
// continuity generation are unchanged and no re-seed is requested.  When the flag returns
// the key is eligible again at once, with no re-validation and no refloat.  A 1 -> 0 edge
// that reset the ambiguity, or a key left eligible under a non-integer bias, fails this case.
TEST(RealtimeReactionLevelEphemeris, ASignalIntegerDropVetoesTheKeyAndKeepsTheHeldState) {
    ConsumerStack s;
    ASSERT_TRUE(s.build());
    ASSERT_TRUE(s.config.networkRequireSignalIntegerForAr());
    ASSERT_TRUE(s.config.getModelCorrections().bias_coverage_requires_apply);
    ASSERT_EQ(s.config.getProcessingMode(), ProcessingMode::PPP_RTK);
    ASSERT_FALSE(s.config.enableBetweenSatelliteSd());

    s.update(/*g12_l1w_integer=*/true);
    const std::string key = "G12_L1W";
    const int idx = s.states->getAmbiguityIndex(key);
    ASSERT_GE(idx, 0);
    EXPECT_TRUE(s.eligible("G12", "L1W"));
    EXPECT_FALSE(s.states->isAmbiguityUnresolvable(key));

    // The key as a fix leaves it: the fixed value with the hold variance (1e-7 m^2).
    {
        Eigen::VectorXd x = s.states->getStateVector();
        x(idx) = 2.379;
        s.states->setStateVector(x);
        Eigen::MatrixXd P = s.states->getCovariance();
        P.row(idx).setZero();
        P.col(idx).setZero();
        P(idx, idx) = 1.0e-7;
        s.states->setCovariance(P);
    }
    const std::uint64_t gen = s.states->getAmbiguityGeneration(key);
    ASSERT_NE(gen, 0u);

    s.update(/*g12_l1w_integer=*/false);   // the flag falls
    EXPECT_FALSE(s.eligible("G12", "L1W"));
    EXPECT_TRUE(s.eligible("G12", "L2W")) << "the veto is per signal";
    EXPECT_TRUE(s.states->isAmbiguityUnresolvable(key));
    EXPECT_FALSE(s.states->isAmbiguityUnresolvable("G12_L2W"));
    EXPECT_FALSE(s.states->isAmbiguityUnresolvable("G15_L1W"));
    ASSERT_EQ(s.states->getAmbiguityIndex(key), idx);
    EXPECT_DOUBLE_EQ(s.states->getStateVector()(idx), 2.379);
    EXPECT_DOUBLE_EQ(s.states->getStateVariance(idx), 1.0e-7) << "the held state is kept";
    EXPECT_EQ(s.states->getAmbiguityGeneration(key), gen);
    EXPECT_FALSE(s.states->isAmbiguityPendingReseed(key));

    s.update(/*g12_l1w_integer=*/true);    // the flag returns
    ASSERT_EQ(s.states->getAmbiguityIndex(key), idx);
    EXPECT_TRUE(s.eligible("G12", "L1W"));
    EXPECT_FALSE(s.states->isAmbiguityUnresolvable(key)) << "eligible again at once";
    EXPECT_DOUBLE_EQ(s.states->getStateVector()(idx), 2.379);
    EXPECT_DOUBLE_EQ(s.states->getStateVariance(idx), 1.0e-7) << "no refloat on the return";
    EXPECT_EQ(s.states->getAmbiguityGeneration(key), gen);
}

// The edges of the integer flag are neither counted nor named.
TEST(RealtimeReactionLevelEphemeris, ASignalIntegerEdgeHasNoCounter) {
    GTEST_SKIP() << "Missing: no counter or identity of the signal integer flag falling from 1 "
                    "to 0 or returning on a key (the veto is visible only at trace level), and "
                    "no release of the held pair when the flag falls.";
}

// ===========================================================================
// A satellite leaving the precise products and returning
// ===========================================================================

namespace {

/// The plain single-receiver stack (uncombined PPP-RTK) with G12 and G15 tracked.
struct DepartureStack {
    LogManager log;
    gnss_test::ModelStack stack;
    ClockRefMap clock_refs;   // empty: no .BIA clock reference, as the seeds suite
    int epoch = 0;

    bool build(const char* tag) {
        if (!stack.load(log, tag, "ppp-rtk", "", "", false)) return false;
        stack.model->setClockRefs(&clock_refs);
        return true;
    }

    StateManager& states() { return *stack.states; }
    GnssTime timeOf(int e) const { return GnssTime{2418, 1000.0 + 30.0 * e}; }

    /// One epoch of the state pre-pass.  With @p g12_phase false, G12 arrives as the PPP
    /// model's broadcast-grade gate delivers it: code only, every phase observable erased.
    void update(bool g12_phase) {
        ++epoch;
        const std::vector<std::string> g12 = g12_phase
            ? std::vector<std::string>{"C1W", "L1W", "C2W", "L2W"}
            : std::vector<std::string>{"C1W", "C2W"};
        const std::vector<PppObservation> obs = {
            gnss_test::syntheticObservation("G12", System::GPS, g12, epoch),
            gnss_test::syntheticObservation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, epoch,
                                            200.0, 60.0),
        };
        stack.model->computeMeasurementUpdate(obs, states().getStateVector());
    }

    /// A clock record of G12 received at @p t (a network clock: applies on its own).
    void pushClock(const GnssTime& t) {
        SsrClockCorrection clk;
        clk.sat = "G12";
        clk.c0 = 0.05;
        clk.received_time = t;
        clk.epoch_time = t;
        stack.store.pushClock(clk, SsrHeader{});
    }

    bool clockFresh(const GnssTime& t) const {
        const auto q = stack.store.getOrbitClock("G12", t, stack.config.getSsrMaxAge());
        return q.has_value() && q->has_clock;
    }

    /// Give a key a converged variance (row and column cleared).
    void converge(const std::string& key, double var) {
        const int i = states().getAmbiguityIndex(key);
        ASSERT_GE(i, 0) << key;
        Eigen::MatrixXd P = states().getCovariance();
        P.row(i).setZero();
        P.col(i).setZero();
        P(i, i) = var;
        states().setCovariance(P);
    }
};

} // namespace

// Start of the reaction.  The precise clock record ages past ssr_max_age_s (30 s by
// default) and the satellite's phase leaves the epoch.  The state pre-pass deletes both
// ambiguities of the satellite on that first miss, converged or not; its ionosphere state
// stays (code still observes it) and the other satellite is untouched.  A departure that
// kept the ambiguities across the gap, or touched another satellite, fails this case.
TEST(RealtimeReactionLevelEphemeris, LeavingThePreciseProductsDeletesTheSatellitesAmbiguities) {
    DepartureStack s;
    ASSERT_TRUE(s.build("reaction_departure"));
    ASSERT_DOUBLE_EQ(s.stack.config.getSsrMaxAge(), 30.0);

    s.pushClock(s.timeOf(1));
    ASSERT_TRUE(s.clockFresh(s.timeOf(1)));
    s.update(true);                       // epoch 1: precise clock, phase in
    ASSERT_TRUE(s.clockFresh(s.timeOf(2)));
    s.update(true);                       // epoch 2: record 30 s old, still fresh
    for (const char* k : {"G12_L1W", "G12_L2W", "G15_L1W"}) s.converge(k, 1.0e-4);
    const std::uint64_t g15_gen = s.states().getAmbiguityGeneration("G15_L1W");

    EXPECT_FALSE(s.clockFresh(s.timeOf(3))) << "60 s old: the satellite is broadcast-grade";
    s.update(false);                      // epoch 3: code only

    EXPECT_EQ(s.states().getAmbiguityIndex("G12_L1W"), -1);
    EXPECT_EQ(s.states().getAmbiguityIndex("G12_L2W"), -1);
    EXPECT_EQ(s.states().getAmbiguityGeneration("G12_L1W"), 0u);
    EXPECT_GE(s.states().getIonosphereIndex("G12"), 0) << "code keeps the ionosphere state";
    const int g15 = s.states().getAmbiguityIndex("G15_L1W");
    ASSERT_GE(g15, 0);
    EXPECT_DOUBLE_EQ(s.states().getStateVariance(g15), 1.0e-4);
    EXPECT_EQ(s.states().getAmbiguityGeneration("G15_L1W"), g15_gen);
}

// End of the reaction.  A fresh record returns the phase, and the ambiguities are born again
// as at a first sighting: the birth variance of a new satellite, a new continuity generation
// and no pending re-seed.  Nothing in the state tells the return from a first sighting.  A
// return that restored the pre-departure state, or kept the old generation, fails this case.
TEST(RealtimeReactionLevelEphemeris, ReturningToThePreciseProductsBirthsFreshAmbiguities) {
    DepartureStack s;
    ASSERT_TRUE(s.build("reaction_return"));
    s.pushClock(s.timeOf(1));
    s.update(true);                       // epoch 1
    const int born = s.states().getAmbiguityIndex("G12_L1W");
    ASSERT_GE(born, 0);
    const double birth_var = s.states().getStateVariance(born);
    EXPECT_GT(birth_var, 1.0) << "a new ambiguity is born loose";
    s.update(true);                       // epoch 2
    s.converge("G12_L1W", 1.0e-4);
    const std::uint64_t gen_before = s.states().getAmbiguityGeneration("G12_L1W");
    ASSERT_NE(gen_before, 0u);

    s.update(false);                      // epoch 3: departed
    ASSERT_EQ(s.states().getAmbiguityIndex("G12_L1W"), -1);

    s.pushClock(s.timeOf(4));
    ASSERT_TRUE(s.clockFresh(s.timeOf(4)));
    s.update(true);                       // epoch 4: returned

    const int back = s.states().getAmbiguityIndex("G12_L1W");
    ASSERT_GE(back, 0);
    EXPECT_DOUBLE_EQ(s.states().getStateVariance(back), birth_var)
        << "the convergence before the departure is gone";
    const std::uint64_t gen_after = s.states().getAmbiguityGeneration("G12_L1W");
    EXPECT_NE(gen_after, 0u);
    EXPECT_NE(gen_after, gen_before) << "a new arc, not the old one resumed";
    EXPECT_FALSE(s.states().isAmbiguityPendingReseed("G12_L1W"));
    EXPECT_GE(s.states().getAmbiguityIndex("G12_L2W"), 0);
}

// The departure and the return are announced once per run per satellite (the phase-gate
// info line) and by an unnamed prune count; neither edge is counted.
TEST(RealtimeReactionLevelEphemeris, BroadcastGradeDepartureAndReturnHaveNoCounters) {
    GTEST_SKIP() << "Missing: no per-satellite counter or line for a satellite leaving the "
                    "precise products and returning (the prune line gives a number without "
                    "keys, the phase-gate line is written once per run), and no mark that "
                    "tells a return from a first sighting.";
}

} // namespace GnssProcessor
