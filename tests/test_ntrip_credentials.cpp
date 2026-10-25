/**
 * @file test_ntrip_credentials.cpp
 * @brief NTRIP login lookup: the config's password, else the netrc entry of the caster host, else none.
 *
 * Committed configs carry no password.  A stream whose config has an empty password reads
 * its login from the user's netrc file, by caster host; a config that still carries a
 * password behaves as before.  The cases pin
 *   - NetrcParse / NetrcFind / NetrcRead: the netrc syntax that is read and the entries that count;
 *   - NetrcLocate: the file search order, which DataDownloader shares;
 *   - NtripCredentialsOrder: the lookup order and what each rung leaves in the result;
 *   - NtripClientLogin: the Authorization header a client really sends to a loopback caster,
 *     one case per rung of the order;
 *   - TrackedRealtimeConfigs: the shipped config/realtime_*.json keep every password empty.
 * Every login, password and host below is synthetic.
 */

#include <gtest/gtest.h>

#include "logging/LogManager.hpp"
#include "products/NetrcFile.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/NtripCredentials.hpp"
#include "support/TestTempDir.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
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

namespace fs = std::filesystem;

const std::string kHost = "caster.example.test";

/// Write @p text to a file called @p name in this test's scratch directory.
fs::path writeFile(const std::string& name, const std::string& text) {
    const fs::path p = gnss_test::tempPath(name);
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return p;
}

/// A home directory in the scratch directory holding the named netrc files (name -> text).
fs::path makeHome(const std::string& dir, const std::vector<std::pair<std::string, std::string>>& files) {
    const fs::path home = gnss_test::tempPath(dir);
    fs::create_directories(home);
    for (const auto& [name, text] : files) writeFile(dir + "/" + name, text);
    return home;
}

// ---------------------------------------------------------------------------
// NetrcParse
// ---------------------------------------------------------------------------

TEST(NetrcParse, OneLineAndMultiLineFormsGiveTheSameEntries) {
    const auto one = parseNetrc("machine h1 login u1 password p1 machine h2 login u2 password p2");
    const auto many = parseNetrc("machine h1\n  login u1\n  password p1\nmachine h2 login u2\npassword p2\n");
    ASSERT_EQ(one.size(), 2u);
    ASSERT_EQ(many.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(one[i].machine, many[i].machine);
        EXPECT_EQ(one[i].login, many[i].login);
        EXPECT_EQ(one[i].password, many[i].password);
    }
    EXPECT_EQ(one[1].machine, "h2");
    EXPECT_EQ(one[1].login, "u2");
    EXPECT_EQ(one[1].password, "p2");
}

TEST(NetrcParse, QuotedTokensKeepBlanksAndBackslashEscapes) {
    const auto e = parseNetrc(R"(machine h login "two words" password "a b\"c\\d")");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].login, "two words");
    EXPECT_EQ(e[0].password, "a b\"c\\d");
}

TEST(NetrcParse, HashInsideATokenIsPartOfThePassword) {
    const auto e = parseNetrc("machine h login u password pa#ss\n");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].password, "pa#ss");
}

TEST(NetrcParse, CommentsRunToTheEndOfTheLine) {
    const auto e = parseNetrc("# first line\nmachine h login u # trailing note\npassword p\n");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].login, "u");
    EXPECT_EQ(e[0].password, "p");
}

TEST(NetrcParse, DefaultEntryAndMacroBodyAreNotEntries) {
    const auto e = parseNetrc(
        "machine a login u1 password p1\n"
        "default login ud password pd\n"
        "macdef init\n"
        "machine zz login uz password pz\n"
        "\n"
        "machine b login u2 password p2\n");
    ASSERT_EQ(e.size(), 2u);                 // a and b: not the default, not the machine line inside the macro
    EXPECT_EQ(e[0].machine, "a");
    EXPECT_EQ(e[0].login, "u1");             // the default's login did not leak into a
    EXPECT_EQ(e[0].password, "p1");
    EXPECT_EQ(e[1].machine, "b");
    EXPECT_EQ(e[1].password, "p2");
}

TEST(NetrcParse, AccountValueIsSkipped) {
    const auto e = parseNetrc("machine a account acc login u password p");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].login, "u");
    EXPECT_EQ(e[0].password, "p");
}

