#!/usr/bin/env python3
"""Judge a brdc_rts armed run against RTF-1 Card 8 criteria 2 & 3 (Card 6 re-level).

Consumes two CSVs the run writes (network.trace_events + service_mode must be on):
  network_events.csv    week,tow,tier,event,sat,station,statistic,threshold,action,detail
  network_solution.csv  week,tow,sat,kind,signal,value_m,disc_counter   (kind 'C' = clock)

Card 8 criteria this checks:
  (2) At every ephemeris-cutover re-level (event 'eph_cutover_relevel', statistic = the
      applied shift = -Delta_eph in metres), the emitted clock C0 steps by ~= that shift so
      the reconstructed clock brdc(iode)+C0 stays continuous.  We check the emitted C0
      first-difference against the logged shift and flag residuals beyond --tol.
      NOTE: this is a PROXY.  Full absolute-clock reconstruction (evaluating the broadcast
      Kepler clock at the stamped IODE) needs the C++ LiveEphemerisStore; a residual near 0
      confirms the re-level absorbed the reference change with no extra transient, but the
      absolute continuity number is a Card-8 C++ tooling item.
  (3) A CLEAN re-level must NOT bump the clock disc counter.  We FAIL any sat whose
      disc_counter increments at a re-level epoch, and separately report 'eph_cutover_disc'
      forced-bump fallbacks (previous eph version evicted) — a large count there means the
      store's current/previous retention is not holding the old version.

Column mapping for network_events.csv is POSITIONAL (verified against emitEvent in
ApplicationNetwork.cpp).  If a future edit reorders those columns, fix EVENT_COLS below.
Sanity-check the first-run output; this script has not been run against live output.

Usage:
  python scripts/check_card6_relevel.py <network_events.csv> <network_solution.csv> [--tol 0.10]
"""
import argparse
import csv
import sys
from collections import defaultdict

EVENT_COLS = {"week": 0, "tow": 1, "tier": 2, "event": 3, "sat": 4,
              "station": 5, "statistic": 6, "threshold": 7, "action": 8, "detail": 9}
WEEK_S = 604800.0


def _t(week, tow):
    return float(week) * WEEK_S + float(tow)


def load_events(path):
    """Return (relevels, forced_disc). relevels=[(t,sat,shift)], forced_disc=[(t,sat)]."""
    relevels, forced_disc = [], []
    with open(path, newline="") as fh:
        for row in csv.reader(fh):
            if len(row) <= EVENT_COLS["statistic"]:
                continue
            if row[EVENT_COLS["week"]].strip().lower() == "week":
                continue  # header, if present
            ev = row[EVENT_COLS["event"]].strip()
            sat = row[EVENT_COLS["sat"]].strip()
            try:
                t = _t(row[EVENT_COLS["week"]], row[EVENT_COLS["tow"]])
            except ValueError:
                continue
            if ev == "eph_cutover_relevel":
                stat = row[EVENT_COLS["statistic"]].strip()
                try:
                    relevels.append((t, sat, float(stat)))
                except ValueError:
                    relevels.append((t, sat, float("nan")))
            elif ev in ("eph_cutover_disc", "eph_cutover_disc_landed"):
                forced_disc.append((t, sat))
    return relevels, forced_disc


def load_clock_series(path):
    """Return {sat: [(t, c0, disc), ...] sorted by t} from the kind='C' rows."""
    series = defaultdict(list)
    with open(path, newline="") as fh:
        rd = csv.DictReader(fh)
        need = {"week", "tow", "sat", "kind", "value_m", "disc_counter"}
        if not need.issubset(set(rd.fieldnames or [])):
            sys.exit(f"network_solution.csv missing columns {need - set(rd.fieldnames or [])}; "
                     f"got {rd.fieldnames}")
        for r in rd:
            if r["kind"] != "C":
                continue
            try:
                series[r["sat"]].append((_t(r["week"], r["tow"]),
                                         float(r["value_m"]), int(r["disc_counter"])))
            except (ValueError, TypeError):
                continue
    for s in series.values():
        s.sort(key=lambda x: x[0])
    return series


