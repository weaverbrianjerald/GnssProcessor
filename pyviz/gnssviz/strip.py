"""Shared (satellite x epoch) strip-chart builder -- the port of
``plotSatEpochStrip.m``.

Driver viewers (``view_*_strip``) melt their CSV into a long-form table ``D`` with
columns ``system, satellite, facet, utc, value`` and call
:func:`build_sat_epoch_strip`, which returns one :class:`Figure` per
constellation (each with one tile per *facet* -- signal / signal-pair / none).
No plotting library is touched here: the figures are rendered later by a backend.

Parity with the MATLAB renderer:
* one shared colormap + colour-limits + colorbar across all constellations;
* satellites sorted by numeric PRN (chars 2-3), unparseable PRNs sorted last;
* ``NaN`` cells transparent; columns spread uniformly across the time span;
* robust auto colour-limits via *nearest-rank* 2nd/98th percentiles (matching the
  toolbox-free ``pctl`` in the MATLAB source), symmetric when diverging.
"""

from __future__ import annotations

from typing import Optional, Sequence

import numpy as np
import pandas as pd

from .colormaps import strip_colormap
from .figures import Axes, ColorBar, Figure, Markers, Raster, TextBox

_CANONICAL_SYS = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")


def _pctl(v: np.ndarray, q: float) -> float:
    """Toolbox-free nearest-rank percentile (matches the MATLAB ``pctl``)."""
    v = np.sort(v[np.isfinite(v)])
    n = v.size
    if n == 0:
        return float("nan")
    # MATLAB round() is half-away-from-zero; Python round() is banker's, which
    # picks a different rank on exact .5 landings (every ~100 samples for q=2/98)
    # and would shift the auto colour-limits.  Argument is non-negative here.
    idx = int(np.floor(q / 100.0 * (n - 1) + 0.5))
    idx = min(max(idx, 0), n - 1)
    return float(v[idx])


