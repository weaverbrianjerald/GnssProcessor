#!/usr/bin/env python3
"""AB1-0: trace-only prototype of the post-solve SH-VTEC field as a consumer product.

No C++ change, no processor run.  Reads the baseline pair's traces only and fits a
post-solve spherical-harmonic VTEC field to the network's absolute slant ionosphere
``iono_total_m``, then grades the field evaluated at the held-out consumer's lines of
sight.  The decision rule under evaluation is frozen in
``docs/refound/AB1_sh_consumer_product_2026-09-20.md`` section 3.

Model
    iono_total(r, s, t) = mf(e) * TECU_TO_L1M * sum_k a_k(t) Ybar_k(ipp, sun-fixed)
                          + b_s + b_r ,        sum_s b_s = 0

    a_k in TECU; b_s, b_r constant in time, in metres of L1 slant.

Conventions (ported, not re-derived)
    basis / normalization / sun-fixed longitude : scripts/sh_vtec.py (self-tested,
        itself a port of src/core/atmosphere/SphericalHarmonicIonosphere.cpp)
    coefficient enumeration : SphericalHarmonicIonosphere.cpp:92-95
        (per degree: C(n,0..n) then S(n,1..n)) -- so degree N is the FIRST (N+1)^2
        columns of the degree-8 enumeration and the basis is built once.
    IPP geometry / thin-shell mapping : SphericalHarmonicIonosphere.cpp:19-53
    TECU -> L1 metres : ApplicationNetworkAssembly.cpp:1056-1057 (40.3e16 / f_L1^2)
    shell 450 km, Re 6371 km : GnssConfigTypes.hpp:3939 (global_vtec_sh defaults)

Scope of the V2 elevation mask
    The mask (15 or 25 deg) gates the FIT INPUT only.  The product is always
    evaluated at every consumer line of sight the consumer trace carries, so the
    15-30 deg grading band stays populated and comparable across mask variants.
    ``network_ionosphere.csv`` itself already carries only el >= 15 deg (the run's
    ``processing.elevation_mask_deg``), so mask 15 means "every traced row".

Usage
    python scripts/ab1_sh_product_proto.py <run_dir> --bed {field,sim} --out <dir>
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh_vtec import legendre_normalized, mean_sun_longitude  # noqa: E402

# --------------------------------------------------------------------------- #
# constants, all sourced above
F_L1 = 1575.42e6
TECU_TO_L1M = 40.3e16 / (F_L1 * F_L1)          # 0.162372... m of L1 slant per TECU
RE_M = 6371000.0
SHELL_M = 450000.0
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563

SIM_TRUTH = r"C:\GnssProcessor\runs\simnet60_quiet_wu93_24h"
WITHHOLD = {"field": "WTZS00DEU", "sim": None}   # sim: nearest station, resolved at load
CONSUMER_GEOM_STATION = {"field": "WTZR00DEU", "sim": None}
REGIONAL_RADIUS_KM = 1500.0


# --------------------------------------------------------------------------- #
# geometry -- ports of Coordinates::ecefToLlh and computeIppGeometry
def ecef_to_llh(xyz: np.ndarray):
    """WGS84 geodetic lat/lon (rad) + height.  Port of Coordinates.cpp:9-50."""
    xyz = np.atleast_2d(np.asarray(xyz, float))
    e2 = WGS84_F * (2.0 - WGS84_F)
    x, y, z = xyz[:, 0], xyz[:, 1], xyz[:, 2]
    p = np.hypot(x, y)
    lon = np.arctan2(y, x)
    lat = np.arctan2(z, p * (1.0 - e2))
    h = np.zeros_like(lat)
    for _ in range(10):
        sl = np.sin(lat)
        N = WGS84_A / np.sqrt(1.0 - e2 * sl * sl)
        h = p / np.cos(lat) - N
        lat = np.arctan2(z, p * (1.0 - e2 * (N / (N + h))))
    return lat, lon, h


def ipp_from_elaz(rx_lat, rx_lon, el_rad, az_rad):
    """Thin-shell mapping and IPP lat/lon.  Port of computeIppGeometry:31-51.

    Takes elevation/azimuth from the trace rather than recomputing them from ECEF;
    the C++ derives the same two angles at :27-28 and the rest of the routine uses
    only (el, az, rx_lat, rx_lon).
    """
    sin_zp = (RE_M / (RE_M + SHELL_M)) * np.cos(el_rad)
    cos_zp = np.sqrt(np.maximum(0.0, 1.0 - sin_zp * sin_zp))
    mapping = 1.0 / cos_zp
    psi = 0.5 * np.pi - el_rad - np.arcsin(np.clip(sin_zp, -1.0, 1.0))
    lat = np.arcsin(np.clip(np.sin(rx_lat) * np.cos(psi)
                            + np.cos(rx_lat) * np.sin(psi) * np.cos(az_rad), -1.0, 1.0))
    lon = rx_lon + np.arcsin(np.clip(np.sin(psi) * np.sin(az_rad)
                                     / np.maximum(1e-9, np.cos(lat)), -1.0, 1.0))
    return mapping, lat, lon


def coeff_order(nmax: int):
    """SphericalHarmonicIonosphere.cpp:92-95 -- per degree, C block then S block."""
    out = []
    for n in range(nmax + 1):
        out += [(n, m, "C") for m in range(n + 1)]
        out += [(n, m, "S") for m in range(1, n + 1)]
    return out


def basis_matrix(ipp_lat, lon_sf, nmax: int) -> np.ndarray:
    """(nrows, (nmax+1)^2) of Pbar_nm(sin lat) * {cos,sin}(m s), in coeff_order."""
    pbar = legendre_normalized(np.sin(ipp_lat), nmax)
    cols = []
    for (n, m, trig) in coeff_order(nmax):
        ang = np.cos(m * lon_sf) if trig == "C" else np.sin(m * lon_sf)
        cols.append(pbar[(n, m)] * ang)
    return np.column_stack(cols)


def degree_of_column(nmax: int) -> np.ndarray:
    return np.array([n for (n, _m, _t) in coeff_order(nmax)], float)


# --------------------------------------------------------------------------- #
# loading
def load_bed(run_dir: str, bed: str) -> dict:
    arm = json.load(open(os.path.join(run_dir, "arm.json")))
    net = arm["network"]
    sta_pos = {s["name"]: np.asarray(s["position_ecef"], float) for s in net["stations"]}
    cl = net["closed_loop"]["station"]
    cons_name, cons_ecef = cl["name"], np.asarray(cl["position_ecef"], float)

    ni = pd.read_csv(os.path.join(run_dir, "logs", "network_ionosphere.csv"))
    ni = ni[ni["station"].isin(sta_pos)].copy()
    ni.sort_values(["tow", "station", "sat"], kind="stable", inplace=True)
    ni.reset_index(drop=True, inplace=True)

    cs = pd.read_csv(os.path.join(run_dir, "logs", "closed_loop_sat_trace.csv"))
    cs = cs.drop_duplicates(["tow", "sat"]).sort_values(["tow", "sat"], kind="stable")
    cs = cs[["week", "tow", "sat", "elev_deg", "iono_m", "iono_var"]].reset_index(drop=True)

    # consumer line-of-sight geometry
    truth = None
    if bed == "sim":
        tp = os.path.join(SIM_TRUTH, cons_name, "truth_atmo.csv")
        t = pd.read_csv(tp)[["tow_s", "satellite", "elev_deg", "az_deg", "I_L1_m"]]
        t = t.rename(columns={"tow_s": "tow", "satellite": "sat",
                              "elev_deg": "g_elev", "az_deg": "g_az"})
        cs = cs.merge(t, on=["tow", "sat"], how="left")
        truth = tp
        geom_src = f"{cons_name}/truth_atmo.csv (elev diff to consumer trace: "
    else:
        gs = CONSUMER_GEOM_STATION[bed]
        w = ni[ni["station"] == gs][["tow", "sat", "elevation_deg", "azimuth_deg"]]
        w = w.rename(columns={"elevation_deg": "g_elev", "azimuth_deg": "g_az"})
        cs = cs.merge(w, on=["tow", "sat"], how="left")
        cs["I_L1_m"] = np.nan
        geom_src = f"{gs} rows of network_ionosphere.csv (elev diff to consumer trace: "
    n_all = len(cs)
    cs = cs.dropna(subset=["g_elev", "g_az"]).reset_index(drop=True)
    dmax = float(np.abs(cs["elev_deg"] - cs["g_elev"]).max())
    geom_src += f"max {dmax:.4f} deg, {len(cs)}/{n_all} rows matched)"

    # withheld station (out-of-sample check)
    if WITHHOLD[bed] is not None:
        wh = WITHHOLD[bed]
    else:
        d = {k: np.linalg.norm(v - cons_ecef) for k, v in sta_pos.items()}
        wh = min(d, key=d.get)

    t0 = float(ni["tow"].min())
    return dict(bed=bed, run_dir=run_dir, arm=arm, sta_pos=sta_pos, ni=ni, cs=cs,
                cons_name=cons_name, cons_ecef=cons_ecef, withhold=wh, t0=t0,
                geom_src=geom_src, truth=truth,
                week=int(ni["week"].iloc[0]))


def prepare(B: dict) -> None:
    """Precompute geometry, the sun-fixed frame and the degree-8 basis, once."""
    ni, cs = B["ni"], B["cs"]
    names = sorted(B["sta_pos"])
    B["stations"] = names
    sidx = {n: i for i, n in enumerate(names)}
    sats = sorted(set(ni["sat"]) | set(cs["sat"]))
    B["sats"] = sats
    tidx = {s: i for i, s in enumerate(sats)}

    lat = np.array([ecef_to_llh(B["sta_pos"][n])[0][0] for n in names])
    lon = np.array([ecef_to_llh(B["sta_pos"][n])[1][0] for n in names])
    B["sta_lat"], B["sta_lon"] = lat, lon

    ni["si"] = ni["station"].map(sidx).to_numpy()
    ni["ti"] = ni["sat"].map(tidx).to_numpy()
    el = np.radians(ni["elevation_deg"].to_numpy())
    az = np.radians(ni["azimuth_deg"].to_numpy())
    mf, ilat, ilon = ipp_from_elaz(lat[ni["si"].to_numpy()], lon[ni["si"].to_numpy()], el, az)

    tows = np.sort(ni["tow"].unique())
    B["tows"] = tows
    sun = {t: mean_sun_longitude(B["week"], float(t)) for t in tows}
    ni_sun = ni["tow"].map(sun).to_numpy()

    B["ni_mf"] = mf
    B["ni_ipplat"] = ilat
    B["ni_lonsf"] = ilon - ni_sun
    B["ni_el"] = el
    B["ni_y"] = ni["iono_total_m"].to_numpy()
    B["ni_tow"] = ni["tow"].to_numpy()
    B["ni_si"] = ni["si"].to_numpy()
    B["ni_ti"] = ni["ti"].to_numpy()
    B["ni_Y8"] = basis_matrix(ilat, B["ni_lonsf"], 8)

    # epoch row slices (ni is sorted by tow)
    B["ep_start"] = np.searchsorted(B["ni_tow"], tows, "left")
    B["ep_end"] = np.searchsorted(B["ni_tow"], tows, "right")

    # consumer geometry at the SAME shell
    clat, clon = ecef_to_llh(B["cons_ecef"])[0][0], ecef_to_llh(B["cons_ecef"])[1][0]
    cel = np.radians(cs["g_elev"].to_numpy())
    caz = np.radians(cs["g_az"].to_numpy())
    cmf, cilat, cilon = ipp_from_elaz(clat, clon, cel, caz)
    csun = cs["tow"].map(sun).to_numpy()
    B["cs_mf"] = cmf
    B["cs_ipplat"] = cilat
    B["cs_lonsf"] = cilon - csun
    B["cs_el_deg"] = cs["g_elev"].to_numpy()
    B["cs_tow"] = cs["tow"].to_numpy()
    B["cs_ti"] = cs["sat"].map(tidx).to_numpy()
    B["cs_Y8"] = basis_matrix(cilat, B["cs_lonsf"], 8)
    B["cs_iono"] = cs["iono_m"].to_numpy()
    B["cs_sig"] = np.sqrt(cs["iono_var"].to_numpy())
    B["cs_truth"] = cs["I_L1_m"].to_numpy()

    # the causal product is read at the consumer epoch by index, so the two epoch
    # grids must coincide exactly -- assert it rather than silently supplying a lag
    j = np.clip(np.searchsorted(tows, B["cs_tow"]), 0, len(tows) - 1)
    B["tow_align_max_s"] = float(np.abs(tows[j] - B["cs_tow"]).max())

    # distances consumer <-> network stations
    d = np.array([np.linalg.norm(B["sta_pos"][n] - B["cons_ecef"]) / 1000.0 for n in names])
    B["sta_dist_km"] = d
    B["near"] = np.where(d <= REGIONAL_RADIUS_KM)[0]


# --------------------------------------------------------------------------- #
# normal-equation blocks
def epoch_blocks(G, w, z, si, ti, nsta, nsat):
    """Per-epoch contributions of one node's design to the normal equations."""
    wG = w[:, None] * G
    S = np.zeros((len(w), nsta)); S[np.arange(len(w)), si] = 1.0
    T = np.zeros((len(w), nsat)); T[np.arange(len(w)), ti] = 1.0
    return dict(GtWG=G.T @ wG,
                GtWS=wG.T @ S,
                GtWT=wG.T @ T,
                StWT=S.T @ (w[:, None] * T),
                sw_s=np.bincount(si, w, nsta),
                sw_t=np.bincount(ti, w, nsat),
                GtWz=wG.T @ z,
                Swz=np.bincount(si, w * z, nsta),
                Twz=np.bincount(ti, w * z, nsat))


