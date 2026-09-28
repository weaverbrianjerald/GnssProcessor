"""Shared helpers for the truth-referenced ``exam_*`` viewers.

The exam dashboard extends the pyviz suite with viewers that compare the real
``gnss_processor`` PPP solution against the **GnssSimulator answer key** (the
``truth_*.csv`` + ``scenario_summary.json`` the synthesizer writes, bound to a
:class:`~gnssviz.trace_paths.TraceContext` via ``--truth-dir``).  Everything the
exam viewers share lives here so the truth-join hygiene (SPEC R5) is written
once:

* **Truth↔trace join** on the integer ``(week, round(tow*1000))`` grid in
  ``int64``, never a float or string tow compare (:func:`merge_truth_trace`,
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

None of these import a plotting library; like every other data-layer helper,
they return prepared pandas / numpy / :mod:`gnssviz.figures` specs.
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
    "flagged":     (0.15, 0.45, 0.85),   # blue: LLI=1 (engine warned)
    "unflagged":   (0.85, 0.20, 0.20),   # red: LLI=0 (detectable only via MW/GF)
    "scripted":    (0.55, 0.30, 0.75),   # purple: hand-authored scenario slip
    "false-alarm": (0.55, 0.55, 0.55),   # grey: Δ=0, LLI=1 only
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
#  (week, tow-ms) join: the one truth↔trace matcher to use (SPEC R5)
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
    offset cancels because the ECEF difference is formed first.
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
#  Full position covariance -> horizontal (E,N) 2x2, and track-relative frames
# --------------------------------------------------------------------------- #
#: chi-square, 2 dof: 95% -> 5.991, 99% -> 9.210.  A 2-D confidence region is not
#: 1.96 sigma per axis; using the 1-D quantile understates the region.
CHI2_95 = 5.991
CHI2_99 = 9.210


def enu_cov2(df, lat0_deg, lon0_deg):
    """Per-epoch horizontal (E,N) 2x2 covariance from a state trace, or ``None``.

    Needs the off-diagonal position columns (``cov_xy_m2`` / ``cov_xz_m2`` /
    ``cov_yz_m2``, emitted since 2026-07-24).  Returns ``None`` when they are
    absent: an ellipse orientation cannot be recovered from variances alone, and
    rotating a diagonal-only ECEF covariance produces a bearing that comes from
    the rotation, not from the filter's correlation.  Callers draw nothing in
    that case.

    Rotated about one fixed geodetic point so the result is consistent with a
    plot drawn in that same frame; over a few hundred km the local ENU triad
    turns by ~(L/R_earth), i.e. ~2.7 deg at 300 km.
    """
    need = ("x_std_m", "y_std_m", "z_std_m", "cov_xy_m2", "cov_xz_m2", "cov_yz_m2")
    cols = {c: first_col(df, c) for c in need}
    if any(cols[c] is None for c in need):
        return None
    sx, sy, sz = (cols[c].to_numpy(float) for c in ("x_std_m", "y_std_m", "z_std_m"))
    cxy, cxz, cyz = (cols[c].to_numpy(float) for c in ("cov_xy_m2", "cov_xz_m2", "cov_yz_m2"))
    C = np.zeros((sx.size, 3, 3), float)
    C[:, 0, 0], C[:, 1, 1], C[:, 2, 2] = sx ** 2, sy ** 2, sz ** 2
    C[:, 0, 1] = C[:, 1, 0] = cxy
    C[:, 0, 2] = C[:, 2, 0] = cxz
    C[:, 1, 2] = C[:, 2, 1] = cyz
    la, lo = np.radians(lat0_deg), np.radians(lon0_deg)
    sl, cl, so, co = np.sin(la), np.cos(la), np.sin(lo), np.cos(lo)
    R = np.array([[-so, co, 0.0], [-sl * co, -sl * so, cl], [cl * co, cl * so, sl]])
    return np.einsum("ij,njk,lk->nil", R, C, R)[:, :2, :2]


def track_frame(e, n):
    """Unit along-track / cross-track vectors per epoch from a plan-view path."""
    de = np.gradient(np.asarray(e, float))
    dn = np.gradient(np.asarray(n, float))
    mag = np.hypot(de, dn)
    ok = mag > 0
    t = np.zeros((de.size, 2))
    t[ok, 0], t[ok, 1] = de[ok] / mag[ok], dn[ok] / mag[ok]
    t[~ok] = [1.0, 0.0]                       # stationary epoch: arbitrary but finite
    return t, np.stack([t[:, 1], -t[:, 0]], axis=1)


def proj_sigma(C2, v):
    """1-sigma of a stack of 2x2 covariances projected onto unit vectors ``v``."""
    return np.sqrt(np.clip(np.einsum("ni,nij,nj->n", v, C2, v), 0.0, None))


def exaggeration(bound_m, span_m, *, target=0.015, skip=0):
    """A 1/2/5-decade factor making the steady-state p90 bound ``target`` of span.

    Cm-level bounds on a multi-hundred-km track are invisible at true scale, so
    plan-view/geographic panels exaggerate them.  The factor is the smallest
    1/2/5 x 10^k at or above ``target * span_m / p90``, where p90 is taken over
    ``bound_m[skip:]`` (the steady state when ``skip`` drops the convergence
    transient) rather than the whole-arc median: a transient of metres against
    a sub-cm steady state would otherwise set the factor on its own.  Returns
    1.0 when the reference bound or the span is not positive.
    """
    b = np.asarray(bound_m, float)[skip:]
    b = b[np.isfinite(b)]
    ref = float(np.percentile(b, 90)) if b.size else 0.0
    if not (ref > 0) or not (span_m > 0):
        return 1.0
    raw = (target * span_m) / ref
    dec = 10.0 ** np.floor(np.log10(raw))
    return float(next((s * dec for s in (1, 2, 5) if s * dec >= raw), 10 * dec))


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


def converge_skip(ctx, default: int = 30) -> int:
    """Leading epochs the Verifier excludes from its steady-state statistics.

    Read from ``verify_report.json`` (the C++ ``Verifier`` echoes its own
    ``converge_skip`` there) so a dashboard statistic matches the graded one.
    Including the convergence transient makes a panel disagree with the
    scorecard next to it (on one run the first 30 epochs raised the ZWD RMS
    from 12 mm to 20 mm and the vertical RMS from 40 mm to 124 mm).  Falls
    back to ``default`` when the report or its value is missing or negative.
    """
    rep = verify_report(ctx)
    if isinstance(rep, dict):
        try:
            n = int(rep.get("converge_skip", default))
            if n >= 0:
                return n
        except (TypeError, ValueError):
            pass
    return default


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
#  Layout
# --------------------------------------------------------------------------- #
#: Per-satellite-row height (inches) for every sat x epoch strip/raster (the iono
#: raster, the slip timeline, the signal-usage strip).  Defined once so a heatmap
#: cell (a "tile") has the same height in every such figure on the dashboard.
SAT_ROW_IN = 0.34


def sat_strip_height(n_rows, *, base: float = 1.8, lo: float = 4.0, hi: float = 12.0) -> float:
    """Figure height (inches) for a satellite-row strip of ``n_rows`` rows.

    Uses the shared :data:`SAT_ROW_IN` per-row height so tiles are consistent across
    figures, clamped to ``[lo, hi]`` so a 3-satellite strip is still legible and a
    32-satellite one does not run off the page.
    """
    return float(min(hi, max(lo, SAT_ROW_IN * float(n_rows) + base)))


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


#: Fraction of epochs above which a storm-window overlay is pure occlusion.
STORM_WHOLE_ARC_FRAC = 0.95


def storm_overlay_is_informative(mask, *, frac: float = STORM_WHOLE_ARC_FRAC) -> bool:
    """``False`` when the storm window covers essentially the whole record.

    The *spatial* viewers mark the storm by recolouring the affected track
    segment, which only carries information if there is unaffected track to
    contrast it against.  On a whole-arc storm (e.g. the DOY-132 scenario, 100%
    of epochs) the overlay is drawn thicker than the track it covers, so it hides
    the trajectory and leaves a legend entry for a colour that is not visible.
    Callers skip the overlay and say "whole arc" in prose instead.  Time-series
    viewers are unaffected: their paired :func:`storm_vlines` fall at the plot
    edges.  Returns ``False`` also for an empty or all-false mask.
    """
    m = np.asarray(mask, dtype=bool)
    if m.size == 0 or not m.any():
        return False
    return float(m.mean()) < frac


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
#  Verifier banner + chip row (SPEC §4.4): trusted HTML for build_html_report
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
