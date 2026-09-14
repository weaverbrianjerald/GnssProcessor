#!/usr/bin/env python3
"""SPEC_event_harness §24.13 — card LV-SEED: where the remaining per-LOS level is born.

READ-ONLY.  Four pre-registered reads on EXISTING runs (no producer run, no new
mechanism); every run dir supplies `logs/network_pf_rows.csv` (signed per-row
post-fit residuals), `logs/network_pf_keyed.csv`, `logs/network_zwd.csv`,
`logs/network_ionosphere.csv`, `logs/network_vtec_sh.csv`, `logs/network_stations.csv`
and `products.txt`.

    python scripts/lvseed_read.py r1        # H1 — is the level born at arc start?
    python scripts/lvseed_read.py r2        # H2 — is the level ionospheric?
    python scripts/lvseed_read.py r3        # H3 — is the level station geometry?
    python scripts/lvseed_read.py r4        # (only if R1 names H1) wire steps at arc births
    python scripts/lvseed_read.py witness   # per-station code rms vs arcs born inside the run
    python scripts/lvseed_read.py all

Each writes `<out>/<read>.json` and `<out>/<read>.txt` (default out = runs/_lvseed).

CONVENTIONS (stated once; every table below obeys them)
  * LOS arc  = consecutive rows of one (station, satellite, signal, kind) whose
    epoch step never exceeds `--gap-epochs` (default 5 ⇒ Δt > 150 s splits).  Arcs
    are kept when they carry >= `--min-arc` rows (default 30).
  * LEVEL    = the UNWEIGHTED mean of the CODE-row residual `resid_m` over the
    stated window.  `resid_m` is the signed post-fit residual v = y − A_s x_s −
    A_r x_r in metres (NetworkFrameDiag.cpp), NOT standardized.  The level is a
    metre quantity, so weighting it by `w` would tilt it toward high elevation;
    `w` (the normal-equation weight, 1/σ²-like — pf_keyed pools |v|·sqrt(w)) is
    used ONLY as the least-squares weight in R3 and as the LOS weight when a
    per-LOS quantity enters a fit, and each such use says so in the output.
  * SETTLED  = the campaign's last hour, k = 121..240 ⇔ tow 262800..266370
    (cm_grade.py's default window), so R2/R3 sit on the same window as §24.11/§24.12.
  * TIE RESIDUAL (R2) = the per-LOS ABSOLUTE slant iono minus the SH field's
    prediction at its pierce point, i.e. exactly the quantity
    `networkAppendShIonoRows_` (ApplicationNetworkAssembly.cpp:997-1100) asserts
    to be zero:  ι(rx,sat) − Σ_l M_l(el)·tecu_to_L1m·VTEC_l(IPP_l).  ι is
    `iono_total_m` = x_lin + x_r, NEVER `iono_m` (x_r alone is a CORRECTION about
    the allocation seed — CLAUDE.md Amendment 4 / NetworkIonoDiag.cpp:95-105).
    The row's R keeps an all-ones per-(rx,const) block, so the per-station-epoch
    MEAN of the tie residual is a free datum the tie does not screen; both the raw
    and the station-epoch-demeaned form are reported and the demeaned one is what
    the tie actually constrains.
  * e_LOS    = the receiver→satellite unit vector in the station's local ENU frame,
    built from the elevation/azimuth the producer itself recorded in
    `network_ionosphere.csv` (az measured North→East, the same convention as
    computeIppGeometry): e_E = cos(el)sin(az), e_N = cos(el)cos(az), e_U = sin(el).
    No SP3 is read.  A residual model v ≈ a + d·e means an implied station
    position error δ = −d (∂ρ/∂r_rx = −e), so the implied height error is −dU.

BARS (frozen in §24.13; this script reports the numbers, it does not grade)
  H1 named : R1 fraction(|m_early − m_late| < 0.3 m) > 0.7 AND r(first-epoch, m_late) > 0.8
  H2 named : R2 level_C2W/level_C1C within 1.6469 ± 0.3 with r > 0.8, OR tie-residual r > 0.5
  H3 named : the |ZWD| > 0.5 m stations carry |d| > 1 m with a consistent vertical
             while the others do not
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import defaultdict

import numpy as np
import pandas as pd

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from sh_vtec import legendre_normalized, mean_sun_longitude  # noqa: E402

PROJECT_DEFAULT = "C:/GnssProcessor"
ARMS_DEFAULT = ["_cm_pvb", "_cm_b7", "_cm_b1"]
TOW0 = 259200.0
DT_S = 30.0
K_SETTLED_LO, K_SETTLED_HI = 121, 240

F_L1 = 1575.42e6
F_L2 = 1227.60e6
F_L5 = 1176.45e6
GAMMA_L2 = (F_L1 / F_L2) ** 2          # 1.646944
GAMMA_L5 = (F_L1 / F_L5) ** 2          # 1.793270
TECU_TO_L1M = 40.3 * 1.0e16 / (F_L1 * F_L1)

# GlobalVtecShConfig of the CM/PV lineage (read back from the arm's own JSON at run
# time; these are only the fallbacks).
SH_EARTH_R_KM = 6371.0
SH_LAYERS_KM = {0: 450.0, 1: 1300.0}

WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563

BAND_OF = {"C1C": "L1", "C1W": "L1", "C1X": "L1",
           "C2W": "L2", "C2X": "L2", "C2L": "L2",
           "C5Q": "L5", "C5X": "L5", "C5I": "L5"}


# --------------------------------------------------------------------------- #
# small numerics (scipy is not installed on this box)
# --------------------------------------------------------------------------- #
def pearson(a, b):
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 3:
        return float("nan")
    a, b = a[m] - a[m].mean(), b[m] - b[m].mean()
    d = math.sqrt(float(a @ a) * float(b @ b))
    return float(a @ b / d) if d > 0 else float("nan")


def spearman(a, b):
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 3:
        return float("nan")
    return pearson(_rank(a[m]), _rank(b[m]))


def _rank(x):
    order = np.argsort(x, kind="mergesort")
    r = np.empty(len(x), float)
    r[order] = np.arange(len(x), dtype=float)
    # average ties
    s = x[order]
    i = 0
    while i < len(s):
        j = i
        while j + 1 < len(s) and s[j + 1] == s[i]:
            j += 1
        if j > i:
            r[order[i:j + 1]] = (i + j) / 2.0
        i = j + 1
    return r


def ols_slope(x, y, weights=None):
    """y = c + s*x; returns (slope, intercept, r).  Weighted when `weights` given."""
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    m = np.isfinite(x) & np.isfinite(y)
    if m.sum() < 3:
        return float("nan"), float("nan"), float("nan")
    x, y = x[m], y[m]
    w = np.ones_like(x) if weights is None else np.asarray(weights, float)[m]
    sw = w.sum()
    xb, yb = (w * x).sum() / sw, (w * y).sum() / sw
    sxx = (w * (x - xb) ** 2).sum()
    sxy = (w * (x - xb) * (y - yb)).sum()
    s = sxy / sxx if sxx > 0 else float("nan")
    return float(s), float(yb - s * xb), pearson(x, y)


def q(v, p):
    v = np.asarray([x for x in v if np.isfinite(x)], float)
    return float(np.quantile(v, p)) if v.size else float("nan")


def f(x, p=3):
    return "nan" if x is None or not np.isfinite(x) else f"{x:.{p}f}"


# --------------------------------------------------------------------------- #
# I/O
# --------------------------------------------------------------------------- #
def arm_dir(project, arm):
    return arm if os.path.isabs(arm) else os.path.join(project, "runs", arm)


def load_pf_rows(project, arm):
    p = os.path.join(arm_dir(project, arm), "logs", "network_pf_rows.csv")
    df = pd.read_csv(p, dtype={"station": "string", "sat": "string",
                               "signal": "string", "kind": "string"})
    df["k"] = np.rint((df["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    return df


def load_iono(project, arm):
    p = os.path.join(arm_dir(project, arm), "logs", "network_ionosphere.csv")
    df = pd.read_csv(p, dtype={"station": "string", "sat": "string"})
    df["k"] = np.rint((df["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    return df


def load_stations(project, arm):
    p = os.path.join(arm_dir(project, arm), "logs", "network_stations.csv")
    df = pd.read_csv(p)
    out = {}
    for r in df.itertuples(index=False):
        lat, lon, h = ecef_to_llh(r.ecef_x, r.ecef_y, r.ecef_z)
        out[r.station] = dict(ecef=(r.ecef_x, r.ecef_y, r.ecef_z),
                              lat_deg=math.degrees(lat), lon_deg=math.degrees(lon),
                              h_m=h, role=r.role)
    return out


def load_sh_cfg(project, arm):
    p = os.path.join(arm_dir(project, arm), "arm.json")
    try:
        cfg = json.load(open(p))["estimation"]["global_vtec_sh"]
    except Exception:
        return dict(earth_radius_km=SH_EARTH_R_KM, layers=dict(SH_LAYERS_KM),
                    sun_fixed=True, representation_sigma_tecu=None)
    layers = {0: float(cfg.get("layer_height_km", 450.0))}
    if int(cfg.get("layer2_max_degree", 0)) > 0:
        layers[1] = float(cfg.get("layer2_height_km", 1300.0))
    return dict(earth_radius_km=float(cfg.get("earth_radius_km", SH_EARTH_R_KM)),
                layers=layers, sun_fixed=bool(cfg.get("sun_fixed", True)),
                representation_sigma_tecu=cfg.get("representation_sigma_tecu"))


def ecef_to_llh(x, y, z):
    e2 = WGS84_F * (2.0 - WGS84_F)
    p = math.hypot(x, y)
    lon = math.atan2(y, x)
    lat = math.atan2(z, p * (1.0 - e2))
    for _ in range(8):
        s = math.sin(lat)
        n = WGS84_A / math.sqrt(1.0 - e2 * s * s)
        h = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1.0 - e2 * n / (n + h)))
    s = math.sin(lat)
    n = WGS84_A / math.sqrt(1.0 - e2 * s * s)
    return lat, lon, p / math.cos(lat) - n


# --------------------------------------------------------------------------- #
# arcs
# --------------------------------------------------------------------------- #
def split_arcs(k, gap_epochs):
    """k: sorted epoch indices of one (station, sat, signal, kind).  -> list of slices."""
    cut = np.nonzero(np.diff(k) > gap_epochs)[0]
    bounds = [0] + (cut + 1).tolist() + [len(k)]
    return [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]


def build_arcs(df, kind, gap_epochs, min_arc):
    """-> list of dicts, one per kept arc, in (station, sat, signal) order."""
    sub = df[df["kind"] == kind]
    # a long run can end on a partially-flushed trace line, which pandas reads as NA
    # in the key columns and which then makes the group-boundary test ambiguous
    sub = sub.dropna(subset=["station", "sat", "signal", "resid_m", "w", "elev_deg"])
    sub = sub.sort_values(["station", "sat", "signal", "k"], kind="mergesort")
    sta = sub["station"].to_numpy(dtype=object)
    sat = sub["sat"].to_numpy(dtype=object)
    sig = sub["signal"].to_numpy(dtype=object)
    kk = sub["k"].to_numpy()
    res = sub["resid_m"].to_numpy(float)
    w = sub["w"].to_numpy(float)
    el = sub["elev_deg"].to_numpy(float)

    key = np.empty(len(sub), dtype=object)
    for i in range(len(sub)):
        key[i] = (sta[i], sat[i], sig[i])
    # group boundaries
    gb = [0] + [i for i in range(1, len(sub)) if key[i] != key[i - 1]] + [len(sub)]
    arcs = []
    for gi in range(len(gb) - 1):
        lo, hi = gb[gi], gb[gi + 1]
        for a, b in split_arcs(kk[lo:hi], gap_epochs):
            i0, i1 = lo + a, lo + b
            n = i1 - i0
            if n < min_arc:
                continue
            arcs.append(dict(station=sta[i0], sat=sat[i0], signal=sig[i0],
                             k=kk[i0:i1], res=res[i0:i1], w=w[i0:i1], el=el[i0:i1],
                             n=n, k0=int(kk[i0]), k1=int(kk[i1 - 1])))
    return arcs


# --------------------------------------------------------------------------- #
# R1 — is the level born at arc start?
# --------------------------------------------------------------------------- #
def r1_stats(rows, rng):
    """rows: list of per-arc dicts with first/m_early/m_late/... -> the table entry."""
    if not rows:
        return dict(n_arcs=0)
    d = np.array([r["m_early"] - r["m_late"] for r in rows])
    first = np.array([r["first"] for r in rows])
    late = np.array([r["m_late"] for r in rows])
    early = np.array([r["m_early"] for r in rows])
    ctrl = np.array([r["ctrl_row"] for r in rows])
    ctrl_late = np.array([r["ctrl_late"] for r in rows])
    within = np.array([r["rms_about_late"] for r in rows])
    sig_L = float(np.std(late, ddof=1)) if len(late) > 1 else float("nan")
    sig_n = float(np.median(within))
    # The pooled per-LOS level is heavy-tailed (a handful of metre-to-100 m arcs), and
    # a Pearson r on a heavy tail is a statement about those few arcs, not the pool —
    # so every correlation is reported THREE ways: raw Pearson (the pre-registered
    # statistic), Spearman (rank, outlier-immune), and Pearson on the |level| < 5 m
    # trim.  A raw r that the trim and the rank both refuse is an outlier artifact.
    tm = np.abs(late) < 5.0
    return dict(
        n_arcs=len(rows),
        frac_delta_lt_0p3=float(np.mean(np.abs(d) < 0.3)),
        med_abs_delta=float(np.median(np.abs(d))),
        p90_abs_delta=float(np.quantile(np.abs(d), 0.9)),
        r_first_vs_late=pearson(first, late),
        rho_first_vs_late=spearman(first, late),
        r_first_vs_late_trim5m=pearson(first[tm], late[tm]),
        n_trim5m=int(tm.sum()),
        frac_delta_lt_0p3_trim5m=float(np.mean(np.abs(d[tm]) < 0.3)) if tm.any() else float("nan"),
        r_early3_vs_late=pearson(early, late),
        # matched control: a RANDOM in-window epoch against the late mean with that
        # epoch left out — the same statistic with the "birth" content removed.
        r_ctrl_random_epoch=pearson(ctrl, ctrl_late),
        rho_ctrl_random_epoch=spearman(ctrl, ctrl_late),
        sigma_level_m=sig_L,
        sigma_within_arc_m=sig_n,
        # what r(first, late) must be if the level is an exact per-arc constant and
        # the single-epoch scatter is the within-arc rms: sigL/sqrt(sigL^2+sign^2).
        r_predicted_if_constant=float(sig_L / math.sqrt(sig_L ** 2 + sig_n ** 2))
        if np.isfinite(sig_L) and np.isfinite(sig_n) and (sig_L or sig_n) else float("nan"),
        med_level_m=float(np.median(late)),
        p50_abs_level_m=float(np.median(np.abs(late))),
        p90_abs_level_m=float(np.quantile(np.abs(late), 0.9)),
        p99_abs_level_m=float(np.quantile(np.abs(late), 0.99)),
        max_abs_level_m=float(np.abs(late).max()),
        n_level_gt_1m=int((np.abs(late) > 1.0).sum()),
        n_level_gt_5m=int((np.abs(late) > 5.0).sum()),
        # direction: does the level GROW over the arc (|late| > |early|) or is it set
        # at birth and held?  0.5 is a coin flip.
        frac_level_grew=float(np.mean(np.abs(late) > np.abs(early))),
        frac_level_grew_gt_1m=float(np.mean((np.abs(late) > np.abs(early))[np.abs(late) > 1.0]))
        if (np.abs(late) > 1.0).any() else float("nan"),
    )


def read_r1(project, arms, gap_epochs, min_arc, seed=20260914):
    rng = np.random.default_rng(seed)
    out = {}
    for arm in arms:
        df = load_pf_rows(project, arm)
        arcs = build_arcs(df, "code", gap_epochs, min_arc)
        rows = []
        for a in arcs:
            res = a["res"]
            m_early = float(res[:3].mean())
            m_late = float(res[9:].mean())
            # matched control: one random row from the late window, vs the late mean
            # computed WITHOUT it (so first-epoch and control share no noise term).
            late = res[9:]
            j = int(rng.integers(0, len(late)))
            ctrl_late = float((late.sum() - late[j]) / (len(late) - 1)) if len(late) > 1 else float("nan")
            rows.append(dict(station=a["station"], sat=a["sat"], signal=a["signal"],
                             band=BAND_OF.get(a["signal"], "?"), n=a["n"],
                             k0=a["k0"], k1=a["k1"], born_inside=a["k0"] > 1,
                             first=float(res[0]), m_early=m_early, m_late=m_late,
                             ctrl_row=float(late[j]), ctrl_late=ctrl_late,
                             rms_about_late=float(np.sqrt(((late - m_late) ** 2).mean())),
                             mean_el=float(a["el"].mean())))
        cuts = {"pooled": rows}
        for b in ("L1", "L2", "L5"):
            cuts[f"band:{b}"] = [r for r in rows if r["band"] == b]
        for s in sorted({r["signal"] for r in rows}):
            cuts[f"signal:{s}"] = [r for r in rows if r["signal"] == s]
        cuts["born_inside"] = [r for r in rows if r["born_inside"]]
        cuts["present_ep1"] = [r for r in rows if not r["born_inside"]]
        for b in ("L1", "L2", "L5"):
            cuts[f"born_inside:{b}"] = [r for r in rows if r["born_inside"] and r["band"] == b]
            cuts[f"present_ep1:{b}"] = [r for r in rows if not r["born_inside"] and r["band"] == b]
        out[arm] = {k: r1_stats(v, rng) for k, v in cuts.items()}
        out[arm]["_n_code_rows"] = int((df["kind"] == "code").sum())
        out[arm]["_n_arcs_total"] = len(arcs)
        out[arm]["_arcs_per_station_born"] = _born_per_station(rows)
        top = sorted(rows, key=lambda r: -abs(r["m_late"]))[:12]
        out[arm]["_top_level_arcs"] = [
            dict(station=r["station"], sat=r["sat"], signal=r["signal"], k0=r["k0"], k1=r["k1"],
                 n=r["n"], mean_el=round(r["mean_el"], 1), first=round(r["first"], 3),
                 m_early=round(r["m_early"], 3), m_late=round(r["m_late"], 3)) for r in top]
    return out


def _born_per_station(rows):
    c = defaultdict(int)
    for r in rows:
        if r["born_inside"]:
            c[r["station"]] += 1
    return dict(c)


R1_CUTS = ("pooled", "band:L1", "band:L2", "band:L5", "born_inside", "present_ep1",
           "born_inside:L1", "born_inside:L2", "born_inside:L5",
           "present_ep1:L1", "present_ep1:L2", "present_ep1:L5")


def _r1_line(arm, cut, s):
    return (f"{arm:8s} {cut:18s} {s['n_arcs']:6d} {f(s['frac_delta_lt_0p3'])!s:>8s} "
            f"{f(s['med_abs_delta'],2)!s:>7s} {f(s['p90_abs_delta'],2)!s:>7s} "
            f"{f(s['r_first_vs_late'])!s:>11s} {f(s['rho_first_vs_late'])!s:>8s} "
            f"{f(s['r_first_vs_late_trim5m'])!s:>8s} {f(s['r_ctrl_random_epoch'])!s:>8s} "
            f"{f(s['r_predicted_if_constant'])!s:>7s} {f(s['sigma_level_m'],2)!s:>7s} "
            f"{f(s['sigma_within_arc_m'],2)!s:>6s} {f(s['p50_abs_level_m'],2)!s:>8s} "
            f"{f(s['p99_abs_level_m'],2)!s:>8s} {f(s['max_abs_level_m'],1)!s:>8s} "
            f"{f(s['frac_level_grew'],2)!s:>6s}")


def fmt_r1(res):
    L = []
    L.append("R1 — H1: is the per-LOS level born at arc start?  (code rows; arc >= min_arc; "
             "m_early = mean of epochs 1-3, m_late = mean of epochs 10..end)")
    L.append("bar: H1 named if frac(|m_early - m_late| < 0.3 m) > 0.7 AND r(first epoch, m_late) > 0.8")
    hdr = (f"{'arm':8s} {'cut':18s} {'n_arc':>6s} {'frac<0.3':>8s} {'med|d|':>7s} {'p90|d|':>7s} "
           f"{'r(1st,late)':>11s} {'rho':>8s} {'r_trim5':>8s} {'r(ctrl)':>8s} {'r_pred':>7s} "
           f"{'sigL':>7s} {'sig_n':>6s} {'p50|lvl|':>8s} {'p99|lvl|':>8s} {'max|lvl|':>8s} {'grew':>6s}")
    L.append(hdr)
    L.append("-" * len(hdr))
    for arm, cuts in res.items():
        for cut in R1_CUTS:
            s = cuts.get(cut)
            if not s or not s.get("n_arcs"):
                continue
            L.append(_r1_line(arm, cut, s))
        L.append("")
    L.append("per-signal cuts (pooled over birth class):")
    L.append(hdr)
    for arm, cuts in res.items():
        for cut in sorted(k for k in cuts if k.startswith("signal:")):
            s = cuts[cut]
            if s.get("n_arcs"):
                L.append(_r1_line(arm, cut, s))
    L.append("")
    L.append("COLUMN NOTES")
    L.append("  r(1st,late) is the PRE-REGISTERED Pearson.  rho is its Spearman rank twin and")
    L.append("    r_trim5 the same Pearson on arcs with |level| < 5 m.  The per-LOS level is")
    L.append("    heavy-tailed, so a raw Pearson that rho and r_trim5 both refuse is carried by a")
    L.append("    handful of arcs, not by the pool — see the top-level-arc list below.")
    L.append("  r(ctrl) = the SAME correlation with the first epoch replaced by a RANDOM epoch of")
    L.append("    the late window, leave-one-out (control, not pre-registered).  r(1st,late) >>")
    L.append("    r(ctrl) is birth-specific information; r(1st,late) ~ r(ctrl) ~ r_pred is only")
    L.append("    'the level is an arc constant large against the single-epoch scatter'.")
    L.append("  r_pred = sigL/sqrt(sigL^2 + sig_n^2): what r must be if the level were an EXACT")
    L.append("    per-arc constant and sig_n the single-epoch scatter.")
    L.append("  grew = fraction of arcs with |m_late| > |m_early| (0.5 = coin flip; > 0.5 means")
    L.append("    the level is LARGER at the end of the arc than at its birth).")
    for arm, cuts in res.items():
        top = cuts.get("_top_level_arcs")
        if not top:
            continue
        L.append("")
        L.append(f"largest |settled level| arcs, {arm} (the arcs the raw Pearson is made of):")
        L.append(f"  {'station':11s} {'sat':4s} {'sig':4s} {'k0':>4s} {'k1':>4s} {'n':>4s} "
                 f"{'el':>5s} {'first':>10s} {'m_early':>10s} {'m_late':>10s}")
        for r in top:
            L.append(f"  {r['station']:11s} {r['sat']:4s} {r['signal']:4s} {r['k0']:4d} {r['k1']:4d} "
                     f"{r['n']:4d} {r['mean_el']:5.1f} {r['first']:10.3f} {r['m_early']:10.3f} "
                     f"{r['m_late']:10.3f}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R2 — is the level ionospheric?
# --------------------------------------------------------------------------- #
def settled_levels(df, kmin=K_SETTLED_LO, kmax=K_SETTLED_HI, min_rows=30):
    """-> DataFrame (station, sat, signal, level_m, n, mean_el, sum_w)."""
    s = df[(df["kind"] == "code") & (df["k"] >= kmin) & (df["k"] <= kmax)]
    g = s.groupby(["station", "sat", "signal"], observed=True).agg(
        level_m=("resid_m", "mean"), n=("resid_m", "size"),
        mean_el=("elev_deg", "mean"), sum_w=("w", "sum"))
    g = g[g["n"] >= min_rows].reset_index()
    return g


def sh_coeffs_by_epoch(project, arm, kmin, kmax):
    p = os.path.join(arm_dir(project, arm), "logs", "network_vtec_sh.csv")
    df = pd.read_csv(p)
    if "layer" not in df.columns:
        df = df.assign(layer=0)
    df["k"] = np.rint((df["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    return df[(df["k"] >= kmin) & (df["k"] <= kmax)]


def sh_predict(iono_df, sh_df, stations, shcfg):
    """Per row of iono_df: Σ_l M_l·tecu_to_L1m·VTEC_l(IPP_l)  (metres on L1).

    A faithful evaluation of the tie row the producer folds
    (ApplicationNetworkAssembly.cpp:1090-1097): the IPP geometry is
    computeIppGeometry(st.ecef, sat, Re, H_l) re-derived from the el/az the
    producer recorded for the same (station, satellite, epoch).
    """
    Re = shcfg["earth_radius_km"] * 1000.0
    lat0 = np.array([math.radians(stations[s]["lat_deg"]) for s in iono_df["station"]])
    lon0 = np.array([math.radians(stations[s]["lon_deg"]) for s in iono_df["station"]])
    el = np.radians(iono_df["elevation_deg"].to_numpy(float))
    az = np.radians(iono_df["azimuth_deg"].to_numpy(float))
    kk = iono_df["k"].to_numpy()
    week = int(iono_df["week"].iloc[0])
    tow = iono_df["tow"].to_numpy(float)

    pred = np.zeros(len(iono_df))
    map0 = np.zeros(len(iono_df))
    for layer, H_km in sorted(shcfg["layers"].items()):
        H = H_km * 1000.0
        sin_zp = (Re / (Re + H)) * np.cos(el)
        cos_zp = np.sqrt(np.maximum(0.0, 1.0 - sin_zp ** 2))
        mapping = np.where(cos_zp > 1e-6, 1.0 / np.maximum(cos_zp, 1e-9), np.nan)
        psi = (math.pi / 2.0) - el - np.arcsin(np.clip(sin_zp, -1, 1))
        lat_i = np.arcsin(np.clip(np.sin(lat0) * np.cos(psi)
                                  + np.cos(lat0) * np.sin(psi) * np.cos(az), -1, 1))
        lon_i = lon0 + np.arcsin(np.clip(np.sin(psi) * np.sin(az)
                                         / np.maximum(1e-9, np.cos(lat_i)), -1, 1))
        if layer == 0:
            map0 = mapping
        lay = sh_df[sh_df["layer"] == layer]
        if lay.empty:
            continue
        nmax = int(lay["n"].max())
        # sun-fixed longitude: one lon_sun per epoch
        lon_sun_by_k = {int(k): (mean_sun_longitude(week, float(t)) if shcfg["sun_fixed"] else 0.0)
                        for k, t in zip(lay["k"].to_numpy(), lay["tow"].to_numpy())}
        lon_sun = np.array([lon_sun_by_k.get(int(k), np.nan) for k in kk])
        s_lon = lon_i - lon_sun
        pbar = legendre_normalized(np.sin(lat_i), nmax)
        # coefficients indexed by epoch
        acc = np.zeros(len(iono_df))
        for (n, m, trig), grp in lay.groupby(["n", "m", "trig"], observed=True):
            vals = dict(zip(grp["k"].to_numpy().astype(int), grp["value_tecu"].to_numpy(float)))
            c = np.array([vals.get(int(k), np.nan) for k in kk])
            ang = np.cos(m * s_lon) if trig == "C" else np.sin(m * s_lon)
            acc = acc + c * pbar[(int(n), int(m))] * ang
        pred = pred + mapping * TECU_TO_L1M * acc
    return pred, map0


def read_r2(project, arms, min_rows, control_arm="_cm_pvb", contrast_arm="_cm_b7"):
    out = {}
    lev_cache = {}
    for arm in arms:
        df = load_pf_rows(project, arm)
        lev = settled_levels(df, min_rows=min_rows)
        lev_cache[arm] = lev
        A = {}

        # (a) cross-band ratio / correlation per LOS ---------------------------
        piv = lev.pivot_table(index=["station", "sat"], columns="signal",
                              values="level_m", observed=True)
        nel = lev.groupby(["station", "sat"], observed=True)["mean_el"].mean()
        for num, den, gam in (("C2W", "C1C", GAMMA_L2), ("C2W", "C1W", GAMMA_L2),
                              ("C1W", "C1C", 1.0), ("C5Q", "C1C", GAMMA_L5),
                              ("C5X", "C1C", GAMMA_L5)):
            if num not in piv.columns or den not in piv.columns:
                continue
            m = piv[num].notna() & piv[den].notna()
            x, y = piv.loc[m, den].to_numpy(float), piv.loc[m, num].to_numpy(float)
            if len(x) < 5:
                continue
            with np.errstate(divide="ignore", invalid="ignore"):
                ratio = np.where(np.abs(x) > 1e-9, y / x, np.nan)
            s_free, c_free, r = ols_slope(x, y)
            s_orig = float((x @ y) / (x @ x)) if (x @ x) > 0 else float("nan")
            # SAME outlier guard as R1: a handful of metre-to-100 m LOS can carry a
            # Pearson r and an OLS slope on their own, so every one is repeated on the
            # |level| < 5 m trim and beside its Spearman rank twin.
            tm = (np.abs(x) < 5.0) & (np.abs(y) < 5.0)
            s_t, c_t, r_t = ols_slope(x[tm], y[tm]) if tm.sum() >= 5 else (
                float("nan"),) * 3
            A[f"ratio:{num}/{den}"] = dict(
                n_los=int(len(x)), gamma=gam,
                med_ratio=q(ratio, 0.5), p25_ratio=q(ratio, 0.25), p75_ratio=q(ratio, 0.75),
                slope_free=s_free, intercept_free=c_free, slope_through_origin=s_orig,
                r=r, rho=spearman(x, y),
                n_trim5m=int(tm.sum()), slope_trim5m=s_t, r_trim5m=r_t,
                frac_ratio_in_gamma_pm_0p3=float(np.nanmean(
                    (ratio > gam - 0.3) & (ratio < gam + 0.3))),
                med_level_num=float(np.median(y)), med_level_den=float(np.median(x)))

        # (b) level vs the SH tie residual -------------------------------------
        stations = load_stations(project, arm)
        shcfg = load_sh_cfg(project, arm)
        ion = load_iono(project, arm)
        ion = ion[(ion["k"] >= K_SETTLED_LO) & (ion["k"] <= K_SETTLED_HI)].copy()
        ion = ion[ion["station"].isin(stations.keys())]
        sh = sh_coeffs_by_epoch(project, arm, K_SETTLED_LO, K_SETTLED_HI)
        pred, map0 = sh_predict(ion, sh, stations, shcfg)
        ion["sh_pred_m"] = pred
        ion["map0"] = map0
        ion["tie_raw_m"] = ion["iono_total_m"].to_numpy(float) - pred
        # the per-(station, epoch) mean is the free datum the all-ones R block leaves
        ion["tie_dm_m"] = ion["tie_raw_m"] - ion.groupby(
            ["station", "k"], observed=True)["tie_raw_m"].transform("mean")
        tie = ion.groupby(["station", "sat"], observed=True).agg(
            tie_raw=("tie_raw_m", "mean"), tie_dm=("tie_dm_m", "mean"),
            iono_abs=("iono_total_m", "mean"), sh_pred=("sh_pred_m", "mean"),
            map0=("map0", "mean"), n_ion=("tie_raw_m", "size")).reset_index()
        # The SH tie row is ADMITTED only above `estimation.ionosphere_weighting.
        # elevation_cutoff_deg` (ApplicationNetworkAssembly.cpp:1044 — `if (o.elevation <
        # iw.elevation_cutoff_deg) continue;`), while the processing mask admits the
        # OBSERVATION from `processing.elevation_mask_deg`.  Between the two cutoffs a
        # LOS's slant-iono state carries code/phase rows with NO tie: an admission valve
        # in the sense of standing invariant #7.  Split every tie quantity there.
        cut = float(json.load(open(os.path.join(arm_dir(project, arm), "arm.json")))
                    .get("estimation", {}).get("ionosphere_weighting", {})
                    .get("elevation_cutoff_deg", 25.0))
        mask = float(json.load(open(os.path.join(arm_dir(project, arm), "arm.json")))
                     .get("processing", {}).get("elevation_mask_deg", 15.0))
        below = ion["elevation_deg"].to_numpy(float) < cut
        A["_tie_cutoff_split"] = dict(
            tie_elev_cutoff_deg=cut, processing_elev_mask_deg=mask,
            n_rows_below=int(below.sum()), n_rows_above=int((~below).sum()),
            frac_rows_below=float(below.mean()),
            **{f"{side}_{stat}": v for side, m in (("below", below), ("above", ~below))
               for stat, v in (
                   ("tie_raw_p50abs", q(np.abs(ion["tie_raw_m"].to_numpy()[m]), .5)),
                   ("tie_raw_p99abs", q(np.abs(ion["tie_raw_m"].to_numpy()[m]), .99)),
                   ("tie_raw_max_abs", float(np.abs(ion["tie_raw_m"].to_numpy()[m]).max())
                    if m.any() else float("nan")),
                   ("iono_abs_p99", q(ion["iono_total_m"].to_numpy()[m], .99)),
                   ("iono_abs_max", float(ion["iono_total_m"].to_numpy()[m].max())
                    if m.any() else float("nan")))})
        lev_el = lev["mean_el"].to_numpy(float)
        lb = lev_el < cut
        A["_level_by_tie_cutoff"] = {
            side: dict(n_los=int(m.sum()),
                       med_abs_level=q(np.abs(lev["level_m"].to_numpy()[m]), .5),
                       p90_abs_level=q(np.abs(lev["level_m"].to_numpy()[m]), .90),
                       p99_abs_level=q(np.abs(lev["level_m"].to_numpy()[m]), .99),
                       max_abs_level=float(np.abs(lev["level_m"].to_numpy()[m]).max())
                       if m.any() else float("nan"))
            for side, m in (("below_cutoff", lb), ("above_cutoff", ~lb))}
        A["_tie_pool"] = dict(
            n_rows=int(len(ion)),
            tie_raw_p50=q(ion["tie_raw_m"], .5), tie_raw_p90abs=q(np.abs(ion["tie_raw_m"]), .9),
            tie_dm_p50=q(ion["tie_dm_m"], .5), tie_dm_p90abs=q(np.abs(ion["tie_dm_m"]), .9),
            iono_abs_p50=q(ion["iono_total_m"], .5), sh_pred_p50=q(ion["sh_pred_m"], .5),
            sigma_tie_tecu=shcfg["representation_sigma_tecu"])

        lv = lev.merge(tie, on=["station", "sat"], how="inner")
        # signal-common level per LOS (the non-dispersive part) for a third cut
        sc = lev.groupby(["station", "sat"], observed=True)["level_m"].mean().rename(
            "level_common").reset_index()
        lvc = sc.merge(tie, on=["station", "sat"], how="inner")
        # A station reports EITHER C1C or C1W as its L1 code (C/A vs the P(Y)-derived
        # W code), so the C2W/C1C and C2W/C1W ratios above are computed on DISJOINT
        # station populations and are not comparable without this split.  The wire's
        # code-bias datum is pinned on C1C-C2W (`cbpin=G:C1C-C2W` in the S line).
        sta_l1 = {}
        for s, g in lev.groupby("station", observed=True):
            sigs = set(g["signal"])
            sta_l1[s] = "C1C-sta" if "C1C" in sigs else ("C1W-sta" if "C1W" in sigs else "other")
        A["_n_sta_by_l1"] = {k: int(sum(1 for v in sta_l1.values() if v == k))
                             for k in ("C1C-sta", "C1W-sta", "other")}
        lev_fam = lev.assign(fam=lev["station"].map(sta_l1))
        A["_level_by_family"] = {
            f"{fm}|{sg}": dict(n=int(len(g)), med_abs_level=float(g["level_m"].abs().median()),
                               rms_level=float(np.sqrt((g["level_m"] ** 2).mean())))
            for (fm, sg), g in lev_fam.groupby(["fam", "signal"], observed=True)}
        cuts = [("C1C", lv[lv["signal"] == "C1C"]), ("C2W", lv[lv["signal"] == "C2W"]),
                ("C1W", lv[lv["signal"] == "C1W"]),
                ("signal-common", lvc.assign(level_m=lvc["level_common"]))]
        fam = lv["station"].map(sta_l1)
        famc = lvc["station"].map(sta_l1)
        cuts += [("C2W@C1C-sta", lv[(lv["signal"] == "C2W") & (fam == "C1C-sta")]),
                 ("C2W@C1W-sta", lv[(lv["signal"] == "C2W") & (fam == "C1W-sta")]),
                 ("common@C1C-sta", lvc[famc == "C1C-sta"].assign(
                     level_m=lvc[famc == "C1C-sta"]["level_common"])),
                 ("common@C1W-sta", lvc[famc == "C1W-sta"].assign(
                     level_m=lvc[famc == "C1W-sta"]["level_common"]))]
        for tag, sel in cuts:
            if len(sel) < 5:
                continue
            for tk in ("tie_raw", "tie_dm"):
                xt = sel[tk].to_numpy(float)
                yt = sel["level_m"].to_numpy(float)
                s, c, r = ols_slope(xt, yt)
                tm = (np.abs(yt) < 5.0) & (np.abs(xt) < 20.0)
                s_t, c_t, r_t = ols_slope(xt[tm], yt[tm]) if tm.sum() >= 5 else (float("nan"),) * 3
                A[f"tie:{tag}:{tk}"] = dict(n_los=int(len(sel)), slope=s, intercept=c, r=r,
                                            rho=spearman(xt, yt), n_trim=int(tm.sum()),
                                            slope_trim=s_t, r_trim=r_t)

        out[arm] = A
        out[arm]["_n_los_levels"] = int(len(lev))

    # (c) b7 - control per-LOS level change vs elevation / station latitude -----
    if control_arm in lev_cache and contrast_arm in lev_cache:
        a = lev_cache[control_arm].set_index(["station", "sat", "signal"])
        b = lev_cache[contrast_arm].set_index(["station", "sat", "signal"])
        j = a.join(b, lsuffix="_ctl", rsuffix="_b7", how="inner")
        stations = load_stations(project, control_arm)
        lat = np.array([stations[s]["lat_deg"] for s, _, _ in j.index])
        d = (j["level_m_b7"] - j["level_m_ctl"]).to_numpy(float)
        el = j["mean_el_ctl"].to_numpy(float)
        eb = [(10, 20), (20, 30), (30, 45), (45, 60), (60, 90.1)]
        lb = [(-90, -45), (-45, -20), (-20, 20), (20, 45), (45, 90.1)]
        tbl_el, tbl_lat = [], []
        for lo, hi in eb:
            m = (el >= lo) & (el < hi)
            if m.sum():
                tbl_el.append(dict(bin=f"{lo}-{hi:.0f}", n=int(m.sum()),
                                   med_dlevel=float(np.median(d[m])),
                                   med_abs_dlevel=float(np.median(np.abs(d[m]))),
                                   med_level_ctl=float(np.median(j["level_m_ctl"].to_numpy()[m]))))
        for lo, hi in lb:
            m = (lat >= lo) & (lat < hi)
            if m.sum():
                tbl_lat.append(dict(bin=f"{lo}..{hi:.0f}", n=int(m.sum()),
                                    med_dlevel=float(np.median(d[m])),
                                    med_abs_dlevel=float(np.median(np.abs(d[m]))),
                                    med_level_ctl=float(np.median(j["level_m_ctl"].to_numpy()[m]))))
        out["__delta_b7_minus_ctl"] = dict(n_los_matched=int(len(j)),
                                          med_dlevel=float(np.median(d)),
                                          med_abs_dlevel=float(np.median(np.abs(d))),
                                          r_level=pearson(j["level_m_ctl"], j["level_m_b7"]),
                                          by_elev=tbl_el, by_lat=tbl_lat)
    return out


def fmt_r2(res):
    L = []
    L.append("R2 — H2: is the per-LOS level ionospheric?  (settled window k 121..240)")
    L.append(f"bar: H2 named if level_C2W/level_C1C within {GAMMA_L2:.4f} +- 0.3 with r > 0.8, "
             "OR tie-residual r > 0.5")
    L.append("")
    hdr = (f"{'arm':8s} {'pair':14s} {'gamma':>6s} {'n':>5s} {'med_ratio':>9s} {'p25..p75':>15s} "
           f"{'slope_free':>10s} {'slope_0':>8s} {'r':>7s} {'rho':>7s} {'n_tr':>5s} "
           f"{'slp_tr':>7s} {'r_tr':>7s} {'frac_in_band':>12s}")
    L.append(hdr)
    L.append("-" * len(hdr))
    for arm, A in res.items():
        if arm.startswith("__"):
            continue
        for k in sorted(x for x in A if x.startswith("ratio:")):
            s = A[k]
            L.append(f"{arm:8s} {k[6:]:14s} {s['gamma']:6.3f} {s['n_los']:5d} "
                     f"{f(s['med_ratio'],2)!s:>9s} "
                     f"{(f(s['p25_ratio'],2) + '..' + f(s['p75_ratio'],2)):>15s} "
                     f"{f(s['slope_free'],3)!s:>10s} {f(s['slope_through_origin'],3)!s:>8s} "
                     f"{f(s['r'])!s:>7s} {f(s['rho'])!s:>7s} {s['n_trim5m']:5d} "
                     f"{f(s['slope_trim5m'],3)!s:>7s} {f(s['r_trim5m'])!s:>7s} "
                     f"{f(s['frac_ratio_in_gamma_pm_0p3'],2)!s:>12s}")
    L.append("NOTE: a station reports EITHER C1C or C1W as its L1 code, so C2W/C1C and C2W/C1W")
    L.append("  sit on DISJOINT station populations.  Station counts by L1 family:")
    for arm, A in res.items():
        if not arm.startswith("__") and "_n_sta_by_l1" in A:
            L.append(f"  {arm:8s} {A['_n_sta_by_l1']}")
            fam = A.get("_level_by_family", {})
            for k in sorted(fam):
                s = fam[k]
                L.append(f"    {k:16s} n {s['n']:5d}  med|level| {f(s['med_abs_level'],3)}  "
                         f"rms {f(s['rms_level'],3)}")
    L.append("")
    hdr = (f"{'arm':8s} {'level':14s} {'tie form':9s} {'n':>5s} {'slope':>8s} {'intercept':>10s} "
           f"{'r':>7s} {'rho':>7s} {'n_tr':>5s} {'slp_tr':>7s} {'r_tr':>7s}")
    L.append(hdr)
    L.append("-" * len(hdr))
    for arm, A in res.items():
        if arm.startswith("__"):
            continue
        for k in sorted(x for x in A if x.startswith("tie:")):
            _, tag, tk = k.split(":")
            s = A[k]
            L.append(f"{arm:8s} {tag:14s} {tk:9s} {s['n_los']:5d} {f(s['slope'])!s:>8s} "
                     f"{f(s['intercept'])!s:>10s} {f(s['r'])!s:>7s} {f(s['rho'])!s:>7s} "
                     f"{s['n_trim']:5d} {f(s['slope_trim'])!s:>7s} {f(s['r_trim'])!s:>7s}")
    L.append("")
    L.append("tie pool (metres on L1; tie_raw = iono_total - SH(IPP), tie_dm = same minus the")
    L.append("  per-(station,epoch) mean, which the all-ones R block leaves as a free datum):")
    for arm, A in res.items():
        if arm.startswith("__") or "_tie_pool" not in A:
            continue
        s = A["_tie_pool"]
        L.append(f"  {arm:8s} rows {s['n_rows']:6d}  sigma_rep {s['sigma_tie_tecu']}  "
                 f"iono_abs p50 {f(s['iono_abs_p50'],2)}  SH p50 {f(s['sh_pred_p50'],2)}  "
                 f"tie_raw p50 {f(s['tie_raw_p50'],2)} p90|.| {f(s['tie_raw_p90abs'],2)}  "
                 f"tie_dm p50 {f(s['tie_dm_p50'],2)} p90|.| {f(s['tie_dm_p90abs'],2)}")
    L.append("")
    L.append("SH-TIE ADMISSION SPLIT — the tie row is appended only above")
    L.append("  estimation.ionosphere_weighting.elevation_cutoff_deg; the OBSERVATION is admitted")
    L.append("  from processing.elevation_mask_deg.  Between the two an LOS's slant-iono state is")
    L.append("  untied (invariant #7 admission valve).  Settled-window rows / LOS:")
    for arm, A in res.items():
        if arm.startswith("__") or "_tie_cutoff_split" not in A:
            continue
        s = A["_tie_cutoff_split"]
        L.append(f"  {arm:8s} tie cutoff {s['tie_elev_cutoff_deg']:.0f} deg vs obs mask "
                 f"{s['processing_elev_mask_deg']:.0f} deg; {s['n_rows_below']} of "
                 f"{s['n_rows_below'] + s['n_rows_above']} iono rows "
                 f"({f(s['frac_rows_below'],3)}) are BELOW the tie cutoff")
        for side in ("below", "above"):
            L.append(f"      {side:5s}  |tie_raw| p50 {f(s[side + '_tie_raw_p50abs'],2)} "
                     f"p99 {f(s[side + '_tie_raw_p99abs'],2)} max {f(s[side + '_tie_raw_max_abs'],1)} | "
                     f"iono_abs p99 {f(s[side + '_iono_abs_p99'],2)} max {f(s[side + '_iono_abs_max'],1)}")
        for side, v in A.get("_level_by_tie_cutoff", {}).items():
            L.append(f"      {side:13s} n_LOS {v['n_los']:4d}  med|level| {f(v['med_abs_level'],3)}  "
                     f"p90 {f(v['p90_abs_level'],2)}  p99 {f(v['p99_abs_level'],2)}  "
                     f"max {f(v['max_abs_level'],1)}")
    d = res.get("__delta_b7_minus_ctl")
    if d:
        L.append("")
        L.append(f"per-LOS Delta level (b7 minus PV-B), {d['n_los_matched']} matched LOS-signals: "
                 f"med {f(d['med_dlevel'],3)} m, med|.| {f(d['med_abs_dlevel'],3)} m, "
                 f"r(level_ctl, level_b7) {f(d['r_level'])}")
        L.append(f"  {'elev bin':>10s} {'n':>6s} {'med dlevel':>11s} {'med|dlevel|':>12s} {'med level ctl':>14s}")
        for r in d["by_elev"]:
            L.append(f"  {r['bin']:>10s} {r['n']:6d} {f(r['med_dlevel'],3)!s:>11s} "
                     f"{f(r['med_abs_dlevel'],3)!s:>12s} {f(r['med_level_ctl'],3)!s:>14s}")
        L.append(f"  {'lat bin':>10s} {'n':>6s} {'med dlevel':>11s} {'med|dlevel|':>12s} {'med level ctl':>14s}")
        for r in d["by_lat"]:
            L.append(f"  {r['bin']:>10s} {r['n']:6d} {f(r['med_dlevel'],3)!s:>11s} "
                     f"{f(r['med_abs_dlevel'],3)!s:>12s} {f(r['med_level_ctl'],3)!s:>14s}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R3 — station geometry
# --------------------------------------------------------------------------- #
_ANT_CACHE = {}


def rinex_antenna_delta(project, station):
    """ANTENNA: DELTA H/E/N from data/2024-129/obs/<station>/*.rnx -> (H, E, N) or None."""
    if (project, station) in _ANT_CACHE:
        return _ANT_CACHE[(project, station)]
    _ANT_CACHE[(project, station)] = None
    d = os.path.join(project, "data", "2024-129", "obs", station.lower())
    if not os.path.isdir(d):
        return None
    for fn in sorted(os.listdir(d)):
        if not fn.lower().endswith((".rnx", ".obs", ".24o")):
            continue
        try:
            with open(os.path.join(d, fn), "r", errors="replace") as fh:
                for _ in range(120):
                    line = fh.readline()
                    if not line:
                        break
                    if line[60:].startswith("ANTENNA: DELTA H/E/N"):
                        v = (float(line[0:14]), float(line[14:28]), float(line[28:42]))
                        _ANT_CACHE[(project, station)] = v
                        return v
                    if line[60:].startswith("END OF HEADER"):
                        break
        except Exception:
            continue
    return None


def read_r3(project, arms, min_rows, min_los=6):
    out = {}
    for arm in arms:
        df = load_pf_rows(project, arm)
        lev = settled_levels(df, min_rows=min_rows)
        stations = load_stations(project, arm)
        ion = load_iono(project, arm)
        ion = ion[(ion["k"] >= K_SETTLED_LO) & (ion["k"] <= K_SETTLED_HI)]
        ga = ion.groupby(["station", "sat"], observed=True).agg(
            el=("elevation_deg", "mean"), az=("azimuth_deg", "mean")).reset_index()

        # per-LOS signal-common level (geometry is non-dispersive) + per-signal C1C
        common = lev.groupby(["station", "sat"], observed=True).agg(
            level=("level_m", "mean"), wsum=("sum_w", "sum"),
            nsig=("signal", "size")).reset_index()
        c1c = lev[lev["signal"] == "C1C"][["station", "sat", "level_m", "sum_w"]].rename(
            columns={"level_m": "level", "sum_w": "wsum"})

        zwd = pd.read_csv(os.path.join(arm_dir(project, arm), "logs", "network_zwd.csv"))
        zwd["k"] = np.rint((zwd["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
        zl = zwd[(zwd["k"] >= K_SETTLED_LO) & (zwd["k"] <= K_SETTLED_HI)]
        zmed = zl.groupby("station")["zwd_m"].median().to_dict()

        tab = {}
        for tag, src in (("signal-common", common), ("C1C", c1c)):
            m = src.merge(ga, on=["station", "sat"], how="inner")
            rows = []
            for sta, grp in m.groupby("station", observed=True):
                if len(grp) < min_los:
                    continue
                el = np.radians(grp["el"].to_numpy(float))
                az = np.radians(grp["az"].to_numpy(float))
                E = np.cos(el) * np.sin(az)
                N = np.cos(el) * np.cos(az)
                U = np.sin(el)
                y = grp["level"].to_numpy(float)
                w = grp["wsum"].to_numpy(float)
                w = w / w.mean()
                A = np.column_stack([np.ones_like(E), E, N, U])
                W = np.sqrt(w)[:, None]
                sol, *_ = np.linalg.lstsq(A * W, y * np.sqrt(w), rcond=None)
                resid = y - A @ sol
                ybar = float((w * y).sum() / w.sum())
                ss_tot_about_mean = float((w * (y - ybar) ** 2).sum())
                ss_res = float((w * resid ** 2).sum())
                ss_tot_about_0 = float((w * y ** 2).sum())
                d = sol[1:]
                rows.append(dict(
                    station=str(sta), n_los=int(len(grp)),
                    a_m=float(sol[0]), dE=float(d[0]), dN=float(d[1]), dU=float(d[2]),
                    d_norm=float(np.linalg.norm(d)),
                    ss_frac_d_beyond_const=float(1.0 - ss_res / ss_tot_about_mean)
                    if ss_tot_about_mean > 0 else float("nan"),
                    # NULL: three free direction parameters on n points explain
                    # 3/(n-1) of the variance about the mean by chance alone, which at
                    # n ~ 9 is 0.375.  The adjusted form is the honest statistic.
                    ss_null_expected=float(3.0 / (len(grp) - 1)) if len(grp) > 1 else float("nan"),
                    ss_frac_d_adj=float(1.0 - (ss_res / ss_tot_about_mean)
                                        * (len(grp) - 1) / (len(grp) - 4))
                    if ss_tot_about_mean > 0 and len(grp) > 4 else float("nan"),
                    ss_frac_full_model=float(1.0 - ss_res / ss_tot_about_0)
                    if ss_tot_about_0 > 0 else float("nan"),
                    rms_resid_m=float(np.sqrt((w * resid ** 2).sum() / w.sum())),
                    rms_level_m=float(np.sqrt((w * y ** 2).sum() / w.sum())),
                    zwd_med_m=float(zmed.get(str(sta), float("nan"))),
                    lat_deg=stations[str(sta)]["lat_deg"] if str(sta) in stations else float("nan"),
                    delta_hen=rinex_antenna_delta(project, str(sta)),
                ))
            rows.sort(key=lambda r: -abs(r["zwd_med_m"]))
            # the pre-registered "alongside the RINEX ANTENNA: DELTA H/E/N": if the
            # producer were working in the wrong (marker vs ARP) vertical frame, the
            # implied height error -dU would track the header's DELTA H.
            dh = np.array([r["delta_hen"][0] if r["delta_hen"] else float("nan") for r in rows])
            du = np.array([r["dU"] for r in rows])
            tab[tag] = rows
            tab[f"_{tag}_delta_h_check"] = dict(
                n=int(np.isfinite(dh).sum()),
                n_delta_h_gt_0p3m=int((dh > 0.3).sum()),
                max_delta_h_m=float(np.nanmax(dh)) if np.isfinite(dh).any() else float("nan"),
                med_abs_d_norm=float(np.median([r["d_norm"] for r in rows])),
                r_minus_dU_vs_delta_h=pearson(-du, dh),
                rho_minus_dU_vs_delta_h=spearman(-du, dh),
                # the pre-registered pairing of |d| with |ZWD|, as a correlation
                r_dU_vs_zwd=pearson(du, [r["zwd_med_m"] for r in rows]),
                rho_dU_vs_zwd=spearman(du, [r["zwd_med_m"] for r in rows]),
                rho_absd_vs_abszwd=spearman([r["d_norm"] for r in rows],
                                            [abs(r["zwd_med_m"]) for r in rows]))
        out[arm] = tab
        # the FULL roster's settled |ZWD| (the 13-station flag set of §24.11/§24.12),
        # independent of whether a station carried enough LOS for a fit.
        out[arm]["_zwd_roster"] = {str(k): float(v) for k, v in sorted(
            zmed.items(), key=lambda kv: -abs(kv[1]))}
        out[arm]["_n_zwd_gt_0p5"] = int(sum(1 for v in zmed.values() if abs(v) > 0.5))
        out[arm]["_n_stations_roster"] = int(len(zmed))
    return out


def fmt_r3(res):
    L = []
    L.append("R3 — H3: do the per-LOS levels fit a station position vector + clock?")
    L.append("  level_LOS ~ a + d.e_LOS, weighted by the LOS's summed normal-equation weight w;")
    L.append("  e_LOS from the producer's own el/az (network_ionosphere.csv), local ENU;")
    L.append("  an implied station position error is delta = -d (so implied height error = -dU).")
    L.append("bar: H3 named if the |ZWD| > 0.5 m stations carry |d| > 1 m with a consistent")
    L.append("  vertical while the others do not.")
    for arm, tab in res.items():
        L.append("")
        L.append(f"{arm}: roster {tab.get('_n_stations_roster')} stations, "
                 f"{tab.get('_n_zwd_gt_0p5')} with settled |ZWD| > 0.5 m — "
                 + ", ".join(f"{k} {v:+.2f}" for k, v in
                             list(tab.get("_zwd_roster", {}).items())[:13]))
        for tag, rows in tab.items():
            if tag.startswith("_") or not rows:
                continue
            flagged = [r for r in rows if abs(r["zwd_med_m"]) > 0.5]
            other = [r for r in rows if abs(r["zwd_med_m"]) <= 0.5]
            L.append("")
            L.append(f"=== {arm}  [{tag} levels]  {len(rows)} stations, "
                     f"{len(flagged)} with |ZWD| > 0.5 m ===")
            hdr = (f"{'station':11s} {'nLOS':>4s} {'ZWD_m':>7s} {'a_m':>7s} {'dE':>7s} {'dN':>7s} "
                   f"{'dU':>7s} {'|d|':>7s} {'SSd':>6s} {'SSadj':>6s} {'SSnul':>6s} {'rms_r':>6s} "
                   f"{'rms_lvl':>7s} {'DELTA H/E/N':>22s}")
            L.append(hdr)
            L.append("-" * len(hdr))
            for r in rows:
                dh = r["delta_hen"]
                ds = "n/a" if dh is None else f"{dh[0]:.4f}/{dh[1]:.4f}/{dh[2]:.4f}"
                mark = "*" if abs(r["zwd_med_m"]) > 0.5 else " "
                L.append(f"{r['station']:11s}{mark}{r['n_los']:4d} {f(r['zwd_med_m'],2)!s:>7s} "
                         f"{f(r['a_m'],2)!s:>7s} {f(r['dE'],2)!s:>7s} {f(r['dN'],2)!s:>7s} "
                         f"{f(r['dU'],2)!s:>7s} {f(r['d_norm'],2)!s:>7s} "
                         f"{f(r['ss_frac_d_beyond_const'],2)!s:>6s} {f(r['ss_frac_d_adj'],2)!s:>6s} "
                         f"{f(r['ss_null_expected'],2)!s:>6s} "
                         f"{f(r['rms_resid_m'],2)!s:>6s} {f(r['rms_level_m'],2)!s:>7s} {ds:>22s}")
            for name, grp in (("|ZWD|>0.5", flagged), ("|ZWD|<=0.5", other)):
                if not grp:
                    continue
                L.append(f"  {name:11s} n={len(grp):3d}  med|d| {f(np.median([abs(r['d_norm']) for r in grp]),2)}"
                         f"  frac |d|>1m {f(np.mean([abs(r['d_norm']) > 1.0 for r in grp]),2)}"
                         f"  med dU {f(np.median([r['dU'] for r in grp]),2)}"
                         f"  frac dU>0 {f(np.mean([r['dU'] > 0 for r in grp]),2)}"
                         f"  med SSd {f(np.median([r['ss_frac_d_beyond_const'] for r in grp]),2)}"
                         f" (null {f(np.median([r['ss_null_expected'] for r in grp]),2)},"
                         f" adj {f(np.median([r['ss_frac_d_adj'] for r in grp]),2)})"
                         f"  med rms_lvl {f(np.median([r['rms_level_m'] for r in grp]),2)}")
            c = tab.get(f"_{tag}_delta_h_check")
            if c:
                L.append(f"  ANTENNA DELTA H check: {c['n']} headers read, "
                         f"{c['n_delta_h_gt_0p3m']} with DELTA H > 0.3 m (max {f(c['max_delta_h_m'],3)} m); "
                         f"med |d| = {f(c['med_abs_d_norm'],2)} m; "
                         f"r(-dU, DELTA H) = {f(c['r_minus_dU_vs_delta_h'])}, "
                         f"rho = {f(c['rho_minus_dU_vs_delta_h'])}")
                L.append(f"  ZWD pairing: r(dU, ZWD) = {f(c['r_dU_vs_zwd'])}, "
                         f"rho(dU, ZWD) = {f(c['rho_dU_vs_zwd'])}, "
                         f"rho(|d|, |ZWD|) = {f(c['rho_absd_vs_abszwd'])}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R4 — wire D_phase steps at arc births (conditional on R1 naming H1)
# --------------------------------------------------------------------------- #
def read_r4(project, arms, gap_epochs, min_arc, step_thresh=0.3, n_shuffle=1000,
            seed=20260914, seed_run="_cm_pvb"):
    import wire_vs_code_census as wc
    rng = np.random.default_rng(seed)
    clk_paths, bia, doy_key, _ = wc.resolve_code_products(
        os.path.join(arm_dir(project, seed_run), "seed.json"))
    clk, n_clk = wc.parse_clk(clk_paths)
    osb, _sigs = wc.parse_bia(bia, doy_key)
    if not clk_paths or not bia:
        raise SystemExit("CODE products not found (seed.json paths)")
    out = {}
    for arm in arms:
        rd = arm_dir(project, arm)
        epochs, meta = wc.parse_products(os.path.join(rd, "products.txt"))
        raw, disc, ev, cnt, miss, no_clk, tows = wc.collect_raw(epochs, clk, osb)
        cm = wc.common_mode(raw["phase"], tows, "median")
        series = wc.demean(raw["phase"], disc["phase"], cm)

        df = load_pf_rows(project, arm)
        arcs = build_arcs(df, "code", gap_epochs, min_arc)
        births = defaultdict(set)
        for a in arcs:
            if a["k0"] > 1:
                births[a["sat"]].add(int(a["k0"]))

        all_k = sorted({int(round((t - TOW0) / DT_S)) + 1 for t in tows})
        k_lo, k_hi = min(all_k), max(all_k)
        per_sat = {}
        hit_tot = step_tot = 0
        null_tot = []
        for (sat, sig), ser in series.items():
            arcs_w = wc.split_arcs(ser, ev.get(sat, {}), gap_s=120.0, declared=False)
            steps = []          # (k_of_second_epoch, |dD|)
            for idx in arcs_w:
                for p, qq in zip(idx[:-1], idx[1:]):
                    t0, d0, _ = ser[p]
                    t1, d1, _ = ser[qq]
                    steps.append((int(round((t1 - TOW0) / DT_S)) + 1, abs(d1 - d0)))
            if not steps:
                continue
            big = np.array([k for k, v in steps if v > step_thresh])
            ks = np.array([k for k, _ in steps])
            b = np.array(sorted(births.get(sat, ())), dtype=int)
            if b.size == 0:
                continue

            def frac_near(bset):
                if big.size == 0:
                    return 0.0
                near = np.isin(big, bset) | np.isin(big - 1, bset) | np.isin(big + 1, bset)
                return float(near.mean())

            obs = frac_near(b)
            cov = float(np.mean(np.isin(ks, b) | np.isin(ks - 1, b) | np.isin(ks + 1, b)))
            null = np.empty(n_shuffle)
            span = np.arange(k_lo, k_hi + 1)
            for i in range(n_shuffle):
                null[i] = frac_near(rng.choice(span, size=b.size, replace=False))
            per_sat[f"{sat}|{sig}"] = dict(
                n_steps=int(len(steps)), n_big=int(big.size), n_births=int(b.size),
                frac_near_birth=obs, coverage=cov,
                null_mean=float(null.mean()), null_p95=float(np.quantile(null, 0.95)))
            hit_tot += int(np.isin(big, b).sum() + 0)
            step_tot += int(big.size)
            null_tot.append(null.mean())
        agg = dict(
            n_series=len(per_sat),
            med_frac_near_birth=q([v["frac_near_birth"] for v in per_sat.values()], .5),
            med_null_mean=q([v["null_mean"] for v in per_sat.values()], .5),
            med_null_p95=q([v["null_p95"] for v in per_sat.values()], .5),
            med_coverage=q([v["coverage"] for v in per_sat.values()], .5),
            frac_series_above_null_p95=float(np.mean(
                [v["frac_near_birth"] > v["null_p95"] for v in per_sat.values()]))
            if per_sat else float("nan"),
            total_big_steps=int(sum(v["n_big"] for v in per_sat.values())),
            total_steps=int(sum(v["n_steps"] for v in per_sat.values())))
        out[arm] = dict(aggregate=agg, per_series=per_sat)
    return out


def fmt_r4(res):
    L = ["R4 — H1 follow-on: wire D_phase within-arc steps at satellite arc-birth epochs",
         "  (|dD| > 0.3 m step within +-1 epoch of a birth, against 1000 shuffled birth sets)",
         "  §24.13 makes this read CONDITIONAL on R1 naming H1.  Run it only after R1 does;",
         "  otherwise it is an unregistered read and carries no verdict."]
    for arm, r in res.items():
        a = r["aggregate"]
        L.append(f"\n=== {arm} ===")
        L.append(f"  series {a['n_series']}, steps {a['total_steps']} of which |dD|>0.3 m: {a['total_big_steps']}")
        L.append(f"  med frac near birth {f(a['med_frac_near_birth'])} | null mean {f(a['med_null_mean'])} "
                 f"| null p95 {f(a['med_null_p95'])} | epoch coverage of +-1-of-birth {f(a['med_coverage'])}")
        L.append(f"  share of series above their own null p95: {f(a['frac_series_above_null_p95'])} "
                 "(0.05 is chance)")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R5 — SPEC_event_harness §24.15: is the DJIG/G06 outlier a wire defect or a record?
# --------------------------------------------------------------------------- #
def _census_series(project, arm, t_lo, t_hi, drop_sat=None):
    """The census's own de-medianed D series for one arm and window.

    `drop_sat` removes that satellite BEFORE the common mode is formed, so the
    per-epoch median is re-taken over the remaining satellites — the honest
    counterfactual beside the cheaper "drop it from the pool only".
    """
    import wire_vs_code_census as wc
    clk_paths, bia, doy_key, _ = wc.resolve_code_products(
        os.path.join(arm_dir(project, arm), "seed.json"))
    if not clk_paths or not bia:
        raise SystemExit("CODE products not found (seed.json paths)")
    clk, _n = wc.parse_clk(clk_paths)
    osb, _s = wc.parse_bia(bia, doy_key)
    epochs, _meta = wc.parse_products(os.path.join(arm_dir(project, arm), "products.txt"))
    raw, disc, ev, cnt, _miss, _nc, tows = wc.collect_raw(epochs, clk, osb, t_lo, t_hi)
    if drop_sat:
        for kind in raw:
            for key in [k for k in raw[kind]
                        if (k[0] if isinstance(k, tuple) else k) == drop_sat]:
                raw[kind].pop(key)
    out = {}
    for kind in ("phase", "code", "clk"):
        cm = wc.common_mode(raw[kind], tows, "median")
        out[kind] = wc.demean(raw[kind], disc[kind], cm)
    return out, ev, cnt


def _pool_within_arc(series, ev, sat_filter=None, step=0.25, min_n=2):
    """Declared-arc within-arc residuals (the (iii) statistic) -> (rms, n, per-sat)."""
    import wire_vs_code_census as wc
    pool, per_sat = [], defaultdict(list)
    for key, ser in series.items():
        sat = key[0] if isinstance(key, tuple) else key
        if sat_filter is not None and sat != sat_filter:
            continue
        for arc in wc.split_arcs(ser, ev.get(sat, {}), declared=True):
            if len(arc) < min_n:
                continue
            d = np.array([ser[i][1] for i in arc], float)
            r = d - d.mean()
            pool.extend(r.tolist())
            per_sat[sat].extend(r.tolist())
    a = np.asarray(pool, float)
    rms = float(np.sqrt(np.mean(a * a))) if a.size else float("nan")
    return rms, int(a.size), {s: (float(np.sqrt(np.mean(np.square(v)))), len(v))
                              for s, v in per_sat.items()}


def _pool_level(series, sat_filter=None):
    """p50 / p95 of |de-medianed value| — the between-satellite LEVEL (level_table)."""
    pool, per_sat = [], defaultdict(list)
    for key, ser in series.items():
        sat = key[0] if isinstance(key, tuple) else key
        if sat_filter is not None and sat != sat_filter:
            continue
        for (_t, d, _disc) in ser:
            pool.append(abs(d))
            per_sat[sat].append(abs(d))
    return (q(pool, .5), q(pool, .95), len(pool),
            {s: (q(v, .5), len(v)) for s, v in per_sat.items()})


def read_r5a(project, arms, sat="G06", t_lo=262800.0, t_hi=266370.0):
    out = {}
    for arm in arms:
        ser, ev, cnt = _census_series(project, arm, t_lo, t_hi)
        ser_re, ev_re, _ = _census_series(project, arm, t_lo, t_hi, drop_sat=sat)
        A = dict(window=[t_lo, t_hi], sat=sat)
        for kind in ("phase", "code"):
            rms_all, n_all, per = _pool_within_arc(ser[kind], ev)
            rms_sat, n_sat, _ = _pool_within_arc(ser[kind], ev, sat_filter=sat)
            keep = {k: v for k, v in ser[kind].items()
                    if (k[0] if isinstance(k, tuple) else k) != sat}
            rms_ex, n_ex, _ = _pool_within_arc(keep, ev)
            rms_re, n_re, _ = _pool_within_arc(ser_re[kind], ev_re)
            # share of the pooled SUM OF SQUARES the satellite carries
            ss_all = rms_all ** 2 * n_all
            ss_sat = rms_sat ** 2 * n_sat if n_sat else 0.0
            A[f"D_{kind}"] = dict(
                rms_all=rms_all, n_all=n_all,
                rms_sat_only=rms_sat, n_sat=n_sat,
                rms_excl_pool_only=rms_ex, n_excl=n_ex,
                rms_excl_cm_recomputed=rms_re, n_excl_cm=n_re,
                share_of_sumsq=float(ss_sat / ss_all) if ss_all > 0 else float("nan"),
                share_of_rows=float(n_sat / n_all) if n_all else float("nan"),
                per_sat={k: dict(rms=v[0], n=v[1]) for k, v in sorted(
                    per.items(), key=lambda kv: -kv[1][0])})
        p50, p95, n, per = _pool_level(ser["clk"])
        p50s, _p95s, ns, _ = _pool_level(ser["clk"], sat_filter=sat)
        keep = {k: v for k, v in ser["clk"].items()
                if (k[0] if isinstance(k, tuple) else k) != sat}
        p50e, p95e, ne, _ = _pool_level(keep)
        p50r, p95r, nr, _ = _pool_level(ser_re["clk"])
        A["clk_LEVEL"] = dict(p50_all=p50, p95_all=p95, n_all=n,
                              p50_sat_only=p50s, n_sat=ns,
                              p50_excl_pool_only=p50e, p95_excl_pool_only=p95e, n_excl=ne,
                              p50_excl_cm_recomputed=p50r, p95_excl_cm_recomputed=p95r,
                              n_excl_cm=nr,
                              per_sat={k: dict(p50=v[0], n=v[1]) for k, v in sorted(
                                  per.items(), key=lambda kv: -kv[1][0])})
        out[arm] = A
    return out


def _scan_csv(path, pred, limit=400):
    """Return (header, [matching raw lines]) — quoted verbatim in the report."""
    if not os.path.isfile(path):
        return None, []
    with open(path, encoding="utf-8", errors="replace") as fh:
        hdr = fh.readline().rstrip("\n")
        cols = hdr.split(",")
        hits = []
        for line in fh:
            f = line.rstrip("\n").split(",")
            try:
                if pred(dict(zip(cols, f)), f) and len(hits) < limit:
                    hits.append(line.rstrip("\n"))
            except Exception:
                continue
    return hdr, hits


def read_r5b(project, arms, station="DJIG00DJI", sat="G06", k_lo=210, k_hi=240):
    t_lo, t_hi = TOW0 + DT_S * (k_lo - 1), TOW0 + DT_S * (k_hi - 1)
    out = {}
    for arm in arms:
        logs = os.path.join(arm_dir(project, arm), "logs")
        R = dict(window_tow=[t_lo, t_hi], window_k=[k_lo, k_hi],
                 station=station, sat=sat)

        def inwin(d):
            t = float(d["tow"])
            return t_lo - 1e-3 <= t <= t_hi + 1e-3

        hdr, hits = _scan_csv(os.path.join(logs, "network_events.csv"),
                              lambda d, f: inwin(d) and d.get("sat") == sat
                              and d.get("station") == station)
        R["events_sat_station"] = dict(header=hdr, lines=hits)
        _h, hits2 = _scan_csv(os.path.join(logs, "network_events.csv"),
                              lambda d, f: inwin(d) and d.get("sat") == sat)
        R["events_sat_any_station"] = dict(n=len(hits2), tiers=dict(
            (t, sum(1 for l in hits2 if l.split(",")[2] == t))
            for t in sorted({l.split(",")[2] for l in hits2})))
        hdr, hits = _scan_csv(os.path.join(logs, "network_raim.csv"),
                              lambda d, f: inwin(d) and (d.get("sat") == sat
                                                         or d.get("station") == station))
        R["raim"] = dict(header=hdr, lines=hits, n=len(hits))
        hdr, hits = _scan_csv(os.path.join(logs, "network_raim_attrib.csv"),
                              lambda d, f: inwin(d) and d.get("entity") in (sat, station))
        R["raim_attrib"] = dict(header=hdr, lines=hits, n=len(hits))
        # the whole-run counts, so "nothing in the window" can be read against
        # "the screen ran at all"
        _h, allraim = _scan_csv(os.path.join(logs, "network_raim.csv"),
                                lambda d, f: True, limit=10 ** 7)
        R["raim_run_total_rows"] = len(allraim)
        R["raim_run_rows_this_sat"] = sum(1 for l in allraim if l.split(",")[2] == sat)
        R["raim_run_rows_this_station"] = sum(1 for l in allraim if l.split(",")[3] == station)
        _h, allev = _scan_csv(os.path.join(logs, "network_events.csv"),
                              lambda d, f: d.get("sat") == sat and d.get("station") == station,
                              limit=10 ** 7)
        R["events_sat_station_whole_run"] = allev
        # epoch dispositions over the window (withhold / sigma0 / adapt)
        hdr, hits = _scan_csv(os.path.join(logs, "network_epoch_status.csv"),
                              lambda d, f: inwin(d) and (d.get("disposition") != "emitted"
                                                         or float(d.get("n_adapt") or 0) > 0))
        R["epoch_status_nonclean"] = dict(header=hdr and ",".join(hdr.split(",")[:8]),
                                          lines=[",".join(l.split(",")[:8]) for l in hits])
        # the residual rows the screens were looking at
        _h, rows = _scan_csv(os.path.join(logs, "network_pf_rows.csv"),
                             lambda d, f: inwin(d) and d.get("station") == station
                             and d.get("sat") == sat and d.get("kind") == "code",
                             limit=10 ** 6)
        vals = [float(l.split(",")[6]) for l in rows]
        R["pf_rows_code"] = dict(n=len(rows), min=min(vals) if vals else None,
                                 max=max(vals) if vals else None,
                                 first_line=rows[0] if rows else None,
                                 last_line=rows[-1] if rows else None)
        out[arm] = R
    return out


def read_r5c(project, arms, tow_focus=265470.0, sat="G06",
             young_epochs=5, low_elev_deg=25.0):
    out = {}
    for arm in arms:
        logs = os.path.join(arm_dir(project, arm), "logs")
        df = load_pf_rows(project, arm)
        # per (station, sat) epoch runs, NO min length — a newborn arc is 1 row
        g = df.groupby(["station", "sat"], observed=True)["k"]
        runs = {}
        for (sta, s), kk in g:
            k = np.unique(kk.to_numpy())
            cut = np.nonzero(np.diff(k) > 5)[0]
            b = [0] + (cut + 1).tolist() + [len(k)]
            runs[(str(sta), str(s))] = [(int(k[b[i]]), int(k[b[i + 1] - 1]))
                                        for i in range(len(b) - 1)]
        el = {}
        for r in df[["station", "sat", "k", "elev_deg"]].itertuples(index=False):
            el[(str(r.station), str(r.sat), int(r.k))] = float(r.elev_deg)

        def age_elev(sta, s, k):
            for a, b in runs.get((sta, s), []):
                if a <= k <= b:
                    return k - a, el.get((sta, s, k))
            return None, el.get((sta, s, k))

        hdr, hand = _scan_csv(os.path.join(logs, "network_events.csv"),
                              lambda d, f: d.get("tier") == "PHASE_ANCHOR", limit=10 ** 6)
        _h, gauge = _scan_csv(os.path.join(logs, "network_events.csv"),
                              lambda d, f: d.get("tier") == "PBGAUGE", limit=10 ** 6)
        tab = []
        for line in hand:
            f = line.split(",")
            t, s, sta, detail = float(f[1]), f[4], f[5], f[9]
            k = int(round((t - TOW0) / DT_S)) + 1
            age, e = age_elev(sta, s, k)
            tab.append(dict(tow=t, k=k, sat=s, to_station=sta, detail=detail,
                            successor_arc_age_ep=age, successor_elev_deg=e,
                            young=(age is not None and age < young_epochs),
                            low=(e is not None and e < low_elev_deg)))
        cold = [l for l in gauge if l.split(",")[3] == "handover_cold"]
        # A satellite handing over files ONE PHASE_ANCHOR row per (sat, signal)
        # family, so the row count over-counts elections; de-duplicate on
        # (tow, sat, successor) and report both, plus the whole-run log's own count.
        seen, uniq = set(), []
        for r in tab:
            key = (r["tow"], r["sat"], r["to_station"])
            if key not in seen:
                seen.add(key)
                uniq.append(r)
        R = dict(n_phase_anchor_rows=len(tab), n_distinct_elections=len(uniq),
                 n_phase_anchor_handovers=len(tab),
                 n_distinct_young=sum(1 for r in uniq if r["young"]),
                 n_distinct_low=sum(1 for r in uniq if r["low"]),
                 n_distinct_young_or_low=sum(1 for r in uniq if r["young"] or r["low"]),
                 n_pbgauge_cold=len(cold), n_pbgauge_continuous=len(gauge) - len(cold),
                 cold_lines=cold,
                 n_young=sum(1 for r in tab if r["young"]),
                 n_low=sum(1 for r in tab if r["low"]),
                 n_young_or_low=sum(1 for r in tab if r["young"] or r["low"]),
                 n_young_and_low=sum(1 for r in tab if r["young"] and r["low"]),
                 n_unresolved=sum(1 for r in tab if r["successor_arc_age_ep"] is None),
                 handovers=tab)
        # the focus election: every station tracking `sat` that epoch, ranked
        k0 = int(round((tow_focus - TOW0) / DT_S)) + 1
        cand = []
        sub = df[(df["k"] == k0) & (df["sat"] == sat)]
        for sta, grp in sub.groupby("station", observed=True):
            age, e = age_elev(str(sta), sat, k0)
            cand.append(dict(station=str(sta), elev_deg=e, arc_age_ep=age,
                             n_rows=int(len(grp)),
                             n_code=int((grp["kind"] == "code").sum()),
                             n_phase=int((grp["kind"] == "phase").sum()),
                             code_resid_min=float(grp[grp["kind"] == "code"]["resid_m"].min())
                             if (grp["kind"] == "code").any() else None))
        cand.sort(key=lambda r: -(r["elev_deg"] or 0))
        R["focus"] = dict(tow=tow_focus, k=k0, sat=sat, candidates=cand,
                          event_lines=[l for l in hand + gauge
                                       if abs(float(l.split(",")[1]) - tow_focus) < 1e-3])
        out[arm] = R
    return out


def read_r5d(project, arms, tow_focus=265470.0, halfwidth_ep=10,
             sat="G06", signals=("L1C", "L1W", "L2W", "L5Q", "L5X")):
    import wire_vs_code_census as wc
    out = {}
    k0 = int(round((tow_focus - TOW0) / DT_S)) + 1
    t_lo = TOW0 + DT_S * (k0 - 1 - halfwidth_ep)
    t_hi = TOW0 + DT_S * (k0 - 1 + halfwidth_ep)
    for arm in arms:
        logs = os.path.join(arm_dir(project, arm), "logs")
        hdr, hits = _scan_csv(os.path.join(logs, "closed_loop_trace.csv"),
                              lambda d, f: t_lo - 1e-3 <= float(d["tow"]) <= t_hi + 1e-3,
                              limit=10 ** 5)
        rows = [dict(zip(hdr.split(","), l.split(","))) for l in hits] if hdr else []
        e3 = [float(r["pos_err_3d_m"]) for r in rows]
        # the largest epoch-to-epoch 3D step inside the window, and the run's own
        # distribution of that step, so "a step" has a scale
        _h, allr = _scan_csv(os.path.join(logs, "closed_loop_trace.csv"),
                             lambda d, f: True, limit=10 ** 6)
        allе = [float(l.split(",")[8]) for l in allr]
        d_all = np.abs(np.diff(np.asarray(allе, float)))
        d_win = np.abs(np.diff(np.asarray(e3, float))) if len(e3) > 1 else np.array([])
        # the wire step the consumer was handed for the handed-over (sat, signal)
        epochs, _meta = wc.parse_products(os.path.join(arm_dir(project, arm), "products.txt"))
        wire = []
        for ep in epochs:
            if not (t_lo - 1e-3 <= ep["tow"] <= t_hi + 1e-3):
                continue
            row = dict(tow=ep["tow"], clock=ep["C"].get(sat, (None,))[0],
                       retro=(sat in ep["retro"]), datum_changed=ep["datum_changed"])
            for sg in signals:
                v = ep["P"].get((sat, sg))
                if v:
                    row[sg] = (v[0], v[3])   # bias_m, disc counter
            wire.append(row)
        steps = []
        for a, b in zip(wire[:-1], wire[1:]):
            for sg in signals:
                if sg in a and sg in b:
                    steps.append(dict(tow=b["tow"], signal=sg,
                                      d_bias_m=b[sg][0] - a[sg][0],
                                      disc_from=a[sg][1], disc_to=b[sg][1],
                                      disc_bumped=a[sg][1] != b[sg][1]))
        # BASE RATE (the lesson of the lag-attribution trap): a per-epoch
        # discontinuity bump means nothing without the rate every other (sat, signal)
        # runs at, in the SAME window and over the whole run.
        def bump_rates(eps, min_pairs):
            bump, tot = defaultdict(int), defaultdict(int)
            for x, y in zip(eps[:-1], eps[1:]):
                for kk in y["P"]:
                    if kk in x["P"]:
                        tot[kk] += 1
                        if x["P"][kk][3] != y["P"][kk][3]:
                            bump[kk] += 1
            return {kk: (bump[kk], tot[kk]) for kk in tot if tot[kk] >= min_pairs}
        win_eps = [e for e in epochs if t_lo - 1e-3 <= e["tow"] <= t_hi + 1e-3]
        wr = bump_rates(win_eps, max(3, halfwidth_ep))
        ar = bump_rates(epochs, 30)
        wv = np.array([b / t for b, t in wr.values()]) if wr else np.array([])
        av = np.array([b / t for b, t in ar.values()]) if ar else np.array([])
        focus_key = (sat, signals[0])
        out[arm] = dict(
            window_tow=[t_lo, t_hi], focus_tow=tow_focus, focus_k=k0,
            consumer_header=hdr, consumer_rows=hits,
            pos3d_at_focus=next((float(r["pos_err_3d_m"]) for r in rows
                                 if abs(float(r["tow"]) - tow_focus) < 1e-3), None),
            pos3d_window_min=min(e3) if e3 else None, pos3d_window_max=max(e3) if e3 else None,
            max_abs_step_in_window=float(d_win.max()) if d_win.size else None,
            run_step_p50=float(np.median(d_all)) if d_all.size else None,
            run_step_p95=float(np.quantile(d_all, .95)) if d_all.size else None,
            run_step_max=float(d_all.max()) if d_all.size else None,
            n_resets_in_window=sum(1 for r in rows if float(r["n_sats"]) == 0),
            wire_steps=[s for s in steps if s["disc_bumped"] or abs(s["d_bias_m"]) > 0.05],
            disc_bump_base_rate=dict(
                window_n_channels=len(wr),
                window_focus=list(wr.get(focus_key, (0, 0))),
                window_focus_rate=float(wr[focus_key][0] / wr[focus_key][1])
                if focus_key in wr else float("nan"),
                window_pool_median=float(np.median(wv)) if wv.size else float("nan"),
                window_pool_mean=float(wv.mean()) if wv.size else float("nan"),
                window_pool_max=float(wv.max()) if wv.size else float("nan"),
                run_n_channels=len(ar),
                run_focus_rate=float(ar[focus_key][0] / ar[focus_key][1])
                if focus_key in ar else float("nan"),
                run_pool_median=float(np.median(av)) if av.size else float("nan"),
                run_pool_p95=float(np.quantile(av, .95)) if av.size else float("nan"),
                run_pool_max=float(av.max()) if av.size else float("nan")),
        )
    return out


def fmt_r5(res):
    a, b, c, d = res["a"], res["b"], res["c"], res["d"]
    L = ["R5 - sec 24.15: is the DJIG00DJI/G06 outlier a wire defect or an outlier record?",
         "bar: G06 carries > 30 % of PV-B's D_phase => a wire defect with a named fix; < 10 % => a record.",
         "",
         "(a) within-arc D rms and the clock LEVEL, last hour (tow 262800..266370), declared arcs,",
         "    median common mode - the same statistic cm_grade reports as (iii).",
         f"    'excl (pool)' drops the satellite's rows from the pool; 'excl (cm)' ALSO re-takes the",
         "    per-epoch median over the remaining satellites."]
    hdr = (f"{'arm':8s} {'quantity':10s} {'all':>9s} {'G06 only':>9s} {'excl(pool)':>11s} "
           f"{'excl(cm)':>9s} {'n_all':>7s} {'n_G06':>6s} {'share SS':>9s} {'share rows':>11s}")
    L += [hdr, "-" * len(hdr)]
    for arm, A in a.items():
        for kind in ("phase", "code"):
            s = A[f"D_{kind}"]
            L.append(f"{arm:8s} {'D_' + kind:10s} {f(s['rms_all'],4)!s:>9s} "
                     f"{f(s['rms_sat_only'],4)!s:>9s} {f(s['rms_excl_pool_only'],4)!s:>11s} "
                     f"{f(s['rms_excl_cm_recomputed'],4)!s:>9s} {s['n_all']:7d} {s['n_sat']:6d} "
                     f"{f(s['share_of_sumsq'],4)!s:>9s} {f(s['share_of_rows'],4)!s:>11s}")
        s = A["clk_LEVEL"]
        L.append(f"{arm:8s} {'clk LEVEL':10s} {f(s['p50_all'],4)!s:>9s} "
                 f"{f(s['p50_sat_only'],4)!s:>9s} {f(s['p50_excl_pool_only'],4)!s:>11s} "
                 f"{f(s['p50_excl_cm_recomputed'],4)!s:>9s} {s['n_all']:7d} {s['n_sat']:6d} "
                 f"{'(p50)':>9s} {f(s['n_sat'] / s['n_all'],4)!s:>11s}")
    for arm, A in a.items():
        L.append("")
        L.append(f"per-satellite within-arc D_phase rms, {arm} (worst 8 of "
                 f"{len(A['D_phase']['per_sat'])}):")
        for sat, v in list(A["D_phase"]["per_sat"].items())[:8]:
            L.append(f"    {sat:5s} rms {f(v['rms'],4)!s:>9s}  n {v['n']:5d}")

    L += ["", "(b) what every screen did with the DJIG00DJI/G06 rows over k 210..240"]
    for arm, R in b.items():
        pr = R["pf_rows_code"]
        L.append(f"  === {arm} === code rows {pr['n']}, residual range "
                 f"{f(pr['min'],2)} .. {f(pr['max'],2)} m")
        L.append(f"      first: {pr['first_line']}")
        L.append(f"      last : {pr['last_line']}")
        L.append(f"      network_raim.csv        : {R['raim']['n']} rows for this sat OR station in "
                 f"the window (whole run: {R['raim_run_total_rows']} rows, "
                 f"{R['raim_run_rows_this_sat']} for the sat, "
                 f"{R['raim_run_rows_this_station']} for the station)")
        L.append(f"      network_raim_attrib.csv : {R['raim_attrib']['n']} rows naming the sat or station")
        L.append(f"      network_events.csv      : {len(R['events_sat_station']['lines'])} rows on "
                 f"(sat, station) in the window; the sat at ANY station: "
                 f"{R['events_sat_any_station']['n']} ({R['events_sat_any_station']['tiers']})")
        for line in R["events_sat_station"]["lines"]:
            L.append(f"        | {line}")
        L.append(f"      epoch_status rows not 'emitted' or with n_adapt > 0: "
                 f"{len(R['epoch_status_nonclean']['lines'])}")
        for line in R["epoch_status_nonclean"]["lines"]:
            L.append(f"        | {line}")
        L.append(f"      whole-run event lines on (sat, station): "
                 f"{len(R['events_sat_station_whole_run'])}")
        for line in R["events_sat_station_whole_run"]:
            L.append(f"        | {line}")

    L += ["", "(c) the G06 phase-anchor election"]
    for arm, R in c.items():
        fo = R["focus"]
        L.append(f"  === {arm} === at tow {fo['tow']:.0f} (k {fo['k']}), {len(fo['candidates'])} "
                 f"stations tracking {fo['sat']}:")
        for line in fo["event_lines"]:
            L.append(f"        | {line}")
        L.append(f"      {'station':11s} {'elev':>6s} {'arc age ep':>10s} {'rows':>5s} "
                 f"{'code':>5s} {'phase':>6s} {'worst code resid m':>19s}")
        for r in fo["candidates"]:
            L.append(f"      {r['station']:11s} {f(r['elev_deg'],2)!s:>6s} "
                     f"{('-' if r['arc_age_ep'] is None else r['arc_age_ep']):>10} "
                     f"{r['n_rows']:5d} {r['n_code']:5d} {r['n_phase']:6d} "
                     f"{f(r['code_resid_min'],2)!s:>19s}")
        L.append(f"      whole run: {R['n_phase_anchor_rows']} PHASE_ANCHOR rows = "
                 f"{R['n_distinct_elections']} distinct (tow, sat, successor) elections "
                 f"(one row per (sat, signal) family); "
                 f"{R['n_pbgauge_cold']} PBGAUGE cold / {R['n_pbgauge_continuous']} continuous.")
        L.append(f"      by ROW   - successor arc younger than 5 epochs: {R['n_young']}; below 25 "
                 f"deg: {R['n_low']}; either: {R['n_young_or_low']}; both: "
                 f"{R['n_young_and_low']} (unresolved successors {R['n_unresolved']})")
        L.append(f"      by ELECTION - younger than 5 epochs: {R['n_distinct_young']}; below 25 "
                 f"deg: {R['n_distinct_low']}; either: {R['n_distinct_young_or_low']}")
        for line in R["cold_lines"]:
            L.append(f"        COLD | {line}")
        yl = [r for r in R["handovers"] if r["young"] or r["low"]]
        if yl:
            L.append(f"      handovers onto a young or low successor:")
            L.append(f"        {'tow':>9s} {'k':>4s} {'sat':4s} {'to':11s} {'elev':>6s} {'age':>4s}")
            for r in yl:
                L.append(f"        {r['tow']:9.0f} {r['k']:4d} {r['sat']:4s} {r['to_station']:11s} "
                         f"{f(r['successor_elev_deg'],2)!s:>6s} "
                         f"{('-' if r['successor_arc_age_ep'] is None else r['successor_arc_age_ep']):>4}")

    L += ["", "(d) the closed-loop consumer within +-10 epochs of the election"]
    for arm, R in d.items():
        L.append(f"  === {arm} === 3D at the election epoch {f(R['pos3d_at_focus'],4)} m; "
                 f"window min/max {f(R['pos3d_window_min'],4)} / {f(R['pos3d_window_max'],4)} m; "
                 f"largest epoch step in the window {f(R['max_abs_step_in_window'],4)} m "
                 f"(run-wide step p50 {f(R['run_step_p50'],4)}, p95 {f(R['run_step_p95'],4)}, "
                 f"max {f(R['run_step_max'],4)}); resets in window {R['n_resets_in_window']}")
        L.append(f"      {R['consumer_header']}")
        for line in R["consumer_rows"]:
            L.append(f"        | {line}")
        L.append("      wire records handed to the consumer that stepped or bumped a disc counter:")
        for s in R["wire_steps"]:
            L.append(f"        | tow {s['tow']:.0f} {s['signal']:4s} d_bias {s['d_bias_m']:+.4f} m  "
                     f"disc {s['disc_from']} -> {s['disc_to']}"
                     f"{'  (BUMPED)' if s['disc_bumped'] else ''}")
        br = R["disc_bump_base_rate"]
        L.append(f"      disc-bump BASE RATE (per-epoch, per (sat,signal)): in this window the focus "
                 f"channel bumps {br['window_focus'][0]}/{br['window_focus'][1]} = "
                 f"{f(br['window_focus_rate'],3)} against a pool of {br['window_n_channels']} "
                 f"channels with median {f(br['window_pool_median'],3)}, mean "
                 f"{f(br['window_pool_mean'],3)}, max {f(br['window_pool_max'],3)}; over the WHOLE "
                 f"run the focus channel is {f(br['run_focus_rate'],3)} against pool median "
                 f"{f(br['run_pool_median'],3)}, p95 {f(br['run_pool_p95'],3)}, max "
                 f"{f(br['run_pool_max'],3)} ({br['run_n_channels']} channels).")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R6 — §24.15: what the wire's noisy satellites are made of
# --------------------------------------------------------------------------- #
R6_SATS = ["G29", "G28", "G32", "G18"]
R6_CONTROLS = ["G27", "G06"]
R_EARTH_M = 6378137.0


def sun_unit_ecef(week, tow):
    """Sub-solar direction as an ECEF unit vector, from the SAME low-precision solar
    series `sh_vtec.mean_sun_longitude` ports from `meanSunLongitude` (sub-solar
    longitude), plus the declination from the ecliptic longitude and obliquity.
    Good to ~0.01 deg — far inside what a cylindrical shadow test needs."""
    jd = 2444244.5 + week * 7.0 + tow / 86400.0
    n = jd - 2451545.0
    L = math.radians((280.460 + 0.9856474 * n) % 360.0)
    g = math.radians((357.528 + 0.9856003 * n) % 360.0)
    lam = L + math.radians(1.915) * math.sin(g) + math.radians(0.020) * math.sin(2.0 * g)
    eps = math.radians(23.439 - 4.0e-7 * n)
    dec = math.asin(math.sin(eps) * math.sin(lam))
    lon = mean_sun_longitude(week, tow)
    return np.array([math.cos(dec) * math.cos(lon),
                     math.cos(dec) * math.sin(lon),
                     math.sin(dec)])


def eclipse_and_beta(sp3_paths, sats, week, t_lo, t_hi):
    """Cylindrical umbra test + beta angle per satellite over [t_lo, t_hi].

    In shadow when the satellite is on the anti-sun side (r.s < 0) AND its distance
    from the Earth-Sun line is below the Earth's radius.  beta is the Sun's elevation
    above the orbital plane: sin(beta) = s . (r x v)/|r x v|, v by central difference
    on the SP3 nodes.
    """
    sys.path.insert(0, os.path.join(REPO, "scripts"))
    from field_wire_phase_clock_census import parse_sp3
    recs = {}
    for p in sp3_paths:
        for sat, (t, xyz) in parse_sp3(p, set(sats)).items():
            if sat in recs:
                t0, x0 = recs[sat]
                t, xyz = np.concatenate([t0, t]), np.concatenate([x0, xyz])
                o = np.argsort(t)
                t, xyz = t[o], xyz[o]
            recs[sat] = (t, xyz)
    out = {}
    for sat in sats:
        if sat not in recs:
            out[sat] = dict(error="no SP3 arc")
            continue
        t, xyz = recs[sat]
        m = (t >= t_lo - 600) & (t <= t_hi + 600)
        t, xyz = t[m], xyz[m]
        # consecutive daily SP3 files share their boundary node; a duplicate time
        # makes np.gradient divide by zero
        _, uniq = np.unique(t, return_index=True)
        t, xyz = t[uniq], xyz[uniq]
        if len(t) < 3:
            out[sat] = dict(error=f"only {len(t)} SP3 nodes in the window")
            continue
        v_ecef = np.gradient(xyz, t, axis=0)
        # SP3 velocity is EARTH-FIXED; the orbit plane is inertially fixed, so the
        # plane normal needs the inertial velocity v_i = v_ecef + omega x r (still
        # written on ECEF axes, which is all the dot product with an ECEF Sun vector
        # needs).  Without this the "beta angle" swings tens of degrees in two hours,
        # which is the Earth turning, not the Sun moving.
        omega = np.array([0.0, 0.0, 7.2921151467e-5])
        rows = []
        for i in range(len(t)):
            s = sun_unit_ecef(week, float(t[i]))
            r = xyz[i]
            along = float(r @ s)
            perp = float(np.linalg.norm(r - along * s))
            nvec = np.cross(r, v_ecef[i] + np.cross(omega, r))
            nvec = nvec / max(1e-9, float(np.linalg.norm(nvec)))
            rows.append(dict(tow=float(t[i]),
                             in_shadow=bool(along < 0.0 and perp < R_EARTH_M),
                             perp_over_Re=perp / R_EARTH_M, along_sign=1 if along > 0 else -1,
                             beta_deg=math.degrees(math.asin(max(-1.0, min(1.0,
                                                                           float(s @ nvec)))))))
        sh = [r["tow"] for r in rows if r["in_shadow"]]
        betas = [r["beta_deg"] for r in rows]
        # the closest approach to the shadow cylinder while on the anti-sun side
        night = [r for r in rows if r["along_sign"] < 0]
        out[sat] = dict(n_nodes=len(rows), any_shadow=bool(sh),
                        shadow_tow_first=min(sh) if sh else None,
                        shadow_tow_last=max(sh) if sh else None,
                        n_shadow_nodes=len(sh),
                        min_perp_over_Re_on_night_side=min((r["perp_over_Re"] for r in night),
                                                           default=None),
                        frac_nodes_night_side=float(len(night) / len(rows)),
                        beta_deg_first=betas[0], beta_deg_last=betas[-1],
                        beta_deg_min=min(betas), beta_deg_max=max(betas))
    return out


def read_r6(project, arm="_cm_pvb", sats=None, t_lo=TOW0, t_hi=266370.0,
            near_ep=2, min_drift_rows=4):
    sats = sats or (R6_SATS + R6_CONTROLS)
    import wire_vs_code_census as wc
    ser, ev, _cnt = _census_series(project, arm, t_lo, t_hi)
    epochs, _m = wc.parse_products(os.path.join(arm_dir(project, arm), "products.txt"))
    epochs = [e for e in epochs if t_lo - 1e-3 <= e["tow"] <= t_hi + 1e-3]
    k_of = lambda t: int(round((t - TOW0) / DT_S)) + 1                      # noqa: E731

    # ---- boundary epochs per satellite: a declared PB or CB discontinuity, a clock
    # disc, an R retro mark, a G datum change, or a PHASE_ANCHOR election.
    bounds = defaultdict(set)
    for a, b in zip(epochs[:-1], epochs[1:]):
        kb = k_of(b["tow"])
        for (s, sg), v in b["P"].items():
            if (s, sg) in a["P"] and a["P"][(s, sg)][3] != v[3]:
                bounds[s].add(kb)
        for (s, sg), v in b["B"].items():
            if (s, sg) in a["B"] and a["B"][(s, sg)][1] != v[1]:
                bounds[s].add(kb)
        for s, v in b["C"].items():
            if s in a["C"] and a["C"][s][2] != v[2]:
                bounds[s].add(kb)
        for s in b["retro"]:
            bounds[s].add(kb)
        if b["datum_changed"]:
            for s in b["C"]:
                bounds[s].add(kb)
    logs = os.path.join(arm_dir(project, arm), "logs")
    _h, anchor_rows = _scan_csv(os.path.join(logs, "network_events.csv"),
                                lambda d, f: d.get("tier") == "PHASE_ANCHOR", limit=10 ** 6)
    anchors = defaultdict(list)
    for line in anchor_rows:
        f = line.split(",")
        anchors[f[4]].append(line)
        bounds[f[4]].add(k_of(float(f[1])))

    # ---- station count per (satellite, epoch) and the elected-anchor geometry
    df = load_pf_rows(project, arm)
    nsta = (df.groupby(["sat", "k"], observed=True)["station"].nunique()
            .reset_index(name="n"))
    nsta_map = defaultdict(dict)
    for r in nsta.itertuples(index=False):
        nsta_map[str(r.sat)][int(r.k)] = int(r.n)
    g = df.groupby(["station", "sat"], observed=True)["k"]
    runs, elev = {}, {}
    for (sta, s), kk in g:
        k = np.unique(kk.to_numpy())
        cut = np.nonzero(np.diff(k) > 5)[0]
        bnd = [0] + (cut + 1).tolist() + [len(k)]
        runs[(str(sta), str(s))] = [(int(k[bnd[i]]), int(k[bnd[i + 1] - 1]))
                                    for i in range(len(bnd) - 1)]
    for r in df[["station", "sat", "k", "elev_deg"]].itertuples(index=False):
        elev[(str(r.station), str(r.sat), int(r.k))] = float(r.elev_deg)

    out = dict(arm=arm, window=[t_lo, t_hi], near_ep=near_ep, sats={})
    for sat in sats:
        keys = [k for k in ser["phase"] if (k[0] if isinstance(k, tuple) else k) == sat]
        B = bounds.get(sat, set())
        near = set()
        for kb in B:
            near.update(range(kb - near_ep, kb + near_ep + 1))
        ss_tot = ss_near = ss_drift_marg = ss_drift_nested = 0.0
        n_rows = n_near = n_arcs = n_arcs_drift = 0
        arclens = []
        dser = defaultdict(list)      # k -> |dD| over that satellite's signals
        for key in keys:
            s_ = ser["phase"][key]
            for arc in wc.split_arcs(s_, ev.get(sat, {}), declared=True):
                if len(arc) < 2:
                    continue
                n_arcs += 1
                arclens.append(len(arc))
                t = np.array([s_[i][0] for i in arc], float)
                d = np.array([s_[i][1] for i in arc], float)
                r = d - d.mean()
                kk = np.array([k_of(x) for x in t])
                ss_tot += float(r @ r)
                n_rows += len(r)
                nm = np.array([int(x) in near for x in kk])
                ss_near += float(r[nm] @ r[nm])
                n_near += int(nm.sum())
                for a_, b_ in zip(range(len(t) - 1), range(1, len(t))):
                    if kk[b_] - kk[a_] == 1:
                        dser[int(kk[b_])].append(abs(d[b_] - d[a_]))
                if len(r) >= min_drift_rows:
                    n_arcs_drift += 1
                    tt = t - t.mean()
                    den = float(tt @ tt)
                    if den > 0:
                        ss_drift_marg += float((tt @ r) ** 2 / den)
                    # nested: the drift of what the boundary rows leave behind
                    if (~nm).sum() >= min_drift_rows:
                        t2 = t[~nm]
                        r2 = r[~nm] - r[~nm].mean()
                        t2 = t2 - t2.mean()
                        d2 = float(t2 @ t2)
                        if d2 > 0:
                            ss_drift_nested += float((t2 @ r2) ** 2 / d2)
        a_share = ss_near / ss_tot if ss_tot > 0 else float("nan")
        b_share_n = ss_drift_nested / ss_tot if ss_tot > 0 else float("nan")
        b_share_m = ss_drift_marg / ss_tot if ss_tot > 0 else float("nan")

        # (2) |dD| vs |d n_stations|
        ks = sorted(dser)
        x, y = [], []
        for k in ks:
            n1, n0 = nsta_map[sat].get(k), nsta_map[sat].get(k - 1)
            if n1 is None or n0 is None:
                continue
            x.append(abs(n1 - n0))
            y.append(float(np.mean(dser[k])))
        ns = sorted(nsta_map[sat].values()) if nsta_map[sat] else []
        # |dn| is zero on ~95 % of epoch pairs, so a Pearson against it is nearly a
        # two-point test.  The conditional contrast is the statistic with power.
        xa, ya = np.asarray(x, float), np.asarray(y, float)
        chg = xa > 0
        m_chg = float(ya[chg].mean()) if chg.any() else float("nan")
        m_no = float(ya[~chg].mean()) if (~chg).any() else float("nan")

        # (3) the elections
        el = []
        for line in anchors.get(sat, []):
            f = line.split(",")
            k = k_of(float(f[1]))
            sta = f[5]
            age = None
            for a_, b_ in runs.get((sta, sat), []):
                if a_ <= k <= b_:
                    age = k - a_
            el.append(dict(line=line, tow=float(f[1]), k=k, to_station=sta,
                           elev_deg=elev.get((sta, sat, k)), arc_age_ep=age))

        out["sats"][sat] = dict(
            n_signal_series=len(keys), n_declared_arcs=n_arcs, n_rows=n_rows,
            arc_len_p50=q(arclens, .5), arc_len_max=max(arclens) if arclens else None,
            within_arc_rms=float(math.sqrt(ss_tot / n_rows)) if n_rows else float("nan"),
            n_boundary_epochs=len(B), boundary_epochs=sorted(B),
            frac_rows_near_boundary=float(n_near / n_rows) if n_rows else float("nan"),
            share_a_near_boundary=a_share,
            # the honest statistic: the boundary band already COVERS `frac near` of the
            # rows, so (a) has to be read as an enrichment over that base rate
            enrichment_a=float(a_share / (n_near / n_rows))
            if n_rows and n_near else float("nan"),
            share_b_drift_nested=b_share_n, share_b_drift_marginal=b_share_m,
            share_c_remainder=float(1.0 - a_share - b_share_n)
            if np.isfinite(a_share) and np.isfinite(b_share_n) else float("nan"),
            n_arcs_drift_fitted=n_arcs_drift,
            nsta_min=min(ns) if ns else None, nsta_max=max(ns) if ns else None,
            nsta_p50=q(ns, .5) if ns else None,
            r_absdD_vs_absdn=pearson(x, y), rho_absdD_vs_absdn=spearman(x, y),
            n_epoch_pairs=len(x), n_set_change_pairs=int(chg.sum()) if x else 0,
            frac_epochs_with_set_change=float(np.mean(np.asarray(x) > 0)) if x else float("nan"),
            mean_absdD_on_set_change=m_chg, mean_absdD_no_change=m_no,
            ratio_absdD_change_over_not=float(m_chg / m_no)
            if np.isfinite(m_chg) and np.isfinite(m_no) and m_no > 0 else float("nan"),
            n_election_epochs=len({e["k"] for e in el}),
            # a satellite's SIGNAL families can elect DIFFERENT anchor stations in the
            # same epoch (the run-end log's "split-referenced satellites"); an election
            # epoch with more than one successor is that
            n_split_reference_epochs=sum(
                1 for k in {e["k"] for e in el}
                if len({e["to_station"] for e in el if e["k"] == k}) > 1),
            elections=el)
    # (4) eclipse / beta
    data_root = os.path.join(project, "data")
    sp3 = sorted(glob_sp3(data_root))
    out["eclipse"] = eclipse_and_beta(sp3, sats, 2313, t_lo, t_hi)
    out["sp3_files"] = sp3
    return out


def glob_sp3(data_root):
    import glob as _g
    return [p for d in ("2024-128", "2024-129", "2024-130")
            for p in _g.glob(os.path.join(data_root, d, "products", "*ORB.SP3"))]


def fmt_r6(R):
    L = [f"R6 - sec 24.15: what the wire's noisy satellites are made of ({R['arm']}, "
         f"tow {R['window'][0]:.0f}..{R['window'][1]:.0f}, the FULL 2 h)",
         "classification: (a) > 50 % => the re-realization mechanism beside declared boundaries;",
         "  (b) > 50 % => a slowly varying satellite term; station-count r > 0.5 => coverage-driven",
         "  re-referencing; an eclipse crossing inside the noisy arcs => the re-admission edge.",
         "",
         "D_phase = the census's de-medianed wire-vs-CODE contrast; arcs are DECLARED arcs, so a",
         "  disc bump / retro mark / datum change already CUTS the arc and the +-2-epoch boundary",
         "  band therefore sits at the arc edges.  (a) and (b) are reported NESTED in the card's",
         "  own order (the pf_rows_attrib house convention) so a+b+c closes exactly; (b) marginal",
         "  is the drift fitted on ALL rows of each arc instead.  'frac near' is the SHARE OF ROWS",
         "  the +-2 band covers - the base rate (a) has to beat."]
    hdr = (f"{'sat':4s} {'arcs':>5s} {'rows':>6s} {'len50':>6s} {'rms':>7s} {'nbnd':>5s} "
           f"{'frac near':>9s} {'(a) bnd':>8s} {'ENRICH':>7s} {'(b) drift':>9s} {'nfit':>5s} "
           f"{'(b) marg':>9s} {'(c) rest':>8s}")
    L += ["", "(1) the within-arc sum-of-squares decomposition", hdr, "-" * len(hdr)]
    for sat, s in R["sats"].items():
        tag = "*" if sat in R6_SATS else " "
        L.append(f"{sat:3s}{tag}{s['n_declared_arcs']:5d} {s['n_rows']:6d} "
                 f"{f(s['arc_len_p50'],0)!s:>6s} {f(s['within_arc_rms'],4)!s:>7s} "
                 f"{s['n_boundary_epochs']:5d} {f(s['frac_rows_near_boundary'],3)!s:>9s} "
                 f"{f(s['share_a_near_boundary'],3)!s:>8s} {f(s['enrichment_a'],2)!s:>7s} "
                 f"{f(s['share_b_drift_nested'],3)!s:>9s} {s['n_arcs_drift_fitted']:5d} "
                 f"{f(s['share_b_drift_marginal'],3)!s:>9s} {f(s['share_c_remainder'],3)!s:>8s}")
    L.append("  (* = the wire's noisy four; the others are the controls)")
    L.append("  ENRICH = (a) / 'frac near'.  1.00 means the boundary band holds exactly its own")
    L.append("    share of the energy, i.e. NO concentration at declared boundaries.  nfit = arcs")
    L.append("    long enough (>= 4 rows) for a drift fit.")
    hdr = (f"{'sat':4s} {'nsta':>10s} {'pairs':>6s} {'set-chg':>8s} {'r |dD|,|dn|':>11s} "
           f"{'rho':>7s} {'|dD| chg':>9s} {'|dD| no':>9s} {'ratio':>7s}")
    L += ["", "(2) does the wire move when the tracking set moves?", hdr, "-" * len(hdr)]
    for sat, s in R["sats"].items():
        tag = "*" if sat in R6_SATS else " "
        L.append(f"{sat:3s}{tag}{(str(s['nsta_min']) + '-' + str(s['nsta_max'])):>10s} "
                 f"{s['n_epoch_pairs']:6d} {s['n_set_change_pairs']:8d} "
                 f"{f(s['r_absdD_vs_absdn'])!s:>11s} {f(s['rho_absdD_vs_absdn'])!s:>7s} "
                 f"{f(s['mean_absdD_on_set_change'],4)!s:>9s} "
                 f"{f(s['mean_absdD_no_change'],4)!s:>9s} "
                 f"{f(s['ratio_absdD_change_over_not'],2)!s:>7s}")
    L.append("  |dn| is 0 on ~95 % of epoch pairs, so the Pearson is nearly a two-point test; the")
    L.append("  conditional means (mean |dD| on set-change pairs vs the rest) are the powered form.")

    L += ["", "(3) anchor elections per satellite (successor elevation and arc age at election)"]
    for sat, s in R["sats"].items():
        L.append(f"  {sat}: {len(s['elections'])} PHASE_ANCHOR row(s) over "
                 f"{s['n_election_epochs']} election epoch(s), of which "
                 f"{s['n_split_reference_epochs']} elect MORE THAN ONE successor station "
                 f"(the signal families split-reference)")
        for e in s["elections"]:
            L.append(f"    | {e['line']}")
            L.append(f"        -> k {e['k']}, successor {e['to_station']} at "
                     f"{f(e['elev_deg'],2)} deg, arc age "
                     f"{'-' if e['arc_age_ep'] is None else e['arc_age_ep']} epochs")

    L += ["", "(4) eclipse (cylindrical umbra) and beta angle over the window",
          f"    SP3: {', '.join(os.path.basename(p) for p in R['sp3_files'])}"]
    hdr = (f"{'sat':4s} {'nodes':>6s} {'in shadow':>10s} {'shadow tow':>20s} {'min perp/Re':>12s} "
           f"{'night frac':>10s} {'beta first':>10s} {'beta last':>10s} {'beta min..max':>16s}")
    L += [hdr, "-" * len(hdr)]
    for sat, e in R["eclipse"].items():
        if "error" in e:
            L.append(f"{sat:4s} {e['error']}")
            continue
        sw = ("-" if not e["any_shadow"]
              else f"{e['shadow_tow_first']:.0f}..{e['shadow_tow_last']:.0f}")
        if e["min_perp_over_Re_on_night_side"] is None:
            e = dict(e, min_perp_over_Re_on_night_side=float("nan"))
        L.append(f"{sat:4s} {e['n_nodes']:6d} {str(e['any_shadow']):>10s} {sw:>20s} "
                 f"{f(e['min_perp_over_Re_on_night_side'],3)!s:>12s} "
                 f"{f(e['frac_nodes_night_side'],2)!s:>10s} {f(e['beta_deg_first'],2)!s:>10s} "
                 f"{f(e['beta_deg_last'],2)!s:>10s} "
                 f"{(f(e['beta_deg_min'],2) + '..' + f(e['beta_deg_max'],2)):>16s}")
    L.append("    in shadow = on the anti-sun side AND within one Earth radius of the Earth-Sun")
    L.append("    line; min perp/Re is the closest the satellite came to that cylinder while on")
    L.append("    the night side (< 1 would be an eclipse).")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R7 — §24.15: is the wire's within-arc wander its ANCHOR station's code level?
# --------------------------------------------------------------------------- #
def anchor_timelines(logs, k_of):
    """Piecewise-constant anchor station per epoch.

    PHASE_ANCHOR rows carry `A->B` in the detail field, so the incumbent BEFORE the
    first handover is that handover's own `from` — which is how a satellite with at
    least one election gets an anchor for the whole window even though the initial
    election files no event.  PBGAUGE rows carry `<sat>_<signal>` and `from=A`, so
    they give the same timeline keyed per SIGNAL (which is what a split-referencing
    satellite like G18 actually has).  Returns (per_sat, per_sat_signal), each
    {key: [(k_start, station)]} sorted, k_start = 1 for the incumbent.
    """
    _h, rows = _scan_csv(os.path.join(logs, "network_events.csv"),
                         lambda d, f: d.get("tier") in ("PHASE_ANCHOR", "PBGAUGE"),
                         limit=10 ** 6)
    per_sat, per_sig = defaultdict(list), defaultdict(list)
    for line in rows:
        f = line.split(",")
        tier, ent, to_sta, detail = f[2], f[4], f[5], f[9]
        k = k_of(float(f[1]))
        frm = None
        if "->" in detail:
            frm = detail.split("->")[0].strip()
        elif detail.startswith("from="):
            frm = detail[5:].strip()
        if tier == "PHASE_ANCHOR":
            per_sat[ent].append((k, frm, to_sta))
        else:
            per_sig[ent.replace("_", "|", 1)].append((k, frm, to_sta))

    def build(d):
        out = {}
        for key, evs in d.items():
            evs = sorted(set(evs))
            tl = []
            if evs[0][1]:
                tl.append((1, evs[0][1]))
            for k, _frm, to in evs:
                if tl and tl[-1][0] == k:
                    tl[-1] = (k, to)
                else:
                    tl.append((k, to))
            out[key] = tl
        return out
    return build(per_sat), build(per_sig)


def anchor_at(timeline, k):
    sta = None
    for k0, s in (timeline or ()):
        if k0 <= k:
            sta = s
        else:
            break
    return sta


def struct_ratio(d, lag_lo=1, lag_hi=10):
    """sigma^2(tau) = 0.5*mean[(D(t+tau)-D(t))^2]; returns sigma^2(hi)/sigma^2(lo).

    White noise gives ~1, a random walk ~lag_hi/lag_lo (=10), a pure linear drift
    ~ (hi/lo)^2 (=100)."""
    d = np.asarray(d, float)
    def sv(tau):
        if len(d) <= tau:
            return float("nan")
        dd = d[tau:] - d[:-tau]
        return 0.5 * float(np.mean(dd * dd))
    lo, hi = sv(lag_lo), sv(lag_hi)
    return float(hi / lo) if np.isfinite(lo) and lo > 0 and np.isfinite(hi) else float("nan")


def read_r7(project, arm="_cm_pvb", t_lo=TOW0, t_hi=266370.0, min_arc_ep=20,
            null_draws=20, seed=20260914):
    import wire_vs_code_census as wc
    rng = np.random.default_rng(seed)
    k_of = lambda t: int(round((t - TOW0) / DT_S)) + 1                      # noqa: E731
    logs = os.path.join(arm_dir(project, arm), "logs")
    ser, ev, _c = _census_series(project, arm, t_lo, t_hi)
    tl_sat, tl_sig = anchor_timelines(logs, k_of)

    df = load_pf_rows(project, arm)
    code = df[df["kind"] == "code"]
    code = code[(code["k"] >= k_of(t_lo)) & (code["k"] <= k_of(t_hi))]
    # per (sat, station, epoch): signal-pooled and C1-only code level, plus elevation
    g = code.groupby(["sat", "station", "k"], observed=True)
    lvl = g["resid_m"].mean().to_dict()
    elv = g["elev_deg"].mean().to_dict()
    c1 = code[code["signal"].isin(["C1C", "C1W", "C1X"])]
    lvl_c1 = c1.groupby(["sat", "station", "k"], observed=True)["resid_m"].mean().to_dict()
    # per (sat, epoch): the tracking-set mean  (B)
    B = code.groupby(["sat", "k"], observed=True)["resid_m"].mean().to_dict()
    # per epoch: the network-wide mean over every satellite  (C, the common-mode control)
    C = code.groupby("k", observed=True)["resid_m"].mean().to_dict()

    def wabsmean(rows, field):
        v = np.abs(np.array([r[field] for r in rows], float))
        w = np.array([r["n"] for r in rows], float)
        m = np.isfinite(v)
        return float((v[m] * w[m]).sum() / w[m].sum()) if m.any() else float("nan")

    sats = sorted({str(s) for s in code["sat"].unique()})
    g32_series = []
    out = dict(arm=arm, window=[t_lo, t_hi], min_arc_ep=min_arc_ep,
               n_sats_in_rows=len(sats),
               n_sats_with_anchor=len(tl_sat),
               sats_without_anchor=[s for s in sats if s not in tl_sat],
               sats={}, arcs=[])

    for sat in sats:
        rows = []
        for key, s_ in ser["phase"].items():
            if (key[0] if isinstance(key, tuple) else key) != sat:
                continue
            sig = key[1] if isinstance(key, tuple) else ""
            for arc in wc.split_arcs(s_, ev.get(sat, {}), declared=True):
                if len(arc) < min_arc_ep:
                    continue
                kk = np.array([k_of(s_[i][0]) for i in arc])
                dd = np.array([s_[i][1] for i in arc], float)
                tlS = tl_sat.get(sat)
                tlG = tl_sig.get(f"{sat}|{sig}") or tlS
                aS = np.array([lvl.get((sat, anchor_at(tlS, int(k)), int(k)), np.nan)
                               if tlS else np.nan for k in kk])
                aG = np.array([lvl.get((sat, anchor_at(tlG, int(k)), int(k)), np.nan)
                               if tlG else np.nan for k in kk])
                a1 = np.array([lvl_c1.get((sat, anchor_at(tlS, int(k)), int(k)), np.nan)
                               if tlS else np.nan for k in kk])
                ae = np.array([elv.get((sat, anchor_at(tlS, int(k)), int(k)), np.nan)
                               if tlS else np.nan for k in kk])
                bb = np.array([B.get((sat, int(k)), np.nan) for k in kk])
                cc = np.array([C.get(int(k), np.nan) for k in kk])
                rA, _i, _ = ols_slope(aS, dd)
                sA = rA
                rA = pearson(aS, dd)
                sB, _i2, rB = ols_slope(bb, dd)
                # a size-matched, autocorrelation-matched NULL: the same statistic
                # against a DIFFERENT satellite's anchor level over the same epochs
                others = [s for s in sats if s != sat and s in tl_sat]
                nulls = []
                for _ in range(null_draws):
                    o = str(rng.choice(others)) if others else None
                    if not o:
                        continue
                    av = np.array([lvl.get((o, anchor_at(tl_sat[o], int(k)), int(k)), np.nan)
                                   for k in kk])
                    nulls.append(abs(pearson(av, dd)))
                rows.append(dict(
                    sat=sat, signal=sig, k0=int(kk[0]), k1=int(kk[-1]), n=len(kk),
                    D_rms=float(np.sqrt(np.mean((dd - dd.mean()) ** 2))),
                    r_A=rA, slope_A=sA, r_A_persignal=pearson(aG, dd),
                    r_A_c1=pearson(a1, dd), r_B=rB, slope_B=sB, r_C_commonmode=pearson(cc, dd),
                    anchor_elev_mean=float(np.nanmean(ae)) if np.isfinite(ae).any() else float("nan"),
                    anchor_stations=sorted({anchor_at(tlS, int(k)) for k in kk} - {None}),
                    null_absr_mean=float(np.mean(nulls)) if nulls else float("nan"),
                    null_absr_p95=float(np.quantile(nulls, .95)) if nulls else float("nan"),
                    struct_ratio_10_over_1=struct_ratio(dd)))
                if sat == "G32":
                    g32_series.append((sig, kk, dd, aS, bb))
        out["arcs"].extend(rows)
        if not rows:
            out["sats"][sat] = dict(n_arcs=0, has_anchor=sat in tl_sat)
            continue
        w = np.array([r["n"] for r in rows], float)

        def wmean(field):
            v = np.array([r[field] for r in rows], float)
            m = np.isfinite(v)
            return float((v[m] * w[m]).sum() / w[m].sum()) if m.any() else float("nan")
        out["sats"][sat] = dict(
            has_anchor=sat in tl_sat, n_arcs=len(rows), n_rows=int(w.sum()),
            D_rms_wmean=wmean("D_rms"),
            r_A_wmean=wmean("r_A"), r_A_persignal_wmean=wmean("r_A_persignal"),
            r_A_c1_wmean=wmean("r_A_c1"), r_B_wmean=wmean("r_B"),
            r_C_wmean=wmean("r_C_commonmode"),
            absr_A_wmean=wabsmean(rows, "r_A"), absr_B_wmean=wabsmean(rows, "r_B"),
            null_absr_wmean=wmean("null_absr_mean"),
            anchor_elev_wmean=wmean("anchor_elev_mean"),
            r_Drms_vs_anchor_elev=pearson([r["anchor_elev_mean"] for r in rows],
                                          [r["D_rms"] for r in rows]),
            struct_ratio_p50=q([r["struct_ratio_10_over_1"] for r in rows], .5),
            anchors=sorted({s for r in rows for s in r["anchor_stations"]}))

    A = out["arcs"]
    w = np.array([r["n"] for r in A], float)

    def pw(field):
        v = np.array([r[field] for r in A], float)
        m = np.isfinite(v)
        return float((v[m] * w[m]).sum() / w[m].sum()) if m.any() else float("nan")
    # arcs whose satellite never elected an anchor have no (A); every (A)-keyed
    # fraction below is taken over the arcs where (A) exists, never over all arcs
    withA = [r for r in A if np.isfinite(r["r_A"])]
    both_weak = [r for r in withA if abs(r["r_A"]) < 0.3 and abs(r["r_B"]) < 0.3]
    out["pooled"] = dict(
        n_arcs=len(A), n_rows=int(w.sum()), n_arcs_with_anchor=len(withA),
        r_A_wmean=pw("r_A"), r_A_persignal_wmean=pw("r_A_persignal"),
        r_A_c1_wmean=pw("r_A_c1"), r_B_wmean=pw("r_B"), r_C_wmean=pw("r_C_commonmode"),
        absr_A_wmean=wabsmean(A, "r_A"), absr_B_wmean=wabsmean(A, "r_B"),
        absr_B_wmean_on_withA=wabsmean(withA, "r_B") if withA else float("nan"),
        null_absr_wmean=pw("null_absr_mean"),
        frac_arcs_rA_gt_0p5=float(np.mean([abs(r["r_A"]) > 0.5 for r in withA]))
        if withA else float("nan"),
        frac_arcs_rB_gt_0p5=float(np.mean([abs(r["r_B"]) > 0.5 for r in A])),
        frac_arcs_both_lt_0p3=float(len(both_weak) / len(withA)) if withA else float("nan"),
        r_Drms_vs_anchor_elev=pearson([r["anchor_elev_mean"] for r in A],
                                      [r["D_rms"] for r in A]),
        rho_Drms_vs_anchor_elev=spearman([r["anchor_elev_mean"] for r in A],
                                         [r["D_rms"] for r in A]),
        weak_arc_struct=dict(
            n=len(both_weak),
            p50=q([r["struct_ratio_10_over_1"] for r in both_weak], .5),
            p10=q([r["struct_ratio_10_over_1"] for r in both_weak], .10),
            p90=q([r["struct_ratio_10_over_1"] for r in both_weak], .90),
            frac_gt_5=float(np.mean([r["struct_ratio_10_over_1"] > 5 for r in both_weak]))
            if both_weak else float("nan"),
            frac_lt_2=float(np.mean([r["struct_ratio_10_over_1"] < 2 for r in both_weak]))
            if both_weak else float("nan")))

    # G32's anchors, read one tenure at a time
    g32 = []
    tl = tl_sat.get("G32", [])
    for i, (k0, sta) in enumerate(tl):
        k1 = tl[i + 1][0] - 1 if i + 1 < len(tl) else k_of(t_hi)
        ks = [k for k in range(k0, k1 + 1)]
        el = [elv.get(("G32", sta, k), np.nan) for k in ks]
        # read each tenure on the SLICE of every arc that lies inside it, not on
        # whole arcs that merely overlap it (an arc can span three tenures)
        seg_d, seg_a, seg_b, nseg, wsum, rms_acc = [], [], [], 0, 0.0, 0.0
        for _sig, kk, dd, aS, bb in g32_series:
            m = (kk >= k0) & (kk <= k1)
            if m.sum() < 5:
                continue
            nseg += 1
            d_ = dd[m]
            seg_d.append(d_ - d_.mean())
            seg_a.append(aS[m])
            seg_b.append(bb[m])
            rms_acc += float(((d_ - d_.mean()) ** 2).sum())
            wsum += int(m.sum())
        g32.append(dict(tenure_k=[k0, k1], n_epochs=k1 - k0 + 1, station=sta,
                        elev_mean=float(np.nanmean(el)) if np.isfinite(el).any() else float("nan"),
                        n_arc_segments=nseg, n_epoch_rows=wsum,
                        D_rms=float(math.sqrt(rms_acc / wsum)) if wsum else float("nan"),
                        r_A=pearson(np.concatenate(seg_a), np.concatenate(seg_d))
                        if seg_d else float("nan"),
                        r_B=pearson(np.concatenate(seg_b), np.concatenate(seg_d))
                        if seg_d else float("nan")))
    out["g32_tenures"] = g32
    return out


def fmt_r7(R):
    P = R["pooled"]
    L = [f"R7 - sec 24.15: is the within-arc D_phase wander the ANCHOR station's own code level?",
         f"  {R['arm']}, tow {R['window'][0]:.0f}..{R['window'][1]:.0f}, declared arcs >= "
         f"{R['min_arc_ep']} epochs.",
         "frozen reading: r(A) > 0.5 and r(A) > r(B) => the wire follows its anchor's code level;",
         "  r(B) > r(A) > 0.5 => network-wide code tracking; both < 0.3 => not code-driven.",
         "",
         "(A) = the anchor station's per-LOS code residual level for that satellite, the anchor per",
         "  epoch reconstructed from the PHASE_ANCHOR rows (a handover's own `from` gives the",
         "  incumbent, so one election covers the whole window).  (B) = the tracking-set mean of",
         "  that satellite's per-LOS code residuals.  (C) = the network-wide mean over EVERY",
         "  satellite, a common-mode control neither (A) nor (B) separates on its own.",
         f"  {R['n_sats_with_anchor']} of {R['n_sats_in_rows']} satellites have an election in the",
         f"  window, so (A) exists for those; no anchor for: "
         f"{', '.join(R['sats_without_anchor']) or '-'}",
         "  NULL = the same |r| against a DIFFERENT satellite's anchor level over the same epochs",
         f"  ({20} draws/arc): it matches arc length and autocorrelation, so r must beat IT,",
         "  not zero."]
    hdr = (f"{'sat':4s} {'arcs':>5s} {'rows':>6s} {'D rms':>7s} {'r(A)':>7s} {'r(A)sig':>8s} "
           f"{'r(A)C1':>7s} {'r(B)':>7s} {'r(C)':>7s} {'|r(A)|':>7s} {'|r(B)|':>7s} {'NULL':>6s} "
           f"{'anc elev':>8s} {'rmsVSel':>8s} {'strct':>6s} {'anchors':>7s}")
    L += ["", hdr, "-" * len(hdr)]
    for sat, s in R["sats"].items():
        if not s.get("n_arcs"):
            continue
        tag = "*" if sat in R6_SATS else ("c" if sat in R6_CONTROLS else " ")
        L.append(f"{sat:3s}{tag}{s['n_arcs']:5d} {s['n_rows']:6d} {f(s['D_rms_wmean'],4)!s:>7s} "
                 f"{f(s['r_A_wmean'])!s:>7s} {f(s['r_A_persignal_wmean'])!s:>8s} "
                 f"{f(s['r_A_c1_wmean'])!s:>7s} {f(s['r_B_wmean'])!s:>7s} "
                 f"{f(s['r_C_wmean'])!s:>7s} {f(s['absr_A_wmean'])!s:>7s} "
                 f"{f(s['absr_B_wmean'])!s:>7s} {f(s['null_absr_wmean'],2)!s:>6s} "
                 f"{f(s['anchor_elev_wmean'],1)!s:>8s} {f(s['r_Drms_vs_anchor_elev'],2)!s:>8s} "
                 f"{f(s['struct_ratio_p50'],1)!s:>6s} {len(s['anchors']):7d}")
    L.append("  (* = the wire's noisy four, c = the controls; blank = every other GPS satellite)")
    L += ["",
          f"POOLED over {P['n_arcs']} arcs / {P['n_rows']} rows, weighted by arc length:",
          f"  r(A) {f(P['r_A_wmean'])}  r(A) per-signal {f(P['r_A_persignal_wmean'])}  "
          f"r(A) C1-only {f(P['r_A_c1_wmean'])}  r(B) {f(P['r_B_wmean'])}  "
          f"r(C) common mode {f(P['r_C_wmean'])}",
          f"  |r(A)| {f(P['absr_A_wmean'])}  |r(B)| {f(P['absr_B_wmean'])}  "
          f"NULL |r| {f(P['null_absr_wmean'])}  -> gap over null: A "
          f"{f(P['absr_A_wmean'] - P['null_absr_wmean'])}, B "
          f"{f(P['absr_B_wmean'] - P['null_absr_wmean'])}",
          f"  of the {P['n_arcs_with_anchor']} arcs where (A) exists: |r(A)| > 0.5 in "
          f"{f(P['frac_arcs_rA_gt_0p5'],3)}, BOTH |r(A)| and |r(B)| < 0.3 in "
          f"{f(P['frac_arcs_both_lt_0p3'],3)};  |r(B)| > 0.5 over ALL arcs: "
          f"{f(P['frac_arcs_rB_gt_0p5'],3)}",
          f"  r(within-arc D rms, anchor elevation) across arcs: Pearson "
          f"{f(P['r_Drms_vs_anchor_elev'])}, Spearman {f(P['rho_Drms_vs_anchor_elev'])}"]
    ws = P["weak_arc_struct"]
    L += ["",
          f"SPECTRUM of the {ws['n']} arcs where BOTH |r(A)| and |r(B)| < 0.3 -- the Allan-like",
          "  structure ratio sigma^2(lag 10) / sigma^2(lag 1).  White noise ~ 1, a random walk ~ 10,",
          "  a pure linear drift ~ 100.",
          f"  p10 {f(ws['p10'],2)}  p50 {f(ws['p50'],2)}  p90 {f(ws['p90'],2)}  "
          f"frac > 5 {f(ws['frac_gt_5'],3)}  frac < 2 {f(ws['frac_lt_2'],3)}"]
    L += ["", "G32's anchors, one tenure at a time (each arc SLICED to the tenure, not whole",
          "  arcs that merely overlap it):",
          f"  {'tenure k':>12s} {'n_ep':>5s} {'station':11s} {'elev':>6s} {'segs':>5s} "
          f"{'rows':>5s} {'D rms':>7s} {'r(A)':>7s} {'r(B)':>7s}"]
    for t in R["g32_tenures"]:
        L.append(f"  {(str(t['tenure_k'][0]) + '-' + str(t['tenure_k'][1])):>12s} "
                 f"{t['n_epochs']:5d} {t['station']:11s} {f(t['elev_mean'],1)!s:>6s} "
                 f"{t['n_arc_segments']:5d} {int(t['n_epoch_rows']):5d} {f(t['D_rms'],4)!s:>7s} "
                 f"{f(t['r_A'])!s:>7s} {f(t['r_B'])!s:>7s}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R8 — §24.19: which channel carries the BULK drift (hours 2-6 of _gl_on6)
# --------------------------------------------------------------------------- #
def perm_null(x, y, n=1000, seed=20260914, groups=None):
    """|r| under permutation of y.  With `groups`, y is permuted WITHIN each group,
    so a station-level (or satellite-level) structure survives the null and only the
    pairing is destroyed."""
    rng = np.random.default_rng(seed)
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    m = np.isfinite(x) & np.isfinite(y)
    x, y = x[m], y[m]
    if len(x) < 5:
        return float("nan"), float("nan")
    g = np.asarray(groups, object)[m] if groups is not None else None
    idx = defaultdict(list)
    if g is not None:
        for i, gg in enumerate(g):
            idx[gg].append(i)
    out = np.empty(n)
    for i in range(n):
        yy = y.copy()
        if g is None:
            rng.shuffle(yy)
        else:
            for _k, ii in idx.items():
                if len(ii) > 1:
                    yy[ii] = rng.permutation(yy[ii])
        out[i] = abs(pearson(x, yy))
    return float(out.mean()), float(np.quantile(out, 0.95))


def circ_lin_r(theta_deg, y):
    """Circular-linear correlation of a linear y with a CIRCULAR theta (local solar
    time).  A plain Pearson against an hour-of-day number is wrong at the wrap, so
    use R = sqrt((rc^2 + rs^2 - 2 rc rs rcs)/(1 - rcs^2))."""
    t = np.radians(np.asarray(theta_deg, float))
    y = np.asarray(y, float)
    m = np.isfinite(t) & np.isfinite(y)
    t, y = t[m], y[m]
    if len(t) < 5:
        return float("nan")
    rc, rs = pearson(np.cos(t), y), pearson(np.sin(t), y)
    rcs = pearson(np.cos(t), np.sin(t))
    den = 1.0 - rcs * rcs
    if den <= 0:
        return float("nan")
    return float(math.sqrt(max(0.0, (rc * rc + rs * rs - 2 * rc * rs * rcs) / den)))


def circ_lin_null(theta_deg, y, n=1000, seed=20260914):
    rng = np.random.default_rng(seed)
    y = np.asarray(y, float)
    out = np.empty(n)
    for i in range(n):
        yy = y.copy()
        rng.shuffle(yy)
        out[i] = circ_lin_r(theta_deg, yy)
    return float(np.nanmean(out)), float(np.nanquantile(out, 0.95))


def _slope(xs, ys):
    s, _c, _r = ols_slope(xs, ys)
    return s


# The eight stations whose rx-CB carry withheld > 90 % of their epochs on _gl_on6
# (the run-end "carried NOTHING" list).  R8b asks whether they are still
# distinguished on (i) and (ii) once the carry is off.
CARRYLESS_8 = ["CAS100ATA", "CPVG00CPV", "GOPE00CZE", "KOKV00USA",
               "LCK400IND", "LMMF00MTQ", "MRO100AUS", "OAFA00ARG"]


def read_r8(project, arm="_gl_on6", contrast="_cm_c10", hours=(2, 3, 4, 5, 6),
            min_rows=30, ep_per_hour=120, nperm=1000, carryless=None):
    carryless = set(carryless or CARRYLESS_8)
    hk = {h: ((h - 1) * ep_per_hour + 1, h * ep_per_hour) for h in hours}
    out = dict(arm=arm, contrast=contrast, hours=list(hours), min_rows=min_rows, per_hour={})

    stations = load_stations(project, arm)
    shcfg = load_sh_cfg(project, arm)
    df = load_pf_rows(project, arm)
    code = df[df["kind"] == "code"]

    # ---------- (c) the SH-tie residual and the field's a00, per hour ----------
    ion = load_iono(project, arm)
    ion = ion[ion["station"].isin(stations.keys())]
    ion = ion[(ion["k"] >= hk[hours[0]][0]) & (ion["k"] <= hk[hours[-1]][1])].copy()
    sh = pd.read_csv(os.path.join(arm_dir(project, arm), "logs", "network_vtec_sh.csv"))
    if "layer" not in sh.columns:
        sh = sh.assign(layer=0)
    sh["k"] = np.rint((sh["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    sh = sh[(sh["k"] >= hk[hours[0]][0]) & (sh["k"] <= hk[hours[-1]][1])]
    pred, map0 = sh_predict(ion, sh, stations, shcfg)
    ion["sh_pred_m"] = pred
    ion["tie_raw_m"] = ion["iono_total_m"].to_numpy(float) - pred
    ion["tie_dm_m"] = ion["tie_raw_m"] - ion.groupby(["station", "k"], observed=True)[
        "tie_raw_m"].transform("mean")
    ion["hour"] = ((ion["k"] - 1) // ep_per_hour + 1).astype(int)
    a00 = sh[(sh["n"] == 0) & (sh["m"] == 0) & (sh["trig"] == "C") & (sh["layer"] == 0)]
    a00 = a00.assign(hour=((a00["k"] - 1) // ep_per_hour + 1).astype(int))

    # ---------- (b) (i) per hour, split by a gross declaration ----------
    logs = os.path.join(arm_dir(project, arm), "logs")
    keyed = pd.read_csv(os.path.join(logs, "network_pf_keyed.csv"))
    keyed["k"] = np.rint((keyed["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    sta_rows = keyed[keyed["kind"] == "STA"]
    _h, gl = _scan_csv(os.path.join(logs, "network_events.csv"),
                       lambda d, f: d.get("tier") == "GROSSLOS", limit=10 ** 6)
    _h, clk = _scan_csv(os.path.join(logs, "network_events.csv"),
                        lambda d, f: d.get("tier") == "CLK_ANCHOR", limit=10 ** 6)
    _h, prr = _scan_csv(os.path.join(logs, "network_events.csv"),
                        lambda d, f: d.get("tier") == "PRIORRES"
                        and "family=rxcb" in (d.get("detail") or ""), limit=10 ** 7)
    zwd = pd.read_csv(os.path.join(logs, "network_zwd.csv"))
    zwd["k"] = np.rint((zwd["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1

    def hour_of(line):
        k = int(round((float(line.split(",")[1]) - TOW0) / DT_S)) + 1
        return (k - 1) // ep_per_hour + 1

    # ---------- (e) the wire's D_phase per hour (the census already run per hour) --
    def census_hour(a, h):
        r = {}
        for tag, fn in (("hour_window", f"win__{a.lstrip('_')}_summary.json"),
                        ("full_run_split", f"_{a.lstrip('_')}_summary.json")):
            p = os.path.join(arm_dir(project, a), f"census_h{h}", fn)
            if not os.path.isfile(p):
                continue
            S = json.load(open(p))
            t = S.get(str(("median", "phase", "declared")), {}).get("table") or []
            lv = S.get(str(("level", "clk"))) or []
            if tag == "hour_window" and t:
                r["D_phase_rms"] = float(t[0][4])
                r["clk_level_p50"] = float(lv[0][2]) if lv else float("nan")
                r["n_arcs"] = S.get(str(("median", "phase", "declared")), {}).get("n_arcs")
            elif t and len(t) >= h:
                r["D_phase_rms_fullrun_split"] = float(t[h - 1][4])
        return r

    for h in hours:
        k0, k1 = hk[h]
        ihr = ion[ion["hour"] == h]
        ahr = a00[a00["hour"] == h]
        s = sta_rows[(sta_rows["k"] >= k0) & (sta_rows["k"] <= k1)]
        med = s.groupby("key")["rms_code"].median()
        glh = {ln.split(",")[5] for ln in gl if hour_of(ln) == h and ln.split(",")[5]}
        decl = {ln.split(",")[5] for ln in gl
                if hour_of(ln) == h and ln.split(",")[3] == "gross_los_declare"}
        wi = [float(v) for k, v in med.items() if str(k) in glh]
        wo = [float(v) for k, v in med.items() if str(k) not in glh]
        out["per_hour"][h] = dict(
            k=[k0, k1], tow=[TOW0 + DT_S * (k0 - 1), TOW0 + DT_S * (k1 - 1)],
            tie_abs_p50=q(np.abs(ihr["tie_dm_m"]), .5), tie_abs_p90=q(np.abs(ihr["tie_dm_m"]), .9),
            tie_raw_abs_p50=q(np.abs(ihr["tie_raw_m"]), .5),
            tie_raw_abs_p90=q(np.abs(ihr["tie_raw_m"]), .9),
            iono_abs_p50=q(ihr["iono_total_m"], .5), sh_pred_p50=q(ihr["sh_pred_m"], .5),
            n_tie_rows=int(len(ihr)),
            a00_tecu_p50=q(ahr["value_tecu"], .5), a00_tecu_first=float(ahr["value_tecu"].iloc[0])
            if len(ahr) else float("nan"),
            a00_tecu_last=float(ahr["value_tecu"].iloc[-1]) if len(ahr) else float("nan"),
            i_p50_all=q(list(med.values), .5), i_p90_all=q(list(med.values), .9),
            i_max_all=float(max(med.values)) if len(med) else float("nan"),
            n_sta_gross=len(glh), n_sta_declared=len(decl),
            i_p50_gross=q(wi, .5), i_p50_clean=q(wo, .5),
            n_gross=len(wi), n_clean=len(wo),
            clk_anchor=dict((a, sum(1 for ln in clk if hour_of(ln) == h
                                    and ln.split(",")[3] == a))
                            for a in ("cta_decouple", "cta_recouple", "cta_disc_landed")),
            rxcb_priorres_rows=sum(1 for ln in prr if hour_of(ln) == h),
            gross_events=dict((a, sum(1 for ln in gl if hour_of(ln) == h
                                      and ln.split(",")[3] == a))
                              for a in ("gross_los_declare", "gross_los_clear",
                                        "gross_los_expire", "gross_los_station_standdown")),
            **census_hour(arm, h))
        # a bigger ionosphere makes a bigger ABSOLUTE representation error, so the
        # tie residual is also reported normalised by the field's own mean
        pp = out["per_hour"][h]
        pp["tie_abs_p50_over_a00_m_per_tecu"] = (
            pp["tie_abs_p50"] / pp["a00_tecu_p50"] if pp["a00_tecu_p50"] else float("nan"))
        # the eight rx-CB carry-less stations against the rest, on (i) and (ii)
        zh = zwd[(zwd["k"] >= k0) & (zwd["k"] <= k1)]
        zmed = zh.groupby("station")["zwd_m"].median()
        ci = [float(v) for k, v in med.items() if str(k) in carryless]
        ri = [float(v) for k, v in med.items() if str(k) not in carryless]
        cz = [abs(float(v)) for k, v in zmed.items() if str(k) in carryless]
        rz = [abs(float(v)) for k, v in zmed.items() if str(k) not in carryless]
        pp["carryless"] = dict(
            stations=sorted(carryless), n_present=len(ci),
            i_p50_carryless=q(ci, .5), i_p50_rest=q(ri, .5),
            i_max_carryless=float(max(ci)) if ci else float("nan"),
            i_ratio=float(q(ci, .5) / q(ri, .5)) if ri and q(ri, .5) else float("nan"),
            zwd_abs_p50_carryless=q(cz, .5), zwd_abs_p50_rest=q(rz, .5),
            n_over_0p5_carryless=int(sum(1 for v in cz if v > 0.5)),
            n_over_0p5_rest=int(sum(1 for v in rz if v > 0.5)))
        cc = census_hour(contrast, h)
        out["per_hour"][h]["contrast_D_phase_rms"] = cc.get("D_phase_rms")

    # ---------- (a) per-LOS level by hour ----------
    code = code[(code["k"] >= hk[hours[0]][0]) & (code["k"] <= hk[hours[-1]][1])].copy()
    code["hour"] = ((code["k"] - 1) // ep_per_hour + 1).astype(int)
    g = code.groupby(["station", "sat", "hour"], observed=True).agg(
        level=("resid_m", "mean"), n=("resid_m", "size"), elev=("elev_deg", "mean"))
    g = g[g["n"] >= min_rows].reset_index()
    tie = ion.groupby(["station", "sat", "hour"], observed=True).agg(
        tie=("tie_dm_m", "mean"), tie_abs=("tie_dm_m", lambda v: float(np.mean(np.abs(v)))),
        n_tie=("tie_dm_m", "size")).reset_index()
    g = g.merge(tie, on=["station", "sat", "hour"], how="left")

    los = []
    for (sta, sat), grp in g.groupby(["station", "sat"], observed=True):
        grp = grp.sort_values("hour")
        if len(grp) < 2:
            continue
        hh = grp["hour"].to_numpy(float)
        lv = grp["level"].to_numpy(float)
        el = grp["elev"].to_numpy(float)
        ta = grp["tie_abs"].to_numpy(float)
        ts = grp["tie"].to_numpy(float)
        st = stations.get(str(sta), {})
        lon = st.get("lon_deg", float("nan"))
        kmid = float(grp["hour"].mean() - 0.5) * ep_per_hour + 1
        tmid = TOW0 + DT_S * (kmid - 1)
        lst_deg = float((math.degrees(math.radians(lon)
                                      - mean_sun_longitude(2313, tmid)) + 540.0) % 360.0 - 180.0)
        los.append(dict(
            station=str(sta), sat=str(sat), n_hours=len(grp),
            hours=[int(x) for x in hh],
            level_first=float(lv[0]), level_last=float(lv[-1]),
            growth_abs=_slope(hh, np.abs(lv)), growth_signed=_slope(hh, lv),
            delta_abs=float(abs(lv[-1]) - abs(lv[0])),
            elev_trend_deg_per_h=_slope(hh, el), elev_mean=float(el.mean()),
            tie_abs_mean=float(np.nanmean(ta)) if np.isfinite(ta).any() else float("nan"),
            tie_abs_growth=_slope(hh, ta), tie_signed_growth=_slope(hh, ts),
            lat_deg=st.get("lat_deg", float("nan")), lon_deg=lon,
            # local solar time as an ANGLE (0 = local solar noon), and in hours
            lst_angle_deg=lst_deg, lst_hours=float((lst_deg / 15.0 + 12.0) % 24.0)))
    out["n_los"] = len(los)
    out["los"] = los

    def corr(field_x, field_y, group=None, label=""):
        x = [r[field_x] for r in los]
        y = [r[field_y] for r in los]
        nm, np95 = perm_null(x, y, n=nperm)
        gm, gp95 = (perm_null(x, y, n=nperm, groups=[r[group] for r in los])
                    if group else (float("nan"),) * 2)
        # the same heavy-tail guard every read in this file carries: a Pearson on a
        # tailed pool is a statement about its tail, so quote Spearman and the
        # 1-99 percentile trim beside it
        xa, ya = np.asarray(x, float), np.asarray(y, float)
        fin = np.isfinite(xa) & np.isfinite(ya)
        tm = fin.copy()
        if fin.sum() > 20:
            for v in (xa, ya):
                lo, hi = np.quantile(v[fin], [0.01, 0.99])
                tm &= (v >= lo) & (v <= hi)
        return dict(label=label, n=int(fin.sum()),
                    r=pearson(x, y), rho=spearman(x, y), slope=_slope(x, y),
                    n_trim=int(tm.sum()), r_trim=pearson(xa[tm], ya[tm]),
                    slope_trim=_slope(xa[tm], ya[tm]),
                    null_absr_mean=nm, null_absr_p95=np95,
                    null_within_station_mean=gm, null_within_station_p95=gp95)

    C = {}
    C["growth_vs_tie_growth"] = corr("tie_abs_growth", "growth_abs", "station",
                                     "|level| growth vs |tie residual| growth")
    C["growth_vs_tie_level"] = corr("tie_abs_mean", "growth_abs", "station",
                                    "|level| growth vs mean |tie residual|")
    C["growth_vs_elev_trend"] = corr("elev_trend_deg_per_h", "growth_abs", "station",
                                     "|level| growth vs elevation trend (+ = rising)")
    C["growth_vs_elev_mean"] = corr("elev_mean", "growth_abs", "station",
                                    "|level| growth vs mean elevation")
    C["growth_vs_abslat"] = dict(corr("lat_deg", "growth_abs", None,
                                      "|level| growth vs latitude"),
                                 r_abs_lat=pearson([abs(r["lat_deg"]) for r in los],
                                                   [r["growth_abs"] for r in los]))
    gy = [r["growth_abs"] for r in los]
    th = [r["lst_angle_deg"] for r in los]
    clm, clp = circ_lin_null(th, gy, n=min(nperm, 500))
    C["growth_vs_local_solar_time"] = dict(
        label="|level| growth vs LOCAL SOLAR TIME (circular-linear R)",
        n=len(los), circ_lin_R=circ_lin_r(th, gy),
        null_mean=clm, null_p95=clp)
    out["correlations"] = C

    # binned views (a circular or non-monotone effect a correlation would miss)
    def bins(field, edges, label):
        rows = []
        v = np.array([r[field] for r in los], float)
        gg = np.array([r["growth_abs"] for r in los], float)
        for lo, hi in zip(edges[:-1], edges[1:]):
            m = (v >= lo) & (v < hi) & np.isfinite(gg)
            if m.sum():
                rows.append(dict(bin=f"{lo:g}..{hi:g}", n=int(m.sum()),
                                 med_growth=float(np.median(gg[m])),
                                 p90_growth=float(np.quantile(gg[m], .9))))
        return dict(label=label, rows=rows)
    out["bins"] = dict(
        lst=bins("lst_hours", [0, 3, 6, 9, 12, 15, 18, 21, 24], "local solar time (h)"),
        elev_trend=bins("elev_trend_deg_per_h", [-30, -10, -3, 3, 10, 30],
                        "elevation trend (deg/hour; negative = setting)"),
        lat=bins("lat_deg", [-90, -45, -20, 20, 45, 90], "station latitude (deg)"),
        elev=bins("elev_mean", [10, 20, 30, 45, 60, 91], "mean elevation (deg)"))
    return out


def fmt_r8(R):
    P = R["per_hour"]
    hs = R["hours"]
    L = [f"R8 - sec 24.19: which channel carries the BULK drift?  {R['arm']} hours "
         f"{hs[0]}-{hs[-1]} (contrast {R['contrast']})",
         "frozen reading: growth tracks the tie residual (r > 0.5) AND the tie residual grows",
         "  => the ionosphere REPRESENTATION is the bulk channel; growth tracks the elevation",
         "  trend (setting arcs) => low-elevation multipath / group delay and the floor is real;",
         "  neither => the level channel's memory (the frozen rx-CB seeds aging).",
         "",
         "(b)+(c)+(d)+(e) PER HOUR"]
    hdr = (f"{'h':>2s} {'(i) p50':>8s} {'p90':>7s} {'max':>7s} {'gross p50':>10s} {'clean p50':>10s} "
           f"{'nG/nC':>8s} {'|tie| p50':>9s} {'p90':>7s} {'/a00':>6s} {'a00':>7s} "
           f"{'CTA dec':>8s} {'rec':>5s} "
           f"{'rxcb':>6s} {'D_ph':>6s} {'c10 D_ph':>8s} {'clkLVL':>7s}")
    L += [hdr, "-" * len(hdr)]
    for h in hs:
        p = P[h]
        L.append(f"{h:2d} {f(p['i_p50_all'],3)!s:>8s} {f(p['i_p90_all'],3)!s:>7s} "
                 f"{f(p['i_max_all'],2)!s:>7s} {f(p['i_p50_gross'],3)!s:>10s} "
                 f"{f(p['i_p50_clean'],3)!s:>10s} "
                 f"{(str(p['n_gross']) + '/' + str(p['n_clean'])):>8s} "
                 f"{f(p['tie_abs_p50'],3)!s:>9s} {f(p['tie_abs_p90'],3)!s:>7s} "
                 f"{f(p['tie_abs_p50_over_a00_m_per_tecu'],4)!s:>6s} "
                 f"{f(p['a00_tecu_p50'],2)!s:>7s} {p['clk_anchor']['cta_decouple']:8d} "
                 f"{p['clk_anchor']['cta_recouple']:5d} {p['rxcb_priorres_rows']:6d} "
                 f"{f(p.get('D_phase_rms'),4)!s:>6s} {f(p.get('contrast_D_phase_rms'),4)!s:>8s} "
                 f"{f(p.get('clk_level_p50'),3)!s:>7s}")
    L += ["  (i) = per-station code rms (pf_keyed STA) median over stations, that hour.",
          "  'gross p50' / 'clean p50' = the same split by whether the station is named in ANY",
          "    GROSSLOS row that hour (declare / standdown / clear / expire).",
          "  |tie| = |iono_total - SH(IPP)| with the per-(station,epoch) mean removed (the free",
          "    datum the all-ones R block leaves); tie_raw is in the JSON beside it.",
          "  CTA dec/rec = clock-anchor decouple / recouple events; rxcb = PRIORRES rows of",
          "    family 'rxcb' (the FLAGGED subset, not the fold count - the run-end totals are",
          "    quoted in the report; no network_prior_residual.csv is armed on this run).",
          "  D_ph = the per-hour census's declared within-arc D_phase rms."]
    c0 = P[hs[0]]["carryless"]
    L += ["", "THE EIGHT rx-CB CARRY-LESS STATIONS against the rest "
              f"({', '.join(c0['stations'])})",
          f"  {'h':>2s} {'n present':>9s} {'(i) p50 8':>10s} {'(i) p50 rest':>13s} {'ratio':>6s} "
          f"{'(i) max 8':>10s} {'|ZWD| p50 8':>12s} {'rest':>7s} {'n>0.5 8/rest':>13s}"]
    for h in hs:
        c = P[h]["carryless"]
        L.append(f"  {h:2d} {c['n_present']:9d} {f(c['i_p50_carryless'],3)!s:>10s} "
                 f"{f(c['i_p50_rest'],3)!s:>13s} {f(c['i_ratio'],2)!s:>6s} "
                 f"{f(c['i_max_carryless'],2)!s:>10s} {f(c['zwd_abs_p50_carryless'],3)!s:>12s} "
                 f"{f(c['zwd_abs_p50_rest'],3)!s:>7s} "
                 f"{(str(c['n_over_0p5_carryless']) + '/' + str(c['n_over_0p5_rest'])):>13s}")
    L += ["", f"(a) per-LOS code level by hour: {R['n_los']} LOS present in >= 2 of hours "
              f"{hs[0]}-{hs[-1]} with >= {R['min_rows']} code rows in the hour", ""]
    hdr = (f"{'read':46s} {'n':>5s} {'r':>7s} {'rho':>7s} {'r_trim':>7s} {'ntrim':>6s} "
           f"{'slope':>9s} {'NULL':>6s} {'NULLp95':>8s} {'NULLsta':>8s}")
    L += [hdr, "-" * len(hdr)]
    for _k, c in R["correlations"].items():
        if "circ_lin_R" in c:
            L.append(f"{c['label']:46s} {c['n']:5d} {f(c['circ_lin_R'])!s:>7s} {'-':>7s} "
                     f"{'-':>7s} {'-':>6s} {'-':>9s} {f(c['null_mean'])!s:>6s} "
                     f"{f(c['null_p95'])!s:>8s} {'-':>8s}")
            continue
        L.append(f"{c['label']:46s} {c['n']:5d} {f(c['r'])!s:>7s} {f(c['rho'])!s:>7s} "
                 f"{f(c['r_trim'])!s:>7s} {c['n_trim']:6d} {f(c['slope'],4)!s:>9s} "
                 f"{f(c['null_absr_mean'])!s:>6s} "
                 f"{f(c['null_absr_p95'])!s:>8s} {f(c['null_within_station_mean'])!s:>8s}")
    L += ["  NULL = |r| under 1000 permutations of the second variable; NULLsta permutes WITHIN",
          "    station, so the station-level structure survives and only the pairing dies.",
          "    r_trim = the same Pearson on the 1-99 percentile trim of BOTH variables - the",
          "    heavy-tail guard every other read in this file carries.",
          "    Local solar time is CIRCULAR, so it gets a circular-linear R, not a Pearson."]
    for key, b in R["bins"].items():
        L += ["", f"  growth binned by {b['label']}:",
              f"    {'bin':>12s} {'n':>6s} {'med growth m/h':>15s} {'p90':>8s}"]
        for r in b["rows"]:
            L.append(f"    {r['bin']:>12s} {r['n']:6d} {f(r['med_growth'],4)!s:>15s} "
                     f"{f(r['p90_growth'],4)!s:>8s}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# R9 — §24.19: the slip response's own mid-arc RE-LEVELS
# --------------------------------------------------------------------------- #
# Every (tier, event) this producer writes that re-seeds or frees a phase level
# mid-arc.  `sig_from` says where that row's signal list lives in the detail
# field; None = the row names no signal and applies to every band of the LOS.
RELEVEL_KINDS = {
    ("T4_attrib", "attrib_arc"): None,            # the arc refloat: == n_arc_refloat
    ("T0_mw_detect", "mw_wl_slip"): "semi",       # detail "L1C;L2W"
    ("T0_mw_detect", "lli_slip"): "semi",         # detail "LLI" (no signal named)
    ("T1_if_slip", "if_slip"): "semi",
    ("T1_if_slip", "refsat_reset"): "semi",
    ("T0_gap", "arc_resume"): "semi",             # detail "L5Q;gap_ep=1;..."
    ("T2_within_epoch", "we_refloat_arc"): "semi",
    ("T2_within_epoch", "we_refloat_sat"): "semi",
}
BAND_OF_PHASE = {"L1": "L1", "L2": "L2", "L5": "L5"}


def _band(sig):
    if not sig or len(sig) < 2 or sig[1] not in "125":
        return None
    return "L" + sig[1]


def read_r9(project, arm="_gl_on6", contrasts=("_gl_rxoff6", "_cm_c10"),
            ep_per_hour=120, min_arc=30, gap_epochs=5, near_ep=1,
            nshuffle=1000, seed=20260914, carryless=None):
    carryless = set(carryless or CARRYLESS_8)
    rng = np.random.default_rng(seed)
    logs = os.path.join(arm_dir(project, arm), "logs")
    k_of = lambda t: int(round((t - TOW0) / DT_S)) + 1                      # noqa: E731

    # ---------------- (a) the event population ----------------
    _h, rows = _scan_csv(os.path.join(logs, "network_events.csv"),
                         lambda d, f: (d.get("tier"), d.get("event")) in RELEVEL_KINDS
                         or (d.get("tier") == "PRIORRES"
                             and "mech=relevel_free" in (d.get("detail") or "")),
                         limit=10 ** 7)
    kinds = defaultdict(list)
    events = []          # (k, station, sat, band or None, kind)
    for line in rows:
        f = line.split(",")
        tow, tier, ev_, sat, sta, detail = (float(f[1]), f[2], f[3], f[4], f[5], f[9])
        key = (tier, ev_)
        if tier == "PRIORRES":
            key = ("PRIORRES", "mech=relevel_free")
            sigs = [t[4:] for t in detail.split() if t.startswith("sig=")]
        elif RELEVEL_KINDS.get(key) == "semi":
            sigs = [s for s in detail.split(";")[0].split(";") if s and s[0] == "L"
                    and len(s) >= 2 and s[1] in "125"]
            if not sigs:                                   # e.g. the bare "LLI" detail
                sigs = []
        else:
            sigs = []
        kinds[key].append(line)
        bands = sorted({_band(s) for s in sigs} - {None}) or [None]
        for b in bands:
            events.append((k_of(tow), sta, sat, b, f"{tier}/{ev_}"))
    ev_df = pd.DataFrame(events, columns=["k", "station", "sat", "band", "kind"])
    ev_df["hour"] = ((ev_df["k"] - 1) // ep_per_hour + 1).astype(int)

    # reconcile against the producer's own run-end voice
    out_txt = _scan_lines(os.path.join(arm_dir(project, arm), "stdout.txt"))
    runend = {}
    for pat, key in ((r"(\d+) detector-marked \(station, amb\) free-epoch latches",
                      "free_epoch_latches"),
                     (r"(\d+) inject-site skips actually taken", "inject_site_skips"),
                     (r"(\d+) RESUMPTION\(s\)", "gap_resumptions")):
        for ln in out_txt:
            m = re.search(pat, ln)
            if m:
                runend[key] = int(m.group(1))
                break
    st = pd.read_csv(os.path.join(logs, "network_epoch_status.csv"))
    st["k"] = np.rint((st["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    st["hour"] = ((st["k"] - 1) // ep_per_hour + 1).astype(int)
    n_arc_refloat = float(pd.to_numeric(st["n_arc_refloat"], errors="coerce").sum())

    df = load_pf_rows(project, arm)
    code = df[df["kind"] == "code"].copy()
    code["hour"] = ((code["k"] - 1) // ep_per_hour + 1).astype(int)
    keyed = pd.read_csv(os.path.join(logs, "network_pf_keyed.csv"))
    keyed["k"] = np.rint((keyed["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
    keyed["hour"] = ((keyed["k"] - 1) // ep_per_hour + 1).astype(int)
    sta_k = keyed[keyed["kind"] == "STA"]

    hours = sorted(int(h) for h in st["hour"].unique())
    per_hour = {}
    for h in hours:
        e = ev_df[ev_df["hour"] == h]
        med = sta_k[sta_k["hour"] == h].groupby("key")["rms_code"].median()
        cnt = e.groupby("station", observed=True).size()
        xs = [float(cnt.get(str(k), 0)) for k in med.index]
        ys = [float(v) for v in med.values]
        per_hour[h] = dict(
            n_events=int(len(e)),
            n_events_by_kind=dict(e.groupby("kind", observed=True).size()),
            n_stations_with_event=int(e["station"].nunique()),
            i_p50=q(ys, .5), i_p90=q(ys, .9),
            spearman_events_vs_rms=spearman(xs, ys), pearson_events_vs_rms=pearson(xs, ys),
            n_sta=len(ys))
    # --------------- (b) per LOS arc ---------------
    arcs = build_arcs(df, "code", gap_epochs, min_arc)
    ev_set = defaultdict(set)          # (sta, sat, band) -> {k}
    ev_any = defaultdict(set)          # (sta, sat)       -> {k}
    for r in events:
        k, sta, sat, b, _kd = r
        ev_any[(sta, sat)].add(k)
        if b:
            ev_set[(sta, sat, b)].add(k)
        else:
            for bb in ("L1", "L2", "L5"):
                ev_set[(sta, sat, bb)].add(k)
    arc_rows = []
    for a in arcs:
        b = BAND_OF.get(a["signal"])
        ks = set(int(x) for x in a["k"])
        n_in = len(ev_set.get((a["station"], a["sat"], b), set()) & ks)
        n_any = len(ev_any.get((a["station"], a["sat"]), set()) & ks)
        res = a["res"]
        arc_rows.append(dict(station=a["station"], sat=a["sat"], signal=a["signal"],
                             band=b, n=a["n"], k0=a["k0"], k1=a["k1"],
                             n_relevel_band=n_in, n_relevel_any=n_any,
                             growth=float(abs(res[:3].mean() - res[9:].mean())),
                             final_abs_level=float(abs(res[9:].mean())),
                             rate_per_hour=float(n_in / (a["n"] * DT_S / 3600.0))))
    def arc_corr(xf, yf):
        x = [r[xf] for r in arc_rows]
        y = [r[yf] for r in arc_rows]
        nm, np95 = perm_null(x, y, n=min(nshuffle, 500))
        xa, ya = np.asarray(x, float), np.asarray(y, float)
        tm = np.isfinite(xa) & np.isfinite(ya)
        if tm.sum() > 20:
            lo, hi = np.quantile(ya[tm], [0.01, 0.99])
            tm &= (ya >= lo) & (ya <= hi)
        return dict(n=len(x), r=pearson(x, y), rho=spearman(x, y),
                    r_trim=pearson(xa[tm], ya[tm]), n_trim=int(tm.sum()),
                    null_absr_mean=nm, null_absr_p95=np95)
    B = dict(
        n_arcs=len(arc_rows),
        frac_arcs_with_relevel=float(np.mean([r["n_relevel_band"] > 0 for r in arc_rows]))
        if arc_rows else float("nan"),
        growth_vs_n_relevel=arc_corr("n_relevel_band", "growth"),
        final_vs_n_relevel=arc_corr("n_relevel_band", "final_abs_level"),
        growth_vs_n_relevel_any=arc_corr("n_relevel_any", "growth"),
        med_growth_with=q([r["growth"] for r in arc_rows if r["n_relevel_band"] > 0], .5),
        med_growth_without=q([r["growth"] for r in arc_rows if r["n_relevel_band"] == 0], .5),
        med_final_with=q([r["final_abs_level"] for r in arc_rows if r["n_relevel_band"] > 0], .5),
        med_final_without=q([r["final_abs_level"] for r in arc_rows
                             if r["n_relevel_band"] == 0], .5))

    # --------------- (c) the wire ---------------
    import wire_vs_code_census as wc
    t_hi = TOW0 + DT_S * (max(hours) * ep_per_hour - 1)
    ser, evc, _c = _census_series(project, arm, TOW0, t_hi)
    # A T4 refloat is TRIGGERED BY a large post-fit residual, so a wire step beside it
    # is coincidence with unknown direction.  The T0/T1 detectors read RAW observables
    # (LLI bits, MW/GF, IF time-difference) and are solve-independent — measured here:
    # their counts are identical across all three arms — so an enrichment on THOSE
    # events does establish direction.  Split the population.
    DETECTOR_KINDS = ("T0_gap/arc_resume", "T0_mw_detect/lli_slip",
                      "T0_mw_detect/mw_wl_slip", "T1_if_slip/if_slip",
                      "T1_if_slip/refsat_reset")
    sat_ep = defaultdict(set)           # sat -> epochs where ANY station re-levelled it
    sat_ep_det = defaultdict(set)       # ... restricted to the raw-observable detectors
    sat_ep_t4 = defaultdict(set)        # ... restricted to the post-fit T4 refloat
    for k, sta, sat, _b, _kd in events:
        sat_ep[sat].add(int(k))
        if _kd in DETECTOR_KINDS:
            sat_ep_det[sat].add(int(k))
        elif _kd == "T4_attrib/attrib_arc":
            sat_ep_t4[sat].add(int(k))
    kspan = np.arange(1, max(hours) * ep_per_hour + 1)
    C, per_sat = {}, {}
    pooled_near, pooled_away = [], []
    for sat in sorted({key[0] if isinstance(key, tuple) else key for key in ser["phase"]}):
        steps = []                       # (k_of_second_epoch, |dD|)
        for key, s_ in ser["phase"].items():
            if (key[0] if isinstance(key, tuple) else key) != sat:
                continue
            for arc in wc.split_arcs(s_, evc.get(sat, {}), declared=False):
                for p, qq in zip(arc[:-1], arc[1:]):
                    t0, d0, _ = s_[p]
                    t1, d1, _ = s_[qq]
                    if k_of(t1) - k_of(t0) == 1:
                        steps.append((k_of(t1), abs(d1 - d0)))
        if not steps:
            continue
        ks = np.array([k for k, _ in steps])
        dv = np.array([v for _, v in steps], float)
        E = np.array(sorted(sat_ep.get(sat, ())), dtype=int)
        if E.size == 0:
            continue
        def enrich(bset):
            near = np.isin(ks, bset)
            for j in range(1, near_ep + 1):
                near |= np.isin(ks - j, bset) | np.isin(ks + j, bset)
            if not near.any() or near.all():
                return float("nan"), float("nan"), float("nan"), near
            return (float(dv[near].mean()), float(dv[~near].mean()),
                    float(dv[near].mean() / dv[~near].mean()), near)
        mn, ma, ratio, near = enrich(E)
        nulls = np.empty(nshuffle)
        for i in range(nshuffle):
            _a, _b2, rr, _n = enrich(rng.choice(kspan, size=E.size, replace=False))
            nulls[i] = rr
        pooled_near.extend(dv[near].tolist())
        pooled_away.extend(dv[~near].tolist())
        sub = {}
        for tag, S in (("detector", sat_ep_det.get(sat, ())),
                       ("t4", sat_ep_t4.get(sat, ()))):
            Es = np.array(sorted(S), dtype=int)
            if Es.size == 0:
                sub[tag] = dict(n=0, enrichment=float("nan"), null_mean=float("nan"),
                                null_p95=float("nan"))
                continue
            m2, a2, r2, _n2 = enrich(Es)
            nl = np.empty(min(nshuffle, 300))
            for i in range(len(nl)):
                _x, _y, rr2, _z = enrich(rng.choice(kspan, size=Es.size, replace=False))
                nl[i] = rr2
            sub[tag] = dict(n=int(Es.size), enrichment=r2,
                            null_mean=float(np.nanmean(nl)),
                            null_p95=float(np.nanquantile(nl, .95)))
        per_sat[sat] = dict(n_steps=int(len(steps)), n_relevel_epochs=int(E.size),
                            coverage=float(near.mean()),
                            mean_absdD_near=mn, mean_absdD_away=ma, enrichment=ratio,
                            null_mean=float(np.nanmean(nulls)),
                            null_p95=float(np.nanquantile(nulls, .95)), **sub)
    C["per_sat"] = per_sat
    C["pooled"] = dict(
        n_near=len(pooled_near), n_away=len(pooled_away),
        mean_near=float(np.mean(pooled_near)) if pooled_near else float("nan"),
        mean_away=float(np.mean(pooled_away)) if pooled_away else float("nan"),
        enrichment=float(np.mean(pooled_near) / np.mean(pooled_away))
        if pooled_away and np.mean(pooled_away) else float("nan"),
        med_enrichment_per_sat=q([v["enrichment"] for v in per_sat.values()], .5),
        med_null_per_sat=q([v["null_mean"] for v in per_sat.values()], .5),
        med_null_p95_per_sat=q([v["null_p95"] for v in per_sat.values()], .5),
        frac_sats_above_own_null_p95=float(np.mean(
            [v["enrichment"] > v["null_p95"] for v in per_sat.values()]))
        if per_sat else float("nan"),
        med_enrich_detector=q([v["detector"]["enrichment"] for v in per_sat.values()], .5),
        med_null_detector=q([v["detector"]["null_mean"] for v in per_sat.values()], .5),
        frac_above_null_p95_detector=float(np.nanmean(
            [1.0 if (np.isfinite(v["detector"]["enrichment"])
                     and v["detector"]["enrichment"] > v["detector"]["null_p95"]) else 0.0
             for v in per_sat.values() if v["detector"]["n"] > 0])),
        med_enrich_t4=q([v["t4"]["enrichment"] for v in per_sat.values()], .5),
        med_null_t4=q([v["t4"]["null_mean"] for v in per_sat.values()], .5),
        frac_above_null_p95_t4=float(np.nanmean(
            [1.0 if (np.isfinite(v["t4"]["enrichment"])
                     and v["t4"]["enrichment"] > v["t4"]["null_p95"]) else 0.0
             for v in per_sat.values() if v["t4"]["n"] > 0])))

    # --------------- (d) the rate ---------------
    # denominator: (station, satellite) LOS-hours actually observed
    los_h = code.groupby(["station", "sat", "hour"], observed=True).size().reset_index()
    los_hours_by_sta = los_h.groupby("station", observed=True).size().to_dict()
    ev_by_sta = ev_df.groupby("station", observed=True).size().to_dict()
    elev = {}
    for r in df[["station", "sat", "k", "elev_deg"]].itertuples(index=False):
        elev[(str(r.station), str(r.sat), int(r.k))] = float(r.elev_deg)
    ev_el = [elev.get((sta, sat, int(k))) for k, sta, sat, _b, _kd in events]
    ev_el = np.array([x for x in ev_el if x is not None], float)
    all_el = code["elev_deg"].to_numpy(float)
    edges = [10, 20, 30, 45, 60, 91]
    el_bins = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        n_ev = int(((ev_el >= lo) & (ev_el < hi)).sum())
        n_rows = int(((all_el >= lo) & (all_el < hi)).sum())
        el_bins.append(dict(bin=f"{lo}-{hi}", n_events=n_ev, n_code_rows=n_rows,
                            events_per_1k_rows=float(1000.0 * n_ev / n_rows)
                            if n_rows else float("nan")))
    sta_rate = []
    for sta, lh in sorted(los_hours_by_sta.items()):
        n = ev_by_sta.get(sta, 0)
        sta_rate.append(dict(station=str(sta), n_events=int(n), los_hours=int(lh),
                             rate_per_los_hour=float(n / lh) if lh else float("nan"),
                             carryless=str(sta) in carryless))
    sta_rate.sort(key=lambda r: -r["rate_per_los_hour"])
    cl = [r["rate_per_los_hour"] for r in sta_rate if r["carryless"]]
    rest = [r["rate_per_los_hour"] for r in sta_rate if not r["carryless"]]
    D = dict(by_elev=el_bins, by_station=sta_rate,
             carryless_p50=q(cl, .5), rest_p50=q(rest, .5),
             carryless_max=float(max(cl)) if cl else float("nan"),
             rest_max=float(max(rest)) if rest else float("nan"),
             n_carryless=len(cl), n_rest=len(rest),
             ratio=float(q(cl, .5) / q(rest, .5)) if rest and q(rest, .5) else float("nan"))

    # contrast arms: the headline counts only
    contrast = {}
    for c in contrasts:
        p = os.path.join(arm_dir(project, c), "logs", "network_events.csv")
        if not os.path.isfile(p):
            continue
        _hh, rr = _scan_csv(p, lambda d, f: (d.get("tier"), d.get("event")) in RELEVEL_KINDS,
                            limit=10 ** 7)
        ot = _scan_lines(os.path.join(arm_dir(project, c), "stdout.txt"))
        re_ = {}
        for pat, key in ((r"(\d+) detector-marked \(station, amb\) free-epoch latches",
                          "free_epoch_latches"),
                         (r"(\d+) inject-site skips actually taken", "inject_site_skips")):
            for ln in ot:
                m = re.search(pat, ln)
                if m:
                    re_[key] = int(m.group(1))
                    break
        contrast[c] = dict(n_rows=len(rr), run_end=re_, by_kind=dict(
            (f"{k[0]}/{k[1]}", sum(1 for ln in rr
                                   if (ln.split(",")[2], ln.split(",")[3]) == k))
            for k in RELEVEL_KINDS))
    return dict(arm=arm, hours=hours, n_event_rows=len(rows), n_expanded=len(events),
                kinds={f"{a}/{b}": dict(n=len(v), sample=v[0]) for (a, b), v in kinds.items()},
                run_end=runend, n_arc_refloat_epoch_status=n_arc_refloat,
                per_hour=per_hour, arcs=B, wire=C, rate=D, contrast=contrast)


def _scan_lines(path):
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.readlines()


def fmt_r9(R):
    L = [f"R9 - sec 24.19: the slip response's own mid-arc RE-LEVELS  ({R['arm']})",
         "frozen reading: (c) enrichment > 1.5 over the null AND (b) r > 0.5 => the re-leveling",
         "  IS the wire's random walk and the drift's channel; (c) ~ 1 and (b) < 0.3 => exonerated;",
         "  a rate > 1 per LOS per hour at the eight carry-less stations against < 0.3 elsewhere",
         "  => the rate is the bad-station signature (false slips on noisy code).",
         "",
         "(a) THE EVENT POPULATION - every (tier, event) this producer writes that frees or",
         "    re-seeds a phase level mid-arc, one quoted row each:"]
    for k, v in sorted(R["kinds"].items(), key=lambda kv: -kv[1]["n"]):
        L.append(f"  {k:34s} n = {v['n']:5d}")
        L.append(f"      | {v['sample']}")
    L += ["",
          f"  rows {R['n_event_rows']} -> {R['n_expanded']} (station, sat, BAND, epoch) events",
          f"  RECONCILIATION against the producer's own voice:",
          f"    run-end 'detector-marked (station, amb) free-epoch latches' : "
          f"{R['run_end'].get('free_epoch_latches')}",
          f"    run-end 'inject-site skips actually taken'                  : "
          f"{R['run_end'].get('inject_site_skips')}",
          f"    run-end 'RESUMPTION(s)' (slip-QC card G gap instrumentation): "
          f"{R['run_end'].get('gap_resumptions')}",
          f"    epoch_status n_arc_refloat summed over the run              : "
          f"{R['n_arc_refloat_epoch_status']:.0f}"]
    L += ["", "(a) per hour, beside (i)",
          f"  {'h':>2s} {'events':>7s} {'stations':>9s} {'(i) p50':>8s} {'(i) p90':>8s} "
          f"{'rho(events, rms)':>17s} {'r':>7s} {'n_sta':>6s}"]
    for h, p in R["per_hour"].items():
        L.append(f"  {h:2d} {p['n_events']:7d} {p['n_stations_with_event']:9d} "
                 f"{f(p['i_p50'],3)!s:>8s} {f(p['i_p90'],3)!s:>8s} "
                 f"{f(p['spearman_events_vs_rms'])!s:>17s} "
                 f"{f(p['pearson_events_vs_rms'])!s:>7s} {p['n_sta']:6d}")
    B = R["arcs"]
    L += ["", f"(b) per LOS code arc (>= 30 rows): {B['n_arcs']} arcs, "
              f"{f(B['frac_arcs_with_relevel'],3)} carry at least one same-band re-level",
          f"  {'read':34s} {'n':>6s} {'r':>7s} {'rho':>7s} {'r_trim':>7s} {'NULL':>6s} {'NULLp95':>8s}"]
    for key in ("growth_vs_n_relevel", "final_vs_n_relevel", "growth_vs_n_relevel_any"):
        c = B[key]
        L.append(f"  {key:34s} {c['n']:6d} {f(c['r'])!s:>7s} {f(c['rho'])!s:>7s} "
                 f"{f(c['r_trim'])!s:>7s} {f(c['null_absr_mean'])!s:>6s} "
                 f"{f(c['null_absr_p95'])!s:>8s}")
    L.append(f"  median growth  with a re-level {f(B['med_growth_with'],3)} m  vs without "
             f"{f(B['med_growth_without'],3)} m")
    L.append(f"  median |level| with a re-level {f(B['med_final_with'],3)} m  vs without "
             f"{f(B['med_final_without'],3)} m")
    P = R["wire"]["pooled"]
    L += ["", "(c) THE WIRE - |dD_phase| at epochs within +-1 of a re-level of that satellite",
          f"  pooled: near {f(P['mean_near'],4)} m ({P['n_near']} steps) vs away "
          f"{f(P['mean_away'],4)} m ({P['n_away']}) -> enrichment {f(P['enrichment'],3)}",
          f"  per satellite: median enrichment {f(P['med_enrichment_per_sat'],3)} against a "
          f"median shuffled null of {f(P['med_null_per_sat'],3)} (null p95 "
          f"{f(P['med_null_p95_per_sat'],3)});",
          f"    satellites above their OWN null p95: {f(P['frac_sats_above_own_null_p95'],3)} "
          "(0.05 is chance)",
          "  DIRECTION SPLIT - a T4 refloat is TRIGGERED BY a large post-fit residual, so a",
          "    wire step beside it is coincidence of unknown direction.  The T0/T1 detectors read",
          "    RAW observables and are solve-independent (their counts are identical in all three",
          "    arms, below), so an enrichment on THOSE establishes direction:",
          f"    raw DETECTOR events : median enrichment {f(P['med_enrich_detector'],3)} vs null "
          f"{f(P['med_null_detector'],3)}; above own null p95 in "
          f"{f(P['frac_above_null_p95_detector'],3)} of satellites",
          f"    post-fit T4 refloat : median enrichment {f(P['med_enrich_t4'],3)} vs null "
          f"{f(P['med_null_t4'],3)}; above own null p95 in {f(P['frac_above_null_p95_t4'],3)}",
          f"  {'sat':4s} {'steps':>6s} {'relev ep':>9s} {'cover':>6s} {'near':>7s} {'away':>7s} "
          f"{'enrich':>7s} {'null':>6s} {'nullp95':>8s} {'det':>6s} {'T4':>6s}"]
    for sat in ("G29", "G28", "G32", "G18"):
        v = R["wire"]["per_sat"].get(sat)
        if not v:
            continue
        L.append(f"  {sat:4s} {v['n_steps']:6d} {v['n_relevel_epochs']:9d} "
                 f"{f(v['coverage'],3)!s:>6s} {f(v['mean_absdD_near'],4)!s:>7s} "
                 f"{f(v['mean_absdD_away'],4)!s:>7s} {f(v['enrichment'],3)!s:>7s} "
                 f"{f(v['null_mean'],3)!s:>6s} {f(v['null_p95'],3)!s:>8s} "
                 f"{f(v['detector']['enrichment'],2)!s:>6s} {f(v['t4']['enrichment'],2)!s:>6s}")
    D = R["rate"]
    L += ["", "(d) THE RATE",
          f"  by elevation: {'bin':>8s} {'events':>8s} {'code rows':>10s} {'events/1k rows':>15s}"]
    for b in D["by_elev"]:
        L.append(f"{'':16s}{b['bin']:>8s} {b['n_events']:8d} {b['n_code_rows']:10d} "
                 f"{f(b['events_per_1k_rows'],3)!s:>15s}")
    L += [f"  the eight carry-less stations: rate per LOS-hour p50 {f(D['carryless_p50'],3)} "
          f"(max {f(D['carryless_max'],3)}, n {D['n_carryless']}) vs the rest p50 "
          f"{f(D['rest_p50'],3)} (max {f(D['rest_max'],3)}, n {D['n_rest']}) -> ratio "
          f"{f(D['ratio'],2)}",
          f"  {'station':11s} {'events':>7s} {'LOS-hours':>10s} {'rate/LOS-h':>11s}  (worst 12)"]
    for r in D["by_station"][:12]:
        L.append(f"  {r['station']:11s}{'*' if r['carryless'] else ' '}{r['n_events']:6d} "
                 f"{r['los_hours']:10d} {f(r['rate_per_los_hour'],3)!s:>11s}")
    L.append("    (* = one of the eight rx-CB carry-less stations)")
    if R["contrast"]:
        L += ["", "  contrast arms (event rows + the run-end latch counters):"]
        for c, v in R["contrast"].items():
            L.append(f"    {c:12s} rows {v['n_rows']:5d}  run-end {v['run_end']}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
# witness — per-station code rms vs arcs born inside the run
# --------------------------------------------------------------------------- #
def read_witness(project, arms, gap_epochs, min_arc):
    out = {}
    for arm in arms:
        df = load_pf_rows(project, arm)
        arcs = build_arcs(df, "code", gap_epochs, min_arc)
        born = defaultdict(int)
        tot = defaultdict(int)
        for a in arcs:
            tot[a["station"]] += 1
            if a["k0"] > 1:
                born[a["station"]] += 1
        keyed = pd.read_csv(os.path.join(arm_dir(project, arm), "logs", "network_pf_keyed.csv"))
        keyed["k"] = np.rint((keyed["tow"].to_numpy() - TOW0) / DT_S).astype(int) + 1
        s = keyed[(keyed["kind"] == "STA") & (keyed["k"] >= K_SETTLED_LO) & (keyed["k"] <= K_SETTLED_HI)]
        med = s.groupby("key")["rms_code"].median()
        rows = []
        for sta, rms in med.items():
            rows.append(dict(station=str(sta), rms_code=float(rms),
                             n_born=int(born.get(sta, 0)), n_arcs=int(tot.get(sta, 0))))
        rows.sort(key=lambda r: -r["rms_code"])
        x = [r["n_born"] for r in rows]
        y = [r["rms_code"] for r in rows]
        out[arm] = dict(n_stations=len(rows), pearson=pearson(x, y), spearman=spearman(x, y),
                        med_born=float(np.median(x)), med_rms=float(np.median(y)), rows=rows)
    return out


def fmt_witness(res):
    L = ["WITNESS — per-station code rms (pf_keyed STA, last-hour median) vs arcs born inside the run"]
    for arm, r in res.items():
        L.append(f"\n=== {arm} ===  n_sta {r['n_stations']}  Pearson {f(r['pearson'])}  "
                 f"Spearman {f(r['spearman'])}  med born {r['med_born']:.0f}  med rms {f(r['med_rms'])}")
        L.append(f"  {'station':11s} {'rms_code':>9s} {'n_born':>7s} {'n_arcs':>7s}   (worst 15 / best 5)")
        for rr in r["rows"][:15] + [dict(station="...", rms_code=float("nan"),
                                         n_born=0, n_arcs=0)] + r["rows"][-5:]:
            L.append(f"  {rr['station']:11s} {f(rr['rms_code'])!s:>9s} {rr['n_born']:7d} {rr['n_arcs']:7d}")
    return "\n".join(L)


# --------------------------------------------------------------------------- #
def emit(out_dir, name, obj, text):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, name + ".json"), "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=1, default=str)
    with open(os.path.join(out_dir, name + ".txt"), "w", encoding="utf-8") as fh:
        fh.write(text + "\n")
    # the file is UTF-8; the Windows console is cp1252, so never let a glyph kill the run
    enc = getattr(sys.stdout, "encoding", None) or "utf-8"
    sys.stdout.write(text.encode(enc, errors="replace").decode(enc, errors="replace") + "\n")
    print(f"\n[wrote {os.path.join(out_dir, name)}.json / .txt]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("read", choices=["r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
                                     "witness", "all"])
    ap.add_argument("--project", default=PROJECT_DEFAULT)
    ap.add_argument("--arms", default=",".join(ARMS_DEFAULT))
    ap.add_argument("--out", default=None)
    ap.add_argument("--gap-epochs", type=int, default=5)
    ap.add_argument("--min-arc", type=int, default=30)
    ap.add_argument("--min-settled-rows", type=int, default=30)
    ap.add_argument("--step", type=float, default=0.3)
    ap.add_argument("--shuffles", type=int, default=1000)
    ap.add_argument("--sat", default="G06", help="r5: the satellite under test")
    ap.add_argument("--station", default="DJIG00DJI", help="r5b: the station under test")
    ap.add_argument("--tow", type=float, default=265470.0, help="r5c/r5d: the election epoch")
    ap.add_argument("--r9-arm", default="_gl_on6", help="r9: the arm under test")
    ap.add_argument("--r9-contrasts", default="_gl_rxoff6,_cm_c10",
                    help="r9: arms read for their event counts only")
    ap.add_argument("--r8-arm", default="_gl_on6", help="r8: the arm under test")
    ap.add_argument("--r8-contrast", default="_cm_c10", help="r8: the no-screen contrast")
    ap.add_argument("--r7-min-arc", type=int, default=20,
                    help="r7: minimum declared-arc length in epochs")
    ap.add_argument("--r6-sats", default=",".join(R6_SATS + R6_CONTROLS),
                    help="r6: satellites under test (the first four are flagged as the noisy set)")
    a = ap.parse_args()
    arms = [s for s in a.arms.split(",") if s]
    out_dir = a.out or os.path.join(a.project, "runs", "_lvseed")

    todo = ["r1", "r2", "r3", "witness"] if a.read == "all" else [a.read]
    for t in todo:
        if t == "r1":
            r = read_r1(a.project, arms, a.gap_epochs, a.min_arc)
            emit(out_dir, "r1", r, fmt_r1(r))
        elif t == "r2":
            r = read_r2(a.project, arms, a.min_settled_rows)
            emit(out_dir, "r2", r, fmt_r2(r))
        elif t == "r3":
            r = read_r3(a.project, arms, a.min_settled_rows)
            emit(out_dir, "r3", r, fmt_r3(r))
        elif t == "r4":
            r = read_r4(a.project, arms, a.gap_epochs, a.min_arc,
                        step_thresh=a.step, n_shuffle=a.shuffles)
            emit(out_dir, "r4", r, fmt_r4(r))
        elif t == "r5":
            r = dict(a=read_r5a(a.project, arms, sat=a.sat),
                     b=read_r5b(a.project, arms, station=a.station, sat=a.sat),
                     c=read_r5c(a.project, arms, tow_focus=a.tow, sat=a.sat),
                     d=read_r5d(a.project, arms, tow_focus=a.tow, sat=a.sat))
            emit(out_dir, "r5", r, fmt_r5(r))
        elif t == "r6":
            r = read_r6(a.project, arm=arms[0],
                        sats=[s for s in a.r6_sats.split(",") if s] or None)
            emit(out_dir, "r6", r, fmt_r6(r))
        elif t == "r7":
            r = read_r7(a.project, arm=arms[0], min_arc_ep=a.r7_min_arc)
            emit(out_dir, "r7", r, fmt_r7(r))
        elif t == "r8":
            r = read_r8(a.project, arm=a.r8_arm, contrast=a.r8_contrast,
                        min_rows=a.min_settled_rows)
            # one output per arm — R8b reads two arms and they must not collide
            emit(out_dir, f"r8{a.r8_arm}", r, fmt_r8(r))
        elif t == "r9":
            r = read_r9(a.project, arm=a.r9_arm,
                        contrasts=tuple(s for s in a.r9_contrasts.split(",") if s))
            emit(out_dir, f"r9{a.r9_arm}", r, fmt_r9(r))
        elif t == "witness":
            r = read_witness(a.project, arms, a.gap_epochs, a.min_arc)
            emit(out_dir, "witness", r, fmt_witness(r))


if __name__ == "__main__":
    main()
