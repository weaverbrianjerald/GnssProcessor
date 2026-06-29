"""Matplotlib implementation of the rendering :class:`Backend`.

Interprets a :class:`gnssviz.figures.Figure` into a matplotlib ``Figure``.  This
is the *only* module in the package that imports matplotlib; the plotly backend
interprets the same specs without touching this file.

Fidelity notes vs the MATLAB suite
----------------------------------
* :class:`Raster` / :class:`RgbRaster` are drawn with ``imshow`` over a date-num
  ``extent`` so columns spread uniformly across the time span -- exactly how
  MATLAB ``imagesc`` lays out the (sat x epoch) grid -- with untracked cells
  transparent and ``YDir`` reversed (row 0 at the top).
* Dual y-axis (``axis == 'right'``) uses a cached ``twinx``.
* A figure-level colorbar/categorical legend is drawn once for raster figures;
  individual axes may also carry their own colorbar (coloured scatter / sky plot).
* ``projection == 'polar'`` renders a sky plot (compass-north zero, clockwise).
"""

from __future__ import annotations

import io
from typing import Optional

import matplotlib
matplotlib.use("Agg", force=False)   # headless-safe default; show() re-checks
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import ListedColormap

from ..figures import (Axes, Band, ColorBar, Figure, HLine, Line, Markers,
                       Mesh, PolarScatter, Raster, RgbRaster, Scatter,
                       StackedArea, Stem, TextBox, VLine)
from .base import Backend


