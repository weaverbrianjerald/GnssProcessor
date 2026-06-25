#pragma once
/**
 * @file RtcmTypes.hpp
 * @brief Decoded message types for RTCM 3.x and IGS-SSR streams.
 *
 * The post-decode representations.  A decoder takes a framed RTCM payload
 * and produces values from these structs (or a vector of them when the
 * message carries per-satellite data).
 *
 * The SSR types map both legacy RTCM 1057-1068 (per-system message blocks)
 * and IGS-SSR 4076 (single message type with sub-types 1..7 for orbit,
 * clock, code/phase bias, URA, high-rate clock, combined orbit-clock)
 * into a single set of internal structs.  System and signal are carried
 * in the per-satellite records, not the message type.
 */

#include "common/GnssTypes.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Header fields shared by all SSR messages.
 *
 * Carries the SSR epoch time, update-interval indicator, the IOD-SSR
 * counter (used to bind orbit/clock/bias messages from one solution
 * together), and the provider/solution identifiers.
 */
struct SsrHeader {
    GnssTime epoch_time{0, 0.0};        ///< SSR epoch time (GPS time scale).
    int      update_interval_index{0};  ///< 4-bit indicator (DF391 / table-coded seconds).
    bool     multi_message{false};      ///< More SSR sub-messages follow for this epoch.
    int      iod_ssr{0};                ///< 4-bit IOD-SSR (DF413).
    int      provider_id{0};            ///< 16-bit provider identifier (DF414).
    int      solution_id{0};            ///< 4-bit solution identifier (DF415).
    System   system{System::Unknown};   ///< GNSS system this header applies to.
};

/**
 * @brief Per-satellite orbit correction in radial / along-track / cross-track frame.
 *
 * Corrections are deltas relative to the broadcast ephemeris identified
 * by @c iode.  Apply by transforming @c delta_rac into ECEF using the
 * satellite's instantaneous orbit frame (radial = position unit vector,
 * along = velocity unit vector projected onto plane normal to radial,
 * cross = radial × along) and subtracting from the broadcast position.
 */
struct SsrOrbitCorrection {
    std::string     sat;                ///< Satellite identifier (e.g. "G01", "E11").
    int             iode{0};            ///< Issue of Data, Ephemeris (DF071).
    Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};      ///< Radial/Along/Cross delta (meters).
    Eigen::Vector3d delta_rac_dot{Eigen::Vector3d::Zero()};  ///< Time derivatives (meters/sec).
    GnssTime        received_time{0, 0.0};                   ///< When the correction was received (wall-clock; staleness gate).
    GnssTime        epoch_time{0, 0.0};                      ///< SSR content reference epoch (DF; dt base for delta_rac_dot propagation).
    int             iod_ssr{0};                              ///< Provider IOD-SSR at receive time.
};

/**
 * @brief Per-satellite clock correction (polynomial in time-from-epoch).
 *
 * The clock correction in meters at time @p t is:
 *     dC(t) = c0 + c1·dt + c2·dt²,  dt = t - epoch_time
 * Apply by adding to the broadcast clock bias (also in meters).
 */
struct SsrClockCorrection {
    std::string sat;                    ///< Satellite identifier.
    double      c0{0.0};                ///< Polynomial offset (meters).
    double      c1{0.0};                ///< Polynomial drift (m/s).
    double      c2{0.0};                ///< Polynomial drift rate (m/s^2).
    GnssTime    received_time{0, 0.0};  ///< Wall-clock receive time (staleness gate).
    GnssTime    epoch_time{0, 0.0};     ///< SSR content reference epoch — dt base for dC(t)=c0+c1·dt+c2·dt² (dt = t − epoch_time).
    int         iod_ssr{0};
};

/**
 * @brief Combined orbit + clock correction (RTCM 1060/1066, IGS-SSR sub-type 7).
 *
 * Packed in a single message so orbit and clock share a consistent IOD.
 */
