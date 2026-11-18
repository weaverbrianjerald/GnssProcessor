"""Unit tests of the network view tool on a small synthetic run folder.

    py -3.14 scripts/netview/test_netview.py        (or: py -3.14 -m unittest scripts/netview/test_netview.py)

The fixture is written to a temporary folder: three receivers, five Galileo satellites, three epochs, the tree of
L1X and L5X, two links that are not tree links (one refused, then held), events, receiver states and products.
"""
from __future__ import annotations

import gzip
import json
import math
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import netview  # noqa: E402
import nv_arm  # noqa: E402
import nv_build as B  # noqa: E402
import nv_page  # noqa: E402
import nv_read as R  # noqa: E402
import nv_tables as T  # noqa: E402

WEEK = 2420
TOWS = (0.0, 30.0, 60.0)
SATS = ('E01', 'E02', 'E03', 'E04', 'E05')


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def _tree_rows():
    """Root R1; E01 under R1, R2 under E01, E02 under R2, R3 under E02; at tow 60 E02 moves under R1."""
    hdr = ('week,tow,system,family,band,comp,r1,edge_sta,edge_sat,child,parent,depth,target_m,target_cyc,'
           'prev_target_m,pinmove_m,known,legacy_cause,row,row_armed,certified,cert_source,flipped,returning,life_stamp\n')
    out = [hdr]
    for tow in TOWS:
        moved = tow >= 60.0
        edges = [('R1', 'E01', 'E01', 'R1', 1, 0), ('R2', 'E01', 'R2', 'E01', 2, 5)]
        if moved:
            edges += [('R1', 'E02', 'E02', 'R1', 1, -3), ('R3', 'E02', 'R3', 'E02', 2, 7)]
        else:
            edges += [('R2', 'E02', 'E02', 'R2', 3, 4), ('R3', 'E02', 'R3', 'E02', 4, 7)]
        for band in ('L1X', 'L5X'):
            for (es, et, child, parent, depth, tgt) in edges:
                row = 'RehostUncertified' if moved and child == 'E02' and tow == 60.0 else 'Kept'
                out.append(f'{WEEK},{tow:.3f},E,signal,{band},0,R1,{es},{et},{child},{parent},{depth},0,{tgt},,0,1,'
                           f'same_edge,{row},1,0,none,0,0,x\n')
    return ''.join(out)


#: A real chord row of _rap_o2s1j_n144 (MBAR00UGA-E07 at TOW 180) and what network_ring_fix.csv says of it.
REAL = dict(f1=-128.168544, f2=-37.464292, q11=22.64359, q12=24.03773, q22=26.01514,
            n_if=-127.2965, sigma_if=4.4151, wl=-91, frac_wl=0.2957)


def make_run(root: Path, fixing=True, raim_header_only=True, rpb=False) -> Path:
    run = root / 'run'
    logs = run / 'logs'
    _write(logs / 'network_stations.csv', 'station,role,ecef_x,ecef_y,ecef_z\n'
           'R1,datum,6378137.0,0.0,0.0\nR2,network,0.0,6378137.0,0.0\nR3,network,0.0,0.0,6356752.3\n')
    _write(logs / 'network_chain_edges.csv', _tree_rows())
    frame = ['week,tow,family,arc_sta,arc_sat,float_cyc_1,float_cyc_2,q11,q12,q22,int_1,int_2,source,conflict,lattice_int\n']
    for tow in TOWS[1:]:
        frame.append(f'{WEEK},{tow:.3f},E|L1X+L5X,R3,E01,{REAL["f1"]},{REAL["f2"]},{REAL["q11"]},{REAL["q12"]},'
                     f'{REAL["q22"]},,,none,0,/\n')
        frame.append(f'{WEEK},{tow:.3f},E|L1X+L5X,R1,E03,10.02,7.01,0.0004,0.0003,0.0004,,,none,0,/\n')
    _write(logs / 'network_chain_frame.csv', ''.join(frame))
    if fixing:
        _write(logs / 'network_ring_fix.csv',
               'week,tow,station,sat,sig1,sig2,ring,candidate,state,refused,f1,f2,n_if,sigma_if,frac_if,wl,frac_wl,'
               'n1,n2,lat1,lat2,resid_if_m,streak,audit\n'
               f'{WEEK},30.000,R3,E01,L1X,L5X,4,1,refused,window,{REAL["f1"]},{REAL["f2"]},-127.2965,4.4151,0.2965,'
               f'-91,0.2957,-127,-36,,,,0,\n'
               f'{WEEK},60.000,R3,E01,L1X,L5X,4,1,held,,{REAL["f1"]},{REAL["f2"]},-127.2965,4.4151,0.2965,-91,0.2957,'
               f'-128,-37,,,0.004,0,unlisted\n')
    _write(logs / 'network_events.csv',
           'week,tow,tier,event,sat,station,statistic,threshold,action,detail\n'
           f'{WEEK},30.000,RINGFIX,hold,E01,R3,0.13,0.10,decide,sig1=L1X sig2=L5X ring=4 n1=-128 n2=-37\n'
           f'{WEEK},60.000,RINGFIX,release,E03,R1,,,slip,sig1=L1X sig2=L5X n1=10 n2=7\n'
           f'{WEEK},60.000,T1_gf_jump,gf_jump,E03,R1,0.17,0.04,refloat,L1X;L5X;r=+0.17\n'
           f'{WEEK},60.000,CHAIN,rehost,E02,R1,0.0,,join,sig=L1X comp=0 row=RehostUncertified\n'
           f'{WEEK},60.000,PRIORRES,prior_residual,E02,,5.4,4.0,report,family=pb\n'
           f'{WEEK},60.000,GROSSLOS,gross_los_declare,E04,R2,9.6,8.0,declare,wire_latency_ep=1\n')
    if raim_header_only:
        _write(logs / 'network_raim.csv', 'week,tow,sat,station,s_max_raw,s_max_norm,sigma0,raim_k,spiked,'
               'n_sta_spiked,min_sta,refloated\n')
    _write(logs / 'network_zwd.csv', 'week,tow,station,zwd_m,std_m,carried\n' + ''.join(
        f'{WEEK},{t:.3f},R{i},0.1{i},0.01,1\n' for t in TOWS for i in (1, 2, 3)))
    _write(logs / 'network_rx_states.csv', 'week,tow,station,kind,key,value_m,std_m\n' + ''.join(
        f'{WEEK},{t:.3f},R{i},CLK,,{i * 10.0 + t / 30.0},0.5\n' for t in TOWS for i in (1, 2, 3)))
    if rpb:
        _write(logs / 'network_rpb_states.csv', 'week,tow,station,system,signal,value_m,sigma_m,pinned,floor_w,'
               'timelink_var_m2,parent_sat,depth,comp,event,shift_m\n' + ''.join(
                   f'{WEEK},{t:.3f},R2,Galileo,L1X,0.5,0.01,0,,,E01,2,0,Kept,0\n' for t in TOWS))
    _write(logs / 'network_ionosphere.csv', 'week,tow,station,sat,iono_m,elevation_deg,azimuth_deg,x_lin_m,'
           'iono_total_m\n' + ''.join(f'{WEEK},{t:.3f},R1,E01,0.1,90.0,0.0,0.0,0.1\n' for t in TOWS))
    _write(run / 'products.txt', _products())
    return run


