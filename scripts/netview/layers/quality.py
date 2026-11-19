"""Block 'quality': the stochastic model and the tests.

    vcf_ep [row epoch] and vcf_<f|g|ef|eg|rho|n|id> [class, row]   variance factors per class of observation
                    (constellation and signal, and the code / phase aggregates)       network_vcf.csv
    eb_n, eb_rms, eb_zrms, eb_sd   [signal, elevation bin]   post-fit residuals by elevation (5 degree bins):
                    count, RMS (m), RMS of the normalized residual, median a priori sigma (m)   network_pf_rows.csv
    hb_rms, hb_zrms                [signal, hour]            the same by hour of the run
    zh  [signal, bin]   histogram of the normalized residual v sqrt(w), -6 .. 6 in 0.25 (first and last bins: tails)
    bb_*            rows of network_block_baarda.csv (per constellation, every few minutes)
    mdb_*           rows of network_mdb.csv (smallest detectable errors per constellation and class)
    pr_*            rows of network_prior_residual.csv (tests of the carried priors per family)
    ra_*            rows of network_raim_attrib.csv (residual attribution to a receiver, satellite or cell)
    if_*            the flagged rows of network_if_slip.csv (the ionosphere-free time-difference slip test)
"""
from __future__ import annotations

import numpy as np

import nv_tables as T
from layers.receivers import pf_rows
from nv_tables import F64, I64, STR

EL_BIN = 5.0
Z_LO, Z_HI, Z_STEP = -6.0, 6.0, 0.25


def _cols(t, names, dtype=np.float32):
    return {n: T.num(t, n).astype(dtype) for n in names if n in t.column_names}


