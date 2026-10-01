/**
 * @file test_atmosphere_models.cpp
 * @brief Unit tests for the atmosphere model layer (troposphere + ionosphere).
 *
 * Scope: the four atmosphere models whose numerics are checked against an
 * external oracle rather than against a re-derivation of the project's own
 * tables:
 *  - Gpt3                    pressure / temperature from the 5° binary grid
 *  - Vmf3                    empirical mapping function (GPT3 a + SH b/c)
 *  - Troposphere (Niell)     the default non-VMF3 mapping path ("gmf", which
 *                            evaluates the Niell (1996) NMF tables)
 *  - HigherOrderIonosphere   the I₂ / I₃ slant components
 *
 * Depth: conventions and coefficients.  Each suite either reproduces a published
 * or independent reference value to full double precision or asserts a
 * monotonicity / symmetry invariant that a transcription error would break.
 * The Python oracles live in scripts/ (vmf3_reference.py, nmf_reference.py,
 * higher_order_iono_reference.py) and read the same coefficient headers this
 * binary links, so a table edit shows up here.
 *
 * Fixtures: Gpt3Test needs the 5° binary grid under
 * data/static/gpt3/gpt3_5.grd, located through the GNSS_TEST_DATA_DIR macro
 * defined by CMake.  If the grid is absent the suite is skipped, not failed; a
 * grid that is present and fails to load is a failure, not a skip.
 * The other three suites are fully synthetic.
 *
 * @see test_math_core.cpp          Coordinates / TimeUtils / Interpolate
 * @see test_site_displacement.cpp  solid-earth / ocean-pole / atmospheric tide
 * @see test_sh_ionosphere.cpp      spherical-harmonic VTEC estimation
 */

#include <gtest/gtest.h>

#include "atmosphere/Gpt3.hpp"
#include "atmosphere/HigherOrderIonosphere.hpp"
#include "atmosphere/MagneticField.hpp"
#include "atmosphere/Troposphere.hpp"
#include "atmosphere/Vmf3.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define GNSS_TEST_GETPID _getpid
#else
#include <unistd.h>
#define GNSS_TEST_GETPID getpid
#endif

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

// ============================================================================
// Gpt3: pressure/temperature lookup from the 5° grid
// ============================================================================

// Shared GPT3 fixture: spdlog's global logger registry doesn't allow
// re-registering "gnss_processor", and the binary grid load (~1 MB) is
// the same for every test, so one LogManager + one loaded grid is reused
// across the suite.
class Gpt3Test : public ::testing::Test {
protected:
    static LogManager  log_mgr_;
    static Gpt3        model_;
    static bool        have_fixture_;   ///< grid present and load() succeeded
    static bool        grid_present_;   ///< grid file found on disk
    static std::string grid_path_;      ///< resolved grid path, for the failure message

    static void SetUpTestSuite() {
        namespace fs = std::filesystem;
        const std::string data_dir = GNSS_TEST_DATA_DIR;
        if (data_dir.empty()) return;
        fs::path grid = fs::path(data_dir) / "static" / "gpt3" / "gpt3_5.grd";
        std::error_code ec;
        if (!fs::exists(grid, ec)) return;
        grid_path_    = grid.string();
        grid_present_ = true;

        // Per-process log directory.  LogManager::initialize() removes stale
        // .log files from this directory, and the getLogger("gpt3") inside
        // load() then holds gpt3.log open for the rest of the process.  With a
        // fixed shared path, two concurrently running gnss_tests.exe processes
        // contend for that one file.  LogManager treats directory preparation as
        // best effort and does not throw, but concurrent suites have no reason
        // to share the file.
        const auto log_dir = fs::temp_directory_path()
                             / ("gnss_gpt3_test_logs_" + std::to_string(GNSS_TEST_GETPID()));
        fs::create_directories(log_dir, ec);
        log_mgr_.initialize(log_dir, LogLevel::Warn);
        have_fixture_ = model_.load(grid, log_mgr_);
    }

