/**
 * @file NetworkStreamFrontEnd.hpp
 * @brief Multi-station streaming front-end for the PPP-RTK network producer
 *        (@c network.streaming, RTF-3).
 *
 * Owns one decode lane per roster station that carries a
 * @c StationTransportConfig (transport: NTRIP, TCP or RTCM3 dump replay; an
 * @c RtcmStreamReader framer; and a per-lane @c RtcmParser that persists for
 * the run), plus one ingest thread.  The ingest thread drains every lane,
 * stamps the station identity on each decoded observation at the framer
 * boundary (@c RinexObservation::receiver_id = the roster station name, the
 * by-station map key downstream), and feeds the wait-budget
 * @c NetworkStreamAligner.  Aligned epochs reach the solve thread through the
 * bounded @c SyncedEpochQueue, whose backpressure drops whole epochs, oldest
 * first.  Bytes are never dropped, so framer and decoder state stay coherent
 * under any solve lag.
 *
 * Station identity: the MSM decoder has no station concept, so N streams
 * decoded through one parser would merge into one station.  Here every frame
 * is decoded by its own lane's parser and its observations are tagged with that
 * lane's station name before any other code sees them; identity is attached at
 * the earliest point and never inferred downstream.  Because each lane keeps
 * its own parser, each stream also has its own RTCM time tracker, so mod-week
 * promotion stays continuous per stream (RTB-1(a), applied per lane).
 *
 * Auxiliary content on an observation lane: broadcast ephemeris (1019/1020/1042/
 * 1044/1046) is pushed into the mutex-guarded @c LiveEphemerisStore in the same
 * way as on the single-rover RTCM path.  SSR records are counted and dropped,
 * with one log line per lane: the producer estimates its own corrections, and
 * consuming a foreign SSR stream here would be circular.  MSM lock-time → LLI
 * derivation (RTB-3) uses one station-keyed @c LockTimeTracker shared by all
 * lanes, keyed by station name as that class is designed for.
 *
 * v1 constraints (deliberate decisions):
 *  - Station construction still reads each roster station's @c rinex_path for
 *    header metadata (antenna type, GLONASS FCN slots).  A header-free
 *    StationMetadata factory is a follow-up increment of the RTF-3 card.
 *  - No signal handling here: a session ends at @c processing.max_duration_s
 *    (content time, checked in the epoch body) or at stream end (file lanes).
 *    Live service supervision follows the restart procedure in
 *    docs/rt_service_operations.md.
 *  - GLONASS FCN slots are not refreshed live from 1020 (the network GLONASS
 *    path is blocked independently); slots still come from the RINEX header.
 *
 * Threading: one ingest thread services every lane.  Network transports buffer
 * on their own receive threads, so for them the sweep only drains buffers and
 * decodes; file lanes are read directly by the sweep.  The aligner is local to
 * the ingest thread; the queue is the only cross-thread object.  @c stop() is
 * idempotent and joins the ingest thread before returning.
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/driver/NetworkStreamAligner.hpp"
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
    /// Per-lane run statistics (run-end telemetry; invariant #4: these counters,
    /// not the absence of crashes, show that the streaming path did work).
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
     * transported).  A lane that fails to open is logged and skipped, and the
     * remaining lanes proceed (degraded operation is preferred to none); if no
     * lane opens, the start fails.  A station without a transport descriptor
     * is logged and absent from every streamed epoch.
     *
     * @return True when at least one lane is live.
     */
    bool start();

    /**
     * @brief Pull the next aligned epoch for the solve loop.
     *
     * Blocks in bounded slices until an epoch is available or the front-end is
     * terminal (every lane ended, aligner flushed, queue drained); the false
     * return ends the streaming pump.  A live NTRIP lane counts as running
     * through transient disconnects (its client reconnects automatically), so a
     * session that stays connected but silent blocks here indefinitely by
     * design.  The heartbeat log is where an operator sees the staleness, and
     * @c processing.max_duration_s is the supported session bound.
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
     * the result into per-time batches (grouping is millisecond-exact: one MSM
     * frame carries one constellation at one tow, but a drain may span an
     * epoch boundary).  Static and transport-free, so the identity, LLI and
     * grouping contract is unit-testable without sockets.
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
    /// A deliberate small duplicate of the file-local @c RealtimeStream in
    /// ApplicationRealtime.cpp.  That class is private to the single-rover
    /// driver TU, and moving it out would change a validated RT path.
    /// Consolidating the two into a shared realtime/ transport seam is open
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

    /// Open one lane's transport from its descriptor.  Returns false on failure
    /// (logged); the caller then skips the lane.
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
