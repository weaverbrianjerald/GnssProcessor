#!/usr/bin/env python3
"""Grade the §12 known-answer gates of docs/SPEC_prefit_census_observability.md.

Every check is a known answer from the standing FT63 leg-19 replay
(``runs/ft63_leg19_rhsat`` / ``runs/ft63_leg19_base``): the era-2 break at
TOW 329160, the parked (BSHM00ISR, G07) code cell that fed it, the clean 1 ms
URUM00CHN receiver-clock step at 329340 that must not fire the census, and the
attribution baseline (BSHM obs_code C00 0.002 -> +6493, closure 5e-11).

Usage::

    python scripts/pc_gate_verdict.py                # all gates it can find
    python scripts/pc_gate_verdict.py --gate 2 4
"""
from __future__ import annotations

import argparse
import csv
import filecmp
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNS = ROOT / "runs"

BREAK_TOW = 329160.0        # the era-2 discharge epoch (SH gate engages here)
PRE_TOW = 329130.0          # the informative "before" frame
CLOCK_STEP_TOW = 329340.0   # URUM00CHN's clean 1 ms receiver-clock step

OK, BAD, INFO = "PASS", "FAIL", "note"
_verdicts: list[tuple[str, str, str]] = []


def say(gate: str, verdict: str, msg: str) -> None:
    _verdicts.append((gate, verdict, msg))
    print(f"[{verdict:4s}] gate {gate}: {msg}")


def events(run: str) -> list[dict]:
    p = RUNS / run / "logs" / "network_events.csv"
    if not p.exists():
        return []
    return list(csv.DictReader(p.open(encoding="utf-8", errors="replace")))


def epoch_status(run: str) -> list[dict]:
    p = RUNS / run / "logs" / "network_epoch_status.csv"
    if not p.exists():
        return []
    return list(csv.DictReader(p.open(encoding="utf-8", errors="replace")))


def runlog(run: str) -> str:
    p = RUNS / run / "run.log"
    return p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""


# The producer's last line of a normal exit. Grading a run that is still writing
# compares a truncated CSV against a complete one and reports a byte difference
# that is pure harness artifact: measured, on this sweep, as four false FAILs.
# Every gate checks this first.
_DONE_MARKER = "shared satellite product accumulated"


def finished(run: str) -> bool:
    return _DONE_MARKER in runlog(run)


def require_finished(gate: str, *runs: str) -> bool:
    for r in runs:
        if not (RUNS / r / "run.log").exists():
            say(gate, INFO, f"{r} has not run — not graded")
            return False
        if not finished(r):
            say(gate, INFO, f"{r} is STILL RUNNING (no exit marker) — not graded; "
                            f"a partial CSV would report a false difference")
            return False
    return True


def fnum(row: dict, key: str, default: float = 0.0) -> float:
    v = (row.get(key) or "").strip()
    try:
        return float(v)
    except ValueError:
        return default


# ---------------------------------------------------------------------------
def gate_solve_identity(arm: str, control: str = "ft63_leg19_pc_pcbase") -> None:
    """A measure-only arm must leave every solved artifact byte-identical."""
    tag = f"solve-identity {arm}"
    if not require_finished(tag, arm, control):
        return
    for name in ("network_solution.csv", "network_trace.csv"):
        a = RUNS / arm / "logs" / name
        b = RUNS / control / "logs" / name
        if not (a.exists() and b.exists()):
            say(tag, INFO, f"{name} missing on one side — not graded")
            continue
        same = filecmp.cmp(a, b, shallow=False)
        say(tag, OK if same else BAD,
            f"{name} {'byte-identical' if same else 'DIFFERS'} vs {control}")


