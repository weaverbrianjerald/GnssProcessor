/**
 * @file test_network_product_io.cpp
 * @brief Unit tests for on-disk (de)serialization of the network product.
 *
 * The contract: a NetworkSolutionEpoch written to disk and read back is
 * bit-identical in every consumed field, and replaying the parsed records
 * through a fresh NetworkProductEmitter reproduces the SsrCorrectionStore the
 * live producer builds, discontinuity counters included, even across a datum
 * change (the one piece of stream state the epoch struct does not carry,
 * serialized as the per-epoch datum_changed flag).  This lets consumer AR-frame
 * work replay products without re-running the network.
 */

#include <gtest/gtest.h>

#include "products/NetworkProductIO.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"   // snapshot + warm-resume gate (RTB-7)
#include "products/SsrCorrectionStore.hpp"
#include "logging/CsvRotator.hpp"   // size-based service-CSV rotation (P1)

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

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
    return (gnss_test::tempDir() / name).string();
}

/// Exact on-disk bytes of @p path (binary, no newline translation), for
/// byte-identity checks and for re-serving captured content in chunks.
std::string readAllBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

/// Non-comment (data) bytes of @p path: every line not starting with '#',
/// newline-joined.  The '#' legend is grammar-complete (handoff §7 schema), so
/// the header contains every key= token name; byte probes about the wire (a
/// token present, absent, or counted once on data lines) search these bytes,
/// not the whole file.
std::string dataBytes(const std::string& path) {
    std::ifstream in(path);
    std::string   out, line;
    while (std::getline(in, line))
        if (line.empty() || line[0] != '#') { out += line; out += '\n'; }
    return out;
}

/// Append raw bytes to @p path (binary), closing the stream so the bytes are
/// on disk when the tail polls.  Simulates a producer flush boundary at an
/// arbitrary byte offset, including mid-line.
void appendBytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------

// The optional per-epoch wall-clock generation time (G record) round-trips
// exactly when the producer stamped it, and is absent when it did not
// (generation_time left {0,0}); the unstamped output is byte-identical to the
// format without G records (RT0-4).  A fixed, non-wall-clock stamp keeps the
// test deterministic.
TEST(NetworkProductIoTest, GenerationTimeRoundTripsAndIsOptional) {
    const std::string path = tempFile("gnss_netprod_gentime.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch stamped = makeEpoch(kT0, /*pb01=*/-0.7);
    stamped.generation_time = GnssTime{2313, 259212.5};        // 12.5 s after the content epoch
    NetworkSolutionEpoch unstamped = makeEpoch(GnssTime{2313, 259230.0}, /*pb01=*/-0.7);
    // unstamped.generation_time stays the default {0,0}: no G record expected.

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(stamped);
        w.write(unstamped);
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);

    // Stamped epoch: generation_time round-trips exactly.
    EXPECT_EQ(recs[0].epoch.generation_time.week, 2313);
    EXPECT_NEAR(recs[0].epoch.generation_time.tow, 259212.5, 1e-6);
    // Unstamped epoch: no G record ⇒ generation_time stays {0,0}.
    EXPECT_EQ(recs[1].epoch.generation_time.week, 0);
    EXPECT_DOUBLE_EQ(recs[1].epoch.generation_time.tow, 0.0);

    // Byte level: one 'G' data line in the whole file (only the stamped epoch
    // wrote one); the unstamped epoch's block matches the format without G records.
    std::string content;
    {
        std::ifstream in(path);
        content.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    }   // close the stream before remove(): Windows locks an open file
    std::size_t count = 0, pos = 0;
    while ((pos = content.find("\nG,", pos)) != std::string::npos) { ++count; pos += 3; }
    EXPECT_EQ(count, 1u);

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------

TEST(NetworkProductIoTest, RoundTripsEveryField) {
    const std::string path = tempFile("gnss_netprod_roundtrip.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch sol = makeEpoch(kT0, /*pb01=*/-0.7);

    // A third satellite with no clock exercises the clock-absent path: it
    // round-trips with has_clock=false.
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
// the discontinuity counter across a forced datum change and an automatic bias
// jump.
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
            w.write(sol, s.datum_changed);   // serialize the same sequence
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

// The per-satellite clock discontinuity counter is recomputed on replay from the
// serialized datum_changed flag and the per-constellation affected set (E-line
// field 8).  A datum change scoped to one constellation bumps only that
// constellation's clocks, and replay reproduces the scoping byte-for-byte (A2).
TEST(NetworkProductIoTest, ClockDiscReplayReproducesScopedCounters) {
    const std::string path = tempFile("gnss_netprod_clkdisc.txt");
    std::filesystem::remove(path);

    auto twoSysEpoch = [](const GnssTime& t) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        auto addSat = [&](const std::string& sat, double c0) {
            NetworkSatSolution s;
            s.sat = sat; s.has_clock = true; s.clock_c0_m = c0;
            sol.sats.push_back(s);
        };
        addSat("G01", 12.5);
        addSat("E11", -3.0);
        return sol;
    };

    // 3 epochs; at epoch 2 the GPS clock pivot re-references (only 'G' affected).
    const std::set<char> steps[] = { {}, {'G'}, {} };

    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(0.05, /*emit_clock_disc=*/true);
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        for (const std::set<char>& consts : steps) {
            const NetworkSolutionEpoch sol = twoSysEpoch(t);
            const bool dc = !consts.empty();
            if (dc) emit_live.markDatumChange(consts);
            emit_live.emitToStore(sol, store_live);
            w.write(sol, dc, consts);
            t.tow += 30.0;
        }
    }

    bool ok = false;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 3u);
    // The affected-constellation set round-trips on the datum-change epoch.
    EXPECT_TRUE(recs[0].datum_consts.empty());
    EXPECT_EQ(recs[1].datum_consts.count('G'), 1u);
    EXPECT_EQ(recs[1].datum_consts.count('E'), 0u);

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk(0.05, /*emit_clock_disc=*/true);
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    // GPS clock bumped once, Galileo untouched; live and replayed agree.
    EXPECT_EQ(emit_live.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emit_live.clockDiscontinuityCounter("E11"), 0);
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("E11"), 0);
    EXPECT_EQ(store_disk.getClockDiscontinuity("G01").value_or(-1), 1);
    EXPECT_EQ(store_disk.getClockDiscontinuity("E11").value_or(-1), 0);

    std::filesystem::remove(path);
}

// Quarantine seam and replay (RT0-5): a scoped datum change recorded on an epoch
// whose affected constellation was pruned from the emission (runaway quarantine)
// pends in the emitter and lands at the constellation's next emitted epoch.  The
// file records the datum_consts on the trigger epoch (including the pruned
// constellation) and the replayed epochs are the same pruned epochs, so a fresh
// replay emitter (makeReplayEmitter) defers identically and reproduces the
// counters byte-for-byte.
TEST(NetworkProductIoTest, QuarantinedDatumChangeDefersAndReplays) {
    const std::string path = tempFile("gnss_netprod_quarantine.txt");
    std::filesystem::remove(path);

    auto ep = [](const GnssTime& t, bool with_gps) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        auto addSat = [&](const std::string& sat, double c0) {
            NetworkSatSolution s;
            s.sat = sat; s.has_clock = true; s.clock_c0_m = c0;
            s.code_bias_m["C1C"] = 0.10;
            sol.sats.push_back(s);
        };
        if (with_gps) addSat("G01", 12.5);
        addSat("E11", -3.0);
        return sol;
    };

    // 1: G+E          2: E only (GPS quarantined), datum change {'G','E'}
    // 3: G+E (return) 4: G+E (steady; the landed bump must not repeat)
    struct Step { bool gps; std::set<char> consts; };
    const Step steps[] = { {true, {}}, {false, {'G', 'E'}}, {true, {}}, {true, {}} };

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;
    c.present                    = true;

    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(c.disc_threshold_m, c.emit_clock_disc);
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        for (const Step& s : steps) {
            const NetworkSolutionEpoch sol = ep(t, s.gps);
            const bool dc = !s.consts.empty();
            if (dc) emit_live.markDatumChange(s.consts);
            emit_live.emitToStore(sol, store_live);
            w.write(sol, dc, s.consts);   // consts recorded on the trigger epoch
            t.tow += 30.0;
        }
    }

    // Live semantics: E consumed its flag at the trigger epoch; G's pended across
    // the pruned epoch and landed at the return epoch; each bumped once.
    EXPECT_EQ(emit_live.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emit_live.clockDiscontinuityCounter("E11"), 1);
    EXPECT_EQ(emit_live.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
    EXPECT_EQ(emit_live.codeBiasDiscontinuityCounter("E11", "C1C"), 1);

    // Consumer side: contract + records come out of the file alone.
    bool           ok = false;
    SbasisContract parsed;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed.present);
    ASSERT_EQ(recs.size(), 4u);
    EXPECT_EQ(recs[1].datum_consts.count('G'), 1u);   // trigger-epoch record carries 'G'
    ASSERT_EQ(recs[1].epoch.sats.size(), 1u);         // ...although GPS was pruned from it
    EXPECT_EQ(recs[1].epoch.sats[0].sat, "E11");

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk = NetworkProductReader::makeReplayEmitter(parsed);
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    // Replay reproduces the deferred counters exactly, at emitter and store level.
    for (const char* sat : {"G01", "E11"}) {
        EXPECT_EQ(emit_disk.clockDiscontinuityCounter(sat),
                  emit_live.clockDiscontinuityCounter(sat)) << sat;
        EXPECT_EQ(emit_disk.codeBiasDiscontinuityCounter(sat, "C1C"),
                  emit_live.codeBiasDiscontinuityCounter(sat, "C1C")) << sat;
        EXPECT_EQ(store_disk.getClockDiscontinuity(sat).value_or(-1),
                  store_live.getClockDiscontinuity(sat).value_or(-2)) << sat;
    }
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("E11"), 1);

    std::filesystem::remove(path);
}

// The per-λ fractional-cycle threshold travels in the S contract (discfrac=), so
// a consumer's replay emitter recomputes the phase-bias discontinuity counters
// byte-for-byte (H3).  A 0.055 m step crosses the 0.25λ L1 gate (4.76 cm) but not
// the L5 gate (6.37 cm).  Without the discfrac round-trip the replay emitter falls
// back to the flat 0.05 m gate and also fires L5, so the test pins both the
// round-trip and the makeReplayEmitter wiring.
TEST(NetworkProductIoTest, PerLambdaThresholdReplayReproducesCounters) {
    const std::string path = tempFile("gnss_netprod_perlambda.txt");
    std::filesystem::remove(path);

    auto phaseEp = [](const GnssTime& t, double b) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        NetworkSatSolution s; s.sat = "G01"; s.has_clock = false;
        s.phase_bias["L1C"] = NetworkPhaseBias{b, true, 1};
        s.phase_bias["L5Q"] = NetworkPhaseBias{b, true, 1};
        sol.sats.push_back(s);
        return sol;
    };

    SbasisContract c;
    c.decl.reference_receiver_id     = "CIT100USA";
    c.disc_threshold_m               = 0.05;
    c.disc_threshold_frac_cycles     = 0.25;   // per-λ mode
    c.present                        = true;

    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(c.disc_threshold_m, /*clk=*/false, /*stale=*/0.0,
                                    /*cbj=*/false, /*cks=*/false,
                                    c.disc_threshold_frac_cycles);
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        for (double b : {0.0, 0.055}) {            // baseline, then the straddling step
            const NetworkSolutionEpoch sol = phaseEp(t, b);
            emit_live.emitToStore(sol, store_live);
            w.write(sol, /*datum_changed=*/false, {});
            t.tow += 30.0;
        }
    }
    EXPECT_EQ(emit_live.discontinuityCounter("G01", "L1C"), 1);   // L1 fired (0.055 > 4.76 cm)
    EXPECT_EQ(emit_live.discontinuityCounter("G01", "L5Q"), 0);   // L5 held (0.055 < 6.37 cm)

    bool           ok = false;
    SbasisContract parsed;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed.present);
    EXPECT_DOUBLE_EQ(parsed.disc_threshold_frac_cycles, 0.25);    // discfrac= round-trip

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk = NetworkProductReader::makeReplayEmitter(parsed);
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    // Replay reproduces the per-λ decision exactly, without the flat-gate L5 false fire.
    EXPECT_EQ(emit_disk.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(emit_disk.discontinuityCounter("G01", "L5Q"), 0);

    std::filesystem::remove(path);
}

// Code-bias discontinuity counters are not serialized; they are recomputed from
// the per-epoch satellite set and band composition (set change, IF-pin band-tuple
// change) plus the serialized datum_changed flag.  A sequence with a rise, a set,
// a forced bump, and a vanish-then-return replays byte-for-byte.
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

// The per-satellite retro-bump is serialized as an R record and reproduced on
// replay, bumping only the named satellite's phase, code and clock counters.  A
// run with the retro-bump off writes no R record, so its format is unchanged
// byte-for-byte (B1, P0-2/P1-3).
TEST(NetworkProductIoTest, RetroBumpPerSatReplayReproducesCounters) {
    const std::string path = tempFile("gnss_netprod_retro.txt");
    std::filesystem::remove(path);

    auto ep = [](const GnssTime& t) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        for (const char* s : {"G01", "G02"}) {
            NetworkSatSolution ns;
            ns.sat = s; ns.has_clock = true; ns.clock_c0_m = 1.0;
            ns.code_bias_m["C1W"] = 0.2;
            ns.phase_bias["L1C"]  = NetworkPhaseBias{0.1, true, 1};
            sol.sats.push_back(ns);
        }
        return sol;
    };
    // Epoch 2 retro-bumps G01 only.
    const std::set<std::string> retro[] = { {}, {"G01"}, {} };

    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(0.05, /*emit_clock_disc=*/true);
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        for (const std::set<std::string>& r : retro) {
            const NetworkSolutionEpoch sol = ep(t);
            if (!r.empty()) emit_live.markSatDiscontinuity(r);
            emit_live.emitToStore(sol, store_live);
            w.write(sol, /*datum_changed=*/false, /*datum_consts=*/{}, /*retro_sats=*/r);
            t.tow += 30.0;
        }
    }

    bool ok = false;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_TRUE(recs[0].retro_sats.empty());
    EXPECT_EQ(recs[1].retro_sats.count("G01"), 1u);
    EXPECT_EQ(recs[1].retro_sats.count("G02"), 0u);

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk(0.05, /*emit_clock_disc=*/true);
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    // G01 bumped once on all three channels, G02 untouched; live == replayed.
    EXPECT_EQ(emit_live.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(emit_disk.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(emit_disk.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emit_disk.discontinuityCounter("G02", "L1C"), 0);
    EXPECT_EQ(emit_disk.codeBiasDiscontinuityCounter("G02", "C1W"), 0);
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("G02"), 0);

    std::filesystem::remove(path);
}

// The per-satellite URA (U record) and clock-model drift C1 (C-line field 4)
// round-trip through the file, and replay pushes the URA into the store (B1/B3b).
TEST(NetworkProductIoTest, UraAndClockDriftRoundTrip) {
    const std::string path = tempFile("gnss_netprod_ura.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch sol;
    sol.epoch_time = kT0; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    NetworkSatSolution g;
    g.sat = "G01"; g.has_clock = true; g.clock_c0_m = 5.0;
    g.clock_c1_m_s = -0.0123456;   // two-state drift
    g.clock_ura_m  = 0.037; g.has_ura = true;
    sol.sats.push_back(g);

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }

    bool ok = false;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_EQ(recs[0].epoch.sats.size(), 1u);
    const NetworkSatSolution& s = recs[0].epoch.sats[0];
    EXPECT_DOUBLE_EQ(s.clock_c0_m, 5.0);
    EXPECT_DOUBLE_EQ(s.clock_c1_m_s, -0.0123456);   // exact round-trip
    EXPECT_TRUE(s.has_ura);
    EXPECT_DOUBLE_EQ(s.clock_ura_m, 0.037);

    SsrCorrectionStore    store;
    NetworkProductEmitter emit;
    NetworkProductReader::replay(recs[0], store, emit);
    const auto u = store.getUra("G01", kT0);
    ASSERT_TRUE(u.has_value());
    EXPECT_DOUBLE_EQ(*u, 0.037);

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------

// The brdc_rts clock ephemeris-anchor IODE and RTS-orbit echo serialize as
// self-describing key=value tokens on the C line (iode=/oiod=/oepoch=) and
// round-trip exactly.  Replay stamps them onto the store's merged
// OrbitClockCorrection, so a file-replayed product gates the clock on the
// producer's broadcast baseline in the same way as the live closed loop.  A
// second satellite left at the default sentinel (clock_iode = -1) writes no
// token (RTF-2).
TEST(NetworkProductIoTest, ClockIodeAndOrbitEchoRoundTripAndStamp) {
    const std::string path = tempFile("gnss_netprod_iode.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch sol = makeEpoch(kT0, /*pb01=*/-0.7);
    // G01: stamped like a brdc_rts producer (broadcast IODE 42, estimated against
    // RTS orbit IOD-SSR 7 at a slightly earlier content epoch).  A non-zero C1
    // drift places the key=value tokens after a positional c1 field.
    sol.sats[0].clock_iode    = 42;
    sol.sats[0].orbit_iod_ssr = 7;
    sol.sats[0].orbit_epoch   = GnssTime{2313, 259100.5};
    sol.sats[0].clock_c1_m_s  = -0.0025;
    // G02: unstamped (default clock_iode = -1); writes no iode= token.

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_EQ(recs[0].epoch.sats.size(), 2u);

    const NetworkSatSolution& g1 = recs[0].epoch.sats[0];
    EXPECT_EQ(g1.clock_iode, 42);
    EXPECT_EQ(g1.orbit_iod_ssr, 7);
    EXPECT_EQ(g1.orbit_epoch.week, 2313);
    EXPECT_DOUBLE_EQ(g1.orbit_epoch.tow, 259100.5);   // exact round-trip
    EXPECT_DOUBLE_EQ(g1.clock_c1_m_s, -0.0025);        // positional c1 still parses

    const NetworkSatSolution& g2 = recs[0].epoch.sats[1];
    EXPECT_EQ(g2.clock_iode, -1);        // unstamped
    EXPECT_EQ(g2.orbit_iod_ssr, -1);
    EXPECT_EQ(g2.orbit_epoch.week, 0);

    // The stamped satellite's tokens are present.
    const std::string content = readAllBytes(path);
    EXPECT_NE(content.find("iode=42"), std::string::npos);
    EXPECT_NE(content.find("oiod=7"), std::string::npos);
    EXPECT_NE(content.find("oepoch=2313:259100.5"), std::string::npos);

    // Replay stamps the store: getOrbitClock on the standalone clock reports the
    // producer's clock_iode (which takes precedence over the -1 inherited from the
    // orbit) and the orbit echo.
    SsrCorrectionStore    store;
    NetworkProductEmitter emit;
    NetworkProductReader::replay(recs[0], store, emit);
    const auto q = store.getOrbitClock("G01", kT0, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->clock_iode, 42);
    EXPECT_EQ(q->orbit_iod_ssr, 7);
    EXPECT_EQ(q->orbit_epoch.week, 2313);
    EXPECT_DOUBLE_EQ(q->orbit_epoch.tow, 259100.5);
    // Consumer gate: applies on a matching broadcast IODE, drops on a mismatch.
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(42, q->clock_iode));
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(41, q->clock_iode));
    // G02 unstamped: clock_iode stays the permissive -1 (inherited orbit anchor).
    const auto q2 = store.getOrbitClock("G02", kT0, /*max_age_s=*/30.0);
    ASSERT_TRUE(q2.has_value());
    EXPECT_EQ(q2->clock_iode, -1);

    std::filesystem::remove(path);
}

