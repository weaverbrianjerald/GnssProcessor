/**
 * @file ApplicationRealtime.cpp
 * @brief Implementation of Application::runRealTime (NTRIP / streaming pipeline).
 *
 * Multi-stream realtime: the primary stream feeds rover observations
 * (file-replay or NTRIP), and an optional corrections stream feeds RTCM
 * SSR (typically an IGS-RTS NTRIP mountpoint).  Both decode through the
 * same engine — observations buffer until an epoch boundary, SSR records
 * push into @c SsrCorrectionStore.  Streams are polled with a short
 * timeout so neither starves the other; the loop exits when every
 * configured stream is inactive (file EOF reached or NTRIP disconnected).
 */

#include "app/Application.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductResolver.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/TcpClient.hpp"
#include "realtime/RtcmStreamReader.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "slip/MwSlipDetector.hpp"
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <fmt/format.h>

namespace GnssProcessor {

namespace {

/**
 * @brief Uniform byte-stream source backing an RTCM3 framer.
 *
 * Wraps either an @c std::ifstream (binary RTCM dump replay) or an
 * @c NtripClient.  The realtime loop holds one of these per logical
 * stream (rover obs + optional SSR corrections) and polls them
 * round-robin without ever blocking on a single source.
 */
class RealtimeStream {
public:
    static std::unique_ptr<RealtimeStream> openFile(
        const std::string& label, const std::string& path) {
        auto s = std::unique_ptr<RealtimeStream>(new RealtimeStream(Kind::File, label));
        s->file_.open(path, std::ios::binary);
        s->path_ = path;
        if (!s->file_.is_open()) return nullptr;
        return s;
    }

    static std::unique_ptr<RealtimeStream> openNtrip(
        const std::string& label, const NtripConfig& cfg, LogManager& lm) {
        auto s = std::unique_ptr<RealtimeStream>(new RealtimeStream(Kind::Ntrip, label));
        s->ntrip_ = std::make_unique<NtripClient>(cfg, lm);
        if (!s->ntrip_->start()) return nullptr;
        return s;
    }

    static std::unique_ptr<RealtimeStream> openTcp(
        const std::string& label, const TcpConfig& cfg, LogManager& lm) {
        auto s = std::unique_ptr<RealtimeStream>(new RealtimeStream(Kind::Tcp, label));
        s->tcp_ = std::make_unique<TcpClient>(cfg, lm);
        if (!s->tcp_->start()) return nullptr;
        return s;
    }

    /// True while the source might still produce bytes.
    bool active() const {
        switch (kind_) {
        case Kind::File:  return !file_eof_;
        case Kind::Ntrip: return ntrip_ && ntrip_->isConnected();
        case Kind::Tcp:   return tcp_   && tcp_->isConnected();
        }
        return false;
    }

    /// Read up to @p max_bytes; @p timeout_ms applies only to socket sources.
    std::size_t read(std::vector<uint8_t>& out, std::size_t max_bytes, int timeout_ms) {
        switch (kind_) {
        case Kind::File: {
            out.resize(max_bytes);
            file_.read(reinterpret_cast<char*>(out.data()), max_bytes);
            const auto n = static_cast<std::size_t>(file_.gcount());
            out.resize(n);
            if (n == 0 || file_.eof()) file_eof_ = true;
            return n;
        }
        case Kind::Ntrip:
            return ntrip_ ? ntrip_->tryRead(out, max_bytes, timeout_ms) : 0;
        case Kind::Tcp:
            return tcp_   ? tcp_->tryRead(out, max_bytes, timeout_ms)   : 0;
        }
        return 0;
    }

    void stop() {
        if (ntrip_) ntrip_->stop();
        if (tcp_)   tcp_->stop();
        if (file_.is_open()) file_.close();
    }

    const std::string& label() const { return label_; }

private:
    enum class Kind { File, Ntrip, Tcp };
    explicit RealtimeStream(Kind k, std::string label)
        : kind_(k), label_(std::move(label)) {}

