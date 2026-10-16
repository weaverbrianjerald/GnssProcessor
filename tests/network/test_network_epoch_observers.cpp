/**
 * @file test_network_epoch_observers.cpp
 * @brief The observer table: every row is filled and appears once, the
 *        table is in spine order, and the rows, their slots and their order are pinned.
 *
 * The table is the epoch's instrument order, and a reordered or re-slotted row changes the
 * interleaving of trace rows, events and log lines without any unit test noticing; the order
 * constraints of network/driver/EpochObservers.hpp are checked by name below, and the whole
 * table is pinned so a change to it is a deliberate edit here.
 */

#include "network/driver/EpochObservers.hpp"

#include "network/state/Entities.hpp"
#include "network/diag/AmbStateTrace.hpp"
#include "network/qc/BlockBaarda.hpp"
#include "network/assembly/ChannelAll.hpp"
#include "network/qc/Congruence.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/solve/DatumGraph.hpp"
#include "network/diag/FrameDiag.hpp"
#include "network/emit/LedgerEmit.hpp"
#include "network/carry/LevelAuthority.hpp"
#include "network/qc/Mdb.hpp"
#include "network/assembly/MultiGnss.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/diag/PhaseVisibleDiag.hpp"
#include "network/qc/PrefitCensus.hpp"
#include "network/qc/RedundancyCensus.hpp"
#include "network/qc/RhsAttrib.hpp"
#include "network/assembly/SecondPhaseScope.hpp"
#include "network/diag/StationBlock.hpp"

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
    NetworkEpochRecorder record{nullptr};   ///< Set, with observe null, on a recorder row.
};

/// The table as pre-registered (scripts/nom3_designs/EP3_design.md).
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
    {"networkPrefitCensus",                 Slot::FrameDiag,     nullptr, &networkPrefitCensus},
    {"networkRedundancyCensus",             Slot::FrameDiag,     nullptr, &networkRedundancyCensus},
    {"networkBlockBaardaEmit",              Slot::FrameDiag,     &networkBlockBaardaEmit},
    {"networkCongruenceEmit",               Slot::FrameDiag,     &networkCongruenceEmit},
    {"networkMdbFlush",                     Slot::FrameDiag,     &networkMdbFlush},
    {"networkDatumGraphCensus",             Slot::FrameDiag,     &networkDatumGraphCensus},
    {"networkRhsAttribution",               Slot::FrameDiag,     &networkRhsAttribution},
    {"networkRhsAttribTrip",                Slot::FrameDiag,     &networkRhsAttribTrip},
    {"networkAmbStateTrace",                Slot::StashTail,     &networkAmbStateTrace},
    {"networkEntitiesReachSample",          Slot::StashTail,     &net::networkEntitiesReachSample},
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
    std::set<NetworkEpochRecorder> recorders;
    for (const NetworkEpochObserverRow& row : kNetworkEpochObservers) {
        ASSERT_NE(row.name, nullptr);
        ASSERT_NE(row.what, nullptr) << row.name;
        ASSERT_NE(row.home, nullptr) << row.name;
        ASSERT_NE(row.observe == nullptr, row.record == nullptr) << row.name << ": exactly one form";
        EXPECT_FALSE(std::string(row.what).empty()) << row.name;
        EXPECT_TRUE(names.insert(row.name).second) << "row named twice: " << row.name;
        if (row.observe)
            EXPECT_TRUE(seams.insert(row.observe).second) << "seam listed twice: " << row.name;
        else
            EXPECT_TRUE(recorders.insert(row.record).second) << "seam listed twice: " << row.name;
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
        EXPECT_EQ(row.record, kPinned[i].record) << row.name << " points at another seam";
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
