/**
 * @file test_network_streaming.cpp
 * @brief RTF-3 multi-station streaming front-end: wait-budget aligner dispatch
 *        semantics, epoch-granular queue backpressure, framer-boundary station
 *        identity (with the RTB-3 LLI derivation), and the network.streaming
 *        config contract.
 *
 * The aligner and queue take the clock as an argument and have no transport, so
 * every dispatch rule is exercised here without sockets or sleeps.  The decode
 * tail is covered through NetworkStreamFrontEnd::tagAndGroup (static, pure), the
 * code the ingest loop runs between the per-lane parser and the aligner.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/driver/StreamAligner.hpp"
#include "network/driver/StreamFrontEnd.hpp"
#include "rtcm/LockTimeTracker.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;
using steady = std::chrono::steady_clock;
using ms     = std::chrono::milliseconds;

namespace {

RinexObservation makeObs(const std::string& sat, int week, double tow) {
    RinexObservation o;
    o.satellite = sat;
    o.time      = GnssTime{ week, tow };
    return o;
}

std::vector<RinexObservation> oneObs(const std::string& sat, int week, double tow) {
    std::vector<RinexObservation> v;
    v.push_back(makeObs(sat, week, tow));
    return v;
}

NetworkStreamAligner::Config alignerCfg(double budget_s = 0.5, int min_sta = 1,
                                        bool require_datum = false,
                                        const std::string& datum = {}) {
    NetworkStreamAligner::Config c;
    c.wait_budget_s      = budget_s;
    c.min_stations       = min_sta;
    c.require_datum      = require_datum;
    c.datum_station      = datum;
    c.epoch_interval_s   = 1.0;
    c.max_pending_epochs = 16;
    return c;
}

SyncedEpoch makeEpoch(double tow) {
    SyncedEpoch se;
    se.time = GnssTime{ 2312, tow };
    se.by_station["X"] = {};
    return se;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p =
        fs::temp_directory_path() / ("net_streaming_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// Minimal loadable ppp-rtk-network config; @p streaming_extra splices into the
// network block, @p transport_a / @p transport_b into the two station objects
// (empty = no transport key, the batch-config norm).
std::string networkConfig(const std::string& streaming_extra,
                          const std::string& transport_a,
                          const std::string& transport_b) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + streaming_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ]"
        + (transport_a.empty() ? "" : (",\n        \"transport\": " + transport_a)) + " },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ]"
        + (transport_b.empty() ? "" : (",\n        \"transport\": " + transport_b)) + " }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

} // namespace

// ===========================================================================
// NetworkStreamAlignerTest: wait-budget dispatch semantics
// ===========================================================================

// An epoch dispatches at first arrival + wait budget, not before, with every
// station that reported by then in by_station.
TEST(NetworkStreamAlignerTest, DispatchesAtWaitBudgetWithArrivals) {
    NetworkStreamAligner a(alignerCfg(0.5), /*n_expected_lanes=*/3);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);
    a.add("BONN", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0 + ms(50));

    EXPECT_TRUE(a.collectReady(t0 + ms(100)).empty());   // inside the budget
    EXPECT_TRUE(a.collectReady(t0 + ms(499)).empty());

    auto ready = a.collectReady(t0 + ms(501));           // budget expired
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].by_station.size(), 2u);
    EXPECT_TRUE(ready[0].by_station.count("OSLO"));
    EXPECT_TRUE(ready[0].by_station.count("BONN"));
    EXPECT_EQ(a.counters().epochs_dispatched, 1u);
    EXPECT_EQ(a.counters().dispatched_partial, 1u);      // 2 of 3 expected lanes
    EXPECT_EQ(a.counters().dispatched_full, 0u);
}

