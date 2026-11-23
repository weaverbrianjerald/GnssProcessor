/**
 * @file test_brdc_clock_no_hold.cpp
 * @brief The broadcast clock a priori (`network.apriori_source` `sp3_brdc_clock`) on the chain posture
 *        without the phase-bias hold: the loader's acceptance and its remaining refusals, and the wire
 *        seam's broadcast terms with the hold off.
 *
 * Under the broadcast clock the loader requires the smoothed hold on the float posture (the posture
 * the broadcast clock was measured with).  On the chain posture (`network.phase_datum_eq34`), which
 * gives the stream its integer frame without a hold and runs the hold off, it accepts the broadcast
 * clock with `network.emit_pb_hold` off.  A hold that is armed must still be the smoothed hold.
 *
 * Covered (each refusal is paired with a control that differs only in the clock source and loads):
 *   - the reference file of the simulator with the broadcast clock and the hold off loads;
 *   - the frozen float lineage with the hold off stays refused under the broadcast clock;
 *   - on the chain posture the phase-clock hold and the plain hold stay refused, the smoothed hold
 *     stays accepted;
 *   - the broadcast clock changes the snapshot fingerprint (fresh convergence only);
 *   - the wire seam (networkBrdcWire), with the hold off: brdc(iode) + C_wire = a + C_est, without
 *     and with the tracked a priori (C_wire = C_est + O + U);
 *   - a change of ephemeris between two epochs: the stamp switches, the switch is counted, and the
 *     reconstruction holds on both sides.
 */

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/emit/BrdcWire.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/BrdcClockApriori.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/RinexNavParser.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kC = 299792458.0;

// ---------------------------------------------------------------------------
// Configurations
// ---------------------------------------------------------------------------

