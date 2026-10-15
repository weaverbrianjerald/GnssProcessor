/**
 * @file test_network_epoch_observers.cpp
 * @brief The observer table (card NOM3 step EP3): every row is filled and appears once, the
 *        table is in spine order, and the rows, their slots and their order are pinned.
 *
 * The table is the epoch's instrument order, and a reordered or re-slotted row changes the
 * interleaving of trace rows, events and log lines without any unit test noticing; the order
 * constraints of filter/NetworkEpochObservers.hpp are checked by name below, and the whole
 * table is pinned so a change to it is a deliberate edit here.
 */

#include "filter/NetworkEpochObservers.hpp"

#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/NetworkBlockBaarda.hpp"
#include "filter/NetworkChannelAll.hpp"
#include "filter/NetworkCongruence.hpp"
#include "filter/NetworkContributions.hpp"
#include "filter/NetworkDatumGraph.hpp"
#include "filter/NetworkFrameDiag.hpp"
#include "filter/NetworkLedgerEmit.hpp"
#include "filter/NetworkLevelAuthority.hpp"
#include "filter/NetworkMdb.hpp"
#include "filter/NetworkMultiGnss.hpp"
#include "filter/NetworkParamInventory.hpp"
#include "filter/NetworkPhaseVisibleDiag.hpp"
#include "filter/NetworkPrefitCensus.hpp"
#include "filter/NetworkRedundancyCensus.hpp"
#include "filter/NetworkRhsAttrib.hpp"
#include "filter/NetworkSecondPhaseScope.hpp"
#include "filter/NetworkStationBlock.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <string>

