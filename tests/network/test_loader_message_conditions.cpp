/**
 * @file test_loader_message_conditions.cpp
 * @brief Loader messages state the behaviour the code has, under the conditions it has it.
 *
 * Two printed statements were wrong:
 *  - The eq34 hygiene warnings ("phase_datum_eq34_pin_round / carry_shift / rpb_q_m2s is
 *    INERT under carry_invariant") printed for every eq34 run. Those knobs are gated off only
 *    when the invariant carry is armed; on the float vehicle they are live.
 *  - The network.sd_ewl.enable line said "no integer fixing", although
 *    network.sd_ewl.fix_enable provides the integer decision.
 *
 * The graph family of the eq34 chain (network.phase_datum_eq34_family) is read here too: its value,
 * and the two cases in which "pair" gives way to "signal" (no chain, pin bridge), print what they do.
 * So are the event rows (network.phase_datum_eq34_event_rows): the entries it takes, drops and
 * clears, and the line that says which rows are armed.
 */

#include <gtest/gtest.h>

#include "ConfigLoadCapture.hpp"

namespace GnssProcessor {
namespace {

using TestSupport::contains;
using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

}  // namespace

// The eq34 hygiene knobs are live on the float vehicle: no "INERT under carry_invariant".
TEST(LoaderMessages, Eq34HygieneNotInertWithoutInvariantCarry) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_float",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"phase_datum_eq34_pin_round\": true,\n"
        "    \"phase_datum_eq34_carry_shift\": true,\n"
        "    \"phase_datum_eq34_rpb_q_m2s\": 3e-6,\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(contains(r.text, "Eq-34 SINGLE-RECEIVER-CHAIN phase-bias datum ON")) << r.text;
    EXPECT_FALSE(contains(r.text, "INERT under carry_invariant")) << r.text;
}

// Under the invariant carry the same knobs are gated off, and the warnings stay.
TEST(LoaderMessages, Eq34HygieneInertUnderInvariantCarry) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_inv",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"carry_invariant\": true,\n"
        "    \"carry_invariant_qz\": 1e-6,\n"
        "    \"phase_datum_eq34_pin_round\": true,\n"
        "    \"phase_datum_eq34_carry_shift\": true,\n"
        "    \"phase_datum_eq34_rpb_q_m2s\": 3e-6,\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_pin_round is INERT under carry_invariant"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_carry_shift is INERT under carry_invariant"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_rpb_q_m2s is INERT under carry_invariant"))
        << r.text;
}

// The sd_ewl.enable line points at fix_enable instead of denying integer fixing.
TEST(LoaderMessages, SdEwlEnableLineDoesNotDenyFixing) {
    const LoadedConfig r = loadNetworkConfigCapturing("sd_ewl",
        "    \"sd_ewl\": { \"enable\": true },\n");
    EXPECT_TRUE(contains(r.text, "SD PRE-REDUCE EWL constraint ON")) << r.text;
    EXPECT_TRUE(contains(r.text, "integer decisions come only from network.sd_ewl.fix_enable"))
        << r.text;
    EXPECT_FALSE(contains(r.text, "no integer fixing")) << r.text;
}

// The graph family of the eq34 chain (network.phase_datum_eq34_family).  The default is "signal"
// and "pair" is accepted with the chain.
TEST(LoaderMessages, Eq34FamilyDefaultsToSignalAndTakesPairWithTheChain) {
    const LoadedConfig d = loadNetworkConfigCapturing("eq34_family_default",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n");
    EXPECT_TRUE(d.ok) << d.text;
    EXPECT_EQ(d.config.networkPhaseDatumEq34Family(), "signal");
    EXPECT_FALSE(d.config.networkPhaseDatumEq34PairFamily());
    EXPECT_FALSE(contains(d.text, "PAIR FAMILY ON")) << d.text;

    const LoadedConfig p = loadNetworkConfigCapturing("eq34_family_pair",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"phase_datum_eq34_family\": \"pair\",\n");
    EXPECT_TRUE(p.ok) << p.text;
    EXPECT_EQ(p.config.networkPhaseDatumEq34Family(), "pair");
    EXPECT_TRUE(p.config.networkPhaseDatumEq34PairFamily());
    EXPECT_TRUE(contains(p.text, "eq34 PAIR FAMILY ON")) << p.text;
}

// Any other value warns and reads "signal"; the match is exact, so a differently cased "Pair" is one.
TEST(LoaderMessages, Eq34FamilyUnknownValueWarnsAndReadsSignal) {
    for (const char* value : {"triple", "Pair", "signal,pair"}) {
        const LoadedConfig r = loadNetworkConfigCapturing(std::string("eq34_family_bad_") + value[0],
            "    \"phase_leveling\": true,\n"
            "    \"phase_datum_eq34\": true,\n"
            "    \"phase_datum_eq34_family\": \"" + std::string(value) + "\",\n");
        EXPECT_TRUE(r.ok) << value << ": " << r.text;
        EXPECT_EQ(r.config.networkPhaseDatumEq34Family(), "signal") << value;
        EXPECT_FALSE(r.config.networkPhaseDatumEq34PairFamily()) << value;
        EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_family = '" + std::string(value)
                                         + "' is not one of signal|pair"))
            << value << ": " << r.text;
        EXPECT_FALSE(contains(r.text, "PAIR FAMILY ON")) << value << ": " << r.text;
    }
}

