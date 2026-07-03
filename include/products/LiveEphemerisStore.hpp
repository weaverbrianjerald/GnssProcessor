#pragma once
/**
 * @file LiveEphemerisStore.hpp
 * @brief Singleton holding the most-recent broadcast ephemeris per satellite.
 *
 * The live-stream analogue of the SP3-backed orbit path in @c ProductCache:
 * the realtime engine pushes broadcast ephemeris decoded from the receiver's
 * RTCM 1019/1020/1042/1044/1046 messages, and @c Geometry::getSatellitePV
 * consults this store as a FALLBACK when no SP3 orbit is loaded.  The
 * computed broadcast position/velocity is then corrected by the SSR
 * orbit/clock overlay in @c SsrCorrectionStore, gated by the IODE this
 * store reports — exactly the broadcast-plus-SSR baseline PPP-RTK expects.
 *
 * Why a separate singleton (not @c ProductCache): ProductCache is a
 * load-once, atomically-populated batch cache (SP3 / CLK / ATX / BIA).
 * Broadcast ephemeris arrives continuously and is mutated from the stream
 * thread, so it needs its own mutex-guarded, overwrite-per-satellite
 * container — mirroring @c SsrCorrectionStore exactly.
 *
 * Threading: a single mutex protects the per-satellite map.  Stream pushes
 * happen on the receive thread; @c computeSatellitePV runs on the per-epoch
 * processing thread.  The lock window covers only a copy of the record;
 * the Kepler / PZ-90 integration runs outside the lock.
 */

#include "products/BroadcastEphemerisTypes.hpp"
#include <Eigen/Dense>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

/**
 * @class LiveEphemerisStore
 * @brief Per-satellite "latest broadcast ephemeris" cache, thread-safe.
 *
 * Each @c insert overwrites that satellite's current record, retaining the
 * immediately-previous one so an IODE transition does not lose the orbit
 * valid just before the cutover.  @c computeSatellitePV selects whichever
 * retained record's reference epoch is closest to the query time.
 */
class LiveEphemerisStore {
public:
    /// Returns the singleton instance.  Construction is the first call.
    static LiveEphemerisStore& getInstance();

    LiveEphemerisStore(const LiveEphemerisStore&)            = delete;
    LiveEphemerisStore& operator=(const LiveEphemerisStore&) = delete;
    LiveEphemerisStore(LiveEphemerisStore&&)                 = delete;
    LiveEphemerisStore& operator=(LiveEphemerisStore&&)      = delete;

    /// Computed satellite state from a broadcast ephemeris.
    struct SatelliteState {
        Eigen::Vector3d position{Eigen::Vector3d::Zero()};  ///< ECEF position [m].
        Eigen::Vector3d velocity{Eigen::Vector3d::Zero()};  ///< ECEF velocity [m/s].
        int             iode{-1};                            ///< IODE in the IGS-SSR matching convention (see ssrMatchIode), for the SSR orbit-overlay gate.
    };

    /**
     * @brief Insert / overwrite a satellite's broadcast ephemeris.
     *
     * Source priority + freshest-toe.  @p from_igs marks whether the record came
     * from a dedicated IGS broadcast-nav stream (RTCM, default) or the receiver's
     * own decoded/assembled nav (e.g. SBF).  The IGS stream is AUTHORITATIVE: the
     * IGS-RTS SSR delta is computed against IGS broadcast ephemeris, so a receiver
     * record must not overwrite a current IGS record at an equal-or-older toe
     * (even at the same IODE the receiver's fit params differ, making the SSR
     * overlay inconsistent).  A receiver record is accepted only as a gap-fill (no
     * record yet) or when the IGS feed has clearly stalled on that satellite
     * (receiver toe newer by more than a constellation refresh).  Within one
     * source the freshest toe wins; an IODE change demotes the current record to
     * "previous" so an SSR delta can re-reference across a cutover.  Default
     * @c true keeps single-source callers (post-processing, tests, the all-RTCM
     * IGS-station config, the network path) on plain freshest-toe behaviour.
     */
    void insert(const BroadcastEphemeris& eph, bool from_igs = true);

    /**
     * @brief Compute the satellite ECEF position + velocity at @p time.
     * @param sat  Satellite identifier (e.g. "G01").
     * @param time Query epoch (GPS time scale).
     * @return The computed state, or @c std::nullopt when no ephemeris is
     *         held for @p sat (caller then falls through to "no orbit").
     *
     * Velocity is obtained by central finite difference of the position
     * model — robust and accurate to well under the SSR-correction floor,
     * and reused for the RAC orbit-frame basis.
     */
    std::optional<SatelliteState> computeSatellitePV(
        const std::string& sat, const GnssTime& time, int ssr_iode = -1) const;

    /// Broadcast satellite-clock evaluated from the polynomial elements.
    struct SatelliteClock {
        double bias{0.0};  ///< SV clock bias [s]: af0+af1·dt+af2·dt² (GLONASS −τ+γ·dt).
        int    iode{-1};   ///< IODE in the IGS-SSR matching convention (see ssrMatchIode), for the SSR clock-delta IODE gate.
    };

