"""Aggregation of one run's traces into the compact block the page reads.

The block is a header (plain values: names, tables, labels) and a set of named numpy arrays.  Large tables are
stored column by column; per-epoch tables are dense [entity, epoch] arrays, which compress to almost nothing
where they are constant.  The page decodes the arrays as typed-array views without copying.

Quantities (all derived here, so the page does no estimation):
  tree      per signal, node and epoch: the parent, the hops from the reference receiver (depth), the whole number
            of the tree link (cycles), what the tree did at that epoch (kept, re-hosted, born, ...);
  links     per link that is not a tree link (a "chord"), and epoch: the wide-lane float and the ionosphere-free
            float with the wide lane rounded (narrow-lane cycles), each as a whole number plus a signed fraction,
            their formal sigmas, and the fixing state (float, refused and why, decided, held) with the held whole
            numbers and the post-fit residual;
  events    every row of network_events.csv but the per-prior diagnostics, in lanes (slips, gaps, tree, holds,
            releases, alarms, code reference, other), with the alarm rows of network_raim.csv added;
  products  per satellite the emitted clock, phase and code biases, and per two-signal family the satellite's own
            step of its correction level between epochs, classed whole / ionosphere-like / fraction;
  receivers wet delay, clock and receiver phase bias.
"""
from __future__ import annotations

import math
import time
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

import nv_read as R

INT32_MIN = -2147483648
JUMP_TOL_CYC = 0.15                     # the tolerance of stream_jumps.py
SIGMA_LOG_LO, SIGMA_LOG_HI = -3.0, 2.0  # sigma code range: 1e-3 .. 1e2 cycles
FRAC_SCALE = 60000.0                    # signed fraction in i16

#: Event lanes, in display order: (key, label).
LANES = [
    ('hold', 'Held'), ('release', 'Released'), ('tree', 'Tree re-formed'), ('slip', 'Slips'),
    ('gap', 'Gaps and returns'), ('alarm', 'Alarms'), ('coderef', 'Code reference'), ('other', 'Other checks'),
]
LANE_INDEX = {k: i for i, (k, _l) in enumerate(LANES)}

#: What the tree did to a vertex at an epoch (network_chain_edges.csv column row), with plain labels.
TREE_ROWS = [
    ('', 'not in the tree'), ('Kept', 'kept'), ('Birth', 'joined the tree'), ('RehostUncertified', 'moved (rounded)'),
    ('RehostCertified', 'moved (certified)'), ('Returning', 'returned'), ('Split', 'split off'),
    ('Merge', 'merged'), ('TreeEdgeSlip', 'tree link slipped'), ('Root', 'reference receiver'),
]
TREE_ROW_INDEX = {k: i for i, (k, _l) in enumerate(TREE_ROWS)}


#: Families of event tiers, for the events view's filter: (key, label, tier names or prefixes).
EVENT_FAMILIES = [
    ('slip', 'Cycle slips and gaps', ('T0_mw_detect', 'T0_gap', 'T0_windup', 'T0_band_scope', 'T1_gf_jump', 'T1_if_slip')),
    ('channel', 'Signals and channels', ('SCC', 'SPC', 'SIGNAL')),
    ('fixing', 'Whole numbers', ('RINGFIX',)),
    ('datum', 'Tree and datum', ('CHAIN', 'PHASE_DATUM', 'CLK_BASIS', 'DATUMGRAPH', 'TOPOLOGY', 'READMIT', 'PIVOT',
                                 'PHASE_ANCHOR', 'PBGAUGE', 'STRANSFORM', 'GATE', 'DD_FOLD', 'D2_DENSE', 'INJECT', 'ARC',
                                 'ADMISSION')),
    ('qc', 'Quality tests', ('BLOCKBAARDA', 'MDB', 'CONGRUENCE', 'PRIORRES', 'GROSSLOS', 'RAIM', 'PREFIT', 'REDUNDANCY',
                             'SIGMA0', 'VCE_APPLY', 'T2_', 'T3_', 'T4_')),
    ('atmosphere', 'Atmosphere', ('ZWD',)),
    ('satellite', 'Satellites and clocks', ('CLK_ANCHOR', 'CB_DATUM', 'RETRO', 'CLK_STEP', 'CLK_FRAME', 'PB_FRAME', 'PB_HOLD',
                                            'PHASE_FRAME', 'ANCHOR_LEVEL', 'BRD_TRACK', 'MARK_SCOPE', 'T5_', 'T6_')),
    ('other', 'Other', ()),
]


def event_family(tier: str) -> int:
    """Index into EVENT_FAMILIES of an event tier."""
    for i, (_k, _l, names) in enumerate(EVENT_FAMILIES):
        if any(tier == n or (n.endswith('_') and tier.startswith(n)) or tier.startswith(n + '_') for n in names):
            return i
    return len(EVENT_FAMILIES) - 1


def event_lane(tier: str, event: str, action: str) -> str:
    """The lane of an event row."""
    if tier == 'RINGFIX':
        return {'hold': 'hold', 'release': 'release'}.get(event, 'tree')
    if tier == 'CHAIN':
        return 'other' if event in ('declared_keys', 'kept') else 'tree'
    if tier == 'PHASE_DATUM':
        return 'tree' if event == 'eq34_reattach' else 'other'
    if tier.startswith('T0_gap') or tier in ('SIGNAL', 'READMIT') or event in ('sta_readmitted',):
        return 'gap'
    if tier.startswith(('T0_mw', 'T1_', 'T0_windup')):
        return 'slip'
    if tier.startswith(('T2_', 'T3_', 'T4_', 'T5_', 'T6_')) or tier in ('RAIM', 'GROSSLOS', 'SIGMA0'):
        return 'alarm'
    if tier == 'CLK_ANCHOR' and action == 'refloat':
        return 'alarm'
    if tier in ('CB_DATUM', 'CLK_BASIS'):
        return 'coderef'
    return 'other'


