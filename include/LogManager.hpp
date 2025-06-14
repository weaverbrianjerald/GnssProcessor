#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace GnssProcessor {

    enum class LogLevel {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
        Critical
    };

    class LogManager {
    public:
        // Default constructor
        LogManager();

        // Initialize logging with specified directory and log level
        void initialize(const std::filesystem::path& log_dir, LogLevel level);

        // Set log level at runtime
        void setLogLevel(LogLevel level);

        // Shutdown logging and flush all sinks
        void shutdown();

        // Get the default logger
        std::shared_ptr<spdlog::logger> getLogger() const { return logger_; }

    private:
        std::shared_ptr<spdlog::logger> logger_;
        std::filesystem::path log_dir_;
        bool initialized_ = false;

        // Convert LogLevel to spdlog level
        spdlog::level::level_enum toSpdlogLevel(LogLevel level) const;
    };

} // namespace GnssProcessor

//#endif // LOGMANAGER_HPP