// The iode=/oiod= key=value tokens coexist with the RTA-4 serialized disc
// counters.  With serialize_disc_counters armed, the C line carries the
// always-written c1 (field 4) and the clock disc counter (field 5) before the
// key=value tokens, and the reader parses both the positional counter and the
// trailing IODE.  This is the field-ordering stress case (RTF-2).
TEST(NetworkProductIoTest, ClockIodeCoexistsWithSerializedDiscCounters) {
    const std::string path = tempFile("gnss_netprod_iode_disc.txt");
    std::filesystem::remove(path);

    auto ep = [](const GnssTime& t) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        NetworkSatSolution g;
        g.sat = "G01"; g.has_clock = true; g.clock_c0_m = 5.0;
        g.clock_c1_m_s  = 0.01;
        g.clock_iode    = 15;
        g.orbit_iod_ssr = 4;
        sol.sats.push_back(g);
        return sol;
    };

    NetworkProductEmitter emit(NetworkProductEmitter::kDefaultDiscThresholdM,
                               /*emit_clock_disc=*/true);
    const GnssTime t2{kT0.week, kT0.tow + 5.0};
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        SsrCorrectionStore   store;
        NetworkSolutionEpoch e1 = ep(kT0);
        emit.emitToStore(e1, store);
        w.write(e1, /*datum_changed=*/false, {}, {}, &emit);   // disc counter 0
        NetworkSolutionEpoch e2 = ep(t2);
        emit.markDatumChange();                                // forced clock bump
        emit.emitToStore(e2, store);
        w.write(e2, /*datum_changed=*/true, {}, {}, &emit);    // disc counter 1
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    // Both the serialized clock disc counter and the stamped IODE parse back.
    EXPECT_EQ(recs[0].epoch.sats[0].clock_iode, 15);
    EXPECT_EQ(recs[0].epoch.sats[0].orbit_iod_ssr, 4);
    EXPECT_DOUBLE_EQ(recs[0].epoch.sats[0].clock_c1_m_s, 0.01);   // positional c1
    ASSERT_EQ(recs[0].serialized_disc.clock.count("G01"), 1u);
    EXPECT_EQ(recs[0].serialized_disc.clock.at("G01"), 0);
    EXPECT_EQ(recs[1].epoch.sats[0].clock_iode, 15);
    ASSERT_EQ(recs[1].serialized_disc.clock.count("G01"), 1u);
    EXPECT_EQ(recs[1].serialized_disc.clock.at("G01"), 1);        // forced bump

    std::filesystem::remove(path);
}

// The sp3-mode sentinel (clock_iode = -1) writes no iode=/oiod=/oepoch= token, so
// a producer that never sets the fields is byte-identical to a run that
// explicitly leaves them at their sentinels: the twin-file invariant
// (RTF-2).
TEST(NetworkProductIoTest, ClockIodeSentinelTwinFileByteIdentical) {
    const std::string a = tempFile("gnss_netprod_iode_a.txt");
    const std::string b = tempFile("gnss_netprod_iode_b.txt");
    std::filesystem::remove(a);
    std::filesystem::remove(b);

    // File A: plain makeEpoch (fields at their in-struct defaults).
    { NetworkProductWriter w(a); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, -0.7)); }
    // File B: the same epoch with the IODE fields explicitly at their sentinels.
    NetworkSolutionEpoch sol = makeEpoch(kT0, -0.7);
    for (auto& s : sol.sats) {
        s.clock_iode    = -1;
        s.orbit_iod_ssr = -1;
        s.orbit_epoch   = GnssTime{0, 0.0};
    }
    { NetworkProductWriter w(b); ASSERT_TRUE(w.good()); w.write(sol); }

    const std::string ba = readAllBytes(a);
    const std::string bb = readAllBytes(b);
    EXPECT_EQ(ba, bb);                                  // byte-identical
    const std::string da = dataBytes(a);                // wire probes: data lines only
    EXPECT_EQ(da.find("iode="), std::string::npos);     // no IODE tokens at all
    EXPECT_EQ(da.find("oiod="), std::string::npos);
    EXPECT_EQ(da.find("oepoch="), std::string::npos);

    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

// The v2 S-basis contract header (S line, including the realized
// per-constellation pivots and the a-priori product id) round-trips, and
// checkContract accepts a matching consumer expectation and flags a mismatched
// one (B6, P1-1).
TEST(NetworkProductIoTest, SbasisContractRoundTripAndCheck) {
    const std::string path = tempFile("gnss_netprod_contract.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.clock_pivot_sat["G"]  = "G05";
    c.decl.clock_pivot_sat["E"]  = "E11";
    c.decl.emit_code_bias        = true;
    c.apriori_product_id         = "COD0MGXFIN_ORB.SP3+COD0MGXFIN_CLK.CLK";  // comma-free
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;   // replay-emitter construction parameter
    c.present                    = true;

    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));   // at least one epoch after the header
    }

    bool           ok = false;
    SbasisContract parsed;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.decl.reference_receiver_id, "CIT100USA");
    ASSERT_EQ(parsed.decl.clock_pivot_sat.count("G"), 1u);
    EXPECT_EQ(parsed.decl.clock_pivot_sat.at("G"), "G05");
    EXPECT_EQ(parsed.decl.clock_pivot_sat.at("E"), "E11");
    EXPECT_TRUE(parsed.decl.emit_code_bias);
    EXPECT_EQ(parsed.apriori_product_id, c.apriori_product_id);
    EXPECT_DOUBLE_EQ(parsed.disc_threshold_m, 0.05);
    EXPECT_TRUE(parsed.emit_clock_disc);

    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, c, &mism));
    EXPECT_TRUE(mism.empty());

    SbasisContract other = c;
    other.decl.reference_receiver_id = "JPLM00USA";   // incompatible reference receiver
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed, other, &mism));
    EXPECT_FALSE(mism.empty());

    // A v1 file carries no contract: read reports present=false and checkContract
    // flags "no contract".
    const std::string v1 = tempFile("gnss_netprod_v1.txt");
    std::filesystem::remove(v1);
    { NetworkProductWriter w(v1); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, 0.1)); }
    SbasisContract none;
    bool ok1 = false;
    NetworkProductReader::read(v1, &none, &ok1);
    EXPECT_TRUE(ok1);
    EXPECT_FALSE(none.present);
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(none, c, &mism));
    EXPECT_FALSE(mism.empty());

    std::filesystem::remove(path);
    std::filesystem::remove(v1);
}

// The '#' legend declares every field the writer can emit (handoff §7 schema).
// A consumer that follows the schema drops any field the legend does not
// declare; the P record's sigma= field is the case that motivated the test.
// This test arms every optional emit channel at once and checks each data line
// against the legend grammar:
//   * its record tag is legended;
//   * its positional field count fits the legend (brackets stripped; a legend
//     token containing "..." makes that tag variadic);
//   * every key=value field's key is declared on that tag's legend line.
// A new emitted field without a matching legend extension fails here, on any
// record type, not only sigma=.
TEST(NetworkProductIoTest, HeaderLegendDeclaresEveryEmittedField) {
    const std::string path = tempFile("gnss_netprod_legend.txt");
    std::filesystem::remove(path);

    // A contract arming every S-line extension.
    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.clock_pivot_sat["G"]  = "G05";
    c.decl.eq34_datum_receiver   = "CIT100USA";
    c.decl.emit_code_bias        = true;
    c.apriori_product_id         = "CODFIN2024129";
    c.disc_threshold_m           = 0.05;
    c.disc_threshold_frac_cycles = 0.25;
    c.emit_clock_disc            = true;
    c.staleness_horizon_s        = 60.0;
    c.code_bias_jump_test        = true;
    c.clock_disc_on_set_change   = true;
    c.generation_id              = 7;
    c.frame_id                   = "IGS20";
    c.coord_epoch                = "2024.5";
    c.atx_id                     = "igs20.atx";
    c.orbit_ref                  = "CoM";
    c.age_scale_k                = 1e-6;
    c.cb_pin_pair["G"]           = "C1C-C2L";
    c.gf_pin_pair["G"]           = "C1C-C2L";
    c.present                    = true;

    // An epoch arming every optional record / field channel.
    NetworkSolutionEpoch sol = makeEpoch(kT0, /*pb01=*/-0.7);
    sol.generation_time       = GnssTime{2313, 259212.5};             // G record
    sol.generation_id         = 7;                                    // I record
    sol.sats[0].clock_c1_m_s  = -0.0025;                              // C c1 field
    sol.sats[0].clock_iode    = 42;                                   // C iode=
    sol.sats[0].orbit_iod_ssr = 7;                                    // C oiod=
    sol.sats[0].orbit_epoch   = GnssTime{2313, 259100.5};             // C oepoch=
    sol.sats[0].sigcc_m       = 0.31;                                 // C sigcc=
    sol.sats[0].rho           = -0.5;                                 // C rho=
    sol.sats[0].clock_ura_m   = 0.037;                                // U record
    sol.sats[0].has_ura       = true;
    for (auto& [sig, pb] : sol.sats[0].phase_bias) {                  // P sigma= / sigc=
        pb.sigma_m = 0.012;
        pb.sigc_m  = 0.0026;
    }

    NetworkProductEmitter emit = NetworkProductReader::makeReplayEmitter(c);
    SsrCorrectionStore    store;
    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        NetworkProducerStatus st;                                     // H record
        st.time             = kT0;
        st.state            = NetworkProducerStatus::State::Ok;
        st.sigma0           = 1.23;
        st.n_sats_published = 2;
        w.writeEpochStatus(st);
        emit.markDatumChange({'G'});                                  // E datum fields
        emit.markSatDiscontinuity({"G01"});                           // R record (live-faithful)
        emit.emitToStore(sol, store);
        w.write(sol, /*datum_changed=*/true, /*datum_consts=*/{'G'},
                /*retro_sats=*/{"G01"}, /*disc_source=*/&emit);       // disc fields on C/B/P
        w.writeContract(c);                                           // mid-file S line (RTA-5)
    }

    auto split = [](const std::string& line) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (true) {
            const std::size_t comma = line.find(',', start);
            if (comma == std::string::npos) { out.push_back(line.substr(start)); break; }
            out.push_back(line.substr(start, comma - start));
            start = comma + 1;
        }
        return out;
    };

    // ---- parse the legend into per-tag grammar -----------------------------
    struct TagGrammar {
        std::size_t           max_positional{0};
        bool                  variadic{false};
        std::set<std::string> keys;
    };
    std::map<std::string, TagGrammar> legend;
    std::vector<std::string>          data_lines;
    {
        std::ifstream in(path);
        std::string   line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (line[0] != '#') { data_lines.push_back(line); continue; }
            // A legend line is "# <TAG>,...": a single-character tag, then a comma.
            if (line.size() < 4 || line[3] != ',') continue;   // banner / prose lines
            std::string body = line.substr(2);
            const auto  paren = body.find('(');                // strip "(...)" annotation
            if (paren != std::string::npos) body = body.substr(0, paren);
            while (!body.empty() && body.back() == ' ') body.pop_back();
            TagGrammar g;
            g.variadic = body.find("...") != std::string::npos;
            std::string flat;                                  // brackets / ellipses stripped
            for (char ch : body)
                if (ch != '[' && ch != ']') flat += ch;
            std::size_t e;
            while ((e = flat.find("...")) != std::string::npos) flat.erase(e, 3);
            const std::vector<std::string> toks = split(flat);
            for (std::size_t i = 0; i < toks.size(); ++i) {
                const auto eq = toks[i].find('=');
                if (eq != std::string::npos) g.keys.insert(toks[i].substr(0, eq));
                else ++g.max_positional;
            }
            legend[std::string(1, body[0])] = g;
        }
    }

    // ---- every data line must fit its tag's declared grammar ---------------
    ASSERT_FALSE(data_lines.empty());
    std::set<std::string> tags_seen;
    for (const std::string& dl : data_lines) {
        const std::vector<std::string> f   = split(dl);
        const std::string              tag = f[0];
        tags_seen.insert(tag);
        ASSERT_EQ(legend.count(tag), 1u) << "unlegended record tag '" << tag << "': " << dl;
        const TagGrammar& g     = legend[tag];
        std::size_t       n_pos = 0;
        for (const std::string& fld : f) {
            const auto eq = fld.find('=');
            if (eq == std::string::npos) { ++n_pos; continue; }
            EXPECT_EQ(g.keys.count(fld.substr(0, eq)), 1u)
                << "undeclared key '" << fld.substr(0, eq) << "' on tag " << tag << ": " << dl;
        }
        if (!g.variadic)
            EXPECT_LE(n_pos, g.max_positional) << "positional overflow on tag " << tag << ": " << dl;
    }

    // The test itself keeps emitting every record type; a channel dropped from
    // the arming set would leave that record type unchecked.
    for (const char* t : {"E", "C", "B", "P", "G", "I", "R", "U", "S", "H"})
        EXPECT_EQ(tags_seen.count(t), 1u) << "test no longer emits record type " << t;

    // The P legend line declares the σ tokens that a producer with
    // emit_phase_bias_sigma armed writes.
    ASSERT_EQ(legend.count("P"), 1u);
    EXPECT_EQ(legend["P"].keys.count("sigma"), 1u);
    EXPECT_EQ(legend["P"].keys.count("sigc"), 1u);
    EXPECT_GE(legend["P"].max_positional, 7u);   // ...,wide_lane_integer,phase_disc

    // The file still parses cleanly end-to-end.
    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);

    // v1 parity: a v1 header carries the same grammar-complete legend (the two
    // headers differ only in the banner and the S line), so the two paths
    // cannot drift apart.
    const std::string v1 = tempFile("gnss_netprod_legend_v1.txt");
    std::filesystem::remove(v1);
    { NetworkProductWriter w(v1); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, 0.1)); }
    auto legendLines = [](const std::string& p) {
        std::vector<std::string> out;
        std::ifstream in(p);
        std::string   line;
        while (std::getline(in, line))
            if (line.size() >= 4 && line[0] == '#' && line[3] == ',') out.push_back(line);
        return out;
    };
    EXPECT_EQ(legendLines(v1), legendLines(path));

    std::filesystem::remove(path);
    std::filesystem::remove(v1);
}

// The S contract can declare the code-bias IF pin pair, the signal pair in which
// the emitted OSB stream is internally consistent (§1.5, day-129 frame-guards
// report).  It round-trips (v2 header and RTA-5 re-broadcast); checkContract
// flags a consumer whose declared pair sits outside the product frame (the
// mixed-pair regime); a consumer that declares no pair never mismatches on it;
// and a producer that does not declare it writes no token, so its S line is
// unchanged byte-for-byte.
TEST(NetworkProductIoTest, CbPinPairTravelsInContractAndChecks) {
    const std::string path = tempFile("gnss_netprod_cbpin.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.disc_threshold_m           = 0.05;
    c.cb_pin_pair["G"]           = "C1C-C2L";
    c.cb_pin_pair["E"]           = "C1C-C5Q";
    c.present                    = true;

    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));
        w.writeContract(c);                    // re-broadcast, same grammar (RTA-5)
        w.write(makeEpoch(GnssTime{kT0.week, kT0.tow + 30.0}, /*pb01=*/-0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_TRUE(parsed.present);
    ASSERT_EQ(parsed.cb_pin_pair.size(), 2u);
    EXPECT_EQ(parsed.cb_pin_pair.at("G"), "C1C-C2L");
    EXPECT_EQ(parsed.cb_pin_pair.at("E"), "C1C-C5Q");
    // The re-broadcast stamped the second epoch's contract_refresh with the pairs.
    ASSERT_TRUE(recs[1].contract_refresh.present);
    EXPECT_EQ(recs[1].contract_refresh.cb_pin_pair.at("G"), "C1C-C2L");

    // Consumer declaring the matching pair for GPS only: compatible (the file's
    // extra Galileo entry is not a mismatch, as with clock_pivot_sat).
    SbasisContract expect_ok = c;
    expect_ok.cb_pin_pair.clear();
    expect_ok.cb_pin_pair["G"] = "C1C-C2L";
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, expect_ok, &mism));
    EXPECT_TRUE(mism.empty());

    // Consumer whose pair sits outside the product frame: flagged.
    SbasisContract expect_bad = expect_ok;
    expect_bad.cb_pin_pair["G"] = "C1W-C2W";
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed, expect_bad, &mism));
    ASSERT_EQ(mism.size(), 1u);
    EXPECT_NE(mism[0].find("cb_pin_pair[G]"), std::string::npos);

    // Consumer that declares nothing never mismatches on the pin pair.
    SbasisContract expect_none = c;
    expect_none.cb_pin_pair.clear();
    mism.clear();
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, expect_none, &mism));

    // Undeclared producer: no cbpin= token anywhere, so the S line is the same
    // as that of a producer without the §1.5 declaration.
    const std::string bare = tempFile("gnss_netprod_cbpin_bare.txt");
    std::filesystem::remove(bare);
    SbasisContract c0 = c;
    c0.cb_pin_pair.clear();
    { NetworkProductWriter w(bare, c0); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, 0.1)); }
    EXPECT_EQ(dataBytes(bare).find("cbpin="), std::string::npos);
    EXPECT_NE(dataBytes(path).find("cbpin=E:C1C-C5Q"), std::string::npos);
    EXPECT_NE(dataBytes(path).find("cbpin=G:C1C-C2L"), std::string::npos);

    std::filesystem::remove(path);
    std::filesystem::remove(bare);
}