// A second batch from the same station for the same epoch is merged, not
// replaced: an MSM epoch spans one frame per constellation.
TEST(NetworkStreamAlignerTest, MergesMultiFrameSameStationBatches) {
    NetworkStreamAligner a(alignerCfg(0.2), 2);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);   // GPS frame
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("E11", 2312, 100.0), t0);   // Galileo frame
    a.add("BONN", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);

    auto ready = a.collectReady(t0 + ms(250));
    ASSERT_EQ(ready.size(), 1u);
    ASSERT_TRUE(ready[0].by_station.count("OSLO"));
    EXPECT_EQ(ready[0].by_station.at("OSLO").size(), 2u);   // G01 + E11 both present
    EXPECT_EQ(a.counters().station_batch_merges, 1u);
    EXPECT_EQ(a.counters().dispatched_full, 1u);            // both expected lanes reported
}

// Below the station floor the epoch is dropped and counted, never dispatched.
TEST(NetworkStreamAlignerTest, DropsBelowMinStations) {
    NetworkStreamAligner a(alignerCfg(0.2, /*min_sta=*/2), 3);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);
    auto ready = a.collectReady(t0 + ms(250));
    EXPECT_TRUE(ready.empty());
    EXPECT_EQ(a.counters().dropped_min_stations, 1u);
    EXPECT_EQ(a.counters().epochs_dispatched, 0u);
}

// With the datum gate armed, an epoch missing the datum station drops; when
// the datum is present its time tag is the epoch's representative time.
TEST(NetworkStreamAlignerTest, DatumGateAndDatumTimeTag) {
    NetworkStreamAligner a(alignerCfg(0.2, 1, /*require_datum=*/true, "AAAA"), 3);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);
    EXPECT_TRUE(a.collectReady(t0 + ms(250)).empty());
    EXPECT_EQ(a.counters().dropped_no_datum, 1u);

    // Next epoch: rover arrives first with a slightly-early tag; the datum's
    // tag must win as the representative epoch time.
    a.add("OSLO", GnssTime{ 2312, 101.004 }, oneObs("G01", 2312, 101.004), t0 + ms(300));
    a.add("AAAA", GnssTime{ 2312, 101.0 },   oneObs("G01", 2312, 101.0),   t0 + ms(320));
    auto ready = a.collectReady(t0 + ms(600));
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_DOUBLE_EQ(ready[0].time.tow, 101.0);
    EXPECT_EQ(a.counters().epochs_dispatched, 1u);
}

// A batch for an already-dispatched epoch is dropped late, never retro-merged.
TEST(NetworkStreamAlignerTest, LateArrivalDropped) {
    NetworkStreamAligner a(alignerCfg(0.2), 2);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);
    ASSERT_EQ(a.collectReady(t0 + ms(250)).size(), 1u);

    a.add("BONN", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0 + ms(300));
    EXPECT_EQ(a.counters().late_arrivals_dropped, 1u);
    EXPECT_TRUE(a.collectReady(t0 + ms(600)).empty());
}

// Head-of-line discipline: a younger epoch whose budget expired never
// overtakes an older epoch still inside its budget; both emit in time order.
TEST(NetworkStreamAlignerTest, MonotonicHeadOfLine) {
    NetworkStreamAligner a(alignerCfg(0.5), 2);
    const auto t0 = steady::now();
    // The younger epoch (tow 101) arrives first (deadline t0+500ms), the older
    // epoch (tow 100) later (deadline t0+700ms).
    a.add("OSLO", GnssTime{ 2312, 101.0 }, oneObs("G01", 2312, 101.0), t0);
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0 + ms(200));

    // tow-101's budget has expired, but tow-100 (the head) has not: nothing
    // may dispatch, or the sequence would go 101 → 100.
    EXPECT_TRUE(a.collectReady(t0 + ms(600)).empty());

    auto ready = a.collectReady(t0 + ms(750));
    ASSERT_EQ(ready.size(), 2u);
    EXPECT_DOUBLE_EQ(ready[0].time.tow, 100.0);
    EXPECT_DOUBLE_EQ(ready[1].time.tow, 101.0);
}

