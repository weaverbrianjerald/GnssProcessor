#pragma once
/**
 * @file SyntheticNav.hpp
 * @brief Synthetic GPS and Galileo broadcast navigation records for unit tests.
 *
 * Several tests build broadcast records by hand, each with its own local helper that sets
 * a few fields (satellite, clock epoch, transmission time, health, group delay) and leaves
 * the orbit at zero.  A record with a zero orbit cannot be evaluated: the store's orbit
 * model returns no position for it, so a test of the orbit or of a reaction to a new
 * record needs a plausible orbit as well.  These builders give one place that makes a
 * complete record in the project's own type (@c BroadcastEphemeris): every field a
 * reader of the record looks at has a stated default, the orbit is a near-circular orbit
 * of the constellation's size, and each field can be set on its own.
 *
 * Times are continuous GPS seconds (week x 604800 + seconds of week), the scale of
 * @c brdcTocGpsSeconds and of the network's broadcast staging.  Galileo times are on the
 * GPS week scale, as the decoders store them.  BeiDou (BDT scale) and GLONASS (a state
 * vector, not Keplerian elements) are not covered.
 *
 * Usage:
 * @code
 *   using namespace gnss_test;
 *   const double d = syntheticNavDayStart();
 *   BroadcastEphemeris g = gpsNav(5, d + 7200.0, 37).health(63).groupDelay(-6e-9).build();
 *   auto series = syntheticNavSeries(galileoNav(11, d).build(), 6, 600.0, 2,
 *                                    setGroupDelay(-2.5e-9));   // BGD steps at the third record
 * @endcode
 * The setters return a reference to the builder, so the chain must end with
 * @c build() inside the same expression (never bind a reference to a temporary builder).
 */
#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "math/TimeUtils.hpp"
#include "products/BroadcastEphemerisTypes.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gnss_test {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// GPS week of the default synthetic day (the week of 2024-05-08).
constexpr int kSyntheticNavWeek = 2313;
/// Seconds of week of the default synthetic day's 00:00 (Wednesday).
constexpr double kSyntheticNavDaySow = 3.0 * 86400.0;

/// The default synthetic day's 00:00 in continuous GPS seconds.
inline double syntheticNavDayStart() {
    return static_cast<double>(kSyntheticNavWeek) * 604800.0 + kSyntheticNavDaySow;
}

/// A continuous GPS second as week and seconds of week (the week is the natural one).
inline GnssProcessor::GnssTime syntheticNavTime(double gps_s) {
    const double wk = std::floor(gps_s / 604800.0);
    return GnssProcessor::GnssTime{static_cast<int>(wk), gps_s - wk * 604800.0};
}

