/**
 * @file test_ledger_emit.cpp
 * @brief Declared signal roster on the wire, the emitter's per-family
 *        bookkeeping read against the arc ledger (@c network.ledger_emit), and
 *        the consumer's latch and freshness response keyed on the declaration
 *        (@c network.closed_loop.roster_latch).
 *
 * ## What is under test
 *
 * Phase 5 is the first phase whose product leaves the process, so the tests pin
 * the properties a run cannot re-derive later:
 *
 *  - the record.  A new wire record type is a permanent contract.  Its grammar
 *    round-trips through the writer and the reader, it is declared in the format
 *    legend, and a block of records replaces the previous declaration instead of
 *    merging into it;
 *  - the reader's tolerance.  The format ignores unknown key=value tokens.  An
 *    unknown record tag is a separate case: parsing continues past it and every
 *    epoch is recovered, while the sticky @c ok flag goes false;
 *  - the generation comparison.  The hold generation of the phase-bias hold and the
 *    ledger's admitted arc generation are two lifecycles of the same family in
 *    two key spaces.  The three classes (a generation change with a birth behind
 *    it, one without, and a birth the generation never reports) must be
 *    separable by counters alone, or the run-end table carries no information;
 *  - the first-sighting test.  The emitter seeds a fresh baseline on
 *    @c find() == end(); the ledger knows whether the family is returning;
 *  - the consumer.  The two branches of C1 (the latch follows a declared signal
 *    the station tracks, and falls back when it tracks none) and the rule of C2
 *    (a signal the declaration does not name is not a staleness).
 *
 * The tests drive the pieces directly: no station build, no RINEX geometry, no
 * solve.
 */

#include <gtest/gtest.h>

#include "support/NetworkTestRun.hpp"   // the run and epoch networkLedgerEmitFold's epoch form takes
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/state/ArcLedger.hpp"
#include "network/emit/LedgerEmit.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "model/PppModel.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/ProductParser.hpp"
#include "products/SignalDeclaration.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "rinex/RinexParser.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;

namespace GnssProcessor {

/// Test seam (declared a friend in include/model/PppModel.hpp): reaches the
/// private first-epoch decision, so the declared-latch comparison can be driven
/// without a geometry or product chain.
struct PppModelDeclaredLatchTestAccess {
    static void decide(PppModel& m, const std::vector<RinexObservation>& obs) {
        m.decideConstellationCanonicals(obs);
    }
};

}  // namespace GnssProcessor

using namespace GnssProcessor;

namespace {

const GnssTime kT0{ 2313, 259200.0 };

std::string tempFile(const char* name) {
    return (gnss_test::tempDir() / name).string();
}

/// The declaration the field posture would state: two bands, two phases and two
/// codes on band 1 (the L1C / L1W split the audit measured), one of each on band
/// 2, a second-code-channel companion, and an S-basis datum designation.
NetworkSignalDeclaration fieldDeclaration() {
    NetworkSignalDeclaration d;
    d.present = true;
    SignalDeclarationBand b1;
    b1.phase      = { "L1C", "L1W" };
    b1.code       = { "C1C", "C1W" };
    b1.companion  = { "C1W" };
    b1.datum_code = "C1C";
    b1.datum_phase = "L1C";
    SignalDeclarationBand b2;
    b2.phase      = { "L2W" };
    b2.code       = { "C2W" };
    b2.datum_code = "C2W";
    d.bands["GPS"]['1'] = b1;
    d.bands["GPS"]['2'] = b2;
    return d;
}

/// A two-satellite epoch mirroring the product-IO fixture: enough records that a
/// filtered byte comparison of the file means something.
NetworkSolutionEpoch makeEpoch(const GnssTime& t, std::uint32_t gen01) {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = t;
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;

    NetworkSatSolution g1;
    g1.sat                = "G01";
    g1.has_clock          = true;
    g1.clock_c0_m         = 12.5;
    g1.code_bias_m["C1W"] = 0.3;
    g1.phase_bias["L1C"]  = NetworkPhaseBias{ -0.7, true, 1 };
    g1.phase_bias["L1C"].hold_generation = gen01;
    g1.phase_bias["L2W"]  = NetworkPhaseBias{ 1.25, false, -2 };
    sol.sats.push_back(g1);

    NetworkSatSolution g2;
    g2.sat               = "G02";
    g2.has_clock         = true;
    g2.clock_c0_m        = -8.75;
    g2.phase_bias["L1C"] = NetworkPhaseBias{ 10.0, true, 1 };
    sol.sats.push_back(g2);
    return sol;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// The file with every N record removed: what "byte-identical after filtering"
/// means for gate G1 of the build brief.
std::string withoutDeclarations(const std::string& text, int* n_removed = nullptr) {
    std::istringstream in(text);
    std::string line, out;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.rfind("N,", 0) == 0) { ++n; continue; }
        out += line;
        out += '\n';
    }
    if (n_removed) *n_removed = n;
    return out;
}

