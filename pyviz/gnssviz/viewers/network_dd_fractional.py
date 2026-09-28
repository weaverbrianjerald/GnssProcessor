"""Network cross-station DD fractional-cycle viewer: the AR go/no-go instrument.

Port of ``scripts/view_network_dd_fractional.py`` (B5). With ``network.trace_dd_fractional``
on, the network producer writes ``network_dd_fractional.csv``: the between-station
double-differenced undifferenced float ambiguities per (constellation, signal), including
the fractional-cycle residual ``frac_cyc = dd_cyc - round(dd_cyc)``. This is the go/no-go
instrument for float-bias user AR: if the emitted phase biases are AR-grade the DD
ambiguities cluster on integers and ``frac_cyc`` piles up near 0 (equivalently ±0.5 is
empty). A smeared, flat histogram means the bias product does not yet support
<0.1-cycle user AR.

The producer already excludes the pivot station and GLONASS; this viewer does not
re-filter.

Figures
-------
* **A**: one tile per (const, signal), capped to the ~8 largest-n groups (disclosed in
  the tile / summary), a 50-bin fixed histogram over (-0.5, 0.5] drawn as a step
  :class:`Line` (no ``Bar`` primitive), a uniform-density :class:`HLine` (n/50), and
  :class:`VLine` markers at ±0.1 cyc. Title carries n, the replicated robust sigma, and
  the within-±0.1-cyc percentage.
* **B** (net-new): per-group robust sigma vs GPS time, one :class:`Line` per group; a
  slowly-shrinking or stable sigma is the healthy signature.

The robust-sigma estimator is replicated exactly from the standalone script (upper-median
of ``|frac|`` without interpolation, times 1.4826) so a 3-decimal cross-check matches:
``np.median`` would differ for even ``n``.
"""

from __future__ import annotations

from typing import List

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, TextBox, VLine
from ..identifiers import system_of
from ..io import add_utc, load_trace, require_cols
from ..palettes import distinguishable_colors, system_color
from .base import Viewer, ViewerResult, ViewerSkip

# Cap Figure A to the busiest groups so a many-signal run stays legible.
_MAX_TILES = 8
# Fixed 50-bin histogram over the wrapped fractional-cycle domain (-0.5, 0.5].
_N_BINS = 50
_WITHIN = 0.1          # cyc: the AR go/no-go band
_GO_PCT = 90.0         # % within ±0.1 cyc to call a group AR-grade


def wrap_half(x):
    """Wrap a fractional cycle into (-0.5, 0.5]  (frac - round(frac))."""
    return x - np.round(x)


