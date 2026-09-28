"""N-run comparison for PPP-RTK network legs ("reference vs guarded vs arm").

:mod:`gnssviz.compare` answers "how did run **B** differ from run **A**?" for a
*pair*.  A validation card is usually graded over a **family** of legs, for
example the DOY-129 reference day, a frame-guards leg that re-ran the same window
with the guard set armed, and the s2z realization arm.  This module answers
"which leg withheld what, when, and where did the misfit sit?" in one report
instead of three pair-wise ones.

A :class:`RunSet` is an ordered list of :class:`RunLeg` (label + a
:class:`~gnssviz.trace_paths.TraceContext`); **leg 0 is the reference** and every
ratio / agreement statistic is quoted against it.  Each *run comparator* is a
plain ``fn(runs) -> ViewerResult``, the same return type the report assembler
already consumes, so the N-run report reuses
:func:`gnssviz.report.write_html_report` unchanged.

What it draws
-------------
``disposition``
    One :class:`~gnssviz.figures.Raster` row per leg over the **union** epoch
    axis, cells coloured by the producer's disposition vocabulary (``emitted`` /
    ``*_withheld`` / ``*_failed`` / ``sync_skip``), with a NaN (transparent) cell
    wherever a leg has no row for that epoch, so differing coverage reads as a
    gap rather than as a dropped sample.  Plus a cumulative-emitted curve per
    leg, and an agreement table listing the first epochs where a leg's
    disposition diverges from the reference's.

``tracks``
    σ̂0 (adaptation), σ̂0_pf (post-fit code robust scale) and the SH-VTEC a₀₀
    tracks overlaid across legs, with each leg's **withheld epochs shaded in its
    own colour**, each leg's **a₀₀ out-of-band epochs shaded** on the a₀₀ axes,
    and vertical lines at the structural frame events (:data:`FRAME_EVENTS`:
    CB-excursion trips, code-datum reseeds, pivot handovers, clock-basis changes,
    SH-gate engage/release).  The σ̂0 trigger / withhold-ceiling **config echoes
    are compared across legs** and a divergence is called out, because legs
    graded against different ceilings are not comparable.

``events``
    Per-tier event counts and **per-epoch rates** across legs with the ratio
    against the reference (>3x or new is called out).  Rates, not raw counts, are
    the comparable quantity, because legs often span different epoch counts.

``keyed_world``
    The RHS geography read.  ``network_pf_keyed.csv`` STA pools are reduced to a
    robust-z of ``s0_code`` at an onset epoch against **each key's own pre-onset
    baseline** (median / 1.4826·MAD, the same statistic
    ``scripts/p2_gate_verdict.py`` §4.3 prints, so the viewer and the verdict
    script agree), then joined to the ``network_stations.csv`` roster
    and painted on a per-leg worldmap through the diverging ``vik`` map.  Roster
    stations with no keyed pool stay grey, and the unjoined-key count is reported,
    so a join that drops keys is visible rather than reading as a clean geography.

``scorecard``
    Every other comparator's summary in one monospaced panel.

Not in the viewers ``REGISTRY``
-------------------------------
Like :data:`gnssviz.compare.COMPARATORS`, these take a :class:`RunSet`, not a
single ``TraceContext``; the single-ctx driver would call them with the wrong
argument.  They live in :data:`RUN_COMPARATORS` and are driven by
``run_compare_n.py``, and this module is intentionally **not** imported from
``gnssviz/viewers/__init__.py``.

Colours
-------
Leg 0 (the reference) is a dark neutral; the remaining legs walk the shared
:func:`~gnssviz.palettes.distinguishable_colors` sequence.  The keyed-z map uses
``vik`` (diverging, signed z) and the disposition raster uses the fixed
per-disposition palette the single-run
:mod:`~gnssviz.viewers.network_epoch_status` viewer defines, not a
hand-rolled ramp.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np
import pandas as pd

from ..colormaps import strip_colormap
from ..figures import (Axes, Band, ColorBar, Figure, HLine, Legend, Line,
                       Raster, Scatter, TextBox, VLine)
from ..io import GPS_EPOCH, add_utc, load_trace, require_cols
from ..network_config import (ROLE_DATUM, ROLE_HELD_OUT, Station, load_stations)
from ..palettes import distinguishable_colors
from ..trace_paths import TraceContext
from .base import ViewerResult, ViewerSkip
# The disposition vocabulary + palette are the producer's, and the single-run
# viewer already owns them; extending that tuple here (rather than re-typing it)
# keeps one table for the six original lanes.  ``sh_gate_withheld`` is the lane
# of the SH health gate (SPEC_day129_frame_guards Phase-2.2).
from .network_epoch_status import _DISP_COLOR as _ES_DISP_COLOR
from .network_epoch_status import _DISPOSITIONS as _ES_DISPOSITIONS
# One source for the map auto-framing rule (regional cluster -> padded bbox,
# global network -> whole world) and its figure-height companion.
from .network_map import _auto_extent, _height_for

# --------------------------------------------------------------------------- #
#  Trace names
# --------------------------------------------------------------------------- #
CSV_EPOCH_STATUS = "network_epoch_status.csv"
CSV_EVENTS = "network_events.csv"
CSV_SH_HEALTH = "network_vtec_sh_health.csv"
CSV_PF_KEYED = "network_pf_keyed.csv"
CSV_STATIONS = "network_stations.csv"

_SEC_PER_WEEK = 604800.0

# --------------------------------------------------------------------------- #
#  Disposition vocabulary (extends the single-run viewer's table)
# --------------------------------------------------------------------------- #
OTHER_LANE = "other"
DISPOSITIONS: Tuple[str, ...] = tuple(_ES_DISPOSITIONS) + ("sh_gate_withheld",)
_LANES: Tuple[str, ...] = DISPOSITIONS + (OTHER_LANE,)
_OTHER_COLOR = (0.75, 0.35, 0.75)
_LANE_COLOR: Dict[str, tuple] = dict(_ES_DISP_COLOR)
_LANE_COLOR["sh_gate_withheld"] = (0.55, 0.30, 0.65)   # purple: the SH health gate
_LANE_COLOR[OTHER_LANE] = _OTHER_COLOR
#: 1-based class codes.  The categorical colorbar styler ticks 1..K when no
#: explicit ticks are given, so a 1-based raster code lines the swatch up with
#: its label without a second tick table.
_LANE_CODE: Dict[str, int] = {d: i + 1 for i, d in enumerate(_LANES)}
_LANE_CMAP = np.array([_LANE_COLOR[d] for d in _LANES], dtype=float)

# --------------------------------------------------------------------------- #
#  Palette (the ``vik`` anchors gnssviz.compare uses, so A/B and A/B/C read alike)
# --------------------------------------------------------------------------- #
C_REFERENCE = (0.20, 0.20, 0.20)     # leg 0: dark neutral (the baseline)
C_GRID = (0.60, 0.60, 0.60)
C_TRIGGER = (0.85, 0.55, 0.10)
C_CEILING = (0.85, 0.16, 0.16)
C_ROSTER_ONLY = (0.62, 0.66, 0.70)   # roster station with no keyed pool
C_DATUM = (0.96, 0.70, 0.10)
C_HELD_OUT = (0.86, 0.16, 0.16)

#: (tier, event-name or ``None``) selectors drawn as vertical event lines on the
#: ``tracks`` figure.  The set is narrow on purpose: these are the *structural
#: frame* events (a handful per run), not the per-epoch QC events (``T0_mw_detect``
#: / ``T1_if_slip`` run to thousands of rows and belong in the ``events`` table).
FRAME_EVENTS: Tuple[Tuple[str, Optional[str]], ...] = (
    ("CB_EXC", None),        # code-bias excursion guard: trip / disc landed
    ("CB_DATUM", None),      # code-datum reseed / pin change
    ("PIVOT", None),         # pivot handover / reselect
    ("CLK_BASIS", None),     # clock-basis change
    ("SH_GATE", "engage"),   # SH health gate engage ...
    ("SH_GATE", "release"),  # ... and release (the per-epoch 'withhold' rows are
                             #     shown by the disposition raster, not as lines)
)


# --------------------------------------------------------------------------- #
#  RunLeg / RunSet
# --------------------------------------------------------------------------- #
def auto_label(log_dir: str) -> str:
    """Short label for a trace folder: ``runs/day129_guards/leg01/logs`` -> ``leg01``.

    A run's traces usually sit in a ``logs`` subfolder of the run dir, so
    the run dir's basename is the name a reader recognises; anything else labels
    by its own basename.
    """
    p = os.path.abspath(log_dir).rstrip(os.sep)
    base = os.path.basename(p)
    if base.lower() in ("logs", "log", "traces"):
        parent = os.path.basename(os.path.dirname(p))
        if parent:
            return parent
    return base or p


@dataclass
class RunLeg:
    """One run in the comparison: a label, its traces, and its plot colour."""
    label: str
    ctx: TraceContext
    color: tuple = C_REFERENCE

    def has(self, csv_name: str) -> bool:
        """``True`` iff this leg carries at least one data row of ``csv_name``."""
        return self.ctx.has_data(csv_name)

    @property
    def log_dir(self) -> str:
        return self.ctx.log_dir


@dataclass
class RunSet:
    """An ordered family of runs; ``legs[0]`` is the reference every ratio uses."""
    legs: List[RunLeg] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.legs:
            raise ValueError("RunSet needs at least one leg")
        for leg, color in zip(self.legs, leg_colors(len(self.legs))):
            leg.color = color

    # -- construction ------------------------------------------------------ #
    @classmethod
    def from_dirs(cls, dirs: Sequence[str], labels: Optional[Sequence[str]] = None,
                  **ctx_kwargs) -> "RunSet":
        """Build a :class:`RunSet` from trace folders (labels default per :func:`auto_label`).

        ``ctx_kwargs`` (e.g. ``network_config=...``) are forwarded to every
        :class:`~gnssviz.trace_paths.TraceContext`; duplicate auto-labels are
        disambiguated with a ``#2`` suffix so two ``logs`` folders never collide.
        """
        if labels is not None and len(labels) != len(dirs):
            raise ValueError(f"{len(labels)} label(s) for {len(dirs)} dir(s)")
        out: List[RunLeg] = []
        seen: Dict[str, int] = {}
        for i, d in enumerate(dirs):
            lab = labels[i] if labels is not None else auto_label(d)
            seen[lab] = seen.get(lab, 0) + 1
            if seen[lab] > 1:
                lab = f"{lab}#{seen[lab]}"
            out.append(RunLeg(label=lab, ctx=TraceContext(d, **ctx_kwargs)))
        return cls(legs=out)

    # -- queries ----------------------------------------------------------- #
    @property
    def reference(self) -> RunLeg:
        return self.legs[0]

    @property
    def labels(self) -> List[str]:
        return [leg.label for leg in self.legs]

    def having(self, csv_name: str) -> List[RunLeg]:
        """The legs carrying ``csv_name`` data, in set order."""
        return [leg for leg in self.legs if leg.has(csv_name)]

    def require(self, csv_name: str, minimum: int = 1) -> List[RunLeg]:
        """Legs carrying ``csv_name``; :class:`ViewerSkip` when fewer than ``minimum``.

        The skip reason names the legs that lack it ("absent on leg01" rather
        than a bare "skipped").
        """
        have = self.having(csv_name)
        if len(have) < minimum:
            miss = [leg.label for leg in self.legs if leg not in have]
            raise ViewerSkip(
                f"{csv_name}: {len(have)} leg(s) carry data, need {minimum}"
                + (f" (absent/empty on: {', '.join(miss)})" if miss else ""))
        return have


def leg_colors(n: int) -> List[tuple]:
    """Per-leg colours: leg 0 dark neutral, the rest a distinguishable walk."""
    if n <= 0:
        return []
    return [C_REFERENCE] + list(distinguishable_colors(max(n - 1, 0)))


# --------------------------------------------------------------------------- #
#  Small helpers
# --------------------------------------------------------------------------- #
@dataclass
class _Loaded:
    """One leg's per-epoch frame plus the malformed-row count that was dropped."""
    leg: RunLeg
    df: pd.DataFrame
    dropped: int


