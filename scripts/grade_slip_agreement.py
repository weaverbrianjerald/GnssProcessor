#!/usr/bin/env python3
"""Grade the network slip layer by cross-checking its two detectors against each
other and against the raw observables.

The network producer runs two independent coarse-slip detectors over the same
epochs: the T1 IF-phase time-difference detector (``network.cycle_slip.if_phase_td``
-> ``network_if_slip.csv`` samples + ``T1_if_slip`` events) and the T0 MW / LLI
detector (``T0_mw_detect`` events).  Neither is ground truth.  This grader decides
which flags are supported: it checks every IF flag against the raw MW / GF / LLI
series that the producer logged, and sweeps the raw series in reverse for steps
the IF detector did not flag.

It is post-processing over the run's CSV logs only; the binary is not needed.

Why adjudicate rather than count
--------------------------------
A slip detector's flag count means nothing on its own: an all-quiet run and a
100%-suppressed run both read "clean".  The questions are (i) whether the two
detectors fire on the same (station, sat, epoch), (ii) whether the raw observable
steps where a detector fired, and (iii) whether the raw observable steps where no
detector fired.  (i) is section (b), (ii) section (e), (iii) section (f).  An IF
flag with no raw support is a false positive; a >3-cycle raw MW step with no IF
flag is a miss, unless it is a gap-coincident re-acquisition arc.  For that reason
the miss list prints the step size (a 40-cycle "miss" is a new arc, not a missed
slip).

Sections
--------
(a) Event counts: ``network_events.csv`` by tier / event / action.  ``action``
    is ``refloat`` (the estimator was told), ``diagnostic`` (logged only: cold
    run or warm gate), or ``suppressed`` (a credibility / corroboration gate
    discarded the flag).  Counts are broken out by action because a tier that is
    100% ``diagnostic`` is inert on the ON path however the flag population looks
    (standing invariant 4: run-end counters are the telemetry).
(b) IF <-> MW/LLI agreement: matched on (station, sat) with a +/-1-epoch
    tolerance (``--epoch-tol``) over the sorted unique (week, tow) epochs of the
    events file.  Reports IF matched / IF-only / MW-only and the match rate.  A
    low match rate is not by itself a defect (the two detectors have different
    sensitivity floors, and MW is often warm-gated); read it together with (e).
(c) Bunching check: events per epoch, plus the top-N stations and satellites.
    A working detector spreads flags thinly across epochs, stations and sats.
    Bunching indicates a problem: one epoch holding most of the run's flags is a
    network-wide re-level or a datum event appearing as N slips; one station
    holding them is a receiver-local family, not a constellation event.
(d) Decisive margin: from ``network_if_slip.csv``, the opportunities, flags, flag
    rate, and the distribution of ``abs_dev_m / thr_m`` over flagged rows.  A
    working detector fires with a clear margin (median well above 1); a population
    piled near 1.0 is grazing the threshold and will chatter under any re-tuning.
(e) Raw adjudication: every ``T1_if_slip`` event (both ``if_slip`` and
    ``refsat_reset``) is looked up in ``network_slip_raw.csv``, which logs
    absolute MW / GF / L1-C1 per (station, sat) per epoch.  Adjacent-epoch
    differencing gives the step; a flag is
      ``supported``   -- |dMW| > ``--mw-gate-cyc`` WL cycles, or |dGF| >
                         ``--gf-gate-m``, or an LLI bit set, within +/-tol;
      ``unsupported`` -- the series is present and flat by all three tests;
      ``no-series``   -- nothing logged for that (station, sat) near that epoch
                         (also covers arc-start epochs, where no step exists).
    Unsupported flags are grouped by (station, sat), which gives the
    false-positive family view: one station x one sat repeating is a
    receiver/refsat artifact, scattered singletons are threshold noise.
(f) Reverse sweep: independent of any flag, the raw series is scanned for |dMW|
    above each of ``--reverse-gates`` WL cycles, and the number that were
    IF-flagged is reported.  Misses are printed with their |dMW| so that
    gap-coincident re-acquisition (large step, new arc) can be told apart from a
    missed slip (2-5 cycles).
(g) D5 re-tap attribution: ``network_d5_flags.csv`` (the per-flag dump of the
    Card-4 D5 fine-slip innovation gate, ``network.mw_wl.d5_dump``) joined against
    the same raw series (docs/SPEC_slip_qc_hardening.md §9.1, card D5-A).  The D5
    gate re-taps carried DD chords, and each re-tap discards accumulated
    double-difference information, so a gate that fires on model tension rather
    than on observable events is a continuous, unreported loss on the NL critical
    path.  The hardening pass measured 1 689 re-taps, of which 50 (3.0%)
    coincided with a raw MW/GF step.  This section decomposes the re-taps to show
    which explanation they support: raw-step coincidence (exact-epoch tag plus an
    independent +/-tol join), the ``std_innov`` distribution against the
    k*sigma0 gate, the top (station, sat) families with their consecutive-run
    lengths (impulsive vs sustained tension), the elevation profile (whether the
    tension is geometry-structured), and the warm/cold split.
(h) Run-end counters: a best-effort verbatim echo of the ``Network (...)`` slip
    counter lines from ``main_log.log``, if present.

Every input file is optional: a missing one prints a "file not found, section
skipped" note instead of failing the run.

Usage
-----
    python scripts/grade_slip_agreement.py [logdir] [--json out.json]

``logdir`` defaults to ``build/x64-Release/logs`` resolved against the repo root
(the parent of this script), i.e. the standard Release run output.  Field runs
use the Release build only, so a Debug log dir is not a valid input.

Cross-check
-----------
The section totals were cross-checked once, when the script was ported, against
the 2026-07-21 IF-TD vs MW slip-agreement audit (150-sta all-freq run): 8717
IF-TD samples -> 32 flags (0.367%) and 17 ``refsat_reset`` events in sections
(a)/(d); 30 of the 49 ``T1_if_slip`` flags raw-confirmed and 14 unsupported in
section (e); and 57 reverse candidates at |dMW| > 3 cyc, of which 39 were
IF-flagged, in section (f).  Reproducing those figures from a re-run of the same
config is the regression test for this script.  A disagreement means either the
log schema changed or the gates were retuned; the arithmetic here has no free
parameters beyond the CLI defaults.

Exit codes: 0 = report produced; 1 = no input file found (nothing graded).
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import os
import re
import statistics as st
import sys
from collections import Counter, defaultdict

SEC_PER_WEEK = 604800.0

IF_TIER = 'T1_if_slip'
MW_TIER = 'T0_mw_detect'
IF_EVENTS = ('if_slip', 'refsat_reset')
MW_EVENTS = ('mw_wl_slip', 'lli_slip')
ACTIONS = ('refloat', 'diagnostic', 'suppressed')

# GPS L1/L2 widelane wavelength c/(f1-f2); overridable for other lane pairs.
LAMBDA_WL_DEFAULT = 0.8619

# main_log lines that mention "Network (" and "slip" but are per-epoch timing
# lines (slip-scans=... ms), not run-end counters.
# 'lane observatory' lines also contain 'slip' through the spec filename
# SPEC_lane_slip_response and would push a real slip counter out of the tail
# window; they belong to a lane-integer instrument, not a slip counter.
_TIMING_NOISE = ('other-net ms', ' ms (', 'lane observatory')
# The producer's first run-end line (the run-timing report, the second row of the run-end
# table, filter/RunEnd.cpp).
_RUN_END_START = 'Network[timing]: epoch-loop wall'


# ---------------------------------------------------------------------------
# small helpers (same contract as grade_ddfrac.py: never raise on a blank cell)
# ---------------------------------------------------------------------------
def _f(x, default=None):
    """float() that returns `default` on blank/malformed cells instead of raising."""
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def _i(x, default=None):
    """int() through float() (handles '1', '1.0', '') without raising."""
    v = _f(x)
    return default if v is None else int(v)


def _s(row, key, default=''):
    """String cell, never None.

    csv.DictReader back-fills missing trailing fields with None, which happens on
    the last (truncated) line of a log that is still being written, and a None in
    a Counter key makes the first sort raise.  Every read of a text column goes
    through this helper."""
    v = row.get(key)
    return default if v is None else str(v).strip()


def _pct(sorted_vals, q):
    """Linear-interpolated percentile of a list that is already sorted; q in [0, 100]."""
    if not sorted_vals:
        return float('nan')
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q / 100.0
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def read_rows(path, label):
    """DictReader a CSV, or print the skip note and return None.

    Returns None (section skipped) for a missing/empty/unreadable file so a
    partial log dir grades what it has instead of aborting the whole report."""
    if not os.path.exists(path):
        print('  [skip] %s: file not found (%s), section skipped' % (label, path))
        return None
    try:                                  # the producer writes UTF-8 (codepage 65001)
        with open(path, newline='', encoding='utf-8', errors='replace') as f:
            rows = list(csv.DictReader(f))
    except OSError as e:
        print('  [skip] %s: unreadable (%s), section skipped' % (label, e))
        return None
    if not rows:
        print('  [skip] %s: no data rows (%s), section skipped' % (label, path))
        return None
    return rows


def ep_key(row):
    """(week, tow) epoch key, or None if either field is blank/malformed."""
    w, t = _f(row.get('week')), _f(row.get('tow'))
    return None if (w is None or t is None) else (w, round(t, 3))


class EpochGrid:
    """Sorted (week, tow) epoch list -> integer index.

    Sorting the (week, tow) tuple is rollover-safe (week increments before tow
    wraps), so index adjacency means "the next epoch of the run" across a week
    boundary.  `nearest` maps an epoch key from another file onto this grid
    (product tow carries ~1e-5 s jitter, and the two files need not share an
    epoch set)."""

    def __init__(self, keys):
        self.keys = sorted(set(k for k in keys if k is not None))
        self.index = {k: i for i, k in enumerate(self.keys)}
        self.times = [k[0] * SEC_PER_WEEK + k[1] for k in self.keys]
        d = [b - a for a, b in zip(self.times, self.times[1:]) if b - a > 1e-6]
        self.dt = st.median(d) if d else 1.0

    def __len__(self):
        return len(self.keys)

    def idx(self, key):
        """Exact index, else None."""
        return self.index.get(key)

    def nearest(self, key, tol_s=None):
        """Index of the closest grid epoch within tol_s seconds, else None."""
        if key is None or not self.times:
            return None
        i = self.index.get(key)
        if i is not None:
            return i
        tol = 0.5 * self.dt if tol_s is None else tol_s
        t = key[0] * SEC_PER_WEEK + key[1]
        j = bisect.bisect_left(self.times, t)
        best, bestd = None, None
        for k in (j - 1, j, j + 1):
            if 0 <= k < len(self.times):
                d = abs(self.times[k] - t)
                if bestd is None or d < bestd:
                    best, bestd = k, d
        return best if (bestd is not None and bestd <= max(tol, 1e-6)) else None

    def tow(self, i):
        return self.keys[i][1] if 0 <= i < len(self.keys) else float('nan')


def hdr(title):
    print('\n' + title)
    print('-' * len(title))


# ---------------------------------------------------------------------------
# (a) event counts by tier / event / action
# ---------------------------------------------------------------------------
def section_events(ev_rows, summary):
    hdr('(a) EVENT COUNTS  [network_events.csv]')
    if ev_rows is None:
        print('  [skip] needs network_events.csv, section skipped')
        summary['events'] = None
        return
    counts = Counter()
    for r in ev_rows:
        counts[(_s(r, 'tier', '?'), _s(r, 'event', '?'), _s(r, 'action') or '(blank)')] += 1
    by_tier = Counter()
    for (tier, _e, _a), n in counts.items():
        by_tier[tier] += n
    print('  %-14s %-16s %-12s %6s' % ('tier', 'event', 'action', 'count'))
    for tier in sorted(by_tier, key=lambda t: (-by_tier[t], t)):
        for (t, e, a), n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
            if t == tier:
                print('  %-14s %-16s %-12s %6d' % (t, e, a, n))
        print('  %-14s %-16s %-12s %6d' % ('', '(tier total)', '', by_tier[tier]))
    print('  %-14s %-16s %-12s %6d' % ('', 'ALL TIERS', '', sum(by_tier.values())))

    det = {t: by_tier.get(t, 0) for t in (IF_TIER, MW_TIER)}
    acts = Counter()
    for (t, _e, a), n in counts.items():
        if t in (IF_TIER, MW_TIER):
            acts[a] += n
    print('  detector tiers: %s=%d  %s=%d   |   actions: %s' %
          (IF_TIER, det[IF_TIER], MW_TIER, det[MW_TIER],
           ', '.join('%s=%d' % (a, acts.get(a, 0)) for a in ACTIONS if acts.get(a, 0)) or 'none'))
    if det[IF_TIER] and acts.get('refloat', 0) == 0:
        print('  NOTE: no refloat actions -- the detector layer is DIAGNOSTIC-ONLY in this run')
    if acts.get('suppressed', 0):
        print('  NOTE: %d event(s) suppressed by the credibility/corroboration gate'
              % acts['suppressed'])
    summary['events'] = {
        'by_tier_event_action': {'|'.join(k): v for k, v in sorted(counts.items())},
        'by_tier': dict(by_tier),
        'by_action_detector_tiers': dict(acts),
        'total': sum(by_tier.values()),
    }


# ---------------------------------------------------------------------------
# (b) IF <-> MW/LLI agreement
# ---------------------------------------------------------------------------
def section_agreement(ev_rows, tol, summary):
    hdr('(b) IF <-> MW/LLI AGREEMENT  [+/-%d epoch tolerance, matched on (station, sat)]' % tol)
    if ev_rows is None:
        print('  [skip] needs network_events.csv, section skipped')
        summary['agreement'] = None
        return
    grid = EpochGrid(ep_key(r) for r in ev_rows)
    if not len(grid):
        print('  [skip] no parseable (week, tow) in events file, section skipped')
        summary['agreement'] = None
        return

    def collect(tier, events):
        out = []
        for r in ev_rows:
            if _s(r, 'tier') != tier or _s(r, 'event') not in events:
                continue
            i = grid.idx(ep_key(r))
            if i is None:
                continue
            out.append((i, _s(r, 'station'), _s(r, 'sat'), _s(r, 'event')))
        return out

    if_ev, mw_ev = collect(IF_TIER, IF_EVENTS), collect(MW_TIER, MW_EVENTS)
    mw_by_key = defaultdict(set)
    for i, sta, sat, _e in mw_ev:
        mw_by_key[(sta, sat)].add(i)
    if_by_key = defaultdict(set)
    for i, sta, sat, _e in if_ev:
        if_by_key[(sta, sat)].add(i)

    def near(idxs, i):
        return any(abs(j - i) <= tol for j in idxs)

    matched = [x for x in if_ev if near(mw_by_key.get((x[1], x[2]), ()), x[0])]
    if_only = [x for x in if_ev if not near(mw_by_key.get((x[1], x[2]), ()), x[0])]
    mw_only = [x for x in mw_ev if not near(if_by_key.get((x[1], x[2]), ()), x[0])]
    rate = len(matched) / len(if_ev) if if_ev else float('nan')

    print('  run epochs (events file): %d   dt0=%.0fs' % (len(grid), grid.dt))
    print('  IF events (%s): %d   MW/LLI events (%s): %d' %
          (IF_TIER, len(if_ev), MW_TIER, len(mw_ev)))
    print('  IF matched by MW/LLI : %d' % len(matched))
    print('  IF-only              : %d' % len(if_only))
    print('  MW/LLI-only          : %d' % len(mw_only))
    print('  match rate           : %s' %
          ('n/a (no IF events)' if not if_ev else '%.1f%%' % (100.0 * rate)))
    if if_ev and not mw_ev:
        print('  NOTE: MW/LLI side is EMPTY -- that detector is off or still warm-gated;')
        print('        the match rate is uninformative, read section (e) instead.')
    summary['agreement'] = {
        'epoch_tol': tol,
        'n_epochs': len(grid),
        'n_if': len(if_ev),
        'n_mw': len(mw_ev),
        'if_matched': len(matched),
        'if_only': len(if_only),
        'mw_only': len(mw_only),
        'match_rate': None if not if_ev else rate,
    }


# ---------------------------------------------------------------------------
# (c) bunching check: per-epoch histogram + top stations / sats
# ---------------------------------------------------------------------------
def section_bunching(ev_rows, top, summary):
    hdr('(c) BUNCHING CHECK  [events per epoch; top-%d stations / satellites]' % top)
    if ev_rows is None:
        print('  [skip] needs network_events.csv, section skipped')
        summary['bunching'] = None
        return
    grid = EpochGrid(ep_key(r) for r in ev_rows)
    per_ep_if, per_ep_mw = Counter(), Counter()
    sta_ct, sat_ct = Counter(), Counter()
    for r in ev_rows:
        tier = _s(r, 'tier')
        if tier not in (IF_TIER, MW_TIER):
            continue
        i = grid.idx(ep_key(r))
        if i is None:
            continue
        (per_ep_if if tier == IF_TIER else per_ep_mw)[i] += 1
        sta_ct[_s(r, 'station', '?')] += 1
        sat_ct[_s(r, 'sat', '?')] += 1
    if not per_ep_if and not per_ep_mw:
        print('  no detector-tier events to histogram')
        summary['bunching'] = None
        return

    tot = sum(per_ep_if.values()) + sum(per_ep_mw.values())
    per_ep_tot = [per_ep_if.get(i, 0) + per_ep_mw.get(i, 0) for i in range(len(grid))]
    live = [i for i in range(len(grid)) if per_ep_tot[i]]
    cap = 4 * top                          # a multi-hour run has thousands of epochs
    shown = live if len(live) <= cap else sorted(
        sorted(live, key=lambda i: -per_ep_tot[i])[:cap])
    if len(shown) < len(live):
        print('  (%d epochs with events; showing the %d busiest, time-ordered)'
              % (len(live), len(shown)))
    print('  %9s %5s %5s' % ('tow', 'IF', 'MW'))
    for i in shown:
        print('  %9.0f %5d %5d  %s' % (grid.tow(i), per_ep_if.get(i, 0), per_ep_mw.get(i, 0),
                                       '#' * min(per_ep_tot[i], 60)))
    nz = [v for v in per_ep_tot if v]
    worst_i = max(range(len(per_ep_tot)), key=lambda i: per_ep_tot[i]) if per_ep_tot else 0
    worst = per_ep_tot[worst_i] if per_ep_tot else 0
    print('  epochs with >=1 event: %d/%d   med/ep=%.1f  max/ep=%d @tow %.0f (%.0f%% of all events)' %
          (len(nz), len(grid), st.median(nz) if nz else 0.0, worst, grid.tow(worst_i),
           100.0 * worst / tot if tot else 0.0))
    if tot and worst / tot > 0.5:
        print('  WARNING: >50%% of all events land in ONE epoch -- bunched, suspect a')
        print('           network-wide re-level / datum event, not N independent slips')
    print('  top stations: ' + ', '.join('%s=%d' % kv for kv in sta_ct.most_common(top)))
    print('  top satellites: ' + ', '.join('%s=%d' % kv for kv in sat_ct.most_common(top)))
    summary['bunching'] = {
        'per_epoch_if': {('%.0f' % grid.tow(i)): n for i, n in sorted(per_ep_if.items())},
        'per_epoch_mw': {('%.0f' % grid.tow(i)): n for i, n in sorted(per_ep_mw.items())},
        'epochs_with_events': len(nz),
        'n_epochs': len(grid),
        'max_per_epoch': worst,
        'max_epoch_share': (worst / tot) if tot else None,
        'top_stations': sta_ct.most_common(top),
        'top_sats': sat_ct.most_common(top),
    }


# ---------------------------------------------------------------------------
# (d) IF-TD sample stats + decisive-margin distribution
# ---------------------------------------------------------------------------
def section_samples(if_rows, summary):
    hdr('(d) IF-TD SAMPLES + DECISIVE MARGIN  [network_if_slip.csv]')
    if if_rows is None:
        print('  [skip] needs network_if_slip.csv, section skipped')
        summary['samples'] = None
        return
    n = len(if_rows)
    flagged = [r for r in if_rows if _i(r.get('flagged'), 0) == 1]
    margins = []
    for r in flagged:
        dev, thr = _f(r.get('abs_dev_m')), _f(r.get('thr_m'))
        if dev is not None and thr not in (None, 0.0):
            margins.append(dev / thr)
    margins.sort()
    print('  opportunities: %d   flags: %d   flag rate: %.3f%%' %
          (n, len(flagged), 100.0 * len(flagged) / n if n else float('nan')))
    if margins:
        print('  margin abs_dev/thr over FLAGGED rows (n=%d):' % len(margins))
        print('    min=%.2f  p5=%.2f  p25=%.2f  med=%.2f  p75=%.2f  p95=%.2f  max=%.2f' %
              (margins[0], _pct(margins, 5), _pct(margins, 25), _pct(margins, 50),
               _pct(margins, 75), _pct(margins, 95), margins[-1]))
        graze = sum(1 for m in margins if m < 1.5) / len(margins)
        print('    grazing (margin < 1.5x): %.0f%%  -- high means threshold-chatter risk' %
              (100.0 * graze))
    else:
        print('  no flagged rows with a usable abs_dev_m/thr_m pair')
    warm = Counter(_s(r, 'carry_warm') or '(blank)' for r in if_rows)
    if warm:
        print('  carry_warm census: ' + ', '.join('%s=%d' % kv for kv in sorted(warm.items())))
    summary['samples'] = {
        'opportunities': n,
        'flags': len(flagged),
        'flag_rate_pct': (100.0 * len(flagged) / n) if n else None,
        'margin': None if not margins else {
            'n': len(margins), 'min': margins[0], 'p5': _pct(margins, 5),
            'p25': _pct(margins, 25), 'p50': _pct(margins, 50), 'p75': _pct(margins, 75),
            'p95': _pct(margins, 95), 'max': margins[-1],
        },
    }


# ---------------------------------------------------------------------------
# raw series: adjacent-epoch steps of the absolute MW / GF logs
# ---------------------------------------------------------------------------
def build_raw_steps(raw_rows):
    """(station, sat) -> {epoch_idx: {'lli': bool, 'dmw': float|None, 'dgf': float|None,
    'elev': float|None}}.

    ``network_slip_raw.csv`` logs absolute MW / GF (metres) per epoch, so the
    step is a difference.  As in grade_ddfrac, a step is taken only between
    epoch-adjacent samples and never across a gap, where a "step" would only be
    the arc restarting.  The step is attributed to the later epoch (the epoch the
    jump lands on, which is where the detector fires)."""
    grid = EpochGrid(ep_key(r) for r in raw_rows)
    lli_ok = any(('lli1' in r or 'lli2' in r) for r in raw_rows[:1])
    per_key = defaultdict(dict)          # (sta,sat) -> idx -> (mw, gf, lli, elev)
    for r in raw_rows:
        i = grid.idx(ep_key(r))
        if i is None:
            continue
        lli = bool((_i(r.get('lli1'), 0) or 0) & 1) or bool((_i(r.get('lli2'), 0) or 0) & 1)
        per_key[(_s(r, 'station'), _s(r, 'sat'))][i] = (
            _f(r.get('mw_m')), _f(r.get('gf_m')), lli, _f(r.get('elev_deg')))
    steps = {}
    for key, ser in per_key.items():
        idxs = sorted(ser)
        out = {}
        for i in idxs:
            mw, gf, lli, elev = ser[i]
            # `elev` is kept for section (g): a re-tap population concentrated at
            # low elevation indicates geometry/iono-structured tension, which needs
            # a different fix from one that is flat in elevation.
            out[i] = {'lli': lli, 'dmw': None, 'dgf': None, 'elev': elev}
        for a, b in zip(idxs, idxs[1:]):
            if b - a != 1:
                continue                  # gap: the arc restarted, not a step
            mwa, gfa = ser[a][0], ser[a][1]
            mwb, gfb = ser[b][0], ser[b][1]
            if mwa is not None and mwb is not None:
                out[b]['dmw'] = mwb - mwa
            if gfa is not None and gfb is not None:
                out[b]['dgf'] = gfb - gfa
        steps[key] = out
    return grid, steps, lli_ok


# ---------------------------------------------------------------------------
# (e) raw adjudication of every IF flag
# ---------------------------------------------------------------------------
def section_adjudicate(ev_rows, raw_rows, grid, steps, lli_ok, tol, lam, mw_cyc, gf_m, summary):
    hdr('(e) RAW ADJUDICATION OF IF FLAGS  [gates: |dMW|>%.2f cyc (%.3f m), |dGF|>%.3f m, LLI bit]'
        % (mw_cyc, mw_cyc * lam, gf_m))
    if ev_rows is None or raw_rows is None:
        print('  [skip] needs both network_events.csv and network_slip_raw.csv, section skipped')
        summary['adjudication'] = None
        return
    mw_gate_m = mw_cyc * lam
    verdicts = Counter()
    unsupported = defaultdict(list)      # (sta,sat) -> [tow, ...]
    reasons = Counter()
    per_event = defaultdict(Counter)
    flags = [r for r in ev_rows if _s(r, 'tier') == IF_TIER]
    for r in flags:
        sta, sat, ev = _s(r, 'station'), _s(r, 'sat'), _s(r, 'event')
        i = grid.nearest(ep_key(r))
        ser = steps.get((sta, sat))
        if i is None or not ser:
            verdicts['no-series'] += 1
            per_event[ev]['no-series'] += 1
            continue
        window = [ser[j] for j in range(i - tol, i + tol + 1) if j in ser]
        if not window:
            verdicts['no-series'] += 1
            per_event[ev]['no-series'] += 1
            continue
        hit = []
        # "testable" means at least one of the three tests had data to reject with.
        # An arc-start epoch has a row but no step; with no LLI columns either, it
        # counts as no-series (untested), not as unsupported.
        testable = lli_ok
        for w in window:
            if w['dmw'] is not None:
                testable = True
                if abs(w['dmw']) > mw_gate_m:
                    hit.append('MW')
            if w['dgf'] is not None:
                testable = True
                if abs(w['dgf']) > gf_m:
                    hit.append('GF')
            if w['lli']:
                hit.append('LLI')
        if hit:
            verdicts['supported'] += 1
            per_event[ev]['supported'] += 1
            reasons['+'.join(sorted(set(hit)))] += 1
        elif testable:
            verdicts['unsupported'] += 1
            per_event[ev]['unsupported'] += 1
            unsupported[(sta, sat)].append(_f(r.get('tow'), float('nan')))
        else:
            verdicts['no-series'] += 1
            per_event[ev]['no-series'] += 1

    n = len(flags)
    print('  %s flags adjudicated: %d   (supported %d / unsupported %d / no-series %d)' %
          (IF_TIER, n, verdicts['supported'], verdicts['unsupported'], verdicts['no-series']))
    if n:
        print('  raw-confirmed rate: %.0f%% of all flags, %.0f%% of flags with a series' %
              (100.0 * verdicts['supported'] / n,
               100.0 * verdicts['supported'] / max(verdicts['supported'] + verdicts['unsupported'], 1)))
    for ev in sorted(per_event):
        c = per_event[ev]
        print('    by event  %-14s supported=%-4d unsupported=%-4d no-series=%d' %
              (ev, c['supported'], c['unsupported'], c['no-series']))
    if reasons:
        print('  support witnessed by: ' +
              ', '.join('%s=%d' % kv for kv in reasons.most_common()))
    if unsupported:
        print('  UNSUPPORTED flags grouped by (station, sat)  [false-positive families]:')
        for (sta, sat), tows in sorted(unsupported.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            print('    %-10s %-4s n=%-3d tow: %s' %
                  (sta, sat, len(tows), ' '.join('%.0f' % t for t in tows)))
    else:
        print('  no unsupported flags')
    summary['adjudication'] = {
        'gates': {'mw_cyc': mw_cyc, 'mw_m': mw_gate_m, 'gf_m': gf_m, 'lambda_wl': lam,
                  'epoch_tol': tol},
        'n_flags': n,
        'supported': verdicts['supported'],
        'unsupported': verdicts['unsupported'],
        'no_series': verdicts['no-series'],
        'by_event': {k: dict(v) for k, v in per_event.items()},
        'support_reasons': dict(reasons),
        'unsupported_families': {'%s|%s' % k: v for k, v in unsupported.items()},
    }


# ---------------------------------------------------------------------------
# (f) reverse sweep: raw steps the IF detector never flagged
# ---------------------------------------------------------------------------
def section_reverse(ev_rows, raw_rows, grid, steps, tol, lam, gates, top, summary):
    hdr('(f) REVERSE SWEEP  [raw |dMW| candidates vs IF flags, +/-%d epoch]' % tol)
    if raw_rows is None:
        print('  [skip] needs network_slip_raw.csv, section skipped')
        summary['reverse'] = None
        return
    flag_idx = defaultdict(set)
    if ev_rows is not None:
        for r in ev_rows:
            if _s(r, 'tier') != IF_TIER:
                continue
            i = grid.nearest(ep_key(r))
            if i is not None:
                flag_idx[(_s(r, 'station'), _s(r, 'sat'))].add(i)
    else:
        print('  NOTE: no events file -- every candidate counts as a miss')

    out = {}
    for g in gates:
        thr_m = g * lam
        cands, hits, miss = [], [], []
        for (sta, sat), ser in steps.items():
            fi = flag_idx.get((sta, sat), ())
            for i, w in ser.items():
                if w['dmw'] is None or abs(w['dmw']) <= thr_m:
                    continue
                c = (sta, sat, i, w['dmw'] / lam)
                cands.append(c)
                (hits if any(abs(j - i) <= tol for j in fi) else miss).append(c)
        hits.sort(key=lambda c: -abs(c[3]))
        miss.sort(key=lambda c: -abs(c[3]))
        print('  |dMW| > %-4g cyc : %4d candidate(s), %4d IF-flagged, %4d missed  (%s)' %
              (g, len(cands), len(hits), len(miss),
               'n/a' if not cands else '%.0f%% caught' % (100.0 * len(hits) / len(cands))))
        if hits:
            print('    hits  : ' + ', '.join('%s/%s@%.0f(%.1fc)' % (h[0], h[1], grid.tow(h[2]), h[3])
                                             for h in hits[:top]) +
                  ('' if len(hits) <= top else '  ... +%d' % (len(hits) - top)))
        if miss:
            print('    misses (top %d by |dMW|; a huge step is usually gap-coincident '
                  're-acquisition, not a missed slip):' % top)
            for sta, sat, i, dmw_c in miss[:top]:
                print('      %-10s %-4s tow=%-9.0f dMW=%+9.2f cyc' % (sta, sat, grid.tow(i), dmw_c))
            if len(miss) > top:
                print('      ... +%d more' % (len(miss) - top))
        out['%g' % g] = {
            'candidates': len(cands), 'if_flagged': len(hits), 'missed': len(miss),
            'caught_frac': (len(hits) / len(cands)) if cands else None,
            'miss_top': [{'station': m[0], 'sat': m[1], 'tow': grid.tow(m[2]),
                          'dmw_cyc': m[3]} for m in miss[:top]],
        }
    summary['reverse'] = {'lambda_wl': lam, 'epoch_tol': tol, 'gates': out}


# ---------------------------------------------------------------------------
# (g) D5 re-tap attribution  [network_d5_flags.csv]
# ---------------------------------------------------------------------------
# Column order of the producer's dump, oldest schema first.  Used only to recover a
# segment whose header line is not in the file (see read_d5_segments).  Producer
# builds up to 2026-07-30 opened the dump in append mode and wrote the header only
# when the file was empty, so a log dir reused across runs holds several runs' rows
# under one header; after the `raw_stepped` column (card D5-A) was appended, that
# header can be one column short of the rows below it.
D5_COLS_V1 = ('week', 'tow', 'group', 'chord_u', 'chord_v', 'station', 'amb_key',
              'std_innov', 'ewl_stasat', 'hold_chord', 'injected_prev')
D5_COLS_V2 = D5_COLS_V1 + ('raw_stepped',)


def read_d5_segments(path, label='network_d5_flags.csv'):
    """Parse the D5 flag dump into per-run segments (list of list-of-dict).

    Segmentation exists for archived runs.  Until 2026-07-30 the producer
    re-opened ``network_d5_flags.csv`` per epoch in ``std::ios::app`` and never
    truncated it, so a log directory that was not wiped between runs accumulated
    several runs in one file, while the run-end counters that a caller
    cross-checks against describe only the last of them.  Grading the whole file
    against those counters would report a disagreement where the data are fine,
    so the rows are split into segments.  The producer now owns the stream in
    NetworkRunTraces and opens it truncating once per run (a re-run replaces the
    file), so a current run yields exactly one segment and the split is a no-op.
    Old log dirs are still graded, and a multi-run file is not read as one long
    run.  Splits are made where:

      * a header line ('week,...') starts a new segment and (re)defines the
        columns; this also handles the pre/post ``raw_stepped`` schema change;
      * a row whose (week, tow) goes backwards relative to the segment's maximum
        starts a new segment, since epochs only advance within a run;
      * a row whose field count differs from the current column map starts a new
        segment, its columns inferred by width (the schema-change case in a file
        with no second header).

    Returns (segments, note) where `note` is a human-readable provenance string.
    """
    if not os.path.exists(path):
        print('  [skip] %s: file not found (%s), section skipped' % (label, path))
        return None, ''
    try:
        with open(path, newline='', encoding='utf-8', errors='replace') as f:
            rows = list(csv.reader(f))
    except OSError as e:
        print('  [skip] %s: unreadable (%s), section skipped' % (label, e))
        return None, ''
    rows = [r for r in rows if r and any(c.strip() for c in r)]
    if not rows:
        print('  [skip] %s: no data rows (%s), section skipped' % (label, path))
        return None, ''

    segments, cols, cur, max_key, n_hdr = [], None, [], None, 0

    def start(new_cols):
        nonlocal cols, cur, max_key
        if cur:
            segments.append(cur)
        cols, cur, max_key = new_cols, [], None

    for r in rows:
        cells = [c.strip() for c in r]
        if cells[0] == 'week':                       # explicit header line
            n_hdr += 1
            start(tuple(cells))
            continue
        if cols is None or len(cells) != len(cols):  # headerless / schema change
            start(D5_COLS_V2 if len(cells) >= len(D5_COLS_V2) else D5_COLS_V1)
        row = dict(zip(cols, cells))
        k = ep_key(row)
        if k is not None:
            if max_key is not None and k < max_key:  # epochs went backwards: new run
                start(cols)
            max_key = k if max_key is None else max(max_key, k)
        cur.append(row)
    if cur:
        segments.append(cur)
    note = '%d segment(s) [%d header line(s)]: %s' % (
        len(segments), n_hdr,
        ', '.join('#%d n=%d' % (i, len(s)) for i, s in enumerate(segments)))
    return segments, note


def parse_run_end_d5(path):
    """Best-effort scrape of the D5 numbers the producer printed at run end.

    Three independent lines, all optional: the Card-G attribution pair (against
    which the section cross-checks its own join), the gate's flag/re-tap totals
    with k, and the final sigma0 inflation of the D5-v2 LS-VCE calibration (the
    gate threshold is k*sigma0 formal-sigma).  Reading them from the log avoids
    retyping them, and a disagreement between these and the CSV is itself a
    finding; see the notes printed by the section."""
    out = {}
    if not os.path.exists(path):
        return out
    # Two accepted shapes for the Card-G attribution line: the current one, which
    # states both units (flag rows and distinct funnel arcs), and the pre-2026-07-22
    # one, which stated only rows.  The second keeps archived runs gradeable.
    pats = (
        ('retaps', 'retaps_stepped',
         re.compile(r'D5 attribution:\s*(\d+)\s*FLAG ROW\(s\) of which\s*(\d+)\s*coincident')),
        ('retaps', 'retaps_stepped',
         re.compile(r'D5 attribution:\s*(\d+)\s*re-tap\(s\),\s*(\d+)\s*coincident')),
        ('retap_ambs', 'retap_ambs_stepped',
         re.compile(r'=\s*(\d+)\s*distinct funnel arc\(s\) re-tapped\s*\((\d+)')),
        ('gate_flags', 'gate_retaps',
         re.compile(r'(\d+)\s*carried-DD innovation\s*flag\(s\),\s*(\d+)\s*fresh re-tap')),
    )
    k_re     = re.compile(r're-tap\(s\) over the run \(k=([0-9.]+)')
    infl_re  = re.compile(r'final empirical inflation[^=]*=\s*([0-9.]+)')
    thr_re   = re.compile(r'gate threshold is\s*k\S{0,6}\s*=\s*([0-9.]+)\s*formal')
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                for a, b, rx in pats:
                    m = rx.search(line)
                    if m:
                        out[a], out[b] = int(m.group(1)), int(m.group(2))
                for key, rx in (('k', k_re), ('sigma0', infl_re), ('gate_sigma', thr_re)):
                    m = rx.search(line)
                    if m:
                        out[key] = float(m.group(1))
    except OSError:
        return out
    return out


def _runs(sorted_idx):
    """Longest run of consecutive epoch indices in a sorted unique list."""
    best = run = 1 if sorted_idx else 0
    for a, b in zip(sorted_idx, sorted_idx[1:]):
        run = run + 1 if b - a == 1 else 1
        best = max(best, run)
    return best


def warm_boundary(if_rows, grid, pl_warmup):
    """First epoch index at which the phase-leveling carry is warm.

    The preferred source is the producer's ``carry_warm`` column in
    ``network_if_slip.csv``: it is the flag the run used, so it cannot disagree
    with the run the way a re-derivation of `pl_warmup` from config can.  Falls
    back to the ``--pl-warmup`` count (epochs), then to None (split not reported,
    because a guessed boundary would mislabel the whole population)."""
    all_cold = False
    if if_rows:
        has_col = any('carry_warm' in r for r in if_rows[:1])
        warm = [grid.nearest(ep_key(r)) for r in if_rows
                if _i(r.get('carry_warm'), 0) == 1]
        warm = [i for i in warm if i is not None]
        if warm:
            return min(warm), 'carry_warm column of network_if_slip.csv'
        all_cold = has_col          # column present, never 1 -> the whole run is cold
    if pl_warmup is not None and pl_warmup >= 0:
        return int(pl_warmup), '--pl-warmup (config value, supplied by the caller)'
    return None, ('the whole run is COLD (carry_warm never 1 in network_if_slip.csv)'
                  if all_cold else '')


def section_d5(segments, seg_note, seg_sel, grid, steps, if_rows,
               tol, lam, mw_cyc, gf_m, top, pl_warmup, run_end, summary):
    # The caller prints the section header before the file is read, so a missing
    # dump prints its skip note under the right heading.
    if not segments:
        summary['d5'] = None
        return
    print('  dump provenance: %s' % seg_note)
    if seg_sel == 'all':
        rows, which = [r for s in segments for r in s], 'ALL segments'
    else:
        if seg_sel == 'last':
            idx = len(segments) - 1
        else:
            try:                          # a bad value must not end the report
                idx = max(0, min(int(seg_sel), len(segments) - 1))
            except (TypeError, ValueError):
                print('  [warn] --d5-segment %r is not last|all|N — grading the last segment'
                      % (seg_sel,))
                idx = len(segments) - 1
        rows, which = segments[idx], 'segment #%d' % idx
    print('  grading %s (%d flag row(s)); the run-end counters describe the LAST run only'
          % (which, len(rows)))
    if not rows:
        summary['d5'] = None
        return

    # The reference epoch grid is the raw one when available: it holds every
    # processed epoch, so a consecutive-run length is measured in epochs the
    # estimator saw.  The D5 file alone holds only the epochs that flagged, which
    # would make every isolated flag look like a run of one and every
    # second-epoch flag look consecutive.
    d5_grid = EpochGrid(ep_key(r) for r in rows)
    ref_grid, grid_src = ((grid, 'network_slip_raw.csv (processed epochs)')
                          if len(grid) else (d5_grid, 'the D5 file itself (flagged epochs only —'
                                                      ' run lengths are UPPER bounds)'))
    print('  epoch grid: %s; %d D5 epoch(s), %d grid epoch(s)'
          % (grid_src, len(d5_grid), len(ref_grid)))

    # --- population + per-row attribution ---------------------------------
    n = len(rows)
    # Any row, not only the first: under --d5-segment all, a pre-D5-A segment can
    # precede a post-D5-A one, and the census must not depend on that order.
    have_col = any('raw_stepped' in r for r in rows)
    col_ct = Counter()                    # exact-epoch producer tag
    join_ct = Counter()                   # independent +/-tol offline join
    fam = defaultdict(set)                # (sta,sat) -> {epoch idx}
    innov, innov_step, innov_flat, innov_ns = [], [], [], []
    dup_ct = Counter()                    # (epoch, group, chord) -> flag rows
    elev_bin = defaultdict(lambda: [0, 0, []])   # bin -> [n, n_stepped, innovs]
    witness = Counter()                   # what made the offline join say "stepped"
    tags = Counter()
    warm_ct = Counter()
    wb, wb_src = warm_boundary(if_rows, ref_grid, pl_warmup)
    for r in rows:
        sta = _s(r, 'station')
        amb = _s(r, 'amb_key')
        sat = amb.split('_', 1)[0] if amb else ''
        i = ref_grid.nearest(ep_key(r))
        v = _f(r.get('std_innov'))
        if v is not None:
            innov.append(v)
        if have_col:
            col_ct[_s(r, 'raw_stepped') or '(blank)'] += 1
        for t in ('ewl_stasat', 'hold_chord', 'injected_prev'):
            if _i(r.get(t), 0) == 1:
                tags[t] += 1
        dup_ct[(ep_key(r), _s(r, 'group'), _s(r, 'chord_u'), _s(r, 'chord_v'))] += 1
        if sat and i is not None:
            fam[(sta, sat)].add(i)
        if wb is not None and i is not None:
            warm_ct['warm' if i >= wb else 'cold'] += 1

        # Independent join: the producer's tag is exact-epoch, this one carries
        # the spec's +/-1-epoch tolerance (a step attributed to the epoch the
        # jump lands on can flag one epoch either side).
        ser = steps.get((sta, sat)) if sat else None
        verdict, elev = 'no-series', None
        if ser and i is not None:
            window = [ser[j] for j in range(i - tol, i + tol + 1) if j in ser]
            here = ser.get(i)
            if here is not None:
                elev = here.get('elev')
            testable = False
            hit = []
            for w in window:
                if w['dmw'] is not None:
                    testable = True
                    if abs(w['dmw']) > mw_cyc * lam:
                        hit.append('MW')
                if w['dgf'] is not None:
                    testable = True
                    if abs(w['dgf']) > gf_m:
                        hit.append('GF')
                if w['lli']:
                    hit.append('LLI')
            verdict = 'stepped' if hit else ('flat' if testable else 'no-series')
            if hit:
                witness['+'.join(sorted(set(hit)))] += 1
        join_ct[verdict] += 1
        # Bucket by verdict, not by negation: a 'no-series' row carries no raw
        # evidence, and folding it into the flat population would turn the
        # stepped-vs-flat comparison into a comparison against "flat + unknown".
        # That comparison is the statistic card D5-A reads to decide whether a
        # slip class exists that the raw observables do not show.
        if v is not None:
            if verdict == 'stepped':
                innov_step.append(v)
            elif verdict == 'flat':
                innov_flat.append(v)
            else:
                innov_ns.append(v)
        if elev is not None:
            b = max(0, min(8, int(elev // 10)))
            elev_bin[b][0] += 1
            if verdict == 'stepped':
                elev_bin[b][1] += 1
            if v is not None:
                elev_bin[b][2].append(v)

    n_chords = len(dup_ct)
    n_dup = n - n_chords
    print('  re-tap population: %d flag ROW(s) = %d distinct (epoch, group, chord) ARC re-tap(s)'
          '%s; %d distinct (station, sat); %d epoch(s)'
          % (n, n_chords,
             '' if not n_dup else '  [+%d duplicate row(s), %.0f%% — a WL-3 joint pair block '
                                  'flags per BAND, so a chord flagged on both bands writes two '
                                  'rows and re-taps ONCE]' % (n_dup, 100.0 * n_dup / n),
             len(fam), len(d5_grid)))
    if have_col:
        print('  producer exact-epoch tag (raw_stepped): ' +
              ', '.join('%s=%d' % (k, v) for k, v in sorted(col_ct.items())) +
              '   [1 = raw MW/GF stepped, 0 = measurable and flat, blank = no evidence]')
    else:
        print('  producer exact-epoch tag: ABSENT (pre-D5-A dump) — the join below stands alone')
    st, fl, ns = join_ct['stepped'], join_ct['flat'], join_ct['no-series']
    testable_n = st + fl
    print('  offline join (+/-%d ep, |dMW|>%.2f cyc / |dGF|>%.3f m / LLI): '
          'stepped=%d, flat=%d, no-series=%d' % (tol, mw_cyc, gf_m, st, fl, ns))
    if witness:
        print('    witnessed by: ' + ', '.join('%s=%d' % kv for kv in witness.most_common()))
    if have_col and col_ct.get('1', 0) != st:
        # The two numbers answer different questions and both are reported.  The
        # producer's tag judges the exact epoch with the empirical per-station
        # gate max(k*sigma_hat, physical floor); this join judges +/-tol epochs
        # with the audit's fixed gates and also accepts a bare LLI bit.  The join
        # is therefore the permissive bound and the tag the strict one; a large
        # spread between them reflects the raw noise floor at those stations, not
        # a disagreement about the data.
        print('    NOTE: producer tag %d vs join %d — different questions: exact-epoch EMPIRICAL '
              'per-station gate vs +/-%d-epoch FIXED audit gates incl. a bare LLI bit. Read the '
              'tag as the strict floor and the join as the permissive ceiling.'
              % (col_ct.get('1', 0), st, tol))
    print('  RAW-STEP-COINCIDENT FRACTION: %.1f%% of all flags, %.1f%% of flags with a series'
          % (100.0 * st / n if n else float('nan'),
             100.0 * st / testable_n if testable_n else float('nan')))
    if testable_n and st / testable_n < 0.10:
        print('    => the gate is firing overwhelmingly on MODEL TENSION, not on anything the')
        print('       receiver observables can see (Card D5-A: this is the decomposition that')
        print('       the Iono-2 cross-experiment must move — see docs/SPEC_slip_qc_hardening.md §10.3)')

    # --- cross-check against the run-end counters --------------------------
    if run_end.get('retaps') is not None:
        rt, rs = run_end['retaps'], run_end['retaps_stepped']
        d_rows = n - rt
        print('  cross-check vs run-end counters: d5_retaps=%d (CSV rows %d, delta %+d), '
              'd5_retaps_stepped=%d (CSV exact-epoch 1s %s)'
              % (rt, n, d_rows, rs, col_ct.get('1', 'n/a') if have_col else 'n/a'))
        if run_end.get('retap_ambs') is not None:
            print('    arc basis: d5_retap_ambs=%d (CSV distinct (epoch,group,chord) %d, '
                  'delta %+d), d5_retap_ambs_stepped=%d'
                  % (run_end['retap_ambs'], n_chords, n_chords - run_end['retap_ambs'],
                     run_end['retap_ambs_stepped']))
        if d_rows:
            print('    NOTE: rows != counter is EXPECTED when (a) this is not the last run in an')
            print('          appended file, or (b) some flags did not resolve in the reverse map')
            print('          (the producer warns once and skips those for the counter, but the')
            print('          dump still writes their row with a blank amb_key).')
    elif run_end:
        print('  cross-check vs run-end counters: the Card-G attribution line is absent from '
              'main_log.log (network.cycle_slip.gap_instrument off?)')

    # --- std_innov distribution vs the gate --------------------------------
    innov.sort()
    gate = run_end.get('gate_sigma')
    if gate is None and run_end.get('k') is not None:
        gate = run_end['k'] * run_end.get('sigma0', 1.0)
    if innov:
        print('  std_innov over flagged rows (n=%d): min=%.2f p25=%.2f med=%.2f p75=%.2f '
              'p90=%.2f max=%.2f' % (len(innov), innov[0], _pct(innov, 25), _pct(innov, 50),
                                     _pct(innov, 75), _pct(innov, 90), innov[-1]))
        if gate:
            graze = sum(1 for v in innov if v < 1.5 * gate) / len(innov)
            print('    gate k*sigma0 = %.2f formal-sigma (k=%s, sigma0=%s, run-end values); '
                  'median margin %.2fx; %.0f%% within 1.5x of the gate'
                  % (gate, run_end.get('k', '?'), run_end.get('sigma0', '?'),
                     _pct(innov, 50) / gate, 100.0 * graze))
            print('    [a population piled just above the gate is threshold-grazing: a global '
                  'sigma0 re-scale would move it wholesale. A population far above it is not a '
                  'scaling problem — it is structure the model does not represent.]')
        else:
            print('    (gate threshold not found in main_log.log — pass --d5-gate to compare)')
    if innov_step and innov_flat:
        print('    stepped rows: med=%.2f (n=%d)   flat rows: med=%.2f (n=%d)%s  '
              '-- a REAL slip class should sit higher than the flat population'
              % (_pct(sorted(innov_step), 50), len(innov_step),
                 _pct(sorted(innov_flat), 50), len(innov_flat),
                 '' if not innov_ns else '   no-series rows (no raw evidence, excluded from '
                                         'the comparison): med=%.2f (n=%d)'
                                         % (_pct(sorted(innov_ns), 50), len(innov_ns))))

    # --- top families + consecutive runs -----------------------------------
    fam_rank = sorted(fam.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    print('  top (station, sat) re-tap families [n = distinct epochs, run = longest '
          'consecutive stretch]:')
    for (sta, sat), idxs in fam_rank[:top]:
        srt = sorted(idxs)
        print('    %-10s %-4s n=%-4d run=%-3d first/last tow: %.0f / %.0f'
              % (sta, sat, len(srt), _runs(srt), ref_grid.tow(srt[0]), ref_grid.tow(srt[-1])))
    if fam_rank:
        runs_all = [_runs(sorted(v)) for v in fam.values()]
        runs_all.sort()
        n_sustained = sum(1 for r in runs_all if r >= 3)
        print('    families=%d; longest run over all families=%d; med run=%.1f; '
              '%d family(ies) with a run >= 3 epochs (%.0f%%)'
              % (len(fam), runs_all[-1], _pct(runs_all, 50), n_sustained,
                 100.0 * n_sustained / len(runs_all)))
        print('    [sustained runs => the tension is a persistent per-arc misfit (the Iono-2 '
              'representation hypothesis); impulsive singletons => an event-like population.]')

    # --- elevation profile (D5-v3 candidate: per-elevation-bin scale) ------
    if elev_bin:
        print('  elevation profile of the re-tap population [10-deg bins]:')
        for b in sorted(elev_bin):
            cnt, stc, iv = elev_bin[b]
            iv_s = sorted(iv)
            print('    %2d-%2d deg  n=%-5d stepped=%-4d med std_innov=%s'
                  % (10 * b, 10 * b + 10, cnt, stc,
                     ('%.2f' % _pct(iv_s, 50)) if iv_s else 'n/a'))

    # --- warm / cold split --------------------------------------------------
    if wb is not None:
        print('  warm/cold split at epoch index %d [source: %s]: cold=%d, warm=%d'
              % (wb, wb_src, warm_ct.get('cold', 0), warm_ct.get('warm', 0)))
    elif wb_src:
        print('  warm/cold split: every re-tap is COLD — %s.  A cold run\'s re-tap population is '
              'not comparable with a warm run\'s (this is exactly why Card W exists).' % wb_src)
    else:
        print('  warm/cold split: NOT determined (no carry_warm column, no --pl-warmup) — '
              'a cold run\'s re-taps are not comparable with a warm run\'s')
    if tags:
        print('  adjacency tags on the flagged chords: ' +
              ', '.join('%s=%d' % kv for kv in tags.most_common()))

    summary['d5'] = {
        'segments': len(segments), 'graded': which, 'rows': n,
        'arc_retaps': n_chords, 'duplicate_rows': n_dup,
        'grid_source': grid_src,
        'raw_stepped_column': dict(col_ct) if have_col else None,
        'join': dict(join_ct),
        'coincident_frac_all': (st / n) if n else None,
        'coincident_frac_testable': (st / testable_n) if testable_n else None,
        'run_end': run_end,
        'std_innov': None if not innov else {
            'n': len(innov), 'min': innov[0], 'p25': _pct(innov, 25),
            'p50': _pct(innov, 50), 'p75': _pct(innov, 75), 'p90': _pct(innov, 90),
            'max': innov[-1], 'gate': gate,
            'p50_stepped': _pct(sorted(innov_step), 50) if innov_step else None,
            'p50_flat': _pct(sorted(innov_flat), 50) if innov_flat else None,
            'p50_no_series': _pct(sorted(innov_ns), 50) if innov_ns else None,
            'n_stepped': len(innov_step), 'n_flat': len(innov_flat),
            'n_no_series': len(innov_ns),
        },
        'families': [{'station': k[0], 'sat': k[1], 'epochs': len(v),
                      'longest_run': _runs(sorted(v))} for k, v in fam_rank[:top]],
        'n_families': len(fam),
        'elevation_bins': {('%d-%d' % (10 * b, 10 * b + 10)):
                           {'n': v[0], 'stepped': v[1],
                            'med_std_innov': _pct(sorted(v[2]), 50) if v[2] else None}
                           for b, v in sorted(elev_bin.items())},
        'warm_split': None if wb is None else {
            'boundary_epoch_idx': wb, 'source': wb_src,
            'cold': warm_ct.get('cold', 0), 'warm': warm_ct.get('warm', 0)},
        'tags': dict(tags),
    }


# ---------------------------------------------------------------------------
# (h) run-end counters from main_log.log (best effort)
# ---------------------------------------------------------------------------
def section_counters(path, summary):
    hdr('(h) RUN-END COUNTERS  [main_log.log, verbatim]')
    if not os.path.exists(path):
        print('  [skip] main_log.log: file not found (%s), section skipped' % path)
        summary['counters'] = None
        return
    lines, run_end = [], None
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if _RUN_END_START in line:
                    run_end = len(lines)  # the run-end block starts here (the last one wins)
                if 'Network (' in line and 'slip' in line.lower():
                    if any(tag in line for tag in _TIMING_NOISE):
                        continue          # per-epoch timing chatter, not a counter
                    lines.append(line.rstrip('\n'))
    except OSError as e:
        print('  [skip] main_log.log unreadable (%s), section skipped' % e)
        summary['counters'] = None
        return
    # The run-end counters are every match inside the run-end block, read in message order:
    # the run end prints by subsystem (card NOM2 step TD2), so its order is not a reading
    # key.  A log without the block start (an older producer) keeps the last 12 matches.
    picked = lines[run_end:] if run_end is not None else lines[-12:]
    picked = sorted(picked, key=lambda l: l.split('] ', 3)[-1])
    if not picked:
        print('  no "Network (... slip ...)" counter lines found')
    for line in picked:
        print('  ' + line)
    summary['counters'] = picked


# ---------------------------------------------------------------------------
def main(argv=None):
    # Counter lines carry non-ASCII (sigma, Delta) that a cp1252 console cannot
    # encode; force UTF-8 so printing them cannot abort the report.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_dir = os.path.join(repo, 'build', 'x64-Release', 'logs')
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('logdir', nargs='?', default=default_dir,
                   help='run log directory (default: %(default)s)')
    p.add_argument('--json', dest='json_out', metavar='PATH',
                   help='also dump the machine-readable summary here')
    p.add_argument('--epoch-tol', type=int, default=1, metavar='N',
                   help='epoch tolerance for every match/lookup (default: %(default)s)')
    p.add_argument('--top', type=int, default=10, metavar='N',
                   help='list length for top stations/sats and miss lists (default: %(default)s)')
    p.add_argument('--lambda-wl', type=float, default=LAMBDA_WL_DEFAULT, metavar='M',
                   help='widelane wavelength in m (default: %(default)s = GPS L1/L2)')
    p.add_argument('--mw-gate-cyc', type=float, default=0.5, metavar='C',
                   help='|dMW| support gate in WL cycles (default: %(default)s)')
    p.add_argument('--gf-gate-m', type=float, default=0.04, metavar='M',
                   help='|dGF| support gate in m (default: %(default)s)')
    p.add_argument('--reverse-gates', default='1.5,2,3', metavar='C,C,...',
                   help='reverse-sweep |dMW| gates in WL cycles (default: %(default)s)')
    p.add_argument('--d5', dest='d5_path', metavar='PATH',
                   help='D5 flag dump to attribute in section (g) '
                        '(default: <logdir>/network_d5_flags.csv)')
    p.add_argument('--d5-segment', default='last', metavar='last|all|N',
                   help='which run segment of an APPENDED d5 dump to grade; the run-end '
                        'counters describe the last one (default: %(default)s)')
    p.add_argument('--d5-gate', type=float, default=None, metavar='S',
                   help='D5 gate threshold in formal sigma (k*sigma0); overrides the value '
                        'scraped from main_log.log')
    p.add_argument('--pl-warmup', type=int, default=None, metavar='N',
                   help='phase-leveling warmup in epochs, for the section (g) warm/cold split '
                        'when network_if_slip.csv is absent')
    a = p.parse_args(argv)

    gates = [g for g in (_f(x) for x in a.reverse_gates.split(',')) if g is not None]
    if not gates:
        p.error('--reverse-gates parsed to nothing usable: %r' % a.reverse_gates)

    print('slip-agreement grade: %s' % a.logdir)
    if not os.path.isdir(a.logdir):
        print('  NOTE: log directory does not exist -- every section will be skipped')
    print('  epoch-tol=%d  lambda_wl=%.4f m  mw-gate=%.2f cyc  gf-gate=%.3f m  reverse-gates=%s'
          % (a.epoch_tol, a.lambda_wl, a.mw_gate_cyc, a.gf_gate_m,
             ','.join('%g' % g for g in gates)))

    ev_rows = read_rows(os.path.join(a.logdir, 'network_events.csv'), 'network_events.csv')
    if_rows = read_rows(os.path.join(a.logdir, 'network_if_slip.csv'), 'network_if_slip.csv')
    raw_rows = read_rows(os.path.join(a.logdir, 'network_slip_raw.csv'), 'network_slip_raw.csv')

    summary = {'logdir': os.path.abspath(a.logdir),
               'params': {'epoch_tol': a.epoch_tol, 'lambda_wl': a.lambda_wl,
                          'mw_gate_cyc': a.mw_gate_cyc, 'gf_gate_m': a.gf_gate_m,
                          'reverse_gates': gates, 'top': a.top}}

    section_events(ev_rows, summary)
    section_agreement(ev_rows, a.epoch_tol, summary)
    section_bunching(ev_rows, a.top, summary)
    section_samples(if_rows, summary)
    if raw_rows is not None:
        rgrid, steps, lli_ok = build_raw_steps(raw_rows)
    else:
        rgrid, steps, lli_ok = EpochGrid([]), {}, False
    section_adjudicate(ev_rows, raw_rows, rgrid, steps, lli_ok, a.epoch_tol, a.lambda_wl,
                       a.mw_gate_cyc, a.gf_gate_m, summary)
    section_reverse(ev_rows, raw_rows, rgrid, steps, a.epoch_tol, a.lambda_wl,
                    gates, a.top, summary)

    # (g) D5 attribution.  The dump is opt-in at the producer
    # (network.mw_wl.d5_dump), so like every other input its absence just skips
    # the section.  main_log.log is read for the run-end cross-check numbers.
    log_path = os.path.join(a.logdir, 'main_log.log')
    hdr('(g) D5 RE-TAP ATTRIBUTION  [network_d5_flags.csv x network_slip_raw.csv]')
    d5_path = a.d5_path or os.path.join(a.logdir, 'network_d5_flags.csv')
    d5_segs, d5_note = read_d5_segments(d5_path)
    if d5_segs:
        run_end = parse_run_end_d5(log_path)
        if a.d5_gate is not None:
            run_end['gate_sigma'] = a.d5_gate
        section_d5(d5_segs, d5_note, a.d5_segment, rgrid, steps, if_rows,
                   a.epoch_tol, a.lambda_wl, a.mw_gate_cyc, a.gf_gate_m, a.top,
                   a.pl_warmup, run_end, summary)
    else:
        summary['d5'] = None

    section_counters(log_path, summary)

    if a.json_out:
        try:                              # never discard a finished report over a bad path
            with open(a.json_out, 'w') as f:
                json.dump(summary, f, indent=2, default=str)
            print('\nwrote %s' % a.json_out)
        except OSError as e:
            print('\n[warn] could not write %s: %s' % (a.json_out, e))

    # `d5_segs` counts as a graded input: network.mw_wl.d5_dump is an opt-in
    # independent of the trace flags, so a log dir holding only the D5 dump is a
    # valid, fully graded configuration.  Reporting "nothing graded" (and exiting
    # 1) after printing a complete section (g) would be wrong, and a harness that
    # keys off the exit status would call a good run a failure.
    if ev_rows is None and if_rows is None and raw_rows is None and not d5_segs:
        print('\nnothing graded: no input file found under %s' % a.logdir)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
