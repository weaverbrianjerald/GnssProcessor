#!/usr/bin/env python3
"""T4 ATTRIBUTION-ARC CENSUS — what does the T4 "attrib_arc" tier actually erase?

On the synthetic SLIPPED bed the network producer's T4 attribution tier erases
every signal-arc of one (station, satellite) per event.  On
`runs/golden_sim_campaign` that is 7,656 events against 2,452 truth slips, and
only ~53 events land within 90 s after a truth slip.  This script asks what the
other ~99 % are, by measuring the arc's OWN level error immediately BEFORE the
erase and tracking it AFTER.

Three hypotheses:

  H-A  T4 is RIGHT      the erased arc carried |level error| >= 0.5 cyc before
                        the erase (the disease is then the re-level VALUE, not
                        the identification).
  H-B  false alarms     the arc was fine (|error| < 0.25 cyc) before the erase.
  H-C  limit cycle      the pre-erase error was itself CREATED by a previous
                        re-level (detector refloat or T4) on the same
                        (station, sat) minutes earlier, and the new re-level
                        does not improve it.

THE STATISTIC
-------------
`<run>/logs/network_dd_fractional.csv` carries, per epoch / signal / chord, the
ABSOLUTE float DD ambiguity

    dd_cyc = ([N_a,i - N_a,ref] - [N_b,i - N_b,ref])_estimated

and the fixture knows the true integers, so the chord error is

    e = dd_cyc - DD_true,   DD_true formed in PYTHON INTS
      = (err_a,i - err_a,ref) - (err_b,i - err_b,ref)

with err_x,y the arc-level error of station x on satellite y, in cycles.  For a
station S and satellite s the per-chord contribution of S's own arc enters with
+1 when S == sta_a and -1 when S == sta_b, so the SIGNED, sign-corrected chord
median over >= `--min-chords` partners is the arc statistic

    e(S, s, sig, t) = median_partners [ +/- e ]
                    ~ err_S,s - err_S,ref  + (partner-averaged noise)

*** The trace has ONE reference satellite (SPEC_amb_iono §6.12).  err_S,ref is
COMMON to every chord of station S, so the as-traced statistic is NOT the arc's
own error — it is the arc's error MINUS S's reference-arc error.  Differences
in time on one (S, s) are ref-free (err_S,ref cancels only if the reference arc
is stable across the window, which it need not be); ABSOLUTE classification is
not.  This script therefore reports BOTH:

    as-traced  e(S, s, sig, t)                       (what the spec asked for)
    ref-free   e(S, s, sig, t) - median_{s' != s} e(S, s', sig, t)   (LEAVE-ONE-OUT
               over the other satellites at the same station/signal/epoch,
               >= --min-sats of them; this is §6.12's fix)

and the CONTROL population below is measured with the identical statistic, so
the OFF / FINE thresholds are calibrated against the same contamination.  The
leave-one-out form is used precisely because subtracting a group's own median
including the member shrinks |residual| on pure noise (the null trap in
CLAUDE.md's wrapped-fraction note) — the control would absorb that, but LOO
removes it outright.

WHAT IS COMPUTED, per T4 event (S, s, t0) and per signal present for (S, s)
--------------------------------------------------------------------------
 1. e_pre  = the statistic at t0 - 30 s (the last epoch BEFORE the erase).
 2. e_at(+k) for k in --offsets epochs, censored at the NEXT re-level event on
    (S, s) (strictly: an offset epoch at or after the next event is dropped).
 3. AGE since the previous re-level on (S, s) — a prior T4_attrib, or a prior
    T0_mw_detect / T1_if_slip row with action=refloat — bucketed.
 4. TRUTH-SLIP proximity: a truth slip on (S, s, any signal) in (t0-90, t0] and
    in (t0-600, t0].
 5. CLASS by max over signals of |e_pre|: OFF >= 0.5, AMBIGUOUS [0.25, 0.5),
    FINE < 0.25, UNMEASURABLE (no signal with >= --min-chords chords at t0-30).
 6. A CONTROL population of --control-n random (S, s, t) slots with NO re-level
    event on (S, s) within +/- --control-guard s, same statistic — this
    calibrates what "fine" reads on this bed.
 7. An arc-age convergence profile: median |e_at(+k)| vs k = 0..--profile-k,
    pooled over all T4 events (and separately over detector refloats).

Usage:
    python scripts/simnet_t4_census.py runs/golden_sim_campaign \\
        runs/simnet60_campaign_wu93 --json out/e0.json --txt out/e0.txt
    python scripts/simnet_t4_census.py --selftest
"""

import argparse
import bisect
import csv
import json
import math
import os
import random
import sys
from array import array
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at   # noqa: E402  (faithful truth join)

EPOCH_S = 30
DEFAULT_OFFSETS = (0, 1, 2, 5, 10, 20, 40)
REPORT_OFFSETS = (0, 5, 10, 40)
HISTORY_OFFSETS = (-2, -10, -20)   # the arc's own level BEFORE the event (censored at the previous one)
AGE_BUCKETS = (("<2min", 0.0, 120.0), ("2-5min", 120.0, 300.0), ("5-15min", 300.0, 900.0),
               ("15-60min", 900.0, 3600.0), (">60min", 3600.0, float("inf")))
AGE_LABELS = [b[0] for b in AGE_BUCKETS] + ["none"]
CLASSES = ("OFF", "AMBIGUOUS", "FINE", "UNMEASURABLE")
PROX_LABELS = ("slip<=90s", "slip<=600s", "no slip<=600s")
OFF_CYC = 0.5
FINE_CYC = 0.25

DETECTOR_TIERS = ("T0_mw_detect", "T1_if_slip")
T4_TIER = "T4_attrib"


# --------------------------------------------------------------------------
# small stats helpers (medians over tiny lists — no numpy dependency, matching
# the house style of scripts/simnet_amb_integer.py)
# --------------------------------------------------------------------------
def median(vals):
    s = sorted(vals)
    n = len(s)
    if n == 0:
        return None
    m = n // 2
    return s[m] if n % 2 else 0.5 * (s[m - 1] + s[m])


def pct(vals, p):
    s = sorted(vals)
    n = len(s)
    if n == 0:
        return None
    i = min(n - 1, max(0, int(round(p * (n - 1)))))
    return s[i]


def summarize(vals):
    """(n, median, p90) of a list of non-negative magnitudes."""
    if not vals:
        return (0, None, None)
    return (len(vals), median(vals), pct(vals, 0.9))


def fmt_v(v, w=7, d=3):
    return " " * (w - 1) + "-" if v is None else f"{v:{w}.{d}f}"


def classify(e_abs):
    if e_abs is None:
        return "UNMEASURABLE"
    if e_abs >= OFF_CYC:
        return "OFF"
    if e_abs < FINE_CYC:
        return "FINE"
    return "AMBIGUOUS"


