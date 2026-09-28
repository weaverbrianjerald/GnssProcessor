#!/usr/bin/env python3
"""Event harness: the slip-dose grader (Study 4, docs/SPEC_event_harness.md §12).

``scripts/event_grade.py`` prices one event on one arc; ``event_frame_grade.py``
prices what that one event did to the station the arc belongs to.  A dose leg
(``scripts/ev_dose_legs.py``) injects a schedule instead: tens to hundreds of true
slips over a 240-epoch leg, on one station or the whole roster.  The question is
then whether a converged network stays converged under the campaign's slip rate,
and which station channel degrades first.  That needs a grader that folds every
injected delta into the truth and reports per-station trajectories rather than
one arc's aftermath.

    python scripts/event_dose_grade.py runs/evleg_<dose> --null runs/evleg_<null> \
        --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h \
        [--stations S21000SIM,...] [--bin 20] [--pre 0] [--post N] [--ref-free]         [--json out.json]

The epoch axis is the leg's own: k = 0 is ``leg_meta['first_tow']``, the first
processed epoch, because a dose leg has no single event tow to hang an axis on.
(The single-event graders count from the event; on a leg whose schedule holds one
slip at ``ep``, this grader's +k is their +(k - ep).)  The schedule is read from
``leg_meta['events']`` when the generator wrote one, else from the leg config's
``network.inject.events``; only ``kind == "slip"`` events fold into the truth.

Both legs resume from the same snapshot with the same config except for
``network.inject``, so the diff column (event - null) cancels the warm-resume
seam transient, the bed and the posture, leaving the dose's own signature.

Channels, per relative epoch, for each dosed station and for the bystander pool
(every other station in the traces), event leg and null leg, then the diff:

  lvl_all    median over the station's arcs of |DD level error| in cycles, where an
             arc is a (sat, band) present in ``logs/network_dd_fractional.csv`` and
             its per-epoch value is the chord-median over the chords that touch the
             station (the ``station_level_errors`` reduction of event_frame_grade.py).
             The truth carries the cumulative injected delta of every scheduled slip
             on that (station, sat, signal) up to the epoch, so a perfectly
             responding producer reads ~0 on every arc and an unresponsive one reads
             the dose.  ``lvl_p90`` is the 90th percentile of the same population,
             ``n_off`` the arcs over 0.5 cyc, ``n_arcs`` the population size, and
             ``L1C/L2W/L5Q/L7Q`` the same median split by band.
             The DD trace carries one reference satellite and its error is in every
             chord (SPEC_amb_iono §6.12).  The diff cancels it only to the extent
             the two legs share it, and on a dose leg they do not: the reference
             satellite's own arcs are slipped and re-levelled at many stations, so
             its error lands in every chord of the event leg and in none of the
             null's.  ``--ref-free`` removes it with the re-referencing of
             scripts/simnet_dd_reref.py (per (ref_sat, sta_a, sta_b, signal) group of
             >= 3 satellites, u_i = e_i - median_i e_i, applied to the raw chords
             before the per-station reduction).  It changes every lvl_* channel, the
             per-band split and the per-slip residual; zwd / clk / ion / cons / wire
             are station-frame and untouched.  The default is as-traced.
  lvl_inj    the same median over the schedule's own slipped arcs only, each counted
             from its first injected slip onward.  The station's remaining arcs split
             in two, because a detector refloat frees the whole satellite, not only
             the slipped band (one L1C slip fires the MW pair L1C;L5Q and the IF-TD
             pair L1C;L2W, i.e. all three bands):
               ``isb`` (lvl_injsat_otherbands): the non-slipped bands of a dosed
                        satellite at that station, from that satellite's first slip.
                        Response collateral, not a control.
               ``oth``: arcs on satellites the schedule never touched at that
                        station.  This is the clean control: "did the station drift".
             An arc of a dosed satellite before its first slip is in none of the
             three (it is not yet dosed, and putting it in ``oth`` would contaminate
             the control half).
  zwd        ZWD error vs ``truth_tropo.csv``, metres; pool = rms over stations.
  clk        receiver-clock contrast error, ``network_rx_states.csv`` (kind=CLK)
             minus ``truth_clock.csv`` minus the per-epoch network mean.  The
             network-common half is the receiver-clock datum and is not a station
             property (standing invariant #5).  Pool = rms over stations.
  ion        rms over the station's LOS of ``iono_total_m`` (the absolute column)
             minus ``truth_atmo.csv`` ``I_L1_m``, metres.  Pool = rms over stations.
  cons       the closed-loop consumer's 3D error, ``closed_loop_trace.csv``;
             network-wide, one series.
  wire       rms across satellites of the emitted satellite-clock difference
             (event - null) after its per-epoch mean is removed: the datum-free
             half of what the dose did to the product.
  qc         ``logs/network_events.csv`` rows for the station, split into rows on an
             injected arc (station+sat is in the schedule, at or after its tow) and
             rows on the station's other arcs, with a whole-leg census keyed by
             (tier, event, action) and classed INJECT / T4 / T3 / RAIM / DETECT
             (anything else is "other").

Two blocks follow the tables, and they are what a dose leg is read for:

  PER-SLIP RESIDUAL   one row per injected slip: the signed level diff (event -
             null) of the slipped arc at +1, +2, +5, +10, +20, +30, +45 epochs after
             that slip's own tow, plus the detector rows on its (station, satellite)
             within +0..+3 with their detail field.  This is the price of each slip.
  COVERAGE   per dosed station, epochs with DD chords per constellation, and per
             injected arc whether it was measurable at all.  The trace differences
             against one reference satellite per constellation and a chord exists
             only while the station sees it, so an arc can be absent from every
             level median rather than zero in it.  The level channels are therefore
             coverage-limited, while zwd / clk / ion / cons / wire are
             trace-complete.

Folding applies to the event leg only: the null leg carries no slip, so its truth
is the fixture's unfolded truth and the per-arc diff is
``(dd_event - truth_folded) - (dd_null - truth_unfolded)``.  Folding the null too
would make the diff read the injected delta itself instead of the residual.

Reductions: the binned table takes the median of the continuous channels within
each bin and the median of the per-epoch population counts (``n_arcs``,
``n_off``), but sums the QC counts, because events are sparse and their median is
0 by construction.

The output is an instrument reading, not a verdict; the verdict is the operator's.
"""
import argparse
import bisect
import csv
import json
import math
import os
import sys
import time
from collections import defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from simnet_amb_integer import load_truth, n_at, pct                    # noqa: E402
from event_grade import (median, leg_meta, rel_ep, fmt, truth_series,   # noqa: E402
                         consumer_series, diff, EPOCH_S)
from event_frame_grade import wire_all_clocks, wire_all_stats           # noqa: E402

BANDS = ("L1C", "L2W", "L5Q", "L7Q")
OFF_CYC = 0.5                      # an arc is "off" past this many cycles
SLIP_OFFSETS = (1, 2, 5, 10, 20, 30, 45)   # per-slip residual read-out offsets
_MISS = object()

# ---------------------------------------------------------------------------
# The per-station channels below (`zwd_all`, `rx_clock_all`, `iono_all`) are the
# all-station single-pass forms of event_grade.zwd_error and
# event_frame_grade.rx_clock_contrast / los_iono_error.  The definitions are
# unchanged; only the loop is inverted.  A dose leg grades up to the whole roster,
# and the single-station helpers re-read the trace (and, for the clock, every
# station's truth) once per station, and los_iono_error once per (station,
# satellite) over a 16 MB trace, which takes minutes rather than seconds at roster
# scale.  Truth files are cached per (fixture, station) because both legs of the
# A/B read the same fixture.
# ---------------------------------------------------------------------------
_TRUTH_ATMO = {}


