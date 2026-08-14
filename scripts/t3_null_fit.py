#!/usr/bin/env python3
"""T3-class null calibration: diagnose the statistic, not the threshold.

QUEUE item 4 (`docs/QUEUE_principled_replacements.md`) — the bandaid being
replaced is a Gaussian 6σ quantile applied to

    S_c = max_{i∈c} |z_i|  /  max(floor, 1.4826 · median_{j∈P} |z_j|)

where c is a (station, satellite) cell, P is the epoch's network-wide pool of
post-fit phase-residual rows, and z = |v|·√w (·/√r_i only when
`network.raim_redundancy_normalize` is armed — it is OFF by default).

The card's finding, and why this script exists: **S_c is invariant to any
COMMON rescaling of z**, because the same factor multiplies numerator and
denominator.  So no global variance-model fix and no recalibrated k can move
its false-alarm rate — only the RELATIVE structure of the pool can.  This tool
measures that structure and reports what a correctly stratified null gives.

Reads `<run>/logs/network_station_residuals.csv` (armed by
`network.trace_station_residuals`), which is exactly the T3 cell population —
one row per (epoch, station, satellite) with s_raw, s_norm, sigma0.  Cross-checks
against `network_flag_rates.csv` when present.

    python scripts/t3_null_fit.py --run runs/pareref_on
    python scripts/t3_null_fit.py --fit runs/pareref_on --verify runs/<other-seed>
    python scripts/t3_null_fit.py --run runs/pareref_on --strata station,epoch
    python scripts/t3_null_fit.py --selftest

⚠ Grade on a SETTLED window.  The residual scale on the 24-sta fixture rises
through ignition (epoch-median s_raw 0.84 first third → 1.82 last third), so a
short arm measures convergence, not the null.  `--warmup-s` (default 1800)
drops the head.

⚠ The verification arm must be SAME-REGIME, different-seed (QUEUE item 4): the
60-sta fixture is LEGACY-N and a different posture, and fitting on one regime
while verifying on the other conflates REGIME with STATISTIC.
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import sys
from collections import defaultdict
from pathlib import Path

# The report prints σ̂ / α / × / ⇒.  A Windows console defaults to cp1252 and
# raises UnicodeEncodeError on the first one, killing the run mid-table.
try:                                            # pragma: no cover - console detail
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, OSError):
    pass

# --- the reference null of the cell statistic ------------------------------------
# A cell is a max over the (station,sat)'s carried-ambiguity phase rows.  Under the
# ideal null those are standard normals, so the cell statistic is max_{n_c}|z|.
# Median of that max, by n_c — used to put a fitted stratum scale on a "1.0 = nominal"
# footing.  Closed form: median m solves (2Φ(m)−1)^n = 1/2.
_DEF_NC = 3  # GPS triple-frequency uncombined: L1C / L2W / L5Q phase rows per cell


def phi_cdf(x: float) -> float:
    """Standard normal CDF."""
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def median_max_abs_normal(n: int) -> float:
    """Median of max of n iid |N(0,1)|:  solve (2Φ(m)−1)^n = 1/2."""
    target = 0.5 ** (1.0 / n)
    lo, hi = 0.0, 12.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if 2.0 * phi_cdf(mid) - 1.0 < target:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def cell_alpha(k: float, n_c: int) -> float:
    """Per-cell false-alarm probability of max_{n_c}|z| > k under an EXACT null."""
    return 1.0 - (2.0 * phi_cdf(k) - 1.0) ** n_c


def implied_scale(rate: float, k: float) -> float:
    """The scale c a Gaussian null would need to produce `rate` at threshold k.

    Solves 2(1−Φ(k/c)) = rate.  If c is flat in k the residue is SCALE; if c rises
    with k the residue is SHAPE and no recalibrated constant can fix it.
    """
    if rate <= 0.0:
        return float("nan")
    lo, hi = 1e-3, 60.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if 2.0 * (1.0 - phi_cdf(k / mid)) < rate:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def denom_noise_sd(m: int) -> float:
    """Relative SD of σ̂ = 1.4826·median|z| over m iid rows (asymptotic).

    med|z| → q = Φ⁻¹(0.75) = 0.6745 with density f(q) = 2φ(q) = 0.63556, so
    Var(med) ≈ 1/(4 m f(q)²) and Var(σ̂) ≈ 1.4826²/(4 m f(q)²).
    """
    q = 0.6744897501960817
    fq = 2.0 * math.exp(-0.5 * q * q) / math.sqrt(2.0 * math.pi)
    return 1.4826 * math.sqrt(1.0 / (4.0 * m * fq * fq))


def exact_k(alpha: float, n_c: int) -> float:
    """Exact-null threshold: P(max_{n_c}|z| > k) = alpha with sigma-hat identically 1.

    Solves 1 − (2Φ(k)−1)^n_c = α in closed form, so it is good at any α — a brute
    Monte Carlo cannot resolve a 1e-6 quantile without ~1e8 draws.
    """
    target = (1.0 - alpha) ** (1.0 / n_c)          # = 2Φ(k) − 1
    lo, hi = 0.0, 40.0
    for _ in range(300):
        mid = 0.5 * (lo + hi)
        if 2.0 * phi_cdf(mid) - 1.0 < target:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def corrected_k(alpha: float, n_c: int, m_denom: int, trials: int = 40_000,
                seed: int = 20260814, floor: float = 1.0) -> float:
    """Threshold k giving per-cell false-alarm `alpha` for max_{n_c}|z| / σ̂.

    σ̂ = max(floor, 1.4826·median|z|) over m_denom rows, so the statistic is
    studentized and its tail is fatter than the exact-null one.

    The denominator is Monte-Carlo'd (drawing the pool and taking its ACTUAL
    median — at small m the sampling distribution of a median is skewed and
    bounded below, so a Gaussian surrogate puts mass near zero and produces
    absurd quantiles), but the NUMERATOR is then integrated in closed form:

        P(S > k) = E_σ̂[ 1 − (2Φ(k σ̂) − 1)^n_c ]

    so the accuracy does not degrade at small α the way a brute-force quantile
    of simulated ratios does.  Pass ``m_denom <= 0`` for the exact null.
    """
    if m_denom <= 0:
        return exact_k(alpha, n_c)
    import numpy as np                       # in-tree; scipy is not
    g = np.random.default_rng(seed)
    # one independent pool per draw; a cell's own rows are a vanishing fraction of
    # a real pool, so the numerator/denominator overlap is O(n_c/m) and is reported
    # separately rather than modelled here
    s = np.maximum(floor,
                   1.4826 * np.median(np.abs(g.standard_normal((trials, m_denom))), axis=1))
    erfc = np.frompyfunc(math.erfc, 1, 1)

    def tail(k: float) -> float:
        p = 0.5 * erfc(-(k * s) / math.sqrt(2.0)).astype(float)   # Φ(k σ̂)
        return float(np.mean(1.0 - (2.0 * p - 1.0) ** n_c))

    lo, hi = 0.0, 60.0
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if tail(mid) > alpha:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


# --- data ------------------------------------------------------------------------

class Cells:
    """The T3 cell population of one run."""

    def __init__(self, tow, sta, sat, s_raw, s_norm, sigma0, src: Path):
        self.tow, self.sta, self.sat = tow, sta, sat
        self.s_raw, self.s_norm, self.sigma0 = s_raw, s_norm, sigma0
        self.src = src

    def __len__(self):
        return len(self.s_raw)

    def settled(self, warmup_s: float):
        """Index list past the warmup head, excluding structurally untestable cells."""
        t0 = min(self.tow)
        return [i for i in range(len(self.s_raw))
                if self.tow[i] > t0 + warmup_s and self.s_raw[i] > 0.0]


def find_csv(run: Path, name: str) -> Path | None:
    for cand in (run, run / "logs", run / "logs_v2"):
        p = cand / name
        if p.is_file():
            return p
    return None


def load_cells(run: Path) -> Cells:
    p = find_csv(run, "network_station_residuals.csv")
    if p is None:
        sys.exit(f"ERROR: no network_station_residuals.csv under {run} "
                 f"(arm network.trace_station_residuals)")
    tow, sta, sat, sr, sn, s0 = [], [], [], [], [], []
    with p.open(newline="", encoding="utf-8", errors="replace") as f:
        for d in csv.DictReader(f):
            try:
                tow.append(float(d["tow"])); sta.append(d["station"]); sat.append(d["sat"])
                sr.append(float(d["s_raw"])); sn.append(float(d["s_norm"]))
                s0.append(float(d["sigma0"]))
            except (KeyError, ValueError):
                continue
    if not sr:
        sys.exit(f"ERROR: {p} parsed to zero cells")
    return Cells(tow, sta, sat, sr, sn, s0, p)


def load_flag_rates(run: Path) -> dict:
    p = find_csv(run, "network_flag_rates.csv")
    if p is None:
        return {}
    out = {}
    with p.open(newline="", encoding="utf-8", errors="replace") as f:
        for d in csv.DictReader(f):
            out[d["tier"]] = d
    return out


# --- statistics ------------------------------------------------------------------

def median(v):
    s = sorted(v)
    n = len(s)
    if n == 0:
        return float("nan")
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def quantile(v, q):
    s = sorted(v)
    if not s:
        return float("nan")
    i = min(len(s) - 1, max(0, int(math.ceil(q * len(s))) - 1))
    return s[i]


def moments(v):
    n = len(v)
    m = sum(v) / n
    var = sum((x - m) ** 2 for x in v) / (n - 1)
    sd = math.sqrt(var)
    sk = sum(((x - m) / sd) ** 3 for x in v) / n
    ku = sum(((x - m) / sd) ** 4 for x in v) / n - 3.0
    return m, sd, sk, ku


def fit_strata(cells: Cells, idx, keys, n_c: int, min_n: int = 20):
    """Robust multiplicative scale per stratum, normalized so 1.0 = the ideal null."""
    mref = median_max_abs_normal(n_c)
    groups = defaultdict(list)
    for i in idx:
        groups[keys(cells, i)].append(cells.s_raw[i])
    return {k: median(v) / mref for k, v in groups.items() if len(v) >= min_n}, mref


def sweep_strata(cells: Cells, idx, strata, keyfns, n_c, sweeps=1, min_n=20):
    """Multiplicative robust scale model, fitted by repeated median sweeps.

    One sweep per stratum in order; `sweeps > 1` re-visits them (median polish on
    log s_raw, in multiplicative form).  Returns (denominators, per-stratum maps).
    A single pass is a LOWER bound on what the model can absorb — the strata are
    not orthogonal, so the first pass leaves cross-terms behind.
    """
    mref = median_max_abs_normal(n_c)
    maps = {s: {} for s in strata}
    fallback = {s: 1.0 for s in strata}

    def denom_for(i):
        d = 1.0
        for s in strata:
            d *= maps[s].get(keyfns[s](cells, i), fallback[s])
        return d if d > 0 else 1.0

    for _ in range(max(1, sweeps)):
        for s in strata:
            kf = keyfns[s]
            # hold every OTHER stratum fixed and refit this one on what they leave
            others = [s2 for s2 in strata if s2 != s]
            resid = []
            for i in idx:
                d = 1.0
                for s2 in others:
                    d *= maps[s2].get(keyfns[s2](cells, i), fallback[s2])
                resid.append(cells.s_raw[i] / (d if d > 0 else 1.0))
            groups = defaultdict(list)
            for j, i in enumerate(idx):
                groups[kf(cells, i)].append(resid[j])
            maps[s] = {k: median(v) / mref for k, v in groups.items() if len(v) >= min_n}
            fallback[s] = median(resid) / mref
    return [denom_for(i) for i in idx], maps, mref


def split_half_oos(cells: Cells, idx, strata, keyfns, n_c, sweeps):
    """Fit the TRANSFERABLE strata on the first half, score the second.

    Epoch factors cannot transfer (they are a within-run nuisance), so they are
    refitted on the held-out half; only station/satellite factors carry a claim.
    """
    tows = sorted({cells.tow[i] for i in idx})
    cut = tows[len(tows) // 2]
    fit = [i for i in idx if cells.tow[i] < cut]
    tst = [i for i in idx if cells.tow[i] >= cut]
    if len(fit) < 200 or len(tst) < 200:
        return None
    slow = [s for s in strata if s != "epoch"]
    if not slow:
        return None
    _, maps, mref = sweep_strata(cells, fit, slow, keyfns, n_c, sweeps)
    gl = median([cells.s_raw[i] for i in fit]) / mref
    den = []
    for i in tst:
        d = 1.0
        for s in slow:
            d *= maps[s].get(keyfns[s](cells, i), gl)
        den.append(d if d > 0 else 1.0)
    v = [cells.s_raw[i] / den[j] for j, i in enumerate(tst)]
    if "epoch" in strata:                     # refit the fast factor on the held-out half
        g = defaultdict(list)
        for j, i in enumerate(tst):
            g[cells.tow[i]].append(v[j])
        sc = {k: median(x) / mref for k, x in g.items()}
        fb = median(v) / mref
        v = [v[j] / max(1e-9, sc.get(cells.tow[i], fb)) for j, i in enumerate(tst)]
    base = [cells.s_norm[i] for i in tst]
    return base, v, len(fit), len(tst), maps, slow


def curve_line(v, ks=(3, 4, 5, 6, 8, 10, 12)):
    out = []
    for k in ks:
        r = sum(1 for x in v if x > k) / len(v)
        c = implied_scale(r, k)
        out.append(f"k={k:<2d} rate={r:.3e} c={c:6.3f}" if r > 0 else f"k={k:<2d} rate=0        c=  ----")
    return out


# --- report ----------------------------------------------------------------------

def diagnose(run: Path, warmup_s: float, n_c: int, strata: list[str],
             sweeps: int = 4, quiet=False):
    cells = load_cells(run)
    idx = cells.settled(warmup_s)
    zero = sum(1 for i in range(len(cells)) if cells.s_raw[i] == 0.0)
    fr = load_flag_rates(run)

    if not quiet:
        print(f"\n{'='*78}\n  {run}   ({cells.src.name})\n{'='*78}")
        print(f"cells {len(cells)} total, {len(idx)} settled "
              f"(> t0+{warmup_s:.0f}s and s_raw>0) over "
              f"{len(set(cells.tow[i] for i in idx))} epochs, "
              f"{len(set(cells.sta[i] for i in idx))} stations")
        print(f"structurally untestable cells (s_raw exactly 0): {zero} "
              f"({100*zero/len(cells):.2f}%) — counted as T3 opportunities but can never flag")

        if "T3" in fr:
            obs = float(fr["T3"]["rate"]); imp = float(fr["T3"]["implied_alpha"])
            k = float(fr["T3"]["k"])
            print(f"\n-- as shipped, per network_flag_rates.csv --")
            print(f"   T3  {fr['T3']['flags']}/{fr['T3']['opportunities']} = {obs:.6e}"
                  f"   implied α(k={k:g}) = {imp:.6e}   ratio = {obs/imp:.4e}×")
            if "T3c" in fr:
                o2 = float(fr["T3c"]["rate"])
                print(f"   T3c {fr['T3c']['flags']}/{fr['T3c']['opportunities']} = {o2:.6e}"
                      f"   (opportunities are CONDITIONAL on having spiked ≥1 station)")
            print(f"   NOTE the per-row α above ignores the max-over-{n_c}-rows: the EXACT "
                  f"per-cell\n        null gives α = {cell_alpha(k, n_c):.6e} "
                  f"({cell_alpha(k, n_c)/imp:.2f}× the quoted one) — a rounding error "
                  f"against the observed ratio.")

        sr = [cells.s_raw[i] for i in idx]
        m, sd, sk, ku = moments(sr)
        print(f"\n-- settled s_raw --  mean {m:.4f} sd {sd:.4f} skew {sk:.3f} exkurt {ku:.2f}")
        print(f"   q50 {quantile(sr,.5):.4f}  q90 {quantile(sr,.9):.4f}  "
              f"q99 {quantile(sr,.99):.4f}  q999 {quantile(sr,.999):.4f}  max {max(sr):.4f}")
        print(f"   reference median of max_{n_c}|N(0,1)| = {median_max_abs_normal(n_c):.4f}")

    # ---- scoring ladder
    results = {}
    if not quiet:
        print(f"\n-- flag rate at k=6, scored by normalization --")
    shipped = [cells.s_norm[i] for i in idx]
    results["shipped"] = shipped
    if not quiet:
        print(f"   {'A. as shipped (one network σ̂₀)':44s} "
              f"{sum(1 for x in shipped if x>6)/len(shipped):8.4%}")
        for ln in curve_line(shipped):
            print(f"        {ln}")

    keyfns = {
        "station": lambda c, i: c.sta[i],
        "epoch": lambda c, i: c.tow[i],
        "satellite": lambda c, i: c.sat[i],
    }
    for s in strata:
        if s not in keyfns:
            sys.exit(f"ERROR: unknown stratum '{s}' (choose from {sorted(keyfns)})")
    label = []
    for s in strata:
        label.append(s)
        denom, maps, _ = sweep_strata(cells, idx, label, keyfns, n_c, sweeps)
        v = [cells.s_raw[i] / denom[j] for j, i in enumerate(idx)]
        results["+".join(label)] = v
        if not quiet:
            vals = sorted(maps[s].values())
            print(f"   {('B. ÷ scale(' + ' × '.join(label) + ')'):44s} "
                  f"{sum(1 for x in v if x>6)/len(v):8.4%}"
                  f"   [{s} factor {vals[0]:.3f}..{vals[-1]:.3f}, "
                  f"spread {vals[-1]/vals[0]:.1f}×]")
            for ln in curve_line(v):
                print(f"        {ln}")

    oos = split_half_oos(cells, idx, strata, keyfns, n_c, sweeps)
    if oos and not quiet:
        base, v, nfit, ntst, maps, slow = oos
        print(f"\n-- OUT-OF-SAMPLE (the honest number): {'/'.join(slow)} scale fitted on the "
              f"first half\n   ({nfit} cells), applied to the held-out second half ({ntst} cells) --")
        print(f"   {'as shipped on the held-out half':44s} "
              f"{sum(1 for x in base if x>6)/len(base):8.4%}")
        print(f"   {('÷ OOS scale(' + ' × '.join(strata) + ')'):44s} "
              f"{sum(1 for x in v if x>6)/len(v):8.4%}")
        for ln in curve_line(v):
            print(f"        {ln}")
        # is the slow factor a stable property, or noise?
        for s in slow:
            a = maps[s]
            _, m2, _ = sweep_strata(cells, [i for i in idx if cells.tow[i] >=
                                            sorted({cells.tow[j] for j in idx})[len(
                                                {cells.tow[j] for j in idx}) // 2]],
                                    slow, keyfns, n_c, sweeps)
            pair = [(a[k], m2[s][k]) for k in a if k in m2.get(s, {})]
            if len(pair) >= 4:
                la = [math.log(x) for x, _ in pair]
                lb = [math.log(y) for _, y in pair]
                ma, mb = sum(la) / len(la), sum(lb) / len(lb)
                num = sum((x - ma) * (y - mb) for x, y in zip(la, lb))
                den = math.sqrt(sum((x - ma) ** 2 for x in la)
                                * sum((y - mb) ** 2 for y in lb))
                r = num / den if den > 0 else float("nan")
                print(f"   corr(log {s} scale: half 1 vs half 2) = {r:+.4f} over {len(pair)} "
                      f"{s}s -> {'STABLE, transferable' if r > 0.8 else 'weak — do not transfer'}")

    if not quiet:
        # denominator-noise budget: how many rows back each scale?
        per_ep = len(idx) / max(1, len(set(cells.tow[i] for i in idx)))
        per_sta_ep = per_ep / max(1, len(set(cells.sta[i] for i in idx)))
        print(f"\n-- denominator noise budget (relative SD of a robust scale from m rows) --")
        for m_, what in ((int(per_ep * n_c), "network pool, one epoch"),
                         (int(per_sta_ep * n_c), "one station, one epoch"),
                         (int(len(idx) / max(1, len(set(cells.sta[i] for i in idx)))),
                          "one station, whole window")):
            if m_ > 1:
                print(f"   m={m_:5d}  SD(σ̂)/σ = {denom_noise_sd(m_):6.2%}   ({what})")
        print(f"   ⇒ a per-station-per-EPOCH scale is too noisy to studentize against; the")
        print(f"     stable design is a slow per-station factor × a fast network-wide one.")

    return cells, idx, results


def transfer(fit_run: Path, ver_run: Path, warmup_s: float, n_c: int,
             strata: list[str], sweeps: int = 4):
    """Fit stratum scales on one arm, apply to another.  The honest test."""
    fc = load_cells(fit_run); fi = fc.settled(warmup_s)
    vc = load_cells(ver_run); vi = vc.settled(warmup_s)
    print(f"\n{'='*78}\n  TRANSFER   fit {fit_run}  →  verify {ver_run}\n{'='*78}")
    print(f"fit {len(fi)} settled cells / verify {len(vi)} settled cells")

    base = [vc.s_norm[i] for i in vi]
    print(f"\n   {'as shipped on the verify arm':44s} {sum(1 for x in base if x>6)/len(base):8.4%}")

    mref = median_max_abs_normal(n_c)
    denom = [1.0] * len(vi)
    fden = [1.0] * len(fi)
    keyfns = {"station": lambda c, i: c.sta[i], "epoch": lambda c, i: c.tow[i],
              "satellite": lambda c, i: c.sat[i]}
    lab = []
    for s in strata:
        if s == "epoch":
            # epoch factors cannot transfer between runs — refit per verify epoch
            resid = [vc.s_raw[i] / denom[j] for j, i in enumerate(vi)]
            g = defaultdict(list)
            for j, i in enumerate(vi):
                g[vc.tow[i]].append(resid[j])
            sc = {k: median(v) / mref for k, v in g.items()}
            fb = median(resid) / mref
            denom = [denom[j] * sc.get(vc.tow[i], fb) for j, i in enumerate(vi)]
            lab.append("epoch*")
        else:
            fresid = [fc.s_raw[i] / fden[j] for j, i in enumerate(fi)]
            g = defaultdict(list)
            for j, i in enumerate(fi):
                g[keyfns[s](fc, i)].append(fresid[j])
            sc = {k: median(v) / mref for k, v in g.items() if len(v) >= 20}
            fb = median(fresid) / mref
            fden = [fden[j] * sc.get(keyfns[s](fc, i), fb) for j, i in enumerate(fi)]
            denom = [denom[j] * sc.get(keyfns[s](vc, i), fb) for j, i in enumerate(vi)]
            lab.append(s)
            missing = sorted({keyfns[s](vc, i) for i in vi} - set(sc))
            if missing:
                print(f"      ({s}s absent from the fit arm, fell back to global: "
                      f"{missing[:6]}{' …' if len(missing) > 6 else ''})")
        v = [vc.s_raw[i] / denom[j] for j, i in enumerate(vi)]
        print(f"   {('÷ scale(' + ' × '.join(lab) + ')'):44s} "
              f"{sum(1 for x in v if x>6)/len(v):8.4%}")
        for ln in curve_line(v):
            print(f"        {ln}")
    print("\n   * an epoch factor is refitted on the verify arm by construction — it is a")
    print("     within-run nuisance, not a transferable property.  Only the station factor")
    print("     is a claim about the network that has to survive a different seed.")


def _simulate_pool(rng, n_sta, n_ep, n_c, cells_per_sta, sta_scale):
    """One synthetic run: returns per-cell (s_raw, sigma0_epoch, station).

    Mirrors the shipped construction exactly — the epoch's sigma0 is the floored
    1.4826·median over the network-wide ROW pool, and the cell is the max over
    that (station,sat)'s n_c rows.
    """
    out = []
    for _ in range(n_ep):
        pool, epoch_cells = [], []
        for s in range(n_sta):
            sc = sta_scale(s)
            for _ in range(cells_per_sta):
                rows = [abs(rng.gauss(0.0, sc)) for _ in range(n_c)]
                pool.extend(rows)
                epoch_cells.append((max(rows), s))
        s0 = max(1.0, 1.4826 * median(pool))
        out.extend((c, s0, s) for c, s in epoch_cells)
    return out


def _selftest() -> None:
    """Synthetic pools with known structure — the instrument must tell them apart."""
    rng = random.Random(4242)
    n_c, n_sta, n_ep, per_sta = _DEF_NC, 24, 60, 8
    mref = median_max_abs_normal(n_c)
    mc = median(sorted(max(abs(rng.gauss(0, 1)) for _ in range(n_c)) for _ in range(200_000)))
    ok = abs(mref - mc) < 0.01
    print(f"median of max_{n_c}|N(0,1)| = {mref:.4f} closed-form vs {mc:.4f} Monte-Carlo "
          f"-> {'ok' if ok else 'FAIL'}")

    for name, f_scale in (("homogeneous", lambda s: 1.0),
                          ("station-heterogeneous 5x", lambda s: 1.0 + 4.0 * s / (n_sta - 1))):
        data = _simulate_pool(rng, n_sta, n_ep, n_c, per_sta, f_scale)
        shipped = [c / s0 for c, s0, _ in data]
        rate = sum(1 for x in shipped if x > 6) / len(shipped)

        g = defaultdict(list)
        for c, _, s in data:
            g[s].append(c)
        sc = {s: median(v) / mref for s, v in g.items()}
        strat = [c / sc[s] for c, _, s in data]
        rate2 = sum(1 for x in strat if x > 6) / len(strat)

        def _c(v, k):
            r = sum(1 for x in v if x > k) / len(v)
            return implied_scale(r, k) if r > 0 else float("nan")

        print(f"  {name:26s} shipped flag@6 {rate:8.4%} | station-stratified {rate2:8.4%}"
              f" | implied c(3)->c(6): {_c(shipped,3):.2f} -> {_c(shipped,6):.2f}")

    print("\n  expected: the homogeneous pool flags ~0 either way with a FLAT implied c;")
    print("  the heterogeneous pool flags heavily as shipped, collapses under stratification,")
    print("  and shows a RISING implied-c curve -- the shape-not-scale signature.")
    print(f"\n  corrected k for alpha=1e-6, n_c={n_c}: "
          f"m=1400 -> {corrected_k(1e-6, n_c, 1400):.3f}, "
          f"m=30 -> {corrected_k(1e-6, n_c, 30):.3f}   "
          f"(exact null, sigma-hat == 1: {exact_k(1e-6, n_c):.3f})")
    print("  -> all three sit near 5.  The floor max(1, .) truncates sigma-hat from below,")
    print("     so under a NOMINAL null the shipped test is already CONSERVATIVE: k = 6 is")
    print(f"     an alpha_cell = {cell_alpha(6.0, n_c):.3e} test, stricter than 1e-6.")
    print("     A threshold that is already too strict cannot be the cause of a 1e7x excess.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", type=Path, help="diagnose one run")
    ap.add_argument("--fit", type=Path, help="run to fit stratum scales on")
    ap.add_argument("--verify", type=Path, help="independent run to apply them to")
    ap.add_argument("--warmup-s", type=float, default=1800.0,
                    help="drop this many seconds of ignition (default 1800)")
    ap.add_argument("--n-c", type=int, default=_DEF_NC,
                    help="phase rows per cell (GPS triple-frequency uncombined: 3)")
    ap.add_argument("--strata", default="station,epoch",
                    help="comma-separated, applied in order (station,epoch,satellite)")
    ap.add_argument("--sweeps", type=int, default=4,
                    help="median-polish sweeps over the strata (default 4); the "
                         "strata are not orthogonal, so one pass under-fits")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        _selftest()
        return 0
    strata = [s.strip() for s in a.strata.split(",") if s.strip()]
    if a.run:
        diagnose(a.run, a.warmup_s, a.n_c, strata, a.sweeps)
    if a.fit and a.verify:
        transfer(a.fit, a.verify, a.warmup_s, a.n_c, strata, a.sweeps)
    elif a.fit or a.verify:
        sys.exit("ERROR: --fit and --verify go together")
    if not (a.run or a.fit):
        ap.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
