"""IONEX reader test: parses the bundled CODE GIM if present.

The bundled fixture (``data/2024-129/products/*GIM.INX``) is .gitignored and only
present after ``bootstrap_internal_data.py``; the test skips cleanly when it is
absent so CI without the data tree still passes.
"""
import datetime as dt
import glob
import os

import numpy as np
import pytest

from gnssviz.ionex import nearest_map, read_ionex

_GIM = sorted(glob.glob(r"C:/GnssProcessor/data/**/*GIM*.INX", recursive=True))


@pytest.mark.skipif(not _GIM, reason="no bundled IONEX GIM (run bootstrap_internal_data.py)")
def test_read_code_gim():
    d = read_ionex(_GIM[0])
    # CODE convention: 71 x 73 grid (LAT 87.5..-87.5/-2.5, LON -180..180/5).
    assert d.lats.size == 71 and d.lons.size == 73
    assert abs(d.lats[0] - 87.5) < 1e-6 and abs(d.lats[-1] + 87.5) < 1e-6
    assert d.n_maps >= 2
    assert abs(d.height_km - 450.0) < 1e-6
    m = d.maps[len(d.maps) // 2]
    assert m.vtec.shape == (71, 73)
    vmax = float(np.nanmax(m.vtec))
    assert 0.0 < vmax < 400.0       # TECU, sane global peak


@pytest.mark.skipif(not _GIM, reason="no bundled IONEX GIM")
def test_nearest_map_picks_closest():
    d = read_ionex(_GIM[0])
    target = d.maps[0].epoch + dt.timedelta(minutes=20)
    assert nearest_map(d, target).epoch == d.maps[0].epoch
