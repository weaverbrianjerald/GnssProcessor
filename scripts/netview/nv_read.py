"""Readers of the network traces the viewer draws from.

Each reader streams one file of a run folder and returns plain Python values; a missing file returns None, so the
page can say what is absent and which key writes it.  No reader writes anything.

Times are kept as integer milliseconds of GPS time (week * 604800 s + tow), so runs that cross a week boundary and
traces with different tow formats meet on one key.

Parsers adapted from the analysis tools of the network integer fixing work (presentation/agu26/fixing_control/tools:
nw_depth.py for the tree, o2_audit.py for the held whole numbers, stream_jumps.py for the correction level).
"""
from __future__ import annotations

import csv
import math
from pathlib import Path

C_M_S = 299792458.0
WEEK_MS = 604800 * 1000

#: Carrier frequency (MHz) by constellation letter and the band digit of a RINEX signal name (L1X -> '1').
FREQ_MHZ = {
    'G': {'1': 1575.42, '2': 1227.60, '5': 1176.45},
    'E': {'1': 1575.42, '5': 1176.45, '7': 1207.14, '6': 1278.75, '8': 1191.795},
    'C': {'1': 1575.42, '2': 1561.098, '5': 1176.45, '6': 1268.52, '7': 1207.14},
    'J': {'1': 1575.42, '2': 1227.60, '5': 1176.45, '6': 1278.75},
}

#: Long names used in a few traces, mapped to the constellation letter.
SYSTEM_LETTER = {'GPS': 'G', 'Galileo': 'E', 'BeiDou': 'C', 'QZSS': 'J', 'GLONASS': 'R'}


def freq_mhz(system: str, signal: str) -> float:
    """Carrier frequency of a signal such as ('E', 'L5X'); NaN when unknown."""
    return FREQ_MHZ.get(system, {}).get(signal[1:2], math.nan)


def gps_ms(week: str, tow: str) -> int:
    """GPS time in integer milliseconds from the week and tow fields of a trace row."""
    return int(week) * WEEK_MS + int(round(float(tow) * 1000.0))


def _rows(path: Path):
    """csv rows of a trace, comment lines (#) skipped; yields (header index, row iterator)."""
    f = open(path, newline='', encoding='utf-8', errors='replace')
    rd = csv.reader(line for line in f if not line.startswith('#'))
    try:
        hdr = next(rd)
    except StopIteration:
        f.close()
        return {}, iter(())
    return {name: i for i, name in enumerate(hdr)}, _closing(rd, f)


def _closing(rd, f):
    try:
        yield from rd
    finally:
        f.close()


def _float(s: str) -> float:
    try:
        return float(s) if s != '' else math.nan
    except ValueError:
        return math.nan


def _int(s: str):
    try:
        return int(s) if s != '' else None
    except ValueError:
        try:
            return int(round(float(s)))
        except ValueError:
            return None


def is_satellite(name: str) -> bool:
    """A satellite identifier such as E07 or G12 (a constellation letter and two digits)."""
    return len(name) == 3 and name[0] in 'GERCJIS' and name[1:].isdigit()


# --------------------------------------------------------------------------------------------------------------- #
# receivers


def ecef_to_geodetic(x: float, y: float, z: float):
    """WGS84 latitude and longitude (degrees) and height (m) of an ECEF position (Bowring's closed form)."""
    a, f = 6378137.0, 1.0 / 298.257223563
    b = a * (1.0 - f)
    e2 = f * (2.0 - f)
    ep2 = (a * a - b * b) / (b * b)
    p = math.hypot(x, y)
    if p < 1e-6:
        return (90.0 if z >= 0 else -90.0), 0.0, abs(z) - b
    th = math.atan2(z * a, p * b)
    lat = math.atan2(z + ep2 * b * math.sin(th) ** 3, p - e2 * a * math.cos(th) ** 3)
    lon = math.atan2(y, x)
    n = a / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
    h = p / math.cos(lat) - n
    return math.degrees(lat), math.degrees(lon), h


