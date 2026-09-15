#!/usr/bin/env python3
"""RF-PD - read the ``TOPOLOGY / sta_pd_dropped`` rows a run armed with
``network.trace_station_pd_null`` wrote into ``logs/network_events.csv``.

Each row names, for ONE station dropped by ``NetworkNormalEquations::addStation``'s
positive-definiteness test in ONE epoch, the null direction of its rejected local
block: the smallest eigenvalue, the near-zero eigenvalue COUNT, the pivot triple the
decision used, and the top-5 largest-|weight| columns of the null eigenvector as
StateManager labels (``AMB|G07|L1C=0.7071`` - the estimability DIR-row style).

Five reads, in the order card RF-PD asks for them:

  (1) the null-column FAMILY census (which parameter family the drop is about);
  (2) per station: count, dominant family, and whether the named top column
      PERSISTS across consecutive rejections (a standing structural null) or
      ROTATES (a birth / transient);
  (3) the always-rejected stations and their named columns;
  (4) time structure - rejections per epoch across the run, ignition vs settled;
  (5) the near-zero eigenvalue MULTIPLICITY distribution (one null per rejected
      station, or several).

Usage:  python scripts/rf_pd_null_read.py <run_dir> [<run_dir> ...] [--settled-tow T]
                                          [--top N] [--station STA]
No plotting, no dependencies outside the standard library.
"""
import argparse
import collections
import csv
import os
import sys

SETTLED_TOW_DEFAULT = 261660.0   # RF-NS-b settled bar on the 6 h field bed

FAMILY_NAME = {
    "AMB": "ambiguity (by signal)",
    "IONO": "per-LOS ionosphere",
    "CBIAS": "rx code bias / IFB (by signal)",
    "ZWD": "troposphere ZWD",
    "CLK": "receiver clock",
    "RPB": "rx phase bias",
    "SATB": "sat code bias (station copy)",
    "SATPB": "sat phase bias (station copy)",
    "SATCLK": "sat clock (station copy)",
    "IONORATE": "per-LOS iono rate",
    "ISB": "inter-system bias",
    "other": "unnamed / no label",
}


def parse_detail(detail):
    """``k=v;...;fam=F:m F:m;cols=LBL=w LBL=w`` -> (dict, cols, family-mass dict).

    ``cols`` is the top-5 slice; ``fam`` is the WHOLE null eigenvector's per-family
    mass sum w^2 (which sums to 1).  The top-5 is a thin slice of a null that is
    spread near-evenly over a whole band, so the family census is taken off ``fam``
    and the top-5 only names the individual columns.
    """
    kv, cols, fam = {}, [], {}
    for part in (detail or "").split(";"):
        if not part:
            continue
        if part.startswith("cols="):
            for tok in part[5:].split():
                lbl, _, w = tok.rpartition("=")
                if not lbl:
                    continue
                try:
                    cols.append((lbl, float(w)))
                except ValueError:
                    cols.append((lbl, float("nan")))
        elif part.startswith("fam="):
            for tok in part[4:].split():
                f, _, m = tok.rpartition(":")
                if not f:
                    continue
                try:
                    fam[f] = float(m)
                except ValueError:
                    pass
        else:
            k, _, v = part.partition("=")
            kv[k] = v
    return kv, cols, fam


def family_of(label):
    """``AMB|G07|L1C`` -> ``AMB``; a bare core state keeps its own name."""
    if not label or label == "?":
        return "other"
    head = label.split("|", 1)[0]
    return head if head else "other"


