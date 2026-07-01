#pragma once
/**
 * @file SsrCorrectionStore.hpp
 * @brief Singleton holding the most-recent SSR corrections per satellite.
 *
 * Sits beside @c ProductCache.  ProductCache holds static / batch products
 * (SP3 orbits, RINEX-CLK clocks, ATX antennas, BIA biases, ERP polar
 * motion).  SsrCorrectionStore holds streaming SSR overlays that the
 * realtime engine pushes from decoded RTCM 1057-1068 / 1240-1270 / 4076
 * frames.  Consumers (@c Geometry::getSatellitePV,
 * @c GnssCorrections::getSatelliteClockCorrection,
 * @c MeasurementModel) query the store and apply corrections on top of
 * the broadcast / SP3 baseline.  When the store is empty, all queries
 * return "no correction" and the caller falls through to broadcast-only.
 *
 * Threading: a single mutex protects the per-satellite map.  Real-time
 * pushes happen on the NTRIP receive thread; engine queries happen on
 * the per-epoch processing thread.  The mutex window covers a copy of
 * the @c SatRecord, so query callers never hold the lock during the
 * subsequent RAC-to-ECEF transform.
 */

#include "rtcm/RtcmTypes.hpp"
#include <Eigen/Dense>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

/**
 * @class SsrCorrectionStore
 * @brief Per-satellite "latest SSR record" cache, thread-safe.
 *
 * Records are keyed by satellite ID (e.g. "G01", "E11").  Each push
 * overwrites the previous value of that record type for that satellite.
 * Lookups are by satellite + maximum acceptable age in seconds; stale
 * records are reported as missing.
 */
class SsrCorrectionStore {
public:
    /// Returns the process-wide singleton instance (used by the live real-time
    /// pipeline).  Construction is the first call.
    static SsrCorrectionStore& getInstance();

    /// Default-constructible so callers can own a LOCAL instance instead of the
    /// process-wide singleton.  The network product emitter's closed-loop test
    /// (and future producer/consumer store injection) uses an owned instance to
    /// stay isolated from global state — a producer must never share the same
    /// store it consumes from, and unit tests must not pollute the singleton.
    SsrCorrectionStore() = default;
    ~SsrCorrectionStore() = default;

    SsrCorrectionStore(const SsrCorrectionStore&)            = delete;
    SsrCorrectionStore& operator=(const SsrCorrectionStore&) = delete;
    SsrCorrectionStore(SsrCorrectionStore&&)                 = delete;
    SsrCorrectionStore& operator=(SsrCorrectionStore&&)      = delete;

    // -----------------------------------------------------------------
    // Push API.  Each method overwrites that satellite's previous record
    // of the same type.  @p header carries the SSR epoch and IOD-SSR
    // shared by every per-satellite record in the message.
    // -----------------------------------------------------------------
    void pushOrbit(const SsrOrbitCorrection& rec, const SsrHeader& header);
    void pushClock(const SsrClockCorrection& rec, const SsrHeader& header);
    void pushCombined(const SsrCombinedCorrection& rec, const SsrHeader& header);
    void pushCodeBias(const SsrCodeBias& rec, const SsrHeader& header);
    void pushPhaseBias(const SsrPhaseBias& rec, const SsrHeader& header);
    void pushUra(const SsrUra& rec, const SsrHeader& header);
    void pushHighRateClock(const SsrHighRateClock& rec, const SsrHeader& header);

