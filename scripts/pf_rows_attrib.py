#!/usr/bin/env python3
"""SPEC_event_harness §24.6: per-LOS code-residual attribution, read from the
producer's own signed post-fit residual rows (`network.trace_postfit_rows` ->
logs/network_pf_rows.csv: week,tow,station,sat,signal,kind,resid_m,w,elev_deg).

    python scripts/pf_rows_attrib.py <legdir> [--k-lo 10 --k-hi 60] [--tow0 277320]
                                     [--seed-run runs/evseed_field6h_v8] [--json out.json]

Window: k = 1 + (tow - tow0)/30, kept for k in [k_lo, k_hi] (the settled part of a
60-epoch leg). Every read below runs on the code rows and is repeated on the phase
rows (the phase is the control: centimeter-scale, and the LOS whose phase shares a
code signature names geometry or troposphere, the last row of the §24.6 table).

Items (numbered as in the SPEC):
  1. per (station, satellite, signal): residual mean over the window (a constant no
     state absorbed) and rms about the mean; pooled distributions and the share of
     the pooled sum of squares the per-LOS means carry.
  2. per station: rms(C2W)/rms(C1C) and rms(C5Q|C5X)/rms(C1C) of the code residuals
     (dispersive gives (f1/f2)^2 = 1.647, (f1/f5)^2 = 1.793; non-dispersive gives 1)
     and the sign agreement per LOS (Pearson r of the C1C and C2W series of one LOS,
     matched by epoch), plus the cross-signal relation of the per-LOS means.
  3. per station: code rms in elevation bins 15-25 / 25-45 / >45 deg (1/sin(el)
     predicts a low/high ratio of about 2.5; a flat error predicts about 1).
  4. per satellite: mean over stations of the per-LOS code mean (a satellite-side
     constant) vs its spread across stations (a station-side one).
  5. the seven stations the frame-blind rx-CB gate refuses (§21.18) vs the rest.
  6. a variance decomposition of the pooled code residual on orthogonal projections
     (every table closes exactly): per-(station,signal) constants, per-(satellite,
     signal) constants, per-(station,epoch) and per-(satellite,epoch) common modes
     (clock-like), then per LOS-epoch the signal-common mean vs the between-signal
     part, the latter split into the gamma-direction (the ionosphere's pattern,
     gamma_b = (f1/fb)^2) and its orthogonal remainder. Reported both marginally
     (each alone on the raw residual, e.g. bar A2's ">70% in one row") and nested
     (what is left for each after the rows before it). With only two signals, any
     between-signal difference is "gamma-direction" by construction, so the
     dispersive verdict comes from the shape tests: (a) between/common energy
     against the 1/(amp-1) ≈ 0.057 a pure dispersive error carries; (b) the
     3-signal groups' gamma-direction share of their between-signal energy (about
     1 for dispersive, about 0.5 for signal-specific); (c) the sign relation of the
     two signals (dispersive +1.647, signal-common +1, and the anti-correlated
     -w1/(w2 g2) that the per-LOS iono column's normal equation forces when the
     code rows alone close it, since free ambiguities carry no iono level
     information; the closure ratio c = |sum w g v| / sqrt(sum (w g v)^2) reads
     that directly). The implied dispersive share is noise-corrected with the
     3-signal groups' empirical sigma_hat and enters bar A2 only when the shape
     tests admit it (otherwise it is reported as shape-refuted); the last named
     row is the per-LOS residual left after every state-side row.
  7. per-station code rms of the leg vs the seed's network_pf_keyed.csv STA ranking
     (bar A1: Spearman > 0.9), on the same statistic (rms of |v|*sqrt(w), the
     pf_keyed convention) at the same epochs and over the seed's settled hours 2-5;
     plus a self-consistency check of the dump against the leg's own pf_keyed STA
     rows.

Conventions taken from the code (FrameDiag.cpp): resid_m = v = y - A_s x_s -
A_r x_r (observed minus predicted, metres, not standardized); the pass pools
|v|*sqrt(w).
"""
import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REFUSED7 = ["KOKV00USA", "CPVG00CPV", "MRO100AUS", "LMMF00MTQ", "GOPE00CZE",
            "GAMB00PYF", "LCK400IND"]

# Carrier frequencies (MHz) by (constellation, RINEX band digit).  The dispersive
# factor of band b is (f_ref / f_b)^2 with f_ref the constellation's band-1 carrier
# (GPS L1 / Galileo E1 / BeiDou B1C / QZSS L1).  GLONASS is FDMA: excluded.
FREQ_MHZ = {
    ("G", "1"): 1575.42, ("G", "2"): 1227.60, ("G", "5"): 1176.45,
    ("E", "1"): 1575.42, ("E", "5"): 1176.45, ("E", "7"): 1207.14,
    ("E", "8"): 1191.795, ("E", "6"): 1278.75,
    ("C", "1"): 1575.42, ("C", "2"): 1561.098, ("C", "5"): 1176.45,
    ("C", "7"): 1207.14, ("C", "6"): 1268.52,
    ("J", "1"): 1575.42, ("J", "2"): 1227.60, ("J", "5"): 1176.45,
}
ELEV_BINS = [(15.0, 25.0), (25.0, 45.0), (45.0, 90.1)]
ELEV_BIN_NAMES = ["15-25", "25-45", ">45"]


def disp_factor(sat, signal):
    """(f_ref/f_b)^2 for the row's band, or None when unknown (GLONASS, odd codes)."""
    if not sat or len(signal) < 2:
        return None
    c = sat[0]
    f1 = FREQ_MHZ.get((c, "1"))
    fb = FREQ_MHZ.get((c, signal[1]))
    if f1 is None or fb is None:
        return None
    return (f1 / fb) ** 2


# --------------------------------------------------------------------------- I/O
def read_rows(path):
    cols = defaultdict(list)
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(fh)
        hdr = next(rd)
        idx = {h: i for i, h in enumerate(hdr)}
        for c in ("tow", "station", "sat", "signal", "kind", "resid_m", "w", "elev_deg"):
            if c not in idx:
                raise SystemExit(f"FATAL: {path} has no column {c!r} (header {hdr})")
        n_bad = 0
        for r in rd:
            if len(r) < len(hdr):
                n_bad += 1
                continue
            try:
                cols["tow"].append(float(r[idx["tow"]]))
                cols["res"].append(float(r[idx["resid_m"]]))
                cols["w"].append(float(r[idx["w"]]))
            except ValueError:
                n_bad += 1
                continue
            cols["sta"].append(r[idx["station"]])
            cols["sat"].append(r[idx["sat"]])
            cols["sig"].append(r[idx["signal"]])
            cols["kind"].append(r[idx["kind"]])
            e = r[idx["elev_deg"]].strip()
            cols["el"].append(float(e) if e else np.nan)
    d = {k: np.array(v) for k, v in cols.items()}
    d["n_bad"] = n_bad
    return d


def read_iono_elev(path):
    """(round(tow,3), station, sat) -> elevation_deg from network_ionosphere.csv."""
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(fh)
        hdr = next(rd)
        idx = {h: i for i, h in enumerate(hdr)}
        if "elevation_deg" not in idx:
            return out
        for r in rd:
            try:
                out[(round(float(r[idx["tow"]]), 3), r[idx["station"]], r[idx["sat"]])] = \
                    float(r[idx["elevation_deg"]])
            except (ValueError, IndexError):
                continue
    return out


