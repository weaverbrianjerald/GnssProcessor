/**
 * @file test_safe_logger.cpp
 * @brief Direct unit tests for include/logging/SafeLogger.hpp.
 *
 * The RTCM decode guards in test_rtcm_logger_lifetime.cpp exercise this header
 * only indirectly, and only for "did not crash". These tests pin the three
 * properties the header actually promises, each of which a plausible wrong fix
 * would violate while still passing a crash-only test:
 *
 *   1. NEVER NULL — a dropped registry yields a usable logger, not nullptr.
 *   2. THE MESSAGE SURVIVES — it is emitted to the fallback, not swallowed.
 *      A fix shaped `if (auto l = spdlog::default_logger()) l->warn(...)` is
 *      crash-free and silently loses the diagnostic; only an output-bound
 *      assertion tells the two apart.
 *   3. PER-CALL RESOLUTION — a logger swapped between calls is honoured. This
 *      is what distinguishes the helper from the cached-accessor shape it
 *      replaces, and it is precisely the property that MSVC v18's
 *      static-conditional miscompile destroyed at RinexNavParser's navLog().
 */

#include <gtest/gtest.h>

#include "logging/SafeLogger.hpp"
#include "SpdlogRegistryGuard.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <memory>
#include <sstream>
#include <string>

namespace GnssProcessor {
namespace {

using TestSupport::DroppedRegistry;
using TestSupport::SinkCapture;

/// A throwaway logger with no sinks, used purely as an identity to swap in.
std::shared_ptr<spdlog::logger> makeDetachedLogger(const std::string& name) {
    return std::make_shared<spdlog::logger>(name);
}

// ============================================================================
// 1. Never null
// ============================================================================

TEST(SafeLoggerTest, DefaultLoggerIsNonNullWithRegistryDropped) {
    DroppedRegistry guard;
    ASSERT_EQ(spdlog::default_logger(), nullptr);      // precondition: the hazard is present
    EXPECT_NE(Logging::safeDefaultLogger(), nullptr);
}

TEST(SafeLoggerTest, NamedLoggerIsNonNullWithRegistryDropped) {
    DroppedRegistry guard;
    EXPECT_NE(Logging::safeLogger("no_such_logger"), nullptr);
}

TEST(SafeLoggerTest, FallbackIsNotRegistered) {
    // A registered fallback could be torn out by the very drop_all() that made
    // it necessary — the defect it exists to prevent.
    DroppedRegistry guard;
    const auto fb = Logging::safeDefaultLogger();
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(spdlog::get(fb->name()), nullptr);
}

TEST(SafeLoggerTest, FallbackIsStableAcrossCalls) {
    DroppedRegistry guard;
    EXPECT_EQ(Logging::safeDefaultLogger(), Logging::safeDefaultLogger());
}

// ============================================================================
// 2. The message survives — degraded, not dropped
// ============================================================================

// The fallback must own a REAL sink. A fallback constructed without one is
// crash-free, satisfies "never null", and silently discards every diagnostic —
// so this cannot be checked by capturing output, because the capture sink is
// itself a sink the test adds. It has to be asserted on the logger directly.
TEST(SafeLoggerTest, FallbackOwnsARealSink) {
    DroppedRegistry guard;
    const auto fb = Logging::safeDefaultLogger();
    ASSERT_NE(fb, nullptr);
    EXPECT_FALSE(fb->sinks().empty())
        << "the fallback logger has no sinks of its own: every degraded-path "
           "diagnostic would be silently discarded";
    EXPECT_TRUE(fb->should_log(spdlog::level::warn))
        << "the fallback is level-gated below warn, so it swallows diagnostics";
}

TEST(SafeLoggerTest, MessageReachesFallbackWhenRegistryDropped) {
    std::ostringstream oss;
    {
        DroppedRegistry guard;
        SinkCapture capture(Logging::fallbackConsoleLogger(), oss);
        Logging::safeDefaultLogger()->warn("safelogger probe alpha");
        Logging::fallbackConsoleLogger()->flush();
    }
    EXPECT_NE(oss.str().find("safelogger probe alpha"), std::string::npos)
        << "the diagnostic was swallowed rather than routed to the fallback; "
           "captured: [" << oss.str() << "]";
}

TEST(SafeLoggerTest, MessageReachesDefaultLoggerOnNormalPath) {
    // Routing fidelity on the non-dropped path: the helper must target the same
    // logger the bare spdlog::warn() free function would have.
    std::ostringstream oss;
    const auto saved = spdlog::default_logger();
    ASSERT_NE(saved, nullptr);
    {
        SinkCapture capture(saved, oss);
        Logging::safeDefaultLogger()->warn("safelogger probe beta");
        saved->flush();
    }
    EXPECT_NE(oss.str().find("safelogger probe beta"), std::string::npos);
}

// ============================================================================
// 3. Per-call resolution
// ============================================================================

TEST(SafeLoggerTest, ReResolvesAfterDefaultLoggerSwap) {
    const auto saved = spdlog::default_logger();
    const auto first  = makeDetachedLogger("safelogger_swap_first");
    const auto second = makeDetachedLogger("safelogger_swap_second");

    spdlog::set_default_logger(first);
    EXPECT_EQ(Logging::safeDefaultLogger(), first);

    spdlog::set_default_logger(second);
    EXPECT_EQ(Logging::safeDefaultLogger(), second)
        << "the helper cached its first resolution — the cached-accessor defect";

    // set_default_logger() also inserts into loggers_ under each name; drop both
    // so the registry is left exactly as found.
    if (saved) spdlog::set_default_logger(saved);
    spdlog::drop("safelogger_swap_first");
    spdlog::drop("safelogger_swap_second");
}

TEST(SafeLoggerTest, NamedLoggerIsPreferredOverDefault) {
    const auto named = makeDetachedLogger("safelogger_named_probe");
    spdlog::register_logger(named);

    EXPECT_EQ(Logging::safeLogger("safelogger_named_probe"), named);
    // An unrelated name must fall through to the default, not to the named one.
    EXPECT_EQ(Logging::safeLogger("safelogger_absent_probe"), spdlog::default_logger());

    spdlog::drop("safelogger_named_probe");
}

TEST(SafeLoggerTest, NamedLookupFallsBackOnceItsLoggerIsDropped) {
    const auto named = makeDetachedLogger("safelogger_transient");
    spdlog::register_logger(named);
    ASSERT_EQ(Logging::safeLogger("safelogger_transient"), named);

    spdlog::drop("safelogger_transient");
    EXPECT_EQ(Logging::safeLogger("safelogger_transient"), spdlog::default_logger())
        << "resolution was frozen at the first call rather than redone";
}

// ============================================================================
// Guard hygiene — the guard itself must not leak state into later tests
// ============================================================================

TEST(SafeLoggerTest, GuardRestoresDefaultAndNamedLoggers) {
    const auto named = makeDetachedLogger("safelogger_guard_probe");
    spdlog::register_logger(named);
    const auto before = spdlog::default_logger();
    {
        DroppedRegistry guard;
        EXPECT_EQ(spdlog::default_logger(), nullptr);
        EXPECT_EQ(spdlog::get("safelogger_guard_probe"), nullptr);
    }
    EXPECT_EQ(spdlog::default_logger(), before);
    EXPECT_EQ(spdlog::get("safelogger_guard_probe"), named)
        << "the guard dropped a named logger and failed to put it back";

    spdlog::drop("safelogger_guard_probe");
}

} // namespace
} // namespace GnssProcessor
