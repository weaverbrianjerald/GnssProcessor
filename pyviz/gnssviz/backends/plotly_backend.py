"""Plotly implementation of the rendering :class:`Backend`.

Interprets the same :class:`gnssviz.figures.Figure` specs as the matplotlib
backend into interactive plotly figures, which is what makes the suite usable
for browser dashboards and the real-time view.  Viewers are unchanged: they emit
specs; this module is the only one that imports plotly.

Report integration: :meth:`to_html` returns a plotly ``<div>`` fragment (the
plotly.js runtime is included once per document by the report when
``needs_js`` is set), so an HTML report can mix or switch backends.

Coverage note: time-series, scatter, bands, stacked area, ref lines,
annotations, scalar rasters (heatmaps), polar sky plots, stems and event markers
are fully supported.  Truecolor :class:`RgbRaster` is rendered best-effort via
``go.Image`` (epoch-index x-axis), since plotly images do not bind to a datetime
axis.
"""

from __future__ import annotations

import math
from typing import List, Optional

import numpy as np

from ..figures import (Axes, Band, Figure, HLine, Line, Markers, PolarScatter,
                       Polygon, Raster, RgbImage, RgbRaster, Scatter, StackedArea,
                       Stem, TextBox, VLine)
from .base import Backend


class PlotlyBackend(Backend):
    name = "plotly"
    needs_js = True

    # ------------------------------------------------------------------ #
    def render(self, fig: Figure):
        from plotly.subplots import make_subplots

        specs = fig.axes or [Axes()]
        n = len(specs)
        row_specs, row_secy = [], []
        for spec in specs:
            if spec.projection == "polar":
                row_specs.append([{"type": "polar"}])
                row_secy.append(False)
            else:
                secy = any(getattr(t, "axis", "left") == "right" for t in spec.traces)
                row_specs.append([{"secondary_y": secy}])
                row_secy.append(secy)

        # Plotly caps vertical_spacing at 1/(rows-1); scale down for tall stacks.
        vspace = min(0.08, 0.8 / max(n, 2))
        gofig = make_subplots(
            rows=n, cols=1, shared_xaxes=fig.link_x, specs=row_specs,
            subplot_titles=[s.title for s in specs], vertical_spacing=vspace,
        )

        polar_n = 0
        for i, spec in enumerate(specs):
            if spec.projection == "polar":
                polar_n += 1
                self._draw_polar(gofig, spec, "polar" + ("" if polar_n == 1 else str(polar_n)))
            else:
                self._draw_rect(gofig, spec, fig, i + 1, row_secy[i])

        title = fig.title or None
        show_any = any(getattr(s, "legend", None) is not None for s in specs)
        # Overlay the legend inside the plot area (top subplot) so it does not reserve a
        # right-hand gutter of blank whitespace on every figure.  Position follows the
        # first axis's matplotlib-style loc; the semi-transparent box lets data show
        # through.  x/y are paper coords (whole plotting area); yanchor near the top
        # keeps it in the first row of a stacked figure.
        leg_spec = next((s.legend for s in specs if getattr(s, "legend", None) is not None), None)
        legend = dict(orientation="v", bgcolor="rgba(255,255,255,0.62)",
                      bordercolor="rgba(0,0,0,0.22)", borderwidth=1,
                      **_legend_pos(getattr(leg_spec, "loc", "best")))
        gofig.update_layout(template="plotly_white", height=int(fig.height * 96),
                            title=title, margin=dict(l=70, r=40, t=60, b=50),
                            showlegend=show_any, legend=legend)
        # Axes facecolor (e.g. ocean behind land polygons) -> figure plot bg.
        fc = next((s.facecolor for s in specs
                   if getattr(s, "facecolor", None) is not None), None)
        if fc is not None:
            gofig.update_layout(plot_bgcolor=_rgb(fc))
        return gofig

    # ------------------------------------------------------------------ #
    def _draw_rect(self, gofig, spec: Axes, fig: Figure, row: int, secy: bool):
        import plotly.graph_objects as go

        leg = spec.legend is not None      # legends only on axes that asked for one
        for tr in spec.traces:
            if isinstance(tr, Line):
                gofig.add_trace(go.Scatter(
                    x=_np(tr.x), y=_f(tr.y), mode="lines", name=tr.label,
                    line=dict(color=_rgb(tr.color), width=tr.width, dash=_dash(tr.style)),
                    showlegend=leg and tr.show_in_legend and tr.label is not None),
                    row=row, col=1, secondary_y=(tr.axis == "right"))
            elif isinstance(tr, Scatter):
                marker = dict(size=max(3, tr.size ** 0.5 * 2),
                              symbol=_symbol(tr.marker), opacity=tr.alpha)
                if tr.c is not None:
                    # Per-point scalar through the axes cmap/clim (mirror of the
                    # PolarScatter branch; the mpl backend maps c the same way).
                    marker.update(color=_f(tr.c), colorscale=_colorscale(spec.cmap)
                                  if spec.cmap is not None else "Viridis",
                                  showscale=spec.colorbar is not None,
                                  colorbar=(dict(title=spec.colorbar.label)
                                            if spec.colorbar else None))
                    if spec.clim is not None:
                        marker.update(cmin=spec.clim[0], cmax=spec.clim[1])
                else:
                    marker.update(color=_rgb(tr.color))
                gofig.add_trace(go.Scatter(
                    x=_np(tr.x), y=_f(tr.y), mode="markers", name=tr.label,
                    marker=marker,
                    showlegend=leg and tr.show_in_legend and tr.label is not None),
                    row=row, col=1, secondary_y=(tr.axis == "right"))
            elif isinstance(tr, Band):
                x = _np(tr.x)
                gofig.add_trace(go.Scatter(x=x, y=_nan_to_none(tr.hi), mode="lines",
                                           line=dict(width=0), showlegend=False,
                                           hoverinfo="skip"), row=row, col=1)
                gofig.add_trace(go.Scatter(x=x, y=_nan_to_none(tr.lo), mode="lines",
                                           line=dict(width=0), fill="tonexty",
                                           fillcolor=_rgba(tr.color, tr.alpha),
                                           name=tr.label,
                                           showlegend=leg and tr.show_in_legend,
                                           hoverinfo="skip"), row=row, col=1)
            elif isinstance(tr, StackedArea):
                for y, lbl, col in zip(tr.ys, tr.labels, tr.colors):
                    gofig.add_trace(go.Scatter(
                        x=_np(tr.x), y=_f(y), mode="lines", name=lbl, showlegend=leg,
                        stackgroup="one", line=dict(width=0.5, color=_rgb(col)),
                        fillcolor=_rgba(col, tr.alpha)), row=row, col=1)
            elif isinstance(tr, HLine):
                hkw = dict(y=tr.y, line=dict(color=_rgb(tr.color), dash=_dash(tr.style)),
                           row=row, col=1)
                if tr.label:                 # only annotate real labels: a None/"" text
                    hkw["annotation_text"] = tr.label   # renders as plotly's "new text"
                gofig.add_hline(**hkw)
            elif isinstance(tr, VLine):
                vkw = dict(x=_xscalar(tr.x), line=dict(color=_rgb(tr.color),
                           dash=_dash(tr.style), width=tr.width), row=row, col=1)
                if tr.label:                 # (repeated slip/storm VLines otherwise
                    vkw["annotation_text"] = tr.label   # spam "new text" across the top)
                gofig.add_vline(**vkw)
            elif isinstance(tr, TextBox):
                yref, xref = _domain_refs(gofig, row)
                gofig.add_annotation(x=tr.x, y=tr.y, xref=xref, yref=yref, text=tr.text,
                                     showarrow=False, xanchor=tr.ha, yanchor=_va(tr.va),
                                     font=dict(color=_rgb(tr.color),
                                               family="monospace" if tr.mono else None),
                                     bgcolor="white" if tr.boxed else None)
            elif isinstance(tr, Raster):
                self._heatmap(gofig, tr, fig, row)
            elif isinstance(tr, RgbRaster):
                self._image(gofig, tr, row)
            elif isinstance(tr, RgbImage):
                self._layout_image(gofig, tr, row)
            elif isinstance(tr, Markers):
                gofig.add_trace(go.Scatter(
                    x=_np(tr.x), y=np.asarray(tr.rows, float) + 1.0, mode="markers",
                    marker=dict(color=_rgb(tr.color), size=tr.size,
                                symbol=_symbol(tr.marker), line=dict(width=1.2)),
                    name=tr.label, showlegend=leg and tr.label is not None), row=row, col=1)
            elif isinstance(tr, Stem):
                sx, sy = _stem_segments(tr.x, tr.y, tr.baseline)
                gofig.add_trace(go.Scatter(x=sx, y=sy, mode="lines",
                                           line=dict(color=_rgb(tr.color), width=1),
                                           showlegend=False, hoverinfo="skip"),
                                row=row, col=1)
                gofig.add_trace(go.Scatter(x=_np(tr.x), y=_f(tr.y), mode="markers",
                                           marker=dict(color=_rgb(tr.color), size=6),
                                           name=tr.label,
                                           showlegend=leg and tr.label is not None),
                                row=row, col=1)
            elif isinstance(tr, Polygon):
                self._polygon(gofig, tr, row)

        self._decorate_rect(gofig, spec, row, secy)

    # ------------------------------------------------------------------ #
    def _polygon(self, gofig, tr: Polygon, row: int):
        """Fill each NaN-separated ring (land) with a self-closing scatter."""
        import plotly.graph_objects as go
        x = np.asarray(tr.x, dtype=float)
        y = np.asarray(tr.y, dtype=float)
        line = (dict(color=_rgb(tr.edge), width=tr.width) if tr.edge is not None
                else dict(width=0))
        fillcolor = _rgba(tr.fill, tr.alpha)
        breaks = np.flatnonzero(~(np.isfinite(x) & np.isfinite(y)))
        start = 0
        for stop in list(breaks) + [x.size]:
            rx, ry = x[start:stop], y[start:stop]
            m = np.isfinite(rx) & np.isfinite(ry)
            if int(m.sum()) >= 3:
                gofig.add_trace(go.Scatter(x=rx[m], y=ry[m], mode="lines",
                                           fill="toself", fillcolor=fillcolor,
                                           line=line, showlegend=False,
                                           hoverinfo="skip"), row=row, col=1)
            start = stop + 1

    # ------------------------------------------------------------------ #
    def _heatmap(self, gofig, tr: Raster, fig: Figure, row: int):
        import plotly.graph_objects as go
        grid = np.array(tr.grid, dtype=float)
        n_rows = grid.shape[0]
        ys = np.arange(1, n_rows + 1)
        cmap = fig.cmap
        if fig.categorical and cmap is not None:
            colorscale = _discrete_colorscale(cmap)
        elif cmap is not None:
            colorscale = _colorscale(cmap)
        else:
            colorscale = "Viridis"
        zmin, zmax = (fig.clim if fig.clim is not None else (None, None))
        cb = None
        if fig.colorbar is not None:
            cb = dict(title=fig.colorbar.label)
            if fig.colorbar.ticks is not None:
                cb["tickvals"] = list(fig.colorbar.ticks)
            if fig.colorbar.tick_labels is not None:
                cb["ticktext"] = list(fig.colorbar.tick_labels)
        # Uniform column index + datetime tick overlay -> matches MATLAB imagesc
        # (columns spread evenly across the span regardless of real epoch gaps).
        n_cols = grid.shape[1]
        xidx = np.arange(n_cols)
        gofig.add_trace(go.Heatmap(
            z=grid, x=xidx, y=ys, colorscale=colorscale, zmin=zmin, zmax=zmax,
            hoverongaps=False, colorbar=cb, showscale=fig.colorbar is not None),
            row=row, col=1)
        tv, tt = _time_ticks(tr.x, n_cols)
        gofig.update_xaxes(tickmode="array", tickvals=tv, ticktext=tt, row=row, col=1)
        gofig.update_yaxes(autorange="reversed", tickmode="array", tickvals=list(ys),
                           ticktext=list(tr.row_labels), row=row, col=1)

    def _layout_image(self, gofig, tr: RgbImage, row: int):
        """A georeferenced basemap as a data-anchored, below-traces layout image.

        Embedded as a base64 ``data:`` URI (the vendored bytes verbatim when
        present, else a stdlib PNG re-encode) so the interactive report stays
        fully offline.  ``layer='below'`` keeps it under the track/markers.
        """
        import base64
        if tr.encoded is not None:
            raw, mime = bytes(tr.encoded), tr.mime
        else:
            from ..pngio import encode_png
            raw, mime = encode_png(tr.rgb), "image/png"
        uri = "data:%s;base64,%s" % (mime, base64.b64encode(raw).decode("ascii"))
        x0, x1, y0, y1 = (float(v) for v in tr.extent)
        suffix = "" if row == 1 else str(row)
        gofig.add_layout_image(dict(
            source=uri, xref="x" + suffix, yref="y" + suffix,
            x=min(x0, x1), y=max(y0, y1),           # anchor at the top-left corner
            sizex=abs(x1 - x0), sizey=abs(y1 - y0),
            xanchor="left", yanchor="top", sizing="stretch",
            layer="below", opacity=tr.alpha))

    def _image(self, gofig, tr: RgbRaster, row: int):
        """Truecolor raster as a STRETCH-to-fill layout image (not go.Image).

        ``go.Image`` locks square pixels, so a wide-but-short strip (many epochs,
        few rows) collapses into an unreadable sliver.  A layout image with
        ``sizing='stretch'`` fills the subplot at any aspect, and being anchored in
        data coords it also carries real GPS-time x-ticks (``go.Image`` could only
        show epoch indices).  Row 0 sits at the top via a reversed y-axis.
        """
        import base64

        from ..pngio import encode_png
        rgb = np.clip(np.array(tr.rgb, float), 0.0, 1.0)
        z = (rgb * 255).astype(np.uint8)
        n_rows, n_cols = z.shape[0], z.shape[1]
        if tr.alpha is not None:
            a = (np.clip(np.asarray(tr.alpha, float), 0.0, 1.0) * 255).astype(np.uint8)
            z = np.concatenate([z, a[..., None]], axis=2)   # RGBA -> empty cells clear
        uri = "data:image/png;base64," + base64.b64encode(encode_png(z)).decode("ascii")
        suffix = "" if row == 1 else str(row)
        # Image spans x in [0, n_cols], y in [0, n_rows]; row 0 at the top edge.
        gofig.add_layout_image(dict(
            source=uri, xref="x" + suffix, yref="y" + suffix,
            x=0.0, y=0.0, sizex=n_cols, sizey=n_rows,
            xanchor="left", yanchor="top", sizing="stretch", layer="below"))
        tv, tt = _time_ticks(tr.x, n_cols)
        gofig.update_xaxes(range=[0, n_cols], tickmode="array", tickvals=tv,
                           ticktext=tt, row=row, col=1)
        gofig.update_yaxes(range=[n_rows, 0], tickmode="array",   # reversed: row 0 on top
                           tickvals=[i + 0.5 for i in range(n_rows)],
                           ticktext=list(tr.row_labels), row=row, col=1)

    # ------------------------------------------------------------------ #
    def _draw_polar(self, gofig, spec: Axes, polar_name: str):
        import plotly.graph_objects as go
        for tr in spec.traces:
            if isinstance(tr, PolarScatter):
                marker = dict(size=max(4, tr.size ** 0.5 * 2))
                if tr.c is not None:
                    marker.update(color=_f(tr.c), colorscale=_colorscale(spec.cmap)
                                  if spec.cmap is not None else "Viridis",
                                  showscale=spec.colorbar is not None,
                                  colorbar=(dict(title=spec.colorbar.label)
                                            if spec.colorbar else None))
                    if spec.clim is not None:
                        marker.update(cmin=spec.clim[0], cmax=spec.clim[1])
                else:
                    marker.update(color=_rgb(tr.color))
                gofig.add_trace(go.Scatterpolar(
                    theta=_f(tr.theta_deg), r=_f(tr.r), mode="markers",
                    marker=marker, name=tr.label, subplot=polar_name))
        direction = "clockwise" if spec.theta_dir == -1 else "counterclockwise"
        rotation = {"N": 90, "E": 0, "S": 270, "W": 180}.get(spec.theta_zero, 90)
        polar = dict(angularaxis=dict(direction=direction, rotation=rotation))
        if spec.rlim is not None:
            polar["radialaxis"] = dict(range=list(spec.rlim))
        gofig.update_layout(**{polar_name: polar})

    # ------------------------------------------------------------------ #
    def _decorate_rect(self, gofig, spec: Axes, row: int, secy: bool):
        if spec.xlabel:
            gofig.update_xaxes(title_text=spec.xlabel, row=row, col=1)
        if spec.ylabel:
            gofig.update_yaxes(title_text=spec.ylabel, row=row, col=1, secondary_y=False)
        if spec.y2label and secy:
            gofig.update_yaxes(title_text=spec.y2label, row=row, col=1, secondary_y=True)
        if spec.yscale == "log":            # log on the primary axis (the common case)
            gofig.update_yaxes(type="log", row=row, col=1, secondary_y=False)
        if spec.xscale == "log":
            gofig.update_xaxes(type="log", row=row, col=1)
        # Plotly expresses a log axis's `range` in exponents, matplotlib in data units.
        # Viewers speak data units (that is the contract Axes.xlim/ylim documents), so a
        # log axis has to be converted here or a perfectly sane limit like 35 m renders as
        # 10^35.  Non-positive bounds are dropped rather than passed to log10.
        def _rng(lim, is_log):
            if not is_log:
                return list(lim)
            out = []
            for v in lim:
                if v is None or v <= 0:
                    return None                  # cannot express on a log axis; autorange
                out.append(math.log10(v))
            return out

        if spec.xlim is not None:
            r = _rng(spec.xlim, spec.xscale == "log")
            if r is not None:
                gofig.update_xaxes(range=r, row=row, col=1)
        if spec.ylim is not None:
            lo, hi = spec.ylim
            if lo is not None and hi is not None:
                r = _rng((lo, hi), spec.yscale == "log")
                if r is not None:
                    gofig.update_yaxes(range=r, row=row, col=1)
            elif lo is not None:            # half-open (e.g. (0, None)) -> floor only
                gofig.update_yaxes(autorangeoptions=dict(minallowed=lo), row=row, col=1)
            elif hi is not None:
                gofig.update_yaxes(autorangeoptions=dict(maxallowed=hi), row=row, col=1)
        if spec.aspect_equal:
            # Lock the primary y-axis to the same data scale as x (1:1), so a
            # plan-view track is not distorted by the panel's aspect ratio.
            xref = "x" if row == 1 else f"x{row}"
            gofig.update_yaxes(scaleanchor=xref, scaleratio=1.0, row=row, col=1,
                               secondary_y=False)
        if spec.axis_off:
            gofig.update_xaxes(visible=False, row=row, col=1)
            gofig.update_yaxes(visible=False, row=row, col=1)

    # ------------------------------------------------------------------ #
    def to_html(self, rendered, dpi: int = 110) -> str:
        return ('<div class="fig">'
                + rendered.to_html(full_html=False, include_plotlyjs=False,
                                   default_height=f"{rendered.layout.height or 420}px")
                + "</div>")

    def live_payload(self, rendered, dpi: int = 110) -> dict:
        # Round-trip through plotly's JSON encoder (handles numpy etc.) into a
        # plain, JSON-serialisable dict the live client feeds to Plotly.react.
        import json
        import plotly.io as pio
        fig = json.loads(pio.to_json(rendered))
        return {"kind": "plotly", "data": fig.get("data", []),
                "layout": fig.get("layout", {})}

    def to_png_bytes(self, rendered, dpi: int = 110) -> bytes:
        try:
            return rendered.to_image(format="png", scale=dpi / 72.0)
        except Exception as exc:  # pragma: no cover - needs kaleido
            raise RuntimeError("plotly PNG export needs the 'kaleido' package; "
                               "use to_html for interactive output") from exc

    def save(self, rendered, path: str, dpi: int = 110) -> None:
        if str(path).lower().endswith(".html"):
            rendered.write_html(path, include_plotlyjs="cdn")
        else:
            rendered.write_image(path, scale=dpi / 72.0)

    def close(self, rendered) -> None:
        pass


