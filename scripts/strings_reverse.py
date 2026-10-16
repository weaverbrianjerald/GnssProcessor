#!/usr/bin/env python3
"""Reverse a declared output-text move on run output (card-name strings rewritten in plain text).

A text move edits string literals in src/ and include/ without touching behaviour.  Each changed
literal is split at its format placeholders ({} {:.3f} %d ...) and at newlines into fragments; a
fragment whose text changed is one row of a reverse map: NEW fragment <TAB> OLD fragment.  Applying
the map to a run's output files turns the new exe's text back into the old exe's text, so the
byte-identity gate can compare the two runs.

usage:
  strings_reverse.py [--map MAP] (--inplace | -o OUTDIR) FILE...
      Apply the map to each file, line by line (single pass: at every position the longest NEW
      fragment wins; replaced text is never rescanned).
  strings_reverse.py [--map MAP] --check [--base REV]
      Prove the map on the source tree: (1) the map equals the fragments regenerated from the diff
      REV..working tree over src/ and include/; (2) NEW fragments are unique (a function NEW -> OLD)
      and none is empty; (3) applying the map to the new text of every changed literal reproduces
      its old text exactly; (4) no NEW fragment occurs in any unchanged literal of src/ or include/
      (a collision would make the reversal wrong).  Exit status 1 on any failure.
  strings_reverse.py [--map MAP] --write-map [--base REV]
      Regenerate the map from the diff and write it.

REV defaults to the merge base of HEAD and main.  The map defaults to
D:/GnssProcessorSupport/gates/strings/reverse_map.tsv (override with --map or STRINGS_REVERSE_MAP).
Fragment text is the literal's decoded text with {{ }} and %% unescaped to the characters that the
formatted output carries.  Test sources are not part of the map (they never appear in run output).
"""
import argparse
import os
import re
import subprocess
import sys

DEFAULT_MAP = os.environ.get('STRINGS_REVERSE_MAP', 'D:/GnssProcessorSupport/gates/strings/reverse_map.tsv')
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRC_EXT = ('.cpp', '.hpp', '.h', '.inl')


# ---- C++ string-literal lexer -------------------------------------------------------------