def knot_grid(t0, t1, step):
    n = int(round((t1 - t0) / step))
    return t0 + step * np.arange(n + 1)


def knot_weights(tow, knots, step):
    j = np.clip(np.searchsorted(knots, tow, "right") - 1, 0, len(knots) - 2)
    f = (tow - knots[j]) / step
    return int(j), float(f)


# --------------------------------------------------------------------------- #
# variant B -- batch, piecewise-linear coefficients on knots
def fit_batch(B, deg, mask_deg, use_bs, cfg, exclude=(), window=None):
    K = (deg + 1) ** 2
    nsta, nsat = len(B["stations"]), len(B["sats"])
    w0, w1 = window if window else (B["t0"] + 3600.0, B["t0"] + 21600.0)
    knots = knot_grid(w0, w1, cfg.knot_s)
    nk = len(knots)
    P = nk * K + nsta + (nsat if use_bs else 0)
    ORB, OSB = nk * K, nk * K + nsta

    sel = (np.degrees(B["ni_el"]) >= mask_deg - 1e-9) & \
          (B["ni_tow"] >= knots[0] - 1e-6) & (B["ni_tow"] <= knots[-1] + 1e-6)
    if len(exclude):
        sel &= ~np.isin(B["ni_si"], exclude)
    rows = np.where(sel)[0]
    if len(rows) == 0:
        return None

    # per-epoch slices restricted to the selected rows
    tows = np.unique(B["ni_tow"][rows])
    row_tow = B["ni_tow"][rows]
    st = np.searchsorted(row_tow, tows, "left")
    en = np.searchsorted(row_tow, tows, "right")

    G8 = B["ni_Y8"][rows][:, :K] * (B["ni_mf"][rows] * TECU_TO_L1M)[:, None]
    y = B["ni_y"][rows]
    si = B["ni_si"][rows]
    ti = B["ni_ti"][rows]
    w_el = np.sin(B["ni_el"][rows]) ** 2 / (cfg.sigma0 ** 2)

    degcol = degree_of_column(deg)
    ridge = np.empty(K)
    ridge[degcol == 0] = 1.0 / (cfg.a00_sigma ** 2)
    nz = degcol > 0
    ridge[nz] = 1.0 / ((cfg.kaula / degcol[nz]) ** 2)

    robust = np.ones(len(rows))
    cond_note = ""
    for it in range(cfg.irls + 1):
        w = w_el * robust
        N = np.zeros((P, P)); b = np.zeros(P)
        for e in range(len(tows)):
            a, z_ = st[e], en[e]
            if z_ <= a:
                continue
            j, f = knot_weights(tows[e], knots, cfg.knot_s)
            c = (1.0 - f, f)
            blk = epoch_blocks(G8[a:z_], w[a:z_], y[a:z_], si[a:z_], ti[a:z_], nsta, nsat)
            for u in (0, 1):
                if c[u] == 0.0:
                    continue
                su = slice((j + u) * K, (j + u + 1) * K)
                b[su] += c[u] * blk["GtWz"]
                N[su, ORB:ORB + nsta] += c[u] * blk["GtWS"]
                N[ORB:ORB + nsta, su] += c[u] * blk["GtWS"].T
                if use_bs:
                    N[su, OSB:OSB + nsat] += c[u] * blk["GtWT"]
                    N[OSB:OSB + nsat, su] += c[u] * blk["GtWT"].T
                for v in (0, 1):
                    if c[v] == 0.0:
                        continue
                    sv = slice((j + v) * K, (j + v + 1) * K)
                    N[su, sv] += c[u] * c[v] * blk["GtWG"]
            N[ORB:ORB + nsta, ORB:ORB + nsta] += np.diag(blk["sw_s"])
            b[ORB:ORB + nsta] += blk["Swz"]
            if use_bs:
                N[ORB:ORB + nsta, OSB:OSB + nsat] += blk["StWT"]
                N[OSB:OSB + nsat, ORB:ORB + nsta] += blk["StWT"].T
                N[OSB:OSB + nsat, OSB:OSB + nsat] += np.diag(blk["sw_t"])
                b[OSB:OSB + nsat] += blk["Twz"]

        if it == 0:
            cond_note = _cond_probe(N, P, K, nk, ORB, OSB, nsta, nsat, use_bs, ridge, cfg)
        Nr = N.copy()
        for k in range(nk):
            Nr[np.arange(k * K, (k + 1) * K), np.arange(k * K, (k + 1) * K)] += ridge
        Nr[np.arange(ORB, ORB + nsta), np.arange(ORB, ORB + nsta)] += 1.0 / cfg.b_sigma ** 2
        if use_bs:
            Nr[np.arange(OSB, OSB + nsat), np.arange(OSB, OSB + nsat)] += 1.0 / cfg.b_sigma ** 2
            C = np.zeros((1, P)); C[0, OSB:OSB + nsat] = 1.0   # gauge sum(b_s)=0, exact (KKT)
            KK = np.block([[Nr, C.T], [C, np.zeros((1, 1))]])
            rhs = np.concatenate([b, [0.0]])
            x = np.linalg.solve(KK, rhs)[:P]
        else:
            x = np.linalg.solve(Nr, b)

        pred = _predict_batch(x, G8, si, ti, row_tow, knots, cfg.knot_s, K, ORB, OSB,
                              nsta, nsat, use_bs)
        res = y - pred
        s = 1.4826 * np.median(np.abs(res - np.median(res)))
        if it < cfg.irls:
            robust = np.minimum(1.0, cfg.huber * max(s, 1e-6) / np.maximum(np.abs(res), 1e-9))

    a_knots = x[:nk * K].reshape(nk, K)
    br = x[ORB:ORB + nsta]
    bs = x[OSB:OSB + nsat] if use_bs else np.zeros(nsat)
    wrms = float(np.sqrt(np.sum(w_el * robust * res ** 2) / np.sum(w_el * robust)))
    return dict(kind="B", deg=deg, mask=mask_deg, bs_on=use_bs, knots=knots, a=a_knots,
                br=br, bs=bs, res_rms=float(np.sqrt(np.mean(res ** 2))), res_wrms=wrms,
                n_rows=len(rows), n_down=int(np.sum(robust < 0.999)), cond=cond_note,
                rows=rows, res=res, robust=robust, sigma_hat=float(s))


