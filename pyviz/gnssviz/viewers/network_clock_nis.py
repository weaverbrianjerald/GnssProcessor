"""Two-state clock-model NIS viewer, the V2 lock-in instrument.

Reads ``network_clock_monitor.csv`` (the PPP-RTK network producer's per-satellite
clock sanity trace). That one CSV serves two detectors, and its columns carry
different meanings depending on which one wrote it:

* **model** mode (``network.clock_model`` on): a two-state oscillator predicts
  each satellite clock forward; the normalized innovation ``nis = (innov − datum
  common-mode) / √S`` is tested against the model's own tight prediction band
  ``k = clock_model_k`` (σ). A healthy clock tracks its prediction
  (``|nis| ~ 1``); a self-consistent lock-in jumps hundreds of σ. The columns
  carry: ``clk_m`` = solved, ``median_m`` = the pre-update prediction,
  ``dev_m`` = de-meaned innovation, ``prev_clk_m`` = drift, ``jump_m`` = raw
  innovation, ``abs_max_m`` = the per-row band ``√S`` (varies row to row),
  ``jump_max_m`` = ``k`` (constant).

* **monitor** mode (``network.clock_monitor`` on, ``clock_model`` off): a
  post-solve absolute/jump sanity check: ``dev_m`` vs the constant absolute bound
  ``abs_max_m`` and ``jump_m`` vs the constant jump bound ``jump_max_m``. ``nis``
  is blank; ``prev_clk_m``/``jump_m`` may be blank (``,,``).

Mode is taken from the ``mode`` column when present. Legacy files (no ``nis`` /
``mode`` columns) are routed by a heuristic: model if ``abs_max_m.nunique() > 1``
(model writes a per-row ``√S``; monitor writes a constant bound), defaulting to
``monitor`` when undecidable (e.g. a single row).

Model-mode figures:
  1. ``|NIS|`` per satellite (scatter, PRN-coloured) with the ``k`` band line;
     red rings where ``flag_abs`` fired, orange where ``flag_ext`` (an already-
     flagged RAIM/within-epoch sat) fired.
  2. Worst-6 satellites by ``max|nis|``: solved clock, its pre-update
     prediction, and the ``median_m ± k·√S`` prediction band, NaN-gapped.
  3. ``|nis|`` (sat × epoch) strip via the shared strip builder (viridis).
  4. Per-satellite drift (``prev_clk_m``).

Monitor-mode fallback: ``dev_m`` vs the ``±abs_max_m`` bound and ``jump_m`` vs the
``±jump_max_m`` bound, flagged samples ringed.
"""

from __future__ import annotations

from typing import List

import numpy as np
import pandas as pd

from ..figures import Axes, Band, Figure, HLine, Legend, Line, Scatter, TextBox
from ..identifiers import system_of
from ..io import add_utc, load_trace, require_cols
from ..palettes import color_by_prn
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult, ViewerSkip

# Ring colours for the flagged-sample overlays.
_C_ABS = (0.85, 0.10, 0.10)   # red: own-bound / NIS fire (flag_abs)
_C_EXT = (0.95, 0.55, 0.10)   # orange: already flagged by RAIM/within-epoch (flag_ext)
_C_JUMP = (0.60, 0.20, 0.70)  # purple: monitor jump-bound fire (flag_jump)
_C_BOUND = (0.55, 0.55, 0.55)

_MONITOR = "monitor"
_MODEL = "model"


# --------------------------------------------------------------------------- #
#  Mode resolution
# --------------------------------------------------------------------------- #
def _resolve_mode(df: pd.DataFrame) -> str:
    """Return ``'model'`` / ``'monitor'`` for the whole file.

    Uses the ``mode`` column when present (first non-blank value); legacy files
    fall back to the ``abs_max_m.nunique() > 1`` heuristic (model writes per-row
    ``√S``; monitor writes a constant bound), defaulting to ``monitor`` when the
    data cannot decide (e.g. a single row, or all-NaN bounds).
    """
    if "mode" in df.columns:
        vals = df["mode"].dropna().astype(str).str.strip()
        vals = vals[vals != ""]
        if not vals.empty:
            m = vals.iloc[0].lower()
            return _MODEL if m == _MODEL else _MONITOR
    # Legacy: no mode column -> fall back to the abs_max_m bound.
    if "abs_max_m" in df.columns:
        bound = pd.to_numeric(df["abs_max_m"], errors="coerce").dropna()
        if bound.nunique() > 1:
            return _MODEL
    return _MONITOR


