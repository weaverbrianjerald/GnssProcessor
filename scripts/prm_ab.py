#!/usr/bin/env python3
"""Card PRM phase 0: the same-build A/B harness for refactors that claim no behaviour change.

    python scripts/prm_ab.py make <tag> [--postures golden,program] [--hours 2]
    python scripts/prm_ab.py run  <tag> --exe <Release exe> [--jobs 4]
    python scripts/prm_ab.py cmp  <tag A> <tag B> [--all-csv]

`make` writes two arms per posture with scripts/rapid_arm.py, all from the committed goldens:
  _rap_prm_<tag>_gf / _gs   config/golden/golden_{field,sim}_refound.json as generated
  _rap_prm_<tag>_pf / _ps   the same plus the knobs of the clock-level program posture
                            (PROGRAM below: the smoothed block hold and the monitors, with card
                            LAH's phase-clock hold off, which the loader refuses with the
                            smoothed hold), the posture of cards CTU2, SMD, REV and LGM
  _rap_prm_<tag>_sf / _ss   the golden with the scalar coupling update (SCALAR below: card CTU's
                            exact update off), the route the goldens ran before card EXL
  _rap_prm_<tag>_df / _ds   the golden on the diagonal border fold (DIAG below: no dense carried
                            covariance, so no conditional coupling, no exact update and the level
                            vehicle)
  _rap_prm_<tag>_c? / r? / q? / o? / h?   card PRM phase 2: card CLV's level re-base, the
                            ephemeris cut-over re-level, the eq34 D4 shift, the declared anchor
                            handover, and the handover S-transform before the rows (CLV,
                            RELEVEL, EQ34, DHO, HANDOVER below)
The sim arms start at 00:30:15 (the fixture-start convention); every arm runs --hours (default 2).

`run` runs the arms of one tag with the given executable (a renamed copy of the Release build;
never the Debug tree).

`cmp` compares two tags arm by arm (with --map X=Y, tag A's posture-X arms against tag B's
posture-Y arms) on the rules of card PRM section 8: products.txt without its
'#' comment lines, logs/closed_loop_trace.csv and logs/network_epoch_status.csv, byte for byte.
With --all-csv it also compares every other CSV under logs/ that both arms wrote, with the
wall-clock columns of VOLATILE_COLS dropped (the only columns that differ between two runs of one
executable, from the phase 0 self-identity check).
Exit status 0 when every compared file is identical.
"""
import argparse, glob, hashlib, io, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
GOLDEN = {'f': ROOT + '/config/golden/golden_field_refound.json',
          's': ROOT + '/config/golden/golden_sim_refound.json'}