# --------------------------------------------------------------------------- #
def _np(x):
    """x for a plotly trace.  datetime64 arrays get their ``NaT`` gap-markers
    turned into ``None`` -- otherwise plotly's JSON encoder collapses ``NaT`` to
    ``Timestamp.min`` (1677-09-21), which blows the auto x-range out to three
    centuries and crushes all the real data into the right edge.  ``None`` both
    breaks the line at the gap (like matplotlib) and is ignored for ranging."""
    a = np.asarray(x)
    if a.dtype.kind == "M":  # datetime64
        return [None if np.isnat(v) else v for v in a]
    return a


def _f(y):
    return np.asarray(y, dtype=float)


def _legend_pos(loc: str) -> dict:
    """Map a matplotlib-style legend ``loc`` to an INSIDE-the-axes plotly position.

    Everything is placed inside the plotting area (including ``best`` / ``bestoutside``)
    so no figure reserves an outer legend gutter.  Corners are inset slightly so the box
    does not touch the axis lines.
    """
    loc = (loc or "best").replace("best outside", "best").replace("bestoutside", "best")
    table = {
        "upper right": (0.99, 0.99, "right", "top"),
        "best":        (0.99, 0.99, "right", "top"),
        "upper left":  (0.01, 0.99, "left", "top"),
        "lower right": (0.99, 0.02, "right", "bottom"),
        "lower left":  (0.01, 0.02, "left", "bottom"),
        "center right": (0.99, 0.5, "right", "middle"),
        "center left": (0.01, 0.5, "left", "middle"),
        "upper center": (0.5, 0.99, "center", "top"),
        "lower center": (0.5, 0.02, "center", "bottom"),
    }
    x, y, xa, ya = table.get(loc, table["best"])
    return dict(x=x, y=y, xanchor=xa, yanchor=ya)


