#!/usr/bin/env python3
"""Card SMH G3 grade: the hidden fraction of an injected a priori clock walk.

    python scripts/clock_level/g3grade.py <walk_csv|none> <run> [<run> ...]      (run = dir or tag under runs/_rap_)

The a priori clock of GPS satellite s is truth + w_s(t) (g3_walk.py), so the true clock
correction is -w_s(t) and the true phase-visible correction D = c - p moves by -dw (the sim's
phase biases are constant).  On the wire D_w = C - P (signal L1C).  Both are de-commoned per
epoch (the median across the GPS satellites present in both is removed: the consumer absorbs a
common mode).  The passed fraction is the pooled slope of the wire's increment on the truth's,
increments taken from a reference epoch t_ref; hidden = 1 - passed.

  clone   segments where the satellite's L1C clone is live (network_pb_hold_clone.csv),
          t_ref = the clone's birth epoch; the prediction f0 rho_D, rho_D = pd0 / (pd0 + W),
          weighted like the measurement (dD_true^2), from the same rows.
  gen     every held emission of a generation, t_ref = the generation's first emission (the
          arm's whole hold, bridges and all; the phase-clock hold's number).
Also: the internal split of the estimator's own motion since birth (revision = xfer - base
against dc = phi + revision), the clock error of the wire with the walk subtracted (rms of the
de-commoned C + w across satellites, by hour), and the consumer 3D p50 by hour.
"""
import csv
import math
import os
import statistics as st
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
T0 = 259200.0


def resolve(a):
    for c in (a, f'{RUNS}/{a}', f'{RUNS}/_rap_{a}'):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def load_walk(p):
    w = defaultdict(dict)
    if p == 'none':
        return w
    for r in csv.DictReader(open(p)):
        w[r['sat']][round(float(r['tow']))] = float(r['w_m'])
    return w


def walk_at(w, s, t):
    d = w.get(s)
    if not d:
        return 0.0
    k = int(round(t / 30.0)) * 30
    return d.get(k, 0.0)


def load_wire(d):
    C = defaultdict(dict)      # tow -> sat -> C
    P = defaultdict(dict)      # tow -> sat -> (P L1C, gen)
    tow = None
    with open(os.path.join(d, 'products.txt'), errors='replace') as f:
        for line in f:
            c0 = line[0]
            if c0 == 'E':
                tow = round(float(line.split(',')[2]), 1)
            elif c0 == 'C' and tow is not None:
                pr = line.split(',')
                if pr[1][0] == 'G':
                    C[tow][pr[1]] = float(pr[2])
            elif c0 == 'P' and tow is not None:
                pr = line.rstrip().split(',')
                if pr[1][0] == 'G' and pr[2] == 'L1C':
                    g = next((x[4:] for x in pr[6:] if x.startswith('gen=')), '0')
                    P[tow][pr[1]] = (float(pr[3]), int(g))
    return C, P


F1 = 1575.42e6; F2 = 1227.60e6; A = F1 * F1 / (F1 * F1 - F2 * F2); B = F2 * F2 / (F1 * F1 - F2 * F2)


def datum_share():
    # the IF(C1C, C2W) of the OSB the sim injects: the datum's share of the wire clock (clklevel.py)
    p = ROOT + '/runs/simnet60_quiet_wu93_24h/S01000SIM/truth_bias.csv'
    d = defaultdict(dict)
    if not os.path.exists(p):
        return {}
    for r in csv.DictReader(open(p)):
        d[r['satellite']][r['signal']] = float(r['osb_m'])
    return {s: A * v['C1C'] - B * v['C2W'] for s, v in d.items() if 'C1C' in v and 'C2W' in v}


def decommon(vals):
    if len(vals) < 3:
        return {}
    m = st.median(vals.values())
    return {s: v - m for s, v in vals.items()}


