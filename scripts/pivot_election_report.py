#!/usr/bin/env python3
"""Clock-pivot report of a network run: the elections, the handovers and the realized pivots.

    python scripts/pivot_election_report.py <run_dir> [<run_dir> ...]

Reads, per run directory (the directory that holds ``logs/`` and ``products.txt``):

* ``logs/network_events.csv``: the ``PIVOT_ELECT`` rows written by ``network.pivot_election``
  (event ``elected`` with the mean station count in ``statistic`` and the runner-up in the
  detail, or ``unranked``) and the ``PIVOT`` handover rows (the pivot a constellation moved to).
* ``products.txt``: the realized pivots of the S lines (``<constellation>:<satellite>``
  entries), listed at the epoch of the first S line and at every change.  An S line is
  written at the start of the product and re-broadcast mid-file; the time is the time of
  the last ``E`` record before it.

Measure-only: reads text, writes nothing.  The report states what the election did; it grades
nothing (the clock product, the drift and the consumer are graded by their own scripts).
"""
import csv
import os
import sys


def hms(tow):
    s = float(tow) % 86400.0
    return "%02d:%02d:%02d" % (int(s // 3600), int(s % 3600 // 60), int(s % 60))


def events(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            if row.get("tier") in ("PIVOT", "PIVOT_ELECT"):
                out.append(row)
    return out


def realized_pivots(path):
    """[(tow, {constellation: satellite})] at the first S line and at every change."""
    out, last, tow = [], None, None
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("E,"):
                try:
                    tow = float(line.split(",")[2])
                except (IndexError, ValueError):
                    pass
            elif line.startswith("S,"):
                piv = {}
                for fld in line.rstrip("\n").split(",")[11:]:
                    if ":" in fld and "=" not in fld:
                        c, sat = fld.split(":", 1)
                        piv[c] = sat
                if piv != last:
                    out.append((tow, piv))
                    last = piv
    return out


def report(run):
    print("== %s" % run)
    ev = events(os.path.join(run, "logs", "network_events.csv"))
    elect = [r for r in ev if r["tier"] == "PIVOT_ELECT"]
    hand = [r for r in ev if r["tier"] == "PIVOT"]
    print("  elections: %d, handover rows: %d" % (len(elect), len(hand)))
    for r in elect:
        stat = r.get("statistic") or ""
        print("    %s  %-9s %-4s mean stations %-9s %s" % (
            hms(r["tow"]), r["event"], r["sat"], stat, r.get("detail", "")))
    for r in hand:
        print("    %s  handover  -> %-4s %s" % (hms(r["tow"]), r["sat"], r.get("detail", "")))
    piv = realized_pivots(os.path.join(run, "products.txt"))
    print("  realized pivots on the S line (first and each change):")
    if not piv:
        print("    (no products.txt or no S line)")
    for tow, p in piv:
        print("    %s  %s" % (hms(tow) if tow is not None else "start",
                              " ".join("%s:%s" % kv for kv in sorted(p.items())) or "(none)"))
    changes = max(0, len(piv) - 1)
    print("  pivot changes on the S line: %d" % changes)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    for run in argv:
        report(run)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
