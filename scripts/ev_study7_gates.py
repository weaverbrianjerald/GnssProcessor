#!/usr/bin/env python3
"""Study 7 storm-arm gates (SPEC_event_harness s16.4-s16.5): W1 (consumer position error), W2 (the 4.99 h fallout), G4 (wrong-narrowing census) and W3 (phase-anchor handovers), for arm B against baseline A (scope off) and C (cbscope3).
Usage: ev_study7_gates.py <B run name> [A run name] [C run name]"""
import csv, collections, math, sys, os
R = 'C:/GnssProcessor/runs/'; FX = 'C:/GnssProcessor/runs/simnet60_campaign_wu93/'
B = sys.argv[1]; A = sys.argv[2] if len(sys.argv) > 2 else 'golden_sim_campaign_zwdblk_mwevid'
C = sys.argv[3] if len(sys.argv) > 3 else 'golden_sim_campaign_zwdblk_cbscope3'
t0 = 302400.0
def load(p): return list(csv.DictReader(open(p, encoding='utf-8', errors='replace')))
H = lambda tow: (float(tow) - t0) / 3600
def p50(x):
    x = sorted(x); n = len(x); return 0.5 * (x[n // 2 - 1] + x[n // 2]) if n % 2 == 0 else x[n // 2]
def rms(x): return math.sqrt(sum(v * v for v in x) / len(x))
runs = [A, C, B]
cons = {r: [(H(x['tow']), float(x['pos_err_3d_m'])) for x in load(R + r + '/logs/closed_loop_trace.csv')] for r in runs}
print('== W1 consumer (settled h>=3): pooled p50/rms | window 4.6833-5.40 EXCLUDED p50/rms | 3.0-4.68 p50 | hourly p50')
for r in runs:
    v = cons[r]; pooled = [e for h, e in v if h >= 3]; excl = [e for h, e in v if h >= 3 and not (4.6833 <= h <= 5.40)]
    pre = [e for h, e in v if 3.0 <= h < 4.6833]
    hourly = ['%.3f' % p50([e for h, e in v if k <= h < k + 1]) for k in range(6)]
    tag = 'A' if r == A else ('C' if r == C else 'B')
    print('  %s %-40s %.4f/%.4f | %.4f/%.4f | %.4f | %s' % (tag, r[-40:], p50(pooled), rms(pooled), p50(excl), rms(excl), p50(pre), ' '.join(hourly)))
ev = {r: load(R + r + '/logs/network_events.csv') for r in runs}
print('\n== W2 the 4.99 h fallout (tow 320370):')
for r in (C, B):
    e = ev[r]
    t4 = [x for x in e if x['tier'] == 'T4_attrib' and abs(float(x['tow']) - 320370) < 1]
    ca = [x for x in e if x['tier'] == 'CLK_ANCHOR' and abs(float(x['tow']) - 320370) < 1 and x['action'] in ('refloat', 'disc_bump')]
    print('  %s: T4 rows at 4.9917 = %d %s; CLK_ANCHOR refloat/disc_bump rows = %d' % (r[-12:], len(t4), [(x['sat'], x['station']) for x in t4][:6], len(ca)))
    dc = collections.defaultdict(dict)
    for x in load(R + r + '/logs/network_solution.csv'):
        if x['kind'] == 'CLK': dc[x['sat']][float(x['tow'])] = x['disc_counter']
    n = sum(1 for s, m in dc.items() if 320340.0 in m and 320370.0 in m and m[320340.0] != m[320370.0])
    print('       declared CLK disc changes at 4.9917: %d' % n)
print('\n== G4 wrong-narrowing census (keep rows): unanswered-exonerate + spurious-implicate')
truth = {}
def tslips(sta):
    if sta not in truth:
        p = FX + sta + '/truth_slips.csv'; truth[sta] = load(p) if os.path.exists(p) else []
    return truth[sta]
for r in (C, B):
    e = ev[r]
    byarc = collections.defaultdict(list)
    for x in e:
        if x['tier'] in ('T0_mw_detect', 'T1_if_slip') and x['action'] == 'refloat': byarc[(x['station'], x['sat'])].append((float(x['tow']), x['detail']))
    keeps = [x for x in e if x['tier'] == 'T0_band_scope' and x['action'] == 'keep']
    wrong = []; spur = []
    for k in keeps:
        tw = float(k['tow']); d = dict(kv.split('=', 1) for kv in k['detail'].split(';') if '=' in kv)
        exo = set(d.get('exonerated', '').split(';')) - {''}; imp = set(d.get('implicated', '').split(';')) - {''}
        ts = [s for s in tslips(k['station']) if s['satellite'] == k['sat']]
        for s in ts:
            st = float(s['tow_s'])
            if tw - 90 <= st <= tw and s['signal'] in exo:
                # answered if a refloat row naming that band exists on the arc between the slip epoch and t-30
                answered = any(st <= rt <= tw - 30 and s['signal'] in det for rt, det in byarc[(k['station'], k['sat'])])
                if not answered: wrong.append((round(H(tw), 4), k['station'], k['sat'], s['signal'], s['delta_cycles']))
        if imp and not any(abs(float(s['tow_s']) - tw) <= 60 and s['signal'] in imp for s in ts):
            if any(abs(float(s['tow_s']) - tw) <= 60 for s in ts): spur.append((round(H(tw), 4), k['station'], k['sat'], sorted(imp)))
    print('  %s: keeps %d | unanswered-exonerate %d %s | spurious-implicate (slip nearby, implicated band clean) %d %s' % (r[-12:], len(keeps), len(wrong), wrong[:4], len(spur), spur[:6]))
print('\n== W3 handovers: cold/continuous by hour, and totals')
for r in runs:
    rows = [x for x in ev[r] if x['tier'] == 'PHASE_ANCHOR' and x['action'] == 'handover']
    by = [(sum('branch=cold' in x['detail'] for x in rows if int(H(x['tow'])) == h), sum('branch=continuous' in x['detail'] for x in rows if int(H(x['tow'])) == h)) for h in range(6)]
    print('  %-40s %s  total %d cold %d' % (r[-40:], ' '.join('%d/%d' % b for b in by), len(rows), sum(b[0] for b in by)))
print('\n== T4 rows by hour');
for r in runs:
    c = collections.Counter(int(H(x['tow'])) for x in ev[r] if x['tier'] == 'T4_attrib'); print('  %-40s %s total %d' % (r[-40:], [c[h] for h in range(6)], sum(c.values())))
