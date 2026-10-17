/**
 * @file test_gpt3_load_path.cpp
 * @brief Unit tests for the GPT3 grid load path and the troposphere a priori
 *        pressure source.
 *
 * Three behaviours are fixed here, all with a synthetic grid so the suite needs
 * no data fixture:
 *
 *  1. Units of the mapping coefficients.  The published @c gpt3_5.grd stores the
 *     VMF3 hydrostatic and wet "a" coefficients multiplied by 1000 (a_h of about
 *     1.2 for a physical 1.2e-3).  @c Gpt3::query returns the physical value.  An
 *     unscaled read puts a_h three orders of magnitude too large, which drives
 *     the VMF3 mapping below 1 at every elevation under 90 degrees, so it is clamped to 1
 *     and the estimated zenith wet delay absorbs the whole slant error.
 *  2. A failed @c load() leaves the instance unloaded and empty.  A reload that
 *     fails after clearing the cells used to keep @c isLoaded() true, and the
 *     next @c query() indexed an empty vector.
 *  3. The Saastamoinen ZHD takes its pressure from GPT3 only when
 *     @c receiver.troposphere_apriori_source is @c gpt3.  Selecting the VMF3
 *     mapping function loads the grid for the @c a coefficients but does not
 *     change the a priori pressure source.
 */

#include <gtest/gtest.h>

#include "atmosphere/Gpt3.hpp"
#include "atmosphere/Troposphere.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Values written into every cell of a synthetic grid.  The pressure, the
/// temperature and the two coefficients are constant over the globe, and every
/// annual and semi-annual term is zero, so a query returns them exactly at any
/// position and day.
struct GridSpec {
    double p_pa{ 101325.0 };   ///< pressure a0, Pa
    double t_k{ 288.15 };      ///< temperature a0, K
    double a_h{ 1.2 };         ///< hydrostatic coefficient a0 as stored (1e3 x physical)
    double a_w{ 0.55 };        ///< wet coefficient a0 as stored (1e3 x physical)
    int rows{ 2592 };          ///< number of data rows written
    int cols{ 64 };            ///< columns written per data row
    bool header{ true };       ///< write the leading '%' line
};

/// Write a gpt3_5.grd-shaped file: one '%' line, then rows of 64 columns in the
/// published order (lat, lon, p x5, T x5, Q x5, dT x5, undu, Hs, a_h x5, a_w x5,
/// then the remaining columns, zero here).
///
/// A file is written once per process and reused by name, since parsing a grid is
/// the slow step of these tests in a Debug build.  A name therefore stands for one
/// spec: two specs never share a name.
fs::path writeGrid(const std::string& name, const GridSpec& s) {
    static std::map<std::string, fs::path> written;
    const auto it = written.find(name);
    if (it != written.end()) return it->second;

    const fs::path path = gnss_test::tempDir() / name;
    std::string text;
    if (s.header) text += "%  lat    lon   p:a0 A1 B1 A2 B2 T:a0 ...\n";
    for (int r = 0; r < s.rows; ++r) {
        double v[64] = {};
        v[0]  = 87.5 - 5.0 * (r / 72);   // latitude
        v[1]  = 2.5 + 5.0 * (r % 72);    // longitude
        v[2]  = s.p_pa;                  // p a0, then A1..B2 stay 0
        v[7]  = s.t_k;                   // T a0
        v[24] = s.a_h;                   // a_h a0
        v[29] = s.a_w;                   // a_w a0
        for (int c = 0; c < s.cols && c < 64; ++c) {
            char tok[32];
            if (v[c] == 0.0) std::snprintf(tok, sizeof(tok), "0");
            else             std::snprintf(tok, sizeof(tok), "%.10g", v[c]);
            text += tok;
            if (c + 1 < s.cols) text += ' ';
        }
        text += '\n';
    }
    std::ofstream f(path, std::ios::binary);
    f << text;
    f.close();
    written[name] = path;
    return path;
}

// ---------------------------------------------------------------------------
// 1. Coefficient units
// ---------------------------------------------------------------------------

