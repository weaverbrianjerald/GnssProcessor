#!/usr/bin/env python3
"""Card SMH offline: the smoothed hold's wire with the constellation common mode of its
transfer removed, and a consumer-replay arm.

    python scripts/clock_level/smh_rewrite.py <smh_run> <rule> <out_tag> [--no-launch]

rules:
  same   P unchanged (the replay must reproduce the live consumer)
  emit   P = held + [C(t) - C(t1)] - Phi(t) from the key's first smoothed emission t1 (emit0: Phi
         zeroed for a clone whose W spans LOOSE epochs; emitz: Phi zeroed for every clone)
  smdc   P = held + [xfer(t) - (M_k(t) - M_k(t0))]: xfer(t) = P(t) - P(t0) the clone's transfer
         read off the wire, M_k the accumulated per-epoch median over constellation k's held keys
         (present at both epochs, same generation) of the transfer's step
Writes runs/_rpl_<out_tag>/{products.txt, arm.json} and launches the replay (Release exe).
The exe is $CLOCK_LEVEL_EXE, by default the renamed Release copy gnss_processor_smh3.exe the card used.
"""
import json, os, statistics as st, subprocess, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
EXE = os.environ.get('CLOCK_LEVEL_EXE', ROOT + '/build/x64-Release/gnss_processor_smh3.exe')


def resolve(a):
    for c in (a, f'{RUNS}/{a}', f'{RUNS}/_rap_{a}'):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def epochs(path):
    cur, tow = [], None
    for ln in open(path, encoding='utf-8'):
        if ln.startswith('H,'):
            if cur:
                yield tow, cur
            cur, tow = [ln], round(float(ln.split(',')[2]), 1)
            continue
        cur.append(ln)
    if cur:
        yield tow, cur


def main():
    run, rule, tag = resolve(sys.argv[1]), sys.argv[2], sys.argv[3]
    launch = '--no-launch' not in sys.argv
    # pass 1: the transfer per key and epoch, and the common mode
    order, pb, gen = [], {}, {}
    for tow, lines in epochs(f'{run}/products.txt'):
        if tow is None:
            continue
        order.append(tow)
        for ln in lines:
            f = ln.rstrip('\n').split(',')
            if f[0] == 'P':
                pb[(tow, f[1], f[2])] = float(f[3])
                for tok in f[6:]:
                    if tok.startswith('gen='):
                        gen[(tow, f[1], f[2])] = int(tok[4:])
    t0v, xfer = {}, {}
    last = {}
    for t in order:
        for (tt, s, sig), v in [(k, v) for k, v in pb.items() if k[0] == t]:
            key = (s, sig)
            g = gen.get((t, s, sig))
            if last.get(key) != g:
                t0v[key] = v
                last[key] = g
                xfer[(t, s, sig)] = 0.0
            else:
                xfer[(t, s, sig)] = v - t0v[key]
    M = defaultdict(float)
    Mt = {}
    prev = None
    for t in order:
        if prev is not None:
            steps = defaultdict(list)
            for (tt, s, sig), x in xfer.items():
                if tt != t:
                    continue
                if (prev, s, sig) in xfer and gen.get((prev, s, sig)) == gen.get((t, s, sig)):
                    steps[s[0]].append(x - xfer[(prev, s, sig)])
            for cc, v in steps.items():
                M[cc] += st.median(v)
        Mt[t] = dict(M)
        prev = t
    # the emitted clock per satellite and epoch, and Phi / W per key and epoch from the trace
    clk = {}
    for tow, lines in epochs(f'{run}/products.txt'):
        if tow is None:
            continue
        for ln in lines:
            f = ln.rstrip('\n').split(',')
            if f[0] == 'C':
                clk[(tow, f[1])] = float(f[2])
    phi, wk = {}, {}
    import csv
    for r in csv.DictReader(open(f'{run}/logs/network_pb_hold_clone.csv')):
        k = (round(float(r['tow']), 1), r['sat'], r['sig'])
        phi[k] = float(r['phi_m']) if r['phi_m'] else 0.0
        wk[k] = float(r['w_m2'])
    out_dir = f'{RUNS}/_rpl_{tag}'
    os.makedirs(out_dir + '/logs', exist_ok=True)
    m0 = {}
    last = {}
    last_t0 = {}
    with open(f'{out_dir}/products.txt', 'w', encoding='utf-8', newline='\n') as fo:
        for tow, lines in epochs(f'{run}/products.txt'):
            for ln in lines:
                if tow is None or not ln.startswith('P,') or rule == 'same':
                    fo.write(ln)
                    continue
                f = ln.rstrip('\n').split(',')
                s, sig = f[1], f[2]
                key = (s, sig)
                g = gen.get((tow, s, sig))
                if last.get(key) != g:
                    last[key] = g
                    m0[key] = Mt[tow].get(s[0], 0.0)
                    last_t0[key] = None
                if rule == 'smdc':
                    v = pb[(tow, s, sig)] - (Mt[tow].get(s[0], 0.0) - m0[key])
                else:
                    # emission frame: P = held + [C(t) - C(t0)] - Phi(t) (emit), with Phi zeroed
                    # for a clone whose W spans LOOSE epochs (emit0)
                    if last_t0.get(key) is None or (tow, s, sig) not in phi:
                        last_t0[key] = (tow, pb[(tow, s, sig)], clk.get((tow, s)))
                        v = pb[(tow, s, sig)]
                    else:
                        t0, held, c0 = last_t0[key]
                        c = clk.get((tow, s))
                        ph = phi[(tow, s, sig)]
                        if (rule == 'emit0' and wk[(tow, s, sig)] > 0.01) or rule == 'emitz':
                            ph = 0.0
                        v = held + ((c - c0) if (c is not None and c0 is not None) else 0.0) - ph
                    f[3] = repr(v)
                    fo.write(','.join(f) + '\n')
                    continue
                f[3] = repr(v)
                fo.write(','.join(f) + '\n')
    print(f'{tag}: wrote {out_dir}/products.txt ({len(order)} epochs); final M {dict(M)}')
    arm = json.load(open(f'{run}/arm.json'))
    nw = arm['network']
    nw['closed_loop']['replay_products_path'] = f'{out_dir}/products.txt'
    nw['product_output_path'] = f'{out_dir}/products_replay_out.txt'
    nw['start_offset_hours'] = 0.0
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
