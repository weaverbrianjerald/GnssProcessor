#pragma once

#include <Eigen/Dense>

#include <optional>
#include <string>

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
 * @par One model for PCO and wind-up.
 * Antenna (satellite PCO/PCV) and GnssCorrections::computePhaseWindUp both take
 * the satellite body frame from this class, so the two cannot disagree on the
 * mode selection or the sign conventions.  The wind-up passes the orbit-normal
 * flag as false: it keeps the BeiDou-2 GEO orbit-normal rule and yaw-steers
 * every other satellite whatever @c processing.enable_orbit_normal_attitude
 * says (the flag moves the PCO frame only).
 *
 * @par Noon turn (opt-in).
 * compute() returns the nominal attitude.  applyNoonTurn() turns that frame into
 * the yaw-rate-limited attitude of a GPS Block IIF or IIR satellite near the
 * orbit noon at small |beta| (Kouba 2009): the Sun-tracking yaw would need a
 * rate above what the controller can deliver, so the satellite yaws at its
 * maximum rate instead and rejoins the nominal yaw afterwards.  The caller
 * decides whether the model applies (@c processing.enable_noon_turn_attitude)
 * and which law (noonTurnLaw(), from the antenna type string).
 *
 * @note Not modelled: eclipse and midnight turns (satellites in the Earth's
 *       umbra are excluded upstream by default), yaw biases, and the Block IIA,
 *       III, GLONASS and Galileo manoeuvre laws.  When the Sun is collinear with
 *       nadir, compute() falls back to a velocity-based Y axis.
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
        OrbitNormalNegative,  ///< Orbit-normal label (QZSS); axes are still ON+.
        VelocityFallback,     ///< No Sun direction: frame built from the velocity (velocityFrame()).
        NoonTurn              ///< Yaw-rate-limited noon turn (applyNoonTurn()); X/Y are rotated off the nominal yaw.
    };

    /// Body-frame result: i_s/j_s/k_s are the X/Y/Z axes expressed in ECEF.
    struct Frame {
        Eigen::Vector3d i_s{ Eigen::Vector3d::UnitX() };  ///< +X axis (ECEF, toward Sun).
        Eigen::Vector3d j_s{ Eigen::Vector3d::UnitY() };  ///< +Y axis (ECEF, cross-track).
        Eigen::Vector3d k_s{ Eigen::Vector3d::UnitZ() };  ///< +Z axis (ECEF, nadir).
        Mode   mode{ Mode::YawSteering };                 ///< Attitude mode label (see the Mode warning).
        double beta{ 0.0 };                               ///< Sun elevation above orbit plane (deg).
        double yaw_offset_deg{ 0.0 };                     ///< Actual minus nominal yaw about nadir (deg); nonzero only in Mode::NoonTurn.
    };

    /**
     * @brief Yaw-rate limit of a satellite's attitude controller.
     *
     * The nominal yaw near the orbit noon sweeps up to 180 deg in a time that
     * shrinks with |beta|; the controller follows it only while the required rate
     * stays below @c max_yaw_rate_deg_s.  The model therefore applies when
     * |beta| < beta0Deg() = atan(orbit_rate / max_yaw_rate): 4.35 deg for Block IIF
     * and 2.39 deg for Block IIR.
     */
    struct NoonTurnLaw {
        double max_yaw_rate_deg_s{ 0.0 };  ///< Maximum controller yaw rate (deg/s).
        double orbit_rate_deg_s{ 0.0 };    ///< Mean orbit angular rate (deg/s).

        /// @brief Largest |beta| (deg) at which the satellite cannot follow the nominal yaw.
        double beta0Deg() const;
    };

    /**
     * @brief Yaw angle of a satellite through a noon turn (Kouba 2009).
     *
     * Yaw is the rotation of the body X axis about the nadir (+Z) axis, measured
     * from the inertial velocity direction.  Nominal yaw is
     * atan2(-tan(beta), sin(mu)), mu the orbit angle from the orbit midnight.
     */
    struct NoonTurnYaw {
        bool   active{ false };            ///< True while the satellite is yawing at its maximum rate.
        double beta_deg{ 0.0 };            ///< Sun elevation above the orbit plane (deg).
        double nominal_yaw_deg{ 0.0 };     ///< Sun-tracking yaw (deg).
        double actual_yaw_deg{ 0.0 };      ///< Yaw with the rate limit (deg); equals the nominal outside the turn.
        double start_angle_deg{ 0.0 };     ///< Orbit angle (deg from midnight) at which the turn starts; 0 when no turn can occur.
        double end_angle_deg{ 0.0 };       ///< Orbit angle (deg from midnight) at which the nominal yaw is rejoined; 0 when no turn can occur.
    };

    /// Sun elevation and orbit angle of a satellite, from an inertial orbit normal.
    struct OrbitGeometry {
        bool   valid{ false };             ///< False when the orbit normal or the in-plane Sun direction is degenerate.
        double beta_rad{ 0.0 };            ///< Sun elevation above the orbit plane (rad), positive on the +(r x v) side.
        double orbit_angle_rad{ 0.0 };     ///< Angle from the orbit midnight (rad): 0 at midnight, pi at noon.
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
     * @par Validation.
     * Checked against an independent computation (scripts/attitude_beta_check.py:
     * SP3 positions rotated to the inertial frame, an Almanac Sun, the orbit
     * normal from the SP3 chord, and a velocity-free second source from the
     * broadcast inclination and node) on seven GPS satellites over DOY 129 of
     * 2024, both signs of beta: agreement within 0.01 deg and no sign mismatch,
     * with @p correct_velocity_frame true.  With it false the ECEF velocity is
     * used as is and beta is wrong by up to tens of degrees; the noon-turn model
     * always converts (orbitGeometry()).  The reference rows are in
     * tests/test_noon_turn_attitude.cpp.
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

    /**
     * @brief Numeric PRN of a satellite identifier ("C07" gives 7).
     * @param satellite Identifier with the system letter first.
     * @return The PRN, or 0 when the identifier carries none.
     */
    static int prnOf(const std::string& satellite);

    /**
     * @brief Whether the BeiDou-2 GEO orbit-normal rule applies to a satellite.
     *
     * True for BeiDou PRNs 1 to 5, 59 and 60, which hold orbit-normal attitude
     * whatever beta is.  Shared by the PCO frame and the wind-up.
     *
     * @param system GNSS constellation.
     * @param prn    Numeric PRN.
     * @return True for the hard-coded BeiDou-2 GEO list.
     */
    static bool usesLegacyOrbitNormal(System system, int prn);

    /**
     * @brief Body frame built from the velocity direction alone.
     *
     * Used when no Sun direction is available.  The frame is built about the
     * nadir axis, so x cross y equals +Z as in every other mode.
     *
     * @param[in] sat_pos Satellite ECEF position (meters).
     * @param[in] sat_vel Satellite velocity (m/s); a zero vector falls back to a fixed axis.
     * @return Frame with mode VelocityFallback and beta 0.
     */
    static Frame velocityFrame(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel);

    /**
     * @brief Sun elevation and orbit angle from an inertial orbit normal.
     *
     * Always converts the ECEF velocity to inertial (inertialVelocity()), so the
     * result does not depend on the legacy velocity-frame flag.  The orbit angle
     * is measured in the orbit plane from the anti-Sun direction (orbit
     * midnight) in the direction of motion.
     *
     * @param[in] sat_pos Satellite ECEF position (meters).
     * @param[in] sat_vel Satellite ECEF velocity (m/s), as returned by getSatellitePV.
     * @param[in] sun_pos Sun ECEF position (meters).
     * @return Geometry; @c valid is false when it is degenerate.
     */
    static OrbitGeometry orbitGeometry(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        const Eigen::Vector3d& sun_pos);

    /**
     * @brief Noon-turn law of a satellite, from its antenna type string.
     *
     * The antenna type is the ATX satellite identifier as parsed
     * ("G25_BLOCKIIF_G062"): Block IIF yaws at 0.11 deg/s, Block IIR, IIR-A,
     * IIR-B and IIR-M at 0.20 deg/s.  No other block or system has a law here.
     *
     * @param[in] system       GNSS constellation.
     * @param[in] antenna_type ATX satellite antenna type of the epoch.
     * @return The law, or no value when the satellite has none.
     */
    static std::optional<NoonTurnLaw> noonTurnLaw(System system,
        const std::string& antenna_type);

    /**
     * @brief Yaw through a noon turn at one orbit position (Kouba 2009).
     *
     * Stateless.  The satellite follows the nominal yaw until the rate the nominal
     * would need reaches the law's maximum, then yaws at the maximum rate (toward
     * -sign(beta) times the yaw axis) until the ramp meets the nominal yaw again.
     * The orbit angle advances at the law's constant mean rate, as in the
     * reference model.  No turn occurs when |beta| >= beta0.
     *
     * @param[in] beta_rad        Sun elevation above the orbit plane (rad).
     * @param[in] orbit_angle_rad Orbit angle from midnight (rad).
     * @param[in] law             Yaw-rate limit of the satellite.
     * @return Nominal and actual yaw, and whether the position is inside a turn.
     */
    static NoonTurnYaw noonTurnYaw(double beta_rad, double orbit_angle_rad,
        const NoonTurnLaw& law);

    /**
     * @brief Apply the noon-turn yaw to a nominal body frame.
     *
     * Rotates X and Y about the nadir axis by the difference between the actual
     * and the nominal yaw.  A frame that is not yaw-steered comes back unchanged;
     * a position outside the turn comes back with the same axes (bit-identical)
     * and only beta set.
     *
     * @param[in] nominal  Frame from compute() (mode YawSteering).
     * @param[in] law      Yaw-rate limit of the satellite.
     * @param[in] sat_pos  Satellite ECEF position (meters).
     * @param[in] sat_vel  Satellite ECEF velocity (m/s).
     * @param[in] sun_pos  Sun ECEF position (meters).
     * @return Frame with mode NoonTurn and @c yaw_offset_deg set inside a turn.
     */
    static Frame applyNoonTurn(const Frame& nominal, const NoonTurnLaw& law,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        const Eigen::Vector3d& sun_pos);
};

}  // namespace GnssProcessor
