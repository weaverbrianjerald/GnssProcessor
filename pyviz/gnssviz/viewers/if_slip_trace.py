"""IF-phase slip-trace viewer: the port of ``view_if_slip_trace.m``.

Reads ``if_slip_trace.csv`` from the staged (rarely wired in)
``IfPhaseSlipDetector`` and reproduces its four diagnostic figures, faceted by
constellation.  The detector forms an ionosphere-free (IF) carrier-phase
combination, single-differences it between satellites against a per-epoch
reference satellite (``grad_phi_if_m``), then takes a between-epoch difference
of that single difference (``delta_grad_m``); a slip is flagged when the
between-epoch double difference exceeds the IF wavelength gate.  A
position-uncertainty gate (``pos_gated``) can suppress detection when the
filter position is itself uncertain.

Expected CSV header (per the MATLAB banner)::

    week,tow_s,satellite,system,elevation_deg,
    phase_if_key,phi_if_omc_m,
    grad_phi_if_m,delta_grad_m,
    ref_satellite,ref_changed,
    lambda_if_m,is_slip,slip_reason,
    pos_sigma_3d_m,pos_gated

The ``pos_sigma_3d_m`` / ``pos_gated`` columns may be absent in older traces
(filled with zeros, exactly as the MATLAB does).

Figures (each one constellation per vertically stacked tile: the backend
stacks axes rather than tiling them side by side as MATLAB's ``tiledlayout``
does, but the per-system grouping, colours and series are identical):

1. ``Δ∇φ_IF`` between-epoch double difference [m], per ``<sat>_<phase_if_key>``
   key, coloured by satellite, with red ``x`` slip markers, grey pos-gated dots,
   and ``±λ_IF`` reference lines at the per-system median IF wavelength.  Only
   the clean (non-slip, non-gated) rows draw lines; slips and pos-gated rows are
   overlaid as markers.
2. ``φ_IF`` observed-minus-computed [m], all rows, per key, with slip markers.
3. ``∇φ_IF`` between-satellite single difference [m], all rows, per key, with
   slip markers.
4. Reference-satellite elevation over time, one line per distinct reference
   satellite, with vertical ``ref change`` event lines.

Fidelity notes
--------------
* Satellite colours come from :func:`distinguishable_colors` sized
  ``max(8, N)`` over the sorted-unique satellite categories, indexed by the
  satellite's position in that ordering: matching the MATLAB
  ``distinguishable_colors(max(8,N))`` + ``find(satCats==sat)`` lookup.  MATLAB
  ``unique(categorical(...))`` sorts; this port sorts too.
* Arc-breaking inserts a NaN sample at every time gap larger than ``1.5 ×``
  the median epoch step, so lines do not connect across visibility gaps (the
  ``break_arcs`` helper, ported verbatim).
* ``median`` of the empty set / all-NaN is NaN (no reference line drawn), as in
  MATLAB ``median(..., 'omitnan')``.
"""

from __future__ import annotations

from typing import List, Tuple

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace
from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox, VLine
from ..palettes import distinguishable_colors
from .base import Viewer, ViewerResult

_SLIP_RED = (1.0, 0.0, 0.0)
_GATE_GREY = (0.7, 0.7, 0.7)
_LAMBDA_GREY = (0.5, 0.5, 0.5)
_REFCHG = (0.8, 0.2, 0.2)


def _break_arcs(t, y, gap_thresh_s: float) -> Tuple[np.ndarray, np.ndarray]:
    """Insert a NaN sample at every time gap > ``gap_thresh_s`` (port of
    MATLAB ``break_arcs``).

    ``t`` is a datetime-like array (sorted), ``y`` a same-length float array.
    Returns ``(t_out, y_out)`` with a ``NaT``/``NaN`` row at every gap so a line
    renderer leaves the arc boundaries open.
    """
    t = pd.to_datetime(np.asarray(t))
    y = np.asarray(y, dtype=float)
    n = len(t)
    if n < 2:
        return t.to_numpy(), y

    dt_sec = np.diff(t.view("int64")) / 1e9          # seconds between samples
    gap_idx = np.nonzero(dt_sec > gap_thresh_s)[0]   # gap follows index gi
    if gap_idx.size == 0:
        return t.to_numpy(), y

    t_out: List = []
    y_out: List = []
    src = 0
    for gi in gap_idx:
        seg_end = gi                      # inclusive last index of this segment
        t_out.extend(t[src:seg_end + 1])
        y_out.extend(y[src:seg_end + 1])
        t_out.append(np.datetime64("NaT"))
        y_out.append(np.nan)
        src = seg_end + 1
    t_out.extend(t[src:])
    y_out.extend(y[src:])
    return np.array(t_out, dtype="datetime64[ns]"), np.asarray(y_out, dtype=float)


