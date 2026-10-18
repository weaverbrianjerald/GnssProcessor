#!/usr/bin/env python3
"""rinex_edit.py: decimate and merge RINEX observation files (RINEX 2.x, 3.x, 4.x).

Two operations, usable together or alone:

1. Decimate (--rate S): keep only the observation epochs whose time is a multiple of
   S seconds on the absolute GPS-time grid (seconds-of-day for any S that divides one
   day), within --tol.  Aligning on the absolute grid rather than by index from each
   file's first epoch puts every station and every file on the same :00/:30 grid.

2. Merge: any number of input files (hourly sessions, for example) are ordered by
   their first epoch and written as one continuous file.  Epochs that repeat across
   files (a shared boundary epoch) are written once.

Correctness details that a line filter does not get right:

- Loss of lock is carried forward.  A phase observation whose LLI bit 0 is set at a
  dropped epoch sets LLI bit 0 on the next retained observation of the same
  satellite and phase type, so a cycle slip inside a decimation gap stays flagged.
  A flag-6 (cycle slip) record at a dropped epoch is converted the same way, and an
  epoch flag 1 (power failure) at a dropped epoch moves to the next retained epoch.
- Header-information event records (flag 4) lose their per-file statistics
  ("PRN / # OF OBS", "# OF SATELLITES"), which are wrong after decimation and
  merging.  A flag-4 record left holding only COMMENT lines is dropped.  Flag 2, 3
  and 5 records are kept as written.
- The output header is the first file's header with TIME OF FIRST OBS, TIME OF LAST
  OBS and INTERVAL rewritten from the retained epochs, "# OF SATELLITES" and
  "PRN / # OF OBS" recomputed (only when the first header carried them), and COMMENT
  lines recording the edit.
- Inputs must share the RINEX version.  Observation-type lists may differ (an
  hourly file with no QZSS in view declares no "J" list, for example): the output
  declares the per-system union in first-seen order and each file's observations
  are remapped onto it, blank where a file lacks a type.  SYS / PHASE SHIFT records
  follow the same union.  Differences in marker, receiver, antenna or GLONASS slot
  records are reported as warnings (the first file's records are written).

Inputs may be files, glob patterns or directories (a directory contributes every
observation file in it: *.YYo, *.YYd, *_?O.rnx, *_?O.crx, optionally .gz / .Z).
Compressed and Hatanaka inputs are read through the `hatanaka` package
(requirements.txt).  The output is plain RINEX with LF line endings.

Default output name: short names ssssDDDh.YYo become ssssDDD0.YYo (the daily
session character); long names get the merged span and the new rate
(..._01D_30S_MO.rnx).  The default directory is that of the first input.

Examples:
  python scripts/rinex_edit.py D:/data/jul20_26o --rate 30
  python scripts/rinex_edit.py "D:/data/seas202?.26o" --rate 30 -o D:/out/seas2020.26o
  python scripts/rinex_edit.py a.26o b.26o                    # merge only
  python scripts/rinex_edit.py seas202a.26o --rate 15         # decimate only
"""
from __future__ import annotations

import argparse
import glob
import gzip
import io
import math
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rinex_ids import norm_sat  # noqa: E402

GPS_EPOCH = datetime(1980, 1, 6)
TOOL = 'rinex_edit.py'

OBS_FILE_RE = re.compile(
    r'(\.\d\d[oOdD]|_[A-Z]O\.(rnx|crx)|\.(rnx|crx))(\.gz|\.Z|\.z)?$', re.IGNORECASE)
SHORT_NAME_RE = re.compile(r'^([A-Za-z0-9]{4})(\d{3})([a-xA-X0-9])\.(\d\d)([oOdD])(\.gz|\.Z)?$')
LONG_NAME_RE = re.compile(
    r'^(\w{9})_([RSU])_(\d{4})(\d{3})(\d{2})(\d{2})_(\d\d[MHDU])_(\d\d[CZSMHDU])_([A-Z]O)'
    r'\.(rnx|crx)(\.gz)?$')


