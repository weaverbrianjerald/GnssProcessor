#!/usr/bin/env python3
"""Card Q3 §3 — the wire-mark TIE-OUT for one arm (SPEC §8.4 gate 5).

A trip is graded as a POPULATION, and clause (b) of that grading is "it lands a
wire mark that ties out event <-> R record <-> counter".  Three independent
records of the same edge:

  EVENT   network_events.csv, tier CLK_ANCHOR, event cta_decouple  (the guard)
  COUNTER the run-end `N wire mark(s) queued`                      (the emitter queue)
  WIRE    products.txt R records                                    (what a consumer reads)

The WIRE is the only one a replay can see.  A mark that exists in the first two
and not the third is the card's own trap #1 verbatim — `markSatDiscontinuity`
alone is replay-invisible — so this script exists to make that failure loud
instead of inferred.

R rows carry the whole epoch's refloat set and are shared with the sibling
queues (eph_cutover / cb_excursion / basis_relatch), so the wire count is an
UPPER bound on Q3's own marks: the tie-out is "every (tow, sat) the guard
decoupled appears on the wire", not "the two totals are equal".

Usage: python scripts/q3_mark_tieout.py <run_dir> [run_dir ...]
"""
import os
import re
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def load_events(run):
    # The producer writes traces to the shared C:\GnssProcessor\logs and the arm
    # runner harvests them into <run>/logs; older arms were copied to the run root.
    # Look in both, newest-harvest first, or a present-but-elsewhere events file
    # reads as "the guard never fired" — the exact false negative this checks for.
    p = next((c for c in (os.path.join(run, "logs", "network_events.csv"),
                          os.path.join(run, "network_events.csv"))
              if os.path.exists(c)), None)
    ev, by_reason = [], {}
    if p is None:
        print(f"  !! {run}: no network_events.csv in <run> or <run>/logs — "
              f"cannot tie out; this is NOT evidence the guard was quiet")
        return ev, by_reason
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "cta_decouple" not in line:
                continue
            c = line.rstrip("\n").split(",")
            tow, sat = round(float(c[1])), c[4]
            m = re.search(r"reason=(\w+)", line)
            r = m.group(1) if m else "?"
            ev.append((tow, sat, r))
            by_reason[r] = by_reason.get(r, 0) + 1
    return ev, by_reason


def load_wire(run):
    """R rows keyed by the epoch of the preceding E record."""
    p = os.path.join(run, "products.txt")
    wire, tow = set(), None
    if not os.path.exists(p):
        return wire
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("E,"):
                tow = round(float(line.split(",")[2]))
            elif line.startswith("R,") and tow is not None:
                for s in line.rstrip("\n").split(",")[1:]:
                    if s:
                        wire.add((tow, s))
    return wire


def counter(run):
    p = os.path.join(run, "console.txt")
    if not os.path.exists(p):
        return None, None
    s = open(p, "rb").read().decode("utf-8", "replace")
    m = re.search(r"(\d+) re-coupling edge\(s\), (\d+) wire mark\(s\) queued", s)
    r = re.search(r"(\d+) in-epoch re-solve\(s\) \((\d+) failed\)", s)
    return (int(m.group(2)) if m else None), (int(r.group(1)) if r else None)


def main():
    for run in sys.argv[1:]:
        ev, by_reason = load_events(run)
        wire = load_wire(run)
        marks, resolves = counter(run)
        arm = os.path.basename(os.path.normpath(run))
        # Marks land in the SAME epoch as the refloat (SolPrep runs before EmitStore),
        # but a decouple detected on a LATER guard pass can be queued after that
        # epoch's emit — so accept the trip's own epoch or the next one.
        hit = [e for e in ev if (e[0], e[1]) in wire or (e[0] + 30, e[1]) in wire]
        miss = [e for e in ev if e not in hit]
        print(f"{arm}: events {len(ev)} {by_reason} | counter marks {marks} | "
              f"wire (tow,sat) rows {len(wire)} | ON THE WIRE {len(hit)}/{len(ev)}"
              f"{'' if not miss else '  MISSING ' + str(miss[:8])}")
        if ev and marks is not None and marks != len(ev):
            print(f"    note: counter {marks} != events {len(ev)} — expected only when "
                  f"clock_anchor_mark is off (then the counter is 0 by design)")
        if resolves is not None and ev:
            print(f"    in-epoch resolves {resolves} against {len(ev)} decoupling edge(s) "
                  f"(a resolve covers ALL trips of one pass, so resolves <= edges by design)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