def _cond_probe(N, P, K, nk, ORB, OSB, nsta, nsat, use_bs, ridge, cfg):
    """Which regularizer the normal matrix actually requires, in order.

    L0 raw; L1 + the sum-to-zero gauge on b_s (the ONE exact null direction:
    b_s += c, b_r -= c leaves every modelled value unchanged); L2 + the loose
    b_r/b_s priors; L3 + the Kaula ridge on the coefficients.
    """
    def ok(M):
        try:
            np.linalg.cholesky(M)
            return True
        except np.linalg.LinAlgError:
            return False
    steps = [("raw", N)]
    M = N.copy()
    if use_bs:
        c = np.zeros(P); c[OSB:OSB + nsat] = 1.0
        M = M + np.outer(c, c) / cfg.gauge_sigma ** 2
        steps.append(("+gauge", M))
    M = M.copy()
    M[np.arange(ORB, P), np.arange(ORB, P)] += 1.0 / cfg.b_sigma ** 2
    steps.append(("+b_prior", M))
    M = M.copy()
    for k in range(nk):
        M[np.arange(k * K, (k + 1) * K), np.arange(k * K, (k + 1) * K)] += ridge
    steps.append(("+kaula", M))
    first = next((nm for nm, m in steps if ok(m)), "NONE")
    note = f"chol first OK at [{first}]"
    if P <= 700:
        ev = np.linalg.eigvalsh(steps[0][1])
        ev2 = np.linalg.eigvalsh(steps[-1][1])
        note += (f", eig max/min raw {ev.max()/max(abs(ev.min()),1e-300):.1e}"
                 f" -> reg {ev2.max()/max(ev2.min(),1e-300):.1e}")
    else:
        d = np.diag(N)
        note += f", diag max/min {d.max()/max(d.min(),1e-300):.1e}"
    return note


def _predict_batch(x, G, si, ti, tow, knots, step, K, ORB, OSB, nsta, nsat, use_bs):
    j = np.clip(np.searchsorted(knots, tow, "right") - 1, 0, len(knots) - 2)
    f = (tow - knots[j]) / step
    A = x[:len(knots) * K].reshape(len(knots), K)
    v = np.einsum("ij,ij->i", G, (1.0 - f)[:, None] * A[j] + f[:, None] * A[j + 1])
    v = v + x[ORB:ORB + nsta][si]
    if use_bs:
        v = v + x[OSB:OSB + nsat][ti]
    return v


