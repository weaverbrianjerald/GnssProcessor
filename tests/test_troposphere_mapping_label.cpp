/**
 * @file test_troposphere_mapping_label.cpp
 * @brief Unit tests for the troposphere mapping-function label and for the
 *        station a priori hydrostatic delay under the GPT3 pressure source.
 *
 * Two behaviours are fixed here:
 *
 *  1. The mapping function selected by @c receiver.troposphere_mapping_function
 *     = "nmf" (the default) is the Niell (1996) NMF.  It was configured and
 *     logged as "gmf"; "gmf" stays accepted as the legacy spelling of the same
 *     path, so the two spellings give identical mappings.
 *  2. With @c receiver.troposphere_apriori_source = "gpt3" the Saastamoinen
 *     hydrostatic delay takes the GPT3 pressure at the receiver's own position
 *     and day of year: the four surrounding cells' harmonics, each reduced
 *     from the cell height to the station with the cell's virtual temperature
 *     (the GPT2 / GPT3 reference form), interpolated bilinearly.  The expected
 *     values are literals computed outside the code under test, from a
 *     synthetic grid whose cells all differ, and (when the shipped grid is
 *     present) from four rows of @c gpt3_5.grd copied verbatim.
 */

#include <gtest/gtest.h>

#include "atmosphere/Troposphere.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double DEG2RAD = Constants::PI / 180.0;

// 2024 day 129 03:00:00 UTC (GPS = UTC + 18 s): fractional day of year 129.125.
const GnssTime kDoy129p125{ 2313, 259200.0 + 10800.0 + 18.0 };
// 2024 day 300 12:00:00 UTC: fractional day of year 300.5.
const GnssTime kDoy300p5{ 2337, 561618.0 };

/// Harmonic coefficients (a0, A1, B1, A2, B2) and heights of one synthetic grid cell.
/// Every cell differs, so a wrong corner, weight or height shows in the result.
struct SynthCell {
    double p[5], T[5], Q[5], undu, Hs;
};

SynthCell synthCell(int i_lat, int i_lon) {
    SynthCell c{};
    const double il = i_lat, io = i_lon;
    const double p[5] = { 95000.0 + 40.0 * il + 25.0 * io, 300.0 + io, -150.0 + 2.0 * il, 80.0, -40.0 };
    const double T[5] = { 250.0 + 0.5 * il + 0.25 * io, 5.0, -3.0, 1.0, 0.5 };
    const double Q[5] = { 2.0 + 0.05 * io, 0.4, 0.2, 0.1, 0.0 };
    for (int k = 0; k < 5; ++k) { c.p[k] = p[k]; c.T[k] = T[k]; c.Q[k] = Q[k]; }
    c.undu = 10.0 + il - 0.5 * io;
    c.Hs   = 20.0 * il + 30.0 * (i_lon % 5);
    return c;
}

/// Write the synthetic grid once per process in the published column order
/// (lat, lon, p x5, T x5, Q x5, dT x5, undu, Hs, a_h x5, a_w x5, rest zero).
fs::path synthGrid() {
    static fs::path written;
    if (!written.empty()) return written;
    written = gnss_test::tempDir() / "synthetic_gpt3.grd";
    std::string text = "%  lat lon p:a0 A1 B1 A2 B2 T:a0 ... (synthetic)\n";
    for (int r = 0; r < 36 * 72; ++r) {
        const int i_lat = r / 72, i_lon = r % 72;
        const SynthCell c = synthCell(i_lat, i_lon);
        double v[64] = {};
        v[0] = 87.5 - 5.0 * i_lat;
        v[1] = 2.5 + 5.0 * i_lon;
        for (int k = 0; k < 5; ++k) { v[2 + k] = c.p[k]; v[7 + k] = c.T[k]; v[12 + k] = c.Q[k]; }
        v[17] = -6.5;          // dT a0, K/km
        v[22] = c.undu;
        v[23] = c.Hs;
        v[24] = 1.2;           // a_h a0 as stored (x1000)
        v[29] = 0.55;          // a_w a0 as stored (x1000)
        for (int k = 0; k < 64; ++k) {
            char tok[40];
            std::snprintf(tok, sizeof(tok), "%.10g", v[k]);
            text += tok;
            text += (k + 1 < 64) ? ' ' : '\n';
        }
    }
    std::ofstream f(written, std::ios::binary);
    f << text;
    return written;
}

/// A configuration with the given receiver block, loaded from a temporary file.
std::unique_ptr<GnssConfig> loadConfig(const std::string& tag, const std::string& receiver_body,
                                       LogManager& log) {
    const fs::path path = gnss_test::tempDir() / ("cfg_" + tag + ".json");
    {
        std::ofstream f(path);
        f << "{ \"processing\": { \"mode\": \"ppp\" },\n  \"receiver\": { " << receiver_body << " } }\n";
    }
    auto cfg = std::make_unique<GnssConfig>();
    EXPECT_TRUE(cfg->loadFromJson(path, log));
    return cfg;
}

