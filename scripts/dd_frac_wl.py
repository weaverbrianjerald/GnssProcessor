#!/usr/bin/env python
"""Synthesize a WIDE-LANE (1,2) DD-fractional trace from a run's per-band network_dd_fractional.csv, in the
SAME schema, so scripts/dd_frac_reref.py can grade it unchanged (same nulls, same readouts).

    WL_cyc = dd_cyc(L1C) - dd_cyc(L2W)      per (week, tow, cons, sat_i, ref_sat, sta_a, sta_b) chord
    lambda_WL = c / (f1 - f2) = 0.861918 m  (GPS L1/L2)

Why: a wrapped-fraction grade is meaningless where the errors are multi-cycle; an n-cycle L1 error is n/4.5 cycles
at WL, so WL structure with uniform L1/L2 = multi-cycle-but-bounded L1 errors (SPEC_amb_iono §6.15.2).

Usage: python dd_frac_wl.py <run_dir> [--hi L1C --lo L2W --out <dir>]
Writes <out>/logs/network_dd_fractional.csv (default <run_dir>_wl12) and prints the chord-pairing census.
"""
import argparse, csv, io, os, sys

C = 299792458.0
F1, F2 = 1575.42e6, 1227.60e6
LAM_WL = C / (F1 - F2)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('--hi', default='L1C'); ap.add_argument('--lo', default='L2W')
    ap.add_argument('--out', default=None); ap.add_argument('--cons', default='G')
    a = ap.parse_args()
    src = os.path.join(a.run_dir, 'logs', 'network_dd_fractional.csv')
    out = a.out or (a.run_dir.rstrip('/\\') + '_wl12')
    os.makedirs(os.path.join(out, 'logs'), exist_ok=True)
    dst = os.path.join(out, 'logs', 'network_dd_fractional.csv')
    hi, lo = {}, {}
    n_rows = 0
    with io.open(src, encoding='utf-8', errors='replace') as f:
        r = csv.DictReader(f)
        cols = r.fieldnames
        for row in r:
            n_rows += 1
            if row['cons'] != a.cons: continue
            key = (row['week'], row['tow'], row['sat_i'], row['ref_sat'], row['sta_a'], row['sta_b'])
            if row['signal'] == a.hi: hi[key] = row
            elif row['signal'] == a.lo: lo[key] = row
    n_pair = 0
    with io.open(dst, 'w', encoding='utf-8', newline='') as g:
        w = csv.DictWriter(g, fieldnames=cols)
        w.writeheader()
        for key, rh in hi.items():
            rl = lo.get(key)
            if rl is None: continue
            try:
                wl = float(rh['dd_cyc']) - float(rl['dd_cyc'])
                s1 = float(rh.get('dd_sigma_cyc') or 'nan'); s2 = float(rl.get('dd_sigma_cyc') or 'nan')
            except ValueError:
                continue
            frac = wl - round(wl)
            o = dict(rh)
            o['signal'] = 'WL12'
            o['dd_cyc'] = f'{wl:.6f}'
            o['frac_cyc'] = f'{frac:.6f}'
            o['lambda_m'] = f'{LAM_WL:.6f}'
            o['dd_m'] = f'{wl * LAM_WL:.6f}'
            o['dd_sigma_cyc'] = f'{(s1 * s1 + s2 * s2) ** 0.5:.6f}' if s1 == s1 and s2 == s2 else ''
            w.writerow(o)
            n_pair += 1
    print(f'{src}: {n_rows} rows; {a.hi} chords {len(hi)}, {a.lo} chords {len(lo)}, paired WL12 chords {n_pair} -> {dst}')
    print(f'grade with: python scripts/dd_frac_reref.py {out} --signal WL12 --cons {a.cons}')

if __name__ == '__main__':
    sys.exit(main())
