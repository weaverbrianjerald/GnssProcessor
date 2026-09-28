#pragma once
/**
 * @file LockTimeTracker.hpp
 * @brief Stateful derivation of the loss-of-lock bit (LLI 0x1) from MSM lock
 *        times (RTB-3).
 *
 * RTCM MSM messages carry a per-(satellite, signal) lock time, the duration of
 * continuous carrier lock (DF402 4-bit basic table in MSM4/MSM5, DF407 10-bit
 * extended table in MSM6/MSM7), instead of a RINEX-style loss-of-lock
 * indicator.  Deriving the LLI 0x1 bit therefore needs history: a lock time
 * that decreases between epochs means tracking restarted (a cycle slip
 * occurred inside the interval), although each message is self-consistent.
 * The stateless MSM decoder only writes the decoded lock time to
 * @c RinexObservation::lock_time_ms; this tracker sits at the epoch-assembly
 * layer (one instance per receiving front-end) and owns the history.
 *
 * @par Detection rule (equivalent to RTKLIB @c lossoflock)
 * For each update with the current lock time @c lock_ms and the stored
 * previous value @c prev (a key not seen before reads as @c prev = 0, matching
 * RTKLIB's zero-initialised @c rtcm->lock array):
 *   - @c lock_ms <  prev: loss.  Lock decreased, so tracking restarted since
 *     the previous epoch.  This also covers a reset to a small value that
 *     continuous lock would have grown past (the tables are monotone in the
 *     raw indicator, so comparing decoded milliseconds is order-equivalent).
 *   - @c lock_ms == 0 and @c prev == 0: loss.  Lock has not yet been held for
 *     a full table quantum; consecutive zeros flag every epoch, as in RTKLIB.
 *     The signal is not usably locked, so downstream slip handling must not
 *     bridge it.  A key seen for the first time with @c lock_ms == 0 therefore
 *     flags on first sight; one with @c lock_ms > 0 does not.
 *   - otherwise (equal non-zero, or increased): no loss.  The extended table
 *     saturates at its top step instead of wrapping, so a long stable lock
 *     reads as equal maxima epoch after epoch and does not flag.
 * The current value is stored after every call.
 *
 * @par Keying
 * Entries are keyed by (station, satellite, signal).  The single-rover
 * real-time engine passes an empty station key; the multi-station streaming
 * front-end (@c NetworkStreamFrontEnd, RTF-3) reuses one tracker across
 * stations and passes each stream's station identifier, so histories of
 * different receivers stay separate.
 *
 * Wired in @c RealTimeEngine::feedRtcmFrame behind the default-off toggle
 * @c realtime.msm_lli_from_locktime (and in the streaming front-end behind
 * @c network.streaming.msm_lli_from_locktime).  Arming it changes
 * slip-detection behaviour on RT replays, so it is an explicit choice.
 */

#include <cstddef>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

/**
 * @class LockTimeTracker
 * @brief Per-(station, satellite, signal) lock-time history with the
 *        RTKLIB-equivalent loss-of-lock decision (see the file Doxygen).
 *
 * Not thread-safe: each instance is used from one epoch-assembly thread (the
 * real-time engine's frame-feed path, or the streaming front-end's single
 * ingest thread).
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
     *         (sat, signal) at this epoch: lock decreased, or lock is zero and
     *         was zero (or unseen) before.  The file Doxygen has the full rule.
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
