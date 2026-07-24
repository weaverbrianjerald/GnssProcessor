/**
 * @file test_sim_selfconsistency.cpp
 * @brief Self-consistency battery for gnss_simulator (CTest gnss_simulator_selftest).
 *
 * The product-/processor-free subset of the DESIGN §10.6 battery — the invariants
 * that can be locked without the precise-product chain or an out-of-process
 * gnss_processor run (those are exercised end-to-end by harness/gnss_verify_loop.ps1):
 *   - RINEX writer<->parser round-trip across GPS+Galileo / all 8 codes (tests 1,2),
 *   - the first-order iono delay law + ionosphere-free cancellation + dateline
 *     continuity of the storm field,
 *   - the Verifier grading each quantity independently (good=PASS, position-biased
 *     and too-perfect=FAIL), the anti-circularity guard included.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>

#include <nlohmann/json.hpp>

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "math/Coordinates.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

#include "sim/IonoDisturbance.hpp"
#include "sim/OtlField.hpp"
#include "sim/RinexObsWriter.hpp"
#include "sim/ScriptedSlips.hpp"
#include "sim/SimConfig.hpp"
#include "sim/TrajectoryGenerator.hpp"
#include "sim/Verifier.hpp"

using namespace GnssProcessor;

namespace {
std::filesystem::path tmpDir(const std::string& name) {
    auto d = std::filesystem::temp_directory_path() / ("gnss_sim_test_" + name);
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}
} // namespace

// --- Test 1 & 2: RINEX-3.04 writer round-trips GPS+Galileo, all 8 codes. ------
TEST(SimSelfConsistency, RinexRoundTripAllCodes) {
    const auto dir = tmpDir("roundtrip");
    sim::ObsHeaderSpec hdr;
    hdr.approx_xyz = { -5063042.0, 4524238.0, -1761984.0 };
    hdr.interval_s = 30.0;
    hdr.time_of_first_obs = GnssTime{ 2313, 302400.0 };
    hdr.obs_types = { { 'G', { "C1C", "L1C", "C2W", "L2W" } },
                      { 'E', { "C1C", "L1C", "C5Q", "L5Q" } } };

    // One GPS + one Galileo satellite with distinct values per code.
    std::map<std::string, std::map<std::string, double>> truth;
    std::vector<sim::SatObs> rows;
    auto add = [&](const std::string& id, std::vector<std::string> codes, double base) {
        sim::SatObs s;
        s.sat_id = id;
        double v = base;
        for (const auto& c : codes) {
            const double val = std::round(v * 1000.0) / 1000.0;
            s.values[c] = val;
            truth[id][c] = val;
            v += 1234.567;
        }
        rows.push_back(std::move(s));
    };
    add("G01", { "C1C", "L1C", "C2W", "L2W" }, 21000000.0);
    add("E11", { "C1C", "L1C", "C5Q", "L5Q" }, 23000000.0);

    const auto path = dir / "RT_R_20241290000_01D_30S_MO.rnx";
    sim::RinexObsWriter w;
    std::string err;
    ASSERT_TRUE(w.open(path, hdr, &err)) << err;
    GnssTime t = hdr.time_of_first_obs;
    for (int e = 0; e < 3; ++e) { w.writeEpoch(t, rows); t.tow += 30.0; }
    w.close();

    GnssConfig config;
    config.setObservationPath(path);
    LogManager log;
    log.initialize(dir / "logs", LogLevel::Warn);
    ProductParser pp(config, log);
    RinexParser parser(config, log, pp);
    ASSERT_TRUE(parser.parse());

    const auto& obs = parser.getObservations();
    EXPECT_EQ(obs.size(), 6u); // 3 epochs * 2 sats
    int checked = 0;
    for (const auto& o : obs) {
        auto sit = truth.find(o.satellite);
        ASSERT_NE(sit, truth.end()) << o.satellite;
        for (const auto& kv : o.measurements) {
            ASSERT_TRUE(sit->second.count(kv.first)) << o.satellite << " " << kv.first;
            EXPECT_NEAR(kv.second, sit->second[kv.first], 1e-3) << o.satellite << " " << kv.first;
            ++checked;
        }
    }
    EXPECT_EQ(checked, 24); // 6 records * 4 codes
}

// --- First-order iono delay law magnitude (30 TECU @ L1 ~ 4.87 m). -----------
TEST(SimSelfConsistency, IonoDelayLawMagnitude) {
    const double i_l1 = sim::IonoDisturbance::slantDelayMetres(30.0, Constants::GPS_L1_FREQ);
    EXPECT_NEAR(i_l1, 4.871, 0.02);
    // L2 > L1 by (f1/f2)^2.
    const double i_l2 = sim::IonoDisturbance::slantDelayMetres(30.0, Constants::GPS_L2_FREQ);
    const double r = (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) /
                     (Constants::GPS_L2_FREQ * Constants::GPS_L2_FREQ);
    EXPECT_NEAR(i_l2 / i_l1, r, 1e-6);
}

// --- Ionosphere-free combination cancels the first-order iono (any STEC). ----
TEST(SimSelfConsistency, IonoFreeCancels) {
    const double f1 = Constants::GPS_L1_FREQ, f2 = Constants::GPS_L2_FREQ;
    const double cc1 = f1 * f1 / (f1 * f1 - f2 * f2), cc2 = -f2 * f2 / (f1 * f1 - f2 * f2);
    for (double stec : { 5.0, 37.5, 90.0, 138.0 }) {
        const double i1 = sim::IonoDisturbance::slantDelayMetres(stec, f1);
        const double i2 = sim::IonoDisturbance::slantDelayMetres(stec, f2);
        EXPECT_NEAR(cc1 * i1 + cc2 * i2, 0.0, 1e-9) << "stec=" << stec;
    }
    // Galileo E1/E5a pair too.
    const double g1 = Constants::GAL_E1_FREQ, g2 = Constants::GAL_E5A_FREQ;
    const double d1 = g1 * g1 / (g1 * g1 - g2 * g2), d2 = -g2 * g2 / (g1 * g1 - g2 * g2);
    const double j1 = sim::IonoDisturbance::slantDelayMetres(72.0, g1);
    const double j2 = sim::IonoDisturbance::slantDelayMetres(72.0, g2);
    EXPECT_NEAR(d1 * j1 + d2 * j2, 0.0, 1e-9);
}

// --- Storm field is continuous across the ±180° dateline (scenario at -178°). -
TEST(SimSelfConsistency, IonoDatelineContinuous) {
    sim::IonoConfig cfg;
    cfg.center_lat0_deg = -18.5;
    cfg.center_lon0_deg = -179.5; // just west of the dateline
    cfg.window_start_tow_s = 0.0;
    cfg.window_duration_s = 3600.0;
    const sim::IonoDisturbance iono(cfg);
    const GnssTime t{ 2313, 1800.0 }; // mid-window (peak)
    // Two near-identical rays whose IPP longitudes straddle +/-180 must give a
    // near-identical SED (no spurious ~360° delta-lon collapsing the Gaussian).
    const sim::IonoSample a = iono.evaluate(-18.5, 179.9, 0.0, 80.0, t, 0.0);   // az north, high el
    const sim::IonoSample b = iono.evaluate(-18.5, -179.9, 0.0, 80.0, t, 0.0);
    // Both are ~1.4° from a center at -179.5 (179.9 == -180.1): SED should be sizeable,
    // not collapsed to ~0 by an unwrapped 359° delta.
    EXPECT_GT(a.dstec_sed_tecu, 1.0);
    EXPECT_GT(b.dstec_sed_tecu, 1.0);
    // The IPP longitude must be reported in (-180,180].
    EXPECT_LE(std::abs(a.ipp_lon_deg), 180.0);
    EXPECT_LE(std::abs(b.ipp_lon_deg), 180.0);
}

// --- Landfall mode: drift start -> destination, grounding on the sea surface. -
// The vessel must land EXACTLY on the destination, at rest, and stay on the
// ellipsoid (h ~ 0) the whole way — a flat ENU tangent plane would bow ~L^2/(2R)
// (hundreds of m) off the surface over a ~90 km arc.
TEST(SimSelfConsistency, LandfallGroundsAtDestinationOnSeaSurface) {
    sim::TrajectoryConfig c;
    c.kinematic = true;
    c.lat0_deg = -16.60; c.lon0_deg = -179.55;    // start: open ocean, E of the dateline
    c.dest_lat_deg = -16.169; c.dest_lon_deg = 179.723;  // grounding: W of the dateline
    c.h0_m = 0.0; c.heave_amp_m = 1.0;
    c.interval_s = 30.0; c.n_epochs = 241;
    sim::TrajectoryGenerator g(c);
    const auto& s = g.states();
    ASSERT_EQ(s.size(), 241u);

    EXPECT_NEAR(s.front().lat_deg, -16.60, 1e-4);          // starts exactly at the origin
    EXPECT_NEAR(s.front().lon_deg, -179.55, 1e-4);
    EXPECT_NEAR(s.back().lat_deg, -16.169, 1e-4);          // grounds exactly on the destination
    EXPECT_NEAR(s.back().lon_deg, 179.723, 1e-4);
    EXPECT_NEAR(s.back().h_m, 0.0, 1e-2);                  // at the waterline (heave tapered out)
    EXPECT_LT(s.back().speed_mps, 0.5);                    // decelerated to a stop
    // Stays on the sea surface throughout (curvature-bug regression guard).
    for (const auto& st : s) EXPECT_LT(std::abs(st.h_m), c.heave_amp_m + 0.1);
    // The track crosses the +/-180 dateline (0-360 longitude spans both sides).
    double lomin = 1e9, lomax = -1e9;
    for (const auto& st : s) {
        const double L = std::fmod(st.lon_deg + 360.0, 360.0);
        lomin = std::min(lomin, L); lomax = std::max(lomax, L);
    }
    EXPECT_LT(lomin, 180.0);
    EXPECT_GT(lomax, 180.0);
}

// Waypoints => a smooth Catmull-Rom spline start -> waypoints... -> dest.  The
// spline must pass THROUGH start and dest exactly (grounding on the reef), stay on
// the sea surface, and — with a waypoint on the far side of the antimeridian —
// carry the track across the +/-180 dateline (a reference-frame teaching point).
TEST(SimSelfConsistency, LandfallSplineMeanderCrossesDatelineAndGrounds) {
    sim::TrajectoryConfig c;
    c.kinematic = true;
    c.lat0_deg = -15.20; c.lon0_deg = -179.00;              // start E of 180 (181.0E)
    c.dest_lat_deg = -16.0872; c.dest_lon_deg = -179.158279; // dest  E of 180 (180.84E)
    c.waypoints = { { { -15.50, 179.70 } } };               // waypoint W of 180 (179.7E)
    c.h0_m = 0.0; c.heave_amp_m = 1.0;
    c.interval_s = 30.0; c.n_epochs = 481;
    sim::TrajectoryGenerator g(c);
    const auto& s = g.states();
    ASSERT_EQ(s.size(), 481u);
    EXPECT_NEAR(s.front().lat_deg, -15.20, 1e-4);           // spline passes through the start
    EXPECT_NEAR(s.front().lon_deg, -179.00, 1e-4);
    EXPECT_NEAR(s.back().lat_deg, -16.0872, 1e-4);          // ...and grounds exactly on dest
    EXPECT_NEAR(s.back().lon_deg, -179.158279, 1e-4);
    EXPECT_NEAR(s.back().h_m, 0.0, 1e-2);                   // at the waterline
    EXPECT_LT(s.back().speed_mps, 0.6);                     // grounded (at rest)
    for (const auto& st : s) EXPECT_LT(std::abs(st.h_m), c.heave_amp_m + 0.1);  // on the surface
    // The bow reaches WEST of the antimeridian while the ends sit east of it.
    double lo360min = 1e9, lo360max = -1e9;
    for (const auto& st : s) {
        const double L = std::fmod(st.lon_deg + 360.0, 360.0);
        lo360min = std::min(lo360min, L); lo360max = std::max(lo360max, L);
    }
    EXPECT_LT(lo360min, 180.0);   // west of the dateline (the waypoint)
    EXPECT_GT(lo360max, 180.0);   // east of the dateline (start + dest)
}

// A non-zero landfall_terminal_frac lets the vessel CREEP aground still moving
// (terminal speed ~ tf*Vmax) instead of dead-stopping — this removes a float-PPP
// position spike that keys off the truth speed being exactly 0 at the grounding.
// The speed integral renormalises (Vmax = L/S), so the creep run still grounds on
// the EXACT same destination as the tf=0 dead-stop run.
TEST(SimSelfConsistency, LandfallTerminalFracCreepsAgroundStillMoving) {
    auto make = [](double tf) {
        sim::TrajectoryConfig c;
        c.kinematic = true;
        c.lat0_deg = -16.60; c.lon0_deg = -179.55;             // start: open ocean, E of the dateline
        c.dest_lat_deg = -16.169; c.dest_lon_deg = 179.723;    // grounding: W of the dateline
        c.h0_m = 0.0; c.heave_amp_m = 1.0;
        c.interval_s = 30.0; c.n_epochs = 241;
        c.landfall_terminal_frac = tf;
        return c;
    };
    sim::TrajectoryGenerator gStop(make(0.0));                 // dead stop (the tf=0 default behaviour)
    sim::TrajectoryGenerator gCreep(make(0.15));              // still creeping at grounding
    const auto& stop  = gStop.states();
    const auto& creep = gCreep.states();
    ASSERT_EQ(stop.size(), 241u);
    ASSERT_EQ(creep.size(), 241u);

    // Both ground on the EXACT same destination (the along-track speed renormalises).
    EXPECT_NEAR(creep.back().lat_deg, -16.169, 1e-4);
    EXPECT_NEAR(creep.back().lon_deg, 179.723, 1e-4);
    EXPECT_NEAR(creep.back().lat_deg, stop.back().lat_deg, 1e-5);
    EXPECT_NEAR(creep.back().lon_deg, stop.back().lon_deg, 1e-5);
    // tf=0 still dead-stops; tf>0 is clearly still moving and strictly faster.
    EXPECT_LT(stop.back().speed_mps, 0.6);                     // default: grounded at rest
    EXPECT_GT(creep.back().speed_mps, 1.0);                    // creep: still under way (>= tf*mean speed)
    EXPECT_GT(creep.back().speed_mps, stop.back().speed_mps + 0.5);
    // Both start essentially at rest (identical ramp-up shape; only the terminal differs).
    EXPECT_LT(creep.front().speed_mps, 0.3);
    // Stays on the sea surface throughout (heave floor is a fraction of a metre).
    for (const auto& st : creep) EXPECT_LT(std::abs(st.h_m), 1.0 + 0.1);
}

// Absent dest_* (NaN default) => the analytic heading-weave model runs, NOT
// landfall — the byte-identity guard for every existing scenario.
TEST(SimSelfConsistency, LandfallDisabledByDefault) {
    sim::TrajectoryConfig c;                 // dest_* default to NaN
    c.kinematic = true; c.n_epochs = 10;
    sim::TrajectoryGenerator g(c);
    const auto& s = g.states();
    ASSERT_EQ(s.size(), 10u);
    EXPECT_FALSE(std::isfinite(c.dest_lat_deg));
    EXPECT_GT(std::hypot(s.back().east_m, s.back().north_m), 1.0);  // analytic model moved
}

namespace {
// Write a minimal truth + state_trace fixture; @p pos_err is a constant ECEF
// offset applied to the recovered state, @p std the formal sigma per axis.
void writeFixture(const std::filesystem::path& truth_dir, const std::filesystem::path& logs_dir,
                  double pos_err, double std_axis, double isb_rec, double zwd_rec, int n) {
    std::error_code ec;
    std::filesystem::create_directories(truth_dir, ec);
    std::filesystem::create_directories(logs_dir, ec);
    const double X = -6047592.0, Y = -190053.2, Z = -2010942.2, LAT = -18.5, LON = -178.2;
    const double t0 = 302400.0;

    std::ofstream tp(truth_dir / "truth_position.csv");
    tp << "week,tow_s,x_m,y_m,z_m,lat_deg,lon_deg,h_m,clock_bias_s,heading_deg,speed_mps\n";
    tp << std::fixed << std::setprecision(4); // preserve the cm offset (default 6 sig figs would not)
    for (int i = 0; i < n; ++i)
        tp << "2313," << std::setprecision(3) << (t0 + 30.0 * i) << std::setprecision(4) << "," << X << ","
           << Y << "," << Z << "," << std::setprecision(7) << LAT << "," << LON << std::setprecision(4)
           << ",0,0.0001,35,0\n";

    std::ofstream ts(truth_dir / "truth_scalars.csv");
    ts << "key,value\nkinematic,0\nisb_gps_gal_m,8.0\nzwd_m,0.15\n"
          "storm_window_start_tow_s,0\nstorm_window_end_tow_s,0\n"
          "first_tow_s," << t0 << "\ninterval_s,30\nn_epochs," << n << "\n";

    std::ofstream st(logs_dir / "state_trace.csv");
    st << "week,tow_s,x_m,y_m,z_m,x_std_m,y_std_m,z_std_m,tropo_m,tropo_std_m,"
          "clock_m,clock_std_m,Galileo_m,Galileo_std_m\n";
    st << std::fixed << std::setprecision(4);
    const double e = pos_err / std::sqrt(3.0); // split across axes
    for (int i = 0; i < n; ++i)
        st << "2313," << std::setprecision(3) << (t0 + 30.0 * i) << std::setprecision(4) << ","
           << (X + e) << "," << (Y + e) << "," << (Z + e) << ","
           << std_axis << "," << std_axis << "," << std_axis << "," << zwd_rec << ",0.003,"
           << "29979.0,0.05," << isb_rec << ",0.05\n";
}
} // namespace

// --- Verifier grades each quantity independently. ----------------------------
TEST(SimSelfConsistency, VerifierPassesGoodSolution) {
    const auto base = tmpDir("verify_good");
    writeFixture(base / "truth", base / "logs", /*pos_err=*/0.025, /*std=*/0.03,
                 /*isb=*/8.02, /*zwd=*/0.151, /*n=*/10);
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 2;
    const auto r = sim::Verifier::run(cfg);
    EXPECT_TRUE(r.pass) << "good solution should PASS";
    EXPECT_EQ(r.failures, 0);
}

