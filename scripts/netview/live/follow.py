"""The live monitor's source that follows a running engine through its live record (logs/network_live.jsonl).

The engine writes the record when network.trace_live_record is on (network/diag/LiveRecord.hpp): a header line with
the receivers, then one JSON line per epoch, flushed at each line, so the file is complete up to its last epoch while
the engine runs.  This source reads the lines as the file grows and turns each into the page's record, the record the
replay source makes from a finished run's blocks (telemetry.py):

- receivers and satellites become indices; the events are classified by tier with the dashboard's rules
  (nv_build.event_family, nv_build.event_lane) and counted per receiver and satellite (cycle slips, quality tests,
  alarms), so the classification has one owner;
- the products' discontinuity counters become jumps (the counter's increase since the satellite's previous epoch);
- with the integer fixing armed (header fields fix_links and fix_held), a receiver's or satellite's held share is its
  held links over its links, and each family's two counts fill the page's held tiles, as the replay source makes them
  from the chain-frame and ring-fix traces.

Satellites, families, event words and dispositions are lists that only grow.  A record that brings new entries carries them in
its "add" object (with fresh colour ranges every RANGE_EVERY records); the page appends the entries it does not have,
so every index stays valid and the page never reloads.  A snapshot carries the whole lists.

A second line of an epoch (a note after the epoch's line, which no writer makes today) is merged into that epoch's
record while the record waits to be released; after release it is counted in `late_lines` and not shown.
"""
from __future__ import annotations

import collections
import json
import re
import threading
from pathlib import Path

import numpy as np

import nv_build
from live.telemetry import NET_FIELDS, QUIET_ACTIONS, SLIP_TIERS, WEEK_MS, colour_range, family_entry

#: (key, label, colour scale, unit) of the per-receiver values of the engine's record.
RX_FIELDS = [
    ('code', 'post-fit code RMS', 'viridis', 'm'), ('phase', 'post-fit phase RMS', 'viridis', 'm'),
    ('nsat', 'satellites with residuals', 'viridis', ''), ('zcode', 'normalized code residual scale', 'viridis', ''),
    ('maxnorm', 'largest normalized post-fit residual', 'viridis', ''), ('slip', 'cycle slips detected', 'count', ''),
    ('qc', 'quality-test flags', 'count', ''), ('alarm', 'alarms that acted', 'count', ''),
    ('held', 'share of its links held at whole numbers', 'viridis', ''),
    ('zwd', 'wet delay', 'vik', 'm'), ('clksd', 'receiver clock sigma', 'viridis', 'm'),
]
#: (key, label, colour scale, unit) of the per-satellite values of the engine's record.
SAT_FIELDS = [
    ('code', 'post-fit code RMS', 'viridis', 'm'), ('nrx', 'receivers with residuals', 'viridis', ''),
    ('jumps', 'declared discontinuities', 'count', ''), ('held', 'share of its links held', 'viridis', ''),
    ('slip', 'cycle slips detected', 'count', ''), ('clk', 'emitted clock', 'vik', 'm'),
]
#: The engine's per-receiver values (header "rx_fields") that fill RX_FIELDS, by key ('held' is a share of two).
RX_FROM_ENGINE = {'code': 'code_rms_m', 'phase': 'phase_rms_m', 'nsat': 'n_sats', 'zcode': 'code_scale',
                  'maxnorm': 'max_norm', 'zwd': 'zwd_m', 'clksd': 'clk_sd_m'}
SAT_FROM_ENGINE = {'code': 'code_rms_m', 'nrx': 'n_rx', 'clk': 'clk_m'}
DIGITS_RX = {'code': 4, 'phase': 5, 'nsat': 0, 'zcode': 3, 'maxnorm': 2, 'slip': 0, 'qc': 0, 'alarm': 0, 'held': 3,
             'zwd': 4, 'clksd': 4}
DIGITS_SAT = {'code': 4, 'nrx': 0, 'jumps': 0, 'held': 3, 'slip': 0, 'clk': 4}
RANGE_EVERY = 20          # records between colour-range refreshes
RANGE_DEPTH = 200000      # values per field the colour ranges look back over
SAT_ID = re.compile(r'^[A-Z][0-9]{2}$')
FAMILY_KEYS = [k for (k, _l, _n) in nv_build.EVENT_FAMILIES]
QC_FAMILY = FAMILY_KEYS.index('qc')


def _num(v, digits):
    """A JSON-ready number rounded to `digits`, or None."""
    if v is None:
        return None
    try:
        x = float(v)
    except (TypeError, ValueError):
        return None
    if x != x or x in (float('inf'), float('-inf')):
        return None
    return round(x, digits) if digits else int(round(x))