/// A minimal network config a NetworkRunContext can be built over.
fs::path writeTempConfig(const std::string& tag, const std::string& network_extra) {
    const std::string body = std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    const fs::path p = gnss_test::tempDir() / ("ledgeremit_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    explicit Loaded(const std::string& tag, const std::string& extra) {
        path = writeTempConfig(tag, extra);
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

/// A traces object over a scratch directory whose streams stay closed, so the
/// event calls of the fold are exercised without writing a file.
std::unique_ptr<NetworkRunTraces> quietTraces() {
    return std::make_unique<NetworkRunTraces>(gnss_test::tempDir() / "ledgeremit_tr");
}

}  // namespace

// ---------------------------------------------------------------------------
// The record round-trips through the writer and the parser (E4 a)
// ---------------------------------------------------------------------------

TEST(LedgerEmitTest, RosterRecordRoundTripsThroughTheWriterAndTheParser) {
    const std::string path = tempFile("gnss_sid5_roundtrip.txt");
    fs::remove(path);
    const NetworkSignalDeclaration decl = fieldDeclaration();

    NetworkProductEmitter emit(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore    store;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        // The block is written before the epoch it describes, following the
        // positional precedent of the S line.
        EXPECT_EQ(w.writeSignalDeclaration(decl), 2u);
        NetworkSolutionEpoch sol = makeEpoch(kT0, /*gen01=*/1);
        emit.emitToStore(sol, store);
        w.write(sol, false, {}, {}, nullptr);
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok) << "an N record must not flag the parser";
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_TRUE(recs[0].declaration_refresh.has_value());
    EXPECT_EQ(*recs[0].declaration_refresh, decl);

    // Every field survives, not just the record count.
    const SignalDeclarationBand* b1 = recs[0].declaration_refresh->find("GPS", '1');
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(b1->phase, (std::vector<std::string>{ "L1C", "L1W" }));
    EXPECT_EQ(b1->code, (std::vector<std::string>{ "C1C", "C1W" }));
    EXPECT_EQ(b1->companion, (std::vector<std::string>{ "C1W" }));
    EXPECT_EQ(b1->datum_code, "C1C");
    EXPECT_EQ(b1->datum_phase, "L1C");
    EXPECT_EQ(recs[0].declaration_refresh->find("GPS", '3'), nullptr);
    fs::remove(path);
}

/// The wire form is the grammar the legend declares: a positional
/// (tag, constellation, band digit) head and self-describing key=value tokens,
/// with an empty list omitted rather than written as a placeholder: "the
/// network carries no phase family on this band" is the absence of `ph=`.
TEST(LedgerEmitTest, DeclarationWireFormIsTheDeclaredGrammar) {
    NetworkSignalDeclaration d;
    d.present = true;
    SignalDeclarationBand b;
    b.code = { "C5Q" };           // a band the network codes but does not phase
    d.bands["GPS"]['5'] = b;
    const auto lines = signalDeclarationToWire(d);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "N,GPS,5,co=C5Q");

    // A row with nothing to say is never written at all.
    NetworkSignalDeclaration empty;
    empty.present = true;
    empty.bands["GPS"]['1'] = SignalDeclarationBand{};
    EXPECT_TRUE(signalDeclarationToWire(empty).empty());
}

/// A declaration replaces the previous one: the reader accumulates a contiguous
/// block and the next block starts from nothing, so a band that disappears from
/// the producer's roster disappears from the consumer's copy too.
TEST(LedgerEmitTest, ASecondBlockReplacesTheFirstWholesale) {
    const std::string path = tempFile("gnss_sid5_replace.txt");
    fs::remove(path);
    NetworkProductEmitter emit(0.05, false);
    SsrCorrectionStore    store;
    NetworkSignalDeclaration second;
    second.present = true;
    SignalDeclarationBand b1;
    b1.phase = { "L1W" };
    b1.code  = { "C1W" };
    second.bands["GPS"]['1'] = b1;
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.writeSignalDeclaration(fieldDeclaration());
        NetworkSolutionEpoch s1 = makeEpoch(kT0, 1);
        emit.emitToStore(s1, store);
        w.write(s1, false, {}, {}, nullptr);
        w.writeSignalDeclaration(second);
        NetworkSolutionEpoch s2 = makeEpoch(GnssTime{ 2313, 259230.0 }, 1);
        emit.emitToStore(s2, store);
        w.write(s2, false, {}, {}, nullptr);
    }
    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_TRUE(recs[1].declaration_refresh.has_value());
    EXPECT_EQ(*recs[1].declaration_refresh, second);
    // Band 2 was in the first block and not in the second, so it is absent.
    EXPECT_EQ(recs[1].declaration_refresh->find("GPS", '2'), nullptr);
    fs::remove(path);
}

