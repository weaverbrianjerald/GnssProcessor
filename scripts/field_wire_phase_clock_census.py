#!/usr/bin/env python3
"""Field wire: per-satellite phase-clock wander census + consumer geometry closure.

Purpose
-------
On the DOY-129 63-station field bed the closed-loop consumer's settled residual
concentrates on one slowly wandering direction (73 % of the [E,N,U] variance on an
axis at el 49.8 deg / az 9.3 deg, ACF < 0.5 at ~12 min, zero AR fixes) with tropo
coupling closed.  That is the signature of a slowly varying per-satellite error in
the emitted wire, which the float consumer places in its weakest geometric direction.

This probe measures that error on the wire itself, without re-processing:

  step 2  Truth-free smoothness.  Per satellite s and signal b the wire phase-clock
          is formed from the emitted records, and a per-epoch datum over satellites
          is removed (--datum, default a 20 % trimmed mean; the consumer's own
          receiver clock re-absorbs any per-epoch common scalar, so the position part
          of any closure below is exactly invariant to this).  The series is cut into
          tenures (anchor handover / discontinuity bump / pivot / datum change /
          absence), and inside each tenure the deviation from a local linear fit over
          a sliding window (--window-min, default 30 min) is taken.  A real
          Rb/H-maser satellite clock cannot produce that deviation: over 30 min its
          aging term contributes ~1e-6 m, so everything above the product's own noise
          floor is estimator wander.  The floor is measured, not assumed: the same
          statistic is run on the external COD final clock (channel `cod_clk`).

  step 3  IGS/COD comparison.  See the conventions block: on this lineage the
          a-priori named in the product's own S record is a COD MGX final SP3+CLK,
          so the emitted C record is already (estimator clock - COD clock) up to a
          per-epoch constellation-common scalar.  The truth-free clock channel is
          therefore also the truth-graded one; the script says so in its output and
          still cross-checks against the CLK on disk.

  step 4  Geometry closure.  SP3 orbits -> LOS unit vectors at the consumer, design
          matrix G = [-e_ENU, 1] over the satellites above the elevation mask,
          W = diag(sin^2 el), and the predicted consumer error from the wire's own
          wander,  dx(t) = (G'WG)^-1 G'W delta(t).  Correlated against the consumer
          trace, projected onto the measured error axis, and attributed per
          satellite.

  step 5  Event overlay.  |delta| in the 5 min after each event tier vs elsewhere.

Conventions relied on (read from the source, not assumed)
---------------------------------------------------------
* C record  = `clock_c0_m`, a correction in metres added to the a-priori clock:
  GnssCorrections.cpp `clock_corr += SsrCorrectionStore::applyClockToBias(c0,...)`,
  where SsrCorrectionStore::applyClockToBias returns `(c0 + c1 dt + ...)/c` in
  seconds, and PppModel.cpp models `range - sat_clock*c ...`.  So C0 enters the
  predicted observable with coefficient -1.
* P record  = `bias_m`, MeasurementModel.cpp `kSsrPhaseBiasSign = +1.0`,
  "the satellite signal bias PRESENT in the observation, so it is added to the clean
  model".  So P enters the predicted observable with coefficient +1.
* => the model-consistent wire phase-clock is  A_{s,b} = P_{s,b} - C0_s  (channel
  `pc_*`), and an anomaly `a` in A enters the innovation with -1, so the
  innovation-side driver is delta = -dev(A).  The literal sum C0 + P (channel
  `sum_*`) is reported alongside so the attribution can be read either way; the
  wander magnitudes are sign-blind, and only the closure sign depends on this.
* IF combination: P_IF = (f1^2 P_L1C - f2^2 P_L2W)/(f1^2 - f2^2), f1 = 1575.42 MHz,
  f2 = 1227.60 MHz; the clock is band-independent so A_IF = P_IF - C0.
* Consumer error sign: NetworkEmitStore.cpp `d = pos - cl_truth`, so the trace's
  east_m/north_m/up_m are (estimate - truth).  dx above is the estimate's displacement
  caused by the wire anomaly, so a real drive shows a positive correlation; a negative
  one is inconsistent with direct drive, not the same effect with a sign slip.
* Elevation mask: taken from the run's own resolved config
  (`processing.elevation_mask_deg`), overridable with --elev-mask; printed.
* The emitted C0 differs from the estimator's clock by a per-constellation,
  per-epoch scalar only (NetworkClockEmitRebase.cpp: `held = c0 - acc` with one
  accumulator per constellation, "between-sat differences preserved exactly"), which
  the per-epoch datum removal cancels exactly.

Usage
-----
  python scripts/field_wire_phase_clock_census.py <run_dir> [--clk <file>] [--sp3 <file>]
         [--settle-h 6] [--json out.json] [--txt out.txt] [--out-dir DIR]
         [--station NAME] [--station-ecef X,Y,Z] [--elev-mask 15]
         [--window-min 30] [--axis 0.104,0.637,0.764]

`--settle-h H` discards the first H hours; the analysis window is everything after.
"""

import argparse
import datetime
import glob
import json
import math
import os
import re
import sys
import warnings

import numpy as np

C_LIGHT = 299792458.0
OMEGA_E = 7.2921151467e-5          # rad/s, WGS84/GRS80 earth rotation rate
F1 = 1575.42e6
F2 = 1227.60e6
IF_A = F1 * F1 / (F1 * F1 - F2 * F2)      # +2.5457
IF_B = -F2 * F2 / (F1 * F1 - F2 * F2)     # -1.5457
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563

#: Event tiers that break a tenure.  PBGAUGE keys on "<sat>_<signal>"; the rest key
#: on the satellite (or are global).  CLK_ANCHOR / READMIT are listed by the card even
#: though this lineage emits neither, so an arm that emits them is handled unchanged.
SAT_SIG_TIERS = ("PBGAUGE",)
SAT_TIERS = ("PHASE_ANCHOR", "RETRO", "CLK_ANCHOR", "READMIT")
GLOBAL_TIERS = ("PIVOT",)
#: Tiers overlaid in step 5 (not all of them break a tenure).
OVERLAY_TIERS = ("PBGAUGE", "PHASE_ANCHOR", "RETRO", "PIVOT",
                 "CLK_ANCHOR", "READMIT", "ADMISSION", "T0_gap")


# ======================================================================
#  small numeric helpers
# ======================================================================

def _pstd(x):
    x = np.asarray(x, float)
    return float(np.std(x)) if x.size else float("nan")


def _rms(x):
    x = np.asarray(x, float)
    x = x[np.isfinite(x)]
    return float(np.sqrt(np.mean(x * x))) if x.size else float("nan")


def _pearson(a, b):
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 30:
        return float("nan"), float("nan"), int(m.sum())
    a, b = a[m] - a[m].mean(), b[m] - b[m].mean()
    saa, sbb = float(a @ a), float(b @ b)
    if saa <= 0 or sbb <= 0:
        return float("nan"), float("nan"), int(m.size)
    return float(a @ b / math.sqrt(saa * sbb)), float(a @ b / saa), int(m.size)