    void SetUp() override {
        if (!grid_present_) GTEST_SKIP() << "GPT3 grid fixture not present";
        // A grid that is present but fails to load is a defect, not a missing
        // fixture, so it fails the test.  A single skip for both cases would
        // report an environmental failure as "fixture not present".
        ASSERT_TRUE(have_fixture_)
            << "GPT3 grid is present at " << grid_path_ << " but Gpt3::load() failed";
    }
};
LogManager  Gpt3Test::log_mgr_;
Gpt3        Gpt3Test::model_;
bool        Gpt3Test::have_fixture_ = false;
bool        Gpt3Test::grid_present_ = false;
std::string Gpt3Test::grid_path_;

// A query at the exact first grid cell (87.5°N, 2.5°E) at the cell's own
// orthometric height should reproduce the harmonic value built from the
// line-2 a0/A1/B1/A2/B2 columns.  The expected value is computed independently
// of the load path, so this checks both the parse order and the harmonic
// evaluation.
TEST_F(Gpt3Test, QueryAtGridCornerMatchesHarmonic) {
    EXPECT_TRUE(model_.isLoaded());

    // Line 2 of gpt3_5.grd, copied verbatim into the expected coefficients:
    //   lat=87.5  lon=2.5
    //   p:  a0=101421   A1=21   B1=409   A2=-217   B2=-122   [Pa]
    //   T:  a0=259.2    A1=-13.2 B1=-6.1 A2=2.6    B2=0.3    [K]
    //   undu=19.36  Hs=-0.00
    constexpr double doy = 100.0;
    constexpr double omega = 2.0 * Constants::PI / 365.25;
    const double c1 = std::cos(omega * doy);
    const double s1 = std::sin(omega * doy);
    const double c2 = std::cos(2.0 * omega * doy);
    const double s2 = std::sin(2.0 * omega * doy);

    const double p_expected_Pa = 101421.0 + 21.0 * c1 + 409.0 * s1 + (-217.0) * c2 + (-122.0) * s2;
    const double T_expected_K  = 259.2 + (-13.2) * c1 + (-6.1) * s1 + 2.6 * c2 + 0.3 * s2;

    // Query at exactly the grid corner so bilinear weights collapse to (1,0,0,0).
    // Use the cell's own orthometric height (h_ellipsoidal = Hs + undu = 19.36 m)
    // so the barometric / lapse reduction is a no-op.
    Gpt3::Result r{};
    ASSERT_TRUE(model_.query(87.5, 2.5, /*h_ellipsoidal_m=*/19.36, doy, r));

    EXPECT_NEAR(r.pressure_mbar, p_expected_Pa * 0.01, 1e-4);
    EXPECT_NEAR(r.temperature_K, T_expected_K, 1e-4);
}

// Sanity: pressure must decrease with height (standard barometric form).
TEST_F(Gpt3Test, PressureDecreasesWithHeight) {
    Gpt3::Result lo{}, hi{};
    ASSERT_TRUE(model_.query(40.0, -75.0,    0.0, 200.0, lo));
    ASSERT_TRUE(model_.query(40.0, -75.0, 3000.0, 200.0, hi));

    // A 3 km altitude difference gives roughly P_hi ≈ 0.7 × P_lo with the
    // standard 5.225 exponent.  The test checks the monotone decrease, not the
    // precise factor.
    EXPECT_LT(hi.pressure_mbar, lo.pressure_mbar);
    EXPECT_GT(hi.pressure_mbar, 0.5 * lo.pressure_mbar);  // sanity floor
    EXPECT_LT(hi.pressure_mbar, 0.9 * lo.pressure_mbar);  // sanity ceiling
}

// Longitude wrap: querying at lon=358° and lon=-2° (same physical
// location) must produce identical pressure and temperature.
TEST_F(Gpt3Test, LongitudeWrapsConsistently) {
    Gpt3::Result a{}, b{};
    ASSERT_TRUE(model_.query(30.0,  358.0, 100.0, 50.0, a));
    ASSERT_TRUE(model_.query(30.0,   -2.0, 100.0, 50.0, b));
    EXPECT_NEAR(a.pressure_mbar, b.pressure_mbar, 1e-9);
    EXPECT_NEAR(a.temperature_K, b.temperature_K, 1e-9);
}

// ============================================================================
// Vmf3: empirical mapping function from GPT3 a + SH-synthesized b/c
// ============================================================================

