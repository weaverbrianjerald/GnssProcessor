/**
 * @file test_signal_roster_always_on.cpp
 * @brief What changed when the signal roster went on for every network run (see
 *        docs/refound/NOM2_network_object_model_rows_2026-10-09.md
 *        section 4): the loader's reader interlocks and the bounded run-scope census.
 *
 * Since step N3a the roster, the arc ledger and their census are built on every
 * network run, and @c network.signal_roster is the switch of the ledger's trace only.
 * Two consequences are pinned here:
 *
 *  - the loader.  The readers of the roster no longer need that key, so
 *    @c network.roster_tracker and @c network.ledger_screens stay armed without it
 *    (as @c network.ledger_emit does, pinned in test_ledger_emit.cpp), while the one
 *    interlock that does not concern the roster's existence remains:
 *    @c network.ledger_reentry is still refused without @c network.roster_tracker;
 *  - the memory.  A census that runs on every production run must not grow with the
 *    run's length.  Class (f), a phase-bias family's carried support, is held as a
 *    histogram (@c RosterFamilySupport) whose two run-end statistics, the median and
 *    the count below a bar, equal those of the full sample it replaced, and whose
 *    size is bounded by the largest station count.
 */

#include <gtest/gtest.h>

#include "network/assembly/SignalRoster.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

/// A minimal network config with @p network_extra inserted into the network section.
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
        "      { \"name\": \"AAAA\",\n"
        "        \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\",\n"
        "        \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    const fs::path p = fs::temp_directory_path() / ("roster_always_on_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// A config loaded from @ref writeTempConfig; the file is removed on destruction.
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

/// The sorted-sample median the histogram replaced (SignalRoster.cpp before N3a).
double sortedMedian(std::vector<int> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2) ? static_cast<double>(v[n / 2])
                   : 0.5 * (static_cast<double>(v[n / 2 - 1]) + static_cast<double>(v[n / 2]));
}

}  // namespace

// ---------------------------------------------------------------------------
// The loader: the readers need no trace switch; the one remaining interlock holds
// ---------------------------------------------------------------------------

/// The roster tracker and the ledger screens read objects built on every network run,
/// so the loader keeps them armed with network.signal_roster absent.
TEST(SignalRosterAlwaysOnTest, ReadersStayArmedWithoutTheTraceSwitch) {
    Loaded L("readers", "    \"roster_tracker\": true,\n    \"ledger_screens\": true,\n");
    EXPECT_FALSE(L.cfg.networkSignalRoster());
    EXPECT_TRUE(L.cfg.networkRosterTracker())
        << "network.roster_tracker must stay armed without network.signal_roster";
    EXPECT_TRUE(L.cfg.networkLedgerScreens())
        << "network.ledger_screens must stay armed without network.signal_roster";
}

/// network.ledger_reentry still needs the roster read: without it the tracker's rows
/// would name a band by a different rule than the population does.  The positive
/// control arms the same keys plus network.roster_tracker, so the refusal is
/// attributable to the missing roster read alone (the gap re-entry response it
/// drives is armed in both arms).
TEST(SignalRosterAlwaysOnTest, LedgerReentryIsStillRefusedWithoutTheRosterRead) {
    const std::string response = "    \"cycle_slip\": { \"gap_reentry_relevel\": true },\n";
    Loaded refused("reentry_no_tracker", response + "    \"ledger_reentry\": true,\n");
    EXPECT_FALSE(refused.cfg.networkRosterTracker());
    EXPECT_FALSE(refused.cfg.networkLedgerReentry())
        << "network.ledger_reentry without network.roster_tracker must be disarmed";

    Loaded armed("reentry_with_tracker",
                 response + "    \"roster_tracker\": true,\n    \"ledger_reentry\": true,\n");
    EXPECT_FALSE(armed.cfg.networkSignalRoster());
    EXPECT_TRUE(armed.cfg.networkRosterTracker());
    EXPECT_TRUE(armed.cfg.networkLedgerReentry())
        << "with the roster read armed, network.ledger_reentry needs no trace switch";
}

// ---------------------------------------------------------------------------
// The memory: a family's support as a histogram
// ---------------------------------------------------------------------------

/// The histogram's median and below-bar count equal the full sample's, for an empty,
/// an odd and an even sample and for long random ones.
TEST(SignalRosterAlwaysOnTest, FamilySupportHistogramReadsTheSampleStatistics) {
    RosterFamilySupport empty;
    EXPECT_EQ(empty.epochs, 0);
    EXPECT_DOUBLE_EQ(empty.median(), 0.0);
    EXPECT_EQ(empty.below(3), 0);

    RosterFamilySupport odd;
    for (int n : { 5, 1, 4 }) odd.add(n);
    EXPECT_DOUBLE_EQ(odd.median(), 4.0);
    EXPECT_EQ(odd.below(3), 1);

    RosterFamilySupport even;
    for (int n : { 2, 7, 3, 2 }) even.add(n);
    EXPECT_DOUBLE_EQ(even.median(), 2.5) << "the mean of the two middle samples";
    EXPECT_EQ(even.below(3), 2);
    EXPECT_EQ(even.below(0), 0);
    EXPECT_EQ(even.below(100), 4) << "a bar above every sample counts them all";

    std::mt19937 rng(20261009u);
    for (int trial = 0; trial < 50; ++trial) {
        std::uniform_int_distribution<int> len(1, 3000), sup(1, 60);
        const int n = len(rng);
        std::vector<int> v;
        RosterFamilySupport h;
        for (int k = 0; k < n; ++k) {
            const int x = sup(rng);
            v.push_back(x);
            h.add(x);
        }
        ASSERT_EQ(h.epochs, static_cast<long long>(v.size()));
        EXPECT_DOUBLE_EQ(h.median(), sortedMedian(v)) << "trial " << trial;
        EXPECT_EQ(h.below(3), std::count_if(v.begin(), v.end(), [](int x) { return x < 3; }))
            << "trial " << trial;
    }
}

/// The histogram's size is set by the largest support seen, not by the number of
/// epochs: a day of 30 s epochs at 60 stations keeps 61 cells.
TEST(SignalRosterAlwaysOnTest, FamilySupportMemoryDoesNotGrowWithTheRun) {
    RosterFamilySupport h;
    for (int ep = 0; ep < 2880; ++ep) h.add(1 + ep % 60);
    EXPECT_EQ(h.epochs, 2880);
    EXPECT_EQ(h.epochs_by_support.size(), 61u);
    for (int ep = 0; ep < 2880; ++ep) h.add(1 + ep % 60);
    EXPECT_EQ(h.epochs, 5760);
    EXPECT_EQ(h.epochs_by_support.size(), 61u) << "a second day adds no cell";
    h.add(-1);
    EXPECT_EQ(h.epochs, 5760) << "a negative support is not a sample";
}

}  // namespace GnssProcessor
