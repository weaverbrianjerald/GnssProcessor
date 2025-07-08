#include "LogManager.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace GnssProcessor {

    LogManager::LogManager() : initialized_(false) {}

    void LogManager::initialize(const std::filesystem::path& log_dir, LogLevel level) {
        if (initialized_) {
            logger_->warn("LogManager already initialized, re-initializing");
            spdlog::drop_all();
        }

        try {
            if (!std::filesystem::exists(log_dir)) {
                std::filesystem::create_directories(log_dir);
            }
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (log_dir / "gnss_processor.log").string(), 1024 * 1024 * 5, 3);
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            logger_ = std::make_shared<spdlog::logger>("gnss_processor", spdlog::sinks_init_list{ file_sink, console_sink });
            spdlog::register_logger(logger_);
            logger_->set_level(toSpdlogLevel(level));
            logger_->flush_on(spdlog::level::info);
            initialized_ = true;
            logger_->info("LogManager initialized with log directory: {}", log_dir.string());
        }
        catch (const spdlog::spdlog_ex& e) {
            logger_ = spdlog::stdout_color_mt("gnss_processor");
            logger_->set_level(toSpdlogLevel(level));
            logger_->error("Failed to initialize file logging: {}. Using console logging.", e.what());
            initialized_ = true;
        }
    }

    void LogManager::setLogLevel(LogLevel level) {
        if (!initialized_) {
            return;
        }
        logger_->set_level(toSpdlogLevel(level));
        logger_->info("Log level set to {}", static_cast<int>(level));
    }

    void LogManager::shutdown() {
        if (initialized_) {
            logger_->info("Shutting down LogManager");
            spdlog::drop_all();
            initialized_ = false;
        }
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