TEST(SimSelfConsistency, VerifierFailsPositionBias) {
    const auto base = tmpDir("verify_bias");
    writeFixture(base / "truth", base / "logs", /*pos_err=*/1.0, /*std=*/0.03,
                 /*isb=*/8.02, /*zwd=*/0.151, /*n=*/10);
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 2;
    const auto r = sim::Verifier::run(cfg);
    EXPECT_FALSE(r.pass) << "1 m position bias should FAIL";
}

TEST(SimSelfConsistency, VerifierFailsTooPerfect) {
    // Zero error everywhere = the student re-inverted the sim model (circular).
    const auto base = tmpDir("verify_circular");
    writeFixture(base / "truth", base / "logs", /*pos_err=*/0.0, /*std=*/0.03,
                 /*isb=*/8.0, /*zwd=*/0.15, /*n=*/10);
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 2;
    const auto r = sim::Verifier::run(cfg);
    EXPECT_FALSE(r.pass) << "exactly-zero error should trip the anti-circularity gate";
}

// --- verify_report.json: emitted, valid JSON, checks agree with report lines. -
// (SPEC §4.6 — the dashboard displays this; it must round-trip the printed verdict.)
TEST(SimSelfConsistency, VerifyReportJsonGoodRoundTrips) {
    const auto base = tmpDir("verify_json_good");
    writeFixture(base / "truth", base / "logs", /*pos_err=*/0.025, /*std=*/0.03,
                 /*isb=*/8.02, /*zwd=*/0.151, /*n=*/10);
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 2;
    const auto r = sim::Verifier::run(cfg);
    ASSERT_TRUE(r.pass);

    const auto jpath = base / "logs" / "verify_report.json";
    ASSERT_TRUE(sim::Verifier::writeJsonReport(r, jpath));
    std::ifstream f(jpath);
    ASSERT_TRUE(f.is_open());
    nlohmann::json j;
    ASSERT_NO_THROW(f >> j);

    EXPECT_TRUE(j.at("pass").get<bool>());
    EXPECT_EQ(j.at("failures").get<int>(), 0);
    EXPECT_DOUBLE_EQ(j.at("k_sigma").get<double>(), cfg.k_sigma);
    EXPECT_EQ(j.at("converge_skip").get<int>(), cfg.converge_skip);
    ASSERT_TRUE(j.at("checks").is_array());
    EXPECT_GT(j.at("checks").size(), 0u);
    // report_lines round-trip the printed report verbatim.
    EXPECT_EQ(j.at("report_lines").size(), r.report.size());
    // A good solution: every check passes and no line is a per-gate [FAIL].
    for (const auto& c : j.at("checks"))
        EXPECT_TRUE(c.at("pass").get<bool>()) << c.at("id").get<std::string>();
    for (const auto& line : j.at("report_lines"))
        EXPECT_EQ(line.get<std::string>().find("[FAIL]"), std::string::npos);
    // Every check carries an id + units; value/gate are numbers.
    for (const auto& c : j.at("checks")) {
        EXPECT_FALSE(c.at("id").get<std::string>().empty());
        EXPECT_TRUE(c.at("value").is_number());
        EXPECT_TRUE(c.at("gate").is_number());
    }
}

