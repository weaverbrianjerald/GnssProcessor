#!/usr/bin/env python3
"""Take the D5 innovation gate's standardized number APART.

`network_d5_flags.csv` has always carried `std_innov` = |v| / sqrt(Cov(v)_ii).  A
population sitting at a standing 76-230 sigma (measured 2026-08-02 on
`runs/ft63_24h_conv`, per-chord and FLAT for hours) says the formal covariance and
the data disagree by orders -- but the ratio alone cannot say WHICH factor is
responsible, and that determines the fix:

  * v large, var_v sane          -> a real physical event the coarse layer missed;
  * v small, var_v tiny          -> the formal Cov(v) is over-optimistic and the
                                    gate is a "large-innovation selective re-tap",
                                    not an N-sigma test (the D5 v2 finding);
  * v large AND var_v tiny       -> both, and they must be attacked separately.

The `zhat,v,var_v,sigma0,z_post` columns (added with this script) make the
question answerable per chord.  The decisive extra test here is the FRACTIONAL
PART of v/lambda: a cycle slip displaces the DD by an INTEGER number of cycles, so
if the re-tap magnitudes pile up near integers the population contains
integer-scale events regardless of what the MW/GF detector said, while a uniform
fractional part is the signature of continuous model tension.  That distinction is
the difference between "harden the slip detector" and "fix the ionosphere
representation", so it is worth measuring rather than assuming.

CAVEAT worth keeping in view: `zhat` and `z_post` are CORRECTION-frame values
(state units, about x_lin), so differencing z_post across epochs mixes the
estimate's real movement with any x_lin re-seed -- x_lin is a FRAME and reseeds
are exact solve no-ops (project_network_xlin_invariance).  `v` has no such
exposure: it is a residual, z_free - G'x_s - zhat, so every statistic below is
built from `v` and `var_v` and never from a z_post difference.

Usage:
    python scripts/grade_d5_algebra.py runs/<arm>/logs/network_d5_flags.csv
    python scripts/grade_d5_algebra.py runs/a/... runs/b/...      # compare arms
    python scripts/grade_d5_algebra.py <csv> --station WIND00NAM --top 20
"""
from __future__ import annotations

import argparse
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# Carrier wavelengths (m).  Keyed by the RINEX band digit in the signal code, which
# is what StateKeys::makeAmbiguityKey embeds ("G19_L1C" -> band '1').
_C = 299792458.0
_FREQ_MHZ = {
    "G1": 1575.42, "G2": 1227.60, "G5": 1176.45,
    "E1": 1575.42, "E5": 1176.45, "E6": 1278.75, "E7": 1207.140, "E8": 1191.795,
    "C1": 1575.42, "C2": 1561.098, "C5": 1176.45, "C6": 1268.52, "C7": 1207.140,
    "J1": 1575.42, "J2": 1227.60, "J5": 1176.45,
}


def wavelength(amb_key: str) -> float | None:
    """lambda for an ambiguity key like 'G19_L1C'.  None when unrecognised."""
    try:
        sat, sig = amb_key.split("_", 1)
    except ValueError:
        return None
    f = _FREQ_MHZ.get(sat[0] + sig[1:2])
    return None if f is None else _C / (f * 1.0e6)


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def load(path: Path):
    """Rows with the algebra columns present and finite."""
    rows = []
    with path.open(newline="") as fh:
        for r in csv.DictReader(fh):
            if not r.get("v") or not r.get("var_v"):
                continue          # pre-2026-08-02 file, or a row the driver had no term for
            try:
                r["v"] = float(r["v"])
                r["var_v"] = float(r["var_v"])
                r["std_innov"] = float(r["std_innov"])
                r["sigma0"] = float(r.get("sigma0") or 1.0)
                r["tow"] = float(r["tow"])
            except ValueError:
                continue
            if not (r["var_v"] > 0.0) or not math.isfinite(r["v"]):
                continue
            rows.append(r)
    return rows


