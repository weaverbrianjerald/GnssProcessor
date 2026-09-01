"""Simulated-network theory panels — the formulations behind the report's numbers.

Three text/math figures (matplotlib mathtext, fully offline) that make the report
self-contained for teaching: the uncombined observation model and its rank
defect, the S-basis THIS run realized (read off the product's S line, with the
wire-record contracts as estimable lumps), and the truth-projection grading
method.  Everything quantitative is quoted from the run's ``simnet_grade.json``
so the equations and the panels above them can never disagree about the datum.

Lights up only when a simnet grade is present, so ordinary runs are unchanged.
"""

from __future__ import annotations

import json

from ..figures import Axes, Figure, TextBox
from .base import Viewer, ViewerResult

# Line kinds: (fontsize, bold, mono, color, gap-weight before the line)
_KINDS = {
    "head": (13.0, True, False, (0.11, 0.23, 0.36), 1.9),
    "math": (12.5, False, False, (0.05, 0.05, 0.05), 1.6),
    "body": (10.0, False, False, (0.15, 0.15, 0.15), 1.2),
    "note": (9.0, False, False, (0.42, 0.42, 0.42), 1.1),
    "mono": (9.5, False, True, (0.15, 0.15, 0.15), 1.2),
    "gap": (1.0, False, False, (0, 0, 0), 0.8),
}


def _text_figure(name, title, subtitle, lines, width=12.0):
    """One frameless axes stacking (kind, text) lines top-down."""
    total = sum(_KINDS[k][4] for k, _ in lines) + 1.0
    ax = Axes(axis_off=True, x_is_datetime=False, grid=False,
              xlim=(0.0, 1.0), ylim=(0.0, 1.0))
    y = 1.0
    for kind, text in lines:
        size, bold, mono, color, gap = _KINDS[kind]
        y -= gap / total
        if kind == "gap" or not text:
            continue
        ax.traces.append(TextBox(x=0.02 if kind != "math" else 0.06, y=y, text=text,
                                 ha="left", va="top", fontsize=size, bold=bold,
                                 mono=mono, color=color, data_coords=True))
    height = max(2.2, 0.30 * sum(_KINDS[k][4] for k, _ in lines))
    return Figure(name=name, title=title, subtitle=subtitle, axes=[ax],
                  width=width, height=height, link_x=False)


def _fmt_pair(p):
    return f"{p[0]}/{p[1]}" if isinstance(p, (list, tuple)) and len(p) == 2 else str(p)