def _sys_present_order(sats) -> List[str]:
    from ..palettes import SYSTEM_ORDER
    present = {system_of(s) for s in sats}
    return [s for s in SYSTEM_ORDER if s in present]


# --------------------------------------------------------------------------- #
#  Build
# --------------------------------------------------------------------------- #
def build_network_clock_nis(ctx) -> ViewerResult:
    df = load_trace(ctx.path("network_clock_monitor.csv"))
    # Only the pre-L4 stable columns are guaranteed; nis/mode are optional (legacy).
    require_cols(df, ("week", "tow", "sat", "clk_m", "median_m", "dev_m",
                      "abs_max_m", "jump_max_m", "flag_abs", "flag_jump",
                      "flag_ext", "refloated"), "network_clock_monitor.csv")
    if df.empty:
        raise ViewerSkip("network_clock_monitor.csv has no rows")

    df = df.copy()
    df["sat"] = df["sat"].astype(str)
    df["system"] = df["sat"].map(system_of)
    df = add_utc(df, sort=True)

    # Numeric coercion (blanks -> NaN already from read_csv, but the mode-remapped
    # columns can carry ",," in monitor mode).
    for col in ("clk_m", "median_m", "dev_m", "prev_clk_m", "jump_m",
                "abs_max_m", "jump_max_m", "nis"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    for col in ("flag_abs", "flag_jump", "flag_ext", "refloated"):
        df[col] = pd.to_numeric(df[col], errors="coerce").fillna(0.0)

    mode = _resolve_mode(df)
    sat_color = color_by_prn(df["sat"].tolist())

    if mode == _MODEL:
        return _build_model(df, sat_color)
    return _build_monitor(df, sat_color)


# --------------------------------------------------------------------------- #
#  Model mode
# --------------------------------------------------------------------------- #
def _build_model(df: pd.DataFrame, sat_color) -> ViewerResult:
    if "nis" not in df.columns:
        # Legacy model file lacking the nis column: recover |nis| from dev_m/√S
        # (abs_max_m == √S in model mode) so the viewer still functions.
        df = df.copy()
        with np.errstate(divide="ignore", invalid="ignore"):
            df["nis"] = df["dev_m"] / df["abs_max_m"]

    nis = df["nis"].to_numpy(float)
    absnis = np.abs(nis)
    t = df["utc"].to_numpy("datetime64[ns]")

    # k = clock_model_k = the (constant) jump_max_m column; median is robust to
    # any stray NaN row.
    k_vals = df["jump_max_m"].to_numpy(float)
    k_vals = k_vals[np.isfinite(k_vals)]
    k = float(np.median(k_vals)) if k_vals.size else 6.0

    flag_abs = df["flag_abs"].to_numpy(float) > 0
    flag_ext = df["flag_ext"].to_numpy(float) > 0

    figures: List[Figure] = []

    # --- Fig 1: |NIS| per sat scatter, PRN-coloured, band line + flag rings ---
    ax1 = Axes(title=f"Clock-model normalized innovation (k = {k:.1f}σ)",
               ylabel="|NIS| (σ)", xlabel="GPS Time", legend=Legend(loc="best"))
    tr1: list = []
    for sat in _ordered(df["sat"].unique()):
        sub = df[df["sat"] == sat]
        tr1.append(Scatter(x=sub["utc"].to_numpy("datetime64[ns]"),
                           y=np.abs(sub["nis"].to_numpy(float)),
                           color=sat_color[sat], size=10.0, marker="o",
                           show_in_legend=False))
    tr1.append(HLine(y=k, color=(0.7, 0.2, 0.2), style="--", label=f"k = {k:.1f}σ"))
    if flag_abs.any():
        tr1.append(Scatter(x=t[flag_abs], y=absnis[flag_abs], size=42.0,
                           marker="o", color=(1.0, 1.0, 1.0), alpha=0.0,
                           edgecolor=_C_ABS, linewidth=1.4,
                           label=f"|NIS|>k ({int(flag_abs.sum())})",
                           zorder=5.0))
    if flag_ext.any():
        tr1.append(Scatter(x=t[flag_ext], y=absnis[flag_ext], size=42.0,
                           marker="o", color=(1.0, 1.0, 1.0), alpha=0.0,
                           edgecolor=_C_EXT, linewidth=1.4,
                           label=f"ext-flagged ({int(flag_ext.sum())})",
                           zorder=4.0))
    ax1.traces = tr1
    figures.append(Figure(name="Clock NIS",
                          title="PPP-RTK network clock-model NIS",
                          axes=[ax1], width=12.0, height=4.5, link_x=True))

    # --- Fig 2: worst-6 sats by max|nis|: clk, prediction, band ---
    worst = _worst_sats(df, absnis, n=6)
    if worst:
        axes2: List[Axes] = []
        for sat in worst:
            sub = df[df["sat"] == sat].sort_values("utc")
            ts = sub["utc"].to_numpy("datetime64[ns]")
            clk = sub["clk_m"].to_numpy(float)
            pred = sub["median_m"].to_numpy(float)      # PRE-update prediction
            band = k * sub["abs_max_m"].to_numpy(float)  # k·√S
            mn = float(np.nanmax(np.abs(sub["nis"].to_numpy(float)))
                       ) if np.isfinite(sub["nis"].to_numpy(float)).any() else float("nan")
            ax = Axes(title=f"{sat}  (max|NIS|={mn:.1f}σ)", ylabel="clock (m)",
                      xlabel="GPS Time", grid=True)
            ax.traces = [
                Band(x=ts, lo=pred - band, hi=pred + band, color=(0.4, 0.5, 0.7),
                     alpha=0.20, label=f"pred ± {k:.0f}·√S"),
                Line(x=ts, y=pred, color=(0.35, 0.35, 0.35), width=1.0,
                     style="--", label="prediction"),
                Line(x=ts, y=clk, color=sat_color[sat], width=1.3,
                     label="solved clk"),
            ]
            axes2.append(ax)
        figures.append(Figure(name="Clock NIS worst sats",
                              title="Worst-6 satellites by max |NIS| — clock vs prediction band",
                              axes=axes2, width=12.0,
                              height=min(11.0, 1.9 * len(axes2) + 0.6),
                              link_x=True))

    # --- Fig 3: |nis| (sat x epoch) strip via the shared builder ---
    strip_df = pd.DataFrame({
        "system": df["system"].to_numpy(str),
        "satellite": df["sat"].to_numpy(str),
        "facet": "",
        "utc": df["utc"].to_numpy("datetime64[ns]"),
        "value": absnis,
    })
    strip_figs = build_sat_epoch_strip(
        strip_df, colormap="viridis", caxis=(0.0, 1.5 * k),
        fig_title="Clock-model |NIS|", value_label="|NIS| (σ)",
        fig_tag="clock_nis_strip", facet_label_fallback="satellite")
    figures.extend(strip_figs)

    # --- Fig 4: per-satellite drift (prev_clk_m) ---
    if "prev_clk_m" in df.columns and np.isfinite(df["prev_clk_m"].to_numpy(float)).any():
        ax4 = Axes(title="Two-state clock drift", ylabel="drift (m/s)",
                   xlabel="GPS Time", grid=True)
        tr4: list = []
        for sat in _ordered(df["sat"].unique()):
            sub = df[df["sat"] == sat].sort_values("utc")
            d = sub["prev_clk_m"].to_numpy(float)
            if not np.isfinite(d).any():
                continue
            tr4.append(Line(x=sub["utc"].to_numpy("datetime64[ns]"), y=d,
                            color=sat_color[sat], width=0.8, show_in_legend=False))
        ax4.traces = tr4
        figures.append(Figure(name="Clock drift", title="Two-state clock-model drift",
                              axes=[ax4], width=12.0, height=4.0, link_x=True))

    return ViewerResult(figures=figures,
                        summary=_summary_model(df, absnis, k, flag_abs, flag_ext))


# --------------------------------------------------------------------------- #
#  Monitor mode
# --------------------------------------------------------------------------- #
def _build_monitor(df: pd.DataFrame, sat_color) -> ViewerResult:
    t = df["utc"].to_numpy("datetime64[ns]")
    dev = df["dev_m"].to_numpy(float)
    abs_bound_vals = df["abs_max_m"].to_numpy(float)
    abs_bound_vals = abs_bound_vals[np.isfinite(abs_bound_vals)]
    abs_bound = float(np.median(abs_bound_vals)) if abs_bound_vals.size else float("nan")
    jump_bound_vals = df["jump_max_m"].to_numpy(float)
    jump_bound_vals = jump_bound_vals[np.isfinite(jump_bound_vals)]
    jump_bound = float(np.median(jump_bound_vals)) if jump_bound_vals.size else float("nan")

    flag_abs = df["flag_abs"].to_numpy(float) > 0
    flag_jump = df["flag_jump"].to_numpy(float) > 0
    flag_ext = df["flag_ext"].to_numpy(float) > 0

    axes: List[Axes] = []

    # --- Axes 1: de-meaned clock deviation vs the absolute bound ---
    ax_dev = Axes(title="Clock-monitor deviation vs absolute bound",
                  ylabel="dev = clk − median (m)", xlabel="GPS Time",
                  legend=Legend(loc="best"))
    tr = []
    for sat in _ordered(df["sat"].unique()):
        sub = df[df["sat"] == sat]
        tr.append(Scatter(x=sub["utc"].to_numpy("datetime64[ns]"),
                          y=sub["dev_m"].to_numpy(float), color=sat_color[sat],
                          size=10.0, marker="o", show_in_legend=False))
    if np.isfinite(abs_bound):
        tr.append(HLine(y=abs_bound, color=_C_BOUND, style="--",
                        label=f"±{abs_bound:.1f} m"))
        tr.append(HLine(y=-abs_bound, color=_C_BOUND, style="--"))
    _ring(tr, t, dev, flag_abs, _C_ABS, f"abs fire ({int(flag_abs.sum())})")
    _ring(tr, t, dev, flag_ext, _C_EXT, f"ext-flagged ({int(flag_ext.sum())})")
    ax_dev.traces = tr
    axes.append(ax_dev)

    # --- Axes 2: one-epoch jump vs the jump bound ---
    if "jump_m" in df.columns and np.isfinite(df["jump_m"].to_numpy(float)).any():
        jump = df["jump_m"].to_numpy(float)
        ax_j = Axes(title="Clock-monitor one-epoch jump vs jump bound",
                    ylabel="jump (m)", xlabel="GPS Time", legend=Legend(loc="best"))
        trj = []
        for sat in _ordered(df["sat"].unique()):
            sub = df[df["sat"] == sat]
            j = sub["jump_m"].to_numpy(float)
            if not np.isfinite(j).any():
                continue
            trj.append(Scatter(x=sub["utc"].to_numpy("datetime64[ns]"), y=j,
                               color=sat_color[sat], size=10.0, marker="o",
                               show_in_legend=False))
        if np.isfinite(jump_bound):
            trj.append(HLine(y=jump_bound, color=_C_BOUND, style="--",
                             label=f"±{jump_bound:.1f} m"))
            trj.append(HLine(y=-jump_bound, color=_C_BOUND, style="--"))
        _ring(trj, t, jump, flag_jump, _C_JUMP, f"jump fire ({int(flag_jump.sum())})")
        ax_j.traces = trj
        axes.append(ax_j)

    fig = Figure(name="Clock monitor",
                 title="PPP-RTK network clock-monitor (absolute / jump sanity)",
                 axes=axes, width=12.0, height=min(9.0, 4.2 * len(axes)),
                 link_x=True)
    return ViewerResult(figures=[fig],
                        summary=_summary_monitor(df, dev, abs_bound, jump_bound,
                                                 flag_abs, flag_jump, flag_ext))


# --------------------------------------------------------------------------- #
#  Helpers
# --------------------------------------------------------------------------- #
def _ring(traces: list, t, y, mask, color, label) -> None:
    """Append a hollow ring Scatter over the flagged samples (if any)."""
    m = np.asarray(mask, bool) & np.isfinite(np.asarray(y, float))
    if m.any():
        traces.append(Scatter(x=t[m], y=np.asarray(y, float)[m], size=42.0,
                              marker="o", color=(1.0, 1.0, 1.0), alpha=0.0,
                              edgecolor=color, linewidth=1.4, label=label,
                              zorder=5.0))


def _worst_sats(df: pd.DataFrame, absnis, n: int) -> List[str]:
    """Return up to ``n`` satellite IDs with the largest finite max|nis|."""
    tmp = pd.DataFrame({"sat": df["sat"].to_numpy(str), "a": absnis})
    tmp = tmp[np.isfinite(tmp["a"])]
    if tmp.empty:
        return []
    order = (tmp.groupby("sat")["a"].max().sort_values(ascending=False))
    return list(order.index[:n])


def _ordered(keys):
    def k(s):
        s = str(s)
        try:
            return (0, int(s[1:3]), s)
        except (ValueError, IndexError):
            return (1, 0, s)
    return sorted((str(x) for x in keys), key=k)


def _std(v):
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    return float(np.std(v, ddof=1)) if v.size > 1 else 0.0


def _median(v):
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    return float(np.median(v)) if v.size else float("nan")


def _worst_sat_name(df: pd.DataFrame, values) -> str:
    """Satellite with the largest |value| (name only), clock-stat convention."""
    tmp = pd.DataFrame({"sat": df["sat"].to_numpy(str),
                        "a": np.abs(np.asarray(values, float))})
    tmp = tmp[np.isfinite(tmp["a"])]
    if tmp.empty:
        return "n/a"
    g = tmp.groupby("sat")["a"].max()
    return str(g.idxmax())


def _summary_model(df, absnis, k, flag_abs, flag_ext) -> str:
    out = ["=== PPP-RTK Network Clock-Model NIS (model mode) ==="]
    out.append(f"Rows / satellites : {len(df)} / {df['sat'].nunique()}")
    out.append(f"Prediction band k : {k:.1f} sigma  (clock_model_k)")
    # median + std + worst-sat convention (never rms alone).
    out.append(f"|NIS| median/std  : {_median(absnis):.3f} / {_std(absnis):.3f} sigma "
               f"(worst sat {_worst_sat_name(df, df['nis'].to_numpy(float))})")
    over = df[np.abs(df["nis"].to_numpy(float)) > k]
    n_over = len(over)
    out.append(f"|NIS| > k fires   : {n_over}"
               + (f"  (own flag_abs={int(flag_abs.sum())}, "
                  f"ext-flagged={int(flag_ext.sum())})" if n_over or flag_ext.any() else ""))
    if n_over:
        tows = sorted(set(f"{w}:{t:.1f}" for w, t in
                          zip(over["week"].to_numpy(), over["tow"].to_numpy(float))))
        shown = tows[:20]
        out.append("  TOWs (week:tow): " + ", ".join(shown)
                   + (f"  (+{len(tows) - len(shown)} more)" if len(tows) > len(shown) else ""))
        worst = over.iloc[np.argmax(np.abs(over["nis"].to_numpy(float)))]
        out.append(f"  worst fire      : {worst['sat']} |NIS|={abs(worst['nis']):.1f} sigma "
                   f"@ {int(worst['week'])}:{worst['tow']:.1f}")
    return "\n".join(out)


def _summary_monitor(df, dev, abs_bound, jump_bound, flag_abs, flag_jump, flag_ext) -> str:
    out = ["=== PPP-RTK Network Clock-Monitor (absolute / jump) ==="]
    out.append(f"Rows / satellites : {len(df)} / {df['sat'].nunique()}")
    out.append(f"Bounds            : |dev|<{abs_bound:.1f} m, |jump|<{jump_bound:.1f} m")
    out.append(f"|dev| median/std  : {_median(np.abs(dev)):.3f} / {_std(np.abs(dev)):.3f} m "
               f"(worst sat {_worst_sat_name(df, dev)})")
    fires = int((flag_abs | flag_jump).sum())
    out.append(f"Bound fires       : {fires}  "
               f"(abs={int(flag_abs.sum())}, jump={int(flag_jump.sum())}, "
               f"ext-flagged={int(flag_ext.sum())})")
    if fires:
        fired = df[(flag_abs | flag_jump)]
        tows = sorted(set(f"{w}:{t:.1f}" for w, t in
                          zip(fired["week"].to_numpy(), fired["tow"].to_numpy(float))))
        shown = tows[:20]
        out.append("  TOWs (week:tow): " + ", ".join(shown)
                   + (f"  (+{len(tows) - len(shown)} more)" if len(tows) > len(shown) else ""))
    return "\n".join(out)


viewer = Viewer(name="network_clock_nis",
                title="Network clock-model NIS / clock-monitor",
                required_csvs=("network_clock_monitor.csv",),
                build=build_network_clock_nis)
