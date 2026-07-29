#!/usr/bin/env python3
"""Characterize a network null-space "storm": decompose the below-cutoff
near-null directions of the shared border by CLASS, constellation and culprit
satellite, so a rank-gate/datum change is judged by what it moved rather than by
a raw count.

The producer's null-space rank gate withholds a whole epoch when the number of
(relatively) zero eigenvalues of the reduced shared normals differs from the
declared S-basis dimension.  When that fires, the raw count is nearly useless on
its own: the below-cutoff population is FOUR different things with FOUR different
owners, and two of them move in opposite directions under the same change.

  declared clock gauges     pure CLK| directions, ev ~1e-9 abs.  EXPECTED.
  sparse-code confounds     1-2-station (sat,signal) code.  GENUINELY null
                            (~1e-11 abs) — no gate value can clear them; the
                            W6 admission census + phase co-gate own them.
  weak-but-real families    observable but weak (~1e-3 abs), MISCOUNTED by a
                            single relative cutoff.  Gate calibration owns them
                            (network.rank_gate_tol).
  deep phase orphans        admitted thin signals with no reachable ambiguity
                            anchor.  The prioritized drop-and-heal owns them.

Two arms whose totals are 22 and 33 per dump can therefore be BETTER, not worse,
if the 33 traded true nulls for weak-real ones.  Every table below is
class-decomposed for exactly that reason.

WHY THE UNITS ARE LABELLED EVERYWHERE
-------------------------------------
The spectrum dump is BUDGET-LATCHED (the producer dumps only the first N gate
evaluations of a run, N = 6 today).  "135 directions" and "22 directions per
dump" are the SAME measurement in two units, and mixing them silently is the
documented anti-pattern this tool exists to end.  Every printed count carries
its unit, and an arm-vs-arm table uses ONE unit for the whole table (--unit).

Sources
-------
(1) ``network_estimability.csv`` — the Card-DG machine-readable dump.  Preferred
    when present: exact values, no text parsing, and it carries the RUN record
    with run-wide gate telemetry the log does not have.
(2) ``main_log.log`` (or any stdout capture) — the ``Network[estimability]:``
    WARN lines.  The ONLY source for archived runs, because LogManager DELETES
    the log directory's *.log on every start, so historical evidence survives
    only as console captures.  This fallback must keep working.

An input may be a run directory (``<run>/logs/`` is searched too), a log
directory, or a plain file — the storm archives are loose ``.log`` captures.

Sections
--------
(a) SOURCE + census: dumps found, directions below cutoff, both units.
(b) CLASS decomposition: the family signature of each direction's significant
    components (``pure_CB`` / ``pure_PB`` / ``pure_CLK`` / ``CB+PB`` / ...).
    This is the table that decides whether a change helped.
(c) EIGENVALUE structure: per-decade histogram plus the gap bands, which is
    where a calibration value is READ OFF (a cutoff must sit >= 2 decades above
    the gauge depth and >= 2 decades below the weakest-real layer).
(d) CONSTELLATION signature and per-satellite culprits.
(e) WITHHOLD disposition — read from the log even when the spectrum came from
    the CSV (a run directory hands back both).
(f) ARM-vs-ARM diff when two inputs are given.

A NOTE ON TWO NUMBERS THAT LOOK COMPARABLE AND ARE NOT: the dump's
``N ev<cutoff`` and the withhold line's ``observed M`` are different quantities —
the withhold message reports the POST-heal re-gate.  Measured on the 305 control
arm at TOW 259200: dump 22, withhold 14, identical cutoff.  This tool never
cross-validates or sums them.

Usage
-----
    python scripts/characterize_storm.py <input> [<input2>] [options]

    python scripts/characterize_storm.py runs/ctl runs/datum --unit per-dump
    python scripts/characterize_storm.py maxnet305_ctl.log --cross-check
    python scripts/characterize_storm.py logs --json storm.json

Cross-check
-----------
``--cross-check`` re-derives the 2026-07-22 CB-ADM investigation's recorded
control-arm numbers from the archived log and reports each as PASS/FAIL.  A
disagreement means either the emitter's label formats moved or the classifier
drifted — the arithmetic here has no free parameters beyond --sig and the band
edges.  Oracle (control arm, ``_maxnet_305_gpsgalbds_d15_5ep_ctl.json``):
135 directions across 6 dumps; constellation BeiDou 123 / Galileo 6 / GPS 6;
families CB+PB 98, pure-CLK 16, CB+CLK+PB 13, pure-CB 7, CLK+PB 1; 118 of 135
involve a CB| component; top culprits C06 71, C13 68, C29 53, C16 49.

Exit codes: 0 = report produced; 1 = no usable input found (nothing analyzed);
2 = --cross-check requested and at least one oracle value disagreed.
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import math
import os
import re
import sys

# A component's weight must reach this to count toward a direction's family /
# constellation signature.  0.10 is the value the CB-ADM investigation used; the
# published oracle numbers are only reproducible at this threshold.
SIG_DEFAULT = 0.10

# The families the producer's column descriptors can carry.  Authoritative list,
# read off the emitter (ApplicationNetworkAssembly column registration):
#   "CLK|sat"  "PB|sat|sig"  "CB|sat|sig"  "SH|n|m|trig"  "SH2|n|m|trig"
# plus the "colN" fallback for an unlabeled column.  Anything else is a NEW
# label format and is reported as such rather than silently dropped.
KNOWN_FAMILIES = ('CLK', 'PB', 'CB', 'SH', 'SH2')

# Band edges (absolute eigenvalue) for the gap read-off in section (c).  The
# defaults are the decades the 305-station spectrum separates into.
BANDS_DEFAULT = (1e-3, 1e-6, 1e-9)

# The dump prints `below + 2` directions -- two ABOVE the cutoff, as context.
# Every census here filters on the dump's OWN cutoff instead of trusting the row
# count, and instead of any hard-coded cutoff value (it differs between arms).
CONSTELLATION_NAMES = {'G': 'GPS', 'R': 'GLONASS', 'E': 'Galileo', 'C': 'BeiDou',
                       'J': 'QZSS', 'S': 'SBAS', 'I': 'IRNSS'}

_RE_HDR = re.compile(
    r'Network\[estimability\]:\s*TOW\s+([-\d.eE+]+)\s+reduced_N_ss\s+(\d+)x(\d+),\s*'
    r'declared_dim=(\d+),\s*ev_max=([-\d.eE+]+),\s*cutoff=([-\d.eE+]+),\s*'
    r'(\d+)\s+ev<cutoff')
_RE_DIR = re.compile(
    r'Network\[estimability\]:\s+ev=([-\d.eE+]+)\s+dir spanned by \[([^\]]*)\]')
_RE_WITHHOLD_TOW = re.compile(r'TOW\s+([-\d.eE+]+)\s+drop-and-heal failed')
_RE_WITHHOLD_OBS = re.compile(r'observed\s+(\d+)\s+\(near-\)zero')


# ---------------------------------------------------------------------------
# parsing
# ---------------------------------------------------------------------------
def parse_label(label):
    """Split one column descriptor into (family, sat, signal).

    Returns family='col' for the unlabeled fallback and family='?' for anything
    the emitter is not known to produce -- surfaced in the report rather than
    dropped, because a silently-unclassified direction reads as "clean".
    """
    label = label.strip()
    if not label:
        return ('?', None, None)
    if label.startswith('col') and label[3:].isdigit():
        return ('col', None, None)
    parts = label.split('|')
    fam = parts[0]
    if fam not in KNOWN_FAMILIES:
        return ('?', None, None)
    if fam in ('SH', 'SH2'):          # SH|n|m|trig -- a field, not a satellite
        return (fam, None, None)
    sat = parts[1].replace(' ', '') if len(parts) > 1 else None
    sig = parts[2] if len(parts) > 2 else None
    return (fam, sat, sig)


def parse_components(body):
    """Parse a ``dir spanned by [...]`` body into [(family, sat, signal, weight)].

    Tokens are ``label=weight`` joined by ', '.  Labels never contain a comma
    (the emitter builds them from ids), so a plain split is exact; rpartition on
    '=' keeps any future '='-bearing label intact.
    """
    out = []
    for tok in body.split(', '):
        tok = tok.strip()
        if not tok:
            continue
        label, sep, wtxt = tok.rpartition('=')
        if not sep:
            continue
        try:
            w = float(wtxt)
        except ValueError:
            continue
        fam, sat, sig = parse_label(label)
        out.append((fam, sat, sig, w))
    return out


class Dump(object):
    """One estimability dump = one gate evaluation the producer chose to record."""

    def __init__(self, tow, n_shared, declared_dim, ev_max, cutoff, n_below):
        self.tow = tow
        self.n_shared = n_shared
        self.declared_dim = declared_dim
        self.ev_max = ev_max
        self.cutoff = cutoff
        self.n_below = n_below          # as REPORTED by the producer
        self.dirs = []                  # [(ev, components)] -- ALL reported rows

    @property
    def below(self):
        """The rows genuinely below THIS dump's cutoff (the +2 context removed)."""
        return [(ev, comps) for ev, comps in self.dirs if ev < self.cutoff]