# --------------------------------------------------------------------------
# inputs
# --------------------------------------------------------------------------
def load_stations(run_dir):
    """(all stations, held-out stations) from <run>/logs/network_stations.csv."""
    path = os.path.join(run_dir, "logs", "network_stations.csv")
    allsta, held = set(), set()
    if not os.path.isfile(path):
        return allsta, held
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            sta = row.get("station", "").strip()
            if not sta:
                continue
            allsta.add(sta)
            if row.get("role", "").strip() == "held_out":
                held.add(sta)
    return allsta, held


def load_events(run_dir, held):
    """Parse <run>/logs/network_events.csv.

    Returns dict with
      t4        [(tow_int, sat, sta), ...]              T4_attrib / attrib_arc
      det       [(tow_int, sat, sta), ...]              T0/T1 with action=refloat
      relevel   (sta, sat) -> sorted [tow_int]          union of the two above
      anyqc     (sta, sat) -> sorted [tow_int]          every T0/T1/T4 row
      counts    Counter of "tier|event|action"
    """
    path = os.path.join(run_dir, "logs", "network_events.csv")
    if not os.path.isfile(path):
        raise SystemExit(f"missing {path}")
    t4, det = [], []
    relevel, anyqc = defaultdict(list), defaultdict(list)
    counts = Counter()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            tier = row.get("tier", "")
            if tier != T4_TIER and tier not in DETECTOR_TIERS:
                continue
            sat = (row.get("sat") or "").strip()
            sta = (row.get("station") or "").strip()
            if not sat or not sta or sta in held:
                continue
            ti = int(round(float(row["tow"])))
            action = (row.get("action") or "").strip()
            ev = (row.get("event") or "").strip()
            counts[f"{tier}|{ev}|{action}"] += 1
            key = (sta, sat)
            anyqc[key].append(ti)
            if tier == T4_TIER:
                t4.append((ti, sat, sta))
                relevel[key].append(ti)
            elif action == "refloat":
                det.append((ti, sat, sta))
                relevel[key].append(ti)
    for d in (relevel, anyqc):
        for k in d:
            d[k].sort()
    t4.sort()
    det.sort()
    return {"t4": t4, "det": det, "relevel": dict(relevel), "anyqc": dict(anyqc), "counts": counts}


def load_truth_slip_index(fixture_dir, stations):
    """(sta, sat) -> sorted [tow_int] over ALL signals, from truth_slips.csv."""
    idx = defaultdict(list)
    total = 0
    for sta in sorted(stations):
        path = os.path.join(fixture_dir, sta, "truth_slips.csv")
        if not os.path.isfile(path):
            continue
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                idx[(sta, row["satellite"])].append(int(round(float(row["tow_s"]))))
                total += 1
    for k in idx:
        idx[k].sort()
    return dict(idx), total


# --------------------------------------------------------------------------
# the DD trace pass: chord errors, sign-corrected per station
# --------------------------------------------------------------------------
def scan_dd(run_dir, fixture_dir, held, min_chords, verbose=True):
    """One streaming pass over network_dd_fractional.csv.

    Returns (med, center_pool, tows, diag) where

      med[(tow_int, sta, sig)]  -> {sat: (n_chords, signed median e)}
      center_pool[(tow_int, sta, sig)] -> sorted [(sat, med_e)] with n >= min_chords
      tows                      -> sorted list of distinct tow_int
      diag                      -> counters
    """
    dd_path = os.path.join(run_dir, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(dd_path):
        raise SystemExit(f"missing {dd_path} — arm network.trace_dd_fractional")

    n_int, slips = {}, {}
    loaded = set()

    def ensure(sta):
        if sta in loaded:
            return
        loaded.add(sta)
        ni, sl = load_truth(fixture_dir, [sta], None)
        n_int.update(ni)
        slips.update(sl)

    raw = {}                       # (tow_int, sta, sig) -> {sat: array('d')}
    tow_set = set()
    diag = Counter()
    intern = sys.intern
    tcache = {}
    cur_tow_s, tow, ti = None, 0.0, 0

    with open(dd_path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        ci = {n: i for i, n in enumerate(hdr)}
        for need in ("tow", "signal", "sat_i", "ref_sat", "sta_a", "sta_b", "dd_cyc", "frac_cyc"):
            if need not in ci:
                raise SystemExit(f"{dd_path}: missing column {need}")
        i_tow, i_sig, i_sat, i_ref = ci["tow"], ci["signal"], ci["sat_i"], ci["ref_sat"]
        i_a, i_b, i_ddc, i_frac = ci["sta_a"], ci["sta_b"], ci["dd_cyc"], ci["frac_cyc"]

        for r in rd:
            diag["rows"] += 1
            tow_s = r[i_tow]
            if tow_s != cur_tow_s:
                cur_tow_s = tow_s
                tow = float(tow_s)
                ti = int(round(tow))
                tow_set.add(ti)
                tcache.clear()
            sat = r[i_sat]
            ref = r[i_ref]
            if sat == ref:                       # degenerate chord: the reference's own arc
                diag["self_ref"] += 1
                continue
            sa, sb = r[i_a], r[i_b]
            if sa in held or sb in held:
                diag["held_out"] += 1
                continue
            sig = r[i_sig]
            if sa not in loaded:
                ensure(sa)
            if sb not in loaded:
                ensure(sb)
            legs = []
            bad = False
            for st_, sv_ in ((sa, sat), (sa, ref), (sb, sat), (sb, ref)):
                k_ = (st_, sv_, sig)
                try:
                    v_ = tcache[k_]
                except KeyError:
                    v_ = n_at(n_int, slips, st_, sv_, sig, tow)
                    tcache[k_] = v_
                if v_ is None:
                    bad = True
                    break
                legs.append(v_)
            if bad:
                diag["no_truth"] += 1
                continue
            dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])     # python ints
            e = float(r[i_ddc]) - dd_true
            frac_trace = float(r[i_frac])
            # parse guard (a tautology for the join itself — see simnet_amb_integer's docstring)
            if abs((e - round(e)) - frac_trace) > 2e-3 and \
               abs(abs(e - round(e)) - abs(frac_trace)) > 2e-3:
                diag["frac_mismatch"] += 1
                continue
            diag["kept"] += 1
            sat_i = intern(sat)
            sig_i = intern(sig)
            for st_, val in ((intern(sa), e), (intern(sb), -e)):
                key = (ti, st_, sig_i)
                d = raw.get(key)
                if d is None:
                    d = raw[key] = {}
                a = d.get(sat_i)
                if a is None:
                    a = d[sat_i] = array("d")
                a.append(val)

    # collapse arrays -> medians, free the raw buffers
    med = {}
    center_pool = {}
    for key, d in raw.items():
        out = {}
        pool = []
        for sat, a in d.items():
            n = len(a)
            m = median(a)
            out[sat] = (n, m)
            if n >= min_chords:
                pool.append((sat, m))
        med[key] = out
        if pool:
            pool.sort()
            center_pool[key] = pool
    raw.clear()
    tows = sorted(tow_set)
    if verbose:
        print(f"    DD trace: {diag['rows']} rows, {diag['kept']} kept, "
              f"{diag['no_truth']} no-truth, {diag['frac_mismatch']} frac-mismatch, "
              f"{diag['self_ref']} self-ref, {diag['held_out']} held-out; "
              f"{len(tows)} epochs, {len(med)} (epoch,sta,sig) slots", file=sys.stderr)
    return med, center_pool, tows, diag