def _load(leg: RunLeg, csv_name: str, cols: Sequence[str]) -> _Loaded:
    """Load a per-epoch trace, key it by GPS seconds, drop unusable rows.

    A run that is still executing (or was killed) leaves a **partial final line**
    in every open trace; pandas reads it as a row with a non-finite ``week`` /
    ``tow``.  Those rows are dropped here and the count is carried through to the
    summary, since an unreported drop would understate a leg's epoch coverage.
    """
    df = load_trace(leg.ctx.path(csv_name))
    require_cols(df, cols, f"{csv_name} ({leg.label})")
    n_raw = len(df)
    wk = pd.to_numeric(df["week"], errors="coerce").to_numpy(float)
    tow = pd.to_numeric(df["tow"], errors="coerce").to_numpy(float)
    keep = np.isfinite(wk) & np.isfinite(tow)
    df = df.loc[keep].copy()
    df["week"] = wk[keep]
    df["tow"] = tow[keep]
    # Millisecond-quantized GPS seconds: the same join key gnssviz.compare's
    # align_epochs uses, widened across the week rollover.
    df["_gsec"] = np.round(df["week"] * _SEC_PER_WEEK + df["tow"], 3)
    if df.empty:
        raise ViewerSkip(f"{csv_name} ({leg.label}) has no usable rows "
                         f"({n_raw} row(s) all lacked a finite week/tow)")
    df = add_utc(df, sort=True)
    return _Loaded(leg=leg, df=df, dropped=n_raw - len(df))


def _gsec_to_utc(gsec) -> np.ndarray:
    """GPS seconds -> the trace timestamps (GPS scale, matching :func:`add_utc`)."""
    return (GPS_EPOCH + pd.to_timedelta(np.asarray(gsec, dtype=float), unit="s")
            ).to_numpy("datetime64[ns]")


def _fmt_time(ts) -> str:
    try:
        return pd.Timestamp(ts).strftime("%H:%M:%S")
    except (ValueError, TypeError):
        return "??:??:??"


def _finite(v) -> np.ndarray:
    v = np.asarray(v, dtype=float)
    return v[np.isfinite(v)]


def _num(df: pd.DataFrame, col: str) -> np.ndarray:
    """Column as float (``NaN`` when absent): schema-tolerant column access."""
    if col not in df.columns:
        return np.full(len(df), np.nan)
    return pd.to_numeric(df[col], errors="coerce").to_numpy(float)


def _text(df: pd.DataFrame, col: str) -> np.ndarray:
    if col not in df.columns:
        return np.full(len(df), "", dtype=object)
    return df[col].astype("string").fillna("").to_numpy(dtype=object)


def _pos(y, floor: float = 1e-12) -> np.ndarray:
    """Clamp non-positive values so a log axis keeps them (never drops a zero)."""
    out = np.asarray(y, dtype=float).copy()
    out[~(out > 0)] = floor
    return out


