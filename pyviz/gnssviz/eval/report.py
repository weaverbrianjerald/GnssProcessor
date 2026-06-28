"""Scorecard / A-B / regression report for a test-suite run.

Renders a ``suite_metrics.json`` (written by ``scripts/run_test_suite.py``) into a
standalone HTML page.  Three layers, each its own section:

* **Scorecard** -- text (truth cross-check + per-cell ENU bias/RMS/convergence +
  A/B deltas vs each base's reference cell) plus bar figures of 3D-RMS and
  vertical bias per cell.
* **Regression vs golden** (when a golden ``suite_metrics.json`` is supplied) --
  the gate verdict, a per-cell delta table, and current-vs-golden / Δ-with-
  tolerance-band figures so a regression is visible at a glance.
* **Convergence** -- auto-generated 3D-error-vs-time curves rebuilt from each
  cell's ``state_trace.csv`` (one panel per station, one line per cell).

Reuses the backend-agnostic :mod:`gnssviz.figures` specs and
:func:`gnssviz.report.write_html_report`, so the matplotlib (static) and plotly
(interactive) backends both render it unchanged.
"""

from __future__ import annotations

import argparse
import html
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, Stem, TextBox, VLine
from ..palettes import distinguishable_colors
from ..report import write_html_report
from ..viewers.base import ViewerResult


# ---------------------------------------------------------------------------
# text scorecard
# ---------------------------------------------------------------------------

def _ok_metrics(suite: dict) -> List[dict]:
    return [m for m in suite.get("metrics", []) if m.get("ok")]


def _ab_groups(suite: dict) -> Dict[Tuple[str, str], Dict[str, dict]]:
    groups: Dict[Tuple[str, str], Dict[str, dict]] = {}
    for m in _ok_metrics(suite):
        groups.setdefault((m["station"], m.get("base", "")), {})[m["cell"]] = m
    return groups


def scorecard_text(suite: dict) -> str:
    out: List[str] = []
    out.append(f"date {suite.get('date','?')}   epoch {suite.get('epoch_decimal_year',0):.4f}"
               f"   warmup {suite.get('warmup_s','?')} s")

    cc = suite.get("cross_check") or []
    if cc:
        out.append("\nTruth cross-check (SINEX vs SIF):")
        for r in cc:
            flag = "ok " if r.get("ok") else "DIFF"
            d = r.get("dist_m")
            ds = "   n/a" if d is None or d != d else f"{d*100:6.2f} cm"
            note = f"   {r['note']}" if r.get("note") else ""
            out.append(f"  [{flag}] {r['station']:<10} |d|={ds}{note}")

    out.append("\nScorecard (ENU error vs independent truth; cm):")
    out.append(f"  {'station/cell':<26}{'biasE':>7}{'biasN':>7}{'biasU':>7}"
               f"{'3D-RMS':>9}{'H-RMS':>8}{'conv':>8}{'n':>7}")
    for m in suite.get("metrics", []):
        key = f"{m.get('station','?')}/{m.get('cell','?')}"
        if not m.get("ok"):
            out.append(f"  {key:<26}  FAILED: {m.get('reason','?')}")
            continue
        be, bn, bu = (v * 100 for v in m["bias_enu_m"])
        ttc = m.get("time_to_threshold_s")
        ttc_s = "n/a" if ttc is None else f"{ttc:.0f}s"
        out.append(f"  {key:<26}{be:>7.1f}{bn:>7.1f}{bu:>7.1f}"
                   f"{m['rms_3d_m']*100:>9.1f}{m['rms_horizontal_m']*100:>8.1f}"
                   f"{ttc_s:>8}{m['n_converged']:>7}")

    ref_cells = suite.get("ref_cells", {})
    groups = _ab_groups(suite)
    ab_lines: List[str] = []
    for (sta, base), cellmap in sorted(groups.items()):
        ref = ref_cells.get(base)
        ref_m = cellmap.get(ref) or cellmap[sorted(cellmap)[0]]
        ref_name = next(k for k, v in cellmap.items() if v is ref_m)
        for label, m in sorted(cellmap.items()):
            if m is ref_m:
                continue
            dE = (m["bias_enu_m"][0] - ref_m["bias_enu_m"][0]) * 100
            dN = (m["bias_enu_m"][1] - ref_m["bias_enu_m"][1]) * 100
            dU = (m["bias_enu_m"][2] - ref_m["bias_enu_m"][2]) * 100
            d3 = (m["rms_3d_m"] - ref_m["rms_3d_m"]) * 100
            ab_lines.append(f"  {sta:<6}{base:<8}{label:<18} vs {ref_name:<14}"
                            f"  dE{dE:+7.2f} dN{dN:+7.2f} dU{dU:+7.2f}  d3D{d3:+7.2f}")
    if ab_lines:
        out.append("\nA/B deltas vs base reference cell (cm):")
        out.extend(ab_lines)
    return "\n".join(out)


