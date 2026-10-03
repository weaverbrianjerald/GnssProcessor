#!/usr/bin/env python3
"""A broadcast-clock a priori product built on a reference CLK file (card SCL pre-work).

    python scripts/clock_level/scl_brdc_apriori.py make <src_clk> <nav.rnx> <dst_clk> <err_csv> [--continuous] [--drop G01] [--rule ttom]

Every GPS 'AS' record's first clock value is replaced by the broadcast clock at that epoch
(af0 + af1 dt + af2 dt^2 of the ephemeris selected by --rule: ttom = the latest record whose
transmission time is <= t, the real-time rule). --continuous removes each ephemeris switch's
step at the switch epoch (a declared a priori re-level), so the a priori is continuous.
Satellites in --drop lose their AS records (a broadcast-unhealthy satellite a real-time producer
would not use). Every other byte of the file is copied (g3_walk.py's formatting). The error
e = a priori - source (m) is written to err_csv (sat, tow, e_m) for the truth grade on the sim.
"""
import argparse, csv, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scl_brdc_clk import parse_nav, pick, bclk          # noqa: E402
from g3_walk import ffmt, value_span, parse_as      # noqa: E402

C = 299792458.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['make'])
    ap.add_argument('src'); ap.add_argument('nav'); ap.add_argument('dst'); ap.add_argument('err')
    ap.add_argument('--continuous', action='store_true')
    ap.add_argument('--drop', default='G01')
    ap.add_argument('--rule', default='ttom', choices=['ttom', 'toc'])
    a = ap.parse_args()
    drop = set(s for s in a.drop.split(',') if s)
    nav = parse_nav(a.nav)
    src = open(a.src, 'r', encoding='ascii', newline='').read().splitlines(keepends=True)
    out, rows, in_header = [], [], True
    state = {}          # sat -> (eph, offset)
    n_mod = n_drop = n_noeph = 0
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
        if sat in drop:
            n_drop += 1
            continue
        e = pick(nav.get(sat, []), tow, a.rule) if sat in nav else None
        if e is None:
            n_noeph += 1
            continue
        prev, off = state.get(sat, (None, 0.0))
        v = bclk(e, tow)
        if a.continuous and prev is not None and e is not prev:
            off += bclk(prev, tow) - v
        state[sat] = (e, off)
        vb = v + off
        body = line.rstrip('\r\n')
        eol = line[len(body):]
        b0, b1 = value_span(body)
        v0 = float(body[b0:b1])
        txt = ffmt(vb)
        new = body[:b1 - len(txt)] + txt + body[b1:]
        assert len(new) == len(body) and new[b1 - len(txt) - 1] == ' ', line
        out.append(new + eol)
        rows.append((sat, f'{tow:.1f}', f'{(vb - v0) * C:.6f}'))
        n_mod += 1
    open(a.dst, 'w', encoding='ascii', newline='').write(''.join(out))
    with open(a.err, 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['sat', 'tow', 'e_m']); w.writerows(rows)
    print(f'{n_mod} GPS records replaced, {n_drop} dropped ({",".join(sorted(drop))}), '
          f'{n_noeph} without an ephemeris dropped; continuous={a.continuous} rule={a.rule}')


if __name__ == '__main__':
    main()
