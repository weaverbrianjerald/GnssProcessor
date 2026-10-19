"""Tests for gnssviz.eval.truth: SINEX + SIF readers and propagation."""

from __future__ import annotations

import contextlib
import json
import math
import warnings
from pathlib import Path

import numpy as np
import pytest

from gnssviz.eval.truth import (
    SIF_GAP_TOLERANCE_YR,
    SIF_MAX_PREFIX_DISTANCE_M,
    CompositeTruth,
    SifExtrapolationWarning,
    SifPrefixRejectedWarning,
    SifTruth,
    SinexTruth,
    SiteTruth,
    TruthCoord,
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
# SIF lookup gate (S1), extrapolation flag (S2), per-occupation epoch (S3)
# ---------------------------------------------------------------------------

_REF = np.array([3970000.0, 1000000.0, 4900000.0])   # synthetic monument, |r| ~ 6.4e6 m
_FAR = np.array([-4147127.4937, -4152221.6682, -2490032.7642])   # ~1e4 km away


def _occ(start="2000-01-01", end="9999-12-31", coords=_REF, vel_mm=(0.0, 0.0, 0.0),
         dnu=False, ref_epoch=2020.0):
    """One OCCUPATION record; ``ref_epoch=None`` omits REFERENCE_EPOCH."""
    o = {"START": f"{start}T00:00:00 GPS", "END": f"{end}T00:00:00 GPS",
         "COORDINATES": [float(c) for c in coords],
         "COORDINATE_SIGMAS": [0.001, 0.001, 0.002],
         "VELOCITY": [float(v) for v in vel_mm], "DO_NOT_USE": dnu}
    if ref_epoch is not None:
        o["REFERENCE_EPOCH"] = ref_epoch
    return o


def _ant(start, type_, serial):
    return {"START": f"{start}T00:00:00 UTC", "END": "9999-12-31T00:00:00 UTC",
            "TYPE": type_, "SERIAL_NUMBER": serial}


def _rcv(start, type_, firmware="1.0", serial="R1"):
    return {"START": f"{start}T00:00:00 UTC", "END": "9999-12-31T00:00:00 UTC",
            "TYPE": type_, "FIRMWARE": firmware, "SERIAL_NUMBER": serial}


def _station(sid="TEST00DEU", domes="12345M678", approx=_REF, occupations=None,
             antennas=(), receivers=(), **extra):
    st = {"ID": sid, "DOMES": domes,
          "OCCUPATION": occupations if occupations is not None else [_occ()],
          "ANTENNA": list(antennas), "RECEIVER": list(receivers)}
    if approx is not None:
        st["APPROXIMATE_COORDINATES"] = [float(c) for c in approx]
    st.update(extra)
    return st


def _sif_file(tmp_path, stations, meta_epoch=2020.0, name="sif.json"):
    doc = {"METADATA": {"REFERENCE_FRAME": "ITRF2020", "REFERENCE_EPOCH": meta_epoch},
           "STATIONS": list(stations)}
    return _write(tmp_path, name, json.dumps(doc))


@contextlib.contextmanager
def _NoWarnings():
    """Turn any warning into an error inside the block (asserts silence)."""
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        yield


# -- S1: lookup --------------------------------------------------------------

def test_sif_exact_id_and_domes_ignore_position(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with _NoWarnings():
        assert sif._find("TEST00DEU", [0.0, 0.0, 6.4e6]).sid == "TEST00DEU"
        assert sif._find("12345M678", _FAR).sid == "TEST00DEU"
        assert sif._find(" test00deu ").sid == "TEST00DEU"


def test_sif_domes_is_tried_before_the_prefix(tmp_path):
    # The DOMES key's first four characters are another station's code.
    sif = SifTruth(_sif_file(tmp_path, [
        _station(sid="123400USA", domes="99999M001"),
        _station(sid="OTHR00DEU", domes="12345M678", approx=_FAR)]))
    with _NoWarnings():
        assert sif._find("12345M678").sid == "OTHR00DEU"


def test_sif_bare_four_char_key_accepted_without_position(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with _NoWarnings():
        assert sif._find(" test ").sid == "TEST00DEU"
        assert sif.resolve("TEST", 2024.0).source == "sif:TEST00DEU"


def test_sif_long_prefix_only_key_rejected_without_position(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with pytest.warns(SifPrefixRejectedWarning,
                      match=r"TEST00DEU for 'TEST00XYZ' rejected"):
        assert sif._find("TEST00XYZ") is None
    with pytest.warns(SifPrefixRejectedWarning):
        assert sif.resolve("TEST00XYZ", 2024.0) is None


@pytest.mark.parametrize("key", ["TEST00XYZ", "TEST"])
def test_sif_prefix_accepted_within_distance(tmp_path, key):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with _NoWarnings():
        assert sif._find(key, _REF + [3000.0, -4000.0, 0.0]).sid == "TEST00DEU"  # 5 km
        # The bound is inclusive: exactly SIF_MAX_PREFIX_DISTANCE_M is accepted.
        at_limit = _REF + [SIF_MAX_PREFIX_DISTANCE_M, 0.0, 0.0]
        assert sif._find(key, at_limit).sid == "TEST00DEU"


@pytest.mark.parametrize("key", ["TEST00XYZ", "TEST"])
def test_sif_prefix_rejected_beyond_distance(tmp_path, key):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with pytest.warns(SifPrefixRejectedWarning,
                      match=rf"TEST00DEU for '{key}' rejected: 10000\.5 m"):
        assert sif._find(key, _REF + [10000.5, 0.0, 0.0]) is None
    # A bare 4-char code is refused too when the position contradicts it.
    with pytest.warns(SifPrefixRejectedWarning):
        assert sif._find(key, _FAR) is None
    # The radius is a parameter; the default is the named constant.
    with _NoWarnings():
        assert sif._find(key, _REF + [10000.5, 0.0, 0.0],
                         max_prefix_distance_m=20000.0).sid == "TEST00DEU"
    assert SIF_MAX_PREFIX_DISTANCE_M == 10000.0


@pytest.mark.parametrize("approx", [
    [0.0, 0.0, 0.0],                       # placeholder
    [1000.0, 2000.0, 3000.0],              # norm < 1e6 m
    [1.0e6, 0.0, 0.0],                     # norm exactly 1e6 m: the bound is exclusive
    [float("nan"), 1.0e6, 6.0e6],          # not finite
    [3970000.0, 1000000.0],                # wrong length
    [3970000.0, 1000000.0, 4900000.0, 1.0],  # wrong length: exactly three, not >= 3
])
def test_sif_unusable_position_counts_as_none(tmp_path, approx):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with _NoWarnings():
        assert sif._find("TEST", approx).sid == "TEST00DEU"
    with pytest.warns(SifPrefixRejectedWarning):
        assert sif._find("TEST00XYZ", approx) is None


def test_sif_prefix_reference_position_fallbacks(tmp_path):
    near, far = _REF, _FAR
    sif = SifTruth(_sif_file(tmp_path, [
        # No APPROXIMATE_COORDINATES: the first NON-flagged occupation is the reference.
        _station(sid="AAAA00XXX", domes="", approx=None, occupations=[
            _occ(coords=far, dnu=True), _occ(coords=near)]),
        # Every occupation flagged: the first occupation is the reference.
        _station(sid="BBBB00XXX", domes="", approx=None, occupations=[
            _occ(coords=near, dnu=True), _occ(coords=far, dnu=True)]),
        # APPROXIMATE_COORDINATES is a placeholder: falls through to the occupation.
        _station(sid="CCCC00XXX", domes="", approx=[0.0, 0.0, 0.0],
                 occupations=[_occ(coords=near)]),
        # Nothing usable anywhere: a positioned prefix query is refused.
        _station(sid="DDDD00XXX", domes="", approx=None,
                 occupations=[_occ(coords=[0.0, 0.0, 0.0])]),
        # A usable APPROXIMATE_COORDINATES wins over a disagreeing occupation.
        _station(sid="EEEE00XXX", domes="", approx=near,
                 occupations=[_occ(coords=far)]),
        # Only the FIRST non-flagged occupation is consulted: a later usable one
        # is not, so with the first occupation also unusable nothing is left.
        _station(sid="FFFF00XXX", domes="", approx=None, occupations=[
            _occ(coords=[0.0, 0.0, 0.0]), _occ(coords=near)]),
        # First non-flagged unusable: the first occupation (flagged) decides,
        # not the later usable one.
        _station(sid="GGGG00XXX", domes="", approx=None, occupations=[
            _occ(coords=near, dnu=True), _occ(coords=[0.0, 0.0, 0.0]),
            _occ(coords=far)]),
    ]))
    with _NoWarnings():
        assert sif._find("AAAA00YYY", near).sid == "AAAA00XXX"
        assert sif._find("BBBB00YYY", near).sid == "BBBB00XXX"
        assert sif._find("CCCC00YYY", near).sid == "CCCC00XXX"
        assert sif._find("EEEE00YYY", near).sid == "EEEE00XXX"
        assert sif._find("GGGG00YYY", near).sid == "GGGG00XXX"
    with pytest.warns(SifPrefixRejectedWarning, match="AAAA00XXX"):
        assert sif._find("AAAA00YYY", far) is None      # the flagged record is skipped
    with pytest.warns(SifPrefixRejectedWarning, match="no usable reference position"):
        assert sif._find("DDDD00YYY", near) is None
    with pytest.warns(SifPrefixRejectedWarning, match=r"EEEE00XXX.*\d+\.\d m from"):
        assert sif._find("EEEE00YYY", far) is None
    with pytest.warns(SifPrefixRejectedWarning,
                      match="FFFF00XXX.*no usable reference position"):
        assert sif._find("FFFF00YYY", near) is None
    with pytest.warns(SifPrefixRejectedWarning, match=r"GGGG00XXX.*\d+\.\d m from"):
        assert sif._find("GGGG00YYY", far) is None


@pytest.mark.parametrize("key, approx", [
    ("TEST00XYZ", None),     # long key, no position
    ("TEST", _FAR),          # beyond the distance limit
    ("DDDD", _REF),          # candidate without a usable reference position
])
def test_sif_prefix_warning_names_the_callers_line(tmp_path, key, approx):
    # Through resolve() the refusal names the caller's line, as the
    # extrapolation warning does; through _find() directly it does too.
    sif = SifTruth(_sif_file(tmp_path, [
        _station(occupations=[_occ("2010-01-01", "2021-01-01")]),
        _station(sid="DDDD00XXX", domes="", approx=None,
                 occupations=[_occ(coords=[0.0, 0.0, 0.0])])]))
    here = Path(__file__).resolve()
    with pytest.warns(SifPrefixRejectedWarning) as via_resolve:
        assert sif.resolve(key, 2024.0, approx_ecef=approx) is None
    with pytest.warns(SifPrefixRejectedWarning) as via_find:
        assert sif._find(key, approx) is None
    with pytest.warns(SifExtrapolationWarning) as extrapolation:
        sif.resolve("TEST00DEU", 2024.0)
    for rec in (via_resolve, via_find, extrapolation):
        assert [Path(w.filename).resolve() for w in rec] == [here]


def test_sif_resolve_takes_an_optional_approx_position(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station()]))
    with _NoWarnings():
        tc = sif.resolve("TEST00XYZ", 2024.0, approx_ecef=_REF + [100.0, 0.0, 0.0])
    assert tc is not None and tc.source == "sif:TEST00DEU"
    # CompositeTruth keeps the base signature (no position): the long key is refused.
    with pytest.warns(SifPrefixRejectedWarning):
        assert CompositeTruth([sif]).resolve("TEST00XYZ", 2024.0) is None


# -- S2: extrapolation flag --------------------------------------------------

def test_sif_covered_epoch_has_no_extrapolation_flags(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station(
        antennas=[_ant("2000-01-01", "TRM59800.00     NONE", "A1"),
                  _ant("2023-06-01", "JAVRINGANT_DM   NONE", "B2")])]))
    with _NoWarnings():
        tc = sif.resolve("TEST00DEU", 2024.0)
    assert tc.extrapolated is False and tc.extrapolation_years == 0.0
    assert tc.antenna_change_in_gap is False and tc.receiver_change_in_gap is False
    assert (tc.segment_start_year, tc.segment_end_year) == (2000.0, 9999.0)


