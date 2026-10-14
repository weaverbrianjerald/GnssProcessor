"""Canonical-table rows of worker W3 (emit and post-fit): producer AR, the gross-LOS screen,
the fixed-cycle fold, solution prep, the PB-hold clones, the emitter, RAIM, clock QC, the
stash, the clock level and the RHS attribution (card NOM3 step EP1, specification sections 3.3
and 7).

One row per converted seam; no row carries a binding or a line (scripts/nom_ep1_cut.py
--survey derives both from the frozen source).  After the solve the only outputs are the
post-fit hand-off (RAIM, clock QC) and the prepared solution (the emitter).
"""
from nom_ep1.model import Seam, DIRECT, ADAPTER, RUN


def _f(stem):
    return f"include/filter/{stem}.hpp", f"src/core/filter/{stem}.cpp"


PBHOLD = _f("NetworkPbHoldClone")
CLOCKLEVEL = _f("NetworkClockLevel")
RHS = _f("NetworkRhsAttrib")

ROWS = (
    Seam("networkProducerAr", *_f("NetworkProducerAr"), mech=DIRECT, worker="W3"),
    Seam("networkGrossLosScreen", *_f("NetworkGrossLos"), mech=DIRECT, kept=("phase",),
         worker="W3", sites=2),
    Seam("networkFixedCycles", *_f("NetworkFixedCycles"), mech=DIRECT, worker="W3"),
    Seam("networkSolPrep", *_f("NetworkSolPrep"), mech=ADAPTER, worker="W3"),
    Seam("networkPbHoldCloneUpdate", *PBHOLD, mech=ADAPTER, worker="W3"),
    Seam("networkPbHoldCloneHarvest", *PBHOLD, mech=ADAPTER, worker="W3"),
    Seam("networkEmitStore", *_f("NetworkEmitStore"), mech=DIRECT, outs=("sol",), worker="W3"),
    Seam("networkPostFitRaim", *_f("NetworkRaim"), mech=DIRECT, outs=("handoff",), worker="W3"),
    Seam("networkClockQc", *_f("NetworkClockQc"), mech=DIRECT, outs=("handoff",), worker="W3"),
    Seam("networkStashStatus", *_f("NetworkStashStatus"), mech=DIRECT, worker="W3"),
    Seam("networkClockLevelRestoreRaise", *CLOCKLEVEL, mech=RUN, worker="W3"),
    Seam("networkClockLevelRebase", *CLOCKLEVEL, mech=DIRECT, worker="W3"),
    Seam("networkClockLevelMonitor", *CLOCKLEVEL, mech=DIRECT, worker="W3"),
    Seam("networkRhsAttribution", *RHS, mech=DIRECT, observer=True, worker="W3"),
    Seam("networkRhsAttribTrip", *RHS, mech=ADAPTER, observer=True, worker="W3"),
)