/// Mapping function selected by a configuration whose receiver block sets the
/// key to @p value (no key at all when @p value is empty).
TroposphereMappingFunction mappingFor(const std::string& tag, const std::string& value) {
    LogManager log;
    const std::string body = value.empty()
        ? std::string{}
        : "\"troposphere_mapping_function\": \"" + value + "\"";
    return loadConfig(tag, body, log)->getTroposphereMappingFunction();
}

/// Hydrostatic delay at (lat, lon, h) and time @p t with the GPT3 source on @p grid.
double gpt3Zhd(const fs::path& grid, double lat_deg, double lon_deg, double h_m, const GnssTime& t) {
    LogManager log;
    auto cfg = loadConfig("gpt3_zhd",
        "\"troposphere_apriori_source\": \"gpt3\", \"gpt3_grid_path\": \"" + grid.generic_string() + "\"",
        log);
    Troposphere tropo(*cfg, log);
    const Eigen::Vector3d rx = Coordinates::llhToEcef(lat_deg * DEG2RAD, lon_deg * DEG2RAD, h_m);
    return tropo.computeSaastamoinenZHD(rx, t);
}

// ---------------------------------------------------------------------------
// 1. The label and the spelling
// ---------------------------------------------------------------------------

TEST(TroposphereMappingLabel, SpellingTable) {
    const auto nmf = parseTroposphereMappingFunction("nmf");
    EXPECT_EQ(nmf.function, TroposphereMappingFunction::Nmf);
    EXPECT_TRUE(nmf.recognized);
    EXPECT_FALSE(nmf.legacy);

    const auto gmf = parseTroposphereMappingFunction("gmf");
    EXPECT_EQ(gmf.function, TroposphereMappingFunction::Nmf);
    EXPECT_TRUE(gmf.recognized);
    EXPECT_TRUE(gmf.legacy);

    const auto vmf3 = parseTroposphereMappingFunction("vmf3");
    EXPECT_EQ(vmf3.function, TroposphereMappingFunction::Vmf3);
    EXPECT_TRUE(vmf3.recognized);
    EXPECT_FALSE(vmf3.legacy);

    const auto bogus = parseTroposphereMappingFunction("gmf2");
    EXPECT_EQ(bogus.function, TroposphereMappingFunction::Nmf);
    EXPECT_FALSE(bogus.recognized);
    EXPECT_FALSE(bogus.legacy);
}

TEST(TroposphereMappingLabel, LabelsAreTheCanonicalSpellings) {
    EXPECT_STREQ(troposphereMappingFunctionLabel(TroposphereMappingFunction::Nmf), "nmf");
    EXPECT_STREQ(troposphereMappingFunctionLabel(TroposphereMappingFunction::Vmf3), "vmf3");
    EXPECT_STREQ(troposphereAprioriSourceLabel(TroposphereAprioriSource::StandardAtmosphere),
                 "standard_atmosphere");
    EXPECT_STREQ(troposphereAprioriSourceLabel(TroposphereAprioriSource::Gpt3), "gpt3");
    // Each label reads back as the function it names.
    for (auto f : { TroposphereMappingFunction::Nmf, TroposphereMappingFunction::Vmf3 }) {
        const auto s = parseTroposphereMappingFunction(troposphereMappingFunctionLabel(f));
        EXPECT_EQ(s.function, f);
        EXPECT_TRUE(s.recognized);
        EXPECT_FALSE(s.legacy);
    }
}

TEST(TroposphereMappingLabel, LoaderAcceptsBothSpellingsOfNiell) {
    EXPECT_EQ(mappingFor("absent", ""), TroposphereMappingFunction::Nmf);
    EXPECT_EQ(mappingFor("nmf", "nmf"), TroposphereMappingFunction::Nmf);
    EXPECT_EQ(mappingFor("nmf_upper", "NMF"), TroposphereMappingFunction::Nmf);
    EXPECT_EQ(mappingFor("gmf", "gmf"), TroposphereMappingFunction::Nmf);
    EXPECT_EQ(mappingFor("gmf_upper", "GMF"), TroposphereMappingFunction::Nmf);
    EXPECT_EQ(mappingFor("vmf3", "vmf3"), TroposphereMappingFunction::Vmf3);
    EXPECT_EQ(mappingFor("unknown", "boehm2006"), TroposphereMappingFunction::Nmf);
}