namespace GnssProcessor {
namespace {

using Slot = NetworkObserverSlot;

struct Pinned {
    const char*          name;
    Slot                 slot;
    NetworkEpochObserver observe;
};

/// The table as card NOM3 step EP3 pre-registered it (scripts/nom3_designs/EP3_design.md).
const Pinned kPinned[] = {
    {"networkParamInventoryNoteSatellites", Slot::Admitted,      &networkParamInventoryNoteSatellites},
    {"networkLevelAuthorityCensus",         Slot::BeforeColumns, &networkLevelAuthorityCensus},
    {"networkPhaseVisibleMonitor",          Slot::AfterClockQc,  &networkPhaseVisibleMonitor},
    {"networkLedgerEmitFold",               Slot::RosterTail,    &networkLedgerEmitFold},
    {"networkSecondPhaseEpochTail",         Slot::RosterTail,    &networkSecondPhaseEpochTail},
    {"networkChannelAllEpoch",              Slot::RosterTail,    &networkChannelAllEpoch},
    {"networkMultiGnssEpoch",               Slot::RosterTail,    &networkMultiGnssEpoch},
    {"networkParamInventoryEpochTail",      Slot::RosterTail,    &networkParamInventoryEpochTail},
    {"networkContributionsEpochTail",       Slot::RosterTail,    &networkContributionsEpochTail},
    {"networkPostfitSigma0",                Slot::FrameDiag,     &networkPostfitSigma0},
    {"networkShSpectrum",                   Slot::FrameDiag,     &networkShSpectrum},
    {"networkStationBlock",                 Slot::FrameDiag,     &networkStationBlock},
    {"networkPrefitCensus",                 Slot::FrameDiag,     &networkPrefitCensus},
    {"networkRedundancyCensus",             Slot::FrameDiag,     &networkRedundancyCensus},
    {"networkBlockBaardaEmit",              Slot::FrameDiag,     &networkBlockBaardaEmit},
    {"networkCongruenceEmit",               Slot::FrameDiag,     &networkCongruenceEmit},
    {"networkMdbFlush",                     Slot::FrameDiag,     &networkMdbFlush},
    {"networkDatumGraphCensus",             Slot::FrameDiag,     &networkDatumGraphCensus},
    {"networkRhsAttribution",               Slot::FrameDiag,     &networkRhsAttribution},
    {"networkRhsAttribTrip",                Slot::FrameDiag,     &networkRhsAttribTrip},
    {"networkAmbStateTrace",                Slot::StashTail,     &networkAmbStateTrace},
};

/// The row named @p name, or the table's size when no row has that name.
std::size_t rowOf(const char* name) {
    for (std::size_t i = 0; i < kNetworkEpochObservers.size(); ++i)
        if (std::string(kNetworkEpochObservers[i].name) == name) return i;
    return kNetworkEpochObservers.size();
}

TEST(NetworkEpochObservers, EveryRowIsFilledAndAppearsOnce) {
    std::set<std::string> names;
    std::set<NetworkEpochObserver> seams;
    for (const NetworkEpochObserverRow& row : kNetworkEpochObservers) {
        ASSERT_NE(row.name, nullptr);
        ASSERT_NE(row.what, nullptr) << row.name;
        ASSERT_NE(row.home, nullptr) << row.name;
        ASSERT_NE(row.observe, nullptr) << row.name;
        EXPECT_FALSE(std::string(row.what).empty()) << row.name;
        EXPECT_TRUE(names.insert(row.name).second) << "row named twice: " << row.name;
        EXPECT_TRUE(seams.insert(row.observe).second) << "seam listed twice: " << row.name;
        const std::string home(row.home);
        EXPECT_EQ(home.rfind(".cpp"), home.size() - 4) << row.name << ": " << home;
    }
}

TEST(NetworkEpochObservers, TableIsInSpineOrder) {
    for (std::size_t i = 1; i < kNetworkEpochObservers.size(); ++i)
        EXPECT_LE(kNetworkEpochObservers[i - 1].slot, kNetworkEpochObservers[i].slot)
            << kNetworkEpochObservers[i].name << " is above a row of an earlier slot";
}

TEST(NetworkEpochObservers, RowsSlotsAndOrderArePinned) {
    constexpr std::size_t n = sizeof(kPinned) / sizeof(kPinned[0]);
    static_assert(n == kNetworkEpochObserverCount, "the pinned list and the table differ in size");
    for (std::size_t i = 0; i < n; ++i) {
        const NetworkEpochObserverRow& row = kNetworkEpochObservers[i];
        EXPECT_STREQ(row.name, kPinned[i].name) << "row " << i;
        EXPECT_EQ(row.slot, kPinned[i].slot) << row.name;
        EXPECT_EQ(row.observe, kPinned[i].observe) << row.name << " points at another seam";
    }
}

TEST(NetworkEpochObservers, OrderConstraintsHold) {
    // RosterTail: the parameters-as-armed table before the contributions-as-armed table.
    EXPECT_LT(rowOf("networkParamInventoryEpochTail"), rowOf("networkContributionsEpochTail"));
    // FrameDiag: the post-fit sigma0 before the MDB drain (its scaled variant).
    EXPECT_LT(rowOf("networkPostfitSigma0"), rowOf("networkMdbFlush"));
    // The prefit walk before the redundancy census it escalates from.
    EXPECT_LT(rowOf("networkPrefitCensus"), rowOf("networkRedundancyCensus"));
    // The redundancy census before the drains it feeds and before the attribution window.
    for (const char* later : {"networkBlockBaardaEmit", "networkCongruenceEmit",
                              "networkMdbFlush", "networkRhsAttribution"})
        EXPECT_LT(rowOf("networkRedundancyCensus"), rowOf(later)) << later;
    // The attribution, then the flight recorder, last of its slot.
    EXPECT_LT(rowOf("networkRhsAttribution"), rowOf("networkRhsAttribTrip"));
    for (const NetworkEpochObserverRow& row : kNetworkEpochObservers)
        if (row.slot == Slot::FrameDiag && std::string(row.name) != "networkRhsAttribTrip")
            EXPECT_LT(rowOf(row.name), rowOf("networkRhsAttribTrip")) << row.name;
}

} // namespace
} // namespace GnssProcessor
