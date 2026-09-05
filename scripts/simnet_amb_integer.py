#!/usr/bin/env python3
"""SYNTHETIC MINIMUM NETWORK — the ambiguity-INTEGER grader
(docs/SPEC_synthetic_minimum_network.md §13.6 (method rules §13.8); SPEC_synthetic_minimum_network §13).

The first absolute integer test the network has ever had.  The field can only
measure the FRACTIONAL part of a DD ambiguity (med|frac| 0.248 says "no integer
structure" but cannot say why); on the sim fixture the true integers are known
per (station, satellite, signal), so the graded quantity is

    e = dd_cyc_est − DD_true          (cycles, FULL value — not the fractional part)

with  DD_true = [N(a,i) − N(a,ref)] − [N(b,i) − N(b,ref)]  (+ cumulative
truth_slips deltas up to the epoch), formed in PYTHON INTS before any float
touches it — the integers are km-scale by construction (N ∈ [1e5, 1e6): the sim
draws them unguessable for the MW/NL exercise), so float differencing would
grade cancellation error, not the estimator.

Lane combinations are formed from matched per-band DDs on the SAME
(sat, ref, sta_a, sta_b) quadruple in cycle-difference form:

    e_WL(1,2)  = e_L1C − e_L2W        N_WL  = N1 − N2  (integer by construction)
    e_EWL(2,5) = e_L2W − e_L5Q        N_EWL = N2 − N5  (integer by construction)

Iono sensitivity per metre of L1 DD-iono error in these units: L1 5.26 cyc,
WL 1.49 cyc, EWL 0.29 cyc — an 18× ladder, which is the cascade the third band
opens.  The pre-registered expected shape (handoff §3.3): EWL near-integer, WL
plausible, NL not — the value is WHICH RUNG BREAKS.

Reads:  <run>/logs/network_dd_fractional.csv   (trace_dd_fractional, stride 1)
        <fixture>/<station>/truth_ambiguities.csv   (satellite,signal,N_int,wavelength_m)
        <fixture>/<station>/truth_slips.csv         (week,tow_s,satellite,signal,delta_cycles,…)
        optional (--iono-join):
        <run>/logs/network_ionosphere.csv           (…,iono_total_m = x_lin + x_r, L1 m, delay-positive)
        <fixture>/<station>/truth_atmo.csv          (…,I_L1_m,…)
        optional (--sigma-ratio):
        <run>/logs/network_run_manifest.json        (network.phase_leveling_warmup_epochs)

The trace is STREAMED (csv.reader, one pass after a cheap epoch pre-scan); the
only per-row state kept is the per-band error lists the grade needs, plus a
compact per-chord run buffer when --by-age is on.

--------------------------------------------------------------------------
MECHANISM SIGNATURES (round-2 tooling, 2026-09-04) — all default-OFF flags, the
default output is unchanged byte-for-byte (docs quote it):

  --sigma-ratio   per band, median formal dd_sigma_cyc at emitted-epoch index
                  k = 0..12, 20, 60, 120, last (k is 0-BASED: k=0 is the first
                  emitted epoch, the pure single-epoch solve), the FIRST-INJECT
                  ratio sigma(k=w+1)/sigma(k=w) with w = phase_leveling_warmup_epochs
                  (manifest, else --warmup), and median |e|/sigma over the LAST
                  window.  A correct recursion gives 1/sqrt(2) = 0.707 at the
                  first inject; B same-LOS ambiguity rows re-injected as
                  INDEPENDENT priors give 1/sqrt(B+1): 0.577 (B=2), 0.500 (B=3).
  --by-age        median |e| by chord age (epochs since the chord's run start;
                  a run = consecutive epochs on which the (signal,sat,ref,sta_a,
                  sta_b) key appears; a gap > 1 epoch starts a new run; age 0 =
                  birth, excluded), bins [1,10) [10,30) [30,100) [100,300)
                  [300,1000), per band + lanes, with the log-log slope over the
                  geometric bin centres on runs >= --by-age-minlen (the
                  time-structure lens's pass2 (iv) definition; -0.5 = 1/sqrt(K)
                  accumulation, ~0 = a frozen level).
  --iono-join     the DD identity  e_b = mu_b * DD(dI) / lambda_b  with
                  dI = iono_total_m − I_L1_m per LOS-epoch (mu_b = (lambda_b /
                  lambda_L1)^2, phase = rho − mu_b I + lambda_b N): residual
                  e − pred (median, med|.|, p90|.|, round0 %) and the slope /
                  intercept / r² of e on pred, per band, all epochs and LAST
                  window; sampled every --stride epochs (default 4).
  --gal-lanes     Galileo lanes EWL(5,7) = e_L5Q − e_L7Q and WL(1,5) = e_L1C −
                  e_L5Q on Galileo quadruples (sat_i starts with 'E'); printed
                  only when Galileo rows exist.
  --check-lambda  asserts per signal that the trace's lambda_m equals
                  truth_ambiguities.csv wavelength_m to 1e-6 (exit 2 on FAIL),
                  and prints the Gaussian-consistency check (sigma from
                  med|e|/0.6745 vs sigma from the round(e)=0 share) per band and
                  lane — the check the theory lens used to validate the truth
                  join.  NOTE: the frac_cyc cross-check in the main loop is a
                  TAUTOLOGY — e − round(e) == frac_cyc for ANY integer DD_true,
                  so '0 dropped' proves parsing, not the join; the join is
                  validated by the wavelength assertion and the Gaussian
                  consistency (no integer-offset population).
--------------------------------------------------------------------------

Usage:
    python scripts/simnet_amb_integer.py runs/simnet60_tri --run runs/simnet60_tri_slant_1h_ddint/run
    python scripts/simnet_amb_integer.py runs/simnet60_quiet_wu93_24h --run runs/camp60wu_quiet24h_sticky \
        --sigma-ratio --by-age --iono-join --gal-lanes --check-lambda --stride 4
    python scripts/simnet_amb_integer.py --selftest
"""

import argparse
import bisect
import csv
import json
import math
import os
import sys
from array import array
from collections import defaultdict

C_LIGHT = 299792458.0
F_L1 = 1575.42e6
LAM_L1 = C_LIGHT / F_L1                       # the iono state's unit wavelength (L1 metres)

AGE_BINS = [(1, 10), (10, 30), (30, 100), (100, 300), (300, 1000)]
AGE_EDGES = [lo for lo, _ in AGE_BINS] + [AGE_BINS[-1][1]]
SIGMA_K_FIXED = list(range(0, 13)) + [20, 60, 120]
GAL_LANES = {"EWL(5,7)": ("L5Q", "L7Q"), "WL(1,5)": ("L1C", "L5Q")}


def load_truth(fixture_dir, stations, wavelengths=None):
    """Per-station truth: N[(sta, sat, sig)] -> int, and per-key sorted slip
    lists [(tow, cum_delta_after)] for cumulative lookup.

    `wavelengths` (optional dict) is filled with sig -> set(float wavelength_m)
    from the truth file, for --check-lambda."""
    n_int = {}
    slips = defaultdict(list)          # (sta, sat, sig) -> [(tow, delta_int), ...]
    for sta in sorted(stations):
        amb_path = os.path.join(fixture_dir, sta, "truth_ambiguities.csv")
        if not os.path.isfile(amb_path):
            raise SystemExit(f"missing truth file: {amb_path}")
        with open(amb_path, newline="") as f:
            for row in csv.DictReader(f):
                n_int[(sta, row["satellite"], row["signal"])] = int(row["N_int"])
                if wavelengths is not None and "wavelength_m" in row:
                    wavelengths.setdefault(row["signal"], set()).add(float(row["wavelength_m"]))
        slip_path = os.path.join(fixture_dir, sta, "truth_slips.csv")
        if os.path.isfile(slip_path):
            with open(slip_path, newline="") as f:
                for row in csv.DictReader(f):
                    slips[(sta, row["satellite"], row["signal"])].append(
                        (float(row["tow_s"]), int(round(float(row["delta_cycles"])))))
    for k in slips:
        slips[k].sort()
    return n_int, slips