# --------------------------------------------------------------------------- #
# variant C -- causal Kalman filter from epoch 0
def fit_causal(B, deg, mask_deg, use_bs, cfg, exclude=(), keep_state=False):
    K = (deg + 1) ** 2
    nsta, nsat = len(B["stations"]), len(B["sats"])
    P = K + nsta + (nsat if use_bs else 0)
    ORB, OSB = K, K + nsta
    degcol = degree_of_column(deg)

    x = np.zeros(P)
    x[0] = cfg.a00_prior
    p0 = np.empty(K)
    p0[degcol == 0] = cfg.a00_sigma_kf ** 2
    nz = degcol > 0
    p0[nz] = (cfg.kaula / degcol[nz]) ** 2
    Pc = np.diag(np.concatenate([p0, np.full(P - K, cfg.b_sigma ** 2)]))
    qa = np.zeros(K)
    qa[degcol == 0] = cfg.q_deg
    qa[nz] = cfg.q_deg / degcol[nz] ** 2
    q = np.concatenate([qa, np.zeros(P - K)])   # b_r / b_s are constants (q = 0)

    tows = B["tows"]
    a_t = np.zeros((len(tows), K))
    br_t = np.zeros((len(tows), nsta))
    bs_t = np.zeros((len(tows), nsat))
    n_gate = 0
    n_used = 0
    resid = []
    prev_t = None
    for e, t in enumerate(tows):
        a0, a1 = B["ep_start"][e], B["ep_end"][e]
        idx = np.arange(a0, a1)
        m = np.degrees(B["ni_el"][idx]) >= mask_deg - 1e-9
        if len(exclude):
            m &= ~np.isin(B["ni_si"][idx], exclude)
        idx = idx[m]
        dt = 30.0 if prev_t is None else float(t - prev_t)
        prev_t = t
        Pc = Pc + np.diag(q * dt)
        if len(idx):
            G = B["ni_Y8"][idx][:, :K] * (B["ni_mf"][idx] * TECU_TO_L1M)[:, None]
            si, ti = B["ni_si"][idx], B["ni_ti"][idx]
            y = B["ni_y"][idx]
            w = np.sin(B["ni_el"][idx]) ** 2 / cfg.sigma0 ** 2
            pred = G @ x[:K] + x[ORB:ORB + nsta][si] + (x[OSB:OSB + nsat][ti] if use_bs else 0.0)
            z = y - pred
            # innovation gate: diag(H P H^T) + R
            hph = np.einsum("ij,jk,ik->i", G, Pc[:K, :K], G)
            hph = hph + 2.0 * np.einsum("ij,ji->i", G, Pc[:K, ORB:ORB + nsta][:, si])
            hph = hph + Pc[ORB + si, ORB + si]
            if use_bs:
                hph = hph + 2.0 * np.einsum("ij,ji->i", G, Pc[:K, OSB:OSB + nsat][:, ti])
                hph = hph + Pc[OSB + ti, OSB + ti]
                hph = hph + 2.0 * Pc[ORB + si, OSB + ti]
            ok = np.abs(z) <= cfg.gate * np.sqrt(np.maximum(hph, 0.0) + 1.0 / w)
            n_gate += int(np.sum(~ok)); n_used += int(np.sum(ok))
            resid.append(z[ok])
            if np.any(ok):
                G, si, ti, z, w = G[ok], si[ok], ti[ok], z[ok], w[ok]
                blk = epoch_blocks(G, w, z, si, ti, nsta, nsat)
                N = np.zeros((P, P)); b = np.zeros(P)
                N[:K, :K] = blk["GtWG"]
                N[:K, ORB:ORB + nsta] = blk["GtWS"]
                N[ORB:ORB + nsta, :K] = blk["GtWS"].T
                N[ORB:ORB + nsta, ORB:ORB + nsta] = np.diag(blk["sw_s"])
                b[:K] = blk["GtWz"]; b[ORB:ORB + nsta] = blk["Swz"]
                if use_bs:
                    N[:K, OSB:OSB + nsat] = blk["GtWT"]
                    N[OSB:OSB + nsat, :K] = blk["GtWT"].T
                    N[ORB:ORB + nsta, OSB:OSB + nsat] = blk["StWT"]
                    N[OSB:OSB + nsat, ORB:ORB + nsta] = blk["StWT"].T
                    N[OSB:OSB + nsat, OSB:OSB + nsat] = np.diag(blk["sw_t"])
                    b[OSB:OSB + nsat] = blk["Twz"]
                Lam = np.linalg.inv(0.5 * (Pc + Pc.T)) + N
                Lam = 0.5 * (Lam + Lam.T)
                Pc = np.linalg.inv(Lam)
                Pc = 0.5 * (Pc + Pc.T)
                x = x + Pc @ b
        if use_bs:                       # gauge sum(b_s)=0 as a tight pseudo-observation
            h = np.zeros(P); h[OSB:OSB + nsat] = 1.0
            Ph = Pc @ h
            den = float(h @ Ph) + cfg.gauge_sigma ** 2
            x = x - Ph * (float(h @ x) / den)
            Pc = Pc - np.outer(Ph, Ph) / den
        a_t[e] = x[:K]
        br_t[e] = x[ORB:ORB + nsta]
        if use_bs:
            bs_t[e] = x[OSB:OSB + nsat]
    r = np.concatenate(resid) if resid else np.zeros(0)
    return dict(kind="C", deg=deg, mask=mask_deg, bs_on=use_bs, a_t=a_t, br_t=br_t,
                bs_t=bs_t, tows=tows, n_gate=n_gate, n_used=n_used,
                res_rms=float(np.sqrt(np.mean(r ** 2))) if len(r) else np.nan)


# --------------------------------------------------------------------------- #
# product evaluation at the consumer
def product_at_consumer(B, fit):
    K = (fit["deg"] + 1) ** 2
    Yc = B["cs_Y8"][:, :K] * (B["cs_mf"] * TECU_TO_L1M)[:, None]
    if fit["kind"] == "B":
        knots, step = fit["knots"], fit["knots"][1] - fit["knots"][0]
        t = B["cs_tow"]
        inside = (t >= knots[0] - 1e-6) & (t <= knots[-1] + 1e-6)
        j = np.clip(np.searchsorted(knots, t, "right") - 1, 0, len(knots) - 2)
        f = np.clip((t - knots[j]) / step, 0.0, 1.0)
        A = (1.0 - f)[:, None] * fit["a"][j] + f[:, None] * fit["a"][j + 1]
        v = np.einsum("ij,ij->i", Yc, A) + fit["bs"][B["cs_ti"]]
        v[~inside] = np.nan
        return v
    ti = np.searchsorted(fit["tows"], B["cs_tow"])
    ti = np.clip(ti, 0, len(fit["tows"]) - 1)
    A = fit["a_t"][ti]
    return np.einsum("ij,ij->i", Yc, A) + fit["bs_t"][ti][np.arange(len(ti)), B["cs_ti"]]


def demedian(tow, err):
    """Remove the per-epoch MEDIAN OF THE DIFFERENCE over the consumer's satellites."""
    s = pd.Series(err)
    return (s - s.groupby(pd.Series(tow)).transform("median")).to_numpy()


# --------------------------------------------------------------------------- #
# variant R -- regional per-satellite interpolation, causal
def product_regional(B, cfit, mask_deg, drop=()):
    near = np.array([i for i in B["near"] if i not in set(drop)], int)
    out = np.full(len(B["cs_tow"]), np.nan)
    nstn = np.zeros(len(B["cs_tow"]), int)
    if len(near) == 0:
        return out, nstn
    sel = np.isin(B["ni_si"], near) & (np.degrees(B["ni_el"]) >= mask_deg - 1e-9)
    idx = np.where(sel)[0]
    key = B["ni_tow"][idx] * 100 + B["ni_ti"][idx]
    order = np.argsort(key, kind="stable")
    idx, key = idx[order], key[order]
    ckey = B["cs_tow"] * 100 + B["cs_ti"]
    lo = np.searchsorted(key, ckey, "left")
    hi = np.searchsorted(key, ckey, "right")
    tmap = {t: i for i, t in enumerate(cfit["tows"])}
    w_d0 = 100.0
    for r in range(len(ckey)):
        rr = idx[lo[r]:hi[r]]
        if len(rr) == 0:
            continue
        nstn[r] = len(rr)
        e = tmap.get(B["cs_tow"][r])
        br = cfit["br_t"][e][B["ni_si"][rr]] if e is not None else 0.0
        v = (B["ni_y"][rr] - br) / B["ni_mf"][rr]          # vertical-equivalent L1 m
        la = B["ni_ipplat"][rr]; lo_ = B["ni_lonsf"][rr]
        dkm = B["sta_dist_km"][B["ni_si"][rr]]
        w = np.sin(B["ni_el"][rr]) ** 2 / (dkm ** 2 + w_d0 ** 2)
        cla, clo = B["cs_ipplat"][r], B["cs_lonsf"][r]
        if len(rr) >= 3:
            A = np.column_stack([np.ones(len(rr)), la - cla,
                                 (lo_ - clo) * np.cos(cla)])
            Nn = A.T @ (w[:, None] * A) + np.diag([1e-9, 1e-6, 1e-6])
            try:
                c = np.linalg.solve(Nn, A.T @ (w * v))
                vc = c[0]
            except np.linalg.LinAlgError:
                vc = float(np.sum(w * v) / np.sum(w))
        else:
            vc = float(np.sum(w * v) / np.sum(w))
        out[r] = vc * B["cs_mf"][r]
    return out, nstn


# --------------------------------------------------------------------------- #
# grading helpers
def rms(a):
    a = np.asarray(a, float); a = a[np.isfinite(a)]
    return float(np.sqrt(np.mean(a ** 2))) if len(a) else np.nan


def hour_of(tow, t0):
    return np.floor((tow - t0) / 3600.0).astype(int) + 1


ELEV_BANDS = [(15, 30), (30, 60), (60, 90)]


def band_of(el):
    b = np.full(len(el), -1)
    for i, (a, z) in enumerate(ELEV_BANDS):
        b[(el >= a) & (el < z + (0.01 if z == 90 else 0))] = i
    return b


