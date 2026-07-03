/**
 * @file test_station_info.cpp
 * @brief Unit tests for the SIF loader/registry (StationInfoFile).
 *
 * Scope: parsing, indexing/lookup, SI unit normalisation, and the documented
 * SIF -> OceanTideModel frame/phase conversion.  Fixture-backed by the bundled
 * data/static/sif/igs20_SIF.json (skipped, not failed, if absent).
 *
 * Reference values are read off the SIF directly (see scripts / inline numbers)
 * for two stations that ship with the IGS fixture set: CIT100USA (the baseline
 * fixture reference) and ANTC00CHL (carries a post-seismic block).
 */

#include <gtest/gtest.h>

#include "products/StationInfoFile.hpp"
#include "corrections/OceanTideModel.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <memory>
#include <string>

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

// One LogManager + one parsed SIF reused across the suite (spdlog's global
// registry forbids re-registering the logger, and the 8 MB parse is the same
// for every test).
class StationInfoTest : public ::testing::Test {
protected:
    static LogManager log_mgr_;
    static std::unique_ptr<StationInfoFile> sif_;
    static bool have_fixture_;

    static void SetUpTestSuite() {
        namespace fs = std::filesystem;
        const std::string data_dir = GNSS_TEST_DATA_DIR;
        if (data_dir.empty()) return;
        const fs::path sif_path = fs::path(data_dir) / "static" / "sif" / "igs20_SIF.json";
        if (!fs::exists(sif_path)) return;

        // Bare LogManager (no initialize()): getLogger falls back to a transient
        // console logger.  initialize() here would clash with spdlog's global
        // registry when another suite has already initialized in this process.
        sif_ = std::make_unique<StationInfoFile>(log_mgr_);
        sif_->load(sif_path);
        have_fixture_ = !sif_->empty();
    }

    void SetUp() override {
        if (!have_fixture_) GTEST_SKIP() << "igs20_SIF.json fixture not present";
    }
};
LogManager StationInfoTest::log_mgr_;
std::unique_ptr<StationInfoFile> StationInfoTest::sif_;
bool StationInfoTest::have_fixture_ = false;

// The IGS SIF carries hundreds of stations and resolves CIT100USA three ways:
// full ID, 4-char prefix (case-insensitive), and DOMES number.
TEST_F(StationInfoTest, LoadsAndResolvesByMultipleKeys) {
    EXPECT_GT(sif_->size(), 100u);

    const StationInfo* by_id = sif_->find("CIT100USA");
    ASSERT_NE(by_id, nullptr);
    EXPECT_EQ(by_id->id, "CIT100USA");
    EXPECT_EQ(by_id->reference_frame, "ITRF2020");
    EXPECT_NEAR(by_id->reference_epoch_year, 2020.0, 1e-9);

    // 4-char prefix, lower-case -> same record (case-insensitive).
    const StationInfo* by_prefix = sif_->find("cit1");
    ASSERT_NE(by_prefix, nullptr);
    EXPECT_EQ(by_prefix->id, "CIT100USA");

    // DOMES number.
    const StationInfo* by_domes = sif_->find("40400S201");
    ASSERT_NE(by_domes, nullptr);
    EXPECT_EQ(by_domes->id, "CIT100USA");

    EXPECT_EQ(sif_->find("NOSUCHSTATION"), nullptr);
}

// Ocean pole tide: the six Desai coefficients are copied verbatim in the
// N/E/U real/imag order GnssCorrections::opt_coeffs_ expects.
TEST_F(StationInfoTest, OceanPoleTideCoefficients) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->opt.valid);
    EXPECT_NEAR(s->opt.north_real, -0.04651, 1e-4);
    EXPECT_NEAR(s->opt.up_real,    -0.05349, 1e-4);
}

