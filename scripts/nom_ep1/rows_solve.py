"""Canonical-table rows of worker W2 (pre-solve and solve): the fast clock, the PASS-A
screens, the level-authority census, SD-EWL AR, the oracle pin, the handover S-transform, the
carry disposition, the two DD-prior builders, the solve and the carry install (card NOM3 step
EP1, specification sections 3.3, 3.4 and 7).

One row per converted seam; no row carries a binding or a line (scripts/nom_ep1_cut.py
--survey derives both from the frozen source).  The solve's outputs are the members its T2
replay rewrites: ws, est_index_of, and, through the DD build it now calls directly, dd_build
and funnel.
"""
from nom_ep1 import REMOVE_KEY
from nom_ep1.model import Seam, DIRECT, ADAPTER


def _f(stem):
    return f"include/filter/{stem}.hpp", f"src/core/filter/{stem}.cpp"


FASTCLOCK = _f("NetworkFastClock")
DDBUILD = _f("NetworkDdPriorBuild")

ROWS = (
    Seam("networkFastClockTick", *FASTCLOCK, mech=DIRECT, kept=("shadow_pass",), worker="W2",
         sites=2, retired_by=REMOVE_KEY),
    Seam("networkFastClockHarvest", *FASTCLOCK, mech=DIRECT, worker="W2", retired_by=REMOVE_KEY),
    Seam("networkSignalIntegrity", *_f("NetworkSignalIntegrity"), mech=ADAPTER,
         outs=("pass_a", "funnel"), worker="W2"),
    Seam("networkMeasurementAdmission", *_f("NetworkMeasurementAdmission"), mech=ADAPTER,
         outs=("pass_a", "funnel"), worker="W2"),
    Seam("networkLevelAuthorityCensus", *_f("NetworkLevelAuthority"), mech=DIRECT,
         observer=True, worker="W2"),
    Seam("networkSdEwlAr", *_f("NetworkSdEwlAr"), mech=DIRECT, worker="W2"),
    Seam("networkOraclePinEpoch", *_f("NetworkOraclePin"), mech=DIRECT, worker="W2"),
    Seam("networkHandoverStransformBeforeRows", *_f("NetworkHandoverStransform"), mech=ADAPTER,
         worker="W2"),
    Seam("networkCarryDecide", *_f("NetworkCarryDisposition"), mech=ADAPTER, worker="W2"),
    Seam("networkBuildEnsembleGauges", *DDBUILD, mech=DIRECT, worker="W2", retired_by=REMOVE_KEY),
    Seam("networkBuildCarryDdPriors", *DDBUILD, mech=DIRECT, outs=("dd_build", "funnel"),
         worker="W2"),
    Seam("networkSolveGlue", *_f("NetworkSolveGlue"), mech=DIRECT,
         outs=("ws", "est_index_of", "dd_build", "funnel"), worker="W2"),
    Seam("networkInstallCarry", *_f("NetworkInstallCarry"), mech=DIRECT, worker="W2"),
)
