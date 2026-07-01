/**
 * @file test_network_product_io.cpp
 * @brief Unit tests for on-disk (de)serialization of the network product.
 *
 * The contract: a NetworkSolutionEpoch written to disk and read back is
 * bit-identical in every consumed field, and replaying the parsed records
 * through a fresh NetworkProductEmitter reproduces the SAME SsrCorrectionStore
 * the live producer builds — discontinuity counters included, even across a
 * datum change (the one piece of stream state the epoch struct does not carry,
 * serialized as the per-epoch datum_changed flag).  This is what lets the
 * consumer AR-frame work replay products without re-running the network.
 */

#include <gtest/gtest.h>

#include "products/NetworkProductIO.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <filesystem>

namespace GnssProcessor {
namespace {

const GnssTime kT0{2313, 259200.0};

// A two-satellite epoch: G01 (clock + C1W code bias + L1C/L2W phase bias) and
// G02 (clock + L1C phase bias).  pb01 sets G01's L1C bias so the caller can
// drive a discontinuity.
NetworkSolutionEpoch makeEpoch(const GnssTime& t, double pb01) {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = t;
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;

    NetworkSatSolution g1;
    g1.sat               = "G01";
    g1.has_clock         = true;
    g1.clock_c0_m        = 12.5;
    g1.code_bias_m["C1W"] = 0.3;
    g1.phase_bias["L1C"]  = NetworkPhaseBias{pb01, /*integer=*/true, /*wl=*/1};
    g1.phase_bias["L2W"]  = NetworkPhaseBias{1.234567890123, /*integer=*/false, /*wl=*/-2};
    sol.sats.push_back(g1);

    NetworkSatSolution g2;
    g2.sat               = "G02";
    g2.has_clock         = true;
    g2.clock_c0_m        = -8.75;
    g2.phase_bias["L1C"]  = NetworkPhaseBias{10.0, true, 1};
    sol.sats.push_back(g2);

    return sol;
}

std::string tempFile(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// ---------------------------------------------------------------------------

TEST(NetworkProductIoTest, RoundTripsEveryField) {
    const std::string path = tempFile("gnss_netprod_roundtrip.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch sol = makeEpoch(kT0, /*pb01=*/-0.7);

    // A third satellite with NO clock (has_clock=false) — exercises the
    // clock-absent path: it must round-trip with has_clock=false.
    NetworkSatSolution g3;
    g3.sat              = "G03";
    g3.has_clock        = false;
    g3.phase_bias["L1C"] = NetworkPhaseBias{3.5, true, 0};
    sol.sats.push_back(g3);

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(sol, /*datum_changed=*/true);
    }

    bool ok = false;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);

    const NetworkProductRecord& r = recs[0];
    EXPECT_TRUE(r.datum_changed);
    EXPECT_EQ(r.epoch.epoch_time.week, kT0.week);
    EXPECT_DOUBLE_EQ(r.epoch.epoch_time.tow, kT0.tow);
    EXPECT_EQ(r.epoch.iod_ssr, 3);
    EXPECT_EQ(r.epoch.provider_id, 42);
    EXPECT_EQ(r.epoch.solution_id, 1);
    ASSERT_EQ(r.epoch.sats.size(), 3u);

    // Satellites preserve first-seen order.
    EXPECT_EQ(r.epoch.sats[0].sat, "G01");
    EXPECT_EQ(r.epoch.sats[1].sat, "G02");
    EXPECT_EQ(r.epoch.sats[2].sat, "G03");

    const NetworkSatSolution& g1 = r.epoch.sats[0];
    EXPECT_TRUE(g1.has_clock);
    EXPECT_DOUBLE_EQ(g1.clock_c0_m, 12.5);
    ASSERT_EQ(g1.code_bias_m.count("C1W"), 1u);
    EXPECT_DOUBLE_EQ(g1.code_bias_m.at("C1W"), 0.3);
    ASSERT_EQ(g1.phase_bias.count("L1C"), 1u);
    EXPECT_DOUBLE_EQ(g1.phase_bias.at("L1C").bias_m, -0.7);
    EXPECT_TRUE(g1.phase_bias.at("L1C").signal_integer);
    EXPECT_EQ(g1.phase_bias.at("L1C").wide_lane_integer, 1);
    ASSERT_EQ(g1.phase_bias.count("L2W"), 1u);
    EXPECT_DOUBLE_EQ(g1.phase_bias.at("L2W").bias_m, 1.234567890123);  // exact round-trip
    EXPECT_FALSE(g1.phase_bias.at("L2W").signal_integer);
    EXPECT_EQ(g1.phase_bias.at("L2W").wide_lane_integer, -2);

    const NetworkSatSolution& g3_out = r.epoch.sats[2];
    EXPECT_FALSE(g3_out.has_clock);
    EXPECT_TRUE(g3_out.code_bias_m.empty());
    ASSERT_EQ(g3_out.phase_bias.count("L1C"), 1u);
    EXPECT_DOUBLE_EQ(g3_out.phase_bias.at("L1C").bias_m, 3.5);

    std::filesystem::remove(path);
}

// Replaying a serialized product sequence reproduces the live store, including
// the discontinuity counter across a forced datum change AND an auto bias jump.
TEST(NetworkProductIoTest, ReplayReproducesLiveStore) {
    const std::string path = tempFile("gnss_netprod_equiv.txt");
    std::filesystem::remove(path);

    // Four epochs, 30 s apart:
    //   1: G01 L1C = -0.70                       (first sight, ctr 0)
    //   2: G01 L1C = -0.71  (steady, < 0.05 m)   (no bump,     ctr 0)
    //   3: G01 L1C = -0.71, datum_changed=true   (forced bump, ctr 1)
    //   4: G01 L1C = +0.20  (jump > 0.05 m)      (auto bump,   ctr 2)
    struct Step { double pb01; bool datum_changed; };
    const Step steps[] = {
        {-0.70, false}, {-0.71, false}, {-0.71, true}, {0.20, false},
    };

    // --- Path A: the live producer path (emit, with markDatumChange) ---------
    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live;
    GnssTime              t = kT0;
    GnssTime              last_t = kT0;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        for (const Step& s : steps) {
            const NetworkSolutionEpoch sol = makeEpoch(t, s.pb01);
            if (s.datum_changed) emit_live.markDatumChange();
            emit_live.emitToStore(sol, store_live);
            w.write(sol, s.datum_changed);   // serialize the SAME sequence
            last_t = t;
            t.tow += 30.0;
        }
    }

