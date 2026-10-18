#!/usr/bin/env python3
"""
Reference parser for RINEX 2.xx mixed observation files.

Mirrors the algorithm implemented in src/core/rinex/RinexParser.cpp to sanity-check
expected outputs (epoch counts, sat counts per epoch, the observation values for a
known satellite) against the real cit11290.24o test fixture without spinning up the
full MSVC build.

Run:
    python scripts/verify_rinex2_parser.py [path/to/file.YYo]

Exits non-zero if anything looks structurally wrong.
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterator


def _take_int(line: str, off: int, length: int):
    s = line[off:off + length].strip()
    if not s:
        return None
    try:
        return int(s)
    except ValueError:
        return None


def _take_float(line: str, off: int, length: int):
    s = line[off:off + length].strip()
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _translate_v2_code(v2: str, sys_letter: str) -> str:
    """Mirror translateV2ObsCodeToV3 in RinexParser.cpp."""
    if len(v2) != 2:
        return v2
    obs_type, freq = v2[0], v2[1]
    if obs_type == 'P':
        attr = 'P' if sys_letter == 'R' else 'W'
        return 'C' + freq + attr
    if obs_type not in 'CLDS':
        return v2
    if sys_letter == 'G':
        # GPS L2 disambiguation: C2 is the L2C-L civilian code, while P2 is L2 P(Y)
        # Z-tracking. L/D/S on L2 stay paired with the Z-tracked phase so the
        # ionosphere-free combination uses (L2W, C2W) and the C2 entry stays distinct.
        if obs_type == 'C':
            attr = {'1': 'C', '2': 'L', '5': 'Q'}.get(freq, 'C')
        else:
            attr = {'1': 'C', '2': 'W', '5': 'Q'}.get(freq, 'C')
    elif sys_letter == 'R':
        attr = 'I' if freq == '3' else 'C'
    elif sys_letter == 'E':
        attr = 'X'
    elif sys_letter == 'C':
        attr = 'I'
    elif sys_letter == 'J':
        attr = 'C' if freq == '1' else 'X'
    elif sys_letter == 'S':
        attr = 'I' if freq == '5' else 'C'
    else:
        attr = 'C'
    return obs_type + freq + attr


def _normalize_sat(raw: str):
    if len(raw) < 3:
        return None
    a, b, c = raw[0], raw[1], raw[2]
    if a == ' ':
        a = 'G'
    if b == ' ':
        b = '0'
    if c == ' ':
        return None
    return a + b + c


def parse_rinex2(path: Path) -> dict:
    text = path.read_text(errors='replace')
    lines = text.splitlines()
    it: Iterator[str] = iter(lines)

    header = {
        'version': None,
        'marker': '',
        'antenna': '',
        'time_system': 'GPS',
        'obs_types': [],
        'interval': None,
    }

    # ------------------------------------------------------------------ header
    while True:
        try:
            line = next(it)
        except StopIteration:
            raise RuntimeError('EOF before END OF HEADER')
        if 'END OF HEADER' in line:
            break
        label = line[60:] if len(line) >= 60 else ''
        if 'RINEX VERSION / TYPE' in label:
            header['version'] = float(line[:9].strip())
        elif 'MARKER NAME' in label:
            header['marker'] = line[:60].strip()
        elif 'ANT # / TYPE' in label:
            header['antenna'] = line[20:40].strip()
        elif 'INTERVAL' in label:
            header['interval'] = float(line[:10].strip())
        elif 'TIME OF FIRST OBS' in label:
            ts = line[48:51].strip()
            if ts:
                header['time_system'] = ts
        elif '# / TYPES OF OBSERV' in label:
            num = int(line[:6].strip())
            codes = []

            def extract(src):
                off = 6
                while off + 6 <= 60 and len(codes) < num:
                    if off + 6 > len(src):
                        break
                    code = src[off + 4:off + 6].strip()
                    if len(code) == 2:
                        codes.append(code)
                    off += 6

            extract(line)
            while len(codes) < num:
                cont = next(it)
                if '# / TYPES OF OBSERV' not in (cont[60:] if len(cont) >= 60 else ''):
                    raise RuntimeError('Bad continuation: ' + repr(cont))
                extract(cont)
            header['obs_types'] = codes

    if header['version'] is None or header['version'] >= 3.0:
        raise RuntimeError('Not a RINEX 2.x file (version=' + repr(header['version']) + ')')
    if not header['obs_types']:
        raise RuntimeError('No # / TYPES OF OBSERV in header')

    n_types = len(header['obs_types'])
    lines_per_sat = (n_types + 4) // 5

    # ----------------------------------------------------- observation epochs
    epochs = []
    pending = None  # Carried-over line if the inner loop reads ahead.

    def next_line():
        nonlocal pending
        if pending is not None:
            v = pending
            pending = None
            return v
        try:
            return next(it)
        except StopIteration:
            return None

    while True:
        line = next_line()
        if line is None:
            break
        if len(line) < 32:
            continue
        if not line[1:3].isdigit():
            continue

        year = _take_int(line, 1, 2)
        month = _take_int(line, 4, 2)
        day = _take_int(line, 7, 2)
        hour = _take_int(line, 10, 2)
        minute = _take_int(line, 13, 2)
        sec = _take_float(line, 15, 11)
        if None in (year, month, day, hour, minute, sec):
            continue
        flag = _take_int(line, 28, 1) or 0
        nrec = _take_int(line, 29, 3) or 0

        if 2 <= flag <= 5:
            for _ in range(nrec):
                next_line()
            continue
        if flag not in (0, 1):
            continue

        year = year + 2000 if year < 80 else year + 1900

        sat_ids = []
        # First-line slots
        pos = 32
        while pos + 3 <= len(line) and len(sat_ids) < nrec:
            slot = line[pos:pos + 3]
            if slot.strip() == '':
                break
            sid = _normalize_sat(slot)
            if sid is not None:
                sat_ids.append(sid)
            pos += 3
        # Continuation lines
        while len(sat_ids) < nrec:
            cont = next_line()
            if cont is None:
                break
            if len(cont) < 32:
                cont = cont + ' ' * (32 - len(cont))
            pos = 32
            while pos + 3 <= len(cont) and len(sat_ids) < nrec:
                slot = cont[pos:pos + 3]
                if slot.strip() == '':
                    break
                sid = _normalize_sat(slot)
                if sid is not None:
                    sat_ids.append(sid)
                pos += 3

        # Per-sat obs lines
        ep_data = {}
        for sid in sat_ids:
            buf = ''
            for _ in range(lines_per_sat):
                ol = next_line()
                if ol is None:
                    break
                ol = ol.rstrip('\r')
                if len(ol) < 80:
                    ol = ol + ' ' * (80 - len(ol))
                buf += ol
            obs = {}
            for j, code in enumerate(header['obs_types']):
                base = (j // 5) * 80 + (j % 5) * 16
                if base + 16 > len(buf):
                    break
                value_str = buf[base:base + 14].strip()
                if not value_str:
                    continue
                try:
                    val = float(value_str)
                except ValueError:
                    continue
                if val == 0.0:
                    continue
                obs[_translate_v2_code(code, sid[0])] = val
            if obs:
                ep_data[sid] = obs

        epochs.append({
            'time': (year, month, day, hour, minute, sec, flag),
            'sat_count_declared': nrec,
            'sat_count_recovered': len(sat_ids),
            'obs': ep_data,
        })

    return {'header': header, 'epochs': epochs}


def main(argv):
    default = Path(__file__).resolve().parents[1] / 'data' / 'products' / 'rinex' / 'unzip' / 'cit11290.24o'
    target = Path(argv[1]) if len(argv) > 1 else default
    if not target.exists():
        print('ERROR: file not found: ' + str(target), file=sys.stderr)
        return 2

    print('Parsing: ' + str(target))
    result = parse_rinex2(target)
    h = result['header']
    print('  version       = ' + str(h['version']))
    print('  marker        = ' + h['marker'])
    print('  antenna       = ' + h['antenna'])
    print('  time system   = ' + h['time_system'])
    print('  interval (s)  = ' + str(h['interval']))
    print('  obs types ({})'.format(len(h['obs_types'])) + ' = ' + str(h['obs_types']))

    eps = result['epochs']
    print('  epochs        = ' + str(len(eps)))
    if not eps:
        print('FAIL: no epochs parsed')
        return 1

    first = eps[0]
    print('  first epoch   = {}, declared {} sats, recovered {}'.format(
        first['time'], first['sat_count_declared'], first['sat_count_recovered']))

    # Spot-check satellite G23 in the first epoch.
    g23 = first['obs'].get('G23')
    if g23 is None:
        print('FAIL: G23 missing from first epoch')
        return 1
    print('  G23 obs       = ' + str(g23))

    # Expected values for G23 in epoch 1 of cit11290.24o:
    #   L1=107456806.465 (-> L1C), L2=83732660.354 (-> L2W),
    #   C1=20448290.422 (-> C1C), P2=20448298.406 (-> C2W, L2 P(Y) Z-tracking),
    #   S1=45.300 (-> S1C), S2=44.900 (-> S2W),
    #   C2=20448298.547 (-> C2L, L2C-L civilian), L5=80243826.296 (-> L5Q),
    #   C5=20448301.359 (-> C5Q), S5=54.700 (-> S5Q).
    # P1 (5th slot on line 1) is blank in the file -> not present in output.
    expected = [
        ('L1C', 107456806.465),
        ('L2W', 83732660.354),
        ('C1C', 20448290.422),
        ('C2W', 20448298.406),
        ('C2L', 20448298.547),
        ('S1C', 45.3),
        ('S2W', 44.9),
        ('L5Q', 80243826.296),
        ('C5Q', 20448301.359),
        ('S5Q', 54.7),
    ]
    fail = 0
    for code, want in expected:
        got = g23.get(code)
        ok = got is not None and abs(got - want) < 1e-3
        flag = 'OK ' if ok else 'BAD'
        print('    [{}] {} expected {}, got {}'.format(flag, code, want, got))
        if not ok:
            fail += 1

    # All recovered sats should equal declared count on a well-formed file.
    drops = sum(1 for e in eps if e['sat_count_declared'] != e['sat_count_recovered'])
    if drops:
        print('WARN: {} epochs lost satellites in continuation parsing'.format(drops))

    if fail:
        print('FAIL: {} value mismatches'.format(fail))
        return 1
    print('PASS: parser logic produces expected values for cit11290.24o')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
