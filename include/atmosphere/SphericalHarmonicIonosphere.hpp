#pragma once

/**
 * @file SphericalHarmonicIonosphere.hpp
 * @brief Single-layer spherical-harmonic VTEC model + shared IPP geometry, for
 *        the co-estimated global ionosphere of the PPP-RTK network producer.
 *
 * The network producer can estimate its own global ionosphere, a single-layer
 * spherical-harmonic VTEC field, jointly with the satellite code biases (W6
 * OSB), so it does not need to import an external GIM (`.INX`).  This is the
 * CODE/Schaer formulation: a sun-fixed spherical-harmonic VTEC expansion
 *
 *   VTEC(β, s) = Σ_{n=0}^{N} Σ_{m=0}^{n} P̄_nm(sin β)
 *                            · [ a_nm cos(m s) + b_nm sin(m s) ]   [TECU]
 *
 * where β is the latitude of the ionospheric pierce point (IPP; see the
 * convention note on @ref IppGeometry), s is the sun-fixed longitude
 * (λ_ipp − λ_mean_sun), and P̄_nm are normalized associated Legendre functions
 * chosen so the real surface harmonics {P̄_nm cos(mλ), P̄_nm sin(mλ)} are
 * orthonormal under (1/4π)∫dΩ (a Gram-matrix test in the unit suite checks this
 * convention).  Because P̄_00 = 1 and every other harmonic integrates to zero
 * over the sphere, a_00 is the global mean VTEC of the field.
 *
 * This header provides three reusable, filter-free pieces (Stage 0 of
 * docs/network_global_sh_iono_design_2026-06-29.md):
 *   1. @ref computeIppGeometry: the single-layer pierce-point geometry
 *      (elevation, thin-shell mapping, IPP lat/lon), shared by the SH model and
 *      the legacy @ref GlobalIonosphereMap so both use one implementation.
 *   2. @ref meanSunLongitude: the sub-solar geographic longitude, used to rotate
 *      the field into a sun-fixed frame.
 *   3. @ref SphericalHarmonicIonosphere: the basis (P̄_nm), VTEC synthesis, and
 *      the analytic partials ∂VTEC/∂coeff used by the network row builder.
 *
 * The class owns no estimator state; the SH coefficients live in
 * @ref NetworkEstimator's shared border (SharedParamKind::VtecSh).
 */

#include <Eigen/Dense>

#include <string>
#include <vector>

#include "math/TimeUtils.hpp"

