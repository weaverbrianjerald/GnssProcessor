/**
 * @file test_chain_named_rows.cpp
 * @brief The chain and ring-fix counts that name their links (card CNN rows 1, 2 and 4, corrections item 7).
 *
 * One case per stage: the pair family's elections per signal by constellation, signal set and cause; the listed
 * ring-fix pairs with the link-epochs each was considered at, the families left out and a listed pair that matched
 * nothing; the hold rows that never reached the solve by cause and link, with the closure of the hold-row counts;
 * the refused link-epochs by cause with their first links.  The tally they share keeps the first ten keys.
 */
#include "network/ar/RingFix.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/carry/Coupling.hpp"
#include "network/diag/BoundedTally.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/solve/ChainDatum.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"
#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "math/TimeUtils.hpp"
#include "../support/NetworkTestRun.hpp"
#include "ConfigLoadCapture.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// A logger that writes into @p text.
std::shared_ptr<spdlog::logger> textLogger(const std::string& name, std::ostringstream& text) {
    auto log = std::make_shared<spdlog::logger>(name, std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
    log->set_level(spdlog::level::info);
    return log;
}

/// What @p report prints over the fixture's run.
std::string runEnd(NetworkTestRun& fx, const std::shared_ptr<spdlog::logger>& log, std::ostringstream& text,
                   int (*report)(const NetworkRunEndView&)) {
    NetworkRun& run = fx.run();
    const NetworkRunEndInputs inputs{run.engines, run.emit, run.timing, run.closed_loop};
    const NetworkRunEndView view{fx.ctx(), fx.config(), log, fx.traces(), fx.setup(), inputs, /*loop_ms=*/0.0};
    EXPECT_EQ(report(view), 0);
    log->flush();
    return text.str();
}

/// The line of @p text that contains @p tag ("" when none).
std::string lineWith(const std::string& text, const std::string& tag) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (line.find(tag) != std::string::npos) return line;
    return {};
}

}  // namespace

// ---- The tally ----

TEST(ChainNamedRows, TheTallyNamesTheFirstTenKeysAndCountsTheRestExactly) {
    BoundedTally<std::string> t;
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.text([](const std::string& s) { return s; }), "none");
    for (int k = 0; k < 12; ++k) t.add("k" + std::to_string(k));
    t.add("k0", 4);    // a kept key keeps counting
    t.add("k11", 2);   // a key past the bound goes to the remainder
    ASSERT_EQ(t.kept.size(), kBoundedTallyCap);
    EXPECT_EQ(t.kept.front().first, "k0");
    EXPECT_EQ(t.kept.front().second, 5);
    EXPECT_EQ(t.kept.back().first, "k9");
    EXPECT_EQ(t.more, 4);   // k10 once, k11 once and twice more
    EXPECT_EQ(t.total(), 12 + 4 + 2);
    const std::string text = t.text([](const std::string& s) { return s; });
    EXPECT_EQ(text.rfind("k0 x5, k1 x1", 0), 0u) << text;
    EXPECT_NE(text.find("k9 x1 and 4 more"), std::string::npos) << text;
}

// ---- Stage 1: the pair family's elections per signal ----

TEST(ChainNamedRows, EachElectionPerSignalUnderThePairFamilyHasItsCause) {
    EXPECT_EQ(chainPairFallbackCause(System::Galileo, {"L1C"}, false), ChainPairFallbackCause::OneSignal);
    EXPECT_EQ(chainPairFallbackCause(System::GPS, {"L1C", "L1W"}, false), ChainPairFallbackCause::TwoOfOneBand);
    EXPECT_EQ(chainPairFallbackCause(System::Galileo, {"L1C", "L1X", "L5Q"}, false), ChainPairFallbackCause::ThreeOrMore);
    EXPECT_EQ(chainPairFallbackCause(System::Galileo, {"L1C", "L1X", "L5Q"}, true),
              ChainPairFallbackCause::NotInConfiguredPairs);
    EXPECT_EQ(chainPairFallbackKeyText({System::Galileo, {"L1C", "L1X", "L5Q"}, ChainPairFallbackCause::ThreeOrMore}),
              "E {L1C L1X L5Q} three or more");
    EXPECT_STREQ(chainPairFallbackCauseName(ChainPairFallbackCause::NotInConfiguredPairs),
                 "a signal not in the configured pairs");
}