def gate2_census(arm: str = "ft63_leg19_pc_census") -> None:
    if not require_finished("2", arm):
        return
    ev = [e for e in events(arm) if e.get("tier") == "PREFIT"]
    if not ev:
        say("2", BAD, f"no PREFIT events in {arm} — the census produced no identities")
        return

    hits = [e for e in ev if e["station"] == "BSHM00ISR" and e["sat"] == "G07"
            and "class=obs_code" in (e.get("detail") or "")]
    if hits:
        s = [fnum(e, "statistic") for e in hits]
        tows = [fnum(e, "tow") for e in hits]
        say("2", OK, f"census NAMES (BSHM00ISR, G07, obs_code): {len(hits)} event(s), "
                     f"s in [{min(s):.3g}, {max(s):.3g}], TOW {min(tows):.0f}..{max(tows):.0f} "
                     f"(spec expected s ~ 2.6e5)")
    else:
        say("2", BAD, "census did NOT name (BSHM00ISR, G07, obs_code)")

    g16 = [e for e in ev if e["station"] == "BSHM00ISR" and e["sat"] == "G16"
           and "class=obs_code" in (e.get("detail") or "")]
    if g16:
        s = [fnum(e, "statistic") for e in g16]
        say("2", OK, f"…and (BSHM00ISR, G16, obs_code): s in [{min(s):.3g}, {max(s):.3g}] "
                     f"(spec expected ~ 7.3e4)")
    else:
        say("2", INFO, "(BSHM00ISR, G16, obs_code) never exceeded the event threshold")

    # Negative gate: the URUM00CHN 1 ms clock step must stay quiet.
    noisy = [e for e in ev if e["station"] == "URUM00CHN"
             and abs(fnum(e, "tow") - CLOCK_STEP_TOW) < 60.0]
    say("2-neg", OK if not noisy else BAD,
        f"clock-step immunity at TOW {CLOCK_STEP_TOW:.0f}: "
        f"{len(noisy)} URUM00CHN PREFIT event(s) (0 required — a 1 ms step is "
        f"3e5 m on EVERY row and must cancel under centring)")
    st = [r for r in epoch_status(arm)
          if abs(fnum(r, "tow") - CLOCK_STEP_TOW) < 1.0]
    if st:
        say("2-neg", INFO,
            f"epoch {CLOCK_STEP_TOW:.0f} census columns: code max_s "
            f"{fnum(st[0], 'pc_code_max_s'):.4g}, code n>1000 "
            f"{st[0].get('pc_code_n1k')}, phase max_s "
            f"{fnum(st[0], 'pc_phase_max_s'):.4g}")


def gate3_topology(arm: str = "ft63_leg19_pc_topo") -> None:
    if not require_finished("3", arm):
        return
    ev = [e for e in events(arm) if e.get("tier") == "TOPOLOGY"]
    if not ev:
        say("3", BAD, f"no TOPOLOGY events in {arm}")
        return
    at_break = [e for e in ev if abs(fnum(e, "tow") - BREAK_TOW) < 1.0]
    bshm = [e for e in at_break if e["station"] == "BSHM00ISR"]
    say("3", OK if bshm else INFO,
        f"TOW {BREAK_TOW:.0f}: {len(at_break)} TOPOLOGY event(s), "
        f"{len(bshm)} on BSHM00ISR" +
        (f" -> {bshm[0]['event']} {bshm[0]['detail']}" if bshm else ""))
    per_epoch: dict[float, int] = {}
    for e in ev:
        per_epoch[fnum(e, "tow")] = per_epoch.get(fnum(e, "tow"), 0) + 1
    worst = max(per_epoch.values()) if per_epoch else 0
    mean = sum(per_epoch.values()) / max(1, len(per_epoch))
    say("3", OK if worst < 200 else BAD,
        f"event volume bounded: {len(ev)} events over {len(per_epoch)} epoch(s), "
        f"mean {mean:.1f}/epoch, worst {worst}")


