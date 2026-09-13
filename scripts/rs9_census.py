#!/usr/bin/env python
"""RS-9 census (read-only): the rx-CB withheld share and the withholding SET.

    python scripts/rs9_census.py <run> [<run> ...] [--lo 277320] [--hi 279090]
                                 [--ref <run>]

A generalisation of the RS-7b census script (§21.18(a)) to an arbitrary set of
runs, and the R1 instrument of card RS-9.  Everything is a read; nothing but
stdout is written.

Method, unchanged from RS-7b and validated there against the seed's own
403/3660 = 11.01 %:

  * the HEADLINE share comes from ``logs/network_prior_residual.csv``, family
    ``rxcb`` — RESPONDED (= innov_withhold) rows over all rows in the window.
    That is the instrument; nothing below may change the count.
  * the per-station IDENTITY is not in that CSV (it is aggregated per (epoch,
    family, label)), so it is recovered from ``logs/network_rx_states.csv``
    kind=CB: a FOLDED station's carry (var ~3e-5 m²) pins |CB(t) − CB(t−1)| to
    ~6 mm while a WITHHELD one is re-solved free (var ~1.3e-2 m²).  Per epoch
    the stations are ranked by |ΔCB| and the top n taken, with n the census's
    OWN RESPONDED count — so the classification cannot drift from the
    instrument it stands in for.  The separation ratio is printed as the
    proxy's own health check.

With ``--ref``, the withholding sets are also compared to that run's (the
seed), which is the R1 bar: the leg's set should BE the seed's ± 1.
"""
import csv
import os
import sys

ROOT = r"C:/GnssProcessor/runs"
PERSIST = 0.75   # a station is "withholding" when it withholds this share of its epochs


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    if len(s) == 1:
        return s[0]
    i = (len(s) - 1) * q / 100.0
    lo, hi = int(i), min(int(i) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (i - lo)


def read_prires(run, lo, hi, fam="rxcb"):
    """-> {tow: {label: n_rows}}"""
    out = {}
    p = os.path.join(ROOT, run, "logs", "network_prior_residual.csv")
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            if r["family"] != fam:
                continue
            t = int(round(float(r["tow"])))
            if lo <= t <= hi:
                out.setdefault(t, {})[r["label"]] = int(r["n_rows"])
    return out


def read_cb(run, lo, hi):
    """-> {station: {tow: value_abs}} for kind=CB."""
    out = {}
    p = os.path.join(ROOT, run, "logs", "network_rx_states.csv")
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] != "CB":
                continue
            t = int(round(float(r["tow"])))
            if lo <= t <= hi:
                out.setdefault(r["station"], {})[t] = float(r["value_m"])
    return out


def withhold_map(cb, tows, census, lo):
    """{station: set(tow withheld)} — top-n by |ΔCB|, n = the census's own count."""
    out = {s: set() for s in cb}
    lo_w, hi_f = [], []
    for i, t in enumerate(tows):
        if i == 0 or t < lo:
            continue
        tp = tows[i - 1]
        n = census.get(t, {}).get("RESPONDED", 0)
        steps = [(abs(cb[s][t] - cb[s][tp]), s) for s in cb
                 if t in cb[s] and tp in cb[s]]
        steps.sort(reverse=True)
        for _, s in steps[:n]:
            out[s].add(t)
        if n and len(steps) > n:
            lo_w.append(steps[n - 1][0])
            hi_f.append(steps[n][0])
    return out, (pct(lo_w, 50), pct(hi_f, 50))


def census_one(run, lo, hi):
    pr = read_prires(run, lo, hi)
    resp = sum(v.get("RESPONDED", 0) for v in pr.values())
    tot = sum(sum(v.values()) for v in pr.values())
    cb = read_cb(run, lo - 30, hi)
    tows = sorted({t for s in cb.values() for t in s})
    wm, sep = withhold_map(cb, tows, pr, lo)
    n_ep = len([t for t in tows if t >= lo])
    persistent = {s: len([t for t in w if t >= lo]) for s, w in wm.items()
                  if len([t for t in w if t >= lo]) >= PERSIST * n_ep}
    return dict(run=run, epochs=len(pr), responded=resp, total=tot,
                share=100.0 * resp / max(tot, 1), n_ep=n_ep, sep=sep,
                persistent=persistent, wm=wm)


def main():
    lo, hi, ref = 277320, 279090, None
    runs = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--lo":
            lo = int(args[i + 1]); i += 2
        elif a == "--hi":
            hi = int(args[i + 1]); i += 2
        elif a == "--ref":
            ref = args[i + 1]; runs.append(ref); i += 2
        else:
            runs.append(a); i += 1

    seen, ordered = set(), []
    for r in runs:
        if r not in seen:
            seen.add(r); ordered.append(r)

    print("# window tow [%d, %d]   (k = 1 + (tow - %d)/30)" % (lo, hi, lo))
    print("\n=== R1a  rx-CB withheld share (network_prior_residual.csv, family rxcb)")
    print("%-26s %5s %10s %8s %9s   %s" % ("run", "ep", "RESPONDED", "total",
                                           "share", "proxy separation (withheld/folded)"))
    res = {}
    for r in ordered:
        try:
            res[r] = census_one(r, lo, hi)
        except OSError as e:
            print("%-26s  MISSING: %s" % (r, e))
            continue
        d = res[r]
        print("%-26s %5d %10d %8d %8.2f%%   %.2e / %.2e (%.0fx)"
              % (r, d["epochs"], d["responded"], d["total"], d["share"],
                 d["sep"][0], d["sep"][1],
                 d["sep"][0] / max(d["sep"][1], 1e-12)))

    print("\n=== R1b  the WITHHOLDING SET (stations withholding >= %.0f %% of their %s epochs)"
          % (PERSIST * 100, "own"))
    for r in ordered:
        if r not in res:
            continue
        d = res[r]
        names = sorted(d["persistent"], key=lambda s: (-d["persistent"][s], s))
        print("%-26s n=%2d  %s" % (r, len(names),
                                   " ".join("%s(%d/%d)" % (s, d["persistent"][s], d["n_ep"])
                                            for s in names)))
    if ref and ref in res:
        base = set(res[ref]["persistent"])
        print("\n=== R1c  set difference against %s (n=%d)" % (ref, len(base)))
        for r in ordered:
            if r == ref or r not in res:
                continue
            cur = set(res[r]["persistent"])
            print("%-26s shared %2d | only-here %2d %s | only-ref %2d %s"
                  % (r, len(cur & base), len(cur - base), sorted(cur - base),
                     len(base - cur), sorted(base - cur)))


if __name__ == "__main__":
    main()
