/**
 * @file NetworkStreamFrontEnd.cpp
 * @brief RTF-3 multi-station streaming front-end (implementation).
 *
 * See the header for the architecture.  The ingest loop is deliberately
 * single-threaded over all lanes: every transport buffers on its own receive
 * thread, so the sweep here only drains ring buffers and decodes — cheap at
 * the 50-station 1 Hz design point, and it keeps the aligner single-writer.
 */
#include "realtime/NetworkStreamFrontEnd.hpp"

#include "math/TimeUtils.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/TcpClient.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// LaneByteSource — minimal transport wrapper (see header for the deliberate
// RealtimeStream duplication note).
// ---------------------------------------------------------------------------

bool NetworkStreamFrontEnd::LaneByteSource::active() const {
    switch (kind) {
        case Kind::File:  return !file_eof;
        // Running, NOT momentarily-connected: the clients auto-reconnect and
        // are briefly unconnected at startup — gating on isConnected() here
        // would end a lane on any transient drop (the RealtimeStream lesson).
        case Kind::Ntrip: return ntrip && ntrip->isRunning();
        case Kind::Tcp:   return tcp   && tcp->isRunning();
    }
    return false;
}

std::size_t NetworkStreamFrontEnd::LaneByteSource::read(
    std::vector<std::uint8_t>& out, std::size_t max_bytes, int timeout_ms) {
    switch (kind) {
        case Kind::File: {
            out.resize(max_bytes);
            file.read(reinterpret_cast<char*>(out.data()),
                      static_cast<std::streamsize>(max_bytes));
            const auto n = static_cast<std::size_t>(file.gcount());
            out.resize(n);
            if (n == 0 || file.eof()) file_eof = true;
            return n;
        }
        case Kind::Ntrip:
            return ntrip ? ntrip->tryRead(out, max_bytes, timeout_ms) : 0;
        case Kind::Tcp:
            return tcp ? tcp->tryRead(out, max_bytes, timeout_ms) : 0;
    }
    return 0;
}

void NetworkStreamFrontEnd::LaneByteSource::stop() {
    if (ntrip) ntrip->stop();
    if (tcp)   tcp->stop();
    if (file.is_open()) file.close();
}

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

NetworkStreamFrontEnd::NetworkStreamFrontEnd(const GnssConfig& config,
                                             LogManager& log_manager)
    : config_(config), log_manager_(log_manager),
      queue_(static_cast<std::size_t>(
          std::max(1, config.getNetworkConfig().streaming.queue_depth_epochs))) {}

NetworkStreamFrontEnd::~NetworkStreamFrontEnd() { stop(); }

bool NetworkStreamFrontEnd::openLane(Lane& lane, const StationTransportConfig& tc) {
    auto log = log_manager_.getLogger("main_log");
    if (tc.input_type == "ntrip") {
        NtripConfig cfg;
        cfg.host              = tc.host;
        cfg.port              = tc.port;
        cfg.mountpoint        = tc.mountpoint;
        cfg.username          = tc.username;
        cfg.password          = tc.password;
        cfg.reconnect_delay_s = tc.reconnect_delay_s;
        cfg.use_tls           = tc.use_tls;
        cfg.tls_insecure      = tc.tls_insecure;
        lane.source.kind  = LaneByteSource::Kind::Ntrip;
        lane.source.ntrip = std::make_unique<NtripClient>(cfg, log_manager_);
        if (!lane.source.ntrip->start()) {
            log->error("Network (RTF-3): lane '{}' NTRIP {}://{}:{}/{} failed to start",
                       lane.station, tc.use_tls ? "https" : "http", tc.host, tc.port,
                       tc.mountpoint);
            return false;
        }
        log->info("Network (RTF-3): lane '{}' NTRIP {}://{}:{}/{}", lane.station,
                  tc.use_tls ? "https" : "http", tc.host, tc.port, tc.mountpoint);
        return true;
    }
    if (tc.input_type == "tcp") {
        TcpConfig cfg;
        cfg.host              = tc.host;
        cfg.port              = tc.port;
        cfg.reconnect_delay_s = tc.reconnect_delay_s;
        lane.source.kind = LaneByteSource::Kind::Tcp;
        lane.source.tcp  = std::make_unique<TcpClient>(cfg, log_manager_);
        if (!lane.source.tcp->start()) {
            log->error("Network (RTF-3): lane '{}' TCP {}:{} failed to start",
                       lane.station, tc.host, tc.port);
            return false;
        }
        log->info("Network (RTF-3): lane '{}' TCP {}:{}", lane.station, tc.host, tc.port);
        return true;
    }
    if (tc.input_type == "file") {
        lane.source.kind = LaneByteSource::Kind::File;
        lane.source.file.open(tc.input_path, std::ios::binary);
        if (!lane.source.file.is_open()) {
            log->error("Network (RTF-3): lane '{}' replay file '{}' failed to open",
                       lane.station, tc.input_path);
            return false;
        }
        log->info("Network (RTF-3): lane '{}' RTCM3 replay from {}", lane.station,
                  tc.input_path);
        return true;
    }
    log->error("Network (RTF-3): lane '{}' has unsupported transport input_type '{}' "
               "(expected ntrip | tcp | file)", lane.station, tc.input_type);
    return false;
}