def load(run_dir):
    path = os.path.join(run_dir, "logs", "network_events.csv")
    if not os.path.isfile(path):
        sys.exit("no network_events.csv under %s" % run_dir)
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        for r in csv.DictReader(fh):
            if r.get("event") != "sta_pd_dropped":
                continue
            kv, cols, fam = parse_detail(r.get("detail", ""))
            rows.append({
                "tow": float(r["tow"]),
                "sta": r.get("station", ""),
                "action": r.get("action", ""),
                # The detail copy is the authority: the `statistic` column is written
                # {:.6f} by emitEvent, so a 1e-10 eigenvalue reads 0.000000 there.
                "ev_min": (float(kv["evmin"]) if "evmin" in kv
                           else (float(r["statistic"]) if r.get("statistic") else None)),
                "d_ref": float(r["threshold"]) if r.get("threshold") else None,
                "d_min": float(kv["dmin"]) if "dmin" in kv else None,
                "d_max": float(kv["dmax"]) if "dmax" in kv else None,
                "ev_max": float(kv["evmax"]) if "evmax" in kv else None,
                "n_eig": int(kv["neig"]) if "neig" in kv else None,
                "n_local": int(kv["nlocal"]) if "nlocal" in kv else None,
                "cols": cols,
                "fam": fam,
                "top": cols[0][0] if cols else "?",
            })
    rows.sort(key=lambda r: (r["tow"], r["sta"]))
    return rows


