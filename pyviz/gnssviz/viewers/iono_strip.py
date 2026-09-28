"""Slant-ionosphere strip viewer: the port of ``view_iono_strip.m``.

(satellite x epoch) raster of the per-satellite slant ionosphere state from
``ionosphere_trace.csv``: the estimate, its standard deviation, or the
vertical TEC.  One tile per constellation (no signal dimension), built on the
shared :func:`gnssviz.strip.build_sat_epoch_strip` renderer.

The iono-sigma raster reinflates at slips the same way the ambiguity sigma
does, giving an independent check on slip resets, and the iono raster shows
the active-ionosphere periods that gate GF slip trust (the TurboEdit IVI cut).
PPP-RTK (uncombined) runs only.

``plot_value`` selects the content (MATLAB default ``'std'``):
  * ``'iono'`` -> slant iono delay (m), signed about 0 (vik diverging, autosym);
  * ``'std'``  -> iono estimate sigma (m), high = hot (viridis_r, auto);
  * ``'vtec'`` -> vertical TEC mapped to range (m) (viridis, auto).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult


def build_iono_strip(ctx, plot_value: str = "std") -> ViewerResult:
    df = load_trace(ctx.path("ionosphere_trace.csv"))
    require_cols(df, ("week", "tow_s", "satellite", "system", "iono_m", "std_m"),
                 "ionosphere_trace.csv")
    df = add_utc(df)

    pv = plot_value.lower()
    if pv == "iono":
        value = df["iono_m"].to_numpy(float)
        figs = build_sat_epoch_strip(
            _melt(df, value), mode="continuous", colormap="vik",
            diverging=True, caxis="autosym",
            fig_title="Slant ionosphere estimate", value_label="Slant iono (m)",
            fig_tag="iono")
    elif pv == "std":
        value = df["std_m"].to_numpy(float)
        figs = build_sat_epoch_strip(
            _melt(df, value), mode="continuous", colormap="viridis_r",
            caxis="auto",
            fig_title=r"Ionosphere estimate $\sigma$", value_label=r"Iono $\sigma$ (m)",
            fig_tag="iono")
    elif pv == "vtec":
        if "vtec_m" not in df.columns:
            raise ValueError("vtec_m column not present in ionosphere_trace.csv")
        value = df["vtec_m"].to_numpy(float)
        figs = build_sat_epoch_strip(
            _melt(df, value), mode="continuous", colormap="viridis",
            caxis="auto",
            fig_title="Vertical TEC (range)", value_label="VTEC (m)",
            fig_tag="iono")
    else:
        raise ValueError("plot_value must be 'iono', 'std', or 'vtec'")

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


viewer = Viewer(name="iono_strip", title="Ionosphere strip",
                required_csvs=("ionosphere_trace.csv",),
                build=build_iono_strip)