TEST(NetrcParse, CrLfLineEndsAndKeywordCaseAreAccepted) {
    const auto e = parseNetrc("MACHINE a\r\nLOGIN u\r\nPassword p\r\n");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].machine, "a");
    EXPECT_EQ(e[0].login, "u");
    EXPECT_EQ(e[0].password, "p");
}

TEST(NetrcParse, TextThatIsNotNetrcGivesNoEntries) {
    EXPECT_TRUE(parseNetrc("").empty());
    EXPECT_TRUE(parseNetrc("machine").empty());                 // keyword with no value
    EXPECT_TRUE(parseNetrc("login u password p").empty());      // no machine to attach to
    EXPECT_TRUE(parseNetrc("just some words here").empty());
}

// ---------------------------------------------------------------------------
// NetrcFind
// ---------------------------------------------------------------------------

TEST(NetrcFind, HostMatchIgnoresCaseAndLoginMatchesExactly) {
    const auto entries = parseNetrc(
        "machine Caster.Example.TEST login alice password pa\n"
        "machine caster.example.test login bob password pb\n");
    const NetrcEntry* any = findNetrcEntry(entries, kHost);
    ASSERT_NE(any, nullptr);
    EXPECT_EQ(any->login, "alice");                              // no login asked for: the first usable entry
    const NetrcEntry* bob = findNetrcEntry(entries, kHost, "bob");
    ASSERT_NE(bob, nullptr);
    EXPECT_EQ(bob->password, "pb");
    EXPECT_EQ(findNetrcEntry(entries, kHost, "Bob"), nullptr);   // logins compare exactly
    EXPECT_EQ(findNetrcEntry(entries, "other.example.test"), nullptr);
}

TEST(NetrcFind, EntryWithoutALoginOrWithoutAPasswordIsNotUsable) {
    EXPECT_EQ(findNetrcEntry(parseNetrc("machine h login u"), "h"), nullptr);
    EXPECT_EQ(findNetrcEntry(parseNetrc("machine h password p"), "h"), nullptr);
    const auto entries = parseNetrc("machine h login u1\nmachine h login u2 password p2\n");
    const NetrcEntry* e = findNetrcEntry(entries, "h");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->login, "u2");                                   // the unusable first entry is skipped
}

// ---------------------------------------------------------------------------
// NetrcRead
// ---------------------------------------------------------------------------

TEST(NetrcRead, MissingFileIsUnreadable) {
    EXPECT_EQ(readNetrcFile(gnss_test::tempPath("absent")).status, NetrcReadStatus::Unreadable);
}

TEST(NetrcRead, Utf8ByteOrderMarkIsSkipped) {
    const auto p = writeFile("bom_netrc", "\xEF\xBB\xBF" "machine h login u password p\n");
    const auto r = readNetrcFile(p);
    ASSERT_EQ(r.status, NetrcReadStatus::Ok);
    ASSERT_EQ(r.entries.size(), 1u);
    EXPECT_EQ(r.entries[0].machine, "h");
}

TEST(NetrcRead, Utf16FileIsReportedNotMisreadAsEmpty) {
    // What Windows PowerShell 5 writes for "text > file": a UTF-16LE byte-order mark and a NUL after each character.
    const std::string utf16le("\xFF\xFE" "m\0a\0c\0h\0i\0n\0e\0 \0h\0", 20);
    EXPECT_EQ(readNetrcFile(writeFile("utf16_netrc", utf16le)).status, NetrcReadStatus::NotText);
    EXPECT_EQ(readNetrcFile(writeFile("nul_netrc", std::string("machine h\0login u", 17))).status,
              NetrcReadStatus::NotText);
}

// ---------------------------------------------------------------------------
// NetrcLocate (the search DataDownloader shares)
// ---------------------------------------------------------------------------

TEST(NetrcLocate, ConfiguredFileWinsOverTheHomeDirectory) {
    const auto home = makeHome("home", {{".netrc", "machine h login u password p"}});
    const auto mine = writeFile("mine_netrc", "machine h login u password p");
    const auto found = locateNetrcFile(mine, home);
    EXPECT_EQ(found.origin, NetrcLocation::Origin::Configured);
    EXPECT_EQ(found.path, mine);
    EXPECT_FALSE(found.configured_missing);
}

TEST(NetrcLocate, MissingConfiguredPathIsReportedAndTheSearchGoesOn) {
    const auto home = makeHome("home", {{"_netrc", "machine h login u password p"}});
    const auto found = locateNetrcFile(gnss_test::tempPath("absent_netrc"), home);
    EXPECT_TRUE(found.configured_missing);
    EXPECT_EQ(found.origin, NetrcLocation::Origin::UnderscoreNetrc);
    EXPECT_EQ(found.path, home / "_netrc");
}

