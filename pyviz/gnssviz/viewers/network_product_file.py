"""Network product-file viewer -- decodes the emitted ``network_products*.txt``.

Unlike :mod:`gnssviz.viewers.network_products` (which reads the producer-side
``network_solution.csv`` trace), this viewer parses the *serialized product file*
the network writes for consumers -- the exact bytes a downstream user ingests.
It is the "what did we actually ship?" view, decoded through
:func:`gnssviz.product_file.parse_product_file` (a re-derivation of the C++
``NetworkProductReader`` grammar).

Discovery
---------
There is no fixed CSV name: the file lives at ``network.product_output_path``
(relative paths land under the run's log dir).  The viewer globs the log dir for
``network_products*.txt`` and picks the newest by mtime -- so the chosen file and
its mtime are surfaced in the summary (a **stale-file hazard**: an older run's
product left in the same folder can win the glob).  Ad-hoc configs that point
``product_output_path`` at an absolute scratch path outside the log dir are
**not** found by this glob; those runs skip.

Figures
-------
1. **Per-sat URA timeline** (from ``U`` records), log-y, one line per satellite
   coloured by PRN, one tile per constellation.  ``R`` retro-bump epochs are
   ringed on the affected satellite; ``datum_changed`` epochs draw a faint
   vertical line labelled with the affected-constellation set.  Note that
   **datum-pivot satellites carry no U line by design** (their pin variance is
   bogus), so a gap is expected, not missing data.
2. **Per-epoch record counts** -- U lines, C lines, C-with-nonzero-drift (C1),
   and R-flagged satellites over the run.
"""

from __future__ import annotations

import glob
import os
from datetime import datetime
from typing import List

import numpy as np
import pandas as pd

from ..figures import Axes, Figure, Legend, Line, Scatter, Stem, VLine
from ..io import gps_to_datetime, load_trace
from ..palettes import color_by_prn
from ..product_file import parse_product_file
from .base import Viewer, ViewerResult, ViewerSkip

# Reuse the private per-constellation mapping/order used across the network
# viewers so tile layout matches network_products.py.
from .network_products import _SYS_OF, _SYS_ORDER


def _find_product_file(ctx) -> str:
    """Newest ``network_products*.txt`` under the run's log dir (or raise skip)."""
    hits = glob.glob(os.path.join(ctx.log_dir, "network_products*.txt"))
    if not hits:
        raise ViewerSkip(
            "no network_products*.txt in the log dir (network.product_output_path "
            "unset, zero epochs emitted so the deferred writer never opened, or an "
            "absolute scratch path outside the log dir)")
    return max(hits, key=os.path.getmtime)


def build_network_product_file(ctx) -> ViewerResult:
    path = _find_product_file(ctx)
    pf = parse_product_file(path)
    df = pf.to_frame()
    if df.empty:
        raise ViewerSkip(f"{os.path.basename(path)} parsed to zero product records")

    df = df.copy()
    df["sat"] = df["sat"].astype(str)
    df["system"] = df["sat"].map(lambda s: _SYS_OF.get(s[:1], "Unknown"))
    df["utc"] = gps_to_datetime(df["week"], df["tow"])
    sat_color = color_by_prn(df["sat"].tolist())

    figures: List[Figure] = []

    # --- Figure 1: per-sat URA timeline (log-y), per-constellation tiles -----
    ura = df[df["record"] == "U"]
    if not ura.empty:
        fig_ura = _ura_by_system(ura, df, sat_color)
        if fig_ura is not None:
            figures.append(fig_ura)

    # --- Figure 2: per-epoch record counts -----------------------------------
    figures.append(_counts_figure(df))

    summary = _summary(pf, df, path, ctx)
    return ViewerResult(figures=figures, summary=summary)


