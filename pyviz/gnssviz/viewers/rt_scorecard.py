"""Truth-referenced run scorecard against a *surveyed* reference position.

The exam dashboard grades a run against the GnssSimulator answer key; a rooftop
/ field run has no answer key, but a **static receiver on a surveyed mark**
has the next best thing: the config's ``receiver.reference_position_ecef``.
This viewer is the headline accuracy panel for such runs (the real-time replay
and live-rooftop lineages foremost), grading the ``state_trace`` solution about
that fixed point rather than about its own median (which is all the
self-referenced ``state`` viewer can do).

Reference resolution order (see :class:`~gnssviz.trace_paths.TraceContext`):
an explicit ``ctx.ref_ecef`` (CLI ``--ref-ecef``), else
``receiver.reference_position_ecef`` from ``ctx.run_config`` (CLI
``--run-config``, comments tolerated).  Without either the viewer auto-skips,
so every existing report is unchanged.

Panels
------
(a) truth-referenced E / N / U + 3D error vs time (±1 m frame; the cold-start
    transient clips rather than rescaling the steady state away);
(b) 3D + horizontal error on a log axis with the ``3·σ3D`` covariance envelope
    (``σ3D = sqrt(x_std² + y_std² + z_std²)``, rotation-invariant) and the
    convergence ladder marked;
(c) post-convergence horizontal (E, N) scatter, time-coloured, with 50 % / 95 %
    error circles: the "how big is the puddle" workshop figure;
(d) post-convergence horizontal & 3D error CDFs.

Convergence is *suffix-stable*: converged-at-``thr`` means the 3D error stays
``≤ thr`` from that epoch to the end of the run (a dip that later diverges does
not count).  The ladder is 1.0 / 0.5 / 0.2 / 0.1 m; steady-state statistics run
from the coarsest achieved rung (falling back to the second half, flagged in
the summary, when even 1.0 m is never held).

:func:`rt_banner_html` renders the same metrics as a coloured banner + chip row
for the top of the report (the real-time analogue of the exam's Verifier
banner).  Like every viewer, this module imports no plotting library.
"""

from __future__ import annotations

import html
import os
from typing import Optional

import numpy as np

from ..colormaps import strip_colormap
from ..figures import Axes, ColorBar, Figure, HLine, Legend, Line, Scatter, TextBox, VLine
from ..geodesy import ecef_to_enu, ecef_to_lla
from ..io import add_utc, first_col, load_trace
from .. import runconfig
from .base import Viewer, ViewerResult, ViewerSkip

#: Suffix-stability convergence ladder (metres, 3D), coarse -> fine.
THRESHOLDS = (1.0, 0.5, 0.2, 0.1)

_C_E = (0.0, 0.0, 1.0)
_C_N = (0.0, 0.6, 0.0)
_C_U = (1.0, 0.0, 0.0)
_C_3D = (0.0, 0.0, 0.0)
_C_H = (0.15, 0.35, 0.75)
_C_SIG = (0.85, 0.20, 0.20)
_C_CONV = (0.45, 0.45, 0.45)


# --------------------------------------------------------------------------- #
#  Reference resolution + metric computation (shared by viewer and banner)
# --------------------------------------------------------------------------- #
def resolve_reference(ctx):
    """``(x, y, z, source_str)`` of the surveyed reference, or ``None``.

    Precedence: explicit ``ctx.ref_ecef`` beats the run config's
    ``receiver.reference_position_ecef`` (same explicit-wins rule as the path
    fields in the C++ config).
    """
    ref = getattr(ctx, "ref_ecef", None)
    if ref is not None:
        return (float(ref[0]), float(ref[1]), float(ref[2]), "--ref-ecef")
    cfg = runconfig.load_run_config(getattr(ctx, "run_config", None))
    xyz = runconfig.ref_ecef_from_config(cfg)
    if xyz is not None:
        src = os.path.basename(getattr(ctx, "run_config", "") or "run config")
        return (xyz[0], xyz[1], xyz[2], f"{src}: receiver.reference_position_ecef")
    return None


def _suffix_converged_index(err3d: np.ndarray, thr: float) -> Optional[int]:
    """First index from which ``err3d`` stays ``<= thr`` to the end (else None).

    Non-finite samples count as unconverged (a run must *hold* the bound
    through every graded epoch, not merely between dropouts).
    """
    e = np.where(np.isfinite(err3d), err3d, np.inf)
    if e.size == 0:
        return None
    sfx = np.maximum.accumulate(e[::-1])[::-1]      # sfx[i] = max(e[i:])
    ok = sfx <= thr
    if not ok.any():
        return None
    return int(np.argmax(ok))