    // --- Path B: read the file back and replay it ----------------------------
    bool ok = false;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 4u);

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk;
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    // --- The two stores must agree at the final epoch ------------------------
    auto expectSamePhase = [&](const std::string& sat, const std::string& sig) {
        const auto a = store_live.getPhaseBias(sat, sig);
        const auto b = store_disk.getPhaseBias(sat, sig);
        ASSERT_TRUE(a.has_value()) << sat << " " << sig;
        ASSERT_TRUE(b.has_value()) << sat << " " << sig;
        EXPECT_DOUBLE_EQ(a->bias_m, b->bias_m) << sat << " " << sig;
        EXPECT_EQ(a->signal_integer, b->signal_integer) << sat << " " << sig;
        EXPECT_EQ(a->wide_lane_integer, b->wide_lane_integer) << sat << " " << sig;
        EXPECT_EQ(a->discontinuity_counter, b->discontinuity_counter) << sat << " " << sig;
    };
    expectSamePhase("G01", "L1C");
    expectSamePhase("G01", "L2W");
    expectSamePhase("G02", "L1C");

    // The discontinuity counter took the documented path (forced + auto bump).
    EXPECT_EQ(emit_live.discontinuityCounter("G01", "L1C"), 2);
    EXPECT_EQ(emit_disk.discontinuityCounter("G01", "L1C"), 2);
    {
        const auto pb = store_disk.getPhaseBias("G01", "L1C");
        ASSERT_TRUE(pb.has_value());
        EXPECT_EQ(pb->discontinuity_counter, 2);
    }