TEST(Gpt3LoadPath, QueryReturnsPhysicalMappingCoefficients) {
    LogManager log;
    Gpt3 grid;
    ASSERT_TRUE(grid.load(writeGrid("default.grd", GridSpec{}), log));

    Gpt3::Result r{};
    ASSERT_TRUE(grid.query(34.2, -118.2, 100.0, 129.25, r));
    EXPECT_NEAR(r.a_h, 1.2e-3, 1e-15);
    EXPECT_NEAR(r.a_w, 0.55e-3, 1e-15);
}

TEST(Gpt3LoadPath, RejectsCoefficientsOutsideThePlausibleRange) {
    LogManager log;
    Gpt3 grid;

    // Already-physical values (a file rescaled by the user) would be read as
    // 1e-6 after the 1e-3 scaling.
    GridSpec physical;
    physical.a_h = 1.2e-3;
    physical.a_w = 0.55e-3;
    EXPECT_FALSE(grid.load(writeGrid("prescaled.grd", physical), log));
    EXPECT_FALSE(grid.isLoaded());

    // A coefficient scaled up again by 1e3 is outside the range as well.
    GridSpec huge;
    huge.a_h = 1200.0;
    EXPECT_FALSE(grid.load(writeGrid("huge.grd", huge), log));
    EXPECT_FALSE(grid.isLoaded());

    // The wet coefficient is checked independently of the hydrostatic one.
    GridSpec wet;
    wet.a_w = 55.0;
    EXPECT_FALSE(grid.load(writeGrid("wet.grd", wet), log));
    EXPECT_FALSE(grid.isLoaded());
}

TEST(Gpt3LoadPath, ScaledCoefficientsGiveAMappingNearNiell) {
    // Mapping at 10 degrees from the grid coefficients must stay near the
    // Niell value (about 5.55 at 45 N) and well above 1.  With the unscaled
    // coefficient the same call returns a value below 1.
    LogManager log;
    GridSpec s;
    s.a_h = 1.2265;
    s.a_w = 0.557;
    const fs::path grid_file = writeGrid("mapping.grd", s);

    const fs::path cfg_path = gnss_test::tempDir() / "vmf3_mapping.json";
    {
        std::ofstream f(cfg_path);
        f << "{ \"processing\": { \"mode\": \"ppp\" },\n"
             "  \"receiver\": { \"troposphere_mapping_function\": \"vmf3\",\n"
             "                  \"gpt3_grid_path\": \"" << grid_file.generic_string() << "\" } }\n";
    }
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(cfg_path, log));
    Troposphere tropo(cfg, log);

    constexpr double DEG2RAD = Constants::PI / 180.0;
    const Eigen::Vector3d rx = Coordinates::llhToEcef(45.0 * DEG2RAD, -75.0 * DEG2RAD, 200.0);
    const GnssTime t{ 2313, 280800.0 };
    double mf_d = 0.0, mf_w = 0.0;
    tropo.getTroposphereMapping(10.0, rx, t, mf_d, mf_w);
    EXPECT_GT(mf_d, 5.0);
    EXPECT_LT(mf_d, 6.2);
    EXPECT_GT(mf_w, 5.0);
    EXPECT_LT(mf_w, 6.2);
}

// ---------------------------------------------------------------------------
// 2. Failed load leaves the instance unloaded and empty
// ---------------------------------------------------------------------------

TEST(Gpt3LoadPath, FailedReloadLeavesTheInstanceUnloaded) {
    LogManager log;
    Gpt3 grid;
    ASSERT_TRUE(grid.load(writeGrid("default.grd", GridSpec{}), log));
    ASSERT_TRUE(grid.isLoaded());
    Gpt3::Result r{};
    ASSERT_TRUE(grid.query(10.0, 20.0, 0.0, 100.0, r));

    // Valid header, too few rows: the cells are cleared part way through the
    // parse, so the instance must not report itself loaded.
    GridSpec short_grid;
    short_grid.rows = 100;
    EXPECT_FALSE(grid.load(writeGrid("short.grd", short_grid), log));
    EXPECT_FALSE(grid.isLoaded());
    EXPECT_FALSE(grid.query(10.0, 20.0, 0.0, 100.0, r));
}

