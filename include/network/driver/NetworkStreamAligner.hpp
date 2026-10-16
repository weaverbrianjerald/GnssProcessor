/**
 * @file NetworkStreamAligner.hpp
 * @brief Wait-budget epoch aligner and bounded epoch queue for the multi-station
 *        streaming front-end (@c network.streaming, RTF-3).
 *
 * Two pure building blocks, free of transport and threads so that the dispatch
 * semantics can be unit-tested with injected clocks:
 *
 *  - @ref NetworkStreamAligner collects per-station observation batches into
 *    per-epoch buckets and dispatches each bucket as a @c SyncedEpoch (the same
 *    record the batch @c MultiRinexSync produces, so the network epoch-loop body
 *    consumes streamed epochs unchanged).  Dispatch follows the RTF-3
 *    wait-budget design: an epoch leaves at T_first_arrival + @c wait_budget_s
 *    with the stations that have arrived by then.  There is no early dispatch
 *    when every expected lane has reported (see @ref collectReady).  Stations
 *    that miss the window are absent from @c by_station, which downstream code
 *    treats as normal; their late batches are dropped and counted, never merged
 *    into the dispatched epoch.
 *
 *  - @ref SyncedEpochQueue is the ingest→solve hand-off with epoch-granular
 *    backpressure: a bounded deque of whole @c SyncedEpoch records whose
 *    overflow policy drops the oldest queued epoch.  No part of the front-end
 *    drops bytes, because a byte-granular drop-oldest policy desynchronizes the
 *    RTCM framer.  When the solve cannot keep up, whole coherent epochs age out,
 *    which is the same skip-to-latest rule the single-rover engine uses.
 *
 * Threading contract: the aligner is single-writer (the ingest thread); the
 * queue is the only cross-thread object (internally locked, one producer and
 * one consumer).
 */
#pragma once

#include "rinex/MultiRinexSync.hpp"   // SyncedEpoch (the batch-identical epoch record)

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Wait-budget aligner: per-station arrival batches → dispatched
 *        @c SyncedEpoch records.
 *
 * Buckets are keyed by the observation time tag quantised to the nominal
 * cadence (@c Config::epoch_interval_s), so v1 assumes a uniform cadence
 * across streams (the IGS MSM standard).  Buckets dispatch strictly in time
 * order: the head bucket leaves when its deadline (first arrival + wait
 * budget) has passed, and a younger bucket never overtakes an older one still
 * inside its budget, so the emitted epoch sequence is monotonic by
 * construction.  A dispatched, gate-dropped or evicted key advances the
 * monotonic floor, and any later arrival for it is counted late and discarded.
 */
class NetworkStreamAligner {
public:
    /// Dispatch-policy knobs.  The caller fills them from @c NetworkStreamingConfig;
    /// @c datum_station comes from the network clock-datum station.
    struct Config {
        double      wait_budget_s{ 0.7 };     ///< First arrival → dispatch budget (s).
        int         min_stations{ 2 };        ///< Below this at dispatch => drop the epoch.
        bool        require_datum{ true };    ///< Drop epochs missing @c datum_station.
        std::string datum_station;            ///< Clock-datum station name ("" = no datum gate).
        double      epoch_interval_s{ 1.0 };  ///< Nominal cadence (s) for bucket quantisation (<= 0 becomes 1.0).
        int         max_pending_epochs{ 16 }; ///< Pending-bucket cap (minimum 2); overflow drops the oldest.
    };

    /// Run counters, read at teardown for the RTF-3 run-end summary (the ON-path
    /// telemetry; "0 dispatched" indicates the wiring is inert).
    struct Counters {
        std::uint64_t station_epochs_in{ 0 };      ///< add() batches accepted into buckets.
        std::uint64_t station_batch_merges{ 0 };   ///< add() batches appended to an existing station slot (multi-frame epochs).
        std::uint64_t late_arrivals_dropped{ 0 };  ///< add() batches at or below the dispatched floor.
        std::uint64_t epochs_dispatched{ 0 };      ///< SyncedEpochs emitted.
        std::uint64_t dispatched_full{ 0 };        ///< ... with every expected lane present.
        std::uint64_t dispatched_partial{ 0 };     ///< ... dispatched on deadline with a subset.
        std::uint64_t dropped_min_stations{ 0 };   ///< Buckets dropped below the station floor.
        std::uint64_t dropped_no_datum{ 0 };       ///< Buckets dropped for a missing datum station.
        std::uint64_t stale_buckets_dropped{ 0 };  ///< Oldest buckets evicted by the pending cap.
    };

    /**
     * @brief Construct the aligner.
     * @param cfg                Dispatch policy.
     * @param n_expected_lanes   Streams the front-end actually runs, not the
     *                           roster size (0 is treated as 1).  An epoch
     *                           counts as "full" (@c dispatched_full) when all
     *                           of these reported, which stays meaningful when
     *                           only a subset of the roster carries transports.
     */
    NetworkStreamAligner(const Config& cfg, std::size_t n_expected_lanes);