def _rgb(c) -> str:
    r, g, b = (int(round(v * 255)) for v in c)
    return f"rgb({r},{g},{b})"


def _rgba(c, a) -> str:
    r, g, b = (int(round(v * 255)) for v in c)
    return f"rgba({r},{g},{b},{a})"


def _dash(style: str) -> str:
    return {"-": "solid", "--": "dash", ":": "dot", "-.": "dashdot"}.get(style, "solid")


def _symbol(m: str) -> str:
    return {"o": "circle", "s": "square", "^": "triangle-up", "v": "triangle-down",
            "x": "x", "+": "cross", "*": "star", "d": "diamond"}.get(m, "circle")


def _va(va: str) -> str:
    return {"top": "top", "bottom": "bottom", "center": "middle"}.get(va, "top")


def _nan_to_none(v):
    return [None if not np.isfinite(x) else float(x) for x in np.asarray(v, float)]


def _xscalar(x):
    try:
        import pandas as pd
        if isinstance(x, pd.Timestamp) or np.issubdtype(np.asarray(x).dtype, np.datetime64):
            return pd.Timestamp(x).to_pydatetime()
    except Exception:
        pass
    return x


def _time_ticks(x, n_cols, n=8):
    """~n evenly-spaced column indices with HH:MM:SS labels from the epoch x."""
    if n_cols == 0:
        return [], []
    idx = np.unique(np.linspace(0, n_cols - 1, min(n, n_cols)).round().astype(int))
    xa = np.asarray(x)
    labels = []
    for i in idx:
        try:
            import pandas as pd
            labels.append(pd.Timestamp(xa[i]).strftime("%H:%M:%S"))
        except Exception:
            labels.append(str(xa[i]))
    return list(idx), labels


def _stem_segments(x, y, base):
    """Build None-separated line segments from baseline to each (x, y)."""
    xs, ys = [], []
    xa = np.asarray(x)
    ya = np.asarray(y, float)
    for i in range(len(xa)):
        xs.extend([xa[i], xa[i], None])
        ys.extend([base, ya[i], None])
    return xs, ys


def _domain_refs(gofig, row: int):
    """Axis-domain refs ('x domain' / 'y domain') for the given subplot row."""
    # Row r maps to xaxis{r}/yaxis{r} in a single-column subplot grid.
    suffix = "" if row == 1 else str(row)
    return f"y{suffix} domain", f"x{suffix} domain"


def _colorscale(cmap: np.ndarray) -> list:
    cmap = np.asarray(cmap, float)
    n = cmap.shape[0]
    return [[i / (n - 1), _rgb(cmap[i])] for i in range(n)]


def _discrete_colorscale(cmap: np.ndarray) -> list:
    """Stepped colorscale (constant colour per band) for categorical rasters."""
    cmap = np.asarray(cmap, float)
    k = cmap.shape[0]
    scale = []
    for i in range(k):
        c = _rgb(cmap[i])
        scale.append([i / k, c])
        scale.append([(i + 1) / k, c])
    return scale