def read_pf_keyed_sta(path):
    """{(tow, station): (n_code, rms_code, n_phase, rms_phase)} from network_pf_keyed.csv."""
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(fh)
        hdr = next(rd)
        idx = {h: i for i, h in enumerate(hdr)}
        for r in rd:
            if r[idx["kind"]] != "STA":
                continue
            try:
                out[(float(r[idx["tow"]]), r[idx["key"]])] = (
                    int(r[idx["n_code"]]), float(r[idx["rms_code"]]),
                    int(r[idx["n_phase"]]), float(r[idx["rms_phase"]]))
            except ValueError:
                continue
    return out


# --------------------------------------------------------------------------- helpers
def sub(d, mask):
    return {k: (v[mask] if isinstance(v, np.ndarray) else v) for k, v in d.items()}


def group(*keys):
    g = defaultdict(list)
    for i, key in enumerate(zip(*keys)):
        g[key].append(i)
    return {k: np.asarray(v) for k, v in g.items()}


def rms(v):
    v = np.asarray(v, dtype=float)
    return float(np.sqrt(np.mean(v * v))) if v.size else float("nan")


def pct(x, q):
    x = np.asarray(x, dtype=float)
    return float(np.percentile(x, q)) if x.size else float("nan")


def pearson(a, b):
    a = np.asarray(a, dtype=float)
    b = np.asarray(b, dtype=float)
    if a.size < 3:
        return float("nan")
    a = a - a.mean()
    b = b - b.mean()
    den = math.sqrt(float(a @ a) * float(b @ b))
    return float(a @ b) / den if den > 0 else float("nan")


def rankdata(x):
    x = np.asarray(x, dtype=float)
    order = np.argsort(x, kind="mergesort")
    ranks = np.empty(x.size, dtype=float)
    ranks[order] = np.arange(1, x.size + 1, dtype=float)
    # average ties
    xs = x[order]
    i = 0
    while i < xs.size:
        j = i
        while j + 1 < xs.size and xs[j + 1] == xs[i]:
            j += 1
        if j > i:
            ranks[order[i:j + 1]] = 0.5 * (i + 1 + j + 1)
        i = j + 1
    return ranks


def spearman(a, b):
    return pearson(rankdata(a), rankdata(b))


def fmt(x, nd=3, width=8):
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return " " * (width - 1) + "-"
    return f"{x:{width}.{nd}f}"


def hr(title):
    print()
    print("=" * 100)
    print(title)
    print("=" * 100)


# --------------------------------------------------------------------------- items
def item1_los(d, kind, out):
    """Per-LOS mean / rms about mean; pooled distributions; the share the means carry."""
    G = group(d["sta"], d["sat"], d["sig"])
    n_los = len(G)
    means, sds, ns = [], [], []
    ss_tot = float(np.sum(d["res"] ** 2))
    ss_mean = 0.0
    top = []
    for (sta, sat, sig), ix in G.items():
        v = d["res"][ix]
        m = float(v.mean())
        s = float(v.std())
        means.append(m)
        sds.append(s)
        ns.append(v.size)
        ss_mean += v.size * m * m
        top.append((abs(m), sta, sat, sig, m, s, v.size))
    means = np.array(means)
    sds = np.array(sds)
    top.sort(reverse=True)
    print(f"[{kind}] {n_los} LOS-signal series, {d['res'].size} rows; pooled rms {rms(d['res']):.4f} m")
    print(f"  per-LOS |mean| : p50 {pct(np.abs(means), 50):.4f}  p90 {pct(np.abs(means), 90):.4f}  "
          f"max {np.abs(means).max():.4f} m")
    print(f"  per-LOS rms about its mean : p50 {pct(sds, 50):.4f}  p90 {pct(sds, 90):.4f}  "
          f"max {sds.max():.4f} m")
    print(f"  share of the pooled sum of squares carried by the per-LOS MEANS : "
          f"{100.0 * ss_mean / ss_tot if ss_tot > 0 else float('nan'):.1f} %  "
          f"(rms of the means-removed residual {math.sqrt(max(ss_tot - ss_mean, 0.0) / d['res'].size):.4f} m)")
    # per signal
    print(f"  {'signal':7s} {'n':>7s} {'mean':>9s} {'rms_raw':>9s} {'rms_about_LOS_means':>20s}")
    per_sig = {}
    for sig, ix in sorted(group(d["sig"]).items()):
        v = d["res"][ix]
        Gs = group(d["sta"][ix], d["sat"][ix])
        ss_about = sum(float(np.sum((v[j] - v[j].mean()) ** 2)) for j in Gs.values())
        per_sig[sig[0]] = dict(n=int(v.size), mean=float(v.mean()), rms=rms(v),
                               rms_about_los_means=math.sqrt(ss_about / v.size))
        print(f"  {sig[0]:7s} {v.size:7d} {v.mean():9.4f} {rms(v):9.4f} "
              f"{math.sqrt(ss_about / v.size):20.4f}")
    print("  top-8 LOS by |mean|:")
    for _, sta, sat, sig, m, s, n in top[:8]:
        print(f"    {sta} {sat} {sig}: mean {m:8.3f} m  rms about mean {s:7.3f} m  n {n}")
    # Level persistence: the per-LOS mean over the first half of the window vs the
    # second half.  A fixed bias (code bias or datum constant) reproduces itself
    # (r ~ 1, slope ~ 1); code multipath at its 10-20 min timescale decorrelates.
    tmid = float(np.median(np.unique(d["tow"])))
    a_, b_, wts = [], [], []
    for (sta, sat, sig), ix in G.items():
        h1 = ix[d["tow"][ix] < tmid]
        h2 = ix[d["tow"][ix] >= tmid]
        if h1.size >= 8 and h2.size >= 8:
            a_.append(float(d["res"][h1].mean()))
            b_.append(float(d["res"][h2].mean()))
    a_ = np.array(a_)
    b_ = np.array(b_)
    r_half = pearson(a_, b_)
    slope_half = float(np.sum(a_ * b_) / np.sum(a_ * a_)) if a_.size and np.sum(a_ * a_) > 0 else float("nan")
    rms_diff = rms(b_ - a_) if a_.size else float("nan")
    print(f"  level PERSISTENCE (per-LOS mean, first half vs second half of the window, {a_.size} LOS-signal "
          f"series with >= 8 epochs in each): r {r_half:.3f}, slope {slope_half:.3f}, rms(second - first) "
          f"{rms_diff:.3f} m vs rms(level) {rms(a_):.3f} m  (fixed bias => r ~ 1, slope ~ 1; multipath => lower)")
    # |level| vs 1/sin(el) across LOS: the elevation shape of the constant part.
    el = d["el"]
    xs, ys = [], []
    for (sta, sat, sig), ix in G.items():
        e = el[ix]
        e = e[np.isfinite(e)]
        if e.size >= 5:
            xs.append(1.0 / math.sin(math.radians(float(np.mean(e)))))
            ys.append(abs(float(d["res"][ix].mean())))
    xs = np.array(xs)
    ys = np.array(ys)
    r_el = pearson(xs, ys) if xs.size > 3 else float("nan")
    lo = ys[xs >= 1.0 / math.sin(math.radians(25.0))]
    hi = ys[xs <= 1.0 / math.sin(math.radians(45.0))]
    print(f"  |level| vs 1/sin(mean el) across {xs.size} LOS-signal series: r {r_el:.3f}; median |level| at el < 25 deg "
          f"{pct(lo, 50):.3f} m (n {lo.size}) vs el > 45 deg {pct(hi, 50):.3f} m (n {hi.size}), ratio "
          f"{(pct(lo, 50) / pct(hi, 50)) if hi.size and pct(hi, 50) > 0 else float('nan'):.2f}")
    out[f"item1_{kind}_persistence"] = dict(n=int(a_.size), r=r_half, slope=slope_half, rms_diff=rms_diff,
                                            rms_level=rms(a_) if a_.size else None,
                                            level_vs_invsin_r=r_el, level_lo_med=pct(lo, 50), level_hi_med=pct(hi, 50))
    out[f"item1_{kind}"] = dict(
        n_los=n_los, n_rows=int(d["res"].size), pooled_rms=rms(d["res"]),
        abs_mean_p50=pct(np.abs(means), 50), abs_mean_p90=pct(np.abs(means), 90),
        sd_p50=pct(sds, 50), sd_p90=pct(sds, 90),
        share_means_pct=100.0 * ss_mean / ss_tot if ss_tot > 0 else None,
        per_signal=per_sig,
        top8=[dict(station=t[1], sat=t[2], signal=t[3], mean=t[4], sd=t[5], n=int(t[6]))
              for t in top[:8]])
    return G