def _die(msg):
    raise SystemExit("event_dose_grade: " + msg)


def truth_atmo(fixture, sta, lo_tow, hi_tow):
    """{sat: {tow_int: I_L1_m}} for `sta`, window only.

    truth_atmo.csv is ~7 MB per station and sorted by tow_s; the leg is a few
    hundred epochs of a 24 h file, so the read stops at the window's end.
    """
    key = (fixture, sta)
    if key in _TRUTH_ATMO:
        return _TRUTH_ATMO[key]
    p = os.path.join(fixture, sta, "truth_atmo.csv")
    out = {}
    if os.path.isfile(p):
        with open(p, newline="") as f:
            rd = csv.reader(f)
            hdr = next(rd, None) or []
            ci = {n: i for i, n in enumerate(hdr)}
            if "tow_s" in ci and "satellite" in ci and "I_L1_m" in ci:
                it, isat, iv = ci["tow_s"], ci["satellite"], ci["I_L1_m"]
                for r in rd:
                    try:
                        t = float(r[it])
                    except (IndexError, ValueError):
                        continue
                    if t > hi_tow + 1.0:
                        break
                    if t < lo_tow - 1.0:
                        continue
                    try:
                        out.setdefault(r[isat], {})[int(round(t))] = float(r[iv])
                    except (IndexError, ValueError):
                        continue
    _TRUTH_ATMO[key] = out
    return out


# ------------------------------------------------------------------ schedule
def load_schedule(legdir, t0):
    """(events, source, error): the leg's injected schedule, each event carrying `ep`.

    ``leg_meta['events']`` is what scripts/ev_dose_legs.py records; a leg built by
    scripts/event_leg.py alone has no such key, so the leg config's
    ``network.inject.events`` (leg.json) is the fallback.  A null leg has neither.
    """
    meta = leg_meta(legdir)
    evs, src = meta.get("events"), "leg_meta.json['events']"
    if not evs:
        p = os.path.join(legdir, "leg.json")
        if not os.path.isfile(p):
            return [], src, f"missing {p}"
        with open(p, encoding="utf-8") as f:
            cfg = json.load(f)
        inj = (cfg.get("network") or {}).get("inject") or {}
        if not isinstance(inj, dict):
            inj = {}
        raw = inj.get("events") or []
        if isinstance(raw, dict):
            raw = [raw]
        evs = [] if inj.get("enable") is False else list(raw)
        src = "leg.json network.inject.events"
    out = []
    for e in evs:
        if not isinstance(e, dict):
            continue
        e = dict(e)
        try:
            e["ep"] = int(e["ep"]) if "ep" in e else rel_ep(float(e["tow"]), t0)
        except (KeyError, TypeError, ValueError):
            e["ep"] = None
        out.append(e)
    out.sort(key=lambda e: (e.get("tow", 0.0), str(e.get("station")), str(e.get("sat"))))
    return out, src, None


def build_fold(events):
    """(fold, inj_tow, sat_tow): the truth folding table and the two onset maps.

    ``fold[(sta, sat, sig)] = (tows, cum)`` with ``cum[i]`` the sum of the first i
    deltas, so the delta carried at a row tow is ``cum[bisect_right(tows, tow+0.25)]``,
    with the same 0.25 s tolerance event_grade.py applies to its single event.
    ``inj_tow`` is each slipped arc's first slip; ``sat_tow[(sta, sat)]`` the first
    slip anywhere on that satellite at that station, because a detector refloat
    frees the satellite's other bands too (one L1C slip fires the MW pair L1C;L5Q
    and the IF-TD pair L1C;L2W), so those bands are not a clean control either.
    """
    per = defaultdict(list)
    for e in events:
        if e.get("kind") != "slip":
            continue
        try:
            key = (e["station"], e["sat"], e["signal"])
            per[key].append((float(e["tow"]), float(e["delta_cycles"])))
        except (KeyError, TypeError, ValueError):
            continue
    fold, inj_tow, sat_tow = {}, {}, {}
    for key, v in per.items():
        v.sort()
        tows, cum, s = [t for t, _ in v], [0.0], 0.0
        for _, d in v:
            s += d
            cum.append(s)
        fold[key] = (tows, cum)
        inj_tow[key] = tows[0]
        sk = (key[0], key[1])
        sat_tow[sk] = min(tows[0], sat_tow.get(sk, tows[0]))
    return fold, inj_tow, sat_tow