// "pair" without the chain has no election to group by band: it warns that it is inert and reads "signal".
TEST(LoaderMessages, Eq34FamilyPairIsInertWithoutTheChain) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_family_no_chain",
        "    \"phase_datum_eq34_family\": \"pair\",\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPhaseDatumEq34Family(), "signal");
    EXPECT_FALSE(r.config.networkPhaseDatumEq34PairFamily());
    EXPECT_TRUE(contains(r.text, "phase_datum_eq34_family = 'pair' is set but .phase_datum_eq34 is OFF")) << r.text;
    EXPECT_TRUE(contains(r.text, "INERT")) << r.text;
    EXPECT_FALSE(contains(r.text, "PAIR FAMILY ON")) << r.text;
}

// The pin bridge grounds a satellite per (satellite, signal) key, which one shared tree cannot hold:
// "pair" with network.phase_datum_time_linked warns and falls back to "signal".
TEST(LoaderMessages, Eq34FamilyPairFallsBackUnderThePinBridge) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_family_bridge",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"eq34_anchor_min_arc_ep\": 3,\n"
        "    \"phase_datum_time_linked\": true,\n"
        "    \"phase_datum_eq34_family\": \"pair\",\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPhaseDatumEq34Family(), "signal");
    EXPECT_FALSE(r.config.networkPhaseDatumEq34PairFamily());
    EXPECT_TRUE(contains(r.text, "phase_datum_eq34_family = 'pair' with network.phase_datum_time_linked")) << r.text;
    EXPECT_TRUE(contains(r.text, "falling back to 'signal'")) << r.text;
    EXPECT_FALSE(contains(r.text, "PAIR FAMILY ON")) << r.text;
    // The bridge itself stays as configured: only the family gave way.
    EXPECT_TRUE(r.config.getNetworkConfig().phase_datum_time_linked);
}

// The event rows of the eq34 chain (network.phase_datum_eq34_event_rows): a list of the entries
// whose integer rule replaces the as-built rule.  The rows are written for the float vehicle on the
// chain datum, which the first lines of each case below set up.
namespace {
const std::string kFloatVehicle =
    "    \"phase_leveling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"ambiguity_vehicle\": \"float_kf\",\n";
const std::string kFloatChain = kFloatVehicle + "    \"phase_datum_eq34\": true,\n";
std::string eventRows(const std::string& json_array) {
    return "    \"phase_datum_eq34_event_rows\": " + json_array + ",\n";
}
}  // namespace