def item2_ratios(d, kind, out):
    """Per-station cross-signal rms ratios and per-LOS sign agreement."""
    b1 = "L1C" if kind == "phase" else "C1C"
    b2 = "L2W" if kind == "phase" else "C2W"
    b5 = ("L5Q", "L5X") if kind == "phase" else ("C5Q", "C5X")
    stations = sorted(set(d["sta"].tolist()))
    print(f"[{kind}] per station: rms per signal (m), ratios to {b1}, and the per-LOS "
          f"Pearson r of the {b1} and {b2} residual series (matched by epoch)")
    print(f"  {'station':10s} {'rms_'+b1:>8s} {'rms_'+b2:>8s} {'rms_5':>8s} {'r2/1':>6s} {'r5/1':>6s} "
          f"{'medLOSr':>8s} {'nLOS':>5s} {'refused':>7s}")
    rows = {}
    ratios2, ratios5, los_r_all = [], [], []
    mean_pairs = []   # (mean C1C, mean C2W) per LOS
    for sta in stations:
        ix = np.nonzero(d["sta"] == sta)[0]
        by_sig = group(d["sig"][ix])
        r = {s[0]: rms(d["res"][ix[j]]) for s, j in by_sig.items()}
        r1 = r.get(b1)
        r2 = r.get(b2)
        r5 = None
        for s5 in b5:
            if s5 in r:
                r5 = r[s5]
                break
        q2 = (r2 / r1) if (r1 and r2 and r1 > 0) else None
        q5 = (r5 / r1) if (r1 and r5 and r1 > 0) else None
        # per-LOS series correlation
        corr = []
        by_sat = group(d["sat"][ix])
        for sat, j in by_sat.items():
            jj = ix[j]
            m1 = jj[d["sig"][jj] == b1]
            m2 = jj[d["sig"][jj] == b2]
            if m1.size < 10 or m2.size < 10:
                continue
            t1 = {round(t, 3): v for t, v in zip(d["tow"][m1], d["res"][m1])}
            t2 = {round(t, 3): v for t, v in zip(d["tow"][m2], d["res"][m2])}
            common = sorted(set(t1) & set(t2))
            if len(common) < 10:
                continue
            a = np.array([t1[t] for t in common])
            b = np.array([t2[t] for t in common])
            corr.append(pearson(a, b))
            mean_pairs.append((a.mean(), b.mean()))
        med_r = float(np.median(corr)) if corr else float("nan")
        los_r_all.extend(corr)
        if q2 is not None:
            ratios2.append(q2)
        if q5 is not None:
            ratios5.append(q5)
        rows[sta] = dict(rms1=r1, rms2=r2, rms5=r5, ratio2=q2, ratio5=q5, med_los_r=med_r,
                         n_los=len(corr))
        print(f"  {sta:10s} {fmt(r1, 3)} {fmt(r2, 3)} {fmt(r5, 3)} {fmt(q2, 2, 6)} {fmt(q5, 2, 6)} "
              f"{fmt(med_r, 2, 8)} {len(corr):5d} {'YES' if sta in REFUSED7 else '':>7s}")
    los_r_all = np.array(los_r_all, dtype=float)
    mp = np.array(mean_pairs, dtype=float) if mean_pairs else np.zeros((0, 2))
    slope = float(np.sum(mp[:, 0] * mp[:, 1]) / np.sum(mp[:, 0] ** 2)) if mp.size and np.sum(mp[:, 0] ** 2) > 0 else float("nan")
    ratio_of_mean_rms = float(rms(mp[:, 1]) / rms(mp[:, 0])) if mp.size and rms(mp[:, 0]) > 0 else float("nan")
    r_means = pearson(mp[:, 0], mp[:, 1]) if mp.size else float("nan")
    print(f"  pooled: rms({b2})/rms({b1}) median {pct(ratios2, 50):.3f} (IQR {pct(ratios2, 25):.3f}-{pct(ratios2, 75):.3f}, n {len(ratios2)}); "
          f"rms(5)/rms({b1}) median {pct(ratios5, 50):.3f} (n {len(ratios5)})")
    print(f"          dispersive prediction 1.647 / 1.793; non-dispersive 1.000")
    frac_pos = float(np.mean(los_r_all > 0.5)) if los_r_all.size else float("nan")
    print(f"  per-LOS series r({b1},{b2}): median {pct(los_r_all, 50):.3f}, p10 {pct(los_r_all, 10):.3f}, "
          f"p90 {pct(los_r_all, 90):.3f}; fraction > 0.5: {100 * frac_pos:.1f} % of {los_r_all.size} LOS")
    print(f"  per-LOS window MEANS, {b2} vs {b1}: r {r_means:.3f}, regression slope through origin "
          f"{slope:.3f}, rms ratio {ratio_of_mean_rms:.3f}  (dispersive 1.647; non-dispersive 1.000; "
          f"independent noise -> r ~ 0)")
    out[f"item2_{kind}"] = dict(per_station=rows, ratio2_median=pct(ratios2, 50),
                                ratio5_median=pct(ratios5, 50), los_r_median=pct(los_r_all, 50),
                                los_r_frac_gt_0p5=frac_pos, n_los=int(los_r_all.size),
                                means_r=r_means, means_slope=slope, means_rms_ratio=ratio_of_mean_rms)