// Ocean tide loading: SIF Up/East/North amplitudes & phases parse into the
// constituent array (M2 first), and the documented conversion to the model's
// Up/West/South lag convention holds (Up passthrough; horizontals keep
// amplitude and shift phase 180 deg).
TEST_F(StationInfoTest, OceanTideLoadingAndFrameConversion) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->has_otl);

    const SifOtlConstituent& m2 = s->otl[0];  // BLQ order: M2 first
    EXPECT_NEAR(m2.up_amp,    0.00460, 1e-4);
    EXPECT_NEAR(m2.up_phase, -53.4,    0.2);
    EXPECT_NEAR(m2.east_amp,  0.00118, 1e-4);
    EXPECT_NEAR(m2.north_amp, 0.00537, 1e-4);

    // The same conversion ModelGeometry applies when feeding the OceanTideModel.
    OtlConstituentCoeffs d;
    d.amp_up    = m2.up_amp;     d.phase_up    = m2.up_phase;
    d.amp_west  = m2.east_amp;   d.phase_west  = m2.east_phase  + 180.0;
    d.amp_south = m2.north_amp;  d.phase_south = m2.north_phase + 180.0;
    EXPECT_DOUBLE_EQ(d.amp_west,  m2.east_amp);
    EXPECT_DOUBLE_EQ(d.amp_south, m2.north_amp);
    EXPECT_DOUBLE_EQ(d.phase_west,  m2.east_phase + 180.0);
    EXPECT_DOUBLE_EQ(d.phase_south, m2.north_phase + 180.0);
}

// Occupation: coordinates pass through in metres; velocity is converted mm/y
// -> m/y; the 6x6 covariance is symmetric with positive diagonal and the
// position-variance diagonal matches the squared coordinate sigma.
TEST_F(StationInfoTest, OccupationCoordinatesVelocityCovariance) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->occupations.empty());

    const SifOccupation& o = s->occupations.front();
    EXPECT_NEAR(o.coordinates.x(), -2491490.58952111, 1e-3);
    EXPECT_NEAR(o.coordinates.y(), -4660802.91919166, 1e-3);
    EXPECT_NEAR(o.coordinates.z(),  3559129.08584545, 1e-3);

    // -28.6339 mm/y -> -0.0286339 m/y.
    EXPECT_NEAR(o.velocity.x(), -0.0286339210265503, 1e-9);
    EXPECT_LT(o.velocity.norm(), 0.1);  // plate motion is cm/y, never > 10 cm/y

    ASSERT_TRUE(o.has_covariance);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            EXPECT_NEAR(o.covariance(i, j), o.covariance(j, i), 1e-18) << i << "," << j;
    for (int i = 0; i < 6; ++i)
        EXPECT_GT(o.covariance(i, i), 0.0) << "diag " << i;
    EXPECT_NEAR(o.covariance(0, 0), 0.0047819 * 0.0047819, 1e-12);  // sigma_X^2 in m^2
}

// Periodic terms: annual/semiannual block parses, PERIOD is preserved, and the
// cos/sin amplitudes are converted mm -> m.
TEST_F(StationInfoTest, PeriodicTermsUnitConversion) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->periodic.empty());

    const SifPeriodicBlock& b = s->periodic.front();
    ASSERT_GE(b.freqs.size(), 2u);          // annual + semiannual
    EXPECT_NEAR(b.freqs[0].period_yr, 1.0, 1e-9);
    EXPECT_NEAR(b.freqs[0].x_cos, 0.463155530400065e-3, 1e-9);  // mm -> m
}

// Antenna history parses with window start in decimal years and verbatim type.
TEST_F(StationInfoTest, AntennaHistory) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->antennas.empty());
    EXPECT_EQ(s->antennas.front().type, "AOAD/M_T        NONE");
    // 1994-09-01 -> ~1994.67.
    EXPECT_NEAR(s->antennas.front().start_year, 1994.666, 0.01);
}

