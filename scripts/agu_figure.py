#!/usr/bin/env python3
"""Compose the 4-tile AGU26 abstract figure from one network run.

Default layout (--layout mechanism):
    (a) network geometry and the network's own estimated ionosphere, as
        ionospheric pierce points colored by vertical TEC at a converged epoch;
    (b) the datum-invariant total_IF level channel, windowed, with the code-bias
        set-change events that coincide with a step;
    (c) how long the product lets an ambiguity live against how long an arc
        needs to mature;
    (d) the held-out consumer's position error (E/N/U + 3D).

--layout products replaces (b) and (c) with the raw per-series ensembles:
satellite code bias (per-signal OSB, W6, self-estimated) and satellite phase
bias. Each series is plotted as a deviation from its own run median: the
estimable clock and biases are datum-dependent, so their raw levels are
metre-scale numbers that say nothing about product quality, while what a
consumer needs is that they be stable. De-medianed, the y-axis is centimetres
and the panel makes that claim directly. A per-series median is a constant
offset, so this is a presentation choice, not a correction: the spread it
shows is real.

``gnssviz`` supplies the domain layer (pierce-point geometry, the slant-to-VTEC
thin-shell mapping, the coastline, the viridis table), so nothing here
re-derives geodesy; the layout is local because ``figures.Figure`` stacks axes
vertically and this figure needs a grid.

Usage
-----
  python scripts/agu_figure.py --run-dir runs/agu63
  python scripts/agu_figure.py --run-dir runs/agu63 --iono-window-min 30 --out fig.png
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "pyviz"))

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap

from gnssviz.colormaps import strip_colormap
from gnssviz.coastline import has_coastline, load_coastline
from gnssviz.network_config import load_stations

sys.path.insert(0, str(_REPO / "scripts"))
from sh_vtec import load_epoch, synth_grid  # noqa: E402
from grade_total_if import _BANDS, if_coeffs, total_if  # noqa: E402

# L1 slant iono delay (m) per TECU, and the thin shell the C++ pierce-point code uses.
_F_L1 = 1.57542e9
_K_L1 = 40.308e16 / (_F_L1 * _F_L1)
_RE_KM, _H_KM = 6371.0, 450.0

# Ink (recessive grid/axes; text never wears a series colour).
_INK = "#1a1a1a"
_MUTED = "#6b6b6b"
_GRID = "#d8d8d8"
# Categorical order for panel (d), fixed, never cycled.
_C_E, _C_N, _C_U, _C_3D = "#1f6feb", "#2a9d4a", "#d1560f", "#1a1a1a"
_ENSEMBLE = "#3b6ea5"


# --------------------------------------------------------------------------- #
def slant_to_vtec(iono_m, el_deg):
    """L1 slant iono delay (m) -> vertical TEC (TECU); mirrors network_iono_map."""
    stec = np.asarray(iono_m, float) / _K_L1
    el = np.radians(np.asarray(el_deg, float))
    sin_zp = (_RE_KM / (_RE_KM + _H_KM)) * np.cos(el)
    cos_zp = np.sqrt(np.maximum(0.0, 1.0 - sin_zp ** 2))
    with np.errstate(invalid="ignore"):
        vtec = stec * cos_zp
    bad = ~np.isfinite(np.asarray(el_deg, float)) | (np.asarray(el_deg, float) <= 0)
    return np.where(bad, np.nan, vtec)


_TOW_MAX = 604800.0        # seconds in a GPS week


def _read(path: Path, what: str, required: tuple = ("tow",)) -> pd.DataFrame:
    """Load a trace CSV, dropping rows a partial write left unusable.

    Trace writers here are not all line-flushed, so a live or crash-truncated
    file can end mid-row.  One NaN ``tow`` is enough to make ``h.max()`` NaN,
    which silently turns every downstream ``h.max() > x`` window test False and
    grades the whole run as if it were the converged tail.  Fail loud instead.
    """
    if not path.is_file():
        raise SystemExit(f"missing {what}: {path}")
    df = pd.read_csv(path)
    n0 = len(df)
    df = df.dropna(subset=[c for c in required if c in df.columns])
    if "tow" in df.columns:
        df = df[(df["tow"] >= 0.0) & (df["tow"] < _TOW_MAX)]
    if df.empty:
        raise SystemExit(f"{what} has no usable rows: {path}")
    if n0 - len(df):
        print(f"  note: {what} dropped {n0 - len(df)} unusable row(s) "
              f"(partial write / out-of-range tow)")
    return df


def _hours(tow: np.ndarray, t0: float) -> np.ndarray:
    return (np.asarray(tow, float) - t0) / 3600.0


def _style(ax, *, title, xlabel, ylabel):
    ax.set_title(title, fontsize=10.5, color=_INK, loc="left", pad=6)
    ax.set_xlabel(xlabel, fontsize=9, color=_MUTED)
    ax.set_ylabel(ylabel, fontsize=9, color=_MUTED)
    ax.tick_params(labelsize=8.5, colors=_MUTED, length=3)
    ax.grid(True, color=_GRID, linewidth=0.6, alpha=0.9)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(_GRID)


def _panel_tag(ax, letter):
    ax.text(-0.035, 1.09, f"({letter})", transform=ax.transAxes, fontsize=12,
            fontweight="bold", color=_INK, va="top", ha="right")


# --------------------------------------------------------------------------- #
def panel_iono(ax, log_dir: Path, cfg: Path | None, window_min: float,
               held_out: str | None):
    """(a) The network's own spherical-harmonic VTEC field and station geometry.

    Synthesized from ``network_vtec_sh.csv``, the estimated coefficients, i.e.
    the actual emitted product. Deliberately not ``network_ionosphere.csv``:
    that trace emits ``x_r`` without its linearization point (IonoDiag.cpp:92)
    while the per-LOS iono state is seeded at allocation (MeasurementModel.cpp:404),
    so it is a correction, not absolute slant ionosphere. Measured on this run it
    medians about 10 TECU against the field's own about 30 TECU a00, with 37% negative
    values.
    """
    coeffs, week, tow = load_epoch(log_dir / "network_vtec_sh.csv")
    lats = np.linspace(-88.0, 88.0, 179)
    lons = np.linspace(-180.0, 180.0, 361)
    grid = synth_grid(coeffs, week, tow, lats, lons)

    stations = load_stations(str(log_dir / "network_stations.csv"),
                             str(cfg) if cfg else None)
    if not stations:
        raise SystemExit("no station roster -- cannot draw network geometry")
    pos = {s.id: (s.lat, s.lon) for s in stations}
    # The roster CSV carries the held-out consumer too; it is not a network
    # station and must not be counted or drawn as one.
    net_stations = [s for s in stations if s.id != held_out]

    cmap = ListedColormap(strip_colormap("viridis", 256))
    mesh = ax.pcolormesh(lons, lats, grid, cmap=cmap, shading="auto", zorder=1)
    if has_coastline():
        clon, clat = load_coastline()
        ax.plot(clon, clat, color="#ffffff", linewidth=0.5, alpha=0.55, zorder=3)

    ax.scatter([s.lon for s in net_stations], [s.lat for s in net_stations],
               marker="^", s=17, c="#ffffff", edgecolors="#111111",
               linewidths=0.5, zorder=6,
               label=f"network stations ({len(net_stations)})")
    if held_out and held_out in pos:
        ax.scatter([pos[held_out][1]], [pos[held_out][0]], marker="*", s=230,
                   c="#ffd24a", edgecolors="#1a1a1a", linewidths=0.9, zorder=7,
                   label=f"held-out consumer ({held_out[:4]})")

    ax.set_xlim(-180, 180)
    ax.set_ylim(-88, 88)
    ax.set_aspect("equal", adjustable="box")
    ax.set_xticks(range(-180, 181, 60))
    ax.set_yticks(range(-90, 91, 45))
    nmax = int(coeffs["n"].max())
    nlay = coeffs["layer"].nunique()
    hrs = (tow - float(pd.read_csv(log_dir / "network_vtec_sh.csv")["tow"].min())) / 3600.0
    _style(ax, title=f"Network geometry + its estimated SH ionosphere "
                     f"(deg {nmax}, {nlay} shell{'s' if nlay > 1 else ''})",
           xlabel="longitude (deg)", ylabel="latitude (deg)")
    ax.grid(False)
    ax.legend(loc="lower left", fontsize=7.5, framealpha=0.9, borderpad=0.4)
    cb = ax.figure.colorbar(mesh, ax=ax, fraction=0.026, pad=0.02)
    cb.set_label("VTEC (TECU)", fontsize=8.5, color=_MUTED)
    cb.ax.tick_params(labelsize=8, colors=_MUTED)
    cb.outline.set_visible(False)
    return {"n_stations": len(net_stations), "sh_degree": nmax, "layers": nlay,
            "vtec_min": round(float(grid.min()), 2),
            "vtec_max": round(float(grid.max()), 2),
            "vtec_mean": round(float(grid.mean()), 2), "epoch_h": round(hrs, 2)}


# --------------------------------------------------------------------------- #
def _bias_panel(ax, sol: pd.DataFrame, kind: str, t0: float, *, title, letter,
                settle_h: float = 2.0):
    """Per-(sat,signal) deviation from own median: the stability claim.

    Both the reference median and the plotted span are taken after ``settle_h``.
    The cold-start ramp is a convergence transient, not product instability, and
    letting it set either the reference level or the y-scale would hide the
    thing the panel is meant to show.  The window is stated in the title.
    """
    d = sol[sol["kind"] == kind].copy()
    if d.empty:
        ax.text(0.5, 0.5, f"no {kind} rows", transform=ax.transAxes,
                ha="center", va="center", color=_MUTED, fontsize=10)
        _style(ax, title=title, xlabel="hours from run start", ylabel="deviation (cm)")
        _panel_tag(ax, letter)
        return {"n_series": 0}

    d["key"] = d["sat"].astype(str) + "|" + d["signal"].fillna("").astype(str)
    d["h"] = _hours(d["tow"].to_numpy(float), t0)
    if d["h"].max() > settle_h * 1.5:
        d = d[d["h"] >= settle_h].copy()
    else:                                     # short run (smoke): keep everything
        settle_h = 0.0
    if d.empty:
        raise SystemExit(f"{kind}: no rows after settle_h={settle_h}")
    d["dev_cm"] = 100.0 * (d["value_m"] - d.groupby("key")["value_m"].transform("median"))

    for _, g in d.groupby("key"):
        g = g.sort_values("h")
        ax.plot(g["h"], g["dev_cm"], color=_ENSEMBLE, linewidth=0.4, alpha=0.16,
                solid_capstyle="round", zorder=2)

    # Cross-series envelope: the readable summary over an unlabellable ensemble.
    grid = d.groupby("tow")["dev_cm"]
    hh = _hours(np.array(sorted(d["tow"].unique()), float), t0)
    p16, p50, p84 = (grid.quantile(q).to_numpy() for q in (0.16, 0.50, 0.84))
    ax.fill_between(hh, p16, p84, color=_ENSEMBLE, alpha=0.30, linewidth=0,
                    zorder=3, label="16-84th pct across satellites")
    ax.plot(hh, p50, color="#14395e", linewidth=1.4, zorder=4, label="median")
    ax.axhline(0.0, color=_MUTED, linewidth=0.7, zorder=1)

    n_series = d["key"].nunique()
    rob = float(np.nanmedian(np.abs(d["dev_cm"])))
    span = f", t>{settle_h:g} h" if settle_h else ""
    _style(ax, title=f"{title} ({n_series} series{span})",
           xlabel="hours from run start", ylabel="deviation from own median (cm)")
    ax.legend(loc="upper right", fontsize=7.5, framealpha=0.9)
    # Scale to a fixed high percentile, not to the extremes: a handful of series
    # carrying a signalled discontinuity would otherwise set a y-range that
    # flattens the bulk into a line.  This bounds the clipped fraction at about
    # 2.5% by construction, and the panel reports the fraction it clips.
    lim = float(np.nanpercentile(np.abs(d["dev_cm"]), 97.5))
    lim = max(lim, float(np.nanmax(np.abs(np.concatenate([p16, p84])))), 1.0)
    ax.set_ylim(-1.05 * lim, 1.05 * lim)
    n_clipped = int((np.abs(d["dev_cm"]) > lim).sum())
    _panel_tag(ax, letter)
    return {"n_series": int(n_series), "med_abs_dev_cm": round(rob, 3),
            "p84_max_cm": round(float(np.nanmax(np.abs(p84))), 2),
            "clipped_pts": n_clipped,
            "clipped_frac": round(n_clipped / max(1, len(d)), 4)}


# --------------------------------------------------------------------------- #
def _product_wide_events(sol: pd.DataFrame, kind: str, t0: float,
                         frac: float = 0.5, step_m: float = 0.5) -> list[float]:
    """Hours at which >`frac` of a product's channels jump >`step_m` in one epoch.

    A set-change datum re-realization, as distinct from per-channel churn.
    """
    d = sol[sol["kind"] == kind].copy()
    if d.empty:
        return []
    d["key"] = d["sat"].astype(str) + "|" + d["signal"].fillna("").astype(str)
    d["h"] = _hours(d["tow"].to_numpy(float), t0)
    d = d.drop_duplicates(["h", "key"])
    piv = d.pivot_table(index="h", columns="key", values="value_m")
    dif = piv.diff().abs()
    live = dif.notna().sum(axis=1).replace(0, np.nan)
    hit = (dif > step_m).sum(axis=1) / live
    return [float(h) for h in hit[hit > frac].index]


def panel_total_if(ax, sol: pd.DataFrame, t0: float, window_min: float = 20.0):
    """(b) The datum-invariant level channel a consumer forms, windowed.

    total_IF = CLK + a1*CB(band1) - a2*CB(band2), as a deviation from the
    per-epoch median across satellites.  Median-referencing makes it invariant to
    any common shift, so a pure S-transformation cannot move it, which is what
    makes a step here evidence that a datum event was not gauge-only.

    Windowed at 20 min deliberately: hourly windows average across the step and
    hide it (they read 8-12 m where the truth is 4.8 m then 12.6 m).
    """
    best = {}
    for cons, (b1, b2, f1, f2, label) in _BANDS.items():
        tf = total_if(sol, cons)
        if tf is None or tf.empty:
            continue
        tf = tf.copy()
        tf["h"] = _hours(tf["tow"].to_numpy(float), t0)
        edges = np.arange(0.0, tf["h"].max() + 1e-9, window_min / 60.0)
        xs, ys = [], []
        for lo in edges:
            s = tf[(tf["h"] >= lo) & (tf["h"] < lo + window_min / 60.0)]
            if len(s) < 10:
                continue
            xs.append(lo + window_min / 120.0)
            ys.append(float(s["dev"].std()))
        if len(xs) < 2:
            continue
        col = _C_E if cons == "G" else _C_N
        ax.plot(xs, ys, color=col, linewidth=1.7, marker="o", markersize=3.0,
                label=label.split()[0], zorder=4)
        i = int(np.argmin(ys))
        best[cons] = (xs[i], ys[i], ys[-1])
        ax.scatter([xs[i]], [ys[i]], s=90, facecolor="white", edgecolor=col,
                   linewidths=1.6, zorder=6)
        ax.annotate(f"{ys[i]:.2f} m", (xs[i], ys[i]), textcoords="offset points",
                    xytext=(6, -13), fontsize=8.5, color=col, fontweight="bold")

    # Set-change datum events on the code bias: the thing the step coincides with.
    # Ignition events are excluded: before the field settles, "most channels moved"
    # is the cold start, not a datum re-realization.
    ev = [h for h in _product_wide_events(sol, "CB", t0) if h >= 0.5]
    for h in ev:
        ax.axvline(h, color="#b02a2a", linewidth=1.1, linestyle="--", zorder=2)
    if ev:
        ax.annotate("code-bias set-change\n(datum re-realization)",
                    (ev[0], 0.62), xycoords=("data", "axes fraction"),
                    textcoords="offset points", xytext=(8, 0), fontsize=8,
                    color="#b02a2a", va="center")

    _style(ax, title="Product level channel: total_IF between-satellite spread "
                     f"({window_min:g}-min windows)",
           xlabel="hours from run start", ylabel="spread of dev-from-median (m)")
    ax.legend(loc="lower left", fontsize=8, framealpha=0.9)
    if "G" in best:
        at, lo_v, last = best["G"]
        drift = 100.0 * (last - lo_v) / max(lo_v, 1e-9)
        ax.text(0.985, 0.05, f"best {lo_v:.2f} m at {at:.1f} h → "
                             f"{last:.2f} m at run end ({drift:+.0f}%)",
                transform=ax.transAxes, fontsize=8.5, color=_INK, ha="right")
    _panel_tag(ax, "b")
    return {k: {"best_h": round(v[0], 2), "best_m": round(v[1], 3),
                "final_m": round(v[2], 3)} for k, v in best.items()}


def panel_disc_vs_maturity(ax, sol: pd.DataFrame, t0: float,
                           maturity_min: float = 120.0, settle_h: float = 2.0):
    """(c) How long the product lets an ambiguity live, against how long it needs.

    Per (satellite,signal) channel, the elapsed time between ``disc_counter``
    increments, i.e. how long a consumer gets to hold an ambiguity before the
    product tells it to reset.  Compared against the measured requirement that
    an arc needs about 240 epochs (about 120 min) to reach resolvable precision.
    The overlap of the two accounts for the zero AR fix rate.
    """
    out = {}
    for kind, col, lbl in (("PB", _C_E, "phase bias"), ("CB", _C_U, "code bias")):
        d = sol[(sol["kind"] == kind)].copy()
        if d.empty or "disc_counter" not in d.columns:
            continue
        d = d.dropna(subset=["disc_counter"])
        d["key"] = d["sat"].astype(str) + "|" + d["signal"].fillna("").astype(str)
        d["h"] = _hours(d["tow"].to_numpy(float), t0)
        d = d[d["h"] >= settle_h].drop_duplicates(["h", "key"]).sort_values("h")
        gaps = []
        for _, g in d.groupby("key"):
            hh = g["h"].to_numpy(float)
            dc = g["disc_counter"].to_numpy(float)
            jump = np.flatnonzero(np.diff(dc) > 0)
            if jump.size == 0:
                continue
            ev = hh[jump + 1]
            gaps.extend(np.diff(np.concatenate([[hh[0]], ev])) * 60.0)
        # Floor at one epoch: two disc increments inside a single epoch produce a
        # ~0 interval that is a bookkeeping artifact, not a 10^-14-minute lifetime.
        gaps = np.asarray([max(g, 0.5) for g in gaps if g > 0], float)
        if gaps.size < 5:
            continue
        xs = np.sort(gaps)
        ys = np.arange(1, xs.size + 1) / xs.size
        ax.plot(xs, 100.0 * ys, color=col, linewidth=1.8, label=lbl, zorder=4)
        frac = float((gaps < maturity_min).mean())
        out[kind] = {"n": int(gaps.size), "median_min": round(float(np.median(gaps)), 1),
                     "frac_below_maturity": round(frac, 3)}

    # Censoring guard: a run of length T cannot contain an interval longer than T,
    # so on a short run "100% of intervals end before maturity" is arithmetic, not
    # a finding.  Require the observation window to be comfortably longer than the
    # threshold before the panel is allowed to make the claim.
    span_min = 60.0 * float(sol["tow"].pipe(lambda s: (s.max() - s.min())) / 3600.0)
    window_min_obs = max(span_min - 60.0 * settle_h, 0.0)
    censored = window_min_obs < 2.0 * maturity_min

    ax.axvline(maturity_min, color="#b02a2a", linewidth=1.4, zorder=5)
    ax.axvspan(0, maturity_min, color="#b02a2a", alpha=0.07, zorder=1)
    ax.annotate(f"arc needs ~{maturity_min:g} min\nto reach resolvable σ",
                (maturity_min, 34), textcoords="offset points", xytext=(9, 0),
                fontsize=8.5, color="#b02a2a", va="center")
    ax.set_xscale("log")
    _style(ax, title="How long the product lets an ambiguity live",
           xlabel="time between signalled discontinuities, per channel (min, log)",
           ylabel="cumulative % of intervals")
    ax.set_ylim(0, 100)
    ax.set_xlim(left=0.4)
    ax.legend(loc="lower right", fontsize=8, framealpha=0.9)
    if censored:
        ax.text(0.02, 0.93,
                f"CENSORED: only {window_min_obs:.0f} min observed against a "
                f"{maturity_min:g} min threshold —\nno interval longer than the "
                f"window can exist, so this fraction is\narithmetic, not a result. "
                f"Needs a run of >= {2*maturity_min/60:.0f} h.",
                transform=ax.transAxes, fontsize=8.5, color="#b02a2a", va="top")
    elif "PB" in out:
        ax.text(0.02, 0.93, f"{100*out['PB']['frac_below_maturity']:.0f}% of "
                            f"phase-bias intervals end before the arc can mature",
                transform=ax.transAxes, fontsize=8.5, color=_INK, va="top")
    out["censored"] = bool(censored)
    out["observed_min"] = round(window_min_obs, 1)
    _panel_tag(ax, "c")
    return out


def panel_consumer(ax, log_dir: Path, t0: float, held_out: str | None,
                   settle_h: float = 2.0):
    """(d) Held-out consumer position error: E/N/U and 3D, one axis, metres."""
    df = _read(log_dir / "closed_loop_trace.csv", "closed_loop_trace.csv",
               required=("tow", "pos_err_3d_m", "east_m", "north_m", "up_m"))
    df = df.sort_values("tow")
    h = _hours(df["tow"].to_numpy(float), t0)
    assert np.isfinite(h).all(), "non-finite epoch times survived the loader"
    if settle_h and h.max() > settle_h * 1.5:
        ax.axvline(settle_h, color=_MUTED, linewidth=0.8, linestyle="--", zorder=2)
        ax.text(settle_h, 0.13, " network converged", transform=ax.get_xaxis_transform(),
                fontsize=7.5, color=_MUTED, va="bottom", ha="left")
    for col, c, lab in (("east_m", _C_E, "East"), ("north_m", _C_N, "North"),
                        ("up_m", _C_U, "Up")):
        ax.plot(h, df[col].to_numpy(float), color=c, linewidth=1.0, label=lab, zorder=3)
    ax.plot(h, df["pos_err_3d_m"].to_numpy(float), color=_C_3D, linewidth=1.6,
            label="3D error", zorder=4)
    ax.axhline(0.0, color=_MUTED, linewidth=0.7, zorder=1)

    # Grade on the final hour: the converged-window convention used throughout
    # this project's network grading (never a whole-run mean, which averages the
    # ignition transient into the verdict).
    n = len(df)
    tail = df[h >= max(0.0, h.max() - 1.0)] if h.max() > 1.5 else df
    stats = {
        "n_epochs": int(n),
        "fix_rate": round(float((df["ar_success"] > 0).mean()), 4),
        "tail_n": int(len(tail)),
        "tail_med_3d": round(float(tail["pos_err_3d_m"].median()), 4),
        "tail_med_e": round(float(tail["east_m"].median()), 4),
        "tail_med_n": round(float(tail["north_m"].median()), 4),
        "tail_med_u": round(float(tail["up_m"].median()), 4),
    }
    name = held_out[:4] if held_out else "held-out"
    _style(ax, title=f"Held-out consumer {name}: position error vs ITRF2020",
           xlabel="hours from run start", ylabel="error (m)")
    ax.legend(loc="upper right", fontsize=7.5, ncol=4, framealpha=0.9,
              columnspacing=1.0, handlelength=1.4)
    ax.text(0.015, 0.05,
            f"final-hour median 3D {stats['tail_med_3d']:.3f} m   "
            f"(E {stats['tail_med_e']:+.3f}  N {stats['tail_med_n']:+.3f}  "
            f"U {stats['tail_med_u']:+.3f})   AR fix rate "
            f"{100*stats['fix_rate']:.0f}%",
            transform=ax.transAxes, fontsize=8, color=_INK)
    _panel_tag(ax, "d")
    return stats


# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-dir", required=True,
                    help="run directory containing logs/ (e.g. runs/agu63)")
    ap.add_argument("--config", default=None,
                    help="network config JSON (station roster fallback)")
    ap.add_argument("--held-out", default="WTZZ00DEU")
    ap.add_argument("--iono-window-min", type=float, default=15.0)
    ap.add_argument("--settle-hours", type=float, default=2.0,
                    help="convergence budget; bias panels reference and plot "
                         "only past this (measured total_IF budget is ~2 h)")
    ap.add_argument("--layout", choices=("mechanism", "products"),
                    default="mechanism",
                    help="'mechanism' (default): (a) iono field, (b) total_IF level + "
                         "datum events, (c) disc-interval vs arc maturity, (d) consumer. "
                         "'products': the original raw code/phase-bias ensembles in (b)/(c).")
    ap.add_argument("--total-if-window-min", type=float, default=20.0)
    ap.add_argument("--maturity-min", type=float, default=120.0,
                    help="arc age needed for resolvable sigma (~240 epochs at 30 s)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--dpi", type=int, default=200)
    args = ap.parse_args()

    run_dir = Path(args.run_dir)
    if not run_dir.is_absolute():
        run_dir = _REPO / run_dir
    log_dir = run_dir / "logs"
    out = Path(args.out) if args.out else run_dir / "agu_figure.png"

    sol = _read(log_dir / "network_solution.csv", "network_solution.csv",
                required=("tow", "kind", "value_m"))
    t0 = float(sol["tow"].min())
    if not np.isfinite(t0):
        raise SystemExit("network_solution.csv yields a non-finite run start")

    fig, axes = plt.subplots(2, 2, figsize=(13.6, 9.2))
    fig.patch.set_facecolor("white")

    stats = {}
    stats["iono"] = panel_iono(axes[0][0], log_dir,
                               Path(args.config) if args.config else None,
                               args.iono_window_min, args.held_out)
    _panel_tag(axes[0][0], "a")
    if args.layout == "mechanism":
        # (b) the product's own quality and the event that damages it; (c) why the
        # user cannot use it.  Shows the causal chain rather than the raw products,
        # whose datum-dependent levels invite the wrong question.
        stats["total_if"] = panel_total_if(axes[0][1], sol, t0,
                                           window_min=args.total_if_window_min)
        stats["disc"] = panel_disc_vs_maturity(axes[1][0], sol, t0,
                                               maturity_min=args.maturity_min,
                                               settle_h=args.settle_hours)
    else:
        stats["cb"] = _bias_panel(axes[0][1], sol, "CB", t0,
                                  title="Satellite code bias (per-signal OSB)",
                                  letter="b", settle_h=args.settle_hours)
        stats["pb"] = _bias_panel(axes[1][0], sol, "PB", t0,
                                  title="Satellite phase bias", letter="c",
                                  settle_h=args.settle_hours)
    stats["consumer"] = panel_consumer(axes[1][1], log_dir, t0, args.held_out,
                                       settle_h=args.settle_hours)

    fig.suptitle("Self-contained multi-GNSS PPP-RTK: network-estimated products "
                 "and held-out user recovery", fontsize=13, color=_INK, y=0.985)
    fig.tight_layout(rect=(0, 0, 1, 0.965))
    fig.savefig(out, dpi=args.dpi, facecolor="white")
    print(f"wrote {out}")
    for k, v in stats.items():
        print(f"  {k:9s} {v}")


if __name__ == "__main__":
    main()