def table_hour_band(tow, el, err, t0, hours=None, label=""):
    hr = hour_of(tow, t0)
    bd = band_of(el)
    hrs = sorted(set(hr)) if hours is None else hours
    lines = [f"  {label}", "    hour |      n     RMS_m  |  15-30    30-60    60-90"]
    for h in hrs:
        m = hr == h
        if not np.any(m & np.isfinite(err)):
            continue
        cells = [rms(err[m & (bd == i)]) for i in range(3)]
        lines.append(f"    {h:4d} | {int(np.sum(m & np.isfinite(err))):6d}  {rms(err[m]):7.3f}  | "
                     + "  ".join(f"{c:7.3f}" if np.isfinite(c) else "      -" for c in cells))
    m = np.isin(hr, hrs)
    cells = [rms(err[m & (bd == i)]) for i in range(3)]
    lines.append(f"     ALL | {int(np.sum(m & np.isfinite(err))):6d}  {rms(err[m]):7.3f}  | "
                 + "  ".join(f"{c:7.3f}" if np.isfinite(c) else "      -" for c in cells))
    return "\n".join(lines)


def arcs(tow, ti, step=30.0):
    """Arc id and age (epochs since the consumer first tracks that satellite)."""
    age = np.zeros(len(tow), int)
    born = np.zeros(len(tow))
    order = np.lexsort((tow, ti))
    k = 0
    prev_s, prev_t, t_birth = -1, -1e18, 0.0
    for i in order:
        if ti[i] != prev_s or tow[i] - prev_t > 1.5 * step:
            t_birth = tow[i]; k = 0
        else:
            k = int(round((tow[i] - t_birth) / step))
        age[i] = k; born[i] = t_birth
        prev_s, prev_t = ti[i], tow[i]
    return age, born


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--bed", required=True, choices=["field", "sim"])
    ap.add_argument("--out", default=r"C:\GnssProcessor\runs\_ab1_proto_20260920")
    ap.add_argument("--degrees", default="4,6,8")
    ap.add_argument("--masks", default="15,25")
    ap.add_argument("--bs", default="1,0", help="with,without the per-satellite bias")
    ap.add_argument("--knot-s", type=float, default=900.0)
    ap.add_argument("--sigma0", type=float, default=0.5, help="unit-weight sigma (m of L1)")
    ap.add_argument("--kaula", type=float, default=10.0, help="Kaula ridge scale (TECU)")
    ap.add_argument("--a00-sigma", type=float, default=100.0, help="batch a00 ridge (TECU)")
    ap.add_argument("--a00-sigma-kf", type=float, default=30.0)
    ap.add_argument("--a00-prior", type=float, default=15.0)
    ap.add_argument("--b-sigma", type=float, default=100.0, help="loose b_r/b_s prior (m)")
    ap.add_argument("--q-deg", type=float, default=1.0e-2, help="RW q (TECU^2/s)")
    ap.add_argument("--gauge-sigma", type=float, default=1.0e-3)
    ap.add_argument("--gate", type=float, default=5.0)
    ap.add_argument("--huber", type=float, default=3.0)
    ap.add_argument("--irls", type=int, default=2)
    ap.add_argument("--no-regional", action="store_true")
    ap.add_argument("--exclude-stations", default="",
                    help="comma-separated station names dropped from every fit "
                         "(sensitivity only; state it in the report)")
    ap.add_argument("--tag", default="", help="suffix on the output file name")
    ap.add_argument("--withhold", default="", help="override the out-of-sample station")
    cfg = ap.parse_args()

    degs = [int(x) for x in cfg.degrees.split(",")]
    masks = [float(x) for x in cfg.masks.split(",")]
    bss = [bool(int(x)) for x in cfg.bs.split(",")]

    t_start = time.time()
    B = load_bed(cfg.run_dir, cfg.bed)
    if cfg.withhold:
        B["withhold"] = cfg.withhold
    prepare(B)
    os.makedirs(cfg.out, exist_ok=True)
    L = []
    P = L.append

    drop = [B["stations"].index(s) for s in cfg.exclude_stations.split(",")
            if s.strip() and s.strip() in B["stations"]]
    t0 = B["t0"]
    cs_tow, cs_el, cs_ti = B["cs_tow"], B["cs_el_deg"], B["cs_ti"]
    age, born = arcs(cs_tow, cs_ti)

    P("=" * 96)
    P(f"AB1-0 trace-only SH consumer-product prototype -- bed={cfg.bed}")
    P(f"run_dir   : {cfg.run_dir}")
    P(f"generated : {time.strftime('%Y-%m-%d %H:%M:%S')}  (deterministic; no RNG)")
    P("=" * 96)
    P("")
    P("CONVENTIONS (ported, file:line)")
    P("  basis / normalization / sun-fixed longitude : scripts/sh_vtec.py (self-tested port of")
    P("      src/core/atmosphere/SphericalHarmonicIonosphere.cpp:86-113 + meanSunLongitude:55-84)")
    P("  coefficient order  : SphericalHarmonicIonosphere.cpp:92-95 (C(n,0..n) then S(n,1..n))")
    P("  IPP + mapping      : SphericalHarmonicIonosphere.cpp:19-53, 1/cos z',")
    P("                       sin z' = Re/(Re+H) cos(el);  Re = 6371 km, H = 450 km")
    P("                       (GnssConfigTypes.hpp:3939 global_vtec_sh defaults)")
    P("  ecefToLlh          : src/core/math/Coordinates.cpp:9-50 (WGS84, 10 iterations)")
    P(f"  TECU -> L1 m       : 40.3e16/f_L1^2 = {TECU_TO_L1M:.6f} "
      "(ApplicationNetworkAssembly.cpp:1056-1057)")
    P(f"  weighting          : w = sin^2(el) / sigma0^2, sigma0 = {cfg.sigma0} m of L1 slant")
    P(f"  batch knots        : {cfg.knot_s:.0f} s, piecewise linear, hours 2-6")
    P(f"  ridge (batch)      : Kaula sigma_n = {cfg.kaula}/n TECU for n>=1; a00 sigma "
      f"{cfg.a00_sigma} TECU; b_r/b_s sigma {cfg.b_sigma} m; gauge sum(b_s)=0 exact (KKT row)")
    P(f"  robust (batch)     : {cfg.irls} IRLS passes, Huber k={cfg.huber} on 1.4826*MAD")
    P(f"  KF (causal)        : a_nm random walk q = {cfg.q_deg}/n^2 TECU^2/s (n>=1), b_r/b_s")
    P(f"                       constant with sigma {cfg.b_sigma} m prior; a00 prior "
      f"{cfg.a00_prior}+-{cfg.a00_sigma_kf} TECU; innovation gate {cfg.gate} sigma;")
    P(f"                       gauge sum(b_s)=0 re-applied each epoch at {cfg.gauge_sigma} m")
    P("")
    P("INPUTS")
    P(f"  network rows   : {len(B['ni']):7d}   stations {len(B['stations'])}   "
      f"sats {len(B['sats'])}   epochs {len(B['tows'])}   "
      f"tow {B['tows'][0]:.0f}..{B['tows'][-1]:.0f}")
    P(f"  consumer       : {B['cons_name']}  rows {len(cs_tow)}  sats {len(set(cs_ti))}")
    P(f"  consumer geom  : {B['geom_src']}")
    P(f"  epoch grids    : max |network tow - consumer tow| = {B['tow_align_max_s']:.3f} s "
      "(0 = the causal product is read same-epoch, no lag)")
    P(f"  truth          : {B['truth'] or '(none on this bed)'}")
    P(f"  withheld sta   : {B['withhold']} "
      f"({B['sta_dist_km'][B['stations'].index(B['withhold'])]:.1f} km from the consumer)")
    P(f"  stations <= {REGIONAL_RADIUS_KM:.0f} km of the consumer (variant R pool): "
      f"{len(B['near'])} -> "
      + ", ".join(f"{B['stations'][i]}({B['sta_dist_km'][i]:.0f}km)" for i in B["near"][:14]))
    y = B["ni_y"]
    if drop:
        P(f"  EXCLUDED       : {cfg.exclude_stations} (sensitivity arm, not the baseline)")
    P(f"  iono_total_m   : median {np.median(y):.3f}  p01 {np.percentile(y,1):.2f}  "
      f"p99 {np.percentile(y,99):.2f}  min {y.min():.2f}  max {y.max():.2f}")
    bad = np.abs(y) > 60.0
    if bad.sum():
        vc = pd.Series(np.array(B["stations"])[B["ni_si"][bad]]).value_counts()
        P(f"  |iono_total| > 60 m : {int(bad.sum())} rows, by station "
          f"{dict(list(vc.items())[:5])}")
    P("")

    # ---------------- fits ----------------
    fits = {}
    P("=" * 96)
    P("VARIANT SWEEP  (V1 degree x V2 mask x V3 b_s x V4 estimator)")
    P("  in-sample = weighted residual RMS over the network's own fitted rows")
    P("  oos       = withheld-station RMS, per-epoch median removed (b_r of a withheld")
    P("              station is unknown, so its level is removed the same way)")
    P("=" * 96)
    hdr = ("  est deg mask b_s |  rows   insampleRMS  oosRMS  |  G1 h2-6   G2 h3-6  | note")
    P(hdr)
    sweep = []
    for deg in degs:
        for mk in masks:
            for bs_on in bss:
                fb = fit_batch(B, deg, mk, bs_on, cfg, exclude=drop)
                fc = fit_causal(B, deg, mk, bs_on, cfg, exclude=drop, keep_state=True)
                wh = B["stations"].index(B["withhold"])
                fb_o = fit_batch(B, deg, mk, bs_on, cfg, exclude=drop + [wh])
                fc_o = fit_causal(B, deg, mk, bs_on, cfg, exclude=drop + [wh],
                                  keep_state=True)
                for f, fo in ((fb, fb_o), (fc, fc_o)):
                    f["oos"] = oos_rms(B, fo, wh, mk)
                    pc = product_at_consumer(B, f)
                    f["prod"] = pc
                    if np.any(np.isfinite(B["cs_truth"])):
                        e1 = demedian(cs_tow, pc - B["cs_truth"])
                        m = hour_of(cs_tow, t0) >= 2
                        f["g1"] = rms(e1[m]); f["e1"] = e1
                    else:
                        f["g1"] = np.nan; f["e1"] = np.full(len(pc), np.nan)
                    e2 = demedian(cs_tow, pc - B["cs_iono"])
                    m36 = (hour_of(cs_tow, t0) >= 3)
                    f["g2"] = rms(e2[m36]); f["e2"] = e2
                    fits[(f["kind"], deg, mk, bs_on)] = f
                    note = f.get("cond", "") if f["kind"] == "B" else \
                        f"gated {f['n_gate']}/{f['n_gate']+f['n_used']}"
                    nrows = f.get("n_rows", f.get("n_used", 0))
                    P(f"  {f['kind']:>3} {deg:3d} {mk:4.0f} {int(bs_on):3d} | {nrows:6d}  "
                      f"{f['res_rms']:9.3f}  {f['oos']:6.3f}  | "
                      f"{f['g1']:8.3f}  {f['g2']:8.3f}  | {note}")
                    sweep.append((f["kind"], deg, mk, bs_on, f["oos"], f["g1"], f["g2"]))
    P("")

    # selection: truth-free, non-circular -- the withheld-station out-of-sample RMS
    def best(kind):
        c = [k for k in fits if k[0] == kind]
        return min(c, key=lambda k: (fits[k]["oos"] if np.isfinite(fits[k]["oos"]) else 1e9))
    kB, kC = best("B"), best("C")
    P(f"SELECTED (by out-of-sample withheld-station RMS, truth-free):")
    P(f"  batch  B : degree {kB[1]}, mask {kB[2]:.0f}, b_s {'on' if kB[3] else 'off'}"
      f"   oos {fits[kB]['oos']:.3f} m")
    P(f"  causal C : degree {kC[1]}, mask {kC[2]:.0f}, b_s {'on' if kC[3] else 'off'}"
      f"   oos {fits[kC]['oos']:.3f} m")
    if np.any(np.isfinite(B["cs_truth"])):
        gB = min((k for k in fits if k[0] == "B"), key=lambda k: fits[k]["g1"])
        gC = min((k for k in fits if k[0] == "C"), key=lambda k: fits[k]["g1"])
        P(f"  cross-check, best by SIM TRUTH G1: B {gB[1:]} ({fits[gB]['g1']:.3f} m), "
          f"C {gC[1:]} ({fits[gC]['g1']:.3f} m)")
    P("")
    P("  degree sensitivity (mask/b_s at the selected values, oos | G1 h2-6 | G2 h3-6):")
    for kind, ksel in (("B", kB), ("C", kC)):
        s = "  ".join(f"n={d}: {fits[(kind,d,ksel[2],ksel[3])]['oos']:.3f}|"
                      f"{fits[(kind,d,ksel[2],ksel[3])]['g1']:.3f}|"
                      f"{fits[(kind,d,ksel[2],ksel[3])]['g2']:.3f}" for d in degs)
        P(f"    {kind}: {s}")
    P("  mask sensitivity (degree/b_s at the selected values):")
    for kind, ksel in (("B", kB), ("C", kC)):
        s = "  ".join(f"{m:.0f}deg: {fits[(kind,ksel[1],m,ksel[3])]['oos']:.3f}|"
                      f"{fits[(kind,ksel[1],m,ksel[3])]['g1']:.3f}|"
                      f"{fits[(kind,ksel[1],m,ksel[3])]['g2']:.3f}" for m in masks)
        P(f"    {kind}: {s}")
    P("  b_s sensitivity (degree/mask at the selected values):")
    for kind, ksel in (("B", kB), ("C", kC)):
        s = "  ".join(f"b_s={'on' if b else 'off'}: "
                      f"{fits[(kind,ksel[1],ksel[2],b)]['oos']:.3f}|"
                      f"{fits[(kind,ksel[1],ksel[2],b)]['g1']:.3f}|"
                      f"{fits[(kind,ksel[1],ksel[2],b)]['g2']:.3f}" for b in bss)
        P(f"    {kind}: {s}")
    P("")

    fB, fC = fits[kB], fits[kC]

    # ---------------- variant R ----------------
    fR = None
    pool = [i for i in B["near"] if i not in set(drop)]
    if not cfg.no_regional and len(pool):
        pr, nst = product_regional(B, fC, kC[2], drop=drop)
        e1 = demedian(cs_tow, pr - B["cs_truth"]) if np.any(np.isfinite(B["cs_truth"])) \
            else np.full(len(pr), np.nan)
        e2 = demedian(cs_tow, pr - B["cs_iono"])
        fR = dict(kind="R", deg=kC[1], mask=kC[2], bs_on=kC[3], prod=pr, e1=e1, e2=e2,
                  nstn=nst, g1=rms(e1[hour_of(cs_tow, t0) >= 2]),
                  g2=rms(e2[hour_of(cs_tow, t0) >= 3]))
        P("=" * 96)
        P("VARIANT R -- regional per-satellite, causal (b_r from variant C)")
        P(f"  pool radius {REGIONAL_RADIUS_KM:.0f} km, {len(pool)} candidate stations; "
          f"planar fit in IPP lat/lon when >=3 contribute, else inverse-distance mean")
        P("  pool: " + ", ".join(f"{B['stations'][i]}({B['sta_dist_km'][i]:.0f}km)"
                                 for i in pool))
        P(f"  contributing stations per consumer LOS: mean {nst[nst>0].mean():.2f}  "
          f"median {np.median(nst[nst>0]):.0f}  min {nst[nst>0].min() if np.any(nst>0) else 0}  "
          f"max {nst.max()}  |  rows with 0 contributors: {int(np.sum(nst==0))}/{len(nst)}")
        hist = pd.Series(nst).value_counts().sort_index()
        P("  histogram n_contrib: " + "  ".join(f"{k}:{v}" for k, v in hist.items()))
        P("  note: R interpolates (iono_total - b_r)/mf as specified, so the per-satellite")
        P("  bias b_s -- a SLANT constant -- is divided by mf(network) and multiplied by")
        P("  mf(consumer).  R therefore carries b_s automatically but distorts it by")
        P("  (mf_c/mf_r - 1); no separate b_s term is fitted for R.")
        P(f"  G1 h2-6 {fR['g1']:.3f} m   G2 h3-6 {fR['g2']:.3f} m")
        P("")

    # ---------------- G1 ----------------
    P("=" * 96)
    P("G1 -- product slant at the consumer minus truth I_L1_m (sim only)")
    P("     per-epoch MEDIAN OF THE DIFFERENCE removed (the satellite set rotates, so the")
    P("     removed quantity is median(product - truth), not median(product) - median(truth))")
    P("=" * 96)
    g1list = [("B " + str(kB[1:]), fB), ("C " + str(kC[1:]), fC)]
    if fR:
        g1list.append(("R " + str((kC[1], kC[2], kC[3])), fR))
    if np.any(np.isfinite(B["cs_truth"])):
        for tag, f in g1list:
            P(table_hour_band(cs_tow, cs_el, f["e1"], t0, label=f"variant {tag}"))
            P("")
        P("  every variant, RMS vs truth by hour (m):")
        P("    est deg mask b_s |    h1     h2     h3     h4     h5     h6  |  ALL  h2-6")
        for k in sorted(fits, key=lambda k: (k[0], k[1], k[2], not k[3])):
            f = fits[k]; hr = hour_of(cs_tow, t0)
            cells = "  ".join(f"{rms(f['e1'][hr==h]):5.2f}" for h in range(1, 7))
            P(f"    {k[0]:>3} {k[1]:3d} {k[2]:4.0f} {int(k[3]):3d} |  {cells}  | "
              f"{rms(f['e1']):5.2f} {f['g1']:5.2f}")
    else:
        P("  (field bed: no truth ionosphere at the consumer -- G1 is sim-only by design)")
    P("")

    # ---------------- G2 ----------------
    P("=" * 96)
    P("G2 -- product slant at the consumer minus the consumer's OWN converged iono state")
    P("     (closed_loop_sat_trace.csv iono_m, hours 3-6), same median-of-difference removal")
    P("=" * 96)
    g2list = [("B " + str(kB[1:]), fB), ("C " + str(kC[1:]), fC)]
    if fR:
        g2list.append(("R " + str((kC[1], kC[2], kC[3])), fR))
    for tag, f in g2list:
        P(table_hour_band(cs_tow, cs_el, f["e2"], t0, hours=[3, 4, 5, 6],
                          label=f"variant {tag}"))
        P("")
    if np.any(np.isfinite(B["cs_truth"])):
        eo = demedian(cs_tow, B["cs_iono"] - B["cs_truth"])
        P(table_hour_band(cs_tow, cs_el, eo, t0, hours=[3, 4, 5, 6],
                          label="reference quality: the CONSUMER'S OWN state vs truth"))
        P("")
        P("  so on this bed the G2 reference itself carries the RMS above; a product G2")
        P("  below that number is not resolvable by G2 alone.")
        P("")
    P("  every variant, G2 RMS hours 3-6 (m):")
    P("    est deg mask b_s |   h3     h4     h5     h6  |  h3-6")
    for k in sorted(fits, key=lambda k: (k[0], k[1], k[2], not k[3])):
        f = fits[k]; hr = hour_of(cs_tow, t0)
        cells = "  ".join(f"{rms(f['e2'][hr==h]):5.2f}" for h in range(3, 7))
        P(f"    {k[0]:>3} {k[1]:3d} {k[2]:4.0f} {int(k[3]):3d} |  {cells}  | {f['g2']:5.2f}")
    P("")

    # ---------------- G3 ----------------
    P("=" * 96)
    P("G3 -- the useful window: RMS vs arc age k (epochs since the consumer first tracks")
    P("     the satellite; 30 s epochs, a gap > 45 s starts a new arc)")
    P("=" * 96)
    if cfg.bed == "sim":
        cons_err = demedian(cs_tow, B["cs_iono"] - B["cs_truth"])
        prod_err = fC["e1"]
        prodR_err = fR["e1"] if fR else None
        ref = "truth I_L1_m"
        proxy = ""
    else:
        cons_err = demedian(cs_tow, B["cs_iono"] - fB["prod"])
        prod_err = demedian(cs_tow, fC["prod"] - fB["prod"])
        prodR_err = demedian(cs_tow, fR["prod"] - fB["prod"]) if fR else None
        ref = f"the variant-B batch product (degree {kB[1]}, mask {kB[2]:.0f})"
        proxy = ("  *** PROXY: the field has no truth, so both the consumer's error and the\n"
                 "  *** product's error are measured against the SAME batch product B.  That\n"
                 "  *** reference is built from the same rows and the same model family as C,\n"
                 "  *** so the comparison is BIASED IN FAVOUR OF C and R.  Read it as an upper\n"
                 "  *** bound on the product's advantage, not as an accuracy.\n"
                 "  *** The reference exists only in hours 2-6, so the field k-table can only\n"
                 "  *** cover arcs BORN inside that window; the epoch-0 cold start has no\n"
                 "  *** field reference at all.\n")
    P(f"  reference : {ref}")
    if proxy:
        P(proxy.rstrip())
    P("")
    for pop, popmask in (("ALL arcs (incl. the cold start at epoch 0)",
                          np.ones(len(cs_tow), bool)),
                         ("arcs BORN AFTER hour 1", born >= t0 + 3600.0)):
        m0 = popmask & np.isfinite(prod_err) & np.isfinite(cons_err)
        if not np.any(m0):
            continue
        P(f"  {pop}   (n arcs = {len(set(zip(born[m0], cs_ti[m0])))})")
        P("      k |    n |  consumer_own  formal_sigma |  product_C   product_R")
        for k in range(0, 41):
            m = m0 & (age == k)
            n = int(np.sum(m))
            if n == 0:
                continue
            pr = rms(prod_err[m])
            rr = rms(prodR_err[m]) if prodR_err is not None else np.nan
            rtxt = f"{rr:8.3f}" if np.isfinite(rr) else "       -"
            P(f"    {k:3d} | {n:4d} |    {rms(cons_err[m]):8.3f}     "
              f"{np.median(B['cs_sig'][m]):8.3f} |  {pr:8.3f}   {rtxt}")
        # crossover
        for nm, pe in (("C", prod_err), ("R", prodR_err)):
            if pe is None:
                continue
            run = -1
            for k in range(0, 41):
                m = m0 & (age == k)
                if np.sum(m) == 0:
                    continue
                if rms(pe[m]) < rms(cons_err[m]):
                    run = k
                else:
                    break
            last = max([k for k in range(41)
                        if np.sum(m0 & (age == k)) > 0
                        and rms(pe[m0 & (age == k)]) < rms(cons_err[m0 & (age == k)])],
                       default=-1)
            P(f"    crossover {nm}: contiguous from k=0 through k={run} "
              f"(-1 = it never beats at k=0); last individual k where it beats = {last}")
        P("")

    # ---------------- G4 ----------------
    P("=" * 96)
    P("G4 -- sigma model for the best causal variant: RMS(err) = sigma_z * mf(el)")
    P("=" * 96)
    err = fC["e1"] if cfg.bed == "sim" else demedian(cs_tow, fC["prod"] - fB["prod"])
    m = np.isfinite(err) & (hour_of(cs_tow, t0) >= 2)
    mf = B["cs_mf"]
    sz = float(np.sqrt(np.mean((err[m] / mf[m]) ** 2)))
    P(f"  sigma_z = {sz:.4f} m of L1 vertical  ({sz/TECU_TO_L1M:.2f} TECU vertical), "
      f"hours 2-6, n={int(m.sum())}")
    if cfg.bed == "field":
        P("  (field: against the batch-B proxy -- an agreement scale, not an accuracy)")
    P("    elev band |     n |  mf_med |  RMS_m  | sigma_z*mf | ratio")
    edges = [15, 20, 25, 30, 40, 50, 60, 75, 90]
    for a, z in zip(edges[:-1], edges[1:]):
        mm = m & (cs_el >= a) & (cs_el < z)
        if np.sum(mm) < 10:
            continue
        mfm = float(np.median(mf[mm])); r = rms(err[mm])
        P(f"    {a:3d}-{z:3d}   | {int(np.sum(mm)):5d} | {mfm:7.3f} | {r:7.3f} | "
          f"{sz*mfm:10.3f} | {r/(sz*mfm):5.2f}")
    P("")

    # ---------------- G5 ----------------
    P("=" * 96)
    P("G5 -- the per-satellite bias b_s (metres of L1 slant; gauge sum_s b_s = 0)")
    P("=" * 96)
    kb_on = ("B", kB[1], kB[2], True)
    if kb_on in fits:
        fb_on = fits[kb_on]
        bs = fb_on["bs"]
        seen = np.unique(B["ni_ti"])
        bsv = bs[seen]
        P(f"  batch (degree {kB[1]}, mask {kB[2]:.0f}): n={len(bsv)}  mean {bsv.mean():+.3f}  "
          f"std {bsv.std(ddof=1):.3f}  min {bsv.min():+.3f}  max {bsv.max():+.3f}  "
          f"p50|b_s| {np.median(np.abs(bsv)):.3f}")
        P(f"  in TECU of slant: std {bsv.std(ddof=1)/TECU_TO_L1M:.2f}  "
          f"range {(bsv.max()-bsv.min())/TECU_TO_L1M:.2f}")
        # halves of the batch window
        mid = B["t0"] + 3600.0 + 0.5 * (21600.0 - 3600.0)
        h1 = fit_batch(B, kB[1], kB[2], True, cfg, window=(B["t0"] + 3600.0, mid))
        h2 = fit_batch(B, kB[1], kB[2], True, cfg, window=(mid, B["t0"] + 21600.0))
        if h1 and h2:
            d = h2["bs"][seen] - h1["bs"][seen]
            P(f"  stability first vs second half of the window: RMS(delta b_s) "
              f"{rms(d):.3f} m, max |delta| {np.abs(d).max():.3f} m, "
              f"corr {np.corrcoef(h1['bs'][seen], h2['bs'][seen])[0,1]:.3f}")
        P("  per satellite (m):  " + "  ".join(
            f"{B['sats'][i]}:{bs[i]:+.2f}" for i in seen))
    P("")
    P("  does b_s help?  (same degree/mask, b_s on vs off)")
    P("    est deg mask |  oos_on   oos_off |  G1_on   G1_off |  G2_on   G2_off")
    for kind in ("B", "C"):
        for deg in degs:
            for mk in masks:
                if (kind, deg, mk, True) not in fits or (kind, deg, mk, False) not in fits:
                    continue
                a, b = fits[(kind, deg, mk, True)], fits[(kind, deg, mk, False)]
                P(f"    {kind:>3} {deg:3d} {mk:4.0f} | {a['oos']:7.3f} {b['oos']:8.3f} | "
                  f"{a['g1']:6.3f} {b['g1']:7.3f} | {a['g2']:6.3f} {b['g2']:7.3f}")
    P("")

    # ---------------- sanity ----------------
    P("=" * 96)
    P("SANITY -- in-sample fit residuals and the withheld-station out-of-sample check")
    P("=" * 96)
    P("    est deg mask b_s |  n_rows  resid_RMS  w_resid_RMS  robust_sigma  n_downweighted")
    for k in sorted(fits, key=lambda k: (k[0], k[1], k[2], not k[3])):
        f = fits[k]
        if f["kind"] != "B":
            continue
        P(f"    {k[0]:>3} {k[1]:3d} {k[2]:4.0f} {int(k[3]):3d} | {f['n_rows']:7d}  "
          f"{f['res_rms']:9.3f}  {f['res_wrms']:11.3f}  {f['sigma_hat']:12.3f}  {f['n_down']:6d}")
    P("    est deg mask b_s |  n_used  n_gated  innov_RMS   (causal)")
    for k in sorted(fits, key=lambda k: (k[0], k[1], k[2], not k[3])):
        f = fits[k]
        if f["kind"] != "C":
            continue
        P(f"    {k[0]:>3} {k[1]:3d} {k[2]:4.0f} {int(k[3]):3d} | {f['n_used']:7d}  "
          f"{f['n_gate']:7d}  {f['res_rms']:9.3f}")
    P("")
    P(f"  out-of-sample: {B['withhold']} withheld from the fit, then its own iono_total_m")
    P("  predicted; the per-epoch median is removed on BOTH sides because a withheld")
    P("  station's b_r is not estimated.")
    P("")

    # ---------------- decision rule ----------------
    P("=" * 96)
    P("DECISION RULE (frozen, AB1 section 3): build the consumer constraint only if the")
    P("best causal variant's error at the consumer is below the consumer's own ionosphere")
    P("error for at least the first 10 epochs of an arc on BOTH beds.")
    P("=" * 96)
    for pop, popmask in (("ALL arcs", np.ones(len(cs_tow), bool)),
                         ("arcs born after hour 1", born >= t0 + 3600.0)):
        m0 = popmask & np.isfinite(prod_err) & np.isfinite(cons_err)
        P(f"  {pop}: k=0..9")
        P("      k |    n | consumer_own |  C_err  C<cons |  R_err  R<cons")
        okC = okR = True
        for k in range(10):
            m = m0 & (age == k)
            if np.sum(m) == 0:
                P(f"    {k:3d} |    0 |            - |      -       - |      -       -")
                okC = okR = False
                continue
            c = rms(cons_err[m]); pc = rms(prod_err[m])
            pr = rms(prodR_err[m]) if prodR_err is not None else np.nan
            okC &= pc < c
            okR &= (pr < c) if np.isfinite(pr) else False
            P(f"    {k:3d} | {int(np.sum(m)):4d} | {c:12.3f} | {pc:6.3f}  {str(pc<c):>6} | "
              + (f"{pr:6.3f}  {str(pr<c):>6}" if np.isfinite(pr) else "     -       -"))
        P(f"    -> variant C passes k=0..9 on this bed/population: {okC}")
        P(f"    -> variant R passes k=0..9 on this bed/population: {okR}")
        P("")
    P(f"  (elapsed {time.time()-t_start:.1f} s)")

    path = os.path.join(cfg.out, f"{cfg.bed}{cfg.tag}.txt")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    print("\n".join(L))
    print(f"\n[written] {path}")