def item3_elevation(d, kind, out):
    """Per-station rms by elevation bin; pooled per signal."""
    el = d["el"]
    have = np.isfinite(el)
    print(f"[{kind}] rows with elevation: {int(have.sum())} of {el.size} "
          f"({100.0 * have.mean() if el.size else 0:.1f} %)")
    stations = sorted(set(d["sta"].tolist()))
    print(f"  {'station':10s} " + " ".join(f"{'rms ' + b:>10s} {'n':>5s}" for b in ELEV_BIN_NAMES)
          + f" {'low/high':>9s}")
    per_sta = {}
    for sta in stations:
        ix = np.nonzero((d["sta"] == sta) & have)[0]
        vals = []
        line = f"  {sta:10s} "
        for lo, hi in ELEV_BINS:
            j = ix[(el[ix] >= lo) & (el[ix] < hi)]
            r = rms(d["res"][j]) if j.size else float("nan")
            vals.append((r, int(j.size)))
            line += f" {fmt(r, 3, 10)} {j.size:5d}"
        lh = vals[0][0] / vals[2][0] if (vals[0][1] >= 20 and vals[2][1] >= 20 and vals[2][0] > 0) else float("nan")
        line += f" {fmt(lh, 2, 9)}"
        print(line)
        per_sta[sta] = dict(bins=[dict(name=n, rms=v[0], n=v[1]) for n, v in zip(ELEV_BIN_NAMES, vals)],
                            low_over_high=lh)
    print("  pooled per signal:")
    pooled = {}
    for sig, ix in sorted(group(d["sig"]).items()):
        ix = ix[have[ix]]
        vals = []
        line = f"  {sig[0]:10s} "
        for lo, hi in ELEV_BINS:
            j = ix[(el[ix] >= lo) & (el[ix] < hi)]
            r = rms(d["res"][j]) if j.size else float("nan")
            vals.append((r, int(j.size)))
            line += f" {fmt(r, 3, 10)} {j.size:5d}"
        lh = vals[0][0] / vals[2][0] if (vals[0][1] and vals[2][1] and vals[2][0] > 0) else float("nan")
        line += f" {fmt(lh, 2, 9)}"
        print(line)
        pooled[sig[0]] = dict(bins=[dict(name=n, rms=v[0], n=v[1]) for n, v in zip(ELEV_BIN_NAMES, vals)],
                              low_over_high=lh)
    # the 1/sin(el) prediction at the bin centroids of the data
    cen = []
    for lo, hi in ELEV_BINS:
        j = np.nonzero(have & (el >= lo) & (el < hi))[0]
        cen.append(float(np.mean(el[j])) if j.size else float("nan"))
    if all(math.isfinite(c) for c in cen):
        pred = math.sin(math.radians(cen[2])) / math.sin(math.radians(cen[0]))
        print(f"  1/sin(el) prediction for low/high at the data's bin centroids "
              f"({cen[0]:.1f} / {cen[2]:.1f} deg): {pred:.2f}; flat: 1.00")
    else:
        pred = float("nan")
    lh_all = [v["low_over_high"] for v in per_sta.values() if math.isfinite(v["low_over_high"])]
    print(f"  per-station low/high median {pct(lh_all, 50):.2f} (IQR {pct(lh_all, 25):.2f}-{pct(lh_all, 75):.2f}, n {len(lh_all)})")
    out[f"item3_{kind}"] = dict(per_station=per_sta, pooled_per_signal=pooled,
                                sin_prediction_low_over_high=pred,
                                station_low_over_high_median=pct(lh_all, 50),
                                rows_with_elevation=int(have.sum()), rows=int(el.size))


def item4_satellite(d, kind, out, signals=("C1C", "C2W")):
    """Per satellite: mean over stations of per-LOS means vs spread across stations."""
    res = {}
    for sig in signals:
        ix = np.nonzero(d["sig"] == sig)[0]
        if not ix.size:
            continue
        G = group(d["sat"][ix])
        print(f"[{kind}] {sig}: per satellite, the per-LOS MEANS over stations — their mean "
              f"(satellite constant) and their std across stations (station side)")
        print(f"  {'sat':5s} {'n_sta':>5s} {'sat_const':>10s} {'sta_spread':>10s} {'rms_raw':>9s}")
        consts, spreads, ns = [], [], []
        tab = {}
        for sat, j in sorted(G.items()):
            jj = ix[j]
            Gs = group(d["sta"][jj])
            los_means = np.array([float(d["res"][jj[k]].mean()) for k in Gs.values()])
            c = float(los_means.mean())
            s = float(los_means.std()) if los_means.size > 1 else float("nan")
            consts.append(c)
            spreads.append(s)
            ns.append(los_means.size)
            tab[sat[0]] = dict(n_sta=int(los_means.size), sat_const=c, sta_spread=s, rms_raw=rms(d["res"][jj]))
            print(f"  {sat[0]:5s} {los_means.size:5d} {c:10.3f} {fmt(s, 3, 10)} {rms(d['res'][jj]):9.3f}")
        consts = np.array(consts)
        spreads = np.array([s for s in spreads if math.isfinite(s)])
        # de-mean the satellite constants (a common-mode over satellites is a datum
        # offset the consumer's receiver clock absorbs; the between-satellite part is not)
        between = float(np.var(consts)) if consts.size > 1 else float("nan")
        within = float(np.mean(spreads ** 2)) if spreads.size else float("nan")
        print(f"  {sig}: between-satellite variance of the constants {between:.3f} m^2 (std {math.sqrt(between):.3f}), "
              f"mean within-satellite (across-station) variance {within:.3f} m^2 (std {math.sqrt(within):.3f}); "
              f"sat-side share {100.0 * between / (between + within):.1f} %")
        res[sig] = dict(per_sat=tab, between_var=between, within_var=within,
                        sat_side_share_pct=100.0 * between / (between + within) if (between + within) > 0 else None)
    out[f"item4_{kind}"] = res


def item5_refused(d, kind, out):
    """The seven rx-CB-refused stations vs the rest, per signal."""
    ref = np.isin(d["sta"], REFUSED7)
    present = sorted(set(d["sta"][ref].tolist()))
    print(f"[{kind}] rx-CB-refused seven present: {present} ({len(present)} of 7)")
    print(f"  {'signal':7s} {'group':8s} {'n':>7s} {'mean':>9s} {'rms_raw':>9s} {'rms_about_LOS_means':>20s} {'|LOSmean| p50':>14s}")
    res = {}
    for sig, ix in sorted(group(d["sig"]).items()):
        for name, mask in (("refused7", ref[ix]), ("rest", ~ref[ix])):
            jj = ix[mask]
            if not jj.size:
                continue
            v = d["res"][jj]
            Gs = group(d["sta"][jj], d["sat"][jj])
            ss_about = sum(float(np.sum((v[k] - v[k].mean()) ** 2)) for k in Gs.values())
            lm = np.array([abs(float(v[k].mean())) for k in Gs.values()])
            res[f"{sig[0]}:{name}"] = dict(n=int(v.size), mean=float(v.mean()), rms=rms(v),
                                           rms_about_los_means=math.sqrt(ss_about / v.size),
                                           abs_los_mean_p50=pct(lm, 50))
            print(f"  {sig[0]:7s} {name:8s} {v.size:7d} {v.mean():9.4f} {rms(v):9.4f} "
                  f"{math.sqrt(ss_about / v.size):20.4f} {pct(lm, 50):14.4f}")
    # per refused station, C1C/C2W mean and rms
    print("  per refused station (C1C | C2W): mean, rms")
    for sta in REFUSED7:
        ix = np.nonzero(d["sta"] == sta)[0]
        if not ix.size:
            print(f"    {sta}: ABSENT from the window")
            continue
        parts = []
        for sig in (("L1C", "L2W") if kind == "phase" else ("C1C", "C2W")):
            j = ix[d["sig"][ix] == sig]
            parts.append(f"{sig} {float(d['res'][j].mean()) if j.size else float('nan'):8.3f} / {rms(d['res'][j]):7.3f} (n {j.size})")
        print(f"    {sta}: " + " | ".join(parts))
    out[f"item5_{kind}"] = res


