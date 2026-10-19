#include "model/SatAttitude.hpp"

#include "common/Constants.hpp"

#include <algorithm>   // std::clamp
#include <cmath>

namespace GnssProcessor {

namespace {
    constexpr double kPi = 3.14159265358979;
    constexpr double kDegToRad = kPi / 180.0;

    // Mean angular rate of a GPS satellite along its orbit: one revolution per
    // sidereal half day, 0.008356 deg/s, rounded as in Kouba (2009).  It fixes the
    // noon-turn threshold beta0 = atan(rate / max yaw rate).
    constexpr double kGpsOrbitRateDegS = 0.00836;
    constexpr double kBlockIifYawRateDegS = 0.11;   // GPS Block IIF maximum yaw rate
    constexpr double kBlockIirYawRateDegS = 0.20;   // GPS Block IIR/IIR-A/-B/-M maximum yaw rate

    // |tan(beta)| floor so that beta = 0 has a defined turn direction (the + side).
    constexpr double kMinAbsTanBeta = 1e-9;
}

double SatAttitude::NoonTurnLaw::beta0Deg() const
{
    if (max_yaw_rate_deg_s <= 0.0) return 0.0;
    return std::atan(orbit_rate_deg_s / max_yaw_rate_deg_s) / kDegToRad;
}

int SatAttitude::prnOf(const std::string& satellite)
{
    if (satellite.size() < 2) return 0;
    try { return std::stoi(satellite.substr(1)); } catch (...) { return 0; }
}

bool SatAttitude::usesLegacyOrbitNormal(System system, int prn)
{
    return (system == System::BeiDou) &&
           ((prn >= 1 && prn <= 5) || prn == 59 || prn == 60);
}

SatAttitude::Frame SatAttitude::velocityFrame(const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel)
{
    // Built about the nadir axis (ez), as in the orbit-normal and yaw-steering
    // modes, so that x cross y equals ez in every mode and the single wind-up
    // quadrature sign holds unconditionally.  Relative to a frame built about
    // z = -ez this negates y and leaves x unchanged.
    Frame fr;
    fr.mode = Mode::VelocityFallback;
    const Eigen::Vector3d ez = -sat_pos.normalized();
    Eigen::Vector3d y_s = sat_vel.norm() > 1e-8 ? sat_vel.normalized().cross(ez)
                                                : Eigen::Vector3d::Zero();
    if (y_s.norm() < 1e-8) y_s = Eigen::Vector3d(0, 1, 0);
    y_s.normalize();
    Eigen::Vector3d x_s = y_s.cross(ez);
    if (x_s.norm() < 1e-8) x_s = Eigen::Vector3d::UnitX();
    x_s.normalize();
    // Re-derive y from the final x so that x cross y equals ez exactly.  A no-op
    // whenever y was already perpendicular to ez; it matters only in the doubly
    // degenerate case where y fell through to the fixed axis.
    y_s = ez.cross(x_s);
    y_s.normalize();
    fr.i_s = x_s;
    fr.j_s = y_s;
    fr.k_s = ez;
    return fr;
}

Eigen::Vector3d SatAttitude::inertialVelocity(const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel)
{
    // omega x r for omega = (0, 0, omega_E).
    const double w = Constants::EARTH_ANGULAR_VELOCITY;
    return Eigen::Vector3d(sat_vel.x() - w * sat_pos.y(),
                           sat_vel.y() + w * sat_pos.x(),
                           sat_vel.z());
}

double SatAttitude::computeBeta(const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel,
    const Eigen::Vector3d& sun_pos,
    bool correct_velocity_frame)
{
    // sat_vel arrives ECEF from getSatellitePV; the orbit plane is inertial.
    const Eigen::Vector3d v_in = correct_velocity_frame
        ? inertialVelocity(sat_pos, sat_vel) : sat_vel;
    const Eigen::Vector3d h = sat_pos.cross(v_in);      // orbit angular momentum, +(r x v)
    if (h.norm() < 1e-8) return 0.0;
    const Eigen::Vector3d n = h.normalized();           // orbit normal
    const Eigen::Vector3d s = (sun_pos - sat_pos);
    if (s.norm() < 1e-8) return 0.0;
    const Eigen::Vector3d e_sun = s.normalized();
    // beta = elevation of the Sun above the orbit plane = asin(n . e_sun),
    // signed by the Sun projection onto the +(r x v) normal.  The sign has not been
    // validated against an external attitude product (see the header warning).
    const double sin_beta = std::clamp(n.dot(e_sun), -1.0, 1.0);
    return std::asin(sin_beta) * 180.0 / kPi;
}

SatAttitude::Mode SatAttitude::determineMode(System system, double beta)
{
    switch (system) {
        case System::GPS:
        case System::GLONASS:
        case System::Galileo:
            return Mode::YawSteering;
        case System::BeiDou:
            // M-PAGES gates GEO on isGeostationary() (a stub returning false), so
            // every BeiDou satellite uses the raw (signed) beta threshold here.
            return (beta >= 4.0) ? Mode::YawSteering : Mode::OrbitNormalPositive;
        case System::QZSS:
            return (beta >= 20.0) ? Mode::YawSteering : Mode::OrbitNormalNegative;
        case System::SBAS:
        case System::Unknown:
        default:
            return Mode::YawSteering;
    }
}

SatAttitude::Frame SatAttitude::compute(System system,
    int prn,
    const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel,
    const Eigen::Vector3d& sun_pos,
    bool enable_orbit_normal,
    bool correct_velocity_frame)
{
    Frame fr;

    // +Z (k_s): nadir, toward Earth centre.  Identical expression to the engine.
    fr.k_s = -sat_pos.normalized();

    // Legacy (flag-off) orbit-normal trigger: the hard-coded BeiDou-2 GEO PRN list.
    const bool legacy_geo = usesLegacyOrbitNormal(system, prn);

    Mode mode = Mode::YawSteering;
    double beta = 0.0;
    bool use_orbit_normal = false;

    if (!enable_orbit_normal) {
        // Reproduce the engine's historical inline behaviour bit-for-bit: yaw-steer
        // for everything except the BeiDou-2 GEO PRNs (beta stays 0, as the engine
        // never computed it on this path).
        use_orbit_normal = legacy_geo;
        mode = legacy_geo ? Mode::OrbitNormalPositive : Mode::YawSteering;
    } else {
        beta = computeBeta(sat_pos, sat_vel, sun_pos, correct_velocity_frame);
        mode = determineMode(system, beta);
        // Keep the legacy hard-coded GEO override on top of the table: those PRNs
        // are always orbit-normal regardless of beta.
        if (legacy_geo) mode = Mode::OrbitNormalPositive;
        use_orbit_normal = (mode != Mode::YawSteering);
    }

    fr.mode = mode;
    fr.beta = beta;

    if (use_orbit_normal) {
        // The engine's orbit-normal construction, formerly inline in Antenna.cpp,
        // with the same operands in the same order, so the flag-off BeiDou-GEO path
        // is bit-identical.  For flag-on QZSS the mode label is OrbitNormalNegative
        // but the geometry is still the engine's ON+ (see the Mode warning in the
        // header); M-PAGES' literal ON- axes are not emitted because no validated
        // consumer exists.  The helper is stateless, so the caller (Antenna) emits
        // the degenerate-normal warning for the GEO PRNs.
        // The phase wind-up takes this same frame (GnssCorrections::computePhaseWindUp).
        // r x v_ecef is not the orbit normal (see SatAttitude::inertialVelocity);
        // with correct_velocity_frame off, the legacy engine block is reproduced
        // bit-for-bit.  With orbit-normal attitude off, this branch serves only the
        // BeiDou-2 GEO PRNs, which are inactive in post-processing (no SP3 entry in
        // the CODE MGX products, so they drop as NoProduct before geometry) and
        // active in real-time, where LiveEphemerisStore supplies their orbits.
        Eigen::Vector3d orbit_normal = sat_pos.cross(
            correct_velocity_frame ? inertialVelocity(sat_pos, sat_vel) : sat_vel);
        if (orbit_normal.norm() < 1e-8) {
            orbit_normal = Eigen::Vector3d::UnitY();
        }
        fr.j_s = orbit_normal.normalized();

        fr.i_s = fr.j_s.cross(fr.k_s);
        if (fr.i_s.norm() < 1e-8) {
            fr.i_s = Eigen::Vector3d::UnitX();
        }
        fr.i_s.normalize();

        // Re-orthogonalise Y for a clean right-handed frame.
        fr.j_s = fr.k_s.cross(fr.i_s);
        fr.j_s.normalize();
    } else {
        // Yaw-steering, with the engine's operand order (j_s = k_s x e_sun, then
        // i_s = j_s x k_s) so the IEEE-754 result is bit-identical to the legacy
        // inline block.  By the BAC-CAB identity i_s = e_sun - k_s(k_s.e_sun), i.e.
        // the Sun direction orthogonalised against nadir (+X in the Sun's half-space).
        // The phase wind-up takes this same frame (GnssCorrections::computePhaseWindUp).
        Eigen::Vector3d e_sun = (sun_pos - sat_pos).normalized();

        fr.j_s = fr.k_s.cross(e_sun);
        if (fr.j_s.norm() < 1e-8) {
            // Fallback for eclipse / noon turn.
            Eigen::Vector3d vhat = sat_vel.norm() > 1e-8 ? sat_vel.normalized()
                                                         : Eigen::Vector3d::UnitY();
            fr.j_s = fr.k_s.cross(vhat);
        }
        fr.j_s.normalize();

        fr.i_s = fr.j_s.cross(fr.k_s);
        if (fr.i_s.norm() < 1e-8) {
            fr.i_s = Eigen::Vector3d::UnitX();
        }
        fr.i_s.normalize();
    }

    return fr;
}

SatAttitude::OrbitGeometry SatAttitude::orbitGeometry(const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel,
    const Eigen::Vector3d& sun_pos)
{
    OrbitGeometry g;
    // The orbit plane is inertial: h = r x v_inertial (see inertialVelocity()).
    const Eigen::Vector3d h = sat_pos.cross(inertialVelocity(sat_pos, sat_vel));
    const Eigen::Vector3d s = sun_pos - sat_pos;
    if (h.norm() < 1e-8 || s.norm() < 1e-8) return g;
    const Eigen::Vector3d n = h.normalized();
    const Eigen::Vector3d e_sun = s.normalized();
    const double sin_beta = std::clamp(n.dot(e_sun), -1.0, 1.0);

    // In-plane axes: p toward the Sun's projection, q = n x p.  The satellite at
    // orbit angle mu sits at r_hat = -cos(mu) p - sin(mu) q, so mu is 0 at the orbit
    // midnight (anti-Sun point), advances along the motion, and is pi at noon.
    Eigen::Vector3d p = e_sun - sin_beta * n;
    if (p.norm() < 1e-9) return g;       // Sun on the orbit normal: no noon or midnight
    p.normalize();
    const Eigen::Vector3d q = n.cross(p);
    const Eigen::Vector3d r_hat = sat_pos.normalized();

    g.beta_rad = std::asin(sin_beta);
    g.orbit_angle_rad = std::atan2(-r_hat.dot(q), -r_hat.dot(p));
    g.valid = true;
    return g;
}

std::optional<SatAttitude::NoonTurnLaw> SatAttitude::noonTurnLaw(System system,
    const std::string& antenna_type)
{
    if (system != System::GPS) return std::nullopt;
    // The block is the field between the first and the second underscore of
    // "G25_BLOCKIIF_G062".
    const auto first = antenna_type.find('_');
    if (first == std::string::npos) return std::nullopt;
    const auto second = antenna_type.find('_', first + 1);
    const std::string block = antenna_type.substr(first + 1,
        second == std::string::npos ? std::string::npos : second - first - 1);

    if (block == "BLOCKIIF") {
        return NoonTurnLaw{ kBlockIifYawRateDegS, kGpsOrbitRateDegS };
    }
    if (block.rfind("BLOCKIIR", 0) == 0) {            // IIR, IIR-A, IIR-B, IIR-M
        return NoonTurnLaw{ kBlockIirYawRateDegS, kGpsOrbitRateDegS };
    }
    return std::nullopt;
}

SatAttitude::NoonTurnYaw SatAttitude::noonTurnYaw(double beta_rad, double orbit_angle_rad,
    const NoonTurnLaw& law)
{
    NoonTurnYaw out;
    out.beta_deg = beta_rad / kDegToRad;

    constexpr double kTwoPi = 2.0 * kPi;
    double mu = std::fmod(orbit_angle_rad, kTwoPi);
    if (mu < 0.0) mu += kTwoPi;

    // beta = 0 has no preferred side; take the + side so that the direction is defined.
    const double sgn = (beta_rad >= 0.0) ? 1.0 : -1.0;
    const double tb = std::max(std::abs(std::tan(beta_rad)), kMinAbsTanBeta);

    out.nominal_yaw_deg = std::atan2(-sgn * tb, std::sin(mu)) / kDegToRad;
    out.actual_yaw_deg = out.nominal_yaw_deg;

    const double mu_dot = law.orbit_rate_deg_s * kDegToRad;   // rad/s
    const double rate = law.max_yaw_rate_deg_s * kDegToRad;   // rad/s
    // The nominal yaw rate peaks at the noon with mu_dot / |tan(beta)|; the
    // satellite follows it whenever that stays below the controller's maximum.
    if (!(mu_dot > 0.0) || !(rate > 0.0) || tb >= mu_dot / rate) return out;

    // u: orbit angle from the noon, in [-pi, pi).  The nominal yaw there is
    // atan2(-sgn tan(beta), -sin(u)); it is continuous through u = 0 and stays in
    // (-pi, 0) for beta > 0 and (0, pi) for beta < 0 over the whole turn.
    const double u = mu - kPi;
    auto nominal = [&](double uu) { return std::atan2(-sgn * tb, -std::sin(uu)); };

    // Start of the turn at -x_s: the nominal yaw rate mu_dot tb cos(u) / (sin^2(u) +
    // tb^2) reaches the maximum rate.  With c = cos(u) that is the quadratic
    // rate * c^2 + mu_dot * tb * c - rate * (1 + tb^2) = 0.
    const double c = (-mu_dot * tb + std::sqrt(mu_dot * mu_dot * tb * tb
                                               + 4.0 * rate * rate * (1.0 + tb * tb))) / (2.0 * rate);
    const double x_s = std::acos(std::min(c, 1.0));
    const double psi_s = nominal(-x_s);
    // The satellite yaws at the maximum rate against the sign of beta from the start.
    auto ramp = [&](double uu) { return psi_s - sgn * rate * (uu + x_s) / mu_dot; };

    // End of the turn: the first u past the noon where the ramp meets the nominal
    // yaw again.  Between -x_s and +x_s the nominal yaw moves faster than the
    // ramp, so at u = x_s the ramp is still on the trailing side of it
    // (sgn * (ramp - nominal) > 0).  Once the ramp reaches -sgn pi, which the
    // nominal yaw never passes, it has crossed it.
    double lo = x_s;
    double hi = std::min(-x_s + (kPi - std::abs(psi_s)) * mu_dot / rate, kPi - 1e-9);
    double u_end = lo;
    if (hi > lo) {
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (sgn * (ramp(mid) - nominal(mid)) > 0.0) lo = mid; else hi = mid;
        }
        u_end = 0.5 * (lo + hi);
    }

