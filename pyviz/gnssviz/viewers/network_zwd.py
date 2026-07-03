"""Network per-station ZWD viewer -- the tropospheric wet-delay time series.

Reads ``network_zwd.csv`` (the PPP-RTK NETWORK producer's per-epoch, per-station
zenith wet delay: the estimated ZWD, its formal sigma, and a ``carried`` flag
marking epochs where the station's ZWD state was *carried* forward -- i.e. held
across a data gap rather than re-observed) and shows, over the run:

  1. **ZWD** -- per-station wet delay (m) with a shaded ±sigma envelope; the
     legend is capped so a large network stays readable.
  2. **Sigma** -- the per-station formal ZWD sigma over time, with the
     ``carried`` windows shaded.  This is the V7 axis: a *carried* stretch
     should show sigma *sharpening* (shrinking) as the carry tightens the state,
     the diagnostic the armed-validation V7 step keys on.
  3. **Station ranking** -- last-30-minute median sigma per station, ranked
     worst-to-best on a numeric x axis (TextBox station labels; no categorical
     xticks exist in the figure vocabulary).

A healthy network shows smooth ZWD, a bounded sigma, and carried windows that
tighten rather than balloon the wet-delay uncertainty.
"""

from __future__ import annotations

from typing import List

import numpy as np

from ..figures import Axes, Band, Figure, Legend, Line, Stem, TextBox
from ..io import add_utc, load_trace, require_cols
from ..palettes import distinguishable_colors
from .base import Viewer, ViewerResult, ViewerSkip

_LEGEND_CAP = 12          # stations shown in the ZWD legend before truncating
_RANK_WINDOW_S = 1800.0   # "last 30 minutes" for the ranking / summary window


def build_network_zwd(ctx) -> ViewerResult:
    if not ctx.has_data("network_zwd.csv"):
        raise ViewerSkip("network_zwd.csv is absent or has no data rows")
    df = load_trace(ctx.path("network_zwd.csv"))
    require_cols(df, ("week", "tow", "station", "zwd_m", "std_m", "carried"),
                 "network_zwd.csv")
    if df.empty:
        raise ViewerSkip("network_zwd.csv has no rows")

    df = df.copy()
    df["station"] = df["station"].astype(str)
    df = add_utc(df, sort=True)

    stations = sorted(df["station"].unique())
    if not stations:
        raise ViewerSkip("network_zwd.csv has no stations")
    colors = distinguishable_colors(len(stations))
    sta_color = {s: colors[i] for i, s in enumerate(stations)}

    # Per-station slices (time-sorted) reused across the axes.
    slices = {s: df[df["station"] == s].sort_values("utc") for s in stations}

    ax_zwd = _axis_zwd(slices, sta_color, stations)
    ax_sig = _axis_sigma(slices, sta_color, stations)
    ax_rank, rank = _axis_rank(slices, sta_color, stations)

    fig = Figure(name="Network ZWD",
                 title="PPP-RTK network per-station zenith wet delay",
                 axes=[ax_zwd, ax_sig, ax_rank],
                 width=12.0, height=10.5, link_x=False)

    return ViewerResult(figures=[fig], summary=_summary(df, rank))


# --------------------------------------------------------------------------- #
def _axis_zwd(slices, sta_color, stations) -> Axes:
    """Per-station ZWD line + ±sigma band; legend capped at _LEGEND_CAP."""
    ax = Axes(title="Zenith wet delay (± formal σ)", ylabel="ZWD (m)",
              xlabel="GPS Time", legend=Legend(loc="best"))
    traces = []
    for i, s in enumerate(stations):
        sub = slices[s]
        t = sub["utc"].to_numpy("datetime64[ns]")
        z = sub["zwd_m"].to_numpy(float)
        sig = sub["std_m"].to_numpy(float)
        col = sta_color[s]
        # Band first (drawn under the line).
        traces.append(Band(x=t, lo=z - sig, hi=z + sig, color=col, alpha=0.15,
                           show_in_legend=False))
        traces.append(Line(x=t, y=z, color=col, width=1.1,
                           label=s, show_in_legend=(i < _LEGEND_CAP)))
    ax.traces = traces
    return ax