// End-to-end VMF3 evaluation against an independent Python port of the
// same algorithm (scripts/vmf3_reference.py).  Both implementations
// read the same Vmf3Coefficients.hpp constants, so this pins:
//  - Legendre recursion (V, W tables)
//  - Spherical-harmonic synthesis loop and (n,m) ordering
//  - Seasonal harmonic evaluation
//  - Marini continued fraction
// to the canonical vmf3.m algorithm to full double precision.
TEST(Vmf3Test, MatchesPythonReference) {
    // Inputs intentionally avoid the equator / prime meridian / DOY 0
    // edge cases so the SH terms exercise non-trivial values.
    constexpr double lat = 0.5;                   // rad (~28.6°N)
    constexpr double lon = 1.0;                   // rad (~57.3°E)
    constexpr double doy = 100.0;
    const     double el  = 30.0 * Constants::PI / 180.0;
    constexpr double a_h = 0.00125;
    constexpr double a_w = 0.00060;

    // Reference values from scripts/vmf3_reference.py executed against
    // the same Vmf3Coefficients.hpp this binary links.
    constexpr double mf_d_ref = 1.9926259863958744;
    constexpr double mf_w_ref = 1.9964305176721975;

    double mf_d = 0.0, mf_w = 0.0;
    Vmf3::computeMapping(lat, lon, doy, el, a_h, a_w, mf_d, mf_w);

    EXPECT_NEAR(mf_d, mf_d_ref, 1e-12);
    EXPECT_NEAR(mf_w, mf_w_ref, 1e-12);
}

// Sanity: at zenith both mapping functions reduce to 1 because Marini
// at sin(el)=1 with positive a/b/c collapses to 1.
TEST(Vmf3Test, AtZenithIsUnity) {
    const double el_zenith = Constants::PI / 2.0;
    double mf_d = 0.0, mf_w = 0.0;
    Vmf3::computeMapping(0.7, 0.3, 150.0, el_zenith, 0.00128, 0.00057, mf_d, mf_w);
    EXPECT_NEAR(mf_d, 1.0, 1e-10);
    EXPECT_NEAR(mf_w, 1.0, 1e-10);
}

// Sanity: at low elevation the mapping factor grows large (cosec-like).
// 10° elevation gives 1/sin ≈ 5.76 as a lower bound; mf is slightly
// larger due to the (a,b,c) correction.
TEST(Vmf3Test, GrowsAtLowElevation) {
    const double el_low = 10.0 * Constants::PI / 180.0;
    double mf_d = 0.0, mf_w = 0.0;
    Vmf3::computeMapping(0.7, 0.3, 150.0, el_low, 0.00128, 0.00057, mf_d, mf_w);
    EXPECT_GT(mf_d, 5.0);
    EXPECT_LT(mf_d, 7.0);
    EXPECT_GT(mf_w, 5.0);
    EXPECT_LT(mf_w, 7.0);
}

// ============================================================================
// Niell Mapping Function (default "GMF"/NMF tropo path): convention check
// ============================================================================
//
// Troposphere::getTroposphereMapping in its default (non-VMF3) mode evaluates
// the Niell Mapping Function.  It is checked against an independent Python port
// (scripts/nmf_reference.py) that mirrors cssrlib tropmapfNiell, itself a port
// of RTKLIB tropmapf, and matches POINT's NMF table.
//
// Two conventions are fixed here:
//   1. the hydrostatic seasonal amplitude arrays carry a leading 0.0 at 15°
//      (dropping it shifts every amplitude by one bin), and
//   2. the seasonal combination is avg − amp·cos, not avg + amp·cos.
// A regression in either shifts the low-elevation hydrostatic mapping by about
// 1e-2, far above this test's 1e-6 tolerance.
TEST(NiellMappingTest, MatchesCanonicalReference) {
    LogManager log_mgr;          // not initialized; see the Gpt3Test fixture note above
    GnssConfig config;           // default → Gmf/NMF mapping, no GPT3 grid
    Troposphere tropo(config, log_mgr);

    // doy 129.25 → 2024-05-08 06:00 GPS ; doy 210.0 → 2024-07-28 00:00 GPS.
    const GnssTime t_129{ 2313, 280800.0 };
    const GnssTime t_210{ 2325, 0.0 };

    struct Case {
        const char* label;
        double lat_deg, lon_deg, h_m, el_deg, mfd, mfw;
        const GnssTime* time;
    };
    const std::vector<Case> cases = {
        { "mid45_doy129_el10",   45.0, -75.0, 200.0, 10.0, 5.5517428493167635, 5.657127344716983,  &t_129 },
        { "high70_doy129_el15",  70.0,  10.0, 100.0, 15.0, 3.8018203160938526, 3.8318732264095114, &t_129 },
        { "mid45_doy210_el5",    45.0, -75.0,   0.0,  5.0, 10.105663915673498, 10.750884210392691, &t_210 },
        { "south40_doy210_el20",-40.0, 150.0,  50.0, 20.0, 2.897534032127845,  2.9112923615249278, &t_210 },
        { "trop15_doy210_el8",   15.0, 100.0,   0.0,  8.0, 6.7889522601127394, 6.9911693990787045, &t_210 },
    };

    constexpr double DEG2RAD = Constants::PI / 180.0;
    for (const auto& c : cases) {
        const Eigen::Vector3d rx =
            Coordinates::llhToEcef(c.lat_deg * DEG2RAD, c.lon_deg * DEG2RAD, c.h_m);
        double mf_d = 0.0, mf_w = 0.0;
        tropo.getTroposphereMapping(c.el_deg, rx, *c.time, mf_d, mf_w);

        EXPECT_NEAR(mf_d, c.mfd, 1e-6) << "case " << c.label << " (mf_d)";
        EXPECT_NEAR(mf_w, c.mfw, 1e-6) << "case " << c.label << " (mf_w)";
    }
}