# --------------------------------------------------------------------------- I/O

def open_text(path: str):
    """Return a text stream over a RINEX file, decompressing when needed."""
    low = path.lower()
    hatanaka_like = low.endswith(('.crx', '.crx.gz', '.z')) or re.search(r'\.\d\dd(\.gz|\.z)?$', low)
    if hatanaka_like or low.endswith('.gz'):
        try:
            import hatanaka
        except ImportError:
            if hatanaka_like:
                sys.exit(f'ERROR: {path} needs the `hatanaka` package (pip install hatanaka)')
            return io.TextIOWrapper(gzip.open(path, 'rb'), encoding='ascii', errors='replace')
        data = hatanaka.decompress(Path(path))
        return io.StringIO(data.decode('ascii', errors='replace'))
    return open(path, 'r', encoding='ascii', errors='replace')


def label_of(line: str) -> str:
    return line[60:].strip() if len(line) > 60 else ''


def with_label(content: str, label: str) -> str:
    return f'{content[:60]:<60}{label}'


# ------------------------------------------------------------------------ header

class Header:
    def __init__(self, path: str, lines: list[str]):
        self.path = path
        self.lines = lines                      # every header line except END OF HEADER
        first = lines[0] if lines else ''
        if label_of(first) != 'RINEX VERSION / TYPE':
            sys.exit(f'ERROR: {path}: first line is not RINEX VERSION / TYPE')
        self.version = float(first[:9])
        if first[20:21].upper() != 'O':
            sys.exit(f'ERROR: {path}: not an observation file (type "{first[20:21]}")')
        self.major = int(self.version)
        self.obs_types = self._parse_obs_types()
        self.time_system = ''
        self.interval = None
        for ln in lines:
            lab = label_of(ln)
            if lab == 'TIME OF FIRST OBS':
                self.time_system = ln[48:51].strip()
            elif lab == 'INTERVAL':
                try:
                    self.interval = float(ln[:10])
                except ValueError:
                    pass

    def _parse_obs_types(self):
        """RINEX 3/4: {sys: [types]}.  RINEX 2: {'*': [types]} (one list for every system)."""
        types: dict[str, list[str]] = {}
        if self.major >= 3:
            cur = None
            for ln in self.lines:
                if label_of(ln) != 'SYS / # / OBS TYPES':
                    continue
                if ln[0] != ' ':
                    cur = ln[0]
                    types[cur] = []
                for i in range(13):
                    t = ln[7 + 4 * i:10 + 4 * i].strip()
                    if t:
                        types[cur].append(t)
        else:
            lst: list[str] = []
            for ln in self.lines:
                if label_of(ln) == '# / TYPES OF OBSERV':
                    for i in range(9):
                        t = ln[10 + 6 * i:12 + 6 * i].strip()
                        if t:
                            lst.append(t)
            types['*'] = lst
        return types

    def types_for(self, sys_char: str) -> list[str]:
        if self.major >= 3:
            return self.obs_types.get(sys_char, [])
        return self.obs_types['*']

    def has(self, label: str) -> bool:
        return any(label_of(ln) == label for ln in self.lines)

    def record(self, label: str) -> list[str]:
        return [ln[:60].rstrip() for ln in self.lines if label_of(ln) == label]


def read_header(fh, path: str) -> Header:
    lines = []
    for raw in fh:
        ln = raw.rstrip('\r\n')
        if label_of(ln) == 'END OF HEADER':
            return Header(path, lines)
        lines.append(ln)
    sys.exit(f'ERROR: {path}: no END OF HEADER')


def union_obs_types(headers: list[Header]) -> dict[str, list[str]]:
    """Per-system union of the observation types, in first-seen order."""
    out: dict[str, list[str]] = {}
    for h in headers:
        for sys_char, types in h.obs_types.items():
            lst = out.setdefault(sys_char, [])
            lst.extend(t for t in types if t not in lst)
    return out


