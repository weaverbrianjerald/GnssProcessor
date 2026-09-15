#pragma once

/**
 * @file SafeLogger.hpp
 * @brief Null-safe, per-call logger resolution for call sites that would
 *        otherwise reach for spdlog's free-function API.
 *
 * ### Why this exists
 *
 * The convenience free functions are not equivalent to "log to the default
 * logger". Each one expands to
 *
 * @code
 *   spdlog::default_logger_raw()->info(...);
 * @endcode
 *
 * and `default_logger_raw()` hands back a **raw, borrowed pointer** into the
 * registry's `default_logger_` slot (`registry::get_default_raw()` is a bare
 * `default_logger_.get()`). The caller borrows a pointer whose lifetime the
 * registry owns and can revoke at any time.
 *
 * The affected surface is the whole default API, not just the six message
 * helpers — `spdlog/spdlog.h` dereferences `default_logger_raw()` at 30 call
 * sites. Besides `trace/debug/info/warn/error/critical`, it covers
 * `spdlog::log(...)` (four overloads), `spdlog::should_log()`,
 * `spdlog::get_level()`, `spdlog::dump_backtrace()`, and the `SPDLOG_*`
 * logging macros. A census that greps only for the six level names will miss
 * the rest.
 *
 * `spdlog::drop_all()` revokes the slot:
 *
 * @code
 *   void registry::drop_all() {
 *       std::lock_guard<std::mutex> lock(logger_map_mutex_);
 *       loggers_.clear();
 *       default_logger_.reset();     // <-- the borrowed pointer is now null
 *   }
 * @endcode
 *
 * Nothing in this codebase reinstalls a default afterwards (`LogManager` uses
 * `spdlog::register_logger()`, never `spdlog::set_default_logger()`), so the
 * slot stays empty for the rest of the process. The next default-API call then
 * dereferences `nullptr`. It does not drop the message and it does not throw:
 * `logger::log_()` reads `level_` off the null `this` **before** the level
 * test, so even a filtered `trace()` faults — an access violation
 * (0xC0000005 on Windows).
 *
 * ### Reachability (measured, do not overstate this)
 *
 * `LogManager` calls `spdlog::drop_all()` in exactly two places:
 * `shutdown()` (LogManager.cpp:201) and the re-initialize branch of
 * `initialize()` (LogManager.cpp:115). **Neither is currently reachable in a
 * production run**: `shutdown()` has no callers anywhere in the tree, and the
 * re-initialize branch needs one `LogManager` *instance* initialized twice,
 * which no call path does (the two `LogManager`s in `src/sim/main.cpp` are
 * distinct objects, each initialized once).
 *
 * So in the shipped binaries this is **latent**, not live. It is live today in
 * `gnss_tests`, where any test that drops the registry poisons every later
 * test in the process — which is exactly how it was found: four
 * `RtcmParserDecodeTest` cases faulted behind an earlier `drop_all()`. Both
 * `drop_all()` sites are public API, so the latency is a property of the
 * current wiring, not a guarantee.
 *
 * This is the same defect class as a cached logger accessor that outlives the
 * registry entry it came from; the difference is only that here the stale
 * handle is created and consumed inside a single expression, so it never shows
 * up in a grep for cached statics.
 *
 * ### What these helpers guarantee
 *
 * - **Per-call resolution.** The registry is consulted at the moment of the
 *   log call, so a logger swapped in or out between calls is honoured.
 * - **Shared ownership for the duration of the call.** A `shared_ptr` is
 *   returned, so a concurrent `drop_all()` cannot free the logger out from
 *   under an in-flight `->warn(...)`.
 * - **Never null.** When the registry has no default at all, an unregistered
 *   module-local console logger is substituted, so logging degrades to the
 *   console rather than crashing the caller.
 *
 * @note Calling convention. The two guarantees above hold jointly only if the
 *       returned `shared_ptr` is kept alive across the `->` call — which the
 *       natural `safeDefaultLogger()->warn(...)` does, since the temporary
 *       lives to the end of the full expression. Never bind a reference to the
 *       result (`auto& l = safeDefaultLogger();` dangles). To avoid a registry
 *       lookup per iteration in a loop, hoist it into a **value**
 *       (`const auto log = Logging::safeDefaultLogger();`) — that trades
 *       per-call re-resolution for a pinned logger, which is the right call
 *       inside a tight loop and the wrong one for a long-lived cache.
 *
 * ### Routing
 *
 * `safeDefaultLogger()` targets the same logger `spdlog::warn()` targets — but
 * that is spdlog's **built-in** default logger, registered under the empty
 * name `""`, *not* `LogManager`'s "gnss_processor". `LogManager` never calls
 * `set_default_logger()`, so its level policy (`setLogLevel`,
 * `setDisabledLoggers`, `setLoggerLevels`) never reaches the default logger;
 * only the formatter does, via `spdlog::set_formatter()`.
 *
 * Routing is therefore byte-identical to the bare free functions **while a
 * default exists**. Once the registry has been dropped, these helpers route to
 * the fallback below instead of faulting — a deliberate divergence, and the
 * same trade `navLog()` and `tailLog()` make elsewhere in the tree.
 */

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>
#include <utility>