TEST(SimSelfConsistency, VerifyReportJsonBiasedAgreesWithReport) {
    const auto base = tmpDir("verify_json_bias");
    writeFixture(base / "truth", base / "logs", /*pos_err=*/1.0, /*std=*/0.03,
                 /*isb=*/8.02, /*zwd=*/0.151, /*n=*/10);
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 2;
    const auto r = sim::Verifier::run(cfg);
    ASSERT_FALSE(r.pass);

    const auto jpath = base / "logs" / "verify_report.json";
    ASSERT_TRUE(sim::Verifier::writeJsonReport(r, jpath));
    std::ifstream f(jpath);
    nlohmann::json j;
    f >> j;

    EXPECT_FALSE(j.at("pass").get<bool>());
    EXPECT_GT(j.at("failures").get<int>(), 0);
    // A failing position gate shows as a failed position_* check...
    bool pos_check_failed = false;
    for (const auto& c : j.at("checks")) {
        const std::string id = c.at("id").get<std::string>();
        if ((id == "position_median" || id == "position_p90") && !c.at("pass").get<bool>())
            pos_check_failed = true;
    }
    EXPECT_TRUE(pos_check_failed);
    // ...and json.failures equals the count of per-gate [FAIL] report lines.
    int fail_lines = 0;
    for (const auto& line : j.at("report_lines"))
        if (line.get<std::string>().find("[FAIL]") != std::string::npos) ++fail_lines;
    EXPECT_EQ(fail_lines, j.at("failures").get<int>());
}

