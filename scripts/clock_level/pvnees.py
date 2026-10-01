#!/usr/bin/env python3
"""Card PVN graders from logs/network_phase_visible.csv (+ network_events.csv).

    python scripts/clock_level/pvnees.py <run> [<run> ...]      (run = dir, or tag under runs/_rap_<tag>)

Per run, after the first 15 epochs:
  NIS    the truth-free normalized update: mean e^2 / (S- - S+) (from the trace's nis column),
         per hour, and the share above 6.63;
  NEES   the within-arc state NEES: arcs per satellite broken at a gap (> 45 s) and at a
         phase-bias frame event of that satellite (PHASE_ANCHOR anchor_handover, PBGAUGE
         handover_*); pooled sum over arcs of sum (d - mean_arc d)^2 / sp^2 over sum (n - 1),
         arcs of >= 10 epochs.  On the sim the true D is constant within an arc, so this is a
         NEES (a lower bound: de-meaning removes drift); on the field the true D moves with the
         real clock correction beyond the a priori, so it reads as scatter over stated sd only.
  also   rms within-arc scatter and rms stated sd sqrt(S+), and rms update over rms predicted
         update sd.
"""
import csv, math, os, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNS = ROOT + '/runs'


def resolve(a):
    for c in (a, f'{RUNS}/{a}', f'{RUNS}/_rap_{a}'):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def grade(d):
    rows = []
    with open(os.path.join(d, 'logs', 'network_phase_visible.csv')) as f:
        for r in csv.DictReader(f):
            try:
                rows.append((float(r['tow']), r['sat'] + '|' + r.get('sigs', ''), float(r['d_hat_m']), float(r['sqrt_sp_m']),
                             float(r['e_hat_m']),
                             float(r['sqrt_su_m']) if r['sqrt_su_m'] else None,
                             float(r['nis']) if r['nis'] else None))
            except (ValueError, KeyError):
                continue
    if not rows:
        return None
    tows = sorted({r[0] for r in rows})
    t_settle = tows[15] if len(tows) > 15 else tows[-1]
    t0 = tows[0]
    ev = defaultdict(list)
    p = os.path.join(d, 'logs', 'network_events.csv')
    if os.path.exists(p):
        with open(p) as f:
            for r in csv.DictReader(f):
                if (r['tier'] == 'PHASE_ANCHOR' and r['event'] == 'anchor_handover') or \
                   (r['tier'] == 'PBGAUGE' and r['event'].startswith('handover')):
                    ev[r['sat'].split('_')[0]].append(float(r['tow']))
    # NIS per hour
    nis_h = defaultdict(list)
    num_e2 = den_su = 0.0
    for t, s, dh, sp, e, su, nis in rows:
        if t < t_settle or nis is None:
            continue
        nis_h[int((t - t0 + 1e-3) // 3600)].append(nis)
        num_e2 += e * e
        den_su += su * su
    # The trace's d_hat is the level less the constellation median of the LEVELS, and each
    # satellite's level carries its own arbitrary phase-bias datum (metres), so that median
    # jumps whenever the satellite set changes.  Rebuild a common-free series from the STEPS:
    # d~_s(t) = d~_s(t-1) + step_s(t) - median over satellites present at both epochs of the
    # step (the median of the levels cancels in every step, so this equals de-commoning the
    # raw D steps).  A satellite absent at t-1 restarts at 0 (a new arc anyway).
    by_t = defaultdict(dict)
    for t, s, dh, sp, e, su, nis in rows:
        by_t[t][s] = (dh, sp)
    dtil, prev = {}, {}
    series = []
    for t in tows:
        cur = by_t[t]
        steps = {s: cur[s][0] - prev[s] for s in cur if s in prev}
        if steps:
            vs = sorted(steps.values())
            k = len(vs)
            med = vs[k // 2] if k % 2 else 0.5 * (vs[k // 2 - 1] + vs[k // 2])
        for s, (dh, sp) in cur.items():
            dtil[s] = dtil[s] + steps[s] - med if s in steps else 0.0
            series.append((t, s, dtil[s], sp))
        prev = {s: v[0] for s, v in cur.items()}
    # within-arc NEES per hour (arc assigned to the hour of its start)
    by_sat = defaultdict(list)
    for t, s, dt_, sp in series:
        if t >= t_settle and sp > 0:
            by_sat[s].append((t, dt_, sp))
    nees_num = defaultdict(float)
    nees_den = defaultdict(float)
    scat2 = defaultdict(float)
    sp2 = defaultdict(float)
    nscat = defaultdict(int)
    n_arcs = 0
    for s, v in by_sat.items():
        v.sort()
        evs = sorted(ev.get(s.split('|')[0], []))
        arcs, cur = [], []
        for t, dh, sp in v:
            brk = bool(cur) and (t - cur[-1][0] > 45.0 or any(cur[-1][0] < te <= t for te in evs))
            if brk:
                arcs.append(cur)
                cur = []
            cur.append((t, dh, sp))
        if cur:
            arcs.append(cur)
        for a in arcs:
            if len(a) < 10:
                continue
            n_arcs += 1
            m = sum(x[1] for x in a) / len(a)
            h = int((a[0][0] - t0 + 1e-3) // 3600)
            nees_num[h] += sum((x[1] - m) ** 2 / x[2] ** 2 for x in a)
            nees_den[h] += len(a) - 1
            scat2[h] += sum((x[1] - m) ** 2 for x in a)
            sp2[h] += sum(x[2] ** 2 for x in a)
            nscat[h] += len(a)
    hours = sorted(set(nis_h) | set(nees_den))
    out = {
        'nis_h': [sum(nis_h[h]) / len(nis_h[h]) if nis_h[h] else float('nan') for h in hours],
        'nis_all': (sum(sum(v) for v in nis_h.values()) / max(1, sum(len(v) for v in nis_h.values()))),
        'nis_gt': 100.0 * sum(sum(1 for x in v if x > 6.63) for v in nis_h.values())
                  / max(1, sum(len(v) for v in nis_h.values())),
        'ratio': math.sqrt(num_e2 / den_su) if den_su > 0 else float('nan'),
        'nees_h': [nees_num[h] / nees_den[h] if nees_den[h] else float('nan') for h in hours],
        'nees_all': sum(nees_num.values()) / max(1.0, sum(nees_den.values())),
        'scat_h': [math.sqrt(scat2[h] / nscat[h]) if nscat[h] else float('nan') for h in hours],
        'sp_h': [math.sqrt(sp2[h] / nscat[h]) if nscat[h] else float('nan') for h in hours],
        'arcs': n_arcs, 'events': sum(len(v) for v in ev.values()),
    }
    return out


def f(v, p=2):
    return ' '.join(('  nan' if x != x else f'{x:.{p}f}') for x in v)


def main():
    for a in sys.argv[1:]:
        d = resolve(a)
        g = grade(d)
        if g is None:
            print(os.path.basename(d), 'no rows')
            continue
        print(f"{os.path.basename(d)}: NIS mean {g['nis_all']:.2f} ({g['nis_gt']:.1f} % > 6.63), "
              f"by hour [{f(g['nis_h'])}]; rms update / rms predicted sd {g['ratio']:.2f}")
        print(f"   within-arc NEES {g['nees_all']:.1f}, by hour [{f(g['nees_h'], 1)}]; "
              f"rms scatter [{f(g['scat_h'], 4)}] m vs rms stated sd [{f(g['sp_h'], 4)}] m; "
              f"{g['arcs']} arcs, {g['events']} frame events")


if __name__ == '__main__':
    main()
