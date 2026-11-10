/**
 * @file test_ephemeris_jump.cpp
 * @brief The broadcast-baseline jump: the store's record selection and evaluation,
 *        the ledger's range-jump test, and the engine's re-seed response.
 *
 * The record is a real Galileo I/NAV set (E04, IODnav 51, 2026 DOY 145, the values
 * the receiver's GALNav block reports; see SubframeAssemblerTest in test_sbf.cpp).
 * The corrupted variant changes sqrt(A) by 0.02 m^1/2, the size of the SBF odd-page
 * decode error that put Galileo E23 about 270 m off on 2026-10-23
 * (docs/record/cards/realtime/INAV-ODD-PAGE.md).
 */

#include <gtest/gtest.h>

#include "realtime/EphemerisJumpLedger.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {

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

constexpr double kPi = Constants::PI;

/// E04, IODnav 51, as the receiver's GALNav reports it (angles converted to radians).
BroadcastEphemeris e04(const std::string& sat = "E04") {
    BroadcastEphemeris eph;
    eph.satellite = sat;
    eph.system    = System::Galileo;
    eph.iode      = 51;
    KeplerianEphemeris& k = eph.kepler;
    k.toe = GnssTime{2420, 106800.0};
    k.toc = GnssTime{2420, 106800.0};
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

/// The same record with sqrt(A) off by @p d (the odd-page decode error's size).
BroadcastEphemeris corruptedSqrtA(double d = 0.02) {
    BroadcastEphemeris eph = e04();
    eph.kepler.sqrt_a += d;
    return eph;
}

const GnssTime kToe{2420, 106800.0};

/// A ground point directly below the satellite, so the line of sight is radial.
Eigen::Vector3d receiverBelow(const BroadcastEphemeris& eph, const GnssTime& t) {
    const auto s = LiveEphemerisStore::evaluateRecord(eph, t);
    return s->position.normalized() * 6371000.0;
}

LiveEphemerisStore::SelectedRecord rec(const BroadcastEphemeris& eph, bool from_igs) {
    LiveEphemerisStore::SelectedRecord r;
    r.eph      = eph;
    r.from_igs = from_igs;
    return r;
}

} // namespace

// ============================================================================
// The range jump between two records
// ============================================================================

TEST(EphemerisJumpLedgerTest, SemiMajorAxisErrorIsARadialRangeJump) {
    // At toe the mean anomaly is m0 for both records and every angle agrees, so the
    // two positions are parallel and differ only in radius, r = a (1 - e cos E): with
    // e = 4e-4 the range change from below is the semi-major-axis change within 0.1 m.
    const BroadcastEphemeris a = e04();
    const BroadcastEphemeris b = corruptedSqrtA();
    const double s = a.kepler.sqrt_a;
    const double d_a = (s + 0.02) * (s + 0.02) - s * s;   // about 218 m
    const auto jump = EphemerisJumpLedger::rangeJump(a, b, kToe, receiverBelow(a, kToe));
    ASSERT_TRUE(jump.has_value());
    EXPECT_NEAR(*jump, d_a, 0.2);
    EXPECT_GT(*jump, 200.0);
}

TEST(EphemerisJumpLedgerTest, ClockChangeIsMinusSpeedOfLightTimesTheChange) {
    BroadcastEphemeris b = e04();
    b.kepler.af0 += 1.0e-6;
    const auto jump = EphemerisJumpLedger::rangeJump(e04(), b, kToe, receiverBelow(b, kToe));
    ASSERT_TRUE(jump.has_value());
    EXPECT_NEAR(*jump, -Constants::SPEED_OF_LIGHT * 1.0e-6, 1e-6);
}

