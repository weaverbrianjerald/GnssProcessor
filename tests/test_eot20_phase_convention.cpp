/**
 * @file test_eot20_phase_convention.cpp
 * @brief The phase sign of the EOT20 load grid reader, `OceanTideModel::parseEot20Load`.
 *
 * The grid stores each constituent as hRe = A cos(phi) and hIm = A sin(phi), phi the Greenwich
 * phase lag, so the reader returns phi = atan2(hIm, hRe).  The opposite sign, atan2(-hIm, hRe),
 * reverses every phase: against the FES2014b loading of the stations of the IGS site-information
 * file it misses the M2 phase by a median of 107 deg, and at Wettzell it applies nearly the
 * opposite of the M2 loading.
 *
 * Covered:
 *
 *   1. a synthetic two-by-two grid written by the test: the reader returns the amplitude |Z| and
 *      the phase lag atan2(hIm, hRe) of each BLQ constituent, and zero horizontal loading;
 *   2. the EOT20 file itself, when present (skipped otherwise; `GNSS_EOT20_NC` overrides the
 *      path): at WTZZ00DEU, STPM00SPM and BAKE00CAN the M2, K1 and O1 up phasors agree with the
 *      FES2014b values of igs20_SIF.json (whose reading is pinned against HARDISP in
 *      test_otl_sif_source.cpp and test_otl_translation.cpp) to within 30 % of the SIF
 *      amplitude plus 0.3 mm.  With the reversed sign every one of the nine cases fails.
 *
 * Suite: Eot20PhaseConventionTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include "corrections/OceanTideModel.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <netcdf.h>

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

constexpr double kPi  = 3.14159265358979323846;
constexpr double kD2R = kPi / 180.0;

/// EOT20 index of each BLQ constituent M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa (the grid's own order is
/// 2n2 j1 k1 k2 m2 m4 mf mm n2 o1 p1 q1 s1 s2 sa ssa t2).
constexpr std::array<int, 11> kEot20IndexOfBlq = { 4, 13, 8, 3, 2, 9, 10, 11, 6, 7, 15 };

/// Amplitude [m] and Greenwich phase lag [deg] that the synthetic grid stores for EOT20 index e.
double syntheticAmp(int e)      { return 0.001 * (e + 1); }
double syntheticPhaseDeg(int e) { return -170.0 + 21.0 * e; }   // distinct, both signs, no wrap

/**
 * @brief Write a 17-constituent grid of two longitudes by two latitudes, every corner holding the
 *        same complex value hRe + i hIm = A cos(phi) + i A sin(phi) per constituent.
 * @return True when every netCDF call succeeded.
 */
bool writeSyntheticGrid(const fs::path& path) {
    int ncid = -1;
    if (nc_create(path.string().c_str(), NC_CLOBBER, &ncid) != NC_NOERR) return false;
    int d_const = -1, d_lat = -1, d_lon = -1;
    int v_lon = -1, v_lat = -1, v_re = -1, v_im = -1;
    bool ok = nc_def_dim(ncid, "constituents", 17, &d_const) == NC_NOERR &&
              nc_def_dim(ncid, "lat", 2, &d_lat) == NC_NOERR &&
              nc_def_dim(ncid, "lon", 2, &d_lon) == NC_NOERR;
    const int dims3[3] = { d_const, d_lat, d_lon };
    ok = ok && nc_def_var(ncid, "lon", NC_DOUBLE, 1, &d_lon, &v_lon) == NC_NOERR &&
               nc_def_var(ncid, "lat", NC_DOUBLE, 1, &d_lat, &v_lat) == NC_NOERR &&
               nc_def_var(ncid, "hRe", NC_DOUBLE, 3, dims3, &v_re) == NC_NOERR &&
               nc_def_var(ncid, "hIm", NC_DOUBLE, 3, dims3, &v_im) == NC_NOERR &&
               nc_enddef(ncid) == NC_NOERR;
    const double lon[2] = { 10.0, 11.0 };
    const double lat[2] = { 50.0, 49.0 };   // descending, as in EOT20
    std::vector<double> re(17 * 4), im(17 * 4);
    for (int e = 0; e < 17; ++e) {
        const double p = syntheticPhaseDeg(e) * kD2R;
        for (int c = 0; c < 4; ++c) {
            re[e * 4 + c] = syntheticAmp(e) * std::cos(p);
            im[e * 4 + c] = syntheticAmp(e) * std::sin(p);
        }
    }
    ok = ok && nc_put_var_double(ncid, v_lon, lon) == NC_NOERR &&
               nc_put_var_double(ncid, v_lat, lat) == NC_NOERR &&
               nc_put_var_double(ncid, v_re, re.data()) == NC_NOERR &&
               nc_put_var_double(ncid, v_im, im.data()) == NC_NOERR;
    return nc_close(ncid) == NC_NOERR && ok;
}

