"""Colormap library: shapes, reverse, passthrough, range."""
import numpy as np

from gnssviz.colormaps import strip_colormap


def test_shape_and_range():
    cm = strip_colormap("viridis", 16)
    assert cm.shape == (16, 3)
    assert cm.min() >= 0.0 and cm.max() <= 1.0


def test_reverse_is_flip():
    a = strip_colormap("vik", 32)
    b = strip_colormap("vik_r", 32)
    assert np.allclose(b, np.flipud(a))


def test_passthrough():
    arr = np.array([[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]])
    assert strip_colormap(arr) is arr


def test_bwr_length():
    assert strip_colormap("bwr", 33).shape == (33, 3)


def test_alias_diverging_is_vik():
    assert np.allclose(strip_colormap("diverging", 24), strip_colormap("vik", 24))


def test_parula_falls_back_to_viridis_without_matplotlib():
    # The pure data layer must not depend on matplotlib; parula has no portable
    # anchor table, so it maps to viridis.
    assert np.allclose(strip_colormap("parula", 32), strip_colormap("viridis", 32))


def test_turbo_is_a_real_rainbow_not_the_viridis_fallback():
    # turbo gained a real anchor table (2026-08-06) so both backends render the
    # genuine colours from the same array.
    t = strip_colormap("turbo", 32)
    assert not np.allclose(t, strip_colormap("viridis", 32))
    assert t.shape == (32, 3) and t.min() >= 0.0 and t.max() <= 1.0
    # Rainbow signature: starts dark blue, runs through green, ends dark red.
    assert t[0][2] > t[0][0] and t[0][2] > t[0][1]          # blue end
    assert t[-1][0] > t[-1][1] and t[-1][0] > t[-1][2]      # red end
    assert t[:, 1].max() > 0.9                              # green in the middle
    assert np.allclose(strip_colormap("jet", 32), t)        # jet aliases to turbo


def test_turbo_reversed_flips():
    t = strip_colormap("turbo", 16)
    assert np.allclose(strip_colormap("turbo_r", 16), np.flipud(t))


def test_colormaps_module_has_no_matplotlib_import():
    import importlib
    src = importlib.util.find_spec("gnssviz.colormaps").origin
    text = open(src, encoding="utf-8").read()
    assert "import matplotlib" not in text and "from matplotlib" not in text
