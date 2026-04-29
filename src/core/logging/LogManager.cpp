#include "logging/LogManager.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iomanip>
#include <sstream>

namespace GnssProcessor {

    // ===================================================================
    // Custom spdlog flag formatter: GPS time (%* flag)
    // ===================================================================

    class GpsTimeFlagFormatter : public spdlog::custom_flag_formatter {
    public:
        explicit GpsTimeFlagFormatter(int offset_s) : offset_s_(offset_s) {}

        void format(const spdlog::details::log_msg& msg, const std::tm& /*unused*/,
                     spdlog::memory_buf_t& dest) override {
            // Shift the message time_point by the GPS-UTC offset.
            auto gps_tp = msg.time + std::chrono::seconds(offset_s_);
            auto tt = std::chrono::system_clock::to_time_t(gps_tp);
            std::tm gps_tm;
#ifdef _WIN32
            gmtime_s(&gps_tm, &tt);
#else
            gmtime_r(&tt, &gps_tm);
#endif
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                gps_tp.time_since_epoch()) % 1000;

            // Format: YYYY-MM-DD HH:MM:SS.mmm
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                gps_tm.tm_year + 1900, gps_tm.tm_mon + 1, gps_tm.tm_mday,
                gps_tm.tm_hour, gps_tm.tm_min, gps_tm.tm_sec,
                static_cast<int>(millis.count()));
            dest.append(std::string_view(buf));
        }

        std::unique_ptr<custom_flag_formatter> clone() const override {
            return std::make_unique<GpsTimeFlagFormatter>(offset_s_);
        }

    private:
        int offset_s_;
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

            // Initialize default logger (gnss_processor)
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (log_dir / "gnss_processor.log").string(), 1024 * 1024 * 5, 3);
            auto logger = std::make_shared<spdlog::logger>("gnss_processor", spdlog::sinks_init_list{ file_sink, console_sink });
            logger->set_formatter(makeGpsFormatter());
            logger->set_level(toSpdlogLevel(level));
            logger->flush_on(spdlog::level::info);
            spdlog::register_logger(logger);
            loggers_["gnss_processor"] = logger;
            initialized_ = true;
            logger->info("LogManager initialized with log directory: {}", log_dir.string());
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
                if (disabled_loggers_.count(logger_name)) {
                    logger->set_level(spdlog::level::off);
                } else {
                    logger->set_level(toSpdlogLevel(level_));
                }
                logger->flush_on(spdlog::level::info);
                spdlog::register_logger(logger);
                loggers_[logger_name] = logger;
                if (!disabled_loggers_.count(logger_name)) {
                    logger->info("Initialized logger {} with file: {}", logger_name, (log_dir_ / (logger_name + ".log")).string());
                }
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
        default: return spdlog::level::info;
        }
    }

    std::unique_ptr<spdlog::pattern_formatter> LogManager::makeGpsFormatter() const {
        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<GpsTimeFlagFormatter>('*', gps_utc_offset_s_);
        formatter->set_pattern("[%*] [%n] [%^%l%$] %v");
        return formatter;
    }

} // namespace GnssProcessor