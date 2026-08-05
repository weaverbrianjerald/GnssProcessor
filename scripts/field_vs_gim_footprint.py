#!/usr/bin/env python3
"""Compare the network's SH-VTEC field with the GIM GLOBALLY and over its FOOTPRINT.

Why this exists.  `scripts/gim_global_mean_series.py` compares a00 with the GIM's
global mean.  That is a comparison of two GLOBAL means — and ours is not a
measurement over the globe: a 61-station, Europe-weighted network constrains the
field only where it has pierce points, and the rest is Kaula-extrapolated.  A
global-mean residual therefore mixes "our field is wrong where we look" with "our
extrapolation is wrong where we cannot look", and those are different cards.

This separates them.  It synthesizes the estimated field from
`logs/network_vtec_sh.csv` (the coefficients ARE the product — `network_ionosphere.csv`
is frame-broken), samples the time-interpolated IONEX on the same grid, and reports
the cos-latitude-weighted mean difference

  * GLOBALLY — reproduces the a00 − GIM_mean number, and
  * over the FOOTPRINT: grid points within `--radius` degrees of any station.  At a
    450 km shell the pierce point sits ~13 deg from the station at el 20 deg and
    ~18 deg at el 10 deg, so 15 deg is a fair proxy for "where we actually observe".

The basis here is a copy of `scripts/sh_vtec.py`'s port of
`SphericalHarmonicIonosphere` (normalization N_nm = sqrt[(2-d_0m)(2n+1)(n-m)!/(n+m)!],
no Condon-Shortley, so Pbar_00 = 1 and C_00 IS the mean VTEC).  sh_vtec.py remains
the reference implementation and carries the `--selftest`; it is duplicated rather
than imported only because it requires pandas, which this environment lacks.

Usage
  python scripts/field_vs_gim_footprint.py <run_dir> [<run_dir> ...]
      [--config config/test_suite/_ext63_gps_dual_1h.json]
      [--radius 15] [--every 240] [--ionex PATH]
"""
from __future__ import annotations

import argparse
import csv as csvmod
import glob
import json
import math
import os
import sys
from math import lgamma

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gim_global_mean_series import load_ionex          # noqa: E402

WGS84_A, WGS84_E2 = 6378137.0, 6.69437999014e-3
_GPS_EPOCH_JD = 2444244.5


# --------------------------------------------------------------- SH synthesis
def norm_factor(n: int, m: int) -> float:
    d = 1.0 if m == 0 else 2.0
    return float(np.sqrt(d * (2 * n + 1) * np.exp(lgamma(n - m + 1) - lgamma(n + m + 1))))


def legendre_normalized(x: np.ndarray, nmax: int) -> dict:
    x = np.asarray(x, float)
    s = np.sqrt(np.maximum(0.0, 1.0 - x * x))
    p = {(0, 0): np.ones_like(x)}
    for m in range(1, nmax + 1):
        p[(m, m)] = (2.0 * m - 1.0) * s * p[(m - 1, m - 1)]
    for m in range(0, nmax + 1):
        if m + 1 <= nmax:
            p[(m + 1, m)] = (2.0 * m + 1.0) * x * p[(m, m)]
        for n in range(m + 2, nmax + 1):
            p[(n, m)] = ((2.0 * n - 1.0) * x * p[(n - 1, m)]
                         - (n + m - 1.0) * p[(n - 2, m)]) / (n - m)
    return {k: v * norm_factor(*k) for k, v in p.items()}


def mean_sun_longitude(week: int, tow: float) -> float:
    jd = _GPS_EPOCH_JD + week * 7.0 + tow / 86400.0
    n = jd - 2451545.0
    L = np.deg2rad((280.460 + 0.9856474 * n) % 360.0)
    g = np.deg2rad((357.528 + 0.9856003 * n) % 360.0)
    lam = L + np.deg2rad(1.915) * np.sin(g) + np.deg2rad(0.020) * np.sin(2.0 * g)
    eps = np.deg2rad(23.439 - 4.0e-7 * n)
    ra = np.arctan2(np.cos(eps) * np.sin(lam), np.cos(lam))
    gmst = np.deg2rad((280.46061837 + 360.98564736629 * n) % 360.0)
    return float((ra - gmst + np.pi) % (2.0 * np.pi) - np.pi)