def twoway_fit(v, a_keys, b_keys, max_sweeps=200, tol=1e-10):
    """Least-squares fit of v = alpha[a] + beta[b] (two-way additive) by backfitting:
    alternating per-key means, which is Gauss-Seidel on the normal equations and
    converges to the same orthogonal projection as the dense solve without forming
    the (rows x keys) design matrix (60k x 4.6k = 2.3 GB on a 61-station leg).
    Returns the fitted values."""
    v = np.asarray(v, dtype=float)
    _, ia = np.unique(a_keys, return_inverse=True)
    _, ib = np.unique(b_keys, return_inverse=True)
    ia = ia.ravel()
    ib = ib.ravel()
    na = np.bincount(ia).astype(float)
    nb = np.bincount(ib).astype(float)
    alpha = np.zeros(na.size)
    beta = np.zeros(nb.size)
    ss_prev = float("inf")
    for _ in range(max_sweeps):
        alpha = np.bincount(ia, weights=v - beta[ib], minlength=na.size) / na
        beta = np.bincount(ib, weights=v - alpha[ia], minlength=nb.size) / nb
        r = v - alpha[ia] - beta[ib]
        ss = float(r @ r)
        if ss_prev - ss <= tol * max(ss, 1e-300):
            break
        ss_prev = ss
    return alpha[ia] + beta[ib]


def proj_means(v, keys):
    """Orthogonal projection of v onto per-key means (a one-way fit); returns the fitted values."""
    fitted = np.empty_like(v)
    for ix in group(keys).values():
        fitted[ix] = v[ix].mean()
    return fitted


def proj_gamma(v, sat, sig, keys):
    """Per group (one LOS-epoch, or one LOS for the window means) with >= 2 distinct
    known dispersive factors: the orthogonal projection of the group's signal-centred
    residuals onto the centred dispersive pattern (gamma_b - mean gamma), one
    parameter per group, giving the between-signal part that has the ionosphere's
    shape.

    Returns (fitted, participated mask, n_groups, n_groups with >= 3 signals,
    amplification), where amplification is the fit-SS-weighted mean over groups of
    sum(gamma^2) / sum((gamma - mean)^2): the factor a purely dispersive error's total
    sum of squares carries relative to its between-signal sum of squares (GPS
    C1C+C2W: 17.7; C1C+C2W+C5: 19.5), i.e. what the gamma-shaped share must be
    multiplied by to read the ionosphere's implied share of the whole residual.
    """
    fitted = np.zeros_like(v)
    part = np.zeros(v.size, dtype=bool)
    gam = np.array([disp_factor(s, g) or np.nan for s, g in zip(sat, sig)], dtype=float)
    n_groups = n3 = 0
    amp_num = amp_den = 0.0
    for ix in group(*keys).values():
        g = gam[ix]
        ok = np.isfinite(g)
        if ok.sum() < 2 or np.unique(g[ok]).size < 2:
            continue
        jj = ix[ok]
        gc = g[ok] - g[ok].mean()
        vc = v[jj] - v[jj].mean()
        den = float(gc @ gc)
        if den <= 0:
            continue
        f = (float(vc @ gc) / den) * gc
        fitted[jj] = f
        part[jj] = True
        n_groups += 1
        n3 += int(jj.size >= 3)
        ssf = float(f @ f)
        amp_num += ssf * float(np.sum(g[ok] ** 2)) / den
        amp_den += ssf
    return fitted, part, n_groups, n3, (amp_num / amp_den if amp_den > 0 else float("nan"))


def gamma_shape_and_closure(v, w, sat, sig, keys):
    """Per LOS-epoch group with >= 2 distinct dispersive factors:
      - 3-signal groups: the gamma-direction share of the between-signal energy
        (a dispersive error gives ~1.0; iid signal-specific energy gives ~0.5 with
        2 dof); their gamma-orthogonal remainder is also the empirical per-signal
        noise estimate (one dof per group): sigma_hat^2 = SS_rem3 / n3.
      - every group: the per-LOS iono normal-equation closure on these rows,
        c = |sum_b w_b g_b v_b| / sqrt(sum_b (w_b g_b v_b)^2). A per-LOS iono
        column whose normal equation is closed by the code rows alone (free
        ambiguities, since the phase rows carry no iono level information) forces
        sum_b w_b g_b v_b = 0 exactly (c ~ 0); if the phase, the SH tie, or a carry
        shares the column, c is O(1).
      - 2-signal groups: the measured slope of v_2 on v_1 through the origin and
        the closure-predicted slope median(-w_1 / (w_2 g_2)) (anti-correlated);
        a dispersive error predicts +g_2 (1.647), a signal-common error +1.
    Returns a dict."""
    gam = np.array([disp_factor(s, g) or np.nan for s, g in zip(sat, sig)], dtype=float)
    ss_b3 = ss_g3 = 0.0
    n3 = 0
    closure = []
    s12 = s11 = 0.0
    pred = []
    n2 = 0
    for ix in group(*keys).values():
        g = gam[ix]
        ok = np.isfinite(g)
        if ok.sum() < 2 or np.unique(g[ok]).size < 2:
            continue
        jj = ix[ok]
        gg = g[ok]
        vv = v[jj]
        ww = w[jj]
        t = ww * gg * vv
        den = math.sqrt(float(np.sum(t * t)))
        if den > 0:
            closure.append(abs(float(np.sum(t))) / den)
        if jj.size >= 3:
            gc = gg - gg.mean()
            vc = vv - vv.mean()
            f = (float(vc @ gc) / float(gc @ gc)) * gc
            ss_b3 += float(vc @ vc)
            ss_g3 += float(f @ f)
            n3 += 1
        if jj.size == 2:
            o = np.argsort(gg)          # band 1 first (smallest gamma)
            v1, v2 = vv[o[0]], vv[o[1]]
            w1, w2 = ww[o[0]], ww[o[1]]
            s12 += v1 * v2
            s11 += v1 * v1
            if w2 > 0 and gg[o[1]] > 0:
                pred.append(-w1 / (w2 * gg[o[1]]))
            n2 += 1
    closure = np.array(closure, dtype=float)
    return dict(n3=n3, gamma_share_3=(ss_g3 / ss_b3 if ss_b3 > 0 else float("nan")),
                sigma_hat=(math.sqrt((ss_b3 - ss_g3) / n3) if n3 > 0 else float("nan")),
                closure_p50=pct(closure, 50), closure_p90=pct(closure, 90), n_closure=int(closure.size),
                n2=n2, slope_meas=(s12 / s11 if s11 > 0 else float("nan")),
                slope_pred=pct(pred, 50))


