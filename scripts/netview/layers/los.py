"""Block 'los': per line of sight (receiver x satellite) and epoch.

A line of sight is any (receiver, satellite) pair that one of the traces mentions; los_sta and los_sat give its two
ends, in the order of the receivers and then the satellites.  Every series is a dense [line of sight, epoch] array of
small integers (a sentinel where absent), which the gzip of the block shrinks where a line of sight is not tracked.

    lsig  [los, 5] u8          signal of each residual slot (index into pf_signals; 255 none): main code of band 1,
                               second code of band 1, code of band 2, phase of band 1, phase of band 2
    lres  [los, 5, epoch] i16  post-fit residual: code slots in cm, phase slots in 0.2 mm     network_pf_rows.csv
    lsd   [los, 5, epoch] u8   a priori sigma 1/sqrt(w), log scale 1e-4 .. 1e2 m (0 none)
    laz   [los, epoch] u8      azimuth, 0..254 for 0..360 degrees (255 none)              network_ionosphere.csv
    lion  [los, epoch] i16     slant ionosphere, cm (iono_total_m)
    lmw, lgf, lcp [los, epoch] i16  Melbourne-Wubbena (cm), geometry-free (mm), phase minus code on band 1 (cm), each
                               less its line of sight's median (lmw_off, lgf_off, lcp_off, m)   network_slip_raw.csv
    llli  [los, epoch] u8      loss-of-lock flags: bit 0 first signal, bit 1 second signal
    ltd, ltt [los, epoch] i16  ionosphere-free time-difference test: deviation from the network median and the
                               threshold, 0.5 mm; ltf u8 bits 1 flagged, 2 warm, 4 reference   network_if_slip.csv
    lsn   [los, epoch] i16     normalized residual of the residual test x 10            network_station_residuals.csv
    lams  [los, 2] u8          signal of each ambiguity slot (phase band 1, band 2; index into amb_signals)
    lam   [los, 2, epoch] i32  carried float ambiguity, mm, less its slot's median (lam_off, m)   network_amb_state.csv
    lav   [los, 2, epoch] u8   its sigma, log scale 1e-4 .. 1e2 m
    ae_los, ae_slot, ae_ep, ae_code   births, drops and erasures of the carried floats (codes: amb_events)
"""
from __future__ import annotations

import numpy as np

import nv_tables as T
from layers.receivers import pf_rows
from nv_tables import F64, I64, STR

LAYER = 'los'
NSLOT = 5
RES_SCALE = [100.0, 100.0, 100.0, 5000.0, 5000.0]       # code slots cm, phase slots 0.2 mm
SCALES = {'lion': 100.0, 'lmw': 100.0, 'lgf': 1000.0, 'lcp': 100.0, 'ltd': 2000.0, 'ltt': 2000.0, 'lsn': 10.0, 'lam': 1000.0}
DELTA = ['lion', 'lgf', 'lam']                             # coded as steps between epochs (smooth series)
SD_LOG = (-4.0, 2.0)


def _band(sig: str) -> int:
    """1 for the first band of a pair (RINEX band digit 1), 2 for the second."""
    return 1 if len(sig) > 1 and sig[1] == '1' else 2


def _group_median(key, v, n):
    """Median of v per key 0 .. n-1 (NaN where a key has no finite value)."""
    out = np.full(n, np.nan)
    ok = np.isfinite(v)
    key, v = key[ok], v[ok]
    order = np.argsort(key, kind='stable')
    ks, vs = key[order], v[order]
    starts = np.searchsorted(ks, np.arange(n))
    ends = np.searchsorted(ks, np.arange(n), side='right')
    for i in np.nonzero(ends > starts)[0]:
        out[i] = np.median(vs[starts[i]:ends[i]])
    return out


