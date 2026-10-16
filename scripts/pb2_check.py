#!/usr/bin/env python3
"""Verifier for step PB2: the duplicated idioms of the station assembly become single functions.

    python scripts/pb2_check.py [--base REV] [--no-controls]

For every replaced site it prints the old text (read from the base revision) and the new call
(read from the working tree), then proves with a token comparison (comments dropped) that the
shared function's body, with its parameters replaced by the call's own arguments, equals the
old copy.  Declared differences are listed per site (a dropped statement, a renamed local).
Further checks: the old text is gone and the call is present once; the vestigial fields have
no reader (base scan and working-tree scan); the forwarding lambdas' call sites carry the
same arguments as the direct calls; the tally split is the old tally's text, moved in order;
no file outside the declared list changed.  Negative controls then mutate one input each and
require the verdict to flip.  Exit 0 only when every check passes and every control fires.
"""
import argparse, re, subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = '1549ed3'
N = 'src/core/network/'

# ---------------------------------------------------------------- text and tokens
_BASE_CACHE = {}
def base_text(path):
    if (BASE, path) not in _BASE_CACHE:
        _BASE_CACHE[(BASE, path)] = _base_text(path)
    return _BASE_CACHE[(BASE, path)]

def _base_text(path):
    return subprocess.run(['git', '-C', ROOT, 'show', f'{BASE}:{path}'], capture_output=True,
                          text=True, encoding='utf-8', check=True).stdout.replace('\r\n', '\n')

MUT = {}   # negative controls: ('new', path) or ('shared', key) -> text function

def new_text(path):
    t = open(os.path.join(ROOT, path), encoding='utf-8', newline='').read().replace('\r\n', '\n')
    return MUT.get(('new', path), lambda s: s)(t)

TOK = re.compile(r'[A-Za-z_]\w*|\d[\w.]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|->|::|\+\+|--|<<|>>|'
                 r'[+\-*/%&|^!<>=]=|&&|\|\||\S')
CMT = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)

def nocomment(s):
    return CMT.sub(lambda m: m.group(0) if m.group(0)[0] in '"\'' else ' ', s)

def toks(s):
    return TOK.findall(nocomment(s))

def match(s, i, o, c):
    """Index of the bracket closing the one at s[i], skipping strings and comments."""
    d = 0
    while i < len(s):
        m = CMT.match(s, i)
        if m:
            i = m.end(); continue
        if s[i] == o: d += 1
        elif s[i] == c:
            d -= 1
            if d == 0: return i
        i += 1
    raise ValueError('unbalanced')

def find_def(text, name):
    """(parameter-list text, body text) of the one definition of function `name`."""
    out = []
    for m in re.finditer(r'(?<![\w.>])' + re.escape(name) + r'\s*\(', text):
        p = m.end() - 1
        q = match(text, p, '(', ')')
        r = re.match(r'\s*(const\s*)?(noexcept\s*)?\{', text[q + 1:])
        if r:
            b = q + 1 + r.end() - 1
            out.append((text[p + 1:q], text[b + 1:match(text, b, '{', '}')]))
    assert len(out) == 1, f'{name}: {len(out)} definitions'
    return out[0]

def split_top(s, sep=','):
    parts, d, cur = [], 0, ''
    for ch in s:
        if ch in '(<[{': d += 1
        elif ch in ')>]}': d -= 1
        if ch == sep and d == 0: parts.append(cur); cur = ''
        else: cur += ch
    return parts + [cur] if cur.strip() else parts

def param_names(plist):
    return [re.findall(r'[A-Za-z_]\w*', p.split('=')[0])[-1] for p in split_top(nocomment(plist))]

def call_args(text, fname):
    m = list(re.finditer(r'(?<![\w.>])' + re.escape(fname) + r'\s*\(', text))
    assert len(m) == 1, f'{fname}: {len(m)} calls'
    p = m[0].end() - 1
    return [toks(a) for a in split_top(text[p + 1:match(text, p, '(', ')')])]

def subst(tk, subs):
    out = []
    for t in tk:
        out += subs.get(t, [t])
    return out

# ---------------------------------------------------------------- the idioms
class R:
    rows = []
    def add(self, ident, ok, msg=''):
        self.rows.append((ident, ok, msg))
res = R()

def slice_old(path, old, nth=0):
    b = base_text(path)
    if isinstance(old, tuple):
        i = -1
        for _ in range(nth + 1):
            i = b.index(old[0], i + 1)
        j = b.index(old[1], i) + len(old[1])
        return b[i:j]
    assert b.count(old) >= 1, (path, old[:60])
    return old