def n_at(n_int, slips, sta, sat, sig, tow):
    """True integer at epoch `tow`: pre-slip N_int + Σ deltas with tow_s <= tow.
    Returns None if the (sta, sat, sig) has no truth row (should not happen)."""
    base = n_int.get((sta, sat, sig))
    if base is None:
        return None
    ss = slips.get((sta, sat, sig))
    if ss:
        tows = [t for t, _ in ss]
        k = bisect.bisect_right(tows, tow)
        base += sum(d for _, d in ss[:k])
    return base


def stats(errs):
    """(n, median e, median |e|, p90 |e|, %|e|<=0.10, %|e|<=0.25, %round(e)==0)"""
    if not errs:
        return None
    s = sorted(errs)
    a = sorted(abs(e) for e in errs)
    n = len(errs)

    def q(v, p):
        i = min(n - 1, max(0, int(round(p * (n - 1)))))
        return v[i]

    within = lambda t: 100.0 * sum(1 for e in a if e <= t) / n
    int0 = 100.0 * sum(1 for e in errs if round(e) == 0) / n
    return (n, q(s, 0.5), q(a, 0.5), q(a, 0.9), within(0.10), within(0.25), int0)


def fmt_row(label, st):
    if st is None:
        return f"  {label:<12} (no rows)"
    n, med, meda, p90, w10, w25, i0 = st
    return (f"  {label:<12} n={n:<7d} med(e)={med:+8.3f}  med|e|={meda:7.3f}  "
            f"p90|e|={p90:8.3f}  <=0.10cyc {w10:5.1f}%  <=0.25cyc {w25:5.1f}%  "
            f"round(e)=0 {i0:5.1f}%")


# --------------------------------------------------------------------------
# small numeric helpers (pure python — the grader has no numpy dependency)
# --------------------------------------------------------------------------
def pct(v, p):
    """Percentile with the same index rule as stats(): v may be any iterable."""
    s = sorted(v)
    n = len(s)
    if n == 0:
        return None
    i = min(n - 1, max(0, int(round(p * (n - 1)))))
    return s[i]


def gaussian_sigmas(med_abs, round0_pct):
    """The theory lens's consistency test: for a zero-mean Gaussian e ~ N(0, s²)
    with NO integer-offset population, med|e| = 0.6745 s and
    P(round(e)=0) = P(|e| < 0.5) = erf(0.5 / (s sqrt2)).  Returns
    (s_from_med, s_from_round0 or None)."""
    s_med = med_abs / 0.6745 if med_abs is not None else None
    p = round0_pct / 100.0
    if p <= 0.0005 or p >= 0.9995:
        return s_med, None
    lo, hi = 1e-4, 1e4
    for _ in range(100):
        mid = 0.5 * (lo + hi)
        if math.erf(0.5 / (mid * math.sqrt(2.0))) > p:
            lo = mid
        else:
            hi = mid
    return s_med, 0.5 * (lo + hi)


def linfit(xs, ys):
    """Least-squares y = slope*x + icpt; returns (slope, icpt, r2) or None."""
    n = len(xs)
    if n < 3:
        return None
    sx = math.fsum(xs)
    sy = math.fsum(ys)
    sxx = math.fsum(x * x for x in xs)
    sxy = math.fsum(x * y for x, y in zip(xs, ys))
    syy = math.fsum(y * y for y in ys)
    den = n * sxx - sx * sx
    ss_tot = syy - sy * sy / n
    if den <= 0 or ss_tot <= 0:
        return None
    slope = (n * sxy - sx * sy) / den
    icpt = (sy - slope * sx) / n
    ss_res = math.fsum((y - slope * x - icpt) ** 2 for x, y in zip(xs, ys))
    return slope, icpt, 1.0 - ss_res / ss_tot


def loglog_slope(xs, ys):
    pts = [(math.log(x), math.log(y)) for x, y in zip(xs, ys) if x > 0 and y > 0]
    if len(pts) < 3:
        return None
    fit = linfit([p[0] for p in pts], [p[1] for p in pts])
    return fit[0] if fit else None


def read_manifest_warmup(run_dir):
    """network.phase_leveling_warmup_epochs from <run>/logs/network_run_manifest.json
    (JSON-lines; the 'startup' record carries the config snapshot).  None if absent."""
    path = os.path.join(run_dir, "logs", "network_run_manifest.json")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                net = rec.get("network") if isinstance(rec, dict) else None
                if isinstance(net, dict) and "phase_leveling_warmup_epochs" in net:
                    return int(net["phase_leveling_warmup_epochs"])
    except OSError:
        return None
    return None


def prescan_epochs(dd_path):
    """Pass 0 (no csv parse): the distinct `tow` strings of the DD trace in
    first-appearance order.  Cheap — the tow field repeats within an epoch, so
    only a string compare per line."""
    tows = []
    seen = set()
    prev = None
    n_lines = 0
    with open(dd_path, newline="") as f:
        header = f.readline()
        if not header:
            return tows, 0
        for line in f:
            n_lines += 1
            i = line.find(",")
            j = line.find(",", i + 1)
            t = line[i + 1:j] if j > 0 else line[i + 1:].rstrip("\r\n")
            if t != prev:
                prev = t
                if t not in seen:
                    seen.add(t)
                    tows.append(t)
    return tows, n_lines


class AgeTracker:
    """|e| by chord age.  A run is a maximal set of CONSECUTIVE epoch indices on
    which a key appears; age = epochs since the run start (0 = birth).  Only the
    CURRENT run's values are buffered per key (compact); runs are closed when
    the key skips an epoch, on the periodic sweep, or at finish()."""

    def __init__(self, minlen):
        self.minlen = minlen
        self.state = {}                                   # key -> [last_k, array('d')]
        self.bins_all = [array("d") for _ in AGE_BINS]
        self.bins_long = [array("d") for _ in AGE_BINS]
        self.n_runs = 0
        self.n_runs_long = 0

    @staticmethod
    def _bin(age):
        b = bisect.bisect_right(AGE_EDGES, age) - 1
        return b if 0 <= b < len(AGE_BINS) else -1

    def _close(self, run):
        self.n_runs += 1
        if len(run) >= self.minlen:
            self.n_runs_long += 1
            for age in range(1, len(run)):
                b = self._bin(age)
                if b >= 0:
                    self.bins_long[b].append(run[age])

    def add(self, key, k, v):
        st = self.state.get(key)
        if st is None or st[0] != k - 1:
            if st is not None:
                self._close(st[1])
            self.state[key] = [k, array("d", (v,))]
            return
        st[0] = k
        age = len(st[1])
        st[1].append(v)
        b = self._bin(age)
        if b >= 0:
            self.bins_all[b].append(v)

    def sweep(self, k):
        """Close every run that cannot continue at epoch k (last_k < k-1)."""
        dead = [key for key, st in self.state.items() if st[0] < k - 1]
        for key in dead:
            self._close(self.state.pop(key)[1])

    def finish(self):
        for st in self.state.values():
            self._close(st[1])
        self.state.clear()

    def report(self):
        """{'all': [(med, n)...], 'long': [(med, n)...], 'slope_all', 'slope_long', 'n_runs', 'n_runs_long'}"""
        out = {"n_runs": self.n_runs, "n_runs_long": self.n_runs_long}
        for name, bins in (("all", self.bins_all), ("long", self.bins_long)):
            rows = []
            xs, ys = [], []
            for (lo, hi), b in zip(AGE_BINS, bins):
                if len(b):
                    m = pct(b, 0.5)
                    rows.append((m, len(b)))
                    xs.append(math.sqrt(lo * min(hi, 3000)))
                    ys.append(m)
                else:
                    rows.append((None, 0))
            out[name] = rows
            out["slope_" + name] = loglog_slope(xs, ys)
        return out


