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
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
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

    const auto& rt_config = config_.getRealTimeConfig();

    // --- RTA-6/RTA-7: standalone network-product consumer -------------------
    // A non-empty realtime.network_products_path arms a live product-file tail
    // feeding an OWNED SsrCorrectionStore injected into the engine.  The
    // processing.network_consumer profile applies the shared consumer couplings
    // (one source of truth with the closed-loop harness) BEFORE the engine is
    // constructed so they shape the state vector.  Both default off — the
    // realtime path is byte-identical without them.
    const std::string& net_products_path = rt_config.network_products_path;
    const auto& ncc = config_.getNetworkConsumerConfig();
    const bool consumer_profile_on = ncc.enable && !net_products_path.empty();
    if (ncc.enable && net_products_path.empty())
        log->warn("processing.network_consumer.enable is set but "
                  "realtime.network_products_path is empty — no product source; "
                  "the profile is NOT applied.");
    if (consumer_profile_on)
        config_.applyNetworkConsumerProfile(log_manager_);

    // T3d: one-shot honesty warnings for a file-tail consumer profile — some correction
    // channels are only conditionally (σ) or never (in-process iono) present on the wire.
    if (consumer_profile_on) {
        if (config_.networkConsumePhaseBiasSigma())
            log->warn("Consumer: network.consume_phase_bias_sigma is on — the per-phase-bias σ "
                      "is received ONLY if the tailed producer emitted network.emit_phase_bias_sigma "
                      "(the P-record sigma= token, T3d); without it the σ is absent and the "
                      "R-inflation is inert.");
        if (config_.getNetworkConfig().closed_loop.consume_network_iono)
            log->warn("Consumer: consume_network_iono is on with a file-tail source — the SH-VTEC "
                      "slant iono is an IN-PROCESS supply (pushSlantIono) with NO product wire "
                      "record, so a file-tail consumer cannot receive it (the setting is inert here).");
    }

    // Declared BEFORE the engine so they outlive it (the engine holds a
    // non-owning pointer to the store via setSsrStore).
    std::unique_ptr<SsrCorrectionStore> network_store;
    std::unique_ptr<NetworkProductTail> product_tail;

    RealTimeEngine engine(config_, log_manager_);
    if (!engine.initialize()) {
        log->error("Failed to initialize real-time engine");
        return 1;
    }

    if (!net_products_path.empty()) {
        network_store = std::make_unique<SsrCorrectionStore>();
        // RTA-13 (opt-in): arm the CODE-domain band-wise bias fallback on the
        // OWNED store (the singleton is armed by RealTimeEngine::initialize).
        if (config_.ssrBiasBandFallback())
            network_store->setCodeBiasBandFallback(true);
        // Card 7 (opt-in): arm the orbit/clock coherence skew budget on the
        // OWNED store — the network clock records this store serves carry the
        // producer's orbit echo, so this is where the withhold policy lives.
        if (const double skew = ncc.orbit_clock_max_skew_s; skew > 0.0)
            network_store->setOrbitEchoMaxSkew(skew);
        // Start-of-file join: replaying the whole file so far reconstructs every
        // discontinuity counter exactly (a fresh consumer start); a producer
        // restart mid-session is handled by the tail's rotation reopen.
        product_tail  = std::make_unique<NetworkProductTail>(net_products_path);
        const bool consume_clock = consumer_profile_on ? ncc.clock : true;
        const bool consume_bias  = consumer_profile_on ? ncc.bias  : true;
        // RTA-8(a): orbit-overlay routing.  Without the profile keep the legacy
        // coupled routing (orbit rides with the clock — byte-identical).  With
        // the profile, orbit_source selects: "external" (default) pins the
        // orbit overlay on the singleton — the external RTS corrections stream
        // — while clocks/biases come from the owned store; "network" points it
        // at the owned store (future orbit-bearing product).
        RealTimeEngine::SsrOrbitSource orbit_source =
            RealTimeEngine::SsrOrbitSource::FollowClock;
        if (consumer_profile_on)
            orbit_source = (ncc.orbit_source == "network")
                               ? RealTimeEngine::SsrOrbitSource::Network
                               : RealTimeEngine::SsrOrbitSource::External;
        engine.setSsrStore(network_store.get(), consume_clock, consume_bias, orbit_source);
        log->info("Realtime network products (RTA-6): tailing '{}' into an owned store "
                  "(consume clock {}, bias {}; orbit overlay {}; profile {}).",
                  net_products_path, consume_clock ? "on" : "off",
                  consume_bias ? "on" : "off",
                  orbit_source == RealTimeEngine::SsrOrbitSource::FollowClock
                      ? "coupled to clock (legacy)"
                      : (orbit_source == RealTimeEngine::SsrOrbitSource::Network
                             ? "owned store (network)"
                             : "singleton (external RTS)"),
                  consumer_profile_on ? "network_consumer" : "default flags");
        // RTA-8(c): the classic misconfiguration — network clocks consumed while
        // an unfiltered external corrections stream still pushes clock content
        // into the singleton.  The singleton's clocks then shadow the network
        // product wherever the singleton is consulted (bias-only clock mode, the
        // obs↔SSR sync horizon, coverage census).  Filter the stream instead.
        if (consumer_profile_on && ncc.orbit_source == "external"
            && rt_config.corrections.enabled && rt_config.corrections.accept == "all") {
            log->warn("Network-consumer profile is on with orbit_source=external, but the "
                      "external corrections stream has corrections_accept=\"all\" — its "
                      "1060/1066 clock content lands in the singleton store and may shadow "
                      "the network clock.  Set realtime.corrections.corrections_accept="
                      "\"orbit_only\" to ingest orbits (and URA) only.");
        }
    }
    // Effective AR MW-WL suppression: the profile resolution when active
    // (explicit override, else the closed-loop-equivalent derivation), otherwise
    // the standalone processing.ar_ignore_mw_wl key (default false — no change).
    engine.setArIgnoreMwWl(consumer_profile_on ? config_.networkConsumerArIgnoreMwWl()
                                               : config_.arIgnoreMwWl());

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

    // RTA-8(b): per-stream SSR ingest policies (parsed/validated at config load;
    // the parse helper only translates the vetted string).  The primary stream
    // is never filtered — it carries obs/nav, and any embedded SSR keeps the
    // legacy unfiltered behaviour.
    const auto corr_policy =
        RealTimeEngine::parseSsrAcceptPolicy(rt_config.corrections.accept);
    const auto eph_policy =
        RealTimeEngine::parseSsrAcceptPolicy(rt_config.ephemeris.accept);

    // --- RTA-7(c): startup / per-change S-contract check ---------------------
    // When the tail sees a contract (v2 header S line, or an RTA-5 re-broadcast
    // after a mid-file join) and the consumer profile is on, cross-check it
    // against the consumer's declared expectation and HARD-WARN (never abort) on
    // mismatch — once per contract CHANGE, keyed on the convention fields
    // (pivot-only refreshes are routine handovers, not contract changes).  The
    // expectation is pivot-less (like the closed-loop per-receipt check): the
    // FIRST received contract seeds it, overlaid with the profile's optional
    // expect_* pins so a declared pin mismatches even on first receipt.
    SbasisContract contract_expectation;
    bool           have_contract_expectation = false;
    std::string    last_contract_fp;
    auto contractFp = [](const SbasisContract& c) {
        // RTA-12: the identity fields (frame/cepoch/atx/oref) are convention
        // fields too — a producer restart with a different frame or ATX must
        // count as a contract CHANGE and re-run the check.
        return fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                           c.decl.reference_receiver_id, c.decl.bias_sign,
                           c.decl.windup_yaw_model_id,
                           static_cast<int>(c.decl.amb_datum),
                           static_cast<int>(c.decl.isb_datum),
                           c.decl.emit_code_bias,
                           static_cast<int>(c.decl.code_bias_datum),
                           c.disc_threshold_m, c.emit_clock_disc,
                           c.apriori_product_id, c.staleness_horizon_s,
                           c.generation_id,
                           c.frame_id, c.coord_epoch, c.atx_id, c.orbit_ref);
    };
    auto checkConsumerContract = [&](const SbasisContract& file_contract) {
        const std::string fp = contractFp(file_contract);
        if (fp == last_contract_fp) return;   // once per contract CHANGE
        last_contract_fp = fp;
        if (!have_contract_expectation) {
            contract_expectation = file_contract;              // adopt the first receipt...
            contract_expectation.decl.clock_pivot_sat.clear(); // ...pivot-less (pivots evolve)
            if (!ncc.expect_reference_receiver.empty())
                contract_expectation.decl.reference_receiver_id = ncc.expect_reference_receiver;
            if (!ncc.expect_apriori_product_id.empty())
                contract_expectation.apriori_product_id = ncc.expect_apriori_product_id;
            // RTA-12 identity pins: a declared expectation overrides the adopted
            // first receipt, so a pinned frame/ATX/orbit-ref mismatches even on
            // the very first contract (checkContract compares each only when the
            // expectation is non-empty).
            if (!ncc.expect_frame_id.empty())
                contract_expectation.frame_id = ncc.expect_frame_id;
            if (!ncc.expect_atx_id.empty())
                contract_expectation.atx_id = ncc.expect_atx_id;
            if (!ncc.expect_orbit_ref.empty())
                contract_expectation.orbit_ref = ncc.expect_orbit_ref;
            // Wind-up model: pin to what THIS BINARY implements, never to the
            // adopted receipt.  Every other field above is a config pin that
            // falls back to the file's own value, which makes checkContract
            // compare the string to itself — fine for conventions the operator
            // chooses, useless for one the CODE fixes.  A pre-fix product
            // (windup_id "nominal") absorbed +2*phi_wu into its satellite clocks
            // and phase biases; consuming it with a corrected model is a silent
            // phase-frame mismatch, and this line is the only thing that makes
            // it audible.  See docs/SPEC_phase_windup_sign.md section 8 item 2.
            contract_expectation.decl.windup_yaw_model_id = kWindUpYawModelId;
            have_contract_expectation = true;
        }
        std::vector<std::string> mismatches;
        if (NetworkProductReader::checkContract(file_contract, contract_expectation,
                                                &mismatches)) {
            log->info("Network products: S-basis contract verified (ref rx '{}', "
                      "a-priori '{}', gid {}).",
                      file_contract.decl.reference_receiver_id,
                      file_contract.apriori_product_id, file_contract.generation_id);
        } else {
            std::string joined;
            for (const auto& m : mismatches) {
                if (!joined.empty()) joined += "; ";
                joined += m;
            }
            log->warn("Network products: S-basis contract MISMATCH against the consumer "
                      "expectation — products may be in an incompatible frame (integer "
                      "recovery / counter replay compromised).  Continuing anyway.  {}",
                      joined);
        }
    };

    // Install the Ctrl+C / termination handler so a live stream can be ended
    // cleanly.  Reset the flag first in case runRealTime is ever re-entered.
    g_stop_requested.store(false, std::memory_order_relaxed);
    installStopHandler();
    log->info("Real-time session running — press Ctrl+C to stop");

    const auto wall_start = std::chrono::high_resolution_clock::now();
    // RTA-6: last product-tail poll instant (throttles the file stat to ~5 Hz).
    auto last_tail_poll = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    // RTA-10: content-epoch key (week·604800+tow) of the last producer heartbeat pushed
    // into the owned store, so each H record is forwarded exactly once (-1 = none yet).
    double last_status_pushed_s = -1.0;

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
                // RTB-1(b): forward the framer-boundary receipt stamp so the
                // engine can account sub-second receive→process latency.
                produced += pumpFrames(*primary, primary_rtcm,
                    [&](const std::vector<uint8_t>& f) {
                        engine.feedRtcmFrame(f, RealTimeEngine::SsrAcceptPolicy::All,
                                             primary_rtcm.lastFrameReceipt());
                    },
                    scratch, timeout_ms);
            }
        }
        if (corrections && corrections->active()) {
            const int timeout_ms = (rt_config.corrections.input_type == "file") ? 0 : 50;
            // Single-chunk (max_drain_bytes=4096): SSR is sparse but critical;
            // serviced every iteration, never throttled by the obs drain.
            // RTA-8(b): frames from THIS stream carry its ingest accept policy.
            // RTB-1(b): plus the framer-boundary receipt stamp.
            produced += pumpFrames(*corrections, corr_framer,
                [&](const std::vector<uint8_t>& f) {
                    engine.feedRtcmFrame(f, corr_policy, corr_framer.lastFrameReceipt());
                },
                scratch, timeout_ms, 4096);
        }
        if (ephemeris && ephemeris->active()) {
            const int timeout_ms = (rt_config.ephemeris.input_type == "file") ? 0 : 50;
            // Single-chunk like corrections: broadcast nav is sparse; serviced
            // every iteration so the obs drain never starves it.
            produced += pumpFrames(*ephemeris, eph_framer,
                [&](const std::vector<uint8_t>& f) {
                    engine.feedRtcmFrame(f, eph_policy, eph_framer.lastFrameReceipt());
                },
                scratch, timeout_ms, 4096);
        }
        // RTA-6: poll the live network-product tail.  Throttled to ~5 Hz — the
        // product cadence is seconds, and a hot file-replay loop must not stat
        // the file thousands of times a second.  New epochs go into the owned
        // store, then the engine's obs↔SSR hold buffer is woken exactly like an
        // RTCM SSR push would (notifyProductsAdvanced -> drainHeldObs).
        if (product_tail) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_tail_poll >= std::chrono::milliseconds(200)) {
                last_tail_poll = now;
                const auto new_products = product_tail->pollInto(*network_store);
                if (!new_products.empty()) {
                    produced += new_products.size();
                    log->debug("Network products: {} new epoch(s) from tail "
                               "(store size {}).",
                               new_products.size(), network_store->size());
                    engine.notifyProductsAdvanced();   // RTA-6(d): product-arrival wake
                    if (consumer_profile_on)
                        for (const auto& rec : new_products)
                            if (rec.contract_refresh.present)
                                checkConsumerContract(rec.contract_refresh);
                }
                // RTA-10: forward the producer's epoch-status heartbeat (H record) into
                // the owned store.  Read the tail's latest-status surface — NOT the
                // per-record field — because a fully WITHHELD epoch's H completes no
                // product record (new_products can be empty while the producer is
                // withholding, which is exactly when the consumer must hear about it).
                // Keyed on the heartbeat's content epoch so each H is pushed once; the
                // engine logs one-shot status TRANSITIONS from the store surface.
                if (const auto hst = product_tail->lastStatus()) {
                    const double hkey = hst->time.week * 604800.0 + hst->time.tow;
                    if (hkey != last_status_pushed_s) {
                        last_status_pushed_s = hkey;
                        network_store->setProducerStatus(*hst);
                    }
                }
            }
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