class Arm(object):
    def __init__(self, label, source, path):
        self.label = label
        self.source = source            # 'csv' | 'log'
        self.path = path
        self.dumps = []
        self.withheld = {}              # tow -> observed count from the withhold line
        self.withhold_source = None     # the log the withhold lines came from, if any
        self.run = None                 # DG6 RUN record (csv source only)

    @property
    def n_dumps(self):
        return len(self.dumps)

    def all_below(self):
        out = []
        for d in self.dumps:
            for ev, comps in d.below:
                out.append((d, ev, comps))
        return out


def read_log(path, label):
    """Parse ``Network[estimability]:`` WARN lines out of a log / console capture."""
    arm = Arm(label, 'log', path)
    cur = None
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            m = _RE_HDR.search(line)
            if m:
                cur = Dump(float(m.group(1)), int(m.group(2)), int(m.group(4)),
                           float(m.group(5)), float(m.group(6)), int(m.group(7)))
                arm.dumps.append(cur)
                continue
            m = _RE_DIR.search(line)
            if m and cur is not None:
                cur.dirs.append((float(m.group(1)), parse_components(m.group(2))))
                continue
            m = _RE_WITHHOLD_TOW.search(line)
            if m:
                obs = _RE_WITHHOLD_OBS.search(line)
                arm.withheld[float(m.group(1))] = int(obs.group(1)) if obs else None
    return arm


