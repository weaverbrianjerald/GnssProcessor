/**
 * @file NetworkStreamFrontEnd.hpp
 * @brief RTF-3: multi-station streaming front-end for the PPP-RTK NETWORK
 *        producer (@c network.streaming).
 *
 * Owns one decode LANE per roster station that carries a
 * @c StationTransportConfig — transport (NTRIP / TCP / RTCM3 dump replay) +
 * @c RtcmStreamReader framer + a PERSISTENT per-lane @c RtcmParser — and an
 * ingest thread that drains every lane, stamps STATION IDENTITY on each
 * decoded observation at the framer boundary (@c RinexObservation::receiver_id
 * = the roster station name; the by-station map key downstream), and feeds the
 * wait-budget @c NetworkStreamAligner.  Aligned epochs cross to the solve
 * thread through the bounded @c SyncedEpochQueue (EPOCH-granular drop-oldest
 * backpressure — bytes are never dropped, so framer/decoder state stays
 * coherent under any solve lag).
 *
 * Identity discipline (the defect this class exists to fix): the MSM decoder
 * itself has no station concept, so N streams through one parser used to merge
 * into ONE phantom station.  Here every frame is decoded by its OWN lane's
 * parser and its observations are tagged with that lane's station name before
 * anything else sees them — identity attaches at the earliest possible point
 * and is never inferred downstream.  Per-lane parser persistence also gives
 * each stream its own RTCM time tracker (mod-week promotion continuity per
 * stream — the RTB-1(a) lesson, per lane).
 *
 * Auxiliary content on an obs lane: broadcast ephemeris (1019/1020/1042/
 * 1044/1046) is pushed into the mutex-guarded @c LiveEphemerisStore exactly as
 * the single-rover RTCM path does; SSR records are counted and DROPPED (the
 * producer estimates its own corrections — consuming a foreign SSR stream
 * here would be circular), with a one-shot log per lane.  MSM lock-time → LLI
 * derivation (RTB-3) reuses one station-keyed @c LockTimeTracker across all
 * lanes, keyed by station name as that class was designed for.
 *
 * v1 constraints (documented decisions, not oversights):
 *  - Station CONSTRUCTION still reads each roster station's @c rinex_path for
 *    header metadata (antenna type, GLONASS FCN slots) — the header-free
 *    StationMetadata factory is a follow-up increment of the RTF-3 card.
 *  - No signal handling here: session bounds come from
 *    @c processing.max_duration_s (content time, checked in the epoch body)
 *    or stream end (file lanes).  Live service supervision is the
 *    docs/rt_service_operations.md restart doctrine.
 *  - GLONASS FCN slots are NOT live-refreshed from 1020 (the network GLONASS
 *    path is blocked independently; slots still come from the RINEX header).
 *
 * Threading: ONE ingest thread services every lane (the transports buffer
 * internally on their own receive threads; the lane sweep only drains ring
 * buffers and decodes).  The aligner is ingest-thread-local; the queue is the
 * only cross-thread object.  @c stop() is idempotent and joins the ingest
 * thread before returning.
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "realtime/NetworkStreamAligner.hpp"
#include "rtcm/LockTimeTracker.hpp"
#include "rtcm/RtcmParser.hpp"
#include "realtime/RtcmStreamReader.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace GnssProcessor {

class NtripClient;
class TcpClient;

/**
 * @brief Multi-station streaming epoch source for @c Application::runNetwork.
 *
 * Lifecycle: construct → @ref start → the solve loop pulls with @ref next
 * until it returns false (all lanes ended and every aligned epoch drained) →
 * @ref stop (idempotent; also run by the destructor) → @ref logRunEndSummary.
 */
class NetworkStreamFrontEnd {
public:
    /// Per-lane run statistics (run-end telemetry; invariant #4 — counters,
    /// not absence-of-crashes, are the ON-path evidence).
    struct LaneStats {
        std::string   station;
        std::uint64_t frames{ 0 };            ///< Complete RTCM frames decoded.
        std::uint64_t obs{ 0 };               ///< Observations tagged + delivered to the aligner.
        std::uint64_t nav_records{ 0 };       ///< Broadcast-eph records pushed to LiveEphemerisStore.
        std::uint64_t ssr_records_dropped{ 0 };///< SSR content counted-and-dropped (producer estimates its own).
        std::uint64_t bytes_discarded{ 0 };   ///< Framer preamble-scan discards (link noise gauge).
        bool          active{ false };        ///< Lane transport still live at last sweep.
    };

    NetworkStreamFrontEnd(const GnssConfig& config, LogManager& log_manager);
    ~NetworkStreamFrontEnd();

    NetworkStreamFrontEnd(const NetworkStreamFrontEnd&)            = delete;
    NetworkStreamFrontEnd& operator=(const NetworkStreamFrontEnd&) = delete;

    /**
     * @brief Open every configured lane and start the ingest thread.
     *
     * Builds one lane per roster station whose @c StationTransportConfig is
     * configured (the closed-loop consumer station included, when armed and
     * transported).  A lane that fails to OPEN is logged and skipped — the
     * remaining lanes proceed (prefer-degraded doctrine); zero opened lanes
     * fails the start.
     *
     * @return True when at least one lane is live.
     */
    bool start();

