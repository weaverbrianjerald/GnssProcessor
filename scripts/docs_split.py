#!/usr/bin/env python3
"""Split a docs/ monolith into one-card-per-file, verbatim and verifiably.

The rule this script exists to enforce: no existing section text is ever
rewritten, shortened or reflowed. Every byte of a monolith ends up in
exactly one target file, in document order, and ``--verify`` proves it by
hash: concatenating the target files' verbatim bodies in document order must
reproduce the archived monolith byte for byte.

Usage
-----
    python scripts/docs_split.py --sizes            # dry run: target sizes only
    python scripts/docs_split.py --build            # write cards + archive + stub
    python scripts/docs_split.py --verify           # hash proof (prints hashes)
    python scripts/docs_split.py --tables           # markdown mapping tables
                                                    #   (for docs/REORG_PLAN.md)

    ... --only SPEC_event_harness.md                # restrict to one monolith

Mapping files
-------------
``scripts/docs_map/<monolith stem>.json``::

    {
      "doc":   "SPEC_event_harness.md",
      "topic": "harness",
      "title": "The event harness",
      "groups": [
        ["__preamble__", "cards/harness/HARNESS-REF.md", "HARNESS",
         "reference", "what the harness is, ..."],
        ["## 9. Study 1", "cards/harness/STUDY-1-erase-vs-refloat.md",
         "Study 1", "closed", ""],
        ...
      ]
    }

Each group is ``[anchor, target, id, status, summary]``. ``anchor`` is a
prefix of exactly one heading line (``##`` or ``###``) in the monolith, or the
literal ``__preamble__`` for the text above the first heading (which must be
the first group). A group owns every heading from its anchor up to (not
including) the next group's anchor, so the groups partition the file into
contiguous slices. The same target may appear in several groups, since the
document is chronological and cards interleave, and its slices are
concatenated in document order.

Line endings are LF everywhere; the reader normalises CRLF on input so the
verification is stable no matter what ``core.autocrlf`` did to the checkout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys

try:  # the docs are full of §, σ̂₀ and em dashes; cp1252 consoles choke
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
except Exception:  # pragma: no cover - older/odd streams
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DOCS = os.path.join(ROOT, 'docs')
MAPS = os.path.join(HERE, 'docs_map')

ARCHIVE_BANNER = (
    '<!-- FROZEN / READ-ONLY: the pre-split monolith, kept byte-for-byte. '
    'Never append here — new results go in the card file '
    '(see docs/INDEX.md). -->'
)
MARKER = '<!-- BEGIN VERBATIM: {doc} -->'

RE_HEADING = re.compile(r'^#{2,3} ', re.M)
RE_COMMIT = re.compile(r'(?<![0-9A-Za-z_/.-])(?=[0-9a-f]{7,40}(?![0-9A-Za-z_]))'
                       r'(?=[0-9]*[a-f])([0-9a-f]{7,40})')
RE_RUN = re.compile(r'\bruns/[A-Za-z0-9_][A-Za-z0-9_.-]*')
RE_RUNB = re.compile(r'\bbuild/x64-Release/run_[A-Za-z0-9_.-]+')
RE_KNOB = re.compile(r'\b(?:network|estimation|processing|realtime|baseline)'
                     r'\.[a-z0-9_]+(?:\.[a-z0-9_]+)*')
RE_DATE = re.compile(r'\b20\d\d-\d\d-\d\d\b')
RE_SECNO = re.compile(r'^#{2,3}\s+(?:§\s*)?([A-Z]?[0-9]+(?:\.[0-9a-z]+)*[a-z]?|'
                      r'§?[A-Z][0-9]*(?:\.[0-9]+)*)\b')


# ---------------------------------------------------------------- utilities

def read_lf(path: str) -> str:
    """Read a file as text with LF endings, whatever the checkout did."""
    with open(path, 'rb') as fh:
        raw = fh.read()
    if raw.startswith(b'\xef\xbb\xbf'):
        raise SystemExit(f'{path}: unexpected UTF-8 BOM')
    return raw.replace(b'\r\n', b'\n').decode('utf-8')


def write_lf(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(text)


def sha(text: str) -> str:
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def load_maps(only: str | None):
    out = []
    for name in sorted(os.listdir(MAPS)):
        if not name.endswith('.json'):
            continue
        m = json.load(open(os.path.join(MAPS, name), encoding='utf-8'))
        if only and m['doc'] != only:
            continue
        m['_map_file'] = os.path.join(MAPS, name)
        out.append(m)
    if only and not out:
        raise SystemExit(f'no mapping file for {only}')
    return out


def heading_offsets(text: str):
    """[(char offset, heading line)] for every ## / ### heading, in order."""
    return [(m.start(), text[m.start():text.index('\n', m.start())
                             if '\n' in text[m.start():] else len(text)])
            for m in RE_HEADING.finditer(text)]


