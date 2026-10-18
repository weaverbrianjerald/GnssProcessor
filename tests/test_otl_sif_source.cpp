/**
 * @file test_otl_sif_source.cpp
 * @brief Ocean tide loading from the SIF registry alone (`model_corrections.otl_from_sif`)
 *        and the nodal corrections (`model_corrections.otl_nodal_corrections`).
 *
 * Covered:
 *
 *   1. the loader: both keys default off; `otl_from_sif` loads the SIF registry while
 *      ocean tide loading is on, without arming `processing.enable_station_info`; the
 *      registry stays unloaded when the key is off or the ocean tide loading master is
 *      off, even with an explicit SIF path (the default path stays byte-identical);
 *   2. the SIF mapping: the radial column is copied in both modes; `BlqRows` copies the
 *      east and north columns unchanged into the west and south rows, `LegacyShifted`
 *      adds 180 degrees to both phases and so reverses the horizontal vector exactly;
 *   3. the nodal factors: the side-line series agrees with the closed forms usually
 *      quoted (M2, K1, O1, Mf) and is the identity outside the 11 constituents;
 *   4. the evaluation: with the corrections off the displacement equals an independent
 *      re-implementation of the original model; with them on, a long-period constituent
 *      differs by exactly its nodal factor and a semidiurnal one by the 18 s of
 *      GPS-minus-UTC in the Greenwich argument;
 *   5. a regression against the IERS HARDISP reference (NGS M-PAGES v1.2, evaluated by
 *      scripts/otl_compare/hardisp_port.py) on SIF-sized coefficients.
 *
 * Suite: OtlSifSourceTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include "common/StationInfo.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/OceanTideModel.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "products/StationInfoFile.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <nlohmann/json.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr double kPi  = 3.14159265358979323846;
constexpr double kD2R = kPi / 180.0;

// A mid-latitude coastal site (Onsala): ECEF position used by every evaluation below.
const Eigen::Vector3d kSite(3370658.5, 711877.0, 5349786.9);

/// A SIF station record with a distinct value in every OTL column.
StationInfo syntheticStation() {
    StationInfo s;
    s.id = "SYNT00XXX";
    s.has_otl = true;
    for (std::size_t k = 0; k < 11; ++k) {
        SifOtlConstituent& c = s.otl[k];
        c.up_amp      = 0.0100 + 0.0010 * k;   c.up_phase    = -100.0 + 7.0 * k;
        c.east_amp    = 0.0020 + 0.0001 * k;   c.east_phase  =   30.0 + 11.0 * k;
        c.north_amp   = 0.0030 + 0.0002 * k;   c.north_phase = -120.0 + 13.0 * k;
    }
    return s;
}

/// Coefficients with only constituent @p only set (Up amplitude 10 mm, phase 25 degrees).
OtlStationCoefficients singleConstituent(std::size_t only, double amp = 0.010) {
    OtlStationCoefficients c;
    c.station_name = "SINGLE";
    c.valid = true;
    c.constituents[only].amp_up = amp;
    c.constituents[only].phase_up = 25.0;
    return c;
}

/// Local east, north, up displacement of @p model at @p t.
Eigen::Vector3d enuAt(const OceanTideModel& model, const GnssTime& t) {
    const Eigen::Vector3d d = model.computeDisplacement(kSite, t);
    return Coordinates::ecefToEnu(d, Coordinates::ecefToLlh(kSite));
}

GnssTime utc(int y, int mo, int d, int h, int mi, double s) {
    return TimeUtils::gregorianToGpsTime(y, mo, d, h, mi, s);
}

/// @p t advanced by @p seconds, rolling the GPS week.
GnssTime advanced(GnssTime t, double seconds) {
    t.tow += seconds;
    while (t.tow >= 604800.0) { t.tow -= 604800.0; ++t.week; }
    return t;
}

/// Independent re-implementation of the original (nodal off) evaluation for the
/// constituents M2 (0), K1 (4), O1 (5) and Mf (8): the Delaunay series, a GMST taken
/// from the GPS Julian date, the Doodson multipliers with the quarter-cycle species bias,
/// and the ENU rotation.  Returns the ECEF displacement.
Eigen::Vector3d referenceOriginal(const OtlStationCoefficients& coeffs, const GnssTime& time) {
    const double T  = TimeUtils::gpsTimeToTtCenturies(time);
    const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
    const double as2r = kD2R / 3600.0;
    auto poly = [&](double c0, double c1, double c2, double c3, double c4) {
        return std::fmod(c0 + c1 * T + c2 * T2 + c3 * T3 + c4 * T4, 1296000.0) * as2r;
    };
    const double l  = poly(485868.249036, 1717915923.2178, 31.8792, 0.051635, -0.00024470);
    const double F  = poly(335779.526232, 1739527262.8478, -12.7512, -0.001037, 0.00000417);
    const double D  = poly(1072260.70369, 1602961601.2090, -6.3706, 0.006593, -0.00003169);
    const double Om = poly(450160.398036, -6962890.5431, 7.4722, 0.007702, -0.00005939);
    const double jd = TimeUtils::J2000_JD + T * TimeUtils::DAYS_PER_JULIAN_CENTURY
                    - TimeUtils::TT_GPS_OFFSET / TimeUtils::SECONDS_IN_DAY;
    const double gmst = std::fmod(280.46061837 + 360.98564736629 * (jd - TimeUtils::J2000_JD)
                                  + 0.000387933 * T2 - T3 / 38710000.0, 360.0) * kD2R;
    const double s = F + Om, h = s - D, p = s - l, N = -Om;
    const double vars[5] = { gmst + kPi, s, h, p, N };
    struct Row { std::size_t j; int m[5]; double bias; };
    static const Row rows[] = {
        { 0, { 2, -2, 0, 0, 0 }, 0.0 }, { 4, { 1, 0, 0, 0, 0 }, 0.25 },
        { 5, { 1, -2, 0, 0, 0 }, -0.25 }, { 8, { 0, 2, 0, 0, 0 }, 0.0 } };
    double up = 0.0, west = 0.0, south = 0.0;
    for (const Row& r : rows) {
        double chi = 0.0;
        for (int k = 0; k < 5; ++k) chi += r.m[k] * vars[k];
        chi += r.bias * 2.0 * kPi;
        chi = std::fmod(chi, 2.0 * kPi);
        if (chi < 0.0) chi += 2.0 * kPi;
        const auto& c = coeffs.constituents[r.j];
        up    += c.amp_up    * std::cos(chi - c.phase_up    * kD2R);
        west  += c.amp_west  * std::cos(chi - c.phase_west  * kD2R);
        south += c.amp_south * std::cos(chi - c.phase_south * kD2R);
    }
    const Eigen::Vector3d enu(-west, -south, up);
    return Coordinates::enuToEcef(enu, Coordinates::ecefToLlh(kSite));
}

/// Load a config body (a minimal PPP post-processing config plus @p extra members).
bool loadConfig(GnssConfig& cfg, LogManager& lm, const std::string& tag, const json& extra) {
    json doc = { { "processing", { { "mode", "ppp" } } } };
    for (auto it = extra.begin(); it != extra.end(); ++it) doc[it.key()] = it.value();
    const fs::path p = gnss_test::tempDir() / (tag + ".json");
    { std::ofstream f(p); f << doc.dump(2); }
    return cfg.loadFromJson(p, lm);
}

/// A one-station SIF with an OTL block, written to the test scratch directory.
fs::path writeSif() {
    json st = {
        { "ID", "SYNT00XXX" }, { "DOMES", "00000M000" }, { "DESCRIPTION", "synthetic" },
        { "APPROXIMATE_COORDINATES", json::array({ kSite.x(), kSite.y(), kSite.z() }) },
        { "ANTENNA", json::array() }, { "RECEIVER", json::array() },
        { "OCCUPATION", json::array() }, { "NONLINEAR_TERMS", json::object() } };
    json otl = { { "COMMENT", json::array({ "synthetic" }) } };
    for (const char* n : SIF_OTL_CONSTITUENTS)
        otl[n] = { { "UP_AMPLITUDE", 0.01 }, { "EAST_AMPLITUDE", 0.002 }, { "NORTH_AMPLITUDE", 0.003 },
                   { "UP_PHASE", -90.0 }, { "EAST_PHASE", 120.0 }, { "NORTH_PHASE", 100.0 } };
    st["OCEAN_TIDE_LOADING"] = otl;
    json doc = { { "METADATA", { { "REFERENCE_FRAME", "ITRF2020" }, { "REFERENCE_EPOCH", 2020.0 },
                                 { "COORDINATE_UNITS", "m" }, { "VELOCITY_UNITS", "mm/y" } } },
                 { "STATIONS", json::array({ st }) } };
    const fs::path p = gnss_test::tempDir() / "otl_synthetic_SIF.json";
    { std::ofstream f(p); f << doc.dump(2); }
    return p;
}

std::string fwd(const fs::path& p) {
    std::string s = p.string();
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The loader
// ---------------------------------------------------------------------------

TEST(OtlSifSourceTest, KeysDefaultOffAndLoadNoRegistry) {
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig(cfg, lm, "otl_default", json::object()));
    EXPECT_FALSE(cfg.getModelCorrections().otl_from_sif);
    EXPECT_FALSE(cfg.getModelCorrections().otl_nodal_corrections);
    EXPECT_FALSE(cfg.enableStationInfo());
    EXPECT_EQ(cfg.getStationInfo(), nullptr);
}

TEST(OtlSifSourceTest, ExplicitSifPathAloneStaysUnloaded) {
    // The registry is read only when station info or otl_from_sif asks for it, so a
    // config that merely names a SIF is unchanged.
    LogManager lm;
    GnssConfig cfg;
    const fs::path sif = writeSif();
    ASSERT_TRUE(loadConfig(cfg, lm, "otl_path_only",
                           { { "files", { { "station_info_path", fwd(sif) } } } }));
    EXPECT_EQ(cfg.getStationInfo(), nullptr);
}

TEST(OtlSifSourceTest, OtlFromSifLoadsRegistryWithoutStationInfo) {
    LogManager lm;
    GnssConfig cfg;
    const fs::path sif = writeSif();
    ASSERT_TRUE(loadConfig(cfg, lm, "otl_from_sif_on",
                           { { "files", { { "station_info_path", fwd(sif) } } },
                             { "model_corrections", { { "enable_ocean_tide_loading", true },
                                                      { "otl_from_sif", true },
                                                      { "otl_nodal_corrections", true } } } }));
    EXPECT_TRUE(cfg.getModelCorrections().otl_from_sif);
    EXPECT_TRUE(cfg.getModelCorrections().otl_nodal_corrections);
    EXPECT_FALSE(cfg.enableStationInfo());          // the a-priori and ocean pole tide paths stay off
    ASSERT_NE(cfg.getStationInfo(), nullptr);
    const StationInfo* si = cfg.getStationInfo()->find("SYNT00XXX");
    ASSERT_NE(si, nullptr);
    EXPECT_TRUE(si->has_otl);
}

TEST(OtlSifSourceTest, OtlFromSifWithOceanTideLoadingOffLoadsNothing) {
    LogManager lm;
    GnssConfig cfg;
    const fs::path sif = writeSif();
    ASSERT_TRUE(loadConfig(cfg, lm, "otl_from_sif_master_off",
                           { { "files", { { "station_info_path", fwd(sif) } } },
                             { "model_corrections", { { "enable_ocean_tide_loading", false },
                                                      { "otl_from_sif", true } } } }));
    EXPECT_EQ(cfg.getStationInfo(), nullptr);
}

// ---------------------------------------------------------------------------
// 2. The SIF mapping
// ---------------------------------------------------------------------------

TEST(OtlSifSourceTest, BlqRowsMappingCopiesColumnsUnchanged) {
    const StationInfo s = syntheticStation();
    const OtlStationCoefficients c = otlCoefficientsFromSif(s, SifOtlHorizontal::BlqRows);
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.station_name, "SYNT00XXX");
    for (std::size_t k = 0; k < 11; ++k) {
        EXPECT_DOUBLE_EQ(c.constituents[k].amp_up,      s.otl[k].up_amp);
        EXPECT_DOUBLE_EQ(c.constituents[k].phase_up,    s.otl[k].up_phase);
        EXPECT_DOUBLE_EQ(c.constituents[k].amp_west,    s.otl[k].east_amp);
        EXPECT_DOUBLE_EQ(c.constituents[k].phase_west,  s.otl[k].east_phase);
        EXPECT_DOUBLE_EQ(c.constituents[k].amp_south,   s.otl[k].north_amp);
        EXPECT_DOUBLE_EQ(c.constituents[k].phase_south, s.otl[k].north_phase);
    }
}

TEST(OtlSifSourceTest, LegacyMappingShiftsHorizontalPhasesByHalfACycle) {
    const StationInfo s = syntheticStation();
    const OtlStationCoefficients c = otlCoefficientsFromSif(s, SifOtlHorizontal::LegacyShifted);
    for (std::size_t k = 0; k < 11; ++k) {
        EXPECT_DOUBLE_EQ(c.constituents[k].phase_up,    s.otl[k].up_phase);
        EXPECT_DOUBLE_EQ(c.constituents[k].phase_west,  s.otl[k].east_phase  + 180.0);
        EXPECT_DOUBLE_EQ(c.constituents[k].phase_south, s.otl[k].north_phase + 180.0);
    }
}

TEST(OtlSifSourceTest, LegacyMappingReversesTheHorizontalVectorOnly) {
    LogManager lm;
    const StationInfo s = syntheticStation();
    OceanTideModel blq(lm), legacy(lm);
    blq.setStationCoefficients(otlCoefficientsFromSif(s, SifOtlHorizontal::BlqRows));
    legacy.setStationCoefficients(otlCoefficientsFromSif(s, SifOtlHorizontal::LegacyShifted));
    for (int i = 0; i < 12; ++i) {
        const GnssTime t = utc(2024, 5, 8, 2 * i, 17, 30.0);
        const Eigen::Vector3d a = enuAt(blq, t);
        const Eigen::Vector3d b = enuAt(legacy, t);
        EXPECT_NEAR(a.x(), -b.x(), 1e-9);
        EXPECT_NEAR(a.y(), -b.y(), 1e-9);
        EXPECT_NEAR(a.z(),  b.z(), 1e-9);
    }
}

// ---------------------------------------------------------------------------
// 3. The nodal factors
// ---------------------------------------------------------------------------

TEST(OtlSifSourceTest, NodalFactorsMatchTheQuotedClosedForms) {
    // Schureman closed forms, f and u in degrees, as tabulated by Pugh and Woodworth.
    // The side-line series is the same quantity from the HARDISP development, so they
    // agree to the accuracy of the quoted coefficients.
    for (int deg = 0; deg < 360; deg += 15) {
        const double N = deg * kD2R;
        const OtlNodalFactor m2 = OceanTideModel::nodalFactor(0, N);
        EXPECT_NEAR(m2.f, 1.0004 - 0.0373 * std::cos(N) + 0.0002 * std::cos(2 * N), 5e-4);
        EXPECT_NEAR(m2.u_rad / kD2R, -2.14 * std::sin(N), 0.1);

        const OtlNodalFactor k1 = OceanTideModel::nodalFactor(4, N);
        EXPECT_NEAR(k1.f, 1.0060 + 0.1150 * std::cos(N) - 0.0088 * std::cos(2 * N)
                          + 0.0006 * std::cos(3 * N), 5e-4);
        EXPECT_NEAR(k1.u_rad / kD2R, -8.86 * std::sin(N) + 0.68 * std::sin(2 * N)
                                     - 0.07 * std::sin(3 * N), 0.1);

        const OtlNodalFactor o1 = OceanTideModel::nodalFactor(5, N);
        EXPECT_NEAR(o1.f, 1.0089 + 0.1871 * std::cos(N) - 0.0147 * std::cos(2 * N)
                          + 0.0014 * std::cos(3 * N), 5e-4);
        EXPECT_NEAR(o1.u_rad / kD2R, 10.80 * std::sin(N) - 1.34 * std::sin(2 * N)
                                     + 0.19 * std::sin(3 * N), 0.1);

        const OtlNodalFactor mf = OceanTideModel::nodalFactor(8, N);
        EXPECT_NEAR(mf.f, 1.0429 + 0.4135 * std::cos(N) - 0.0040 * std::cos(2 * N), 5e-4);
        EXPECT_NEAR(mf.u_rad / kD2R, -23.74 * std::sin(N) + 2.68 * std::sin(2 * N)
                                     - 0.38 * std::sin(3 * N), 0.2);
    }
}

TEST(OtlSifSourceTest, NodalFactorsAreNearlyIdentityForSolarConstituents) {
    for (int deg = 0; deg < 360; deg += 30) {
        const double N = deg * kD2R;
        for (std::size_t j : { std::size_t(1) /*S2*/, std::size_t(6) /*P1*/ }) {
            const OtlNodalFactor f = OceanTideModel::nodalFactor(j, N);
            EXPECT_NEAR(f.f, 1.0, 0.013);
            EXPECT_NEAR(f.u_rad, 0.0, 0.012);
        }
    }
}

