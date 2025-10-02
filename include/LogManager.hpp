#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <map>

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
        bool initialize(const std::filesystem::path& log_dir, LogLevel level);

        // Set log level for all loggers at runtime
        void setLogLevel(LogLevel level);

        // Shutdown logging and flush all sinks
        void shutdown();

        // Get the default logger (gnss_processor)
        std::shared_ptr<spdlog::logger> getLogger() const { return loggers_.at("gnss_processor"); }

        // Get or create a module-specific logger
        std::shared_ptr<spdlog::logger> getLogger(const std::string& logger_name);

    private:
        std::map<std::string, std::shared_ptr<spdlog::logger>> loggers_;
        std::filesystem::path log_dir_;
        LogLevel level_;
        bool initialized_ = false;

        // Convert LogLevel to spdlog level
        spdlog::level::level_enum toSpdlogLevel(LogLevel level) const;
    };

} // namespace GnssProcessor