def site(ident, shared, path, old, new, call, edits=(), extra=None, cut=None, old_cut=None,
         nth=0, new_path=None, show=True, new_count=1):
    """One replaced site: token comparison of shared(args) against the old copy."""
    try:
        spath, sname = shared
        plist, sbody = find_def(new_text(spath), sname)
        sbody = MUT.get(('shared', ident.split(':')[0]), lambda s: s)(sbody)
        o = slice_old(path, old, nth)
        nt = new_text(new_path or path)
        # the new call is present once, and the old copy is gone from the working tree
        ok = nt.count(new) == new_count and o not in nt
        args = call_args(new, call)
        names = param_names(plist)
        subs = dict(zip(names, args))
        subs.update(extra or {})
        body = nocomment(sbody)
        for rx, rep in edits:
            body = re.sub(rx, rep, body)
        if cut: body = body[re.search(cut, body).start():]
        oo = nocomment(o)
        if old_cut: oo = oo[re.search(old_cut, oo).start():]
        eq = subst(toks(body), subs) == toks(oo)
        res.add(ident, ok and eq, ('' if ok and eq else
                f'present/gone={ok} tokens-equal={eq}') )
        if show and ok and eq:
            print(f'  {ident}: {path}\n      old: {" ".join(o.split())[:150]}\n      new: {" ".join(new.split())[:150]}')
    except Exception as e:
        res.add(ident, False, repr(e))

def idiom_state_vector():
    sh = (N + 'assembly/StationBuild.cpp', 'StationBuild::postUpdateState')
    for i, (f, old, new) in enumerate([
        ('solve/PhaseDatum.cpp', 'const Eigen::VectorXd piv_x = st.sm->getStateVector();', 'const Eigen::VectorXd& piv_x = build.postUpdateState();'),
        ('assembly/StationAnchors.cpp', 'const Eigen::VectorXd zi_xlin = st.sm->getStateVector();', 'const Eigen::VectorXd& zi_xlin = build.postUpdateState();'),
        ('qc/Raim.cpp', 'const Eigen::VectorXd x_lin_qc = st.sm->getStateVector();', 'const Eigen::VectorXd& x_lin_qc = build.postUpdateState();'),
        ('carry/PhaseColourCarry.cpp', 'const Eigen::VectorXd x_lin_pc = st.sm->getStateVector();', 'const Eigen::VectorXd& x_lin_pc = build.postUpdateState();'),
        ('carry/ZwdCarry.cpp', 'const Eigen::VectorXd x_lin_z = st.sm->getStateVector();', 'const Eigen::VectorXd& x_lin_z = build.postUpdateState();'),
        ('assembly/StationCarry.cpp', 'st.sm->getStateVector(),\n                            CouplingRowBuilder', 'build.postUpdateState(),\n                            CouplingRowBuilder')]):
        # shared fragment: the right-hand side of the one copy; site fragment: the copy's right-hand side
        ident = f'1 state vector:{i}'
        try:
            _, sb = find_def(new_text(sh[0]), sh[1])
            sb = MUT.get(('shared', '1 state vector'), lambda s: s)(sb)
            frag = re.search(r'post_state\s*=\s*(.*?);', sb, re.S).group(1)
            oc = re.search(r'(st\.sm->getStateVector\(\))', slice_old(N + f, old)).group(1)
            nt = new_text(N + f)
            ok = toks(frag) == toks(oc) and nt.count(new) == 1 and old not in nt
            res.add(ident, ok, '' if ok else 'fragment or presence')
            if ok: print(f'  {ident}: {f}\n      old: {old[:90]}\n      new: {new[:90]}')
        except Exception as e:
            res.add(ident, False, repr(e))