// The pending cap evicts the oldest bucket, and its stragglers count as late.
TEST(NetworkStreamAlignerTest, StaleBucketEvictionAdvancesFloor) {
    auto cfg = alignerCfg(30.0);   // huge budget: nothing expires during the test
    cfg.max_pending_epochs = 2;
    NetworkStreamAligner a(cfg, 2);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);
    a.add("OSLO", GnssTime{ 2312, 101.0 }, oneObs("G01", 2312, 101.0), t0);
    a.add("OSLO", GnssTime{ 2312, 102.0 }, oneObs("G01", 2312, 102.0), t0);   // evicts tow-100

    EXPECT_EQ(a.counters().stale_buckets_dropped, 1u);
    EXPECT_EQ(a.pendingBuckets(), 2u);

    a.add("BONN", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);   // straggler for the evictee
    EXPECT_EQ(a.counters().late_arrivals_dropped, 1u);
}

// flush() drains the pending tail regardless of deadlines (stream end).
TEST(NetworkStreamAlignerTest, FlushDrainsTail) {
    NetworkStreamAligner a(alignerCfg(30.0), 1);
    const auto t0 = steady::now();
    a.add("OSLO", GnssTime{ 2312, 100.0 }, oneObs("G01", 2312, 100.0), t0);
    a.add("OSLO", GnssTime{ 2312, 101.0 }, oneObs("G01", 2312, 101.0), t0);
    auto ready = a.flush();
    ASSERT_EQ(ready.size(), 2u);
    EXPECT_DOUBLE_EQ(ready[0].time.tow, 100.0);
    EXPECT_DOUBLE_EQ(ready[1].time.tow, 101.0);
    EXPECT_EQ(a.pendingBuckets(), 0u);
}

// ===========================================================================
// SyncedEpochQueueTest: epoch-granular backpressure
// ===========================================================================

// Overflow drops the oldest whole epoch; the newest epochs are kept.
TEST(SyncedEpochQueueTest, DropOldestOnOverflow) {
    SyncedEpochQueue q(2);
    EXPECT_EQ(q.push(makeEpoch(100.0)), 0u);
    EXPECT_EQ(q.push(makeEpoch(101.0)), 0u);
    EXPECT_EQ(q.push(makeEpoch(102.0)), 1u);   // tow-100 aged out
    EXPECT_EQ(q.dropped(), 1u);

    SyncedEpoch se;
    ASSERT_TRUE(q.pop(se, 10));
    EXPECT_DOUBLE_EQ(se.time.tow, 101.0);
    ASSERT_TRUE(q.pop(se, 10));
    EXPECT_DOUBLE_EQ(se.time.tow, 102.0);
}

// pop() times out cleanly on an empty open queue (the pump's poll slice).
TEST(SyncedEpochQueueTest, PopTimesOutEmpty) {
    SyncedEpochQueue q(2);
    SyncedEpoch se;
    EXPECT_FALSE(q.pop(se, 10));
    EXPECT_FALSE(q.closed());
}

// close() lets the consumer drain what remains, then signals terminal.
TEST(SyncedEpochQueueTest, CloseDrainsThenTerminal) {
    SyncedEpochQueue q(4);
    q.push(makeEpoch(100.0));
    q.close();
    SyncedEpoch se;
    ASSERT_TRUE(q.pop(se, 10));                // the queued tail drains
    EXPECT_DOUBLE_EQ(se.time.tow, 100.0);
    EXPECT_FALSE(q.pop(se, 10));               // closed + drained = terminal
    EXPECT_TRUE(q.closed());
    EXPECT_EQ(q.push(makeEpoch(101.0)), 0u);   // post-close push is discarded
    EXPECT_FALSE(q.pop(se, 10));
}

// ===========================================================================
// NetworkStreamFrontEndDecodeTest: identity at the framer boundary
// ===========================================================================