def compute_metrics(ctx) -> Optional[dict]:
    """All scorecard quantities from ``state_trace.csv`` + the reference.

    Returns ``None`` when no reference resolves or the trace is unusable;
    callers (the viewer, :func:`rt_banner_html`) degrade to skip/empty.
    """
    ref = resolve_reference(ctx)
    if ref is None:
        return None
    p = ctx.path("state_trace.csv")
    if not ctx.has_data("state_trace.csv"):
        return None
    df = add_utc(load_trace(p))
    x = first_col(df, "x_m", "x")
    y = first_col(df, "y_m", "y")
    z = first_col(df, "z_m", "z")
    if x is None or y is None or z is None:
        return None
    x, y, z = (v.to_numpy(float) for v in (x, y, z))
    fin = np.isfinite(x) & np.isfinite(y) & np.isfinite(z)
    if fin.sum() < 2:
        return None
    df = df.loc[fin].reset_index(drop=True)
    x, y, z = x[fin], y[fin], z[fin]
    t = df["utc"]
    tow = first_col(df, "tow_s", "tow").to_numpy(float)

    rx, ry, rz, ref_src = ref
    lat, lon, hgt = ecef_to_lla(rx, ry, rz)
    e, n, u = ecef_to_enu(x, y, z, lat, lon, hgt)   # about the truth point => the error
    err3d = np.sqrt(e ** 2 + n ** 2 + u ** 2)
    errh = np.hypot(e, n)

    # 3-sigma envelope (rotation-invariant; None when the trace lacks stds).
    xs, ys, zs = (first_col(df, c) for c in ("x_std_m", "y_std_m", "z_std_m"))
    sig3 = (np.sqrt(xs.to_numpy(float) ** 2 + ys.to_numpy(float) ** 2
                    + zs.to_numpy(float) ** 2)
            if xs is not None and ys is not None and zs is not None else None)

    conv = {thr: _suffix_converged_index(err3d, thr) for thr in THRESHOLDS}
    t0 = tow[0]
    conv_s = {thr: (tow[i] - t0 if i is not None else None)
              for thr, i in conv.items()}

    # Steady-state window: from the coarsest achieved rung; else second half.
    w0, w_src = None, ""
    for thr in THRESHOLDS:                          # coarse -> fine
        if conv[thr] is not None:
            w0, w_src = conv[thr], f"converged ≤{thr:g} m"
            break
    if w0 is None:
        w0, w_src = len(err3d) // 2, "second half (never held ≤1.0 m)"

    def _stats(v):
        w = np.asarray(v)[w0:]
        w = w[np.isfinite(w)]
        if w.size == 0:
            return None
        return {"mean": float(np.mean(w)), "rms": float(np.sqrt(np.mean(w ** 2))),
                "p50": float(np.median(w)), "p90": float(np.percentile(w, 90)),
                "p95": float(np.percentile(w, 95)), "max": float(np.max(w))}

    span_s = float(tow[-1] - tow[0])
    steps = np.diff(tow)
    rate_s = float(np.median(steps)) if steps.size else float("nan")

    return {
        "t": t, "tow": tow, "e": e, "n": n, "u": u,
        "err3d": err3d, "errh": errh, "sig3": sig3,
        "conv": conv, "conv_s": conv_s, "w0": int(w0), "w_src": w_src,
        "stats": {"E": _stats(e), "N": _stats(n), "U": _stats(u),
                  "H": _stats(errh), "3D": _stats(err3d)},
        "final3d": float(err3d[-1]), "n_epochs": int(len(err3d)),
        "span_s": span_s, "rate_s": rate_s,
        "ref": (rx, ry, rz), "ref_src": ref_src,
        "ref_lla": (float(np.asarray(lat)), float(np.asarray(lon)),
                    float(np.asarray(hgt))),
    }


def _fmt_dur(seconds: Optional[float]) -> str:
    if seconds is None or not np.isfinite(seconds):
        return "—"                             # em dash: never achieved
    if seconds < 90.0:
        return f"{seconds:.0f} s"
    return f"{seconds / 60.0:.1f} min"


# --------------------------------------------------------------------------- #
#  Figures
# --------------------------------------------------------------------------- #
def _circle(r: float, label: Optional[str], style: str = "--"):
    th = np.linspace(0.0, 2.0 * np.pi, 181)
    return Line(x=r * np.cos(th), y=r * np.sin(th), color=(0.35, 0.35, 0.35),
                style=style, width=1.0, label=label)


