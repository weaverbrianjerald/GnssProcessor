#include "LogManager.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace GnssProcessor {

    /**
     * @file src/core/common/LogManager.cpp
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
                logger->set_level(toSpdlogLevel(level_));
                logger->flush_on(spdlog::level::info);
                spdlog::register_logger(logger);
                loggers_[logger_name] = logger;
                logger->info("Initialized logger {} with file: {}", logger_name, (log_dir_ / (logger_name + ".log")).string());
            }
            catch (const spdlog::spdlog_ex& e) {
                loggers_["gnss_processor"]->error("Failed to initialize logger {}: {}. Using default logger.", logger_name, e.what());
                return loggers_["gnss_processor"];
            }
        }
        return loggers_[logger_name];
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

} // namespace GnssProcessor