# ---------------------------------------------------------------------------
# bar figures
# ---------------------------------------------------------------------------

_C_RMS = (0.10, 0.30, 0.70)
_C_U = (0.80, 0.20, 0.20)


def scorecard_figures(suite: dict) -> List[Figure]:
    ok = _ok_metrics(suite)
    if not ok:
        return []
    labels = [f"{m['station']}/{m['cell']}" for m in ok]
    x = list(range(len(ok)))
    rms = [m["rms_3d_m"] * 100 for m in ok]
    ubias = [m["bias_enu_m"][2] * 100 for m in ok]

    # Vertical (newline-joined) index->cell legend; a single long horizontal
    # string collapses the constrained layout, a stacked block does not.
    legend = "\n".join(f"{i:>2}: {lab}" for i, lab in enumerate(labels))

    ax_rms = Axes(title="3D-RMS error vs truth (cm)", ylabel="cm",
                  x_is_datetime=False, xlabel="cell index")
    ax_rms.traces = [Stem(x=x, y=rms, color=_C_RMS, marker="o"),
                     TextBox(x=1.01, y=1.0, text=legend, mono=True, va="top",
                             color=(0.3, 0.3, 0.3))]
    ax_u = Axes(title="Vertical (Up) bias vs truth (cm)", ylabel="cm",
                x_is_datetime=False, xlabel="cell index")
    ax_u.traces = [Stem(x=x, y=ubias, color=_C_U, marker="o")]

    return [Figure(name="scorecard", title="Test-suite scorecard",
                   axes=[ax_rms, ax_u], width=13, height=6.5)]


# ---------------------------------------------------------------------------
# regression vs golden
# ---------------------------------------------------------------------------

_C_GOLD = (0.55, 0.55, 0.55)
_C_REG = (0.85, 0.15, 0.15)


def _ok_index(suite: dict) -> Dict[Tuple[str, str], dict]:
    from .regression import index_metrics
    return index_metrics(_ok_metrics(suite))


def regression_summary(current: dict, golden: dict, *,
                       bias_tol_m: float = 0.02, rms_tol_m: float = 0.02):
    """``(text, n_total, deltas)`` for the current-vs-golden regression gate.

    Mirrors the gate in ``scripts/run_test_suite.py`` exactly: a cell *regresses*
    on an ENU-bias / 3D-RMS / NaN-flip move beyond tolerance
    (:func:`gnssviz.eval.regression.compare_metrics`); a golden-``ok`` cell now
    failed or missing from the current run is a hard *broken*.  ``n_total =
    n_regressed + n_broken`` is the suite's non-zero exit count.
    """
    from .regression import compare_metrics

    cur_ok = _ok_metrics(current)
    gold_ok = _ok_metrics(golden)
    deltas, n_reg = compare_metrics(cur_ok, gold_ok,
                                    bias_tol_m=bias_tol_m, rms_tol_m=rms_tol_m)
    cur_keys = {(m["station"], m["cell"]) for m in cur_ok}
    broken = [(g["station"], g["cell"]) for g in gold_ok
              if (g["station"], g["cell"]) not in cur_keys]
    n_total = n_reg + len(broken)

    verdict = "FAIL" if n_total else "PASS"
    out: List[str] = [
        f"Regression gate: {verdict}   "
        f"({n_reg} regressed, {len(broken)} broken/missing;   "
        f"tol bias {bias_tol_m*100:.0f} cm, 3D-RMS {rms_tol_m*100:.0f} cm)",
        "",
    ]
    out.extend(d.one_line() for d in deltas)
    if broken:
        out.append("")
        out.append("Previously-passing cells now failed / diverged / missing:")
        out.extend(f"    BROKEN {sta}/{cell}" for sta, cell in broken)
    return "\n".join(out), n_total, deltas