def build_rt_scorecard(ctx) -> ViewerResult:
    m = compute_metrics(ctx)
    if m is None:
        raise ViewerSkip("no surveyed reference (give --run-config with "
                         "receiver.reference_position_ecef, or --ref-ecef X Y Z)")

    t, e, n, u = m["t"], m["e"], m["n"], m["u"]
    err3d, errh, sig3 = m["err3d"], m["errh"], m["sig3"]
    st = m["stats"]
    figures = []

    # Convergence ladder VLines.  Only the finest achieved rung carries a label:
    # the rungs sit seconds apart on a 1 Hz run, and both backends draw VLine
    # labels as top-anchored annotations, so a label per rung overlaps into an
    # unreadable stack.  The full ladder is in the banner chips + summary text
    # instead.
    conv_lines, labelled = [], False
    for thr in reversed(THRESHOLDS):                # fine -> coarse
        i = m["conv"][thr]
        if i is not None:
            conv_lines.append(VLine(
                x=t.iloc[i], color=_C_CONV, style=":", width=1.1,
                label=(f"≤{thr:g} m @ {_fmt_dur(m['conv_s'][thr])}"
                       if not labelled else None)))
            labelled = True

    # (a) ENU + 3D vs time on a fixed ±1 m frame (transient clips, steady state legible).
    axa = Axes(title=f"Position error vs surveyed reference ({m['ref_src']})",
               xlabel="GPS Time", ylabel="Error (m)",
               legend=Legend(loc="best"), ylim=(-1.0, 1.0))
    axa.traces = [
        Line(x=t, y=e, color=_C_E, width=1.0, label="E"),
        Line(x=t, y=n, color=_C_N, width=1.0, label="N"),
        Line(x=t, y=u, color=_C_U, width=1.0, label="U"),
        Line(x=t, y=err3d, color=_C_3D, width=1.4, label="3D"),
    ] + conv_lines
    ypos = 0.97
    for lbl, col in (("E", _C_E), ("N", _C_N), ("U", _C_U)):
        s = st[lbl]
        if s:
            axa.traces.append(TextBox(
                x=0.02, y=ypos, mono=True, bold=True, color=col,
                text=f"{lbl}: mean {s['mean'] * 100:+5.1f} cm  RMS {s['rms'] * 100:5.1f} cm"))
            ypos -= 0.045
    figures.append(Figure(name="Position error (reference)", axes=[axa],
                          width=13, height=4.4))

    # (b) log-scale convergence: 3D + horizontal + 3·σ3D envelope.
    axb = Axes(title="Convergence — 3D / horizontal error vs 3·σ3D envelope",
               xlabel="GPS Time", ylabel="Error (m, log)", yscale="log",
               legend=Legend(loc="best"))
    axb.traces = [
        Line(x=t, y=np.where(err3d > 0, err3d, np.nan), color=_C_3D, width=1.4,
             label="3D"),
        Line(x=t, y=np.where(errh > 0, errh, np.nan), color=_C_H, width=1.1,
             label="horizontal"),
    ]
    if sig3 is not None:
        axb.traces.append(Line(x=t, y=3.0 * sig3, color=_C_SIG, style="--",
                               width=1.2, label="3·σ3D"))
    axb.traces += conv_lines
    figures.append(Figure(name="Convergence (log)", axes=[axb], width=13, height=4.2))

    # (c) post-convergence horizontal scatter, time-coloured, 50/95 % circles.
    w0 = m["w0"]
    ecm, ncm = e[w0:] * 100.0, n[w0:] * 100.0
    hcm = errh[w0:] * 100.0
    hfin = hcm[np.isfinite(hcm)]
    if hfin.size:
        r50, r95 = np.percentile(hfin, 50), np.percentile(hfin, 95)
        minutes = (m["tow"][w0:] - m["tow"][0]) / 60.0
        lim = max(1.25 * r95, 1.0)
        axc = Axes(title=f"Horizontal scatter, steady state ({m['w_src']})",
                   xlabel="East (cm)", ylabel="North (cm)",
                   x_is_datetime=False, aspect_equal=True,
                   xlim=(-lim, lim), ylim=(-lim, lim),
                   cmap=strip_colormap("viridis"),
                   colorbar=ColorBar(label="minutes since start"),
                   legend=Legend(loc="best"))
        axc.traces = [
            HLine(y=0.0, color=(0.75, 0.75, 0.75), style="-"),
            Scatter(x=ecm, y=ncm, c=minutes, size=9.0, alpha=0.75,
                    show_in_legend=False),
            _circle(r50, f"50% → {r50:.1f} cm"),
            _circle(r95, f"95% → {r95:.1f} cm", style=":"),
        ]
        figures.append(Figure(name="Horizontal scatter", axes=[axc],
                              width=7.2, height=6.6))

        # (d) steady-state error CDFs (horizontal + 3D).
        d3cm = err3d[w0:] * 100.0
        d3cm = d3cm[np.isfinite(d3cm)]
        axd = Axes(title="Steady-state error CDF", xlabel="Error (cm)",
                   ylabel="Fraction of epochs ≤ x", x_is_datetime=False,
                   xscale="log", ylim=(0.0, 1.0), legend=Legend(loc="best"))
        cdfs = [(np.sort(hfin), _C_H, "horizontal")]
        if d3cm.size:
            cdfs.append((np.sort(d3cm), _C_3D, "3D"))
        axd.traces = [Line(x=v, y=(np.arange(1, v.size + 1)) / v.size,
                           color=c, width=1.4, label=lbl)
                      for v, c, lbl in cdfs]
        axd.traces += [HLine(y=0.5, color=(0.6, 0.6, 0.6), style=":"),
                       HLine(y=0.95, color=(0.6, 0.6, 0.6), style=":")]
        figures.append(Figure(name="Error CDF", axes=[axd], width=7.2, height=4.2))

    return ViewerResult(figures=figures, summary=_summary(m))