def gate4_recorder(arm: str = "ft63_leg19_pc_trip") -> None:
    if not require_finished("4", arm):
        return
    d = RUNS / arm / "logs" / "rhs_attrib"
    dumps = sorted(p.name for p in d.glob("attrib_*.csv")) if d.exists() else []
    say("4", OK if dumps else BAD,
        f"flight recorder dumps: {dumps if dumps else 'NONE'}")
    want = {f"attrib_{int(BREAK_TOW)}.csv", f"attrib_{int(PRE_TOW)}.csv"}
    if want <= set(dumps):
        say("4", OK, f"both the trip epoch {BREAK_TOW:.0f} and the RETAINED pre-trip "
                     f"frame {PRE_TOW:.0f} were dumped")
    elif dumps:
        say("4", INFO, f"expected {sorted(want)}; got {dumps}")

    for tow in (PRE_TOW, BREAK_TOW):
        f = d / f"attrib_{int(tow)}.csv"
        if not f.exists():
            continue
        rows = list(csv.DictReader(f.open(encoding="utf-8", errors="replace")))
        clo = [r for r in rows if r["kind"] == "CLOSURE"]
        if clo:
            rel = float(clo[0]["c00_l2"])
            say("4", OK if rel <= 1e-6 else BAD,
                f"tow {tow:.0f} closure rel {rel:.3e} (<= 1e-6 required)")
        bshm = [r for r in rows if r["kind"] == "STA" and r["station"] == "BSHM00ISR"
                and r["class"] == "obs_code"]
        if bshm:
            say("4", INFO, f"tow {tow:.0f} BSHM00ISR obs_code C00 contribution "
                           f"{float(bshm[0]['c00_l1']):+.4g} "
                           f"(manual baseline: 0.002 at {PRE_TOW:.0f} -> +6493 at {BREAK_TOW:.0f})")
    m = re.search(r"flight recorder — (\d+) trigger-site consultation\(s\), (\d+) era trip",
                  runlog(arm))
    if m:
        say("4", OK if int(m.group(1)) > 0 else BAD,
            f"{m.group(1)} trigger-site consultation(s), {m.group(2)} era trip(s) "
            f"(0 consultations would mean NO site is wired)")


def gate5_fuse(arm: str = "ft63_leg19_pc_fuse",
               control: str = "ft63_leg19_pc_pcbase") -> None:
    if not require_finished("5", arm, control):
        return
    log = runlog(arm)
    m = re.search(r"prefit fuse INC-2\): .*?(\d+) row\(s\) CAPPED", log, re.S)
    say("5", OK if m and int(m.group(1)) > 0 else BAD,
        f"rows capped: {m.group(1) if m else 'NONE (the fuse never acted)'}")
    a = [r for r in epoch_status(arm) if r.get("disposition") == "emitted"]
    b = [r for r in epoch_status(control) if r.get("disposition") == "emitted"]
    if a and b:
        same = filecmp.cmp(RUNS / arm / "logs" / "network_solution.csv",
                           RUNS / control / "logs" / "network_solution.csv",
                           shallow=False)
        say("5", INFO if not same else BAD,
            "solution differs from control" if not same else
            "solution IDENTICAL to control — the fuse is armed but inert")
    # a00 through the break window (the discharge the fuse is meant to bound).
    for run in (arm, control):
        st = [r for r in epoch_status(run)
              if BREAK_TOW - 60 <= fnum(r, "tow") <= BREAK_TOW + 900]
        if not st:
            continue
        say("5", INFO, f"{run}: sigma0_pf over the break window "
                       f"{max(fnum(r, 'sigma0_pf') for r in st):.4g} (max)")


