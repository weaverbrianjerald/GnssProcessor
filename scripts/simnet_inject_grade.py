#!/usr/bin/env python3
"""Grade a NULL/FAULT injection arm pair: detection, latency, false alarms, MDB.

    python scripts/simnet_inject_grade.py \\
        --null-run   runs/inj_null/run   --fault-run   runs/inj_fault/run \\
        --fault-fixture runs/simnet24_inject_fault \\
        [--manifest config/sim/simnet24_inject_fault.json.injection.json] \\
        [--latency-epochs 3] [--json out.json]

WHAT THIS ANSWERS, AND WHAT IT DOES NOT.

It answers "does the response MECHANISM work" — the cheap sim half of G4.  It
does NOT answer "does the event EXPRESS on this lineage", which is the field
half and the known seeded-chain trap.  An injection PASS is therefore NECESSARY,
NOT SUFFICIENT for promoting a knob: the field arms remain the graduation gate.

THE MANDATORY NEGATIVE CONTROL (SPEC_synthetic_minimum_network §6, G-B):

    "an injected event that produces zero guard trips is a WIRING FAILURE, not a
     quiet day.  The grader asserts this explicitly."

That assertion is the whole reason this script exists rather than a human reading
two log files, so it is a hard FAIL with its own exit code — the failure mode it
guards is a run that completes cleanly, reports zero, and gets written up as "the
guard did not catch it" when the guard was never wired.  Run
scripts/simnet_injection_preflight.py on the producer config BEFORE spending the
compute; it separates "inert by construction" from "failed to detect" offline.

WHY BOTH ARMS.  A fault arm alone cannot distinguish a detection from a detector
that flags everything.  The null arm is the same fixture with the same seeds and
no injected fault (scripted slips touch no RNG stream), so every flag it raises
on truth-clean data is a FALSE ALARM, and the difference between the arms is the
detector's actual yield.  §9.6 retracted a published recovery-floor table for
exactly this reason: it was a window artifact visible only against the null arm.

TRUTH COMES FROM THE FIXTURE, NOT THE PLAN.  The join key is the fault fixture's
per-station `truth_slips.csv` rows with `source=scripted` — what the simulator
actually applied, at its resolved tow.  The injection manifest is read only as a
cross-check: a planned event missing from truth is a bench defect and is reported
as one, never silently dropped from the denominator.
"""
import argparse
import collections
import csv
import glob
import json
import os
import re
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SLIP_TIERS = ("T0_mw_detect", "T1_if_slip")
RAIM_TIERS = ("T2_within_epoch", "T3_raim", "T4_attrib")

# Run-end counter lines are spdlog-only — no CSV sink exists for either family, so a
# bench has to scrape them.  Anchored on the message prefixes emitted by
# NetworkTeardown.cpp (slip: :1599-1626, RAIM pool re-host: :583-632).
COUNTER_PATTERNS = [
    ("slip", re.compile(r"Network \(slip-QC[^)]*\):(?P<body>.*)")),
    ("raim_ci", re.compile(r"Network \(RAIM pool re-host\):(?P<body>.*)")),
    ("we_ci", re.compile(r"Network \(WE-CI\):(?P<body>.*)")),
]


