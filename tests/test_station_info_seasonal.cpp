/**
 * @file test_station_info_seasonal.cpp
 * @brief The periodic (seasonal) term of the SIF coordinate function outside
 *        its block windows (`resolveStationCoordinate`).
 *
 * The linear term of a SIF coordinate goes on past a closed occupation
 * segment, and the periodic blocks end with the segments they were fitted
 * with.  Until this rule the seasonal term was summed over the blocks that
 * cover the epoch and was therefore exactly zero outside every window: a step
 * of up to several millimetres where the window ended, while the linear term
 * went on.  The rule is now the one of NGS M-PAGES v1.2
 * (`PeriodicMotion::computeDisplacement`), pinned here:
 *
 *   1. one block supplies the term: the first, in start order, whose window
 *      covers the epoch (start inclusive, end exclusive);
 *   2. before every window the first block, at or after the end of the last
 *      window the last block, and inside a gap the block that ended last;
 *   3. the order of the blocks in the file does not matter, and overlapping
 *      windows are not summed;
 *   4. the block's sinusoids continue past its window with the phase taken
 *      from its own reference epoch, so the term is continuous where the last
 *      window ends;
 *   5. where a block covers the epoch the result is bit-for-bit the one the
 *      summing rule gave.
 *
 * Synthetic cases (fixture-free) build `StationInfo` records directly; one
 * case writes a SIF JSON file to check that the loader keeps the file order and
 * that `resolveRosterCoordinate` carries the diagnostics.  The fixture-backed
 * cases read data/static/sif/{igs20,ncn20}_SIF.json (skipped, not failed, when
 * absent): the 8 stations of the 2024-129 field golden roster whose epoch lies
 * past their last periodic window, with the offset the old rule left out (zero
 * by construction there) and the one now applied.
 *
 * Suite: SifSeasonalTermTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include "logging/LogManager.hpp"
#include "products/StationInfoFile.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;

/// Epoch of the 2024-129 field golden: data_download year 2024, doy 129, at the
/// half-day back-off the config loader applies (366-day year).
constexpr double kGoldenEpoch = 2024.0 + (129.0 - 0.5) / 366.0;

// --- Synthetic records ---------------------------------------------------

/// One annual frequency; the amplitudes differ per block (see @ref blockAt) so
/// the block that supplied a term is identifiable from the displacement alone.
SifPeriodicFreq annual(const Eigen::Vector3d& cos_amp, const Eigen::Vector3d& sin_amp,
                       double period_yr = 1.0) {
    SifPeriodicFreq f;
    f.period_yr = period_yr;
    f.x_cos = cos_amp.x(); f.y_cos = cos_amp.y(); f.z_cos = cos_amp.z();
    f.x_sin = sin_amp.x(); f.y_sin = sin_amp.y(); f.z_sin = sin_amp.z();
    return f;
}

/// A periodic block over [start, end) whose amplitudes are @p k millimetres (so
/// block k is distinguishable from block j), referred to 2020.0 as the loader does.
SifPeriodicBlock blockAt(double start, double end, double k) {
    SifPeriodicBlock b;
    b.start_year = start;
    b.end_year = end;
    b.reference_epoch_year = 2020.0;
    b.freqs.push_back(annual({ 1.0e-3 * k, -0.5e-3 * k, 0.25e-3 * k },
                             { -0.75e-3 * k, 1.5e-3 * k, 2.0e-3 * k }));
    return b;
}

/// A station with one open-ended occupation (no velocity, so the linear term is
/// a constant) and the given periodic blocks in the given (file) order.
StationInfo stationWith(std::vector<SifPeriodicBlock> blocks) {
    StationInfo s;
    s.id = "SEAS00TST";
    SifOccupation o;
    o.start_year = 1990.0;
    o.end_year = 9999.0;
    o.coordinates = Eigen::Vector3d(4000000.0, 1000000.0, 4800000.0);
    o.velocity = Eigen::Vector3d::Zero();
    o.reference_epoch_year = 2020.0;
    s.occupations.push_back(o);
    s.periodic = std::move(blocks);
    return s;
}

/// The displacement of one block at @p t, evaluated from the definition.
Eigen::Vector3d blockDisplacement(const SifPeriodicBlock& b, double t) {
    Eigen::Vector3d d = Eigen::Vector3d::Zero();
    for (const auto& f : b.freqs) {
        if (f.period_yr == 0.0) continue;
        const double w = 2.0 * kPi / f.period_yr;
        const double dT = t - b.reference_epoch_year;
        d.x() += f.x_cos * std::cos(w * dT) + f.x_sin * std::sin(w * dT);
        d.y() += f.y_cos * std::cos(w * dT) + f.y_sin * std::sin(w * dT);
        d.z() += f.z_cos * std::cos(w * dT) + f.z_sin * std::sin(w * dT);
    }
    return d;
}

/// The seasonal term as the caller sees it: the coordinate with the periodic
/// term minus the coordinate without it (same occupation, no post-seismic term).
Eigen::Vector3d seasonalTerm(const StationInfo& s, double t) {
    const ResolvedStationCoord with    = resolveStationCoordinate(s, t, true,  false);
    const ResolvedStationCoord without = resolveStationCoordinate(s, t, false, false);
    return with.ecef - without.ecef;
}

/// The summing rule that preceded the block selection: every block whose window
/// covers @p t, in file order, added into the coordinate one frequency at a time.
/// Kept to show that a covered epoch is unchanged bit for bit.
Eigen::Vector3d legacyCoordinate(const StationInfo& s, double t) {
    Eigen::Vector3d out = resolveStationCoordinate(s, t, false, false).ecef;
    for (const auto& b : s.periodic) {
        if (t < b.start_year || t >= b.end_year) continue;
        const double dT = t - b.reference_epoch_year;
        for (const auto& f : b.freqs) {
            if (f.period_yr == 0.0) continue;
            const double w = 2.0 * 3.14159265358979323846 / f.period_yr;
            const double c = std::cos(w * dT), sn = std::sin(w * dT);
            out.x() += f.x_cos * c + f.x_sin * sn;
            out.y() += f.y_cos * c + f.y_sin * sn;
            out.z() += f.z_cos * c + f.z_sin * sn;
        }
    }
    return out;
}

/// The three blocks used by most cases: [2010,2014) k=1, [2015,2019) k=2 (a one-year
/// gap before it) and [2019,2022) k=3 (contiguous with the second).
std::vector<SifPeriodicBlock> threeBlocks() {
    return { blockAt(2010.0, 2014.0, 1.0), blockAt(2015.0, 2019.0, 2.0), blockAt(2019.0, 2022.0, 3.0) };
}

void expectTerm(const StationInfo& s, double t, const SifPeriodicBlock& expect_block,
                const char* what) {
    const Eigen::Vector3d got  = seasonalTerm(s, t);
    const Eigen::Vector3d want = blockDisplacement(expect_block, t);
    EXPECT_LT((got - want).norm(), 5e-9) << what << ": got " << got.transpose()
                                         << " want " << want.transpose();
}

// --- Fixture: the synthetic registry (loader and roster cases) and the real SIFs ---

/// A SIF JSON document with one station whose periodic blocks are written in
/// file order [2015-2019, 2019-2022, 2010-2014]: the first stored block is not
/// the earliest.  Block k has X_COS = k mm so the evaluated block is visible.
json seasonalSif() {
    const auto freq = [](double k) {
        return json{ { "PERIOD", 1.0 }, { "X_COS", k }, { "X_SIN", 0.0 },
                     { "Y_COS", 0.0 }, { "Y_SIN", 0.0 }, { "Z_COS", 0.0 }, { "Z_SIN", 0.0 } };
    };
    const auto block = [&](const char* start, const char* end, double k) {
        return json{ { "START", start }, { "END", end }, { "FREQUENCIES", json::array({ freq(k) }) } };
    };
    json occupation{
        { "START", "1990-01-01T00:00:00 GPS" }, { "END", "9999-12-31T00:00:00 GPS" },
        { "REFERENCE_FRAME", "ITRF2020" }, { "REFERENCE_EPOCH", 2020.0 },
        { "COORDINATES", json::array({ 4000000.0, 1000000.0, 4800000.0 }) },
        { "COORDINATE_SIGMAS", json::array({ 0.002, 0.002, 0.003 }) },
        { "VELOCITY", json::array({ 0.0, 0.0, 0.0 }) },
        { "VELOCITY_SIGMAS", json::array({ 0.1, 0.1, 0.2 }) },
        { "6X6_COVARIANCE_MATRIX", std::vector<double>(15, 0.0) },
        { "DO_NOT_USE", false }, { "SOURCE", "synthetic" } };
    json st{
        { "ID", "SEAS00TST" }, { "DOMES", "" }, { "DESCRIPTION", "synthetic" },
        { "APPROXIMATE_COORDINATES", json::array({ 4000000.0, 1000000.0, 4800000.0 }) },
        { "ANTENNA", json::array() }, { "RECEIVER", json::array() },
        { "OCCUPATION", json::array({ occupation }) },
        { "NONLINEAR_TERMS", { { "PERIODIC_TERMS", json::array({
              block("2015-01-01T00:00:00 GPS", "2019-01-01T00:00:00 GPS", 2.0),
              block("2019-01-01T00:00:00 GPS", "2022-01-01T00:00:00 GPS", 3.0),
              block("2010-01-01T00:00:00 GPS", "2014-01-01T00:00:00 GPS", 1.0) }) } } } };
    return json{ { "METADATA", { { "REFERENCE_FRAME", "ITRF2020" }, { "REFERENCE_EPOCH", 2020.0 } } },
                 { "STATIONS", json::array({ st }) } };
}

// One LogManager and both registries reused across the suite.  Bare LogManager
// (no initialize()): getLogger falls back to a transient console logger, and
// initialize() would clash with spdlog's global registry when another suite has
// already initialized in this process.
class SifSeasonalTermTest : public ::testing::Test {
protected:
    static LogManager log_mgr_;
    static std::unique_ptr<StationInfoFile> syn_;   ///< The one-station synthetic file.
    static std::unique_ptr<StationInfoFile> real_;  ///< igs20 + ncn20, loaded on first use (see real()).
    static bool real_tried_;

    static void SetUpTestSuite() {
        syn_ = std::make_unique<StationInfoFile>(log_mgr_);
        const fs::path p = gnss_test::tempDir() / "gnss_station_info_seasonal.json";
        std::ofstream(p) << seasonalSif().dump(1);
        syn_->load(p);
        std::error_code ec;
        fs::remove(p, ec);
    }

    static void TearDownTestSuite() {
        syn_.reset();
        real_.reset();
        real_tried_ = false;
    }

    /// igs20 + ncn20, parsed on first use (about 25 s in a Debug build, so only
    /// when a fixture-backed case runs), or nullptr when either file is absent.
    static const StationInfoFile* real() {
        if (!real_tried_) {
            real_tried_ = true;
            const std::string data_dir = GNSS_TEST_DATA_DIR;
            if (data_dir.empty()) return nullptr;
            const fs::path dir = fs::path(data_dir) / "static" / "sif";
            const fs::path igs = dir / "igs20_SIF.json";
            const fs::path ncn = dir / "ncn20_SIF.json";
            if (!fs::exists(igs) || !fs::exists(ncn)) return nullptr;
            auto reg = std::make_unique<StationInfoFile>(log_mgr_);
            reg->load(igs);
            reg->load(ncn);
            if (!reg->empty()) real_ = std::move(reg);
        }
        return real_.get();
    }
};

/// Skip the calling test when the shipped SIFs are absent.  A macro, not a
/// member function: GTEST_SKIP returns from the function it expands in, which
/// must be the test body.
#define REQUIRE_REAL_SIF()\
    do {\
        if (real() == nullptr) GTEST_SKIP() << "igs20_SIF.json / ncn20_SIF.json fixtures not present";\
    } while (0)

LogManager SifSeasonalTermTest::log_mgr_;
std::unique_ptr<StationInfoFile> SifSeasonalTermTest::syn_;
std::unique_ptr<StationInfoFile> SifSeasonalTermTest::real_;
bool SifSeasonalTermTest::real_tried_ = false;

// --- 1. The covering block -------------------------------------------------

TEST_F(SifSeasonalTermTest, CoveredEpochEvaluatesItsOwnBlock) {
    const StationInfo s = stationWith(threeBlocks());
    const ResolvedStationCoord r = resolveStationCoordinate(s, 2016.4, true, false);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::Covered);
    EXPECT_FALSE(r.periodic.extrapolated());
    EXPECT_EQ(r.periodic.extrapolation_years, 0.0);
    EXPECT_EQ(r.periodic.block_start_year, 2015.0);
    EXPECT_EQ(r.periodic.block_end_year, 2019.0);
    expectTerm(s, 2016.4, s.periodic[1], "covered, middle block");
    EXPECT_EQ(describePeriodicExtrapolation(r.periodic), "");
}

TEST_F(SifSeasonalTermTest, WindowStartIsInclusiveAndEndExclusive) {
    const StationInfo s = stationWith(threeBlocks());
    // 2019.0 is the end of block 2 and the start of block 3: block 3 covers it.
    EXPECT_EQ(resolveStationCoordinate(s, 2019.0, true, false).periodic.block_start_year, 2019.0);
    EXPECT_EQ(resolveStationCoordinate(s, 2019.0, true, false).periodic.reach, SifPeriodicReach::Covered);
    // The first instant of block 2 is covered; the first instant of the gap before it is not.
    EXPECT_EQ(resolveStationCoordinate(s, 2015.0, true, false).periodic.reach, SifPeriodicReach::Covered);
    const ResolvedStationCoord at_end = resolveStationCoordinate(s, 2014.0, true, false);
    EXPECT_EQ(at_end.periodic.reach, SifPeriodicReach::InGap);
    EXPECT_EQ(at_end.periodic.block_start_year, 2010.0);   // block 1 ended last
    EXPECT_EQ(at_end.periodic.extrapolation_years, 0.0);   // exactly at its end
    expectTerm(s, 2014.0, s.periodic[0], "gap at the end of block 1");
}

// --- 2. Outside every window -------------------------------------------------

TEST_F(SifSeasonalTermTest, BeforeTheFirstWindowUsesTheFirstBlock) {
    const StationInfo s = stationWith(threeBlocks());
    const ResolvedStationCoord r = resolveStationCoordinate(s, 2008.5, true, false);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::BeforeFirst);
    EXPECT_TRUE(r.periodic.extrapolated());
    EXPECT_NEAR(r.periodic.extrapolation_years, 2008.5 - 2010.0, 1e-12);   // negative
    EXPECT_EQ(r.periodic.block_start_year, 2010.0);
    expectTerm(s, 2008.5, s.periodic[0], "before the first window");
}

TEST_F(SifSeasonalTermTest, AfterTheLastWindowUsesTheLastBlock) {
    const StationInfo s = stationWith(threeBlocks());
    const ResolvedStationCoord r = resolveStationCoordinate(s, 2023.25, true, false);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::AfterLast);
    EXPECT_NEAR(r.periodic.extrapolation_years, 2023.25 - 2022.0, 1e-12);  // positive
    EXPECT_EQ(r.periodic.block_end_year, 2022.0);
    expectTerm(s, 2023.25, s.periodic[2], "after the last window");
    // The end of the last window itself is already outside it.
    EXPECT_EQ(resolveStationCoordinate(s, 2022.0, true, false).periodic.reach, SifPeriodicReach::AfterLast);
}

TEST_F(SifSeasonalTermTest, InsideAGapUsesTheBlockThatEndedLast) {
    const StationInfo s = stationWith(threeBlocks());
    const ResolvedStationCoord r = resolveStationCoordinate(s, 2014.6, true, false);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::InGap);
    EXPECT_NEAR(r.periodic.extrapolation_years, 2014.6 - 2014.0, 1e-12);
    EXPECT_EQ(r.periodic.block_start_year, 2010.0);
    expectTerm(s, 2014.6, s.periodic[0], "in the gap, the earlier block (not the next)");
}

TEST_F(SifSeasonalTermTest, TheSinusoidContinuesPastTheWindowOnTheBlocksOwnPhase) {
    // A block that is evaluated outside its window keeps its reference epoch:
    // 6 months after the window end the phase has advanced 6 months, which is
    // not the phase at the window end.
    const StationInfo s = stationWith(threeBlocks());
    const Eigen::Vector3d at_end   = seasonalTerm(s, 2022.0);
    const Eigen::Vector3d later    = seasonalTerm(s, 2022.5);
    EXPECT_GT((later - at_end).norm(), 1e-3);
    EXPECT_LT((later - blockDisplacement(s.periodic[2], 2022.5)).norm(), 5e-9);
}

TEST_F(SifSeasonalTermTest, TheTermIsContinuousWhereTheLastWindowEnds) {
    // The step the summing rule left: a term of several millimetres that fell to
    // zero at the end of the last window.  Now it goes on without a step.
    const StationInfo s = stationWith(threeBlocks());
    const double eps = 1e-6;   // about 30 s
    const Eigen::Vector3d before = seasonalTerm(s, 2022.0 - eps);
    const Eigen::Vector3d after  = seasonalTerm(s, 2022.0 + eps);
    EXPECT_GT(before.norm(), 1e-3) << "the term must be non-trivial at the end for the test to bite";
    EXPECT_LT((after - before).norm(), 5e-7);   // the term moves at most ~5 cm/yr: ~1e-7 m over 2e-6 yr
}

// --- 3. Order and overlap ---------------------------------------------------

TEST_F(SifSeasonalTermTest, FileOrderDoesNotChangeTheSelection) {
    const std::vector<SifPeriodicBlock> chrono = threeBlocks();
    // Every permutation of the three blocks, at an epoch of each selection case.
    std::array<int, 3> idx = { 0, 1, 2 };
    do {
        std::vector<SifPeriodicBlock> shuffled;
        for (int i : idx) shuffled.push_back(chrono[static_cast<std::size_t>(i)]);
        const StationInfo s = stationWith(shuffled);
        expectTerm(s, 2008.5, chrono[0], "before first, shuffled");
        expectTerm(s, 2012.0, chrono[0], "covered by block 1, shuffled");
        expectTerm(s, 2014.6, chrono[0], "gap, shuffled");
        expectTerm(s, 2016.0, chrono[1], "covered by block 2, shuffled");
        expectTerm(s, 2020.0, chrono[2], "covered by block 3, shuffled");
        expectTerm(s, 2024.0, chrono[2], "after last, shuffled");
        EXPECT_EQ(resolveStationCoordinate(s, 2014.6, true, false).periodic.reach, SifPeriodicReach::InGap);
    } while (std::next_permutation(idx.begin(), idx.end()));
}

TEST_F(SifSeasonalTermTest, OverlappingWindowsAreNotSummed) {
    // Blocks 1 and 2 overlap on [2012, 2014): only the first in start order applies.
    const StationInfo s = stationWith({ blockAt(2010.0, 2014.0, 1.0), blockAt(2012.0, 2018.0, 2.0) });
    const ResolvedStationCoord r = resolveStationCoordinate(s, 2013.0, true, false);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::Covered);
    EXPECT_EQ(r.periodic.block_start_year, 2010.0);
    expectTerm(s, 2013.0, s.periodic[0], "overlap: first block only");
    // Past the first block's end the second one is the only cover.
    expectTerm(s, 2016.0, s.periodic[1], "after the overlap: second block");
}

TEST_F(SifSeasonalTermTest, EqualStartsKeepFileOrder) {
    const SifPeriodicBlock a = blockAt(2010.0, 2020.0, 1.0);
    const SifPeriodicBlock b = blockAt(2010.0, 2020.0, 2.0);
    expectTerm(stationWith({ a, b }), 2015.0, a, "equal starts: file order, a first");
    expectTerm(stationWith({ b, a }), 2015.0, b, "equal starts: file order, b first");
}

TEST_F(SifSeasonalTermTest, AnOpenEndedLastBlockCoversEveryLaterEpoch) {
    std::vector<SifPeriodicBlock> blocks = threeBlocks();
    blocks.back().end_year = 9999.0;
    const StationInfo s = stationWith(blocks);
    const ResolvedStationCoord r = resolveStationCoordinate(s, 2060.0, true, false);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::Covered);
    EXPECT_EQ(r.periodic.block_end_year, 9999.0);
}

// --- 4. Off, absent and independent ------------------------------------------

TEST_F(SifSeasonalTermTest, SwitchedOffOrAbsentIsNotEvaluated) {
    const StationInfo s = stationWith(threeBlocks());
    const ResolvedStationCoord off = resolveStationCoordinate(s, 2023.0, false, false);
    EXPECT_EQ(off.periodic.reach, SifPeriodicReach::NotEvaluated);
    EXPECT_FALSE(off.periodic.extrapolated());
    EXPECT_EQ(describePeriodicExtrapolation(off.periodic), "");

    const StationInfo none = stationWith({});
    const ResolvedStationCoord r = resolveStationCoordinate(none, 2023.0, true, false);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::NotEvaluated);
    EXPECT_EQ(seasonalTerm(none, 2023.0).norm(), 0.0);
}

TEST_F(SifSeasonalTermTest, ABlockWithAZeroPeriodAddsNothing) {
    SifPeriodicBlock b = blockAt(2010.0, 2020.0, 1.0);
    b.freqs.push_back(annual({ 9.0, 9.0, 9.0 }, { 9.0, 9.0, 9.0 }, /*period_yr=*/0.0));
    const StationInfo s = stationWith({ b });
    const Eigen::Vector3d got = seasonalTerm(s, 2015.0);
    EXPECT_TRUE(got.allFinite());
    EXPECT_LT((got - blockDisplacement(b, 2015.0)).norm(), 5e-9);
}