def add_dashboard_traces(run: Path) -> Path:
    """The traces the dashboard's other blocks read, small: post-fit rows, epoch status, variance factors, slip
    test and raw observables, carried floats, products' discontinuity counters, the held-out user check."""
    logs = run / 'logs'
    pf = ['week,tow,station,sat,signal,kind,resid_m,w,elev_deg\n']
    for t in TOWS:
        pf += [f'{WEEK},{t:.3f},R1,E01,C1X,code,0.30,4.0,90.0\n', f'{WEEK},{t:.3f},R1,E01,C5X,code,-0.40,4.0,90.0\n',
               f'{WEEK},{t:.3f},R1,E01,L1X,phase,0.002,250000.0,90.0\n', f'{WEEK},{t:.3f},R1,E01,L5X,phase,-0.004,250000.0,90.0\n',
               f'{WEEK},{t:.3f},R2,E02,C1X,code,0.50,1.0,45.0\n']
    _write(logs / 'network_pf_rows.csv', ''.join(pf))
    _write(logs / 'network_epoch_status.csv', 'week,tow,disposition,sigma0,n_present,n_clock_sats,sigma0_pf_code\n' +
           f'{WEEK},0.000,emitted,1.0,3,5,0.8\n{WEEK},30.000,gate_withheld,1.0,3,5,0.9\n{WEEK},60.000,emitted,1.0,2,5,1.1\n')
    _write(logs / 'network_vcf.csv', 'week,tow,class,sys,signal,kind,n,sum_wv2,sum_r,rho,f,g,ewma_f,ewma_g,identifiable,n_dw,n_unchk\n' +
           ''.join(f'{WEEK},{t:.3f},E C1X,E,C1X,code,60,30.0,50.0,0.8,0.6,0.5,0.6,0.5,1,0,0\n' for t in (0.0, 60.0)))
    _write(logs / 'network_if_slip.csv', 'week,tow,station,sat,sig1,sig2,elev_deg,dd_m,med_m,s_pool_m,ewma_scale_m,count,'
           'w_shrink,s_eff_m,elev_map,thr_m,floor_m,abs_dev_m,flagged,carry_warm,is_ref\n' + ''.join(
               f'{WEEK},{t:.3f},R1,E01,L1X,L5X,90.0,{0.3 if t == 30.0 else 0.001},0.0,0.003,0,0,0,0.003,1,0.1,0.1,0,'
               f'{1 if t == 30.0 else 0},1,0\n' for t in TOWS[1:]))
    _write(logs / 'network_slip_raw.csv', 'week,tow,station,sat,sig1,sig2,elev_deg,mw_m,gf_m,l1_c1_m,lli1,lli2\n' + ''.join(
        f'{WEEK},{t:.3f},R1,E01,L1X,L5X,90.0,{-2.0 + (0.86 if t == 60.0 else 0)},0.5,1.0,{1 if t == 30.0 else 0},0\n' for t in TOWS))
    _write(logs / 'network_amb_state.csv', '# network_amb_state.csv - a description line, as the engine writes it\n'
           'week,tow,station,sat,signal,value_abs_m,var_m2,var_first_m2,var_birth_m2,fold_var_m2,n_folds,age_ep,'
           'age_since_relevel_ep,folded,route,event\n' + ''.join(
               f'{WEEK},{t:.3f},R1,E01,L1X,{10.0 + (0.5 if t == 60.0 else 0)},0.01,0.01,0.01,0,0,0,0,0,,{"birth" if t == 0.0 else ""}\n'
               for t in TOWS))
    _write(logs / 'network_solution.csv', 'week,tow,sat,kind,signal,value_m,disc_counter\n' + ''.join(
        f'{WEEK},{t:.3f},E01,PB,L1X,1.0,{1 if t == 60.0 else 0}\n' for t in TOWS))
    _write(logs / 'closed_loop_trace.csv', 'week,tow,n_sats,pos_err_3d_m,east_m,north_m,up_m\n' + ''.join(
        f'{WEEK},{t:.3f},5,{0.5 - t / 150},0.1,0.1,0.1\n' for t in TOWS))
    return run