# --------------------------------------------------------------------------
# the arc statistic
# --------------------------------------------------------------------------
class ArcStat:
    """e(S, s, sig, t), as-traced and leave-one-out ref-free."""

    def __init__(self, med, center_pool, min_chords, min_sats):
        self.med = med
        self.pool = center_pool
        self.min_chords = min_chords
        self.min_sats = min_sats

    def signals(self, ti, sta):
        """Signals with a measurable slot for ANY satellite at (ti, sta)."""
        return [k[2] for k in self.med if k[0] == ti and k[1] == sta]

    def raw(self, ti, sta, sat, sig):
        """(n_chords, signed median e) or None if absent / too few chords."""
        d = self.med.get((ti, sta, sig))
        if not d:
            return None
        v = d.get(sat)
        if v is None or v[0] < self.min_chords:
            return None
        return v

    def value(self, ti, sta, sat, sig, ref_free):
        """The signed arc statistic, or None."""
        v = self.raw(ti, sta, sat, sig)
        if v is None:
            return None
        if not ref_free:
            return v[1]
        pool = self.pool.get((ti, sta, sig))
        if not pool:
            return None
        others = [m for s2, m in pool if s2 != sat]     # LEAVE-ONE-OUT
        if len(others) < self.min_sats:
            return None
        return v[1] - median(others)

    def sigs_present(self, ti, sta, sat):
        """Signals for which (ti, sta, sat) has a measurable slot."""
        out = []
        for sig in ("L1C", "L2W", "L5Q", "L7Q", "L1W", "L5X", "L7X", "L2C", "L6C"):
            if self.raw(ti, sta, sat, sig) is not None:
                out.append(sig)
        return out


def build_sig_index(med):
    """(tow_int, sta) -> set of signals seen — avoids scanning `med` per query."""
    idx = defaultdict(set)
    for (ti, sta, sig) in med:
        idx[(ti, sta)].add(sig)
    return dict(idx)


# --------------------------------------------------------------------------
# per-event measurement
# --------------------------------------------------------------------------
def prev_next(sorted_list, t):
    """(last value strictly < t, first value strictly > t) or (None, None)."""
    if not sorted_list:
        return None, None
    i = bisect.bisect_left(sorted_list, t)
    prev = sorted_list[i - 1] if i > 0 else None
    j = bisect.bisect_right(sorted_list, t)
    nxt = sorted_list[j] if j < len(sorted_list) else None
    return prev, nxt


def age_bucket(dt):
    if dt is None:
        return "none"
    for label, lo, hi in AGE_BUCKETS:
        if lo <= dt < hi:
            return label
    return ">60min"


def measure_event(ev, stat, sig_index, relevel, slip_idx, offsets, ref_free):
    """One event -> a record dict.  `ev` = (tow_int, sat, sta)."""
    t0, sat, sta = ev
    sigs = sorted(sig_index.get((t0 - EPOCH_S, sta), ()))     # signals present at the PRE epoch
    pre = {}
    for sig in sigs:
        v = stat.value(t0 - EPOCH_S, sta, sat, sig, ref_free)
        if v is not None:
            pre[sig] = v
    rec = {"tow": t0, "sat": sat, "sta": sta, "pre": pre}
    rec["pre_max"] = max((abs(v) for v in pre.values()), default=None)
    rec["cls"] = classify(rec["pre_max"])

    rl = relevel.get((sta, sat), ())
    prev, nxt = prev_next(rl, t0)
    rec["age_s"] = (t0 - prev) if prev is not None else None
    rec["age"] = age_bucket(rec["age_s"])
    rec["next_s"] = (nxt - t0) if nxt is not None else None

    sl = slip_idx.get((sta, sat), ())
    i = bisect.bisect_right(sl, t0)
    last_slip = sl[i - 1] if i > 0 else None
    rec["slip_dt"] = (t0 - last_slip) if last_slip is not None else None
    if rec["slip_dt"] is not None and rec["slip_dt"] <= 90:
        rec["prox"] = "slip<=90s"
    elif rec["slip_dt"] is not None and rec["slip_dt"] <= 600:
        rec["prox"] = "slip<=600s"
    else:
        rec["prox"] = "no slip<=600s"

    at = {}
    for k in offsets:
        tk = t0 + k * EPOCH_S
        if k >= 0:
            if nxt is not None and tk >= nxt:      # censored by the NEXT re-level
                continue
        elif prev is not None and tk <= prev:      # censored by the PREVIOUS re-level
            continue
        vals = {}
        for sig in (pre or {}):
            v = stat.value(tk, sta, sat, sig, ref_free)
            if v is not None:
                vals[sig] = v
        if vals:
            at[k] = vals
    rec["at"] = at
    rec["at_max"] = {k: max(abs(v) for v in d.values()) for k, d in at.items()}
    return rec


# --------------------------------------------------------------------------
# aggregation / report
# --------------------------------------------------------------------------
def class_table(recs, offsets):
    """class -> {n, share, |e_pre| med/p90, |e_at(k)| med/p90, improve fractions}."""
    out = {}
    total = len(recs)
    for cls in CLASSES:
        sub = [r for r in recs if r["cls"] == cls]
        row = {"n": len(sub), "share": (100.0 * len(sub) / total) if total else 0.0}
        pre = [r["pre_max"] for r in sub if r["pre_max"] is not None]
        row["pre"] = summarize(pre)
        for k in offsets:
            row[f"at{k}"] = summarize([r["at_max"][k] for r in sub if k in r["at_max"]])
        for k in (10, 40):
            pairs = [(r["pre_max"], r["at_max"][k]) for r in sub
                     if k in r["at_max"] and r["pre_max"] is not None]
            row[f"imp{k}_n"] = len(pairs)
            row[f"imp{k}"] = (100.0 * sum(1 for a, b in pairs if b < a) / len(pairs)) if pairs else None
            row[f"worse{k}"] = (100.0 * sum(1 for a, b in pairs if b >= a) / len(pairs)) if pairs else None
        out[cls] = row
    return out


def crosstab(recs, field, labels):
    tab = {cls: {l: 0 for l in labels} for cls in CLASSES}
    for r in recs:
        tab[r["cls"]][r[field]] += 1
    return tab