def _held_share(eng: dict):
    """An entry's held links over its links (header fields fix_links and fix_held), or None without links."""
    links, held = eng.get('fix_links'), eng.get('fix_held')
    if not links or held is None:
        return None
    return round(float(held) / float(links), 3)


class FollowSource:
    """Page records from an engine's live record, read as the file grows (call poll() to read new lines)."""

    mode = 'engine'

    def __init__(self, path: Path, run: str | None = None):
        self.path = Path(path)
        self.run = run or (self.path.parent.parent.name if self.path.parent.name == 'logs' else self.path.stem)
        self.archive = False
        self.lock = threading.RLock()
        self._pos = 0
        self._tail = b''
        self.header = None
        self.stations: list[dict] = []
        self.sta_ix: dict[str, int] = {}
        self.rx_engine: list[str] = []
        self.sat_engine: list[str] = []
        self.sats: list[str] = []
        self.sat_ix: dict[str, int] = {}
        self.families: list[dict] = []                         # the integer fixing's families, as family_entry gives them
        self.fam_ix: dict[str, int] = {}
        self.tiers: list[str] = []
        self.names: list[str] = []
        self.actions: list[str] = []
        self.dispositions: list[str] = ['emitted']             # index 0 is what the page counts as emitted
        self._words = {'tiers': {}, 'names': {}, 'actions': {}, 'dispositions': {'emitted': 0}}
        self.records: list[dict] = []
        self.by_t: dict[tuple, int] = {}
        self.released = 0                                      # records the server has taken (merges stop there)
        self.late_lines = 0
        self.bad_lines = 0
        self.prev_disc: dict[str, int] = {}
        self.step_s = 30.0
        self._step_known = False
        self._recent = {f'rx.{k}': collections.deque(maxlen=RANGE_DEPTH) for k, *_r in RX_FIELDS}
        self._recent.update({f'sat.{k}': collections.deque(maxlen=RANGE_DEPTH) for k, *_r in SAT_FIELDS})
        self._ranges, self._scale_kinds = {}, {}
        self._pending_add: dict = {}

    # ------------------------------------------------------------------ reading

    def poll(self) -> int:
        """Reads the lines added since the last call; returns the number of new records."""
        with self.lock:
            if not self.path.exists():
                return 0
            size = self.path.stat().st_size
            if size < self._pos:                               # the file was rewritten (a new run): start over
                self.__init__(self.path, self.run)
                size = self.path.stat().st_size
            if size == self._pos:
                return 0
            with open(self.path, 'rb') as fh:
                fh.seek(self._pos)
                chunk = fh.read(size - self._pos)
            self._pos = size
            data = self._tail + chunk
            lines = data.split(b'\n')
            self._tail = lines.pop()                           # an incomplete last line waits for its newline
            before = len(self.records)
            for raw in lines:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    obj = json.loads(raw.decode('utf-8', errors='replace'))
                except ValueError:
                    self.bad_lines += 1
                    continue
                if obj.get('kind') == 'header':
                    self._header(obj)
                elif 't' in obj and self.header is not None:
                    self._line(obj)
            return len(self.records) - before

    def _header(self, h: dict):
        self.header = h
        self.stations = [{'name': s['name'], 'lat': s.get('lat'), 'lon': s.get('lon'), 'role': ''} for s in h.get('stations', [])]
        self.sta_ix = {s['name']: i for i, s in enumerate(self.stations)}
        self.rx_engine = list(h.get('rx_fields', []))
        self.sat_engine = list(h.get('sat_fields', []))
        self._pending_add['stations'] = self.stations          # a page opened before the header learns them here

    def _word(self, table: str, s: str) -> int:
        """Index of `s` in an append-only list, appending it (and noting the addition) when new."""
        ix = self._words[table]
        v = ix.get(s)
        if v is None:
            v = ix[s] = len(ix)
            getattr(self, table).append(s)
            self._pending_add.setdefault(table, []).append(s)
        return v

    def _sat(self, s: str) -> int:
        if not s or not SAT_ID.match(s):
            return -1
        v = self.sat_ix.get(s)
        if v is None:
            v = self.sat_ix[s] = len(self.sats)
            self.sats.append(s)
            self._pending_add.setdefault('sats', []).append(s)
        return v

    def _family(self, key: str) -> int:
        v = self.fam_ix.get(key)
        if v is None:
            v = self.fam_ix[key] = len(self.families)
            entry = family_entry(key)
            self.families.append(entry)
            self._pending_add.setdefault('families', []).append(entry)
        return v

    def _line(self, obj: dict):
        week, tow = int(obj['t'][0]), float(obj['t'][1])
        key = (week, round(tow, 3))
        k = self.by_t.get(key)
        if k is not None:                                      # a second line of an epoch
            if k < self.released:
                self.late_lines += 1
                return
            rec = self.records[k] = self._record(obj, self.records[k])
        else:
            ms = week * WEEK_MS + int(round(tow * 1000.0))
            if not self._step_known and self.records:
                d = (ms - self.records[-1]['ms']) / 1000.0
                if d > 0:
                    self.step_s, self._step_known = d, True
                    self._pending_add['step_s'] = d
            rec = self._record(obj, None, e=len(self.records), ms=ms)
            self.by_t[key] = len(self.records)
            self.records.append(rec)
            if len(self.records) % RANGE_EVERY == 1:
                self._refresh_ranges()
                self._pending_add['ranges'] = self._ranges
                self._pending_add['scale_kinds'] = self._scale_kinds
        if self._pending_add:
            add = rec.setdefault('add', {})
            for name, v in self._pending_add.items():
                if isinstance(v, list):
                    add.setdefault(name, []).extend(v)
                else:
                    add[name] = v
            self._pending_add = {}

    def _record(self, obj: dict, old: dict | None, e: int = 0, ms: int = 0) -> dict:
        """The page record of an engine line (merged into `old`, the epoch's earlier record, when given)."""
        ns = len(self.stations)
        # events first: their counts fill the receivers' and satellites' count fields
        ev = list(old['ev']) if old else []
        counts = {'slip': ({}, {}), 'qc': ({}, {}), 'alarm': ({}, {})}
        for row in obj.get('ev', []) or []:
            tier, name, sat, sta, action, detail = (str(x) if x is not None else '' for x in row[:6])
            lane = nv_build.LANE_INDEX[nv_build.event_lane(tier, name, action)]
            fam = nv_build.event_family(tier)
            j, s = self.sta_ix.get(sta, -1), self._sat(sat)
            ev.append([lane, fam, self._word('tiers', tier), self._word('names', name), self._word('actions', action),
                       j, s, detail[:160]])
        for x in ev:
            tier, action = self.tiers[x[2]], self.actions[x[4]]
            is_alarm = nv_build.LANES[x[0]][0] == 'alarm'
            kinds = []
            if tier in SLIP_TIERS and action not in QUIET_ACTIONS:
                kinds.append('slip')
            if is_alarm:
                kinds.append('alarm')
            elif x[1] == QC_FAMILY and action not in QUIET_ACTIONS:
                kinds.append('qc')
            for kind in kinds:
                rxc, satc = counts[kind]
                if x[5] >= 0:
                    rxc[x[5]] = rxc.get(x[5], 0) + 1
                if x[6] >= 0:
                    satc[x[6]] = satc.get(x[6], 0) + 1

        # the satellites of the line (the residual and product values)
        sat_vals = {}
        for sat, vals in (obj.get('sat') or {}).items():
            s = self._sat(sat)
            if s < 0:
                continue
            row = dict(zip(self.sat_engine, vals))
            disc = row.get('disc')
            jumps = None
            if disc is not None:
                prev = self.prev_disc.get(sat)
                jumps = max(0, int(disc) - prev) if prev is not None else 0
                self.prev_disc[sat] = int(disc)
            sat_vals[s] = (row, jumps)

        # the network values
        if old:
            net = list(old['net'])
        else:
            net = [None] * len(NET_FIELDS)
        nf = [f[0] for f in NET_FIELDS]

        def put(key, v, digits=4):
            if v is not None:
                net[nf.index(key)] = _num(v, digits)

        if obj.get('disp'):
            net[nf.index('disp')] = self._word('dispositions', str(obj['disp']))
        s0 = obj.get('s0') or [None, None]
        put('s0c', s0[0])
        put('s0p', s0[1])
        put('npres', obj.get('npres'), 0)
        put('nclk', obj.get('nclk'), 0)
        user = obj.get('user')
        if user:
            for key, v in zip(('user3d', 'ue', 'un', 'uu'), user):
                put(key, v)
        put('msep', obj.get('wall_ms'), 1)

        # the receivers: every header station has a row (counts 0, values null when it was not in the solve)
        rx_in = obj.get('rx') or []
        rx = []
        for j in range(ns):
            eng = dict(zip(self.rx_engine, rx_in[j])) if j < len(rx_in) and rx_in[j] is not None else None
            base = old['rx'][j] if old else None
            row = []
            for i, (k, *_r) in enumerate(RX_FIELDS):
                if k in counts:
                    row.append(counts[k][0].get(j, 0))
                elif eng is not None:
                    row.append(_held_share(eng) if k == 'held' else _num(eng.get(RX_FROM_ENGINE[k]), DIGITS_RX[k]))
                else:
                    row.append(base[i] if base else None)
            rx.append(row)
            if eng is not None:
                for i, (k, *_r) in enumerate(RX_FIELDS):
                    if row[i] is not None:
                        self._recent[f'rx.{k}'].append(row[i])

        # the satellites: every satellite known so far has a row (the list may have grown with this line)
        sat = []
        for s in range(len(self.sats)):
            got = sat_vals.get(s)
            base = old['sat'][s] if old and s < len(old['sat']) else None
            row = []
            for i, (k, *_r) in enumerate(SAT_FIELDS):
                if k == 'slip':
                    row.append(counts['slip'][1].get(s, 0))
                elif k == 'jumps':
                    row.append(got[1] if got else (base[i] if base else None))
                elif got is not None:
                    row.append(_held_share(got[0]) if k == 'held' else _num(got[0].get(SAT_FROM_ENGINE[k]), DIGITS_SAT[k]))
                else:
                    row.append(base[i] if base else None)
            sat.append(row)
            if got is not None:
                for i, (k, *_r) in enumerate(SAT_FIELDS):
                    if row[i] is not None:
                        self._recent[f'sat.{k}'].append(row[i])
        # the integer fixing's families: [links, held] in the order of the family list (None where not counted)
        fam_in = obj.get('fam') or {}
        if fam_in:
            for key in fam_in:
                self._family(key)
            fam = [None] * len(self.families)
            for key, v in fam_in.items():
                fam[self.fam_ix[key]] = [int(v[0]), int(v[1])]
        else:
            fam = list(old['fam']) if old else []
        rec = {'e': old['e'] if old else e, 'ms': old['ms'] if old else ms, 'net': net, 'fam': fam, 'rx': rx, 'sat': sat,
               'ev': ev}
        if old and 'add' in old:
            rec['add'] = old['add']
        return rec

    def _refresh_ranges(self):
        ranges, kinds = {}, {}
        for tag, fields in (('rx', RX_FIELDS), ('sat', SAT_FIELDS)):
            for k, _l, kind, _u in fields:
                v = np.asarray(self._recent[f'{tag}.{k}'], np.float64)
                r, sk = colour_range(k, kind, v[np.isfinite(v)])
                ranges[f'{tag}.{k}'] = r
                if sk:
                    kinds[f'{tag}.{k}'] = sk
        self._ranges, self._scale_kinds = ranges, kinds

    # ------------------------------------------------------------------ what the server reads

    @property
    def n_ep(self) -> int:
        return len(self.records)

    def record(self, e: int) -> dict:
        with self.lock:
            self.released = max(self.released, e + 1)
            return self.records[e]

    def lists(self) -> dict:
        """The append-only lists and the current colour ranges (a snapshot's 'lists')."""
        with self.lock:
            return {'stations': self.stations, 'sats': list(self.sats), 'families': list(self.families),
                    'tiers': list(self.tiers), 'names': list(self.names),
                    'actions': list(self.actions), 'dispositions': list(self.dispositions),
                    'ranges': self._ranges, 'scale_kinds': self._scale_kinds, 'step_s': self.step_s}

    def meta(self, window: int) -> dict:
        with self.lock:
            if not self._ranges:
                self._refresh_ranges()
            return {
                'run': self.run, 'mode': self.mode, 'n_ep': None, 'step_s': self.step_s, 'window': int(window),
                'source': str(self.path), 'stations': self.stations, 'sats': list(self.sats),
                'families': list(self.families),
                'rx_fields': [list(f) for f in RX_FIELDS], 'sat_fields': [list(f) for f in SAT_FIELDS],
                'net_fields': [list(f) for f in NET_FIELDS], 'dispositions': list(self.dispositions),
                'events': {'tiers': list(self.tiers), 'names': list(self.names), 'actions': list(self.actions),
                           'families': [[k, l] for (k, l, _n) in nv_build.EVENT_FAMILIES], 'tier_family': []},
                'lanes': [{'key': k, 'label': l} for (k, l) in nv_build.LANES], 'archive': False,
                'ranges': self._ranges, 'scale_kinds': self._scale_kinds,
            }