TEST_F(SifSeasonalTermTest, TheSelectionIsIndependentOfTheOccupationSelection) {
    // The occupation closed in 2018, so the linear term extrapolates at 2020.5;
    // the periodic block still covers that epoch (and the reverse at 2016.5).
    StationInfo s = stationWith({ blockAt(2010.0, 2030.0, 2.0) });
    s.occupations[0].end_year = 2018.0;
    ResolvedStationCoord r = resolveStationCoordinate(s, 2020.5, true, false);
    EXPECT_TRUE(r.extrapolated);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::Covered);

    const StationInfo closed_block = stationWith({ blockAt(2010.0, 2018.0, 2.0) });
    r = resolveStationCoordinate(closed_block, 2016.5, true, false);
    EXPECT_FALSE(r.extrapolated);
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::Covered);
    r = resolveStationCoordinate(closed_block, 2020.5, true, false);
    EXPECT_FALSE(r.extrapolated);   // the occupation here is still open
    EXPECT_EQ(r.periodic.reach, SifPeriodicReach::AfterLast);
}

TEST_F(SifSeasonalTermTest, CoveredEpochsAreBitIdenticalToTheSummingRule) {
    // Non-overlapping blocks: wherever one covers the epoch the new rule adds the
    // same terms in the same order as the summing rule did.
    const StationInfo s = stationWith(threeBlocks());
    for (double t : { 2010.0, 2011.37, 2013.999, 2015.0, 2017.25, 2018.9999, 2019.0, 2021.5, 2021.99 }) {
        const Eigen::Vector3d now = resolveStationCoordinate(s, t, true, false).ecef;
        const Eigen::Vector3d was = legacyCoordinate(s, t);
        EXPECT_EQ(now.x(), was.x()) << "t=" << t;
        EXPECT_EQ(now.y(), was.y()) << "t=" << t;
        EXPECT_EQ(now.z(), was.z()) << "t=" << t;
    }
}

