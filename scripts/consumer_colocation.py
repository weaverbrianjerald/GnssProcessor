#!/usr/bin/env python3
"""Distance from the closed-loop consumer to every roster station of a network config.

    python scripts/consumer_colocation.py [<config.json>] [--radii 1,350,460] [--show N] [--site NAME=X,Y,Z]
    python scripts/consumer_colocation.py --selftest

Reads network.stations[].position_ecef (the roster) and network.closed_loop.station.position_ecef (the
consumer) from the config (default config/golden/golden_field_refound.json), or the consumer coordinate
from --site.  Prints the nearest N stations with the chord distance and the great-circle distance on a
6371 km sphere (km), then the count of roster stations within each radius (great circle).
"""
import argparse, json, math, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R_EARTH_KM = 6371.0


def distances(consumer_ecef, stations):
    """[(name, chord_km, great_circle_km)] nearest first."""
    c = consumer_ecef
    rc = math.sqrt(sum(x * x for x in c))
    out = []
    for s in stations:
        p = s['position_ecef']
        chord = math.sqrt(sum((a - b) ** 2 for a, b in zip(p, c))) / 1000.0
        rp = math.sqrt(sum(x * x for x in p))
        cosang = max(-1.0, min(1.0, sum(a * b for a, b in zip(p, c)) / (rp * rc)))
        out.append((s['name'], chord, R_EARTH_KM * math.acos(cosang)))
    return sorted(out, key=lambda t: t[1])


def selftest():
    ok = True
    st = [{'name': 'A', 'position_ecef': [6371000.0, 0.0, 0.0]},
          {'name': 'B', 'position_ecef': [0.0, 6371000.0, 0.0]},
          {'name': 'C', 'position_ecef': [6371000.0, 1.6, 0.0]}]
    d = distances([6371000.0, 0.0, 0.0], st)
    ok &= d[0][0] == 'A' and d[0][1] == 0.0
    ok &= d[1][0] == 'C' and abs(d[1][1] - 0.0016) < 1e-12
    ok &= abs(d[2][2] - R_EARTH_KM * math.pi / 2) < 1e-6
    print('selftest', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('config', nargs='?', default=os.path.join(ROOT, 'config', 'golden', 'golden_field_refound.json'))
    ap.add_argument('--radii', default='1,350,460')
    ap.add_argument('--show', type=int, default=12)
    ap.add_argument('--site', help='NAME=X,Y,Z in place of the config consumer')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    cfg = json.load(open(a.config))
    net = cfg['network']
    if a.site:
        name, xyz = a.site.split('=')
        ecef = [float(v) for v in xyz.split(',')]
    else:
        name = net['closed_loop']['station']['name']
        ecef = net['closed_loop']['station']['position_ecef']
    d = distances(ecef, net['stations'])
    print(f'{name}: {len(d)} roster stations in {os.path.basename(a.config)}')
    for n, ch, gc in d[:a.show]:
        print(f'  {n:10s} chord {ch:10.4f} km   great circle {gc:9.1f} km')
    for r in (float(x) for x in a.radii.split(',')):
        print(f'  within {r:6.0f} km: {sum(1 for _, _, gc in d if gc <= r)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
