#!/usr/bin/env python3
"""Card SCL2: matched-walk a priori beds (the a priori error a known random walk, plus optional white noise).

    python scripts/clock_level/scl2_rw_bed.py <src_root> <dst_root> <walk_csv> --q 5e-6 --seed S [--white SD_M | --white-file sat_sd.csv] [--wseed S2]

Every GPS 'AS' record of the three CLK files of src_root (2024-128/129/130, in time order) gets
w_s(t) = a seeded random walk (increment N(0, q*dt), dt the time since that satellite's previous
record; 0 at the satellite's first record of 2024-128; continuous across the day files), plus an
optional independent white term per record (N(0, sd), sd uniform or per satellite). The value written
is COD + w/c (g3_walk.py's formatting: the token keeps its end column; every other byte copied).
w (walk + white, metres) is written to walk_csv as (sat, tow, w_m) for every record of all three days.
The day directories are copied file by file; the 'static' junction is NOT created here.
"""
import argparse, csv, math, os, random, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from g3_walk import ffmt, value_span, parse_as   # noqa: E402
C = 299792458.0
DAYS = ['2024-128', '2024-129', '2024-130']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('walk')
    ap.add_argument('--q', type=float, default=5e-6)
    ap.add_argument('--seed', default='scl2rw')
    ap.add_argument('--white', type=float, default=0.0)
    ap.add_argument('--white-file', default='')
    ap.add_argument('--wseed', default='scl2rww')
    a = ap.parse_args()
    wsd = {}
    if a.white_file:
        for r in csv.DictReader(open(a.white_file)):
            wsd[r['sat']] = float(r['sd_m'])
    state, rng, rngw, rows = {}, {}, {}, []
    n_mod = 0
    for day in DAYS:
        sd_ = os.path.join(a.src, day, 'products'); dd_ = os.path.join(a.dst, day, 'products')
        os.makedirs(dd_, exist_ok=True)
        for fn in sorted(os.listdir(sd_)):
            sp, dp = os.path.join(sd_, fn), os.path.join(dd_, fn)
            if not fn.endswith('_CLK.CLK'):
                shutil.copy2(sp, dp); continue
            src = open(sp, 'r', encoding='ascii', newline='').read().splitlines(keepends=True)
            out, in_header = [], True
            for line in src:
                if in_header:
                    out.append(line)
                    if 'END OF HEADER' in line:
                        in_header = False
                    continue
                p = parse_as(line)
                if p is None or not p[0].startswith('G'):
                    out.append(line); continue
                sat, tow = p
                if sat not in rng:
                    rng[sat] = random.Random(f'{a.seed}-{sat}')
                    rngw[sat] = random.Random(f'{a.wseed}-{sat}')
                    state[sat] = (tow, 0.0)
                t_last, w = state[sat]
                dt = tow - t_last
                assert dt >= 0, (sat, tow, t_last)
                if dt > 0:
                    w += rng[sat].gauss(0.0, math.sqrt(a.q * dt))
                state[sat] = (tow, w)
                sdw = wsd.get(sat, a.white) if a.white_file else a.white
                wt = w + (rngw[sat].gauss(0.0, sdw) if sdw > 0 else 0.0)
                body = line.rstrip('\r\n'); eol = line[len(body):]
                b0, b1 = value_span(body)
                v = float(body[b0:b1])
                txt = ffmt(v + wt / C)
                new = body[:b1 - len(txt)] + txt + body[b1:]
                assert len(new) == len(body) and new[b1 - len(txt) - 1] == ' ', line
                out.append(new + eol)
                rows.append((sat, f'{tow:.1f}', f'{wt:.6f}'))
                n_mod += 1
            open(dp, 'w', encoding='ascii', newline='').write(''.join(out))
    with open(a.walk, 'w', newline='') as f:
        wr = csv.writer(f); wr.writerow(['sat', 'tow', 'w_m']); wr.writerows(rows)
    print(f'{n_mod} GPS AS records modified, {len(state)} satellites, q {a.q}, white '
          f'{"per-sat file " + a.white_file if a.white_file else a.white}')


if __name__ == '__main__':
    main()