TEST_F(SifSeasonalTermTest, TheWarningTextNamesTheCase) {
    const StationInfo s = stationWith(threeBlocks());
    const std::string before = describePeriodicExtrapolation(resolveStationCoordinate(s, 2008.5, true, false).periodic);
    const std::string after  = describePeriodicExtrapolation(resolveStationCoordinate(s, 2023.25, true, false).periodic);
    const std::string gap    = describePeriodicExtrapolation(resolveStationCoordinate(s, 2014.6, true, false).periodic);
    EXPECT_NE(before.find("1.500 yr before the first periodic block [2010.0000, 2014.0000)"), std::string::npos) << before;
    EXPECT_NE(after.find("1.250 yr after the last periodic block [2019.0000, 2022.0000)"), std::string::npos) << after;
    EXPECT_NE(gap.find("0.600 yr after the periodic block [2010.0000, 2014.0000) and before the next one"),
              std::string::npos) << gap;
}

// --- 5. The loader and the roster path ------------------------------------------

TEST_F(SifSeasonalTermTest, LoaderKeepsFileOrderAndTheSelectionDoesNotDependOnIt) {
    const StationInfo* s = syn_->find("SEAS00TST");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->periodic.size(), 3u);
    EXPECT_GT(s->periodic[0].start_year, s->periodic[2].start_year) << "the file lists the blocks out of order";
    // Block k carries X_COS = k mm, so the x term at a cosine peak identifies it.
    EXPECT_EQ(resolveStationCoordinate(*s, 2008.0, true, false).periodic.block_start_year, s->periodic[2].start_year);
    EXPECT_EQ(resolveStationCoordinate(*s, 2024.0, true, false).periodic.block_end_year, s->periodic[1].end_year);
    const ResolvedStationCoord gap = resolveStationCoordinate(*s, 2014.5, true, false);
    EXPECT_EQ(gap.periodic.reach, SifPeriodicReach::InGap);
    EXPECT_EQ(gap.periodic.block_start_year, s->periodic[2].start_year);
}

