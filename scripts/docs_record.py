#!/usr/bin/env python3
"""Keep ``docs/`` to the active set and move closed material into ``docs/record/``.

The working set of ``docs/`` holds what a reader needs today: the index, the current
program, the open and parked cards, the registers of known issues (``docs/status/``),
the config ledger, the onboarding pages, the mathematical reference (``docs/report/``)
and the few reference documents an active file cites.  Everything else is the record:
it moves, by ``git mv``, into ``docs/record/``, which mirrors the old relative layout
(``docs/refound/X.md`` becomes ``docs/record/refound/X.md``), so links between record
files keep resolving without an edit.

The rule (one paragraph, also in ``docs/INDEX.md``):

    A card whose STATUS is ``closed`` or ``superseded`` moves.  A card whose STATUS is
    ``open`` or ``parked`` stays where it is.  A ``reference`` card, and a document with
    no STATUS block (a SPEC, design, plan, handoff or queue), stays only while an active
    file cites it by path: ``CLAUDE.md``, ``README.md``, ``CONTRIBUTING.md``,
    ``docs/onboarding/``, ``docs/status/``, ``docs/report/``, the current program's own
    page and the open and parked cards.  A companion file without a STATUS block
    (``*_part2.md``, a notices file) follows its parent card.  The frozen monoliths
    (``docs/archive/``), their split routing stubs, the attic, the decisions and the
    topic pages move whole; the topic page of the config ledger stays with the ledger.

Commands::

    python scripts/docs_record.py plan [--out FILE]   # inventory: stay or move, why, who cites it
    python scripts/docs_record.py apply [--no-code]   # move, re-point, split the lists, verify
    python scripts/docs_record.py verify              # the record invariants and every docs check
    python scripts/docs_record.py close <card>        # move one card now (the commit that closes it)
    python scripts/docs_record.py resolve <old path>  # where an old docs path lives now

``apply`` is re-runnable: on the old layout it performs the whole move and writes the new
index; on a tree already split it moves whatever has closed since (new cards, merged
branches) and leaves the rest alone.  ``close`` is ``apply`` restricted to one card.

What ``apply`` edits, and what it never edits:

* Files that stay (``docs/`` outside the record, ``CLAUDE.md``, ``README.md``,
  ``CONTRIBUTING.md``, ``src/``, ``include/``, ``tests/``, ``scripts/``): Markdown link
  targets and ``docs/...`` path strings that name a moved file are re-pointed.
* Files in the record: only Markdown link targets that pointed outside the record (to a
  file that stays, or outside ``docs/``) are re-pointed; prose is never touched.
* In every file, nothing below a ``<!-- BEGIN VERBATIM:`` marker is touched, so
  ``scripts/docs_split.py --verify`` keeps proving the split byte for byte.
* ``config/`` is not edited (out of scope); its ``docs/`` mentions resolve through the
  path map, ``docs/record/path_map.json``.
* The ledger JSON is edited as text (its ``card`` paths only) and re-rendered.
"""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import subprocess
import sys
from collections import defaultdict

try:
    sys.stdout.reconfigure(encoding='utf-8')
except Exception:  # pragma: no cover
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REC = 'docs/record'                      # the record folder, repo-relative
PATH_MAP = REC + '/path_map.json'
INVENTORY = REC + '/inventory.tsv'
VERBATIM = '<!-- BEGIN VERBATIM:'

# ----------------------------------------------------------------------- roles

#: Folders that stay whole: the registers, onboarding, the mathematical reference.
STAY_DIRS = ('docs/status/', 'docs/onboarding/', 'docs/report/', 'docs/css/')

#: Single files that stay by role, with the reason printed in the plan.
STAY_FILES = {
    'docs/INDEX.md': 'the entry point (rewritten to the active set)',
    'docs/CONFIG_promotion_ledger.json': 'the config ledger',
    'docs/CONFIG_promotion_ledger.md': 'the ledger view generated from the JSON',
    'docs/topics/config-ledger.md': 'the ledger\'s own page',
    'docs/rt_seam_manifest.json': 'read by scripts/rt_seamcut.py',
    'docs/refound/NETWORK_REFOUNDING_2026-09-14.md': 'the current program',
}

#: The program's card lists.  On the first apply both move to the record holding their
#: closed entries, and a new docs/refound/CARDS.md holds the open and parked entries.
ACTIVE_LIST = 'docs/refound/CARDS.md'
OLD_LISTS = ('docs/refound/CARDS.md', 'docs/refound/CARDS_part2.md')
RECORD_LIST_TAIL = 'docs/record/refound/CARDS_part2.md'

#: Folders that move whole, with the reason.
MOVE_DIRS = {
    'docs/archive/': 'frozen monolith',
    'docs/attic/': 'attic',
    'docs/topics/': 'topic page of the closed material',
    'docs/decisions/': 'decision record',
}

#: Single files that move whatever their citations say, with the reason.
MOVE_FILES = {
    'docs/README.md': 'the generated pre-2026-09-14 index, superseded by INDEX.md',
    'docs/REORG_PLAN.md': 'plan of the 2026-09-14 split; this move is its last item',
    'docs/REORG_MAPPING.md': 'mapping of the 2026-09-14 split',
}