def _summary(m: dict) -> str:
    st = m["stats"]
    out = ["=== Run scorecard vs surveyed reference ===",
           f"Reference        : {m['ref'][0]:.3f}, {m['ref'][1]:.3f}, "
           f"{m['ref'][2]:.3f} m ECEF  ({m['ref_src']})",
           f"Epochs           : {m['n_epochs']} @ {m['rate_s']:g} s "
           f"(span {_fmt_dur(m['span_s'])})",
           "Convergence (3D suffix-stable: holds the bound to run end):"]
    for thr in THRESHOLDS:
        out.append(f"  ≤{thr:4g} m       : {_fmt_dur(m['conv_s'][thr])}")
    out.append(f"Steady-state window: from {m['w_src']} "
               f"(epoch {m['w0']}/{m['n_epochs']})")
    for lbl in ("E", "N", "U"):
        s = st[lbl]
        if s:
            out.append(f"  {lbl} bias / RMS  : {s['mean'] * 100:+6.1f} / "
                       f"{s['rms'] * 100:6.1f} cm")
    for lbl in ("H", "3D"):
        s = st[lbl]
        if s:
            out.append(f"  {lbl:<2} p50/p95/max: {s['p50'] * 100:6.1f} / "
                       f"{s['p95'] * 100:6.1f} / {s['max'] * 100:6.1f} cm")
    out.append(f"Final 3D error   : {m['final3d'] * 100:.1f} cm")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
#  Report banner (real-time analogue of the exam Verifier banner)
# --------------------------------------------------------------------------- #
def rt_banner_html(ctx) -> str:
    """Coloured summary banner + metric chip row for the top of the report.

    Trusted HTML for ``build_html_report``'s ``extra_html`` (caller-generated,
    same contract as :func:`~gnssviz.viewers.exam_common.verifier_banner_html`).
    Green when the run converged ≤0.5 m 3D and held it; neutral otherwise
    (an unconverged short run is information, not a failure verdict).  ``""``
    when no reference/trace resolves, so non-RT reports are unchanged.
    """
    m = compute_metrics(ctx)
    if m is None:
        return ""
    st = m["stats"]
    conv05 = m["conv_s"].get(0.5)
    good = conv05 is not None
    cls = " pass" if good else ""
    style = "" if good else " style='background:#eef1f4;color:#33414e;border:1px solid #d5dbe1'"
    head = (f"converged ≤0.5 m 3D in {_fmt_dur(conv05)}" if good
            else "did not hold ≤0.5 m 3D")
    verdict = (f"RUN vs SURVEYED REFERENCE — {m['n_epochs']} epochs @ "
               f"{m['rate_s']:g} s · span {_fmt_dur(m['span_s'])} · {head}")
    banner = (f"<div class='banner{cls}'{style}>{html.escape(verdict)}"
              f" <span style='font-weight:400'>&mdash; "
              f"{html.escape(m['ref_src'])}</span></div>")

    chips = []

    def chip(label: str, tip: str = ""):
        chips.append(
            f"<span title='{html.escape(tip)}' style='display:inline-block;"
            f"padding:1px 8px;margin:2px;border-radius:10px;font-size:0.78rem;"
            f"background:#e8eef5;color:#1b3a5c'>{html.escape(label)}</span>")

    for thr in THRESHOLDS:
        chip(f"≤{thr:g} m: {_fmt_dur(m['conv_s'][thr])}",
             "3D convergence, suffix-stable (holds the bound to run end)")
    for lbl, key, name in (("H", "H", "horizontal"), ("U", "U", "vertical"),
                           ("3D", "3D", "3D")):
        s = st[key]
        if s:
            chip(f"{lbl} RMS {s['rms'] * 100:.1f} cm",
                 f"steady-state {name} RMS ({m['w_src']}); "
                 f"p95 {s['p95'] * 100:.1f} cm")
    chip(f"final 3D {m['final3d'] * 100:.1f} cm", "error at the last epoch")
    return banner + f"<div style='margin:2px 0 14px'>{''.join(chips)}</div>"


viewer = Viewer(name="rt_scorecard", title="Run scorecard (vs surveyed reference)",
                required_csvs=("state_trace.csv",), build=build_rt_scorecard)