def _resolve_caxis(caxis, vals: np.ndarray, diverging: bool) -> tuple:
    """Numeric passthrough or robust auto limits (2nd/98th pct), symmetric if diverging."""
    if isinstance(caxis, (list, tuple, np.ndarray)) and len(caxis) == 2:
        return (float(caxis[0]), float(caxis[1]))
    v = np.asarray(vals, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return (0.0, 1.0)
    sym = (caxis == "autosym") or (diverging and caxis == "auto")
    if sym:
        m = _pctl(np.abs(v), 98)
        if not (m > 0):
            m = float(np.max(np.abs(v)))
        if not (m > 0):
            m = 1.0
        return (-m, m)
    lo, hi = _pctl(v, 2), _pctl(v, 98)
    if not (hi > lo):
        hi = lo + max(np.finfo(float).eps, abs(lo))
    return (lo, hi)


def _prn_sort(sats: Sequence[str]) -> list:
    """Sort satellite IDs by numeric PRN (chars 2-3); unparseable PRNs last."""
    uniq = list(dict.fromkeys(str(s) for s in sats))   # stable unique

    def key(s):
        try:
            return (0, int(s[1:3]))
        except (ValueError, IndexError):
            return (1, 0)
    return sorted(uniq, key=key)


def build_sat_epoch_strip(
    D: pd.DataFrame,
    *,
    mode: str = "continuous",
    colormap="parula",
    diverging: bool = False,
    caxis="auto",
    annotate: bool = True,
    ref_ticks: Optional[Sequence[float]] = None,
    fig_title: str = "",
    value_label: str = "",
    subtitle: str = "",
    fig_tag: str = "strip",
    sys_order: Sequence[str] = _CANONICAL_SYS,
    cat: Optional[dict] = None,                # {'colors': (K,3), 'labels': [...]}
    markers: Optional[pd.DataFrame] = None,
    marker_styles: Optional[dict] = None,
    facet_label_fallback: str = "",
) -> list:
    """Return a list of :class:`Figure` (one per present constellation)."""
    if D is None or len(D) == 0:
        return []

    D = D.copy()
    for c in ("system", "satellite", "facet"):
        D[c] = D[c].astype("string").fillna("")

    is_cat = mode.lower() == "categorical"
    if is_cat and (cat is None or "colors" not in cat):
        raise ValueError("categorical mode requires cat={'colors':(K,3),'labels':[...]}")

    epochs = np.sort(pd.unique(D["utc"]))
    n_epochs = epochs.size
    # Key the column lookup on integer nanoseconds, not datetime objects, so a
    # Timestamp-vs-datetime64 hash mismatch can never silently drop cells.
    epoch_key = epochs.astype("datetime64[ns]").astype("int64")
    epoch_pos = {int(k): j for j, k in enumerate(epoch_key)}

    # Shared colormap + colour limits resolved once (uniform across figures).
    if is_cat:
        cmap = np.asarray(cat["colors"], dtype=float)
        k = cmap.shape[0]
        clim = (0.5, k + 0.5)
    else:
        cm_name = colormap
        if diverging and isinstance(cm_name, str) and cm_name.lower() == "parula":
            cm_name = "diverging"
        cmap = strip_colormap(cm_name, 256)
        clim = _resolve_caxis(caxis, D["value"].to_numpy(dtype=float), diverging)

    sys_present = [s for s in sys_order if (D["system"] == s).any()]
    figures: list = []

    for sys_name in sys_present:
        d_sys = D[D["system"] == sys_name]
        if d_sys.empty:
            continue

        facets = sorted(set(d_sys["facet"].tolist()))
        if not facets:
            facets = [""]
        n_f = len(facets)

        axes_list: list = []
        for r, fac in enumerate(facets):
            sub = d_sys[d_sys["facet"] == fac]
            if sub.empty:
                continue
            sats = _prn_sort(sub["satellite"].tolist())
            n_sats = len(sats)
            sat_pos = {s: i for i, s in enumerate(sats)}

            grid = np.full((n_sats, n_epochs), np.nan)
            si = sub["satellite"].map(sat_pos).to_numpy()
            ev = sub["utc"].to_numpy("datetime64[ns]").astype("int64")
            ei = pd.Series(ev).map(epoch_pos).to_numpy()
            vv = sub["value"].to_numpy(dtype=float)
            ok = ~pd.isna(si) & ~pd.isna(ei)
            ri, ci = si[ok].astype(int), ei[ok].astype(int)
            grid[ri, ci] = vv[ok]

            # Optional per-cell annotation mask: cells with annot=False are still
            # drawn but excluded from the mu/sigma/N stat (e.g. synthetic pivot
            # rows in the residual strip, which MATLAB omits from the read-out).
            annot_grid = None
            if "annot" in sub.columns:
                annot_grid = np.zeros((n_sats, n_epochs), dtype=bool)
                annot_grid[ri, ci] = sub["annot"].to_numpy(bool)[ok]

            traces = [Raster(x=epochs, row_labels=sats, grid=grid)]

            # event glyphs overlaid for this (system, facet) tile
            traces += _marker_traces(markers, marker_styles, sys_name, fac, sat_pos)

            if (not is_cat) and annotate:
                stat_mask = np.isfinite(grid)
                if annot_grid is not None:
                    stat_mask &= annot_grid
                v = grid[stat_mask]
                if v.size:
                    sd = float(v.std(ddof=1)) if v.size > 1 else 0.0
                    traces.append(TextBox(
                        x=0.995, y=0.94,
                        text=fr"$\mu$={_g(v.mean())}  $\sigma$={_g(sd)}  N={v.size}",
                        ha="right", va="top", boxed=True))

            ylabel = (str(fac) if fac != "" else facet_label_fallback)
            axes_list.append(Axes(
                traces=traces,
                xlabel=("GPS Time" if r == n_f - 1 else ""),
                ylabel=ylabel,
                x_is_datetime=True,
            ))

        if not axes_list:
            continue

        cbar = ColorBar(label=value_label)
        if is_cat:
            cbar.ticks = list(range(1, cmap.shape[0] + 1))
            cbar.tick_labels = list(cat.get("labels", []))
        elif ref_ticks is not None:
            cbar.ticks = list(ref_ticks)

        fig_height = min(9.5, 1.75 * n_f + 1.5)
        figures.append(Figure(
            name=f"{fig_tag} -- {sys_name}",
            title=(f"{fig_title} -- {sys_name}" if fig_title else sys_name),
            subtitle=subtitle,
            axes=axes_list,
            width=14.0,
            height=fig_height,
            link_x=True,
            cmap=cmap,
            clim=clim,
            colorbar=cbar,
            categorical=is_cat,
        ))

    return figures


def _marker_traces(markers, styles, sys_name, fac, sat_pos) -> list:
    if markers is None or not isinstance(markers, pd.DataFrame) or markers.empty:
        return []
    m = markers[(markers["system"].astype(str) == sys_name)
                & (markers["facet"].astype(str) == str(fac))]
    if m.empty:
        return []
    styles = styles or {}
    out = []
    for kind, grp in m.groupby(m["kind"].astype(str)):
        rows, xs = [], []
        for _, e in grp.iterrows():
            row = sat_pos.get(str(e["satellite"]))
            if row is not None:
                rows.append(row)
                xs.append(e["utc"])
        if not rows:
            continue
        st = styles.get(kind, {})
        out.append(Markers(
            x=np.array(xs), rows=rows,
            marker=st.get("marker", "o"),
            color=st.get("color", (0, 0, 0)),
            size=st.get("size", 6.0),
            label=kind,
        ))
    return out


def _g(x: float) -> str:
    """Compact %.3g formatting (matches MATLAB ``%.3g`` in the annotation)."""
    return f"{x:.3g}"