def build(ctx):
    ix, ne, ns, nt = ctx.ix, ctx.ix.n_ep, len(ctx.ix.stations), len(ctx.ix.sats)
    H, A = {}, {}
    pf = pf_rows(ctx)
    io = ctx.table('network_ionosphere.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 'azimuth_deg': F64,
                   'iono_total_m': F64}, 'atmosphere', 'slant ionosphere per line of sight', 'network.trace_ionosphere')
    sr = ctx.table('network_slip_raw.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 'mw_m': F64, 'gf_m': F64,
                   'l1_c1_m': F64, 'lli1': F64, 'lli2': F64}, 'slips', 'raw slip observables (MW, GF, phase minus code)',
                   'network.trace_slip_raw')
    ifs = ctx.table('network_if_slip.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 'dd_m': F64, 'med_m': F64,
                    'thr_m': F64, 'flagged': F64, 'carry_warm': F64, 'is_ref': F64}, 'slips',
                    'ionosphere-free time-difference slip test', 'network.trace_slip_events')
    snr = ctx.table('network_station_residuals.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 's_norm': F64},
                    'receivers', 'residual test per receiver and satellite', 'network.trace_station_residuals')
    am = ctx.table('network_amb_state.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 'signal': STR,
                   'value_abs_m': F64, 'var_m2': F64, 'event': STR}, 'los', 'carried float ambiguities', 'network.trace_amb_state')

    def ends(t):
        e = T.epochs(t, ix)
        j = T.names(t, 'station', ix.sta)
        k = T.names(t, 'sat', ix.sat)
        return e, j, k

    src = {}
    need = {'io': ('azimuth_deg', 'iono_total_m'), 'sr': ('mw_m', 'gf_m', 'l1_c1_m', 'lli1', 'lli2'),
            'if': ('dd_m', 'med_m', 'thr_m', 'flagged', 'carry_warm', 'is_ref'), 'sn': ('s_norm',),
            'am': ('signal', 'value_abs_m', 'var_m2', 'event')}
    for name, t in (('io', io), ('sr', sr), ('if', ifs), ('sn', snr), ('am', am)):
        if T.has(t, 'station', 'sat', *need[name]):
            src[name] = ends(t)
    keys = []
    if pf is not None:
        keys.append(pf['j'].astype(np.int64) * nt + pf['k'])
    for e, j, k in src.values():
        ok = (e >= 0) & (j >= 0) & (k >= 0)
        keys.append(j[ok].astype(np.int64) * nt + k[ok])
    if not keys:
        return None
    los_keys = np.unique(np.concatenate(keys))
    nl = len(los_keys)
    A['los_sta'] = (los_keys // nt).astype(np.uint16)
    A['los_sat'] = (los_keys % nt).astype(np.uint16)

    def lid(j, k):
        return np.searchsorted(los_keys, j.astype(np.int64) * nt + k)

    if pf is not None:
        _residual_slots(pf, lid, nl, ne, A, H)
    if 'io' in src:
        e, j, k = src['io']
        ok = (e >= 0) & (j >= 0) & (k >= 0)
        l = lid(j[ok], k[ok])
        az = T.num(io, 'azimuth_deg')[ok]
        laz = np.full((nl, ne), 255, np.uint8)
        laz[l, e[ok]] = np.where(np.isfinite(az), np.round(np.mod(az, 360.0) / 360.0 * 254), 255).astype(np.uint8)
        lion = np.full((nl, ne), np.iinfo(np.int16).min, np.int16)
        lion[l, e[ok]] = T.quantize(T.num(io, 'iono_total_m')[ok], SCALES['lion'])
        A['laz'], A['lion'] = laz, lion
    if 'sr' in src:
        e, j, k = src['sr']
        ok = (e >= 0) & (j >= 0) & (k >= 0)
        l = lid(j[ok], k[ok])
        for col, name in (('mw_m', 'lmw'), ('gf_m', 'lgf'), ('l1_c1_m', 'lcp')):
            scale = SCALES[name]
            v = T.num(sr, col)[ok]
            off = _group_median(l, v, nl)
            arr = np.full((nl, ne), np.iinfo(np.int16).min, np.int16)
            arr[l, e[ok]] = T.quantize(v - off[l], scale)
            A[name], A[name + '_off'] = arr, off.astype(np.float32)
        lli = np.zeros((nl, ne), np.uint8)
        b = ((np.nan_to_num(T.num(sr, 'lli1')[ok]).astype(np.int64) & 1) |
             ((np.nan_to_num(T.num(sr, 'lli2')[ok]).astype(np.int64) & 1) << 1)).astype(np.uint8)
        lli[l, e[ok]] = b
        A['llli'] = lli
    if 'if' in src:
        e, j, k = src['if']
        ok = (e >= 0) & (j >= 0) & (k >= 0)
        l = lid(j[ok], k[ok])
        sentinel = np.iinfo(np.int16).min
        ltd = np.full((nl, ne), sentinel, np.int16)
        ltt = np.full((nl, ne), sentinel, np.int16)
        ltf = np.zeros((nl, ne), np.uint8)
        ltd[l, e[ok]] = T.quantize((T.num(ifs, 'dd_m') - T.num(ifs, 'med_m'))[ok], SCALES['ltd'])
        ltt[l, e[ok]] = T.quantize(T.num(ifs, 'thr_m')[ok], SCALES['ltt'])
        bits = ((T.num(ifs, 'flagged') > 0).astype(np.uint8) | ((T.num(ifs, 'carry_warm') > 0).astype(np.uint8) << 1) |
                ((T.num(ifs, 'is_ref') > 0).astype(np.uint8) << 2))
        ltf[l, e[ok]] = bits[ok]
        A['ltd'], A['ltt'], A['ltf'] = ltd, ltt, ltf
    if 'sn' in src:
        e, j, k = src['sn']
        ok = (e >= 0) & (j >= 0) & (k >= 0)
        lsn = np.full((nl, ne), np.iinfo(np.int16).min, np.int16)
        lsn[lid(j[ok], k[ok]), e[ok]] = T.quantize(T.num(snr, 's_norm')[ok], SCALES['lsn'])
        A['lsn'] = lsn
    if 'am' in src:
        _ambiguities(am, src['am'], lid, nl, ne, A, H)
    for name in DELTA:
        if name in A:
            A[name], A[name + '_dz'] = T.delta_rows(A[name], np.iinfo(A[name].dtype).min)
    H['res_scale'] = RES_SCALE
    H['scales'] = SCALES
    H['delta'] = [n for n in DELTA if n in A]
    H['sd_log'] = list(SD_LOG)
    return H, A


def _residual_slots(pf, lid, nl, ne, A, H):
    sigs = pf['signals']
    band = np.array([_band(s) for s in sigs], np.int64)
    l = lid(pf['j'], pf['k'])
    s = pf['sig'].astype(np.int64)
    ph = pf['phase']
    pair = l * len(sigs) + s
    upair, inv = np.unique(pair, return_inverse=True)
    pl, ps = upair // len(sigs), upair % len(sigs)
    w = pf['w']
    logw = np.where(np.isfinite(w) & (w > 0), np.log10(np.where(w > 0, w, 1)), np.nan)
    mean_logw = np.bincount(inv, weights=np.nan_to_num(logw), minlength=len(upair)) / np.maximum(
        np.bincount(inv, weights=np.isfinite(logw).astype(float), minlength=len(upair)), 1)
    is_phase = np.zeros(len(upair), bool)
    is_phase[inv[ph]] = True
    slot = np.full(len(upair), -1, np.int64)
    slot[is_phase & (band[ps] == 1)] = 3
    slot[is_phase & (band[ps] == 2)] = 4
    slot[~is_phase & (band[ps] == 2)] = 2
    c1 = np.nonzero(~is_phase & (band[ps] == 1))[0]
    order = c1[np.lexsort((-mean_logw[c1], pl[c1]))]          # per line of sight, heaviest weight first
    first = np.ones(len(order), bool)
    first[1:] = pl[order][1:] != pl[order][:-1]
    slot[order[first]] = 0
    slot[order[~first]] = 1
    lsig = np.full((nl, NSLOT), 255, np.uint8)
    ok = slot >= 0
    lsig[pl[ok], slot[ok]] = ps[ok]
    row_slot = slot[inv]
    lres = np.full((nl, NSLOT, ne), np.iinfo(np.int16).min, np.int16)
    lsd = np.zeros((nl, NSLOT, ne), np.uint8)
    for sl in range(NSLOT):
        m = row_slot == sl
        lres[l[m], sl, pf['e'][m]] = T.quantize(pf['r'][m], RES_SCALE[sl])
        with np.errstate(divide='ignore', invalid='ignore'):
            lsd[l[m], sl, pf['e'][m]] = T.log_code(1.0 / np.sqrt(w[m]), *SD_LOG)
    A['lsig'], A['lres'], A['lsd'] = lsig, lres, lsd
    H['pf_signals'] = sigs


def _ambiguities(am, ends, lid, nl, ne, A, H):
    e, j, k = ends
    sc, sigs = T.strings(am, 'signal')
    ok = (e >= 0) & (j >= 0) & (k >= 0) & (sc >= 0)
    band = np.array([_band(s) for s in sigs] + [0], np.int64)
    slot = band[sc] - 1
    l = lid(j[ok], k[ok])
    sl, ee, sg = slot[ok], e[ok], sc[ok]
    val = T.num(am, 'value_abs_m')[ok]
    var = T.num(am, 'var_m2')[ok]
    evc, evs = T.strings(am, 'event')
    ev = evc[ok]
    plain = ev < 0                                       # value rows (an event row repeats the value at its moment)
    off = _group_median(l * 2 + sl, np.where(plain, val, np.nan), nl * 2)
    lam = np.full((nl, 2, ne), np.iinfo(np.int32).min, np.int32)
    lav = np.zeros((nl, 2, ne), np.uint8)
    keep = plain | (ev >= 0)
    lam[l[keep], sl[keep], ee[keep]] = T.quantize(val[keep] - off[(l * 2 + sl)[keep]], SCALES['lam'], np.int32)
    with np.errstate(invalid='ignore'):
        lav[l[keep], sl[keep], ee[keep]] = T.log_code(np.sqrt(var[keep]), *SD_LOG)
    lams = np.full((nl, 2), 255, np.uint8)
    lams[l, sl] = sg
    A['lam'], A['lav'], A['lams'] = lam, lav, lams
    A['lam_off'] = off.reshape(nl, 2).astype(np.float64)
    H['amb_signals'] = sigs
    evm = ev >= 0
    A['ae_los'] = l[evm].astype(np.uint16)
    A['ae_slot'] = sl[evm].astype(np.uint8)
    A['ae_ep'] = ee[evm].astype(np.uint16)
    A['ae_code'] = ev[evm].astype(np.uint8)
    H['amb_events'] = evs