TEST(OtlSifSourceTest, NodalFactorOutsideTheTableIsTheIdentity) {
    const OtlNodalFactor f = OceanTideModel::nodalFactor(11, 1.0);
    EXPECT_DOUBLE_EQ(f.f, 1.0);
    EXPECT_DOUBLE_EQ(f.u_rad, 0.0);
}

// ---------------------------------------------------------------------------
// 4. The evaluation
// ---------------------------------------------------------------------------

TEST(OtlSifSourceTest, OffEqualsTheOriginalModelAndIsTheDefault) {
    LogManager lm;
    OceanTideModel model(lm);
    EXPECT_FALSE(model.nodalCorrections());
    OtlStationCoefficients c = otlCoefficientsFromSif(syntheticStation(), SifOtlHorizontal::BlqRows);
    // Only M2, K1, O1 and Mf are re-implemented; zero the other constituents so the two
    // sums are over the same terms.
    for (std::size_t j = 0; j < 11; ++j) {
        if (j == 0 || j == 4 || j == 5 || j == 8) continue;
        c.constituents[j] = OtlConstituentCoeffs{};
    }
    model.setStationCoefficients(c);
    for (int i = 0; i < 24; ++i) {
        const GnssTime t = utc(2024, 5, 8, i, 5, 0.0);
        const Eigen::Vector3d got = model.computeDisplacement(kSite, t);
        const Eigen::Vector3d ref = referenceOriginal(c, t);
        EXPECT_LT((got - ref).norm(), 1e-9) << "hour " << i;
        EXPECT_GT(got.norm(), 1e-3);                 // a real signal, not two zeros
    }
}

