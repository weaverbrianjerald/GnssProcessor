/**
 * @file NtripClient.hpp
 * @brief NTRIP v2 client for receiving RTCM correction streams.
 *
 * Connects to an NTRIP caster over HTTP or HTTPS and provides a blocking
 * byte-read interface for the RTCM stream reader.  The HTTP(S) transport is
 * libcurl, which the project already links.
 */
#pragma once

#include "logging/LogManager.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Configuration for an NTRIP caster connection.
 */
struct NtripConfig {
    std::string host;                   ///< Caster hostname (e.g. "caster.example.com").
    int         port{ 2101 };           ///< Caster port.
    std::string mountpoint;             ///< Mountpoint name (e.g. "RTCM3").
    std::string username;               ///< NTRIP username (empty for anonymous).
    std::string password;               ///< NTRIP password.
    /// Optional NMEA GGA sentence for VRS / nearest-station mountpoints.
    /// Sent as the NTRIP rev2 @c "Ntrip-GGA:" request header on the GET
    /// (RTB-2(e)).  It is not sent through @c CURLOPT_POSTFIELDS, which would turn
    /// the request into a POST; that is wrong for fixed-station rev2 casters and is
    /// not the rev2 GGA mechanism.  NTRIP rev1-style in-stream GGA upload (writing
    /// sentences into the open connection, as some legacy VRS casters expect) is
    /// not supported by this client.
    std::string nmea_gga;
    /// Base seconds to wait before reconnecting on failure.  A uniform 0-2 s
    /// jitter is added per attempt (RTB-2(b)) so that N clients dropped by one
    /// caster outage do not re-dial the recovering caster in lockstep.
    int         reconnect_delay_s{ 5 };
    bool        use_tls{ false };       ///< Connect over HTTPS (curl SSL) instead of plain HTTP.  Required for port-443 casters (e.g. CDDIS).  Default false keeps plain-HTTP casters (igs-ip.net, SBF replay) byte-identical.
    bool        tls_insecure{ false };  ///< When @c use_tls, disable peer/host certificate verification (CURLOPT_SSL_VERIFYPEER/VERIFYHOST → 0).  For testing against self-signed or mis-chained casters only; the default verifies the chain.
};

/**
 * @brief NTRIP v2 client that receives a continuous RTCM byte stream.
 *
 * A background thread keeps the HTTP connection alive and appends received bytes
 * to a mutex-protected FIFO buffer capped at 4 MB (the oldest bytes are dropped
 * on overflow).  The public read() method blocks until at least one byte is
 * available or the client is stopped.
 */
class NtripClient {
public:
    explicit NtripClient(const NtripConfig& config, LogManager& log_manager);
    ~NtripClient();

    /// Start the background receive thread.  Returns immediately.
    bool start();

    /// Stop the background thread and close the connection.
    void stop();

    /// True while a transfer attempt is in progress.  The flag is set just before
    /// the transfer starts, so it also covers the connect phase.
    bool isConnected() const { return connected_.load(); }

    /// True from start() until stop(): the client's intent to run, independent of
    /// the momentary connection state.  The realtime poll loop uses it so that a
    /// stream not yet established or transiently dropped stays "active".
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Read up to @p max_bytes from the internal buffer.
     *
     * Blocks until at least 1 byte is available or the client is stopped.
     *
     * @param[out] out     Destination buffer (resized to actual bytes read).
     * @param      max_bytes Maximum number of bytes to return.
     * @return Number of bytes placed in @p out (0 if stopped with no data).
     */
    size_t read(std::vector<uint8_t>& out, size_t max_bytes);

    /**
     * @brief Read with a bounded wait of @p timeout_ms milliseconds.
     *
     * Used by the multi-stream realtime loop, where blocking on one stream would
     * starve the other.  Waits up to @p timeout_ms for data to arrive (no wait when
     * @p timeout_ms <= 0); returns 0 on timeout, on stop, or when no data is
     * buffered after the wait.
     */
    size_t tryRead(std::vector<uint8_t>& out, size_t max_bytes, int timeout_ms);

private:
    /// libcurl write callback: appends bytes to the buffer.
    static size_t curlWriteCallback(void* data, size_t size, size_t nmemb, void* userp);

    /// Background thread entry point.
    void receiveLoop();

    /**
     * @brief Next reconnect back-off in milliseconds (RTB-2(b)).
     *
     * @c reconnect_delay_s × 1000 plus a uniform 0-2000 ms jitter drawn from a
     * per-client @c std::mt19937 (seeded from @c std::random_device at
     * construction), so that several streams dropped by a single caster outage
     * spread their re-dials instead of reconnecting to the recovering caster in
     * lockstep.  Called only on the receive thread.
     */
    int reconnectDelayMs();

    NtripConfig           config_;
    LogManager&           log_manager_;

    /// Per-client jitter source for reconnect back-off (see reconnectDelayMs).
    std::mt19937          jitter_rng_;

    std::thread           thread_;
    std::atomic<bool>     running_{ false };
    std::atomic<bool>     connected_{ false };
    std::atomic<size_t>   total_rx_bytes_{ 0 };  ///< Cumulative bytes received (stall diagnostics).

    // Thread-safe byte buffer.
    std::mutex              buf_mutex_;
    std::condition_variable buf_cv_;
    std::vector<uint8_t>    buf_;
    static constexpr size_t kMaxBufSize = 4 * 1024 * 1024; // 4 MB cap
};

} // namespace GnssProcessor
