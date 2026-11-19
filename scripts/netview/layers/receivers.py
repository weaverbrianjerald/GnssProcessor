"""Block 'receivers': per receiver and epoch.

    rp_c_rms, rp_p_rms  [receiver, epoch]  RMS of the post-fit code / phase residuals (m)   network_pf_rows.csv
    rp_c_max, rp_p_max                      largest |post-fit residual| (m)
    rp_c_n, rp_p_n      u16                 post-fit code / phase rows
    rp_nsat             u8                  satellites with a post-fit code row
    pk_c_s0, pk_p_s0                        robust scale of the normalized residuals |v| sqrt(w)   network_pf_keyed.csv
    rxs, rxs_sd         [state, receiver, epoch]  receiver code biases and inter-system biases (m)   network_rx_states.csv
    cg                  [field, receiver, epoch]  coordinate test: east, north, up offset (m), test value, reported,
                                                  frame epoch   network_congruence.csv
    sr_max, sr_n3                           largest normalized residual of the receiver's cells and the number above 3,
                                            on the epochs of the residual test   network_station_residuals.csv
"""
from __future__ import annotations

import numpy as np

import nv_tables as T
from nv_tables import F64, I64, STR

LAYER = 'receivers'
CG_FIELDS = [('d_e', 'east offset (m)'), ('d_n', 'north offset (m)'), ('d_u', 'up offset (m)'), ('t', 'test value (1 when the coordinate is right)'),
             ('reported', 'reported as an event'), ('frame', 'epoch with a datum event')]


def pf_rows(ctx):
    """network_pf_rows.csv with the indices every block needs (read once, kept on the context)."""
    if hasattr(ctx, '_pf'):
        return ctx._pf
    t = ctx.table('network_pf_rows.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 'signal': STR, 'kind': STR,
                  'resid_m': F64, 'w': F64, 'elev_deg': F64}, 'stochastic', 'post-fit residuals per signal',
                  'network.trace_postfit_rows')
    if not T.has(t, 'resid_m', 'station', 'sat', 'signal', 'kind', 'w', 'elev_deg'):
        ctx._pf = None
        return None
    e = T.epochs(t, ctx.ix)
    j = T.names(t, 'station', ctx.ix.sta)
    k = T.names(t, 'sat', ctx.ix.sat)
    kc, kinds = T.strings(t, 'kind')
    phase_code = kinds.index('phase') if 'phase' in kinds else -2
    sc, sigs = T.strings(t, 'signal')
    ok = (e >= 0) & (j >= 0) & (k >= 0) & (sc >= 0)
    ctx._pf = {
        'e': e[ok], 'j': j[ok], 'k': k[ok], 'phase': (kc[ok] == phase_code), 'sig': sc[ok], 'signals': sigs,
        'r': T.num(t, 'resid_m')[ok], 'w': T.num(t, 'w')[ok], 'el': T.num(t, 'elev_deg')[ok],
    }
    return ctx._pf


def _group(key, v, n):
    """count, RMS and largest |v| per key (0 .. n-1)."""
    cnt = np.bincount(key, minlength=n)
    s2 = np.bincount(key, weights=v * v, minlength=n)
    mx = np.zeros(n)
    np.maximum.at(mx, key, np.abs(v))
    with np.errstate(invalid='ignore', divide='ignore'):
        rms = np.sqrt(s2 / cnt)
    rms[cnt == 0] = np.nan
    mx[cnt == 0] = np.nan
    return cnt, rms, mx


def residual_summary(pf, n_ent, ent, ne, prefix):
    """RMS, largest and count of the post-fit residuals per entity and epoch, code and phase apart."""
    out = {}
    fin = np.isfinite(pf['r'])
    for tag, sel in (('c', ~pf['phase'] & fin), ('p', pf['phase'] & fin)):
        key = ent[sel].astype(np.int64) * ne + pf['e'][sel]
        cnt, rms, mx = _group(key, pf['r'][sel], n_ent * ne)
        out[f'{prefix}_{tag}_rms'] = rms.reshape(n_ent, ne).astype(np.float32)
        out[f'{prefix}_{tag}_max'] = mx.reshape(n_ent, ne).astype(np.float32)
        out[f'{prefix}_{tag}_n'] = np.minimum(cnt, 65535).reshape(n_ent, ne).astype(np.uint16)
    return out


