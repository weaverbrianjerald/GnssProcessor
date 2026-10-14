#!/usr/bin/env python3
"""Card UOM: the same-build identical-output gate for the user side (gate G-UEP).

    python scripts/uom_gate.py make <tag> [--set quick|full]
    python scripts/uom_gate.py run  <tag> --exe <Release exe> [--jobs 4] [--arms a,b]
    python scripts/uom_gate.py cmp  <tag A> <tag B> [--arms a,b] [--show 3]

`make` writes one arm directory per arm of the set under runs/_uom_<tag>/<arm>/ with its
arm.json: a committed config (by absolute path, `extends`) plus a duration, the data root
pinned to the populated tree (DATA_ROOT, so an exe built in a worktree reads the same input),
every user-side module log at info and every user-side CSV trace on.  The arm differs from its
base by observability, duration and the named scratch knobs alone.

`run` runs every arm of a tag with one executable, each from its own directory as the working
directory (traces go to ./logs) and the executable by path out of its build directory, beside
its DLLs (a copy elsewhere dies at load).  Never a Debug executable.

`cmp` compares tag A against tag B arm by arm (card UOM section 6, measured in the
organization map, uom_map/gates.md section 8.3):
  1. the two arm.json files, equal;
  2. every file under logs/ present in both, none in one only;
  3. every CSV under logs/ in full, in row order, byte for byte (no column needs a mask on the
     user side);
  4. every .log under logs/ and the whole stdout.txt, after scripts/nom_td_gate.py's
     mask_lines plus one family of this gate: the six continuation lines of the KalmanFilter
     timing block (microsecond figures, no unit token), whose header line and call count are
     kept;
  5. the exit code.
Exit status 0 when every compared arm is IDENTICAL.  With --allow-inserted (a step that only
adds log lines, such as run-end counters), a .log or stdout.txt may differ by inserted lines
alone; the inserted lines are listed by file and text, and every CSV must still be identical.

Sets.  `quick` is sixteen 600 s arms (about 5 minutes a side at four jobs), the development
gate; `full` is the matrix of the organization map (gates.md section 8.2) without its
Baseline-Multi arm (not runnable as shipped), about 15 minutes a side.  Known: the two smoother
arms on BSSD (u_rtk_smooth, UA10) exit 0xC0000005 until the BSSD smoother trace defect is
fixed; an identical crash compares IDENTICAL.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE).replace(os.sep, '/')
RUNS = ROOT + '/runs'
CFG = ROOT + '/config/'
DATA_ROOT = 'C:/GnssProcessor/data'

sys.path.insert(0, HERE)
import nom_td_gate as g  # noqa: E402  (mask_lines, read_lines, report: one mask table)

TRACES = ['state', 'ambiguity', 'ionosphere', 'sat_bias', 'code_bias', 'sat_status', 'dop',
          'residual', 'wls', 'smoother', 'mw_slip', 'turbo_edit', 'windup']
MODULES = ['main_log', 'gnss_config', 'kalman_filter', 'state_manager', 'measurement_model',
           'state_initializer', 'cycle_slip_manager', 'mw_slip_detector', 'turbo_edit_resolver',
           'ambiguity_resolver', 'bssd_pivot_manager', 'bssd_transform', 'smoother', 'ppp_model',
           'rt_engine']
LOGGING = {f'enable_{m}': True for m in MODULES}
LOGGING.update({f'enable_{t}_trace': True for t in TRACES})
LOGGING['levels'] = {m: 'info' for m in MODULES}

TE_GR = {'cycle_slip': {'turbo_edit': True, 'turbo_edit_bridge_lli': True,
                        'graphic': True, 'ewl': True}}
# (base config relative to config/, duration s or None for the base's own, processing keys,
#  estimation keys)
QUICK = {
    'u_ppp_if':      ('ppp_internal.json', 600, {}, {}),
    'u_ppp_static':  ('test_suite/ppp_static.jsonc', 600, {}, {}),
    'u_ppp_unc_ar':  ('test_suite/_pppar_algo_static_gps_l1l2.json', 600, {}, {}),
    'u_rtk_bssd_ar': ('ppp_rtk_internal.json', 600, {}, {}),
    'u_rtk_rxcb':    ('ppp_rtk_user_rxcb.json', 600, {}, {}),
    'u_rtk_kin':     ('ppp_rtk_kinematic_multi.json', 600, {}, {}),
    'u_rtk_static':  ('test_suite/ppp_rtk_static.jsonc', 600, {}, {}),
    'u_rtk_triple':  ('ppp_rtk_internal.json', 600, {'frequency_mode': 'triple'}, {}),
    'u_rtk_te_gr':   ('ppp_rtk_internal.json', 600, {}, TE_GR),
    'u_rtk_smooth':  ('ppp_rtk_internal.json', 600, {'run_smoother': True}, {}),
    'u_rxcb_smooth': ('ppp_rtk_user_rxcb.json', 600, {'run_smoother': True}, {}),
    'u_baseline':    ('baseline_single.json', 600, {}, {}),
    'u_sbf':         ('_rtseam_sbf.json', 600, {}, {}),
    'u_sbf_te':      ('_rtseam_sbf_te.json', 600, {}, {}),
    # Reach arms (card UOM step U4): the NL/WL ambiguity basis, and the baseline DD
    # ionosphere-free row collapse.
    'u_nlwl':        ('test_suite/_nlwl_guarded.json', 600, {}, {}),
    'u_baseline_if': ('baseline_single.json', 600, {}, {}, {'baseline': {'use_iono_free': True}}),
}
FULL = {
    'UA1':  ('ppp_internal.json', 7200, {}, {}),
    'UA2':  ('test_suite/ppp_static.jsonc', 7200, {}, {}),
    'UA3':  ('test_suite/_pppar_algo_static_gps_l1l2.json', 7200, {}, {}),
    'UA4':  ('ppp_rtk_internal.json', 3600, {}, {}),
    'UA5':  ('ppp_rtk_user_rxcb.json', 3600, {}, {}),
    'UA6':  ('ppp_rtk_kinematic_multi.json', 3600, {}, {}),
    'UA7':  ('test_suite/ppp_rtk_static.jsonc', 3600, {}, {}),
    'UA8':  ('ppp_rtk_internal.json', 1800, {},
             {**TE_GR, 'adaptive_iono_process_noise': {'enable': True}}),
    'UA9':  ('ppp_rtk_user_rxcb.json', 3600, {'run_smoother': True}, {}),
    'UA10': ('ppp_rtk_internal.json', 1800, {'run_smoother': True}, {}),
    'UA11': ('ppp_rtk_internal.json', 1800, {'frequency_mode': 'triple'}, {}),
    'UA12': ('baseline_single.json', 7200, {}, {}),
    'UA13': ('baseline_single.json', 7200, {}, {'ambiguity_resolution': {'mode': 1}}),
    'UA15': ('_rtseam_sbf.json', None, {}, {}),
    'UA16': ('_rtseam_sbf_bssd.json', None, {}, {}),
    'UA17': ('_rtseam_sbf_te.json', None, {}, {}),
}
SETS = {'quick': QUICK, 'full': FULL}

# The KalmanFilter timing block (KalmanFilter::logKalmanFilterTimings): six continuation lines
# whose figures are wall-clock microseconds with no unit token for nom_td_gate's masks.
KF_BLOCK = re.compile(r'^  (S form  \(H\*P \+ S=HP\*Ht\+R\)|K solve \(LDLT \+ solve\)   |'
                      r"P Joseph \(I-KH P \(I-KH\)'\)|Sr diag \(R - H\*P_post\*Ht\)|"
                      r'total per call           |cumulative wall \(s\)      ) :\s+-?\d+(?:\.\d+)?$')


def tag_dir(tag):
    return f'{RUNS}/_uom_{tag}'


def arms_of(tag, sel):
    d = tag_dir(tag)
    names = sorted(os.path.basename(p) for p in glob.glob(d + '/*') if os.path.isdir(p))
    if sel:
        want = sel.split(',')
        names = [n for n in names if n in want]
    return names


def make(a):
    d = tag_dir(a.tag)
    for name, spec in SETS[a.set].items():
        base, dur, proc, est = spec[:4]
        top = spec[4] if len(spec) > 4 else {}
        arm = {'extends': CFG + base,
               'files': {'internal_data_root': DATA_ROOT},
               'logging': LOGGING}
        p = dict(proc)
        if dur is not None:
            p['max_duration_s'] = float(dur)
        if p:
            arm['processing'] = p
        if est:
            arm['estimation'] = est
        arm.update(top)
        os.makedirs(f'{d}/{name}', exist_ok=True)
        with open(f'{d}/{name}/arm.json', 'w') as f:
            json.dump(arm, f, indent=2)
    print(f'{len(SETS[a.set])} arm(s) -> {d}')


def run_one(exe, arm_dir):
    with open(arm_dir + '/stdout.txt', 'w', encoding='utf-8', errors='replace') as o, \
         open(arm_dir + '/stderr.txt', 'w', encoding='utf-8', errors='replace') as e:
        rc = subprocess.call([exe, arm_dir + '/arm.json'], cwd=arm_dir, stdout=o, stderr=e)
    with open(arm_dir + '/exitcode.txt', 'w') as f:
        f.write(f'{rc}\n')
    return os.path.basename(arm_dir), rc


def run(a):
    if 'x64-Debug' in a.exe.replace('\\', '/'):
        sys.exit('refusing a Debug executable')
    d = tag_dir(a.tag)
    names = arms_of(a.tag, a.arms)
    with open(d + '/exe.txt', 'w') as f:
        f.write(a.exe + '\n')
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name, rc in ex.map(lambda n: run_one(a.exe, f'{d}/{n}'), names):
            print(f'{name:16s} exit {rc}', flush=True)


def masked(lines, run_name):
    out = []
    for line in g.mask_lines(lines, run_name, False):
        m = KF_BLOCK.match(line)
        out.append(f'  {m.group(1)} : <US>' if m else line)
    return out


def files_under(d):
    return {os.path.relpath(p, d).replace('\\', '/')
            for p in glob.glob(d + '/logs/**/*', recursive=True) if os.path.isfile(p)}


def inserted_only(a, b):
    """True when list b is list a with lines inserted (a is a subsequence of b, in order).
    Returns (ok, inserted) with the inserted lines of b."""
    inserted, i = [], 0
    for line in b:
        if i < len(a) and line == a[i]:
            i += 1
        else:
            inserted.append(line)
    return i == len(a), inserted


def compare_arm(da, db, show, allow_inserted=False, families=None):
    ok = True
    if g.read_lines(da + '/arm.json') != g.read_lines(db + '/arm.json'):
        print('  arm.json differs')
        ok = False
    fa, fb = files_under(da), files_under(db)
    for rel in sorted(fa ^ fb):
        print('  present in one run only:', rel)
        ok = False
    na, nb = os.path.basename(os.path.dirname(da)), os.path.basename(os.path.dirname(db))
    for rel in sorted(fa & fb):
        la, lb = g.read_lines(f'{da}/{rel}'), g.read_lines(f'{db}/{rel}')
        if rel.endswith('.log'):
            la, lb = masked(la, na), masked(lb, nb)
            if la != lb and allow_inserted:
                sub, ins = inserted_only(la, lb)
                if sub:
                    for line in ins:
                        families[(rel, line.split('] ', 3)[-1][:90])] = families.get(
                            (rel, line.split('] ', 3)[-1][:90]), 0) + 1
                    continue
        if la != lb:
            ok = g.report(rel, la, lb, show) and ok
    sa = masked(g.read_lines(da + '/stdout.txt'), na)
    sb = masked(g.read_lines(db + '/stdout.txt'), nb)
    if sa != sb:
        sub, ins = inserted_only(sa, sb) if allow_inserted else (False, [])
        if sub:
            for line in ins:
                key = ('stdout.txt', line.split('] ', 3)[-1][:90])
                families[key] = families.get(key, 0) + 1
        else:
            ok = g.report('stdout.txt (masked)', sa, sb, show) and ok
    ea = open(da + '/exitcode.txt').read().split()[:1]
    eb = open(db + '/exitcode.txt').read().split()[:1]
    if ea != eb:
        print(f'  exit code {ea} against {eb}')
        ok = False
    return ok, ea


def cmp(a):
    names = arms_of(a.tag_a, a.arms)
    bad = 0
    families = {}
    for name in names:
        da, db = f'{tag_dir(a.tag_a)}/{name}', f'{tag_dir(a.tag_b)}/{name}'
        if not os.path.isdir(db):
            print(f'{name:16s} MISSING in {a.tag_b}')
            bad += 1
            continue
        ok, ec = compare_arm(da, db, a.show, a.allow_inserted, families)
        print(f'{name:16s} {"IDENTICAL" if ok else "DIFFERENT"} (exit {ec[0] if ec else "?"})')
        bad += 0 if ok else 1
    if families:
        print('inserted log lines (file, text up to 90 characters: count over the arms):')
        for (rel, text), n in sorted(families.items()):
            print(f'  {rel:34s} {n:5d}  {text}')
    print(f'{len(names) - bad}/{len(names)} arm(s) identical'
          + (' (logs allowed to differ by inserted lines only)' if a.allow_inserted else ''))
    print('RESULT', 'IDENTICAL' if bad == 0 else 'DIFFERENT')
    return 1 if bad else 0


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make'); m.add_argument('tag'); m.add_argument('--set', default='quick',
                                                                      choices=sorted(SETS))
    r = sub.add_parser('run'); r.add_argument('tag'); r.add_argument('--exe', required=True)
    r.add_argument('--jobs', type=int, default=4); r.add_argument('--arms')
    c = sub.add_parser('cmp'); c.add_argument('tag_a'); c.add_argument('tag_b')
    c.add_argument('--arms'); c.add_argument('--show', type=int, default=3)
    c.add_argument('--allow-inserted', action='store_true',
                   help='CSVs must be identical; a .log or stdout may differ only by inserted lines, which are listed')
    a = p.parse_args()
    if a.cmd == 'make':
        return make(a)
    if a.cmd == 'run':
        return run(a)
    return cmp(a)


if __name__ == '__main__':
    sys.exit(main() or 0)