def test_sif_extrapolation_past_segment_end(tmp_path):
    vel = (-12.0, 17.0, 9.0)
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2010-01-01", "2021-01-01", vel_mm=vel)],
        antennas=[_ant("2010-01-01", "TRM59800.00     NONE", "A1"),
                  _ant("2022-06-01", "JAVRINGANT_DM   NONE", "B2")],
        receivers=[_rcv("2010-01-01", "SEPT POLARX5", "5.3.2"),
                   _rcv("2023-01-01", "SEPT POLARX5", "5.5.0")])]))   # firmware only
    with pytest.warns(SifExtrapolationWarning,
                      match=r"TEST00DEU.*\+3\.000 yr.*antenna change in gap: yes"
                            r".*receiver change in gap: no"):
        tc = sif.resolve("TEST00DEU", 2024.0)
    assert tc.extrapolated is True
    assert tc.extrapolation_years == 3.0
    assert (tc.segment_start_year, tc.segment_end_year) == (2010.0, 2021.0)
    assert tc.antenna_change_in_gap is True and tc.receiver_change_in_gap is False
    # S2 only reports: the coordinate is the plain linear extrapolation.
    assert np.array_equal(tc.ecef, _REF + np.array(vel) * 1e-3 * (2024.0 - 2020.0))