def undelta(a, flags, sentinel):
    """The page's adding back of a series coded as steps (core.js undeltaBlock), for the tests."""
    out = a.astype(np.int64).reshape(-1, a.shape[-1]).copy()
    for r, f in enumerate(flags.reshape(-1)):
        if not f:
            continue
        acc, first = 0, True
        for i in range(out.shape[1]):
            if out[r, i] == sentinel:
                continue
            acc = out[r, i] if first else acc + out[r, i]
            first = False
            out[r, i] = acc
    return out.reshape(a.shape)


def _products():
    """Five Galileo satellites; E01 steps by whole cycles (+1, +1) and E02 by a fraction (+0.5, 0) at the third
    epoch, on L1X / L5X (codes C1X / C5X stay put)."""
    l1 = R.C_M_S / (R.freq_mhz('E', 'L1X') * 1e6)
    l5 = R.C_M_S / (R.freq_mhz('E', 'L5X') * 1e6)
    lines = ['# gnss_network_product v2\n', 'S,R1,1,nominal-wu93,2,0,1,0,0.05,1,x\n']
    for k, tow in enumerate(TOWS):
        lines.append(f'H,{WEEK},{int(tow)},ok,1.0,5,-\n')
        lines.append(f'E,{WEEK},{int(tow)},0,0,0,0\n')
        for j, s in enumerate(SATS):
            c1, c5 = 0.0, 0.0
            if k == 2 and s == 'E01':
                c1, c5 = 1.0, 1.0
            if k == 2 and s == 'E02':
                c1 = 0.5
            lines.append(f'C,{s},{0.1 * j}\n')
            lines.append(f'B,{s},C1X,{0.2 * j}\nB,{s},C5X,{0.3 * j}\n')
            # the level E_b = (C - P_b + mu_b x) / lambda_b rises by c_b cycles when P_b falls by c_b lambda_b
            lines.append(f'P,{s},L1X,{1.0 + j - c1 * l1},1,0\nP,{s},L5X,{2.0 + j - c5 * l5},1,0\n')
    return ''.join(lines)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()


class TestReaders(Fixture):
    def test_tree_rows_and_bands(self):
        run = make_run(self.root)
        rows, bands = R.read_tree(run / 'logs')
        self.assertEqual(bands, ['E:L1X', 'E:L5X'])
        self.assertEqual(len(rows), 3 * 2 * 4)
        self.assertEqual(rows[0][0], WEEK * R.WEEK_MS)

    def test_missing_file_is_none(self):
        self.assertIsNone(R.read_ring_fix(self.root))
        self.assertIsNone(R.read_rpb(self.root))

    def test_events_drop_diagnostics(self):
        run = make_run(self.root)
        rows, dropped = R.read_events(run / 'logs')
        self.assertEqual(dropped, {'PRIORRES': 1})
        self.assertEqual(len(rows), 5)

    def test_ecef_to_geodetic(self):
        lat, lon, h = R.ecef_to_geodetic(0.0, 6378137.0, 0.0)
        self.assertAlmostEqual(lat, 0.0, places=6)
        self.assertAlmostEqual(lon, 90.0, places=6)
        self.assertAlmostEqual(h, 0.0, places=3)


class TestQuantities(Fixture):
    def test_gate_algebra_matches_the_fixing_trace(self):
        wl_int, wl_frac, s_wl, n_int, n_frac, s_if = B.chord_quantities(
            'E|L1X+L5X', [REAL['f1']], [REAL['f2']], [REAL['q11']], [REAL['q12']], [REAL['q22']])
        self.assertEqual(int(wl_int[0]), REAL['wl'])
        self.assertAlmostEqual(abs(wl_frac[0]), REAL['frac_wl'], places=3)
        self.assertAlmostEqual(n_int[0] + n_frac[0], REAL['n_if'], places=3)
        self.assertAlmostEqual(s_if[0], REAL['sigma_if'], places=3)

    def test_jump_classes(self):
        r = (R.freq_mhz('E', 'L1X') / R.freq_mhz('E', 'L5X'))
        f1, f2 = R.freq_mhz('E', 'L1X'), R.freq_mhz('E', 'L5X')
        dn = lambda a1, a2: (f1 * a1 - f2 * a2) / (f1 - f2)          # noqa: E731
        self.assertEqual(B.jump_class(0.05, 0.02, dn(0.05, 0.02)), 0)
        self.assertEqual(B.jump_class(1.02, 0.98, dn(1.02, 0.98)), 1)
        self.assertEqual(B.jump_class(0.3, 0.3 * r, dn(0.3, 0.3 * r)), 2)
        self.assertEqual(B.jump_class(0.5, 0.0, dn(0.5, 0.0)), 3)

    def test_event_lanes(self):
        self.assertEqual(B.event_lane('RINGFIX', 'hold', 'decide'), 'hold')
        self.assertEqual(B.event_lane('RINGFIX', 'release', 'gap'), 'release')
        self.assertEqual(B.event_lane('RINGFIX', 'to_tree', 'chain'), 'tree')
        self.assertEqual(B.event_lane('CHAIN', 'rehost', 'join'), 'tree')
        self.assertEqual(B.event_lane('T1_gf_jump', 'gf_jump', 'refloat'), 'slip')
        self.assertEqual(B.event_lane('T0_gap', 'arc_resume', 'clean'), 'gap')
        self.assertEqual(B.event_lane('GROSSLOS', 'gross_los_declare', 'declare'), 'alarm')
        self.assertEqual(B.event_lane('CB_DATUM', 'reseed_disc_landed', 'disc_bump'), 'coderef')
        self.assertEqual(B.event_lane('MDB', 'mdb_exceedance', 'observe'), 'other')

    def test_hops_classes(self):
        d = np.array([-1, 1, 3, 5, 7, 9])
        np.testing.assert_array_equal(B.hops_class_of(d, d), [0, 1, 2, 3, 4, 4])


