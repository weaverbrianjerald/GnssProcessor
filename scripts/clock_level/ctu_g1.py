#!/usr/bin/env python3
"""Card CTU gate G1 (structure) from the run-end lines of stdout.txt: the anchor's folds at
its variance floor (share of all clock folds), the anchor trips, the post-fit RAIM-attrib
re-floats (epochs and ambiguities), epochs emitted / withheld, and the CTU telemetry.
    python scripts/clock_level/ctu_g1.py <run dir> [<run dir> ...]"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'


def grade(d):
    if not os.path.isdir(d):
        d = os.path.join(RUNS, d if d.startswith('_') else '_rap_' + d)
    out = {'run': os.path.basename(d)}
    raim_ep = raim_amb = 0
    ctu = []
    for ln in open(os.path.join(d, 'stdout.txt'), encoding='utf-8', errors='replace'):
        m = re.search(r'Card Q3 / guarded temporal clock anchor\): (\d+) epoch\(s\); (\d+) clock carry '
                      r'fold\(s\) . (\d+) ANCHORED.*?, (\d+) LOOSE, (\d+) at the variance floor\.\s+Trips (\d+)', ln)
        if m:
            ep, folds, anch, loose, floor, trips = map(int, m.groups())
            out.update(folds=folds, floor=floor, floor_pct=100.0 * floor / max(folds, 1), trips=trips)
        m = re.search(r'RAIM-attrib\]: TOW [\d.]+ refloated (\d+) carried', ln)
        if m:
            raim_ep += 1
            raim_amb += int(m.group(1))
        m = re.search(r'producer complete: (\d+) epochs emitted, (\d+) skipped \((\d+) of those withheld by '
                      r'the observability gate, (\d+) by the A4', ln)
        if m:
            out.update(emitted=int(m.group(1)), skipped=int(m.group(2)), gate_wh=int(m.group(3)))
        if 'card CTU' in ln and 'main_log' in ln:
            ctu.append(ln[ln.index('Network (card CTU'):].strip())
    out.update(raim_ep=raim_ep, raim_amb=raim_amb)
    return out, ctu


def main():
    rows = [grade(a) for a in sys.argv[1:]]
    cols = ['run', 'emitted', 'skipped', 'gate_wh', 'folds', 'floor', 'floor_pct', 'trips', 'raim_ep', 'raim_amb']
    fmt = lambda c, v: f'{v:.1f}' if c == 'floor_pct' and isinstance(v, float) else str(v)
    w = {c: max(len(c), *(len(fmt(c, r[0].get(c, ''))) for r in rows)) for c in cols}
    print('  '.join(c.ljust(w[c]) for c in cols))
    for r, _ in rows:
        print('  '.join(fmt(c, r.get(c, '')).ljust(w[c]) for c in cols))
    for r, ctu in rows:
        if ctu:
            print('--', r['run'])
            for l in ctu:
                print('  ', l[:600])


if __name__ == '__main__':
    main()
