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
     * If the new record's IODE differs from the current one, the current
     * record is demoted to "previous" before being replaced.
     */
    void insert(const BroadcastEphemeris& eph);

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

    /// True iff no ephemeris has been inserted.
    bool isEmpty() const;

    /// Number of satellites with at least one ephemeris.
    std::size_t size() const;

    /// Drop every record.
    void clear();

private:
    LiveEphemerisStore() = default;
    ~LiveEphemerisStore() = default;

    struct SatRecord {
        BroadcastEphemeris                current;
        std::optional<BroadcastEphemeris> previous;
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
