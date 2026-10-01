#!/usr/bin/env python3
"""Card SMH G3: a known per-satellite random walk added to the a priori clock product.

    python scripts/clock_level/g3_walk.py make  <src_clk> <dst_clk> <walk_csv> [--q 5e-6] [--t0 261000] [--seed 129]
    python scripts/clock_level/g3_walk.py check <src_clk> <dst_clk> <walk_csv>

Every GPS 'AS' record at or after t0 (TOW, s) is moved by w_s(t)/c, w_s a random walk in
metres with increment variance q * dt per record step, starting at 0 at t0 (the records
before t0 are untouched).  Each satellite's walk is drawn from its own seeded stream, so a
satellite's walk does not depend on which other satellites the file carries.  The walk is
written to walk_csv (sat, tow, w_m) at every record time, for the truth grade.
Formatting: the first clock value of an AS line is rewritten in the file's own Fortran E
format, ending in the same column; every other byte of the file is copied.
"""
import argparse
import csv
import math
import random
import sys

C = 299792458.0


def tow_of(fields):
    # 'AS G01  2024 05 08 00 00  0.000000  2 ...' -> TOW (2024-05-08 is a Wednesday)
    y, mo, d, h, mi, s = fields
    import datetime as dt
    t = dt.datetime(int(y), int(mo), int(d), int(h), int(mi)) + dt.timedelta(seconds=float(s))
    gps0 = dt.datetime(1980, 1, 6)
    sec = (t - gps0).total_seconds()
    return sec % 604800.0


def ffmt(v):
    """Fortran E format with a mantissa in [0.1, 1): '0.276601612507E-03'."""
    if v == 0.0:
        return '0.000000000000E+00'
    e = math.floor(math.log10(abs(v))) + 1
    m = v / 10.0 ** e
    if round(abs(m), 12) >= 1.0:
        e += 1
        m = v / 10.0 ** e
    return f'{m:.12f}E{e:+03d}'


def value_span(line):
    """(start, end) of the first clock value token of an AS line (the 10th token)."""
    import re
    toks = list(re.finditer(r'\S+', line))
    return toks[9].start(), toks[9].end()


def parse_as(line):
    if not line.startswith('AS '):
        return None
    parts = line.split()
    sat = parts[1]
    tow = tow_of(parts[2:8])
    return sat, tow


def cmd_make(a):
    src = open(a.src, 'r', encoding='ascii', newline='').read().splitlines(keepends=True)
    in_header = True
    walk = {}      # sat -> (last tow, w)
    rng = {}
    rows = []
    out = []
    n_mod = 0
    for line in src:
        if in_header:
            out.append(line)
            if 'END OF HEADER' in line:
                in_header = False
            continue
        p = parse_as(line)
        if p is None or not p[0].startswith('G'):
            out.append(line)
            continue
        sat, tow = p
        if tow < a.t0 - 1e-6:
            out.append(line)
            continue
        if sat not in rng:
            rng[sat] = random.Random(f'{a.seed}-{sat}')
            walk[sat] = (a.t0, 0.0)
        t_last, w = walk[sat]
        dt = tow - t_last
        if dt > 0:
            w += rng[sat].gauss(0.0, math.sqrt(a.q * dt))
        walk[sat] = (tow, w)
        rows.append((sat, f'{tow:.1f}', f'{w:.6f}'))
        # the clock value: the first number after the 'number of values' field
        body = line.rstrip('\r\n')
        eol = line[len(body):]
        b0, b1 = value_span(body)
        v = float(body[b0:b1])
        txt = ffmt(v + w / C)
        # keep the token's end column; a longer text (a sign) eats a leading blank
        new = body[:b1 - len(txt)] + txt + body[b1:]
        assert len(new) == len(body) and new[b1 - len(txt) - 1] == ' ', line
        out.append(new + eol)
        n_mod += 1
    open(a.dst, 'w', encoding='ascii', newline='').write(''.join(out))
    with open(a.walk, 'w', newline='') as f:
        wr = csv.writer(f)
        wr.writerow(['sat', 'tow', 'w_m'])
        wr.writerows(rows)
    ws = [walk[s][1] for s in walk]
    print(f'modified {n_mod} AS records of {len(walk)} GPS satellites from TOW {a.t0}; '
          f'final |w| rms {math.sqrt(sum(x * x for x in ws) / max(1, len(ws))):.4f} m '
          f'(expected {math.sqrt(a.q * (max(walk[s][0] for s in walk) - a.t0)):.4f})')


def cmd_check(a):
    w = {}
    for r in csv.DictReader(open(a.walk)):
        w[(r['sat'], round(float(r['tow']), 1))] = float(r['w_m'])
    s = open(a.src, encoding='ascii').read().splitlines()
    d = open(a.dst, encoding='ascii').read().splitlines()
    assert len(s) == len(d), 'line count differs'
    n = bad = other = 0
    for ls, ld in zip(s, d):
        if ls == ld:
            continue
        p = parse_as(ls)
        b0, b1 = value_span(ls) if p else (0, 0)
        if p is None or ld[b1:] != ls[b1:] or ld[:b0 - 2] != ls[:b0 - 2]:
            other += 1
            continue
        dm = (float(ld.split()[9]) - float(ls.split()[9])) * C
        ww = w.get((p[0], round(p[1], 1)))
        n += 1
        if ww is None or abs(dm - ww) > 1e-3:
            bad += 1
    print(f'{n} records differ, {bad} not equal to the walk within 1 mm, {other} other lines differ')
    sys.exit(1 if (bad or other) else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['make', 'check'])
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('walk')
    ap.add_argument('--q', type=float, default=5e-6)
    ap.add_argument('--t0', type=float, default=261000.0)
    ap.add_argument('--seed', type=int, default=129)
    a = ap.parse_args()
    (cmd_make if a.cmd == 'make' else cmd_check)(a)


if __name__ == '__main__':
    main()