def _class_of(disposition: str) -> str:
    """Coarse disposition class: ``emitted`` / ``withheld`` / ``failed`` / ``other``."""
    d = str(disposition)
    if d == "emitted":
        return "emitted"
    if "withheld" in d:
        return "withheld"
    if "failed" in d or d == "sync_skip":
        return "failed"
    return "other"


def _ylim_for(series: Sequence[np.ndarray], pad: float = 0.06) -> Optional[Tuple[float, float]]:
    """Padded (lo, hi) across every finite sample, or ``None`` when there are none.

    Shading a withheld stretch needs a band with *numeric* extents, and
    ``fill_between`` participates in autoscale, so the axes limits are pinned
    here and the band is drawn to them, instead of using a large fixed number
    that would distort the y-scale.
    """
    vals = np.concatenate([_finite(s) for s in series]) if series else np.array([])
    if vals.size == 0:
        return None
    lo, hi = float(np.min(vals)), float(np.max(vals))
    if not np.isfinite(lo) or not np.isfinite(hi):
        return None
    if hi <= lo:
        span = max(abs(hi), 1.0) * 0.1
        return (lo - span, hi + span)
    span = hi - lo
    return (lo - pad * span, hi + pad * span)


def _mask_band(t: np.ndarray, mask: np.ndarray, ylim: Tuple[float, float],
               color, label: Optional[str]) -> Band:
    """A vertical shade over the epochs where ``mask`` is set (NaN breaks the fill)."""
    lo = np.where(mask, ylim[0], np.nan)
    hi = np.where(mask, ylim[1], np.nan)
    return Band(x=t, lo=lo, hi=hi, color=color, alpha=0.12, label=label,
                show_in_legend=label is not None)


def _table(header: Sequence[str], rows: Sequence[Sequence[str]],
           widths: Sequence[int]) -> str:
    """A monospaced fixed-width table (first column left-aligned, rest right)."""
    def line(cells):
        out = []
        for i, (c, w) in enumerate(zip(cells, widths)):
            out.append(f"{str(c):<{w}}" if i == 0 else f"{str(c):>{w}}")
        return "".join(out)
    body = [line(header), "-" * int(sum(widths))]
    body += [line(r) for r in rows]
    return "\n".join(body)


def _text_axes(text: str, title: str = "", fontsize: float = 8.5,
               boxed: bool = False) -> Axes:
    return Axes(title=title, axis_off=True, x_is_datetime=False, grid=False,
                traces=[TextBox(x=0.01, y=0.99, text=text, ha="left", va="top",
                                mono=True, boxed=boxed, fontsize=fontsize)])


# --------------------------------------------------------------------------- #
#  (a) disposition timelines
# --------------------------------------------------------------------------- #
_ES_COLS = ("week", "tow", "disposition")


def compare_dispositions(runs: RunSet, *, max_divergences: int = 12) -> ViewerResult:
    """N disposition timelines on one union epoch axis + agreement vs the reference."""
    legs = runs.require(CSV_EPOCH_STATUS, minimum=1)
    loaded = [_load(leg, CSV_EPOCH_STATUS, _ES_COLS) for leg in legs]

    # Column axis.  A Raster spreads its columns uniformly across the time span,
    # so the columns must form a regular time grid or the tick labels are wrong:
    # two legs covering disjoint windows (a seed and its resume leg) would
    # otherwise render as one continuous block with each leg's start hours off.
    gs_union = np.unique(np.concatenate([L.df["_gsec"].to_numpy(float) for L in loaded]))
    gs_cols, step, grid_note = _epoch_grid(gs_union)
    t_union = _gsec_to_utc(gs_cols)

    grid = np.full((len(loaded), gs_cols.size), np.nan)
    disp_of: List[Dict[float, str]] = []
    n_unplaced = 0
    for r, L in enumerate(loaded):
        disp = _text(L.df, "disposition")
        gs = L.df["_gsec"].to_numpy(float)
        cols, ok = _snap(gs, gs_cols, step)
        n_unplaced += int((~ok).sum())
        codes = np.array([_LANE_CODE.get(str(d), _LANE_CODE[OTHER_LANE]) for d in disp],
                         dtype=float)
        grid[r, cols[ok]] = codes[ok]
        disp_of.append({float(g): str(d) for g, d in zip(gs, disp)})
    if n_unplaced:
        grid_note = ((grid_note + "; ") if grid_note else "") + (
            f"{n_unplaced} epoch(s) fell off the column grid and are NOT painted "
            f"(off-cadence rows); they still count in every table below")

    ax_raster = Axes(
        title="Per-epoch disposition (row = leg; blank = leg has no row for that epoch)",
        ylabel="", xlabel="", grid=False, x_is_datetime=True,
        traces=[Raster(x=t_union, row_labels=[L.leg.label for L in loaded], grid=grid)])

    # Cumulative emitted: where a leg stops contributing product, and by how much.
    ax_cum = Axes(title="Cumulative emitted epochs", ylabel="emitted (cum.)",
                  xlabel="GPS Time", legend=Legend(loc="upper left"))
    cum_traces = []
    for L in loaded:
        emitted = (_text(L.df, "disposition") == "emitted")
        cum_traces.append(Line(
            x=L.df["utc"].to_numpy("datetime64[ns]"), y=np.cumsum(emitted.astype(float)),
            color=L.leg.color, width=1.3,
            label=f"{L.leg.label} ({int(emitted.sum())}/{len(L.df)})"))
    ax_cum.traces = cum_traces

    fig = Figure(name="N-run dispositions",
                 title=f"Epoch dispositions across {len(loaded)} run(s)",
                 axes=[ax_raster, ax_cum], width=13.0,
                 height=max(6.0, 0.45 * len(loaded) + 5.0), link_x=True,
                 cmap=_LANE_CMAP, clim=(0.5, len(_LANES) + 0.5),
                 colorbar=ColorBar(label="", tick_labels=list(_LANES)),
                 categorical=True)

    return ViewerResult(figures=[fig],
                        summary=_disposition_summary(loaded, gs_union, gs_cols, step,
                                                     disp_of, max_divergences,
                                                     grid_note))


def _epoch_grid(gs_union: np.ndarray, max_cols: int = 20000
                ) -> Tuple[np.ndarray, float, str]:
    """A regular GPS-second column grid spanning the union, at its modal cadence.

    Returns ``(columns, step, note)``.  Falls back to the distinct-epoch axis,
    with the reason stated in ``note``, when a regular grid would need more
    than ``max_cols`` columns (legs days apart at a 1 s cadence).
    """
    if gs_union.size < 2:
        return gs_union, 0.0, ""
    d = np.diff(gs_union)
    pos = d[d > 0]
    step = float(np.median(pos)) if pos.size else 0.0
    if step <= 0:
        return gs_union, 0.0, ""
    n = int(round((gs_union[-1] - gs_union[0]) / step)) + 1
    if n > max_cols:
        return (gs_union, step,
                f"column axis: fell back to {gs_union.size} distinct-epoch columns "
                f"(a regular {step:g} s grid would need {n} > {max_cols}); "
                f"columns are then uniform in epoch INDEX, not in time")
    return gs_union[0] + step * np.arange(n, dtype=float), step, ""


def _snap(gs: np.ndarray, gs_cols: np.ndarray, step: float
          ) -> Tuple[np.ndarray, np.ndarray]:
    """Map epochs onto column indices; ``ok`` is False where none is within step/2."""
    if step <= 0 or gs_cols.size < 2:
        pos = {float(g): i for i, g in enumerate(gs_cols)}
        cols = np.array([pos.get(float(g), 0) for g in gs], dtype=int)
        ok = np.array([float(g) in pos for g in gs], dtype=bool)
        return cols, ok
    cols = np.rint((gs - gs_cols[0]) / step).astype(int)
    ok = (cols >= 0) & (cols < gs_cols.size)
    safe = np.clip(cols, 0, gs_cols.size - 1)
    ok &= np.abs(gs - gs_cols[safe]) <= 0.5 * step
    return safe, ok