def profile(recs, kmax):
    """median |e_at(+k)| pooled over events AND signals, k = 0..kmax."""
    prof = []
    for k in range(kmax + 1):
        vals = []
        for r in recs:
            d = r["at"].get(k)
            if d:
                vals.extend(abs(v) for v in d.values())
        prof.append((k, len(vals), median(vals)))
    return prof


def per_signal_pre(recs):
    out = {}
    for r in recs:
        for sig, v in r["pre"].items():
            d = out.setdefault(sig, {"n": 0, "OFF": 0, "AMBIGUOUS": 0, "FINE": 0, "abs": []})
            d["n"] += 1
            d[classify(abs(v))] += 1
            d["abs"].append(abs(v))
    for sig, d in out.items():
        d["med"] = median(d["abs"])
        d["p90"] = pct(d["abs"], 0.9)
        del d["abs"]
    return out


def scan_ambient(stat, sig_index, med, relevel, anyqc, slip_idx, held,
                 n_want, seed, guard, ref_free, min_epoch_tow):
    """Sweep EVERY measurable (tow, sta, sat) slot, keeping the ones with no
    re-level event on (sta, sat) within +/- `guard` seconds.

    Returns (control_sample, strict_sample, ambient, per_signal, diag) where
    `ambient[tow_int]` is the median max-over-signals |e| of the quiet slots at
    that epoch — the epoch-matched reference an event's |e_pre| is compared to
    (the bed's own error level moves with time, so a global control would
    confound that)."""
    rng = random.Random(seed)
    # (tow_int, sta) -> {sat: {sig: value}} is rebuilt on the fly per slot
    cand = defaultdict(set)                                # (ti, sta) -> {sat}
    for (ti, sta, sig), d in med.items():
        if sta in held or ti < min_epoch_tow:
            continue
        s = cand[(ti, sta)]
        for sat, (n, _m) in d.items():
            if n >= stat.min_chords:
                s.add(sat)
    eligible, strict = [], []
    per_epoch = defaultdict(list)
    per_sig = defaultdict(list)
    n_rej_rl = n_rej_qc = n_slots = 0
    for (ti, sta), sats in cand.items():
        sigs = sig_index.get((ti, sta), ())
        for sat in sats:
            n_slots += 1
            rl = relevel.get((sta, sat), ())
            i = bisect.bisect_left(rl, ti - guard)
            if i < len(rl) and rl[i] <= ti + guard:
                n_rej_rl += 1
                continue
            pre = {}
            for sig in sigs:
                v = stat.value(ti, sta, sat, sig, ref_free)
                if v is not None:
                    pre[sig] = v
            if not pre:
                continue
            pmax = max(abs(v) for v in pre.values())
            for sig, v in pre.items():
                per_sig[sig].append(abs(v))
            per_epoch[ti].append(pmax)
            rec = {"tow": ti, "sat": sat, "sta": sta, "pre": pre, "pre_max": pmax,
                   "cls": classify(pmax)}
            eligible.append(rec)
            qc = anyqc.get((sta, sat), ())
            j = bisect.bisect_left(qc, ti - guard)
            if j < len(qc) and qc[j] <= ti + guard:
                n_rej_qc += 1
            else:
                sl = slip_idx.get((sta, sat), ())
                k = bisect.bisect_left(sl, ti - guard)
                rec["slip_near"] = (k < len(sl) and sl[k] <= ti + guard)
                strict.append(rec)
    ambient = {ti: median(v) for ti, v in per_epoch.items()}
    ctl = rng.sample(eligible, min(n_want, len(eligible))) if eligible else []
    rng2 = random.Random(seed)
    ctls = rng2.sample(strict, min(n_want, len(strict))) if strict else []
    psig = {}
    for sig, v in per_sig.items():
        psig[sig] = {"n": len(v), "med": median(v), "p90": pct(v, 0.9),
                     "OFF": sum(1 for x in v if x >= OFF_CYC),
                     "AMBIGUOUS": sum(1 for x in v if FINE_CYC <= x < OFF_CYC),
                     "FINE": sum(1 for x in v if x < FINE_CYC)}
    diag = {"slots_scanned": n_slots, "eligible": len(eligible), "strict": len(strict),
            "rejected_relevel": n_rej_rl, "rejected_anyqc": n_rej_qc,
            "epochs_with_ambient": len(ambient)}
    return ctl, ctls, ambient, psig, diag


def chain_stats(recs_all, relevel, max_gap_ep):
    """H-C's direct test: for CONSECUTIVE re-level events on the same (S, s)
    separated by <= max_gap_ep epochs, pair the FIRST event's post-erase level
    (its |e| at +1 epoch) with the SECOND event's pre-erase level.  A limit
    cycle looks like: post_1 already large, pre_2 ~ post_1, and the second
    erase does not reduce it."""
    by_key = {}
    for r in recs_all:
        by_key[(r["sta"], r["sat"], r["tow"])] = r
    pairs = []
    for (sta, sat), tows in relevel.items():
        for a, b in zip(tows, tows[1:]):
            if (b - a) > max_gap_ep * EPOCH_S:
                continue
            ra = by_key.get((sta, sat, a))
            rb = by_key.get((sta, sat, b))
            if ra is None or rb is None:
                continue
            post = ra["at_max"].get(1)
            if post is None:
                post = ra["at_max"].get(0)
            pre2 = rb["pre_max"]
            if post is None or pre2 is None:
                continue
            pairs.append((post, pre2, b - a, ra["pre_max"]))
    if not pairs:
        return {"n": 0}
    post = [p[0] for p in pairs]
    pre2 = [p[1] for p in pairs]
    pre1 = [p[3] for p in pairs if p[3] is not None]
    return {"n": len(pairs),
            "med_pre1": median(pre1), "med_post1": median(post), "med_pre2": median(pre2),
            "p90_post1": pct(post, 0.9), "p90_pre2": pct(pre2, 0.9),
            "frac_pre2_ge_post1": 100.0 * sum(1 for a, b in zip(post, pre2) if b >= a) / len(pairs),
            "frac_post1_ge_pre1": (100.0 * sum(1 for p in pairs if p[3] is not None and p[0] >= p[3])
                                   / max(1, len(pre1))),
            "med_gap_s": median([p[2] for p in pairs])}