def oos_rms(B, fit, wh_idx, mask_deg):
    """Predict the withheld station's iono_total_m; remove the per-epoch median both sides."""
    sel = (B["ni_si"] == wh_idx) & (np.degrees(B["ni_el"]) >= mask_deg - 1e-9)
    idx = np.where(sel)[0]
    if len(idx) == 0:
        return np.nan
    K = (fit["deg"] + 1) ** 2
    G = B["ni_Y8"][idx][:, :K] * (B["ni_mf"][idx] * TECU_TO_L1M)[:, None]
    tow, ti = B["ni_tow"][idx], B["ni_ti"][idx]
    if fit["kind"] == "B":
        knots = fit["knots"]; step = knots[1] - knots[0]
        inside = (tow >= knots[0] - 1e-6) & (tow <= knots[-1] + 1e-6)
        j = np.clip(np.searchsorted(knots, tow, "right") - 1, 0, len(knots) - 2)
        f = np.clip((tow - knots[j]) / step, 0.0, 1.0)
        A = (1.0 - f)[:, None] * fit["a"][j] + f[:, None] * fit["a"][j + 1]
        pred = np.einsum("ij,ij->i", G, A) + fit["bs"][ti]
        idx, pred, tow = idx[inside], pred[inside], tow[inside]
    else:
        e = np.searchsorted(fit["tows"], tow)
        e = np.clip(e, 0, len(fit["tows"]) - 1)
        pred = np.einsum("ij,ij->i", G, fit["a_t"][e]) \
            + fit["bs_t"][e][np.arange(len(e)), ti]
        keep = tow >= B["t0"] + 3600.0          # grade out-of-sample after ignition
        idx, pred, tow = idx[keep], pred[keep], tow[keep]
    if len(idx) == 0:
        return np.nan
    return rms(demedian(tow, B["ni_y"][idx] - pred))


if __name__ == "__main__":
    main()
