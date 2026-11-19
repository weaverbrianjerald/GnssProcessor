"""Block 'epoch': per-epoch series of the whole network and the run-level tables.

    es     [field, epoch]  network_epoch_status.csv: sigma0 values, coverage, clock spread, prefit screen, counts
    es_disp [epoch] u8     what happened to the epoch (emitted, withheld by the gate or sigma0, failed, skipped)
    nt     [field, epoch]  network_trace.csv: phase biases, datum dimension, gate, clock RMS, pins
    sh     [field, epoch]  network_service_health.csv (every few minutes): time per epoch, emit age, sigma0 average
    cl     [field, epoch]  closed_loop_trace.csv: the held-out user receiver's position error and fixing
    cli    [system, 2, epoch] closed_loop_isb.csv: its inter-system bias and sigma
    dg     [constellation, field, epoch] network_datum_graph.csv: datum dimension and the graph's size
Header: run summary, flag rates, measured carried-prior floors, and the run manifest's parameter tables.
"""
from __future__ import annotations

import json

import numpy as np

import nv_tables as T
from nv_tables import F64, I64, STR

LAYER = 'products'

ES_FIELDS = [
    ('sigma0', 'sigma0 of the within-epoch residual test'), ('sigma0_pf', 'post-fit sigma0, all rows'),
    ('sigma0_pf_code', 'post-fit sigma0, code'), ('sigma0_pf_phase', 'post-fit sigma0, phase'),
    ('n_present', 'receivers in the solution'), ('n_clock_sats', 'satellites with a clock'),
    ('clock_median_m', 'median satellite clock correction (m)'), ('clock_std_m', 'spread of the satellite clock corrections (m)'),
    ('worst_dev_m', 'largest satellite clock deviation from the median (m)'), ('n_adapt', 're-solves within the epoch'),
    ('n_refloat_sats', 'satellites whose clock was re-floated'), ('datum_changed', 'datum changed (0/1)'),
    ('n_arc_refloat', 'arcs re-floated'), ('n_chord_births', 'new links that are not tree links'),
    ('n_slip_refloats', 're-floats after slips'), ('n_pf_rows', 'post-fit rows'), ('n_redun_rows', 'rows with redundancy'),
    ('n_uncheckable_rows', 'rows that no test can check'), ('pc_code_max_s', 'prefit screen, code: largest (sigma)'),
    ('pc_code_p99_s', 'prefit screen, code: 99th percentile (sigma)'), ('pc_code_n10', 'prefit screen, code: rows above 10 sigma'),
    ('pc_phase_max_s', 'prefit screen, phase: largest (sigma)'), ('pc_phase_p99_s', 'prefit screen, phase: 99th percentile (sigma)'),
    ('pc_phase_n10', 'prefit screen, phase: rows above 10 sigma'), ('sigma0_trigger', 'sigma0 that starts a re-solve'),
    ('withhold_ceiling', 'sigma0 above which the epoch is withheld'),
]
DISPOSITIONS = ['emitted', 'gate_withheld', 'sigma0_withheld', 'solve_failed', 'datum_failed', 'sync_skip']
NT_FIELDS = [('n_phase_bias', 'phase biases emitted'), ('declared_dim', 'declared datum dimension'),
             ('gate_ok', 'datum gate passed (0/1)'), ('solved', 'solved (0/1)'), ('rms_clock_m', 'RMS of the clock corrections (m)'),
             ('max_clock_m', 'largest clock correction (m)'), ('n_pivot_pins', 'pivot pins')]
SH_FIELDS = [('ms_per_epoch_window', 'time per epoch over the window (ms)'), ('last_emit_age_s', 'age of the last emit (s)'),
             ('sigma0_ewma', 'running sigma0'), ('n_runaway_epochs', 'runaway epochs (cumulative)'),
             ('n_gate_withheld', 'epochs withheld by the gate (cumulative)'), ('n_sigma0_withheld', 'epochs withheld by sigma0 (cumulative)')]