    /**
     * @brief Evaluate the broadcast SV clock bias at @p time.
     * @param sat  Satellite identifier (e.g. "G01").
     * @param time Query epoch (GPS time scale).
     * @return The clock bias + originating IODE, or @c std::nullopt when no
     *         ephemeris is held for @p sat (caller then keeps its prior value).
     *
     * The Keplerian constellations return @c af0+af1·dt+af2·dt² with
     * @c dt = (t − toc) reduced on the satellite's NATIVE scale (BeiDou on BDT,
     * mirroring @c keplerPosition so the clock and orbit scales stay
     * self-consistent); GLONASS returns @c −τ_n+γ_n·(t−t_b).  The periodic
     * relativistic term and the group delay (TGD/BGD) are deliberately
     * EXCLUDED: relativistic is applied separately downstream and TGD is
     * structurally absent from the ionosphere-free combination this pipeline
     * forms — matching the precise-CLK convention so the broadcast value is a
     * drop-in baseline for the SSR clock delta to add onto.
     */
    std::optional<SatelliteClock> computeSatelliteClock(
        const std::string& sat, const GnssTime& time, int ssr_iode = -1) const;

    /**
     * @brief Metres of broadcast-clock discontinuity between the freshest held
     *        ephemeris and the version @p old_iode, evaluated at @p time:
     *        @c c·(bias_new − bias_old).
     *
     * The network PPP-RTK producer (RTF-2 Card 6, @c network.apriori_source =
     * @c brdc_rts) estimates each satellite clock as Δδtˢ relative to the
     * BROADCAST a-priori clock of the ephemeris version its geometry currently
     * uses.  When that version rolls (old IODE → new IODE) the a-priori jumps by
     * this amount; the TRUE satellite clock is continuous, so the estimable Δδtˢ
     * must be re-based by @c −(this) to keep referencing the same satellite —
     * the driver's deterministic eph-cutover re-level, which emits NO
     * discontinuity bump because the consumer reconstructs @c brdc(new_iode)+C0.
     *
     * Uses the same pinned-version retention as @ref computeSatelliteClock: the
     * freshest held record supplies @c bias_new and the record whose SSR-match
     * IODE equals @p old_iode (current OR previous) supplies @c bias_old.  Both
     * biases are evaluated at @p time, so a non-zero @c af1/af2 is honoured.
     *
     * @param sat      Satellite id ("G01").
     * @param time     Epoch to evaluate both clock polynomials at.
     * @param old_iode The SSR-convention IODE the carried Δδtˢ is referenced to.
     * @return c·(bias_new − bias_old) in metres; @c std::nullopt when no
     *         ephemeris is held for @p sat, or when @p old_iode is no longer
     *         retained (current/previous) — the caller then forces a per-sat
     *         discontinuity rather than re-leveling against an unknown baseline.
     */
    std::optional<double> ephCutoverShiftMetres(const std::string& sat,
                                                const GnssTime& time, int old_iode) const;

    /// @brief Signed age (seconds, query − toe) of the closest-toe held
    ///        ephemeris for @p sat, or @c std::nullopt when none is held.
    ///
    /// A cheap freshness probe (no orbit integration) used by the real-time BSSD
    /// pivot selector to avoid seating a satellite whose ephemeris is much staler
    /// than its constellation peers as the pivot.  |age| is the broadcast
    /// extrapolation magnitude; smaller is a better pivot anchor.
    std::optional<double> toeAgeSeconds(const std::string& sat, const GnssTime& time) const;

    /// True iff no ephemeris has been inserted.
    bool isEmpty() const;

    /// Number of satellites with at least one ephemeris.
    std::size_t size() const;

    /// Drop every record.
    void clear();

    /// Snapshot of the GLONASS FDMA channel assignments currently held
    /// (satellite id "R01" → channel k, -7..+6) from each stored GLONASS
    /// record's broadcast @c freq_channel.  The real-time engine feeds this to
    /// @c PppModel so per-satellite GLONASS wavelengths resolve from the live
    /// stream rather than the hard-coded default table.  Empty when no GLONASS
    /// ephemeris is held.
    std::map<std::string, int> glonassFreqChannels() const;

private:
    LiveEphemerisStore() = default;
    ~LiveEphemerisStore() = default;

    struct SatRecord {
        BroadcastEphemeris                current;
        std::optional<BroadcastEphemeris> previous;
        /// True when @c current came from a dedicated IGS broadcast-nav stream
        /// (authoritative); false when it came from the receiver's own nav.  Used
        /// by insert() so a receiver record can't clobber an IGS one at equal toe.
        bool                              current_from_igs{true};
    };

    /// Pick current vs previous by reference-epoch proximity to @p time.
    static const BroadcastEphemeris& pick(const SatRecord& rec, const GnssTime& time);

    /// Pick the retained version (current or previous) whose SSR-convention IODE
    /// equals @p ssr_iode, so an SSR delta re-references to the ephemeris version
    /// it was computed for across a cutover desync.  Falls back to @c pick() when
    /// @p ssr_iode < 0 or neither held version matches (the caller's IODE gate
    /// then drops the satellite).
    static const BroadcastEphemeris& pickForSsr(const SatRecord& rec,
                                                const GnssTime& time, int ssr_iode);

    /// Keplerian ECEF position (GPS/Galileo/BeiDou/QZSS) at @p time.
    static Eigen::Vector3d keplerPosition(const BroadcastEphemeris& eph, const GnssTime& time);

    /// GLONASS PZ-90 ECEF state (position + velocity) at @p time.
    static SatelliteState glonassState(const BroadcastEphemeris& eph, const GnssTime& time);

    mutable std::mutex                          mutex_;
    std::unordered_map<std::string, SatRecord>  by_sat_;
};

} // namespace GnssProcessor
