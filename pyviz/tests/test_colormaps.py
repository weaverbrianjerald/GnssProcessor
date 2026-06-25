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


def test_parula_turbo_fall_back_to_viridis_without_matplotlib():
    # The pure data layer must not depend on matplotlib; parula/turbo map to viridis.
    vir = strip_colormap("viridis", 32)
    assert np.allclose(strip_colormap("parula", 32), vir)
    assert np.allclose(strip_colormap("turbo", 32), vir)


def test_colormaps_module_has_no_matplotlib_import():
    import importlib
    src = importlib.util.find_spec("gnssviz.colormaps").origin
    text = open(src, encoding="utf-8").read()
    assert "import matplotlib" not in text and "from matplotlib" not in text
