#!/usr/bin/env python3
"""The wet-delay grade of card ZTL (docs/refound/ZTL_zwd_time_link_float_vehicle_2026-11-07.md).

usage: ztl_zwd_grade.py <run dir> [<run dir> ...] [--t0 TOW] [--hours N] [--truth FIXTURE_DIR]

Per run, by hour from --t0 (GPS seconds of week; default 259200, 00:00 of DOY 129; the sim arms
start at 00:30:15, --t0 261015):
  - the station wet delay (`zwd_m` of logs/network_zwd.csv): the epoch-to-epoch difference sd per
    station, median over stations; the formal sigma (`std_m`) p50;
  - with --truth (the sim fixture, runs/simnet60_quiet_col_24h): each station's `zwd_m` against
    <fixture>/<station>/truth_tropo.csv at the same epoch: rms, median |e|, NEES (mean of
    (e / std_m)^2);
  - the held-out consumer's 3D p50 (`pos_err_3d_m` of logs/closed_loop_trace.csv);
then, over hours 2 to N, the median of the per-station epoch-to-epoch sd and every step above 5 cm
(station, TOW, step), and the ZWD rows of logs/network_events.csv by event.

A station's epoch-to-epoch difference is taken only between consecutive epochs of the trace (one
epoch step apart), so an absence does not count as a step.
"""
import argparse
import collections
import csv
import math
import os
import statistics as st


def p50(v):
    return st.median(v) if v else float('nan')


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float('nan')


def read_zwd(rd):
    rows = collections.defaultdict(list)   # station -> [(tow, zwd, std)]
    with open(os.path.join(rd, 'logs', 'network_zwd.csv'), newline='') as f:
        for r in csv.DictReader(f):
            rows[r['station']].append((float(r['tow']), float(r['zwd_m']), float(r['std_m'])))
    for s in rows:
        rows[s].sort()
    return rows


def read_truth(fixture, station):
    p = os.path.join(fixture, station, 'truth_tropo.csv')
    if not os.path.exists(p):
        return {}
    out = {}
    with open(p, newline='') as f:
        for r in csv.DictReader(f):
            out[round(float(r['tow_s']), 1)] = float(r['zwd_m'])
    return out


def hour_of(tow, t0):
    return int((tow - t0) // 3600.0) + 1


def grade(rd, t0, nh, fixture):
    z = read_zwd(rd)
    dt = 30.0
    by_h = {h: {'d': collections.defaultdict(list), 'sd': [], 'e': [], 'nees': []} for h in range(1, nh + 1)}
    steps = []
    for s, rows in z.items():
        truth = read_truth(fixture, s) if fixture else {}
        for k, (tow, v, sd) in enumerate(rows):
            h = hour_of(tow, t0)
            if h < 1 or h > nh:
                continue
            by_h[h]['sd'].append(sd)
            if truth:
                tv = truth.get(round(tow, 1))
                if tv is not None:
                    e = v - tv
                    by_h[h]['e'].append(e)
                    if sd > 0:
                        by_h[h]['nees'].append((e / sd) ** 2)
            if k > 0 and abs(tow - rows[k - 1][0] - dt) < 1.0:
                d = v - rows[k - 1][1]
                by_h[h]['d'][s].append(d)
                if h >= 2 and abs(d) > 0.05:
                    steps.append((s, tow, d))
    cons = collections.defaultdict(list)
    cp = os.path.join(rd, 'logs', 'closed_loop_trace.csv')
    if os.path.exists(cp):
        with open(cp, newline='') as f:
            for r in csv.DictReader(f):
                h = hour_of(float(r['tow']), t0)
                if 1 <= h <= nh:
                    cons[h].append(float(r['pos_err_3d_m']))
    out = {'hours': {}, 'steps': steps}
    sd26 = collections.defaultdict(list)
    for h in range(1, nh + 1):
        b = by_h[h]
        per_sta = [st.pstdev(v) for v in b['d'].values() if len(v) >= 2]
        for s, v in b['d'].items():
            if h >= 2:
                sd26[s].extend(v)
        out['hours'][h] = {
            'e2e_sd_mm': 1e3 * p50(per_sta),
            'sigma_p50_mm': 1e3 * p50(b['sd']),
            'rms_mm': 1e3 * rms(b['e']) if b['e'] else None,
            'med_abs_mm': 1e3 * p50([abs(x) for x in b['e']]) if b['e'] else None,
            'nees': (sum(b['nees']) / len(b['nees'])) if b['nees'] else None,
            'cons_p50_m': p50(cons[h]) if cons[h] else None,
        }
    out['e2e_sd_h2_mm'] = 1e3 * p50([st.pstdev(v) for v in sd26.values() if len(v) >= 2])
    ev = collections.Counter()
    ep = os.path.join(rd, 'logs', 'network_events.csv')
    if os.path.exists(ep):
        with open(ep, newline='') as f:
            for r in csv.DictReader(f):
                if r['tier'] == 'ZWD':
                    key = r['event']
                    if r['event'] in ('recouple',):
                        cause = r['detail'].split(';')[0].replace('cause=', '')
                        key += ':' + cause
                    ev[key] += 1
                elif r['tier'] in ('T4_attrib', 'T1_if_slip'):
                    ev[r['tier']] += 1
    out['events'] = dict(sorted(ev.items()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--t0', type=float, default=259200.0)
    ap.add_argument('--hours', type=int, default=6)
    ap.add_argument('--truth', default=None)
    a = ap.parse_args()
    for rd in a.runs:
        g = grade(rd, a.t0, a.hours, a.truth)
        print(f'== {os.path.basename(rd.rstrip("/"))}')
        print('hour  e2e_sd_mm  sigma_p50_mm  rms_mm  med|e|_mm  NEES   consumer_p50_m')
        for h, r in g['hours'].items():
            f = lambda x, fmt: (fmt % x) if x is not None else '-'
            print(f"{h:>4}  {r['e2e_sd_mm']:9.2f}  {r['sigma_p50_mm']:12.2f}  {f(r['rms_mm'], '%6.2f'):>6}  "
                  f"{f(r['med_abs_mm'], '%9.2f'):>9}  {f(r['nees'], '%5.2f'):>5}  {f(r['cons_p50_m'], '%.4f'):>14}")
        print(f"e2e sd hours 2-{a.hours}, median over stations: {g['e2e_sd_h2_mm']:.2f} mm")
        print(f"steps > 5 cm in hours 2-{a.hours}: {len(g['steps'])}")
        for s, tow, d in sorted(g['steps'], key=lambda x: (x[1], x[0])):
            print(f'   {s} {tow:.1f} {d:+.3f} m')
        print('events:', g['events'])


if __name__ == '__main__':
    main()