// The default is empty (every row as built); "kept" arms the kept row on the float chain and the
// loader says so, naming the row and that the others run the as-built rule.
TEST(LoaderMessages, Eq34EventRowsDefaultToEmptyAndKeptArmsOnTheFloatChain) {
    const LoadedConfig d = loadNetworkConfigCapturing("eq34_rows_default", kFloatChain);
    EXPECT_TRUE(d.ok) << d.text;
    EXPECT_TRUE(d.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_FALSE(d.config.networkPhaseDatumEq34EventRowArmed("kept"));
    EXPECT_FALSE(contains(d.text, "EVENT ROWS ON")) << d.text;

    const LoadedConfig k = loadNetworkConfigCapturing("eq34_rows_kept", kFloatChain + eventRows("[\"kept\"]"));
    EXPECT_TRUE(k.ok) << k.text;
    EXPECT_EQ(k.config.networkPhaseDatumEq34EventRows(), std::vector<std::string>{"kept"});
    EXPECT_TRUE(k.config.networkPhaseDatumEq34EventRowArmed("kept"));
    EXPECT_FALSE(k.config.networkPhaseDatumEq34EventRowArmed("returning"));
    EXPECT_TRUE(contains(k.text, "eq34 EVENT ROWS ON (kept)")) << k.text;
    EXPECT_TRUE(contains(k.text, "The other rows run the as-built rule")) << k.text;
    // The line of a run with the kept row alone is the line it always was.
    EXPECT_TRUE(contains(k.text,
        "eq34 EVENT ROWS ON (kept) - the integer rule of each armed row replaces the as-built rule for its class of "
        "tree edge: 'kept' holds an arc that stays in the tree, in either direction, with its pinned life continuing "
        "at its previous pin target, with no frame step of its own.  The other rows run the as-built rule.  The "
        "run-end eq34 block reports the edges held and the incumbent edges not held, by cause.")) << k.text;
    EXPECT_FALSE(contains(k.text, "'returning'")) << k.text;

    // An empty list is the default.
    const LoadedConfig e = loadNetworkConfigCapturing("eq34_rows_empty", kFloatChain + eventRows("[]"));
    EXPECT_TRUE(e.ok) << e.text;
    EXPECT_TRUE(e.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_FALSE(contains(e.text, "EVENT ROWS ON")) << e.text;
}

// An entry the event table does not name is dropped with a warning that names the key and the
// entry; a duplicate is dropped without a word; a value that is no array is ignored.
TEST(LoaderMessages, Eq34EventRowsUnknownEntryWarnsAndADuplicateIsDroppedSilently) {
    const LoadedConfig u = loadNetworkConfigCapturing("eq34_rows_unknown",
        kFloatChain + eventRows("[\"kept\", \"Kept\", \"bogus\", 7]"));
    EXPECT_TRUE(u.ok) << u.text;
    EXPECT_EQ(u.config.networkPhaseDatumEq34EventRows(), std::vector<std::string>{"kept"});
    for (const char* bad : {"Kept", "bogus"})
        EXPECT_TRUE(contains(u.text, std::string("network.phase_datum_eq34_event_rows entry '") + bad
                                         + "' is not one of kept|returning|rehost|root|slip|merge_split"))
            << bad << ": " << u.text;
    EXPECT_TRUE(contains(u.text, "holds a non-string entry")) << u.text;
    EXPECT_TRUE(contains(u.text, "eq34 EVENT ROWS ON (kept)")) << u.text;

    const LoadedConfig dup = loadNetworkConfigCapturing("eq34_rows_dup",
        kFloatChain + eventRows("[\"kept\", \"kept\"]"));
    EXPECT_TRUE(dup.ok) << dup.text;
    EXPECT_EQ(dup.config.networkPhaseDatumEq34EventRows(), std::vector<std::string>{"kept"});
    EXPECT_FALSE(contains(dup.text, "network.phase_datum_eq34_event_rows")) << dup.text;   // no message names the key
    EXPECT_TRUE(contains(dup.text, "eq34 EVENT ROWS ON (kept)")) << dup.text;

    const LoadedConfig str = loadNetworkConfigCapturing("eq34_rows_string",
        kFloatChain + eventRows("\"kept\""));
    EXPECT_TRUE(str.ok) << str.text;
    EXPECT_TRUE(str.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_TRUE(contains(str.text, "network.phase_datum_eq34_event_rows must be an array")) << str.text;
}

// The rows are built in table order: an entry whose rule is not built yet is dropped with a warning
// and arms nothing.
TEST(LoaderMessages, Eq34EventRowsNotBuiltYetAreDroppedWithAWarning) {
    for (const char* entry : {"root", "slip", "merge_split"}) {
        const LoadedConfig r = loadNetworkConfigCapturing(std::string("eq34_rows_nb_") + entry,
            kFloatChain + eventRows(std::string("[\"") + entry + "\"]"));
        EXPECT_TRUE(r.ok) << entry << ": " << r.text;
        EXPECT_TRUE(r.config.networkPhaseDatumEq34EventRows().empty()) << entry;
        EXPECT_FALSE(r.config.networkPhaseDatumEq34EventRowArmed(entry)) << entry;
        EXPECT_TRUE(contains(r.text, std::string("the rows are built in table order; '") + entry
                                         + "' is not built yet")) << entry << ": " << r.text;
        EXPECT_FALSE(contains(r.text, "EVENT ROWS ON")) << entry << ": " << r.text;
    }
    // One built entry beside an unbuilt one: the built one arms.
    const LoadedConfig mixed = loadNetworkConfigCapturing("eq34_rows_nb_mixed",
        kFloatChain + eventRows("[\"root\", \"kept\"]"));
    EXPECT_TRUE(mixed.ok) << mixed.text;
    EXPECT_EQ(mixed.config.networkPhaseDatumEq34EventRows(), std::vector<std::string>{"kept"});
    EXPECT_TRUE(contains(mixed.text, "'root' is not built yet")) << mixed.text;
}

// The rehost row is built, its certified branch held: its entry arms on the float chain beside the others, in the
// order the config gave them, and the loader names what it does; the kept-only and kept-and-returning lines are
// the lines they were.
TEST(LoaderMessages, Eq34EventRowsRehostArmsOnTheFloatChainAndTheLoaderSaysWhatItDoes) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_rows_rehost", kFloatChain + eventRows("[\"rehost\"]"));
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPhaseDatumEq34EventRows(), std::vector<std::string>{"rehost"});
    EXPECT_TRUE(r.config.networkPhaseDatumEq34EventRowArmed("rehost"));
    EXPECT_FALSE(r.config.networkPhaseDatumEq34EventRowArmed("kept"));
    EXPECT_TRUE(contains(r.text, "eq34 EVENT ROWS ON (rehost)")) << r.text;
    EXPECT_TRUE(contains(r.text,
        "'rehost' releases every re-host, its certified branch held: the re-hosted vertex and its subtree take the cut "
        "release, the new pin keeps its rounded integer target with an unknown step, and the subtree's keys are "
        "declared moved.")) << r.text;
    EXPECT_TRUE(contains(r.text, "The run-end eq34 block reports the re-hosts released, what the release took and the "
                                 "keys declared.")) << r.text;
    EXPECT_FALSE(contains(r.text, "is not built yet")) << r.text;
    EXPECT_FALSE(contains(r.text, "'kept' holds")) << r.text;
    EXPECT_FALSE(contains(r.text, "'returning' releases")) << r.text;

    const LoadedConfig all = loadNetworkConfigCapturing("eq34_rows_all_three",
        kFloatChain + eventRows("[\"kept\", \"returning\", \"rehost\"]"));
    EXPECT_TRUE(all.ok) << all.text;
    EXPECT_EQ(all.config.networkPhaseDatumEq34EventRows(), (std::vector<std::string>{"kept", "returning", "rehost"}));
    EXPECT_TRUE(contains(all.text, "eq34 EVENT ROWS ON (kept, returning, rehost)")) << all.text;
    EXPECT_TRUE(contains(all.text, "'kept' holds an arc that stays in the tree")) << all.text;
    EXPECT_TRUE(contains(all.text, "'returning' releases a vertex that rejoins the tree")) << all.text;
    EXPECT_TRUE(contains(all.text, "'rehost' releases every re-host")) << all.text;
    EXPECT_TRUE(contains(all.text, "The run-end eq34 block reports the edges held and the incumbent edges not held, by "
                                   "cause; the edges released, what the release took and the keys declared; the "
                                   "re-hosts released, what the release took and the keys declared.")) << all.text;

    // The text of the kept row beside the returning row is the text it was.
    const LoadedConfig kr = loadNetworkConfigCapturing("eq34_rows_kept_returning_text",
        kFloatChain + eventRows("[\"kept\", \"returning\"]"));
    EXPECT_FALSE(contains(kr.text, "'rehost'")) << kr.text;
    EXPECT_TRUE(contains(kr.text, "eq34 EVENT ROWS ON (kept, returning)")) << kr.text;
    EXPECT_TRUE(contains(kr.text, "The run-end eq34 block reports the edges held and the incumbent edges not held, by "
                                  "cause; the edges released, what the release took and the keys declared.")) << kr.text;

    // Without the chain the entry is cleared like any other.
    const LoadedConfig nochain = loadNetworkConfigCapturing("eq34_rows_rehost_nochain",
        kFloatVehicle + eventRows("[\"rehost\"]"));
    EXPECT_TRUE(nochain.ok) << nochain.text;
    EXPECT_TRUE(nochain.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_TRUE(contains(nochain.text, "event rows are inert without the chain")) << nochain.text;
}

// The returning row is built: its entry arms on the float chain, alone or beside the kept row, in the order the
// config gave them, and the loader names each armed row and what it does (and the rows that stay as built).
TEST(LoaderMessages, Eq34EventRowsReturningArmsOnTheFloatChainAndTheLoaderSaysWhatItDoes) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_rows_returning", kFloatChain + eventRows("[\"returning\"]"));
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPhaseDatumEq34EventRows(), std::vector<std::string>{"returning"});
    EXPECT_TRUE(r.config.networkPhaseDatumEq34EventRowArmed("returning"));
    EXPECT_FALSE(r.config.networkPhaseDatumEq34EventRowArmed("kept"));
    EXPECT_TRUE(contains(r.text, "eq34 EVENT ROWS ON (returning)")) << r.text;
    EXPECT_TRUE(contains(r.text, "'returning' releases a vertex that rejoins the tree after an absence")) << r.text;
    EXPECT_TRUE(contains(r.text, "the keys of its subtree are declared moved")) << r.text;
    EXPECT_TRUE(contains(r.text, "The other rows run the as-built rule")) << r.text;
    EXPECT_TRUE(contains(r.text, "The run-end eq34 block reports the edges released, what the release took and the "
                                 "keys declared.")) << r.text;
    EXPECT_FALSE(contains(r.text, "'kept' holds")) << r.text;              // the kept row is not armed: not described
    EXPECT_FALSE(contains(r.text, "is not built yet")) << r.text;

    const LoadedConfig both = loadNetworkConfigCapturing("eq34_rows_kept_returning",
        kFloatChain + eventRows("[\"kept\", \"returning\"]"));
    EXPECT_TRUE(both.ok) << both.text;
    EXPECT_EQ(both.config.networkPhaseDatumEq34EventRows(), (std::vector<std::string>{"kept", "returning"}));
    EXPECT_TRUE(contains(both.text, "eq34 EVENT ROWS ON (kept, returning)")) << both.text;
    EXPECT_TRUE(contains(both.text, "'kept' holds an arc that stays in the tree")) << both.text;
    EXPECT_TRUE(contains(both.text, "'returning' releases a vertex that rejoins the tree")) << both.text;
    EXPECT_TRUE(contains(both.text, "The run-end eq34 block reports the edges held and the incumbent edges not held, by "
                                    "cause; the edges released, what the release took and the keys declared.")) << both.text;

    // Without the chain, or off the float vehicle, the entry is cleared like any other.
    const LoadedConfig nochain = loadNetworkConfigCapturing("eq34_rows_returning_nochain",
        kFloatVehicle + eventRows("[\"returning\"]"));
    EXPECT_TRUE(nochain.ok) << nochain.text;
    EXPECT_TRUE(nochain.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_FALSE(nochain.config.networkPhaseDatumEq34EventRowArmed("returning"));
    EXPECT_TRUE(contains(nochain.text, "event rows are inert without the chain")) << nochain.text;
}