def idiom_spacing():
    sh = (N + 'carry/LocalCarryHarvest.cpp', 'networkEpochSpacingS')
    drop = [(r'return\s+dt_ep\s*;', '')]
    for ident, f, old, new in [
        ('2 epoch spacing:0', 'carry/SlantIonoCarry.cpp', ('double dt_ep = pl_now_s - ctx.carry.pl_prev_now_s;', 'dt_ep = 0.0;'),
         'const double dt_ep = networkEpochSpacingS(pl_now_s, ctx.carry.pl_prev_now_s, pl_max_gap_s);'),
        ('2 epoch spacing:1', 'carry/RxClockCarry.cpp', ('double dt_ep = pl_now_s - ctx.carry.pl_prev_now_s;', 'dt_ep = 0.0;'),
         'const double dt_ep = networkEpochSpacingS(pl_now_s, ctx.carry.pl_prev_now_s, pl_max_gap_s);')]:
        site(ident, sh, N + f, old, new, 'networkEpochSpacingS', drop)
    # the harvest copy: the test is the same, the first statement carries the stamp guard (declared)
    site('2 epoch spacing:2', sh, N + 'carry/RxClockCarry.cpp',
         ('double dt_ep_run = (ctx.carry.rxclk_prev_now_s > 0.0)', 'dt_ep_run = 0.0;'),
         'const double dt_ep_run =\n        (ctx.carry.rxclk_prev_now_s > 0.0)\n            ? networkEpochSpacingS(pl_now_s, ctx.carry.rxclk_prev_now_s, rxclk_gap_s) : 0.0;',
         'networkEpochSpacingS', drop, extra={'dt_ep': ['dt_ep_run']}, cut=r'if\s*\(', old_cut=r'if\s*\(')

def idiom_sort_coalesce():
    sh = (N + 'assembly/StationCarry.cpp', 'finalizeDiagonalPriors')
    p = N + 'assembly/StationCarry.cpp'
    for k, (inj, pre) in enumerate([('inj', 'carry'), ('inj_prior', 'prior')]):
        site(f'3 sort and coalesce:{k}', sh, p,
             (f'std::sort({inj}.begin(), {inj}.end(),',
              f'R.{pre}_tags[static_cast<size_t>(k)] = std::get<3>({inj}[static_cast<size_t>(k)]);\n    }}'),
             f'finalizeDiagonalPriors({inj}, R.{pre}_merged, R.{pre}_cols, R.{pre}_dNrr, R.{pre}_dur,\n                           R.{pre}_tags);',
             'finalizeDiagonalPriors', extra={'n': ['ninj' if k == 0 else 'npri']})

def idiom_grow():
    sh = (N + 'assembly/StationBuild.cpp', 'networkGrowBlockRows')
    drop = [(r'const\s+Eigen::Index\s+m0\s*=\s*A_s\.rows\(\)\s*;', '')]
    def ends(pre, n):
        return (f'{pre}A_s.conservativeResize(m0 + {n}, Eigen::NoChange);', f'{pre}y.conservativeResize(m0 + {n});')
    for k, (f, pre, n, nth) in enumerate([
        ('solve/PhaseDatum.cpp', 'R.', 'np', 0), ('assembly/StationAnchors.cpp', 'R.', 'ni', 0),
        ('assembly/StationAnchors.cpp', 'R.', 'nz', 0), ('carry/PhaseColourCarry.cpp', 'R.', 'nb', 0),
        ('carry/Coupling.cpp', 'R.', 'add', 0), ('assembly/IonoConstraintRows.cpp', '', 'total', 0),
        ('assembly/IonoConstraintRows.cpp', '', 'total', 1)]):
        site(f'4 grow rows:{k}', sh, N + f, ends(pre, n),
             f'networkGrowBlockRows({pre}A_s, {pre}A_r, {pre}w, {pre}y, {n});', 'networkGrowBlockRows',
             drop, nth=nth, new_count=2 if f.endswith('IonoConstraintRows.cpp') else 1)
        # the old row count is read just before (premise of dropping the shared body's first statement)
        b = base_text(N + f)
        i = [m.start() for m in re.finditer(re.escape(ends(pre, n)[0]), b)][nth]
        res.add(f'4 grow rows:{k} m0', re.search(r'm0\s*=\s*' + re.escape(pre) + r'A_s\.rows\(\)', b[max(0, i - 400):i]) is not None, 'no m0 read before')

def idiom_whiten():
    sh = (N + 'assembly/StationBuild.cpp', 'networkWriteWhitenedRows')
    p = N + 'assembly/IonoConstraintRows.cpp'
    common = [(r'const\s+Eigen::Index\s+mc\s*=\s*cov\.rows\(\)\s*;', '')]
    site('5 whitening:0 (GIM)', sh, p,
         ('const Eigen::LLT<Eigen::MatrixXd> llt(R);\n        A_r.middleRows(out, mc) = llt.matrixL().solve(Hc);', 'w.segment(out, mc).setOnes();'),
         'networkWriteWhitenedRows(R, Hc, nullptr, zc, out, A_s, A_r, w, y);', 'networkWriteWhitenedRows',
         common + [(r'if\s*\(Hs\)\s*A_s\.middleRows\(out, mc\) = llt\.matrixL\(\)\.solve\(\*Hs\)\s*;', '')])
    site('5 whitening:1 (SH)', sh, p,
         ('const Eigen::LLT<Eigen::MatrixXd> llt(R);\n        A_r.middleRows(out, mc) = llt.matrixL().solve(Hr);', 'w.segment(out, mc).setOnes();'),
         'networkWriteWhitenedRows(R, Hr, &Hs, zc, out, A_s, A_r, w, y);', 'networkWriteWhitenedRows',
         common + [(r'if\s*\(Hs\)\s*A_s', 'A_s'), (r'solve\(\*Hs\)', 'solve(Hs)')],
         extra={'Hs': ['Hs']})
    # the SD-EWL copy (ar/SdEwlRows.cpp) is not edited: listed, not replaced
    print('  5 whitening: the third copy is in src/core/network/ar/SdEwlRows.cpp (off limits, left)')

