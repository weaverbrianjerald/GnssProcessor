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
 * @brief Satellite reference point an SSR orbit correction is expressed for.
 *
 * The two SSR dialects disagree on WHERE the corrected orbit points:
 *   - Legacy RTCM SSR (1057/1060, 1240-series, …) corrects the broadcast
 *     orbit to the satellite ANTENNA PHASE CENTER (APC) — RTCM 10403.3
 *     sec. 3.5.12: "the reference point … is the antenna phase centre".
 *   - IGS-SSR (RTCM 4076) corrects it to the CENTER OF MASS (CoM) — IGS
 *     SSR Format v1.00 sec. 4 — matching SP3/CLK convention, so the user
 *     model must still apply the satellite PCO (ATX z-offset) itself.
 *
 * @c PppModel applies the satellite PCO unconditionally (correct for
 * SP3/CoM and 4076/CoM baselines).  When an APC-referenced legacy-SSR
 * orbit is in effect that PCO term double-counts the z-offset (0.6-2.6 m
 * along the LOS); the tag lets the model skip it (see
 * @c realtime.honor_ssr_reference_point).  Default is @c CoM so every
 * existing construction (network products, tests) is unchanged.
 */
enum class SsrReferencePoint {
    CoM,  ///< Corrected orbit points at the satellite center of mass (IGS-SSR 4076, SP3 convention).
    APC   ///< Corrected orbit points at the satellite antenna phase center (legacy RTCM SSR).
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
    int             iode{-1};           ///< Issue of Data, Ephemeris (DF071), IGS-SSR matching convention; -1 = unknown → iodeMatches() permissive (RTF-2/RT0-1 sibling: decoders always set it explicitly, so the default only affects unstamped test/product constructions).
    Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};      ///< Radial/Along/Cross delta (meters).
    Eigen::Vector3d delta_rac_dot{Eigen::Vector3d::Zero()};  ///< Time derivatives (meters/sec).
    GnssTime        received_time{0, 0.0};                   ///< When the correction was received (wall-clock; staleness gate).
    GnssTime        epoch_time{0, 0.0};                      ///< SSR content reference epoch (DF; dt base for delta_rac_dot propagation).
    int             iod_ssr{0};                              ///< Provider IOD-SSR at receive time.
    /// Satellite reference point of the CORRECTED orbit (RTB-5): @c APC for
    /// legacy 1057/1240-series decodes, @c CoM for IGS-SSR 4076.  Default
    /// @c CoM keeps every non-decoder construction (network products, tests)
    /// unchanged.  Consumed via @c SsrCorrectionStore::OrbitClockCorrection.
    SsrReferencePoint reference_point{SsrReferencePoint::CoM};
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
    /// Network clock discontinuity counter.  0 for RTCM/IGS-SSR-decoded clocks
    /// (the wire standard has no such field) and whenever the network producer's
    /// clock-disc channel (@c network.emit_clock_disc) is off — an internal
    /// extension populated ONLY by @c NetworkProductEmitter.  Increments when the
    /// producer's per-constellation clock DATUM re-references (a clock-pivot
    /// handover for THIS satellite's constellation).  NOT serialized: recomputed
    /// deterministically on replay from the per-epoch datum-change signal (mirrors
    /// the phase/code disc counters).  The consumer reacts by noting the
    /// re-reference (and, if it carries a stiff receiver-clock state, re-inflating
    /// it) — the clock analog of the phase-bias → ambiguity reset.
    int         disc_counter{0};
    /// RTF-2 (Card 3): broadcast ephemeris version (IODE, in the IGS-SSR matching
    /// convention) the producer's C0 baseline was referenced to.  -1 = unknown:
    /// the RTCM/IGS-SSR wire clock message carries NO IODE, so decoded clocks
    /// leave this at -1 and @c getOrbitClock then inherits the paired ORBIT's
    /// IODE for the gate (byte-identical to the pre-card behaviour).  Stamped >= 0
    /// ONLY by the network producer in @c brdc_rts mode (C0 = a delta against
    /// broadcast(IODE)); the consumer then gates clock application on
    /// @c iodeMatches(brdc_iode, iode) instead of the orbit anchor.
    int         iode{-1};
    /// RTF-2 (Card 3): orbit coherence echo — the IOD-SSR of the RTS orbit
    /// correction this clock was estimated against.  Broadcast radial-orbit error
    /// absorbs 1:1 into the estimated clock, so the consumer verifies it applies
    /// the SAME orbit update the producer used (Card 7 echo check).  -1 = not
    /// stamped (standard SSR / non-brdc_rts).
    int         orbit_iod_ssr{-1};
    /// RTF-2 (Card 3): SSR content epoch of that referenced RTS orbit correction
    /// (the other operand of the consumer orbit/clock age-skew budget).  {0,0} =
    /// not stamped.
    GnssTime    orbit_epoch{0, 0.0};
    /// M4 (contract v2): formal 1σ (m) of the COMBINED CODE correction for this
    /// satellite (clock + code bias at a reference signal), from the network
    /// shared covariance (EWMA(σ̂0)×√Var(clk+cb), cross-covariance included).  0
    /// for every RTCM/IGS-SSR-decoded clock and when @c network.emit_combined_sigma
    /// is off — an internal extension populated ONLY by @c NetworkProductEmitter,
    /// carried on the C-record @c sigcc= wire token.  During satellite-rise
    /// reconvergence the individual clock/bias σ are cm-level but this COMBINED σ
    /// is mm-level (the components are strongly correlated), so a consumer weighting
    /// by it (rather than the individual-σ RSS) can keep using a fresh satellite's
    /// products instead of deweighting them 10–100× (Zhang 2021 / Psychas 2024b).
    double      combined_code_sigma_m{0.0};
    /// M4 (contract v2): correlation ρ (dimensionless, [-1,1]) between this
    /// satellite's COMBINED CODE and COMBINED PHASE corrections at the reference
    /// signal — both share the clock term, so ρ is typically 0.6–0.9 (Psychas Fig.
    /// 5).  0 for decoded clocks / when the channel is off; C-record @c rho= wire
    /// token.  Carried for a future correlated-measurement (full-R) consumer; the
    /// present diagonal-R consumer does not apply it (it has no off-diagonal slot).
    double      code_phase_corr{0.0};
};