    /**
     * @brief Aggregated orbit + clock correction for one satellite.
     *
     * Built on demand by merging the satellite's most recent orbit
     * (or combined) record with its most recent clock record.  When a
     * combined record is present and not stale, it wins over separate
     * orbit + clock messages.
     */
    struct OrbitClockCorrection {
        Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};      ///< Radial / along-track / cross-track delta (m).
        Eigen::Vector3d delta_rac_dot{Eigen::Vector3d::Zero()};  ///< RAC velocity delta (m/s).
        double          c0{0.0};                                  ///< Clock C0 (m).
        double          c1{0.0};                                  ///< Clock C1 (m/s).
        double          c2{0.0};                                  ///< Clock C2 (m/s^2).
        double          hr_clock{0.0};                            ///< High-rate clock delta (m); 0 if not present.
        GnssTime        epoch_time{0, 0.0};                       ///< SSR content reference epoch of the CLOCK correction — dt base for applyClockToBias.
        GnssTime        orbit_epoch_time{0, 0.0};                 ///< SSR content reference epoch of the ORBIT correction — dt base for applyOrbitToPosition (equals epoch_time for a combined record).
        int             iode{0};                                  ///< Broadcast IODE this correction applies to.
        bool            has_orbit{false};                         ///< True if orbit fields are valid.
        bool            has_clock{false};                         ///< True if clock fields are valid.
    };

    /**
     * @brief Look up the merged orbit+clock correction for a satellite.
     * @param sat        Satellite ID (e.g. "G01").
     * @param query_time Reference time of the query, used for the staleness check.
     * @param max_age_s  Maximum acceptable age (query_time - record_time) in seconds.
     *                   Defaults to 60 s — typical IGS-RTS update interval.
     * @return Populated correction, or @c std::nullopt if there is no
     *         record for this satellite or all records are stale.
     */
    std::optional<OrbitClockCorrection> getOrbitClock(
        const std::string& sat,
        const GnssTime&    query_time,
        double             max_age_s = 60.0) const;

    /**
     * @brief Look up the code bias for a (sat, signal) pair.
     * @return Bias in meters, or 0.0 if absent.
     */
    double getCodeBias(const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Test whether the stream supplies a code bias for a (sat, signal).
     *
     * Presence check mirroring @c getCodeBias's lookup: true iff the satellite
     * has a code-bias record whose @c bias_per_signal map contains the EXACT
     * RINEX obs code.  A present-but-zero entry counts as covered — the
     * provider's bias set defines that signal's datum frame regardless of the
     * numeric value, so an estimator must not also estimate it.  Distinct from
     * @c getCodeBias != 0.0, which cannot tell a genuine-zero bias from an
     * absent signal.
     *
     * Used by @c MeasurementModel to suppress redundant per-(sat, signal)
     * sat-bias STATE allocation for signals the live stream already corrects;
     * it must agree exactly with @c getCodeBias so a covered signal is never
     * both estimated AND SSR-corrected on the same row.
     *
     * @param sat            Satellite ID (e.g. "G01").
     * @param rinex_obs_code Exact RINEX obs code (e.g. "C1C", "C5Q").
     * @return true if the stream carries a code bias for this exact pair.
     */
    bool hasCodeBias(const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Look up the phase bias entry for a (sat, signal) pair.
     */
    std::optional<SsrPhaseBiasEntry> getPhaseBias(
        const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Look up the network code-bias discontinuity counter for a (sat,
     *        signal) pair.
     *
     * Returns @c std::nullopt when the stream carries no code bias for the pair
     * OR carries one without a discontinuity counter (every RTCM/IGS-SSR-decoded
     * code bias — the wire format has no such field).  A value is present only
     * for biases emitted by the network producer (@c NetworkProductEmitter) with
     * @c network.estimate_sat_code_bias on.  The consumer uses an increment of
     * this counter to re-inflate the affected receiver code-bias state when the
     * producer's code-bias datum re-references, the analog of the phase-bias
     * discontinuity → ambiguity reset.
     *
     * @param sat            Satellite ID (e.g. "G01").
     * @param rinex_obs_code Exact RINEX obs code (e.g. "C1C", "C5Q").
     * @return The counter, or @c std::nullopt if untracked.
     */
    std::optional<int> getCodeBiasDiscontinuity(
        const std::string& sat, const std::string& rinex_obs_code) const;

    /// SSR reference epoch of the most recent record pushed for @p sat
    /// (from the message header), or @c std::nullopt if none.
    std::optional<GnssTime> getSsrEpoch(const std::string& sat) const;

    /// @brief Freshest SSR content epoch across ALL satellites in the store.
    ///
    /// Returns the maximum message header epoch_time held for any satellite, or
    /// @c std::nullopt when the store is empty / no header has been set.  Unlike
    /// @c getSsrEpoch (per-satellite) this is the stream-wide "newest correction
    /// content received" signal.  The real-time obs↔SSR sync uses it to bound how
    /// far ahead of the corrections the obs stream may run: an obs epoch newer
    /// than (newestSsrEpoch + ssr_max_age) has no usable SSR for any satellite and
    /// is held until the stream advances (see RealTimeEngine obs↔SSR sync).
    std::optional<GnssTime> newestSsrEpoch() const;

    /// IOD-SSR of the most recent record pushed for @p sat, or -1 if none.
    int getSsrIod(const std::string& sat) const;

    /// True iff no records have been pushed.
    bool isEmpty() const;

    /// Number of satellites with at least one SSR record.
    std::size_t size() const;

    /**
     * @brief Compact per-constellation census of cached records, for diagnostics.
     *
     * Returns a string like @c "G[o8 c8 cb8 pb8] E[o4 c4 cb4 pb4] C[o4 c4 cb0 pb4]"
     * where each constellation letter is followed by the count of satellites
     * carrying an orbit (or combined), a clock (or combined), a code bias, and a
     * phase bias record respectively.  Used by the realtime startup gate to
     * localise an SSR problem to decode/store (counts are zero) vs. application
     * (counts are non-zero but clocks still don't apply — an IODE-match issue).
     */
    std::string censusSummary() const;

    /**
     * @brief Per-epoch SSR coverage for a specific set of satellites.
     *
     * Unlike censusSummary() (which tallies the WHOLE store), this counts only
     * @p sats — the satellites actually being modeled this epoch — so the
     * realtime engine can log how many sats in the solution have a FRESH SSR
     * orbit / clock and a code / phase bias available.  "Available" means a
     * record is present within @p max_age_s; whether the orbit/clock additionally
     * *applies* depends on the IODE gate (see getSatelliteClockCorrection), so
     * the engine pairs this with the true clock-applied (cm-grade) count.
     */
    struct Coverage {
        int total      = 0;   ///< Satellites queried.
        int orbit      = 0;   ///< With a fresh SSR orbit (or combined) record.
        int clock      = 0;   ///< With a fresh SSR clock (or combined / high-rate) record.
        int code_bias  = 0;   ///< With a code-bias record.
        int phase_bias = 0;   ///< With a phase-bias record.
    };
    Coverage coverageFor(const std::vector<std::string>& sats,
                         const GnssTime& time, double max_age_s = 60.0) const;

    /// Drop every record.
    void clear();

    /**
     * @brief Apply an orbit RAC delta to a broadcast ECEF position.
     *
     * Frame convention (RTCM 10403.3 sec. 3.5.13):
     *   e_along  = vel / |vel|
     *   e_cross  = (pos × vel) / |pos × vel|
     *   e_radial = e_along × e_cross
     *
     * The propagated delta (delta_rac + delta_rac_dot · dt) is rotated
     * into ECEF and SUBTRACTED from @p brdc_pos to yield the precise
     * position (RTCM defines delta = broadcast − precise).
     */
    static Eigen::Vector3d applyOrbitToPosition(
        const Eigen::Vector3d& brdc_pos,
        const Eigen::Vector3d& brdc_vel,
        const Eigen::Vector3d& delta_rac,
        const Eigen::Vector3d& delta_rac_dot,
        double                 dt);

    /**
     * @brief Combine clock polynomial and high-rate delta into seconds.
     * @return Clock correction in seconds; ADD to broadcast clock bias.
     */
    static double applyClockToBias(double c0, double c1, double c2, double hr_clock, double dt);

    /**
     * @brief Decide whether an SSR record is safe to apply to a given broadcast ephemeris.
     *
     * SSR corrections are deltas anchored to a specific broadcast
     * ephemeris record identified by its IODE.  When the rover's
     * broadcast nav has rolled to a newer IODE before the SSR provider
     * caught up (or vice versa), applying the SSR delta would land on
     * the wrong baseline orbit/clock and produce a degraded answer.
     *
     * The check is intentionally permissive when either side is
     * @c -1 ("unknown"):
     *   - SP3-derived @c OrbitData carries @c iode=-1, so post-processing
     *     paths (which never actually ought to apply SSR) silently
     *     pass the gate today.  Tightening this is one of the things
     *     a future broadcast-nav parser will enable.
     *   - Some SSR providers omit the IODE for certain message subtypes
     *     (rare).
     *
     * @param broadcast_iode  IODE of the closest broadcast-nav sample, or -1 if unknown.
     * @param ssr_iode        IODE the SSR record was issued against, or -1 if unknown.
     * @return true if the correction may be applied; false only when
     *         both IODEs are known (>= 0) and they differ.
     */
    static bool iodeMatches(int broadcast_iode, int ssr_iode) {
        if (broadcast_iode < 0 || ssr_iode < 0) return true;
        return broadcast_iode == ssr_iode;
    }

private:
    struct SatRecord {
        std::optional<SsrOrbitCorrection>    orbit;
        std::optional<SsrClockCorrection>    clock;
        std::optional<SsrCombinedCorrection> combined;
        std::optional<SsrUra>                ura;
        std::optional<SsrHighRateClock>      hrclock;
        std::optional<SsrCodeBias>           code_bias;
        std::optional<SsrPhaseBias>          phase_bias;
        SsrHeader                            header{};   ///< Header of the most recent push (epoch/IOD-SSR).
    };

    mutable std::mutex                       mutex_;
    std::unordered_map<std::string, SatRecord> by_sat_;
};

} // namespace GnssProcessor