SIM_START_H = '0.504167'
PROGRAM = [
    'network.amb_float_drop_decouple_always=true',
    'network.carry_coupling_exact_update=true',
    'network.carry_rx_code_bias_in_block=true',
    'network.clock_anchor_guard_space=phase',
    'network.clock_level_monitor=true',
    'network.emit_pb_hold_smoothed=true',
    'network.emit_pb_hold_smoothed_block=true',
    'network.phase_visible_monitor=true',
    'network.rank_gate_tol=1e-11',
]
# Since card EXL (2026-10-01) the goldens arm the exact update with its companions and card LAH's
# phase-clock hold; the smoothed hold is refused with the phase-clock hold, so the program posture
# switches the latter off.  The remaining entries are already in the goldens and stay listed as
# the posture's statement.
PROGRAM = ['network.emit_pb_hold_phase_clock=false'] + PROGRAM
SCALAR = ['network.carry_coupling_exact_update=false']
# The float vehicle's fold is the conditional coupling, which needs the dense carry, so the diagonal
# border fold is reached only on the level vehicle (and without the held phase datum it needs).
DIAG = [
    'network.ambiguity_vehicle=level',
    'network.carry_conditional_coupling=false',
    'network.carry_coupling_gauge_project=false',
    'network.carry_coupling_reinit_sources=false',
    'network.carry_coupling_exact_update=false',
    'network.carry_coupling_time_update=false',
    'network.carry_rx_code_bias_in_block=false',
    'network.phase_datum_held_basis=false',
    'network.shared_carry_full_cov=false',
]
# Card PRM phase 2: the postures on which each carry re-base mechanism the goldens leave inert
# actually runs (the probes of 2026-10-02, card PRM section 16).  Card CLV's level re-base
# (with the code-bias follow; the loader refuses the level carry with card LAH's hold); the
# ephemeris cut-over re-level (broadcast a priori from the brdc-only data root, without the
# satellite code bias the loader refuses there without the SH field; the field needs 2.1 h to
# reach the 02:00 navigation batch); the eq34 chain datum's D4 carry shift; the float vehicle's
# declared anchor handover; and the clock-pivot handover's S-transform before the coupling rows
# (step 3: behaviour-changing at the 6 h goldens' handover, compared with --map g=h).
CLV = [
    'network.emit_pb_hold_phase_clock=false',
    'network.clock_level_carry=true',
    'network.clock_level_free_cb_follow=if',
]
RELEVEL = [
    'network.apriori_source=brdc_rts',
    'files.internal_data_root=' + ROOT + '/data_brdc_rts',
    'network.estimate_sat_code_bias=false',
]
EQ34 = ['network.phase_datum_eq34=true', 'network.phase_datum_eq34_carry_shift=true']
DHO = ['network.amb_float_declared_handover=true']
HANDOVER = ['network.handover_stransform_before_rows=true']
# name -> (arm letter, knobs); the arm suffix is the letter and the bed.
POSTURES = {'golden': ('g', []), 'program': ('p', PROGRAM), 'scalar': ('s', SCALAR),
            'diag': ('d', DIAG), 'clv': ('c', CLV), 'relevel': ('r', RELEVEL),
            'eq34': ('q', EQ34), 'dho': ('o', DHO), 'handover': ('h', HANDOVER)}
CORE = ('products.txt', 'logs/closed_loop_trace.csv', 'logs/network_epoch_status.csv')
# Wall-clock columns, by file and header name: the only differences between two runs of one
# executable on the four arms (the phase 0 self-identity check of 2026-10-01, card PRM section
# 13).  `cmp --all-csv` drops these columns before comparing; every other column must match.
VOLATILE_COLS = {
    'network_datum_graph.csv': {'ms'},
    'network_run_summary.csv': {'loop_ms', 'ms_per_epoch'},
    'network_service_health.csv': {'ms_per_epoch_window'},
}


def arms(tag):
    letters = ''.join(v[0] for v in POSTURES.values())
    return sorted(os.path.basename(p)[len('_rap_'):]
                  for p in glob.glob(f'{RUNS}/_rap_prm_{tag}_[{letters}][fs]'))


def loader_reads(key):
    """Whether the config loader reads `key` (a quoted literal in src/core/config): the loader
    ignores an unknown key without a word, so an arm that sets one is silently the base."""
    src = ''.join(io.open(f, encoding='utf-8', errors='replace').read()
                  for f in glob.glob(ROOT + '/src/core/config/*.cpp'))
    return f'"{key}"' in src


def make(a):
    postures = a.postures.split(',')
    unread = sorted({k.split('=')[0] for post in postures for k in POSTURES[post][1]
                     if not loader_reads(k.split('=')[0])})
    if unread:
        sys.exit('refusing knobs the loader does not read: ' + ', '.join(unread))
    for post in postures:
        for bed in ('f', 's'):
            letter, knobs = POSTURES[post]
            name = f'prm_{a.tag}_{letter}{bed}'
            cmd = [sys.executable, ROOT + '/scripts/rapid_arm.py', 'make', '--base', GOLDEN[bed],
                   '--tag', name, '--hours', str(a.hours)]
            sets = knobs + (
                [f'network.start_offset_hours={SIM_START_H}'] if bed == 's' else [])
            for s in sets:
                cmd += ['--set', s]
            subprocess.run(cmd, check=True)


def run(a):
    tags = arms(a.tag)
    if not tags:
        sys.exit(f'no arms for tag {a.tag}; run make first')
    if 'x64-Debug' in a.exe.replace('\\', '/'):
        sys.exit('refusing a Debug executable')
    subprocess.run([sys.executable, ROOT + '/scripts/rapid_arm.py', 'run', '--tags', *tags,
                    '--jobs', str(a.jobs), '--exe', a.exe], check=True)