// ============================================================================
// Higher-order ionosphere (I₂ + I₃): coefficient and convention check
// ============================================================================
//
// HigherOrderIonosphere::slantHigherOrderComponents is checked against an
// independent Python oracle (scripts/higher_order_iono_reference.py) that
// derives the coefficients from first principles and the POINT reference
// (higher_order_iono/Class_Iono.cpp).  The dipole B-field constants are shared
// with the engine (MagneticField); the I₂/I₃ coefficients are derived
// independently, so three conventions are fixed here:
//
//   1. The I₂ group (code) coefficient is 7527·c, not 7527·c/2.  The /2 form is
//      the phase coefficient; using it for code makes both code and phase I₂ a
//      factor of two too small.  This case's I₂ would change by about 4e-3 m at
//      L1 on such a regression, about 4e6 times the 1e-9 m tolerance.
//   2. The I₃ group (code) term scales with ∫N_e²dl = η·N_max·STEC, not
//      η·STEC².  The STEC² form is about 2.3e5 times too large.
//   3. The I₂ sign: B is projected on the propagation direction k̂ (satellite
//      to receiver), not on the receiver-to-satellite line of sight (card I2S,
//      docs/refound/I2S_second_order_iono_sign_2026-10-01.md).  Here B points
//      north and down at the 40° N pierce point and k̂ points mostly down, so
//      B·k̂ > 0 and the code I₂ is a positive delay; the line-of-sight form
//      flips every I₂ golden below.
//
// Internal consistency the goldens also encode: phase = −½·I₂code and
// −⅓·I₃code; L2/L1 ratios are (f₁/f₂)³ for I₂ and (f₁/f₂)⁴ for I₃.
TEST(HigherOrderIonosphereTest, MatchesCanonicalReference) {
    const HigherOrderIonosphere hoi;  // stateless

    // Same fixed geometry as the Python oracle (explicit ECEF literals so the
    // test does not depend on the WGS84 conversion matching Python's).
    const Eigen::Vector3d rx(4.6e6, 0.8e6, 4.3e6);
    const Eigen::Vector3d sat(1.5e7, 1.0e7, 2.0e7);
    const double ipp_lat = 0.70;                       // rad
    const double ipp_lon = 0.18;                       // rad
    const double ipp_radius = (6371.0 + 450.0) * 1.0e3;  // m
    const double f_l1 = Constants::GPS_L1_FREQ;
    const double f_l2 = Constants::GPS_L2_FREQ;

    struct Case { const char* band; char meas; double freq; double stec; double i2; double i3; };
    const std::vector<Case> cases = {
        { "L1", 'C', f_l1,  50.0,  0.007714137670442714,  0.000288285519644309   },
        { "L1", 'L', f_l1,  50.0, -0.003857068835221357, -9.609517321476967e-05  },
        { "L2", 'C', f_l2,  50.0,  0.016304437097686218,  0.0007819531397965047  },
        { "L1", 'C', f_l1, 120.0,  0.018513930409062514,  0.00166052459315122    },
    };

    for (const auto& c : cases) {
        const auto comp = hoi.slantHigherOrderComponents(
            rx, sat, ipp_lat, ipp_lon, ipp_radius, c.stec, c.freq, c.meas);
        ASSERT_TRUE(comp.valid) << "case " << c.band << "/" << c.meas;
        EXPECT_NEAR(comp.i2_m, c.i2, 1e-9) << "case " << c.band << "/" << c.meas << " (I2)";
        EXPECT_NEAR(comp.i3_m, c.i3, 1e-9) << "case " << c.band << "/" << c.meas << " (I3)";
    }
}