def _axis_sigma(slices, sta_color, stations) -> Axes:
    """Per-station σ vs time (the V7 axis) with carried windows shaded.

    No VSpan primitive exists, so the carried windows are drawn as a Band that
    spans the current σ y-range where ``carried==1`` and is ``NaN`` elsewhere
    (NaN breaks the band, leaving un-carried stretches open), plus a 0/1 Stem
    at the bottom marking carried epochs unambiguously.
    """
    ax = Axes(title="Per-station ZWD σ — carried windows shaded (V7)",
              ylabel="ZWD σ (m)", xlabel="GPS Time", legend=Legend(loc="best"))
    traces = []

    # Global σ range to size the carried-window shading band.
    all_sig = np.concatenate(
        [slices[s]["std_m"].to_numpy(float) for s in stations]) if stations else np.array([])
    finite = all_sig[np.isfinite(all_sig)]
    sig_hi = float(finite.max()) if finite.size else 1.0
    sig_lo = float(finite.min()) if finite.size else 0.0
    if not (sig_hi > sig_lo):
        sig_hi = sig_lo + 1.0

    # One shading band spanning ALL stations' carried epochs (union), so the
    # V7 "shaded windows" read at a glance regardless of station count.
    t_all = np.sort(np.unique(np.concatenate(
        [slices[s]["utc"].to_numpy("datetime64[ns]") for s in stations])))
    pos = {int(k): j for j, k in enumerate(t_all.astype("int64"))}
    carried_any = np.zeros(t_all.size, dtype=bool)
    for s in stations:
        sub = slices[s]
        ts = sub["utc"].to_numpy("datetime64[ns]").astype("int64")
        cw = sub["carried"].to_numpy(float) > 0
        for k, c in zip(ts, cw):
            if c:
                carried_any[pos[int(k)]] = True
    if carried_any.any():
        lo = np.where(carried_any, sig_lo, np.nan)
        hi = np.where(carried_any, sig_hi, np.nan)
        traces.append(Band(x=t_all, lo=lo, hi=hi, color=(0.80, 0.80, 0.30),
                           alpha=0.22, label="carried window", show_in_legend=True))

    for i, s in enumerate(stations):
        sub = slices[s]
        t = sub["utc"].to_numpy("datetime64[ns]")
        sig = sub["std_m"].to_numpy(float)
        traces.append(Line(x=t, y=sig, color=sta_color[s], width=1.1,
                           label=s, show_in_legend=(i < _LEGEND_CAP)))

    # A 0/1 stem of the carried union at the bottom, unambiguous even when the
    # shading band overlaps a busy σ plot.
    if carried_any.any():
        stem_level = sig_lo + 0.04 * (sig_hi - sig_lo)
        traces.append(Stem(x=t_all[carried_any],
                           y=np.full(int(carried_any.sum()), stem_level),
                           baseline=sig_lo, color=(0.55, 0.45, 0.10),
                           marker="o", label="carried"))
    ax.traces = traces
    return ax


def _axis_rank(slices, sta_color, stations):
    """Station-ranked last-30-min median σ on a numeric x axis (no cat xticks).

    Returns ``(Axes, rank)`` where ``rank`` is a list of ``(station, med_sigma,
    carried_frac)`` sorted worst (largest σ) first.
    """
    rank = []
    for s in stations:
        sub = slices[s]
        t = sub["utc"].to_numpy("datetime64[ns]")
        if t.size == 0:
            continue
        t_end = t.max()
        window0 = t_end - np.timedelta64(int(_RANK_WINDOW_S), "s")
        in_win = t >= window0
        sig = sub["std_m"].to_numpy(float)[in_win]
        carried = sub["carried"].to_numpy(float)[in_win] > 0
        sig = sig[np.isfinite(sig)]
        med = float(np.median(sig)) if sig.size else float("nan")
        cfrac = float(carried.mean() * 100.0) if carried.size else 0.0
        rank.append((s, med, cfrac))

    # Worst (largest median σ) first; NaN medians sort last.
    rank.sort(key=lambda r: (np.isnan(r[1]), -(r[1] if not np.isnan(r[1]) else 0.0)))

    ax = Axes(title=f"Last-{int(_RANK_WINDOW_S / 60)}-min median ZWD σ — worst first",
              ylabel="median σ (m)", xlabel="station rank", x_is_datetime=False,
              legend=None)
    xs = list(range(1, len(rank) + 1))
    ys = [(r[1] if not np.isnan(r[1]) else 0.0) for r in rank]
    traces = []
    for x, (s, med, _cfrac) in zip(xs, rank):
        col = sta_color.get(s, (0.4, 0.4, 0.4))
        y = med if not np.isnan(med) else 0.0
        # A vertical "bar" via a Stem (no Bar primitive), coloured per station.
        traces.append(Stem(x=[x], y=[y], baseline=0.0, color=col, marker="o"))
        traces.append(TextBox(x=x, y=y, text=s, ha="center", va="bottom",
                              data_coords=True, fontsize=7, mono=True))
    ax.traces = traces
    if xs:
        ax.xlim = (0.5, len(xs) + 0.5)
    return ax, rank


def _summary(df, rank) -> str:
    """`per-station sigma (last 30 min): median ...; worst <STA> ...; carried ...%`."""
    meds = [r[1] for r in rank if not np.isnan(r[1])]
    n_sta = df["station"].nunique()
    carried_pct = float((df["carried"].to_numpy(float) > 0).mean() * 100.0) if len(df) else 0.0

    out = ["=== PPP-RTK Network Per-Station ZWD ==="]
    out.append(f"Stations : {n_sta}   Epochs : {df['utc'].nunique()}")
    if meds:
        overall_med = float(np.median(meds))
        worst_sta, worst_med, worst_cfrac = rank[0]
        line = (f"per-station sigma (last 30 min): median {overall_med:.4f} m; "
                f"worst {worst_sta} {worst_med:.4f} m; "
                f"carried {carried_pct:.1f}%")
    else:
        line = (f"per-station sigma (last 30 min): median n/a; worst n/a; "
                f"carried {carried_pct:.1f}%")
    out.append(line)

    out.append("")
    out.append(f"{'Station':<10} {'medσ(m)':>10} {'carried%':>10}")
    out.append("-" * 32)
    for s, med, cfrac in rank:
        med_s = f"{med:>10.4f}" if not np.isnan(med) else f"{'n/a':>10}"
        out.append(f"{s:<10} {med_s} {cfrac:>10.1f}")
    return "\n".join(out)


viewer = Viewer(name="network_zwd",
                title="Network per-station ZWD (wet delay & carried σ)",
                required_csvs=("network_zwd.csv",),
                build=build_network_zwd)
