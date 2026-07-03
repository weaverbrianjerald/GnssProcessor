"""Network structured QC-event timeline -- the L2 ``network_events.csv`` log.

Reads ``network_events.csv`` (one long-format row per discrete QC action the
PPP-RTK NETWORK producer took) and renders the events as glyphs over a
per-satellite lattice, so a lock-in / storm of refloats / a pivot handover is
visible at a glance and attributable to a satellite and a QC tier.

Tier vocabulary (the ``tier`` column):
  ``T1_if_slip`` ``T2_within_epoch`` ``T3_raim`` ``T4_attrib``
  ``T5_clock_model`` ``T6_clock_monitor`` ``GATE`` ``SIGMA0`` ``PIVOT`` ``RETRO``

Figure 1 -- "QC events by satellite" -- one Axes per present constellation (in
``SYSTEM_ORDER``).  Each tile is an all-NaN :class:`Raster` (rows = the tile's
satellite names, via ``row_labels`` -- the only y-tick facility for a raster; a
NaN grid paints nothing) with one :class:`Markers` layer per (tier-colour ×
action-shape) group.  Markers are raster-addressed by 0-based row index.  Shapes
map from the *event* name: refloat -> "x", withhold -> "v", retro_bump -> "s",
pivot_handover -> "D".  Colours map from the *tier*.

Figure 2 -- "Epoch disposition raster" -- OPTIONAL: drawn only when
``network_trace.csv`` is present, as a single-lane emitted/withheld strip
(degrades gracefully to nothing when that trace is absent).

The summary tallies events per tier, the top-5 most-flagged satellites, and the
disposition counts (from ``network_trace.csv`` when available).
"""

from __future__ import annotations

import numpy as np

from ..figures import (Axes, ColorBar, Figure, Legend, Markers, Raster,
                       TextBox)
from ..identifiers import system_of
from ..io import add_utc, load_trace, require_cols
from ..palettes import SYSTEM_ORDER, distinguishable_colors
from .base import Viewer, ViewerResult, ViewerSkip

# Fixed tier -> colour, drawn from the shared distinguishable-colour walk so the
# palette is stable and never a hand-rolled ramp.  10 tiers, 10 colours.
_TIERS = ("T1_if_slip", "T2_within_epoch", "T3_raim", "T4_attrib",
          "T5_clock_model", "T6_clock_monitor", "GATE", "SIGMA0",
          "PIVOT", "RETRO")
_TIER_COLORS = dict(zip(_TIERS, distinguishable_colors(len(_TIERS))))
_UNKNOWN_TIER_COLOR = (0.5, 0.5, 0.5)

# Event-name -> marker shape.  Substring match (event names carry variants like
# ``we_refloat_sat`` / ``consensus_refloat`` / ``clk_monitor_refloat``); order
# matters (first hit wins), so specific bumps/handovers precede the generic verbs.
_SHAPE_RULES = (
    ("retro_bump", "s"),
    ("handover", "D"),
    ("pivot", "D"),
    ("refloat", "x"),
    ("reset", "x"),
    ("withhold", "v"),
    ("exclude", "v"),
    ("suppress", "v"),
    ("wipe", "v"),
)
_DEFAULT_SHAPE = "o"


def _shape_for(event: str) -> str:
    e = str(event).lower()
    for token, shape in _SHAPE_RULES:
        if token in e:
            return shape
    return _DEFAULT_SHAPE


def _tier_color(tier: str):
    return _TIER_COLORS.get(str(tier), _UNKNOWN_TIER_COLOR)


