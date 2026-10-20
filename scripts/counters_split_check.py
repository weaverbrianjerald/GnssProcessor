#!/usr/bin/env python3
"""Prove the NetworkServiceCounters split lost and duplicated nothing.

The run-scope counters of the network producer were one struct in
include/network/state/RunContext.hpp.  They are now a set of per-owner aggregate
structs (each declared in the header of the subsystem that writes it) that
NetworkServiceCounters inherits.  This script checks, against the struct as it stood
in a base revision (default: the commit before the split):

  1. the multiset of (name, type, initializer) over the old struct's data members
     equals the multiset over the union of the new base structs;
  2. no member name is declared in two bases (or twice in one base);
  3. every nested type the old struct declared is declared exactly once in the new bases;
  4. the new NetworkServiceCounters adds no data members of its own;
  5. each base keeps the relative order the old struct gave its members.

The parser here is also imported by scripts/counters_split_cut.py, which performs the cut.

Usage: python scripts/counters_split_check.py [--base-rev REV]
Exit status 0 when every check passes.
"""
import argparse
import os
import re
import subprocess
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_CTX = 'include/network/state/RunContext.hpp'
BASE_REV_DEFAULT = '1549ed3'
STRUCT = 'NetworkServiceCounters'


# --------------------------------------------------------------------------- scanning

class Item:
    """One statement of a struct body with the text around it.

    text  raw source from the end of the previous item through the end of this
          statement's last line (leading comments and the trailing comment included),
          so the concatenation of all items plus the tail is the body verbatim.
    code  the statement with comments removed and whitespace collapsed.
    """
    __slots__ = ('text', 'code', 'kind', 'names', 'members', 'type_name')

    def __init__(self, text, code):
        self.text, self.code = text, code
        self.kind = None          # 'member' | 'type' | 'other'
        self.names = []
        self.members = []         # (name, type, init) per declarator
        self.type_name = None


def _skip_trailing(body, k):
    """Index just past the comments and the newline that close the line holding body[k-1]
    when nothing but comments follows on that line; else k."""
    i, n = k, len(body)
    while True:
        while i < n and body[i] in ' \t':
            i += 1
        if body.startswith('//', i):
            j = body.find('\n', i)
            return n if j < 0 else j + 1
        if body.startswith('/*', i):
            j = body.find('*/', i + 2)
            if j < 0:
                return n
            i = j + 2
            continue
        if i < n and body[i] == '\r':
            i += 1
        if i < n and body[i] == '\n':
            return i + 1
        if i >= n:
            return n
        return k


def scan_body(body):
    """Split a struct body (text between the braces) into Items plus a tail of trailing text."""
    items, n = [], len(body)
    i, start = 0, 0
    code = []
    bd = pd = 0
    while i < n:
        c = body[i]
        two = body[i:i + 2]
        if two == '//':
            j = body.find('\n', i)
            i = n if j < 0 else j
            code.append(' ')
            continue
        if two == '/*':
            j = body.find('*/', i + 2)
            i = n if j < 0 else j + 2
            code.append(' ')
            continue
        if c in '"\'':
            j = i + 1
            while j < n and body[j] != c:
                j += 2 if body[j] == '\\' else 1
            code.append(body[i:j + 1])
            i = j + 1
            continue
        if c == '{':
            bd += 1
        elif c == '}':
            bd -= 1
        elif c == '(':
            pd += 1
        elif c == ')':
            pd -= 1
        code.append(c)
        i += 1
        if c == ';' and bd == 0 and pd == 0:
            end = _skip_trailing(body, i)
            items.append(Item(body[start:end], re.sub(r'\s+', ' ', ''.join(code)).strip()))
            start, code = end, []
            i = end
        elif c == '}' and bd == 0 and pd == 0:
            head = ''.join(code).split('{')[0]
            if (re.search(r'\)\s*(const|noexcept|override|\s)*$', head) and '=' not in head
                    and not head.strip().startswith(('struct', 'enum'))):
                end = _skip_trailing(body, i)       # an inline member function body closed
                items.append(Item(body[start:end], re.sub(r'\s+', ' ', ''.join(code)).strip()))
                start, code = end, []
                i = end
    return items, body[start:]


