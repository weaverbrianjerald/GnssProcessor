"""Network satellite-product viewer: the emitted clock and bias time series.

Reads ``network_solution.csv`` (the PPP-RTK network producer's per-epoch, per-
parameter solution: estimable satellite clock, phase bias, and, when enabled,
satellite code bias) and plots each product over time, one constellation per
tile, one line per satellite coloured by PRN:

  1. Estimable satellite clock correction (m).
  2. Satellite phase bias (m), one line per (sat, signal); discontinuity bumps
     (the ``disc_counter`` increments the consumer turns into ambiguity resets)
     are ringed.
  3. Satellite code bias / OSB (m), only when the run estimated it.

This is the product-quality view: a clean, slowly-varying clock and a piecewise-
constant phase bias indicate a consumable network product.
"""

from __future__ import annotations

from typing import List

import numpy as np

from ..figures import Axes, Figure, Legend, Line, Scatter, TextBox
from ..io import add_utc, load_trace, require_cols
from ..palettes import color_by_prn
from .base import Viewer, ViewerResult, ViewerSkip

_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")
_SYS_OF = {"G": "GPS", "E": "Galileo", "R": "GLONASS", "C": "BeiDou",
           "J": "QZSS", "S": "SBAS"}


def build_network_products(ctx) -> ViewerResult:
    df = load_trace(ctx.path("network_solution.csv"))
    require_cols(df, ("week", "tow", "sat", "kind", "signal", "value_m",
                      "disc_counter"), "network_solution.csv")
    if df.empty:
        raise ViewerSkip("network_solution.csv has no rows")

    df = df.copy()
    df["sat"] = df["sat"].astype(str)
    df["kind"] = df["kind"].astype(str)
    df["signal"] = df["signal"].fillna("").astype(str)
    df["system"] = df["sat"].map(lambda s: _SYS_OF.get(s[:1], "Unknown"))
    df = add_utc(df)
    sat_color = color_by_prn(df["sat"].tolist())
    sys_present = [s for s in _SYS_ORDER if (df["system"] == s).any()]

    figures: List[Figure] = []
    clk = df[df["kind"] == "CLK"]
    if not clk.empty:
        figures.append(_by_system(clk, sat_color, sys_present, "key_sat",
                                  "Estimable sat clock (m)",
                                  "Estimable satellite clock correction",
                                  "Sat clock"))
    pb = df[df["kind"] == "PB"]
    if not pb.empty:
        figures.append(_by_system(pb, sat_color, sys_present, "key_sat_sig",
                                  "Phase bias (m)",
                                  "Satellite phase bias (one line per sat·signal)",
                                  "Sat phase bias", rings=True))
    cb = df[df["kind"] == "CB"]
    if not cb.empty:
        figures.append(_by_system(cb, sat_color, sys_present, "key_sat_sig",
                                  "Code bias / OSB (m)",
                                  "Satellite code bias (OSB)", "Sat code bias"))

    if not figures:
        raise ViewerSkip("network_solution.csv has no CLK/PB/CB rows")

    summary = _summary(df, sys_present)
    # L14 hook: append the machine-readable S-contract check (network_contract_check.csv,
    # written by the producer whenever it writes a product file) when present. It is
    # not a required_csv, so the block does not appear on a run that never checked.
    if ctx.has_data("network_contract_check.csv"):
        cc = load_trace(ctx.path("network_contract_check.csv"))
        if {"check", "status", "detail"}.issubset(cc.columns) and not cc.empty:
            lines = ["", "=== S-basis contract check (network_contract_check.csv) ==="]
            for _, r in cc.iterrows():
                det = str(r.get("detail", "") or "").strip()
                lines.append(f"  {str(r['check']):<20} {str(r['status']):<5}"
                             + (f"  {det}" if det else ""))
            summary += "\n" + "\n".join(lines)

    return ViewerResult(figures=figures, summary=summary)