def _sat_colors(df: pd.DataFrame):
    """``distinguishable_colors(max(8,N))`` keyed by sorted-unique satellite.

    Returns ``(color_of_sat, sat_order)`` where ``color_of_sat`` maps each
    satellite ID to its colour (by index in the sorted ordering), matching
    MATLAB's ``find(string(satCats)==sat)`` lookup against
    ``unique(categorical(...))`` (which sorts).
    """
    sat_cats = sorted(df["satellite"].astype(str).unique())
    n = len(sat_cats)
    colors = distinguishable_colors(max(8, n))
    color_of = {}
    for i, s in enumerate(sat_cats):
        color_of[s] = colors[i % len(colors)]
    return color_of, sat_cats


def _key_lines(t_sys: pd.DataFrame, value_col: str, color_of, sat_cats,
               gap_thresh_s: float) -> List[Line]:
    """One arc-broken line per ``key`` in ``t_sys``, coloured by satellite."""
    traces: List[Line] = []
    keys = sorted(t_sys["key"].unique())
    for k, this_key in enumerate(keys):
        sub = t_sys[t_sys["key"] == this_key].sort_values("utc")
        sat = str(this_key).split("_", 1)[0]
        if sat in color_of:
            c = color_of[sat]
        else:
            colors = distinguishable_colors(max(8, len(sat_cats)))
            c = colors[k % len(colors)]
        t_arc, y_arc = _break_arcs(sub["utc"].to_numpy(),
                                   sub[value_col].to_numpy(float), gap_thresh_s)
        traces.append(Line(x=t_arc, y=y_arc, color=c, width=0.8,
                           label=str(this_key)))
    return traces


def _no_data_axes(sys_name: str) -> Axes:
    ax = Axes(title=sys_name, axis_off=True, x_is_datetime=False)
    ax.traces = [TextBox(x=0.5, y=0.5, text="No Data", ha="center", va="center")]
    return ax