TEST(EphemerisJumpLedgerTest, SortsEachChangeIntoOneOutcome) {
    EphemerisJumpLedger ledger(10.0);
    const Eigen::Vector3d rx = receiverBelow(e04(), kToe);

    // First sighting records only; an identical record reports nothing.
    EXPECT_FALSE(ledger.observe("E04", rec(e04(), false), kToe, rx).has_value());
    EXPECT_FALSE(ledger.observe("E04", rec(e04(), false), kToe, rx).has_value());
    // The same elements from the other source: no change of the modelled range.
    EXPECT_FALSE(ledger.observe("E04", rec(e04(), true), kToe, rx).has_value());

    // A same-IOD clock refresh of 1 ns: a change, 0.3 m, below the threshold.
    BroadcastEphemeris small = e04();
    small.kepler.af0 += 1.0e-9;
    const auto ev1 = ledger.observe("E04", rec(small, true), kToe, rx);
    ASSERT_TRUE(ev1.has_value());
    EXPECT_TRUE(ev1->decided);
    EXPECT_FALSE(ev1->jump);
    EXPECT_FALSE(ev1->sourceSwitched());

    // The receiver's corrupted record takes over: a source switch and a jump.
    const auto ev2 = ledger.observe("E04", rec(corruptedSqrtA(), false), kToe, rx);
    ASSERT_TRUE(ev2.has_value());
    EXPECT_TRUE(ev2->decided);
    EXPECT_TRUE(ev2->jump);
    EXPECT_TRUE(ev2->sourceSwitched());
    EXPECT_EQ(ev2->old_iode, 51);
    EXPECT_EQ(ev2->new_iode, 51);
    EXPECT_GT(ev2->range_jump_m, 200.0);

    const auto& c = ledger.counters();
    EXPECT_EQ(c.record_changes, 2u);
    EXPECT_EQ(c.source_switches, 1u);
    EXPECT_EQ(c.below_threshold, 1u);
    EXPECT_EQ(c.jumps, 1u);
    EXPECT_EQ(c.undecided, 0u);
    EXPECT_EQ(c.max_jump_satellite, "E04");
    EXPECT_TRUE(c.closureHolds());
}

TEST(EphemerisJumpLedgerTest, OldRecordBeyondItsFitWindowIsUndecided) {
    // After a three-hour gap the old record cannot be evaluated (the Galileo window is
    // two hours), so the change is counted but not tested.
    EphemerisJumpLedger ledger(10.0);
    const Eigen::Vector3d rx = receiverBelow(e04(), kToe);
    ledger.observe("E04", rec(e04(), true), kToe, rx);
    BroadcastEphemeris later = e04();
    later.iode = 69;
    later.kepler.toe.tow += 3 * 3600.0;
    later.kepler.toc.tow += 3 * 3600.0;
    const GnssTime t_later{2420, kToe.tow + 3 * 3600.0};
    const auto ev = ledger.observe("E04", rec(later, true), t_later, rx);
    ASSERT_TRUE(ev.has_value());
    EXPECT_FALSE(ev->decided);
    EXPECT_FALSE(ev->jump);
    EXPECT_EQ(ledger.counters().undecided, 1u);
    EXPECT_TRUE(ledger.counters().closureHolds());
}

// ============================================================================
// The store: the selected record, its source, and its evaluation
// ============================================================================

TEST(EphemerisJumpStoreTest, SelectedRecordReportsTheVersionAndItsSource) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    const BroadcastEphemeris rx_rec = e04();            // IODnav 51, from the receiver
    BroadcastEphemeris igs_rec = e04();                 // IODnav 52, from the IGS stream
    igs_rec.iode = 52;
    igs_rec.kepler.toe.tow += 600.0;
    igs_rec.kepler.toc.tow += 600.0;
    store.insert(rx_rec, /*from_igs=*/false);
    store.insert(igs_rec, /*from_igs=*/true);           // demotes the receiver record

    const GnssTime t{2420, kToe.tow + 300.0};
    const auto cur = store.selectedRecord("E04", t, LiveEphemerisStore::ssrIode(igs_rec));
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(cur->eph.iode, 52);
    EXPECT_TRUE(cur->from_igs);
    const auto prev = store.selectedRecord("E04", t, LiveEphemerisStore::ssrIode(rx_rec));
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->eph.iode, 51);
    EXPECT_FALSE(prev->from_igs);
    // Beyond the fit window nothing is reported, as the orbit query rejects it.
    EXPECT_FALSE(store.selectedRecord("E04", GnssTime{2420, kToe.tow + 5 * 3600.0}).has_value());
    EXPECT_FALSE(store.selectedRecord("E05", t).has_value());
    store.clear();
}

