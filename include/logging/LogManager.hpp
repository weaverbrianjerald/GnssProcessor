#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <set>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>
#include <map>
#include <mutex>
#include <chrono>

namespace GnssProcessor {

    struct GnssTime;

    /**
     * @file include/logging/LogManager.hpp
     * @brief Lightweight logging manager wrapper around spdlog for the GnssProcessor project.
     *
     * Prepares the log directory, creates and retrieves module-specific loggers, changes
     * log levels at runtime, and flushes and drops the loggers on shutdown.
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
        /// Disabled: emit nothing. Silences individual loggers via per-logger overrides.
        Off
    };

    /**
     * @brief Parse a LogLevel from a JSON-friendly string.
     *
     * Accepts, case-insensitively: "trace", "debug", "info", "warn"/"warning",
     * "error"/"err", "critical"/"crit", "off"/"none"/"disabled". Unknown strings throw
     * @c std::invalid_argument so the config loader can warn about them.
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
     * - Call shutdown() during application shutdown to flush and drop all loggers.
     */
    class LogManager {
    public:
        /**
         * @brief Default constructor.
         *
         * Constructs a LogManager instance; no spdlog sinks or loggers are created until
         * initialize() is called.
         */
        LogManager();

        /**
         * @brief Initialize logging subsystem.
         *
         * Creates @p log_dir if needed and removes the stale @c .log files in it, installs
         * the GPS-time formatter on the spdlog registry, and registers the default logger
         * (\"gnss_processor\"), which is console-only.  Module loggers with rotating file
         * sinks in @p log_dir are created later, on first getLogger(name).  A second call
         * on an initialized instance calls spdlog::drop_all() and starts over; that also
         * clears spdlog's default logger, so spdlog::default_logger() is null afterwards.
         *
         * @param log_dir Filesystem directory where log files will be written. The directory will be
         *                used as the base path for rotating log files.
         * @param level  Initial logging level to apply to all created loggers.
         * @return true if initialization succeeded (default logger created and registered),
         *         false if it fell back to a console-only logger after an exception.
         *
         * @note Never throws. Log-directory preparation (create and stale-.log
         *       scrub) is best-effort via the @c error_code overloads, and any
         *       remaining exception degrades to a console-only logger with a
         *       @c false return. A logging-setup failure does not abort the
         *       caller; on Windows a concurrently running process holding a
         *       @c .log open makes the scrub's @c remove() fail routinely.
         */
        bool initialize(const std::filesystem::path& log_dir, LogLevel level);

        /**
         * @brief Set the global log level for all managed loggers.
         *
         * Updates the level on every logger stored in the manager so the change takes
         * effect immediately.  A no-op before initialize().
         *
         * @param level New logging level to apply.
         */
        void setLogLevel(LogLevel level);

        /**
         * @brief Shutdown logging and flush all sinks.
         *
         * Flushes every managed logger, calls spdlog::drop_all() (which also clears
         * spdlog's default logger, so spdlog::default_logger() is null afterwards) and
         * clears the internal logger references. After
         * shutdown(), initialize() can be called again to recreate loggers.
         */
        void shutdown();

        /**
         * @brief Get the default application logger.
         *
         * The default logger is named \"gnss_processor\" and is created during initialize().
         *
         * Before initialize() (and after shutdown()) the default logger does not exist,
         * and a transient console-only logger is returned instead, as the named
         * getLogger(name) overload does when uninitialized.  Call sites that can run
         * before logging is configured (for example GnssConfig::loadFromJson, whose
         * catch handler logs through this accessor) then log to the console instead of
         * throwing std::out_of_range from the handler that is trying to log.
         *
         * @return shared_ptr to the default spdlog::logger instance, or a
         *         transient console-only logger when initialize() has not run.
         */
        std::shared_ptr<spdlog::logger> getLogger() const;

        /**
         * @brief Retrieve or create a module-specific logger.
         *
         * If a logger with @p logger_name already exists it is returned. Otherwise a new logger
         * is created with a rotating file sink (@c <log_dir>/<logger_name>.log, 5 MB, 3 files)
         * and a console sink, stored and returned.  Its level is off if the name is in the
         * disabled set, else its per-logger override, else the global level.  If the file
         * sink cannot be created, the default logger is returned.  Before initialize(), a
         * transient console-only logger with the requested name is returned.
         *
         * @param logger_name Name of the logger/module (e.g., \"gnss_processor.core\").
         * @return shared_ptr to the requested spdlog::logger.
         */
        std::shared_ptr<spdlog::logger> getLogger(const std::string& logger_name);

        /**
         * @brief Mark a set of logger names as disabled.
         *
         * Disabled loggers are created with level=off so their .log files
         * remain empty.  Loggers already created are set to off immediately,
         * so the call works before or after the first getLogger().
         *
         * @param names Set of logger names to disable.
         */
        void setDisabledLoggers(const std::set<std::string>& names);

        /**
         * @brief Replace a named logger's sinks with a GatedSink wrapper.
         *
         * The GatedSink forwards messages in Passthrough mode and buffers them
         * in Buffer mode. KalmanFilter uses it to suppress intermediate
         * rebuild-pass output without touching call sites. Idempotent:
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
         * here. Loggers already created are updated immediately; loggers
         * not yet created pick up their override when first requested.
         *
         * @param levels Map of logger name → LogLevel.
         */
        void setLoggerLevels(const std::map<std::string, LogLevel>& levels);

        /**
         * @brief Set the GNSS epoch time printed in the epoch field (%E) of
         *        every subsequent log message on the calling thread.
         *
         * Set this at the top of each per-epoch processing iteration so
         * log lines emitted during that epoch carry the epoch's week, TOW
         * and GPS-scale calendar time, aligned with the RINEX cadence
         * (e.g. tow 30.000, 60.000).  The value is thread-local; each
         * worker thread sees its own setting.  Until the first call, the
         * field prints a placeholder of matching width
         * (@c "----, --------- (---------- --:--:------)") for boot-time
         * and pre-epoch messages.
         */
        static void setCurrentEpochTime(const GnssTime& t);

        /**
         * @brief Clear the calling thread's epoch time so subsequent log
         *        messages print the placeholder in the epoch field.
         */
        static void clearCurrentEpochTime();

    private:
        /// Map of logger name to spdlog logger instance.
        std::map<std::string, std::shared_ptr<spdlog::logger>> loggers_;

        /// Serializes lazy logger creation in getLogger().  The fetch of an
        /// already-created logger is lock-free (std::map::find is data-race-safe
        /// for concurrent readers); only the first-touch create path locks.  The
        /// PPP-RTK network producer fetches loggers from its per-station worker
        /// threads; it prewarms every reachable logger first, so no insert races
        /// a concurrent fetch inside a parallel region.
        std::mutex logger_create_mutex_;

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
         * @brief Build the pattern_formatter used by every managed logger.
         *
         * Pattern @c "[%E] [%n] [%^%l%$] %v".  The custom flag @c %E prints the
         * calling thread's current epoch (see setCurrentEpochTime()) as week, TOW
         * and GPS-scale calendar time, with no leap-second subtraction, so log lines
         * align with RINEX GPS epoch times.  It does not use the message's wall-clock
         * timestamp.
         */
        std::unique_ptr<spdlog::pattern_formatter> makeGpsFormatter() const;

        /// GPS−UTC offset in seconds (currently 18).  Not read by the formatter.
        int gps_utc_offset_s_{ 18 };
    };

} // namespace GnssProcessor
