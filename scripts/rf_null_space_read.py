#!/usr/bin/env python3
"""RF-NS -- the S-basis null-space read.

Grades ONE network run directory for *why* the ``NetworkSbasis`` rank gate
withheld: it names every near-null direction by the shared parameters that
span it, classifies it into a family, and crosses each withheld epoch with the
events (held-basis handovers, arc births/deaths, column adds/removes,
readmissions, clock-anchor decouples) in its immediate neighbourhood.

Inputs, all under ``<run_dir>/logs/`` (missing files are skipped, not fatal):

  network_estimability.csv  the gate's eigen-census -- EPOCH rows carry
                            (n_shared, declared_dim, ev_max, cutoff, n_below),
                            DIR rows carry one eigenvalue plus up to eight
                            ``FAMILY|SAT|SIGNAL=weight`` labels.  The dump is
                            budgeted (``network.trace_estimability_budget``);
                            with the historical budget of 6 it covers ignition
                            only, which is exactly the gap this script's
                            ``--tow-min/--tow-max`` windowing exists to expose.
  network_epoch_status.csv  disposition per epoch (the withhold ground truth).
  network_events.csv        GATE sbc_skip / sbc_engage / phase_heal /
                            gate_withheld -- the per-epoch attribution that
                            exists on EVERY epoch, budget or no budget.
  network_phase_anchor.csv  the elected (sat,signal) -> anchor station map.
  network_amb_state.csv     per-arc carry rows incl. birth / drop / erase.

Usage:
    python scripts/rf_null_space_read.py runs/_rfns_field6_both
    python scripts/rf_null_space_read.py runs/_rfns_field6_both \
        --tow-min 274859 --tow-max 276299

No plotting, no network access, read-only.
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
from collections import Counter, defaultdict

EPOCH_S = 30.0

# --------------------------------------------------------------------------
# direction family classification
# --------------------------------------------------------------------------
# A census DIR row's col1..col8 are "FAMILY|SAT|SIGNAL=weight" (CLK has no
# SIGNAL).  The classifier below is deliberately structural -- it asks what the
# WEIGHTS say, not what we expect to find -- because the whole point of the read
# is to separate "a phase datum is momentarily unpinned" (a DECLARATION fix)
# from anything else (a rank read).
CLS_CB_ORPHAN = "CB-orphan"     # one code-bias column, essentially alone
CLS_CLK_FLAT = "CLK-flat"       # all-clock, weights ~ 1/sqrt(n_clk): the datum itself
CLS_SAT_LUMP = "sat-lump"       # ONE satellite's CLK + its own PB columns
CLS_PB_PAIR = "PB-pair"         # two satellites' PB columns (a handover pair)
CLS_PB_BROAD = "PB-broad"       # many satellites' PB, low weights
CLS_MIXED = "mixed"


def parse_cols(cols):
    """['PB|G13|L2W=0.5176', ...] -> [(family, sat, signal, weight)]."""
    out = []
    for c in cols:
        c = (c or "").strip()
        if not c or "=" not in c:
            continue
        label, _, w = c.rpartition("=")
        try:
            w = float(w)
        except ValueError:
            continue
        parts = label.split("|")
        fam = parts[0] if parts else ""
        sat = parts[1] if len(parts) > 1 else ""
        sig = parts[2] if len(parts) > 2 else ""
        out.append((fam, sat, sig, w))
    return out


def classify(entries):
    """Family label + the satellites/signals the direction lives on."""
    if not entries:
        return CLS_MIXED, [], []
    fams = Counter(f for f, _s, _g, _w in entries)
    sats = sorted({s for _f, s, _g, _w in entries if s})
    sigs = sorted({g for _f, _s, g, _w in entries if g})
    wmax = max(w for _f, _s, _g, w in entries)
    top = [e for e in entries if e[3] >= 0.9 * wmax]

    if fams.get("CB", 0) == len(entries) and wmax >= 0.9:
        return CLS_CB_ORPHAN, sats, sigs
    if fams.get("CLK", 0) == len(entries):
        # A perfectly flat clock direction has every weight equal; the label set
        # is then an arbitrary slice of the constellation.  wmax small + all-CLK
        # is the network clock datum (or an unseparable rotation of it).
        return (CLS_CLK_FLAT if wmax < 0.30 else CLS_SAT_LUMP), sats, sigs
    if len(sats) == 1:
        return CLS_SAT_LUMP, sats, sigs
    # one satellite dominates AND carries the CLK component -> still a sat lump
    dom = Counter(s for _f, s, _g, w in top if s)
    if len(dom) == 1 and any(f == "CLK" and s in dom for f, s, _g, _w in entries):
        return CLS_SAT_LUMP, sats, sigs
    if fams.get("PB", 0) >= len(entries) - 1:
        return (CLS_PB_PAIR if len(sats) <= 2 else CLS_PB_BROAD), sats, sigs
    return CLS_MIXED, sats, sigs


# --------------------------------------------------------------------------
# readers
# --------------------------------------------------------------------------
def read_csv_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        # network_amb_state.csv leads with a '#' provenance banner.
        lines = [ln for ln in fh if not ln.startswith("#")]
    return list(csv.DictReader(lines))


def read_census(path):
    """-> ([epoch dicts], run row dict or None)."""
    if not os.path.exists(path):
        return [], None
    epochs, cur, run = [], None, None
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        rdr = csv.reader(fh)
        header = next(rdr, None)
        for row in rdr:
            if not row:
                continue
            kind = row[0]
            if kind == "EPOCH":
                cur = {
                    "tow": float(row[2]), "n_shared": int(row[3]),
                    "declared": int(row[4]), "ev_max": float(row[5]),
                    "cutoff": float(row[6]), "n_below": int(row[7]), "dirs": [],
                }
                epochs.append(cur)
            elif kind == "DIR" and cur is not None:
                ev = float(row[8])
                ents = parse_cols(row[9:17])
                cls, sats, sigs = classify(ents)
                cur["dirs"].append({"ev": ev, "cls": cls, "sats": sats,
                                    "sigs": sigs, "ents": ents,
                                    "below": ev < cur["cutoff"]})
            elif kind == "RUN":
                run = dict(zip(header, row)) if header else None
    return epochs, run


def stretches(tows, gap=EPOCH_S * 1.5):
    out = []
    for t in sorted(tows):
        if out and t - out[-1][-1] <= gap:
            out[-1].append(t)
        else:
            out.append([t])
    return out


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------
def fmt_dir(d):
    ents = ";".join("%s%s%s=%.2f" % (f, "|" + s if s else "",
                                     "|" + g if g else "", w)
                    for f, s, g, w in d["ents"][:5])
    return "%12.4e  %-9s %s" % (d["ev"], d["cls"], ents)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--tow-min", type=float, default=None)
    ap.add_argument("--tow-max", type=float, default=None)
    ap.add_argument("--context-ep", type=int, default=2,
                    help="epochs of event context before each marked epoch (default 2)")
    a = ap.parse_args(argv)

    logs = os.path.join(a.run_dir, "logs")
    if not os.path.isdir(logs):
        print("no logs/ under %s" % a.run_dir, file=sys.stderr)
        return 2

    def inwin(t):
        return ((a.tow_min is None or t >= a.tow_min)
                and (a.tow_max is None or t <= a.tow_max))

    status = read_csv_rows(os.path.join(logs, "network_epoch_status.csv"))
    events = read_csv_rows(os.path.join(logs, "network_events.csv"))
    anchors = read_csv_rows(os.path.join(logs, "network_phase_anchor.csv"))
    ambs = read_csv_rows(os.path.join(logs, "network_amb_state.csv"))
    census, run_row = read_census(os.path.join(logs, "network_estimability.csv"))

    print("=" * 78)
    print("RF-NS null-space read  --  %s" % os.path.abspath(a.run_dir))
    if a.tow_min is not None or a.tow_max is not None:
        print("window: tow %s .. %s" % (a.tow_min, a.tow_max))
    print("=" * 78)

    # ---- 1. dispositions + withheld stretches ----------------------------
    disp = Counter(r["disposition"] for r in status)
    print("\n[1] EPOCHS  %d total   %s" % (len(status), dict(disp)))
    wtows = [float(r["tow"]) for r in status if r["disposition"] == "gate_withheld"]
    print("    withheld stretches (contiguous, %.0f s):" % EPOCH_S)
    for st in stretches(wtows):
        print("      %10.0f .. %-10.0f n=%-4d (%.1f min)"
              % (st[0], st[-1], len(st), (st[-1] - st[0]) / 60.0))

    # ---- 2. the census ----------------------------------------------------
    print("\n[2] EIGEN-CENSUS  %d dumped epoch(s)%s"
          % (len(census), "" if census else "  -- NONE (budget spent elsewhere)"))
    if run_row:
        print("    RUN row: %s" % {k: v for k, v in run_row.items()
                                   if v not in ("", None)})
    fam_tally = Counter()
    sat_tally = Counter()
    shown = 0
    for e in census:
        if not inwin(e["tow"]):
            continue
        shown += 1
        below = [d for d in e["dirs"] if d["below"]]
        print("\n    tow=%.0f  n_shared=%d  declared=%d  n_below=%d  "
              "ev_max=%.4e  cutoff=%.4e"
              % (e["tow"], e["n_shared"], e["declared"], e["n_below"],
                 e["ev_max"], e["cutoff"]))
        print("      %-12s  %-9s %s" % ("ev", "family", "top columns"))
        for d in e["dirs"]:
            mark = " " if d["below"] else "+"   # '+' = above the cutoff (context)
            print("     %s%s" % (mark, fmt_dir(d)))
        for d in below:
            fam_tally[d["cls"]] += 1
            for s in d["sats"]:
                sat_tally[s] += 1
    if fam_tally:
        print("\n    family tally over below-cutoff directions (%d epoch(s)): %s"
              % (shown, dict(fam_tally.most_common())))
        print("    satellites appearing:  %s"
              % ", ".join("%s(%d)" % (s, n) for s, n in sat_tally.most_common()))

    # ---- 3. per-epoch GATE attribution (exists on every epoch) -----------
    gate = defaultdict(list)
    for r in events:
        if r.get("tier") == "GATE":
            gate[float(r["tow"])].append(r)
    print("\n[3] GATE ATTRIBUTION per withheld epoch")
    print("    sbc_skip = a direction the S-basis completion could NOT separate")
    print("    phase_heal = a near-null PHASE-BIAS column soft-anchored "
          "(the heal sees neither CLK nor CB columns)")
    for st in stretches([t for t in wtows if inwin(t)]):
        print("\n    --- stretch %.0f .. %.0f  (n=%d) ---" % (st[0], st[-1], len(st)))
        print("      %-10s %-4s %-5s %s" % ("tow", "heal", "skip",
                                            "skipped direction(s)"))
        for t in st:
            rows = gate.get(t, [])
            sk = [r for r in rows if r["event"] == "sbc_skip"]
            hl = [r for r in rows if r["event"] == "phase_heal"]
            det = " | ".join(r["detail"] for r in sk) or "-"
            print("      %-10.0f %-4d %-5d %s" % (t, len(hl), len(sk), det[:110]))
        print("      recurring skipped directions in this stretch:")
        loc = Counter()
        for t in st:
            for r in gate.get(t, []):
                if r["event"] == "sbc_skip":
                    loc[r["detail"]] += 1
        for det, n in loc.most_common(8):
            print("        %4d x  %s" % (n, det[:100]))

    # ---- 4. co-incident events -------------------------------------------
    watch = ("PHASE_ANCHOR", "PBGAUGE", "STRANSFORM", "TOPOLOGY", "READMIT",
             "T0_gap", "CLK_ANCHOR", "T4_attrib", "T1_if_slip", "ADMISSION",
             "CB_DATUM", "CLK_BASIS", "PIVOT")
    byt = defaultdict(list)
    for r in events:
        if r.get("tier") in watch:
            byt[float(r["tow"])].append(r)
    print("\n[4] CO-INCIDENT EVENTS in (t - %d ep, t] at each stretch's FIRST epoch"
          " and at every epoch whose skipped direction CHANGED" % a.context_ep)
    for st in stretches([t for t in wtows if inwin(t)]):
        marks, prev = [st[0]], None
        for t in st:
            det = " | ".join(r["detail"] for r in gate.get(t, [])
                             if r["event"] == "sbc_skip")
            if prev is not None and det != prev:
                marks.append(t)
            prev = det
        print("\n    --- stretch %.0f .. %.0f ---" % (st[0], st[-1]))
        for m in marks:
            lo = m - a.context_ep * EPOCH_S
            rows = [r for t, rs in byt.items() if lo <= t <= m for r in rs]
            print("      at %.0f (%d event(s) in the window):" % (m, len(rows)))
            for r in sorted(rows, key=lambda r: (float(r["tow"]), r["tier"])):
                print("        %.0f %-13s %-22s %-8s %-12s %s"
                      % (float(r["tow"]), r["tier"], r["event"], r["sat"],
                         r["station"], (r["detail"] or "")[:60]))

    # ---- 5. phase-datum coverage -----------------------------------------
    if anchors:
        per = defaultdict(set)
        for r in anchors:
            per[float(r["tow"])].add((r["sat"], r["signal"]))
        ts = sorted(per)
        print("\n[5] PHASE-DATUM COVERAGE  (families with an elected anchor; "
              "rows exist only on EMITTED epochs)")
        print("    epochs with anchor rows: %d   first %.0f   last %.0f"
              % (len(ts), ts[0], ts[-1]))
        print("    families elected: min %d  max %d"
              % (min(len(v) for v in per.values()),
                 max(len(v) for v in per.values())))
        for st in stretches([t for t in wtows if inwin(t)]):
            before = [t for t in ts if t < st[0]]
            after = [t for t in ts if t > st[-1]]
            if not before or not after:
                continue
            b, af = per[before[-1]], per[after[0]]
            print("    stretch %.0f..%.0f: last emitted %.0f had %d families; "
                  "next emitted %.0f had %d" % (st[0], st[-1], before[-1], len(b),
                                                after[0], len(af)))
            lost, gained = sorted(b - af), sorted(af - b)
            if lost:
                print("      families LOST across the stretch (%d): %s"
                      % (len(lost), ", ".join("%s/%s" % x for x in lost[:14])))
            if gained:
                print("      families GAINED across the stretch (%d): %s"
                      % (len(gained), ", ".join("%s/%s" % x for x in gained[:14])))

    # ---- 6. arc births / deaths ------------------------------------------
    if ambs:
        print("\n[6] ARC EVENTS (network_amb_state.csv) around each stretch")
        ev_by_t = defaultdict(Counter)
        sat_by_t = defaultdict(lambda: defaultdict(Counter))
        for r in ambs:
            e = (r.get("event") or "").strip()
            if not e:
                continue
            t = float(r["tow"])
            ev_by_t[t][e] += 1
            sat_by_t[t][e][r["sat"]] += 1
        for st in stretches([t for t in wtows if inwin(t)]):
            lo, hi = st[0] - 3 * EPOCH_S, st[-1] + 3 * EPOCH_S
            agg = Counter()
            sats = defaultdict(Counter)
            for t, c in ev_by_t.items():
                if lo <= t <= hi:
                    agg.update(c)
                    for e, sc in sat_by_t[t].items():
                        sats[e].update(sc)
            print("    stretch %.0f..%.0f  %s" % (st[0], st[-1],
                                                  dict(agg.most_common(8))))
            for e in ("birth", "erase:gap_prune", "erase:anchor_reref",
                      "erase:t4_attrib"):
                if sats.get(e):
                    print("      %-20s top sats: %s"
                          % (e, ", ".join("%s(%d)" % kv
                                          for kv in sats[e].most_common(6))))
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
