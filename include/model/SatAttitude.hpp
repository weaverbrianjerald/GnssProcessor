#pragma once

#include <Eigen/Dense>

#include "common/GnssTypes.hpp"   // GnssProcessor::System

namespace GnssProcessor {

/**
 * @brief Stateless satellite body-frame (yaw) attitude model.
 *
 * Computes the IGS/ANTEX satellite body frame {i_s (X), j_s (Y), k_s (Z)}
 * expressed in ECEF, for use by the satellite PCO/PCV geometry.  The frame is
 * right-handed:
 *   - k_s (+Z): nadir, toward Earth centre  (= -r_hat)
 *   - i_s (+X): toward the Sun (in-plane, orthogonalised against nadir)
 *   - j_s (+Y): completes the right-handed triad (solar-panel / cross axis)
 *
 * Two attitude regimes:
 *   - YawSteering: nominal Sun-tracking yaw (GPS, Galileo, GLONASS, BDS-MEO).
 *   - OrbitNormal*: orbit-normal hold (GEO/IGSO and low-beta windows).
 *
 * Mode selection mirrors M-PAGES SatCoords::determineAttitudeMode but ONLY
 * when orbit-normal attitude is enabled by the caller.  The default (legacy)
 * path forces yaw-steering for every system except the hard-coded BeiDou-2 GEO
 * PRNs {C01-C05, C59, C60}, reproducing the engine's historical inline
 * behaviour bit-for-bit (including its degenerate-orbit-normal fallback).
 *
 * @par Frame duplication.
 * An IDENTICAL satellite frame is independently constructed for phase wind-up
 * in GnssCorrections.cpp (ez = -r_hat, ey = ez x e_sun, ex = ey x ez), which
 * still hard-codes BeiDou-GEO orbit-normal.  When this helper's mode selection
 * evolves, keep the two sign conventions in sync or they will silently fork.
 *
 * @note No eclipse / noon-midnight / yaw-rate maneuver modelling — a static
 *       per-epoch nominal-attitude model, as in M-PAGES.
 */
class SatAttitude {
public:
    /**
     * @brief Satellite attitude regime LABEL.
     *
     * @warning NON-LOAD-BEARING for axis sign.  The Frame axes returned by
     *          compute() always use the engine's orbit-normal-POSITIVE geometry
     *          (j_s = +(r x v)) for EVERY orbit-normal case, including
     *          OrbitNormalNegative.  A consumer must NOT infer the orbit-normal
     *          sign from this enum and apply a -orbit-normal Y — that would
     *          double-flip relative to the actually-emitted +orbit-normal axes.
     *          The enum is for trace / diagnostics only until a dedicated,
     *          separately-verified ON- consumer exists.
     */
    enum class Mode {
        YawSteering,          ///< Nominal Sun-tracking yaw.
        OrbitNormalPositive,  ///< Orbit-normal hold (BeiDou GEO convention).
        OrbitNormalNegative   ///< Orbit-normal LABEL (QZSS); axes are still ON+.
    };

    /// Body-frame result: i_s/j_s/k_s are the X/Y/Z axes expressed in ECEF.
    struct Frame {
        Eigen::Vector3d i_s{ Eigen::Vector3d::UnitX() };  ///< +X axis (ECEF, toward Sun).
        Eigen::Vector3d j_s{ Eigen::Vector3d::UnitY() };  ///< +Y axis (ECEF, cross-track).
        Eigen::Vector3d k_s{ Eigen::Vector3d::UnitZ() };  ///< +Z axis (ECEF, nadir).
        Mode   mode{ Mode::YawSteering };                 ///< Attitude mode LABEL (see Mode warning).
        double beta{ 0.0 };                               ///< Sun elevation above orbit plane (deg).
    };

