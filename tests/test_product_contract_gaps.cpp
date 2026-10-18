/**
 * @file test_product_contract_gaps.cpp
 * @brief The S contract line declares the wind-up mode, and @c checkContract compares
 *        every replay-emitter parameter, the code-jump and set-change flags included.
 *
 * Two gaps are closed:
 *   - The S line named the wind-up construction (field 3) but not the mode it was applied
 *     in (off, fractional, accumulated).  A replay or real-time consumer on another mode
 *     than the producer sits in a different phase frame by whole cycles per satellite and
 *     signal; its float ambiguities absorb the difference, so only the contract can show it.
 *     The producer now stamps @c wumode=<mode>; a wire without the token is undeclared and
 *     still replays.
 *   - @c checkContract compared five of the seven replay-emitter construction parameters.
 *     A consumer replaying a wire that carried @c cbjump=1 or @c clkset=1 with an emitter
 *     built without that channel reproduced different discontinuity counters and passed.
 *
 * Suite @c ProductContractGaps (owned by this file alone):
 *  - the three modes and an unknown future mode round-trip through the S line, the legend
 *    names the token, and a contract without a mode writes the S line byte for byte as before;
 *  - a consumer whose mode differs from the wire's is refused, for every pair of modes, and
 *    the refusal names both values;
 *  - an undeclared wire is not refused, and neither is a consumer that declares no mode;
 *  - a mid-file re-broadcast carries the mode, and the per-receipt check refuses the receipt
 *    of a producer restarted on another mode;
 *  - cbjump and clkset are compared in both directions, and each of the seven replay-emitter
 *    parameters, flipped alone, is refused;
 *  - @c windUpModeOf follows the loaded configuration, and the closed-loop consumer derivation
 *    keeps it, so the replay driver's expectation (the consumer's own configuration) is the
 *    mode the consumer runs.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductIO.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

const GnssTime kT0{ 2313, 259200.0 };

/// A contract a producer would write: every field the checks compare is set, so a
/// flipped field is a real difference and not a default.
SbasisContract baseContract() {
    SbasisContract c;
    c.decl.reference_receiver_id = "CIT100USA";
    c.decl.clock_pivot_sat["G"]  = "G05";
    c.apriori_product_id         = "COD0MGXFIN";
    c.disc_threshold_m           = 0.05;
    c.emit_clock_disc            = true;
    c.present                    = true;
    return c;
}

/// One satellite with a clock and a phase bias, so a header is followed by an epoch.
NetworkSolutionEpoch epochAt(const GnssTime& t) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = t;
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    NetworkSatSolution ns;
    ns.sat = "G01"; ns.has_clock = true; ns.clock_c0_m = 1.0;
    ns.phase_bias["L1C"] = NetworkPhaseBias{ 0.1, true, 1 };
    sol.sats.push_back(ns);
    return sol;
}

/// The first line of @p path that starts with @p prefix ("" when none).
std::string pick(const fs::path& path, const char* prefix) {
    std::ifstream f(path);
    std::string ln;
    while (std::getline(f, ln))
        if (ln.rfind(prefix, 0) == 0) return ln;
    return std::string{};
}

/// Write @p c as a v2 header (plus one epoch), read it back and return the parsed contract.
SbasisContract roundTrip(const SbasisContract& c, const std::string& name,
                         fs::path* path_out = nullptr) {
    const fs::path p = gnss_test::tempDir() / name;
    std::error_code ec;
    fs::remove(p, ec);
    {
        NetworkProductWriter w(p.string(), c);
        EXPECT_TRUE(w.good());
        w.write(epochAt(kT0));
    }
    bool ok = false;
    SbasisContract parsed;
    NetworkProductReader::read(p.string(), &parsed, &ok);
    EXPECT_TRUE(ok);
    if (path_out) *path_out = p;
    return parsed;
}

/// True when @p mism has a line containing @p needle.
bool mentions(const std::vector<std::string>& mism, const std::string& needle) {
    for (const auto& m : mism)
        if (m.find(needle) != std::string::npos) return true;
    return false;
}

/// A network configuration file with @p estimation_json and @p model_json as the
/// @c estimation and @c model_corrections sections ("" omits the section).
fs::path writeConfig(const std::string& tag, const std::string& estimation_json,
                     const std::string& model_json) {
    const fs::path p = gnss_test::tempDir() / ("contract_gaps_" + tag + ".json");
    std::ofstream f(p);
    f << "{\n"
         "  \"processing\": {\n"
         "    \"mode\": \"ppp-rtk-network\",\n"
         "    \"active_systems\": [ \"GPS\" ],\n"
         "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
         "  },\n";
    if (!estimation_json.empty()) f << "  \"estimation\": { " << estimation_json << " },\n";
    if (!model_json.empty())      f << "  \"model_corrections\": { " << model_json << " },\n";
    f << "  \"network\": {\n"
         "    \"clock_datum_station\": \"AAAA\",\n"
         "    \"stations\": [\n"
         "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
         "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
         "    ]\n"
         "  }\n"
         "}\n";
    return p;
}

/// The mode a configuration with these two sections runs.
std::string modeOfConfig(const std::string& tag, const std::string& estimation_json,
                         const std::string& model_json, std::string* derived_out = nullptr) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = writeConfig(tag, estimation_json, model_json);
    EXPECT_TRUE(cfg.loadFromJson(p, log));
    if (derived_out) *derived_out = windUpModeOf(cfg.deriveClosedLoopConsumerConfig());
    std::error_code ec;
    fs::remove(p, ec);
    return windUpModeOf(cfg);
}

}  // namespace

// ---------------------------------------------------------------------------
// The wind-up mode on the S line.
// ---------------------------------------------------------------------------
TEST(ProductContractGaps, WindUpModeRoundTripsThroughTheSLine) {
    for (const char* mode : { kWindUpModeOff, kWindUpModeFractional, kWindUpModeAccumulated,
                              "a_mode_a_later_build_adds" }) {
        SbasisContract c = baseContract();
        c.windup_mode = mode;
        fs::path path;
        const SbasisContract parsed = roundTrip(c, std::string("gaps_mode_") + mode + ".txt", &path);
        ASSERT_TRUE(parsed.present);
        EXPECT_EQ(parsed.windup_mode, mode) << "stored verbatim, so an unknown mode survives";
        EXPECT_NE(pick(path, "S,").find(std::string(",wumode=") + mode), std::string::npos)
            << pick(path, "S,");
        std::vector<std::string> mism;
        EXPECT_TRUE(NetworkProductReader::checkContract(parsed, c, &mism))
            << (mism.empty() ? "" : mism.front());
    }
}

TEST(ProductContractGaps, ContractWithoutAModeWritesTheSLineAsBefore) {
    SbasisContract none = baseContract();
    SbasisContract declared = none;
    declared.windup_mode = kWindUpModeAccumulated;
    fs::path p_none, p_declared;
    const SbasisContract parsed_none = roundTrip(none, "gaps_undeclared.txt", &p_none);
    roundTrip(declared, "gaps_declared.txt", &p_declared);

    const std::string a = pick(p_none, "S,"), b = pick(p_declared, "S,");
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a.find("wumode="), std::string::npos)
        << "an empty mode writes no token, so a contract assembled without one is unchanged";
    EXPECT_EQ(b, a + ",wumode=accumulated") << "the token is the only addition to the S line";
    EXPECT_TRUE(parsed_none.windup_mode.empty());

    // The header legend is grammar-complete by contract: it names the token.
    EXPECT_NE(pick(p_none, "# S,").find("wumode="), std::string::npos) << pick(p_none, "# S,");
    EXPECT_EQ(pick(p_declared, "# S,"), pick(p_none, "# S,"));
}

TEST(ProductContractGaps, MismatchedWindUpModeConsumerIsRefused) {
    const std::vector<std::string> modes = { kWindUpModeOff, kWindUpModeFractional,
                                             kWindUpModeAccumulated };
    for (const std::string& file_mode : modes) {
        SbasisContract c = baseContract();
        c.windup_mode = file_mode;
        const SbasisContract parsed = roundTrip(c, "gaps_pair_" + file_mode + ".txt");
        for (const std::string& consumer_mode : modes) {
            SbasisContract expect = c;
            expect.windup_mode = consumer_mode;
            std::vector<std::string> mism;
            const bool ok = NetworkProductReader::checkContract(parsed, expect, &mism);
            if (consumer_mode == file_mode) {
                EXPECT_TRUE(ok) << file_mode;
                EXPECT_TRUE(mism.empty());
            } else {
                EXPECT_FALSE(ok) << "file " << file_mode << " vs consumer " << consumer_mode;
                ASSERT_TRUE(mentions(mism, "windup_mode"));
                EXPECT_TRUE(mentions(mism, "file '" + file_mode + "'"));
                EXPECT_TRUE(mentions(mism, "expected '" + consumer_mode + "'"));
            }
        }
    }
}

TEST(ProductContractGaps, UndeclaredWireAndUndeclaringConsumerAreNotRefused) {
    // A wire written before the S line carried the mode: parsed from text, as an old
    // product file is.  Its mode cannot be verified; it must keep replaying.
    const fs::path old_wire = gnss_test::tempDir() / "gaps_old_wire.txt";
    {
        std::ofstream f(old_wire);
        f << "# gnss_network_product v2\n"
             "S,CIT100USA,1,nominal-wu93,0,0,0,0,0.05,1,COD0MGXFIN,G:G05\n"
             "E,2313,259200,3,42,1,0\n"
             "C,G01,1\n";
    }
    bool ok = false;
    SbasisContract old_contract;
    NetworkProductReader::read(old_wire.string(), &old_contract, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(old_contract.present);
    EXPECT_TRUE(old_contract.windup_mode.empty());

    SbasisContract consumer = baseContract();
    consumer.windup_mode = kWindUpModeAccumulated;
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(old_contract, consumer, &mism))
        << (mism.empty() ? "" : mism.front());

    // A consumer that declares no mode expects none, whatever the wire carries.
    SbasisContract declared_wire = baseContract();
    declared_wire.windup_mode = kWindUpModeFractional;
    const SbasisContract parsed = roundTrip(declared_wire, "gaps_declared_wire.txt");
    SbasisContract silent = baseContract();   // no mode
    mism.clear();
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, silent, &mism));
}

TEST(ProductContractGaps, ReBroadcastCarriesTheModeAndTheReceiptCheckRefusesARestartOnAnotherMode) {
    SbasisContract first = baseContract();
    first.windup_mode = kWindUpModeAccumulated;
    SbasisContract restarted = first;
    restarted.windup_mode = kWindUpModeFractional;   // the producer came back on another mode

    const fs::path p = gnss_test::tempDir() / "gaps_rebroadcast.txt";
    std::error_code ec;
    fs::remove(p, ec);
    GnssTime t2 = kT0; t2.tow += 30.0;
    {
        NetworkProductWriter w(p.string(), first);
        ASSERT_TRUE(w.good());
        w.write(epochAt(kT0));
        w.writeContract(restarted);          // mid-file re-broadcast in the header grammar
        w.write(epochAt(t2));
    }
    bool ok = false;
    SbasisContract last;
    const auto recs = NetworkProductReader::read(p.string(), &last, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_TRUE(recs[0].contract_refresh.present);
    ASSERT_TRUE(recs[1].contract_refresh.present);
    EXPECT_EQ(recs[0].contract_refresh.windup_mode, kWindUpModeAccumulated);
    EXPECT_EQ(recs[1].contract_refresh.windup_mode, kWindUpModeFractional);
    EXPECT_EQ(last.windup_mode, kWindUpModeFractional) << "the reader's state is the last S line";

    SbasisContract consumer = first;
    consumer.decl.clock_pivot_sat.clear();
    std::vector<std::string> mism;
    EXPECT_TRUE(NetworkProductReader::checkContract(recs[0].contract_refresh, consumer, &mism));
    mism.clear();
    EXPECT_FALSE(NetworkProductReader::checkContract(recs[1].contract_refresh, consumer, &mism));
    EXPECT_TRUE(mentions(mism, "windup_mode"));
}

// ---------------------------------------------------------------------------
// The replay-emitter parameters.
// ---------------------------------------------------------------------------
TEST(ProductContractGaps, CodeBiasJumpAndClockSetChangeFlagsAreCompared) {
    struct Flag {
        const char* name;
        bool SbasisContract::* member;
    };
    const Flag flags[] = {
        { "code_bias_jump_test",    &SbasisContract::code_bias_jump_test },
        { "clock_disc_on_set_change", &SbasisContract::clock_disc_on_set_change },
    };
    for (const Flag& flag : flags) {
        for (const bool wire_armed : { true, false }) {
            SbasisContract wire = baseContract();
            wire.*(flag.member) = wire_armed;
            const SbasisContract parsed =
                roundTrip(wire, std::string("gaps_flag_") + flag.name + (wire_armed ? "_on" : "_off")
                                    + ".txt");
            EXPECT_EQ(parsed.*(flag.member), wire_armed) << flag.name << ": the token round-trips";

            SbasisContract consumer = wire;
            std::vector<std::string> mism;
            EXPECT_TRUE(NetworkProductReader::checkContract(parsed, consumer, &mism)) << flag.name;

            consumer.*(flag.member) = !wire_armed;   // a replay built without (or with) the channel
            mism.clear();
            EXPECT_FALSE(NetworkProductReader::checkContract(parsed, consumer, &mism))
                << flag.name << " wire " << wire_armed;
            EXPECT_TRUE(mentions(mism, flag.name)) << flag.name;
        }
    }
}

TEST(ProductContractGaps, EverySevenReplayEmitterParameterIsCompared) {
    struct Param {
        const char* name;                                  // as the mismatch line names it
        std::function<void(SbasisContract&)> flip;
    };
    const std::vector<Param> params = {
        { "disc_threshold_m",             [](SbasisContract& c) { c.disc_threshold_m += 0.01; } },
        { "emit_clock_disc",              [](SbasisContract& c) { c.emit_clock_disc = !c.emit_clock_disc; } },
        { "staleness_horizon_s",          [](SbasisContract& c) { c.staleness_horizon_s += 60.0; } },
        { "code_bias_jump_test",          [](SbasisContract& c) { c.code_bias_jump_test = !c.code_bias_jump_test; } },
        { "clock_disc_on_set_change",     [](SbasisContract& c) { c.clock_disc_on_set_change = !c.clock_disc_on_set_change; } },
        { "disc_threshold_frac_cycles",   [](SbasisContract& c) { c.disc_threshold_frac_cycles += 0.05; } },
        { "disc_mark_scope_channel",      [](SbasisContract& c) { c.disc_mark_scope_channel = !c.disc_mark_scope_channel; } },
    };
    ASSERT_EQ(params.size(), 7u);
    const SbasisContract wire = baseContract();
    for (const Param& param : params) {
        SbasisContract consumer = wire;
        param.flip(consumer);
        std::vector<std::string> mism;
        EXPECT_FALSE(NetworkProductReader::checkContract(wire, consumer, &mism)) << param.name;
        EXPECT_TRUE(mentions(mism, param.name)) << param.name;
        EXPECT_EQ(mism.size(), 1u) << param.name << ": flipped alone, only it is reported";
    }
}

// ---------------------------------------------------------------------------
// The mode a configuration runs.
// ---------------------------------------------------------------------------
TEST(ProductContractGaps, WindUpModeOfFollowsTheConfiguration) {
    {
        GnssConfig defaults;
        EXPECT_EQ(windUpModeOf(defaults), kWindUpModeFractional)
            << "the loader default models wind-up and does not accumulate it";
    }
    std::string derived;
    EXPECT_EQ(modeOfConfig("fractional", "", "", &derived), kWindUpModeFractional);
    EXPECT_EQ(derived, kWindUpModeFractional);
    EXPECT_EQ(modeOfConfig("accumulated", "\"accumulate_phase_windup\": true", "", &derived),
              kWindUpModeAccumulated);
    EXPECT_EQ(derived, kWindUpModeAccumulated) << "the closed-loop consumer runs the producer's mode";
    EXPECT_EQ(modeOfConfig("off", "", "\"enable_phase_windup\": false", &derived), kWindUpModeOff);
    EXPECT_EQ(derived, kWindUpModeOff);
    EXPECT_EQ(modeOfConfig("off_accumulating", "\"accumulate_phase_windup\": true",
                           "\"enable_phase_windup\": false"),
              kWindUpModeOff) << "with the correction off, accumulation has nothing to accumulate";
}

TEST(ProductContractGaps, ConsumerPinnedToItsOwnConfigurationIsRefusedOnAProducerOnAnotherMode) {
    // The replay driver's expectation: the wire's own contract, with the mode replaced by
    // the one the consumer's configuration runs.  The producer ran accumulated.
    LogManager log;
    GnssConfig producer, consumer;
    const fs::path pp = writeConfig("producer", "\"accumulate_phase_windup\": true", "");
    const fs::path cp = writeConfig("consumer", "", "");   // fractional: the loader default
    ASSERT_TRUE(producer.loadFromJson(pp, log));
    ASSERT_TRUE(consumer.loadFromJson(cp, log));

    SbasisContract wire = baseContract();
    wire.windup_mode = windUpModeOf(producer);
    const SbasisContract parsed = roundTrip(wire, "gaps_consumer_pin.txt");

    SbasisContract expectation = parsed;
    expectation.decl.clock_pivot_sat.clear();
    expectation.windup_mode = windUpModeOf(consumer);
    std::vector<std::string> mism;
    EXPECT_FALSE(NetworkProductReader::checkContract(parsed, expectation, &mism));
    EXPECT_TRUE(mentions(mism, "file 'accumulated' vs expected 'fractional'"));

    expectation.windup_mode = windUpModeOf(producer);   // a consumer on the producer's mode
    mism.clear();
    EXPECT_TRUE(NetworkProductReader::checkContract(parsed, expectation, &mism));
    std::error_code ec;
    fs::remove(pp, ec);
    fs::remove(cp, ec);
}

}  // namespace GnssProcessor
