"""NRCAN-style satellite-status viewer -- the port of ``plotSatelliteStatus.m``.

One figure per constellation: a (satellite x epoch) raster coloured by the
integer ``status_code`` (0 Clean, 1 Pivot, 2 Slip, 3 CodeOutlier, 4 BelowMask,
5 reserved, 6 NoProduct, 7 NoModel) through a fixed 8-colour discrete map, a thin
per-row elevation overlay, and a discrete colorbar (code 5 hidden).  Differs from
``satellite_strip`` (status mode) by using the numeric code map directly and the
elevation lane overlay.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..identifiers import prn_num, system_of
from ..io import add_utc, load_trace, require_cols
from ..figures import Axes, ColorBar, Figure, Line, Raster
from .base import Viewer, ViewerResult

# status_code -> RGB (indices 0..7; 5 is the reserved near-white sentinel).
_STATUS_COLORS = np.array([
    [0.20, 0.65, 0.30],   # 0 Clean
    [0.15, 0.45, 0.85],   # 1 Pivot
    [0.85, 0.20, 0.20],   # 2 Slip
    [0.95, 0.55, 0.10],   # 3 CodeOutlier
    [0.40, 0.40, 0.40],   # 4 BelowMask
    [0.97, 0.97, 0.97],   # 5 (reserved)
    [0.55, 0.80, 0.95],   # 6 NoProduct
    [0.80, 0.30, 0.70],   # 7 NoModel
])
_TICKS = [0, 1, 2, 3, 4, 6, 7]
_TICK_LABELS = ["Clean", "Pivot", "Slip", "CodeOutlier", "BelowMask",
                "NoProduct", "NoModel"]
_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")


def build_satellite_status(ctx) -> ViewerResult:
    df = load_trace(ctx.path("satellite_status_trace.csv"))
    require_cols(df, ("week", "tow_s", "satellite", "status_code"),
                 "satellite_status_trace.csv")
    df = add_utc(df)
    df["satellite"] = df["satellite"].astype(str)
    if "system" in df.columns:
        df["system"] = df["system"].astype(str)
    else:
        df["system"] = df["satellite"].map(system_of)

    epochs = np.sort(pd.unique(df["utc"]))
    epoch_pos = {int(k): j for j, k in enumerate(
        epochs.astype("datetime64[ns]").astype("int64"))}
    n_epochs = epochs.size
    has_elev = "elevation_deg" in df.columns

    # "Used" systems only: a constellation present in the trace but entirely
    # NoModel (code 7) -- e.g. an active_systems entry with no signal_priorities,
    # so every satellite is skipped -- carries no information and is dropped from
    # the default view.  Keep a system if it has any non-NoModel sample (which
    # subsumes "has any Clean data").
    _NOMODEL = 7
    present_all = [s for s in _SYS_ORDER if (df["system"] == s).any()]
    sys_present = [
        s for s in present_all
        if np.any(df.loc[df["system"] == s, "status_code"].to_numpy(float) != _NOMODEL)
    ]
    hidden = [s for s in present_all if s not in sys_present]
    figures = []
    for sys_name in sys_present:
        d = df[df["system"] == sys_name]
        sats = _prn_sort(d["satellite"].unique())
        n_sats = len(sats)
        sat_pos = {s: i for i, s in enumerate(sats)}

        grid = np.full((n_sats, n_epochs), np.nan)
        si = d["satellite"].map(sat_pos).to_numpy()
        ei = pd.Series(d["utc"].to_numpy("datetime64[ns]").astype("int64")).map(epoch_pos).to_numpy()
        vv = d["status_code"].to_numpy(float)
        ok = ~pd.isna(si) & ~pd.isna(ei)
        grid[si[ok].astype(int), ei[ok].astype(int)] = vv[ok]

        traces = [Raster(x=epochs, row_labels=sats, grid=grid)]
        if has_elev:                            # thin per-row elevation lane
            for k, sat in enumerate(sats):
                ds = d[d["satellite"] == sat].sort_values("utc")
                elev = ds["elevation_deg"].to_numpy(float)
                elev = np.where(np.isfinite(elev), elev, np.nan)
                y = (k + 1) - ((elev / 90.0) - 0.5) * 0.9
                traces.append(Line(x=ds["utc"], y=y, color=(0.15, 0.15, 0.15),
                                   width=0.4, show_in_legend=False))

        ax = Axes(traces=traces, xlabel="GPS Time", ylabel="Satellite",
                  title=f"{sys_name} satellite status  (epochs={n_epochs}, sats={n_sats})")
        figures.append(Figure(
            name=f"Sat Status -- {sys_name}",
            axes=[ax], width=14, height=min(9.5, 0.34 * n_sats + 1.8),
            cmap=_STATUS_COLORS, clim=(-0.5, 7.5),
            colorbar=ColorBar(label="Status", ticks=_TICKS, tick_labels=_TICK_LABELS)))

    return ViewerResult(figures=figures, summary=_summary(df, sys_present, epochs, hidden))


def _prn_sort(sats):
    uniq = list(dict.fromkeys(str(s) for s in sats))
    return sorted(uniq, key=lambda s: (0, prn_num(s)) if prn_num(s) is not None
                  else (1, uniq.index(s)))


def _summary(df, sys_present, epochs, hidden=()) -> str:
    out = ["=== Satellite Status Summary ===",
           f"Epochs     : {epochs.size}",
           f"Span (GPS) : {pd.Timestamp(epochs[0])} -> {pd.Timestamp(epochs[-1])}",
           "",
           "=== Per-Constellation Epoch Health (mean sats/epoch) ===",
           f"{'System':<9}{'Clean':>9}{'Pivot':>9}{'Slip':>9}{'COutl':>9}"
           f"{'BMask':>9}{'NoProd':>9}{'NoMod':>9}",
           "-" * 72]
    for sys_name in sys_present:
        d = df[df["system"] == sys_name]
        ne = d["utc"].nunique()
        code = d["status_code"].to_numpy(float)
        def per_epoch(c):
            return np.sum(code == c) / ne if ne else 0.0
        out.append(f"{sys_name:<9}" + "".join(
            f"{per_epoch(c):>9.2f}" for c in (0, 1, 2, 3, 4, 6, 7)))
    if hidden:
        out.append("")
        out.append(f"Hidden (present but all NoModel): {', '.join(hidden)}")
    return "\n".join(out)


viewer = Viewer(name="satellite_status", title="Satellite status (NRCAN-style)",
                required_csvs=("satellite_status_trace.csv",),
                build=build_satellite_status)