def _disposition_summary(loaded: List[_Loaded], gs_union: np.ndarray,
                         gs_cols: np.ndarray, step: float,
                         disp_of: List[Dict[float, str]],
                         max_divergences: int, grid_note: str) -> str:
    out = [f"=== N-Run Epoch Dispositions ({len(loaded)} legs) ==="]
    for L in loaded:
        out.append(f"  {L.leg.label:<14} {L.leg.log_dir}")
    out.append("")

    widths = (16, 9, 9, 9, 10, 9, 8, 10)
    rows = []
    for L in loaded:
        disp = _text(L.df, "disposition")
        cls = np.array([_class_of(d) for d in disp])
        n = len(L.df)
        n_emit = int((cls == "emitted").sum())
        rows.append([L.leg.label, n, n_emit, f"{100.0 * n_emit / max(n, 1):.1f}%",
                     int((cls == "withheld").sum()), int((cls == "failed").sum()),
                     int((cls == "other").sum()), L.dropped])
    out.append(_table(("leg", "epochs", "emitted", "%emit", "withheld", "failed",
                       "other", "badrows"), rows, widths))

    # Coverage: how comparable the legs even are before any value is differenced.
    out.append("")
    out.append(f"Union epoch axis : {gs_union.size} distinct epochs over "
               f"{gs_cols.size} column(s)"
               + (f" on a regular {step:g} s grid" if step > 0 else ""))
    if grid_note:
        out.append(f"                   {grid_note}")
    ref = loaded[0]
    ref_set = set(ref.df["_gsec"].to_numpy(float).tolist())
    for L in loaded[1:]:
        s = set(L.df["_gsec"].to_numpy(float).tolist())
        out.append(f"  {L.leg.label:<14} shared {len(ref_set & s)}, "
                   f"only-{ref.leg.label} {len(ref_set - s)}, only-{L.leg.label} "
                   f"{len(s - ref_set)}")

    # Per-disposition counts, fixed vocabulary order then anything unrecognised.
    out.append("")
    seen = set()
    for L in loaded:
        seen |= set(str(d) for d in _text(L.df, "disposition"))
    lanes = [d for d in DISPOSITIONS if d in seen]
    extra = sorted(x for x in seen if x not in DISPOSITIONS)
    if lanes or extra:
        w = [22] + [max(9, len(L.leg.label) + 2) for L in loaded]
        drows = []
        for d in lanes + extra:
            drows.append([d + ("  (unrecognised)" if d in extra else "")]
                         + [int((_text(L.df, "disposition") == d).sum()) for L in loaded])
        out.append(_table(["disposition"] + [L.leg.label for L in loaded], drows, w))

    # Agreement vs the reference on the shared epochs (the A/B/C read).
    out.append("")
    out.append(f"Disposition agreement vs '{ref.leg.label}':")
    if len(loaded) == 1:
        out.append("  (single leg -- nothing to compare)")
        return "\n".join(out)
    ref_map = disp_of[0]
    for L, dmap in zip(loaded[1:], disp_of[1:]):
        shared = [g for g in dmap if g in ref_map]
        diff = [g for g in shared if dmap[g] != ref_map[g]]
        pct = 100.0 * (len(shared) - len(diff)) / max(len(shared), 1)
        out.append(f"  {L.leg.label:<14} {len(shared)} shared, "
                   f"{len(shared) - len(diff)} agree ({pct:.1f}%), {len(diff)} differ")
        for g in sorted(diff)[:max_divergences]:
            t = _fmt_time(_gsec_to_utc([g])[0])
            out.append(f"      tow {g % _SEC_PER_WEEK:9.0f} ({t})  "
                       f"{ref.leg.label}={ref_map[g]}  ->  {L.leg.label}={dmap[g]}")
        if len(diff) > max_divergences:
            out.append(f"      ... {len(diff) - max_divergences} further "
                       f"divergence(s) not listed")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
#  (b) sigma0 / a00 tracks with event + withhold shading
# --------------------------------------------------------------------------- #
_SH_COLS = ("week", "tow", "a00_tecu")
_EV_COLS = ("week", "tow", "tier", "event")


def compare_tracks(runs: RunSet, *,
                   event_selectors: Sequence[Tuple[str, Optional[str]]] = FRAME_EVENTS,
                   max_event_lines: int = 60,
                   shade_withheld: bool = True) -> ViewerResult:
    """σ̂0 / σ̂0_pf / a₀₀ overlaid across legs, shaded by each leg's withheld epochs."""
    es_legs = runs.having(CSV_EPOCH_STATUS)
    sh_legs = runs.having(CSV_SH_HEALTH)
    if not es_legs and not sh_legs:
        raise ViewerSkip(f"neither {CSV_EPOCH_STATUS} nor {CSV_SH_HEALTH} "
                         f"present on any leg")
    es = [_load(leg, CSV_EPOCH_STATUS, _ES_COLS) for leg in es_legs]
    sh = [_load(leg, CSV_SH_HEALTH, _SH_COLS) for leg in sh_legs]

    # Per-leg withheld mask (epoch_status is the only trace that carries it), so
    # the a00 axes can shade with the same mask even though it plots SH rows.
    withheld: Dict[str, Tuple[np.ndarray, np.ndarray]] = {}
    for L in es:
        cls = np.array([_class_of(d) for d in _text(L.df, "disposition")])
        withheld[L.leg.label] = (L.df["_gsec"].to_numpy(float), cls != "emitted")

    axes: List[Axes] = []
    notes: List[str] = []

    # -- axes 1: sigma0 (adaptation) + config echoes + frame-event lines ------ #
    if es:
        # The gate levels are folded into the y-limit before the axes is built:
        # this axis shows how far σ̂0 sits below the ceiling, and a reference line
        # outside the limits also collapses constrained layout (its label is an
        # artist at that off-scale y).
        trig, ceil, echo_note = _config_echoes(es)
        levels = [v for v in (trig, ceil) if np.isfinite(v) and v > 0]
        ax = _overlay_axes(es, "sigma0", title="Robust variance factor σ̂0 "
                                               "(adaptation)", ylabel="σ̂0",
                           withheld=withheld if shade_withheld else None,
                           extra_levels=levels)
        if np.isfinite(trig):
            ax.traces.append(HLine(y=trig, color=C_TRIGGER, style="--",
                                   label=f"trigger ({trig:.2f})"))
        if np.isfinite(ceil) and ceil > 0:
            ax.traces.append(HLine(y=ceil, color=C_CEILING, style="-",
                                   label=f"withhold ceiling ({ceil:.2f})"))
        if echo_note:
            notes.append(echo_note)
        ev_lines, ev_note = _event_lines(runs, event_selectors, max_event_lines)
        ax.traces.extend(ev_lines)
        notes.append(ev_note)
        axes.append(ax)

    # -- axes 2: post-fit robust scale (the RHS-relevant pool) ---------------- #
    if es:
        pf_col = next((c for c in ("sigma0_pf_code", "sigma0_pf")
                       if any(c in L.df.columns for L in es)), None)
        if pf_col is not None:
            axes.append(_overlay_axes(
                es, pf_col,
                title=f"Post-fit robust scale ({pf_col}) — the RHS misfit track",
                ylabel=pf_col,
                withheld=withheld if shade_withheld else None))
        else:
            notes.append("post-fit σ̂0 track: no sigma0_pf/sigma0_pf_code column "
                         "on any leg (older schema) — axis omitted")

    # -- axes 3: SH-VTEC a00 + out-of-band shading ---------------------------- #
    if sh:
        ax_a00 = _overlay_axes(sh, "a00_tecu", title="SH-VTEC a₀₀ (mean VTEC)",
                               ylabel="a₀₀ (TECU)", withheld=None)
        # Count first, shade second: the count is the reported fact and must not
        # depend on whether a y-limit could be computed (an all-NaN a₀₀ column
        # yields no limit, and "0 flagged" would then be wrong).
        lim = ax_a00.ylim
        has_flag = any("a00_in_band" in L.df.columns for L in sh)
        n_oob_total = sum(int((_num(L.df, "a00_in_band") == 0.0).sum()) for L in sh)
        for L in sh:
            oob = _num(L.df, "a00_in_band") == 0.0
            if lim is not None and oob.any():
                ax_a00.traces.insert(0, _mask_band(
                    L.df["utc"].to_numpy("datetime64[ns]"), oob, lim,
                    L.leg.color, f"{L.leg.label} a₀₀ out-of-band ({int(oob.sum())})"))
        if has_flag and n_oob_total == 0:
            notes.append("a₀₀ out-of-band shading: 0 epochs flagged on any leg "
                         "(a00_in_band == 1 throughout)")
        elif n_oob_total and lim is None:
            notes.append(f"a₀₀ out-of-band shading: {n_oob_total} epoch(s) flagged "
                         f"but NOT shaded (a₀₀ carries no finite value to scale to)")
        axes.append(ax_a00)

    if not axes:
        raise ViewerSkip("no plottable track columns on any leg")
    axes[-1].xlabel = "GPS Time"

    fig = Figure(name="N-run tracks",
                 title="σ̂0 / a₀₀ tracks across runs "
                       "(shaded = that leg's withheld epochs)",
                 axes=axes, width=13.0, height=max(6.0, 3.2 * len(axes)),
                 link_x=True)
    return ViewerResult(figures=[fig], summary=_tracks_summary(es, sh, notes))


