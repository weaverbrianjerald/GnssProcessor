#pragma once
/**
 * @file BroadcastEphemerisTypes.hpp
 * @brief Decoded broadcast-ephemeris records for the live RTCM nav stream.
 *
 * These are the post-decode shapes produced by @c RtcmNav::decodeBroadcast
 * (RTCM 1019 / 1020 / 1042 / 1044 / 1046) and consumed by
 * @c LiveEphemerisStore::computeSatellitePV.  They are the real-time
 * analogue of the precise @c OrbitData that @c ProductCache serves from
 * SP3 files: when no SP3 orbit is loaded (live rooftop mode), satellite
 * positions are computed from these elements and then corrected by the
 * SSR orbit/clock overlay in @c SsrCorrectionStore.
 *
 * The four Keplerian constellations (GPS, Galileo, BeiDou, QZSS) share one
 * orbit model and therefore one @c KeplerianEphemeris struct; GLONASS is
 * integrated from a PZ-90 state vector and has its own struct.  Angular
 * elements are stored in radians (the RTCM semicircle scaling is applied at
 * decode time).  Times follow the GPS week/TOW convention, with one
 * exception: the BeiDou @c toe/@c toc stay on the native BDT scale
 * (BDT = GPS − 14 s), and the consumer converts the query epoch to BDT so the
 * node-rotation term @c ωₑ·toe stays self-consistent (see
 * LiveEphemerisStore::computeSatellitePV).
 */

#include "common/GnssTypes.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <string>

namespace GnssProcessor {

/**
 * @brief Keplerian broadcast elements (GPS / Galileo / BeiDou / QZSS).
 *
 * Field set and units follow RTCM 10403.3 messages 1019/1042/1044/1046
 * after scale factors and the semicircle→radian conversion are applied.
 * For BeiDou, @c toe / @c toc are on the BDT time scale and the elements
 * refer to CGCS2000; for the others the times are on the GPS scale
 * (Galileo GST time-of-week aligns with GPS).
 */
struct KeplerianEphemeris {
    GnssTime toe{0, 0.0};        ///< Ephemeris reference epoch (native scale: GPS, or BDT for BeiDou).
    GnssTime toc{0, 0.0};        ///< Clock reference epoch (native scale).

    double sqrt_a{0.0};          ///< Square root of semi-major axis [sqrt(m)].
    double e{0.0};               ///< Eccentricity [-].
    double i0{0.0};              ///< Inclination at @c toe [rad].
    double idot{0.0};            ///< Rate of inclination [rad/s].
    double omega0{0.0};          ///< Longitude of ascending node at weekly epoch [rad].
    double omega_dot{0.0};       ///< Rate of right ascension [rad/s].
    double omega{0.0};           ///< Argument of perigee [rad].
    double m0{0.0};              ///< Mean anomaly at @c toe [rad].
    double delta_n{0.0};         ///< Mean motion difference from computed value [rad/s].
    double crs{0.0};             ///< Sine harmonic, orbit radius [m].
    double crc{0.0};             ///< Cosine harmonic, orbit radius [m].
    double cus{0.0};             ///< Sine harmonic, argument of latitude [rad].
    double cuc{0.0};             ///< Cosine harmonic, argument of latitude [rad].
    double cis{0.0};             ///< Sine harmonic, inclination [rad].
    double cic{0.0};             ///< Cosine harmonic, inclination [rad].

    double af0{0.0};             ///< Clock bias [s].
    double af1{0.0};             ///< Clock drift [s/s].
    double af2{0.0};             ///< Clock drift rate [s/s^2].