def test_sif_extrapolation_before_segment_start(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2015-01-01", "9999-12-31")],
        receivers=[_rcv("2010-01-01", "TRIMBLE NETR9"),
                   _rcv("2015-01-02", "SEPT POLARX5")])]))   # inside [t, start + tol]
    with pytest.warns(SifExtrapolationWarning, match=r"-0\.500 yr"):
        tc = sif.resolve("TEST00DEU", 2014.5)
    assert tc.extrapolated is True
    assert tc.extrapolation_years == pytest.approx(-0.5, abs=1e-12)
    assert tc.extrapolation_years < 0.0
    assert tc.receiver_change_in_gap is True and tc.antenna_change_in_gap is False


@pytest.mark.parametrize("change_date, expected", [
    ("2020-12-31", True),    # 1 day before the segment end: inside the tolerance
    ("2020-12-29", False),   # 3 days before: outside
    ("2023-12-31", True),    # inside (end, t]
    ("2024-06-01", False),   # after the query epoch
])
def test_sif_gap_tolerance_after_end(tmp_path, change_date, expected):
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2010-01-01", "2021-01-01")],
        antennas=[_ant("2010-01-01", "TRM59800.00     NONE", "A1"),
                  _ant(change_date, "JAVRINGANT_DM   NONE", "B2")])]))
    with pytest.warns(SifExtrapolationWarning):
        tc = sif.resolve("TEST00DEU", 2024.0)
    assert tc.antenna_change_in_gap is expected
    assert SIF_GAP_TOLERANCE_YR == 2.0 / 365.25


@pytest.mark.parametrize("change_date, expected", [
    ("2015-01-02", True),    # 1 day after the segment start: inside the tolerance
    ("2015-01-04", False),   # 3 days after: outside
    ("2014-01-01", False),   # before the query epoch
])
def test_sif_gap_tolerance_before_start(tmp_path, change_date, expected):
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2015-01-01", "9999-12-31")],
        antennas=[_ant("2010-01-01", "TRM59800.00     NONE", "A1"),
                  _ant(change_date, "JAVRINGANT_DM   NONE", "B2")])]))
    with pytest.warns(SifExtrapolationWarning):
        tc = sif.resolve("TEST00DEU", 2014.5)
    assert tc.antenna_change_in_gap is expected


@pytest.mark.parametrize("seg, t, edge, step, expected", [
    # Past the end: gap [end - tol, t].
    (("2010-01-01", "2021-01-01"), 2024.0, "end-tol", 0.0, True),
    (("2010-01-01", "2021-01-01"), 2024.0, "end-tol", -math.inf, False),
    (("2010-01-01", "2021-01-01"), 2024.0, "t", 0.0, True),
    (("2010-01-01", "2021-01-01"), 2024.0, "t", math.inf, False),
    # Before the start: gap [t, start + tol].
    (("2015-01-01", "9999-12-31"), 2014.5, "t", 0.0, True),
    (("2015-01-01", "9999-12-31"), 2014.5, "t", -math.inf, False),
    (("2015-01-01", "9999-12-31"), 2014.5, "start+tol", 0.0, True),
    (("2015-01-01", "9999-12-31"), 2014.5, "start+tol", math.inf, False),
])
def test_sif_gap_is_closed_at_both_ends(tmp_path, seg, t, edge, step, expected):
    # A change starting exactly on either bound is in the gap; one ulp outside
    # is not.  No file date lands exactly on end - tol, so the START is set on
    # the loaded record.
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(*seg)],
        antennas=[_ant("2000-01-01", "TRM59800.00     NONE", "A1"),
                  _ant("2001-01-01", "JAVRINGANT_DM   NONE", "B2")])]))
    st = sif._find("TEST00DEU")
    occ = st.occupations[0]
    bound = {"end-tol": occ.end_year - SIF_GAP_TOLERANCE_YR,
             "start+tol": occ.start_year + SIF_GAP_TOLERANCE_YR, "t": t}[edge]
    st.antennas[1].start_year = bound if step == 0.0 else float(np.nextafter(bound, step))
    with pytest.warns(SifExtrapolationWarning):
        assert sif.resolve("TEST00DEU", t).antenna_change_in_gap is expected