/// Load @p golden (a file of config/golden) with @p edit applied; a refusal (false or a throw)
/// returns false.
bool loadGoldenWith(const std::string& golden, const std::function<void(nlohmann::json&)>& edit,
                    GnssConfig& cfg, const std::string& tag) {
    const fs::path src = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / golden;
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const fs::path p = gnss_test::tempDir() / ("gnss_bnh_" + tag + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    bool ok = false;
    try { ok = cfg.loadFromJson(p, log); } catch (const std::exception&) { ok = false; }
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

const char* kReference = "golden_sim_refound.json";   // the chain posture, the hold off
const char* kFrozen    = "golden_sim_rf0hv18.json";   // the float posture with the hold

/// The broadcast clock a priori, the only change of the target arm.
void brdc(nlohmann::json& j) {
    j["network"]["apriori_source"] = "sp3_brdc_clock";
    j["network"]["brdc_select"]    = "ttom";
}

std::uint64_t fingerprintOf(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

// ---------------------------------------------------------------------------
// A synthetic navigation file (the helpers of test_brdc_clock_apriori.cpp)
// ---------------------------------------------------------------------------

std::string navField(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "% .12E", v);
    std::string s(buf);
    if (s.size() > 19) {
        const auto e = s.find_first_of("Ee");
        if (e != std::string::npos && s.size() - e == 5 && s[e + 2] == '0') s.erase(e + 2, 1);
    }
    if (s.size() < 19) s = std::string(19 - s.size(), ' ') + s;
    return s.substr(0, 19);
}

std::string navLine(std::initializer_list<double> vals) {
    std::string s = "    ";
    for (double v : vals) s += navField(v);
    return s + "\n";
}

/// A GPS LNAV record at 2024-05-08 hh:00 with chosen clock terms, IODE, health and ORBIT-7.
std::string gpsRecord(int prn, int hh, double af0, double af1, int iode, int health,
                      double ttom, double fit, double toe) {
    char hdr[40];
    std::snprintf(hdr, sizeof(hdr), "G%02d %4d %02d %02d %02d %02d %02d", prn, 2024, 5, 8, hh, 0, 0);
    std::string s = std::string(hdr) + navField(af0) + navField(af1) + navField(0.0) + "\n";
    s += navLine({double(iode), 10.0, 4.5e-9, 1.0});
    s += navLine({1e-6, 0.01, 1e-6, 5153.7});
    s += navLine({toe, 1e-8, 0.3, 1e-8});
    s += navLine({0.95, 200.0, 1.0, -8e-9});
    s += navLine({1e-10, 1.0, 2313.0, 0.0});
    s += navLine({2.0, double(health), -1e-8, double(iode)});
    s += navLine({ttom, fit});
    return s;
}

const char* kHdrV3 =
    "     3.04           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
    "                                                            END OF HEADER\n";

/// 2024-05-08 00:00:00 GPS, continuous seconds.
double day129Origin() {
    return TimeUtils::continuousGpsSeconds(TimeUtils::gregorianGpsToGpsTime(2024, 5, 8, 0, 0, 0.0));
}

GnssTime fromContinuous(double s) {
    const double wk = std::floor(s / 604800.0);
    return GnssTime{static_cast<int>(wk), s - wk * 604800.0};
}

/// G05 with a switch of record (IODE 10 to 11) at the first node at or after 00:59:50, and G06
/// with one record all along.
std::string switchNav() {
    const double day  = day129Origin();
    const double wk   = std::floor(day / 604800.0) * 604800.0;
    const double sow0 = day - wk;
    std::string body = std::string(kHdrV3);
    body += gpsRecord(5, 0, 1.0e-4, 1.0e-11, 10, 0, sow0 - 7200.0, 4.0, sow0);
    body += gpsRecord(5, 2, 1.0e-4 + 5e-10, 2.0e-11, 11, 0, sow0 + 3590.0, 4.0, sow0 + 7200.0);
    body += gpsRecord(6, 0, 2.0e-4, 0.0, 20, 0, sow0 - 7200.0, 4.0, sow0);
    return body;
}

// ---------------------------------------------------------------------------
// The wire seam with the hold off
// ---------------------------------------------------------------------------

/// The reference file of the simulator with the broadcast clock (the hold off), the selector of
/// switchNav(), and the run context, traces and setup the seam reads.
class BrdcNoHoldWire : public ::testing::Test {
protected:
    void SetUp() override { LiveEphemerisStore::getInstance().clear(); }

    /// Load the configuration (with the tracked a priori and its two-way centring when @p track)
    /// and build what the seam reads.
    void init(bool track) {
        ASSERT_TRUE(loadGoldenWith(kReference, [track](nlohmann::json& j) {
            brdc(j);
            if (track) {
                j["network"]["apriori_clock_track"] = true;
                j["network"]["cycle_slip"]["if_phase_td_sat_centre"] = true;
            }
        }, cfg_, track ? "wire_track" : "wire"));
        ASSERT_EQ(cfg_.networkAprioriClockTrack(), track);
        ASSERT_FALSE(cfg_.getNetworkConfig().emit_pb_hold);
        ctx_ = std::make_unique<NetworkRunContext>(cfg_, cfg_.getNetworkConfig());
        ASSERT_TRUE(ctx_->knobs.nbp_on);
        ASSERT_EQ(ctx_->knobs.apriori_track_on, track);
        const double day = day129Origin();
        setup_.brdc_apriori = std::make_unique<BrdcClockApriori>(
            RinexNavParser::parseString(switchNav()), day, day + 3.0 * 3600.0);
        traces_ = std::make_unique<NetworkRunTraces>(gnss_test::tempDir() / "bnh_traces");
        log_ = std::make_shared<spdlog::logger>("bnh_wire",
                                                std::make_shared<spdlog::sinks::null_sink_mt>());
    }
    void TearDown() override { LiveEphemerisStore::getInstance().clear(); }

    /// One epoch at continuous GPS time @p t_s: the store fed by transmission time, the two
    /// satellites at the estimates @p c_est (m), the seam run.  Returns the emission after the seam.
    NetworkSolutionEpoch runEpoch(double t_s, const std::map<std::string, double>& c_est) {
        setup_.brdc_apriori->feedStore(LiveEphemerisStore::getInstance(), t_s);
        NetworkSolutionEpoch sol;
        sol.epoch_time = fromContinuous(t_s);
        for (const auto& [sat, c] : c_est) {
            NetworkSatSolution s;
            s.sat        = sat;
            s.has_clock  = true;
            s.clock_c0_m = c;
            sol.sats.push_back(s);
        }
        SyncedEpoch se;
        se.time = sol.epoch_time;
        networkBrdcWire(*ctx_, cfg_, log_, *traces_, setup_, se, sol);
        return sol;
    }

    /// brdc(iode) + C_wire, the user's clock, from the store's record pinned to the stamp (m).
    double userClock(const NetworkSatSolution& s, double t_s) const {
        const auto bc = LiveEphemerisStore::getInstance().computeSatelliteClock(
            s.sat, fromContinuous(t_s), s.clock_iode);
        EXPECT_TRUE(bc.has_value());
        if (!bc) return 0.0;
        EXPECT_EQ(bc->iode, s.clock_iode);
        return bc->bias * kC + s.clock_c0_m;
    }

    /// a + C_est, the producer's clock (m).
    double producerClock(const std::string& sat, double t_s, double c_est) const {
        const auto ev = setup_.brdc_apriori->evaluate(sat, fromContinuous(t_s));
        EXPECT_TRUE(ev.has_value());
        return ev ? ev->clock_s * kC + c_est : 0.0;
    }

    GnssConfig                         cfg_;
    std::unique_ptr<NetworkRunContext> ctx_;
    NetworkSetupResult                 setup_;
    std::unique_ptr<NetworkRunTraces>  traces_;
    std::shared_ptr<spdlog::logger>    log_;
};

}  // namespace

// ===========================================================================
// The loader
// ===========================================================================

TEST(BrdcNoHoldConfig, TheReferenceFileLoadsOnTheBroadcastClockWithTheHoldOff) {
    GnssConfig control, cfg;
    ASSERT_TRUE(loadGoldenWith(kReference, [](nlohmann::json&) {}, control, "ref_control"));
    ASSERT_TRUE(loadGoldenWith(kReference, brdc, cfg, "ref_brdc"));
    EXPECT_FALSE(control.networkAprioriBrdcClock());
    EXPECT_TRUE(cfg.networkAprioriBrdcClock());
    EXPECT_TRUE(cfg.networkPhaseDatumEq34());
    EXPECT_FALSE(cfg.getNetworkConfig().emit_pb_hold);
    EXPECT_FALSE(cfg.getNetworkConfig().emit_pb_hold_smoothed);
    EXPECT_FALSE(cfg.getNetworkConfig().emit_pb_hold_phase_clock);
    // sp3's anchor-sigma auto, not brdc_rts's 3 m (card NBP).
    EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(),
                     control.resolveNetworkAprioriClockAnchorSigmaM());
    // Fingerprinted: fresh convergence only.
    EXPECT_NE(fingerprintOf(cfg), fingerprintOf(control));
}