// Every decoded observation is stamped with the lane's roster station name,
// and a drain spanning an epoch boundary groups into per-time batches.
TEST(NetworkStreamFrontEndDecodeTest, TagsIdentityAndGroupsEpochs) {
    std::vector<RinexObservation> obs;
    obs.push_back(makeObs("G01", 2312, 100.0));
    obs.push_back(makeObs("E11", 2312, 100.0));
    obs.push_back(makeObs("G01", 2312, 101.0));

    auto batches = NetworkStreamFrontEnd::tagAndGroup(std::move(obs), "OSLO",
                                                      nullptr, false);
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_DOUBLE_EQ(batches[0].first.tow, 100.0);
    EXPECT_EQ(batches[0].second.size(), 2u);
    EXPECT_DOUBLE_EQ(batches[1].first.tow, 101.0);
    EXPECT_EQ(batches[1].second.size(), 1u);
    for (const auto& [t, b] : batches)
        for (const auto& o : b) EXPECT_EQ(o.receiver_id, "OSLO");
}

// The LLI 0x1 bit is derived from a lock-time decrease (RTB-3 logic reused),
// tracked per (station, sat, signal), so two lanes' histories stay separate.
TEST(NetworkStreamFrontEndDecodeTest, DerivesLliPerStationHistories) {
    LockTimeTracker tracker;

    auto obsWithLock = [](double tow, double lock_ms) {
        auto o = makeObs("G01", 2312, tow);
        o.lock_time_ms["L1C"] = lock_ms;
        return std::vector<RinexObservation>{ o };
    };

    // OSLO epoch 1: new key, long lock time; no flag.
    auto b1 = NetworkStreamFrontEnd::tagAndGroup(obsWithLock(100.0, 5000.0),
                                                 "OSLO", &tracker, true);
    ASSERT_EQ(b1.size(), 1u);
    EXPECT_EQ(b1[0].second[0].lli_flags["L1C"] & 0x1, 0);

    // BONN epoch 1: same satellite, separate station key, also new; no flag.
    auto b2 = NetworkStreamFrontEnd::tagAndGroup(obsWithLock(100.0, 7000.0),
                                                 "BONN", &tracker, true);
    EXPECT_EQ(b2[0].second[0].lli_flags["L1C"] & 0x1, 0);

    // OSLO epoch 2: lock time decreased (5000 → 100 ms), so tracking restarted;
    // the LLI 0x1 bit is set, on OSLO's copy only.
    auto b3 = NetworkStreamFrontEnd::tagAndGroup(obsWithLock(101.0, 100.0),
                                                 "OSLO", &tracker, true);
    EXPECT_EQ(b3[0].second[0].lli_flags["L1C"] & 0x1, 1);

    // BONN epoch 2: lock time kept growing; no flag (histories are per station).
    auto b4 = NetworkStreamFrontEnd::tagAndGroup(obsWithLock(101.0, 8000.0),
                                                 "BONN", &tracker, true);
    EXPECT_EQ(b4[0].second[0].lli_flags["L1C"] & 0x1, 0);

    // Derivation disarmed: the same decrease leaves the flag untouched.
    LockTimeTracker fresh;
    auto c1 = NetworkStreamFrontEnd::tagAndGroup(obsWithLock(100.0, 5000.0),
                                                 "OSLO", &fresh, false);
    auto c2 = NetworkStreamFrontEnd::tagAndGroup(obsWithLock(101.0, 100.0),
                                                 "OSLO", &fresh, false);
    EXPECT_EQ(c2[0].second[0].lli_flags["L1C"] & 0x1, 0);
}

// ===========================================================================
// NetworkStreamingConfigTest: the network.streaming config contract
// ===========================================================================