# --------------------------------------------------------------------------
def render(res, out):
    def P(*a):
        print(*a, file=out)

    P("=" * 100)
    P("T4 ATTRIBUTION-ARC CENSUS — what does `T4_attrib / attrib_arc` erase?")
    P("=" * 100)
    P(f"  run            : {res['run']}")
    P(f"  fixture        : {res['fixture']}")
    P(f"  statistic      : {'REF-FREE (leave-one-out over other sats)' if res['ref_free'] else 'AS-TRACED (carries the trace reference sat)'}")
    P(f"  min chords     : {res['min_chords']}   min sats for ref-free: {res['min_sats']}")
    P(f"  class cuts     : OFF >= {OFF_CYC} cyc, FINE < {FINE_CYC} cyc  (max over signals of |e_pre|)")
    P()
    P("-- SANITY --")
    d = res["dd_diag"]
    P(f"  DD rows {d['rows']}  kept {d['kept']}  no-truth {d.get('no_truth', 0)}  "
      f"frac-mismatch {d.get('frac_mismatch', 0)}  self-ref {d.get('self_ref', 0)}  held-out {d.get('held_out', 0)}")
    P(f"  epochs {res['n_epochs']}  (tow {res['tow_first']} .. {res['tow_last']}, step {EPOCH_S} s)")
    P(f"  T4 events found            : {res['n_t4']}")
    P(f"  ... measurable (>= {res['min_chords']} chords on >= 1 signal at t0-30s): "
      f"{res['n_t4_meas']}  ({100.0 * res['n_t4_meas'] / max(1, res['n_t4']):.1f} %)")
    P(f"  detector refloat events    : {res['n_det']}  measurable {res['n_det_meas']}")
    P(f"  truth slip rows in fixture : {res['n_truth_slip_rows']} on the {res['n_stations_graded']} graded stations")
    P(f"  DD-trace station coverage  : {res['n_dd_stations']} of {res['n_stations_graded']} stations appear in a chord")
    P(f"  control population         : n={res['control']['n']}  median |e| = "
      f"{fmt_v(res['control']['med'], 6)} cyc   (strict variant n={res['control_strict']['n']}, "
      f"median {fmt_v(res['control_strict']['med'], 6)})")
    P()

    for tag, tbl, n_tot in (("T4 events", res["t4_class"], res["n_t4"]),
                            ("detector refloats", res["det_class"], res["n_det"])):
        P(f"-- CLASS SHARES + the re-level's own error, {tag} (n = {n_tot}) --")
        P(f"  {'class':<13}{'n':>7}{'share':>8}  | {'|e_pre| med':>11}{'p90':>8}  | "
          + "".join(f"{('|e+%d| med' % k):>11}{'p90':>8}" for k in REPORT_OFFSETS)
          + f"  | {'impr@+10':>9}{'worse@+10':>10}{'n':>7}")
        for cls in CLASSES:
            r = tbl[cls]
            cells = ""
            for k in REPORT_OFFSETS:
                n_, m_, p_ = r[f"at{k}"]
                cells += fmt_v(m_, 11) + fmt_v(p_, 8)
            npre, mpre, ppre = r["pre"]
            imp = r["imp10"]
            wor = r["worse10"]
            P(f"  {cls:<13}{r['n']:>7}{r['share']:>7.1f}%  | {fmt_v(mpre, 11)}{fmt_v(ppre, 8)}  | "
              f"{cells}  | " + (f"{imp:>8.1f}%{wor:>9.1f}%" if imp is not None else f"{'-':>9}{'-':>10}")
              + f"{r['imp10_n']:>7}")
        P()

    P("-- THE DISCRIMINATOR: |e_pre| of an erased arc vs the SAME-EPOCH quiet arcs --")
    P(f"  {'population':<22}{'n':>7}{'med|e_pre|':>12}{'p90':>10}  | "
      f"{'ratio med':>10}{'p90':>9}{'>1x':>8}{'>2x':>8}  | {'ctl quantile':>13}{'>ctl p90':>10}{'<ctl p50':>10}")
    for name, b in (("T4 erased arcs", res["t4_overall"]), ("detector refloats", res["det_overall"])):
        P(f"  {name:<22}{b['n']:>7}{fmt_v(b['med'], 12)}{fmt_v(b['p90'], 10)}  | "
          f"{fmt_v(b['ratio_med'], 10, 2)}{fmt_v(b['ratio_p90'], 9, 2)}"
          f"{(('%.1f%%' % b['frac_ratio_gt1']) if b['frac_ratio_gt1'] is not None else '-'):>8}"
          f"{(('%.1f%%' % b['frac_ratio_gt2']) if b['frac_ratio_gt2'] is not None else '-'):>8}  | "
          f"{fmt_v(b.get('ctl_quantile_med'), 13, 3)}"
          f"{(('%.1f%%' % b['frac_above_ctl_p90']) if b.get('frac_above_ctl_p90') is not None else '-'):>10}"
          f"{(('%.1f%%' % b['frac_below_ctl_p50']) if b.get('frac_below_ctl_p50') is not None else '-'):>10}")
    P(f"  quiet arcs (control) {res['control']['n']:>9}{fmt_v(res['control']['med'], 12)}"
      f"{fmt_v(res['control']['p90'], 10)}  |  (by construction: ratio 1.0, quantile 0.50)")
    P("  ratio = |e_pre| / (median |e| of the QUIET arcs at the SAME epoch).  ratio ~1 and quantile ~0.5")
    P("  means the erased arc is indistinguishable from an untouched one (H-B); >>1 means T4 finds real damage.")
    P()

    P("-- WAS THE ARC ALREADY BAD?  the erased arc's own level BEFORE the event (censored at the previous one) --")
    P(f"  {'population':<20}{'offset':>9}{'n':>8}{'med|e| then':>13}{'med|e_pre|':>12}{'grew':>9}{'grew >2x':>10}")
    for name, h in (("T4 erased arcs", res["t4_history"]), ("detector refloats", res["det_history"])):
        for k in HISTORY_OFFSETS:
            b = h.get(str(k), h.get(k))
            if not b or not b["n"]:
                continue
            P(f"  {name:<20}{('%d ep (%d s)' % (k, k * EPOCH_S)):>9}{b['n']:>8}{fmt_v(b['med_then'], 13)}"
              f"{fmt_v(b['med_pre'], 12)}{b['frac_grew']:>8.1f}%{b['frac_grew_2x']:>9.1f}%")
    P("  'grew' = |e_pre| exceeded the arc's own level that far back.  ~50% and equal medians mean T4 is")
    P("  firing on a CHRONICALLY bad arc, not on a fresh degradation.")
    P()

    P("-- H-C, the RE-LEVEL CHAIN: consecutive re-levels on one (S,s) within the profile window --")
    for name, c in (("T4 -> T4", res["chain_t4"]), ("any re-level -> any", res["chain_all"])):
        if not c.get("n"):
            P(f"  {name:<22} (no pairs)")
            continue
        P(f"  {name:<22} n={c['n']:<6} med gap {c['med_gap_s']:.0f} s  |  "
          f"med |e| BEFORE 1st erase {fmt_v(c['med_pre1'], 7)} -> AFTER 1st erase "
          f"{fmt_v(c['med_post1'], 7)} -> BEFORE 2nd erase {fmt_v(c['med_pre2'], 7)}")
        P(f"  {'':<22} the 1st erase made it WORSE in {c['frac_post1_ge_pre1']:.1f}% of pairs; "
          f"the level was still >= its post-erase value at the 2nd erase in {c['frac_pre2_ge_post1']:.1f}%")
    P()

    P("-- CONTROL calibration: the SAME statistic on quiet arcs (no re-level within "
      f"+/-{res['control']['guard']} s) --")
    for name, c in (("control", res["control"]), ("control (strict: no T0/T1/T4 row at all)", res["control_strict"])):
        P(f"  {name:<42} n={c['n']:<6} med|e| {fmt_v(c['med'], 7)}  p90 {fmt_v(c['p90'], 7)}  "
          f"classes: " + "  ".join(f"{k} {c['classes'].get(k, 0) * 100.0 / max(1, c['n']):.1f}%"
                                   for k in ("OFF", "AMBIGUOUS", "FINE")))
    if res["control_strict"].get("slip_split"):
        s = res["control_strict"]["slip_split"]
        P(f"  {'... strict, split by a truth slip nearby':<42} "
          f"slip-near n={s['near_n']} med {fmt_v(s['near_med'], 7)} | "
          f"quiet n={s['far_n']} med {fmt_v(s['far_med'], 7)}")
    P(f"  control per-signal: " + "  ".join(
        f"{sig} n={d['n']} med {d['med']:.3f} OFF {100.0 * d['OFF'] / max(1, d['n']):.0f}%"
        for sig, d in sorted(res["control_per_signal"].items())))
    P()

    P("-- CROSS-TAB  class x AGE since the previous re-level on (S, s)  [T4 events] --")
    P(f"  {'class':<13}" + "".join(f"{l:>11}" for l in AGE_LABELS) + f"{'row n':>9}")
    for cls in CLASSES:
        row = res["t4_age"][cls]
        n = sum(row.values())
        P(f"  {cls:<13}" + "".join(f"{row[l]:>11}" for l in AGE_LABELS) + f"{n:>9}")
    P(f"  {'col total':<13}" + "".join(
        f"{sum(res['t4_age'][c][l] for c in CLASSES):>11}" for l in AGE_LABELS))
    P("  column shares of the MEASURABLE events only (OFF / AMBIGUOUS / FINE):")
    for l in AGE_LABELS:
        tot = sum(res["t4_age"][c][l] for c in ("OFF", "AMBIGUOUS", "FINE"))
        if tot:
            P(f"    {l:<10} n={tot:<7} " + "  ".join(
                f"{c} {100.0 * res['t4_age'][c][l] / tot:5.1f}%" for c in ("OFF", "AMBIGUOUS", "FINE")))
    P()
    if res.get("t4_age_improve"):
        P("  improvement @+10 epochs by age bucket (measurable events):")
        for l in AGE_LABELS:
            v = res["t4_age_improve"].get(l)
            if v and v["n"]:
                P(f"    {l:<10} n={v['n']:<7} improved {v['imp']:5.1f}%   "
                  f"med|e_pre| {fmt_v(v['med_pre'], 6)}  med|e+10| {fmt_v(v['med_at'], 6)}")
        P()

    P("-- CROSS-TAB  class x TRUTH-SLIP proximity  [T4 events] --")
    P(f"  {'class':<13}" + "".join(f"{l:>16}" for l in PROX_LABELS) + f"{'row n':>9}")
    for cls in CLASSES:
        row = res["t4_prox"][cls]
        P(f"  {cls:<13}" + "".join(f"{row[l]:>16}" for l in PROX_LABELS) + f"{sum(row.values()):>9}")
    P(f"  {'col total':<13}" + "".join(
        f"{sum(res['t4_prox'][c][l] for c in CLASSES):>16}" for l in PROX_LABELS))
    P()

    P("-- PER-SIGNAL |e_pre| shares  [T4 events, measurable slots] --")
    P(f"  {'signal':<8}{'n':>8}{'med':>9}{'p90':>9}{'OFF':>9}{'AMBIG':>9}{'FINE':>9}")
    for sig, d in sorted(res["t4_per_signal"].items()):
        P(f"  {sig:<8}{d['n']:>8}{fmt_v(d['med'], 9)}{fmt_v(d['p90'], 9)}"
          f"{100.0 * d['OFF'] / max(1, d['n']):>8.1f}%{100.0 * d['AMBIGUOUS'] / max(1, d['n']):>8.1f}%"
          f"{100.0 * d['FINE'] / max(1, d['n']):>8.1f}%")
    P()

    P("-- ARC-AGE CONVERGENCE after a re-level: median |e| (cyc) vs epochs since the event --")
    P("   (pooled over events AND signals; censored at the next re-level on the same (S,s))")
    P(f"   ambient (quiet-arc) level for reference: median over epochs = {fmt_v(res['ambient_med'], 6)} cyc")
    P("   'cohort' = only events with NO next re-level inside the whole window (a FIXED cohort:")
    P("   the censored profile above it drops the re-hit arcs as k grows, which is survivorship bias)")
    for tag, prof in (("T4", res["t4_profile"]), ("T4 cohort", res["t4_profile_cohort"]),
                      ("detector", res["det_profile"]), ("detector cohort", res["det_profile_cohort"])):
        P(f"  {tag}:")
        for lo in range(0, len(prof), 10):
            chunk = prof[lo:lo + 10]
            P("    k   " + "".join(f"{k:>9}" for k, _, _ in chunk))
            P("    med " + "".join(fmt_v(m, 9) for _, _, m in chunk))
            P("    n   " + "".join(f"{n:>9}" for _, n, _ in chunk))
        # 1/sqrt(k) reference
        p0 = next((m for k, n, m in prof if k == 1 and m), None)
        p40 = next((m for k, n, m in prof if k == 40 and m), None)
        if p0 and p40:
            P(f"    k=1 -> k=40: {p0:.3f} -> {p40:.3f}  (ratio {p40 / p0:.2f}; "
              f"1/sqrt(40) = {1.0 / math.sqrt(40):.2f} would be full averaging, 1.00 = frozen)")
    P()
    P("=" * 100)