class Index:
    """Name -> index maps for the epochs, receivers, satellites, signals and families of one run."""

    def __init__(self, t_ms, stations, sats, bands, families):
        self.t_ms = sorted(t_ms)
        self.ep = {t: i for i, t in enumerate(self.t_ms)}
        self.stations = stations
        self.sta = {s: i for i, s in enumerate(stations)}
        self.sats = sats
        self.sat = {s: i for i, s in enumerate(sats)}
        self.nodes = stations + sats
        self.node = {s: i for i, s in enumerate(self.nodes)}
        self.bands = bands
        self.band = {b: i for i, b in enumerate(bands)}
        self.families = families
        self.family = {f: i for i, f in enumerate(families)}

    @property
    def n_ep(self):
        return len(self.t_ms)


def _sat_key(s):
    return ('GECJRIS'.find(s[0]) if s[0] in 'GECJRIS' else 9, s)


def _band_key(b):
    sy, sig = b.split(':')
    return ('GECJRIS'.find(sy), sig[1], sig)


def make_index(tr):
    """Collect the epochs and names every trace mentions (tr: dict of what the readers returned)."""
    t_ms, stations, sats, bands, families = set(), [], set(), set(), set()
    roster = tr.get('stations') or []
    stations = [s['name'] for s in roster]
    seen_sta = set(stations)

    def sta(name):
        if name and not R.is_satellite(name) and name not in seen_sta:
            seen_sta.add(name)
            stations.append(name)

    if tr.get('products'):
        for e in tr['products'][0]:
            t_ms.add(e[0])
            sats.update(e[1])
    if tr.get('tree'):
        rows, bks = tr['tree']
        bands.update(bks)
        for r in rows:
            t_ms.add(r[0])
            for v in (r[2], r[3], r[9]):
                (sats.add(v) if R.is_satellite(v) else sta(v))
    if tr.get('chords'):
        for r in tr['chords']:
            t_ms.add(r[0])
            families.add(r[1])
            sta(r[2])
            sats.add(r[3])
    if tr.get('ring_fix'):
        for (t, fam, s, sat) in tr['ring_fix']:
            families.add(fam)
    for fam in families:
        sy, b1, b2 = R.family_bands(fam)
        bands.update((f'{sy}:{b1}', f'{sy}:{b2}'))
    if tr.get('events'):
        for r in tr['events'][0]:
            t_ms.add(r[0])
    if tr.get('zwd'):
        for (t, s) in tr['zwd']:
            t_ms.add(t)
            sta(s)
    sats = sorted((s for s in sats if R.is_satellite(s)), key=_sat_key)
    return Index(t_ms, stations, sats, sorted(bands, key=_band_key), sorted(families, key=lambda f: _band_key(
        f.replace('|', ':').split('+')[0])))


# --------------------------------------------------------------------------------------------------------------- #
# tree


def build_tree(ix: Index, tree):
    """Dense [band, node, epoch] arrays of the tree: parent, depth, whole number of the tree link, row kind.

    parent: node index, -1 not in this signal's tree, -2 a reference receiver (root).
    """
    nb, nn, ne = len(ix.bands), len(ix.nodes), ix.n_ep
    parent = np.full((nb, nn, ne), -1, np.int16)
    depth = np.full((nb, nn, ne), -1, np.int8)
    target = np.full((nb, nn, ne), INT32_MIN, np.int32)
    row = np.zeros((nb, nn, ne), np.uint8)
    if not tree:
        return {}
    rows, _bands = tree
    for (t, bk, child, par, dep, tc, rk, cause, comp, root, es, et) in rows:
        b, e = ix.band.get(bk), ix.ep.get(t)
        c, p = ix.node.get(child), ix.node.get(par)
        if b is None or e is None or c is None or p is None:
            continue
        parent[b, c, e] = p
        depth[b, c, e] = min(dep, 127)
        if tc is not None:
            target[b, c, e] = tc
        row[b, c, e] = TREE_ROW_INDEX.get(rk, 1)
        rt = ix.node.get(root)
        if rt is not None and parent[b, rt, e] == -1:
            parent[b, rt, e] = -2
            depth[b, rt, e] = 0
            row[b, rt, e] = TREE_ROW_INDEX['Root']
    return {'tree_parent': parent, 'tree_depth': depth, 'tree_target': target, 'tree_row': row}


# --------------------------------------------------------------------------------------------------------------- #
# links that are not tree links


def _sigma_code(s):
    s = np.asarray(s, np.float64)
    with np.errstate(divide='ignore', invalid='ignore'):
        c = np.round((np.log10(s) - SIGMA_LOG_LO) * 255.0 / (SIGMA_LOG_HI - SIGMA_LOG_LO))
    c = np.where(np.isfinite(c), np.clip(c, 1, 255), 0)
    return c.astype(np.uint8)


