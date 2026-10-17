/**
 * @file test_tcp_client_silence.cpp
 * @brief TcpClient detection of a peer that stops sending without closing the socket.
 *
 * The receive timeout of the client only bounds shutdown, so a receiver that hangs with
 * its socket open used to keep the stream silent for good.  With
 * `TcpConfig::silent_timeout_s` set the client closes a connection that delivered no
 * byte within that time and dials again.  The cases run against a loopback server that
 * accepts connections and then stays quiet (or sends on a schedule).
 *
 * Also covered: the `realtime.tcp_silent_timeout_s` config keys of the three streams.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "realtime/TcpClient.hpp"
#include "support/TestTempDir.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define closesocket close
   using SOCKET = int;
#  define INVALID_SOCKET (-1)
#endif

namespace GnssProcessor {
namespace {

using Clock = std::chrono::steady_clock;

// A loopback server: accepts every connection, keeps it open, optionally sends one byte
// per `send_period_ms`.  Counts accepted connections.
class LoopbackServer {
public:
    explicit LoopbackServer(int send_period_ms = 0) : send_period_ms_(send_period_ms) {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ok_ = listener_ != INVALID_SOCKET
           && bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
           && listen(listener_, 8) == 0;
        if (ok_) {
            socklen_t len = sizeof(addr);
            getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
            port_ = ntohs(addr.sin_port);
            thread_ = std::thread([this] { serve(); });
        }
    }
    ~LoopbackServer() {
        stop_.store(true);
        if (listener_ != INVALID_SOCKET) closesocket(listener_);   // wakes accept()
        if (thread_.joinable()) thread_.join();
        for (SOCKET c : clients_) closesocket(c);
    }
    bool ok() const { return ok_; }
    int port() const { return port_; }
    int accepted() const { return accepted_.load(); }

private:
    void serve() {
        while (!stop_.load()) {
            SOCKET c = accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            accepted_.fetch_add(1);
            clients_.push_back(c);
            if (send_period_ms_ > 0) {
                senders_.emplace_back([this, c] {
                    while (!stop_.load()) {
                        const char byte = 'x';
                        if (send(c, &byte, 1, 0) <= 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(send_period_ms_));
                    }
                });
            }
        }
        stop_.store(true);
        for (auto& t : senders_) t.join();
    }

    int                send_period_ms_;
    SOCKET             listener_{INVALID_SOCKET};
    bool               ok_{false};
    int                port_{0};
    std::atomic<bool>  stop_{false};
    std::atomic<int>   accepted_{0};
    std::vector<SOCKET> clients_;
    std::thread        thread_;
    std::vector<std::thread> senders_;
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds limit) {
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

TcpConfig configFor(const LoopbackServer& server, double silent_timeout_s) {
    TcpConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = server.port();
    cfg.reconnect_delay_s = 0;
    cfg.silent_timeout_s = silent_timeout_s;
    return cfg;
}

TEST(TcpClientSilenceTest, SilentPeerIsDroppedAndRedialed) {
    LoopbackServer server;
    ASSERT_TRUE(server.ok());
    LogManager lm;
    TcpClient client(configFor(server, 0.5), lm);
    ASSERT_TRUE(client.start());

    // The server accepts and never sends: after the limit the client closes and dials again.
    EXPECT_TRUE(waitFor([&] { return server.accepted() >= 2; }, std::chrono::seconds(20)))
        << "accepted " << server.accepted();
    EXPECT_GE(client.silentDrops(), 1u);
    EXPECT_GE(client.connectionCount(), 2u);
    client.stop();
}

TEST(TcpClientSilenceTest, TimeoutOffKeepsTheSilentConnection) {
    LoopbackServer server;
    ASSERT_TRUE(server.ok());
    LogManager lm;
    TcpClient client(configFor(server, 0.0), lm);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitFor([&] { return server.accepted() >= 1; }, std::chrono::seconds(20)));

    // Longer than the limit the other case needs, several receive timeouts of the client.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_EQ(server.accepted(), 1);
    EXPECT_EQ(client.silentDrops(), 0u);
    EXPECT_TRUE(client.isConnected());
    client.stop();
}

TEST(TcpClientSilenceTest, SteadyDataKeepsTheConnection) {
    // A byte every 50 ms against a 2 s limit: a loaded machine has to stall the sender
    // for the whole limit before this case can fail.
    LoopbackServer server(/*send_period_ms=*/50);
    ASSERT_TRUE(server.ok());
    LogManager lm;
    TcpClient client(configFor(server, 2.0), lm);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitFor([&] { return server.accepted() >= 1; }, std::chrono::seconds(20)));

    std::this_thread::sleep_for(std::chrono::milliseconds(4500));   // more than two limits
    EXPECT_EQ(server.accepted(), 1);
    EXPECT_EQ(client.silentDrops(), 0u);

    std::vector<uint8_t> got;
    EXPECT_GT(client.tryRead(got, 1024, 200), 0u);                  // data is flowing
    client.stop();
}

// ---------------------------------------------------------------------------
// Config keys
// ---------------------------------------------------------------------------

class TcpSilenceConfigTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;

    bool load(const std::string& realtime_json) {
        const auto path = gnss_test::tempDir() / "tcp_silence.json";
        {
            std::ofstream out(path);
            out << "{ \"realtime\": " << realtime_json << " }";
        }
        const bool ok = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return ok;
    }
};

TEST_F(TcpSilenceConfigTest, DefaultsOffOnEveryStream) {
    ASSERT_TRUE(load(R"({})"));
    const auto& rt = config.getRealTimeConfig();
    EXPECT_EQ(rt.tcp_silent_timeout_s, 0.0);
    EXPECT_EQ(rt.corrections.tcp_silent_timeout_s, 0.0);
    EXPECT_EQ(rt.ephemeris.tcp_silent_timeout_s, 0.0);
}

TEST_F(TcpSilenceConfigTest, EachStreamTakesItsOwnValue) {
    ASSERT_TRUE(load(R"({
        "tcp_silent_timeout_s": 20,
        "corrections": {"enabled": true, "input_type": "tcp", "tcp_host": "h", "tcp_silent_timeout_s": 30.5},
        "ephemeris":   {"enabled": true, "input_type": "tcp", "tcp_host": "h", "tcp_silent_timeout_s": 180}
    })"));
    const auto& rt = config.getRealTimeConfig();
    EXPECT_DOUBLE_EQ(rt.tcp_silent_timeout_s, 20.0);
    EXPECT_DOUBLE_EQ(rt.corrections.tcp_silent_timeout_s, 30.5);
    EXPECT_DOUBLE_EQ(rt.ephemeris.tcp_silent_timeout_s, 180.0);
}

TEST_F(TcpSilenceConfigTest, NegativeValueReadsAsOff) {
    ASSERT_TRUE(load(R"({"tcp_silent_timeout_s": -5})"));
    EXPECT_EQ(config.getRealTimeConfig().tcp_silent_timeout_s, 0.0);
}

} // namespace
} // namespace GnssProcessor