# --------------------------------------------------------------------------- #
def _ura_by_system(ura, df_all, sat_color) -> Figure:
    """One log-y tile per present constellation; R rings + datum_changed lines."""
    # Datum-change epochs (drawn as faint VLines on every tile) with their
    # affected-constellation label.
    dc = df_all[df_all["datum_changed"]]
    dc_marks = []
    if not dc.empty:
        seen = {}
        for _, r in dc.iterrows():
            t = r["utc"]
            seen.setdefault(t, str(r["datum_consts"] or ""))
        dc_marks = sorted(seen.items(), key=lambda kv: kv[0])

    present = [s for s in _SYS_ORDER if (ura["system"] == s).any()]
    axes_list: List[Axes] = []
    for sys_name in present:
        dd = ura[ura["system"] == sys_name]
        traces: list = []
        for sat in _ordered(dd["sat"].unique()):
            sub = dd[dd["sat"] == sat].sort_values("utc")
            t = sub["utc"].to_numpy("datetime64[ns]")
            v = sub["value"].to_numpy(float)
            v = np.where(v > 0.0, v, 1e-12)   # clamp for log axis (0 URA -> floor)
            traces.append(Line(x=t, y=v, color=sat_color[sat], width=0.9,
                               show_in_legend=False))
            # R retro-bump epochs on this sat -> black rings.
            rr = sub[sub["retro"].astype(bool)]
            if not rr.empty:
                rt = rr["utc"].to_numpy("datetime64[ns]")
                rv = rr["value"].to_numpy(float)
                rv = np.where(rv > 0.0, rv, 1e-12)
                traces.append(Scatter(x=rt, y=rv, size=26, marker="o",
                                      color=(1.0, 1.0, 1.0),
                                      edgecolor=(0.0, 0.0, 0.0), linewidth=1.1,
                                      show_in_legend=False))
        # datum_changed vertical marks (faint), labelled with datum_consts.
        for t, consts in dc_marks:
            traces.append(VLine(x=np.datetime64(t), color=(0.6, 0.6, 0.6),
                                style=":", width=0.8,
                                label=(f"datum:{consts}" if consts else "datum")))
        ax = Axes(title=sys_name, ylabel="clock URA (m, 1σ)", xlabel="GPS Time",
                  yscale="log", grid=True)
        ax.traces = traces
        axes_list.append(ax)

    if not axes_list:
        return None
    return Figure(name="Product URA", title="Emitted per-sat clock URA (product file)",
                  axes=axes_list, width=max(11.0, 5.0 * len(axes_list)),
                  height=min(11.0, 3.4 * len(axes_list)), link_x=True)


def _counts_figure(df) -> Figure:
    """Per-epoch counts of U / C / C-with-C1 / R-flagged records."""
    epochs = np.sort(df["utc"].unique()).astype("datetime64[ns]")

    # Precompute per-epoch series with a single grouped pass over the sorted keys.
    d = df.copy()
    d["is_u"] = (d["record"] == "U").astype(float)
    d["is_c"] = (d["record"] == "C").astype(float)
    d["is_c1"] = ((d["record"] == "C") & d["has_c1"].astype(bool)).astype(float)
    d["is_r"] = d["retro"].astype(bool)
    grp = d.groupby("utc")
    # groupby sorts keys by default, matching np.sort(epochs) above.
    n_u = grp["is_u"].sum().to_numpy(float)
    n_c = grp["is_c"].sum().to_numpy(float)
    n_c1 = grp["is_c1"].sum().to_numpy(float)
    # Distinct R-flagged sats per epoch.
    n_r = grp.apply(lambda x: x.loc[x["is_r"], "sat"].nunique()).to_numpy(float)

    ax = Axes(title="Per-epoch product record counts", ylabel="count",
              xlabel="GPS Time", legend=Legend(loc="best"))
    ax.traces = [
        Line(x=epochs, y=n_u, color=(0.13, 0.40, 0.70), width=1.2, label="U lines"),
        Line(x=epochs, y=n_c, color=(0.85, 0.45, 0.10), width=1.2, label="C lines"),
        Line(x=epochs, y=n_c1, color=(0.20, 0.60, 0.25), width=1.0,
             label="C w/ drift (C1)"),
        Stem(x=epochs, y=n_r, color=(0.85, 0.16, 0.16), marker="o",
             label="R-flagged sats"),
    ]
    return Figure(name="Product counts", title="Product-file record counts per epoch",
                  axes=[ax], width=12.0, height=4.5, link_x=True)


def _ordered(keys):
    def k(s):
        s = str(s)
        try:
            return (0, int(s[1:3]), s)
        except (ValueError, IndexError):
            return (1, 0, s)
    return sorted((str(x) for x in keys), key=k)


def _std(v) -> float:
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    return float(np.std(v, ddof=1)) if v.size > 1 else 0.0