// --- Verifier degrades gracefully on malformed external state_trace.csv. ------
// (It grades student submissions, so a truncated / missing-column file must fail
//  cleanly, never index out of bounds — the review found two OOB paths here.)
TEST(SimSelfConsistency, VerifierMissingPositionColumnIsGraceful) {
    const auto base = tmpDir("verify_missing_ym");
    writeFixture(base / "truth", base / "logs", 0.03, 0.03, 8.0, 0.15, 5);
    // state_trace whose header lacks y_m/z_m: must fail (not crash on row[-1]).
    { std::ofstream st(base / "logs" / "state_trace.csv");
      st << "week,tow_s,x_m,x_std_m\n2313,302400.000,-6047592.0,0.03\n"; }
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 0;
    const auto r = sim::Verifier::run(cfg);   // must return, not abort
    EXPECT_FALSE(r.pass);
}

TEST(SimSelfConsistency, VerifierTruncatedSigmaRowIsGraceful) {
    const auto base = tmpDir("verify_trunc_row");
    writeFixture(base / "truth", base / "logs", 0.03, 0.03, 8.0, 0.15, 3);
    // Full position header (y_std_m=idx6, z_std_m=idx7), but the middle row is cut
    // off right after x_std_m (6 fields) at a valid tow -> must fall back, not OOB.
    { std::ofstream st(base / "logs" / "state_trace.csv");
      st << "week,tow_s,x_m,y_m,z_m,x_std_m,y_std_m,z_std_m\n"
         << "2313,302400.000,-6047592.0,-190053.2,-2010942.2,0.03,0.02,0.025\n"
         << "2313,302430.000,-6047592.0,-190053.2,-2010942.2,0.03\n"          // truncated
         << "2313,302460.000,-6047592.0,-190053.2,-2010942.2,0.03,0.02,0.025\n"; }
    sim::VerifierConfig cfg;
    cfg.truth_dir = base / "truth";
    cfg.run_logs_dir = base / "logs";
    cfg.converge_skip = 0;
    const auto r = sim::Verifier::run(cfg);   // must return, not abort on row[6]
    EXPECT_FALSE(r.report.empty());
}

// ============================================================================
// Scripted "gotcha" cycle slips (SPEC_scripted_slips.md §6).  All product-free:
// the ScriptedSlipSchedule state machine + the loadSimConfig validator on temp
// JSON, no precise products or processor run.
// ============================================================================

namespace {
sim::ScriptedSlip mkEvent(const std::string& sat, const std::string& sig) {
    sim::ScriptedSlip e;
    e.sat = sat;
    e.signal = sig;
    return e;
}
} // namespace

// §6.1 — Form A fires at its epoch exactly once; repeat=k applies on k consecutive
// calls; a visibility gap stalls (skipped calls => repetitions resume, count k).
TEST(ScriptedSlips, FormAFiresRepeatsAndStalls) {
    auto e = mkEvent("G05", "L1C");
    e.epoch = 10;
    e.delta_cycles = 5;
    e.repeat = 3;
    sim::ScriptedSlipSchedule sched({ e });

    // Wrong (sat,signal) never fires this event.
    EXPECT_TRUE(sched.onEpoch("G05", "L2W", 10, 40.0).empty());
    EXPECT_TRUE(sched.onEpoch("G07", "L1C", 10, 40.0).empty());
    // Before the trigger epoch: nothing.
    for (int ep = 1; ep <= 9; ++ep)
        EXPECT_TRUE(sched.onEpoch("G05", "L1C", ep, 40.0).empty()) << "ep " << ep;

    int applied = 0, sum = 0;
    auto apply = [&](int ep) {
        auto f = sched.onEpoch("G05", "L1C", ep, 40.0);
        ASSERT_EQ(f.size(), 1u);
        EXPECT_EQ(f[0].delta_cycles, 5);
        EXPECT_EQ(f[0].event_index, 0);
        ++applied;
        sum += f[0].delta_cycles;
    };
    apply(10);                 // first repetition (trigger epoch)
    apply(11);                 // second repetition
    // epoch 12: satellite not visible => onEpoch simply is NOT called (stall).
    apply(13);                 // third repetition resumes after the gap
    EXPECT_TRUE(sched.onEpoch("G05", "L1C", 14, 40.0).empty()); // repeat exhausted

    EXPECT_EQ(applied, 3);
    EXPECT_EQ(sum, 15);
    EXPECT_TRUE(sched.unfinished().empty());
}