#: Active files whose path citations keep a reference card or a loose document.
LIVE_ROOT_FILES = ('CLAUDE.md', 'README.md', 'CONTRIBUTING.md')
LIVE_DIRS = ('docs/onboarding/', 'docs/status/', 'docs/report/')
LIVE_EXTRA = ('docs/refound/NETWORK_REFOUNDING_2026-09-14.md', 'docs/topics/config-ledger.md')

#: Trees scanned for citations and re-pointed by apply (config/ is counted, never edited).
SCAN_ROOT_FILES = ('CLAUDE.md', 'README.md', 'CONTRIBUTING.md', 'CMakeLists.txt')
SCAN_DIRS = ('docs/', 'src/', 'include/', 'tests/', 'scripts/', '.githooks/')
CODE_DIRS = ('src/', 'include/', 'tests/', 'scripts/', '.githooks/', 'CMakeLists.txt')
COUNT_ONLY_DIRS = ('config/',)
TEXT_EXT = ('.md', '.json', '.tex', '.py', '.ps1', '.sh', '.cpp', '.hpp', '.h', '.c',
            '.txt', '.cmake', '.bib', '.sty', '.tsv', '.yml', '.yaml', '.bat', '')
#: Files apply never rewrites by pattern: the tools that own the layout.
SELF_FILES = {'scripts/docs_record.py', 'scripts/docs_index_check.py', 'scripts/docs_split.py',
              'scripts/docs_record_index.md'}

RE_LINK = re.compile(r'(\[[^\]\n]*\]\()(<?)([^)\s>]+)(>?)((?:\s+"[^"\n]*")?\))')
RE_REFDEF = re.compile(r'^(\s*\[[^\]\n]+\]:\s*)(\S+)', re.M)
RE_PATH = re.compile(r'(?<![\w.\-])docs/[\w.\-/]+')
#: The same in a .tex file, where LaTeX writes an underscore as ``\_``.
RE_PATH_TEX = re.compile(r'(?<![\w.\-])docs/(?:[\w.\-/]|\\_)+')
RE_STATUS = re.compile(r'^- \*\*status:\*\*\s*[`*]*([A-Za-z]+)', re.M)
RE_ENTRY_LINK = re.compile(r'^- \[[^\]]*\]\(([^)#\s]+)')


# --------------------------------------------------------------------- helpers

def run(cmd, check=True):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding='utf-8')
    if check and r.returncode != 0:
        raise SystemExit(f'{" ".join(cmd)} failed:\n{r.stdout}{r.stderr}')
    return r


_INDEX = None          # path -> blob sha of the index (git ls-files -s)
_DIRTY = None          # paths whose working-tree content differs from the index
_BLOBS = {}            # blob sha -> bytes, filled in one git cat-file --batch call


def _load_index():
    """The index and the dirty set.  Content is read from the object store, not from the
    working tree: the worktree may sit on a slow USB disk shared with data runs, and one
    cat-file call reads every blob from the pack in a single process."""
    global _INDEX, _DIRTY
    if _INDEX is not None:
        return
    out = subprocess.run(['git', 'ls-files', '-s', '-z'], cwd=ROOT, capture_output=True).stdout
    _INDEX = {}
    for rec in out.split(b'\0'):
        if not rec:
            continue
        meta, path = rec.split(b'\t', 1)
        _mode, sha, _stage = meta.split(b' ')
        _INDEX[path.decode('utf-8')] = sha.decode()
    d = subprocess.run(['git', 'diff', '--name-only', '-z'], cwd=ROOT, capture_output=True).stdout
    _DIRTY = {p.decode('utf-8') for p in d.split(b'\0') if p}


def _prefetch(paths):
    _load_index()
    want = [_INDEX[p] for p in paths if p in _INDEX and p not in _DIRTY
            and _INDEX[p] not in _BLOBS]
    if not want:
        return
    want = list(dict.fromkeys(want))
    proc = subprocess.run(['git', 'cat-file', '--batch'], cwd=ROOT, capture_output=True,
                          input=('\n'.join(want) + '\n').encode())
    buf, pos = proc.stdout, 0
    for sha in want:
        nl = buf.index(b'\n', pos)
        hdr = buf[pos:nl].split()
        size = int(hdr[2])
        _BLOBS[sha] = buf[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1


def _reset_cache():
    global _INDEX, _DIRTY
    _INDEX, _DIRTY = None, None
    _BLOBS.clear()


def tracked(prefixes=None):
    _load_index()
    files = list(_INDEX)
    if prefixes:
        files = [f for f in files if f.startswith(prefixes) or f in prefixes]
    return sorted(files)


def read(path):
    """Text with its own line endings and BOM kept, so a write changes only what we edit."""
    _load_index()
    sha = _INDEX.get(path)
    if sha is not None and path not in _DIRTY:
        if sha not in _BLOBS:
            _prefetch([path])
        raw = _BLOBS[sha]
    else:
        with open(os.path.join(ROOT, path), 'rb') as fh:
            raw = fh.read()
    return raw.decode('utf-8', errors='surrogateescape')


_AUTOCRLF = None


def write(path, text):
    """Write as a checkout would: text read from the object store has LF endings, and with
    core.autocrlf=true (this repository) the working tree carries CRLF.  Git normalises
    either way on add, so the commit is the same; the working copy just stays consistent."""
    global _AUTOCRLF
    if _AUTOCRLF is None:
        r = subprocess.run(['git', 'config', '--get', 'core.autocrlf'], cwd=ROOT,
                           capture_output=True, text=True)
        _AUTOCRLF = r.stdout.strip().lower() == 'true'
    if _AUTOCRLF and '\r\n' not in text and not path.startswith('.githooks/'):
        text = text.replace('\n', '\r\n')
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, 'wb') as fh:
        fh.write(text.encode('utf-8', errors='surrogateescape'))