def _delta_axes(title: str, x, dvals, tol: float) -> Axes:
    """A Δ-stem axes with a ±``tol`` reference band; out-of-band points reddened."""
    inside = [(xi, d) for xi, d in zip(x, dvals) if abs(d) <= tol]
    outside = [(xi, d) for xi, d in zip(x, dvals) if abs(d) > tol]
    ax = Axes(title=title, ylabel="cm", x_is_datetime=False, xlabel="cell index")
    ax.traces = [
        Stem(x=[xi for xi, _ in inside], y=[d for _, d in inside],
             color=_C_RMS, marker="o"),
        HLine(y=tol, color=_C_REG, style="--", label=f"±{tol:.0f} cm tol"),
        HLine(y=-tol, color=_C_REG, style="--"),
    ]
    if outside:
        ax.traces.append(Stem(x=[xi for xi, _ in outside], y=[d for _, d in outside],
                              color=_C_REG, marker="o"))
    return ax


def regression_figures(current: dict, golden: dict, *,
                       bias_tol_m: float = 0.02, rms_tol_m: float = 0.02) -> List[Figure]:
    """Current-vs-golden 3D-RMS + Δ-with-tolerance-band figures.

    Only cells present and ``ok`` in *both* runs are comparable; ``new`` /
    ``missing`` cells are surfaced in :func:`regression_summary` text instead.
    """
    cur, gold = _ok_index(current), _ok_index(golden)
    keys = sorted(set(cur) & set(gold))
    if not keys:
        return []
    labels = [f"{s}/{c}" for (s, c) in keys]
    x = list(range(len(keys)))
    cur_rms = [cur[k]["rms_3d_m"] * 100 for k in keys]
    gold_rms = [gold[k]["rms_3d_m"] * 100 for k in keys]
    d_rms = [c - g for c, g in zip(cur_rms, gold_rms)]
    d_u = [(cur[k]["bias_enu_m"][2] - gold[k]["bias_enu_m"][2]) * 100 for k in keys]

    legend = "\n".join(f"{i:>2}: {lab}" for i, lab in enumerate(labels))
    ax_rms = Axes(title="3D-RMS vs truth: current vs golden (cm)", ylabel="cm",
                  x_is_datetime=False, xlabel="cell index",
                  legend=Legend(loc="upper left"))
    ax_rms.traces = [
        Scatter(x=x, y=gold_rms, color=_C_GOLD, marker="s", size=45, label="golden"),
        Scatter(x=x, y=cur_rms, color=_C_RMS, marker="o", size=45, label="current"),
        TextBox(x=1.01, y=1.0, text=legend, mono=True, va="top", color=(0.3, 0.3, 0.3)),
    ]
    ax_drms = _delta_axes("Δ 3D-RMS vs golden (cm) — outside band = regression",
                          x, d_rms, rms_tol_m * 100)
    ax_du = _delta_axes("Δ Up-bias vs golden (cm) — outside band = regression",
                        x, d_u, bias_tol_m * 100)
    return [Figure(name="regression", title="Regression vs golden",
                   axes=[ax_rms, ax_drms, ax_du], width=13, height=10)]