// §6.1 (corollary) — a form-A event whose exact epoch is never presented (sat not
// visible then) never fires and is reported unfinished.
TEST(ScriptedSlips, FormANeverFiresIfEpochMissed) {
    auto e = mkEvent("G05", "L1C");
    e.epoch = 10;
    e.delta_cycles = 5;
    sim::ScriptedSlipSchedule sched({ e });
    for (int ep : { 8, 9, 11, 12 }) // epoch 10 skipped
        sched.onEpoch("G05", "L1C", ep, 40.0);
    EXPECT_EQ(sched.unfinished().size(), 1u);
}

// §6.2 — Form B fires at the first qualifying (not_before, window) call; the window
// is [lo, hi) and selects the START only; a never-qualifying event is unfinished.
TEST(ScriptedSlips, FormBFiresAtFirstQualifyingCall) {
    auto e = mkEvent("E11", "L5Q");
    e.elev_lo_deg = 10.0;
    e.elev_hi_deg = 15.0;
    e.not_before_epoch = 5;
    e.delta_cycles = 1000;
    sim::ScriptedSlipSchedule sched({ e });

    EXPECT_TRUE(sched.onEpoch("E11", "L5Q", 3, 12.0).empty());  // in window but too early
    EXPECT_TRUE(sched.onEpoch("E11", "L5Q", 6, 20.0).empty());  // late enough, outside window
    EXPECT_TRUE(sched.onEpoch("E11", "L5Q", 6, 15.0).empty());  // hi is exclusive
    {
        auto f = sched.onEpoch("E11", "L5Q", 7, 10.0);          // lo is inclusive => fire
        ASSERT_EQ(f.size(), 1u);
        EXPECT_EQ(f[0].delta_cycles, 1000);
    }
    EXPECT_TRUE(sched.onEpoch("E11", "L5Q", 8, 12.0).empty());  // repeat=1 exhausted
    EXPECT_TRUE(sched.unfinished().empty());
}

TEST(ScriptedSlips, FormBNeverQualifyingIsUnfinished) {
    auto e = mkEvent("E11", "L5Q");
    e.elev_lo_deg = 80.0; // the sat never reaches [80,81) below
    e.elev_hi_deg = 81.0;
    e.delta_cycles = 1000;
    sim::ScriptedSlipSchedule sched({ e });
    for (int ep = 1; ep <= 120; ++ep)
        sched.onEpoch("E11", "L5Q", ep, 15.0);
    EXPECT_EQ(sched.unfinished().size(), 1u);
}

// §6.3 — Same-key same-epoch events each contribute a Fired (caller sums the
// deltas and ORs the LLI flags).
TEST(ScriptedSlips, SameKeySameEpochSumAndLliOr) {
    auto a = mkEvent("G05", "L1C");
    a.epoch = 20;
    a.delta_cycles = 5;
    a.flag_lli = false;
    auto b = mkEvent("G05", "L1C");
    b.epoch = 20;
    b.delta_cycles = 4;
    b.flag_lli = true;
    sim::ScriptedSlipSchedule sched({ a, b });

    auto f = sched.onEpoch("G05", "L1C", 20, 40.0);
    ASSERT_EQ(f.size(), 2u);
    const int sum = f[0].delta_cycles + f[1].delta_cycles;
    const bool any_lli = f[0].lli || f[1].lli;
    EXPECT_EQ(sum, 9);        // MW-blind-pair style: net +9 on this ambiguity
    EXPECT_TRUE(any_lli);     // LLI ORs across same-epoch events
    EXPECT_TRUE(sched.unfinished().empty());
}

// ---- Loader validation (§3 / §6.4-6.5), product-free on temp JSON. ----------
namespace {
bool tryLoadScript(const std::string& body, std::string* err, sim::SimConfig* out) {
    static int seq = 0;
    const auto dir = tmpDir("script_loader");
    const auto path = dir / ("cfg_" + std::to_string(seq++) + ".json");
    { std::ofstream f(path); f << body; }
    sim::SimConfig cfg;
    const bool ok = sim::loadSimConfig(path, cfg, err);
    if (out) *out = cfg;
    return ok;
}
std::string oneEvent(const std::string& ev) {
    return "{\"cycle_slips\":{\"scripted\":[" + ev + "]}}";
}
} // namespace

// §6.5 — absent key and "scripted": [] are equal empty schedules.
TEST(ScriptedSlipsLoader, AbsentAndEmptyAreEqualEmpty) {
    std::string err;
    sim::SimConfig a, b;
    EXPECT_TRUE(tryLoadScript("{}", &err, &a)) << err;
    EXPECT_TRUE(tryLoadScript("{\"cycle_slips\":{\"scripted\":[]}}", &err, &b)) << err;
    EXPECT_TRUE(a.slips.scripted.empty());
    EXPECT_TRUE(b.slips.scripted.empty());
}

// §6.4 — a valid event loads; ints are read tolerantly (20.0 rounds).
TEST(ScriptedSlipsLoader, AcceptsValidWithTolerantInt) {
    std::string err;
    sim::SimConfig cfg;
    ASSERT_TRUE(tryLoadScript(oneEvent(
        "{\"sat\":\"G05\",\"signal\":\"L2W\",\"epoch\":20.0,\"delta_cycles\":-3,\"repeat\":5.0}"),
        &err, &cfg)) << err;
    ASSERT_EQ(cfg.slips.scripted.size(), 1u);
    const auto& e = cfg.slips.scripted[0];
    EXPECT_EQ(e.sat, "G05");
    EXPECT_EQ(e.signal, "L2W");
    EXPECT_EQ(e.epoch, 20);      // 20.0 tolerant-rounded
    EXPECT_EQ(e.repeat, 5);
    EXPECT_EQ(e.delta_cycles, -3);
    // Elevation-window form + a valid Galileo phase + false-alarm form.
    sim::SimConfig cfg2;
    ASSERT_TRUE(tryLoadScript(oneEvent(
        "{\"sat\":\"E11\",\"signal\":\"L5Q\",\"elev_deg\":[10.0,15.0],"
        "\"not_before_epoch\":105,\"delta_cycles\":1000}"), &err, &cfg2)) << err;
    ASSERT_EQ(cfg2.slips.scripted.size(), 1u);
    EXPECT_DOUBLE_EQ(cfg2.slips.scripted[0].elev_lo_deg, 10.0);
    EXPECT_DOUBLE_EQ(cfg2.slips.scripted[0].elev_hi_deg, 15.0);
    EXPECT_EQ(cfg2.slips.scripted[0].not_before_epoch, 105);
}