@pytest.mark.parametrize("prev, new, expected", [
    (("TRM59800.00     NONE", "A1"), ("JAVRINGANT_DM   NONE", "A1"), True),   # model
    (("TRM59800.00     NONE", "A1"), ("TRM59800.00     SCIS", "A1"), True),   # radome
    (("TRM59800.00     NONE", "A1"), ("TRM59800.00     NONE", "A2"), True),   # serial
    (("TRM59800.00     NONE", "A1"), ("TRM59800.00     NONE", "A1"), False),  # re-log
    (("TRM59800.00     NONE ", " A1"), ("TRM59800.00     NONE", "A1"), False),  # trim
])
def test_sif_antenna_change_kinds(tmp_path, prev, new, expected):
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2010-01-01", "2021-01-01")],
        antennas=[_ant("2010-01-01", *prev), _ant("2022-01-01", *new)])]))
    with pytest.warns(SifExtrapolationWarning):
        assert sif.resolve("TEST00DEU", 2024.0).antenna_change_in_gap is expected


@pytest.mark.parametrize("prev, new, expected", [
    (("TPS NET-G3A", "4.7", "R1"), ("SEPT POLARX5", "5.3.2", "R2"), True),   # type
    (("SEPT POLARX5", "5.3.2", "R1"), ("SEPT POLARX5", "5.5.0", "R1"), False),  # firmware
    (("SEPT POLARX5", "5.3.2", "R1"), ("SEPT POLARX5", "5.3.2", "R2"), False),  # serial
])
def test_sif_receiver_change_kinds(tmp_path, prev, new, expected):
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2010-01-01", "2021-01-01")],
        receivers=[_rcv("2010-01-01", *prev), _rcv("2022-01-01", *new)])]))
    with pytest.warns(SifExtrapolationWarning):
        assert sif.resolve("TEST00DEU", 2024.0).receiver_change_in_gap is expected


def test_sif_first_equipment_window_never_counts(tmp_path):
    # The only window starts inside the gap, but nothing precedes it to differ from.
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2010-01-01", "2021-01-01")],
        antennas=[_ant("2022-01-01", "JAVRINGANT_DM   NONE", "B2")],
        receivers=[_rcv("2022-01-01", "SEPT POLARX5")])]))
    with pytest.warns(SifExtrapolationWarning):
        tc = sif.resolve("TEST00DEU", 2024.0)
    assert tc.antenna_change_in_gap is False and tc.receiver_change_in_gap is False


def test_sif_equipment_windows_sorted_by_start(tmp_path):
    # File order is newest-first; the comparison runs on START order.
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ("2010-01-01", "2021-01-01")],
        antennas=[_ant("2022-01-01", "JAVRINGANT_DM   NONE", "B2"),
                  _ant("2010-01-01", "TRM59800.00     NONE", "A1")])]))
    with pytest.warns(SifExtrapolationWarning):
        assert sif.resolve("TEST00DEU", 2024.0).antenna_change_in_gap is True


def test_sif_flagged_covering_window_does_not_count_as_covered(tmp_path):
    sif = SifTruth(_sif_file(tmp_path, [_station(occupations=[
        _occ("2010-01-01", "2021-01-01"),
        _occ("2021-01-01", "9999-12-31", coords=_REF + 1.0, dnu=True)])]))
    with pytest.warns(SifExtrapolationWarning):
        tc = sif.resolve("TEST00DEU", 2024.0)
    assert tc.extrapolated is True and tc.extrapolation_years == 3.0
    assert np.array_equal(tc.ecef, _REF)


def test_truth_coord_extrapolation_defaults_for_other_backends(tmp_path):
    snx = SinexTruth(_write(tmp_path, "test.snx", _SNX)).resolve("TEST", 2030.0)
    site = SiteTruth(_write(tmp_path, "covl.json", json.dumps(_site_doc()))).resolve(
        "COVL", 2030.0)
    bare = TruthCoord(ecef=np.zeros(3), epoch_year=2024.0)
    for tc in (snx, site, bare):
        assert tc.extrapolated is False and tc.extrapolation_years == 0.0
        assert tc.segment_start_year is None and tc.segment_end_year is None
        assert tc.antenna_change_in_gap is False and tc.receiver_change_in_gap is False


# -- S3: per-occupation reference epoch --------------------------------------

def test_sif_occupation_reference_epoch_is_used(tmp_path):
    vel = (-13.4, 1.2, -6.5)
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(vel_mm=vel, ref_epoch=2010.0)])], meta_epoch=2020.0))
    tc = sif.resolve("TEST00DEU", 2024.35)
    assert np.array_equal(tc.ecef, _REF + np.array(vel) * 1e-3 * (2024.35 - 2010.0))
    # Against the old file-level epoch this is ten years of velocity.
    old = _REF + np.array(vel) * 1e-3 * (2024.35 - 2020.0)
    assert np.linalg.norm(tc.ecef - old) == pytest.approx(
        np.linalg.norm(np.array(vel)) * 1e-3 * 10.0, abs=1e-8)   # ulp at 6e6 m ~ 1e-9