TEST(EphemerisJumpStoreTest, EvaluateRecordMatchesTheStoreQueries) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    store.insert(e04());
    const GnssTime t{2420, kToe.tow + 123.0};
    const auto pv  = store.computeSatellitePV("E04", t);
    const auto clk = store.computeSatelliteClock("E04", t);
    const auto ev  = LiveEphemerisStore::evaluateRecord(e04(), t);
    ASSERT_TRUE(pv && clk && ev);
    EXPECT_EQ(ev->position, pv->position);
    EXPECT_EQ(ev->clock_bias_s, clk->bias);
    store.clear();
}

// ============================================================================
// The engine's response
// ============================================================================

namespace {

class EphemerisJumpEngineTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    std::unique_ptr<RealTimeEngine> engine;

    void build(bool armed) {
        const auto path = gnss_test::tempDir() / "ephemeris_jump.json";
        {
            std::ofstream out(path);
            out << R"({
                "processing": {
                    "mode": "ppp-rtk",
                    "active_systems": ["GPS", "Galileo"],
                    "ephemeris_jump_reseed": )" << (armed ? "true" : "false") << R"(
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
        ASSERT_TRUE(loaded);
        engine = std::make_unique<RealTimeEngine>(config, log);
        EphemerisJumpTestAccess::initialize(*engine);
        EphemerisJumpTestAccess::setReceiver(*engine, receiverBelow(e04(), kToe));

        // E04 converged under the old record: tight ionosphere, satellite-bias and
        // ambiguity states; E05 likewise, untouched by a jump of E04.
        for (const char* s : {"E04", "E05"}) {
            const std::string sat = s;
            states().addIonosphereState(sat, -266.0);
            states().addSatBiasStateWithVariance(sat + "|C5Q", 10.0, 1.0);
            states().addAmbiguity(sat + "_L1C", -270.0, 0.02);
        }
        Eigen::MatrixXd P = states().getCovariance();
        for (const char* s : {"E04", "E05"}) {
            const std::string sat = s;
            const int io = states().getIonosphereIndex(sat);
            const int sb = states().getSatBiasIndex(sat + "|C5Q");
            const int am = states().getAmbiguityIndex(sat + "_L1C");
            P(io, io) = 0.09;          // 0.3 m
            P(sb, sb) = 6.25e-4;       // 0.025 m
            P(am, am) = 4.0e-4;        // 0.02 m
        }
        states().setCovariance(P);
        LiveEphemerisStore::getInstance().clear();
        SsrCorrectionStore::getInstance().clear();
    }
    void TearDown() override {
        LiveEphemerisStore::getInstance().clear();
        SsrCorrectionStore::getInstance().clear();
    }

    StateManager& states() { return EphemerisJumpTestAccess::states(*engine); }
    double var(int idx) { return states().getStateVariance(idx); }

    /// Run the seam twice: E04 on its receiver record, then on the IGS record.
    void receiverThenIgs(const BroadcastEphemeris& receiver_rec, bool live_orbit = true) {
        auto& store = LiveEphemerisStore::getInstance();
        store.insert(receiver_rec, /*from_igs=*/false);
        BroadcastEphemeris e05 = e04("E05");
        store.insert(e05, /*from_igs=*/true);
        EphemerisJumpTestAccess::react(*engine, {"E04", "E05"}, kToe, live_orbit);
        store.insert(e04(), /*from_igs=*/true);   // same IODnav, IGS takes priority
        EphemerisJumpTestAccess::react(*engine, {"E04", "E05"}, GnssTime{2420, kToe.tow + 1.0},
                                       live_orbit);
    }
};

} // namespace

TEST_F(EphemerisJumpEngineTest, AJumpReseedsTheSatelliteAndOnlyIt) {
    build(true);
    receiverThenIgs(corruptedSqrtA());

    const auto& c = EphemerisJumpTestAccess::ledger(*engine).counters();
    EXPECT_EQ(c.jumps, 1u);
    EXPECT_EQ(c.source_switches, 1u);
    EXPECT_EQ(EphemerisJumpTestAccess::reseeded(*engine), 1);

    // E04: variances raised to at least the jump (about 218 m), ambiguity reset.
    const double jump2 = 200.0 * 200.0;
    EXPECT_GT(var(states().getIonosphereIndex("E04")), jump2);
    EXPECT_GT(var(states().getSatBiasIndex("E04|C5Q")), jump2);
    EXPECT_TRUE(states().isAmbiguityPendingReseed("E04_L1C"));
    EXPECT_GT(var(states().getAmbiguityIndex("E04_L1C")), jump2);
    // E05 is left as it was.
    EXPECT_DOUBLE_EQ(var(states().getIonosphereIndex("E05")), 0.09);
    EXPECT_DOUBLE_EQ(var(states().getSatBiasIndex("E05|C5Q")), 6.25e-4);
    EXPECT_FALSE(states().isAmbiguityPendingReseed("E05_L1C"));
}

TEST_F(EphemerisJumpEngineTest, ASmallChangeIsCountedAndLeavesTheStates) {
    build(true);
    BroadcastEphemeris close = e04();
    close.kepler.af0 += 1.0e-9;                      // 0.3 m from the IGS record
    receiverThenIgs(close);

    const auto& c = EphemerisJumpTestAccess::ledger(*engine).counters();
    EXPECT_EQ(c.record_changes, 1u);
    EXPECT_EQ(c.below_threshold, 1u);
    EXPECT_EQ(c.jumps, 0u);
    EXPECT_DOUBLE_EQ(var(states().getIonosphereIndex("E04")), 0.09);
    EXPECT_FALSE(states().isAmbiguityPendingReseed("E04_L1C"));
}

TEST_F(EphemerisJumpEngineTest, AnSp3ModelledSatelliteIsNotTested) {
    // The orbit came from an SP3, so the broadcast record does not reach the model.
    build(true);
    receiverThenIgs(corruptedSqrtA(), /*live_orbit=*/false);
    EXPECT_EQ(EphemerisJumpTestAccess::ledger(*engine).counters().record_changes, 0u);
    EXPECT_DOUBLE_EQ(var(states().getIonosphereIndex("E04")), 0.09);
    EXPECT_FALSE(states().isAmbiguityPendingReseed("E04_L1C"));
}

TEST_F(EphemerisJumpEngineTest, UnarmedMeasuresAndWritesNothing) {
    build(false);
    receiverThenIgs(corruptedSqrtA());

    const auto& c = EphemerisJumpTestAccess::ledger(*engine).counters();
    EXPECT_EQ(c.record_changes, 0u);
    EXPECT_EQ(EphemerisJumpTestAccess::reseeded(*engine), 0);
    EXPECT_DOUBLE_EQ(var(states().getIonosphereIndex("E04")), 0.09);
    EXPECT_DOUBLE_EQ(var(states().getSatBiasIndex("E04|C5Q")), 6.25e-4);
    EXPECT_FALSE(states().isAmbiguityPendingReseed("E04_L1C"));
}

TEST(EphemerisJumpTargetsTest, APivotJumpReseedsItsConstellationPartners) {
    const std::set<std::string> held = {"E04", "E05", "E23", "G01", "G02"};
    EXPECT_EQ(EphemerisJumpTestAccess::targets("E23", false, held),
              (std::set<std::string>{"E23"}));
    EXPECT_EQ(EphemerisJumpTestAccess::targets("E23", true, held),
              (std::set<std::string>{"E04", "E05"}));
}

} // namespace GnssProcessor