// Coordinate resolution: within a single OCCUPATION window (no periodic/PSD),
// the displacement between two epochs is exactly velocity * dt, which pins both
// the window selection and the linear propagation.  The propagated covariance
// must be present, symmetric, and positive-definite on the diagonal.
TEST_F(StationInfoTest, CoordinateResolutionVelocityWithinWindow) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);

    const SifOccupation* occ = nullptr;
    for (const auto& o : s->occupations)
        if (!o.do_not_use && o.end_year < 9000.0 && (o.end_year - o.start_year) > 0.5) {
            occ = &o;
            break;
        }
    ASSERT_NE(occ, nullptr) << "no finite occupation window found";

    const double t1 = occ->start_year + 0.1;
    const double t2 = occ->start_year + 0.4;
    const ResolvedStationCoord r1 = resolveStationCoordinate(*s, t1, false, false);
    const ResolvedStationCoord r2 = resolveStationCoordinate(*s, t2, false, false);
    ASSERT_TRUE(r1.valid);
    ASSERT_TRUE(r2.valid);

    const Eigen::Vector3d expect = occ->velocity * (t2 - t1);
    EXPECT_TRUE((r2.ecef - r1.ecef).isApprox(expect, 1e-6))
        << "got " << (r2.ecef - r1.ecef).transpose()
        << " expected " << expect.transpose();

    ASSERT_TRUE(r1.has_covariance);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            EXPECT_NEAR(r1.covariance(i, j), r1.covariance(j, i), 1e-18);
    for (int i = 0; i < 3; ++i) EXPECT_GT(r1.covariance(i, i), 0.0);
}

// Periodic (seasonal) terms perturb the position at the millimetre-centimetre
// level: enabling them must change the coordinate, but only slightly.
TEST_F(StationInfoTest, CoordinateResolutionPeriodicIsSmallNonzero) {
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);

    const double year = 2024.36;  // within the IGS fixture span
    const ResolvedStationCoord with    = resolveStationCoordinate(*s, year, true,  false);
    const ResolvedStationCoord without = resolveStationCoordinate(*s, year, false, false);
    ASSERT_TRUE(with.valid);
    ASSERT_TRUE(without.valid);

    const double delta = (with.ecef - without.ecef).norm();
    EXPECT_GT(delta, 0.0);     // periodic terms exist for CIT
    EXPECT_LT(delta, 0.05);    // seasonal amplitude is cm-level, never > 5 cm
}

// RTA-12: the network-roster resolution helper — (registry, name, date) ->
// optional<ECEF + epoch> — is the find() + resolveStationCoordinate composition
// (periodic + PSD terms on) used by the config loader under
// network.use_sif_coordinates.  Any of the three lookup keys works; an unknown
// station returns nullopt (the loader then hard-errors).
TEST_F(StationInfoTest, RosterCoordinateResolvesByNameAtRunDate) {
    const double year = 2024.36;  // within the IGS fixture span

    const auto roster = resolveRosterCoordinate(*sif_, "CIT100USA", year);
    ASSERT_TRUE(roster.has_value());
    EXPECT_DOUBLE_EQ(roster->epoch_year, year);

    // Exactly the full SIF coordinate function at the run date.
    const StationInfo* s = sif_->find("CIT100USA");
    ASSERT_NE(s, nullptr);
    const ResolvedStationCoord direct = resolveStationCoordinate(*s, year, true, true);
    ASSERT_TRUE(direct.valid);
    EXPECT_TRUE(roster->ecef.isApprox(direct.ecef, 1e-12));

    // Velocity propagation actually applied: 4.36 years of ~ -2.9 cm/yr in X.
    const SifOccupation& o = s->occupations.front();
    EXPECT_GT((roster->ecef - o.coordinates).norm(), 0.05);
    EXPECT_LT((roster->ecef - o.coordinates).norm(), 1.0);   // still survey-grade close

    // 4-char prefix resolves to the same record (loader passes config names).
    const auto by_prefix = resolveRosterCoordinate(*sif_, "cit1", year);
    ASSERT_TRUE(by_prefix.has_value());
    EXPECT_TRUE(by_prefix->ecef.isApprox(roster->ecef, 1e-12));

    // Unknown station -> nullopt (never a zero coordinate).
    EXPECT_FALSE(resolveRosterCoordinate(*sif_, "NOSUCHSTATION", year).has_value());
}

// Post-seismic deformation parses for a station with a seismic event (ANTC,
// the 2010 Maule earthquake).
TEST_F(StationInfoTest, PostSeismicDeformation) {
    const StationInfo* s = sif_->find("ANTC00CHL");
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->psd.empty());

    const SifPsdBlock& pb = s->psd.front();
    EXPECT_NEAR(pb.event_year, 2010.157, 0.01);  // 2010-02-27
    ASSERT_GE(pb.east.alog.size(), 2u);
    EXPECT_NEAR(pb.east.alog[0], -0.13792224996689, 1e-9);
}

} // namespace
} // namespace GnssProcessor