def _overlay_axes(loaded: List[_Loaded], col: str, *, title: str, ylabel: str,
                  withheld: Optional[Dict[str, Tuple[np.ndarray, np.ndarray]]],
                  extra_levels: Sequence[float] = ()) -> Axes:
    """One Line per leg over ``col`` + optional per-leg withheld shading.

    ``extra_levels`` are reference levels (gate thresholds) the y-limit must
    contain; the limit is pinned explicitly because the withheld shading is a
    :class:`~gnssviz.figures.Band` drawn to those exact extents.
    """
    ax = Axes(title=title, ylabel=ylabel, xlabel="", legend=Legend(loc="best"))
    series = [_num(L.df, col) for L in loaded]
    ax.ylim = _ylim_for(series + [np.asarray([v], dtype=float)
                                  for v in extra_levels if np.isfinite(v)])
    traces: List = []
    if withheld is not None and ax.ylim is not None:
        for L in loaded:
            gs_w, mask_w = withheld.get(L.leg.label, (None, None))
            if gs_w is None:
                continue
            # Map the withheld mask onto this frame's epochs (the SH trace has a
            # row only on epochs that reached the SH stage, so a positional zip
            # would mis-shade).
            wmap = dict(zip(gs_w.tolist(), mask_w.tolist()))
            m = np.array([bool(wmap.get(float(g), False))
                          for g in L.df["_gsec"].to_numpy(float)])
            if m.any():
                traces.append(_mask_band(
                    L.df["utc"].to_numpy("datetime64[ns]"), m, ax.ylim, L.leg.color,
                    f"{L.leg.label} withheld ({int(m.sum())})"))
    for L, y in zip(loaded, series):
        traces.append(Line(x=L.df["utc"].to_numpy("datetime64[ns]"), y=y,
                           color=L.leg.color, width=1.2, label=L.leg.label))
    ax.traces = traces
    return ax


def _config_echoes(es: List[_Loaded]) -> Tuple[float, float, str]:
    """Reference trigger/ceiling + a call-out when the legs disagree.

    Two legs graded against different σ̂0 gates are not comparable on withhold
    counts; that has to surface as text, not be silently averaged away.
    """
    def first(L, col):
        v = _finite(_num(L.df, col))
        return float(v[0]) if v.size else float("nan")
    trig = [first(L, "sigma0_trigger") for L in es]
    ceil = [first(L, "withhold_ceiling") for L in es]
    note = ""
    def spread(vals):
        f = [v for v in vals if np.isfinite(v)]
        return len({round(v, 6) for v in f}) > 1
    if spread(trig) or spread(ceil):
        parts = [f"{L.leg.label}(trig {t:.3f}/ceil {c:.3f})"
                 for L, t, c in zip(es, trig, ceil)]
        note = ("CONFIG ECHO DIVERGENCE — legs were graded against different σ̂0 "
                "gates: " + ", ".join(parts))
    return (trig[0] if trig else float("nan"),
            ceil[0] if ceil else float("nan"), note)


def _event_lines(runs: RunSet, selectors: Sequence[Tuple[str, Optional[str]]],
                 max_lines: int) -> Tuple[List[VLine], str]:
    """Vertical lines at the structural frame events, one legend entry per (leg, tier)."""
    legs = runs.having(CSV_EVENTS)
    if not legs:
        return ([], "frame-event lines: no network_events.csv on any leg")
    picked: List[Tuple[RunLeg, str, float]] = []
    per_leg: Dict[str, int] = {}
    for leg in legs:
        try:
            L = _load(leg, CSV_EVENTS, _EV_COLS)
        except (ValueError, ViewerSkip):
            continue
        tier = _text(L.df, "tier")
        event = _text(L.df, "event")
        keep = np.zeros(len(L.df), dtype=bool)
        for sel_tier, sel_event in selectors:
            m = (tier == sel_tier)
            if sel_event is not None:
                m &= (event == sel_event)
            keep |= m
        gs = L.df["_gsec"].to_numpy(float)
        seen = set()
        for i in np.flatnonzero(keep):
            key = (str(tier[i]), float(gs[i]))
            if key in seen:
                continue                    # one line per (tier, epoch), not per sat
            seen.add(key)
            picked.append((leg, str(tier[i]), float(gs[i])))
        per_leg[leg.label] = len(seen)

    picked.sort(key=lambda p: (p[2], p[0].label))
    dropped = max(len(picked) - max_lines, 0)
    lines: List[VLine] = []
    labelled = set()
    for leg, tier, g in picked[:max_lines]:
        key = (leg.label, tier)
        lines.append(VLine(x=_gsec_to_utc([g])[0], color=leg.color, style=":",
                           width=1.0,
                           label=(f"{leg.label} {tier}" if key not in labelled else None)))
        labelled.add(key)
    tally = ", ".join(f"{k} {v}" for k, v in per_leg.items()) or "none"
    note = f"frame-event lines: {len(lines)} drawn ({tally})"
    if dropped:
        note += (f"; {dropped} NOT drawn (max_event_lines={max_lines}) — "
                 f"see the events comparator for the full tally")
    return (lines, note)