def fmt_age_row(label, rows, slope):
    cells = []
    for m, n in rows:
        cells.append("      -    (      0)" if m is None else f"{m:7.3f} ({n:8d})")
    s = "    n/a" if slope is None else f"{slope:+7.3f}"
    return f"  {label:<10}" + " ".join(cells) + f"   slope {s}"


def load_producer_iono(path, keep_towint):
    """(station, sat, tow_int) -> iono_total_m at the kept epochs (streamed)."""
    prod = {}
    intern = sys.intern
    with open(path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd, None)
        if hdr is None:
            raise SystemExit(f"empty {path}")
        ci = {n: i for i, n in enumerate(hdr)}
        for need in ("tow", "station", "sat", "iono_total_m"):
            if need not in ci:
                raise SystemExit(f"{path}: missing column {need}")
        i_tow, i_sta, i_sat, i_tot = ci["tow"], ci["station"], ci["sat"], ci["iono_total_m"]
        prev = None
        keep = False
        ti = 0
        for r in rd:
            ts = r[i_tow]
            if ts != prev:
                prev = ts
                ti = int(round(float(ts)))
                keep = ti in keep_towint
            if not keep:
                continue
            prod[(intern(r[i_sta]), intern(r[i_sat]), ti)] = float(r[i_tot])
    return prod


def load_truth_atmo(path, sta, prod, d_iono, keep_towint):
    """Fill d_iono[(sta, sat, tow_int)] = iono_total_m − I_L1_m for the kept
    epochs where the producer has the LOS.  Returns rows matched."""
    n = 0
    intern = sys.intern
    with open(path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd, None)
        if hdr is None:
            return 0
        ci = {c: i for i, c in enumerate(hdr)}
        for need in ("tow_s", "satellite", "I_L1_m"):
            if need not in ci:
                raise SystemExit(f"{path}: missing column {need}")
        i_tow, i_sat, i_I = ci["tow_s"], ci["satellite"], ci["I_L1_m"]
        prev = None
        keep = False
        ti = 0
        for r in rd:
            ts = r[i_tow]
            if ts != prev:
                prev = ts
                ti = int(round(float(ts)))
                keep = ti in keep_towint
            if not keep:
                continue
            key = (sta, intern(r[i_sat]), ti)
            v = prod.get(key)
            if v is not None:
                d_iono[key] = v - float(r[i_I])
                n += 1
    return n


def identity_block(e, p):
    """Residual + fit statistics for the DD identity on paired lists."""
    if len(e) < 3:
        return None
    r = [a - b for a, b in zip(e, p)]
    ra = [abs(x) for x in r]
    fit = linfit(p, e)
    return {
        "n": len(e),
        "med": pct(r, 0.5), "med_abs": pct(ra, 0.5), "p90_abs": pct(ra, 0.9),
        "round0": 100.0 * sum(1 for x in r if round(x) == 0) / len(r),
        "slope": fit[0] if fit else None, "icpt": fit[1] if fit else None, "r2": fit[2] if fit else None,
        "med_abs_e": pct((abs(x) for x in e), 0.5), "med_abs_pred": pct((abs(x) for x in p), 0.5),
    }


def fmt_identity(label, b):
    if b is None:
        return f"  {label:<16} (no rows)"
    sl = "   n/a " if b["slope"] is None else f"{b['slope']:+7.4f}"
    ic = "  n/a " if b["icpt"] is None else f"{b['icpt']:+6.3f}"
    r2 = " n/a " if b["r2"] is None else f"{b['r2']:5.3f}"
    return (f"  {label:<16} n={b['n']:<7d} slope={sl} icpt={ic} r2={r2}  "
            f"resid: med={b['med']:+7.3f} med|.|={b['med_abs']:6.3f} p90|.|={b['p90_abs']:7.3f} "
            f"round0 {b['round0']:5.1f}%   (med|e| {b['med_abs_e']:.3f} med|pred| {b['med_abs_pred']:.3f})")


