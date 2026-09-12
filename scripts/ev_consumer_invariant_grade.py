#!/usr/bin/env python3
"""The DATUM-INVARIANT consumer census (SPEC_event_harness §15.10).

Why it exists.  The storm arms of the cold anchor-fallback card were graded on the
closed-loop consumer's 3D position error, and that grade said the card cost the
user 49 %.  Decomposing the same numbers into ENU showed the settled mean EAST
error is +7.7 / +7.8 / +7.9 cm on the scope-OFF baseline and BOTH candidacy arms
and +3.8 cm on `cbscope5` — i.e. the reference arm is the outlier, and the pooled
median was reading a static realization of one station's coordinate, not the card.
Standing invariant #5 says exactly this about consumer position as a channel.

So this grades the arms on quantities a static position bias cannot enter:

  bias      the settled mean ENU and its magnitude — reported so the confound is
            visible rather than silently inside every other number.
  debias    |error − the arm's OWN settled mean ENU|, p50 and rms; and the same
            with a per-HOUR mean removed, so a slowly MOVING bias cannot hide
            inside the static removal (if the two disagree the bias is drifting).
  events    the consumer's own re-convergence work, from `logs/rt_engine.log`:
            SSR phase-bias resets, code-bias discontinuities, clock
            discontinuities — totals and by hour.  These are what a producer
            datum re-realization COSTS the user, and they are position-free.
  shocks    epochs after the first hour whose 3D error exceeds a threshold
            (default 0.5 m) — a bias of 8 cm cannot manufacture one.
  ar        the AR channel from `closed_loop_trace.csv` (ar_ran / ar_success /
            num_fixed / num_candidates / ratio).  On a float-only consumer this
            reports ZERO RUNS rather than a rate, which is the honest output.
  pbstep    the between-satellite emitted-bias consistency: per epoch, the step of
            every emitted (sat, signal) phase bias against the previous epoch, and
            the fraction of epochs carrying ANY |step| above a threshold.  Read
            from `products.txt`, so it is a property of the WIRE, not of any
            station's position.

    python scripts/ev_consumer_invariant_grade.py <run> [<run> ...]
        [--t0 302400] [--settle-h 3] [--shock-m 0.5] [--step-m 0.05] [--json out]
"""
import argparse
import csv
import json
import math
import os
import re
import statistics as st
import sys
from collections import defaultdict

EPOCH_S = 30.0


def p50(v):
    return st.median(v) if v else float("nan")


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")


def load_consumer(run, t0):
    """(hour, 3D, E, N, U, ar_ran, ar_success, n_fixed, n_cand, ratio) per epoch."""
    p = os.path.join(run, "logs", "closed_loop_trace.csv")
    out = []
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if r["pos_err_3d_m"] in ("", "nan"):
                continue
            out.append((
                (float(r["tow"]) - t0) / 3600.0,
                float(r["pos_err_3d_m"]), float(r["east_m"]),
                float(r["north_m"]), float(r["up_m"]),
                int(r.get("ar_ran") or 0), int(r.get("ar_success") or 0),
                int(r.get("num_fixed") or 0), int(r.get("num_candidates") or 0),
                float(r.get("ratio") or 0.0)))
    return out


_TOW = re.compile(r"^\[\s*(\d+),\s*([0-9.]+)")
_KIND = (("pb_reset", "SSR phase-bias discontinuity"),
         ("cb_disc",  "SSR code-bias discontinuity"),
         ("clk_disc", "SSR clock discontinuity"))


