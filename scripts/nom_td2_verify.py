#!/usr/bin/env python3
"""Card NOM2 step TD2: the reduced relocation verifier (gate G-TD2-R).

Proves the regrouping of the run-end table by subsystem (scripts/nom_td2_cut.py) against
the tree at BASE (nom/wave3 when the cut ran):

  C1  every BASE report is called exactly once in the new run end, as a table row or as a
      section of one subsystem report (under its declared new name); the only new report is
      the split's (the receiver-clock jump gate).
  C2  every report body is verbatim but for its signature line (`static`, the declared new
      name); the split section plus the split report's moved block equal the BASE body, so
      every moved report line lands once.
  C3  each row's home file defines its report; a section is `static`, defined in its
      subsystem's file, and declared in no header; each subsystem report is declared once.
  C4  within a subsystem the sections keep their BASE print order.
  C5  the order constraints of filter/NetworkRunEnd.hpp: the trace close first; the manifest
      records in their BASE order and before `complete`; the contract round trip before the
      closed-loop and run summaries; the ambiguity-block bar before the injection verdict;
      the injection, canonical-pair, all-channel, multi-GNSS and ledger-emit reports last.
  C6  the walk prints the one declared new line, the heading `Network[run-end] <row name>`.

Usage: python scripts/nom_td2_verify.py [--negative]
  --negative runs three mutations in memory (a changed literal, a dropped section, two
  sections swapped) and passes only when each fails its named check.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nom_td2_cut as cut  # noqa: E402  (the cut's lists and parsers)

BASE = '4816d1d'
ROOT = cut.ROOT
ROW_RE = re.compile(r'    \{"((?:[^"\\]|\\.)*)",\n     "([^"]+)", (\w+)\},\n')
SECTIONS_RE = re.compile(r'return networkRunEndSections\(v, \{([^}]*)\}\);')


def git_show(path):
    return subprocess.run(['git', '-C', ROOT, 'show', f'{BASE}:{path}'], capture_output=True,
                          check=True).stdout.decode('utf-8').replace('\r\n', '\n')


class Tree:
    def __init__(self, base, overrides=None):
        self.base, self.over, self.cache = base, overrides or {}, {}

    def read(self, rel):
        if rel in self.over:
            return self.over[rel]
        if rel not in self.cache:
            self.cache[rel] = git_show(rel) if self.base else \
                cut.rd(os.path.join(ROOT, rel)).replace('\r\n', '\n')
        return self.cache[rel]

    def body(self, home, name):
        s = self.read('src/core/' + home)
        a, b = cut.find_def(s, name)
        return s[a:b]

    def rows(self):
        return ROW_RE.findall(self.read('src/core/filter/NetworkRunEnd.cpp'))


def leaves(tree):
    """[(row index, report, section or report, home)] in print order."""
    out = []
    for k, (_, home, rep) in enumerate(tree.rows()):
        m = SECTIONS_RE.search(tree.body(home, rep))
        for sec in ([x.strip() for x in m.group(1).split(',') if x.strip()] if m else [rep]):
            out.append((k, rep, sec, home))
    return out


def check(new, base, fails):
    def fail(tag, msg):
        fails.append(f'{tag} {msg}')

    rename = {sec: nn for _, _, _, _, secs in cut.MERGES for sec, nn in secs if nn}
    merged = {rep for rep, *_ in cut.MERGES}
    split_sec, _ = cut.SPLIT_FROM
    split_rep, split_home, _ = cut.SPLIT_REPORT
    brows = base.rows()
    bnames = [f for _, _, f in brows]
    bhome = {f: h for _, h, f in brows}
    lv = leaves(new)
    names = [sec for _, _, sec, _ in lv]
    # C1: once each.
    expect = [rename.get(f, f) for f in bnames] + [split_rep]
    for n in expect:
        if names.count(n) != 1:
            fail('C1', f'{n} called {names.count(n)} time(s)')
    for n in names:
        if n not in expect:
            fail('C1', f'{n} is not a BASE report or the declared split')
    inv = {v: k for k, v in rename.items()}
    # C2 and C3: verbatim bodies, homes, static sections.
    for k, rep, sec, home in lv:
        s = new.read('src/core/' + home)
        try:
            a, b = cut.find_def(s, sec)
        except KeyError:
            fail('C3', f'{sec} is not defined in {home}')
            continue
        nb = s[a:b]
        if (sec != rep) != nb.lstrip().startswith('static '):
            fail('C3', f'{sec}: a section is static, a row report is not')
        if sec == split_rep:
            continue
        old = inv.get(sec, sec)
        ob = base.body(bhome[old], old)
        sig_o, sig_n = ob[:ob.index('\n')], nb[:nb.index('\n')]
        if sig_n.replace('static ', '', 1).replace(sec + '(', old + '(', 1) != sig_o:
            fail('C2', f'{sec}: signature {sig_n.strip()!r} against {sig_o.strip()!r}')
        if sec == split_sec:
            moved = new.body(split_home, split_rep)
            blk = moved[moved.index('\n\n') + 2:moved.rindex('    return 0;\n')]
            z = nb.rindex('    return 0;\n')
            nb = nb[:z] + blk + nb[z:]
        if nb[nb.index('\n'):] != ob[ob.index('\n'):]:
            fail('C2', f'{sec}: body differs from {BASE}:{bhome[old]} {old}')
    touched = merged | {r for r, *_ in cut.MOVES} | {split_rep}
    for _, home, rep in new.rows():
        if rep not in touched:
            continue
        hdr = new.read('include/' + home[:-4] + '.hpp')
        if hdr.count(f'int {rep}(const NetworkRunEndView& v);') != 1:
            fail('C3', f'{rep} is not declared once in include/{home[:-4]}.hpp')
    for k, rep, sec, home in lv:
        if sec != rep and f'int {sec}(const NetworkRunEndView& v);' in \
                new.read('include/' + home[:-4] + '.hpp'):
            fail('C3', f'section {sec} is still declared in its header')
    for rep, home, *_ in cut.MERGES:
        if rep not in [r for _, _, r in new.rows()]:
            fail('C3', f'subsystem report {rep} has no row')
    for rep, frm, to, _ in cut.MOVES:
        if dict((r, h) for _, h, r in new.rows()).get(rep) != to:
            fail('C3', f'{rep} does not live in {to}')
    # C4: section order within a subsystem.
    pos = {f: i for i, f in enumerate(bnames)}
    for rep in merged:
        secs = [inv.get(sec, sec) for _, r, sec, _ in lv if r == rep]
        if secs != sorted(secs, key=lambda f: pos[f]):
            fail('C4', f'{rep}: sections {secs} are not in BASE order')
    # C5: order constraints (by leaf).
    at = {sec: i for i, (_, _, sec, _) in enumerate(lv)}
    if lv and lv[0][2] != 'networkTraceCloseRunEnd':
        fail('C5', 'the trace close is not first')
    recs = ['networkParamInventoryTableRunEnd', 'networkContributionsRunEnd',
            'networkSignalRosterPlanRunEnd', 'networkCarryDispositionTableRunEnd',
            'networkCarrySitesRunEnd', 'networkResponsesRunEnd', 'networkRunManifestCompleteRunEnd']
    if [at.get(r, -1) for r in recs] != sorted(at.get(r, -1) for r in recs):
        fail('C5', 'the manifest records are out of order (or complete is not last)')
    c = at.get('networkProductContractRunEnd', 1 << 30)
    if not (c < at.get('networkClosedLoopRunEnd', -1) and c < at.get('networkRunSummaryRunEnd', -1)):
        fail('C5', 'the contract round trip does not precede the closed-loop and run summaries')
    if not at.get('networkAmbVehicleRunEnd', 1 << 30) < at.get('networkInjectRunEnd', -1):
        fail('C5', 'the ambiguity-block bar does not precede the injection verdict')
    tail = ['networkInjectRunEnd', 'networkCanonicalPairRunEnd', 'networkChannelAllRunEnd',
            'networkMultiGnssRunEnd', 'networkLedgerEmitRunEnd']
    if names[-len(tail):] != tail:
        fail('C5', f'the tail reports are not last in order: {names[-len(tail):]}')
    # C6: the heading is the one new line, printed by the walk.
    walk = new.read('src/core/filter/NetworkRunEnd.cpp')
    if walk.count('"Network[run-end] "') != 1:
        fail('C6', 'the heading literal "Network[run-end] " is not written once by the walk')
    if 'networkRunEndCallRow(row, view)' not in new.read('src/core/filter/NetworkTeardown.cpp'):
        fail('C6', 'networkTeardown does not walk the rows through networkRunEndCallRow')
    return lv


def run(overrides=None, quiet=False):
    fails = []
    lv = check(Tree(False, overrides), Tree(True), fails)
    if not quiet:
        for f in fails:
            print('FAIL', f)
        rows = len({k for k, *_ in lv})
        secs = sum(1 for _, rep, sec, _ in lv if sec != rep)
        print(f'{len(lv)} reports in {rows} rows ({secs} as sections of '
              f'{len(cut.MERGES)} subsystem reports); BASE {BASE}: {len(Tree(True).rows())} rows; '
              + ('PASS' if not fails else f'{len(fails)} failure(s)'))
    return fails


def negative():
    t = Tree(False)
    slip = 'src/core/slip/NetworkSlipQcRunEnd.cpp'
    raim = 'src/core/filter/NetworkRaim.cpp'
    s, r = t.read(slip), t.read(raim)
    cases = [
        ('C2', 'a changed literal', {slip: s.replace('Network (IF-TD re-seed, card ARL)',
                                                     'Network (IF-TD re-seed, card ARL!)', 1)}),
        ('C1', 'a dropped section', {slip: s.replace('        networkIfTdReseedRunEnd,\n', '', 1)}),
        ('C4', 'two sections swapped', {raim: r.replace(
            '        networkRaimPoolRunEnd,\n        networkRaimWithholdRunEnd\n',
            '        networkRaimWithholdRunEnd,\n        networkRaimPoolRunEnd\n', 1)}),
    ]
    ok = True
    for tag, what, over in cases:
        f = run(over, quiet=True)
        hit = any(x.startswith(tag) for x in f)
        ok &= hit
        print(f'negative control, {what}: {"fails " + tag if hit else "NOT DETECTED"}')
    return ok


if __name__ == '__main__':
    bad = run()
    neg = negative() if '--negative' in sys.argv else True
    sys.exit(0 if not bad and neg else 1)
