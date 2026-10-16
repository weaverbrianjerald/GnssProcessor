"""Verifier of the epoch's events (N5a) and the entity-table writers (N5b).

    python scripts/nom_n5_verify.py [--base REV]

Reads the working tree (and, for E2, the base revision through git; default nom/n4f2).
Each check runs its negative control first; a control that passes is itself a failure.

  E1  The 68 moved members.  None is declared in NetworkCarryState or reached as
      `carry.<member>` anywhere under src/, include/ or tests/; each is declared exactly once
      in its EpochEvents group (include/network/driver/Epoch.hpp), and has exactly one net::kRunFields
      row, named "events.<group>.<member>", was == member, Persist::Epoch.
  E2  The hand clears.  Every hand clear of a moved member in the base revision (a `.clear()`
      of it or of an alias bound to it, or a reset to a literal) is listed by member with its
      site, and none remains at HEAD: no `.clear()` of a moved member, of an alias of one, or
      a literal reset of one anywhere under src/.  The reasoning that each removed clear is a
      no-op on a fresh record (every reader follows the writer in the same epoch, or the clear
      sat at the top of the only writer) is the step's results table; the check proves the
      clears are gone and that nothing re-creates them.
  E3  The 22 retained members.  Each is still a NetworkCarryState member, with its net::kRunFields
      row "carry.<member>" and the declared persistence (EngineEpoch, Pending, ColdStart, Latch).
  E4  The seam contract.  scripts/nom_epoch_lint.py passes (V0, V4, V10, V11, V12), and no
      converted seam spells `ep.events` / `nep.events` below its binding block.
  E4b The entity-table writers: the lint's check V12 finds every non-const entity-table
      binding under src/core in net::kEntityWriters and every row backed by a binding; its
      control adds an unlisted binding.
  E5  scripts/nom_n4_verify.py --whole passes (G1 for the whole struct).
"""
import argparse
import copy
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

RUN_CTX = 'include/network/state/RunContext.hpp'
EPOCH_HPP = 'include/network/driver/Epoch.hpp'
RUN_CPP = 'src/core/network/state/RunFields.cpp'

GROUPS = {
    'slip': ['carry_innov_flat_now', 'relevel', 'sid_reentry_outcome'],
    'admission': ['adm_ep_tow', 'ep_adm_sta_worst_z', 'ep_adm_net_worst_z'],
    'phase_datum': ['this_epoch_anchor', 'eq34_epoch_declared', 'eq34_epoch_bridged', 'eq34_reref'],
    'handover': ['sts_pre', 'sts_consts', 'sts_c0', 'sts_dense'],
    'code_datum': ['this_epoch_if_pin_sel', 'this_epoch_if_pin_changes', 'this_epoch_if_pin_holds',
                   'this_epoch_cb_family_rows', 'this_epoch_cb_family_births',
                   'this_epoch_cb_family_birth_skips', 'this_epoch_if_pin_real',
                   'this_epoch_extra_band_held', 'this_epoch_cb_ifpin', 'this_epoch_cb_reseed',
                   'this_epoch_cb_reseed_channels'],
    'solve': ['readmit_seed_check', 'ep_sigma0_first', 'ep_we_refloat_sats', 'ep_we_clk_move_max',
              'ep_we_clk_move_sat'],
    'install': ['ep_chord_births', 'ep_slip_refloats', 'ep_d5_flags', 'ep_d5_retaps'],
    'gross': ['gl_emit_withhold', 'gl_emit_disc', 'gl_act_declare', 'gl_act_release',
              'gl_act_station', 'gl_act_cold_refloat'],
    'qc': ['rhs_trip_pending', 'cb_excursion_sats'],
    'harvest': ['pc_trace_rows', 'amb_if_rows', 'sid_carry_prune_deaths'],
    'census': ['ep_pc_max_s', 'ep_pc_p99_s', 'ep_pc_n_gt_10', 'ep_pc_n_gt_1000', 'ep_pc_rows',
               'redun_ep_tow', 'ep_redun_rows', 'ep_uncheckable_rows', 'ep_uncheckable_max_s',
               'ep_unobservable_local', 'bb_hits', 'cg_hits'],
    'status': ['prires_ep_tow', 'ep_prires_rows', 'ep_prires_resp', 'ep_prires_frame',
               'ep_prires_reported', 'ep_prires_max_t', 'ep_s0g_formed', 'ep_s0g_sigma0',
               'ep_s0g_tension_def', 'ep_s0g_tension', 'ep_s0g_flags'],
}
GROUP_OF = {m: g for g, ms in GROUPS.items() for m in ms}
# The retained members: (persistence, why each stays in the run).
RETAINED = {
    'cxu_epoch': ('EngineEpoch', 'the station build holds a pointer into it'),
    'ccl': ('EngineEpoch', 'the station build holds a pointer into it; the run end reads it'),
    'shx': ('EngineEpoch', 'the held compression is run state; the station build holds a pointer'),
    'ep_sigma0_pf': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'ep_sigma0_pf_code': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'ep_sigma0_pf_phase': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'ep_pf_rows': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'ep_pf_code_rows': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'ep_pf_phase_rows': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'ep_h_sigma0_census': ('EngineEpoch', 'the run end prints the last epoch\'s value'),
    'clm_seam_q': ('Pending', 'set once after a warm resume, drained by the D2 inject; a PASS B skip carries it'),
    'phase_anchor_reref': ('Pending', 'drained by the stash; a withheld epoch carries it by design'),
    'sts_carry_pivot': ('Pending', 'drained by the stash; the next preview reads it'),
    'refsat_mark_sats': ('Pending', 'written in PASS A, drained by the emit prep; a skip carries it'),
    'amb_state_rows': ('Pending', 'written from PASS A on, drained by the carry-state trace; a skip carries it'),
    'this_epoch_isc_obs': ('Pending', 'cleared only by an epoch whose code datum registers code columns'),
    'this_epoch_isc_pinned': ('Pending', 'cleared only by an epoch whose code datum registers code columns'),
    'pb_hold_coupled_step_m': ('ColdStart', 'a run accumulation the run end reads'),
    'pb_hold_frz_step_abs_m': ('ColdStart', 'a run accumulation the run end reads'),
    'carry_innov_pending': ('Latch', 'set at the install, read by the next epoch\'s funnel'),
    'carry_innov_pending_flat': ('Latch', 'set at the install, read by the next epoch\'s funnel'),
    'redun_escalate_pending': ('Latch', 'set by the census, swept at the next epoch'),
}


