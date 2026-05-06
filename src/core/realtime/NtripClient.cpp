/**
 * @file NtripClient.cpp
 * @brief NTRIP v2 client implementation using libcurl.
 */

#include "realtime/NtripClient.hpp"
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

NtripClient::NtripClient(const NtripConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
}

NtripClient::~NtripClient() {
    stop();
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool NtripClient::start() {
    if (running_.load()) return true; // already running

    if (config_.host.empty() || config_.mountpoint.empty()) {
        log_manager_.getLogger("ntrip")->error("NTRIP host or mountpoint not configured");
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&NtripClient::receiveLoop, this);
    return true;
}

void NtripClient::stop() {
    running_.store(false);
    buf_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

// ---------------------------------------------------------------------------
// Read (blocking consumer)
// ---------------------------------------------------------------------------

size_t NtripClient::read(std::vector<uint8_t>& out, size_t max_bytes) {
    std::unique_lock<std::mutex> lock(buf_mutex_);
    buf_cv_.wait(lock, [this] { return !buf_.empty() || !running_.load(); });

    if (buf_.empty()) { out.clear(); return 0; }

    size_t n = (std::min)(max_bytes, buf_.size());
    out.assign(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

size_t NtripClient::tryRead(std::vector<uint8_t>& out, size_t max_bytes, int timeout_ms) {
    std::unique_lock<std::mutex> lock(buf_mutex_);
    if (timeout_ms > 0) {
        buf_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !buf_.empty() || !running_.load(); });
    }
    if (buf_.empty()) { out.clear(); return 0; }
    size_t n = (std::min)(max_bytes, buf_.size());
    out.assign(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

// ---------------------------------------------------------------------------
// libcurl write callback
// ---------------------------------------------------------------------------

size_t NtripClient::curlWriteCallback(void* data, size_t size, size_t nmemb, void* userp) {
    auto* self = static_cast<NtripClient*>(userp);
    const size_t total = size * nmemb;
    if (total == 0) return 0;

    {
        std::lock_guard<std::mutex> lock(self->buf_mutex_);
        // Drop oldest data if buffer grows too large (back-pressure).
        if (self->buf_.size() + total > kMaxBufSize) {
            size_t excess = (self->buf_.size() + total) - kMaxBufSize;
            self->buf_.erase(self->buf_.begin(),
                             self->buf_.begin() + static_cast<std::ptrdiff_t>(excess));
        }
        auto* bytes = static_cast<const uint8_t*>(data);
        self->buf_.insert(self->buf_.end(), bytes, bytes + total);
    }
    self->buf_cv_.notify_one();

    // Returning 0 tells curl to abort  honour the running flag.
    return self->running_.load() ? total : 0;
}

// ---------------------------------------------------------------------------
// Background receive loop
// ---------------------------------------------------------------------------

void NtripClient::receiveLoop() {
    auto log = log_manager_.getLogger("ntrip");

    while (running_.load()) {
        log->info("Connecting to NTRIP caster {}:{}/{}", config_.host, config_.port, config_.mountpoint);

        CURL* curl = curl_easy_init();
        if (!curl) {
            log->error("Failed to create libcurl handle");
            std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_delay_s));
            continue;
        }

        // Build URL: http://host:port/mountpoint
        std::string url = "http://" + config_.host + ":" + std::to_string(config_.port)
                          + "/" + config_.mountpoint;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // NTRIP v2 uses standard HTTP GET with specific headers.
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Ntrip-Version: Ntrip/2.0");
        headers = curl_slist_append(headers, "User-Agent: NTRIP GnssProcessor/1.0");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Authentication (if provided).
        if (!config_.username.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, config_.username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.password.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        }

        // Streaming: no timeout  the transfer runs until we abort.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &NtripClient::curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);

        // Send NMEA GGA if configured (for VRS mountpoints).
        if (!config_.nmea_gga.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, config_.nmea_gga.c_str());
        }

        connected_.store(true);
        CURLcode res = curl_easy_perform(curl);
        connected_.store(false);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK && running_.load()) {
            log->warn("NTRIP connection lost ({}), reconnecting in {} s",
                      curl_easy_strerror(res), config_.reconnect_delay_s);
            std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_delay_s));
        }
    }

    log->info("NTRIP receive loop exited");
}

} // namespace GnssProcessor
