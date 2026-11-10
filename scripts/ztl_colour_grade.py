#!/usr/bin/env python3
"""Card LGM's gate G1 read on a sim run with the phase colour state (card ZTL section 6.4).

usage: ztl_colour_grade.py <run dir> [<run dir> ...] [--fixture DIR] [--t0 TOW] [--min-age-s 600]

Joins logs/network_phase_colour.csv (week,tow,station,sat,elev_deg,x_m,sd_m,carried) with each
station's truth_phase_colour.csv (week,tow_s,satellite,e_m,u) of the sim fixture at the same epoch
and line of sight.  Rows from --t0 on whose line of sight has been traced without a break for at
least --min-age-s (default 600 s: card LGM's "arcs older than 10 minutes") are kept.  By latitude
class (the fixture's colour amplitude: 6.0 mm tropical, 4.9 mm mid, 4.4 mm high): the ratio
rms(x - e) / rms(e) (card LGM G1: at most 0.7 passes), the correlation of x with e, the rms of x
and of e (mm), and the NEES of x against e with sd_m.
"""
import argparse
import collections
import csv
import json
import math
import os

CLASS = {0.0060: 'tropical', 0.0049: 'mid', 0.0044: 'high'}


def station_class(fixture, station):
    p = os.path.join(fixture, station, 'scenario_summary.json')
    if not os.path.exists(p):
        return None
    s = json.load(open(p))['phase_colour']['sigma_zen_m']
    return min(CLASS.items(), key=lambda kv: abs(kv[0] - s))[1]


def truth(fixture, station):
    out = {}
    with open(os.path.join(fixture, station, 'truth_phase_colour.csv'), newline='') as f:
        for r in csv.DictReader(f):
            out[(round(float(r['tow_s']), 1), r['satellite'])] = float(r['e_m'])
    return out


def grade(rd, fixture, t0, min_age):
    rows = collections.defaultdict(list)   # (station, sat) -> [(tow, x, sd)]
    with open(os.path.join(rd, 'logs', 'network_phase_colour.csv'), newline='') as f:
        for r in csv.DictReader(f):
            rows[(r['station'], r['sat'])].append((float(r['tow']), float(r['x_m']), float(r['sd_m'])))
    acc = collections.defaultdict(lambda: {'x': [], 'e': [], 'nees': []})
    tcache, ccache = {}, {}
    for (sta, sat), rs in rows.items():
        if sta not in tcache:
            tcache[sta] = truth(fixture, sta) if os.path.exists(os.path.join(fixture, sta)) else {}
            ccache[sta] = station_class(fixture, sta)
        tr, cl = tcache[sta], ccache[sta]
        if not tr or cl is None:
            continue
        rs.sort()
        start = None
        prev = None
        for tow, x, sd in rs:
            if prev is None or tow - prev > 45.0:
                start = tow
            prev = tow
            if tow < t0 or tow - start < min_age:
                continue
            e = tr.get((round(tow, 1), sat))
            if e is None:
                continue
            a = acc[cl]
            a['x'].append(x)
            a['e'].append(e)
            if sd > 0:
                a['nees'].append(((x - e) / sd) ** 2)
    out = {}
    for cl in ('tropical', 'mid', 'high'):
        a = acc[cl]
        n = len(a['x'])
        if n == 0:
            continue
        rx = math.sqrt(sum(v * v for v in a['x']) / n)
        re = math.sqrt(sum(v * v for v in a['e']) / n)
        rd_ = math.sqrt(sum((x - e) ** 2 for x, e in zip(a['x'], a['e'])) / n)
        mx, me = sum(a['x']) / n, sum(a['e']) / n
        cov = sum((x - mx) * (e - me) for x, e in zip(a['x'], a['e'])) / n
        sx = math.sqrt(sum((x - mx) ** 2 for x in a['x']) / n)
        se = math.sqrt(sum((e - me) ** 2 for e in a['e']) / n)
        out[cl] = {'n': n, 'ratio': rd_ / re, 'corr': cov / (sx * se) if sx > 0 and se > 0 else float('nan'),
                   'rms_x_mm': 1e3 * rx, 'rms_e_mm': 1e3 * re,
                   'nees': sum(a['nees']) / len(a['nees']) if a['nees'] else float('nan')}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--fixture', default='C:/GnssProcessor/runs/simnet60_quiet_col_24h')
    ap.add_argument('--t0', type=float, default=261015.0)
    ap.add_argument('--min-age-s', type=float, default=600.0)
    a = ap.parse_args()
    for rd in a.runs:
        print(f'== {os.path.basename(rd.rstrip("/"))}')
        print('class      n       ratio  corr   rms_x_mm  rms_e_mm  NEES')
        for cl, r in grade(rd, a.fixture, a.t0, a.min_age_s).items():
            print(f"{cl:9} {r['n']:7d}  {r['ratio']:.3f}  {r['corr']:+.2f}  {r['rms_x_mm']:8.2f}  "
                  f"{r['rms_e_mm']:8.2f}  {r['nees']:.2f}")


if __name__ == '__main__':
    main()