def resolve_groups(text: str, groups):
    """Turn the mapping's groups into (target, id, status, summary, slices).

    Returns ``ordered``: the groups in document order, each with the
    ``(start, end)`` char range it owns and the heading lines inside it.
    """
    heads = heading_offsets(text)
    if not heads:
        raise SystemExit('no ## headings found')
    if groups[0][0] != '__preamble__':
        raise SystemExit('the first group must anchor at __preamble__')

    resolved = []
    for g in groups:
        anchor = g[0]
        if anchor == '__preamble__':
            resolved.append((0, g))
            continue
        hits = [off for off, line in heads if line.startswith(anchor)]
        if len(hits) != 1:
            raise SystemExit(
                f'anchor {anchor!r} matched {len(hits)} headings '
                f'(need exactly 1)')
        resolved.append((hits[0], g))

    offs = [r[0] for r in resolved]
    if offs != sorted(offs):
        raise SystemExit('group anchors are not in document order')
    if len(set(offs)) != len(offs):
        raise SystemExit('two groups anchor at the same heading')

    ordered = []
    for i, (off, g) in enumerate(resolved):
        end = resolved[i + 1][0] if i + 1 < len(resolved) else len(text)
        inside = [line for h, line in heads if off <= h < end]
        ordered.append(dict(start=off, end=end, target=g[1], id=g[2],
                            status=g[3], summary=(g[4] if len(g) > 4 else ''),
                            anchor=g[0], headings=inside))
    return ordered


def sec_label(line: str) -> str:
    """The citable section label of a heading line, e.g. '§24.10' or '11.2'."""
    m = RE_SECNO.match(line)
    if not m:
        return ''
    lab = m.group(1)
    return ('§' + lab) if '§' in line[:line.index(lab)] else lab


def facts(body: str):
    """Auto-extract the STATUS facts that are in the text (never invented)."""
    commits, seen = [], set()
    for m in RE_COMMIT.finditer(body):
        t = m.group(1)
        if t not in seen:
            seen.add(t)
            commits.append(t)
    runs = []
    for rx in (RE_RUN, RE_RUNB):
        for t in rx.findall(body):
            t = t.rstrip('.,;:')
            if t not in runs:
                runs.append(t)
    knobs = []
    for t in RE_KNOB.findall(body):
        t = t.rstrip('.')
        if t.endswith('_') or t.count('.') == 0:
            continue  # a line-wrapped knob name; do not quote half a knob
        if t not in knobs:
            knobs.append(t)
    dates = sorted(set(RE_DATE.findall(body)))
    return dict(commits=commits, runs=runs, knobs=knobs, dates=dates)


def fmt_list(items, cap, code=True):
    if not items:
        return '—'
    shown = items[:cap]
    s = ', '.join(f'`{x}`' if code else x for x in shown)
    if len(items) > cap:
        s += f' … (+{len(items) - cap} more in the text)'
    return s


# ------------------------------------------------------------------- build

def target_topic(tgt, fallback):
    """A card's topic is the directory it sits in; decisions/ keep the doc's."""
    parts = tgt.split('/')
    return parts[1] if parts[0] == 'cards' and len(parts) > 2 else fallback