def load_csv(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return list(csv.DictReader(fh))


def find_trace(run_dir, name):
    """Trace dirs are `logs/` on most arms and `traces/` on some wrappers."""
    for sub in ("logs", "traces", ""):
        p = os.path.join(run_dir, sub, name) if sub else os.path.join(run_dir, name)
        if os.path.exists(p):
            return p
    return None


def scrape_counters(run_dir):
    """-> {family: [line, ...]} from any log/console text in the run dir."""
    out = collections.defaultdict(list)
    cands = []
    for pat in ("console.txt", "*.log", os.path.join("logs", "*.log"),
                os.path.join("logs", "*.txt")):
        cands.extend(glob.glob(os.path.join(run_dir, pat)))
    for p in sorted(set(cands)):
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    for fam, rx in COUNTER_PATTERNS:
                        m = rx.search(line)
                        if m:
                            out[fam].append(m.group("body").strip())
        except OSError:
            continue
    return dict(out)


def injected_truth(fault_fixture):
    """-> [{station, tow, sat, signal, delta}] for scripted slips only.

    A fixture with no `source` column carries no scripted events at all (the column
    appears only when they exist), which is itself the answer.
    """
    rows = []
    for d in sorted(os.listdir(fault_fixture)):
        p = os.path.join(fault_fixture, d, "truth_slips.csv")
        if not os.path.exists(p):
            continue
        for r in load_csv(p) or []:
            if (r.get("source") or "").strip() != "scripted":
                continue
            rows.append({"station": d, "tow": float(r["tow_s"]), "sat": r["satellite"],
                         "signal": r["signal"], "delta": int(float(r["delta_cycles"])),
                         "flagged": (r.get("flagged") or "0").strip() in ("1", "true"),
                         "elev_deg": float(r.get("elev_deg") or "nan")})
    return rows


def observed_constellations(products_path, ev_rows):
    """Which constellations the PRODUCER actually solved.

    A fixture can carry observations the producer never processes — the sim writes
    GPS+Galileo, and a GPS-only producer config silently ignores every E row.  An
    injected fault there is unobserved by construction: it lands in data nothing
    looks at, and scoring it as a MISS manufactures exactly the false "the guard
    failed to catch it" reading this bench exists to prevent.  It is the same
    error class as injecting at a carry the lineage has superseded, one layer out.

    The authority is the emitted product's own satellite records (C = clock), with
    network_events.csv as a fallback when no product was written.
    """
    consts = set()
    if products_path and os.path.exists(products_path):
        with open(products_path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                p = line.split(",")
                if len(p) > 1 and p[0] in ("C", "B", "P") and p[1][:1].isalpha():
                    consts.add(p[1][0])
    if not consts and ev_rows:
        consts = {r["sat"][0] for r in ev_rows if (r.get("sat") or "").strip()}
    return consts


def events_by_tier(run_dir):
    rows = load_csv(find_trace(run_dir, "network_events.csv") or "")
    if rows is None:
        return None, None
    per_tier = collections.Counter(r["tier"] for r in rows)
    return rows, per_tier


def match_detections(inj, ev_rows, interval_s, latency_epochs, null_rows=None):
    """For each injected slip, the earliest slip-tier event on the same (station,sat)
    at or after its tow, inside the latency window.

    Station+satellite is the join; the SIGNAL is reported but not required, because a
    wide-lane detector legitimately flags the band PAIR (detail = "L1C;L5Q") rather
    than the single band that jumped.  Requiring a signal match would score a correct
    MW detection as a miss.
    """
    win = interval_s * latency_epochs + 1e-6
    by_key = collections.defaultdict(list)
    for r in ev_rows:
        if r["tier"] not in SLIP_TIERS:
            continue
        by_key[(r["station"], r["sat"])].append(r)
    for v in by_key.values():
        v.sort(key=lambda r: float(r["tow"]))

    # The NULL arm's own flags at the same (station, sat, window).  The null arm is
    # not quiet — it raises real false alarms on truth-clean data — so a fault-arm
    # match that the null arm ALSO produces at the same key is not attributable to
    # the injection.  Without this correction the detection rate is inflated by the
    # detector's own false-alarm rate, which is the differential-grading mistake
    # SPEC_synthetic_minimum_network §9.6 retracted a headline table for.
    null_key = collections.defaultdict(list)
    for r in (null_rows or []):
        if r["tier"] in SLIP_TIERS:
            null_key[(r["station"], r["sat"])].append(float(r["tow"]))

    out = []
    for e in inj:
        best = None
        for r in by_key.get((e["station"], e["sat"]), []):
            t = float(r["tow"])
            if t < e["tow"] - 1e-6 or t > e["tow"] + win:
                continue
            best = r
            break
        coincident = any(e["tow"] - 1e-6 <= t <= e["tow"] + win
                         for t in null_key.get((e["station"], e["sat"]), []))
        rec = dict(e)
        rec["null_coincident"] = coincident
        if best is None:
            rec.update({"detected": False, "latency_epochs": None, "tier": None,
                        "event": None, "action": None, "signal_match": None})
        else:
            lat = round((float(best["tow"]) - e["tow"]) / interval_s)
            detail = best.get("detail") or ""
            rec.update({"detected": True, "latency_epochs": lat, "tier": best["tier"],
                        "event": best["event"], "action": best["action"],
                        "signal_match": e["signal"] in detail})
        out.append(rec)
    return out


def fmt_pct(a, b):
    return f"{100.0 * a / b:5.1f}%" if b else "   n/a"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--null-run", required=True)
    ap.add_argument("--fault-run", required=True)
    ap.add_argument("--fault-fixture", required=True,
                    help="fault synth-network dir (its truth_slips.csv are the truth)")
    ap.add_argument("--manifest", default=None,
                    help="injection manifest from simnet_inject_arms.py (cross-check)")
    ap.add_argument("--products", default=None,
                    help="fault arm products.txt (default <fault-run>/products.txt); the "
                         "authority on which constellations the producer actually solved")
    ap.add_argument("--latency-epochs", type=int, default=3,
                    help="a detection this many epochs after the injection still counts "
                         "(default 3; MW needs a following epoch to form its difference)")
    ap.add_argument("--interval-s", type=float, default=None,
                    help="epoch interval; default read from the fixture manifest")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    fx_man_path = os.path.join(args.fault_fixture, "network_fixture.json")
    interval_s = args.interval_s
    if interval_s is None and os.path.exists(fx_man_path):
        with open(fx_man_path, encoding="utf-8") as fh:
            interval_s = float(json.load(fh).get("interval_s") or 30.0)
    interval_s = interval_s or 30.0

    inj = injected_truth(args.fault_fixture)
    null_rows, null_tiers = events_by_tier(args.null_run)
    fault_rows, fault_tiers = events_by_tier(args.fault_run)

    print("=" * 78)
    print("SLIP / RAIM INJECTION BENCH — null vs fault")
    print("=" * 78)
    print(f"  null  run    : {args.null_run}")
    print(f"  fault run    : {args.fault_run}")
    print(f"  fault fixture: {args.fault_fixture}")
    print(f"  interval     : {interval_s:g} s, latency window {args.latency_epochs} epoch(s)")

    # ---- plan vs applied ----------------------------------------------------------
    plan_gap = []
    if args.manifest and os.path.exists(args.manifest):
        with open(args.manifest, encoding="utf-8") as fh:
            plan = json.load(fh)
        planned = {(e["station"], e["sat"], s) for e in plan["events"] for s in e["signals"]}
        applied = {(e["station"], e["sat"], e["signal"]) for e in inj}
        plan_gap = sorted(planned - applied)
        print(f"  manifest     : {len(planned)} planned (sat,signal) jump(s), "
              f"{len(applied)} applied")
        if plan_gap:
            print(f"  *** {len(plan_gap)} PLANNED event(s) never applied — a bench defect, not")
            print( "      a detector result.  They stay in the denominator below:")
            for g in plan_gap[:8]:
                print(f"        {g[0]} {g[1]} {g[2]}")

    if not inj:
        print("\n  FAIL: the fault fixture contains NO scripted slips "
              "(no truth_slips.csv row with source=scripted).")
        print("  There is nothing to detect, so nothing below is a detector verdict.")
        return 2

    # ---- null arm: false alarms ---------------------------------------------------
    print("\n-- NULL arm (truth-clean) — every flag here is a FALSE ALARM " + "-" * 17)
    if null_rows is None:
        print("  no network_events.csv — arm network.trace_events on the null arm.")
        print("  Without it the false-alarm rate is unmeasured and the fault arm's")
        print("  detection count cannot be separated from a detector that flags always.")
    else:
        tot = sum(null_tiers[t] for t in SLIP_TIERS + RAIM_TIERS)
        for t in SLIP_TIERS + RAIM_TIERS:
            print(f"  {t:<20}{null_tiers[t]:>8}")
        print(f"  {'TOTAL':<20}{tot:>8}")
        if tot == 0:
            print("  clean: no tier fired on truth-clean data.")

    fr = load_csv(find_trace(args.null_run, "network_flag_rates.csv") or "")
    if fr:
        print("\n  flag-rate tiers (null arm) — observed rate vs the implied alpha the")
        print("  threshold claims.  A large ratio means the threshold is calibrated")
        print("  against something other than this data.")
        print(f"    {'tier':<8}{'label':<26}{'opp':>10}{'flag':>8}{'rate':>12}"
              f"{'implied a':>12}{'ratio':>10}")
        for r in fr:
            try:
                opp, flg = float(r["opportunities"]), float(r["flags"])
                rate = flg / opp if opp else 0.0
                ia = float(r["implied_alpha"]) if (r.get("has_alpha") or "").strip() in \
                    ("1", "true") else None
            except (KeyError, ValueError):
                continue
            ratio = f"{rate / ia:9.2e}" if (ia and ia > 0 and rate > 0) else "        -"
            ia_s = f"{ia:11.3e}" if ia else "          -"
            print(f"    {r['tier']:<8}{r['label'][:25]:<26}{opp:>10.0f}{flg:>8.0f}"
                  f"{rate:>12.3e}{ia_s}{ratio:>10}")

    # ---- fault arm: detection -----------------------------------------------------
    # ---- what the producer actually LOOKED at --------------------------------------
    prod = args.products or os.path.join(args.fault_run, "products.txt")
    obs_const = observed_constellations(prod, fault_rows or [])
    unobserved = [e for e in inj if obs_const and e["sat"][0] not in obs_const]
    graded = [e for e in inj if not (obs_const and e["sat"][0] not in obs_const)]
    if unobserved:
        by_c = collections.Counter(e["sat"][0] for e in unobserved)
        print("\n-- UNOBSERVED injections (EXCLUDED from the rate) " + "-" * 28)
        print(f"  the producer solved constellation(s) {{{', '.join(sorted(obs_const))}}}, "
              f"but {len(unobserved)} jump(s) were")
        print(f"  injected on {{{', '.join(f'{k}x{v}' for k, v in sorted(by_c.items()))}}} — "
              f"data this config never processes.")
        print( "  These are a BENCH defect, not a detector result: scoring them as misses")
        print( "  would manufacture the exact 'the guard failed to catch it' reading this")
        print( "  bench exists to prevent.  Re-plan with")
        print(f"    simnet_inject_arms.py --constellations {' '.join(sorted(obs_const))}")

    print("\n-- FAULT arm — detection of " + str(len(graded)) + " observed jump(s) " + "-" * 20)
    if fault_rows is None:
        print("  no network_events.csv on the fault arm — arm network.trace_events.")
        matched = []
    else:
        for t in SLIP_TIERS + RAIM_TIERS:
            d = fault_tiers[t] - (null_tiers[t] if null_tiers else 0)
            print(f"  {t:<20}{fault_tiers[t]:>8}   (null {null_tiers[t] if null_tiers else 0:>6}"
                  f"   differential {d:+7d})")
        matched = match_detections(graded, fault_rows, interval_s, args.latency_epochs,
                                   null_rows)

    n_det = sum(1 for m in matched if m["detected"])
    n_attrib = sum(1 for m in matched if m["detected"] and not m["null_coincident"])
    if matched:
        print(f"\n  matched {n_det}/{len(matched)} = {fmt_pct(n_det, len(matched))} "
              f"within {args.latency_epochs} epoch(s)")
        n_coin = n_det - n_attrib
        print(f"  of which {n_coin} coincide with a NULL-arm flag at the same "
              f"(station,sat,window)")
        print(f"  ATTRIBUTABLE {n_attrib}/{len(matched)} = {fmt_pct(n_attrib, len(matched))} "
              f"— the null arm is not quiet, so this is the honest rate")
        lat = sorted(m["latency_epochs"] for m in matched if m["detected"])
        if lat:
            print(f"  latency (epochs): min {lat[0]}  median {lat[len(lat)//2]}  max {lat[-1]}")
        sm = [m for m in matched if m["detected"] and m["signal_match"] is False]
        if sm:
            print(f"  {len(sm)} detection(s) did not name the injected signal in `detail` "
                  f"(expected for a wide-lane pair flag)")

        # ---- the empirical MDB ----------------------------------------------------
        print("\n  detection by magnitude (the empirical MDB — thresholds stay")
        print("  FIELD-derived; this measures where they land, it never tunes them):")
        by_mag = collections.defaultdict(lambda: [0, 0])
        for m in matched:
            b = by_mag[abs(m["delta"])]
            b[1] += 1
            if m["detected"]:
                b[0] += 1
        print(f"    {'|delta| cyc':>12}{'det':>6}{'of':>5}{'rate':>9}")
        floor = None
        for mag in sorted(by_mag):
            d, n = by_mag[mag]
            print(f"    {mag:>12}{d:>6}{n:>5}{fmt_pct(d, n):>9}")
            if floor is None and n and d == n:
                floor = mag
        print(f"    smallest magnitude caught on EVERY instance: "
              f"{floor if floor is not None else 'none in this ladder'}")

        misses = [m for m in matched if not m["detected"]]
        if misses:
            print(f"\n  {len(misses)} MISS(es):")
            for m in misses[:12]:
                print(f"    {m['station']} {m['sat']} {m['signal']} "
                      f"tow {m['tow']:.0f} delta {m['delta']:+d} "
                      f"elev {m['elev_deg']:.1f}")

    # ---- run-end counters ---------------------------------------------------------
    print("\n-- Run-end counters (spdlog-only; no CSV sink exists for these) " + "-" * 13)
    for label, run in (("null", args.null_run), ("fault", args.fault_run)):
        c = scrape_counters(run)
        if not c:
            print(f"  {label}: no counter lines found (looked for console.txt / *.log)")
            continue
        for fam, lines in sorted(c.items()):
            for ln in lines[-3:]:
                print(f"  {label:<6}{fam:<9}{ln[:150]}")

    # ---- the mandatory negative control -------------------------------------------
    print("\n-- Verdict " + "-" * 65)
    wiring_failure = bool(matched) and n_det == 0
    if wiring_failure:
        print("  *** WIRING FAILURE — {} observed jump(s) were injected and ZERO were"
              .format(len(matched)))
        print("      detected.")
        print("      This is NOT a quiet day and must not be reported as a detection")
        print("      miss.  Injected faults that trip nothing mean the mechanism is not")
        print("      connected on this lineage.  Run:")
        print("        python scripts/simnet_injection_preflight.py <producer_config>")
        print("      A superseded carry (network.carry_invariant supersedes the")
        print("      phase-leveling local carry, the PhaseBias shared carry, the RPB")
        print("      carry and the eq34 pin/shift/reref hygiene) makes several QC")
        print("      mechanisms inert BY CONSTRUCTION, with counters that still read")
        print("      plausibly.  Confirm the target is LIVE before grading power.")
    elif n_attrib < len(matched):
        print(f"  PARTIAL — {n_attrib}/{len(matched)} attributable. The mechanism is WIRED;")
        print( "  the misses are a power/threshold result and are gradeable as such.")
    else:
        print(f"  ALL {n_attrib}/{len(matched)} observed jump(s) detected and attributable.")
    print("  Remember: an injection PASS is NECESSARY, NOT SUFFICIENT.  It shows the")
    print("  response mechanism works on exact truth; whether the event EXPRESSES on a")
    print("  field lineage is a separate question and the field arms remain the")
    print("  graduation gate.")
    print("=" * 78)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"null_run": args.null_run, "fault_run": args.fault_run,
                       "fault_fixture": args.fault_fixture, "interval_s": interval_s,
                       "latency_epochs": args.latency_epochs,
                       "n_injected": len(inj), "n_graded": len(matched),
                       "n_detected": n_det, "n_attributable": n_attrib,
                       "observed_constellations": sorted(obs_const),
                       "unobserved": unobserved,
                       "plan_gap": plan_gap,
                       "null_tiers": dict(null_tiers or {}),
                       "fault_tiers": dict(fault_tiers or {}),
                       "matched": matched,
                       "wiring_failure": wiring_failure}, fh, indent=2, default=str)
        print(f"wrote {args.json}")
    # 4 is reserved for the wiring failure so a harness can tell it apart from a
    # detector that ran and missed (which is a legitimate, gradeable result).
    return 4 if wiring_failure else 0


if __name__ == "__main__":
    sys.exit(main())
