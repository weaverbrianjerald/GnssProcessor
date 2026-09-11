#!/usr/bin/env python3
"""EVENT HARNESS — census of 'MW slip detected' lines in mw_slip_detector*.log: lane wavelength, arc age,
evidence ratio |dev|/thresh, integer known?, rounding fraction.  Prices candidate refloat gates.
Usage: mw_detect_census.py <logdir> [--list]"""
import sys, os, re, glob, collections
logdir = sys.argv[1]; do_list = '--list' in sys.argv
def rot(p):
    m = re.search(r'\.(\d+)\.log$', p); return -int(m.group(1)) if m else 0
files = sorted(glob.glob(os.path.join(logdir, 'mw_slip_detector*.log')), key=rot)
rx = re.compile(r"MW slip detected for (\S+) \((\w+)\): \|dev\|=([\d.]+) m \(([\d.]+) WL\) > thresh=([\d.]+) m .*?sigma=(-?[\d.]+) m, arc_n=(\d+), lambda_wl=([\d.]+) m, bia=(\w+), locked=(\w+), delta_wl=(\S+)")
rxf = re.compile(r"WL slip estimated from jump for (\S+): dN_WL=(-?\d+), dev_cyc=(-?[\d.]+), frac=([\d.]+)")
rows = []; last_frac = {}
for f in files:
    for line in open(f, encoding='utf-8', errors='replace'):
        m = rxf.search(line)
        if m:
            last_frac[m.group(1).split('_')[0]] = float(m.group(4)); continue
        m = rx.search(line)
        if not m: continue
        sat, cons, dev, devwl, thr, sig, n, lam, bia, locked, dwl = m.groups()
        known = dwl != 'n/a'
        frac = last_frac.pop(sat, None) if known else None
        thr = float(thr)
        rows.append(dict(idx=len(rows), sat=sat, cons=cons, dev=float(dev), devwl=float(devwl), thr=thr,
                         ratio=float(dev) / thr if thr > 0 else 0.0, sigma=float(sig), n=int(n),
                         lam=float(lam), bia=bia, known=known, frac=frac))
print(f"{len(rows)} detections in {len(files)} file(s)")
LANES = {0.751: 'G(1,5)', 5.861: 'G(2,5)', 0.862: 'G(1,2)', 0.814: 'E(1,7)', 9.768: 'E(5,7)'}
def lane(r):
    for k, v in LANES.items():
        if abs(r['lam'] - k) < 0.01: return v
    return f"{r['cons'][0]}lam{r['lam']:.2f}"
def gate_today(r): return r['known'] and r['frac'] is not None and r['frac'] <= 0.15
def bucket_n(n): return 'n<=2' if n <= 2 else ('n3-9' if n < 10 else 'n>=10')
g = collections.defaultdict(collections.Counter)
for r in rows:
    g[lane(r)][(bucket_n(r['n']), 'known' if r['known'] else 'unknown', 'ACCEPT' if gate_today(r) else 'drop')] += 1
for ln in sorted(g):
    print(f"\n== lane {ln}  total {sum(g[ln].values())}")
    for k, v in sorted(g[ln].items()): print(f"   {k[0]:6s} {k[1]:8s} {k[2]:7s} {v}")
print("\n== evidence ratio |dev|/thresh, by maturity (all lanes)")
for b in ('n<=2', 'n3-9', 'n>=10'):
    rs = sorted(r['ratio'] for r in rows if bucket_n(r['n']) == b)
    if not rs: continue
    q = lambda p: rs[min(len(rs) - 1, int(p * len(rs)))]
    print(f"   {b:6s} n={len(rs):4d}  p50 {q(.5):6.2f}  p90 {q(.9):6.2f}  p99 {q(.99):7.2f}  max {rs[-1]:8.2f}   >=3: {sum(x >= 3 for x in rs)}  >=5: {sum(x >= 5 for x in rs)}  >=10: {sum(x >= 10 for x in rs)}")
print("\n== candidate gates (detections each would REFLOAT):")
for name, fn in [('today: known & frac<=0.15', gate_today),
                 ('known (any frac)', lambda r: r['known']),
                 ('ratio>=3 & n>=5', lambda r: r['ratio'] >= 3 and r['n'] >= 5),
                 ('ratio>=3 & n>=10', lambda r: r['ratio'] >= 3 and r['n'] >= 10),
                 ('ratio>=5 & n>=10', lambda r: r['ratio'] >= 5 and r['n'] >= 10),
                 ('today OR (ratio>=3 & n>=10)', lambda r: gate_today(r) or (r['ratio'] >= 3 and r['n'] >= 10))]:
    print(f"   {name:32s} {sum(fn(r) for r in rows):5d}")
if do_list:
    print(f"\n== mature (n>=5) detections by ratio (idx = order in log)")
    for r in sorted([r for r in rows if r['n'] >= 5], key=lambda r: -r['ratio']):
        print(f"   idx{r['idx']:4d} {r['sat']} {lane(r):7s} n={r['n']:3d} dev={r['dev']:8.3f} m ({r['devwl']:6.1f} WL) thr={r['thr']:6.3f} ratio={r['ratio']:6.1f} known={r['known']} frac={r['frac']} bia={r['bia']}")
    acc = [r['idx'] for r in rows if r['n'] <= 2 and gate_today(r)]
    print(f"\n== ACCEPTED-today warm-up (n<=2) detections: {len(acc)}; idx: {acc}")
    print("== warm-up (n<=2) with ratio>=3: idx", [r['idx'] for r in rows if r['n'] <= 2 and r['ratio'] >= 3])