def synth(rows, week, tow, LAT_rad, LON_rad):
    """rows: list of (layer, n, m, trig, value).  Shells add (a vertical column
    pierces every shell overhead)."""
    s = LON_rad - mean_sun_longitude(week, tow)
    out = np.zeros_like(LAT_rad)
    for layer in sorted({r[0] for r in rows}):
        sub = [r for r in rows if r[0] == layer]
        nmax = max(r[1] for r in sub)
        pbar = legendre_normalized(np.sin(LAT_rad), nmax)
        for _l, n, m, trig, val in sub:
            ang = np.cos(m * s) if trig == "C" else np.sin(m * s)
            out = out + val * pbar[(n, m)] * ang
    return out


def load_sh(csv_path):
    """tow -> (week, [(layer, n, m, trig, value), ...])."""
    by_tow = {}
    with open(csv_path) as f:
        for r in csvmod.DictReader(f):
            try:
                tow = float(r["tow"])
                rec = (int(r.get("layer", 0) or 0), int(r["n"]), int(r["m"]),
                       r["trig"].strip(), float(r["value_tecu"]))
            except (TypeError, ValueError, KeyError):
                continue
            by_tow.setdefault(tow, (int(r["week"]), []))[1].append(rec)
    return by_tow


# --------------------------------------------------------------- geometry / config
def ecef_to_geodetic(x, y, z):
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1.0 - WGS84_E2))
    for _ in range(5):
        n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(lat) ** 2)
        lat = math.atan2(z + WGS84_E2 * n * math.sin(lat), p)
    return math.degrees(lat), math.degrees(lon)


def resolve_config(path):
    """Merge an `extends` chain base-first, child wins (the loader's own rule)."""
    d = json.load(open(path, encoding="utf-8"))
    out = {}
    for e in (d.get("extends") or []):
        base = resolve_config(e)
        for k, v in base.items():
            if isinstance(v, dict) and isinstance(out.get(k), dict):
                out[k].update(v)
            else:
                out[k] = v
    for k, v in d.items():
        if k in ("extends", "_comment"):
            continue
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = {**out[k], **v}
        else:
            out[k] = v
    return out


def station_latlon(config_path):
    cfg = resolve_config(config_path)
    return [ecef_to_geodetic(*s["position_ecef"])
            for s in cfg.get("network", {}).get("stations", [])
            if len(s.get("position_ecef", [])) == 3]


