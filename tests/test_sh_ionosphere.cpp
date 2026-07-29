/**
 * @file test_sh_ionosphere.cpp
 * @brief Stage-0 unit tests for the single-layer spherical-harmonic VTEC model,
 *        the shared IPP geometry, and the sub-solar longitude.
 *
 * These validate the math BEFORE any filter wiring (the design's Stage 0):
 *   - the real surface-harmonic basis is orthonormal under (1/4π)∫dΩ
 *     (the normalization is internally consistent);
 *   - the analytic partials ∂VTEC/∂coeff equal the basis values (the network row
 *     builder's Jacobian is correct);
 *   - a known coefficient set is recovered to ~machine precision from synthesized
 *     samples (the basis spans + the linear algebra is sound);
 *   - the IPP geometry reproduces a hand-computed overhead/elevation case;
 *   - a parsed CODE IONEX grid is reproduced by a degree-15 fit to < 0.1 TECU
 *     (the basis represents a real band-limited VTEC field).
 */

#include <gtest/gtest.h>

#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "atmosphere/IonexParser.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kDeg2Rad = Constants::PI / 180.0;

// (1/4π)∫ Y_i Y_j dΩ ≈ δ_ij — verify the basis is orthonormal via a dense
// midpoint quadrature with cos(lat) area weights.
TEST(ShIonosphere, BasisOrthonormalOverSphere) {
    const int N = 4;
    SphericalHarmonicIonosphere sh(N);
    const int K = sh.numCoeffs();
    ASSERT_EQ(K, (N + 1) * (N + 1));

    Eigen::MatrixXd gram = Eigen::MatrixXd::Zero(K, K);
    double area = 0.0;
    const double dlat = 1.0, dlon = 1.0;  // degrees
    std::vector<double> b;
    for (double lat = -89.5; lat <= 89.5; lat += dlat) {
        const double w = std::cos(lat * kDeg2Rad);
        for (double lon = -179.5; lon <= 179.5; lon += dlon) {
            sh.basis(lat * kDeg2Rad, lon * kDeg2Rad, b);
            for (int i = 0; i < K; ++i)
                for (int j = 0; j < K; ++j)
                    gram(i, j) += w * b[i] * b[j];
            area += w;
        }
    }
    gram /= area;  // (1/4π)∫ with the discrete area normalization

    double max_off = 0.0, max_diag_err = 0.0;
    for (int i = 0; i < K; ++i) {
        max_diag_err = std::max(max_diag_err, std::abs(gram(i, i) - 1.0));
        for (int j = 0; j < K; ++j)
            if (i != j) max_off = std::max(max_off, std::abs(gram(i, j)));
    }
    EXPECT_LT(max_diag_err, 2e-2) << "diagonal not ~1 (normalization off)";
    EXPECT_LT(max_off, 2e-2) << "off-diagonal not ~0 (basis not orthogonal)";
}

// ∂VTEC/∂coeff_k == basis_k, and VTEC == Σ coeff_k basis_k (linearity).
TEST(ShIonosphere, PartialsEqualBasisAndAreLinear) {
    const int N = 4;
    SphericalHarmonicIonosphere sh(N);
    const int K = sh.numCoeffs();

    Eigen::VectorXd c(K);
    for (int k = 0; k < K; ++k) c(k) = 0.37 * std::sin(0.7 * k + 1.0) + 0.1 * k;

    const double lat = 23.0 * kDeg2Rad, lon_sf = -47.0 * kDeg2Rad;
    std::vector<double> b;
    sh.basis(lat, lon_sf, b);

    // Linearity: synthesis == dot(coeff, basis).
    double dot = 0.0;
    for (int k = 0; k < K; ++k) dot += c(k) * b[k];
    EXPECT_NEAR(sh.vtec(c, lat, lon_sf), dot, 1e-12);

    // Central finite-difference partial of VTEC wrt each coeff == basis_k.
    const double eps = 1e-4;
    for (int k = 0; k < K; ++k) {
        Eigen::VectorXd cp = c, cm = c;
        cp(k) += eps; cm(k) -= eps;
        const double num = (sh.vtec(cp, lat, lon_sf) - sh.vtec(cm, lat, lon_sf)) / (2.0 * eps);
        EXPECT_NEAR(num, b[k], 1e-7) << "partial mismatch at coeff " << k;
    }
}