TEST_F(SifSeasonalTermTest, RosterCoordinateCarriesThePeriodicDiagnostics) {
    const auto covered = resolveRosterCoordinate(*syn_, "SEAS00TST", 2016.0);
    ASSERT_TRUE(covered.has_value());
    EXPECT_EQ(covered->periodic.reach, SifPeriodicReach::Covered);

    const auto after = resolveRosterCoordinate(*syn_, "SEAS00TST", 2024.5);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->periodic.reach, SifPeriodicReach::AfterLast);
    EXPECT_NEAR(after->periodic.extrapolation_years, 2024.5 - 2022.0, 1e-12);
    // The roster coordinate includes the continued term: 3 mm * cos(2*pi*(t - 2020)).
    const StationInfo* s = syn_->find("SEAS00TST");
    ASSERT_NE(s, nullptr);
    const Eigen::Vector3d linear = resolveStationCoordinate(*s, 2024.5, false, false).ecef;
    EXPECT_NEAR(after->ecef.x() - linear.x(), 3.0e-3 * std::cos(2.0 * kPi * (2024.5 - 2020.0)), 5e-9);
}

// --- 6. The shipped SIFs: the 8 stations of the field golden roster ------------

struct RosterExtrapolation {
    const char* id;
    std::array<double, 3> seasonal_xyz;   ///< The seasonal ECEF term now applied [m].
    double years_past_end;                ///< t - (end of the last periodic window) [yr].
    double block_start;                   ///< The evaluated (last) block's window.
    double block_end;
};