def consumer(d):
    p = os.path.join(d, 'logs', 'closed_loop_trace.csv')
    by = defaultdict(list)
    if not os.path.exists(p):
        return ''
    t0 = None
    for r in csv.DictReader(open(p)):
        try:
            t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, KeyError):
            continue
        if t0 is None:
            t0 = t
        by[int(round((t - t0) * 1000)) // 3600000].append(e)
    return ' / '.join(f'{st.median(by[h]):.3f}' for h in sorted(by) if len(by[h]) > 30)


def slope(pairs):
    sxy = sum(x * y for x, y in pairs)
    sxx = sum(x * x for x, y in pairs)
    return sxy / sxx if sxx > 0 else float('nan'), len(pairs), math.sqrt(sxx / max(1, len(pairs)))


def main():
    w = load_walk(sys.argv[1])
    ds = datum_share()
    for a in sys.argv[2:]:
        d = resolve(a)
        C, P = load_wire(d)
        tows = sorted(set(C) & set(P))
        dw, dt = {}, {}
        err_by_h = defaultdict(list)
        for t in tows:
            sats = [s for s in C[t] if s in P[t]]
            dw[t] = decommon({s: C[t][s] - P[t][s][0] for s in sats})
            dt[t] = decommon({s: -walk_at(w, s, t) for s in sats})
            ce = decommon({s: C[t][s] + ds[s] + walk_at(w, s, t) for s in C[t] if s in ds})
            if ce:
                err_by_h[int((t - tows[0]) // 3600)].append(math.sqrt(sum(v * v for v in ce.values()) / len(ce)))
        # gen: increments from each generation's first emission
        gen_pairs = []
        first = {}
        for t in tows:
            for s, (p, g) in P[t].items():
                if s not in dw[t] or s not in dt[t]:
                    continue
                k = (s, g)
                if k not in first:
                    first[k] = t
                    continue
                t0 = first[k]
                if s in dw.get(t0, {}) and s in dt.get(t0, {}):
                    gen_pairs.append((dt[t][s] - dt[t0][s], dw[t][s] - dw[t0][s]))
        out = [f'== {os.path.basename(d)}']
        sg, ng, xg = slope(gen_pairs)
        out.append(f'  gen   : hidden {1 - sg:.3f} over {ng} held emissions (rms injected increment {xg:.4f} m)')
        cp = os.path.join(d, 'logs', 'network_pb_hold_clone.csv')
        if os.path.exists(cp):
            cl_pairs, pred_w, int_pairs = [], [], []
            births = set()
            for r in csv.DictReader(open(cp)):
                if r['sig'] != 'L1C':
                    continue
                t = round(float(r['tow']), 1)
                tb = round(t - float(r['age_s']), 1)
                s = r['sat']
                births.add((s, r['gen'], tb))
                try:
                    phi = float(r['phi_m']); rev = float(r['xfer_m']) - float(r.get('base_m') or 0.0)
                    int_pairs.append((phi + rev, phi))
                except ValueError:
                    pass
                if s not in dw.get(t, {}) or s not in dw.get(tb, {}) or s not in dt.get(tb, {}):
                    continue
                x = dt[t][s] - dt[tb][s]
                y = dw[t][s] - dw[tb][s]
                cl_pairs.append((x, y))
                try:
                    pd0 = float(r.get('pd0_m2') or 'nan'); f0 = float(r.get('f0') or 'nan'); W = float(r['w_m2'])
                    if math.isfinite(pd0) and math.isfinite(f0):
                        pred_w.append((f0 * pd0 / (pd0 + W), x * x))
                except (ValueError, ZeroDivisionError):
                    pass
            sc, nc, xc = slope(cl_pairs)
            hp = (sum(h * ww for h, ww in pred_w) / sum(ww for h, ww in pred_w)) if pred_w and sum(ww for h, ww in pred_w) > 0 else float('nan')
            si, ni, xi = slope(int_pairs)
            out.append(f'  clone : hidden {1 - sc:.3f} measured, {hp:.3f} predicted f0 rho_D '
                       f'({nc} clone-epochs of {len(births)} clone(s); {len(pred_w)} with a prediction; '
                       f'rms injected increment {xc:.4f} m)')
            out.append(f'  inside: the estimator\'s motion since birth passed {si:.3f} (revision {1 - si:.3f}) '
                       f'over {ni} rows, rms {xi:.4f} m')
        out.append('  clock error (datum share and walk removed), de-commoned rms, median by hour: '
                   + ' / '.join(f'{st.median(v):.3f}' for h, v in sorted(err_by_h.items())))
        out.append('  consumer 3D p50 by hour: ' + consumer(d))
        print('\n'.join(out))


if __name__ == '__main__':
    main()