    /**
     * @brief Compute the satellite body frame in ECEF.
     *
     * @param[in] system               GNSS constellation of the satellite.
     * @param[in] prn                  Numeric PRN (e.g. 1 for "C01").
     * @param[in] sat_pos              Satellite ECEF position (meters).
     * @param[in] sat_vel              Satellite ECEF velocity (m/s).
     * @param[in] sun_pos              Sun ECEF position (meters).
     * @param[in] enable_orbit_normal  When false, force the legacy path:
     *                                 yaw-steering everywhere except the
     *                                 hard-coded BeiDou-2 GEO PRN list (engine
     *                                 historical orbit-normal frame, bit-identical).
     *                                 When true, apply the beta-driven
     *                                 determineMode() table.
     * @return Frame with axes (ECEF), mode label, and beta (deg).
     */
    static Frame compute(System system,
        int prn,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        const Eigen::Vector3d& sun_pos,
        bool enable_orbit_normal,
        bool correct_velocity_frame = false);

    /**
     * @brief Sun elevation above the orbital plane (beta angle), in degrees.
     *
     * beta = asin( orbit_normal . sun_hat ), with
     * orbit_normal = normalize(sat_pos x sat_vel) (the +(r x v) angular-momentum
     * normal).  Positive when the Sun is on the +(r x v) side of the orbit plane.
     *
     * @warning The SIGN of beta is UNVALIDATED against any external attitude
     *          product (M-PAGES never computes beta).  beta gates QZSS ON-/YS at
     *          20 deg and BDS ON+/YS at 4 deg, so a flipped normal would silently
     *          select the wrong mode near threshold crossings.  This routine is
     *          the SINGLE point of change if a sign flip is later required.
     *
     * @param[in] sat_pos Satellite ECEF position (meters).
     * @param[in] sat_vel Satellite ECEF velocity (m/s).
     * @param[in] sun_pos Sun ECEF position (meters).
     * @return Beta angle in degrees (0 if geometry is degenerate).
     */
    /**
     * @brief Convert an ECEF (Earth-relative) satellite velocity to inertial.
     *
     * @c Geometry::getSatellitePV deliberately returns ECEF velocity
     * (v_ecef = v_inertial - omega x r, ModelGeometry.cpp) because its primary
     * consumers -- Doppler line-of-sight rate and the relativistic r.v term --
     * want Earth-relative motion.  Attitude does NOT: the orbit plane, and hence
     * the orbit normal h = r x v and the Sun elevation beta above it, are defined
     * in an INERTIAL frame.  Forming r x v_ecef instead yields
     * h_ecef = h_inertial - r x (omega x r), and since |r x (omega x r)| =
     * omega*r^2*cos(lat) is 29-50% of |h| at MEO the resulting normal is wrong by
     * a large angle -- measured on real CODE MGX orbits for 2024-129: 2.2-30.3 deg
     * for GPS, 1.6-36.0 deg for Galileo, 18.0-65.1 deg for BeiDou IGSO.  For a true
     * GEO v_ecef -> 0, so r x v_ecef degenerates entirely to station-keeping drift.
     *
     * Callers holding a @c getSatellitePV velocity must pass it through this
     * helper before any cross-product attitude use.  Note r.v is invariant under
     * the conversion (r . (omega x r) = 0), so the relativistic path is unaffected
     * either way.
     *
     * @param sat_pos   Satellite ECEF position (meters).
     * @param sat_vel   Satellite ECEF velocity (m/s), as returned by getSatellitePV.
     * @return Inertial velocity expressed in ECEF axes (m/s): v_ecef + omega x r.
     */
    static Eigen::Vector3d inertialVelocity(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel);

    static double computeBeta(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        const Eigen::Vector3d& sun_pos,
        bool correct_velocity_frame = false);

    /**
     * @brief Select the attitude mode from system + beta (M-PAGES table).
     *
     * GPS/GAL/GLO always yaw-steer; BeiDou yaw-steers when beta >= 4 deg else
     * ON+; QZSS yaw-steers when beta >= 20 deg else ON-.  SBAS/Unknown -> YS.
     * The threshold compares the RAW (signed) beta, matching M-PAGES
     * SatCoords::determineAttitudeMode verbatim (note: M-PAGES gates BeiDou GEO
     * on isGeostationary(), a stub returning false, so all BeiDou use the beta
     * threshold).  The asymmetric signed test is a faithful port and is gated by
     * the (unvalidated) beta sign — see computeBeta().
     *
     * @param[in] system GNSS constellation.
     * @param[in] beta   Beta angle (degrees, signed).
     * @return Selected attitude mode label.
     */
    static Mode determineMode(System system, double beta);
};

}  // namespace GnssProcessor
