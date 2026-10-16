/**
 * @file EpochObservers.cpp
 * @brief The observer table, kNetworkEpochObservers: every measure-only per-epoch instrument
 *        of the network producer in spine order, with its slot, what it measures and its home
 *        file (card NOM3 step EP3).  network/driver/EpochObservers.hpp states how to add a row,
 *        the slots and the order constraints.
 *
 * Each row's seam is declared in the header of its home file and gates itself on its first
 * line after its bindings.
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

namespace GnssProcessor {

using Slot = NetworkObserverSlot;

const std::array<NetworkEpochObserverRow, kNetworkEpochObserverCount> kNetworkEpochObservers = {{
    // ---- Admitted: after the admission ----
    {"networkParamInventoryNoteSatellites", "satellite ids of no modelled constellation, and SBAS",
     "filter/ParamInventory.cpp", Slot::Admitted, &networkParamInventoryNoteSatellites},
    // ---- BeforeColumns: after PASS A and the receiver phase-bias allocation ----
    {"networkLevelAuthorityCensus", "level-authority census (LV-6)",
     "filter/LevelAuthority.cpp", Slot::BeforeColumns, &networkLevelAuthorityCensus},
    // ---- AfterClockQc: after the epoch's last re-solve ----
    {"networkPhaseVisibleMonitor", "phase-visible consistency (card PVN)",
     "filter/PhaseVisibleDiag.cpp", Slot::AfterClockQc, &networkPhaseVisibleMonitor},
    // ---- RosterTail: after the roster audit ----
    {"networkLedgerEmitFold", "ledger emit, fold half (card SID phase 5a)",
     "filter/LedgerEmit.cpp", Slot::RosterTail, &networkLedgerEmitFold},
    {"networkSecondPhaseEpochTail", "second phase channel, epoch tail (card SID phase 6a)",
     "filter/SecondPhaseScope.cpp", Slot::RosterTail, &networkSecondPhaseEpochTail},
    {"networkChannelAllEpoch", "all-channel census (card SID phase 6b)",
     "filter/ChannelAll.cpp", Slot::RosterTail, &networkChannelAllEpoch},
    {"networkMultiGnssEpoch", "second-constellation census (card GAL)",
     "filter/MultiGnss.cpp", Slot::RosterTail, &networkMultiGnssEpoch},
    {"networkParamInventoryEpochTail", "parameters as armed, once (card NOM N1)",
     "filter/ParamInventory.cpp", Slot::RosterTail, &networkParamInventoryEpochTail},
    {"networkContributionsEpochTail", "contributions as armed, once (card NOM N2a)",
     "filter/Contributions.cpp", Slot::RosterTail, &networkContributionsEpochTail},
    // ---- FrameDiag: after the two predict halves, before the stash ----
    {"networkPostfitSigma0", "post-fit sigma0 and the residual traces (P1-3/P1-4)",
     "filter/FrameDiag.cpp", Slot::FrameDiag, &networkPostfitSigma0},
    {"networkShSpectrum", "reduced shared-normal spectrum",
     "filter/FrameDiag.cpp", Slot::FrameDiag, &networkShSpectrum},
    {"networkStationBlock", "per-station local-block audit (card ZP)",
     "filter/StationBlock.cpp", Slot::FrameDiag, &networkStationBlock},
    {"networkPrefitCensus", "prefit census, seed-birth tripwire, topology ledger",
     "filter/PrefitCensus.cpp", Slot::FrameDiag, nullptr, &networkPrefitCensus},
    {"networkRedundancyCensus", "redundancy / testability census",
     "filter/RedundancyCensus.cpp", Slot::FrameDiag, nullptr, &networkRedundancyCensus},
    {"networkBlockBaardaEmit", "block-Baarda drain",
     "filter/BlockBaarda.cpp", Slot::FrameDiag, &networkBlockBaardaEmit},
    {"networkCongruenceEmit", "station-congruence drain",
     "filter/Congruence.cpp", Slot::FrameDiag, &networkCongruenceEmit},
    {"networkMdbFlush", "MDB drain",
     "filter/Mdb.cpp", Slot::FrameDiag, &networkMdbFlush},
    {"networkDatumGraphCensus", "structural S-basis dimension census",
     "filter/DatumGraph.cpp", Slot::FrameDiag, &networkDatumGraphCensus},
    {"networkRhsAttribution", "reduced-RHS attribution window",
     "filter/RhsAttrib.cpp", Slot::FrameDiag, &networkRhsAttribution},
    {"networkRhsAttribTrip", "flight recorder (INC-5)",
     "filter/RhsAttrib.cpp", Slot::FrameDiag, &networkRhsAttribTrip},
    // ---- StashTail: after every carry-state row source ----
    {"networkAmbStateTrace", "carry-state trace drain (RF-1 7.4)",
     "filter/AmbStateTrace.cpp", Slot::StashTail, &networkAmbStateTrace},
    {"networkEntitiesReachSample", "the entity tables' reach sampler (card NOM step N4)",
     "app/Entities.cpp", Slot::StashTail, &net::networkEntitiesReachSample},
}};

void networkObserveEpoch(NetworkRun& run, const NetworkEpoch& ep, NetworkObserverSlot slot, EpochEvents& events) {
    for (const NetworkEpochObserverRow& row : kNetworkEpochObservers)
        if (row.slot == slot) {
            if (row.record) row.record(run, ep, events);
            else            row.observe(run, ep);
        }
}

} // namespace GnssProcessor
