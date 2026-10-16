/**
 * @file NetworkEpochObservers.cpp
 * @brief The observer table, kNetworkEpochObservers: every measure-only per-epoch instrument
 *        of the network producer in spine order, with its slot, what it measures and its home
 *        file (card NOM3 step EP3).  filter/NetworkEpochObservers.hpp states how to add a row,
 *        the slots and the order constraints.
 *
 * Each row's seam is declared in the header of its home file and gates itself on its first
 * line after its bindings.
 */

#include "filter/NetworkEpochObservers.hpp"

#include "app/NetworkEntities.hpp"
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

namespace GnssProcessor {

using Slot = NetworkObserverSlot;

const std::array<NetworkEpochObserverRow, kNetworkEpochObserverCount> kNetworkEpochObservers = {{
    // ---- Admitted: after the admission ----
    {"networkParamInventoryNoteSatellites", "satellite ids of no modelled constellation, and SBAS",
     "filter/NetworkParamInventory.cpp", Slot::Admitted, &networkParamInventoryNoteSatellites},
    // ---- BeforeColumns: after PASS A and the receiver phase-bias allocation ----
    {"networkLevelAuthorityCensus", "level-authority census (LV-6)",
     "filter/NetworkLevelAuthority.cpp", Slot::BeforeColumns, &networkLevelAuthorityCensus},
    // ---- AfterClockQc: after the epoch's last re-solve ----
    {"networkPhaseVisibleMonitor", "phase-visible consistency (card PVN)",
     "filter/NetworkPhaseVisibleDiag.cpp", Slot::AfterClockQc, &networkPhaseVisibleMonitor},
    // ---- RosterTail: after the roster audit ----
    {"networkLedgerEmitFold", "ledger emit, fold half (card SID phase 5a)",
     "filter/NetworkLedgerEmit.cpp", Slot::RosterTail, &networkLedgerEmitFold},
    {"networkSecondPhaseEpochTail", "second phase channel, epoch tail (card SID phase 6a)",
     "filter/NetworkSecondPhaseScope.cpp", Slot::RosterTail, &networkSecondPhaseEpochTail},
    {"networkChannelAllEpoch", "all-channel census (card SID phase 6b)",
     "filter/NetworkChannelAll.cpp", Slot::RosterTail, &networkChannelAllEpoch},
    {"networkMultiGnssEpoch", "second-constellation census (card GAL)",
     "filter/NetworkMultiGnss.cpp", Slot::RosterTail, &networkMultiGnssEpoch},
    {"networkParamInventoryEpochTail", "parameters as armed, once (card NOM N1)",
     "filter/NetworkParamInventory.cpp", Slot::RosterTail, &networkParamInventoryEpochTail},
    {"networkContributionsEpochTail", "contributions as armed, once (card NOM N2a)",
     "filter/NetworkContributions.cpp", Slot::RosterTail, &networkContributionsEpochTail},
    // ---- FrameDiag: after the two predict halves, before the stash ----
    {"networkPostfitSigma0", "post-fit sigma0 and the residual traces (P1-3/P1-4)",
     "filter/NetworkFrameDiag.cpp", Slot::FrameDiag, &networkPostfitSigma0},
    {"networkShSpectrum", "reduced shared-normal spectrum",
     "filter/NetworkFrameDiag.cpp", Slot::FrameDiag, &networkShSpectrum},
    {"networkStationBlock", "per-station local-block audit (card ZP)",
     "filter/NetworkStationBlock.cpp", Slot::FrameDiag, &networkStationBlock},
    {"networkPrefitCensus", "prefit census, seed-birth tripwire, topology ledger",
     "filter/NetworkPrefitCensus.cpp", Slot::FrameDiag, nullptr, &networkPrefitCensus},
    {"networkRedundancyCensus", "redundancy / testability census",
     "filter/NetworkRedundancyCensus.cpp", Slot::FrameDiag, nullptr, &networkRedundancyCensus},
    {"networkBlockBaardaEmit", "block-Baarda drain",
     "filter/NetworkBlockBaarda.cpp", Slot::FrameDiag, &networkBlockBaardaEmit},
    {"networkCongruenceEmit", "station-congruence drain",
     "filter/NetworkCongruence.cpp", Slot::FrameDiag, &networkCongruenceEmit},
    {"networkMdbFlush", "MDB drain",
     "filter/NetworkMdb.cpp", Slot::FrameDiag, &networkMdbFlush},
    {"networkDatumGraphCensus", "structural S-basis dimension census",
     "filter/NetworkDatumGraph.cpp", Slot::FrameDiag, &networkDatumGraphCensus},
    {"networkRhsAttribution", "reduced-RHS attribution window",
     "filter/NetworkRhsAttrib.cpp", Slot::FrameDiag, &networkRhsAttribution},
    {"networkRhsAttribTrip", "flight recorder (INC-5)",
     "filter/NetworkRhsAttrib.cpp", Slot::FrameDiag, &networkRhsAttribTrip},
    // ---- StashTail: after every carry-state row source ----
    {"networkAmbStateTrace", "carry-state trace drain (RF-1 7.4)",
     "filter/NetworkAmbStateTrace.cpp", Slot::StashTail, &networkAmbStateTrace},
    {"networkEntitiesReachSample", "the entity tables' reach sampler (card NOM step N4)",
     "app/NetworkEntities.cpp", Slot::StashTail, &net::networkEntitiesReachSample},
}};

void networkObserveEpoch(NetworkRun& run, const NetworkEpoch& ep, NetworkObserverSlot slot, EpochEvents& events) {
    for (const NetworkEpochObserverRow& row : kNetworkEpochObservers)
        if (row.slot == slot) {
            if (row.record) row.record(run, ep, events);
            else            row.observe(run, ep);
        }
}

} // namespace GnssProcessor