namespace GnssProcessor {

/// Single-layer ionospheric pierce-point geometry for one receiver→satellite
/// line of sight.  Angles in radians; @c valid is false below the horizon or on
/// a degenerate mapping.
///
/// @par IPP latitude convention (read before reusing @c ipp_lat_rad)
/// The shell is a sphere of radius @f$R_e + H@f$ and the IONEX / SH grids are
/// tabulated on it, so the latitude that indexes them is a spherical
/// (geocentric) one.  @ref computeIppGeometry, however, propagates it by
/// spherical trigonometry from the receiver's geodetic latitude
/// (@c Coordinates::ecefToLlh), because that is the IONEX / RTKLIB @c ionppp
/// convention.  The seed carries the ellipsoid and the propagation does not, so
/// this value is neither a strict geodetic latitude nor a strictly geocentric
/// one.  The two conventions differ by up to 0.192° (11.5′) near 45°, so
/// neither label fits, and the field below is described without one.
///
/// This is correct for the consumers that index a spherical grid or basis with
/// it (the IONEX bilinear lookup, the P̄_nm(sin β) basis).  The one consumer
/// that leaves the shell is @c HigherOrderIonosphere, which rebuilds a
/// Cartesian IPP as R·(cos φ cos λ, cos φ sin λ, sin φ): consistent with the
/// sphere, but inheriting the seed offset as about 23 km of along-shell
/// displacement where it samples the magnetic field.  That is immaterial for a
/// cm-level term; do not reuse the value where 0.2° of latitude matters.
struct IppGeometry {
    double elevation_rad = 0.0;   ///< Satellite elevation at the receiver.
    double azimuth_rad   = 0.0;   ///< Satellite azimuth (North→East).
    double mapping       = 0.0;   ///< Thin-shell VTEC→STEC mapping 1/cos(z').
    double ipp_lat_rad   = 0.0;   ///< Latitude of the IPP on the shell; see the
                                  ///< convention note above.  Not plain geodetic.
    double ipp_lon_rad   = 0.0;   ///< Longitude of the IPP on the shell (unambiguous:
                                  ///< geodetic and geocentric longitude are equal).
    bool   valid         = false; ///< False if the LOS is below the horizon / degenerate.
};

/**
 * @brief Single-layer ionospheric pierce-point geometry.
 *
 * This is the geometry @ref GlobalIonosphereMap::slantIonoDelay uses (it calls
 * this function, so the two share one implementation): the satellite elevation /
 * azimuth in the receiver's local ENU frame, the thin-shell mapping
 * @f$1/\cos z'@f$ with @f$\sin z' = \frac{R_e}{R_e+H}\cos(el)@f$, and the IPP
 * latitude / longitude on the single-layer shell.
 *
 * @param rx_pos_ecef    Receiver ECEF position (m).
 * @param sat_pos_ecef   Satellite ECEF position (m).
 * @param earth_radius_m IONEX base radius @f$R_e@f$ (m), e.g. 6371000.
 * @param shell_height_m Single-layer shell height @f$H@f$ (m), e.g. 450000.
 * @return The pierce-point geometry; @c valid is false below the horizon.
 */
IppGeometry computeIppGeometry(const Eigen::Vector3d& rx_pos_ecef,
                               const Eigen::Vector3d& sat_pos_ecef,
                               double earth_radius_m, double shell_height_m);

/**
 * @brief Sub-solar geographic longitude at @p time.
 *
 * Returns the ECEF longitude of the point directly beneath the Sun
 * (@f$\lambda_{ss} = \alpha_\odot - \mathrm{GMST}@f$), used to convert a
 * geographic IPP longitude to the sun-fixed longitude @f$s = \lambda_{ipp} -
 * \lambda_{ss}@f$ the field is expanded in.  Despite the function name, the Sun's
 * right ascension @f$\alpha_\odot@f$ is the apparent one, so the equation of
 * time is included (the true Sun, not the mean Sun).  Closed form (NOAA-style
 * low-order solar series + GMST), accurate to well under 1°, which is sufficient
 * for a sun-fixed VTEC frame: the field coefficients are estimated, so a small
 * consistent offset is absorbed into them.
 *
 * @param time GNSS epoch (GPS time scale; the GPS−UT1 offset costs < 0.1°).
 * @return Sub-solar longitude in radians, wrapped to [−π, π].
 */
double meanSunLongitude(const GnssTime& time);

/**
 * @class SphericalHarmonicIonosphere
 * @brief Real spherical-harmonic single-layer VTEC basis, synthesis, partials.
 *
 * Triangular truncation to degree @c N has @c (N+1)² real coefficients (a
 * cos/"C" term for every (n, m≥0) plus a sin/"S" term for every (n, m≥1)),
 * enumerated once by @ref coeffs in a fixed order that the network's shared
 * column layout mirrors.  All angles in radians.
 */
class SphericalHarmonicIonosphere {
public:
    /// One real coefficient of the expansion.
    struct Coeff {
        int  n;     ///< Degree (0..N).
        int  m;     ///< Order (0..n).
        char trig;  ///< 'C' (cosine; includes m=0) or 'S' (sine; m≥1 only).
    };

    /// @param max_degree Truncation degree N (≥ 0).
    explicit SphericalHarmonicIonosphere(int max_degree);

    /// Truncation degree N.
    int maxDegree() const { return max_degree_; }
    /// Number of real coefficients = (N+1)².
    int numCoeffs() const { return static_cast<int>(coeffs_.size()); }
    /// The coefficient enumeration (fixed order; matches the basis() output order).
    const std::vector<Coeff>& coeffs() const { return coeffs_; }

