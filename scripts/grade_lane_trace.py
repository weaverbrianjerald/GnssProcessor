"""Grade the WL-2 lane trace (F-B instrument): injected/held-cycle circR vs control.

Input: logs/network_wl_lane_trace.csv from a run with network.mw_wl.lane_trace=true
(one row per primary cycle per epoch: post-install carried WL lane + frac + tags).

A chord joins the constrained population the first epoch it is held or injected and
stays there (the carry holds the lane between events). Everything else is control:
the same run, same epochs, same estimator, differing only in whether the WL cascade
touched the cycle. PASS criterion (SPEC_ewl_wl_cascade_card §7): constrained circR
integer-clustered much higher than control.

Usage: python grade_lane_trace.py <path/to/network_wl_lane_trace.csv>
"""
import csv, math, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else 'logs/network_wl_lane_trace.csv'
rows = list(csv.DictReader(open(path)))
if not rows:
    sys.exit('empty trace')

tows = sorted({float(r['tow']) for r in rows})
constrained = set()          # chords ever held/injected up to the current epoch
per_epoch = []               # (tow, stats_constrained, stats_control)

def stats(fracs):
    n = len(fracs)
    if n == 0:
        return (0, float('nan'), float('nan'), float('nan'))
    af = sorted(abs(x) for x in fracs)
    R = math.hypot(sum(math.cos(2 * math.pi * x) for x in fracs) / n,
                   sum(math.sin(2 * math.pi * x) for x in fracs) / n)
    return (n, af[n // 2], sum(1 for x in af if x < 0.15) / n, R)

by_tow = defaultdict(list)
for r in rows:
    by_tow[float(r['tow'])].append(r)

pooled_c, pooled_u = [], []
print(f"{'ep':>3} {'tow':>9} | constrained: n  med|f|  P<.15  circR | control: n  med|f|  P<.15  circR")
for i, t in enumerate(tows):
    for r in by_tow[t]:
        if r['held'] == '1' or r['injected'] == '1':
            constrained.add((r['chord_u'], r['chord_v']))
    fc = [float(r['frac_cyc']) for r in by_tow[t]
          if (r['chord_u'], r['chord_v']) in constrained]
    fu = [float(r['frac_cyc']) for r in by_tow[t]
          if (r['chord_u'], r['chord_v']) not in constrained]
    nc, mc, pc, Rc = stats(fc)
    nu, mu, pu, Ru = stats(fu)
    print(f"{i+1:>3} {t:>9.1f} | {nc:4d}  {mc:.3f}  {pc:.3f}  {Rc:.3f} "
          f"| {nu:5d}  {mu:.3f}  {pu:.3f}  {Ru:.3f}")
    if i >= len(tows) - 5:          # pool the last 5 epochs
        pooled_c += fc
        pooled_u += fu

nc, mc, pc, Rc = stats(pooled_c)
nu, mu, pu, Ru = stats(pooled_u)
print(f"\nPOOLED last 5 epochs — constrained: n={nc} med|f|={mc:.3f} P(<0.15)={pc:.3f} circR={Rc:.3f}")
print(f"POOLED last 5 epochs — control:     n={nu} med|f|={mu:.3f} P(<0.15)={pu:.3f} circR={Ru:.3f}")
print("uniform baseline: med|f|=0.250 P(<0.15)=0.300 circR=0.000")
