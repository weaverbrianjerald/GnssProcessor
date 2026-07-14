/**
 * @file test_transforms.cpp
 * @brief Unit tests for measurement transforms (BRSD, BSSD).
 *
 * Both transforms are documented as stateless / pure on at least part of
 * their public surface:
 *
 *   - `BrsdTransform::apply`: single static method, no instance state.
 *   - `BssdTransform::differenceBrsd`: static helper, pure function of its
 *     inputs (the instance method `apply` is heavier — it requires a
 *     PivotManager + StateManager — and is not covered here).
 *
 * Tests build minimal `PppObservation` / `BrsdObservation` inputs in-code
 * and check residuals, variance composition, metadata propagation, the
 * cross-attribute aliased-code flag, and the BSSD pairing / pivot-skip
 * rules. Hand-computed numerics throughout.
 */

#include <gtest/gtest.h>

#include "measurement/BrsdTransform.hpp"
#include "measurement/BssdTransform.hpp"
#include "common/PppObservation.hpp"
#include "common/SignalBand.hpp"
#include "common/Constants.hpp"

#include <Eigen/Dense>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// ============================================================================
// Helpers
// ============================================================================

// Build a minimal PppObservation with one observation entry under `code`.
// The fields touched by BrsdTransform::apply are: observed[code],
// modeled[code], variances[code] (optional), satellite, los, elevation, time.
// Everything else stays default-initialized.
PppObservation makeObs(const std::string& satellite,
                       const std::string& code,
                       double observed,
                       double modeled,
                       double variance,
                       const Eigen::Vector3d& los = Eigen::Vector3d(1.0, 0.0, 0.0),
                       double elevation_deg = 45.0) {
    PppObservation obs{};
    obs.satellite = satellite;
    obs.observed[code] = observed;
    obs.modeled[code] = modeled;
    if (variance > 0.0) obs.variances[code] = variance;
    obs.los = los;
    obs.elevation = elevation_deg;
    obs.time = GnssTime{ 2313, 259200.0 };
    return obs;
}

BrsdObservation makeBrsd(const std::string& sat,
                         const std::string& cons,
                         FrequencyBand band,
                         char obs_type,
                         double residual,
                         double var_rov,
                         double var_ref,
                         const std::string& rov_code,
                         const std::string& ref_code,
                         const Eigen::Vector3d& los = Eigen::Vector3d(1.0, 0.0, 0.0)) {
    BrsdObservation b;
    b.satellite = sat;
    b.constellation = cons;
    b.band = band;
    b.obs_type = obs_type;
    b.rov_code = rov_code;
    b.ref_code = ref_code;
    b.brsd_aliased = (rov_code != ref_code);
    b.residual = residual;
    b.var_rov = var_rov;
    b.var_ref = var_ref;
    b.variance = var_rov + var_ref;
    b.los_rov = los;
    return b;
}

// ============================================================================
// BrsdTransform
// ============================================================================

// Canonical case: same RINEX code on both receivers, finite values. Residual
// must be (obs_rov - mod_rov) - (obs_ref - mod_ref). Variance is the sum.
TEST(BrsdTransformTest, ResidualAndVarianceMatchHandComputed) {
    // Rover residual: 100.5 - 100.0 = 0.5
    // Reference residual: 200.3 - 200.1 = 0.2
    // BRSD: 0.5 - 0.2 = 0.3
    PppObservation rov = makeObs("G05", "C1C", 100.5, 100.0, 0.04);
    PppObservation ref = makeObs("G05", "C1C", 200.3, 200.1, 0.09);

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1C",
                                    FrequencyBand::GpsL1, 'C');
    ASSERT_TRUE(out.has_value());

    EXPECT_NEAR(out->residual, 0.3, 1e-12);
    EXPECT_NEAR(out->var_rov, 0.04, 1e-12);
    EXPECT_NEAR(out->var_ref, 0.09, 1e-12);
    EXPECT_NEAR(out->variance, 0.13, 1e-12);

    // Metadata propagated unchanged.
    EXPECT_EQ(out->satellite, "G05");
    EXPECT_EQ(out->constellation, "GPS");
    EXPECT_EQ(out->rov_code, "C1C");
    EXPECT_EQ(out->ref_code, "C1C");
    EXPECT_FALSE(out->brsd_aliased);
    EXPECT_EQ(out->band, FrequencyBand::GpsL1);
    EXPECT_EQ(out->obs_type, 'C');

    // Rover-side LOS preserved (DD baseline mode estimates only the rover).
    EXPECT_DOUBLE_EQ(out->los_rov.x(), 1.0);
    EXPECT_DOUBLE_EQ(out->los_rov.y(), 0.0);
    EXPECT_DOUBLE_EQ(out->los_rov.z(), 0.0);
    EXPECT_DOUBLE_EQ(out->elevation_rov, 45.0);
}