    Kind                          kind_;
    std::string                   label_;
    std::string                   path_;
    std::ifstream                 file_;
    bool                          file_eof_{false};
    std::unique_ptr<NtripClient>  ntrip_;
    std::unique_ptr<TcpClient>    tcp_;
};

/// Build the primary observation stream from RealTimeConfig.
std::unique_ptr<RealtimeStream> openPrimary(const RealTimeConfig& rt,
                                            LogManager& lm) {
    auto log = lm.getLogger("main_log");
    if (rt.input_type == "file") {
        log->info("Realtime primary: file replay from {}", rt.input_path);
        auto s = RealtimeStream::openFile("primary", rt.input_path);
        if (!s) log->error("Failed to open RTCM replay file: {}", rt.input_path);
        return s;
    }
    if (rt.input_type == "ntrip") {
        NtripConfig cfg;
        cfg.host              = rt.ntrip_host;
        cfg.port              = rt.ntrip_port;
        cfg.mountpoint        = rt.ntrip_mountpoint;
        cfg.username          = rt.ntrip_user;
        cfg.password          = rt.ntrip_password;
        cfg.nmea_gga          = rt.nmea_gga;
        cfg.reconnect_delay_s = rt.reconnect_delay_s;
        log->info("Realtime primary: NTRIP {}:{}/{}", cfg.host, cfg.port, cfg.mountpoint);
        auto s = RealtimeStream::openNtrip("primary", cfg, lm);
        if (!s) log->error("Failed to start primary NTRIP client");
        return s;
    }
    if (rt.input_type == "tcp") {
        TcpConfig cfg;
        cfg.host              = rt.tcp_host;
        cfg.port              = rt.tcp_port;
        cfg.reconnect_delay_s = rt.reconnect_delay_s;
        log->info("Realtime primary: TCP {}:{}", cfg.host, cfg.port);
        auto s = RealtimeStream::openTcp("primary", cfg, lm);
        if (!s) log->error("Failed to start primary TCP client");
        return s;
    }
    log->error("Unsupported real-time input_type: '{}'", rt.input_type);
    return nullptr;
}

/// Build the optional SSR corrections stream.
std::unique_ptr<RealtimeStream> openCorrections(
    const RealTimeConfig::Corrections& cc,
    LogManager& lm) {
    auto log = lm.getLogger("main_log");
    if (cc.input_type == "file") {
        log->info("Realtime corrections: file replay from {}", cc.input_path);
        auto s = RealtimeStream::openFile("corrections", cc.input_path);
        if (!s) log->error("Failed to open corrections RTCM file: {}", cc.input_path);
        return s;
    }
    if (cc.input_type == "ntrip") {
        NtripConfig cfg;
        cfg.host              = cc.ntrip_host;
        cfg.port              = cc.ntrip_port;
        cfg.mountpoint        = cc.ntrip_mountpoint;
        cfg.username          = cc.ntrip_user;
        cfg.password          = cc.ntrip_password;
        cfg.nmea_gga          = cc.nmea_gga;
        cfg.reconnect_delay_s = cc.reconnect_delay_s;
        log->info("Realtime corrections: NTRIP {}:{}/{}", cfg.host, cfg.port, cfg.mountpoint);
        auto s = RealtimeStream::openNtrip("corrections", cfg, lm);
        if (!s) log->error("Failed to start corrections NTRIP client");
        return s;
    }
    if (cc.input_type == "tcp") {
        TcpConfig cfg;
        cfg.host              = cc.tcp_host;
        cfg.port              = cc.tcp_port;
        cfg.reconnect_delay_s = cc.reconnect_delay_s;
        log->info("Realtime corrections: TCP {}:{}", cfg.host, cfg.port);
        auto s = RealtimeStream::openTcp("corrections", cfg, lm);
        if (!s) log->error("Failed to start corrections TCP client");
        return s;
    }
    log->error("Unsupported corrections input_type: '{}'", cc.input_type);
    return nullptr;
}

/// Drain a stream chunk into its framer, then dispatch every complete
/// frame to the engine.  Returns the number of frames handed off.
std::size_t pumpFrames(RealtimeStream& src, RtcmStreamReader& framer,
                       RealTimeEngine& engine, std::vector<uint8_t>& scratch,
                       int timeout_ms) {
    constexpr std::size_t kChunk = 4096;
    const std::size_t n = src.read(scratch, kChunk, timeout_ms);
    if (n == 0) return 0;
    framer.feed(scratch.data(), n);
    std::size_t handled = 0;
    while (auto frame = framer.nextFrame()) {
        engine.feedRtcmFrame(*frame);
        ++handled;
    }
    return handled;
}

} // namespace

int Application::runRealTime() {
    auto log = log_manager_.getLogger("main_log");
    log->info("Starting real-time processing mode");

    if (!resolveData()) return 1;

    RealTimeEngine engine(config_, log_manager_);
    if (!engine.initialize()) {
        log->error("Failed to initialize real-time engine");
        return 1;
    }

    const auto& rt_config = config_.getRealTimeConfig();

    auto primary = openPrimary(rt_config, log_manager_);
    if (!primary) return 1;

    std::unique_ptr<RealtimeStream> corrections;
    if (rt_config.corrections.enabled) {
        corrections = openCorrections(rt_config.corrections, log_manager_);
        if (!corrections) {
            log->warn("Corrections stream failed to start; continuing with obs-only mode");
        }
    }

    RtcmStreamReader   primary_framer;
    RtcmStreamReader   corr_framer;
    std::vector<uint8_t> scratch;

    const auto wall_start = std::chrono::high_resolution_clock::now();

    // Single-threaded multi-source poll loop.  Each iteration drains a
    // chunk from each active source, frames it, and dispatches frames
    // to the engine.  When neither source produces any bytes this turn
    // we yield briefly to avoid a busy spin (file replay: streams are
    // exhausted; NTRIP: tryRead has already waited internally).
    while (primary->active() || (corrections && corrections->active())) {
        std::size_t produced = 0;

        if (primary->active()) {
            // File replay benefits from polling with no extra delay
            // (read returns immediately on EOF).  NTRIP polls with a
            // short timeout so the corrections side gets serviced.
            const int timeout_ms = (rt_config.input_type == "file") ? 0 : 50;
            produced += pumpFrames(*primary, primary_framer, engine, scratch, timeout_ms);
        }
        if (corrections && corrections->active()) {
            const int timeout_ms = (rt_config.corrections.input_type == "file") ? 0 : 50;
            produced += pumpFrames(*corrections, corr_framer, engine, scratch, timeout_ms);
        }
        if (produced == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    primary->stop();
    if (corrections) corrections->stop();

    const auto wall_end = std::chrono::high_resolution_clock::now();
    const auto dur_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                            wall_end - wall_start).count();
    log->info("Real-time session ended: {} epochs, {} ms wall, primary frames={}/discarded={}, "
              "corrections frames={}/discarded={}",
              engine.epochCount(), dur_ms,
              primary_framer.framesExtracted(), primary_framer.bytesDiscarded(),
              corr_framer.framesExtracted(),    corr_framer.bytesDiscarded());

    return 0;
}

} // namespace GnssProcessor
