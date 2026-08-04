#!/usr/bin/env python3
"""Grade the docs/SPEC_measurement_admission.md §7 gates from the arm run dirs.

Companion to ``adm_make_arms.py``.  Reads only committed artifacts
(``network_events.csv``, ``network_epoch_status.csv``, ``main_log.log``) so a
verdict can be re-derived later without the run still being warm.

Like ``pc_gate_verdict.py`` this REFUSES to grade a run that is still writing:
the producer's run-end telemetry block is the exit marker, and grading a
mid-flight run produced false verdicts on the prefit card before that guard
existed.

Usage::

    python scripts/adm_gate_verdict.py                  # every arm present
    python scripts/adm_gate_verdict.py --gate birth
"""
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNS = ROOT / "runs"

# The §7 gate-2 known answer, from docs/SPEC_prefit_census_observability.md §17.
KNOWN = [
    ("BSHM00ISR", "G07", 327030.0, 1.98e5),
    ("BSHM00ISR", "G16", 327210.0, 9.75e4),
]
# The single-band INNOCENCE control: also C1C-only at BSHM, and healthy.
CONTROL = ("BSHM00ISR", "G21")


def finished(run: Path) -> bool:
    """Did the producer reach its run-end telemetry?  (the exit marker)"""
    log = run / "logs" / "main_log.log"
    if not log.is_file():
        return False
    txt = log.read_text(encoding="utf-8", errors="replace")
    return "measurement admission): " in txt or "Network: run complete" in txt


def events(run: Path) -> list[dict]:
    p = run / "logs" / "network_events.csv"
    if not p.is_file():
        return []
    with p.open(encoding="utf-8", errors="replace", newline="") as f:
        return [r for r in csv.DictReader(f) if r.get("tier") == "ADMISSION"]


def status_rows(run: Path) -> list[dict]:
    p = run / "logs" / "network_epoch_status.csv"
    if not p.is_file():
        return []
    with p.open(encoding="utf-8", errors="replace", newline="") as f:
        return list(csv.DictReader(f))


def runend(run: Path) -> list[str]:
    log = run / "logs" / "main_log.log"
    if not log.is_file():
        return []
    return [ln.split("] ", 3)[-1].strip()
            for ln in log.read_text(encoding="utf-8", errors="replace").splitlines()
            if "measurement admission" in ln]


def a00_at(run: Path, tow: float) -> float | None:
    """SH a00 SUM at one TOW, from the run log's per-epoch SH line."""
    log = run / "logs" / "main_log.log"
    if not log.is_file():
        return None
    pat = re.compile(r"TOW " + f"{tow:.1f}".replace(".", r"\.") + r"\b")
    for ln in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "a00" in ln and pat.search(ln):
            m = re.search(r"a00[^=]*=\s*(-?[\d.]+)", ln)
            if m:
                return float(m.group(1))
    return None


def grade_birth(tag: str = "birth") -> int:
    """§7 gate 2 — the KNOWN-ANSWER birth gate."""
    run = RUNS / f"ft63_adm_{tag}"
    if not run.is_dir():
        print(f"gate 2 birth : SKIP (no {run})")
        return 0
    if not finished(run):
        print(f"gate 2 birth : REFUSED — {run.name} is still writing "
              f"(no run-end admission block).  Grading a mid-flight run is how the "
              f"prefit card got four false verdicts.")
        return 1
    ev = events(run)
    ok = True
    for sta, sat, tow, z_exp in KNOWN:
        hit = [e for e in ev
               if e["station"] == sta and e["sat"] == sat
               and abs(float(e["tow"]) - tow) < 1e-3
               and e["event"] in ("adm_reject_birth", "adm_reject_resume")]
        if not hit:
            print(f"  MISS  {sta}/{sat} @ {tow:.0f}: no birth/resume rejection")
            ok = False
            continue
        z = abs(float(hit[0]["statistic"]))
        rel = abs(z - z_exp) / z_exp
        verdict = "OK " if rel < 0.10 else "OFF"
        if rel >= 0.10:
            ok = False
        print(f"  {verdict}   {sta}/{sat} @ {tow:.0f}: {hit[0]['event']} "
              f"z={z:.4g} (spec {z_exp:.4g}, {rel*100:.1f}% off)")
    # The innocence control: G21@BSHM is ALSO C1C-only and must NEVER be rejected.
    bad = [e for e in ev if e["station"] == CONTROL[0] and e["sat"] == CONTROL[1]
           and e["event"].startswith("adm_reject")]
    print(f"  {'OK ' if not bad else 'FAIL'}   control {CONTROL[0]}/{CONTROL[1]} "
          f"(single-band, healthy): {len(bad)} rejection(s) — must be 0")
    ok = ok and not bad
    sta_ev = [e for e in ev if e["event"] == "adm_station_event"]
    print(f"  {'OK ' if not sta_ev else 'NOTE'}   station events: {len(sta_ev)}")
    print(f"gate 2 birth : {'PASS' if ok else 'FAIL'}")
    for ln in runend(run):
        print("    " + ln)
    return 0 if ok else 1


def grade_sweep() -> int:
    """§7 gate 4 — the bound sweep.  The reject SET must be identical."""
    sets = {}
    for tag, bound in (("b300", 300), ("birth", 1000), ("b3k", 3000)):
        run = RUNS / f"ft63_adm_{tag}"
        if not run.is_dir() or not finished(run):
            continue
        sets[bound] = {(e["station"], e["sat"], e["detail"].split(";")[0])
                       for e in events(run) if e["event"].startswith("adm_reject")}
    if len(sets) < 2:
        print("gate 4 sweep : SKIP (need >= 2 of b300 / birth / b3k)")
        return 0
    ref_b = sorted(sets)[0]
    same = all(v == sets[ref_b] for v in sets.values())
    for b in sorted(sets):
        print(f"  bound {b:>5}: {len(sets[b])} rejected channel(s) {sorted(sets[b])}")
    print(f"gate 4 sweep : {'PASS (set identical across the sweep)' if same else 'FAIL (set moved)'}")
    return 0 if same else 1


def grade_break() -> int:
    """§7 gate 3 — the era-2 break gate + the 329190 free experiment."""
    on, off = RUNS / "ft63_adm_break", RUNS / "ft63_adm_breakoff"
    if not (on.is_dir() and finished(on)):
        print("gate 3 break : SKIP (arm absent or still writing)")
        return 0
    print("  TOW      OFF a00      ON a00      ON disposition")
    for tow in (329130.0, 329160.0, 329190.0, 330000.0):
        rows = {r["tow"]: r for r in status_rows(on)}
        disp = rows.get(f"{tow:.3f}", {}).get("disposition", "-")
        print(f"  {tow:.0f}  {str(a00_at(off, tow)):>10}  {str(a00_at(on, tow)):>10}  {disp}")
    print("  §7 gate 3 verdict is a JUDGEMENT on those four rows — record which of the "
          "two outcomes for 329190 (vanishes / persists) actually happened.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", choices=("birth", "sweep", "break", "all"), default="all")
    args = ap.parse_args()
    rc = 0
    if args.gate in ("birth", "all"):
        rc |= grade_birth()
    if args.gate in ("sweep", "all"):
        rc |= grade_sweep()
    if args.gate in ("break", "all"):
        rc |= grade_break()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