// §4.5 — delta_cycles == 0 is accepted ONLY with flag_lli=true (LLI false alarm).
TEST(ScriptedSlipsLoader, ZeroDeltaRequiresFlag) {
    std::string err;
    EXPECT_TRUE(tryLoadScript(oneEvent(
        "{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":12,\"delta_cycles\":0,\"flag_lli\":true}"),
        &err, nullptr)) << err;
    EXPECT_FALSE(tryLoadScript(oneEvent(
        "{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":12,\"delta_cycles\":0}"), &err, nullptr));
}

// §6.4 — each rejection case from §3 returns false with a message.
TEST(ScriptedSlipsLoader, RejectsInvalidEvents) {
    std::string err;
    auto rej = [&](const std::string& ev) { return !tryLoadScript(oneEvent(ev), &err, nullptr); };

    EXPECT_TRUE(rej("{\"sat\":\"X05\",\"signal\":\"L1C\",\"epoch\":20,\"delta_cycles\":3}"));   // bad sat letter
    EXPECT_TRUE(rej("{\"sat\":\"G5\",\"signal\":\"L1C\",\"epoch\":20,\"delta_cycles\":3}"));    // bad sat length
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"C1C\",\"epoch\":20,\"delta_cycles\":3}"));   // code, not phase
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L5Q\",\"epoch\":20,\"delta_cycles\":3}"));   // wrong-system phase
    EXPECT_TRUE(rej("{\"sat\":\"E11\",\"signal\":\"L2W\",\"epoch\":20,\"delta_cycles\":3}"));   // wrong-system phase
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20,\"elev_deg\":[10,20],\"delta_cycles\":3}")); // both triggers
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"delta_cycles\":3}"));                // neither trigger
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20,\"not_before_epoch\":5,\"delta_cycles\":3}")); // nbe + epoch
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":0,\"delta_cycles\":3}"));    // epoch < 1
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":121,\"delta_cycles\":3}"));  // epoch > n_epochs
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20,\"delta_cycles\":3,\"repeat\":0}"));   // repeat < 1
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20,\"delta_cycles\":3,\"repeat\":121}")); // repeat > n_epochs
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20,\"delta_cycles\":100001}"));           // |delta| too big
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20}"));                       // delta missing
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"elev_deg\":[20,10],\"delta_cycles\":3}"));  // lo >= hi
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"elev_deg\":[10,95],\"delta_cycles\":3}"));  // hi > 90
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"elev_deg\":[-1,10],\"delta_cycles\":3}"));  // lo < 0
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"elev_deg\":[10],\"delta_cycles\":3}"));     // not a 2-array
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"elev_deg\":[10,20],\"not_before_epoch\":121,\"delta_cycles\":3}")); // nbe out of range
    // Out-of-int-range magnitudes must saturate (not wrap mod 2^32 into a valid range).
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":10,\"delta_cycles\":4294967346,\"flag_lli\":true}")); // 2^32+50 must NOT become +50
    EXPECT_TRUE(rej("{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":4294967306,\"delta_cycles\":3}"));                    // 2^32+10 must NOT become epoch 10
}

// M7 — the full-correction stack flags parse; absent block -> all false (so the
// bare dataset stays byte-identical).  Product-free (loader only).
TEST(SimConfigLoader, ParsesM7CorrectionsFlags) {
    std::string err;
    sim::SimConfig cfg;
    ASSERT_TRUE(tryLoadScript(
        "{\"corrections\":{\"relativity\":true,\"shapiro\":true,"
        "\"solid_earth_tide\":true,\"pole_tide\":true,\"signal_bias\":true}}", &err, &cfg)) << err;
    EXPECT_TRUE(cfg.corrections.relativity);
    EXPECT_TRUE(cfg.corrections.shapiro);
    EXPECT_TRUE(cfg.corrections.solid_earth_tide);
    EXPECT_TRUE(cfg.corrections.pole_tide);
    EXPECT_TRUE(cfg.corrections.signal_bias);
    EXPECT_FALSE(cfg.corrections.sat_antenna);   // unset -> default false

    sim::SimConfig cfg2;
    ASSERT_TRUE(tryLoadScript("{}", &err, &cfg2)) << err;
    EXPECT_FALSE(cfg2.corrections.relativity);
    EXPECT_FALSE(cfg2.corrections.shapiro);
    EXPECT_FALSE(cfg2.corrections.solid_earth_tide);
    EXPECT_FALSE(cfg2.corrections.pole_tide);
    EXPECT_FALSE(cfg2.corrections.signal_bias);   // unset -> default false (byte-identity)
}

// ---------------------------------------------------------------------------
// Along-track ocean tidal loading.  The EOT20 sampling half needs the 286 MB
// grid and so cannot live here; these cover the blending math, which is where
// the silent-corruption risk actually is.
// ---------------------------------------------------------------------------

namespace {
/// One-constituent coefficient set (M2 slot) for the blend tests.
OtlStationCoefficients otlWithM2(double amp_up, double phase_up_deg) {
    OtlStationCoefficients c;
    c.station_name = "TEST";
    c.valid = true;
    c.constituents[0].amp_up   = amp_up;
    c.constituents[0].phase_up = phase_up_deg;
    return c;
}
} // namespace

// Anchors must be reproduced EXACTLY: an epoch that lands on an anchor may not
// drift by a round trip through the complex plane.
TEST(OtlField, LerpEndpointsAreExact) {
    const auto a = otlWithM2(0.0123, 47.5);
    const auto b = otlWithM2(0.0201, -132.25);

    const auto at0 = sim::lerpOtlCoefficients(a, b, 0.0);
    EXPECT_DOUBLE_EQ(at0.constituents[0].amp_up,   a.constituents[0].amp_up);
    EXPECT_DOUBLE_EQ(at0.constituents[0].phase_up, a.constituents[0].phase_up);

    const auto at1 = sim::lerpOtlCoefficients(a, b, 1.0);
    EXPECT_DOUBLE_EQ(at1.constituents[0].amp_up,   b.constituents[0].amp_up);
    EXPECT_DOUBLE_EQ(at1.constituents[0].phase_up, b.constituents[0].phase_up);
}