def chord_quantities(fam, f1, f2, q11, q12, q22):
    """Wide lane and ionosphere-free float (wide lane rounded) of a two-signal link, as in the fixing's gate.

    Same algebra as ringFixGate (src/core/network/ar/RingFix.cpp): r = F1 / F2, n_if = (r f1 - f2 - round(wl))
    / (r - 1), var_if = a^2 q11 + 2 a b q12 + b^2 q22 with a = r / (r - 1), b = -1 / (r - 1).
    Returns numpy arrays (wl_int, wl_frac, sigma_wl, n_int, n_frac, sigma_if).
    """
    sy, b1, b2 = R.family_bands(fam)
    r = R.freq_mhz(sy, b1) / R.freq_mhz(sy, b2)
    a, b = r / (r - 1.0), -1.0 / (r - 1.0)
    f1, f2, q11, q12, q22 = (np.asarray(v, np.float64) for v in (f1, f2, q11, q12, q22))
    wl = f1 - f2
    wl_int = np.round(wl)
    n_if = (r * f1 - f2 - wl_int) / (r - 1.0)
    n_int = np.round(n_if)
    with np.errstate(invalid='ignore'):
        s_wl = np.sqrt(np.maximum(q11 + q22 - 2.0 * q12, 0.0))
        s_if = np.sqrt(np.maximum(a * a * q11 + 2.0 * a * b * q12 + b * b * q22, 0.0))
    return wl_int, wl - wl_int, s_wl, n_int, n_if - n_int, s_if


def _fixing_columns(ix: Index, rf: dict, key, ep, n_int, wl_int):
    """The fixing's view of one link at each of its epochs: state code, held whole numbers as offsets from the
    nearest ones (dn1 on n1, dwl on the wide lane; 0 where nothing is held), post-fit residual, ring."""
    fam, s, sat = key
    n = len(ep)
    state, ring = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
    dn1, dwl = np.zeros(n, np.int8), np.zeros(n, np.int8)
    res = np.full(n, np.nan, np.float32)
    for j, e in enumerate(ep):
        v = rf.get((ix.t_ms[e], fam, s, sat))
        if v is None:
            continue
        code, rg, n1, n2, _wl, _nif, _sif, rr, _aud = v
        state[j] = code
        ring[j] = min(rg or 0, 255)
        if n1 is not None and n2 is not None and math.isfinite(n_int[j]):
            dn1[j] = max(-127, min(127, n1 - int(n_int[j])))
            dwl[j] = max(-127, min(127, (n1 - n2) - int(wl_int[j])))
        if math.isfinite(rr):
            res[j] = rr
    return state, dn1, dwl, res, ring


def build_links(ix: Index, chords, ring_fix):
    """Per link (family, receiver, satellite) its rows by epoch, stored column-wise and grouped by link."""
    if not chords and not ring_fix:
        return {}, []
    groups = defaultdict(list)                       # (fam, sta, sat) -> [(ep, f1, f2, q11, q12, q22)]
    for (t, fam, s, sat, f1, f2, q11, q12, q22, _i1, _i2) in chords or ():
        e = ix.ep.get(t)
        if e is not None and fam in ix.family and s in ix.sta and sat in ix.sat:
            groups[(fam, s, sat)].append((e, f1, f2, q11, q12, q22))
    rf = ring_fix or {}
    keys = sorted(groups, key=lambda k: (ix.family[k[0]], ix.sta[k[1]], ix.sat[k[2]]))
    link_fam, link_sta, link_sat, link_start, link_count = [], [], [], [], []
    cols = defaultdict(list)
    n = 0
    for k in keys:
        fam, s, sat = k
        g = sorted(groups[k])
        ep = np.array([x[0] for x in g], np.int64)
        f1, f2, q11, q12, q22 = (np.array([x[i] for x in g]) for i in range(1, 6))
        wl_int, wl_frac, s_wl, n_int, n_frac, s_if = chord_quantities(fam, f1, f2, q11, q12, q22)
        state, dn1, dwl, res, ring = _fixing_columns(ix, rf, k, ep, n_int, wl_int)
        link_fam.append(ix.family[fam])
        link_sta.append(ix.sta[s])
        link_sat.append(ix.sat[sat])
        link_start.append(n)
        link_count.append(len(g))
        n += len(g)
        cols['ce_ep'].append(ep.astype(np.uint16))
        cols['ce_wlint'].append(np.where(np.isfinite(wl_int), wl_int, 0).astype(np.int32))
        cols['ce_wlfrac'].append(np.round(np.nan_to_num(wl_frac) * FRAC_SCALE).astype(np.int16))
        cols['ce_nint'].append(np.where(np.isfinite(n_int), n_int, 0).astype(np.int32))
        cols['ce_nfrac'].append(np.round(np.nan_to_num(n_frac) * FRAC_SCALE).astype(np.int16))
        cols['ce_swl'].append(_sigma_code(s_wl))
        cols['ce_sif'].append(_sigma_code(s_if))
        cols['ce_state'].append(state)
        cols['ce_dn1'].append(dn1)
        cols['ce_dwl'].append(dwl)
        cols['ce_res'].append(res)
        cols['ce_ring'].append(ring)
    out = {k: np.concatenate(v) for k, v in cols.items()}
    out.update({'link_fam': np.array(link_fam, np.uint8), 'link_sta': np.array(link_sta, np.uint16),
                'link_sat': np.array(link_sat, np.uint16), 'link_start': np.array(link_start, np.uint32),
                'link_count': np.array(link_count, np.uint32)})
    # The audit column is written on held rows only: right / wrong against the run's audit file, unlisted when the
    # run has no audit file or the file lists neither band. Counted in held link-epochs.
    audits = dict(sorted(Counter(v[8] for v in rf.values() if v[8]).items()))
    return out, audits


# --------------------------------------------------------------------------------------------------------------- #
# events


def _event_band(sat: str, detail: str, bands: dict):
    """The signal an event names in its detail (sig=, sig1=, or a leading signal token), as a band index."""
    sy = sat[:1] if R.is_satellite(sat) else ''
    for tok in detail.replace(';', ' ').split():
        for pre in ('sig=', 'sig1=', 'band='):
            if tok.startswith(pre):
                v = tok[len(pre):]
                return bands.get(f'{sy}:{v}', -1)
        if len(tok) == 3 and tok[0] == 'L' and tok[1].isdigit():
            return bands.get(f'{sy}:{tok}', -1)
    return -1