def _summary(pf, df, path, ctx) -> str:
    out = ["=== PPP-RTK Network Product File ==="]
    mtime = datetime.fromtimestamp(os.path.getmtime(path))
    out.append(f"File           : {os.path.basename(path)}  "
               f"(mtime {mtime:%Y-%m-%d %H:%M:%S})")
    out.append(f"  (stale-file hazard: newest network_products*.txt in {ctx.log_dir})")
    out.append(f"Header version : {pf.header_version or 'unknown'}")
    out.append(f"Epochs (E)     : {pf.n_epochs}")
    if pf.unknown_tags:
        u = ", ".join(f"{t}x{n}" for t, n in sorted(pf.unknown_tags.items()))
        out.append(f"Unknown tags   : {u}")
    if pf.malformed:
        out.append(f"Malformed recs : {pf.malformed} (skipped)")

    # Decoded S contract (every field + realized pivots).
    out.append("")
    if pf.contract is not None:
        c = pf.contract
        out.append("--- S-basis contract (v2) ---")
        out.append(f"  reference_receiver_id : {c.reference_receiver_id}")
        out.append(f"  bias_sign             : {c.bias_sign:g}")
        out.append(f"  windup_yaw_model_id   : {c.windup_yaw_model_id}")
        out.append(f"  amb_datum             : {c.amb_datum} ({c.amb_datum_name})")
        out.append(f"  isb_datum             : {c.isb_datum} ({c.isb_datum_name})")
        out.append(f"  emit_code_bias        : {c.emit_code_bias}")
        out.append(f"  code_bias_datum       : {c.code_bias_datum} "
                   f"({c.code_bias_datum_name})")
        out.append(f"  disc_threshold_m      : {c.disc_threshold_m:g}")
        out.append(f"  emit_clock_disc       : {c.emit_clock_disc}")
        out.append(f"  apriori_product_id    : {c.apriori_product_id}")
        if c.clock_pivot_sat:
            piv = ", ".join(f"{k}:{v}" for k, v in sorted(c.clock_pivot_sat.items()))
            out.append(f"  realized pivots       : {piv}")
        else:
            out.append("  realized pivots       : (none)")
    else:
        out.append("--- S-basis contract: NONE (v1 / pre-B6 file) ---")

    # URA stats per constellation: median + std + worst-sat NAME.
    ura = df[df["record"] == "U"]
    out.append("")
    if not ura.empty:
        out.append("--- Clock URA (m) per constellation ---")
        out.append(f"{'Sys':<9} {'N':>6} {'median':>10} {'std':>10} {'worst-sat':>12}")
        out.append("-" * 50)
        for sys_name in _SYS_ORDER:
            sub = ura[ura["system"] == sys_name]
            if sub.empty:
                continue
            v = sub["value"].to_numpy(float)
            # Worst sat = highest per-sat median URA.
            med_by_sat = sub.groupby("sat")["value"].median()
            worst = med_by_sat.idxmax() if not med_by_sat.empty else "-"
            out.append(f"{sys_name:<9} {v.size:>6} {np.nanmedian(v):>10.4f} "
                       f"{_std(v):>10.4f} {str(worst):>12}")
        out.append("  (datum-pivot sats carry NO U line by design)")
    else:
        out.append("--- Clock URA: no U records (emit_ura off) ---")

    # R totals + c1-carrying count.
    n_retro = int(df["retro"].astype(bool).sum())
    n_c1 = int(((df["record"] == "C") & df["has_c1"].astype(bool)).sum())
    out.append("")
    out.append(f"R-flagged rows : {n_retro}")
    out.append(f"C rows w/ drift: {n_c1}")

    # L14: append network_contract_check.csv rows verbatim when present.
    if ctx.has_data("network_contract_check.csv"):
        out.append("")
        out.append("--- network_contract_check.csv (L14) ---")
        try:
            cc = load_trace(ctx.path("network_contract_check.csv"))
            out.append(", ".join(str(c) for c in cc.columns))
            for _, r in cc.iterrows():
                out.append(", ".join("" if pd.isna(x) else str(x) for x in r.tolist()))
        except Exception as exc:   # pragma: no cover - defensive
            out.append(f"(could not read: {exc})")

    return "\n".join(out)


viewer = Viewer(name="network_product_file",
                title="Network product file (emitted URA / contract / counts)",
                required_csvs=(), build=build_network_product_file)
