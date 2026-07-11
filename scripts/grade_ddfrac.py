import csv, math, cmath, sys, os, statistics as st
from collections import defaultdict

run, tail_s = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 3600.0
p = os.path.join(run, 'logs', 'network_dd_fractional.csv')
rows = [r for r in csv.DictReader(open(p)) if r.get('frac_cyc') not in (None, '')]
tmax = max(float(r['tow']) for r in rows)
tail = [r for r in rows if float(r['tow']) >= tmax - tail_s]
fr = [abs(float(r['frac_cyc'])) for r in tail]
n = len(fr)
R = abs(sum(cmath.exp(2j * math.pi * float(r['frac_cyc'])) for r in tail)) / n
sig = [float(r['dd_sigma_cyc']) for r in tail if float(r.get('dd_sigma_cyc', -1)) > 0]
# per-series epoch step + per-series R
series = defaultdict(list)
for r in tail:
    series[(r['cons'], r['signal'], r['sat_i'], r['ref_sat'], r['sta_a'], r['sta_b'])].append(
        (float(r['tow']), float(r['dd_cyc'])))
steps = []
for k, v in series.items():
    v.sort()
    steps += [abs(b[1] - a[1]) for a, b in zip(v, v[1:])]
med_step = st.median(steps) if steps else float('nan')
med_sig = st.median(sig) if sig else float('nan')
print('n=%d  mean|f|=%.3f  med|f|=%.3f  P(<0.1)=%.3f  circR=%.3f' %
      (n, sum(fr) / n, st.median(fr), sum(1 for x in fr if x < 0.1) / n, R))
print('formal dd_sigma med=%.3f cyc   DD step med=%.3f cyc/ep   calib(step/(sqrt2*sig))=%.2fx' %
      (med_sig, med_step, med_step / (math.sqrt(2) * med_sig) if sig and steps else float('nan')))
# T1a health trace: LOS>3sigma fraction
h = os.path.join(run, 'logs', 'network_vtec_sh_health.csv')
if os.path.exists(h):
    hr = [r for r in csv.DictReader(open(h)) if r.get('tow')]
    ht = [r for r in hr if float(r['tow']) >= tmax - tail_s]
    cols = ht[0].keys() if ht else []
    c3 = [c for c in cols if '3s' in c or '3sig' in c or 'over3' in c]
    cn = [c for c in cols if c.startswith('n_los') or c == 'n_rows' or c == 'nlos']
    if ht and c3:
        f3 = sum(float(r[c3[0]]) for r in ht)
        nn = sum(float(r[cn[0]]) for r in ht) if cn else len(ht)
        print('LOS>3sig frac (tail) = %.3f  [cols %s/%s]' % (f3 / max(nn, 1), c3[0], cn[0] if cn else '-'))
    else:
        print('health cols:', list(cols))
a = os.path.join(run, 'logs', 'network_vtec_sh.csv')
if os.path.exists(a):
    ar = [r for r in csv.DictReader(open(a)) if r.get('n') == '0' and r.get('m') == '0']
    at = [float(r['value_tecu']) for r in ar if float(r['tow']) >= tmax - tail_s] if ar and 'value_tecu' in ar[0] else []
    if at:
        print('a00 tail = %.1f +/- %.1f TECU' % (st.mean(at), st.stdev(at) if len(at) > 1 else 0))
