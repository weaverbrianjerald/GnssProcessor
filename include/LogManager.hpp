#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <map>

namespace GnssProcessor {

    /**
     * @file include/LogManager.hpp
     * @brief Lightweight logging manager wrapper around spdlog for the GnssProcessor project.
     *
     * Provides initialization of log directory, creation and retrieval of module-specific loggers,
     * runtime log level changes and graceful shutdown/flush of logging sinks.
     */

    /**
     * @enum LogLevel
     * @brief Project-level logging severity that maps to spdlog levels.
     */
    enum class LogLevel {
        /// Trace-level messages (most verbose).
        Trace,
        /// Debug-level messages.
        Debug,
        /// Informational messages.
        Info,
        /// Warning messages.
        Warn,
        /// Error messages.
        Error,
        /// Critical (fatal) messages.
        Critical
    };

    /**
     * @class LogManager
     * @brief Manages creation and lifetime of spdlog loggers used across the application.
     *
     * Typical usage:
     * - Call initialize() once at application startup with a log directory and desired level.
     * - Use getLogger("module_name") to retrieve or create module-specific loggers.
     * - Call setLogLevel() to change logging verbosity at runtime.
     * - Call shutdown() during application shutdown to flush and stop all sinks.
     */
    class LogManager {
    public:
        /**
         * @brief Default constructor.
         *
         * Constructs a LogManager instance; no initialization of spdlog sinks/loggers is performed
         * until initialize() is called.
         */
        LogManager();

        /**
         * @brief Initialize logging subsystem.
         *
         * Creates the default logger (\"gnss_processor\") and any requested module loggers as rotating
         * file sinks inside @p log_dir. Also sets the logging level.
         *
         * @param log_dir Filesystem directory where log files will be written. The directory will be
         *                used as the base path for rotating log files.
         * @param level  Initial logging level to apply to all created loggers.
         * @return true if initialization succeeded (sinks and default logger created), false on failure.
         */
        bool initialize(const std::filesystem::path& log_dir, LogLevel level);

        /**
         * @brief Set the global log level for all managed loggers.
         *
         * This will update the level on every logger stored in the manager so that logging
         * verbosity changes take effect immediately.
         *
         * @param level New logging level to apply.
         */
        void setLogLevel(LogLevel level);

        /**
         * @brief Shutdown logging and flush all sinks.
         *
         * Flushes and stops all spdlog sinks and clears internal logger references. After calling
         * shutdown(), initialize() can be called again to recreate loggers.
         */
        void shutdown();

        /**
         * @brief Get the default application logger.
         *
         * The default logger is named \"gnss_processor\" and is created during initialize().
         *
         * @return shared_ptr to the default spdlog::logger instance.
         * @throws std::out_of_range if the default logger does not exist (i.e., initialize() was not called).
         */
        std::shared_ptr<spdlog::logger> getLogger() const { return loggers_.at("gnss_processor"); }

        /**
         * @brief Retrieve or create a module-specific logger.
         *
         * If a logger with @p logger_name already exists it is returned. Otherwise a new logger
         * is created (using the same sink configuration as the default logger), stored and returned.
         *
         * @param logger_name Name of the logger/module (e.g., \"gnss_processor.core\").
         * @return shared_ptr to the requested spdlog::logger.
         */
        std::shared_ptr<spdlog::logger> getLogger(const std::string& logger_name);

    private:
        /// Map of logger name to spdlog logger instance.
        std::map<std::string, std::shared_ptr<spdlog::logger>> loggers_;

        /// Directory where log files are stored.
        std::filesystem::path log_dir_;

        /// Current configured log level.
        LogLevel level_;

        /// Whether initialize() has been successfully called.
        bool initialized_ = false;

        /**
         * @brief Convert project LogLevel to spdlog::level::level_enum.
         *
         * @param level Project log level to convert.
         * @return Corresponding spdlog level enum value.
         */
        spdlog::level::level_enum toSpdlogLevel(LogLevel level) const;
    };

} // namespace GnssProcessor