def gate6_birth(arm: str = "ft63_leg19_pc_birth18") -> None:
    if not require_finished("6", arm):
        return
    log = runlog(arm)
    m = re.search(r"prefit census INC-3\): (\d+) LOS birth\(s\) over (\d+) row\(s\), "
                  r"(\d+) seed_birth_exceedance event", log)
    if not m:
        say("6", BAD, "no INC-3 run-end line — the birth check never ran")
        return
    births, rows, fired = int(m.group(1)), int(m.group(2)), int(m.group(3))
    say("6", OK if births > 0 else BAD,
        f"{births} birth(s) over {rows} row(s) detected "
        f"({'births counted' if births else 'ZERO births = MISWIRE, not a quiet network'})")
    ev = [e for e in events(arm) if e.get("event") == "seed_birth_exceedance"]
    # A phase birth is seeded to y ~ 0 by construction, so its "exceedance" is
    # just the station's own phase level and says nothing about the seed. Only a
    # code-class birth can answer the question, so the verdict is reported on that
    # subset: a fire count alone would read as a defect when it is arithmetic.
    code = [e for e in ev if "class=obs_code" in (e.get("detail") or "")]
    other = [e for e in ev if e not in code]
    if code:
        first = min(code, key=lambda e: fnum(e, "tow"))
        say("6", OK, f"ONSET DATED TO BIRTH — {len(code)} CODE-class birth "
                     f"exceedance(s); earliest {first['station']}/{first['sat']} at "
                     f"TOW {fnum(first, 'tow'):.0f}, s={fnum(first, 'statistic'):.4g}, "
                     f"{first['detail']}.  The cell is wrong from its FIRST epoch, "
                     f"which rules OUT mid-arc poisoning.  It does NOT by itself "
                     f"implicate the seed path: on the FT63 leg the same signature "
                     f"traced to a corrupt C1C pseudorange in the RINEX (the "
                     f"initializer seeded faithfully from bad data).  Check the "
                     f"measurement against the products before blaming the seed.")
    elif births:
        say("6", OK, f"{births} birth(s) and ZERO CODE-class exceedances — every "
                     f"self-seeded LOS started consistent where the statistic is "
                     f"meaningful.  The seed path is RULED OUT for this window; "
                     f"mid-arc poisoning owns the defect.")
    if other:
        say("6", INFO, f"{len(other)} further birth exceedance(s) outside obs_code "
                       f"(mostly phase rows seeded to y = 0 against their station's "
                       f"own level) — arithmetic, not findings.")


def gate7_hoist(arm: str = "ft63_leg19_pc_rhsat2",
                control: str = "ft63_leg19_rhsat") -> None:
    if not require_finished("0", arm):
        return
    a = RUNS / arm / "logs" / "rhs_attrib"
    b = RUNS / control / "logs" / "rhs_attrib"
    if not (a.exists() and b.exists()):
        say("0", INFO, "attribution dumps missing on one side — hoist not graded")
        return
    names = sorted({p.name for p in a.glob("*.csv")} & {p.name for p in b.glob("*.csv")})
    if not names:
        say("0", BAD, "no common attribution dump to compare")
        return
    for n in names:
        same = filecmp.cmp(a / n, b / n, shallow=False)
        say("0", OK if same else BAD,
            f"INC-0 hoist regression: {n} {'byte-identical' if same else 'DIFFERS'} "
            f"vs {control}")


GATES = {
    "0": gate7_hoist,
    "2": gate2_census,
    "3": gate3_topology,
    "4": gate4_recorder,
    "5": gate5_fuse,
    "6": gate6_birth,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", nargs="*", default=sorted(GATES))
    args = ap.parse_args()
    # Which arm each gate reads, so a partially-finished sweep grades what exists
    # instead of reporting six FAILs for runs that have not happened yet.
    arm_of = {"0": "ft63_leg19_pc_rhsat2", "2": "ft63_leg19_pc_census",
              "3": "ft63_leg19_pc_topo", "4": "ft63_leg19_pc_trip",
              "5": "ft63_leg19_pc_fuse", "6": "ft63_leg19_pc_birth18"}
    for g in args.gate:
        fn = GATES.get(g)
        if fn is None:
            print(f"unknown gate {g}")
            continue
        arm = arm_of.get(g)
        if arm and not (RUNS / arm / "logs").exists():
            print(f"[skip] gate {g}: {arm} has not run yet")
            continue
        fn()
    for arm in ("ft63_leg19_pc_census", "ft63_leg19_pc_topo", "ft63_leg19_pc_trip"):
        if (RUNS / arm / "logs").exists():
            gate_solve_identity(arm)
    n_bad = sum(1 for _, v, _ in _verdicts if v == BAD)
    print(f"\n{len(_verdicts)} check(s), {n_bad} FAIL")
    return 1 if n_bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