# --------------------------------------------------------------------- levels
def dd_levels(legdir, fixture, t0, lo, hi, fold, inj_tow, sat_tow, keep_signed,
              ref_free=False):
    """One pass over network_dd_fractional.csv -> every station's level channels.

    Returns ({sta: channels}, {arc: signed per-epoch chord-median},
    {sta: {constellation: set of epochs with chords}}, (rf_graded, rf_skipped), err).
    Each epoch's chords are reduced as they are read and the raw lists dropped, so
    the memory is one epoch of chords no matter how long the leg is.

    Coverage: the trace differences every satellite against one reference per
    constellation and a chord exists only for a station pair that both see it, so a
    station can have chords on one constellation and none on the other, and an arc
    can vanish mid-leg.  The returned coverage map keeps an unmeasurable arc from
    dropping unnoticed out of a median.

    ``ref_free`` applies the SPEC_amb_iono §6.12 re-referencing before the chords are
    reduced, in the same way as scripts/simnet_dd_reref.py and simnet_amb_integer.py
    ``--ref-free``: the epoch's chords are grouped by (ref_sat, sta_a, sta_b,
    signal); a group of at least 3 satellites gives ``c = median_i e_i`` and every
    chord becomes ``u_i = e_i - c``; groups with fewer than 3 satellites are skipped
    and counted.  Since ``e_i = s_i - s_ref``, the reference satellite's own
    between-station error is common to the group and cancels.  This matters on a
    dose leg, where the reference's own arcs are slipped and re-levelled at many
    stations, so its error does not cancel in the event - null difference either.
    """
    path = os.path.join(legdir, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(path):
        return {}, {}, {}, (0, 0), ("no network_dd_fractional.csv "
                                    "(arm network.trace_dd_fractional)")
    n_int, slips = load_truth(fixture, set())
    loaded = set()

    def ensure(s):
        if s in loaded:
            return
        loaded.add(s)
        ni, sl = load_truth(fixture, [s])
        n_int.update(ni)
        for k, v in sl.items():
            slips[k] = v

    out, signed, cover = {}, defaultdict(dict), defaultdict(lambda: defaultdict(set))
    rf = [0, 0]                        # groups graded (>= 3 sats), groups skipped

    def blank():
        return {"all": {}, "p90": {}, "n_off": {}, "n_arcs": {}, "inj": {},
                "injsat_oth": {}, "oth": {}, "band": {b: {} for b in BANDS}}

    def flush(k, cur):
        tow = t0 + k * EPOCH_S
        per_sta = defaultdict(dict)
        for (sta, sat, sig), v in cur.items():
            per_sta[sta][(sat, sig)] = median(v)
        for sta, arcs in per_sta.items():
            st = out.setdefault(sta, blank())
            av, inj, isb, oth = [], [], [], []
            by_band = defaultdict(list)
            for (sat, sig), m in arcs.items():
                if m is None:
                    continue
                a = abs(m)
                av.append(a)
                by_band[sig].append(a)
                cover[sta][sat[0]].add(k)
                key = (sta, sat, sig)
                s_on = sat_tow.get((sta, sat))
                if key in inj_tow:
                    if tow >= inj_tow[key] - 0.25:
                        inj.append(a)
                    if keep_signed:
                        signed[key][k] = m
                elif s_on is None:
                    oth.append(a)                     # the schedule never touched it
                elif tow >= s_on - 0.25:
                    isb.append(a)                     # a freed band of a dosed satellite
            if not av:
                continue
            st["all"][k] = median(av)
            st["p90"][k] = pct(av, 0.9)
            st["n_arcs"][k] = float(len(av))
            st["n_off"][k] = float(sum(1 for a in av if a > OFF_CYC))
            if inj:
                st["inj"][k] = median(inj)
            if isb:
                st["injsat_oth"][k] = median(isb)
            if oth:
                st["oth"][k] = median(oth)
            for b in BANDS:
                if by_band.get(b):
                    st["band"][b][k] = median(by_band[b])

    with open(path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd, None)
        if not hdr:
            return {}, {}, {}, (0, 0), f"{path}: empty"
        ci = {n: i for i, n in enumerate(hdr)}
        for need in ("tow", "signal", "sat_i", "ref_sat", "sta_a", "sta_b", "dd_cyc"):
            if need not in ci:
                return {}, {}, {}, (0, 0), f"{path}: missing column {need}"
        i_tow, i_sig, i_sat, i_ref = ci["tow"], ci["signal"], ci["sat_i"], ci["ref_sat"]
        i_a, i_b, i_dd = ci["sta_a"], ci["sta_b"], ci["dd_cyc"]
        cur, cur_k, memo, seen, dup = defaultdict(list), None, {}, set(), 0
        gbuf = defaultdict(dict)      # (ref, sa, sb, sig) -> {sat_i: e}, ref-free only

        def expand(g):
            """Re-reference one epoch's chords (§6.12) and fan them out per station."""
            c2 = defaultdict(list)
            for (_ref, sa, sb, sig), d in g.items():
                if len(d) < 3:
                    rf[1] += 1
                    continue
                rf[0] += 1
                c = median(list(d.values()))
                for sat, e in d.items():
                    u = e - c
                    c2[(sa, sat, sig)].append(u)
                    c2[(sb, sat, sig)].append(-u)
            return c2

        def n_true(s, v, g, tow):
            key = (s, v, g)
            r = memo.get(key, _MISS)
            if r is not _MISS:
                return r
            ensure(s)
            n = n_at(n_int, slips, s, v, g, tow)
            if n is not None:
                fd = fold.get(key)
                if fd:
                    n += fd[1][bisect.bisect_right(fd[0], tow + 0.25)]
            memo[key] = n
            return n

        for r in rd:
            try:
                tow = float(r[i_tow])
            except (IndexError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            if k != cur_k:
                if cur_k is not None:
                    flush(cur_k, expand(gbuf) if ref_free else cur)
                    seen.add(cur_k)
                if k in seen:
                    dup += 1
                cur, gbuf, cur_k, memo = defaultdict(list), defaultdict(dict), k, {}
            sat_i, ref = r[i_sat], r[i_ref]
            if ref == sat_i:
                continue
            sa, sb, sig = r[i_a], r[i_b], r[i_sig]
            legs = (n_true(sa, sat_i, sig, tow), n_true(sa, ref, sig, tow),
                    n_true(sb, sat_i, sig, tow), n_true(sb, ref, sig, tow))
            if None in legs:
                continue
            try:
                e = float(r[i_dd]) - ((legs[0] - legs[1]) - (legs[2] - legs[3]))
            except ValueError:
                continue
            if ref_free:
                gbuf[(ref, sa, sb, sig)][sat_i] = e
            else:
                cur[(sa, sat_i, sig)].append(e)
                cur[(sb, sat_i, sig)].append(-e)
        if cur_k is not None:
            flush(cur_k, expand(gbuf) if ref_free else cur)
    err = (f"{path}: {dup} out-of-order epoch block(s) — per-epoch medians may be "
           f"split" if dup else None)
    return (out, {k: dict(v) for k, v in signed.items()},
            {s: {c: set(v) for c, v in per.items()} for s, per in cover.items()},
            tuple(rf), err)


# -------------------------------------------------------- per-station states
def zwd_all(legdir, fixture, t0, lo, hi):
    """{sta: {rel_ep: ZWD error, m}}, the all-station form of zwd_error."""
    p = os.path.join(legdir, "logs", "network_zwd.csv")
    if not os.path.isfile(p):
        return {}, "no network_zwd.csv (arm network.trace_zwd)"
    out, tcache = defaultdict(dict), {}
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        scol = next((c for c in cols if c in ("station", "sta")), None)
        vcol = next((c for c in cols if "zwd" in c.lower() and "sig" not in c.lower()),
                    None)
        if not scol or not vcol:
            return {}, f"network_zwd.csv: no station/zwd columns in {cols}"
        for row in rd:
            try:
                tow = float(row["tow"])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            sta = row[scol]
            if sta not in tcache:
                tcache[sta] = truth_series(fixture, sta, "truth_tropo.csv",
                                           "tow_s", "zwd_m") or {}
            t = tcache[sta].get(int(round(tow)))
            if t is None:
                continue
            try:
                out[sta][k] = float(row[vcol]) - t
            except ValueError:
                continue
    return dict(out), None


def rx_clock_all(legdir, fixture, t0, lo, hi):
    """{sta: {rel_ep: clock contrast error, m}}, the all-station rx_clock_contrast."""
    p = os.path.join(legdir, "logs", "network_rx_states.csv")
    if not os.path.isfile(p):
        return {}, "no network_rx_states.csv (arm network.trace_rx_states)"
    est = defaultdict(dict)
    with open(p, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("kind") != "CLK":
                continue
            try:
                tow = float(row["tow"])
                v = float(row["value_m"])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if lo <= k <= hi:
                est[k][row["station"]] = v
    tcache = {}

    def tclk(s, tow_i):
        if s not in tcache:
            tcache[s] = truth_series(fixture, s, "truth_clock.csv",
                                     "tow_s", "clock_bias_m") or {}
        return tcache[s].get(tow_i)

    out = defaultdict(dict)
    for k, per in est.items():
        tow_i = int(round(t0 + k * EPOCH_S))
        errs = {}
        for s, v in per.items():
            t = tclk(s, tow_i)
            if t is not None:
                errs[s] = v - t
        if not errs:
            continue
        mu = sum(errs.values()) / len(errs)
        for s, e in errs.items():
            out[s][k] = e - mu
    return dict(out), None


def iono_all(legdir, fixture, t0, lo, hi):
    """{sta: {rel_ep: rms over LOS of slant-iono error, m}}, the all-station los_iono_error."""
    p = os.path.join(legdir, "logs", "network_ionosphere.csv")
    if not os.path.isfile(p):
        return {}, "no network_ionosphere.csv (arm network.trace_ionosphere)"
    lo_tow, hi_tow = t0 + lo * EPOCH_S, t0 + hi * EPOCH_S
    acc = defaultdict(lambda: defaultdict(list))
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        if "iono_total_m" not in cols:
            return {}, ("network_ionosphere.csv has no iono_total_m column — this "
                        "binary predates the §7.8 frame fix; iono_m alone is a "
                        "CORRECTION, not the slant ionosphere")
        for row in rd:
            try:
                tow = float(row["tow"])
                v = float(row["iono_total_m"])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            sta, sat = row.get("station", ""), row.get("sat", "")
            t = truth_atmo(fixture, sta, lo_tow, hi_tow).get(sat, {}).get(int(round(tow)))
            if t is not None:
                acc[sta][k].append(v - t)
    out = {}
    for sta, per in acc.items():
        out[sta] = {k: math.sqrt(sum(x * x for x in v) / len(v))
                    for k, v in per.items() if v}
    return out, None


# ------------------------------------------------------------------------ qc
def qc_classes(tier, event):
    t, e = (tier or "").upper(), (event or "").lower()
    if t == "INJECT":
        return "INJECT"
    if "T4" in t or "attrib" in e:
        return "T4"
    if "T3" in t:
        return "T3"
    if "RAIM" in t:
        return "RAIM"
    if t in ("T0_MW_DETECT", "T1_IF_SLIP", "LLI") or "lli" in e:
        return "DETECT"
    return "other"


def qc_all(legdir, t0, lo, hi, inj_sat):
    """QC rows per station, split on/off the schedule's arcs.

    ``inj_sat[(sta, sat)]`` is that arc's first injected tow; a row is "on an
    injected arc" when its (station, sat) is in that map at or after the tow.
    Returns ({sta: {'inj'|'oth': {rel_ep: count}}},
             {sta: {(tier, ev, act): [inj, oth]}},
             {(sta, sat): [(rel_ep, tier, event, action, detail), ...]}, err).  The
    third map is the per-slip detector readout and is kept only for the schedule's
    own (station, satellite) pairs.
    """
    p = os.path.join(legdir, "logs", "network_events.csv")
    if not os.path.isfile(p):
        return {}, {}, {}, "no network_events.csv (arm network.trace_events)"
    per = defaultdict(lambda: {"inj": defaultdict(float), "oth": defaultdict(float)})
    census = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    rows = defaultdict(list)
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        for row in rd:
            sta = (row.get("station") or "").strip()
            if not sta:
                continue                      # epoch- or satellite-level: not a station's
            try:
                tow = float(row["tow"])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            sat = (row.get("sat") or "").strip()
            t_inj = inj_sat.get((sta, sat))
            side = "inj" if (t_inj is not None and tow >= t_inj - 0.25) else "oth"
            per[sta][side][k] += 1.0
            key = (row.get("tier", "?"), row.get("event", "?"), row.get("action", "?"))
            census[sta][key][0 if side == "inj" else 1] += 1
            if (sta, sat) in inj_sat:
                rows[(sta, sat)].append((k, key[0], key[1], key[2],
                                         (row.get("detail") or "")))
    return ({s: {k: dict(v) for k, v in d.items()} for s, d in per.items()},
            {s: {k: list(v) for k, v in d.items()} for s, d in census.items()},
            dict(rows), None)


# -------------------------------------------------------------------- reduce
def pool_stat(per_sta, stations, how):
    """{rel_ep: stat} over `stations`: 'median', 'rms' or 'sum' across stations."""
    acc = defaultdict(list)
    for s in stations:
        for k, v in (per_sta.get(s) or {}).items():
            if v is not None:
                acc[k].append(v)
    if how == "sum":
        return {k: float(sum(v)) for k, v in acc.items()}
    if how == "rms":
        return {k: math.sqrt(sum(x * x for x in v) / len(v)) for k, v in acc.items()}
    return {k: median(v) for k, v in acc.items()}


def binned(series, bins, how="median"):
    out = []
    for b0, b1 in bins:
        v = [series[k] for k in range(b0, b1 + 1)
             if k in series and series[k] is not None]
        out.append(None if not v else (float(sum(v)) if how == "sum" else median(v)))
    return out


def peak(series):
    v = [(abs(x), k) for k, x in series.items() if x is not None]
    if not v:
        return None, None
    _, k = max(v)
    return series[k], k


def demeaned_rms(mean, rms):
    """rms of (d - mean(d)) from wire_all_stats' (mean, rms): var = E[d²] - E[d]²."""
    out = {}
    for k, r in rms.items():
        m = mean.get(k)
        if m is None:
            continue
        out[k] = math.sqrt(max(0.0, r * r - m * m))
    return out


# ---------------------------------------------------------------------- main
def table(title, bins, cols, note=""):
    """cols = [(header, [value per bin], decimals), ...]; values already reduced."""
    print()
    print(f"-- {title}" + (f"   ({note})" if note else ""))
    hdr = f"{'bin':>10}" + "".join(f"{h:>9}" for h, _v, _d in cols)
    print(hdr)
    print("-" * len(hdr))
    for i, (b0, b1) in enumerate(bins):
        lab = f"+{b0}..+{b1}"
        print(f"{lab:>10}" + "".join(fmt(v[i], d, 9) for _h, v, d in cols))


def col(h, vals, dec=3):
    return (h, vals, dec)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("legdir")
    ap.add_argument("--null", required=True, help="the matched no-event control leg")
    ap.add_argument("--fixture", required=True, help="simulator fixture (truth files)")
    ap.add_argument("--stations", default=None,
                    help="comma list graded as DOSED instead of the schedule's own "
                         "stations (everything else is a bystander)")
    ap.add_argument("--bin", type=int, default=20, help="epochs per table bin")
    ap.add_argument("--pre", type=int, default=0,
                    help="epochs before the leg's first processed epoch (normally 0 — "
                         "a dose leg's axis starts there)")
    ap.add_argument("--post", type=int, default=None,
                    help="last relative epoch (default: the leg's end)")
    ap.add_argument("--ref-free", action="store_true",
                    help="re-reference the LEVEL channels per (ref_sat, sta_a, sta_b, "
                         "signal) group before reducing them (SPEC_amb_iono §6.12, the "
                         "method of scripts/simnet_dd_reref.py): u_i = e_i - median_i "
                         "e_i over groups of >= 3 satellites, so the trace's single "
                         "reference satellite's own error is not common to every "
                         "chord.  On a dose leg the reference's OWN arcs are slipped "
                         "and re-levelled, so it does not cancel in event - null "
                         "either.  Affects lvl_* only; zwd / clk / ion / cons / wire "
                         "are station-frame and unchanged.")
    ap.add_argument("--max-slip-detail", type=int, default=60,
                    help="print each slip's detector rows (with their detail field) on "
                         "continuation lines while the schedule is no longer than this")
    ap.add_argument("--max-tables", type=int, default=4,
                    help="per-station tables to print; every dosed station still "
                         "gets a summary line and a census")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    t_start = time.time()

    for d in (args.legdir, args.null):
        if not os.path.isdir(d):
            _die(f"no such leg directory: {d}")
    if not os.path.isdir(args.fixture):
        _die(f"no such fixture directory: {args.fixture}")

    meta = leg_meta(args.legdir)
    t0 = meta.get("first_tow")
    if t0 is None:
        _die(f"{args.legdir}/leg_meta.json has no first_tow")
    notes = []
    meta_n = leg_meta(args.null)
    if meta_n.get("first_tow") is not None and \
       abs(float(meta_n["first_tow"]) - float(t0)) > 0.25:
        notes.append(f"the null leg starts at tow {meta_n['first_tow']:.1f}, the leg at "
                     f"{t0:.1f} — the epoch axes are NOT aligned")
    events, src, err = load_schedule(args.legdir, t0)
    if err:
        notes.append("schedule: " + err)
    ev_null, _, _ = load_schedule(args.null, t0)
    if ev_null:
        notes.append(f"the --null leg carries {len(ev_null)} injected event(s) of its "
                     f"own — it is not a clean control")
    slips = [e for e in events if e.get("kind") == "slip"]
    other_kinds = sorted({str(e.get("kind")) for e in events if e.get("kind") != "slip"})
    if other_kinds:
        notes.append("non-slip events listed but NOT folded into the truth: "
                     + ", ".join(other_kinds))

    lo = -abs(args.pre)
    hi = args.post if args.post is not None else int(meta.get("epochs", 240)) + 8
    hi = abs(hi)
    fold, inj_tow, inj_sat = build_fold(slips)

    # ------------------------------------------------------------- read both legs
    chan, rf_counts = {}, {}
    for tag, d in (("e", args.legdir), ("n", args.null)):
        f_fold = fold if tag == "e" else {}
        lv, sg, cov, rfc, err = dd_levels(d, args.fixture, t0, lo, hi, f_fold,
                                          inj_tow, inj_sat, True, args.ref_free)
        if err:
            notes.append(f"lvl[{tag}]: " + err)
        if args.ref_free:
            rf_counts[tag] = rfc
        zw, err = zwd_all(d, args.fixture, t0, lo, hi)
        if err:
            notes.append(f"zwd[{tag}]: " + err)
        ck, err = rx_clock_all(d, args.fixture, t0, lo, hi)
        if err:
            notes.append(f"clk[{tag}]: " + err)
        io, err = iono_all(d, args.fixture, t0, lo, hi)
        if err:
            notes.append(f"ion[{tag}]: " + err)
        qc, cen, qrows, err = qc_all(d, t0, lo, hi, inj_sat)
        if err:
            notes.append(f"qc[{tag}]: " + err)
        cons, _gate, err = consumer_series(d, t0, lo, hi)
        if err:
            notes.append(f"cons[{tag}]: " + err)
        wall, err = wire_all_clocks(d, t0, lo, hi)
        if err:
            notes.append(f"wire[{tag}]: " + err)
        chan[tag] = {"lvl": lv, "signed": sg, "cover": cov, "zwd": zw, "clk": ck,
                     "ion": io, "qc": qc, "census": cen, "qrows": qrows,
                     "cons": cons, "wire": wall}

    w_mean, w_rms = wire_all_stats(chan["e"]["wire"], chan["n"]["wire"])
    wire_d = demeaned_rms(w_mean, w_rms)
    d_cons = diff(chan["e"]["cons"], chan["n"]["cons"])

    universe = set()
    for tag in ("e", "n"):
        for ch in ("lvl", "zwd", "clk", "ion"):
            universe |= set(chan[tag][ch])
    if args.stations:
        dosed = [s.strip() for s in args.stations.split(",") if s.strip()]
        missing = [s for s in dosed if s not in universe]
        if missing:
            notes.append("--stations not present in any trace: " + ", ".join(missing))
    else:
        dosed = sorted({e["station"] for e in slips if e.get("station")})
    dosed_set = set(dosed)
    byst = sorted(universe - dosed_set)

    k_seen = set()
    for tag in ("e", "n"):
        for ch in ("zwd", "clk", "ion"):
            for per in chan[tag][ch].values():
                k_seen |= set(per)
        for st in chan[tag]["lvl"].values():
            k_seen |= set(st["all"])
        k_seen |= set(chan[tag]["cons"])
    if not k_seen:
        _die("no graded epoch in either leg — check --fixture and the leg's traces")
    k_hi = min(hi, max(k_seen))
    step = max(1, args.bin)
    bins = [(b, min(b + step - 1, k_hi)) for b in range(max(0, lo), k_hi + 1, step)]

    # ------------------------------------------------------------------- header
    per_sta_n = defaultdict(int)
    for e in slips:
        per_sta_n[e.get("station")] += 1
    print("EVENT DOSE grade")
    print(f"  leg      {os.path.abspath(args.legdir)}")
    print(f"  null     {os.path.abspath(args.null)}")
    print(f"  fixture  {os.path.abspath(args.fixture)}")
    print(f"  events   {len(events)} ({len(slips)} slip, folded into the truth) on "
          f"{len(per_sta_n)} station(s); {len(inj_tow)} injected arc(s)   [{src}]")
    print(f"  dosed    {len(dosed)}: "
          + (", ".join(f"{s}({per_sta_n.get(s, 0)})" for s in dosed[:12])
             + (" ..." if len(dosed) > 12 else "") if dosed else "(none)"))
    print(f"  bystand  {len(byst)} station(s) in the traces")
    print(f"  t0       tow {t0:.1f} = leg epoch +0; graded +{max(0, lo)}..+{k_hi} "
          f"in bins of {step}")
    print("  units    lvl cycles (3 dp) | zwd, clk metres (4 dp) | ion, cons, wire "
          "metres (3 dp) | qc counts summed per bin")
    if args.ref_free:
        print(f"  lvl mode REF-FREE (§6.12): chords re-referenced per (ref_sat, sta_a, "
              f"sta_b, signal);")
        print(f"           groups >= 3 sats graded / < 3 skipped — event "
              f"{rf_counts.get('e', (0, 0))[0]}/{rf_counts.get('e', (0, 0))[1]}, "
              f"null {rf_counts.get('n', (0, 0))[0]}/{rf_counts.get('n', (0, 0))[1]}")
    else:
        print("  lvl mode AS-TRACED (every chord carries the trace's reference "
              "satellite; --ref-free removes it)")
    print("  ⚠ scope  zwd / clk / ion / cons / wire are TRACE-COMPLETE (every station, "
          "every epoch).")
    print("           The lvl_* channels are COVERAGE-LIMITED: the DD trace "
          "differences against ONE")
    print("           reference satellite per constellation, so a station has chords "
          "only while it")
    print("           sees that reference — see the COVERAGE block before reading any "
          "level number.")

    # ------------------------------------------------------------- per station
    def lvl_of(tag, sta, what, band=None):
        st = chan[tag]["lvl"].get(sta)
        if not st:
            return {}
        return st["band"].get(band, {}) if band else st[what]

    def station_tables(sta):
        e, n = "e", "n"

        def get(t, w, b=None):
            return lvl_of(t, sta, w, b)

        cols = [col("n_arcs", binned(get(e, "n_arcs"), bins)),
                col("all_e", binned(get(e, "all"), bins)),
                col("all_n", binned(get(n, "all"), bins)),
                col("all_d", binned(diff(get(e, "all"), get(n, "all")), bins)),
                col("p90_e", binned(get(e, "p90"), bins)),
                col("p90_d", binned(diff(get(e, "p90"), get(n, "p90")), bins)),
                col("off_e", binned(get(e, "n_off"), bins)),
                col("off_n", binned(get(n, "n_off"), bins)),
                col("inj_e", binned(get(e, "inj"), bins)),
                col("inj_n", binned(get(n, "inj"), bins)),
                col("inj_d", binned(diff(get(e, "inj"), get(n, "inj")), bins)),
                col("isb_e", binned(get(e, "injsat_oth"), bins)),
                col("isb_n", binned(get(n, "injsat_oth"), bins)),
                col("isb_d", binned(diff(get(e, "injsat_oth"),
                                         get(n, "injsat_oth")), bins)),
                col("oth_e", binned(get(e, "oth"), bins)),
                col("oth_n", binned(get(n, "oth"), bins)),
                col("oth_d", binned(diff(get(e, "oth"), get(n, "oth")), bins))]
        cols += [col(f"{b}_d", binned(diff(get(e, "band", b), get(n, "band", b)), bins))
                 for b in BANDS]
        table(f"LEVELS  {sta}  (cycles)", bins, cols)
        qe = chan[e]["qc"].get(sta, {})
        qn = chan[n]["qc"].get(sta, {})
        cols = [col("zwd_e", binned(chan[e]["zwd"].get(sta, {}), bins), 4),
                col("zwd_n", binned(chan[n]["zwd"].get(sta, {}), bins), 4),
                col("zwd_d", binned(diff(chan[e]["zwd"].get(sta, {}),
                                         chan[n]["zwd"].get(sta, {})), bins), 4),
                col("clk_e", binned(chan[e]["clk"].get(sta, {}), bins), 4),
                col("clk_n", binned(chan[n]["clk"].get(sta, {}), bins), 4),
                col("clk_d", binned(diff(chan[e]["clk"].get(sta, {}),
                                         chan[n]["clk"].get(sta, {})), bins), 4),
                col("ion_e", binned(chan[e]["ion"].get(sta, {}), bins)),
                col("ion_n", binned(chan[n]["ion"].get(sta, {}), bins)),
                col("ion_d", binned(diff(chan[e]["ion"].get(sta, {}),
                                         chan[n]["ion"].get(sta, {})), bins)),
                col("qci_e", binned(qe.get("inj", {}), bins, "sum"), 0),
                col("qci_n", binned(qn.get("inj", {}), bins, "sum"), 0),
                col("qco_e", binned(qe.get("oth", {}), bins, "sum"), 0),
                col("qco_n", binned(qn.get("oth", {}), bins, "sum"), 0)]
        table(f"STATE   {sta}", bins, cols)

    shown = sorted(dosed, key=lambda s: (-per_sta_n.get(s, 0), s))[:max(0, args.max_tables)]
    for sta in shown:
        station_tables(sta)
    if len(dosed) > len(shown):
        print(f"\n  ({len(dosed) - len(shown)} further dosed station(s) not tabled — "
              f"raise --max-tables; all of them are in the summary and the census)")

    def pool_table(title, group):
        if not group:
            return None
        d_all = diff(pool_stat({s: chan["e"]["lvl"][s]["all"] for s in group
                                if s in chan["e"]["lvl"]}, group, "median"),
                     pool_stat({s: chan["n"]["lvl"][s]["all"] for s in group
                                if s in chan["n"]["lvl"]}, group, "median"))
        d_p90 = diff(pool_stat({s: chan["e"]["lvl"][s]["p90"] for s in group
                                if s in chan["e"]["lvl"]}, group, "median"),
                     pool_stat({s: chan["n"]["lvl"][s]["p90"] for s in group
                                if s in chan["n"]["lvl"]}, group, "median"))
        off_e = pool_stat({s: chan["e"]["lvl"][s]["n_off"] for s in group
                           if s in chan["e"]["lvl"]}, group, "sum")
        off_n = pool_stat({s: chan["n"]["lvl"][s]["n_off"] for s in group
                           if s in chan["n"]["lvl"]}, group, "sum")
        d_inj = diff(pool_stat({s: chan["e"]["lvl"][s]["inj"] for s in group
                                if s in chan["e"]["lvl"]}, group, "median"),
                     pool_stat({s: chan["n"]["lvl"][s]["inj"] for s in group
                                if s in chan["n"]["lvl"]}, group, "median"))
        d_oth = diff(pool_stat({s: chan["e"]["lvl"][s]["oth"] for s in group
                                if s in chan["e"]["lvl"]}, group, "median"),
                     pool_stat({s: chan["n"]["lvl"][s]["oth"] for s in group
                                if s in chan["n"]["lvl"]}, group, "median"))
        d_isb = diff(pool_stat({s: chan["e"]["lvl"][s]["injsat_oth"] for s in group
                                if s in chan["e"]["lvl"]}, group, "median"),
                     pool_stat({s: chan["n"]["lvl"][s]["injsat_oth"] for s in group
                                if s in chan["n"]["lvl"]}, group, "median"))
        band_d = {b: diff(pool_stat({s: chan["e"]["lvl"][s]["band"][b] for s in group
                                     if s in chan["e"]["lvl"]}, group, "median"),
                          pool_stat({s: chan["n"]["lvl"][s]["band"][b] for s in group
                                     if s in chan["n"]["lvl"]}, group, "median"))
                  for b in BANDS}
        d_zwd = diff(pool_stat(chan["e"]["zwd"], group, "rms"),
                     pool_stat(chan["n"]["zwd"], group, "rms"))
        d_clk = diff(pool_stat(chan["e"]["clk"], group, "rms"),
                     pool_stat(chan["n"]["clk"], group, "rms"))
        d_ion = diff(pool_stat(chan["e"]["ion"], group, "rms"),
                     pool_stat(chan["n"]["ion"], group, "rms"))
        qi_e = pool_stat({s: chan["e"]["qc"].get(s, {}).get("inj", {}) for s in group},
                         group, "sum")
        qi_n = pool_stat({s: chan["n"]["qc"].get(s, {}).get("inj", {}) for s in group},
                         group, "sum")
        qo_e = pool_stat({s: chan["e"]["qc"].get(s, {}).get("oth", {}) for s in group},
                         group, "sum")
        qo_n = pool_stat({s: chan["n"]["qc"].get(s, {}).get("oth", {}) for s in group},
                         group, "sum")
        cols = [col("all_d", binned(d_all, bins)), col("p90_d", binned(d_p90, bins)),
                col("off_e", binned(off_e, bins)), col("off_n", binned(off_n, bins)),
                col("inj_d", binned(d_inj, bins)), col("isb_d", binned(d_isb, bins)),
                col("oth_d", binned(d_oth, bins))]
        cols += [col(f"{b}_d", binned(band_d[b], bins)) for b in BANDS]
        cols += [col("zwd_d", binned(d_zwd, bins), 4),
                 col("clk_d", binned(d_clk, bins), 4),
                 col("ion_d", binned(d_ion, bins)),
                 col("qci_e", binned(qi_e, bins, "sum"), 0),
                 col("qci_n", binned(qi_n, bins, "sum"), 0),
                 col("qco_e", binned(qo_e, bins, "sum"), 0),
                 col("qco_n", binned(qo_n, bins, "sum"), 0)]
        table(f"{title} ({len(group)} stations)", bins, cols,
              "DIFF (event - null); lvl medians / n_off summed / zwd,clk,ion rms "
              "across stations")
        return {"all_d": d_all, "p90_d": d_p90, "n_off_e": off_e, "n_off_n": off_n,
                "inj_d": d_inj, "isb_d": d_isb, "oth_d": d_oth,
                "band_d": {b: band_d[b] for b in BANDS},
                "zwd_d": d_zwd, "clk_d": d_clk, "ion_d": d_ion,
                "qc_inj_e": qi_e, "qc_inj_n": qi_n, "qc_oth_e": qo_e, "qc_oth_n": qo_n}

    pool_dosed = pool_table("DOSED POOL", dosed) if len(dosed) > 1 else None
    pool_byst = pool_table("BYSTANDER POOL", byst)

    table("NETWORK", bins,
          [col("cons_e", binned(chan["e"]["cons"], bins)),
           col("cons_n", binned(chan["n"]["cons"], bins)),
           col("cons_d", binned(d_cons, bins)),
           col("wire_d", binned(wire_d, bins), 4)],
          "consumer 3D error, m; wire = rms across satellites of the emitted-clock "
          "diff, de-meaned, m")

    # ------------------------------------------------------------------ summary
    last = len(bins) - 1
    print()
    print(f"WHOLE-LEG SUMMARY  (last bin +{bins[last][0]}..+{bins[last][1]})")
    hdr = (f"{'station':<12}{'n_ev':>5}{'all_d':>9}{'max|d|':>9}{'@bin':>6}"
           f"{'inj_d':>9}{'isb_d':>9}{'oth_d':>9}{'zwd_d':>10}{'clk_d':>10}{'ion_d':>9}"
           f"{'qc_inj':>8}{'qc_oth':>8}")
    print(hdr)
    print("-" * len(hdr))
    rows = {}

    def summary_row(label, n_ev, d_all, d_inj, d_isb, d_oth, d_zwd, d_clk, d_ion,
                    qi, qo):
        b_all = binned(d_all, bins)
        pk, pk_k = peak(d_all)
        pk_bin = None
        if pk_k is not None:
            for i, (b0, b1) in enumerate(bins):
                if b0 <= pk_k <= b1:
                    pk_bin = i
                    break
        print(f"{label:<12}{n_ev:>5d}{fmt(b_all[last], 3, 9)}{fmt(pk, 3, 9)}"
              f"{('' if pk_bin is None else '+%d' % bins[pk_bin][0]):>6}"
              f"{fmt(binned(d_inj, bins)[last], 3, 9)}"
              f"{fmt(binned(d_isb, bins)[last], 3, 9)}"
              f"{fmt(binned(d_oth, bins)[last], 3, 9)}"
              f"{fmt(binned(d_zwd, bins)[last], 4, 10)}"
              f"{fmt(binned(d_clk, bins)[last], 4, 10)}"
              f"{fmt(binned(d_ion, bins)[last], 3, 9)}"
              f"{fmt(binned(qi, bins, 'sum')[last], 0, 8)}"
              f"{fmt(binned(qo, bins, 'sum')[last], 0, 8)}")
        return {"n_events": n_ev, "all_d_last": b_all[last], "all_d_peak": pk,
                "all_d_peak_ep": pk_k,
                "inj_d_last": binned(d_inj, bins)[last],
                "isb_d_last": binned(d_isb, bins)[last],
                "oth_d_last": binned(d_oth, bins)[last],
                "zwd_d_last": binned(d_zwd, bins)[last],
                "clk_d_last": binned(d_clk, bins)[last],
                "ion_d_last": binned(d_ion, bins)[last]}

    def g_of(sta):
        def g(t, w):
            return (chan[t]["lvl"].get(sta) or {}).get(w, {})
        return g

    for sta in dosed:
        g = g_of(sta)
        rows[sta] = summary_row(
            sta, per_sta_n.get(sta, 0),
            diff(g("e", "all"), g("n", "all")), diff(g("e", "inj"), g("n", "inj")),
            diff(g("e", "injsat_oth"), g("n", "injsat_oth")),
            diff(g("e", "oth"), g("n", "oth")),
            diff(chan["e"]["zwd"].get(sta, {}), chan["n"]["zwd"].get(sta, {})),
            diff(chan["e"]["clk"].get(sta, {}), chan["n"]["clk"].get(sta, {})),
            diff(chan["e"]["ion"].get(sta, {}), chan["n"]["ion"].get(sta, {})),
            chan["e"]["qc"].get(sta, {}).get("inj", {}),
            chan["e"]["qc"].get(sta, {}).get("oth", {}))
    for label, pool, group in (("POOL:dosed", pool_dosed, dosed),
                               ("POOL:byst", pool_byst, byst)):
        if not pool:
            continue
        rows[label] = summary_row(
            label, sum(per_sta_n.get(s, 0) for s in group), pool["all_d"],
            pool["inj_d"], pool["isb_d"], pool["oth_d"], pool["zwd_d"],
            pool["clk_d"], pool["ion_d"], pool["qc_inj_e"], pool["qc_oth_e"])
    pkc, pkc_k = peak(d_cons)
    print(f"  consumer 3D (event - null): last bin {fmt(binned(d_cons, bins)[last], 3, 0).strip()} m,"
          f" peak {fmt(pkc, 3, 0).strip()} m at +{pkc_k if pkc_k is not None else '-'}"
          f"; wire de-meaned rms last bin "
          f"{fmt(binned(wire_d, bins)[last], 4, 0).strip()} m")

    # ------------------------------------------------------------------- census
    def census_block(title, group):
        agg = defaultdict(lambda: [0, 0, 0, 0])     # ev_inj, ev_oth, nl_inj, nl_oth
        for s in group:
            for key, v in (chan["e"]["census"].get(s) or {}).items():
                agg[key][0] += v[0]
                agg[key][1] += v[1]
            for key, v in (chan["n"]["census"].get(s) or {}).items():
                agg[key][2] += v[0]
                agg[key][3] += v[1]
        if not agg:
            return {}
        print()
        print(f"-- QC CENSUS  {title} ({len(group)} stations)")
        h = (f"{'class':<7}{'tier/event/action':<52}{'ev_inj':>8}{'ev_oth':>8}"
             f"{'nl_inj':>8}{'nl_oth':>8}{'d_inj':>8}{'d_oth':>8}")
        print(h)
        print("-" * len(h))
        for key in sorted(agg, key=lambda k: (-sum(agg[k]), k)):
            v = agg[key]
            cls = qc_classes(key[0], key[1])
            lab = "/".join(str(x) for x in key)
            print(f"{cls:<7}{lab[:52]:<52}{v[0]:>8d}{v[1]:>8d}{v[2]:>8d}{v[3]:>8d}"
                  f"{v[0] - v[2]:>+8d}{v[1] - v[3]:>+8d}")
        by_cls = defaultdict(lambda: [0, 0, 0, 0])
        for key, v in agg.items():
            c = by_cls[qc_classes(key[0], key[1])]
            for i in range(4):
                c[i] += v[i]
        print(f"{'':<7}{'TOTAL by class':<52}")
        for c in sorted(by_cls):
            v = by_cls[c]
            print(f"{c:<7}{'':<52}{v[0]:>8d}{v[1]:>8d}{v[2]:>8d}{v[3]:>8d}"
                  f"{v[0] - v[2]:>+8d}{v[1] - v[3]:>+8d}")
        return {"/".join(str(x) for x in k): v for k, v in agg.items()}

    # ------------------------------------------------------- level coverage
    print()
    print("-- COVERAGE  (level channels only; a station has DD chords only while it "
          "sees the trace's reference satellite for that constellation)")
    h = (f"{'station':<12}{'cons':>6}{'ep_e':>7}{'ep_n':>7}{'first_e':>9}{'last_e':>9}"
         f"   window {max(0, lo)}..{k_hi}")
    print(h)
    print("-" * (len(h) - 10))
    cov_rows = {}
    for sta in dosed:
        cons_all = sorted(set(chan["e"]["cover"].get(sta, {}))
                          | set(chan["n"]["cover"].get(sta, {})))
        if not cons_all:
            print(f"{sta:<12}{'-':>6}{0:>7}{0:>7}{'-':>9}{'-':>9}   NO DD CHORDS AT ALL")
            cov_rows[sta] = {}
            continue
        cov_rows[sta] = {}
        for c in cons_all:
            ke = chan["e"]["cover"].get(sta, {}).get(c, set())
            kn = chan["n"]["cover"].get(sta, {}).get(c, set())
            print(f"{sta:<12}{c:>6}{len(ke):>7}{len(kn):>7}"
                  f"{(min(ke) if ke else '-'):>9}{(max(ke) if ke else '-'):>9}")
            cov_rows[sta][c] = {"n_ep_e": len(ke), "n_ep_n": len(kn),
                                "first_e": (min(ke) if ke else None),
                                "last_e": (max(ke) if ke else None)}
    missing = sorted({c for sta in dosed for c in ("G", "E")
                      if c not in (chan["e"]["cover"].get(sta) or {})})
    if missing:
        print(f"  constellations with NO chords at a dosed station: "
              f"{', '.join(missing)} — level channels cannot see them")

    # ------------------------------------------------------- per-slip residual
    if slips:
        print()
        print("-- PER-SLIP RESIDUAL  (signed level diff event - null of the SLIPPED arc, "
              "cycles, at +N epochs after that slip)")
        h = (f"{'ep':>5} {'station':<11}{'sat':<5}{'sig':<5}{'delta':>8}{'lli':>4}"
             + "".join(f"{('+%d' % o):>8}" for o in SLIP_OFFSETS)
             + f"{'lastc':>7}  detector rows on (sta,sat) within +0..+3")
        print(h)
        print("-" * min(len(h), 170))
        detail = len(slips) <= args.max_slip_detail
        slip_rows = []
        for e in slips:
            key = (e.get("station"), e.get("sat"), e.get("signal"))
            ep = e.get("ep")
            se = chan["e"]["signed"].get(key, {})
            sn = chan["n"]["signed"].get(key, {})
            d = diff(se, sn)
            vals = [d.get(ep + o) if ep is not None else None for o in SLIP_OFFSETS]
            after = [j for j in se if ep is not None and j >= ep]
            last_c = max(se) if se else None
            qr = sorted(r for r in chan["e"]["qrows"].get((key[0], key[1]), [])
                        if ep is not None and ep <= r[0] <= ep + 3)
            if not se and not sn:
                flag = "NOT MEASURABLE — no DD chord on this arc in either leg"
            elif not after:
                flag = f"NO CHORD AFTER ITS SLIP — last chord +{last_c}"
            else:
                flag = ""
            inline = (flag + "  " if flag else "") + (
                "" if detail else
                ";".join(f"+{r[0] - ep}:{r[1]}/{r[2]}/{r[3]}" for r in qr if r[1] != "INJECT")
                or "-")
            print(f"{('+%d' % ep) if ep is not None else '?':>5} {key[0]:<11}{key[1]:<5}"
                  f"{key[2]:<5}{e.get('delta_cycles', 0.0):>+8.0f}"
                  f"{int(bool(e.get('flag_lli'))):>4}"
                  + "".join(fmt(v, 3, 8) for v in vals)
                  + f"{(last_c if last_c is not None else '-'):>7}"
                  + ("  " + inline[:110] if inline else ""))
            if detail:
                for r in qr:
                    if r[1] == "INJECT":
                        continue
                    print(f"{'':>5} {'':<11}{'':<5}{'':<5}{'':>8}{'':>4}"
                          f"      +{r[0] - ep:<3d} {r[1]}/{r[2]}/{r[3]}"
                          + (f"  [{r[4]}]" if r[4] else ""))
            slip_rows.append({"ep": ep, "station": key[0], "sat": key[1],
                              "signal": key[2], "delta_cycles": e.get("delta_cycles"),
                              "flag_lli": bool(e.get("flag_lli")),
                              "measurable": bool(se or sn),
                              "measurable_after_slip": bool(after),
                              "last_chord_ep": last_c,
                              "n_ep_event": len(se), "n_ep_null": len(sn),
                              "resid": {f"+{o}": vals[i]
                                        for i, o in enumerate(SLIP_OFFSETS)},
                              "qc": [{"d_ep": r[0] - ep if ep is not None else None,
                                      "tier": r[1], "event": r[2], "action": r[3],
                                      "detail": r[4]} for r in qr]})
        n_none = sum(1 for r in slip_rows if not r["measurable"])
        n_gone = sum(1 for r in slip_rows
                     if r["measurable"] and not r["measurable_after_slip"])
        if n_none or n_gone:
            print(f"  {n_none} of {len(slip_rows)} injected arcs have NO DD chord at all "
                  f"and {n_gone} have none after their own slip — those arcs are ABSENT "
                  f"from every level median, not zero in it")
    else:
        slip_rows = []

    cen_dosed = census_block("DOSED", dosed)
    cen_byst = census_block("BYSTANDERS", byst)

    print()
    for n in notes:
        print("  note: " + n)
    print(f"  graded in {time.time() - t_start:.1f} s")

    if args.json:
        def ser(d):
            return {str(k): v for k, v in d.items()}

        def sta_block(tag, s):
            st = chan[tag]["lvl"].get(s) or {}
            b = {w: ser(st.get(w, {}))
                 for w in ("all", "p90", "n_off", "n_arcs", "inj", "injsat_oth",
                           "oth")}
            b["band"] = {bd: ser((st.get("band") or {}).get(bd, {})) for bd in BANDS}
            b["zwd"] = ser(chan[tag]["zwd"].get(s, {}))
            b["clk"] = ser(chan[tag]["clk"].get(s, {}))
            b["ion"] = ser(chan[tag]["ion"].get(s, {}))
            b["qc_inj"] = ser(chan[tag]["qc"].get(s, {}).get("inj", {}))
            b["qc_oth"] = ser(chan[tag]["qc"].get(s, {}).get("oth", {}))
            return b

        def pool_block(p):
            out_p = {}
            for k, v in (p or {}).items():
                out_p[k] = ({bd: ser(v[bd]) for bd in v} if k == "band_d" else ser(v))
            return out_p

        out = {
            "leg": os.path.abspath(args.legdir), "null": os.path.abspath(args.null),
            "fixture": os.path.abspath(args.fixture), "t0": t0,
            "window": [max(0, lo), k_hi], "bin": step, "bins": bins,
            "ref_free": bool(args.ref_free),
            "ref_free_groups": {k: list(v) for k, v in rf_counts.items()},
            "events": events, "n_slip": len(slips),
            "dosed": dosed, "bystanders": byst,
            "stations": {s: {tag: sta_block(tag, s) for tag in ("e", "n")}
                         for s in sorted(set(dosed) | set(byst))},
            "arcs_injected": {
                "|".join(k): {"event": ser(chan["e"]["signed"].get(k, {})),
                              "null": ser(chan["n"]["signed"].get(k, {})),
                              "diff": ser(diff(chan["e"]["signed"].get(k, {}),
                                               chan["n"]["signed"].get(k, {}))),
                              "first_tow": inj_tow[k],
                              "delta_cycles": fold[k][1][-1]}
                for k in sorted(inj_tow)},
            "pool_dosed": pool_block(pool_dosed),
            "pool_bystander": pool_block(pool_byst),
            "network": {"cons_e": ser(chan["e"]["cons"]), "cons_n": ser(chan["n"]["cons"]),
                        "cons_d": ser(d_cons), "wire_d": ser(wire_d),
                        "wire_mean": ser(w_mean), "wire_rms": ser(w_rms)},
            "coverage": cov_rows, "slips": slip_rows,
            "summary": rows, "census_dosed": cen_dosed, "census_bystander": cen_byst,
            "notes": notes,
        }
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2, default=str)
        print(f"  json: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
