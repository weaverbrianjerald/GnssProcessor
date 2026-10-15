/**
 * @file test_network_run_end.cpp
 * @brief The run-end table (card NOM step TD1b): every row is filled and appears once, and
 *        the order constraints filter/NetworkRunEnd.hpp states hold.
 *
 * The table is the run end's order, so these are the constraints a reordered or added row
 * could break without any data run noticing: the trace close first, every manifest record
 * before the manifest's complete record, the product contract round trip before the
 * closed-loop summary, the six tail reports last, and the ambiguity-block bar before the
 * injection verdict (the exit code's precedence).  One placement is pinned besides: the
 * culprit withhold of card WCF directly after the within-epoch RAIM report.
 */

#include "filter/NetworkRunEnd.hpp"

#include "app/NetworkClosedLoopRunEnd.hpp"
#include "app/NetworkRunArtifactsRunEnd.hpp"
#include "app/SignalRoster.hpp"
#include "filter/NetworkAmbFloat.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp"
#include "filter/NetworkCarryDisposition.hpp"
#include "filter/NetworkCarrySites.hpp"
#include "filter/NetworkChannelAll.hpp"
#include "filter/NetworkContributions.hpp"
#include "filter/NetworkEmitStore.hpp"
#include "filter/NetworkInject.hpp"
#include "filter/NetworkLedgerEmit.hpp"
#include "filter/NetworkMultiGnss.hpp"
#include "filter/NetworkParamInventory.hpp"
#include "filter/NetworkRaimWithinEpoch.hpp"
#include "filter/NetworkResponses.hpp"
#include "filter/NetworkWithinEpochCulprits.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
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
    const NetworkRunEndReport records[] = {
        networkParamInventoryRunEnd, networkContributionsRunEnd, networkSignalRosterPlanRunEnd,
        networkCarryDispositionRunEnd, networkCarrySitesRunEnd, networkResponsesRunEnd};
    for (NetworkRunEndReport r : records) EXPECT_LT(rowOf(r), complete);
}

TEST(NetworkRunEndTable, TheContractRoundTripPrecedesTheClosedLoopSummary) {
    const std::size_t contract = rowOf(networkProductContractRunEnd);
    ASSERT_LT(contract, kNetworkRunEndReports.size());
    EXPECT_LT(contract, rowOf(networkClosedLoopRunEnd));
    EXPECT_LT(rowOf(networkRunManifestCompleteRunEnd), contract);
    EXPECT_LT(contract, rowOf(networkRunSummaryRunEnd));
}

TEST(NetworkRunEndTable, TheTailReportsAreTheLastRowsInOrder) {
    const NetworkRunEndReport tail[] = {
        networkInjectRunEnd, networkCanonicalPairRunEnd, networkSignalRosterRunEnd,
        networkSignalRosterSitesRunEnd,   // card NOM step N3b: beside the roster's report
        networkChannelAllRunEnd, networkMultiGnssRunEnd, networkLedgerEmitRunEnd};
    const std::size_t n = kNetworkRunEndReports.size();
    for (std::size_t k = 0; k < 7; ++k) EXPECT_EQ(rowOf(tail[k]), n - 7 + k);
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