/// Week and seconds of week as continuous GPS seconds.
inline double syntheticNavSeconds(const GnssProcessor::GnssTime& t) {
    return static_cast<double>(t.week) * 604800.0 + t.tow;
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

/// Default GPS T_GD [s].
constexpr double kSyntheticGpsTgdS = -5.0e-9;
/// Default Galileo BGD(E1, E5a) [s], stored in @c tgd0 as the RINEX parser stores it.
constexpr double kSyntheticGalBgdE5aS = -2.0e-9;
/// Default Galileo BGD(E1, E5b) [s], stored in @c tgd1 as the RINEX parser stores it.
constexpr double kSyntheticGalBgdE5bS = -2.2e-9;
/// Default clock polynomial: bias [s], drift [s/s], drift rate [s/s^2].
constexpr double kSyntheticAf0S = 1.0e-4;
constexpr double kSyntheticAf1 = 1.0e-12;
constexpr double kSyntheticAf2 = 0.0;
/// Default lead of the clock epoch over the transmission time [s]: GPS uploads are
/// transmitted about two hours before their clock epoch, Galileo about ten minutes.
constexpr double kSyntheticGpsLeadS = 7200.0;
constexpr double kSyntheticGalLeadS = 600.0;
/// Galileo data-source words: F/NAV (bits 1 and 8: E5a-I, clock for (E5a, E1)) and
/// I/NAV (bits 0, 2 and 9: E1-B, E5b-I, clock for (E5b, E1)).
constexpr int kSyntheticGalFnav = 258;
constexpr int kSyntheticGalInav = 517;
/// Default orbit sizes and shapes: square root of the semi-major axis [m^1/2],
/// eccentricity and inclination [deg].
constexpr double kSyntheticGpsSqrtA = 5153.7;      // a about 26 560 km
constexpr double kSyntheticGpsE     = 0.002;
constexpr double kSyntheticGpsIncDeg = 55.0;
constexpr double kSyntheticGalSqrtA = 5440.6;      // a about 29 600 km
constexpr double kSyntheticGalE     = 0.0005;
constexpr double kSyntheticGalIncDeg = 56.0;

// ---------------------------------------------------------------------------
// One record
// ---------------------------------------------------------------------------

/**
 * @brief A broadcast record under construction.
 *
 * Holds the record and offers one setter per field a test commonly varies.  The clock
 * epoch (toc) is fixed by the constructing function (@ref gpsNav, @ref galileoNav); the
 * ephemeris epoch (toe) equals it unless @ref toe moves it.
 */
struct SyntheticNavRecord {
    GnssProcessor::BroadcastEphemeris record;   ///< The record as built so far.

    /// Issue of data (GPS IODE, also written as IODC; Galileo IODnav).
    SyntheticNavRecord& iode(int v) {
        record.iode = v;
        if (record.system == GnssProcessor::System::GPS
            || record.system == GnssProcessor::System::QZSS)
            record.iodc = v;
        return *this;
    }
    /// SV health word (0 healthy).
    SyntheticNavRecord& health(int v) { record.health = v; return *this; }
    /// The first group delay [s]: GPS T_GD, Galileo BGD(E1, E5a).
    SyntheticNavRecord& groupDelay(double s) { record.kepler.tgd0 = s; return *this; }
    /// The second group delay [s]: Galileo BGD(E1, E5b); GPS records keep 0 here.
    SyntheticNavRecord& secondGroupDelay(double s) { record.kepler.tgd1 = s; return *this; }
    /// Clock polynomial af0 [s], af1 [s/s], af2 [s/s^2] about the clock epoch.
    SyntheticNavRecord& clock(double af0, double af1 = 0.0, double af2 = 0.0) {
        record.kepler.af0 = af0;
        record.kepler.af1 = af1;
        record.kepler.af2 = af2;
        return *this;
    }
    /// Ephemeris reference epoch, continuous GPS seconds.
    SyntheticNavRecord& toe(double gps_s) { record.kepler.toe = syntheticNavTime(gps_s); return *this; }
    /// Transmission time, continuous GPS seconds.
    SyntheticNavRecord& transmittedAt(double gps_s) {
        record.transmission_time     = syntheticNavTime(gps_s);
        record.has_transmission_time = true;
        return *this;
    }
    /// No transmission time (as the RTCM decoders deliver a record).
    SyntheticNavRecord& noTransmissionTime() {
        record.transmission_time     = GnssProcessor::GnssTime{0, 0.0};
        record.has_transmission_time = false;
        return *this;
    }
    /// Galileo data-source word (-1 for none).
    SyntheticNavRecord& dataSource(int word) { record.data_source = word; return *this; }
    /// Receive time, continuous GPS seconds.
    SyntheticNavRecord& receivedAt(double gps_s) { record.received_time = syntheticNavTime(gps_s); return *this; }
    /// The finished record (a copy).
    GnssProcessor::BroadcastEphemeris build() const { return record; }
};

namespace synthetic_nav_detail {

inline std::string satId(char sys, int prn) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d", sys, prn);
    return buf;
}

inline double radians(double deg) { return deg * GnssProcessor::Constants::PI / 180.0; }

/// Near-circular orbit: the plane and the phase in it spread by PRN so that two
/// satellites never share a position.
inline void setOrbit(GnssProcessor::KeplerianEphemeris& k, int prn, double sqrt_a, double e,
                     double inc_deg, int planes) {
    const double two_pi = 2.0 * GnssProcessor::Constants::PI;
    k.sqrt_a    = sqrt_a;
    k.e         = e;
    k.i0        = radians(inc_deg);
    k.omega0    = two_pi * static_cast<double>(prn % planes) / static_cast<double>(planes);
    k.m0        = std::fmod(0.7 * static_cast<double>(prn), two_pi);
    k.omega     = 0.0;
    k.omega_dot = -8.0e-9;
    k.idot      = 0.0;
    k.delta_n   = 0.0;
    k.crs = k.crc = k.cus = k.cuc = k.cis = k.cic = 0.0;
}

}  // namespace synthetic_nav_detail