@pytest.mark.parametrize("ref_epoch", [None, "2010.0", True])
def test_sif_occupation_reference_epoch_falls_back_to_metadata(tmp_path, ref_epoch):
    vel = (-12.0, 17.0, 9.0)
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(vel_mm=vel, ref_epoch=ref_epoch)])], meta_epoch=2015.0))
    tc = sif.resolve("TEST00DEU", 2024.35)
    assert np.array_equal(tc.ecef, _REF + np.array(vel) * 1e-3 * (2024.35 - 2015.0))


@pytest.mark.parametrize("ref_epoch", [2020.0, 2020, None])
def test_sif_reference_epoch_2020_is_bit_identical(tmp_path, ref_epoch):
    vel = (-12.0, 17.0, 9.0)
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(vel_mm=vel, ref_epoch=ref_epoch)])], meta_epoch=2020.0))
    for t in (2015.5, 2020.0, 2024.35):
        assert np.array_equal(sif.resolve("TEST00DEU", t).ecef,
                              _REF + np.array(vel) * 1e-3 * (t - 2020.0))


def test_sif_periodic_terms_keep_the_file_epoch(tmp_path):
    periodic = {"PERIODIC_TERMS": [{
        "START": "2000-01-01T00:00:00 GPS", "END": "9999-12-31T00:00:00 GPS",
        "FREQUENCIES": [{"PERIOD": 0.7, "X_COS": 10.0, "X_SIN": 0.0, "Y_COS": 0.0,
                         "Y_SIN": 0.0, "Z_COS": 0.0, "Z_SIN": 0.0}]}]}
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(ref_epoch=2010.0)], NONLINEAR_TERMS=periodic)],
        meta_epoch=2020.0), with_periodic=True, with_psd=False)
    t = 2024.0
    tc = sif.resolve("TEST00DEU", t)
    w = 2.0 * math.pi / 0.7
    expected_x = _REF[0] + 0.010 * math.cos(w * (t - 2020.0))     # file epoch
    wrong_x = _REF[0] + 0.010 * math.cos(w * (t - 2010.0))         # occupation epoch
    assert tc.ecef[0] == pytest.approx(expected_x, abs=1e-12)
    assert abs(expected_x - wrong_x) > 1e-3


def _periodic_block(start, end, mm):
    """One annual periodic block, X_COS = ``mm`` millimetres, ``end`` None = open."""
    return {"START": f"{start}-01-01T00:00:00 GPS",
            "END": "9999-12-31T00:00:00 GPS" if end is None else f"{end}-01-01T00:00:00 GPS",
            "FREQUENCIES": [{"PERIOD": 1.0, "X_COS": mm, "X_SIN": 0.0, "Y_COS": 0.0,
                             "Y_SIN": 0.0, "Z_COS": 0.0, "Z_SIN": 0.0}]}


@pytest.mark.parametrize("t, block_mm, warns", [
    (2008.5, 1.0, True),    # before the first window: the first block
    (2012.0, 1.0, False),   # covered
    (2014.6, 1.0, True),    # in the gap: the block that ended last
    (2016.0, 2.0, False),   # covered by the second block
    (2023.25, 3.0, True),   # past the last window: the last block
])
def test_sif_periodic_block_outside_its_window_follows_the_mpages_rule(tmp_path, t, block_mm, warns):
    """The seasonal term is one block chosen by the epoch (NGS M-PAGES v1.2): it does
    not fall to zero outside the windows while the linear term goes on.  The blocks are
    listed out of start order to show that the file order is irrelevant."""
    blocks = [_periodic_block(2015, 2019, 2.0), _periodic_block(2019, 2022, 3.0),
              _periodic_block(2010, 2014, 1.0)]
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(ref_epoch=2020.0)], NONLINEAR_TERMS={"PERIODIC_TERMS": blocks})],
        meta_epoch=2020.0), with_periodic=True, with_psd=False)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        tc = sif.resolve("TEST00DEU", t)
    periodic_warned = any("periodic window" in str(w.message) for w in caught)
    assert periodic_warned is warns
    expected_x = _REF[0] + 1e-3 * block_mm * math.cos(2.0 * math.pi * (t - 2020.0))
    assert tc.ecef[0] == pytest.approx(expected_x, abs=1e-9)


def test_sif_periodic_overlapping_windows_are_not_summed(tmp_path):
    blocks = [_periodic_block(2010, 2014, 1.0), _periodic_block(2012, 2018, 2.0)]
    sif = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(ref_epoch=2020.0)], NONLINEAR_TERMS={"PERIODIC_TERMS": blocks})],
        meta_epoch=2020.0), with_periodic=True, with_psd=False)
    t = 2013.0
    tc = sif.resolve("TEST00DEU", t)
    assert tc.ecef[0] == pytest.approx(
        _REF[0] + 1e-3 * 1.0 * math.cos(2.0 * math.pi * (t - 2020.0)), abs=1e-9)


# -- S4: post-seismic term relative to the reference epoch --------------------

_QUAKE_A = {"EVENT_EPOCH": "2010-01-01T00:00:00 UTC",
            "EAST": {"AEXP": [0.020, 0.005], "TEXP": [4.0, 0.2], "ALOG": [0.010], "TLOG": [0.5]},
            "NORTH": {"ALOG": [-0.030], "TLOG": [1.0]},
            "UP": {"AEXP": [0.004], "TEXP": [2.0]}}
_QUAKE_B = {"EVENT_EPOCH": "2021-01-01T00:00:00 UTC",
            "EAST": {"AEXP": [0.050], "TEXP": [1.5]}}
