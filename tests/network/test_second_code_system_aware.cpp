/**
 * @file test_second_code_system_aware.cpp
 * @brief Contract tests for the constellation-aware second-code row test
 *        (`network.second_code_system_aware`).
 *
 * The station split scales, tags and counts the code rows that route through a
 * receiver inter-signal state `<system>_ISC_<signal>`.  The states are keyed on the
 * constellation and the signal, but the historic test matched the observation code
 * alone, so at a station whose GPS companion is C1C a Galileo C1C row (canonical, with
 * no inter-signal state of its own) took the second-code scale too.
 *
 * Covered here:
 *   (a) @c secondCodeIscRows on a station whose GPS companion is C1C: the code-only
 *       form returns the GPS and the Galileo C1C rows, the aware form the GPS row
 *       only, and the station split's weights follow;
 *   (b) a station whose GPS companion is C1W: both forms agree;
 *   (c) a Galileo state of its own is honoured by the aware form, and the walk's
 *       bounds are unchanged;
 *   (d) the loader: default off, read when set, kept (and inert) without the channel.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/SecondCodeScope.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;

using GnssProcessor::GnssConfig;
using GnssProcessor::LogManager;
using GnssProcessor::ParamForm;
using GnssProcessor::RxCbKey;
using GnssProcessor::SignalId;
using GnssProcessor::System;
using GnssProcessor::secondCodeIscRows;

namespace {

/// The receiver inter-signal state of @p sys and @p code, as the receiver bias resolver
/// keys it (constellation, form Isc, signal; band and slot left at their defaults).
RxCbKey iscKey(System sys, const char* code) {
    RxCbKey k;
    k.sys    = sys;
    k.form   = ParamForm::Isc;
    k.signal = *SignalId::fromObsCode(sys, code);
    return k;
}

/// The per-constellation differential bias key (form Dcb): never an inter-signal state.
RxCbKey dcbKey(System sys) {
    RxCbKey k;
    k.sys  = sys;
    k.form = ParamForm::Dcb;
    return k;
}

/// The weights the station split forms for @p n rows at unit weight when the second-code
/// scale @p scale applies to @p isc_rows: w /= scale^2 on each returned row.
std::vector<double> splitWeights(std::size_t n, const std::vector<std::size_t>& isc_rows,
                                 double scale) {
    std::vector<double> w(n, 1.0);
    for (const std::size_t r : isc_rows) w[r] /= scale * scale;
    return w;
}

// One station's rows: a GPS satellite on its canonical C1W and companion C1C, a Galileo
// satellite on its canonical C1C, and the phase rows the walk must skip.
const std::vector<std::string> kSats{ "G05", "G05", "E11", "E11", "G05" };
const std::vector<std::string> kSigs{ "C1W", "C1C", "C1C", "L1C", "L1C" };

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("scc2_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra splices into the network
/// block (the test_network_second_code_scope.cpp shape).
std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
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
}

/// The network block that arms the second code channel (its two interlocks satisfied).
const char* kArmable =
    "    \"second_code_channel\": true,\n"
    "    \"carry_rx_code_bias\": true,\n"
    "    \"code_datum_time_linked\": true,\n";

}  // namespace

// --------------------------------------------------------------------------
// (a) a station whose GPS companion is C1C
// --------------------------------------------------------------------------

TEST(SecondCodeSystemAware, OffMatchesTheGalileoC1CRowBesideGpsIscC1C) {
    const std::map<RxCbKey, int> rx_cb{ { dcbKey(System::GPS), 0 },
                                        { iscKey(System::GPS, "C1C"), 1 } };
    long long other = 0;
    const auto rows = secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), false, &other);
    EXPECT_EQ(rows, (std::vector<std::size_t>{ 1, 2 }));
    EXPECT_EQ(other, 1);
}

TEST(SecondCodeSystemAware, OnMatchesTheGpsCompanionRowOnly) {
    const std::map<RxCbKey, int> rx_cb{ { dcbKey(System::GPS), 0 },
                                        { iscKey(System::GPS, "C1C"), 1 } };
    long long other = 0;
    const auto rows = secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), true, &other);
    EXPECT_EQ(rows, (std::vector<std::size_t>{ 1 }));
    EXPECT_EQ(other, 1);   // counted in both forms
}

TEST(SecondCodeSystemAware, OffDividesBothC1CRowsOnDividesTheGpsRowOnly) {
    const std::map<RxCbKey, int> rx_cb{ { iscKey(System::GPS, "C1C"), 0 } };
    const auto off = splitWeights(kSats.size(),
        secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), false, nullptr), 10.0);
    const auto on = splitWeights(kSats.size(),
        secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), true, nullptr), 10.0);
    EXPECT_EQ(off, (std::vector<double>{ 1.0, 0.01, 0.01, 1.0, 1.0 }));
    EXPECT_EQ(on,  (std::vector<double>{ 1.0, 0.01, 1.0,  1.0, 1.0 }));
}

// --------------------------------------------------------------------------
// (b) a station whose GPS companion is C1W
// --------------------------------------------------------------------------

TEST(SecondCodeSystemAware, AC1WCompanionStationIsUnchangedByTheAwareForm) {
    // GPS canonical C1C, companion C1W; the Galileo C1C row shares no code with the state.
    const std::vector<std::string> sats{ "G07", "G07", "E11", "E11" };
    const std::vector<std::string> sigs{ "C1C", "C1W", "C1C", "C5Q" };
    const std::map<RxCbKey, int> rx_cb{ { dcbKey(System::GPS), 0 },
                                        { iscKey(System::GPS, "C1W"), 1 } };
    long long other_off = 0, other_on = 0;
    const auto off = secondCodeIscRows(rx_cb, sats, sigs, sats.size(), false, &other_off);
    const auto on  = secondCodeIscRows(rx_cb, sats, sigs, sats.size(), true, &other_on);
    EXPECT_EQ(off, (std::vector<std::size_t>{ 1 }));
    EXPECT_EQ(on, off);
    EXPECT_EQ(other_off, 0);
    EXPECT_EQ(other_on, 0);
}

// --------------------------------------------------------------------------
// (c) a Galileo state of its own, and the walk's bounds
// --------------------------------------------------------------------------

TEST(SecondCodeSystemAware, AGalileoStateOfItsOwnIsMatchedByTheAwareForm) {
    // Galileo canonical C1X with a Galileo C1C companion: the Galileo C1C row routes
    // through GALILEO_ISC_C1C and keeps the scale in both forms.
    const std::map<RxCbKey, int> rx_cb{ { iscKey(System::GPS, "C1C"), 0 },
                                        { iscKey(System::Galileo, "C1C"), 1 } };
    long long other = 0;
    const auto rows = secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), true, &other);
    EXPECT_EQ(rows, (std::vector<std::size_t>{ 1, 2 }));
    EXPECT_EQ(other, 0);
}

TEST(SecondCodeSystemAware, NoInterSignalStateMatchesNothing) {
    const std::map<RxCbKey, int> rx_cb{ { dcbKey(System::GPS), 0 } };
    EXPECT_TRUE(secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), false, nullptr).empty());
    EXPECT_TRUE(secondCodeIscRows(rx_cb, kSats, kSigs, kSats.size(), true, nullptr).empty());
}

TEST(SecondCodeSystemAware, TheWalkBoundsAreTheSameInBothForms) {
    const std::map<RxCbKey, int> rx_cb{ { iscKey(System::GPS, "C1C"), 0 } };
    // The design-matrix row count bounds the walk.
    EXPECT_EQ(secondCodeIscRows(rx_cb, kSats, kSigs, 2, false, nullptr),
              (std::vector<std::size_t>{ 1 }));
    // A matched row without a satellite id is skipped; a matched row beyond the
    // satellite ids stops the walk.
    const std::vector<std::string> sats{ "", "G05" };
    const std::vector<std::string> sigs{ "C1C", "C1C", "C1C" };
    for (const bool aware : { false, true }) {
        EXPECT_EQ(secondCodeIscRows(rx_cb, sats, sigs, sigs.size(), aware, nullptr),
                  (std::vector<std::size_t>{ 1 }));
    }
}

// --------------------------------------------------------------------------
// (d) the loader
// --------------------------------------------------------------------------

TEST(SecondCodeSystemAware, DefaultsToOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(kArmable));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondCodeChannel());
    EXPECT_FALSE(cfg.networkSecondCodeSystemAware());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondCodeSystemAware, IsReadWhenSet) {
    LogManager log;
    const fs::path p = writeTempConfig("armed", networkConfig(
        std::string(kArmable) + "    \"second_code_system_aware\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkSecondCodeChannel());
    EXPECT_TRUE(cfg.networkSecondCodeSystemAware());
    std::error_code ec; fs::remove(p, ec);
}

TEST(SecondCodeSystemAware, IsKeptButInertWithoutTheChannel) {
    LogManager log;
    const fs::path p = writeTempConfig("inert", networkConfig(
        "    \"second_code_system_aware\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkSecondCodeRowsActive());
    EXPECT_TRUE(cfg.networkSecondCodeSystemAware());
    std::error_code ec; fs::remove(p, ec);
}