/// The wire outside the declaration is untouched: filter the N records out and
/// the file is byte-identical to the same epochs written without one.  This is
/// gate G1's identity test at unit scale.
TEST(LedgerEmitTest, TheWireIsByteIdenticalOnceTheRecordsAreFiltered) {
    const std::string with    = tempFile("gnss_sid5_with.txt");
    const std::string without = tempFile("gnss_sid5_without.txt");
    fs::remove(with);
    fs::remove(without);
    const auto writeArm = [](const std::string& path, bool declare) {
        NetworkProductEmitter emit(0.05, false);
        SsrCorrectionStore    store;
        NetworkProductWriter  w(path);
        EXPECT_TRUE(w.good());
        if (declare) w.writeSignalDeclaration(fieldDeclaration());
        for (int k = 0; k < 3; ++k) {
            NetworkSolutionEpoch sol =
                makeEpoch(GnssTime{ 2313, 259200.0 + 30.0 * k }, 1);
            emit.emitToStore(sol, store);
            w.write(sol, false, {}, {}, nullptr);
        }
    };
    writeArm(with, true);
    writeArm(without, false);
    int n_removed = 0;
    EXPECT_EQ(withoutDeclarations(readFile(with), &n_removed), readFile(without));
    EXPECT_EQ(n_removed, 2);
    EXPECT_NE(readFile(with), readFile(without));   // the arm is not vacuous
    fs::remove(with);
    fs::remove(without);
}

/// The legend is grammar-complete for the new tag: the rule
/// HeaderLegendDeclaresEveryEmittedField enforces for every other record.
TEST(LedgerEmitTest, TheFormatLegendDeclaresTheDeclarationRecord) {
    const std::string path = tempFile("gnss_sid5_legend.txt");
    fs::remove(path);
    { NetworkProductWriter w(path); ASSERT_TRUE(w.good()); }
    const std::string text = readFile(path);
    const std::size_t at = text.find("# N,constellation,band_digit");
    ASSERT_NE(at, std::string::npos) << text;
    const std::string legend = text.substr(at, text.find('\n', at) - at);
    for (const char* key : { "ph=", "co=", "comp=", "dph=", "dco=" })
        EXPECT_NE(legend.find(key), std::string::npos) << key << " in: " << legend;
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// The reader's tolerance (E4 b)
// ---------------------------------------------------------------------------

/// An unknown record tag is skipped and parsing continues: every epoch is
/// recovered and the declaration around it survives.  The sticky @c ok flag goes
/// false; that is the reader's existing "malformed line(s)" signal, which every
/// call site treats as a warning, not an abort.  The test pins this contract.
TEST(LedgerEmitTest, AnUnknownRecordIsIgnoredByTheParser) {
    const std::string path = tempFile("gnss_sid5_unknown.txt");
    fs::remove(path);
    {
        NetworkProductEmitter emit(0.05, false);
        SsrCorrectionStore    store;
        NetworkProductWriter  w(path);
        ASSERT_TRUE(w.good());
        w.writeSignalDeclaration(fieldDeclaration());
        NetworkSolutionEpoch s1 = makeEpoch(kT0, 1);
        emit.emitToStore(s1, store);
        w.write(s1, false, {}, {}, nullptr);
        NetworkSolutionEpoch s2 = makeEpoch(GnssTime{ 2313, 259230.0 }, 1);
        emit.emitToStore(s2, store);
        w.write(s2, false, {}, {}, nullptr);
    }
    // Splice a record type from a future revision between the two epochs.
    std::string text = readFile(path);
    const std::size_t second = text.find("E,2313,259230");
    ASSERT_NE(second, std::string::npos);
    text.insert(second, "Q,2313,259230,something,else\n");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    bool ok = true;
    const auto recs = NetworkProductReader::read(path, &ok);
    EXPECT_EQ(recs.size(), 2u) << "every epoch must still be recovered";
    EXPECT_FALSE(ok) << "an unknown TAG flags the sticky parse-health bit (a WARN, "
                        "never an abort) while unknown key=value TOKENS do not";
    ASSERT_TRUE(recs[0].declaration_refresh.has_value());
    EXPECT_EQ(*recs[0].declaration_refresh, fieldDeclaration());
    fs::remove(path);
}

/// An unknown key=value token inside an N record is ignored and the known ones
/// still land: the format's own forward-compatibility rule, and the reason the
/// record carries tokens rather than positional fields.
TEST(LedgerEmitTest, AnUnknownTokenInsideTheRecordIsIgnored) {
    NetworkSignalDeclaration d;
    const std::vector<std::string> f{ "N", "GPS", "1", "ph=L1C+L1W", "future=42", "co=C1C" };
    ASSERT_TRUE(signalDeclarationParseRecord(f, d));
    ASSERT_TRUE(d.present);
    const SignalDeclarationBand* b = d.find("GPS", '1');
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->phase, (std::vector<std::string>{ "L1C", "L1W" }));
    EXPECT_EQ(b->code, (std::vector<std::string>{ "C1C" }));

    // A malformed head is refused and leaves the declaration untouched.
    NetworkSignalDeclaration bad;
    EXPECT_FALSE(signalDeclarationParseRecord({ "N", "GPS" }, bad));
    EXPECT_FALSE(signalDeclarationParseRecord({ "N", "", "1" }, bad));
    EXPECT_FALSE(signalDeclarationParseRecord({ "N", "GPS", "12" }, bad));
    EXPECT_FALSE(bad.present);
}

