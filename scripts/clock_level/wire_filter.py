#!/usr/bin/env python3
"""A filtered copy of a network wire for consumer-only replays (card L5B, the L5 return plan's R-2 variant (i)):
the B (code-bias) records of the listed constellation and bands are removed, everything else is copied byte for
byte, so a census replay of the copy grades "the band is withheld from the wire" with no producer run.  The
replay reproduces the in-process store exactly (NetworkProductIO), and a uniform withhold has no edge at the
consumer (a first sighting is not a freshness flip).
    python scripts/clock_level/wire_filter.py <wire run dir> <new run dir> --drop-b G:5 [--drop-b E:7]
The new run dir receives products.txt (filtered) and the wire's arm.json (so census tools can build from it)."""
import argparse, os, shutil


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--drop-b', action='append', required=True,
                    help='<constellation letter>:<band digit>, e.g. G:5 (repeatable)')
    a = ap.parse_args()
    drop = {tuple(x.split(':')) for x in a.drop_b}
    os.makedirs(a.dst, exist_ok=True)
    kept = removed = 0
    with open(os.path.join(a.src, 'products.txt'), 'rb') as fi, open(os.path.join(a.dst, 'products.txt'), 'wb') as fo:
        for line in fi:
            if line.startswith(b'B,'):
                f = line.split(b',', 3)
                sat, sig = f[1].decode(), f[2].decode()
                if (sat[:1], sig[1:2]) in drop:
                    removed += 1
                    continue
            fo.write(line)
            kept += 1
    shutil.copy(os.path.join(a.src, 'arm.json'), os.path.join(a.dst, 'arm.json'))
    print(f'{a.dst}: {removed} B records removed, {kept} lines kept')


if __name__ == '__main__':
    main()