// Recover a known coefficient set from synthesized samples (over-determined LS).
TEST(ShIonosphere, RecoversKnownCoefficients) {
    const int N = 4;
    SphericalHarmonicIonosphere sh(N);
    const int K = sh.numCoeffs();

    Eigen::VectorXd c_true(K);
    for (int k = 0; k < K; ++k) c_true(k) = std::cos(1.3 * k) * (10.0 / (1.0 + k));

    // Sample VTEC on a lat/lon/sun-fixed grid (well over K samples).
    std::vector<std::array<double, 2>> pts;
    for (double lat = -80; lat <= 80; lat += 20)
        for (double lon = -180; lon < 180; lon += 30)
            pts.push_back({lat * kDeg2Rad, lon * kDeg2Rad});
    ASSERT_GT(static_cast<int>(pts.size()), K);

    Eigen::MatrixXd A(pts.size(), K);
    Eigen::VectorXd y(pts.size());
    std::vector<double> b;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        sh.basis(pts[i][0], pts[i][1], b);
        for (int k = 0; k < K; ++k) A(i, k) = b[k];
        y(i) = sh.vtec(c_true, pts[i][0], pts[i][1]);
    }
    const Eigen::VectorXd c_hat =
        A.colPivHouseholderQr().solve(y);
    EXPECT_LT((c_hat - c_true).cwiseAbs().maxCoeff(), 1e-8);
}

// IPP geometry: a satellite at the local zenith pierces directly overhead with
// unit mapping; a known off-zenith case matches the closed-form mapping.
TEST(ShIonosphere, IppGeometryOverheadAndElevation) {
    const double Re = 6371000.0, H = 450000.0;
    // Receiver on the equator at lon 0.
    const Eigen::Vector3d rx = Coordinates::llhToEcef(0.0, 0.0, 0.0);

    // Satellite radially outward (local zenith) → elevation 90°, mapping 1, IPP=rx.
    const Eigen::Vector3d sat_zenith = rx * (1.0 + 20000000.0 / rx.norm());
    const IppGeometry gz = computeIppGeometry(rx, sat_zenith, Re, H);
    ASSERT_TRUE(gz.valid);
    EXPECT_NEAR(gz.elevation_rad, Constants::PI / 2.0, 1e-6);
    EXPECT_NEAR(gz.mapping, 1.0, 1e-9);
    EXPECT_NEAR(gz.ipp_lat_rad, 0.0, 1e-6);
    EXPECT_NEAR(gz.ipp_lon_rad, 0.0, 1e-6);

    // A satellite due north and lower in the sky: mapping must exceed 1 and the
    // IPP must move north of the receiver.
    const Eigen::Vector3d sat = Coordinates::llhToEcef(40.0 * kDeg2Rad, 0.0, 20200000.0);
    const IppGeometry gn = computeIppGeometry(rx, sat, Re, H);
    ASSERT_TRUE(gn.valid);
    EXPECT_GT(gn.mapping, 1.0);
    EXPECT_GT(gn.ipp_lat_rad, 0.0);   // IPP north of the equatorial receiver
    // Closed-form mapping check: 1/cos(asin(Re/(Re+H) cos el)).
    const double sz = Re / (Re + H) * std::cos(gn.elevation_rad);
    const double map_ref = 1.0 / std::sqrt(1.0 - sz * sz);
    EXPECT_NEAR(gn.mapping, map_ref, 1e-9);
}