def report(run_dir, rows, settled_tow, topn, only_sta):
    name = os.path.basename(run_dir.rstrip("/\\"))
    epochs = sorted({r["tow"] for r in rows})
    print("=" * 78)
    print("RUN %s - %d sta_pd_dropped rows over %d distinct epochs, %d stations"
          % (name, len(rows), len(epochs), len({r["sta"] for r in rows})))
    if not rows:
        return
    unnamed = [r for r in rows if not r["cols"]]
    if unnamed:
        print("  !! %d row(s) carry NO null direction (a non-PD throw, or the eigen "
              "read was inert)" % len(unnamed))
    n = float(len(rows))

    # --- (1) family census -------------------------------------------------
    print("\n(1) NULL-COLUMN FAMILY CENSUS (top |weight| column of each rejection)")
    fam = collections.Counter(family_of(r["top"]) for r in rows)
    for f, c in fam.most_common():
        print("    %-8s %7d  %5.1f%%   %s" % (f, c, 100.0 * c / n, FAMILY_NAME.get(f, "")))
    fam_mass = collections.Counter()
    for r in rows:
        for f, m in r["fam"].items():
            fam_mass[f] += m
    if fam_mass:
        tot = sum(fam_mass.values()) or 1.0
        print("    -- FULL null-eigenvector mass by family (sum w^2 over EVERY column;")
        print("       each rejection contributes 1.0 — this is the census that does NOT")
        print("       depend on where the top-5 cut fell):")
        for f, m in fam_mass.most_common():
            print("       %-8s %6.2f%%   %s" % (f, 100.0 * m / tot, FAMILY_NAME.get(f, "")))
        # Per-rejection spread of the DOMINANT family's mass: a null CONFINED to one
        # family reads near 1.0; a mixed null reads well under it.
        dom = fam_mass.most_common(1)[0][0]
        v = sorted(r["fam"].get(dom, 0.0) for r in rows)
        print("       %s mass per rejection: min %.3f  p50 %.3f  max %.3f"
              % (dom, v[0], v[len(v) // 2], v[-1]))

    # --- (2) per station ---------------------------------------------------
    print("\n(2) PER STATION - count, dominant family, PERSIST vs ROTATE")
    by_sta = collections.defaultdict(list)
    for r in rows:
        by_sta[r["sta"]].append(r)
    print("    %-11s %6s %6s  %-6s %-6s %-24s %6s %7s"
          % ("station", "n_rej", "n_lbl", "family", "mode", "dominant column",
             "share", "consec"))
    for sta, rs in sorted(by_sta.items(), key=lambda kv: -len(kv[1])):
        if only_sta and sta != only_sta:
            continue
        f = collections.Counter(family_of(r["top"]) for r in rs).most_common(1)[0][0]
        tops = collections.Counter(r["top"] for r in rs)
        lbl, lblc = tops.most_common(1)[0]
        same = sum(1 for a, b in zip(rs, rs[1:]) if a["top"] == b["top"])
        frac = same / float(max(1, len(rs) - 1))
        mode = "PERSIST" if frac >= 0.8 else ("mixed" if frac >= 0.3 else "ROTATE")
        print("    %-11s %6d %6d  %-6s %-6s %-24s %5.1f%% %6.0f%%"
              % (sta, len(rs), len(tops), f, mode, lbl,
                 100.0 * lblc / len(rs), 100.0 * frac))

    # --- (3) the always-rejected stations ----------------------------------
    n_ep = len(epochs)
    always = [s for s, rs in by_sta.items() if len(rs) == n_ep]
    print("\n(3) STATIONS REJECTED ON EVERY EPOCH OF THE RUN (%d of %d, %d epochs)"
          % (len(always), len(by_sta), n_ep))
    for sta in sorted(always):
        rs = by_sta[sta]
        tops = collections.Counter(r["top"] for r in rs)
        print("    %-11s %s" % (sta, "  ".join("%s x%d" % (l, c)
                                               for l, c in tops.most_common(3))))

    # --- (4) time structure ------------------------------------------------
    print("\n(4) TIME STRUCTURE - rejections per epoch")
    per_ep = collections.Counter(r["tow"] for r in rows)

    def stat(sel):
        v = sorted(per_ep[e] for e in sel)
        return (len(v), v[len(v) // 2], v[0], v[-1]) if v else (0, 0, 0, 0)

    ign = [e for e in epochs if e < settled_tow]
    stl = [e for e in epochs if e >= settled_tow]
    for lab, sel in (("ignition (TOW < %.0f)" % settled_tow, ign),
                     ("settled  (TOW >= %.0f)" % settled_tow, stl),
                     ("whole run", epochs)):
        k, med, lo, hi = stat(sel)
        print("    %-26s epochs %4d   median %3d   min %3d   max %3d"
              % (lab, k, med, lo, hi))
    print("    hourly (epoch-median drops; dominant families that hour):")
    by_hr = collections.defaultdict(list)
    fam_hr = collections.defaultdict(collections.Counter)
    for r in rows:
        fam_hr[int((r["tow"] - epochs[0]) // 3600)][family_of(r["top"])] += 1
    for e in epochs:
        by_hr[int((e - epochs[0]) // 3600)].append(per_ep[e])
    for hr in sorted(by_hr):
        v = sorted(by_hr[hr])
        print("      h%-2d  epochs %3d  median %3d  min %3d  max %3d   %s"
              % (hr, len(v), v[len(v) // 2], v[0], v[-1],
                 " ".join("%s=%d" % kv for kv in fam_hr[hr].most_common(3))))

    # --- (5) eigenvalue multiplicity ---------------------------------------
    print("\n(5) NEAR-ZERO EIGENVALUE MULTIPLICITY (|lambda| < 1e-12 * lambda_max)")
    mult = collections.Counter(r["n_eig"] for r in rows if r["n_eig"] is not None)
    for k in sorted(mult):
        print("    n_eig = %-3d : %6d  (%5.1f%%)" % (k, mult[k], 100.0 * mult[k] / n))
    evs = sorted(r["ev_min"] for r in rows if r["ev_min"] is not None)
    if evs:
        neg = sum(1 for v in evs if v < 0)
        print("    ev_min  min %.3e  p50 %.3e  max %.3e   (negative => a genuine null)"
              % (evs[0], evs[len(evs) // 2], evs[-1]))
        print("    ev_min < 0 on %d of %d rows (%.1f%%)"
              % (neg, len(evs), 100.0 * neg / len(evs)))
    nl = sorted(r["n_local"] for r in rows if r["n_local"] is not None)
    if nl:
        print("    local block dim: min %d  p50 %d  max %d"
              % (nl[0], nl[len(nl) // 2], nl[-1]))
    print("    dispositions: "
          + "  ".join("%s=%d" % kv
                      for kv in collections.Counter(r["action"] for r in rows).most_common()))
    print("    -- top %d null COLUMNS run-wide --" % topn)
    for lbl, c in collections.Counter(r["top"] for r in rows).most_common(topn):
        print("       %-26s %6d" % (lbl, c))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="+")
    ap.add_argument("--settled-tow", type=float, default=SETTLED_TOW_DEFAULT)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--station", default=None)
    a = ap.parse_args()
    for rd in a.run_dir:
        report(rd, load(rd), a.settled_tow, a.top, a.station)


if __name__ == "__main__":
    main()