// Cross-attribute aliasing: rover and reference use different RINEX codes
// (e.g. C1C vs C1W). The transform must still produce a result and flag
// the row as `brsd_aliased = true` so downstream AR knows it can't round
// to integer.
TEST(BrsdTransformTest, AliasedCodesFlaggedAndDifferenced) {
    PppObservation rov = makeObs("G05", "C1C", 50.5, 50.0, 0.04);
    PppObservation ref = makeObs("G05", "C1W", 60.3, 60.0, 0.09);

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1W",
                                    FrequencyBand::GpsL1, 'C');
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->brsd_aliased);
    EXPECT_NEAR(out->residual, 0.5 - 0.3, 1e-12);
    EXPECT_NEAR(out->variance, 0.13, 1e-12);
}

// Sign convention: positive residual = rover sees the satellite "later" than
// the reference. Verify by setting up a configuration where rov has a larger
// (observed - modeled) than ref.
TEST(BrsdTransformTest, SignConventionPositiveWhenRoverIsLater) {
    PppObservation rov = makeObs("G05", "C1C", 100.0,  99.0, 0.0);  // residual +1
    PppObservation ref = makeObs("G05", "C1C", 200.0, 199.5, 0.0);  // residual +0.5

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1C",
                                    FrequencyBand::GpsL1, 'C');
    ASSERT_TRUE(out.has_value());
    EXPECT_GT(out->residual, 0.0);
    EXPECT_NEAR(out->residual, 0.5, 1e-12);
}

// Missing observation on the rover side returns nullopt (no silent zero).
TEST(BrsdTransformTest, MissingRoverObservationReturnsNullopt) {
    PppObservation rov;
    rov.satellite = "G05";
    // No observed/modeled entry for "C1C".
    PppObservation ref = makeObs("G05", "C1C", 200.3, 200.1, 0.09);

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1C",
                                    FrequencyBand::GpsL1, 'C');
    EXPECT_FALSE(out.has_value());
}

TEST(BrsdTransformTest, MissingReferenceObservationReturnsNullopt) {
    PppObservation rov = makeObs("G05", "C1C", 100.5, 100.0, 0.04);
    PppObservation ref;
    ref.satellite = "G05";

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1C",
                                    FrequencyBand::GpsL1, 'C');
    EXPECT_FALSE(out.has_value());
}

// Non-finite residual rejected. We construct a NaN difference by giving the
// rover side observed - modeled = NaN (NaN minus anything is NaN).
TEST(BrsdTransformTest, NonFiniteResidualReturnsNullopt) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    PppObservation rov = makeObs("G05", "C1C", kNaN, 0.0, 0.04);
    PppObservation ref = makeObs("G05", "C1C", 200.3, 200.1, 0.09);

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1C",
                                    FrequencyBand::GpsL1, 'C');
    EXPECT_FALSE(out.has_value());
}

// Variance-lookup contract: missing or non-positive variance entries become
// 0.0 in the output, but the row is still emitted (variance positivity is
// the caller's responsibility, not BRSD's).
TEST(BrsdTransformTest, MissingVarianceTreatedAsZero) {
    PppObservation rov = makeObs("G05", "C1C", 100.5, 100.0, /*variance=*/0.0);
    PppObservation ref = makeObs("G05", "C1C", 200.3, 200.1, /*variance=*/0.0);

    auto out = BrsdTransform::apply(rov, ref, "GPS",
                                    "C1C", "C1C",
                                    FrequencyBand::GpsL1, 'C');
    ASSERT_TRUE(out.has_value());
    EXPECT_DOUBLE_EQ(out->var_rov, 0.0);
    EXPECT_DOUBLE_EQ(out->var_ref, 0.0);
    EXPECT_DOUBLE_EQ(out->variance, 0.0);
}

// ============================================================================
// BssdTransform::differenceBrsd
// ============================================================================

