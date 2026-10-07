/**
 * @file test_station_info_guards.cpp
 * @brief Unit tests for the SIF lookup / resolution guards (StationInfoFile).
 *
 * Three defects, one test group each:
 *
 *  - Prefix guard (find): a 4-char prefix match is only a candidate.  It is
 *    accepted when an approximate position lies within
 *    SIF_MAX_PREFIX_DISTANCE_M of the candidate, or, without a position, for a
 *    bare 4-char key only.  Exact ID and DOMES matches are unaffected.
 *  - Extrapolation diagnostics (resolveStationCoordinate): the occupation
 *    selection is unchanged, but the result reports an epoch outside every
 *    non-flagged window, the signed years outside the selected segment, and
 *    antenna (TYPE or serial) / receiver (TYPE) changes inside the gap.
 *  - Per-occupation reference epoch: the linear term and the covariance
 *    propagate from each OCCUPATION's own REFERENCE_EPOCH, falling back to the
 *    METADATA epoch only when the record has none.
 *
 * The bulk runs on SYNTHETIC SIF JSON written to the temp directory, so it is
 * deterministic and independent of the 28 MB bundled files.  A second group
 * replays the field cases on data/static/sif/{igs20,ncn20}_SIF.json (skipped,
 * not failed, if absent).
 */

#include <gtest/gtest.h>

#include "products/StationInfoFile.hpp"
#include "logging/LogManager.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