def item6_decomposition(d, kind, out):
    """Variance decomposition of the pooled residual on orthogonal projections, so every
    table closes exactly. Marginal: each projection alone on the raw residual (bar A2).
    Nested: each fitted to what the previous ones left, in the physically nested order.
    Also runs the dispersive-shape tests that a two-signal split cannot make on its own."""
    v = d["res"].astype(float)
    w = d["w"].astype(float)
    n = v.size
    ss_tot = float(np.sum(v * v))
    print(f"[{kind}] pooled sum of squares {ss_tot:.1f} m^2 over {n} rows (rms {math.sqrt(ss_tot / n):.4f} m)")
    tow_s = np.array([f"{t:.3f}" for t in d["tow"]])
    key_ss = np.char.add(np.char.add(d["sta"], "|"), d["sig"])   # (station, signal)
    key_js = np.char.add(np.char.add(d["sat"], "|"), d["sig"])   # (satellite, signal)
    key_st = np.char.add(np.char.add(d["sta"], "|"), tow_s)      # (station, epoch)
    key_jt = np.char.add(np.char.add(d["sat"], "|"), tow_s)      # (satellite, epoch)
    key_le = np.char.add(np.char.add(key_st, "|"), d["sat"])     # (station, satellite, epoch)

    def share(x):
        return 100.0 * float(np.sum(x * x)) / ss_tot if ss_tot > 0 else float("nan")

    # ---- marginal ------------------------------------------------------------------
    f_common = proj_means(v, key_le)
    between = v - f_common
    f_gamma, part, ng, n3, amp = proj_gamma(v, d["sat"], d["sig"], (d["sta"], d["sat"], tow_s))
    marg = {
        "(station,signal) constants  [rx code bias row]": share(proj_means(v, key_ss)),
        "(satellite,signal) constants  [satellite constant row]": share(proj_means(v, key_js)),
        "(station,epoch) common mode  [receiver clock vs code]": share(proj_means(v, key_st)),
        "(satellite,epoch) common mode  [satellite clock vs code]": share(proj_means(v, key_jt)),
        "(station,epoch)+(satellite,epoch) joint": share(twoway_fit(v, key_st, key_jt)),
        "LOS-epoch signal-COMMON mean (non-dispersive by construction)": share(f_common),
        "LOS-epoch BETWEEN-signal part (all)": share(between),
        "   of which gamma-direction (with 2 signals: ALL of it, by construction)": share(f_gamma),
        "   of which gamma-orthogonal remainder (3-signal groups only)": share(between - f_gamma),
    }
    print("  MARGINAL shares (each projection alone, % of the pooled sum of squares):")
    for k_, s in marg.items():
        print(f"    {k_:78s} {s:6.1f} %")
    s_common = marg["LOS-epoch signal-COMMON mean (non-dispersive by construction)"]
    s_between = marg["LOS-epoch BETWEEN-signal part (all)"]
    s_gamma = marg["   of which gamma-direction (with 2 signals: ALL of it, by construction)"]
    print(f"    LOS-epoch groups with >= 2 signals: {ng} ({n3} with >= 3); rows in them {int(part.sum())} of {n}; "
          f"amplification sum(g^2)/sum((g-mean)^2) = {amp:.2f}")
    # ---- dispersive-shape consistency ----------------------------------------------
    pure = 1.0 / (amp - 1.0) if amp > 1.0 else float("nan")
    ratio = s_between / s_common if s_common > 0 else float("nan")
    print(f"  SHAPE: between-signal / signal-common energy = {ratio:.3f}; a PURELY dispersive error carries "
          f"{pure:.3f} (= 1/(amp-1)).  << that: a small dispersive rider on a signal-common error; "
          f">> that: the between-signal part is NOT the ionosphere's amplitude pattern (per-signal energy).")
    sh = gamma_shape_and_closure(v, w, d["sat"], d["sig"], (d["sta"], d["sat"], tow_s))
    print(f"  SHAPE (3-signal LOS-epochs, n {sh['n3']}): gamma-direction share of their between-signal energy = "
          f"{sh['gamma_share_3']:.2f}  (dispersive => ~1.0; iid signal-specific => ~0.5); empirical per-signal "
          f"noise from their gamma-orthogonal dof: sigma_hat = {sh['sigma_hat']:.3f} m")
    corr_gamma_ss = float(np.sum(f_gamma ** 2)) - (sh["sigma_hat"] ** 2 * ng if math.isfinite(sh["sigma_hat"]) else 0.0)
    implied_raw = s_gamma * amp
    implied_corr = 100.0 * max(corr_gamma_ss, 0.0) * amp / ss_tot if ss_tot > 0 else float("nan")
    implied = min(implied_corr, 100.0)
    flag = ("" if implied_corr <= 100.0 else
            "  [> 100 %: the between-signal energy exceeds what a dispersive error can carry -> NOT dispersive-shaped; capped]")
    print(f"  IMPLIED dispersive (ionosphere) share of the whole residual: gamma-direction x amplification = "
          f"{implied_raw:.1f} % raw, {implied_corr:.1f} % after the sigma_hat noise correction{flag}")
    print(f"  CLOSURE of the per-LOS iono normal equation on these rows, c = |sum w g v| / sqrt(sum (w g v)^2): "
          f"p50 {sh['closure_p50']:.3f}, p90 {sh['closure_p90']:.3f} over {sh['n_closure']} LOS-epochs "
          f"(~0 => the iono column is closed by the CODE rows alone: free ambiguities, no phase LEVEL "
          f"information; O(1) => the phase / SH tie / a carry share it)")
    print(f"  2-signal LOS-epochs (n {sh['n2']}): slope of v_2 on v_1 through the origin, measured {sh['slope_meas']:.3f}; "
          f"closure-predicted -w1/(w2 g2) median {sh['slope_pred']:.3f}; dispersive +g2 = 1.647; signal-common +1.000")
    # ---- the same on the per-LOS window means (the constant part; noise averaged down) --
    G = group(d["sta"], d["sat"], d["sig"])
    m_sta, m_sat, m_sig, m_val, m_w = [], [], [], [], []
    for (sta, sat, sig), ix in G.items():
        m_sta.append(sta); m_sat.append(sat); m_sig.append(sig)
        m_val.append(float(v[ix].mean())); m_w.append(float(w[ix].mean()))
    m_sta, m_sat, m_sig, m_val, m_w = map(np.array, (m_sta, m_sat, m_sig, m_val, m_w))
    ss_m = float(np.sum(m_val ** 2))
    key_m = np.char.add(np.char.add(m_sta, "|"), m_sat)
    fm_common = proj_means(m_val, key_m)
    fm_gamma, pm, ngm, n3m, ampm = proj_gamma(m_val, m_sat, m_sig, (m_sta, m_sat))
    shm = gamma_shape_and_closure(m_val, m_w, m_sat, m_sig, (m_sta, m_sat))
    sm_common = 100.0 * float(np.sum(fm_common ** 2)) / ss_m if ss_m > 0 else float("nan")
    sm_gamma = 100.0 * float(np.sum(fm_gamma ** 2)) / ss_m if ss_m > 0 else float("nan")
    sm_rest = 100.0 * float(np.sum((m_val - fm_common - fm_gamma) ** 2)) / ss_m if ss_m > 0 else float("nan")
    print(f"  per-LOS window MEANS ({m_val.size} LOS-signal series; the CONSTANT part): signal-common "
          f"{sm_common:.1f} %, gamma-direction {sm_gamma:.1f} %, gamma-orthogonal {sm_rest:.1f} %; "
          f"3-signal LOS (n {shm['n3']}) gamma-direction share {shm['gamma_share_3']:.2f}; "
          f"2-signal slope measured {shm['slope_meas']:.3f} vs closure {shm['slope_pred']:.3f}; "
          f"closure c p50 {shm['closure_p50']:.3f}")
    # ---- nested (closes exactly) -----------------------------------------------------
    print("  NESTED sequence (orthogonal projections on what the previous rows left; shares of the "
          "ORIGINAL pooled sum of squares; closes to 100):")
    rem = v.copy()
    nested = []
    for name, keys in (("(station,signal) constants", key_ss),
                       ("(satellite,signal) constants", key_js),
                       ("(station,epoch) common mode", key_st),
                       ("(satellite,epoch) common mode", key_jt),
                       ("LOS-epoch signal-common mean (geometry/tropo/multipath common to the signals)", key_le)):
        f = proj_means(rem, keys)
        nested.append((name, share(f)))
        rem = rem - f
    f, *_ = proj_gamma(rem, d["sat"], d["sig"], (d["sta"], d["sat"], tow_s))
    nested.append(("LOS-epoch gamma-direction between-signal (2 signals: all of it)", share(f)))
    rem = rem - f
    nested.append(("remainder (gamma-orthogonal; single-signal rows)", share(rem)))
    for name, s in nested:
        print(f"    {name:78s} {s:6.1f} %")
    print(f"    {'(sum)':78s} {sum(s for _, s in nested):6.1f} %")
    # ---- bar A2 --------------------------------------------------------------------
    # Named rows of the SPEC table only.  The ionosphere row enters with its implied
    # share only when its own shape test admits it (between/common energy within 3x
    # of the 1/(amp-1) a dispersive error carries, and the 3-signal gamma-direction
    # share >= 0.8); otherwise it is shape-refuted and reported as such.  The
    # multipath or observation-model row is what is left per LOS after every named
    # state-side row (the nested LOS-epoch rows' sum): elevation-shaped and
    # phase-clean names that row, metre amplitude and persistence argue against it
    # (item 1 and item 3 carry both reads).
    iono_ok = (math.isfinite(ratio) and math.isfinite(pure) and ratio <= 3.0 * pure
               and math.isfinite(sh["gamma_share_3"]) and sh["gamma_share_3"] >= 0.8)
    los_unexplained = sum(s for name, s in nested[4:])
    cand = {
        "rx code bias: per-(station,signal) constant": marg["(station,signal) constants  [rx code bias row]"],
        "satellite constant: per-(satellite,signal)": marg["(satellite,signal) constants  [satellite constant row]"],
        "satellite clock vs code: per-(satellite,epoch) common mode": marg["(satellite,epoch) common mode  [satellite clock vs code]"],
        "receiver clock vs code: per-(station,epoch) common mode": marg["(station,epoch) common mode  [receiver clock vs code]"],
        "ionosphere: implied dispersive (noise-corrected; shape-admitted only)": (implied if iono_ok else float("nan")),
        "per-LOS residual after every named state row (multipath / observation-model row IF elevation-shaped and phase-clean)": los_unexplained,
    }
    finite = {k_: s for k_, s in cand.items() if math.isfinite(s)}
    best = max(finite.items(), key=lambda kv: kv[1])
    print("  bar A2 candidates (NAMED rows; marginal shares of the pooled code variance, the last one nested):")
    for k_, s in cand.items():
        tag = (f"{s:6.1f} %" if math.isfinite(s) else
               f"   shape-REFUTED (between/common {ratio:.3f} vs dispersive {pure:.3f}; 3-signal gamma share {sh['gamma_share_3']:.2f})")
        print(f"    {k_:110s} {tag}")
    print(f"  bar A2 (> 70 % in ONE named row): largest = {best[0]} at {best[1]:.1f} % -> "
          f"{'PASS' if best[1] > 70 else 'FAIL (no single named row above 70 %)'}")
    out[f"item6_{kind}"] = dict(ss_total=ss_tot, n=int(n), marginal=marg, nested=nested,
                                groups=ng, groups_3plus=n3, amplification=amp,
                                between_over_common=ratio, pure_dispersive_ratio=pure,
                                shape=sh, implied_dispersive_raw=implied_raw,
                                implied_dispersive_corrected=implied_corr,
                                means=dict(n=int(m_val.size), common=sm_common, gamma=sm_gamma, rest=sm_rest,
                                           shape=shm),
                                a2_candidates=cand, a2_best=best[0], a2_best_share=best[1],
                                a2_pass=bool(best[1] > 70))