// Two-satellite, one-constellation, one-band, one-obs-type setup. With G01
// chosen as the pivot, the only non-pivot is G05, so we expect exactly one
// DD row whose innov_pre = BRSD_G05 - BRSD_G01.
TEST(BssdDifferenceBrsdTest, SingleNonPivotDdRow) {
    auto bi = makeBrsd("G05", "GPS", FrequencyBand::GpsL1, 'C',
                       /*residual=*/0.30, /*var_rov=*/0.04, /*var_ref=*/0.05,
                       /*rov_code=*/"C1C", /*ref_code=*/"C1C",
                       /*los=*/Eigen::Vector3d(0.6, 0.0, 0.8));
    auto bp = makeBrsd("G01", "GPS", FrequencyBand::GpsL1, 'C',
                       /*residual=*/0.10, /*var_rov=*/0.03, /*var_ref=*/0.02,
                       /*rov_code=*/"C1C", /*ref_code=*/"C1C",
                       /*los=*/Eigen::Vector3d(0.0, 0.0, 1.0));

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G05"].push_back(bi);
    brsd_by_cons_sat["GPS"]["G01"].push_back(bp);

    std::map<std::string, std::string> pivots = { { "GPS", "G01" } };

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat, pivots);
    ASSERT_EQ(rows.size(), 1u);
    const auto& r = rows.front();

    EXPECT_EQ(r.obs_type, 'C');
    EXPECT_EQ(r.sat_i, "G05");
    EXPECT_EQ(r.sat_p, "G01");
    EXPECT_EQ(r.cons, "GPS");
    EXPECT_EQ(r.band, FrequencyBand::GpsL1);
    EXPECT_TRUE(r.integer_capable);  // No aliasing on either side, codes match.

    // BSSD: DD = BRSD_i - BRSD_p (residual space).
    EXPECT_NEAR(r.innov_pre, 0.20, 1e-12);
    EXPECT_NEAR(r.var_brsd_i, bi.variance, 1e-12);
    EXPECT_NEAR(r.var_brsd_p, bp.variance, 1e-12);

    // LOS pairs propagate so downstream can build h_pos = -(los_i - los_p).
    EXPECT_TRUE(r.los_rov_i.isApprox(bi.los_rov, 1e-12));
    EXPECT_TRUE(r.los_rov_p.isApprox(bp.los_rov, 1e-12));
}

// The pivot satellite itself never produces a DD row (it would be
// difference-with-itself = 0 and is unobservable).
TEST(BssdDifferenceBrsdTest, PivotSatelliteSkipped) {
    auto bp = makeBrsd("G01", "GPS", FrequencyBand::GpsL1, 'C',
                       0.1, 0.03, 0.02, "C1C", "C1C");

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G01"].push_back(bp);  // Only the pivot.

    std::map<std::string, std::string> pivots = { { "GPS", "G01" } };

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat, pivots);
    EXPECT_TRUE(rows.empty());
}

// integer_capable must be false when any of the four DD legs (rov_i, ref_i,
// rov_p, ref_p) uses a different RINEX code from the others.  Here the
// non-pivot side aliases C1C (rov) vs C1W (ref).
TEST(BssdDifferenceBrsdTest, AliasedNonPivotMarksIntegerIncapable) {
    auto bi = makeBrsd("G05", "GPS", FrequencyBand::GpsL1, 'C',
                       0.30, 0.04, 0.05, "C1C", "C1W");
    auto bp = makeBrsd("G01", "GPS", FrequencyBand::GpsL1, 'C',
                       0.10, 0.03, 0.02, "C1C", "C1C");

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G05"].push_back(bi);
    brsd_by_cons_sat["GPS"]["G01"].push_back(bp);

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat,
                                              { { "GPS", "G01" } });
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows.front().integer_capable);
}

// Pivot present in the pivots map but absent from BRSD data => no rows
// (silent skip is the documented behavior).
TEST(BssdDifferenceBrsdTest, MissingPivotInDataSkipsConstellation) {
    auto bi = makeBrsd("G05", "GPS", FrequencyBand::GpsL1, 'C',
                       0.30, 0.04, 0.05, "C1C", "C1C");

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G05"].push_back(bi);  // No G01 entry.

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat,
                                              { { "GPS", "G01" } });
    EXPECT_TRUE(rows.empty());
}

// (band, obs_type) pairing rule: a non-pivot row only emits a DD row if the
// pivot also has a BRSD entry on the SAME (band, obs_type). A non-pivot code
// row without a matching pivot code row is silently dropped.
TEST(BssdDifferenceBrsdTest, NoMatchingBandObsTypeOnPivotSkipsRow) {
    auto bi = makeBrsd("G05", "GPS", FrequencyBand::GpsL1, 'C',
                       0.30, 0.04, 0.05, "C1C", "C1C");
    // Pivot only has a phase row on L1 -- there is no code-on-L1 pair.
    auto bp = makeBrsd("G01", "GPS", FrequencyBand::GpsL1, 'L',
                       0.10, 0.03, 0.02, "L1C", "L1C");

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G05"].push_back(bi);
    brsd_by_cons_sat["GPS"]["G01"].push_back(bp);

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat,
                                              { { "GPS", "G01" } });
    EXPECT_TRUE(rows.empty());
}