class TestBlock(Fixture):
    def test_tree_arrays(self):
        header, a = B.build_block(make_run(self.root), log=lambda *x: None)
        nodes = [s['name'] for s in header['stations']] + header['sats']
        n = {name: i for i, name in enumerate(nodes)}
        b = header['bands'].index('E:L1X')
        par, dep, tgt = a['tree_parent'], a['tree_depth'], a['tree_target']
        self.assertEqual(par[b, n['R1'], 0], -2)                      # the reference receiver
        self.assertEqual(par[b, n['E02'], 0], n['R2'])
        self.assertEqual(dep[b, n['E02'], 0], 3)
        self.assertEqual(par[b, n['E02'], 2], n['R1'])                # moved at the third epoch
        self.assertEqual(dep[b, n['E02'], 2], 1)
        self.assertEqual(tgt[b, n['E02'], 2], -3)
        self.assertEqual(par[b, n['E05'], 0], -1)                     # never on the tree

    def test_held_whole_numbers_decode(self):
        header, a = B.build_block(make_run(self.root), log=lambda *x: None)
        sta = [s['name'] for s in header['stations']].index('R3')
        sat = header['sats'].index('E01')
        link = [l for l in range(len(a['link_fam'])) if a['link_sta'][l] == sta and a['link_sat'][l] == sat][0]
        rows = range(a['link_start'][link], a['link_start'][link] + a['link_count'][link])
        states = [int(a['ce_state'][r]) for r in rows]
        self.assertEqual(states, [R.STATE_CODES['window'], R.STATE_CODES['held']])
        r = list(rows)[1]
        n1 = int(a['ce_nint'][r]) + int(a['ce_dn1'][r])
        wl = int(a['ce_wlint'][r]) + int(a['ce_dwl'][r])
        self.assertEqual((n1, n1 - wl), (-128, -37))                  # the held whole numbers of the trace
        self.assertAlmostEqual(float(a['ce_res'][r]), 0.004, places=6)
        summary = a['sum_links']
        self.assertEqual(int(summary[1, 0, 2]), 1)                     # one held link at the third epoch

    def test_jumps_from_products(self):
        header, a = B.build_block(make_run(self.root), log=lambda *x: None)
        got = {(header['sats'][int(s)], int(c)) for s, c in zip(a['jp_sat'], a['jp_cls'])}
        self.assertIn(('E01', 1), got)                                 # whole cycles
        self.assertIn(('E02', 3), got)                                 # a fraction of a cycle
        self.assertEqual(len(got), 2)

    def test_missing_and_empty_parts(self):
        header, _a = B.build_block(make_run(self.root, fixing=False), log=lambda *x: None)
        parts = {m['part'] for m in header['missing']}
        self.assertIn('fixing', parts)
        self.assertIn('receiver phase bias', parts)
        self.assertNotIn('alarms', parts)                              # present with a header and no rows
        header, a = B.build_block(make_run(self.root / 'b', rpb=True), log=lambda *x: None)
        self.assertNotIn('receiver phase bias', {m['part'] for m in header['missing']})
        self.assertIn('rx_rpb', a)

    def test_config_summary_keeps_every_fixing_key(self):
        run = make_run(self.root)
        rf = {'enable': True, 'rule': 'gated', 'pairs': ['E:L1X/L5X'], 'settled_flags': True, 'settled_rule': 'largest'}
        _write(run / 'arm.json', json.dumps({'processing': {'active_systems': ['E']},
                                             'network': {'ring_fix': rf, 'phase_datum_eq34': True}}))
        cfg = B._config_summary(run)
        self.assertEqual(cfg['file'], 'arm.json')
        self.assertEqual(list(cfg['ring_fix']), list(rf))              # every key, in the order of the file
        self.assertEqual(cfg['ring_fix']['settled_rule'], 'largest')

    def test_audit_counts_held_rows(self):
        header, _a = B.build_block(make_run(self.root), log=lambda *x: None)
        self.assertEqual(header['audits'], {'unlisted': 1})           # one held row, no audit file

    def test_sky_overhead_satellite(self):
        header, a = B.build_block(make_run(self.root), log=lambda *x: None)
        k = header['sats'].index('E01')
        self.assertAlmostEqual(float(a['sat_lat'][k, 0]), 0.0, places=3)   # straight above R1 (0 N, 0 E)
        self.assertAlmostEqual(float(a['sat_lon'][k, 0]), 0.0, places=3)
        self.assertEqual(int(a['los_el'][0, k, 0]), 90)

    def test_pack_round_trip(self):
        header, a = B.build_block(make_run(self.root), log=lambda *x: None)
        raw = nv_page.pack(header, a)
        self.assertEqual(raw[:4], b'NVB1')
        hlen = struct.unpack('<I', raw[4:8])[0]
        head = json.loads(raw[8:8 + hlen].decode('utf-8'))
        base = 8 + hlen + (-(8 + hlen)) % 8
        types = {v: k for k, v in nv_page.JS_TYPES.items()}
        for e in head['arrays']:
            self.assertEqual((base + e['offset']) % 8, 0)
            dt = types[e['type']]
            got = np.frombuffer(raw, dtype=dt.newbyteorder('<'), count=e['length'], offset=base + e['offset'])
            np.testing.assert_array_equal(got.reshape(e['shape']), np.asarray(a[e['name']]).astype(dt))


