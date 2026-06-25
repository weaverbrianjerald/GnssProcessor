"""Satellite-status strip viewer -- the port of ``view_satellite_strip.m``.

(satellite x epoch) raster of per-satellite status from
``satellite_status_trace.csv`` -- one tile per constellation, built on the shared
:func:`gnssviz.strip.build_sat_epoch_strip` renderer.  ``plot_value`` selects:
  * ``'status'``       -> categorical processing status (Clean/Pivot/...);
  * ``'elevation'``    -> elevation_deg [0..90] (viridis);
  * ``'signals_used'`` -> n_signals_used (viridis, auto-scaled).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult

# Status name -> stable colour (defined order = legend order).
_STATUS_CATS = [
    ("Clean", (0.20, 0.65, 0.30)),
    ("Pivot", (0.15, 0.45, 0.80)),
    ("BelowMask", (0.70, 0.70, 0.70)),
    ("NoProduct", (0.95, 0.60, 0.15)),
    ("NoModel", (0.80, 0.15, 0.15)),
    ("Excluded", (0.55, 0.10, 0.55)),
]


def build_satellite_strip(ctx, plot_value: str = "status") -> ViewerResult:
    df = load_trace(ctx.path("satellite_status_trace.csv"))
    require_cols(df, ("week", "tow_s", "satellite", "system"),
                 "satellite_status_trace.csv")
    df = add_utc(df)

    pv = plot_value.lower()
    if pv == "elevation":
        value = df["elevation_deg"].to_numpy(float)
        figs = build_sat_epoch_strip(
            _melt(df, value), mode="continuous", colormap="viridis", caxis=[0, 90],
            fig_title="Satellite elevation", value_label="Elevation (deg)",
            subtitle="low elevation = dark (slip / multipath danger band)",
            fig_tag="sat")
    elif pv == "signals_used":
        value = df["n_signals_used"].to_numpy(float)
        figs = build_sat_epoch_strip(
            _melt(df, value), mode="continuous", colormap="viridis", caxis="auto",
            fig_title="Signals used per satellite", value_label="n signals used",
            fig_tag="sat")
    elif pv == "status":
        names, colors, value = _status_categories(df)
        figs = build_sat_epoch_strip(
            _melt(df, value), mode="categorical",
            cat={"labels": names, "colors": colors},
            fig_title="Satellite processing status", value_label="status",
            fig_tag="sat")
    else:
        raise ValueError("plot_value must be 'status', 'elevation', or 'signals_used'")

    return ViewerResult(figures=figs)


def _melt(df: pd.DataFrame, value) -> pd.DataFrame:
    """Long-form table for the strip renderer; drop non-finite values."""
    d = pd.DataFrame({
        "system": df["system"].astype(str),
        "satellite": df["satellite"].astype(str),
        "facet": "",
        "utc": df["utc"],
        "value": np.asarray(value, dtype=float),
    })
    return d[np.isfinite(d["value"].to_numpy(float))]


def _status_categories(df: pd.DataFrame):
    """Map status strings to stable 1..K indices; unknowns appended as grey."""
    status = df["status"].astype(str)
    names = [nm for (nm, _) in _STATUS_CATS if (status == nm).any()]
    colors = [c for (nm, c) in _STATUS_CATS if nm in names]
    extras = [s for s in dict.fromkeys(status) if s not in names and s not in ("", "nan")]
    names += extras
    colors += [(0.4, 0.4, 0.4)] * len(extras)

    value = np.zeros(len(df))
    for k, nm in enumerate(names, start=1):
        value[(status == nm).to_numpy()] = k
    value[value == 0] = np.nan          # unknown -> transparent
    return names, np.array(colors, float), value


viewer = Viewer(name="satellite_strip", title="Satellite status strip",
                required_csvs=("satellite_status_trace.csv",),
                build=build_satellite_strip)
