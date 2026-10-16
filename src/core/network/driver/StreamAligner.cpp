/**
 * @file StreamAligner.cpp
 * @brief Wait-budget aligner and bounded epoch queue, implementation (RTF-3).
 *
 * See the header for the dispatch semantics.  Everything here is clock-injected
 * and transport-free; the only time source is the caller's @c steady_clock
 * instants, so unit tests exercise the whole dispatch policy without sleeping.
 */
#include "network/driver/StreamAligner.hpp"

#include "math/TimeUtils.hpp"

#include <cmath>
#include <utility>

namespace GnssProcessor {

NetworkStreamAligner::NetworkStreamAligner(const Config& cfg,
                                           std::size_t n_expected_lanes)
    : cfg_(cfg), n_expected_(n_expected_lanes ? n_expected_lanes : 1) {
    if (!(cfg_.epoch_interval_s > 0.0)) cfg_.epoch_interval_s = 1.0;
    if (cfg_.max_pending_epochs < 2)    cfg_.max_pending_epochs = 2;
}

std::int64_t NetworkStreamAligner::keyOf(const GnssTime& t) const {
    return static_cast<std::int64_t>(
        std::llround(TimeUtils::continuousGpsSeconds(t) / cfg_.epoch_interval_s));
}

void NetworkStreamAligner::add(const std::string& station, const GnssTime& t,
                               std::vector<RinexObservation>&& obs,
                               std::chrono::steady_clock::time_point now) {
    if (obs.empty()) return;
    const std::int64_t key = keyOf(t);
    // Monotonic floor: this epoch already left (dispatched, gate-dropped or
    // evicted).  A late merge would hand the solve a second, conflicting
    // realisation of a consumed epoch, so the batch is counted and discarded.
    if (key <= last_dispatched_key_) {
        ++c_.late_arrivals_dropped;
        return;
    }
    auto it = pending_.find(key);
    if (it == pending_.end()) {
        Bucket b;
        b.time     = t;
        b.deadline = now + std::chrono::microseconds(
                         static_cast<std::int64_t>(cfg_.wait_budget_s * 1e6));
        it = pending_.emplace(key, std::move(b)).first;
        // Pending cap: evict the oldest bucket (stalest content, per the
        // skip-to-latest rule) and advance the floor so its stragglers count
        // late instead of re-creating the bucket.
        if (pending_.size() > static_cast<std::size_t>(cfg_.max_pending_epochs)) {
            auto oldest = pending_.begin();
            if (oldest->first != key) {   // never evict the bucket just minted
                last_dispatched_key_ = std::max(last_dispatched_key_, oldest->first);
                pending_.erase(oldest);
                ++c_.stale_buckets_dropped;
            }
        }
    }
    Bucket& b = it->second;
    // The datum station's tag is the epoch's representative time (the batch
    // synchroniser walks the reference station's schedule; keep that contract).
    if (!b.datum_time_locked && !cfg_.datum_station.empty()
        && station == cfg_.datum_station) {
        b.time = t;
        b.datum_time_locked = true;
    }
    auto [slot, inserted] = b.by_station.try_emplace(station);
    if (inserted) {
        slot->second = std::move(obs);
    } else {
        // Merge-append: an MSM epoch spans one frame per constellation at the
        // same tow, so later frames extend the station's batch.  Replace
        // semantics would drop every constellation but the last without notice
        // (the multi-GNSS analogue of the phantom-station defect).
        ++c_.station_batch_merges;
        slot->second.insert(slot->second.end(),
                            std::make_move_iterator(obs.begin()),
                            std::make_move_iterator(obs.end()));
    }
    ++c_.station_epochs_in;
}

std::optional<SyncedEpoch> NetworkStreamAligner::finalize(Bucket&& b, bool full) {
    if (b.by_station.size() < static_cast<std::size_t>(cfg_.min_stations)) {
        ++c_.dropped_min_stations;
        return std::nullopt;
    }
    if (cfg_.require_datum && !cfg_.datum_station.empty()
        && b.by_station.find(cfg_.datum_station) == b.by_station.end()) {
        ++c_.dropped_no_datum;
        return std::nullopt;
    }
    ++c_.epochs_dispatched;
    if (full) ++c_.dispatched_full; else ++c_.dispatched_partial;
    SyncedEpoch se;
    se.time       = b.time;
    se.by_station = std::move(b.by_station);
    return se;
}

std::vector<SyncedEpoch>
NetworkStreamAligner::collectReady(std::chrono::steady_clock::time_point now) {
    std::vector<SyncedEpoch> out;
    while (!pending_.empty()) {
        auto it = pending_.begin();
        // Deadline-only dispatch, with no "all lanes present" fast path (see the
        // header): presence is per frame, not per complete epoch, so an early
        // dispatch would split multi-constellation epochs.  Head-of-line
        // discipline keeps the emitted sequence monotonic without a reorder
        // buffer: the walk stops at the first still-waiting bucket.
        if (now < it->second.deadline) break;
        const bool full = it->second.by_station.size() >= n_expected_;
        Bucket taken = std::move(it->second);
        last_dispatched_key_ = it->first;
        pending_.erase(it);
        if (auto se = finalize(std::move(taken), full)) out.push_back(std::move(*se));
    }
    return out;
}

std::vector<SyncedEpoch> NetworkStreamAligner::flush() {
    std::vector<SyncedEpoch> out;
    while (!pending_.empty()) {
        auto it = pending_.begin();
        const bool full = it->second.by_station.size() >= n_expected_;
        Bucket taken = std::move(it->second);
        last_dispatched_key_ = it->first;
        pending_.erase(it);
        if (auto se = finalize(std::move(taken), full)) out.push_back(std::move(*se));
    }
    return out;
}

std::size_t SyncedEpochQueue::push(SyncedEpoch&& se) {
    std::size_t n_dropped = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return 0;   // producer raced a close: drop silently (terminal anyway)
        while (q_.size() >= depth_) {
            q_.pop_front();      // whole-epoch drop-oldest, never bytes
            ++dropped_;
            ++n_dropped;
        }
        q_.push_back(std::move(se));
    }
    cv_.notify_one();
    return n_dropped;
}

bool SyncedEpochQueue::pop(SyncedEpoch& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms),
                 [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;   // timeout, or closed-and-drained
    out = std::move(q_.front());
    q_.pop_front();
    return true;
}

void SyncedEpochQueue::close() {
    {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
    }
    cv_.notify_all();
}

bool SyncedEpochQueue::closed() const {
    std::lock_guard<std::mutex> lk(m_);
    return closed_;
}

std::size_t SyncedEpochQueue::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
}

std::uint64_t SyncedEpochQueue::dropped() const {
    std::lock_guard<std::mutex> lk(m_);
    return dropped_;
}

} // namespace GnssProcessor