# --------------------------------------------------------------------------
def run_census(args, out=sys.stdout):
    allsta, held = load_stations(args.run_dir)
    if args.exclude:
        held |= set(args.exclude)
    print(f"[1/5] stations: {len(allsta)} in roster, held-out excluded: {sorted(held)}", file=sys.stderr)

    ev = load_events(args.run_dir, held)
    print(f"[2/5] events: {len(ev['t4'])} T4_attrib, {len(ev['det'])} detector refloats", file=sys.stderr)

    graded = sorted(s for s in allsta if s not in held)
    slip_idx, n_slip_rows = load_truth_slip_index(args.fixture_dir, graded)
    print(f"[3/5] truth slips: {n_slip_rows} rows on {len(graded)} graded stations", file=sys.stderr)

    print("[4/5] streaming the DD trace ...", file=sys.stderr)
    med, center_pool, tows, dd_diag = scan_dd(args.run_dir, args.fixture_dir, held, args.min_chords)
    stat = ArcStat(med, center_pool, args.min_chords, args.min_sats)
    sig_index = build_sig_index(med)
    dd_stations = {k[1] for k in med}

    offsets = tuple(sorted(set(list(args.offsets) + list(HISTORY_OFFSETS)
                               + list(range(0, args.profile_k + 1)))))
    print(f"[5/5] measuring {len(ev['t4'])} T4 + {len(ev['det'])} detector events ...", file=sys.stderr)
    t4_recs = [measure_event(e, stat, sig_index, ev["relevel"], slip_idx, offsets, args.ref_free)
               for e in ev["t4"]]
    det_recs = [measure_event(e, stat, sig_index, ev["relevel"], slip_idx, offsets, args.ref_free)
                for e in ev["det"]]

    t4_meas = [r for r in t4_recs if r["cls"] != "UNMEASURABLE"]
    det_meas = [r for r in det_recs if r["cls"] != "UNMEASURABLE"]

    print("      ... sweeping the ambient / control population", file=sys.stderr)
    ctl, ctl_strict, ambient, ctl_psig, ctl_diag = scan_ambient(
        stat, sig_index, med, ev["relevel"], ev["anyqc"], slip_idx, held,
        args.control_n, args.seed, args.control_guard, args.ref_free,
        tows[0] + args.control_skip_epochs * EPOCH_S if tows else 0)

    # epoch-matched ratio: the event's own |e_pre| against the quiet arcs of the
    # SAME epoch (the bed's error level moves with time)
    for recs in (t4_recs, det_recs):
        for r in recs:
            amb = ambient.get(r["tow"] - EPOCH_S)
            r["ambient"] = amb
            r["ratio"] = (r["pre_max"] / amb) if (amb and r["pre_max"] is not None and amb > 0) else None

    def ctl_block(recs):
        a = [r["pre_max"] for r in recs]
        cl = Counter(r["cls"] for r in recs)
        return {"n": len(recs), "med": median(a), "p90": pct(a, 0.9),
                "p50": median(a), "p75": pct(a, 0.75), "p99": pct(a, 0.99),
                "classes": dict(cl), "guard": args.control_guard}

    cb = ctl_block(ctl)
    cbs = ctl_block(ctl_strict)
    near = [r["pre_max"] for r in ctl_strict if r.get("slip_near")]
    far = [r["pre_max"] for r in ctl_strict if not r.get("slip_near")]
    cbs["slip_split"] = {"near_n": len(near), "near_med": median(near),
                         "far_n": len(far), "far_med": median(far)}

    def meas_block(recs):
        a = [r["pre_max"] for r in recs if r["pre_max"] is not None]
        rat = [r["ratio"] for r in recs if r.get("ratio") is not None]
        out = {"n": len(a), "med": median(a), "p90": pct(a, 0.9),
               "ratio_n": len(rat), "ratio_med": median(rat), "ratio_p90": pct(rat, 0.9),
               "frac_ratio_gt1": (100.0 * sum(1 for x in rat if x > 1.0) / len(rat)) if rat else None,
               "frac_ratio_gt2": (100.0 * sum(1 for x in rat if x > 2.0) / len(rat)) if rat else None}
        # quantile of each event's |e_pre| inside the control distribution
        ctl_sorted = sorted(r["pre_max"] for r in ctl)
        if ctl_sorted and a:
            q = [bisect.bisect_left(ctl_sorted, x) / len(ctl_sorted) for x in a]
            out["ctl_quantile_med"] = median(q)
            out["frac_above_ctl_p90"] = 100.0 * sum(1 for x in a if x > cb["p90"]) / len(a)
            out["frac_below_ctl_p50"] = 100.0 * sum(1 for x in a if x < cb["p50"]) / len(a)
        return out

    def history_block(recs):
        """Was the arc ALREADY this bad before the event?  A T4 that fires on a
        NEW degradation has |e_pre| >> |e| a few minutes earlier; one that keeps
        hammering a chronically-bad arc has them equal."""
        out = {}
        for k in HISTORY_OFFSETS:
            pairs = [(r["pre_max"], r["at_max"][k]) for r in recs
                     if k in r["at_max"] and r["pre_max"] is not None]
            out[k] = {"n": len(pairs), "med_then": median([b for _, b in pairs]),
                      "med_pre": median([a for a, _ in pairs]),
                      "frac_grew": (100.0 * sum(1 for a, b in pairs if a > b) / len(pairs)) if pairs else None,
                      "frac_grew_2x": (100.0 * sum(1 for a, b in pairs if b > 0 and a > 2.0 * b) / len(pairs))
                      if pairs else None}
        return out

    t4_overall = meas_block(t4_meas)
    det_overall = meas_block(det_meas)
    t4_hist = history_block(t4_meas)
    det_hist = history_block(det_meas)
    chain_t4 = chain_stats(t4_recs, {k: v for k, v in ev["relevel"].items()}, args.profile_k)
    chain_all = chain_stats(t4_recs + det_recs, ev["relevel"], args.profile_k)

    # age-bucket improvement
    age_imp = {}
    for l in AGE_LABELS:
        sub = [r for r in t4_meas if r["age"] == l and 10 in r["at_max"]]
        if sub:
            age_imp[l] = {
                "n": len(sub),
                "imp": 100.0 * sum(1 for r in sub if r["at_max"][10] < r["pre_max"]) / len(sub),
                "med_pre": median([r["pre_max"] for r in sub]),
                "med_at": median([r["at_max"][10] for r in sub]),
            }

    res = {
        "run": os.path.abspath(args.run_dir), "fixture": os.path.abspath(args.fixture_dir),
        "ref_free": bool(args.ref_free), "min_chords": args.min_chords, "min_sats": args.min_sats,
        "dd_diag": dict(dd_diag), "n_epochs": len(tows),
        "tow_first": tows[0] if tows else None, "tow_last": tows[-1] if tows else None,
        "n_t4": len(t4_recs), "n_t4_meas": len(t4_meas),
        "n_det": len(det_recs), "n_det_meas": len(det_meas),
        "n_truth_slip_rows": n_slip_rows, "n_stations_graded": len(graded),
        "n_dd_stations": len(dd_stations),
        "event_counts": dict(ev["counts"]),
        "t4_class": class_table(t4_recs, REPORT_OFFSETS),
        "det_class": class_table(det_recs, REPORT_OFFSETS),
        "t4_age": crosstab(t4_recs, "age", AGE_LABELS),
        "t4_age_improve": age_imp,
        "t4_prox": crosstab(t4_recs, "prox", PROX_LABELS),
        "t4_per_signal": per_signal_pre(t4_meas),
        "control": cb, "control_strict": cbs, "control_diag": ctl_diag,
        "control_per_signal": ctl_psig,
        "t4_overall": t4_overall, "det_overall": det_overall,
        "t4_history": t4_hist, "det_history": det_hist,
        "chain_t4": chain_t4, "chain_all": chain_all,
        "t4_profile": profile(t4_recs, args.profile_k),
        "det_profile": profile(det_recs, args.profile_k),
        "t4_profile_cohort": profile(
            [r for r in t4_recs if r["next_s"] is None or r["next_s"] > args.profile_k * EPOCH_S],
            args.profile_k),
        "det_profile_cohort": profile(
            [r for r in det_recs if r["next_s"] is None or r["next_s"] > args.profile_k * EPOCH_S],
            args.profile_k),
        "ambient_med": median(list(ambient.values())) if ambient else None,
    }
    render(res, out)
    return res


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run_dir", nargs="?", help="producer run dir (its logs/ holds the traces)")
    ap.add_argument("fixture_dir", nargs="?", help="sim fixture dir (per-station truth_*.csv)")
    ap.add_argument("--json", dest="json_out", default=None, help="write the results dict here")
    ap.add_argument("--txt", dest="txt_out", default=None, help="also write the text report here")
    ap.add_argument("--control-n", type=int, default=3000)
    ap.add_argument("--control-guard", type=float, default=600.0,
                    help="a control slot needs no re-level on (S,s) within +/- this many s (default 600)")
    ap.add_argument("--control-skip-epochs", type=int, default=0,
                    help="skip this many leading epochs when sampling controls (ignition guard)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--min-chords", type=int, default=3)
    ap.add_argument("--min-sats", type=int, default=3,
                    help="satellites needed in the leave-one-out ref-free centre (default 3)")
    ap.add_argument("--ref-free", action="store_true",
                    help="difference the trace's reference satellite away (SPEC_amb_iono §6.12)")
    ap.add_argument("--offsets", type=int, nargs="*", default=list(DEFAULT_OFFSETS))
    ap.add_argument("--profile-k", type=int, default=40)
    ap.add_argument("--exclude", nargs="*", default=[], help="extra stations to drop")
    ap.add_argument("--selftest", action="store_true")
    return ap


