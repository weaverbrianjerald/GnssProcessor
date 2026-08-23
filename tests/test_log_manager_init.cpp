/**
 * @file test_log_manager_init.cpp
 * @brief LogManager::initialize() non-throw contract.
 *
 * Scope: one narrow property, paid for in a real failure — initialize() must
 * NEVER let an exception escape.  Its documented behaviour is "fall back to a
 * console-only logger and return false", and every caller is written against
 * that: nothing checks the return, and several call it from places that cannot
 * unwind (a gtest SetUpTestSuite, GnssConfig's own catch handler).
 *
 * The defect this pins: the log-directory preparation step (create the dir,
 * delete stale *.log so the run overwrites rather than appends) used the
 * THROWING std::filesystem overloads while the catch clause named only
 * spdlog::spdlog_ex.  std::filesystem_error is not an spdlog_ex, so it went
 * straight through.  On Windows the CRT opens log files with _SH_DENYNO, which
 * grants FILE_SHARE_READ|WRITE but NOT FILE_SHARE_DELETE — so a *.log still
 * held open by any live process makes that remove() fail with
 * ERROR_SHARING_VIOLATION.  Concurrent runs are routine in this project, so
 * this fired in the field: a second gnss_tests.exe holding the shared temp
 * gpt3.log open killed Gpt3Test::SetUpTestSuite, which gtest reported as
 *   [  FAILED  ] Gpt3Test: SetUpTestSuite or TearDownTestSuite
 * with all three cases SKIPPED as "GPT3 grid fixture not present" and the
 * binary exiting 1 — a diagnosis that pointed at the fixture, not the logger.
 *
 * @see test_turbo_edit_resolver.cpp  LogManagerFallback — the getLogger()
 *                                    (pre-initialize) half of the same contract
 * @see test_atmosphere_models.cpp    Gpt3Test — where this surfaced
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

/// Per-process, per-case scratch directory.  A fixed shared path is the very
/// hazard under test, so these must not collide between concurrent binaries.
fs::path scratchDir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path()
                       / ("gnss_logmgr_init_" + tag + "_" + std::to_string(GNSS_TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// Registry hygiene, TARGETED — mirrors the rationale in
/// test_codebias_network_config.cpp: initialize() registers "gnss_processor",
/// which another live LogManager in this binary may already own.  Drop only
/// that one name, never spdlog::drop_all() (which wipes loggers other live
/// objects still hold and faults RtcmParserDecodeTest with 0xC0000005).
struct DefaultLoggerRegistryGuard {
    DefaultLoggerRegistryGuard()  { spdlog::drop("gnss_processor"); }
    ~DefaultLoggerRegistryGuard() { spdlog::drop("gnss_processor"); }
};

} // namespace

// A stale .log that CANNOT be deleted must not take the caller down with it.
//
// The open ofstream below is the portable stand-in for "another process still
// has this file open": on Windows it makes remove() fail with a sharing
// violation, which is exactly the field trigger.  On POSIX an open file unlinks
// happily, so there the assertion is merely trivially satisfied — the
// non-throw contract is the same either way.
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

// Calling initialize() twice on the SAME manager re-initializes rather than
// throwing.  The duplicate-name register_logger() throw used to be "handled"
// by a catch clause that immediately re-ran the same registration and threw
// again, uncaught — so this pins both the re-init path and that the fallback
// arm is itself exception-safe.
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
// "gnss_processor" collides and its scrub meets the first one's OPEN log
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
