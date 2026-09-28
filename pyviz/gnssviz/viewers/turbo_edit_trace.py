"""TurboEdit (MW+GF joint slip resolver) viewer: the port of
``view_turbo_edit_trace.m``.

Reads ``turbo_edit_trace.csv``, one row per mature (satellite, phase-pair)
per epoch (the first two epochs after any arc (re)start emit no rows), and
draws the cycle-slip-repair diagnostics that mirror the look of the MW wide-lane
viewer: per-constellation columns, the constellation's own (sig1/sig2) phase
pairs stacked within each column, one coloured line per satellite, and a
mu/sigma annotation in the top-right corner of every tile.

Figures produced (guarded by the same content switches the MATLAB exposes, each
defaulting to its MATLAB default):

  * **TD2 grid** (``show_td2``): the geometry-free second time-difference
    ``td2_gf_m`` (m), the detector statistic. A slip ``(dN1,dN2)`` spikes it
    by ``lambda1*dN1 - lambda2*dN2`` at the slip epoch. One figure per
    constellation (a column), with the constellation's signal pairs stacked as
    vertically-linked axes (rows), one coloured line per satellite, NaN-broken at
    time gaps, plus a per-tile ``mu/sigma`` text read-out.
  * **IVI** (``show_ivi``): per-constellation cross-satellite ionospheric
    variability index ``median(|TD2|) + MAD-sigma(|TD2|)``, one line per
    constellation on a single axes with the 0.06 m iono-active gate. Built from
    the pre-filter snapshot (all rows, flagged included).
  * **Event examples** (``show_event_examples``): a two-panel zoom on one
    ``repair``, one ``reset`` and one ``code_event`` exemplar (each only if
    present in the trace). Top panel: raw GF with, for a repair, the
    ``lambda1*dN1 - lambda2*dN2`` step removed from the post-event samples (the
    "repaired" overlay). Bottom panel: TD2 against the +/- per-row threshold
    envelope, with the event sample starred.

The remaining MATLAB content switches (``showRatioPlot``, ``showThresholdPlot``,
``showRawGfPlot``, ``showLegend``) default to off / their MATLAB default and are
honoured by the keyword arguments to :func:`build_turbo_edit_trace` so a caller
can reproduce the alternate views.

Fidelity notes
--------------
* ``thresh_m`` and ``thresh_unfloored_m`` carry ``-1`` as the C++
  "no-threshold-yet" sentinel; thresholds are physically nonnegative so the
  sentinel is mapped to NaN (matching the MATLAB ``T.x(x<0)=NaN``).
* The flag columns (``iono_stable``, ``mw_flagged``, ``gf_flagged``,
  ``wl_known``) are coerced to numeric with NaN -> 0.
* ``hide_flagged_epochs`` (default ``True``) drops rows the resolver flagged
  (``mw_flagged | gf_flagged``) from the grid figures only, so they autoscale on
  in-arc noise; the IVI plot, the event inventory and the example figures are all
  built from the full ``Tall`` snapshot.
* Per-satellite colour is taken from the numeric PRN (chars 2-3) through the
  shared ``color_by_prn`` palette, the data-layer analogue of MATLAB's
  ``distinguishable_colors`` indexed by ``str2double(sat(2:3))``.
* Time gaps larger than ``gap_factor x median(dt)`` get a NaN break so lines do
  not cross them (the MATLAB ``insertGapBreaks``).
* ``std`` is the sample standard deviation (ddof=1), 0.0 for a single sample, to
  match MATLAB ``std``.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..palettes import color_by_prn, system_color
from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox, VLine
from .base import Viewer, ViewerResult

# Canonical constellation render order (matches the rest of the suite).
_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS", "IRNSS")

# Reference-line / gate constants (mirror the MATLAB literals).
_IVI_GATE = 0.06          # m, iono deemed active above this
_FLOOR_M = 0.04           # kMinThresholdM, the GF gate floor
_HALF_WIN = 60            # samples either side of an event in the zoom panels

# Colours used by the example panels (BGR-ish triples copied from the MATLAB).
_GF_BLUE = (0.0, 0.35, 0.75)
_REPAIR_RED = (0.75, 0.0, 0.1)
_THRESH_RED = (1.0, 0.0, 0.0)
_STAR_FACE = (1.0, 0.85, 0.0)
_ZERO_GRAY = (0.5, 0.5, 0.5)

_REQUIRED = ("week", "tow_s", "satellite", "system", "sig1", "sig2",
             "gf_m", "td2_gf_m", "thresh_m", "ivi_m", "iono_stable",
             "mw_flagged", "gf_flagged", "dN_wl", "wl_known",
             "dN1", "dN2", "frac", "action")


# --------------------------------------------------------------------------- #
#  Gap-break helper (the MATLAB insertGapBreaks)
# --------------------------------------------------------------------------- #
def _insert_gap_breaks(t, y, gap_factor):
    """Insert NaT/NaN where ``dt > gap_factor * median(dt)`` so a line breaks.

    ``t`` is a datetime-like Series/array, ``y`` numeric.  Returns
    ``(t_out, y_out)`` as numpy arrays (datetime64[ns] / float).
    """
    t = pd.to_datetime(np.asarray(t)).to_numpy("datetime64[ns]")
    y = np.asarray(y, dtype=float)
    if t.size < 2:
        return t, y
    order = np.argsort(t, kind="stable")
    t = t[order]
    y = y[order]
    dt = np.diff(t).astype("timedelta64[ns]").astype(np.int64) / 1e9  # seconds
    med = np.median(dt)
    if not np.isfinite(med) or med <= 0:
        med = 1.0
    gap_idx = np.flatnonzero(dt > gap_factor * med)
    if gap_idx.size == 0:
        return t, y
    t_out = []
    y_out = []
    start = 0
    for g in gap_idx:
        t_out.append(t[start:g + 1])
        y_out.append(y[start:g + 1])
        t_out.append(np.array(["NaT"], dtype="datetime64[ns]"))
        y_out.append(np.array([np.nan]))
        start = g + 1
    t_out.append(t[start:])
    y_out.append(y[start:])
    return np.concatenate(t_out), np.concatenate(y_out)


def _sample_std(v):
    """MATLAB ``std``: sample std (ddof=1); 0.0 for a single finite sample."""
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return float("nan")
    if v.size == 1:
        return 0.0
    return float(np.std(v, ddof=1))


def _g3(x):
    """MATLAB ``%.3g`` formatting of a float."""
    return f"{x:.3g}"


# --------------------------------------------------------------------------- #
#  Data prep
# --------------------------------------------------------------------------- #
def _prepare(ctx):
    df = load_trace(ctx.path("turbo_edit_trace.csv"))
    require_cols(df, _REQUIRED, "turbo_edit_trace.csv")

    # Coerce numerics (readtable may have picked the wrong type).
    num_cols = ["week", "tow_s", "elevation_deg", "lambda1_m", "lambda2_m",
                "gf_m", "td2_gf_m", "thresh_m", "ivi_m", "frac",
                "dN_wl", "dN1", "dN2"]
    for c in num_cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    for c in ("iono_stable", "mw_flagged", "gf_flagged", "wl_known"):
        df[c] = pd.to_numeric(df[c], errors="coerce").fillna(0.0)

    # -1 sentinel -> NaN (thresholds are physically nonnegative).
    df.loc[df["thresh_m"] < 0, "thresh_m"] = np.nan

    has_unfloored = "thresh_unfloored_m" in df.columns
    if has_unfloored:
        df["thresh_unfloored_m"] = pd.to_numeric(df["thresh_unfloored_m"],
                                                 errors="coerce")
        df.loc[df["thresh_unfloored_m"] < 0, "thresh_unfloored_m"] = np.nan

    if "bridged" in df.columns:
        df["bridged"] = pd.to_numeric(df["bridged"], errors="coerce").fillna(0.0)
    else:
        df["bridged"] = 0.0

    df["signal"] = df["sig1"].astype(str) + "/" + df["sig2"].astype(str)
    df = add_utc(df)
    df["system"] = df["system"].astype(str)
    df["satellite"] = df["satellite"].astype(str)
    df["action"] = df["action"].astype(str)

    # Detector-faithful gate ratio (|y|>1 == gf_exceeds); NaN on immature gate.
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = df["td2_gf_m"].to_numpy(float) / df["thresh_m"].to_numpy(float)
    ratio[~np.isfinite(ratio)] = np.nan
    df["gf_ratio"] = ratio

    return df, has_unfloored


def _systems_present(df):
    return [s for s in _SYS_ORDER if (df["system"] == s).any()]


def _prn_sort(sats):
    from ..identifiers import prn_num
    uniq = list(dict.fromkeys(str(s) for s in sats))
    return sorted(uniq, key=lambda s: (0, prn_num(s)) if prn_num(s) is not None
                  else (1, uniq.index(s)))


# --------------------------------------------------------------------------- #
#  TD2 / generic value grid (one Figure per constellation column)
# --------------------------------------------------------------------------- #
def _build_grid(df, value_col, value_label, fig_title, systems, plot_style,
                gap_factor, show_legend, ref_lines):
    """One Figure per constellation: signal pairs stacked as vertical axes.

    MATLAB used an outer ``tiledlayout(1, nCons)`` of inner column stacks; the
    backend renders a Figure as a vertical axes stack, so each constellation
    column becomes its own Figure (rows = that constellation's signal pairs).
    """
    figures = []
    for sys_name in systems:
        d_sys = df[df["system"] == sys_name]
        if d_sys.empty:
            continue
        sigs = sorted(s for s in d_sys["signal"].unique() if s and s != "nan")
        if not sigs:
            continue

        axes = []
        for r, sig in enumerate(sigs):
            sub = d_sys[d_sys["signal"] == sig]
            sats = _prn_sort(sub["satellite"].unique())
            cmap = color_by_prn(sats)

            traces = []
            for sat in sats:
                ss = sub[sub["satellite"] == sat]
                col = cmap[sat]
                if plot_style == "lines":
                    tt, yy = _insert_gap_breaks(ss["utc"], ss[value_col],
                                                gap_factor)
                    traces.append(Line(x=tt, y=yy, color=col, width=1.2,
                                       label=sat, show_in_legend=show_legend))
                else:
                    traces.append(Scatter(
                        x=ss["utc"].to_numpy("datetime64[ns]"),
                        y=ss[value_col].to_numpy(float), color=col, size=10.0,
                        label=sat, show_in_legend=show_legend))

            # Reference lines (0 -> gray zero; else red threshold).
            for yref in np.atleast_1d(ref_lines):
                if yref == 0:
                    traces.append(HLine(y=0.0, color=_ZERO_GRAY, style="--"))
                else:
                    traces.append(HLine(y=float(yref), color=_THRESH_RED,
                                       style="--"))

            # mu/sigma read-out (finite values only).
            vals = sub[value_col].to_numpy(float)
            vals = vals[np.isfinite(vals)]
            if vals.size:
                traces.append(TextBox(
                    x=0.98, y=0.97,
                    text=f"μ={_g3(float(np.mean(vals)))}  "
                         f"σ={_g3(_sample_std(vals))}",
                    ha="right", va="top", boxed=True))

            ax = Axes(
                traces=traces,
                title=f"{sys_name} - {sig}",
                xlabel="GPS Time" if r == len(sigs) - 1 else "",
                ylabel=value_label,
                legend=Legend(loc="bestoutside") if (show_legend and r == 0)
                else None,
            )
            axes.append(ax)

        figures.append(Figure(
            name=f"{fig_title} -- {sys_name}",
            title=f"{fig_title} ({sys_name})",
            axes=axes,
            width=max(6.0, 4.0),
            height=max(4.5, 1.7 * len(sigs) + 1.2),
            link_x=True,
        ))
    return figures


# --------------------------------------------------------------------------- #
#  IVI plot (one line per constellation on a single axes)
# --------------------------------------------------------------------------- #
def _build_ivi(tall, systems, gap_factor):
    traces = []
    for sys_name in systems:
        rows = tall[tall["system"] == sys_name]
        # IVI is identical across a constellation's rows each epoch -> collapse.
        sub = rows[["utc", "ivi_m"]].drop_duplicates().sort_values("utc")
        tt, yy = _insert_gap_breaks(sub["utc"], sub["ivi_m"], gap_factor)
        traces.append(Line(x=tt, y=yy, color=system_color(sys_name), width=1.0,
                          label=sys_name))
    traces.append(HLine(y=_IVI_GATE, label=f"IVI gate ({_IVI_GATE:.2f} m)",
                       color=_THRESH_RED, style="--"))
    ax = Axes(
        traces=traces,
        title=r"Cross-satellite ionospheric variability index "
              r"(median|TD2| + MAD$\sigma$|TD2|)",
        xlabel="GPS Time", ylabel="IVI (m)", legend=Legend(loc="best"),
    )
    return Figure(name="TurboEdit IVI", axes=[ax], width=11, height=4.0)


# --------------------------------------------------------------------------- #
#  Two-panel event example
# --------------------------------------------------------------------------- #
def _build_event_example(tall, ev, fig_name, is_repair, gap_factor):
    sat = str(ev["satellite"])
    sig = str(ev["signal"])
    t0 = ev["utc"]

    rows = tall[(tall["satellite"] == sat) & (tall["signal"] == sig)]
    s = rows.sort_values("utc").reset_index(drop=True)
    match = np.flatnonzero(s["utc"].to_numpy("datetime64[ns]")
                           == np.datetime64(t0))
    if match.size == 0:
        return None
    i0 = int(match[0])
    lo = max(0, i0 - _HALF_WIN)
    hi = min(len(s) - 1, i0 + _HALF_WIN)
    w = s.iloc[lo:hi + 1].reset_index(drop=True)
    i_ev = i0 - lo

    title = (f"{fig_name} -- {sat} {sig} @ {t0} "
             f"(dN1={int(ev['dN1']):+d}, dN2={int(ev['dN2']):+d}, "
             f"frac={ev['frac']:.3f})")

    # --- Panel 1: raw GF (+ repaired overlay for a repair) ---
    tt, yy = _insert_gap_breaks(w["utc"], w["gf_m"], gap_factor)
    p1 = [Line(x=tt, y=yy, color=_GF_BLUE, width=0.8, label="GF raw")]
    if is_repair and (ev["dN1"] != 0 or ev["dN2"] != 0):
        lam1 = float(ev.get("lambda1_m", np.nan))
        lam2 = float(ev.get("lambda2_m", np.nan))
        step_m = lam1 * ev["dN1"] - lam2 * ev["dN2"]
        post = w[w["utc"] >= t0]
        tt2, yy2 = _insert_gap_breaks(post["utc"],
                                      post["gf_m"].to_numpy(float) - step_m,
                                      gap_factor)
        p1.append(Line(x=tt2, y=yy2, color=_REPAIR_RED, width=1.2, style="--",
                       label=f"repaired: GF - ({step_m:+.4f} m)"))
    p1.append(VLine(x=np.datetime64(t0), color=(0, 0, 0), style=":"))
    p1.append(Scatter(x=[np.datetime64(t0)], y=[float(w["gf_m"].iloc[i_ev])],
                      color=_STAR_FACE, size=110.0, marker="*",
                      label="event epoch"))
    ax1 = Axes(traces=p1, title="Raw geometry-free phase", ylabel="GF (m)",
               legend=Legend(loc="best"))

    # --- Panel 2: TD2 vs +/- threshold envelope ---
    tt, yy = _insert_gap_breaks(w["utc"], w["td2_gf_m"], gap_factor)
    p2 = [Line(x=tt, y=yy, color=_GF_BLUE, width=0.8, label="TD2_GF")]
    tt, yy = _insert_gap_breaks(w["utc"], w["thresh_m"], gap_factor)
    p2.append(Line(x=tt, y=yy, color=_THRESH_RED, width=0.9, style="--",
                   label="+/- threshold"))
    tt, yy = _insert_gap_breaks(w["utc"], -w["thresh_m"].to_numpy(float),
                                gap_factor)
    p2.append(Line(x=tt, y=yy, color=_THRESH_RED, width=0.9, style="--",
                   show_in_legend=False))
    p2.append(VLine(x=np.datetime64(t0), color=(0, 0, 0), style=":"))
    p2.append(Scatter(x=[np.datetime64(t0)],
                      y=[float(w["td2_gf_m"].iloc[i_ev])],
                      color=_STAR_FACE, size=110.0, marker="*",
                      label="event epoch"))
    src = _flag_src(ev)
    p2.append(TextBox(
        x=0.02, y=0.95,
        text=(f"flag: {src}   dN_WL={int(ev['dN_wl']):+d} "
              f"(known={int(ev['wl_known'])})   "
              f"td2={ev['td2_gf_m']:+.4f} m   "
              f"thresh={ev['thresh_m']:.4f} m   action: {ev['action']}"),
        ha="left", va="top", boxed=True))
    ax2 = Axes(traces=p2, title="GF second time-difference vs detector gate",
               xlabel="GPS Time", ylabel="TD2_GF (m)", legend=Legend(loc="best"))

    return Figure(name=fig_name, title=title, axes=[ax1, ax2],
                  width=11, height=7.0, link_x=True)


def _flag_src(ev):
    src = ""
    if ev["mw_flagged"]:
        src += "MW"
    if ev["gf_flagged"]:
        if src:
            src += "+"
        src += "GF"
    return src


# --------------------------------------------------------------------------- #
#  Event inventory + summary text
# --------------------------------------------------------------------------- #
def _summary(tall, evt, systems, has_unfloored):
    n_repair = int((evt["action"] == "repair").sum())
    n_reset = int((evt["action"] == "reset").sum())
    n_code = int((evt["action"] == "code_event").sum())
    n_moot = int((evt["action"] == "moot").sum())
    n_none = int((evt["action"] == "none").sum())
    n_other = len(evt) - n_repair - n_reset - n_code - n_moot - n_none
    mw = evt["mw_flagged"].astype(bool)
    gf = evt["gf_flagged"].astype(bool)
    n_joint = int((mw & gf).sum())
    n_mwonly = int((mw & ~gf).sum())
    n_gfonly = int((~mw & gf).sum())
    n_bridged = int(evt["bridged"].astype(bool).sum())

    out = [
        f"=== TurboEdit flagged events: {len(evt)} (repair: {n_repair}, "
        f"reset: {n_reset}, code_event: {n_code}, moot: {n_moot}, "
        f"none: {n_none}; bridged: {n_bridged}) ===",
    ]
    if n_other > 0:
        out.append(f"    WARNING: {n_other} event(s) carry an unrecognized "
                   f"action value")
    out.append(f"    flag sources: joint MW+GF: {n_joint}, MW-only: "
               f"{n_mwonly}, GF-only: {n_gfonly}")

    n_show = min(len(evt), 30)
    for e in range(n_show):
        ev = evt.iloc[e]
        src = _flag_src(ev)
        out.append(
            f"  {ev['utc']}  {ev['satellite']} {ev['signal']:<9} {src:<5} "
            f"td2={ev['td2_gf_m']:+8.4f} m  thresh={ev['thresh_m']:7.4f} m  "
            f"dN_wl={int(ev['dN_wl']):+d}(k={int(ev['wl_known'])})  "
            f"dN1={int(ev['dN1']):+d} dN2={int(ev['dN2']):+d} "
            f"frac={ev['frac']:.3f}  -> {ev['action']}")
    if len(evt) > n_show:
        out.append(f"  ... ({len(evt) - n_show} more)")

    out += ["", "=== TurboEdit GF Statistics by System and Signal Pair ===", ""]
    for sys_name in systems:
        d_sys = tall[tall["system"] == sys_name]
        sig_list = sorted(d_sys["signal"].unique())
        for sig in sig_list:
            idx = d_sys["signal"] == sig
            if not idx.any():
                continue
            sub = d_sys[idx]
            td2 = sub["td2_gf_m"].to_numpy(float)
            td2 = td2[np.isfinite(td2)]
            th = sub["thresh_m"].to_numpy(float)
            n_mature = int(np.isfinite(th).sum())
            med_abs = float(np.median(np.abs(td2))) if td2.size else float("nan")
            max_abs = float(np.max(np.abs(td2))) if td2.size else 0.0
            un_str = ""
            if has_unfloored:
                un = sub["thresh_unfloored_m"].to_numpy(float)
                un = un[np.isfinite(un)]
                pct_floor = 100.0 * np.sum(un < _FLOOR_M) / max(un.size, 1)
                med_un = float(np.median(un)) if un.size else float("nan")
                un_str = (f"  median kσMAD={med_un:.4f} m  "
                          f"floor-bound={pct_floor:.0f}%")
            out.append(
                f"{sys_name} {sig}: N={int(idx.sum())} (mature {n_mature}), "
                f"|TD2| med={med_abs:.4f} max={max_abs:.4f} m{un_str}")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
#  Build entry point
# --------------------------------------------------------------------------- #
def build_turbo_edit_trace(
    ctx,
    plot_style: str = "lines",       # 'lines' | 'scatter'  (MATLAB plotStyle)
    gap_factor: float = 3.0,         # gap = dt > gap_factor * median(dt)
    show_td2: bool = True,           # td2_gf_m grid (primary statistic)
    show_ratio: bool = False,        # td2 / thresh grid
    show_threshold: bool = False,    # thresh_unfloored_m vs 0.04 floor
    show_raw_gf: bool = False,       # raw gf_m grid
    show_ivi: bool = True,           # per-constellation IVI + 0.06 gate
    show_event_examples: bool = True,  # zoomed repair/reset/code-event panels
    show_legend: bool = False,       # per-tile satellite legend on top tile
    hide_flagged_epochs: bool = True,  # drop flagged rows from grid figures
) -> ViewerResult:
    """Build the TurboEdit MW+GF resolver diagnostic figures.

    Keyword arguments default to the MATLAB script's default content switches.
    """
    tall, has_unfloored = _prepare(ctx)
    systems = _systems_present(tall)
    if not systems:
        raise ValueError("No usable constellations in turbo_edit_trace.csv")

    # Grid table optionally drops flagged rows so it autoscales on in-arc noise.
    grid_df = tall
    if hide_flagged_epochs:
        flag_mask = (tall["mw_flagged"].astype(bool)
                     | tall["gf_flagged"].astype(bool))
        if flag_mask.any():
            grid_df = tall[~flag_mask]

    figures = []

    if show_td2:
        figures += _build_grid(
            grid_df, "td2_gf_m", "TD2_GF (m)",
            "TurboEdit GF Second Time-Difference by Signal Pair", systems,
            plot_style, gap_factor, show_legend, [0.0])

    if show_ratio:
        figures += _build_grid(
            grid_df, "gf_ratio", "TD2 / thresh_C++",
            "TurboEdit GF Gate (TD2 / threshold) by Signal Pair", systems,
            plot_style, gap_factor, show_legend, [-1.0, 1.0])

    if show_threshold and has_unfloored:
        figures += _build_grid(
            grid_df, "thresh_unfloored_m", "kσ.MAD (m)",
            "TurboEdit Unfloored Threshold (vs 0.04 m floor) by Signal Pair",
            systems, plot_style, gap_factor, show_legend, [_FLOOR_M])

    if show_raw_gf:
        figures += _build_grid(
            grid_df, "gf_m", "GF (m)",
            "TurboEdit Raw Geometry-Free Phase by Signal Pair", systems,
            plot_style, gap_factor, show_legend, [])

    if show_ivi:
        figures.append(_build_ivi(tall, systems, gap_factor))

    # Event inventory / examples are built from the full (pre-filter) snapshot.
    evt = tall[tall["mw_flagged"].astype(bool)
               | tall["gf_flagged"].astype(bool)].reset_index(drop=True)

    if show_event_examples and not evt.empty:
        rep = evt[evt["action"] == "repair"]
        if not rep.empty:
            k = int(rep["frac"].abs().to_numpy().argmin())
            fig = _build_event_example(tall, rep.iloc[k],
                                       "TurboEdit repair (example)",
                                       True, gap_factor)
            if fig is not None:
                figures.append(fig)

        rst = evt[evt["action"] == "reset"]
        if not rst.empty:
            ratio = np.abs(rst["td2_gf_m"].to_numpy(float)
                           / rst["thresh_m"].to_numpy(float))
            ratio[~np.isfinite(ratio)] = -np.inf
            k = int(np.argmax(ratio))
            fig = _build_event_example(tall, rst.iloc[k],
                                       "TurboEdit reset (example)",
                                       False, gap_factor)
            if fig is not None:
                figures.append(fig)

        cev = evt[evt["action"] == "code_event"]
        if not cev.empty:
            ratio = np.abs(cev["td2_gf_m"].to_numpy(float)
                           / cev["thresh_m"].to_numpy(float))
            ratio[~np.isfinite(ratio)] = np.inf
            k = int(np.argmin(ratio))
            fig = _build_event_example(
                tall, cev.iloc[k],
                "TurboEdit code-domain event (example: MW flagged, "
                "GF stable, phase preserved)", False, gap_factor)
            if fig is not None:
                figures.append(fig)

    summary = _summary(tall, evt, systems, has_unfloored)
    return ViewerResult(figures=figures, summary=summary)


viewer = Viewer(name="turbo_edit_trace",
                title="TurboEdit MW+GF slip resolver",
                required_csvs=("turbo_edit_trace.csv",),
                build=build_turbo_edit_trace)
