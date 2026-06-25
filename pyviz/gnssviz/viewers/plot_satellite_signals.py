"""Per-epoch, per-satellite SIGNAL-usage strip -- the port of
``plotSatelliteSignals.m``.

Companion to :mod:`gnssviz.viewers.satellite_status`.  Where that viewer answers
"was the satellite used?" (one row per sat, status raster), this one resolves the
usage into the actual signals tracked -- which frequency BANDS and which tracking
CHANNELS (RINEX attributes) each satellite contributed at every epoch.

Data source
-----------
``measurement_residuals.csv`` -- the same file the residual viewer reads.  It
carries one row per (epoch, satellite, signal) for every signal the filter
actually USED in its update, so a signal's presence in the file == "this signal
was used this epoch".

Required columns (read by name; order irrelevant)::

    week, tow_s     GPS week + time-of-week  -> epoch axis
    satellite       e.g. "G01"  (BSSD "E02-E36" -> primary "E02")
    signal          RINEX-3 obs code, e.g. "L1C", "C2W", "L5X"

Each obs code decomposes as ``<type><band><attribute>``:

* ``type``    L/C/D/S       carrier phase / code / doppler / SNR
* ``band``    the digit     1,2,5,6,7,8,9   -> drives COLOR
* ``channel`` attribute     C,W,Q,X,I,L,... -> the "many of them" dimension

``L1C`` and ``C1C`` are the SAME signal (band 1, channel C) measured two ways, so
we collapse on (band, channel) and treat type as a sub-dimension.

Visual encoding -- the channel problem
--------------------------------------
There are far too many tracking channels to give each its own colour, so each
satellite is split into one thin sub-row per band it tracks; a cell is filled
when that (sat, band) was used at that epoch.  The band is named in the ROW LABEL
("G01 L1" / "G01 L2"), which frees the cell COLOUR to carry the channel
dimension.  How a cell is coloured is selectable via ``channel_encoding``:

* ``'count'``   (default) DISCRETE colour = how many distinct tracking channels
  were used in that band this epoch (1=yellow 2=orange 3=blue 4=cyan 5+=purple).
* ``'shade'``   COLOUR = frequency band, brightness = #channels (light = 1).
* ``'obstype'`` COLOUR = band, brightness = phase-only / code-only / both.
* ``'letters'`` COLOUR = band; attribute letters drawn inside each cell
  (auto-hidden above ``max_label_epochs`` epochs -- the matplotlib backend has no
  per-cell text primitive, so this falls back to ``'shade'`` shading; see
  ``missing_primitives``).

Layout switch
-------------
* ``'per_system'`` (default): one figure per constellation (mirrors
  ``satellite_status``).
* ``'single'``     : every constellation stacked into one figure.

The console tally (``showChannelTally`` in MATLAB) is reproduced in the viewer
summary.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..figures import Axes, Figure, Legend, RgbRaster, Scatter
from .base import Viewer, ViewerResult

# --- Per-band colour palette (by RINEX band digit) -------------------------- #
# Harmonized by frequency where the constellations agree (band 5 ~1176 MHz for
# GPS L5 / Galileo E5a / BeiDou B2a, etc.).  '0' is the gray fallback.
_BAND_COLORS = {
    "1": (0.00, 0.45, 0.74), "2": (0.85, 0.33, 0.10), "3": (0.49, 0.18, 0.56),
    "4": (0.80, 0.30, 0.65), "5": (0.20, 0.63, 0.17), "6": (0.64, 0.08, 0.18),
    "7": (0.93, 0.69, 0.13), "8": (0.20, 0.70, 0.90), "9": (0.55, 0.35, 0.20),
    "0": (0.50, 0.50, 0.50),
}

# --- Discrete channel-COUNT palette (channel_encoding='count') -------------- #
# Row N (1-based) is the colour for "N distinct channels used in this band this
# epoch".  Counts at or above the last row clamp to it.
_COUNT_COLORS = np.array([
    [0.95, 0.85, 0.15],   # 1 channel  -> yellow
    [0.95, 0.55, 0.10],   # 2 channels -> orange
    [0.15, 0.45, 0.85],   # 3 channels -> blue
    [0.30, 0.78, 0.90],   # 4 channels -> cyan
    [0.55, 0.25, 0.70],   # 5+ channels-> purple
])

# Constellations present, in canonical render order: (letter, name).
_SYS_DEFS = [
    ("G", "GPS"), ("E", "Galileo"), ("R", "GLONASS"), ("C", "BeiDou"),
    ("J", "QZSS"), ("I", "NavIC"), ("S", "SBAS"),
]
_SYS_RANK = {letter: i for i, (letter, _) in enumerate(_SYS_DEFS)}

# Constellation-aware band names for a RINEX band digit (matches MATLAB).
_BAND_LABELS = {
    "G": {"1": "L1", "2": "L2", "5": "L5"},
    "R": {"1": "G1", "2": "G2", "3": "G3", "4": "G1a", "6": "G2a"},
    "E": {"1": "E1", "5": "E5a", "6": "E6", "7": "E5b", "8": "E5"},
    "C": {"1": "B1", "2": "B1I", "5": "B2a", "6": "B3", "7": "B2b", "8": "B2"},
    "J": {"1": "L1", "2": "L2", "5": "L5", "6": "L6"},
    "I": {"5": "L5", "9": "S"},
    "S": {"1": "L1", "5": "L5"},
}


def _band_label(sys_char: str, band_digit: str) -> str:
    """Constellation-aware band name; falls back to ``B<digit>``."""
    return _BAND_LABELS.get(sys_char, {}).get(band_digit, f"B{band_digit}")


def _band_color(band_digit: str):
    """Band-digit -> RGB, gray fallback for unknown digits."""
    return _BAND_COLORS.get(band_digit, (0.5, 0.5, 0.5))


def build_plot_satellite_signals(
    ctx,
    layout: str = "per_system",
    channel_encoding: str = "count",
    max_label_epochs: int = 150,
    show_legend: bool = True,
    show_channel_tally: bool = True,
) -> ViewerResult:
    """Render the signal-usage raster(s).

    Parameters mirror the MATLAB switches; defaults match the script
    (``layout='per_system'``, ``channel_encoding='count'``).
    """
    fname = ctx.path("measurement_residuals.csv")
    df = load_trace(fname)
    df.columns = [c.lower() for c in df.columns]
    require_cols(df, ("week", "tow_s", "satellite", "signal"),
                 "measurement_residuals.csv")

    # --- Strip BSSD compound IDs ("E02-E36" / "G05/G03" -> primary) -------- #
    sat = df["satellite"].astype(str)
    for sep in ("-", "/"):
        sat = sat.str.split(sep, n=1).str[0]
    df = df.copy()
    df["satellite"] = sat.to_numpy()

    # --- Decompose the signal code into type / band / channel ------------- #
    sig = df["signal"].astype(str)
    obstype = sig.str.slice(0, 1)
    band = sig.str.slice(1, 2)
    chan = sig.str.slice(2)

    # band must be a single digit (str2double of MATLAB's extractBetween).
    good = band.str.fullmatch(r"\d").fillna(False).to_numpy()
    n_bad = int((~good).sum())
    df = df.loc[good].copy()
    df["obstype"] = obstype.to_numpy()[good]
    df["band"] = band.to_numpy()[good]
    df["chan"] = chan.to_numpy()[good]
    df["code"] = sig.to_numpy()[good]
    df["sys_char"] = df["satellite"].str.slice(0, 1)

    # --- GPS-time epoch axis (trace files carry pure GPS time) ------------ #
    df = add_utc(df)
    epochs_all = np.sort(pd.unique(df["utc"]))
    epoch_pos = {int(k): j for j, k in enumerate(
        epochs_all.astype("datetime64[ns]").astype("int64"))}
    df["epoch_idx"] = pd.Series(
        df["utc"].to_numpy("datetime64[ns]").astype("int64")).map(epoch_pos).to_numpy()
    n_epoch_all = epochs_all.size

    enc = str(channel_encoding).lower()

    # --- Constellations present, canonical order -------------------------- #
    present = [(c, nm) for (c, nm) in _SYS_DEFS if (df["sys_char"] == c).any()]
    if not present:
        raise ValueError("plot_satellite_signals: no recognized constellations in CSV.")

    figures = []
    if layout.lower() == "single":
        fig = _build_signal_figure(df, "All constellations", "", epochs_all, enc,
                                   max_label_epochs, show_legend)
        if fig is not None:
            figures.append(fig)
    else:
        for sys_char, sys_name in present:
            d = df[df["sys_char"] == sys_char]
            if d.empty:
                continue
            fig = _build_signal_figure(d, sys_name, sys_char, epochs_all, enc,
                                       max_label_epochs, show_legend)
            if fig is not None:
                figures.append(fig)

    summary = ""
    if show_channel_tally:
        summary = _tally(df, fname, epochs_all, n_epoch_all, n_bad)

    return ViewerResult(figures=figures, summary=summary)


def _build_signal_figure(d, sys_name, sys_char, epochs_all, enc,
                         max_label_epochs, show_legend):
    """Render one signal-usage raster: rows grouped by sat, one sub-row per band."""
    n_epoch = epochs_all.size

    # Satellites, PRN-sorted within each constellation.
    sats = pd.unique(d["satellite"])
    def _sat_key(s):
        rank = _SYS_RANK.get(str(s)[0], len(_SYS_DEFS))
        try:
            prn = int(str(s)[1:])
        except ValueError:
            prn = 0
        return (rank, prn)
    sats = sorted(map(str, sats), key=_sat_key)

    # One display row per (satellite, band-it-uses).
    row_sat, row_band, row_label = [], [], []
    for s in sats:
        bset = sorted(pd.unique(d.loc[d["satellite"] == s, "band"]))
        for b in bset:
            row_sat.append(s)
            row_band.append(b)
            row_label.append(f"{s} {_band_label(str(s)[0], b)}")
    n_row = len(row_sat)
    if n_row == 0:
        return None

    # Per-cell aggregates: distinct-channel count, obs-type code (1 phase /
    # 2 code / 3 both), and the exact-codes / attribute strings.
    count_grid = np.zeros((n_row, n_epoch))
    type_grid = np.zeros((n_row, n_epoch))
    tip_grid = np.full((n_row, n_epoch), "", dtype=object)
    attr_grid = np.full((n_row, n_epoch), "", dtype=object)

    for r in range(n_row):
        m = (d["satellite"] == row_sat[r]) & (d["band"] == row_band[r])
        sub = d[m]
        if sub.empty:
            continue
        for e, g in sub.groupby("epoch_idx"):
            ei = int(e)
            ch = g["chan"]
            ot = g["obstype"]
            cd = g["code"]
            count_grid[r, ei] = ch.nunique()
            has_l = (ot == "L").any()
            has_c = (ot == "C").any()
            type_grid[r, ei] = (1 * (has_l and not has_c)
                                + 2 * (not has_l and has_c)
                                + 3 * (has_l and has_c))
            # unique() preserves first-seen order (MATLAB unique sorts, but the
            # join is only for the data-tip text -- order is cosmetic).
            tip_grid[r, ei] = "  ".join(sorted(pd.unique(cd)))
            attr_grid[r, ei] = "".join(sorted(pd.unique(ch)))

    used = count_grid > 0

    # Per-band shade cap (so a band that maxes at 2 channels still spans the
    # full light->dark range instead of looking uniformly dim).
    cap_map = {}
    for b in dict.fromkeys(row_band):    # stable unique without pandas coercion
        rows = np.array([rb == b for rb in row_band])
        sub = count_grid[rows, :]
        cap_map[b] = max(1.0, float(sub.max()) if sub.size else 1.0)

    n_count_lvl = _COUNT_COLORS.shape[0]
    enc_desc = {
        "count": "color = # channels",
        "letters": "color = band, attribute letters",
        "obstype": "color = band, shade = phase/code/both",
    }.get(enc, "color = band, shade = # channels")

    # Compose the truecolor image, blended over white (empties alpha'd out).
    img = np.ones((n_row, n_epoch, 3))
    if enc == "count":
        for r in range(n_row):
            cnt = count_grid[r, :]
            for e in np.flatnonzero(used[r, :]):
                lvl = min(int(cnt[e]), n_count_lvl)      # clamp 5,6,... to top
                img[r, e, :] = _COUNT_COLORS[lvl - 1, :]
    else:
        for r in range(n_row):
            c = _band_color(row_band[r])
            nz = used[r, :]
            a = np.zeros(n_epoch)
            if enc == "obstype":
                v = type_grid[r, :]
                pos = v > 0
                a[pos] = 0.45 + 0.55 * (v[pos] - 1) / 2.0   # 1->.45 2->.725 3->1
            elif enc == "letters":
                a[nz] = 0.85
            else:  # 'shade'
                cap = cap_map[row_band[r]]
                cnt = count_grid[r, :]
                a[nz] = 0.5 + 0.5 * np.minimum(cnt[nz] - 1, cap - 1) / max(cap - 1, 1)
            a = np.clip(a, 0.0, 1.0)
            for ch in range(3):
                img[r, :, ch] = (1 - a) * 1.0 + a * c[ch]

    alpha = used.astype(float)

    traces = [RgbRaster(x=epochs_all, row_labels=row_label, rgb=img, alpha=alpha)]

    # Legend (dummy off-screen scatters carry the colours, like MATLAB's dummy
    # patches).  'count' mode keys the channel-count palette; otherwise the
    # band-colour key.
    if show_legend:
        if enc == "count":
            for i in range(n_count_lvl):
                if i == n_count_lvl - 1:
                    nm = f"{i + 1}+ channels"
                elif i == 0:
                    nm = "1 channel"
                else:
                    nm = f"{i + 1} channels"
                traces.append(_legend_swatch(_COUNT_COLORS[i, :], nm))
        else:
            bs = sorted(pd.unique(row_band))
            for b in bs:
                nm = (f"Band {b}" if sys_char == "" else _band_label(sys_char, b))
                traces.append(_legend_swatch(_band_color(b), nm))

    ax = Axes(
        traces=traces,
        xlabel="GPS Time",
        ylabel="Satellite / band",
        title=f"{sys_name} signals used   ({enc_desc})   "
              f"[{len(sats)} sat, {n_epoch} epochs]",
        legend=(Legend(loc="upper left") if show_legend else None),
    )

    return Figure(
        name=f"Sat Signals -- {sys_name}",
        axes=[ax],
        width=14,
        height=max(2.8, 0.18 * n_row + 1.4),
    )


def _legend_swatch(color, label):
    """An off-screen scatter point that contributes only a coloured legend key."""
    return Scatter(x=[], y=[], color=tuple(color), label=label,
                   marker="s", size=80.0, show_in_legend=True)


def _tally(df, fname, epochs_all, n_epoch_all, n_bad):
    """Reproduce the MATLAB console tally as the viewer summary text."""
    lines = ["=== Signal usage tally ===",
             f"File   : {fname}",
             f"Epochs : {n_epoch_all}   Span: {pd.Timestamp(epochs_all[0])} -> "
             f"{pd.Timestamp(epochs_all[-1])}"]
    if n_bad:
        lines.append(f"(dropped {n_bad} row(s) whose signal code is not "
                     f"<type><band><attr>)")
    lines += ["",
              f"{'code':<6} {'obs':<10} band|channel",
              "-" * 34]
    codes = df["code"].to_numpy()
    uc, cnt = np.unique(codes, return_counts=True)
    for code, n in zip(uc, cnt):
        bc = f"{code[1:2]}|{code[2:]}"
        lines.append(f"{code:<6} {int(n):<10d} {bc}")
    lines += ["",
              "Tip: click any cell in the figures to read the exact codes used."]
    return "\n".join(lines)


viewer = Viewer(name="plot_satellite_signals",
                title="Satellite signal-usage strip",
                required_csvs=("measurement_residuals.csv",),
                build=build_plot_satellite_signals)