_SPECIAL = re.compile(r'[/\'"]')
_ESC = {'n': '\n', 't': '\t', '\\': '\\', '"': '"', "'": "'", 'r': '\r', '0': '\0', 'a': '\a',
        'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}


def lex(src):
    """Return string-literal tokens as (start, end, cstart, cend, is_raw) skipping comments and chars."""
    toks, i, n = [], 0, len(src)
    while i < n:
        m = _SPECIAL.search(src, i)
        if not m:
            break
        i = m.start()
        c = src[i]
        if c == '/' and src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            while 0 < j < n and src[j - 1] == '\\':
                j = src.find('\n', j + 1)
                j = n if j < 0 else j
            i = j
        elif c == '/' and src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == "'":
            p = src[i - 1] if i else ''
            if p.isalnum() and not (p in 'LuU8' and (i < 2 or not src[i - 2].isalnum())):
                i += 1  # digit separator
                continue
            j = i + 1
            while j < n and src[j] != "'":
                if src[j] == '\\':
                    j += 1
                if j < n and src[j] == '\n':
                    break
                j += 1
            i = j + 1
        elif c == '"':
            s = i
            m1 = re.search(r'(?:u8|u|U|L)?R$', src[max(0, i - 3):i])
            if m1 and not (i - len(m1.group(0)) > 0 and (src[i - len(m1.group(0)) - 1].isalnum()
                                                           or src[i - len(m1.group(0)) - 1] == '_')):
                s = i - len(m1.group(0))
                k = src.find('(', i)
                delim = src[i + 1:k]
                endm = ')' + delim + '"'
                e = src.find(endm, k)
                toks.append((s, e + len(endm), k + 1, e, True))
                i = e + len(endm)
                continue
            m2 = re.search(r'(?:u8|u|U|L)$', src[max(0, i - 2):i])
            if m2 and not (i - len(m2.group(0)) > 0 and (src[i - len(m2.group(0)) - 1].isalnum()
                                                           or src[i - len(m2.group(0)) - 1] == '_')):
                s = i - len(m2.group(0))
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == '\\':
                    j += 1
                j += 1
            toks.append((s, j + 1, i + 1, j, False))
            i = j + 1
        else:
            i += 1
    return toks


def decode(raw, is_raw):
    if is_raw:
        return raw
    out, i, n = [], 0, len(raw)
    while i < n:
        c = raw[i]
        if c == '\\' and i + 1 < n:
            d = raw[i + 1]
            if d in _ESC:
                out.append(_ESC[d]); i += 2; continue
            if d == 'x':
                m = re.match(r'[0-9a-fA-F]+', raw[i + 2:])
                out.append(chr(int(m.group(0), 16))); i += 2 + len(m.group(0)); continue
            if d in 'uU':
                ln = 4 if d == 'u' else 8
                out.append(chr(int(raw[i + 2:i + 2 + ln], 16))); i += 2 + ln; continue
            if d.isdigit():
                m = re.match(r'[0-7]{1,3}', raw[i + 1:])
                out.append(chr(int(m.group(0), 8))); i += 1 + len(m.group(0)); continue
            out.append(raw[i:i + 2]); i += 2; continue
        out.append(c); i += 1
    return ''.join(out)


def literal_groups(src):
    """Adjacent string tokens (only whitespace/comments between) form one literal; return decoded texts."""
    texts, cur, last_end = [], [], 0
    for t in lex(src):
        if cur:
            gap = re.sub(r'//[^\n]*|/\*.*?\*/', '', src[last_end:t[0]], flags=re.S)
            if gap.strip() != '':
                texts.append(''.join(cur)); cur = []
        cur.append(decode(src[t[2]:t[3]], t[4]))
        last_end = t[1]
    if cur:
        texts.append(''.join(cur))
    return texts


# ---- fragments -------------------------------------------------------------------------------

# a real placeholder splits the text; {{ }} %% are escaped characters of the formatted output
_TOK = re.compile(r'(\{\{|\}\}|%%)|(\{[^{}]*\}|%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcspn])')


def pieces(text):
    """Split a literal's text at placeholders and newlines; escaped {{ }} %% become { } %."""
    out, cur, pos = [], [], 0
    for m in _TOK.finditer(text):
        cur.append(text[pos:m.start()])
        if m.group(1):
            cur.append({'{{': '{', '}}': '}', '%%': '%'}[m.group(1)])
        else:
            out.append(''.join(cur)); cur = []
        pos = m.end()
    cur.append(text[pos:])
    out.append(''.join(cur))
    res = []
    for p in out:
        res.extend(p.split('\n'))
        res.append(None)  # placeholder boundary marker keeps piece counts comparable
    return res


def output_form(text):
    """The literal's text as the formatted output carries it ({{ }} %% unescaped)."""
    return text.replace('{{', '{').replace('}}', '}').replace('%%', '%')


def rows_for(old_text, new_text):
    po, pn = pieces(old_text), pieces(new_text)
    if len(po) != len(pn):
        raise ValueError('placeholder or newline count changed: %r -> %r' % (old_text[:80], new_text[:80]))
    return [(b, a) for a, b in zip(po, pn) if a != b and a is not None]


def git(*args, check=True):
    r = subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError('git %s failed: %s' % (' '.join(args), r.stderr.decode('utf-8', 'replace')))
    return r.stdout


def default_base():
    return git('merge-base', 'HEAD', 'main').decode().strip()


def changed_pairs(base):
    """Yield (path, old_texts, new_texts, changed_indices) for src/ and include/ files changed since base."""
    names = git('diff', '--name-only', base, '--', 'src', 'include').decode().split()
    for path in names:
        if not path.endswith(SRC_EXT):
            continue
        old = git('show', '%s:%s' % (base, path), check=False).decode('utf-8', 'surrogateescape')
        with open(os.path.join(ROOT, path), encoding='utf-8', errors='surrogateescape', newline='') as f:
            new = f.read()
        go, gn = literal_groups(old), literal_groups(new)
        if len(go) != len(gn):
            raise ValueError('%s: literal count changed %d -> %d' % (path, len(go), len(gn)))
        yield path, go, gn


# ---- map I/O and application -----------------------------------------------------------------

def esc(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\r', '\\r')


def unesc(s):
    return re.sub(r'\\(.)', lambda m: {'t': '\t', 'r': '\r', '\\': '\\'}.get(m.group(1), m.group(0)), s)


def write_map(path, rows):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        for new, old in sorted(rows):
            f.write(esc(new) + '\t' + esc(old) + '\n')


def read_map(path):
    rows = []
    with open(path, encoding='utf-8', newline='') as f:
        for line in f.read().split('\n'):
            if not line:
                continue
            new, old = line.split('\t')
            rows.append((unesc(new), unesc(old)))
    return rows


def trie_regex(words):
    """One regex matching the longest word at each position (common prefixes factored)."""
    trie = {}
    for w in words:
        node = trie
        for ch in w:
            node = node.setdefault(ch, {})
        node[''] = True

    def emit(node):
        end = '' in node
        kids = sorted(k for k in node if k != '')
        if not kids:
            return ''
        alts = []
        for k in kids:
            lit, child = k, node[k]
            while len(child) == 1 and '' not in child:   # collapse single-child chains
                (ck,) = child.keys()
                lit += ck
                child = child[ck]
            alts.append(re.escape(lit) + emit(child))
        body = alts[0] if len(alts) == 1 else '(?:' + '|'.join(alts) + ')'
        return ('(?:' + body + ')?') if end else body

    return re.compile(emit(trie))


class Reverser:
    def __init__(self, rows):
        self.map = dict(rows)
        self.rx = trie_regex(list(self.map)) if self.map else None

    def line(self, s):
        if self.rx is None:
            return s
        return self.rx.sub(lambda m: self.map[m.group(0)], s)

    def text(self, s):
        return ''.join(self.line(l) for l in s.splitlines(keepends=True))


# ---- commands --------------------------------------------------------------------------------

def regenerate(base):
    rows, bad = set(), []
    for path, go, gn in changed_pairs(base):
        for a, b in zip(go, gn):
            if a != b:
                try:
                    rows.update(rows_for(a, b))
                except ValueError as e:
                    bad.append('%s: %s' % (path, e))
    return rows, bad


def cmd_check(args):
    base = args.base or default_base()
    fails = []
    mrows = read_map(args.map)
    gen, bad = regenerate(base)
    fails += bad
    if set(mrows) != gen:
        for new, old in sorted(gen - set(mrows))[:10]:
            fails.append('map is missing a row: %r' % new[:90])
        for new, old in sorted(set(mrows) - gen)[:10]:
            fails.append('map has a stale row: %r' % new[:90])
    news, dup = {}, {}
    for new, old in mrows:
        if new == '':
            fails.append('empty NEW fragment for OLD %r' % old[:90])
        if news.setdefault(new, old) != old:
            dup.setdefault(new, {news[new]}).add(old)
    for new, olds in sorted(dup.items()):
        fails.append('NEW fragment %r maps to %d OLD texts: %s' % (new[:90], len(olds), ' | '.join(sorted(o[:50] for o in olds))))
    rev = Reverser(mrows)
    nchanged = nbad = 0
    changed_paths = set()
    for path, go, gn in changed_pairs(base):
        changed_paths.add(path)
        for a, b in zip(go, gn):
            if a == b:
                continue
            nchanged += 1
            if rev.text(output_form(b)) != output_form(a):
                nbad += 1
                fails.append('%s: reversing a changed literal does not reproduce it: %r' % (path, b[:90]))
    # collisions against unchanged literals of the whole tree
    ncoll = nlit = 0
    hits = {}
    probe = trie_regex([n for n in news if n]) if news else None
    for top in ('src', 'include'):
        for dp, _dn, fn in os.walk(os.path.join(ROOT, top)):
            for f in fn:
                if not f.endswith(SRC_EXT):
                    continue
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, ROOT).replace('\\', '/')
                with open(p, encoding='utf-8', errors='surrogateescape', newline='') as fh:
                    new_src = fh.read()
                texts = literal_groups(new_src)
                if rel in changed_paths:
                    old_src = git('show', '%s:%s' % (base, rel), check=False).decode('utf-8', 'surrogateescape')
                    olds = literal_groups(old_src)
                    texts = [t for t, o in zip(texts, olds) if t == o]
                for t in texts:
                    nlit += 1
                    if probe is not None:
                        for m in probe.finditer(t):
                            ncoll += 1
                            hits.setdefault(m.group(0), []).append(rel)
    for frag, where in sorted(hits.items()):
        fails.append('NEW fragment %r occurs in %d unchanged literal(s), e.g. %s' % (frag[:90], len(where), where[0]))
    print('map rows: %d; changed literals: %d (reverse mismatches %d); unchanged literals scanned: %d (collisions %d)'
          % (len(mrows), nchanged, nbad, nlit, ncoll))
    for f in fails:
        print('FAIL', f)
    print('CHECK', 'PASS' if not fails else 'FAIL (%d)' % len(fails))
    return 1 if fails else 0


def cmd_apply(args):
    rev = Reverser(read_map(args.map))
    for path in args.files:
        with open(path, 'rb') as f:
            data = f.read().decode('utf-8', 'surrogateescape')
        out = rev.text(data)
        if args.inplace:
            dest = path
        else:
            dest = os.path.join(args.outdir, os.path.basename(path))
            os.makedirs(args.outdir, exist_ok=True)
        with open(dest, 'wb') as f:
            f.write(out.encode('utf-8', 'surrogateescape'))
        print('%s: %s' % (dest, 'changed' if out != data else 'unchanged'))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*')
    ap.add_argument('--map', default=DEFAULT_MAP)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--write-map', action='store_true')
    ap.add_argument('--base')
    ap.add_argument('--inplace', action='store_true')
    ap.add_argument('-o', '--outdir')
    args = ap.parse_args()
    if args.write_map:
        rows, bad = regenerate(args.base or default_base())
        for b in bad:
            print('FAIL', b)
        write_map(args.map, rows)
        print('wrote %d rows to %s' % (len(rows), args.map))
        return 1 if bad else 0
    if args.check:
        return cmd_check(args)
    if not args.files or not (args.inplace or args.outdir):
        ap.error('give FILE... with --inplace or -o OUTDIR (or use --check / --write-map)')
    return cmd_apply(args)


if __name__ == '__main__':
    sys.exit(main())
