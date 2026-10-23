/**
 * @file EphemerisJumpLedger.cpp
 * @brief The last-used broadcast record per satellite and the range-jump test.
 */

#include "realtime/EphemerisJumpLedger.hpp"
#include "common/Constants.hpp"

#include <cmath>

namespace GnssProcessor {

bool EphemerisJumpLedger::sameOrbitAndClock(const BroadcastEphemeris& a,
                                            const BroadcastEphemeris& b) {
    if (a.system != b.system || a.iode != b.iode) return false;
    if (a.system == System::GLONASS) {
        const GlonassEphemeris& x = a.glonass;
        const GlonassEphemeris& y = b.glonass;
        return x.toe.week == y.toe.week && x.toe.tow == y.toe.tow
            && x.pos == y.pos && x.vel == y.vel && x.acc == y.acc
            && x.tau_n == y.tau_n && x.gamma_n == y.gamma_n;
    }
    const KeplerianEphemeris& x = a.kepler;
    const KeplerianEphemeris& y = b.kepler;
    return x.toe.week == y.toe.week && x.toe.tow == y.toe.tow
        && x.toc.week == y.toc.week && x.toc.tow == y.toc.tow
        && x.sqrt_a == y.sqrt_a && x.e == y.e && x.m0 == y.m0 && x.delta_n == y.delta_n
        && x.i0 == y.i0 && x.idot == y.idot && x.omega0 == y.omega0
        && x.omega_dot == y.omega_dot && x.omega == y.omega
        && x.cuc == y.cuc && x.cus == y.cus && x.crc == y.crc && x.crs == y.crs
        && x.cic == y.cic && x.cis == y.cis
        && x.af0 == y.af0 && x.af1 == y.af1 && x.af2 == y.af2;
}

std::optional<double> EphemerisJumpLedger::rangeJump(const BroadcastEphemeris& old_eph,
                                                     const BroadcastEphemeris& new_eph,
                                                     const GnssTime& time,
                                                     const Eigen::Vector3d& receiver_ecef) {
    const auto s_old = LiveEphemerisStore::evaluateRecord(old_eph, time);
    const auto s_new = LiveEphemerisStore::evaluateRecord(new_eph, time);
    if (!s_old || !s_new) return std::nullopt;
    const Eigen::Vector3d los = s_new->position - receiver_ecef;
    if (!(los.norm() > 0.0)) return std::nullopt;
    const Eigen::Vector3d u = los / los.norm();
    return u.dot(s_new->position - s_old->position)
         - Constants::SPEED_OF_LIGHT * (s_new->clock_bias_s - s_old->clock_bias_s);
}

std::optional<EphemerisJumpLedger::Event>
EphemerisJumpLedger::observe(const std::string& sat,
                             const LiveEphemerisStore::SelectedRecord& now,
                             const GnssTime& time,
                             const Eigen::Vector3d& receiver_ecef) {
    auto it = last_.find(sat);
    if (it == last_.end()) {   // first sighting: record only
        last_.emplace(sat, now);
        return std::nullopt;
    }
    const LiveEphemerisStore::SelectedRecord old = it->second;
    it->second = now;
    if (sameOrbitAndClock(old.eph, now.eph)) return std::nullopt;

    Event ev;
    ev.satellite    = sat;
    ev.old_iode     = old.eph.iode;
    ev.new_iode     = now.eph.iode;
    ev.old_from_igs = old.from_igs;
    ev.new_from_igs = now.from_igs;
    ++counters_.record_changes;
    if (ev.sourceSwitched()) ++counters_.source_switches;

    const auto jump = LiveEphemerisStore::withinFitWindow(old.eph, time)
                    ? rangeJump(old.eph, now.eph, time, receiver_ecef)
                    : std::nullopt;
    if (!jump) {
        ++counters_.undecided;
        return ev;
    }
    ev.decided      = true;
    ev.range_jump_m = *jump;
    ev.jump         = std::abs(*jump) > threshold_m_;
    if (ev.jump) ++counters_.jumps;
    else         ++counters_.below_threshold;
    if (std::abs(*jump) > counters_.max_abs_jump_m) {
        counters_.max_abs_jump_m     = std::abs(*jump);
        counters_.max_jump_satellite = sat;
    }
    return ev;
}

} // namespace GnssProcessor