# ---------------------------------------------------------------------------
# convergence (auto-figures rebuilt from each cell's state_trace.csv)
# ---------------------------------------------------------------------------

def convergence_figures(suite: dict, suite_dir) -> List[Figure]:
    """3D-error-vs-time curves per cell, one panel per station.

    Rebuilds each cell's ENU error from its ``state_trace.csv`` under
    ``<suite_dir>/<station>/<cell>/logs/`` (the layout
    ``scripts/run_test_suite.py`` writes), referenced to the truth ECEF stored in
    the metric.  Cells whose trace is missing/unreadable are silently skipped.
    """
    from .metrics import enu_error_series

    ok = _ok_metrics(suite)
    if not ok:
        return []
    threshold_m = float(suite.get("threshold_m", 0.10))
    warmup_s = suite.get("warmup_s")
    suite_dir = Path(suite_dir)

    by_station: Dict[str, List[dict]] = {}
    for m in ok:
        by_station.setdefault(m["station"], []).append(m)

    figs: List[Figure] = []
    for sta, ms in sorted(by_station.items()):
        ms = sorted(ms, key=lambda d: d["cell"])
        colors = distinguishable_colors(len(ms))
        traces, plotted = [], 0
        for i, m in enumerate(ms):
            csv = suite_dir / m["station"] / m["cell"] / "logs" / "state_trace.csv"
            es = (enu_error_series(str(csv), m.get("truth_ecef_m"))
                  if csv.is_file() and m.get("truth_ecef_m") else None)
            if es is None or es.elapsed_s.size == 0:
                continue
            plotted += 1
            traces.append(Line(x=(es.elapsed_s / 60.0).tolist(),
                               y=(es.err_3d * 100.0).tolist(),
                               label=m["cell"], color=colors[i], width=1.0))
        if not plotted:
            continue
        traces.append(HLine(y=threshold_m * 100.0, color=(0.15, 0.15, 0.15),
                            style=":", label=f"{threshold_m*100:.0f} cm threshold"))
        if warmup_s:
            traces.append(VLine(x=float(warmup_s) / 60.0, color=(0.6, 0.6, 0.6),
                               style="--", label=f"warmup {float(warmup_s)/60:.0f} min"))
        ax = Axes(title=f"{sta}: 3D position error vs time (per cell)",
                  ylabel="3D error (cm)", xlabel="elapsed (min)",
                  x_is_datetime=False, yscale="log", legend=Legend(loc="upper right"))
        ax.traces = traces
        figs.append(Figure(name=f"convergence_{sta}", title=f"{sta} convergence",
                           axes=[ax], width=12, height=4.8))
    return figs


# ---------------------------------------------------------------------------
# HTML
# ---------------------------------------------------------------------------

def _scorecard_result(suite: dict) -> ViewerResult:
    return ViewerResult(figures=scorecard_figures(suite), summary=scorecard_text(suite))


def _per_cell_links_html(suite: dict, suite_dir: Path) -> str:
    """Trusted-HTML links to each cell's per-run ``gnss_report.html``, when present.

    Returns ``""`` if none exist on disk (e.g. ``run_test_suite.py`` was run
    without ``--per-cell-reports``), so the block only appears when it resolves.
    """
    items = []
    for m in _ok_metrics(suite):
        rel = Path(m["station"]) / m["cell"] / "logs" / "gnss_report.html"
        if (suite_dir / rel).is_file():
            items.append(f"<a href='{html.escape(rel.as_posix())}'>"
                         f"{html.escape(m['station'])}/{html.escape(m['cell'])}</a>")
    if not items:
        return ""
    return ("<div class='links'><b>Per-cell reports:</b> "
            + " &nbsp;·&nbsp; ".join(items) + "</div>")