def build_events(ix: Index, events, raim):
    """Event rows as columns, details interned in a string table; RAIM alarm rows added in the alarm lane."""
    names, tiers, actions, details = {}, {}, {}, {}

    def intern(tab, s):
        v = tab.get(s)
        if v is None:
            v = tab[s] = len(tab)
        return v

    rows = []
    for (t, tier, ev, sat, sta, stat, thr, act, det) in (events or ()):
        e = ix.ep.get(t)
        if e is None:
            continue
        rows.append((e, LANE_INDEX[event_lane(tier, ev, act)], intern(tiers, tier), intern(names, ev),
                     intern(actions, act), ix.sat.get(sat, -1), ix.sta.get(sta, -1),
                     _event_band(sat, det, ix.band), stat, intern(details, det[:240])))
    for (t, sat, sta, s, refl) in (raim or ()):
        e = ix.ep.get(t)
        if e is None:
            continue
        rows.append((e, LANE_INDEX['alarm'], intern(tiers, 'RAIM'), intern(names, 'residual_alarm'),
                     intern(actions, 'refloat' if refl else 'flag'), ix.sat.get(sat, -1), ix.sta.get(sta, -1), -1, s,
                     intern(details, f'normalized post-fit residual {s:.1f}' + (', satellite refloated' if refl else ''))))
    rows.sort(key=lambda r: (r[0], r[1]))
    cols = list(zip(*rows)) if rows else [[]] * 10
    arr = {
        'ev_ep': np.array(cols[0], np.uint16), 'ev_lane': np.array(cols[1], np.uint8),
        'ev_tier': np.array(cols[2], np.uint16), 'ev_name': np.array(cols[3], np.uint16),
        'ev_action': np.array(cols[4], np.uint16), 'ev_sat': np.array(cols[5], np.int16),
        'ev_sta': np.array(cols[6], np.int16), 'ev_band': np.array(cols[7], np.int8),
        'ev_stat': np.array(cols[8], np.float32), 'ev_detail': np.array(cols[9], np.uint32),
    }

    def table(tab):
        return [k for k, _v in sorted(tab.items(), key=lambda kv: kv[1])]

    tier_list = table(tiers)
    return arr, {'tiers': tier_list, 'names': table(names), 'actions': table(actions), 'details': table(details),
                 'tier_family': [event_family(t) for t in tier_list],
                 'families': [[k, l] for (k, l, _n) in EVENT_FAMILIES]}


# --------------------------------------------------------------------------------------------------------------- #
# products


def build_products(ix: Index, products):
    """Satellite clock, phase and code biases per epoch (metres), dense [signal, satellite, epoch]."""
    if not products:
        return {}, {}
    epochs, header = products
    psig, csig = set(), set()
    for (_t, _c, cb, pb, _st) in epochs:
        psig.update(k[1] for k in pb)
        csig.update(k[1] for k in cb)
    psig = sorted(psig, key=lambda s: (s[1], s))
    csig = sorted(csig, key=lambda s: (s[1], s))
    ps, cs = {s: i for i, s in enumerate(psig)}, {s: i for i, s in enumerate(csig)}
    ns, ne = len(ix.sats), ix.n_ep
    clk = np.full((ns, ne), np.nan, np.float32)
    pb_a = np.full((len(psig), ns, ne), np.nan, np.float32)
    cb_a = np.full((len(csig), ns, ne), np.nan, np.float32)
    status = np.zeros(ne, np.uint8)
    for (t, c, cb, pb, st) in epochs:
        e = ix.ep.get(t)
        if e is None:
            continue
        for sat, v in c.items():
            j = ix.sat.get(sat)
            if j is not None:
                clk[j, e] = v
        for (sat, sig), v in pb.items():
            j = ix.sat.get(sat)
            if j is not None:
                pb_a[ps[sig], j, e] = v
        for (sat, sig), v in cb.items():
            j = ix.sat.get(sat)
            if j is not None:
                cb_a[cs[sig], j, e] = v
        status[e] = 1 if st is None or st[0] == 'ok' else 2
    meta = {'phase_signals': psig, 'code_signals': csig, 'header': header or []}
    return {'sat_clk': clk, 'sat_pb': pb_a, 'sat_cb': cb_a, 'epoch_status': status}, meta