def build_if_slip_trace(ctx) -> ViewerResult:
    df = load_trace(ctx.path("if_slip_trace.csv"))

    # Required columns (matches the MATLAB guard).
    need = ["week", "tow_s", "satellite", "system", "elevation_deg",
            "phase_if_key", "delta_grad_m", "ref_satellite", "is_slip"]
    missing = [c for c in need if c not in df.columns]
    if missing:
        raise ValueError("if_slip_trace.csv: missing required column(s): "
                         + ", ".join(missing))

    # Numeric coercions.
    for c in ("week", "tow_s", "elevation_deg", "phi_if_omc_m", "grad_phi_if_m",
              "delta_grad_m", "lambda_if_m", "is_slip", "ref_changed"):
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")

    # Optional position-uncertainty gating columns (default to zeros).
    if "pos_sigma_3d_m" in df.columns:
        df["pos_sigma_3d_m"] = pd.to_numeric(df["pos_sigma_3d_m"], errors="coerce")
    else:
        df["pos_sigma_3d_m"] = 0.0
    if "pos_gated" in df.columns:
        df["pos_gated"] = pd.to_numeric(df["pos_gated"], errors="coerce")
    else:
        df["pos_gated"] = 0.0

    df["satellite"] = df["satellite"].astype(str)
    df["system"] = df["system"].astype(str)
    df["phase_if_key"] = df["phase_if_key"].astype(str)
    df["ref_satellite"] = df["ref_satellite"].astype(str)

    # Key like "G01_L1C_L2W".
    df["key"] = df["satellite"] + "_" + df["phase_if_key"]

    df = add_utc(df)

    # Nominal epoch step -> arc-break gap threshold (1.5x the median dt).
    all_tow = np.sort(df["tow_s"].dropna().unique())
    if all_tow.size > 1:
        dt_nominal = float(np.median(np.diff(all_tow)))
    else:
        dt_nominal = 30.0
    gap_thresh_s = 1.5 * dt_nominal

    # Slip / pos-gated / clean partitions.
    is_slip = df["is_slip"] == 1
    is_gated = (df["is_slip"] == 0) & (df["pos_gated"] == 1)
    t_slip = df[is_slip]
    t_gated = df[is_gated]
    t_clean = df[(df["is_slip"] == 0) & (df["pos_gated"] == 0)]

    # MATLAB categories() returns the sorted category list.
    system_list = sorted(df["system"].unique())
    color_of, sat_cats = _sat_colors(df)

    figures: List[Figure] = []

    # ---- figure 1: between-epoch double difference (delta_grad) -------------
    axes1: List[Axes] = []
    for sys_name in system_list:
        t_sys = t_clean[t_clean["system"] == sys_name]
        t_sys_slip = t_slip[t_slip["system"] == sys_name]
        t_sys_gated = t_gated[t_gated["system"] == sys_name]
        if t_sys.empty and t_sys_slip.empty and t_sys_gated.empty:
            axes1.append(_no_data_axes(sys_name))
            continue

        ax = Axes(title=sys_name, xlabel="GPS Time",
                  ylabel="Δ∇φ_IF (m)", legend=Legend("best"))
        ax.traces = _key_lines(t_sys, "delta_grad_m", color_of, sat_cats,
                               gap_thresh_s)
        if not t_sys_slip.empty:
            ax.traces.append(Scatter(
                x=t_sys_slip["utc"], y=t_sys_slip["delta_grad_m"].to_numpy(float),
                color=_SLIP_RED, marker="x", size=80.0, label="SLIP"))
        if not t_sys_gated.empty:
            ax.traces.append(Scatter(
                x=t_sys_gated["utc"], y=t_sys_gated["delta_grad_m"].to_numpy(float),
                color=_GATE_GREY, marker=".", size=12.0, label="pos-gated"))

        # +/- median lambda_IF reference lines (per system).
        lam = df.loc[df["system"] == sys_name, "lambda_if_m"]
        lam = lam.dropna() if "lambda_if_m" in df.columns else pd.Series([], dtype=float)
        lam_median = float(np.median(lam)) if len(lam) else float("nan")
        if np.isfinite(lam_median) and lam_median > 0:
            ax.traces.append(HLine(y=lam_median, color=_LAMBDA_GREY, style="--",
                                   label=f"+λ_IF {lam_median:.3f} m"))
            ax.traces.append(HLine(y=-lam_median, color=_LAMBDA_GREY, style="--",
                                   label=f"-λ_IF {lam_median:.3f} m"))
        axes1.append(ax)
    figures.append(Figure(name="IF phase double-diff (delta_grad) by System",
                          title="Δ∇φ_IF (between-satellite, between-epoch) [m]",
                          axes=axes1, width=14, height=max(4.0, 2.4 * len(axes1))))

    # ---- figure 2: IF phase OMC (phi_if_omc) -------------------------------
    if "phi_if_omc_m" in df.columns:
        axes2: List[Axes] = []
        for sys_name in system_list:
            t_sys = df[df["system"] == sys_name]
            if t_sys.empty:
                axes2.append(_no_data_axes(sys_name))
                continue
            ax = Axes(title=sys_name, xlabel="GPS Time",
                      ylabel="φ_IF OMC (m)", legend=Legend("best"))
            ax.traces = _key_lines(t_sys, "phi_if_omc_m", color_of, sat_cats,
                                   gap_thresh_s)
            t_sys_slip = t_sys[t_sys["is_slip"] == 1]
            if not t_sys_slip.empty:
                ax.traces.append(Scatter(
                    x=t_sys_slip["utc"],
                    y=t_sys_slip["phi_if_omc_m"].to_numpy(float),
                    color=_SLIP_RED, marker="x", size=80.0, label="SLIP"))
            axes2.append(ax)
        figures.append(Figure(name="IF phase OMC by System",
                              title="φ_IF observed-minus-computed [m]",
                              axes=axes2, width=14,
                              height=max(4.0, 2.4 * len(axes2))))

    # ---- figure 3: between-satellite single difference (grad_phi_if) -------
    if "grad_phi_if_m" in df.columns:
        axes3: List[Axes] = []
        for sys_name in system_list:
            t_sys = df[df["system"] == sys_name]
            if t_sys.empty:
                axes3.append(_no_data_axes(sys_name))
                continue
            ax = Axes(title=sys_name, xlabel="GPS Time",
                      ylabel="∇φ_IF (m)", legend=Legend("best"))
            ax.traces = _key_lines(t_sys, "grad_phi_if_m", color_of, sat_cats,
                                   gap_thresh_s)
            t_sys_slip = t_sys[t_sys["is_slip"] == 1]
            if not t_sys_slip.empty:
                ax.traces.append(Scatter(
                    x=t_sys_slip["utc"],
                    y=t_sys_slip["grad_phi_if_m"].to_numpy(float),
                    color=_SLIP_RED, marker="x", size=80.0, label="SLIP"))
            axes3.append(ax)
        figures.append(Figure(name="IF phase gradient (grad) by System",
                              title="∇φ_IF between-satellite single difference [m]",
                              axes=axes3, width=14,
                              height=max(4.0, 2.4 * len(axes3))))

    # ---- figure 4: reference satellite elevation over time -----------------
    has_refchg = "ref_changed" in df.columns
    axes4: List[Axes] = []
    for sys_name in system_list:
        t_sys = df[df["system"] == sys_name]
        if t_sys.empty:
            axes4.append(_no_data_axes(sys_name))
            continue

        # One row per epoch (first occurrence; all share the same ref_satellite).
        t_epoch = t_sys.drop_duplicates(subset="tow_s", keep="first")

        ax = Axes(title=sys_name, xlabel="GPS Time",
                  ylabel="Ref Elevation (deg)", legend=Legend("best"))

        ref_sats = sorted(t_epoch["ref_satellite"].unique())
        for rs_i, rs in enumerate(ref_sats):
            ep = t_epoch[t_epoch["ref_satellite"] == rs].sort_values("utc")
            # Look up the reference sat's own elevation in each of its epochs.
            ref_elev = np.full(len(ep), np.nan)
            for i, tow in enumerate(ep["tow_s"].to_numpy()):
                m = t_sys[(t_sys["tow_s"] == tow) & (t_sys["satellite"] == rs)]
                if not m.empty:
                    ref_elev[i] = float(m["elevation_deg"].iloc[0])
            if rs in color_of:
                c = color_of[rs]
            else:
                c = distinguishable_colors(max(8, len(sat_cats)))[rs_i]
            t_arc, y_arc = _break_arcs(ep["utc"].to_numpy(), ref_elev, gap_thresh_s)
            ax.traces.append(Line(x=t_arc, y=y_arc, color=c, width=1.0, label=rs))

        # Mark ref changes as vertical event lines.
        if has_refchg:
            chg = t_epoch[t_epoch["ref_changed"] == 1]
            for _, row in chg.iterrows():
                ax.traces.append(VLine(x=row["utc"], color=_REFCHG, style=":",
                                       width=0.8))
            if not chg.empty:
                # Legend proxy for "ref change".
                ax.traces.append(VLine(x=chg["utc"].iloc[0], color=_REFCHG,
                                       style=":", width=0.8, label="ref change"))
        axes4.append(ax)
    figures.append(Figure(name="IF slip reference satellite",
                          title="Reference satellite elevation & changes",
                          axes=axes4, width=14,
                          height=max(4.0, 2.2 * len(axes4))))

    return ViewerResult(figures=figures,
                        summary=_summary(df, t_slip, system_list))