def build(ctx):
    ix, ne = ctx.ix, ctx.ix.n_ep
    H, A = {}, {}
    vcf = ctx.table('network_vcf.csv', {'week': I64, 'tow': F64, 'class': STR, 'n': F64, 'rho': F64, 'f': F64, 'g': F64,
                    'ewma_f': F64, 'ewma_g': F64, 'identifiable': F64}, 'stochastic', 'variance factors', 'network.vcf')
    if T.has(vcf, 'class', 'tow'):
        e = T.epochs(vcf, ix)
        cc, classes = T.strings(vcf, 'class')
        eps = np.unique(e[e >= 0])
        col = np.searchsorted(eps, e)
        ok = (e >= 0) & (cc >= 0)
        A['vcf_ep'] = eps.astype(np.uint16)
        for src, name in (('f', 'f'), ('g', 'g'), ('ewma_f', 'ef'), ('ewma_g', 'eg'), ('rho', 'rho'), ('n', 'n'), ('identifiable', 'id')):
            arr = np.full((len(classes), len(eps)), np.nan, np.float32)
            if src in vcf.column_names:
                arr[cc[ok], col[ok]] = T.num(vcf, src)[ok]
            A['vcf_' + name] = arr
        H['vcf_classes'] = classes
    pf = pf_rows(ctx)
    if pf is not None:
        A.update(_binned(pf, ne, ctx))
        H['pf_signals'] = pf['signals']
        H['el_bin'] = EL_BIN
        H['z_hist'] = [Z_LO, Z_HI, Z_STEP]
    bb = ctx.table('network_block_baarda.csv', {'week': I64, 'tow': F64, 'family': STR, 'label': STR, 'n_blocks': F64,
                   'n_rows': F64, 'n_gt_3': F64, 'n_gt_4': F64, 'n_gt_6': F64, 'n_gt_12': F64, 'max_t': F64,
                   'max_station': STR, 'max_sat': STR, 'sigma0': F64}, 'stochastic', 'outlier test of receiver-satellite blocks',
                   'network.block_baarda')
    if T.has(bb, 'family', 'tow'):
        A.update(_row_arrays(ctx, bb, 'bb', ['n_blocks', 'n_rows', 'n_gt_3', 'n_gt_4', 'n_gt_6', 'n_gt_12', 'max_t', 'sigma0']))
        fc, fams = T.strings(bb, 'family')
        A['bb_fam'] = np.where(fc >= 0, fc, 255).astype(np.uint8)[A.pop('_keep')]
        H['bb_families'] = fams
    mdb = ctx.table('network_mdb.csv', {'week': I64, 'tow': F64, 'const': STR, 'class': STR, 'n_rows': F64, 'n_inf': F64,
                    'mdb_med_m': F64, 'mdb_p95_m': F64, 'mdb_max_m': F64, 'mdb_scaled_med_m': F64, 'mdb_scaled_p95_m': F64,
                    'worst_station': STR, 'worst_sat': STR}, 'stochastic', 'smallest detectable errors', 'network.mdb')
    if T.has(mdb, 'class', 'const', 'tow'):
        A.update(_row_arrays(ctx, mdb, 'mdb', ['n_rows', 'n_inf', 'mdb_med_m', 'mdb_p95_m', 'mdb_max_m', 'mdb_scaled_med_m',
                                               'mdb_scaled_p95_m'], sta='worst_station', sat='worst_sat'))
        keep = A.pop('_keep')
        cc, consts = T.strings(mdb, 'const')
        kc, classes = T.strings(mdb, 'class')
        A['mdb_const'] = np.where(cc >= 0, cc, 255).astype(np.uint8)[keep]
        A['mdb_class'] = np.where(kc >= 0, kc, 255).astype(np.uint8)[keep]
        H['mdb_consts'], H['mdb_classes'] = consts, classes
    pr = ctx.table('network_prior_residual.csv', {'week': I64, 'tow': F64, 'family': STR, 'label': STR, 'n_rows': F64,
                   'n_gt_3': F64, 'n_gt_4': F64, 'n_gt_6': F64, 'max_abs_t': F64, 'max_station': STR, 'max_sat': STR,
                   'sigma0': F64}, 'stochastic', 'tests of the carried priors', 'network.prior_residual_census')
    if T.has(pr, 'family', 'label', 'tow'):
        A.update(_row_arrays(ctx, pr, 'pr', ['n_rows', 'n_gt_3', 'n_gt_4', 'n_gt_6', 'max_abs_t', 'sigma0'],
                             sta='max_station', sat='max_sat'))
        keep = A.pop('_keep')
        fc, fams = T.strings(pr, 'family')
        lc, labels = T.strings(pr, 'label')
        A['pr_fam'] = np.where(fc >= 0, fc, 255).astype(np.uint8)[keep]
        A['pr_label'] = np.where(lc >= 0, lc, 255).astype(np.uint8)[keep]
        H['pr_families'], H['pr_labels'] = fams, labels
    ra = ctx.table('network_raim_attrib.csv', {'week': I64, 'tow': F64, 'dim': STR, 'entity': STR, 'effect': F64,
                   'prominence': F64, 'acted': F64}, 'stochastic', 'attribution of large residuals', 'network.raim_attribution')
    if T.has(ra, 'dim', 'entity', 'effect', 'prominence', 'acted'):
        e = T.epochs(ra, ix)
        ok = e >= 0
        dc, dims = T.strings(ra, 'dim')
        ec, ents = T.strings(ra, 'entity')
        A['ra_ep'] = e[ok].astype(np.uint16)
        A['ra_dim'] = np.where(dc >= 0, dc, 255).astype(np.uint8)[ok]
        A['ra_ent'] = ec[ok].astype(np.int32)
        A['ra_effect'] = T.num(ra, 'effect').astype(np.float32)[ok]
        A['ra_prom'] = T.num(ra, 'prominence').astype(np.float32)[ok]
        A['ra_acted'] = (T.num(ra, 'acted') > 0).astype(np.uint8)[ok]
        H['ra_dims'], H['ra_entities'] = dims, ents
    sl = ctx.table('network_if_slip.csv', {'week': I64, 'tow': F64, 'station': STR, 'sat': STR, 'elev_deg': F64, 'dd_m': F64,
                   'med_m': F64, 'thr_m': F64, 'flagged': F64}, 'slips', 'ionosphere-free time-difference slip test',
                   'network.trace_slip_events')
    if T.has(sl, 'flagged', 'station', 'sat', 'dd_m', 'med_m', 'thr_m', 'elev_deg'):
        fl = T.num(sl, 'flagged') > 0
        e = T.epochs(sl, ix)
        j = T.names(sl, 'station', ix.sta)
        k = T.names(sl, 'sat', ix.sat)
        ok = fl & (e >= 0) & (j >= 0) & (k >= 0)
        A['if_ep'] = e[ok].astype(np.uint16)
        A['if_sta'] = j[ok].astype(np.uint16)
        A['if_sat'] = k[ok].astype(np.uint16)
        A['if_dev'] = (T.num(sl, 'dd_m') - T.num(sl, 'med_m')).astype(np.float32)[ok]
        A['if_thr'] = T.num(sl, 'thr_m').astype(np.float32)[ok]
        A['if_el'] = T.num(sl, 'elev_deg').astype(np.float32)[ok]
        H['if_tests'] = int(sl.num_rows)
    if not A:
        return None
    return H, A