# --------------------------------------------------------------------------
# the grade
# --------------------------------------------------------------------------
def grade(args, out=None):
    """Run the grade described by `args` (an argparse Namespace); prints the
    report to `out` (default stdout) and returns a results dict (selftest)."""
    if out is None:
        out = sys.stdout

    def P(*a):
        print(*a, file=out)

    dd_path = os.path.join(args.run, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(dd_path):
        raise SystemExit(f"missing {dd_path} — arm network.trace_dd_fractional")

    want_sigma = bool(getattr(args, "sigma_ratio", False))
    want_age = bool(getattr(args, "by_age", False))
    want_iono = bool(getattr(args, "iono_join", False))
    want_gal = bool(getattr(args, "gal_lanes", False))
    want_lam = bool(getattr(args, "check_lambda", False))
    stride = max(1, int(getattr(args, "stride", 4) or 4))
    minlen = max(2, int(getattr(args, "by_age_minlen", 200) or 200))

    # --- pass 0: epochs -------------------------------------------------------
    tow_strs, n_lines = prescan_epochs(dd_path)
    if n_lines == 0:
        raise SystemExit("DD trace is empty")
    tows = sorted({float(t) for t in tow_strs})
    t_last = tows[int(len(tows) * (1.0 - args.last_frac))] if len(tows) > 1 else tows[0]
    k_of_float = {t: i for i, t in enumerate(tows)}
    k_of = {s: k_of_float[float(s)] for s in tow_strs}        # tow string -> 0-based epoch index
    k_last = len(tows) - 1

    # --- optional inputs ------------------------------------------------------
    warm_w, warm_src = None, None
    if want_sigma:
        warm_w = read_manifest_warmup(args.run)
        warm_src = "manifest"
        if warm_w is None:
            warm_w = int(getattr(args, "warmup", 6))
            warm_src = "--warmup"
    sigma_ks = set(SIGMA_K_FIXED + [warm_w, warm_w + 1, k_last]) if want_sigma else set()
    sigma_ks = {k for k in sigma_ks if k is not None and 0 <= k <= k_last}

    prod_iono, d_iono, join_towint = None, {}, set()
    if want_iono:
        iono_path = os.path.join(args.run, "logs", "network_ionosphere.csv")
        if not os.path.isfile(iono_path):
            raise SystemExit(f"missing {iono_path} — arm network.trace_ionosphere")
        join_towint = {int(round(tows[k])) for k in range(0, len(tows), stride)}
        prod_iono = load_producer_iono(iono_path, join_towint)

    # --- truth (lazy per station, same SystemExit semantics as before) --------
    n_int, slips = {}, {}
    wl_truth = {} if want_lam else None
    truth_loaded = set()
    iono_truth_rows = 0

    def ensure_truth(sta):
        nonlocal iono_truth_rows
        if sta in truth_loaded:
            return
        truth_loaded.add(sta)
        ni, sl = load_truth(args.fixture_dir, [sta], wl_truth)
        n_int.update(ni)
        slips.update(sl)
        if want_iono:
            atmo = os.path.join(args.fixture_dir, sta, "truth_atmo.csv")
            if not os.path.isfile(atmo):
                raise SystemExit(f"missing truth file: {atmo}")
            iono_truth_rows += load_truth_atmo(atmo, sys.intern(sta), prod_iono, d_iono, join_towint)

    # --- accumulators ---------------------------------------------------------
    band_all = defaultdict(list)
    band_last = defaultdict(list)
    lanes = {"EWL(2,5)": ("L2W", "L5Q"), "WL(1,2)": ("L1C", "L2W")}
    lane_all = defaultdict(list)
    lane_last = defaultdict(list)
    gal_all = defaultdict(list)
    gal_last = defaultdict(list)
    sig_at_k = defaultdict(lambda: defaultdict(list))      # k -> sig -> [sigma]
    abse_at_k = defaultdict(lambda: defaultdict(list))     # k -> sig -> [|e|]
    ratio_last = defaultdict(list)                         # sig -> [|e|/sigma] (last window)
    age_band = defaultdict(lambda: AgeTracker(minlen)) if want_age else None
    age_lane = defaultdict(lambda: AgeTracker(minlen)) if want_age else None
    id_e = defaultdict(list)                               # sig -> e   (iono join, kept epochs)
    id_p = defaultdict(list)                               # sig -> pred
    id_last = defaultdict(list)                            # sig -> is_last flag
    n_id_nodI = 0
    lam_seen = defaultdict(set)                            # sig -> {lambda strings}

    n_rows = 0
    n_unmatched = 0
    n_tow_revisit = 0
    flushed = set()
    qbuf = {}                                              # (sat, ref, sa, sb) -> {sig: e}

    def flush_epoch(k, is_last):
        for (sat, ref, sa, sb), sigs in qbuf.items():
            for lane, (s1, s2) in lanes.items():
                if s1 in sigs and s2 in sigs:
                    e = sigs[s1] - sigs[s2]
                    lane_all[lane].append(e)
                    if is_last:
                        lane_last[lane].append(e)
                    if want_age:
                        age_lane[lane].add((sat, ref, sa, sb), k, abs(e))
            if want_gal and sat[:1] == "E":
                for lane, (s1, s2) in GAL_LANES.items():
                    if s1 in sigs and s2 in sigs:
                        e = sigs[s1] - sigs[s2]
                        gal_all[lane].append(e)
                        if is_last:
                            gal_last[lane].append(e)
                        if want_age:
                            age_lane[lane].add((sat, ref, sa, sb), k, abs(e))
        qbuf.clear()

    # --- pass 1: the stream ---------------------------------------------------
    with open(dd_path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        ci = {n: i for i, n in enumerate(hdr)}
        for need in ("tow", "signal", "sat_i", "ref_sat", "sta_a", "sta_b", "dd_cyc", "frac_cyc"):
            if need not in ci:
                raise SystemExit(f"{dd_path}: missing column {need}")
        i_tow, i_sig, i_sat, i_ref = ci["tow"], ci["signal"], ci["sat_i"], ci["ref_sat"]
        i_a, i_b, i_ddc, i_frac = ci["sta_a"], ci["sta_b"], ci["dd_cyc"], ci["frac_cyc"]
        i_sg = ci.get("dd_sigma_cyc")
        i_lam = ci.get("lambda_m")
        if want_sigma and i_sg is None:
            raise SystemExit(f"{dd_path}: --sigma-ratio needs column dd_sigma_cyc")
        if (want_iono or want_lam) and i_lam is None:
            raise SystemExit(f"{dd_path}: --iono-join / --check-lambda need column lambda_m")

        cur_tow_s = None
        cur_k = -1
        tow = 0.0
        is_last = False
        k_sel = False
        k_join = False
        for r in rd:
            n_rows += 1
            tow_s = r[i_tow]
            if tow_s != cur_tow_s:
                if cur_tow_s is not None:
                    flush_epoch(cur_k, is_last)
                    flushed.add(cur_tow_s)
                if tow_s in flushed:
                    n_tow_revisit += 1
                cur_tow_s = tow_s
                tow = float(tow_s)
                cur_k = k_of.get(tow_s)
                if cur_k is None:                       # quoted field: prescan saw the raw text
                    cur_k = k_of_float[tow]
                is_last = tow >= t_last
                k_sel = cur_k in sigma_ks
                k_join = want_iono and (cur_k % stride == 0)
                if want_age and cur_k % 100 == 0:
                    for tr in age_band.values():
                        tr.sweep(cur_k)
                    for tr in age_lane.values():
                        tr.sweep(cur_k)
            sat, ref = r[i_sat], r[i_ref]
            sa, sb = r[i_a], r[i_b]
            sig = r[i_sig]
            if sa not in truth_loaded:
                ensure_truth(sa)
            if sb not in truth_loaded:
                ensure_truth(sb)
            legs = [n_at(n_int, slips, sa, sat, sig, tow),
                    n_at(n_int, slips, sa, ref, sig, tow),
                    n_at(n_int, slips, sb, sat, sig, tow),
                    n_at(n_int, slips, sb, ref, sig, tow)]
            if any(v is None for v in legs):
                n_unmatched += 1
                continue
            dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])     # python ints
            e = float(r[i_ddc]) - dd_true
            # sanity: our e and the trace's own frac agree modulo integers
            # (a TAUTOLOGY for the truth join — see the docstring; kept as the
            # parse guard it actually is)
            frac_trace = float(r[i_frac])
            if abs((e - round(e)) - frac_trace) > 2e-3 and abs(abs(e - round(e)) - abs(frac_trace)) > 2e-3:
                n_unmatched += 1
                continue
            band_all[sig].append(e)
            if is_last:
                band_last[sig].append(e)
            q = qbuf.get((sat, ref, sa, sb))
            if q is None:
                q = qbuf[(sat, ref, sa, sb)] = {}
            q[sig] = e

            if want_lam:
                lam_seen[sig].add(r[i_lam])
            if want_sigma:
                sg = float(r[i_sg])
                if k_sel:
                    abse_at_k[cur_k][sig].append(abs(e))
                    if sg > 0:
                        sig_at_k[cur_k][sig].append(sg)
                if is_last and sg > 0:
                    ratio_last[sig].append(abs(e) / sg)
            if want_age:
                age_band[sig].add((sat, ref, sa, sb), cur_k, abs(e))
            if k_join:
                ti = int(round(tow))
                d0 = d_iono.get((sa, sat, ti))
                d1 = d_iono.get((sa, ref, ti))
                d2 = d_iono.get((sb, sat, ti))
                d3 = d_iono.get((sb, ref, ti))
                if d0 is None or d1 is None or d2 is None or d3 is None:
                    n_id_nodI += 1
                else:
                    lam = float(r[i_lam])
                    pred = lam / (LAM_L1 * LAM_L1) * ((d0 - d1) - (d2 - d3))   # mu_b * DD(dI) / lambda_b
                    id_e[sig].append(e)
                    id_p[sig].append(pred)
                    id_last[sig].append(is_last)
        if cur_tow_s is not None:
            flush_epoch(cur_k, is_last)
    if n_rows == 0:
        raise SystemExit("DD trace is empty")
    if n_tow_revisit:
        print(f"WARNING: {n_tow_revisit} epoch(s) revisited out of order in {dd_path}; "
              f"lane matching is per contiguous epoch block", file=sys.stderr)
    if want_age:
        for tr in age_band.values():
            tr.finish()
        for tr in age_lane.values():
            tr.finish()
    n_slip_events = sum(len(v) for v in slips.values())

    res = {"n_rows": n_rows, "n_unmatched": n_unmatched, "n_epochs": len(tows), "t_last": t_last,
           "band_all": {}, "band_last": {}, "lane_all": {}, "lane_last": {}}

    # --- the default report (BYTE-IDENTICAL to the pre-round-2 grader) --------
    P("=" * 78)
    P("AMBIGUITY-INTEGER GRADE — DD estimates vs KNOWN true integers (cycles)")
    P("=" * 78)
    P(f"  run       : {args.run}")
    P(f"  rows      : {n_rows} traced, {n_unmatched} dropped (no truth / frac mismatch)")
    P(f"  epochs    : {len(tows)}  (last-window cut at tow >= {t_last:.0f}, "
      f"= final {args.last_frac:.0%})")
    P(f"  slips     : {n_slip_events} truth slip event(s) applied "
      f"({'quiet fixture' if n_slip_events == 0 else 'CUMULATIVE deltas active'})")
    P()
    P("-- per band (NL-class: the raw uncombined DD against its own integer) --")
    for sig in sorted(band_all):
        st = stats(band_all[sig])
        res["band_all"][sig] = st
        P(fmt_row(sig, st))
    P(f"-- same, LAST {args.last_frac:.0%} of epochs only --")
    for sig in sorted(band_last):
        st = stats(band_last[sig])
        res["band_last"][sig] = st
        P(fmt_row(sig, st))
    P()
    P("-- lane combinations (matched quadruples, cycle-difference form) --")
    P("   iono sensitivity per metre of L1 DD-iono error: L1-band 5.26 cyc, "
      "WL(1,2) 1.49, EWL(2,5) 0.29")
    for lane in lanes:
        st = stats(lane_all[lane])
        res["lane_all"][lane] = st
        P(fmt_row(lane, st))
    P(f"-- same, LAST {args.last_frac:.0%} of epochs only --")
    for lane in lanes:
        st = stats(lane_last[lane])
        res["lane_last"][lane] = st
        P(fmt_row(lane, st))

    # --- (d) Galileo lanes ----------------------------------------------------
    if want_gal and any(len(v) for v in gal_all.values()):
        res["gal_all"], res["gal_last"] = {}, {}
        P()
        P("-- Galileo lanes (sat_i = E*, matched quadruples, cycle-difference form) --")
        P("   iono sensitivity per metre of L1 DD-iono error: WL(1,5) 1.78 cyc, EWL(5,7) 0.16")
        for lane in GAL_LANES:
            st = stats(gal_all[lane])
            res["gal_all"][lane] = st
            P(fmt_row(lane, st))
        P(f"-- same, LAST {args.last_frac:.0%} of epochs only --")
        for lane in GAL_LANES:
            st = stats(gal_last[lane])
            res["gal_last"][lane] = st
            P(fmt_row(lane, st))

    bands = sorted(band_all)

    # --- (a) formal sigma schedule + first-inject ratio -----------------------
    if want_sigma:
        P()
        P("-- formal DD sigma schedule: median dd_sigma_cyc per band (k = 0-BASED emitted-epoch index; "
          "k=0 = single-epoch solve) --")
        hdr_b = "".join(f"{b:>9}" for b in bands)
        P(f"  {'k':>10}{hdr_b}   |  med|e| {hdr_b}")
        med_sig = {}
        for k in sorted(sigma_ks):
            label = f"{k}" + (" (last)" if k == k_last else "")
            cells = ""
            cells_e = ""
            for b in bands:
                m = pct(sig_at_k[k][b], 0.5) if sig_at_k[k][b] else None
                me = pct(abse_at_k[k][b], 0.5) if abse_at_k[k][b] else None
                med_sig[(k, b)] = m
                cells += "        -" if m is None else f"{m:9.3f}"
                cells_e += "        -" if me is None else f"{me:9.3f}"
            P(f"  {label:>10}{cells}   |          {cells_e}")
        ratio = {}
        parts = []
        for b in bands:
            s0, s1 = med_sig.get((warm_w, b)), med_sig.get((warm_w + 1, b))
            rr = (s1 / s0) if (s0 and s1) else None
            ratio[b] = rr
            parts.append(f"{b} " + ("  n/a" if rr is None else f"{rr:.3f}"))
        P(f"  first-inject ratio sigma(k={warm_w + 1})/sigma(k={warm_w})  "
          f"[warmup w={warm_w}, {warm_src}]: " + "  ".join(parts))
        P("     reference: correct recursion 0.707; B=2 0.577; B=3 0.500   "
          "(1/sqrt(B+1): B = same-LOS ambiguity rows re-injected as independent priors)")
        settled = {}
        parts = []
        for b in bands:
            m = pct(ratio_last[b], 0.5) if ratio_last[b] else None
            p9 = pct(ratio_last[b], 0.9) if ratio_last[b] else None
            settled[b] = m
            parts.append(f"{b} " + ("  n/a" if m is None else f"{m:.1f} (p90 {p9:.0f})"))
        P(f"  settled |e|/sigma, median over LAST {args.last_frac:.0%} (sigma>0 rows): " + "  ".join(parts))
        P("     (a calibrated formal sigma reads ~1; a frozen value under a collapsing prior reads >>1)")
        res["sigma"] = {"w": warm_w, "w_src": warm_src, "med_sigma": med_sig, "ratio": ratio,
                        "settled_ratio": settled, "k_last": k_last}

    # --- (b) |e| by chord age ---------------------------------------------------
    if want_age:
        P()
        P("-- |e| by chord age: median |e| (n) per age bin; age = epochs since the chord's run start "
          "(consecutive epochs of one (signal,sat,ref,sta_a,sta_b) key; age 0 = birth, excluded) --")
        hdr_bins = " ".join(f"{('[%d,%d)' % (lo, hi)):>18}" for lo, hi in AGE_BINS)
        P(f"  runs >= {minlen} epochs only  (slope = log-log fit of median |e| on the geometric bin centre; "
          f"-0.5 = 1/sqrt(K), ~0 = frozen level):")
        P(f"  {'':<10}{hdr_bins}")
        res["age"] = {}
        rep = {}
        for b in bands:
            rep[b] = age_band[b].report()
            P(fmt_age_row(b, rep[b]["long"], rep[b]["slope_long"]))
        for lane in list(lanes) + (list(GAL_LANES) if want_gal else []):
            if lane in age_lane:
                rep[lane] = age_lane[lane].report()
                P(fmt_age_row(lane, rep[lane]["long"], rep[lane]["slope_long"]))
        P("  all runs:")
        P(f"  {'':<10}{hdr_bins}")
        for label in rep:
            P(fmt_age_row(label, rep[label]["all"], rep[label]["slope_all"]))
        nr = ", ".join(f"{b} {rep[b]['n_runs']} ({rep[b]['n_runs_long']} >= {minlen})" for b in bands)
        P(f"  runs: {nr}")
        res["age"] = rep

    # --- (c) the DD identity vs the per-LOS iono error -----------------------
    if want_iono:
        P()
        P(f"-- DD identity: e_b vs pred = mu_b*DD(dI)/lambda_b, dI = iono_total_m - I_L1_m per LOS-epoch "
          f"(every {stride} epoch(s): {len(join_towint)} epochs; {len(prod_iono)} producer LOS-epochs, "
          f"{iono_truth_rows} joined to truth; {n_id_nodI} DD rows without all four legs) --")
        P("   a correct amb/iono split leaves resid ~ phase noise with NO slope; slope 1 / r2 ~ 1 says the "
          "per-band DD error IS the DD of the per-LOS iono error")
        res["iono"] = {}
        for b in bands:
            if b not in id_e:
                continue
            ea, pa = id_e[b], id_p[b]
            el = [x for x, f in zip(ea, id_last[b]) if f]
            pl = [x for x, f in zip(pa, id_last[b]) if f]
            ba = identity_block(ea, pa)
            bl = identity_block(el, pl)
            res["iono"][b] = {"all": ba, "last": bl}
            P(fmt_identity(f"{b} (all)", ba))
            P(fmt_identity(f"{b} (last {args.last_frac:.0%})", bl))

    # --- (e) lambda assertion + Gaussian consistency -------------------------
    lam_status = None
    if want_lam:
        P()
        P("-- lambda check: trace lambda_m vs truth_ambiguities.csv wavelength_m (tol 1e-6) --")
        lam_status = "PASS"
        res["lambda"] = {}
        for b in bands:
            tr = sorted(float(s) for s in lam_seen[b])
            th = sorted(wl_truth.get(b, ()))
            if not th:
                verdict, d = "FAIL (no truth wavelength)", None
            else:
                d = max(abs(a - c) for a in tr for c in th)
                verdict = "PASS" if (d <= 1e-6 and len(tr) == 1 and len(th) == 1) else "FAIL"
            if verdict != "PASS":
                lam_status = "FAIL"
            res["lambda"][b] = (verdict, d, tr, th)
            trs = ",".join(f"{x:.6f}" for x in tr)
            ths = ",".join(f"{x:.6f}" for x in th) if th else "-"
            ds = "   n/a" if d is None else f"{d:.1e}"
            P(f"  {b:<6} trace {trs:<10} truth {ths:<10} |d| {ds}  {verdict}")
        P(f"  lambda check: {lam_status}   (the in-loop frac_cyc check is a tautology; this and the "
          f"Gaussian consistency below validate the join)")
        P("-- Gaussian consistency: sigma from med|e|/0.6745 vs sigma from round(e)=0 share "
          "(a wrong-join integer-offset population breaks the agreement) --")
        res["gauss"] = {}
        rows = [(b, res["band_all"][b]) for b in bands] + \
               [(f"{b} last", res["band_last"][b]) for b in bands if b in res["band_last"]] + \
               [(l, res["lane_all"][l]) for l in lanes]
        if want_gal and "gal_all" in res:
            rows += [(l, res["gal_all"][l]) for l in GAL_LANES]
        for label, st in rows:
            if st is None:
                continue
            s_med, s_p0 = gaussian_sigmas(st[2], st[6])
            res["gauss"][label] = (s_med, s_p0)
            if s_p0 is None:
                P(f"  {label:<10} sigma(med|e|) {s_med:7.3f} cyc   sigma(round0) n/a (round0 {st[6]:.1f}%)")
            else:
                P(f"  {label:<10} sigma(med|e|) {s_med:7.3f} cyc   sigma(round0) {s_p0:7.3f} cyc   "
                  f"{abs(s_med - s_p0) / s_med * 100:4.0f}% apart")
        res["lambda_status"] = lam_status

    P()
    P("  READING GUIDE: 'round(e)=0' is the absolute question (near ITS OWN")
    P("  integer); 'med|e|' bounds the whole error including integer offsets —")
    P("  a fractional-part test can never separate the two, which is the point.")
    P("=" * 78)
    return res


