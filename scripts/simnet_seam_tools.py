#!/usr/bin/env python3
"""simnet_seam_tools.py -- seam / wire-realization instruments for gnss_network_product v2.

Seven read-only instruments over the producer wire (products.txt) and the
closed-loop consumer trace, ported 1:1 from the seam-control campaign
scratchpad (docs/SPEC_synthetic_minimum_network.md section 18.9 quotes their
output, so the numbers are contractual).  One shared wire parser (records
E / C / B / P / R / U / H), one shared frequency table, stdlib only.

Subcommands (A, B = products.txt paths; tows in GPS seconds of week):

  diff      A B tow_lo tow_hi [every=1]
      Per-epoch realization difference A-B, per channel (C clock, P L1C,
      P L2W/L5Q, B C1C) and per constellation: n, common mode, SD, max.
  decompose A B tow
      One-epoch per-(sat,signal) split of A-B into dC / dB / dP, the
      code-side (dB-dC) and phase-side (dP-dC) change a consumer sees,
      with the sub-cycle part of the phase side and the URA on both wires.
  walk      A B t0 t_hi [every=40]
      Post-seam WALK of A-B relative to its value at t0: per-sat clock walk
      and per-key phase-side walk, between-sat SD / max per constellation.
  quality   A B t0 t_hi [every=40]
      Wire-quality series (URA, P sigma, sigcc, H status) on both wires plus
      the SUB-CYCLE phase-side walk of A-B (rms in cycles, count > 0.1 cyc).
  steps     W t_lo t_hi [every=1] [--t-ref T]
      Epoch-to-epoch phase-side step on ONE wire: n, rms, p50, max, counts
      above 0.05 m (the contract's disc_thresh_m) and above half a cycle,
      the clock-alone and PB-alone steps, and the sub-cycle rms.
  drift     W t_a t_b
      Total phase-side drift on ONE wire between two epochs, reduced to its
      sub-cycle part, per constellation; integer-class (> 0.5 cyc) count.
  consumer  run[:label] ... [--bin-min N] [--tow-lo T] [--tow-hi T] [--day0 T]
      Hourly consumer 3-D truth-error table (n, rms, p50, max) from
      <run>/logs/closed_loop_trace.csv, one column block per run.

Conventions shared by every subcommand:

  (a) Between-satellite statistics are COMMON-MODE REMOVED per constellation:
      the per-constellation MEDIAN of the quantity is subtracted first, so a
      wandering datum level (which a consumer's receiver clock absorbs) can
      not masquerade as a per-satellite effect.  'cm' columns print that
      median; 'sd' is the sample SD (n-1) of the residuals, 'max' their
      largest absolute value.  (`quality` and `steps` print a plain rms of
      already-differenced quantities instead -- no median step there.)
  (b) "Phase-side" means (P - C): satellite phase bias minus satellite clock,
      the combination a consumer's ambiguity absorbs.  Its SUB-CYCLE part is
      the mod-lambda CENTRED reduction  ((x/lambda + 1/2) mod 1) - 1/2  per
      signal wavelength, which removes integer re-references (the marked
      events) and SATURATES at +-lambda/2: a phase-side change larger than
      half a cycle is reported as its wrapped remainder, never its true size.
      Read the raw columns (ps, D, the |d| > 0.5 cyc counts) for the
      integer-class part.

Fine print the outputs depend on:
  * Wavelengths: GPS L1C / L2W / L5Q and Galileo L1C / L5Q / L7Q (FREQ_HZ);
    the code signal paired with phase signal Lxy is Cxy.
  * The H (heartbeat) record PRECEDES the E record of its own epoch on the
    wire, so the H status `quality` shows against epoch t is the heartbeat
    written at the start of the FOLLOWING epoch; the last epoch shows '-'.
  * `steps` counts "> 0.5 cyc" against a fixed 0.095 m (half of 0.19 m) for
    every signal; its sub-cycle rms uses the per-signal wavelength.
  * `steps` dt(h) is measured from --t-ref (default: t_lo); `consumer` bins
    are hours from --day0 (default 259200 = 00:00 of the product day, wk 2313).

--selftest writes a synthetic three-epoch wire pair to a temp dir (clock
+1.0 m on G02 stepping to +1.5 m at the third epoch; phase bias +0.19 m on
G03/L1C stepping to +0.21 m) plus a tiny consumer trace, runs every
subcommand on them and checks the printed numbers against hand-derived values.

Usage:
  python scripts/simnet_seam_tools.py diff runs/A/products.txt runs/B/products.txt 302400 303700 1
  python scripts/simnet_seam_tools.py decompose runs/A/products.txt runs/B/products.txt 302430
  python scripts/simnet_seam_tools.py walk runs/A/products.txt runs/B/products.txt 302430 331300 40
  python scripts/simnet_seam_tools.py quality runs/A/products.txt runs/B/products.txt 302430 331300 40
  python scripts/simnet_seam_tools.py steps runs/A/products.txt 302430 303700 2
  python scripts/simnet_seam_tools.py drift runs/A/products.txt 303630 316830
  python scripts/simnet_seam_tools.py consumer runs/A:warm runs/B:cont --tow-lo 302400 --tow-hi 345600
  python scripts/simnet_seam_tools.py --selftest
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import csv
import io
import math
import os
import re
import statistics as st
import sys
import tempfile

# ---------------------------------------------------------------------------
# Shared constants
# ---------------------------------------------------------------------------

C_LIGHT = 299792458.0

#: Carrier frequencies keyed on (constellation letter, RINEX phase signal).
FREQ_HZ = {
    ("G", "L1C"): 1575.42e6, ("G", "L2W"): 1227.60e6, ("G", "L5Q"): 1176.45e6,
    ("E", "L1C"): 1575.42e6, ("E", "L5Q"): 1176.45e6, ("E", "L7Q"): 1207.14e6,
}

NAN = float("nan")

#: `steps` half-cycle threshold (m): a fixed 0.19 m wavelength for every signal.
STEPS_HALF_CYCLE_M = 0.5 * 0.19

#: `consumer` default day origin: tow of 00:00 on the product day (wk 2313, DOY-129).
DEFAULT_DAY0 = 259200.0


def wavelength(sat, sig):
    """Carrier wavelength (m) of phase signal `sig` on satellite `sat` (constellation letter)."""
    return C_LIGHT / FREQ_HZ[(sat[0], sig)]


def centred_frac(x_m, lam):
    """Sub-cycle part of x_m, in cycles: ((x/lam + 1/2) mod 1) - 1/2, in [-1/2, 1/2)."""
    return (x_m / lam + 0.5) % 1.0 - 0.5


# ---------------------------------------------------------------------------
# Shared wire parser
# ---------------------------------------------------------------------------

_RE_SIGCC = re.compile(r"sigcc=([-0-9.e+]+)")
_RE_SIGMA = re.compile(r"sigma=([-0-9.e+]+)")


def _new_epoch(tokens):
    return {
        "E": tokens,
        "datum": tokens[6] if len(tokens) > 6 else "?",
        "dconst": tokens[7] if len(tokens) > 7 else "",
        "C": {},        # sat -> clock_c0_m
        "P": {},        # (sat, signal) -> phase bias m
        "B": {},        # (sat, signal) -> code bias m
        "R": [],        # retro sats (discontinuity bump) in wire order
        "U": {},        # sat -> URA m
        "Uv": [],       # every URA value in wire order (for medians)
        "Psig": [],     # every P 'sigma=' token in wire order
        "sigcc": [],    # every C 'sigcc=' token in wire order
        "H": None,      # [status, sigma0, n_sats] of the heartbeat seen while this epoch was current
    }


def parse_wire(path, tow_lo, tow_hi):
    """Parse a gnss_network_product v2 wire into {round(tow): epoch dict}.

    Only epochs with tow_lo <= tow <= tow_hi are kept; records between an
    out-of-window E and the next in-window E are dropped.  Insertion order of
    every per-epoch dict/list is wire order (the summations downstream depend
    on it at the ulp level).  Records of types not listed (S, G, I, F, K, ...)
    are ignored.
    """
    epochs = {}
    cur = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            t = line.rstrip("\n").split(",")
            k = t[0]
            if k == "E":
                tow = float(t[2])
                if tow < tow_lo or tow > tow_hi:
                    cur = None
                    continue
                cur = epochs.setdefault(round(tow), _new_epoch(t))
            elif cur is None:
                continue
            elif k == "C":
                if len(t) > 2 and t[2] != "":
                    try:
                        cur["C"][t[1]] = float(t[2])
                    except ValueError:
                        pass
                m = _RE_SIGCC.search(line)
                if m:
                    cur["sigcc"].append(float(m.group(1)))
            elif k == "P":
                try:
                    cur["P"][(t[1], t[2])] = float(t[3])
                except (ValueError, IndexError):
                    pass
                m = _RE_SIGMA.search(line)
                if m:
                    cur["Psig"].append(float(m.group(1)))
            elif k == "B":
                try:
                    cur["B"][(t[1], t[2])] = float(t[3])
                except (ValueError, IndexError):
                    pass
            elif k == "R":
                cur["R"].extend(t[1:])
            elif k == "U":
                if len(t) > 2:
                    try:
                        u = float(t[2])
                    except ValueError:
                        continue
                    cur["U"][t[1]] = u
                    cur["Uv"].append(u)
            elif k == "H":
                cur["H"] = t[3:6]
    return epochs


# ---------------------------------------------------------------------------
# Shared statistics helpers
# ---------------------------------------------------------------------------

def cm_sd_max(vals):
    """(n, median, sample SD about the median, max |residual|); nan below n=2."""
    if len(vals) < 2:
        return (len(vals), NAN, NAN, NAN)
    cm = st.median(vals)
    r = [v - cm for v in vals]
    sd = (sum(x * x for x in r) / (len(r) - 1)) ** 0.5
    return (len(vals), cm, sd, max(abs(x) for x in r))


def paired_cm_sd_max(dA, dB, keyconst):
    """Per-constellation (n, cm, sd, max) of A-B over the keys present in both dicts."""
    out = {}
    byc = collections.defaultdict(list)
    for key, a in dA.items():
        b = dB.get(key)
        if b is None:
            continue
        byc[keyconst(key)].append(a - b)
    for c, v in sorted(byc.items()):
        if len(v) < 2:
            out[c] = (len(v), v[0] if v else NAN, 0.0, 0.0)
            continue
        cm = st.median(v)
        r = [x - cm for x in v]
        sd = (sum(x * x for x in r) / (len(r) - 1)) ** 0.5
        out[c] = (len(v), cm, sd, max(abs(x) for x in r))
    return out


def rms_p50_max(v, absolute):
    """(n, rms, p50, max) -- of |v| when absolute, else of v as given."""
    if not v:
        return (0, NAN, NAN, NAN)
    s = sorted(abs(x) for x in v) if absolute else sorted(v)
    return (len(v), math.sqrt(sum(x * x for x in v) / len(v)), s[len(s) // 2], s[-1])


def med(v):
    return st.median(v) if v else NAN


def mx(v):
    return max(v) if v else NAN


# ---------------------------------------------------------------------------
# diff
# ---------------------------------------------------------------------------

def run_diff(A, B, tow_lo, tow_hi, every=1):
    ea = parse_wire(A, tow_lo, tow_hi)
    eb = parse_wire(B, tow_lo, tow_hi)
    common = sorted(set(ea) & set(eb))
    print(f"# A={A}\n# B={B}\n# common epochs {len(common)} (A {len(ea)}, B {len(eb)})")
    print("tow      dA dB Rcnt | const : C n cm sd max | P(L1C) n cm sd max | P(L2W/L5Q) n cm sd max | B(C1C) n cm sd max")
    for i, tow in enumerate(common):
        if i % every:
            continue
        a, b = ea[tow], eb[tow]
        cs = paired_cm_sd_max(a["C"], b["C"], lambda k: k[0])
        pl1 = paired_cm_sd_max({k: v for k, v in a["P"].items() if k[1] == "L1C"},
                               {k: v for k, v in b["P"].items() if k[1] == "L1C"}, lambda k: k[0][0])
        pl2 = paired_cm_sd_max({k: v for k, v in a["P"].items() if k[1] in ("L2W", "L5Q")},
                               {k: v for k, v in b["P"].items() if k[1] in ("L2W", "L5Q")}, lambda k: k[0][0])
        bc1 = paired_cm_sd_max({k: v for k, v in a["B"].items() if k[1] == "C1C"},
                               {k: v for k, v in b["B"].items() if k[1] == "C1C"}, lambda k: k[0][0])
        for c in sorted(set(cs) | set(pl1) | set(pl2) | set(bc1)):
            def fmt(d):
                if c not in d:
                    return "   -  "
                n, cm, sd, mxv = d[c]
                return f"{n:2d} {cm:+9.4f} {sd:8.4f} {mxv:8.4f}"
            print(f"{tow:8.0f} {a['datum']}{a['dconst']:>2s} {b['datum']}{b['dconst']:>2s} {len(a['R']):2d}/{len(b['R']):2d} | {c} : {fmt(cs)} | {fmt(pl1)} | {fmt(pl2)} | {fmt(bc1)}"
                  + (f"  RA={' '.join(a['R'][:12])}" if a["R"] else ""))


# ---------------------------------------------------------------------------
# decompose
# ---------------------------------------------------------------------------

def run_decompose(A, B, tow):
    ea = parse_wire(A, tow - 1, tow + 1)
    eb = parse_wire(B, tow - 1, tow + 1)
    a = ea[round(tow)]
    b = eb[round(tow)]
    rows = []
    for s in sorted(set(a["C"]) & set(b["C"])):
        dC = a["C"][s] - b["C"][s]
        for (ss, j), pa in sorted(a["P"].items()):
            if ss != s or (s, j) not in b["P"]:
                continue
            dP = pa - b["P"][(s, j)]
            lam = wavelength(s, j)
            ps = dP - dC
            frac = centred_frac(ps, lam)
            cj = "C" + j[1:]
            dB = a["B"].get((s, cj), NAN) - b["B"].get((s, cj), NAN)
            cs = dB - dC
            rows.append((s, j, dC, dB, dP, cs, ps, ps / lam, frac, a["U"].get(s, NAN), b["U"].get(s, NAN)))
    print(f"epoch {tow:.0f}: sat sig | dC(m) dB(m) dP(m) | code-side cs=dB-dC | phase-side ps=dP-dC  ps/lam  frac(cyc) | URA_A URA_B")
    for r in rows:
        print(f"{r[0]} {r[1]} | {r[2]:+8.3f} {r[3]:+8.4f} {r[4]:+9.3f} | {r[5]:+8.3f} | {r[6]:+9.3f} {r[7]:+9.3f} {r[8]:+6.3f} | {r[9]:.3f} {r[10]:.3f}")
    # summaries per constellation
    byc = collections.defaultdict(lambda: {"frac": [], "cs": []})
    for r in rows:
        byc[r[0][0]]["frac"].append(abs(r[8]))
        byc[r[0][0]]["cs"].append(abs(r[5]))
    for c, d in sorted(byc.items()):
        fr = sorted(d["frac"])
        cs = sorted(d["cs"])
        print(f"{c}: n={len(fr)} |frac| p50={fr[len(fr)//2]:.3f} p90={fr[int(len(fr)*0.9)]:.3f} max={fr[-1]:.3f} cyc ;"
              f" |code-side| p50={cs[len(cs)//2]:.3f} p90={cs[int(len(cs)*0.9)]:.3f} max={cs[-1]:.3f} m")
    # per-sat dC alone
    dcs = sorted(((s, a["C"][s] - b["C"][s]) for s in set(a["C"]) & set(b["C"])), key=lambda x: -abs(x[1]))
    print("largest |dC|:", " ".join(f"{s}:{v:+.2f}" for s, v in dcs[:12]))


# ---------------------------------------------------------------------------
# walk
# ---------------------------------------------------------------------------

def _seam_reference(a0, b0):
    """dC0 per sat and phase-side ps0 per key of A-B at the reference epoch."""
    dC0 = {s: a0["C"][s] - b0["C"][s] for s in set(a0["C"]) & set(b0["C"])}
    ps0 = {k: (a0["P"][k] - b0["P"][k]) - dC0[k[0]]
           for k in set(a0["P"]) & set(b0["P"]) if k[0] in dC0}
    return dC0, ps0


def run_walk(A, B, t0, thi, every=40):
    ea = parse_wire(A, t0 - 1, thi)
    eb = parse_wire(B, t0 - 1, thi)
    common = sorted(set(ea) & set(eb))
    dC0, ps0 = _seam_reference(ea[round(t0)], eb[round(t0)])
    print("tow      dt(h) | const | clock walk: n  cm  sd  max | phase-side walk (all signals): n  cm  sd  max | worst sats (clock walk)")
    for i, t in enumerate(common):
        if i % every:
            continue
        a, b = ea[t], eb[t]
        byc_c = collections.defaultdict(list)
        byc_p = collections.defaultdict(list)
        worst = []
        for s in set(a["C"]) & set(b["C"]) & set(dC0):
            w = (a["C"][s] - b["C"][s]) - dC0[s]
            byc_c[s[0]].append(w)
            worst.append((abs(w), s, w))
        for k in set(a["P"]) & set(b["P"]) & set(ps0):
            if k[0] not in a["C"] or k[0] not in b["C"]:
                continue
            w = ((a["P"][k] - b["P"][k]) - (a["C"][k[0]] - b["C"][k[0]])) - ps0[k]
            byc_p[k[0][0]].append(w)
        for c in sorted(set(byc_c) | set(byc_p)):
            nc, cmc, sdc, mxc = cm_sd_max(byc_c.get(c, []))
            np_, cmp_, sdp, mxp = cm_sd_max(byc_p.get(c, []))
            ws = " ".join(f"{s}:{w:+.2f}" for _, s, w in sorted(worst, reverse=True) if s[0] == c)[:60]
            print(f"{t:8.0f} {(t - t0) / 3600:5.2f} | {c} | {nc:2d} {cmc:+7.3f} {sdc:6.3f} {mxc:6.3f} | {np_:3d} {cmp_:+7.3f} {sdp:6.3f} {mxp:6.3f} | {ws}")


# ---------------------------------------------------------------------------
# quality
# ---------------------------------------------------------------------------

def run_quality(A, B, t0, thi, every=40):
    ea = parse_wire(A, t0 - 1, thi)
    eb = parse_wire(B, t0 - 1, thi)
    common = sorted(set(ea) & set(eb))
    dC0, ps0 = _seam_reference(ea[round(t0)], eb[round(t0)])
    print("tow      dt(h) | A: URA med/max  Psig med/max  sigcc med  H | B: URA med/max  Psig med/max  sigcc med  H | subcycle phase-side walk SD (cyc) E  G | n>0.1cyc E G")
    for i, t in enumerate(common):
        if i % every:
            continue
        a, b = ea[t], eb[t]
        fr = collections.defaultdict(list)
        for k in set(a["P"]) & set(b["P"]) & set(ps0):
            s = k[0]
            if s not in a["C"] or s not in b["C"]:
                continue
            w = ((a["P"][k] - b["P"][k]) - (a["C"][s] - b["C"][s])) - ps0[k]
            fr[s[0]].append(centred_frac(w, wavelength(s, k[1])))

        def sd(v):
            return (sum(x * x for x in v) / len(v)) ** 0.5 if v else NAN

        def nbig(v):
            return sum(1 for x in v if abs(x) > 0.1)

        print(f"{t:8.0f} {(t - t0) / 3600:5.2f} | {med(a['Uv']):6.3f}/{mx(a['Uv']):6.3f} {med(a['Psig']):6.3f}/{mx(a['Psig']):6.3f} {med(a['sigcc']):6.3f} {a['H'][0] if a['H'] else '-':>3s} | {med(b['Uv']):6.3f}/{mx(b['Uv']):6.3f} {med(b['Psig']):6.3f}/{mx(b['Psig']):6.3f} {med(b['sigcc']):6.3f} {b['H'][0] if b['H'] else '-':>3s} | {sd(fr['E']):6.3f} {sd(fr['G']):6.3f} | {nbig(fr['E']):2d}/{len(fr['E']):2d} {nbig(fr['G']):2d}/{len(fr['G']):2d}")


# ---------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------

def run_steps(W, lo, hi, every=1, t_ref=None):
    if t_ref is None:
        t_ref = lo
    ep = parse_wire(W, lo - 31, hi)
    ts = sorted(ep)
    print("tow      dt(h) | phase-side d=(P-C) step: n rms p50 max | n>0.05m n>0.5cyc | clock dC: rms max | PB dP: rms max | subcycle(P-C) rms(cyc)")
    for i in range(1, len(ts)):
        if (i - 1) % every:
            continue
        a, b = ep[ts[i]], ep[ts[i - 1]]
        if ts[i] - ts[i - 1] > 31:
            continue
        d, dc, dp, fr = [], [], [], []
        for s in set(a["C"]) & set(b["C"]):
            dc.append(a["C"][s] - b["C"][s])
        for k in set(a["P"]) & set(b["P"]):
            s = k[0]
            if s not in a["C"] or s not in b["C"]:
                continue
            dP = a["P"][k] - b["P"][k]
            dp.append(dP)
            x = dP - (a["C"][s] - b["C"][s])
            d.append(x)
            fr.append(centred_frac(x, wavelength(s, k[1])))
        n, rms, p50, mxd = rms_p50_max(d, absolute=True)
        nbig = sum(1 for x in d if abs(x) > 0.05)
        nint = sum(1 for x in d if abs(x) > STEPS_HALF_CYCLE_M)
        _, rc, _, mc = rms_p50_max(dc, absolute=True)
        _, rp, _, mp = rms_p50_max(dp, absolute=True)
        frms = math.sqrt(sum(f * f for f in fr) / len(fr)) if fr else NAN
        print(f"{ts[i]:8.0f} {(ts[i] - t_ref) / 3600:6.3f} | {n:3d} {rms:7.4f} {p50:7.4f} {mxd:7.3f} | {nbig:3d} {nint:3d} | {rc:7.4f} {mc:7.3f} | {rp:7.4f} {mp:7.3f} | {frms:6.3f}")


# ---------------------------------------------------------------------------
# drift
# ---------------------------------------------------------------------------

def run_drift(W, ta, tb):
    ea = parse_wire(W, ta - 1, ta + 1)[round(ta)]
    eb = parse_wire(W, tb - 1, tb + 1)[round(tb)]
    byc = collections.defaultdict(list)
    big = collections.Counter()
    rows = []
    for k in set(ea["P"]) & set(eb["P"]):
        s = k[0]
        if s not in ea["C"] or s not in eb["C"]:
            continue
        D = (eb["P"][k] - eb["C"][s]) - (ea["P"][k] - ea["C"][s])
        lam = wavelength(s, k[1])
        sub = centred_frac(D, lam) * lam
        if abs(D) > 0.5 * lam:
            big[s[0]] += 1
        byc[s[0]].append(sub)
        rows.append((abs(sub), k, sub, D))
    for c, v in sorted(byc.items()):
        a = sorted(abs(x) for x in v)
        m = sum(v) / len(v)
        sd = (sum((x - m) ** 2 for x in v) / (len(v) - 1)) ** 0.5
        print(f"{c}: n={len(v)} subcycle drift mean={m:+.4f} sd={sd:.4f} p50|.|={a[len(a)//2]:.4f} max|.|={a[-1]:.4f} m ; integer-class |D|>0.5cyc: {big[c]}")
    rows.sort(reverse=True)
    print("largest sub-cycle drifts:", " ".join(f"{k[0]}/{k[1]}:{sub:+.3f}" for _, k, sub, D in rows[:10]))


# ---------------------------------------------------------------------------
# consumer
# ---------------------------------------------------------------------------

def load_consumer_trace(run):
    """(tow, pos_err_3d_m, up_m, n_sats) rows of <run>/logs/closed_loop_trace.csv; torn rows skipped."""
    rows = []
    with open(run + "/logs/closed_loop_trace.csv") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((float(r["tow"]), float(r["pos_err_3d_m"]), float(r["up_m"]), int(r["n_sats"])))
            except (ValueError, KeyError, TypeError):
                pass   # torn / partial row (live tail) -- skip
    return rows


def split_run_label(arg):
    """'run_dir[:label]' -> (run_dir, label).  A Windows drive-letter colon ('C:\\...') is not a separator."""
    i = arg.rfind(":")
    if i > 1 and "/" not in arg[i:] and "\\" not in arg[i:]:
        return arg[:i], arg[i + 1:]
    return arg, ""


def run_consumer(runs, bin_min=60, lo=-1e9, hi=1e9, day0=DEFAULT_DAY0):
    data = {}
    for r in runs:
        path, label = split_run_label(r)
        data[label or path] = load_consumer_trace(path)
    labels = list(data)
    bins = set()
    binned = {l: collections.defaultdict(list) for l in labels}
    for l in labels:
        for tow, e3, up, ns in data[l]:
            if tow < lo or tow > hi:
                continue
            b = int((tow - day0) // (bin_min * 60.0))
            bins.add(b)
            binned[l][b].append(e3)
    print("bin(h)   " + "  ".join(f"{l:>28s}" for l in labels))
    print("         " + "  ".join(f"{'n  rms    p50    max':>28s}" for _ in labels))
    for b in sorted(bins):
        h = b * bin_min / 60.0
        cells = []
        for l in labels:
            n, rms, p50, mxv = rms_p50_max(binned[l][b], absolute=False)
            cells.append(f"{n:4d} {rms:6.3f} {p50:6.3f} {mxv:6.2f}" if n else f"{'-':>28s}")
        print(f"{h:6.2f}   " + "  ".join(f"{c:>28s}" for c in cells))


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------

SELFTEST_T0 = 1000.0


def _selftest_wire(path, perturbed):
    """Three-epoch synthetic wire: 4 GPS + 3 GAL sats, two signals each.

    perturbed=True adds dC = +1.0 m on G02 (+1.5 at the third epoch) and
    dP = +0.19 m on G03/L1C (+0.21 at the third epoch).  Everything else is
    constant in time, so the base wire has zero steps and zero drift.
    """
    sats = [("G01", 0.0), ("G02", 0.5), ("G03", -0.3), ("G04", 0.2),
            ("E01", 0.0), ("E02", 0.4), ("E03", -0.2)]
    sigs = {"G": ("L1C", "L2W"), "E": ("L1C", "L5Q")}
    with open(path, "w", newline="\n") as f:
        f.write("# gnss_network_product v2\n# simnet_seam_tools selftest wire\n")
        f.write("S,SELFTEST,1,nominal-wu93,0,0,1,0,0.05,1,none,E:E01,G:G01\n")
        for i in range(3):
            tow = SELFTEST_T0 + 30.0 * i
            f.write(f"H,2313,{tow:.3f},ok,1.0000,7,-\n")
            f.write(f"E,2313,{tow:.3f},{1000 + i},0,0,{1 if i == 0 else 0}{',EG' if i == 0 else ''}\n")
            for k, (sat, c0) in enumerate(sats):
                dc = 0.0
                if perturbed and sat == "G02":
                    dc = 1.0 if i < 2 else 1.5
                f.write(f"C,{sat},{c0 + dc!r},sigcc=0.25\n")
                for m, sig in enumerate(sigs[sat[0]]):
                    f.write(f"B,{sat},C{sig[1:]},{0.01 * (k + 1) * (m + 1)!r}\n")
                for m, sig in enumerate(sigs[sat[0]]):
                    dp = 0.0
                    if perturbed and (sat, sig) == ("G03", "L1C"):
                        dp = 0.19 if i < 2 else 0.21
                    f.write(f"P,{sat},{sig},{1.0 + 0.1 * k + m + dp!r},1,0,sigma=0.5\n")
                if sat not in ("G01", "E01"):
                    f.write(f"U,{sat},0.3\n")
            if i == 0:
                f.write("R,G02\n")


def _selftest_consumer(run_dir, day0):
    os.makedirs(os.path.join(run_dir, "logs"), exist_ok=True)
    with open(os.path.join(run_dir, "logs", "closed_loop_trace.csv"), "w", newline="\n") as f:
        f.write("week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,"
                "pos_err_3d_m,east_m,north_m,up_m,ar_gate,p_succ\n")
        for j, e in enumerate((0.1, 0.2, 0.3, 0.4)):
            f.write(f"2313,{day0 + 30 * j:.3f},18,0,0,0,0,0.00,{e},0,0,0.1,not_run,0\n")
        for j, e in enumerate((1.0, 2.0)):
            f.write(f"2313,{day0 + 3600 + 30 * j:.3f},18,0,0,0,0,0.00,{e},0,0,0.1,not_run,0\n")
        f.write("2313,263000.000,18\n")          # torn live-tail row: must be skipped


def _capture(fn, *args, **kw):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fn(*args, **kw)
    return buf.getvalue().splitlines()


def _floats(s):
    """Every float-looking token of s (labels like 'G02:+0.50' are skipped)."""
    out = []
    for tok in s.replace("/", " ").split():
        try:
            out.append(float(tok))
        except ValueError:
            pass
    return out


def selftest():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    def near(got, want, tol, what):
        check(abs(got - want) <= tol, f"{what}: got {got!r}, want {want!r} (tol {tol})")

    lam1 = C_LIGHT / 1575.42e6
    lam2 = C_LIGHT / 1227.60e6
    t0, t1, t2 = SELFTEST_T0, SELFTEST_T0 + 30, SELFTEST_T0 + 60
    with tempfile.TemporaryDirectory() as td:
        base = os.path.join(td, "base.txt")
        pert = os.path.join(td, "pert.txt")
        _selftest_wire(base, perturbed=False)
        _selftest_wire(pert, perturbed=True)

        # ---- shared parser -------------------------------------------------
        ep = parse_wire(pert, t0 - 1, t2 + 1)
        check(sorted(ep) == [1000, 1030, 1060], f"parse: epochs {sorted(ep)}")
        e0 = ep[1000]
        check(len(e0["C"]) == 7 and len(e0["P"]) == 14 and len(e0["B"]) == 14, "parse: C/P/B counts")
        check(e0["R"] == ["G02"] and e0["datum"] == "1" and e0["dconst"] == "EG", "parse: R / datum fields")
        check(len(e0["Uv"]) == 5 and e0["U"].get("G02") == 0.3, "parse: U records")
        check(len(e0["sigcc"]) == 7 and len(e0["Psig"]) == 14, "parse: sigcc / sigma tokens")
        check(e0["H"] == ["ok", "1.0000", "7"] and ep[1060]["H"] is None, "parse: H attachment (next-epoch heartbeat)")
        near(e0["C"]["G02"], 1.5, 1e-12, "parse: G02 clock at t0")
        near(e0["P"][("G03", "L1C")], 1.2 + 0.19, 1e-12, "parse: G03/L1C bias at t0")
        near(wavelength("G02", "L1C"), lam1, 0.0, "wavelength table")

        # ---- diff ----------------------------------------------------------
        out = _capture(run_diff, pert, base, t0, t2, 1)
        check(out[2] == "# common epochs 3 (A 3, B 3)", f"diff: header {out[2]!r}")
        check(len(out) == 4 + 3 * 2, f"diff: {len(out)} lines")
        g0 = [l for l in out if l.startswith(f"{t0:8.0f}") and "| G :" in l]
        g2 = [l for l in out if l.startswith(f"{t2:8.0f}") and "| G :" in l]
        e0l = [l for l in out if l.startswith(f"{t0:8.0f}") and "| E :" in l]
        check(len(g0) == 1 and len(g2) == 1 and len(e0l) == 1, "diff: one G and one E line per epoch")
        if g0 and g2 and e0l:
            check(g0[0].startswith(f"{t0:8.0f} 1EG 1EG  1/ 1 |"), f"diff: datum/R columns {g0[0][:26]!r}")
            check(g0[0].endswith("  RA=G02"), "diff: RA suffix on the R-marked epoch")
            c0 = _floats(g0[0].split("|")[1].split(":")[1])
            p0 = _floats(g0[0].split("|")[2])
            check(c0[0] == 4, "diff: G clock n")
            near(c0[1], 0.0, 1e-9, "diff: G clock cm")
            near(c0[2], math.sqrt(1.0 / 3), 1e-4, "diff: G clock sd at t0")
            near(c0[3], 1.0, 1e-4, "diff: G clock max at t0")
            near(p0[2], math.sqrt(0.19 ** 2 / 3), 1e-4, "diff: G P(L1C) sd at t0")
            near(p0[3], 0.19, 1e-4, "diff: G P(L1C) max at t0")
            c2 = _floats(g2[0].split("|")[1].split(":")[1])
            p2 = _floats(g2[0].split("|")[2])
            near(c2[2], math.sqrt(1.5 ** 2 / 3), 1e-4, "diff: G clock sd at t2")
            near(c2[3], 1.5, 1e-4, "diff: G clock max at t2")
            near(p2[3], 0.21, 1e-4, "diff: G P(L1C) max at t2")
            ce = _floats(e0l[0].split("|")[1].split(":")[1])
            check(ce[0] == 3 and ce[2] == 0.0 and ce[3] == 0.0, "diff: E clock untouched")

        # ---- decompose -----------------------------------------------------
        out = _capture(run_decompose, pert, base, t0)
        check(out[0].startswith(f"epoch {t0:.0f}:"), "decompose: header")
        r02 = [l for l in out if l.startswith("G02 L1C |")]
        r02b = [l for l in out if l.startswith("G02 L2W |")]
        r03 = [l for l in out if l.startswith("G03 L1C |")]
        check(len(r02) == 1 and len(r02b) == 1 and len(r03) == 1, "decompose: expected rows present")
        check(sum(1 for l in out if " | " in l and not l.startswith("epoch")) == 14, "decompose: 14 key rows")
        if r02 and r02b and r03:
            v = _floats(r02[0][len("G02 L1C |"):])
            near(v[0], 1.0, 1e-3, "decompose: G02 dC")
            near(v[1], 0.0, 1e-4, "decompose: G02 dB")
            near(v[2], 0.0, 1e-3, "decompose: G02 dP")
            near(v[3], -1.0, 1e-3, "decompose: G02 code-side")
            near(v[4], -1.0, 1e-3, "decompose: G02 phase-side")
            near(v[5], -1.0 / lam1, 1e-3, "decompose: G02 ps/lam")
            near(v[6], -0.2550, 1e-3, "decompose: G02 L1C frac (wrapped)")
            near(v[7], 0.3, 1e-3, "decompose: G02 URA_A")
            near(v[8], 0.3, 1e-3, "decompose: G02 URA_B")
            near(_floats(r02b[0][len("G02 L2W |"):])[6], -0.0948, 1e-3, "decompose: G02 L2W frac (wrapped)")
            w = _floats(r03[0][len("G03 L1C |"):])
            near(w[2], 0.19, 1e-3, "decompose: G03 dP")
            near(w[4], 0.19, 1e-3, "decompose: G03 phase-side")
            near(w[6], -0.0015, 1e-3, "decompose: G03 L1C frac")
        gsum = [l for l in out if l.startswith("G: n=8 ")]
        check(gsum == ["G: n=8 |frac| p50=0.000 p90=0.255 max=0.255 cyc ; |code-side| p50=0.000 p90=1.000 max=1.000 m"],
              f"decompose: G summary {gsum!r}")
        check(out[-1].startswith("largest |dC|: G02:+1.00 "), f"decompose: largest |dC| {out[-1][:30]!r}")

        # ---- walk ----------------------------------------------------------
        out = _capture(run_walk, pert, base, t0, t2, 1)
        check(len(out) == 1 + 3 * 2, f"walk: {len(out)} lines")
        g2 = [l for l in out if l.startswith(f"{t2:8.0f}") and "| G |" in l]
        g0 = [l for l in out if l.startswith(f"{t0:8.0f}") and "| G |" in l]
        check(len(g2) == 1 and len(g0) == 1, "walk: G lines at t0 / t2")
        if g2 and g0:
            parts = g2[0].split("|")
            c = _floats(parts[2])
            p = _floats(parts[3])
            check(c[0] == 4 and p[0] == 8, "walk: n counts")
            near(c[1], 0.0, 1e-9, "walk: clock cm at t2")
            near(c[2], math.sqrt(0.25 / 3), 1e-3, "walk: clock sd at t2")
            near(c[3], 0.5, 1e-3, "walk: clock max at t2")
            near(p[2], math.sqrt((0.25 + 0.25 + 0.02 ** 2) / 7), 1e-3, "walk: phase-side sd at t2")
            near(p[3], 0.5, 1e-3, "walk: phase-side max at t2")
            check(parts[4].strip().startswith("G02:+0.50"), f"walk: worst sat {parts[4].strip()[:12]!r}")
            check(all(x == 0.0 for x in _floats(g0[0].split("|")[2])[1:]), "walk: zero walk at t0")

        # ---- quality -------------------------------------------------------
        out = _capture(run_quality, pert, base, t0, t2, 1)
        check(len(out) == 4, f"quality: {len(out)} lines")
        q0 = out[1] if len(out) > 1 else ""
        q2 = out[3] if len(out) > 3 else ""
        check(q0.startswith(f"{t0:8.0f}  0.00 |  0.300/ 0.300  0.500/ 0.500  0.250  ok |"), f"quality: A columns {q0[:60]!r}")
        check(q2.split("|")[1].strip().endswith("-"), "quality: last epoch has no following heartbeat")
        if q2:
            w = _floats(q2.split("|")[3])
            near(w[0], 0.0, 1e-9, "quality: E sub-cycle walk sd")
            near(w[1], 0.13786, 1e-3, "quality: G sub-cycle walk sd at t2")
            check(q2.split("|")[4].strip() == "0/ 6  2/ 8", f"quality: n>0.1cyc {q2.split('|')[4].strip()!r}")

        # ---- steps ---------------------------------------------------------
        out = _capture(run_steps, pert, t0, t2, 1)
        check(len(out) == 3, f"steps: {len(out)} lines")
        if len(out) == 3:
            s1 = _floats(out[1])
            s2 = _floats(out[2])
            check(s1[0] == t1 and s1[2] == 14 and all(x == 0.0 for x in s1[3:]), "steps: zero step t0->t1")
            check(s2[0] == t2, "steps: t2 row")
            near(s2[1], 60 / 3600, 1e-3, "steps: dt(h) from t_lo")
            check(s2[2] == 14, "steps: n keys")
            near(s2[3], math.sqrt((0.25 + 0.25 + 0.02 ** 2) / 14), 1e-4, "steps: rms")
            near(s2[4], 0.0, 1e-4, "steps: p50")
            near(s2[5], 0.5, 1e-3, "steps: max")
            check(s2[6] == 2 and s2[7] == 2, "steps: n>0.05m / n>0.5cyc")
            near(s2[8], math.sqrt(0.25 / 7), 1e-4, "steps: clock rms")
            near(s2[9], 0.5, 1e-3, "steps: clock max")
            near(s2[10], math.sqrt(0.02 ** 2 / 14), 1e-4, "steps: PB rms")
            near(s2[11], 0.02, 1e-3, "steps: PB max")
            near(s2[12], 0.10421, 1e-3, "steps: sub-cycle rms")
        out_ref = _capture(run_steps, pert, t0, t2, 1, t_ref=t0 - 3600)
        if len(out_ref) == 3:
            near(_floats(out_ref[2])[1], 1 + 60 / 3600, 1e-3, "steps: --t-ref honoured")

        # ---- drift ---------------------------------------------------------
        out = _capture(run_drift, pert, t0, t2)
        check(len(out) == 3, f"drift: {len(out)} lines")
        eline = [l for l in out if l.startswith("E: ")]
        gline = [l for l in out if l.startswith("G: ")]
        check(eline == ["E: n=6 subcycle drift mean=+0.0000 sd=0.0000 p50|.|=0.0000 max|.|=0.0000 m ; integer-class |D|>0.5cyc: 0"],
              f"drift: E line {eline!r}")
        if gline:
            kv = dict(re.findall(r"([A-Za-z0-9|.]+)=([-+0-9.]+)", gline[0]))
            n_int = re.search(r"0\.5cyc: (\d+)$", gline[0])
            # sub-cycle drifts: G02/L1C wraps to +0.3725 cyc, G02/L2W to -0.0474 cyc, G03/L1C +0.02 m
            subs = [0.3724823 * lam1, -0.0474164 * lam2, 0.02, 0, 0, 0, 0, 0]
            mean = sum(subs) / 8
            check(kv.get("n") == "8", f"drift: G n {kv.get('n')!r}")
            near(float(kv.get("mean", "nan")), mean, 1e-4, "drift: G mean")
            near(float(kv.get("sd", "nan")), math.sqrt(sum((x - mean) ** 2 for x in subs) / 7), 1e-4, "drift: G sd")
            near(float(kv.get("p50|.|", "nan")), 0.0, 1e-4, "drift: G p50")
            near(float(kv.get("max|.|", "nan")), subs[0], 1e-4, "drift: G max")
            check(n_int is not None and n_int.group(1) == "2", "drift: G integer-class count")
        check(out[-1].startswith("largest sub-cycle drifts: G02/L1C:+0.071 G03/L1C:+0.020 G02/L2W:-0.012"),
              f"drift: largest list {out[-1][:70]!r}")

        # ---- consumer ------------------------------------------------------
        run_dir = os.path.join(td, "run")
        _selftest_consumer(run_dir, DEFAULT_DAY0)
        check(split_run_label("runs/x:warm") == ("runs/x", "warm"), "consumer: relative run:label split")
        check(split_run_label(r"C:\t\run") == (r"C:\t\run", ""), "consumer: drive-letter colon is not a label")
        check(split_run_label(r"C:\t\run:b") == (r"C:\t\run", "b"), "consumer: absolute run:label split")
        out = _capture(run_consumer, [run_dir + ":a", run_dir + ":b"])
        check(len(out) == 4, f"consumer: {len(out)} lines")
        if len(out) == 4:
            check(out[0].split() == ["bin(h)", "a", "b"], f"consumer: labels {out[0]!r}")
            h0 = _floats(out[2])
            h1 = _floats(out[3])
            check(h0[0] == 0.0 and h0[1] == 4 and h0[5] == 4, "consumer: hour-0 n (torn row skipped)")
            near(h0[2], math.sqrt((0.01 + 0.04 + 0.09 + 0.16) / 4), 1e-3, "consumer: hour-0 rms")
            near(h0[3], 0.3, 1e-3, "consumer: hour-0 p50")
            near(h0[4], 0.4, 1e-2, "consumer: hour-0 max")
            check(h1[0] == 1.0 and h1[1] == 2, "consumer: hour-1 n")
            near(h1[2], math.sqrt(2.5), 1e-3, "consumer: hour-1 rms")
            near(h1[3], 2.0, 1e-3, "consumer: hour-1 p50")
        out = _capture(run_consumer, [run_dir + ":a"], bin_min=30, hi=DEFAULT_DAY0 + 3600)
        check(len(out) == 4 and _floats(out[3])[:2] == [1.0, 1], f"consumer: --bin-min/--tow-hi {out[-1]!r}")

    if fails:
        print("selftest FAIL")
        for f in fails:
            print("  -", f)
        return 1
    print("selftest PASS (parser, diff, decompose, walk, quality, steps, drift, consumer)")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    ap = argparse.ArgumentParser(
        prog="simnet_seam_tools.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true",
                    help="build a synthetic wire pair in a temp dir and check every subcommand")
    sub = ap.add_subparsers(dest="cmd", metavar="subcommand")

    p = sub.add_parser("diff", help="per-epoch realization difference A-B per channel/constellation")
    p.add_argument("A"); p.add_argument("B")
    p.add_argument("tow_lo", type=float); p.add_argument("tow_hi", type=float)
    p.add_argument("every", nargs="?", type=int, default=1)

    p = sub.add_parser("decompose", help="one-epoch per-(sat,signal) dC/dB/dP split of A-B")
    p.add_argument("A"); p.add_argument("B"); p.add_argument("tow", type=float)

    p = sub.add_parser("walk", help="post-seam walk of A-B relative to t0")
    p.add_argument("A"); p.add_argument("B")
    p.add_argument("t0", type=float); p.add_argument("t_hi", type=float)
    p.add_argument("every", nargs="?", type=int, default=40)

    p = sub.add_parser("quality", help="wire-quality series on both wires + sub-cycle phase-side walk")
    p.add_argument("A"); p.add_argument("B")
    p.add_argument("t0", type=float); p.add_argument("t_hi", type=float)
    p.add_argument("every", nargs="?", type=int, default=40)

    p = sub.add_parser("steps", help="epoch-to-epoch phase-side step on one wire")
    p.add_argument("wire"); p.add_argument("t_lo", type=float); p.add_argument("t_hi", type=float)
    p.add_argument("every", nargs="?", type=int, default=1)
    p.add_argument("--t-ref", type=float, default=None, help="origin of the dt(h) column (default: t_lo)")

    p = sub.add_parser("drift", help="total sub-cycle phase-side drift on one wire between two epochs")
    p.add_argument("wire"); p.add_argument("t_a", type=float); p.add_argument("t_b", type=float)

    p = sub.add_parser("consumer", help="hourly consumer 3-D truth-error table from closed_loop_trace.csv")
    p.add_argument("runs", nargs="+", metavar="run_dir[:label]")
    p.add_argument("--bin-min", type=float, default=60)
    p.add_argument("--tow-lo", type=float, default=-1e9)
    p.add_argument("--tow-hi", type=float, default=1e9)
    p.add_argument("--day0", type=float, default=DEFAULT_DAY0,
                   help="tow of 00:00 on the product day (default 259200)")

    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.cmd == "diff":
        run_diff(args.A, args.B, args.tow_lo, args.tow_hi, args.every)
    elif args.cmd == "decompose":
        run_decompose(args.A, args.B, args.tow)
    elif args.cmd == "walk":
        run_walk(args.A, args.B, args.t0, args.t_hi, args.every)
    elif args.cmd == "quality":
        run_quality(args.A, args.B, args.t0, args.t_hi, args.every)
    elif args.cmd == "steps":
        run_steps(args.wire, args.t_lo, args.t_hi, args.every, args.t_ref)
    elif args.cmd == "drift":
        run_drift(args.wire, args.t_a, args.t_b)
    elif args.cmd == "consumer":
        run_consumer(args.runs, args.bin_min, args.tow_lo, args.tow_hi, args.day0)
    else:
        ap.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
