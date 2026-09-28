"""Simulated-network truth scorecard: the grader's verdict, displayed, not re-graded.

The synthetic minimum-network fixture (docs/SPEC_synthetic_minimum_network.md) is
the one absolute grade the network producer has: ``scripts/simnet_grade.py``
transforms injected truth into the producer's realized S-basis and compares the
emitted wire against it. This viewer displays that verdict; it never re-derives
a number, exactly as ``exam_scorecard`` displays the Verifier's.

Inputs (both written by ``scripts/simnet_report.py`` into the run's log dir):

  ``simnet_grade.json``   machine-readable channel grades (required)
  ``simnet_grade.txt``    the grader's full console readout (optional; appended
                          verbatim because its caveat text (census-first,
                          void-consumer, inert-tier warnings) is the scorecard's
                          fine print and must not drift from the grader)
"""

from __future__ import annotations

import json
import os

from .base import Viewer, ViewerResult


def _stats_line(s, unit=""):
    """Mirror the grader's ``fmt`` for a stats dict (or None)."""
    if not s:
        return "(no population)"
    return (f"n={s['n']:<6d} mean={s['mean']:+.4f}{unit}  rms={s['rms']:.4f}{unit}  "
            f"p50|.|={s['p50_abs']:.4f}{unit}  max|.|={s['max_abs']:.4f}{unit}")


def _vf_line(vf, n):
    """The grader's own calibration verdict thresholds, quoted, not invented."""
    if vf is None:
        return "vf n/a"
    verdict = ("CALIBRATED" if 0.5 <= vf <= 2.0 else
               "OPTIMISTIC (sigma too small)" if vf > 2.0 else
               "CONSERVATIVE (sigma too large)")
    return f"vf {vf:.3f} over {n} rows => {verdict}"


def build_simnet_scorecard(ctx) -> ViewerResult:
    with open(ctx.path("simnet_grade.json"), encoding="utf-8") as fh:
        g = json.load(fh)

    lines = ["=== Simulated network — truth scorecard (scripts/simnet_grade.py) ==="]
    s_line = g.get("s_line") or {}
    if s_line:
        lines.append(f"Realized datum : pivot {s_line.get('pivots')}   "
                     f"ref_rx {s_line.get('ref_rx')}")
        lines.append(f"                 cbpin {s_line.get('cbpin') or '(none)'}   "
                     f"gfpin {s_line.get('gfpin') or '(none — single-layer)'}")
    else:
        lines.append("Realized datum : *** NO S LINE — datum was assumed, distrust below ***")
    lines.append(f"Epochs emitted : {g.get('n_epochs')}"
                 f"   product census {g.get('product_census')}")

    lines.append("")
    lines.append("--- Channel grades (truth transformed INTO the realized basis) ---")
    clk = g.get("clock") or {}
    if clk:
        lines.append(f"clock residual : {_stats_line(clk.get('residual'), ' m')}")
        lines.append(f"      contrast : {_stats_line(clk.get('contrast'), ' m')}"
                     "   <- datum-free cross-check")
        lines.append(f"      {_vf_line(clk.get('vf'), clk.get('n_vf'))}")
    cb = g.get("code_bias") or {}
    if cb.get("armed") and not cb.get("no_pin"):
        lines.append(f"code bias      : {_stats_line(cb.get('residual'), ' m')}")
        lines.append(f"      {_vf_line(cb.get('vf'), cb.get('n_vf'))}")
    elif cb.get("no_pin"):
        lines.append("code bias      : REFUSED — no usable cbpin= on the S line")
    else:
        lines.append("code bias      : not armed (INT posture, no injected OSB)")
    ph = g.get("phase") or {}
    for sig, s in sorted(ph.items()):
        lines.append(f"phase SD {sig:<6}: {_stats_line(s, ' cyc')}")
    rx = g.get("rx_states") or {}
    if rx.get("n_cb"):
        lines.append(f"rx DCB contrast: {_stats_line(rx.get('contrast'), ' m')}")
        lines.append(f"        level  : {_stats_line(rx.get('level'), ' m')}"
                     "   <- absorbed gauge (Table-8), NOT an error")
    zwd = g.get("zwd") or {}
    if zwd:
        lines.append(f"ZWD (absolute) : {_stats_line(zwd.get('residual'), ' m')}")
        vf = zwd.get("vf")
        if vf is not None:
            lines.append(f"      vf {vf:.3f} over {zwd.get('n_vf')} rows"
                         "   <- the cleanest available vf")
    con = g.get("consumer")
    if con and not con.get("refused"):
        lines.append(f"consumer 3D    : {_stats_line(con.get('err3d'), ' m')}")
        lines.append(f"consumer up    : {_stats_line(con.get('up'), ' m')}"
                     f"   AR accepted {con.get('ar_success')}/{con.get('n_epochs')}")
    elif con:
        lines.append(f"consumer       : VOID ({con.get('refused')}) — correction set "
                     "empty, the number would grade the consumer alone")

    # The grader's full console readout, verbatim: the caveats live there.
    txt = ctx.path("simnet_grade.txt")
    if os.path.exists(txt) and os.path.getsize(txt) > 0:
        lines.append("")
        lines.append("--- Full grader readout (verbatim; read the census FIRST) ---")
        with open(txt, encoding="utf-8", errors="replace") as fh:
            lines.append(fh.read().rstrip())

    return ViewerResult(summary="\n".join(lines))


viewer = Viewer(name="simnet_scorecard",
                title="Simulated network — truth scorecard",
                required_csvs=("simnet_grade.json",),
                build=build_simnet_scorecard)