def _tracks_summary(es: List[_Loaded], sh: List[_Loaded], notes: List[str]) -> str:
    out = ["=== N-Run σ̂0 / a₀₀ Tracks ==="]
    if es:
        widths = (16, 12, 12, 14, 14, 12)
        rows = []
        for L in es:
            emitted = _text(L.df, "disposition") == "emitted"
            s0 = _finite(_num(L.df, "sigma0")[emitted])
            pf = _num(L.df, "sigma0_pf_code")
            if not np.isfinite(pf).any():
                pf = _num(L.df, "sigma0_pf")
            pf = _finite(pf[emitted])
            rows.append([
                L.leg.label, int(emitted.sum()),
                f"{np.median(s0):.3f}" if s0.size else "-",
                f"{np.max(s0):.3f}" if s0.size else "-",
                f"{np.median(pf):.3f}" if pf.size else "-",
                f"{np.max(pf):.3f}" if pf.size else "-"])
        out.append(_table(("leg", "emitted", "σ̂0 med", "σ̂0 max",
                           "σ̂0pf med", "σ̂0pf max"), rows, widths))
    if sh:
        out.append("")
        rows = []
        for L in sh:
            a00 = _finite(_num(L.df, "a00_tecu"))
            oob = _num(L.df, "a00_in_band") == 0.0
            rows.append([
                L.leg.label, len(L.df),
                f"{np.median(a00):.2f}" if a00.size else "-",
                f"{np.min(a00):.2f}" if a00.size else "-",
                f"{np.max(a00):.2f}" if a00.size else "-",
                int(oob.sum())])
        out.append(_table(("leg", "sh rows", "a₀₀ med", "a₀₀ min", "a₀₀ max",
                           "out-of-band"), rows, (16, 10, 11, 11, 11, 13)))
    for n in notes:
        if n:
            out.append("")
            out.append(n)
    return "\n".join(out)


# --------------------------------------------------------------------------- #
#  (c) per-tier event rates
# --------------------------------------------------------------------------- #
def compare_events(runs: RunSet, *, ratio_flag: float = 3.0,
                   top_n: int = 12) -> ViewerResult:
    """Per-tier event counts + per-epoch rates across legs, as ratios to leg 0.

    Counts alone mislead when two legs span different epoch counts (common: a
    seed and its resume leg, a full day and a window), so the ratio is taken on
    the rate and the epoch denominator is printed.
    """
    legs = runs.require(CSV_EVENTS, minimum=1)
    loaded = [_load(leg, CSV_EVENTS, _EV_COLS) for leg in legs]

    # Epoch denominators: the epoch-status row count when the leg has it, else
    # the distinct epoch count in its own events file (stated in the summary).
    denom: Dict[str, Tuple[float, str]] = {}
    for L in loaded:
        if L.leg.has(CSV_EPOCH_STATUS):
            try:
                denom[L.leg.label] = (float(len(_load(L.leg, CSV_EPOCH_STATUS,
                                                      _ES_COLS).df)), "status")
                continue
            except (ValueError, ViewerSkip):
                pass
        denom[L.leg.label] = (float(np.unique(L.df["_gsec"].to_numpy(float)).size),
                              "events")

    counts: Dict[str, Dict[str, int]] = {
        L.leg.label: _text(L.df, "tier").tolist() for L in loaded}
    counts = {lab: dict(pd.Series(v).value_counts()) for lab, v in counts.items()}
    tiers = sorted({t for c in counts.values() for t in c if str(t).strip()},
                   key=lambda t: -max(c.get(t, 0) for c in counts.values()))

    ref_lab = loaded[0].leg.label
    header = ["tier"]
    widths = [20]
    for L in loaded:
        header += [f"n_{L.leg.label}"[:11], f"rate_{L.leg.label}"[:11]]
        widths += [11, 12]
        if L.leg.label != ref_lab:
            header.append(f"x/{ref_lab}"[:10])
            widths.append(11)
    rows, called_out = [], []
    for t in tiers:
        row = [t]
        rate_ref = counts[ref_lab].get(t, 0) / max(denom[ref_lab][0], 1.0)
        for L in loaded:
            n = int(counts[L.leg.label].get(t, 0))
            rate = n / max(denom[L.leg.label][0], 1.0)
            row += [n, f"{rate:.4f}"]
            if L.leg.label == ref_lab:
                continue
            if rate_ref > 0:
                ratio = rate / rate_ref
                rstr = f"{ratio:.2f}x"
                flag = ratio > ratio_flag
            elif rate > 0:
                rstr, flag = "new", True
            else:
                rstr, flag = "-", False
            row.append(rstr + (" <<" if flag else ""))
            if flag:
                called_out.append(f"{t}/{L.leg.label} ({rstr})")
        rows.append(row)
    table = _table(header, rows, widths)

    # Rank scatter: per-tier rate per leg (log y so a 1e-4 tier stays visible).
    x = np.arange(len(tiers), dtype=float)
    ax_rank = Axes(title="Per-tier event rate (events per processed epoch)",
                   ylabel="rate (1/epoch)", xlabel="tier rank (most-frequent first)",
                   x_is_datetime=False, yscale="log", legend=Legend(loc="best"))
    ax_rank.traces = [
        Scatter(x=x, y=_pos([counts[L.leg.label].get(t, 0) / max(denom[L.leg.label][0], 1.0)
                             for t in tiers]),
                color=L.leg.color, marker=("o" if i == 0 else "^"), size=28,
                label=L.leg.label)
        for i, L in enumerate(loaded)]

    fig = Figure(name="N-run events", title="Per-tier QC event rates across runs",
                 axes=[ax_rank, _text_axes(table, boxed=True, fontsize=8.0)],
                 width=13.0, height=max(7.0, 0.28 * (len(tiers) + 6) + 4.0),
                 link_x=False)

    out = [f"=== N-Run QC Event Rates ({len(loaded)} legs) ==="]
    for L in loaded:
        n_ep, src = denom[L.leg.label]
        out.append(f"  {L.leg.label:<14} {len(L.df)} events over {int(n_ep)} epochs "
                   f"(denominator from {src}"
                   + (f"; {L.dropped} malformed row(s) dropped" if L.dropped else "")
                   + ")")
    out.append("")
    out.append(table)
    out.append("")
    out.append("Called out (>%.0fx the reference rate, or new): %s"
               % (ratio_flag, ", ".join(called_out) if called_out
                  else "none — every tier within %.0fx" % ratio_flag))

    # Top (tier, event) pairs per leg: what the tier counts are made of.
    out.append("")
    out.append(f"Top {top_n} (tier, event) pairs per leg:")
    for L in loaded:
        pair = pd.Series([f"{t}/{e}" for t, e in zip(_text(L.df, "tier"),
                                                     _text(L.df, "event"))])
        out.append(f"  {L.leg.label}:")
        for name, c in pair.value_counts().head(top_n).items():
            out.append(f"      {str(name):<40} {int(c)}")
    return ViewerResult(figures=[fig], summary="\n".join(out))


# --------------------------------------------------------------------------- #
#  (d) keyed post-fit z, joined to the station roster (the geography read)
# --------------------------------------------------------------------------- #
_PK_COLS = ("week", "tow", "kind", "key", "s0_code")
_KINDS = ("STA", "SAT", "SIG")


