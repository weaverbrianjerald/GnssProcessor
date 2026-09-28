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
 *   - i_s (+X): toward the Sun (in-plane, orthogonalised against nadir) under
 *               yaw-steering; under orbit-normal hold, j_s x k_s
 *   - j_s (+Y): completes the right-handed triad (solar-panel / cross axis)
 *
 * Two attitude regimes:
 *   - YawSteering: nominal Sun-tracking yaw (GPS, Galileo, GLONASS, BDS-MEO).
 *   - OrbitNormal*: orbit-normal hold (GEO/IGSO and low-beta windows).
 *
 * Mode selection follows M-PAGES SatCoords::determineAttitudeMode, but only
 * when the caller enables orbit-normal attitude.  The default (legacy) path
 * yaw-steers every system except the hard-coded BeiDou-2 GEO PRNs
 * {C01-C05, C59, C60}, and reproduces the engine's historical inline behaviour
 * bit-for-bit, including its degenerate-orbit-normal fallback.  With orbit-normal
 * attitude enabled, those GEO PRNs stay orbit-normal whatever beta is.
 *
 * @par Frame duplication.
 * GnssCorrections.cpp builds the same satellite frame independently for phase
 * wind-up (ez = -r_hat, ey = ez x e_sun, ex = ey x ez) and still hard-codes
 * BeiDou-GEO orbit-normal regardless of the orbit-normal flag.  A change to the
 * mode selection or sign conventions here must be made there too; nothing
 * checks that the two agree.
 *
 * @note No eclipse, noon/midnight-turn or yaw-rate manoeuvre modelling: this is
 *       a static per-epoch nominal-attitude model, as in M-PAGES.  When the Sun
 *       is collinear with nadir, compute() falls back to a velocity-based Y axis.
 */
class SatAttitude {
public:
    /**
     * @brief Satellite attitude regime label.
     *
     * @warning The label does not determine the axis signs.  compute() returns
     *          axes built with the engine's positive orbit-normal geometry
     *          (j_s = +(r x v)) for every orbit-normal case, OrbitNormalNegative
     *          included.  A consumer must not read the orbit-normal sign from this
     *          enum and apply a negative orbit-normal Y: that would flip the axes a
     *          second time relative to the +orbit-normal axes actually returned.
     *          The enum serves traces and diagnostics only, until a separately
     *          verified ON- consumer exists.
     */
    enum class Mode {
        YawSteering,          ///< Nominal Sun-tracking yaw.
        OrbitNormalPositive,  ///< Orbit-normal hold (BeiDou GEO convention).
        OrbitNormalNegative   ///< Orbit-normal label (QZSS); axes are still ON+.
    };