def is_text(path):
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1].lower()
    return ext in TEXT_EXT and not base.endswith(('.pdf', '.png'))


RE_VERBATIM_LINE = re.compile(r'^' + re.escape(VERBATIM), re.M)


def head_tail(text):
    """(text above the verbatim marker, the marker and everything below).  Only a marker
    at the start of a line counts: CLAUDE.md and the index quote it inside a sentence."""
    m = RE_VERBATIM_LINE.search(text)
    return (text, '') if not m else (text[:m.start()], text[m.start():])


def status_of(text):
    head = head_tail(text)[0][:30000]
    if '**STATUS**' not in head:
        return None
    m = RE_STATUS.search(head)
    return m.group(1).lower() if m else 'unknown'


def is_split_stub(path, text):
    return path.count('/') == 1 and text.split('\n', 1)[0].rstrip('\r').endswith('— SPLIT')


def in_record(path):
    return path.startswith(REC + '/')


def to_record(path):
    return REC + '/' + path[len('docs/'):]


def from_record(path):
    return 'docs/' + path[len(REC) + 1:]


def norm(p):
    return posixpath.normpath(p).replace('\\', '/')


# ------------------------------------------------------------------- citations

def link_targets(path, text):
    """Docs-tree paths a Markdown file links to (above any verbatim marker)."""
    out = set()
    base = posixpath.dirname(path)
    head = head_tail(text)[0]
    for m in list(RE_LINK.finditer(head)):
        t = m.group(3).split('#')[0]
        if not t or '://' in t or t.startswith(('/', 'mailto:')):
            continue
        out.add(norm(posixpath.join(base, t)))
    for m in RE_REFDEF.finditer(head):
        t = m.group(2).split('#')[0]
        if t and '://' not in t:
            out.add(norm(posixpath.join(base, t)))
    return out


def path_strings(text, tex=False):
    r"""``docs/...`` path strings in ``text``; in a .tex file ``\_`` reads as ``_``."""
    out = set()
    for m in (RE_PATH_TEX if tex else RE_PATH).finditer(text):
        p = m.group(0).rstrip('.-/')
        out.add(p.replace('\\_', '_') if tex else p)
    return out


def citations(path, text):
    """Repo-relative paths a file cites, by link or by ``docs/...`` path string."""
    got = set()
    if path.endswith('.md'):
        got |= link_targets(path, text)
        got |= path_strings(head_tail(text)[0])
    else:
        got |= path_strings(text, tex=path.endswith('.tex'))
    return got


# --------------------------------------------------------------------- the plan

class Item:
    __slots__ = ('path', 'status', 'action', 'reason', 'new', 'live', 'code', 'other', 'kind')

    def __init__(self, path):
        self.path, self.status, self.action, self.reason = path, None, '', ''
        self.new, self.live, self.code, self.other, self.kind = path, set(), set(), set(), ''


