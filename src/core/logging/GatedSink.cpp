/**
 * @file GatedSink.cpp
 * @brief Implementation of GatedSink (spdlog wrapper with passthrough/buffer modes).
 */

#include "logging/GatedSink.hpp"

namespace GnssProcessor {

GatedSink::GatedSink(std::vector<spdlog::sink_ptr> inner_sinks)
    : inner_(std::move(inner_sinks)) {}

void GatedSink::log(const spdlog::details::log_msg& msg) {
    if (policy_.load(std::memory_order_relaxed) == Policy::Passthrough) {
        for (auto& s : inner_) s->log(msg);
        return;
    }
    // Buffer mode: take an owned copy. log_msg_buffer copies the formatted
    // payload string + name string so the message survives past the
    // originating logger's call frame.
    std::lock_guard<std::mutex> lk(buffer_mutex_);
    buffer_.emplace_back(msg);
}

void GatedSink::flush() {
    for (auto& s : inner_) s->flush();
}

void GatedSink::set_pattern(const std::string& pattern) {
    for (auto& s : inner_) s->set_pattern(pattern);
}

void GatedSink::set_formatter(std::unique_ptr<spdlog::formatter> formatter) {
    // Each inner sink gets its own clone: formatters are not shareable.
    for (auto& s : inner_) s->set_formatter(formatter->clone());
}

void GatedSink::setPolicy(Policy p) {
    policy_.store(p, std::memory_order_relaxed);
}

void GatedSink::clearBuffer() {
    std::lock_guard<std::mutex> lk(buffer_mutex_);
    buffer_.clear();
}

void GatedSink::replayBuffer() {
    // Take a snapshot under the mutex, then forward without holding it: the
    // inner sinks take their own locks, and nesting those under the buffer
    // mutex is unnecessary and risks lock-ordering problems.
    std::vector<spdlog::details::log_msg_buffer> snapshot;
    {
        std::lock_guard<std::mutex> lk(buffer_mutex_);
        snapshot = buffer_;
    }
    for (auto& m : snapshot) {
        for (auto& s : inner_) s->log(m);
    }
}

} // namespace GnssProcessor
