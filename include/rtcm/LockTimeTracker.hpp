#pragma once
/**
 * @file LockTimeTracker.hpp
 * @brief Stateful MSM lock-time → loss-of-lock (LLI 0x1) derivation (RTB-3).
 *
 * RTCM MSM messages carry a per-(satellite, signal) LOCK TIME — how long the
 * receiver has held continuous carrier lock (DF402 4-bit basic table in
 * MSM4/MSM5, DF407 10-bit extended table in MSM7) — instead of a RINEX-style
 * loss-of-lock indicator.  Deriving the LLI 0x1 bit therefore needs HISTORY:
 * a lock time that went DOWN between epochs means tracking restarted (a cycle
 * slip happened inside the interval), even though every individual message is
 * self-consistent.  The stateless MSM decoder only surfaces the decoded lock
 * time on @c RinexObservation::lock_time_ms; this tracker lives at the
 * EPOCH-ASSEMBLY layer (one instance per receiving front-end) and owns the
 * history.
 *
 * @par Detection rule (RTKLIB @c lossoflock equivalent)
 * For each update with the current lock time @c lock_ms and the stored
 * previous value @c prev (a never-seen key reads as @c prev = 0, matching
 * RTKLIB's zero-initialised @c rtcm->lock array):
 *   - @c lock_ms <  prev  →  LOSS.  Lock decreased: tracking restarted since
 *     the previous epoch.  This also covers a reset to a small value that
 *     continuity would have grown past (the tables are monotone in the raw
 *     indicator, so comparing decoded milliseconds is order-equivalent).
 *   - @c lock_ms == 0 AND @c prev == 0  →  LOSS.  Lock never (yet) held for a
 *     full table quantum; consecutive zeros flag every epoch, exactly like
 *     RTKLIB — the signal is not usably locked, so downstream slip handling
 *     must not bridge it.  Note this makes a FRESH key with @c lock_ms == 0
 *     flag on first sight, while a fresh key with @c lock_ms > 0 does not.
 *   - otherwise (equal non-zero, or increased)  →  no loss.  The extended
 *     table SATURATES at its top step rather than wrapping, so an old stable
 *     lock reads as equal maxima epoch after epoch and never false-flags.
 * The current value is stored after every call.
 *
 * @par Keying
 * Entries are keyed by (station, satellite, signal).  The single-rover
 * real-time engine passes an empty station key today; the planned
 * multi-station network front-end (RTF-3) reuses ONE tracker across
 * stations by passing each stream's station identifier, so histories can
 * never cross-talk between receivers.
 *
 * Wired in @c RealTimeEngine::feedRtcmFrame behind the default-OFF toggle
 * @c realtime.msm_lli_from_locktime — arming it changes slip-detection
 * behaviour on RT replays, so it must be an explicit choice.
 */

#include <cstddef>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

/**
 * @class LockTimeTracker
 * @brief Per-(station, satellite, signal) lock-time history with the
 *        RTKLIB-equivalent loss-of-lock decision (see file Doxygen).
 *
 * Not thread-safe: intended to live on the epoch-assembly thread (the
 * real-time engine's frame-feed path is single-threaded).
 */
class LockTimeTracker {
public:
    /**
     * @brief Record a decoded lock time and decide whether LLI 0x1 must be set.
     *
     * @param station  Station / stream key ("" for the single-rover engine;
     *                 the multi-station front-end passes a per-stream id).
     * @param sat      Satellite id ("G05", "R12", ...).
     * @param signal   Phase-signal obs code the lock time belongs to ("L1C").
     * @param lock_ms  Decoded lock time in milliseconds (from the MSM DF402 /
     *                 DF407 tables; @c RinexObservation::lock_time_ms).
     * @return true when the loss-of-lock (LLI 0x1) bit should be set for this
     *         (sat, signal) at this epoch — lock decreased, or lock is zero
     *         and was zero (or unseen) before.  See the file Doxygen for the
     *         full rule.
     */
    bool update(const std::string& station, const std::string& sat,
                const std::string& signal, double lock_ms);

    /// Drop all history (e.g. across a deliberate session restart).
    void clear() { last_lock_ms_.clear(); }

    /// Number of tracked (station, sat, signal) keys (diagnostics/tests).
    std::size_t size() const { return last_lock_ms_.size(); }

private:
    /// "station|sat|signal" -> last seen lock time (ms).
    std::unordered_map<std::string, double> last_lock_ms_;
};

} // namespace GnssProcessor