def classify(only=None):
    """-> (items by path, texts by path).  Pure: reads the tree, changes nothing."""
    docs = tracked(('docs/',))
    _prefetch([p for p in docs if p.endswith('.md')])
    texts = {}
    items = {}
    for p in docs:
        it = Item(p)
        items[p] = it
        if p.endswith('.md'):
            texts[p] = read(p)
            it.status = status_of(texts[p])

    # companions without a STATUS block follow their parent card
    def parent_status(p):
        t = texts.get(p, '')
        stem = re.sub(r'_part\d+\.md$', '.md', p)
        if stem != p and stem in items and items[stem].status:
            return items[stem].status, stem
        for tgt in sorted(link_targets(p, '\n'.join(t.split('\n')[:6]))):
            if tgt in items and items[tgt].status:
                return items[tgt].status, tgt
        return None, None

    # who is a live source: fixed files, live dirs, the program page, open/parked cards
    def live_source(p):
        if p in LIVE_ROOT_FILES or p.startswith(LIVE_DIRS) or p in LIVE_EXTRA:
            return True
        it = items.get(p)
        return bool(it and not in_record(p) and it.status in ('open', 'parked'))

    # citation scan
    scan = [f for f in tracked() if is_text(f) and (
        f in SCAN_ROOT_FILES or f.startswith(SCAN_DIRS) or f.startswith(COUNT_ONLY_DIRS))]
    _prefetch(scan)
    cited_by = defaultdict(lambda: {'live': set(), 'code': set(), 'other': set()})
    alltexts = {}
    for f in scan:
        t = texts.get(f) if f in texts else read(f)
        alltexts[f] = t
        for c in citations(f, t):
            if c == f:
                continue
            if live_source(f):
                cited_by[c]['live'].add(f)
            elif f.startswith(CODE_DIRS) or f in CODE_DIRS or f.startswith(COUNT_ONLY_DIRS):
                cited_by[c]['code'].add(f)
            else:
                cited_by[c]['other'].add(f)

    for p, it in items.items():
        c = cited_by.get(p)
        if c:
            it.live, it.code, it.other = c['live'], c['code'], c['other']

    split_done = os.path.isdir(os.path.join(ROOT, REC))
    for p, it in items.items():
        t = texts.get(p, '')
        if in_record(p):
            it.action, it.reason = 'record', 'already in the record'
            continue
        if p in OLD_LISTS:
            if split_done and p == ACTIVE_LIST:
                it.action, it.reason = 'stay', 'the list of open and parked cards'
            else:
                it.action, it.new = 'move', to_record(p)
                it.reason = 'card list: moves with its closed entries; open entries go to ' \
                            'a new refound/CARDS.md'
            continue
        if p in STAY_FILES:
            it.action, it.reason = 'stay', STAY_FILES[p]
            continue
        if p in MOVE_FILES:
            it.action, it.reason = 'move', MOVE_FILES[p]
            continue
        if p.startswith(STAY_DIRS):
            it.action, it.reason = 'stay', 'folder stays: ' + p.split('/')[1]
            continue
        hit = next((d for d in MOVE_DIRS if p.startswith(d)), None)
        if hit:
            it.action, it.reason = 'move', MOVE_DIRS[hit]
            continue
        if p.endswith('.md') and is_split_stub(p, t):
            it.action, it.reason = 'move', 'routing stub of a frozen monolith'
            continue
        st = it.status
        why_status = ''
        if st is None and p.startswith(('docs/refound/', 'docs/cards/')):
            st, parent = parent_status(p)
            why_status = f' (follows {posixpath.basename(parent)})' if parent else ''
        if st in ('closed', 'superseded'):
            it.action, it.reason = 'move', f'card {st}{why_status}'
        elif st in ('open', 'parked'):
            it.action, it.reason = 'stay', f'card {st}{why_status}'
        elif it.live:
            it.action = 'stay'
            it.reason = ('reference card' if st == 'reference' else 'document') + \
                ' cited by an active file (keep-list)'
            it.kind = 'keep'
        else:
            it.action = 'move'
            it.reason = ('reference card' if st == 'reference' else
                         'document' if st is None else f'status {st}') + \
                ' not cited by an active file'
    for p, it in items.items():
        if it.action == 'move':
            it.new = to_record(p)
    if only:
        for p, it in items.items():
            if p not in only and it.action == 'move':
                it.action, it.new, it.reason = 'stay', p, 'not named by close'
    return items, alltexts


# --------------------------------------------------------------- the rewriting

def make_dirmap(items):
    """Directories every tracked file of which moves: old dir -> new dir."""
    by_dir = defaultdict(list)
    for p, it in items.items():
        d = posixpath.dirname(p)
        while d.startswith('docs/') or d == 'docs':
            by_dir[d].append(it)
            d = posixpath.dirname(d)
    out = {}
    for d, its in by_dir.items():
        if d == 'docs' or in_record(d + '/') or d == REC:
            continue
        if its and all(i.action == 'move' for i in its):
            out[d] = to_record(d + '/').rstrip('/')
    return out


def resolve_new(p, pmap, dmap):
    if p in pmap:
        return pmap[p]
    q = p.rstrip('/')
    if q in dmap:
        return dmap[q]
    for d in sorted(dmap, key=len, reverse=True):
        if q.startswith(d + '/'):
            return dmap[d] + q[len(d):]
    return p