def build_network_events(ctx) -> ViewerResult:
    df = load_trace(ctx.path("network_events.csv"))
    require_cols(df, ("week", "tow", "tier", "event", "sat", "station",
                      "statistic", "threshold", "action", "detail"),
                 "network_events.csv")
    if df.empty:
        raise ViewerSkip("network_events.csv has no rows")

    df = df.copy()
    df["tier"] = df["tier"].astype("string").fillna("")
    df["event"] = df["event"].astype("string").fillna("")
    df["sat"] = df["sat"].astype("string").fillna("")
    df["shape"] = df["event"].map(_shape_for)
    df = add_utc(df, sort=True)

    figures = [_events_by_sat(df)]

    # Figure 2: optional epoch-disposition raster from the aggregate trace.
    disp_summary = ""
    if ctx.has_data("network_trace.csv"):
        fig2, disp_summary = _disposition_raster(ctx)
        if fig2 is not None:
            figures.append(fig2)

    return ViewerResult(figures=figures,
                        summary=_summary(df, disp_summary))


# --------------------------------------------------------------------------- #
def _events_by_sat(df) -> Figure:
    """Figure 1: per-constellation all-NaN raster + tier/shape marker layers.

    Only satellite-attributed events (non-empty ``sat``) place a glyph; the
    system-wide events (empty sat -- e.g. GATE/SIGMA0 epoch withholds) are
    surfaced in the summary and, when the trace is present, the Figure-2 strip.
    """
    sat_df = df[df["sat"] != ""]
    axes_list = []

    if sat_df.empty:
        # No sat-attributed events at all: draw a single empty note tile so the
        # figure is still valid (the story is entirely system-wide).
        ax = Axes(title="QC events by satellite", axis_off=True, grid=False,
                  x_is_datetime=False,
                  traces=[TextBox(x=0.5, y=0.5,
                                  text="no satellite-attributed QC events\n"
                                       "(all events are epoch/system-wide)",
                                  ha="center", va="center", boxed=True)])
        return Figure(name="Network QC events", title="QC events by satellite",
                      axes=[ax], width=12.0, height=3.0, link_x=False)

    x_all = df["utc"].to_numpy("datetime64[ns]")
    sat_df = sat_df.assign(_system=sat_df["sat"].map(system_of))
    present = [s for s in SYSTEM_ORDER if (sat_df["_system"] == s).any()]

    for sys_name in present:
        d_sys = sat_df[sat_df["_system"] == sys_name]
        sats = _prn_sort(d_sys["sat"].tolist())
        row_of = {s: i for i, s in enumerate(sats)}
        n_sats = len(sats)

        # All-NaN backdrop grid: the row_labels are the ONLY y-tick facility; the
        # grid paints nothing (all transparent).  Columns span the full run time.
        grid = np.full((n_sats, max(1, x_all.size)), np.nan)
        traces = [Raster(x=(x_all if x_all.size else d_sys["utc"].to_numpy()),
                         row_labels=sats, grid=grid)]

        # One Markers layer per (tier, shape) group so colour == tier and
        # shape == action class, matching the legend.
        for (tier, shape), grp in d_sys.groupby(["tier", "shape"]):
            rows, xs = [], []
            for _, e in grp.iterrows():
                r = row_of.get(str(e["sat"]))
                if r is not None:
                    rows.append(r)
                    xs.append(e["utc"])
            if not rows:
                continue
            traces.append(Markers(
                x=np.array(xs, dtype="datetime64[ns]"), rows=rows,
                marker=str(shape), color=_tier_color(tier), size=7.0,
                label=f"{tier} [{shape}]"))

        axes_list.append(Axes(
            title=sys_name, ylabel="satellite", xlabel="GPS Time",
            legend=Legend(loc="upper right"), grid=True,
            traces=traces, x_is_datetime=True))

    return Figure(name="Network QC events",
                  title="QC events by satellite (colour = tier, shape = action)",
                  axes=axes_list, width=13.0,
                  height=min(11.0, 2.2 * len(axes_list) + 1.0), link_x=True)