TEST(OtlSifSourceTest, LongPeriodConstituentDiffersByItsNodalFactorOnly) {
    // Mf has no Greenwich dependence (n1 = 0), so the only difference between the two
    // evaluations is f and u.
    LogManager lm;
    const OtlStationCoefficients c = singleConstituent(8);
    OceanTideModel off(lm), on(lm);
    off.setStationCoefficients(c);
    on.setStationCoefficients(c);
    on.setNodalCorrections(true);
    EXPECT_TRUE(on.nodalCorrections());

    const GnssTime t0 = utc(2024, 5, 8, 0, 0, 0.0);
    const double T = TimeUtils::gpsTimeToTtCenturies(t0);
    const double as2r = kD2R / 3600.0;
    const double Om = std::fmod(450160.398036 - 6962890.5431 * T + 7.4722 * T * T
                                + 0.007702 * T * T * T - 0.00005939 * T * T * T * T, 1296000.0) * as2r;
    const OtlNodalFactor nf = OceanTideModel::nodalFactor(8, Om);

    // The phase correction only moves the series in time, so compare the extremes of
    // the radial series over one Mf period (13.66 days): their ratio is f.
    double max_off = 0.0, max_on = 0.0;
    for (int step = 0; step < 14 * 4; ++step) {                       // every 6 h for 14 days
        const GnssTime ti = advanced(t0, step * 6 * 3600.0);
        max_off = std::max(max_off, std::fabs(enuAt(off, ti).z()));
        max_on  = std::max(max_on,  std::fabs(enuAt(on, ti).z()));
    }
    EXPECT_NEAR(max_on / max_off, nf.f, 0.01);
    EXPECT_GT(std::fabs(nf.f - 1.0), 0.1);          // 2024 is near the node maximum: a real test
}