TEST(ChainNamedRows, ThePairFamilyLinesEndWithEveryConstellationAndTheSignalSetsElectedPerSignal) {
    const TestSupport::LoadedConfig loaded = TestSupport::loadNetworkConfigCapturing(
        "cnn_pair_family", "    \"phase_leveling\": true,\n"
                           "    \"shared_carry_full_cov\": true,\n"
                           "    \"carry_conditional_coupling\": true,\n"
                           "    \"ambiguity_vehicle\": \"float_kf\",\n"
                           "    \"phase_datum_eq34\": true,\n"
                           "    \"phase_datum_eq34_family\": \"pair\",\n");
    ASSERT_TRUE(loaded.ok) << loaded.text;
    std::ostringstream text;
    const auto log = textLogger("cnn_pair_family", text);
    auto fx = std::make_unique<NetworkTestRun>(loaded.config, log);
    ASSERT_TRUE(fx->ctx().knobs.eq34_pair_family);
    ChainDatumCounters& c = fx->ctx().counters;
    c.eq34_pair_families = 240;   // GPS formed a pair at every epoch
    c.eq34_pair_epochs_by_sys[System::GPS] = 240;
    c.eq34_pair_epochs_by_sys[System::Galileo] = 240;
    c.eq34_pair_fallbacks = 3;
    c.eq34_pair_fallbacks_by_sys[System::Galileo] = 3;
    c.eq34_pair_fallback_sets.add({System::Galileo, {"L1C", "L1X", "L5Q"}, ChainPairFallbackCause::ThreeOrMore}, 2);
    c.eq34_pair_fallback_sets.add({System::Galileo, {"L1C"}, ChainPairFallbackCause::OneSignal});
    const std::string line = lineWith(runEnd(*fx, log, text, networkChainDatumRunEnd), "eq34 pair family");
    EXPECT_NE(line.find("240 pair family(ies) elected, 3 constellation-epoch(s) elected per signal"),
              std::string::npos) << line;
    EXPECT_NE(line.find("Per constellation: GPS 0 of 240 constellation-epoch(s) elected per signal, Galileo 3 of 240;"),
              std::string::npos) << line;
    EXPECT_NE(line.find("per signal: E {L1C L1X L5Q} three or more x2, E {L1C} one signal x1."), std::string::npos)
        << line;
}

// ---- Stage 2: the listed ring-fix pairs ----

TEST(ChainNamedRows, EachListedPairIsCountedAndOneThatMatchedNoChordIsNamed) {
    std::ostringstream text;
    const auto log = textLogger("cnn_pairs", text);
    auto fx = std::make_unique<NetworkTestRun>(GnssConfig{}, log);
    fx->ctx().carry.ring_fix = std::make_unique<NetworkRingFix>();
    NetworkRingFix& rf = *fx->ctx().carry.ring_fix;
    rf.pairs = {{"E", "L1X", "L5X"}, {"E", "L1C", "L5Q"}, {"G", "L1C", "L2W"}};
    rf.counters.pair_link_epochs["E:L1C/L5Q"] = 120;
    rf.counters.pair_link_epochs["G:L1C/L2W"] = 300;
    rf.counters.excluded_link_epochs = 7;
    rf.counters.excluded_families.add("E:L1C/L5X", 7);
    const std::string out = runEnd(*fx, log, text, networkRingFixRunEnd);
    const std::string pairs = lineWith(out, "chord link-epoch(s) of other pair families");
    EXPECT_NE(pairs.find("7 chord link-epoch(s) of other pair families were not considered (E:L1C/L5X x7); "
                         "considered by listed pair: E:L1C/L5Q 120, E:L1X/L5X 0, G:L1C/L2W 300."),
              std::string::npos) << pairs;
    const std::string unmatched = lineWith(out, "matched no chord");
    EXPECT_NE(unmatched.find("the listed pair E:L1X/L5X matched no chord link-epoch over the run"), std::string::npos)
        << out;
    EXPECT_EQ(lineWith(out, "E:L1C/L5Q matched no chord"), "");
}

// ---- Stage 3: the hold rows that never reached the solve, and the refused link-epochs ----