    double tgd0{0.0};            ///< Group delay (GPS Tgd / BDS TGD1 / GAL BGD E1-E5a) [s].
    double tgd1{0.0};            ///< Second group delay (BDS TGD2 / GAL BGD E1-E5b) [s]; 0 if absent.
};

/**
 * @brief GLONASS broadcast state (PZ-90 ECEF, integrated to query time).
 *
 * GLONASS broadcasts an instantaneous position/velocity/luni-solar
 * acceleration at the reference epoch @c toe; the orbit is obtained by
 * numerically integrating the PZ-90 equations of motion.  Stored here in
 * SI units (the RTCM 1020 km, km/s and km/s² scaling is already applied).
 *
 * @note The broadcast @c toe (DF109 tb) is a GLONASS time of day with no
 *       date; the 1020 decoder promotes it to a full GPS time by anchoring
 *       on the host's current GPS time (TimeUtils::glonassTodToGpsTime, the
 *       same converter the MSM observation path uses).
 */
struct GlonassEphemeris {
    GnssTime        toe{0, 0.0};                       ///< Reference epoch (GPS scale, day anchored on the host clock).
    Eigen::Vector3d pos{Eigen::Vector3d::Zero()};      ///< PZ-90 ECEF position [m].
    Eigen::Vector3d vel{Eigen::Vector3d::Zero()};      ///< PZ-90 ECEF velocity [m/s].
    Eigen::Vector3d acc{Eigen::Vector3d::Zero()};      ///< Luni-solar acceleration [m/s^2].
    double          tau_n{0.0};                        ///< SV clock bias (−τ) [s].
    double          gamma_n{0.0};                      ///< Relative frequency offset [-].
    int             freq_channel{0};                   ///< FDMA frequency channel k ∈ [−7, +6].
};

/**
 * @brief One decoded broadcast-ephemeris record for a single satellite.
 *
 * Exactly one of @c kepler / @c glonass is meaningful, selected by
 * @c system.  @c iode is the raw broadcast issue-of-data as transmitted
 * (GPS/QZSS 8-bit IODE, Galileo 10-bit IODnav, BeiDou 5-bit AODE); −1 means
 * unknown.  The IGS-SSR overlay does not gate against this raw value: the
 * IGS-SSR "GNSS IOD" (IDF012) is an 8-bit field with a per-constellation
 * definition (GAL = low 8 bits of IODnav, BDS = mod(toe/720,240), GPS = the
 * 8-bit IODE).  @c LiveEphemerisStore maps this raw field onto that matching
 * convention (see @c ssrMatchIode) when it reports the IODE to the SSR gate.
 */
struct BroadcastEphemeris {
    std::string satellite;                  ///< Satellite identifier (e.g. "G01", "E11", "C08").
    System      system{System::Unknown};    ///< Constellation this record belongs to.
    int         iode{-1};                   ///< Issue of data, ephemeris (−1 = unknown).
    int         health{0};                  ///< SV health (0 = healthy; system-specific encoding).
    /// The Galileo "data sources" word (RINEX ORBIT-5 field 2): bit 0 I/NAV E1-B,
    /// bit 1 F/NAV E5a-I, bit 2 I/NAV E5b-I, bit 8 clock and group delay for (E5a, E1),
    /// bit 9 for (E5b, E1).  −1 for every other system and for a record whose source
    /// carries no such word (the RTCM decoders).  (Card OWD, O3.)
    int         data_source{-1};
    GnssTime    received_time{0, 0.0};      ///< Wall-clock receive time (GPS scale) for staleness.
    /// GPS / QZSS issue of data, clock (RINEX ORBIT-6 field 4); −1 for every other
    /// system and for a record decoded from a stream that carries no such field.
    /// (Card NBP.)
    int         iodc{-1};
    /// Transmission time of the message (RINEX ORBIT-7 field 1), on the record's native
    /// week scale (BDT for BeiDou), reduced to within half a week of the clock reference
    /// epoch @c kepler.toc (RINEX permits a value referred to the neighbouring week).
    /// Valid only when @ref has_transmission_time is true.  (Card NBP: the broadcast
    /// clock a priori selects the latest record transmitted at or before the epoch.)
    GnssTime    transmission_time{0, 0.0};
    /// True when @ref transmission_time was read from the record.  False for the RTCM
    /// decoders and for the RINEX "unknown" value (0.9999E+09).
    bool        has_transmission_time{false};
    /// RINEX ORBIT-7 field 2 as written: the GPS / QZSS fit interval (hours, or the
    /// fit flag of older writers); 0 when absent.  Parsed for completeness, not used.
    double      fit_interval{0.0};

    KeplerianEphemeris kepler;              ///< Valid when @c system is GPS/Galileo/BeiDou/QZSS.
    GlonassEphemeris   glonass;             ///< Valid when @c system is GLONASS.
};

/**
 * @brief A Keplerian record's clock reference epoch (toc) on the continuous GPS time scale.
 *
 * week x 604800 + seconds of week; a BeiDou toc (native BDT) is shifted by 1356 weeks plus
 * 14 s.  The single definition behind the network's broadcast staging
 * (@c navAvailabilityGpsSeconds) and its group-delay latch (card OWD).
 *
 * @param e A GPS / Galileo / BeiDou / QZSS record.
 * @return Its toc in continuous GPS seconds.
 */
inline double brdcTocGpsSeconds(const BroadcastEphemeris& e) {
    double c = static_cast<double>(e.kepler.toc.week) * 604800.0 + e.kepler.toc.tow;
    if (e.system == System::BeiDou) c += 1356.0 * 604800.0 + 14.0;   // BDT week 0 = GPS week 1356, BDT = GPS − 14 s
    return c;
}

/**
 * @brief A Keplerian record's transmission time on the continuous GPS time scale.
 *
 * week x 604800 + seconds of week of @c BroadcastEphemeris::transmission_time, with the
 * BeiDou shift of @ref brdcTocGpsSeconds.  Meaningful only when
 * @c has_transmission_time is true.  The availability instant of a record under the
 * broadcast clock a priori of card NBP (@c network.brdc_select ttom).
 *
 * @param e A GPS / Galileo / BeiDou / QZSS record.
 * @return Its transmission time in continuous GPS seconds.
 */
inline double brdcTransmissionGpsSeconds(const BroadcastEphemeris& e) {
    double c = static_cast<double>(e.transmission_time.week) * 604800.0 + e.transmission_time.tow;
    if (e.system == System::BeiDou) c += 1356.0 * 604800.0 + 14.0;
    return c;
}

} // namespace GnssProcessor