bool NetworkStreamFrontEnd::start() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();
    const auto& ns = nc.streaming;

    // Lane candidates: the roster, plus the held-out closed-loop consumer
    // station when it carries its own transport (the loader already warned
    // that without one it grades zero epochs).
    std::vector<const StationConfig*> candidates;
    for (const auto& s : nc.stations) candidates.push_back(&s);
    if (nc.closed_loop.enable && nc.closed_loop.station.transport.configured())
        candidates.push_back(&nc.closed_loop.station);

    for (const auto* sc : candidates) {
        if (!sc->transport.configured()) {
            log->warn("Network (RTF-3): station '{}' has no transport descriptor — it "
                      "will be ABSENT from every streamed epoch (fed only by the batch "
                      "path, which streaming mode replaces).", sc->name);
            continue;
        }
        auto lane = std::make_unique<Lane>();
        lane->station       = sc->name;
        lane->stats.station = sc->name;
        if (!openLane(*lane, sc->transport)) continue;   // prefer-degraded: skip, never sink the roster
        lane->stats.active = true;
        lanes_.push_back(std::move(lane));
    }
    if (lanes_.empty()) {
        log->error("Network (RTF-3): no lane could be opened — streaming front-end "
                   "cannot start.");
        return false;
    }

    // Aligner policy: the datum resolution mirrors the producer ("" => first
    // roster station realizes the clock datum).
    NetworkStreamAligner::Config ac;
    ac.wait_budget_s      = ns.wait_budget_s;
    ac.min_stations       = ns.min_stations;
    ac.require_datum      = ns.require_datum_station;
    ac.datum_station      = nc.clock_datum_station.empty()
                                ? (nc.stations.empty() ? std::string{}
                                                       : nc.stations.front().name)
                                : nc.clock_datum_station;
    ac.epoch_interval_s   = ns.epoch_interval_s;
    ac.max_pending_epochs = ns.max_pending_epochs;
    aligner_ = std::make_unique<NetworkStreamAligner>(ac, lanes_.size());

    stop_requested_.store(false);
    started_.store(true);
    ingest_thread_ = std::thread(&NetworkStreamFrontEnd::ingestLoop, this);
    log->info("Network (RTF-3): streaming front-end UP — {} lane(s), wait budget "
              "{:.2f} s, min {} station(s)/epoch, datum '{}'{}, queue depth {}.",
              lanes_.size(), ac.wait_budget_s, ac.min_stations, ac.datum_station,
              ac.require_datum ? " (required)" : "", nc.streaming.queue_depth_epochs);
    return true;
}

void NetworkStreamFrontEnd::stop() {
    if (!started_.exchange(false)) {
        // Never started (or already stopped): still ensure transports die.
        for (auto& l : lanes_) l->source.stop();
        return;
    }
    stop_requested_.store(true);
    for (auto& l : lanes_) l->source.stop();   // unblocks lane reads
    if (ingest_thread_.joinable()) ingest_thread_.join();
    queue_.close();
}

bool NetworkStreamFrontEnd::next(SyncedEpoch& out) {
    for (;;) {
        if (queue_.pop(out, kNextPollSliceMs)) return true;
        if (queue_.closed() && queue_.size() == 0) return false;   // terminal
    }
}

// ---------------------------------------------------------------------------
// Decode tail (static, pure): identity + LLI + epoch grouping
// ---------------------------------------------------------------------------