namespace {

/// One station with five ambiguity columns and a hold-table hit on each, every skip cause met once.
struct HoldRowFixture {
    std::ostringstream                   text;
    std::shared_ptr<spdlog::logger>      log;
    std::unique_ptr<NetworkTestRun>      run;
    std::unique_ptr<GlobalIonosphereMap> gim;
    std::vector<StationAssembly>         slots;
    Eigen::VectorXd                      x_lin;
    GlobalVtecShConfig                   shc{};
    struct Arc { const char* sat; const char* sig; };
    // Columns 0 to 4 in the station's map order (by satellite, then signal).
    static constexpr Arc kArcs[] = {{"G05", "L1C"}, {"G05", "L2W"}, {"G07", "L1C"}, {"G07", "L2W"}, {"G09", "L1C"}};

    HoldRowFixture() {
        log = textLogger("cnn_hold_rows", text);
        run = std::make_unique<NetworkTestRun>(GnssConfig{}, log);
        run->setup().stations.resize(1);
        run->setup().stations[0].name = "AAAA00SIM";
        x_lin = Eigen::VectorXd(4);   // column 4 lies past the linearization point
        slots.resize(1);
        for (int k = 0; k < 5; ++k) {
            LocalSlot s;
            const SatId sat = *SatId::parse(kArcs[k].sat);
            s.ref = ambiguityRef(sat, *SignalId::fromObsCode(System::GPS, kArcs[k].sig));
            s.x_lin = 1.0 + k;
            EXPECT_TRUE(addParam(run->setup().stations[0].params, s));
            if (k < 4) x_lin(k) = s.x_lin;
            if (k != 3) slots[0].h2local[k] = k;   // column 3 is not a column of the epoch
        }
        NetworkEpoch& e = run->newEpoch(GnssTime{2313, 259230.0});
        e.clock = EpochClock{30.0, TimeUtils::continuousGpsSeconds(e.se.time), true};
        // G05 L2W is a tree edge (its pin is the frame).
        run->setup().stations[0].pinned_amb_targets[StateKeys::makeAmbiguityKey("G05", "L2W")] = 0.0;
        run->ctx().carry.ring_fix = std::make_unique<NetworkRingFix>();
        NetworkRingFix& rf = *run->ctx().carry.ring_fix;
        rf.table[{0, "G05", "L1C"}] = 1.0;    // folded
        rf.table[{0, "G05", "L2W"}] = 2.0;    // on a tree edge
        rf.table[{0, "G07", "L1C"}] = std::numeric_limits<double>::quiet_NaN();   // a non-finite row
        rf.table[{0, "G07", "L2W"}] = 4.0;    // not a column of the epoch
        rf.table[{0, "G09", "L1C"}] = 5.0;    // index past the state
        rf.links[{0, "G07", "L1C", "L2W"}].ring = 2;
        rf.links[{0, "G09", "L1C", "L2W"}].ring = 3;
    }

    /// PASS B's hold-row step at the station, then the serial tally.
    void passB() {
        NetworkEpoch& e = run->ep();
        StationBuild b{run->ctx(), run->config(), run->est(), run->setup(), e.se, e.clock,
                       e.pass_a, e.events, gim, 0, run->setup().stations[0], slots[0]};
        StationCarryFrame carry{b, /*legacy_local_inject=*/true, /*do_amb_carry=*/true, x_lin,
                                CouplingRowBuilder{run->est(), shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4,
                                                   /*q_pb=*/1e-4, /*tu=*/false, /*gauge=*/false,
                                                   /*reinit=*/false, run->ctx().entities.sat_signals, slots[0]}};
        networkRingFixPriors(carry);
        networkRingFixStationTally(run->ctx(), slots);
    }
};

}  // namespace