// Captured from the evaluation of each station's last periodic block at the golden
// epoch (the Python twin tools/sif_propagate agrees with the C++ to 1e-9 m).
// The pre-fix rule applied none of it: each epoch is past every window.
constexpr std::array<RosterExtrapolation, 8> kRoster = { {
    { "BSHM00ISR", { 0.000390350, -0.000331744,  0.000834791}, 0.359312, 2023.101370, 2023.991781 },
    { "PALM00ATA", {-0.000266316, -0.001007491, -0.000626162}, 0.331968, 2020.568306, 2024.019125 },
    { "NIUM00NIU", {-0.000479490, -0.000263441,  0.000028151}, 0.331968, 2022.950685, 2024.019125 },
    { "CHUR00CAN", {-0.001482611,  0.004894193, -0.003925403}, 0.230875, 2010.671233, 2024.120218 },
    { "MAJU00MHL", {-0.001004803, -0.000469438,  0.000000759}, 3.353826, 2015.441096, 2020.997267 },
    { "CPVG00CPV", { 0.000651877, -0.001013427, -0.000729831}, 0.214481, 2023.635616, 2024.136612 },
    { "URUM00CHN", { 0.000981005,  0.000920497,  0.002439989}, 0.211749, 2018.767123, 2024.139344 },
    { "WROC00POL", { 0.000022002, -0.000243837,  0.000364945}, 0.225411, 2012.808743, 2024.125682 },
} };