/**
 * @brief Combined orbit + clock correction (RTCM 1060/1066, IGS-SSR sub-type 7).
 *
 * Packed in a single message so orbit and clock share a consistent IOD.
 */
struct SsrCombinedCorrection {
    std::string     sat;
    int             iode{-1};           ///< Issue of Data, Ephemeris; -1 = unknown → permissive (decoders set it explicitly; RTF-2/RT0-1 sibling).
    Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};
    Eigen::Vector3d delta_rac_dot{Eigen::Vector3d::Zero()};
    double          c0{0.0};
    double          c1{0.0};
    double          c2{0.0};
    GnssTime        received_time{0, 0.0};  ///< Wall-clock receive time (staleness gate).
    GnssTime        epoch_time{0, 0.0};     ///< SSR content reference epoch (shared orbit+clock dt base).
    int             iod_ssr{0};
    /// Satellite reference point of the CORRECTED orbit (RTB-5): @c APC for
    /// legacy 1060/1243-series decodes, @c CoM for IGS-SSR 4076.  Default
    /// @c CoM keeps every non-decoder construction unchanged.  The clock part
    /// is reference-point-agnostic (a scalar range bias).
    SsrReferencePoint reference_point{SsrReferencePoint::CoM};
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
    /// RINEX obs code -> network code-bias discontinuity counter.
    ///
    /// Empty for RTCM/IGS-SSR-decoded code biases — the wire standard has NO
    /// per-signal discontinuity field for CODE biases (only PHASE biases do,
    /// because only phase carries an integer property).  This is therefore an
    /// internal extension, populated ONLY by @c NetworkProductEmitter when
    /// @c network.estimate_sat_code_bias is on.  It increments when the
    /// producer's per-(constellation, signal) sum-to-zero datum re-references
    /// (a satellite for that signal rises/sets), when a satellite's
    /// ionosphere-free pin band-set changes, or on a forced (clock-pivot)
    /// datum change.  NOT serialized: recomputed deterministically on replay
    /// from the per-epoch satellite-set composition (mirrors the phase path).
    std::map<std::string, int>     disc_per_signal;
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
    /// B1b: 1σ formal uncertainty (m) of this phase-bias correction, from the network
    /// shared covariance (EWMA(σ̂0)×√var, gauge-exact).  0 for every RTCM/IGS-SSR-decoded
    /// bias and for network products emitted with @c network.emit_phase_bias_sigma off —
    /// the wire format has NO such field, so this is an internal in-process-store
    /// extension (closed-loop consumer only).  The consumer folds sigma_m² into the
    /// measurement variance of that phase row when @c network.consume_phase_bias_sigma is
    /// on (the Tienstra §6.6 phase-bias block of Qŷ_B1 the diagonal-only URA channel omits).
    double sigma_m{0.0};
    /// M4 (contract v2): formal 1σ (m) of the COMBINED PHASE correction for this
    /// (sat, signal) — clock + phase bias, cross-covariance included (NOT iono: the
    /// network iono is SH-VTEC, supplied separately via @c SsrSlantIono with its own
    /// σ; bundling it here would double-count).  Distinct from @c sigma_m (the
    /// phase-bias term ALONE): this is the honest quality of the whole satellite-side
    /// phase correction the consumer applies.  0 for decoded biases / when
    /// @c network.emit_combined_sigma is off; carried on the P-record @c sigc= wire
    /// token.  When @c closed_loop.consume_combined_sigma is on the consumer folds
    /// @c combined_sigma_m² (age-inflated) into the phase row variance INSTEAD OF the
    /// URA + @c sigma_m RSS — mm-level for a fresh satellite where the individual σ
    /// are cm-level, so a reconverging satellite is no longer deweighted off.
    double combined_sigma_m{0.0};
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
 * @brief Per-satellite UNDIFFERENCED slant ionospheric delay supplied by the
 *        network producer to a closed-loop consumer.
 *
 * The producer evaluates its co-estimated single-layer SH-VTEC field at the
 * consumer's ionospheric pierce point for each satellite, maps VTEC→slant with
 * the thin-shell obliquity, and expresses the result as a range delay on GPS L1
 * (the same reference the consumer's per-satellite slant-iono STATE uses — see
 * @c MeasurementModel's iono Jacobian, α=(f_L1/f)²).  It is emitted UNDIFFERENCED
 * per satellite (Odijk 2016 Table 11: a vertical→slant mapping intervenes at the
 * user, so it must NOT be between-satellite differenced) and carries a per-LOS
 * 1σ so the consumer can weight it as an ionosphere-weighted / BLUP prior rather
 * than trusting it as exact.  Carried in-process only (an owned
 * @c SsrCorrectionStore channel); it is NOT part of the on-disk product stream.
 */
struct SsrSlantIono {
    std::string sat;
    double      iono_m{0.0};             ///< Slant ionospheric delay on GPS L1 (m), undifferenced.
    double      sigma_m{0.0};            ///< Per-LOS representativeness 1σ (m).
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

/**
 * @brief A decoded RTCM 1033 receiver + antenna descriptor block (RTB-2(d)).
 *
 * Free-form metadata the reference station broadcasts about its own
 * equipment: the IGS antenna type (20-char "model + radome" convention),
 * setup id, serials, and the receiver type / firmware.  Surfaced on the
 * parser output and logged once at info; retained as STORAGE for a future
 * SIF (station-info) cross-check — deliberately NOT wired into the antenna
 * model or anywhere else yet.
 */
struct RtcmReceiverAntennaDescriptor {
    int         reference_station_id{0};  ///< DF003.
    std::string antenna_descriptor;       ///< DF030: IGS antenna type (e.g. "TRM59800.00     NONE").
    int         antenna_setup_id{0};      ///< DF031.
    std::string antenna_serial;           ///< DF033.
    std::string receiver_type;            ///< DF228: receiver type descriptor.
    std::string receiver_firmware;        ///< DF230: receiver firmware version.
    std::string receiver_serial;          ///< DF232.
};

/**
 * @brief Decoded RTCM 1230 GLONASS L1/L2 code-phase biases (RTB-2(d)).
 *
 * Per-station biases the reference receiver applies (or declares) between
 * its GLONASS code and phase observables, one per FDMA signal:
 * L1 C/A (DF423), L1 P (DF424), L2 C/A (DF425), L2 P (DF426), each in
 * meters at 0.02 m resolution.  @c fdma_signal_mask (DF422, 4 bits, MSB =
 * L1 C/A … LSB = L2 P) says which of the four fields were present in the
 * message; absent signals leave their bias at 0 with the mask bit clear.
 *
 * Surfaced on the parser output and logged once at info only.  NOT wired
 * into the GLONASS inter-frequency-bias (IFB) estimation path: consuming a
 * base-receiver code-phase bias in the per-FCN-slot IFB datum is an
 * RTF-scale change (it re-references the k=0 datum the network/user IFB
 * states are built on) and needs its own card.
 */
struct RtcmGlonassBias {
    int    reference_station_id{0};   ///< DF003.
    bool   code_phase_bias_indicator{false};  ///< DF421: biases ARE aligned by the receiver.
    int    fdma_signal_mask{0};       ///< DF422 (4 bits): bit3=L1 C/A, bit2=L1 P, bit1=L2 C/A, bit0=L2 P.
    double l1_ca_bias_m{0.0};         ///< DF423 (m), valid when mask bit3 set.
    double l1_p_bias_m{0.0};          ///< DF424 (m), valid when mask bit2 set.
    double l2_ca_bias_m{0.0};         ///< DF425 (m), valid when mask bit1 set.
    double l2_p_bias_m{0.0};          ///< DF426 (m), valid when mask bit0 set.
};

} // namespace GnssProcessor