class TestTables(Fixture):
    def test_read_names_and_empty_fields(self):
        logs = self.root / 'logs'
        _write(logs / 't.csv', 'week,tow,station,v\n2420,0.000,R1,1.5\n2420,30.000,RX,\n')
        t = T.read(logs, 't.csv', {'week': T.I64, 'tow': T.F64, 'station': T.STR, 'v': T.F64, 'absent': T.F64})
        self.assertEqual(T.missing_columns(t), ['absent'])
        self.assertEqual(list(T.names(t, 'station', {'R1': 0})), [0, -1])
        v = T.num(t, 'v')
        self.assertEqual(v[0], 1.5)
        self.assertTrue(np.isnan(v[1]))
        self.assertIsNone(T.read(logs, 'missing.csv', {}))

    def test_delta_rows_round_trip(self):
        rng = np.random.default_rng(1)
        q = rng.integers(-3000, 3000, (6, 50)).astype(np.int16)
        q[q % 7 == 0] = -32768
        q[5, 0], q[5, 1] = 32000, -32000                       # steps that do not fit: the row stays as it is
        coded, flags = T.delta_rows(q, -32768)
        self.assertEqual(flags[5], 0)
        self.assertTrue((flags[:5] == 1).all())
        self.assertTrue(np.array_equal(undelta(coded, flags, -32768), q.astype(np.int64)))


class TestDashboardBlocks(Fixture):
    def setUp(self):
        super().setUp()
        self.blocks = B.build_blocks(add_dashboard_traces(make_run(self.root)), log=lambda *x: None)
        self.ne = len(TOWS)

    def test_receiver_residuals(self):
        h, a = self.blocks['receivers']
        self.assertAlmostEqual(float(a['rp_c_rms'][0, 0]), math.sqrt((0.3 ** 2 + 0.4 ** 2) / 2), places=5)
        self.assertAlmostEqual(float(a['rp_p_rms'][0, 1]), math.sqrt((0.002 ** 2 + 0.004 ** 2) / 2), places=6)
        self.assertAlmostEqual(float(a['rp_c_rms'][1, 2]), 0.5, places=5)
        self.assertEqual(int(a['rp_nsat'][0, 0]), 1)
        self.assertTrue(np.isnan(a['rp_c_rms'][2, 0]))         # R3 has no rows

    def test_line_of_sight_slots_and_steps(self):
        h, a = self.blocks['los']
        sigs = h['pf_signals']
        l = [i for i in range(len(a['los_sta'])) if a['los_sta'][i] == 0 and a['los_sat'][i] == 0][0]
        self.assertEqual(sigs[a['lsig'][l, 0]], 'C1X')
        self.assertEqual(sigs[a['lsig'][l, 2]], 'C5X')
        self.assertEqual(sigs[a['lsig'][l, 3]], 'L1X')
        self.assertEqual(sigs[a['lsig'][l, 4]], 'L5X')
        self.assertEqual(int(a['lres'][l, 0, 0]), round(0.30 * h['res_scale'][0]))
        self.assertEqual(int(a['lres'][l, 3, 0]), round(0.002 * h['res_scale'][3]))
        lam = undelta(a['lam'], a['lam_dz'], -2147483648)
        self.assertEqual(int(lam[l, 0, 2] - lam[l, 0, 0]), 500)  # +0.5 m in mm
        self.assertEqual(h['amb_events'][a['ae_code'][0]], 'birth')
        self.assertEqual(int(a['llli'][l, 1]), 1)
        self.assertEqual(int(a['ltf'][l, 1]) & 1, 1)              # flagged by the time-difference test at 30 s
        mw = a['lmw'][l].astype(float) / h['scales']['lmw'] + a['lmw_off'][l]
        self.assertAlmostEqual(mw[2] - mw[0], 0.86, places=2)

    def test_epoch_and_satellite_blocks(self):
        h, a = self.blocks['epoch']
        self.assertEqual([h['dispositions'][v] for v in a['es_disp']], ['emitted', 'gate_withheld', 'emitted'])
        i = [f[0] for f in h['es_fields']].index('sigma0_pf_code')
        self.assertAlmostEqual(float(a['es'][i, 2]), 1.1, places=5)
        j = [f[0] for f in h['cl_fields']].index('pos_err_3d_m')
        self.assertAlmostEqual(float(a['cl'][j, 2]), 0.1, places=5)
        hs, s = self.blocks['satellites']
        self.assertEqual(list(s['dc_ep']), [2])
        self.assertEqual(hs['dc_items'][s['dc_item'][0]], 'PB L1X')

    def test_quality_block(self):
        h, a = self.blocks['quality']
        self.assertEqual(h['vcf_classes'], ['E C1X'])
        self.assertAlmostEqual(float(a['vcf_f'][0, 0]), 0.6, places=5)
        k = h['pf_signals'].index('C1X')
        self.assertAlmostEqual(float(a['eb_rms'][k, 17]), 0.30, places=5)   # 90 degrees, R1-E01
        self.assertAlmostEqual(float(a['eb_zrms'][k, 17]), 0.60, places=5)  # residual / 0.5 m
        self.assertAlmostEqual(float(a['eb_rms'][k, 9]), 0.50, places=5)    # 45 degrees, R2-E02
        self.assertEqual(int(a['zh'].sum()), 15)
        self.assertEqual(list(a['if_ep']), [1])
        self.assertAlmostEqual(float(a['if_dev'][0]), 0.3, places=5)

    def test_missing_parts_named_per_layer(self):
        blocks = B.build_blocks(make_run(self.root / 'plain'), log=lambda *x: None)
        miss = {(m['layer'], m['file']) for m in blocks['core'][0]['missing']}
        self.assertIn(('stochastic', 'logs/network_pf_rows.csv'), miss)
        self.assertIn(('los', 'logs/network_amb_state.csv'), miss)
        self.assertNotIn('lres', blocks['los'][1])              # the ionosphere trace alone gives azimuths only
        self.assertIn('laz', blocks['los'][1])
        self.assertNotIn('quality', blocks)                     # nothing to show without its traces