def item7_ranking(d, kind, legdir, seed_run, k_lo, k_hi, out):
    """Leg per-station standardized rms vs the seed's pf_keyed STA ranking; self-consistency."""
    z = np.abs(d["res"]) * np.sqrt(np.maximum(d["w"], 0.0))
    G = group(d["sta"])
    leg_std = {sta[0]: rms(z[ix]) for sta, ix in G.items()}
    leg_m = {sta[0]: rms(d["res"][ix]) for sta, ix in G.items()}
    tows = sorted(set(d["tow"].tolist()))
    # self-consistency against the leg's own pf_keyed STA rows
    own = read_pf_keyed_sta(os.path.join(legdir, "logs", "network_pf_keyed.csv"))
    col_n, col_r = (0, 1) if kind == "code" else (2, 3)
    maxd = 0.0
    nmis = 0
    ncmp = 0
    for (tow, sta), ix in group(d["tow"], d["sta"]).items():
        rec = own.get((tow, sta))
        if rec is None:
            continue
        ncmp += 1
        if rec[col_n] != ix.size:
            nmis += 1
        maxd = max(maxd, abs(rec[col_r] - rms(z[ix])))
    if ncmp:
        print(f"[{kind}] self-consistency vs the leg's own pf_keyed STA rows: {ncmp} (tow, station) cells compared; "
              f"n mismatches {nmis}; max |rms(|v|sqrt(w)) - rms_{kind}| {maxd:.5f}")
    else:
        print(f"[{kind}] self-consistency: the leg has no pf_keyed STA rows at the window tows")
    seed = read_pf_keyed_sta(os.path.join(seed_run, "logs", "network_pf_keyed.csv"))
    res = dict(self_consistency=dict(cells=ncmp, n_mismatch=nmis, max_abs_diff=maxd))
    if not seed:
        print(f"  seed pf_keyed not found under {seed_run}")
        out[f"item7_{kind}"] = res
        return
    seed_tows = sorted(set(t for t, _ in seed))
    t0 = seed_tows[0]

    def seed_rank(tow_set, label):
        acc = defaultdict(lambda: [0.0, 0])
        for (tow, sta), rec in seed.items():
            if tow in tow_set and rec[col_n] > 0:
                acc[sta][0] += rec[col_n] * rec[col_r] ** 2
                acc[sta][1] += rec[col_n]
        seed_rms = {s: math.sqrt(a[0] / a[1]) for s, a in acc.items() if a[1] > 0}
        common = sorted(set(seed_rms) & set(leg_std))
        if len(common) < 5:
            print(f"  {label}: only {len(common)} common stations")
            return None
        a = np.array([leg_std[s] for s in common])
        b = np.array([seed_rms[s] for s in common])
        rho = spearman(a, b)
        r = pearson(a, b)
        print(f"  {label}: {len(common)} stations, Spearman {rho:.3f} (bar A1 > 0.9 -> {'PASS' if rho > 0.9 else 'FAIL'}), "
              f"Pearson {r:.3f}; leg median {np.median(a):.3f} vs seed median {np.median(b):.3f} (standardized |v|sqrt(w))")
        top = sorted(common, key=lambda s: -leg_std[s])[:10]
        print("   top-10 leg stations (leg std-rms / seed std-rms / leg rms m): "
              + ", ".join(f"{s[:4]} {leg_std[s]:.2f}/{seed_rms[s]:.2f}/{leg_m[s]:.2f}" for s in top))
        return dict(n=len(common), spearman=rho, pearson=r,
                    leg=dict((s, leg_std[s]) for s in common), seed=dict((s, seed_rms[s]) for s in common))

    res["same_epochs"] = seed_rank(set(tows), f"seed at the SAME epochs (k {k_lo}..{k_hi})")
    res["seed_h2_5"] = seed_rank(set(t for t in seed_tows if t0 + 7200 <= t < t0 + 18000),
                                 "seed hours 2-5 (settled)")
    out[f"item7_{kind}"] = res