class MatplotlibBackend(Backend):
    name = "matplotlib"

    # ------------------------------------------------------------------ #
    def render(self, fig: Figure):
        n = max(1, len(fig.axes))
        # Safety net: guarantee enough height for the tile count so constrained
        # layout never collapses axes to zero on tall multi-tile figures.
        height = max(fig.height, 1.05 * n + 0.6)
        mfig = plt.figure(figsize=(fig.width, height), layout="constrained")
        if mfig.canvas.manager is not None:
            mfig.canvas.manager.set_window_title(fig.name)
        gs = mfig.add_gridspec(n, 1)

        fig_cmap = ListedColormap(fig.cmap) if fig.cmap is not None else None
        maxes, raster_mappables, axes_mappables = [], [], []
        first_rect = None

        specs = fig.axes or [Axes()]
        for i, spec in enumerate(specs):
            proj = "polar" if spec.projection == "polar" else None
            share = first_rect if (fig.link_x and proj is None
                                   and first_rect is not None) else None
            ax = mfig.add_subplot(gs[i, 0], projection=proj, sharex=share)
            if proj is None and first_rect is None:
                first_rect = ax
            raster_m, point_m = self._draw_axes(spec, ax, fig, fig_cmap)
            maxes.append(ax)
            if raster_m is not None:
                raster_mappables.append(raster_m)
            # per-axes colorbar (coloured scatter / sky plot)
            if spec.colorbar is not None and (point_m or raster_m) is not None:
                self._axes_colorbar(mfig, ax, point_m or raster_m, spec.colorbar)

        # Figure-wide colorbar shared across raster tiles.
        if fig.colorbar is not None and raster_mappables:
            self._figure_colorbar(mfig, maxes, raster_mappables[-1], fig)

        if fig.title:
            mfig.suptitle(fig.title)
        if fig.subtitle and not maxes[0].get_title():
            maxes[0].set_title(fig.subtitle, fontsize=8, color=(0.35, 0.35, 0.35))
        return mfig

    # ------------------------------------------------------------------ #
    def _draw_axes(self, spec: Axes, ax, fig: Figure, fig_cmap):
        if spec.projection == "polar":
            return None, self._draw_polar(spec, ax)
        if spec.projection == "geo":
            return self._draw_geo(spec, ax, fig, fig_cmap)

        ax_cmap = ListedColormap(spec.cmap) if spec.cmap is not None else fig_cmap
        raster_m, point_m, twin = self._draw_traces(spec, ax, fig, ax_cmap)
        self._decorate(spec, ax, twin)
        return raster_m, point_m

    # ------------------------------------------------------------------ #
    def _draw_traces(self, spec: Axes, ax, fig: Figure, ax_cmap):
        """Draw every trace of ``spec`` onto rectangular/geo axes ``ax``.

        Returns ``(raster_mappable, point_mappable, twin)`` -- the raster (or
        Mesh) mappable and the coloured-scatter mappable for colorbars, plus the
        lazily-created secondary-y handle the caller's ``_decorate`` needs.
        """
        twin = {"ax": None}
        raster_m = point_m = None
        vmin, vmax = (spec.clim if spec.clim is not None else (None, None))

        def right():
            if twin["ax"] is None:
                twin["ax"] = ax.twinx()
            return twin["ax"]

        for tr in spec.traces:
            if isinstance(tr, Line):
                tgt = right() if tr.axis == "right" else ax
                tgt.plot(np.asarray(tr.x), np.asarray(tr.y, dtype=float), tr.style,
                         color=tuple(tr.color), linewidth=tr.width,
                         label=(tr.label if tr.show_in_legend else "_nolegend_"))
            elif isinstance(tr, Scatter):
                tgt = right() if tr.axis == "right" else ax
                kw = dict(s=tr.size, marker=tr.marker, alpha=tr.alpha,
                          label=(tr.label if tr.show_in_legend else "_nolegend_"))
                if tr.edgecolor is not None:
                    kw["edgecolors"] = tuple(tr.edgecolor)
                    kw["linewidths"] = tr.linewidth
                if tr.zorder is not None:
                    kw["zorder"] = tr.zorder
                if tr.c is not None:
                    # Per-point scalar -> coloured through the axes cmap/clim; the
                    # mappable is surfaced so a colorbar can attach.
                    point_m = tgt.scatter(np.asarray(tr.x),
                                          np.asarray(tr.y, dtype=float),
                                          c=np.asarray(tr.c, dtype=float),
                                          cmap=ax_cmap, vmin=vmin, vmax=vmax, **kw)
                else:
                    tgt.scatter(np.asarray(tr.x), np.asarray(tr.y, dtype=float),
                                color=tuple(tr.color), **kw)
            elif isinstance(tr, Band):
                lo = np.asarray(tr.lo, dtype=float)
                hi = np.asarray(tr.hi, dtype=float)
                mask = np.isfinite(lo) & np.isfinite(hi)
                ax.fill_between(np.asarray(tr.x), lo, hi, where=mask,
                                interpolate=False, color=tuple(tr.color),
                                alpha=tr.alpha, linewidth=0,
                                label=(tr.label if tr.show_in_legend else "_nolegend_"))
            elif isinstance(tr, StackedArea):
                ys = [np.asarray(y, dtype=float) for y in tr.ys]
                ax.stackplot(np.asarray(tr.x), *ys, labels=list(tr.labels),
                             colors=[tuple(c) for c in tr.colors], alpha=tr.alpha,
                             edgecolor="none")
            elif isinstance(tr, HLine):
                ax.axhline(tr.y, linestyle=tr.style, color=tuple(tr.color), linewidth=1.0)
                if tr.label:
                    ax.text(0.01, tr.y, tr.label, transform=ax.get_yaxis_transform(),
                            va="bottom", ha="left", fontsize=8, color=tuple(tr.color))
            elif isinstance(tr, VLine):
                ax.axvline(_xnum(tr.x), linestyle=tr.style, color=tuple(tr.color),
                           linewidth=tr.width,
                           label=(tr.label or "_nolegend_"))
            elif isinstance(tr, TextBox):
                trans = ax.transData if tr.data_coords else ax.transAxes
                kw = {}
                if tr.zorder is not None:
                    kw["zorder"] = tr.zorder
                ax.text(tr.x, tr.y, tr.text, transform=trans, ha=tr.ha,
                        va=tr.va, color=tuple(tr.color), fontsize=tr.fontsize,
                        fontweight="bold" if tr.bold else "normal",
                        family="monospace" if tr.mono else None,
                        bbox=(dict(facecolor="white", edgecolor=(0.7, 0.7, 0.7),
                                   pad=1.5) if tr.boxed else None), **kw)
            elif isinstance(tr, Raster):
                raster_m = self._draw_raster(tr, ax, fig, ax_cmap)
            elif isinstance(tr, RgbRaster):
                raster_m = self._draw_rgb_raster(tr, ax)
            elif isinstance(tr, Mesh):
                raster_m = self._draw_mesh(tr, ax, ax_cmap, vmin, vmax)
            elif isinstance(tr, Markers):
                ax.plot(np.asarray(tr.x), np.asarray(tr.rows, dtype=float) + 1.0,
                        tr.marker, markersize=tr.size, markerfacecolor="none",
                        markeredgecolor=tuple(tr.color), linewidth=1.2,
                        label=(tr.label or "_nolegend_"))
            elif isinstance(tr, Stem):
                # Emulated with vlines + markers so it is datetime-x safe.
                xv = np.asarray(tr.x)
                yv = np.asarray(tr.y, dtype=float)
                ax.vlines(xv, tr.baseline, yv, color=tuple(tr.color), linewidth=1.0)
                ax.plot(xv, yv, tr.marker, color=tuple(tr.color), markersize=5,
                        linestyle="none", label=(tr.label or "_nolegend_"))
                ax.axhline(tr.baseline, color=(0.6, 0.6, 0.6), linewidth=0.6)

        return raster_m, point_m, twin

    # ------------------------------------------------------------------ #
    def _draw_geo(self, spec: Axes, ax, fig: Figure, fig_cmap):
        """Equirectangular world map: coastline + graticule + ordinary traces.

        A ``geo`` axes plots lon (x) vs lat (y) in degrees.  The vendored
        Natural Earth coastline is drawn over any :class:`Mesh` field but under
        the markers (zorder ordering); ``Scatter``/``Line``/``TextBox`` traces
        from the viewer render through the shared trace loop on top.
        """
        ax_cmap = ListedColormap(spec.cmap) if spec.cmap is not None else fig_cmap
        ext = spec.geo_extent or (-180.0, 180.0, -90.0, 90.0)

        # Graticule (under everything).
        g = spec.graticule_deg
        if g and g > 0:
            import numpy as _np
            for lon in _np.arange(_np.ceil(ext[0] / g) * g, ext[1] + 1e-6, g):
                ax.axvline(lon, color=(0.86, 0.86, 0.86), linewidth=0.5, zorder=0.5)
            for lat in _np.arange(_np.ceil(ext[2] / g) * g, ext[3] + 1e-6, g):
                ax.axhline(lat, color=(0.86, 0.86, 0.86), linewidth=0.5, zorder=0.5)

        # Viewer traces (Mesh field at low zorder, then lines/markers).
        raster_m, point_m, _twin = self._draw_traces(spec, ax, fig, ax_cmap)

        # Coastline over the field, under the markers.
        if spec.coastline:
            from ..coastline import load_coastline
            clon, clat = load_coastline()
            if clon.size:
                ax.plot(clon, clat, color=(0.30, 0.30, 0.30), linewidth=0.6,
                        zorder=2.0)

        ax.set_xlim(ext[0], ext[1])
        ax.set_ylim(ext[2], ext[3])
        ax.set_aspect("equal", adjustable="box")
        if spec.title:
            ax.set_title(spec.title)
        ax.set_xlabel(spec.xlabel or "Longitude (deg)")
        ax.set_ylabel(spec.ylabel or "Latitude (deg)")
        if spec.grid:
            ax.grid(False)   # graticule already drawn; suppress the default grid
        if spec.legend is not None:
            handles, labels = ax.get_legend_handles_labels()
            if handles:
                ax.legend(handles, labels, loc=spec.legend.loc, fontsize=7,
                          framealpha=0.9)
        return raster_m, point_m

    # ------------------------------------------------------------------ #
    def _draw_mesh(self, tr: Mesh, ax, cmap_obj, vmin, vmax):
        lon = np.asarray(tr.lon, dtype=float)
        lat = np.asarray(tr.lat, dtype=float)
        grid = np.ma.masked_invalid(np.asarray(tr.grid, dtype=float))
        cmap = (cmap_obj if cmap_obj is not None else plt.get_cmap("viridis")).copy()
        cmap.set_bad(alpha=0.0)
        lon_e = _centers_to_edges(lon)
        lat_e = _centers_to_edges(lat)
        zorder = tr.zorder if tr.zorder is not None else 0.0
        return ax.pcolormesh(lon_e, lat_e, grid, cmap=cmap, vmin=vmin, vmax=vmax,
                             alpha=tr.alpha, shading="auto", zorder=zorder)

    # ------------------------------------------------------------------ #
    def _draw_polar(self, spec: Axes, ax):
        ax.set_theta_zero_location(spec.theta_zero)
        ax.set_theta_direction(spec.theta_dir)
        point_m = None
        cmap = ListedColormap(spec.cmap) if spec.cmap is not None else None
        vmin, vmax = (spec.clim if spec.clim is not None else (None, None))
        for tr in spec.traces:
            if isinstance(tr, PolarScatter):
                th = np.radians(np.asarray(tr.theta_deg, dtype=float))
                r = np.asarray(tr.r, dtype=float)
                if tr.c is not None:
                    point_m = ax.scatter(th, r, c=np.asarray(tr.c, dtype=float),
                                         s=tr.size, cmap=cmap, vmin=vmin, vmax=vmax,
                                         label=tr.label)
                else:
                    ax.scatter(th, r, s=tr.size, color=tuple(tr.color), label=tr.label)
        if spec.rlim is not None:
            ax.set_rlim(spec.rlim)
        if spec.rticks is not None:
            ax.set_rticks(list(spec.rticks))
            if spec.rtick_labels is not None:
                ax.set_yticklabels(list(spec.rtick_labels))
        if spec.title:
            ax.set_title(spec.title)
        return point_m

    # ------------------------------------------------------------------ #
    def _draw_raster(self, tr: Raster, ax, fig: Figure, cmap_obj):
        grid = np.array(tr.grid, dtype=float)
        n_rows = grid.shape[0]
        masked = np.ma.masked_invalid(grid)
        x0, x1, is_date = _extent_x(tr.x)
        cmap = (cmap_obj if cmap_obj is not None else plt.get_cmap("viridis")).copy()
        cmap.set_bad(alpha=0.0)
        vmin, vmax = (fig.clim if fig.clim is not None else (None, None))
        im = ax.imshow(masked, aspect="auto", origin="upper", cmap=cmap,
                       vmin=vmin, vmax=vmax, interpolation="nearest",
                       extent=(x0, x1, n_rows + 0.5, 0.5))
        _raster_axes(ax, n_rows, tr.row_labels, x0, x1, is_date)
        return im

    def _draw_rgb_raster(self, tr: RgbRaster, ax):
        rgb = np.array(tr.rgb, dtype=float)
        n_rows = rgb.shape[0]
        if tr.alpha is not None:
            rgba = np.concatenate([rgb, np.asarray(tr.alpha, float)[..., None]], axis=2)
        else:
            rgba = rgb
        x0, x1, is_date = _extent_x(tr.x)
        im = ax.imshow(rgba, aspect="auto", origin="upper", interpolation="nearest",
                       extent=(x0, x1, n_rows + 0.5, 0.5))
        _raster_axes(ax, n_rows, tr.row_labels, x0, x1, is_date)
        return im

    # ------------------------------------------------------------------ #
    def _decorate(self, spec: Axes, ax, twin):
        if spec.title:
            ax.set_title(spec.title)
        if spec.xlabel:
            ax.set_xlabel(spec.xlabel)
        if spec.ylabel:
            ax.set_ylabel(spec.ylabel)
        if spec.y2label and twin["ax"] is not None:
            twin["ax"].set_ylabel(spec.y2label)
        # Log scale only when the axis actually carries positive data; a
        # degenerate column (e.g. ADOP all-zero in a float PPP run) would
        # otherwise make savefig raise "no positive values" and abort the
        # whole report.  Fall back to linear in that case.
        if spec.yscale == "log" and _axis_has_positive(ax, "y"):
            ax.set_yscale("log")
        if spec.xscale == "log" and _axis_has_positive(ax, "x"):
            ax.set_xscale("log")
        if spec.xlim is not None:
            ax.set_xlim(spec.xlim)
        if spec.ylim is not None:
            ax.set_ylim(spec.ylim)
        if spec.axis_off:
            ax.axis("off")
        elif spec.grid:
            ax.grid(True, alpha=0.4)
        if spec.legend is not None:
            handles, labels = ax.get_legend_handles_labels()
            if twin["ax"] is not None:
                h2, l2 = twin["ax"].get_legend_handles_labels()
                handles += h2
                labels += l2
            if handles:
                outside = (spec.legend.loc in ("bestoutside", "best outside")
                           or len(handles) > 15)   # dense legend -> never collapse axes
                if outside:
                    ax.legend(handles, labels, loc="upper left",
                              bbox_to_anchor=(1.005, 1.0), borderaxespad=0.0,
                              fontsize=7, ncol=1 + len(handles) // 25)
                else:
                    ax.legend(handles, labels, loc=spec.legend.loc, fontsize=8)

    # ------------------------------------------------------------------ #
    def _figure_colorbar(self, mfig, maxes, mappable, fig: Figure):
        cb = mfig.colorbar(mappable, ax=list(maxes), location="right",
                           fraction=0.046, pad=0.02)
        self._style_colorbar(cb, fig.colorbar, fig.categorical)

    def _axes_colorbar(self, mfig, ax, mappable, spec: ColorBar):
        cb = mfig.colorbar(mappable, ax=ax, location="right", fraction=0.046, pad=0.02)
        self._style_colorbar(cb, spec, categorical=False)

    @staticmethod
    def _style_colorbar(cb, spec: ColorBar, categorical: bool):
        if spec.label:
            cb.set_label(spec.label)
        if spec.ticks is not None:
            cb.set_ticks(list(spec.ticks))
            if spec.tick_labels is not None:
                cb.set_ticklabels(list(spec.tick_labels))
        elif categorical and spec.tick_labels is not None:
            cb.set_ticks(list(range(1, len(spec.tick_labels) + 1)))
            cb.set_ticklabels(list(spec.tick_labels))

    # ------------------------------------------------------------------ #
    def to_png_bytes(self, rendered, dpi: int = 110, tight: bool = True) -> bytes:
        buf = io.BytesIO()
        # Animation frames render with tight=False so every frame shares pixel
        # dimensions (a per-frame "tight" bbox would jitter the GIF canvas).
        if tight:
            rendered.savefig(buf, format="png", dpi=dpi, bbox_inches="tight")
        else:
            rendered.savefig(buf, format="png", dpi=dpi)
        return buf.getvalue()

    def save(self, rendered, path: str, dpi: int = 110) -> None:
        rendered.savefig(path, dpi=dpi, bbox_inches="tight")

    def show(self, rendered) -> None:  # pragma: no cover - interactive
        plt.show()

    def close(self, rendered) -> None:
        plt.close(rendered)


# --------------------------------------------------------------------------- #
def _axis_has_positive(ax, which: str) -> bool:
    """True if the drawn artists put any finite positive value on ``which`` axis.

    Used to gate ``set_*scale("log")``: matplotlib defers the "no positive
    values" error to draw/savefig time, so we must decide *before* enabling the
    log scale.  On any inspection error we return True (leave the spec honoured)
    so this guard can only ever prevent a crash, never introduce one.
    """
    try:
        idx = 1 if which == "y" else 0
        vals = []
        for ln in ax.lines:
            d = ln.get_ydata() if which == "y" else ln.get_xdata()
            vals.append(np.asarray(d, dtype=float).ravel())
        for col in ax.collections:
            off = np.asarray(col.get_offsets(), dtype=float)
            if off.ndim == 2 and off.shape[1] == 2:
                vals.append(off[:, idx])
        if not vals:
            return True
        allv = np.concatenate(vals)
        allv = allv[np.isfinite(allv)]
        return bool(allv.size) and bool(allv.max() > 0.0)
    except Exception:
        return True


def _centers_to_edges(c):
    """Cell-centre coordinates -> (n+1) cell edges for ``pcolormesh``.

    Interior edges are midpoints; the two ends are extrapolated by half a cell
    so the painted grid spans symmetrically around the outer centres.
    """
    c = np.asarray(c, dtype=float)
    if c.size == 0:
        return c
    if c.size == 1:
        return np.array([c[0] - 0.5, c[0] + 0.5])
    mid = 0.5 * (c[:-1] + c[1:])
    first = c[0] - (mid[0] - c[0])
    last = c[-1] + (c[-1] - mid[-1])
    return np.concatenate([[first], mid, [last]])


def _extent_x(x):
    """Return (x0, x1, is_date) for an imshow extent over (possibly datetime) x.

    Guards against a degenerate axis: an all-NaT/empty raster column would make
    ``date2num`` yield NaN, and matplotlib >= 3.10 raises "Axis limits cannot be
    NaN or Inf" at draw time.  Fall back to ``(0, 1)`` when no finite coordinate
    is present (the raster still draws; only its time extent is nominal) so a
    single bad trace never aborts the whole report.
    """
    x = np.asarray(x)
    if np.issubdtype(x.dtype, np.datetime64) or _is_datetime(x):
        xnum = mdates.date2num(np.asarray(x, dtype="datetime64[ns]"))
        is_date = True
    else:
        xnum = np.asarray(x, dtype=float)
        is_date = False
    xnum = np.asarray(xnum, dtype=float)
    finite = xnum[np.isfinite(xnum)]
    if finite.size == 0:
        return 0.0, 1.0, is_date
    x0, x1 = float(finite.min()), float(finite.max())
    if not (x1 > x0):
        x1 = x0 + 1.0
    return x0, x1, is_date


def _raster_axes(ax, n_rows, row_labels, x0, x1, is_date):
    ax.set_yticks(np.arange(1, n_rows + 1))
    ax.set_yticklabels(list(row_labels))
    ax.set_ylim(n_rows + 0.5, 0.5)
    if is_date:
        ax.xaxis_date()
        ax.set_xlim(x0, x1)
    ax.grid(True, alpha=0.3)


def _xnum(x):
    """Convert a possibly-datetime scalar to the axis numeric for axvline."""
    try:
        import pandas as pd
        if isinstance(x, (pd.Timestamp,)) or np.issubdtype(np.asarray(x).dtype,
                                                            np.datetime64):
            return mdates.date2num(pd.Timestamp(x).to_pydatetime())
    except Exception:
        pass
    return x


def _is_datetime(x) -> bool:
    try:
        import pandas as pd
        return pd.api.types.is_datetime64_any_dtype(pd.Series(x))
    except Exception:
        return False