    /**
     * @brief Add one station's observation batch for one epoch.
     *
     * All observations in @p obs must share @p t (the caller groups by time
     * tag when a drained frame set spans an epoch boundary).  @p now is the
     * arrival instant, injected so tests never sleep.  An empty batch is
     * ignored.  A batch for an epoch at or below the monotonic dispatch floor
     * is counted late and discarded.  A second batch from the same station for
     * the same bucket is appended (merged): an MSM epoch spans several frames
     * (one per constellation, same tow), so replace semantics would discard
     * every constellation but the last.
     */
    void add(const std::string& station, const GnssTime& t,
             std::vector<RinexObservation>&& obs,
             std::chrono::steady_clock::time_point now);

    /**
     * @brief Dispatch every bucket whose deadline has passed at @p now.
     *
     * Ordered head-of-line walk: the oldest bucket dispatches once its wait
     * budget expires; the walk stops at the first still-waiting bucket, so
     * later epochs cannot overtake it.  There is intentionally no early
     * "every lane reported" path: a lane counts as present after its first
     * frame for the epoch, while its remaining constellation frames may still
     * be in flight, so dispatching on presence would split epochs at the
     * multi-constellation boundary.  The budget is the dispatch time (the
     * card's "T + 0.3-1 s with whoever arrived").  Gate-dropped buckets
     * (station floor or datum) still advance the monotonic floor.
     *
     * @return Ready epochs in strictly increasing time order (possibly empty).
     */
    std::vector<SyncedEpoch> collectReady(std::chrono::steady_clock::time_point now);

    /**
     * @brief Stream-end drain: dispatch every pending bucket regardless of
     *        deadline (still subject to the station-floor and datum gates).
     */
    std::vector<SyncedEpoch> flush();

    /// Run counters (see @ref Counters).
    const Counters& counters() const { return c_; }

    /// Pending (not yet dispatched) bucket count; the heartbeat gauge.
    std::size_t pendingBuckets() const { return pending_.size(); }

private:
    /// One assembling epoch: representative time, per-station batches, deadline.
    struct Bucket {
        GnssTime time{};   ///< Representative tag: the datum station's when it arrives (the
                           ///< batch schedule walks the reference), else the first arrival's.
        std::map<std::string, std::vector<RinexObservation>> by_station;
        std::chrono::steady_clock::time_point deadline{};
        bool datum_time_locked{ false };   ///< time came from the datum station.
    };

    /// Quantise a time tag to its epoch-bucket key (continuous GPS seconds /
    /// cadence, rounded); safe across week rollover by construction.
    std::int64_t keyOf(const GnssTime& t) const;

    /// Apply the dispatch gates and convert a bucket into a SyncedEpoch
    /// (nullopt = gate-dropped, counters advanced).
    std::optional<SyncedEpoch> finalize(Bucket&& b, bool full);

    Config      cfg_;
    std::size_t n_expected_;
    std::map<std::int64_t, Bucket> pending_;   ///< key → bucket, time-ordered.
    std::int64_t last_dispatched_key_{ std::numeric_limits<std::int64_t>::min() };
    Counters    c_;
};

/**
 * @brief Bounded ingest→solve epoch queue with drop-oldest overflow.
 *
 * One producer (ingest thread), one consumer (the solve loop).  @ref push
 * never blocks: at depth, the oldest queued epoch is discarded to make room.
 * This is whole-epoch backpressure: the solve always works on the freshest
 * coherent epochs, and the discard count is the lag telemetry.  @ref close
 * wakes the consumer, which drains whatever remains and then receives false.
 */
class SyncedEpochQueue {
public:
    explicit SyncedEpochQueue(std::size_t depth) : depth_(depth ? depth : 1) {}

    /**
     * @brief Enqueue one epoch; drop the oldest on overflow.  A push after
     *        @ref close is discarded without counting.
     * @return Number of epochs discarded to make room (0 or 1).
     */
    std::size_t push(SyncedEpoch&& se);

    /**
     * @brief Dequeue the next epoch, waiting up to @p timeout_ms.
     * @return True with @p out filled; false on timeout, or once the queue is
     *         closed and drained (the terminal condition; callers distinguish
     *         the two through @ref closed).
     */
    bool pop(SyncedEpoch& out, int timeout_ms);

    /// Producer-side terminal signal: no further pushes will arrive.
    void close();

    /// True once close() has been called (pending epochs may still drain).
    bool closed() const;

    /// Current queue depth (heartbeat gauge).
    std::size_t size() const;

    /// Total epochs discarded by the overflow policy (run-end telemetry).
    std::uint64_t dropped() const;

private:
    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::deque<SyncedEpoch> q_;
    std::size_t             depth_;
    std::uint64_t           dropped_{ 0 };
    bool                    closed_{ false };
};

} // namespace GnssProcessor
