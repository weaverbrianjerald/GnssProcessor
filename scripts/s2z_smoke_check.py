#!/usr/bin/env python3
"""SPEC_s2z_realization_ab Phase-0/1 smoke assertions (staged-cadence gate).

Runs against the 10-epoch s2z smoke (runs/s2z_ab/smoke) before the 6 h seed is
launched. Encodes the spec's Phase-1 smoke gate list:

  1. loader sweep clean: the sum-to-zero activation line is present and no
     s2z warn/mutex fired (no "IGNORED under", no "INERT under
     network.clock_datum_sum_to_zero"), no [error]/[critical] anywhere;
  2. declared-dimension accounting: network_trace.csv must read
     declared_dim == 0 with gate_ok == 1 and solved == 1 on every row (the pin
     lineage reads declared_dim == 1); the rank gate passing with the
     Sigma-row-closed gauge confirms the brdc-anchored card's silent-empty-
     product failure mode is not occurring. (n_pivot_pins is the assembly pin
     total, not the clock pivot count; reported, not asserted);
  3. first processed epoch is 13:00:00 GPS exactly (tow 306000; the half-epoch
     back-off discipline), 10 epochs, every disposition 'emitted';
  4. run-end counters live (invariant #4): producer-complete line reads
     10 emitted / 0 skipped; keyed landing-site trace has rows for all
     3 kinds (STA/SAT/SIG); products.txt and the snapshot M/N header exist.

Exit 0 = PASS (seed may launch), 1 = FAIL with every violated assertion listed.

Usage:
    python scripts/s2z_smoke_check.py [--smokedir runs/s2z_ab/smoke]
"""
import argparse
import csv
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

FIRST_TOW = 306000.0     # 13:00:00 GPS of DOY-129 (day anchor tow 259200)
N_EPOCHS = 10
EPOCH_S = 30.0


def grep(path, pattern):
    rx = re.compile(pattern)
    hits = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if rx.search(line):
                    hits.append(line.rstrip())
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smokedir", default=r"C:\GnssProcessor\runs\s2z_ab\smoke")
    args = ap.parse_args()
    d = os.path.abspath(args.smokedir)
    logs = os.path.join(d, "logs")
    fails = []
    note = []

    def check(ok, msg):
        (note if ok else fails).append(("PASS " if ok else "FAIL ") + msg)

    # --- 1. loader sweep ---------------------------------------------------
    gcl = os.path.join(logs, "gnss_config.log")
    act = grep(gcl, r"SUM-TO-ZERO clock datum ON")
    check(len(act) >= 1, f"s2z activation line in gnss_config.log (found {len(act)})")
    dirty = grep(gcl, r"IGNORED under network\.clock_datum_sum_to_zero"
                      r"|INERT under network\.clock_datum_sum_to_zero")
    check(not dirty, f"no s2z warned-inert/ignored knobs (found {len(dirty)})")
    for line in dirty:
        note.append("      " + line[:150])
    mlog = os.path.join(logs, "main_log.log")
    nerr = len(grep(mlog, r"\[(error|critical)\]")) + len(grep(gcl, r"\[(error|critical)\]"))
    check(nerr == 0, f"no error/critical log lines (found {nerr})")

    # --- 2. declared-dim accounting ---------------------------------------
    ntr = os.path.join(logs, "network_trace.csv")
    rows = []
    if os.path.isfile(ntr):
        with open(ntr, newline="") as fh:
            rows = list(csv.DictReader(fh))
    check(len(rows) == N_EPOCHS, f"network_trace.csv rows == {N_EPOCHS} (got {len(rows)})")
    bad_dim = [r for r in rows if r.get("declared_dim") != "0"]
    bad_gate = [r for r in rows if r.get("gate_ok") != "1" or r.get("solved") != "1"]
    check(not bad_dim, f"declared_dim == 0 on all rows ({len(bad_dim)} bad)")
    check(not bad_gate, f"gate_ok == 1 and solved == 1 on all rows ({len(bad_gate)} bad)")
    if rows:
        note.append(f"      n_pivot_pins (assembly total, informational): "
                    f"{rows[0].get('n_pivot_pins')} .. {rows[-1].get('n_pivot_pins')}")

    # --- 3. window + dispositions ------------------------------------------
    esp = os.path.join(logs, "network_epoch_status.csv")
    es = []
    if os.path.isfile(esp):
        with open(esp, newline="") as fh:
            es = list(csv.DictReader(fh))
    check(len(es) == N_EPOCHS, f"epoch_status rows == {N_EPOCHS} (got {len(es)})")
    if es:
        t0 = float(es[0]["tow"])
        check(abs(t0 - FIRST_TOW) < 1.0,
              f"first epoch tow {t0:.3f} == {FIRST_TOW:.0f} (13:00:00, back-off held)")
        nde = [r for r in es if r.get("disposition") != "emitted"]
        check(not nde, f"every disposition 'emitted' ({len(nde)} not)")

    # --- 4. run-end counters / instruments ---------------------------------
    done = grep(mlog, r"producer complete")
    ok_line = bool(done) and f"{N_EPOCHS} epochs emitted, 0 skipped" in done[-1]
    check(ok_line, "producer-complete line reads 10 emitted / 0 skipped"
          + (f"  [{done[-1].split(']')[-1].strip()[:90]}]" if done else "  [line absent]"))
    pk = os.path.join(logs, "network_pf_keyed.csv")
    kinds = set()
    if os.path.isfile(pk):
        with open(pk, newline="") as fh:
            for r in csv.DictReader(fh):
                kinds.add(r.get("kind", ""))
    check({"STA", "SAT", "SIG"} <= kinds,
          f"keyed landing-site trace live with STA/SAT/SIG rows (kinds={sorted(kinds)})")
    prod = os.path.join(d, "products.txt")
    check(os.path.isfile(prod) and os.path.getsize(prod) > 10000,
          f"products.txt non-trivial ({os.path.getsize(prod) if os.path.isfile(prod) else 0} B)")
    snap = os.path.join(d, "snap.txt")
    mline = grep(snap, r"^M,")[:1] if os.path.isfile(snap) else []
    check(bool(mline), "snapshot M header present")
    if mline:
        f = mline[0].split(",")
        note.append(f"      snapshot week/tow {f[1]}/{f[2]}  fingerprint {f[5]}")

    for line in note:
        print(line)
    for line in fails:
        print(line)
    print()
    if fails:
        print(f"SMOKE GATE: FAIL ({len(fails)} assertion(s)) — do NOT launch the seed")
        return 1
    print("SMOKE GATE: PASS — seed launch sanctioned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