_QUAKE_C = {"EVENT_EPOCH": "2005-01-01T00:00:00 UTC",
            "NORTH": {"AEXP": [0.040], "TEXP": [3.0]}}


def _quake_year(quake):
    return float(quake["EVENT_EPOCH"][:4])        # every synthetic event is on 1 January


def _psd_enu_def(quakes, year):
    """Accumulated post-seismic (E, N, U) in metres at ``year``, from the definition."""
    out = np.zeros(3)
    for q in quakes:
        dT = year - _quake_year(q)
        if dT <= 0.0:
            continue
        for k, key in enumerate(("EAST", "NORTH", "UP")):
            c = q.get(key, {})
            for a, t in zip(c.get("ALOG", []), c.get("TLOG", [])):
                out[k] += a * math.log(1.0 + dT / t)
            for a, t in zip(c.get("AEXP", []), c.get("TEXP", [])):
                out[k] += a * (1.0 - math.exp(-dT / t))
    return out


def _geodetic_rad(ref):
    """Latitude and longitude [rad] of an ECEF point on WGS84 (fixed-point iteration)."""
    e2 = (1.0 / 298.257223563) * (2.0 - 1.0 / 298.257223563)
    lon = math.atan2(ref[1], ref[0])
    p = math.hypot(ref[0], ref[1])
    lat = math.atan2(ref[2], p * (1.0 - e2))
    for _ in range(8):
        n = 6378137.0 / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
        lat = math.atan2(ref[2] + e2 * n * math.sin(lat), p)
    return lat, lon


def _enu_of(delta, ref):
    """Local (E, N, U) at ECEF point ``ref`` of the ECEF vector ``delta``."""
    lat, lon = _geodetic_rad(ref)
    sl, cl, so, co = math.sin(lat), math.cos(lat), math.sin(lon), math.cos(lon)
    d = np.asarray(delta, dtype=float)
    return np.array([-so * d[0] + co * d[1],
                     -sl * co * d[0] - sl * so * d[1] + cl * d[2],
                     cl * co * d[0] + cl * so * d[1] + sl * d[2]])


def _psd_term(tmp_path, quakes, t, ref_epoch=2020.0, meta_epoch=2020.0):
    """The post-seismic term the evaluator applies at ``t``: local ENU metres, and the
    coordinate with and without it."""
    with_psd = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(ref_epoch=ref_epoch)],
        NONLINEAR_TERMS={"POST_SEISMIC": list(quakes)})], meta_epoch=meta_epoch, name="psd.json"),
        with_periodic=False, with_psd=True)
    linear = SifTruth(_sif_file(tmp_path, [_station(
        occupations=[_occ(ref_epoch=ref_epoch)])], meta_epoch=meta_epoch, name="lin.json"),
        with_periodic=False, with_psd=False)
    full = with_psd.resolve("TEST00DEU", t).ecef
    lin = linear.resolve("TEST00DEU", t).ecef
    return _enu_of(full - lin, lin), full, lin


def test_sif_psd_vanishes_at_the_reference_epoch(tmp_path):
    """The reference coordinate is the model position at the reference epoch, the
    post-seismic displacement accumulated by then included, so the term is zero there."""
    enu, full, lin = _psd_term(tmp_path, [_QUAKE_A], 2020.0)
    assert np.array_equal(full, lin)


@pytest.mark.parametrize("t", [2015.0, 2022.0, 2024.0, 2030.0])
def test_sif_psd_is_the_change_since_the_reference_epoch(tmp_path, t):
    enu, _full, _lin = _psd_term(tmp_path, [_QUAKE_A], t)
    want = _psd_enu_def([_QUAKE_A], t) - _psd_enu_def([_QUAKE_A], 2020.0)
    assert np.allclose(enu, want, atol=1e-7), (enu, want)
    # The relaxation since the event (the term before this was corrected) differs by
    # decimetres here, so the two rules are distinguishable.
    assert np.linalg.norm(_psd_enu_def([_QUAKE_A], t) - want) > 0.02


def test_sif_psd_before_the_event_removes_the_accumulated_displacement(tmp_path):
    """Before the event the term is minus what the reference coordinate carries: the
    position is the pre-event linear one."""
    enu, _full, _lin = _psd_term(tmp_path, [_QUAKE_A], 2005.0)
    assert np.allclose(enu, -_psd_enu_def([_QUAKE_A], 2020.0), atol=1e-7)


def test_sif_psd_event_after_the_reference_epoch_enters_in_full(tmp_path):
    quakes = [_QUAKE_A, _QUAKE_B]
    for t in (2020.5, 2023.0):
        enu, _full, _lin = _psd_term(tmp_path, quakes, t)
        want = _psd_enu_def(quakes, t) - _psd_enu_def(quakes, 2020.0)
        assert np.allclose(enu, want, atol=1e-7), (t, enu, want)
    # At 2020.5 the later event has not happened; at 2023.0 it adds its whole relaxation.
    assert _psd_enu_def([_QUAKE_B], 2020.5)[0] == 0.0
    assert _psd_enu_def([_QUAKE_B], 2023.0)[0] > 0.03


def test_sif_psd_uses_the_occupations_own_reference_epoch(tmp_path):
    enu, _full, _lin = _psd_term(tmp_path, [_QUAKE_C], 2024.0, ref_epoch=2010.0)
    want = _psd_enu_def([_QUAKE_C], 2024.0) - _psd_enu_def([_QUAKE_C], 2010.0)
    assert np.allclose(enu, want, atol=1e-7)
    _enu0, full0, lin0 = _psd_term(tmp_path, [_QUAKE_C], 2010.0, ref_epoch=2010.0)
    assert np.array_equal(full0, lin0)


# -- real-file checks (skipped when data/static/sif is not populated) --------

