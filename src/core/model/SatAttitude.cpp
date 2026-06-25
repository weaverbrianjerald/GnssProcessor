#include "model/SatAttitude.hpp"

#include <algorithm>   // std::clamp
#include <cmath>

namespace GnssProcessor {

namespace {
    constexpr double kPi = 3.14159265358979;
}

double SatAttitude::computeBeta(const Eigen::Vector3d& sat_pos,
    const Eigen::Vector3d& sat_vel,
    const Eigen::Vector3d& sun_pos)
{
    const Eigen::Vector3d h = sat_pos.cross(sat_vel);   // orbit angular momentum, +(r x v)
    if (h.norm() < 1e-8) return 0.0;
    const Eigen::Vector3d n = h.normalized();           // orbit normal
    const Eigen::Vector3d s = (sun_pos - sat_pos);
    if (s.norm() < 1e-8) return 0.0;
    const Eigen::Vector3d e_sun = s.normalized();
    // beta = elevation of the Sun above the orbit plane = asin(n . e_sun),
    // signed by the Sun projection onto the +(r x v) normal.  Sign is UNVALIDATED
    // against an external attitude product — see the header warning.
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
    bool enable_orbit_normal)
{
    Frame fr;

    // +Z (k_s): nadir, toward Earth centre.  Identical expression to the engine.
    fr.k_s = -sat_pos.normalized();

    // Legacy (flag-off) orbit-normal trigger: the hard-coded BeiDou-2 GEO PRN list.
    const bool legacy_geo = (system == System::BeiDou) &&
                            ((prn >= 1 && prn <= 5) || prn == 59 || prn == 60);

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
        beta = computeBeta(sat_pos, sat_vel, sun_pos);
        mode = determineMode(system, beta);
        // Keep the legacy hard-coded GEO override on top of the table: those PRNs
        // are always orbit-normal regardless of beta.
        if (legacy_geo) mode = Mode::OrbitNormalPositive;
        use_orbit_normal = (mode != Mode::YawSteering);
    }

    fr.mode = mode;
    fr.beta = beta;

    if (use_orbit_normal) {
        // Engine orbit-normal construction (Antenna.cpp:517-538), used VERBATIM so
        // the flag-off BeiDou-GEO path is bit-identical.  For flag-on QZSS the mode
        // LABEL is OrbitNormalNegative but the geometry is still engine ON+ (see the
        // Mode warning in the header) — we deliberately do NOT emit M-PAGES literal
        // ON- axes (no validated consumer).  The degenerate-normal warn is emitted
        // by the caller (Antenna) on the flag-off GEO path; this helper is stateless.
        // FRAME DUPLICATION: mirror of the GnssCorrections.cpp wind-up ON frame.
        Eigen::Vector3d orbit_normal = sat_pos.cross(sat_vel);
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
        // Yaw-steering — EXACTLY the engine's operand ordering (j_s = k_s x e_sun,
        // then i_s = j_s x k_s) so the IEEE-754 result is bit-identical to the
        // legacy inline block.  By the BAC-CAB identity i_s = e_sun - k_s(k_s.e_sun),
        // i.e. the Sun direction orthogonalised against nadir (+X half-toward Sun).
        // FRAME DUPLICATION: mirror of the GnssCorrections.cpp wind-up YS frame.
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

}  // namespace GnssProcessor
