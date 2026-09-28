/**
 * @file TcpClient.cpp
 * @brief Socket implementation of TcpClient (Winsock2 on Windows).
 *
 * The project builds on Windows (CLAUDE.md, Build), where the file uses Winsock2
 * directly under _WIN32.  The #else branch maps the same code onto POSIX
 * sockets: POSIX headers, @c closesocket defined as @c close, and a no-op
 * @c WSAStartup gate.
 */

#include "realtime/TcpClient.hpp"
#include "logging/SafeLogger.hpp"
#include <chrono>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket    close
   using SOCKET = int;
#endif

namespace GnssProcessor {

namespace {

#ifdef _WIN32
// Initialise Winsock once for the whole process.  WSACleanup is never called:
// the OS reclaims the resources at process exit, and an early cleanup would
// break any other library still using sockets.
struct WsaGuard {
    WsaGuard() {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            Logging::safeDefaultLogger()->error("TcpClient: WSAStartup failed");
        }
    }
};
void ensureWsaInit() {
    static WsaGuard guard;
    (void)guard;
}
#else
void ensureWsaInit() {}
#endif

// Apply a receive timeout so recv() returns periodically and the loop can
// re-check running_: shutdown completes within one timeout instead of blocking
// until the peer sends or closes.  A timeout alone is not treated as a lost
// connection; the loop retries on the same socket (see recvTimedOut()).
void setRecvTimeout(SOCKET sock, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// True when a negative recv() return is the SO_RCVTIMEO firing (no data this
// interval; on POSIX also EINTR) rather than a real error or peer close.  The
// caller then retries on the same socket instead of reconnecting.
bool recvTimedOut() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// Resolve host:port and connect.  Returns INVALID_SOCKET on failure.
SOCKET dialTcp(const std::string& host, int port, LogManager& lm) {
    auto log = lm.getLogger("tcp");

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;          // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
        log->error("TcpClient: getaddrinfo failed for {}:{}", host, port);
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    for (auto* a = result; a != nullptr; a = a->ai_next) {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
            break;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        log->warn("TcpClient: connect failed for {}:{}", host, port);
    } else {
        log->info("TcpClient: connected to {}:{}", host, port);
    }
    return sock;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TcpClient::TcpClient(const TcpConfig& cfg, LogManager& lm)
    : cfg_(cfg), log_manager_(lm) {
    ensureWsaInit();
}

TcpClient::~TcpClient() {
    stop();
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool TcpClient::start() {
    if (running_.load()) return true;
    if (cfg_.host.empty() || cfg_.port <= 0) {
        log_manager_.getLogger("tcp")->error("TcpClient: host/port not configured");
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&TcpClient::receiveLoop, this);
    return true;
}

void TcpClient::stop() {
    running_.store(false);
    buf_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

std::size_t TcpClient::tryRead(std::vector<uint8_t>& out, std::size_t max_bytes, int timeout_ms) {
    std::unique_lock<std::mutex> lock(buf_mutex_);
    if (timeout_ms > 0) {
        buf_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !buf_.empty() || !running_.load(); });
    }
    if (buf_.empty()) { out.clear(); return 0; }
    const std::size_t n = (std::min)(max_bytes, buf_.size());
    out.assign(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

// ---------------------------------------------------------------------------
// Receive loop
// ---------------------------------------------------------------------------

void TcpClient::receiveLoop() {
    auto log = log_manager_.getLogger("tcp");

    while (running_.load()) {
        SOCKET sock = dialTcp(cfg_.host, cfg_.port, log_manager_);
        if (sock == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::seconds(cfg_.reconnect_delay_s));
            continue;
        }

        connected_.store(true);

        constexpr int kRecvBufBytes   = 4096;
        constexpr int kRecvTimeoutMs  = 1000;   // bounds shutdown latency
        setRecvTimeout(sock, kRecvTimeoutMs);
        std::vector<char> tmp(kRecvBufBytes);

        while (running_.load()) {
            const int n = recv(sock, tmp.data(), kRecvBufBytes, 0);
            if (n == 0) break;                       // peer closed -> reconnect
            if (n < 0) {
                if (recvTimedOut()) continue;        // no data this interval -> re-check running_
                break;                               // real error -> reconnect
            }

            std::lock_guard<std::mutex> lock(buf_mutex_);
            // Drop the oldest bytes when the buffer would exceed kMaxBufSize.
            if (buf_.size() + static_cast<std::size_t>(n) > kMaxBufSize) {
                const std::size_t excess = (buf_.size() + n) - kMaxBufSize;
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(excess));
            }
            const auto* bytes = reinterpret_cast<const uint8_t*>(tmp.data());
            buf_.insert(buf_.end(), bytes, bytes + n);
            buf_cv_.notify_one();
        }

        connected_.store(false);
        closesocket(sock);

        if (running_.load()) {
            log->warn("TcpClient: connection to {}:{} lost; reconnecting in {} s",
                      cfg_.host, cfg_.port, cfg_.reconnect_delay_s);
            std::this_thread::sleep_for(std::chrono::seconds(cfg_.reconnect_delay_s));
        }
    }
    log->info("TcpClient: receive loop exited");
}

} // namespace GnssProcessor
