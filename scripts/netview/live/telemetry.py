"""The per-epoch record of the live monitor, and a source that builds it from the blocks of a finished run.

A live engine would send one record per epoch; the replay source makes the same records from the dashboard's
blocks (a built page or a run folder), so the page and the server are built and judged before any engine change.

The record (JSON, field lists in the description `meta()` sends once):
    {"e": epoch index, "ms": GPS time in ms,
     "net": [one value per NET_FIELDS],            the whole network: disposition, sigma0, coverage, user error
     "fam": [[links that are not tree links, held], per family],
     "rx":  [[one value per RX_FIELDS], per receiver],
     "sat": [[one value per SAT_FIELDS], per satellite],
     "ev":  [[lane, family, tier, name, action, receiver, satellite, detail], per event of the epoch]}
Empty values are null.  About 1,100 numbers per epoch for 56 receivers and 62 satellites (7 to 9 KB of JSON).
"""
from __future__ import annotations

import math

import numpy as np

import nv_read as R

WEEK_MS = 604800000

#: (key, label, colour scale, unit) of the per-receiver values.
RX_FIELDS = [
    ('code', 'post-fit code RMS', 'viridis', 'm'), ('phase', 'post-fit phase RMS', 'viridis', 'm'),
    ('nsat', 'satellites with residuals', 'viridis', ''), ('zcode', 'normalized code residual scale', 'viridis', ''),
    ('raim', 'largest normalized residual (residual test)', 'viridis', ''), ('slip', 'cycle slips detected', 'count', ''),
    ('qc', 'quality-test flags', 'count', ''), ('alarm', 'alarms that acted', 'count', ''),
    ('held', 'share of its links held at whole numbers', 'viridis', ''), ('zwd', 'wet delay (correction to the model)', 'vik', 'm'),
    ('clksd', 'receiver clock sigma', 'viridis', 'm'),
]
#: (key, label, colour scale, unit) of the per-satellite values.
SAT_FIELDS = [
    ('code', 'post-fit code RMS', 'viridis', 'm'), ('nrx', 'receivers with residuals', 'viridis', ''),
    ('anchor', 'clock anchor test (|w|, sigma)', 'viridis', ''), ('frac', 'fraction jumps of the correction level', 'count', ''),
    ('jumps', 'jumps of the correction level', 'count', ''), ('held', 'share of its links held', 'viridis', ''),
    ('slip', 'cycle slips detected', 'count', ''), ('clk', 'emitted clock', 'vik', 'm'),
]
#: (key, label) of the whole-network values.
NET_FIELDS = [
    ('disp', 'what happened to the epoch'), ('s0c', 'post-fit sigma0, code'), ('s0p', 'post-fit sigma0, phase'),
    ('npres', 'receivers in the solution'), ('nclk', 'satellites with a clock'), ('user3d', 'held-out user, 3D error (m)'),
    ('ue', 'held-out user, east error (m)'), ('un', 'held-out user, north error (m)'), ('uu', 'held-out user, up error (m)'),
    ('msep', 'time per epoch (ms)'),
]
SYSTEM_NAMES = {'G': 'GPS', 'E': 'Galileo', 'C': 'BeiDou'}
SLIP_TIERS = {'T0_mw_detect', 'T1_gf_jump', 'T1_if_slip'}
#: Actions that record without acting (a periodic report, a diagnostic, a coincident duplicate): counted neither as a
#: cycle slip nor as a quality-test flag, and filed under bookkeeping in the page's feed.
QUIET_ACTIONS = {'diagnostic', 'coincident', 'report'}


def family_entry(key: str, fixing: bool = True) -> dict:
    """The page's description of a two-signal family ('E|L1X+L5X'): key, system, label ('Galileo L1X + L5X') and
    whether the integer fixing works on it (the page shows a held tile only for such a family)."""
    sy, b1, b2 = R.family_bands(key)
    return {'key': key, 'sys': sy, 'label': SYSTEM_NAMES.get(sy, sy) + ' ' + b1 + ' + ' + b2, 'fixing': bool(fixing)}