/**
 * @brief A healthy GPS LNAV record of satellite G<prn>.
 *
 * Defaults: toe = toc = @p toc_gps_s; transmitted @ref kSyntheticGpsLeadS before toc;
 * IODE = IODC = @p iode; health 0; T_GD @ref kSyntheticGpsTgdS; clock
 * (@ref kSyntheticAf0S, @ref kSyntheticAf1, @ref kSyntheticAf2); fit interval 4 h;
 * a near-circular orbit of radius about 26 560 km, inclination 55 degrees.
 *
 * @param prn       Satellite number (1 to 32).
 * @param toc_gps_s Clock reference epoch, continuous GPS seconds.
 * @param iode      Issue of data.
 * @return The builder.
 */
inline SyntheticNavRecord gpsNav(int prn, double toc_gps_s, int iode = 1) {
    SyntheticNavRecord b;
    GnssProcessor::BroadcastEphemeris& e = b.record;
    e.satellite    = synthetic_nav_detail::satId('G', prn);
    e.system       = GnssProcessor::System::GPS;
    e.iode         = iode;
    e.iodc         = iode;
    e.health       = 0;
    e.data_source  = -1;
    e.fit_interval = 4.0;
    e.kepler.toc   = syntheticNavTime(toc_gps_s);
    e.kepler.toe   = syntheticNavTime(toc_gps_s);
    e.kepler.af0   = kSyntheticAf0S;
    e.kepler.af1   = kSyntheticAf1;
    e.kepler.af2   = kSyntheticAf2;
    e.kepler.tgd0  = kSyntheticGpsTgdS;
    e.kepler.tgd1  = 0.0;
    synthetic_nav_detail::setOrbit(e.kepler, prn, kSyntheticGpsSqrtA, kSyntheticGpsE,
                                   kSyntheticGpsIncDeg, 6);
    b.transmittedAt(toc_gps_s - kSyntheticGpsLeadS);
    return b;
}

/**
 * @brief A healthy Galileo F/NAV record of satellite E<prn>.
 *
 * Defaults: toe = toc = @p toc_gps_s; transmitted @ref kSyntheticGalLeadS before toc;
 * IODnav @p iodnav; health 0; data-source word @ref kSyntheticGalFnav; BGD(E1, E5a)
 * @ref kSyntheticGalBgdE5aS in @c tgd0 and BGD(E1, E5b) @ref kSyntheticGalBgdE5bS in
 * @c tgd1; clock as for GPS; a near-circular orbit of radius about 29 600 km,
 * inclination 56 degrees.  An I/NAV record is the same with
 * @c dataSource(kSyntheticGalInav).
 *
 * @param prn       Satellite number (1 to 36).
 * @param toc_gps_s Clock reference epoch, continuous GPS seconds (GPS week scale).
 * @param iodnav    Issue of data (10 bits).
 * @return The builder.
 */