def read_csv(path, label):
    """Parse the Card-DG ``network_estimability.csv`` (kind = EPOCH | DIR | RUN)."""
    arm = Arm(label, 'csv', path)
    cur = None
    with open(path, encoding='utf-8', errors='replace', newline='') as fh:
        for row in csv.DictReader(fh):
            kind = (row.get('kind') or '').strip()
            if kind == 'EPOCH':
                cur = Dump(_f(row.get('tow')), _i(row.get('n_shared')),
                           _i(row.get('declared_dim')), _f(row.get('ev_max')),
                           _f(row.get('cutoff')), _i(row.get('n_below')))
                arm.dumps.append(cur)
            elif kind == 'DIR' and cur is not None:
                # range to 9 for forward-compatibility with the widened header;
                # .get() returns None for a column an older CSV does not have, so
                # a 14-column file from before the widening still reads correctly.
                body = ', '.join(row.get('col%d' % i) or '' for i in range(1, 9))
                cur.dirs.append((_f(row.get('ev')), parse_components(body)))
            elif kind == 'RUN':
                # The RUN record reuses the numeric columns; see the writer's
                # comment in NetworkRunTraces.cpp for the mapping.
                arm.run = {
                    'gate_evaluated': _i(row.get('n_shared')),
                    'withheld_under': _i(row.get('declared_dim')),
                    'withheld_over': _i(row.get('ev_max')),
                    'rank_gate_tol': _f(row.get('cutoff')),
                    'emitted': _i(row.get('n_below')),
                    'withheld_total': _i(row.get('ev')),
                }
                for i in range(1, 9):
                    tok = (row.get('col%d' % i) or '').strip()
                    if '=' in tok:
                        k, _, v = tok.partition('=')
                        arm.run[k] = _i(v)
    return arm


def _f(x, default=0.0):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def _i(x, default=0):
    try:
        return int(float(x))
    except (TypeError, ValueError):
        return default


def find_log(path):
    """The log / console capture for a run dir, or None."""
    if os.path.isfile(path):
        return path if not path.lower().endswith('.csv') else None
    for base in (path, os.path.join(path, 'logs')):
        for name in ('main_log.log', 'run.log'):
            log_p = os.path.join(base, name)
            if os.path.isfile(log_p):
                return log_p
    return None


def resolve_input(path):
    """Return (kind, spectrum_path, log_path) for a run dir, log dir, or file.

    The CSV wins for the SPECTRUM (exact values, no text parsing) but carries no
    per-epoch withhold lines, so a run dir that has both hands back the log as
    well — section (e) then works on either source.
    """
    log_p = find_log(path)
    if os.path.isfile(path):
        if path.lower().endswith('.csv'):
            return ('csv', path, None)
        return ('log', path, path)
    if not os.path.isdir(path):
        return (None, path, None)
    for base in (path, os.path.join(path, 'logs')):
        csv_p = os.path.join(base, 'network_estimability.csv')
        if os.path.isfile(csv_p):
            return ('csv', csv_p, log_p)
    if log_p:
        return ('log', log_p, log_p)
    return (None, path, None)