def read_stations(logs: Path):
    """Receivers of the run (network_stations.csv): name, role, ECEF, latitude, longitude, in file order."""
    p = logs / 'network_stations.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    out = []
    for r in rows:
        if len(r) < len(ix):
            continue
        x, y, z = (_float(r[ix[k]]) for k in ('ecef_x', 'ecef_y', 'ecef_z'))
        lat, lon, _h = ecef_to_geodetic(x, y, z) if all(map(math.isfinite, (x, y, z))) else (math.nan, math.nan, 0)
        out.append({'name': r[ix['station']], 'role': r[ix['role']], 'ecef': (x, y, z), 'lat': lat, 'lon': lon})
    return out


# --------------------------------------------------------------------------------------------------------------- #
# the tree of each signal (network_chain_edges.csv)


def read_tree(logs: Path):
    """The chain's tree per signal and epoch.

    Returns (rows, bands): rows is a list of tuples
        (t_ms, band_key, child, parent, depth, target_cyc or None, row kind, cause, comp, root, edge_sta, edge_sat)
    in file order, band_key is 'E:L1X'; bands is the ordered set of band keys.  The reference receiver (root) of a
    component has no row of its own: it is the 'root' field of its component's rows.
    """
    p = logs / 'network_chain_edges.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    need = ('week', 'tow', 'system', 'band', 'child', 'parent', 'depth')
    if any(k not in ix for k in need):
        return None
    iw, it, isy, ib, ich, ipa, idp = (ix[k] for k in need)
    itc = ix.get('target_cyc')
    irow = ix.get('row')
    icause = ix.get('legacy_cause')
    icomp = ix.get('comp')
    ir1 = ix.get('r1')
    ies, iet = ix.get('edge_sta'), ix.get('edge_sat')
    out, bands = [], {}
    width = max(ix.values()) + 1
    for r in rows:
        if len(r) < width:
            continue                                   # a row cut short (the run was still writing)
        key = f'{r[isy]}:{r[ib]}'
        bands.setdefault(key, None)
        tc = _float(r[itc]) if itc is not None else math.nan
        comp = (_int(r[icomp]) or 0) if icomp is not None else 0
        out.append((gps_ms(r[iw], r[it]), key, r[ich], r[ipa], int(r[idp]),
                    int(round(tc)) if math.isfinite(tc) else None,
                    r[irow] if irow is not None else '', r[icause] if icause is not None else '',
                    comp, r[ir1] if ir1 is not None else '',
                    r[ies] if ies is not None else '', r[iet] if iet is not None else ''))
    return out, list(bands)


# --------------------------------------------------------------------------------------------------------------- #
# the links that are not tree links (network_chain_frame.csv) and the fixing decisions (network_ring_fix.csv)


def family_bands(family: str):
    """'E|L1X+L5X' -> ('E', 'L1X', 'L5X')."""
    sy, sigs = family.split('|', 1)
    s1, s2 = sigs.split('+', 1)
    return sy, s1, s2


def read_chords(logs: Path):
    """Every link of a two-signal family that is not a tree link of either signal, per epoch.

    Returns a list of tuples (t_ms, family, station, satellite, f1, f2, q11, q12, q22, lattice int 1, lattice int 2)
    with the floats in cycles of each signal and their 2x2 covariance (cycles squared).
    """
    p = logs / 'network_chain_frame.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    need = ('week', 'tow', 'family', 'arc_sta', 'arc_sat', 'float_cyc_1', 'float_cyc_2', 'q11', 'q12', 'q22')
    if any(k not in ix for k in need):
        return None
    iw, it, ifa, ist, isa, if1, if2, i11, i12, i22 = (ix[k] for k in need)
    ii1, ii2 = ix.get('int_1'), ix.get('int_2')
    width = max(ix.values()) + 1
    out = []
    for r in rows:
        if len(r) < width:
            continue
        out.append((gps_ms(r[iw], r[it]), r[ifa], r[ist], r[isa], _float(r[if1]), _float(r[if2]),
                    _float(r[i11]), _float(r[i12]), _float(r[i22]),
                    _int(r[ii1]) if ii1 is not None else None, _int(r[ii2]) if ii2 is not None else None))
    return out


#: The fixing states of a link that is not a tree link, as codes stored in the page (0 = float, no fixing).
STATE_CODES = {
    'float': 0, 'not_candidate': 1, 'no_float': 2, 'window': 3, 'sigma': 4, 'fraction': 5, 'lattice': 6,
    'persist': 7, 'odds': 8, 'cap': 9, 'wl_fraction': 10, 'code_event': 11, 'refused': 12, 'decided': 13,
    'held': 14, 'would_hold': 15,
}