TEST(NetrcLocate, DotNetrcComesBeforeUnderscoreNetrc) {
    const auto home = makeHome("home", {{"_netrc", "x"}, {".netrc", "x"}});
    const auto found = locateNetrcFile({}, home);
    EXPECT_EQ(found.origin, NetrcLocation::Origin::DotNetrc);
    EXPECT_EQ(found.path, home / ".netrc");
}

TEST(NetrcLocate, NothingFoundReportsBothCandidates) {
    const auto home = makeHome("home", {});
    const auto found = locateNetrcFile({}, home);
    EXPECT_EQ(found.origin, NetrcLocation::Origin::None);
    EXPECT_TRUE(found.path.empty());
    EXPECT_FALSE(found.no_home);
    EXPECT_EQ(found.dot_netrc, home / ".netrc");
    EXPECT_EQ(found.underscore_netrc, home / "_netrc");
}

TEST(NetrcLocate, NoHomeDirectoryMeansNoAutoDiscovery) {
    const auto found = locateNetrcFile({}, fs::path{});
    EXPECT_EQ(found.origin, NetrcLocation::Origin::None);
    EXPECT_TRUE(found.no_home);
}

// ---------------------------------------------------------------------------
// NtripCredentialsOrder
// ---------------------------------------------------------------------------

constexpr const char* kUser = "tst-user";
constexpr const char* kConfigPassword = "tst-config-pass";
constexpr const char* kNetrcPassword = "tst-netrc-pass";

std::string entryFor(const std::string& host, const std::string& login, const std::string& password) {
    return "machine " + host + " login " + login + " password " + password + "\n";
}

TEST(NtripCredentialsOrder, ConfigPasswordIsUsedAsWrittenAndNoFileIsRead) {
    const auto netrc = writeFile("netrc", entryFor(kHost, kUser, kNetrcPassword));
    const auto with_file = resolveNtripCredentials(kHost, kUser, kConfigPassword, netrc, fs::path{});
    EXPECT_EQ(with_file.origin, NtripCredentialOrigin::Config);
    EXPECT_EQ(with_file.username, kUser);
    EXPECT_EQ(with_file.password, kConfigPassword);          // the file's different password is not consulted
    EXPECT_TRUE(with_file.netrc_file.empty());

    const auto no_file = resolveNtripCredentials(kHost, kUser, kConfigPassword,
                                                 gnss_test::tempPath("absent"), fs::path{});
    EXPECT_EQ(no_file.origin, NtripCredentialOrigin::Config);   // and it does not matter that no file exists
    EXPECT_EQ(no_file.password, kConfigPassword);
}

TEST(NtripCredentialsOrder, EmptyPasswordTakesTheNetrcEntryOfTheConfiguredLogin) {
    const auto netrc = writeFile("netrc",
        entryFor("other.example.test", kUser, "tst-other-host-pass") +
        entryFor(kHost, "tst-someone-else", "tst-else-pass") +
        entryFor(kHost, kUser, kNetrcPassword));
    const auto got = resolveNtripCredentials(kHost, kUser, "", netrc, fs::path{});
    EXPECT_EQ(got.origin, NtripCredentialOrigin::Netrc);
    EXPECT_EQ(got.username, kUser);
    EXPECT_EQ(got.password, kNetrcPassword);
    EXPECT_EQ(got.netrc_file, netrc);
}

TEST(NtripCredentialsOrder, NoLoginInTheConfigTakesLoginAndPasswordFromTheEntry) {
    const auto netrc = writeFile("netrc", entryFor(kHost, kUser, kNetrcPassword));
    const auto got = resolveNtripCredentials(kHost, "", "", netrc, fs::path{});
    EXPECT_EQ(got.origin, NtripCredentialOrigin::Netrc);
    EXPECT_EQ(got.username, kUser);
    EXPECT_EQ(got.password, kNetrcPassword);
}

TEST(NtripCredentialsOrder, ConfiguredLoginAbsentFromTheFileIsLeftAlone) {
    const auto netrc = writeFile("netrc", entryFor(kHost, kUser, kNetrcPassword));
    const auto got = resolveNtripCredentials(kHost, "tst-other-user", "", netrc, fs::path{});
    EXPECT_EQ(got.origin, NtripCredentialOrigin::NoNetrcEntry);
    EXPECT_EQ(got.username, "tst-other-user");               // the config's login, as before the lookup existed
    EXPECT_TRUE(got.password.empty());
    EXPECT_EQ(got.netrc_file, netrc);
}

