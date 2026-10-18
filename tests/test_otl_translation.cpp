/**
 * @file test_otl_translation.cpp
 * @brief The horizontal convention of the SIF ocean tide loading block pinned against the
 *        IERS HARDISP reference at Wettzell, and the removal of the net translation of the
 *        diurnal loading field (`model_corrections.otl_remove_diurnal_translation`).
 *
 * Covered:
 *
 *   1. the loader: the key defaults off and loads when set;
 *   2. the convention at the held-out consumer WTZZ00DEU: with the SIF east and north columns
 *      read as the BLQ west and south rows the production series agrees with the HARDISP
 *      reference (NGS M-PAGES v1.2, evaluated by scripts/otl_compare/hardisp_port.py) in
 *      east, north and up at hourly epochs of 2024-129, and the reading of
 *      `processing.enable_station_info` returns the north component with the opposite sign;
 *   3. the removal: it changes the K1, O1, P1 and Q1 rows only, it subtracts one uniform ECEF
 *      displacement (the same vector at stations in different places), it reproduces the
 *      table values at Wettzell (numpy, scripts/otl_compare/net_translation.py), and it
 *      leaves invalid input alone;
 *   4. the effect at Wettzell: the north component of the loading over the hours 2 to 6 of
 *      the field day, with and without the translation.
 *
 * Suite: OtlTranslationTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include "common/StationInfo.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/OceanTideModel.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
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

/// WTZZ00DEU: the truth position of the held-out consumer (ITRF2020 at 2024-129), ECEF [m].
const Eigen::Vector3d kWettzell(4075579.122, 931853.4443, 4801569.233);
/// KOKB00USA (Kokee Park, Hawaii), ECEF [m].
const Eigen::Vector3d kKokee(-5543838.334, -2054585.81, 2387810.441);

/// One row of the SIF block: the three columns UP, EAST, NORTH (amplitude [m], phase [deg]).
struct SifRow { double up_a, up_p, e_a, e_p, n_a, n_p; };

/// WTZZ00DEU, igs20_SIF.json (FES2014b, CARGA at SEGAL), constituents M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa.
constexpr std::array<SifRow, 11> kWtzz = {{
    { 0.00554,  -89.7, 0.00219,  120.6, 0.00152,   78.3 },  // M2
    { 0.00183,  -68.9, 0.00030, -173.9, 0.00029,  133.4 },  // S2
    { 0.00127, -105.7, 0.00053,   88.3, 0.00034,   49.5 },  // N2
    { 0.00052,  -72.3, 0.00004,  -95.7, 0.00006,  179.9 },  // K2
    { 0.00197,  153.1, 0.00210,   77.4, 0.00428, -102.4 },  // K1
    { 0.00188,  139.5, 0.00132,   39.8, 0.00280, -109.8 },  // O1
    { 0.00061,  148.9, 0.00064,   78.2, 0.00134, -105.4 },  // P1
    { 0.00047,  127.2, 0.00025,    7.5, 0.00047, -117.4 },  // Q1
    { 0.00029,  101.5, 0.00003, -167.8, 0.00037,   73.6 },  // Mf
    { 0.00019,   59.0, 0.00003,   53.3, 0.00024,   35.7 },  // Mm
    { 0.00022,    5.8, 0.00001,  -91.4, 0.00028,    4.0 },  // Ssa
}};

StationInfo wettzellStation() {
    StationInfo s;
    s.id = "WTZZ00DEU";
    s.has_otl = true;
    for (std::size_t k = 0; k < 11; ++k) {
        s.otl[k].up_amp = kWtzz[k].up_a;     s.otl[k].up_phase = kWtzz[k].up_p;
        s.otl[k].east_amp = kWtzz[k].e_a;    s.otl[k].east_phase = kWtzz[k].e_p;
        s.otl[k].north_amp = kWtzz[k].n_a;   s.otl[k].north_phase = kWtzz[k].n_p;
    }
    return s;
}

/// A coefficient set with a distinct value in every row and column.
OtlStationCoefficients busyCoefficients(double scale) {
    OtlStationCoefficients c;
    c.station_name = "BUSY";
    c.valid = true;
    for (std::size_t k = 0; k < 11; ++k) {
        OtlConstituentCoeffs& d = c.constituents[k];
        d.amp_up    = scale * (0.0100 + 0.0010 * k);  d.phase_up    = -100.0 + 7.0 * k;
        d.amp_west  = scale * (0.0020 + 0.0001 * k);  d.phase_west  =   30.0 + 11.0 * k;
        d.amp_south = scale * (0.0030 + 0.0002 * k);  d.phase_south = -120.0 + 13.0 * k;
    }
    return c;
}

GnssTime utc(int y, int mo, int d, int h, int mi, double s) {
    return TimeUtils::gregorianToGpsTime(y, mo, d, h, mi, s);
}

Eigen::Vector3d enuAt(const OceanTideModel& model, const Eigen::Vector3d& site, const GnssTime& t) {
    return Coordinates::ecefToEnu(model.computeDisplacement(site, t), Coordinates::ecefToLlh(site));
}

/// HARDISP (M-PAGES v1.2, SIF columns as up, west and south rows) at WTZZ00DEU, 2024-129 UTC:
/// local east, north, up [m].
struct HardispRef { int hour; double e, n, u; };
constexpr HardispRef kHardisp[] = {
    {  0, +0.002545, -0.002949, +0.002821 }, {  1, +0.001766, -0.005551, -0.001990 },
    {  2, +0.000857, -0.007619, -0.005842 }, {  3, +0.000125, -0.008895, -0.007934 },
    {  4, -0.000191, -0.009260, -0.007920 }, {  5, +0.000019, -0.008749, -0.005994 },
    {  6, +0.000699, -0.007533, -0.002820 }, {  9, +0.003109, -0.002323, +0.004659 },
    { 12, +0.000814, +0.001292, -0.001506 }, { 15, -0.004718, +0.003933, -0.008288 },
    { 18, -0.005097, +0.006995, +0.000839 }, { 21, +0.000399, +0.006329, +0.012564 },
};

/// Load a config body (a minimal PPP post-processing config plus @p extra members).
bool loadConfig(GnssConfig& cfg, LogManager& lm, const std::string& tag, const json& extra) {
    json doc = { { "processing", { { "mode", "ppp" } } } };
    for (auto it = extra.begin(); it != extra.end(); ++it) doc[it.key()] = it.value();
    const fs::path p = gnss_test::tempDir() / (tag + ".json");
    { std::ofstream f(p); f << doc.dump(2); }
    return cfg.loadFromJson(p, lm);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The loader
// ---------------------------------------------------------------------------

TEST(OtlTranslationTest, KeyDefaultsOffAndLoadsWhenSet) {
    LogManager lm;
    GnssConfig off;
    ASSERT_TRUE(loadConfig(off, lm, "otl_tr_off", json::object()));
    EXPECT_FALSE(off.getModelCorrections().otl_remove_diurnal_translation);

    GnssConfig on;
    ASSERT_TRUE(loadConfig(on, lm, "otl_tr_on",
                           { { "model_corrections", { { "otl_remove_diurnal_translation", true } } } }));
    EXPECT_TRUE(on.getModelCorrections().otl_remove_diurnal_translation);
}

// ---------------------------------------------------------------------------
// 2. The horizontal convention at the held-out consumer
// ---------------------------------------------------------------------------

TEST(OtlTranslationTest, SifColumnsAsBlqRowsAgreeWithHardispAtWettzell) {
    // Bounds are the day maxima of the gate (E 0.42, N 1.30, U 0.82 mm without the nodal
    // corrections; 0.31, 0.82, 0.46 mm with them) with margin.  The horizontal signal is
    // 9 mm, so a reversed vector would miss by 18 mm.
    LogManager lm;
    OceanTideModel plain(lm), nodal(lm);
    plain.setStationCoefficients(otlCoefficientsFromSif(wettzellStation(), SifOtlHorizontal::BlqRows));
    nodal.setStationCoefficients(otlCoefficientsFromSif(wettzellStation(), SifOtlHorizontal::BlqRows));
    nodal.setNodalCorrections(true);
    for (const HardispRef& r : kHardisp) {
        const GnssTime t = utc(2024, 5, 8, r.hour, 0, 0.0);
        const Eigen::Vector3d p = enuAt(plain, kWettzell, t);
        const Eigen::Vector3d n = enuAt(nodal, kWettzell, t);
        EXPECT_NEAR(p.x(), r.e, 0.8e-3) << "hour " << r.hour;
        EXPECT_NEAR(p.y(), r.n, 1.5e-3) << "hour " << r.hour;
        EXPECT_NEAR(p.z(), r.u, 1.0e-3) << "hour " << r.hour;
        EXPECT_NEAR(n.x(), r.e, 0.5e-3) << "hour " << r.hour;
        EXPECT_NEAR(n.y(), r.n, 1.0e-3) << "hour " << r.hour;
        EXPECT_NEAR(n.z(), r.u, 0.7e-3) << "hour " << r.hour;
    }
}

TEST(OtlTranslationTest, StationInfoReadingReversesTheNorthComponentAtWettzell) {
    LogManager lm;
    OceanTideModel blq(lm), legacy(lm);
    blq.setStationCoefficients(otlCoefficientsFromSif(wettzellStation(), SifOtlHorizontal::BlqRows));
    legacy.setStationCoefficients(otlCoefficientsFromSif(wettzellStation(), SifOtlHorizontal::LegacyShifted));
    for (int hour : { 3, 4 }) {
        const GnssTime t = utc(2024, 5, 8, hour, 0, 0.0);
        const Eigen::Vector3d a = enuAt(blq, kWettzell, t);
        const Eigen::Vector3d b = enuAt(legacy, kWettzell, t);
        EXPECT_LT(a.y(), -0.007) << "hour " << hour;     // south: HARDISP reads -8.9 and -9.3 mm
        EXPECT_GT(b.y(), +0.007) << "hour " << hour;     // the station-info reading says north
        EXPECT_NEAR(a.y(), -b.y(), 1e-9) << "hour " << hour;
        EXPECT_NEAR(a.z(), b.z(), 1e-9) << "hour " << hour;
    }
}

// ---------------------------------------------------------------------------
// 3. The removal of the net translation
// ---------------------------------------------------------------------------

TEST(OtlTranslationTest, ChangesTheDiurnalRowsOnly) {
    const OtlStationCoefficients before = busyCoefficients(1.0);
    OtlStationCoefficients after = before;
    removeOtlDiurnalTranslation(after, kWettzell);
    for (std::size_t k = 0; k < 11; ++k) {
        const bool diurnal = (k >= 4 && k <= 7);
        const auto& a = before.constituents[k];
        const auto& b = after.constituents[k];
        const bool same = a.amp_up == b.amp_up && a.phase_up == b.phase_up
                       && a.amp_west == b.amp_west && a.phase_west == b.phase_west
                       && a.amp_south == b.amp_south && a.phase_south == b.phase_south;
        EXPECT_EQ(same, !diurnal) << "constituent " << OTL_CONSTITUENT_NAMES[k];
    }
}

TEST(OtlTranslationTest, SubtractsOneUniformEcefDisplacementAtEveryStation) {
    // The part removed is the same ECEF vector at stations in different places.
    LogManager lm;
    const std::array<Eigen::Vector3d, 2> sites = { kWettzell, kKokee };
    const GnssTime t0 = utc(2024, 5, 8, 0, 0, 0.0);
    for (int step = 0; step < 6; ++step) {
        GnssTime t = t0;
        t.tow += 3600.0 * 3.7 * step;
        std::array<Eigen::Vector3d, 2> removed;
        for (std::size_t i = 0; i < 2; ++i) {
            const OtlStationCoefficients before = busyCoefficients(1.0 + 0.5 * static_cast<double>(i));
            OtlStationCoefficients after = before;
            removeOtlDiurnalTranslation(after, sites[i]);
            OceanTideModel mb(lm), ma(lm);
            mb.setStationCoefficients(before);
            ma.setStationCoefficients(after);
            removed[i] = mb.computeDisplacement(sites[i], t) - ma.computeDisplacement(sites[i], t);
        }
        EXPECT_NEAR((removed[0] - removed[1]).norm(), 0.0, 1e-9) << "epoch " << step;
        EXPECT_LT(removed[0].norm(), 0.0125) << "epoch " << step;   // sum of the four vector magnitudes
    }
    // And it is not nothing: over a day the removed vector reaches several millimetres.
    OtlStationCoefficients before = busyCoefficients(1.0), after = before;
    removeOtlDiurnalTranslation(after, kWettzell);
    OceanTideModel mb(lm), ma(lm);
    mb.setStationCoefficients(before);
    ma.setStationCoefficients(after);
    double peak = 0.0;
    for (int m = 0; m < 24 * 12; ++m) {
        GnssTime t = t0;
        t.tow += 300.0 * m;
        peak = std::max(peak, (mb.computeDisplacement(kWettzell, t) - ma.computeDisplacement(kWettzell, t)).norm());
    }
    EXPECT_GT(peak, 0.004);
}

TEST(OtlTranslationTest, ReproducesTheTableValuesAtWettzell) {
    // A station with no loading: after the removal the rows hold the negated up component and
    // the west and south components (east = -west, north = -south) of the table vectors.
    // Reference: numpy, scripts/otl_compare/net_translation.py (area-weighted fit, 597 stations
    // of igs20_SIF.json), projected at the Wettzell position.
    struct Ref { std::size_t k; double ua, up, wa, wp, sa, sp; };
    constexpr Ref refs[] = {
        { 4, 0.0034980,  -50.177, 0.0022226, -111.616, 0.0041578, 71.705 },   // K1
        { 5, 0.0023981,  -63.721, 0.0014209, -138.713, 0.0025520, 65.873 },   // O1
        { 6, 0.0010894,  -53.190, 0.0006726, -110.347, 0.0013011, 68.782 },   // P1
        { 7, 0.0004377,  -64.230, 0.0002682, -162.053, 0.0004186, 62.229 },   // Q1
    };
    OtlStationCoefficients c;
    c.valid = true;
    removeOtlDiurnalTranslation(c, kWettzell);
    auto wrapped = [](double d) { return std::remainder(d, 360.0); };
    for (const Ref& r : refs) {
        const auto& d = c.constituents[r.k];
        const char* name = OTL_CONSTITUENT_NAMES[r.k];
        EXPECT_NEAR(d.amp_up, r.ua, 2e-7) << name;
        EXPECT_NEAR(wrapped(d.phase_up - r.up), 0.0, 0.02) << name;
        EXPECT_NEAR(d.amp_west, r.wa, 2e-7) << name;
        EXPECT_NEAR(wrapped(d.phase_west - r.wp), 0.0, 0.02) << name;
        EXPECT_NEAR(d.amp_south, r.sa, 2e-7) << name;
        EXPECT_NEAR(wrapped(d.phase_south - r.sp), 0.0, 0.02) << name;
    }
    for (std::size_t k : { std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3),
                           std::size_t(8), std::size_t(9), std::size_t(10) })
        EXPECT_EQ(c.constituents[k].amp_up, 0.0) << OTL_CONSTITUENT_NAMES[k];
}

TEST(OtlTranslationTest, LeavesInvalidInputAlone) {
    const OtlStationCoefficients ref = busyCoefficients(1.0);
    OtlStationCoefficients invalid = ref;
    invalid.valid = false;
    removeOtlDiurnalTranslation(invalid, kWettzell);
    EXPECT_EQ(invalid.constituents[4].amp_up, ref.constituents[4].amp_up);

    OtlStationCoefficients a = ref, b = ref;
    removeOtlDiurnalTranslation(a, Eigen::Vector3d(0.0, 0.0, 0.0));
    removeOtlDiurnalTranslation(b, Eigen::Vector3d(std::nan(""), 1.0, 1.0));
    for (std::size_t k = 0; k < 11; ++k) {
        EXPECT_EQ(a.constituents[k].amp_up, ref.constituents[k].amp_up);
        EXPECT_EQ(a.constituents[k].phase_south, ref.constituents[k].phase_south);
        EXPECT_EQ(b.constituents[k].amp_west, ref.constituents[k].amp_west);
    }
}

// ---------------------------------------------------------------------------
// 4. The effect at Wettzell
// ---------------------------------------------------------------------------

TEST(OtlTranslationTest, TheTranslationIsMostOfTheNorthLoadingAtWettzell) {
    // Mean over the hours 1 to 6 UTC of 2024-129 (the settled window of the field arms,
    // TOW 262800 to 280800), sampled every 5 minutes, with the nodal corrections as the
    // field arms run them: the north component of the loading is -8.0 mm; the diurnal
    // translation carries -5.7 mm of it.  Reference: gnss_otl_diag series with and without
    // --drop-translation, checked against HARDISP fed the same coefficients.
    LogManager lm;
    OceanTideModel full(lm), stripped(lm);
    full.setNodalCorrections(true);
    stripped.setNodalCorrections(true);
    OtlStationCoefficients c = otlCoefficientsFromSif(wettzellStation(), SifOtlHorizontal::BlqRows);
    full.setStationCoefficients(c);
    removeOtlDiurnalTranslation(c, kWettzell);
    stripped.setStationCoefficients(c);
    Eigen::Vector3d mf = Eigen::Vector3d::Zero(), mr = Eigen::Vector3d::Zero();
    int n = 0;
    for (double s = 3600.0; s < 6.0 * 3600.0; s += 300.0, ++n) {
        const GnssTime t = utc(2024, 5, 8, 0, 0, 0.0);
        GnssTime tt = t;
        tt.tow += s;
        mf += enuAt(full, kWettzell, tt);
        mr += enuAt(stripped, kWettzell, tt);
    }
    mf /= n;
    mr /= n;
    EXPECT_NEAR(mf.y(), -0.00795, 0.0003);     // the full loading
    EXPECT_NEAR(mr.y(), -0.00230, 0.0003);     // the translation removed
    EXPECT_NEAR(mf.x(), +0.00023, 0.0003);
    EXPECT_NEAR(mr.x(), -0.00244, 0.0003);
    EXPECT_NEAR(mf.z(), -0.00596, 0.0004);
    EXPECT_NEAR(mr.z(), -0.00428, 0.0004);
}

} // namespace GnssProcessor
