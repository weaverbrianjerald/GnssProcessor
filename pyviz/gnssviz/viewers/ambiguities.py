"""Float-ambiguity viewer: the port of ``plotAmbiguities.m``.

Reads ``ambiguity_trace.csv`` (header ``week,tow_s,key,value_m,std_m``) where the
``key`` column is a ``<SAT>_<SIGNAL>`` state key (``G01_L1C``, ``E11_L1C_L5Q``,
...).  The satellite token is the first underscore-separated field; the remainder
(joined with ``_``) is the signal label.  Timestamps are pure GPS time.

It reproduces the two MATLAB figures (and the console summary):

* **Figure 1, "Ambiguity estimates (meters) by System".**  One tile per
  constellation present (in MATLAB ``categories`` alphabetical order: BeiDou,
  GLONASS, GPS, Galileo, QZSS, SBAS).  Each unique key in that system is drawn as
  scatter markers (no connecting line, no error bars: the MATLAB error-bar block
  is commented out).  Colour tracks the *global* satellite index (the satellite's
  position in the whole-dataset alphabetical sort), via ``distinguishable_colors``
  cycled with ``mod``; marker glyph cycles per key within the system through the
  MATLAB marker list ``o s d ^ v > < p h x +``.  Each key gets a legend entry.

* **Figure 2, "Ambiguity time-series per key"** (only when
  ``1 < nKeys <= 16``).  A grid of one tile per key: scatter markers, a shaded
  +/-1 sigma fill band (``std_m``), and the +/-1 sigma envelope edges (the MATLAB
  errorbar verticals, rendered here as band edges since ``figures.py`` has no
  vertical-errorbar primitive).  Tiles use the MATLAB default blue
  ``[0 0.4470 0.7410]``.  For the bundled trace (63 keys) this figure is suppressed,
  matching MATLAB.

* **Summary.**  Per-key count / mean / sample-std (``ddof=1``; 0 for a single
  sample, matching MATLAB ``std`` of one element).

Faithfulness notes
------------------
* The MATLAB grid layout is ``rows = ceil(sqrt(nKeys)); cols = ceil(nKeys/rows)``.
  ``figures.py`` stacks axes vertically (one column) rather than honouring a true
  rows x cols tiled layout, so the per-key figure becomes a tall single column of
  tiles: same tiles, same content, linear stacking.
* MATLAB draws each per-system marker's value-vs-time as ``scatter`` only; the
  ``errorbar`` overlay in Figure 1 is commented out in the source, and this port
  omits it too.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..palettes import distinguishable_colors
from ..figures import Axes, Band, Figure, Legend, Scatter, TextBox
from .base import Viewer, ViewerResult

# MATLAB marker list (plotAmbiguities.m line 78).
_MARKERS = ["o", "s", "d", "^", "v", ">", "<", "p", "h", "x", "+"]

# System assignment by leading satellite letter (plotAmbiguities.m lines 60-65).
_SYS_BY_LETTER = {
    "G": "GPS", "E": "Galileo", "R": "GLONASS",
    "C": "BeiDou", "J": "QZSS", "S": "SBAS",
}

# MATLAB ``categories`` order is a case-sensitive alphabetical sort of the system
# names that appear; this is the fixed render order (Unknown excluded).
_SYS_ORDER = ["BeiDou", "GLONASS", "GPS", "Galileo", "QZSS", "SBAS"]

# MATLAB default first-colour-order blue used in the per-key figure.
_MATLAB_BLUE = (0.0, 0.4470, 0.7410)
_BAND_FILL = (0.9, 0.9, 1.0)        # [0.9 0.9 1] fill colour


def _split_key(key: str):
    """``'G01_L1C_L2W'`` -> (``'G01'``, ``'L1C_L2W'``); signal '' if no '_'."""
    s = str(key)
    if "_" in s:
        sat, sig = s.split("_", 1)
        return sat, sig
    return s, ""


def _system_of(sat: str) -> str:
    return _SYS_BY_LETTER.get(str(sat)[:1].upper(), "Unknown")


def build_ambiguities(ctx, max_keys_single_fig: int = 16) -> ViewerResult:
    """Build the by-system scatter figure (+ optional per-key grid) and summary.

    ``max_keys_single_fig`` defaults to 16, the MATLAB ``maxKeysSingleFig``: the
    per-key detail figure is only emitted when ``1 < nKeys <= max_keys_single_fig``.
    """
    df = load_trace(ctx.path("ambiguity_trace.csv"))
    require_cols(df, ("week", "tow_s", "key", "value_m", "std_m"),
                 "ambiguity_trace.csv")
    df = add_utc(df)
    df["key"] = df["key"].astype(str)

    sats = df["key"].map(lambda k: _split_key(k)[0])
    df = df.assign(satellite=sats, system=sats.map(_system_of))

    # Global satellite index: position of each satellite in the whole-dataset
    # alphabetical sort (MATLAB ``categories(T.satellite)`` + ``find``).
    unique_sats = sorted(df["satellite"].unique())
    sat_global_idx = {s: i for i, s in enumerate(unique_sats)}
    n_sats = len(unique_sats)

    # MATLAB ``distinguishable_colors(max(8,N))``.
    colors = distinguishable_colors(max(8, n_sats))
    n_colors = len(colors)

    figures = []

    # ---- Figure 1: per-system scatter ------------------------------------- #
    systems_present = [s for s in _SYS_ORDER if (df["system"] == s).any()]
    if not systems_present:
        raise ValueError("No recognized GNSS systems found in ambiguity CSV")

    sys_axes = []
    for sys_name in systems_present:
        d_sys = df[df["system"] == sys_name]
        ax = Axes(title=sys_name, xlabel="GPS Time", ylabel="Ambiguity (m)",
                  legend=Legend(loc="best"))
        keys_in_sys = sorted(d_sys["key"].unique())
        for k, this_key in enumerate(keys_in_sys):
            d_key = d_sys[d_sys["key"] == this_key].sort_values("utc")
            sat_name = _split_key(this_key)[0]
            g = sat_global_idx.get(sat_name, k)
            c = colors[g % n_colors]
            marker = _MARKERS[k % len(_MARKERS)]
            ax.traces.append(Scatter(
                x=d_key["utc"], y=d_key["value_m"].to_numpy(float),
                label=this_key, color=c, size=36.0, marker=marker))
        sys_axes.append(ax)

    figures.append(Figure(
        name="Ambiguity traces by System",
        title="Ambiguity estimates (meters) by System",
        axes=sys_axes, width=14, height=5.0))

    # ---- Figure 2: per-key detail (only for a moderate key count) --------- #
    all_keys = sorted(df["key"].unique())
    n_keys = len(all_keys)
    if 1 < n_keys <= max_keys_single_fig:
        key_axes = []
        for this_key in all_keys:
            d_key = df[df["key"] == this_key].sort_values("utc")
            t = d_key["utc"]
            vals = d_key["value_m"].to_numpy(float)
            err = d_key["std_m"].to_numpy(float)
            ax = Axes(title=this_key, xlabel="GPS Time", ylabel="Ambiguity (m)")
            # Shaded +/-1 sigma band (MATLAB fill of [vals-err, vals+err]); the
            # band edges stand in for the (no-marker) errorbar verticals.
            ax.traces.append(Band(x=t, lo=vals - err, hi=vals + err,
                                  color=_BAND_FILL, alpha=0.5))
            ax.traces.append(Scatter(x=t, y=vals, color=_MATLAB_BLUE,
                                     size=36.0, marker="o", show_in_legend=False))
            key_axes.append(ax)
        figures.append(Figure(
            name="Ambiguity per Key",
            title="Ambiguity time-series per key",
            axes=key_axes, width=12,
            height=min(16.0, 2.2 * n_keys)))

    return ViewerResult(figures=figures, summary=_summary(df, all_keys))


def _summary(df: pd.DataFrame, all_keys) -> str:
    """Per-key count / mean / sample-std read-out (MATLAB summary table)."""
    lines = ["=== Ambiguity Summary ===",
             f"{'Key':<16}{'Count':>8}{'Mean_m':>14}{'Std_m':>14}",
             "-" * 52]
    for key in all_keys:
        vals = df.loc[df["key"] == key, "value_m"].to_numpy(float)
        cnt = vals.size
        if cnt == 0:
            mean = std = float("nan")
        else:
            mean = float(np.mean(vals))
            std = float(np.std(vals, ddof=1)) if cnt > 1 else 0.0
        lines.append(f"{key:<16}{cnt:>8d}{mean:>14.4f}{std:>14.4f}")
    return "\n".join(lines)


viewer = Viewer(name="ambiguities", title="Float ambiguities",
                required_csvs=("ambiguity_trace.csv",),
                build=build_ambiguities)
