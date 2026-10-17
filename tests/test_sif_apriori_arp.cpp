/**
 * @file test_sif_apriori_arp.cpp
 * @brief The single-receiver PPP a-priori from an NGS SIF is reduced from the
 *        monument to the antenna reference point (`reduceSifCoordinateToArp`).
 *
 * A SIF OCCUPATION coordinate refers to the ground reference point (GRP), the
 * monument, and the SIF stores the antenna's offset from it per antenna window
 * (`ARP_MINUS_GRP_ENU`).  The position state is the ARP.  Until 2026-10-07
 * `Application::initializePosition` replaced the ARP-reduced RINEX seed with the
 * raw SIF coordinate under a comment that called it ARP-referenced: one antenna
 * height (plus any horizontal eccentricity) low, 1.964 m at GRAZ00AUT.  The
 * defect was latent, because `PppModel::initializeState` then replaced the seed
 * with the RINEX-derived position before the first epoch; `Application::run`
 * now re-applies the (reduced) SIF seed after it.  That wiring is checked by a
 * Release run, not here: `Application` has no unit seam.
 *
 * Covered here:
 *
 *   1. the SIF coordinate is the monument: GRAZ00AUT's agrees with the published
 *      monument at the millimetre and sits one antenna height below its ARP;
 *   2. the defect and the fix at GRAZ00AUT (antenna height 1.964 m), against the
 *      RINEX APPROX position reduced to the ARP: the pre-fix seed is 1.964 m low,
 *      the reduced seed agrees in height (APPROX is about 1 m off horizontally,
 *      so the horizontal is graded against the monument instead);
 *   3. the RINEX `ANTENNA: DELTA H/E/N` and the SIF `ARP_MINUS_GRP_ENU` agree in
 *      all three components at KRGG00ATF, so the SIF fallback and the header give
 *      the same ARP;
 *   4. synthetic, fixture-free: the header record wins and equals the network
 *      roster reduction (`Coordinates::markerToArp(coordinate, header H/E/N)`)
 *      bit for bit; the SIF window is the fallback for a header with no record,
 *      in the right (E, N, U) -> (H, E, N) order; a recorded 0/0/0 header is
 *      applied and its disagreement with the SIF reported; with neither source
 *      the coordinate is returned unchanged; the window is chosen by epoch.
 *
 * Fixture-backed parts read data/static/sif/igs20_SIF.json and the 2024-129
 * GRAZ00AUT / KRGG00ATF observation files; each is skipped, not failed, when
 * absent.  The RINEX files are parsed to their first 30 s only.
 *
 * Suite: SifAprioriArpTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductParser.hpp"
#include "products/StationInfoFile.hpp"
#include "rinex/RinexParser.hpp"

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// GRAZ00AUT published ITRF2020 monument coordinate (the 2024-129 field golden
/// roster, `config/golden/golden_field_refound.json`) and its header
/// eccentricity, the largest on that roster.
const Eigen::Vector3d kGrazMonument(4194423.4920, 1162703.0403, 4647245.6154);
const Eigen::Vector3d kGrazHen(1.9640, 0.0000, 0.0000);

/// KRGG00ATF: all three eccentricity components populated, in RINEX (H, E, N)
/// order, and its monument (same roster).
const Eigen::Vector3d kKrggMonument(1406336.1576, 3918154.5954, -4816172.8992);
const Eigen::Vector3d kKrggHen(1.0332, 0.0034, 0.0004);

/// Local (E, N, U) components of @p d at @p at.
Eigen::Vector3d enuAt(const Eigen::Vector3d& d, const Eigen::Vector3d& at) {
    return Coordinates::ecefToEnu(d, Coordinates::ecefToLlh(at));
}

void expectBitwiseEqual(const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
    EXPECT_EQ(a.x(), b.x());
    EXPECT_EQ(a.y(), b.y());
    EXPECT_EQ(a.z(), b.z());
}

/// A synthetic station with one antenna window per (start, end, ENU) triple.
StationInfo syntheticStation(
    std::initializer_list<std::tuple<double, double, Eigen::Vector3d>> windows) {
    StationInfo s;
    s.id = "SYNT00TST";
    for (const auto& w : windows) {
        SifAntennaWindow aw;
        aw.start_year = std::get<0>(w);
        aw.end_year   = std::get<1>(w);
        aw.type = "TESTANT         NONE";
        aw.arp_minus_grp_enu = std::get<2>(w);
        s.antennas.push_back(aw);
    }
    return s;
}

/// What `Application::initializePosition` seeds the state with when the SIF is
/// enabled: the marker confirmed by the RINEX ARP seed, the SIF coordinate at
/// the first-observation epoch, reduced to the ARP.  @p unreduced receives the
/// SIF coordinate as the pre-fix code used it.
struct SifSeed {
    bool ok{ false };
    Eigen::Vector3d unreduced{ Eigen::Vector3d::Zero() };
    SifArpReduction red;
};

SifSeed sifSeedLikeApplication(const StationInfoFile& sif, const RinexParser& rp) {
    SifSeed out;
    const Eigen::Vector3d pre_override = rp.getArpPositionXyz();
    const StationInfo* si = sif.find(rp.getHeader().marker_name, &pre_override);
    if (si == nullptr) return out;
    const double year =
        TimeUtils::mjdToDecimalYear(TimeUtils::gpsTimeToUtcMjd(rp.getTimeOfFirstObs()));
    const ResolvedStationCoord rc = resolveStationCoordinate(*si, year);
    if (!rc.valid) return out;
    const Eigen::Vector3d hen = rp.getAntennaDeltaHen();
    out.unreduced = rc.ecef;
    out.red = reduceSifCoordinateToArp(*si, rc.ecef, year,
                                       rp.hasAntennaDeltaHen() ? &hen : nullptr);
    out.ok = true;
    return out;
}

/// The bundled SIF and two 2024-129 observation headers, shared by the suite
/// (spdlog's registry forbids re-registering loggers, and the SIF parse is 8 MB).
class SifAprioriArpTest : public ::testing::Test {
protected:
    static LogManager log_mgr_;
    static std::unique_ptr<StationInfoFile> sif_;

    static void SetUpTestSuite() {
        const std::string data_dir = GNSS_TEST_DATA_DIR;
        if (data_dir.empty()) return;
        const fs::path sif_path = fs::path(data_dir) / "static" / "sif" / "igs20_SIF.json";
        if (!fs::exists(sif_path)) return;
        // Bare LogManager (no initialize()), as in StationInfoTest.
        sif_ = std::make_unique<StationInfoFile>(log_mgr_);
        sif_->load(sif_path);
        if (sif_->empty()) sif_.reset();
    }

    static void TearDownTestSuite() { sif_.reset(); }

    static fs::path obsPath(const std::string& dir, const std::string& file) {
        const std::string data_dir = GNSS_TEST_DATA_DIR;
        if (data_dir.empty()) return {};
        return fs::path(data_dir) / "2024-129" / "obs" / dir / file;
    }

    /// Parse the first 30 s of @p obs through the real config loader and parser.
    static std::unique_ptr<RinexParser> parseHead(const fs::path& obs, const std::string& tag,
                                                  GnssConfig& cfg, std::unique_ptr<ProductParser>& pp) {
        const fs::path j = gnss_test::tempDir() / ("sif_apriori_arp_" + tag + ".json");
        {
            std::string fwd = obs.string();
            for (char& c : fwd) if (c == '\\') c = '/';
            std::ofstream f(j);
            f << "{\n"
              << "  \"processing\": { \"mode\": \"ppp\" },\n"
              << "  \"files\": { \"observation_path\": \"" << fwd << "\" }\n"
              << "}\n";
        }
        const bool loaded = cfg.loadFromJson(j, log_mgr_);
        std::error_code ec;
        fs::remove(j, ec);
        if (!loaded) return nullptr;
        pp = std::make_unique<ProductParser>(cfg, log_mgr_);
        auto rp = std::make_unique<RinexParser>(cfg, log_mgr_, *pp);
        rp->setMaxObservationSpanSeconds(30.0);
        if (!rp->parse()) return nullptr;
        return rp;
    }
};
LogManager SifAprioriArpTest::log_mgr_;
std::unique_ptr<StationInfoFile> SifAprioriArpTest::sif_;

} // namespace

// (1) The SIF coordinate is the monument.  GRAZ00AUT at its first 2024-129
// epoch agrees with the published monument at the millimetre (0.6 mm measured
// 2026-10-07) and sits a full antenna height below the ARP.
TEST_F(SifAprioriArpTest, SifCoordinateIsTheMonumentNotTheArp) {
    if (!sif_) GTEST_SKIP() << "igs20_SIF.json fixture not present";
    const StationInfo* si = sif_->find("GRAZ00AUT");
    ASSERT_NE(si, nullptr);
    const double year = 2024.0 + 128.0 / 366.0;  // 2024-05-08 (DOY 129) 00:00
    const ResolvedStationCoord rc = resolveStationCoordinate(*si, year);
    ASSERT_TRUE(rc.valid);
    EXPECT_FALSE(rc.extrapolated);

    EXPECT_LT((rc.ecef - kGrazMonument).norm(), 0.002);
    const Eigen::Vector3d arp = Coordinates::markerToArp(kGrazMonument, kGrazHen);
    EXPECT_NEAR(enuAt(rc.ecef - arp, arp).z(), -kGrazHen(0), 0.002);

    // The SIF's own record of that eccentricity, (E, N, U).
    const SifArpReduction red = reduceSifCoordinateToArp(*si, rc.ecef, year, nullptr);
    ASSERT_TRUE(red.has_sif_eccentricity);
    EXPECT_NEAR(red.sif_delta_hen(0), 1.964, 1e-9);
    EXPECT_NEAR(red.sif_delta_hen(1), 0.0, 1e-9);
    EXPECT_NEAR(red.sif_delta_hen(2), 0.0, 1e-9);
}

// (2) The defect and the fix, graded against the RINEX APPROX position reduced
// to the ARP.  GRAZ00AUT's APPROX is a monument coordinate whose height is good
// to the millimetre and whose horizontal is about 1 m off, so the height is the
// clean comparison: the pre-fix seed (the raw SIF coordinate) is 1.964 m below
// the RINEX ARP, the reduced seed agrees with it.  The full 3D position is graded
// against the published monument reduced the same way.
TEST_F(SifAprioriArpTest, GrazSeedMatchesRinexApproxReducedToArp) {
    if (!sif_) GTEST_SKIP() << "igs20_SIF.json fixture not present";
    const fs::path obs = obsPath("graz00aut", "GRAZ00AUT_R_20241290000_01D_30S_MO.rnx");
    if (obs.empty() || !fs::exists(obs)) GTEST_SKIP() << "GRAZ00AUT 2024-129 RINEX not present";
    GnssConfig cfg;
    std::unique_ptr<ProductParser> pp;
    const auto rp = parseHead(obs, "graz", cfg, pp);
    ASSERT_NE(rp, nullptr);
    ASSERT_TRUE(rp->hasAntennaDeltaHen());
    ASSERT_NEAR(rp->getAntennaDeltaHen()(0), kGrazHen(0), 1e-9);

    const Eigen::Vector3d rinex_arp =
        Coordinates::markerToArp(rp->getApproxPositionXyz(), rp->getAntennaDeltaHen());

    const SifSeed seed = sifSeedLikeApplication(*sif_, *rp);
    ASSERT_TRUE(seed.ok);
    EXPECT_EQ(seed.red.source, SifEccentricitySource::RinexHeader);
    EXPECT_LT(seed.red.eccentricity_disagreement_m, 1e-9);

    // Pre-fix: the monument coordinate seeded the ARP state, one antenna height low.
    EXPECT_NEAR(enuAt(seed.unreduced - rinex_arp, rinex_arp).z(), -kGrazHen(0), 0.01);
    // Fixed: the seed is the ARP.
    EXPECT_NEAR(enuAt(seed.red.arp_ecef - rinex_arp, rinex_arp).z(), 0.0, 0.01);
    EXPECT_LT((seed.red.arp_ecef - Coordinates::markerToArp(kGrazMonument, kGrazHen)).norm(),
              0.002);
    // The reduction is the network roster's, argument for argument.
    expectBitwiseEqual(seed.red.arp_ecef,
                       Coordinates::markerToArp(seed.unreduced, rp->getAntennaDeltaHen()));
}

// (3) The header and the SIF agree in all three components at KRGG00ATF
// (H/E/N 1.0332 / 0.0034 / 0.0004 m), so the SIF fallback gives the header's ARP
// and an (E, N, U) transposition cannot hide behind a height-only station.
TEST_F(SifAprioriArpTest, KrggHeaderAndSifEccentricityAgree) {
    if (!sif_) GTEST_SKIP() << "igs20_SIF.json fixture not present";
    const fs::path obs = obsPath("krgg00atf", "KRGG00ATF_R_20241290000_01D_30S_MO.rnx");
    if (obs.empty() || !fs::exists(obs)) GTEST_SKIP() << "KRGG00ATF 2024-129 RINEX not present";
    GnssConfig cfg;
    std::unique_ptr<ProductParser> pp;
    const auto rp = parseHead(obs, "krgg", cfg, pp);
    ASSERT_NE(rp, nullptr);
    ASSERT_TRUE(rp->hasAntennaDeltaHen());
    EXPECT_LT((rp->getAntennaDeltaHen() - kKrggHen).norm(), 1e-9);

    const SifSeed seed = sifSeedLikeApplication(*sif_, *rp);
    ASSERT_TRUE(seed.ok);
    ASSERT_TRUE(seed.red.has_sif_eccentricity);
    EXPECT_LT(seed.red.eccentricity_disagreement_m, 1e-9);
    EXPECT_LT((seed.red.sif_delta_hen - kKrggHen).norm(), 1e-9);

    const StationInfo* si = sif_->find("KRGG00ATF");
    ASSERT_NE(si, nullptr);
    const double year =
        TimeUtils::mjdToDecimalYear(TimeUtils::gpsTimeToUtcMjd(rp->getTimeOfFirstObs()));
    const SifArpReduction from_sif = reduceSifCoordinateToArp(*si, seed.unreduced, year, nullptr);
    EXPECT_EQ(from_sif.source, SifEccentricitySource::SifAntennaWindow);
    EXPECT_LT((from_sif.arp_ecef - seed.red.arp_ecef).norm(), 1e-9);
    EXPECT_LT((seed.unreduced - kKrggMonument).norm(), 0.002);
}

// (4a) A header record wins, and the result is the network roster's reduction
// (ApplicationNetworkSetup: Coordinates::markerToArp(position, header H/E/N))
// bit for bit.
TEST_F(SifAprioriArpTest, HeaderRecordWinsAndEqualsRosterReduction) {
    const StationInfo s = syntheticStation(
        { { 2017.1, 9999.0, Eigen::Vector3d(0.0034, 0.0004, 1.0332) } });
    const SifArpReduction red = reduceSifCoordinateToArp(s, kKrggMonument, 2024.35, &kKrggHen);
    EXPECT_EQ(red.source, SifEccentricitySource::RinexHeader);
    expectBitwiseEqual(red.delta_hen, kKrggHen);
    expectBitwiseEqual(red.arp_ecef, Coordinates::markerToArp(kKrggMonument, kKrggHen));
    EXPECT_TRUE(red.has_sif_eccentricity);
    EXPECT_EQ(red.eccentricity_disagreement_m, 0.0);
}

// (4b) With no header record the covering SIF window is applied, reordered from
// (E, N, U) to (H, E, N): the ARP lands at the window's E, N, U above the monument.
TEST_F(SifAprioriArpTest, SifWindowIsTheFallbackForAHeaderWithoutRecord) {
    const StationInfo s = syntheticStation(
        { { 2017.1, 9999.0, Eigen::Vector3d(0.0034, 0.0004, 1.0332) } });
    const SifArpReduction red = reduceSifCoordinateToArp(s, kKrggMonument, 2024.35, nullptr);
    EXPECT_EQ(red.source, SifEccentricitySource::SifAntennaWindow);
    expectBitwiseEqual(red.delta_hen, kKrggHen);
    expectBitwiseEqual(red.arp_ecef, Coordinates::markerToArp(kKrggMonument, kKrggHen));
    const Eigen::Vector3d enu = enuAt(red.arp_ecef - kKrggMonument, kKrggMonument);
    EXPECT_NEAR(enu.x(), 0.0034, 1e-9);
    EXPECT_NEAR(enu.y(), 0.0004, 1e-9);
    EXPECT_NEAR(enu.z(), 1.0332, 1e-9);
    EXPECT_EQ(red.eccentricity_disagreement_m, 0.0);
}

// (4c) A recorded 0/0/0 header is a valid eccentricity: it is applied (an exact
// no-op), and its disagreement with the SIF is reported above the tolerance.
TEST_F(SifAprioriArpTest, RecordedZeroHeaderIsAppliedAndDisagreementReported) {
    const StationInfo s = syntheticStation(
        { { 2017.1, 9999.0, Eigen::Vector3d(0.0, 0.0, 1.964) } });
    const Eigen::Vector3d zero = Eigen::Vector3d::Zero();
    const SifArpReduction red = reduceSifCoordinateToArp(s, kGrazMonument, 2024.35, &zero);
    EXPECT_EQ(red.source, SifEccentricitySource::RinexHeader);
    expectBitwiseEqual(red.arp_ecef, kGrazMonument);
    EXPECT_NEAR(red.eccentricity_disagreement_m, 1.964, 1e-12);
    EXPECT_GT(red.eccentricity_disagreement_m, SIF_ECCENTRICITY_TOLERANCE_M);
}

// (4d) The window is chosen by epoch on [start, end); with no covering window
// and no header record the coordinate is returned unchanged, source None.
TEST_F(SifAprioriArpTest, WindowChosenByEpochAndNoSourceLeavesCoordinate) {
    const StationInfo s = syntheticStation(
        { { 2010.0, 2020.0, Eigen::Vector3d(0.0, 0.0, 1.0) },
          { 2020.0, 2025.0, Eigen::Vector3d(0.0, 0.0, 2.0) } });

    const SifArpReduction early = reduceSifCoordinateToArp(s, kGrazMonument, 2015.0, nullptr);
    EXPECT_EQ(early.source, SifEccentricitySource::SifAntennaWindow);
    EXPECT_EQ(early.delta_hen(0), 1.0);

    const SifArpReduction at_boundary = reduceSifCoordinateToArp(s, kGrazMonument, 2020.0, nullptr);
    EXPECT_EQ(at_boundary.delta_hen(0), 2.0);

    for (const double year : { 2009.0, 2025.0, 2030.0 }) {
        const SifArpReduction none = reduceSifCoordinateToArp(s, kGrazMonument, year, nullptr);
        EXPECT_EQ(none.source, SifEccentricitySource::None) << year;
        EXPECT_FALSE(none.has_sif_eccentricity) << year;
        expectBitwiseEqual(none.arp_ecef, kGrazMonument);
        expectBitwiseEqual(none.delta_hen, Eigen::Vector3d::Zero());
    }
}

} // namespace GnssProcessor