/// Replay is the path the census uses, and it is where the declaration reaches a
/// consumer's store.
TEST(LedgerEmitTest, ReplayPushesTheDeclarationIntoTheConsumerStore) {
    const std::string path = tempFile("gnss_sid5_replay.txt");
    fs::remove(path);
    {
        NetworkProductEmitter emit(0.05, false);
        SsrCorrectionStore    store;
        NetworkProductWriter  w(path);
        ASSERT_TRUE(w.good());
        w.writeSignalDeclaration(fieldDeclaration());
        NetworkSolutionEpoch sol = makeEpoch(kT0, 1);
        emit.emitToStore(sol, store);
        w.write(sol, false, {}, {}, nullptr);
    }
    bool ok = false;
    SbasisContract contract;
    const auto recs = NetworkProductReader::read(path, &contract, &ok);
    ASSERT_EQ(recs.size(), 1u);

    SsrCorrectionStore    consumer;
    NetworkProductEmitter replay_emit = NetworkProductReader::makeReplayEmitter(contract);
    EXPECT_FALSE(consumer.declaredRoster().present);
    NetworkProductReader::replay(recs[0], consumer, replay_emit);
    const NetworkSignalDeclaration got = consumer.declaredRoster();
    ASSERT_TRUE(got.present);
    EXPECT_EQ(got, fieldDeclaration());
    fs::remove(path);
}

/// The lookups a consumer makes, by role: a second-code-channel companion code is a family
/// the network emits, so it answers the code question yes.
TEST(LedgerEmitTest, DeclarationLookupsAnswerByRole) {
    const NetworkSignalDeclaration d = fieldDeclaration();
    EXPECT_TRUE(d.declaresPhase("GPS", "L1W"));
    EXPECT_TRUE(d.declaresPhase("GPS", "L2W"));
    EXPECT_FALSE(d.declaresPhase("GPS", "L1X"));
    EXPECT_FALSE(d.declaresPhase("GPS", "L5Q"));
    EXPECT_FALSE(d.declaresPhase("Galileo", "L1C"));
    EXPECT_TRUE(d.declaresCode("GPS", "C1W"));      // modelled and a companion
    EXPECT_TRUE(d.declaresCode("GPS", "C2W"));
    EXPECT_FALSE(d.declaresCode("GPS", "C5Q"));
    EXPECT_TRUE(d.knowsConstellation("GPS"));
    EXPECT_FALSE(d.knowsConstellation("BeiDou"));
}

// ---------------------------------------------------------------------------
// The generation comparison and the first-sighting test (E1)
// ---------------------------------------------------------------------------

namespace {

/// Drives one epoch of the capture and fold pair over a context whose ledger is
/// advanced by hand.  Returns nothing; the tests read the counters.
void runEpoch(NetworkTestRun& fx,
              const NetworkProductEmitter& emitter, const NetworkSolutionEpoch& sol,
              const std::set<ArcLedger::Key>& admitted, long long ep) {
    NetworkRunContext& ctx = fx.ctx();
    ctx.arc_ledger.beginEpoch(ep, 259200.0 + 30.0 * static_cast<double>(ep));
    networkLedgerEmitCapture(ctx, emitter, sol);
    std::vector<std::pair<ArcLedger::Key, ArcStep>> steps;
    ctx.arc_ledger.admitEpoch(admitted, steps);
    NetworkEpoch& e = fx.newEpoch(GnssTime{ 2313, 259200.0 + 30.0 * static_cast<double>(ep) });
    networkLedgerEmitFold(fx.run(), e);
}

}  // namespace

