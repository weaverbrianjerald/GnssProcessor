"""Shared helpers for the truth-referenced ``exam_*`` viewers.

The exam dashboard extends the pyviz suite with viewers that compare the real
``gnss_processor`` PPP solution against the **GnssSimulator answer key** (the
``truth_*.csv`` + ``scenario_summary.json`` the synthesizer writes, bound to a
:class:`~gnssviz.trace_paths.TraceContext` via ``--truth-dir``).  Everything the
exam viewers share lives here so the truth-join hygiene (SPEC R5) is written
once:

* **Truth↔trace join** on the integer ``(week, round(tow*1000))`` grid in
  ``int64`` — never a float/string tow compare (:func:`merge_truth_trace`,
  :func:`tow_key`).
* **ENU** about each epoch's truth point via the same rotation as
  :mod:`gnssviz.geodesy` (:func:`enu_error`).
* **Storm window** (from ``truth_scalars``) drawn as paired :class:`VLine`s on
  every time series (:func:`storm_vlines`).
* **Slip classification** (flagged / unflagged / scripted / false-alarm) with an
  optional ``source`` column (:func:`slip_class`).
* Answer-key / verdict readers: ``truth_scalars.csv`` scalars, kinematic mode
  (SPEC R7), ``scenario_summary.json``, and the machine-readable
  ``verify_report.json`` (SPEC R6), plus the report banner it feeds.

None of these import a plotting library — they return prepared pandas / numpy /
:mod:`gnssviz.figures` specs, like every other data-layer helper.
"""

from __future__ import annotations

import html
import json
import os
from typing import Optional

import numpy as np
import pandas as pd

from ..figures import VLine
from ..io import first_col, gps_to_datetime
from .base import ViewerSkip

# Milliseconds per GPS week (the (week, tow-ms) key base).
_MS_PER_WEEK = 7 * 86400 * 1000

# Storm-window marker colour (a muted amber, drawn as paired dashed VLines).
STORM_COLOR = (0.85, 0.45, 0.10)

# Slip-class → RGB (stable across the skyplot + timeline viewers).
SLIP_CLASS_COLORS = {
    "flagged":     (0.15, 0.45, 0.85),   # blue  — LLI=1 (engine warned)
    "unflagged":   (0.85, 0.20, 0.20),   # red   — LLI=0 (must be detected via MW/GF)
    "scripted":    (0.55, 0.30, 0.75),   # purple— hand-authored gotcha
    "false-alarm": (0.55, 0.55, 0.55),   # grey  — Δ=0, LLI=1 only
}
SLIP_CLASS_ORDER = ("flagged", "unflagged", "scripted", "false-alarm")


# --------------------------------------------------------------------------- #
#  Truth-dir file access
# --------------------------------------------------------------------------- #
def truth_frame(ctx, name: str) -> Optional[pd.DataFrame]:
    """Load a truth CSV from ``ctx.truth_dir``; ``None`` if unset/absent/empty.

    A header-only file (logger enabled but never fired) also returns ``None`` so
    a viewer never indexes an empty frame.
    """
    p = ctx.truth_path(name)
    if not p or not os.path.exists(p) or os.path.getsize(p) <= 0:
        return None
    try:
        df = pd.read_csv(p)
    except Exception:
        return None
    return df if not df.empty else None


def require_truth(ctx, name: str) -> pd.DataFrame:
    """Load a truth CSV or raise :class:`ViewerSkip` (clean "skipped" entry)."""
    df = truth_frame(ctx, name)
    if df is None:
        raise ViewerSkip(f"no data in {name} (truth dir {'unset' if not ctx.truth_dir else 'missing/empty file'})")
    return df


# --------------------------------------------------------------------------- #
#  (week, tow-ms) join — the ONLY sanctioned truth↔trace matcher (SPEC R5)
# --------------------------------------------------------------------------- #
def tow_key(week, tow) -> np.ndarray:
    """Integer join key ``week*ms_per_week + round(tow*1000)`` as ``int64``.

    Float tows are snapped to the millisecond grid (``round``) before the cast,
    so two epochs that agree to the ms match exactly and float noise never
    splits a pair.  Never string-compare tows.
    """
    week = np.asarray(week, dtype=float)
    tow_ms = np.rint(np.asarray(tow, dtype=float) * 1000.0)
    return (week.astype(np.int64) * _MS_PER_WEEK + tow_ms.astype(np.int64))


def _with_key(df: pd.DataFrame) -> pd.DataFrame:
    out = df.copy()
    wk = first_col(out, "week")
    tw = first_col(out, "tow_s", "tow")
    if wk is None or tw is None:
        raise ValueError("frame lacks week and/or tow_s/tow columns for the tow join")
    out["_key"] = tow_key(wk, tw)
    return out