TEST_F(SifSeasonalTermTest, EightRosterStationsGainTheirSeasonalTermAtTheGoldenEpoch) {
    REQUIRE_REAL_SIF();
    for (const auto& want : kRoster) {
        SCOPED_TRACE(want.id);
        const StationInfo* s = real()->find(want.id);
        ASSERT_NE(s, nullptr);
        ASSERT_EQ(s->id, want.id);

        // Before: no window covers the epoch, so the summing rule applied nothing.
        for (const auto& b : s->periodic)
            EXPECT_TRUE(kGoldenEpoch < b.start_year || kGoldenEpoch >= b.end_year);

        // After: the last block, continued.
        const ResolvedStationCoord r = resolveStationCoordinate(*s, kGoldenEpoch, true, false);
        ASSERT_TRUE(r.valid);
        EXPECT_EQ(r.periodic.reach, SifPeriodicReach::AfterLast);
        EXPECT_NEAR(r.periodic.extrapolation_years, want.years_past_end, 5e-7);
        EXPECT_NEAR(r.periodic.block_start_year, want.block_start, 5e-7);
        EXPECT_NEAR(r.periodic.block_end_year, want.block_end, 5e-7);
        EXPECT_TRUE(r.extrapolated) << "the linear term extrapolates too";

        const Eigen::Vector3d got = seasonalTerm(*s, kGoldenEpoch);
        const Eigen::Vector3d expect(want.seasonal_xyz[0], want.seasonal_xyz[1], want.seasonal_xyz[2]);
        EXPECT_LT((got - expect).norm(), 5e-9) << got.transpose() << " vs " << expect.transpose();
        EXPECT_GT(got.norm(), 4.0e-4);   // the smallest of the eight is 0.44 mm
    }
    // The two largest offsets: CHUR00CAN 6.4 mm, URUM00CHN 2.8 mm.
    const Eigen::Vector3d chur = seasonalTerm(*real()->find("CHUR00CAN"), kGoldenEpoch);
    const Eigen::Vector3d urum = seasonalTerm(*real()->find("URUM00CHN"), kGoldenEpoch);
    EXPECT_NEAR(chur.norm(), 6.447e-3, 5e-6);
    EXPECT_NEAR(urum.norm(), 2.786e-3, 5e-6);
}