    out.start_angle_deg = (kPi - x_s) / kDegToRad;
    out.end_angle_deg = (kPi + u_end) / kDegToRad;
    if (u >= -x_s && u <= u_end) {
        out.active = true;
        out.actual_yaw_deg = ramp(u) / kDegToRad;
    }
    return out;
}

SatAttitude::Frame SatAttitude::applyNoonTurn(const Frame& nominal, const NoonTurnLaw& law,
    const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel,
    const Eigen::Vector3d& sun_pos)
{
    if (nominal.mode != Mode::YawSteering) return nominal;
    Frame out = nominal;
    const OrbitGeometry g = orbitGeometry(sat_pos, sat_vel, sun_pos);
    if (!g.valid) return out;

    const NoonTurnYaw yaw = noonTurnYaw(g.beta_rad, g.orbit_angle_rad, law);
    out.beta = yaw.beta_deg;
    if (!yaw.active) return out;

    // Yaw is positive about the nadir axis, so a yaw change of delta rotates the
    // X axis about +Z by delta (Rodrigues' formula for a vector normal to the axis).
    double delta = (yaw.actual_yaw_deg - yaw.nominal_yaw_deg) * kDegToRad;
    delta -= kPi * 2.0 * std::round(delta / (kPi * 2.0));
    const Eigen::Vector3d& k = nominal.k_s;
    out.i_s = (std::cos(delta) * nominal.i_s + std::sin(delta) * k.cross(nominal.i_s)).normalized();
    out.j_s = k.cross(out.i_s).normalized();
    out.mode = Mode::NoonTurn;
    out.yaw_offset_deg = delta / kDegToRad;
    return out;
}

bool SatAttitude::inNoonTurnWindow(System system,
    const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel,
    const Eigen::Vector3d& sun_pos,
    double margin_s)
{
    if (system != System::GPS) return false;
    const OrbitGeometry g = orbitGeometry(sat_pos, sat_vel, sun_pos);
    if (!g.valid) return false;

    const NoonTurnLaw law{ kBlockIifYawRateDegS, kGpsOrbitRateDegS };
    const NoonTurnYaw yaw = noonTurnYaw(g.beta_rad, g.orbit_angle_rad, law);
    if (yaw.active) return true;
    // start and end stay 0 when |beta| is at or above beta0: no turn can occur.
    if (!(yaw.end_angle_deg > yaw.start_angle_deg)) return false;

    const double margin_deg = law.orbit_rate_deg_s * std::max(margin_s, 0.0);
    double mu_deg = std::fmod(g.orbit_angle_rad / kDegToRad, 360.0);
    if (mu_deg < 0.0) mu_deg += 360.0;
    return mu_deg >= yaw.start_angle_deg - margin_deg
        && mu_deg <= yaw.end_angle_deg + margin_deg;
}

}  // namespace GnssProcessor