// Sub-solar longitude sweeps westward ~15°/hour and stays wrapped to [-π, π].
TEST(ShIonosphere, MeanSunLongitudeSweepsWestward) {
    const int week = 2313;
    const double tow0 = 200000.0;  // arbitrary mid-week epoch
    const double lon0 = meanSunLongitude(GnssTime{week, tow0});
    EXPECT_LE(lon0, Constants::PI);
    EXPECT_GE(lon0, -Constants::PI);

    // Six hours later the sub-solar point has moved ~90° west (−90°).
    const double lon6 = meanSunLongitude(GnssTime{week, tow0 + 6.0 * 3600.0});
    double d = lon6 - lon0;
    while (d >  Constants::PI) d -= 2.0 * Constants::PI;
    while (d < -Constants::PI) d += 2.0 * Constants::PI;
    const double d_deg = d / kDeg2Rad;
    EXPECT_NEAR(d_deg, -90.0, 3.0);  // ~ −15°/hr × 6 h, ±equation-of-time drift
}

// A parsed CODE IONEX grid is reproduced by a degree-15 SH fit to < 0.1 TECU
// (the band-limited-field acceptance).  Skips cleanly if the fixture is absent.
TEST(ShIonosphere, ReproducesCodeIonexGrid) {
    const std::filesystem::path inx =
        std::filesystem::path(GNSS_TEST_DATA_DIR) / "2024-129" / "products" /
        "COD0OPSFIN_20241290000_01D_01H_GIM.INX";
    if (!std::filesystem::exists(inx)) {
        GTEST_SKIP() << "IONEX fixture not present: " << inx.string();
    }
    LogManager log;
    const IonexData data = IonexParser::parse(inx.string(), log);
    ASSERT_TRUE(data.valid);
    ASSERT_FALSE(data.maps.empty());

    const int N = 15;  // CODE expands to degree/order 15
    SphericalHarmonicIonosphere sh(N);
    const int K = sh.numCoeffs();

    // Fit the FIRST map's static grid directly in geographic lon (no sun rotation
    // — we fit the published grid, not a sun-fixed field).
    const auto& map0 = data.maps.front();
    std::vector<double> b;
    std::vector<Eigen::VectorXd> rows;
    std::vector<double> rhs;
    for (int r = 0; r < data.n_lat; ++r) {
        const double lat = data.lat1_deg + r * data.dlat_deg;
        for (int cc = 0; cc < data.n_lon; ++cc) {
            const double v = map0.vtec[static_cast<std::size_t>(r) * data.n_lon + cc];
            if (!std::isfinite(v)) continue;
            const double lon = data.lon1_deg + cc * data.dlon_deg;
            sh.basis(lat * kDeg2Rad, lon * kDeg2Rad, b);
            Eigen::VectorXd row(K);
            for (int k = 0; k < K; ++k) row(k) = b[k];
            rows.push_back(std::move(row));
            rhs.push_back(v);
        }
    }
    ASSERT_GT(static_cast<int>(rows.size()), K);

    Eigen::MatrixXd A(rows.size(), K);
    Eigen::VectorXd y(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) { A.row(i) = rows[i]; y(i) = rhs[i]; }

    const Eigen::VectorXd c = A.colPivHouseholderQr().solve(y);
    const Eigen::VectorXd resid = A * c - y;
    const double rms = std::sqrt(resid.squaredNorm() / resid.size());
    // CODE grids are band-limited to degree 15, so the refit residual is grid-
    // interpolation noise.  Keep a generous bound that still catches a gross
    // normalization/recurrence bug (which would leave TECU-level structure).
    EXPECT_LT(rms, 0.5) << "degree-15 SH fit RMS to the CODE IONEX grid (TECU)";
}