/// The three classes of the generation comparison, separable by counters alone.
TEST(LedgerEmitTest, HoldGenerationAgainstAdmittedArcBirths) {
    Loaded L("gen", "    \"signal_roster\": true,\n    \"ledger_emit\": true,\n");
    ASSERT_TRUE(L.cfg.networkSignalRoster());
    ASSERT_TRUE(L.cfg.networkLedgerEmit());
    NetworkTestRun fx(L.cfg);   // the run networkLedgerEmitFold's epoch form takes
    NetworkRunContext& ctx = fx.ctx();
    ctx.ledger_emit.armed = true;
    ctx.arc_ledger.configure(3900.0, kArcLedgerForgetEp);
    auto traces = quietTraces();
    NetworkProductEmitter emitter(0.05, false);
    const ArcLedger::Key a{ 0, "G01", "L1C" };
    const ArcLedger::Key b{ 1, "G01", "L1C" };

    // Epoch 0: both arcs are born and the key is emitted for the first time.
    // A first emission is not a change a consumer could compare, so it is
    // excluded from both classes and counted by the first-sighting half.
    runEpoch(fx, emitter, makeEpoch(kT0, 1), { a, b }, 0);
    EXPECT_EQ(ctx.counters.le.gen_changed, 0);
    EXPECT_EQ(ctx.counters.le.first_phase, 3)   // G01 L1C, G01 L2W and G02 L1C
        << "the emitter has no baseline at all on the first epoch";

    // Epoch 1: the same arcs, the same generation.  Nothing moves.
    runEpoch(fx, emitter, makeEpoch(GnssTime{ 2313, 259230.0 }, 1), { a, b }, 1);
    EXPECT_EQ(ctx.counters.le.gen_changed, 0);
    EXPECT_EQ(ctx.counters.le.birth_no_gen, 0);

    // Epoch 2: the hold restarts (generation 1 -> 2) while both arcs keep
    // tracking: a hold restart with no arc birth behind it.
    runEpoch(fx, emitter, makeEpoch(GnssTime{ 2313, 259260.0 }, 2), { a, b }, 2);
    EXPECT_EQ(ctx.counters.le.gen_changed, 1);
    EXPECT_EQ(ctx.counters.le.gen_no_birth, 1);
    EXPECT_EQ(ctx.counters.le.gen_with_birth, 0);

    // Epoch 3: arc b leaves (an admitted gap opens).
    runEpoch(fx, emitter, makeEpoch(GnssTime{ 2313, 259290.0 }, 2), { a }, 3);
    // Epoch 4: b is back after a gap, so it re-enters.  A re-entry is not a
    // birth, and no generation change is due.
    runEpoch(fx, emitter, makeEpoch(GnssTime{ 2313, 259320.0 }, 2), { a, b }, 4);
    EXPECT_EQ(ctx.counters.le.birth_no_gen, 0)
        << "a re-entry is not a birth: the admitted generation does not advance";

    // Epoch 5: a new arc of the same family is born at a third station while the
    // hold generation stays put: a birth the consumer never learns about.
    const ArcLedger::Key c{ 2, "G01", "L1C" };
    runEpoch(fx, emitter, makeEpoch(GnssTime{ 2313, 259350.0 }, 2), { a, b, c }, 5);
    EXPECT_EQ(ctx.counters.le.birth_no_gen, 1);

    // Epoch 6: a birth and a generation change together (the agreement class).
    const ArcLedger::Key d{ 3, "G01", "L1C" };
    runEpoch(fx, emitter, makeEpoch(GnssTime{ 2313, 259380.0 }, 3), { a, b, c, d }, 6);
    EXPECT_EQ(ctx.counters.le.gen_with_birth, 1);
    EXPECT_EQ(ctx.counters.le.gen_no_birth, 1) << "the earlier class did not move";
    EXPECT_GT(ctx.counters.le.epochs, 0);
    EXPECT_GT(ctx.counters.le.families, 0);
}

/// The emitter's first-sighting test against the ledger: a family the ledger has
/// already admitted is a return, whatever the emitter's baseline map says.
TEST(LedgerEmitTest, EmitterFirstSightingOnAReturnIsCounted) {
    Loaded L("first", "    \"signal_roster\": true,\n    \"ledger_emit\": true,\n");
    NetworkTestRun fx(L.cfg);   // the run networkLedgerEmitFold's epoch form takes
    NetworkRunContext& ctx = fx.ctx();
    ctx.ledger_emit.armed = true;
    ctx.arc_ledger.configure(3900.0, kArcLedgerForgetEp);
    auto traces = quietTraces();
    const ArcLedger::Key a{ 0, "G01", "L1C" };

    // Epoch 0: the ledger admits the arc and a fresh emitter has no baseline, so
    // every emitted key is a first sighting.  The ledger calls G01 L1C a return
    // (its arc was admitted in the same epoch, so its generation is already 1);
    // the keys without an arc are true first sightings.
    {
        NetworkProductEmitter fresh(0.05, false);
        runEpoch(fx, fresh, makeEpoch(kT0, 1), { a }, 0);
    }
    EXPECT_EQ(ctx.counters.le.first_phase, 3);
    EXPECT_EQ(ctx.counters.le.first_phase_return, 1) << "G01 L1C has a ledger arc";
    EXPECT_EQ(ctx.counters.le.first_phase_true, 2)
        << "G01 L2W and G02 L1C have no arc of their own";
    EXPECT_EQ(ctx.counters.le.first_code, 1);
    EXPECT_EQ(ctx.counters.le.first_code_true, 1)
        << "the C1W family has no arc of its own in the ledger";

    // With the baselines seeded, the same epoch is no longer a first sighting.
    NetworkProductEmitter warm(0.05, false);
    SsrCorrectionStore    store;
    NetworkSolutionEpoch  seed = makeEpoch(kT0, 1);
    warm.emitToStore(seed, store);
    const long long before = ctx.counters.le.first_phase;
    runEpoch(fx, warm, makeEpoch(GnssTime{ 2313, 259230.0 }, 1), { a }, 1);
    EXPECT_EQ(ctx.counters.le.first_phase, before);
}