class TestLive(Fixture):
    def setUp(self):
        super().setUp()
        from live import telemetry
        self.blocks = B.build_blocks(add_dashboard_traces(make_run(self.root)), log=lambda *x: None)
        self.src = telemetry.ReplaySource(self.blocks)
        self.telemetry = telemetry

    def test_record_from_blocks(self):
        meta = self.src.meta(window=10)
        rx_keys = [f[0] for f in meta['rx_fields']]
        rec = self.src.record(1)
        json.dumps(rec, allow_nan=False)                                   # plain JSON: empty values are null
        self.assertAlmostEqual(rec['rx'][0][rx_keys.index('code')], math.sqrt((0.3 ** 2 + 0.4 ** 2) / 2), places=4)
        self.assertIsNone(rec['rx'][2][rx_keys.index('code')])            # R3 has no residuals
        self.assertEqual(rec['e'], 1)
        n_events = int(np.sum(np.asarray(self.blocks['core'][1]['ev_ep']) == 1))
        self.assertEqual(len(rec['ev']), n_events)
        net_keys = [f[0] for f in meta['net_fields']]
        self.assertEqual(rec['net'][net_keys.index('disp')], 1.0)          # gate_withheld at 30 s

    def test_page_round_trip(self):
        out = self.root / 'page.html'
        nv_page.write_page(out, self.blocks)
        back = nv_page.read_page(out)
        self.assertEqual(sorted(back), sorted(self.blocks))
        self.assertTrue(np.array_equal(back['receivers'][1]['rp_c_rms'], self.blocks['receivers'][1]['rp_c_rms'], equal_nan=True))

    def test_server_endpoints(self):
        import threading
        import urllib.request
        from live import server
        srv, replay, _clock = server.serve(self.src, '<html>live</html>', port=0, window=10, speed=50.0)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{srv.server_address[1]}'
        try:
            meta = json.loads(urllib.request.urlopen(base + '/meta', timeout=10).read())
            self.assertEqual(meta['n_ep'], len(TOWS))
            st = json.loads(urllib.request.urlopen(base + '/control?pause=1&jump=2', timeout=10).read())
            self.assertTrue(st['paused'])
            self.assertEqual(st['e'], 2)
            snap = json.loads(urllib.request.urlopen(base + '/snapshot', timeout=10).read())
            self.assertEqual([r['e'] for r in snap['records']], [0, 1])     # the window before the jump target
            self.assertEqual(urllib.request.urlopen(base + '/', timeout=10).read(), b'<html>live</html>')
        finally:
            replay.stopped = True
            srv.shutdown()
            srv.server_close()

    def test_live_page_sources(self):
        html = nv_page.live_page_html('run')
        self.assertIn('window.NV_ASSETS', html)
        self.assertIn('turbo', html)
        self.assertNotRegex(html, r'<script[^>]+src=')


LIVE_HEADER = {'kind': 'header', 'format': 'netview-live/1',
               'stations': [{'name': 'R1', 'lat': 40.0, 'lon': -83.0}, {'name': 'R2', 'lat': 41.0, 'lon': -82.0}],
               'rx_fields': ['code_rms_m', 'phase_rms_m', 'n_sats', 'code_scale', 'max_norm', 'zwd_m', 'clk_sd_m'],
               'sat_fields': ['code_rms_m', 'n_rx', 'clk_m', 'disc']}


def live_line(tow, **kw):
    """One engine line of the live record (network/diag/LiveRecord.hpp) at GPS week 2419."""
    return dict({'t': [2419, tow]}, **kw)