TEST(Gpt3LoadPath, EveryFailureKindUnloadsAPreviouslyLoadedInstance) {
    LogManager log;
    Gpt3 loaded;
    ASSERT_TRUE(loaded.load(writeGrid("default.grd", GridSpec{}), log));

    GridSpec few_cols;
    few_cols.rows = 3;
    few_cols.cols = 40;
    GridSpec no_header;
    no_header.rows = 3;
    no_header.header = false;

    struct Case { const char* label; fs::path path; };
    const Case cases[] = {
        { "missing file",  gnss_test::tempDir() / "does_not_exist.grd" },
        { "no header",     writeGrid("noheader.grd", no_header) },
        { "short row",     writeGrid("fewcols.grd", few_cols) },
    };
    for (const auto& c : cases) {
        Gpt3 grid = loaded;                      // an instance holding a valid grid
        ASSERT_TRUE(grid.isLoaded()) << c.label;
        EXPECT_FALSE(grid.load(c.path, log)) << c.label;
        EXPECT_FALSE(grid.isLoaded()) << c.label;
        Gpt3::Result r{};
        EXPECT_FALSE(grid.query(0.0, 0.0, 0.0, 1.0, r)) << c.label;
    }
}

TEST(Gpt3LoadPath, SuccessfulReloadReplacesTheGrid) {
    LogManager log;
    Gpt3 grid;
    ASSERT_TRUE(grid.load(writeGrid("default.grd", GridSpec{}), log));
    GridSpec other;
    other.p_pa = 90000.0;
    ASSERT_TRUE(grid.load(writeGrid("b.grd", other), log));
    Gpt3::Result r{};
    ASSERT_TRUE(grid.query(0.0, 0.0, 0.0, 1.0, r));
    EXPECT_NEAR(r.pressure_mbar, 900.0, 1e-6);
}

// ---------------------------------------------------------------------------
// 3. A priori pressure source
// ---------------------------------------------------------------------------

/// A Troposphere for one receiver block, with a synthetic grid whose pressure
/// differs clearly from the standard atmosphere (900 mbar at height 0).
struct TropoRig {
    LogManager log;
    GnssConfig cfg;
    std::unique_ptr<Troposphere> tropo;

    TropoRig(const std::string& tag, const std::string& apriori, const std::string& mapping,
             bool with_grid) {
        GridSpec s;
        s.p_pa = 90000.0;
        const fs::path grid_file = with_grid ? writeGrid("zhd.grd", s)
                                             : gnss_test::tempDir() / "absent.grd";
        const fs::path cfg_path = gnss_test::tempDir() / ("zhd_" + tag + ".json");
        {
            std::ofstream f(cfg_path);
            f << "{ \"processing\": { \"mode\": \"ppp\" },\n"
                 "  \"receiver\": {\n"
                 "    \"troposphere_apriori_source\": \"" << apriori << "\",\n"
                 "    \"troposphere_mapping_function\": \"" << mapping << "\",\n"
                 "    \"gpt3_grid_path\": \"" << grid_file.generic_string() << "\"\n"
                 "  } }\n";
        }
        EXPECT_TRUE(cfg.loadFromJson(cfg_path, log));
        tropo = std::make_unique<Troposphere>(cfg, log);
    }

    /// ZHD in metres at 40 N, 105 W, height 0, on a fixed epoch.
    double zhd() const {
        constexpr double DEG2RAD = Constants::PI / 180.0;
        const Eigen::Vector3d rx = Coordinates::llhToEcef(40.0 * DEG2RAD, -105.0 * DEG2RAD, 0.0);
        const GnssTime t{ 2313, 280800.0 };
        return tropo->computeSaastamoinenZHD(rx, t);
    }
};

/// One rig per receiver block for the whole process: building a rig parses a grid.
/// With @p with_grid false the grid path points at a missing file.
const TropoRig& rig(const std::string& apriori, const std::string& mapping, bool with_grid = true) {
    static std::map<std::string, std::unique_ptr<TropoRig>> rigs;
    std::string key = apriori + "_" + mapping + (with_grid ? "_grid" : "_nogrid");
    auto& slot = rigs[key];
    if (!slot) slot = std::make_unique<TropoRig>(key, apriori, mapping, with_grid);
    return *slot;
}

TEST(TroposphereAprioriPressure, StandardAtmosphereWithVmf3KeepsTheConfiguredPressure) {
    // The mapping function does not move the a priori pressure.
    EXPECT_DOUBLE_EQ(rig("standard_atmosphere", "vmf3").zhd(),
                     rig("standard_atmosphere", "gmf", false).zhd());
}

