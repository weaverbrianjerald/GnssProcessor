#!/usr/bin/env python3
"""Condition the DD fractional part on its FORMAL sigma — the payoff test of
``docs/SPEC_carry_aware_covariance.md``.

WHY THIS SCRIPT EXISTS
----------------------
``docs/RUNQUEUE_pdfix_reask.md`` P6(b) is the strongest negative result in the
whole DOY-129 campaign: six hours on a working fold, 55-epoch chord lifetimes,
and ``med|frac|`` dead flat at 0.246-0.254 — the uniform-distribution value — in
every hour and every age bin, reproduced on GPS+Galileo by P4.  It is recorded
as "the DD integer information is NOT PRESENT at this cadence/geometry".

**That conclusion is not decidable from ``med|frac|`` alone.**  Wrap a Gaussian
of sigma = 0.3 cyc around the unit circle and its fractional part is already
indistinguishable from uniform, whatever the underlying truth.  So
``med|frac| = 0.25`` separates

    (a) "no integer information exists"                        from
    (b) "integer information exists but this estimator's DD uncertainty
         is too large to expose it"

ONLY when read against a valid sigma_DD.  Until the carry-aware readback landed
there was no such sigma: ``dd_sigma_cyc`` came from a covariance that did not
contain the carry (it sat pinned near 3.9 cyc for the entire campaign, and
``scripts/grade_pdfix_arms.py`` excludes it ON PURPOSE).

This script does the conditioning.  It bins the DDs by ``dd_sigma_cyc`` and
reports ``med|frac|``, ``P(<0.1)`` and the circular resultant per bin.  On the
tight bins those statistics finally MEAN something:

* uniform on a bin where sigma << 0.25 cyc  =>  (a).  A genuine, quantified
  negative: the estimator could have resolved integer structure and there was
  none.
* pile-up appearing as sigma falls           =>  (b).  The six-hour null was a
  statement about the ESTIMATOR, not about the geometry, and the whole
  integer-information axis re-opens.

READ THE MODE COLUMN FIRST
--------------------------
``network_dd_fractional.csv`` carries ``sigma_carry_aware`` (0/1) precisely so
this script can refuse to draw the conclusion from a blind sigma.  A run whose
rows are all 0 gets its bins printed and its verdict WITHHELD — that is the
point of the card, and silently conditioning on a carry-blind sigma would be the
same class of mistake the parent card (SPEC_pd_safe_dd_fold.md section 11) was
written to end.  A file without the column at all is a pre-card exe: same
treatment.

Usage
-----
    python scripts/grade_ddfrac_sigma_conditioned.py <run_dir> [tail_seconds=3600]
    python scripts/grade_ddfrac_sigma_conditioned.py <run_a> <run_b> [tail_seconds]

With two run dirs it prints them side by side — the intended use is
``cacov_off`` vs ``cacov_on``, where the SOLVE is byte-identical by construction
so every difference in this table is attributable to the readback alone.
"""

from __future__ import annotations

import csv
import math
import statistics as st
import sys
from pathlib import Path

# sigma bins in cycles.  The 0.25 boundary is where a wrapped Gaussian stops
# being distinguishable from uniform, so it is the boundary the verdict turns on.
SIGMA_BINS = [
    ("sigma < 0.10", 0.0, 0.10),
    ("0.10-0.25", 0.10, 0.25),
    ("0.25-0.50", 0.25, 0.50),
    ("0.50-1.00", 0.50, 1.00),
    ("sigma > 1.00", 1.00, float("inf")),
]

UNIFORM_MED_ABS_FRAC = 0.25   # E|frac| for frac ~ U(-0.5, 0.5) is 0.25
UNIFORM_P_WITHIN_01 = 0.20    # P(|frac| < 0.1) under the same


