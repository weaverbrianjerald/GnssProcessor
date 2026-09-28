#pragma once

/**
 * @file SpdlogRegistryGuard.hpp
 * @brief Test-only RAII helpers for exercising an empty spdlog registry without
 *        leaving the rest of the test binary with a dropped registry.
 *
 * Shared by tests/test_rtcm_logger_lifetime.cpp and tests/test_safe_logger.cpp.
 * It is a shared header rather than two copies because the restore sequence below
 * is order-dependent, and a copy that drifts would break without failing a test.
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
 * @brief Empties the spdlog registry the same way LogManager does (re-initialize
 *        and shutdown both call `spdlog::drop_all()`), and restores it on scope exit.
 *
 * `spdlog::drop_all()` is process-global: without the restore, one test using it
 * would leave every later test in the binary logging into a dropped registry,
 * which is the defect these suites guard against.
 *
 * @warning The destructor's ordering is required.  It re-registers the snapshot
 *          first, skipping the saved default inside the loop, and calls
 *          `set_default_logger()` last.  `registry::set_default_logger()`
 *          re-inserts the logger into `loggers_` under its own name, so also
 *          re-registering it in the loop would throw `spdlog_ex` on the duplicate.
 *          Omitting the trailing `set_default_logger()` would leave
 *          `default_logger_` null while `loggers_` looks populated, a less visible
 *          form of the same defect.  The skip and the trailing call go together;
 *          changing one requires changing the other.
 *
 * @note If a test body faults (SEH) rather than failing an assertion, the
 *       destructor does not run and the registry stays dropped for the rest of the
 *       process.  That is acceptable because a fault is the failure these suites
 *       detect: the later failures follow from it and are not separate defects.
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
 *        on what was actually emitted, then removes it again.
 *
 * "Did not crash" is too weak an assertion for this defect class: a wrong fix that
 * discards the message when the registry is empty (e.g.
 * `if (auto l = spdlog::default_logger()) l->warn(...)`) does not crash either.
 * Capturing the output distinguishes "fell back to another logger" from "dropped
 * without a trace".
 *
 * The sink is removed in the destructor because the loggers involved outlive the
 * test.  In particular, `Logging::fallbackConsoleLogger()` is a module-local static
 * shared by every test in the binary, so a sink left attached would hold a dangling
 * reference to this test's stream.
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
