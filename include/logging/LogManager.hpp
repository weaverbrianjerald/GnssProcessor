#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <set>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>
#include <map>
#include <chrono>

namespace GnssProcessor {

    struct GnssTime;

    /**
     * @file include/logging/LogManager.hpp
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
        Critical,
        /// Disabled — emit nothing. Used to silence individual loggers via per-logger overrides.
        Off
    };

    /**
     * @brief Parse a LogLevel from a JSON-friendly string.
     *
     * Accepts case-insensitive: "trace", "debug", "info", "warn"/"warning",
     * "error"/"err", "critical"/"crit", "off". Unknown strings throw
     * @c std::invalid_argument so the config loader can warn the user.
     *
     * @param s Level name from the JSON config.
     * @return Corresponding LogLevel.
     * @throws std::invalid_argument if @p s is not a recognized level.
     */
    LogLevel logLevelFromString(const std::string& s);

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
         * If the logger name is in the disabled set, it is created at spdlog::level::off so that
         * no messages are written to its file.
         *
         * @param logger_name Name of the logger/module (e.g., \"gnss_processor.core\").
         * @return shared_ptr to the requested spdlog::logger.
         */
        std::shared_ptr<spdlog::logger> getLogger(const std::string& logger_name);

        /**
         * @brief Mark a set of logger names as disabled.
         *
         * Disabled loggers are created with level=off so their .log files
         * remain empty.  Call this before the first getLogger() call for
         * best effect; loggers already created are retroactively silenced.
         *
         * @param names Set of logger names to disable.
         */
        void setDisabledLoggers(const std::set<std::string>& names);

        /**
         * @brief Replace a named logger's sinks with a GatedSink wrapper.
         *
         * The GatedSink forwards messages in Passthrough mode, buffers them
         * in Buffer mode. Used by KalmanFilter to suppress intermediate
         * rebuild-pass output without touching call sites. Idempotent —
         * calling it twice on the same logger returns the same wrapper.
         * Must be called after the logger has been created.
         *
         * @param logger_name Name of the logger to wrap.
         * @return Shared pointer to the GatedSink, or nullptr if the logger
         *         doesn't exist.
         */
        std::shared_ptr<class GatedSink> installGatedSink(const std::string& logger_name);

        /**
         * @brief Apply per-logger level overrides.
         *
         * Each entry maps a logger name (e.g. "kalman_filter") to the level
         * at which that logger should emit. Loggers not listed continue to
         * use the global level passed to @c initialize(). Disabled loggers
         * (via @c setDisabledLoggers) take precedence over any override
         * here. Loggers already created are updated immediately; not-yet-
         * created loggers will pick up their override when first requested.
         *
         * @param levels Map of logger name → LogLevel.
         */
        void setLoggerLevels(const std::map<std::string, LogLevel>& levels);

        /**
         * @brief Set the GNSS epoch time used for the [tow=...] field in
         *        every subsequent log message on the calling thread.
         *
         * Set this at the top of each per-epoch processing iteration so
         * log lines emitted during that epoch carry a TOW that aligns
         * with the RINEX cadence (e.g. tow=30.000, 60.000).  The value
         * is thread-local; worker threads see their own setting.  Until
         * the first call, log lines emit @c "tow=---------" as a
         * placeholder for boot-time / pre-epoch messages.
         */
        static void setCurrentEpochTime(const GnssTime& t);

        /**
         * @brief Clear the current epoch time so subsequent log
         *        messages emit the @c "tow=---------" placeholder.
         */
        static void clearCurrentEpochTime();

    private:
        /// Map of logger name to spdlog logger instance.
        std::map<std::string, std::shared_ptr<spdlog::logger>> loggers_;

        /// Directory where log files are stored.
        std::filesystem::path log_dir_;

        /// Current configured log level.
        LogLevel level_;

        /// Whether initialize() has been successfully called.
        bool initialized_ = false;

        /// Set of logger names whose level should be forced to off.
        std::set<std::string> disabled_loggers_;

        /// Per-logger level overrides; absent entries fall back to @c level_.
        std::map<std::string, LogLevel> per_logger_levels_;

        /**
         * @brief Convert project LogLevel to spdlog::level::level_enum.
         *
         * @param level Project log level to convert.
         * @return Corresponding spdlog level enum value.
         */
        spdlog::level::level_enum toSpdlogLevel(LogLevel level) const;

        /**
         * @brief Build a pattern_formatter that uses GPS time for the %* flag.
         *
         * The returned formatter replaces the '%*' custom flag with the
         * message timestamp shifted by the GPS�UTC leap-second offset so
         * that log timestamps align with RINEX GPS epoch times.
         */
        std::unique_ptr<spdlog::pattern_formatter> makeGpsFormatter() const;

        /// GPS�UTC offset in seconds (currently 18).
        int gps_utc_offset_s_{ 18 };
    };

} // namespace GnssProcessor