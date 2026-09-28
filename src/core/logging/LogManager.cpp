#include "logging/LogManager.hpp"
#include "logging/GatedSink.hpp"
#include "math/TimeUtils.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace GnssProcessor {

    LogLevel logLevelFromString(const std::string& s) {
        std::string lower(s.size(), '\0');
        std::transform(s.begin(), s.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "trace")                            return LogLevel::Trace;
        if (lower == "debug")                            return LogLevel::Debug;
        if (lower == "info")                             return LogLevel::Info;
        if (lower == "warn" || lower == "warning")       return LogLevel::Warn;
        if (lower == "error" || lower == "err")          return LogLevel::Error;
        if (lower == "critical" || lower == "crit")      return LogLevel::Critical;
        if (lower == "off" || lower == "none" || lower == "disabled") return LogLevel::Off;
        throw std::invalid_argument("Unknown LogLevel string: '" + s + "'");
    }

    // ===================================================================
    // Per-thread current-epoch context for log lines
    // ===================================================================
    //
    // Set by the application at the top of each per-epoch processing
    // iteration; read by EpochFlagFormatter on every log message.
    // Thread-local, so each worker thread (e.g. multi-rover baselines)
    // carries its own current epoch, and a thread that never sets one
    // prints the placeholder.
    namespace {
        thread_local std::optional<GnssTime> tls_current_epoch_;
    }

    void LogManager::setCurrentEpochTime(const GnssTime& t) {
        tls_current_epoch_ = t;
    }

    void LogManager::clearCurrentEpochTime() {
        tls_current_epoch_.reset();
    }

    // ===================================================================
    // Custom spdlog flag formatter: unified epoch field (%E)
    // ===================================================================
    //
    // Emits "wk, tow.mmm (YYYY-MM-DD HH:MM:SS.mmm)" in GPS time scale
    // when the calling thread has a current epoch set, a placeholder of
    // matching width otherwise.  The datetime is GPS-scale (no UTC leap-
    // second subtraction), so it matches the TOW: a 30 s RINEX file
    // produces lines on the HH:MM:00 / HH:MM:30 grid, not 18 s off it.
    class EpochFlagFormatter : public spdlog::custom_flag_formatter {
    public:
        void format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*unused*/,
                     spdlog::memory_buf_t& dest) override {
            char buf[64];
            if (tls_current_epoch_.has_value()) {
                int year = 0, month = 0, day = 0, hour = 0, minute = 0;
                double second = 0.0;
                TimeUtils::gpsTimeToGpsScaleCalendar(
                    *tls_current_epoch_, year, month, day, hour, minute, second);
                std::snprintf(buf, sizeof(buf),
                              "%4d, %9.3f (%04d-%02d-%02d %02d:%02d:%06.3f)",
                              tls_current_epoch_->week, tls_current_epoch_->tow,
                              year, month, day, hour, minute, second);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "----, --------- (---------- --:--:------)");
            }
            dest.append(std::string_view(buf));
        }

        std::unique_ptr<custom_flag_formatter> clone() const override {
            return std::make_unique<EpochFlagFormatter>();
        }
    };

    /**
     * @file src/core/logging/LogManager.cpp
     * @brief Implementation of LogManager: wrapper around spdlog for application-wide logging.
     *
     * Provides initialization of a console-only default logger, creation of
     * module-specific rotating-file + console loggers, runtime log level changes, and
     * shutdown with flush.
     */

    /**
     * @brief Default constructor.
     *
     * Constructs a LogManager with a default LogLevel of Info. No spdlog resources are created
     * until initialize() is called.
     */
    LogManager::LogManager() : level_(LogLevel::Info), initialized_(false) {}

    /**
     * @brief Initialize the logging subsystem.
     *
     * Creates the log directory if necessary, removes existing .log files in the directory
     * (best-effort), installs the GPS-time formatter on the spdlog registry, and registers a
     * console-only default logger named "gnss_processor".  If any step throws, falls back
     * to a console-only logger that is not guaranteed to be registered, and returns false.  Re-initializing an
     * initialized instance calls spdlog::drop_all() first.
     *
     * @param log_dir Filesystem path to the directory where log files will be created.
     * @param level Initial logging level to apply to the created logger(s).
     * @return true on success; false if an exception forced the console-only fallback.
     */
    bool LogManager::initialize(const std::filesystem::path& log_dir, LogLevel level) {
        if (initialized_) {
            loggers_["gnss_processor"]->warn("LogManager already initialized, re-initializing");
            spdlog::drop_all();
            loggers_.clear();
        }

        log_dir_ = log_dir;
        level_ = level;
        try {
            // Log-directory preparation is best-effort and does not throw.
            //
            // The throwing std::filesystem overloads raise std::filesystem_error,
            // which would abort the caller against this function's "fall back
            // to console-only, return false" contract.  Concurrent runs are
            // normal in this project, and on Windows `remove()` on a .log that
            // another live process holds open fails with
            // ERROR_SHARING_VIOLATION (for example a second gnss_tests.exe
            // holding the shared temp gpt3.log that Gpt3Test's
            // SetUpTestSuite logs to).
            //
            // The error_code overloads are used instead, and an entry that
            // cannot be removed is skipped: the stale .log is then appended to
            // or rotated by spdlog instead of overwritten.
            std::error_code ec;
            if (!std::filesystem::exists(log_dir, ec)) {
                std::filesystem::create_directories(log_dir, ec);
            }
            for (std::filesystem::directory_iterator it(log_dir, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                std::error_code entry_ec;
                if (it->is_regular_file(entry_ec) && it->path().extension() == ".log") {
                    std::filesystem::remove(it->path(), entry_ec);
                }
            }

            // Apply the epoch-field formatter to the spdlog registry so the
            // call sites that use spdlog's own default logger directly
            // (spdlog::info, spdlog::warn, ...) also get the GPS-scale epoch
            // field.  spdlog clones the formatter onto every registered logger
            // and keeps it for loggers its factories create later; loggers
            // added with register_logger() do not receive it, so the
            // per-logger set_formatter() calls below apply it explicitly.
            spdlog::set_formatter(makeGpsFormatter());

            // The manager's default logger ("gnss_processor") is console-only
            // by design.  It serves the few legacy sites that call getLogger()
            // with no name.  It is registered with register_logger(), not
            // set_default_logger(), so spdlog::xxx() free-function calls go to
            // spdlog's own default logger instead (see logging/SafeLogger.hpp).
            // Module-specific logging goes to a named logger
            // (getLogger("module_name")) that writes its own .log file.
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto logger = std::make_shared<spdlog::logger>("gnss_processor", console_sink);
            logger->set_formatter(makeGpsFormatter());
            logger->set_level(toSpdlogLevel(level));
            logger->flush_on(spdlog::level::info);
            spdlog::register_logger(logger);
            loggers_["gnss_processor"] = logger;
            initialized_ = true;
            return true;
        }
        // Catch std::exception, not only spdlog_ex: a logging-setup failure
        // must not abort any caller, and not everything that can throw here is
        // spdlog's (formatter construction, allocation).  The recovery is
        // itself exception-safe: the most common cause is register_logger()
        // rejecting a duplicate "gnss_processor", and the registration below
        // would hit the same rejection, so it is guarded.
        catch (const std::exception& e) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto logger = std::make_shared<spdlog::logger>("gnss_processor", console_sink);
            logger->set_formatter(makeGpsFormatter());
            logger->set_level(toSpdlogLevel(level));
            logger->error("Failed to initialize file logging: {}. Using console logging.", e.what());
            loggers_["gnss_processor"] = logger;
            try {
                spdlog::register_logger(logger);
            } catch (const spdlog::spdlog_ex&) {
                // Name already registered.  The map entry above is what this
                // manager hands out, so a failed registry insert is harmless.
            }
            initialized_ = true;
            return false;
        }
    }

    /**
     * @brief Set the current log level for all managed loggers.
     *
     * If LogManager has not been initialized this call is a no-op.
     *
     * @param level New LogLevel to apply.
     */
    void LogManager::setLogLevel(LogLevel level) {
        if (!initialized_) {
            return;
        }
        level_ = level;
        for (auto& [name, logger] : loggers_) {
            logger->set_level(toSpdlogLevel(level));
            logger->info("Log level set to {} for logger {}", static_cast<int>(level), name);
        }
    }

    /**
     * @brief Shutdown logging subsystem.
     *
     * Flushes all managed loggers, drops all spdlog loggers and sinks (spdlog::drop_all()
     * also clears spdlog's default logger), clears internal state and marks the manager
     * as uninitialized so it can be reinitialized.
     */
    void LogManager::shutdown() {
        if (initialized_) {
            loggers_["gnss_processor"]->info("Shutting down LogManager");
            for (auto& [name, logger] : loggers_) {
                logger->flush();
            }
            spdlog::drop_all();
            loggers_.clear();
            initialized_ = false;
        }
    }

    /**
     * @brief Default ("gnss_processor") logger accessor.
     *
     * Returns the registered default logger when initialized; before
     * initialize() (or after shutdown()), returns a transient console-only
     * fallback.  The header doc gives the rationale: pre-initialization call
     * sites such as GnssConfig::loadFromJson log to the console instead of
     * throwing.
     */
    std::shared_ptr<spdlog::logger> LogManager::getLogger() const {
        const auto it = loggers_.find("gnss_processor");
        if (it != loggers_.end()) return it->second;
        // Uninitialized: transient console-only fallback, as in the named
        // overload below.  Not registered with spdlog, so repeated calls
        // cannot collide in the global registry.
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("gnss_processor", console_sink);
        logger->set_level(spdlog::level::info);
        logger->error("LogManager not initialized, returning console-only default logger");
        return logger;
    }

    /**
     * @brief Retrieve or create a module-specific logger.
     *
     * If the manager is not initialized, returns a console-only logger with the requested
     * name and issues an error-level message indicating the manager wasn't initialized.
     * When initialized, returns the existing logger of that name, or creates a rotating-file
     * (5 MB, 3 files) + console logger. If creation throws spdlog_ex (for example when the
     * file sink cannot be opened), the default `gnss_processor` logger is returned.
     *
     * @param logger_name Name of the logger to retrieve/create.
     * @return Shared pointer to the requested spdlog::logger.
     */
    std::shared_ptr<spdlog::logger> LogManager::getLogger(const std::string& logger_name) {
        if (!initialized_) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto logger = std::make_shared<spdlog::logger>(logger_name, console_sink);
            logger->set_level(spdlog::level::info);
            logger->error("LogManager not initialized, returning console-only logger for {}", logger_name);
            return logger;
        }

        // Lock-free fast path.  std::map::find is data-race-safe for concurrent
        // readers (see [container.requirements.dataraces]), and the returned
        // shared_ptr copy is atomically ref-counted, so once a logger exists it
        // can be fetched from many threads at once.  The PPP-RTK network producer
        // parallelizes its per-station geometry/assembly across threads and
        // prewarms every logger its model path can touch before entering that
        // region, so in steady state this is the only branch taken concurrently
        // and the map is not mutated.  The fast path returns find()->second, not
        // the associative operator[], because operator[] is not in the const,
        // data-race-safe set for associative containers.
        if (auto it = loggers_.find(logger_name); it != loggers_.end())
            return it->second;

        // Slow path: create + register + insert under a mutex.  Runs at startup
        // and on first touch of a new logger.  The mutex serializes concurrent
        // creates.  It does not protect the lock-free fast path: the prewarm
        // contract above is what keeps this path from running concurrently with
        // a fast-path fetch inside a parallel region (a find-vs-insert race).
        std::lock_guard<std::mutex> create_lock(logger_create_mutex_);
        // Re-check: another thread may have created it while this one waited.
        if (auto it = loggers_.find(logger_name); it != loggers_.end())
            return it->second;
        try {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (log_dir_ / (logger_name + ".log")).string(), 1024 * 1024 * 5, 3);
            auto logger = std::make_shared<spdlog::logger>(logger_name, spdlog::sinks_init_list{ file_sink, console_sink });
            logger->set_formatter(makeGpsFormatter());
            // Effective level = (disabled ? Off : per-logger override ?: global level_).
            if (disabled_loggers_.count(logger_name)) {
                logger->set_level(spdlog::level::off);
            } else if (auto it = per_logger_levels_.find(logger_name); it != per_logger_levels_.end()) {
                logger->set_level(toSpdlogLevel(it->second));
            } else {
                logger->set_level(toSpdlogLevel(level_));
            }
            logger->flush_on(spdlog::level::info);
            spdlog::register_logger(logger);
            loggers_[logger_name] = logger;
        }
        catch (const spdlog::spdlog_ex& e) {
            loggers_["gnss_processor"]->error("Failed to initialize logger {}: {}. Using default logger.", logger_name, e.what());
            return loggers_["gnss_processor"];
        }
        return loggers_[logger_name];
    }

    void LogManager::setDisabledLoggers(const std::set<std::string>& names) {
        disabled_loggers_ = names;
        // Retroactively silence any already-created loggers.
        for (const auto& name : names) {
            auto it = loggers_.find(name);
            if (it != loggers_.end()) {
                it->second->set_level(spdlog::level::off);
            }
        }
    }

    std::shared_ptr<GatedSink> LogManager::installGatedSink(const std::string& logger_name) {
        auto it = loggers_.find(logger_name);
        if (it == loggers_.end() || !it->second) return nullptr;
        auto& logger = it->second;
        auto& sinks = logger->sinks();

        // Idempotency: if the logger already has exactly one sink and it's
        // a GatedSink, return it. Avoids stacking gates on repeated calls.
        if (sinks.size() == 1) {
            if (auto existing = std::dynamic_pointer_cast<GatedSink>(sinks.front())) {
                return existing;
            }
        }

        // Wrap the current sinks in a GatedSink and replace.
        auto gate = std::make_shared<GatedSink>(sinks);
        sinks.clear();
        sinks.push_back(gate);
        return gate;
    }

    void LogManager::setLoggerLevels(const std::map<std::string, LogLevel>& levels) {
        per_logger_levels_ = levels;
        // Retroactively apply to already-created loggers.  Disabled loggers
        // keep their off state (disable wins over a per-logger level).
        for (const auto& [name, level] : levels) {
            if (disabled_loggers_.count(name)) continue;
            auto it = loggers_.find(name);
            if (it != loggers_.end()) {
                it->second->set_level(toSpdlogLevel(level));
            }
        }
    }

    /**
     * @brief Convert project LogLevel to spdlog's level enum.
     *
     * @param level LogLevel value to convert.
     * @return Equivalent spdlog::level::level_enum value.
     */
    spdlog::level::level_enum LogManager::toSpdlogLevel(LogLevel level) const {
        switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info: return spdlog::level::info;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        case LogLevel::Off: return spdlog::level::off;
        default: return spdlog::level::info;
        }
    }

    std::unique_ptr<spdlog::pattern_formatter> LogManager::makeGpsFormatter() const {
        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<EpochFlagFormatter>('E');
        formatter->set_pattern("[%E] [%n] [%^%l%$] %v");
        return formatter;
    }

} // namespace GnssProcessor