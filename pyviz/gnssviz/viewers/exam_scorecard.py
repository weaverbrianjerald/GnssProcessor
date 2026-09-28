"""Exam scorecard: the run's answer key against the Verifier verdict (no plots).

A text-only viewer (SPEC §4.2): a ``summary`` block stating the scenario, mode,
seed, truth ISB/ZWD, storm window/peak, slip counts, and the truth↔trace epoch
match count (SPEC R5), followed by the per-check gate table read straight from
``verify_report.json`` (SPEC R6). The dashboard displays the Verifier's
verdict; it does not re-grade. The coloured PASS/FAIL banner is injected at the
top of the report by ``run_report.py`` via the ``extra_html`` hook (SPEC §4.4);
this viewer carries the detail table in a plain ``<pre>`` (no added CSS).

Renders whenever a truth dir is bound (``truth_scalars.csv`` is always emitted);
degrades gracefully when ``verify_report.json`` / ``scenario_summary.json`` are
absent.  A dashboard number shown next to a Verifier gate is labelled
``unverified`` when no verify report is present.
"""

from __future__ import annotations

from ..io import first_col, load_trace
from .base import Viewer, ViewerResult
from . import exam_common as ec


def _fmt(v, nd=3):
    try:
        return f"{float(v):.{nd}f}"
    except (TypeError, ValueError):
        return str(v)


def _epoch_match_count(ctx) -> str:
    """truth_position ↔ state_trace overlap on the (week, tow-ms) grid (R5)."""
    truth = ec.truth_frame(ctx, "truth_position.csv")
    if truth is None or not ctx.has_data("state_trace.csv"):
        return "n/a (truth_position or state_trace absent)"
    try:
        state = load_trace(ctx.path("state_trace.csv"))
        m = ec.merge_truth_trace(truth, state)
        return f"{len(m)} epochs joined (truth {len(truth)}, state {len(state)})"
    except Exception as exc:  # pragma: no cover - defensive
        return f"n/a ({type(exc).__name__})"


def _slip_line(ctx) -> str:
    """Slip counts by class from truth_slips.csv (falls back to scalar counts)."""
    df = ec.truth_frame(ctx, "truth_slips.csv")
    if df is None or "delta_cycles" not in df.columns:
        sc = ec.scalars(ctx)
        if "n_cycle_slips" in sc:
            return (f"{int(sc['n_cycle_slips'])} total "
                    f"({int(sc.get('n_cycle_slips_flagged', 0))} flagged)")
        return "none recorded"
    cls = ec.classify_slips(df)
    counts = cls.value_counts()
    parts = [f"{k}={int(counts.get(k, 0))}" for k in ec.SLIP_CLASS_ORDER if counts.get(k, 0)]
    biggest = df["delta_cycles"].abs().max() if len(df) else 0
    return f"{len(df)} total — " + ", ".join(parts) + f"; largest |Δ|={int(biggest)} cyc"


def build_exam_scorecard(ctx) -> ViewerResult:
    sc = ec.scalars(ctx)
    ss = ec.scenario_summary(ctx) or {}
    rep = ec.verify_report(ctx)
    kine = ec.is_kinematic(ctx)

    truth = ss.get("truth", {}) if isinstance(ss.get("truth"), dict) else {}
    storm = ss.get("storm", {}) if isinstance(ss.get("storm"), dict) else {}
    noise = ss.get("noise", {}) if isinstance(ss.get("noise"), dict) else {}

    isb = truth.get("gps_galileo_isb_m", sc.get("isb_gps_gal_m"))
    zwd = truth.get("zwd_m", sc.get("zwd_m"))
    seed = noise.get("seed")

    lines = ["=== GnssSimulator exam scorecard ==="]
    lines.append(f"Scenario     : {ss.get('scenario', '(scenario_summary.json absent)')}")
    if ss.get("date"):
        lines.append(f"Date         : {ss['date']}    Station: {ss.get('station', '?')}")
    lines.append(f"Mode         : {'KINEMATIC' if kine else 'STATIC'}"
                 + (f"    Epochs: {int(sc['n_epochs'])}" if "n_epochs" in sc else "")
                 + (f"    Seed: {int(seed)}" if seed is not None else ""))
    lines.append("")
    lines.append("--- Answer key (truth by construction) ---")
    lines.append(f"GPS→Galileo ISB : {_fmt(isb)} m" if isb is not None else "GPS→Galileo ISB : (n/a)")
    lines.append(f"ZWD (wet tropo) : {_fmt(zwd)} m" if zwd is not None else "ZWD (wet tropo) : (n/a)")
    t0, t1 = ec.storm_window_utc(ctx)
    if t0 is not None:
        peak = storm.get("peak_slant_dI_L1_m", sc.get("peak_dI_L1_m"))
        peak_sat = storm.get("peak_sat", "?")
        lines.append(f"Storm window    : {t0:%H:%M:%S} → {t1:%H:%M:%S} GPS"
                     + (f"   peak dI_L1 {_fmt(peak, 2)} m @ {peak_sat}" if peak is not None else ""))
    else:
        lines.append("Storm window    : (none in this run)")
    lines.append(f"Cycle slips     : {_slip_line(ctx)}")
    lines.append(f"Epoch match     : {_epoch_match_count(ctx)}")

    lines.append("")
    if rep is None:
        lines.append("--- Verifier gates: verify_report.json ABSENT (verdict unverified) ---")
        lines.append("  Run `gnss_simulator verify <truth> <logs>` (or the `all`/harness")
        lines.append("  loop) to emit the machine-readable gate report.")
    else:
        passed = bool(rep.get("pass"))
        lines.append(f"--- Verifier gates: {'PASS' if passed else 'FAIL'} "
                     f"({int(rep.get('failures', 0))} failure(s), k={_fmt(rep.get('k_sigma', 3), 0)}, "
                     f"converge_skip={int(rep.get('converge_skip', 0))}) ---")
        lines.append(f"  {'check':<18}{'value':>12}{'gate':>12}  verdict")
        lines.append("  " + "-" * 52)
        for c in rep.get("checks", []):
            cid = str(c.get("id", "?"))
            units = str(c.get("units", ""))
            val = c.get("value")
            gate = c.get("gate")
            vs = (f"{val:.4g} {units}".strip() if isinstance(val, (int, float)) else str(val))
            gs = (f"{gate:.4g} {units}".strip() if isinstance(gate, (int, float)) else str(gate))
            verdict = "PASS" if c.get("pass") else "FAIL"
            lines.append(f"  {cid:<18}{vs:>12}{gs:>12}  {verdict}")
            if c.get("detail"):
                lines.append(f"      {c['detail']}")

    return ViewerResult(summary="\n".join(lines))


viewer = Viewer(name="exam_scorecard", title="Exam scorecard & Verifier verdict",
                required_csvs=("truth_scalars.csv",), build=build_exam_scorecard)