def _f(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def load(run_dir: Path, tail_s: float):
    """Rows of the DD-fractional trace within the final ``tail_s`` seconds."""
    path = run_dir / "logs" / "network_dd_fractional.csv"
    if not path.is_file():
        raise SystemExit(f"missing {path} (enable network.trace_dd_fractional)")
    rows, has_mode, tows = [], False, []
    with path.open(newline="") as fh:
        rd = csv.DictReader(fh)
        has_mode = "sigma_carry_aware" in (rd.fieldnames or [])
        for r in rd:
            tow, sig, frac = _f(r.get("tow")), _f(r.get("dd_sigma_cyc")), _f(r.get("frac_cyc"))
            lam = _f(r.get("lambda_m"))
            if tow is None or sig is None or frac is None:
                continue
            if not lam:            # lambda 0 => the cycle columns are placeholders
                continue
            if sig < 0.0:          # -1 sentinel: the covariance was unavailable
                continue
            mode = _f(r.get("sigma_carry_aware")) if has_mode else None
            rows.append((tow, sig, abs(frac), frac, int(mode) if mode is not None else None))
            tows.append(tow)
    if not rows:
        raise SystemExit(f"{path}: no usable rows (all sigma sentinels / lambda 0?)")
    cutoff = max(tows) - tail_s
    kept = [r for r in rows if r[0] >= cutoff]
    return kept or rows, has_mode


def circ_r(fracs) -> float:
    """Circular resultant of 2*pi*frac: 0 = uniform, -> 1 = piled on integers."""
    if not fracs:
        return float("nan")
    c = sum(math.cos(2.0 * math.pi * f) for f in fracs) / len(fracs)
    s = sum(math.sin(2.0 * math.pi * f) for f in fracs) / len(fracs)
    return math.hypot(c, s)


def summarize(rows):
    """Per-sigma-bin statistics, plus the carry-aware-mode census."""
    out = []
    for label, lo, hi in SIGMA_BINS:
        sel = [r for r in rows if lo <= r[1] < hi]
        if not sel:
            out.append((label, 0, None, None, None, None))
            continue
        absf = [r[2] for r in sel]
        frac = [r[3] for r in sel]
        out.append((label, len(sel), st.median(absf),
                    sum(1 for a in absf if a < 0.1) / len(absf),
                    circ_r(frac), st.median([r[1] for r in sel])))
    return out


def mode_census(rows, has_mode):
    if not has_mode:
        return None
    aware = sum(1 for r in rows if r[4] == 1)
    return aware, len(rows)


def report(name: str, rows, has_mode):
    census = mode_census(rows, has_mode)
    print(f"\n=== {name} ===  {len(rows)} DD rows in window")
    if census is None:
        print("  sigma mode : COLUMN ABSENT — pre-card exe.  Treat every sigma below as")
        print("               CARRY-BLIND: it is the covariance of no estimator (the shared")
        print("               block saw the carry, the local block did not).")
    else:
        aware, tot = census
        pct = 100.0 * aware / tot if tot else 0.0
        state = ("CARRY-AWARE" if pct > 99.9 else
                 "carry-BLIND" if pct < 0.1 else f"MIXED ({pct:.1f}% aware)")
        print(f"  sigma mode : {state}  ({aware}/{tot} rows carry-aware)")
    print(f"  {'sigma bin':<14}{'n':>8}{'med sigma':>11}{'med|frac|':>11}"
          f"{'P(<0.1)':>10}{'circR':>9}")
    for label, n, medf, p01, cr, medsig in summarize(rows):
        if n == 0:
            print(f"  {label:<14}{0:>8}{'-':>11}{'-':>11}{'-':>10}{'-':>9}")
            continue
        print(f"  {label:<14}{n:>8}{medsig:>11.4f}{medf:>11.4f}{p01:>10.4f}{cr:>9.4f}")
    print(f"  {'uniform ref':<14}{'':>8}{'':>11}{UNIFORM_MED_ABS_FRAC:>11.4f}"
          f"{UNIFORM_P_WITHIN_01:>10.4f}{0.0:>9.4f}")
    return census


def verdict(rows, census):
    print("\n--- verdict ---")
    if census is None or census[0] == 0:
        print("  WITHHELD.  These sigmas are carry-blind, so a uniform fractional part is")
        print("  the EXPECTED reading regardless of the truth and this table cannot")
        print("  distinguish 'no integer information' from 'the estimator cannot see it'.")
        print("  Re-run with network.carry_aware_covariance = true.")
        return
    tight = [r for r in rows if r[1] < 0.25]
    if not tight:
        print("  WITHHELD.  No DD in the window has sigma < 0.25 cyc, so no bin is")
        print("  informative about integers even with an honest covariance.  That is")
        print("  itself a result: report the sigma distribution, not a frac verdict.")
        return
    absf = [r[2] for r in tight]
    med, cr = st.median(absf), circ_r([r[3] for r in tight])
    p01 = sum(1 for a in absf if a < 0.1) / len(absf)
    print(f"  {len(tight)} DD(s) with sigma < 0.25 cyc: med|frac| {med:.4f} "
          f"(uniform {UNIFORM_MED_ABS_FRAC}), P(<0.1) {p01:.4f} (uniform "
          f"{UNIFORM_P_WITHIN_01}), circR {cr:.4f}.")
    if med < 0.20 or cr > 0.15:
        print("  INTEGER STRUCTURE PRESENT on the resolvable population.  The six-hour")
        print("  P6(b) null was a statement about the ESTIMATOR, not the geometry —")
        print("  re-open the integer-information axis.")
    else:
        print("  STILL UNIFORM where sigma is small enough to have shown structure.  This")
        print("  is the QUANTIFIED form of P6(b)'s negative: the DDs the estimator can")
        print("  actually resolve carry no integer information either.")


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    tail = 3600.0
    if len(args) >= 2:
        try:
            tail = float(args[-1])
            args = args[:-1]
        except ValueError:
            pass
    for a in args:
        d = Path(a)
        rows, has_mode = load(d, tail)
        census = report(d.name, rows, has_mode)
        verdict(rows, census)
    if len(args) == 2:
        print("\nNOTE: with cacov_off vs cacov_on the SOLVE is byte-identical by")
        print("construction (both consumers are read-only), so every difference above is")
        print("attributable to the covariance READBACK and nothing else.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