TEST(BrdcNoHoldConfig, WithoutTheChainTheHoldOffStaysRefused) {
    // The float posture with its hold switched off loads on the CODE clock and is refused on the
    // broadcast clock: off the chain the smoothed hold is still required.
    const auto no_hold = [](nlohmann::json& j) {
        j["network"]["emit_pb_hold"]             = false;
        j["network"]["emit_pb_hold_phase_clock"] = false;
        j["network"]["emit_pb_hold_smoothed"]    = false;
    };
    GnssConfig control, cfg;
    ASSERT_TRUE(loadGoldenWith(kFrozen, no_hold, control, "frozen_nohold_control"));
    ASSERT_FALSE(control.networkPhaseDatumEq34());
    EXPECT_FALSE(loadGoldenWith(kFrozen, [&](nlohmann::json& j) { no_hold(j); brdc(j); }, cfg,
                                "frozen_nohold_brdc"));
}

TEST(BrdcNoHoldConfig, OnTheChainThePhaseClockHoldStaysRefused) {
    const auto lah = [](nlohmann::json& j) {
        j["network"]["emit_pb_hold"]             = true;
        j["network"]["emit_pb_hold_phase_clock"] = true;
    };
    GnssConfig cfg;
    EXPECT_FALSE(loadGoldenWith(kReference, [&](nlohmann::json& j) { lah(j); brdc(j); }, cfg,
                                "ref_lah_brdc"));
}

TEST(BrdcNoHoldConfig, OnTheChainAnArmedHoldMustBeTheSmoothedHold) {
    // The plain hold (armed, neither the smoothed nor the phase-clock hold) is refused.
    GnssConfig plain;
    EXPECT_FALSE(loadGoldenWith(kReference, [](nlohmann::json& j) {
        j["network"]["emit_pb_hold"] = true;
        brdc(j);
    }, plain, "ref_plain_brdc"));
}

TEST(BrdcNoHoldConfig, OnTheChainTheSmoothedHoldIsStillAccepted) {
    const auto smh = [](nlohmann::json& j) {
        j["network"]["emit_pb_hold"]          = true;
        j["network"]["emit_pb_hold_smoothed"] = true;
    };
    GnssConfig control, cfg;
    ASSERT_TRUE(loadGoldenWith(kReference, smh, control, "ref_smh_control"));
    ASSERT_TRUE(loadGoldenWith(kReference, [&](nlohmann::json& j) { smh(j); brdc(j); }, cfg,
                               "ref_smh_brdc"));
    EXPECT_TRUE(cfg.networkAprioriBrdcClock());
    EXPECT_TRUE(cfg.getNetworkConfig().emit_pb_hold_smoothed);
}

// ===========================================================================
// The wire seam with the hold off
// ===========================================================================