def build_simnet_theory(ctx) -> ViewerResult:
    with open(ctx.path("simnet_grade.json"), encoding="utf-8") as fh:
        g = json.load(fh)
    s_line = g.get("s_line") or {}
    pivots = s_line.get("pivots") or {}
    cbpin = s_line.get("cbpin") or {}
    gfpin = s_line.get("gfpin") or {}

    model = _text_figure(
        "simnet_theory_model",
        "The uncombined network model and its rank defect",
        "one equation pair per (receiver r, satellite s, band j) — no combinations, "
        "every physical parameter kept",
        [
            ("head", "Observation model"),
            ("math", r"$\mathrm{E}[p^{s}_{r,j}] \;=\; \rho^{s}_{r} \;+\; c\,(dt_{r} - dt^{s}) \;+\; m^{s}_{r}\,T_{r} \;+\; \mu_{j}\,\iota^{s}_{r} \;+\; d_{r,j} \;-\; d^{s}_{j}$"),
            ("math", r"$\mathrm{E}[\varphi^{s}_{r,j}] \;=\; \rho^{s}_{r} \;+\; c\,(dt_{r} - dt^{s}) \;+\; m^{s}_{r}\,T_{r} \;-\; \mu_{j}\,\iota^{s}_{r} \;+\; \lambda_{j}\,N^{s}_{r,j} \;+\; \delta_{r,j} \;-\; \delta^{s}_{j}$"),
            ("body", r"$\rho$ geometry (orbits held fixed) · $dt$ receiver/satellite clock · $T$ zenith wet delay with mapping $m$ ·"),
            ("body", r"$\iota$ slant ionosphere at $f_1$, scaled by $\mu_j = f_1^2/f_j^2$ · $d,\ \delta$ code/phase biases · $N \in \mathbb{Z}$ ambiguity"),
            ("gap", ""),
            ("head", "Rank defect — why a datum must be chosen"),
            ("body", "Clocks, biases, ionosphere and ambiguities share unobservable directions; the normal matrix is singular"),
            ("body", "by construction, not by data weakness.  Per CDMA constellation with f frequencies, n receivers, m satellites:"),
            ("math", r"$\#\,\mathrm{defects} \;=\; 1 \;+\; 2f \;+\; (1+f)(n-1+m)$   [Odijk et al. 2016, Eq. 24]"),
            ("note", "Scaling the roster cannot buy these directions back — more stations bring more ambiguities with them"),
            ("note", "(measured: the L2W phase-bias common mode is ~1/√N at n=50 AND n=300 — a structural gauge)."),
            ("gap", ""),
            ("body", "A minimum-constraints S-basis fixes exactly these directions.  Everything on the wire is then an"),
            ("body", "ESTIMABLE FUNCTION of the original parameters — a lump, never the bare parameter itself."),
        ])

    piv_txt = ", ".join(f"{c}: {s}" for c, s in sorted(pivots.items())) or "(none on the S line)"
    pin_txt = ", ".join(f"{c}: {_fmt_pair(p)}" for c, p in sorted(cbpin.items())) or "(none declared)"
    gf_txt = (", ".join(f"{c}: {_fmt_pair(p)}" for c, p in sorted(gfpin.items()))
              if gfpin else "(single-layer posture — no GF pin)")
    sbasis = _text_figure(
        "simnet_theory_sbasis",
        "The S-basis this run realized — wire-record contracts",
        "read off the product's own S line, never assumed (invariant #5)",
        [
            ("mono", f"clock pivot per constellation : {piv_txt}"),
            ("mono", f"reference receiver            : {s_line.get('ref_rx', '?')}"),
            ("mono", f"IF-pinned code pair (cbpin)   : {pin_txt}"),
            ("mono", f"geometry-free pin (gfpin)     : {gf_txt}"),
            ("gap", ""),
            ("head", "What each wire record MEANS (estimable lumps)"),
            ("math", r"$\mathrm{C:}\ \ \widetilde{dt}^{\,s} \;=\; \left[dt^{s} + d^{s}_{\mathrm{IF}}\right] \;-\; \left[dt^{\mathrm{piv}} + d^{\mathrm{piv}}_{\mathrm{IF}}\right] \;-\; A_{c}$"),
            ("note", "the satellite clock absorbs the IF code-bias lump of the pinned pair; the pivot's lump and the"),
            ("note", "common-mode accumulator subtract out of every graded difference"),
            ("math", r"$\mathrm{B:}\ \ \widetilde{d}^{\,s}_{j} \;=\; \left(d^{s}_{j} \;-\; \mu_{j}\,d^{s}_{\mathrm{GF}} \;-\; d^{s}_{\mathrm{IF}}\right) \;-\; \mathrm{mean}_{s}(\cdot)$"),
            ("note", "zero-meaned per (constellation, channel) over each epoch's registered set; the GF term folds only"),
            ("note", "in the slant posture, where both pinned channels project to exactly zero (Odijk §5.1)"),
            ("math", r"$\mathrm{P:}\ \ \widetilde{\delta}^{\,s}_{j} - \widetilde{\delta}^{\,s'}_{j} \;\in\; \lambda_{j}\,\mathbb{Z}$   (INT posture)"),
            ("note", "the between-satellite SD phase bias must be an integer number of cycles: the sharp wire test of"),
            ("note", "integer structure, graded as the fractional part (0.25 med|frac| = uniform = no integer info)"),
            ("gap", ""),
            ("body", "The datum is realized PER CONSTELLATION (one pivot, one IF pin, one zero-mean set each), and a datum"),
            ("body", "change is also a GRADER change: every truth projection above depends on these choices."),
        ])

    grading = _text_figure(
        "simnet_theory_grading",
        "Truth-projection grading — and why contrasts would answer backwards",
        "the first ABSOLUTE grade the producer has; every internal grade is satisfiable "
        "by a confidently wrong estimator",
        [
            ("head", "The method"),
            ("math", r"$e_{s} \;=\; \hat{x}_{s} \;-\; \mathcal{S}\!\left[x^{\mathrm{true}}\right]_{s}\,,\qquad vf \;=\; \mathrm{mean}\!\left(e_{s}^{2}/\sigma_{s}^{2}\right)$"),
            ("body", "Transform TRUTH into the producer's realized basis; compare in that basis, against the marginal,"),
            ("body", "datum-conditioned σ of exactly that quantity.  vf ≈ 1 means the published σ is honest."),
            ("gap", ""),
            ("head", "Why not difference the estimate into contrasts?"),
            ("math", r"$\sigma^{2}\!\left(\hat{x}_{s} - \hat{x}_{s'}\right) \;\neq\; \sigma_{s}^{2} + \sigma_{s'}^{2}$   (both share the datum ⇒ strongly correlated)"),
            ("body", "The published marginal σ OVERSTATES the contrast σ, so a contrast-based vf biases DOWNWARD —"),
            ("body", "it makes a mis-calibrated estimator look calibrated, answering the calibration question backwards."),
            ("note", "The datum-free contrast is kept as a CROSS-CHECK: it must agree, and disagreement means the"),
            ("note", "datum was misread (that is the failure it catches)."),
            ("gap", ""),
            ("head", "Reading rules the panels obey"),
            ("body", "• Settle first: the level channel's convergence budget is ~2 h — any level claim from a shorter"),
            ("body", "   window measures ignition, not the estimator (the settle boundary is drawn on every axis)."),
            ("body", "• Census before channels: a product of header/S records alone grades 'clean' on every empty channel."),
            ("body", "• Counters, not crashes: a tier with 0 opportunities is INERT, and says nothing at all."),
        ])

    summary = ("Theory panels: uncombined model + rank defect (Odijk Eq. 24), the realized "
               "S-basis wire contracts (C/B/P lumps), and truth-projection grading "
               f"(vf in the realized basis).  Realized pivots: {piv_txt}; cbpin: {pin_txt}.")
    return ViewerResult(figures=[model, sbasis, grading], summary=summary)


viewer = Viewer(name="simnet_theory",
                title="Simulated network — theoretical formulations",
                required_csvs=("simnet_grade.json",),
                build=build_simnet_theory)
