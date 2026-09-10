#!/usr/bin/env python3
"""Join the closed-loop CONSUMER's float ambiguities to the simulator's TRUTH integers and
measure the fixability of the integer-natured combinations (per-band SD, WL, EWL, NL).

WHY
---
On the synthetic quiet 24 h bed the held-out consumer (``network.closed_loop``) runs
``ar_mode 1`` every epoch and fixes NOTHING (ratio ~ 1.0).  Its per-band float sigma is
0.29-0.45 cycles, far too loose for a direct per-band LAMBDA but not obviously too loose
for a WL / EWL / NL cascade.  This measures what such a cascade would actually see.

TRUTH SEMANTICS
---------------
``<fixture>/<station>/truth_ambiguities.csv`` = ``satellite,signal,N_int,wavelength_m``.
The simulator (src/sim/main.cpp) writes it commented as

    "Integer carrier ambiguities are arc-constant per (sat, band) and recorded once, the
     first epoch each (sat, signal) is seen."
    ``if (amb_seen.insert(akey).second) // record the INITIAL integer (pre-slip)``

and the phase observable it belongs to is (same file, the ``row.values`` block)

    L_b = (P + tropo + m7 + ant + bias_phase + rxb_phase + isb - I_b
           + windup_m + lambda_b * N + np + np_scint + mp_phase) / lambda_b

so ``N_int`` is the PURE INTEGER.  Every bias is a separate additive metres term:
``bias_phase`` = satellite observable-specific phase OSB (satellite-dependent, does NOT
cancel between satellites), ``rxb_phase`` = this station's receiver phase bias (satellite-
INDEPENDENT, cancels exactly in a between-satellite SD), ``isb`` = GPS->Gal inter-system
bias (constellation-common).  Hence only a WITHIN-CONSTELLATION between-satellite SD is
comparable to truth.  ``truth_slips.csv`` is checked: on a bed with zero slips the recorded
integer is the true integer all day.

THE GAUGE INTEGER (why the raw SD error is tens of cycles)
----------------------------------------------------------
The producer's per-(sat,signal) phase-bias gauge is pinned at an ANCHOR receiver's own
absolute ambiguity estimate, so the emitted phase bias carries that anchor's integer.  The
consumer's ambiguity state therefore converges to
    N_consumer - N_anchor + (float error of both sides)
and the SD error against truth is  e = -(N_anchor,i - N_anchor,ref) + eps  --  an exact
INTEGER plus the real (producer + consumer) float residue ``eps``.  That integer is a gauge
artefact that cancels in the wire->consumer loop, so:
  * the UNWRAPPED std of ``e`` measures the gauge, not fixability (reported for
    completeness, flagged);
  * the WRAPPED residue eps = e - round(e) is the fixability quantity.
Per-band and WL/EWL wrapping is legitimate (a band difference of integers is an integer).
The NL is NOT: (f1*n1 - f2*n2)/(f1-f2) = n1 + n_WL*f2/(f1-f2) is only an integer when
n_WL = 0, i.e. the NL is defined modulo 1 ONLY once the widelane is fixed.  Both forms are
reported and the amplification identity is printed.

Usage
-----
    python scripts/consumer_float_truth_join.py \
        --run runs/golden_sim_consumer_ar_a \
        --fixture runs/simnet60_quiet_wu93_24h \
        --station SZZ000SIM --settle-h 12

``--station`` is optional (recovered from ``<run>/logs/gnss_config.log``).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import Counter, defaultdict

import numpy as np

C_LIGHT = 299792458.0

# Signal -> carrier frequency (Hz).  Confirmed against include/common/Constants.hpp
#   GPS_L1_FREQ 1575.42e6 / GPS_L2_FREQ 1227.60e6 / GPS_L5_FREQ 1176.45e6
#   GAL_E1_FREQ 1575.42e6 / GAL_E5A_FREQ 1176.45e6 / GAL_E5B_FREQ 1207.14e6
# and cross-checked at run time against truth_ambiguities.csv's wavelength_m column.
FREQ = {
    "G": {"L1C": 1575.42e6, "L2W": 1227.60e6, "L5Q": 1176.45e6},
    "E": {"L1C": 1575.42e6, "L5Q": 1176.45e6, "L7Q": 1207.14e6},
}
BANDS = {"G": ["L1C", "L2W", "L5Q"], "E": ["L1C", "L5Q", "L7Q"]}
CONST_NAME = {"G": "GPS", "E": "Galileo"}
LANES = {
    "G": {"WL": ("L1C", "L2W"), "EWL": ("L2W", "L5Q"), "NL": ("L1C", "L2W")},
    "E": {"WL": ("L1C", "L5Q"), "EWL": ("L5Q", "L7Q"), "NL": ("L1C", "L5Q")},
}
DT_S = 30.0


# ---------------------------------------------------------------------------
def wrap(e):
    """Wrapped fraction e - round(e), in [-0.5, 0.5)."""
    return np.asarray(e, dtype=float) - np.round(np.asarray(e, dtype=float))


def stat_block(e, wrappable=True):
    """Statistics for ambiguity errors in CYCLES of the named channel.

    Unwrapped moments (gauge-contaminated, see module docstring) plus, when the channel is
    integer-shift invariant, the wrapped residue statistics -- each quoted against its own
    size-matched uniform null:
      |frac| p50 null 0.25 ; P(|frac|<0.15) null 0.30 ; P(|frac|<0.25) null 0.50 ;
      circular resultant R = |mean(exp(2 pi i e))| with uniform null E[R] ~ sqrt(pi/(4n)).
    ``mu`` is the resultant's DIRECTION mapped to [-0.5, 0.5): R is a concentration test,
    not a location test, so a large R at a large |mu| is a common non-integer offset, not
    integer content.
    """
    e = np.asarray(e, dtype=float)
    e = e[np.isfinite(e)]
    n = int(e.size)
    if n == 0:
        return {"n": 0}
    out = {
        "n": n,
        "std": float(np.std(e, ddof=1)) if n > 1 else 0.0,
        "rms": float(np.sqrt(np.mean(e ** 2))),
        "p50_abs": float(np.median(np.abs(e))),
        "p95_abs": float(np.percentile(np.abs(e), 95)),
        "wrappable": bool(wrappable),
    }
    if not wrappable:
        return out
    f = wrap(e)
    absf = np.abs(f)
    z = np.mean(np.exp(2j * np.pi * e))
    R = float(np.abs(z))
    out.update({
        "frac_p50": float(np.median(absf)),
        "frac_rms": float(np.sqrt(np.mean(f ** 2))),
        "frac_lt_015": float(np.mean(absf < 0.15)),
        "frac_lt_025": float(np.mean(absf < 0.25)),
        "R": R,
        "R_null": float(math.sqrt(math.pi / (4.0 * n))),
        "R_gap": R - float(math.sqrt(math.pi / (4.0 * n))),
        "mu": float(np.angle(z) / (2.0 * np.pi)),
    })
    return out


def nl_from(e1, e2, f1, f2):
    """NL-cycle error implied by per-band errors e1, e2 with the widelane substituted.

    B_IF = (f1^2 lam1 N1 - f2^2 lam2 N2)/(f1^2-f2^2) [m]; with N_WL = N1-N2 known,
    B_IF = lam_NL*N1 - (f2/(f1+f2))*lam2*N_WL, so e_NL = e_IF/lam_NL with
    e_IF = (f1^2 lam1 e1 - f2^2 lam2 e2)/(f1^2-f2^2).  Since f^2*lam = c*f this reduces to
    the closed form below (asserted in __main__ against the metres derivation).
    Expanding: e_NL = e1 + (f2/(f1-f2))*(e1-e2)  --  the widelane residue enters amplified
    by f2/(f1-f2) (3.53x on GPS L1/L2, 3.33x on Galileo E1/E5a).
    """
    return (f1 * np.asarray(e1) - f2 * np.asarray(e2)) / (f1 - f2)


def lam_nl(f1, f2):
    return C_LIGHT / (f1 + f2)


def lam_wl(f1, f2):
    return C_LIGHT / (f1 - f2)


# ---------------------------------------------------------------------------
def find_station(run_dir, explicit):
    if explicit:
        return explicit
    cfg_log = os.path.join(run_dir, "logs", "gnss_config.log")
    if os.path.isfile(cfg_log):
        pat = re.compile(r"held-out consumer '([A-Za-z0-9_]+)'")
        with open(cfg_log, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = pat.search(line)
                if m:
                    return m.group(1)
    raise SystemExit("could not determine the consumer station; pass --station")


def load_truth(fixture, station):
    path = os.path.join(fixture, station, "truth_ambiguities.csv")
    if not os.path.isfile(path):
        raise SystemExit("truth ambiguities not found: %s" % path)
    N, lam = {}, {}
    with open(path, "r", encoding="utf-8") as fh:
        hdr = fh.readline().strip().split(",")
        if hdr[:4] != ["satellite", "signal", "N_int", "wavelength_m"]:
            raise SystemExit("unexpected truth_ambiguities header: %s" % hdr)
        for line in fh:
            p = line.strip().split(",")
            if len(p) < 4:
                continue
            N[(p[0], p[1])] = int(p[2])
            lam[(p[0], p[1])] = float(p[3])
    slips = 0
    spath = os.path.join(fixture, station, "truth_slips.csv")
    if os.path.isfile(spath):
        with open(spath, "r", encoding="utf-8") as fh:
            fh.readline()
            slips = sum(1 for line in fh if line.strip())
    return N, lam, slips


def load_float(run_dir):
    path = os.path.join(run_dir, "logs", "ambiguity_trace.csv")
    if not os.path.isfile(path):
        raise SystemExit("consumer ambiguity trace not found: %s" % path)
    per_epoch, week = {}, None
    with open(path, "r", encoding="utf-8") as fh:
        hdr = fh.readline().strip().split(",")
        if hdr != ["week", "tow_s", "key", "value_m", "std_m"]:
            raise SystemExit("unexpected ambiguity_trace header: %s" % hdr)
        for line in fh:
            p = line.rstrip("\n").split(",")
            if len(p) < 5:
                continue
            if week is None:
                week = int(p[0])
            key = p[2]
            us = key.find("_")
            if us <= 0:
                continue
            try:
                per_epoch.setdefault(float(p[1]), {})[(key[:us], key[us + 1:])] = \
                    (float(p[3]), float(p[4]))
            except ValueError:
                continue
    return per_epoch, week


def load_closed_loop(run_dir):
    path = os.path.join(run_dir, "logs", "closed_loop_trace.csv")
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as fh:
        hdr = fh.readline().strip().split(",")
        idx = {k: i for i, k in enumerate(hdr)}
        rows = [ln.rstrip("\n").split(",") for ln in fh if ln.strip()]
    return hdr, idx, [r for r in rows if len(r) >= len(hdr)]


def count_rt_resets(run_dir):
    out = Counter()
    path = os.path.join(run_dir, "logs", "rt_engine.log")
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "SSR phase-bias discontinuity" in line:
                out["ssr_phase_bias_reset"] += 1
            elif "SSR code-bias discontinuity" in line:
                out["ssr_code_bias_disc"] += 1
            elif "SSR clock discontinuity" in line:
                out["ssr_clock_disc"] += 1
    return out


# ---------------------------------------------------------------------------
def compute(per_epoch, truthN):
    """Per-epoch between-satellite SD errors.

    Reference: ONE satellite per (epoch, constellation) -- the one present in BOTH float
    and truth on EVERY band of that constellation with the smallest sum-of-float-std.  A
    single reference across bands is REQUIRED for the lanes to be defined (a per-band
    reference would mix two different reference errors into e_WL).  A secondary per-band
    reference (min std on that band alone) is kept for the per-band table.
    """
    tows = sorted(per_epoch)
    tow0 = tows[0]
    band_e = defaultdict(list)              # (const, band) -> [(tow, sat, e_raw)]
    band_e_pbref = defaultdict(list)
    lane_e = defaultdict(list)              # (const, lane) -> [(tow, sat, e)]
    refs = defaultdict(Counter)
    fixable = []
    sd_sigma = defaultdict(list)
    zd_sigma = defaultdict(list)

    for tow in tows:
        ep = per_epoch[tow]
        fix = {"tow": tow}
        for const, bands in BANDS.items():
            fr = FREQ[const]
            sats = sorted({s for (s, _g) in ep if s.startswith(const)})
            cand = [s for s in sats
                    if all((s, b) in ep and (s, b) in truthN for b in bands)]
            if len(cand) < 2:
                continue
            ref = min(cand, key=lambda s: sum(ep[(s, b)][1] for b in bands))
            refs[const][ref] += 1

            e_by_band = {}
            for b in bands:
                lam = C_LIGHT / fr[b]
                vref, sref = ep[(ref, b)]
                Nref = truthN[(ref, b)]
                d = {}
                for s in sats:
                    if s == ref or (s, b) not in ep or (s, b) not in truthN:
                        continue
                    v, sd = ep[(s, b)]
                    e = (v - vref) / lam - (truthN[(s, b)] - Nref)
                    d[s] = e
                    band_e[(const, b)].append((tow, s, e))
                    sd_sigma[(const, b)].append((tow, math.hypot(sd, sref) / lam))
                    zd_sigma[(const, b)].append((tow, sd / lam))
                e_by_band[b] = d
                bsats = [s for s in sats if (s, b) in ep and (s, b) in truthN]
                if len(bsats) >= 2:
                    bref = min(bsats, key=lambda s: ep[(s, b)][1])
                    vb = ep[(bref, b)][0]
                    Nb = truthN[(bref, b)]
                    for s in bsats:
                        if s != bref:
                            band_e_pbref[(const, b)].append(
                                (tow, s, (ep[(s, b)][0] - vb) / lam - (truthN[(s, b)] - Nb)))

            for lane, (bhi, blo) in LANES[const].items():
                if bhi not in e_by_band or blo not in e_by_band:
                    continue
                f1, f2 = fr[bhi], fr[blo]
                common = sorted(set(e_by_band[bhi]) & set(e_by_band[blo]))
                nfix = 0
                for s in common:
                    e1, e2 = e_by_band[bhi][s], e_by_band[blo][s]
                    if lane == "NL":
                        # raw (task formula: the gauge integers ride through -> NOT
                        # wrappable) and integer-removed (the per-band gauge integers
                        # rounded away, i.e. "NL given the recovered widelane").
                        lane_e[(const, "NL_raw")].append((tow, s, nl_from(e1, e2, f1, f2)))
                        r1, r2 = float(wrap(e1)), float(wrap(e2))
                        e = float(nl_from(r1, r2, f1, f2))
                        lane_e[(const, "NL")].append((tow, s, e))
                    else:
                        e = e1 - e2
                        lane_e[(const, lane)].append((tow, s, e))
                    if abs(float(wrap(e))) < 0.25:
                        nfix += 1
                fix["%s_%s_n" % (const, lane)] = len(common)
                fix["%s_%s_ok" % (const, lane)] = nfix
        fixable.append(fix)

    return {"tow0": tow0, "tows": tows, "band_e": band_e, "band_e_pbref": band_e_pbref,
            "lane_e": lane_e, "refs": refs, "fixable": fixable,
            "sd_sigma": sd_sigma, "zd_sigma": zd_sigma}


def window_filter(recs, tow0, lo_h, hi_h):
    lo, hi = tow0 + lo_h * 3600.0, tow0 + hi_h * 3600.0
    return np.array([e for (t, _s, e) in recs if lo <= t < hi], dtype=float)


def demeaned(recs, tow0, lo_h, hi_h):
    """Per-epoch mean removed -- a reference-FREE view (it removes the reference
    satellite's own error, common to every chord by construction).  UNWRAPPED only:
    removing a group's own centre shrinks a wrapped |residual| ~42 % on pure noise."""
    lo, hi = tow0 + lo_h * 3600.0, tow0 + hi_h * 3600.0
    per = defaultdict(list)
    for (t, _s, e) in recs:
        if lo <= t < hi:
            per[t].append(e)
    out = []
    for v in per.values():
        if len(v) >= 2:
            a = np.asarray(v, dtype=float)
            out.extend(a - a.mean())
    return np.asarray(out, dtype=float)


def _segment(pts, min_len):
    """pts = [(tow, e)] -> per-arc (within-arc std, |wrap(arc mean)|, length).

    A new arc starts on a gap > 90 s or a |de| > 0.5 cyc step (a producer/consumer arc
    reset).  Within an arc the series is continuous, so the arc mean is the arc's own
    integer + its offset and the std about it is the part a cascade could average down.
    """
    stds, offs, lens = [], [], []

    def flush(arc):
        if len(arc) >= min_len:
            a = np.asarray(arc, float)
            stds.append(float(np.std(a, ddof=1)))
            offs.append(abs(float(wrap(a.mean()))))
            lens.append(len(a))

    arc = []
    for i, (t, e) in enumerate(pts):
        if arc and (t - pts[i - 1][0] > 90.0 or abs(e - pts[i - 1][1]) > 0.5):
            flush(arc)
            arc = []
        arc.append(e)
    flush(arc)
    return stds, offs, lens


def arc_stats(per_epoch, truthN, tow0, lo_h, hi_h, min_len=40):
    """Is the wrapped residue NOISE (averages down) or a stable per-arc BIAS?

    Uses ONE fixed reference satellite per constellation -- the most-tracked one in the
    window, so every chord and every lane shares it and each is a continuous series.
    """
    lo, hi = tow0 + lo_h * 3600.0, tow0 + hi_h * 3600.0
    tows = sorted(t for t in per_epoch if lo <= t < hi)
    out = {}
    for const, bands in BANDS.items():
        cnt = Counter()
        for t in tows:
            for (s, g) in per_epoch[t]:
                if s.startswith(const) and g in bands and (s, g) in truthN:
                    cnt[s] += 1
        if len(cnt) < 2:
            continue
        ref = cnt.most_common(1)[0][0]
        # (band, sat) -> [(tow, e)]
        series = defaultdict(list)
        for t in tows:
            ep = per_epoch[t]
            for b in bands:
                if (ref, b) not in ep or (ref, b) not in truthN:
                    continue
                lam = C_LIGHT / FREQ[const][b]
                vr, Nr = ep[(ref, b)][0], truthN[(ref, b)]
                for (s, g) in ep:
                    if g != b or s == ref or not s.startswith(const) or (s, b) not in truthN:
                        continue
                    series[(b, s)].append(
                        (t, (ep[(s, b)][0] - vr) / lam - (truthN[(s, b)] - Nr)))
        chans = [(b, {s: dict(series[(b, s)]) for (bb, s) in series if bb == b})
                 for b in bands]
        lane_series = {}
        for lane in ("WL", "EWL", "NL"):
            bhi, blo = LANES[const][lane]
            d1 = {s: dict(series[(bhi, s)]) for (bb, s) in series if bb == bhi}
            d2 = {s: dict(series[(blo, s)]) for (bb, s) in series if bb == blo}
            f1, f2 = FREQ[const][bhi], FREQ[const][blo]
            per_sat = {}
            for s in set(d1) & set(d2):
                pts = []
                for t in sorted(set(d1[s]) & set(d2[s])):
                    e1, e2 = d1[s][t], d2[s][t]
                    if lane == "NL":
                        pts.append((t, float(nl_from(float(wrap(e1)), float(wrap(e2)),
                                                     f1, f2))))
                    else:
                        pts.append((t, e1 - e2))
                if pts:
                    per_sat[s] = pts
            lane_series[lane] = per_sat

        for label, per_sat in ([("%s %s" % (CONST_NAME[const], b),
                                 {s: sorted(d.items()) for s, d in dd.items()})
                                for (b, dd) in chans]
                               + [("%s %s" % (CONST_NAME[const], l), per_sat)
                                  for l, per_sat in lane_series.items()]):
            stds, offs, lens = [], [], []
            for s, pts in per_sat.items():
                a, b_, c = _segment(sorted(pts), min_len)
                stds += a
                offs += b_
                lens += c
            if stds:
                out[label] = {
                    "ref": ref, "n_arcs": len(stds),
                    "arc_len_p50_ep": float(np.median(lens)),
                    "within_arc_std_p50": float(np.median(stds)),
                    "within_arc_std_p90": float(np.percentile(stds, 90)),
                    "arc_offset_p50": float(np.median(offs)),
                    "arc_offset_lt_015": float(np.mean(np.asarray(offs) < 0.15)),
                }
    return out


# ---------------------------------------------------------------------------
def hdr_line():
    return ("%-24s %7s %9s %9s %9s %9s | %8s %8s %8s %8s | %7s %7s %8s"
            % ("channel", "n", "std", "rms", "p50|e|", "p95|e|",
               "p50|fr|", "rms fr", "<0.15", "<0.25", "R", "R_null", "mu"))


def fmt_stat(label, st):
    if not st or st.get("n", 0) == 0:
        return "%-24s %7s" % (label, "-")
    base = ("%-24s %7d %9.3f %9.3f %9.3f %9.3f"
            % (label, st["n"], st["std"], st["rms"], st["p50_abs"], st["p95_abs"]))
    if not st.get("wrappable", True):
        return base + " |  (not integer-shift invariant -- see NL note)"
    return base + (" | %8.4f %8.4f %8.4f %8.4f | %7.4f %7.4f %+8.4f"
                   % (st["frac_p50"], st["frac_rms"], st["frac_lt_015"],
                      st["frac_lt_025"], st["R"], st["R_null"], st["mu"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True)
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--station", default=None)
    ap.add_argument("--settle-h", type=float, default=12.0)
    ap.add_argument("--out", default=None, help="output dir (default <run>/artrack)")
    args = ap.parse_args()

    run = os.path.abspath(args.run)
    fixture = os.path.abspath(args.fixture)
    station = find_station(run, args.station)
    out_dir = os.path.abspath(args.out) if args.out else os.path.join(run, "artrack")
    os.makedirs(out_dir, exist_ok=True)

    truthN, truthLam, n_slip = load_truth(fixture, station)
    per_epoch, week = load_float(run)
    res = compute(per_epoch, truthN)
    tow0, tows = res["tow0"], res["tows"]
    span_h = (tows[-1] - tow0) / 3600.0
    settle = args.settle_h
    end_h = math.ceil(span_h) + 1e-6

    lam_check = []
    for const, bands in BANDS.items():
        for b in bands:
            got = [truthLam[k] for k in truthLam if k[0][0] == const and k[1] == b]
            if got:
                cf = C_LIGHT / FREQ[const][b]
                lam_check.append((CONST_NAME[const], b, cf, float(np.mean(got)),
                                  float(np.max(np.abs(np.array(got) - cf)))))

    J = {"run": run, "fixture": fixture, "station": station, "week": week,
         "tow_first": tow0, "tow_last": tows[-1], "n_epochs": len(tows),
         "span_h": span_h, "settle_h": settle, "truth_slip_rows": n_slip,
         "lambda_check": [{"const": a, "band": b, "c_over_f": c, "truth_mean": d,
                           "max_abs_diff": e} for (a, b, c, d, e) in lam_check]}

    L = []
    P = L.append
    P("=" * 126)
    P("CONSUMER FLOAT vs TRUTH  --  integer-combination fixability of the closed-loop consumer")
    P("=" * 126)
    P("run       : %s" % run)
    P("fixture   : %s" % fixture)
    P("consumer  : %s   (week %s, tow %.0f .. %.0f, %d epochs, %.2f h)"
      % (station, week, tow0, tows[-1], len(tows), span_h))
    P("windows   : h0-h%g (transient) and h%g-h%.0f (settled)" % (settle, settle, span_h))
    P("")
    P("TRUTH SEMANTICS (src/sim/main.cpp, the truth_ambiguities.csv writer)")
    P("  header 'satellite,signal,N_int,wavelength_m'; the writer's own comments:")
    P("    'Integer carrier ambiguities are arc-constant per (sat, band) and recorded once,")
    P("     the first epoch each (sat, signal) is seen.'")
    P("    'if (amb_seen.insert(akey).second) // record the INITIAL integer (pre-slip)'")
    P("  truth_slips.csv on this station holds %d slip row(s) -> N_int %s the true integer"
      % (n_slip, "IS" if n_slip == 0 else "is NOT everywhere"))
    P("  for the whole day.  The sim's phase model is")
    P("    L_b = (P + tropo + m7 + ant + bias_phase + rxb_phase + isb - I_b + windup")
    P("           + lambda_b*N + np + np_scint + mp_phase) / lambda_b")
    P("  so N_int is the PURE INTEGER; the satellite phase OSB (bias_phase, truth_bias.csv)")
    P("  is satellite-dependent and does NOT cancel between satellites, while the receiver")
    P("  phase bias (rxb_phase) and the GPS->Gal ISB are satellite-independent and DO.")
    P("  => only a WITHIN-CONSTELLATION between-satellite SD is comparable to truth.")
    P("")
    P("THE GAUGE INTEGER  (why the raw SD error below is TENS of cycles)")
    P("  The producer pins each (sat,signal) phase bias at an ANCHOR receiver's own absolute")
    P("  ambiguity estimate, so the consumer's ambiguity converges to N_consumer - N_anchor.")
    P("  The SD error is therefore  e = -(N_anchor,i - N_anchor,ref) + eps  = an exact gauge")
    P("  INTEGER plus the real residue eps.  The integer cancels in the wire->consumer loop,")
    P("  so the UNWRAPPED std measures the gauge (reported, flagged) and the WRAPPED residue")
    P("  eps = e - round(e) is the fixability quantity.  Per-band and WL/EWL wrapping is")
    P("  legitimate; the NL is integer-shift invariant ONLY once the widelane is fixed")
    P("  ((f1*n1-f2*n2)/(f1-f2) = n1 + n_WL*f2/(f1-f2)), so both forms are reported.")
    P("")
    P("-- lambda cross-check (c/f vs truth_ambiguities.csv wavelength_m) --")
    for (cn, b, cf, tm, mx) in lam_check:
        P("   %-8s %-4s  c/f = %.12f m   truth = %.12f m   max|diff| = %.2e"
          % (cn, b, cf, tm, mx))
    P("")
    P("-- lane wavelengths and the widelane-residue amplification into the NL --")
    for const in ("G", "E"):
        fr = FREQ[const]
        for lane in ("WL", "EWL", "NL"):
            bhi, blo = LANES[const][lane]
            f1, f2 = fr[bhi], fr[blo]
            lw = lam_nl(f1, f2) if lane == "NL" else lam_wl(f1, f2)
            extra = ("   e_NL = e_hi + %.2f * e_WL" % (f2 / (f1 - f2))) if lane == "NL" else ""
            P("   %-8s %-4s %s-%s   lambda = %8.4f m  (0.25 cyc = %7.1f mm)%s"
              % (CONST_NAME[const], lane, bhi, blo, lw, 250.0 * abs(lw), extra))
    P("")

    J["windows"] = {}
    for wname, (lo_h, hi_h) in (("h0-h%g" % settle, (0.0, settle)),
                                ("h%g-h%.0f" % (settle, span_h), (settle, end_h))):
        P("=" * 126)
        P("WINDOW %s   SD errors in CYCLES of the named channel." % wname)
        P("  one common reference satellite per (epoch, constellation) = min sum-of-float-std")
        P("  among satellites present in float AND truth on every band of that constellation")
        P("=" * 126)
        P(hdr_line())
        P("-" * 126)
        wj = {}
        for const in ("G", "E"):
            for b in BANDS[const]:
                st = stat_block(window_filter(res["band_e"][(const, b)], tow0, lo_h, hi_h))
                lbl = "%s SD %s" % (CONST_NAME[const], b)
                P(fmt_stat(lbl, st))
                wj[lbl] = st
            P("-" * 126)
            for lane in ("WL", "EWL", "NL", "NL_raw"):
                key = (const, lane)
                if not res["lane_e"][key]:
                    continue
                bhi, blo = LANES[const]["NL" if lane.startswith("NL") else lane]
                st = stat_block(window_filter(res["lane_e"][key], tow0, lo_h, hi_h),
                                wrappable=(lane != "NL_raw"))
                nm = {"WL": "WL", "EWL": "EWL",
                      "NL": "NL|int-removed", "NL_raw": "NL|raw(gauge in)"}[lane]
                lbl = "%s SD %s(%s-%s)" % (CONST_NAME[const], nm, bhi, blo)
                P(fmt_stat(lbl, st))
                wj[lbl] = st
            P("-" * 126)
        P("uniform null                   -         -         -         -         -"
          " |   0.2500   0.2887   0.3000   0.5000 |  R_null    ---       ---")
        P("")
        P("-- REFERENCE-FREE cross-check (per-epoch mean removed; UNWRAPPED only) --")
        P("%-30s %8s %10s %10s %10s" % ("channel", "n", "std", "p50|e|", "p95|e|"))
        rf = {}
        for const in ("G", "E"):
            for ch, recs in ([(b, res["band_e"][(const, b)]) for b in BANDS[const]]
                             + [(l, res["lane_e"][(const, l)]) for l in ("WL", "EWL", "NL")]):
                a = demeaned(recs, tow0, lo_h, hi_h)
                if a.size:
                    P("%-30s %8d %10.3f %10.3f %10.3f"
                      % ("%s SD %s" % (CONST_NAME[const], ch), a.size,
                         np.std(a, ddof=1), np.median(np.abs(a)),
                         np.percentile(np.abs(a), 95)))
                    rf["%s SD %s" % (CONST_NAME[const], ch)] = {
                        "n": int(a.size), "std": float(np.std(a, ddof=1)),
                        "p50_abs": float(np.median(np.abs(a)))}
        wj["_reffree"] = rf
        P("")
        P("-- per-band with a SECONDARY reference (min float std on THAT band alone) --")
        P(hdr_line())
        for const in ("G", "E"):
            for b in BANDS[const]:
                P(fmt_stat("%s SD %s" % (CONST_NAME[const], b),
                           stat_block(window_filter(res["band_e_pbref"][(const, b)],
                                                    tow0, lo_h, hi_h))))
        P("")
        J["windows"][wname] = wj

    # ---- arcs -----------------------------------------------------------------
    P("=" * 126)
    P("ARC STRUCTURE  --  is the wrapped residue NOISE (averages down) or a stable BIAS?")
    P("  ONE fixed reference per constellation (most-tracked satellite) so bands and lanes")
    P("  share it; a new arc starts on a gap > 90 s or a |de| > 0.5 cyc step (an arc reset).")
    P("  'instd' = within-arc std about the arc's own mean -- what averaging could remove.")
    P("  '|offset|' = |wrap(arc mean)| -- the IRREDUCIBLE per-arc bias (uniform null 0.25).")
    P("=" * 126)
    for wname, (lo_h, hi_h) in (("h0-h%g" % settle, (0.0, settle)),
                                ("h%g-h%.0f" % (settle, span_h), (settle, end_h))):
        A = arc_stats(per_epoch, truthN, tow0, lo_h, hi_h)
        P("-- %s --" % wname)
        P("%-16s %5s %7s %9s %11s %11s %12s %11s"
          % ("channel", "ref", "n_arcs", "len[ep]", "instd p50", "instd p90",
             "|offset| p50", "off<0.15"))
        for k, v in A.items():
            P("%-16s %5s %7d %9.0f %11.4f %11.4f %12.4f %11.4f"
              % (k, v["ref"], v["n_arcs"], v["arc_len_p50_ep"],
                 v["within_arc_std_p50"], v["within_arc_std_p90"],
                 v["arc_offset_p50"], v["arc_offset_lt_015"]))
        J.setdefault("arcs", {})[wname] = A
        P("")

    # ---- hourly ---------------------------------------------------------------
    chans = []
    for const in ("G", "E"):
        chans += [("%s_%s" % (const, b), res["band_e"][(const, b)]) for b in BANDS[const]]
        chans += [("%s_%s" % (const, l), res["lane_e"][(const, l)])
                  for l in ("WL", "EWL", "NL")]
    P("=" * 126)
    P("HOURLY EVOLUTION -- p50 of the WRAPPED |residue| (uniform null 0.25).  This is the")
    P("  convergence-rate view; the unwrapped std is gauge-dominated and follows below.")
    P("=" * 126)
    P("%-3s " % "h" + " ".join("%9s" % c[0] for c in chans))
    hf, hs = {}, {}
    for h in range(int(math.ceil(span_h)) + 1):
        cells, row = [], {}
        for name, recs in chans:
            a = window_filter(recs, tow0, h, h + 1)
            if a.size > 1:
                v = float(np.median(np.abs(wrap(a))))
                cells.append("%9.4f" % v)
                row[name] = v
            else:
                cells.append("%9s" % "-")
        if row:
            P("%-3d " % h + " ".join(cells))
            hf[h] = row
    P("")
    P("-- same, hourly UNWRAPPED std (gauge-contaminated; magnitude only) --")
    P("%-3s " % "h" + " ".join("%9s" % c[0] for c in chans))
    for h in range(int(math.ceil(span_h)) + 1):
        cells, row = [], {}
        for name, recs in chans:
            a = window_filter(recs, tow0, h, h + 1)
            if a.size > 1:
                v = float(np.std(a, ddof=1))
                cells.append("%9.3f" % v)
                row[name] = v
            else:
                cells.append("%9s" % "-")
        if row:
            P("%-3d " % h + " ".join(cells))
            hs[h] = row
    J["hourly_frac_p50"] = hf
    J["hourly_std"] = hs
    P("")

    # ---- fixable set ----------------------------------------------------------
    P("=" * 126)
    P("FIXABLE SET PER EPOCH  --  |wrapped residue| < 0.25 cyc would round to the correct")
    P("  integer (uniform null = 0.50 of the set)")
    P("=" * 126)
    P("%-20s %-12s %8s %10s %10s %10s %11s"
      % ("channel", "window", "epochs", "mean n", "mean ok", "ok rate", "ep all-ok"))
    fx = {}
    for const in ("G", "E"):
        for lane in ("WL", "EWL", "NL"):
            for wname, (lo_h, hi_h) in (("h0-h%g" % settle, (0.0, settle)),
                                        ("h%g+" % settle, (settle, span_h + 1))):
                lo, hi = tow0 + lo_h * 3600.0, tow0 + hi_h * 3600.0
                ns, oks = [], []
                for f in res["fixable"]:
                    k = "%s_%s_n" % (const, lane)
                    if lo <= f["tow"] < hi and k in f:
                        ns.append(f[k])
                        oks.append(f["%s_%s_ok" % (const, lane)])
                if not ns:
                    continue
                na, oa = np.array(ns, float), np.array(oks, float)
                P("%-20s %-12s %8d %10.2f %10.2f %10.4f %11.4f"
                  % ("%s %s" % (CONST_NAME[const], lane), wname, len(ns), na.mean(),
                     oa.mean(), oa.sum() / max(na.sum(), 1.0), float(np.mean(oa == na))))
                fx["%s_%s_%s" % (const, lane, wname)] = {
                    "epochs": len(ns), "mean_n": float(na.mean()),
                    "mean_ok": float(oa.mean()),
                    "ok_rate": float(oa.sum() / max(na.sum(), 1.0)),
                    "epochs_all_ok": float(np.mean(oa == na))}
    J["fixable"] = fx
    P("")

    # ---- float sigma ----------------------------------------------------------
    P("=" * 126)
    P("FLOAT SIGMA  --  the trace's std_m in cycles, p50 over h%g+  (SD sigma removes the"
      % settle)
    P("  receiver-bias/clock uncertainty the ZD sigma carries)")
    P("=" * 126)
    P("%-18s %10s %14s %14s   %s"
      % ("channel", "n", "ZD sig[cyc]", "SD sig[cyc]", "SD = sqrt(sig_s^2+sig_ref^2)/lambda"))
    sg = {}
    for const in ("G", "E"):
        for b in BANDS[const]:
            zd = window_filter([(t, "", v) for (t, v) in res["zd_sigma"][(const, b)]],
                               tow0, settle, span_h + 1)
            sd = window_filter([(t, "", v) for (t, v) in res["sd_sigma"][(const, b)]],
                               tow0, settle, span_h + 1)
            if zd.size:
                P("%-18s %10d %14.4f %14.4f"
                  % ("%s %s" % (CONST_NAME[const], b), zd.size,
                     np.median(zd), np.median(sd)))
                sg["%s_%s" % (const, b)] = {
                    "n": int(zd.size), "zd_sigma_cyc_p50": float(np.median(zd)),
                    "sd_sigma_cyc_p50": float(np.median(sd))}
    J["float_sigma"] = sg
    P("")

    # ---- resolver -------------------------------------------------------------
    P("=" * 126)
    P("RESOLVER ATTEMPT STRUCTURE  (closed_loop_trace.csv + rt_engine.log)")
    P("=" * 126)
    cl = load_closed_loop(run)
    if cl:
        hdr, idx, rows = cl
        P("closed_loop_trace.csv header: %s" % ",".join(hdr))
        ran = sum(1 for r in rows if r[idx["ar_ran"]] == "1")
        suc = sum(1 for r in rows if r[idx["ar_success"]] == "1")
        nfix = sum(int(r[idx["num_fixed"]]) for r in rows)
        cand = np.array([int(r[idx["num_candidates"]]) for r in rows], float)
        ratio = np.array([float(r[idx["ratio"]]) for r in rows], float)
        gates = Counter(re.sub(r"\(.*\)", "(..)", r[idx["ar_gate"]]) for r in rows)
        err = np.array([float(r[idx["pos_err_3d_m"]]) for r in rows], float)
        tw = np.array([float(r[idx["tow"]]) for r in rows], float)
        m = tw >= tow0 + settle * 3600.0
        P("epochs=%d  ar_ran=%d  ar_success=%d  total num_fixed=%d" % (len(rows), ran, suc, nfix))
        P("num_candidates (= SD pair count handed to LAMBDA): min %d  p50 %.0f  mean %.1f  max %d"
          % (cand.min(), np.median(cand), cand.mean(), cand.max()))
        P("ratio: min %.2f  p50 %.2f  p95 %.2f  max %.2f  (threshold 3.0; epochs >= 3.0: %d)"
          % (ratio.min(), np.median(ratio), np.percentile(ratio, 95), ratio.max(),
             int((ratio >= 3.0).sum())))
        P("ar_gate: %s" % ", ".join("%s=%d" % kv for kv in gates.most_common()))
        P("consumer 3D pos err: all p50 %.3f m rms %.3f m ; h%g+ p50 %.3f m rms %.3f m"
          % (np.median(err), np.sqrt(np.mean(err ** 2)), settle,
             np.median(err[m]), np.sqrt(np.mean(err[m] ** 2))))
        J["resolver"] = {"epochs": len(rows), "ar_ran": ran, "ar_success": suc,
                         "num_fixed_total": nfix, "cand_min": int(cand.min()),
                         "cand_p50": float(np.median(cand)), "cand_max": int(cand.max()),
                         "ratio_p50": float(np.median(ratio)),
                         "ratio_max": float(ratio.max()),
                         "ratio_ge_3": int((ratio >= 3.0).sum()), "ar_gate": dict(gates),
                         "pos_err_p50_all": float(np.median(err)),
                         "pos_err_p50_settled": float(np.median(err[m]))}
    P("")
    P("AR FRAME (AmbiguityResolver::buildDdPairs, src/core/filter/AmbiguityResolver.cpp):")
    P("  the resolver walks the consumer's per-(sat,signal) ZD ambiguity STATES, groups them")
    P("  by (constellation, signal), picks the HIGHEST-ELEVATION member of each group as the")
    P("  pivot and emits one pair per non-pivot satellite.  So num_candidates is the")
    P("  BETWEEN-SATELLITE SD pair count and LAMBDA searches SD integers, per band, all bands")
    P("  jointly.  It is NOT a ZD fix (a ZD fix would be meaningless: the receiver phase bias")
    P("  rides in every ZD ambiguity) and it is NOT a WL/NL cascade.")
    rst = count_rt_resets(run)
    if rst:
        P("")
        P("consumer arc resets driven by the wire (rt_engine.log) -- each restarts an")
        P("ambiguity and its convergence:")
        for k, v in rst.most_common():
            P("   %-24s %6d   (%.3f per epoch)" % (k, v, v / max(len(tows), 1)))
        J["rt_resets"] = dict(rst)
    P("")
    P("SD reference occupancy (how often each satellite was the per-epoch SD reference):")
    for const in ("G", "E"):
        cc = res["refs"][const]
        if cc:
            P("   %-8s %s" % (CONST_NAME[const],
                              ", ".join("%s:%d" % kv for kv in cc.most_common(8))))
            J.setdefault("ref_occupancy", {})[CONST_NAME[const]] = dict(cc)
    P("")

    txt = os.path.join(out_dir, "consumer_float_truth_join.txt")
    jsn = os.path.join(out_dir, "consumer_float_truth_join.json")
    with open(txt, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    with open(jsn, "w", encoding="utf-8") as fh:
        json.dump(J, fh, indent=1, default=float)
    print("\n".join(L))
    print("wrote %s" % txt)
    print("wrote %s" % jsn)


if __name__ == "__main__":
    for (f1, f2) in ((1575.42e6, 1227.60e6), (1575.42e6, 1176.45e6)):
        l1, l2 = C_LIGHT / f1, C_LIGHT / f2
        e1, e2 = 0.37, -0.21
        e_if = (f1 * f1 * l1 * e1 - f2 * f2 * l2 * e2) / (f1 * f1 - f2 * f2)
        assert abs(e_if / lam_nl(f1, f2) - float(nl_from(e1, e2, f1, f2))) < 1e-9
        assert abs(float(nl_from(e1, e2, f1, f2))
                   - (e1 + f2 / (f1 - f2) * (e1 - e2))) < 1e-9
    sys.exit(main())
