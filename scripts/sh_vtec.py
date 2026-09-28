#!/usr/bin/env python3
"""Synthesize the network's spherical-harmonic VTEC field from its coefficients.

A faithful Python port of ``SphericalHarmonicIonosphere`` (basis + normalization)
and ``meanSunLongitude`` from ``src/core/atmosphere/SphericalHarmonicIonosphere.cpp``,
so a figure can render the field the producer actually estimated:

    VTEC(beta, s) = sum_n sum_m Pbar_nm(sin beta) * [C_nm cos(m s) + S_nm sin(m s)]

with ``beta`` the geographic latitude, ``s = lon - lon_meansun`` the sun-fixed
longitude, and ``Pbar_nm`` normalized by
``N_nm = sqrt[(2 - d_0m)(2n+1)(n-m)!/(n+m)!]`` (no Condon-Shortley phase), which
makes ``Pbar_00 = 1`` so the ``C_00`` coefficient is the mean VTEC in TECU.

Why this exists: ``network_ionosphere.csv`` is not the absolute slant ionosphere.
``NetworkIonoDiag.cpp:92`` emits ``x_r`` alone, and the per-LOS iono state is
seeded at allocation by ``computeIonosphereSeed`` (``MeasurementModel.cpp:404``,
bounded +-200 m), so its linearization point is non-zero and the trace is a
correction, not an absolute value.  Measured on a converged 61-station run it
medians ~10 TECU against the field's own ~30 TECU a00, with 37 % of values
negative.  The coefficients in ``network_vtec_sh.csv`` are the unambiguous
product; use them.

Self-test:  python scripts/sh_vtec.py --selftest
"""
from __future__ import annotations

import argparse
from math import lgamma

import numpy as np
import pandas as pd

_GPS_EPOCH_JD = 2444244.5      # 1980-01-06 00:00:00 UTC


def norm_factor(n: int, m: int) -> float:
    """N_nm = sqrt[(2 - delta_0m)(2n+1)(n-m)!/(n+m)!] via log-gamma (n up to ~50)."""
    d = 1.0 if m == 0 else 2.0
    log_ratio = lgamma(n - m + 1) - lgamma(n + m + 1)
    return float(np.sqrt(d * (2 * n + 1) * np.exp(log_ratio)))


def legendre_normalized(x: np.ndarray, nmax: int) -> dict:
    """Pbar_nm(x) for 0<=m<=n<=nmax; x = sin(lat).  Same recurrences as the C++."""
    x = np.asarray(x, float)
    s = np.sqrt(np.maximum(0.0, 1.0 - x * x))
    p: dict = {(0, 0): np.ones_like(x)}
    for m in range(1, nmax + 1):                      # sectorial
        p[(m, m)] = (2.0 * m - 1.0) * s * p[(m - 1, m - 1)]
    for m in range(0, nmax + 1):                      # tesseral / zonal
        if m + 1 <= nmax:
            p[(m + 1, m)] = (2.0 * m + 1.0) * x * p[(m, m)]
        for n in range(m + 2, nmax + 1):
            p[(n, m)] = ((2.0 * n - 1.0) * x * p[(n - 1, m)]
                         - (n + m - 1.0) * p[(n - 2, m)]) / (n - m)
    return {(n, m): p[(n, m)] * norm_factor(n, m)
            for n in range(nmax + 1) for m in range(n + 1)}


def mean_sun_longitude(week: int, tow: float) -> float:
    """Sub-solar geographic longitude (rad, east +).  Port of meanSunLongitude."""
    jd = _GPS_EPOCH_JD + week * 7.0 + tow / 86400.0
    n = jd - 2451545.0
    L = np.deg2rad((280.460 + 0.9856474 * n) % 360.0)
    g = np.deg2rad((357.528 + 0.9856003 * n) % 360.0)
    lam = L + np.deg2rad(1.915) * np.sin(g) + np.deg2rad(0.020) * np.sin(2.0 * g)
    eps = np.deg2rad(23.439 - 4.0e-7 * n)
    ra = np.arctan2(np.cos(eps) * np.sin(lam), np.cos(lam))
    gmst = np.deg2rad((280.46061837 + 360.98564736629 * n) % 360.0)
    return float((ra - gmst + np.pi) % (2.0 * np.pi) - np.pi)