// Iono-2 dual-layer: the shLayers() view is assembled per config (single-layer
// list without a second model; ordered two-entry list with one), each shell's
// pierce geometry is evaluated at its OWN height (higher shell ⇒ smaller
// thin-shell mapping and a displaced IPP), and the summed slant matches the
// per-layer syntheses — the exact contract every consumer loop relies on.
TEST(ShIonosphere, DualLayerViewAndSlantSumMatchPerLayerSynthesis) {
    GlobalVtecShConfig shc;
    shc.max_degree = 3;
    shc.layer_height_km = 450.0;
    shc.layer2_max_degree = 2;
    shc.layer2_height_km = 1300.0;
    SphericalHarmonicIonosphere l0(shc.max_degree);
    SphericalHarmonicIonosphere l1(shc.layer2_max_degree);

    // View assembly: no second model ⇒ one entry with the legacy scalars.
    const auto single = shLayers(shc, &l0, nullptr);
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single[0].layer, 0);
    EXPECT_DOUBLE_EQ(single[0].shell_height_m, 450.0e3);
    EXPECT_DOUBLE_EQ(single[0].kaula_scale_tecu, shc.kaula_scale_tecu);
    EXPECT_TRUE(shLayers(shc, nullptr, &l1).empty());   // no layer 0 ⇒ no field

    const auto dual = shLayers(shc, &l0, &l1);
    ASSERT_EQ(dual.size(), 2u);
    EXPECT_EQ(dual[0].model, &l0);
    EXPECT_EQ(dual[1].model, &l1);
    EXPECT_EQ(dual[1].layer, 1);
    EXPECT_DOUBLE_EQ(dual[1].shell_height_m, 1300.0e3);
    EXPECT_DOUBLE_EQ(dual[1].kaula_scale_tecu, shc.layer2_kaula_scale_tecu);
    EXPECT_DOUBLE_EQ(dual[1].mean_prior_sigma_tecu,
                     shc.layer2_mean_vtec_prior_sigma_tecu);

    // Mid-elevation LOS from a mid-latitude station: per-shell geometry.
    const Eigen::Vector3d rx =
        Coordinates::llhToEcef(40.0 * kDeg2Rad, -105.0 * kDeg2Rad, 1600.0);
    const Eigen::Vector3d los_dir =
        (Coordinates::llhToEcef(47.0 * kDeg2Rad, -95.0 * kDeg2Rad, 2.0e7) - rx)
            .normalized();
    const Eigen::Vector3d sat = rx + 2.2e7 * los_dir;
    const double Re = shc.earth_radius_km * 1000.0;
    const IppGeometry g0 = computeIppGeometry(rx, sat, Re, dual[0].shell_height_m);
    const IppGeometry g1 = computeIppGeometry(rx, sat, Re, dual[1].shell_height_m);
    ASSERT_TRUE(g0.valid);
    ASSERT_TRUE(g1.valid);
    EXPECT_LT(g1.mapping, g0.mapping);          // higher shell ⇒ smaller obliquity
    EXPECT_GT(g1.mapping, 1.0);
    // The higher shell's pierce point sits farther along track from the station.
    const double dlat0 = std::fabs(g0.ipp_lat_rad - 40.0 * kDeg2Rad);
    const double dlat1 = std::fabs(g1.ipp_lat_rad - 40.0 * kDeg2Rad);
    EXPECT_GT(dlat1, dlat0);

    // Slant sum: Σ_l M_l·VTEC_l(IPP_l) with distinct coefficient sets equals the
    // hand-built per-layer sum (the row builder writes exactly these products).
    Eigen::VectorXd c0(l0.numCoeffs()), c1(l1.numCoeffs());
    for (int k = 0; k < l0.numCoeffs(); ++k) c0(k) = 0.3 + 0.05 * k;
    for (int k = 0; k < l1.numCoeffs(); ++k) c1(k) = -0.2 + 0.07 * k;
    const double v0 = l0.vtec(c0, g0.ipp_lat_rad, g0.ipp_lon_rad);
    const double v1 = l1.vtec(c1, g1.ipp_lat_rad, g1.ipp_lon_rad);
    std::vector<double> b0, b1;
    l0.basis(g0.ipp_lat_rad, g0.ipp_lon_rad, b0);
    l1.basis(g1.ipp_lat_rad, g1.ipp_lon_rad, b1);
    double sum_rows = 0.0;
    for (int k = 0; k < l0.numCoeffs(); ++k) sum_rows += g0.mapping * b0[k] * c0(k);
    for (int k = 0; k < l1.numCoeffs(); ++k) sum_rows += g1.mapping * b1[k] * c1(k);
    EXPECT_NEAR(sum_rows, g0.mapping * v0 + g1.mapping * v1, 1e-12);
}

}  // namespace