def _summary(df: pd.DataFrame, t_slip: pd.DataFrame, system_list) -> str:
    n_slip = int((df["is_slip"] == 1).sum())
    lines = [
        "=== IF Phase Slip Trace Summary ===",
        f"Rows       : {len(df)}",
        f"Epochs     : {df['tow_s'].nunique()}",
        f"Satellites : {df['satellite'].nunique()}",
        f"Systems    : {', '.join(str(s) for s in system_list)}",
        f"Slips      : {n_slip}",
        f"Pos-gated  : {df.loc[df['pos_gated'] == 1, 'tow_s'].nunique()} epochs",
    ]
    if n_slip > 0:
        lines.append("")
        lines.append("--- Slip details ---")
        for _, r in t_slip.iterrows():
            reason = r["slip_reason"] if "slip_reason" in t_slip.columns else ""
            lines.append(
                f"  TOW {float(r['tow_s']):10.1f}  {r['satellite']}  "
                f"{str(r['phase_if_key']):<12}  "
                f"delta_grad={float(r['delta_grad_m']):+.4f} m  reason={reason}")
    return "\n".join(lines)


viewer = Viewer(name="if_slip_trace",
                title="IF-phase slip trace (STAGED detector)",
                required_csvs=("if_slip_trace.csv",),
                build=build_if_slip_trace)
