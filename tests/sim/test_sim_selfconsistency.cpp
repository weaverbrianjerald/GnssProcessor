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

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

#include "sim/IonoDisturbance.hpp"
#include "sim/RinexObsWriter.hpp"
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
