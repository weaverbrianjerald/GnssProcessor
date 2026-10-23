/**
 * @file EphemerisJumpLedger.hpp
 * @brief The broadcast record each satellite's geometry used last, and the jump in
 *        modelled range when that record changes.
 *
 * The consumer's range model starts from a broadcast ephemeris (orbit and clock) and
 * adds the SSR correction.  When the record a satellite uses is replaced (a new issue
 * of data, a same-IOD overwrite, or a switch between the IGS broadcast stream and the
 * receiver's own navigation message), the modelled range moves by
 *
 *     Δρ = u · (p_new − p_old) − c · (dt_new − dt_old),
 *
 * with both records evaluated at the same epoch and u the unit line of sight from the
 * receiver to the satellite.  Consecutive records of a healthy satellite agree to well
 * under a metre.  A large Δρ means one of the two records was wrong, and the satellite's
 * states absorbed the error of the old one: its ionosphere, satellite-bias and ambiguity
 * states then hold a value the new model no longer explains.
 *
 * This ledger only measures.  It remembers the record each satellite used last, reports
 * an @ref Event when the orbit or clock content of the record changes, and decides
 * whether the change is a jump (|Δρ| above the threshold).  The response (re-seeding the
 * satellite's states) is the engine's: @c RealTimeEngine::reactToEphemerisJumps_.
 *
 * A change is undecided when the old record cannot be evaluated at the current epoch
 * (beyond its fit window, after a long gap) or either record yields no position; the
 * new record is recorded either way.
 *
 * Counter closure: every reported change is exactly one of undecided, jump or
 * below the threshold, so @ref Counters::closureHolds sums to @c record_changes.
 */
#pragma once

#include "products/LiveEphemerisStore.hpp"

#include <Eigen/Dense>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

/// Per-satellite last-used broadcast record, the range-jump test and its counters.
class EphemerisJumpLedger {
public:
    /// One change of the record a satellite uses.
    struct Event {
        std::string satellite;              ///< Satellite id ("E23").
        int    old_iode{-1};                ///< Raw issue of data of the record used before.
        int    new_iode{-1};                ///< Raw issue of data of the record used now.
        bool   old_from_igs{true};          ///< The old record came from the IGS broadcast stream.
        bool   new_from_igs{true};          ///< The new record came from the IGS broadcast stream.
        bool   decided{false};              ///< Both records evaluated at the epoch.
        double range_jump_m{0.0};           ///< Δρ, new minus old (m); valid when decided.
        bool   jump{false};                 ///< decided and |Δρ| above the threshold.

        /// The record moved between the IGS stream and the receiver's navigation message.
        bool sourceSwitched() const { return old_from_igs != new_from_igs; }
    };

    /// Run-end counters (all monotone).
    struct Counters {
        uint64_t record_changes{0};         ///< Orbit or clock content of a used record changed.
        uint64_t source_switches{0};        ///< …of which the source switched (IGS stream ↔ receiver).
        uint64_t undecided{0};              ///< …old record not evaluable at the epoch.
        uint64_t below_threshold{0};        ///< …|Δρ| at or below the threshold.
        uint64_t jumps{0};                  ///< …|Δρ| above the threshold.
        double   max_abs_jump_m{0.0};       ///< Largest decided |Δρ| (m).
        std::string max_jump_satellite;     ///< The satellite of that largest |Δρ|.

        /// Every change took exactly one outcome.
        bool closureHolds() const {
            return undecided + below_threshold + jumps == record_changes;
        }
    };

    /// @param threshold_m |Δρ| above which a change is a jump (m, positive).
    explicit EphemerisJumpLedger(double threshold_m = 10.0) : threshold_m_(threshold_m) {}

    /// Set the jump threshold (m, positive).
    void setThreshold(double threshold_m) { threshold_m_ = threshold_m; }

    /// The jump threshold (m).
    double threshold() const { return threshold_m_; }

    /**
     * @brief Record the record @p sat uses at @p time and compare it with the last one.
     *
     * The first record seen for a satellite is only recorded.  A record whose orbit and
     * clock elements equal the last one's (a same-content re-broadcast, or a source
     * switch that carries the same elements) reports nothing.
     *
     * @param sat          Satellite id.
     * @param now          The record the satellite's geometry uses at @p time.
     * @param time         The epoch.
     * @param receiver_ecef The receiver position (ECEF, m), for the line of sight.
     * @return The change, or @c std::nullopt when there is none to report.
     */
    std::optional<Event> observe(const std::string& sat,
                                 const LiveEphemerisStore::SelectedRecord& now,
                                 const GnssTime& time,
                                 const Eigen::Vector3d& receiver_ecef);

    /**
     * @brief Modelled-range change Δρ between two records at one epoch (m).
     * @param old_eph       The record used before.
     * @param new_eph       The record used now.
     * @param time          The epoch both are evaluated at.
     * @param receiver_ecef The receiver position (ECEF, m).
     * @return Δρ = u · (p_new − p_old) − c · (dt_new − dt_old), or @c std::nullopt when
     *         either record yields no position.
     */
    static std::optional<double> rangeJump(const BroadcastEphemeris& old_eph,
                                           const BroadcastEphemeris& new_eph,
                                           const GnssTime& time,
                                           const Eigen::Vector3d& receiver_ecef);

    /// True when the orbit and clock elements of two records are equal.
    static bool sameOrbitAndClock(const BroadcastEphemeris& a, const BroadcastEphemeris& b);

    /// Drop every remembered record (the counters are kept).
    void clear() { last_.clear(); }

    /// The run-end counters.
    const Counters& counters() const { return counters_; }

private:
    double threshold_m_;
    std::map<std::string, LiveEphemerisStore::SelectedRecord> last_;
    Counters counters_;
};

} // namespace GnssProcessor
