/**
 * @file test_network_time_windows.cpp
 * @brief Contract tests for the table of time windows of the network producer
 *        (network/driver/TimeWindows.hpp).
 *
 * The table states every live window that is counted in epochs, or that bounds a gap, in seconds
 * at a fixed reference interval of 30 s.  Introducing it changed no value: these tests pin
 *
 *  1. the table's shape: 18 rows, each identity at its own index, unique identities and names;
 *  2. each row's seconds form resolved at 30 s against today's integer or value, written here
 *     independently of the table;
 *  3. each constant that now reads its row (kRingFixGapS and the others) against its old literal;
 *  4. each configuration row's recorded default against the loader's default;
 *  5. each configuration row's recorded reference value against the two reference
 *     configurations (config/golden/golden_{field,sim}_refound.json, read as plain JSON);
 *  6. the run-start table's shape.
 */

#include <gtest/gtest.h>

#include "network/driver/TimeWindows.hpp"
#include "network/ar/RingFix.hpp"              // kRingFixGapS, kRingFixWindowS, kRingFixPersistEpochs, kRingFixMonitorEpochs
#include "network/driver/RunTypes.hpp"         // kReadmitMinGapEp, kReadmitVerifyWindowS, kRxCbMaxGapS
#include "network/emit/ClockEmitRebase.hpp"    // NetworkClockEmitRebase::Config::max_gap_s
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using GnssProcessor::GnssConfig;
using GnssProcessor::kNetworkReferenceIntervalS;
using GnssProcessor::kNetworkTimeWindowCount;
using GnssProcessor::kNetworkTimeWindows;
using GnssProcessor::LogManager;
using GnssProcessor::NetworkTimeWindow;
using GnssProcessor::NetworkTimeWindowRow;
using GnssProcessor::NetworkWindowClock;
using GnssProcessor::NetworkWindowUnit;
using GnssProcessor::networkTimeWindowDefault;
using GnssProcessor::networkTimeWindowEpochs;
using GnssProcessor::networkTimeWindowInForce;
using GnssProcessor::networkTimeWindowInUnit;
using GnssProcessor::networkTimeWindowRow;
using GnssProcessor::networkTimeWindowSeconds;
using GnssProcessor::networkTimeWindowsTable;

namespace fs = std::filesystem;
using W = NetworkTimeWindow;