struct SsrCombinedCorrection {
    std::string     sat;
    int             iode{0};
    Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};
    Eigen::Vector3d delta_rac_dot{Eigen::Vector3d::Zero()};
    double          c0{0.0};
    double          c1{0.0};
    double          c2{0.0};
    GnssTime        received_time{0, 0.0};  ///< Wall-clock receive time (staleness gate).
    GnssTime        epoch_time{0, 0.0};     ///< SSR content reference epoch (shared orbit+clock dt base).
    int             iod_ssr{0};
};

/**
 * @brief Per-satellite code biases (one entry per signal).
 *
 * Bias is in meters; subtract from the corresponding observed pseudorange.
 * Signal keys are RINEX 3 observation codes (e.g. "C1C", "C2W").
 */
struct SsrCodeBias {
    std::string                    sat;
    std::map<std::string, double>  bias_per_signal;     ///< RINEX obs code -> meters.
    GnssTime                       received_time{0, 0.0};
    int                            iod_ssr{0};
};

/**
 * @brief Phase-bias entry for one (sat, signal) pair.
 *
 * For PPP-RTK ambiguity resolution the @c integer_indicator and the
 * @c discontinuity_counter are critical: they tell the rover whether
 * the bias preserves the integer property of the ambiguity and when
 * the bias jumped (forcing an ambiguity reset).
 */
struct SsrPhaseBiasEntry {
    double bias_m{0.0};                 ///< Phase bias (meters) — subtract from carrier-phase observable.
    bool   signal_integer{false};       ///< Bias preserves integer ambiguity (per IGS-SSR).
    int    wide_lane_integer{0};        ///< Wide-lane integer indicator (2 bits).
    int    discontinuity_counter{0};    ///< Increments on bias discontinuity (forces AR reset).
};

/**
 * @brief Per-satellite phase biases (one entry per signal) plus yaw metadata.
 *
 * The yaw_angle / yaw_rate fields describe the satellite attitude state
 * the provider used when computing the phase biases — needed to compensate
 * for phase wind-up consistently between the network and the rover.
 */
struct SsrPhaseBias {
    std::string sat;
    std::map<std::string, SsrPhaseBiasEntry> bias_per_signal;
    int      dispersive_bias_consistency{0};  ///< 1 bit (DF490).
    int      mw_consistency{0};               ///< 1 bit (DF491).
    double   yaw_angle_deg{0.0};              ///< Network's modelled satellite yaw (degrees).
    double   yaw_rate_deg_per_s{0.0};         ///< Network's modelled satellite yaw rate.
    GnssTime received_time{0, 0.0};
    int      iod_ssr{0};
};

/**
 * @brief Per-satellite User Range Accuracy (URA) value.
 *
 * Encoded in RTCM as a 6-bit index; this struct carries the decoded
 * 1-sigma value in meters.  Used as a quality / weighting hint by the
 * rover's stochastic model.
 */
struct SsrUra {
    std::string sat;
    double      ura_m{0.0};
    GnssTime    received_time{0, 0.0};
    int         iod_ssr{0};
};

/**
 * @brief Per-satellite high-rate clock correction (RTCM 1058/1064/etc, IGS-SSR sub-type 6).
 *
 * High-rate clocks are an additive delta on top of the standard clock
 * polynomial, updated more frequently (typically 1 s vs 5-30 s).  Apply
 * after the standard clock correction.
 */
struct SsrHighRateClock {
    std::string sat;
    double      dc{0.0};                ///< Additive clock correction delta (meters).
    GnssTime    received_time{0, 0.0};
    int         iod_ssr{0};
};

/**
 * @brief A decoded RTCM 1005/1006 stationary reference station ARP.
 *
 * Used to seed the reference-station coordinates for DGNSS / RTK base
 * streams.  1006 carries an antenna height in addition to ARP.
 */
struct RtcmStationCoords {
    int             reference_station_id{0};
    Eigen::Vector3d arp_ecef{Eigen::Vector3d::Zero()};   ///< Antenna Reference Point ECEF (m).
    double          antenna_height{0.0};                 ///< 1006 only; 1005 leaves this 0.
    bool            has_height{false};
};

} // namespace GnssProcessor