inline SyntheticNavRecord galileoNav(int prn, double toc_gps_s, int iodnav = 1) {
    SyntheticNavRecord b;
    GnssProcessor::BroadcastEphemeris& e = b.record;
    e.satellite   = synthetic_nav_detail::satId('E', prn);
    e.system      = GnssProcessor::System::Galileo;
    e.iode        = iodnav;
    e.iodc        = -1;
    e.health      = 0;
    e.data_source = kSyntheticGalFnav;
    e.kepler.toc  = syntheticNavTime(toc_gps_s);
    e.kepler.toe  = syntheticNavTime(toc_gps_s);
    e.kepler.af0  = kSyntheticAf0S;
    e.kepler.af1  = kSyntheticAf1;
    e.kepler.af2  = kSyntheticAf2;
    e.kepler.tgd0 = kSyntheticGalBgdE5aS;
    e.kepler.tgd1 = kSyntheticGalBgdE5bS;
    synthetic_nav_detail::setOrbit(e.kepler, prn, kSyntheticGalSqrtA, kSyntheticGalE,
                                   kSyntheticGalIncDeg, 3);
    b.transmittedAt(toc_gps_s - kSyntheticGalLeadS);
    return b;
}

// ---------------------------------------------------------------------------
// A series of records
// ---------------------------------------------------------------------------

/// A change applied to a record of a series (see @ref syntheticNavSeries).
using NavChange = std::function<void(GnssProcessor::BroadcastEphemeris&)>;

/// A change that sets the first group delay (GPS T_GD, Galileo BGD(E1, E5a)) [s].
inline NavChange setGroupDelay(double s) {
    return [s](GnssProcessor::BroadcastEphemeris& e) { e.kepler.tgd0 = s; };
}

/// A change that sets the health word.
inline NavChange setHealth(int h) {
    return [h](GnssProcessor::BroadcastEphemeris& e) { e.health = h; };
}

/// A change that sets the clock bias af0 [s].
inline NavChange setClockBias(double af0) {
    return [af0](GnssProcessor::BroadcastEphemeris& e) { e.kepler.af0 = af0; };
}

/**
 * @brief @p count successive uploads of one satellite, @p step_s apart, with one change
 *        from a chosen record on.
 *
 * Record k is @p first with toc, toe, the transmission time (when present) and the
 * receive time (when set) moved by k x @p step_s, and the issue of data advanced by k
 * (GPS IODE modulo 256 with IODC modulo 1024, Galileo IODnav modulo 1024; a negative
 * issue of data stays as it is).  From record @p change_from on (0-based), @p change is
 * applied after the move; with @p change_from negative or no change, every record keeps
 * @p first's other fields.
 *
 * @param first       The first record.
 * @param count       Number of records.
 * @param step_s      Spacing of the uploads [s].
 * @param change_from Index of the first changed record (-1 for none).
 * @param change      The change.
 * @return The records in time order.
 */
inline std::vector<GnssProcessor::BroadcastEphemeris>
syntheticNavSeries(const GnssProcessor::BroadcastEphemeris& first, int count, double step_s,
                   int change_from = -1, const NavChange& change = NavChange{}) {
    using GnssProcessor::System;
    std::vector<GnssProcessor::BroadcastEphemeris> out;
    out.reserve(count > 0 ? static_cast<std::size_t>(count) : 0u);
    const bool gps_like = first.system == System::GPS || first.system == System::QZSS;
    const int iode_mod = gps_like ? 256 : 1024;
    for (int k = 0; k < count; ++k) {
        GnssProcessor::BroadcastEphemeris e = first;
        const double shift = static_cast<double>(k) * step_s;
        e.kepler.toc = syntheticNavTime(syntheticNavSeconds(first.kepler.toc) + shift);
        e.kepler.toe = syntheticNavTime(syntheticNavSeconds(first.kepler.toe) + shift);
        if (first.has_transmission_time)
            e.transmission_time = syntheticNavTime(syntheticNavSeconds(first.transmission_time) + shift);
        if (first.received_time.week != 0 || first.received_time.tow != 0.0)
            e.received_time = syntheticNavTime(syntheticNavSeconds(first.received_time) + shift);
        if (first.iode >= 0) e.iode = (first.iode + k) % iode_mod;
        if (gps_like && first.iodc >= 0) e.iodc = (first.iodc + k) % 1024;
        if (change_from >= 0 && k >= change_from && change) change(e);
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace gnss_test