def split_top(s, sep=','):
    """Split on sep at bracket depth 0; angle brackets count only before an initializer."""
    out, cur, d, ang, init = [], '', 0, 0, False
    for ch in s:
        if ch in '({[':
            d += 1
            if ch in '{[':
                init = True
        elif ch in ')}]':
            d -= 1
        elif ch == '=' and d == 0:
            init = True
        elif ch == '<' and d == 0 and not init:
            ang += 1
        elif ch == '>' and d == 0 and not init and ang:
            ang -= 1
        if ch == sep and d == 0 and ang == 0:
            out.append(cur)
            cur, init = '', False
        else:
            cur += ch
    out.append(cur)
    return out


def classify(it):
    code = it.code
    m = re.match(r'(struct|class|enum(?:\s+class)?|union)\s+(\w+)\b', code)
    if m:
        tail = code[code.rfind('}') + 1:].strip(' ;') if '}' in code else ''
        it.type_name = m.group(2)
        if tail:                       # a nested type declared together with its member
            it.kind = 'member'
            it.names = [tail]
            it.members = [(tail, 'struct ' + m.group(2), code[code.find('{'):code.rfind('}') + 1])]
            return
        it.kind = 'type'
        return
    if re.match(r'(using|typedef|static|friend|template)\b', code) or code == ';':
        it.kind = 'other'
        return
    pieces = split_top(code[:-1] if code.endswith(';') else code)
    first_head = re.split(r'=|\{|\[', pieces[0], maxsplit=1)[0].strip()
    mname = re.search(r'(\w+)\s*$', first_head)
    if not mname or '(' in first_head:
        it.kind = 'other'
        return
    base_type = re.sub(r'\s+', ' ', first_head[:mname.start()]).strip()
    for k, piece in enumerate(pieces):
        head = re.split(r'=|\{|\[', piece, maxsplit=1)[0].strip()
        mn = re.search(r'(\w+)\s*$', head)
        if not mn:
            it.kind = 'other'
            return
        init = re.sub(r'\s+', ' ', piece.strip()[len(head):]).strip()
        it.members.append((mn.group(1), base_type, init))
        it.names.append(mn.group(1))
    it.kind = 'member'


def brace_span(text, open_idx):
    """Index of the brace closing the one at open_idx (comments and strings skipped)."""
    d, i, n = 0, open_idx, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if two == '/*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j + 1
            continue
        if c == '{':
            d += 1
        elif c == '}':
            d -= 1
            if d == 0:
                return i
        i += 1
    raise ValueError('unbalanced braces')


def find_struct(text, name):
    """(body_start, body_end) of 'struct name ... {' in text; body excludes the braces."""
    m = re.search(r'\bstruct\s+' + re.escape(name) + r'\b[^;{]*\{', text)
    if not m:
        return None
    o = m.end() - 1
    return o + 1, brace_span(text, o)


def parse_struct(text, name):
    """(items, tail, bases) for struct name in text, or None."""
    sp = find_struct(text, name)
    if not sp:
        return None
    items, tail = scan_body(text[sp[0]:sp[1]])
    for it in items:
        classify(it)
    hdr = re.search(r'\bstruct\s+' + re.escape(name) + r'\b([^;{]*)\{', text).group(1)
    bases = []
    if ':' in hdr:
        for b in hdr.split(':', 1)[1].split(','):
            b = re.sub(r'\b(public|protected|private)\b', '', b).strip()
            if b:
                bases.append(b)
    return items, tail, bases


def git_show(rev, path):
    return subprocess.run(['git', '-C', ROOT, 'show', f'{rev}:{path}'], capture_output=True,
                          check=True).stdout.decode('utf-8').replace('\r\n', '\n')


def read_tree(path):
    with open(os.path.join(ROOT, path), 'rb') as f:
        return f.read().decode('utf-8').replace('\r\n', '\n')


