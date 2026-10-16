#!/usr/bin/env python3
"""Cut NetworkServiceCounters into per-owner base structs (one-shot generator).

Reads include/network/state/RunContext.hpp, assigns every run-scope counter to the
subsystem that writes it, moves the member text verbatim into an aggregate struct declared
in that subsystem's header (or in a light <Owner>Counters.hpp beside it when the owner
header includes RunContext.hpp, directly or not), and rewrites NetworkServiceCounters as a
struct that inherits all of them.  Every `ctx.counters.x` access compiles unchanged.

Owner rule, in order:
  1. a member whose type is a project struct (not a container of one) goes to the header
     that declares that type;
  2. otherwise to the header of the source file that writes it most (test files and
     run-end readers do not count while another file writes it);
  3. writers without a header of their own (the state lifecycle files) get a light header
     named after the file; writers in the Application drivers, members nothing writes and
     groups that would need a new one-or-two-member header go to GeneralCounters, kept in
     RunContext.hpp.

Usage:
  python scripts/counters_split_cut.py --plan    print the assignment, write nothing
  python scripts/counters_split_cut.py --apply   edit the headers
Run on a clean checkout of the base revision; scripts/counters_split_check.py then proves
the result.  Idempotence: reset include/ first (git checkout -- include; remove the
generated *Counters.hpp files).
"""
import argparse
import os
import re
import sys
from collections import defaultdict, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import counters_split_check as K

ROOT = K.ROOT
RUN_CTX = K.RUN_CTX
PRIM = re.compile(r'(unsigned )?(long long|long|int|double|bool|size_t|std::size_t|uint32_t|float|std::string)$')
INCL = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
GENERAL = 'GENERAL'
MIN_NEW_LIGHT = 3          # a group smaller than this does not earn a new header
STATE_OWNERS = {           # state lifecycle files declare their functions in RunContext.hpp itself
    'src/core/network/state/WarmResume.cpp': 'include/network/state/WarmResumeCounters.hpp',
    'src/core/network/state/BorderPrior.cpp': 'include/network/state/BorderCounters.hpp',
    'src/core/network/state/BorderStash.cpp': 'include/network/state/BorderCounters.hpp',
    'src/core/network/state/StateSnapshotWrite.cpp': 'include/network/state/StateSnapshotCounters.hpp',
}
STD_HDR = [(r'\bstd::map\b', '<map>'), (r'\bstd::set\b', '<set>'), (r'\bstd::vector\b', '<vector>'),
           (r'\bstd::string\b', '<string>'), (r'\bstd::array\b', '<array>'),
           (r'\b(?:std::)?(?:size_t|ptrdiff_t)\b', '<cstddef>'), (r'\buint32_t\b', '<cstdint>'),
           (r'\bstd::(?:pair|tuple)\b', '<utility>'), (r'\bstd::(?:numeric_limits)\b', '<limits>')]


def eol_of(raw):
    return '\r\n' if raw.count(b'\r\n') * 2 > raw.count(b'\n') else '\n'


def read(path):
    with open(os.path.join(ROOT, path), 'rb') as f:
        raw = f.read()
    return raw.decode('utf-8').replace('\r\n', '\n'), eol_of(raw)


def write(path, text, eol):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, 'wb') as f:
        f.write(text.replace('\n', eol).encode('utf-8'))