def rewrite(path, newpath, text, pmap, dmap, exists, strings):
    """Re-point links (and, when ``strings``, ``docs/`` path strings) in ``text``.

    ``path`` is where the file was, ``newpath`` where it will be.  Nothing below a
    verbatim marker is touched.  A link whose target did not exist before is left alone.
    """
    head, tail = head_tail(text) if path.endswith('.md') else (text, '')
    old_dir, new_dir = posixpath.dirname(path), posixpath.dirname(newpath)
    n_links = n_paths = 0
    tex = path.endswith('.tex')

    def fix_target(t):
        nonlocal n_links
        if not t or '://' in t or t.startswith(('/', '#', 'mailto:')):
            return t
        tp, sep, anchor = t.partition('#')
        x_old = norm(posixpath.join(old_dir, tp))
        if x_old.startswith('..') or not exists(x_old):
            return t
        x_new = resolve_new(x_old, pmap, dmap)
        if x_new == x_old and new_dir == old_dir:
            return t
        nt = posixpath.relpath(x_new, new_dir or '.')
        if tp.endswith('/') and not nt.endswith('/'):
            nt += '/'
        if norm(posixpath.join(new_dir, nt)) == norm(posixpath.join(old_dir, tp)) and nt == tp:
            return t
        if nt != tp:
            n_links += 1
        return nt + sep + anchor

    def fix_strings(seg):
        nonlocal n_paths

        def sub(m):
            nonlocal n_paths
            s = m.group(0)
            escaped = tex and '\\_' in s       # \texttt{} escapes; \url{} and \path{} do not
            if escaped:                        # match with the escape, resolve without it
                s = s.replace('\\_', '_')
            core = s
            trail = ''
            while core and core not in pmap and core.rstrip('/') not in dmap \
                    and core[-1] in '.-/':
                trail = core[-1] + trail
                core = core[:-1]
            if core in pmap or core.rstrip('/') in dmap:
                new = resolve_new(core, pmap, dmap)
                if core.endswith('/') and not new.endswith('/'):
                    new += '/'
                if new != core:
                    n_paths += 1
                return (new + trail).replace('_', '\\_') if escaped else new + trail
            return m.group(0)
        return (RE_PATH_TEX if tex else RE_PATH).sub(sub, seg)

    if path.endswith('.md'):
        out, pos = [], 0
        for m in RE_LINK.finditer(head):
            seg = head[pos:m.start()]
            out.append(fix_strings(seg) if strings else seg)
            out.append(m.group(1) + m.group(2) + fix_target(m.group(3)) + m.group(4) + m.group(5))
            pos = m.end()
        seg = head[pos:]
        out.append(fix_strings(seg) if strings else seg)
        head = ''.join(out)
        head = RE_REFDEF.sub(lambda m: m.group(1) + fix_target(m.group(2)), head)
    elif strings:
        head = fix_strings(head)
    return head + tail, n_links, n_paths


# ------------------------------------------------------------ the card lists

def parse_entries(text):
    """Split a card list into (preamble lines, [entry blocks]); an entry is a ``- [`` line
    plus its indented continuation lines."""
    lines = text.splitlines(keepends=True)
    pre, entries, cur = [], [], None
    for ln in lines:
        if ln.startswith('- ['):
            cur = [ln]
            entries.append(cur)
        elif cur is not None and (ln.startswith('  ') and ln.strip()):
            cur.append(ln)
        elif cur is None:
            pre.append(ln)
        else:
            cur = None
            if ln.strip():
                pre.append(ln)       # stray text after entries: kept with the preamble
    return pre, [''.join(e) for e in entries]


def entry_card(list_path, entry):
    m = RE_ENTRY_LINK.match(entry)
    if not m:
        return None
    return norm(posixpath.join(posixpath.dirname(list_path), m.group(1)))


ACTIVE_CARDS_HEAD = """# The current program: open and parked cards

The cards of the network re-founding program
([NETWORK_REFOUNDING_2026-09-14.md](NETWORK_REFOUNDING_2026-09-14.md)) that are open or parked,
one entry per card in pre-registration order. A new card is pre-registered here (rule 3 of
[the index](../INDEX.md)); when it closes, `python scripts/docs_record.py close <card>` moves the
card into the record and its entry to [the record's card list](../record/refound/CARDS_part2.md).
The closed cards of the program are listed in [record/refound/CARDS.md](../record/refound/CARDS.md)
and [record/refound/CARDS_part2.md](../record/refound/CARDS_part2.md).

"""


def split_lists(items, texts, rec_pmap, dmap, exists):
    """The card lists.  First apply: each old list moves to the record keeping its closed
    entries (links re-pointed), and the open and parked entries of both, verbatim and in
    order, form the new docs/refound/CARDS.md.  Later runs: the entries of cards that move
    leave docs/refound/CARDS.md for the end of the record's part 2.  -> {path: text}"""
    writes = {}
    if not os.path.isdir(os.path.join(ROOT, REC)):
        active = []
        for src in OLD_LISTS:
            if src not in texts:
                continue
            dst = to_record(src)
            pre, entries = parse_entries(texts[src])
            closed = []
            for e in entries:
                it = items.get(entry_card(src, e))
                if it is not None and it.action == 'stay':
                    active.append(e)
                else:
                    closed.append(rewrite(src, dst, e, rec_pmap, dmap, exists, False)[0])
            writes[dst] = rewrite(src, dst, ''.join(pre), rec_pmap, dmap, exists,
                                  False)[0] + ''.join(closed)
        writes[ACTIVE_LIST] = ACTIVE_CARDS_HEAD + ''.join(active)
    else:
        pre, entries = parse_entries(texts[ACTIVE_LIST])
        stay_e, move_e = [], []
        for e in entries:
            card = entry_card(ACTIVE_LIST, e)
            if card in rec_pmap:
                move_e.append(rewrite(ACTIVE_LIST, RECORD_LIST_TAIL, e, rec_pmap, dmap,
                                      exists, False)[0])
            else:
                stay_e.append(e)
        if move_e:
            writes[ACTIVE_LIST] = ''.join(pre) + ''.join(stay_e)
            tail = read(RECORD_LIST_TAIL)
            nl = '\r\n' if '\r\n' in tail else '\n'
            if not tail.endswith('\n'):
                tail += nl
            writes[RECORD_LIST_TAIL] = tail + ''.join(move_e)
    return writes


