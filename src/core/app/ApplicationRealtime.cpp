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
#include "sbf/SbfStreamReader.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "slip/MwSlipDetector.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <fmt/format.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX            // keep windows.h from clobbering std::min/std::max (Eigen)
#  endif
#  include <windows.h>
#else
#  include <csignal>
#endif

namespace GnssProcessor {

namespace {

// Cooperative stop flag for the real-time poll loop.  A live TCP/NTRIP stream
// never reaches EOF, so the only way to end the session is an interrupt; this
// lets Ctrl+C unwind the loop normally (flush the final epoch, close trace
// files, shut down logging) instead of hard-killing the process.  Set from an
// async handler, so it must stay a plain atomic — nothing else.
std::atomic<bool> g_stop_requested{ false };

#ifdef _WIN32
// Console control handler (runs on its own OS-spawned thread).  Catches Ctrl+C,
// Ctrl+Break AND console-window close / logoff / shutdown — std::signal would
// miss the close case, which is a common way to stop a console app and would
// otherwise lose the trace tail.  Returning TRUE marks the event handled so the
// default terminator doesn't fire; a SECOND Ctrl+C/Break (flag already set)
// returns FALSE to let the default force-kill, preserving the usual escalation.
BOOL WINAPI realtimeConsoleHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return g_stop_requested.exchange(true) ? FALSE : TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop_requested.store(true, std::memory_order_relaxed);
        return TRUE;
    default:
        return FALSE;
    }
}
#else
// POSIX: the only async-signal-safe action is the atomic store.
void realtimeSignalHandler(int /*sig*/) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}
#endif

// Install/remove the cooperative-stop handler around the poll loop.
void installStopHandler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(realtimeConsoleHandler, TRUE);
#else
    std::signal(SIGINT,  realtimeSignalHandler);
    std::signal(SIGTERM, realtimeSignalHandler);
#endif
}
void removeStopHandler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(realtimeConsoleHandler, FALSE);
#else
    std::signal(SIGINT,  SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
#endif
}

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
        // A socket source is active while its client is running, NOT only
        // while momentarily connected: the client auto-reconnects internally,
        // and at startup isConnected() is briefly false until the background
        // thread dials in.  Gating on isConnected() here would let the poll
        // loop exit before the first byte (0 epochs) or on any transient drop.
        case Kind::Ntrip: return ntrip_ && ntrip_->isRunning();
        case Kind::Tcp:   return tcp_   && tcp_->isRunning();
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
        cfg.use_tls           = rt.ntrip_use_tls;
        cfg.tls_insecure      = rt.ntrip_tls_insecure;
        log->info("Realtime primary: NTRIP {}://{}:{}/{}",
                  cfg.use_tls ? "https" : "http", cfg.host, cfg.port, cfg.mountpoint);
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

/// Build an optional auxiliary RTCM stream (SSR corrections or broadcast
/// ephemeris).  Both share the @c RealTimeConfig::Corrections transport
/// descriptor and route to @c feedRtcmFrame; only the @p label (used for the
/// stream tag and log lines) distinguishes them.  Templated on the config so
/// the @c Ephemeris alias resolves without an extra overload.
template <typename AuxCfg>
std::unique_ptr<RealtimeStream> openAuxRtcmStream(
    const char* label, const AuxCfg& cc, LogManager& lm) {
    auto log = lm.getLogger("main_log");
    if (cc.input_type == "file") {
        log->info("Realtime {}: file replay from {}", label, cc.input_path);
        auto s = RealtimeStream::openFile(label, cc.input_path);
        if (!s) log->error("Failed to open {} RTCM file: {}", label, cc.input_path);
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
        cfg.use_tls           = cc.ntrip_use_tls;
        cfg.tls_insecure      = cc.ntrip_tls_insecure;
        log->info("Realtime {}: NTRIP {}://{}:{}/{}", label,
                  cfg.use_tls ? "https" : "http", cfg.host, cfg.port, cfg.mountpoint);
        auto s = RealtimeStream::openNtrip(label, cfg, lm);
        if (!s) log->error("Failed to start {} NTRIP client", label);
        return s;
    }
    if (cc.input_type == "tcp") {
        TcpConfig cfg;
        cfg.host              = cc.tcp_host;
        cfg.port              = cc.tcp_port;
        cfg.reconnect_delay_s = cc.reconnect_delay_s;
        log->info("Realtime {}: TCP {}:{}", label, cfg.host, cfg.port);
        auto s = RealtimeStream::openTcp(label, cfg, lm);
        if (!s) log->error("Failed to start {} TCP client", label);
        return s;
    }
    log->error("Unsupported {} input_type: '{}'", label, cc.input_type);
    return nullptr;
}

