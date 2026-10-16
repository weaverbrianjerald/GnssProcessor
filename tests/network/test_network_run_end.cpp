/**
 * @file test_network_run_end.cpp
 * @brief The run-end table (card NOM step TD1b): every row is filled and appears once, and
 *        the order constraints filter/NetworkRunEnd.hpp states hold; one row per subsystem
 *        and the heading line of the walk (card NOM2 step TD2).
 *
 * The table is the run end's order, so these are the constraints a reordered or added row
 * could break without any data run noticing: the trace close first, every manifest record
 * before the manifest's complete record, the product contract round trip before the
 * closed-loop summary, the five tail reports last, and the ambiguity-block bar before the
 * injection verdict (the exit code's precedence).  One placement is pinned besides: the
 * culprit withhold of card WCF directly after the within-epoch RAIM report.
 */

#include "network/runend/NetworkRunEnd.hpp"

#include "network/runend/NetworkClosedLoopRunEnd.hpp"
#include "network/runend/NetworkRunArtifactsRunEnd.hpp"
#include "network/assembly/SignalRoster.hpp"
#include "network/ar/NetworkAmbFloat.hpp"
#include "network/assembly/NetworkCanonicalPairAdmission.hpp"
#include "network/state/NetworkCarryDisposition.hpp"
#include "network/state/NetworkCarrySites.hpp"
#include "network/assembly/NetworkChannelAll.hpp"
#include "network/assembly/NetworkContributions.hpp"
#include "network/emit/NetworkEmitStore.hpp"
#include "network/carry/NetworkInject.hpp"
#include "network/emit/NetworkLedgerEmit.hpp"
#include "network/assembly/NetworkMultiGnss.hpp"
#include "network/assembly/NetworkParamInventory.hpp"
#include "network/qc/NetworkRaimWithinEpoch.hpp"
#include "network/assembly/NetworkResponses.hpp"
#include "network/qc/NetworkWithinEpochCulprits.hpp"

#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace GnssProcessor {
namespace {

/// The row of @p report in the table, or the table's size when it is not a row.
std::size_t rowOf(NetworkRunEndReport report) {
    for (std::size_t i = 0; i < kNetworkRunEndReports.size(); ++i)
        if (kNetworkRunEndReports[i].report == report) return i;
    return kNetworkRunEndReports.size();
}

TEST(NetworkRunEndTable, EveryRowIsFilledAndAppearsOnce) {
    std::set<std::string> names;
    std::set<NetworkRunEndReport> reports;
    for (const NetworkRunEndRow& row : kNetworkRunEndReports) {
        ASSERT_NE(row.name, nullptr);
        ASSERT_NE(row.home, nullptr);
        ASSERT_NE(row.report, nullptr) << row.name;
        EXPECT_TRUE(names.insert(row.name).second) << "row named twice: " << row.name;
        EXPECT_TRUE(reports.insert(row.report).second) << "report listed twice: " << row.name;
        EXPECT_EQ(std::string(row.home).rfind(".cpp"), std::string(row.home).size() - 4)
            << row.home;
    }
}

TEST(NetworkRunEndTable, TheTraceCloseIsFirst) {
    EXPECT_EQ(rowOf(networkTraceCloseRunEnd), 0u);
}

TEST(NetworkRunEndTable, EveryManifestRecordPrecedesTheCompleteRecord) {
    const std::size_t complete = rowOf(networkRunManifestCompleteRunEnd);
    ASSERT_LT(complete, kNetworkRunEndReports.size());
    // The subsystem reports whose sections write the records (card NOM2 step TD2): the
    // parameter inventory, the contributions, the signal roster's plan, the carried border's
    // dispositions, the carried sites and the responses.
    const NetworkRunEndReport records[] = {
        networkParamInventoryRunEnd, networkContributionsRunEnd, networkSignalRosterRunEnd,
        networkCarryDispositionRunEnd, networkCarrySitesRunEnd, networkResponsesRunEnd};
    for (NetworkRunEndReport r : records) EXPECT_LT(rowOf(r), complete);
}

TEST(NetworkRunEndTable, TheContractRoundTripPrecedesTheClosedLoopSummary) {
    // The emit store's report holds the product contract round trip (card NOM2 step TD2).
    const std::size_t contract = rowOf(networkEmitStoreRunEnd);
    ASSERT_LT(contract, kNetworkRunEndReports.size());
    EXPECT_LT(contract, rowOf(networkClosedLoopRunEnd));
    EXPECT_LT(contract, rowOf(networkRunSummaryRunEnd));
}

TEST(NetworkRunEndTable, TheTailReportsAreTheLastRowsInOrder) {
    const NetworkRunEndReport tail[] = {
        networkInjectRunEnd, networkCanonicalPairRunEnd, networkChannelAllRunEnd,
        networkMultiGnssRunEnd, networkLedgerEmitRunEnd};
    const std::size_t n = kNetworkRunEndReports.size();
    for (std::size_t k = 0; k < 5; ++k) EXPECT_EQ(rowOf(tail[k]), n - 5 + k);
}

TEST(NetworkRunEndTable, TheRosterSiteCensusFollowsTheRoster) {
    // Card NOM step N3b: the site census is read beside the roster's report.
    const std::size_t roster = rowOf(networkSignalRosterRunEnd);
    ASSERT_LT(roster, kNetworkRunEndReports.size());
    EXPECT_EQ(rowOf(networkSignalRosterSitesRunEnd), roster + 1);
}

TEST(NetworkRunEndTable, EachSubsystemHasOneRow) {
    // Card NOM2 step TD2: one report per subsystem (its home file).  The run artifacts keep
    // three rows held apart by the order constraints; only the run summary prints.
    std::map<std::string, int> rows;
    for (const NetworkRunEndRow& row : kNetworkRunEndReports) ++rows[row.home];
    for (const auto& [home, n] : rows)
        EXPECT_EQ(n, home == "app/NetworkRunArtifactsRunEnd.cpp" ? 3 : 1) << home;
}

/// The captured text without spdlog's platform end-of-line carriage returns.
std::string text(const std::ostringstream& oss) {
    std::string t = oss.str();
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    return t;
}

std::shared_ptr<spdlog::logger> holdingTestLogger(std::ostringstream& oss) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_pattern("[%n] [%l] %v");
    auto lg = std::make_shared<spdlog::logger>("run_end_test", sink);
    lg->set_level(spdlog::level::info);
    return lg;
}