// The geometry-free datum pair (the retired network.code_bias_gf_datum, Odijk rd 3b;
// the producer no longer fills it, and the wire keeps it for products written with
// it).  Its presence on the S line tells a consumer that the emitted code biases are the
// slant-posture estimable function (the pinned pair is identically the datum,
// content only from j >= 3, each satellite's geometry-free bias absorbed by the
// ionosphere, Table 8) rather than the single-layer one; the record layout is
// the same in both cases.  It round-trips (v2 header and RTA-5 re-broadcast);
// checkContract flags a consumer that expects the GF datum against a
// single-layer file (the severe direction: the two disagree about what the B
// records are, not only about their frame); a consumer that declares nothing
// never mismatches; and a single-layer producer writes no token, so its S line
// is unchanged byte-for-byte.  The parser also accepts the '+'-joined
// per-family value form (docs/SPEC_family_gf_datum.md) verbatim, so a
// multi-family product round-trips through the current reader.
TEST(NetworkProductIoTest, GfPinPairTravelsInContractAndChecks) {
    const std::string path = tempFile("gnss_netprod_gfpin.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.disc_threshold_m           = 0.05;
    c.cb_pin_pair["G"]           = "C1C-C2W";
    c.cb_pin_pair["E"]           = "C1C-C5Q";
    c.gf_pin_pair["G"]           = "C1C-C2W";   // same pair as the IF pin, by construction
    c.gf_pin_pair["E"]           = "C1C-C5Q";
    c.present                    = true;

    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));
        w.writeContract(c);                    // re-broadcast, same grammar (RTA-5)
        w.write(makeEpoch(GnssTime{kT0.week, kT0.tow + 30.0}, /*pb01=*/-0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_TRUE(parsed.present);
    ASSERT_EQ(parsed.gf_pin_pair.size(), 2u);
    EXPECT_EQ(parsed.gf_pin_pair.at("G"), "C1C-C2W");
    EXPECT_EQ(parsed.gf_pin_pair.at("E"), "C1C-C5Q");
    // cbpin= and gfpin= coexist: the gfpin branch dispatches before the bare-':'
    // realized-pivot fallback, as cbpin= does (both values contain a ':').
    EXPECT_EQ(parsed.cb_pin_pair.at("G"), "C1C-C2W");
    EXPECT_TRUE(parsed.decl.clock_pivot_sat.empty());
    // The re-broadcast stamped the second epoch's contract_refresh too.
    ASSERT_TRUE(recs[1].contract_refresh.present);
    EXPECT_EQ(recs[1].contract_refresh.gf_pin_pair.at("E"), "C1C-C5Q");

    // Consumer declaring the matching GF datum for GPS only: compatible.
    SbasisContract expect_ok = c;
    expect_ok.gf_pin_pair.clear();
    expect_ok.gf_pin_pair["G"] = "C1C-C2W";
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, expect_ok, &mism));
    EXPECT_TRUE(mism.empty());

    // Consumer that declares nothing never mismatches on the GF pair; no token on
    // the wire reads as "single-layer", the safe default.
    SbasisContract expect_none = c;
    expect_none.gf_pin_pair.clear();
    mism.clear();
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, expect_none, &mism));

    // Single-layer producer: no gfpin= token, and a consumer expecting the GF datum
    // is flagged against it.  Passing this case would let the consumer apply the
    // two-direction S-transform to one-direction biases.
    const std::string bare = tempFile("gnss_netprod_gfpin_bare.txt");
    std::filesystem::remove(bare);
    SbasisContract c0 = c;
    c0.gf_pin_pair.clear();
    { NetworkProductWriter w(bare, c0); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, 0.1)); }
    EXPECT_EQ(dataBytes(bare).find("gfpin="), std::string::npos);
    EXPECT_NE(dataBytes(bare).find("cbpin=G:C1C-C2W"), std::string::npos);  // cbpin unaffected
    EXPECT_NE(dataBytes(path).find("gfpin=G:C1C-C2W"), std::string::npos);

    bool           ok0 = false;
    SbasisContract parsed0;
    NetworkProductReader::read(bare, &parsed0, &ok0);
    ASSERT_TRUE(ok0);
    EXPECT_TRUE(parsed0.gf_pin_pair.empty());
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed0, expect_ok, &mism));
    ASSERT_EQ(mism.size(), 1u);
    EXPECT_NE(mism[0].find("gf_pin_pair[G]"), std::string::npos);
    EXPECT_NE(mism[0].find("single-layer"), std::string::npos);

    // Forward compatibility: the per-family value form parses verbatim.
    const std::string fam = tempFile("gnss_netprod_gfpin_family.txt");
    std::filesystem::remove(fam);
    SbasisContract cf = c;
    cf.gf_pin_pair.clear();
    cf.gf_pin_pair["E"] = "C1C-C5Q+C1X-C5X";
    { NetworkProductWriter w(fam, cf); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, 0.1)); }
    bool           okf = false;
    SbasisContract parsedf;
    NetworkProductReader::read(fam, &parsedf, &okf);
    ASSERT_TRUE(okf);
    EXPECT_EQ(parsedf.gf_pin_pair.at("E"), "C1C-C5Q+C1X-C5X");

    std::filesystem::remove(path);
    std::filesystem::remove(bare);
    std::filesystem::remove(fam);
}

// csvSanitize makes an arbitrary string safe for one CSV field: every ',' becomes
// ';' and a comma-free string is unchanged.  network_contract_check.csv writes
// mismatch strings through it, so a mismatch message containing a comma cannot
// break the column grid (L14).
TEST(NetworkProductIoTest, CsvSanitizeReplacesCommas) {
    EXPECT_EQ(NetworkProductReader::csvSanitize("reference_receiver_id: A vs B"),
              "reference_receiver_id: A vs B");                 // comma-free unchanged
    EXPECT_EQ(NetworkProductReader::csvSanitize("a,b,c"), "a;b;c");
    EXPECT_EQ(NetworkProductReader::csvSanitize(""), "");
    EXPECT_EQ(NetworkProductReader::csvSanitize(",,"), ";;");
}

// The replay-emitter construction parameters travel in the contract: a consumer
// that builds its emitter with makeReplayEmitter(parsed), with no out-of-band
// knowledge of the producer's config, reproduces the clock discontinuity
// counters byte-for-byte.  A v1 file (no contract) yields the default emitter,
// whose clock channel is silent (the replay behaviour without B6).
TEST(NetworkProductIoTest, ReplayEmitterFromContractReproducesClockDisc) {
    const std::string path = tempFile("gnss_netprod_replayctor.txt");
    std::filesystem::remove(path);

    auto clockEpoch = [](const GnssTime& t) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        NetworkSatSolution g, e;
        g.sat = "G01"; g.has_clock = true; g.clock_c0_m = 12.5;
        e.sat = "E11"; e.has_clock = true; e.clock_c0_m = -3.0;
        sol.sats.push_back(g); sol.sats.push_back(e);
        return sol;
    };

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;
    c.present                    = true;

    // 3 epochs; at epoch 2 the GPS clock pivot re-references (scoped to 'G').
    const std::set<char> steps[] = { {}, {'G'}, {} };
    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(c.disc_threshold_m, c.emit_clock_disc);
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        for (const std::set<char>& consts : steps) {
            const NetworkSolutionEpoch sol = clockEpoch(t);
            const bool dc = !consts.empty();
            if (dc) emit_live.markDatumChange(consts);
            emit_live.emitToStore(sol, store_live);
            w.write(sol, dc, consts);
            t.tow += 30.0;
        }
    }

    // Consumer side: everything needed comes out of the file itself.
    bool           ok = false;
    SbasisContract parsed;
    std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed.present);
    ASSERT_EQ(recs.size(), 3u);

    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk = NetworkProductReader::makeReplayEmitter(parsed);
    for (const NetworkProductRecord& r : recs)
        NetworkProductReader::replay(r, store_disk, emit_disk);

    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("G01"),
              emit_live.clockDiscontinuityCounter("G01"));
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("G01"), 1);
    EXPECT_EQ(emit_disk.clockDiscontinuityCounter("E11"), 0);
    EXPECT_EQ(store_disk.getClockDiscontinuity("G01").value_or(-1), 1);
    EXPECT_EQ(store_disk.getClockDiscontinuity("E11").value_or(-1), 0);

    // v1 file: makeReplayEmitter of an absent contract gives the default emitter,
    // so the clock channel stays silent (the replay behaviour without B6).
    const std::string v1 = tempFile("gnss_netprod_replayctor_v1.txt");
    std::filesystem::remove(v1);
    {
        NetworkProductWriter w(v1);
        ASSERT_TRUE(w.good());
        w.write(clockEpoch(kT0), /*datum_changed=*/true);
    }
    SbasisContract none;
    bool ok1 = false;
    std::vector<NetworkProductRecord> recs1 = NetworkProductReader::read(v1, &none, &ok1);
    ASSERT_TRUE(ok1);
    EXPECT_FALSE(none.present);
    SsrCorrectionStore    store_v1;
    NetworkProductEmitter emit_v1 = NetworkProductReader::makeReplayEmitter(none);
    for (const NetworkProductRecord& r : recs1)
        NetworkProductReader::replay(r, store_v1, emit_v1);
    EXPECT_EQ(emit_v1.clockDiscontinuityCounter("G01"), 0);

    std::filesystem::remove(path);
    std::filesystem::remove(v1);
}

// The two optional replay-emitter channel flags, code_bias_jump_test (the opt-in
// code-bias magnitude jump test) and clock_disc_on_set_change (the sum-to-zero
// clock-set datum-move signal), travel in the S contract as trailing cbjump=1 /
// clkset=1 key=value tokens, written only when armed.  This checks default-off
// byte identity at the wire level: an off S line carries neither token (the
// format without the channels), and the armed file differs from the off file by
// those token bytes and nothing else.
TEST(NetworkProductIoTest, ContractRoundTripOptionalDiscChannels) {
    const std::string on  = tempFile("gnss_netprod_channels_on.txt");
    const std::string off = tempFile("gnss_netprod_channels_off.txt");
    std::filesystem::remove(on);
    std::filesystem::remove(off);

    SbasisContract base;
    base.decl.reference_receiver_id = "CIT100USA";
    base.disc_threshold_m           = 0.05;
    base.present                    = true;
    // On: both channels armed.  Off: the same contract with both at the default.
    SbasisContract armed = base;
    armed.code_bias_jump_test      = true;
    armed.clock_disc_on_set_change = true;

    {
        NetworkProductWriter w(on, armed);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));
    }
    {
        NetworkProductWriter w(off, base);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));   // identical epoch content
    }

    // Round-trip: the armed file parses both flags back true...
    bool           ok = false;
    SbasisContract parsed_on;
    NetworkProductReader::read(on, &parsed_on, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed_on.present);
    EXPECT_TRUE(parsed_on.code_bias_jump_test);
    EXPECT_TRUE(parsed_on.clock_disc_on_set_change);

    // ...and the off file parses both back false (absent token = default).
    SbasisContract parsed_off;
    ok = false;
    NetworkProductReader::read(off, &parsed_off, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed_off.present);
    EXPECT_FALSE(parsed_off.code_bias_jump_test);
    EXPECT_FALSE(parsed_off.clock_disc_on_set_change);

    // Byte identity (off): neither token appears on the off file's wire (data
    // lines; the grammar-complete legend names both tokens).
    const std::string off_bytes = readAllBytes(off);
    const std::string off_data  = dataBytes(off);
    EXPECT_EQ(off_data.find("cbjump="), std::string::npos);
    EXPECT_EQ(off_data.find("clkset="), std::string::npos);

    // Byte surgery (on): deleting ",cbjump=1,clkset=1" from the armed file
    // reproduces the off file byte-for-byte, so arming the flags adds those bytes
    // and changes nothing else in the header or the epoch blocks.
    std::string on_bytes = readAllBytes(on);
    const std::string tokens = ",cbjump=1,clkset=1";
    const std::size_t tpos   = on_bytes.find(tokens);
    ASSERT_NE(tpos, std::string::npos);
    on_bytes.erase(tpos, tokens.size());
    EXPECT_EQ(on_bytes, off_bytes);

    std::filesystem::remove(on);
    std::filesystem::remove(off);
}

// The third IsbDatum enumerator (BroadcastAnchoredPerSatellite, the brdc_rts
// broadcast-anchored clock gauge) uses the existing static_cast<int>
// serialization path as wire int 2 and round-trips.  The anchors are the gauge
// in this mode, so the realized clock_pivot_sat list is empty: the S line is the
// 11 positional fields, and the empty pivot list round-trips too (no spurious
// pivot entries on the consumer side).
TEST(NetworkProductIoTest, ContractRoundTripBroadcastAnchoredIsbDatum) {
    const std::string path = tempFile("gnss_netprod_contract_brdcanchor.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
    // No clock_pivot_sat entries: the anchor-free datum publishes no pivots.
    c.apriori_product_id = "BRDC00IGS_RTS";   // non-empty (comma-free) so the S
                                              // line has no trailing empty field
    c.present            = true;

    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.decl.isb_datum,
              SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite);
    EXPECT_EQ(static_cast<int>(parsed.decl.isb_datum), 2);   // the wire int
    EXPECT_TRUE(parsed.decl.clock_pivot_sat.empty());        // empty pivot list round-trips

    // Wire level: the S line's isb_datum positional field (reader f[5]) is "2",
    // and with no pivots and no optional key=value tokens the line is the 11
    // positional fields; enumerator 2 adds no field of its own.
    const std::string content = readAllBytes(path);
    const std::size_t s_pos   = content.find("\nS,");
    ASSERT_NE(s_pos, std::string::npos);
    const std::size_t s_end   = content.find('\n', s_pos + 1);
    ASSERT_NE(s_end, std::string::npos);
    const std::string s_line  = content.substr(s_pos + 1, s_end - s_pos - 1);
    std::vector<std::string> f;
    std::stringstream ss(s_line);
    for (std::string tok; std::getline(ss, tok, ',');) f.push_back(tok);
    ASSERT_EQ(f.size(), 11u);
    EXPECT_EQ(f[5], "2");

    // checkContract: a consumer declaring the same datum passes; one expecting
    // the legacy pin datum is flagged, so the enum participates in the contract.
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, c, &mism));
    EXPECT_TRUE(mism.empty());
    SbasisContract pin = c;
    pin.decl.isb_datum = SbasisDeclaration::IsbDatum::PerConstellationSatClockPin;
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed, pin, &mism));
    EXPECT_FALSE(mism.empty());

    std::filesystem::remove(path);
}

// The ensemble (anchor-free Σ_r z=0) phase-bias datum round-trips through the S
// contract.  The on-disk declaration carries amb_datum (S-line field 4) instead of
// defaulting to the pivot-receiver frame, so a consumer knows the emitted bias
// references the receiver ensemble and is not integer-recoverable (T3b).
TEST(NetworkProductIoTest, ContractRoundTripEnsembleAmbDatum) {
    const std::string path = tempFile("gnss_netprod_contract_ensembleamb.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.amb_datum     = SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
    c.apriori_product_id = "BRDC00IGS_RTS";   // non-empty (comma-free): no trailing empty field
    c.present            = true;

    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.decl.amb_datum,
              SbasisDeclaration::AmbDatum::SumToZeroPerReceiver);
    EXPECT_EQ(static_cast<int>(parsed.decl.amb_datum), 1);   // the wire int

    // Wire level: the S line's amb_datum positional field (reader f[4]) is "1".
    const std::string content = readAllBytes(path);
    const std::size_t s_pos   = content.find("\nS,");
    ASSERT_NE(s_pos, std::string::npos);
    const std::size_t s_end   = content.find('\n', s_pos + 1);
    ASSERT_NE(s_end, std::string::npos);
    const std::string s_line  = content.substr(s_pos + 1, s_end - s_pos - 1);
    std::vector<std::string> f;
    std::stringstream ss(s_line);
    for (std::string tok; std::getline(ss, tok, ',');) f.push_back(tok);
    ASSERT_GE(f.size(), 5u);
    EXPECT_EQ(f[4], "1");

    // checkContract: a consumer declaring the same datum passes; one expecting the
    // legacy pivot datum is flagged, so the enum participates in the contract.
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, c, &mism));
    SbasisContract pivot = c;
    pivot.decl.amb_datum = SbasisDeclaration::AmbDatum::PivotReceiverZeroAmbiguity;
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed, pivot, &mism));
    EXPECT_FALSE(mism.empty());

    std::filesystem::remove(path);
}