def colour_range(key: str, kind: str, v: np.ndarray):
    """The colour range of one field over the finite values v: (range, scale kind or None).

    Counts run from 0 to the 99th percentile of their positive values; a share from 0 to 1; a signed value (vik)
    symmetric about 0, unless nearly all of one sign (1st percentile above -15 % of the 99th), when it runs from 0 to
    its 99th percentile with the lowest colour marked "<0" (or the mirror); any other value from its 1st to its 99th
    percentile.
    """
    if not v.size:
        return [0.0, 1.0], None
    if kind == 'count':
        pos = v[v > 0]
        return [0.0, float(max(1.0, np.percentile(pos, 99) if pos.size else 1.0))], None
    if key == 'held':
        return [0.0, 1.0], None
    if kind == 'vik':
        p1, p99 = (float(x) for x in np.percentile(v, [1, 99]))
        if p99 > 0 and p1 >= -0.15 * p99:                    # nearly all positive: 0 .. p99, '<0' below
            return [0.0, p99], {'lo_label': '<0', 'hi_label': None}
        if p1 < 0 and p99 <= -0.15 * p1:                     # nearly all negative
            return [p1, 0.0], {'lo_label': None, 'hi_label': '>0'}
        m = max(abs(p1), abs(p99)) or 1.0
        return [-m, m], None
    lo, hi = np.percentile(v, [1, 99])
    return [float(lo), float(max(hi, lo + 1e-9))], None


def _round_list(a, digits):
    """A float array as a JSON-ready list (NaN and infinities become None)."""
    a = np.asarray(a, np.float64)
    out = np.round(a, digits).tolist()
    return [None if (x is None or not math.isfinite(x)) else x for x in out]