std::vector<std::pair<GnssTime, std::vector<RinexObservation>>>
NetworkStreamFrontEnd::tagAndGroup(std::vector<RinexObservation> obs,
                                   const std::string& station,
                                   LockTimeTracker* tracker,
                                   bool lli_from_locktime) {
    std::vector<std::pair<GnssTime, std::vector<RinexObservation>>> batches;
    for (auto& o : obs) {
        // STATION IDENTITY at the framer boundary: the lane's roster name, not
        // anything inferred from the stream.  This is the single write that
        // keeps N streams from merging into one phantom station.
        o.receiver_id = station;
        if (lli_from_locktime && tracker) {
            // RTB-3: LLI 0x1 from MSM lock-time regression, tracked per
            // (station, sat, signal) through the SHARED tracker — the
            // station key is what keeps two lanes' histories apart.
            for (const auto& [code, lock_ms] : o.lock_time_ms) {
                if (tracker->update(station, o.satellite, code, lock_ms))
                    o.lli_flags[code] |= 0x1;
            }
        }
        // Millisecond-exact epoch grouping (a drain may span an epoch
        // boundary; one MSM frame is one constellation at one tow).  Batches
        // keep first-seen order — the aligner merges same-epoch batches.
        const auto same_epoch = [&](const GnssTime& t) {
            return t.week == o.time.week
                   && std::llround(t.tow * 1e3) == std::llround(o.time.tow * 1e3);
        };
        if (batches.empty() || !same_epoch(batches.back().first)) {
            auto it = std::find_if(batches.begin(), batches.end(),
                                   [&](const auto& b) { return same_epoch(b.first); });
            if (it == batches.end()) {
                batches.emplace_back(o.time, std::vector<RinexObservation>{});
                it = std::prev(batches.end());
            }
            it->second.push_back(std::move(o));
        } else {
            batches.back().second.push_back(std::move(o));
        }
    }
    return batches;
}

// ---------------------------------------------------------------------------
// Ingest loop
// ---------------------------------------------------------------------------

std::size_t NetworkStreamFrontEnd::drainLane(Lane& lane,
                                             std::chrono::steady_clock::time_point now) {
    const auto& ns = config_.getNetworkConfig().streaming;
    auto log = log_manager_.getLogger("main_log");
    std::vector<std::uint8_t> chunk;
    std::size_t drained = 0;
    // Bounded drain (the pumpFrames doctrine): consume what is buffered, cap
    // the burst so one lane never starves the sweep.  Timeout 0 — the sweep
    // must not block on any single lane.
    while (drained < kMaxDrainPerLane) {
        const std::size_t n = lane.source.read(chunk, kReadChunk, /*timeout_ms=*/0);
        if (n == 0) break;
        drained += n;
        lane.framer.feed(chunk);
        while (auto frame = lane.framer.nextFrame()) {
            ++lane.stats.frames;
            // Persistent per-lane parser: outputs are per-frame (clear), the
            // stream-time tracker survives per lane (RTB-1(a), per stream).
            lane.parser.clear();
            lane.parser.parseFrame(*frame);

            // Observations → identity tag → LLI → per-epoch batches → aligner.
            if (!lane.parser.getObservations().empty()) {
                auto batches = tagAndGroup(
                    std::vector<RinexObservation>(lane.parser.getObservations()),
                    lane.station,
                    ns.msm_lli_from_locktime ? &lock_time_tracker_ : nullptr,
                    ns.msm_lli_from_locktime);
                for (auto& [t, b] : batches) {
                    lane.stats.obs += b.size();
                    aligner_->add(lane.station, t, std::move(b), now);
                }
            }

            // Broadcast ephemeris → the mutex-guarded live store (the same
            // call the single-rover RTCM path makes; provenance class:
            // network-side stream).
            if (!lane.parser.getBroadcastEphemeris().empty()) {
                auto& eph_store = LiveEphemerisStore::getInstance();
                for (const auto& eph : lane.parser.getBroadcastEphemeris())
                    eph_store.insert(eph);
                lane.stats.nav_records += lane.parser.getBroadcastEphemeris().size();
            }

            // SSR content on an obs lane: counted and DROPPED — the producer
            // estimates its own corrections; a foreign SSR stream here would
            // be circular.  One-shot log so the drop is never silent.
            const std::size_t n_ssr =
                lane.parser.getSsrOrbits().size() + lane.parser.getSsrClocks().size()
                + lane.parser.getSsrCombined().size()
                + lane.parser.getSsrCodeBiases().size()
                + lane.parser.getSsrPhaseBiases().size()
                + lane.parser.getSsrUras().size()
                + lane.parser.getSsrHighRateClocks().size();
            if (n_ssr > 0) {
                lane.stats.ssr_records_dropped += n_ssr;
                if (!lane.ssr_drop_logged) {
                    lane.ssr_drop_logged = true;
                    log->info("Network (RTF-3): lane '{}' carries SSR content — {} "
                              "record(s) DROPPED (the producer estimates its own "
                              "corrections; this drop is by design and counted).",
                              lane.station, n_ssr);
                }
            }
        }
    }
    lane.stats.bytes_discarded = lane.framer.bytesDiscarded();
    return drained;
}