def nearest_idx(series, t, tol=0.6):
    """Index of the series sample nearest time t (within tol seconds), else None."""
    best, bd = None, tol
    for i, (ts, _, _) in enumerate(series):
        d = abs(ts - t)
        if d <= bd:
            best, bd = i, d
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events_csv")
    ap.add_argument("solution_csv")
    ap.add_argument("--tol", type=float, default=0.10,
                    help="metres: |dC0 - shift| above this at a re-level = flagged (default 0.10)")
    a = ap.parse_args()

    relevels, forced_disc = load_events(a.events_csv)
    series = load_clock_series(a.solution_csv)

    print(f"== Card 6 re-level judge ==")
    print(f"re-level events (eph_cutover_relevel): {len(relevels)}")
    print(f"forced-disc fallbacks (eph_cutover_disc[_landed]): {len(forced_disc)}")
    if not relevels and not forced_disc:
        print("NO eph-cutover activity in this run — the window contained no IODE roll that\n"
              "reached the emit, so Card 6's path was not exercised. Pick a roll-rich window\n"
              "(e.g. start 05:00 UTC on 2024-129) or a longer run.")
        return

    # --- Criterion 3: a clean re-level must not bump the disc counter ---
    c3_fail = []
    for t, sat, _shift in relevels:
        s = series.get(sat)
        if not s:
            continue
        i = nearest_idx(s, t)
        if i is None or i == 0:
            continue
        if s[i][2] != s[i - 1][2]:   # disc_counter changed across the re-level epoch
            c3_fail.append((sat, t, s[i - 1][2], s[i][2]))

    # --- Criterion 2 (proxy): emitted C0 step ~= logged shift ---
    c2_resid = []
    for t, sat, shift in relevels:
        s = series.get(sat)
        if not s or shift != shift:  # nan shift
            continue
        i = nearest_idx(s, t)
        if i is None or i == 0:
            continue
        dc0 = s[i][1] - s[i - 1][1]
        c2_resid.append((sat, t, dc0, shift, dc0 - shift))

    print("\n-- Criterion 3: re-level disc-neutrality --")
    if c3_fail:
        print(f"  FAIL: {len(c3_fail)} re-level(s) coincided with a clock disc bump "
              f"(a clean re-level must NOT bump):")
        for sat, t, a0, a1 in c3_fail[:20]:
            print(f"    {sat} @ t={t:.1f}  disc {a0} -> {a1}")
    else:
        print(f"  PASS: 0 of {len(relevels)} re-levels bumped the disc counter.")
    if forced_disc:
        by_sat = defaultdict(int)
        for _t, sat in forced_disc:
            by_sat[sat] += 1
        print(f"  note: {len(forced_disc)} forced-disc fallback(s) (previous eph version "
              f"evicted) across {len(by_sat)} sat(s). Large counts => current/previous "
              f"retention not holding the old version; investigate the store.")

    print("\n-- Criterion 2 (proxy): C0 step vs logged shift --")
    if c2_resid:
        resids = sorted(abs(r[4]) for r in c2_resid)
        flagged = [r for r in c2_resid if abs(r[4]) > a.tol]
        n = len(resids)
        print(f"  checked {n} re-levels; |dC0 - shift| median={resids[n // 2]:.4f} m "
              f"max={resids[-1]:.4f} m (drift over dt is included, so small nonzero is normal)")
        if flagged:
            print(f"  {len(flagged)} re-level(s) exceed --tol={a.tol} m (possible uncompensated "
                  f"transient — inspect):")
            for sat, t, dc0, shift, res in sorted(flagged, key=lambda x: -abs(x[4]))[:20]:
                print(f"    {sat} @ t={t:.1f}  dC0={dc0:+.4f}  shift={shift:+.4f}  resid={res:+.4f} m")
        else:
            print(f"  PASS: all within --tol={a.tol} m.")
    else:
        print("  (no re-level had a paired clock series to check)")

    verdict = "FAIL" if c3_fail else "PASS (criterion 3)"
    print(f"\n== verdict: {verdict}; criterion 2 is a proxy — confirm continuity on the "
          f"reconstructed clock for the flagged sats. ==")


if __name__ == "__main__":
    main()