TEST(NtripCredentialsOrder, HostMissingFromTheFileLeavesAnAnonymousStreamAnonymous) {
    // The default entry would hand one login to every host; it is never used.
    const auto netrc = writeFile("netrc",
        entryFor("other.example.test", kUser, kNetrcPassword) +
        "default login tst-default password tst-default-pass\n");
    const auto got = resolveNtripCredentials(kHost, "", "", netrc, fs::path{});
    EXPECT_EQ(got.origin, NtripCredentialOrigin::NoNetrcEntry);
    EXPECT_TRUE(got.username.empty());
    EXPECT_TRUE(got.password.empty());
}

TEST(NtripCredentialsOrder, NoNetrcFileAnywhereLeavesTheConfigAlone) {
    const auto home = makeHome("home", {});
    const auto got = resolveNtripCredentials(kHost, kUser, "", fs::path{}, home);
    EXPECT_EQ(got.origin, NtripCredentialOrigin::NoNetrcFile);
    EXPECT_EQ(got.username, kUser);
    EXPECT_TRUE(got.password.empty());
    EXPECT_TRUE(got.netrc_file.empty());
}

TEST(NtripCredentialsOrder, DefaultLocationsAreSearchedWhenNoPathIsConfigured) {
    const auto only_underscore = makeHome("home_u", {{"_netrc", entryFor(kHost, kUser, "tst-underscore-pass")}});
    EXPECT_EQ(resolveNtripCredentials(kHost, kUser, "", fs::path{}, only_underscore).password,
              "tst-underscore-pass");

    const auto both = makeHome("home_b", {{"_netrc", entryFor(kHost, kUser, "tst-underscore-pass")},
                                          {".netrc", entryFor(kHost, kUser, "tst-dot-pass")}});
    EXPECT_EQ(resolveNtripCredentials(kHost, kUser, "", fs::path{}, both).password, "tst-dot-pass");
}

TEST(NtripCredentialsOrder, MissingConfiguredPathFallsBackToTheHomeDirectory) {
    const auto home = makeHome("home", {{".netrc", entryFor(kHost, kUser, kNetrcPassword)}});
    const auto got = resolveNtripCredentials(kHost, kUser, "", gnss_test::tempPath("absent"), home);
    EXPECT_EQ(got.origin, NtripCredentialOrigin::Netrc);
    EXPECT_EQ(got.password, kNetrcPassword);
}

TEST(NtripCredentialsOrder, UnreadableFileLeavesTheConfigAlone) {
    const auto netrc = writeFile("netrc", std::string("\xFF\xFE" "m\0", 4));
    const auto got = resolveNtripCredentials(kHost, kUser, "", netrc, fs::path{});
    EXPECT_EQ(got.origin, NtripCredentialOrigin::UnreadableNetrcFile);
    EXPECT_EQ(got.username, kUser);
    EXPECT_TRUE(got.password.empty());
}

TEST(NtripCredentialsOrder, ChangedPasswordIsReadAgainOnTheNextCall) {
    const auto netrc = writeFile("netrc", entryFor(kHost, kUser, "tst-first-pass"));
    EXPECT_EQ(resolveNtripCredentials(kHost, kUser, "", netrc, fs::path{}).password, "tst-first-pass");
    writeFile("netrc", entryFor(kHost, kUser, "tst-rotated-pass"));
    EXPECT_EQ(resolveNtripCredentials(kHost, kUser, "", netrc, fs::path{}).password, "tst-rotated-pass");
}