// The AmbDatum enumerators are serialized as bare positional integers in S-line
// field 4, so their numeric values are wire format, not an implementation detail:
// reordering the enum re-interprets every product already written (a stored eq34
// file would read back as the ensemble datum, and vice versa).
// `ContractRoundTripEnsembleAmbDatum` pins 1 and the eq34 consumer suite pins 2,
// but only these asserts pin the whole set.  They are compile-time, so they cost
// nothing and cannot be skipped.
//
// Wire-value contract: 0 = PivotReceiverZeroAmbiguity (legacy single-pivot),
// 1 = SumToZeroPerReceiver (Fix A ensemble), 2 = SingleReceiverChain (eq34).
// Append new realizations; do not insert or reorder.
static_assert(static_cast<int>(SbasisDeclaration::AmbDatum::PivotReceiverZeroAmbiguity) == 0,
              "AmbDatum wire value 0 is frozen — it is S-line field 4 on every stored product");
static_assert(static_cast<int>(SbasisDeclaration::AmbDatum::SumToZeroPerReceiver) == 1,
              "AmbDatum wire value 1 is frozen — it is S-line field 4 on every stored product");
static_assert(static_cast<int>(SbasisDeclaration::AmbDatum::SingleReceiverChain) == 2,
              "AmbDatum wire value 2 is frozen — it is S-line field 4 on every stored product");

// The eq34 declaration survives the wire as value 2.  Companion to
// ContractRoundTripEnsembleAmbDatum above, which covers the ensemble datum; this
// test covers the eq34 datum.  The serialized contract has its own eq34 branch,
// separate from the per-solve declaration; without it an eq34 product declares the
// legacy pivot datum it does not use (docs/SPEC_sbasis_realization_contract.md §D).
TEST(NetworkProductIoTest, ContractRoundTripEq34AmbDatum) {
    const std::string path = tempFile("gnss_netprod_contract_eq34amb.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.amb_datum             = SbasisDeclaration::AmbDatum::SingleReceiverChain;
    c.decl.eq34_datum_receiver   = "POHN00FSM";   // pdr=, stamped only under eq34
    c.apriori_product_id         = "BRDC00IGS_RTS";
    c.present                    = true;

    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, /*pb01=*/-0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.decl.amb_datum, SbasisDeclaration::AmbDatum::SingleReceiverChain);
    EXPECT_EQ(static_cast<int>(parsed.decl.amb_datum), 2);
    EXPECT_EQ(parsed.decl.eq34_datum_receiver, "POHN00FSM");

    // A product must not carry pdr= (eq34 datum receiver) while declaring the
    // legacy pivot datum: pdr= and amb_datum agree on the S line.
    const std::string content = readAllBytes(path);
    const std::size_t s_pos   = content.find("\nS,");
    ASSERT_NE(s_pos, std::string::npos);
    const std::string s_line =
        content.substr(s_pos + 1, content.find('\n', s_pos + 1) - s_pos - 1);
    std::vector<std::string> f;
    std::stringstream ss(s_line);
    for (std::string tok; std::getline(ss, tok, ',');) f.push_back(tok);
    ASSERT_GE(f.size(), 5u);
    EXPECT_EQ(f[4], "2");
    EXPECT_NE(s_line.find("pdr=POHN00FSM"), std::string::npos);

    std::filesystem::remove(path);
}

// The optional per-phase-bias σ (B1b) round-trips through the P record as a
// key=value token (sigma=), coexisting with the positional disc field; a bias with
// σ=0 emits no token, so its line is byte-identical to the wire without σ (T3d).
TEST(NetworkProductIoTest, PhaseBiasSigmaRoundTrip) {
    const std::string path = tempFile("gnss_netprod_pbsigma.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch sol;
    sol.epoch_time = kT0;
    NetworkSatSolution g;
    g.sat = "G01";
    NetworkPhaseBias with_sigma;   // σ populated → sigma= token emitted
    with_sigma.bias_m = -0.7; with_sigma.signal_integer = true;
    with_sigma.wide_lane_integer = 1; with_sigma.sigma_m = 0.012345;
    g.phase_bias["L1C"] = with_sigma;
    NetworkPhaseBias no_sigma;      // σ == 0 → no token (off-path byte identity)
    no_sigma.bias_m = 1.5; no_sigma.signal_integer = false; no_sigma.wide_lane_integer = -2;
    g.phase_bias["L2W"] = no_sigma;
    sol.sats.push_back(g);

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }

    const std::string content = dataBytes(path);   // wire probes: data lines only
    EXPECT_NE(content.find("sigma=0.012345"), std::string::npos);   // armed bias carries it
    // The σ==0 L2W bias emits no token: one "sigma=" on the wire.
    EXPECT_EQ(content.find("sigma=", content.find("sigma=") + 1), std::string::npos);

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_FALSE(recs[0].epoch.sats.empty());
    const NetworkSatSolution& g1 = recs[0].epoch.sats[0];
    EXPECT_EQ(g1.sat, "G01");
    EXPECT_NEAR(g1.phase_bias.at("L1C").sigma_m, 0.012345, 1e-9);   // survives round-trip
    EXPECT_DOUBLE_EQ(g1.phase_bias.at("L2W").sigma_m, 0.0);         // absent → default 0
    EXPECT_DOUBLE_EQ(g1.phase_bias.at("L1C").bias_m, -0.7);         // positional fields intact
    EXPECT_EQ(g1.phase_bias.at("L1C").wide_lane_integer, 1);

    std::filesystem::remove(path);
}

// The combined-correction σ tokens (P-record sigc=, C-record sigcc=/rho=) survive
// a write→read round-trip, coexist with the B1b sigma= token on the same P line,
// and emit nothing when their value is 0 (off-path byte identity).  (M4, contract v2)
TEST(NetworkProductIoTest, CombinedSigmaRoundTrip) {
    const std::string path = tempFile("gnss_netprod_combsigma.txt");
    std::filesystem::remove(path);

    NetworkSolutionEpoch sol;
    sol.epoch_time = kT0;

    NetworkSatSolution g1;                    // combined-σ populated → tokens emitted
    g1.sat = "G01"; g1.has_clock = true; g1.clock_c0_m = 12.5;
    g1.sigcc_m = 0.037;                       // C-record sigcc=
    g1.rho     = 0.73;                        // C-record rho=
    NetworkPhaseBias pb1;                     // both sigma= (B1b) and sigc= (M4) on one P line
    pb1.bias_m = -0.7; pb1.signal_integer = true; pb1.wide_lane_integer = 1;
    pb1.sigma_m = 0.011; pb1.sigc_m = 0.0026;
    g1.phase_bias["L1C"] = pb1;
    NetworkPhaseBias pb1b;                    // no combined σ → no sigc= token
    pb1b.bias_m = 1.5; pb1b.signal_integer = false; pb1b.wide_lane_integer = -2;
    g1.phase_bias["L2W"] = pb1b;
    sol.sats.push_back(g1);

    NetworkSatSolution g2;                    // no combined σ at all → no C-record tokens
    g2.sat = "G02"; g2.has_clock = true; g2.clock_c0_m = -8.75;
    g2.phase_bias["L1C"] = NetworkPhaseBias{10.0, true, 1};
    sol.sats.push_back(g2);

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }

    const std::string content = dataBytes(path);   // wire probes: data lines only
    EXPECT_NE(content.find("sigc=0.0026"), std::string::npos);
    EXPECT_NE(content.find("sigcc=0.037"), std::string::npos);
    EXPECT_NE(content.find("rho=0.73"),   std::string::npos);
    // One sigc= / sigcc= / rho= each (G01 armed; G02 and L2W emit none).
    EXPECT_EQ(content.find("sigc=",  content.find("sigc=")  + 1), std::string::npos);
    EXPECT_EQ(content.find("sigcc=", content.find("sigcc=") + 1), std::string::npos);
    EXPECT_EQ(content.find("rho=",   content.find("rho=")   + 1), std::string::npos);

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_EQ(recs[0].epoch.sats.size(), 2u);
    const NetworkSatSolution& r1 = recs[0].epoch.sats[0];
    const NetworkSatSolution& r2 = recs[0].epoch.sats[1];
    ASSERT_EQ(r1.sat, "G01");
    EXPECT_NEAR(r1.sigcc_m, 0.037, 1e-9);
    EXPECT_NEAR(r1.rho,     0.73,  1e-9);
    EXPECT_NEAR(r1.phase_bias.at("L1C").sigc_m,  0.0026, 1e-9);   // M4 token
    EXPECT_NEAR(r1.phase_bias.at("L1C").sigma_m, 0.011,  1e-9);   // B1b token still intact
    EXPECT_EQ(r1.phase_bias.at("L1C").bias_m, -0.7);              // positional fields intact
    EXPECT_DOUBLE_EQ(r1.phase_bias.at("L2W").sigc_m, 0.0);        // absent → default 0
    EXPECT_DOUBLE_EQ(r2.sigcc_m, 0.0);                            // absent → default 0
    EXPECT_DOUBLE_EQ(r2.rho, 0.0);

    std::filesystem::remove(path);
}

// The age-scaling coefficient travels in the S-line agek= token, only when
// non-zero, and round-trips through the v2 header contract (M4, contract v2).
TEST(NetworkProductIoTest, CombinedSigmaAgekRoundTrip) {
    const std::string path  = tempFile("gnss_netprod_agek.txt");
    const std::string pathz = tempFile("gnss_netprod_agek_zero.txt");
    std::filesystem::remove(path);
    std::filesystem::remove(pathz);

    NetworkSolutionEpoch sol;
    sol.epoch_time = kT0;
    NetworkSatSolution g;
    g.sat = "G01"; g.has_clock = true; g.clock_c0_m = 1.0;
    sol.sats.push_back(g);

    SbasisContract contract;
    contract.present = true;
    contract.decl.reference_receiver_id = "REF1";
    contract.age_scale_k = 0.0625;
    {
        NetworkProductWriter w(path, contract);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }
    EXPECT_NE(readAllBytes(path).find("agek=0.0625"), std::string::npos);

    bool ok = false;
    SbasisContract parsed;
    NetworkProductReader::read(path, &parsed, &ok);
    EXPECT_TRUE(ok);
    ASSERT_TRUE(parsed.present);
    EXPECT_NEAR(parsed.age_scale_k, 0.0625, 1e-12);

    // agek == 0 ⇒ no token (byte-identical to a v2 header without M4).
    SbasisContract zero = contract;
    zero.age_scale_k = 0.0;
    {
        NetworkProductWriter w(pathz, zero);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }
    EXPECT_EQ(dataBytes(pathz).find("agek="), std::string::npos);

    std::filesystem::remove(path);
    std::filesystem::remove(pathz);
}

// With cbjump=1 in the contract, makeReplayEmitter arms the opt-in code-bias
// magnitude jump test on the replay emitter.  A 0.2 m value step (same satellite
// set, same band tuple, no datum change, so every structural trigger is silent)
// bumps the replayed store's code-bias discontinuity as it did on the live
// producer.  The same epoch data under a flag-false contract replays with no
// bump: the behaviour travels in the contract, not the epoch records, so a
// consumer needs no out-of-band config (producer datum stability).
TEST(NetworkProductIoTest, ReplayEmitterHonoursJumpTestFlag) {
    auto codeEpoch = [](const GnssTime& t, double bias) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = t; sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
        NetworkSatSolution s;
        s.sat = "G01"; s.has_clock = true; s.clock_c0_m = 1.0;
        s.code_bias_m["C1C"] = bias;
        sol.sats.push_back(s);
        return sol;
    };
    // Two epochs, one sat, one signal: bias 0.10 -> 0.30, a 0.2 m step well
    // above the 0.05 m threshold and invisible to the structural triggers.
    const double biases[] = {0.10, 0.30};

    // The same epoch sequence written under two contracts (flag on / off).
    auto writeProduct = [&](const std::string& path, const SbasisContract& c) {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        GnssTime t = kT0;
        for (double b : biases) {
            w.write(codeEpoch(t, b));
            t.tow += 30.0;
        }
    };

    SbasisContract con_on;
    con_on.decl.reference_receiver_id = "CIT100USA";
    con_on.disc_threshold_m           = 0.05;
    con_on.code_bias_jump_test        = true;
    con_on.present                    = true;
    SbasisContract con_off = con_on;
    con_off.code_bias_jump_test = false;

    const std::string p_on  = tempFile("gnss_netprod_cbjump_on.txt");
    const std::string p_off = tempFile("gnss_netprod_cbjump_off.txt");
    std::filesystem::remove(p_on);
    std::filesystem::remove(p_off);
    writeProduct(p_on,  con_on);
    writeProduct(p_off, con_off);

    // Live reference: an emitter constructed like the armed producer's; the value
    // step bumps the code counter once (0 at first sight, 1 after the jump).
    {
        SsrCorrectionStore    store_live;
        NetworkProductEmitter emit_live(con_on.disc_threshold_m,
                                        /*emit_clock_disc=*/false,
                                        /*staleness_horizon_s=*/0.0,
                                        /*code_bias_jump_test=*/true);
        GnssTime t = kT0;
        for (double b : biases) {
            emit_live.emitToStore(codeEpoch(t, b), store_live);
            t.tow += 30.0;
        }
        EXPECT_EQ(emit_live.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
        ASSERT_TRUE(store_live.getCodeBiasDiscontinuity("G01", "C1C").has_value());
        EXPECT_EQ(*store_live.getCodeBiasDiscontinuity("G01", "C1C"), 1);
    }

    // Armed file: contract -> makeReplayEmitter arms the jump test -> the bump
    // shows in the replayed store, matching the live reference above.
    {
        bool           ok = false;
        SbasisContract parsed;
        const auto recs = NetworkProductReader::read(p_on, &parsed, &ok);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(parsed.present);
        EXPECT_TRUE(parsed.code_bias_jump_test);
        ASSERT_EQ(recs.size(), 2u);
        SsrCorrectionStore    store;
        NetworkProductEmitter emit = NetworkProductReader::makeReplayEmitter(parsed);
        for (const auto& r : recs) NetworkProductReader::replay(r, store, emit);
        EXPECT_EQ(emit.codeBiasDiscontinuityCounter("G01", "C1C"), 1);
        const auto d = store.getCodeBiasDiscontinuity("G01", "C1C");
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(*d, 1);   // the replayed store carries the bump
    }

    // Off file: the same records under a flag-false contract -> no bump on the
    // emitter or in the store (the counter exists from the first sighting but
    // stays 0).
    {
        bool           ok = false;
        SbasisContract parsed;
        const auto recs = NetworkProductReader::read(p_off, &parsed, &ok);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(parsed.present);
        EXPECT_FALSE(parsed.code_bias_jump_test);
        ASSERT_EQ(recs.size(), 2u);
        SsrCorrectionStore    store;
        NetworkProductEmitter emit = NetworkProductReader::makeReplayEmitter(parsed);
        for (const auto& r : recs) NetworkProductReader::replay(r, store, emit);
        EXPECT_EQ(emit.codeBiasDiscontinuityCounter("G01", "C1C"), 0);
        const auto d = store.getCodeBiasDiscontinuity("G01", "C1C");
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(*d, 0);   // steady counter: no discontinuity signalled
    }

    std::filesystem::remove(p_on);
    std::filesystem::remove(p_off);
}

// The optional solution-generation id round-trips as an I record, reaches the
// replayed store's epoch surface, and is deterministic across replays of the same
// file (the id comes from the file, not from process state).  A file written
// without it carries no I record, so its format is unchanged (RTA-3).
TEST(NetworkProductIoTest, GenerationIdRoundTripsAndReplaysDeterministically) {
    const std::string path = tempFile("gnss_netprod_genid.txt");
    std::filesystem::remove(path);

    constexpr uint32_t kGid = 1399161600u;   // week·604800 + ⌊tow⌋ of the first emit
    NetworkSolutionEpoch e1 = makeEpoch(kT0, /*pb01=*/-0.7);
    e1.generation_id = kGid;
    GnssTime t2 = kT0; t2.tow += 30.0;
    NetworkSolutionEpoch e2 = makeEpoch(t2, /*pb01=*/-0.7);
    e2.generation_id = kGid;                 // constant for the whole run

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(e1);
        w.write(e2);
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0].epoch.generation_id, kGid);
    EXPECT_EQ(recs[1].epoch.generation_id, kGid);

    // Replay determinism: two independent replays of the same file produce the
    // same store-level generation id.
    for (int pass = 0; pass < 2; ++pass) {
        SsrCorrectionStore    store;
        NetworkProductEmitter emit;
        for (const auto& r : recs) NetworkProductReader::replay(r, store, emit);
        ASSERT_TRUE(store.getGenerationId().has_value()) << "pass " << pass;
        EXPECT_EQ(*store.getGenerationId(), kGid) << "pass " << pass;
    }

    // Byte level: one I line per stamped epoch.
    std::string content;
    {
        std::ifstream in(path);
        content.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    }
    std::size_t count = 0, pos = 0;
    while ((pos = content.find("\nI,", pos)) != std::string::npos) { ++count; pos += 3; }
    EXPECT_EQ(count, 2u);
    std::filesystem::remove(path);

    // Off path: an unstamped run writes no I record (byte-identical format).
    const std::string off = tempFile("gnss_netprod_genid_off.txt");
    std::filesystem::remove(off);
    { NetworkProductWriter w(off); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, -0.7)); }
    {
        std::ifstream in(off);
        content.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(content.find("\nI,"), std::string::npos);
    bool ok_off = false;
    const auto recs_off = NetworkProductReader::read(off, &ok_off);
    ASSERT_TRUE(ok_off);
    ASSERT_EQ(recs_off.size(), 1u);
    EXPECT_EQ(recs_off[0].epoch.generation_id, 0u);   // absent -> unset
    std::filesystem::remove(off);
}