# --------------------------------------------------------------------------- #
def _by_system(d, sat_color, sys_present, key_mode, ylabel, title, name,
               *, rings: bool = False) -> Figure:
    axes_list: List[Axes] = []
    present = [s for s in sys_present if (d["system"] == s).any()]
    for sys_name in present:
        dd = d[d["system"] == sys_name]
        ax = Axes(title=sys_name, ylabel=ylabel, xlabel="GPS Time", grid=True)
        traces = []
        # One line per series (sat, or sat+signal).
        keys = (dd["sat"] if key_mode == "key_sat"
                else dd["sat"] + "|" + dd["signal"])
        for key in _ordered(keys.unique()):
            sub = dd[keys == key].sort_values("utc")
            t = sub["utc"].to_numpy("datetime64[ns]")
            v = sub["value_m"].to_numpy(float)
            sat = str(sub["sat"].iloc[0])
            traces.append(Line(x=t, y=v, color=sat_color[sat], width=0.9,
                               show_in_legend=False))
            if rings and "disc_counter" in sub.columns:
                dc = sub["disc_counter"].to_numpy(float)
                jump = np.zeros(dc.size, dtype=bool)
                jump[1:] = dc[1:] > dc[:-1]
                if jump.any():
                    traces.append(Scatter(x=t[jump], y=v[jump], size=18,
                                          marker="o", color=(0.0, 0.0, 0.0),
                                          edgecolor=(0.0, 0.0, 0.0), linewidth=0.8,
                                          show_in_legend=False))
        vv = dd["value_m"].to_numpy(float)
        vv = vv[np.isfinite(vv)]
        if vv.size:
            traces.append(TextBox(x=0.98, y=0.97,
                                  text=f"N={vv.size}  μ={vv.mean():.3g}  "
                                       f"σ={_std(vv):.3g}", ha="right", va="top",
                                  boxed=True))
        ax.traces = traces
        axes_list.append(ax)
    return Figure(name=name, title=title, axes=axes_list,
                  width=max(11.0, 5.0 * len(axes_list)),
                  height=min(11.0, 3.4 * len(axes_list)), link_x=True)


def _ordered(keys):
    def k(s):
        s = str(s)
        try:
            return (0, int(s[1:3]), s)
        except (ValueError, IndexError):
            return (1, 0, s)
    return sorted((str(x) for x in keys), key=k)


def _std(v):
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    return float(np.std(v, ddof=1)) if v.size > 1 else 0.0


def _summary(df, sys_present) -> str:
    out = ["=== PPP-RTK Network Satellite Products ==="]
    out.append(f"Epochs       : {df['utc'].nunique()} "
               f"({df['utc'].min()} .. {df['utc'].max()})")
    for kind, lab in (("CLK", "sat clocks"), ("PB", "phase biases"),
                      ("CB", "code biases")):
        sub = df[df["kind"] == kind]
        if not sub.empty:
            n_disc = int((sub.groupby(["sat", "signal"])["disc_counter"]
                          .max() > 0).sum()) if kind == "PB" else 0
            extra = f", {n_disc} (sat·sig) with discontinuities" if kind == "PB" else ""
            out.append(f"{lab:<13}: {sub['sat'].nunique()} sats, "
                       f"{len(sub)} rows{extra}")
    clk = df[df["kind"] == "CLK"]
    if not clk.empty:
        out.append("")
        out.append(f"{'Sat':<5} {'N':>5} {'meanClk(m)':>11} {'stdClk(m)':>10}")
        out.append("-" * 34)
        for sat in _ordered(clk["sat"].unique()):
            s = clk[clk["sat"] == sat]["value_m"].to_numpy(float)
            out.append(f"{sat:<5} {len(s):>5} {np.nanmean(s):>11.3f} "
                       f"{_std(s):>10.3f}")
    return "\n".join(out)


viewer = Viewer(name="network_products",
                title="Network satellite products (clock & bias)",
                required_csvs=("network_solution.csv",),
                build=build_network_products)