namespace GnssProcessor {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// --- Synthetic record numbers shared by the JSON writer and the expectations ---

// ABCD00USA: lookup target (approximate coordinates near Pasadena).
const Eigen::Vector3d kAbcdApprox(-2491490.0, -4660803.0, 3559129.0);

// EPOC00USA occupation 1, referred to 2010.0 (as the NCN cors_ex records are).
constexpr double kC1[3]      = { -1981233.59243938, -4990304.17764443, 3431957.6684601 };
constexpr double kV1mm[3]    = { -13.4, 1.2, -6.5 };   // mm/yr
constexpr double kSig1[3]    = { 0.003, 0.004, 0.005 }; // m
constexpr double kVSig1mm[3] = { 0.5, 0.6, 0.7 };       // mm/yr
// 6X6_COVARIANCE_MATRIX off-diagonals, row-major upper triangle, mm^2 family.
constexpr double kCov1[15] = { 1.0, -0.5, 0.2, 0.01, -0.02,
                               0.8, 0.03, 0.25, 0.04,
                               -0.01, 0.02, 0.3,
                               0.05, -0.03,
                               0.06 };
// EPOC00USA occupation 2, referred to 2020.0.
constexpr double kC2[3]   = { -1981233.70, -4990304.10, 3431957.60 };
constexpr double kV2mm[3] = { -14.0, 1.0, -6.0 };

// Field record AZAZ00USA (ncn20_SIF.json), copied from its single OCCUPATION:
// REFERENCE_EPOCH 2010.0 under a METADATA epoch of 2020.0.
constexpr double kAzazC[3]    = { -1981233.59243938, -4990304.17764443, 3431957.6684601 };
constexpr double kAzazVmm[3]  = { -13.4, 1.2, -6.5 };

json vec3(double x, double y, double z) { return json::array({ x, y, z }); }
json vec3(const Eigen::Vector3d& v) { return vec3(v.x(), v.y(), v.z()); }

/// One OCCUPATION record (REFERENCE_EPOCH 2020.0 unless the caller edits it).
json occupation(const std::string& start, const std::string& end,
                const Eigen::Vector3d& xyz, const Eigen::Vector3d& vel_mm,
                bool do_not_use = false) {
    return json{
        { "START", start }, { "END", end },
        { "REFERENCE_FRAME", "ITRF2020" }, { "REFERENCE_EPOCH", 2020.0 },
        { "COORDINATES", vec3(xyz) },
        { "COORDINATE_SIGMAS", vec3(0.002, 0.002, 0.003) },
        { "VELOCITY", vec3(vel_mm) },
        { "VELOCITY_SIGMAS", vec3(0.1, 0.1, 0.2) },
        { "6X6_COVARIANCE_MATRIX", std::vector<double>(15, 0.0) },
        { "DO_NOT_USE", do_not_use },
        { "SOURCE", "synthetic" }
    };
}

json antenna(const std::string& start, const std::string& end,
             const std::string& type, const std::string& serial) {
    return json{ { "START", start }, { "END", end }, { "TYPE", type },
                 { "SERIAL_NUMBER", serial }, { "ARP_MINUS_GRP_ENU", vec3(0.0, 0.0, 0.0) },
                 { "ORIENTATION", 0.0 }, { "DO_NOT_USE", false } };
}

json receiver(const std::string& start, const std::string& end, const std::string& type,
              const std::string& firmware, const std::string& serial) {
    return json{ { "START", start }, { "END", end }, { "TYPE", type },
                 { "FIRMWARE", firmware }, { "SERIAL_NUMBER", serial },
                 { "SATELLITE_SYSTEMS", "GPS" }, { "DO_NOT_USE", false } };
}

json station(const std::string& id, const std::string& domes, const Eigen::Vector3d& approx,
             json occs, json ants = json::array(), json recs = json::array()) {
    return json{ { "ID", id }, { "DOMES", domes }, { "DESCRIPTION", "synthetic" },
                 { "APPROXIMATE_COORDINATES", vec3(approx) },
                 { "ANTENNA", std::move(ants) }, { "RECEIVER", std::move(recs) },
                 { "OCCUPATION", std::move(occs) }, { "NONLINEAR_TERMS", json::object() } };
}

json sifDocument(double meta_epoch, json stations) {
    return json{ { "METADATA", { { "REFERENCE_FRAME", "ITRF2020" },
                                 { "REFERENCE_EPOCH", meta_epoch } } },
                 { "STATIONS", std::move(stations) } };
}

constexpr const char* kOpen = "9999-12-31T00:00:00 GPS";

/// Main synthetic SIF (METADATA epoch 2020.0).
json mainSyntheticSif() {
    const Eigen::Vector3d zero = Eigen::Vector3d::Zero();
    const Eigen::Vector3d vel(-10.0, 20.0, 5.0);
    json st = json::array();

    // Lookup target with usable approximate coordinates.
    st.push_back(station("ABCD00USA", "40400A001", kAbcdApprox,
        json::array({ occupation("2000-01-01T00:00:00 GPS", kOpen,
                                 kAbcdApprox + Eigen::Vector3d(0.5, -0.3, 0.2), vel) })));

    // Reference-position fallbacks: no approximate coordinates.  EFGH: first
    // occupation flagged (its coordinates must NOT be used), second usable.
    st.push_back(station("EFGH00USA", "", zero, json::array({
        occupation("2000-01-01T00:00:00 GPS", "2009-12-31T23:59:59 GPS",
                   Eigen::Vector3d(1100000.0, -4900000.0, 3900000.0), vel, /*dnu=*/true),
        occupation("2010-01-01T00:00:00 GPS", kOpen,
                   Eigen::Vector3d(1200000.0, -4800000.0, 3950000.0), vel) })));
    // IJKL: every occupation flagged, so the first occupation's coordinates.
    st.push_back(station("IJKL00USA", "", zero, json::array({
        occupation("2000-01-01T00:00:00 GPS", kOpen,
                   Eigen::Vector3d(-3000000.0, 4000000.0, 3500000.0), vel, /*dnu=*/true) })));
    // MNOP: nothing usable at all.
    st.push_back(station("MNOP00USA", "", zero, json::array()));

    // Per-occupation epoch: occupation 1 at 2010.0 with a full covariance,
    // occupation 2 at 2020.0 (the bit-identical sibling).
    {
        json o1 = occupation("2000-01-01T00:00:00 GPS", "2010-01-01T00:00:00 GPS",
                             Eigen::Vector3d(kC1[0], kC1[1], kC1[2]),
                             Eigen::Vector3d(kV1mm[0], kV1mm[1], kV1mm[2]));
        o1["REFERENCE_EPOCH"]   = 2010.0;
        o1["COORDINATE_SIGMAS"] = vec3(kSig1[0], kSig1[1], kSig1[2]);
        o1["VELOCITY_SIGMAS"]   = vec3(kVSig1mm[0], kVSig1mm[1], kVSig1mm[2]);
        o1["6X6_COVARIANCE_MATRIX"] = std::vector<double>(std::begin(kCov1), std::end(kCov1));
        json o2 = occupation("2010-01-01T00:00:00 GPS", kOpen,
                             Eigen::Vector3d(kC2[0], kC2[1], kC2[2]),
                             Eigen::Vector3d(kV2mm[0], kV2mm[1], kV2mm[2]));
        st.push_back(station("EPOC00USA", "", Eigen::Vector3d(kC1[0], kC1[1], kC1[2]),
                             json::array({ o1, o2 })));
    }

    // Extrapolation A: closed last segment, antenna MODEL change in the gap,
    // receiver firmware-only update in the gap.
    const Eigen::Vector3d niue(-5937160.928, -1054675.164, -2071385.997);
    st.push_back(station("EXTA00USA", "", niue,
        json::array({
            occupation("2010-01-01T00:00:00 GPS", "2014-12-31T23:59:59 GPS", niue, vel),
            occupation("2015-01-01T00:00:00 GPS", "2020-06-30T23:59:30 GPS", niue, vel) }),
        json::array({
            antenna("2010-01-01T00:00:00 UTC", "2020-09-01T04:38:59 UTC", "TRM59800.00     NONE", "5347"),
            antenna("2020-09-01T04:39:00 UTC", "9999-12-31T00:00:00 UTC", "JAVRINGANT_DM   NONE", "02116") }),
        json::array({
            receiver("2010-01-01T00:00:00 UTC", "2020-07-31T23:59:59 UTC", "SEPT POLARX5", "5.3.2", "3025338"),
            receiver("2020-08-01T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "SEPT POLARX5", "5.5.0", "3025338") })));

    // Extrapolation B: record-only antenna re-log in the gap (same TYPE, same
    // serial; a trailing blank on the re-logged TYPE exercises the trim), and a
    // receiver TYPE change 16 min BEFORE the segment END (inside the 2-day
    // tolerance, as at CHUR00CAN).
    st.push_back(station("EXTB00USA", "", niue,
        json::array({ occupation("2012-01-01T00:00:00 GPS", "2020-02-13T23:59:30 GPS", niue, vel) }),
        json::array({
            antenna("2012-01-01T00:00:00 UTC", "2020-04-14T23:59:59 UTC", "ASH700936D_M    SCIS", "CR14107"),
            antenna("2020-04-15T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "ASH700936D_M    SCIS ", "CR14107") }),
        json::array({
            receiver("2012-01-01T00:00:00 UTC", "2020-02-13T23:42:59 UTC", "TPS NET-G3A", "4.7", "618-00844"),
            receiver("2020-02-13T23:43:00 UTC", "9999-12-31T00:00:00 UTC", "SEPT POLARX5", "5.3.2", "3022918") })));

    // Extrapolation C: antenna SERIAL-only change in the gap (counts); receiver
    // TYPE change 4 days before the segment END (outside the tolerance).
    st.push_back(station("EXTC00USA", "", niue,
        json::array({ occupation("2012-01-01T00:00:00 GPS", "2020-02-13T23:59:30 GPS", niue, vel) }),
        json::array({
            antenna("2012-01-01T00:00:00 UTC", "2020-04-30T23:59:59 UTC", "TRM59800.00     NONE", "1111"),
            antenna("2020-05-01T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "TRM59800.00     NONE", "2222") }),
        json::array({
            receiver("2012-01-01T00:00:00 UTC", "2020-02-09T23:59:59 UTC", "TRIMBLE NETR9", "5.44", "5338K45574"),
            receiver("2020-02-10T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "SEPT POLARX5", "5.3.2", "3025338") })));

    // Extrapolation D: RADOME-only antenna change in the gap (whole TYPE
    // counts); receiver SERIAL-only change in the gap (does not count).
    st.push_back(station("EXTD00USA", "", niue,
        json::array({ occupation("2012-01-01T00:00:00 GPS", "2020-02-13T23:59:30 GPS", niue, vel) }),
        json::array({
            antenna("2012-01-01T00:00:00 UTC", "2020-04-30T23:59:59 UTC", "TRM59800.00     NONE", "1111"),
            antenna("2020-05-01T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "TRM59800.00     SCIS", "1111") }),
        json::array({
            receiver("2012-01-01T00:00:00 UTC", "2020-05-31T23:59:59 UTC", "SEPT POLARX5", "5.5.0", "3025338"),
            receiver("2020-06-01T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "SEPT POLARX5", "5.5.0", "3099999") })));

    // Extrapolation E: epoch BEFORE the first segment, with an antenna model
    // change one day before the segment START (inside [t, start + tol]).
    st.push_back(station("EXTE00USA", "", niue,
        json::array({ occupation("2010-01-01T00:00:00 GPS", kOpen, niue, vel) }),
        json::array({
            antenna("2008-01-01T00:00:00 UTC", "2009-12-30T23:59:59 UTC", "AOAD/M_T        NONE", "387"),
            antenna("2009-12-31T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "TRM59800.00     NONE", "1234") }),
        json::array({
            receiver("2008-01-01T00:00:00 UTC", "9999-12-31T00:00:00 UTC", "TRIMBLE NETRS", "1.15", "4520250855") })));

    return sifDocument(2020.0, std::move(st));
}

/// Second synthetic SIF with a METADATA epoch of 2015.0: one occupation without
/// REFERENCE_EPOCH, one with a non-numeric REFERENCE_EPOCH.
json metadataFallbackSif() {
    const Eigen::Vector3d p(4000000.0, 1000000.0, 4800000.0);
    const Eigen::Vector3d vel(-10.0, 20.0, 5.0);
    json missing = occupation("2000-01-01T00:00:00 GPS", kOpen, p, vel);
    missing.erase("REFERENCE_EPOCH");
    json text = occupation("2000-01-01T00:00:00 GPS", kOpen, p, vel);
    text["REFERENCE_EPOCH"] = "2010.0";
    return sifDocument(2015.0, json::array({ station("EPOD00DEU", "", p, json::array({ missing })),
                                             station("EPOE00DEU", "", p, json::array({ text })) }));
}

// One LogManager + both registries reused across the suite.  Bare LogManager
// (no initialize()): getLogger falls back to a transient console logger, and
// initialize() would clash with spdlog's global registry when another suite has
// already initialized in this process.
class StationInfoGuardsTest : public ::testing::Test {
protected:
    static LogManager log_mgr_;
    static std::unique_ptr<StationInfoFile> syn_;   ///< Synthetic registry (always present).
    static std::unique_ptr<StationInfoFile> real_;  ///< igs20 + ncn20, loaded on first use.
    static bool real_tried_;
    static std::vector<fs::path> temp_files_;

    static void SetUpTestSuite() {
        syn_ = std::make_unique<StationInfoFile>(log_mgr_);
        const std::array<std::pair<const char*, json>, 2> docs = { {
            { "gnss_station_info_guards_main.json", mainSyntheticSif() },
            { "gnss_station_info_guards_meta2015.json", metadataFallbackSif() } } };
        for (const auto& [name, doc] : docs) {
            const fs::path p = fs::temp_directory_path() / name;
            std::ofstream(p) << doc.dump(1);
            temp_files_.push_back(p);
            syn_->load(p);
        }
    }

    static void TearDownTestSuite() {
        std::error_code ec;
        for (const auto& p : temp_files_) fs::remove(p, ec);
        temp_files_.clear();
        syn_.reset();
        real_.reset();
        real_tried_ = false;
    }

    /// Both bundled SIFs (IGS first, as auto-discovery orders them), or nullptr
    /// when either is absent.
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
LogManager StationInfoGuardsTest::log_mgr_;
std::unique_ptr<StationInfoFile> StationInfoGuardsTest::syn_;
std::unique_ptr<StationInfoFile> StationInfoGuardsTest::real_;
bool StationInfoGuardsTest::real_tried_ = false;
std::vector<fs::path> StationInfoGuardsTest::temp_files_;

#define REQUIRE_REAL_SIF(reg)                                                   \
    const StationInfoFile* reg = real();                                        \
    if (reg == nullptr) GTEST_SKIP() << "igs20/ncn20 SIF fixtures not present"

// =============================================================================
// Prefix guard
// =============================================================================

// A 9-char key that is neither an ID nor a DOMES only prefix-matches; without a
// position it carries conflicting information (a different 5..9) and is refused.
TEST_F(StationInfoGuardsTest, PrefixRejectedWithoutPositionForLongKey) {
    ASSERT_EQ(syn_->size(), 12u);
    EXPECT_EQ(syn_->find("ABCD00PYF"), nullptr);
    EXPECT_EQ(syn_->find("abcd00pyf"), nullptr);
    EXPECT_EQ(syn_->find("ABCDE"), nullptr);
    // An unusable position (zero vector of a missing APPROX header, NaN) is the
    // same as no position.
    const Eigen::Vector3d zero = Eigen::Vector3d::Zero();
    const Eigen::Vector3d nan(std::nan(""), 0.0, 6.4e6);
    EXPECT_EQ(syn_->find("ABCD00PYF", &zero), nullptr);
    EXPECT_EQ(syn_->find("ABCD00PYF", &nan), nullptr);
}

// A bare 4-char code (any case, trimmed) resolves without a position.
TEST_F(StationInfoGuardsTest, PrefixAcceptedForBare4CharKeyWithoutPosition) {
    for (const char* key : { "ABCD", "abcd", "  aBcD \t" }) {
        const StationInfo* s = syn_->find(key);
        ASSERT_NE(s, nullptr) << key;
        EXPECT_EQ(s->id, "ABCD00USA");
    }
    const Eigen::Vector3d zero = Eigen::Vector3d::Zero();
    ASSERT_NE(syn_->find("abcd", &zero), nullptr);  // unusable position == none
    EXPECT_EQ(syn_->find("abcd", &zero)->id, "ABCD00USA");
    EXPECT_EQ(syn_->find("abc"), nullptr);           // shorter than a code
}

// With a usable position the candidate is accepted iff it lies within the radius
// (inclusive), for long and bare keys alike; the radius is a parameter.
TEST_F(StationInfoGuardsTest, PrefixAcceptedWithin10kmRejectedBeyond) {
    const Eigen::Vector3d near5km   = kAbcdApprox + Eigen::Vector3d(5000.0, 0.0, 0.0);
    const Eigen::Vector3d at10km    = kAbcdApprox + Eigen::Vector3d(10000.0, 0.0, 0.0);
    const Eigen::Vector3d past10km  = kAbcdApprox + Eigen::Vector3d(10000.5, 0.0, 0.0);
    const Eigen::Vector3d far       = Eigen::Vector3d(-4147127.4937, -4152221.6682, -2490032.7642);

    ASSERT_NE(syn_->find("ABCD00PYF", &near5km), nullptr);
    EXPECT_EQ(syn_->find("ABCD00PYF", &near5km)->id, "ABCD00USA");
    ASSERT_NE(syn_->find("ABCD00PYF", &at10km), nullptr);  // boundary is inclusive
    EXPECT_EQ(syn_->find("ABCD00PYF", &past10km), nullptr);
    EXPECT_EQ(syn_->find("ABCD00PYF", &far), nullptr);

    // A bare code is checked against a supplied position too.
    ASSERT_NE(syn_->find("abcd", &near5km), nullptr);
    EXPECT_EQ(syn_->find("abcd", &far), nullptr);

    // Caller-supplied radius.
    EXPECT_EQ(syn_->find("ABCD00PYF", &near5km, 1000.0), nullptr);
    EXPECT_NE(syn_->find("ABCD00PYF", &past10km, 20000.0), nullptr);
    EXPECT_DOUBLE_EQ(SIF_MAX_PREFIX_DISTANCE_M, 10000.0);
}

// The candidate's reference position: approximate coordinates, else the first
// NON-FLAGGED occupation, else the first occupation; nothing usable -> reject.
TEST_F(StationInfoGuardsTest, PrefixReferencePositionFallbacks) {
    const Eigen::Vector3d efgh_flagged(1100000.0, -4900000.0, 3900000.0);
    const Eigen::Vector3d efgh_usable(1200000.0, -4800000.0, 3950000.0);
    const Eigen::Vector3d off(100.0, 0.0, 0.0);

    const Eigen::Vector3d near_usable  = efgh_usable + off;
    const Eigen::Vector3d near_flagged = efgh_flagged + off;
    ASSERT_NE(syn_->find("EFGH00XXX", &near_usable), nullptr);
    EXPECT_EQ(syn_->find("EFGH00XXX", &near_usable)->id, "EFGH00USA");
    EXPECT_EQ(syn_->find("EFGH00XXX", &near_flagged), nullptr);

    const Eigen::Vector3d near_ijkl = Eigen::Vector3d(-3000000.0, 4000000.0, 3500000.0) + off;
    ASSERT_NE(syn_->find("IJKL00XXX", &near_ijkl), nullptr);
    EXPECT_EQ(syn_->find("IJKL00XXX", &near_ijkl)->id, "IJKL00USA");

    EXPECT_EQ(syn_->find("MNOP00XXX", &near_ijkl), nullptr);  // no reference position
    EXPECT_NE(syn_->find("MNOP"), nullptr);                   // bare code, no position
    EXPECT_NE(syn_->find("MNOP00USA", &near_ijkl), nullptr);  // exact ID
}

// Exact ID and DOMES matches are authoritative whatever the position says.
TEST_F(StationInfoGuardsTest, ExactIdAndDomesIgnorePosition) {
    const Eigen::Vector3d far(0.0, 0.0, 6.4e6);
    const StationInfo* by_id = syn_->find("abcd00usa", &far);
    ASSERT_NE(by_id, nullptr);
    EXPECT_EQ(by_id->id, "ABCD00USA");
    const StationInfo* by_domes = syn_->find("40400a001", &far);
    ASSERT_NE(by_domes, nullptr);
    EXPECT_EQ(by_domes->id, "ABCD00USA");
    EXPECT_EQ(syn_->find("ABCD00USA"), by_id);
}

// =============================================================================
// Per-occupation reference epoch
// =============================================================================

// The 2010.0 occupation propagates from 2010.0 (linear term and covariance); the
// 2020.0 sibling is bit-identical to the pre-fix t - 2020.0 arithmetic.
TEST_F(StationInfoGuardsTest, PerOccupationReferenceEpoch) {
    const StationInfo* s = syn_->find("EPOC00USA");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->occupations.size(), 2u);
    EXPECT_DOUBLE_EQ(s->reference_epoch_year, 2020.0);            // METADATA
    EXPECT_DOUBLE_EQ(s->occupations[0].reference_epoch_year, 2010.0);
    EXPECT_DOUBLE_EQ(s->occupations[1].reference_epoch_year, 2020.0);

    // Occupation 1 at 2005.0: x = C1 + v1 * (2005 - 2010), computed from the raw
    // numbers written to the JSON.
    const double t1 = 2005.0;
    const ResolvedStationCoord r1 = resolveStationCoordinate(*s, t1, false, false);
    ASSERT_TRUE(r1.valid);
    EXPECT_FALSE(r1.extrapolated);
    const double dt1 = t1 - 2010.0;
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(r1.ecef[k], kC1[k] + kV1mm[k] * 1.0e-3 * dt1, 1e-6) << "axis " << k;
    // Not the METADATA-epoch answer: that one is off by v1 * 10 yr (>= 1.2 cm).
    const Eigen::Vector3d v1(kV1mm[0] * 1e-3, kV1mm[1] * 1e-3, kV1mm[2] * 1e-3);
    const Eigen::Vector3d meta_answer =
        Eigen::Vector3d(kC1[0], kC1[1], kC1[2]) + v1 * (t1 - 2020.0);
    EXPECT_NEAR((r1.ecef - meta_answer).norm(), (v1 * 10.0).norm(), 1e-6);

    // Covariance over the same dt: Cxx + dt (Cxv + Cxv^T) + dt^2 Cvv, with the
    // 6x6 rebuilt independently (diag from sigmas, off-diag mm^2 -> m^2).
    Eigen::Matrix<double, 6, 6> C = Eigen::Matrix<double, 6, 6>::Zero();
    for (int k = 0; k < 3; ++k) {
        C(k, k)         = kSig1[k] * kSig1[k];
        C(k + 3, k + 3) = (kVSig1mm[k] * 1e-3) * (kVSig1mm[k] * 1e-3);
    }
    int n = 0;
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j) { C(i, j) = C(j, i) = 1e-6 * kCov1[n++]; }
    const Eigen::Matrix3d Cxx = C.topLeftCorner<3, 3>();
    const Eigen::Matrix3d Cxv = C.topRightCorner<3, 3>();
    const Eigen::Matrix3d Cvv = C.bottomRightCorner<3, 3>();
    const Eigen::Matrix3d expect = Cxx + dt1 * (Cxv + Cxv.transpose()) + (dt1 * dt1) * Cvv;
    ASSERT_TRUE(r1.has_covariance);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            EXPECT_NEAR(r1.covariance(i, j), expect(i, j), 1e-15) << i << "," << j;
    const double dtm = t1 - 2020.0;
    const Eigen::Matrix3d meta_cov = Cxx + dtm * (Cxv + Cxv.transpose()) + (dtm * dtm) * Cvv;
    EXPECT_GT((meta_cov - r1.covariance).cwiseAbs().maxCoeff(), 1e-6);

    // Occupation 2 at 2024.0: exactly the old expression, t - 2020.0.
    const double t2 = 2024.0;
    const ResolvedStationCoord r2 = resolveStationCoordinate(*s, t2, false, false);
    ASSERT_TRUE(r2.valid);
    const SifOccupation& o2 = s->occupations[1];
    const Eigen::Vector3d old_expr = o2.coordinates + o2.velocity * (t2 - 2020.0);
    for (int k = 0; k < 3; ++k) EXPECT_EQ(r2.ecef[k], old_expr[k]) << "axis " << k;
}

// No numeric REFERENCE_EPOCH on the record (absent or a string): the METADATA
// epoch of ITS file (2015.0 here), not a hard-coded 2020.0.
TEST_F(StationInfoGuardsTest, MissingOccupationEpochFallsBackToMetadata) {
    for (const char* id : { "EPOD00DEU", "EPOE00DEU" }) {
        const StationInfo* s = syn_->find(id);
        ASSERT_NE(s, nullptr) << id;
        EXPECT_DOUBLE_EQ(s->reference_epoch_year, 2015.0) << id;
        ASSERT_EQ(s->occupations.size(), 1u);
        const SifOccupation& o = s->occupations.front();
        EXPECT_DOUBLE_EQ(o.reference_epoch_year, 2015.0) << id;
        const ResolvedStationCoord r = resolveStationCoordinate(*s, 2024.0, false, false);
        ASSERT_TRUE(r.valid);
        const Eigen::Vector3d expect = o.coordinates + o.velocity * (2024.0 - 2015.0);
        EXPECT_TRUE(r.ecef.isApprox(expect, 1e-15)) << id;
    }
}

// =============================================================================
// Extrapolation diagnostics
// =============================================================================

// After a closed last segment with an antenna model change in the gap: flagged,
// years exact, receiver firmware-only update not counted.
TEST_F(StationInfoGuardsTest, ExtrapolationAfterClosedSegmentWithModelChange) {
    const StationInfo* s = syn_->find("EXTA00USA");
    ASSERT_NE(s, nullptr);
    const double t = 2021.0;
    const ResolvedStationCoord r = resolveStationCoordinate(*s, t);
    ASSERT_TRUE(r.valid);
    EXPECT_TRUE(r.extrapolated);
    // END "2020-06-30T23:59:30": day-of-year index 181 in a leap year.
    const double end_year = 2020.0 + (181.0 + 86370.0 / 86400.0) / 366.0;
    EXPECT_NEAR(r.segment_end_year, end_year, 1e-12);
    EXPECT_DOUBLE_EQ(r.segment_start_year, 2015.0);
    EXPECT_DOUBLE_EQ(r.extrapolation_years, t - r.segment_end_year);
    EXPECT_NEAR(r.extrapolation_years, t - end_year, 1e-12);
    EXPECT_TRUE(r.antenna_change_in_gap);
    EXPECT_FALSE(r.receiver_change_in_gap);

    // The coordinate is unchanged by the diagnostics: the selected occupation
    // propagated linearly (no nonlinear terms on this record).
    const SifOccupation& o = s->occupations[1];
    EXPECT_TRUE(r.ecef.isApprox(o.coordinates + o.velocity * (t - 2020.0), 1e-15));
}

// A covered epoch reports nothing (the segment bounds are still filled).
TEST_F(StationInfoGuardsTest, CoveredEpochReportsNoExtrapolation) {
    const StationInfo* s = syn_->find("EXTA00USA");
    ASSERT_NE(s, nullptr);
    const ResolvedStationCoord r = resolveStationCoordinate(*s, 2012.0);
    ASSERT_TRUE(r.valid);
    EXPECT_FALSE(r.extrapolated);
    EXPECT_EQ(r.extrapolation_years, 0.0);
    EXPECT_FALSE(r.antenna_change_in_gap);
    EXPECT_FALSE(r.receiver_change_in_gap);
    EXPECT_DOUBLE_EQ(r.segment_start_year, 2010.0);
    EXPECT_GT(r.segment_end_year, 2014.99);
    EXPECT_LT(r.segment_end_year, 2015.0);

    // The open-ended segment of EXTE covers any later epoch.
    const ResolvedStationCoord open = resolveStationCoordinate(*syn_->find("EXTE00USA"), 2030.0);
    ASSERT_TRUE(open.valid);
    EXPECT_FALSE(open.extrapolated);
    EXPECT_DOUBLE_EQ(open.segment_end_year, 9999.0);
}

// A record-only antenna re-log (same TYPE after trimming, same serial) does not
// count; a receiver TYPE change just before the segment END does (2-day
// tolerance).
TEST_F(StationInfoGuardsTest, RecordOnlyAntennaRelogVsReceiverTypeChange) {
    const ResolvedStationCoord r = resolveStationCoordinate(*syn_->find("EXTB00USA"), 2021.0);
    ASSERT_TRUE(r.valid);
    EXPECT_TRUE(r.extrapolated);
    EXPECT_FALSE(r.antenna_change_in_gap);
    EXPECT_TRUE(r.receiver_change_in_gap);
}

// An antenna serial-only change counts; a receiver TYPE change 4 days before the
// segment END lies outside the 2-day tolerance and does not.
TEST_F(StationInfoGuardsTest, AntennaSerialChangeAndToleranceEdge) {
    const ResolvedStationCoord r = resolveStationCoordinate(*syn_->find("EXTC00USA"), 2021.0);
    ASSERT_TRUE(r.valid);
    EXPECT_TRUE(r.extrapolated);
    EXPECT_TRUE(r.antenna_change_in_gap);
    EXPECT_FALSE(r.receiver_change_in_gap);
    EXPECT_DOUBLE_EQ(SIF_GAP_CHANGE_TOLERANCE_YR, 2.0 / 365.25);
}

// A radome-only change is a TYPE change; a receiver serial-only update is not.
TEST_F(StationInfoGuardsTest, RadomeChangeCountsReceiverSerialDoesNot) {
    const ResolvedStationCoord r = resolveStationCoordinate(*syn_->find("EXTD00USA"), 2021.0);
    ASSERT_TRUE(r.valid);
    EXPECT_TRUE(r.extrapolated);
    EXPECT_TRUE(r.antenna_change_in_gap);
    EXPECT_FALSE(r.receiver_change_in_gap);
}

// An epoch before the first segment: negative years, gap [t, start + tol].
TEST_F(StationInfoGuardsTest, EpochBeforeFirstSegment) {
    const StationInfo* s = syn_->find("EXTE00USA");
    ASSERT_NE(s, nullptr);
    const ResolvedStationCoord r = resolveStationCoordinate(*s, 2009.5);
    ASSERT_TRUE(r.valid);
    EXPECT_TRUE(r.extrapolated);
    EXPECT_DOUBLE_EQ(r.segment_start_year, 2010.0);
    EXPECT_DOUBLE_EQ(r.extrapolation_years, -0.5);
    EXPECT_TRUE(r.antenna_change_in_gap);   // 2009-12-31 lies in [2009.5, 2010 + tol]
    EXPECT_FALSE(r.receiver_change_in_gap); // one receiver window only

    // The first window in start order never counts as a change by itself.
    const ResolvedStationCoord early = resolveStationCoordinate(*syn_->find("EXTA00USA"), 2009.5);
    ASSERT_TRUE(early.valid);
    EXPECT_TRUE(early.extrapolated);
    EXPECT_DOUBLE_EQ(early.extrapolation_years, -0.5);
    EXPECT_FALSE(early.antenna_change_in_gap);
    EXPECT_FALSE(early.receiver_change_in_gap);
}

// =============================================================================
// Network roster resolution
// =============================================================================

// The roster has no position: a 9-char name that only prefix-matches is refused
// (never a borrowed coordinate), a bare code resolves, and the extrapolation
// diagnostics are carried through unchanged.
TEST_F(StationInfoGuardsTest, RosterRejectsPrefixOnlyNameAndCarriesFlags) {
    EXPECT_FALSE(resolveRosterCoordinate(*syn_, "ABCD00PYF", 2021.0).has_value());

    const auto bare = resolveRosterCoordinate(*syn_, "abcd", 2021.0);
    ASSERT_TRUE(bare.has_value());
    EXPECT_FALSE(bare->extrapolated);

    const auto rc = resolveRosterCoordinate(*syn_, "EXTA00USA", 2021.0);
    ASSERT_TRUE(rc.has_value());
    const ResolvedStationCoord direct = resolveStationCoordinate(*syn_->find("EXTA00USA"), 2021.0);
    EXPECT_TRUE(rc->extrapolated);
    EXPECT_EQ(rc->extrapolation_years, direct.extrapolation_years);
    EXPECT_EQ(rc->segment_start_year, direct.segment_start_year);
    EXPECT_EQ(rc->segment_end_year, direct.segment_end_year);
    EXPECT_TRUE(rc->antenna_change_in_gap);
    EXPECT_FALSE(rc->receiver_change_in_gap);
    for (int k = 0; k < 3; ++k) EXPECT_EQ(rc->ecef[k], direct.ecef[k]);
    EXPECT_DOUBLE_EQ(rc->epoch_year, 2021.0);
}

// =============================================================================
// Field cases on the bundled IGS + NCN SIFs
// =============================================================================

// GAMB00PYF (Gambier) is in neither SIF; GAMB00USA is, 7,688 km away.  Refused
// with no position and with the true Gambier position; accepted only next to
// GAMB00USA itself.
TEST_F(StationInfoGuardsTest, RealGambierPrefixNotBorrowed) {
    REQUIRE_REAL_SIF(reg);
    EXPECT_EQ(reg->find("GAMB00PYF"), nullptr);
    const Eigen::Vector3d gambier(-4147127.4937, -4152221.6682, -2490032.7642);
    EXPECT_EQ(reg->find("GAMB00PYF", &gambier), nullptr);
    const StationInfo* usa = reg->find("GAMB00USA");
    ASSERT_NE(usa, nullptr);
    EXPECT_GT((usa->approximate_coordinates - gambier).norm(), 7.6e6);
    EXPECT_EQ(reg->find("GAMB00PYF", &usa->approximate_coordinates), usa);
}

// A legitimate rename (STHL00GBR -> STHL00SHN, same monument) still resolves
// when the caller's position confirms it.
TEST_F(StationInfoGuardsTest, RealStHelenaRenameResolvesWithPosition) {
    REQUIRE_REAL_SIF(reg);
    EXPECT_EQ(reg->find("STHL00GBR"), nullptr);
    const StationInfo* shn = reg->find("STHL00SHN");
    ASSERT_NE(shn, nullptr);
    const StationInfo* s = reg->find("STHL00GBR", &shn->approximate_coordinates);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->id, "STHL00SHN");
    // Bare codes still resolve without a position.
    ASSERT_NE(reg->find("P389"), nullptr);
    EXPECT_EQ(reg->find("P389")->id, "P38900USA");
    ASSERT_NE(reg->find("cit1"), nullptr);
    EXPECT_EQ(reg->find("cit1")->id, "CIT100USA");
}

// AZAZ00USA's occupation is referred to 2010.0.  The record is flagged
// DO_NOT_USE, so the unchanged selection rule leaves it unresolved; with the flag
// cleared on a copy, the coordinate is x + v (2024.35 - 2010.0) from the JSON
// numbers (no nonlinear terms on this record).
TEST_F(StationInfoGuardsTest, RealAzazPropagatesFromItsOwnEpoch) {
    REQUIRE_REAL_SIF(reg);
    const StationInfo* s = reg->find("AZAZ00USA");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->occupations.size(), 1u);
    EXPECT_DOUBLE_EQ(s->reference_epoch_year, 2020.0);
    EXPECT_DOUBLE_EQ(s->occupations.front().reference_epoch_year, 2010.0);
    EXPECT_TRUE(s->occupations.front().do_not_use);
    EXPECT_FALSE(resolveStationCoordinate(*s, 2024.35).valid);

    StationInfo usable = *s;
    usable.occupations.front().do_not_use = false;
    const ResolvedStationCoord r = resolveStationCoordinate(usable, 2024.35);
    ASSERT_TRUE(r.valid);
    EXPECT_FALSE(r.extrapolated);
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(r.ecef[k], kAzazC[k] + kAzazVmm[k] * 1.0e-3 * (2024.35 - 2010.0), 1e-6)
            << "axis " << k;
}

// NIUM00NIU: last segment ends 2024-01-07, antenna model change on 2024-02-27.
TEST_F(StationInfoGuardsTest, RealNiueExtrapolatedAcrossAntennaChange) {
    REQUIRE_REAL_SIF(reg);
    const StationInfo* s = reg->find("NIUM00NIU");
    ASSERT_NE(s, nullptr);
    const ResolvedStationCoord r = resolveStationCoordinate(*s, 2024.35);
    ASSERT_TRUE(r.valid);
    EXPECT_TRUE(r.extrapolated);
    // END "2024-01-07T23:59:30": day-of-year index 6 in a leap year.
    EXPECT_NEAR(r.segment_end_year, 2024.0 + (6.0 + 86370.0 / 86400.0) / 366.0, 1e-12);
    EXPECT_DOUBLE_EQ(r.extrapolation_years, 2024.35 - r.segment_end_year);
    EXPECT_TRUE(r.antenna_change_in_gap);
    EXPECT_FALSE(r.receiver_change_in_gap);

    // Covered in 2010: nothing reported.
    const ResolvedStationCoord c = resolveStationCoordinate(*s, 2010.0);
    ASSERT_TRUE(c.valid);
    EXPECT_FALSE(c.extrapolated);
    EXPECT_FALSE(c.antenna_change_in_gap);
}

#undef REQUIRE_REAL_SIF

} // namespace
} // namespace GnssProcessor