// With serialize_disc_counters armed, the writer appends the post-emit counter
// values queried from the emitter that just pushed the epoch, so per epoch they
// equal what the live store received.  The reader parses them back, a full-file
// replay recompute agrees with them (self-check silent, no adoption), and the
// replayed store round-trips byte-for-byte (RTA-4).
TEST(NetworkProductIoTest, SerializedDiscCountersMatchStoreAndReplay) {
    const std::string path = tempFile("gnss_netprod_disc_vals.txt");
    std::filesystem::remove(path);

    // e1 first sight (ctr 0), e2 forced datum change (phase/code/clock ctr 1),
    // e3 phase jump -0.71 -> +0.20 (phase ctr 2; code/clock stay 1).
    struct Step { double pb01; bool dc; };
    const Step steps[] = { {-0.70, false}, {-0.71, true}, {0.20, false} };

    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(0.05, /*emit_clock_disc=*/true);
    std::vector<int> live_phase_ctr;   // post-emit counter per epoch (the wire contract)
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        for (const Step& s : steps) {
            const NetworkSolutionEpoch sol = makeEpoch(t, s.pb01);
            if (s.dc) emit_live.markDatumChange();
            emit_live.emitToStore(sol, store_live);
            w.write(sol, s.dc, {}, {}, &emit_live);   // counters post-emit (RTA-4)
            live_phase_ctr.push_back(emit_live.discontinuityCounter("G01", "L1C"));
            t.tow += 30.0;
        }
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 3u);

    // Every epoch's serialized phase counter equals the live post-emit counter...
    for (std::size_t i = 0; i < recs.size(); ++i) {
        ASSERT_TRUE(recs[i].serialized_disc.any()) << "epoch " << i;
        ASSERT_EQ(recs[i].serialized_disc.phase.count("G01|L1C"), 1u) << "epoch " << i;
        EXPECT_EQ(recs[i].serialized_disc.phase.at("G01|L1C"), live_phase_ctr[i])
            << "epoch " << i;
    }
    // ...and the final epoch's serialized values equal the live store record
    // values on all three channels (the emitToStore == wire equivalence).
    const auto& last = recs.back().serialized_disc;
    ASSERT_TRUE(store_live.getPhaseBias("G01", "L1C").has_value());
    EXPECT_EQ(last.phase.at("G01|L1C"),
              store_live.getPhaseBias("G01", "L1C")->discontinuity_counter);
    ASSERT_TRUE(store_live.getCodeBiasDiscontinuity("G01", "C1W").has_value());
    EXPECT_EQ(last.code.at("G01|C1W"),
              *store_live.getCodeBiasDiscontinuity("G01", "C1W"));
    ASSERT_TRUE(store_live.getClockDiscontinuity("G01").has_value());
    EXPECT_EQ(last.clock.at("G01"), *store_live.getClockDiscontinuity("G01"));
    EXPECT_EQ(last.phase.at("G01|L1C"), 2);
    EXPECT_EQ(last.code.at("G01|C1W"), 1);
    EXPECT_EQ(last.clock.at("G01"), 1);

    // Full-file replay: the recompute agrees with the serialized values (the
    // self-check never adopts) and the store reproduces byte-for-byte.
    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk(0.05, /*emit_clock_disc=*/true);
    for (const auto& r : recs) NetworkProductReader::replay(r, store_disk, emit_disk);
    EXPECT_EQ(emit_disk.discontinuityCounter("G01", "L1C"), 2);
    EXPECT_EQ(store_disk.getPhaseBias("G01", "L1C")->discontinuity_counter,
              store_live.getPhaseBias("G01", "L1C")->discontinuity_counter);
    EXPECT_EQ(*store_disk.getCodeBiasDiscontinuity("G01", "C1W"),
              *store_live.getCodeBiasDiscontinuity("G01", "C1W"));
    EXPECT_EQ(store_disk.getClockDiscontinuity("G01").value_or(-1),
              store_live.getClockDiscontinuity("G01").value_or(-2));

    std::filesystem::remove(path);
}

// A tail reader (joining mid-file) recomputes 0 from a first sighting, while the
// serialized values carry the producer's counters.  Replay detects the
// disagreement and adopts the file's values, and the reconstructed store records
// carry them without a replay from epoch 1 (RTA-4).
TEST(NetworkProductIoTest, SerializedDiscCountersAdoptedOnTailJoin) {
    const std::string path = tempFile("gnss_netprod_disc_tail.txt");
    std::filesystem::remove(path);

    struct Step { double pb01; bool dc; };
    const Step steps[] = { {-0.70, false}, {-0.71, true}, {0.20, false} };
    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(0.05, /*emit_clock_disc=*/true);
    GnssTime t = kT0;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        for (const Step& s : steps) {
            const NetworkSolutionEpoch sol = makeEpoch(t, s.pb01);
            if (s.dc) emit_live.markDatumChange();
            emit_live.emitToStore(sol, store_live);
            w.write(sol, s.dc, {}, {}, &emit_live);
            t.tow += 30.0;
        }
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 3u);

    // Tail join: replay only the last record with a fresh emitter.  The recompute
    // sees a first sighting (0 everywhere); adoption installs the file's values.
    SsrCorrectionStore    store_tail;
    NetworkProductEmitter emit_tail(0.05, /*emit_clock_disc=*/true);
    NetworkProductReader::replay(recs.back(), store_tail, emit_tail);
    EXPECT_EQ(emit_tail.discontinuityCounter("G01", "L1C"), 2);
    EXPECT_EQ(emit_tail.codeBiasDiscontinuityCounter("G01", "C1W"), 1);
    EXPECT_EQ(emit_tail.clockDiscontinuityCounter("G01"), 1);
    ASSERT_TRUE(store_tail.getPhaseBias("G01", "L1C").has_value());
    EXPECT_EQ(store_tail.getPhaseBias("G01", "L1C")->discontinuity_counter, 2);
    EXPECT_EQ(store_tail.getCodeBiasDiscontinuity("G01", "C1W").value_or(-1), 1);
    EXPECT_EQ(store_tail.getClockDiscontinuity("G01").value_or(-1), 1);
    // ...matching the live producer's store exactly.
    EXPECT_EQ(store_tail.getPhaseBias("G01", "L1C")->discontinuity_counter,
              store_live.getPhaseBias("G01", "L1C")->discontinuity_counter);

    // Off path: a file written without the counters (disc_source null) keeps the
    // field counts without them, and no serialized_disc content parses back.
    const std::string off = tempFile("gnss_netprod_disc_off.txt");
    std::filesystem::remove(off);
    { NetworkProductWriter w(off); ASSERT_TRUE(w.good()); w.write(makeEpoch(kT0, -0.7)); }
    bool ok_off = false;
    const auto recs_off = NetworkProductReader::read(off, &ok_off);
    ASSERT_TRUE(ok_off);
    ASSERT_EQ(recs_off.size(), 1u);
    EXPECT_FALSE(recs_off[0].serialized_disc.any());
    std::filesystem::remove(off);

    std::filesystem::remove(path);
}

// Repeated S lines are accepted positionally (RTA-5).  Each receipt updates the
// reader's current contract state (contract_out ends as the last one), is
// surfaced per epoch through contract_refresh so a consumer can run
// checkContract on every receipt, and never feeds the replay emitter (replay of
// the same epochs with and without the re-broadcast is identical).  The RTA-3
// gid= trailing field round-trips through the S grammar.
TEST(NetworkProductIoTest, RepeatedSLinesUpdateContractPositionally) {
    const std::string path = tempFile("gnss_netprod_srebroadcast.txt");
    const std::string twin = tempFile("gnss_netprod_srebroadcast_twin.txt");
    std::filesystem::remove(path);
    std::filesystem::remove(twin);

    SbasisContract a;
    a.decl.reference_receiver_id = "CIT100USA";
    a.decl.clock_pivot_sat["G"]  = "G05";
    a.disc_threshold_m           = 0.05;
    a.emit_clock_disc            = true;
    a.generation_id              = 777u;   // gid= travels in the S line (RTA-3)
    a.present                    = true;

    SbasisContract b = a;
    b.decl.clock_pivot_sat["G"] = "G07";   // refreshed realized pivot post-handover

    GnssTime t2 = kT0; t2.tow += 30.0;
    {
        NetworkProductWriter w(path, a);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.writeContract(b);                 // mid-file re-broadcast (RTA-5)
        w.write(makeEpoch(t2, -0.7));
    }
    {   // Twin without the re-broadcast: replay must be byte-identical to it.
        NetworkProductWriter w(twin, a);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.write(makeEpoch(t2, -0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    // Reader's final contract state is the last S line.
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.decl.clock_pivot_sat.at("G"), "G07");
    EXPECT_EQ(parsed.generation_id, 777u);
    // Positional per-epoch receipts: header S -> epoch 1, re-broadcast -> epoch 2.
    ASSERT_TRUE(recs[0].contract_refresh.present);
    EXPECT_EQ(recs[0].contract_refresh.decl.clock_pivot_sat.at("G"), "G05");
    ASSERT_TRUE(recs[1].contract_refresh.present);
    EXPECT_EQ(recs[1].contract_refresh.decl.clock_pivot_sat.at("G"), "G07");
    // Per-receipt checkContract with a pivot-less expectation (pivots evolve;
    // convention fields and the run's single generation id are run-constant).
    SbasisContract expect = a;
    expect.decl.clock_pivot_sat.clear();
    for (const auto& r : recs) {
        std::vector<std::string> mism;
        EXPECT_TRUE(NetworkProductReader::checkContract(r.contract_refresh, expect, &mism))
            << (mism.empty() ? "" : mism.front());
    }

    // Replay equivalence: the extra S line feeds nothing into the emitter.
    bool ok_twin = false;
    const auto recs_twin = NetworkProductReader::read(twin, &ok_twin);
    ASSERT_TRUE(ok_twin);
    ASSERT_EQ(recs_twin.size(), 2u);
    SsrCorrectionStore s1, s2;
    NetworkProductEmitter e1 = NetworkProductReader::makeReplayEmitter(parsed);
    NetworkProductEmitter e2 = NetworkProductReader::makeReplayEmitter(parsed);
    for (const auto& r : recs)      NetworkProductReader::replay(r, s1, e1);
    for (const auto& r : recs_twin) NetworkProductReader::replay(r, s2, e2);
    EXPECT_EQ(e1.discontinuityCounter("G01", "L1C"), e2.discontinuityCounter("G01", "L1C"));
    EXPECT_DOUBLE_EQ(s1.getPhaseBias("G01", "L1C")->bias_m,
                     s2.getPhaseBias("G01", "L1C")->bias_m);
    EXPECT_EQ(s1.getPhaseBias("G01", "L1C")->discontinuity_counter,
              s2.getPhaseBias("G01", "L1C")->discontinuity_counter);

    std::filesystem::remove(path);
    std::filesystem::remove(twin);
}

// The staleness horizon travels in the contract (stale= field), so
// makeReplayEmitter reproduces the horizon-armed counters from the file alone; a
// default contract (horizon 0) replays with the legacy keep-forever baseline,
// and checkContract flags the construction divergence (RTA-11).
TEST(NetworkProductIoTest, StalenessHorizonTravelsInContract) {
    const std::string path = tempFile("gnss_netprod_stale.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;
    c.staleness_horizon_s        = 100.0;   // replay-emitter parameter (RTA-11)
    c.present                    = true;

    // e1, then a 200 s (> horizon) gap into e2 whose phase bias moved by ~4.9 m:
    // with the horizon armed this is a first sighting (no bump).
    GnssTime t2 = kT0; t2.tow += 200.0;
    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(c.disc_threshold_m, c.emit_clock_disc,
                                    c.staleness_horizon_s);
    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        const NetworkSolutionEpoch e1 = makeEpoch(kT0, 0.10);
        const NetworkSolutionEpoch e2 = makeEpoch(t2, 5.00);
        emit_live.emitToStore(e1, store_live);
        w.write(e1);
        emit_live.emitToStore(e2, store_live);
        w.write(e2);
    }
    EXPECT_EQ(emit_live.discontinuityCounter("G01", "L1C"), 0);   // horizon suppressed

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(parsed.present);
    EXPECT_DOUBLE_EQ(parsed.staleness_horizon_s, 100.0);

    // Replay from the file alone reproduces the horizon-armed counters.
    SsrCorrectionStore    store_disk;
    NetworkProductEmitter emit_disk = NetworkProductReader::makeReplayEmitter(parsed);
    for (const auto& r : recs) NetworkProductReader::replay(r, store_disk, emit_disk);
    EXPECT_EQ(emit_disk.discontinuityCounter("G01", "L1C"), 0);
    EXPECT_EQ(store_disk.getPhaseBias("G01", "L1C")->discontinuity_counter, 0);

    // checkContract passes against the matching expectation and flags a
    // horizon divergence (different replay-emitter construction).
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, c, &mism));
    SbasisContract other = c;
    other.staleness_horizon_s = 0.0;
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed, other, &mism));
    EXPECT_FALSE(mism.empty());

    // Control: a default (horizon-0) emitter replaying the same records produces
    // the legacy spurious bump, the divergence the contract field prevents.
    SsrCorrectionStore    store_v1;
    NetworkProductEmitter emit_v1(c.disc_threshold_m, c.emit_clock_disc);
    for (const auto& r : recs) NetworkProductReader::replay(r, store_v1, emit_v1);
    EXPECT_EQ(emit_v1.discontinuityCounter("G01", "L1C"), 1);

    std::filesystem::remove(path);
}

// The contract identity fields (frame=/cepoch=/atx=/oref=) round-trip through
// the S grammar, in the v2 header and in the RTA-5 mid-file re-broadcast (shared
// writeSLine_), and are absent from the S line when empty, so a producer that
// declares none of them writes the header unchanged byte-for-byte (RTA-12).
TEST(NetworkProductIoTest, IdentityFieldsTravelInContractAndAreOptional) {
    const std::string path = tempFile("gnss_netprod_identity.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.clock_pivot_sat["G"]  = "G05";
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;
    c.frame_id                   = "IGS20";      // identity fields (RTA-12)
    c.coord_epoch                = "2024.5";
    c.atx_id                     = "igs20.atx";
    c.orbit_ref                  = "CoM";
    c.present                    = true;

    GnssTime t2 = kT0; t2.tow += 30.0;
    {
        NetworkProductWriter w(path, c);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.writeContract(c);              // re-broadcast, same grammar (RTA-5)
        w.write(makeEpoch(t2, -0.7));
    }

    bool           ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path, &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.frame_id,    "IGS20");
    EXPECT_EQ(parsed.coord_epoch, "2024.5");
    EXPECT_EQ(parsed.atx_id,      "igs20.atx");
    EXPECT_EQ(parsed.orbit_ref,   "CoM");
    // The realized-pivot trailing entry still parses next to the key=value fields.
    EXPECT_EQ(parsed.decl.clock_pivot_sat.at("G"), "G05");
    // The mid-file re-broadcast surfaces them per epoch too (contract_refresh).
    ASSERT_TRUE(recs[1].contract_refresh.present);
    EXPECT_EQ(recs[1].contract_refresh.frame_id,  "IGS20");
    EXPECT_EQ(recs[1].contract_refresh.orbit_ref, "CoM");
    std::filesystem::remove(path);

    // Absent-when-empty: a contract with no identity declarations writes an S
    // line without any of the four tokens, byte-identical to the format without
    // RTA-12 (the gid=/stale= optionality pattern).
    const std::string off = tempFile("gnss_netprod_identity_off.txt");
    std::filesystem::remove(off);
    SbasisContract bare = c;
    bare.frame_id.clear();
    bare.coord_epoch.clear();
    bare.atx_id.clear();
    bare.orbit_ref.clear();
    {
        NetworkProductWriter w(off, bare);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
    }
    const std::string content = dataBytes(off);    // wire probes: data lines only
    EXPECT_EQ(content.find("frame="),  std::string::npos);
    EXPECT_EQ(content.find("cepoch="), std::string::npos);
    EXPECT_EQ(content.find("atx="),    std::string::npos);
    EXPECT_EQ(content.find("oref="),   std::string::npos);
    bool           ok_off = false;
    SbasisContract parsed_off;
    NetworkProductReader::read(off, &parsed_off, &ok_off);
    ASSERT_TRUE(ok_off);
    ASSERT_TRUE(parsed_off.present);
    EXPECT_TRUE(parsed_off.frame_id.empty());     // absent -> unset
    EXPECT_TRUE(parsed_off.coord_epoch.empty());
    EXPECT_TRUE(parsed_off.atx_id.empty());
    EXPECT_TRUE(parsed_off.orbit_ref.empty());
    std::filesystem::remove(off);
}

