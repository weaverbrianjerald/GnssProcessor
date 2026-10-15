#!/usr/bin/env python3
"""Verifier of card NOM step N4: one entity's members moved from NetworkCarryState into its
entity table (scripts/nom3_designs/N4_design.md section 6.0, checks V1 to V7).

    python scripts/nom_n4_verify.py --entity station [--tree DIR] [--base REV] [--runs DIR ...]

Every check first runs on a mutated copy of the tree (its negative control), which must
FAIL; a control that passes stops the verifier before the real checks run.  Exit 0 when every
control fails and every check passes, 1 otherwise.  Standard library only; reads the working
files and, for V7, one `git diff` against --base.

  V1  coverage: each member the entity's sub-step owns (the TSV) is gone from NetworkCarryState,
      has exactly one FieldDecl row (was == member) whose persistence agrees with the TSV, and
      that row's name is a member of exactly one entity row struct.
  V2  no stale access: zero `carry.<m>` / `cs.<m>` / `ctx.carry.<m>` under src/, include/, tests/
      (the compiler is the second tool: the build fails on a stale access).
  V3  order: no range loop over an entity table outside app/NetworkEntities.*; a member keyed by
      station NAME is never walked by forEach (byStationName only, O3); no lower_bound /
      upper_bound / equal_range on a moved field (O8).  The loops over moved fields are listed.
  V4  round trip: every reach-table converter line of every --runs dir reads 0 failures
      (SKIP without --runs).
  V5  parallel: no row-inserting or resetting call (net::put / mark / drop / dropAll / unmark)
      inside a function the parallel station produce runs (kStationBuildSteps); every write
      site of a moved field is listed with its function.
  V6  snapshot: every Record row has both fill functions, a round-trip unit test names its tag,
      the writer calls recordToSnapshot for it, and kRestoreOrder equals the reader's call order.
  V7  python scripts/nom_epoch_lint.py passes, and tests/test_network_epoch_contract.cpp differs
      from --base only by the reach sampler's lines (N4.0).
"""
import argparse
import copy
import os
import re
import subprocess
import sys

STEP_OF = {'station': 'N4a', 'constellation': 'N4b', 'los': 'N4c', 'sat_signal': 'N4d',
           'satellite': 'N4e', 'arc': 'N4f'}
ROW_STRUCTS = {'station': ['StationState', 'StationSignalState'],
               'constellation': ['ConstellationState', 'ChannelState'],
               'los': ['LosState', 'LosEventState'], 'sat_signal': ['SatSignalState'],
               'satellite': ['SatelliteState'], 'arc': ['ArcState']}
TABLES = ('constellations', 'sats', 'sat_signals', 'stations', 'los', 'arcs')
RUN_CTX = 'include/app/NetworkRunContext.hpp'
ENT_HPP = 'include/app/NetworkEntities.hpp'
ENT_CPP = 'src/core/app/NetworkEntities.cpp'
TSV = 'scripts/nom3_designs/N4_fields.tsv'
WRITER = 'src/core/app/NetworkStateSnapshotWrite.cpp'
READER = 'src/core/app/NetworkWarmResume.cpp'
BUILD = 'src/core/filter/NetworkStationBuild.cpp'
CONTRACT = 'tests/test_network_epoch_contract.cpp'