// Absent block: streaming stays off and no station carries a transport, so the
// batch path's config surface is unchanged (structural byte-identity).
TEST(NetworkStreamingConfigTest, DefaultsOffWithNoTransports) {
    LogManager log;
    const fs::path p =
        writeTempConfig("defaults", networkConfig("", "", ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    const auto& nc = cfg.getNetworkConfig();
    EXPECT_FALSE(nc.streaming.enable);
    for (const auto& s : nc.stations) EXPECT_FALSE(s.transport.configured());
    fs::remove(p);
}

// Armed block: values parse, out-of-range values clamp with a warning, and the two
// station transports land on their stations.
TEST(NetworkStreamingConfigTest, ParsesTransportsAndClamps) {
    LogManager log;
    const std::string streaming =
        "    \"streaming\": {\n"
        "      \"enable\": true,\n"
        "      \"wait_budget_s\": 0.4,\n"
        "      \"min_stations\": 1,\n"
        "      \"queue_depth_epochs\": 0,\n"
        "      \"epoch_interval_s\": 30.0,\n"
        "      \"max_pending_epochs\": 1,\n"
        "      \"msm_lli_from_locktime\": true\n"
        "    },\n";
    const std::string ta =
        "{ \"input_type\": \"ntrip\", \"host\": \"igs-ip.net\", \"port\": 2101,\n"
        "          \"mountpoint\": \"AAAA00DEU0\", \"username\": \"u\", \"password\": \"p\" }";
    const std::string tb =
        "{ \"input_type\": \"file\", \"input_path\": \"C:/tmp/bbbb.rtcm3\" }";
    const fs::path p =
        writeTempConfig("parse", networkConfig(streaming, ta, tb));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    const auto& nc = cfg.getNetworkConfig();
    const auto& ns = nc.streaming;
    EXPECT_TRUE(ns.enable);
    EXPECT_DOUBLE_EQ(ns.wait_budget_s, 0.4);
    EXPECT_EQ(ns.min_stations, 2);          // clamped up from 1
    EXPECT_EQ(ns.queue_depth_epochs, 1);    // clamped up from 0
    EXPECT_DOUBLE_EQ(ns.epoch_interval_s, 30.0);
    EXPECT_EQ(ns.max_pending_epochs, 2);    // clamped up from 1
    EXPECT_TRUE(ns.msm_lli_from_locktime);

    ASSERT_EQ(nc.stations.size(), 2u);
    const auto& a = nc.stations[0].transport;
    EXPECT_TRUE(a.configured());
    EXPECT_EQ(a.input_type, "ntrip");
    EXPECT_EQ(a.host, "igs-ip.net");
    EXPECT_EQ(a.port, 2101);
    EXPECT_EQ(a.mountpoint, "AAAA00DEU0");
    const auto& b = nc.stations[1].transport;
    EXPECT_TRUE(b.configured());
    EXPECT_EQ(b.input_type, "file");
    EXPECT_EQ(b.input_path, "C:/tmp/bbbb.rtcm3");
    fs::remove(p);
}

// Inert-arming refusal (invariant #4): streaming armed with zero transports
// would run zero lanes and log cleanly, so the load fails instead of warning.
TEST(NetworkStreamingConfigTest, RefusesArmedWithZeroTransports) {
    LogManager log;
    const std::string streaming = "    \"streaming\": { \"enable\": true },\n";
    const fs::path p =
        writeTempConfig("refuse", networkConfig(streaming, "", ""));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    fs::remove(p);
}

// Off-path pass-through: an unarmed streaming block's out-of-range values are
// not clamped.  The clamps are gated on the master switch, so an off config
// round-trips verbatim (structural byte-identity for default-off work).
TEST(NetworkStreamingConfigTest, OffPathValuesPassThroughUnclamped) {
    LogManager log;
    const std::string streaming =
        "    \"streaming\": { \"enable\": false, \"min_stations\": 1, "
        "\"queue_depth_epochs\": 0 },\n";
    const fs::path p =
        writeTempConfig("offpath", networkConfig(streaming, "", ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().streaming.min_stations, 1);
    EXPECT_EQ(cfg.getNetworkConfig().streaming.queue_depth_epochs, 0);
    fs::remove(p);
}