def build_parser():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixture_dir", nargs="?")
    ap.add_argument("--run", required=False, help="producer run dir (its logs/ holds network_dd_fractional.csv)")
    ap.add_argument("--last-frac", type=float, default=0.25,
                    help="also report the LAST this fraction of epochs (default 0.25)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--sigma-ratio", action="store_true",
                    help="formal dd_sigma_cyc schedule vs epoch index, first-inject ratio, settled |e|/sigma")
    ap.add_argument("--warmup", type=int, default=6,
                    help="phase_leveling_warmup_epochs when the run manifest lacks it (default 6)")
    ap.add_argument("--by-age", action="store_true", help="median |e| by chord age + log-log slope")
    ap.add_argument("--by-age-minlen", type=int, default=200,
                    help="runs >= this many epochs feed the slope column (default 200)")
    ap.add_argument("--iono-join", action="store_true",
                    help="DD identity e_b vs mu_b*DD(dI)/lambda_b from network_ionosphere.csv + truth_atmo.csv")
    ap.add_argument("--stride", type=int, default=4, help="--iono-join epoch stride (default 4)")
    ap.add_argument("--gal-lanes", action="store_true", help="Galileo lanes EWL(5,7) and WL(1,5)")
    ap.add_argument("--check-lambda", action="store_true",
                    help="assert trace lambda_m == truth wavelength_m (1e-6) + Gaussian-consistency check")
    return ap