TEST(TroposphereAprioriPressure, Gpt3SourceTakesTheGridPressure) {
    // The synthetic grid reads 900 mbar against 1013.25 mbar of the standard
    // atmosphere at height 0, so the ZHDs differ by the pressure ratio.
    const double std_zhd  = rig("standard_atmosphere", "gmf", false).zhd();
    const double gpt3_zhd = rig("gpt3", "gmf").zhd();
    EXPECT_NEAR(gpt3_zhd / std_zhd, 900.0 / 1013.25, 1e-6);
    // Together with VMF3 the pressure is the same as with GMF.
    EXPECT_DOUBLE_EQ(rig("gpt3", "vmf3").zhd(), gpt3_zhd);
}

TEST(TroposphereAprioriPressure, MissingGridFallsBackToTheStandardAtmosphere) {
    EXPECT_DOUBLE_EQ(rig("gpt3", "vmf3", false).zhd(),
                     rig("standard_atmosphere", "gmf", false).zhd());
}

// ---------------------------------------------------------------------------
// 4. The shipped grid (skipped when data/static/gpt3/gpt3_5.grd is absent)
// ---------------------------------------------------------------------------

class Gpt3RealGrid : public ::testing::Test {
protected:
    static LogManager log_;
    static Gpt3       model_;
    static bool       present_;   ///< grid file found on disk
    static bool       loaded_;    ///< load() succeeded

    static void SetUpTestSuite() {
        const std::string data_dir = GNSS_TEST_DATA_DIR;
        if (data_dir.empty()) return;
        const fs::path grid = fs::path(data_dir) / "static" / "gpt3" / "gpt3_5.grd";
        std::error_code ec;
        if (!fs::exists(grid, ec)) return;
        present_ = true;
        loaded_  = model_.load(grid, log_);
    }

    void SetUp() override {
        if (!present_) GTEST_SKIP() << "GPT3 grid fixture not present";
        ASSERT_TRUE(loaded_) << "the shipped GPT3 grid is present but Gpt3::load() refused it";
    }
};
LogManager Gpt3RealGrid::log_;
Gpt3       Gpt3RealGrid::model_;
bool       Gpt3RealGrid::present_ = false;
bool       Gpt3RealGrid::loaded_  = false;

// The mapping coefficients are physical over the whole globe and all seasons:
// a_h near 1.2e-3 and a_w near 0.55e-3 (Niell: 1.27e-3 and 0.58e-3).
TEST_F(Gpt3RealGrid, MappingCoefficientsArePhysicalEverywhere) {
    for (double lat = -85.0; lat <= 85.0; lat += 10.0) {
        for (double lon = -180.0; lon < 180.0; lon += 20.0) {
            for (double doy : { 30.0, 200.0 }) {
                Gpt3::Result r{};
                ASSERT_TRUE(model_.query(lat, lon, 100.0, doy, r));
                EXPECT_GT(r.a_h, 1.0e-3) << lat << " " << lon << " " << doy;
                EXPECT_LT(r.a_h, 1.4e-3) << lat << " " << lon << " " << doy;
                EXPECT_GT(r.a_w, 0.3e-3) << lat << " " << lon << " " << doy;
                EXPECT_LT(r.a_w, 0.8e-3) << lat << " " << lon << " " << doy;
            }
        }
    }
}

// Line 2 of gpt3_5.grd (87.5 N, 2.5 E), a_h columns copied verbatim:
//   a_h: a0=1.18309  A1=-0.03573  B1=-0.01009  A2=0.00516  B2=0.00399   [x1000]
TEST_F(Gpt3RealGrid, CornerCellMatchesTheStoredHarmonicInPhysicalUnits) {
    constexpr double doy = 100.0;
    constexpr double omega = 2.0 * Constants::PI / 365.25;
    const double expected =
        (1.18309 + (-0.03573) * std::cos(omega * doy) + (-0.01009) * std::sin(omega * doy) +
         0.00516 * std::cos(2.0 * omega * doy) + 0.00399 * std::sin(2.0 * omega * doy)) * 1.0e-3;
    Gpt3::Result r{};
    ASSERT_TRUE(model_.query(87.5, 2.5, 19.36, doy, r));
    EXPECT_NEAR(r.a_h, expected, 1e-12);
}

} // namespace
