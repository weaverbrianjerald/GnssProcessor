"""Field-arm grader: closed-loop consumer ENU/3D + fleet ZWD span, settled window.

Built for the FIELD ZWD-in-block arm (SPEC_amb_iono_split_overcount.md 6.16.7d Arm F2),
where the graded quantities are

  * consumer  -- runs/<arm>/logs/closed_loop_trace.csv, columns
                 tow, pos_err_3d_m, east_m, north_m, up_m.  Reported settled
                 (tow >= t0 + settle_h*3600): 3D rms / p50, and per-component
                 mean / sd / rms.  The UP MEAN is the vertical DC bias -- the
                 quantity the in-block ZWD vehicle removes.
  * fleet ZWD -- runs/<arm>/logs/network_zwd.csv, columns tow, station, zwd_m.
                 Per-station SPAN = max - min over the settled window; the fleet
                 statistic is the median span across stations plus the counts over
                 2 m and 10 m (the pre-registered gates).
  * corr(UP, ZWD deviation of one station) -- Pearson r between the consumer's
                 up_m and a named station's ZWD minus its own settled mean, on the
                 epochs the two traces share.  This is the "producer ZWD sink
                 reaches the consumer through the wire" instrument
                 (project_zwd_sink_census_2026-09-09); positive/zero means the
                 sink is not steering the vertical.

t0 defaults to the first tow in the consumer trace, so a run needs no date knobs.

Usage:
  python scripts/field_consumer_zwd_grade.py runs/fldf3_on2 runs/fldf3_on2_zwdblk \
      --settle-h 6 --corr-station BSHM00ISR
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import statistics as st


def _rows(path):
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            yield row


def _f(row, key):
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return None


def _stats(vals):
    if not vals:
        return None
    n = len(vals)
    mean = sum(vals) / n
    rms = math.sqrt(sum(v * v for v in vals) / n)
    sd = st.pstdev(vals) if n > 1 else 0.0
    return {"n": n, "mean": mean, "sd": sd, "rms": rms, "p50": st.median(vals)}


def consumer(run, settle_h):
    """Settled consumer statistics; returns (stats dict, t0, settle_tow, up-by-tow map)."""
    path = os.path.join(run, "logs", "closed_loop_trace.csv")
    if not os.path.exists(path):
        return None, None, None, {}
    rows = [r for r in _rows(path) if _f(r, "tow") is not None]
    if not rows:
        return None, None, None, {}
    t0 = min(_f(r, "tow") for r in rows)
    settle = t0 + settle_h * 3600.0
    sett = [r for r in rows if _f(r, "tow") >= settle]
    out = {"n_all": len(rows), "n_settled": len(sett), "t0": t0, "settle_tow": settle,
           "tow_end": max(_f(r, "tow") for r in rows)}
    for key, label in (("pos_err_3d_m", "3d"), ("east_m", "east"),
                       ("north_m", "north"), ("up_m", "up")):
        vals = [_f(r, key) for r in sett]
        vals = [v for v in vals if v is not None]
        out[label] = _stats(vals)
    up_by_tow = {_f(r, "tow"): _f(r, "up_m") for r in sett if _f(r, "up_m") is not None}
    return out, t0, settle, up_by_tow


def zwd(run, settle_tow, corr_station):
    """Per-station settled ZWD span + the corr station's settled deviation series."""
    path = os.path.join(run, "logs", "network_zwd.csv")
    if not os.path.exists(path):
        return None, {}
    per = {}
    series = {}
    for r in _rows(path):
        tow = _f(r, "tow")
        z = _f(r, "zwd_m")
        if tow is None or z is None or tow < settle_tow:
            continue
        sta = r["station"].strip()
        per.setdefault(sta, []).append(z)
        if sta == corr_station:
            series[tow] = z
    if not per:
        return None, {}
    spans = sorted(((max(v) - min(v)), s) for s, v in per.items() if len(v) > 1)
    vals = [v for v, _ in spans]
    out = {
        "n_sta": len(spans),
        "p50": st.median(vals),
        "mean": sum(vals) / len(vals),
        "max": vals[-1],
        "over_2m": sum(1 for v in vals if v > 2.0),
        "over_10m": sum(1 for v in vals if v > 10.0),
        "worst5": [(s, round(v, 3)) for v, s in spans[-5:]][::-1],
        "corr_station_span": next((v for v, s in spans if s == corr_station), None),
    }
    if series:
        mu = sum(series.values()) / len(series)
        series = {t: z - mu for t, z in series.items()}
    return out, series


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return None
    return sxy / math.sqrt(sxx * syy)


def grade(run, settle_h, corr_station):
    print(f"=== {run} ===")
    c, t0, settle, up_by_tow = consumer(run, settle_h)
    if not c:
        print("  no closed_loop_trace.csv")
        return
    print(f"  window: t0 {t0:.0f}, settle >= {settle:.0f} (h{settle_h}), end {c['tow_end']:.0f}"
          f"  [{c['n_settled']}/{c['n_all']} epochs settled]")
    d = c["3d"]
    print(f"  consumer 3D   : rms {d['rms']:.3f}  p50 {d['p50']:.3f}  (n {d['n']})")
    for label in ("east", "north", "up"):
        s = c[label]
        if s:
            print(f"  consumer {label:<6}: mean {s['mean']:+.3f}  sd {s['sd']:.3f}  rms {s['rms']:.3f}")
    z, dev = zwd(run, settle, corr_station)
    if z:
        print(f"  fleet ZWD span ({z['n_sta']} sta): p50 {z['p50']:.3f} m  mean {z['mean']:.3f}"
              f"  max {z['max']:.3f}  n>2 m {z['over_2m']}  n>10 m {z['over_10m']}")
        print(f"    worst5: {z['worst5']}")
        if z["corr_station_span"] is not None:
            print(f"    {corr_station} settled span: {z['corr_station_span']:.3f} m")
    else:
        print("  no network_zwd.csv")
    if dev and up_by_tow:
        common = sorted(set(dev) & set(up_by_tow))
        if common:
            r = pearson([up_by_tow[t] for t in common], [dev[t] for t in common])
            print(f"  corr(UP, {corr_station} ZWD deviation): "
                  f"{'n/a' if r is None else f'{r:+.3f}'}  (n {len(common)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="+", help="run directory / directories")
    ap.add_argument("--settle-h", type=float, default=6.0,
                    help="hours after the first consumer epoch before grading (default 6)")
    ap.add_argument("--corr-station", default="BSHM00ISR",
                    help="station whose ZWD deviation is correlated against consumer UP")
    a = ap.parse_args()
    for run in a.run:
        grade(run, a.settle_h, a.corr_station)


if __name__ == "__main__":
    main()