TEST_F(SifSeasonalTermTest, ABlockBeforeItsFirstWindowIsTheOracleCaseOfThePythonTwin) {
    REQUIRE_REAL_SIF();
    // AMC400USA at 2018.5 lies 2.63 yr before its only periodic window (2021.13).
    // The first block's term is continued: 0.68 mm on top of the linear term,
    // where the summing rule gave the linear term alone.  The same numbers are
    // the oracle row of tools/sif_propagate/test_sif_propagate.py.
    const StationInfo* s = real()->find("AMC400USA");
    ASSERT_NE(s, nullptr);
    const ResolvedStationCoord full = resolveStationCoordinate(*s, 2018.5, true, true);
    const ResolvedStationCoord lin  = resolveStationCoordinate(*s, 2018.5, false, false);
    ASSERT_TRUE(full.valid);
    EXPECT_EQ(full.periodic.reach, SifPeriodicReach::BeforeFirst);
    EXPECT_NEAR(full.periodic.extrapolation_years, 2018.5 - s->periodic.front().start_year, 1e-12);
    EXPECT_LT((lin.ecef - Eigen::Vector3d(-1248596.393444710, -4819428.201899112, 3976505.924241956)).norm(), 5e-9);
    EXPECT_LT((full.ecef - Eigen::Vector3d(-1248596.393551557, -4819428.201226350, 3976505.924238630)).norm(), 5e-9)
        << full.ecef.transpose();
    EXPECT_NEAR((full.ecef - lin.ecef).norm(), 6.81e-4, 5e-6);   // sub-millimetre, not zero
}