TEST(NetworkRunEndHeading, APrintingReportGetsOneHeadingAheadOfItsLines) {
    std::ostringstream oss;
    auto lg = holdingTestLogger(oss);
    const int rc = networkRunEndWithHeading(*lg, "the test subsystem", [&] {
        lg->info("first line");
        lg->warn("second line {}", 2);
        lg->debug("below the logger's level");
        return 7;
    });
    EXPECT_EQ(rc, 7);
    EXPECT_EQ(text(oss), "[run_end_test] [info] Network[run-end] the test subsystem\n"
                         "[run_end_test] [info] first line\n"
                         "[run_end_test] [warning] second line 2\n");
    ASSERT_EQ(lg->sinks().size(), 1u);   // the logger's own sink is back
}

TEST(NetworkRunEndHeading, ASilentReportGetsNoHeading) {
    std::ostringstream oss;
    auto lg = holdingTestLogger(oss);
    EXPECT_EQ(networkRunEndWithHeading(*lg, "silent", [] { return 0; }), 0);
    EXPECT_TRUE(text(oss).empty());
    lg->info("after");
    EXPECT_EQ(text(oss), "[run_end_test] [info] after\n");
}

TEST(NetworkRunEndHeading, HeldLinesAreWrittenWhenTheReportThrows) {
    std::ostringstream oss;
    auto lg = holdingTestLogger(oss);
    EXPECT_THROW(networkRunEndWithHeading(*lg, "thrower", [&]() -> int {
        lg->info("before the throw");
        throw std::runtime_error("x");
    }), std::runtime_error);
    EXPECT_EQ(text(oss), "[run_end_test] [info] Network[run-end] thrower\n"
                         "[run_end_test] [info] before the throw\n");
}

TEST(NetworkRunEndTable, TheCulpritWithholdFollowsTheWithinEpochRaimReport) {
    // Card WCF: the hand-off's line is read beside the T2 responses it accounts for.
    const std::size_t we = rowOf(networkWithinEpochRaimRunEnd);
    ASSERT_LT(we, kNetworkRunEndReports.size());
    EXPECT_EQ(rowOf(networkWithinEpochCulpritWithholdRunEnd), we + 1);
}

TEST(NetworkRunEndTable, TheInjectionVerdictFollowsTheAmbiguityBlockBar) {
    // networkTeardown keeps the last non-zero verdict, so this order makes the injection
    // verdict win over the joint-block PD-fallback bar.
    const NetworkRunEndReport bar = networkAmbVehicleRunEnd;
    const NetworkRunEndReport inject = networkInjectRunEnd;
    EXPECT_LT(rowOf(bar), rowOf(inject));
}

} // namespace
} // namespace GnssProcessor