def _neff(a, b, max_lag=60):
    """Bartlett effective sample size for corr(a, b) on autocorrelated series.

    n_eff = n / (1 + 2 * sum_k rho_a(k) rho_b(k)).  Both series here have a ~13 min
    correlation time on a 30 s grid, so the nominal n = 720 gives far fewer than 720
    independent samples; a raw Pearson p-value would overstate every correlation in
    this report by an order of magnitude.
    """
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    m = np.isfinite(a) & np.isfinite(b)
    a, b = a[m] - a[m].mean(), b[m] - b[m].mean()
    n = a.size
    if n < 60:
        return float(n)

    def rho(x, k):
        return float(x[:-k] @ x[k:] / (x @ x)) if (x @ x) > 0 else 0.0
    s = 0.0
    for k in range(1, min(max_lag, n // 4) + 1):
        s += rho(a, k) * rho(b, k)
    return float(max(3.0, n / max(1e-9, 1.0 + 2.0 * s)))


def _fisher_z(r, n_eff):
    """Two-sided normal-approximation |z| for a Pearson r at n_eff samples."""
    if not np.isfinite(r) or n_eff <= 3 or abs(r) >= 1.0:
        return float("nan")
    return abs(math.atanh(r)) * math.sqrt(n_eff - 3.0)


def _acf_first_below(series_list, thresh=0.5, max_lag=120):
    """Pooled ACF over a list of 1-D NaN-bearing series; first lag with acf < thresh.

    Pairs are pooled across series (each series demeaned on its own finite entries),
    so a satellite with a short tenure contributes what it has instead of being
    dropped.  Returns (lag_epochs, acf_array).
    """
    num = np.zeros(max_lag + 1)
    den0 = 0.0
    cnt = np.zeros(max_lag + 1)
    for s in series_list:
        s = np.asarray(s, float)
        f = np.isfinite(s)
        if f.sum() < 60:
            continue
        d = np.where(f, s - s[f].mean(), np.nan)
        den0 += float(np.nansum(d * d))
        n0 = int(f.sum())
        for k in range(max_lag + 1):
            if k == 0:
                num[0] += float(np.nansum(d * d))
                cnt[0] += n0
                continue
            p = d[:-k] * d[k:]
            g = np.isfinite(p)
            if g.any():
                num[k] += float(p[g].sum())
                cnt[k] += int(g.sum())
    with np.errstate(invalid="ignore", divide="ignore"):
        mean_prod = np.where(cnt > 0, num / np.maximum(cnt, 1), np.nan)
    if not np.isfinite(mean_prod[0]) or mean_prod[0] <= 0:
        return None, None
    acf = mean_prod / mean_prod[0]
    below = np.where(acf < thresh)[0]
    return (int(below[0]) if below.size else None), acf


# ======================================================================
#  wire parsing
# ======================================================================

def qtow(t):
    """Quantize a TOW to the nearest second.  The emitter prints accumulated float
    error (302370.00000190735) while every trace CSV prints %.3f; the grid is 30 s so
    nearest-second is unambiguous and makes cross-file joins exact."""
    return int(round(t))


class Wire:
    """Parsed products.txt: the epoch grid, the C/P values, the R bumps, the S contract."""

    def __init__(self):
        self.contract = {}
        self.tows = []                 # int TOW, emission order
        self.week = None
        self.datum_changed = []        # per epoch
        self.status = []               # per epoch H status ('' when absent)
        self.clk = {}                  # sat -> {ep_index: c0_m}
        self.pb = {}                   # (sat, sig) -> {ep_index: bias_m}
        self.retro = {}                # sat -> set(ep_index)


def parse_wire(path):
    w = Wire()
    ep = -1
    pending_h = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            p = line.rstrip("\n").split(",")
            k = p[0]
            if k == "S":
                w.contract = {
                    "ref_rx": p[1] if len(p) > 1 else "",
                    "bias_sign": p[2] if len(p) > 2 else "",
                    "windup_id": p[3] if len(p) > 3 else "",
                    "emit_cb": p[6] if len(p) > 6 else "",
                    "disc_thresh_m": p[8] if len(p) > 8 else "",
                    "apriori_product_id": p[10] if len(p) > 10 else "",
                    "extra": [x for x in p[11:]],
                }
            elif k == "E":
                try:
                    w.week = int(p[1])
                    t = qtow(float(p[2]))
                except (IndexError, ValueError):
                    continue
                ep += 1
                w.tows.append(t)
                dc = 0
                if len(p) > 6 and p[6] and "=" not in p[6]:
                    try:
                        dc = int(p[6])
                    except ValueError:
                        dc = 0
                w.datum_changed.append(dc)
                w.status.append(pending_h.pop(t, ""))
            elif k == "H":
                try:
                    t = qtow(float(p[2]))
                except (IndexError, ValueError):
                    continue
                # H precedes its own E in the stream (heartbeat first).
                pending_h[t] = p[3] if len(p) > 3 else ""
            elif k == "R" and ep >= 0:
                for s in p[1:]:
                    if s:
                        w.retro.setdefault(s, set()).add(ep)
            elif k == "C" and ep >= 0:
                if len(p) >= 3 and "=" not in p[2]:
                    try:
                        v = float(p[2])
                    except ValueError:
                        continue
                    if math.isfinite(v):
                        w.clk.setdefault(p[1], {})[ep] = v
            elif k == "P" and ep >= 0:
                if len(p) >= 4:
                    try:
                        v = float(p[3])
                    except ValueError:
                        continue
                    if math.isfinite(v):
                        w.pb.setdefault((p[1], p[2]), {})[ep] = v
    # H rows whose E was withheld (no epoch emitted) simply stay in pending_h.
    w.n_withheld_h = len(pending_h)
    return w


def parse_events(path):
    """network_events.csv -> {tier: [(tow, key, detail)]}, key = sat or sat_signal."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\n").split(",")
        try:
            i_tow, i_tier, i_sat = header.index("tow"), header.index("tier"), header.index("sat")
        except ValueError:
            return out
        i_evt = header.index("event") if "event" in header else None
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) <= max(i_tow, i_tier, i_sat):
                continue
            try:
                t = qtow(float(p[i_tow]))
            except ValueError:
                continue
            out.setdefault(p[i_tier], []).append(
                (t, p[i_sat], p[i_evt] if i_evt is not None else ""))
    return out


# ======================================================================
#  external product parsing
# ======================================================================

def parse_clk(path, sats=None):
    """RINEX CLK `AS` records -> {sat: {tow_int: bias_m}} for the file's own day.

    TOW is derived from the record's own calendar date via the GPS epoch, so a file
    for a neighbouring day lands on the right absolute grid with no --dow guessing.
    """
    out = {}
    gps0 = datetime.datetime(1980, 1, 6)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        in_header = True
        for line in f:
            if in_header:
                if "END OF HEADER" in line:
                    in_header = False
                continue
            if not line.startswith("AS "):
                continue
            p = line.split()
            if len(p) < 10:
                continue
            sat = p[1]
            if sats is not None and sat not in sats:
                continue
            try:
                yy, mm, dd = int(p[2]), int(p[3]), int(p[4])
                hh, mi = int(p[5]), int(p[6])
                sec = float(p[7])
                bias = float(p[9])
            except ValueError:
                continue
            days = (datetime.datetime(yy, mm, dd) - gps0).days
            tow = (days % 7) * 86400.0 + hh * 3600.0 + mi * 60.0 + sec
            out.setdefault(sat, {})[qtow(tow)] = bias * C_LIGHT
    return out


def parse_sp3(path, sats=None):
    """SP3-c/d -> {sat: (tow_int array, xyz metres array)}; one day, GPS TOW."""
    recs = {}
    gps0 = datetime.datetime(1980, 1, 6)
    tow = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("*"):
                p = line.split()
                try:
                    yy, mm, dd, hh, mi = int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])
                    sec = float(p[6])
                except (IndexError, ValueError):
                    tow = None
                    continue
                days = (datetime.datetime(yy, mm, dd) - gps0).days
                tow = (days % 7) * 86400.0 + hh * 3600.0 + mi * 60.0 + sec
            elif line.startswith("P") and tow is not None:
                sat = line[1:4].strip()
                if sats is not None and sat not in sats:
                    continue
                try:
                    x = float(line[4:18]) * 1000.0
                    y = float(line[18:32]) * 1000.0
                    z = float(line[32:46]) * 1000.0
                except ValueError:
                    continue
                if abs(x) < 1.0 and abs(y) < 1.0 and abs(z) < 1.0:
                    continue                      # SP3 "bad/absent" sentinel
                recs.setdefault(sat, []).append((tow, x, y, z))
    out = {}
    for sat, rows in recs.items():
        rows.sort()
        t = np.array([r[0] for r in rows], float)
        p = np.array([[r[1], r[2], r[3]] for r in rows], float)
        out[sat] = (t, p)
    return out


def neville(tq, t, y, order=9):
    """Neville interpolation of a vector series y(t) at scalar tq, `order` points."""
    n = len(t)
    i = int(np.searchsorted(t, tq))
    lo = max(0, min(i - order // 2, n - order))
    hi = lo + order
    tt = t[lo:hi]
    c = y[lo:hi].astype(float).copy()
    for k in range(1, order):
        for j in range(order - k):
            c[j] = ((tq - tt[j + k]) * c[j] + (tt[j] - tq) * c[j + 1]) / (tt[j] - tt[j + k])
    return c[0]


# ======================================================================
#  geodesy
# ======================================================================

def ecef_to_geodetic(x, y, z):
    e2 = WGS84_F * (2.0 - WGS84_F)
    lon = math.atan2(y, x)
    r = math.hypot(x, y)
    lat = math.atan2(z, r * (1.0 - e2))
    for _ in range(8):
        sl = math.sin(lat)
        n = WGS84_A / math.sqrt(1.0 - e2 * sl * sl)
        h = r / math.cos(lat) - n
        lat = math.atan2(z, r * (1.0 - e2 * n / (n + h)))
    sl = math.sin(lat)
    n = WGS84_A / math.sqrt(1.0 - e2 * sl * sl)
    h = r / math.cos(lat) - n
    return lat, lon, h


def enu_basis(lat, lon):
    sla, cla = math.sin(lat), math.cos(lat)
    slo, clo = math.sin(lon), math.cos(lon)
    return np.array([[-slo, clo, 0.0],
                     [-sla * clo, -sla * slo, cla],
                     [cla * clo, cla * slo, sla]])


# ======================================================================
#  tenure segmentation + local-linear detrend
# ======================================================================

def build_tenures(valid, breaks):
    """valid: bool[N]; breaks: set of epoch indices where a new tenure starts.

    Returns int[N] tenure id (-1 where invalid).  A missing epoch always breaks the
    tenure, so every tenure is a contiguous run of valid epochs, which lets the
    sliding fit below use O(1) prefix sums.
    """
    n = len(valid)
    tid = np.full(n, -1, dtype=np.int64)
    cur = -1
    prev_valid = False
    for i in range(n):
        if not valid[i]:
            prev_valid = False
            continue
        if (not prev_valid) or (i in breaks):
            cur += 1
        tid[i] = cur
        prev_valid = True
    return tid


def local_linear_resid(vals, tid, tows, halfwin_s, min_pts=12, min_span_s=300.0):
    """Deviation of vals from a local linear fit over a +/- halfwin_s window, never
    crossing a tenure boundary.  Returns float[N] with NaN where undefined.

    The residual is scaled by sqrt(n/(n-2)) so that windows truncated at a tenure
    edge (which have fewer degrees of freedom, hence structurally smaller residuals)
    are directly comparable with full-width windows.  Without the scaling every
    post-event epoch would read artificially quiet, and those epochs are the
    population step 5 examines.
    """
    n = len(vals)
    out = np.full(n, np.nan)
    i = 0
    while i < n:
        if tid[i] < 0:
            i += 1
            continue
        j = i
        while j + 1 < n and tid[j + 1] == tid[i]:
            j += 1
        seg = slice(i, j + 1)
        t = (tows[seg] - tows[i]) / 30.0        # epochs, conditioning-friendly
        y = vals[seg]
        L = len(t)
        if L >= min_pts:
            c1 = np.concatenate(([0.0], np.cumsum(np.ones(L))))
            ct = np.concatenate(([0.0], np.cumsum(t)))
            ct2 = np.concatenate(([0.0], np.cumsum(t * t)))
            cy = np.concatenate(([0.0], np.cumsum(y)))
            cty = np.concatenate(([0.0], np.cumsum(t * y)))
            hw = halfwin_s / 30.0
            res = np.full(L, np.nan)
            for k in range(L):
                a = int(np.searchsorted(t, t[k] - hw, side="left"))
                b = int(np.searchsorted(t, t[k] + hw, side="right"))
                m = b - a
                if m < min_pts or (t[b - 1] - t[a]) * 30.0 < min_span_s:
                    continue
                s1 = c1[b] - c1[a]
                st = ct[b] - ct[a]
                st2 = ct2[b] - ct2[a]
                sy = cy[b] - cy[a]
                sty = cty[b] - cty[a]
                det = s1 * st2 - st * st
                if det <= 0:
                    continue
                slope = (s1 * sty - st * sy) / det
                icpt = (sy - slope * st) / s1
                res[k] = (y[k] - (icpt + slope * t[k])) * math.sqrt(m / (m - 2.0))
            out[seg] = res
        i = j + 1
    return out


# ======================================================================
#  channel construction
# ======================================================================

def channel_matrix(w, sats, n_ep, kind):
    """Raw (undatumed) channel value per (sat, epoch); NaN where absent.

    kind is one of: clk, pb_<band>, pc_<band>, sum_<band> with
    band in {L1C, L2W, IF, MEAN}.
    `pc_*` = P - C0 (the model-consistent wire term; see the conventions block).
    `sum_*` = C0 + P (the literal c = clk + P of the request).
    `*_IF`   = the iono-free combination of the two band biases.
    `*_MEAN` = the plain band average: the band-common driver for a consumer
               that runs uncombined with a tight external ionosphere prior
               (`consume_network_iono`), where each band's error maps to
               position on its own rather than through the IF coefficients.
    """
    M = np.full((len(sats), n_ep), np.nan)
    for si, s in enumerate(sats):
        c = w.clk.get(s, {})
        if kind == "clk":
            for ep, v in c.items():
                M[si, ep] = v
            continue
        band = kind.split("_", 1)[1]
        if band in ("IF", "MEAN"):
            ca, cb = (IF_A, IF_B) if band == "IF" else (0.5, 0.5)
            a = w.pb.get((s, "L1C"), {})
            b = w.pb.get((s, "L2W"), {})
            keys = a.keys() & b.keys()
            pb = {ep: ca * a[ep] + cb * b[ep] for ep in keys}
        else:
            pb = w.pb.get((s, band), {})
        if kind.startswith("pb_"):
            for ep, v in pb.items():
                M[si, ep] = v
        elif kind.startswith("pc_"):
            for ep, v in pb.items():
                if ep in c:
                    M[si, ep] = v - c[ep]
        elif kind.startswith("sum_"):
            for ep, v in pb.items():
                if ep in c:
                    M[si, ep] = v + c[ep]
    return M


def channel_breaks(w, sats, events, n_ep, tow_to_ep, kind):
    """Per-satellite set of epoch indices that start a new tenure."""
    glob = set()
    for i, dc in enumerate(w.datum_changed):
        if dc:
            glob.add(i)
    for tier in GLOBAL_TIERS:
        for t, _key, _e in events.get(tier, []):
            ep = tow_to_ep.get(t)
            if ep is not None:
                glob.add(ep)
    per = {s: set(glob) for s in sats}
    for tier in SAT_TIERS:
        for t, key, _e in events.get(tier, []):
            ep = tow_to_ep.get(t)
            if ep is not None and key in per:
                per[key].add(ep)
    for s, eps in w.retro.items():
        if s in per:
            per[s] |= set(eps)
    if kind != "clk":
        band = kind.split("_", 1)[1]
        want = {"L1C", "L2W"} if band in ("IF", "MEAN") else {band}
        for tier in SAT_SIG_TIERS:
            for t, key, _e in events.get(tier, []):
                ep = tow_to_ep.get(t)
                if ep is None or "_" not in key:
                    continue
                s, sig = key.rsplit("_", 1)
                if s in per and sig in want:
                    per[s].add(ep)
    return per


def epoch_center(M, mode="trim", frac=0.20):
    """Per-epoch location estimate over the satellites present (NaN-aware).

    'trim' is a symmetric `frac` trimmed mean.  It is the default because the two
    obvious choices each fail one of the two requirements on this estimator:
      * the mean is smooth in time but a single satellite's jump leaks into every
        other one (measured: a -189.7 m step at TOW 292740 became -5.94 m on all 32);
      * the median is outlier-proof but its own value hops between order statistics,
        and that hopping sets a floor: on the COD final clock the median datum's own
        30-min residual is 0.034 m against 0.007 m for the mean, so the datum
        estimator alone would set the noise floor of this report.
    A 20 % trimmed mean measures 0.008 m on the same test: outlier-proof and smooth.
    """
    n_s, n_ep = M.shape
    out = np.full(n_ep, np.nan)
    for i in range(n_ep):
        v = M[:, i]
        v = v[np.isfinite(v)]
        if v.size == 0:
            continue
        if mode == "mean":
            out[i] = v.mean()
        elif mode == "median":
            out[i] = float(np.median(v))
        else:
            v = np.sort(v)
            k = int(np.floor(frac * v.size))
            out[i] = v[k:v.size - k].mean() if v.size - 2 * k > 0 else v.mean()
    return out


def post_center(D, mode="trim"):
    """Make a per-satellite deviation field orthogonal to the per-epoch constant.

    The consumer carries a free receiver clock, so any per-epoch value common to all
    satellites is absorbed exactly and cannot reach position.  Removing it here (a)
    reports only the part that can reach position and (b) cancels, to first order,
    whatever noise the datum estimator itself injected, which is why the numbers
    below barely move between --datum mean / median / trim.
    """
    return D - epoch_center(D, mode)[None, :]


def datum_remove(M, mode="median"):
    """Remove a per-epoch constant over the satellites present at that epoch.

    Datum invariance: the geometry closure's position components are exactly
    invariant to this choice, because G carries a receiver-clock column that
    re-absorbs any per-epoch constant.  Removing it here only makes the per-satellite
    contrast readable.

    Every level datum has an inlier-set edge, at which one satellite's jump is copied
    onto all the others.  Measured on this bed: at TOW 292740 one satellite's IF
    phase-clock steps -189.7 m while the other 31 move < 0.13 m, yet the datum itself
    moves -5.94 m (mean) / -4.88 m (trim) / -1.22 m (median) because that satellite
    crosses the estimator's inclusion boundary.  Nothing downstream is therefore
    thresholded on this series directly: the residual field goes through
    post_center() and the step census re-centres the one-epoch differences on their
    own median.  With those two in place the report does not depend on the datum
    choice.
    """
    ctr = epoch_center(M, mode)
    return M - ctr[None, :], ctr


# ======================================================================
#  main analysis
# ======================================================================

def tenure_demean(x, tid):
    """Remove the mean of each tenure (the consumer's constant float ambiguity per
    satellite-signal arc absorbs exactly this level, and nothing more)."""
    out = np.full_like(x, np.nan)
    for t in np.unique(tid[tid >= 0]):
        m = tid == t
        v = x[m]
        f = np.isfinite(v)
        if f.sum() >= 3:
            out[m] = v - v[f].mean()
    return out


def pass_segments(vis_row, break_eps):
    """Contiguous runs of consumer visibility, further broken at `break_eps`.

    This is the segmentation a float consumer imposes: one constant ambiguity per
    satellite-signal arc, re-floated only when the arc breaks or the wire signals a
    discontinuity.  The producer's own anchor tenures are much shorter, so they are
    the wrong unit for "what the consumer can absorb".
    """
    segs = []
    i, n = 0, len(vis_row)
    while i < n:
        if not vis_row[i]:
            i += 1
            continue
        j = i
        while j + 1 < n and vis_row[j + 1] and (j + 1) not in break_eps:
            j += 1
        segs.append((i, j + 1))
        i = j + 1
    return segs


def pass_demean(x, segs, order=0, min_pts=20):
    """Remove the per-pass mean (order 0) or per-pass mean+trend (order 1)."""
    out = np.full_like(x, np.nan)
    for a, b in segs:
        v = x[a:b]
        f = np.isfinite(v)
        if f.sum() < min_pts:
            continue
        if order == 0:
            out[a:b] = v - v[f].mean()
        else:
            t = np.arange(b - a, dtype=float)
            c = np.polyfit(t[f], v[f], 1)
            out[a:b] = v - np.polyval(c, t)
    return out


def analyse_channel(w, sats, events, tows, tow_to_ep, kind, halfwin_s, settle_ep,
                    step_cut_m=None, datum="median"):
    """One channel end to end.

    `step_cut_m` (metres): when given, every within-tenure one-epoch jump larger than
    this is treated as an extra tenure boundary.  Comparing the wander with and
    without that cut splits the measured deviation into unmarked step content and
    smooth wander, that is, into "an unsignalled re-referencing" and "a slowly varying
    per-satellite error", which are different defects with different fixes.
    """
    n_ep = len(tows)
    raw = channel_matrix(w, sats, n_ep, kind)
    X, epoch_mean = datum_remove(raw, datum)
    breaks = channel_breaks(w, sats, events, n_ep, tow_to_ep, kind)
    D = np.full_like(X, np.nan)
    T = np.full_like(X, np.nan)
    tenures = {}
    steps = {}
    step_hist = np.zeros(5, dtype=int)      # >0.05 >0.10 >0.25 >0.50 >1.00 m
    step_epochs = {}

    # ---- pass 1: tenures + one-epoch differences ---------------------------
    # The differences are re-centred per epoch on their own median across
    # satellites before anything is counted.  Without that, any level-based datum
    # moves when one satellite leaves the estimator's inlier set, and the census
    # reads a single satellite's jump as "all 32 stepped": at TOW 292740 one
    # satellite steps -189.7 m and the level datum itself moves -4.9 m (trim) /
    # -5.9 m (mean) / -1.2 m (median), which then appears on every other satellite.
    # Re-centring the differences makes the census independent of the datum choice.
    tids = {}
    d1m = np.full((len(sats), n_ep), np.nan)
    for si, s in enumerate(sats):
        valid = np.isfinite(X[si])
        if valid.sum() < 20:
            continue
        tid = build_tenures(valid, breaks[s])
        tids[s] = tid
        d1m[si, 1:] = np.where((tid[1:] >= 0) & (tid[1:] == tid[:-1]),
                               X[si, 1:] - X[si, :-1], np.nan)
    with np.errstate(invalid="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        d1c = np.nanmedian(d1m, axis=0)
    d1m = d1m - np.where(np.isfinite(d1c), d1c, 0.0)[None, :]

    # ---- pass 2: step census + (optionally step-cut) tenure detrend ---------
    for si, s in enumerate(sats):
        tid = tids.get(s)
        if tid is None:
            continue
        valid = np.isfinite(X[si])
        d1 = d1m[si]
        a = np.abs(d1[settle_ep:])
        for k, th in enumerate((0.05, 0.10, 0.25, 0.50, 1.00)):
            step_hist[k] += int(np.nansum(a > th))
        big = set(np.where(np.abs(d1) > (step_cut_m or 1e18))[0].tolist())
        steps[s] = big
        for ep in np.where(a > 0.25)[0]:
            step_epochs.setdefault(int(ep) + settle_ep, []).append(s)
        if step_cut_m:
            tid = build_tenures(valid, breaks[s] | big)
        tenures[s] = int(tid.max() + 1) if tid.max() >= 0 else 0
        D[si] = local_linear_resid(X[si], tid, np.asarray(tows, float), halfwin_s)
        T[si] = tenure_demean(X[si], tid)
    D = post_center(D, datum)
    T = post_center(T, datum)
    Dw = D[:, settle_ep:]
    per_sat = {}
    for si, s in enumerate(sats):
        v = Dw[si][np.isfinite(Dw[si])]
        if v.size >= 60:
            per_sat[s] = {"rms_m": float(np.sqrt(np.mean(v * v))),
                          "p95_abs_m": float(np.percentile(np.abs(v), 95)),
                          "n": int(v.size),
                          "tenures": tenures.get(s, 0)}
    rmss = np.array([d["rms_m"] for d in per_sat.values()])
    lag, acf = _acf_first_below([Dw[si] for si in range(len(sats))])
    allv = Dw[np.isfinite(Dw)]
    return {
        "kind": kind,
        "n_sat": len(per_sat),
        "rms_p50_m": float(np.percentile(rmss, 50)) if rmss.size else float("nan"),
        "rms_p90_m": float(np.percentile(rmss, 90)) if rmss.size else float("nan"),
        "rms_max_m": float(rmss.max()) if rmss.size else float("nan"),
        "rms_pooled_m": _rms(allv),
        "abs_p50_m": float(np.percentile(np.abs(allv), 50)) if allv.size else float("nan"),
        "abs_p90_m": float(np.percentile(np.abs(allv), 90)) if allv.size else float("nan"),
        "abs_p99_m": float(np.percentile(np.abs(allv), 99)) if allv.size else float("nan"),
        "acf_below_half_lag_ep": lag,
        "acf_below_half_min": (lag * 30.0 / 60.0) if lag else None,
        "hist_abs": np.histogram(np.abs(allv),
                                 bins=[0, .01, .02, .05, .1, .2, .5, 1.0, 2.0, 1e9])[0].tolist(),
        "tenure_rms_p50_m": float(np.percentile(
            [r for r in (_rms(T[si, settle_ep:]) for si in range(len(sats)))
             if np.isfinite(r)], 50)),
        "per_sat": per_sat,
        "step_hist": step_hist.tolist(),
        "step_count_per_epoch": np.nansum(np.abs(d1m) > 0.25, axis=0).astype(float),
        "step_epochs": {k: v for k, v in sorted(step_epochs.items(),
                                                key=lambda kv: -len(kv[1]))[:10]},
        "n_step_25": int(step_hist[2]),
        "X": X,
        "D": D,
        "T": T,
        "epoch_mean": epoch_mean,
    }


def cod_floor_channel(clk, sats, tows, tow_to_ep, halfwin_s, settle_ep, events, w, n_ep,
                      datum="median"):
    """The same 30-min statistic on the external COD final clock: the measured
    noise floor of the instrument."""
    M = np.full((len(sats), n_ep), np.nan)
    for si, s in enumerate(sats):
        d = clk.get(s, {})
        for t, v in d.items():
            ep = tow_to_ep.get(t)
            if ep is not None:
                M[si, ep] = v
    X, _ = datum_remove(M, datum)
    D = np.full_like(X, np.nan)
    for si in range(len(sats)):
        valid = np.isfinite(X[si])
        if valid.sum() < 20:
            continue
        tid = build_tenures(valid, set())
        D[si] = local_linear_resid(X[si], tid, np.asarray(tows, float), halfwin_s)
    D = post_center(D, datum)
    Dw = D[:, settle_ep:]
    rmss = np.array([_rms(Dw[si]) for si in range(len(sats))])
    rmss = rmss[np.isfinite(rmss)]
    allv = Dw[np.isfinite(Dw)]
    return {"kind": "cod_clk", "n_sat": int(rmss.size),
            "rms_p50_m": float(np.percentile(rmss, 50)) if rmss.size else float("nan"),
            "rms_p90_m": float(np.percentile(rmss, 90)) if rmss.size else float("nan"),
            "rms_max_m": float(rmss.max()) if rmss.size else float("nan"),
            "rms_pooled_m": _rms(allv),
            "abs_p50_m": float(np.percentile(np.abs(allv), 50)) if allv.size else float("nan"),
            "abs_p90_m": float(np.percentile(np.abs(allv), 90)) if allv.size else float("nan"),
            "abs_p99_m": float(np.percentile(np.abs(allv), 99)) if allv.size else float("nan")}


# ======================================================================
#  consumer trace + geometry
# ======================================================================

def parse_closed_loop(path):
    tow, e, n, u, ns, nf = [], [], [], [], [], []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        head = f.readline().rstrip("\n").split(",")
        idx = {k: i for i, k in enumerate(head)}
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) < len(head):
                continue
            try:
                tow.append(qtow(float(p[idx["tow"]])))
                e.append(float(p[idx["east_m"]]))
                n.append(float(p[idx["north_m"]]))
                u.append(float(p[idx["up_m"]]))
                ns.append(int(p[idx["n_sats"]]))
                nf.append(int(p[idx["num_fixed"]]))
            except (ValueError, KeyError):
                continue
    return (np.array(tow), np.array(e), np.array(n), np.array(u),
            np.array(ns), np.array(nf))


def geometry(sp3, sats, tows, rx_ecef, elev_mask_deg):
    """Per epoch: unit LOS in ENU + elevation for each satellite with an SP3 arc."""
    lat, lon, _h = ecef_to_geodetic(*rx_ecef)
    R = enu_basis(lat, lon)
    n_ep, n_s = len(tows), len(sats)
    E = np.full((n_s, n_ep, 3), np.nan)
    EL = np.full((n_s, n_ep), np.nan)
    rx = np.asarray(rx_ecef, float)
    for si, s in enumerate(sats):
        arc = sp3.get(s)
        if arc is None:
            continue
        t_sp3, p_sp3 = arc
        for ei, t in enumerate(tows):
            if t < t_sp3[0] - 1.0 or t > t_sp3[-1] + 1.0:
                continue
            tau = 0.075
            pr = None
            for _ in range(2):
                p = neville(t - tau, t_sp3, p_sp3)
                th = OMEGA_E * tau
                ct, st = math.cos(th), math.sin(th)
                pr = np.array([ct * p[0] + st * p[1], -st * p[0] + ct * p[1], p[2]])
                tau = float(np.linalg.norm(pr - rx)) / C_LIGHT
            d = pr - rx
            d /= np.linalg.norm(d)
            enu = R @ d
            E[si, ei] = enu
            EL[si, ei] = math.degrees(math.asin(max(-1.0, min(1.0, enu[2]))))
    VIS = np.isfinite(EL) & (EL >= elev_mask_deg)
    return E, EL, VIS


def closure(E, EL, VIS, D, sats, tows, settle_ep, axis, with_zwd=False, min_sats=5):
    """dx(t) = (G'WG)^-1 G'W delta(t), delta = -dev(A) (see the module conventions)."""
    n_s, n_ep = EL.shape
    ncol = 5 if with_zwd else 4
    dx = np.full((n_ep, 3), np.nan)
    contrib = np.zeros((n_s, n_ep))
    nvis = np.zeros(n_ep, dtype=int)
    axis = np.asarray(axis, float)
    axis = axis / np.linalg.norm(axis)
    for ei in range(settle_ep, n_ep):
        m = VIS[:, ei] & np.isfinite(D[:, ei])
        k = int(m.sum())
        nvis[ei] = k
        if k < min_sats:
            continue
        e = E[m, ei, :]
        el = np.radians(EL[m, ei])
        G = np.empty((k, ncol))
        G[:, 0:3] = -e
        G[:, 3] = 1.0
        if with_zwd:
            G[:, 4] = 1.0 / np.sin(el)
        wgt = np.sin(el) ** 2
        delta = -D[m, ei]                      # innovation-side sign
        GtW = G.T * wgt
        N = GtW @ G
        try:
            Ninv = np.linalg.inv(N)
        except np.linalg.LinAlgError:
            continue
        M = Ninv @ GtW                          # (ncol, k)
        sol = M @ delta
        dx[ei] = sol[0:3]
        wrow = axis @ M[0:3, :]                 # per-sat weight along the axis
        contrib[np.where(m)[0], ei] = wrow * delta
    return dx, contrib, nvis


# ======================================================================
#  driver
# ======================================================================

def resolve_run_config(run_dir):
    """Read the run's own manifest -> resolved config (station, ECEF, mask)."""
    info = {"station": None, "ecef": None, "elev_mask": None, "config_path": None,
            "n_stations": None}
    man = os.path.join(run_dir, "logs", "network_run_manifest.json")
    if not os.path.exists(man):
        return info
    cfg_path = None
    with open(man, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get("record") == "startup":
                cfg_path = rec.get("config_path")
                break
    info["config_path"] = cfg_path
    if not cfg_path or not os.path.exists(cfg_path):
        return info
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        from gnss_config_chain import load_config
        cfg, _chain = load_config(cfg_path)
    except Exception as exc:                                   # noqa: BLE001
        print(f"  (config chain unreadable: {exc})", file=sys.stderr)
        return info
    st = cfg.get("network", {}).get("closed_loop", {}).get("station", {})
    info["station"] = st.get("name")
    ecef = st.get("position_ecef")
    if isinstance(ecef, list) and len(ecef) == 3:
        info["ecef"] = [float(v) for v in ecef]
    em = cfg.get("processing", {}).get("elevation_mask_deg")
    if em is not None:
        info["elev_mask"] = float(em)
    info["n_stations"] = len(cfg.get("network", {}).get("stations", []) or [])
    return info


def autodiscover_products(week, tow, root="data"):
    gps0 = datetime.datetime(1980, 1, 6)
    day = gps0 + datetime.timedelta(weeks=int(week), days=int(tow // 86400))
    tag = f"{day.year}-{day.timetuple().tm_yday:03d}"
    base = os.path.join(root, tag, "products")
    clk = sorted(glob.glob(os.path.join(base, "*.CLK")) + glob.glob(os.path.join(base, "*CLK*")))
    sp3 = sorted(glob.glob(os.path.join(base, "*.SP3")) + glob.glob(os.path.join(base, "*ORB*")))

    def rank(p):
        b = os.path.basename(p).upper()
        return (0 if ("COD" in b or "IGS" in b) else 1, 0 if "FIN" in b else 1, b)
    clk = sorted({p for p in clk if p.upper().endswith(".CLK")}, key=rank)
    sp3 = sorted({p for p in sp3 if p.upper().endswith(".SP3")}, key=rank)
    return (clk[0] if clk else None), (sp3[0] if sp3 else None), tag


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--clk", default=None, help="external RINEX CLK (diagnostic only)")
    ap.add_argument("--sp3", default=None, help="SP3 orbit for the LOS geometry")
    ap.add_argument("--settle-h", type=float, default=6.0,
                    help="hours DISCARDED from the start (default 6)")
    ap.add_argument("--window-min", type=float, default=30.0,
                    help="sliding local-linear window, minutes (default 30)")
    ap.add_argument("--json", default=None)
    ap.add_argument("--txt", default=None)
    ap.add_argument("--out-dir", default="fieldres")
    ap.add_argument("--station", default=None)
    ap.add_argument("--station-ecef", default=None, help="X,Y,Z metres")
    ap.add_argument("--elev-mask", type=float, default=None)
    ap.add_argument("--axis", default=None,
                    help="ENU error axis to project onto; default = the consumer "
                         "trace's OWN settled PC1 (measured, not assumed)")
    ap.add_argument("--data-root", default="data")
    ap.add_argument("--datum", choices=("trim", "median", "mean"), default="trim",
                    help="per-epoch datum removed over satellites (default trim = 20 %% "
                         "trimmed mean; 'mean' leaks one satellite's jump into all of "
                         "them, 'median' hops between order statistics)")
    args = ap.parse_args()

    run_dir = os.path.abspath(args.run_dir)
    run_name = os.path.basename(run_dir.rstrip("\\/"))
    os.makedirs(args.out_dir, exist_ok=True)
    txt_path = args.txt or os.path.join(args.out_dir, f"wire_phase_clock_{run_name}.txt")
    json_path = args.json or os.path.join(args.out_dir, f"wire_phase_clock_{run_name}.json")

    out = []

    def say(s=""):
        out.append(s)
        print(s)

    # ---------------- inputs ----------------
    wire_path = os.path.join(run_dir, "products.txt")
    if not os.path.exists(wire_path):
        sys.exit(f"FATAL: no products.txt in {run_dir}")
    w = parse_wire(wire_path)
    tows = np.array(w.tows, dtype=np.int64)
    n_ep = len(tows)
    if n_ep < 200:
        sys.exit(f"FATAL: only {n_ep} emitted epochs")
    tow_to_ep = {int(t): i for i, t in enumerate(tows)}
    sats = sorted(w.clk.keys())
    events = parse_events(os.path.join(run_dir, "logs", "network_events.csv"))

    cfg = resolve_run_config(run_dir)
    station = args.station or cfg["station"]
    if args.station_ecef:
        ecef = [float(v) for v in args.station_ecef.split(",")]
    else:
        ecef = cfg["ecef"]
    elev_mask = args.elev_mask if args.elev_mask is not None else (cfg["elev_mask"] or 15.0)
    axis = (np.array([float(v) for v in args.axis.split(",")], float)
            if args.axis else None)

    clk_path, sp3_path, daytag = autodiscover_products(w.week, float(tows[0]), args.data_root)
    if args.clk:
        clk_path = args.clk
    if args.sp3:
        sp3_path = args.sp3

    dt = float(np.median(np.diff(tows))) if n_ep > 1 else 30.0
    settle_ep = int(max(0, round(args.settle_h * 3600.0 / dt)))
    if settle_ep >= n_ep - 60:
        sys.exit(f"FATAL: --settle-h {args.settle_h} leaves {n_ep - settle_ep} epochs")
    halfwin_s = args.window_min * 60.0 / 2.0

    say("=" * 96)
    say(f"FIELD WIRE PHASE-CLOCK CENSUS — {run_name}")
    say("=" * 96)
    say(f"  wire            : {wire_path}")
    say(f"  epochs          : {n_ep} emitted, dt {dt:.0f} s, TOW {tows[0]}..{tows[-1]} "
        f"(week {w.week}, {daytag}), {n_ep*dt/3600.0:.1f} h")
    say(f"  H rows with no emitted epoch (withheld) : {w.n_withheld_h}")
    say(f"  satellites      : {len(sats)} ({sats[0]}..{sats[-1]})")
    say(f"  S contract      : ref_rx={w.contract.get('ref_rx')} "
        f"bias_sign={w.contract.get('bias_sign')} windup={w.contract.get('windup_id')} "
        f"emit_cb={w.contract.get('emit_cb')} disc_thresh={w.contract.get('disc_thresh_m')} m")
    say(f"  a-priori id     : {w.contract.get('apriori_product_id')}")
    say(f"  consumer        : {station}  ECEF {ecef}")
    say(f"  elevation mask  : {elev_mask:.1f} deg   (from {cfg['config_path']})")
    say(f"  error axis      : {'given ' + args.axis if args.axis else 'measured from the consumer trace (PC1 of the settled ENU error)'}")
    say(f"  settle          : first {args.settle_h:g} h discarded -> "
        f"{n_ep - settle_ep} epochs = {(n_ep-settle_ep)*dt/3600.0:.1f} h analysed")
    say(f"  local-linear win: {args.window_min:g} min sliding, tenure-clipped")
    say(f"  external CLK    : {clk_path}")
    say(f"  external SP3    : {sp3_path}")
    say(f"  datum changes   : {sum(w.datum_changed)} epoch(s); "
        f"R (retro) bumps: {sum(len(v) for v in w.retro.values())} on "
        f"{len(w.retro)} sat(s); PBGAUGE {len(events.get('PBGAUGE', []))}, "
        f"PHASE_ANCHOR {len(events.get('PHASE_ANCHOR', []))}, "
        f"PIVOT {len(events.get('PIVOT', []))}")
    say()

    result = {"run": run_name, "run_dir": run_dir, "n_epochs": n_ep, "dt_s": dt,
              "week": w.week, "day": daytag, "sats": sats,
              "settle_h": args.settle_h, "window_min": args.window_min,
              "elev_mask_deg": elev_mask, "station": station, "station_ecef": ecef,
              "contract": w.contract,
              "clk_file": clk_path, "sp3_file": sp3_path,
              "n_withheld_h": w.n_withheld_h}

    # ---------------- step 2: truth-free smoothness ----------------
    say("-" * 96)
    say("[2] TRUTH-FREE SMOOTHNESS — deviation from a local linear fit, "
        f"{args.window_min:g}-min sliding window")
    say(f"    (per-epoch {args.datum.upper()} over satellites removed = datum; "
        "tenure-clipped at PBGAUGE / PHASE_ANCHOR /")
    say("     retro bump / pivot / datum change / absence)")
    say("-" * 96)
    kinds = ["clk", "pb_L1C", "pb_L2W", "pb_MEAN", "pb_IF",
             "pc_L1C", "pc_L2W", "pc_MEAN", "pc_IF", "sum_L1C", "sum_IF"]
    chans = {}
    say(f"    {'channel':<10}{'nsat':>5}{'rms p50':>10}{'rms p90':>10}{'rms max':>10}"
        f"{'pooled':>10}{'|d| p50':>10}{'|d| p90':>10}{'|d| p99':>10}{'ACF<.5':>9}")
    say(f"    {'':<10}{'':>5}{'(m)':>10}{'(m)':>10}{'(m)':>10}{'(m)':>10}"
        f"{'(m)':>10}{'(m)':>10}{'(m)':>10}{'(min)':>9}")
    for kind in kinds:
        r = analyse_channel(w, sats, events, tows, tow_to_ep, kind, halfwin_s,
                            settle_ep, datum=args.datum)
        chans[kind] = r
        ac = r["acf_below_half_min"]
        say(f"    {kind:<10}{r['n_sat']:>5}{r['rms_p50_m']:>10.4f}{r['rms_p90_m']:>10.4f}"
            f"{r['rms_max_m']:>10.4f}{r['rms_pooled_m']:>10.4f}{r['abs_p50_m']:>10.4f}"
            f"{r['abs_p90_m']:>10.4f}{r['abs_p99_m']:>10.4f}"
            f"{(f'{ac:.1f}' if ac else '>60'):>9}")
    say()
    say("    |deviation| histogram, pc_IF (m):  "
        "<.01  <.02  <.05  <.1   <.2   <.5   <1    <2    >=2")
    say("                                      " +
        "  ".join(f"{v:5d}" for v in chans["pc_IF"]["hist_abs"]))
    say()
    ps = chans["pc_IF"]["per_sat"]
    top = sorted(ps.items(), key=lambda kv: -kv[1]["rms_m"])[:8]
    say("    pc_IF worst satellites:  " +
        "  ".join(f"{s}={d['rms_m']:.3f}" for s, d in top))
    say("    pc_IF best  satellites:  " +
        "  ".join(f"{s}={d['rms_m']:.3f}"
                  for s, d in sorted(ps.items(), key=lambda kv: kv[1]["rms_m"])[:5]))
    say()
    for kind in kinds:
        r = chans[kind]
        result.setdefault("channels", {})[kind] = {
            k: v for k, v in r.items()
            if k not in ("X", "D", "T", "P", "L", "epoch_mean",
                         "step_count_per_epoch")}

    # ---- [2b] unmarked within-tenure steps + wander decomposition -------------
    say("-" * 96)
    say("[2b] UNMARKED WITHIN-TENURE STEPS (invariant #7: an event with no edge)")
    say("     one-epoch |dX| INSIDE a tenure, i.e. NOT explained by PBGAUGE / "
        "PHASE_ANCHOR / retro bump /")
    say("     pivot / datum change / absence.  The one-epoch differences are "
        "re-centred on their own MEDIAN")
    say("     across satellites first, so a step COMMON to the constellation is "
        "invisible by construction and")
    say("     no level-datum choice can stamp one satellite's jump onto the others "
        "(it did: see --datum).")
    say("-" * 96)
    settled_sat_ep = int(np.isfinite(chans['pc_IF']['X'][:, settle_ep:]).sum())
    say(f"     settled population: {settled_sat_ep} sat-epochs")
    say(f"     {'channel':<10}{'>0.05 m':>10}{'>0.10 m':>10}{'>0.25 m':>10}"
        f"{'>0.50 m':>10}{'>1.00 m':>10}{'rate/sat/h(>0.25)':>20}")
    for kind in kinds:
        h = chans[kind]["step_hist"]
        rate = h[2] / max(1e-9, settled_sat_ep * dt / 3600.0)
        say(f"     {kind:<10}{h[0]:>10}{h[1]:>10}{h[2]:>10}{h[3]:>10}{h[4]:>10}"
            f"{rate:>20.3f}")
    say()
    top_ep = chans["pc_IF"]["step_epochs"]
    say("     epochs with the most SIMULTANEOUS cross-satellite pc_IF steps > 0.25 m:")
    for ep, ss in list(top_ep.items())[:6]:
        t = int(tows[ep])
        others = []
        for tier in ("T4_attrib", "PBGAUGE", "PHASE_ANCHOR", "RETRO", "T1_if_slip",
                     "ADMISSION", "GATE", "IONO_DW", "DATUMGRAPH", "PIVOT"):
            c = sum(1 for tt, _k, _e in events.get(tier, []) if tt == t)
            if c:
                others.append(f"{tier}x{c}")
        say(f"       tow {t}  ep {ep}  {len(ss)} sats ({','.join(sorted(ss)[:8])}"
            f"{'...' if len(ss) > 8 else ''})   events: "
            f"{', '.join(others) if others else 'NONE'}")
    say()
    # Which event tier co-occurs with the steps?  A per-epoch count-vs-count
    # correlation, so it does not depend on the per-satellite attribution being
    # right, only on "more of this tier this epoch => more steps this epoch".
    nstep = chans["pc_IF"]["step_count_per_epoch"]
    say("     which tier co-occurs?  corr( #events of tier at epoch , #pc_IF steps "
        "> 0.25 m at epoch ):")
    say(f"     {'tier':<16}{'events':>9}{'corr':>9}{'n_eff':>8}{'|z|':>7}")
    assoc = {}
    for tier in sorted(events, key=lambda k: -len(events[k])):
        if len(events[tier]) < 20 or tier in ("PRIORRES", "MDB", "BLOCKBAARDA",
                                              "CONGRUENCE", "TOPOLOGY", "REDUNDANCY"):
            continue
        cnt = np.zeros(n_ep)
        for t, _k, _e in events[tier]:
            i = tow_to_ep.get(t)
            if i is not None:
                cnt[i] += 1
        rr, _b, _nn = _pearson(cnt[settle_ep:], nstep[settle_ep:])
        ne = _neff(cnt[settle_ep:], nstep[settle_ep:])
        z = _fisher_z(rr, ne)
        assoc[tier] = {"n_events": len(events[tier]), "corr": rr, "n_eff": ne, "z": z}
        say(f"     {tier:<16}{len(events[tier]):>9}{rr:>+9.3f}{ne:>8.0f}{z:>7.1f}")
    result["step_event_association"] = assoc
    say(f"     epochs with >=1 pc_IF step: {int((nstep[settle_ep:] > 0).sum())}"
        f"/{n_ep - settle_ep}; mean {nstep[settle_ep:].mean():.2f}/epoch; "
        f"max {int(nstep[settle_ep:].max())}")
    say()
    say("     wander DECOMPOSITION — same statistic with every >0.50 m within-tenure "
        "step also cut:")
    say(f"     {'channel':<10}{'rms p50 all':>14}{'rms p50 step-cut':>19}"
        f"{'step share of var':>19}")
    for kind in ("clk", "pb_L1C", "pc_L2W", "pc_IF"):
        rc = analyse_channel(w, sats, events, tows, tow_to_ep, kind, halfwin_s,
                             settle_ep, step_cut_m=0.50, datum=args.datum)
        a, b = chans[kind]["rms_p50_m"], rc["rms_p50_m"]
        share = 1.0 - (b * b) / (a * a) if a > 0 else float("nan")
        result["channels"][kind]["rms_p50_stepcut_m"] = b
        result["channels"][kind]["step_var_share"] = share
        say(f"     {kind:<10}{a:>14.4f}{b:>19.4f}{share*100:>18.1f} %")
    say()

    # ---------------- step 3: external clock comparison ----------------
    say("-" * 96)
    say("[3] EXTERNAL (COD/IGS final) CLOCK COMPARISON — DIAGNOSTIC ONLY")
    say("-" * 96)
    ap_id = (w.contract.get("apriori_product_id") or "")
    ap_is_cod = "CLK" in ap_id.upper() and ("COD" in ap_id.upper() or "IGS" in ap_id.upper())
    if ap_is_cod:
        say("    NOTE — the a-priori the C records are a correction ABOUT is itself a")
        say(f"    final SP3+CLK ('{ap_id}').  The emitted C0 is therefore ALREADY")
        say("    (estimator clock - final clock), up to the per-constellation per-epoch")
        say("    scalar the CLK-R emit re-base subtracts (NetworkClockEmitRebase.cpp:")
        say("    `held = c0 - acc`; between-sat differences preserved exactly), which the")
        say("    per-epoch mean removal above kills.  So channel `clk` IS the")
        say("    truth-graded clock error e_s(t); no independent .CLK is needed for it.")
        say("    A genuinely INDEPENDENT check would need a different AC's final clock")
        say("    (e.g. IGS0OPSFIN); only COD is on disk, so that is NOT done here.")
    if clk_path and os.path.exists(clk_path):
        say(f"    parsing {os.path.basename(clk_path)} ...")
        clk = parse_clk(clk_path, set(sats))
        cov = sum(1 for s in sats if s in clk)
        say(f"    COD final clock: {cov}/{len(sats)} GPS satellites, "
            f"{sum(len(v) for v in clk.values())} AS records")
        floor = cod_floor_channel(clk, sats, tows, tow_to_ep, halfwin_s,
                                  settle_ep, events, w, n_ep, args.datum)
        result["cod_floor"] = floor
        say(f"    NOISE FLOOR of the identical statistic on the COD final clock itself:")
        say(f"      rms p50 {floor['rms_p50_m']:.4f} m   p90 {floor['rms_p90_m']:.4f} m   "
            f"pooled {floor['rms_pooled_m']:.4f} m   |d| p99 {floor['abs_p99_m']:.4f} m")
        f50 = floor["rms_p50_m"]
        for kind in ("clk", "pc_IF", "pb_L1C"):
            r = chans[kind]
            say(f"      {kind:<7} / floor  =  {r['rms_p50_m']/f50:7.1f} x   "
                f"({r['rms_p50_m']:.4f} m vs {f50:.4f} m)")
        # absolute-frame contrast of the COD clock, for scale
        say("    COD phase biases are DAILY constants (…_01D_01D_OSB.BIA), so every")
        say("    time-variation seen in the P channels above is wire-side by construction.")
    else:
        say("    NO external .CLK found on disk — IGS/COD comparison SKIPPED.")
        result["cod_floor"] = None
    say()

    # ---------------- step 4: geometry closure ----------------
    say("-" * 96)
    say("[4] GEOMETRY CLOSURE — does the wire's per-sat wander reproduce the consumer's?")
    say("-" * 96)
    cl_path = os.path.join(run_dir, "logs", "closed_loop_trace.csv")
    geo = {}
    if axis is not None:
        result["axis_enu"] = np.asarray(axis, float).tolist()
    if not (sp3_path and os.path.exists(sp3_path)):
        say("    NO SP3 on disk — geometry closure SKIPPED.")
    elif not os.path.exists(cl_path):
        say("    NO closed_loop_trace.csv — geometry closure SKIPPED.")
    elif ecef is None:
        say("    NO consumer ECEF (config unreadable and --station-ecef not given) — SKIPPED.")
    else:
        ct, cE, cN, cU, cns, cnf = parse_closed_loop(cl_path)
        say(f"    consumer trace: {len(ct)} epochs, n_sats(rows) med "
            f"{int(np.median(cns))}, total AR fixes {int(cnf.sum())}")
        # Measure the consumer's own settled error axis unless one was pinned.
        t0 = ct[0] + args.settle_h * 3600.0
        msk = ct >= t0
        Xs = np.vstack([cE[msk], cN[msk], cU[msk]])
        Xs = Xs - Xs.mean(axis=1, keepdims=True)
        ev, evec = np.linalg.eigh(Xs @ Xs.T / Xs.shape[1])
        kk = int(np.argmax(ev))
        meas = evec[:, kk] * (1.0 if evec[2, kk] >= 0 else -1.0)
        say(f"    consumer settled error: sd E/N/U = "
            f"{Xs[0].std():.3f}/{Xs[1].std():.3f}/{Xs[2].std():.3f} m, mean U "
            f"{cU[msk].mean():+.3f} m; PC1 {ev[kk]/ev.sum():.3f} of variance at "
            f"({meas[0]:+.3f},{meas[1]:+.3f},{meas[2]:+.3f}) = el "
            f"{math.degrees(math.asin(max(-1., min(1., meas[2])))):.1f} az "
            f"{math.degrees(math.atan2(meas[0], meas[1])) % 360.0:.1f}")
        if axis is None:
            axis = meas
        result["axis_enu"] = np.asarray(axis, float).tolist()
        result["consumer_pc1_frac"] = float(ev[kk] / ev.sum())
        result["consumer_sd_enu"] = [float(Xs[0].std()), float(Xs[1].std()),
                                     float(Xs[2].std())]
        sp3 = parse_sp3(sp3_path, set(sats))
        say(f"    SP3: {len(sp3)} GPS arcs, "
            f"{len(next(iter(sp3.values()))[0])} nodes, 9-pt Neville, "
            "Sagnac-rotated to the receive-time frame")
        E, EL, VIS = geometry(sp3, sats, tows, ecef, elev_mask)
        say(f"    visible sats above {elev_mask:.0f} deg at the consumer: "
            f"median {int(np.median(VIS[:, settle_ep:].sum(axis=0)))}")
        # Consumer-arc preparations: one constant float ambiguity per pass, so the
        # part of a per-satellite wire error that can reach position is the
        # pass-mean-removed part ('pass'); 'passlin' additionally removes a slow ramp,
        # which the consumer's own clock / ZWD can absorb.
        segs_by_sat = {}
        for si, sname in enumerate(sats):
            segs_by_sat[sname] = pass_segments(VIS[si], w.retro.get(sname, set()))
        npass = [len(v) for v in segs_by_sat.values()]
        say(f"    consumer passes: {sum(npass)} over {len(sats)} sats "
            f"(median {int(np.median(npass))}/sat; broken at loss of visibility and at "
            "the wire's own R discontinuity bumps)")
        for kind_ in list(chans.keys()):
            Xc = chans[kind_]["X"]
            Pm = np.full_like(Xc, np.nan)
            Pl = np.full_like(Xc, np.nan)
            for si, sname in enumerate(sats):
                sg = segs_by_sat[sname]
                Pm[si] = pass_demean(Xc[si], sg, order=0)
                Pl[si] = pass_demean(Xc[si], sg, order=1)
            chans[kind_]["P"] = post_center(Pm, args.datum)
            chans[kind_]["L"] = post_center(Pl, args.datum)
        # align the consumer trace onto the wire epoch grid
        cmap = {int(t): i for i, t in enumerate(ct)}
        idx = np.array([cmap.get(int(t), -1) for t in tows])
        okc = idx >= 0
        aE = np.where(okc, cE[np.clip(idx, 0, len(cE) - 1)], np.nan)
        aN = np.where(okc, cN[np.clip(idx, 0, len(cN) - 1)], np.nan)
        aU = np.where(okc, cU[np.clip(idx, 0, len(cU) - 1)], np.nan)
        act = np.vstack([aE, aN, aU]).T
        sl = slice(settle_ep, n_ep)
        uax = axis / np.linalg.norm(axis)
        pa = act[sl] @ uax
        sd_act_axis = _pstd(pa[np.isfinite(pa)])

        def run_one(Ddrv, wz=False, trim=0, label=""):
            Duse = Ddrv
            dropped = []
            if trim > 0:
                r = np.array([_rms(Ddrv[si, sl]) for si in range(len(sats))])
                order = np.argsort(-np.nan_to_num(r))[:trim]
                Duse = Ddrv.copy()
                Duse[order, :] = np.nan
                dropped = [sats[i] for i in order]
            dx, contrib, _ = closure(E, EL, VIS, Duse, sats, tows, settle_ep, axis,
                                     with_zwd=wz)
            g = {"label": label, "with_zwd": wz, "trim": trim, "dropped": dropped}
            for j, nm in enumerate(("E", "N", "U")):
                r, b, nn = _pearson(dx[sl, j], act[sl, j])
                g[f"corr_{nm}"] = r
                g[f"slope_{nm}"] = b
                g[f"sd_pred_{nm}"] = _pstd(dx[sl, j][np.isfinite(dx[sl, j])])
                g[f"sd_act_{nm}"] = _pstd(act[sl, j][np.isfinite(act[sl, j])])
            pp = dx[sl] @ uax
            r, b, _n = _pearson(pp, pa)
            g.update(corr_axis=r, slope_axis=b,
                     sd_pred_axis=_pstd(pp[np.isfinite(pp)]), sd_act_axis=sd_act_axis)
            Xp = dx[sl][np.isfinite(dx[sl]).all(axis=1)]
            if Xp.shape[0] > 60:
                Xc = Xp - Xp.mean(axis=0)
                ev, evec = np.linalg.eigh(Xc.T @ Xc / Xc.shape[0])
                k = int(np.argmax(ev))
                a2 = evec[:, k] * (1.0 if evec[2, k] >= 0 else -1.0)
                g.update(pred_pc1_frac=float(ev[k] / ev.sum()),
                         pred_axis=a2.tolist(),
                         pred_axis_el=math.degrees(math.asin(max(-1., min(1., a2[2])))),
                         pred_axis_az=math.degrees(math.atan2(a2[0], a2[1])) % 360.0,
                         cos_to_measured=float(abs(a2 @ uax)))
            return g, dx, contrib

        # ---- driver x preparation x trim matrix -------------------------------
        drivers = ["clk", "pc_L1C", "pc_L2W", "pc_MEAN", "pc_IF"]
        say("    delta = -dev(<driver>), G = [-e_ENU, 1], W = diag(sin^2 el).")
        say("    prep 'hp30'   = the step-2 local-linear residual (what a 30-min "
            "high-pass leaves)")
        say("    prep 'tenure' = within-PRODUCER-tenure mean removed ONLY")
        say("    prep 'pass'   = within-CONSUMER-pass mean removed (one constant float "
            "ambiguity per arc)")
        say("    prep 'passlin'= within-pass mean AND trend removed (clock/ZWD soak a ramp)")
        say("    corr is quoted with a Bartlett EFFECTIVE sample size (both series are "
            "autocorrelated at ~13 min); |z| is the Fisher-z two-sided normal score, so")
        say("    |z| < 2 is NOT a detection no matter how many epochs the nominal n is.")
        say()
        say(f"    {'driver':<9}{'prep':<9}{'trim':>5}{'corr E':>9}{'corr N':>9}"
            f"{'corr U':>9}{'corr AX':>9}{'n_eff':>7}{'|z|AX':>7}"
            f"{'sd_pred AX':>12}{'sd_act AX':>11}{'ratio':>8}")
        best = None
        PREPKEY = {"hp30": "D", "tenure": "T", "pass": "P", "passlin": "L"}
        for drv in drivers:
            for prep in ("hp30", "tenure", "pass", "passlin"):
                Ddrv = chans[drv][PREPKEY[prep]]
                for trim in (0, 2):
                    g, dx, contrib = run_one(Ddrv, trim=trim,
                                             label=f"{drv}/{prep}/trim{trim}")
                    pp = dx[sl] @ uax
                    g["n_eff_axis"] = _neff(pp, pa)
                    g["z_axis"] = _fisher_z(g["corr_axis"], g["n_eff_axis"])
                    geo[g["label"]] = g
                    say(f"    {drv:<9}{prep:<9}{trim:>5}{g['corr_E']:>+9.3f}"
                        f"{g['corr_N']:>+9.3f}{g['corr_U']:>+9.3f}{g['corr_axis']:>+9.3f}"
                        f"{g['n_eff_axis']:>7.0f}{g['z_axis']:>7.2f}"
                        f"{g['sd_pred_axis']:>12.3f}{g['sd_act_axis']:>11.3f}"
                        f"{g['sd_pred_axis']/g['sd_act_axis']:>8.2f}")
                    if best is None or (np.isfinite(g["z_axis"])
                                        and g["z_axis"] > best[0].get("z_axis", -1)):
                        best = (g, dx, contrib, drv, prep, trim)
        say()
        # ---- detail for the nominal arm (pc_IF / hp30 / no trim) --------------
        for drv, prep in (("pc_IF", "hp30"), (best[3], best[4])):
            tag = f"{drv}/{prep}/trim0"
            g = geo[tag]
            say(f"    --- detail, {tag} ---")
            for nm in ("E", "N", "U"):
                say(f"      {nm}: corr {g['corr_'+nm]:+.3f}  slope(act~pred) "
                    f"{g['slope_'+nm]:+.3f}  sd_pred {g['sd_pred_'+nm]:.3f} m  "
                    f"sd_act {g['sd_act_'+nm]:.3f} m  ratio "
                    f"{g['sd_pred_'+nm]/g['sd_act_'+nm]:.2f}")
            if "pred_axis" in g:
                a2 = g["pred_axis"]
                say(f"      predicted-error OWN axis: PC1 {g['pred_pc1_frac']:.3f} of var, "
                    f"({a2[0]:+.3f},{a2[1]:+.3f},{a2[2]:+.3f})  el {g['pred_axis_el']:.1f}  "
                    f"az {g['pred_axis_az']:.1f}   [measured: el "
                    f"{math.degrees(math.asin(uax[2])):.1f} "
                    f"az {math.degrees(math.atan2(uax[0], uax[1])) % 360.0:.1f}]")
                say(f"      |cos| predicted-PC1 vs measured axis: {g['cos_to_measured']:.3f}")
            Ddrv = chans[drv][PREPKEY[prep]]
            _g, _dx, contrib = run_one(Ddrv, trim=0, label=tag)
            rms_c = np.array([_rms(contrib[si, sl]) for si in range(len(sats))])
            order = np.argsort(-np.nan_to_num(rms_c))[:6]
            say("      per-satellite contribution to the AXIS projection "
                "(rms of w_s * delta_s):")
            cont = []
            for si in order:
                el_med = float(np.nanmedian(np.where(VIS[si, sl], EL[si, sl], np.nan))) \
                    if VIS[si, sl].any() else float("nan")
                cont.append({"sat": sats[si], "rms_contrib_m": float(rms_c[si]),
                             "median_elev_deg": el_med,
                             "visible_frac": float(VIS[si, sl].mean()),
                             "rms_delta_m": _rms(Ddrv[si, sl])})
                say(f"        {sats[si]}  contrib {rms_c[si]:8.4f} m   med elev "
                    f"{el_med:5.1f} deg   visible {VIS[si, sl].mean()*100:4.0f} %   "
                    f"own wander {_rms(Ddrv[si, sl]):8.4f} m")
            g["top_contributors"] = cont
            say()
        # ---- amplitude concentration ladder ----------------------------------
        lad = []
        for tr in (0, 1, 2, 3, 5):
            gg, _d, _c = run_one(chans["pc_IF"]["D"], trim=tr, label=f"ladder{tr}")
            lad.append((tr, gg["sd_pred_axis"]))
        say("    amplitude concentration — sd_pred on the AXIS as the k worst-wander "
            "satellites are dropped:")
        say("      " + "   ".join(f"trim{t}={v:.2f} m" for t, v in lad) +
            f"   (measured consumer: {sd_act_axis:.2f} m)")
        geo["amplitude_ladder"] = [{"trim": t, "sd_pred_axis": v} for t, v in lad]
        # ---- magnitude link (geometry-free) ----------------------------------
        # A weaker but far more robust test than the signed closure: does the consumer
        # wander more in the epochs where the wire is noisier?  It needs no sign, no
        # per-satellite projection fidelity, and no assumption about which combination
        # the consumer forms, only that a noisier wire is a worse wire.
        Dn = chans["pc_IF"]["D"]
        wire_amp = np.full(n_ep, np.nan)
        for ei in range(settle_ep, n_ep):
            m = VIS[:, ei] & np.isfinite(Dn[:, ei])
            if m.sum() >= 5:
                wire_amp[ei] = float(np.sqrt(np.mean(Dn[m, ei] ** 2)))
        aproj = np.full(n_ep, np.nan)
        aproj[sl] = np.abs(pa)
        r1, _b1, _n1 = _pearson(wire_amp, aproj)
        ne1 = _neff(wire_amp[sl], aproj[sl])
        r2, _b2, _n2 = _pearson(np.log(np.maximum(wire_amp, 1e-6)),
                                np.log(np.maximum(aproj, 1e-6)))
        say("    magnitude link (geometry-free): corr( rms_visible|delta| , "
            f"|consumer error on the axis| ) = {r1:+.3f} "
            f"(log-log {r2:+.3f}), n_eff {ne1:.0f}, |z| {_fisher_z(r1, ne1):.2f}")
        geo["magnitude_link"] = {"corr": r1, "corr_loglog": r2, "n_eff": ne1,
                                 "z": _fisher_z(r1, ne1)}
        # ---- ZWD-column variant on the nominal arm ---------------------------
        gz, _dxz, _cz = run_one(chans["pc_IF"]["D"], wz=True, trim=0,
                                label="pc_IF/hp30/trim0/+mw")
        geo[gz["label"]] = gz
        say(f"    ZWD column added (G=[-e,1,1/sin el]), pc_IF/hp30: "
            f"corr AX {gz['corr_axis']:+.3f}, sd_pred AX {gz['sd_pred_axis']:.3f} m "
            f"(vs {geo['pc_IF/hp30/trim0']['sd_pred_axis']:.3f} m without)")
        # ---- control: same per-sat rms, white in time ------------------------
        rng = np.random.default_rng(20260910)
        Dn = chans["pc_IF"]["D"]
        Dw_ = np.where(np.isfinite(Dn), rng.standard_normal(Dn.shape) *
                       np.nanstd(Dn, axis=1, keepdims=True), np.nan)
        gw, _dxw, _cw = run_one(Dw_, label="white_control")
        geo["white_control"] = gw
        say(f"    control — same per-sat rms but WHITE in time: axis sd_pred "
            f"{gw['sd_pred_axis']:.3f} m, corr {gw['corr_axis']:+.3f}  "
            f"(real wander: {geo['pc_IF/hp30/trim0']['sd_pred_axis']:.3f} m, "
            f"{geo['pc_IF/hp30/trim0']['corr_axis']:+.3f})")
    result["geometry"] = {k: v for k, v in geo.items()}
    say()

    # ---------------- step 5: event overlay ----------------
    say("-" * 96)
    say("[5] EVENT OVERLAY — |deviation| in the 5 min after an event vs elsewhere")
    say("-" * 96)
    D = chans["pc_IF"]["D"]
    sat_idx = {s: i for i, s in enumerate(sats)}
    horizon_ep = int(round(300.0 / dt))
    base_mask = np.zeros_like(D, dtype=bool)
    base_mask[:, settle_ep:] = True
    ov = {}
    any_mask = np.zeros_like(D, dtype=bool)
    say(f"    {'tier':<15}{'events':>8}{'sat-ep':>9}{'rms after':>12}{'rms elsewhere':>15}{'ratio':>8}")
    for tier in OVERLAY_TIERS:
        evs = events.get(tier, [])
        if not evs:
            continue
        m = np.zeros_like(D, dtype=bool)
        n_used = 0
        for t, key, _e in evs:
            ep = tow_to_ep.get(t)
            if ep is None:
                continue
            s = key.rsplit("_", 1)[0] if ("_" in key and key.rsplit("_", 1)[0] in sat_idx) else key
            lo, hi = ep, min(n_ep, ep + horizon_ep + 1)
            if s in sat_idx:
                m[sat_idx[s], lo:hi] = True
                n_used += 1
            elif not s:
                m[:, lo:hi] = True
                n_used += 1
        m &= base_mask
        any_mask |= m
        inn = _rms(D[m])
        outv = _rms(D[base_mask & ~m])
        ov[tier] = {"n_events": len(evs), "n_used": n_used, "n_sat_ep": int(m.sum()),
                    "rms_after_m": inn, "rms_elsewhere_m": outv,
                    "ratio": (inn / outv) if outv else float("nan")}
        say(f"    {tier:<15}{len(evs):>8}{int(m.sum()):>9}{inn:>12.4f}{outv:>15.4f}"
            f"{(inn/outv if outv else float('nan')):>8.2f}")
    if any_mask.any():
        inn, outv = _rms(D[any_mask & base_mask]), _rms(D[base_mask & ~any_mask])
        ov["ANY"] = {"n_sat_ep": int((any_mask & base_mask).sum()),
                     "rms_after_m": inn, "rms_elsewhere_m": outv,
                     "ratio": (inn / outv) if outv else float("nan")}
        say(f"    {'ANY':<15}{'':>8}{int((any_mask & base_mask).sum()):>9}{inn:>12.4f}"
            f"{outv:>15.4f}{(inn/outv if outv else float('nan')):>8.2f}")
    result["event_overlay"] = ov
    say()

    with open(txt_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, default=float)
    print(f"wrote {txt_path}")
    print(f"wrote {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