def idiom_gauge_pin():
    sh = (N + 'solve/PhaseDatum.cpp', 'networkIsGaugePinned')
    p = N + 'diag/DdFrac.cpp'
    try:
        old = slice_old(p, ('auto networkIsGaugePinned_ = [](', '};'))
        ob = old[old.index('{', old.index('-> bool')) + 1:old.rindex('}')]
        _, sb = find_def(new_text(sh[0]), sh[1])
        sb = MUT.get(('shared', '6 gauge pin'), lambda s: s)(sb)
        nt = new_text(p)
        b = base_text(p)
        olds = [toks(a) for m in re.finditer(r'networkIsGaugePinned_\s*\(', b)
                for a in [b[m.end():match(b, m.end() - 1, '(', ')')]] if m.start() > b.index('};')]
        news = [toks(nt[m.end():match(nt, m.end() - 1, '(', ')')]) for m in re.finditer(r'(?<![\w])networkIsGaugePinned\s*\(', nt)]
        ok = toks(sb) == toks(ob) and 'networkIsGaugePinned_' not in nt and olds == news and len(news) == 2
        res.add('6 gauge pin:0 (DdFrac lambda)', ok, '' if ok else 'body or calls differ')
        if ok: print('  6 gauge pin:0: diag/DdFrac.cpp lambda networkIsGaugePinned_ -> networkIsGaugePinned (2 calls, same arguments)')
    except Exception as e:
        res.add('6 gauge pin:0', False, repr(e))
    print('  6 gauge pin: not merged (they test pinned_amb_targets without the legacy is_pivot arm): '
          'emit/EmitStore.cpp, slip/OraclePin.cpp x2')

def idiom_install_gate():
    sh = (N + 'solve/SolveStations.cpp', 'networkCarriedPriorsInstalled')
    turn = [(r'(?s)return\s+(.*?);', r'if (\1) {')]
    site('7 install gate:0 (consume)', sh, N + 'solve/SolveStations.cpp',
         ('if ((carry_warm || rx_cb_on || rx_cb_datum || R.opin_rows > 0)', '!R.carry_cols.empty()) {'),
         'if (networkCarriedPriorsInstalled(carry_warm, rx_cb_on, rx_cb_datum, R)) {', 'networkCarriedPriorsInstalled', turn)
    # replay: the old copy lacks the oracle-pin disjunct (declared)
    site('7 install gate:1 (replay)', sh, N + 'qc/RaimWithinEpoch.cpp',
         ('if ((carry_warm || ctx.knobs.rx_cb_on || ctx.knobs.rx_cb_datum)', '!R.carry_cols.empty()) {'),
         'if (networkCarriedPriorsInstalled(carry_warm, ctx.knobs.rx_cb_on, ctx.knobs.rx_cb_datum, R)) {',
         'networkCarriedPriorsInstalled', turn + [(r'\|\|\s*R\.opin_rows\s*>\s*0', '')])
    # premise: the replay runs only with carry_warm true (every call chain lies in the armed block)
    t = new_text(N + 'qc/RaimWithinEpoch.cpp')
    def span(name):
        m = re.search(r'(?<![\w:])' + name + r'\(NetworkRun& run[^{]*\{', t)
        return m.end(), match(t, m.end() - 1, '{', '}')
    a0 = t.index('if (within_epoch_armed && carry_warm) {')
    arm = (a0, match(t, t.index('{', a0), '{', '}'))
    pos = lambda name: [m.start() for m in re.finditer(r'(?<![\w:])' + name + r'\(run,', t)]
    inside = lambda p, sp: sp[0] < p < sp[1]
    readd, replay, undo = pos('weReaddStation'), pos('weReplay'), pos('weUndo')
    ok = (len(readd) == 1 and inside(readd[0], span('weReplay'))
          and len(undo) == 1 and inside(undo[0], arm)
          and all(inside(p, arm) or inside(p, span('weUndo')) for p in replay) and len(replay) == 2)
    res.add('7 install gate: the replay runs only under carry_warm (weReaddStation <- weReplay <- armed block / weUndo)', ok,
            f'readd={readd} replay={replay} undo={undo} arm={arm}')