def synth_grid(coeffs: pd.DataFrame, week: int, tow: float,
               lats: np.ndarray, lons: np.ndarray) -> np.ndarray:
    """VTEC (TECU) on a lat x lon grid, summed over shells.

    ``coeffs`` holds one epoch's rows (layer, n, m, trig, value_tecu).  A vertical
    column pierces every shell directly overhead, so each layer is evaluated at
    the same geographic point and the shells add.
    """
    lon_sun = mean_sun_longitude(week, tow)
    LON, LAT = np.meshgrid(np.radians(lons), np.radians(lats))
    s = LON - lon_sun                                   # sun-fixed longitude
    out = np.zeros_like(LAT)
    for _, layer in coeffs.groupby("layer"):
        nmax = int(layer["n"].max())
        pbar = legendre_normalized(np.sin(LAT), nmax)
        for _, r in layer.iterrows():
            n, m = int(r["n"]), int(r["m"])
            ang = np.cos(m * s) if r["trig"] == "C" else np.sin(m * s)
            out = out + float(r["value_tecu"]) * pbar[(n, m)] * ang
    return out


def load_epoch(csv_path, tow=None) -> tuple[pd.DataFrame, int, float]:
    """Load one epoch of coefficients (default: the last COMPLETE one).

    Trace writers in this tree are not all line-flushed, so the final line of a
    live or crash-truncated file can be partial.  Drop incomplete rows, then
    require the chosen epoch to carry the modal coefficient count: a short
    epoch would silently synthesize a truncated field.
    """
    df = pd.read_csv(csv_path).dropna(subset=["n", "m", "trig", "value_tecu"])
    if df.empty:
        raise SystemExit(f"no usable SH coefficient rows in {csv_path}")
    # A single-shell run (layer2_max_degree = 0) omits the `layer` column entirely
    # rather than writing a constant 0, so the schema differs between postures.
    if "layer" not in df.columns:
        df = df.assign(layer=0)
    df = df.astype({"n": int, "m": int, "layer": int})
    per_epoch = df.groupby("tow").size()
    full = int(per_epoch.mode().iloc[0])
    complete = per_epoch[per_epoch == full].index
    if len(complete) == 0:
        raise SystemExit(f"no complete SH epoch in {csv_path}")
    t = float(max(complete)) if tow is None else float(tow)
    sel = df[df["tow"] == t]
    if len(sel) != full:
        raise SystemExit(f"SH epoch tow={t} has {len(sel)} coeffs, expected {full}")
    return sel, int(sel["week"].iloc[0]), t


# --------------------------------------------------------------------------- #
def _selftest() -> None:
    """Pbar_00 == 1, orthonormality under (1/4pi)*integral dOmega, C00 == field mean."""
    lats = np.linspace(-89.5, 89.5, 360)
    lons = np.linspace(-179.5, 179.5, 720)
    LAT = np.radians(lats)[:, None] * np.ones((1, lons.size))
    w = np.cos(LAT)                                      # dOmega weight

    pbar = legendre_normalized(np.sin(LAT), 8)
    assert np.allclose(pbar[(0, 0)], 1.0), "Pbar_00 must be 1"

    LON = np.radians(lons)[None, :] * np.ones((lats.size, 1))
    for (n, m), val in pbar.items():
        for trig in ("C", "S"):
            if m == 0 and trig == "S":
                continue
            f = val * (np.cos(m * LON) if trig == "C" else np.sin(m * LON))
            mean_sq = np.sum(f * f * w) / np.sum(w)
            assert abs(mean_sq - 1.0) < 2e-3, f"({n},{m},{trig}) norm {mean_sq:.5f}"
    print("orthonormality OK for all (n,m,trig) up to degree 8")

    # A pure C00 field must synthesize to a uniform field equal to C00.
    c = pd.DataFrame([{"layer": 0, "n": 0, "m": 0, "trig": "C", "value_tecu": 27.5}])
    g = synth_grid(c, 2313, 259200.0, lats, lons)
    assert np.allclose(g, 27.5), "C00-only field must be uniform at C00"
    print("C00-only field synthesizes uniformly to 27.5 TECU")

    # tow 259200 = 3.0 days into GPS week 2313 = 2024-05-08 00:00 GPS ~ 00:00 UTC,
    # so the sub-solar point sits near the antimeridian (local noon there when it
    # is midnight at Greenwich).
    lon_sun = np.degrees(mean_sun_longitude(2313, 259200.0))
    assert abs(abs(lon_sun) - 180.0) < 3.0, f"sub-solar longitude {lon_sun:.2f} deg"
    print(f"mean-sun longitude at 2024-05-08 00:00 GPS = {lon_sun:.2f} deg "
          f"(expected near +-180)")
    print("SELFTEST PASS")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        _selftest()
    else:
        ap.print_help()