// THE trap: Greenwich phase lag is circular.  Blending +179 deg with -179 deg
// arithmetically gives 0 deg — a half-cycle error that flips the sign of the
// constituent — and it only bites on the few segments that straddle the wrap.
// Blending through the complex plane must stay near +/-180 and keep the amplitude.
TEST(OtlField, LerpCrossesPhaseWrapWithoutCollapsing) {
    const auto a = otlWithM2(0.02, 179.0);
    const auto b = otlWithM2(0.02, -179.0);

    const auto mid = sim::lerpOtlCoefficients(a, b, 0.5);
    // Amplitude survives (two nearly-equal phasors): |cos(1 deg)| of the input.
    EXPECT_NEAR(mid.constituents[0].amp_up, 0.02 * std::cos(1.0 * Constants::PI / 180.0), 1e-9);
    // Phase stays at the wrap, NOT at 0.
    EXPECT_GT(std::abs(mid.constituents[0].phase_up), 179.0);
    // The naive arithmetic mean would have been 0 deg; prove we are nowhere near it.
    EXPECT_GT(std::abs(mid.constituents[0].phase_up), 90.0);
}

// A blend of two phasors is the phasor of the blended signal: evaluating the
// interpolated coefficients at any argument must equal the interpolated
// evaluation.  This is the property that makes anchor spacing a pure accuracy
// knob rather than a change of model.
TEST(OtlField, LerpIsLinearInTheSignal) {
    const auto a = otlWithM2(0.031, 20.0);
    const auto b = otlWithM2(0.008, 250.0);
    const double f = 0.37;
    const auto m = sim::lerpOtlCoefficients(a, b, f);

    for (double chi = 0.0; chi < 2.0 * Constants::PI; chi += 0.31) {
        auto sig = [chi](const OtlStationCoefficients& c) {
            return c.constituents[0].amp_up *
                   std::cos(chi - c.constituents[0].phase_up * Constants::PI / 180.0);
        };
        EXPECT_NEAR(sig(m), sig(a) + f * (sig(b) - sig(a)), 1e-12) << "chi=" << chi;
    }
}

// Zero-amplitude components (EOT20 carries no horizontals) must stay zero rather
// than picking up a phase from atan2(0, 0).
TEST(OtlField, LerpLeavesAbsentComponentsZero) {
    const auto m = sim::lerpOtlCoefficients(otlWithM2(0.02, 30.0), otlWithM2(0.03, 60.0), 0.5);
    for (std::size_t j = 0; j < m.constituents.size(); ++j) {
        EXPECT_DOUBLE_EQ(m.constituents[j].amp_west,  0.0);
        EXPECT_DOUBLE_EQ(m.constituents[j].amp_south, 0.0);
    }
    EXPECT_TRUE(m.valid);
}

// The along-track field is opt-in and cannot be switched on by itself: silently
// falling back to the frozen (cancelling) path would be invisible in the output.
TEST(SimConfigLoader, OceanTideAlongTrackIsOptInAndRequiresOceanTide) {
    std::string err;
    sim::SimConfig def;
    ASSERT_TRUE(tryLoadScript("{}", &err, &def)) << err;
    EXPECT_FALSE(def.corrections.ocean_tide_along_track);
    EXPECT_DOUBLE_EQ(def.corrections.otl_anchor_spacing_km, 5.0);

    // along_track without ocean_tide is rejected, not silently ignored.
    EXPECT_FALSE(tryLoadScript(
        "{\"corrections\":{\"ocean_tide_along_track\":true}}", &err, nullptr));

    // A non-positive spacing would make anchor placement degenerate.
    EXPECT_FALSE(tryLoadScript(
        "{\"corrections\":{\"ocean_tide\":true,\"eot20_nc_path\":\"x.nc\","
        "\"ocean_tide_along_track\":true,\"otl_anchor_spacing_km\":0}}", &err, nullptr));

    sim::SimConfig on;
    ASSERT_TRUE(tryLoadScript(
        "{\"corrections\":{\"ocean_tide\":true,\"eot20_nc_path\":\"x.nc\","
        "\"ocean_tide_along_track\":true,\"otl_anchor_spacing_km\":2.5}}", &err, &on)) << err;
    EXPECT_TRUE(on.corrections.ocean_tide_along_track);
    EXPECT_DOUBLE_EQ(on.corrections.otl_anchor_spacing_km, 2.5);
}

// §3 — the >64-event paste-accident guard.
TEST(ScriptedSlipsLoader, RejectsMoreThan64Events) {
    std::string arr;
    for (int i = 0; i < 65; ++i) {
        if (i) arr += ",";
        arr += "{\"sat\":\"G05\",\"signal\":\"L1C\",\"epoch\":20,\"delta_cycles\":1}";
    }
    std::string err;
    EXPECT_FALSE(tryLoadScript("{\"cycle_slips\":{\"scripted\":[" + arr + "]}}", &err, nullptr));
}

// ============================================================================
//  RINEX header realism / leak knobs (SPEC R2 / R2.4).  Loader defaults +
//  validation, plus the APPROX-POSITION quantization contract.  The quantization
//  math in quantizeApprox() below MIRRORS runSynth (src/sim/main.cpp): truth start
//  ECEF -> geodetic, round lat/lon to the grid, force h = 0, convert back.  The full
//  end-to-end path (header bytes + OTL x0 following the quantized point) is exercised
//  by the A2 loop in harness/gnss_verify_loop.ps1; here we lock the loader contract
//  and the geodesy so a refactor cannot silently move the disclosed LKP.
// ============================================================================
namespace {
Eigen::Vector3d quantizeApprox(const Eigen::Vector3d& truth, double q_deg) {
    const double d2r = Constants::PI / 180.0;
    const Coordinates::Llh llh = Coordinates::ecefToLlh(truth);
    const double lat_q = std::round((llh.lat_rad / d2r) / q_deg) * q_deg;
    const double lon_q = std::round((llh.lon_rad / d2r) / q_deg) * q_deg;
    // Height rounds to the nearest METRE (not forced to 0): the vessel floats at the sea
    // SURFACE, which sits at +geoid-undulation ellipsoidal height (~+48 m at the hero LKP),
    // and a zeroed header height would put the disclosed fix ~50 m under water.
    return Coordinates::llhToEcef(lat_q * d2r, lon_q * d2r, std::round(llh.h_m));
}
} // namespace