/// Disarmed, both halves return on their first line and nothing is counted,
/// which keeps an unarmed run byte-identical and free of cost.
TEST(LedgerEmitTest, DisarmedTheSeamsDoNothing) {
    Loaded L("off", "");
    NetworkTestRun fx(L.cfg);   // the run networkLedgerEmitFold's epoch form takes
    NetworkRunContext& ctx = fx.ctx();
    EXPECT_FALSE(L.cfg.networkLedgerEmit());
    EXPECT_FALSE(ctx.ledger_emit.armed);
    auto traces = quietTraces();
    NetworkProductEmitter emitter(0.05, false);
    runEpoch(fx, emitter, makeEpoch(kT0, 1), { ArcLedger::Key{ 0, "G01", "L1C" } }, 0);
    EXPECT_EQ(ctx.counters.le.epochs, 0);
    EXPECT_EQ(ctx.counters.le.families, 0);
    EXPECT_EQ(ctx.counters.le.first_phase, 0);
}

/// The knob needs no other: the roster and the ledger it
/// reads are built on every network run, and network.signal_roster is only the
/// ledger's trace switch.  The loader used to refuse it without that key, when an
/// unbuilt roster would have put an empty declaration on the wire.
TEST(LedgerEmitTest, LedgerEmitNeedsNoTraceSwitch) {
    Loaded L("norost", "    \"ledger_emit\": true,\n");
    EXPECT_FALSE(L.cfg.networkSignalRoster());
    EXPECT_TRUE(L.cfg.networkLedgerEmit())
        << "network.ledger_emit must stay armed without network.signal_roster";
}

// ---------------------------------------------------------------------------
// The declaration is built from the roster, in support order (E2)
// ---------------------------------------------------------------------------

TEST(LedgerEmitTest, DeclarationIsBuiltFromTheRosterInSupportOrder) {
    Loaded L("build", "    \"signal_roster\": true,\n    \"ledger_emit\": true,\n");
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ctx.roster.active = true;
    ctx.roster.by_station.assign(3, {});
    // Two stations model L1C, one models L1W: the declaration states both and
    // puts the better-supported one first, which is the order a consumer keyed
    // to the declaration walks.
    for (int i = 0; i < 3; ++i) {
        BandRoster b1;
        b1.modelled_phase = (i < 2) ? "L1C" : "L1W";
        b1.modelled_code  = (i < 2) ? "C1C" : "C1W";
        b1.companion_code = { "C1W" };
        b1.datum_code     = "C1C";
        BandRoster b2;
        b2.modelled_phase = "L2W";
        b2.modelled_code  = "C2W";
        ctx.roster.by_station[i]["GPS"]['1'] = b1;
        ctx.roster.by_station[i]["GPS"]['2'] = b2;
    }
    const std::vector<NetStation> no_stations;   // the latch fall-back is unreachable here
    const NetworkSignalDeclaration d = networkLedgerEmitBuildDeclaration(ctx, no_stations);
    ASSERT_TRUE(d.present);
    const SignalDeclarationBand* b1 = d.find("GPS", '1');
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(b1->phase, (std::vector<std::string>{ "L1C", "L1W" }));
    EXPECT_EQ(b1->code, (std::vector<std::string>{ "C1C", "C1W" }));
    EXPECT_EQ(b1->companion, (std::vector<std::string>{ "C1W" }));
    EXPECT_EQ(b1->datum_code, "C1C");
    const SignalDeclarationBand* b2 = d.find("GPS", '2');
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(b2->phase, (std::vector<std::string>{ "L2W" }));
    EXPECT_TRUE(b2->datum_code.empty()) << "no pin was designated on band 2";

    // A datum designation the epoch's cells disagree on is left empty rather
    // than collapsed to a majority the S-basis never realized.
    ctx.roster.by_station[2]["GPS"]['1'].datum_code = "C1W";
    const NetworkSignalDeclaration d2 = networkLedgerEmitBuildDeclaration(ctx, no_stations);
    EXPECT_TRUE(d2.find("GPS", '1')->datum_code.empty());

    // With no roster set up (a context no run armed) there is nothing to declare,
    // and `present` says so.
    ctx.roster.active = false;
    EXPECT_FALSE(networkLedgerEmitBuildDeclaration(ctx, no_stations).present);

    // A roster whose cells carry no modelled role names nothing a consumer can
    // act on, so the build refuses it rather than declaring an empty network.
    // A constellation's first epoch produces this case, and without the refusal
    // it would be the declaration the consumer's own latch is decided against.
    ctx.roster.active = true;
    for (int i = 0; i < 3; ++i) {
        ctx.roster.by_station[i]["GPS"]['1'] = BandRoster{};
        ctx.roster.by_station[i]["GPS"]['1'].datum_code = "C1C";
        ctx.roster.by_station[i]["GPS"].erase('2');
    }
    const long long before = ctx.counters.le.decl_empty;
    EXPECT_FALSE(networkLedgerEmitBuildDeclaration(ctx, no_stations).present);
    EXPECT_EQ(ctx.counters.le.decl_empty, before + 1);
}

