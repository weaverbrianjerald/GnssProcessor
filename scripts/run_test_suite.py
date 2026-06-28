#!/usr/bin/env python3
"""run_test_suite.py -- manifest-driven "standard test run" harness.

Runs ``gnss_processor.exe`` over a matrix of (station x config-cell) defined in a
JSON manifest, grading each run's position against an **independent** truth
coordinate (IGS SINEX/SSC primary -- auto-downloaded -- with NGS SIF fallback).
Produces:

  * a SINEX-vs-SIF truth cross-check (flags stations whose truth sources disagree),
  * an ENU bias / RMS / convergence scorecard per cell,
  * A/B deltas vs each base's reference (empty-overrides) cell,
  * a regression gate vs a stored golden ``suite_metrics.json``,
  * a standalone HTML report (``<output_root>/report.html``): the scorecard, the
    regression-vs-golden diff, and auto-generated per-cell convergence curves
    (disable with ``--no-report``).

This supersedes the inline tables in run_test_slice.py -- the matrix now lives in
``config/test_suite/manifest.json``.

Usage
-----
::

    C:\\GnssProcessor\\venv\\Scripts\\python.exe scripts\\run_test_suite.py
    ...\\python.exe scripts\\run_test_suite.py --workers 3
    ...\\python.exe scripts\\run_test_suite.py --golden test_suite_results/golden.json
    ...\\python.exe scripts\\run_test_suite.py --update-golden test_suite_results/golden.json
    ...\\python.exe scripts\\run_test_suite.py --manifest path/to/manifest.json --force
    ...\\python.exe scripts\\run_test_suite.py --no-run --golden ...\\golden.json --per-cell-reports
"""

from __future__ import annotations

import argparse
import concurrent.futures
import copy
import json
import math
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "pyviz"))

# NOTE: gnssviz.eval (numpy/pandas) is imported lazily inside main() so the
# ProcessPoolExecutor workers -- which only need stdlib for _run_exe -- don't
# pay the import cost on every spawn.

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

_JSONC_RE = re.compile(
    r'(?P<s>"(?:[^"\\]|\\.)*")|//[^\n]*|/\*.*?\*/', re.DOTALL)


def load_jsonc(path: Path) -> dict:
    return json.loads(_JSONC_RE.sub(lambda m: m.group("s") or "",
                                    Path(path).read_text(encoding="utf-8-sig")))


def set_dotpath(cfg: dict, dotted: str, value) -> None:
    keys = dotted.split(".")
    node = cfg
    for k in keys[:-1]:
        node = node.setdefault(k, {})
    node[keys[-1]] = value


def rp(rel: str) -> Path:
    """Resolve a manifest-relative path against the repo root."""
    p = Path(rel)
    return p if p.is_absolute() else (REPO / p)


# ---------------------------------------------------------------------------
# build + run one cell
# ---------------------------------------------------------------------------

def build_cell_config(base_cfg: dict, station: dict, overrides: dict,
                      year: int, doy: int) -> dict:
    cfg = copy.deepcopy(base_cfg)
    cfg["data_source"] = "internal"
    files = cfg.setdefault("files", {})
    files["observation_path"] = str(rp(station["obs"]))
    files["internal_data_root"] = str(REPO / "data")
    dl = cfg.setdefault("data_download", {})
    dl.update({"enabled": False, "station": station["name"], "year": year, "doy": doy})
    for dotted, value in overrides.items():
        set_dotpath(cfg, dotted, value)
    return cfg


def _run_exe(payload: dict) -> dict:
    """Pool-friendly: run the exe for one already-written config."""
    run_dir = Path(payload["run_dir"])
    marker = run_dir / ".success"
    if marker.exists() and not payload["force"]:
        return {"id": payload["id"], "skipped": True, "returncode": 0, "elapsed_s": 0.0}
    t0 = time.time()
    try:
        proc = subprocess.run(
            [payload["exe"], payload["config_path"]], cwd=str(run_dir),
            capture_output=True, text=True, timeout=payload["timeout"])
        (run_dir / "stdout.log").write_text(proc.stdout or "", encoding="utf-8")
        (run_dir / "stderr.log").write_text(proc.stderr or "", encoding="utf-8")
        if proc.returncode == 0:
            marker.touch()
        return {"id": payload["id"], "skipped": False,
                "returncode": proc.returncode, "elapsed_s": time.time() - t0}
    except subprocess.TimeoutExpired:
        return {"id": payload["id"], "skipped": False, "returncode": -1,
                "elapsed_s": time.time() - t0}


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------