# ------------------------------------------------------------------ the indexes

RECORD_INDEX_HEAD = """# The record

This folder holds the project's closed documentation: the closed and superseded cards, the
frozen monoliths and their routing stubs, the attic, the decisions, the topic pages and the
specs, designs, plans and handoffs no active file cites. It mirrors the layout `docs/` had
before the move (`docs/refound/X.md` is now `docs/record/refound/X.md`), so links between
record files resolve unchanged. Nothing here was rewritten: only link targets that pointed
outside the record were re-pointed. The active documentation starts at [../INDEX.md](../INDEX.md).

- **Where an old path went:** [path_map.json](path_map.json) maps every moved file, old path to
  new; `python scripts/docs_record.py resolve <old path>` looks one up.
- **What moved and why:** [inventory.tsv](inventory.tsv), one row per docs file at the move.
- **Closed program cards:** [refound/CARDS.md](refound/CARDS.md) and
  [refound/CARDS_part2.md](refound/CARDS_part2.md), in pre-registration order.
- **A card that closes later** moves here in the commit that closes it
  (`python scripts/docs_record.py close <card>`); a result for a closed card is still appended
  to the card's own file, below its text, under a dated heading.

The index of the docs tree as it stood on {date}, the day of the move, follows unchanged
(its topic tree is the map of the closed material; its rules were the rules of that period).

---

"""


def build_record_index(old_index, pmap, dmap, exists, date):
    body, _, _ = rewrite('docs/INDEX.md', REC + '/INDEX.md', old_index, pmap, dmap, exists,
                         strings=False)
    return RECORD_INDEX_HEAD.replace('{date}', date) + body


def build_active_index(items, date):
    keep = sorted(p for p, it in items.items() if it.action == 'stay' and it.kind == 'keep')
    opens = sorted(p for p, it in items.items() if it.action == 'stay'
                   and it.reason.startswith('card ') and 'follows' not in it.reason
                   and not re.search(r'_part\d+\.md$', p))

    def rel(p):
        return posixpath.relpath(p, 'docs')

    def citers(it):
        names = sorted(posixpath.relpath(c, 'docs') if c.startswith('docs/') else c
                       for c in it.live)
        return ', '.join(f'`{n}`' for n in names[:3]) + \
            (f' and {len(names) - 3} more' if len(names) > 3 else '')

    tmpl = open(os.path.join(HERE, 'docs_record_index.md'), encoding='utf-8').read()
    keep = [p for p in keep if f']({rel(p)})' not in tmpl]     # the template links some
    keep_lines = '\n'.join(f'  - [{rel(p)}]({rel(p)}): cited by {citers(items[p])}'
                           for p in keep)
    return (tmpl.replace('{date}', date)
                .replace('{n_open}', str(len(opens)))
                .replace('{keep_list}', keep_lines)
                .replace('\r\n', '\n'))


# ------------------------------------------------------------------- the maps

def update_split_maps(pmap):
    """Re-point the docs_split mapping files: card targets, the archive, the stub."""
    mdir = os.path.join(HERE, 'docs_map')
    changed = []
    for name in sorted(os.listdir(mdir)):
        if not name.endswith('.json'):
            continue
        fp = os.path.join(mdir, name)
        raw = open(fp, 'rb').read().decode('utf-8')
        m = json.loads(raw)
        doc = m['doc']
        arch_old = m.get('archive', 'archive/' + doc)
        stub_old = m.get('stub', doc)
        arch_new = resolve_new('docs/' + arch_old, pmap, {})[len('docs/'):]
        stub_new = resolve_new('docs/' + stub_old, pmap, {})[len('docs/'):]
        # Edited as text, so the file keeps its shape and only paths change.
        text = re.sub(r'"((?:cards|decisions|record)/[^"\n]+\.md)"',
                      lambda mm: '"' + resolve_new('docs/' + mm.group(1), pmap, {})[5:] + '"', raw)
        for key, old, new in (('archive', arch_old, arch_new), ('stub', stub_old, stub_new)):
            if new == old:
                continue
            if f'"{key}":' in text:
                text = re.sub(r'("' + key + r'":\s*)"[^"\n]*"', lambda mm: mm.group(1) + f'"{new}"',
                              text, count=1)
            else:                          # a new key, on its own line after "title"/"archive"
                anchor = 'archive' if '"archive":' in text else 'title'
                mt = re.search(r'^([ \t]*)"' + anchor + r'":[^\n]*,(\r?\n)', text, re.M)
                ins = f'{mt.group(1)}"{key}": "{new}",{mt.group(2)}'
                text = text[:mt.end()] + ins + text[mt.end():]
        if text != raw:
            json.loads(text)                   # still valid JSON
            open(fp, 'wb').write(text.encode('utf-8'))
            changed.append('scripts/docs_map/' + name)
    return changed


def load_path_map():
    fp = os.path.join(ROOT, PATH_MAP)
    if os.path.exists(fp):
        return json.load(open(fp, encoding='utf-8'))
    return {}


