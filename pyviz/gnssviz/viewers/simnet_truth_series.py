"""Simulated-network truth-error time series: formal sigma vs true error, over the run.

Reads ``simnet_grade_series.csv`` (written by ``scripts/simnet_grade.py --series``):
per-epoch aggregates of the same residuals the grader grades, produced by the same
transform code path so the plots cannot drift from the verdict. Four figures:

  1. **Truth error vs formal sigma**: per-epoch rms truth error beside the published
     formal sigma, per channel. This is the calibration exhibit: a confidently wrong
     estimator shows a shrinking sigma while the true error grows, with every
     internal instrument silent (SPEC §9). The settle boundary is drawn on every
     axis; everything left of it is ignition and is shown, never graded.
  2. **ZWD truth error per station**: the determinability exhibit (n=3 carries a
     metre-class ZWD bias that two more stations remove entirely).
  3. **Receiver-side channels**: rx-DCB between-station contrast (graded) and
     fleet-mean level (the absorbed Table-8 gauge, not an error), plus the rx
     clock contrast.
  4. **Phase SD fractional**: the between-satellite SD phase-bias residual in
     fractional cycles; med|frac| pinned near 0.25 means uniform, i.e. zero
     integer information.

The code-bias series covers the settled window only: the grader's transform
never runs on ignition epochs.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, VLine
from ..io import load_trace, require_cols
from ..palettes import distinguishable_colors
from .base import Viewer, ViewerResult, ViewerSkip

_LEGEND_CAP = 12


def _hours(tow, tow0):
    return (tow - tow0) / 3600.0


def _channel(df, name):
    """(tow_hours, value, n) arrays of one channel, time-sorted."""
    sub = df[df["channel"] == name].sort_values("tow")
    return (sub["h"].to_numpy(float), sub["value"].to_numpy(float),
            sub["n"].to_numpy(float) if "n" in sub else None)


def _log_if_positive(ax, series_list):
    """Log y when the data spans decades and is positive (the collapse exhibit)."""
    vals = np.concatenate([np.asarray(v, float) for v in series_list]) if series_list else np.array([])
    vals = vals[np.isfinite(vals) & (vals > 0)]
    if vals.size and vals.max() / max(vals.min(), 1e-12) > 30.0:
        ax.yscale = "log"


def _settle_vline(h_settle):
    return VLine(x=h_settle, color=(0.75, 0.35, 0.10), style="--", width=1.4,
                 label="settle boundary (left = ignition, shown not graded)")


def build_simnet_truth_series(ctx) -> ViewerResult:
    df = load_trace(ctx.path("simnet_grade_series.csv"))
    require_cols(df, ("tow", "channel", "id", "value"), "simnet_grade_series.csv")
    if df.empty:
        raise ViewerSkip("simnet_grade_series.csv has no rows")
    df = df.copy()
    df["tow"] = df["tow"].astype(float)
    tow0 = float(df.loc[df["channel"] != "_settle", "tow"].min())
    df["h"] = _hours(df["tow"], tow0)

    settle = df[df["channel"] == "_settle"]
    h_settle = float(settle["h"].iloc[0]) if len(settle) else None

    figs = [f for f in (_fig_error_vs_sigma(df, h_settle),
                        _fig_zwd_stations(df, h_settle),
                        _fig_rx_channels(df, h_settle),
                        _fig_phase_frac(df, h_settle)) if f is not None]
    if not figs:
        raise ViewerSkip("no plottable channels in simnet_grade_series.csv")
    return ViewerResult(figures=figs, summary=_summary(df, h_settle))


# --------------------------------------------------------------------------- #
def _fig_error_vs_sigma(df, h_settle):
    axes = []

    h, clk, _ = _channel(df, "clk_rms")
    hs, sig, _ = _channel(df, "clk_sig_med")
    hc, con, _ = _channel(df, "clk_contrast_rms")
    if h.size:
        ax = Axes(title="Satellite clock — per-epoch rms truth error vs published σ",
                  ylabel="m", xlabel="run time (h)", x_is_datetime=False,
                  legend=Legend(loc="best"))
        ax.traces = [Line(x=h, y=clk, color=(0.72, 0.11, 0.11), width=1.4,
                          label="rms truth error (realized basis)")]
        if hc.size:
            ax.traces.append(Line(x=hc, y=con, color=(0.72, 0.11, 0.11), width=1.0,
                                  style=":", label="rms contrast error (datum-free check)"))
        if hs.size:
            ax.traces.append(Line(x=hs, y=sig, color=(0.25, 0.25, 0.25), width=1.2,
                                  style="--", label="published σ (median URA)"))
        _log_if_positive(ax, [clk, sig] if hs.size else [clk])
        if h_settle is not None:
            ax.traces.append(_settle_vline(h_settle))
        axes.append(ax)

    zwd = df[df["channel"] == "zwd_e"]
    zsg = df[df["channel"] == "zwd_sig"]
    if len(zwd):
        g = zwd.groupby("h")["value"]
        hz = np.array(sorted(g.groups))
        rms = np.sqrt(zwd.groupby("h")["value"].apply(lambda v: float(np.mean(np.square(v))))
                      ).reindex(hz).to_numpy(float)
        ax = Axes(title="ZWD — per-epoch rms truth error (all stations) vs published σ",
                  ylabel="m", xlabel="run time (h)", x_is_datetime=False,
                  legend=Legend(loc="best"))
        ax.traces = [Line(x=hz, y=rms, color=(0.13, 0.36, 0.58), width=1.4,
                          label="rms truth error")]
        if len(zsg):
            med = zsg.groupby("h")["value"].median().reindex(hz).to_numpy(float)
            ax.traces.append(Line(x=hz, y=med, color=(0.25, 0.25, 0.25), width=1.2,
                                  style="--", label="published σ (median)"))
            _log_if_positive(ax, [rms, med])
        else:
            _log_if_positive(ax, [rms])
        if h_settle is not None:
            ax.traces.append(_settle_vline(h_settle))
        axes.append(ax)

    hb, cbr, _ = _channel(df, "cb_rms")
    hm, chm, _ = _channel(df, "cb_chmean_absmax")
    if hb.size:
        ax = Axes(title="Satellite code bias — per-epoch rms truth error (settled window only)",
                  ylabel="m", xlabel="run time (h)", x_is_datetime=False,
                  legend=Legend(loc="best"))
        ax.traces = [Line(x=hb, y=cbr, color=(0.33, 0.49, 0.18), width=1.4,
                          label="rms truth error (W6 S-transform)")]
        if hm.size:
            ax.traces.append(Line(x=hm, y=chm, color=(0.55, 0.42, 0.10), width=1.0,
                                  style=":",
                                  label="max |per-channel mean| (sum-to-zero datum check)"))
        if h_settle is not None:
            ax.traces.append(_settle_vline(h_settle))
        axes.append(ax)

    if not axes:
        return None
    return Figure(name="simnet_error_vs_sigma",
                  title="Truth error vs formal σ — the calibration exhibit",
                  subtitle="a collapsing σ under a growing truth error is the failure "
                           "this fixture exists to expose (internal instruments stay silent)",
                  axes=axes, width=12.0, height=3.6 * len(axes), link_x=True)


def _fig_zwd_stations(df, h_settle):
    zwd = df[df["channel"] == "zwd_e"]
    if not len(zwd):
        return None
    stations = sorted(zwd["id"].astype(str).unique())
    colors = distinguishable_colors(len(stations))
    ax = Axes(title="Per-station ZWD truth error", ylabel="ZWD error (m)",
              xlabel="run time (h)", x_is_datetime=False, legend=Legend(loc="best"))
    traces = []
    for i, s in enumerate(stations):
        sub = zwd[zwd["id"] == s].sort_values("h")
        traces.append(Line(x=sub["h"].to_numpy(float), y=sub["value"].to_numpy(float),
                           color=colors[i], width=0.9, label=str(s),
                           show_in_legend=(i < _LEGEND_CAP)))
    traces.append(HLine(y=0.0, color=(0.4, 0.4, 0.4), style="-"))
    if h_settle is not None:
        traces.append(_settle_vline(h_settle))
    ax.traces = traces
    return Figure(name="simnet_zwd_stations",
                  title="ZWD truth error per station — the determinability exhibit",
                  subtitle="n=3 carries a metre-class common ZWD bias every internal "
                           "instrument calls clean; two more stations remove it (SPEC §9)",
                  axes=[ax], width=12.0, height=4.2)


def _fig_rx_channels(df, h_settle):
    hl, lvl, _ = _channel(df, "rxdcb_level")
    hc, con, _ = _channel(df, "rxdcb_contrast_rms")
    hk, clk, _ = _channel(df, "rxclk_contrast_rms")
    if not (hl.size or hc.size or hk.size):
        return None
    ax = Axes(title="Receiver-side channels vs truth",
              ylabel="m", xlabel="run time (h)", x_is_datetime=False,
              legend=Legend(loc="best"))
    traces = []
    if hc.size:
        traces.append(Line(x=hc, y=con, color=(0.13, 0.36, 0.58), width=1.3,
                           label="rx DCB contrast rms (GRADED — the estimable part)"))
    if hl.size:
        traces.append(Line(x=hl, y=lvl, color=(0.72, 0.11, 0.11), width=1.3,
                           label="rx DCB fleet-mean level (absorbed Table-8 gauge, NOT an error)"))
    if hk.size:
        traces.append(Line(x=hk, y=clk, color=(0.33, 0.49, 0.18), width=1.1, style="--",
                           label="rx clock contrast rms"))
    traces.append(HLine(y=0.0, color=(0.4, 0.4, 0.4), style="-"))
    if h_settle is not None:
        traces.append(_settle_vline(h_settle))
    ax.traces = traces
    return Figure(name="simnet_rx_channels",
                  title="Receiver DCB / clock — contrast is graded, level is the gauge",
                  subtitle="the fleet-mean GF bias is absorbed into the ionosphere "
                           "(Table-8 identity): its wander is the level exchange, not an error",
                  axes=[ax], width=12.0, height=4.0)


def _fig_phase_frac(df, h_settle):
    ph = df[df["channel"] == "phase_frac_med"]
    if not len(ph):
        return None
    sigs = sorted(ph["id"].astype(str).unique())
    colors = distinguishable_colors(max(len(sigs), 3))
    ax = Axes(title="Between-satellite SD phase bias — med|fractional cycle| per signal",
              ylabel="cycles", xlabel="run time (h)", x_is_datetime=False,
              legend=Legend(loc="best"), ylim=(0.0, 0.55))
    traces = []
    for i, s in enumerate(sigs):
        sub = ph[ph["id"] == s].sort_values("h")
        traces.append(Line(x=sub["h"].to_numpy(float), y=sub["value"].to_numpy(float),
                           color=colors[i], width=1.2, label=str(s)))
    # No inline label: the backend draws HLine labels at the line, which sits
    # inside the data here. The 0.25-uniform meaning lives in the subtitle instead.
    traces.append(HLine(y=0.25, color=(0.72, 0.11, 0.11), style="--"))
    if h_settle is not None:
        traces.append(_settle_vline(h_settle))
    ax.traces = traces
    return Figure(name="simnet_phase_frac",
                  title="Phase-bias SD fractional residual — integer structure on the wire",
                  subtitle="in the INT posture the SD must sit at integer×λ; "
                           "med|frac| pinned near 0.25 means the product carries no integer info",
                  axes=[ax], width=12.0, height=4.0)


# --------------------------------------------------------------------------- #
def _summary(df, h_settle) -> str:
    out = ["=== Simulated network truth-error series ==="]
    if h_settle is not None:
        out.append(f"settle boundary at t = {h_settle:.2f} h "
                   "(stats below are settled-window only)")
    settled = df if h_settle is None else df[df["h"] >= h_settle]

    def med(name):
        v = settled.loc[settled["channel"] == name, "value"].to_numpy(float)
        return float(np.median(v)) if v.size else None

    rows = [("clk_rms", "clock rms error (m)"),
            ("clk_sig_med", "clock published σ (m)"),
            ("cb_rms", "code-bias rms error (m)"),
            ("rxdcb_contrast_rms", "rx DCB contrast rms (m)"),
            ("rxdcb_level", "rx DCB level (m, gauge)"),
            ("rxclk_contrast_rms", "rx clock contrast rms (m)")]
    for ch, label in rows:
        m = med(ch)
        if m is not None:
            out.append(f"  settled median {label:<28}: {m:+.4f}")
    z = settled[settled["channel"] == "zwd_e"]
    if len(z):
        out.append(f"  settled ZWD |error| p50 (m)               : "
                   f"{float(np.median(np.abs(z['value'].to_numpy(float)))):.4f} "
                   f"over {z['id'].nunique()} stations")
    p = settled[settled["channel"] == "phase_frac_med"]
    for sig in sorted(p["id"].astype(str).unique()):
        v = p.loc[p["id"] == sig, "value"].to_numpy(float)
        out.append(f"  settled med|frac| {sig:<25}: {float(np.median(v)):.3f} cyc")
    return "\n".join(out)


viewer = Viewer(name="simnet_truth_series",
                title="Simulated network — truth error time series",
                required_csvs=("simnet_grade_series.csv",),
                build=build_simnet_truth_series)