def strip_cm(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


# ------------------------------------------------------------------ include graph

class Graph:
    def __init__(self):
        self.text = {p: read(p)[0] for p in K.header_paths()}
        self.inc = {p: [i for i in INCL.findall(strip_cm(t)) if 'include/' + i in self.text]
                    for p, t in self.text.items()}
        self.memo = {}
        self.decl = defaultdict(set)       # identifier -> headers that define it
        pats = [r'\b(?:struct|class|enum(?:\s+class|\s+struct)?)\s+(\w+)\b[^;{(=]*\{',
                r'\busing\s+(\w+)\s*=',
                r'\bconstexpr\s+[\w:<>,\s\*&]*?\b(\w+)\s*(?:=|\{|\[)']
        for p, t in self.text.items():
            c = strip_cm(t)
            for pt in pats:
                for m in re.finditer(pt, c):
                    self.decl[m.group(1)].add(p)

    def closure(self, p):
        if p in self.memo:
            return self.memo[p]
        self.memo[p] = set()                        # break include cycles
        out = set()
        for i in self.inc.get(p, []):
            q = 'include/' + i
            out.add(q)
            out |= self.closure(q)
        self.memo[p] = out
        return out


# ------------------------------------------------------------------ usage

def usage(names, skip):
    """name -> file -> [occurrences, writes], member-access occurrences only (x.name, p->name)."""
    W = r'(?:\+\+|--|(?:[-+*/|&^%]|<<|>>)?=(?!=)|\.(?:push_back|emplace_back|emplace|insert|clear|erase|assign|resize|fill|swap)\b)'
    wre = re.compile(r'\s*(\[[^\]]*\]\s*)*' + W)
    tok = re.compile(r'\b[A-Za-z_]\w*\b')
    use = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    for d in ('src', 'include', 'tests'):
        for dp, _, fs in os.walk(os.path.join(ROOT, d)):
            for f in fs:
                if not f.endswith(('.cpp', '.hpp', '.h', '.inl')):
                    continue
                rel = os.path.relpath(os.path.join(dp, f), ROOT).replace(os.sep, '/')
                if rel in skip:
                    continue
                txt = open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace').read()
                for m in tok.finditer(txt):
                    n = m.group(0)
                    if n not in names:
                        continue
                    if not re.search(r'(\.|->)\s*$', txt[max(0, m.start() - 6):m.start()]):
                        continue
                    ls = txt.rfind('\n', 0, m.start()) + 1
                    if '//' in txt[ls:m.start()] or txt[ls:m.start()].lstrip().startswith(('*', '/*')):
                        continue
                    use[n][rel][0] += 1
                    rest = txt[m.end():m.end() + 80]
                    if wre.match(rest) or re.search(r'(\+\+|--)\s*[\w.\->]*$', txt[max(0, m.start() - 40):m.start()]) \
                            or (not names[n] and re.match(r'\s*[.\[]', rest)):
                        use[n][rel][1] += 1
    return use


def is_runend(f):
    return os.path.basename(f).startswith(('Teardown', 'RunEnd')) or 'RunEnd' in os.path.basename(f)


def top_writer(use_n):
    cand = {f: v for f, v in use_n.items() if not f.startswith('tests/')}
    net = {f: v for f, v in cand.items() if '/network/' in f}
    cand = net or cand
    if not cand:
        return None
    pool = {f: v for f, v in cand.items() if not is_runend(f)}
    if not any(v[1] for v in pool.values()):
        pool = {f: v for f, v in cand.items() if v[1]} or pool or cand
    return max(pool, key=lambda f: (pool[f][1], pool[f][0], f))


# ------------------------------------------------------------------ plan

def build_plan():
    text, _ = read(RUN_CTX)
    parsed = K.parse_struct(text, K.STRUCT)
    items, tail, bases = parsed
    if bases or not any(it.kind == 'member' for it in items):
        sys.exit('NetworkServiceCounters is already cut (it has bases or no members)')
    g = Graph()
    names = {}                                   # name -> is a primitive/library member
    for it in items:
        for (n, ty, init) in it.members:
            names[n] = bool(PRIM.match(ty))
    local_types = {it.type_name for it in items if it.type_name}
    use = usage(names, {RUN_CTX, 'src/core/network/state/RunFields.cpp'})

    def home_of_file(f):
        if f in STATE_OWNERS:
            return STATE_OWNERS[f], True
        if f.endswith('.hpp') and f.startswith('include/'):
            return f, False
        if f.startswith('src/core/app/') or f.startswith('src/core/config/') \
                or os.path.basename(f).startswith('ApplicationNetwork'):
            return GENERAL, False
        b = os.path.splitext(os.path.basename(f))[0]
        d = os.path.dirname(f)
        cands = []
        if d.startswith('src/core/network/'):
            cands.append('include/network/%s/%s.hpp' % (d[len('src/core/network/'):], b))
        cands += sorted(p for p in g.text if os.path.splitext(os.path.basename(p))[0] == b)
        for c in cands:
            if c in g.text:
                return c, False
        return GENERAL, False

    owner_of = {}                                 # item index -> owner key
    by_type = {'SecondCode': 'include/network/assembly/SecondCodeScope.hpp',      # nested types live
               'SecondPhase': 'include/network/assembly/SecondPhaseScope.hpp'}    # with their scope
    why = {}
    for k, it in enumerate(items):
        if it.kind != 'member':
            continue
        first = it.members[0]
        ty = first[1]
        if ty in by_type:
            owner_of[k], why[k] = by_type[ty], 'type'
            continue
        if not PRIM.match(ty) and not ty.startswith(('std::', 'struct ')) and ty not in local_types:
            hs = [h for h in g.decl.get(ty.split('::')[0], ()) if h.startswith('include/network/')]
            if len(hs) == 1:
                owner_of[k], why[k] = hs[0], 'type'
                continue
        votes = defaultdict(int)
        for n in it.names:
            f = top_writer(use.get(n, {}))
            if f:
                votes[home_of_file(f)[0]] += 1
        if votes:
            owner_of[k] = max(votes, key=lambda h: (votes[h], h))
            why[k] = 'writer'
        else:
            owner_of[k], why[k] = None, 'unwritten'
    # unwritten members follow the previous member with a two-token name prefix in common
    last_by_prefix = {}
    for k, it in enumerate(items):
        if it.kind != 'member':
            continue
        pre = '_'.join(re.sub(r'^n_', '', it.names[0]).split('_')[:2])
        if owner_of[k] is None:
            owner_of[k] = last_by_prefix.get(pre) or GENERAL
            why[k] = 'prefix' if last_by_prefix.get(pre) else 'general'
        elif owner_of[k] != GENERAL:
            last_by_prefix[pre] = owner_of[k]
    # nested type declarations travel with their first user
    for k, it in enumerate(items):
        if it.kind == 'type':
            users = [j for j, o in enumerate(items) if o.kind == 'member' and j > k
                     and re.search(r'\b' + it.type_name + r'\b', ' '.join(m[1] for m in o.members))]
            if not users:
                sys.exit('nested type %s has no user' % it.type_name)
            owner_of[k], why[k] = owner_of[users[0]], 'type-user'
    # run-state owners that need a header of their own
    groups = OrderedDict()
    for k, it in enumerate(items):
        if k in owner_of:
            groups.setdefault(owner_of[k], []).append(k)
    return text, items, g, groups, why, use, home_of_file


def decide_homes(g, items, groups):
    """owner key -> dict(struct, home, light, includes)."""
    taken = set()
    for p, t in g.text.items():
        for m in re.finditer(r'\b(?:struct|class|enum|union)\s+(\w+)|\busing\s+(\w+)\s*=', strip_cm(t)):
            taken.add(m.group(1) or m.group(2))
    plan = OrderedDict()
    for key, ks in groups.items():
        if key == GENERAL:
            plan[key] = dict(struct='GeneralCounters', home=RUN_CTX, light=False, hdr=RUN_CTX, ks=ks, inc=[])
            continue
        stem = os.path.splitext(os.path.basename(key))[0]
        stem = re.sub(r'Counters$', '', stem)
        plan[key] = dict(struct=stem + 'Counters', hdr=key, ks=ks, inc=[], light=False, home=key)
        if key in g.text:
            circular = RUN_CTX in g.closure(key) or key == RUN_CTX
            if circular:
                lp = os.path.join(os.path.dirname(key), stem + 'Counters.hpp').replace(os.sep, '/')
                plan[key].update(light=True, home=lp)
        else:                                       # state lifecycle owner: light header only
            plan[key].update(light=True, home=key, hdr=None)
    for key, p in plan.items():
        if p['struct'] in taken:
            p['struct'] = p['struct'][:-len('Counters')] + 'RunCounters'
    return plan


def required_headers(g, items, p):
    local = {items[k].type_name for k in p['ks'] if items[k].type_name}
    need = set()
    for it_k in p['ks']:
        it = items[it_k]
        blob = (it.code if it.kind == 'type' else ' '.join(m[1] + ' ' + m[2] for m in it.members))
        for ident in re.findall(r'\b[A-Za-z_]\w*\b', blob):
            if ident in local or ident == 'std':
                continue
            hs = g.decl.get(ident)
            if hs:
                need |= hs
    return need


# ------------------------------------------------------------------ emit

def dedent(txt, n):
    if n <= 0:
        return txt
    return '\n'.join(l[n:] if l.startswith(' ' * n) else l for l in txt.split('\n'))


def struct_text(p, items, base_indent, member_shift):
    body = ''.join(items[k].text for k in p['ks'])
    body = dedent(body, member_shift) if member_shift > 0 else body
    ind = ' ' * base_indent
    return ('%s/// @brief %s\n%sstruct %s {\n%s%s};\n'
            % (ind, p['brief'], ind, p['struct'], body if body.endswith('\n') else body + '\n', ind))


def insertion_point(text):
    """Offset of the line closing the top-level namespace and the indent of its structs."""
    m = None
    for m in re.finditer(r'^\}[ \t]*(?://[^\n]*)?[ \t]*$', text, re.M):
        pass
    if m is None:
        return None
    pre = strip_cm(text[:m.start()])
    if pre.count('{') - pre.count('}') != 1:
        return None
    return m.start()


def std_needed(items, ks):
    blob = ' '.join(m[1] + ' ' + m[2] for k in ks for m in items[k].members) \
        + ' ' + ' '.join(items[k].code for k in ks if items[k].kind == 'type')
    return [h for rx, h in STD_HDR if re.search(rx, blob)]


def light_text(p, items):
    body = struct_text(p, items, 0, 4)
    inc = ['#include "%s"' % h[len('include/'):] for h in p['inc']] \
        + ['#include %s' % h for h in std_needed(items, p['ks'])]
    why_apart = ('Declared apart from the owner header, which includes RunContext.hpp.' if p['hdr']
                 else 'The writing file declares its functions in RunContext.hpp, so the counters have a header of their own.')
    return '\n'.join(['#pragma once', '/**', ' * @file ' + os.path.basename(p['home']), ' * @brief ' + p['brief'],
                      ' *', ' * ' + why_apart, ' */', ''] + inc
                     + ['', 'namespace GnssProcessor {', '', body, '} // namespace GnssProcessor', ''])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--plan', action='store_true')
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    if a.plan == a.apply:
        ap.error('give exactly one of --plan, --apply')
    text, items, g, groups, why, use, home_of_file = build_plan()
    plan = decide_homes(g, items, groups)

    # a small group that would need a new header goes to GeneralCounters
    for key in list(plan):
        p = plan[key]
        members = sum(len(items[k].members) for k in p['ks'])
        if key != GENERAL and p['light'] and not os.path.exists(os.path.join(ROOT, p['home'])) \
                and members < MIN_NEW_LIGHT:
            plan.setdefault(GENERAL, dict(struct='GeneralCounters', home=RUN_CTX, light=False,
                                          hdr=RUN_CTX, ks=[], inc=[]))
            plan[GENERAL]['ks'] = sorted(plan[GENERAL]['ks'] + p['ks'])
            del plan[key]

    # briefs and the includes each struct needs beyond its header's own
    for key, p in plan.items():
        stem = re.sub(r'(Run)?Counters$', '', p['struct'])
        p['brief'] = ('Run-scope counters of %s, a base of NetworkServiceCounters (network/state/RunContext.hpp).'
                      % ('the Application drivers' if key == GENERAL else stem))
        if key == GENERAL:
            continue
        need = required_headers(g, items, p) - {p['home'], p['hdr'], RUN_CTX}
        have = (g.closure(p['hdr']) | {p['hdr']}) if p['hdr'] in g.text else set()
        extra = sorted(h for h in need if h not in have)
        for h in extra:
            if p['home'] in g.closure(h) or RUN_CTX in g.closure(h):
                if p['light']:
                    sys.exit('include cycle for %s needing %s' % (p['struct'], h))
                p.update(light=True, home=os.path.join(os.path.dirname(key), p['struct'] + '.hpp').replace(os.sep, '/'))
        p['inc'] = sorted(set(extra) | (need if p['light'] else set()))

    if a.plan:
        for key, p in plan.items():
            n = sum(len(items[k].members) for k in p['ks'] if items[k].kind == 'member')
            print('%-32s %4d  %s%s' % (p['struct'], n, p['home'], '  [light]' if p['light'] and p['home'] != RUN_CTX else ''))
            for h in p['inc']:
                print('      + include', h)
        print(len(plan), 'bases')
        return 0

    ctx_text, ctx_eol = read(RUN_CTX)
    for key, p in plan.items():
        if key == GENERAL:
            continue
        if p['light']:
            write(p['home'], light_text(p, items), ctx_eol)
            continue
        t, eol = read(key)
        pos = insertion_point(t)
        if pos is None:
            sys.exit('%s: cannot find the namespace close' % key)
        m = re.search(r'^([ \t]*)(?:struct|class)\s+\w+', strip_cm(t), re.M)
        indent = len(m.group(1)) if m else 0
        body = struct_text(p, items, indent, 4 if indent == 0 else 0)
        add = ['#include "%s"\n' % h[len('include/'):] for h in p['inc']] \
            + ['#include %s\n' % h for h in std_needed(items, p['ks']) if ('#include ' + h) not in t]
        if add:
            incs = list(re.finditer(r'^[ \t]*#[ \t]*include[^\n]*\n', t, re.M))
            at = incs[-1].end() if incs else t.index('#pragma once') + len('#pragma once\n')
            t = t[:at] + ''.join(add) + t[at:]
            pos += sum(len(x) for x in add)
        write(key, t[:pos] + body + '\n' + t[pos:], eol)

    # RunContext.hpp: GeneralCounters, the banner, the inheriting struct and the includes
    t = ctx_text
    start = re.search(r'    /// @internal Service counters[^\n]*\n(?:    ///[^\n]*\n)*    struct NetworkServiceCounters \{', t)
    end = t.index('    /// @internal The run context: one instance per runNetwork()')
    ordered = sorted(plan, key=lambda k: min(plan[k]['ks']))
    gen = struct_text(plan[GENERAL], items, 4, 0) + '\n' if GENERAL in plan else ''
    rows = ['    ///   %-34s %s' % (plan[k]['struct'], plan[k]['home'][len('include/'):]) for k in ordered]
    inh = ',\n'.join(('          %s' if i else '        : %s') % plan[k]['struct'] for i, k in enumerate(ordered))
    new = (gen +
           '    /// @internal Service counters, health-window anchors and B8 flag-rate tallies,\n'
           '    /// relocated from the run loop.\n'
           '    ///\n'
           '    /// The members are declared by owner: each base is an aggregate in the header of the\n'
           '    /// subsystem that writes its members (in a light Counters header beside it where the\n'
           '    /// owner header includes this file), so `ctx.counters.x` is spelled as it always was.\n'
           '    /// A new run-scope counter goes to the counter struct of the subsystem that writes it;\n'
           '    /// a new struct is one more base below.  Bases and the header declaring each:\n'
           + '\n'.join(rows) + '\n'
           '    struct NetworkServiceCounters\n' + inh + ' {};\n\n')
    t = t[:start.start()] + new + t[end:]
    rows = []
    for k in ordered:
        p = plan[k]
        line = '#include "%s"' % p['home'][len('include/'):]
        if p['home'] != RUN_CTX and line not in t:
            rows.append('%-60s // %s\n' % (line, p['struct']))
    last = list(re.finditer(r'^#include "[^\n]*\n', t, re.M))[-1]
    write(RUN_CTX, t[:last.end()] + ''.join(rows) + t[last.end():], ctx_eol)
    print('applied: %d bases' % len(plan))
    return 0


if __name__ == '__main__':
    sys.exit(main())