def _selftest():
    """Tiny synthetic bed: 4 stations, 3 sats, 1 signal, a known arc offset."""
    import tempfile
    ok = True

    def check(c, msg):
        nonlocal ok
        print(("  PASS " if c else "  FAIL ") + msg)
        ok = ok and bool(c)

    with tempfile.TemporaryDirectory() as td:
        run = os.path.join(td, "run")
        fix = os.path.join(td, "fix")
        os.makedirs(os.path.join(run, "logs"))
        stas = ["SA", "SB", "SC", "SD"]
        sats = ["G01", "G02", "G03"]
        N = {(s, v): 100 + 7 * i + 3 * j for i, s in enumerate(stas) for j, v in enumerate(sats)}
        for s in stas:
            os.makedirs(os.path.join(fix, s))
            with open(os.path.join(fix, s, "truth_ambiguities.csv"), "w", newline="") as f:
                f.write("satellite,signal,N_int,wavelength_m\n")
                for v in sats:
                    f.write(f"{v},L1C,{N[(s, v)]},0.190293672798\n")
            with open(os.path.join(fix, s, "truth_slips.csv"), "w", newline="") as f:
                f.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
        with open(os.path.join(run, "logs", "network_stations.csv"), "w", newline="") as f:
            f.write("station,role,ecef_x,ecef_y,ecef_z\n")
            for s in stas:
                f.write(f"{s},network,0,0,0\n")
            f.write("SZZ,held_out,0,0,0\n")
        # SB/G02 carries a +0.8 cycle arc error from tow 1000 onward; a T4 fires at 1090
        tows = [1000 + 30 * k for k in range(8)]
        with open(os.path.join(run, "logs", "network_dd_fractional.csv"), "w", newline="") as f:
            f.write("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,"
                    "dd_cyc,frac_cyc,dd_sigma_cyc,sigma_carry_aware\n")
            for t in tows:
                for si in ("G02", "G03"):
                    for a in stas:
                        for b in stas:
                            if a >= b:
                                continue
                            err = 0.0
                            if si == "G02" and t < 1090:
                                err += 0.8 if a == "SB" else (-0.8 if b == "SB" else 0.0)
                            ddt = (N[(a, si)] - N[(a, "G01")]) - (N[(b, si)] - N[(b, "G01")])
                            dd = ddt + err
                            fr = dd - round(dd)
                            f.write(f"1,{t}.000,G,L1C,{si},G01,{a},{b},0.0,0.190294,"
                                    f"{dd:.4f},{fr:.4f},1.0,0\n")
        with open(os.path.join(run, "logs", "network_events.csv"), "w", newline="") as f:
            f.write("week,tow,tier,event,sat,station,statistic,threshold,action,detail\n")
            f.write("1,1090.000,T4_attrib,attrib_arc,G02,SB,,6.0,refloat,sigma0=1.00\n")
            f.write("1,1030.000,T0_mw_detect,mw_wl_slip,G03,SC,,,refloat,L1C;L2W\n")
        args = build_parser().parse_args([run, fix, "--control-n", "20", "--profile-k", "3",
                                          "--min-sats", "1"])
        import io
        res = run_census(args, io.StringIO())
        check(res["n_t4"] == 1, "one T4 event parsed")
        r = res["t4_class"]
        check(r["OFF"]["n"] == 1, f"the +0.8 cyc arc classifies OFF (got {[(c, r[c]['n']) for c in CLASSES]})")
        check(abs(r["OFF"]["pre"][1] - 0.8) < 1e-6, f"e_pre = 0.8 exactly (got {r['OFF']['pre'][1]})")
        check(r["OFF"]["at0"][1] is not None and r["OFF"]["at0"][1] < 1e-6,
              f"e_at(+0) = 0 after the erase (got {r['OFF']['at0'][1]})")
        check(res["n_det"] == 1 and res["det_class"]["FINE"]["n"] == 1, "the clean detector refloat is FINE")
        # sign convention: SB is sta_b in SA-SB and sta_a in SB-SC / SB-SD -> +0.8 either way
        args2 = build_parser().parse_args([run, fix, "--control-n", "20", "--profile-k", "3",
                                           "--min-sats", "1", "--ref-free"])
        res2 = run_census(args2, io.StringIO())
        check(res2["t4_class"]["OFF"]["n"] == 1, "ref-free keeps the OFF classification")
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    args = build_parser().parse_args()
    if args.selftest:
        return _selftest()
    if not args.run_dir or not args.fixture_dir:
        build_parser().print_usage(sys.stderr)
        raise SystemExit("run_dir and fixture_dir are required")
    import io
    buf = io.StringIO()
    res = run_census(args, buf)
    text = buf.getvalue()
    sys.stdout.write(text)
    if args.txt_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.txt_out)), exist_ok=True)
        with open(args.txt_out, "w", encoding="utf-8") as f:
            f.write(text)
    if args.json_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.json_out)), exist_ok=True)
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
