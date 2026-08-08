"""Station-keeping window — what a FIXED-POINT solve recovers over the first 10 minutes.

The exam's opening epochs are truth-stationary (`scenario_summary.json` ->
``static_segment``; ``trajectory.static_prefix_s`` in the sim config), and the brief
discloses that, so Parts B and C both ask for a single static position over exactly
this window.  This viewer is the author-side answer to those items: it takes the same
three solutions the dashboard already shows per-epoch and collapses each one over the
window the way a student would, then draws what averaging did and did not fix.

  (a) per-epoch 3D error across the window on a log axis, with each solution's
      **static** (window-mean) 3D error as a dotted line.  A solution whose dotted
      line sits well below its own scatter was noise-limited and averaging paid;
      one whose line lands in the middle of its scatter was bias-limited and
      averaging bought nothing;
  (b) the same result decomposed into horizontal scatter about truth, at leg-1
      scale and again zoomed to the precise solutions, with the static fix marked.

The teaching point the rubric grades (ANSWER_KEY, Part B's station-keeping item):
averaging N epochs beats the NOISE down by ~sqrt(N) and does nothing whatever to a
BIAS common to every epoch in the window.  Leg 1's ionospheric error is exactly such
a bias, so its static fix is as wrong as its epoch-wise fixes while now carrying a
confidently small formal sigma -- Part D's covariance lesson from the other side.

Truth-only inputs plus the processor trace; auto-skips without ``--truth-dir``, and
without a ``static_segment`` (an instance with no station-keeping prefix has nothing
to draw).
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox
from ..io import first_col, gps_to_datetime, load_trace
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

#: Same leg colours as the Part-B reference panels, plus one for the PPP track, so a
#: reader carries the identity across the dashboard.
_C_L1 = (0.80, 0.25, 0.12)
_C_IF = (0.10, 0.35, 0.75)
_C_PPP = (0.10, 0.55, 0.25)
_C_REF = (0.45, 0.45, 0.45)
_C_TRUTH = (0.0, 0.0, 0.0)


def _window(ctx):
    """``(tow0, tow1, n_epochs)`` of the station-keeping window, or skip."""
    ss = ec.scenario_summary(ctx) or {}
    seg = ss.get("static_segment") if isinstance(ss, dict) else None
    if not isinstance(seg, dict) or not seg.get("n_epochs"):
        raise ViewerSkip("no static_segment in scenario_summary.json "
                         "(instance has no station-keeping prefix)")
    return (float(seg["tow_start_s"]), float(seg["tow_end_s"]), int(seg["n_epochs"]))


def _collapse(name, e, n, u, color):
    """One solution over the window -> the numbers a fixed-point solve produces.

    The window truth is a single point, so the static solution's error is exactly the
    MEAN of the per-epoch errors -- no need to reconstruct positions.  ``scatter`` is
    the per-epoch spread about that mean (the part averaging removes); ``bias`` is the
    mean itself (the part it cannot touch).
    """
    e, n, u = (np.asarray(v, float) for v in (e, n, u))
    fin = np.isfinite(e) & np.isfinite(n) & np.isfinite(u)
    e, n, u = e[fin], n[fin], u[fin]
    if e.size == 0:
        return None
    per_epoch = np.sqrt(e ** 2 + n ** 2 + u ** 2)
    me, mn, mu = e.mean(), n.mean(), u.mean()
    resid = np.sqrt((e - me) ** 2 + (n - mn) ** 2 + (u - mu) ** 2)
    return dict(name=name, color=color, n=int(e.size), e=e, n_=n, u=u,
                per_epoch=per_epoch, med=float(np.median(per_epoch)),
                bias=float(np.sqrt(me ** 2 + mn ** 2 + mu ** 2)),
                bias_e=float(me), bias_n=float(mn), bias_u=float(mu),
                scatter=float(np.sqrt((resid ** 2).mean())))


def _ppp_errors(ctx, tow0, tow1):
    """PPP per-epoch ENU errors inside the window (None when the trace is absent)."""
    truth = ec.truth_frame(ctx, "truth_position.csv")
    if truth is None:
        return None
    try:
        state = load_trace(ctx.path("state_trace.csv"))
    except (OSError, ValueError):
        return None
    if state is None or state.empty:
        return None
    m = ec.merge_truth_trace(truth, state)
    if m.empty:
        return None
    tow = first_col(m, "tow_s_truth", "tow_s")
    if tow is None:
        return None
    sel = (tow.to_numpy(float) >= tow0 - 1e-3) & (tow.to_numpy(float) <= tow1 + 1e-3)
    if not sel.any():
        return None
    w = m[sel]
    e, n, u = ec.enu_error(first_col(w, "x_m_trace"), first_col(w, "y_m_trace"),
                           first_col(w, "z_m_trace"), first_col(w, "x_m_truth"),
                           first_col(w, "y_m_truth"), first_col(w, "z_m_truth"),
                           first_col(w, "lat_deg_truth", "lat_deg"),
                           first_col(w, "lon_deg_truth", "lon_deg"))
    wk = first_col(w, "week_truth", "week")
    tw = first_col(w, "tow_s_truth", "tow_s")
    return e, n, u, gps_to_datetime(wk, tw)


def _scatter_axes(sols, *, title, lim, note=None):
    """Horizontal (E,N) scatter about truth with each solution's static fix marked."""
    ax = Axes(title=title, xlabel="East error (m)", ylabel="North error (m)",
              x_is_datetime=False, aspect_equal=True,
              xlim=(-lim, lim), ylim=(-lim, lim), legend=Legend(loc="best"))
    ax.traces = []
    for s in sols:
        ax.traces.append(Scatter(x=s["e"], y=s["n_"], color=s["color"], size=14,
                                 alpha=0.35, label=f"{s['name']} epochs"))
    for s in sols:
        # The static fix: a big ringed marker so it reads over its own cloud.
        ax.traces.append(Scatter(x=[s["bias_e"]], y=[s["bias_n"]], color=s["color"],
                                 size=110, marker="*", edgecolor=(0, 0, 0), linewidth=0.8,
                                 label=f"{s['name']} static fix"))
    ax.traces.append(Scatter(x=[0.0], y=[0.0], color=_C_TRUTH, size=70, marker="+",
                             label="truth"))
    if note:
        ax.traces.append(TextBox(x=0.02, y=0.97, mono=True, boxed=True, text=note))
    return ax