# --------------------------------------------------------------------------- main
def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("legdir")
    ap.add_argument("--k-lo", type=int, default=10)
    ap.add_argument("--k-hi", type=int, default=60)
    ap.add_argument("--tow0", type=float, default=277320.0,
                    help="tow of k = 1 (default 277320 = the v8 ep0604 legs' first epoch)")
    ap.add_argument("--seed-run", default=os.path.join(REPO, "runs", "evseed_field6h_v8"))
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    legdir = os.path.abspath(args.legdir)
    rows_path = os.path.join(legdir, "logs", "network_pf_rows.csv")
    if not os.path.isfile(rows_path):
        raise SystemExit(f"FATAL: {rows_path} not found (arm network.trace_postfit_rows)")
    d = read_rows(rows_path)
    n_all = d["res"].size
    out = dict(legdir=legdir, rows_file=rows_path, n_rows_file=int(n_all), n_bad_lines=int(d["n_bad"]))
    hr(f"§24.6 per-LOS post-fit residual attribution — {legdir}")
    print(f"rows in file: {n_all} ({d['n_bad']} malformed skipped); kinds: "
          + ", ".join(f"{k}={int(np.sum(d['kind'] == k))}" for k in sorted(set(d['kind'].tolist()))))
    if n_all == 0:
        raise SystemExit("FATAL: 0 rows — an ARMED trace wrote nothing (invariant #4)")
    k = 1 + np.rint((d["tow"] - args.tow0) / 30.0).astype(int)
    print(f"tow {d['tow'].min():.1f}..{d['tow'].max():.1f} -> k {k.min()}..{k.max()} (tow0 {args.tow0:.0f}); "
          f"window k = {args.k_lo}..{args.k_hi}")
    # elevation fallback from network_ionosphere.csv for rows written without one
    miss = ~np.isfinite(d["el"])
    n_miss0 = int(miss.sum())
    if n_miss0:
        io = read_iono_elev(os.path.join(legdir, "logs", "network_ionosphere.csv"))
        filled = 0
        if io:
            for i in np.nonzero(miss)[0]:
                e = io.get((round(float(d["tow"][i]), 3), d["sta"][i], d["sat"][i]))
                if e is not None:
                    d["el"][i] = e
                    filled += 1
        print(f"rows without elev_deg: {n_miss0}; filled from network_ionosphere.csv: {filled}")
    else:
        print("rows without elev_deg: 0")
    out["rows_no_elev"] = n_miss0

    win = (k >= args.k_lo) & (k <= args.k_hi)
    dw = sub(d, win)
    print(f"rows in window: {int(win.sum())}")
    # anomalies
    hr("Anomalies / coverage")
    stations = sorted(set(dw["sta"].tolist()))
    sats = sorted(set(dw["sat"].tolist()))
    sigs = sorted(set(dw["sig"].tolist()))
    print(f"stations {len(stations)}, satellites {len(sats)}, signals {sigs}")
    no_code = [s for s in stations if not np.any((dw["sta"] == s) & (dw["kind"] == "code"))]
    no_phase = [s for s in stations if not np.any((dw["sta"] == s) & (dw["kind"] == "phase"))]
    empty_sat = int(np.sum(dw["sat"] == ""))
    empty_sig = int(np.sum(dw["sig"] == ""))
    print(f"stations without code rows: {no_code or 'none'}; without phase rows: {no_phase or 'none'}")
    print(f"rows with empty sat: {empty_sat}; with empty signal: {empty_sig}; non-finite resid: "
          f"{int(np.sum(~np.isfinite(dw['res'])))}")
    n_ep = len(set(dw["tow"].tolist()))
    print(f"epochs in window: {n_ep}; rows/epoch {int(win.sum()) / max(n_ep, 1):.0f}")
    per_sta_sig = defaultdict(set)
    for s, g in zip(dw["sta"], dw["sig"]):
        per_sta_sig[s].add(g)
    missing_c2w = [s for s in stations if "C2W" not in per_sta_sig[s]]
    no_l5 = [s for s in stations if not ({"C5Q", "C5X"} & per_sta_sig[s])]
    print(f"stations without C2W rows: {missing_c2w or 'none'}; stations without any C5 rows: "
          f"{len(no_l5)} ({', '.join(x[:4] for x in no_l5) if no_l5 else 'none'})")
    absent_refused = [s for s in REFUSED7 if s not in stations]
    print(f"rx-CB-refused seven absent from the window: {absent_refused or 'none'}")
    out["anomalies"] = dict(stations=len(stations), satellites=len(sats), signals=sigs,
                            no_code=no_code, no_phase=no_phase, empty_sat=empty_sat, empty_sig=empty_sig,
                            epochs=n_ep, no_c2w=missing_c2w, no_c5=no_l5, refused_absent=absent_refused)

    ok = np.isfinite(dw["res"]) & (dw["sat"] != "") & (dw["sig"] != "")
    for kind in ("code", "phase"):
        dk = sub(dw, ok & (dw["kind"] == kind))
        if dk["res"].size == 0:
            hr(f"{kind.upper()}: no rows in the window")
            continue
        hr(f"{kind.upper()} — item 1: per-LOS mean and rms about the mean")
        item1_los(dk, kind, out)
        hr(f"{kind.upper()} — item 2: cross-signal rms ratios and per-LOS sign agreement")
        item2_ratios(dk, kind, out)
        hr(f"{kind.upper()} — item 3: elevation dependence")
        item3_elevation(dk, kind, out)
        hr(f"{kind.upper()} — item 4: satellite-side constants vs station-side spread")
        item4_satellite(dk, kind, out, signals=(("L1C", "L2W") if kind == "phase" else ("C1C", "C2W")))
        hr(f"{kind.upper()} — item 5: the rx-CB-refused seven vs the rest")
        item5_refused(dk, kind, out)
        hr(f"{kind.upper()} — item 6: variance decomposition")
        item6_decomposition(dk, kind, out)
        hr(f"{kind.upper()} — item 7: per-station ranking vs the seed's pf_keyed (bar A1) + self-consistency")
        item7_ranking(dk, kind, legdir, args.seed_run, args.k_lo, args.k_hi, out)

    if args.json:
        def conv(o):
            if isinstance(o, (np.floating,)):
                return float(o)
            if isinstance(o, (np.integer,)):
                return int(o)
            if isinstance(o, np.ndarray):
                return o.tolist()
            if isinstance(o, float) and not math.isfinite(o):
                return None
            raise TypeError(str(type(o)))
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=1, default=conv)
        print(f"\njson -> {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