TEST_F(SifSeasonalTermTest, CoveredRosterStationsAreUnchangedAtTheGoldenEpoch) {
    REQUIRE_REAL_SIF();
    // Roster stations whose open last block covers the golden epoch: the same bits
    // as the summing rule.
    for (const char* id : { "JPLM00USA", "CUSV00THA", "POVE00BRA", "LCK400IND", "YKRO00CIV",
                            "KRGG00ATF", "GRAZ00AUT", "WTZR00DEU", "DARW00AUS", "REYK00ISL" }) {
        SCOPED_TRACE(id);
        const StationInfo* s = real()->find(id);
        ASSERT_NE(s, nullptr);
        const ResolvedStationCoord r = resolveStationCoordinate(*s, kGoldenEpoch, true, false);
        ASSERT_TRUE(r.valid);
        EXPECT_EQ(r.periodic.reach, SifPeriodicReach::Covered);
        const Eigen::Vector3d was = legacyCoordinate(*s, kGoldenEpoch);
        const Eigen::Vector3d now = r.ecef;
        EXPECT_EQ(now.x(), was.x());
        EXPECT_EQ(now.y(), was.y());
        EXPECT_EQ(now.z(), was.z());
    }
}

TEST_F(SifSeasonalTermTest, RosterPathReportsTheExtrapolatedStations) {
    REQUIRE_REAL_SIF();
    int extrapolated = 0;
    for (const auto& want : kRoster) {
        const auto rc = resolveRosterCoordinate(*real(), want.id, kGoldenEpoch);
        ASSERT_TRUE(rc.has_value()) << want.id;
        EXPECT_TRUE(rc->periodic.extrapolated()) << want.id;
        EXPECT_FALSE(describePeriodicExtrapolation(rc->periodic).empty()) << want.id;
        ++extrapolated;
    }
    EXPECT_EQ(extrapolated, 8);
}

} // namespace
} // namespace GnssProcessor
