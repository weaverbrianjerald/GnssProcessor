"""Canonical-table rows of worker W4 (diagnostics and observers): the post-solve ionosphere,
DD-fraction, SH and leveling diagnostics, and the measure-only instruments of the roster tail,
the frame diagnostics and the stash (card NOM3 step EP1, specification sections 3.3 and 7).

One row per converted seam; no row carries a binding or a line (scripts/nom_ep1_cut.py
--survey derives both from the frozen source).  observer=True marks a measure-only seam whose
epoch form must be NetworkEpochObserver, void(NetworkRun&, const NetworkEpoch&).
"""
from nom_ep1.model import Seam, DIRECT, ADAPTER


def _f(stem):
    return f"include/filter/{stem}.hpp", f"src/core/filter/{stem}.cpp"


PARAMINV = _f("NetworkParamInventory")
FRAMEDIAG = _f("NetworkFrameDiag")

ROWS = (
    Seam("networkIonoDiag", *_f("NetworkIonoDiag"), mech=DIRECT, worker="W4"),
    Seam("networkDdFrac", *_f("NetworkDdFrac"), mech=DIRECT, worker="W4"),
    Seam("networkShDevVtec", *_f("NetworkShDevVtec"), mech=DIRECT, worker="W4"),
    Seam("networkShHealth", *_f("NetworkShHealth"), mech=DIRECT, worker="W4"),
    Seam("networkIonoDwRelease", *_f("NetworkIonoDwRelease"), mech=DIRECT, worker="W4"),
    Seam("networkLeveling", *_f("NetworkLeveling"), mech=DIRECT, worker="W4"),
    Seam("networkPhaseVisibleMonitor", *_f("NetworkPhaseVisibleDiag"), mech=ADAPTER,
         observer=True, worker="W4"),
    Seam("networkLedgerEmitFold", *_f("NetworkLedgerEmit"), mech=ADAPTER, observer=True,
         worker="W4"),
    Seam("networkSecondPhaseEpochTail", *_f("NetworkSecondPhaseScope"), mech=DIRECT,
         observer=True, worker="W4"),
    Seam("networkChannelAllEpoch", *_f("NetworkChannelAll"), mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkMultiGnssEpoch", *_f("NetworkMultiGnss"), mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkParamInventoryNoteSatellites", *PARAMINV, mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkParamInventoryEpochTail", *PARAMINV, mech=DIRECT, observer=True, worker="W4"),
    Seam("networkContributionsEpochTail", *_f("NetworkContributions"), mech=DIRECT,
         observer=True, worker="W4"),
    Seam("networkPostfitSigma0", *FRAMEDIAG, mech=DIRECT, observer=True, worker="W4"),
    Seam("networkShSpectrum", *FRAMEDIAG, mech=DIRECT, observer=True, worker="W4"),
    Seam("networkStationBlock", *_f("NetworkStationBlock"), mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkPrefitCensus", *_f("NetworkPrefitCensus"), mech=ADAPTER, observer=True,
         worker="W4"),
    Seam("networkRedundancyCensus", *_f("NetworkRedundancyCensus"), mech=DIRECT,
         observer=True, worker="W4"),
    Seam("networkBlockBaardaEmit", *_f("NetworkBlockBaarda"), mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkCongruenceEmit", *_f("NetworkCongruence"), mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkMdbFlush", *_f("NetworkMdb"), mech=DIRECT, observer=True, worker="W4"),
    Seam("networkDatumGraphCensus", *_f("NetworkDatumGraph"), mech=DIRECT, observer=True,
         worker="W4"),
    Seam("networkAmbStateTrace", *_f("NetworkAmbStateTrace"), mech=DIRECT, observer=True,
         worker="W4"),
)
