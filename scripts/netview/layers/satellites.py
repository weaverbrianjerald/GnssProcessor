"""Block 'satellites': per satellite and epoch.

    sp_c_rms, sp_p_rms, sp_c_max, sp_p_max, sp_c_n, sp_p_n   post-fit residuals over the satellite's rows (m)
    sp_nrx      u8                    receivers with a post-fit code row of the satellite
    pk_c_s0, pk_p_s0                  robust scale of its normalized residuals   network_pf_keyed.csv
    sb, sb_sd   [signal, sat, epoch]  emitted code bias and its sigma (m)        network_sat_bias_states.csv
    sb_pin      u8                    1 where the code bias is a datum pin
    ag          [field, sat, epoch]   clock anchor guard: anchored, clock innovation (m), its sigma (m), the test,
                                      the test on clock minus phase bias       network_anchor_guard.csv
    ag_dec      u8 [sat, epoch]       the guard's decision (header ag_decisions; 255 none)
    dc_ep, dc_sat, dc_item, dc_step   discontinuity counter steps of the emitted products   network_solution.csv
"""
from __future__ import annotations

import numpy as np

import nv_tables as T
from layers.receivers import keyed, pf_rows, residual_summary, tracked_count
from nv_tables import F64, I64, STR

LAYER = 'satellites'
AG_FIELDS = [('anchored', 'anchored (1) or loose (0)'), ('e_clk', 'clock innovation, median removed (m)'),
             ('sqrtS_clk', 'sigma of the innovation (m)'), ('w_clk', 'clock test (sigma)'),
             ('w_phi', 'test of clock minus phase bias (sigma)')]


def build(ctx):
    ix, ne, nt = ctx.ix, ctx.ix.n_ep, len(ctx.ix.sats)
    H, A = {}, {}
    pf = pf_rows(ctx)
    if pf is not None:
        A.update(residual_summary(pf, nt, pf['k'], ne, 'sp'))
        A['sp_nrx'] = tracked_count(pf, nt, pf['k'], pf['j'], len(ix.stations), ne)
    for name, a in keyed(ctx, 'SAT', ix.sat, nt, LAYER).items():
        A['pk_' + name] = a
    sb = ctx.table('network_sat_bias_states.csv', {'week': I64, 'tow': F64, 'sat': STR, 'signal': STR, 'code_bias_m': F64,
                   'sigma_m': F64, 'pinned': F64}, LAYER, 'code-bias states', 'network.trace_sat_bias_states')
    if T.has(sb, 'signal', 'sat', 'code_bias_m', 'sigma_m', 'pinned'):
        sc, sigs = T.strings(sb, 'signal')
        e = T.epochs(sb, ix)
        k = T.names(sb, 'sat', ix.sat)
        val = np.full((len(sigs), nt, ne), np.nan, np.float32)
        sd = np.full((len(sigs), nt, ne), np.nan, np.float32)
        pin = np.zeros((len(sigs), nt, ne), np.uint8)
        T.scatter(val, (sc, k, e), T.num(sb, 'code_bias_m').astype(np.float32))
        T.scatter(sd, (sc, k, e), T.num(sb, 'sigma_m').astype(np.float32))
        T.scatter(pin, (sc, k, e), (T.num(sb, 'pinned') > 0).astype(np.uint8))
        A['sb'], A['sb_sd'], A['sb_pin'] = val, sd, pin
        H['sb_signals'] = sigs
    ag = ctx.table('network_anchor_guard.csv', dict([('week', I64), ('tow', F64), ('sat', STR), ('decision', STR)] +
                   [(c, F64) for c, _l in AG_FIELDS]), LAYER, 'clock anchor guard',
                   'the clock anchor guard with network.trace_events')
    if T.has(ag, 'sat', 'decision'):
        e = T.epochs(ag, ix)
        k = T.names(ag, 'sat', ix.sat)
        arr = np.full((len(AG_FIELDS), nt, ne), np.nan, np.float32)
        for i, (c, _l) in enumerate(AG_FIELDS):
            if c in ag.column_names:
                T.scatter(arr[i], (k, e), T.num(ag, c).astype(np.float32))
        dc, decs = T.strings(ag, 'decision')
        dec = np.full((nt, ne), 255, np.uint8)
        T.scatter(dec, (k, e), np.where(dc >= 0, dc, 255).astype(np.uint8))
        A['ag'], A['ag_dec'] = arr, dec
        H['ag_fields'] = [[c, l] for c, l in AG_FIELDS]
        H['ag_decisions'] = decs
    so = ctx.table('network_solution.csv', {'week': I64, 'tow': F64, 'sat': STR, 'kind': STR, 'signal': STR,
                   'disc_counter': F64}, LAYER, 'discontinuity counters of the products', 'always written by a network run')
    if T.has(so, 'disc_counter', 'sat', 'kind', 'signal'):
        e = T.epochs(so, ix)
        k = T.names(so, 'sat', ix.sat)
        kc, kinds = T.strings(so, 'kind')
        sc, sigs = T.strings(so, 'signal')
        item = kc.astype(np.int64) * (len(sigs) + 1) + (sc + 1)
        dcv = T.num(so, 'disc_counter')
        ok = (e >= 0) & (k >= 0) & np.isfinite(dcv)
        e, k, item, dcv = e[ok], k[ok], item[ok], dcv[ok]
        order = np.lexsort((e, item, k))
        e, k, item, dcv = e[order], k[order], item[order], dcv[order]
        same = (k[1:] == k[:-1]) & (item[1:] == item[:-1])
        step = np.zeros(len(dcv))
        step[1:] = np.where(same, dcv[1:] - dcv[:-1], 0)
        hit = step != 0
        items = sorted(set(item[hit].tolist()))
        pos = {u: i for i, u in enumerate(items)}
        labels = [(kinds[u // (len(sigs) + 1)] + (' ' + sigs[u % (len(sigs) + 1) - 1] if u % (len(sigs) + 1) else '')) for u in items]
        A['dc_ep'] = e[hit].astype(np.uint16)
        A['dc_sat'] = k[hit].astype(np.uint16)
        A['dc_item'] = np.array([pos[u] for u in item[hit]], np.uint8)
        A['dc_step'] = np.clip(step[hit], -32767, 32767).astype(np.int16)
        H['dc_items'] = labels
    if not A:
        return None
    return H, A