TEST(OtlSifSourceTest, GreenwichArgumentFromUtcShiftsSemidiurnalByTheLeapSeconds) {
    // S2 carries n1 = 2 and (almost) no nodal modulation.  Taking the Greenwich angle
    // from UTC instead of GPS time moves the argument by 2 * 360.98564736629 deg/day *
    // 18 s = 0.1504 degrees, so the two series differ by about A * 0.0026.
    LogManager lm;
    const double A = 0.010;
    const OtlStationCoefficients c = singleConstituent(1, A);
    OceanTideModel off(lm), on(lm);
    off.setStationCoefficients(c);
    on.setStationCoefficients(c);
    on.setNodalCorrections(true);
    double max_diff = 0.0;
    const GnssTime t0 = utc(2024, 5, 8, 0, 0, 0.0);
    for (int i = 0; i < 48; ++i) {
        const GnssTime ti = advanced(t0, i * 1800.0);
        max_diff = std::max(max_diff, std::fabs(enuAt(on, ti).z() - enuAt(off, ti).z()));
    }
    const double shift = 2.0 * 360.98564736629 * 18.0 / 86400.0 * kD2R;      // rad
    EXPECT_NEAR(max_diff, A * shift, A * 0.0035);   // the 0.2 % S2 side line adds at most that
    EXPECT_GT(max_diff, A * shift * 0.5);
}