def build_jumps(ix: Index, arrays, pmeta, tree_arrays):
    """The satellite's own step of its correction level between consecutive epochs, per two-signal family.

    stream_jumps.py's definition (card IRD section 8 on the emitted records): per satellite the per-signal level
    E_b = (C - P_b + mu_b x) / lambda_b with x = ((C - B_1) - (C - B_2)) / (1 - gamma), mu_1 = 1, mu_2 = gamma; its
    step between consecutive epochs less the median step over the constellation's satellites is the satellite's
    own step (d1, d2); dn = (f1 d1 - f2 d2) / (f1 - f2) is its ionosphere-free part in narrow-lane cycles.
    Classes: quiet (|d1|, |d2| < tol), whole (both within tol of whole numbers), ionosphere-like (|dn| < tol),
    fraction (the rest).  Returns the step rows, the satellites' cumulative own level per signal, and the class
    counts per hour and hops class.
    """
    if 'sat_clk' not in arrays:
        return {}, []
    clk, pb, cb = arrays['sat_clk'], arrays['sat_pb'], arrays['sat_cb']
    ps = {s: i for i, s in enumerate(pmeta['phase_signals'])}
    cs = {s: i for i, s in enumerate(pmeta['code_signals'])}
    t = np.array(ix.t_ms, np.float64) / 1000.0
    step = float(np.median(np.diff(t))) if len(t) > 1 else 30.0
    rows, tables = [], []
    lvl = np.full((len(ix.families), 2, len(ix.sats), ix.n_ep), np.nan, np.float32)
    for fi, fam in enumerate(ix.families):
        sy, b1, b2 = R.family_bands(fam)
        c1, c2 = 'C' + b1[1:], 'C' + b2[1:]
        if not all(k in d for k, d in ((b1, ps), (b2, ps), (c1, cs), (c2, cs))):
            continue
        valid = _family_steps(ix, fi, fam, clk, pb[ps[b1]], pb[ps[b2]], cb[cs[c1]], cb[cs[c2]], t, step, rows, lvl)
        tables.append(jump_table(ix, fi, [r for r in rows if r[2] == fi], valid, tree_arrays, step))
    rows.sort()
    cols = list(zip(*rows)) if rows else [[]] * 7
    out = {'jp_ep': np.array(cols[0], np.uint16), 'jp_sat': np.array(cols[1], np.uint16),
           'jp_fam': np.array(cols[2], np.uint8), 'jp_cls': np.array(cols[3], np.uint8),
           'jp_d1': np.array(cols[4], np.float32), 'jp_d2': np.array(cols[5], np.float32),
           'jp_dn': np.array(cols[6], np.float32), 'sat_level': lvl}
    return out, tables


def jump_class(a1: float, a2: float, an: float) -> int:
    """0 quiet, 1 whole cycles, 2 ionosphere-like, 3 fraction of a cycle (tolerance JUMP_TOL_CYC)."""
    if abs(a1) < JUMP_TOL_CYC and abs(a2) < JUMP_TOL_CYC:
        return 0
    if abs(a1 - round(a1)) < JUMP_TOL_CYC and abs(a2 - round(a2)) < JUMP_TOL_CYC:
        return 1
    return 2 if abs(an) < JUMP_TOL_CYC else 3


def _family_steps(ix, fi, fam, clk, p1, p2, cb1, cb2, t, step, rows, lvl):
    """Steps of one family; appends the non-quiet steps to rows, fills lvl[fi]; returns the [sat, epoch] mask of
    satellite-epochs whose step is defined (the denominator of the per-hour rates)."""
    sy, b1, b2 = R.family_bands(fam)
    f1, f2 = R.freq_mhz(sy, b1) * 1e6, R.freq_mhz(sy, b2) * 1e6
    gam = (f1 / f2) ** 2
    x = ((clk - cb1) - (clk - cb2)) / (1.0 - gam)
    e1 = (clk - p1 + x) / (R.C_M_S / f1)
    e2 = (clk - p2 + gam * x) / (R.C_M_S / f2)
    other = np.array([s[0] != sy for s in ix.sats])
    e1[other] = np.nan
    e2[other] = np.nan
    valid = np.zeros(e1.shape, bool)
    cum1, cum2 = np.zeros(len(ix.sats)), np.zeros(len(ix.sats))
    for k in range(ix.n_ep):
        ok_now = np.isfinite(e1[:, k]) & np.isfinite(e2[:, k])
        if k > 0 and t[k] - t[k - 1] <= 1.5 * step:
            both = ok_now & np.isfinite(e1[:, k - 1]) & np.isfinite(e2[:, k - 1])
            if both.sum() >= 4:
                d1 = e1[both, k] - e1[both, k - 1]
                d2 = e2[both, k] - e2[both, k - 1]
                x1, x2 = d1 - np.median(d1), d2 - np.median(d2)
                dn = (f1 * x1 - f2 * x2) / (f1 - f2)
                idx = np.nonzero(both)[0]
                valid[idx, k] = True
                cum1[idx] += x1
                cum2[idx] += x2
                for j, a1, a2, an in zip(idx, x1, x2, dn):
                    cls = jump_class(a1, a2, an)
                    if cls:
                        rows.append((k, int(j), fi, cls, float(a1), float(a2), float(an)))
        lvl[fi, 0, ok_now, k] = cum1[ok_now]
        lvl[fi, 1, ok_now, k] = cum2[ok_now]
    return valid


def hops_class_of(d1: np.ndarray, d2: np.ndarray) -> np.ndarray:
    """Hops class of satellites from their depth in the two signals' trees: 0 not in both trees, 1 one hop,
    2 three, 3 five, 4 seven or more (the depth classes of stream_jumps.py)."""
    d = np.where((d1 >= 0) & (d2 >= 0), np.maximum(d1, d2), -1)
    return np.select([d < 0, d <= 1, d <= 3, d <= 5], [0, 1, 2, 3], 4)


def jump_table(ix: Index, fi, rows, valid, tree_arrays, step):
    """Per hour and hops class: satellite-hours with a defined step, and steps by class (whole, iono, fraction)."""
    t0 = ix.t_ms[0]
    hour = (np.array(ix.t_ms, np.int64) - t0) // 3600000
    nh = int(hour[-1]) + 1
    ns = len(ix.stations)
    sy, b1, b2 = R.family_bands(ix.families[fi])
    depth = tree_arrays.get('tree_depth')
    bi1, bi2 = ix.band.get(f'{sy}:{b1}'), ix.band.get(f'{sy}:{b2}')
    if depth is None or bi1 is None or bi2 is None:
        cls = np.zeros(valid.shape, np.int64)
    else:
        cls = hops_class_of(depth[bi1, ns:, :].astype(np.int64), depth[bi2, ns:, :].astype(np.int64))
    sat_hours = np.zeros((nh, 5))
    counts = np.zeros((nh, 5, 3))
    for h in range(nh):
        cols = hour == h
        v, c = valid[:, cols], cls[:, cols]
        for k in range(5):
            sat_hours[h, k] = np.count_nonzero(v & (c == k)) * step / 3600.0
    for (k, j, _f, cl, _a, _b, _c) in rows:
        counts[hour[k], cls[j, k], cl - 1] += 1
    return {'family': fi, 'sat_hours': np.round(sat_hours, 3).tolist(), 'counts': counts.astype(int).tolist()}


