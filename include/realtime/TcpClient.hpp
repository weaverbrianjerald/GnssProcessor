#pragma once
/**
 * @file TcpClient.hpp
 * @brief Raw TCP client for receiving an RTCM byte stream from a receiver.
 *
 * Septentrio and most current receivers can push RTCM3 MSM messages over a TCP
 * server socket, e.g. a Septentrio AsteRx configured with
 * "RTCMv3 output → IPS1, port 28784".  This client connects to that host:port
 * pair and copies the raw bytes into an internal buffer that the realtime
 * polling loop drains via tryRead().
 *
 * Mirrors @c NtripClient: a background thread does a blocking @c recv into a
 * mutex-protected buffer and wakes the consumer through a condition variable.
 * The client reconnects automatically on connection loss.
 */

#include "logging/LogManager.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Configuration for a raw TCP byte-stream connection.
 */
struct TcpConfig {
    std::string host;                   ///< Hostname or IP (e.g. "rooftop.example.com" or "10.0.0.5").
    int         port{ 28784 };          ///< TCP port the receiver is listening on.
    int         reconnect_delay_s{ 5 }; ///< Wait before reconnecting after a connection loss.
};

/**
 * @class TcpClient
 * @brief Buffered, threaded raw TCP byte-stream client.
 *
 * Lifetime mirrors @c NtripClient: construct, @c start(), poll via
 * @c tryRead() until done, @c stop().  The destructor calls @c stop().
 */
class TcpClient {
public:
    TcpClient(const TcpConfig& cfg, LogManager& lm);
    ~TcpClient();

    /// Start the background receive thread.
    bool start();

    /// Stop the thread, close the socket.
    void stop();

    /// True while the connection is established.
    bool isConnected() const { return connected_.load(); }

    /// True from start() until stop(): the client's intent to run, independent of
    /// the momentary connection state.  The realtime poll loop treats a socket
    /// source as active while it is running (drops are handled by the internal
    /// auto-reconnect), so a transient disconnect or a connection not yet
    /// established at startup does not end the session.
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Buffered read that waits up to @p timeout_ms milliseconds for data
     *        (no wait when @p timeout_ms ≤ 0) and returns at most @p max_bytes.
     *
     * Same contract as @c NtripClient::tryRead; used by the multi-stream
     * realtime polling loop.
     */
    std::size_t tryRead(std::vector<uint8_t>& out, std::size_t max_bytes, int timeout_ms);

private:
    void receiveLoop();

    TcpConfig    cfg_;
    LogManager&  log_manager_;

    std::thread        thread_;
    std::atomic<bool>  running_{ false };
    std::atomic<bool>  connected_{ false };

    std::mutex              buf_mutex_;
    std::condition_variable buf_cv_;
    std::vector<uint8_t>    buf_;
    static constexpr std::size_t kMaxBufSize = 4 * 1024 * 1024; // 4 MB cap
};

} // namespace GnssProcessor