def compare_keyed_world(runs: RunSet, *, onset_tow: Optional[float] = None,
                        min_baseline: int = 10, top_n: int = 8,
                        z_clip: float = 8.0) -> ViewerResult:
    """Keyed post-fit robust-z at an onset epoch, per leg, on a station worldmap.

    For every ``(kind, key)`` pool the statistic is
    ``z = (s0_code(onset) - median(pre-onset)) / (1.4826 * MAD(pre-onset))`` --
    each key against **its own** history, which is what makes a station with a
    permanently noisy pool score 0 and a station that *changed* score large.
    This is the same reduction ``scripts/p2_gate_verdict.py`` §4.3 prints.

    Parameters
    ----------
    onset_tow:
        Grade at this time-of-week (nearest epoch per leg).  ``None`` (default)
        auto-picks the reference keyed leg's worst epoch: the one maximising the
        cross-STA median ``s0_code``, restricted to epochs that have at least
        ``min_baseline`` predecessors so a baseline exists.
    z_clip:
        Colour-scale saturation for the diverging map; the table always prints
        the unclipped z.
    """
    legs = runs.require(CSV_PF_KEYED, minimum=1)
    loaded = [_load(leg, CSV_PF_KEYED, _PK_COLS) for leg in legs]
    onset_gsec, how = _resolve_onset(loaded[0], onset_tow, min_baseline)

    axes: List[Axes] = []
    summary: List[str] = [
        f"=== N-Run Keyed Post-Fit RHS Localization ({len(loaded)} legs) ===",
        f"Onset epoch      : {how}",
        f"Statistic        : robust-z of s0_code vs each key's own pre-onset "
        f"baseline (median / 1.4826·MAD, >= {min_baseline} baseline epochs)",
        ""]

    for L in loaded:
        gs = np.unique(L.df["_gsec"].to_numpy(float))
        step = float(np.median(np.diff(gs))) if gs.size > 1 else 30.0
        near = gs[int(np.argmin(np.abs(gs - onset_gsec)))]
        if abs(near - onset_gsec) > 1.5 * max(step, 1.0):
            summary.append(f"  {L.leg.label:<14} no epoch within "
                           f"{1.5 * step:.0f} s of the onset "
                           f"(nearest {abs(near - onset_gsec):.0f} s away) — "
                           f"not mapped")
            summary.append("")
            continue

        z_by_kind = {k: _keyed_z(L.df, near, k, min_baseline) for k in _KINDS}
        sta = z_by_kind["STA"]
        stations = load_stations(L.leg.ctx.path(CSV_STATIONS), L.leg.ctx.network_config)
        ax, join = _keyed_axes(L.leg, sta, stations, near, z_clip)
        if ax is not None:
            axes.append(ax)

        summary.append(f"  {L.leg.label}  @ tow {near % _SEC_PER_WEEK:.0f} "
                       f"({_fmt_time(_gsec_to_utc([near])[0])})"
                       + (f"   [{abs(near - onset_gsec):.0f} s off the common onset]"
                          if near != onset_gsec else ""))
        summary.append(f"      roster {join['n_roster']} station(s); STA pools "
                       f"{join['n_pool']}; joined {join['n_joined']}; "
                       f"unjoined {join['n_unjoined']}"
                       + (f" ({', '.join(join['unjoined'][:6])}"
                          + (" ..." if len(join['unjoined']) > 6 else "") + ")"
                          if join["n_unjoined"] else ""))
        ranked = pd.concat([d.assign(kind=k) for k, d in z_by_kind.items()
                            if not d.empty], ignore_index=True) \
            if any(not d.empty for d in z_by_kind.values()) else pd.DataFrame()
        if ranked.empty:
            summary.append(f"      (no key had >= {min_baseline} pre-onset epochs)")
        else:
            ranked = ranked.reindex(ranked["z"].abs().sort_values(ascending=False).index)
            for _, r in ranked.head(top_n).iterrows():
                summary.append(f"      z={r['z']:+9.1f}  {r['kind']:<3} "
                               f"{str(r['key']):<12} s0_code {r['base_med']:.2f} "
                               f"-> {r['value']:.2f} m")
        summary.append("")

    if not axes:
        raise ViewerSkip("no leg produced a mappable keyed-z set (missing station "
                         "roster, or no epoch near the onset)")

    heights = [_height_for(ax.geo_extent) for ax in axes]
    fig = Figure(name="N-run keyed world",
                 title=f"Keyed post-fit robust-z at "
                       f"{_fmt_time(_gsec_to_utc([onset_gsec])[0])} "
                       f"(STA pools x station roster)",
                 axes=axes, width=13.0,
                 height=float(min(14.0, sum(h * 0.62 for h in heights) + 1.0)),
                 link_x=False)
    return ViewerResult(figures=[fig], summary="\n".join(summary).rstrip())


def _resolve_onset(ref: _Loaded, onset_tow: Optional[float],
                   min_baseline: int) -> Tuple[float, str]:
    """Common onset epoch (GPS seconds) + how it was chosen, from the reference leg."""
    gs = np.unique(ref.df["_gsec"].to_numpy(float))
    if onset_tow is not None:
        tow = ref.df["tow"].to_numpy(float)
        i = int(np.argmin(np.abs(tow - float(onset_tow))))
        g = float(ref.df["_gsec"].to_numpy(float)[i])
        return (g, f"tow {g % _SEC_PER_WEEK:.0f} "
                   f"({_fmt_time(_gsec_to_utc([g])[0])}) — nearest epoch to the "
                   f"requested onset_tow={onset_tow:.0f} on '{ref.leg.label}'")
    sta = ref.df[_text(ref.df, "kind") == "STA"]
    if sta.empty:
        g = float(gs[min(min_baseline, gs.size - 1)])
        return (g, f"tow {g % _SEC_PER_WEEK:.0f} "
                   f"({_fmt_time(_gsec_to_utc([g])[0])}) — no STA rows on "
                   f"'{ref.leg.label}'; first epoch past the baseline window")
    per_ep = sta.groupby("_gsec")["s0_code"].median()
    eligible = per_ep.iloc[min_baseline:] if per_ep.size > min_baseline else per_ep
    g = float(eligible.idxmax())
    return (g, f"tow {g % _SEC_PER_WEEK:.0f} ({_fmt_time(_gsec_to_utc([g])[0])}) — "
               f"auto: worst cross-STA median s0_code ({float(eligible.max()):.2f} m) "
               f"on '{ref.leg.label}'")


def _keyed_z(df: pd.DataFrame, onset_gsec: float, kind: str,
             min_baseline: int) -> pd.DataFrame:
    """Per-key robust-z at ``onset_gsec`` vs that key's own pre-onset baseline."""
    d = df[_text(df, "kind") == kind]
    if d.empty:
        return pd.DataFrame(columns=["key", "value", "base_med", "base_sigma",
                                     "n_base", "z"])
    at = d[d["_gsec"].to_numpy(float) == onset_gsec]
    pre = d[d["_gsec"].to_numpy(float) < onset_gsec]
    if at.empty or pre.empty:
        return pd.DataFrame(columns=["key", "value", "base_med", "base_sigma",
                                     "n_base", "z"])
    at_v = at.groupby("key")["s0_code"].last()
    g = pre.groupby("key")["s0_code"]
    med = g.median()
    n_base = g.size()
    dev = (pre["s0_code"].astype(float) - pre["key"].map(med)).abs()
    sigma = 1.4826 * dev.groupby(pre["key"].to_numpy()).median()

    out = pd.DataFrame({"value": at_v}).join(
        pd.DataFrame({"base_med": med, "base_sigma": sigma, "n_base": n_base}),
        how="inner")
    out = out[out["n_base"] >= int(min_baseline)]
    if out.empty:
        return out.reset_index().rename(columns={"index": "key"})
    s = out["base_sigma"].to_numpy(float)
    out["z"] = np.where(s > 1e-6,
                        (out["value"].to_numpy(float) - out["base_med"].to_numpy(float))
                        / np.where(s > 1e-6, s, 1.0), 0.0)
    return out.reset_index().rename(columns={"index": "key"})