/// Difference of two phase angles, wrapped to (-180, 180] deg.
double wrapDeg(double d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d <= 0.0) d += 360.0;
    return d - 180.0;
}

TEST(Eot20PhaseConventionTest, SyntheticGridGivesTheLagOfReAndIm) {
    const fs::path nc = gnss_test::tempPath("eot20_synthetic.nc");
    ASSERT_TRUE(writeSyntheticGrid(nc));

    LogManager lm;
    OceanTideModel model(lm);
    const Eigen::Vector3d site = Coordinates::llhToEcef(49.5 * kD2R, 10.5 * kD2R, 300.0);
    OtlStationCoefficients c;
    ASSERT_TRUE(model.parseEot20Load(nc.string(), site, c));
    ASSERT_TRUE(c.valid);

    for (std::size_t k = 0; k < kEot20IndexOfBlq.size(); ++k) {
        const int e = kEot20IndexOfBlq[k];
        SCOPED_TRACE("BLQ constituent " + std::to_string(k));
        EXPECT_NEAR(c.constituents[k].amp_up, syntheticAmp(e), 1e-12);
        EXPECT_NEAR(wrapDeg(c.constituents[k].phase_up - syntheticPhaseDeg(e)), 0.0, 1e-9);
        EXPECT_EQ(c.constituents[k].amp_west, 0.0);
        EXPECT_EQ(c.constituents[k].amp_south, 0.0);
    }
}

/// One receiver of the check against the SIF: ECEF [m] and the SIF up value (amplitude [mm],
/// Greenwich phase lag [deg]) of M2, K1 and O1, from igs20_SIF.json (FES2014b).
struct SifUp {
    const char* marker;
    Eigen::Vector3d ecef;
    std::array<double, 3> amp_mm;
    std::array<double, 3> phase_deg;
};

/// The BLQ indices of M2, K1 and O1.
constexpr std::array<int, 3> kBlqM2K1O1 = { 0, 4, 5 };

fs::path findEot20Grid() {
    if (const char* v = std::getenv("GNSS_EOT20_NC")) {
        if (fs::exists(v)) return fs::path(v);
    }
    for (const fs::path p : { fs::path("C:/GnssProcessorSupport/products/nc/EOT20_load.nc"),
                              fs::path("data/static/nc/EOT20_load.nc") })
        if (fs::exists(p)) return p;
    return {};
}

TEST(Eot20PhaseConventionTest, RealGridAgreesWithTheSiteFileAtThreeReceivers) {
    const fs::path nc = findEot20Grid();
    if (nc.empty()) GTEST_SKIP() << "EOT20_load.nc not found (set GNSS_EOT20_NC)";

    const std::array<SifUp, 3> sites = {{
        { "WTZZ00DEU", { 4075579.191, 931853.370, 4801569.189 },
          { 5.54, 1.97, 1.88 }, { -89.7, 153.1, 139.5 } },
        { "STPM00SPM", { 2436208.003, -3634804.893, 4624885.547 },
          { 19.83, 4.34, 3.69 }, { 166.4, 42.1, 39.1 } },
        { "BAKE00CAN", { -289834.288, -2756501.166, 5725162.379 },
          { 5.33, 6.76, 4.38 }, { 125.0, 81.6, 72.0 } },
    }};

    LogManager lm;
    OceanTideModel model(lm);
    for (const SifUp& s : sites) {
        OtlStationCoefficients c;
        ASSERT_TRUE(model.parseEot20Load(nc.string(), s.ecef, c)) << s.marker;
        for (std::size_t j = 0; j < kBlqM2K1O1.size(); ++j) {
            const auto& eot = c.constituents[kBlqM2K1O1[j]];
            const std::complex<double> z_eot = std::polar(1000.0 * eot.amp_up, eot.phase_up * kD2R);
            const std::complex<double> z_sif = std::polar(s.amp_mm[j], s.phase_deg[j] * kD2R);
            const double tol_mm = 0.3 * s.amp_mm[j] + 0.3;
            EXPECT_LE(std::abs(z_eot - z_sif), tol_mm)
                << s.marker << " constituent " << kBlqM2K1O1[j] << ": EOT20 "
                << 1000.0 * eot.amp_up << " mm at " << eot.phase_up << " deg, SIF " << s.amp_mm[j]
                << " mm at " << s.phase_deg[j] << " deg (lag in deg, |dZ| in mm)";
        }
    }
}

}  // namespace
}  // namespace GnssProcessor