def write_inventory(items, path=None, date=''):
    rows = ['path\taction\tnew path\treason\tlive citers\tcode citers\tother citers']
    for p in sorted(items):
        it = items[p]
        if it.action == 'record':
            continue
        live = ', '.join(sorted(it.live)[:6]) + (f' (+{len(it.live) - 6})' if len(it.live) > 6 else '')
        rows.append('\t'.join([p, it.action, it.new if it.action == 'move' else '', it.reason,
                               live, str(len(it.code)), str(len(it.other))]))
    text = '\n'.join(rows) + '\n'
    if path:
        write(path, text)
    return text


# ------------------------------------------------------------------- commands

def summarize(items):
    by = defaultdict(lambda: defaultdict(int))
    for p, it in items.items():
        if it.action == 'record':
            continue
        parts = p.split('/')
        folder = parts[1] if len(parts) > 2 else '(top)'
        by[folder][it.action] += 1
    print(f'{"folder":<14} {"stay":>5} {"move":>5}')
    tot = defaultdict(int)
    for f in sorted(by):
        print(f'{f:<14} {by[f]["stay"]:>5} {by[f]["move"]:>5}')
        for k, v in by[f].items():
            tot[k] += v
    print(f'{"total":<14} {tot["stay"]:>5} {tot["move"]:>5}')
    keep = sorted(p for p, it in items.items() if it.kind == 'keep' and it.action == 'stay')
    print(f'\nkeep-list ({len(keep)}): reference cards and documents an active file cites')
    for p in keep:
        it = items[p]
        print(f'  {p}  <- ' + ', '.join(sorted(it.live)[:4]) +
              (f' (+{len(it.live) - 4})' if len(it.live) > 4 else ''))