def obs_type_lines(major: int, types: dict[str, list[str]]) -> list[str]:
    lines = []
    if major >= 3:
        for sys_char, lst in types.items():
            for k in range(0, max(1, len(lst)), 13):
                lead = f'{sys_char}  {len(lst):3d}' if k == 0 else '      '
                lines.append(with_label(lead + ''.join(f' {t:<3}' for t in lst[k:k + 13]),
                                        'SYS / # / OBS TYPES'))
    else:
        lst = types['*']
        for k in range(0, max(1, len(lst)), 9):
            lead = f'{len(lst):6d}' if k == 0 else '      '
            lines.append(with_label(lead + ''.join(f'    {t:<2}' for t in lst[k:k + 9]),
                                    '# / TYPES OF OBSERV'))
    return lines


# ----------------------------------------------------------------------- records

class Record:
    """One epoch record: the epoch line(s) plus either satellite blocks or header lines."""
    __slots__ = ('epoch_lines', 'flag', 't', 'tfields', 'sats', 'body')

    def __init__(self):
        self.epoch_lines: list[str] = []
        self.flag = 0
        self.t = None          # seconds since the GPS epoch, None when the record has no time
        self.tfields = None    # (y, mo, d, h, mi, sec)
        self.sats: list[tuple[str, list[str]]] = []   # (sat id, its observation lines)
        self.body: list[str] = []                     # event records: the header lines

    @property
    def is_obs(self):
        return self.flag in (0, 1, 6)

    def lines(self):
        out = list(self.epoch_lines)
        if self.is_obs:
            for _, sl in self.sats:
                out.extend(sl)
        else:
            out.extend(self.body)
        return out


def _time(y, mo, d, h, mi, sec):
    return ((datetime(y, mo, d, h, mi) - GPS_EPOCH).total_seconds() + sec,
            (y, mo, d, h, mi, sec))


def _int(s, default=0):
    s = s.strip()
    return int(s) if s else default


def iter_records(fh, hdr: Header):
    """Yield Record objects from a stream positioned just after END OF HEADER."""
    v3 = hdr.major >= 3
    it = (raw.rstrip('\r\n') for raw in fh)
    for line in it:
        if not line.strip():
            continue
        rec = Record()
        rec.epoch_lines.append(line)
        if v3:
            if not line.startswith('>'):
                raise ValueError(f'{hdr.path}: expected an epoch line, got: {line[:40]!r}')
            rec.flag = _int(line[31:32])
            n = _int(line[32:35])
            if line[2:29].strip():
                rec.t, rec.tfields = _time(int(line[2:6]), int(line[7:9]), int(line[10:12]),
                                           int(line[13:15]), int(line[16:18]), float(line[18:29]))
            if rec.is_obs:
                for _ in range(n):
                    sl = next(it)
                    rec.sats.append((norm_sat(sl[:3]), [sl]))
            else:
                rec.body = [next(it) for _ in range(n)]
        else:
            rec.flag = _int(line[28:29])
            n = _int(line[29:32])
            if line[1:26].strip():
                yy = int(line[1:3])
                rec.t, rec.tfields = _time(yy + (1900 if yy >= 80 else 2000), int(line[4:6]),
                                           int(line[7:9]), int(line[10:12]), int(line[13:15]),
                                           float(line[15:26]))
            if rec.is_obs:
                ids = [line[32 + 3 * i:35 + 3 * i] for i in range(min(n, 12))]
                while len(ids) < n:
                    cont = next(it)
                    rec.epoch_lines.append(cont)
                    ids.extend(cont[32 + 3 * i:35 + 3 * i] for i in range(min(n - len(ids), 12)))
                per_sat = max(1, math.ceil(len(hdr.types_for('*')) / 5))
                for sid in ids:
                    sid = ('G' + sid[1:]) if sid[0] == ' ' else sid
                    rec.sats.append((sid.replace(' ', '0'), [next(it) for _ in range(per_sat)]))
            else:
                rec.body = [next(it) for _ in range(n)]
        yield rec


