#!/usr/bin/env python3
"""
field_consumer_residual_census.py — what drives the REMAINING closed-loop
consumer residual on a NETWORK field run.

Analysis-only.  Reads a completed run's traces (nothing is re-run, no C++ is
touched) and answers one question: after a posture change has removed the
gross vertical bias, is the leftover consumer error slow (a wandering state:
tropo / clock / datum) or fast (per-epoch noise: geometry / iono / integers),
and which producer-side EVENTS coincide with the bad blocks.

Instruments
-----------
  logs/closed_loop_trace.csv    consumer position error (east/north/up, 3D)
  logs/network_zwd.csv          per-station producer ZWD  (the tropo channel)
  logs/network_events.csv       tiered producer events (gauge/pivot/slip/...)
  logs/network_epoch_status.csv per-epoch counts + sigma0
  logs/network_stations.csv     roster + ECEF (locates the held-out consumer)
  ambiguity_trace.csv           consumer ambiguity resets — OPTIONAL, only
                                written when logging.enable_ambiguity_trace
                                is armed; reported as MISSING otherwise.

Computed on the SETTLED window only (>= --settle-h after the first epoch):

  1. Character   ACF of each component about its mean; the lag where ACF
                 first falls below 0.5 (and 1/e); a slow/fast variance split
                 from 30-min block means vs pooled within-block scatter.
  2. Tropo       corr(UP, fleet-median per-station ZWD deviation), then
                 corr(UP, per-station ZWD deviation) for every station, with
                 each station's settled ZWD span and its distance from the
                 consumer.  Plus the consumer's OWN ZWD if any trace carries
                 it (a static receiver whose vertical is being driven through
                 the wet mapping shows a UP/ZWD slope near -3 .. -4).
  3. Blocks      per 30-min block: UP rms, 3D rms, |UP mean| vs the block's
                 event counts; Spearman rho of every count against the block
                 error; then the worst blocks itemised by tier.
  4. E/N         hourly rms of east/north vs n_sats, vs local solar time at
                 the consumer (an ionospheric-activity proxy), and vs the
                 refloat/reset counts.
  5. Compare     pass more than one run dir and every scalar is tabulated
                 side by side, first run first.

Usage
-----
  python scripts/field_consumer_residual_census.py <run_dir> [<run_dir> ...]
         [--settle-h 6] [--out-dir DIR] [--json PATH] [--block-min 30]

Writes <out-dir>/<run_name>.txt and <out-dir>/<run_name>.json per run, plus
<out-dir>/compare.txt when more than one run is given.  Everything printed to
stdout as well.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd

# --------------------------------------------------------------------------
# small stats helpers (scipy is not a dependency of this repo)
# --------------------------------------------------------------------------


def _finite_pair(x, y):
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    m = np.isfinite(x) & np.isfinite(y)
    return x[m], y[m]


def pearson(x, y):
    """Pearson r; nan if fewer than 3 finite pairs or either side is constant."""
    x, y = _finite_pair(x, y)
    if x.size < 3:
        return float("nan")
    sx, sy = x.std(), y.std()
    if sx == 0 or sy == 0:
        return float("nan")
    return float(np.corrcoef(x, y)[0, 1])


def _rank(a):
    """Average-tie ranks."""
    a = np.asarray(a, dtype=float)
    order = a.argsort(kind="mergesort")
    ranks = np.empty(a.size, dtype=float)
    ranks[order] = np.arange(a.size, dtype=float)
    # average ties
    srt = a[order]
    i = 0
    while i < srt.size:
        j = i
        while j + 1 < srt.size and srt[j + 1] == srt[i]:
            j += 1
        if j > i:
            ranks[order[i : j + 1]] = np.arange(i, j + 1).mean()
        i = j + 1
    return ranks


def spearman(x, y):
    """Spearman rho + a two-sided p from the t approximation."""
    x, y = _finite_pair(x, y)
    n = x.size
    if n < 4:
        return float("nan"), float("nan"), n
    rho = pearson(_rank(x), _rank(y))
    if not np.isfinite(rho) or abs(rho) >= 1.0:
        return float(rho), 0.0 if np.isfinite(rho) else float("nan"), n
    t = rho * math.sqrt((n - 2) / max(1e-15, 1.0 - rho * rho))
    return float(rho), float(_t_sf2(abs(t), n - 2)), n


def _betacf(a, b, x, itmax=200, eps=3e-12):
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < 1e-30:
        d = 1e-30
    d = 1.0 / d
    h = d
    for m in range(1, itmax + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-30:
            d = 1e-30
        c = 1.0 + aa / c
        if abs(c) < 1e-30:
            c = 1e-30
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-30:
            d = 1e-30
        c = 1.0 + aa / c
        if abs(c) < 1e-30:
            c = 1e-30
        d = 1.0 / d
        de = d * c
        h *= de
        if abs(de - 1.0) < eps:
            break
    return h


def _betainc(a, b, x):
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lb = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1.0 - x)
    bt = math.exp(lb)
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def _t_sf2(t, df):
    """Two-sided survival for Student-t."""
    if df <= 0:
        return float("nan")
    return float(_betainc(0.5 * df, 0.5, df / (df + t * t)))


def acf(x, max_lag):
    """Autocorrelation of x about its mean, lags 0..max_lag."""
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    n = x.size
    if n < 8:
        return np.array([np.nan])
    d = x - x.mean()
    denom = float(np.dot(d, d))
    if denom <= 0:
        return np.array([np.nan])
    max_lag = int(min(max_lag, n - 2))
    return np.array([float(np.dot(d[: n - k], d[k:]) / denom) for k in range(max_lag + 1)])


def first_lag_below(a, thresh):
    """First lag index whose ACF value drops below thresh (nan if never)."""
    for k in range(1, a.size):
        if np.isfinite(a[k]) and a[k] < thresh:
            return k
    return float("nan")


def slow_fast_split(t, v, block_s):
    """
    Variance decomposition of v(t) into a between-block (slow wander) part and
    a within-block (fast noise) part, at a block length of block_s seconds.
    Returns (sd_block_means, pooled_within_sd, frac_slow, n_blocks).
    """
    t = np.asarray(t, dtype=float)
    v = np.asarray(v, dtype=float)
    m = np.isfinite(v)
    t, v = t[m], v[m]
    if t.size < 4:
        return float("nan"), float("nan"), float("nan"), 0
    b = np.floor((t - t.min()) / block_s).astype(int)
    means, wss, wdf = [], 0.0, 0
    for bi in np.unique(b):
        s = v[b == bi]
        if s.size < 2:
            continue
        means.append(s.mean())
        wss += float(((s - s.mean()) ** 2).sum())
        wdf += s.size - 1
    if len(means) < 2 or wdf < 1:
        return float("nan"), float("nan"), float("nan"), len(means)
    sd_between = float(np.std(means, ddof=1))
    sd_within = float(math.sqrt(wss / wdf))
    tot = sd_between**2 + sd_within**2
    frac = sd_between**2 / tot if tot > 0 else float("nan")
    return sd_between, sd_within, frac, len(means)


def lag1(x):
    """Lag-1 autocorrelation (used for the effective-sample-size correction)."""
    a = acf(np.asarray(x, dtype=float), 1)
    return float(a[1]) if a.size > 1 and np.isfinite(a[1]) else 0.0


def corr_autocorr_aware(x, y):
    """
    Pearson r plus a p-value that accounts for SERIAL CORRELATION in both
    series.  Everything in this census is a slow-varying state, so the naive
    n is wildly optimistic: two AR(1) series with r1 ~ 0.97 have an effective
    sample size ~1.5 % of n.  Bartlett/Quenouille first-order correction:
        n_eff = n * (1 - r1x*r1y) / (1 + r1x*r1y)
    Returns (r, n, n_eff, p_eff).
    """
    x, y = _finite_pair(x, y)
    n = x.size
    if n < 8:
        return float("nan"), n, float("nan"), float("nan")
    r = pearson(x, y)
    r1x, r1y = lag1(x), lag1(y)
    fac = (1.0 - r1x * r1y) / (1.0 + r1x * r1y) if (1.0 + r1x * r1y) > 1e-9 else 0.0
    n_eff = max(3.0, n * max(0.0, fac))
    if not np.isfinite(r) or abs(r) >= 1.0 or n_eff <= 2:
        return r, n, n_eff, float("nan")
    t = r * math.sqrt((n_eff - 2) / max(1e-15, 1 - r * r))
    return float(r), int(n), float(n_eff), float(_t_sf2(abs(t), n_eff - 2))


def rms(v):
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    return float(math.sqrt(float((v**2).mean()))) if v.size else float("nan")


def ecef_to_geodetic(x, y, z):
    """WGS-84 ECEF -> (lat_deg, lon_deg, h_m), Bowring."""
    a, f = 6378137.0, 1.0 / 298.257223563
    b = a * (1 - f)
    e2 = f * (2 - f)
    ep2 = (a * a - b * b) / (b * b)
    p = math.hypot(x, y)
    th = math.atan2(z * a, p * b)
    lat = math.atan2(z + ep2 * b * math.sin(th) ** 3, p - e2 * a * math.cos(th) ** 3)
    lon = math.atan2(y, x)
    n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
    h = p / math.cos(lat) - n if abs(math.cos(lat)) > 1e-12 else abs(z) - b
    return math.degrees(lat), math.degrees(lon), h


def great_circle_km(lat1, lon1, lat2, lon2):
    r = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(h)))


# --------------------------------------------------------------------------
# trace loading
# --------------------------------------------------------------------------

ZWD_COL_RE = re.compile(r"zwd|trop|wet|ztd", re.I)


def _logs(run: Path) -> Path:
    return run / "logs" if (run / "logs").is_dir() else run


def load_consumer(run: Path, missing: list):
    p = _logs(run) / "closed_loop_trace.csv"
    if not p.exists():
        missing.append(str(p))
        return None
    d = pd.read_csv(p)
    d = d.dropna(subset=["tow"]).sort_values("tow").reset_index(drop=True)
    return d


def load_zwd(run: Path, missing: list):
    p = _logs(run) / "network_zwd.csv"
    if not p.exists():
        missing.append(str(p))
        return None
    return pd.read_csv(p)


def load_events(run: Path, missing: list):
    p = _logs(run) / "network_events.csv"
    if not p.exists():
        missing.append(str(p))
        return None
    return pd.read_csv(p, low_memory=False)


def load_status(run: Path, missing: list):
    p = _logs(run) / "network_epoch_status.csv"
    if not p.exists():
        missing.append(str(p))
        return None
    return pd.read_csv(p, low_memory=False)


def load_stations(run: Path, missing: list):
    p = _logs(run) / "network_stations.csv"
    if not p.exists():
        missing.append(str(p))
        return None
    return pd.read_csv(p)


def load_amb_resets(run: Path, missing: list):
    """
    Consumer ambiguity resets per epoch.  A reset shows as std_m jumping back
    up toward its initial (large) value for a key that was previously tight.
    Requires logging.enable_ambiguity_trace; absent on most network runs.
    """
    for p in (run / "ambiguity_trace.csv", _logs(run) / "ambiguity_trace.csv"):
        if p.exists():
            d = pd.read_csv(p)
            tcol = "tow_s" if "tow_s" in d.columns else "tow"
            d = d.sort_values([ "key", tcol])
            prev = d.groupby("key")["std_m"].shift(1)
            # a reset: std grows by >5x and lands above 1 m (initial-prior scale)
            hit = (d["std_m"] > 5.0 * prev) & (d["std_m"] > 1.0)
            out = d.loc[hit].groupby(tcol).size().rename("n_amb_reset")
            return out.reset_index().rename(columns={tcol: "tow"}), str(p)
    missing.append(
        f"{run.name}/ambiguity_trace.csv (consumer ambiguity resets; needs "
        f"logging.enable_ambiguity_trace — NOT armed on this run)"
    )
    return None, None


def geometry_worst_direction(run: Path, station: str, t_settle: float, missing: list):
    """
    The direction a static single-receiver solve determines WORST, computed
    from the REAL line-of-sight geometry at `station` (the consumer is held
    out of the network, so its co-located twin stands in — same sky).

    Per epoch, build G = [-e_E, -e_N, -e_U, 1] over the tracked satellites
    (the trailing 1 is the estimated receiver clock, which is what tilts the
    worst direction away from the vertical), invert G'G, and take the
    position block's largest-eigenvalue eigenvector.  Averaging the position
    cofactor over the settled epochs gives the window's worst-determined
    axis.  If the residual's PCA axis lands on it, the error is a common-mode
    range-like quantity being dumped into the geometric null — not a
    direction-specific defect.
    """
    p = _logs(run) / "network_ionosphere.csv"
    if not p.exists():
        missing.append(f"{p} (needed for the geometry cross-check)")
        return None
    try:
        d = pd.read_csv(p, usecols=["tow", "station", "sat", "elevation_deg", "azimuth_deg"])
    except (ValueError, OSError) as e:
        missing.append(f"{p} ({e})")
        return None
    d = d[(d["station"] == station) & (d["tow"] >= t_settle)]
    if len(d) < 100:
        return None
    Qsum = np.zeros((3, 3))
    n_ep = 0
    el_min = float(d["elevation_deg"].min())
    for _tow, g in d.groupby("tow"):
        if len(g) < 5:
            continue
        el = np.radians(g["elevation_deg"].to_numpy(float))
        az = np.radians(g["azimuth_deg"].to_numpy(float))
        ce = np.cos(el)
        G = np.column_stack([-ce * np.sin(az), -ce * np.cos(az), -np.sin(el), np.ones(el.size)])
        try:
            Q = np.linalg.inv(G.T @ G)
        except np.linalg.LinAlgError:
            continue
        Qsum += Q[:3, :3]
        n_ep += 1
    if n_ep < 10:
        return None
    # sky occupancy: 8 azimuth sectors x 4 elevation bands, LOS-epochs.
    # A mid-northern-latitude site has a genuine VOID toward true north above
    # ~45 deg elevation (GPS/Galileo inclination 55 deg), and any wire error
    # on the few satellites that do cross it gets outsized (N,U) leverage.
    az_edges = np.arange(0, 361, 45)
    el_edges = np.array([0, 30, 45, 60, 90])
    H, _, _ = np.histogram2d(
        d["azimuth_deg"].to_numpy(float) % 360.0,
        d["elevation_deg"].to_numpy(float),
        bins=[az_edges, el_edges],
    )
    sky = {
        "az_edges": az_edges.tolist(),
        "el_edges": el_edges.tolist(),
        "counts": H.astype(int).tolist(),
        "total": int(H.sum()),
    }
    Q = Qsum / n_ep
    w, V = np.linalg.eigh(Q)
    idx = np.argsort(w)[::-1]
    w, V = w[idx], V[:, idx]
    v = V[:, 0]
    if v[2] < 0:
        v = -v
    e, n_, u = v
    return {
        "station": station,
        "n_epochs": n_ep,
        "elev_cutoff_deg": el_min,
        "worst_axis_enu": [float(e), float(n_), float(u)],
        "worst_axis_elev_deg": float(math.degrees(math.atan2(u, math.hypot(e, n_)))),
        "worst_axis_azim_deg": float(math.degrees(math.atan2(e, n_)) % 360.0),
        "eig_ratio_1_2": float(w[0] / w[1]) if w[1] > 0 else float("nan"),
        "pdop_like": float(math.sqrt(max(0.0, w.sum()))),
        "sky": sky,
    }


def scan_for_consumer_zwd(run: Path, consumer: str):
    """
    Grep every CSV header in the run root and logs/ for a wet-tropo column and
    report which ones (if any) carry a row for the consumer station.
    """
    found = []
    for d in {run, _logs(run)}:
        if not d.is_dir():
            continue
        for p in sorted(d.glob("*.csv")):
            try:
                with p.open("r", errors="replace") as f:
                    hdr = f.readline().strip()
            except OSError:
                continue
            cols = hdr.split(",")
            hits = [c for c in cols if ZWD_COL_RE.search(c)]
            if not hits:
                continue
            has_consumer = None
            if consumer:
                try:
                    # cheap scan: does the consumer name appear anywhere?
                    with p.open("r", errors="replace") as f:
                        has_consumer = False
                        for i, line in enumerate(f):
                            if consumer in line:
                                has_consumer = True
                                break
                            if i > 400000:
                                break
                except OSError:
                    has_consumer = None
            found.append({"file": str(p), "cols": hits, "has_consumer_rows": has_consumer})
    return found


# --------------------------------------------------------------------------
# event classification
# --------------------------------------------------------------------------


def classify_events(ev: pd.DataFrame) -> pd.DataFrame:
    """Return a per-row boolean matrix of the event classes we track."""
    tier = ev["tier"].astype(str)
    event = ev["event"].astype(str) if "event" in ev.columns else pd.Series("", index=ev.index)
    act = ev["action"].astype(str) if "action" in ev.columns else pd.Series("", index=ev.index)
    det = tier.isin(["T0_mw_detect", "T1_if_slip", "T2_within_epoch"])
    cls = pd.DataFrame(index=ev.index)
    cls["PBGAUGE"] = tier.eq("PBGAUGE")
    cls["PBGAUGE_cold"] = tier.eq("PBGAUGE") & event.str.contains("cold", case=False, na=False)
    cls["CLK_STEP"] = tier.eq("CLK_STEP")
    cls["CLK_ANCHOR"] = tier.eq("CLK_ANCHOR")
    cls["PIVOT_PHASE_ANCHOR"] = tier.isin(["PIVOT", "PHASE_ANCHOR"])
    cls["READMIT"] = tier.eq("READMIT") | act.eq("readmit")
    cls["T4_attrib"] = tier.eq("T4_attrib")
    cls["T3_raim"] = tier.eq("T3_raim") | tier.eq("RAIM")
    cls["detector_refloat"] = det & act.eq("refloat")
    cls["T0_gap_resume"] = tier.eq("T0_gap")
    cls["TOPOLOGY"] = tier.eq("TOPOLOGY")
    cls["GATE"] = tier.eq("GATE")
    cls["IONO_DW"] = tier.eq("IONO_DW")
    cls["ADMISSION_withhold"] = act.eq("withhold")
    cls["CONGRUENCE"] = tier.eq("CONGRUENCE")
    cls["BLOCKBAARDA"] = tier.eq("BLOCKBAARDA")
    cls["PRIORRES"] = tier.eq("PRIORRES")
    return cls


EVENT_CLASSES = [
    "PBGAUGE",
    "PBGAUGE_cold",
    "CLK_STEP",
    "CLK_ANCHOR",
    "PIVOT_PHASE_ANCHOR",
    "READMIT",
    "T4_attrib",
    "T3_raim",
    "detector_refloat",
    "T0_gap_resume",
    "TOPOLOGY",
    "GATE",
    "IONO_DW",
    "ADMISSION_withhold",
    "CONGRUENCE",
    "BLOCKBAARDA",
    "PRIORRES",
]

STATUS_COUNTS = [
    "n_arc_refloat",
    "n_slip_refloats",
    "n_refloat_sats",
    "n_d5_flags",
    "n_chord_births",
    "n_dw_engaged",
]


# --------------------------------------------------------------------------
# the census
# --------------------------------------------------------------------------


def census(run: Path, settle_h: float, block_min: float) -> dict:
    missing: list = []
    out: dict = {"run": run.name, "run_dir": str(run), "settle_h": settle_h, "block_min": block_min}

    cl = load_consumer(run, missing)
    if cl is None or cl.empty:
        out["missing_traces"] = missing
        out["error"] = "no closed_loop_trace.csv — nothing to grade"
        return out

    t0 = float(cl["tow"].min())
    t_end = float(cl["tow"].max())
    t_settle = t0 + settle_h * 3600.0
    s = cl[cl["tow"] >= t_settle].copy()
    out["n_epochs_total"] = int(len(cl))
    out["n_epochs_settled"] = int(len(s))
    out["tow_first"] = t0
    out["tow_last"] = t_end
    out["tow_settle"] = t_settle
    out["duration_h"] = (t_end - t0) / 3600.0
    dt = float(np.median(np.diff(cl["tow"].to_numpy()))) if len(cl) > 2 else 30.0
    out["epoch_dt_s"] = dt
    out["n_fixed_settled"] = int(s["num_fixed"].sum()) if "num_fixed" in s else None
    if "ar_gate" in s:
        # collapse "sigma_gate(0.134m)" -> "sigma_gate"; the numeric payload is
        # per-epoch and would otherwise print a few hundred singleton keys.
        gate_kind = s["ar_gate"].astype(str).str.replace(r"\(.*\)", "", regex=True)
        out["ar_gate_settled"] = {k: int(v) for k, v in gate_kind.value_counts().items()}

    # --- roster / consumer geometry -------------------------------------
    consumer = None
    cons_lat = cons_lon = None
    sta = load_stations(run, missing)
    sta_ll = {}
    if sta is not None and not sta.empty:
        held = sta[sta["role"].astype(str).str.contains("held", case=False, na=False)]
        if not held.empty:
            consumer = str(held.iloc[0]["station"])
        for _, r in sta.iterrows():
            la, lo, _h = ecef_to_geodetic(float(r["ecef_x"]), float(r["ecef_y"]), float(r["ecef_z"]))
            sta_ll[str(r["station"])] = (la, lo)
        if consumer in sta_ll:
            cons_lat, cons_lon = sta_ll[consumer]
    out["consumer"] = consumer
    out["consumer_lat_deg"] = cons_lat
    out["consumer_lon_deg"] = cons_lon
    out["n_stations_roster"] = int(len(sta)) if sta is not None else None
    # the co-located twin that stands in for the held-out consumer's sky
    twin = None
    if cons_lat is not None:
        cands = [
            (great_circle_km(cons_lat, cons_lon, la, lo), nm)
            for nm, (la, lo) in sta_ll.items()
            if nm != consumer
        ]
        if cands:
            twin = min(cands)[1]
    out["geometry_twin_station"] = twin

    # ================================================================
    # 1. character of the residual
    # ================================================================
    char = {}
    max_lag = int(min(240, max(8, len(s) // 3)))
    for comp, col in (("up", "up_m"), ("east", "east_m"), ("north", "north_m"), ("d3", "pos_err_3d_m")):
        v = s[col].to_numpy(dtype=float)
        a = acf(v - np.nanmean(v), max_lag)
        k05 = first_lag_below(a, 0.5)
        kie = first_lag_below(a, 1.0 / math.e)
        k00 = first_lag_below(a, 0.0)
        sd_b, sd_w, frac, nb = slow_fast_split(s["tow"].to_numpy(dtype=float), v, block_min * 60.0)
        # slope over the settled window (m/h)
        tt = (s["tow"].to_numpy(dtype=float) - t_settle) / 3600.0
        slope = float(np.polyfit(tt, v, 1)[0]) if len(tt) > 3 else float("nan")
        char[comp] = {
            "mean_m": float(np.nanmean(v)),
            "sd_m": float(np.nanstd(v, ddof=1)),
            "rms_m": rms(v),
            "p95_abs_m": float(np.nanpercentile(np.abs(v), 95)),
            "acf_lag_half_min": (k05 * dt / 60.0) if np.isfinite(k05) else None,
            "acf_lag_1e_min": (kie * dt / 60.0) if np.isfinite(kie) else None,
            "acf_zero_cross_min": (k00 * dt / 60.0) if np.isfinite(k00) else None,
            "acf_at": {
                f"{int(round(k * dt / 60.0))}min": (float(a[k]) if k < a.size else None)
                for k in (2, 10, 20, 40, 60, 120)
            },
            "blockmean_sd_m": sd_b,
            "within_block_sd_m": sd_w,
            "frac_var_slow": frac,
            "n_blocks": nb,
            "trend_m_per_h": slope,
        }
    out["character"] = char

    # --- error-vector PCA: is the residual a single direction in ENU? -----
    M = np.column_stack(
        [s["east_m"].to_numpy(float), s["north_m"].to_numpy(float), s["up_m"].to_numpy(float)]
    )
    M = M[np.isfinite(M).all(axis=1)]
    pca = {}
    if M.shape[0] > 10:
        for tag, X in (("about_mean", M - M.mean(axis=0)), ("about_truth", M)):
            C = np.cov(X, rowvar=False)
            w, V = np.linalg.eigh(C)
            idx = np.argsort(w)[::-1]
            w, V = w[idx], V[:, idx]
            v0 = V[:, 0]
            if v0[2] < 0:  # sign convention: point the axis up
                v0 = -v0
            e, n_, u = v0
            pca[tag] = {
                "eigvals_m2": [float(x) for x in w],
                "frac_var_axis1": float(w[0] / w.sum()) if w.sum() > 0 else float("nan"),
                "axis1_enu": [float(e), float(n_), float(u)],
                "axis1_elev_deg": float(math.degrees(math.atan2(u, math.hypot(e, n_)))),
                "axis1_azim_deg": float(math.degrees(math.atan2(e, n_)) % 360.0),
                "axis1_sd_m": float(math.sqrt(max(0.0, w[0]))),
            }
        pca["corr_matrix_enu"] = [
            [pearson(M[:, i], M[:, j]) for j in range(3)] for i in range(3)
        ]
    out["error_pca"] = pca

    # --- does that axis sit on the geometric null? -----------------------
    gnull = geometry_worst_direction(run, twin, t_settle, missing) if twin else None
    if gnull and pca.get("about_mean"):
        a = np.array(pca["about_mean"]["axis1_enu"], dtype=float)
        b = np.array(gnull["worst_axis_enu"], dtype=float)
        cosang = float(abs(np.dot(a, b)) / (np.linalg.norm(a) * np.linalg.norm(b)))
        gnull["angle_to_error_axis_deg"] = float(math.degrees(math.acos(min(1.0, cosang))))
        # how much of the error variance projects onto the geometric null
        Mc = M - M.mean(axis=0)
        proj = Mc @ b
        gnull["frac_var_on_null"] = float(proj.var() / Mc.var(axis=0).sum())
        gnull["sd_on_null_m"] = float(proj.std())
    out["geometry_null"] = gnull

    # ================================================================
    # 2. tropo coupling
    # ================================================================
    tropo = {"consumer_own_zwd": None, "zwd_trace_scan": scan_for_consumer_zwd(run, consumer or "")}
    zwd = load_zwd(run, missing)
    if zwd is not None and not zwd.empty:
        zs = zwd[zwd["tow"] >= t_settle]
        wide = zs.pivot_table(index="tow", columns="station", values="zwd_m", aggfunc="last")
        med = wide.median(axis=0)
        dev = wide.subtract(med, axis=1)
        # align the consumer error onto the ZWD epochs
        up = s.set_index("tow")["up_m"].reindex(dev.index)
        fleet = dev.median(axis=1)
        tropo["n_stations_zwd"] = int(dev.shape[1])
        tropo["fleet_dev_sd_m"] = float(fleet.std())
        tropo["fleet_dev_span_m"] = float(fleet.max() - fleet.min())
        tropo["corr_up_fleet_dev"] = pearson(up.to_numpy(), fleet.to_numpy())
        tropo["corr_3d_fleet_dev"] = pearson(
            s.set_index("tow")["pos_err_3d_m"].reindex(dev.index).to_numpy(), fleet.to_numpy()
        )
        # per-station
        rows = []
        upv = up.to_numpy()
        for stn in dev.columns:
            dv = dev[stn].to_numpy()
            r = pearson(upv, dv)
            fin = dv[np.isfinite(dv)]
            span = float(fin.max() - fin.min()) if fin.size else float("nan")
            slope = float("nan")
            xx, yy = _finite_pair(dv, upv)
            if xx.size > 3 and xx.std() > 1e-9:
                slope = float(np.polyfit(xx, yy, 1)[0])
            rows.append(
                {
                    "station": str(stn),
                    "corr_up": r,
                    "zwd_span_m": span,
                    "zwd_sd_m": float(np.nanstd(dv)),
                    "zwd_median_m": float(med[stn]),
                    "slope_up_per_zwd": slope,
                    "dist_km": (
                        great_circle_km(cons_lat, cons_lon, *sta_ll[str(stn)])
                        if (cons_lat is not None and str(stn) in sta_ll)
                        else None
                    ),
                    "n_epochs": int(np.isfinite(dv).sum()),
                }
            )
        rows.sort(key=lambda r: (-abs(r["corr_up"]) if np.isfinite(r["corr_up"]) else 0.0))
        tropo["per_station_top"] = rows[:8]
        tropo["per_station_all"] = rows
        spans = np.array([r["zwd_span_m"] for r in rows if np.isfinite(r["zwd_span_m"])])
        if spans.size:
            tropo["zwd_span_p50_m"] = float(np.median(spans))
            tropo["zwd_span_p95_m"] = float(np.percentile(spans, 95))
            tropo["zwd_span_max_m"] = float(spans.max())
            tropo["n_stations_span_gt_10m"] = int((spans > 10.0).sum())
        corrs = np.array([r["corr_up"] for r in rows if np.isfinite(r["corr_up"])])
        if corrs.size:
            tropo["per_station_corr_absmax"] = float(np.abs(corrs).max())
            tropo["per_station_corr_absmed"] = float(np.median(np.abs(corrs)))
        # nearest neighbours to the consumer
        withd = [r for r in rows if r["dist_km"] is not None]
        withd.sort(key=lambda r: r["dist_km"])
        tropo["nearest_stations"] = withd[:5]
        # CO-LOCATED twins (< 5 km) are the best available stand-in for the
        # consumer's own wet troposphere when its state is not traced.
        colo = [r["station"] for r in withd if r["dist_km"] < 5.0]
        tropo["colocated_stations"] = colo
        if colo:
            site = dev[colo].mean(axis=1)
            sv = site.to_numpy()
            tropo["corr_up_site_zwd"] = pearson(upv, sv)
            xx, yy = _finite_pair(sv, upv)
            if xx.size > 3 and xx.std() > 1e-9:
                p = np.polyfit(xx, yy, 1)
                tropo["slope_up_per_site_zwd"] = float(p[0])
                resid = yy - np.polyval(p, xx)
                tropo["up_rms_after_site_zwd_regress_m"] = rms(resid)
            tropo["site_zwd_sd_m"] = float(np.nanstd(sv))
            tropo["site_zwd_span_m"] = float(np.nanmax(sv) - np.nanmin(sv))
        if "carried" in zwd.columns:
            tropo["carried_frac_settled"] = float(zs["carried"].astype(float).mean())
    out["tropo"] = tropo

    # ================================================================
    # 3. per-block event coincidence
    # ================================================================
    blk_s = block_min * 60.0
    cl["blk"] = np.floor((cl["tow"] - t0) / blk_s).astype(int)
    st = load_status(run, missing)
    ev = load_events(run, missing)
    amb, amb_path = load_amb_resets(run, missing)
    out["amb_trace_path"] = amb_path

    blocks = []
    ev_cls = None
    if ev is not None and not ev.empty:
        ev = ev.dropna(subset=["tow"])
        ev_cls = classify_events(ev)
        ev_cls["blk"] = np.floor((ev["tow"].to_numpy(dtype=float) - t0) / blk_s).astype(int)
        ev_blk = ev_cls.groupby("blk")[EVENT_CLASSES].sum()
        ev["blk"] = ev_cls["blk"]
    else:
        ev_blk = pd.DataFrame()

    st_blk = pd.DataFrame()
    if st is not None and not st.empty:
        st = st.dropna(subset=["tow"]).copy()
        st["blk"] = np.floor((st["tow"].to_numpy(dtype=float) - t0) / blk_s).astype(int)
        agg = {c: "sum" for c in STATUS_COUNTS if c in st.columns}
        if "sigma0" in st.columns:
            agg["sigma0"] = "mean"
        if "sigma0_global" in st.columns:
            agg["sigma0_global"] = "mean"
        if "n_present" in st.columns:
            agg["n_present"] = "mean"
        st_blk = st.groupby("blk").agg(agg)

    amb_blk = pd.Series(dtype=float)
    if amb is not None and not amb.empty:
        amb = amb.copy()
        amb["blk"] = np.floor((amb["tow"].to_numpy(dtype=float) - t0) / blk_s).astype(int)
        amb_blk = amb.groupby("blk")["n_amb_reset"].sum()

    zwd_blk = pd.Series(dtype=float)
    if zwd is not None and not zwd.empty:
        zz = zwd[zwd["tow"] >= t_settle].copy()
        zz["blk"] = np.floor((zz["tow"].to_numpy(dtype=float) - t0) / blk_s).astype(int)
        # same reference as section 2: each station's own SETTLED median
        smed = zz.groupby("station")["zwd_m"].median()
        zz["dev"] = zz["zwd_m"] - zz["station"].map(smed)
        zwd_blk = zz.groupby("blk")["dev"].median()

    for b, g in cl.groupby("blk"):
        if g["tow"].max() < t_settle:
            continue
        rec = {
            "blk": int(b),
            "t_start_h": float((g["tow"].min() - t0) / 3600.0),
            "n_ep": int(len(g)),
            "up_rms_m": rms(g["up_m"]),
            "up_mean_m": float(g["up_m"].mean()),
            "up_sd_m": float(g["up_m"].std()),
            "d3_rms_m": rms(g["pos_err_3d_m"]),
            "east_rms_m": rms(g["east_m"]),
            "north_rms_m": rms(g["north_m"]),
            "n_sats_mean": float(g["n_sats"].mean()) if "n_sats" in g else float("nan"),
        }
        for c in EVENT_CLASSES:
            rec[c] = int(ev_blk[c].get(b, 0)) if not ev_blk.empty and c in ev_blk else 0
        for c in STATUS_COUNTS:
            rec[c] = float(st_blk[c].get(b, 0.0)) if not st_blk.empty and c in st_blk else float("nan")
        for c in ("sigma0", "sigma0_global", "n_present"):
            rec[c] = float(st_blk[c].get(b, float("nan"))) if not st_blk.empty and c in st_blk else float("nan")
        rec["n_amb_reset"] = float(amb_blk.get(b, 0.0)) if not amb_blk.empty else float("nan")
        rec["zwd_fleet_dev_m"] = float(zwd_blk.get(b, float("nan"))) if not zwd_blk.empty else float("nan")
        blocks.append(rec)
    out["blocks"] = blocks

    # spearman of each count against the block error
    bdf = pd.DataFrame(blocks)
    corr_rows = []
    if not bdf.empty:
        drivers = (
            EVENT_CLASSES
            + [c for c in STATUS_COUNTS if c in bdf.columns]
            + ["sigma0", "sigma0_global", "n_sats_mean", "n_present", "n_amb_reset", "zwd_fleet_dev_m"]
        )
        for c in drivers:
            if c not in bdf.columns:
                continue
            v = bdf[c].to_numpy(dtype=float)
            if not np.isfinite(v).any() or (np.nanstd(v) == 0):
                corr_rows.append(
                    {"driver": c, "rho_up_rms": None, "p_up": None, "rho_d3_rms": None,
                     "p_d3": None, "rho_absupmean": None, "n": int(len(bdf)), "note": "constant/absent"}
                )
                continue
            r1, p1, n1 = spearman(v, bdf["up_rms_m"].to_numpy())
            r2, p2, _ = spearman(v, bdf["d3_rms_m"].to_numpy())
            r3, _p3, _ = spearman(v, np.abs(bdf["up_mean_m"].to_numpy()))
            corr_rows.append(
                {
                    "driver": c,
                    "rho_up_rms": r1,
                    "p_up": p1,
                    "rho_d3_rms": r2,
                    "p_d3": p2,
                    "rho_absupmean": r3,
                    "n": n1,
                    "mean": float(np.nanmean(v)),
                    "note": "",
                }
            )
        corr_rows.sort(key=lambda r: (-abs(r["rho_up_rms"]) if r["rho_up_rms"] is not None and np.isfinite(r["rho_up_rms"]) else -1))
    out["block_driver_corr"] = corr_rows

    # worst blocks by |UP mean|, itemised by tier
    worst = []
    if not bdf.empty and ev is not None and not ev.empty:
        wb = bdf.reindex(bdf["up_mean_m"].abs().sort_values(ascending=False).index).head(4)
        for _, r in wb.iterrows():
            b = int(r["blk"])
            sub = ev[ev["blk"] == b]
            tiers = {str(k): int(v) for k, v in sub["tier"].value_counts().items()}
            worst.append(
                {
                    "blk": b,
                    "t_start_h": float(r["t_start_h"]),
                    "up_mean_m": float(r["up_mean_m"]),
                    "up_rms_m": float(r["up_rms_m"]),
                    "d3_rms_m": float(r["d3_rms_m"]),
                    "zwd_fleet_dev_m": float(r["zwd_fleet_dev_m"]),
                    "sigma0": float(r["sigma0"]),
                    "n_sats_mean": float(r["n_sats_mean"]),
                    "tiers": tiers,
                }
            )
    out["worst_blocks"] = worst

    # ----------------------------------------------------------------
    # 3b. PER-EPOCH driver correlations (n = every settled epoch, not 12
    #     blocks).  Serial correlation is corrected for explicitly — these
    #     are all slow states and the naive n would be a fiction.
    # ----------------------------------------------------------------
    per_ep = []
    if st is not None and not st.empty:
        sst = st[st["tow"] >= t_settle].set_index("tow")
        tgt = s.set_index("tow")
        common = tgt.index.intersection(sst.index)
        if len(common) > 20:
            up = tgt.loc[common, "up_m"].to_numpy(float)
            d3 = tgt.loc[common, "pos_err_3d_m"].to_numpy(float)
            hz = np.hypot(
                tgt.loc[common, "east_m"].to_numpy(float),
                tgt.loc[common, "north_m"].to_numpy(float),
            )
            drivers = [
                "sigma0", "sigma0_global", "sigma0_pf", "sigma0_pf_code", "sigma0_pf_phase",
                "clock_median_m", "clock_std_m", "worst_dev_m", "n_present", "n_clock_sats",
                "n_dw_engaged", "n_dw_zero", "dw_mean_w", "dw_min_w", "n_arc_refloat",
                "n_slip_refloats", "n_refloat_sats", "n_chord_births", "n_d5_flags",
                "prior_tension_ratio", "prires_max_t", "prires_rows", "adm_net_worst_z",
                "n_pf_rows", "n_redun_rows", "n_uncheckable_rows",
            ]
            for drv in drivers:
                if drv not in sst.columns:
                    continue
                v = pd.to_numeric(sst.loc[common, drv], errors="coerce").to_numpy(float)
                if not np.isfinite(v).any() or np.nanstd(v) == 0:
                    continue
                r_up, n_, ne, p_up = corr_autocorr_aware(v, up)
                r_d3, _, _, p_d3 = corr_autocorr_aware(v, d3)
                r_hz, _, _, p_hz = corr_autocorr_aware(v, hz)
                per_ep.append(
                    {
                        "driver": drv,
                        "mean": float(np.nanmean(v)),
                        "sd": float(np.nanstd(v)),
                        "lag1": lag1(v),
                        "r_up": r_up, "p_up_eff": p_up,
                        "r_3d": r_d3, "p_3d_eff": p_d3,
                        "r_horiz": r_hz, "p_hz_eff": p_hz,
                        "n": n_, "n_eff": ne,
                    }
                )
            # the fleet ZWD deviation, on the same footing
            if zwd is not None and not zwd.empty:
                zsub = zwd[zwd["tow"] >= t_settle]
                w2 = zsub.pivot_table(index="tow", columns="station", values="zwd_m", aggfunc="last")
                d2 = w2.subtract(w2.median(axis=0), axis=1).median(axis=1).reindex(common)
                r_up, n_, ne, p_up = corr_autocorr_aware(d2.to_numpy(), up)
                r_d3, _, _, p_d3 = corr_autocorr_aware(d2.to_numpy(), d3)
                r_hz, _, _, p_hz = corr_autocorr_aware(d2.to_numpy(), hz)
                per_ep.append(
                    {"driver": "zwd_fleet_dev_m", "mean": float(np.nanmean(d2)),
                     "sd": float(np.nanstd(d2)), "lag1": lag1(d2.to_numpy()),
                     "r_up": r_up, "p_up_eff": p_up, "r_3d": r_d3, "p_3d_eff": p_d3,
                     "r_horiz": r_hz, "p_hz_eff": p_hz, "n": n_, "n_eff": ne}
                )
            per_ep.sort(key=lambda r: -abs(r["r_up"]) if np.isfinite(r["r_up"]) else 0.0)
    out["per_epoch_driver_corr"] = per_ep

    # ================================================================
    # 4. east/north hourly
    # ================================================================
    cl["hr"] = np.floor((cl["tow"] - t0) / 3600.0).astype(int)
    hrs = []
    st_hr = pd.DataFrame()
    if st is not None and not st.empty:
        st["hr"] = np.floor((st["tow"].to_numpy(dtype=float) - t0) / 3600.0).astype(int)
        agg = {c: "sum" for c in STATUS_COUNTS if c in st.columns}
        if "sigma0" in st.columns:
            agg["sigma0"] = "mean"
        st_hr = st.groupby("hr").agg(agg)
    amb_hr = pd.Series(dtype=float)
    if amb is not None and not amb.empty:
        amb["hr"] = np.floor((amb["tow"].to_numpy(dtype=float) - t0) / 3600.0).astype(int)
        amb_hr = amb.groupby("hr")["n_amb_reset"].sum()

    # UTC hour of day from tow (GPS tow, week seconds); local solar time at the consumer
    for h, g in cl.groupby("hr"):
        tmid = float(g["tow"].mean())
        utc_h = (tmid % 86400.0) / 3600.0
        lst = (utc_h + (cons_lon / 15.0 if cons_lon is not None else 0.0)) % 24.0
        rec = {
            "hr": int(h),
            "settled": bool(g["tow"].max() >= t_settle),
            "east_rms_m": rms(g["east_m"]),
            "north_rms_m": rms(g["north_m"]),
            "east_mean_m": float(g["east_m"].mean()),
            "north_mean_m": float(g["north_m"].mean()),
            "up_rms_m": rms(g["up_m"]),
            "n_sats_mean": float(g["n_sats"].mean()) if "n_sats" in g else float("nan"),
            "utc_h": utc_h,
            "local_solar_h": lst,
            "solar_proxy": float(math.cos(math.radians(15.0 * (lst - 14.0)))),  # peaks ~14 LT
            "n_amb_reset": float(amb_hr.get(h, 0.0)) if not amb_hr.empty else float("nan"),
        }
        for c in STATUS_COUNTS:
            rec[c] = float(st_hr[c].get(h, float("nan"))) if not st_hr.empty and c in st_hr else float("nan")
        rec["sigma0"] = float(st_hr["sigma0"].get(h, float("nan"))) if not st_hr.empty and "sigma0" in st_hr else float("nan")
        hrs.append(rec)
    out["hours"] = hrs

    hdf = pd.DataFrame([h for h in hrs if h["settled"]])
    en = {}
    if not hdf.empty and len(hdf) >= 4:
        for comp in ("east_rms_m", "north_rms_m"):
            en[comp] = {}
            for drv in ["n_sats_mean", "local_solar_h", "solar_proxy", "hr", "sigma0",
                        "n_amb_reset", "n_arc_refloat", "n_slip_refloats"]:
                if drv not in hdf.columns:
                    continue
                v = hdf[drv].to_numpy(dtype=float)
                if not np.isfinite(v).any() or np.nanstd(v) == 0:
                    en[comp][drv] = {"r": None, "rho": None, "n": int(len(hdf)), "note": "constant/absent"}
                    continue
                r = pearson(v, hdf[comp].to_numpy())
                rho, p, n = spearman(v, hdf[comp].to_numpy())
                en[comp][drv] = {"r": r, "rho": rho, "p": p, "n": n}
    # per-epoch settled correlations, which have real sample size
    en["per_epoch_settled"] = {
        "corr_east_north": pearson(s["east_m"], s["north_m"]),
        "corr_east_up": pearson(s["east_m"], s["up_m"]),
        "corr_north_up": pearson(s["north_m"], s["up_m"]),
        "corr_nsats_up_abs": pearson(s["n_sats"], np.abs(s["up_m"])) if "n_sats" in s else None,
        "corr_nsats_east_abs": pearson(s["n_sats"], np.abs(s["east_m"])) if "n_sats" in s else None,
        "corr_nsats_north_abs": pearson(s["n_sats"], np.abs(s["north_m"])) if "n_sats" in s else None,
        "n_sats_mean": float(s["n_sats"].mean()) if "n_sats" in s else None,
        "n_sats_min": int(s["n_sats"].min()) if "n_sats" in s else None,
    }
    out["east_north"] = en

    # ================================================================
    # headline scalars, for the compare table
    # ================================================================
    out["headline"] = {
        "d3_rms_m": char["d3"]["rms_m"],
        "up_mean_m": char["up"]["mean_m"],
        "up_sd_m": char["up"]["sd_m"],
        "up_rms_m": char["up"]["rms_m"],
        "east_mean_m": char["east"]["mean_m"],
        "east_sd_m": char["east"]["sd_m"],
        "north_mean_m": char["north"]["mean_m"],
        "north_sd_m": char["north"]["sd_m"],
        "up_acf_half_min": char["up"]["acf_lag_half_min"],
        "east_acf_half_min": char["east"]["acf_lag_half_min"],
        "north_acf_half_min": char["north"]["acf_lag_half_min"],
        "up_frac_var_slow": char["up"]["frac_var_slow"],
        "east_frac_var_slow": char["east"]["frac_var_slow"],
        "north_frac_var_slow": char["north"]["frac_var_slow"],
        "up_blockmean_sd_m": char["up"]["blockmean_sd_m"],
        "up_within_block_sd_m": char["up"]["within_block_sd_m"],
        "up_trend_m_per_h": char["up"]["trend_m_per_h"],
        "corr_up_fleet_zwd": tropo.get("corr_up_fleet_dev"),
        "corr_up_site_zwd": tropo.get("corr_up_site_zwd"),
        "slope_up_per_site_zwd": tropo.get("slope_up_per_site_zwd"),
        "site_zwd_sd_m": tropo.get("site_zwd_sd_m"),
        "per_station_corr_absmax": tropo.get("per_station_corr_absmax"),
        "zwd_span_p50_m": tropo.get("zwd_span_p50_m"),
        "zwd_span_max_m": tropo.get("zwd_span_max_m"),
        "n_stations_span_gt_10m": tropo.get("n_stations_span_gt_10m"),
        "n_fixed_settled": out.get("n_fixed_settled"),
        "pca_frac_var_axis1": pca.get("about_mean", {}).get("frac_var_axis1"),
        "pca_axis1_elev_deg": pca.get("about_mean", {}).get("axis1_elev_deg"),
        "pca_axis1_azim_deg": pca.get("about_mean", {}).get("axis1_azim_deg"),
        "corr_north_up": (pca.get("corr_matrix_enu") or [[None] * 3] * 3)[1][2],
        "corr_east_up": (pca.get("corr_matrix_enu") or [[None] * 3] * 3)[0][2],
        "top_per_epoch_driver": (per_ep[0]["driver"] if per_ep else None),
        "top_per_epoch_r_up": (per_ep[0]["r_up"] if per_ep else None),
        "geom_null_elev_deg": (gnull or {}).get("worst_axis_elev_deg"),
        "geom_null_azim_deg": (gnull or {}).get("worst_axis_azim_deg"),
        "angle_err_to_geom_null_deg": (gnull or {}).get("angle_to_error_axis_deg"),
        "frac_var_on_geom_null": (gnull or {}).get("frac_var_on_null"),
    }
    out["missing_traces"] = missing
    return out


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------


def _f(v, n=3, w=0):
    if v is None:
        return "n/a".rjust(w)
    try:
        if isinstance(v, float) and not np.isfinite(v):
            return "n/a".rjust(w)
        return f"{v:.{n}f}".rjust(w)
    except (TypeError, ValueError):
        return str(v).rjust(w)


def render(c: dict) -> str:
    L = []
    A = L.append
    A("=" * 100)
    A(f"CONSUMER RESIDUAL CENSUS  —  {c['run']}")
    A("=" * 100)
    if "error" in c:
        A(f"ERROR: {c['error']}")
        return "\n".join(L)
    A(f"run dir           : {c['run_dir']}")
    A(f"consumer          : {c['consumer']}  lat {_f(c['consumer_lat_deg'],3)} lon {_f(c['consumer_lon_deg'],3)}"
      f"   (roster {c['n_stations_roster']} incl. held-out)")
    A(f"epochs            : {c['n_epochs_total']} total, {c['n_epochs_settled']} settled "
      f"(>= {c['settle_h']:g} h), dt {c['epoch_dt_s']:.0f} s, span {c['duration_h']:.2f} h")
    A(f"AR                : {c.get('n_fixed_settled')} fixes in the settled window; "
      f"gates {c.get('ar_gate_settled')}")
    A("")

    # --- 1
    A("-" * 100)
    A("1. CHARACTER OF THE SETTLED RESIDUAL  (slow wander vs fast noise)")
    A("-" * 100)
    A(f"{'comp':<6}{'mean':>9}{'sd':>9}{'rms':>9}{'p95|.|':>9}{'trend/h':>9}"
      f"{'ACF<0.5':>9}{'ACF<1/e':>9}{'ACF=0':>9}{'sd(blk)':>9}{'sd(in)':>9}{'%slow':>8}")
    A(f"{'':<6}{'(m)':>9}{'(m)':>9}{'(m)':>9}{'(m)':>9}{'(m)':>9}"
      f"{'(min)':>9}{'(min)':>9}{'(min)':>9}{'(m)':>9}{'(m)':>9}{'':>8}")
    for comp in ("up", "east", "north", "d3"):
        d = c["character"][comp]
        A(f"{comp:<6}{_f(d['mean_m'],3,9)}{_f(d['sd_m'],3,9)}{_f(d['rms_m'],3,9)}"
          f"{_f(d['p95_abs_m'],3,9)}{_f(d['trend_m_per_h'],3,9)}"
          f"{_f(d['acf_lag_half_min'],1,9)}{_f(d['acf_lag_1e_min'],1,9)}{_f(d['acf_zero_cross_min'],1,9)}"
          f"{_f(d['blockmean_sd_m'],3,9)}{_f(d['within_block_sd_m'],3,9)}"
          f"{_f(100*d['frac_var_slow'] if d['frac_var_slow'] is not None and np.isfinite(d['frac_var_slow']) else None,1,8)}")
    A("")
    A("ACF by lag (settled, about the mean):")
    keys = list(c["character"]["up"]["acf_at"].keys())
    A(f"{'comp':<6}" + "".join(f"{k:>9}" for k in keys))
    for comp in ("up", "east", "north"):
        d = c["character"][comp]["acf_at"]
        A(f"{comp:<6}" + "".join(_f(d[k], 3, 9) for k in keys))
    A("")
    A(f"  sd(blk) = std of the {c['block_min']:g}-min block MEANS (slow wander); "
      f"sd(in) = pooled within-block scatter (fast noise); %slow = slow share of variance.")
    A("")
    pca = c.get("error_pca", {})
    if pca:
        A("Error-vector PCA (is the residual ONE direction in ENU?):")
        A(f"    {'basis':<13}{'frac var ax1':>14}{'sd ax1 (m)':>12}{'E':>8}{'N':>8}{'U':>8}"
          f"{'elev':>8}{'azim':>8}")
        for tag in ("about_mean", "about_truth"):
            if tag not in pca:
                continue
            p = pca[tag]
            e, n_, u = p["axis1_enu"]
            A(f"    {tag:<13}{_f(p['frac_var_axis1'],3,14)}{_f(p['axis1_sd_m'],3,12)}"
              f"{_f(e,3,8)}{_f(n_,3,8)}{_f(u,3,8)}{_f(p['axis1_elev_deg'],1,8)}"
              f"{_f(p['axis1_azim_deg'],1,8)}")
        cm = pca.get("corr_matrix_enu")
        if cm:
            A(f"    corr(E,N) {_f(cm[0][1],3)}   corr(E,U) {_f(cm[0][2],3)}   corr(N,U) {_f(cm[1][2],3)}")
        gn = c.get("geometry_null")
        if gn:
            e, n_, u = gn["worst_axis_enu"]
            A(f"    GEOMETRIC NULL from the real sky at {gn['station']} "
              f"({gn['n_epochs']} settled epochs, elev cutoff {gn['elev_cutoff_deg']:.0f} deg,")
            A(f"      a receiver-clock column included — that is what tilts the worst direction off vertical):")
            A(f"      worst-determined axis  E {e:+.3f}  N {n_:+.3f}  U {u:+.3f}   "
              f"elev {gn['worst_axis_elev_deg']:.1f} deg, azim {gn['worst_axis_azim_deg']:.1f} deg")
            A(f"      ANGLE between the error axis and the geometric null = "
              f"{_f(gn.get('angle_to_error_axis_deg'),1)} deg   "
              f"(error variance projecting onto the null: "
              f"{_f(100*gn['frac_var_on_null'] if gn.get('frac_var_on_null') is not None else None,1)} %, "
              f"sd {_f(gn.get('sd_on_null_m'),3)} m)")
            sky = gn.get("sky")
            if sky:
                cnt = np.array(sky["counts"], dtype=float)
                pct = 100.0 * cnt / max(1.0, cnt.sum())
                # bin i spans azimuth [45i, 45i+45); label it by its MIDPOINT
                labels = ["NNE", "ENE", "ESE", "SSE", "SSW", "WSW", "WNW", "NNW"]
                bands = [f"{a}-{b}" for a, b in zip(sky["el_edges"][:-1], sky["el_edges"][1:])]
                A(f"      sky occupancy at {gn['station']} (% of LOS-epochs; the NORTH column at high "
                  f"elevation is the constellation void):")
                A("        " + f"{'elev':<8}" + "".join(f"{lab:>7}" for lab in labels))
                for j, band in enumerate(bands):
                    A("        " + f"{band:<8}" + "".join(f"{pct[i, j]:>7.2f}" for i in range(8)))
        A("")

    # --- 2
    tr = c.get("tropo", {})
    A("-" * 100)
    A("2. TROPO COUPLING  (producer ZWD deviation vs consumer UP)")
    A("-" * 100)
    A(f"stations with ZWD : {tr.get('n_stations_zwd')}   "
      f"span p50 {_f(tr.get('zwd_span_p50_m'),3)} m, p95 {_f(tr.get('zwd_span_p95_m'),3)} m, "
      f"max {_f(tr.get('zwd_span_max_m'),3)} m, n(span>10 m) {tr.get('n_stations_span_gt_10m')}")
    A(f"fleet-median dev  : sd {_f(tr.get('fleet_dev_sd_m'),4)} m, span {_f(tr.get('fleet_dev_span_m'),4)} m")
    A(f"corr(UP, fleet dev)   = {_f(tr.get('corr_up_fleet_dev'),4)}      "
      f"corr(3D, fleet dev) = {_f(tr.get('corr_3d_fleet_dev'),4)}")
    A(f"per-station |corr|: max {_f(tr.get('per_station_corr_absmax'),4)}, "
      f"median {_f(tr.get('per_station_corr_absmed'),4)}")
    if tr.get("carried_frac_settled") is not None:
        A(f"ZWD 'carried' flag: {_f(tr['carried_frac_settled'],3)} of settled rows")
    A("")
    A("Top 8 stations by |corr(UP, that station's ZWD deviation)|:")
    A(f"{'station':<12}{'corr':>9}{'slope':>9}{'span':>9}{'sd':>9}{'median':>9}{'dist_km':>10}{'n_ep':>7}")
    for r in tr.get("per_station_top", []):
        A(f"{r['station']:<12}{_f(r['corr_up'],4,9)}{_f(r['slope_up_per_zwd'],2,9)}"
          f"{_f(r['zwd_span_m'],3,9)}{_f(r['zwd_sd_m'],3,9)}{_f(r['zwd_median_m'],3,9)}"
          f"{_f(r['dist_km'],0,10)}{r['n_epochs']:>7}")
    A("")
    A("Nearest 5 stations to the consumer:")
    A(f"{'station':<12}{'dist_km':>10}{'corr':>9}{'slope':>9}{'span':>9}")
    for r in tr.get("nearest_stations", []):
        A(f"{r['station']:<12}{_f(r['dist_km'],0,10)}{_f(r['corr_up'],4,9)}"
          f"{_f(r['slope_up_per_zwd'],2,9)}{_f(r['zwd_span_m'],3,9)}")
    A("")
    if tr.get("colocated_stations"):
        A(f"CO-LOCATED site proxy ({', '.join(tr['colocated_stations'])}, < 5 km — the best available "
          f"stand-in for the consumer's own wet tropo):")
        A(f"  corr(UP, site ZWD dev) = {_f(tr.get('corr_up_site_zwd'),4)}    "
          f"slope dUP/dZWD = {_f(tr.get('slope_up_per_site_zwd'),2)}  "
          f"(a wet-mapping-collinear vertical error sits near -3 .. -4)")
        A(f"  site ZWD dev sd {_f(tr.get('site_zwd_sd_m'),4)} m, span {_f(tr.get('site_zwd_span_m'),4)} m;"
          f"  UP rms after regressing site ZWD out = {_f(tr.get('up_rms_after_site_zwd_regress_m'),3)} m")
        A("")
    A("Consumer's OWN ZWD:")
    if tr.get("consumer_own_zwd"):
        A(f"  {tr['consumer_own_zwd']}")
    else:
        A("  NOT TRACED — no CSV in this run carries a wet-tropo column for the held-out station.")
    scan = tr.get("zwd_trace_scan", [])
    A(f"  wet-tropo columns found in {len(scan)} file(s):")
    for f in scan:
        A(f"    {Path(f['file']).name:<28} cols={f['cols']} consumer_rows={f['has_consumer_rows']}")
    A("")

    # --- 3
    A("-" * 100)
    A(f"3. PER-{c['block_min']:g}-MIN BLOCK  (settled blocks only)")
    A("-" * 100)
    cols = ["PBGAUGE", "PIVOT_PHASE_ANCHOR", "T4_attrib", "detector_refloat", "T0_gap_resume",
            "TOPOLOGY", "GATE", "IONO_DW", "CLK_STEP", "READMIT"]
    A(f"{'blk':>4}{'t_h':>7}{'UPrms':>8}{'UPmean':>8}{'3Drms':>8}{'Erms':>7}{'Nrms':>7}"
      f"{'nsat':>6}{'sig0':>7}{'zwddev':>8}{'arcRF':>7}{'ambRS':>7}"
      + "".join(f"{k[:6]:>7}" for k in cols))
    for b in c["blocks"]:
        A(f"{b['blk']:>4}{b['t_start_h']:>7.1f}{_f(b['up_rms_m'],3,8)}{_f(b['up_mean_m'],3,8)}"
          f"{_f(b['d3_rms_m'],3,8)}{_f(b['east_rms_m'],2,7)}{_f(b['north_rms_m'],2,7)}"
          f"{_f(b['n_sats_mean'],1,6)}{_f(b.get('sigma0'),2,7)}{_f(b.get('zwd_fleet_dev_m'),3,8)}"
          f"{_f(b.get('n_arc_refloat'),0,7)}{_f(b.get('n_amb_reset'),0,7)}"
          + "".join(f"{int(b.get(k,0)):>7}" for k in cols))
    A("")
    A("Spearman rho of each block driver against the block's consumer error "
      f"(n = {len(c['blocks'])} settled blocks; |rho| > ~0.58 is p<0.05 at n=12):")
    A(f"{'driver':<22}{'mean':>9}{'rho(UPrms)':>12}{'p':>8}{'rho(3Drms)':>12}{'p':>8}{'rho(|UPmean|)':>15}")
    for r in c.get("block_driver_corr", []):
        if r.get("note"):
            A(f"{r['driver']:<22}{'—':>9}{'(constant / absent)':>12}")
            continue
        A(f"{r['driver']:<22}{_f(r.get('mean'),2,9)}{_f(r['rho_up_rms'],3,12)}{_f(r['p_up'],3,8)}"
          f"{_f(r['rho_d3_rms'],3,12)}{_f(r['p_d3'],3,8)}{_f(r['rho_absupmean'],3,15)}")
    A("")
    A("Worst 4 blocks by |UP block mean|, itemised:")
    for w in c.get("worst_blocks", []):
        A(f"  blk {w['blk']:>3} @ t+{w['t_start_h']:.1f} h : UPmean {w['up_mean_m']:+.3f}  "
          f"UPrms {w['up_rms_m']:.3f}  3Drms {w['d3_rms_m']:.3f}  "
          f"zwd_fleet_dev {_f(w['zwd_fleet_dev_m'],3)}  sig0 {_f(w['sigma0'],2)}  "
          f"nsat {_f(w['n_sats_mean'],1)}")
        A(f"        tiers: {w['tiers']}")
    A("")
    pe = c.get("per_epoch_driver_corr", [])
    if pe:
        A("3b. PER-EPOCH driver correlations over the settled window "
          f"(n = {pe[0]['n']} epochs, but every series is serially correlated —")
        A("    p is computed on the AUTOCORRELATION-CORRECTED effective n, which is what makes these honest):")
        A(f"{'driver':<22}{'mean':>11}{'sd':>10}{'lag1':>7}{'r(UP)':>9}{'p_eff':>8}"
          f"{'r(3D)':>9}{'p_eff':>8}{'r(horiz)':>10}{'p_eff':>8}{'n_eff':>8}")
        for r in pe[:20]:
            A(f"{r['driver']:<22}{_f(r['mean'],3,11)}{_f(r['sd'],3,10)}{_f(r['lag1'],2,7)}"
              f"{_f(r['r_up'],3,9)}{_f(r['p_up_eff'],3,8)}{_f(r['r_3d'],3,9)}{_f(r['p_3d_eff'],3,8)}"
              f"{_f(r['r_horiz'],3,10)}{_f(r['p_hz_eff'],3,8)}{_f(r['n_eff'],1,8)}")
        A("")

    # --- 4
    A("-" * 100)
    A("4. EAST / NORTH")
    A("-" * 100)
    A(f"{'hr':>4}{'set':>5}{'Erms':>8}{'Nrms':>8}{'Emean':>8}{'Nmean':>8}{'UPrms':>8}"
      f"{'nsat':>7}{'UTC_h':>8}{'LST_h':>8}{'solar':>8}{'arcRF':>7}{'ambRS':>7}")
    for h in c["hours"]:
        A(f"{h['hr']:>4}{('Y' if h['settled'] else '.'):>5}{_f(h['east_rms_m'],3,8)}{_f(h['north_rms_m'],3,8)}"
          f"{_f(h['east_mean_m'],3,8)}{_f(h['north_mean_m'],3,8)}{_f(h['up_rms_m'],3,8)}"
          f"{_f(h['n_sats_mean'],1,7)}{_f(h['utc_h'],2,8)}{_f(h['local_solar_h'],2,8)}"
          f"{_f(h['solar_proxy'],2,8)}{_f(h.get('n_arc_refloat'),0,7)}{_f(h.get('n_amb_reset'),0,7)}")
    A("")
    en = c.get("east_north", {})
    for comp in ("east_rms_m", "north_rms_m"):
        if comp not in en:
            continue
        A(f"hourly {comp} vs:")
        A(f"    {'driver':<20}{'pearson r':>12}{'spearman':>12}{'p':>8}{'n':>5}")
        for drv, v in en[comp].items():
            if v.get("note"):
                A(f"    {drv:<20}{'(constant/absent)':>12}")
                continue
            A(f"    {drv:<20}{_f(v['r'],3,12)}{_f(v['rho'],3,12)}{_f(v.get('p'),3,8)}{v['n']:>5}")
    pe = en.get("per_epoch_settled", {})
    A("")
    A("per-epoch settled cross-correlations (n = all settled epochs):")
    for k, v in pe.items():
        A(f"    {k:<24}{_f(v,4) if isinstance(v,(int,float)) else v}")
    A("")

    # --- missing
    A("-" * 100)
    A("MISSING / UNAVAILABLE TRACES")
    A("-" * 100)
    if c.get("missing_traces"):
        for m in c["missing_traces"]:
            A(f"  - {m}")
    else:
        A("  (none)")
    A("")
    return "\n".join(L)


COMPARE_KEYS = [
    ("d3_rms_m", "3D rms (m)"),
    ("up_mean_m", "UP mean (m)"),
    ("up_sd_m", "UP sd (m)"),
    ("up_rms_m", "UP rms (m)"),
    ("east_mean_m", "E mean (m)"),
    ("east_sd_m", "E sd (m)"),
    ("north_mean_m", "N mean (m)"),
    ("north_sd_m", "N sd (m)"),
    ("up_acf_half_min", "UP ACF<0.5 (min)"),
    ("east_acf_half_min", "E  ACF<0.5 (min)"),
    ("north_acf_half_min", "N  ACF<0.5 (min)"),
    ("up_frac_var_slow", "UP frac var slow"),
    ("east_frac_var_slow", "E  frac var slow"),
    ("north_frac_var_slow", "N  frac var slow"),
    ("up_blockmean_sd_m", "UP sd(blk mean) m"),
    ("up_within_block_sd_m", "UP sd(within blk) m"),
    ("up_trend_m_per_h", "UP trend (m/h)"),
    ("corr_up_fleet_zwd", "corr(UP, fleet ZWD)"),
    ("corr_up_site_zwd", "corr(UP, SITE ZWD)"),
    ("slope_up_per_site_zwd", "slope UP/siteZWD"),
    ("site_zwd_sd_m", "site ZWD dev sd (m)"),
    ("per_station_corr_absmax", "max |corr(UP,staZWD)|"),
    ("zwd_span_p50_m", "ZWD span p50 (m)"),
    ("zwd_span_max_m", "ZWD span max (m)"),
    ("n_stations_span_gt_10m", "n sta span>10 m"),
    ("n_fixed_settled", "AR fixes (settled)"),
    ("corr_north_up", "corr(N, UP)"),
    ("corr_east_up", "corr(E, UP)"),
    ("pca_frac_var_axis1", "PCA frac var axis1"),
    ("pca_axis1_elev_deg", "PCA axis1 elev (deg)"),
    ("pca_axis1_azim_deg", "PCA axis1 azim (deg)"),
    ("geom_null_elev_deg", "geom null elev (deg)"),
    ("geom_null_azim_deg", "geom null azim (deg)"),
    ("angle_err_to_geom_null_deg", "angle err<->null (deg)"),
    ("frac_var_on_geom_null", "frac var on null"),
    ("top_per_epoch_r_up", "top per-ep r(UP)"),
]


def render_compare(cs: list) -> str:
    L = []
    A = L.append
    A("=" * 100)
    A("COMPARISON  (first run is the reference column)")
    A("=" * 100)
    names = [c["run"] for c in cs]
    A(f"{'statistic':<24}" + "".join(f"{n[:20]:>22}" for n in names))
    A("-" * (24 + 22 * len(names)))
    for key, label in COMPARE_KEYS:
        A(f"{label:<24}" + "".join(_f(c.get("headline", {}).get(key), 4, 22) for c in cs))
    A("")
    A("Block-driver Spearman rho(UP rms), top drivers per run (n = settled blocks only):")
    for c in cs:
        top = [r for r in c.get("block_driver_corr", []) if r.get("rho_up_rms") is not None
               and np.isfinite(r["rho_up_rms"])][:6]
        A(f"  {c['run']:<24}" + ", ".join(f"{r['driver']}={r['rho_up_rms']:+.2f}" for r in top))
    A("")
    A("Per-epoch driver r(UP) with autocorrelation-corrected p, top drivers per run:")
    for c in cs:
        top = [r for r in c.get("per_epoch_driver_corr", []) if np.isfinite(r.get("r_up", float("nan")))][:6]
        A(f"  {c['run']:<24}"
          + ", ".join(f"{r['driver']}={r['r_up']:+.2f}(p{r['p_up_eff']:.2f})" for r in top))
    A("")
    return "\n".join(L)


def _jsonable(o):
    if isinstance(o, dict):
        return {str(k): _jsonable(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_jsonable(v) for v in o]
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.floating, float)):
        f = float(o)
        return f if math.isfinite(f) else None
    if isinstance(o, (np.bool_,)):
        return bool(o)
    return o


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="+", type=Path, help="run directory (contains logs/); first is the reference")
    ap.add_argument("--settle-h", type=float, default=6.0, help="settle time in hours (default 6)")
    ap.add_argument("--block-min", type=float, default=30.0, help="block length in minutes (default 30)")
    ap.add_argument("--out-dir", type=Path, default=Path("fieldres"), help="output directory (default ./fieldres)")
    ap.add_argument("--json", type=Path, default=None, help="write the FIRST run's json here as well")
    args = ap.parse_args(argv)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for rd in args.run_dir:
        rd = rd.resolve()
        if not rd.is_dir():
            print(f"!! not a directory: {rd}", file=sys.stderr)
            continue
        c = census(rd, args.settle_h, args.block_min)
        results.append(c)
        txt = render(c)
        (args.out_dir / f"{rd.name}.txt").write_text(txt, encoding="utf-8")
        (args.out_dir / f"{rd.name}.json").write_text(
            json.dumps(_jsonable(c), indent=1), encoding="utf-8"
        )
        print(txt)

    if len(results) > 1:
        cmp_txt = render_compare(results)
        (args.out_dir / "compare.txt").write_text(cmp_txt, encoding="utf-8")
        print(cmp_txt)

    if args.json and results:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(_jsonable(results[0]), indent=1), encoding="utf-8")

    print(f"\n[written] {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