void NetworkStreamFrontEnd::ingestLoop() {
    auto log = log_manager_.getLogger("main_log");
    const auto& ns = config_.getNetworkConfig().streaming;
    auto last_heartbeat = std::chrono::steady_clock::now();

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        std::size_t swept = 0;
        bool any_active = false;
        for (auto& lane : lanes_) {
            if (stop_requested_.load()) break;
            const bool live = lane->source.active();
            lane->stats.active = live;
            if (live) {
                any_active = true;
                swept += drainLane(*lane, now);
            }
        }
        // Aligned epochs → the bounded solve queue (whole-epoch drop-oldest).
        for (auto& se : aligner_->collectReady(std::chrono::steady_clock::now()))
            queue_.push(std::move(se));

        if (stop_requested_.load()) break;
        if (!any_active) {
            // Stream end (file replay exhausted / all transports stopped):
            // drain the aligner's tail regardless of deadlines, then close.
            for (auto& se : aligner_->flush()) queue_.push(std::move(se));
            break;
        }

        // Heartbeat: the operator's staleness surface for a silent session.
        if (ns.log_interval_s > 0.0
            && std::chrono::duration<double>(now - last_heartbeat).count()
                   >= ns.log_interval_s) {
            last_heartbeat = now;
            const auto& c = aligner_->counters();
            std::size_t n_live = 0;
            for (const auto& lane : lanes_) if (lane->stats.active) ++n_live;
            log->info("Network (RTF-3): heartbeat — {}/{} lane(s) live, {} epoch(s) "
                      "dispatched ({} full / {} partial), drops: {} min-sta, {} "
                      "no-datum, {} late, {} stale-bucket, {} queue; {} pending, "
                      "queue depth {}.",
                      n_live, lanes_.size(), c.epochs_dispatched, c.dispatched_full,
                      c.dispatched_partial, c.dropped_min_stations, c.dropped_no_datum,
                      c.late_arrivals_dropped, c.stale_buckets_dropped,
                      queue_.dropped(), aligner_->pendingBuckets(), queue_.size());
        }

        if (swept == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMs));
    }
    queue_.close();
}

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------

const NetworkStreamAligner::Counters& NetworkStreamFrontEnd::alignerCounters() const {
    static const NetworkStreamAligner::Counters kEmpty{};
    return aligner_ ? aligner_->counters() : kEmpty;
}

std::vector<NetworkStreamFrontEnd::LaneStats> NetworkStreamFrontEnd::laneStats() const {
    std::vector<LaneStats> out;
    out.reserve(lanes_.size());
    for (const auto& l : lanes_) out.push_back(l->stats);
    return out;
}

void NetworkStreamFrontEnd::logRunEndSummary(
    const std::shared_ptr<spdlog::logger>& log) const {
    const auto& c = alignerCounters();
    log->info("Network (RTF-3) run-end: {} epoch(s) dispatched ({} full / {} "
              "partial); drops: {} min-stations, {} no-datum, {} late arrivals, "
              "{} stale buckets, {} queue overflow.",
              c.epochs_dispatched, c.dispatched_full, c.dispatched_partial,
              c.dropped_min_stations, c.dropped_no_datum, c.late_arrivals_dropped,
              c.stale_buckets_dropped, queue_.dropped());
    for (const auto& s : laneStats())
        log->info("Network (RTF-3) run-end: lane '{}' — {} frame(s), {} obs, {} nav, "
                  "{} SSR dropped, {} byte(s) framer-discarded{}.",
                  s.station, s.frames, s.obs, s.nav_records, s.ssr_records_dropped,
                  s.bytes_discarded, s.active ? "" : " (ended)");
}

} // namespace GnssProcessor