def build_exam_static_window(ctx) -> ViewerResult:
    tow0, tow1, n_disclosed = _window(ctx)
    spp = ec.truth_frame(ctx, "truth_spp.csv")
    sols, t_axis = [], None

    if spp is not None and {"tow_s"}.issubset(spp.columns):
        tw = spp["tow_s"].to_numpy(float)
        sel = (tw >= tow0 - 1e-3) & (tw <= tow1 + 1e-3)
        if sel.any():
            w = spp[sel]
            t_axis = gps_to_datetime(w["week"], w["tow_s"]) if "week" in w.columns else None
            for pre, name, color in (("l1", "leg 1 (L1 + brdc + Klob)", _C_L1),
                                     ("if", "leg 2 (IF + precise)", _C_IF)):
                cols = [f"{pre}_e_m", f"{pre}_n_m", f"{pre}_u_m"]
                if all(c in w.columns for c in cols):
                    s = _collapse(name, w[cols[0]], w[cols[1]], w[cols[2]], color)
                    if s:
                        s["t"] = t_axis
                        sols.append(s)

    ppp = _ppp_errors(ctx, tow0, tow1)
    if ppp is not None:
        e, n, u, t = ppp
        # Labelled "kinematic, mean" on purpose: the processor estimates a NEW position
        # every epoch, so its window mean is not the static solve the exam asks for --
        # a true fixed point carries one position instead of 21 and is better
        # determined.  Read this series as an upper bound on the static item, and note
        # that over the FIRST 21 epochs it is dominated by filter convergence, not noise.
        s = _collapse("PPP (kinematic, mean)", e, n, u, _C_PPP)
        if s:
            s["t"] = t
            sols.append(s)
            t_axis = t_axis if t_axis is not None else t

    if not sols:
        raise ViewerSkip("no solution covers the station-keeping window")

    figures = []

    # ------------------------------------------------ (a) per-epoch vs static
    # Log y: leg 1 sits ~3 decades above the PPP fix, so a linear axis shows one
    # of the three and a flat line for the others.
    # Headroom above the data so the read-out box does not sit on top of the traces
    # (leg 1 runs flat near the top of its own range).
    allv = np.concatenate([s["per_epoch"][np.isfinite(s["per_epoch"])] for s in sols])
    allv = allv[allv > 0]
    ylim = ((0.6 * float(allv.min()), 3.5 * float(allv.max()))
            if allv.size else None)
    axa = Axes(title=f"Station-keeping window ({n_disclosed} epochs, tow {tow0:.0f}"
                     f" to {tow1:.0f} s GPS): per-epoch 3D error, and the static fix "
                     f"(dotted) each one collapses to",
               xlabel="GPS Time", ylabel="3D error (m, log)", yscale="log", ylim=ylim,
               legend=Legend(loc="best"))
    axa.traces = []
    for s in sols:
        y = np.where(s["per_epoch"] > 0, s["per_epoch"], np.nan)
        x = s["t"] if s.get("t") is not None else np.arange(s["n"])
        axa.traces.append(Line(x=x, y=y, color=s["color"], width=1.2,
                               label=f"{s['name']} per epoch"))
        if s["bias"] > 0:
            axa.traces.append(HLine(y=s["bias"], color=s["color"], style=":"))
    axa.traces.append(TextBox(
        x=0.02, y=0.97, mono=True, boxed=True,
        text="\n".join(
            f"{s['name'][:22]:<22} epoch {s['med']:8.3f}  static {s['bias']:8.3f} m"
            for s in sols)
        + f"\naveraging {sols[0]['n']} epochs: noise /{np.sqrt(sols[0]['n']):.1f}, bias /1"))
    figures.append(Figure(name="Static window: per-epoch vs static fix",
                          axes=[axa], width=13, height=4.6))

    # ------------------------------------------------------- (b) E/N scatter
    # Two panels because the solutions differ by ~3 decades: one at leg-1 scale so
    # the comparison is honest, one zoomed so the precise fixes are more than a dot.
    full = max(float(np.nanmax(np.abs(np.concatenate([s["e"], s["n_"]])))) for s in sols)
    precise = [s for s in sols if s is not sols[0]] or sols
    zoom = max(float(np.nanmax(np.abs(np.concatenate([s["e"], s["n_"]])))) for s in precise)
    # The horizontal view alone would MISLEAD on this dataset: leg 1's error is
    # vertical-dominated (the ionosphere projects onto Up), so in plan view it sits
    # among the precise solutions while being metres wrong.  Carry each static fix's
    # E/N/U in the box so the plane cannot be read as the whole story.
    note = ("star = the static (window-mean) fix, + = truth\n"
            "static fix E / N / U (m):\n"
            + "\n".join(f"  {s['name'][:22]:<22}{s['bias_e']:+7.2f}{s['bias_n']:+7.2f}"
                        f"{s['bias_u']:+7.2f}" for s in sols)
            + "\n(plan view only: the Up column is where leg 1's error lives)")
    axes_b = [_scatter_axes(sols, title="Horizontal scatter about truth, full scale",
                            lim=1.1 * full if full > 0 else 1.0, note=note)]
    if zoom > 0 and full > 3.0 * zoom:
        axes_b.append(_scatter_axes(
            precise, lim=1.2 * zoom,
            title=f"Zoom to the precise solutions ({full / zoom:.0f}x)"))
    figures.append(Figure(name="Static window: horizontal scatter", axes=axes_b,
                          width=13, height=5.6 if len(axes_b) == 1 else 10.4))

    return ViewerResult(figures=figures, summary=_summary(sols, tow0, tow1, n_disclosed))