/// Drain a stream chunk into its framer, then dispatch every complete frame
/// to @p feed.  Generic over the framer (RtcmStreamReader / SbfStreamReader),
/// which share the feed()/nextFrame() contract, and over the per-frame sink so
/// the primary stream can route to feedRtcmFrame or feedSbfFrame by format.
template <typename Framer, typename FeedFn>
std::size_t pumpFrames(RealtimeStream& src, Framer& framer, FeedFn&& feed,
                       std::vector<uint8_t>& scratch, int timeout_ms,
                       std::size_t max_drain_bytes = (1u << 20)) {
    // Bounded drain: empty whatever is already buffered this iteration instead of
    // a single 4 KB chunk, so a high-rate obs stream can't accumulate a backlog
    // faster than it is consumed (the root of the live-obs-lagging-corrections
    // bug).  The FIRST read honours the caller's timeout; subsequent reads use
    // timeout 0 so we drain only what's present without stalling the poll loop.
    // max_drain_bytes caps one burst so the obs drain never monopolizes the
    // iteration and starves the sparse SSR/nav streams.
    constexpr std::size_t kChunk = 4096;
    std::size_t handled = 0, drained = 0;
    int to = timeout_ms;
    for (;;) {
        const std::size_t n = src.read(scratch, kChunk, to);
        if (n == 0) break;
        framer.feed(scratch.data(), n);
        while (auto frame = framer.nextFrame()) {
            feed(*frame);
            ++handled;
        }
        drained += n;
        if (n < kChunk || drained >= max_drain_bytes) break;  // buffer drained, or budget hit
        to = 0;
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
        corrections = openAuxRtcmStream("corrections", rt_config.corrections, log_manager_);
        if (!corrections) {
            log->warn("Corrections stream failed to start; continuing with obs-only mode");
        }
    }

    // Optional third stream: dedicated broadcast ephemeris (nav) for an
    // obs-only station that carries no 1019/1020/etc.  Routes to feedRtcmFrame
    // like corrections — decoded nav lands in LiveEphemerisStore.  Without it,
    // an obs-only primary yields 0 epochs (no orbit/clock baseline for SSR).
    std::unique_ptr<RealtimeStream> ephemeris;
    if (rt_config.ephemeris.enabled) {
        ephemeris = openAuxRtcmStream("ephemeris", rt_config.ephemeris, log_manager_);
        if (!ephemeris) {
            log->warn("Ephemeris stream failed to start; broadcast orbits/clocks "
                      "will be unavailable unless the primary carries nav");
        }
    }

    // Primary stream wire format: RTCM3 (default) or Septentrio SBF.  The
    // corrections (IGS-SSR) and ephemeris (nav) streams are always RTCM.
    const bool primary_sbf = (rt_config.input_format == "sbf");
    log->info("Real-time primary format: {}", primary_sbf ? "SBF" : "RTCM3");

    RtcmStreamReader   primary_rtcm;
    SbfStreamReader    primary_sbf_framer;
    RtcmStreamReader   corr_framer;
    RtcmStreamReader   eph_framer;
    std::vector<uint8_t> scratch;

    // Install the Ctrl+C / termination handler so a live stream can be ended
    // cleanly.  Reset the flag first in case runRealTime is ever re-entered.
    g_stop_requested.store(false, std::memory_order_relaxed);
    installStopHandler();
    log->info("Real-time session running — press Ctrl+C to stop");

    const auto wall_start = std::chrono::high_resolution_clock::now();

    // Single-threaded multi-source poll loop.  Each iteration drains a
    // chunk from each active source, frames it, and dispatches frames
    // to the engine.  When neither source produces any bytes this turn
    // we yield briefly to avoid a busy spin (file replay: streams are
    // exhausted; NTRIP/TCP: tryRead has already waited internally).  A
    // stop request (Ctrl+C) breaks the loop so the flush/stop below runs.
    while (!g_stop_requested.load(std::memory_order_relaxed)
           && !engine.maxDurationReached()
           && (primary->active() || (corrections && corrections->active())
                                 || (ephemeris   && ephemeris->active()))) {
        std::size_t produced = 0;

        if (primary->active()) {
            // File replay polls with no extra delay (read returns
            // immediately on EOF); sockets poll with a short timeout so the
            // corrections side gets serviced.
            const int timeout_ms = (rt_config.input_type == "file") ? 0 : 50;
            if (primary_sbf) {
                produced += pumpFrames(*primary, primary_sbf_framer,
                    [&](const std::vector<uint8_t>& f) { engine.feedSbfFrame(f); },
                    scratch, timeout_ms);
            } else {
                produced += pumpFrames(*primary, primary_rtcm,
                    [&](const std::vector<uint8_t>& f) { engine.feedRtcmFrame(f); },
                    scratch, timeout_ms);
            }
        }
        if (corrections && corrections->active()) {
            const int timeout_ms = (rt_config.corrections.input_type == "file") ? 0 : 50;
            // Single-chunk (max_drain_bytes=4096): SSR is sparse but critical;
            // serviced every iteration, never throttled by the obs drain.
            produced += pumpFrames(*corrections, corr_framer,
                [&](const std::vector<uint8_t>& f) { engine.feedRtcmFrame(f); },
                scratch, timeout_ms, 4096);
        }
        if (ephemeris && ephemeris->active()) {
            const int timeout_ms = (rt_config.ephemeris.input_type == "file") ? 0 : 50;
            // Single-chunk like corrections: broadcast nav is sparse; serviced
            // every iteration so the obs drain never starves it.
            produced += pumpFrames(*ephemeris, eph_framer,
                [&](const std::vector<uint8_t>& f) { engine.feedRtcmFrame(f); },
                scratch, timeout_ms, 4096);
        }
        if (produced == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Either a stream went inactive (file EOF / disconnect) or the user asked
    // to stop.  Restore the default signal disposition (a second Ctrl+C during
    // teardown then force-kills) and submit the final buffered epoch before
    // tearing down so the last observations and trace rows aren't lost.
    removeStopHandler();
    if (g_stop_requested.load(std::memory_order_relaxed))
        log->info("Stop requested (Ctrl+C) — flushing and shutting down cleanly");
    else if (engine.maxDurationReached())
        log->info("Max processing duration reached — flushing and shutting down cleanly");

    engine.flush();

    primary->stop();
    if (corrections) corrections->stop();
    if (ephemeris)   ephemeris->stop();

    const auto wall_end = std::chrono::high_resolution_clock::now();
    const auto dur_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                            wall_end - wall_start).count();
    const uint64_t pframes    = primary_sbf ? primary_sbf_framer.framesExtracted()
                                            : primary_rtcm.framesExtracted();
    const uint64_t pdiscarded = primary_sbf ? primary_sbf_framer.bytesDiscarded()
                                            : primary_rtcm.bytesDiscarded();
    log->info("Real-time session ended: {} epochs, {} ms wall, primary frames={}/discarded={}, "
              "corrections frames={}/discarded={}, ephemeris frames={}/discarded={}",
              engine.epochCount(), dur_ms,
              pframes, pdiscarded,
              corr_framer.framesExtracted(), corr_framer.bytesDiscarded(),
              eph_framer.framesExtracted(),  eph_framer.bytesDiscarded());

    return 0;
}

} // namespace GnssProcessor
