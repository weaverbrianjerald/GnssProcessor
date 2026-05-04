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
    // iteration; consumed by TowFlagFormatter on every log message.
    // Thread-local so worker threads (e.g. multi-rover baselines, future
    // network mode) each carry their own current epoch.
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
    // matching width otherwise.  The datetime is GPS-scale (NO UTC leap-
    // second subtraction) so it aligns with TOW exactly — a 30 s RINEX
    // file produces lines on the HH:MM:00 / HH:MM:30 grid, not 18 s off.
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
     * Provides initialization of a default rotating file + console logger, creation of
     * module-specific loggers, runtime log level changes, and graceful shutdown/flush.
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
     * Creates the log directory if necessary, removes existing .log files in the directory,
     * and registers a default logger named "gnss_processor" which writes to a rotating file
     * and the console. If initialization fails for file sinks, falls back to a console-only
     * logger and returns false.
     *
     * @param log_dir Filesystem path to the directory where log files will be created.
     * @param level Initial logging level to apply to the created logger(s).
     * @return true on successful initialization of file + console logger; false if only
     *         console logging could be established due to an spdlog error.
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
            // Create log directory if it doesn't exist
            if (!std::filesystem::exists(log_dir)) {
                std::filesystem::create_directories(log_dir);
            }
            // Remove existing .log files to ensure overwrite
            for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".log") {
                    std::filesystem::remove(entry.path());
                }
            }

            // Apply the GPS+TOW formatter globally so that the 48-odd
            // call sites that use the default global logger directly
            // (spdlog::info, spdlog::warn, ...) also get GPS-scale
            // timestamps and the [tow=...] field.  spdlog clones the
            // formatter onto every registered logger and stores it as
            // the default for new loggers, so per-logger
            // set_formatter() calls below remain a defensive belt-and-
            // braces but are not strictly required.
            spdlog::set_formatter(makeGpsFormatter());

            // Default logger ("gnss_processor") is console-only by design.
            // It catches messages from spdlog::xxx() global calls and from a
            // small number of legacy sites that use getLogger() with no name;
            // all module-specific logging should go to a named logger
            // (getLogger("module_name")) that lands in its own .log file.
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
        catch (const spdlog::spdlog_ex& e) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto logger = std::make_shared<spdlog::logger>("gnss_processor", console_sink);
            logger->set_formatter(makeGpsFormatter());
            logger->set_level(toSpdlogLevel(level));
            logger->error("Failed to initialize file logging: {}. Using console logging.", e.what());
            loggers_["gnss_processor"] = logger;
            spdlog::register_logger(logger);
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
     * Flushes all registered loggers, drops all spdlog loggers/sinks, clears internal
     * state and marks the manager as uninitialized so it may be safely reinitialized.
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
     * @brief Retrieve or create a module-specific logger.
     *
     * If the manager is not initialized, returns a console-only logger with the requested
     * name and issues an error-level message indicating the manager wasn't initialized.
     * When initialized, attempts to create a rotating-file + console logger using the same
     * rotation policy as the default logger. On failure to create a file sink, the default
     * `gnss_processor` logger is returned.
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

        if (loggers_.find(logger_name) == loggers_.end()) {
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