CL_FIELDS = [('pos_err_3d_m', '3D position error (m)'), ('east_m', 'east error (m)'), ('north_m', 'north error (m)'),
             ('up_m', 'up error (m)'), ('sig_e_m', 'east sigma (m)'), ('sig_n_m', 'north sigma (m)'), ('sig_u_m', 'up sigma (m)'),
             ('sig_3d_m', '3D sigma (m)'), ('n_sats', 'satellites used'), ('ar_ran', 'fixing ran (0/1)'),
             ('ar_success', 'fixing accepted (0/1)'), ('num_fixed', 'ambiguities fixed'), ('num_candidates', 'fixing candidates'),
             ('ratio', 'ratio test'), ('p_succ', 'success rate')]
DG_FIELDS = [('k', 'predicted datum dimension'), ('d', 'datum dimension'), ('n_sta', 'receivers'), ('n_sat', 'satellites'),
             ('n_edges', 'receiver-satellite edges'), ('n_cells', 'cells'), ('declared', 'declared dimension'),
             ('observed', 'observed dimension'), ('n_comps', 'connected parts')]


def _fields(ctx, t, fields, rows=None):
    ne = ctx.ix.n_ep
    e = T.epochs(t, ctx.ix)
    out = np.full((len(fields), ne), np.nan, np.float32)
    for i, (c, _l) in enumerate(fields):
        if c in t.column_names:
            T.scatter(out[i], (e,), T.num(t, c).astype(np.float32))
    return out


def build(ctx):
    ix, ne = ctx.ix, ctx.ix.n_ep
    H, A = {}, {}
    es = ctx.table('network_epoch_status.csv', dict([('week', I64), ('tow', F64), ('disposition', STR), ('worst_sat', STR)] +
                   [(c, F64) for c, _l in ES_FIELDS]), LAYER, 'solution state per epoch', 'network.trace_epoch_status')
    if T.has(es, 'tow'):
        A['es'] = _fields(ctx, es, ES_FIELDS)
        e = T.epochs(es, ix)
        if 'disposition' in es.column_names:
            disp = np.full(ne, 255, np.uint8)
            codes, values = T.strings(es, 'disposition')
            lut = np.array([DISPOSITIONS.index(v) if v in DISPOSITIONS else len(DISPOSITIONS) for v in values] + [255], np.uint8)
            T.scatter(disp, (e,), lut[codes])
            A['es_disp'] = disp
        if 'worst_sat' in es.column_names:
            ws = np.full(ne, -1, np.int16)
            T.scatter(ws, (e,), T.names(es, 'worst_sat', ix.sat).astype(np.int16))
            A['es_worst_sat'] = ws
        H['es_fields'] = [[c, l] for c, l in ES_FIELDS]
        H['dispositions'] = DISPOSITIONS + ['other']
    nt = ctx.table('network_trace.csv', dict([('week', I64), ('tow', F64)] + [(c, F64) for c, _l in NT_FIELDS]),
                   LAYER, 'solve health per epoch', 'always written by a network run')
    if T.has(nt, 'tow'):
        A['nt'] = _fields(ctx, nt, NT_FIELDS)
        H['nt_fields'] = [[c, l] for c, l in NT_FIELDS]
    sh = ctx.table('network_service_health.csv', dict([('week', I64), ('tow', F64), ('pivot', STR)] + [(c, F64) for c, _l in SH_FIELDS]),
                   LAYER, 'service health', 'network.health_interval_s (above 0)')
    if T.has(sh, 'tow'):
        A['sh'] = _fields(ctx, sh, SH_FIELDS)
        H['sh_fields'] = [[c, l] for c, l in SH_FIELDS]
    cl = ctx.table('closed_loop_trace.csv', dict([('week', I64), ('tow', F64), ('ar_gate', STR)] + [(c, F64) for c, _l in CL_FIELDS]),
                   LAYER, 'held-out user receiver', 'network.closed_loop.station (a receiver outside the network, scored against its known position)')
    if T.has(cl, 'tow'):
        A['cl'] = _fields(ctx, cl, CL_FIELDS)
        H['cl_fields'] = [[c, l] for c, l in CL_FIELDS]
        H['cl_station'] = _closed_loop_station(ctx)
    cli = ctx.table('closed_loop_isb.csv', {'week': I64, 'tow': F64, 'system': STR, 'isb_m': F64, 'isb_std_m': F64},
                    LAYER, 'held-out user inter-system bias', 'network.closed_loop.station')
    if T.has(cli, 'system'):
        codes, systems = T.strings(cli, 'system')
        e = T.epochs(cli, ix)
        arr = np.full((len(systems), 2, ne), np.nan, np.float32)
        ok = (e >= 0) & (codes >= 0)
        arr[codes[ok], 0, e[ok]] = T.num(cli, 'isb_m')[ok]
        arr[codes[ok], 1, e[ok]] = T.num(cli, 'isb_std_m')[ok]
        A['cli'] = arr
        H['cli_systems'] = systems
    dg = ctx.table('network_datum_graph.csv', dict([('week', I64), ('tow', F64), ('cons', STR), ('comps', STR)] +
                   [(c, F64) for c, _l in DG_FIELDS if c != 'n_comps']), LAYER, 'datum graph', 'network.datum_graph_census')
    if T.has(dg, 'cons'):
        codes, cons = T.strings(dg, 'cons')
        e = T.epochs(dg, ix)
        arr = np.full((len(cons), len(DG_FIELDS), ne), np.nan, np.float32)
        ok = (e >= 0) & (codes >= 0)
        for i, (c, _l) in enumerate(DG_FIELDS):
            if c == 'n_comps':
                comp = dg.column('comps').to_pylist()
                v = np.array([float(str(x).count('@')) if x else np.nan for x in comp], np.float32)
            elif c in dg.column_names:
                v = T.num(dg, c).astype(np.float32)
            else:
                continue
            arr[codes[ok], i, e[ok]] = v[ok]
        A['dg'] = arr
        H['dg_cons'] = cons
        H['dg_fields'] = [[c, l] for c, l in DG_FIELDS]
    H.update(_run_tables(ctx))
    if not A and len(H) <= 1:
        return None
    return H, A