# --------------------------------------------------------------------------------------------------------------- #
# receivers


def build_receivers(ix: Index, zwd, clock, rpb):
    """Wet delay, clock and receiver phase bias per receiver and epoch (metres; NaN where absent)."""
    ns, ne = len(ix.stations), ix.n_ep
    out = {}
    for name, src in (('rx_zwd', zwd), ('rx_clk', clock)):
        if not src:
            continue
        val = np.full((ns, ne), np.nan, np.float32)
        sd = np.full((ns, ne), np.nan, np.float32)
        for (t, s), (v, d) in src.items():
            e, j = ix.ep.get(t), ix.sta.get(s)
            if e is not None and j is not None:
                val[j, e], sd[j, e] = v, d
        out[name], out[name + '_sd'] = val, sd
    if rpb:
        val = np.full((len(ix.bands), ns, ne), np.nan, np.float32)
        sd = np.full((len(ix.bands), ns, ne), np.nan, np.float32)
        for (t, s, bk), (v, d, _p) in rpb.items():
            e, j, b = ix.ep.get(t), ix.sta.get(s), ix.band.get(bk)
            if e is not None and j is not None and b is not None:
                val[b, j, e], sd[b, j, e] = v, d
        out['rx_rpb'], out['rx_rpb_sd'] = val, sd
    return out


# --------------------------------------------------------------------------------------------------------------- #
# sky: elevation of every line of sight, and the satellites' sub-satellite points for the world map

ORBIT_KM = {'G': 26560.0, 'E': 29600.0, 'R': 25510.0, 'J': 42164.0, 'I': 42164.0}
BDS_HIGH = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 16, 38, 39, 40, 59, 60, 61, 62}   # BeiDou GEO and IGSO


def orbit_radius_m(sat: str) -> float:
    """Nominal orbit radius of a satellite (m): MEO per constellation, GEO / IGSO for the BeiDou high orbits."""
    if sat[0] == 'C':
        return (42164.0 if int(sat[1:]) in BDS_HIGH else 27906.0) * 1000.0
    return ORBIT_KM.get(sat[0], 26560.0) * 1000.0


def build_sky(ix: Index, los, stations):
    """los_el [receiver, satellite, epoch] elevation in whole degrees (255 = not tracked); sat_lat, sat_lon
    [satellite, epoch] of the sub-satellite point: each receiver's line of sight (azimuth, elevation) met with the
    nominal orbit sphere, averaged over the receivers.  Good to a few tenths of a degree, enough for a map."""
    if not los or not stations:
        return {}
    t, sta, sat, el, az = los
    e = np.fromiter((ix.ep.get(x, -1) for x in t), np.int64, len(t))
    j = np.fromiter((ix.sta.get(x, -1) for x in sta), np.int64, len(t))
    k = np.fromiter((ix.sat.get(x, -1) for x in sat), np.int64, len(t))
    el, az = np.asarray(el, np.float64), np.asarray(az, np.float64)
    ok = (e >= 0) & (j >= 0) & (k >= 0) & np.isfinite(el) & np.isfinite(az)
    e, j, k, el, az = e[ok], j[ok], k[ok], el[ok], az[ok]
    ns, nt = len(ix.stations), len(ix.sats)
    los_el = np.full((ns, nt, ix.n_ep), 255, np.uint8)
    los_el[j, k, e] = np.clip(np.round(el), 0, 90).astype(np.uint8)
    roster = {s['name']: s for s in stations}
    pos = np.full((ns, 3), np.nan)
    geo = np.full((ns, 2), np.nan)
    for name, i in ix.sta.items():
        s = roster.get(name)
        if s and all(math.isfinite(v) for v in s['ecef']):
            pos[i] = s['ecef']
            geo[i] = (math.radians(s['lat']), math.radians(s['lon']))
    r, la, lo = pos[j], geo[j, 0], geo[j, 1]
    ce, se, ca, sa = np.cos(np.radians(el)), np.sin(np.radians(el)), np.cos(np.radians(az)), np.sin(np.radians(az))
    ue, un, uu = ce * sa, ce * ca, se
    u = np.stack([-np.sin(lo) * ue - np.sin(la) * np.cos(lo) * un + np.cos(la) * np.cos(lo) * uu,
                  np.cos(lo) * ue - np.sin(la) * np.sin(lo) * un + np.cos(la) * np.sin(lo) * uu,
                  np.cos(la) * un + np.sin(la) * uu], axis=1)
    rs = np.array([orbit_radius_m(s) for s in ix.sats])[k]
    b = np.einsum('ij,ij->i', r, u)
    c = np.einsum('ij,ij->i', r, r) - rs * rs
    with np.errstate(invalid='ignore'):
        p = r + (-b + np.sqrt(b * b - c))[:, None] * u
        p /= np.linalg.norm(p, axis=1)[:, None]
    good = np.all(np.isfinite(p), axis=1)
    acc = np.zeros((nt, ix.n_ep, 3))
    np.add.at(acc, (k[good], e[good]), p[good])
    n = np.linalg.norm(acc, axis=2)
    with np.errstate(invalid='ignore', divide='ignore'):
        lat = np.degrees(np.arctan2(acc[..., 2], np.hypot(acc[..., 0], acc[..., 1])))
        lon = np.degrees(np.arctan2(acc[..., 1], acc[..., 0]))
    lat[n == 0], lon[n == 0] = np.nan, np.nan
    return {'los_el': los_el, 'sat_lat': lat.astype(np.float32), 'sat_lon': lon.astype(np.float32)}