_SIF_DIR = Path(__file__).resolve().parents[2] / "data" / "static" / "sif"
_IGS_SIF = _SIF_DIR / "igs20_SIF.json"
_NCN_SIF = _SIF_DIR / "ncn20_SIF.json"
_needs_real_sif = pytest.mark.skipif(
    not (_IGS_SIF.is_file() and _NCN_SIF.is_file()),
    reason="real SIF files absent (data/static/sif/{igs20,ncn20}_SIF.json)")


@pytest.fixture(scope="module")
def real_sif():
    return SifTruth([_IGS_SIF, _NCN_SIF], with_periodic=True, with_psd=True)


@_needs_real_sif
def test_real_sif_gamb00pyf_does_not_resolve_to_gamb00usa(real_sif):
    with pytest.warns(SifPrefixRejectedWarning, match="GAMB00USA for 'GAMB00PYF'"):
        assert real_sif.resolve("GAMB00PYF", 2024.35) is None
    gambier = [-4147127.4937, -4152221.6682, -2490032.7642]     # the real GAMB monument
    with pytest.warns(SifPrefixRejectedWarning, match=r"7687\d{3}\.\d m"):
        assert real_sif._find("GAMB00PYF", gambier) is None
    with _NoWarnings():
        own = real_sif._find("GAMB00USA").approx
        assert real_sif._find("GAMB00PYF", own).sid == "GAMB00USA"


@_needs_real_sif
def test_real_sif_sthl_rename_needs_a_position(real_sif):
    with pytest.warns(SifPrefixRejectedWarning, match="STHL00SHN for 'STHL00GBR'"):
        assert real_sif._find("STHL00GBR") is None
    with _NoWarnings():
        shn = real_sif._find("STHL00SHN").approx
        assert real_sif._find("STHL00GBR", shn).sid == "STHL00SHN"
        assert real_sif._find("P389").sid == "P38900USA"
        assert real_sif._find("cit1").sid == "CIT100USA"


@_needs_real_sif
def test_real_sif_azaz00usa_uses_its_2010_epoch(real_sif, tmp_path):
    occ = real_sif._find("AZAZ00USA").occupations
    assert len(occ) == 1 and occ[0].ref_epoch == 2010.0
    # Its only occupation is DO_NOT_USE, and the selection rule is unchanged,
    # so the registry itself does not resolve it.
    assert real_sif.resolve("AZAZ00USA", 2024.35) is None
    # With the flag lifted, the real record propagates from its own epoch.
    doc = json.loads(_NCN_SIF.read_text(encoding="utf-8-sig"))
    sj = next(s for s in doc["STATIONS"] if s["ID"] == "AZAZ00USA")
    sj["OCCUPATION"][0]["DO_NOT_USE"] = False
    p = _write(tmp_path, "azaz.json",
               json.dumps({"METADATA": doc["METADATA"], "STATIONS": [sj]}))
    tc = SifTruth(p, with_periodic=False, with_psd=False).resolve("AZAZ00USA", 2024.35)
    o = sj["OCCUPATION"][0]
    coords = np.array(o["COORDINATES"], dtype=float)
    vel = np.array(o["VELOCITY"], dtype=float) * 1e-3
    assert np.array_equal(tc.ecef, coords + vel * (2024.35 - 2010.0))
    assert 0.11 < np.linalg.norm(tc.ecef - (coords + vel * (2024.35 - 2020.0))) < 0.16


@_needs_real_sif
def test_real_sif_nium00niu_extrapolated_across_an_antenna_change(real_sif):
    # Two warnings: the occupation segment (antenna change in the gap) and, because
    # the last periodic window ended 0.33 yr earlier, the seasonal block continued.
    with pytest.warns(SifExtrapolationWarning) as rec:
        tc = real_sif.resolve("NIUM00NIU", 2024.35)
    texts = [str(w.message) for w in rec]
    assert any("NIUM00NIU" in t and "antenna change in gap: yes" in t for t in texts)
    assert any("NIUM00NIU" in t and "periodic window (after last)" in t for t in texts)
    assert tc.extrapolated is True and tc.extrapolation_years > 0.3
    assert tc.segment_end_year < 2024.35
    assert tc.antenna_change_in_gap is True and tc.receiver_change_in_gap is False
    with _NoWarnings():
        assert real_sif.resolve("NIUM00NIU", 2010.0).extrapolated is False


@_needs_real_sif
def test_real_sif_chur00can_receiver_change_and_palm00ata_relog(real_sif):
    with pytest.warns(SifExtrapolationWarning):
        chur = real_sif.resolve("CHUR00CAN", 2024.35)
        palm = real_sif.resolve("PALM00ATA", 2024.35)
    assert chur.extrapolated is True
    assert chur.receiver_change_in_gap is True and chur.antenna_change_in_gap is False
    # PALM00ATA re-logs the same antenna (type and serial) inside its gap.
    assert palm.extrapolated is True
    assert palm.antenna_change_in_gap is False and palm.receiver_change_in_gap is False