def _keyed_axes(leg: RunLeg, sta_z: pd.DataFrame, stations: List[Station],
                onset_gsec: float, z_clip: float) -> Tuple[Optional[Axes], dict]:
    """One geo axes: roster stations coloured by their STA-pool z (grey = no pool)."""
    join = {"n_roster": len(stations), "n_pool": int(len(sta_z)),
            "n_joined": 0, "n_unjoined": 0, "unjoined": []}
    if not stations:
        join["n_unjoined"] = int(len(sta_z))
        join["unjoined"] = [str(k) for k in sta_z["key"].tolist()]
        return (None, join)

    by_id = {s.id: s for s in stations}
    by_short = {}
    for s in stations:                       # 4-char fallback (RINEX-3 short code)
        by_short.setdefault(s.id[:4], s)
    lon, lat, zv, names = [], [], [], []
    hit = set()
    for _, r in sta_z.iterrows():
        key = str(r["key"])
        st = by_id.get(key) or by_short.get(key[:4])
        if st is None:
            join["unjoined"].append(key)
            continue
        hit.add(st.id)
        lon.append(st.lon)
        lat.append(st.lat)
        zv.append(float(r["z"]))
        names.append(st.label)
    join["n_joined"] = len(zv)
    join["n_unjoined"] = len(join["unjoined"])

    extent = _auto_extent(stations)
    zc = float(np.clip(np.nanpercentile(np.abs(zv), 95) if zv else 3.0,
                       3.0, float(z_clip)))
    ax = Axes(projection="geo", geo_extent=extent, grid=False, x_is_datetime=False,
              coastline=True, graticule_deg=30.0, legend=Legend(loc="lower left"),
              title=f"{leg.label} — keyed STA robust-z "
                    f"({len(zv)}/{len(stations)} stations, colour clipped ±{zc:.1f})",
              cmap=strip_colormap("vik", 256), clim=(-zc, zc),
              colorbar=ColorBar(label="robust-z of s0_code"))
    traces: List = []

    # Roster stations with no keyed pool are drawn, not dropped: an absent pool
    # is itself a result (that station contributed no code rows this epoch).
    miss = [s for s in stations if s.id not in hit]
    if miss:
        traces.append(Scatter(x=[s.lon for s in miss], y=[s.lat for s in miss],
                              color=C_ROSTER_ONLY, marker="o", size=12, alpha=0.85,
                              zorder=4, label=f"no keyed pool ({len(miss)})"))
    if zv:
        traces.append(Scatter(x=lon, y=lat, c=zv, marker="o", size=46,
                              edgecolor=(1.0, 1.0, 1.0), linewidth=0.5, zorder=6,
                              label=f"keyed z ({len(zv)})"))
    for role, color, marker, size in ((ROLE_DATUM, C_DATUM, "*", 320),
                                      (ROLE_HELD_OUT, C_HELD_OUT, "^", 130)):
        pts = [s for s in stations if s.role == role]
        if pts:
            traces.append(Scatter(x=[p.lon for p in pts], y=[p.lat for p in pts],
                                  color=color, marker=marker, size=size,
                                  edgecolor=(0.15, 0.15, 0.15), linewidth=0.6,
                                  zorder=7,
                                  label=f"{role} ({len(pts)})"))
    # Label the three largest |z|: naming those stations is what the map is for.
    if zv:
        order = np.argsort(-np.abs(np.asarray(zv, dtype=float)))[:3]
        for i in order:
            traces.append(TextBox(x=lon[i] + 1.5, y=lat[i] + 1.5,
                                  text=f"{names[i]} {zv[i]:+.0f}", data_coords=True,
                                  fontsize=7, bold=True, color=(0.1, 0.1, 0.1),
                                  zorder=8))
    ax.traces = traces
    return (ax, join)


# --------------------------------------------------------------------------- #
#  (e) scorecard
# --------------------------------------------------------------------------- #
def compare_scorecard(runs: RunSet) -> ViewerResult:
    """Every other comparator's summary in one monospaced panel."""
    lines: List[str] = [f"=== N-Run Scorecard: {' vs '.join(runs.labels)} ==="]
    for leg in runs.legs:
        present = [c for c in (CSV_EPOCH_STATUS, CSV_EVENTS, CSV_SH_HEALTH,
                               CSV_PF_KEYED, CSV_STATIONS) if leg.has(c)]
        lines.append(f"  {leg.label:<14} {leg.log_dir}")
        lines.append(f"  {'':<14} traces: "
                     + (", ".join(c.replace('network_', '').replace('.csv', '')
                                  for c in present) or "(none)"))
    lines.append("")
    for name, fn in _SCORECARD_SOURCES:
        try:
            res = fn(runs)
            lines.append(res.summary if res.summary else f"[{name}] (no summary)")
        except ViewerSkip as sk:
            lines.append(f"[{name}] skipped: {sk}")
        except Exception as exc:  # pragma: no cover - defensive
            lines.append(f"[{name}] error: {type(exc).__name__}: {exc}")
        lines.append("")
    text = "\n".join(lines)
    fig = Figure(name="N-run scorecard", title="N-run scorecard",
                 axes=[_text_axes(text, fontsize=8.0)], width=13.0,
                 height=max(4.0, 0.17 * (text.count("\n") + 1)))
    return ViewerResult(figures=[fig], summary=text)


# --------------------------------------------------------------------------- #
#  Ordered comparator registry (separate from the single-ctx viewers REGISTRY)
# --------------------------------------------------------------------------- #
@dataclass
class RunComparator:
    """A named N-run comparator (the N-ary analogue of a :class:`Viewer`)."""
    name: str
    title: str
    build: Callable[[RunSet], ViewerResult]


RUN_COMPARATORS: List[RunComparator] = [
    RunComparator("disposition", "Epoch dispositions across runs", compare_dispositions),
    RunComparator("tracks", "σ̂0 / a₀₀ tracks across runs", compare_tracks),
    RunComparator("events", "QC event rates across runs", compare_events),
    RunComparator("keyed", "Keyed post-fit RHS worldmap", compare_keyed_world),
    RunComparator("scorecard", "N-run scorecard", compare_scorecard),
]

#: The comparators the scorecard rolls up (everything but itself).
_SCORECARD_SOURCES = [(c.name, c.build) for c in RUN_COMPARATORS
                      if c.name != "scorecard"]


@dataclass
class MultiCompareRun:
    """Outcome of running the comparators over one :class:`RunSet`."""
    ran: List[Tuple[RunComparator, ViewerResult]] = field(default_factory=list)
    skipped: List[Tuple[str, str]] = field(default_factory=list)
    failed: List[Tuple[str, str]] = field(default_factory=list)


def run_multi_comparators(runs: RunSet, only: Optional[Sequence[str]] = None,
                          registry: Sequence[RunComparator] = None) -> MultiCompareRun:
    """Run every (or an ``only`` subset of) comparator over ``runs``.

    A comparator that raises :class:`ViewerSkip` becomes a ``(name, reason)``
    skip; any other exception becomes a ``(name, error)`` failure.  Neither
    aborts the rest, mirroring :func:`gnssviz.run_viewers`.
    """
    want = set(only) if only else None
    out = MultiCompareRun()
    for comp in (RUN_COMPARATORS if registry is None else registry):
        if want is not None and comp.name not in want:
            continue
        try:
            out.ran.append((comp, comp.build(runs)))
        except ViewerSkip as sk:
            out.skipped.append((comp.name, str(sk)))
        except Exception as exc:  # pragma: no cover - defensive
            out.failed.append((comp.name, f"{type(exc).__name__}: {exc}"))
    return out