def cmd_plan(a):
    items, _ = classify()
    summarize(items)
    if a.out:
        write_inventory(items, None)
        with open(a.out, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(write_inventory(items))
        print(f'\ninventory: {a.out}')
    return 0


def do_apply(only=None, code=True, date=None):
    import datetime as _dt
    date = date or _dt.date.today().isoformat()
    first = not os.path.isdir(os.path.join(ROOT, REC))
    items, texts = classify(only)
    moves = {p: it.new for p, it in items.items() if it.action == 'move'}
    if not moves:
        print('nothing to move')
        return 0
    dmap = make_dirmap(items)
    # Record files that link to the card list mean the list their entry sits in (the
    # record's); files that stay mean the active list, which keeps the old path.
    rec_pmap = dict(moves)
    stay_pmap = {k: v for k, v in moves.items() if k != ACTIVE_LIST}
    pre_files = set(tracked())
    pre_dirs = set()
    for f in pre_files:
        d = posixpath.dirname(f)
        while d and d not in pre_dirs:
            pre_dirs.add(d)
            d = posixpath.dirname(d)

    def exists_pre(p):
        return p in pre_files or p.rstrip('/') in pre_dirs

    # 1. every rewrite is computed against the tree before any move
    new_text = {}
    n_l = n_p = 0
    for f, t in texts.items():
        if f in SELF_FILES or f.startswith('scripts/docs_map/') or f.startswith(COUNT_ONLY_DIRS):
            continue
        if f in OLD_LISTS and (first or f == ACTIVE_LIST):
            continue                           # the lists are split in step 2
        if f == 'docs/INDEX.md' and first:
            continue                           # replaced in step 2
        if not code and f.startswith(CODE_DIRS):
            continue
        dest = moves.get(f, f)
        record_file = in_record(dest)
        if record_file and not f.endswith('.md'):
            continue
        out, nl, np_ = rewrite(f, dest, t, rec_pmap if record_file else stay_pmap, dmap,
                               exists_pre, strings=not record_file)
        if out != t:
            new_text[dest] = out
            n_l, n_p = n_l + nl, n_p + np_

    # 2. the card lists and, on the first apply, the two indexes
    new_text.update(split_lists(items, texts, rec_pmap, dmap, exists_pre))
    if first:
        new_text[REC + '/INDEX.md'] = build_record_index(texts['docs/INDEX.md'], rec_pmap,
                                                         dmap, exists_pre, date)
        new_text['docs/INDEX.md'] = build_active_index(items, date)

    # 3. the moves: one git mv per destination folder
    by_dir = defaultdict(list)
    for src in sorted(moves):
        by_dir[posixpath.dirname(moves[src])].append(src)
    for d, srcs in sorted(by_dir.items()):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
        for i in range(0, len(srcs), 100):
            run(['git', 'mv'] + srcs[i:i + 100] + [d + '/'])
    # folders the move emptied (git tracks no folders; the working tree keeps them)
    for d in sorted({posixpath.dirname(s) for s in moves}, key=len, reverse=True):
        while d.startswith('docs/') and not in_record(d + '/'):
            full = os.path.join(ROOT, d)
            if not os.path.isdir(full) or os.listdir(full):
                break
            os.rmdir(full)
            d = posixpath.dirname(d)
    # 4. the rewritten texts
    for p, t in sorted(new_text.items()):
        write(p, t)
    run(['git', 'add', '--'] + sorted(new_text))
    # 5. the split maps, the ledger view, the path map and the inventory
    changed = update_split_maps(moves)
    if changed:
        run(['git', 'add', '--'] + changed)
    run([sys.executable, os.path.join(HERE, 'ledger_render.py')])
    pm = load_path_map()
    pm.update(moves)
    for k in list(pm):                      # a chain old -> mid -> new collapses
        seen = set()
        while pm[k] in pm and pm[k] not in seen:
            seen.add(pm[k])
            pm[k] = pm[pm[k]]
    write(PATH_MAP, json.dumps(dict(sorted(pm.items())), indent=1, ensure_ascii=False) + '\n')
    if first:
        write_inventory(items, INVENTORY)
    run(['git', 'add', '--', PATH_MAP, 'docs/CONFIG_promotion_ledger.md'] +
        ([INVENTORY] if first else []))
    _reset_cache()
    print(f'moved {len(moves)} files; re-pointed {n_l} links and {n_p} path strings in '
          f'{len(new_text)} files; split maps changed: {len(changed)}')
    return 0


def cmd_apply(a):
    rc = do_apply(code=not a.no_code)
    return rc or cmd_verify(a)


def cmd_close(a):
    if not os.path.isdir(os.path.join(ROOT, REC)):
        raise SystemExit('close: docs/record/ does not exist yet; the first move is '
                         '"python scripts/docs_record.py apply"')
    card = a.card.replace('\\', '/')
    if not card.startswith('docs/'):
        hits = [f for f in tracked(('docs/',)) if not in_record(f) and f.endswith('.md')
                and (posixpath.basename(f).startswith(card + '_') or posixpath.basename(f) == card
                     or posixpath.basename(f) == card + '.md')]
        if len(hits) > 1:                  # a card and its companions: name the card
            hits = [f for f in hits if not re.search(r'_part\d+\.md$', f)] or hits
        if len(hits) != 1:
            raise SystemExit(f'close: "{a.card}" matches {len(hits)} files: {hits}')
        card = hits[0]
    st = status_of(read(card))
    if st not in ('closed', 'superseded'):
        raise SystemExit(f'close: {card} has STATUS "{st}"; set it to closed or superseded first')
    names = {card} | {f for f in tracked(('docs/',))
                      if re.sub(r'_part\d+\.md$', '.md', f) == card}
    rc = do_apply(only=names)
    return rc or cmd_verify(a)


def cmd_resolve(a):
    pm = load_path_map()
    p = a.path.replace('\\', '/')
    if not p.startswith('docs/'):
        p = 'docs/' + p
    print(pm.get(p, f'{p} (not moved)'))
    return 0


def cmd_verify(a):
    bad = []
    if os.path.isdir(os.path.join(ROOT, REC)):
        items, _ = classify()
        for p, it in sorted(items.items()):
            if it.action == 'move':
                bad.append(f'RECORD    {p} should be in the record ({it.reason})')
        pm = load_path_map()
        for old, new in pm.items():
            if not os.path.exists(os.path.join(ROOT, new)):
                bad.append(f'PATHMAP   {old} -> {new}: target missing')
        # an open or parked program card is listed in the active card list
        listed = {entry_card(ACTIVE_LIST, e) for e in parse_entries(read(ACTIVE_LIST))[1]}
        for p, it in sorted(items.items()):
            if (p.startswith('docs/refound/') and it.status in ('open', 'parked')
                    and not re.search(r'_part\d+\.md$', p) and p not in listed):
                print(f'  WARN      {p} is {it.status} but has no entry in {ACTIVE_LIST}')
    for cmd in (['docs_index_check.py'], ['docs_split.py', '--verify'],
                ['ledger_render.py', '--check'], ['make_golden_config.py', '--verify']):
        r = run([sys.executable, os.path.join(HERE, cmd[0])] + cmd[1:], check=False)
        last = (r.stdout.strip().splitlines() or [''])[-1]
        fails = [ln for ln in r.stdout.splitlines() if ln.startswith('FAIL')]
        print(f'{"PASS" if r.returncode == 0 else "FAIL"}  {" ".join(cmd)}: {last[:110]}')
        if r.returncode != 0:
            bad.append(f'CHECK     {" ".join(cmd)} exit {r.returncode}: ' +
                       '; '.join(fails[:3]) + (r.stdout[-600:] if not fails else ''))
    for b in bad:
        print('  ' + b)
    print('verify: ' + ('PASS' if not bad else f'{len(bad)} problem(s)'))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('plan')
    p.add_argument('--out')
    p = sub.add_parser('apply')
    p.add_argument('--no-code', action='store_true',
                   help='leave src/, include/, tests/, scripts/ unedited')
    sub.add_parser('verify')
    p = sub.add_parser('close')
    p.add_argument('card')
    p = sub.add_parser('resolve')
    p.add_argument('path')
    a = ap.parse_args()
    return {'plan': cmd_plan, 'apply': cmd_apply, 'verify': cmd_verify,
            'close': cmd_close, 'resolve': cmd_resolve}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
