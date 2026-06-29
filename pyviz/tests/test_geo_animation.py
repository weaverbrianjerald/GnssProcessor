"""Geo projection + Mesh + coloured Scatter render, and Animation -> GIF."""
import numpy as np

from gnssviz.animation import Animation, render_gif
from gnssviz.backends.matplotlib_backend import MatplotlibBackend
from gnssviz.colormaps import strip_colormap
from gnssviz.figures import Axes, ColorBar, Figure, Line, Mesh, Scatter, TextBox


def _geo_fig():
    cmap = strip_colormap("viridis", 64)
    lons = np.linspace(-180, 180, 37)
    lats = np.linspace(80, -80, 33)
    grid = np.cos(np.radians(lats))[:, None] * np.ones(lons.size)[None, :] * 40.0
    ax = Axes(projection="geo", title="t", cmap=cmap, clim=(0, 40),
              colorbar=ColorBar(label="VTEC"), x_is_datetime=False, grid=False,
              geo_extent=(-180, 180, -90, 90))
    ax.traces = [
        Mesh(lon=lons, lat=lats, grid=grid, alpha=0.6, zorder=0),
        Line(x=[-118, 139], y=[34, 35], color=(0.4, 0.4, 0.4), width=0.8),
        Scatter(x=[-118.0], y=[34.0], c=[20.0], size=60, marker="o",
                edgecolor=(1, 1, 1), linewidth=0.6, zorder=5),
        TextBox(x=-115, y=34, text="ABCD", data_coords=True, fontsize=8),
    ]
    return Figure(name="geo", title="geo", axes=[ax], width=10, height=5,
                  link_x=False)


def test_geo_figure_renders_png():
    b = MatplotlibBackend()
    r = b.render(_geo_fig())
    try:
        png = b.to_png_bytes(r, dpi=70)
    finally:
        b.close(r)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert len(png) > 2000


def test_tight_flag_changes_size():
    b = MatplotlibBackend()
    r = b.render(_geo_fig())
    try:
        tight = b.to_png_bytes(r, dpi=70, tight=True)
        fixed = b.to_png_bytes(r, dpi=70, tight=False)
    finally:
        b.close(r)
    assert tight[:8] == b"\x89PNG\r\n\x1a\n" and fixed[:8] == b"\x89PNG\r\n\x1a\n"


def test_animation_to_gif():
    b = MatplotlibBackend()
    frames = []
    for k in range(3):
        f = _geo_fig()
        f.name = f"f{k}"
        frames.append(f)
    anim = Animation(name="m", title="movie", frames=frames, fps=3.0)
    gif = render_gif(anim, b, dpi=60)
    assert gif[:6] in (b"GIF87a", b"GIF89a")
    assert len(gif) > 1000


def test_empty_animation_raises():
    b = MatplotlibBackend()
    try:
        render_gif(Animation(name="empty", frames=[]), b)
        assert False, "expected ValueError"
    except ValueError:
        pass