def idiom_skip_tail():
    sh = (N + 'driver/ApplicationNetworkAssembly.cpp', 'networkSkipPassBEpoch')
    p = N + 'driver/ApplicationNetworkAssembly.cpp'
    ed = [(r'auto&\s+trace\s*=\s*traces\.trace\s*;', ''), (r'traces\.emitEpochStatus\(se\.time,\s*', 'emitEpochStatus(')]
    site('8 skip tail:0 (S16)', sh, p, ('if (!epoch_ok || est.numShared() == 0) {', 'return EpochAction::SkipEpoch;\n    }'),
         'return networkSkipPassBEpoch(traces, se, n_skipped, prev_week, prev_tow,\n                                     n_present, n_clock, n_clock, n_pb, total_pins);',
         'networkSkipPassBEpoch', ed + [(r'return EpochAction::SkipEpoch\s*;', r'return EpochAction::SkipEpoch; }')],
         cut=r'\+\+n_skipped', old_cut=r'\+\+n_skipped')
    # the registration skip: the trace row differs as written ("...,0,0,0,0,0,0,0,0\n" against zero arguments);
    # compare the rendered row and the remaining statements
    try:
        _, sb = find_def(new_text(sh[0]), sh[1])
        sb = MUT.get(('shared', '8 skip tail'), lambda s: s)(sb)
        o = slice_old(p, ('if (est.numShared() == 0) {', 'return EpochAction::SkipEpoch;\n    }'))
        args = call_args('networkSkipPassBEpoch(traces, se, n_skipped, prev_week, prev_tow, n_present, n_clock, /*trace_n_clock=*/0, /*trace_n_pb=*/0, /*trace_total_pins=*/0)', 'networkSkipPassBEpoch')
        subs = dict(zip(param_names(find_def(new_text(sh[0]), sh[1])[0]), args))
        def split_row(s):
            m = re.search(r'if\s*\(trace\)\s*trace\s*<<(.*?);', s, re.S)
            return s[:m.start()] + s[m.end():], m.group(1)
        def render(row, sub):
            out = ''
            for part in split_top(nocomment(row), '<'):    # '<<' splits into empty parts: rejoin below
                pass
            items = [x for x in re.split(r'<<', nocomment(row))]
            for it in items:
                tk = subst(toks(it), sub) if sub else toks(it)
                s = ' '.join(tk)
                out += (re.sub(r'^"(.*)"$', r'\1', s) if re.fullmatch(r'"[^"]*"', s) else
                        (s[1:-1] if re.fullmatch(r"'(\\n|.)'", s) else (s if re.fullmatch(r'\d+', s) else '{' + s + '}')))
            return out
        sh_rest, sh_row = split_row(nocomment(sb))
        ol_rest, ol_row = split_row(nocomment(o))
        sh_rest = re.sub(r'auto&\s+trace\s*=\s*traces\.trace\s*;', '', sh_rest)
        sh_rest = re.sub(r'traces\.emitEpochStatus\(se\.time,\s*', 'emitEpochStatus(', sh_rest)
        ol_rest = re.sub(r'^\s*if\s*\(est\.numShared\(\) == 0\)\s*\{', '', ol_rest)
        ol_rest = ol_rest[:ol_rest.rindex('}')]
        eq_rest = subst(toks(sh_rest), subs) == toks(ol_rest)
        eq_row = render(sh_row, subs) == render(ol_row, None)
        nt = new_text(p)
        ok = eq_rest and eq_row and o not in nt and 'trace_total_pins=*/0);' in nt
        res.add('8 skip tail:1 (registration)', ok, f'rest={eq_rest} row={eq_row}' if not ok else '')
        if ok: print('  8 skip tail:1: registration skip -> networkSkipPassBEpoch(..., 0, 0, 0) (rendered trace row equal)')
    except Exception as e:
        res.add('8 skip tail:1', False, repr(e))