// checkContract compares each identity field only when the consumer expectation
// declares it non-empty (the apriori_product_id semantics): an undeclared field
// always passes (also against a file without identity fields), a declared match
// passes, and each declared mismatch fails with a per-field message.  A declared
// field against a file that does not carry it is a mismatch (RTA-12).
TEST(NetworkProductIoTest, IdentityFieldsCheckedOnlyWhenDeclared) {
    SbasisContract file;
    file.decl.reference_receiver_id = "CIT100USA";
    file.disc_threshold_m           = 0.05;
    file.frame_id                   = "IGS20";
    file.coord_epoch                = "2024.5";
    file.atx_id                     = "igs20.atx";
    file.orbit_ref                  = "CoM";
    file.present                    = true;

    // Undeclared: expectation leaves every identity field empty -> compatible,
    // even though the file carries values.
    SbasisContract undeclared = file;
    undeclared.frame_id.clear();
    undeclared.coord_epoch.clear();
    undeclared.atx_id.clear();
    undeclared.orbit_ref.clear();
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(file, undeclared, &mism));
    EXPECT_TRUE(mism.empty());

    // Declared match: pinning the file's own values passes.
    mism.clear();
    EXPECT_TRUE(NetworkProductReader::checkContract(file, file, &mism));
    EXPECT_TRUE(mism.empty());

    // Declared mismatch, one field at a time: one message, naming the field.
    const struct { std::string SbasisContract::* member; const char* tag; } cases[] = {
        { &SbasisContract::frame_id,    "frame_id"    },
        { &SbasisContract::coord_epoch, "coord_epoch" },
        { &SbasisContract::atx_id,      "atx_id"      },
        { &SbasisContract::orbit_ref,   "orbit_ref"   },
    };
    for (const auto& cse : cases) {
        SbasisContract expect = file;
        expect.*(cse.member) = "SOMETHING_ELSE";
        mism.clear();
        EXPECT_FALSE(NetworkProductReader::checkContract(file, expect, &mism)) << cse.tag;
        ASSERT_EQ(mism.size(), 1u) << cse.tag;
        EXPECT_NE(mism[0].find(cse.tag), std::string::npos) << mism[0];
    }

    // Declared against a file that does not carry the field (a producer without
    // RTA-12): the pin reports a mismatch instead of passing.
    SbasisContract bare_file = file;
    bare_file.atx_id.clear();
    SbasisContract pin = undeclared;
    pin.atx_id = "igs20.atx";
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(bare_file, pin, &mism));
    ASSERT_EQ(mism.size(), 1u);
    EXPECT_NE(mism[0].find("atx_id"), std::string::npos);
}

// flush() makes the just-written epoch visible on disk while the writer is still
// open (the property a live tail depends on), and a flushed run's final bytes
// equal those of an unflushed twin: flushing changes when bytes reach disk, not
// the bytes (RTA-6(a)).
TEST(NetworkProductIoTest, WriterFlushPerEpochChangesNoBytes) {
    const std::string a = tempFile("gnss_netprod_flush_a.txt");
    const std::string b = tempFile("gnss_netprod_flush_b.txt");
    std::filesystem::remove(a);
    std::filesystem::remove(b);

    GnssTime t2 = kT0; t2.tow += 30.0;
    {
        NetworkProductWriter w(a);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.flush();
        // Visible now, with the writer still open: a live tail can read the epoch block.
        const std::string visible = readAllBytes(a);
        EXPECT_NE(visible.find("\nE,"), std::string::npos);
        w.write(makeEpoch(t2, -0.7));
        w.flush();
    }
    {
        NetworkProductWriter w(b);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.write(makeEpoch(t2, -0.7));
    }
    EXPECT_EQ(readAllBytes(a), readAllBytes(b));   // byte-identical with/without flush

    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

// The tail consumes a growing file incrementally: complete lines only (a torn
// line is buffered until its newline arrives), an epoch completes when the next
// epoch's E line lands, and every epoch is delivered exactly once (RTA-6(b)).
TEST(NetworkProductTailTest, DeliversEpochsIncrementallyExactlyOnce) {
    const std::string scratch = tempFile("gnss_netprod_tail_scratch.txt");
    const std::string path    = tempFile("gnss_netprod_tail_live.txt");
    std::filesystem::remove(scratch);
    std::filesystem::remove(path);

    // Capture the full 4-epoch file up front so it can be re-served to the
    // tailed path in controlled chunks, including mid-line splits.
    {
        NetworkProductWriter w(scratch);
        ASSERT_TRUE(w.good());
        for (int k = 0; k < 4; ++k) {
            GnssTime t = kT0; t.tow += 30.0 * k;
            w.write(makeEpoch(t, -0.7));
        }
    }
    const std::string content = readAllBytes(scratch);

    // Locate the data E lines (line-initial "E,"; every header/legend line
    // starts with '#', so it cannot false-match).
    std::vector<std::size_t> e_pos;
    for (std::size_t p = content.find("\nE,"); p != std::string::npos;
         p = content.find("\nE,", p + 1))
        e_pos.push_back(p + 1);   // index of the 'E'
    ASSERT_EQ(e_pos.size(), 4u);

    NetworkProductTail tail(path);
    EXPECT_TRUE(tail.poll().empty());   // file does not exist yet: quiet no-op

    // Chunk 1: header, all of epoch 1, and a partial epoch-2 E line ("E,23...").
    // Epoch 1 is not yet complete (its terminating E line has not fully arrived)
    // and the torn line is buffered, not parsed.
    appendBytes(path, content.substr(0, e_pos[1] + 4));
    EXPECT_TRUE(tail.poll().empty());
    EXPECT_TRUE(tail.healthy());

    // Chunk 2: the rest of epoch 2's E line and epoch 2's body.  The completed E
    // line terminates epoch 1, which is delivered once with every field intact.
    appendBytes(path, content.substr(e_pos[1] + 4, e_pos[2] - (e_pos[1] + 4)));
    {
        const auto recs = tail.poll();
        ASSERT_EQ(recs.size(), 1u);
        EXPECT_DOUBLE_EQ(recs[0].epoch.epoch_time.tow, kT0.tow);
        ASSERT_EQ(recs[0].epoch.sats.size(), 2u);
        EXPECT_DOUBLE_EQ(recs[0].epoch.sats[0].clock_c0_m, 12.5);
        ASSERT_EQ(recs[0].epoch.sats[0].phase_bias.count("L1C"), 1u);
        EXPECT_DOUBLE_EQ(recs[0].epoch.sats[0].phase_bias.at("L1C").bias_m, -0.7);
    }

    // Chunk 3: epochs 3 and 4 in one burst.  Epochs 2 and 3 complete; epoch 4
    // stays pending until a successor E line arrives (live-tail semantics).
    appendBytes(path, content.substr(e_pos[2]));
    {
        const auto recs = tail.poll();
        ASSERT_EQ(recs.size(), 2u);
        EXPECT_DOUBLE_EQ(recs[0].epoch.epoch_time.tow, kT0.tow + 30.0);
        EXPECT_DOUBLE_EQ(recs[1].epoch.epoch_time.tow, kT0.tow + 60.0);
    }

    EXPECT_TRUE(tail.poll().empty());   // no new bytes -> nothing re-delivered
    EXPECT_TRUE(tail.healthy());

    std::filesystem::remove(scratch);
    std::filesystem::remove(path);
}

// Mid-file join (RTA-6(b)): a tail that starts at the current end of file, having
// missed the v2 header and earlier epochs, (i) picks up the contract from the
// next S line (RTA-5 re-broadcast), (ii) adopts discontinuity-counter values from
// the wire (RTA-4, serialize_disc_counters) instead of replaying from the start,
// and (iii) surfaces the RTA-3 generation id per epoch.
TEST(NetworkProductTailTest, MidFileJoinAdoptsCountersAndPicksUpContract) {
    const std::string path = tempFile("gnss_netprod_tail_join.txt");
    std::filesystem::remove(path);

    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;
    c.generation_id              = 777u;
    c.present                    = true;

    // Live producer: e1 first sight, e2 forced datum change (phase/code/clock
    // ctr 1), then post-join e3 with a > 0.05 m phase jump (phase ctr 2), e4.
    struct Step { double pb01; bool dc; };
    SsrCorrectionStore    store_live;
    NetworkProductEmitter emit_live(c.disc_threshold_m, c.emit_clock_disc);
    GnssTime t = kT0;

    auto w = std::make_unique<NetworkProductWriter>(path, c);
    ASSERT_TRUE(w->good());
    auto produce = [&](const Step& s) {
        NetworkSolutionEpoch sol = makeEpoch(t, s.pb01);
        sol.generation_id = 777u;                       // id on every epoch (RTA-3)
        if (s.dc) emit_live.markDatumChange();
        emit_live.emitToStore(sol, store_live);
        w->write(sol, s.dc, {}, {}, &emit_live);        // counters on the wire (RTA-4)
        t.tow += 30.0;
    };
    produce({-0.70, false});
    produce({-0.71, true});
    w->flush();   // pre-join content on disk

    // Consumer (re)start: join at the current end; header S and epochs 1-2 unseen.
    NetworkProductTail tail(path, /*start_at_end=*/true);
    SsrCorrectionStore store_tail;
    EXPECT_TRUE(tail.pollInto(store_tail).empty());
    EXPECT_FALSE(tail.contract().present);

    // Producer continues: RTA-5 contract re-broadcast, then two more epochs.
    w->writeContract(c);
    produce({0.20, false});
    produce({0.20, false});
    w->flush();

    const auto recs = tail.pollInto(store_tail);
    ASSERT_EQ(recs.size(), 1u);            // epoch 3 (epoch 4 pending)
    // (i) contract from the next S line, stamped on the joined epoch too.
    ASSERT_TRUE(tail.contract().present);
    EXPECT_EQ(tail.contract().decl.reference_receiver_id, "CIT100USA");
    EXPECT_EQ(tail.contract().generation_id, 777u);
    EXPECT_TRUE(tail.contract().emit_clock_disc);
    ASSERT_TRUE(recs[0].contract_refresh.present);
    // (iii) generation id surfaces per epoch.
    EXPECT_EQ(recs[0].epoch.generation_id, 777u);
    // (ii) adopted counter values: a fresh tail emitter recomputes 0 (first
    // sighting), but the wire carries the producer's post-emit values.
    ASSERT_TRUE(store_tail.getPhaseBias("G01", "L1C").has_value());
    EXPECT_EQ(store_tail.getPhaseBias("G01", "L1C")->discontinuity_counter, 2);
    EXPECT_EQ(store_tail.getPhaseBias("G01", "L1C")->discontinuity_counter,
              store_live.getPhaseBias("G01", "L1C")->discontinuity_counter);
    EXPECT_EQ(store_tail.getCodeBiasDiscontinuity("G01", "C1W").value_or(-1),
              store_live.getCodeBiasDiscontinuity("G01", "C1W").value_or(-2));
    EXPECT_EQ(store_tail.getClockDiscontinuity("G01").value_or(-1),
              store_live.getClockDiscontinuity("G01").value_or(-2));
    EXPECT_EQ(tail.epochsDelivered(), 1u);
    EXPECT_TRUE(tail.healthy());

    w.reset();   // close before remove: Windows locks an open file
    std::filesystem::remove(path);
}

// Truncation/rotation (RTA-6(b)): a file-size shrink (a producer restart
// truncated the file) makes the tail warn, reopen from the start with fresh parse
// state, and deliver the new run's epochs.
TEST(NetworkProductTailTest, TruncationReopensFromStart) {
    const std::string path = tempFile("gnss_netprod_tail_rot.txt");
    std::filesystem::remove(path);

    {   // Run A: four epochs.
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        GnssTime t = kT0;
        for (int k = 0; k < 4; ++k) { w.write(makeEpoch(t, -0.7)); t.tow += 30.0; }
    }
    NetworkProductTail tail(path);
    SsrCorrectionStore store;
    EXPECT_EQ(tail.pollInto(store).size(), 3u);   // epochs 1-3 (epoch 4 pending)

    {   // Producer restart: NetworkProductWriter truncates and writes a shorter run.
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        GnssTime t = kT0; t.tow += 7200.0;
        w.write(makeEpoch(t, 0.1));
        t.tow += 30.0;
        w.write(makeEpoch(t, 0.1));
    }
    const auto recs = tail.pollInto(store);   // shrink detected -> reopen from start
    ASSERT_EQ(recs.size(), 1u);               // new run's epoch 1 (epoch 2 pending)
    EXPECT_DOUBLE_EQ(recs[0].epoch.epoch_time.tow, kT0.tow + 7200.0);
    EXPECT_TRUE(tail.healthy());
    EXPECT_EQ(tail.epochsDelivered(), 1u);    // reset with the reopen

    std::filesystem::remove(path);
}

TEST(NetworkProductIoTest, MissingFileReportsNotOk) {
    bool ok = true;
    std::vector<NetworkProductRecord> recs =
        NetworkProductReader::read(tempFile("gnss_netprod_does_not_exist.txt"), &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(recs.empty());
}

// ---------------------------------------------------------------------------
// Product-level epoch-status heartbeat (H record,
// network.emit_epoch_status_record; RTA-10).
// ---------------------------------------------------------------------------

namespace {
/// An RTA-10 heartbeat for @p t (helper for the H-record tests).
NetworkProducerStatus makeStatus(const GnssTime& t, NetworkProducerStatus::State state,
                                 double sigma0, int n_sats, const std::string& withheld) {
    NetworkProducerStatus st;
    st.time             = t;
    st.state            = state;
    st.sigma0           = sigma0;
    st.n_sats_published = n_sats;
    st.withheld         = withheld;
    return st;
}
/// Count line-initial occurrences of "H," in @p content (all data lines are
/// newline-preceded, because the file starts with a '#' banner line).
std::size_t countHLines(const std::string& content) {
    std::size_t n = 0;
    for (std::size_t p = content.find("\nH,"); p != std::string::npos;
         p = content.find("\nH,", p + 1))
        ++n;
    return n;
}
} // namespace

// The armed writer emits one H per epoch with the documented grammar.  An emitted
// epoch's own H (matching week/tow, written right before its E line) attaches to
// that epoch's record, while a fully withheld epoch is an H with no E line that
// attaches to nothing.  A writer not asked for the heartbeat emits no H, and
// stripping the H lines from the armed file reproduces the off file's bytes
// exactly, so the H record is purely additive.
TEST(NetworkProductIoTest, EpochStatusHeartbeatRoundTripsAndIsPurelyAdditive) {
    const std::string path = tempFile("gnss_netprod_hbeat.txt");
    const std::string twin = tempFile("gnss_netprod_hbeat_twin.txt");
    std::filesystem::remove(path);
    std::filesystem::remove(twin);

    GnssTime t2 = kT0; t2.tow += 30.0;   // fully withheld epoch (no E line)
    GnssTime t3 = kT0; t3.tow += 60.0;   // partial epoch
    const auto h1 = makeStatus(kT0, NetworkProducerStatus::State::Ok, 1.2345, 2, "-");
    const auto h2 = makeStatus(t2, NetworkProducerStatus::State::WithheldSigma0, 9.87, 0, "*");
    const auto h3 = makeStatus(t3, NetworkProducerStatus::State::Partial, 5.5, 2, "C;R");

    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.writeEpochStatus(h1);
        w.write(makeEpoch(kT0, -0.7));
        w.writeEpochStatus(h2);              // withheld: H, no E
        w.writeEpochStatus(h3);
        w.write(makeEpoch(t3, -0.7));
    }
    {   // Off twin: identical epochs, heartbeat never requested.
        NetworkProductWriter w(twin);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.write(makeEpoch(t3, -0.7));
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);                        // H is a known tag (the reader parses it)
    ASSERT_EQ(recs.size(), 2u);             // the withheld H creates no record

    ASSERT_TRUE(recs[0].epoch_status.has_value());
    EXPECT_EQ(recs[0].epoch_status->state, NetworkProducerStatus::State::Ok);
    EXPECT_DOUBLE_EQ(recs[0].epoch_status->sigma0, 1.2345);
    EXPECT_EQ(recs[0].epoch_status->n_sats_published, 2);
    EXPECT_EQ(recs[0].epoch_status->withheld, "-");
    EXPECT_EQ(recs[0].epoch_status->time.week, kT0.week);
    EXPECT_DOUBLE_EQ(recs[0].epoch_status->time.tow, kT0.tow);

    ASSERT_TRUE(recs[1].epoch_status.has_value());   // h3 attached (h2's time mismatches)
    EXPECT_EQ(recs[1].epoch_status->state, NetworkProducerStatus::State::Partial);
    EXPECT_EQ(recs[1].epoch_status->withheld, "C;R");
    EXPECT_DOUBLE_EQ(recs[1].epoch_status->time.tow, t3.tow);

    // Byte level: three H lines when armed, zero when not, and the armed file
    // minus its H lines is the off file, byte for byte (purely additive).
    const std::string armed = readAllBytes(path);
    const std::string off   = readAllBytes(twin);
    EXPECT_EQ(countHLines(armed), 3u);
    EXPECT_EQ(countHLines(off), 0u);
    std::string stripped;
    stripped.reserve(armed.size());
    std::size_t pos = 0;
    while (pos < armed.size()) {
        const std::size_t eol  = armed.find('\n', pos);
        const std::size_t next = (eol == std::string::npos) ? armed.size() : eol + 1;
        if (armed.compare(pos, 2, "H,") != 0) stripped.append(armed, pos, next - pos);
        pos = next;
    }
    EXPECT_EQ(stripped, off);

    std::filesystem::remove(path);
    std::filesystem::remove(twin);
}

// H lines never feed replay (the S-line precedent, twin-file style): replaying
// the armed file produces a store and discontinuity counters byte-identical to
// replaying the H-less twin of the same epochs.
TEST(NetworkProductIoTest, EpochStatusReplayMatchesTwinWithoutHLines) {
    const std::string path = tempFile("gnss_netprod_hbeat_replay.txt");
    const std::string twin = tempFile("gnss_netprod_hbeat_replay_twin.txt");
    std::filesystem::remove(path);
    std::filesystem::remove(twin);

    GnssTime t2 = kT0; t2.tow += 30.0;
    {   // Armed file: heartbeats around a phase-bias jump (-0.7 -> 5.0 trips the
        // 0.05 m disc threshold on replay) plus a standalone withheld H between.
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.writeEpochStatus(makeStatus(kT0, NetworkProducerStatus::State::Ok, 0.0, 2, "-"));
        w.write(makeEpoch(kT0, -0.7));
        GnssTime tw = kT0; tw.tow += 15.0;
        w.writeEpochStatus(
            makeStatus(tw, NetworkProducerStatus::State::WithheldGate, 0.0, 0, "*"));
        w.writeEpochStatus(makeStatus(t2, NetworkProducerStatus::State::Ok, 1.1, 2, "-"));
        w.write(makeEpoch(t2, 5.0));
    }
    {
        NetworkProductWriter w(twin);
        ASSERT_TRUE(w.good());
        w.write(makeEpoch(kT0, -0.7));
        w.write(makeEpoch(t2, 5.0));
    }

    bool ok_a = false, ok_b = false;
    const auto recs_a = NetworkProductReader::read(path, &ok_a);
    const auto recs_b = NetworkProductReader::read(twin, &ok_b);
    ASSERT_TRUE(ok_a);
    ASSERT_TRUE(ok_b);
    ASSERT_EQ(recs_a.size(), 2u);
    ASSERT_EQ(recs_b.size(), 2u);

    SsrCorrectionStore    s_a, s_b;
    NetworkProductEmitter e_a, e_b;
    for (const auto& r : recs_a) NetworkProductReader::replay(r, s_a, e_a);
    for (const auto& r : recs_b) NetworkProductReader::replay(r, s_b, e_b);
    EXPECT_EQ(s_a.size(), s_b.size());
    EXPECT_EQ(e_a.discontinuityCounter("G01", "L1C"), 1);   // the jump, not the H lines
    EXPECT_EQ(e_a.discontinuityCounter("G01", "L1C"), e_b.discontinuityCounter("G01", "L1C"));
    EXPECT_EQ(e_a.discontinuityCounter("G01", "L2W"), e_b.discontinuityCounter("G01", "L2W"));
    EXPECT_EQ(e_a.codeBiasDiscontinuityCounter("G01", "C1W"),
              e_b.codeBiasDiscontinuityCounter("G01", "C1W"));
    ASSERT_TRUE(s_a.getPhaseBias("G01", "L1C").has_value());
    EXPECT_DOUBLE_EQ(s_a.getPhaseBias("G01", "L1C")->bias_m,
                     s_b.getPhaseBias("G01", "L1C")->bias_m);
    EXPECT_EQ(s_a.getPhaseBias("G01", "L1C")->discontinuity_counter,
              s_b.getPhaseBias("G01", "L1C")->discontinuity_counter);

    std::filesystem::remove(path);
    std::filesystem::remove(twin);
}

// The tail surfaces the latest heartbeat, including a fully withheld epoch's
// standalone H, which completes no record.  An H line terminates the epoch under
// assembly, which bounds the live latency of the preceding emitted epoch across a
// withheld stretch.  Store surface: setProducerStatus/getProducerStatus
// round-trips what the runRealTime wiring pushes.
TEST(NetworkProductTailTest, HeartbeatSurfacesLatestStatusAcrossWithheldEpochs) {
    const std::string scratch = tempFile("gnss_netprod_hbeat_tail_scratch.txt");
    const std::string path    = tempFile("gnss_netprod_hbeat_tail.txt");
    std::filesystem::remove(scratch);
    std::filesystem::remove(path);

    GnssTime t2 = kT0; t2.tow += 30.0;   // withheld epoch
    GnssTime t3 = kT0; t3.tow += 60.0;   // second emitted epoch
    const auto h1 = makeStatus(kT0, NetworkProducerStatus::State::Ok, 1.0, 2, "-");
    const auto h2 = makeStatus(t2, NetworkProducerStatus::State::WithheldSigma0, 12.5, 0, "*");
    const auto h3 = makeStatus(t3, NetworkProducerStatus::State::Partial, 4.2, 2, "C");

    // Capture the pieces so they can be re-served incrementally: the header and
    // first block, then the standalone withheld H, then the next block.
    std::string part1, part2, part3;
    {
        NetworkProductWriter w(scratch);
        ASSERT_TRUE(w.good());
        w.writeEpochStatus(h1);
        w.write(makeEpoch(kT0, -0.7));
        w.flush();
        part1 = readAllBytes(scratch);
        w.writeEpochStatus(h2);
        w.flush();
        part2 = readAllBytes(scratch).substr(part1.size());
        w.writeEpochStatus(h3);
        w.write(makeEpoch(t3, -0.7));
        w.flush();
        part3 = readAllBytes(scratch).substr(part1.size() + part2.size());
    }

    NetworkProductTail tail(path);
    SsrCorrectionStore store;
    EXPECT_FALSE(tail.lastStatus().has_value());   // nothing consumed yet

    appendBytes(path, part1);                      // H1 + epoch-1 block
    EXPECT_TRUE(tail.pollInto(store).empty());     // epoch 1 not yet terminated
    ASSERT_TRUE(tail.lastStatus().has_value());    // ...but H1 already surfaced
    EXPECT_EQ(tail.lastStatus()->state, NetworkProducerStatus::State::Ok);

    appendBytes(path, part2);                      // standalone withheld H2
    {
        const auto recs = tail.pollInto(store);    // H2 terminates epoch 1
        ASSERT_EQ(recs.size(), 1u);
        EXPECT_DOUBLE_EQ(recs[0].epoch.epoch_time.tow, kT0.tow);
        ASSERT_TRUE(recs[0].epoch_status.has_value());   // epoch 1's own H1
        EXPECT_EQ(recs[0].epoch_status->state, NetworkProducerStatus::State::Ok);
    }
    ASSERT_TRUE(tail.lastStatus().has_value());
    EXPECT_EQ(tail.lastStatus()->state, NetworkProducerStatus::State::WithheldSigma0);
    EXPECT_EQ(tail.lastStatus()->withheld, "*");
    EXPECT_DOUBLE_EQ(tail.lastStatus()->time.tow, t2.tow);
    EXPECT_TRUE(tail.healthy());

    appendBytes(path, part3);                      // H3 + epoch-2 block (pending)
    {
        const auto recs = tail.pollInto(store);
        EXPECT_TRUE(recs.empty());                 // epoch 2 awaits its terminator
        ASSERT_TRUE(tail.lastStatus().has_value());
        EXPECT_EQ(tail.lastStatus()->state, NetworkProducerStatus::State::Partial);
        EXPECT_EQ(tail.lastStatus()->withheld, "C");
        EXPECT_DOUBLE_EQ(tail.lastStatus()->sigma0, 4.2);
    }
    EXPECT_EQ(tail.epochsDelivered(), 1u);

    // Store surface (the runRealTime tail wiring's push target).
    EXPECT_FALSE(store.getProducerStatus().has_value());
    store.setProducerStatus(*tail.lastStatus());
    const auto got = store.getProducerStatus();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->state, NetworkProducerStatus::State::Partial);
    EXPECT_EQ(got->withheld, "C");
    EXPECT_EQ(got->n_sats_published, 2);
    EXPECT_DOUBLE_EQ(got->time.tow, t3.tow);
    store.clear();
    EXPECT_FALSE(store.getProducerStatus().has_value());   // cleared with the store

    std::filesystem::remove(scratch);
    std::filesystem::remove(path);
}