def _summary(sols, tow0, tow1, n_disclosed) -> str:
    out = [f"=== Station-keeping window: fixed-point solutions "
           f"({n_disclosed} epochs, tow {tow0:.0f} to {tow1:.0f} s GPS) ===",
           "",
           "The window truth is ONE point, so a static solve is the mean of the per-epoch",
           "positions and its error is the mean of the per-epoch errors. Splitting that mean",
           "from the spread about it separates what averaging fixes from what it cannot:",
           "",
           f"  {'solution':<26}{'epoch med':>11}{'static 3D':>11}{'scatter':>10}"
           f"{'bias U':>10}{'gain':>8}"]
    for s in sols:
        gain = (s["med"] / s["bias"]) if s["bias"] > 0 else float("inf")
        out.append(f"  {s['name']:<26}{s['med']:>11.3f}{s['bias']:>11.3f}"
                   f"{s['scatter']:>10.3f}{s['bias_u']:>+10.3f}{gain:>8.1f}x")
    n = sols[0]["n"]
    out += ["",
            f"  scatter = per-epoch spread about the window mean (averaging divides this by",
            f"            ~sqrt({n}) = {np.sqrt(n):.2f}); static 3D = |mean error| = the BIAS,",
            "            which averaging leaves untouched.  gain = epoch median / static.",
            "",
            "A leg whose gain is ~1 was bias-limited: it is exactly as wrong after averaging,",
            "but its formal sigma has shrunk, so it now reports high confidence in a wrong",
            "position.  That is the Part-B station-keeping item and Part D's covariance",
            "lesson meeting in one number.",
            "",
            "CAVEAT on the PPP row: it is the KINEMATIC filter's window mean, not a static",
            "solve.  The processor estimates a new position every epoch, so over the first",
            "few minutes it is still converging its float ambiguities -- which is why its",
            "gain can fall BELOW 1 (the mean is dragged by the early epochs, so it is worse",
            "than the per-epoch median).  A student's true fixed point carries ONE position",
            "instead of 21 and should beat this; read the row as an upper bound on the",
            "Part-C item, and do not grade a student against it as if it were the key."]
    return "\n".join(out)


viewer = Viewer(name="exam_static_window",
                title="Station-keeping window: static SPP & PPP fixes",
                required_csvs=("truth_spp.csv",),
                build=build_exam_static_window)