def _write_per_cell_reports(jobs, *, backend_name: str = "matplotlib") -> None:
    """Generate each cell's full per-run ``gnss_report.html`` (best-effort).

    These are the standard per-run viewer reports (``run_report.py``); the suite
    report links to them when present.  A failure on one cell is reported and
    skipped -- it must not abort the suite.
    """
    from gnssviz import TraceContext, run_viewers
    from gnssviz.backends import get_backend
    from gnssviz.report import write_html_report

    backend = get_backend(backend_name)
    n = 0
    for j in jobs:
        log_dir = Path(j["run_dir"]) / "logs"
        if not (log_dir / "state_trace.csv").is_file():
            continue
        try:
            ctx = TraceContext(str(log_dir))
            out = run_viewers(ctx)
            write_html_report(str(log_dir / "gnss_report.html"),
                              [(v.title, r) for (v, r) in out.ran],
                              backend, log_dir=str(log_dir), skipped=out.skipped)
            n += 1
        except Exception as exc:                              # noqa: BLE001
            print(f"   ! per-cell report failed for {j['id']}: {exc}", file=sys.stderr)
    if n:
        print(f"-- Wrote {n} per-cell gnss_report.html --")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--manifest", type=Path,
                    default=REPO / "config" / "test_suite" / "manifest.json")
    ap.add_argument("--exe", type=Path, default=None)
    ap.add_argument("--output-root", type=Path, default=None)
    ap.add_argument("--workers", type=int, default=3,
                    help="Parallel exe runs (data/ is read-only, logs are isolated).")
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--warmup-s", type=float, default=None)
    ap.add_argument("--threshold-m", type=float, default=None)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--golden", type=Path, default=None,
                    help="Compare the scorecard against this golden suite_metrics.json.")
    ap.add_argument("--update-golden", type=Path, default=None,
                    help="Write the current scorecard to this path as the new golden.")
    ap.add_argument("--no-run", action="store_true",
                    help="Skip running the exe; just (re)grade existing logs.")
    ap.add_argument("--no-report", action="store_true",
                    help="Skip the HTML suite report (scorecard + regression + convergence).")
    ap.add_argument("--report-backend", default="matplotlib",
                    choices=("matplotlib", "plotly"),
                    help="Rendering backend for the suite report figures.")
    ap.add_argument("--per-cell-reports", action="store_true",
                    help="Also generate each cell's full per-run gnss_report.html "
                         "(slower) and link them from the suite report.")
    args = ap.parse_args(argv)

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    from gnssviz.eval import (
        SifTruth, SinexTruth, CompositeTruth,
        compare_metrics, cross_check_truth,
        doy_to_decimal_year, ensure_itrf_ssc, evaluate_state_trace, find_sinex_files,
    )

    man = load_jsonc(args.manifest)
    year, doy = man["date"]["year"], man["date"]["doy"]
    epoch = doy_to_decimal_year(year, doy)
    exe = args.exe or rp(man["exe"])
    out_root = args.output_root or rp(man["output_root"])
    ev = man.get("eval", {})
    warmup_s = args.warmup_s if args.warmup_s is not None else ev.get("warmup_s", 1800.0)
    threshold_m = args.threshold_m if args.threshold_m is not None else ev.get("threshold_m", 0.10)

    if not exe.is_file():
        print(f"error: exe not found: {exe}", file=sys.stderr)
        return 2

    # ---- truth ----
    tr = man["truth"]
    sinex_dir = rp(tr["sinex_dir"])
    if tr.get("auto_fetch_sinex", True) and not args.no_run:
        ensure_itrf_ssc(sinex_dir)
    sinex_files = find_sinex_files(sinex_dir)
    sif_paths = [rp(p) for p in tr.get("sif_paths", []) if rp(p).is_file()]
    sif_kwargs = {"with_periodic": tr.get("sif_with_periodic", True),
                  "with_psd": tr.get("sif_with_psd", True)}

    snx_sources = [s for s in (SinexTruth(p) for p in sinex_files) if s.stations()]
    sif_source = SifTruth(sif_paths, **sif_kwargs) if sif_paths else None
    # Cross-check against the *secular* (periodic-free) SIF so a seasonal term
    # can't masquerade as a bad coordinate; grading still uses sif_source.
    sif_linear = (SifTruth(sif_paths, with_periodic=False, with_psd=False)
                  if sif_paths else None)
    truth_sources = list(snx_sources) + ([sif_source] if sif_source else [])
    truth = CompositeTruth(truth_sources)

    print(f"== Standard test run ==  {year}-{doy:03d}  epoch={epoch:.4f}")
    print(f"   exe:    {exe}")
    print(f"   truth:  SINEX({len(sinex_files)} file(s): "
          f"{', '.join(p.name for p in sinex_files) or 'none'}) -> "
          f"SIF({len(sif_paths)} file(s))")
    print(f"   out:    {out_root}\n")

    # ---- truth cross-check (SINEX vs SIF) ----
    truth_keys = sorted({s["truth_key"] for s in man["stations"]})
    cross_check = []
    if snx_sources and sif_linear:
        rows, n_flagged = cross_check_truth(
            snx_sources[0], sif_linear, truth_keys, epoch,
            tol_m=tr.get("cross_check_tol_m", 0.05))
        print("-- Truth cross-check (SINEX vs secular SIF) --")
        for r in rows:
            print(r.one_line())
            cross_check.append({"station": r.station, "primary": r.primary_source,
                                "secondary": r.secondary_source, "dist_m": r.dist_m,
                                "enu_m": list(r.enu_m), "ok": r.ok, "note": r.note})
        if n_flagged:
            print(f"   ! {n_flagged} station(s) disagree > "
                  f"{tr.get('cross_check_tol_m', 0.05)*100:.0f} cm "
                  f"(newer stations extrapolated past the SSC span often do)")
        print()

    # ---- build the job list ----
    stations = {s["name"]: s for s in man["stations"]}
    cells = {c["label"]: c for c in man["cells"]}
    bases_cfg = {name: load_jsonc(rp(path)) for name, path in man["bases"].items()}

    jobs = []
    for entry in man["matrix"]:
        sta = stations[entry["station"]]
        if not rp(sta["obs"]).is_file():
            print(f"[data] {sta['name']}: obs missing {sta['obs']} -- skipped")
            continue
        for label in entry["cells"]:
            cell = cells[label]
            run_dir = out_root / sta["name"] / label
            run_dir.mkdir(parents=True, exist_ok=True)
            cfg = build_cell_config(bases_cfg[cell["base"]], sta,
                                    cell.get("overrides", {}), year, doy)
            cfg_path = run_dir / "run_config.json"
            cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
            jobs.append({"id": f"{sta['name']}/{label}", "station": sta["name"],
                         "label": label, "base": cell["base"],
                         "truth_key": sta["truth_key"], "run_dir": run_dir,
                         "config_path": str(cfg_path)})

    # ---- run the exes (parallel pool) ----
    run_info = {}
    if not args.no_run:
        payloads = [{"id": j["id"], "exe": str(exe), "config_path": j["config_path"],
                     "run_dir": str(j["run_dir"]), "timeout": args.timeout,
                     "force": args.force} for j in jobs]
        print(f"-- Running {len(payloads)} cell(s) on {args.workers} worker(s) --")
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as ex:
            for r in ex.map(_run_exe, payloads):
                run_info[r["id"]] = r
                tag = "skip" if r["skipped"] else ("ok" if r["returncode"] == 0 else "FAIL")
                print(f"   {tag:>4}  {r['id']:<26} {r['elapsed_s']:6.1f}s")
        print()

    # ---- grade ----
    metrics = []
    for j in jobs:
        tc = truth.resolve(j["truth_key"], epoch)
        info = run_info.get(j["id"], {"returncode": 0, "elapsed_s": 0.0, "skipped": True})
        trace = j["run_dir"] / "logs" / "state_trace.csv"
        if tc is None or info["returncode"] != 0 or not trace.is_file():
            metrics.append({"station": j["station"], "cell": j["label"], "base": j["base"],
                            "ok": False, "returncode": info["returncode"],
                            "reason": ("no truth" if tc is None else
                                       "run failed" if info["returncode"] != 0 else
                                       "no state_trace")})
            continue
        try:
            m = evaluate_state_trace(str(trace), tc.ecef, station=j["station"],
                                     source=tc.source, frame=tc.frame,
                                     warmup_s=warmup_s, threshold_m=threshold_m)
        except Exception as exc:                              # noqa: BLE001
            metrics.append({"station": j["station"], "cell": j["label"], "base": j["base"],
                            "ok": False, "returncode": info["returncode"],
                            "reason": f"grade error: {exc}"})
            continue
        # A converged, finite solution is the only thing graded "ok"; a diverged
        # run (no finite samples / NaN RMS) is a failure so it can't sneak past
        # the regression gate as a benign all-NaN metric.
        diverged = (m.n_converged == 0) or not math.isfinite(m.rms_3d)
        d = m.to_dict()
        d.update({"cell": j["label"], "base": j["base"], "ok": not diverged,
                  "elapsed_s": round(info["elapsed_s"], 1)})
        if diverged:
            d["reason"] = m.notes or "diverged (no finite samples)"
        metrics.append(d)

    # ---- scorecard ----
    print("=" * 104)
    print("SCORECARD  (ENU error vs independent truth; bias/RMS in cm)")
    print("=" * 104)
    ok_metrics = [m for m in metrics if m.get("ok")]
    for m in metrics:
        if not m.get("ok"):
            print(f"  {m['station']}/{m['cell']:<18} -- FAILED: {m.get('reason','?')}")
            continue
        be, bn, bu = (v * 100 for v in m["bias_enu_m"])
        ttc = m["time_to_threshold_s"]
        ttc_s = "n/a" if ttc is None else f"{ttc:5.0f}s"
        print(f"  {m['station']}/{m['cell']:<18} bias[E{be:+6.1f} N{bn:+6.1f} U{bu:+6.1f}]cm "
              f" 3D-RMS {m['rms_3d_m']*100:6.1f}  H-RMS {m['rms_horizontal_m']*100:6.1f}  "
              f"conv {ttc_s}  (n={m['n_converged']})")

    # ---- A/B deltas vs each (station, base) reference cell ----
    print("\n" + "-" * 104)
    print("A/B DELTAS vs base reference cell  (delta in cm)")
    print("-" * 104)
    ref_label = {}   # base -> empty-overrides cell label
    for c in man["cells"]:
        if not c.get("overrides"):
            ref_label.setdefault(c["base"], c["label"])
    groups = {}
    for m in ok_metrics:
        groups.setdefault((m["station"], m["base"]), {})[m["cell"]] = m
    for (sta, base), cellmap in sorted(groups.items()):
        ref = ref_label.get(base)
        ref_m = cellmap.get(ref) or cellmap[sorted(cellmap)[0]]
        ref_name = next(k for k, v in cellmap.items() if v is ref_m)
        for label, m in sorted(cellmap.items()):
            if m is ref_m:
                continue
            dE = (m["bias_enu_m"][0] - ref_m["bias_enu_m"][0]) * 100
            dN = (m["bias_enu_m"][1] - ref_m["bias_enu_m"][1]) * 100
            dU = (m["bias_enu_m"][2] - ref_m["bias_enu_m"][2]) * 100
            d3 = (m["rms_3d_m"] - ref_m["rms_3d_m"]) * 100
            print(f"  {sta:<6} {base:<7} {label:<18} vs {ref_name:<14} "
                  f"dE{dE:+7.2f} dN{dN:+7.2f} dU{dU:+7.2f}  d3D-RMS{d3:+7.2f}")

    # ---- write scorecard ----
    out_root.mkdir(parents=True, exist_ok=True)
    suite_out = {"date": f"{year}-{doy:03d}", "epoch_decimal_year": epoch,
                 "warmup_s": warmup_s, "threshold_m": threshold_m,
                 "ref_cells": ref_label, "cross_check": cross_check, "metrics": metrics}
    suite_path = out_root / "suite_metrics.json"
    suite_path.write_text(json.dumps(suite_out, indent=2), encoding="utf-8")
    print(f"\nWrote {suite_path}")

    # ---- golden gate ----
    rc = 0
    if args.update_golden:
        args.update_golden.parent.mkdir(parents=True, exist_ok=True)
        args.update_golden.write_text(json.dumps(suite_out, indent=2), encoding="utf-8")
        print(f"Updated golden: {args.update_golden}")

    # ---- visual suite report (scorecard + regression + convergence) ----
    # Emitted here -- after any --update-golden write but before the gate's
    # compare/early-return -- so (a) it still lands when the gate exits early on a
    # missing golden, and (b) when --update-golden seeds the same file passed to
    # --golden it reads the just-written baseline, so the report's pass/fail
    # banner matches the gate's exit code instead of the pre-update golden.
    # render_suite_report reports a missing --golden as a red ERROR banner (it
    # cannot show clean/green while the gate is about to return rc=2).
    if not args.no_report:
        if args.per_cell_reports:
            _write_per_cell_reports(jobs, backend_name=args.report_backend)
        from gnssviz.eval.report import render_suite_report
        report_path = out_root / "report.html"
        try:
            render_suite_report(
                suite_path, report_path, golden_path=args.golden,
                bias_tol_m=ev.get("bias_tol_m", 0.02),
                rms_tol_m=ev.get("rms_tol_m", 0.02),
                backend_name=args.report_backend)
            print(f"Wrote {report_path}")
        except Exception as exc:                              # noqa: BLE001
            print(f"! suite report generation failed: {exc}", file=sys.stderr)

    if args.golden:
        if not args.golden.is_file():
            # A gate that can't find its baseline must NOT report success.
            print(f"! golden not found: {args.golden} (use --update-golden to seed it); "
                  f"gate cannot run", file=sys.stderr)
            return 2
        golden = json.loads(args.golden.read_text(encoding="utf-8"))
        golden_ok = [g for g in golden.get("metrics", []) if g.get("ok")]
        deltas, n_reg = compare_metrics(
            ok_metrics, golden_ok,
            bias_tol_m=ev.get("bias_tol_m", 0.02), rms_tol_m=ev.get("rms_tol_m", 0.02))
        print("\n" + "=" * 104)
        print(f"REGRESSION GATE vs {args.golden.name}  "
              f"(tol: bias {ev.get('bias_tol_m',0.02)*100:.0f} cm, "
              f"RMS {ev.get('rms_tol_m',0.02)*100:.0f} cm)")
        print("=" * 104)
        for d in deltas:
            print(d.one_line())
        # A cell that passed in golden but is now failed/missing is a hard
        # regression (a working config stopped working); compare_metrics reports
        # it only as 'missing', so count it explicitly toward the exit code.
        cur_ok = {(m["station"], m["cell"]) for m in ok_metrics}
        broken = [(g["station"], g["cell"]) for g in golden_ok
                  if (g["station"], g["cell"]) not in cur_ok]
        if broken:
            print(f"\n! {len(broken)} previously-passing cell(s) now failed/missing:")
            for sta, cell in broken:
                print(f"    BROKEN {sta}/{cell}")
        n_total = n_reg + len(broken)
        rc = 1 if n_total else 0
        print(f"\n{'REGRESSIONS: ' + str(n_total) if rc else 'No regressions.'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
