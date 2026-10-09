"""Card ARL: decompose the producer-AR float DD errors (per band, cycles) into wide-lane and
ionosphere-free / narrow-lane parts.  e1, e2: L1, L2 float errors against truth (with the
grader's wind-up branch).  WL = e1 - e2 (cycles of 86 cm).  IF error in metres =
(f1^2 l1 e1 - f2^2 l2 e2)/(f1^2 - f2^2); with the WL fixed, the N1 it implies is off by
IF_err / l_NL (l_NL = c/(f1+f2), 10.7 cm).  Iono direction: e1 - (f1/f2) e2... reported as the
geometry-free error in metres, l1 e1 - l2 e2.

A grading lens only: the producer stays uncombined.  Truth integers carry each arc's wind-up
branch (simnet_producer_ar_grade.load_windup_branches).

Usage: python scripts/simnet_producer_ar_lanes.py <fixture_dir> <run_dir> [settle_h=1]"""
import os, sys, io, csv, statistics as st, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_producer_ar_grade as g
from simnet_amb_integer import load_truth, n_at
import os
fx, run, settle = sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
f1, f2 = 1575.42e6, 1227.60e6; c = 299792458.0
l1, l2 = c / f1, c / f2; lnl = c / (f1 + f2)
stations = sorted(d for d in os.listdir(fx) if os.path.isfile(os.path.join(fx, d, 'truth_ambiguities.csv')))
n_int, slips = load_truth(fx, stations)
br, _ = g.load_windup_branches(fx, run, stations)
def N(sta, sat, sig, t): return n_at(n_int, slips, sta, sat, sig, t) + g.branch_at(br, sta, sat, t)
rows = collections.defaultdict(dict)
tow0 = None
with open(os.path.join(run, 'logs', 'network_producer_ar.csv')) as f:
    for r in csv.DictReader(f):
        t = float(r['tow']); tow0 = tow0 or t
        if t - tow0 < settle * 3600: continue
        k = (t, r['sat_j'], r['ref_sat'], r['sta_r'], r['ref_sta'])
        dd = (N(r['sta_r'], r['sat_j'], r['signal'], t) - N(r['sta_r'], r['ref_sat'], r['signal'], t)) - \
             (N(r['ref_sta'], r['sat_j'], r['signal'], t) - N(r['ref_sta'], r['ref_sat'], r['signal'], t))
        rows[k][r['signal']] = (float(r['float_cyc']) - dd, float(r['sigma_cyc']))
wl, nl, gf, e1s = [], [], [], []
for k, d in rows.items():
    if 'L1C' in d and 'L2W' in d:
        e1, e2 = d['L1C'][0], d['L2W'][0]
        wl.append(e1 - e2)
        nl.append((f1*f1*l1*e1 - f2*f2*l2*e2) / (f1*f1 - f2*f2) / lnl)
        gf.append(l1*e1 - l2*e2); e1s.append(e1)
def p(v, name, unit):
    a = sorted(abs(x) for x in v); n = len(a)
    print('  %-34s n %6d  |.| p50 %.3f  p90 %.3f  within 0.25 %5.1f%%  (%s)' % (name, n, a[n//2], a[int(.9*n)], 100*sum(x < .25 for x in a)/n, unit))
print(run, 'settled >= h%.0f' % settle)
p(e1s, 'L1 float error', 'cycles')
p(wl, 'wide-lane error (L1 - L2)', 'WL cycles, 86 cm')
p(nl, 'iono-free error / narrow-lane', 'NL cycles, 10.7 cm')
p(gf, 'geometry-free error l1e1 - l2e2', 'metres')
