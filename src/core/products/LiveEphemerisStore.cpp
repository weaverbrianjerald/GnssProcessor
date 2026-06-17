/**
 * @file LiveEphemerisStore.cpp
 * @brief Implementation of the live broadcast-ephemeris cache + orbit models.
 *
 * Keplerian propagation (GPS / Galileo / BeiDou / QZSS) follows the
 * broadcast-ICD algorithm common to all four constellations, with
 * per-system gravitational parameter μ and Earth rotation rate ωₑ.  BeiDou
 * is evaluated on its native BDT time scale (BDT = GPS − 14 s) so the node
 * term ωₑ·toe is self-consistent.  GLONASS is integrated from its PZ-90
 * state with a 4th-order Runge–Kutta step.
 */

#include "products/LiveEphemerisStore.hpp"
#include "common/Constants.hpp"
#include <spdlog/spdlog.h>
#include <array>
#include <cmath>

namespace GnssProcessor {

namespace {

// Continuous seconds on whatever week scale the GnssTime is expressed in.
double continuousSeconds(const GnssTime& t) {
    return static_cast<double>(t.week) * 604800.0 + t.tow;
}

// BeiDou toe is stored on the native BDT scale (BDT week 0 = GPS week 1356,
// BDT = GPS − 14 s).  This offset maps a BDT-continuous-seconds value up to
// GPS-continuous seconds for honest cross-system epoch comparisons.
constexpr double kBdtToGpsOffsetSec = 1356.0 * 604800.0 + 14.0;

// toe of any record expressed in GPS-continuous seconds (for selection).
double toeGpsContinuous(const BroadcastEphemeris& e) {
    if (e.system == System::GLONASS) return continuousSeconds(e.glonass.toe);
    double c = continuousSeconds(e.kepler.toe);
    if (e.system == System::BeiDou) c += kBdtToGpsOffsetSec;
    return c;
}

// Per-system gravitational parameter μ [m^3/s^2] and Earth rotation ωₑ [rad/s].
struct GravityModel { double mu; double we; };

GravityModel gravityFor(System sys) {
    switch (sys) {
    case System::GPS:
    case System::QZSS:    return {3.986005e14,    7.2921151467e-5};
    case System::Galileo: return {3.986004418e14, 7.2921151467e-5};
    case System::BeiDou:  return {3.986004418e14, 7.292115e-5};
    default:              return {3.986005e14,    7.2921151467e-5};
    }
}

// Reduce a time-from-ephemeris difference into [-302400, +302400] s.
double normalizeTk(double tk) {
    if (tk >  302400.0) tk -= 604800.0;
    if (tk < -302400.0) tk += 604800.0;
    return tk;
}

// Map a decoded broadcast ephemeris's issue-of-data onto the IGS-SSR
// (RTCM 4076) "GNSS IOD" matching convention — an 8-bit field (IDF012) whose
// per-constellation DEFINITION differs from the raw broadcast field stored in
// BroadcastEphemeris::iode.  The SSR overlay gate (SsrCorrectionStore::
// iodeMatches) compares THIS value against ssr->iode, so it must be the same
// quantity the IGS-SSR provider encoded:
//   GPS / QZSS : the 8-bit LNAV IODE — already the SSR convention (passthrough).
//   Galileo    : the low 8 bits of the 10-bit I/NAV IODnav (spec: "8 LSBs of
//                IODnav") — the SSR field is only 8 bits wide.
//   BeiDou     : a toe-derived value, mod(toe_sow / 720, 240), per IGS-SSR v1.00
//                (which references the BDS toe / DF505).  The broadcast AODE that
//                consumeBds stores in eph.iode is a DIFFERENT quantity and would
//                NEVER match, so it is deliberately ignored here.
//   GLONASS    : passthrough (excluded from active_systems).
// The raw eph.iode is left intact for the store's IODE-transition (demote)
// bookkeeping; only the value handed to the SSR gate is normalised here, so the
// convention lives in exactly one place regardless of which decoder (SBF
// SubframeAssembler or RTCM RtcmNavDecoder) produced the ephemeris.
int ssrMatchIode(const BroadcastEphemeris& eph) {
    switch (eph.system) {
    case System::Galileo:
        return (eph.iode < 0) ? eph.iode : (eph.iode & 0xFF);
    case System::BeiDou: {
        const double toe_sow = eph.kepler.toe.tow;   // native BDT seconds-of-week
        if (toe_sow <= 0.0) return eph.iode;          // degenerate; fall back (likely -1)
        return static_cast<int>(toe_sow / 720.0) % 240;
    }
    default:
        return eph.iode;
    }
}

// Max usable |t − toe| per system ≈ the broadcast fit interval × ~2 margin.
// Beyond this an extrapolated orbit/clock is junk; reject (sat skipped) rather
// than emit a silently-wrong position.  Sized so a legitimately old record
// (GPS toe can be ~2 h out just before the next upload) is NEVER rejected.
double maxEphAgeForSystem(System s) {
    switch (s) {
    case System::GPS:
    case System::QZSS:    return 4.0 * 3600.0;
    case System::Galileo: return 2.0 * 3600.0;
    case System::BeiDou:  return 2.0 * 3600.0;
    case System::GLONASS: return 0.5 * 3600.0;
    default:              return 4.0 * 3600.0;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

LiveEphemerisStore& LiveEphemerisStore::getInstance() {
    static LiveEphemerisStore instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Push
// ---------------------------------------------------------------------------

void LiveEphemerisStore::insert(const BroadcastEphemeris& eph) {
    if (eph.satellite.empty() || eph.system == System::Unknown) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_sat_.find(eph.satellite);
    if (it == by_sat_.end()) {
        by_sat_.emplace(eph.satellite, SatRecord{eph, std::nullopt});
        return;
    }
    // Demote the current record to "previous" only when the IODE changes, so
    // an IODE transition keeps the orbit valid just before the cutover.
    if (it->second.current.iode != eph.iode) {
        // Diagnostic: a broadcast-ephemeris cutover.  The SSR orbit/clock delta
        // is referenced to a specific broadcast version (iode_ssr); each cutover
        // steps the broadcast baseline the delta is applied to, so a residual
        // saw-tooth that lines up with these events is the ephemeris-turnover
        // mechanism.  Galileo I/NAV cuts over every few minutes vs GPS ~2 h, so
        // expect far more EPH-IODE-CHANGE lines for E# than G#.  rt_engine is
        // registered in the global spdlog registry by LogManager (debug level);
        // guard the get() since a cutover can arrive before logging is wired up.
        if (auto lg = spdlog::get("rt_engine"))
            lg->debug("EPH-IODE-CHANGE {} iode {} -> {} (toe tow {:.0f} -> {:.0f})",
                      eph.satellite, it->second.current.iode, eph.iode,
                      it->second.current.kepler.toe.tow, eph.kepler.toe.tow);
        it->second.previous = it->second.current;
    }
    it->second.current = eph;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

const BroadcastEphemeris& LiveEphemerisStore::pick(const SatRecord& rec,
                                                   const GnssTime& time) {
    if (!rec.previous) return rec.current;
    const double q = continuousSeconds(time);
    const double d_cur = std::abs(q - toeGpsContinuous(rec.current));
    const double d_prv = std::abs(q - toeGpsContinuous(*rec.previous));
    return (d_prv < d_cur) ? *rec.previous : rec.current;
}

const BroadcastEphemeris& LiveEphemerisStore::pickForSsr(const SatRecord& rec,
                                                         const GnssTime& time,
                                                         int ssr_iode) {
    // Re-reference: when the SSR delta names a version we still hold, return THAT
    // record (current or the retained previous) so orbit and clock are applied
    // against the ephemeris the correction was computed for.  Across a cutover
    // the receiver can be a version ahead of the SSR stream; without this the
    // current-vs-SSR IODE mismatch would drop the satellite to broadcast-grade.
    if (ssr_iode >= 0) {
        if (ssrMatchIode(rec.current) == ssr_iode) return rec.current;
        if (rec.previous && ssrMatchIode(*rec.previous) == ssr_iode) return *rec.previous;
    }
    return pick(rec, time);
}

// ---------------------------------------------------------------------------
// Keplerian position (GPS / Galileo / BeiDou / QZSS)
// ---------------------------------------------------------------------------

Eigen::Vector3d LiveEphemerisStore::keplerPosition(const BroadcastEphemeris& eph,
                                                   const GnssTime& time) {
    const KeplerianEphemeris& k = eph.kepler;
    const GravityModel g = gravityFor(eph.system);

    const double a = k.sqrt_a * k.sqrt_a;
    if (a < 1.0) return Eigen::Vector3d::Zero();      // uninitialised / invalid

    // Evaluate on the satellite's native scale.  BeiDou broadcasts on BDT, so
    // both the query epoch and the stored toe live on BDT and the node term
    // ωₑ·toe (below) uses the BDT seconds-of-week directly.
    double q = continuousSeconds(time);
    if (eph.system == System::BeiDou) q -= kBdtToGpsOffsetSec;
    const double tk = normalizeTk(q - continuousSeconds(k.toe));

    const double n0 = std::sqrt(g.mu / (a * a * a));
    const double n  = n0 + k.delta_n;
    const double M  = k.m0 + n * tk;

    // Eccentric anomaly via Newton iteration.
    double E = M;
    for (int it = 0; it < 15; ++it) {
        const double dE = (M - (E - k.e * std::sin(E))) / (1.0 - k.e * std::cos(E));
        E += dE;
        if (std::abs(dE) < 1e-13) break;
    }
    const double sinE = std::sin(E);
    const double cosE = std::cos(E);

    const double nu  = std::atan2(std::sqrt(1.0 - k.e * k.e) * sinE, cosE - k.e);
    const double phi = nu + k.omega;
    const double s2  = std::sin(2.0 * phi);
    const double c2  = std::cos(2.0 * phi);

    const double u = phi + k.cus * s2 + k.cuc * c2;
    const double r = a * (1.0 - k.e * cosE) + k.crs * s2 + k.crc * c2;
    const double i = k.i0 + k.idot * tk + k.cis * s2 + k.cic * c2;

    const double xo = r * std::cos(u);
    const double yo = r * std::sin(u);

    // Corrected longitude of ascending node (ECEF).  Standard MEO/IGSO
    // formula; BeiDou GEO satellites require an additional fixed rotation
    // that is not applied here (GEO are rarely used for PPP-RTK).
    const double omega = k.omega0 + (k.omega_dot - g.we) * tk - g.we * k.toe.tow;
    const double cosO = std::cos(omega), sinO = std::sin(omega);
    const double cosi = std::cos(i),     sini = std::sin(i);

    return Eigen::Vector3d{
        xo * cosO - yo * cosi * sinO,
        xo * sinO + yo * cosi * cosO,
        yo * sini
    };
}

// ---------------------------------------------------------------------------
// GLONASS PZ-90 integration
// ---------------------------------------------------------------------------

LiveEphemerisStore::SatelliteState
LiveEphemerisStore::glonassState(const BroadcastEphemeris& eph, const GnssTime& time) {
    // PZ-90 constants (GLONASS ICD).
    constexpr double mu  = 3.9860044e14;     // [m^3/s^2]
    constexpr double ae  = 6378136.0;        // [m]
    constexpr double J2  = 1.0826257e-3;
    constexpr double we  = 7.292115e-5;      // [rad/s]

    const GlonassEphemeris& gl = eph.glonass;

    // State [x,y,z,vx,vy,vz]; luni-solar acceleration treated as constant.
    std::array<double, 6> y{ gl.pos.x(), gl.pos.y(), gl.pos.z(),
                             gl.vel.x(), gl.vel.y(), gl.vel.z() };
    const Eigen::Vector3d acc = gl.acc;

    const auto deriv = [&](const std::array<double, 6>& s) {
        const double x = s[0], yy = s[1], z = s[2];
        const double r2 = x * x + yy * yy + z * z;
        const double r  = std::sqrt(r2);
        const double r3 = r2 * r;
        const double mur3 = mu / r3;
        const double k1 = 1.5 * J2 * mu * ae * ae / (r2 * r3);   // (3/2) J2 μ ae^2 / r^5
        const double zr2 = z * z / r2;
        std::array<double, 6> d{};
        d[0] = s[3];
        d[1] = s[4];
        d[2] = s[5];
        d[3] = -mur3 * x + k1 * x * (1.0 - 5.0 * zr2) + we * we * x + 2.0 * we * s[4] + acc.x();
        d[4] = -mur3 * yy + k1 * yy * (1.0 - 5.0 * zr2) + we * we * yy - 2.0 * we * s[3] + acc.y();
        d[5] = -mur3 * z + k1 * z * (3.0 - 5.0 * zr2) + acc.z();
        return d;
    };

    double tk = normalizeTk(continuousSeconds(time) - continuousSeconds(gl.toe));

    // Fixed-sign RK4 marching toward the query time.
    const double max_step = 30.0;
    while (std::abs(tk) > 1e-9) {
        const double h = (tk > 0.0) ? std::min(max_step, tk) : std::max(-max_step, tk);
        const auto k1 = deriv(y);
        std::array<double, 6> tmp{};
        for (int j = 0; j < 6; ++j) tmp[j] = y[j] + 0.5 * h * k1[j];
        const auto k2 = deriv(tmp);
        for (int j = 0; j < 6; ++j) tmp[j] = y[j] + 0.5 * h * k2[j];
        const auto k3 = deriv(tmp);
        for (int j = 0; j < 6; ++j) tmp[j] = y[j] + h * k3[j];
        const auto k4 = deriv(tmp);
        for (int j = 0; j < 6; ++j)
            y[j] += (h / 6.0) * (k1[j] + 2.0 * k2[j] + 2.0 * k3[j] + k4[j]);
        tk -= h;
    }

    SatelliteState out;
    out.position = Eigen::Vector3d{ y[0], y[1], y[2] };
    out.velocity = Eigen::Vector3d{ y[3], y[4], y[5] };
    out.iode = eph.iode;
    return out;
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

std::optional<LiveEphemerisStore::SatelliteState>
LiveEphemerisStore::computeSatellitePV(const std::string& sat, const GnssTime& time, int ssr_iode) const {
    BroadcastEphemeris eph;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = by_sat_.find(sat);
        if (it == by_sat_.end()) return std::nullopt;
        eph = pickForSsr(it->second, time, ssr_iode);   // copy under lock; integrate outside
    }

    // Staleness guard + EPH-PICK diagnostic.  Computed AFTER pickForSsr so the
    // IODE current/previous re-referencing is preserved.  age = query − toe with
    // BOTH operands on GPS-continuous seconds.  The BDT offset does NOT cancel
    // automatically: `time` is GPS-scale but BeiDou's kepler.toe is stored on
    // its native BDT week (BDT week 0 = GPS week 1356), so toeGpsContinuous must
    // lift it to GPS scale first — otherwise every BeiDou sat reads ~1356 weeks
    // (~26 years) stale and is wrongly skipped.  Reject an ephemeris extrapolated
    // beyond its fit window (never-refreshed sat / obs desync) rather than emit a
    // silently-wrong orbit.
    const double toe_c = toeGpsContinuous(eph);
    const double age = continuousSeconds(time) - toe_c;
    if (!std::isfinite(age) || std::abs(age) > maxEphAgeForSystem(eph.system)) {
        if (auto lg = spdlog::get("rt_engine"))
            lg->warn("EPH-STALE {} REJECTED: age={:+.0f}s ({:+.2f}h from toe) exceeds {:.1f}h "
                     "fit window — sat skipped", sat, age, age / 3600.0,
                     maxEphAgeForSystem(eph.system) / 3600.0);
        return std::nullopt;
    }
    if (auto lg = spdlog::get("rt_engine"); lg && lg->should_log(spdlog::level::trace))
        lg->trace("EPH-PICK {} iode={} ssr_iode={} age={:+.0f}s ({:+.2f}h from toe)",
                  sat, eph.iode, ssr_iode, age, age / 3600.0);

    if (eph.system == System::GLONASS) {
        return glonassState(eph, time);
    }

    // Keplerian: central finite difference for velocity (reused RAC basis).
    constexpr double dt = 0.5;
    const GnssTime tm{ time.week, time.tow - dt };
    const GnssTime tp{ time.week, time.tow + dt };
    const Eigen::Vector3d p  = keplerPosition(eph, time);
    if (p.squaredNorm() < 1.0) return std::nullopt;   // model produced nothing usable
    const Eigen::Vector3d pm = keplerPosition(eph, tm);
    const Eigen::Vector3d pp = keplerPosition(eph, tp);

    SatelliteState out;
    out.position = p;
    out.velocity = (pp - pm) / (2.0 * dt);
    out.iode = ssrMatchIode(eph);   // IGS-SSR matching convention (see helper)
    return out;
}

// ---------------------------------------------------------------------------
// Broadcast clock
// ---------------------------------------------------------------------------

std::optional<LiveEphemerisStore::SatelliteClock>
LiveEphemerisStore::computeSatelliteClock(const std::string& sat, const GnssTime& time, int ssr_iode) const {
    BroadcastEphemeris eph;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = by_sat_.find(sat);
        if (it == by_sat_.end()) return std::nullopt;
        eph = pickForSsr(it->second, time, ssr_iode);   // copy under lock; evaluate outside
    }

    // Staleness guard (mirrors computeSatellitePV; no warn here to avoid
    // double-logging the same sat/epoch the PV path already flagged): an
    // ephemeris beyond its fit window yields no clock so the satellite drops
    // rather than carrying an extrapolated wrong value.
    {
        // GPS-continuous on both sides — toeGpsContinuous lifts BeiDou's BDT toe
        // (see computeSatellitePV); a raw kepler.toe here would read ~1356 weeks
        // stale and silently drop every BeiDou clock.
        const double toe_c = toeGpsContinuous(eph);
        const double age = continuousSeconds(time) - toe_c;
        if (!std::isfinite(age) || std::abs(age) > maxEphAgeForSystem(eph.system))
            return std::nullopt;
    }

    SatelliteClock out;
    out.iode = ssrMatchIode(eph);   // IGS-SSR matching convention (see helper)

    if (eph.system == System::GLONASS) {
        // GLONASS broadcast clock: Δt = −τ_n + γ_n·(t − t_b), with the raw RTCM
        // 1020 τ_n (DF123) / γ_n (DF120) and t_b = toe.  (GLONASS is currently
        // excluded from active_systems, so this path is implemented for
        // completeness but not exercised by the live replay.)
        const double tk = normalizeTk(continuousSeconds(time)
                                    - continuousSeconds(eph.glonass.toe));
        out.bias = -eph.glonass.tau_n + eph.glonass.gamma_n * tk;
        return out;
    }

    // Keplerian (GPS / Galileo / BeiDou / QZSS): af0 + af1·dt + af2·dt², with
    // dt = t − toc on the satellite's native scale.  BeiDou toc is on BDT, so
    // the query epoch is shifted down to BDT exactly as keplerPosition does for
    // the node term — keeping the clock and orbit scales self-consistent.
    const KeplerianEphemeris& k = eph.kepler;
    double q = continuousSeconds(time);
    if (eph.system == System::BeiDou) q -= kBdtToGpsOffsetSec;
    const double dt = normalizeTk(q - continuousSeconds(k.toc));
    out.bias = k.af0 + k.af1 * dt + k.af2 * dt * dt;
    return out;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

bool LiveEphemerisStore::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_sat_.empty();
}

std::size_t LiveEphemerisStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_sat_.size();
}

void LiveEphemerisStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    by_sat_.clear();
}

std::map<std::string, int> LiveEphemerisStore::glonassFreqChannels() const {
    std::map<std::string, int> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [sat, rec] : by_sat_) {
        if (rec.current.system == System::GLONASS)
            out[sat] = rec.current.glonass.freq_channel;
    }
    return out;
}

} // namespace GnssProcessor