// ===========================================================================
// Size-based service-CSV rotation (logging/CsvRotator, P1): the helper the
// network producer's epoch loop applies to network_trace.csv /
// network_solution.csv / network_service_health.csv.
// ===========================================================================

namespace {

/// Fresh empty scratch directory under the system temp dir for rotation tests.
std::filesystem::path rotatorScratchDir(const char* name) {
    const std::filesystem::path dir = gnss_test::tempDir() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Data rows (header stripped) of a rotated-generation CSV; expects every file
/// to start with @p header.
std::vector<std::string> dataRows(const std::filesystem::path& p, const std::string& header) {
    std::ifstream in(p.string());
    std::vector<std::string> rows;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            EXPECT_EQ(line + "\n", header) << p.string() << " missing/garbled header";
            continue;
        }
        if (!line.empty()) rows.push_back(line);
    }
    return rows;
}

}  // namespace

// Threshold 0 (the default) is a strict no-op: nothing is rotated, no suffix file
// is created, and the stream keeps appending, so a run with rotation off stays
// byte-identical.
TEST(CsvRotatorTest, OffThresholdIsStrictNoOp) {
    namespace fs = std::filesystem;
    const fs::path dir  = rotatorScratchDir("gnss_csv_rot_off");
    const fs::path live = dir / "trace.csv";
    const std::string header = "week,tow,value\n";

    std::ofstream os(live, std::ios::out | std::ios::trunc);
    os << header;
    for (int i = 0; i < 500; ++i) {
        os << "2313," << i << ",1.5\n";
        EXPECT_FALSE(maybeRotateCsv(os, live, header, /*threshold_mb=*/0.0, /*keep=*/3));
        EXPECT_FALSE(maybeRotateCsv(os, live, header, /*threshold_mb=*/-1.0, /*keep=*/3));
    }
    os.close();
    EXPECT_FALSE(fs::exists(fs::path(live.string() + ".1")));
    EXPECT_EQ(dataRows(live, header).size(), 500u);   // every row still in the live file

    // A closed stream is also a no-op even with a live threshold.
    std::ofstream closed;
    EXPECT_FALSE(maybeRotateCsv(closed, live, header, /*threshold_mb=*/1.0, /*keep=*/3));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Rows written past the threshold roll the file: the suffix chain shifts
// (.k -> .k+1, oldest deleted at .keep), the live file becomes .1, the fresh
// live file re-writes the header (every generation independently parseable),
// and the surviving chain read oldest-first is gap-free consecutive rows.
TEST(CsvRotatorTest, RolloverShiftsChainRewritesHeaderAndDeletesOldest) {
    namespace fs = std::filesystem;
    const fs::path dir  = rotatorScratchDir("gnss_csv_rot_roll");
    const fs::path live = dir / "trace.csv";
    const std::string header = "week,tow,value\n";
    // ~100-byte threshold => a roll every handful of ~12-byte rows; 200 rows
    // produce far more rolls than keep=2, so oldest-deletion is exercised.
    const double thr_mb = 100.0 / (1024.0 * 1024.0);
    const int    keep   = 2;

    std::ofstream os(live, std::ios::out | std::ios::trunc);
    ASSERT_TRUE(os.is_open());
    os << header;
    int rotations = 0;
    for (int i = 0; i < 200; ++i) {
        os << "2313," << i << ",1.5\n";
        if (maybeRotateCsv(os, live, header, thr_mb, keep)) ++rotations;
    }
    os.close();

    EXPECT_GE(rotations, 3);   // rolled repeatedly, not just once

    // Chain shape: live, .1 and .2 exist; .3 does not (keep=2 caps the chain).
    const fs::path g1(live.string() + ".1"), g2(live.string() + ".2"),
                   g3(live.string() + ".3");
    EXPECT_TRUE(fs::exists(live));
    ASSERT_TRUE(fs::exists(g1));
    ASSERT_TRUE(fs::exists(g2));
    EXPECT_FALSE(fs::exists(g3));

    // Header re-write and chain order: dataRows checks each generation's header;
    // the oldest-first concatenation (.2, .1, live) has strictly consecutive row
    // indices, so rows are neither lost nor duplicated across a roll (only
    // generations older than .keep are dropped).
    std::vector<std::string> all;
    for (const fs::path& p : {g2, g1, live}) {
        const auto rows = dataRows(p, header);   // also asserts the header of every generation
        // Rolled generations are always non-empty; the live file may hold only its
        // header when the last row triggered the roll.
        if (p != live) EXPECT_FALSE(rows.empty()) << p.string();
        all.insert(all.end(), rows.begin(), rows.end());
    }
    ASSERT_GE(all.size(), 2u);
    auto rowIndex = [](const std::string& row) {
        const auto a = row.find(','), b = row.rfind(',');
        return std::stoi(row.substr(a + 1, b - a - 1));
    };
    for (size_t k = 1; k < all.size(); ++k)
        ASSERT_EQ(rowIndex(all[k]), rowIndex(all[k - 1]) + 1)
            << "chain gap between '" << all[k - 1] << "' and '" << all[k] << "'";
    EXPECT_EQ(rowIndex(all.back()), 199);   // newest row is in the live file

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The bare suffix-shift: missing live file fails cleanly; a sparse chain
// shifts what exists (gaps skipped); keep clamps below 1 to a single rolled
// generation whose predecessor is deleted.
TEST(CsvRotatorTest, RotateCsvSuffixesSparseChainAndKeepClamp) {
    namespace fs = std::filesystem;
    const fs::path dir  = rotatorScratchDir("gnss_csv_rot_shift");
    const fs::path live = dir / "t.csv";
    const fs::path g1(live.string() + ".1"), g2(live.string() + ".2"),
                   g3(live.string() + ".3");
    auto put = [](const fs::path& p, const std::string& s) {
        std::ofstream o(p, std::ios::out | std::ios::trunc); o << s;
    };

    // No live file at all -> false (nothing to retire).
    EXPECT_FALSE(rotateCsvSuffixes(live, 3));

    // Sparse chain: live + .2 (no .1), keep=3.  Shift: .2 -> .3, live -> .1.
    put(live, "LIVE");
    put(g2, "GEN2");
    EXPECT_TRUE(rotateCsvSuffixes(live, 3));
    EXPECT_FALSE(fs::exists(live));
    EXPECT_EQ(readAllBytes(g1.string()), "LIVE");
    EXPECT_FALSE(fs::exists(g2));
    EXPECT_EQ(readAllBytes(g3.string()), "GEN2");

    // keep < 1 clamps to 1: the previous .1 is deleted, the live file becomes .1.
    put(live, "NEWLIVE");
    EXPECT_TRUE(rotateCsvSuffixes(live, 0));
    EXPECT_EQ(readAllBytes(g1.string()), "NEWLIVE");
    EXPECT_FALSE(fs::exists(live));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The staged-rename order (live -> .rotating, then shift, then .rotating -> .1)
// leaves no ".rotating" scratch behind on success, clears a stale scratch from a
// crashed prior roll (step 1) instead of colliding with it, and preserves the
// live file's rows across the roll (m4).
TEST(CsvRotatorTest, RotateCsvSuffixesStagingCleansScratchAndPreservesData) {
    namespace fs = std::filesystem;
    const fs::path dir  = rotatorScratchDir("gnss_csv_rot_stage");
    const fs::path live = dir / "t.csv";
    const fs::path g1(live.string() + ".1"), g2(live.string() + ".2");
    const fs::path scratch(live.string() + ".rotating");
    auto put = [](const fs::path& p, const std::string& s) {
        std::ofstream o(p, std::ios::out | std::ios::trunc); o << s;
    };

    // A stale scratch file from a crashed prior roll must not block the next roll.
    put(scratch, "STALE");
    put(live, "LIVE");
    EXPECT_TRUE(rotateCsvSuffixes(live, 3));
    EXPECT_EQ(readAllBytes(g1.string()), "LIVE");   // live content preserved in .1
    EXPECT_FALSE(fs::exists(live));                  // live retired
    EXPECT_FALSE(fs::exists(scratch));               // no ".rotating" residue

    // A second roll of a fresh live file leaves no scratch, preserves data, and
    // shifts the prior generation down one slot.
    put(live, "LIVE2");
    EXPECT_TRUE(rotateCsvSuffixes(live, 3));
    EXPECT_EQ(readAllBytes(g1.string()), "LIVE2");
    EXPECT_EQ(readAllBytes(g2.string()), "LIVE");   // prior .1 shifted to .2
    EXPECT_FALSE(fs::exists(scratch));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// =============================================================================
// NetworkStateSnapshot (RTB-7): serialize/parse round-trip, atomic write,
// warm/cold resume gate, and the quality-ramp lift rule.
// =============================================================================

/// A snapshot with at least one entry in every state group (incl. a VtecSh
/// shared-carry entry with the sh_* fields and an emitter blob).
NetworkStateSnapshot makeFullSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week          = 2313;
    s.snap_tow           = 259260.0;
    s.wall_utc           = "2026-07-02T12:00:00Z";
    s.generation_id      = 1399161600u;
    s.config_fingerprint = 0x0123456789abcdefULL;
    s.n_emitted = 42; s.n_skipped = 3; s.n_gate_withheld = 1; s.n_sigma0_withheld = 2;
    s.n_runaway_epochs = 1; s.n_perconst_partial = 4; s.n_perconst_diffuse = 5;
    s.iod = 43; s.prev_week = 2313; s.prev_tow = 259260.0;
    s.last_emit_content_s = 1399161660.5; s.last_health_write_s = 1399161650.0;
    s.ura_sigma0_ewma = 1.2345678901234567; s.ura_ewma_updates = 36;
    s.ramp_lifted = true; s.first_emit_content_s = 1399160400.0;

    NetworkStateSnapshot::SharedCarry pb;
    pb.kind = 2; pb.sat = "G01"; pb.signal = "L1C";
    pb.value = -0.71234567890123; pb.var = 1.5e-6; pb.inflate_q = false;
    s.shared_carry.push_back(pb);
    NetworkStateSnapshot::SharedCarry clk;
    clk.kind = 0; clk.sat = "E11";
    clk.value = 12.5; clk.var = 0.04; clk.inflate_q = true;   // clock-monitor mark
    s.shared_carry.push_back(clk);
    NetworkStateSnapshot::SharedCarry sh;
    sh.kind = 3; sh.sh_n = 2; sh.sh_m = 1; sh.sh_trig = 'S';
    sh.value = 3.25; sh.var = 0.5;
    s.shared_carry.push_back(sh);

    s.prev_pivot = {{"G", "G05"}, {"E", "E11"}};
    s.last_pivot = {{"G", "G05"}, {"E", "E11"}, {"R", "R08"}};
    s.clock_osc["G01"] = {12.5, -1.25e-4, 0.04, 1e-6, 1e-8, 1399161660.0, true};
    s.prev_clk["G02"]  = {3.75, 1399161630.0};
    s.local_carry[{7, "G01", "L1C"}] = {123456.789012345, 2.5e-5, 1399161660.0};
    s.zwd_carry[7]                   = {0.1523, 1e-6, 1399161660.0};
    s.ura_refloat_horizon["G13"]     = 4;
    s.ura_retro_sats = {"G13", "E09"};
    s.retro_active   = {"G13"};
    s.c1_zoh_active  = {"C21"};
    s.relevel_iode   = {{"G01", 10}, {"E11", 7}};   // re-level baselines
    s.eph_cutover_disc_pending = {"C21"};           // pending forced disc

    // Coupling regressors (format v3; see §18.12 for the design): the
    // shared-column dictionary plus one packed regression per carried row, keyed
    // on the L / Z keys above (the parser refuses an orphan QL/QZ, so these
    // reference local_carry[{7,"G01","L1C"}] and zwd_carry[7]).  Entry order
    // inside a row is harvest order and is preserved verbatim; it is observable
    // at the ulp, since buildCplRow accumulates rhs += coeff*s_prev in vector
    // order, so the ids here are deliberately not monotone.  The dictionary carries a Clock
    // column (empty signal), a PhaseBias column, and a VtecSh column (empty sat,
    // sh_layer in its own field, not the D record's "L1"-in-sat encoding).  QR is
    // the write-epoch clock-refloat set, which no other record in the file can
    // carry (a hard-refloated satellite is skip-stashed).
    s.coupling_dict = {
        {0, "G01", "",    0, 0, 'C', 0, -0.9718706123456789, 1.7312e-05},
        {2, "G01", "L1C", 0, 0, 'C', 0, -1.2178912315598864, 8.128093975162085e-07},
        {3, "",    "",    2, 1, 'S', 1,  0.4471821,          0.0},   // pinned: p_prev == 0
    };
    s.coupling_rows[{7, "G01", "L1C"}] = {{2, -0.99876543210987}, {0, 0.48312}};
    s.coupling_zrows[7]                = {{1, 0.331}, {0, -0.0042}};
    s.clock_refloat_sats = {"G14", "G17"};

    // A real emitter blob (round-trips through restoreState in the driver),
    // carrying a pending forced datum bump: G is tracked (first emit), then a
    // G-scoped markDatumChange lands on an E-only epoch.  The RT0-5 seam keeps
    // the G flag pending, and the snapshot preserves it.
    SsrCorrectionStore store;
    NetworkProductEmitter em(0.05, /*emit_clock_disc=*/true, /*staleness=*/100.0);
    em.emitToStore(makeEpoch(GnssTime{2313, 259230.0}, /*pb01=*/-0.7), store);
    em.markDatumChange({'G'});
    NetworkSolutionEpoch eonly;
    eonly.epoch_time = GnssTime{2313, 259260.0};
    eonly.iod_ssr = 3; eonly.provider_id = 42; eonly.solution_id = 1;
    NetworkSatSolution e11;
    e11.sat = "E11"; e11.has_clock = true; e11.clock_c0_m = 1.0;
    eonly.sats.push_back(e11);
    em.emitToStore(eonly, store);   // G absent -> its forced bump stays pending
    std::ostringstream blob;
    em.serializeState(blob);
    s.emitter_state = blob.str();
    return s;
}

// Every state group survives a serialize -> parse round trip exactly (doubles
// via the shortest round-trippable form), the emitter blob verbatim.
TEST(NetworkStateSnapshotTest, SerializeParseRoundTripsEveryGroup) {
    const NetworkStateSnapshot s = makeFullSnapshot();
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(os.str());
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;

    EXPECT_EQ(r.snap_week, s.snap_week);
    EXPECT_DOUBLE_EQ(r.snap_tow, s.snap_tow);
    EXPECT_EQ(r.wall_utc, s.wall_utc);
    EXPECT_EQ(r.generation_id, s.generation_id);
    EXPECT_EQ(r.config_fingerprint, s.config_fingerprint);
    EXPECT_EQ(r.n_emitted, s.n_emitted);
    EXPECT_EQ(r.n_skipped, s.n_skipped);
    EXPECT_EQ(r.n_gate_withheld, s.n_gate_withheld);
    EXPECT_EQ(r.n_sigma0_withheld, s.n_sigma0_withheld);
    EXPECT_EQ(r.n_runaway_epochs, s.n_runaway_epochs);
    EXPECT_EQ(r.n_perconst_partial, s.n_perconst_partial);
    EXPECT_EQ(r.n_perconst_diffuse, s.n_perconst_diffuse);
    EXPECT_EQ(r.iod, s.iod);
    EXPECT_EQ(r.prev_week, s.prev_week);
    EXPECT_DOUBLE_EQ(r.prev_tow, s.prev_tow);
    EXPECT_DOUBLE_EQ(r.last_emit_content_s, s.last_emit_content_s);
    EXPECT_DOUBLE_EQ(r.last_health_write_s, s.last_health_write_s);
    EXPECT_DOUBLE_EQ(r.ura_sigma0_ewma, s.ura_sigma0_ewma);
    EXPECT_EQ(r.ura_ewma_updates, s.ura_ewma_updates);
    EXPECT_EQ(r.ramp_lifted, s.ramp_lifted);
    EXPECT_DOUBLE_EQ(r.first_emit_content_s, s.first_emit_content_s);

    ASSERT_EQ(r.shared_carry.size(), s.shared_carry.size());
    for (size_t i = 0; i < s.shared_carry.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(r.shared_carry[i].kind, s.shared_carry[i].kind);
        EXPECT_EQ(r.shared_carry[i].sat, s.shared_carry[i].sat);
        EXPECT_EQ(r.shared_carry[i].signal, s.shared_carry[i].signal);
        EXPECT_EQ(r.shared_carry[i].sh_n, s.shared_carry[i].sh_n);
        EXPECT_EQ(r.shared_carry[i].sh_m, s.shared_carry[i].sh_m);
        EXPECT_EQ(r.shared_carry[i].sh_trig, s.shared_carry[i].sh_trig);
        EXPECT_DOUBLE_EQ(r.shared_carry[i].value, s.shared_carry[i].value);
        EXPECT_DOUBLE_EQ(r.shared_carry[i].var, s.shared_carry[i].var);
        EXPECT_EQ(r.shared_carry[i].inflate_q, s.shared_carry[i].inflate_q);
    }
    EXPECT_EQ(r.prev_pivot, s.prev_pivot);
    EXPECT_EQ(r.last_pivot, s.last_pivot);
    ASSERT_EQ(r.clock_osc.size(), 1u);
    EXPECT_DOUBLE_EQ(r.clock_osc.at("G01").x0, 12.5);
    EXPECT_DOUBLE_EQ(r.clock_osc.at("G01").x1, -1.25e-4);
    EXPECT_DOUBLE_EQ(r.clock_osc.at("G01").p01, 1e-6);
    EXPECT_DOUBLE_EQ(r.clock_osc.at("G01").t_last, 1399161660.0);
    EXPECT_TRUE(r.clock_osc.at("G01").init);
    EXPECT_EQ(r.prev_clk, s.prev_clk);
    ASSERT_EQ(r.local_carry.size(), 1u);
    {
        const auto& lc = r.local_carry.at({7, "G01", "L1C"});
        EXPECT_DOUBLE_EQ(lc.value_abs, 123456.789012345);
        EXPECT_DOUBLE_EQ(lc.var, 2.5e-5);
        EXPECT_DOUBLE_EQ(lc.last_seen_s, 1399161660.0);
    }
    ASSERT_EQ(r.zwd_carry.size(), 1u);
    EXPECT_DOUBLE_EQ(r.zwd_carry.at(7).value_abs, 0.1523);
    EXPECT_EQ(r.ura_refloat_horizon, s.ura_refloat_horizon);
    EXPECT_EQ(r.ura_retro_sats, s.ura_retro_sats);
    EXPECT_EQ(r.retro_active, s.retro_active);
    EXPECT_EQ(r.c1_zoh_active, s.c1_zoh_active);
    EXPECT_EQ(r.relevel_iode, s.relevel_iode);                       // re-level baselines
    EXPECT_EQ(r.eph_cutover_disc_pending, s.eph_cutover_disc_pending);// pending forced disc
    // Coupling regressors (§18.12): the dictionary field-for-field, then
    // whole-container equality on the rows.  std::vector<std::pair<int,double>>
    // compares element-wise in order, which is the required property: the
    // restored row is bit-for-bit the write epoch's harvest, in harvest order.
    // The format-contract detail (traps, negative cases, the loader interlocks)
    // lives in tests/test_resume_coupling_restore.cpp; this suite only keeps its
    // "every group" claim true.
    ASSERT_EQ(r.coupling_dict.size(), s.coupling_dict.size());
    for (size_t i = 0; i < s.coupling_dict.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(r.coupling_dict[i].kind,     s.coupling_dict[i].kind);
        EXPECT_EQ(r.coupling_dict[i].sat,      s.coupling_dict[i].sat);
        EXPECT_EQ(r.coupling_dict[i].signal,   s.coupling_dict[i].signal);
        EXPECT_EQ(r.coupling_dict[i].sh_n,     s.coupling_dict[i].sh_n);
        EXPECT_EQ(r.coupling_dict[i].sh_m,     s.coupling_dict[i].sh_m);
        EXPECT_EQ(r.coupling_dict[i].sh_trig,  s.coupling_dict[i].sh_trig);
        EXPECT_EQ(r.coupling_dict[i].sh_layer, s.coupling_dict[i].sh_layer);
        EXPECT_DOUBLE_EQ(r.coupling_dict[i].s_prev, s.coupling_dict[i].s_prev);
        EXPECT_DOUBLE_EQ(r.coupling_dict[i].p_prev, s.coupling_dict[i].p_prev);
    }
    EXPECT_EQ(r.coupling_rows, s.coupling_rows);
    EXPECT_EQ(r.coupling_zrows, s.coupling_zrows);
    EXPECT_EQ(r.clock_refloat_sats, s.clock_refloat_sats);
    EXPECT_TRUE(r.hasCouplingState());
    // Emitter blob verbatim; it restores into a matching emitter with the pending
    // G datum bump intact, and G's next emit after the round trip lands it.
    EXPECT_EQ(r.emitter_state, s.emitter_state);
    NetworkProductEmitter em(0.05, true, 100.0);
    std::istringstream eis(r.emitter_state);
    ASSERT_TRUE(em.restoreState(eis));
    EXPECT_EQ(em.clockDiscontinuityCounter("G01"), 0);   // not yet landed
    SsrCorrectionStore st;
    em.emitToStore(makeEpoch(GnssTime{2313, 259290.0}, /*pb01=*/-0.7), st);
    EXPECT_EQ(em.clockDiscontinuityCounter("G01"), 1);   // pending bump landed
    EXPECT_EQ(em.clockDiscontinuityCounter("G02"), 1);
    // ...once (consumed by that emit).
    em.emitToStore(makeEpoch(GnssTime{2313, 259320.0}, /*pb01=*/-0.7), st);
    EXPECT_EQ(em.clockDiscontinuityCounter("G01"), 1);
}

// An all-empty snapshot (fresh producer, nothing carried) round-trips too.
TEST(NetworkStateSnapshotTest, EmptySnapshotRoundTrips) {
    NetworkStateSnapshot s;
    s.snap_week = 2313; s.snap_tow = 259200.0;
    s.config_fingerprint = 7;
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(os.str());
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_TRUE(r.shared_carry.empty());
    EXPECT_TRUE(r.local_carry.empty());
    EXPECT_TRUE(r.zwd_carry.empty());
    EXPECT_TRUE(r.clock_osc.empty());
    EXPECT_TRUE(r.prev_pivot.empty());
    EXPECT_TRUE(r.ura_retro_sats.empty());
    EXPECT_TRUE(r.emitter_state.empty());
    EXPECT_EQ(r.config_fingerprint, 7u);
}

// Corrupt or truncated files fail the parse, which is the caller's cold-start
// signal.
TEST(NetworkStateSnapshotTest, CorruptOrTruncatedFileFailsParse) {
    const NetworkStateSnapshot s = makeFullSnapshot();
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string full = os.str();

    NetworkStateSnapshot r;
    std::string err;
    // Truncated: cut the END terminator (crash mid-write of a non-atomic file).
    {
        std::istringstream is(full.substr(0, full.size() - 10));
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    }
    // Truncated inside the emitter section.
    {
        const std::size_t pos = full.find("EMITTER,");
        ASSERT_NE(pos, std::string::npos);
        std::istringstream is(full.substr(0, pos + 12));
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    }
    // Corrupt: garble a numeric field.
    {
        std::string bad = full;
        const std::size_t pos = bad.find("\nK,");
        ASSERT_NE(pos, std::string::npos);
        bad[pos + 8] = 'x';
        std::istringstream is(bad);
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    }
    // Wrong header version.
    {
        std::string bad = full;
        bad.replace(0, 32, "# gnss_network_state_snapshot v9");
        std::istringstream is(bad);
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    }
    // Line-count mismatch (a line deleted but END untouched).
    {
        std::string bad = full;
        const std::size_t a = bad.find("\nJ,");
        ASSERT_NE(a, std::string::npos);
        const std::size_t b = bad.find('\n', a + 1);
        bad.erase(a, b - a);
        std::istringstream is(bad);
        EXPECT_FALSE(NetworkStateSnapshotIO::parse(is, r, &err));
    }
    // The pristine bytes still parse (the corruptions above were the cause).
    {
        std::istringstream is(full);
        EXPECT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    }
}

// Atomic write: replaces an existing snapshot in one step, leaves no .tmp
// behind, and the replaced file reads back as the new snapshot.
TEST(NetworkStateSnapshotTest, WriteAtomicallyReplacesExisting) {
    namespace fs = std::filesystem;
    const std::string path = tempFile("gnss_netsnap_atomic.txt");
    fs::remove(path);
    fs::remove(path + ".tmp");

    NetworkStateSnapshot a = makeFullSnapshot();
    a.n_emitted = 1;
    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::write(path, a, &err)) << err;
    ASSERT_TRUE(fs::exists(path));
    EXPECT_FALSE(fs::exists(path + ".tmp"));

    NetworkStateSnapshot b = makeFullSnapshot();
    b.n_emitted = 2;
    ASSERT_TRUE(NetworkStateSnapshotIO::write(path, b, &err)) << err;   // target exists
    EXPECT_FALSE(fs::exists(path + ".tmp"));

    NetworkStateSnapshot r;
    ASSERT_TRUE(NetworkStateSnapshotIO::read(path, r, &err)) << err;
    EXPECT_EQ(r.n_emitted, 2u);

    fs::remove(path);
}

// The warm/cold gate: fingerprint mismatch => cold; stale => cold; resuming at
// or before the snapshot epoch => cold; fresh + matching => warm.
TEST(NetworkStateSnapshotTest, WarmResumeGate) {
    NetworkStateSnapshot s;
    s.snap_week = 2313; s.snap_tow = 259200.0;   // snapshot epoch
    s.config_fingerprint = 0xDEADBEEFULL;
    const double snap_s = 2313 * 604800.0 + 259200.0;
    const double horizon = 120.0;
    std::string why;

    // Fresh and matching => warm.
    EXPECT_TRUE(NetworkStateSnapshotIO::warmResumeAllowed(
        s, 0xDEADBEEFULL, snap_s + 30.0, horizon, &why)) << why;
    // Boundary: age == horizon is still warm; just past it is cold.
    EXPECT_TRUE(NetworkStateSnapshotIO::warmResumeAllowed(
        s, 0xDEADBEEFULL, snap_s + horizon, horizon, &why)) << why;
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(
        s, 0xDEADBEEFULL, snap_s + horizon + 0.5, horizon, &why));
    EXPECT_NE(why.find("stale"), std::string::npos);
    // Fingerprint mismatch => cold even when fresh.
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(
        s, 0xBADC0DEULL, snap_s + 30.0, horizon, &why));
    EXPECT_NE(why.find("fingerprint"), std::string::npos);
    // Resuming at or before the snapshot epoch => cold (the epoch would be folded twice).
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(
        s, 0xDEADBEEFULL, snap_s, horizon, &why));
    EXPECT_FALSE(NetworkStateSnapshotIO::warmResumeAllowed(
        s, 0xDEADBEEFULL, snap_s - 30.0, horizon, &why));
}