def build_target(doc, topic, tgt, entries, text):
    """Render one card / decision file."""
    topic = target_topic(tgt, topic)
    body = ''.join(text[e['start']:e['end']] for e in entries)
    first = entries[0]
    labels = []
    for e in entries:
        for h in e['headings']:
            lab = sec_label(h)
            if lab and lab not in labels:
                labels.append(lab)
    if first['anchor'] == '__preamble__':
        labels.insert(0, '(preamble)')
    f = facts(body)
    summary = first['summary'] or (
        first['headings'][0].lstrip('# ').strip() if first['headings']
        else topic)
    title = f"{first['id']} — {summary}"

    span = '—'
    if f['dates']:
        span = f['dates'][0] if f['dates'][0] == f['dates'][-1] else \
            f"{f['dates'][0]} → {f['dates'][-1]}"

    head = [
        f'# {title}',
        '',
        '**STATUS**',
        '',
        f"- **id:** {first['id']}",
        f'- **topic:** {topic}',
        f"- **status:** {first['status']}",
        f'- **summary:** {summary}',
        f'- **dates in the text:** {span}',
        f"- **knobs / posture:** {fmt_list(f['knobs'], 10)}",
        f"- **hashes named in the text** (commit / exe / byte-cert): "
        f"{fmt_list(f['commits'], 8)}",
        f"- **run dirs named in the text:** {fmt_list(f['runs'], 8)}",
        f"- **source:** `docs/archive/{doc}` — "
        f"{', '.join(labels) if labels else '(see body)'}",
        '',
        '> Results are appended TO THIS FILE, below the verbatim text. The '
        'archived monolith is frozen; never append there.',
        '',
        '---',
        MARKER.format(doc=doc),
    ]
    return '\n'.join(head) + '\n' + body


def build_stub(doc, topic, title, ordered):
    rows = []
    for e in ordered:
        rel = '../' * 0  # stubs live in docs/, targets under docs/
        if e['anchor'] == '__preamble__':
            rows.append(('(title + preamble)', e['target'], e['id']))
        for h in e['headings']:
            txt = h.lstrip('# ').strip()
            if len(txt) > 96:
                txt = txt[:93] + '…'
            txt = txt.replace('|', '\\|')
            rows.append((txt, e['target'], e['id']))
    out = [
        f'# {title} — SPLIT',
        '',
        f'`docs/{doc}` was split on 2026-09-14 — the original is '
        f'[archive/{doc}](archive/{doc}); sections map as follows.',
        '',
        f'The archived copy is **frozen**: it is kept byte-for-byte as proof '
        f'and is never appended to. New results go in the card file named '
        f'below. See [INDEX.md](INDEX.md) for the whole tree and the rules, '
        f'and [topics/{topic}.md](topics/{topic}.md) for this subject.',
        '',
        '| section | card id | file |',
        '| --- | --- | --- |',
    ]
    for label, target, cid in rows:
        out.append(f'| {label} | {cid} | [{target}]({target}) |')
    out.append('')
    return '\n'.join(out)


def do_build(maps, dry):
    report = []
    for m in maps:
        doc, topic = m['doc'], m['topic']
        src = os.path.join(DOCS, doc)
        arch_path = os.path.join(DOCS, 'archive', doc)
        if os.path.exists(arch_path):
            # already split once: the archive is the source of truth, never the
            # stub that now sits at the old path.  Keeps --build idempotent.
            text = read_lf(arch_path).split('\n', 1)[1]
        else:
            text = read_lf(src)
        ordered = resolve_groups(text, m['groups'])

        by_target = {}
        for e in ordered:
            by_target.setdefault(e['target'], []).append(e)

        sizes = []
        for tgt, entries in by_target.items():
            content = build_target(doc, topic, tgt, entries, text)
            sizes.append((len(content.encode('utf-8')), tgt))
            if not dry:
                write_lf(os.path.join(DOCS, tgt), content)

        stub = build_stub(doc, topic, m['title'], ordered)
        arch = ARCHIVE_BANNER + '\n' + text
        if not dry:
            write_lf(os.path.join(DOCS, 'archive', doc), arch)
            write_lf(src, stub)

        sizes.sort(reverse=True)
        report.append((doc, len(text.encode('utf-8')), len(by_target),
                       len(stub.encode('utf-8')), sizes))

    print(f'{"monolith":<42} {"bytes":>8} {"cards":>6} {"stub":>7} '
          f'{"largest card":>13}')
    over = []
    for doc, nb, ncards, nstub, sizes in report:
        print(f'{doc:<42} {nb:>8} {ncards:>6} {nstub:>7} '
              f'{sizes[0][0]:>13}  {sizes[0][1]}')
        for n, t in sizes:
            if n > 60 * 1024:
                over.append((n, t))
        if nstub > 60 * 1024:
            over.append((nstub, f'docs/{doc} (stub)'))
    if over:
        print('\n!! over the 60 KB rule:')
        for n, t in sorted(over, reverse=True):
            print(f'   {n:>8}  {t}')
    return 1 if over else 0


# ------------------------------------------------------------------ verify