def read_ring_fix(logs: Path):
    """The fixing decision of every candidate link per epoch.

    Returns a dict keyed (t_ms, family, station, satellite) -> (state code, ring, n1, n2, wide-lane integer,
    n_if float, sigma_if, resid_if_m, audit) where family is 'E|L1X+L5X'.  The audit is '', 'right', 'wrong' or
    'unlisted' (only a simulation run with a truth root can say right or wrong).
    """
    p = logs / 'network_ring_fix.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    need = ('week', 'tow', 'station', 'sat', 'sig1', 'sig2', 'state')
    if any(k not in ix for k in need):
        return None
    iw, it, ist, isa, is1, is2, ista = (ix[k] for k in need)
    g = ix.get
    iref, iring, in1, in2, iwl, inif, isif, ires, iaud = (g(k) for k in ('refused', 'ring', 'n1', 'n2', 'wl', 'n_if',
                                                                         'sigma_if', 'resid_if_m', 'audit'))
    width = max(ix.values()) + 1
    out = {}
    for r in rows:
        if len(r) < width:
            continue
        sat = r[isa]
        fam = f'{sat[0]}|{r[is1]}+{r[is2]}'
        state = r[ista]
        if state == 'refused':
            code = STATE_CODES.get(r[iref] if iref is not None else '', STATE_CODES['refused'])
        else:
            code = STATE_CODES.get(state, STATE_CODES['refused'])
        out[(gps_ms(r[iw], r[it]), fam, r[ist], sat)] = (
            code, _int(r[iring]) if iring is not None else None,
            _int(r[in1]) if in1 is not None else None, _int(r[in2]) if in2 is not None else None,
            _int(r[iwl]) if iwl is not None else None,
            _float(r[inif]) if inif is not None else math.nan, _float(r[isif]) if isif is not None else math.nan,
            _float(r[ires]) if ires is not None else math.nan, (r[iaud] if iaud is not None else '').lower())
    return out


# --------------------------------------------------------------------------------------------------------------- #
# events and alarms


def read_events(logs: Path, drop_tiers=('PRIORRES',)):
    """network_events.csv rows as tuples (t_ms, tier, event, sat, station, statistic, threshold, action, detail).

    Tiers in drop_tiers are counted, not kept (PRIORRES writes a diagnostic row per prior and epoch).
    Returns (rows, dropped counts by tier).
    """
    p = logs / 'network_events.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    need = ('week', 'tow', 'tier', 'event', 'sat', 'station', 'statistic', 'threshold', 'action', 'detail')
    if any(k not in ix for k in need):
        return None
    iw, it, iti, iev, isa, ist, istat, ithr, iac, ide = (ix[k] for k in need)
    out, dropped = [], {}
    for r in rows:
        if len(r) < 10:
            continue
        tier = r[iti]
        if tier in drop_tiers:
            dropped[tier] = dropped.get(tier, 0) + 1
            continue
        # the detail is the last column; a stray comma inside it would split it
        detail = ','.join(r[ide:]) if len(r) > ide + 1 else r[ide]
        out.append((gps_ms(r[iw], r[it]), tier, r[iev], r[isa], r[ist], _float(r[istat]), _float(r[ithr]),
                    r[iac], detail))
    return out, dropped


def read_raim(logs: Path):
    """network_raim.csv rows (t_ms, sat, station, normalized statistic, refloated) or None."""
    p = logs / 'network_raim.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    if 'tow' not in ix:
        return None
    out = []
    for r in rows:
        if len(r) < len(ix):
            continue
        out.append((gps_ms(r[ix['week']], r[ix['tow']]), r[ix.get('sat', 2)], r[ix.get('station', 3)],
                    _float(r[ix['s_max_norm']]) if 's_max_norm' in ix else math.nan,
                    (_int(r[ix['refloated']]) or 0) if 'refloated' in ix else 0))
    return out


# --------------------------------------------------------------------------------------------------------------- #
# lines of sight (network_ionosphere.csv): elevation and azimuth of every receiver-satellite pair


