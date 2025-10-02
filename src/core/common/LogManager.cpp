#include "LogManager.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace GnssProcessor {

    LogManager::LogManager() : level_(LogLevel::Info), initialized_(false) {}

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