def tracked_count(pf, n_ent, ent, other, n_other, ne):
    """Distinct partners (satellites of a receiver, or receivers of a satellite) with a code row, per epoch."""
    sel = ~pf['phase']
    key = (ent[sel].astype(np.int64) * n_other + other[sel]) * ne + pf['e'][sel]
    key = np.unique(key)
    ent_ep = (key // (n_other * ne)) * ne + key % ne
    return np.minimum(np.bincount(ent_ep, minlength=n_ent * ne), 255).reshape(n_ent, ne).astype(np.uint8)


def keyed(ctx, kind, index, n_ent, layer):
    """network_pf_keyed.csv rows of one kind (STA or SAT): robust scales of the normalized residuals."""
    t = ctx.table('network_pf_keyed.csv', {'week': I64, 'tow': F64, 'kind': STR, 'key': STR, 's0_code': F64,
                  's0_phase': F64, 'max_code': F64, 'max_phase': F64}, layer, 'normalized post-fit residuals',
                  'network.trace_postfit_keyed')
    if not T.has(t, 'kind', 'key', 's0_code', 's0_phase', 'max_code', 'max_phase'):
        return {}
    kc, kinds = T.strings(t, 'kind')
    want = kinds.index(kind) if kind in kinds else -2
    sel = kc == want
    e = T.epochs(t, ctx.ix)
    ent = T.names(t, 'key', index)
    ne = ctx.ix.n_ep
    out = {}
    for col, name in (('s0_code', 'c_s0'), ('s0_phase', 'p_s0'), ('max_code', 'c_zmax'), ('max_phase', 'p_zmax')):
        a = np.full((n_ent, ne), np.nan, np.float32)
        T.scatter(a, (ent, e), T.num(t, col).astype(np.float32), sel)
        out[name] = a
    return out


def build(ctx):
    ix, ne, ns = ctx.ix, ctx.ix.n_ep, len(ctx.ix.stations)
    H, A = {}, {}
    pf = pf_rows(ctx)
    if pf is not None:
        A.update(residual_summary(pf, ns, pf['j'], ne, 'rp'))
        A['rp_nsat'] = tracked_count(pf, ns, pf['j'], pf['k'], len(ix.sats), ne)
    for name, a in keyed(ctx, 'STA', ix.sta, ns, LAYER).items():
        A['pk_' + name] = a
    rx = ctx.table('network_rx_states.csv', {'week': I64, 'tow': F64, 'station': STR, 'kind': STR, 'key': STR,
                   'value_m': F64, 'std_m': F64}, LAYER, 'receiver code biases and inter-system biases', 'network.trace_rx_states')
    if T.has(rx, 'kind', 'key', 'station', 'value_m', 'std_m'):
        kc, kinds = T.strings(rx, 'kind')
        yc, keys = T.strings(rx, 'key')
        # the (kind, key) pairs other than the clock (the clock is in the core block)
        combo = kc.astype(np.int64) * (len(keys) + 1) + (yc + 1)
        uniq = [u for u in np.unique(combo) if kinds[u // (len(keys) + 1)] != 'CLK']
        labels = [[kinds[u // (len(keys) + 1)], keys[u % (len(keys) + 1) - 1] if u % (len(keys) + 1) else ''] for u in uniq]
        pos = {u: i for i, u in enumerate(uniq)}
        si = np.array([pos.get(u, -1) for u in combo], np.int64)
        e = T.epochs(rx, ix)
        j = T.names(rx, 'station', ix.sta)
        val = np.full((len(uniq), ns, ne), np.nan, np.float32)
        sd = np.full((len(uniq), ns, ne), np.nan, np.float32)
        T.scatter(val, (si, j, e), T.num(rx, 'value_m').astype(np.float32))
        T.scatter(sd, (si, j, e), T.num(rx, 'std_m').astype(np.float32))
        A['rxs'], A['rxs_sd'] = val, sd
        H['rxs_keys'] = labels
    cg = ctx.table('network_congruence.csv', {'week': I64, 'tow': F64, 'station': STR, 'label': STR, 'd_e': F64, 'd_n': F64,
                   'd_u': F64, 't': F64, 'reported': F64}, LAYER, 'coordinate test', 'network.congruence_screen')
    if T.has(cg, 'station', 'label', 'd_e', 'd_n', 'd_u', 't', 'reported'):
        e = T.epochs(cg, ix)
        j = T.names(cg, 'station', ix.sta)
        arr = np.full((len(CG_FIELDS), ns, ne), np.nan, np.float32)
        for i, (c, _l) in enumerate(CG_FIELDS[:5]):
            T.scatter(arr[i], (j, e), T.num(cg, c).astype(np.float32))
        lc, labels = T.strings(cg, 'label')
        frame = np.array([1.0 if v == 'FRAME' else 0.0 for v in labels] + [np.nan], np.float32)[lc]
        T.scatter(arr[5], (j, e), frame)
        A['cg'] = arr
        H['cg_fields'] = [[c, l] for c, l in CG_FIELDS]
    sr = ctx.table('network_station_residuals.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 's_norm': F64},
                   LAYER, 'residual test per receiver and satellite', 'network.trace_station_residuals')
    if T.has(sr, 's_norm', 'station'):
        e = T.epochs(sr, ix)
        j = T.names(sr, 'station', ix.sta)
        v = T.num(sr, 's_norm')
        ok = (e >= 0) & (j >= 0) & np.isfinite(v)
        key = j[ok].astype(np.int64) * ne + e[ok]
        mx = np.full(ns * ne, np.nan)
        cnt = np.bincount(key, minlength=ns * ne)
        mx[cnt > 0] = 0
        np.maximum.at(mx, key, v[ok])
        n3 = np.bincount(key, weights=(v[ok] > 3).astype(float), minlength=ns * ne)
        A['sr_max'] = mx.reshape(ns, ne).astype(np.float32)
        A['sr_n3'] = np.minimum(n3, 255).reshape(ns, ne).astype(np.uint8)
    if not A:
        return None
    return H, A