class TestFollow(unittest.TestCase):
    """The follow source on a live record written in steps, as a running engine writes it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'logs' / 'network_live.jsonl'
        self.path.parent.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, *objs, partial=''):
        with open(self.path, 'a', encoding='utf-8', newline='\n') as fh:
            for o in objs:
                fh.write(json.dumps(o) + '\n')
            fh.write(partial)

    def test_held_links_from_a_fixing_record(self):
        from live import follow
        header = dict(LIVE_HEADER, rx_fields=LIVE_HEADER['rx_fields'] + ['fix_links', 'fix_held'],
                      sat_fields=LIVE_HEADER['sat_fields'] + ['fix_links', 'fix_held'])
        self.write(header,
                   live_line(30, disp='emitted', rx=[[0.25, 0.001, 12, 0.3, 4.1, 0.14, 0.008, None, None], None],
                             sat={'G05': [0.23, 2, 12.5, 3, None, None]}),          # ignition: nothing noted
                   live_line(60, disp='emitted', rx=[[0.25, 0.001, 12, 0.3, 4.1, 0.14, 0.008, 4, 3], None],
                             sat={'G05': [0.23, 2, 12.5, 3, 2, 2], 'E11': [None, 0, None, None, 2, 1]},
                             fam={'E|L1X+L5X': [2, 1], 'G|L1C+L2W': [2, 2]}))
        src = follow.FollowSource(self.path)
        self.assertEqual(src.poll(), 2)
        meta = src.meta(window=10)
        rx_keys, sat_keys = [f[0] for f in meta['rx_fields']], [f[0] for f in meta['sat_fields']]
        r0, r1 = src.record(0), src.record(1)
        self.assertEqual(r0['fam'], [])
        self.assertIsNone(r0['rx'][0][rx_keys.index('held')])
        self.assertEqual([f['key'] for f in src.families], ['E|L1X+L5X', 'G|L1C+L2W'])
        self.assertEqual(src.families[0]['label'], 'Galileo L1X + L5X')
        self.assertTrue(all(f['fixing'] for f in src.families))
        self.assertEqual(r1['fam'], [[2, 1], [2, 2]])
        self.assertEqual([f['key'] for f in r1['add']['families']], ['E|L1X+L5X', 'G|L1C+L2W'])
        self.assertAlmostEqual(r1['rx'][0][rx_keys.index('held')], 0.75)
        self.assertIsNone(r1['rx'][1][rx_keys.index('held')])               # R2 not in the solve
        self.assertAlmostEqual(r1['sat'][src.sat_ix['G05']][sat_keys.index('held')], 1.0)
        self.assertAlmostEqual(r1['sat'][src.sat_ix['E11']][sat_keys.index('held')], 0.5)
        self.assertEqual([f['key'] for f in src.lists()['families']], ['E|L1X+L5X', 'G|L1C+L2W'])
        self.assertEqual(src.meta(window=10)['families'][1]['label'], 'GPS L1C + L2W')

    def test_records_grow_with_the_file(self):
        from live import follow
        self.write(LIVE_HEADER,
                   live_line(30, disp='emitted', npres=2, nclk=30, s0=[0.29, 0.18], user=[0.02, 0.01, -0.01, 0.015],
                             rx=[[0.25, 0.001, 12, 0.3, 4.1, 0.14, 0.008], None],
                             sat={'G05': [0.23, 2, 12.5, 3]},
                             ev=[['T1_if_slip', 'slip', 'G05', 'R1', 'reset', 'k=4', 1.2, 0.5],
                                 ['T3_raim', 'flag', 'G07', 'R2', 'refloat', '', 6.0, 4.0]]))
        src = follow.FollowSource(self.path)
        self.assertEqual(src.poll(), 1)
        self.assertEqual(src.run, Path(self.tmp.name).name)
        meta = src.meta(window=10)
        self.assertEqual(meta['mode'], 'engine')
        self.assertEqual([s['name'] for s in meta['stations']], ['R1', 'R2'])
        rx_keys, sat_keys = [f[0] for f in meta['rx_fields']], [f[0] for f in meta['sat_fields']]
        net_keys = [f[0] for f in meta['net_fields']]
        r0 = src.record(0)
        json.dumps(r0, allow_nan=False)
        self.assertEqual(r0['net'][net_keys.index('disp')], 0)               # emitted is index 0
        self.assertEqual(r0['net'][net_keys.index('npres')], 2)
        self.assertAlmostEqual(r0['net'][net_keys.index('user3d')], 0.02)
        self.assertAlmostEqual(r0['rx'][0][rx_keys.index('code')], 0.25)
        self.assertEqual(r0['rx'][0][rx_keys.index('slip')], 1)              # the slip event on R1
        self.assertEqual(r0['rx'][1][rx_keys.index('alarm')], 1)             # the residual-test refloat on R2
        self.assertIsNone(r0['rx'][1][rx_keys.index('code')])                # R2 not in the solve
        self.assertEqual(src.sats, ['G05', 'G07'])                           # G07 known from its event only
        self.assertEqual(r0['sat'][0][sat_keys.index('jumps')], 0)           # the first counter is the baseline
        self.assertEqual(r0['add']['sats'], ['G05', 'G07'])
        self.assertIn('ranges', r0['add'])

        # a skipped epoch, an epoch with a new satellite and a counter step, and an incomplete line
        self.write(live_line(60, disp='sync_skip', ev=[['GATE', 'skip', '', '', 'skip', 'no stations', None, None]]),
                   live_line(90, disp='emitted', npres=2, nclk=31, s0=[0.3, 0.2],
                             rx=[[0.26, 0.001, 12, 0.3, 4.0, 0.15, 0.008], [0.3, 0.002, 10, 0.4, 3.0, 0.12, 0.01]],
                             sat={'E11': [0.2, 2, -3.1, 0], 'G05': [0.22, 2, 12.6, 5]}),
                   partial='{"t":[2419,120],"disp":"emi')
        self.assertEqual(src.poll(), 2)
        self.assertEqual(src.step_s, 30.0)
        r1, r2 = src.record(1), src.record(2)
        self.assertEqual(src.dispositions, ['emitted', 'sync_skip'])
        self.assertEqual(r1['net'][net_keys.index('disp')], 1)
        self.assertIsNone(r1['net'][net_keys.index('s0c')])
        self.assertEqual(r1['add']['dispositions'], ['sync_skip'])
        self.assertEqual(src.sats, ['G05', 'G07', 'E11'])
        self.assertEqual(len(r2['sat']), 3)
        self.assertEqual(r2['sat'][0][sat_keys.index('jumps')], 2)           # counter 3 -> 5
        self.assertAlmostEqual(r2['sat'][2][sat_keys.index('clk')], -3.1)
        self.assertEqual(r2['add']['sats'], ['E11'])
        self.assertEqual(r2['ms'] - r0['ms'], 60000)

        # the incomplete line completes; a second line of an unreleased epoch merges into it
        self.write(partial='tted","npres":1}\n')
        self.write(live_line(120, ev=[['CHAIN', 'late', 'E11', '', 'note', '', None, None]]))
        self.assertEqual(src.poll(), 1)
        r3 = src.record(3)
        self.assertEqual(r3['net'][net_keys.index('npres')], 1)
        self.assertEqual(len(r3['ev']), 1)
        self.write(live_line(120, ev=[['CHAIN', 'later', '', '', 'note', '', None, None]]))
        self.assertEqual(src.poll(), 0)
        self.assertEqual(src.late_lines, 1)                                  # record 3 was already released
        lists = src.lists()
        self.assertEqual(lists['sats'], ['G05', 'G07', 'E11'])
        self.assertIn('CHAIN', lists['tiers'])

    def test_server_follows_a_growing_record(self):
        import threading
        import time
        import urllib.request
        from live import follow, server
        self.write(LIVE_HEADER, *[live_line(30.0 * k, disp='emitted', npres=2, rx=[[0.2, 0.001, 9, 0.3, 3.0, 0.1, 0.01], None],
                                            sat={'G05': [0.2, 1, 1.0, 0]}) for k in range(1, 6)])
        src = follow.FollowSource(self.path)
        srv, replay, _clock = server.serve(src, '<html>live</html>', port=0, window=10, speed=200.0, start=99)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{srv.server_address[1]}'
        try:
            meta = json.loads(urllib.request.urlopen(base + '/meta', timeout=10).read())
            self.assertIsNone(meta['n_ep'])
            self.write(live_line(180.0, disp='emitted', npres=2, sat={'E01': [0.3, 1, 2.0, 0]}))
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and replay.e < 6:
                time.sleep(0.05)
            snap = json.loads(urllib.request.urlopen(base + '/snapshot', timeout=10).read())
            self.assertEqual([r['e'] for r in snap['records']], [0, 1, 2, 3, 4, 5])
            self.assertEqual(snap['lists']['sats'], ['G05', 'E01'])
            self.assertEqual(snap['status']['mode'], 'engine')
        finally:
            replay.stopped = True
            srv.shutdown()
            srv.server_close()


class TestCommands(Fixture):
    def test_build_writes_one_self_contained_page(self):
        run = make_run(self.root)
        out = self.root / 'out' / 'page.html'
        self.assertEqual(netview.main(['build', str(run), '-o', str(out)]), 0)
        html = out.read_text(encoding='utf-8')
        self.assertNotRegex(html, r'<script[^>]+src=')                # nothing loaded from elsewhere
        self.assertNotRegex(html, r'<link[^>]+href=')
        m = re.search(r'<script id="nv-data"[^>]*data-encoding="gzip">([A-Za-z0-9+/=]+)</script>', html)
        self.assertIsNotNone(m)
        import base64
        self.assertEqual(gzip.decompress(base64.b64decode(m.group(1)))[:4], b'NVB1')

    def test_arm_switches_trace_keys_only(self):
        src = self.root / 'cfg.json'
        src.write_text(json.dumps({'network': {'trace_events': True, 'phase_datum_eq34': True}}), encoding='utf-8')
        dst = self.root / 'armed.json'
        changed, notes = nv_arm.arm(src, dst)
        cfg = json.loads(dst.read_text(encoding='utf-8'))
        for k, _what in nv_arm.TRACE_KEYS + [nv_arm.MAP_KEY]:
            self.assertIs(cfg['network'][k], True)
        self.assertNotIn('network.trace_events', [c[0] for c in changed])
        self.assertTrue(any('ring_fix' in n for n in notes))           # the fixing is not switched on here
        self.assertFalse(any('phase_datum_eq34' in n for n in notes))
        self.assertNotIn('ring_fix', cfg['network'])
        with self.assertRaises(SystemExit):
            nv_arm.arm(src, src)

    def test_compare_page_links_two_pages(self):
        a, b = self.root / 'a.html', self.root / 'sub' / 'b.html'
        _write(a, 'x')
        _write(b, 'y')
        out = self.root / 'cmp.html'
        nv_page.write_compare(out, a, b)
        html = out.read_text(encoding='utf-8')
        self.assertIn('src="a.html"', html)
        self.assertIn('src="sub/b.html"', html)


class TestPlainWords(unittest.TestCase):
    """The page's own text keeps to plain words (the author's list of words to avoid)."""
    BANNED = ('wire', 'harness', 'field', 'producer', 'consumer', 'census', 'golden', 'ladder', 'legs', 'card',
              'standing')

    def test_page_sources(self):
        names = sorted(set(['netview.html', 'live.html'] + nv_page.CSS_FILES + nv_page.JS_FILES + nv_page.LIVE_CSS_FILES + nv_page.LIVE_JS_FILES))
        self.assertTrue(all((HERE / 'page' / n).exists() for n in names), 'a page file named in nv_page is missing')
        for name in names:
            text = (HERE / 'page' / name).read_text(encoding='utf-8').lower()
            # the one substitution the page makes for a raw trace text names the word on purpose
            text = text.replace(".replace(/wire/gi, 'stream')", '')
            for w in self.BANNED:
                self.assertIsNone(re.search(r'\b' + w + r's?\b', text), f'{name}: "{w}"')


if __name__ == '__main__':
    unittest.main(verbosity=1)