def _closed_loop_station(ctx):
    try:
        j = json.loads((ctx.run / 'arm.json').read_text(encoding='utf-8'))
        return ((j.get('network') or {}).get('closed_loop') or {}).get('station', {}).get('name', '')
    except (OSError, ValueError, AttributeError):
        return ''


def _rows(t):
    if t is None or t.num_rows == 0:
        return []
    cols = t.column_names
    data = [t.column(c).to_pylist() for c in cols]
    return [dict(zip(cols, r)) for r in zip(*data)]


def _run_tables(ctx):
    """Small run-level tables: run summary, flag rates, carried-prior floors, the manifest's parameter tables."""
    H = {}
    rs = T.read(ctx.logs, 'network_run_summary.csv')
    if rs is not None and rs.num_rows:
        H['run_summary'] = _rows(rs)[0]
    fr = T.read(ctx.logs, 'network_flag_rates.csv')
    if fr is not None:
        H['flag_rates'] = _rows(fr)
    else:
        ctx.note_missing('slips', 'false-alarm rates of the slip tests', 'logs/network_flag_rates.csv', 'network.trace_flag_rates')
    cv = T.read(ctx.logs, 'network_carry_vce.csv')
    if cv is not None:
        H['carry_vce'] = _rows(cv)
    else:
        ctx.note_missing('stochastic', 'measured floors of the carried priors', 'logs/network_carry_vce.csv', 'the carried-prior variance estimation (network.carry_vce)')
    man = ctx.logs / 'network_run_manifest.json'
    if man.exists():
        recs = {}
        for line in man.read_text(encoding='utf-8', errors='replace').splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                j = json.loads(line)
            except ValueError:
                continue
            k = j.get('record')
            if k in ('param_model', 'param_dispositions', 'param_responses', 'complete', 'startup'):
                recs.setdefault(k, j)
        if 'startup' in recs:
            s = recs['startup']
            recs['startup'] = {k: s.get(k) for k in ('written_utc', 'config_path', 'config_hash_fnv1a64', 'build')}
        H['manifest'] = recs
    else:
        ctx.note_missing('stochastic', 'parameter tables of the run', 'logs/network_run_manifest.json', 'network.write_run_manifest')
    return H