def merge_truth_trace(truth_df: pd.DataFrame, trace_df: pd.DataFrame) -> pd.DataFrame:
    """Inner-join truth & trace rows on the integer ``(week, tow-ms)`` key.

    Overlapping column names (``week``, ``tow_s``, ``x_m`` …) get ``_truth`` /
    ``_trace`` suffixes; columns unique to one side keep their name.  Adds a
    ``utc`` datetime column (GPS scale).  ``len(result)`` is the epoch match
    count (surfaced in the scorecard, SPEC R5).
    """
    t = _with_key(truth_df)
    s = _with_key(trace_df)
    m = pd.merge(t, s, on="_key", suffixes=("_truth", "_trace"))
    wk = first_col(m, "week_truth", "week")
    tw = first_col(m, "tow_s_truth", "tow_s", "tow")
    if wk is not None and tw is not None:
        m["utc"] = gps_to_datetime(wk, tw)
    return m


# --------------------------------------------------------------------------- #
#  ENU error about each epoch's truth point (rotation matches geodesy.ecef_to_enu)
# --------------------------------------------------------------------------- #
def enu_error(est_x, est_y, est_z, tru_x, tru_y, tru_z, lat_deg, lon_deg):
    """ENU components of ``(estimate − truth)`` about each row's truth point.

    Vectorized per row (each epoch carries its own truth lat/lon for a kinematic
    run).  Same E/N/U rotation as :func:`gnssviz.geodesy.ecef_to_enu`; the origin
    offset cancels because we difference ECEF first.
    """
    dx = np.asarray(est_x, float) - np.asarray(tru_x, float)
    dy = np.asarray(est_y, float) - np.asarray(tru_y, float)
    dz = np.asarray(est_z, float) - np.asarray(tru_z, float)
    lat = np.radians(np.asarray(lat_deg, float))
    lon = np.radians(np.asarray(lon_deg, float))
    sl, cl = np.sin(lat), np.cos(lat)
    so, co = np.sin(lon), np.cos(lon)
    e = -so * dx + co * dy
    n = -sl * co * dx - sl * so * dy + cl * dz
    u = cl * co * dx + cl * so * dy + sl * dz
    return e, n, u


# --------------------------------------------------------------------------- #
#  Answer-key scalars, mode, summary, verdict
# --------------------------------------------------------------------------- #
def scalars(ctx) -> dict:
    """``truth_scalars.csv`` (key,value) as a dict; floats where parseable."""
    df = truth_frame(ctx, "truth_scalars.csv")
    out: dict = {}
    if df is not None and {"key", "value"}.issubset(df.columns):
        for k, v in zip(df["key"].astype(str), df["value"]):
            try:
                out[k] = float(v)
            except (TypeError, ValueError):
                out[k] = v
    return out