TEST(TroposphereMappingLabel, LegacySpellingGivesTheSameMappings) {
    LogManager log;
    auto cfg_nmf = loadConfig("map_nmf", "\"troposphere_mapping_function\": \"nmf\"", log);
    auto cfg_gmf = loadConfig("map_gmf", "\"troposphere_mapping_function\": \"gmf\"", log);
    auto cfg_def = loadConfig("map_default", "", log);
    Troposphere t_nmf(*cfg_nmf, log), t_gmf(*cfg_gmf, log), t_def(*cfg_def, log);
    const Eigen::Vector3d rx = Coordinates::llhToEcef(-66.28 * DEG2RAD, 110.52 * DEG2RAD, 22.5);
    for (double el : { 7.0, 15.0, 30.0, 60.0, 89.0 }) {
        double d1 = 0, w1 = 0, d2 = 0, w2 = 0, d3 = 0, w3 = 0;
        t_nmf.getTroposphereMapping(el, rx, kDoy129p125, d1, w1);
        t_gmf.getTroposphereMapping(el, rx, kDoy129p125, d2, w2);
        t_def.getTroposphereMapping(el, rx, kDoy129p125, d3, w3);
        EXPECT_EQ(d1, d2) << el;
        EXPECT_EQ(w1, w2) << el;
        EXPECT_EQ(d1, d3) << el;
        EXPECT_EQ(w1, w3) << el;
    }
}

// ---------------------------------------------------------------------------
// 2. The station's a priori hydrostatic delay under the GPT3 source
// ---------------------------------------------------------------------------

// Literal values from an independent evaluation of the published GPT3 query
// (four-corner harmonics, P0 exp(-g M_d dh / (R Tv)) per corner, bilinear
// weights) followed by Saastamoinen, 0.0022768 P / (1 - 0.00266 cos 2phi - 0.00028 h_km).

TEST(TroposphereMappingLabel, Gpt3HydrostaticDelayAtAKnownCoordinateAndDay) {
    // 41.3 N, 11.7 E, 1650 m: corners (42.5, 7.5), (42.5, 12.5), (37.5, 7.5), (37.5, 12.5),
    // weights 0.76/0.24 in latitude and 0.16/0.84 in longitude, cell heights 210 to 260 m,
    // so each corner is reduced over about 1.4 km.  P = 786.200948 hPa.
    EXPECT_NEAR(gpt3Zhd(synthGrid(), 41.3, 11.7, 1650.0, kDoy129p125), 1.791463723878, 2e-8);
}

TEST(TroposphereMappingLabel, Gpt3DelayFollowsTheDayOfYear) {
    // The same station on day 300.5: the harmonics move the pressure to 796.248000 hPa.
    EXPECT_NEAR(gpt3Zhd(synthGrid(), 41.3, 11.7, 1650.0, kDoy300p5), 1.814357272407, 2e-8);
}

TEST(TroposphereMappingLabel, Gpt3DelayFollowsTheStationsOwnCoordinates) {
    // Another station on the same day (the position of CAS100ATA, 22.5 m): 1052.366890 hPa.
    EXPECT_NEAR(gpt3Zhd(synthGrid(), -66.28, 110.52, 22.5, kDoy129p125), 2.391740967632, 2e-8);
}

TEST(TroposphereMappingLabel, Gpt3DelayAtCas1FromTheShippedGrid) {
    // Rows 2181, 2182, 2253 and 2254 of gpt3_5.grd (the cells at 62.5 S and 67.5 S,
    // 107.5 E and 112.5 E around CAS100ATA), columns copied verbatim:
    //   p (Pa)                           T (K)                       Q (g/kg)                 undu    Hs
    //   98195 245 159 192 -123           268.7 3.4 3.0 0.8 -0.4      2.44 0.58 0.46 0.17 0.01  -13.29  0.00
    //   98227 213 157 207 -129           269.1 3.0 2.9 0.7 -0.3      2.50 0.54 0.45 0.16 0.03  -20.17  0.00
    //   79653 337 269 281 -73            253.6 5.6 1.2 2.2 0.4       0.69 0.40 0.09 0.22 0.06  -14.20  1644.24
    //   85333 241 248 274 -70            256.6 5.9 1.3 2.1 0.5       0.87 0.51 0.11 0.26 0.09  -17.43  1121.25
    // At the field golden's CAS100ATA coordinate on day 129.125: P = 986.708848 hPa (the
    // standard atmosphere gives 1010.55 hPa; the standard-lapse reduction this replaced
    // gave 968.3 hPa, because the two southern cells lie 1.1 and 1.6 km up on the ice sheet).
    const std::string data_dir = GNSS_TEST_DATA_DIR;
    const fs::path grid = fs::path(data_dir) / "static" / "gpt3" / "gpt3_5.grd";
    std::error_code ec;
    if (data_dir.empty() || !fs::exists(grid, ec)) GTEST_SKIP() << "GPT3 grid fixture not present";
    LogManager log;
    auto cfg = loadConfig("gpt3_cas1",
        "\"troposphere_apriori_source\": \"gpt3\", \"gpt3_grid_path\": \"" + grid.generic_string() + "\"",
        log);
    Troposphere tropo(*cfg, log);
    const Eigen::Vector3d cas1(-901776.1304, 2409383.1971, -5816748.5796);
    EXPECT_NEAR(tropo.computeSaastamoinenZHD(cas1, kDoy129p125), 2.242517758988, 2e-8);
}

} // namespace