def main():
    ap = build_parser()
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.fixture_dir or not args.run:
        ap.error("fixture_dir and --run are required (or --selftest)")
    res = grade(args)
    if res.get("lambda_status") == "FAIL":
        return 2
    return 0


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------
_WL = {"L1C": 0.190293672798, "L2W": 0.244210213425, "L5Q": 0.254828048790, "L7Q": 0.248349369584}


def _write_mini_bed(td, warmup=2, bad_lambda=False, with_manifest=True):
    """A hand-built 2-station bed: GPS quad (G02 vs G01, L1C/L2W/L5Q) on every
    epoch, Galileo quad (E02 vs E01, L1C/L5Q/L7Q) on epochs 0-9 and 20-39, a
    truth slip on (SBBB, G02, L1C) at tow 150, the per-band error engineered as
    EXACTLY the DD identity of a known per-LOS iono error, and a formal sigma
    that halves per epoch after the warm-up (floor 0.01).  Returns the expected
    values the selftest asserts against, computed independently here."""
    fx = os.path.join(td, "fixture")
    run = os.path.join(td, "run")
    os.makedirs(os.path.join(run, "logs"))
    sats = {"G01": 0, "G02": 7, "E01": 11, "E02": 20}
    boff = {"L1C": 0, "L2W": 3, "L5Q": 5, "L7Q": 9}
    sigs_of = {"G": ("L1C", "L2W", "L5Q"), "E": ("L1C", "L5Q", "L7Q")}
    n_ep = 40
    tows = [30.0 * k for k in range(n_ep)]
    # per-LOS iono error dI (L1 m), varying with k so the identity fit has spread
    def dI(sta, sat, k):
        base = {("SAAA", "G02"): 0.30, ("SAAA", "G01"): 0.10, ("SBBB", "G02"): -0.05, ("SBBB", "G01"): 0.02,
                ("SAAA", "E02"): 0.20, ("SAAA", "E01"): 0.05, ("SBBB", "E02"): 0.00, ("SBBB", "E01"): -0.03}[(sta, sat)]
        if sat == "G02" and sta == "SAAA":
            base += 0.01 * k
        if sat == "E02" and sta == "SAAA":
            base += 0.005 * k
        return base
    n_true = {}
    for sta, base in (("SAAA", 100000), ("SBBB", 200000)):
        os.makedirs(os.path.join(fx, sta))
        with open(os.path.join(fx, sta, "truth_ambiguities.csv"), "w") as f:
            f.write("satellite,signal,N_int,wavelength_m\n")
            for sat, off in sats.items():
                for sig in sigs_of[sat[0]]:
                    wl = _WL[sig] + (1e-3 if (bad_lambda and sta == "SAAA" and sig == "L2W") else 0.0)
                    n_true[(sta, sat, sig)] = base + off + boff[sig]
                    f.write(f"{sat},{sig},{base + off + boff[sig]},{wl:.12f}\n")
        with open(os.path.join(fx, sta, "truth_slips.csv"), "w") as f:
            f.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
            if sta == "SBBB":
                f.write("2313,150.0,G02,L1C,5,1,45.0\n")
        with open(os.path.join(fx, sta, "truth_atmo.csv"), "w") as f:
            f.write("week,tow_s,satellite,elev_deg,I_L1_m,dI_L1_m\n")
            for k, tow in enumerate(tows):
                for sat in sats:
                    if sat[0] == "E" and 10 <= k < 20:
                        continue
                    i_true = round(5.0 + 0.1 * k + 0.5 * sats[sat], 4)
                    f.write(f"2313,{tow:.3f},{sat},45.0,{i_true:.4f},0.0000\n")

    def n_at_local(sta, sat, sig, tow):
        n = n_true[(sta, sat, sig)]
        if sta == "SBBB" and sat == "G02" and sig == "L1C" and tow >= 150.0:
            n += 5
        return n

    def sigma_at(k):
        return 6.0 if k <= warmup else max(6.0 * 0.5 ** (k - warmup), 0.01)

    e_of = {}                                                  # (sig, sat, k) -> e
    with open(os.path.join(run, "logs", "network_ionosphere.csv"), "w") as fi, \
            open(os.path.join(run, "logs", "network_dd_fractional.csv"), "w") as fd:
        fi.write("week,tow,station,sat,iono_m,elevation_deg,azimuth_deg,x_lin_m,iono_total_m\n")
        fd.write("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc,sigma_carry_aware\n")
        for k, tow in enumerate(tows):
            for sta in ("SAAA", "SBBB"):
                for sat in sats:
                    if sat[0] == "E" and 10 <= k < 20:
                        continue
                    i_true = round(5.0 + 0.1 * k + 0.5 * sats[sat], 4)
                    tot = i_true + dI(sta, sat, k)
                    fi.write(f"2313,{tow:.3f},{sta},{sat},0.0,45.00,0.00,0.0,{tot:.6f}\n")
            for sat, ref in (("G02", "G01"), ("E02", "E01")):
                if sat[0] == "E" and 10 <= k < 20:
                    continue
                dd_di = (dI("SAAA", sat, k) - dI("SAAA", ref, k)) - (dI("SBBB", sat, k) - dI("SBBB", ref, k))
                for sig in sigs_of[sat[0]]:
                    lam = _WL[sig]
                    e = lam / (LAM_L1 * LAM_L1) * dd_di
                    dd_true = ((n_at_local("SAAA", sat, sig, tow) - n_at_local("SAAA", ref, sig, tow))
                               - (n_at_local("SBBB", sat, sig, tow) - n_at_local("SBBB", ref, sig, tow)))
                    dd_cyc = dd_true + e
                    e_of[(sig, sat, k)] = e
                    fd.write(f"2313,{tow:.3f},{sat[0]},{sig},{sat},{ref},SAAA,SBBB,{dd_cyc * lam:.4f},{lam:.6f},"
                             f"{dd_cyc:.4f},{e - round(e):.4f},{sigma_at(k):.6g},0\n")
    if with_manifest:
        with open(os.path.join(run, "logs", "network_run_manifest.json"), "w") as f:
            f.write(json.dumps({"record": "startup", "network": {"phase_leveling_warmup_epochs": warmup}}) + "\n")
            f.write(json.dumps({"record": "complete", "n_emitted": n_ep}) + "\n")

    # --- expectations, computed independently of the grader's machinery -----
    exp = {"n_rows": sum(3 for k in range(n_ep)) + sum(3 for k in range(n_ep) if not 10 <= k < 20),
           "n_epochs": n_ep, "sigma_at": sigma_at, "warmup": warmup}
    t_last = tows[int(n_ep * 0.75)]
    last_ks = [k for k in range(n_ep) if tows[k] >= t_last]
    # settled |e|/sigma per band (GPS bands have all epochs; L7Q only the Galileo quad)
    def settled_ratio(sig):
        vals = []
        for (s, sat, k), e in e_of.items():
            if s == sig and k in last_ks:
                vals.append(abs(e) / sigma_at(k))
        return pct(vals, 0.5)
    exp["settled_ratio"] = {s: settled_ratio(s) for s in ("L1C", "L2W", "L5Q", "L7Q")}
    # by-age bins (minlen 5): GPS run 0..39; Galileo runs 0..9 and 20..39
    def age_bins(sig):
        per_bin = [[] for _ in AGE_BINS]
        runs = [[k for k in range(n_ep)]] if sig in ("L2W",) else None
        keys = [("G02", list(range(n_ep)))] if sig != "L7Q" else []
        if sig in ("L1C", "L5Q", "L7Q"):
            keys += [("E02", list(range(0, 10))), ("E02", list(range(20, 40)))]
        for sat, ks in keys:
            for age, k in enumerate(ks):
                if age == 0:
                    continue
                for bi, (lo, hi) in enumerate(AGE_BINS):
                    if lo <= age < hi:
                        per_bin[bi].append(abs(e_of[(sig, sat, k)]))
        return [(pct(b, 0.5) if b else None, len(b)) for b in per_bin]
    exp["age"] = {s: age_bins(s) for s in ("L1C", "L2W", "L5Q", "L7Q")}
    # Galileo lanes
    ewl57 = [e_of[("L5Q", "E02", k)] - e_of[("L7Q", "E02", k)] for k in range(n_ep) if not 10 <= k < 20]
    wl15 = [e_of[("L1C", "E02", k)] - e_of[("L5Q", "E02", k)] for k in range(n_ep) if not 10 <= k < 20]
    exp["gal"] = {"EWL(5,7)": (len(ewl57), pct([abs(x) for x in ewl57], 0.5)),
                  "WL(1,5)": (len(wl15), pct([abs(x) for x in wl15], 0.5))}
    exp["fixture"], exp["run"] = fx, run
    return exp