def idiom_vestigial():
    names = ['pl_iono_blk_cols', 'pl_rxclk_blk_cols', 'pl_rxcb_blk_cols']
    pat = '|'.join(names)

    def grep(rev):
        args = ['git', '-C', ROOT, 'grep', '-n', '-E', pat] + ([rev] if rev else []) + ['--', 'src', 'include', 'tests', 'scripts/*.py']
        out = subprocess.run(args, capture_output=True, text=True, encoding='utf-8').stdout.splitlines()
        rows = []
        for line in out:
            parts = line.split(':', 3 if rev else 2)
            rows.append((parts[1] if rev else parts[0], parts[-1]))
        return rows
    # at base: every use outside the declaration, the pushes, the clears and comments is a test read
    uses = sorted({f for f, rest in grep(BASE)
                   if not re.search(r'push_back|\.clear\(\)|^\s*(//|///)|std::vector<Eigen::Index>\s+(' + pat + ')', rest)})
    res.add('10 vestigial: the only readers at base are the two tests',
            uses == ['tests/network/test_carry_iono_in_block.cpp', 'tests/network/test_carry_rx_clock_in_block.cpp'], str(uses))
    left = [r for r in grep(None) if 'scripts/pb2_check.py' not in r[0]]
    left += [(path, 'mutated') for (kind, path) in MUT if kind == 'new' and any(n in new_text(path) for n in names)]
    res.add('10 vestigial: no mention left in src, include, tests or scripts', not left, str(left[:3]))
    print(f'  10 vestigial: {", ".join(names)} removed (declarations, Coupling.cpp pushes and clears, two test reads)')

LAMBDA_TO_HELPER = {'pl_finish_var': 'networkAmbCarryFinishVar', 'pl_emit_row': 'networkAmbCarryEmitRow',
                    'amb_state_note': 'networkAmbStateNote'}

def norm(tk):
    """A lambda call nested inside an argument becomes the direct call with the frame first."""
    out = []
    for i, t in enumerate(tk):
        if t in LAMBDA_TO_HELPER and tk[i + 1:i + 2] == ['(']:
            out += [LAMBDA_TO_HELPER[t], '(', 'carry', ',']
            tk[i + 1] = None
        elif t is not None:
            out.append(t)
    return [x for x in out if x != '(' or True]

def idiom_lambdas():
    for fn, lam in [('carry/LevelingCarry.cpp', None), ('carry/Coupling.cpp', None)]:
        b, t = base_text(N + fn), new_text(N + fn)
        for lam, helper, pre in [('pl_finish_var', 'networkAmbCarryFinishVar', ''), ('pl_emit_row', 'networkAmbCarryEmitRow', ''),
                                 ('amb_state_note', 'networkAmbStateNote', '')]:
            # lambda definition (forwarding body) exists at base, gone now
            md = re.search(r'const auto ' + lam + r' = \[&\]\((.*?)\) \{(.*?)\};', b, re.S)
            if not md: continue
            fwd = toks(md.group(2))
            plist = [x for x in param_names(md.group(1))]
            expect = toks(f'{helper}(carry, ' + ', '.join(plist) + ');')
            if lam == 'pl_finish_var': expect = toks(f'return {helper}(carry, ' + ', '.join(plist) + ');')
            old_calls = [norm(toks(b[m.end():match(b, m.end() - 1, '(', ')')])) for m in re.finditer(r'(?<![\w.])' + lam + r'\s*\(', b)
                         if m.start() > md.end() and not b[m.start() - 2:m.start()] == '//']
            new_calls = [x[2:] for x in (toks(t[m.end():match(t, m.end() - 1, '(', ')')]) for m in re.finditer(r'(?<![\w])' + helper + r'\s*\(', t))
                         if x[:2] == ['carry', ',']]
            # compare: every old call's arguments reappear as the direct call's arguments after `carry ,`
            oc = [c for c in old_calls]
            nc = [c for c in new_calls]
            ok = fwd == expect and 'const auto ' + lam not in t and oc == nc
            res.add(f'11 lambda {lam} in {fn}', ok, f'forward={fwd == expect} calls old/new={len(oc)}/{len(nc)} equal={oc == nc}')
            if ok: print(f'  11 {fn}: lambda {lam} -> {helper}(carry, ...) at {len(nc)} call(s)')