class Tree:
    """The files the checks read, loaded once; a control mutates a deep copy."""

    def __init__(self, root):
        self.root = root
        self.files = {}
        for top in ('src', 'include', 'tests'):
            for d, _, fs in os.walk(os.path.join(root, top)):
                for f in fs:
                    if f.endswith(('.cpp', '.hpp', '.h')):
                        p = os.path.relpath(os.path.join(d, f), root).replace('\\', '/')
                        with open(os.path.join(d, f), encoding='utf-8', errors='replace') as fh:
                            self.files[p] = fh.read()
        with open(os.path.join(root, TSV), encoding='utf-8') as fh:
            self.tsv = fh.read()
        self.logs = {}        # run dir -> list of reach-table converter lines
        self.contract_diff = ''

    def get(self, p):
        return self.files[p]


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def body_after(text, start):
    """The text of the brace block that opens at or after index start."""
    i = text.index('{', start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return text[i:]


def struct_body(text, name):
    m = re.search(r'\bstruct\s+' + name + r'\s*\{', text)
    return body_after(text, m.start()) if m else None


def tsv_rows(tree, entity):
    rows = []
    for line in tree.tsv.splitlines():
        if line.startswith('#') or line.startswith('hpp_line'):
            continue
        c = line.split('\t')
        if len(c) >= 9 and c[8] == STEP_OF[entity]:
            rows.append({'field': c[1], 'key': c[4], 'entity': c[5], 'persist': c[7]})
    return rows


DECL_RE = re.compile(r'\{\s*"(\w+)",\s*"(\w+)",\s*Persist::(\w+),\s*"(\w*)",\s*"([\w.]*)",\s*'
                     r'([\w:&<>]+),\s*([\w:&<>]+),\s*([\w:&<>]+)\s*\}')


def field_decls(tree):
    return [dict(zip(('name', 'was', 'persist', 'record', 'fp', 'to', 'frm', 'rows'), m.groups()))
            for m in DECL_RE.finditer(strip_comments(tree.get(ENT_CPP)))]


def struct_members(body):
    """Member names declared at depth 1 of a struct body (fields only)."""
    out, depth, cur = [], 0, ''
    for ch in strip_comments(body)[1:-1]:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        elif depth == 0:
            cur += ch
            if ch == ';':
                m = re.search(r'(\w+)\s*(?:\{[^;]*)?(?:=[^;]*)?;$', cur.strip())
                if m and '(' not in cur and not cur.strip().startswith(('using', 'struct', 'enum')):
                    out.append(m.group(1))
                cur = ''
    return out


def entity_struct_members(tree, entity):
    hpp = tree.get(ENT_HPP)
    res = {}
    for s in ROW_STRUCTS[entity]:
        b = struct_body(hpp, s)
        res[s] = struct_members(b) if b else []
    return res


def moved(tree, entity):
    """(was, new name) for the entity's FieldDecl rows whose was is a TSV member."""
    tsv = {r['field'] for r in tsv_rows(tree, entity)}
    return [(d['was'], d['name']) for d in field_decls(tree) if d['was'] in tsv]


# ---------------------------------------------------------------------------
# The checks: each returns a list of failure strings (empty = pass)
# ---------------------------------------------------------------------------

def v1(tree, entity):
    fails = []
    carry = strip_comments(struct_body(tree.get(RUN_CTX), 'NetworkCarryState') or '')
    decls = field_decls(tree)
    members = entity_struct_members(tree, entity)
    for r in tsv_rows(tree, entity):
        m = r['field']
        if re.search(r'[\s>,*&]' + m + r'\s*(\{|;|=)', carry):
            fails.append(f'{m}: still declared in NetworkCarryState')
        rows = [d for d in decls if d['was'] == m]
        if len(rows) != 1:
            fails.append(f'{m}: {len(rows)} FieldDecl rows (want 1)')
            continue
        d = rows[0]
        want = r['persist'].split(':')
        if d['persist'] != want[0] or (want[0] == 'Record' and d['record'] != want[1]):
            fails.append(f"{m}: FieldDecl {d['persist']}:{d['record']} vs TSV {r['persist']}")
        owners = [s for s, ms in members.items() if d['name'] in ms]
        if len(owners) != 1:
            fails.append(f"{m}: row name {d['name']} declared in {owners} (want one entity struct)")
    return fails


def v2(tree, entity):
    fails = []
    names = [r['field'] for r in tsv_rows(tree, entity)]
    pat = re.compile(r'\b(?:ctx\.carry|carry|cs)\s*\.\s*(' + '|'.join(names) + r')\b')
    for p, s in tree.files.items():
        for i, line in enumerate(strip_comments(s).splitlines(), 1):
            for m in pat.finditer(line):
                fails.append(f'{p}:{i}: stale access {m.group(0)}')
    return fails


def v3(tree, entity, listing=None):
    fails = []
    mv = moved(tree, entity)
    news = [n for _, n in mv]
    name_keyed = {d['name'] for d in field_decls(tree)
                  for r in tsv_rows(tree, entity)
                  if d['was'] == r['field'] and r['key'] == 'std::string' and r['entity'] == 'Station'}
    loop_tbl = re.compile(r'for\s*\([^;{]*:\s*[\w.>\-]*\bentities\s*\.\s*(' + '|'.join(TABLES) + r')\s*\)')
    scan = re.compile(r'\b(lower_bound|upper_bound|equal_range)\b')
    for p, s in tree.files.items():
        if p in (ENT_HPP, ENT_CPP):
            continue
        txt = strip_comments(s)
        for i, line in enumerate(txt.splitlines(), 1):
            if loop_tbl.search(line):
                fails.append(f'{p}:{i}: range loop over an entity table')
            for n in news:
                if re.search(r'::' + n + r'\b', line):
                    if 'forEach(' in line or 'byStationName(' in line:
                        if listing is not None:
                            listing.append(f'{p}:{i}: {n}: ' + ('forEach' if 'forEach(' in line else 'byStationName'))
                        if n in name_keyed and 'forEach(' in line:
                            fails.append(f'{p}:{i}: name-keyed {n} walked by forEach (O3: byStationName)')
                    if scan.search(line):
                        fails.append(f'{p}:{i}: range scan on moved field {n} (O8)')
    return fails


CONV_RE = re.compile(r'entities as carried\): converter (.+?) round-trip failures (\d+)')


def v4(tree, entity):
    if not tree.logs:
        return []
    fails = []
    for run, lines in tree.logs.items():
        if not lines:
            fails.append(f'{run}: no reach-table converter line')
        for line in lines:
            m = CONV_RE.search(line)
            if m and int(m.group(2)) != 0:
                fails.append(f'{run}: converter {m.group(1)}: {m.group(2)} failure(s)')
    return fails


# The row-inserting or resetting calls: the generic ones (N4.0) and the keyed forms of N4b
# (the letter maps) and N4c (the line-of-sight forms, eraseIf).
WRITE_RE = re.compile(r'\bnet::(put|mark|drop|dropAll|unmark|putLos|dropLos|assignLos|'
                      r'assignLosFlags|eraseIf|assignLetterMap|mergeLetterMap)\s*\(')


def functions_of(text):
    """(name, start, end) of each top-level function definition of a .cpp file."""
    out = []
    t = strip_comments(text)
    for m in re.finditer(r'^[A-Za-z][\w:<>,\* \t&]*?\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?\{', t, re.M):
        b = body_after(t, m.end() - 1)
        out.append((m.group(1), m.start(), m.end() - 1 + len(b)))
    return out


def parallel_functions(tree):
    return set(re.findall(r'&(network\w+)\}', tree.get(BUILD)))


def v5(tree, entity, listing=None):
    fails = []
    par = parallel_functions(tree)
    news = [n for _, n in moved(tree, entity)]
    for p, s in tree.files.items():
        if not p.startswith('src/'):
            continue
        t = strip_comments(s)
        for name, a, b in functions_of(s):
            body = t[a:b]
            for m in WRITE_RE.finditer(body):
                stmt = body[m.start():body.find(';', m.start()) + 1]
                hit = [n for n in news if re.search(r'::' + n + r'\b', stmt)]
                if name in par:
                    fails.append(f'{p}: {name} (parallel produce): net::{m.group(1)}')
                if hit and listing is not None:
                    listing.append(f"{p}: {name}: net::{m.group(1)} {','.join(hit)} "
                                   f"[{'PARALLEL' if name in par else 'serial'}]")
    return fails


def v6(tree, entity):
    fails = []
    tsv = {r['field'] for r in tsv_rows(tree, entity)}
    recs = [d for d in field_decls(tree) if d['persist'] == 'Record']
    tests = '\n'.join(s for p, s in tree.files.items() if p.startswith('tests/'))
    writer = strip_comments(tree.get(WRITER))
    for d in recs:
        if d['to'] == 'nullptr' or d['frm'] == 'nullptr':
            fails.append(f"Record {d['record']}: a null fill function")
        if d['was'] in tsv:
            if not re.search(r'RecordRoundTrip\w*[^\n]*\n(?:(?!\nTEST).)*?"' + d['record'] + '"',
                             tests, re.S):
                fails.append(f"Record {d['record']}: no round-trip unit test names it")
            if f'recordToSnapshot("{d["record"]}"' not in writer:
                fails.append(f"Record {d['record']}: the writer does not call recordToSnapshot")
    m = re.search(r'kRestoreOrder\s*=\s*\{([^}]*)\}', strip_comments(tree.get(ENT_CPP)))
    order = re.findall(r'"(\w+)"', m.group(1)) if m else []
    calls = re.findall(r'recordFromSnapshot\("(\w+)"', strip_comments(tree.get(READER)))
    if order != calls:
        fails.append(f'kRestoreOrder {order} != reader call order {calls}')
    return fails


def v7(tree, entity, run_lint=True):
    fails = []
    if run_lint:
        r = subprocess.run([sys.executable, os.path.join(tree.root, 'scripts', 'nom_epoch_lint.py'),
                            '--tree', tree.root], capture_output=True, text=True,
                           encoding='utf-8', errors='replace', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        if r.returncode != 0:
            fails.append('nom_epoch_lint.py failed: ' + (r.stdout + r.stderr).strip().splitlines()[-1])
    for line in tree.contract_diff.splitlines():
        if line.startswith(('+++', '---')) or not line.startswith(('+', '-')):
            continue
        if 'networkEntitiesReachSample' in line or 'app/NetworkEntities.hpp' in line:
            continue
        fails.append('contract test changed: ' + line.strip())
    return fails


# ---------------------------------------------------------------------------
# Negative controls: one mutation per check; each must make its check fail
# ---------------------------------------------------------------------------

def controls(tree, entity):
    rows = tsv_rows(tree, entity)
    m0 = rows[0]['field']
    mv = moved(tree, entity)
    out = []

    def mutate(fn):
        t = copy.deepcopy(tree)
        fn(t)
        return t

    def add_carry(t):
        s = t.files[RUN_CTX]
        i = s.index('struct NetworkCarryState {') + len('struct NetworkCarryState {')
        t.files[RUN_CTX] = s[:i] + f'\n        std::map<int, int> {m0};\n' + s[i:]
    out.append(('V1', v1, mutate(add_carry)))
    out.append(('V2', v2, mutate(lambda t: t.files.__setitem__(
        READER, t.files[READER] + f'\nvoid ctl() {{ auto& x = ctx.carry.{m0}; }}\n'))))
    out.append(('V3', v3, mutate(lambda t: t.files.__setitem__(
        READER, t.files[READER] + '\nvoid ctl() { for (auto& kv : ctx.entities.stations) {} }\n'))))

    def bad_log(t):
        t.logs = {'control': ['Network (entities as carried): converter station name round-trip failures 1']}
    out.append(('V4', v4, mutate(bad_log)))

    def par_write(t):
        par = sorted(parallel_functions(t))
        for p, s in t.files.items():
            if not p.startswith('src/'):
                continue
            for name, a, b in functions_of(s):
                if name in par:
                    s2 = strip_comments(s)
                    i = s2.index('{', a) + 1
                    # The comment strip keeps offsets only up to the first comment; mutate the
                    # stripped text instead (the checks strip comments anyway).
                    new = mv[0][1] if mv else 'zwd'
                    t.files[p] = s2[:i] + f'\n    net::put(x, y, &net::StationState::{new});\n' + s2[i:]
                    return
    out.append(('V5', v5, mutate(par_write)))

    def swap_restore(t):
        s = t.files[ENT_CPP]
        m = re.search(r'(kRestoreOrder\s*=\s*\{)([^}]*)\}', s)
        tags = re.findall(r'"(\w+)"', m.group(2)) if m else []
        if len(tags) >= 2:
            new = ', '.join(f'"{x}"' for x in [tags[1], tags[0]] + tags[2:])
        else:
            new = '"CONTROL"'
        t.files[ENT_CPP] = s[:m.start(2)] + new + s[m.end(2):] if m else s
    out.append(('V6', v6, mutate(swap_restore)))

    def contract(t):
        t.contract_diff += '\n+static_assert(true); // control\n'
    out.append(('V7', lambda t, e: v7(t, e, run_lint=False), mutate(contract)))
    return out


def load_runs(tree, runs):
    for run in runs:
        lines = []
        for d, _, fs in os.walk(run):
            for f in fs:
                if f.startswith('main_log') or f == 'stdout.txt':
                    with open(os.path.join(d, f), encoding='utf-8', errors='replace') as fh:
                        lines += [l for l in fh if CONV_RE.search(l)]
        tree.logs[run] = lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--entity', required=True, choices=sorted(STEP_OF))
    ap.add_argument('--tree', default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument('--base', default='nom/wave4', help='revision for the contract-test diff (V7)')
    ap.add_argument('--runs', nargs='*', default=[], help='run dirs whose reach table V4 reads')
    ap.add_argument('-v', '--verbose', action='store_true', help='list the loops and write sites')
    a = ap.parse_args()
    tree = Tree(a.tree)
    tree.contract_diff = subprocess.run(['git', '-C', a.tree, 'diff', a.base, '--', CONTRACT],
                                        capture_output=True, text=True).stdout
    load_runs(tree, a.runs)
    ok = True
    print(f'nom_n4_verify: entity {a.entity} ({STEP_OF[a.entity]}), '
          f'{len(tsv_rows(tree, a.entity))} TSV member(s), {len(moved(tree, a.entity))} moved')
    for name, check, mutated in controls(tree, a.entity):
        f = check(mutated, a.entity)
        print(f'  control {name}: {"FAILS as it must" if f else "PASSES (control broken)"}'
              + (f' ({f[0]})' if f else ''))
        ok &= bool(f)
    if not ok:
        print('nom_n4_verify: a negative control passed; the checks are not trusted')
        return 1
    loops, writes = [], []
    checks = [('V1', lambda: v1(tree, a.entity)), ('V2', lambda: v2(tree, a.entity)),
              ('V3', lambda: v3(tree, a.entity, loops)),
              ('V4', lambda: v4(tree, a.entity)),
              ('V5', lambda: v5(tree, a.entity, writes)), ('V6', lambda: v6(tree, a.entity)),
              ('V7', lambda: v7(tree, a.entity))]
    for name, fn in checks:
        f = fn()
        extra = ' (SKIP: no --runs)' if name == 'V4' and not tree.logs else ''
        extra += f' ({len(loops)} loop(s) over moved fields)' if name == 'V3' else ''
        extra += f' ({len(writes)} write site(s), all serial)' if name == 'V5' and not f else ''
        print(f'  {name}: {"PASS" if not f else "FAIL"}{extra}')
        for x in f[:20]:
            print('      ' + x)
        ok &= not f
    if a.verbose:
        for x in loops + writes:
            print('    ' + x)
    print('nom_n4_verify: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
