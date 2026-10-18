#!/usr/bin/env python3
"""Broadcast GPS clock against a reference CLK product (scratchpad, card SCL pre-work).

  python scripts/clock_level/scl_brdc_clk.py stats <nav.rnx> <ref.CLK> [--t0 H] [--hours H] [--rule ttom|toc]
  python scripts/clock_level/scl_brdc_clk.py write <nav.rnx> <ref.CLK> <out.CLK> [--continuous] [--rule ttom|toc]

stats: per GPS satellite, broadcast minus reference clock (m) at the reference's epochs, with
the per-epoch median over satellites removed (the fleet-common part is absorbed by every
receiver clock); the de-commoned error's rms, its increments' rms at 1, 10, 120 and 240 epochs,
the steps at the ephemeris switches, and the same for the raw error.

write: a CLK file at the reference's epochs and satellites, values = the broadcast clock (s).
--continuous removes each switch's step (the declared re-level of an a priori cut-over), so the
a priori is continuous and its error is the within-ephemeris drift plus a change of slope.

Ephemeris rule: ttom (default) = the latest record whose transmission time is <= t (the
real-time rule); toc = the record with the nearest toc.
"""
import argparse, math, os, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rinex_ids import norm_sat  # noqa: E402

C = 299792458.0


def fnum(s):
    s = s.strip().replace('D', 'E').replace('d', 'e')
    return float(s) if s else 0.0


def gps_sow(y, mo, d, h, mi, s):
    # seconds of GPS week (no leap seconds: GPS time scale)
    import datetime
    t = datetime.datetime(y, mo, d, h, mi, 0) + datetime.timedelta(seconds=s)
    t0 = datetime.datetime(1980, 1, 6)
    dt = (t - t0).total_seconds()
    week = int(dt // 604800)
    return week, dt - week * 604800


def parse_nav(path):
    eph = {}
    with open(path, encoding='ascii', errors='replace') as f:
        lines = f.readlines()
    i = 0
    while i < len(lines) and 'END OF HEADER' not in lines[i]:
        i += 1
    i += 1
    while i < len(lines):
        ln = lines[i]
        sys_ = ln[0]
        nlines = {'G': 8, 'E': 8, 'J': 8, 'C': 8, 'I': 8, 'R': 4, 'S': 4}.get(sys_, 8)
        if sys_ != 'G':
            i += nlines
            continue
        rec = lines[i:i + 8]
        i += 8
        sat = norm_sat(ln[0:3])
        y, mo, d, h, mi, s = int(ln[4:8]), int(ln[9:11]), int(ln[12:14]), int(ln[15:17]), int(ln[18:20]), int(ln[21:23])
        wk, toc = gps_sow(y, mo, d, h, mi, s)
        af0, af1, af2 = fnum(ln[23:42]), fnum(ln[42:61]), fnum(ln[61:80])
        def fld(k, j):
            r = rec[k]
            return fnum(r[4 + 19 * j: 4 + 19 * (j + 1)])
        health = fld(6, 1)
        iodc = fld(6, 3)
        ttom = fld(7, 0)
        toe = fld(3, 0)
        eph.setdefault(sat, []).append(dict(wk=wk, toc=toc, af0=af0, af1=af1, af2=af2,
                                            health=health, iodc=iodc, ttom=ttom, toe=toe))
    for s in eph:
        eph[s].sort(key=lambda e: (e['ttom'], e['toc']))
    return eph


def parse_clk(path):
    clk = {}
    with open(path, encoding='ascii', errors='replace') as f:
        for ln in f:
            if not ln.startswith('AS G'):
                continue
            p = ln.split()
            sat = p[1]
            y, mo, d, h, mi, s = int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6]), float(p[7])
            wk, sow = gps_sow(y, mo, d, h, mi, s)
            v = float(p[9])
            clk.setdefault(sat, {})[round(sow, 3)] = v
    return clk


def pick(ephs, t, rule):
    if rule == 'toc':
        return min(ephs, key=lambda e: abs(e['toc'] - t))
    cand = [e for e in ephs if e['ttom'] <= t + 1e-6]
    return cand[-1] if cand else None


def bclk(e, t):
    dt = t - e['toc']
    return e['af0'] + e['af1'] * dt + e['af2'] * dt * dt


def series(nav, clk, rule, t0=None, t1=None):
    out = {}
    for sat, ref in clk.items():
        if sat not in nav:
            continue
        rows = []
        for t in sorted(ref):
            if t0 is not None and (t < t0 or t > t1):
                continue
            e = pick(nav[sat], t, rule)
            if e is None:
                continue
            rows.append((t, (bclk(e, t) - ref[t]) * C, e['iodc'], e['health']))
        out[sat] = rows
    return out


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else float('nan')