# The IGS weekly combined SINEX of GPS week 2313 (IGS0OPSSNX_20241260000_07D_07D_SOL.SNX): a
# mean position at 2024-05-08 12:00 (formal 3D sigma 0.3 to 1.9 mm).  The eight stations are
# those of the 2024-129 field golden roster that carry a post-seismic term at that epoch; the
# last item is the per-component horizontal tolerance in mm.  It is twice the 95th percentile
# (4.1 mm) of the same horizontal difference over 264 stations without a post-seismic block, so
# it stands for weekly noise and unmodelled motion, not for the post-seismic term.  NIUM00NIU is
# extrapolated 0.33 yr past its occupation across an antenna change and gets 12 mm.
_SINEX_PSD_STATIONS = [
    ("GCGO00USA", (-2281621.77483, -1453585.12678, 5756964.86859), 8.0),
    ("SOLO00SLB", (-5911340.08837, 2156887.93540, -1038663.46655), 8.0),
    ("CUSV00THA", (-1132915.09704, 6092528.52228, 1504633.12884), 8.0),
    ("MAC100AUS", (-3464038.98262, 1334173.29737, -5169223.81332), 8.0),
    ("NIUM00NIU", (-5937161.02270, -1054674.91356, -2071385.86297), 12.0),
    ("COCO00AUS", (-741951.26822, 6190961.73679, -1337767.15664), 8.0),
    ("REYK00ISL", (2587383.90543, -1043033.59986, 5716564.19577), 8.0),
    ("MIZU00JPN", (-3857171.22964, 3108692.78119, 4004040.11358), 8.0),
]
_GOLDEN_EPOCH = 2024.0 + (129.0 - 0.5) / 366.0     # 2024-05-08 12:00, the SINEX reference epoch


@pytest.fixture(scope="module")
def real_sif_no_psd():
    return SifTruth([_IGS_SIF, _NCN_SIF], with_periodic=True, with_psd=False)


@_needs_real_sif
@pytest.mark.parametrize("sid, snx, tol_mm", _SINEX_PSD_STATIONS)
def test_real_sif_psd_station_matches_the_igs_weekly_sinex(real_sif, real_sif_no_psd, sid, snx, tol_mm):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", SifExtrapolationWarning)
        tc = real_sif.resolve(sid, _GOLDEN_EPOCH)
    assert tc is not None and tc.source == f"sif:{sid}"
    e, n, _u = 1e3 * _enu_of(tc.ecef - np.array(snx), np.array(snx))
    assert abs(e) <= tol_mm and abs(n) <= tol_mm, (sid, round(e, 1), round(n, 1))
    # The term itself is millimetres to decimetres; the full relaxation since the event
    # (the rule before this one) was up to 1.6 m (MIZU00JPN).
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", SifExtrapolationWarning)
        shift = np.linalg.norm(tc.ecef - real_sif_no_psd.resolve(sid, _GOLDEN_EPOCH).ecef)
    assert shift < 0.2, (sid, shift)


@_needs_real_sif
def test_real_sif_antc00chl_psd_term_is_the_change_since_2020(real_sif, real_sif_no_psd):
    """ANTC00CHL carries 0.66 m of post-seismic displacement (Maule 2010) at 2020.0; the
    term at 2024.36 is the 69 mm accumulated since."""
    full = real_sif.resolve("ANTC00CHL", 2024.36).ecef
    lin = real_sif_no_psd.resolve("ANTC00CHL", 2024.36).ecef
    assert 0.05 < np.linalg.norm(full - lin) < 0.09


# ---------------------------------------------------------------------------
# Site database
# ---------------------------------------------------------------------------

_SITE_ECEF = [-2497215.425, -3805289.764, 4453261.057]


def _site_doc(**site_over):
    site = {
        "ecef_m": list(_SITE_ECEF),
        "sigma_m": [0.02, 0.02, 0.03],
        "source": "static PPP survey-in",
        "aliases": ["COVL00USA"],
    }
    site.update(site_over)
    return {"frame": "IGS20", "ref_epoch": 2026.0, "sites": {"COVL": site}}


def test_site_basic_resolve(tmp_path):
    p = _write(tmp_path, "covl.json", json.dumps(_site_doc()))
    src = SiteTruth(p)
    tc = src.resolve("COVL", 2026.0)
    assert tc is not None
    assert np.allclose(tc.ecef, _SITE_ECEF, atol=1e-9)   # dt == 0 -> no velocity
    assert tc.frame == "IGS20"
    assert tc.source.startswith("site:COVL")
    assert np.allclose(tc.sigma, [0.02, 0.02, 0.03], atol=1e-9)
    assert src.stations() == ["COVL"]


def test_site_resolves_by_alias_and_prefix(tmp_path):
    p = _write(tmp_path, "covl.json", json.dumps(_site_doc()))
    src = SiteTruth(p)
    by_alias = src.resolve("COVL00USA", 2026.0)      # declared alias
    by_prefix = src.resolve("COVL0OTHER", 2026.0)    # 4-char prefix of canonical
    assert by_alias is not None and by_prefix is not None
    assert np.allclose(by_alias.ecef, _SITE_ECEF)
    assert np.allclose(by_prefix.ecef, _SITE_ECEF)


def test_site_velocity_propagation_and_epoch_override(tmp_path):
    # Site-level ref_epoch overrides the file default; velocity propagates.
    p = _write(tmp_path, "covl.json",
               json.dumps(_site_doc(velocity_m_yr=[0.01, -0.02, 0.03],
                                    ref_epoch=2020.0)))
    src = SiteTruth(p)
    tc = src.resolve("COVL", 2025.0)
    expected = np.array(_SITE_ECEF) + np.array([0.01, -0.02, 0.03]) * 5.0
    assert np.allclose(tc.ecef, expected, atol=1e-9)


def test_site_unknown_returns_none_and_no_position_skipped(tmp_path):
    doc = _site_doc()
    doc["sites"]["NOPS"] = {"source": "missing ecef"}   # no ecef_m -> skipped
    p = _write(tmp_path, "covl.json", json.dumps(doc))
    src = SiteTruth(p)
    assert src.resolve("ZZZZ", 2026.0) is None
    assert "NOPS" not in src.stations()


def test_make_default_truth_includes_sites(tmp_path):
    sitep = _write(tmp_path, "covl.json", json.dumps(_site_doc()))
    comp = make_default_truth(site_paths=[sitep])
    tc = comp.resolve("COVL", 2026.0)
    assert tc is not None and tc.source.startswith("site:COVL")


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