namespace GnssProcessor {
namespace Logging {

/**
 * @brief Console logger used when the spdlog registry holds no default.
 *
 * Deliberately **not** registered with spdlog: a registered fallback could be
 * torn out by the very `drop_all()` that made the fallback necessary, which
 * would reintroduce the defect it exists to prevent. It owns its own sink, so
 * its lifetime is independent of the registry's.
 *
 * @note The immediately-invoked lambda is **load-bearing**, not a style
 *       choice. MSVC v18 (VS 2026) miscompiles a function-local
 *       `static std::shared_ptr<T> x = cond ? f() : g();`, leaving the static
 *       empty (`use_count() == 0`); the IIFE form initializes correctly.
 *       Do not "simplify" this into a conditional-operator initializer.
 *
 * Lifetime: spdlog is linked as a **shared** library here
 * (`SPDLOG_SHARED_LIB;SPDLOG_COMPILED_LIB`), so `registry::instance()`'s static
 * lives in `spdlog.dll` while this static lives in the consuming module. The
 * DLL's statics are constructed before any consumer's and destroyed after, and
 * this one is constructed lazily — only from `safeDefaultLogger()`, which
 * touches the registry first — so the registry is always live for the whole of
 * this object's lifetime.
 *
 * "Module-local", not process-local: `gnss_core` is a static library linked
 * into nine targets (including `gnss_processor_module`, a Python `.pyd`), so a
 * process loading two `gnss_core`-linked modules gets one fallback logger — and
 * one console mutex — per module.
 *
 * Writes to **stderr**, not stdout, because `gnss_core` is linked into targets
 * that stream tool data on stdout (`src/sim/main.cpp` alone has 65 `std::cout`
 * writes); a degraded log path must not corrupt that. Note this differs from
 * `LogManager`'s console sinks, which are stdout.
 *
 * Level is spdlog's own default (`info`), matching the built-in default logger
 * this stands in for, so the degraded path emits the same set of records rather
 * than a different one. It carries spdlog's stock pattern, so fallback lines
 * lack the `[%E]` GPS week/TOW field that `LogManager::makeGpsFormatter()`
 * installs — an accepted, visible degradation: the line is still emitted and
 * still timestamped, just not tied to an epoch.
 *
 * @return Shared pointer to the fallback logger; never null.
 */
inline std::shared_ptr<spdlog::logger> fallbackConsoleLogger() {
    static const std::shared_ptr<spdlog::logger> fallback = [] {
        auto sink   = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("gnss_fallback", std::move(sink));
        logger->set_level(spdlog::level::info);
        return logger;
    }();
    return fallback;
}

/**
 * @brief Resolve the current default logger, per call, as a shared_ptr.
 *
 * Drop-in replacement for the `spdlog::xxx()` free functions:
 * `spdlog::warn(fmt, args...)` becomes
 * `safeDefaultLogger()->warn(fmt, args...)`, with the same routing and the same
 * level filtering whenever a default logger exists, minus the null dereference
 * once one does not. See the file-level @note on calling convention.
 *
 * @return The registry's default logger, or fallbackConsoleLogger() when the
 *         registry holds none. Never null.
 */
inline std::shared_ptr<spdlog::logger> safeDefaultLogger() {
    if (auto logger = spdlog::default_logger()) return logger;
    return fallbackConsoleLogger();
}

/**
 * @brief Resolve a named logger, per call, as a shared_ptr.
 *
 * Mirrors the named-then-default fallback chain used by `navLog()`
 * (RinexNavParser.cpp) and `tailLog()` (NetworkProductIO.cpp), with the same
 * never-null guarantee as safeDefaultLogger(). Prefer this over
 * safeDefaultLogger() at a call site that should follow a module's own logger
 * when `LogManager` has registered one.
 *
 * @param name Registered logger name to prefer.
 * @return The named logger if registered, else the default logger, else
 *         fallbackConsoleLogger(). Never null.
 */
inline std::shared_ptr<spdlog::logger> safeLogger(const std::string& name) {
    if (auto logger = spdlog::get(name)) return logger;
    return safeDefaultLogger();
}

} // namespace Logging
} // namespace GnssProcessor