// The I₂ sign stated as physics, without an oracle (card I2S).  A right-hand
// circularly polarized signal travelling along B is the R (extraordinary)
// mode: extra group delay on code (I₂ > 0) and half as much phase advance.
// Travelling against B it is the L mode and both signs reverse.  The geometry
// places the receiver and the satellite on the field line through the pierce
// point, so the propagation direction is ±B̂ exactly and |I₂| = 7527·c·STEC·|B|/f³.
// A shared-convention oracle cannot catch a flipped direction vector; this can.
TEST(HigherOrderIonosphereTest, SignFollowsPropagationAlongField) {
    const HigherOrderIonosphere hoi;

    const double ipp_lat = 0.70;                       // rad
    const double ipp_lon = 0.18;                       // rad
    const double ipp_radius = (6371.0 + 450.0) * 1.0e3;  // m
    const Eigen::Vector3d ipp(ipp_radius * std::cos(ipp_lat) * std::cos(ipp_lon),
                              ipp_radius * std::cos(ipp_lat) * std::sin(ipp_lon),
                              ipp_radius * std::sin(ipp_lat));
    const Eigen::Vector3d B = MagneticField::fieldAtEcef(ipp);
    const Eigen::Vector3d b_hat = B.normalized();

    const double stec_tecu = 50.0;
    const double f = Constants::GPS_L1_FREQ;
    const double expected = 7527.0 * Constants::SPEED_OF_LIGHT
                          * stec_tecu * 1.0e16 * B.norm() / (f * f * f);

    // Along B: the satellite up-field, the receiver down-field of the pierce point.
    const Eigen::Vector3d sat_up = ipp - 2.0e7 * b_hat;
    const Eigen::Vector3d rx_dn  = ipp + 4.0e5 * b_hat;
    const auto along_c = hoi.slantHigherOrderComponents(
        rx_dn, sat_up, ipp_lat, ipp_lon, ipp_radius, stec_tecu, f, 'C');
    const auto along_l = hoi.slantHigherOrderComponents(
        rx_dn, sat_up, ipp_lat, ipp_lon, ipp_radius, stec_tecu, f, 'L');
    ASSERT_TRUE(along_c.valid);
    ASSERT_TRUE(along_l.valid);
    EXPECT_GT(along_c.i2_m, 0.0) << "code I2 along B must be a delay";
    EXPECT_LT(along_l.i2_m, 0.0) << "phase I2 along B must be an advance";
    EXPECT_NEAR(along_c.i2_m, expected, 1e-12);
    EXPECT_NEAR(along_l.i2_m, -0.5 * expected, 1e-12);

    // Against B: the same field line, the path reversed.
    const Eigen::Vector3d sat_dn = ipp + 2.0e7 * b_hat;
    const Eigen::Vector3d rx_up  = ipp - 4.0e5 * b_hat;
    const auto against_c = hoi.slantHigherOrderComponents(
        rx_up, sat_dn, ipp_lat, ipp_lon, ipp_radius, stec_tecu, f, 'C');
    ASSERT_TRUE(against_c.valid);
    EXPECT_LT(against_c.i2_m, 0.0) << "code I2 against B must be an advance";
    EXPECT_NEAR(against_c.i2_m, -expected, 1e-12);

    // I₃ does not depend on the direction.
    EXPECT_NEAR(along_c.i3_m, against_c.i3_m, 1e-15);
}

}  // namespace
}  // namespace GnssProcessor