TEST(ChainNamedRows, EachHoldRowThatMissedTheSolveNamesItsLinkByCauseAndTheCountsClose) {
    HoldRowFixture f;
    f.passB();
    const StationAssembly& R = f.slots[0];
    EXPECT_EQ(R.rfix_offered, 5);
    EXPECT_EQ(R.rfix_rows, 1);
    EXPECT_EQ(R.rfix_gauge_skipped, 1);
    EXPECT_EQ(R.rfix_no_column_index, 1);
    EXPECT_EQ(R.rfix_no_column_local, 1);
    EXPECT_EQ(R.rfix_no_column_eta, 1);
    const RingFixCounters& C = f.run->ctx().carry.ring_fix->counters;
    EXPECT_EQ(C.rows_offered, C.rows_injected + C.rows_gauge_skipped + C.rows_no_column_index
                                  + C.rows_no_column_local + C.rows_no_column_eta);
    const auto only = [&](RingFixRowSkip c) {
        const BoundedTally<RingFixNamedLink>& t = C.rows_skipped_links[static_cast<std::size_t>(c)];
        EXPECT_EQ(t.kept.size(), 1u) << ringFixRowSkipName(c);
        return t.kept.empty() ? RingFixNamedLink{} : t.kept.front().first;
    };
    EXPECT_EQ(only(RingFixRowSkip::TreeEdge).signals, "L2W");   // no link of G05 is known: the signal alone
    EXPECT_EQ(only(RingFixRowSkip::NonFiniteRow).signals, "L1C of L1C/L2W");
    EXPECT_EQ(only(RingFixRowSkip::NonFiniteRow).ring, 2);
    EXPECT_EQ(only(RingFixRowSkip::NoLocalColumn).signals, "L2W of L1C/L2W");
    EXPECT_EQ(only(RingFixRowSkip::IndexPastState).sat, "G09");
    EXPECT_EQ(only(RingFixRowSkip::IndexPastState).ring, 3);

    const std::string out = runEnd(*f.run, f.log, f.text, networkRingFixRunEnd);
    const std::string head = lineWith(out, "hold-table hit(s)");
    EXPECT_NE(head.find("5 hold-table hit(s): 1 hold row(s) folded, 1 skipped on a tree edge, 3 without a column (1 index "
                        "past the state, 1 not a column of the epoch, 1 a non-finite row; CLOSED)."),
              std::string::npos) << head;
    const std::string named = lineWith(out, "hold rows not folded");
    EXPECT_NE(named.find("on a tree edge 1: AAAA00SIM-G05 L2W no ring x1"), std::string::npos) << named;
    EXPECT_NE(named.find("index past the state 1: AAAA00SIM-G09 L1C of L1C/L2W ring 3 x1"), std::string::npos) << named;
    EXPECT_NE(named.find("not a column of the epoch 1: AAAA00SIM-G07 L2W of L1C/L2W ring 2 x1"), std::string::npos)
        << named;
    EXPECT_NE(named.find("a non-finite row 1: AAAA00SIM-G07 L1C of L1C/L2W ring 2 x1"), std::string::npos) << named;
}

TEST(ChainNamedRows, RefusedLinkEpochsNameTheirFirstLinksByCause) {
    NetworkRingFix rf;
    RingFixDecideObs a;
    a.id = {4, "G12", "L1C", "L2W"};
    a.ring = 1;
    a.candidate = true;   // no float: refused for that
    RingFixDecideObs b = a;
    b.id = {6, "E11", "L1C", "L5Q"};
    b.ring = 2;
    b.candidate = false;  // not its ring's turn
    for (int k = 0; k < 3; ++k) ringFixDecideStep(rf, {a, b}, 1000.0 + 30.0 * k);
    const BoundedTally<RingFixNamedLink>& nf = rf.counters.refused_links[static_cast<std::size_t>(RingFixRefuse::NoFloat)];
    const BoundedTally<RingFixNamedLink>& nc =
        rf.counters.refused_links[static_cast<std::size_t>(RingFixRefuse::NotCandidate)];
    ASSERT_EQ(nf.kept.size(), 1u);
    EXPECT_EQ(nf.kept.front().first, (RingFixNamedLink{4, "G12", "L1C/L2W", 1}));
    EXPECT_EQ(nf.kept.front().second, 3);
    ASSERT_EQ(nc.kept.size(), 1u);
    EXPECT_EQ(nc.kept.front().first, (RingFixNamedLink{6, "E11", "L1C/L5Q", 2}));
    EXPECT_EQ(nf.total(), rf.counters.refused[static_cast<std::size_t>(RingFixRefuse::NoFloat)]);
    EXPECT_EQ(nc.total(), rf.counters.refused[static_cast<std::size_t>(RingFixRefuse::NotCandidate)]);
    EXPECT_EQ(ringFixNamedLinkText(nf.kept.front().first, {"A", "B", "C", "D", "BOGT00COL"}), "BOGT00COL-G12 L1C/L2W ring 1");
    EXPECT_EQ(ringFixNamedLinkText(nc.kept.front().first, {"A"}), "#6-E11 L1C/L5Q ring 2");
}