def scenario_summary(ctx) -> Optional[dict]:
    """Parse ``scenario_summary.json`` from the truth dir (``None`` if absent)."""
    p = ctx.truth_path("scenario_summary.json") if ctx.truth_dir else None
    if not p or not os.path.exists(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return None


def verify_report(ctx) -> Optional[dict]:
    """Parse ``verify_report.json`` (SPEC R6).

    Path precedence: an explicit ``ctx.verify_report`` (set by ``run_report.py``
    from ``--verify-report`` / auto-detect) else ``<log_dir>/verify_report.json``
    where the ``verify`` subcommand writes it.  ``None`` when absent/unreadable.
    """
    p = getattr(ctx, "verify_report", None) or ctx.path("verify_report.json")
    if not p or not os.path.exists(p) or os.path.getsize(p) <= 0:
        return None
    try:
        with open(p, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return None


def is_kinematic(ctx) -> bool:
    """Static vs kinematic (SPEC R7): truth_scalars.kinematic, else
    scenario_summary.kinematic, else assume kinematic (legacy partial dirs)."""
    sc = scalars(ctx)
    if "kinematic" in sc:
        try:
            return float(sc["kinematic"]) > 0.5
        except (TypeError, ValueError):
            pass
    ss = scenario_summary(ctx)
    if ss is not None and "kinematic" in ss:
        return bool(ss["kinematic"])
    return True


# --------------------------------------------------------------------------- #
#  Storm window
# --------------------------------------------------------------------------- #
def storm_window_utc(ctx):
    """``(t0, t1)`` storm-window datetimes (GPS scale), or ``(None, None)``.

    Reads ``storm_window_start/end_tow_s`` + ``week`` from ``truth_scalars.csv``.
    """
    sc = scalars(ctx)
    wk = sc.get("week")
    w0 = sc.get("storm_window_start_tow_s")
    w1 = sc.get("storm_window_end_tow_s")
    try:
        wk, w0, w1 = float(wk), float(w0), float(w1)
    except (TypeError, ValueError):
        return None, None
    if not (w1 > w0):
        return None, None
    ts = list(gps_to_datetime([wk, wk], [w0, w1]))   # Series/Index-agnostic
    return ts[0], ts[1]


def storm_vlines(ctx, *, label: str = "storm"):
    """Paired dashed :class:`VLine`s bounding the storm window (``[]`` if none).

    Draw these on every exam time-series figure (SPEC §4.2).  Only the first line
    carries the label so the legend isn't doubled.
    """
    t0, t1 = storm_window_utc(ctx)
    if t0 is None:
        return []
    return [VLine(x=t0, color=STORM_COLOR, style="--", width=1.0, label=label),
            VLine(x=t1, color=STORM_COLOR, style="--", width=1.0)]


# --------------------------------------------------------------------------- #
#  Slip classification (optional trailing ``source`` column, SPEC §4.2 / R4)
# --------------------------------------------------------------------------- #
def slip_class(delta_cycles, flagged, source: str = "") -> str:
    """One of flagged / unflagged / scripted / false-alarm.

    Priority: a Δ=0 event is a **false-alarm** (LLI-only, §4.5) regardless of
    origin; otherwise a ``source=='scripted'`` event is **scripted**; otherwise
    the LLI flag splits **flagged** vs **unflagged**.  Robust to a missing/empty
    ``source`` (the pre-SPEC_scripted_slips 7-column file).
    """
    try:
        d = int(round(float(delta_cycles)))
    except (TypeError, ValueError):
        d = 0
    if d == 0:
        return "false-alarm"
    if str(source).strip().lower() == "scripted":
        return "scripted"
    try:
        return "flagged" if float(flagged) > 0.5 else "unflagged"
    except (TypeError, ValueError):
        return "unflagged"


def classify_slips(df: pd.DataFrame) -> pd.Series:
    """Vectorized :func:`slip_class` over a ``truth_slips.csv`` frame."""
    src = df["source"].astype(str) if "source" in df.columns else pd.Series([""] * len(df), index=df.index)
    flg = df["flagged"] if "flagged" in df.columns else pd.Series([0] * len(df), index=df.index)
    return pd.Series(
        [slip_class(d, f, s) for d, f, s in zip(df["delta_cycles"], flg, src)],
        index=df.index)


# --------------------------------------------------------------------------- #
#  Verifier banner + chip row (SPEC §4.4) — trusted HTML for build_html_report
# --------------------------------------------------------------------------- #
def verifier_banner_html(rep: Optional[dict], report_path: str = "") -> str:
    """Coloured PASS/FAIL banner + compact per-check chip row from a
    ``verify_report`` dict (``""`` when the report is absent).

    Uses the ``.banner.pass/.fail`` CSS already in :mod:`gnssviz.report`; the
    chips carry inline styles (zero new CSS).  Each chip's tooltip shows the
    check value vs gate.
    """
    if not rep:
        return ""
    passed = bool(rep.get("pass"))
    nf = int(rep.get("failures", 0))
    cls = "pass" if passed else "fail"
    verdict = f"VERIFIER {'PASS' if passed else 'FAIL'} ({nf} failure{'' if nf == 1 else 's'})"
    tail = (f" <span style='font-weight:400'>&mdash; {html.escape(report_path)}</span>"
            if report_path else "")
    banner = f"<div class='banner {cls}'>{html.escape(verdict)}{tail}</div>"

    chips = []
    for c in rep.get("checks", []):
        ok = bool(c.get("pass"))
        color, bg = (("#1b5e20", "#e6f4ea") if ok else ("#9b1c14", "#fdecea"))
        cid = html.escape(str(c.get("id", "?")))
        val, gate = c.get("value"), c.get("gate")
        units = html.escape(str(c.get("units", "")))
        tip = ""
        if isinstance(val, (int, float)):
            tip = f"value {val:g}"
            if isinstance(gate, (int, float)):
                tip += f" / gate {gate:g}"
            if units:
                tip += f" {units}"
        chips.append(
            f"<span title='{html.escape(tip)}' style='display:inline-block;"
            f"padding:1px 8px;margin:2px;border-radius:10px;font-size:0.78rem;"
            f"background:{bg};color:{color}'>{cid}</span>")
    chip_row = f"<div style='margin:2px 0 14px'>{''.join(chips)}</div>" if chips else ""
    return banner + chip_row