// exclude_keys: callers can suppress specific (sat_i, signal_token) rows
// (the format MeasurementModel writes into em.signal_types: rov_code, or
// "<rov>/<ref>" when aliased).
TEST(BssdDifferenceBrsdTest, ExcludeKeysSuppressesMatchingRow) {
    auto bi = makeBrsd("G05", "GPS", FrequencyBand::GpsL1, 'C',
                       0.30, 0.04, 0.05, "C1C", "C1C");
    auto bp = makeBrsd("G01", "GPS", FrequencyBand::GpsL1, 'C',
                       0.10, 0.03, 0.02, "C1C", "C1C");

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G05"].push_back(bi);
    brsd_by_cons_sat["GPS"]["G01"].push_back(bp);

    // sat_i + "_" + rov_code; non-aliased so signal_token == rov_code.
    std::set<std::string> exclude = { "G05_C1C" };

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat,
                                              { { "GPS", "G01" } },
                                              exclude);
    EXPECT_TRUE(rows.empty());

    // A non-matching exclusion does not suppress the row.
    auto rows_keep = BssdTransform::differenceBrsd(brsd_by_cons_sat,
                                                   { { "GPS", "G01" } },
                                                   { { "G05_C1W" } });
    EXPECT_EQ(rows_keep.size(), 1u);
}

// Exclude format for aliased rows uses "<rov>/<ref>".
TEST(BssdDifferenceBrsdTest, ExcludeKeysAliasedFormat) {
    auto bi = makeBrsd("G05", "GPS", FrequencyBand::GpsL1, 'C',
                       0.30, 0.04, 0.05, "C1C", "C1W");
    auto bp = makeBrsd("G01", "GPS", FrequencyBand::GpsL1, 'C',
                       0.10, 0.03, 0.02, "C1C", "C1C");

    std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>
        brsd_by_cons_sat;
    brsd_by_cons_sat["GPS"]["G05"].push_back(bi);
    brsd_by_cons_sat["GPS"]["G01"].push_back(bp);

    auto rows = BssdTransform::differenceBrsd(brsd_by_cons_sat,
                                              { { "GPS", "G01" } },
                                              { { "G05_C1C/C1W" } });
    EXPECT_TRUE(rows.empty());
}

// H3: nominal band wavelength — the pure static identity the per-λ disc contract
// is built on (deterministic on producer and replay consumer alike).
TEST(SignalBandWavelengthTest, NominalWavelengthPerBand) {
    // Active-network bands resolve to their Constants wavelengths.
    EXPECT_NEAR(bandNominalWavelengthMeters(FrequencyBand::GpsL1), Constants::GPS_L1_WAVELENGTH, 1e-12);
    EXPECT_NEAR(bandNominalWavelengthMeters(FrequencyBand::GpsL2), Constants::GPS_L2_WAVELENGTH, 1e-12);
    EXPECT_NEAR(bandNominalWavelengthMeters(FrequencyBand::GpsL5), Constants::GPS_L5_WAVELENGTH, 1e-12);
    EXPECT_NEAR(bandNominalWavelengthMeters(FrequencyBand::GalE1),  Constants::GAL_E1_WAVELENGTH,  1e-12);
    EXPECT_NEAR(bandNominalWavelengthMeters(FrequencyBand::GalE5a), Constants::GAL_E5A_WAVELENGTH, 1e-12);
    // FDMA GLONASS: the base-frequency (channel k=0) wavelength — the "would-be-FDMA"
    // case the emitter's (sat,signal) API is built to extend later.
    EXPECT_NEAR(bandNominalWavelengthMeters(FrequencyBand::GloG1),
                Constants::SPEED_OF_LIGHT / Constants::GLONASS_G1_BASE_FREQ, 1e-12);
    // Unknown / unmapped bands return the 0.0 sentinel (caller falls back to the flat gate).
    EXPECT_EQ(bandNominalWavelengthMeters(FrequencyBand::Unknown), 0.0);
    EXPECT_EQ(bandNominalWavelengthMeters(FrequencyBand::IrnS), 0.0);
    // Composed with classifySignalForSat: (sat, RINEX code) -> λ, incl a GLONASS sat.
    EXPECT_NEAR(bandNominalWavelengthMeters(classifySignalForSat("G01", "L2W").band),
                Constants::GPS_L2_WAVELENGTH, 1e-12);
    EXPECT_NEAR(bandNominalWavelengthMeters(classifySignalForSat("E11", "L5Q").band),
                Constants::GAL_E5A_WAVELENGTH, 1e-12);
    EXPECT_GT(bandNominalWavelengthMeters(classifySignalForSat("R03", "L1C").band), 0.0);
    // The handoff's 0.25λ sanity numbers.
    EXPECT_NEAR(0.25 * Constants::GPS_L1_WAVELENGTH, 0.0476, 5e-4);
    EXPECT_NEAR(0.25 * Constants::GPS_L2_WAVELENGTH, 0.0610, 5e-4);
    EXPECT_NEAR(0.25 * Constants::GPS_L5_WAVELENGTH, 0.0637, 5e-4);
}

}  // namespace
}  // namespace GnssProcessor