def lines(path, comments):
    with io.open(path, encoding='utf-8', errors='replace') as f:
        out = [l.rstrip('\n').rstrip('\r') for l in f]
    if comments:
        return [l for l in out if not l.startswith('#')]
    drop = VOLATILE_COLS.get(os.path.basename(path))
    if drop and out:
        keep = [i for i, c in enumerate(out[0].split(',')) if c not in drop]
        out = [','.join(f[i] for i in keep if i < len(f)) for f in (l.split(',') for l in out)]
    return out


def digest(ls):
    m = hashlib.sha256()
    for l in ls:
        m.update(l.encode('utf-8')); m.update(b'\n')
    return m.hexdigest()[:16]


def cmp(a):
    ok = True
    remap = dict(m.split('=') for m in a.map)
    pairs = 0
    for suffix in [v[0] + b for v in POSTURES.values() for b in 'fs']:
        suffix_b = remap.get(suffix[0], suffix[0]) + suffix[1]
        da, db = f'{RUNS}/_rap_prm_{a.tag_a}_{suffix}', f'{RUNS}/_rap_prm_{a.tag_b}_{suffix_b}'
        if not (os.path.isdir(da) and os.path.isdir(db)):
            continue
        pairs += 1
        files = list(CORE)
        if a.all_csv:
            ca = {os.path.relpath(p, da).replace(os.sep, '/') for p in glob.glob(da + '/logs/*.csv')}
            cb = {os.path.relpath(p, db).replace(os.sep, '/') for p in glob.glob(db + '/logs/*.csv')}
            files += sorted((ca & cb) - set(CORE))
            for only in sorted(ca ^ cb):
                print(f'  {suffix}  {only}: present in one arm only')
                ok = False
        print(f'== {suffix}: {os.path.basename(da)} vs {os.path.basename(db)}')
        for rel in files:
            pa, pb = f'{da}/{rel}', f'{db}/{rel}'
            if not (os.path.exists(pa) and os.path.exists(pb)):
                print(f'  {rel:42s} MISSING ({"A" if not os.path.exists(pa) else "B"})')
                ok = False
                continue
            la, lb = lines(pa, rel == 'products.txt'), lines(pb, rel == 'products.txt')
            same = la == lb
            if not same:
                ok = False
            first = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y),
                         None if len(la) == len(lb) else min(len(la), len(lb)))
            tagv = ' (wall-clock columns dropped)' if os.path.basename(rel) in VOLATILE_COLS else ''
            print(f'  {rel:42s} {digest(la)}/{len(la)}  {digest(lb)}/{len(lb)}  '
                  f'{"IDENTICAL" if same else "DIFFER at line " + str(first)}{tagv}')
            if not same and first is not None:
                print('     A:', (la[first] if first < len(la) else '<eof>')[:150])
                print('     B:', (lb[first] if first < len(lb) else '<eof>')[:150])
    if pairs == 0:
        print('no arm pairs to compare')
        ok = False
    print(f'{pairs} arm pair(s) compared')
    print('RESULT', 'IDENTICAL' if ok else 'DIFFERENT')
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make'); m.add_argument('tag')
    m.add_argument('--postures', default='golden,program')
    m.add_argument('--hours', type=float, default=2.0)
    r = sub.add_parser('run'); r.add_argument('tag'); r.add_argument('--exe', required=True)
    r.add_argument('--jobs', type=int, default=4)
    c = sub.add_parser('cmp'); c.add_argument('tag_a'); c.add_argument('tag_b')
    c.add_argument('--all-csv', action='store_true')
    c.add_argument('--map', action='append', default=[],
                   help="X=Y: compare tag A's posture-X arms with tag B's posture-Y arms "
                        "(e.g. g=h: the goldens against the handover posture)")
    a = p.parse_args()
    if a.cmd == 'make':
        make(a)
    elif a.cmd == 'run':
        run(a)
    else:
        sys.exit(cmp(a))


if __name__ == '__main__':
    main()