def robust_std(vals) -> float:
    """Robust sigma == 1.4826 * upper-median(|frac|), replicated exactly.

    Mirrors the standalone script's ``robust_std``: sort ``|v|`` ascending and
    take the element at integer index ``len//2`` (the upper median for even n,
    no interpolation). ``np.median`` averages the two middle elements for even
    n and would disagree at the 3rd decimal, so this keeps the hand-rolled index.
    """
    v = [float(x) for x in vals if np.isfinite(x)]
    if not v:
        return 0.0
    s = sorted(abs(x) for x in v)
    mad = s[len(s) // 2]
    return 1.4826 * mad


def build_network_dd_fractional(ctx) -> ViewerResult:
    # The driver normally guards via required_csvs, but keep the build fn safe to
    # call directly (and skip gracefully on a header-only / missing trace).
    if not ctx.has_data("network_dd_fractional.csv"):
        raise ViewerSkip("network_dd_fractional.csv absent or empty")
    df = load_trace(ctx.path("network_dd_fractional.csv"))
    require_cols(df, ("week", "tow", "cons", "signal", "sat_i", "ref_sat",
                      "sta_a", "sta_b", "dd_m", "lambda_m", "dd_cyc", "frac_cyc"),
                 "network_dd_fractional.csv")
    if df.empty:
        raise ViewerSkip("network_dd_fractional.csv has no rows")

    df = df.copy()
    df["lambda_m"] = df["lambda_m"].astype(float)
    df["frac_cyc"] = df["frac_cyc"].astype(float)
    # Skip rows with no wavelength (unknown signal -> cycle columns are 0).
    df = df[df["lambda_m"] > 0.0]
    df = df[np.isfinite(df["frac_cyc"].to_numpy(float))]
    if df.empty:
        raise ViewerSkip("network_dd_fractional.csv: no rows with a known wavelength")

    df["cons"] = df["cons"].astype(str)
    df["signal"] = df["signal"].fillna("").astype(str)
    df["frac"] = wrap_half(df["frac_cyc"].to_numpy(float))
    # Optional formal DD sigma (cycles); −1 or absent means covariance unavailable (T2a).
    has_sigma = "dd_sigma_cyc" in df.columns
    if has_sigma:
        df["dd_sigma_cyc"] = df["dd_sigma_cyc"].astype(float)
    df = add_utc(df)

    # --- Aggregate per (cons, signal) once (the file is O(sta*sat*epoch)). ---
    groups = []   # list of dicts: cons, signal, sys, n, sigma, pct, frac(np), color, ...
    for (cons, sig), sub in df.groupby(["cons", "signal"], sort=True):
        frac = sub["frac"].to_numpy(float)
        n = frac.size
        if n == 0:
            continue
        sys = system_of(cons)                 # single-letter -> canonical name
        within = float(np.sum(np.abs(frac) < _WITHIN)) / n * 100.0
        mean_abs = float(np.mean(np.abs(frac)))                    # unif -> 0.25
        circ_R = float(np.abs(np.mean(np.exp(2j * np.pi * frac)))) # unif -> 0
        # Per-series std of dd_cyc per (sat,ref,sta_a,sta_b): the temporal-drift signature.
        ser_std_med = float("nan")
        if "dd_cyc" in sub.columns:
            gk = sub.groupby(["sat_i", "ref_sat", "sta_a", "sta_b"])["dd_cyc"]
            stds = gk.std().to_numpy(float)
            stds = stds[np.isfinite(stds)]
            if stds.size:
                ser_std_med = float(np.median(stds))
        # σ calibration (χ² intent): per-series temporal scatter vs the formal DD σ.
        # calib = scatter/formal: about 1 is well calibrated, much greater than 1 is
        # overconfident (σ derived from the same data it grades), much less than 1 is
        # conservative.
        med_sig = float("nan")
        calib = float("nan")
        if has_sigma:
            sv = sub["dd_sigma_cyc"].to_numpy(float)
            sv = sv[np.isfinite(sv) & (sv > 0.0)]
            if sv.size:
                med_sig = float(np.median(sv))
                if med_sig > 0.0 and ser_std_med == ser_std_med:
                    calib = ser_std_med / med_sig
        groups.append({
            "cons": cons, "signal": sig, "sys": sys,
            "n": n, "sigma": robust_std(frac), "pct": within,
            "mean_abs": mean_abs, "circ_R": circ_R, "med_sig": med_sig,
            "calib": calib, "ser_std_med": ser_std_med,
            "frac": frac, "utc": sub["utc"], "color": system_color(sys),
        })

    if not groups:
        raise ViewerSkip("network_dd_fractional.csv: no (cons, signal) samples")

    groups.sort(key=lambda g: g["n"], reverse=True)
    total_groups = len(groups)
    shown = groups[:_MAX_TILES]
    capped = total_groups > _MAX_TILES

    figures: List[Figure] = []
    figures.append(_figure_histograms(shown, total_groups, capped))
    figures.append(_figure_sigma_over_time(shown))

    return ViewerResult(figures=figures,
                        summary=_summary(groups, total_groups, capped))


# --------------------------------------------------------------------------- #
def _figure_histograms(shown, total_groups, capped) -> Figure:
    """Figure A: one 50-bin fixed histogram tile per (cons, signal)."""
    edges = np.linspace(-0.5, 0.5, _N_BINS + 1)
    centers = 0.5 * (edges[:-1] + edges[1:])
    axes_list: List[Axes] = []
    for g in shown:
        counts, _ = np.histogram(g["frac"], bins=edges)
        # Step outline through bin centers (no Bar primitive available).
        step_x, step_y = _step_xy(edges, counts)
        uniform = g["n"] / _N_BINS
        title = (f"{g['sys']} {g['signal']}  n={g['n']}  "
                 f"sigma={g['sigma']:.3f} cyc  {g['pct']:.1f}% within +/-0.1")
        ax = Axes(
            title=title, xlabel="fractional cycle", ylabel="count",
            x_is_datetime=False, xlim=(-0.5, 0.5), grid=True,
            traces=[
                Line(x=step_x, y=step_y, color=g["color"], width=1.4,
                     show_in_legend=False),
                HLine(y=uniform, color=(0.55, 0.55, 0.55), style=":",
                      label="uniform density n/50"),
                VLine(x=-_WITHIN, color=(0.20, 0.57, 0.55), style="--",
                      width=1.0, label="+/-0.1 cyc"),
                VLine(x=+_WITHIN, color=(0.20, 0.57, 0.55), style="--",
                      width=1.0),
            ],
        )
        axes_list.append(ax)

    subtitle = "tight near 0 = AR-grade biases"
    if capped:
        subtitle += (f"  (showing {len(shown)} of {total_groups} groups, "
                     f"largest-n first)")
    return Figure(name="Network DD fractional (histogram)",
                  title="Cross-station DD fractional cycle",
                  subtitle=subtitle, axes=axes_list,
                  width=max(11.0, 4.6 * min(len(axes_list), 3)),
                  height=min(11.0, 2.9 * len(axes_list) + 0.6), link_x=False)


def _figure_sigma_over_time(shown) -> Figure:
    """Figure B (net-new): per-group robust sigma vs GPS time, one Line each."""
    colors = distinguishable_colors(len(shown))
    traces = []
    for g, c in zip(shown, colors):
        t, sig = _sigma_series(g["utc"], g["frac"])
        if t.size == 0:
            continue
        traces.append(Line(x=t, y=sig, color=c, width=1.1,
                           label=f"{g['sys']} {g['signal']}"))
    ax = Axes(title="Per-group robust sigma over time (shrinking / stable = healthy)",
              xlabel="GPS Time", ylabel="robust sigma (cyc)",
              x_is_datetime=True, grid=True, legend=Legend(loc="best"),
              traces=traces)
    if not traces:
        ax.traces = [TextBox(x=0.5, y=0.5, text="(single-epoch run: no sigma series)",
                             ha="center", va="center")]
    return Figure(name="Network DD fractional (sigma over time)",
                  title="Cross-station DD fractional-cycle spread over time",
                  axes=[ax], width=12.0, height=4.4, link_x=True)


def _sigma_series(utc, frac):
    """Per-epoch robust sigma of the wrapped fractional cycle for one group."""
    epochs = np.sort(np.unique(utc.to_numpy("datetime64[ns]")))
    ev = utc.to_numpy("datetime64[ns]")
    ts, ss = [], []
    for e in epochs:
        vals = frac[ev == e]
        if vals.size:
            ts.append(e)
            ss.append(robust_std(vals))
    return np.array(ts, dtype="datetime64[ns]"), np.array(ss, dtype=float)


def _step_xy(edges, counts):
    """Build an outline (step) polyline for a histogram, dropping to 0 at ends."""
    xs = [edges[0]]
    ys = [0.0]
    for i in range(len(counts)):
        xs.extend([edges[i], edges[i + 1]])
        ys.extend([float(counts[i]), float(counts[i])])
    xs.append(edges[-1])
    ys.append(0.0)
    return np.array(xs, dtype=float), np.array(ys, dtype=float)


def _summary(groups, total_groups, capped) -> str:
    out = ["=== Network cross-station DD fractional cycle (user-AR go/no-go) ==="]
    out.append(f"Groups (cons,signal): {total_groups}"
               + (f"  (Figure A shows the {_MAX_TILES} largest-n)" if capped else ""))
    out.append("Pivot station and GLONASS already excluded by the producer.")
    out.append("UNIFORM BENCHMARK (no integer info): mean|frac|=0.250  P(<0.1)=20.0%  R=0.00")
    out.append("")
    out.append(f"{'Group':<12} {'N':>7} {'mean|f|':>8} {'P<0.1%':>7} {'R':>6} "
               f"{'sig(cyc)':>9} {'calib':>6} {'ser_std':>8}  verdict")
    out.append("-" * 82)
    for g in groups:
        verdict = "GO" if g["pct"] >= _GO_PCT else "NO-GO"
        label = f"{g['sys']} {g['signal']}"
        calib = g.get("calib", float("nan"))
        calib_s = f"{calib:>6.2f}" if calib == calib else f"{'--':>6}"   # nan -> --
        sser = g.get("ser_std_med", float("nan"))
        sser_s = f"{sser:>8.3f}" if sser == sser else f"{'--':>8}"
        out.append(f"{label:<12} {g['n']:>7} {g['mean_abs']:>8.3f} {g['pct']:>7.1f} "
                   f"{g['circ_R']:>6.3f} {g['sigma']:>9.3f} {calib_s} {sser_s}  {verdict}")
    out.append("")
    n_go = sum(1 for g in groups if g["pct"] >= _GO_PCT)
    if n_go == total_groups:
        out.append(f"ADVISORY: GO -- all {total_groups} groups >= {_GO_PCT:.0f}% "
                   "within +/-0.1 cyc; biases support float-AR.")
    elif n_go == 0:
        out.append(f"ADVISORY: NO-GO -- no group reaches {_GO_PCT:.0f}% within "
                   "+/-0.1 cyc; bias product not yet AR-grade.")
    else:
        out.append(f"ADVISORY: MIXED -- {n_go}/{total_groups} groups GO "
                   f"(>= {_GO_PCT:.0f}% within +/-0.1 cyc); the rest are NO-GO.")
    return "\n".join(out)


viewer = Viewer(name="network_dd_fractional",
                title="Network DD fractional cycle (user-AR go/no-go)",
                required_csvs=("network_dd_fractional.csv",),
                build=build_network_dd_fractional)