    /**
     * @brief Pull the next aligned epoch for the solve loop.
     *
     * Blocks in bounded slices until an epoch is available or the front-end is
     * TERMINAL (every lane ended, aligner flushed, queue drained) — the false
     * return that ends the streaming pump.  A live NTRIP lane counts as
     * "running" through transient disconnects (its client auto-reconnects), so
     * a silent-but-connected session blocks here indefinitely by design; the
     * heartbeat log is the operator's staleness surface and
     * @c processing.max_duration_s the sanctioned session bound.
     */
    bool next(SyncedEpoch& out);

    /// Stop transports, join the ingest thread, close the queue.  Idempotent.
    void stop();

    /// Run-end one-liner block: per-lane stats + aligner counters + queue drops.
    void logRunEndSummary(const std::shared_ptr<spdlog::logger>& log) const;

    /// Aligner counters (teardown + tests).
    const NetworkStreamAligner::Counters& alignerCounters() const;

    /// Per-lane stats snapshot (teardown + tests).
    std::vector<LaneStats> laneStats() const;

    /// Epochs discarded by solve-queue backpressure (teardown).
    std::uint64_t queueDropped() const { return queue_.dropped(); }

    /**
     * @brief Pure decode-tail helper: tag station identity + derive LLI +
     *        group by epoch.
     *
     * Takes the observations one drained frame produced, stamps
     * @c receiver_id = @p station on every record (identity at the framer
     * boundary), optionally derives the LLI 0x1 bit from MSM lock-time
     * regressions via the shared station-keyed @p tracker (RTB-3), and groups
     * the result into per-time batches (millisecond-exact grouping — one MSM
     * frame carries one constellation at one tow, but a drain may span an
     * epoch boundary).  Static + transport-free so the identity/LLI/grouping
     * contract is unit-testable without sockets.
     *
     * @param obs                Decoded observations (consumed).
     * @param station            Roster station name of the lane.
     * @param tracker            Shared lock-time tracker (nullptr = LLI derivation off).
     * @param lli_from_locktime  Arm the RTB-3 LLI derivation.
     * @return Per-epoch batches in first-seen order: (time, observations).
     */
    static std::vector<std::pair<GnssTime, std::vector<RinexObservation>>>
    tagAndGroup(std::vector<RinexObservation> obs, const std::string& station,
                LockTimeTracker* tracker, bool lli_from_locktime);

private:
    /// Minimal per-lane byte source (NTRIP / TCP / RTCM3 dump file).
    ///
    /// Deliberate small duplication of the file-local @c RealtimeStream in
    /// ApplicationRealtime.cpp: that class is private to the single-rover
    /// driver TU, and hoisting it mid-card would churn a validated RT path.
    /// Consolidating the two into a shared realtime/ transport seam is flagged
    /// follow-up work.
    struct LaneByteSource {
        enum class Kind { Ntrip, Tcp, File };
        Kind                          kind{ Kind::File };
        std::unique_ptr<NtripClient>  ntrip;
        std::unique_ptr<TcpClient>    tcp;
        std::ifstream                 file;
        bool                          file_eof{ false };

        bool        active() const;
        std::size_t read(std::vector<std::uint8_t>& out, std::size_t max_bytes,
                         int timeout_ms);
        void        stop();
    };

    /// One station's decode lane: transport + framer + persistent parser.
    struct Lane {
        std::string      station;
        LaneByteSource   source;
        RtcmStreamReader framer;
        RtcmParser       parser;      ///< Persistent per lane (per-stream time continuity).
        LaneStats        stats;
        bool             ssr_drop_logged{ false };
    };

    /// Open one lane's transport from its descriptor (nullptr-equivalent on
    /// failure: returns false and the lane is skipped).
    bool openLane(Lane& lane, const StationTransportConfig& tc);

    /// Ingest thread body: sweep lanes → frames → decode → tag → aligner →
    /// queue; heartbeat; terminal flush + queue close.
    void ingestLoop();

    /// Drain one lane's buffered bytes through framer + parser into the
    /// aligner (bounded per sweep).  Returns bytes consumed.
    std::size_t drainLane(Lane& lane, std::chrono::steady_clock::time_point now);

    const GnssConfig& config_;
    LogManager&       log_manager_;

    std::vector<std::unique_ptr<Lane>> lanes_;
    LockTimeTracker                    lock_time_tracker_;   ///< Shared, station-keyed (RTB-3).
    std::unique_ptr<NetworkStreamAligner> aligner_;          ///< Ingest-thread-local after start().
    SyncedEpochQueue                   queue_;

    std::thread        ingest_thread_;
    std::atomic<bool>  stop_requested_{ false };
    std::atomic<bool>  started_{ false };

    // Ingest-loop pacing + drain bounds (per sweep, per lane).
    static constexpr std::size_t kReadChunk        = 4096;
    static constexpr std::size_t kMaxDrainPerLane  = 256 * 1024;
    static constexpr int         kIdleSleepMs      = 5;
    static constexpr int         kNextPollSliceMs  = 250;
};

} // namespace GnssProcessor