class ReplaySource:
    """Per-epoch records of a finished run, from its blocks ({name: (header, arrays)} of nv_build or nv_page)."""

    mode = 'replay'

    def __init__(self, blocks: dict, archive: bool = False):
        self._ranges = None
        self._meta = None
        self.H, self.A = blocks['core']
        self.blocks = blocks
        self.archive = archive
        H, A = self.H, self.A
        self.n_ep = int(H['n_ep'])
        self.ns, self.nt = len(H['stations']), len(H['sats'])
        t = np.asarray(A['t_ms'], np.int64)
        self.step_s = float((t[-1] - t[0]) / max(1, self.n_ep - 1) / 1000.0) if self.n_ep > 1 else 30.0
        self.abs_ms = int(H['week0']) * WEEK_MS + int(H['tow0_ms']) + t
        self._events()
        self._links()
        self.rx = self._rx_table()
        self.sat = self._sat_table()
        self.net = self._net_table()

    # ------------------------------------------------------------------ derived tables

    def _events(self):
        H, A = self.H, self.A
        E = H['events']
        ne, ns, nt = self.n_ep, self.ns, self.nt
        ep = np.asarray(A.get('ev_ep', np.zeros(0)), np.int64)
        self.ev_off = np.searchsorted(ep, np.arange(ne + 1))
        lanes = [l['key'] for l in H['lanes']]
        fam_keys = [f[0] for f in E.get('families', [])]
        tier_fam = E.get('tier_family', [len(fam_keys) - 1] * len(E['tiers']))
        tier = np.asarray(A.get('ev_tier', np.zeros(0)), np.int64)
        act = np.asarray(A.get('ev_action', np.zeros(0)), np.int64)
        lane = np.asarray(A.get('ev_lane', np.zeros(0)), np.int64)
        is_slip = np.array([E['tiers'][a] in SLIP_TIERS and E['actions'][b] not in QUIET_ACTIONS for a, b in zip(tier, act)], bool)
        is_alarm = np.array([lanes[x] == 'alarm' for x in lane], bool)
        qc = fam_keys.index('qc') if 'qc' in fam_keys else -1
        is_qc = np.array([tier_fam[a] == qc and E['actions'][b] not in QUIET_ACTIONS for a, b in zip(tier, act)], bool) & ~is_alarm
        self.ev_family = np.array([tier_fam[a] for a in tier], np.int64)
        sta = np.asarray(A.get('ev_sta', np.zeros(0)), np.int64)
        sat = np.asarray(A.get('ev_sat', np.zeros(0)), np.int64)
        self.ev_counts = {}
        for name, sel in (('slip', is_slip), ('qc', is_qc), ('alarm', is_alarm)):
            rx = np.zeros((ns, ne), np.float32)
            m = sel & (sta >= 0)
            np.add.at(rx, (sta[m], ep[m]), 1)
            st = np.zeros((nt, ne), np.float32)
            m = sel & (sat >= 0)
            np.add.at(st, (sat[m], ep[m]), 1)
            self.ev_counts[name] = (rx, st)

    def _links(self):
        """Held and all links that are not tree links, per receiver / satellite and epoch (fixing families)."""
        H, A = self.H, self.A
        ne = self.n_ep
        self.rx_links = np.zeros((self.ns, ne), np.float32)
        self.rx_held = np.zeros((self.ns, ne), np.float32)
        self.sat_links = np.zeros((self.nt, ne), np.float32)
        self.sat_held = np.zeros((self.nt, ne), np.float32)
        if 'link_fam' not in A or len(A['link_fam']) == 0:
            return
        fixing = np.array([f['fixing'] for f in H['families']], bool)
        link = np.repeat(np.arange(len(A['link_fam'])), np.asarray(A['link_count'], np.int64))
        keep = fixing[np.asarray(A['link_fam'], np.int64)[link]]
        e = np.asarray(A['ce_ep'], np.int64)[keep]
        held = (np.asarray(A['ce_state'], np.int64) == R.STATE_CODES['held'])[keep]
        j = np.asarray(A['link_sta'], np.int64)[link[keep]]
        k = np.asarray(A['link_sat'], np.int64)[link[keep]]
        np.add.at(self.rx_links, (j, e), 1)
        np.add.at(self.rx_held, (j[held], e[held]), 1)
        np.add.at(self.sat_links, (k, e), 1)
        np.add.at(self.sat_held, (k[held], e[held]), 1)

    def _block(self, name):
        b = self.blocks.get(name)
        return b[1] if b else {}

    def _rx_table(self):
        """[field, receiver, epoch] of RX_FIELDS."""
        R, A = self._block('receivers'), self.A
        ns, ne = self.ns, self.n_ep
        out = np.full((len(RX_FIELDS), ns, ne), np.nan, np.float32)
        src = {'code': R.get('rp_c_rms'), 'phase': R.get('rp_p_rms'), 'zcode': R.get('pk_c_s0'), 'raim': R.get('sr_max'),
               'zwd': A.get('rx_zwd'), 'clksd': A.get('rx_clk_sd')}
        for i, (k, *_r) in enumerate(RX_FIELDS):
            if k in src and src[k] is not None:
                out[i] = src[k]
            elif k == 'nsat' and R.get('rp_nsat') is not None:
                out[i] = np.where(R['rp_nsat'] > 0, R['rp_nsat'], np.nan)
            elif k in self.ev_counts:
                out[i] = self.ev_counts[k][0]
            elif k == 'held':
                with np.errstate(invalid='ignore', divide='ignore'):
                    out[i] = np.where(self.rx_links > 0, self.rx_held / self.rx_links, np.nan)
        return out

    def _sat_table(self):
        S, A = self._block('satellites'), self.A
        nt, ne = self.nt, self.n_ep
        out = np.full((len(SAT_FIELDS), nt, ne), np.nan, np.float32)
        frac = np.zeros((nt, ne), np.float32)
        jumps = np.zeros((nt, ne), np.float32)
        if 'jp_ep' in A:
            cls = np.asarray(A['jp_cls'], np.int64)
            m = cls > 0
            np.add.at(jumps, (np.asarray(A['jp_sat'], np.int64)[m], np.asarray(A['jp_ep'], np.int64)[m]), 1)
            m = cls == 3
            np.add.at(frac, (np.asarray(A['jp_sat'], np.int64)[m], np.asarray(A['jp_ep'], np.int64)[m]), 1)
        anchor = None
        if S.get('ag') is not None:
            fields = [f[0] for f in self.blocks['satellites'][0].get('ag_fields', [])]
            if 'w_clk' in fields:
                anchor = np.abs(S['ag'][fields.index('w_clk')])
        for i, (k, *_r) in enumerate(SAT_FIELDS):
            if k == 'code' and S.get('sp_c_rms') is not None:
                out[i] = S['sp_c_rms']
            elif k == 'nrx' and S.get('sp_nrx') is not None:
                out[i] = np.where(S['sp_nrx'] > 0, S['sp_nrx'], np.nan)
            elif k == 'anchor' and anchor is not None:
                out[i] = anchor
            elif k == 'frac':
                out[i] = frac
            elif k == 'jumps':
                out[i] = jumps
            elif k == 'held':
                with np.errstate(invalid='ignore', divide='ignore'):
                    out[i] = np.where(self.sat_links > 0, self.sat_held / self.sat_links, np.nan)
            elif k == 'slip':
                out[i] = self.ev_counts['slip'][1]
            elif k == 'clk' and A.get('sat_clk') is not None:
                out[i] = A['sat_clk']
        return out

    def _net_table(self):
        Eb = self.blocks.get('epoch')
        ne = self.n_ep
        out = np.full((len(NET_FIELDS), ne), np.nan, np.float32)
        if not Eb:
            return out
        h, a = Eb

        def field(block, name):
            names = [f[0] for f in h.get(block + '_fields', [])]
            return a[block][names.index(name)] if block in a and name in names else None

        src = {'s0c': field('es', 'sigma0_pf_code'), 's0p': field('es', 'sigma0_pf_phase'), 'npres': field('es', 'n_present'),
               'nclk': field('es', 'n_clock_sats'), 'user3d': field('cl', 'pos_err_3d_m'), 'ue': field('cl', 'east_m'),
               'un': field('cl', 'north_m'), 'uu': field('cl', 'up_m'), 'msep': field('sh', 'ms_per_epoch_window')}
        for i, (k, _l) in enumerate(NET_FIELDS):
            if k == 'disp' and 'es_disp' in a:
                d = a['es_disp'].astype(np.float32)
                out[i] = np.where(d == 255, np.nan, d)
            elif src.get(k) is not None:
                out[i] = src[k]
        return out

    # ------------------------------------------------------------------ what the server sends

    def meta(self, window: int) -> dict:
        if self._meta is None or self._meta['window'] != int(window):
            self._meta = self._meta_of(window)
        return self._meta

    def _meta_of(self, window: int) -> dict:
        H = self.H
        E = H['events']
        es = self.blocks.get('epoch')
        return {
            'run': H['run'], 'mode': 'replay', 'n_ep': self.n_ep, 'step_s': self.step_s, 'window': int(window),
            'stations': [{'name': s['name'], 'lat': s['lat'], 'lon': s['lon'], 'role': s.get('role', '')} for s in H['stations']],
            'sats': H['sats'],
            'families': [family_entry(f['key'], f['fixing']) for f in H['families']],
            'rx_fields': [list(f) for f in RX_FIELDS], 'sat_fields': [list(f) for f in SAT_FIELDS],
            'net_fields': [list(f) for f in NET_FIELDS],
            'dispositions': es[0].get('dispositions', []) if es else [],
            'events': {'tiers': E['tiers'], 'names': E['names'], 'actions': E['actions'], 'families': E.get('families', []),
                       'tier_family': E.get('tier_family', [])},
            'lanes': H['lanes'], 'archive': self.archive,
            'ranges': self.ranges(), 'scale_kinds': self.scale_kinds,
        }

    def ranges(self) -> dict:
        """A colour range per field over the run (colour_range; computed once)."""
        if self._ranges is None:
            out, self.scale_kinds = {}, {}
            for tag, table, fields in (('rx', self.rx, RX_FIELDS), ('sat', self.sat, SAT_FIELDS)):
                for i, (k, _l, kind, _u) in enumerate(fields):
                    out[f'{tag}.{k}'], sk = colour_range(k, kind, table[i][np.isfinite(table[i])])
                    if sk:
                        self.scale_kinds[f'{tag}.{k}'] = sk
            self._ranges = out
        return self._ranges

    def poll(self) -> int:
        """A finished run does not grow (the follow source's poll reads new lines)."""
        return 0

    def lists(self):
        """A finished run's lists are all in its description (the follow source's grow)."""
        return None

    def record(self, e: int) -> dict:
        """The record of epoch e."""
        ev = []
        A, H = self.A, self.H
        details = H['events']['details']
        for i in range(self.ev_off[e], self.ev_off[e + 1]):
            ev.append([int(A['ev_lane'][i]), int(self.ev_family[i]), int(A['ev_tier'][i]), int(A['ev_name'][i]),
                       int(A['ev_action'][i]), int(A['ev_sta'][i]), int(A['ev_sat'][i]),
                       details[int(A['ev_detail'][i])][:160] if 'ev_detail' in A else ''])
        fam = []
        if 'sum_links' in A:
            nf = len(H['families'])
            for f in range(nf):
                fam.append([int(A['sum_links'][0, f, e]), int(A['sum_links'][1, f, e])])
        digits_rx = [4, 5, 0, 3, 2, 0, 0, 0, 3, 4, 4]
        digits_sat = [4, 0, 2, 0, 0, 3, 0, 3]
        rx = np.stack([np.round(self.rx[i, :, e], d) for i, d in enumerate(digits_rx)], axis=1)
        sat = np.stack([np.round(self.sat[i, :, e], d) for i, d in enumerate(digits_sat)], axis=1)
        return {
            'e': int(e), 'ms': int(self.abs_ms[e]),
            'net': _round_list(self.net[:, e], 4),
            'fam': fam,
            'rx': [_round_list(r, 6) for r in rx],
            'sat': [_round_list(r, 6) for r in sat],
            'ev': ev,
        }
