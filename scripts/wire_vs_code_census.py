#!/usr/bin/env python
"""
wire_vs_code_census.py - the wire-vs-CODE consistency census (SPEC_event_harness section 24.5).

Read-only. Compares the network wire (products.txt C / P / B records) against the
CODE 30 s RINEX clock and the CODE OSB .BIA the run's a-priori identifies, on the
consumer-relevant, datum-invariant contrast

    D(s,L,t) = X(s,L,t) - cm_L(t),      cm_L(t) = median over satellites present at t of X(.,L,t)

where X is the satellite-side term the consumer adds to its predicted observable,
network minus CODE. A float user is indifferent to any per-satellite constant and
to any per-epoch common-mode difference; D's within-arc variation is what reaches
the position. A second common-mode estimator (the per-epoch median of the
satellite-wise differences, integrated, immune to a rotating satellite set) is
reported beside the pre-registered per-epoch median so the reading is instrument-robust.

Every field order, unit and sign below was taken from the source, not from memory:

  products.txt writer  src/core/products/NetworkProductIO.cpp
    E,week,tow,iod_ssr,provider,solution,datum_changed[,consts]   :593-601
    R,sat[,sat...]  (retro-bump / declared re-realization set)    :620-623
    C,sat,clock_c0_m[,c1_m_s,clock_disc][,key=val...]             :633-637
    B,sat,signal,code_bias_m[,code_disc]                          :671-673
    P,sat,signal,bias_m,signal_integer,wl_integer[,disc][,sigma=] :677-680
  products.txt reader  src/core/products/NetworkProductIO.cpp (LineAssembler)
    C: f[2]=clock_c0_m, f[3]=c1 (if no '='), f[4]=clock disc       :145-159
    B: f[3]=bias_m, f[4]=code disc                                 :184-188
    P: f[3]=bias_m, f[4]=si, f[5]=wl, f[6+]=disc | sigma=          :189-206
  units  include/products/NetworkProductEmitter.hpp
    clock_c0_m "Estimable satellite clock C0 (m)"                  :82
    NetworkPhaseBias::bias_m "(m)", code_bias_m "(m)"              :71, :120
  producer fill  Estimator.cpp:1042 clock_c0_m = x_s(c) (the residual-clock column);
    EmitStore.cpp:354 overrides it with the CLK-R held/rebased value when armed.

  what the consumer applies (signs)
    emitter -> store, all +1:  rec.c0 = clock_c0_m                 NetworkProductEmitter.cpp:273
                               rec.bias_per_signal[sig] = bias     NetworkProductEmitter.cpp:385
                               entry.bias_m = npb.bias_m           NetworkProductEmitter.cpp:424
    store getters return the stored value unchanged                SsrCorrectionStore.cpp:308-345
    clock: clock_corr(s) = interpolateClock(CLK product) [seconds] GnssCorrections.cpp:161
           clock_corr += applyClockToBias(c0,c1,c2,hr,dt)          GnssCorrections.cpp:343-345
           applyClockToBias = (c0 + c1 dt + c2 dt^2 + hr)/c [m->s] SsrCorrectionStore.cpp:666-670
           => the C record is a delta on the CODE clock: c*dt_net = c*dt_CODE + C_net
    modeled = range - c*sat_clock - c*rel + ... + bias_corr        PppModel.cpp:1489-1495
           bias_corr = enable_code_phase_bias ? getBiasCorrection : 0   PppModel.cpp:1485-1487
           getBiasCorrection returns the OSB entry's bias (m), +1  GnssCorrections.cpp:1766-1778
    SSR biases: kSsrCodeBiasSign = +1, kSsrPhaseBiasSign = +1      MeasurementModel.cpp:1608-1609
           predicted = modeled + ... + sat_clock_term + ssr_bias_term    MeasurementModel.cpp:1642-1644
           (sat_clock_term = -x(sc_idx) is the producer's residual-clock state, :1587;
            "obs.modeled has already removed the a-priori product clock", :1569-1576)
    S-line bias_sign is cross-checked only, never applied           NetworkProductIO.cpp:847
    producer posture: enable_code_phase_bias = off (W6 forces it)  seed gnss_config.log; the wire
           P/B are therefore absolute biases in the producer's datum, not residuals to the OSB
    full consumer inherits enable_code_phase_bias=false, estimate_sat_biases=!consume_clock
                                                                   GnssConfigLoadNetwork.cpp:10629-10640
  => applied-term convention used here (network minus CODE, common -c*dt_CODE cancels):
       X_phase(s,L,t) = -C_net(s,t) + PB_net(s,L,t) - OSB_m(s,L)
       X_code (s,X,t) = -C_net(s,t) + B_net(s,X,t)  - OSB_m(s,X)
       raw clock contrast  c*dt_net - c*dt_CODE = C_net(s,t)   (de-medianed per epoch)
     plus the decomposition kinds  pb = PB_net - OSB  and  cb = B_net - OSB  (each de-medianed),
     and the spec-literal same-sign form (+C_net + PB_net - OSB) as a control only.

  CODE CLK  src/core/products/ProductParser.cpp
    "AS sat Y M D h m s NV clock_bias [sigma]" whitespace tokens, seconds  :509-548
    interpolateClock: exact node, else linear, bracket > 65 s rejected      GnssCorrections.cpp:483
  CODE BIA  src/core/products/ProductParser.cpp
    line trimmed (:1051-1052) then fixed columns: type[0,4) svn[5,4) prn[10,3) station[14,9)
    obs1[24,4) obs2[29,4) start[34,14) end[49,14) unit[64,4) bias[69,21) std[91,13)  :1121-1131
    bias_m = ns * c * 1e-9  (+1)                                            :1240
  a-priori identity  ApplicationNetworkSetup.cpp:1097-1101 = getProductPath("SP3")+"+"+getProductPath("CLK");
    getProductPath returns the first product path with that extension (GnssConfig.cpp:67-78), so a
    three-day CLK set (128/129/130, "1045243 clocks" in the seed log) is named by its lexically-first
    file; the run's epochs (DOY 129) are served by the DOY-129 file (the 128 file ends 23:59:30 of 128).

Usage (defaults reproduce the section 24.5 census):
    python scripts/wire_vs_code_census.py
    python scripts/wire_vs_code_census.py --out scratch/census            # + per-arc CSVs / summary JSON
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys
from collections import defaultdict
from datetime import datetime, timedelta

import numpy as np

C_LIGHT = 299792458.0          # include/common/Constants.hpp:31
GPS_EPOCH = datetime(1980, 1, 6)
WEEK_S = 604800.0
CLK_MAX_BRACKET_S = 65.0       # GnssCorrections.cpp:483 MAX_BRACKET_GAP
F1, F2 = 1575.42e6, 1227.60e6  # GPS L1 / L2 (for the pinned-pair IF reading of the code constants)
IF1, IF2 = F1 * F1 / (F1 * F1 - F2 * F2), -F2 * F2 / (F1 * F1 - F2 * F2)

DATA_ROOT_FALLBACK = 'C:/GnssProcessor'
KINDS = ('phase', 'code', 'clk', 'pb', 'cb')


# ----------------------------------------------------------------------------
# parsing
# ----------------------------------------------------------------------------
def parse_products(path):
    """products.txt -> (epochs, meta).  epochs: dicts in file order with tow (int s), week,
    datum_changed, consts, retro (set), C {sat:(c0,c1,disc)}, P {(sat,sig):(bias,si,wl,disc,sigma)},
    B {(sat,sig):(bias,disc)}."""
    epochs, cur = [], None
    meta = {'S': None, 'n_lines': 0, 'unknown_tags': defaultdict(int), 'off_grid_ms': 0}
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            meta['n_lines'] += 1
            line = raw.rstrip('\r\n')
            if not line or line[0] == '#':
                continue
            f = line.split(',')
            tag = f[0]
            if tag == 'E':                       # writer :593, reader :117-143
                tow = float(f[2])
                if abs(tow - 30 * round(tow / 30.0)) > 1e-3:
                    meta['off_grid_ms'] += 1
                cur = {'week': int(f[1]), 'tow': int(round(tow)), 'tow_raw': tow,
                       'datum_changed': (len(f) > 6 and int(f[6]) != 0),
                       'consts': set(f[7]) if len(f) > 7 else set(),
                       'retro': set(), 'C': {}, 'P': {}, 'B': {}}
                epochs.append(cur)
            elif tag == 'S':                     # contract line (header precedes the first E)
                if meta['S'] is None:
                    meta['S'] = line
            elif cur is None:
                continue
            elif tag == 'C':                     # writer :633, reader :145-159
                c0 = float(f[2]); k = 3; c1 = 0.0; disc = None
                if k < len(f) and '=' not in f[k]:
                    c1 = float(f[k]); k += 1
                if k < len(f) and '=' not in f[k]:
                    disc = int(f[k]); k += 1
                cur['C'][f[1]] = (c0, c1, disc)
            elif tag == 'B':                     # writer :671, reader :184-188
                # A field holding '=' is a key=value token (isc=, card CLV's lvl=), never the
                # positional code disc counter, exactly as the C++ reader treats it.
                cur['B'][(f[1], f[2])] = (float(f[3]),
                                          int(f[4]) if len(f) > 4 and '=' not in f[4] else None)
            elif tag == 'P':                     # writer :677, reader :189-206
                disc = None; sigma = None
                for tok in f[6:]:
                    if tok.startswith('sigma='):
                        sigma = float(tok[6:])
                    elif '=' not in tok:
                        disc = int(tok)
                cur['P'][(f[1], f[2])] = (float(f[3]), int(f[4]), int(f[5]), disc, sigma)
            elif tag == 'R':                     # writer :620-623, reader :207-210
                cur['retro'].update(x for x in f[1:] if x)
            elif tag in ('H', 'U', 'G', 'I', 'K'):
                pass
            else:
                meta['unknown_tags'][tag] += 1
    return epochs, meta


def civil_to_gps_seconds(y, mo, d, h, mi, s):
    return ((datetime(y, mo, d, h, mi) + timedelta(seconds=s)) - GPS_EPOCH).total_seconds()


def parse_clk(paths, systems=('G',)):
    """RINEX CLK 'AS' records (ProductParser.cpp:509-548): {sat: {t_gps_s(int): bias_s}}."""
    clk = defaultdict(dict)
    n = 0
    for path in paths:
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if not line.startswith('AS '):
                    continue
                f = line.split()
                if len(f) < 10 or f[1][0] not in systems:
                    continue
                t = civil_to_gps_seconds(int(f[2]), int(f[3]), int(f[4]), int(f[5]), int(f[6]), float(f[7]))
                clk[f[1]][int(round(t))] = float(f[9])
                n += 1
    return clk, n


_clk_sorted = {}


def clk_lookup(clk, sat, t):
    """Mirror GnssCorrections::interpolateClock: exact node, else linear within a 65 s bracket."""
    d = clk.get(sat)
    if not d:
        return None, None
    v = d.get(t)
    if v is not None:
        return v, 'exact'
    keys = _clk_sorted.get(sat)
    if keys is None:
        keys = np.array(sorted(d.keys()))
        _clk_sorted[sat] = keys
    i = int(np.searchsorted(keys, t))
    if i == 0 or i >= len(keys):
        return None, None
    lo, hi = int(keys[i - 1]), int(keys[i])
    if hi - lo > CLK_MAX_BRACKET_S:
        return None, None
    w = (t - lo) / (hi - lo)
    return d[lo] * (1 - w) + d[hi] * w, 'interp'


def parse_bia(path, doy_key, systems=('G',)):
    """Satellite OSB records (ProductParser.cpp:1051-1052 trim, :1121-1131 columns, :1240 ns->m).
    -> ({(sat, signal): bias_m} valid on doy_key 'YYYY:DOY', set of signals)."""
    osb = {}
    sigs = set()
    y, doy = (int(x) for x in doy_key.split(':'))
    in_block = False
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith('+BIAS/SOLUTION'):
                in_block = True; continue
            if line.startswith('-BIAS/SOLUTION'):
                break
            if not in_block or not line.startswith('OSB ') or len(line) < 70:
                continue
            prn = line[10:13].strip(); station = line[14:23].strip()
            obs1 = line[24:28].strip(); obs2 = line[29:33].strip()
            start = line[34:48].strip(); end = line[49:63].strip(); unit = line[64:68].strip()
            bias = line[69:90].strip()
            if station or not prn or prn[0] not in systems or unit != 'ns':
                continue
            sig = obs2 if obs2 else obs1
            t0 = tuple(int(x) for x in start.split(':')); t1 = tuple(int(x) for x in end.split(':'))
            if not (t0 <= (y, doy, 1) <= t1):
                continue
            osb[(prn, sig)] = float(bias) * C_LIGHT * 1e-9
            sigs.add(sig)
    return osb, sigs


# ----------------------------------------------------------------------------
# census
# ----------------------------------------------------------------------------
def collect_raw(epochs, clk, osb, t_lo=None, t_hi=None, systems=('G',)):
    """Raw per-epoch X per kind before any de-meaning, plus the event bookkeeping and counters.
    raw[kind][key][t] = x ; disc[kind][key][t] = record disc ; ev[sat][t] = (clk_disc, retro, datum_changed).
    `systems` (card GAL): the constellation letters kept; a C record of any other letter is
    counted as non_gps_C (the pre-card name), so the default reproduces the GPS-only census."""
    raw = {k: defaultdict(dict) for k in KINDS + ('phase_spec', 'clk_full')}
    disc = {k: defaultdict(dict) for k in KINDS}
    ev = defaultdict(dict)
    cnt = defaultdict(int)
    missing_bia = defaultdict(int)
    no_clk_sat = defaultdict(int)
    tows = []
    for ep in epochs:
        t = ep['tow']
        if (t_lo is not None and t < t_lo) or (t_hi is not None and t > t_hi):
            continue
        tows.append(t)
        tabs = int(round(ep['week'] * WEEK_S + t))
        cnt['epochs'] += 1
        for s in {k[0] for k in ep['P']} | {k[0] for k in ep['B']}:
            if s not in ep['C']:
                cnt['bias_rows_without_C'] += 1
        have = {}
        for s, (c0, c1, cdisc) in ep['C'].items():
            if s[0] not in systems:
                cnt['non_gps_C'] += 1; continue
            # the E-line datum flag names the constellations whose clock datum moved
            dchg = ep['datum_changed'] and (not ep['consts'] or s[0] in ep['consts'])
            code_clk, kind = clk_lookup(clk, s, tabs)
            if code_clk is None:
                no_clk_sat[s] += 1; cnt['C_without_CODE_clock'] += 1; continue
            cnt['clk_' + kind] += 1
            have[s] = c0
            raw['clk'][s][t] = c0
            raw['clk_full'][s][t] = c0 + code_clk * C_LIGHT
            disc['clk'][s][t] = cdisc
            ev[s][t] = (cdisc, s in ep['retro'], dchg)
        for (s, sig), (pb, si, wl, pdisc, sigma) in ep['P'].items():
            if s not in have:
                continue
            if sigma is not None and sigma > 0.1:
                cnt['P_sigma_gt_0p1'] += 1
            o = osb.get((s, sig))
            if o is None:
                missing_bia[sig] += 1; cnt['P_no_OSB'] += 1; continue
            raw['phase'][(s, sig)][t] = -have[s] + pb - o
            raw['phase_spec'][(s, sig)][t] = have[s] + pb - o
            raw['pb'][(s, sig)][t] = pb - o
            disc['phase'][(s, sig)][t] = pdisc; disc['pb'][(s, sig)][t] = pdisc
        for (s, sig), (b, bdisc) in ep['B'].items():
            if s not in have:
                continue
            o = osb.get((s, sig))
            if o is None:
                missing_bia[sig] += 1; cnt['B_no_OSB'] += 1; continue
            raw['code'][(s, sig)][t] = -have[s] + b - o
            raw['cb'][(s, sig)][t] = b - o
            disc['code'][(s, sig)][t] = bdisc; disc['cb'][(s, sig)][t] = bdisc
    return raw, disc, ev, cnt, missing_bia, no_clk_sat, tows


def sig_of(key):
    return key[1] if isinstance(key, tuple) else ''


def grp_of(key):
    """The common-mode group of a key: (constellation letter, signal). The clock
    common mode is per constellation (each has its own gauge) and a signal name
    such as C1C exists in more than one constellation; on a single-system census this
    partition is the per-signal one it always was (card GAL)."""
    sat = key[0] if isinstance(key, tuple) else key
    return (sat[:1], sig_of(key))


def common_mode(raw_kind, tows, method):
    """cm[sig][t].  'median': per-epoch median over satellites present (the pre-registered form).
    'diffmedian': cm(t) = cm(t_prev) + median over satellites present at both epochs of dX (integrated),
    so a rotating satellite set cannot move the common-mode estimate."""
    groups = defaultdict(lambda: defaultdict(dict))   # (const, sig) -> t -> {key: x}
    for key, ser in raw_kind.items():
        g = groups[grp_of(key)]
        for t, x in ser.items():
            g[t][key] = x
    cm = {}
    for sig, g in groups.items():
        c = {}
        if method == 'median':
            for t, kv in g.items():
                c[t] = float(np.median(list(kv.values())))
        else:
            acc = 0.0; prev = None
            for t in tows:
                kv = g.get(t)
                if kv is None:
                    continue
                if prev is not None:
                    d = [kv[k] - g[prev][k] for k in kv if k in g[prev]]
                    if d:
                        acc += float(np.median(d))
                c[t] = acc; prev = t
        cm[sig] = c
    return cm


def demean(raw_kind, disc_kind, cm):
    series = {}
    for key, ser in raw_kind.items():
        c = cm[grp_of(key)]
        series[key] = sorted((t, x - c[t], disc_kind.get(key, {}).get(t)) for t, x in ser.items())
    return series


def split_arcs(ser, ev_sat, gap_s=120.0, declared=False):
    """ser: sorted [(t, d, disc)].  'tracking' arcs split at gaps > gap_s only; 'declared' arcs also
    split where the record's disc counter changes, the satellite's clock disc counter changes, an R mark
    names the satellite, or a G datum change is flagged (the E-line field-7 flag)."""
    arcs, cur = [], []
    prev_t = prev_disc = prev_cdisc = None
    for i, (t, d, disc) in enumerate(ser):
        cdisc, retro, dchg = ev_sat.get(t, (None, False, False))
        cut = prev_t is not None and t - prev_t > gap_s
        if declared and prev_t is not None:
            if disc is not None and prev_disc is not None and disc != prev_disc:
                cut = True
            if cdisc is not None and prev_cdisc is not None and cdisc != prev_cdisc:
                cut = True
            if retro or dchg:
                cut = True
        if cut and cur:
            arcs.append(cur); cur = []
        cur.append(i)
        prev_t, prev_disc, prev_cdisc = t, disc, cdisc
    if cur:
        arcs.append(cur)
    return arcs


def arc_stats(series, ev, t0, declared, step_thresh=0.25, min_n=2):
    rows = []
    pool = defaultdict(list)          # hour -> |r|
    pool_signed = defaultdict(list)   # hour -> r
    steps_h = defaultdict(int)
    arcs_with_step = 0
    n_short = 0
    for key, ser in series.items():
        sat = key[0] if isinstance(key, tuple) else key
        for arc in split_arcs(ser, ev.get(sat, {}), declared=declared):
            if len(arc) < min_n:
                n_short += 1; continue
            ts = np.array([ser[i][0] for i in arc], dtype=float)
            ds = np.array([ser[i][1] for i in arc], dtype=float)
            mean = float(ds.mean()); r = ds - mean
            dd = np.abs(np.diff(ds))
            nstep = int((dd > step_thresh).sum())
            if nstep:
                arcs_with_step += 1
                for j in np.nonzero(dd > step_thresh)[0]:
                    steps_h[int((ts[j + 1] - t0) // 3600)] += 1
            rows.append({'sat': sat, 'sig': sig_of(key), 't_start': int(ts[0]), 't_end': int(ts[-1]),
                         'n': len(arc), 'mean': mean, 'rms': float(np.sqrt(np.mean(r * r))),
                         'max_abs': float(np.abs(r).max()), 'nstep': nstep,
                         'max_step': float(dd.max()) if len(dd) else 0.0})
            for tt, rr in zip(ts, r):
                h = int((tt - t0) // 3600)
                pool[h].append(abs(rr)); pool_signed[h].append(rr)
    return rows, pool, pool_signed, steps_h, arcs_with_step, n_short


def pooled_table(pool, pool_signed, steps_h, hours):
    out = []
    for h in hours:
        a = np.array(pool.get(h, []), dtype=float); r = np.array(pool_signed.get(h, []), dtype=float)
        if len(a) == 0:
            out.append((h, 0, float('nan'), float('nan'), float('nan'), steps_h.get(h, 0))); continue
        out.append((h, len(a), float(np.percentile(a, 50)), float(np.percentile(a, 95)),
                    float(np.sqrt(np.mean(r * r))), steps_h.get(h, 0)))
    return out


def level_table(series, t0, hours):
    """p50 / p95 per hour of |de-medianed value|: the between-satellite level, not within-arc."""
    pool = defaultdict(list)
    for key, ser in series.items():
        for (t, d, disc) in ser:
            pool[int((t - t0) // 3600)].append(abs(d))
    out = []
    for h in hours:
        a = np.array(pool.get(h, []), dtype=float)
        out.append((h, len(a), float(np.percentile(a, 50)) if len(a) else float('nan'),
                    float(np.percentile(a, 95)) if len(a) else float('nan')))
    return out


def fmt(x, w=7, p=3):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return ' ' * (w - 1) + '-'
    return f'{x:{w}.{p}f}'


# ----------------------------------------------------------------------------
# driver
# ----------------------------------------------------------------------------
def resolve_code_products(seed_json, data_root_override=None):
    """seed.json (files.internal_data_root, data_download.{year,doy,product_source,product_type}) ->
    the CODE 30 s CLK files of DOY-1..DOY+1 (what the producer loads) and the DOY OSB .BIA."""
    cfg = json.load(open(seed_json))
    root = data_root_override or cfg.get('files', {}).get('internal_data_root') or os.path.join(DATA_ROOT_FALLBACK, 'data')
    dl = cfg.get('data_download', {})
    year, doy = int(dl.get('year', 2024)), int(dl.get('doy', 129))
    src, typ = dl.get('product_source', 'COD'), dl.get('product_type', 'FIN')
    clk_paths = []
    for d in (doy - 1, doy, doy + 1):
        clk_paths += sorted(glob.glob(os.path.join(root, f'{year}-{d:03d}', 'products', f'{src}0MGX{typ}_*_30S_CLK.CLK')))
    bia = sorted(glob.glob(os.path.join(root, f'{year}-{doy:03d}', 'products', f'{src}0MGX{typ}_*_OSB.BIA')))
    return clk_paths, (bia[0] if bia else None), f'{year}:{doy:03d}', cfg


def run_census(run_dir, clk, osb, label, t_lo=None, t_hi=None, hours=None, out_dir=None,
               step_thresh=0.25, min_n=2, systems=('G',)):
    epochs, meta = parse_products(os.path.join(run_dir, 'products.txt'))
    if not epochs:
        print(f'!! {label}: no epochs parsed'); return None
    t0 = epochs[0]['tow'] if t_lo is None else t_lo
    raw, disc, ev, cnt, missing_bia, no_clk_sat, tows = collect_raw(epochs, clk, osb, t_lo, t_hi,
                                                                     systems)
    if hours is None:
        hours = list(range(int((tows[-1] - t0) // 3600) + 1))
    res = {'label': label, 'S': meta['S'], 'cnt': dict(cnt), 'missing_bia': dict(missing_bia),
           'no_clk_sat': dict(no_clk_sat), 'n_epochs': len(tows), 't0': t0, 'hours': hours}
    print(f'\n===== {label}: {run_dir} =====')
    print(f'S line: {meta["S"][:230] if meta["S"] else "(none)"}')
    print(f'epochs in window {len(tows)}  (tow {tows[0]}..{tows[-1]}; off-grid E tows {meta["off_grid_ms"]}; unknown tags {dict(meta["unknown_tags"])})')
    print(f'C rows with CODE clock: exact {cnt.get("clk_exact",0)}, interpolated {cnt.get("clk_interp",0)}, '
          f'C without CODE clock {cnt.get("C_without_CODE_clock",0)}, non-GPS C {cnt.get("non_gps_C",0)}, '
          f'bias rows without a C record {cnt.get("bias_rows_without_C",0)}, P rows without OSB {cnt.get("P_no_OSB",0)}, '
          f'B rows without OSB {cnt.get("B_no_OSB",0)}, P rows sigma>0.1 m {cnt.get("P_sigma_gt_0p1",0)}')
    if missing_bia:
        print('  signals absent in the BIA (rows skipped): ' + ', '.join(f'{k}:{v}' for k, v in sorted(missing_bia.items())))
    if no_clk_sat:
        print('  satellites without a CODE clock at some epochs: ' + ', '.join(f'{k}:{v}' for k, v in sorted(no_clk_sat.items())))
    for method in ('median', 'diffmedian'):
        for kind in KINDS:
            series = demean(raw[kind], disc[kind], common_mode(raw[kind], tows, method))
            for declared in (False, True):
                tag = 'declared' if declared else 'tracking'
                rows, pool, pools, steps_h, aws, nshort = arc_stats(series, ev, t0, declared, step_thresh, min_n)
                res[(method, kind, tag)] = {'rows': rows, 'table': pooled_table(pool, pools, steps_h, hours),
                                            'arcs_with_step': aws, 'n_arcs': len(rows), 'n_short': nshort}
            if method == 'median':
                res[('level', kind)] = level_table(series, t0, hours)
    series = demean(raw['phase_spec'], disc['phase'], common_mode(raw['phase_spec'], tows, 'median'))
    rows, pool, pools, steps_h, aws, nshort = arc_stats(series, ev, t0, False, step_thresh, min_n)
    res['phase_spec'] = pooled_table(pool, pools, steps_h, hours)
    series = demean(raw['clk_full'], disc['clk'], common_mode(raw['clk_full'], tows, 'median'))
    res['clk_full'] = level_table(series, t0, hours)
    print_tables(res)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        for kind in KINDS:
            for tag in ('tracking', 'declared'):
                with open(os.path.join(out_dir, f'{label}_{kind}_{tag}_arcs.csv'), 'w') as fh:
                    fh.write('sat,sig,t_start,t_end,n,mean_m,rms_m,max_abs_m,nstep,max_step_m\n')
                    for r in res[('median', kind, tag)]['rows']:
                        fh.write(f"{r['sat']},{r['sig']},{r['t_start']},{r['t_end']},{r['n']},{r['mean']:.4f},"
                                 f"{r['rms']:.4f},{r['max_abs']:.4f},{r['nstep']},{r['max_step']:.4f}\n")
        summ = {str(k): (v if not (isinstance(v, dict) and 'rows' in v) else {kk: vv for kk, vv in v.items() if kk != 'rows'})
                for k, v in res.items()}
        with open(os.path.join(out_dir, f'{label}_summary.json'), 'w') as fh:
            json.dump(summ, fh, indent=1, default=str)
    return res


def print_tables(res):
    hours = res['hours']
    print(f'\n-- {res["label"]}: within-arc |D - arc mean| pooled per hour, cm=per-epoch MEDIAN (pre-registered); '
          f'arcs n>=2; hour = floor((tow-t0)/3600), t0={res["t0"]} --')
    print('kind   seg       hour      N     p50     p95     rms  steps>0.25 | diffmedian cm:    rms  steps')
    for kind in KINDS:
        for tag in ('tracking', 'declared'):
            d = res[('median', kind, tag)]; d2 = res[('diffmedian', kind, tag)]
            for (h, n, p50, p95, rms, st), (_, _, _, _, rms2, st2) in zip(d['table'], d2['table']):
                print(f'{kind:6s} {tag:8s}  {h:4d} {n:6d} {fmt(p50)} {fmt(p95)} {fmt(rms)}  {st:5d}      |'
                      f'                 {fmt(rms2)}  {st2:5d}')
            print(f'{kind:6s} {tag:8s}  arcs {d["n_arcs"]:5d}  arcs_with_step>0.25 {d["arcs_with_step"]:5d}  '
                  f'arcs dropped (n<2) {d["n_short"]:5d}      | arcs {d2["n_arcs"]:5d} with_step {d2["arcs_with_step"]:5d}')
    print(f'\n-- {res["label"]}: LEVEL (between-satellite) p50 / p95 of |de-medianed| per hour --')
    print('kind   hour      N     p50     p95')
    for kind in KINDS:
        for (h, n, p50, p95) in res[('level', kind)]:
            print(f'{kind:6s} {h:4d} {n:6d} {fmt(p50)} {fmt(p95)}')
    print(f'\n-- {res["label"]}: CONTROLS --')
    print('SPEC-literal same-sign phase (+C_net + PB - OSB), tracking arcs, within-arc rms per hour: '
          + ', '.join(f'h{h}={fmt(rms,6,3).strip()}' for (h, n, p50, p95, rms, st) in res['phase_spec']))
    print('full-clock control |C_net + c*dt_CODE - median| p50 per hour (m): '
          + ', '.join(f'h{h}={p50:.0f}' for (h, n, p50, p95) in res['clk_full']))


def worst_sats(res, kind='phase', tag='tracking', n=5, method='median'):
    rows = res[(method, kind, tag)]['rows']
    by_sat = defaultdict(list)
    for r in rows:
        by_sat[r['sat']].append(r)
    score = {s: max(r['rms'] for r in v) for s, v in by_sat.items()}
    worst = sorted(score, key=lambda s: -score[s])[:n]
    print(f'\n-- {res["label"]}: per-satellite arc means (the datum offset) for the worst {n} satellites by max within-arc rms ({kind}, {tag} arcs) --')
    print('sat  sig   t_start  t_end     n   mean_m    rms_m  max|r|  nstep')
    for s in worst:
        for r in sorted(by_sat[s], key=lambda r: (r['sig'], r['t_start'])):
            print(f'{s}  {r["sig"]}  {r["t_start"]:7d} {r["t_end"]:7d} {r["n"]:5d} {r["mean"]:8.3f} {r["rms"]:8.3f} {r["max_abs"]:7.3f}  {r["nstep"]:3d}')
    return worst


def code_constant_offsets(res, thresh=1.0, tag='tracking', method='median'):
    """Run-mean of D_code per (sat, signal) (epoch-weighted over arcs); flags |mean| > thresh.  Also the
    pinned-pair IF combination (C1C, C2W) per satellite - the direction the consumer's clock+code sees."""
    rows = res[(method, 'code', tag)]['rows']
    acc = defaultdict(lambda: [0.0, 0])
    for r in rows:
        a = acc[(r['sat'], r['sig'])]; a[0] += r['mean'] * r['n']; a[1] += r['n']
    mean = {k: v[0] / v[1] for k, v in acc.items() if v[1]}
    flagged = {k: m for k, m in mean.items() if abs(m) > thresh}
    print(f'\n-- {res["label"]}: code-side per-(sat,signal) run-mean of D_code with |mean| > {thresh} m ({tag} arcs): '
          f'{len(flagged)} of {len(mean)} (sat,signal) keys --')
    print('  ' + '  '.join(f'{s} {sig} {m:+.2f}' for (s, sig), m in sorted(flagged.items(), key=lambda kv: -abs(kv[1]))))
    print(f'-- {res["label"]}: pinned-pair reading per satellite: D(C1C), D(C2W), IF(C1C,C2W) = {IF1:.4f}*D1 {IF2:+.4f}*D2 (m) --')
    sats = sorted({s for (s, sig) in mean if sig in ('C1C', 'C2W')})
    line = []
    for s in sats:
        d1, d2 = mean.get((s, 'C1C')), mean.get((s, 'C2W'))
        if d1 is None or d2 is None:
            continue
        line.append(f'{s}:{d1:+.2f}/{d2:+.2f}/IF{IF1*d1+IF2*d2:+.2f}')
    print('  ' + '  '.join(line))
    return flagged


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--project', default=DATA_ROOT_FALLBACK, help='project root holding runs/ and data/')
    ap.add_argument('--seed', default='runs/evseed_field6h_v8', help='run whose seed.json names the CODE products')
    ap.add_argument('--run', action='append', default=None, help='run dir(s) for the full-window census (repeatable)')
    ap.add_argument('--window-run', action='append', default=None, help='run dir(s) for the 60-epoch window (repeatable)')
    ap.add_argument('--window-tow', type=int, default=277320)
    ap.add_argument('--window-n', type=int, default=60)
    ap.add_argument('--step', type=float, default=0.25, help='within-arc step threshold (m)')
    ap.add_argument('--min-n', type=int, default=2, help='minimum arc length (epochs) entering the pooled stats')
    ap.add_argument('--out', default=None, help='directory for per-arc CSVs and summary JSON (optional)')
    ap.add_argument('--data-root', default=None)
    ap.add_argument('--systems', default='G',
                    help='card GAL: comma list of constellation letters; the census runs once per '
                         'letter, restricted to it (default G, the pre-card output)')
    a = ap.parse_args()
    systems = [s.strip() for s in a.systems.split(',') if s.strip()]
    if systems == ['G']:
        return census_main(a, ('G',), '')
    for sy in systems:
        print(f'\n########## card GAL: census for constellation {sy} ##########')
        census_main(a, (sy,), '_' + sy)


