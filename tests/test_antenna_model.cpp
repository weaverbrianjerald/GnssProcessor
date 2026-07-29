/**
 * @file test_antenna_model.cpp
 * @brief Unit tests for the ANTEX antenna model — PCV interpolation, mounting
 *        orientation, and the '+'-signed PCV parse path.
 *
 * Scope: Antenna::interpolatePcv (the pure bilinear ANTEX PCV grid
 * interpolator), Antenna::correctAzimuth (mounting-orientation rotation,
 * ported from M-PAGES NGS::RxAntenna::correctAzimuth), and the
 * ProductParser::parseAtx PCV hot-loop as reached through a synthetic ATX.
 *
 * Depth: boundary + convention locks on hand-computable synthetic grids
 * (value = 10·row + col, so every corner is identifiable by inspection).
 * Two of these are May-2026 regression pins — the last-zenith-ring
 * out-of-bounds read and the from_chars '+'-sign rejection — both of which
 * failed silently in production rather than crashing.
 *
 * @see test_antenna_radome_fallback.cpp  radome -> NONE type-mean fallback
 * @see test_math_core.cpp                Coordinates / TimeUtils / Interpolate
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/Antenna.hpp"
#include "products/ProductParser.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// ============================================================================
// Antenna PCV interpolation — bilinear, NOAZI, and the last-ring edge
// ============================================================================
//
// Antenna::interpolatePcv is a pure ANTEX PCV grid interpolator.  These cases
// pin the bilinear math, the NOAZI (azimuth-independent) path, the azimuth
// wrap, and — the May-2026 fix — the last-zenith-ring boundary, which
// previously dereferenced pcv_table[zen_idx + 1] out of bounds (the bounds
// check was placed AFTER the access).  A query at zenith == the table's
// largest ring must hold the value flat, not read past the table.
//
// Synthetic 4×5 grid (zenith rings 0/30/60/90, azimuth cols 0/90/180/270/360):
// value = 10·row + col, so every corner is identifiable by inspection.
TEST(AntennaPcvTest, BilinearNoaziAndLastRingClamp) {
    const std::vector<std::vector<double>> grid = {
        {  0,  1,  2,  3,  4 },   // zenith 0°
        { 10, 11, 12, 13, 14 },   // zenith 30°
        { 20, 21, 22, 23, 24 },   // zenith 60°
        { 30, 31, 32, 33, 34 },   // zenith 90°
    };
    constexpr double dzen = 30.0, dazi = 90.0;

    // (a) Interior bilinear at zen=15, az=45 (frac 0.5/0.5 in both):
    //     mean of corners 0,1,10,11 = 5.5.
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 15.0, 45.0, dazi, dzen), 5.5, 1e-12);

    // (b) Last-ring edge (the fix): zen=90 sits exactly on the last ring.
    //     zen_frac=0, so the result is the last-ring value at az=45:
    //     (30 + 31)/2 = 30.5.  Pre-fix this read pcv_table[4] out of bounds.
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 90.0, 45.0, dazi, dzen), 30.5, 1e-12);

    // (c) Beyond the last ring (zen=120) clamps to the last ring — same 30.5,
    //     and again no out-of-bounds access.
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 120.0, 45.0, dazi, dzen), 30.5, 1e-12);

    // (d) Azimuth wrap: az=360 → az_idx=4, az_next=(4+1)%5=0, az_frac=0 →
    //     returns column 4 of row 0 = 4.0 (and exercises the %cols wrap path
    //     without going out of bounds).
    EXPECT_NEAR(Antenna::interpolatePcv(grid, 0.0, 360.0, dazi, dzen), 4.0, 1e-12);

    // (e) NOAZI (single column) path with its own last-ring clamp.
    const std::vector<std::vector<double>> noazi = { {0}, {10}, {20}, {30} };
    EXPECT_NEAR(Antenna::interpolatePcv(noazi, 15.0, 0.0, dazi, dzen),  5.0, 1e-12); // mid 0→30
    EXPECT_NEAR(Antenna::interpolatePcv(noazi, 90.0, 0.0, dazi, dzen), 30.0, 1e-12); // last ring
    EXPECT_NEAR(Antenna::interpolatePcv(noazi, 120.0, 0.0, dazi, dzen), 30.0, 1e-12); // clamp
}

// ============================================================================
// Receiver antenna orientation — azimuth correction
// ============================================================================
//
// Antenna::correctAzimuth rotates a measured azimuth (CW from North) into the
// antenna frame by subtracting the mounting orientation, wrapping into
// [0, 360).  Ported from M-PAGES NGS::RxAntenna::correctAzimuth.  Pins the
// reference vector from the Item-5 verify steps plus the two wrap branches and
// the orientation==0 identity (which keeps orientation-off runs byte-identical).
TEST(AntennaOrientationTest, CorrectAzimuthMatchesMpagesReference) {
    // Reference vector: 10 - 90 = -80 -> +360 -> 280.
    EXPECT_NEAR(GnssProcessor::Antenna::correctAzimuth(10.0, 90.0), 280.0, 1e-12);

    // orientation == 0 is the identity (off-path bit-identical).
    EXPECT_NEAR(GnssProcessor::Antenna::correctAzimuth(123.456, 0.0), 123.456, 1e-12);

    // No wrap needed when az >= orientation.
    EXPECT_NEAR(GnssProcessor::Antenna::correctAzimuth(200.0, 50.0), 150.0, 1e-12);

    // Negative branch wraps up by 360.
    EXPECT_NEAR(GnssProcessor::Antenna::correctAzimuth(0.0, 270.0), 90.0, 1e-12);

    // A negative orientation (az - (-x) = az + x) wraps down past 360.
    EXPECT_NEAR(GnssProcessor::Antenna::correctAzimuth(350.0, -20.0), 10.0, 1e-12);
}

// Regression: std::from_chars' float overload accepts '-' but rejects a leading
// '+'.  Some IGS calibration labs (Geo++ robot files — e.g. the LEIAR25 NONE /
// LEIT type-means in igs20.atx) prefix EVERY PCV value with '+', so the parser's
// from_chars-based PCV hot-loop stopped at the first value and stored an EMPTY
// PCV grid.  That silently zeroed receiver PCV and, under require_atx_coverage,
// dropped all of that antenna's signals (e.g. "Skipping C2W … no strict ATX
// coverage").  This ATX uses '+'-signed NOAZI + azimuth PCV; the grid must parse.
TEST(AntennaPcvTest, AtxPlusSignedPcvGridParses) {
    auto col60 = [](std::string body, const std::string& tag) {
        if (body.size() < 60) body.resize(60, ' ');
        return body + tag + "\n";
    };
    std::string atx;
    atx += col60("",                               "START OF ANTENNA");
    atx += col60("TESTANT         NONE",            "TYPE / SERIAL NO");
    atx += col60("    90.0",                        "DAZI");
    atx += col60("     0.0  10.0   5.0",            "ZEN1 / ZEN2 / DZEN");
    atx += col60("     1",                          "# OF FREQUENCIES");
    atx += col60("   G01",                          "START OF FREQUENCY");
    atx += col60("     +1.41     +1.00   +155.34",  "NORTH / EAST / UP");
    atx += "   NOAZI   +0.00   +0.50   +1.00\n";
    atx += "     0.0   +0.00   +0.50   +1.00\n";   // azimuth rows (DAZI 90°): 0,90,180,270,360
    atx += "    90.0   +0.10   +0.60   +1.10\n";
    atx += "   180.0   +0.20   +0.70   +1.20\n";
    atx += "   270.0   +0.30   +0.80   +1.30\n";
    atx += "   360.0   +0.00   +0.50   +1.00\n";
    atx += col60("   G01",                          "END OF FREQUENCY");
    atx += col60("",                                "END OF ANTENNA");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "gnss_test_plus_signed.atx";
    { std::ofstream o(tmp); o << atx; }

    GnssConfig    config;
    LogManager    lm;
    ProductParser pp(config, lm);
    std::vector<AntennaData> ants;
    ASSERT_TRUE(pp.parseAtx(tmp, ants));
    fs::remove(tmp);

    const AntennaData* e = nullptr;
    for (const auto& a : ants) if (a.type == "TESTANT_NONE") { e = &a; break; }
    ASSERT_NE(e, nullptr) << "receiver entry not parsed";

    // PCO (+-signed) parses via parseDouble — always worked.
    ASSERT_EQ(e->pco.count("G01"), 1u);
    EXPECT_NEAR(e->pco.at("G01").z(), 155.34, 1e-9);

    // PCV grid must be NON-EMPTY (the bug) — 3 zenith (0,5,10) × 5 azimuth.
    ASSERT_EQ(e->pcv.count("G01"), 1u);
    const auto& grid = e->pcv.at("G01");
    ASSERT_FALSE(grid.empty()) << "PCV grid empty — '+'-signed values failed to parse";
    ASSERT_EQ(grid.size(), 3u);
    ASSERT_EQ(grid.front().size(), 5u);
    // grid[zen][az]: zenith 10° (idx 2), azimuth 90° (idx 1) row value = +1.10.
    EXPECT_NEAR(grid[2][1], 1.10, 1e-9);
}

}  // namespace
}  // namespace GnssProcessor