def header_paths():
    out = []
    for d, _, fs in os.walk(os.path.join(ROOT, 'include')):
        for f in fs:
            if f.endswith('.hpp'):
                out.append(os.path.relpath(os.path.join(d, f), ROOT).replace('\\', '/'))
    return out


# --------------------------------------------------------------------------- checks

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base-rev', default=BASE_REV_DEFAULT)
    ap.add_argument('--strict', action='store_true', help='fail on members added after the base revision')
    a = ap.parse_args()
    fails, notes = [], []

    old_items, _, _ = parse_struct(git_show(a.base_rev, RUN_CTX), STRUCT)
    new_text = read_tree(RUN_CTX)
    new_items, _, bases = parse_struct(new_text, STRUCT)
    if not bases:
        print('FAIL: NetworkServiceCounters has no bases')
        return 1
    if any(it.kind == 'member' for it in new_items):
        fails.append('NetworkServiceCounters still declares data members: '
                     + ', '.join(n for it in new_items for n in it.names))

    # locate each base struct in the headers
    headers = {p: read_tree(p) for p in header_paths()}
    base_items, home = {}, {}
    for b in bases:
        hits = [(p, parse_struct(t, b)) for p, t in headers.items() if re.search(r'\bstruct\s+' + b + r'\s*\{', t)]
        if len(hits) != 1:
            fails.append(f'base {b}: declared in {len(hits)} headers (want 1)')
            continue
        home[b], base_items[b] = hits[0][0], hits[0][1][0]

    old_m = Counter(m for it in old_items if it.kind == 'member' for m in it.members)
    new_m = Counter(m for b in bases for it in base_items.get(b, []) if it.kind == 'member' for m in it.members)
    for k, v in (old_m - new_m).items():
        fails.append(f'missing from the bases: {k} x{v}')
    # Members added after the base are new counters, not a defect of the split; --strict (the proof at the
    # split commit itself) still fails on them.
    added = sorted((new_m - old_m).items())
    for k, v in added:
        (fails if a.strict else notes).append(f'not in the old struct: {k} x{v}')

    names = Counter(n for b in bases for it in base_items.get(b, []) if it.kind == 'member' for n in it.names)
    for n, v in names.items():
        if v > 1:
            where = [b for b in bases for it in base_items.get(b, []) if n in it.names]
            fails.append(f'member {n} declared {v} times: {where}')

    old_t = Counter(it.type_name for it in old_items if it.kind == 'type')
    new_t = Counter(it.type_name for b in bases for it in base_items.get(b, []) if it.kind == 'type')
    if old_t != new_t:
        fails.append(f'nested types differ: old {dict(old_t)} new {dict(new_t)}')
    old_o = Counter(it.code for it in old_items if it.kind == 'other')
    new_o = Counter(it.code for b in bases for it in base_items.get(b, []) if it.kind == 'other')
    if old_o != new_o:
        fails.append(f'non-member statements differ: {dict(old_o - new_o)} / {dict(new_o - old_o)}')

    # relative order inside each base
    pos = {n: k for k, n in enumerate(n for it in old_items if it.kind == 'member' for n in it.names)}
    for b in bases:
        seq = [pos[n] for it in base_items.get(b, []) if it.kind == 'member' for n in it.names if n in pos]
        if seq != sorted(seq):
            fails.append(f'base {b}: members out of the old relative order')

    n_old = sum(len(it.members) for it in old_items if it.kind == 'member')
    print(f'old struct: {n_old} members, {sum(old_t.values())} nested types, {len(bases)} bases')
    for b in bases:
        cnt = sum(len(it.members) for it in base_items.get(b, []) if it.kind == 'member')
        print(f'  {b:44s} {cnt:4d}  {home.get(b, "?")}')
    for n in notes:
        print('  added since the base: ' + n.split(': ', 1)[1])
    if fails:
        print('FAIL')
        for f in fails:
            print('  ' + f)
        return 1
    print(f'PASS: every member of the base struct kept ({sum(old_m.values())}; {len(notes)} added since), '
          f'no duplicate names, nested types and order preserved')
    return 0


if __name__ == '__main__':
    sys.exit(main())