// ---------------------------------------------------------------------------
// 5. The HARDISP regression
// ---------------------------------------------------------------------------

namespace {

// KOKB00USA (Kauai, Hawaii) from igs20_SIF.json: up amplitude [m], up phase [deg], east
// column amplitude and phase, north column amplitude and phase, in the BLQ order
// M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa.  A Pacific island with a diurnal-dominated load
// (K1 16.6 mm radial) and a 45 mm peak radial day: the case where the nodal factors and
// the missing minor diurnals matter most among the stations of the field golden.
constexpr double kKokb[11][6] = {
    { 0.013750, -117.20, 0.001020,  131.90, 0.005360,   79.80 },  // M2
    { 0.004630, -112.60, 0.001000,  160.20, 0.002030,  103.10 },  // S2
    { 0.002610, -126.20, 0.000200,  173.50, 0.001010,   69.20 },  // N2
    { 0.001150, -122.80, 0.000360,  157.50, 0.000570,   96.40 },  // K2
    { 0.016590,   60.90, 0.001430,  -10.20, 0.002540,  -54.00 },  // K1
    { 0.009570,   53.90, 0.000810,  -23.60, 0.001890,  -61.50 },  // O1
    { 0.005150,   59.20, 0.000490,  -10.70, 0.000800,  -56.80 },  // P1
    { 0.001510,   48.40, 0.000120,  -22.30, 0.000360,  -55.40 },  // Q1
    { 0.000860, -112.70, 0.000050,  -82.00, 0.000680,   -6.40 },  // Mf
    { 0.000570, -145.50, 0.000070, -137.60, 0.000310,  -13.20 },  // Mm
    { 0.000640, -176.30, 0.000050,  172.90, 0.000190,   -2.60 },  // Ssa
};
const Eigen::Vector3d kKokbEcef(-5543838.334, -2054585.810, 2387810.441);

// IERS HARDISP as shipped in NGS M-PAGES v1.2 (hardisp_port.py, validated against the two
// test cases printed in HARDISP.F), fed the SIF columns as M-PAGES feeds them, at
// 2024-05-08 hh:00:00 UTC.  East, north, up in metres.
struct HardispRef { int hour; double e, n, u; };
constexpr HardispRef kHardisp[] = {
    {  0, +0.003697, +0.000681, -0.024947 },
    {  3, -0.000076, -0.008962, -0.044419 },
    {  6, -0.001869, -0.002864, -0.016430 },
    {  9, -0.000518, +0.003661, +0.008550 },
    { 12, -0.000675, -0.002510, +0.004627 },
    { 15, -0.002563, -0.006722, +0.007680 },
    { 18, -0.001247, +0.003929, +0.030680 },
    { 21, +0.003077, +0.013420, +0.028470 },
};

StationInfo kokbStation() {
    StationInfo s;
    s.id = "KOKB00USA";
    s.has_otl = true;
    for (std::size_t k = 0; k < 11; ++k) {
        s.otl[k].up_amp = kKokb[k][0];     s.otl[k].up_phase = kKokb[k][1];
        s.otl[k].east_amp = kKokb[k][2];   s.otl[k].east_phase = kKokb[k][3];
        s.otl[k].north_amp = kKokb[k][4];  s.otl[k].north_phase = kKokb[k][5];
    }
    return s;
}

/// Local ENU displacement of @p model at the KOKB site.
Eigen::Vector3d enuAtKokb(const OceanTideModel& model, const GnssTime& t) {
    const Eigen::Vector3d d = model.computeDisplacement(kKokbEcef, t);
    return Coordinates::ecefToEnu(d, Coordinates::ecefToLlh(kKokbEcef));
}

} // namespace