def read_lines_of_sight(logs: Path):
    """Elevation and azimuth (degrees) per (t_ms, station, sat) from network_ionosphere.csv, as parallel lists
    (t_ms, station, sat, elevation, azimuth), or None."""
    p = logs / 'network_ionosphere.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    need = ('week', 'tow', 'station', 'sat', 'elevation_deg', 'azimuth_deg')
    if any(k not in ix for k in need):
        return None
    iw, it, ist, isa, iel, iaz = (ix[k] for k in need)
    width = max(ix.values()) + 1
    t, sta, sat, el, az = [], [], [], [], []
    for r in rows:
        if len(r) < width:
            continue
        t.append(gps_ms(r[iw], r[it]))
        sta.append(r[ist])
        sat.append(r[isa])
        el.append(_float(r[iel]))
        az.append(_float(r[iaz]))
    return t, sta, sat, el, az


# --------------------------------------------------------------------------------------------------------------- #
# receiver states


def read_zwd(logs: Path):
    """Wet delay per receiver and epoch: {(t_ms, station): (zwd_m, std_m)} or None."""
    p = logs / 'network_zwd.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    out = {}
    for r in rows:
        if len(r) < 5:
            continue
        out[(gps_ms(r[ix['week']], r[ix['tow']]), r[ix['station']])] = (_float(r[ix['zwd_m']]), _float(r[ix['std_m']]))
    return out


def read_rx_clock(logs: Path):
    """Receiver clock per receiver and epoch (network_rx_states.csv kind CLK): {(t_ms, station): (m, std m)}."""
    p = logs / 'network_rx_states.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    ik, iv, isd = ix['kind'], ix['value_m'], ix['std_m']
    out = {}
    for r in rows:
        if len(r) < 7 or r[ik] != 'CLK':
            continue
        out[(gps_ms(r[ix['week']], r[ix['tow']]), r[ix['station']])] = (_float(r[iv]), _float(r[isd]))
    return out


def read_rpb(logs: Path):
    """Receiver phase bias per receiver, signal and epoch (network_rpb_states.csv).

    Returns {(t_ms, station, 'E:L1X'): (value_m, sigma_m, pinned)} or None.
    """
    p = logs / 'network_rpb_states.csv'
    if not p.exists():
        return None
    ix, rows = _rows(p)
    out = {}
    for r in rows:
        if len(r) < 8:
            continue
        sy = SYSTEM_LETTER.get(r[ix['system']], r[ix['system']][:1])
        out[(gps_ms(r[ix['week']], r[ix['tow']]), r[ix['station']], f'{sy}:{r[ix["signal"]]}')] = (
            _float(r[ix['value_m']]), _float(r[ix['sigma_m']]), _int(r[ix['pinned']]) or 0)
    return out


# --------------------------------------------------------------------------------------------------------------- #
# the emitted corrections (products.txt)


def read_products(run: Path):
    """The emitted corrections per epoch (products.txt).

    Returns (epochs, header) where epochs is an ordered list of (t_ms, clocks, code biases, phase biases, status):
    clocks {sat: m}, code biases {(sat, 'C1X'): m}, phase biases {(sat, 'L1X'): m}, status the H record's
    (status, sigma0) or None.  header is the S record's fields (reference receiver first), or None.
    """
    p = run / 'products.txt'
    if not p.exists():
        return None
    epochs = []
    header = None
    cur = None
    status = None
    with open(p, encoding='utf-8', errors='replace') as f:
        for line in f:
            k = line[:2]
            if k == 'E,':
                q = line.rstrip().split(',')
                cur = (gps_ms(q[1], q[2]), {}, {}, {}, status)
                epochs.append(cur)
                status = None
            elif k == 'H,':
                q = line.rstrip().split(',')
                status = (q[3], _float(q[4])) if len(q) > 4 else None
            elif cur is None:
                if k == 'S,':
                    header = line.rstrip().split(',')[1:]
            elif k == 'C,':
                q = line.rstrip().split(',')
                cur[1][q[1]] = float(q[2])
            elif k == 'B,':
                q = line.rstrip().split(',')
                cur[2][(q[1], q[2])] = float(q[3])
            elif k == 'P,':
                q = line.rstrip().split(',')
                cur[3][(q[1], q[2])] = float(q[3])
    return epochs, header
