"""Backend-agnostic figure description (a small declarative "scene graph").

This is the contract between the *viewers* (which know GNSS semantics and build
a :class:`Figure` out of prepared data) and the *backends* (which know a plotting
library and render a :class:`Figure` to pixels).  Nothing in this module imports
matplotlib / plotly: a :class:`Figure` is pure data, fully picklable, trivially
unit-testable, and identical whether it is later drawn by the matplotlib backend
(static report PNGs) or the plotly backend (interactive).

The vocabulary intentionally tracks what the MATLAB viewer suite actually draws
(``plot`` / ``area`` / ``yline`` / ``imagesc`` / ``text`` / dual y-axis / log y /
colorbars / categorical legends) rather than trying to mirror a whole plotting
API.  Add trace kinds here only when a real viewer needs one.

Coordinate conventions
----------------------
* ``x`` values are either python/pandas ``datetime`` objects (the common case --
  GPS epochs) or plain numbers.  Set :attr:`Axes.x_is_datetime` accordingly.
* :class:`TextBox` uses *axes-fraction* coordinates (0..1), like MATLAB's
  ``'Units','normalized'`` text.
* :class:`Raster` mirrors MATLAB ``imagesc``: ``grid`` is ``(n_rows, n_cols)``
  with row 0 drawn at the top (``YDir='reverse'``), columns spread *uniformly*
  across the time span ``[x[0], x[-1]]`` regardless of real epoch gaps, and
  ``NaN`` cells left transparent.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional, Sequence, Union

import numpy as np

RGB = Sequence[float]  # 3-tuple in 0..1


# --------------------------------------------------------------------------- #
#  Trace kinds (things drawn inside one Axes)
# --------------------------------------------------------------------------- #
@dataclass
class Line:
    """A line series; ``style`` mirrors MATLAB line specs ('-', '--', ':')."""
    x: Sequence
    y: Sequence
    label: Optional[str] = None
    color: RGB = (0.0, 0.0, 0.0)
    width: float = 1.2
    style: str = "-"
    axis: str = "left"            # 'left' | 'right' (secondary y-axis, yyaxis)
    show_in_legend: bool = True


@dataclass
class StackedArea:
    """A stacked area (MATLAB ``area``); ``ys`` are bottom->top bands."""
    x: Sequence
    ys: Sequence[Sequence]        # list of equal-length series
    labels: Sequence[str]
    colors: Sequence[RGB]
    alpha: float = 0.85


@dataclass
class HLine:
    """A horizontal reference line (MATLAB ``yline``)."""
    y: float
    label: Optional[str] = None
    color: RGB = (0.6, 0.6, 0.6)
    style: str = "--"


@dataclass
class TextBox:
    """Text anchored in axes-fraction coords (MATLAB ``'Units','normalized'``).

    Set ``data_coords=True`` to place the text at *data* coordinates instead
    (``x``/``y`` are then data values, e.g. lon/lat on a geo axes); used for
    on-map station labels.
    """
    x: float
    y: float
    text: str
    color: RGB = (0.0, 0.0, 0.0)
    ha: str = "left"              # horizontal alignment
    va: str = "top"              # vertical alignment
    mono: bool = False           # monospace font (stat read-outs)
    bold: bool = False           # bold weight
    boxed: bool = False          # white background w/ light edge
    data_coords: bool = False     # x/y are data coords (not axes-fraction)
    fontsize: float = 9.0
    zorder: Optional[float] = None


@dataclass
class Raster:
    """An ``imagesc``-style (row x column) grid painted by the figure colormap.

    ``grid`` is ``(n_rows, n_cols)``; ``NaN`` -> transparent.  ``row_labels``
    label the rows top->bottom.  ``x`` gives the per-column positions used only
    to set the time extent (columns are spread uniformly, like ``imagesc``).
    """
    x: Sequence
    row_labels: Sequence[str]
    grid: np.ndarray


@dataclass
class Markers:
    """Event glyphs overlaid on a raster, addressed by (x, row-index)."""
    x: Sequence
    rows: Sequence[int]           # 0-based row indices into the raster
    marker: str = "o"
    color: RGB = (0.0, 0.0, 0.0)
    size: float = 6.0
    label: Optional[str] = None


@dataclass
class Scatter:
    """A scatter series (MATLAB ``scatter`` / ``plot(...,'o')``).

    When ``c`` is given, points are coloured by that per-point scalar through the
    axes ``cmap`` / ``clim`` (the mappable is surfaced for a colorbar) instead of
    the flat ``color``; the geo viewers use this to colour station/pierce-point
    markers by a value (e.g. slant iono).  ``edgecolor`` / ``linewidth`` draw a
    marker outline (e.g. a white ring around a coloured station dot).
    """
    x: Sequence
    y: Sequence
    label: Optional[str] = None
    color: RGB = (0.0, 0.0, 0.0)
    size: float = 12.0
    marker: str = "o"
    alpha: float = 1.0
    axis: str = "left"
    show_in_legend: bool = True
    c: Optional[Sequence] = None
    edgecolor: Optional[RGB] = None
    linewidth: float = 0.0
    zorder: Optional[float] = None


@dataclass
class Band:
    """A shaded ±envelope between ``lo`` and ``hi`` (MATLAB ``fill`` sigma band).

    ``NaN`` in ``lo``/``hi`` breaks the band into segments (gaps stay open).
    """
    x: Sequence
    lo: Sequence
    hi: Sequence
    color: RGB = (0.5, 0.5, 0.5)
    alpha: float = 0.2
    label: Optional[str] = None
    show_in_legend: bool = False


@dataclass
class VLine:
    """A vertical reference/event line (MATLAB ``xline``)."""
    x: object                     # datetime or numeric
    color: RGB = (0.5, 0.5, 0.5)
    style: str = "--"
    width: float = 1.0
    label: Optional[str] = None


@dataclass
class RgbRaster:
    """A truecolor (row x col x 3) raster with a transparency mask.

    Used where cells encode a colour directly rather than a scalar mapped
    through a colormap (e.g. per-(sat,signal) band-usage in
    ``plotSatelliteSignals``).  ``alpha`` (row x col, 0..1) leaves untracked
    cells transparent; columns spread uniformly across the time span.
    """
    x: Sequence
    row_labels: Sequence[str]
    rgb: np.ndarray               # (n_rows, n_cols, 3) in 0..1
    alpha: Optional[np.ndarray] = None  # (n_rows, n_cols) in 0..1


@dataclass
class Stem:
    """A stem plot (MATLAB ``stem(...,'filled')``); good for 0/1 event flags."""
    x: Sequence
    y: Sequence
    color: RGB = (0.0, 0.0, 0.0)
    marker: str = "o"
    baseline: float = 0.0
    label: Optional[str] = None


@dataclass
class PolarScatter:
    """A polar scatter (sky plot): azimuth (deg, 0=N, CW) x radius.

    Radius is typically ``90 - elevation`` so zenith is at the centre.  ``c`` (if
    given) colours points through the axes/figure colormap + colour limits.
    """
    theta_deg: Sequence
    r: Sequence
    c: Optional[Sequence] = None
    color: RGB = (0.1, 0.3, 0.7)
    size: float = 18.0
    label: Optional[str] = None


@dataclass
class Polygon:
    """A filled closed path (e.g. land polygons for a basemap).

    ``NaN`` separators in ``x``/``y`` split multiple rings so one Polygon can
    carry a whole continent-and-islands set; each ring is auto-closed and filled.
    Used with an :attr:`Axes.facecolor` (ocean) behind it for a land/ocean map.
    """
    x: Sequence
    y: Sequence
    fill: RGB = (0.92, 0.90, 0.83)   # land tan
    edge: Optional[RGB] = None        # outline colour (None => no edge)
    alpha: float = 1.0
    width: float = 0.5
    zorder: Optional[float] = None
    label: Optional[str] = None
    show_in_legend: bool = False


@dataclass
class RgbImage:
    """A georeferenced truecolor image pinned to a data-coordinate extent.

    Unlike :class:`RgbRaster` (``imagesc``-style: columns spread *uniformly*
    across a time span, row 0 at the top, per-row labels), an ``RgbImage`` places
    the pixels at an explicit ``extent = (x0, x1, y0, y1)`` in data coordinates,
    e.g. a satellite basemap behind a lon/lat track.  Row 0 of ``rgb`` is the top
    (max-y) row.  Drawn *below* other traces via a low :attr:`zorder` (a basemap).

    ``encoded`` optionally carries the same pixels already compressed (e.g. the
    vendored JPEG) so a backend can embed them verbatim (small offline HTML)
    instead of re-encoding; ``rgb`` stays the canonical array both backends and
    tests use.
    """
    rgb: np.ndarray                       # (H, W, 3) uint8 or float 0..1
    extent: Sequence[float]               # (x0, x1, y0, y1) in data coords
    alpha: float = 1.0
    zorder: float = -1.0
    interpolation: str = "bilinear"       # matplotlib imshow interpolation
    encoded: Optional[bytes] = None       # pre-compressed bytes of the same pixels
    mime: str = "image/png"               # mime of ``encoded`` (e.g. image/jpeg)


@dataclass
class Mesh:
    """A georeferenced scalar grid drawn with ``pcolormesh`` (lon × lat).

    ``lon`` (length ``n_col``) and ``lat`` (length ``n_row``) are cell-centre
    coordinates; ``grid`` is ``(n_row, n_col)`` with ``NaN`` cells transparent.
    Coloured through the *axes* ``cmap`` / ``clim`` (set on the :class:`Axes`),
    with the mappable surfaced so a colorbar can attach.  Used on a ``geo`` axes
    to paint a global VTEC field behind pierce-point markers.
    """
    lon: Sequence
    lat: Sequence
    grid: np.ndarray
    alpha: float = 1.0
    zorder: Optional[float] = None


Trace = Union[Line, StackedArea, HLine, TextBox, Raster, Markers,
              Scatter, Band, VLine, RgbRaster, PolarScatter, Stem, Mesh, Polygon,
              RgbImage]


# --------------------------------------------------------------------------- #
#  Axes / figure decorations
# --------------------------------------------------------------------------- #
@dataclass
class Legend:
    loc: str = "best"             # matplotlib-style location string


@dataclass
class ColorBar:
    label: str = ""
    ticks: Optional[Sequence[float]] = None
    tick_labels: Optional[Sequence[str]] = None   # categorical legend on the bar


@dataclass
class Axes:
    """One set of axes within a figure (a tile in a vertical stack)."""
    traces: list = field(default_factory=list)
    title: str = ""
    xlabel: str = ""
    ylabel: str = ""
    y2label: str = ""
    xscale: str = "linear"
    yscale: str = "linear"        # 'linear' | 'log'
    xlim: Optional[tuple] = None
    ylim: Optional[tuple] = None
    grid: bool = True
    legend: Optional[Legend] = None
    x_is_datetime: bool = True

    # Per-axes colormap for colored Scatter/PolarScatter (independent of the
    # figure-level raster map).  ``cmap`` is an (N,3) RGB array.
    cmap: Optional[np.ndarray] = None
    clim: Optional[tuple] = None
    colorbar: Optional["ColorBar"] = None

    projection: str = "rect"      # 'rect' | 'polar' | 'geo'
    axis_off: bool = False        # frameless (text-only panels)
    aspect_equal: bool = False    # equal x/y data scale (plan-view track: 1 m E == 1 m N)
    facecolor: Optional[RGB] = None  # axes background (e.g. ocean blue behind land polygons)

    # Polar (sky-plot) configuration; ignored unless projection == 'polar'.
    theta_zero: str = "N"         # compass zero location
    theta_dir: int = -1           # -1 clockwise (azimuth), +1 counter-clockwise
    rlim: Optional[tuple] = None
    rticks: Optional[Sequence[float]] = None
    rtick_labels: Optional[Sequence[str]] = None

    # Geographic (equirectangular) configuration; ignored unless projection ==
    # 'geo'.  A geo axes plots lon (x, deg) vs lat (y, deg) with a vendored world
    # coastline backdrop; ordinary Scatter/Line/Mesh/TextBox traces draw on top.
    geo_extent: Optional[tuple] = None   # (lon_min, lon_max, lat_min, lat_max); None => world
    coastline: bool = True               # draw the vendored Natural Earth coastline
    graticule_deg: float = 30.0          # lon/lat gridline spacing (0 => none)


@dataclass
class Figure:
    """A renderable figure: one or more vertically-stacked, x-linked axes.

    Raster figures (the strip charts) carry a single shared colormap / colour
    limits / colorbar at the figure level, matching ``plotSatEpochStrip`` where
    every constellation tile shares one map and one colorbar.
    """
    name: str = "figure"          # window / file token
    title: str = ""
    subtitle: str = ""
    axes: list = field(default_factory=list)
    width: float = 11.0           # inches
    height: float = 4.0
    link_x: bool = True

    # Raster styling (figure-wide); ``cmap`` is an (N,3) RGB array.
    cmap: Optional[np.ndarray] = None
    clim: Optional[tuple] = None
    colorbar: Optional[ColorBar] = None
    categorical: bool = False     # colorbar shows discrete category labels

    @property
    def is_raster(self) -> bool:
        return any(isinstance(t, Raster) for ax in self.axes for t in ax.traces)