TEST(NtripCredentialsOrder, DescriptionNamesTheFileButNeverTheLoginOrThePassword) {
    const auto netrc = writeFile("netrc", entryFor(kHost, kUser, kNetrcPassword));
    const auto home = makeHome("home", {});
    const NtripCredentials cases[] = {
        resolveNtripCredentials(kHost, kUser, kConfigPassword, netrc, fs::path{}),            // Config
        resolveNtripCredentials(kHost, kUser, "", netrc, fs::path{}),                         // Netrc
        resolveNtripCredentials(kHost, "tst-other-user", "", netrc, fs::path{}),              // NoNetrcEntry
        resolveNtripCredentials(kHost, kUser, "", fs::path{}, home),                          // NoNetrcFile
        resolveNtripCredentials(kHost, kUser, "", writeFile("utf16", std::string("\xFF\xFE", 2)), fs::path{}),
    };
    for (const auto& c : cases) {
        const std::string text = describeNtripCredentials(c);
        for (const char* secret : {kUser, "tst-other-user", kConfigPassword, kNetrcPassword})
            EXPECT_EQ(text.find(secret), std::string::npos) << text;
    }
    EXPECT_TRUE(describeNtripCredentials(cases[0]).empty());                                  // the config's own login: nothing to say
    EXPECT_NE(describeNtripCredentials(cases[1]).find(netrc.string()), std::string::npos);    // the file is named
    for (std::size_t i = 1; i < std::size(cases); ++i) EXPECT_FALSE(describeNtripCredentials(cases[i]).empty());
}

// ---------------------------------------------------------------------------
// NtripClientLogin: the header a client sends to a loopback caster
// ---------------------------------------------------------------------------

/// Base64 of @p in (the form of the HTTP Basic Authorization header).
std::string base64(const std::string& in) {
    static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8)
                         | static_cast<unsigned char>(in[i + 2]);
        out += digits[(v >> 18) & 63]; out += digits[(v >> 12) & 63]; out += digits[(v >> 6) & 63]; out += digits[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += digits[(v >> 18) & 63]; out += digits[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += digits[(v >> 18) & 63]; out += digits[(v >> 12) & 63]; out += digits[(v >> 6) & 63]; out += '=';
    }
    return out;
}

/// Sets an environment variable for one scope and restores the previous state.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) { had_ = true; old_ = old; }
        set(value);
    }
    ~ScopedEnv() { if (had_) set(old_.c_str()); else unsetVar(); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }
    void unsetVar() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    bool        had_{false};
    std::string old_;
};

/// A loopback caster: answers every connection with "200 OK" and a few bytes, keeps the
/// request head of each, and closes the connection shortly after replying.
class RecordingCaster {
public:
    RecordingCaster() {
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
           && listen(listener_, 4) == 0;
        if (ok_) {
            socklen_t len = sizeof(addr);
            getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
            port_ = ntohs(addr.sin_port);
            thread_ = std::thread([this] { serve(); });
        }
    }
    ~RecordingCaster() {
        stop_.store(true);
        if (listener_ != INVALID_SOCKET) closesocket(listener_);   // wakes accept()
        if (thread_.joinable()) thread_.join();
    }
    bool ok() const { return ok_; }
    int port() const { return port_; }

    /// The request head (request line and headers) of the first connection; empty until one arrived.
    std::string firstRequest() {
        std::lock_guard<std::mutex> lock(mutex_);
        return heads_.empty() ? std::string{} : heads_.front();
    }

private:
    void serve() {
        while (!stop_.load()) {
            SOCKET c = accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
#ifdef _WIN32
            DWORD timeout_ms = 3000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#endif
            std::string head;
            char buf[1024];
            while (head.find("\r\n\r\n") == std::string::npos) {
                const int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                head.append(buf, static_cast<std::size_t>(n));
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                heads_.push_back(head);
            }
            const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: gnss/data\r\nConnection: close\r\n\r\nabc";
            send(c, reply.data(), static_cast<int>(reply.size()), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));   // time for the test to stop the client first
            closesocket(c);
        }
    }

    SOCKET            listener_{INVALID_SOCKET};
    bool              ok_{false};
    int               port_{0};
    std::atomic<bool> stop_{false};
    std::mutex        mutex_;
    std::vector<std::string> heads_;
    std::thread       thread_;
};

/// The value of the Authorization header in @p head, or empty when there is none.
std::string authorizationOf(const std::string& head) {
    std::string lower = head;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string key = "\r\nauthorization: ";
    const auto at = lower.find(key);
    if (at == std::string::npos) return {};
    const auto from = at + key.size();
    return head.substr(from, head.find("\r\n", from) - from);
}

/// Run a client against a fresh recording caster and return the request head it sent.
std::string requestHeadOf(NtripConfig cfg) {
    RecordingCaster caster;
    EXPECT_TRUE(caster.ok());
    cfg.host = "127.0.0.1";
    cfg.port = caster.port();
    cfg.mountpoint = "TEST00XXX0";
    cfg.reconnect_delay_s = 0;
    LogManager lm;
    NtripClient client(cfg, lm);
    EXPECT_TRUE(client.start());
    std::string head;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (head.empty() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        head = caster.firstRequest();
    }
    client.stop();
    return head;
}

