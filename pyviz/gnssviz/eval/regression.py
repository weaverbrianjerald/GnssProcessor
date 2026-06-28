"""Cross-checks and regression gating for the test suite.

Two independent guards:

* :func:`cross_check_truth` -- compare two truth backends (e.g. SINEX vs SIF) for
  a set of stations at an epoch, flagging any whose coordinates disagree beyond a
  tolerance.  Catches a bad/stale truth coordinate before it silently biases the
  scorecard (long-running IGS stations agree sub-cm; a freshly-installed station
  extrapolated past the solution span may not).

* :func:`compare_metrics` -- diff a current scorecard against a stored *golden*
  one, flagging any (station, cell) whose ENU bias or 3D-RMS moved beyond
  tolerance.  This is the regression gate: a config or physics change that shifts
  a position by more than the tolerance trips it.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np


# ---------------------------------------------------------------------------
# Truth cross-check
# ---------------------------------------------------------------------------

@dataclass
class CrossCheckRow:
    station: str
    primary_source: str
    secondary_source: str
    dist_m: float
    enu_m: Tuple[float, float, float]
    ok: bool
    note: str = ""

    def one_line(self) -> str:
        flag = "ok " if self.ok else "DIFF"
        e, n, u = (v * 100 for v in self.enu_m)
        return (f"  [{flag}] {self.station:<10} |Δ|={self.dist_m*100:6.2f} cm "
                f"(E{e:+6.2f} N{n:+6.2f} U{u:+6.2f})  "
                f"{self.primary_source} vs {self.secondary_source}"
                + (f"  {self.note}" if self.note else ""))


def cross_check_truth(primary, secondary, stations: Sequence[str],
                      epoch_decimal_year: float, *, tol_m: float = 0.05):
    """Compare two :class:`TruthSource` backends over ``stations``.

    Returns ``(rows, n_flagged)``.  A station is flagged when both backends
    resolve it but their coordinates differ by more than ``tol_m`` (default
    5 cm).  Stations only one backend knows are reported as informational
    (``ok=True``, with a note) rather than failures.  Needs
    :func:`gnssviz.geodesy.ecef_to_enu` / ``ecef_to_lla`` for the ENU breakdown.
    """
    from ..geodesy import ecef_to_enu, ecef_to_lla

    rows: List[CrossCheckRow] = []
    n_flagged = 0
    for sta in stations:
        a = primary.resolve(sta, epoch_decimal_year)
        b = secondary.resolve(sta, epoch_decimal_year)
        if a is None and b is None:
            rows.append(CrossCheckRow(sta, "-", "-", float("nan"),
                                      (0, 0, 0), True, "neither backend resolves"))
            continue
        if a is None or b is None:
            have = a or b
            rows.append(CrossCheckRow(
                sta, a.source if a else "-", b.source if b else "-",
                float("nan"), (0, 0, 0), True,
                f"only {have.source} resolves (no cross-check)"))
            continue
        d = np.asarray(a.ecef) - np.asarray(b.ecef)
        lat, lon, h = (float(np.ravel(v)[0]) for v in ecef_to_lla(*b.ecef))
        e, n, u = (float(np.ravel(v)[0]) for v in
                   ecef_to_enu(a.ecef[0], a.ecef[1], a.ecef[2], lat, lon, h))
        dist = float(np.linalg.norm(d))
        ok = dist <= tol_m
        if not ok:
            n_flagged += 1
        rows.append(CrossCheckRow(sta, a.source, b.source, dist, (e, n, u), ok))
    return rows, n_flagged


# ---------------------------------------------------------------------------
# Golden-metrics regression gate
# ---------------------------------------------------------------------------

def index_metrics(metrics_list: Sequence[dict]) -> Dict[Tuple[str, str], dict]:
    """Index a scorecard ``metrics`` list by ``(station, cell)``."""
    out: Dict[Tuple[str, str], dict] = {}
    for m in metrics_list:
        out[(m.get("station", ""), m.get("cell", ""))] = m
    return out


@dataclass
class MetricDelta:
    key: Tuple[str, str]
    status: str                       # ok | regressed | new | missing
    d_bias_enu_cm: Optional[Tuple[float, float, float]] = None
    d_rms_3d_cm: Optional[float] = None
    reasons: Tuple[str, ...] = ()

    def one_line(self) -> str:
        sta, cell = self.key
        tag = {"ok": "ok ", "regressed": "FAIL", "new": "new ",
               "missing": "MISS"}[self.status]
        head = f"  [{tag}] {sta}/{cell:<16}"
        if self.d_bias_enu_cm is None:
            return head + ("(not in golden)" if self.status == "new"
                           else "(absent from current)")
        e, n, u = self.d_bias_enu_cm
        s = (f"{head} Δbias E{e:+6.2f} N{n:+6.2f} U{u:+6.2f} cm  "
             f"Δ3D-RMS {self.d_rms_3d_cm:+6.2f} cm")
        if self.reasons:
            s += "   <- " + "; ".join(self.reasons)
        return s


def compare_metrics(current: Sequence[dict], golden: Sequence[dict], *,
                    bias_tol_m: float = 0.02, rms_tol_m: float = 0.02):
    """Diff a current scorecard against a golden one.

    Returns ``(deltas, n_regressed)``.  A cell *regresses* when any ENU bias
    component or the 3D-RMS moved by more than its tolerance (defaults 2 cm).
    Cells present in only one set are reported as ``new`` / ``missing`` (not
    counted as regressions -- the matrix shape changing is a separate concern).
    """
    cur = index_metrics(current)
    gold = index_metrics(golden)
    deltas: List[MetricDelta] = []
    n_regressed = 0

    for key in sorted(set(cur) | set(gold)):
        if key not in gold:
            deltas.append(MetricDelta(key, "new"))
            continue
        if key not in cur:
            deltas.append(MetricDelta(key, "missing"))
            continue
        c, g = cur[key], gold[key]
        cb = np.asarray(c.get("bias_enu_m", [np.nan] * 3), float)
        gb = np.asarray(g.get("bias_enu_m", [np.nan] * 3), float)
        cr = float(c.get("rms_3d_m", np.nan))
        gr = float(g.get("rms_3d_m", np.nan))
        dbias = cb - gb
        drms = cr - gr
        reasons = []
        # A finite<->NaN transition in any component is itself a regression: a run
        # diverged (lost a finite solution) or a previously-broken cell recovered.
        # The magnitude tests below silently ignore NaN, so without this an
        # all-NaN diverged run would sail through the gate.
        nan_flip = bool(np.any(np.isnan(cb) != np.isnan(gb))
                        or (np.isnan(cr) != np.isnan(gr)))
        if nan_flip:
            diverged = bool(np.isnan(cb).any() or np.isnan(cr))
            reasons.append("metric became NaN (run diverged)" if diverged
                           else "metric recovered from NaN")
        finite_bias = np.isfinite(dbias)
        if np.any(finite_bias & (np.abs(dbias) > bias_tol_m)):
            j = int(np.nanargmax(np.where(finite_bias, np.abs(dbias), np.nan)))
            reasons.append(f"bias {'ENU'[j]} moved {dbias[j]*100:+.1f} cm "
                           f"(tol {bias_tol_m*100:.0f})")
        if np.isfinite(drms) and abs(drms) > rms_tol_m:
            reasons.append(f"3D-RMS moved {drms*100:+.1f} cm (tol {rms_tol_m*100:.0f})")
        status = "regressed" if reasons else "ok"
        if status == "regressed":
            n_regressed += 1
        deltas.append(MetricDelta(
            key, status,
            d_bias_enu_cm=tuple(float(v * 100) for v in dbias),
            d_rms_3d_cm=drms * 100 if np.isfinite(drms) else float("nan"),
            reasons=tuple(reasons)))
    return deltas, n_regressed