def do_verify(maps):
    bad = 0
    for m in maps:
        doc, topic = m['doc'], m['topic']
        arch_path = os.path.join(DOCS, 'archive', doc)
        if not os.path.exists(arch_path):
            print(f'MISSING ARCHIVE  {doc}')
            bad += 1
            continue
        arch = read_lf(arch_path)
        if not arch.startswith('<!-- FROZEN'):
            print(f'{doc}: archive is missing its READ-ONLY banner')
            bad += 1
            continue
        original = arch.split('\n', 1)[1]

        ordered = resolve_groups(original, m['groups'])
        marker = MARKER.format(doc=doc)

        bodies, cursors = {}, {}
        for e in ordered:
            tgt = e['target']
            if tgt not in bodies:
                p = os.path.join(DOCS, tgt)
                if not os.path.exists(p):
                    print(f'MISSING CARD  {tgt}')
                    bad += 1
                    bodies[tgt] = None
                    continue
                t = read_lf(p)
                if t.count(marker) != 1:
                    print(f'{tgt}: expected exactly one verbatim marker')
                    bad += 1
                    bodies[tgt] = None
                    continue
                bodies[tgt] = t.split(marker + '\n', 1)[1]
                cursors[tgt] = 0
        if any(v is None for v in bodies.values()):
            continue

        rebuilt = []
        for e in ordered:
            n = e['end'] - e['start']
            tgt = e['target']
            rebuilt.append(bodies[tgt][cursors[tgt]:cursors[tgt] + n])
            cursors[tgt] += n
        rebuilt = ''.join(rebuilt)

        tail = {t: len(b) - cursors[t] for t, b in bodies.items()
                if len(b) != cursors[t]}
        h_arch, h_cards = sha(original), sha(rebuilt)
        ok = (h_arch == h_cards) and not tail
        print(f'{"PASS" if ok else "FAIL"}  {doc}')
        print(f'      archive sha256 {h_arch}')
        print(f'      cards   sha256 {h_cards}   '
              f'({len(set(e["target"] for e in ordered))} files, '
              f'{len(original)} chars)')
        if tail:
            print(f'      trailing bytes not accounted for: {tail}')
        if not ok:
            bad += 1
    return 1 if bad else 0


# ------------------------------------------------------------------ tables

def do_tables(maps):
    for m in maps:
        doc, topic = m['doc'], m['topic']
        text = read_lf(os.path.join(DOCS, doc))
        if text.startswith('# ') and ' — SPLIT' in text.split('\n', 1)[0]:
            text = read_lf(os.path.join(DOCS, 'archive', doc)).split('\n', 1)[1]
        ordered = resolve_groups(text, m['groups'])
        nb = len(text.encode('utf-8'))
        print(f'\n### `docs/{doc}` → `docs/cards/{topic}/` '
              f'({nb // 1024} KB, {len(ordered)} slices, '
              f'{len(set(e["target"] for e in ordered))} files)\n')
        print('| sections | card id | status | target file |')
        print('| --- | --- | --- | --- |')
        for e in ordered:
            labs, seen = [], set()
            for h in e['headings']:
                lab = sec_label(h)
                if lab and lab not in seen:
                    seen.add(lab)
                    labs.append(lab)
            if e['anchor'] == '__preamble__':
                labs.insert(0, '(preamble)')
            if len(labs) > 2:
                rng = f'{labs[0]}–{labs[-1]}'
            elif labs:
                rng = ', '.join(labs)
            else:
                first = e['headings'][0].lstrip('# ').strip()
                n = len(e['headings'])
                rng = (first[:46] + ('…' if len(first) > 46 else '') +
                       (f' … ({n} rows)' if n > 1 else ''))
                rng = rng.replace('|', '\\|')
            print(f"| {rng} | {e['id']} | {e['status']} | "
                  f"`docs/{e['target']}` |")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--sizes', action='store_true')
    ap.add_argument('--verify', action='store_true')
    ap.add_argument('--tables', action='store_true')
    ap.add_argument('--only', default=None)
    a = ap.parse_args()
    if not (a.build or a.sizes or a.verify or a.tables):
        ap.error('pick one of --build / --sizes / --verify / --tables')
    maps = load_maps(a.only)
    rc = 0
    if a.sizes:
        rc |= do_build(maps, dry=True)
    if a.build:
        rc |= do_build(maps, dry=False)
    if a.verify:
        rc |= do_verify(maps)
    if a.tables:
        rc |= do_tables(maps)
    return rc


if __name__ == '__main__':
    sys.exit(main())
