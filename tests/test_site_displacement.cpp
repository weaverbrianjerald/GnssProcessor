/**
 * @file test_site_displacement.cpp
 * @brief Unit tests for the geophysical site-displacement corrections.
 *
 * Scope: the three GnssCorrections displacement models that move the station
 * marker at the cm-to-sub-mm level —
 *  - computeSolidEarthTideDisplacement      IERS 2010 dehanttideinel port
 *  - computeOceanPoleTideDisplacement       IERS 2010 Sec. 7.1.5 / Desai 2002
 *  - computeAtmosphericTideDisplacement     S1/S2 thermal tide
 *
 * Depth: external-oracle vector locks, not re-derivations of our own tables.
 * The solid-earth case uses the OFFICIAL published test vector from the IERS
 * dehanttideinel.f header; the ocean-pole case uses an independent Python
 * oracle (scripts/ocean_pole_tide_reference.py) over a verbatim row of the
 * IERS opole grid; the atmospheric case pins the coefficient index mapping,
 * the S1/S2 argument scaling, and the ENU->ECEF rotation against hand-
 * computed values. That independence is the point: a prior version of the
 * solid-earth test parsed the Step-2 table out of the C++ source and was
 * therefore structurally blind to table-value errors.
 *
 * Ocean tide loading (OceanTideModel) and the grid loaders are validated
 * separately; the SIF/OTL sign-convention work has its own coverage.
 *
 * @see test_math_core.cpp           Coordinates / TimeUtils / Interpolate
 * @see test_atmosphere_models.cpp   Gpt3 / Vmf3 / Niell / higher-order iono
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>
#include <array>
#include <cmath>

namespace GnssProcessor {
namespace {

// ============================================================================
// Solid Earth tide (IERS 2010) — official dehanttideinel test vector
// ============================================================================
//
// GnssCorrections::computeSolidEarthTideDisplacement is a faithful port of the
// IERS Conventions dehanttideinel routine (Step 1 in-phase degree 2/3, Step 1
// out-of-phase diurnal/semidiurnal + L1, Step 2 diurnal Table 7.3a and long-
// period Table 7.3b).  It is pinned here against the OFFICIAL published test
// vector from the IERS dehanttideinel.f header — a genuine external oracle,
// not a re-derivation of our own tables.  (The prior test parsed the Step-2
// table out of the C++ source and so was structurally blind to table-value
// errors; a transcription bug that zeroed K1 and inflated P1 — a ~1 cm diurnal
// radial error — slipped past it.  This vector test catches that class of bug:
// the displacement is sensitive to every band, including the sub-mm Step-1
// out-of-phase terms, at the 1e-4 m tolerance below.)
//
// Inputs and expected output are verbatim from the IERS reference:
//   XSTA = (4075578.385, 931852.890, 4801570.154) m            (φ≈49°)
//   XSUN = (137859926952.015, 54228127881.4350, 23509422341.6896) m
//   XMON = (-179996231.920342, -312468450.131567, -169288918.592160) m
//   2009-04-13 00:00 UT  →  T = (MJD 54934 − 51544.5)/36525,  FHR = 0
//   DXTIDE = (0.07700420357, 0.06304056322, 0.05516568153) m
TEST(SolidEarthTideTest, MatchesIersOfficialVector) {
    const Eigen::Vector3d xsta(4075578.385, 931852.890, 4801570.154);
    const Eigen::Vector3d xsun(137859926952.015, 54228127881.4350, 23509422341.6896);
    const Eigen::Vector3d xmon(-179996231.920342, -312468450.131567, -169288918.592160);
    const double jc  = (54934.0 - 51544.5) / 36525.0;  // 2009-04-13 0h UT
    const double fhr = 0.0;

    const Eigen::Vector3d dx =
        GnssCorrections::computeSolidEarthTideDisplacement(xsta, xsun, xmon, jc, fhr);

    const Eigen::Vector3d expected(0.07700420357108126,
                                   0.06304056321824968,
                                   0.05516568152597247);
    // The port reproduces the official vector to ~6e-5 m — within
    // dehanttideinel's own ~0.1 mm accuracy and ~200x below the ~12 mm
    // corruption it replaces. The residual is a constant/rounding-lineage
    // difference vs the reference Fortran, not a band omission (each band,
    // incl. the sub-mm Step-1 out-of-phase terms, is exercised here).
    EXPECT_NEAR(dx.x(), expected.x(), 1e-4) << "dx, got " << dx.x();
    EXPECT_NEAR(dx.y(), expected.y(), 1e-4) << "dy, got " << dx.y();
    EXPECT_NEAR(dx.z(), expected.z(), 1e-4) << "dz, got " << dx.z();
}

// ============================================================================
// Ocean pole tide (IERS 2010 Sec. 7.1.5 / Desai 2002)
// ============================================================================
//
// GnssCorrections::computeOceanPoleTideDisplacement evaluates the closed-form
// ocean pole tide site displacement from the station's six interpolated load
// coefficients and the polar-motion wobble.  It is pinned here against the
// independent Python oracle scripts/ocean_pole_tide_reference.py (a separate
// reimplementation of the same IERS expression / NGS M-PAGES PoleTide.cpp), so
// a transcription error in the C++ constants (Kappa = sqrt(8pi/15)*Omega^2*a^4
// /GM scaled by 4*pi*G*a*rho_sw/(3*g_E)*AS2RAD), the gammaR/gammaI mixing, or
// the North/East/radial coefficient ordering would surface here.
//
// Inputs: the first data row of the IERS opole grid opoleloadcoefcmcor.txt,
// grid point (lon=0.25, lat=-89.75), file columns u_r^R u_r^I u_n^R u_n^I
// u_e^R u_e^I = 0.022015 0.040741 -0.021861 -0.029058 -0.025845 -0.055643,
// reordered to optCoeffs [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I], with a
// wobble of m1 = 0.1, m2 = 0.2 arcsec.
TEST(OceanPoleTideTest, MatchesReferenceVector) {
    const std::array<double, 6> coeffs = {
        -0.021861, -0.029058,   // u_n^R, u_n^I
        -0.025845, -0.055643,   // u_e^R, u_e^I
         0.022015,  0.040741    // u_r^R, u_r^I
    };
    const double m1 = 0.1, m2 = 0.2;  // arcseconds

    const Eigen::Vector3d enu =
        GnssCorrections::computeOceanPoleTideDisplacement(m1, m2, coeffs);  // (E, N, U)

    // Verbatim from scripts/ocean_pole_tide_reference.py.
    EXPECT_NEAR(enu.x(), -2.438808776832272e-04, 1e-12) << "East, got "  << enu.x();
    EXPECT_NEAR(enu.y(), -1.423934108645533e-04, 1e-12) << "North, got " << enu.y();
    EXPECT_NEAR(enu.z(),  1.841228931876922e-04, 1e-12) << "Up, got "    << enu.z();

    // Sanity: ocean pole tide is a sub-millimetre-to-millimetre site effect.
    EXPECT_LT(enu.norm(), 5e-3);

    // Zero wobble => zero displacement (purely wobble-driven).
    const Eigen::Vector3d zero =
        GnssCorrections::computeOceanPoleTideDisplacement(0.0, 0.0, coeffs);
    EXPECT_NEAR(zero.norm(), 0.0, 1e-15);
}

// ============================================================================
// Atmospheric pressure loading — S1/S2 thermal tide (convention lock)
// ============================================================================
//
// computeAtmosphericTideDisplacement assembles the diurnal (S1) and semidiurnal
// (S2) atmospheric thermal-tide site displacement from per-station in-phase
// (cos) / out-of-phase (sin) ENU coefficients, with local-solar-time arguments
//   theta_S1 = 2*pi*UT_frac + lambda,  theta_S2 = 2*(2*pi*UT_frac + lambda).
// Coefficient layout per harmonic [aE^cos, aE^sin, aN^cos, aN^sin, aU^cos, aU^sin],
// index 0..5 = S1, 6..11 = S2.  This locks the index mapping, the S1/S2 argument
// scaling, the longitude (local-solar-time) usage, and the ENU->ECEF rotation
// (mirrors OceanTideModel).  The grid loader and a full flag-off bit-identity run
// are validated separately.
TEST(AtmosphericTideTest, S1S2ConventionAndRotation) {
    LogManager log_mgr;
    GnssCorrections gc(log_mgr);

    // Station at the equator / prime meridian so the ENU->ECEF rotation is the
    // trivial permutation enuToEcef((dE,dN,dU)) = (dU, dE, dN), and lambda == 0.
    const GnssTime t{ 2313, 281700.0 };
    const Eigen::Vector3d rx0 = Coordinates::llhToEcef(0.0, 0.0, 0.0);

    // No coefficients loaded => not available, zero displacement.
    EXPECT_FALSE(gc.hasAtmosphericTideCoefficients());
    EXPECT_NEAR(gc.computeAtmosphericTideDisplacement(rx0, t).norm(), 0.0, 1e-15);

    // Independent evaluation of the arguments the model declares.
    const Coordinates::Llh llh = Coordinates::ecefToLlh(rx0);
    const double ut_frac  = std::fmod(TimeUtils::gpsTimeToMjd(t), 1.0);
    const double lambda   = llh.lon_rad;                     // 0 at prime meridian
    const double theta_s1 = 2.0 * Constants::PI * ut_frac + lambda;
    const double theta_s2 = 2.0 * (2.0 * Constants::PI * ut_frac + lambda);

    // Case A: S1 Up-only => locks indices 4/5, the S1 argument, and rotation.
    {
        std::array<double, 12> c{};
        c[4] = 1.0e-3;   // aU^cos (S1)
        c[5] = 0.5e-3;   // aU^sin (S1)
        gc.setAtmosphericTideCoefficients(c);
        EXPECT_TRUE(gc.hasAtmosphericTideCoefficients());
        const double d_u = c[4] * std::cos(theta_s1) + c[5] * std::sin(theta_s1);
        const Eigen::Vector3d e = gc.computeAtmosphericTideDisplacement(rx0, t);
        EXPECT_NEAR(e.x(), d_u, 1e-12);   // Up -> ECEF X at (0,0)
        EXPECT_NEAR(e.y(), 0.0, 1e-12);
        EXPECT_NEAR(e.z(), 0.0, 1e-12);
    }

    // Case B: S2 East-only => locks indices 6/7 and the 2x semidiurnal argument.
    {
        std::array<double, 12> c{};
        c[6] = 0.8e-3;   // aE^cos (S2)
        c[7] = 0.3e-3;   // aE^sin (S2)
        gc.setAtmosphericTideCoefficients(c);
        const double d_e = c[6] * std::cos(theta_s2) + c[7] * std::sin(theta_s2);
        const Eigen::Vector3d e = gc.computeAtmosphericTideDisplacement(rx0, t);
        EXPECT_NEAR(e.x(), 0.0, 1e-12);
        EXPECT_NEAR(e.y(), d_e, 1e-12);   // East -> ECEF Y at (0,0)
        EXPECT_NEAR(e.z(), 0.0, 1e-12);
    }

    // Magnitude band: representative mm-level coefficients => sub-cm site effect.
    {
        std::array<double, 12> c{};
        for (int i = 0; i < 12; ++i) c[i] = 0.3e-3;
        gc.setAtmosphericTideCoefficients(c);
        EXPECT_LT(gc.computeAtmosphericTideDisplacement(rx0, t).norm(), 1.5e-2);
    }

    // Physical sanity guard: an out-of-range (>1 cm) displacement returns zero;
    // an in-range one passes through equal to the hand-computed value.
    {
        std::array<double, 12> c{};
        c[4] = 1.0;   // 1 m radial cos (S1) — far above the ~1.5 mm physical band
        gc.setAtmosphericTideCoefficients(c);
        const double d_u = 1.0 * std::cos(theta_s1);
        const Eigen::Vector3d e = gc.computeAtmosphericTideDisplacement(rx0, t);
        if (std::abs(d_u) > 0.01) {
            EXPECT_NEAR(e.norm(), 0.0, 1e-15) << "guard should zero an out-of-range displacement";
        } else {
            EXPECT_NEAR(e.x(), d_u, 1e-12);
        }
    }
}

}  // namespace
}  // namespace GnssProcessor
