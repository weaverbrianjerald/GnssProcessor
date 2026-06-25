/**
 * @file NtripClient.hpp
 * @brief NTRIP v2 client for receiving RTCM correction streams.
 *
 * Connects to an NTRIP caster over HTTP or HTTPS and provides a blocking
 * byte-read interface that the RTCM stream reader can consume.  Uses libcurl
 * (already linked) for the HTTP(S) transport layer.
 */
#pragma once

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
 * @brief Configuration for an NTRIP caster connection.
 */
struct NtripConfig {
    std::string host;                   ///< Caster hostname (e.g. "caster.example.com").
    int         port{ 2101 };           ///< Caster port.
    std::string mountpoint;             ///< Mountpoint name (e.g. "RTCM3").
    std::string username;               ///< NTRIP username (empty for anonymous).
    std::string password;               ///< NTRIP password.
    std::string nmea_gga;               ///< Optional GGA sentence sent to caster for VRS.
    int         reconnect_delay_s{ 5 }; ///< Seconds to wait before reconnecting on failure.
    bool        use_tls{ false };       ///< Connect over HTTPS (curl SSL) instead of plain HTTP.  Required for port-443 casters (e.g. CDDIS).  Default false keeps plain-HTTP casters (igs-ip.net, SBF replay) byte-identical.
    bool        tls_insecure{ false };  ///< When @c use_tls, disable peer/host certificate verification (CURLOPT_SSL_VERIFYPEER/VERIFYHOST → 0).  Testing only — against self-signed or mis-chained casters; default verifies the chain.
};

/**
 * @brief NTRIP v2 client that receives a continuous RTCM byte stream.
 *
 * Internally runs a background thread that keeps the HTTP connection
 * alive and pushes received bytes into a thread-safe ring buffer.
 * The public read() method blocks until the requested number of bytes
 * is available (or the connection is stopped).
 */
class NtripClient {
public:
    explicit NtripClient(const NtripConfig& config, LogManager& log_manager);
    ~NtripClient();

    /// Start the background receive thread.  Returns immediately.
    bool start();

    /// Stop the background thread and close the connection.
    void stop();

    /// True while the background thread is running and the connection is live.
    bool isConnected() const { return connected_.load(); }

    /// True from start() until stop() — the client's intent-to-run, independent
    /// of momentary connection state.  Used by the realtime poll loop so a
    /// not-yet-established or transiently-dropped stream stays "active".
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
     * @brief Non-blocking read with a millisecond timeout.
     *
     * Used by the multi-stream realtime loop, where blocking on one
     * stream would starve the other.  Waits up to @p timeout_ms for
     * data to arrive; returns 0 on timeout, on stop, or when no data
     * is buffered after the wait.
     */
    size_t tryRead(std::vector<uint8_t>& out, size_t max_bytes, int timeout_ms);

private:
    /// libcurl write-callback feeds bytes into the ring buffer.
    static size_t curlWriteCallback(void* data, size_t size, size_t nmemb, void* userp);

    /// Background thread entry point.
    void receiveLoop();

    NtripConfig           config_;
    LogManager&           log_manager_;

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