def census_main(a, systems, suffix):
    """The pre-card main body, for the constellation letters `systems`; `suffix` is
    appended to every run label (empty for the default GPS-only census)."""
    P = a.project
    runs = a.run or ['runs/evseed_field6h_v8', 'runs/evseed_field6h_v8fp2']
    wruns = a.window_run or ['runs/evseed_field6h_v8', 'runs/evseed_field6h_v8fp2', 'runs/evleg_cert_null_v8']
    seed_json = os.path.join(P, a.seed, 'seed.json')
    clk_paths, bia_path, doy_key, cfg = resolve_code_products(seed_json, a.data_root)
    print('CODE products resolved from', seed_json)
    for p in clk_paths:
        print('  CLK:', p)
    print('  BIA:', bia_path)
    print('  producer enable_code_phase_bias (seed.json model_corrections):', cfg.get('model_corrections', {}).get('enable_code_phase_bias'))
    if not clk_paths or not bia_path:
        sys.exit('CODE products not found')
    clk, nclk = parse_clk(clk_paths, systems)
    osb, sigs = parse_bia(bia_path, doy_key, systems)
    sysname = 'GPS' if tuple(systems) == ('G',) else '+'.join(systems)
    print(f'  parsed {nclk} {sysname} AS records for {len(clk)} satellites; {len(osb)} {sysname} OSB entries valid on {doy_key}, signals {sorted(sigs)}')

    results = {}
    for r in runs:
        lab = os.path.basename(r.rstrip('/\\')) + suffix
        results[lab] = run_census(os.path.join(P, r), clk, osb, lab, out_dir=a.out, step_thresh=a.step, min_n=a.min_n,
                                  systems=systems)
    t_lo, t_hi = a.window_tow, a.window_tow + 30 * (a.window_n - 1)
    for r in wruns:
        lab = 'win_' + os.path.basename(r.rstrip('/\\')) + suffix
        run_census(os.path.join(P, r), clk, osb, lab, t_lo=t_lo, t_hi=t_hi, hours=[0], out_dir=a.out,
                   step_thresh=a.step, min_n=a.min_n, systems=systems)

    for lab, res in results.items():
        if res is None:
            continue
        worst_sats(res, 'phase', 'tracking', 5)
        code_constant_offsets(res, 1.0, 'tracking')

    def rms_by_hour(res, kind, tag, method='median'):
        return {h: rms for (h, n, p50, p95, rms, st) in res[(method, kind, tag)]['table']}

    labs = [l for l in results if results[l]]
    if labs:
        v8 = results[labs[0]]
        ph = rms_by_hour(v8, 'phase', 'tracking'); cd = rms_by_hour(v8, 'code', 'tracking')
        phd = rms_by_hour(v8, 'phase', 'declared'); cdd = rms_by_hour(v8, 'code', 'declared')
        after1 = [h for h in v8['hours'] if h >= 1]
        print('\n===== VERDICTS (pre-registered bars, SPEC_event_harness section 24.5) =====')
        print(f'W1 ({labs[0]}): phase within-arc rms per hour, tracking arcs = ' + ', '.join(f'h{h}={ph[h]:.3f}' for h in v8['hours']))
        print(f'                                               declared arcs = ' + ', '.join(f'h{h}={phd[h]:.3f}' for h in v8['hours']))
        print(f'    hours>=1 all <= 0.05 m: {all(ph[h] <= 0.05 for h in after1)} / declared {all(phd[h] <= 0.05 for h in after1)};  '
              f'all >= 0.5 m: {all(ph[h] >= 0.5 for h in after1)} / declared {all(phd[h] >= 0.5 for h in after1)}')
        print(f'W3 ({labs[0]}): code within-arc rms per hour, tracking arcs = ' + ', '.join(f'h{h}={cd[h]:.3f}' for h in v8['hours'])
              + f';  hours>=1 all <= 0.3 m: {all(cd[h] <= 0.3 for h in after1)}')
        print(f'                                              declared arcs = ' + ', '.join(f'h{h}={cdd[h]:.3f}' for h in v8['hours'])
              + f';  hours>=1 all <= 0.3 m: {all(cdd[h] <= 0.3 for h in after1)}')
    if len(labs) >= 2:
        v8, fp = results[labs[0]], results[labs[1]]
        for kind in ('phase', 'code'):
            for tag in ('tracking', 'declared'):
                for method in ('median', 'diffmedian'):
                    r0 = rms_by_hour(v8, kind, tag, method); r1 = rms_by_hour(fp, kind, tag, method)
                    hs = [h for h in v8['hours'] if h >= 2]
                    ratios = {h: (r1[h] / r0[h] if r0[h] > 0 else float('nan')) for h in hs}
                    worse = [h for h in hs if ratios[h] > 1.10]
                    print(f'W2 {kind:5s} {tag:8s} cm={method:10s}: rms {labs[1]}/{labs[0]} = '
                          + ', '.join(f'h{h}={ratios[h]:.3f}' for h in hs)
                          + f'  -> worse by >10% in hours {worse if worse else "none"}')
        for lab in labs:
            for tag in ('tracking', 'declared'):
                p = results[lab][('median', 'phase', tag)]; c = results[lab][('median', 'code', tag)]
                print(f'W2 steps>0.25 m ({lab}, {tag}, cm=median): phase arcs {p["arcs_with_step"]}/{p["n_arcs"]}, '
                      f'code arcs {c["arcs_with_step"]}/{c["n_arcs"]}')


if __name__ == '__main__':
    main()