namespace {

/// Today's value of one window, written independently of the table: the default (the constant or
/// the loader default) and the value of the two reference configurations, each in the window's
/// own unit, and the reference value in seconds at 30 s with its counting clock.
struct Today {
    W                  id;
    NetworkWindowUnit  unit;
    double             dflt;          ///< Constant or loader default, in the window's unit.
    double             reference;     ///< Reference configurations' value, in the window's unit.
    double             seconds_30;    ///< The reference value in seconds (per second for a count per epoch) at 30 s.
    NetworkWindowClock clock;
};

constexpr NetworkWindowUnit  kS  = NetworkWindowUnit::Seconds;
constexpr NetworkWindowUnit  kEp = NetworkWindowUnit::Epochs;
constexpr NetworkWindowUnit  kPe = NetworkWindowUnit::PerEpoch;
constexpr NetworkWindowClock kPr = NetworkWindowClock::Presence;
constexpr NetworkWindowClock kDe = NetworkWindowClock::Decision;

const std::vector<Today>& today() {
    static const std::vector<Today> t = {
        {W::RingFixGap,          kS,   45.0,   45.0,   45.0, kDe},
        {W::ClockAnchorMaxGap,   kS,  120.0,  120.0,  120.0, kDe},
        {W::RingFixMaxDecisions, kPe,   3.0,   12.0,    0.4, kDe},
        {W::RingFixPersist,      kEp,  10.0,   10.0,  300.0, kDe},
        {W::RingFixMonitor,      kEp,   4.0,    4.0,  120.0, kPr},
        {W::RingFixSettledWait,  kEp,   0.0,   10.0,  300.0, kDe},
        {W::RingFixNnGuardMin,   kEp,  60.0,   60.0, 1800.0, kDe},
        {W::ArSuspect,           kEp,   0.0,   10.0,  300.0, kDe},
        {W::PhaseLevelingWarmup, kEp,   6.0,    6.0,  180.0, kPr},
        {W::ClockAnchorRecouple, kEp,   4.0,    4.0,  120.0, kDe},
        {W::ReadmitMinGap,       kEp,   3.0,    3.0,   90.0, kPr},
        {W::PhaseLevelingMaxGap, kS,  120.0,  900.0,  900.0, kPr},
        {W::RxCbMaxGap,          kS,  900.0,  900.0,  900.0, kPr},
        {W::PhaseColourMaxGap,   kS,  900.0,  900.0,  900.0, kPr},
        {W::ReadmitVerify,       kS,  300.0,  300.0,  300.0, kPr},
        {W::RingFixWindow,       kS,  600.0,  600.0,  600.0, kDe},
        {W::ClockEmitRebaseGap,  kS,  600.0,  600.0,  600.0, kPr},
        {W::ReadmitSeedHorizon,  kS,    0.0, 3900.0, 3900.0, kPr},
    };
    return t;
}

bool isKeyed(const NetworkTimeWindowRow& r) { return r.config_key[0] != '\0'; }

/// A minimal ppp-rtk-network configuration that loads (no window key set).
std::string minimalNetworkConfig() {
    return "{\n"
           "  \"processing\": {\n"
           "    \"mode\": \"ppp-rtk-network\",\n"
           "    \"active_systems\": [ \"GPS\" ],\n"
           "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
           "  },\n"
           "  \"network\": {\n"
           "    \"clock_datum_station\": \"AAAA\",\n"
           "    \"stations\": [\n"
           "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
           "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
           "    ]\n"
           "  }\n"
           "}\n";
}

bool loadMinimal(GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / "time_windows_minimal.json";
    {
        std::ofstream f(p);
        f << minimalNetworkConfig();
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The value at a dotted key of @p root, or null when any step is absent.
const nlohmann::json* lookup(const nlohmann::json& root, const std::string& dotted) {
    const nlohmann::json* node = &root;
    std::stringstream ss(dotted);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (!node->is_object()) return nullptr;
        const auto it = node->find(part);
        if (it == node->end()) return nullptr;
        node = &*it;
    }
    return node;
}

}  // namespace

// ---- 1. Shape ------------------------------------------------------------------------------------

TEST(NetworkTimeWindows, TableHasEighteenRowsWithUniqueIdentities) {
    EXPECT_EQ(kNetworkTimeWindowCount, 18u);
    ASSERT_EQ(kNetworkTimeWindows.size(), 18u);
    std::set<int> ids;
    std::set<std::string> names;
    for (std::size_t i = 0; i < kNetworkTimeWindows.size(); ++i) {
        const NetworkTimeWindowRow& r = kNetworkTimeWindows[i];
        EXPECT_EQ(static_cast<std::size_t>(r.id), i) << r.name;
        EXPECT_TRUE(ids.insert(static_cast<int>(r.id)).second) << r.name;
        EXPECT_TRUE(names.insert(r.name).second) << r.name;
        EXPECT_NE(std::string(r.replaces), "") << r.name;
        EXPECT_EQ(&networkTimeWindowRow(r.id), &r) << r.name;
    }
    EXPECT_EQ(ids.size(), 18u);
    ASSERT_EQ(today().size(), 18u);
}

// ---- 2. Each row resolves to today's value at 30 s ----------------------------------------------

TEST(NetworkTimeWindows, EachRowResolvesToTodaysValueAtThirtySeconds) {
    EXPECT_EQ(kNetworkReferenceIntervalS, 30.0);
    for (const Today& t : today()) {
        const NetworkTimeWindowRow& r = networkTimeWindowRow(t.id);
        EXPECT_EQ(r.unit, t.unit) << r.name;
        EXPECT_EQ(r.clock, t.clock) << r.name;
        EXPECT_EQ(networkTimeWindowDefault(t.id), t.dflt) << r.name;
        EXPECT_EQ(networkTimeWindowInUnit(r.unit, r.reference_s), t.reference) << r.name;
        EXPECT_DOUBLE_EQ(r.reference_s, t.seconds_30) << r.name;
        if (t.unit != NetworkWindowUnit::Seconds) {   // a count resolves to a whole number, exactly
            EXPECT_EQ(static_cast<double>(networkTimeWindowEpochs(t.id)), t.dflt) << r.name;
        } else {
            EXPECT_EQ(networkTimeWindowSeconds(t.id), t.dflt) << r.name;
        }
    }
}

// ---- 3. The constants read their rows and keep their values -------------------------------------

TEST(NetworkTimeWindows, ConstantsKeepTheirValuesAndReadTheirRows) {
    static_assert(std::is_same_v<std::remove_cv_t<decltype(GnssProcessor::kRingFixPersistEpochs)>, int>);
    static_assert(std::is_same_v<std::remove_cv_t<decltype(GnssProcessor::kRingFixMonitorEpochs)>, int>);
    static_assert(std::is_same_v<std::remove_cv_t<decltype(GnssProcessor::kReadmitMinGapEp)>, long long>);
    static_assert(GnssProcessor::kRingFixGapS == 45.0);
    static_assert(GnssProcessor::kRingFixWindowS == 600.0);
    static_assert(GnssProcessor::kRingFixPersistEpochs == 10);
    static_assert(GnssProcessor::kRingFixMonitorEpochs == 4);
    static_assert(GnssProcessor::kReadmitMinGapEp == 3);
    static_assert(GnssProcessor::kReadmitVerifyWindowS == 300.0);
    static_assert(GnssProcessor::kRxCbMaxGapS == 900.0);

    EXPECT_EQ(GnssProcessor::kRingFixGapS, networkTimeWindowSeconds(W::RingFixGap));
    EXPECT_EQ(GnssProcessor::kRingFixWindowS, networkTimeWindowSeconds(W::RingFixWindow));
    EXPECT_EQ(GnssProcessor::kRingFixPersistEpochs, networkTimeWindowEpochs(W::RingFixPersist));
    EXPECT_EQ(GnssProcessor::kRingFixMonitorEpochs, networkTimeWindowEpochs(W::RingFixMonitor));
    EXPECT_EQ(GnssProcessor::kReadmitMinGapEp, networkTimeWindowEpochs(W::ReadmitMinGap));
    EXPECT_EQ(GnssProcessor::kReadmitVerifyWindowS, networkTimeWindowSeconds(W::ReadmitVerify));
    EXPECT_EQ(GnssProcessor::kRxCbMaxGapS, networkTimeWindowSeconds(W::RxCbMaxGap));

    const GnssProcessor::NetworkClockEmitRebase::Config rebase{};
    EXPECT_EQ(rebase.max_gap_s, 600.0);
    EXPECT_EQ(rebase.max_gap_s, networkTimeWindowSeconds(W::ClockEmitRebaseGap));

    // Exactly the eight constant rows carry no key.
    int constants = 0;
    for (const NetworkTimeWindowRow& r : kNetworkTimeWindows) {
        if (isKeyed(r)) continue;
        ++constants;
        EXPECT_EQ(r.default_s, r.reference_s) << r.name;   // a constant has one value
    }
    EXPECT_EQ(constants, 8);
}

// ---- 4. Configuration rows: the recorded default is the loader's --------------------------------

TEST(NetworkTimeWindows, ConfigurationRowsRecordTheLoaderDefault) {
    GnssConfig cfg;
    ASSERT_TRUE(loadMinimal(cfg));
    for (const NetworkTimeWindowRow& r : kNetworkTimeWindows)
        EXPECT_EQ(networkTimeWindowInForce(r.id, cfg), networkTimeWindowDefault(r.id))
            << r.name << " (" << (isKeyed(r) ? r.config_key : "constant") << ")";
}

// ---- 5. Configuration rows: the recorded reference value is the reference files' ---------------

TEST(NetworkTimeWindows, ConfigurationRowsRecordTheReferenceFilesValue) {
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        const fs::path p = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / name;
        std::ifstream f(p);
        ASSERT_TRUE(f.good()) << p.string();
        const nlohmann::json j = nlohmann::json::parse(f);
        for (const NetworkTimeWindowRow& r : kNetworkTimeWindows) {
            if (!isKeyed(r)) continue;
            const nlohmann::json* v = lookup(j, r.config_key);
            const double armed = v != nullptr ? v->get<double>() : networkTimeWindowDefault(r.id);
            EXPECT_EQ(armed, networkTimeWindowInUnit(r.unit, r.reference_s))
                << name << ": " << r.config_key << (v != nullptr ? "" : " (absent: the loader default)");
        }
    }
}

// ---- 6. The run-start table ---------------------------------------------------------------------

TEST(NetworkTimeWindows, RunStartTableHasOneLinePerWindow) {
    GnssConfig cfg;
    ASSERT_TRUE(loadMinimal(cfg));
    const std::vector<std::string> lines = networkTimeWindowsTable(cfg);
    ASSERT_EQ(lines.size(), 2u + kNetworkTimeWindowCount);   // a note and a column header first
    for (std::size_t i = 0; i < kNetworkTimeWindowCount; ++i) {
        const NetworkTimeWindowRow& r = kNetworkTimeWindows[i];
        const std::string& line = lines[2 + i];
        EXPECT_EQ(line.rfind(r.name, 0), 0u) << line;
        EXPECT_NE(line.find(isKeyed(r) ? r.config_key : "constant"), std::string::npos) << line;
        EXPECT_EQ(line.find("(default"), std::string::npos) << line;   // every key at its loader default
    }
}
