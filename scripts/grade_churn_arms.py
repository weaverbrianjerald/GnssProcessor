#!/usr/bin/env python3
"""Scorecard for the D5 churn-card arms (docs/SPEC_d5_churn_limit_cycle.md).

Two of the card's four grading axes, side by side across arms:

  CHURN      -- n_chord_births / n_slip_refloats / n_d5_flags per epoch, from
                network_epoch_status.csv.  This is what the knobs act on.
  DD         -- med|frac_cyc|, median dd_sigma_cyc and the adjacent-epoch DD step,
                from network_dd_fractional.csv, over the LAST hour (the tail, so
                cold ignition is excluded -- never verdict inside ignition).
                This is whether acting on the churn bought anything.

The other two axes are deliberately NOT wrapped here, because each has an
existing owner whose methodology is the one on record and re-implementing it
would create a second, quietly different answer:

  python scripts/chain_stability.py --runroot runs/<arm>      # total_IF, the NO-HARM check
  python scripts/grade_ddfrac_perhour.py runs/<arm>           # per-hour DD convergence

Epoch-status columns are read BY NAME.  The positional indices around
n_arc_refloat(17) / n_d5_flags(18) / n_d5_retaps(19) / n_chord_births(20) /
n_slip_refloats(21) are off by one and reading them positionally has cost this
project two measurement-hygiene errors already.

Usage:
    python scripts/grade_churn_arms.py runs/churn_base runs/churn_v3 runs/churn_v4
    python scripts/grade_churn_arms.py runs/churn_* --tail-s 3600
"""
from __future__ import annotations

import argparse
import csv
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path


def _f(x):
    try:
        v = float(x)
        return v if v == v else None       # NaN out
    except (TypeError, ValueError):
        return None


def churn(run: Path) -> dict:
    p = run / "logs" / "network_epoch_status.csv"
    if not p.exists():
        return {"err": f"missing {p.name}"}
    cols = defaultdict(list)
    n = 0
    with p.open(newline="") as fh:
        for r in csv.DictReader(fh):
            n += 1
            for k in ("n_chord_births", "n_slip_refloats", "n_d5_flags",
                      "n_arc_refloat", "sigma0_pf_code"):
                v = _f(r.get(k))
                if v is not None:
                    cols[k].append(v)
    out = {"epochs": n}
    for k, xs in cols.items():
        out[k] = sum(xs) / len(xs) if xs else float("nan")
    return out


def dd_tail(run: Path, tail_s: float) -> dict:
    """med|frac|, median dd_sigma_cyc and the adjacent-epoch |dDD| over the tail.

    The DD step is formed per (cons, signal, sat_i, ref_sat, sta_a, sta_b) series
    between CONSECUTIVE epochs only -- a step across a gap is not a step, it is two
    different arcs, and averaging it in is how a churn metric flatters itself.
    """
    p = run / "logs" / "network_dd_fractional.csv"
    if not p.exists():
        return {"err": f"missing {p.name}"}
    rows = []
    with p.open(newline="") as fh:
        for r in csv.DictReader(fh):
            tow, fr = _f(r.get("tow")), _f(r.get("frac_cyc"))
            if tow is None or fr is None:
                continue
            rows.append(r)
    if not rows:
        return {"err": "no usable rows"}
    tows = sorted({_f(r["tow"]) for r in rows})
    t_end = tows[-1]
    dts = [b - a for a, b in zip(tows, tows[1:]) if b > a]
    dt = st.median(dts) if dts else 30.0
    keep = [r for r in rows if _f(r["tow"]) >= t_end - tail_s]

    frac = [abs(_f(r["frac_cyc"])) for r in keep]
    sig = [_f(r["dd_sigma_cyc"]) for r in keep if _f(r.get("dd_sigma_cyc")) is not None]

    series = defaultdict(dict)
    for r in keep:
        key = (r["cons"], r["signal"], r["sat_i"], r["ref_sat"], r["sta_a"], r["sta_b"])
        v = _f(r.get("dd_cyc"))
        if v is not None:
            series[key][_f(r["tow"])] = v
    steps = []
    for s in series.values():
        ts = sorted(s)
        for a, b in zip(ts, ts[1:]):
            if abs((b - a) - dt) < 1e-6:          # ADJACENT epochs only
                steps.append(abs(s[b] - s[a]))
    return {
        "tail_rows": len(keep),
        "med_abs_frac": st.median(frac) if frac else float("nan"),
        "med_dd_sigma_cyc": st.median(sig) if sig else float("nan"),
        "n_steps": len(steps),
        "med_dd_step_cyc": st.median(steps) if steps else float("nan"),
        "tail_from_tow": t_end - tail_s,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--tail-s", type=float, default=3600.0,
                    help="DD statistics window at the END of the run (default 1 h) — "
                         "cold ignition must not enter a convergence verdict")
    a = ap.parse_args()

    print(f"\nCHURN  (per-epoch means over the whole run)")
    print(f"  {'arm':<16} {'epochs':>7} {'births':>9} {'refloats':>9} "
          f"{'d5_flags':>9} {'arc_refl':>9} {'sig0_code':>10}")
    for run in a.runs:
        c = churn(run)
        if "err" in c:
            print(f"  {run.name:<16} {c['err']}")
            continue
        print(f"  {run.name:<16} {c['epochs']:>7} "
              f"{c.get('n_chord_births', float('nan')):>9.1f} "
              f"{c.get('n_slip_refloats', float('nan')):>9.1f} "
              f"{c.get('n_d5_flags', float('nan')):>9.1f} "
              f"{c.get('n_arc_refloat', float('nan')):>9.1f} "
              f"{c.get('sigma0_pf_code', float('nan')):>10.4f}")

    print(f"\nDD  (last {a.tail_s:.0f} s only)")
    print(f"  {'arm':<16} {'rows':>8} {'med|frac|':>10} {'dd_sigma_cyc':>13} "
          f"{'n_steps':>8} {'med|dDD| cyc/ep':>16}")
    for run in a.runs:
        d = dd_tail(run, a.tail_s)
        if "err" in d:
            print(f"  {run.name:<16} {d['err']}")
            continue
        print(f"  {run.name:<16} {d['tail_rows']:>8} {d['med_abs_frac']:>10.4f} "
              f"{d['med_dd_sigma_cyc']:>13.4f} {d['n_steps']:>8} "
              f"{d['med_dd_step_cyc']:>16.4f}")

    print("\n  med|frac| ~0.25 = UNIFORM = zero integer information (the pathology).")
    print("  dd_sigma_cyc pinned ~3.9 cyc is the symptom the churn card exists to move.")
    print("  NO-HARM check is NOT in this table — run scripts/chain_stability.py")
    print("  --runroot <arm> per arm and compare total_IF before calling anything a win.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