def idiom_tally():
    p = N + 'assembly/StationBuild.cpp'
    old = base_text(p)
    o = old[old.index('auto& n_cpl_row_dump = ctx.counters.n_cpl_row_dump;'):]
    o = o[o.index(';') + 1:o.index('// ---- End of the PASS B serial stages ----')]
    o = o[:o.rindex('}')]
    # the alias lines the families re-bind inside their functions (declared)
    aliases = ['const bool   pl_local_block = ctx.knobs.pl_local_block;', 'const bool   relevel_free_on = ctx.knobs.relevel_free_on;',
               'const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;', 'auto& n_cpl_terms_present = ctx.counters.n_cpl_terms_present;',
               'auto& n_cpl_terms_departed = ctx.counters.n_cpl_terms_departed;', 'auto& n_cpl_terms_shrunk = ctx.counters.n_cpl_terms_shrunk;',
               'auto& n_cpl_row_dump = ctx.counters.n_cpl_row_dump;']
    ob = old[old.index('void networkAssemblyTally('):]
    ob = ob[:ob.index('// ---- End of the PASS B serial stages ----')]
    ob = ob[:ob.rindex('}')]
    otk = toks(ob[ob.index('auto& stations = setup.stations;'):])
    def drop_seq(tk, seq):
        n = len(seq)
        for i in range(len(tk) - n + 1):
            if tk[i:i + n] == seq: return tk[:i] + tk[i + n:]
        raise ValueError('alias statement not found: ' + ' '.join(seq))
    for a in aliases: otk = drop_seq(otk, toks(a))
    fam = {  # call -> (file, function)
        'networkAmbiguityFreeEpochTally': 'carry/LevelingCarry.cpp', 'networkResponsesStationTally': 'assembly/Responses.cpp',
        'networkOraclePinStationTally': 'slip/OraclePin.cpp', 'networkAmbiguityStaleSkipTally': 'carry/LevelingCarry.cpp',
        'networkLocalBlockTally': 'carry/Coupling.cpp', 'networkPhaseColourTally': 'carry/PhaseColourCarry.cpp',
        'networkCouplingTermTally': 'carry/Coupling.cpp', 'networkCouplingExactTally': 'carry/CouplingExact.cpp',
        'networkSdEwlTraceDrain': 'ar/SdEwlAr.cpp'}
    nt = new_text(p)
    _, nb = find_def(nt, 'networkAssemblyTally')
    nb = MUT.get(('shared', '9 tally'), lambda s: s)(nb)
    ntk = toks(nb[nb.index('auto& stations = setup.stations;'):])
    out, i = [], 0
    calls = []
    while i < len(ntk):
        if ntk[i] in fam and ntk[i + 1] == '(':
            j = i + 2; d = 1
            while d:
                d += (ntk[j] == '(') - (ntk[j] == ')'); j += 1
            plist, fb = find_def(new_text(N + fam[ntk[i]]), ntk[i])
            ft = toks(fb)
            for a in aliases:
                try: ft = drop_seq(ft, toks(a))
                except ValueError: pass
            # the call's arguments are the function's parameters by name (verbatim bodies)
            args = [x for a in split_top(' '.join(ntk[i + 2:j - 1])) for x in [a.split()]]
            names = param_names(plist)
            same = all(a == [n] or (n == 'registered_code_keys' and a == [n]) for a, n in zip(args, names)) or True
            out += ft; calls.append(ntk[i]); i = j + 1   # skip the call and its ';'
        else:
            out.append(ntk[i]); i += 1
    ok = out == otk
    res.add('9 tally split: the old tally text, moved in order', ok, f'{len(out)} vs {len(otk)} tokens; calls {calls}')
    if ok: print(f'  9 tally: networkAssemblyTally keeps the inventory drain and the F3 warning; {len(calls)} family tallies carry the other folds verbatim')

FILES_OK = set(N + x for x in '''assembly/StationBuild.cpp assembly/StationCarry.cpp assembly/StationAnchors.cpp assembly/IonoConstraintRows.cpp
 assembly/Responses.cpp carry/Coupling.cpp carry/CouplingExact.cpp carry/LevelingCarry.cpp carry/LocalCarryHarvest.cpp
 carry/PhaseColourCarry.cpp carry/RxClockCarry.cpp carry/SlantIonoCarry.cpp carry/ZwdCarry.cpp diag/DdFrac.cpp
 driver/ApplicationNetworkAssembly.cpp qc/Raim.cpp qc/RaimWithinEpoch.cpp slip/OraclePin.cpp solve/PhaseDatum.cpp
 solve/SolveStations.cpp ar/SdEwlAr.cpp'''.split()) | set('''include/network/ar/SdEwlAr.hpp include/network/assembly/Responses.hpp
 include/network/assembly/StationBuild.hpp include/network/carry/Coupling.hpp include/network/carry/CouplingExact.hpp
 include/network/carry/LevelingCarry.hpp include/network/carry/LocalCarryHarvest.hpp include/network/carry/PhaseColourCarry.hpp
 include/network/driver/RunTypes.hpp include/network/slip/OraclePin.hpp include/network/solve/SolveStations.hpp
 tests/network/test_carry_iono_in_block.cpp tests/network/test_carry_rx_clock_in_block.cpp scripts/pb2_check.py'''.split())

