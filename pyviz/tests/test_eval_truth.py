"""Tests for gnssviz.eval.truth -- SINEX + SIF readers and propagation."""

from __future__ import annotations

import json

import numpy as np
import pytest

from gnssviz.eval.truth import (
    CompositeTruth,
    SifTruth,
    SinexTruth,
    make_default_truth,
)


# ---------------------------------------------------------------------------
# SINEX
# ---------------------------------------------------------------------------

_SNX = """\
%=SNX 2.10 TST 24:001:00000 TST 15:001:00000 24:001:00000 P 00006 2 X V
* Reference frame: ITRF2020 (test fixture)
+SOLUTION/ESTIMATE
*INDEX TYPE__ CODE PT SOLN _REF_EPOCH__ UNIT S __ESTIMATED VALUE____ _STD_DEV___
     1 STAX   TEST  A    1 15:001:00000 m    2 -2492006.9389 0.0003
     2 STAY   TEST  A    1 15:001:00000 m    2 -4659170.3174 0.0004
     3 STAZ   TEST  A    1 15:001:00000 m    2 3559273.6829 0.0005
     4 VELX   TEST  A    1 15:001:00000 m/y  2 -0.0150 0.0001
     5 VELY   TEST  A    1 15:001:00000 m/y  2 0.0050 0.0001
     6 VELZ   TEST  A    1 15:001:00000 m/y  2 -0.0100 0.0001
-SOLUTION/ESTIMATE
%ENDSNX
"""

_POS0 = np.array([-2492006.9389, -4659170.3174, 3559273.6829])
_VEL = np.array([-0.0150, 0.0050, -0.0100])
_EPOCH0 = 2015.0  # 15:001:00000


def _write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text, encoding="utf-8")
    return p


def test_sinex_propagates_with_velocity(tmp_path):
    snx = SinexTruth(_write(tmp_path, "test.snx", _SNX))
    yr = 2024.0
    tc = snx.resolve("TEST", yr)
    assert tc is not None
    expected = _POS0 + _VEL * (yr - _EPOCH0)
    assert np.allclose(tc.ecef, expected, atol=1e-6)
    assert tc.frame == "ITRF2020"
    assert tc.source == "sinex:TEST"
    # 1-sigma pulled straight from the STD_DEV column.
    assert np.allclose(tc.sigma, [0.0003, 0.0004, 0.0005], atol=1e-9)


def test_sinex_long_name_resolves_on_first_four_chars(tmp_path):
    snx = SinexTruth(_write(tmp_path, "test.snx", _SNX))
    a = snx.resolve("TEST00DEU", 2024.0)
    b = snx.resolve("TEST", 2024.0)
    assert a is not None and b is not None
    assert np.allclose(a.ecef, b.ecef)


def test_sinex_unknown_station_returns_none(tmp_path):
    snx = SinexTruth(_write(tmp_path, "test.snx", _SNX))
    assert snx.resolve("ZZZZ", 2024.0) is None


def test_sinex_epoch_zero_at_reference(tmp_path):
    snx = SinexTruth(_write(tmp_path, "test.snx", _SNX))
    tc = snx.resolve("TEST", _EPOCH0)
    assert np.allclose(tc.ecef, _POS0, atol=1e-9)  # dt == 0 -> no velocity term


# ---------------------------------------------------------------------------
# SIF
# ---------------------------------------------------------------------------

def _sif_doc(coords, vel_mm_per_yr, ref_epoch=2020.0):
    return {
        "METADATA": {"REFERENCE_FRAME": "ITRF2020", "REFERENCE_EPOCH": ref_epoch},
        "STATIONS": [{
            "ID": "TEST00DEU",
            "DOMES": "12345M678",
            "OCCUPATION": [{
                "START": "2000-01-01T00:00:00 UTC",
                "END": "2099-01-01T00:00:00 UTC",
                "COORDINATES": list(coords),
                "COORDINATE_SIGMAS": [0.001, 0.001, 0.002],
                "VELOCITY": list(vel_mm_per_yr),       # mm/yr
                "VELOCITY_SIGMAS": [0.1, 0.1, 0.1],
                "DO_NOT_USE": False,
            }],
        }],
    }


def test_sif_linear_propagation(tmp_path):
    coords = [3970000.0, 1000000.0, 4900000.0]
    vel_mm = [-12.0, 17.0, 9.0]   # mm/yr
    p = tmp_path / "sif.json"
    p.write_text(json.dumps(_sif_doc(coords, vel_mm)), encoding="utf-8")

    sif = SifTruth([p], with_periodic=False, with_psd=False)
    yr = 2024.0
    tc = sif.resolve("TEST00DEU", yr)
    assert tc is not None
    expected = np.array(coords) + np.array(vel_mm) * 1e-3 * (yr - 2020.0)
    assert np.allclose(tc.ecef, expected, atol=1e-9)
    assert tc.source == "sif:TEST00DEU"
    assert tc.frame == "ITRF2020"


def test_sif_resolves_by_4char_and_domes(tmp_path):
    p = tmp_path / "sif.json"
    p.write_text(json.dumps(_sif_doc([1e6, 2e6, 3e6], [0, 0, 0])), encoding="utf-8")
    sif = SifTruth([p])
    by_full = sif.resolve("TEST00DEU", 2024.0)
    by_four = sif.resolve("TEST", 2024.0)
    by_domes = sif.resolve("12345M678", 2024.0)
    assert by_full is not None and by_four is not None and by_domes is not None
    assert np.allclose(by_full.ecef, by_four.ecef)
    assert np.allclose(by_full.ecef, by_domes.ecef)


def test_sif_unknown_returns_none(tmp_path):
    p = tmp_path / "sif.json"
    p.write_text(json.dumps(_sif_doc([1e6, 2e6, 3e6], [0, 0, 0])), encoding="utf-8")
    assert SifTruth([p]).resolve("NOPE", 2024.0) is None


# ---------------------------------------------------------------------------
# Composite / factory
# ---------------------------------------------------------------------------

def test_composite_prefers_sinex_then_sif(tmp_path):
    snx = SinexTruth(_write(tmp_path, "t.snx", _SNX))
    sifp = tmp_path / "sif.json"
    # SIF carries a *different* station only (OTHR), so each backend owns one id.
    doc = _sif_doc([1e6, 2e6, 3e6], [0, 0, 0])
    doc["STATIONS"][0]["ID"] = "OTHR00DEU"
    doc["STATIONS"][0]["DOMES"] = "99999M999"
    sifp.write_text(json.dumps(doc), encoding="utf-8")
    sif = SifTruth([sifp])

    comp = CompositeTruth([snx, sif])
    assert comp.resolve("TEST", 2024.0).source.startswith("sinex:")
    assert comp.resolve("OTHR00DEU", 2024.0).source.startswith("sif:")
    assert comp.resolve("MISS", 2024.0) is None


def test_make_default_truth_skips_missing_sinex(tmp_path):
    sifp = tmp_path / "sif.json"
    sifp.write_text(json.dumps(_sif_doc([1e6, 2e6, 3e6], [0, 0, 0])), encoding="utf-8")
    comp = make_default_truth(
        sinex_paths=[tmp_path / "does_not_exist.snx"],
        sif_paths=[sifp],
    )
    tc = comp.resolve("TEST00DEU", 2024.0)
    assert tc is not None and tc.source.startswith("sif:")