# Files whose `carry` is a test's own stand-in struct, not NetworkCarryState (the frozen
# chain-election oracle's FakeCarry keeps the old member names).
FAKE_CARRY = {'tests/network/test_network_arc_graph_oracles.cpp'}


def strip_comments(s):
    return re.sub(r'//[^\n]*|/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), s, flags=re.S)


class Tree:
    def __init__(self, root):
        self.root = root
        self.files = {}

    def get(self, p):
        if p not in self.files:
            fp = os.path.join(self.root, p)
            self.files[p] = open(fp, encoding='utf-8', errors='replace').read() if os.path.exists(fp) else None
        return self.files[p]

    def walk(self, *dirs, exts=('.cpp', '.hpp')):
        out = []
        for d in dirs:
            for dp, _, fs in os.walk(os.path.join(self.root, d)):
                for f in fs:
                    if f.endswith(exts):
                        out.append(os.path.relpath(os.path.join(dp, f), self.root).replace(os.sep, '/'))
        return sorted(out)


def struct_body(text, name):
    m = re.search(r'\bstruct\s+' + name + r'\s*\{', text)
    if not m:
        return None
    i, d = m.end() - 1, 0
    for j in range(i, len(text)):
        d += (text[j] == '{') - (text[j] == '}')
        if d == 0:
            return text[i:j + 1]
    return None


def declared(body, member):
    return re.search(r'[\w>\]]\s+(?:\w+\s*=\s*[^,;]*,\s*)*' + member + r'\s*(\[[^\]]*\])?\s*(\{[^}]*\}|=[^;,]*)?\s*[;,]', body) is not None


def run_fields(tree):
    t = tree.get(RUN_CPP) or ''
    return re.findall(r'\{"([\w.]+)", "(\w+)", Persist::(\w+),', t)


def e1(tree):
    fails = []
    carry = strip_comments(struct_body(strip_comments(tree.get(RUN_CTX)), 'NetworkCarryState') or '')
    ev = strip_comments(struct_body(strip_comments(tree.get(EPOCH_HPP)), 'EpochEvents') or '')
    rows = run_fields(tree)
    access = re.compile(r'\b(?:carry|cs)\s*(?:\.|->)\s*(' + '|'.join(GROUP_OF) + r')\b')
    for p in tree.walk('src', 'include', 'tests'):
        if p.endswith('RunFields.cpp') or p in FAKE_CARRY:
            continue
        for m in access.finditer(strip_comments(tree.get(p))):
            fails.append(f'{p}: carry.{m.group(1)} (a moved member reached through the run state)')
    for m, g in GROUP_OF.items():
        if declared(carry, m):
            fails.append(f'NetworkCarryState::{m}: still declared')
        gb = struct_body(ev, g.title().replace('_', '')) or ''
        n_all = sum(1 for gname in GROUPS if declared(struct_body(ev, gname.title().replace('_', '')) or '', m))
        if not declared(gb, m) or n_all != 1:
            fails.append(f'EpochEvents::{g.title().replace("_", "")}::{m}: declared in {n_all} groups (want 1, its own)')
        r = [x for x in rows if x[1] == m]
        if len(r) != 1 or r[0][0] != f'events.{g}.{m}' or r[0][2] != 'Epoch':
            fails.append(f'{m}: kRunFields rows {r} (want one "events.{g}.{m}", Persist::Epoch)')
    return fails


CLEAR_RX = None


def clear_sites(text, members):
    """(member, line, text) of every hand clear of a member: `.m.clear()`, `m.clear()` on an
    alias named after it or bound to it, or a reset of `.m` to a literal."""
    out = []
    lines = text.split('\n')
    alias = {}
    for i, l in enumerate(lines):
        code = l.split('//')[0]
        b = re.search(r'&\s*(\w+)\s*=\s*[\w.>-]*?\b(' + '|'.join(members) + r')\s*;', code)
        if b:
            alias[b.group(1)] = b.group(2)
        for m in re.finditer(r'(?:\.|->)(' + '|'.join(members) + r')\s*\.\s*clear\s*\(\s*\)', code):
            out.append((m.group(1), i + 1, l.strip()))
        for m in re.finditer(r'\b(\w+)\s*\.\s*clear\s*\(\s*\)', code):
            if m.group(1) in alias and not code[:m.start()].rstrip().endswith(('.', '->')):
                out.append((alias[m.group(1)], i + 1, l.strip()))
        for m in re.finditer(r'(?:\.|->)(' + '|'.join(members) + r')\s*=\s*(-?1\.0|-1|0|0\.0|false)\s*;', code):
            out.append((m.group(1), i + 1, l.strip()))
    return out


def e2(tree, base, base_files=None):
    fails, listed = [], []
    members = list(GROUP_OF)
    if base_files is None:
        rx = r'(carry|cs)(\.|->)(' + '|'.join(members) + r')\b'
        names = [q.split(':', 1)[1] for q in subprocess.run(
            ['git', '-C', tree.root, 'grep', '-l', '-E', rx, base, '--', 'src'],
            capture_output=True, text=True).stdout.split()]
        base_files = {}
        for p in names:
            if p.endswith(('.cpp', '.hpp')):
                t = subprocess.run(['git', '-C', tree.root, 'show', f'{base}:{p}'], capture_output=True,
                                   text=True, encoding='utf-8', errors='replace').stdout
                if any(m in t for m in ('this_epoch_', 'ep_', 'gl_', 'sts_', 'relevel', 'rhs_trip',
                                        'readmit_seed_check', 'bb_hits', 'cg_hits', 'eq34_', 'carry_innov',
                                        'sid_', 'pc_trace', 'amb_if', 'cb_excursion', 'redun_ep_tow',
                                        'prires_ep_tow', 'adm_ep_tow')):
                    base_files[p] = t
    for p, t in sorted(base_files.items()):
        for site in clear_sites(strip_comments(t), members):
            listed.append((p,) + site)
    for p in tree.walk('src'):
        for m, ln, txt in clear_sites(strip_comments(tree.get(p)), members):
            fails.append(f'{p}:{ln}: a hand clear of the epoch event {m}: {txt}')
    return fails, listed


def e3(tree):
    fails = []
    carry = strip_comments(struct_body(strip_comments(tree.get(RUN_CTX)), 'NetworkCarryState') or '')
    rows = run_fields(tree)
    for m, (persist, _) in RETAINED.items():
        if not declared(carry, m):
            fails.append(f'NetworkCarryState::{m}: not declared (retained in the run)')
        r = [x for x in rows if x[1] == m]
        if len(r) != 1 or r[0][0] != f'carry.{m}' or r[0][2] != persist:
            fails.append(f'{m}: kRunFields rows {r} (want one "carry.{m}", Persist::{persist})')
    return fails


def body_spellings(tree):
    fails = []
    for p in tree.walk('src', exts=('.cpp',)):
        if p.endswith('ApplicationNetworkEpoch.cpp'):
            continue
        in_bind = False
        for i, l in enumerate((tree.get(p) or '').split('\n')):
            if re.match(r'^\s*//\s*---- From', l):
                in_bind = True
            if '// ---- End of the bindings ----' in l:
                in_bind = False
            if re.search(r'\bn?ep\.events\b', l.split('//')[0]) and not in_bind \
                    and not re.match(r'^\s*(const\s+)?[\w:<>,\s]+&\s*\w+\s*=\s*n?ep\.events', l):
                fails.append(f'{p}:{i + 1}: `ep.events` below the binding block: {l.strip()[:90]}')
    return fails


def e4(tree):
    r = subprocess.run([sys.executable, os.path.join(tree.root, 'scripts', 'nom_epoch_lint.py'),
                        '--tree', tree.root], capture_output=True, text=True, encoding='utf-8',
                       errors='replace', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
    fails = [] if r.returncode == 0 else ['nom_epoch_lint.py: ' + r.stdout.strip().splitlines()[-1]]
    return fails + body_spellings(tree)


def mutated(tree, path, fn):
    t = copy.copy(tree)
    t.files = dict(tree.files)
    t.get(path)
    t.files[path] = fn(tree.get(path))
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='nom/n4f2')
    a = ap.parse_args()
    tree = Tree(ROOT)
    ok = True

    def report(name, fails, control_fails, extra=''):
        nonlocal ok
        good = bool(control_fails) and not fails
        ok &= good
        print(f'  [{"pass" if good else "FAIL"}] {name}' + (f': {extra}' if extra else ''))
        if not control_fails:
            print('      negative control did not fail')
        for f in fails[:20]:
            print('      ' + f)

    c1 = e1(mutated(tree, RUN_CTX, lambda s: s.replace('struct NetworkCarryState {',
                                                         'struct NetworkCarryState {\n        long long ep_d5_flags = 0;', 1)))
    report('E1 moved members', e1(tree), c1, f'{len(GROUP_OF)} members in {len(GROUPS)} groups')
    gl = 'src/core/network/qc/GrossLos.cpp'
    c2, _ = e2(mutated(tree, gl, lambda s: s.replace('// ---- End of the bindings ----',
                                                     'gross_ev.gl_act_declare.clear();\n    // ---- End of the bindings ----', 1)),
               a.base, base_files={})
    f2, listed = e2(tree, a.base)
    by = {}
    for p, m, ln, txt in listed:
        by.setdefault(m, []).append(f'{p.split("/")[-1]}:{ln}')
    report('E2 hand clears', f2, c2, f'{len(listed)} clears in the base, of {len(by)} members; none at HEAD')
    for m in sorted(by):
        print(f'        {m}: {", ".join(by[m])}')
    c3 = e3(mutated(tree, RUN_CPP, lambda s: s.replace('Persist::Latch', 'Persist::Pending', 1)))
    report('E3 retained members', e3(tree), c3, f'{len(RETAINED)} members')
    sp = 'src/core/network/carry/InstallCarry.cpp'
    c4 = body_spellings(mutated(tree, sp, lambda s: s.replace('    // ---- End of the bindings ----\n',
                                                              '    // ---- End of the bindings ----\n    (void)ep.events;\n', 1)))
    report('E4 seam contract (lint V0 V4 V10 V11 V12, bindings)', e4(tree), c4)
    import nom_epoch_lint as lint

    class LintTree:   # the lint's tree interface over a mutated copy
        def __init__(self, t):
            self.t = t

        def files(self, *prefixes):
            return [q for q in self.t.walk(*prefixes)]

        def read(self, q):
            return self.t.get(q)

    c12, _ = lint.check_v12(LintTree(mutated(tree, sp, lambda s: s.replace(
        '    // ---- End of the bindings ----\n',
        '    net::LosTable& los_extra = ctx.entities.los;\n    // ---- End of the bindings ----\n', 1))))
    f12, i12 = lint.check_v12(LintTree(tree))
    report('E4b entity-table writers (V12, the kEntityWriters table)', f12, c12, i12)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'nom_n4_verify.py'), '--whole'],
                       capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=ROOT)
    whole = r.returncode == 0 and 'control G1: FAILS as it must' in r.stdout
    report('E5 nom_n4_verify --whole', [] if whole else [r.stdout.strip().splitlines()[-1]], ['inherited'])
    print('nom_n5_verify: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
