/**
 * @file test_log_manager_init.cpp
 * @brief LogManager::initialize() non-throw contract.
 *
 * Scope: one narrow property.  initialize() never lets an exception escape.  Its
 * documented behaviour is "fall back to a console-only logger and return false", and
 * every caller relies on that: none checks the return, and several call it from places
 * that cannot unwind (a gtest SetUpTestSuite, GnssConfig's own catch handler).
 *
 * The log-directory preparation step (create the directory, delete stale *.log so the
 * run overwrites rather than appends) has to use the non-throwing std::filesystem
 * overloads.  On Windows the CRT opens log files with _SH_DENYNO, which grants
 * FILE_SHARE_READ|WRITE but not FILE_SHARE_DELETE, so remove() on a *.log still held
 * open by any live process fails with ERROR_SHARING_VIOLATION.  Concurrent runs are
 * routine in this project; a second gnss_tests.exe holding the shared temp gpt3.log
 * open is one example.  A std::filesystem_error escaping from Gpt3Test::SetUpTestSuite
 * is reported by gtest as
 *   [  FAILED  ] Gpt3Test: SetUpTestSuite or TearDownTestSuite
 * with all three cases reported as skipped ("GPT3 grid fixture not present") and the binary
 * exiting 1, a report that points at the fixture rather than the logger.
 *
 * @see test_turbo_edit_resolver.cpp  LogManagerFallback: the getLogger()
 *                                    (pre-initialize) half of the same contract
 * @see test_atmosphere_models.cpp    Gpt3Test, the suite that exposed the failure
 */

#include <gtest/gtest.h>

#include "logging/LogManager.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define GNSS_TEST_GETPID _getpid
#else
#include <unistd.h>
#define GNSS_TEST_GETPID getpid
#endif

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// Per-process, per-case scratch directory.  A fixed shared path is the hazard under
/// test, so these directories must not collide between concurrent binaries.
fs::path scratchDir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path()
                       / ("gnss_logmgr_init_" + tag + "_" + std::to_string(GNSS_TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// Targeted registry cleanup, for the reason given in
/// test_codebias_network_config.cpp: initialize() registers "gnss_processor", which
/// another live LogManager in this binary may already own.  Only that one name is
/// dropped, not spdlog::drop_all(), which wipes loggers that other live objects still
/// hold and faults RtcmParserDecodeTest with 0xC0000005.
struct DefaultLoggerRegistryGuard {
    DefaultLoggerRegistryGuard()  { spdlog::drop("gnss_processor"); }
    ~DefaultLoggerRegistryGuard() { spdlog::drop("gnss_processor"); }
};

} // namespace

// A stale .log that cannot be deleted does not make initialize() throw.
//
// The open ofstream below stands in portably for "another process still has this
// file open": on Windows it makes remove() fail with a sharing violation, the same
// failure concurrent runs produce.  On POSIX an open file can be unlinked, so there
// the assertion is trivially satisfied; the non-throw contract is the same either way.
TEST(LogManagerInit, UndeletableStaleLogDoesNotThrow) {
    const fs::path dir = scratchDir("locked");
    const fs::path locked = dir / "stale.log";

    std::ofstream holder(locked);
    ASSERT_TRUE(holder.is_open());
    holder << "held open across initialize()\n";
    holder.flush();

    DefaultLoggerRegistryGuard guard;
    LogManager lm;
    EXPECT_NO_THROW(lm.initialize(dir, LogLevel::Off));

    // Degrading is allowed; being unusable is not.  Whichever branch ran, the
    // manager must hand back a live logger that can be written through.
    auto logger = lm.getLogger();
    ASSERT_NE(logger, nullptr);
    EXPECT_NO_THROW(logger->info("usable after a locked-stale-log initialize"));

    holder.close();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The scrub itself still has to work: a deletable stale .log is removed, so a
// run overwrites rather than appends.  Without this, "never throws" could be
// satisfied by simply not scrubbing at all.
TEST(LogManagerInit, DeletableStaleLogIsStillRemoved) {
    const fs::path dir = scratchDir("scrub");
    const fs::path stale = dir / "previous_run.log";
    const fs::path keep  = dir / "keep_me.csv";
    { std::ofstream f(stale); f << "from an earlier run\n"; }
    { std::ofstream f(keep);  f << "not a .log — must survive\n"; }
    ASSERT_TRUE(fs::exists(stale));

    DefaultLoggerRegistryGuard guard;
    LogManager lm;
    EXPECT_NO_THROW(lm.initialize(dir, LogLevel::Off));

    EXPECT_FALSE(fs::exists(stale)) << "stale .log should have been scrubbed";
    EXPECT_TRUE(fs::exists(keep))   << "the scrub must only touch .log files";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// A log directory that does not exist yet is created, not fatal.
TEST(LogManagerInit, MissingLogDirectoryIsCreated) {
    const fs::path dir = scratchDir("mkdir") / "nested" / "logs";
    ASSERT_FALSE(fs::exists(dir));

    DefaultLoggerRegistryGuard guard;
    LogManager lm;
    EXPECT_NO_THROW(lm.initialize(dir, LogLevel::Off));
    EXPECT_TRUE(fs::exists(dir));

    std::error_code ec;
    fs::remove_all(dir.parent_path().parent_path(), ec);
}

// Calling initialize() twice on the same manager re-initializes rather than
// throwing: the second call drops the registry (spdlog::drop_all) before registering
// "gnss_processor" again, and the manager still hands back a usable logger.  The
// duplicate-name path through the exception-safe fallback arm is exercised by
// SecondManagerOverSameDirectoryDoesNotThrow below.
TEST(LogManagerInit, ReinitializeDoesNotThrow) {
    const fs::path dir_a = scratchDir("reinit_a");
    const fs::path dir_b = scratchDir("reinit_b");

    DefaultLoggerRegistryGuard guard;
    LogManager lm;
    EXPECT_NO_THROW(lm.initialize(dir_a, LogLevel::Off));
    EXPECT_NO_THROW(lm.initialize(dir_b, LogLevel::Off));

    auto logger = lm.getLogger();
    ASSERT_NE(logger, nullptr);
    EXPECT_NO_THROW(logger->info("usable after re-initialize"));

    std::error_code ec;
    fs::remove_all(dir_a, ec);
    fs::remove_all(dir_b, ec);
}

// Two managers initializing into the same directory is the concurrent-run
// shape reduced to one process: the second one's registration of
// "gnss_processor" collides and its scrub meets the first one's open log
// files.  Neither may throw.
TEST(LogManagerInit, SecondManagerOverSameDirectoryDoesNotThrow) {
    const fs::path dir = scratchDir("shared");

    DefaultLoggerRegistryGuard guard;
    LogManager first;
    ASSERT_NO_THROW(first.initialize(dir, LogLevel::Off));
    // Force a real file sink so a .log is open when the second manager scrubs.
    auto first_named = first.getLogger("shared_probe");
    ASSERT_NE(first_named, nullptr);
    first_named->warn("open file sink");
    first_named->flush();

    LogManager second;
    EXPECT_NO_THROW(second.initialize(dir, LogLevel::Off));
    auto logger = second.getLogger();
    ASSERT_NE(logger, nullptr);
    EXPECT_NO_THROW(logger->info("second manager usable"));

    spdlog::drop("shared_probe");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace GnssProcessor