# --------------------------------------------------------------------------------------------------------------- #
# summaries


def build_summary(ix: Index, links):
    """Per family and epoch: links that are not tree links, held, decided, refused (counts)."""
    nf, ne = len(ix.families), ix.n_ep
    out = np.zeros((4, nf, ne), np.uint16)                   # chords, held, decided, refused
    if not links:
        return {'sum_links': out}
    fam = np.repeat(links['link_fam'].astype(np.int64), links['link_count'].astype(np.int64))
    ep = links['ce_ep'].astype(np.int64)
    st = links['ce_state']
    np.add.at(out[0], (fam, ep), 1)
    np.add.at(out[1], (fam[st == R.STATE_CODES['held']], ep[st == R.STATE_CODES['held']]), 1)
    np.add.at(out[2], (fam[st == R.STATE_CODES['decided']], ep[st == R.STATE_CODES['decided']]), 1)
    ref = (st >= 1) & (st <= 12)
    np.add.at(out[3], (fam[ref], ep[ref]), 1)
    return {'sum_links': out}


# --------------------------------------------------------------------------------------------------------------- #
# the whole block


#: What each part of the core block needs: (part, file in logs/ or the run folder, the config key that writes it,
#: the layer of the dashboard that shows it).
NEEDS = [
    ('tree', 'logs/network_chain_edges.csv', 'network.trace_chain_datum (written on a run with network.phase_datum_eq34)', 'network'),
    ('links', 'logs/network_chain_frame.csv', 'network.trace_chain_datum (written on a run with network.phase_datum_eq34)', 'network'),
    ('fixing', 'logs/network_ring_fix.csv', 'network.ring_fix.enable (the fixing itself; a run without it has floats only)', 'network'),
    ('events', 'logs/network_events.csv', 'network.trace_events', 'events'),
    ('alarms', 'logs/network_raim.csv', 'network.trace_slip_events', 'events'),
    ('receivers', 'logs/network_stations.csv', 'network.trace_stations', 'receivers'),
    ('wet delay', 'logs/network_zwd.csv', 'network.trace_zwd', 'atmosphere'),
    ('receiver clock', 'logs/network_rx_states.csv', 'network.trace_rx_states', 'receivers'),
    ('receiver phase bias', 'logs/network_rpb_states.csv', 'network.trace_rpb_states (written on a chain run)', 'receivers'),
    ('satellite positions and elevations', 'logs/network_ionosphere.csv', 'network.trace_ionosphere', 'satellites'),
    ('corrections', 'products.txt', 'the run output itself (always written)', 'satellites'),
]


def read_all(run: Path, log=print):
    """Every trace the page uses, read once; returns a dict of the readers' values and the timings."""
    logs = run / 'logs'
    jobs = [('stations', lambda: R.read_stations(logs)), ('tree', lambda: R.read_tree(logs)),
            ('chords', lambda: R.read_chords(logs)), ('ring_fix', lambda: R.read_ring_fix(logs)),
            ('events', lambda: R.read_events(logs)), ('raim', lambda: R.read_raim(logs)),
            ('zwd', lambda: R.read_zwd(logs)), ('clock', lambda: R.read_rx_clock(logs)),
            ('rpb', lambda: R.read_rpb(logs)), ('los', lambda: R.read_lines_of_sight(logs)),
            ('products', lambda: R.read_products(run))]
    out, timing = {}, {}
    for name, fn in jobs:
        t = time.time()
        out[name] = fn()
        timing[name] = round(time.time() - t, 2)
        log(f'  read {name:9s} {timing[name]:6.2f} s' + ('  (absent)' if out[name] is None else '' if out[name] else '  (empty)'))
    return out, timing


def build_blocks(run: Path, log=print) -> dict:
    """Read a run folder and return every block of the page: {'core': (header, arrays), 'epoch': ..., ...}."""
    import layers
    t_start = time.time()
    header, arrays, ix, tr = _core(run, log)
    ctx = layers.Ctx(run, ix, tr, log)
    blocks = layers.build_all(ctx)
    header['missing'] = header['missing'] + ctx.missing
    header['blocks'] = sorted(blocks)
    header['read_s'].update(ctx.timing)
    header['build_s'] = round(time.time() - t_start, 1)
    return {'core': (header, arrays), **blocks}


def build_block(run: Path, log=print):
    """Read a run folder and return the core block only: (header dict, {name: numpy array})."""
    header, arrays, _ix, _tr = _core(run, log)
    return header, arrays


def _core(run: Path, log=print):
    """The core block (trees, links, events, products, receivers' wet delay and clock, sky) and the run's index."""
    t_start = time.time()
    tr, timing = read_all(run, log)
    ix = make_index(tr)
    if ix.n_ep == 0:
        raise SystemExit(f'{run}: no epochs in any trace (is this a run folder with logs/ and products.txt?)')
    arrays = {'t_ms': (np.array(ix.t_ms, np.int64) - ix.t_ms[0]).astype(np.int32)}
    arrays.update(build_tree(ix, tr['tree']))
    links, audits = build_links(ix, tr['chords'], tr['ring_fix'])
    arrays.update(links)
    ev, ev_tables = build_events(ix, tr['events'][0] if tr['events'] else None, tr['raim'])
    arrays.update(ev)
    prod, pmeta = build_products(ix, tr['products'])
    arrays.update(prod)
    jumps, jtables = build_jumps(ix, arrays, pmeta, arrays) if prod else ({}, [])
    arrays.update(jumps)
    arrays.update(build_receivers(ix, tr['zwd'], tr['clock'], tr['rpb']))
    arrays.update(build_sky(ix, tr['los'], tr['stations']))
    arrays.update(build_summary(ix, links))
    header = make_header(run, ix, tr, pmeta, ev_tables, jtables, audits, timing)
    header['build_s'] = round(time.time() - t_start, 1)
    return header, arrays, ix, tr