// Without the chain the list is inert and cleared; off the float vehicle, or with the invariant
// carry, it is cleared with the reason that the rows are written for the float vehicle.
TEST(LoaderMessages, Eq34EventRowsAreInertWithoutTheChainAndWrittenForTheFloatVehicle) {
    const LoadedConfig nochain = loadNetworkConfigCapturing("eq34_rows_nochain",
        kFloatVehicle + eventRows("[\"kept\"]"));
    EXPECT_TRUE(nochain.ok) << nochain.text;
    EXPECT_TRUE(nochain.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_TRUE(contains(nochain.text, "event rows are inert without the chain")) << nochain.text;
    EXPECT_FALSE(contains(nochain.text, "EVENT ROWS ON")) << nochain.text;

    const LoadedConfig level = loadNetworkConfigCapturing("eq34_rows_level",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n" + eventRows("[\"kept\"]"));
    EXPECT_TRUE(level.ok) << level.text;
    EXPECT_TRUE(level.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_TRUE(contains(level.text, "the event rows are written for the float vehicle")) << level.text;
    EXPECT_FALSE(contains(level.text, "EVENT ROWS ON")) << level.text;

    // The float vehicle refuses the invariant carry outright; the list is cleared first.
    const LoadedConfig inv = loadNetworkConfigCapturing("eq34_rows_invariant",
        kFloatChain + "    \"carry_invariant\": true,\n    \"carry_invariant_qz\": 1e-6,\n"
        + eventRows("[\"kept\"]"));
    EXPECT_TRUE(inv.config.networkPhaseDatumEq34EventRows().empty());
    EXPECT_TRUE(contains(inv.text, "is set with network.carry_invariant")) << inv.text;
    EXPECT_FALSE(contains(inv.text, "EVENT ROWS ON")) << inv.text;
}

}  // namespace GnssProcessor