# ----------------------------------------------- per-observation field access

def _field_pos(v3: bool, i: int):
    """(line index, column) of observation i's 14-char value field."""
    if v3:
        return 0, 3 + 16 * i
    return i // 5, (i % 5) * 16


def remap_obs(v3, lines, src_to_dst, n_dst):
    """Rewrite one satellite's observation lines from a file's type order to the output's."""
    n_src = len(src_to_dst)
    if v3:
        ln = lines[0]
        fields = [ln[3 + 16 * i:19 + 16 * i] for i in range(n_src)]
    else:
        padded = [ln.ljust(80) for ln in lines]
        fields = [padded[i // 5][(i % 5) * 16:(i % 5) * 16 + 16] if i // 5 < len(padded) else ''
                  for i in range(n_src)]
    out = [' ' * 16] * n_dst
    for i, j in enumerate(src_to_dst):
        out[j] = fields[i].ljust(16)
    if v3:
        return [(lines[0][:3] + ''.join(out)).rstrip()]
    return [''.join(out[k:k + 5]).rstrip() for k in range(0, n_dst, 5)] or ['']


def obs_present(v3, lines, i):
    li, col = _field_pos(v3, i)
    return li < len(lines) and lines[li][col:col + 14].strip() != ''


def obs_lli(v3, lines, i):
    li, col = _field_pos(v3, i)
    c = lines[li][col + 14:col + 15] if li < len(lines) else ''
    return int(c) if c.isdigit() else 0


def set_lli_bit0(v3, lines, i):
    li, col = _field_pos(v3, i)
    ln = lines[li]
    pos = col + 14
    if len(ln) <= pos:
        ln = ln.ljust(pos + 1)
    c = ln[pos]
    val = (int(c) if c.isdigit() else 0) | 1
    lines[li] = ln[:pos] + str(val) + ln[pos + 1:]


def set_epoch_flag(v3, line, flag):
    pos = 31 if v3 else 28
    line = line.ljust(pos + 1)
    return line[:pos] + str(flag) + line[pos + 1:]


def set_epoch_count(v3, line, n):
    a, b = (32, 35) if v3 else (29, 32)
    line = line.ljust(b)
    return line[:a] + f'{n:3d}' + line[b:]


# ---------------------------------------------------------------------- editing

class Editor:
    def __init__(self, hdr: Header, out_types: dict[str, list[str]], rate: float | None,
                 tol: float):
        self.hdr = hdr
        self.out_types = out_types
        self.v3 = hdr.major >= 3
        self.remap: dict[str, list[int] | None] = {}
        self.rate = rate
        self.tol = tol
        self.out: list[str] = []
        self.last_seen_t = -math.inf       # latest observation epoch read (retained or not)
        self.first_kept = None
        self.last_kept = None
        self.pending_lli: set[tuple[str, int]] = set()
        self.pending_power = False
        self.counts: dict[str, list[int]] = {}
        self.stats = dict(kept=0, dropped=0, duplicate=0, lli_carried=0, power_carried=0,
                          slip_records_converted=0, flag4_dropped=0, flag4_trimmed=0,
                          events_kept=0, remapped=0)
        self._phase_idx_cache: dict[str, list[int]] = {}

    def types_for(self, sat):
        return self.out_types.get(sat[0] if self.v3 else '*', [])

    def set_source(self, src: Header):
        """Prepare the type remap for the next input file (None = already in output order)."""
        self.remap = {}
        for sys_char, lst in src.obs_types.items():
            dst = self.out_types[sys_char]
            self.remap[sys_char] = None if lst == dst else [dst.index(t) for t in lst]

    def phase_indices(self, sat):
        key = sat[0] if self.v3 else '*'
        idx = self._phase_idx_cache.get(key)
        if idx is None:
            idx = [i for i, t in enumerate(self.out_types.get(key, [])) if t.startswith('L')]
            self._phase_idx_cache[key] = idx
        return idx

    def aligned(self, t):
        if self.rate is None:
            return True
        r = math.fmod(t, self.rate)
        return r <= self.tol or self.rate - r <= self.tol

    def harvest(self, rec: Record, slip_record=False):
        """Remember loss-of-lock at an epoch that will not be written."""
        for sat, lines in rec.sats:
            for i in self.phase_indices(sat):
                if slip_record:
                    if obs_present(self.v3, lines, i):
                        self.pending_lli.add((sat, i))
                elif obs_lli(self.v3, lines, i) & 1:
                    self.pending_lli.add((sat, i))
        if rec.flag == 1:
            self.pending_power = True

    def apply_pending(self, rec: Record):
        if self.pending_lli:
            for sat, lines in rec.sats:
                for i in self.phase_indices(sat):
                    if (sat, i) in self.pending_lli and obs_present(self.v3, lines, i):
                        if not obs_lli(self.v3, lines, i) & 1:
                            set_lli_bit0(self.v3, lines, i)
                            self.stats['lli_carried'] += 1
                        self.pending_lli.discard((sat, i))
        if self.pending_power:
            if rec.flag == 0:
                rec.epoch_lines[0] = set_epoch_flag(self.v3, rec.epoch_lines[0], 1)
                rec.flag = 1
                self.stats['power_carried'] += 1
            self.pending_power = False

    def count(self, rec: Record):
        for sat, lines in rec.sats:
            ntypes = len(self.types_for(sat))
            c = self.counts.get(sat)
            if c is None:
                c = self.counts[sat] = [0] * ntypes
            for i in range(ntypes):
                if obs_present(self.v3, lines, i):
                    c[i] += 1

    def feed(self, rec: Record):
        if rec.is_obs and self.remap:
            for k, (sat, lines) in enumerate(rec.sats):
                m = self.remap.get(sat[0] if self.v3 else '*')
                if m is not None:
                    rec.sats[k] = (sat, remap_obs(self.v3, lines, m, len(self.types_for(sat))))
                    self.stats['remapped'] += 1
        if rec.flag in (0, 1):
            if rec.t is None:
                raise ValueError(f'{self.hdr.path}: observation epoch without a time')
            if rec.t <= self.last_seen_t + 1e-6:
                self.stats['duplicate'] += 1
                return
            self.last_seen_t = rec.t
            if not self.aligned(rec.t):
                self.harvest(rec)
                self.stats['dropped'] += 1
                return
            self.apply_pending(rec)
            self.count(rec)
            self.out.extend(rec.lines())
            self.stats['kept'] += 1
            if self.first_kept is None:
                self.first_kept = rec.tfields
            self.last_kept = rec.tfields
        elif rec.flag == 6:
            if rec.t is not None and self.aligned(rec.t):
                self.out.extend(rec.lines())
                self.stats['events_kept'] += 1
            else:
                self.harvest(rec, slip_record=True)
                self.stats['slip_records_converted'] += 1
        elif rec.flag == 4:
            body = [ln for ln in rec.body
                    if label_of(ln) not in ('PRN / # OF OBS', '# OF SATELLITES')]
            if all(label_of(ln) == 'COMMENT' for ln in body):
                self.stats['flag4_dropped'] += 1
                return
            if len(body) != len(rec.body):
                self.stats['flag4_trimmed'] += 1
            self.out.append(set_epoch_count(self.v3, rec.epoch_lines[0], len(body)))
            self.out.extend(body)
            self.stats['events_kept'] += 1
        else:                       # 2 start moving, 3 new site, 5 external event
            self.out.extend(rec.lines())
            self.stats['events_kept'] += 1

    # ------------------------------------------------------------- output header

    def build_header(self, headers: list[Header], src_interval: float | None) -> list[str]:
        h = self.hdr
        n_files = len(headers)
        type_label = 'SYS / # / OBS TYPES' if self.v3 else '# / TYPES OF OBSERV'
        shifts, seen_shift = [], set()
        for hh in headers:
            for ln in hh.lines:
                if label_of(ln) == 'SYS / PHASE SHIFT' and ln[:60].rstrip() not in seen_shift:
                    seen_shift.add(ln[:60].rstrip())
                    shifts.append(ln)
        fmt_time = lambda f: (f'{f[0]:6d}{f[1]:6d}{f[2]:6d}{f[3]:6d}{f[4]:6d}{f[5]:13.7f}'
                              f'     {h.time_system:<3}')
        drop = {'# OF SATELLITES', 'PRN / # OF OBS', 'TIME OF FIRST OBS', 'TIME OF LAST OBS',
                'INTERVAL'}
        had_stats = h.has('PRN / # OF OBS')
        had_last = h.has('TIME OF LAST OBS')
        interval = self.rate if self.rate is not None else (h.interval or src_interval)

        now = datetime.now(timezone.utc).strftime('%Y%m%d %H%M%S UTC')
        notes = [f'Edited by {TOOL} {now}']
        if self.rate is not None:
            src = f'{src_interval:.3f} s' if src_interval else 'native rate'
            notes.append(f'Decimated from {src} to {self.rate:.3f} s')
        if n_files > 1:
            notes.append(f'Merged from {n_files} files')
        if self.stats['lli_carried'] or self.stats['power_carried']:
            notes.append(f'LLI carried forward from dropped epochs: {self.stats["lli_carried"]}')

        out: list[str] = []
        placed_notes = placed_types = placed_shifts = False
        for ln in h.lines:
            lab = label_of(ln)
            if lab == type_label:
                if not placed_types:
                    out.extend(obs_type_lines(h.major, self.out_types))
                    placed_types = True
                continue
            if lab == 'SYS / PHASE SHIFT':
                if not placed_shifts:
                    out.extend(shifts)
                    placed_shifts = True
                continue
            if lab in drop:
                if lab == 'TIME OF FIRST OBS':
                    if interval:
                        out.append(with_label(f'{interval:10.3f}', 'INTERVAL'))
                    out.append(with_label(fmt_time(self.first_kept), 'TIME OF FIRST OBS'))
                    if had_last:
                        out.append(with_label(fmt_time(self.last_kept), 'TIME OF LAST OBS'))
                continue
            out.append(ln)
            if lab == 'PGM / RUN BY / DATE' and not placed_notes:
                out.extend(with_label(n, 'COMMENT') for n in notes)
                placed_notes = True
        if not placed_notes:
            out[1:1] = [with_label(n, 'COMMENT') for n in notes]
        if not h.has('TIME OF FIRST OBS'):
            sys.exit('ERROR: first header has no TIME OF FIRST OBS')

        if had_stats:
            order = list(self.out_types) if self.v3 else None
            def sort_key(s):
                sys_rank = order.index(s[0]) if order and s[0] in order else 99
                return (sys_rank, s)
            sats = sorted(self.counts, key=sort_key)
            out.append(with_label(f'{len(sats):6d}', '# OF SATELLITES'))
            for s in sats:
                c = self.counts[s]
                for k in range(0, max(1, len(c)), 9):
                    lead = f'   {s}' if k == 0 else '      '
                    out.append(with_label(lead + ''.join(f'{v:6d}' for v in c[k:k + 9]),
                                          'PRN / # OF OBS'))
        out.append(with_label('', 'END OF HEADER'))
        return out


# -------------------------------------------------------------------- utilities

def written_by_this_tool(path: str) -> bool:
    with open_text(path) as fh:
        for raw in fh:
            if label_of(raw) == 'END OF HEADER':
                return False
            if label_of(raw) == 'COMMENT' and raw.startswith(f'Edited by {TOOL}'):
                return True
    return False


def expand_inputs(items: list[str]) -> list[str]:
    """Files are taken as given; directory and glob matches skip this tool's own outputs."""
    files: list[str] = []
    for it in items:
        if os.path.isdir(it) or any(ch in it for ch in '*?['):
            if os.path.isdir(it):
                found = [os.path.join(it, f) for f in sorted(os.listdir(it))
                         if OBS_FILE_RE.search(f) and os.path.isfile(os.path.join(it, f))]
            else:
                found = sorted(glob.glob(it))
            for f in found:
                if written_by_this_tool(f):
                    print(f'  skip {os.path.basename(f)} (an earlier {TOOL} output)')
                else:
                    files.append(f)
        elif os.path.isfile(it):
            files.append(it)
        else:
            sys.exit(f'ERROR: no such file or directory: {it}')
    seen, uniq = set(), []
    for f in files:
        k = os.path.normcase(os.path.abspath(f))
        if k not in seen:
            seen.add(k)
            uniq.append(f)
    return uniq


def first_epoch_time(path: str) -> float:
    with open_text(path) as fh:
        hdr = read_header(fh, path)
        for rec in iter_records(fh, hdr):
            if rec.flag in (0, 1) and rec.t is not None:
                return rec.t
    return math.inf


def default_output(first: str, span_s: float, rate: float | None) -> str:
    d, name = os.path.split(first)
    m = SHORT_NAME_RE.match(name)
    if m and span_s > 3600 + 1:
        return os.path.join(d, f'{m.group(1)}{m.group(2)}0.{m.group(4)}o')
    if m:
        return os.path.join(d, f'{m.group(1)}{m.group(2)}{m.group(3)}.{m.group(4)}o.edit')
    m = LONG_NAME_RE.match(name)
    if m:
        if span_s >= 86400 - 3600:
            period = f'{max(1, round(span_s / 86400)):02d}D'
        elif span_s >= 3600:
            period = f'{max(1, round(span_s / 3600)):02d}H'
        else:
            period = f'{max(1, round(span_s / 60)):02d}M'
        freq = m.group(8)
        if rate is not None:
            r = rate
            freq = (f'{int(r):02d}S' if r < 60 else f'{int(r // 60):02d}M') if r >= 1 \
                else f'{int(round(1 / r)):02d}Z'
        return os.path.join(d, f'{m.group(1)}_{m.group(2)}_{m.group(3)}{m.group(4)}{m.group(5)}'
                               f'{m.group(6)}_{period}_{freq}_{m.group(9)}.rnx')
    stem = re.sub(r'(\.gz|\.Z)$', '', name)
    return os.path.join(d, stem + '.edited.rnx' if stem.endswith('.rnx') else stem + '.edited')


def check_consistency(ref: Header, other: Header):
    if other.major != ref.major:
        sys.exit(f'ERROR: {other.path}: RINEX {other.version} vs {ref.version} in {ref.path}')
    for lab in ('MARKER NAME', 'REC # / TYPE / VERS', 'ANT # / TYPE', 'ANTENNA: DELTA H/E/N',
                'GLONASS SLOT / FRQ #', 'SYS / SCALE FACTOR'):
        if ref.record(lab) != other.record(lab):
            print(f'WARNING: {os.path.basename(other.path)}: "{lab}" differs from the first '
                  f'file ({other.record(lab)} vs {ref.record(lab)}); first file\'s record kept',
                  file=sys.stderr)


# ------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description='Decimate and/or merge RINEX observation files.',
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog='Examples:' + __doc__.split('Examples:')[1])
    ap.add_argument('inputs', nargs='+', help='files, glob patterns or directories')
    ap.add_argument('-r', '--rate', type=float, help='output interval in seconds (omit to merge only)')
    ap.add_argument('-o', '--output', help='output path (default: derived from the first input)')
    ap.add_argument('--tol', type=float, default=0.005,
                    help='grid tolerance in seconds for receiver clock jitter (default 0.005)')
    ap.add_argument('--force', action='store_true', help='overwrite an existing output file')
    a = ap.parse_args()

    if a.rate is not None and a.rate <= 0:
        sys.exit('ERROR: --rate must be positive')
    files = expand_inputs(a.inputs)
    if a.output:
        out_key = os.path.normcase(os.path.abspath(a.output))
        files = [f for f in files if os.path.normcase(os.path.abspath(f)) != out_key]
    if not files:
        sys.exit('ERROR: no RINEX observation files found')

    # order by first epoch
    starts = {f: first_epoch_time(f) for f in files}
    files.sort(key=lambda f: starts[f])

    headers = []
    for f in files:
        with open_text(f) as fh:
            headers.append(read_header(fh, f))
    ref = headers[0]
    for h in headers[1:]:
        check_consistency(ref, h)
    src_intervals = {h.interval for h in headers if h.interval}
    src_interval = min(src_intervals) if src_intervals else None
    if a.rate is not None and src_interval and a.rate < src_interval - 1e-9:
        print(f'WARNING: --rate {a.rate} is finer than the input interval {src_interval}',
              file=sys.stderr)
    if a.rate is not None and src_interval and \
            abs(a.rate / src_interval - round(a.rate / src_interval)) > 1e-6:
        print(f'WARNING: --rate {a.rate} is not a multiple of the input interval {src_interval}',
              file=sys.stderr)

    out_types = union_obs_types(headers)
    for h in headers:
        if h.obs_types != out_types:
            print(f'  note: {os.path.basename(h.path)} declares a subset of the merged '
                  'observation types; remapped onto the union')
    ed = Editor(ref, out_types, a.rate, a.tol)
    for f in files:
        with open_text(f) as fh:
            hdr = read_header(fh, f)
            ed.set_source(hdr)
            try:
                for rec in iter_records(fh, hdr):
                    ed.feed(rec)
            except StopIteration:
                print(f'WARNING: {f}: truncated final record ignored', file=sys.stderr)
        print(f'  read {os.path.basename(f)}  (kept so far {ed.stats["kept"]})')

    if ed.first_kept is None:
        sys.exit('ERROR: no epochs retained')

    t0 = _time(*ed.first_kept)[0]
    t1 = _time(*ed.last_kept)[0]
    out_path = a.output or default_output(files[0], t1 - t0 + (a.rate or src_interval or 0), a.rate)
    in_keys = {os.path.normcase(os.path.abspath(f)) for f in files}
    if os.path.normcase(os.path.abspath(out_path)) in in_keys:
        sys.exit(f'ERROR: output {out_path} would overwrite an input; pass -o')
    if os.path.exists(out_path) and not a.force:
        sys.exit(f'ERROR: {out_path} exists; pass --force to overwrite')

    header = ed.build_header(headers, src_interval)
    with open(out_path, 'w', encoding='ascii', newline='\n') as fo:
        fo.write('\n'.join(header))
        fo.write('\n')
        fo.write('\n'.join(ed.out))
        fo.write('\n')

    s = ed.stats
    fmt = lambda f: f'{f[0]:04d}-{f[1]:02d}-{f[2]:02d} {f[3]:02d}:{f[4]:02d}:{f[5]:06.3f}'
    print(f'\nwrote {out_path}')
    print(f'  files merged        {len(files)}')
    print(f'  epochs kept         {s["kept"]}   dropped {s["dropped"]}   duplicate {s["duplicate"]}')
    print(f'  first / last        {fmt(ed.first_kept)}  /  {fmt(ed.last_kept)}')
    print(f'  satellites          {len(ed.counts)}   satellite records remapped {s["remapped"]}')
    print(f'  LLI carried fwd     {s["lli_carried"]}   power-fail flags carried {s["power_carried"]}')
    print(f'  flag-6 converted    {s["slip_records_converted"]}   flag-4 dropped {s["flag4_dropped"]}'
          f'   flag-4 trimmed {s["flag4_trimmed"]}   events kept {s["events_kept"]}')
    if ed.pending_lli:
        print(f'  note: {len(ed.pending_lli)} loss-of-lock flag(s) after the last retained epoch '
              'had no later observation to attach to')


if __name__ == '__main__':
    main()
