/**
 * @file NtripClient.cpp
 * @brief NTRIP v2 client implementation using libcurl.
 */

#include "realtime/NtripClient.hpp"
#include "common/CurlGlobalGuard.hpp"
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

NtripClient::NtripClient(const NtripConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager),
      jitter_rng_(std::random_device{}()) {
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

    // RTB-2(a): guarantee curl_global_init ran exactly once BEFORE the receive
    // thread's first curl_easy_init.  An NTRIP-only session (no DataDownloader
    // constructed) previously relied on libcurl's lazy auto-init, which is
    // documented as thread-unsafe.  See CurlGlobalGuard.hpp.
    ensureCurlGlobalInit();

    running_.store(true);
    thread_ = std::thread(&NtripClient::receiveLoop, this);
    return true;
}

int NtripClient::reconnectDelayMs() {
    // Uniform 0-2000 ms of jitter on top of the configured base (RTB-2(b)).
    std::uniform_int_distribution<int> jitter(0, 2000);
    return config_.reconnect_delay_s * 1000 + jitter(jitter_rng_);
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
    self->total_rx_bytes_.fetch_add(total);
    self->buf_cv_.notify_one();

    // Returning 0 tells curl to abort  honour the running flag.
    return self->running_.load() ? total : 0;
}

// ---------------------------------------------------------------------------
// Background receive loop
// ---------------------------------------------------------------------------

void NtripClient::receiveLoop() {
    auto log = log_manager_.getLogger("ntrip");

    // One-time diagnostic when TLS is requested: surface the libcurl build's
    // SSL backend (e.g. "Schannel", "OpenSSL/3.x") so a caster failure can be
    // distinguished from a curl-without-SSL build at a glance.
    if (config_.use_tls) {
        log->info("NTRIP TLS enabled (libcurl: {})", curl_version());
        if (config_.tls_insecure) {
            log->warn("NTRIP TLS certificate verification DISABLED (tls_insecure) — the "
                      "caster connection is NOT authenticated and is open to MITM; use "
                      "only for testing against self-signed/mis-chained casters");
        }
    }

    while (running_.load()) {
        const char* scheme = config_.use_tls ? "https" : "http";
        log->info("Connecting to NTRIP caster {}://{}:{}/{}", scheme, config_.host,
                  config_.port, config_.mountpoint);

        CURL* curl = curl_easy_init();
        if (!curl) {
            log->error("Failed to create libcurl handle");
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelayMs()));
            continue;
        }

        // Build URL: http(s)://host:port/mountpoint.  The URL scheme is what
        // actually drives TLS for the HTTP transport.
        std::string url = std::string(scheme) + "://" + config_.host + ":"
                          + std::to_string(config_.port) + "/" + config_.mountpoint;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // TLS transport (port-443 casters such as CDDIS).  CURLUSESSL_ALL
        // requires SSL for all communication (belt-and-suspenders alongside the
        // https scheme above); verification stays ON (VERIFYPEER=1/VERIFYHOST=2)
        // unless tls_insecure is set for testing against self-signed casters.
        if (config_.use_tls) {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config_.tls_insecure ? 0L : 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config_.tls_insecure ? 0L : 2L);
        }

        // NTRIP v2 uses standard HTTP GET with specific headers.
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Ntrip-Version: Ntrip/2.0");
        headers = curl_slist_append(headers, "User-Agent: NTRIP GnssProcessor/1.0");
        // RTB-2(e): a configured GGA sentence rides as the NTRIP rev2
        // "Ntrip-GGA:" request header on the GET.  The previous
        // CURLOPT_POSTFIELDS approach silently turned the request into a POST
        // — wrong for fixed-station rev2 casters and not the rev2 GGA
        // mechanism.  rev1-style IN-STREAM GGA (periodic sentences written
        // into the open connection) is unsupported; see NtripConfig::nmea_gga.
        std::string gga_header;
        if (!config_.nmea_gga.empty()) {
            gga_header = "Ntrip-GGA: " + config_.nmea_gga;
            headers = curl_slist_append(headers, gga_header.c_str());
        }
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

        connected_.store(true);
        const size_t bytes_before = total_rx_bytes_.load();
        CURLcode res = curl_easy_perform(curl);
        connected_.store(false);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        const size_t bytes_this = total_rx_bytes_.load() - bytes_before;

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (!running_.load()) break;   // stop() requested during the transfer

        // Classify the outcome so a misconfiguration is an actionable error
        // rather than a silent stall: the realtime poll loop now runs as long
        // as the client is "running" (so it can auto-reconnect), which means a
        // bad mountpoint or credential would otherwise spin invisibly with the
        // engine reporting 0 epochs.  ALWAYS back off before retrying.
        if (res != CURLE_OK) {
            log->warn("NTRIP transport error ({}); reconnecting in {} s",
                      curl_easy_strerror(res), config_.reconnect_delay_s);
        } else if (http_code != 200 && http_code != 0) {
            // NTRIP v1 casters may answer "ICY 200 OK" (reported as 0); only a
            // genuine non-200 HTTP status is an error here.
            const char* hint = (http_code == 401) ? " — check NTRIP user/password"
                             : (http_code == 403) ? " — credentials valid but mountpoint access denied"
                             : (http_code == 404) ? " — unknown mountpoint name"
                             : "";
            log->error("NTRIP caster returned HTTP {}{}; retrying in {} s",
                       http_code, hint, config_.reconnect_delay_s);
        } else if (bytes_this == 0) {
            log->warn("NTRIP connected to {}/{} but received 0 bytes — mountpoint idle, "
                      "or it requires a GGA position (VRS)?  Retrying in {} s",
                      config_.host, config_.mountpoint, config_.reconnect_delay_s);
        } else {
            log->info("NTRIP stream {}/{} ended after {} bytes; reconnecting in {} s",
                      config_.host, config_.mountpoint, bytes_this, config_.reconnect_delay_s);
        }
        // RTB-2(b): base delay + per-client jitter — N streams dropped by one
        // caster outage must not re-dial the recovering caster in lockstep.
        std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelayMs()));
    }

    log->info("NTRIP receive loop exited");
}

} // namespace GnssProcessor