TEST(OtlSifSourceTest, AgreesWithTheHardispReferenceAtAnOceanSite) {
    // The bounds are the day maxima of the gate (scripts/otl_compare/otl_gate.py) with
    // margin: up 4.9 mm without and 2.5 mm with the nodal corrections, horizontals
    // 1.1 mm and 0.7 mm.  The remainder is the minor diurnal constituents that the
    // 11-line model does not carry (2.4 mm peak at this site).
    LogManager lm;
    OceanTideModel plain(lm), nodal(lm), legacy(lm);
    plain.setStationCoefficients(otlCoefficientsFromSif(kokbStation(), SifOtlHorizontal::BlqRows));
    nodal.setStationCoefficients(otlCoefficientsFromSif(kokbStation(), SifOtlHorizontal::BlqRows));
    nodal.setNodalCorrections(true);
    legacy.setStationCoefficients(otlCoefficientsFromSif(kokbStation(), SifOtlHorizontal::LegacyShifted));
    for (const HardispRef& r : kHardisp) {
        const GnssTime t = utc(2024, 5, 8, r.hour, 0, 0.0);
        const Eigen::Vector3d p = enuAtKokb(plain, t);
        const Eigen::Vector3d n = enuAtKokb(nodal, t);
        EXPECT_NEAR(p.x(), r.e, 1.6e-3) << "hour " << r.hour;
        EXPECT_NEAR(p.y(), r.n, 2.0e-3) << "hour " << r.hour;
        EXPECT_NEAR(p.z(), r.u, 6.0e-3) << "hour " << r.hour;
        EXPECT_NEAR(n.x(), r.e, 0.8e-3) << "hour " << r.hour;
        EXPECT_NEAR(n.y(), r.n, 1.2e-3) << "hour " << r.hour;
        EXPECT_NEAR(n.z(), r.u, 3.2e-3) << "hour " << r.hour;
    }
    // The station-info reading is the same radial series with the horizontal vector
    // reversed: at the epoch of the largest north component it is 2 * 13 mm away.
    const GnssTime t21 = utc(2024, 5, 8, 21, 0, 0.0);
    EXPECT_GT(std::fabs(enuAtKokb(legacy, t21).y() - 0.013420), 0.020);
}

} // namespace GnssProcessor
