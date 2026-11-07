#!/usr/bin/env python3
"""crs2_matrix_diff.py (card CRS2): two scenario-matrix labels, cell by cell.

Reads the two labels' grades.json (scripts/scenario_matrix.py grade) and prints, for every
cell both hold: the verdict of each label, the channels whose measured value E or spread S
changed (beyond a printed tolerance in mm), and the channels that are identical to the
printed precision.  The summary counts the verdict changes and, per channel, the cells in
which the channel moved: the prediction of card CRS2 section 3 (G5) is that the channels
that read the producer (clk, pb, zwd, iono, emit, clk_truth) do not move in any cell when
only the consumer's resume changed, and that cons_peak, cons_plat and cons_p50 do.

    python scripts/crs2_matrix_diff.py --a smx1 --b crs2 [--tol-mm 0.05] [--md out.md]

Card: docs/refound/CRS2_resume_hold_and_latches_2026-11-06.md.
"""
import argparse
import json
import os
import sys

GATES = "D:/GnssProcessorSupport/gates/smx"
PRODUCER = ("clk", "pb", "zwd", "iono", "emit", "clk_truth")
CONSUMER = ("cons_peak", "cons_plat", "cons_p50")


def load(label):
    p = f"{GATES}/{label}/grades.json"
    if not os.path.exists(p):
        raise SystemExit(f"{p}: missing (run scenario_matrix.py grade --label {label})")
    g = json.load(open(p, encoding="utf-8"))
    return {(c["row"], c["posture"]): c for c in g["cells"]}, g


def verdict(c):
    if c.get("no_effect"):
        return "PASS NO-EFFECT" if c.get("pass") else "FAIL NO-EFFECT"
    return "PASS" if c.get("pass") else "FAIL"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="the reference label (smx1)")
    ap.add_argument("--b", required=True, help="the label under test")
    ap.add_argument("--tol-mm", type=float, default=0.05)
    ap.add_argument("--md", default="")
    a = ap.parse_args()
    A, ga = load(a.a)
    B, gb = load(a.b)
    shared = sorted(set(A) & set(B))
    lines = []
    out = lines.append
    out(f"| row | posture | {a.a} | {a.b} | changed channels (E {a.a} -> {a.b} / bar; S {a.a} -> {a.b}) |")
    out("|---|---|---|---|---|")
    n_verdict = 0
    moved = {}
    for key in shared:
        ca, cb = A[key], B[key]
        va, vb = verdict(ca), verdict(cb)
        changes = []
        for ch in sorted(set(ca.get("channels", {})) | set(cb.get("channels", {}))):
            xa, xb = ca["channels"].get(ch), cb["channels"].get(ch)
            if xa is None or xb is None:
                changes.append(f"{ch}: only in {a.a if xa else a.b}")
                moved[ch] = moved.get(ch, 0) + 1
                continue
            dE = abs(xa["E"] - xb["E"])
            dS = abs(xa["S"] - xb["S"])
            if dE > a.tol_mm or dS > a.tol_mm:
                changes.append(f"{ch}: {xa['E']:.1f} -> {xb['E']:.1f} / {xb['bar']:.1f}; S {xa['S']:.1f} -> {xb['S']:.1f}"
                               + (" X" if not xb["pass"] else ""))
                moved[ch] = moved.get(ch, 0) + 1
        if va != vb:
            n_verdict += 1
        out(f"| {key[0]} | {key[1]} | {va} | {vb}{' **' if va != vb else ''} | {'; '.join(changes) or 'none'} |")
    out("")
    out(f"{len(shared)} shared cell(s), {n_verdict} verdict change(s); cells with a moved channel: "
        + ", ".join(f"{ch} {moved.get(ch, 0)}" for ch in PRODUCER + CONSUMER)
        + f" (tolerance {a.tol_mm} mm on E and S).")
    out(f"producer channels moved in {sum(moved.get(ch, 0) for ch in PRODUCER)} cell-channel(s); "
        f"consumer channels moved in {sum(moved.get(ch, 0) for ch in CONSUMER)}.")
    pa, pb = sum(1 for k in shared if A[k].get("pass")), sum(1 for k in shared if B[k].get("pass"))
    out(f"pass counts over the shared cells: {a.a} {pa}, {a.b} {pb}.")
    only_a, only_b = sorted(set(A) - set(B)), sorted(set(B) - set(A))
    if only_a or only_b:
        out(f"cells only in {a.a}: {only_a}; only in {a.b}: {only_b}")
    text = "\n".join(lines)
    print(text)
    if a.md:
        open(a.md, "w", encoding="utf-8").write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