    /// Body-frame result: i_s/j_s/k_s are the X/Y/Z axes expressed in ECEF.
    struct Frame {
        Eigen::Vector3d i_s{ Eigen::Vector3d::UnitX() };  ///< +X axis (ECEF, toward Sun).
        Eigen::Vector3d j_s{ Eigen::Vector3d::UnitY() };  ///< +Y axis (ECEF, cross-track).
        Eigen::Vector3d k_s{ Eigen::Vector3d::UnitZ() };  ///< +Z axis (ECEF, nadir).
        Mode   mode{ Mode::YawSteering };                 ///< Attitude mode label (see the Mode warning).
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
     * @param[in] enable_orbit_normal  When false, use the legacy path:
     *                                 yaw-steering everywhere except the
     *                                 hard-coded BeiDou-2 GEO PRN list (the engine's
     *                                 historical orbit-normal frame, bit-identical);
     *                                 beta is not computed and stays 0.
     *                                 When true, apply the beta-driven
     *                                 determineMode() table, with the GEO PRNs
     *                                 kept orbit-normal.
     * @param[in] correct_velocity_frame When true, convert sat_vel to inertial
     *                                 (inertialVelocity()) before forming the
     *                                 orbit normal r x v.  False reproduces the
     *                                 legacy frame bit-for-bit.
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
     * @brief Convert an ECEF (Earth-relative) satellite velocity to inertial.
     *
     * @c Geometry::getSatellitePV returns ECEF velocity
     * (v_ecef = v_inertial - omega x r, ModelGeometry.cpp) on purpose: its main
     * consumers, the Doppler line-of-sight rate and the relativistic r.v term,
     * need Earth-relative motion.  Attitude does not: the orbit plane, and with it
     * the orbit normal h = r x v and the Sun elevation beta above that plane, are
     * defined in an inertial frame.  Forming r x v_ecef instead gives
     * h_ecef = h_inertial - r x (omega x r).  Since |r x (omega x r)| =
     * omega*r^2*cos(lat) is about 29-59 % of |h| at GPS and Galileo MEO, the
     * resulting normal is off by tens of degrees (measured on CODE MGX orbits: up
     * to 30 deg for GPS, 36 deg for Galileo and 65 deg for BeiDou IGSO).  For a
     * true GEO, v_ecef -> 0, so r x v_ecef reduces to station-keeping drift.
     *
     * Callers holding a @c getSatellitePV velocity pass it through this helper
     * before any cross-product attitude use.  r.v is invariant under the
     * conversion (r . (omega x r) = 0), so the relativistic path is unaffected
     * either way.
     *
     * @param sat_pos   Satellite ECEF position (meters).
     * @param sat_vel   Satellite ECEF velocity (m/s), as returned by getSatellitePV.
     * @return Inertial velocity expressed in ECEF axes (m/s): v_ecef + omega x r.
     */
    static Eigen::Vector3d inertialVelocity(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel);

    /**
     * @brief Sun elevation above the orbital plane (beta angle), in degrees.
     *
     * beta = asin( orbit_normal . sun_hat ), with
     * orbit_normal = normalize(sat_pos x sat_vel) (the +(r x v) angular-momentum
     * normal).  Positive when the Sun is on the +(r x v) side of the orbit plane.
     *
     * @warning The sign of beta has not been validated against any external
     *          attitude product (M-PAGES never computes beta).  beta gates QZSS
     *          ON-/YS at 20 deg and BDS ON+/YS at 4 deg, so a flipped normal would
     *          select the wrong mode near threshold crossings without any error.
     *          A sign flip, if one is later required, is made in this routine
     *          alone.
     *
     * @param[in] sat_pos Satellite ECEF position (meters).
     * @param[in] sat_vel Satellite ECEF velocity (m/s).
     * @param[in] sun_pos Sun ECEF position (meters).
     * @param[in] correct_velocity_frame When true, convert sat_vel to inertial
     *                    (inertialVelocity()) before forming r x v.
     * @return Beta angle in degrees (0 if geometry is degenerate).
     */
    static double computeBeta(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        const Eigen::Vector3d& sun_pos,
        bool correct_velocity_frame = false);

    /**
     * @brief Select the attitude mode from system + beta (M-PAGES table).
     *
     * GPS/GAL/GLO always yaw-steer; BeiDou yaw-steers when beta >= 4 deg else
     * ON+; QZSS yaw-steers when beta >= 20 deg else ON-.  SBAS/Unknown -> YS.
     * The threshold compares the raw (signed) beta, as M-PAGES
     * SatCoords::determineAttitudeMode does.  M-PAGES gates BeiDou GEO on
     * isGeostationary(), a stub returning false, so every BeiDou satellite uses
     * the beta threshold here too.  The asymmetric signed test is a direct port
     * and depends on the unvalidated beta sign (see computeBeta()).
     *
     * @param[in] system GNSS constellation.
     * @param[in] beta   Beta angle (degrees, signed).
     * @return Selected attitude mode label.
     */
    static Mode determineMode(System system, double beta);
};

}  // namespace GnssProcessor