def _row_arrays(ctx, t, prefix, num_cols, sta='max_station', sat='max_sat'):
    """Columns of a row table as arrays named prefix_col, with the epoch, worst receiver and satellite indices."""
    e = T.epochs(t, ctx.ix)
    keep = e >= 0
    out = {f'{prefix}_ep': e[keep].astype(np.uint16), '_keep': keep}
    for c in num_cols:
        if c in t.column_names:
            out[f'{prefix}_{c}'] = T.num(t, c).astype(np.float32)[keep]
    if sta in t.column_names:
        out[f'{prefix}_sta'] = T.names(t, sta, ctx.ix.sta).astype(np.int16)[keep]
    if sat in t.column_names:
        out[f'{prefix}_sat'] = T.names(t, sat, ctx.ix.sat).astype(np.int16)[keep]
    return out


def _binned(pf, ne, ctx):
    """Residual statistics by signal and elevation bin, by signal and hour, and histograms of the normalized residual."""
    ns = len(pf['signals'])
    nb = int(round(90 / EL_BIN))
    r, w, el, sig = pf['r'], pf['w'], pf['el'], pf['sig'].astype(np.int64)
    fin = np.isfinite(r) & np.isfinite(w) & (w > 0)
    z = np.where(fin, r * np.sqrt(np.where(fin, w, 0)), np.nan)
    sd = np.where(fin, 1 / np.sqrt(np.where(fin, w, 1)), np.nan)
    b = np.clip((np.nan_to_num(el, nan=-1) // EL_BIN).astype(np.int64), -1, nb - 1)
    ok = fin & (b >= 0)
    key = sig[ok] * nb + b[ok]
    cnt = np.bincount(key, minlength=ns * nb)
    with np.errstate(invalid='ignore', divide='ignore'):
        rms = np.sqrt(np.bincount(key, weights=r[ok] ** 2, minlength=ns * nb) / cnt)
        zrms = np.sqrt(np.bincount(key, weights=z[ok] ** 2, minlength=ns * nb) / cnt)
    med = np.full(ns * nb, np.nan)
    order = np.argsort(key, kind='stable')
    ks, vs = key[order], sd[ok][order]
    starts = np.searchsorted(ks, np.arange(ns * nb))
    ends = np.searchsorted(ks, np.arange(ns * nb), side='right')
    for i in np.nonzero(ends > starts)[0]:
        med[i] = np.median(vs[starts[i]:ends[i]])
    out = {'eb_n': cnt.reshape(ns, nb).astype(np.uint32), 'eb_rms': rms.reshape(ns, nb).astype(np.float32),
           'eb_zrms': zrms.reshape(ns, nb).astype(np.float32), 'eb_sd': med.reshape(ns, nb).astype(np.float32)}
    ep_ms = np.asarray(ctx.ix.t_ms, np.int64)
    hour = ((ep_ms - ep_ms[0]) // 3600000).astype(np.int64)
    nh = int(hour[-1]) + 1
    hk = sig[fin] * nh + hour[pf['e'][fin]]
    hc = np.bincount(hk, minlength=ns * nh)
    with np.errstate(invalid='ignore', divide='ignore'):
        out['hb_rms'] = np.sqrt(np.bincount(hk, weights=r[fin] ** 2, minlength=ns * nh) / hc).reshape(ns, nh).astype(np.float32)
        out['hb_zrms'] = np.sqrt(np.bincount(hk, weights=z[fin] ** 2, minlength=ns * nh) / hc).reshape(ns, nh).astype(np.float32)
    nz = int(round((Z_HI - Z_LO) / Z_STEP)) + 2
    zb = np.clip(np.floor((z[fin] - Z_LO) / Z_STEP).astype(np.int64) + 1, 0, nz - 1)
    out['zh'] = np.bincount(sig[fin] * nz + zb, minlength=ns * nz).reshape(ns, nz).astype(np.uint32)
    return out