class NtripClientLogin : public ::testing::Test {
protected:
    // A loopback connection must not be routed through a proxy named in the environment.
    ScopedEnv no_proxy_upper_{"NO_PROXY", "127.0.0.1,localhost"};
    ScopedEnv no_proxy_lower_{"no_proxy", "127.0.0.1,localhost"};
};

TEST_F(NtripClientLogin, ConfigPasswordIsSentAndTheNetrcIsNotConsulted) {
    NtripConfig cfg;
    cfg.username = kUser;
    cfg.password = kConfigPassword;
    cfg.netrc_path = writeFile("netrc", entryFor("127.0.0.1", kUser, kNetrcPassword));
    const std::string head = requestHeadOf(cfg);
    ASSERT_FALSE(head.empty());
    EXPECT_EQ(authorizationOf(head), "Basic " + base64(std::string(kUser) + ":" + kConfigPassword));
}

TEST_F(NtripClientLogin, EmptyPasswordSendsTheNetrcPasswordOfTheCasterHost) {
    NtripConfig cfg;
    cfg.username = kUser;
    cfg.netrc_path = writeFile("netrc", entryFor("127.0.0.1", kUser, kNetrcPassword));
    const std::string head = requestHeadOf(cfg);
    ASSERT_FALSE(head.empty());
    EXPECT_EQ(authorizationOf(head), "Basic " + base64(std::string(kUser) + ":" + kNetrcPassword));
}

TEST_F(NtripClientLogin, NoLoginInTheConfigSendsTheNetrcLoginAndPassword) {
    NtripConfig cfg;
    cfg.netrc_path = writeFile("netrc", entryFor("127.0.0.1", kUser, kNetrcPassword));
    const std::string head = requestHeadOf(cfg);
    ASSERT_FALSE(head.empty());
    EXPECT_EQ(authorizationOf(head), "Basic " + base64(std::string(kUser) + ":" + kNetrcPassword));
}

TEST_F(NtripClientLogin, LoginWithoutAnyPasswordIsSentWithAnEmptyPasswordAsBefore) {
    NtripConfig cfg;
    cfg.username = kUser;
    cfg.netrc_path = writeFile("netrc", entryFor("other.example.test", kUser, kNetrcPassword));
    const std::string head = requestHeadOf(cfg);
    ASSERT_FALSE(head.empty());
    EXPECT_EQ(authorizationOf(head), "Basic " + base64(std::string(kUser) + ":"));
}

TEST_F(NtripClientLogin, AnonymousStreamWithNoNetrcEntrySendsNoAuthorization) {
    NtripConfig cfg;
    cfg.netrc_path = writeFile("netrc", entryFor("other.example.test", kUser, kNetrcPassword));
    const std::string head = requestHeadOf(cfg);
    ASSERT_FALSE(head.empty());
    EXPECT_TRUE(authorizationOf(head).empty());
}

// ---------------------------------------------------------------------------
// TrackedRealtimeConfigs: no password in a shipped config
// ---------------------------------------------------------------------------

// The shipped real-time configs are copied, edited and committed; a password pasted into one
// would be published with the next commit.  The login belongs in the netrc file.  The check is
// on the text, so a commented-out line counts too.  It reports file and line, never the value.
TEST(TrackedRealtimeConfigs, NoPasswordIsKeptInAShippedConfig) {
    const fs::path dir = fs::path(GNSS_PROJECT_ROOT) / "config";
    ASSERT_TRUE(fs::is_directory(dir)) << dir.string();
    const std::regex password(R"re("(ntrip_password|password)"\s*:\s*"[^"]+")re");

    int checked = 0;
    std::string offenders;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("realtime_", 0) != 0 || entry.path().extension() != ".json") continue;
        ++checked;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            if (line.find("password") != std::string::npos && std::regex_search(line, password))
                offenders += " " + name + ":" + std::to_string(n);
        }
    }
    EXPECT_GE(checked, 3) << "expected the shipped config/realtime_*.json files in " << dir.string();
    EXPECT_TRUE(offenders.empty())
        << "A password is set in:" << offenders
        << ".  Keep the key empty and put the login in the netrc file "
           "(docs/onboarding/02_code_tour.md, section \"NTRIP logins\").";
}

} // namespace
} // namespace GnssProcessor