    /**
     * @brief Evaluate every basis function at one (latitude, sun-fixed longitude).
     *
     * Fills @p out (resized to @ref numCoeffs) with
     * @f$\bar P_{nm}(\sin\beta)\,\{\cos,\sin\}(m\,s)@f$ in @ref coeffs order, so
     * that @c VTEC = Σ_k coeff_k · out_k and @c ∂VTEC/∂coeff_k = out_k.
     *
     * @param lat_rad     Geographic latitude β of the IPP.
     * @param lon_sf_rad  Sun-fixed longitude s = λ_ipp − λ_mean_sun.
     * @param out         Output basis values (length @ref numCoeffs).
     */
    void basis(double lat_rad, double lon_sf_rad, std::vector<double>& out) const;

    /**
     * @brief Synthesize VTEC (TECU) from a coefficient vector.
     * @param coeff_tecu Coefficients in @ref coeffs order (length @ref numCoeffs).
     * @param lat_rad    Geographic latitude β.
     * @param lon_sf_rad Sun-fixed longitude s.
     * @return VTEC in TECU.
     */
    double vtec(const Eigen::VectorXd& coeff_tecu, double lat_rad,
                double lon_sf_rad) const;

private:
    /// Normalized associated Legendre P̄_nm(x) for all 0≤m≤n≤N, x = sin(lat).
    /// Indexed [n][m]; includes the (2−δ_0m) real-harmonic normalization so the
    /// cos/sin basis is orthonormal under (1/4π)∫dΩ.
    void legendre_(double x, std::vector<std::vector<double>>& p) const;

    int max_degree_;
    std::vector<Coeff> coeffs_;
    /// Normalization N_nm = sqrt[(2−δ_0m)(2n+1)(n−m)!/(n+m)!], precomputed [n][m].
    std::vector<std::vector<double>> norm_;
};

struct GlobalVtecShConfig;  // config/GnssConfigTypes.hpp

/**
 * @brief One shell of the (possibly dual-layer) co-estimated SH-VTEC field: a
 *        non-owning view bundling the basis model with the per-shell geometry
 *        and prior scalars every consumer site needs (Iono-2 mechanism A).
 *
 * Every site that evaluates the field (pin-row builder, prior injector, carry
 * stash, deviation harvest, health sweep, consumer supply) loops over the same
 * @ref shLayers list, so a shell is never evaluated at another shell's geometry.
 * The single-layer path builds a one-entry list from the legacy config fields
 * and stays byte-identical.
 */
struct ShLayer {
    const SphericalHarmonicIonosphere* model = nullptr;  ///< Basis (non-owning, non-null).
    int    layer = 0;                 ///< Shell index (0 = legacy "SH", 1 = "SH2").
    double shell_height_m = 0.0;      ///< Shell height H_l (m).
    double kaula_scale_tecu = 0.0;    ///< Kaula σ scale (TECU) for n ≥ 1.
    double q_per_degree = 0.0;        ///< Per-degree RW q (TECU²/s), 1/n² scaled for n ≥ 1.
    double mean_prior_tecu = 0.0;     ///< a₀₀ climatology mean (TECU).
    double mean_prior_sigma_tecu = 0.0;  ///< a₀₀ anchor σ (TECU).
};

/**
 * @brief Assemble the active shell list (one or two entries) from the SH config
 *        and the constructed basis models.
 *
 * @param shc     The global_vtec_sh config block (heights, Kaula/q/a₀₀ scalars).
 * @param layer0  The legacy single-layer basis (may be null → empty list).
 * @param layer1  The second-shell basis (null unless layer2_max_degree > 0).
 * @return Shell views in layer order; empty when @p layer0 is null.
 */
std::vector<ShLayer> shLayers(const GlobalVtecShConfig& shc,
                              const SphericalHarmonicIonosphere* layer0,
                              const SphericalHarmonicIonosphere* layer1);

} // namespace GnssProcessor
