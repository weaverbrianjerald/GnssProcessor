#!/usr/bin/env python3
"""Offline phase-bias hold rules on a recorded wire, then a consumer-replay arm.

The hold is emit-only: the hold-on and hold-off wires of the same arm differ in the P values only
(and gen= / pbhold=1).  So any hold rule that uses emitted quantities can be written offline:
generation structure from the hold-on wire, estimates from the hold-off wire.

    python scripts/clock_level/hold_rewrite.py <hold_on_run> <hold_off_run> <rule> <out_tag> [--no-launch]

rules:
  hold    p = held (must reproduce the hold-on wire)
  off     p = estimate (must reproduce the hold-off P values)
  dc      p = held + accumulated de-commoned clock step since the generation start (the sim oracle)
  dcraw   the same without de-commoning
  if      p = held + accumulated de-commoned IF PB step, sum_j w_j dp_j (w = 2.5457 / -1.5457)
  ifraw   the same without de-commoning
Writes runs/_rpl_<out_tag>/{products.txt, arm.json} and launches the replay (Release exe, cwd = run dir).
The exe is $CLOCK_LEVEL_EXE, by default the renamed Release copy gnss_processor_ctu15.exe the card used.
"""
import json, os, statistics as st, subprocess, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
EXE = os.environ.get('CLOCK_LEVEL_EXE', ROOT + '/build/x64-Release/gnss_processor_ctu15.exe')
F1, F2 = 1575.42e6, 1227.60e6
W = {'1': F1 * F1 / (F1 * F1 - F2 * F2), '2': -F2 * F2 / (F1 * F1 - F2 * F2)}


def resolve(a):
    for c in (a, f'{RUNS}/{a}', f'{RUNS}/_rap_{a}'):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def epochs(path):
    """Yield (tow, [lines]) per epoch; header lines (before the first H/E) as tow None."""
    cur, tow = [], None
    for ln in open(path, encoding='utf-8'):
        if ln.startswith('H,'):
            if cur:
                yield tow, cur
            cur, tow = [ln], float(ln.split(',')[2])
            continue
        cur.append(ln)
    if cur:
        yield tow, cur


def read_wire(path):
    clk, pb, gen = {}, {}, {}
    order = []
    for tow, lines in epochs(path):
        if tow is None:
            continue
        order.append(tow)
        for ln in lines:
            f = ln.rstrip('\n').split(',')
            if f[0] == 'C':
                clk[(tow, f[1])] = float(f[2])
            elif f[0] == 'P':
                pb[(tow, f[1], f[2])] = float(f[3])
                for tok in f[6:]:
                    if tok.startswith('gen='):
                        gen[(tow, f[1], f[2])] = int(tok[4:])
    return order, clk, pb, gen


def median(v):
    return st.median(v) if v else 0.0


def main():
    on, off, rule, tag = resolve(sys.argv[1]), resolve(sys.argv[2]), sys.argv[3], sys.argv[4]
    launch = '--no-launch' not in sys.argv
    order, clk_on, pb_on, gen_on = read_wire(f'{on}/products.txt')
    order2, clk, est, _ = read_wire(f'{off}/products.txt')
    assert order == order2, 'epoch sets differ'
    decom = rule in ('dc', 'if')
    base = rule[:2] if rule.startswith(('dc', 'if')) else rule

    # per-epoch per-satellite step of the transferred quantity, de-commoned across satellites
    step = {}
    prev_t = None
    for t in order:
        if prev_t is not None:
            raw = {}
            sats = {s for (tt, s) in clk if tt == t}
            for s in sats:
                if base == 'dc':
                    if (prev_t, s) in clk:
                        raw[s] = clk[(t, s)] - clk[(prev_t, s)]
                elif base == 'if':
                    v, ok = 0.0, True
                    for sig in ('L1C', 'L2W'):
                        a, b = est.get((t, s, sig)), est.get((prev_t, s, sig))
                        if a is None or b is None:
                            ok = False; break
                        v += W[sig[1]] * (a - b)
                    if ok:
                        raw[s] = v
            m = median(list(raw.values())) if decom else 0.0
            for s, v in raw.items():
                step[(t, s)] = v - m
        prev_t = t

    # rewrite
    out_dir = f'{RUNS}/_rpl_{tag}'
    os.makedirs(out_dir + '/logs', exist_ok=True)
    held, acc, last_gen = {}, defaultdict(float), {}
    n_mis = 0
    with open(f'{out_dir}/products.txt', 'w', encoding='utf-8', newline='\n') as fo:
        for tow, lines in epochs(f'{on}/products.txt'):
            if tow is not None:
                for s in {k[1] for k in step if k[0] == tow}:
                    acc[s] += step[(tow, s)]
            for ln in lines:
                if tow is None or not ln.startswith('P,'):
                    fo.write(ln); continue
                f = ln.rstrip('\n').split(',')
                s, sig = f[1], f[2]
                k = (tow, s, sig)
                g = gen_on.get(k)
                key = (s, sig)
                if g is not None and last_gen.get(key) != g:
                    held[key] = (est[k], acc[s]); last_gen[key] = g
                    if abs(est[k] - pb_on[k]) > 1e-9:
                        n_mis += 1
                if rule == 'hold':
                    v = held[key][0]
                elif rule == 'off':
                    v = est[k]
                else:
                    v = held[key][0] + (acc[s] - held[key][1])
                f[3] = repr(v)
                fo.write(','.join(f) + '\n')
    print(f'{tag}: wrote {out_dir}/products.txt ({len(order)} epochs, gen-start mismatches {n_mis})')

    arm = json.load(open(f'{on}/arm.json'))
    nw = arm['network']
    nw['closed_loop']['replay_products_path'] = f'{out_dir}/products.txt'
    nw['product_output_path'] = f'{out_dir}/products_replay_out.txt'
    nw['start_offset_hours'] = 0.0
    for k in ('state_snapshot_path',):
        if k in nw:
            nw[k] = ''
    nw['state_snapshot_require'] = False
    dur = 30.0 * len(order) + 30.0
    arm['processing']['max_duration_s'] = dur
    nw['max_duration_s'] = dur
    json.dump(arm, open(f'{out_dir}/arm.json', 'w'), indent=1)
    if launch:
        subprocess.run(['pwsh', '-NoProfile', '-File', ROOT + '/scripts/ev_launch.ps1', '-Exe', EXE,
                        '-Config', f'{out_dir}/arm.json', '-RunDir', out_dir], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(tag, open(f'{out_dir}/exitcode.txt').read().split())


if __name__ == '__main__':
    main()