def render_suite_report(suite_metrics_path, out_html, *,
                        golden_path=None,
                        bias_tol_m: float = 0.02, rms_tol_m: float = 0.02,
                        with_convergence: bool = True,
                        backend_name: str = "matplotlib") -> str:
    """Render ``suite_metrics.json`` to a standalone HTML suite report.

    Sections: **Scorecard** (always), **Regression vs golden** (when
    ``golden_path`` is given and readable), and **Convergence** (when
    ``with_convergence`` and at least one cell's ``state_trace.csv`` is found).
    A coloured pass/fail gate banner and any per-cell report links are injected
    above the table of contents.
    """
    from ..backends import get_backend

    suite_metrics_path = Path(suite_metrics_path)
    suite = json.loads(suite_metrics_path.read_text(encoding="utf-8"))
    suite_dir = suite_metrics_path.parent

    sections: List[Tuple[str, ViewerResult]] = [("Scorecard", _scorecard_result(suite))]
    banner = ""

    if golden_path is not None and Path(golden_path).is_file():
        golden = json.loads(Path(golden_path).read_text(encoding="utf-8"))
        text, n_total, _ = regression_summary(
            suite, golden, bias_tol_m=bias_tol_m, rms_tol_m=rms_tol_m)
        figs = regression_figures(
            suite, golden, bias_tol_m=bias_tol_m, rms_tol_m=rms_tol_m)
        sections.append(("Regression vs golden", ViewerResult(figures=figs, summary=text)))
        cls = "fail" if n_total else "pass"
        verdict = (f"REGRESSION GATE: FAIL — {n_total} regression(s)"
                   if n_total else "REGRESSION GATE: PASS")
        banner = (f"<div class='banner {cls}'>{html.escape(verdict)} "
                  f"<span style='font-weight:400'>vs {html.escape(Path(golden_path).name)}"
                  f"</span></div>")
    elif golden_path is not None:
        # --golden was requested but the file is absent: the suite's gate hard-
        # fails (rc=2, "gate cannot run").  The report must mirror that -- never
        # render clean/green -- so surface an explicit error banner.
        banner = (f"<div class='banner fail'>"
                  f"{html.escape('REGRESSION GATE: ERROR — golden not found: ' + Path(golden_path).name)}"
                  f"</div>")

    if with_convergence:
        conv = convergence_figures(suite, suite_dir)
        if conv:
            sections.append(("Convergence", ViewerResult(figures=conv)))

    extra_html = banner + _per_cell_links_html(suite, suite_dir)
    return write_html_report(
        str(out_html), sections, get_backend(backend_name),
        log_dir=str(suite_dir), extra_html=extra_html,
        title=f"Test-suite report {suite.get('date','')}")


def render_scorecard_html(suite_metrics_path, out_html, *,
                          backend_name: str = "matplotlib") -> str:
    """Render just the text scorecard + bar figures (no golden / convergence).

    Kept for the standalone ``scorecard.html`` use; the full suite report (with
    the regression and convergence sections) is :func:`render_suite_report`.
    """
    from ..backends import get_backend

    suite = json.loads(Path(suite_metrics_path).read_text(encoding="utf-8"))
    return write_html_report(
        str(out_html),
        [("Scorecard", _scorecard_result(suite))],
        get_backend(backend_name),
        log_dir=str(Path(suite_metrics_path).parent),
        title=f"Test-suite scorecard {suite.get('date','')}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("suite_metrics", type=Path,
                    help="Path to suite_metrics.json (from run_test_suite.py).")
    ap.add_argument("-o", "--out", type=Path, default=None,
                    help="Output HTML (default: <suite dir>/report.html).")
    ap.add_argument("--golden", type=Path, default=None,
                    help="Golden suite_metrics.json to add a regression section.")
    ap.add_argument("--no-convergence", action="store_true",
                    help="Skip rebuilding per-cell convergence curves from traces.")
    ap.add_argument("--backend", default="matplotlib", choices=("matplotlib", "plotly"))
    args = ap.parse_args(argv)
    out = args.out or args.suite_metrics.parent / "report.html"
    path = render_suite_report(
        args.suite_metrics, out, golden_path=args.golden,
        with_convergence=not args.no_convergence, backend_name=args.backend)
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