# --------------------------------------------------------------- GIM sampling
def gim_sampler(maps):
    series = sorted(maps)

    def sample_one(grid, lat, lon):
        lats = np.array(sorted(grid.keys()))
        lon = (lon + 180.0) % 360.0 - 180.0
        out = np.zeros_like(lat, dtype=float)
        i = np.clip(np.searchsorted(lats, lat) - 1, 0, len(lats) - 2)
        f_lat = (lat - lats[i]) / np.where(lats[i + 1] == lats[i], 1.0, lats[i + 1] - lats[i])
        for k, wk in ((0, 1.0 - f_lat), (1, f_lat)):
            la = lats[i + k]
            vals = np.zeros_like(lat, dtype=float)
            for u in np.unique(la):
                lon1, dlon, row = grid[float(u)]
                row = np.asarray(row, float)
                idx = (lon - lon1) / dlon
                j0 = np.clip(np.floor(idx).astype(int), 0, len(row) - 2)
                fr = np.clip(idx - j0, 0.0, 1.0)
                sel = (la == u)
                vals[sel] = ((1 - fr) * row[j0] + fr * row[j0 + 1])[sel]
            out += wk * vals
        return out

    def f(sod, lat, lon):
        sods = [s for s, _ in series]
        k = int(np.clip(np.searchsorted(sods, sod) - 1, 0, len(series) - 2))
        (t0, g0), (t1, g1) = series[k], series[k + 1]
        w = 0.0 if t1 == t0 else float(np.clip((sod - t0) / (t1 - t0), 0.0, 1.0))
        return (1 - w) * sample_one(g0, lat, lon) + w * sample_one(g1, lat, lon)

    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--config",
                    default=r"C:\GnssProcessor\config\test_suite\_ext63_gps_dual_1h.json")
    ap.add_argument("--radius", type=float, default=15.0)
    ap.add_argument("--every", type=int, default=240)
    ap.add_argument("--ionex", default=None)
    ap.add_argument("--quiet", action="store_true", help="summary rows only")
    args = ap.parse_args()

    ionex = args.ionex or glob.glob(
        r"C:\GnssProcessor\data\2024-129\products\*GIM.INX")[0]
    gim = gim_sampler(load_ionex(ionex))

    stations = station_latlon(args.config)
    lats = np.arange(-87.5, 87.6, 2.5)
    lons = np.arange(-180.0, 180.0, 5.0)
    LON, LAT = np.meshgrid(lons, lats)
    LON_r, LAT_r = np.radians(LON), np.radians(LAT)
    W = np.cos(LAT_r)

    d = np.full(LAT.shape, 1e9)
    for slat, slon in stations:
        dlon = np.radians(((LON - slon + 180.0) % 360.0) - 180.0)
        a = (np.sin(np.radians(LAT - slat) / 2) ** 2
             + np.cos(np.radians(slat)) * np.cos(LAT_r) * np.sin(dlon / 2) ** 2)
        d = np.minimum(d, np.degrees(2 * np.arcsin(np.sqrt(np.clip(a, 0, 1)))))
    mask = d <= args.radius
    print(f"stations {len(stations)} | footprint radius {args.radius:.0f} deg = "
          f"{100*(W*mask).sum()/W.sum():.1f} % of the sphere by area")
    print()

    for run in args.runs:
        path = os.path.join(run, "logs", "network_vtec_sh.csv")
        if not os.path.exists(path):
            print(f"{os.path.basename(run):26} no network_vtec_sh.csv")
            continue
        by_tow = load_sh(path)
        tows = sorted(by_tow)[:: args.every]
        print(os.path.basename(run))
        if not args.quiet:
            print(f"{'tow':>10} {'a00':>7} | {'ours_fp':>8} {'gim_fp':>7} {'d_fp':>7} "
                  f"| {'ours_gl':>8} {'gim_gl':>7} {'d_gl':>7}")
        fp, gl = [], []
        for tow in tows:
            week, rows = by_tow[tow]
            ours = synth(rows, week, tow, LAT_r, LON_r)
            theirs = gim((tow - 259200.0) % 86400.0, LAT, LON)
            a00 = next((v for l, n, m, t, v in rows
                        if l == 0 and n == 0 and m == 0 and t == "C"), float("nan"))
            o_fp = float((W * mask * ours).sum() / (W * mask).sum())
            g_fp = float((W * mask * theirs).sum() / (W * mask).sum())
            o_gl = float((W * ours).sum() / W.sum())
            g_gl = float((W * theirs).sum() / W.sum())
            fp.append(o_fp - g_fp)
            gl.append(o_gl - g_gl)
            if not args.quiet:
                print(f"{tow:10.0f} {a00:7.2f} | {o_fp:8.2f} {g_fp:7.2f} {o_fp-g_fp:+7.2f} "
                      f"| {o_gl:8.2f} {g_gl:7.2f} {o_gl-g_gl:+7.2f}")
        if fp:
            print(f"{'MEAN':>10} {'':>7} | {'':>8} {'':>7} {np.mean(fp):+7.2f} "
                  f"| {'':>8} {'':>7} {np.mean(gl):+7.2f}"
                  f"   (rms fp {np.sqrt(np.mean(np.square(fp))):.2f}, "
                  f"gl {np.sqrt(np.mean(np.square(gl))):.2f})")
        print()


if __name__ == "__main__":
    main()