TEST_F(BrdcNoHoldWire, TheUserClockReproducesTheEstimatedClock) {
    init(false);
    if (HasFatalFailure()) return;
    const double t = day129Origin() + 1800.0;
    const std::map<std::string, double> c_est{{"G05", -1.234567}, {"G06", 0.456789}};
    const NetworkSolutionEpoch sol = runEpoch(t, c_est);
    ASSERT_EQ(sol.sats.size(), 2u);
    for (const auto& s : sol.sats) {
        EXPECT_GE(s.clock_iode, 0) << s.sat;
        EXPECT_NEAR(userClock(s, t), producerClock(s.sat, t, c_est.at(s.sat)), 1e-9) << s.sat;
    }
    EXPECT_EQ(sol.sats[0].clock_iode, 10);
    EXPECT_EQ(sol.sats[1].clock_iode, 20);
    const auto& c = ctx_->counters;
    EXPECT_EQ(c.nbp_epochs, 1);
    EXPECT_EQ(c.nbp_stamped, 2);
    EXPECT_EQ(c.nbp_unstamped, 0);
    EXPECT_EQ(c.nbp_held, 2);
    EXPECT_EQ(c.brd_track_wire, 0);
}

TEST_F(BrdcNoHoldWire, TheTrackedSumReachesTheProducts) {
    // The tracked a priori: the solve ran against a + U, so brdc(iode) + C_wire = a + U + C_est.
    init(true);
    if (HasFatalFailure()) return;
    const std::map<std::string, double> u{{"G05", 0.0731}, {"G06", -0.0412}};
    net::assignSat(ctx_->entities, &net::SatelliteState::apriori_track_m, u);
    const double t = day129Origin() + 1800.0;
    const std::map<std::string, double> c_est{{"G05", -1.234567}, {"G06", 0.456789}};
    const NetworkSolutionEpoch sol = runEpoch(t, c_est);
    ASSERT_EQ(sol.sats.size(), 2u);
    for (const auto& s : sol.sats)
        EXPECT_NEAR(userClock(s, t), producerClock(s.sat, t, c_est.at(s.sat)) + u.at(s.sat), 1e-9)
            << s.sat;
    EXPECT_EQ(ctx_->counters.brd_track_wire, 2);
}

TEST_F(BrdcNoHoldWire, AChangeOfEphemerisBetweenTwoEpochs) {
    init(false);
    if (HasFatalFailure()) return;
    const double t_sw = day129Origin() + 3600.0;   // G05's switch node
    const std::map<std::string, double> c_est{{"G05", -1.234567}, {"G06", 0.456789}};
    const NetworkSolutionEpoch before = runEpoch(t_sw - 30.0, c_est);
    const NetworkSolutionEpoch after  = runEpoch(t_sw, c_est);
    ASSERT_EQ(before.sats.size(), 2u);
    ASSERT_EQ(after.sats.size(), 2u);
    // The stamp switches on G05 only, and the switch is counted.
    EXPECT_EQ(before.sats[0].clock_iode, 10);
    EXPECT_EQ(after.sats[0].clock_iode, 11);
    EXPECT_EQ(before.sats[1].clock_iode, 20);
    EXPECT_EQ(after.sats[1].clock_iode, 20);
    EXPECT_EQ(ctx_->counters.nbp_switches, 1);
    EXPECT_EQ(ctx_->counters.nbp_records.size(), 3u);
    // The reconstruction holds on both sides of the switch.
    for (const auto& s : before.sats)
        EXPECT_NEAR(userClock(s, t_sw - 30.0), producerClock(s.sat, t_sw - 30.0, c_est.at(s.sat)), 1e-9);
    for (const auto& s : after.sats)
        EXPECT_NEAR(userClock(s, t_sw), producerClock(s.sat, t_sw, c_est.at(s.sat)), 1e-9);
    // The a priori is continuous at the switch, so the wire's step there is the records' step:
    // C_wire(new) - C_wire(old, extended) = brdc_10(t_sw) - brdc_11(t_sw).
    const auto& store = LiveEphemerisStore::getInstance();
    const auto b10 = store.computeSatelliteClock("G05", fromContinuous(t_sw), 10);
    const auto b11 = store.computeSatelliteClock("G05", fromContinuous(t_sw), 11);
    ASSERT_TRUE(b10 && b11);
    const double a_before = setup_.brdc_apriori->evaluate("G05", fromContinuous(t_sw - 30.0))->clock_s;
    const double a_after  = setup_.brdc_apriori->evaluate("G05", fromContinuous(t_sw))->clock_s;
    const double wire_old_at_sw = before.sats[0].clock_c0_m
                                + (a_after - a_before) * kC
                                - (b10->bias - store.computeSatelliteClock(
                                       "G05", fromContinuous(t_sw - 30.0), 10)->bias) * kC;
    EXPECT_NEAR(after.sats[0].clock_c0_m - wire_old_at_sw, (b10->bias - b11->bias) * kC, 1e-9);
}