// The quality-ramp lift rule (RTB-7(b)): the emitted-epoch horizon is
// max(warmup_epochs, pl_warmup), the EWMA update count must reach warmup_epochs,
// and a structurally unavailable EWMA waives the count.
TEST(NetworkStateSnapshotTest, WarmupRampLiftRule) {
    // warmup_epochs=10, pl_warmup=6 (EWMA structurally available).
    EXPECT_FALSE(networkWarmupRampLifted(9, 10, 6, 100, true));    // below epoch horizon
    EXPECT_FALSE(networkWarmupRampLifted(10, 10, 6, 9, true));     // EWMA not stabilized
    EXPECT_TRUE(networkWarmupRampLifted(10, 10, 6, 10, true));     // both satisfied
    // pl_warmup dominates the epoch horizon when larger: EWMA updates only
    // begin once the carry is warm, so the lift waits for both.
    EXPECT_FALSE(networkWarmupRampLifted(10, 4, 12, 100, true));   // n < max(4,12)
    EXPECT_FALSE(networkWarmupRampLifted(12, 4, 12, 3, true));     // updates < warmup_epochs
    EXPECT_TRUE(networkWarmupRampLifted(12, 4, 12, 4, true));
    // Structurally unavailable EWMA (emit_ura/raim/phase_leveling not all
    // armed): the epoch horizon alone decides; there is no statistic to wait for.
    EXPECT_FALSE(networkWarmupRampLifted(9, 10, 6, 0, false));
    EXPECT_TRUE(networkWarmupRampLifted(10, 10, 6, 0, false));
    // warmup_epochs=0 with a floor: the ramp still waits for the phase-leveling
    // warmup (carry-cold epochs are never product grade).
    EXPECT_FALSE(networkWarmupRampLifted(5, 0, 6, 0, true));
    EXPECT_TRUE(networkWarmupRampLifted(6, 0, 6, 0, true));
}

} // namespace
} // namespace GnssProcessor
