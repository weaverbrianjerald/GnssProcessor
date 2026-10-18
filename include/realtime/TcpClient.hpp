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

    /// Seconds without a received byte after which a connected peer is treated as
    /// lost: the socket is closed and redialed after @c reconnect_delay_s, the same
    /// path as a closed connection.  A receiver that stops sending without closing
    /// the socket (a hung output task, a dropped route that leaves no reset) would
    /// otherwise keep the stream silent for good.  0 (default) is off and keeps the
    /// socket open for as long as the peer does.  Set it above the longest interval
    /// between messages of the stream, which for an ephemeris-only stream can be a
    /// minute or more.
    double      silent_timeout_s{ 0.0 };
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

    /// Connections dropped because the peer sent nothing for
    /// @c TcpConfig::silent_timeout_s.  Always 0 with the timeout off.
    std::uint64_t silentDrops() const { return silent_drops_.load(); }

    /// Connections established since start(), the first one included.
    std::uint64_t connectionCount() const { return connections_.load(); }

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
    std::atomic<std::uint64_t> silent_drops_{ 0 };
    std::atomic<std::uint64_t> connections_{ 0 };

    std::mutex              buf_mutex_;
    std::condition_variable buf_cv_;
    std::vector<uint8_t>    buf_;
    static constexpr std::size_t kMaxBufSize = 4 * 1024 * 1024; // 4 MB cap
};

} // namespace GnssProcessor