    // Clock + code bias agree too.
    EXPECT_DOUBLE_EQ(store_live.getCodeBias("G01", "C1W"),
                     store_disk.getCodeBias("G01", "C1W"));

    // The code-bias discontinuity counter (single sat, single band: no set or
    // band-tuple change) takes the forced-datum-change path at epoch 3 and is
    // reproduced on replay from the serialized datum_changed flag alone.
    {
        const auto a = store_live.getCodeBiasDiscontinuity("G01", "C1W");
        const auto b = store_disk.getCodeBiasDiscontinuity("G01", "C1W");
        ASSERT_TRUE(a.has_value());
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ(*a, 1);          // one forced bump at the datum_changed epoch
        EXPECT_EQ(*a, *b);
    }
    const auto oc_live = store_live.getOrbitClock("G01", last_t);
    const auto oc_disk = store_disk.getOrbitClock("G01", last_t);
    ASSERT_TRUE(oc_live.has_value());
    ASSERT_TRUE(oc_disk.has_value());
    EXPECT_DOUBLE_EQ(oc_live->c0, oc_disk->c0);

    std::filesystem::remove(path);
}

// Code-bias discontinuity counters are NOT serialized; they are recomputed from
// the per-epoch satellite-set + band composition (set-change, IF-pin band-tuple
// change) plus the serialized datum_changed flag.  A sequence with a rise, a
// set, a forced bump, and a vanish-then-return must replay byte-for-byte.
TEST(NetworkProductIoTest, CodeBiasReplayReproducesCounters) {
    const std::string path = tempFile("gnss_netprod_cbias.txt");
    std::filesystem::remove(path);

    auto codeEpoch = [](const GnssTime& t, bool with_g03) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        auto addSat = [&](const std::string& sat) {
            NetworkSatSolution s;
            s.sat = sat; s.has_clock = true; s.clock_c0_m = 1.0;
            s.code_bias_m["C1C"] = 0.10;
            s.code_bias_m["C2W"] = 0.20;
            sol.sats.push_back(s);
        };
        addSat("G01");
        addSat("G02");
        if (with_g03) addSat("G03");
        return sol;
    };

    // 1: G01,G02            2: +G03 (rise)   3: -G03 (set, datum_changed)
    // 4: G01,G02 forced     5: +G03 (return)
    struct Step { bool g03; bool datum_changed; };
    const Step steps[] = {
        {false, false}, {true, false}, {false, true}, {false, true}, {true, false},
    };

    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live;
    GnssTime              t = kT0;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        for (const Step& s : steps) {
            const NetworkSolutionEpoch sol = codeEpoch(t, s.g03);
            if (s.datum_changed) emit_live.markDatumChange();
            emit_live.emitToStore(sol, store_live);
            w.write(sol, s.datum_changed);
            t.tow += 30.0;
        }
    }

    bool ok = false;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 5u);

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk;
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    // Every (sat,code) counter in the final store reproduces exactly.
    for (const char* sat : {"G01", "G02", "G03"}) {
        for (const char* code : {"C1C", "C2W"}) {
            const auto a = store_live.getCodeBiasDiscontinuity(sat, code);
            const auto b = store_disk.getCodeBiasDiscontinuity(sat, code);
            ASSERT_TRUE(a.has_value()) << sat << " " << code;
            ASSERT_TRUE(b.has_value()) << sat << " " << code;
            EXPECT_EQ(*a, *b) << sat << " " << code;
            EXPECT_EQ(emit_live.codeBiasDiscontinuityCounter(sat, code),
                      emit_disk.codeBiasDiscontinuityCounter(sat, code)) << sat << " " << code;
        }
    }
    // G01 stepped at every later epoch (rise, set+force, force, return) -> 4.
    EXPECT_EQ(emit_live.codeBiasDiscontinuityCounter("G01", "C1C"), 4);

    std::filesystem::remove(path);
}

TEST(NetworkProductIoTest, MissingFileReportsNotOk) {
    bool ok = true;
    std::vector<NetworkProductRecord> recs =
        NetworkProductReader::read(tempFile("gnss_netprod_does_not_exist.txt"), &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(recs.empty());
}

} // namespace
} // namespace GnssProcessor
