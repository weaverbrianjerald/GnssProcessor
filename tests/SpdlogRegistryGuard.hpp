#pragma once

/**
 * @file SpdlogRegistryGuard.hpp
 * @brief Test-only RAII helpers for exercising an EMPTY spdlog registry without
 *        poisoning the rest of the test binary.
 *
 * Shared by tests/test_rtcm_logger_lifetime.cpp and tests/test_safe_logger.cpp.
 * It lives in a header rather than being copied into both because the restore
 * sequence below is order-dependent in a way that is easy to break silently.
 */

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <memory>
#include <ostream>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace TestSupport {

/**
 * @brief Empties the spdlog registry exactly the way LogManager does, and puts
 *        it back on scope exit.
 *
 * `spdlog::drop_all()` is process-global: without the restore, one test using it
 * would leave every later test in the binary logging into a dropped registry —
 * which is the very defect these suites exist to guard against.
 *
 * @warning The destructor's ordering is load-bearing. It re-registers the
 *          snapshot FIRST and calls `set_default_logger()` LAST, and it skips
 *          the saved default inside the loop. That is not redundancy:
 *          `registry::set_default_logger()` re-inserts the logger into
 *          `loggers_` under its own name, so re-registering it in the loop as
 *          well would throw `spdlog_ex` on the duplicate. Equally, dropping the
 *          trailing `set_default_logger()` would leave `default_logger_` null
 *          while `loggers_` looked healthy — a subtler version of the original
 *          bug. Change one half and you must change the other.
 *
 * @note If a test body faults (SEH) rather than merely failing, the destructor
 *       does not run and the registry stays dropped for the remainder of the
 *       process. That is acceptable here precisely because a fault is the
 *       failure these suites detect: the cascade is a symptom, not a new bug.
 */
class DroppedRegistry {
public:
    DroppedRegistry() {
        spdlog::apply_all([this](std::shared_ptr<spdlog::logger> l) {
            saved_.push_back(std::move(l));
        });
        saved_default_ = spdlog::default_logger();
        spdlog::drop_all();
    }

    ~DroppedRegistry() {
        for (auto& l : saved_) {
            if (!l || l == saved_default_) continue;      // see @warning above
            try { spdlog::register_logger(l); } catch (const spdlog::spdlog_ex&) {}
        }
        if (saved_default_) spdlog::set_default_logger(saved_default_);
    }

    DroppedRegistry(const DroppedRegistry&)            = delete;
    DroppedRegistry& operator=(const DroppedRegistry&) = delete;

private:
    std::vector<std::shared_ptr<spdlog::logger>> saved_;
    std::shared_ptr<spdlog::logger>              saved_default_;
};

/**
 * @brief Temporarily attaches an ostream sink to @p logger so a test can assert
 *        on what was actually EMITTED, then removes it again.
 *
 * Needed because "did not crash" is too weak an assertion for this defect class:
 * a wrong fix that swallows the message when the registry is empty (e.g.
 * `if (auto l = spdlog::default_logger()) l->warn(...)`) also does not crash.
 * Binding the output distinguishes "degraded to a fallback" from "silently
 * dropped".
 *
 * The sink is popped in the destructor because the loggers involved outlive the
 * test — in particular `Logging::fallbackConsoleLogger()` is a module-local
 * static shared by every test in the binary, so an unbalanced push would leak a
 * dangling reference to this test's stream.
 */
class SinkCapture {
public:
    SinkCapture(std::shared_ptr<spdlog::logger> logger, std::ostream& os)
        : logger_(std::move(logger)),
          sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(os)),
          saved_level_(logger_->level()) {
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);   // do not let level policy hide the probe
    }

    ~SinkCapture() {
        logger_->set_level(saved_level_);
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) { sinks.erase(it); break; }
        }
    }

    SinkCapture(const SinkCapture&)            = delete;
    SinkCapture& operator=(const SinkCapture&) = delete;

private:
    std::shared_ptr<spdlog::logger>                  logger_;
    std::shared_ptr<spdlog::sinks::ostream_sink_mt>  sink_;
    spdlog::level::level_enum                        saved_level_;
};

} // namespace TestSupport
} // namespace GnssProcessor