// R2.1 / R2.4 — an absent rinex block and an all-default rinex block both leave the
// defaults, and those defaults ARE today's behaviour (truth APPROX, build date stamp),
// which is the loader half of the byte-identity gate.
TEST(RinexKnobsLoader, DefaultsAreByteIdenticalContract) {
    std::string err;
    sim::SimConfig a, b;
    ASSERT_TRUE(tryLoadScript("{}", &err, &a)) << err;
    ASSERT_TRUE(tryLoadScript(
        "{\"rinex\":{\"approx_position_mode\":\"truth\",\"approx_quantize_deg\":0.001,"
        "\"pgm\":\"\",\"run_by\":\"\",\"observer\":\"\",\"agency\":\"\","
        "\"file_date_mode\":\"build\",\"gps_utc_leap_s\":18}}", &err, &b)) << err;
    for (const sim::SimConfig* c : { &a, &b }) {
        EXPECT_EQ(c->rinex.approx_position_mode, "truth");
        EXPECT_DOUBLE_EQ(c->rinex.approx_quantize_deg, 0.001);
        EXPECT_TRUE(c->rinex.pgm.empty());
        EXPECT_TRUE(c->rinex.run_by.empty());
        EXPECT_TRUE(c->rinex.observer.empty());
        EXPECT_TRUE(c->rinex.agency.empty());
        EXPECT_EQ(c->rinex.file_date_mode, "build");
        EXPECT_EQ(c->rinex.gps_utc_leap_s, 18);
    }
}

// R2 — overrides parse; a typo'd enum / overlong field / non-positive grid is REJECTED,
// never silently accepted (a silent fallback would ship a leaky or malformed header).
TEST(RinexKnobsLoader, ParsesOverridesAndRejectsBadValues) {
    std::string err;
    sim::SimConfig cfg;
    ASSERT_TRUE(tryLoadScript(
        "{\"rinex\":{\"approx_position_mode\":\"quantized\",\"approx_quantize_deg\":0.01,"
        "\"pgm\":\"SARV-LOGGER 1.0\",\"run_by\":\"MV SARV\",\"observer\":\"SARV\","
        "\"agency\":\"South Pacific SAR\",\"file_date_mode\":\"collection_end\","
        "\"gps_utc_leap_s\":18.0}}", &err, &cfg)) << err;
    EXPECT_EQ(cfg.rinex.approx_position_mode, "quantized");
    EXPECT_DOUBLE_EQ(cfg.rinex.approx_quantize_deg, 0.01);
    EXPECT_EQ(cfg.rinex.pgm, "SARV-LOGGER 1.0");
    EXPECT_EQ(cfg.rinex.file_date_mode, "collection_end");
    EXPECT_EQ(cfg.rinex.gps_utc_leap_s, 18);   // 18.0 tolerant-rounded

    EXPECT_FALSE(tryLoadScript("{\"rinex\":{\"approx_position_mode\":\"fuzzy\"}}", &err, nullptr));
    EXPECT_FALSE(tryLoadScript("{\"rinex\":{\"file_date_mode\":\"yesterday\"}}", &err, nullptr));
    EXPECT_FALSE(tryLoadScript("{\"rinex\":{\"approx_quantize_deg\":0}}", &err, nullptr));
    EXPECT_FALSE(tryLoadScript("{\"rinex\":{\"approx_quantize_deg\":-1}}", &err, nullptr));
    EXPECT_FALSE(tryLoadScript(   // pgm overruns the 20-char RINEX field
        "{\"rinex\":{\"pgm\":\"THIS-STRING-IS-WAY-TOO-LONG-FOR-THE-FIELD\"}}", &err, nullptr));
    EXPECT_FALSE(tryLoadScript("{\"rinex\":{\"pgm\":123}}", &err, nullptr));   // not a string
}

// R2.2 / R2.4 — quantized APPROX differs from the exact truth start for an OFF-GRID
// last-known position, stays within the quantization bound, and rounds h to the metre.
TEST(RinexKnobsQuantize, DiffersFromTruthWithinBoundAndHeightRounded) {
    const double q = 0.001, d2r = Constants::PI / 180.0;
    // An off-grid LKP with a non-round height (the exact, sub-mm truth start).
    const Eigen::Vector3d truth = Coordinates::llhToEcef(-15.20074 * d2r, -179.00037 * d2r, 3.2);
    const Eigen::Vector3d qz = quantizeApprox(truth, q);

    // Different from the truth row-1 ECEF (the leak the mode fixes) — metres, not sub-mm.
    EXPECT_GT((qz - truth).norm(), 1.0);

    const Coordinates::Llh tl = Coordinates::ecefToLlh(truth);
    const Coordinates::Llh ql = Coordinates::ecefToLlh(qz);
    const double dlat_deg = (ql.lat_rad - tl.lat_rad) / d2r;
    const double dlon_deg = (ql.lon_rad - tl.lon_rad) / d2r;
    // Each axis rounds by <= q/2 deg...
    EXPECT_LE(std::abs(dlat_deg), q / 2.0 + 1e-9);
    EXPECT_LE(std::abs(dlon_deg), q / 2.0 + 1e-9);
    // ...so the horizontal offset stays under q * 111 km * sqrt(2) (R2.4 bound)...
    const double horiz = std::hypot(dlat_deg * 111000.0,
                                    dlon_deg * 111000.0 * std::cos(tl.lat_rad));
    EXPECT_LT(horiz, q * 111000.0 * std::sqrt(2.0));
    // ...and the disclosed height is the truth height rounded to the metre (3.2 -> 3),
    // NOT forced to 0 — a floating vessel's ellipsoidal height is +N (geoid), not 0.
    EXPECT_NEAR(ql.h_m, 3.0, 1e-6);
    // The geodetically-consistent hero case: a vessel at the sea surface near the LKP
    // (EGM2008 undulation +48.07 m) discloses h = 48 m, sub-metre from the truth height.
    const Eigen::Vector3d afloat = Coordinates::llhToEcef(-15.20074 * d2r, -179.00037 * d2r, 48.07);
    const Coordinates::Llh al = Coordinates::ecefToLlh(quantizeApprox(afloat, q));
    EXPECT_NEAR(al.h_m, 48.0, 1e-6);
}

// R2.2 — a point already ON the grid at h = 0 (round-number coordinates, as the hero
// LKP -15.200/-179.000 happens to be) barely moves: documents why 0.001 deg quantization
// is a near no-op there and a coarser grid or an off-grid start is needed to anonymize.
TEST(RinexKnobsQuantize, OnGridRoundNumbersAreUnchanged) {
    const double d2r = Constants::PI / 180.0;
    const Eigen::Vector3d truth = Coordinates::llhToEcef(-15.200 * d2r, -179.000 * d2r, 0.0);
    EXPECT_LT((quantizeApprox(truth, 0.001) - truth).norm(), 1e-3);   // sub-mm: nothing to hide here
}
