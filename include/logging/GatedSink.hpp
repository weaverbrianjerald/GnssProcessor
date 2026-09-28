#pragma once

/**
 * @file GatedSink.hpp
 * @brief spdlog sink wrapper that buffers messages and replays them on request.
 *
 * Used by KalmanFilter for concise per-epoch logging: pass-1 messages pass
 * through immediately, the messages of every later rebuild pass accumulate in
 * the buffer, and endEpoch() replays the whole buffer in order and then clears
 * it.  Generic enough to be reused for any logger with a similar
 * "live first pass, deferred later passes" emission policy.
 */

#include <spdlog/sinks/sink.h>
#include <spdlog/details/log_msg_buffer.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Sink that wraps one or more inner sinks and chooses, per message,
 *        whether to forward to them or hold the message in a buffer.
 *
 * Lifecycle of an emitted message:
 * - In @c Policy::Passthrough mode: forwarded immediately to every inner sink.
 *   Equivalent to a transparent passthrough wrapper.
 * - In @c Policy::Buffer mode: appended (as an owned copy) to an in-memory
 *   buffer. Inner sinks are not touched.
 *
 * The caller sets the policy with @c setPolicy. The buffer is exposed via:
 * - @c clearBuffer: discard everything currently buffered (no replay).
 * - @c replayBuffer: forward every buffered message to the inner sinks
 *   exactly as if it had been emitted in passthrough mode.
 *
 * Thread-safe: concurrent log() calls use a mutex around the buffer; the policy
 * is an atomic, so a switch from Buffer to Passthrough is observed
 * monotonically. Inner-sink calls rely on spdlog's per-sink mutex (the inner
 * sinks should be the @c _mt variants).
 */
class GatedSink : public spdlog::sinks::sink {
public:
    enum class Policy {
        Passthrough,  ///< Forward every message to inner sinks immediately.
        Buffer        ///< Hold every message in the in-memory buffer.
    };

    /**
     * @brief Construct a gated sink wrapping @p inner_sinks.
     * @param inner_sinks Sinks that receive forwarded messages and replays.
     */
    explicit GatedSink(std::vector<spdlog::sink_ptr> inner_sinks);

    // spdlog::sinks::sink overrides
    void log(const spdlog::details::log_msg& msg) override;
    void flush() override;
    void set_pattern(const std::string& pattern) override;
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override;

    /// Switch the gating policy. Lock-free; subsequent log() calls observe
    /// the new policy.
    void setPolicy(Policy p);

    /// Discard every message currently in the buffer. Inner sinks unaffected.
    void clearBuffer();

    /// Forward every buffered message to the inner sinks. The buffer is
    /// retained; call @c clearBuffer afterward to start an empty one.
    void replayBuffer();

private:
    std::vector<spdlog::sink_ptr> inner_;
    std::atomic<Policy> policy_{ Policy::Passthrough };

    std::mutex buffer_mutex_;
    std::vector<spdlog::details::log_msg_buffer> buffer_;
};

} // namespace GnssProcessor
