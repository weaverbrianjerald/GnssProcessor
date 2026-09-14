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
    ap.add_argument("read", choices=["r1", "r2", "r3", "r4", "r5", "witness", "all"])
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
        elif t == "witness":
            r = read_witness(a.project, arms, a.gap_epochs, a.min_arc)
            emit(out_dir, "witness", r, fmt_witness(r))


if __name__ == "__main__":
    main()