def _close(a, b, tol):
    return a is not None and b is not None and abs(a - b) <= tol


def selftest():
    """Exercise the integer join + slip accumulation + lane matching on a
    hand-built mini-case, then every mechanism-signature flag on a synthetic
    mini bed with independently computed expectations."""
    import io
    import tempfile
    ok = True
    fails = []

    def check(cond, what):
        nonlocal ok
        if not cond:
            ok = False
            fails.append(what)

    with tempfile.TemporaryDirectory() as td:
        for sta, base in (("SAAA", 100000), ("SBBB", 200000)):
            os.makedirs(os.path.join(td, sta))
            with open(os.path.join(td, sta, "truth_ambiguities.csv"), "w") as f:
                f.write("satellite,signal,N_int,wavelength_m\n")
                for sat, off in (("G01", 0), ("G02", 7)):
                    for sig, boff in (("L1C", 0), ("L2W", 3)):
                        f.write(f"{sat},{sig},{base + off + boff},0.19\n")
            with open(os.path.join(td, sta, "truth_slips.csv"), "w") as f:
                f.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
                if sta == "SBBB":
                    f.write("2313,150.0,G02,L1C,5,1,45.0\n")
        n_int, slips = load_truth(td, {"SAAA", "SBBB"})
        # before the slip: DD_true = [(A,G02)-(A,G01)] - [(B,G02)-(B,G01)] = 7-7 = 0
        legs = lambda tow, sig: ((n_at(n_int, slips, "SAAA", "G02", sig, tow)
                                  - n_at(n_int, slips, "SAAA", "G01", sig, tow))
                                 - (n_at(n_int, slips, "SBBB", "G02", sig, tow)
                                    - n_at(n_int, slips, "SBBB", "G01", sig, tow)))
        check(legs(100.0, "L1C") == 0, "legs pre-slip")
        # after the slip on (SBBB, G02, L1C): DD_true = 0 - 5 = -5
        check(legs(200.0, "L1C") == -5, "legs post-slip")
        check(legs(200.0, "L2W") == 0, "legs other band")          # other band untouched
        # stats: round(e)==0 share
        st = stats([0.02, -0.10, 0.90, 1.04])
        check(st[0] == 4 and abs(st[6] - 50.0) < 1e-9, "stats round0")

    # Gaussian consistency on a unit Gaussian: med|e| = 0.6745, P(|e|<0.5) = erf(0.5/sqrt2) = 38.29 %
    s_med, s_p0 = gaussian_sigmas(0.6745, 100.0 * math.erf(0.5 / math.sqrt(2.0)))
    check(_close(s_med, 1.0, 1e-6) and _close(s_p0, 1.0, 1e-3), "gaussian_sigmas unit")
    check(gaussian_sigmas(0.1, 100.0)[1] is None, "gaussian_sigmas saturated")
    fit = linfit([0, 1, 2, 3], [1, 3, 5, 7])
    check(fit is not None and _close(fit[0], 2.0, 1e-12) and _close(fit[1], 1.0, 1e-12) and _close(fit[2], 1.0, 1e-12),
          "linfit")
    check(_close(loglog_slope([1, 10, 100], [1, 0.1, 0.01]), -1.0, 1e-9), "loglog_slope")
    tr = AgeTracker(3)
    for k in (0, 1, 2, 3, 5, 6):                                  # run 0..3 (len 4), gap, run 5..6 (len 2)
        tr.add("x", k, float(k))
    tr.finish()
    rep = tr.report()
    check(rep["n_runs"] == 2 and rep["n_runs_long"] == 1, "AgeTracker runs")
    # long: ages 1,2,3 of run 1 -> [1,2,3]; all: + age 1 of run 2 -> [1,2,3,6], pct index round(1.5)=2 -> 3.0
    check(rep["long"][0] == (2.0, 3) and rep["all"][0] == (3.0, 4), "AgeTracker bins")

    # --- the mini bed: every flag ------------------------------------------
    with tempfile.TemporaryDirectory() as td:
        exp = _write_mini_bed(td, warmup=2)
        args = build_parser().parse_args(
            [exp["fixture"], "--run", exp["run"], "--sigma-ratio", "--by-age", "--by-age-minlen", "5",
             "--iono-join", "--stride", "4", "--gal-lanes", "--check-lambda"])
        buf = io.StringIO()
        res = grade(args, buf)
        text = buf.getvalue()
        check(res["n_rows"] == exp["n_rows"] and res["n_unmatched"] == 0, "mini rows/unmatched")
        check(res["n_epochs"] == exp["n_epochs"], "mini epochs")
        # per-band e recovered exactly (4-decimal trace) — the slip on L1C is absorbed into DD_true
        check(all(res["band_all"][s][2] > 1.0 for s in ("L1C", "L2W", "L5Q", "L7Q")), "mini band med|e|")
        # (a) sigma schedule + ratio
        sg = res["sigma"]
        check(sg["w"] == 2 and sg["w_src"] == "manifest", "sigma warmup from manifest")
        for s in ("L1C", "L2W", "L5Q"):
            check(_close(sg["ratio"][s], 0.5, 1e-6), f"first-inject ratio {s}")
            for k in (0, 2, 3, 4, 12, res["sigma"]["k_last"]):
                check(_close(sg["med_sigma"].get((k, s)), exp["sigma_at"](k), 1e-6), f"sigma at k={k} {s}")
            check(_close(sg["settled_ratio"][s], exp["settled_ratio"][s], 0.05), f"settled ratio {s}")
        check(sg["med_sigma"].get((12, "L7Q")) is None, "L7Q absent at k=12 (gap)")    # Galileo gap 10..19
        # (b) by-age
        for s in ("L1C", "L2W", "L5Q", "L7Q"):
            got = res["age"][s]["long"]
            for bi in range(len(AGE_BINS)):
                em, en = exp["age"][s][bi]
                gm, gn = got[bi]
                check(gn == en and (em is None or _close(gm, em, 1e-3)), f"age bin {bi} {s}: got {got[bi]} exp {exp['age'][s][bi]}")
        check(res["age"]["L7Q"]["n_runs"] == 2 and res["age"]["L1C"]["n_runs"] == 3, "age run counts")
        check(res["age"]["L1C"]["slope_long"] is not None and res["age"]["L1C"]["slope_long"] > 0, "age slope sign")
        check("EWL(2,5)" in res["age"] and "EWL(5,7)" in res["age"], "age lanes present")
        # (c) identity: e == pred by construction -> slope 1, icpt 0, r2 1, resid ~0
        for s in ("L1C", "L2W", "L5Q", "L7Q"):
            b = res["iono"][s]["all"]
            check(b is not None and _close(b["slope"], 1.0, 2e-3) and _close(b["icpt"], 0.0, 2e-3)
                  and b["r2"] > 0.999 and b["med_abs"] < 1e-3 and b["round0"] == 100.0, f"identity {s}: {b}")
        # stride 4 -> k = 0,4,...,36: GPS quad 10 rows + Galileo quad L1C rows at k=0,4,8,20,...,36 (8) = 18
        check(res["iono"]["L1C"]["all"]["n"] == 18 and res["iono"]["L2W"]["all"]["n"] == 10,
              "identity stride (40 epochs / 4)")
        # (d) Galileo lanes restricted to E quads
        for lane, (n, m) in exp["gal"].items():
            st = res["gal_all"][lane]
            check(st is not None and st[0] == n and _close(st[2], m, 1e-3), f"gal lane {lane}")
        # (e) lambda + Gaussian block present
        check(res["lambda_status"] == "PASS" and all(v[0] == "PASS" for v in res["lambda"].values()), "lambda PASS")
        check("L1C" in res["gauss"] and "EWL(5,7)" in res["gauss"], "gauss rows")
        for marker in ("first-inject ratio", "|e| by chord age", "DD identity", "Galileo lanes",
                       "lambda check: PASS", "Gaussian consistency"):
            check(marker in text, f"report marker '{marker}'")
        # default path on the same bed: none of the new blocks, same header lines
        args0 = build_parser().parse_args([exp["fixture"], "--run", exp["run"]])
        buf0 = io.StringIO()
        res0 = grade(args0, buf0)
        text0 = buf0.getvalue()
        check(all(m not in text0 for m in ("first-inject", "chord age", "DD identity", "Galileo lanes", "lambda check")),
              "default output has no new blocks")
        check(res0["band_all"] == res["band_all"] and res0["lane_all"] == res["lane_all"], "default == flagged core")
        # warmup fallback without a manifest
        os.remove(os.path.join(exp["run"], "logs", "network_run_manifest.json"))
        args1 = build_parser().parse_args([exp["fixture"], "--run", exp["run"], "--sigma-ratio", "--warmup", "3"])
        res1 = grade(args1, io.StringIO())
        check(res1["sigma"]["w"] == 3 and res1["sigma"]["w_src"] == "--warmup", "warmup fallback")
        check(_close(res1["sigma"]["ratio"]["L1C"], 0.5, 1e-6), "ratio at w+1=4 over w=3 (0.5 by construction)")

    # a wrong truth wavelength must FAIL the lambda check
    with tempfile.TemporaryDirectory() as td:
        exp = _write_mini_bed(td, warmup=2, bad_lambda=True)
        args = build_parser().parse_args([exp["fixture"], "--run", exp["run"], "--check-lambda"])
        res = grade(args, io.StringIO())
        check(res["lambda_status"] == "FAIL" and res["lambda"]["L2W"][0] == "FAIL"
              and res["lambda"]["L1C"][0] == "PASS", "lambda FAIL detected")

    print("selftest:", "PASS" if ok else "FAIL")
    for f_ in fails:
        print("  FAILED:", f_)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
