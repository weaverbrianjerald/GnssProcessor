"""Canonical-table rows of worker W1 (app): PASS A and PASS B, the signal roster, the border
prior's exact-coupling preparation, the receiver code-bias de-mean pre-pass and the state
snapshot (card NOM3 step EP1, specification sections 3.3 and 7).

One row per converted seam; no row carries a binding or a line (scripts/nom_ep1_cut.py
--survey derives both from the frozen source).  W1 owns Application.hpp (the five member
declarations), ApplicationNetworkAssembly.cpp, SignalRoster.{hpp,cpp}, NetworkRunContext.hpp
(three declarations), NetworkBorderPrior.cpp, NetworkStateSnapshotWrite.cpp and
NetworkLocalCarryHarvest.cpp.
"""
from nom_ep1 import REMOVE_KEY
from nom_ep1.model import Seam, DIRECT, MEMBER

APP_HPP = "include/app/Application.hpp"
ASSEMBLY_CPP = "src/core/app/ApplicationNetworkAssembly.cpp"
ROSTER_HPP = "include/app/SignalRoster.hpp"
ROSTER_CPP = "src/core/app/SignalRoster.cpp"
RUNCTX_HPP = "include/app/NetworkRunContext.hpp"

ROWS = (
    Seam("networkPassAHead_", hpp=APP_HPP, cpp=ASSEMBLY_CPP, mech=MEMBER,
         outs=("pass_a",), worker="W1"),
    Seam("networkPassASlipScan_", hpp=APP_HPP, cpp=ASSEMBLY_CPP, mech=MEMBER,
         outs=("pass_a", "funnel"), worker="W1"),
    Seam("networkAssemblyPrefilter_", hpp=APP_HPP, cpp=ASSEMBLY_CPP, mech=MEMBER,
         outs=("pass_a",), worker="W1"),
    Seam("networkAssemblyRegisterColumns_", hpp=APP_HPP, cpp=ASSEMBLY_CPP, mech=MEMBER,
         outs=("columns",), worker="W1"),
    Seam("networkPassB_", hpp=APP_HPP, cpp=ASSEMBLY_CPP, mech=MEMBER,
         outs=("pass_a", "assembly"), worker="W1"),
    Seam("networkBuildSignalRoster", hpp=ROSTER_HPP, cpp=ROSTER_CPP, mech=DIRECT, worker="W1"),
    Seam("networkSignalRosterAudit", hpp=ROSTER_HPP, cpp=ROSTER_CPP, mech=DIRECT, worker="W1"),
    Seam("networkCouplingExactPrepare", hpp=RUNCTX_HPP,
         cpp="src/core/app/NetworkBorderPrior.cpp", mech=DIRECT, worker="W1"),
    Seam("networkRxCbDemeanPrepass", hpp=RUNCTX_HPP,
         cpp="src/core/app/NetworkLocalCarryHarvest.cpp", mech=DIRECT, worker="W1",
         retired_by=REMOVE_KEY),
    Seam("networkWriteStateSnapshot", hpp=RUNCTX_HPP,
         cpp="src/core/app/NetworkStateSnapshotWrite.cpp", mech=DIRECT, worker="W1"),
)