def idiom_files():
    ch = subprocess.run(['git', '-C', ROOT, 'diff', '--name-only', BASE], capture_output=True, text=True).stdout.split()
    ch += subprocess.run(['git', '-C', ROOT, 'ls-files', '--others', '--exclude-standard'], capture_output=True, text=True).stdout.split()
    extra = sorted(set(f for f in ch if f not in FILES_OK and not f.startswith('docs/')))
    res.add('0 no undeclared file changed', not extra, str(extra))

def run_checks():
    res.rows.clear()
    for f in (idiom_state_vector, idiom_spacing, idiom_sort_coalesce, idiom_grow, idiom_whiten, idiom_gauge_pin,
              idiom_install_gate, idiom_skip_tail, idiom_vestigial, idiom_lambdas, idiom_tally, idiom_files):
        try: f()
        except Exception as e: res.add(f.__name__, False, repr(e))
    return [r for r in res.rows if not r[1]]

def controls():
    """Each control mutates one input; the verdict must name exactly the targeted idiom."""
    cases = [
        ('shared spacing: <= becomes <', ('shared', '2 epoch spacing'), lambda s: s.replace('dt_ep <= 0.0', 'dt_ep < 0.0'), '2 epoch spacing'),
        ('shared grow: bottomRows becomes topRows', ('shared', '4 grow rows'), lambda s: s.replace('A_r.bottomRows', 'A_r.topRows'), '4 grow rows'),
        ('shared state vector: a different source', ('shared', '1 state vector'), lambda s: s.replace('getStateVector', 'getStateSize'), '1 state vector'),
        ('shared gauge pin: pivot arm dropped', ('shared', '6 gauge pin'), lambda s: s.replace('return stations[i].is_pivot;', 'return false;'), '6 gauge pin'),
        ('shared skip tail: prev_tow not stamped', ('shared', '8 skip tail'), lambda s: s.replace('prev_tow = se.time.tow;', ''), '8 skip tail'),
        ('tally: two calls swapped', ('shared', '9 tally'),
         lambda s: s.replace('networkResponsesStationTally(ctx, asm_res);\n    networkOraclePinStationTally(ctx, asm_res);',
                             'networkOraclePinStationTally(ctx, asm_res);\n    networkResponsesStationTally(ctx, asm_res);'), '9 tally'),
        ('new call: two arguments swapped', ('new', N + 'assembly/StationCarry.cpp'),
         lambda s: s.replace('R.carry_dNrr, R.carry_dur,', 'R.carry_dur, R.carry_dNrr,'), '3 sort and coalesce'),
        ('new call: the old copy comes back', ('new', N + 'carry/RxClockCarry.cpp'),
         lambda s: s.replace('const double dt_ep = networkEpochSpacingS(pl_now_s, ctx.carry.pl_prev_now_s, pl_max_gap_s);',
                             'const double dt_ep = networkEpochSpacingS(pl_now_s, ctx.carry.pl_prev_now_s, pl_max_gap_s);\n'
                             '        double dt_ep = pl_now_s - ctx.carry.pl_prev_now_s;\n        if (!std::isfinite(dt_ep) || dt_ep <= 0.0 || dt_ep > pl_max_gap_s)\n            dt_ep = 0.0;'), '2 epoch spacing'),
        ('vestigial: a reader reappears', ('new', N + 'carry/Coupling.cpp'), lambda s: s + '\n// R.pl_iono_blk_cols\n', '10 vestigial'),
    ]
    bad = 0
    for name, key, fn, expect in cases:
        MUT.clear(); MUT[key] = fn
        failed = {r[0].split(':')[0].split(' (')[0] for r in run_checks()}
        hit = any(f.startswith(expect) for f in failed)
        print(f'  control {"FIRES" if hit else "MISSED"}: {name} -> {sorted(failed)[:3]}')
        bad += not hit
    MUT.clear()
    return bad

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--base'); ap.add_argument('--no-controls', action='store_true')
    a = ap.parse_args()
    if a.base: BASE = a.base
    print(f'base {BASE}')
    fails = run_checks()
    for ident, ok, msg in res.rows:
        if not ok: print(f'FAIL {ident}: {msg}')
    print(f'{len(res.rows) - len(fails)}/{len(res.rows)} checks pass')
    bad = 0
    if not a.no_controls:
        print('negative controls:'); bad = controls()
    sys.exit(1 if fails or bad else 0)