def stats(args):
    nav = parse_nav(args.nav)
    clk = parse_clk(args.ref)
    allt = sorted({t for s in clk for t in clk[s]})
    t0 = allt[0] + args.t0 * 3600 if args.t0 is not None else None
    t1 = t0 + args.hours * 3600 if t0 is not None else None
    ser = series(nav, clk, args.rule, t0, t1)
    # per-epoch median over healthy satellites
    by_t = {}
    for sat, rows in ser.items():
        for t, v, iod, hl in rows:
            if hl == 0:
                by_t.setdefault(t, []).append(v)
    med = {t: statistics.median(v) for t, v in by_t.items() if len(v) >= 5}
    print(f'rule={args.rule}  epochs={len(med)}  sats={len(ser)}')
    print('sat  hl  mean_raw  rms_dc   d1    d10   d120  d240   nsw  |step|max  (m; dc = de-commoned, dk = rms of k-epoch increment of dc)')
    agg = {k: [] for k in ('d1', 'd10', 'd120', 'd240')}
    steps_all = []
    for sat in sorted(ser):
        rows = [(t, v, iod, hl) for t, v, iod, hl in ser[sat] if t in med]
        if len(rows) < 250:
            continue
        hl = max(r[3] for r in rows)
        dc = [(t, v - med[t], iod) for t, v, iod, _ in rows]
        vals = [v for _, v, _ in dc]
        mu = sum(vals) / len(vals)
        dev = [v - mu for v in vals]
        tt = [t for t, _, _ in dc]
        idx = {t: i for i, t in enumerate(tt)}
        def inc(k):
            r = []
            for i, t in enumerate(tt):
                j = idx.get(t + 30.0 * k)
                if j is not None:
                    r.append(vals[j] - vals[i])
            return r
        d = {f'd{k}': rms(inc(k)) for k in (1, 10, 120, 240)}
        steps = []
        for i in range(1, len(dc)):
            if dc[i][2] != dc[i - 1][2] and abs(dc[i][0] - dc[i - 1][0] - 30) < 1:
                steps.append(abs((dc[i][1] - dc[i - 1][1])))
        if hl == 0:
            for k in agg:
                agg[k].append(d[k])
            steps_all += steps
        mraw = sum(v for _, v, _, _ in rows) / len(rows)
        print(f'{sat} {int(hl):3d} {mraw:9.3f} {rms(dev):6.3f} {d["d1"]:6.4f} {d["d10"]:6.3f} {d["d120"]:6.3f} {d["d240"]:6.3f} {len(steps):4d} {max(steps) if steps else 0:8.3f}')
    print('healthy-sat medians:', {k: round(statistics.median(v), 4) for k, v in agg.items() if v})
    if steps_all:
        s = sorted(steps_all)
        print(f'switch steps (healthy): n={len(s)} p50={s[len(s)//2]:.3f} p90={s[int(.9*len(s))]:.3f} max={s[-1]:.3f} m')


def write(args):
    nav = parse_nav(args.nav)
    clk = parse_clk(args.ref)
    ser = {}
    for sat, ref in clk.items():
        if sat not in nav:
            continue
        rows, off, prev = [], 0.0, None
        for t in sorted(ref):
            e = pick(nav[sat], t, args.rule)
            if e is None:
                continue
            v = bclk(e, t)
            if args.continuous and prev is not None and e is not prev[0]:
                off += bclk(prev[0], t) - v      # remove the switch step at t
            rows.append((t, v + off))
            prev = (e, t)
        ser[sat] = rows
    with open(args.ref, encoding='ascii', errors='replace') as f:
        lines = f.readlines()
    with open(args.out, 'w', encoding='ascii', newline='\n') as g:
        for ln in lines:
            g.write(ln)
            if 'END OF HEADER' in ln:
                break
        # one AS record per (epoch, satellite), in the reference's epoch order
        recs = []
        for sat, rows in ser.items():
            for t, v in rows:
                recs.append((t, sat, v))
        recs.sort()
        import datetime
        wk = None
        for ln in lines:
            if ln.startswith('AS G'):
                p = ln.split()
                y, mo, d, h, mi = int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6])
                wk = gps_sow(y, mo, d, h, mi, 0.0)[0]
                break
        base = datetime.datetime(1980, 1, 6) + datetime.timedelta(weeks=wk)
        for t, sat, v in recs:
            tm = base + datetime.timedelta(seconds=t)
            g.write(f'AS {sat}       {tm.year:4d} {tm.month:02d} {tm.day:02d} {tm.hour:02d} {tm.minute:02d} '
                    f'{tm.second + tm.microsecond * 1e-6:9.6f}  1   {v:19.12E}\n')
    print(f'wrote {len(recs)} records for {len(ser)} satellites to {args.out}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('stats'); a.add_argument('nav'); a.add_argument('ref')
    a.add_argument('--t0', type=float); a.add_argument('--hours', type=float, default=24)
    a.add_argument('--rule', default='ttom', choices=['ttom', 'toc'])
    b = sub.add_parser('write'); b.add_argument('nav'); b.add_argument('ref'); b.add_argument('out')
    b.add_argument('--continuous', action='store_true'); b.add_argument('--rule', default='ttom', choices=['ttom', 'toc'])
    args = ap.parse_args()
    stats(args) if args.cmd == 'stats' else write(args)