def load_events(run, t0):
    """kind -> hour -> count, from the consumer's own log."""
    p = os.path.join(run, "logs", "rt_engine.log")
    out = {k: defaultdict(int) for k, _ in _KIND}
    if not os.path.exists(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = _TOW.match(ln)
            if not m:
                continue
            h = int((float(m.group(2)) - t0) // 3600)
            for k, pat in _KIND:
                if pat in ln:
                    out[k][h] += 1
                    break
    return out


def pb_steps(run, t0, thresh):
    """Emitted phase-bias step series from products.txt.

    Returns (n_epochs, frac_epochs_with_any_big_step, p90 of the per-epoch max
    |step|, total big steps), where a step is the change of one emitted
    (sat, signal) bias between consecutive EMITTED epochs.  A CONTINUOUS handover
    is an S-transform and produces no step; a COLD one re-realizes the family and
    does.  Nothing here depends on a station position.
    """
    p = os.path.join(run, "products.txt")
    prev, cur, tow = {}, {}, None
    n_ep = n_big_ep = n_big = 0
    maxes = []
    def close():
        nonlocal prev, cur, n_ep, n_big_ep, n_big
        if tow is None:
            return
        n_ep += 1
        steps = [abs(v - prev[k]) for k, v in cur.items() if k in prev]
        if steps:
            m = max(steps)
            maxes.append(m)
            big = sum(1 for s in steps if s > thresh)
            if big:
                n_big_ep += 1
                n_big += big
        prev, cur = cur, {}
    with open(p, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            f = ln.rstrip("\n").split(",")
            if f[0] == "E" and len(f) > 2:
                close()
                tow = float(f[2])
            elif tow is not None and f[0] == "P" and len(f) > 3:
                try:
                    cur[(f[1], f[2])] = float(f[3])
                except ValueError:
                    pass
    close()
    maxes.sort()
    return {"epochs": n_ep,
            "frac_epochs_big": (n_big_ep / n_ep) if n_ep else float("nan"),
            "n_big_steps": n_big,
            "p90_max_step_m": maxes[int(0.9 * (len(maxes) - 1))] if maxes else float("nan")}


def grade(run, a):
    c = load_consumer(run, a.t0)
    settled = [x for x in c if x[0] >= a.settle_h]
    out = {"run": os.path.basename(run), "n_settled": len(settled)}

    # (1) the static bias, and its magnitude
    mE, mN, mU = (st.mean(x[i] for x in settled) for i in (2, 3, 4))
    out["bias_E"], out["bias_N"], out["bias_U"] = mE, mN, mU
    out["bias_mag"] = math.sqrt(mE * mE + mN * mN + mU * mU)
    out["raw_p50"] = p50([x[1] for x in settled])
    out["raw_rms"] = rms([x[1] for x in settled])

    # (2) de-biased, static and per-hour
    deb = [math.dist((x[2], x[3], x[4]), (mE, mN, mU)) for x in settled]
    out["debias_p50"], out["debias_rms"] = p50(deb), rms(deb)
    byh = defaultdict(list)
    for x in settled:
        byh[int(x[0])].append(x)
    debh = []
    for h, rows in byh.items():
        hE, hN, hU = (st.mean(r[i] for r in rows) for i in (2, 3, 4))
        debh += [math.dist((r[2], r[3], r[4]), (hE, hN, hU)) for r in rows]
    out["debias_hourly_p50"], out["debias_hourly_rms"] = p50(debh), rms(debh)
    out["hourly"] = {}
    for h in sorted({int(x[0]) for x in c}):
        rows = [x for x in c if int(x[0]) == h]
        hE, hN, hU = (st.mean(r[i] for r in rows) for i in (2, 3, 4))
        d = [math.dist((r[2], r[3], r[4]), (hE, hN, hU)) for r in rows]
        out["hourly"][h] = {"raw_p50": p50([r[1] for r in rows]),
                            "bias": (hE, hN, hU), "debias_p50": p50(d)}

    # (3) the consumer's own re-convergence work + shocks
    ev = load_events(run, a.t0)
    for k, _ in _KIND:
        out[k] = sum(ev[k].values())
        out[k + "_hourly"] = [ev[k].get(h, 0) for h in range(6)]
    out["shock_epochs"] = sum(1 for x in c if x[0] >= 1.0 and x[1] > a.shock_m)
    out["shock_hourly"] = [sum(1 for x in c if int(x[0]) == h and x[1] > a.shock_m)
                           for h in range(6)]

    # (4) the AR channel
    ran = sum(x[5] for x in c)
    out["ar_ran"] = ran
    out["ar_success"] = sum(x[6] for x in c)
    out["ar_n_fixed"] = sum(x[7] for x in c)
    out["ar_n_cand"] = sum(x[8] for x in c)
    out["ar_ratio_p50"] = p50([x[9] for x in c if x[5]]) if ran else None

    # (5) the emitted-bias step instrument
    out["pbstep"] = pb_steps(run, a.t0, a.step_m)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--t0", type=float, default=302400.0)
    ap.add_argument("--settle-h", type=float, default=3.0)
    ap.add_argument("--shock-m", type=float, default=0.5)
    ap.add_argument("--step-m", type=float, default=0.05)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    rows = [grade(r, a) for r in a.runs]
    n = lambda s: s[-26:]
    print(f"settled window h >= {a.settle_h}; shock > {a.shock_m} m after h1; "
          f"emitted-step threshold {a.step_m} m\n")
    print(f"{'arm':<26} {'E':>8} {'N':>8} {'U':>8} {'|bias|':>8} | "
          f"{'raw p50':>8} {'raw rms':>8} | {'deb p50':>8} {'deb rms':>8} | "
          f"{'debH p50':>9} {'debH rms':>9}")
    for r in rows:
        print(f"{n(r['run']):<26} {r['bias_E']:+8.4f} {r['bias_N']:+8.4f} "
              f"{r['bias_U']:+8.4f} {r['bias_mag']:8.4f} | {r['raw_p50']:8.4f} "
              f"{r['raw_rms']:8.4f} | {r['debias_p50']:8.4f} {r['debias_rms']:8.4f} | "
              f"{r['debias_hourly_p50']:9.4f} {r['debias_hourly_rms']:9.4f}")

    print(f"\n{'arm':<26} {'pb_reset':>9} {'cb_disc':>8} {'clk_disc':>9} "
          f"{'shocks':>7} | {'ar_ran':>7} {'ar_ok':>6} {'fixed':>6} | "
          f"{'wire ep':>8} {'frac>thr':>9} {'p90 max':>8}")
    for r in rows:
        s = r["pbstep"]
        print(f"{n(r['run']):<26} {r['pb_reset']:9d} {r['cb_disc']:8d} "
              f"{r['clk_disc']:9d} {r['shock_epochs']:7d} | {r['ar_ran']:7d} "
              f"{r['ar_success']:6d} {r['ar_n_fixed']:6d} | {s['epochs']:8d} "
              f"{s['frac_epochs_big']:9.4f} {s['p90_max_step_m']:8.4f}")

    print("\nhourly (raw p50 / de-biased-per-hour p50 / pb_reset / shocks):")
    for r in rows:
        cells = []
        for h in range(6):
            hh = r["hourly"].get(h)
            cells.append(f"{hh['raw_p50']:.3f}/{hh['debias_p50']:.3f}/"
                         f"{r['pb_reset_hourly'][h]}/{r['shock_hourly'][h]}"
                         if hh else "-")
        print(f"  {n(r['run']):<26} " + "  ".join(cells))

    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=2)
        print(f"\nwrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