def make_header(run, ix, tr, pmeta, ev_tables, jtables, audits, timing):
    """The plain-value part of the block."""
    roster = {s['name']: s for s in (tr['stations'] or [])}
    stations = []
    for s in ix.stations:
        r = roster.get(s, {})
        stations.append({'name': s, 'role': r.get('role', ''), 'lat': _num(r.get('lat')), 'lon': _num(r.get('lon'))})
    fam_held = {f for (_t, f, _s, _x) in (tr['ring_fix'] or {})}
    held_any = defaultdict(int)
    for v_key, v in (tr['ring_fix'] or {}).items():
        if v[0] == R.STATE_CODES['held']:
            held_any[v_key[1]] += 1
    families = []
    for f in ix.families:
        sy, b1, b2 = R.family_bands(f)
        families.append({'key': f, 'sys': sy, 'b1': b1, 'b2': b2, 'band1': ix.band.get(f'{sy}:{b1}', -1),
                         'band2': ix.band.get(f'{sy}:{b2}', -1), 'f1': R.freq_mhz(sy, b1), 'f2': R.freq_mhz(sy, b2),
                         'fixing': f in fam_held, 'held_link_epochs': held_any.get(f, 0)})
    present = {'tree': tr['tree'], 'links': tr['chords'], 'fixing': tr['ring_fix'], 'events': tr['events'],
               'alarms': tr['raim'], 'receivers': tr['stations'], 'wet delay': tr['zwd'],
               'receiver clock': tr['clock'], 'receiver phase bias': tr['rpb'],
               'satellite positions and elevations': tr['los'], 'corrections': tr['products']}
    # absent means no file (None); a file with a header and no rows is present and simply has nothing to show
    missing = [{'part': p, 'file': f, 'key': k, 'layer': lay} for (p, f, k, lay) in NEEDS if present.get(p) is None]
    t0 = ix.t_ms[0]
    week, tow0 = divmod(t0, R.WEEK_MS)
    cfg = _config_summary(run)
    return {
        'schema': 1, 'run': run.name, 'path': str(run), 'built': time.strftime('%Y-%m-%d %H:%M'),
        'week0': int(week), 'tow0_ms': int(tow0), 'n_ep': ix.n_ep,
        'stations': stations, 'sats': ix.sats, 'bands': ix.bands, 'families': families,
        'tree_rows': [l for (_k, l) in TREE_ROWS], 'states': _state_labels(), 'audits': audits,
        'lanes': [{'key': k, 'label': l} for (k, l) in LANES],
        'events': ev_tables, 'events_dropped': (tr['events'][1] if tr['events'] else {}),
        'phase_signals': pmeta.get('phase_signals', []), 'code_signals': pmeta.get('code_signals', []),
        'products_header': pmeta.get('header', []), 'jump_tables': jtables, 'jump_tol': JUMP_TOL_CYC,
        'missing': missing, 'read_s': timing, 'config': cfg,
        'sigma_code': [SIGMA_LOG_LO, SIGMA_LOG_HI], 'frac_scale': FRAC_SCALE,
    }


def _num(v):
    return None if v is None or not math.isfinite(v) else round(float(v), 5)


def _state_labels():
    labels = {
        'float': 'float (no fixing on this family)', 'not_candidate': 'waiting: a link nearer the reference is not held',
        'no_float': 'refused: no usable float', 'window': 'refused: an event in the last 10 min',
        'sigma': 'refused: uncertainty too large', 'fraction': 'refused: too far from a whole number',
        'lattice': 'refused: disagrees with the fixed cycles', 'persist': 'passed, not yet 10 epochs in a row',
        'odds': 'refused: odds against the next whole number too low', 'cap': 'passed, over the cap of decisions per epoch',
        'wl_fraction': 'refused: wide lane too far from a whole number', 'code_event': 'refused: code reference moved',
        'refused': 'refused', 'decided': 'decided this epoch (held from the next)', 'held': 'held at whole numbers',
        'would_hold': 'would be held (log only)',
    }
    return [labels[k] for k, _v in sorted(R.STATE_CODES.items(), key=lambda kv: kv[1])]


def _config_summary(run: Path):
    """Settings of the run's own config (arm.json, else the first *.json in the folder) shown on the page.

    Every key of network.ring_fix is kept, in the order of the file, so the setting that tells two runs apart
    (for example settled_rule) is on the page whatever its name; plus the systems, the chain switch and its root.
    """
    import json
    for p in [run / 'arm.json'] + sorted(run.glob('*.json')):
        if not p.exists():
            continue
        try:
            j = json.loads(p.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        n = j.get('network', {}) if isinstance(j, dict) else {}
        rf = n.get('ring_fix') or {}
        return {'file': p.name, 'active_systems': (j.get('processing') or {}).get('active_systems'),
                'ring_fix': dict(rf) if isinstance(rf, dict) else {},
                'chain': n.get('phase_datum_eq34'), 'roots': n.get('chain_root') or n.get('phase_datum_root')}
    return {}