def _disposition_raster(ctx):
    """Figure 2: single-lane emitted/withheld strip from network_trace.csv.

    Returns ``(Figure|None, summary_text)``.  Degrades to ``(None, "")`` if the
    trace lacks the ``solved`` column (older schema) or has no rows.
    """
    try:
        tr = load_trace(ctx.path("network_trace.csv"))
    except Exception:
        return None, ""
    if tr.empty or "solved" not in tr.columns:
        return None, ""
    tr = add_utc(tr, sort=True)
    t = tr["utc"].to_numpy("datetime64[ns]")
    solved = tr["solved"].to_numpy(float) > 0

    # One-row raster: value 1 = emitted, 0 = withheld/skipped; NaN never occurs.
    grid = np.where(solved, 1.0, 0.0).reshape(1, -1)
    ax = Axes(title="Epoch disposition (emitted = 1, withheld = 0)",
              ylabel="", xlabel="GPS Time", grid=False, x_is_datetime=True,
              traces=[Raster(x=t, row_labels=["epoch"], grid=grid)])
    # Sequential map: viridis (never a hand-rolled ramp).
    from ..colormaps import strip_colormap
    fig = Figure(name="Network disposition strip",
                 title="Epoch disposition raster", axes=[ax],
                 width=13.0, height=2.2, link_x=True,
                 cmap=strip_colormap("viridis", 2), clim=(0.0, 1.0),
                 colorbar=ColorBar(label="", ticks=[0.0, 1.0],
                                   tick_labels=["withheld", "emitted"]),
                 categorical=True)
    n = len(tr)
    n_emit = int(solved.sum())
    txt = (f"Aggregate trace  : {n} epochs, {n_emit} emitted "
           f"({100.0 * n_emit / max(n, 1):.1f}%), {n - n_emit} withheld/skipped")
    return fig, txt


def _prn_sort(sats):
    uniq = list(dict.fromkeys(str(s) for s in sats))

    def key(s):
        try:
            return (0, int(s[1:3]))
        except (ValueError, IndexError):
            return (1, 0)
    return sorted(uniq, key=key)


def _summary(df, disp_summary) -> str:
    out = ["=== PPP-RTK Network QC Events ==="]
    out.append(f"Total events     : {len(df)}")
    out.append(f"Time span        : {df['utc'].min()} .. {df['utc'].max()}")

    out.append("")
    out.append("Events per tier:")
    counts = df["tier"].value_counts()
    for tier in _TIERS:
        c = int(counts.get(tier, 0))
        if c:
            out.append(f"  {tier:<16}: {c}")
    for tier, c in counts.items():
        if str(tier) not in _TIERS and str(tier) != "":
            out.append(f"  {str(tier):<16}: {int(c)}  (unrecognised tier)")

    sat_df = df[df["sat"] != ""]
    out.append("")
    out.append("Top-5 flagged satellites:")
    if sat_df.empty:
        out.append("  (none — all events are epoch/system-wide)")
    else:
        top = sat_df["sat"].value_counts().head(5)
        for sat, c in top.items():
            tiers = ", ".join(sorted(set(sat_df.loc[sat_df["sat"] == sat,
                                                    "tier"].astype(str))))
            out.append(f"  {str(sat):<6}: {int(c):>4}  [{tiers}]")

    # Action-class breakdown (by mapped shape) — a quick refloat/withhold tally.
    out.append("")
    out.append("Action classes (by shape):")
    shape_label = {"x": "refloat/reset", "v": "withhold/exclude",
                   "s": "retro_bump", "D": "pivot/handover", "o": "other"}
    sc = df["shape"].value_counts()
    for shp in ("x", "v", "s", "D", "o"):
        c = int(sc.get(shp, 0))
        if c:
            out.append(f"  {shape_label[shp]:<18}: {c}")

    if disp_summary:
        out.append("")
        out.append(disp_summary)
    return "\n".join(out)


viewer = Viewer(name="network_events",
                title="Network QC events (structured timeline)",
                required_csvs=("network_events.csv",),
                build=build_network_events)