def scavenge_withholds(arm, log_path):
    """Fill an arm's withhold map from a log when the spectrum came from the CSV."""
    if not log_path or not os.path.isfile(log_path):
        return
    arm.withhold_source = log_path
    with open(log_path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            m = _RE_WITHHOLD_TOW.search(line)
            if m:
                obs = _RE_WITHHOLD_OBS.search(line)
                arm.withheld[float(m.group(1))] = int(obs.group(1)) if obs else None


# ---------------------------------------------------------------------------
# classification
# ---------------------------------------------------------------------------
def signature(comps, sig):
    """The '+'-joined FAMILY signature of a direction's significant components."""
    fams = sorted({c[0] for c in comps if c[3] >= sig})
    if not fams:
        return 'none'
    if len(fams) == 1:
        return 'pure_' + fams[0]
    return '+'.join(fams)


def const_signature(comps, sig):
    """The '+'-joined CONSTELLATION signature (satellite-bearing families only)."""
    cs = sorted({c[1][0] for c in comps if c[3] >= sig and c[1]})
    return '+'.join(cs) if cs else 'none'


def decade(ev):
    """Histogram bin label for one eigenvalue (<=0 is its own bin: the reduced
    normals are only positive-SEMIdefinite, so a rounding-negative gauge is
    normal and must not be dropped by a log10)."""
    if ev <= 0.0:
        return '<=0'
    e = int(math.floor(math.log10(ev)))
    return '1e%d' % e


def band(ev, bands):
    edges = list(bands)
    if ev <= 0.0:
        return '<=0'
    for i, edge in enumerate(edges):
        if ev >= edge:
            return '>=%g' % edge if i == 0 else '[%g, %g)' % (edge, edges[i - 1])
    return '<%g' % edges[-1]


def analyze(arm, sig, bands):
    """Everything section (b)-(d) prints, in BOTH units, for one arm."""
    rows = arm.all_below()
    n_dumps = max(arm.n_dumps, 1)
    r = {
        'label': arm.label,
        'source': arm.source,
        'path': arm.path,
        'n_dumps': arm.n_dumps,
        'n_below_across_dumps': len(rows),
        'n_below_per_dump': len(rows) / float(n_dumps),
        'declared_dim': sorted({d.declared_dim for d in arm.dumps}),
        'cutoffs': sorted({d.cutoff for d in arm.dumps}),
        'ev_max': sorted({d.ev_max for d in arm.dumps}),
        'n_shared': sorted({d.n_shared for d in arm.dumps}),
        'reported_below_per_dump': [d.n_below for d in arm.dumps],
        'run': arm.run,
    }
    # Consistency guard: the producer's own N ev<cutoff vs what we re-derived
    # from the printed rows.  They can differ ONLY if the dump was truncated
    # (it prints below+2 rows, so it cannot be) -- a mismatch means the parser
    # missed rows, which would silently deflate every table below.
    mismatch = [(d.tow, d.n_below, len(d.below)) for d in arm.dumps
                if d.n_below != len(d.below)]
    r['parse_mismatch'] = mismatch

    cls = collections.Counter()
    cons = collections.Counter()
    culprit_comp = collections.Counter()    # component appearances (oracle unit)
    culprit_dir = collections.Counter()     # directions touched (deduped)
    satsig = collections.Counter()
    dec = collections.Counter()
    bnd = collections.Counter()
    unknown = collections.Counter()
    n_any_cb = 0
    for _d, ev, comps in rows:
        cls[signature(comps, sig)] += 1
        cons[const_signature(comps, sig)] += 1
        dec[decade(ev)] += 1
        bnd[band(ev, bands)] += 1
        seen = set()
        for fam, sat, sg, w in comps:
            if w < sig:
                continue
            if fam == '?':
                unknown['unrecognized_label'] += 1
            if sat:
                culprit_comp[sat] += 1
                seen.add(sat)
                if sg:
                    satsig[(sat, sg)] += 1
        for sat in seen:
            culprit_dir[sat] += 1
        if any(c[0] == 'CB' and c[3] >= sig for c in comps):
            n_any_cb += 1
    r['class_across_dumps'] = dict(cls.most_common())
    r['const_across_dumps'] = dict(cons.most_common())
    r['culprit_sat_components_across_dumps'] = dict(culprit_comp.most_common())
    r['culprit_sat_directions_across_dumps'] = dict(culprit_dir.most_common())
    r['culprit_satsig_across_dumps'] = {'%s|%s' % k: v for k, v in satsig.most_common()}
    r['ev_decades_across_dumps'] = dict(sorted(dec.items(), key=_bin_key))
    r['ev_bands_across_dumps'] = dict(bnd.most_common())
    r['n_directions_involving_CB'] = n_any_cb
    r['unknown_labels'] = dict(unknown)
    r['withheld_epochs'] = len(arm.withheld)
    r['withhold_source'] = arm.withhold_source
    r['withhold_observed_counts'] = sorted({v for v in arm.withheld.values()
                                            if v is not None})
    return r


def _bin_key(item):
    k = item[0]
    return (-1e9, ) if k == '<=0' else (float(k[2:]), )


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------
def fmt_count(n, n_dumps, unit):
    if unit == 'across-dumps':
        return '%d' % n
    return '%.1f' % (n / float(max(n_dumps, 1)))


def unit_suffix(unit):
    return 'across dumps' if unit == 'across-dumps' else 'per dump'


def report_arm(r, args):
    n_dumps = r['n_dumps']
    u = args.unit
    print('')
    print('=' * 78)
    print('ARM %-20s  source=%s  %s' % (r['label'], r['source'], r['path']))
    print('=' * 78)
    print('(a) CENSUS')
    print('    dumps recorded                 : %d   [the producer budget-latches '
          'these; every count below is per-dump or across-dumps, never both]'
          % n_dumps)
    print('    directions below cutoff        : %d across dumps  =  %.1f per dump'
          % (r['n_below_across_dumps'], r['n_below_per_dump']))
    print('    declared S-basis dimension     : %s' % _seq(r['declared_dim']))
    print('    reduced border n_shared        : %s' % _seq(r['n_shared']))
    print('    lambda_max                     : %s' % _seq(r['ev_max'], '%.3e'))
    print('    absolute cutoff (tol*lambda)   : %s' % _seq(r['cutoffs'], '%.3e'))
    if r['parse_mismatch']:
        print('    !! PARSE MISMATCH — producer N ev<cutoff vs re-derived rows: %s'
              % r['parse_mismatch'][:5])
        print('       (every table below is deflated; fix the parser before '
              'believing any of it)')
    if r['unknown_labels']:
        print('    !! UNRECOGNIZED column labels: %s — the emitter grew a format '
              'this tool does not classify' % r['unknown_labels'])
    if r['run']:
        run = r['run']
        print('    DG6 run telemetry (CSV RUN record):')
        print('       rank_gate_tol=%g  gate evaluated on %d epoch(s): %d emitted, '
              '%d withheld UNDER-specified, %d withheld OVER-specified'
              % (run.get('rank_gate_tol', 0.0), run.get('gate_evaluated', 0),
                 run.get('emitted', 0), run.get('withheld_under', 0),
                 run.get('withheld_over', 0)))
        print('       observed near-null count over the run: min %s, max %s   '
              '(target = declared dim)'
              % (run.get('obs_min', '?'), run.get('obs_max', '?')))
        att_e = run.get('heal_attempt_epochs')
        att_c = run.get('heal_attempt_cols')
        print('       heal: FIRED on %s epoch(s) / %s column(s); RECOVERED %s epoch(s) '
              '/ %s column(s)'
              % ('?' if att_e is None else att_e, '?' if att_c is None else att_c,
                 run.get('healed_epochs', '?'), run.get('healed_cols', '?')))
        if att_e and not run.get('healed_epochs'):
            print('             ^ fired but never recovered — the heal is live and '
                  'aiming at real columns, it just cannot resolve the residual alone '
                  '(PA5 insufficiency, NOT dead wiring)')
        print('       CB-ADM phase-orphan window: %s (key,epoch) pair(s)'
              % run.get('cbadm_phase_orphan_key_epochs', '?'))

    print('')
    print('(b) CLASS DECOMPOSITION  [%s]' % unit_suffix(u))
    print('    A direction\'s class is the family signature of its components at '
          'weight >= %.2f.' % args.sig)
    print('    %-22s %10s   %s' % ('class', unit_suffix(u), 'owner / meaning'))
    for k, v in sorted(r['class_across_dumps'].items(), key=lambda x: -x[1]):
        print('    %-22s %10s   %s' % (k, fmt_count(v, n_dumps, u), _class_note(k)))
    print('    %-22s %10s   directions with ANY CB| component'
          % ('(any CB| involved)', fmt_count(r['n_directions_involving_CB'], n_dumps, u)))

    print('')
    print('(c) EIGENVALUE STRUCTURE  [%s]  — this is where a rank_gate_tol value '
          'is READ OFF' % unit_suffix(u))
    print('    per decade:')
    for k, v in sorted(r['ev_decades_across_dumps'].items(), key=_bin_key):
        print('      %-10s %8s' % (k, fmt_count(v, n_dumps, u)))
    print('    gap bands (edges %s):' % ', '.join('%g' % b for b in args.bands))
    for k, v in sorted(r['ev_bands_across_dumps'].items(), key=lambda x: -x[1]):
        print('      %-16s %8s' % (k, fmt_count(v, n_dumps, u)))
    _placement_hint(r, args)

    print('')
    print('(d) CONSTELLATION SIGNATURE  [%s]' % unit_suffix(u))
    for k, v in sorted(r['const_across_dumps'].items(), key=lambda x: -x[1]):
        name = ' + '.join(CONSTELLATION_NAMES.get(c, c) for c in k.split('+')) \
            if k != 'none' else '(no satellite-bearing component)'
        print('    %-12s %8s   %s' % (k, fmt_count(v, n_dumps, u), name))
    print('    top culprit satellites (component appearances / directions '
          'touched, both %s):' % unit_suffix(u))
    comp = r['culprit_sat_components_across_dumps']
    dirs = r['culprit_sat_directions_across_dumps']
    for sat, n in list(comp.items())[:args.top]:
        print('      %-6s %8s comp   %8s dir'
              % (sat, fmt_count(n, n_dumps, u), fmt_count(dirs.get(sat, 0), n_dumps, u)))
    if args.top_satsig:
        print('    top culprit (sat|signal) pairs [%s]:' % unit_suffix(u))
        for k, n in list(r['culprit_satsig_across_dumps'].items())[:args.top_satsig]:
            print('      %-12s %8s' % (k, fmt_count(n, n_dumps, u)))

    print('')
    print('(e) WITHHOLD DISPOSITION')
    if not r['withhold_source']:
        print('    (no log alongside the CSV — the per-epoch withhold lines live only '
              'in the log; the RUN record above is the run-wide answer)')
    print('    epochs with a "drop-and-heal failed; withholding" line: %d   [source: %s]'
          % (r['withheld_epochs'], r['withhold_source'] or 'unavailable'))
    if r['withhold_observed_counts']:
        print('    observed counts on those lines: %s' % r['withhold_observed_counts'])
        print('    NOTE these are POST-heal re-gate counts and are NOT comparable '
              'to the dump\'s below-cutoff census above — different quantity, '
              'same epoch.  Never sum or cross-validate the two.')


def _seq(vals, fmt='%s'):
    if not vals:
        return '(none)'
    if len(vals) == 1:
        return fmt % vals[0]
    return '%s .. %s (%d distinct)' % (fmt % vals[0], fmt % vals[-1], len(vals))


def _class_note(k):
    return {
        'pure_CLK': 'the DECLARED clock gauges — expected, not the storm',
        'pure_CB': 'sparse-code confound — genuinely null; admission census owns it',
        'pure_PB': 'phase orphan — genuinely null; drop-and-heal owns it',
        'CB+PB': 'code<->phase confounded lump — weak-but-real if ev ~1e-3',
        'CB+CLK+PB': 'single-station code/clock/phase triple — genuinely null',
        'CLK+PB': 'clock<->phase lump',
        'pure_SH': 'SH-VTEC datum direction — a MISSING iono datum, not a storm',
        'pure_SH2': 'second-shell SH-VTEC datum direction',
        'pure_col': 'UNLABELED column — arm trace_estimability so col_desc is built',
        'none': 'no component above the significance threshold',
    }.get(k, '')


def _placement_hint(r, args):
    """Print the DG3 placement read-off: the three numbers a non-default
    network.rank_gate_tol must be justified by, plus what would SURVIVE it."""
    pl = r.get('placement') or {}
    gd, wf = pl.get('gauge_depth_abs'), pl.get('weak_real_floor_abs')
    print('    DG3 placement read-off (the three numbers an armed tolerance must '
          'be recorded with):')
    if gd is None:
        print('      gauge depth        : (no pure_CLK direction below cutoff — the '
              'declared gauges are not in this census; cannot place a value)')
        return
    print('      gauge depth        : %.3e   (max |ev| over the pure_CLK declared '
          'gauges; the cutoff must stay ABOVE this or they stop being counted '
          'and a HEALTHY config withholds "over-specified")' % gd)
    if wf is None:
        print('      weak-real floor    : (nothing in the top band — no weak-but-real '
              'layer to rescue, so calibration has nothing to do here)')
        return
    print('      weak-real floor    : %.3e   (min ev in the >= %g band — the layer '
          'the cutoff must drop BELOW to reclassify as observable)'
          % (wf, args.bands[0]))
    if not (gd > 0 and wf > gd):
        print('      (degenerate ordering — no usable gap)')
        return
    decades = math.log10(wf / gd)
    cut = math.sqrt(gd * wf)
    lam = r['ev_max'][-1] if r['ev_max'] else 1.0
    tol = cut / max(lam, 1.0)
    print('      proposed cutoff    : %.3e  =>  network.rank_gate_tol %.2e  '
          '(geometric mid-gap at lambda_max %.3e)' % (cut, tol, lam))
    print('      gap width          : %.1f decades (%.1f each side of mid-gap)'
          % (decades, decades / 2.0))
    if decades < 4.0:
        print('      !! WARNING: under 4 decades leaves less than 2 decades of '
              'margin per side — DG3 does not authorize arming a value here.')
    # The falsifier prediction: DG clears the weak-real layer, but anything
    # genuinely null stays below ANY positive cutoff.  Say so up front so the
    # armed run is judged on the census SHIFT, not on whether it emitted.
    rem = pl.get('remaining_below_proposed', {})
    n_rem = sum(rem.values())
    print('      at that cutoff, %d direction(s) %s would REMAIN below it:'
          % (n_rem, unit_suffix('across-dumps')))
    for k, v in sorted(rem.items(), key=lambda x: -x[1]):
        print('        %-16s %6d   %s' % (k, v, _class_note(k)))
    dim = r['declared_dim'][-1] if r['declared_dim'] else None
    if dim is not None:
        n_per_dump = n_rem / float(max(r['n_dumps'], 1))
        verdict = ('the gate would PASS' if abs(n_per_dump - dim) < 1e-9 else
                   'the gate would STILL WITHHOLD (a non-gauge residual survives; '
                   'that class is the drop-and-heal\'s, not the calibration\'s)')
        print('      vs declared dim %d: %.1f remain per dump => %s'
              % (dim, n_per_dump, verdict))


def compute_placement(arm, r, sig, bands):
    """Derive DG3's placement inputs from the measured spectrum.

    gauge depth      = max |ev| over the DECLARED gauges, identified by class
                       (pure_CLK).  |ev| because the reduced normals are only
                       positive-SEMIdefinite: a rounding-negative gauge is normal.
    weak-real floor  = min ev among below-cutoff directions in the TOP band
                       (>= bands[0]) — the weak-but-real layer as the operator's
                       own band edge defines it.
    remaining        = what would still be below a mid-gap cutoff, BY CLASS.  A
                       genuinely null direction is null at every positive cutoff,
                       so this is the honest prediction of whether calibration
                       alone can make the arm emit.
    """
    gauge, top_band = [], []
    for _d, ev, comps in arm.all_below():
        if signature(comps, sig) == 'pure_CLK':
            gauge.append(abs(ev))
        if ev >= bands[0]:
            top_band.append(ev)
    pl = {'gauge_depth_abs': max(gauge) if gauge else None,
          'weak_real_floor_abs': min(top_band) if top_band else None}
    gd, wf = pl['gauge_depth_abs'], pl['weak_real_floor_abs']
    if gd and wf and wf > gd > 0:
        cut = math.sqrt(gd * wf)
        lam = max(max((d.ev_max for d in arm.dumps), default=1.0), 1.0)
        pl['proposed_cutoff_abs'] = cut
        pl['proposed_rank_gate_tol'] = cut / lam
        pl['gap_decades'] = math.log10(wf / gd)
        rem = collections.Counter()
        for _d, ev, comps in arm.all_below():
            if ev < cut:
                rem[signature(comps, sig)] += 1
        pl['remaining_below_proposed'] = dict(rem.most_common())
    r['placement'] = pl


# ---------------------------------------------------------------------------
# arm-vs-arm
# ---------------------------------------------------------------------------
def report_diff(ra, rb, args):
    u = args.unit
    na, nb = ra['n_dumps'], rb['n_dumps']
    print('')
    print('=' * 78)
    print('(f) ARM vs ARM   A=%s  B=%s   [ALL counts %s]'
          % (ra['label'], rb['label'], unit_suffix(u)))
    print('=' * 78)
    if u == 'across-dumps' and na != nb:
        print('    !! the arms recorded %d and %d dumps — across-dumps counts are '
              'NOT comparable between them.  Re-run with --unit per-dump.' % (na, nb))
    print('    %-24s %10s %10s %10s' % ('metric', 'A', 'B', 'B-A'))
    _diff_row('directions below cutoff', ra['n_below_across_dumps'],
              rb['n_below_across_dumps'], na, nb, u)
    _diff_row('  ...involving CB|', ra['n_directions_involving_CB'],
              rb['n_directions_involving_CB'], na, nb, u)
    print('    -- by class --')
    for k in sorted(set(ra['class_across_dumps']) | set(rb['class_across_dumps']),
                    key=lambda k: -(ra['class_across_dumps'].get(k, 0)
                                    + rb['class_across_dumps'].get(k, 0))):
        _diff_row(k, ra['class_across_dumps'].get(k, 0),
                  rb['class_across_dumps'].get(k, 0), na, nb, u)
    print('    -- by constellation signature --')
    for k in sorted(set(ra['const_across_dumps']) | set(rb['const_across_dumps']),
                    key=lambda k: -(ra['const_across_dumps'].get(k, 0)
                                    + rb['const_across_dumps'].get(k, 0))):
        _diff_row(k, ra['const_across_dumps'].get(k, 0),
                  rb['const_across_dumps'].get(k, 0), na, nb, u)
    print('    -- by eigenvalue band (the calibration read-off) --')
    for k in sorted(set(ra['ev_bands_across_dumps']) | set(rb['ev_bands_across_dumps'])):
        _diff_row(k, ra['ev_bands_across_dumps'].get(k, 0),
                  rb['ev_bands_across_dumps'].get(k, 0), na, nb, u)
    print('    -- top culprit satellites (component appearances) --')
    keys = sorted(set(list(ra['culprit_sat_components_across_dumps'])[:args.top])
                  | set(list(rb['culprit_sat_components_across_dumps'])[:args.top]))
    for k in keys:
        _diff_row(k, ra['culprit_sat_components_across_dumps'].get(k, 0),
                  rb['culprit_sat_components_across_dumps'].get(k, 0), na, nb, u)
    print('')
    print('    READ THIS AS A TRADE, NOT A TOTAL: a bigger below-cutoff count is '
          'an IMPROVEMENT when it trades genuinely-null classes (pure_CB, '
          'CB+CLK+PB, pure_PB orphans) for weak-but-real ones that a calibrated '
          'cutoff can clear.  Compare the class rows and the 1e-3 band, not the '
          'first line.')


def _diff_row(name, a, b, na, nb, u):
    if u == 'across-dumps':
        print('    %-24s %10d %10d %+10d' % (name[:24], a, b, b - a))
    else:
        pa, pb = a / float(max(na, 1)), b / float(max(nb, 1))
        print('    %-24s %10.1f %10.1f %+10.1f' % (name[:24], pa, pb, pb - pa))


# ---------------------------------------------------------------------------
# A4 cross-check
# ---------------------------------------------------------------------------
ORACLE = {
    'source': 'docs/HANDOFF_cb_admission_card.md, 305 CONTROL arm '
              '(_maxnet_305_gpsgalbds_d15_5ep_ctl.json), 2026-07-22',
    'n_dumps': 6,
    'n_below_across_dumps': 135,
    'const_across_dumps': {'C': 123, 'E': 6, 'G': 6},
    'class_across_dumps': {'CB+PB': 98, 'pure_CLK': 16, 'CB+CLK+PB': 13,
                           'pure_CB': 7, 'CLK+PB': 1},
    'n_directions_involving_CB': 118,
    'culprit_sat_components_across_dumps': {'C06': 71, 'C13': 68, 'C29': 53,
                                            'C16': 49, 'C34': 43, 'C36': 32,
                                            'C38': 31},
}


def cross_check(r):
    """Re-derive the recorded control-arm numbers.  Returns (n_pass, n_fail)."""
    print('')
    print('=' * 78)
    print('A4 CROSS-CHECK against the recorded CB-ADM control-arm numbers')
    print('    oracle: %s' % ORACLE['source'])
    print('=' * 78)
    npass = nfail = 0
    def chk(name, got, want):
        nonlocal npass, nfail
        ok = got == want
        npass, nfail = npass + (1 if ok else 0), nfail + (0 if ok else 1)
        print('    [%s] %-42s got %-8s want %s'
              % ('PASS' if ok else 'FAIL', name, got, want))
    chk('dumps recorded', r['n_dumps'], ORACLE['n_dumps'])
    chk('directions below cutoff (across dumps)',
        r['n_below_across_dumps'], ORACLE['n_below_across_dumps'])
    chk('directions involving CB|',
        r['n_directions_involving_CB'], ORACLE['n_directions_involving_CB'])
    for k, want in ORACLE['const_across_dumps'].items():
        chk('constellation %s' % k, r['const_across_dumps'].get(k, 0), want)
    for k, want in ORACLE['class_across_dumps'].items():
        chk('class %s' % k, r['class_across_dumps'].get(k, 0), want)
    for k, want in ORACLE['culprit_sat_components_across_dumps'].items():
        chk('culprit %s (components)' % k,
            r['culprit_sat_components_across_dumps'].get(k, 0), want)
    print('    ---- %d passed, %d failed ----' % (npass, nfail))
    if nfail:
        print('    A disagreement means the emitter\'s label formats moved or the '
              'classifier drifted — investigate before trusting any table above.')
    return npass, nfail


# ---------------------------------------------------------------------------
def main(argv=None):
    # The estimability / withhold lines carry em-dashes and sigmas the cp1252
    # console cannot encode; force UTF-8 so a report never dies on content.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass

    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('inputs', nargs='+', metavar='INPUT',
                   help='run dir, log dir, network_estimability.csv, or a log / '
                        'console capture.  Two inputs enable arm-vs-arm diff mode.')
    p.add_argument('--labels', metavar='A,B',
                   help='names for the arms in the report (default: basenames)')
    p.add_argument('--unit', choices=('per-dump', 'across-dumps'),
                   default='across-dumps',
                   help='unit for every printed count (default: %(default)s)')
    p.add_argument('--sig', type=float, default=SIG_DEFAULT, metavar='W',
                   help='component weight above which a family counts toward a '
                        'direction\'s signature (default: %(default)s)')
    p.add_argument('--bands', default=','.join('%g' % b for b in BANDS_DEFAULT),
                   metavar='E,E,E',
                   help='absolute eigenvalue band edges, descending '
                        '(default: %(default)s)')
    p.add_argument('--top', type=int, default=12, metavar='N',
                   help='culprit-satellite list length (default: %(default)s)')
    p.add_argument('--top-satsig', type=int, default=0, metavar='N',
                   help='also list the top N (sat|signal) pairs (default: off)')
    p.add_argument('--cross-check', action='store_true',
                   help='re-derive the recorded CB-ADM control-arm numbers from '
                        'the FIRST input and report PASS/FAIL (A4)')
    p.add_argument('--json', dest='json_out', metavar='PATH',
                   help='also dump the machine-readable summary here')
    a = p.parse_args(argv)

    try:
        a.bands = tuple(sorted((float(x) for x in a.bands.split(',')), reverse=True))
    except ValueError:
        p.error('--bands must be comma-separated numbers: %r' % a.bands)
    if len(a.inputs) > 2:
        p.error('at most two inputs (one arm, or two for a diff)')

    labels = (a.labels.split(',') if a.labels
              else [os.path.basename(os.path.normpath(i)) or i for i in a.inputs])
    while len(labels) < len(a.inputs):
        labels.append('arm%d' % (len(labels) + 1))

    arms, results = [], []
    for path, label in zip(a.inputs, labels):
        kind, resolved, log_path = resolve_input(path)
        if kind is None:
            print('input not usable (no network_estimability.csv, main_log.log or '
                  'run.log found): %s' % path)
            continue
        if kind == 'csv':
            arm = read_csv(resolved, label)
            scavenge_withholds(arm, log_path)   # the CSV has no withhold lines
        else:
            arm = read_log(resolved, label)
            arm.withhold_source = resolved
        if not arm.dumps:
            print('no estimability dumps in %s — was network.trace_estimability '
                  'armed for that run?' % resolved)
            continue
        r = analyze(arm, a.sig, a.bands)
        compute_placement(arm, r, a.sig, a.bands)
        arms.append(arm)
        results.append(r)

    if not results:
        print('nothing analyzed: no usable input found')
        return 1

    print('storm characterization: significance weight >= %.2f, band edges %s'
          % (a.sig, ', '.join('%g' % b for b in a.bands)))
    for r in results:
        report_arm(r, a)
    if len(results) == 2:
        report_diff(results[0], results[1], a)

    rc = 0
    if a.cross_check:
        _, nfail = cross_check(results[0])
        if nfail:
            rc = 2

    if a.json_out:
        summary = {'sig': a.sig, 'bands': list(a.bands), 'unit': a.unit,
                   'arms': [{k: v for k, v in r.items() if not k.startswith('_')}
                            for r in results]}
        try:
            with open(a.json_out, 'w') as fh:
                json.dump(summary, fh, indent=2, default=str)
            print('\nwrote %s' % a.json_out)
        except OSError as e:
            print('\n[warn] could not write %s: %s' % (a.json_out, e))
    return rc


if __name__ == '__main__':
    sys.exit(main())