// ---------------------------------------------------------------------------
// The freshness flip keyed on the declaration (C2)
// ---------------------------------------------------------------------------

/// The three-way attribution the consumer's response keys on, and the one class
/// phase 5b suppresses: a signal the declaration does not name is structurally
/// absent, not stale.
TEST(LedgerEmitTest, AbsentByDeclarationIsNotAStalenessEvent) {
    const NetworkSignalDeclaration d = fieldDeclaration();
    // (1) undeclared: the network carries no family for L5Q at all.  Whether a
    //     record is on the wire does not matter; the declaration decides, which
    //     is what makes this class actionable.
    EXPECT_EQ(signalDeclarationAttributeFlip(d, "GPS", "L5Q", true, false),
              FlipAttribution::Undeclared);
    EXPECT_EQ(signalDeclarationAttributeFlip(d, "GPS", "L5Q", true, true),
              FlipAttribution::Undeclared);
    // (2) off wire: the family is declared and this satellite has no record.
    EXPECT_EQ(signalDeclarationAttributeFlip(d, "GPS", "C1W", false, false),
              FlipAttribution::OffWire);
    // (3) stale: a record exists and the age gate refused it.
    EXPECT_EQ(signalDeclarationAttributeFlip(d, "GPS", "C1W", false, true),
              FlipAttribution::Stale);
    // (4) with no declaration nothing can be attributed, so the existing
    //     response stands.  Every wire written without the N record is this case.
    NetworkSignalDeclaration none;
    EXPECT_EQ(signalDeclarationAttributeFlip(none, "GPS", "L5Q", true, false),
              FlipAttribution::NoDeclaration);
    EXPECT_STREQ(flipAttributionName(FlipAttribution::Undeclared), "undeclared");
}

// ---------------------------------------------------------------------------
// The consumer's latch follows the declaration (C1)
// ---------------------------------------------------------------------------

namespace {

/// A GPS epoch whose satellites carry the listed observation codes.
std::vector<RinexObservation> gpsEpoch(const std::vector<std::string>& codes, int n) {
    std::vector<RinexObservation> obs;
    for (int i = 0; i < n; ++i) {
        RinexObservation o;
        o.satellite = "G" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        o.system    = System::GPS;
        for (const std::string& c : codes) o.measurements[c] = 1.0e8 + i;
        obs.push_back(std::move(o));
    }
    return obs;
}

/// The PppModel construction chain, owned so each case is independent.  The
/// priority list is "WC" (the W attribute first), so the station's own count rule
/// latches L1W wherever both are tracked; C1 has to move that latch.
struct ModelFixture {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    std::unique_ptr<ProductParser> pp;
    std::unique_ptr<RinexParser>   rp;
    std::unique_ptr<StateManager>  sm;
    std::unique_ptr<PppModel>      model;
    explicit ModelFixture(const std::string& tag) {
        path = writeTempConfig(tag, "");
        EXPECT_TRUE(cfg.loadFromJson(path, log));
        pp    = std::make_unique<ProductParser>(cfg, log);
        rp    = std::make_unique<RinexParser>(cfg, log, *pp);
        sm    = std::make_unique<StateManager>(5, log, cfg);
        model = std::make_unique<PppModel>(cfg, *pp, log, *rp, *sm);
        model->setCanonicalTraceLabel("AAAA");
    }
    ~ModelFixture() { std::error_code ec; fs::remove(path, ec); }
};

/// The declaration the producer would state with L1C better supported than L1W.
NetworkSignalDeclaration civilFirst() {
    NetworkSignalDeclaration d;
    d.present = true;
    SignalDeclarationBand b1;
    b1.phase = { "L1C", "L1W" };
    b1.code  = { "C1C", "C1W" };
    SignalDeclarationBand b2;
    b2.phase = { "L2W" };
    b2.code  = { "C2W" };
    d.bands["GPS"]['1'] = b1;
    d.bands["GPS"]['2'] = b2;
    return d;
}

}  // namespace

