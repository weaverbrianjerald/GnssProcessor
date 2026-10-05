"""Derive the field golden for another day of 2024 (the multi-day replay).

Every string naming DOY 129 (2024129 in RINEX long names, 2024-129 in data paths) is moved to the
target day, the station and consumer RINEX paths are rooted at --obs-root (default: the repo's data/;
the multi-day observations live in D:/GnssProcessorSupport/data_mday), data_download.doy is set, and
the stations whose RINEX is missing that day are dropped.  The products still resolve under
files.internal_data_root.  The consumer (closed_loop.station) must exist; the script refuses otherwise.

    python scripts/mday_golden_config.py <out dir> <doy> [<doy> ...] [--obs-root D:/GnssProcessorSupport/data_mday]
"""
import json, os, sys

SRC = 'C:/GnssProcessor/config/golden/golden_field_refound.json'


def move(o, doy):
    if isinstance(o, dict):
        return {k: move(v, doy) for k, v in o.items()}
    if isinstance(o, list):
        return [move(v, doy) for v in o]
    if isinstance(o, str):
        return o.replace('2024129', f'2024{doy:03d}').replace('2024-129', f'2024-{doy:03d}')
    return o


args = sys.argv[1:]
root = 'C:/GnssProcessor/data'
if '--obs-root' in args:
    i = args.index('--obs-root'); root = args[i + 1]; del args[i:i + 2]
OUT, days = args[0], [int(x) for x in args[1:]]
for doy in days:
    g = move(json.load(open(SRC)), doy)
    g = json.loads(json.dumps(g).replace(f'C:/GnssProcessor/data/2024-{doy:03d}/obs/', f'{root}/2024-{doy:03d}/obs/'))
    g['data_download']['doy'] = doy
    net = g['network']
    keep = [s for s in net['stations'] if os.path.exists(s['rinex_path'])]
    dropped = [s['name'] for s in net['stations'] if not os.path.exists(s['rinex_path'])]
    net['stations'] = keep
    cons = net['closed_loop']['station']['rinex_path']
    assert os.path.exists(cons), f'DOY {doy}: consumer RINEX missing: {cons}'
    assert os.path.exists(g['files']['observation_path']), g['files']['observation_path']
    if net.get('clock_datum_station') and net['clock_datum_station'] not in {s['name'] for s in keep}:
        print(f'DOY {doy}: WARNING clock_datum_station {net["clock_datum_station"]} dropped')
    assert '2024129' not in json.dumps(g) and '2024-129' not in json.dumps(g)
    p = f'{OUT}/golden_field_doy{doy}.json'
    json.dump(g, open(p, 'w'), indent=2)
    print(f'DOY {doy}: {len(keep)} stations, dropped {dropped} -> {p}')
