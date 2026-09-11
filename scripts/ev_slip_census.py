#!/usr/bin/env python3
"""EVENT HARNESS — detector-coverage census over injected slips (SPEC_event_harness §13.1): for each schedule row, what caught it
(within +0..+2 epochs, same station+sat) and at what elevation / band / size / LLI.
Usage: slip_census.py <leg> [<leg> ...]"""
import sys, os, csv, collections
def load(leg):
    sched = list(csv.DictReader(open(os.path.join(leg, 'schedule.csv'))))
    ev = collections.defaultdict(list)
    for r in csv.DictReader(open(os.path.join(leg, 'logs', 'network_events.csv'))):
        ev[(r['station'], r['sat'])].append((float(r['tow']), r['tier'], r['event'], r['action'], r['detail']))
    raw = {}
    for r in csv.DictReader(open(os.path.join(leg, 'logs', 'network_slip_raw.csv'))):
        raw[(r['station'], r['sat'], float(r['tow']))] = float(r['elev_deg'])
    return sched, ev, raw
def classify(s, ev, raw):
    st, sat, tow = s['station'], s['sat'], float(s['tow'])
    if (st, sat, tow) not in raw: return 'INERT', '', raw.get((st, sat, tow))
    el = raw[(st, sat, tow)]
    hits = [e for e in ev[(st, sat)] if tow <= e[0] <= tow + 60 and e[1] != 'INJECT']
    det = [e for e in hits if e[1] in ('T0_mw_detect', 'T1_if_slip', 'T0_lli') or 'slip' in e[2]]
    det = [e for e in det if e[1] != 'T4_attrib']
    if det:
        d = det[0]; lag = int((d[0] - tow) // 30)
        return f'{d[1]}/{d[2]}', f'{d[4]}@+{lag}', el
    t4 = [e for e in hits if e[1] == 'T4_attrib']
    if t4: return 'T4_only', f'@+{int((t4[0][0]-tow)//30)}', el
    return 'MISS', '', el
out = collections.Counter(); rows = []
for leg in sys.argv[1:]:
    sched, ev, raw = load(leg)
    for s in sched:
        cat, det, el = classify(s, ev, raw)
        rows.append((os.path.basename(leg), s['station'], s['sat'], s['signal'], int(float(s['delta_cycles'])), s['flag_lli'], el, cat, det))
def band(sig): return sig[:2]
print(f"{'leg':18s} {'n':>4s}  {'by category':s}")
for leg in sorted({r[0] for r in rows}):
    c = collections.Counter(r[7] for r in rows if r[0] == leg)
    print(f"{leg:18s} {sum(c.values()):4d}  " + '  '.join(f"{k}:{v}" for k, v in c.most_common()))
live = [r for r in rows if r[7] != 'INERT']
print("\n== misses and T4-only (non-inert), all legs")
print(f"{'leg':18s} {'station':10s} {'sat':4s} {'sig':4s} {'dcyc':>5s} {'lli':>3s} {'elev':>5s} {'cat':10s} det")
for r in live:
    if r[7] in ('MISS', 'T4_only'):
        print(f"{r[0]:18s} {r[1]:10s} {r[2]:4s} {r[3]:4s} {r[4]:5d} {r[5]:>3s} {r[6]:5.1f} {r[7]:10s} {r[8]}")
print("\n== miss/T4-only rate by (band, lli) over non-inert rows")
g = collections.defaultdict(lambda: [0, 0])
for r in live:
    k = (band(r[3]), r[5]); g[k][1] += 1; g[k][0] += (r[7] in ('MISS', 'T4_only'))
for k in sorted(g): print(f"  {k[0]} lli={k[1]}: {g[k][0]}/{g[k][1]} missed")
print("\n== miss/T4-only rate by elevation bin (lli=0 only)")
g = collections.defaultdict(lambda: [0, 0])
for r in live:
    if r[5] != '0': continue
    b = int(r[6] // 10) * 10; g[b][1] += 1; g[b][0] += (r[7] in ('MISS', 'T4_only'))
for k in sorted(g): print(f"  {k:2d}-{k+9:2d} deg: {g[k][0]}/{g[k][1]} missed")
print("\n== which pair fired on lli=0 hits, by injected band")
g = collections.Counter()
for r in live:
    if r[5] == '0' and r[7] not in ('MISS', 'T4_only', 'INERT'):
        g[(band(r[3]), r[7], r[8].split('@')[0])] += 1
for k, v in sorted(g.items(), key=lambda kv: (-kv[1])): print(f"  {k[0]} {k[1]:28s} {k[2]:22s} {v}")