def report(path: Path, station: str | None, top: int, born_sigma: float) -> None:
    rows = load(path)
    print(f"\n=== {path} ===")
    if not rows:
        print("  no rows with algebra columns — was the file written by an exe that has them,")
        print("  and was network.mw_wl.d5_dump armed?")
        return
    if station:
        rows = [r for r in rows if r["station"] == station]
        if not rows:
            print(f"  no rows for station {station}")
            return

    epochs = sorted({r["tow"] for r in rows})
    print(f"  {len(rows)} flag row(s) over {len(epochs)} flagged epoch(s), "
          f"{len({(r['station'], r['amb_key']) for r in rows})} distinct chord(s)")

    # --- Which factor is responsible? -------------------------------------
    sig_v = [math.sqrt(r["var_v"]) for r in rows]
    absv = [abs(r["v"]) for r in rows]
    print("  FACTORS (median over all flags)")
    print(f"    |v|            {med(absv):10.4f} m      <- the disagreement itself")
    print(f"    sqrt(var_v)    {med(sig_v):10.4f} m      <- the formal claim about it")
    print(f"    std_innov      {med([r['std_innov'] for r in rows]):10.2f} sigma")
    print(f"    sigma0         {med([r['sigma0'] for r in rows]):10.4f}        "
          f"<- empirical inflation already applied")

    # --- BORN vs CARRIED: the single most decisive split in this file ------
    # A chord born this epoch has the DIFFUSE prior (Config::fresh_prior_var, 1e6 by
    # default), so var_v lands on it almost exactly and |v| is just the correction
    # frame's distance from x_lin -- not a physical DD disagreement, and not a
    # hypothesis test, because there is no carried prior for H0 to be about.  The
    # threshold is a floor on sqrt(var_v), so it identifies the population without
    # needing the driver's chord_carried mask in the CSV.
    born = [r for r in rows if math.sqrt(r["var_v"]) >= born_sigma]
    carr = [r for r in rows if math.sqrt(r["var_v"]) < born_sigma]
    print(f"  BORN vs CARRIED  (born := sqrt(var_v) >= {born_sigma:g} m, the diffuse "
          f"fresh-chord prior)")
    for label, g in (("born   ", born), ("carried", carr)):
        if not g:
            print(f"    {label} {0:8d}    {0.0:6.2f} %")
            continue
        print(f"    {label} {len(g):8d}  {100.0*len(g)/len(rows):6.2f} %   "
              f"med|v| {med([abs(r['v']) for r in g]):12.3f} m   "
              f"med sqrt(var_v) {med([math.sqrt(r['var_v']) for r in g]):10.3f} m   "
              f"med std_innov {med([r['std_innov'] for r in g]):7.2f}")
    if born and len(born) >= 0.5 * len(rows):
        print("    => MOST OF THIS GATE IS NOT TESTING A HYPOTHESIS.  A born chord's reset is")
        print("       a no-op (its posterior already IS the data-only tap-off); the flag's only")
        print("       lasting effect is the funnel entry that re-borns it next epoch.  See")
        print("       network.carry_invariant_innov_carried_only.")

    # --- Integer-scale or not?  The fractional part of v/lambda. -----------
    fracs, cycs, unknown = [], [], 0
    for r in rows:
        lam = wavelength(r["amb_key"])
        if lam is None:
            unknown += 1
            continue
        c = r["v"] / lam
        cycs.append(abs(c))
        f = c - round(c)
        fracs.append(abs(f))
    if fracs:
        near = sum(1 for f in fracs if f <= 0.10)
        far = sum(1 for f in fracs if f >= 0.40)
        print("  INTEGER TEST on v/lambda "
              f"({len(fracs)} row(s){f', {unknown} unrecognised signal(s)' if unknown else ''})")
        print(f"    median |v| in cycles       {med(cycs):8.2f}")
        print(f"    median |frac(v/lambda)|    {med(fracs):8.3f}    "
              f"(0.25 = uniform, 0 = integer-scale)")
        print(f"    within 0.10 cyc of integer {100.0 * near / len(fracs):7.1f} %   "
              f"(20 % = chance)")
        print(f"    beyond 0.40 cyc            {100.0 * far / len(fracs):7.1f} %   "
              f"(20 % = chance)")
        # A one-line verdict, deliberately conservative about which way it points.
        if near >= 0.35 * len(fracs):
            print("    => a real INTEGER-SCALE sub-population is present; the re-taps are not")
            print("       all model tension, and the coarse detector is missing events.")
        elif med(fracs) > 0.20:
            print("    => fractional part is ~uniform: continuous model tension, not slips.")
            print("       Threshold tuning cannot fix this; the misfit source has to.")

    # --- Raw-step corroboration (tri-state, blank = no evidence) -----------
    tag = defaultdict(int)
    for r in rows:
        tag[(r.get("raw_stepped") or "").strip()] += 1
    n = len(rows)
    print("  RAW CORROBORATION (tri-state; blank = no evidence, never counted as 'flat')")
    print(f"    stepped              {tag['1']:8d}  {100.0*tag['1']/n:6.2f} %")
    print(f"    measured and flat    {tag['0']:8d}  {100.0*tag['0']/n:6.2f} %")
    print(f"    no evidence          {tag['']:8d}  {100.0*tag['']/n:6.2f} %")

    # --- Deweight-gate overlap -------------------------------------------
    dw = [float(r["dw_w"]) for r in rows if (r.get("dw_w") or "").strip()]
    if dw:
        n_dw = sum(1 for w in dw if w < 0.999)
        print(f"  SH DEWEIGHT  median w {med(dw):.4f}; "
              f"{100.0*n_dw/len(dw):.1f} % of flags are on a DEWEIGHTED station "
              f"({len(dw)} row(s) with a weight)")
    else:
        print("  SH DEWEIGHT  no dw_w column populated — arm network.trace_topology")

    # --- The chronic chords ----------------------------------------------
    by_chord = defaultdict(list)
    for r in rows:
        by_chord[(r["station"], r["amb_key"])].append(r)
    ranked = sorted(by_chord.items(), key=lambda kv: -len(kv[1]))[:top]
    print(f"  TOP {len(ranked)} CHORD(S) BY FLAG COUNT "
          f"(duty = flagged epochs / epochs in this file's span)")
    print(f"    {'station':<12} {'amb':<10} {'n':>5} {'duty':>6} {'med sig':>9} "
          f"{'med|v| m':>9} {'med sig_v':>10} {'med|frac|':>9}")
    span = max(1, len(epochs))
    for (sta, amb), rs in ranked:
        lam = wavelength(amb)
        fr = [abs(r["v"] / lam - round(r["v"] / lam)) for r in rs] if lam else []
        print(f"    {sta:<12} {amb:<10} {len(rs):>5} "
              f"{len({r['tow'] for r in rs})/span:>6.2f} "
              f"{med([r['std_innov'] for r in rs]):>9.1f} "
              f"{med([abs(r['v']) for r in rs]):>9.4f} "
              f"{med([math.sqrt(r['var_v']) for r in rs]):>10.4f} "
              f"{(med(fr) if fr else float('nan')):>9.3f}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", type=Path,
                    help="one or more network_d5_flags.csv (several = arm comparison)")
    ap.add_argument("--station", help="restrict to one station")
    ap.add_argument("--top", type=int, default=12, help="chronic-chord table length")
    ap.add_argument("--born-sigma", type=float, default=900.0,
                    help="sqrt(var_v) at or above which a row is treated as a BORN chord "
                         "(default 900 m; the driver's diffuse fresh_prior_var of 1e6 gives "
                         "exactly 1000 m, so anything near it is a fresh chord)")
    a = ap.parse_args()
    for p in a.csv:
        if not p.exists():
            print(f"missing {p}", file=sys.stderr)
            return 2
        report(p, a.station, a.top, a.born_sigma)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