/// C1, branch one: the station tracks a declared signal, so the latch follows
/// the declaration's order instead of the station's own rule.
TEST(LedgerEmitTest, LatchFollowsTheDeclarationWhenTheStationTracksIt) {
    ModelFixture own("c1_own");
    const auto obs = gpsEpoch({ "C1C", "C1W", "L1C", "L1W", "C2W", "L2W" }, 6);

    // The control: the station's own rule under the "WC" priority list.
    PppModelDeclaredLatchTestAccess::decide(*own.model, obs);
    const std::string own_l1 = own.model->latchedCanonicalPhaseOnBandDigit("GPS", '1');
    EXPECT_EQ(own_l1, "L1W");
    EXPECT_EQ(own.model->declaredLatchCounters().compared, 0)
        << "no declaration reached this model, so the class is dead wiring";

    // The arm: the same station and epoch, with the declaration acting.
    ModelFixture act("c1_act");
    act.model->setDeclaredSignalRoster(civilFirst(), /*act=*/true);
    PppModelDeclaredLatchTestAccess::decide(*act.model, obs);
    EXPECT_EQ(act.model->latchedCanonicalPhaseOnBandDigit("GPS", '1'), "L1C");
    EXPECT_EQ(act.model->latchedCanonicalCodeOnBandDigit("GPS", '1'), "C1C");
    EXPECT_EQ(act.model->latchedCanonicalPhaseOnBandDigit("GPS", '2'), "L2W");
    const auto& c = act.model->declaredLatchCounters();
    EXPECT_TRUE(c.armed);
    EXPECT_GE(c.compared, 4);
    EXPECT_EQ(c.from_roster, 2) << "band 1 phase and band 1 code moved";
    EXPECT_GE(c.differ, 2);
    EXPECT_EQ(c.own_not_declared, 0) << "L1W and C1W are both declared";

    // The observatory: the same declaration with act=false compares and counts
    // and leaves the latch where the station's own rule put it.
    ModelFixture obsv("c1_obs");
    obsv.model->setDeclaredSignalRoster(civilFirst(), /*act=*/false);
    PppModelDeclaredLatchTestAccess::decide(*obsv.model, obs);
    EXPECT_EQ(obsv.model->latchedCanonicalPhaseOnBandDigit("GPS", '1'), own_l1);
    EXPECT_FALSE(obsv.model->declaredLatchCounters().armed);
    EXPECT_GE(obsv.model->declaredLatchCounters().differ, 1);
    EXPECT_EQ(obsv.model->declaredLatchCounters().from_roster, 0);
}

/// C1, branch two: the station tracks none of the declared signals of a band, so
/// its own rule stands and the fall-back is counted rather than silent.
TEST(LedgerEmitTest, LatchFallsBackWhenNoDeclaredSignalIsTracked) {
    ModelFixture f("c1_fallback");
    // The producer declares only the W attribute on band 1; this station tracks
    // only the civil one.
    NetworkSignalDeclaration d;
    d.present = true;
    SignalDeclarationBand b1;
    b1.phase = { "L1W" };
    b1.code  = { "C1W" };
    d.bands["GPS"]['1'] = b1;
    f.model->setDeclaredSignalRoster(d, /*act=*/true);

    const auto obs = gpsEpoch({ "C1C", "L1C", "C2W", "L2W" }, 5);
    PppModelDeclaredLatchTestAccess::decide(*f.model, obs);
    EXPECT_EQ(f.model->latchedCanonicalPhaseOnBandDigit("GPS", '1'), "L1C")
        << "the consumer's own rule stands when it tracks no declared signal";
    const auto& c = f.model->declaredLatchCounters();
    EXPECT_EQ(c.from_roster, 0);
    EXPECT_GE(c.none_tracked, 2) << "band 1 phase and band 1 code both fell back";
    EXPECT_GT(c.own_rule, 0);
    // This fall-back is also the MSC class in its widest form: the station's own
    // latch is a signal the network declares no family for, on both the phase
    // and the code of band 1.
    EXPECT_GE(c.own_not_declared, 2);

    // The MSC class alone: the station's own latch is a signal the network
    // declares no family for, a disagreement the run-end table has to name.
    ModelFixture g("c1_notdecl");
    NetworkSignalDeclaration e = d;
    SignalDeclarationBand b1b;
    b1b.phase = { "L1W", "L1C" };   // declared, so the latch can move
    b1b.code  = { "C1W" };          // C1C is not declared
    e.bands["GPS"]['1'] = b1b;
    g.model->setDeclaredSignalRoster(e, /*act=*/false);
    PppModelDeclaredLatchTestAccess::decide(*g.model, obs);
    EXPECT_GE(g.model->declaredLatchCounters().own_not_declared, 1)
        << "C1C is latched here and the network carries no C1C family";
}
