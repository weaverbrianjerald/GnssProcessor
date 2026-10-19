/**
 * @file test_station_info_psd.cpp
 * @brief The post-seismic term of the SIF coordinate function
 *        (`resolveStationCoordinate`) is the change since the occupation's
 *        reference epoch.
 *
 * The reference coordinate of an occupation (ITRF2020 at 2020.0 in the shipped
 * files) is the model position at that epoch with the post-seismic displacement
 * accumulated by then already in it: it differs from the secular position of the
 * ITRF2020 IGS SSC by psd(t_ref).  The coordinate function is therefore
 *
 *   x(t) = x_ref + v (t - t_ref) + periodic(t) + [psd(t) - psd(t_ref)]
 *
 * with psd(t) the sum over earthquakes of the log/exp relaxation since each
 * event (zero before it).  Adding psd(t) in full counted the displacement
 * accumulated before t_ref twice: 1.48 m at MIZU00JPN and about 5 cm at
 * GCGO00USA and MAC100AUS in 2024.  Pinned here:
 *
 *   1. the term is exactly zero at the reference epoch;
 *   2. after the reference epoch it is the change since then, and before it the
 *      change is negative;
 *   3. before an event the term is minus the displacement the reference
 *      coordinate carries (the pre-event linear position);
 *   4. an earthquake after the reference epoch has nothing to subtract and enters
 *      in full once it has happened;
 *   5. the reference epoch is the occupation's own, not the file's;
 *   6. with the term switched off the linear coordinate is unchanged.
 *
 * Synthetic cases (fixture-free) build `StationInfo` records directly.  The
 * fixture-backed cases read data/static/sif/{igs20,ncn20}_SIF.json (skipped, not
 * failed, when absent): the eight stations of the 2024-129 field golden roster
 * that carry a post-seismic term at the epoch are compared with the IGS weekly
 * combined SINEX of GPS week 2313 (a mean position at 2024-05-08 12:00), and
 * ANTC00CHL pins the numbers of the Python twin tools/sif_propagate.
 *
 * Suite: SifPostSeismicTermTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "products/StationInfoFile.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// Epoch of the 2024-129 field golden: data_download year 2024, doy 129, at the
/// half-day back-off the config loader applies (366-day year).  It is the
/// reference epoch of the weekly SINEX, 2024-05-08 12:00.
constexpr double kGoldenEpoch = 2024.0 + (129.0 - 0.5) / 366.0;

// --- Synthetic records ---------------------------------------------------

/// One local component's relaxation parameters.
SifPsdComponent component(std::vector<double> aexp, std::vector<double> texp,
                          std::vector<double> alog = {}, std::vector<double> tlog = {}) {
    SifPsdComponent c;
    c.aexp = std::move(aexp);
    c.texp = std::move(texp);
    c.alog = std::move(alog);
    c.tlog = std::move(tlog);
    return c;
}

/// An earthquake on 1 January of @p year, so its epoch is the integer year.
SifPsdBlock quake(double year, SifPsdComponent east, SifPsdComponent north, SifPsdComponent up) {
    SifPsdBlock b;
    b.event_year = year;
    b.east = std::move(east);
    b.north = std::move(north);
    b.up = std::move(up);
    return b;
}

/// Mixed log and exp terms in every component.
SifPsdBlock quakeA() {
    return quake(2010.0,
                 component({ 0.020, 0.005 }, { 4.0, 0.2 }, { 0.010 }, { 0.5 }),
                 component({}, {}, { -0.030 }, { 1.0 }),
                 component({ 0.004 }, { 2.0 }));
}
/// After the reference epoch 2020.0, east only.
SifPsdBlock quakeB() { return quake(2021.0, component({ 0.050 }, { 1.5 }), {}, {}); }
/// Before the occupation of @ref stationWith at reference 2010.0, north only.
SifPsdBlock quakeC() { return quake(2005.0, {}, component({ 0.040 }, { 3.0 }), {}); }

/// A station with one open-ended occupation (no velocity, so the linear term is a
/// constant) referred to @p ref_epoch, carrying @p blocks.
StationInfo stationWith(double ref_epoch, std::vector<SifPsdBlock> blocks) {
    StationInfo s;
    s.id = "PSD00TST";
    SifOccupation o;
    o.start_year = 1990.0;
    o.end_year = 9999.0;
    o.coordinates = Eigen::Vector3d(4000000.0, 1000000.0, 4800000.0);
    o.velocity = Eigen::Vector3d::Zero();
    o.reference_epoch_year = ref_epoch;
    s.occupations.push_back(o);
    s.psd = std::move(blocks);
    return s;
}

/// One component's relaxation since an event, from the definition [m].
double relaxation(const SifPsdComponent& c, double dT) {
    if (dT <= 0.0) return 0.0;
    double d = 0.0;
    for (std::size_t i = 0; i < c.alog.size(); ++i) d += c.alog[i] * std::log(1.0 + dT / c.tlog[i]);
    for (std::size_t i = 0; i < c.aexp.size(); ++i) d += c.aexp[i] * (1.0 - std::exp(-dT / c.texp[i]));
    return d;
}

/// The accumulated post-seismic (E, N, U) at @p year over @p blocks [m].
Eigen::Vector3d accumulated(const std::vector<SifPsdBlock>& blocks, double year) {
    Eigen::Vector3d out = Eigen::Vector3d::Zero();
    for (const auto& b : blocks) {
        const double dT = year - b.event_year;
        out += Eigen::Vector3d(relaxation(b.east, dT), relaxation(b.north, dT), relaxation(b.up, dT));
    }
    return out;
}

/// The post-seismic term as the caller sees it: the coordinate with the term minus
/// the coordinate without it (periodic term off in both), as local ENU at the
/// station [m].
Eigen::Vector3d appliedTerm(const StationInfo& s, double t) {
    const Eigen::Vector3d full = resolveStationCoordinate(s, t, false, true).ecef;
    const Eigen::Vector3d lin  = resolveStationCoordinate(s, t, false, false).ecef;
    return Coordinates::ecefToEnu(full - lin, Coordinates::ecefToLlh(lin));
}

constexpr double kTol = 1.0e-7;   // ECEF round trip of a 6e6 m position

// --- Synthetic cases ------------------------------------------------------

TEST(SifPostSeismicTermTest, TheTermIsExactlyZeroAtTheReferenceEpoch) {
    const StationInfo s = stationWith(2020.0, { quakeA() });
    const Eigen::Vector3d full = resolveStationCoordinate(s, 2020.0, false, true).ecef;
    const Eigen::Vector3d lin  = resolveStationCoordinate(s, 2020.0, false, false).ecef;
    EXPECT_EQ(full.x(), lin.x());
    EXPECT_EQ(full.y(), lin.y());
    EXPECT_EQ(full.z(), lin.z());
}

TEST(SifPostSeismicTermTest, SwitchedOffTheLinearCoordinateIsUnchanged) {
    const StationInfo s = stationWith(2020.0, { quakeA() });
    const ResolvedStationCoord off = resolveStationCoordinate(s, 2024.0, false, false);
    EXPECT_EQ(off.ecef, Eigen::Vector3d(4000000.0, 1000000.0, 4800000.0));
}

TEST(SifPostSeismicTermTest, AfterAndBeforeTheReferenceEpochItIsTheChangeSinceThen) {
    const StationInfo s = stationWith(2020.0, { quakeA() });
    for (const double t : { 2015.0, 2022.0, 2024.0, 2030.0 }) {
        SCOPED_TRACE(t);
        const Eigen::Vector3d got  = appliedTerm(s, t);
        const Eigen::Vector3d want = accumulated(s.psd, t) - accumulated(s.psd, 2020.0);
        EXPECT_LT((got - want).norm(), kTol) << got.transpose() << " vs " << want.transpose();
        // The relaxation since the event, which the term used to be, is far from it.
        EXPECT_GT((accumulated(s.psd, t) - want).norm(), 0.02);
    }
    // North relaxes negatively and further with time, so the change back to 2015 is positive.
    EXPECT_GT(appliedTerm(s, 2015.0).y(), 0.0);
}

TEST(SifPostSeismicTermTest, BeforeTheEventItIsMinusTheDisplacementTheReferenceCarries) {
    const StationInfo s = stationWith(2020.0, { quakeA() });
    const Eigen::Vector3d got = appliedTerm(s, 2005.0);
    EXPECT_LT((got + accumulated(s.psd, 2020.0)).norm(), kTol) << got.transpose();
}

TEST(SifPostSeismicTermTest, AnEarthquakeAfterTheReferenceEpochEntersInFull) {
    const StationInfo s = stationWith(2020.0, { quakeA(), quakeB() });
    // 2020.5: the later event has not happened, the earlier one relaxes on.
    EXPECT_EQ(accumulated({ quakeB() }, 2020.5).x(), 0.0);
    for (const double t : { 2020.5, 2023.0 }) {
        SCOPED_TRACE(t);
        const Eigen::Vector3d got  = appliedTerm(s, t);
        const Eigen::Vector3d want = accumulated(s.psd, t) - accumulated(s.psd, 2020.0);
        EXPECT_LT((got - want).norm(), kTol) << got.transpose() << " vs " << want.transpose();
    }
    // At 2023.0 the later event adds its whole relaxation (36.8 mm east).
    EXPECT_GT(accumulated({ quakeB() }, 2023.0).x(), 0.03);
    EXPECT_LT((appliedTerm(s, 2023.0) - appliedTerm(stationWith(2020.0, { quakeA() }), 2023.0)
               - accumulated({ quakeB() }, 2023.0)).norm(), kTol);
}

TEST(SifPostSeismicTermTest, TheReferenceEpochIsTheOccupationsOwn) {
    const StationInfo s = stationWith(2010.0, { quakeC() });
    const Eigen::Vector3d got  = appliedTerm(s, 2024.0);
    const Eigen::Vector3d want = accumulated(s.psd, 2024.0) - accumulated(s.psd, 2010.0);
    EXPECT_LT((got - want).norm(), kTol) << got.transpose() << " vs " << want.transpose();
    const Eigen::Vector3d full = resolveStationCoordinate(s, 2010.0, false, true).ecef;
    const Eigen::Vector3d lin  = resolveStationCoordinate(s, 2010.0, false, false).ecef;
    EXPECT_EQ(full, lin) << "zero at the occupation's own epoch, not at 2020.0";
    // Evaluated against 2020.0 instead, the term would differ by the 2010 to 2020 change.
    EXPECT_GT((accumulated(s.psd, 2020.0) - accumulated(s.psd, 2010.0)).norm(), 0.005);
}

// --- Fixture-backed cases --------------------------------------------------

/// One LogManager and the real registries reused across the suite.  Bare
/// LogManager (no initialize()): getLogger falls back to a transient console
/// logger, and initialize() would clash with spdlog's global registry when
/// another suite has already initialized in this process.
class SifPostSeismicRealTest : public ::testing::Test {
protected:
    static LogManager log_mgr_;
    static std::unique_ptr<StationInfoFile> real_;  ///< igs20 + ncn20, loaded on first use.
    static bool real_tried_;

    static void TearDownTestSuite() {
        real_.reset();
        real_tried_ = false;
    }

    /// igs20 + ncn20, parsed on first use (about 25 s in a Debug build), or nullptr
    /// when either file is absent.
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

LogManager SifPostSeismicRealTest::log_mgr_;
std::unique_ptr<StationInfoFile> SifPostSeismicRealTest::real_;
bool SifPostSeismicRealTest::real_tried_ = false;

/// Skip the calling test when the shipped SIFs are absent.  A macro, not a
/// member function: GTEST_SKIP returns from the function it expands in.
#define REQUIRE_REAL_SIF()\
    do {\
        if (real() == nullptr) GTEST_SKIP() << "igs20_SIF.json / ncn20_SIF.json fixtures not present";\
    } while (0)

struct SinexStation {
    const char* id;
    std::array<double, 3> ecef;   ///< SINEX mean position at 2024-05-08 12:00 [m].
    double tol_mm;                ///< Per horizontal component.
};

// The IGS weekly combined SINEX of GPS week 2313 (IGS0OPSSNX_20241260000_07D_07D_SOL.SNX): a
// mean position at 2024-05-08 12:00 with formal 3D sigmas of 0.3 to 1.9 mm.  The tolerance is
// twice the 95th percentile (4.1 mm) of the same horizontal difference over 264 stations
// without a post-seismic block, so it stands for weekly noise and unmodelled motion, not for
// the post-seismic term; NIUM00NIU is extrapolated 0.33 yr past its occupation across an
// antenna change and gets 12 mm.
constexpr std::array<SinexStation, 8> kSinex = { {
    { "GCGO00USA", { -2281621.77483, -1453585.12678,  5756964.86859 },  8.0 },
    { "SOLO00SLB", { -5911340.08837,  2156887.93540, -1038663.46655 },  8.0 },
    { "CUSV00THA", { -1132915.09704,  6092528.52228,  1504633.12884 },  8.0 },
    { "MAC100AUS", { -3464038.98262,  1334173.29737, -5169223.81332 },  8.0 },
    { "NIUM00NIU", { -5937161.02270, -1054674.91356, -2071385.86297 }, 12.0 },
    { "COCO00AUS", {  -741951.26822,  6190961.73679, -1337767.15664 },  8.0 },
    { "REYK00ISL", {  2587383.90543, -1043033.59986,  5716564.19577 },  8.0 },
    { "MIZU00JPN", { -3857171.22964,  3108692.78119,  4004040.11358 },  8.0 },
} };

TEST_F(SifPostSeismicRealTest, EightRosterStationsAgreeWithTheWeeklySinexHorizontally) {
    REQUIRE_REAL_SIF();
    for (const auto& want : kSinex) {
        SCOPED_TRACE(want.id);
        const StationInfo* s = real()->find(want.id);
        ASSERT_NE(s, nullptr);
        ASSERT_EQ(s->id, want.id);
        ASSERT_FALSE(s->psd.empty());

        const ResolvedStationCoord r = resolveStationCoordinate(*s, kGoldenEpoch, true, true);
        ASSERT_TRUE(r.valid);
        const Eigen::Vector3d snx(want.ecef[0], want.ecef[1], want.ecef[2]);
        const Eigen::Vector3d enu =
            Coordinates::ecefToEnu(r.ecef - snx, Coordinates::ecefToLlh(snx));
        EXPECT_LE(std::abs(enu.x()) * 1e3, want.tol_mm) << "east " << enu.x() * 1e3 << " mm";
        EXPECT_LE(std::abs(enu.y()) * 1e3, want.tol_mm) << "north " << enu.y() * 1e3 << " mm";

        // The term itself is millimetres to decimetres (MIZU00JPN 0.16 m); the full
        // relaxation since the events, applied before, was up to 1.64 m.
        const ResolvedStationCoord plain = resolveStationCoordinate(*s, kGoldenEpoch, true, false);
        EXPECT_LT((r.ecef - plain.ecef).norm(), 0.2);
    }
}

TEST_F(SifPostSeismicRealTest, Mizu00jpnTermIsTheChangeSince2020NotTheWholeRelaxation) {
    REQUIRE_REAL_SIF();
    const StationInfo* s = real()->find("MIZU00JPN");
    ASSERT_NE(s, nullptr);
    const double full  = (resolveStationCoordinate(*s, kGoldenEpoch, true, true).ecef
                          - resolveStationCoordinate(*s, kGoldenEpoch, true, false).ecef).norm();
    const double at_ref = (resolveStationCoordinate(*s, 2020.0, true, true).ecef
                           - resolveStationCoordinate(*s, 2020.0, true, false).ecef).norm();
    EXPECT_GT(full, 0.10);
    EXPECT_LT(full, 0.20);
    EXPECT_EQ(at_ref, 0.0);
}

// Captured from the Python twin tools/sif_propagate (test_sif_propagate.py, ORACLE), which
// agrees with this evaluator to 1e-9 m; the pre-fix numbers carried 0.5 m of post-seismic
// displacement a second time.
TEST_F(SifPostSeismicRealTest, Antc00chlMatchesThePythonTwin) {
    REQUIRE_REAL_SIF();
    const StationInfo* s = real()->find("ANTC00CHL");
    ASSERT_NE(s, nullptr);
    struct Row { double t; Eigen::Vector3d full; };
    const std::array<Row, 3> rows = { {
        { 2022.0, { 1608538.527130818, -4816370.562133572, -3847798.263421474 } },
        { 2024.36, { 1608538.529997902, -4816370.603508504, -3847798.252403526 } },
        { 2018.5, { 1608538.535370068, -4816370.498579748, -3847798.285808865 } },
    } };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.t);
        const ResolvedStationCoord r = resolveStationCoordinate(*s, row.t, true, true);
        ASSERT_TRUE(r.valid);
        EXPECT_LT((r.ecef - row.full).norm(), 5e-9) << r.ecef.transpose();
    }
    // 0.66 m accumulated by 2020.0; 69 mm of change since at 2024.36.
    const double term = (resolveStationCoordinate(*s, 2024.36, true, true).ecef
                         - resolveStationCoordinate(*s, 2024.36, true, false).ecef).norm();
    EXPECT_GT(term, 0.05);
    EXPECT_LT(term, 0.09);
}

} // namespace
} // namespace GnssProcessor
