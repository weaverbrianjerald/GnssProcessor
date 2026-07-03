"""Tests for the network product-file parser + viewer (Card L8).

Exercises :func:`gnssviz.product_file.parse_product_file` against synthetic v2 /
v1 files (grammar re-derived from ``src/core/products/NetworkProductIO.cpp``) and
the :mod:`gnssviz.viewers.network_product_file` viewer's build + ViewerSkip
paths.  Numbers cross-check the C++ ``UraAndClockDriftRoundTrip`` fixture
(clock_c1_m_s = -0.0123456, clock_ura_m = 0.037).
"""
import math

import pytest

from gnssviz.product_file import parse_product_file
from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_product_file import build_network_product_file


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


# --------------------------------------------------------------------------- #
# A v2 file exercising every record + CRLF line endings + an unknown tag.
# datum_consts on the E line is a CONCATENATED string ("GE"), NOT comma-sep.
_V2 = (
    "# gnss_network_product v2\r\n"
    "S,CIT100USA,-1,windup0,1,1,1,2,0.05,1,COD0MGXFIN.SP3+COD0MGXFIN.CLK,G:G05,E:E11\r\n"
    "# C,sat,clock_c0_m  (comment skipped)\r\n"
    "E,2313,259200.000,3,42,1,0\r\n"
    "G,2313,259200.500\r\n"
    "C,G01,12.5,-0.0123456\r\n"
    "B,G01,C1W,0.3\r\n"
    "P,G01,L1C,-0.7,1,1\r\n"
    "U,G01,0.037\r\n"
    "C,E11,-3.0\r\n"
    "U,E11,0.021\r\n"
    "E,2313,259230.000,3,42,1,1,GE\r\n"
    "R,G01\r\n"
    "C,G01,12.6\r\n"
    "U,G01,0.041\r\n"
    "U,E11,0.022\r\n"
    "Z,junk,line\r\n"          # unknown tag -> counted + skipped
)

# A v1 file: no S line (contract None), no c1, no U, no datum_consts.
_V1 = (
    "# gnss_network_product v1\n"
    "# E,week,tow,iod_ssr,provider_id,solution_id,datum_changed\n"
    "E,2313,259200.000,3,42,1,0\n"
    "C,G01,5.0\n"
    "B,G01,C1W,0.2\n"
    "P,G01,L1C,0.1,1,1\n"
)


def _write_v2(tmp_path, name="network_products.txt"):
    p = tmp_path / name
    p.write_bytes(_V2.encode("utf-8"))
    return str(p)


# --------------------------------------------------------------------------- #
# Parser tests
# --------------------------------------------------------------------------- #
def test_parse_v2_header_and_contract(tmp_path):
    pf = parse_product_file(_write_v2(tmp_path))
    assert pf.header_version == "v2"
    assert pf.n_epochs == 2
    assert pf.unknown_tags == {"Z": 1}
    assert pf.malformed == 0

    c = pf.contract
    assert c is not None
    assert c.reference_receiver_id == "CIT100USA"
    assert c.bias_sign == -1.0
    assert c.windup_yaw_model_id == "windup0"
    assert c.amb_datum == 1 and c.isb_datum == 1
    assert c.emit_code_bias is True
    assert c.code_bias_datum == 2
    assert c.disc_threshold_m == 0.05
    assert c.emit_clock_disc is True
    assert c.apriori_product_id == "COD0MGXFIN.SP3+COD0MGXFIN.CLK"
    assert c.clock_pivot_sat == {"G": "G05", "E": "E11"}


def test_parse_v2_records_and_carried_flags(tmp_path):
    pf = parse_product_file(_write_v2(tmp_path))
    df = pf.to_frame()

    # C-line drift (field 4) exact round-trip, matches C++ fixture.
    g01_c = df[(df["sat"] == "G01") & (df["record"] == "C")].sort_values("tow")
    assert g01_c.iloc[0]["value"] == 12.5
    assert g01_c.iloc[0]["clock_c1_m"] == -0.0123456
    assert bool(g01_c.iloc[0]["has_c1"]) is True
    # E11 clock had no drift field -> has_c1 False, clock_c1_m NaN.
    e11_c = df[(df["sat"] == "E11") & (df["record"] == "C")].iloc[0]
    assert bool(e11_c["has_c1"]) is False
    assert math.isnan(e11_c["clock_c1_m"])

    # URA round-trip.
    u = df[(df["sat"] == "G01") & (df["record"] == "U")].sort_values("tow")
    assert u.iloc[0]["value"] == 0.037

    # P-line integer fields.
    p = df[(df["record"] == "P") & (df["sat"] == "G01")].iloc[0]
    assert p["value"] == -0.7
    assert bool(p["signal_integer"]) is True
    assert int(p["wide_lane_integer"]) == 1

    # Epoch-2 datum_changed + concatenated datum_consts carried onto its rows.
    ep2 = df[df["tow"] == 259230.0]
    assert ep2["datum_changed"].all()
    assert (ep2["datum_consts"] == "GE").all()

    # R,G01 in epoch 2 -> retro flag on G01 rows only, and back-filled onto rows
    # emitted before the R line was read (C,G01 precedes... here R precedes C,
    # but the back-fill path is still covered by U,E11 staying unflagged).
    ep2_g01 = ep2[ep2["sat"] == "G01"]
    ep2_e11 = ep2[ep2["sat"] == "E11"]
    assert ep2_g01["retro"].all()
    assert not ep2_e11["retro"].any()
    # Epoch-1 rows are never retro.
    ep1 = df[df["tow"] == 259200.0]
    assert not ep1["retro"].any()


def test_parse_v1_no_optionals(tmp_path):
    p = tmp_path / "network_products.txt"
    p.write_text(_V1, encoding="utf-8")
    pf = parse_product_file(str(p))
    assert pf.header_version == "v1"
    assert pf.contract is None
    assert pf.n_epochs == 1
    assert pf.unknown_tags == {}
    df = pf.to_frame()
    # No U records in v1 file.
    assert (df["record"] == "U").sum() == 0
    # C row has no drift.
    c = df[df["record"] == "C"].iloc[0]
    assert bool(c["has_c1"]) is False


def test_parse_malformed_short_s_line(tmp_path):
    # An S line with < 11 fields is flagged malformed, contract stays None.
    body = ("# gnss_network_product v2\n"
            "S,CIT100USA,-1,windup0\n"        # too short (4 fields)
            "E,2313,259200.000,3,42,1,0\n"
            "C,G01,5.0\n")
    p = tmp_path / "network_products.txt"
    p.write_text(body, encoding="utf-8")
    pf = parse_product_file(str(p))
    assert pf.contract is None
    assert pf.malformed >= 1
    assert pf.n_epochs == 1


# --------------------------------------------------------------------------- #
# Viewer tests
# --------------------------------------------------------------------------- #
def test_viewer_builds_figures(tmp_path):
    _write_v2(tmp_path)
    res = build_network_product_file(_ctx(tmp_path))
    # URA timeline (>=1 constellation tile) + per-epoch counts figure.
    assert len(res.figures) == 2
    ura_fig = res.figures[0]
    # G and E both present -> two tiles, log-y.
    assert len(ura_fig.axes) == 2
    assert all(ax.yscale == "log" for ax in ura_fig.axes)
    # Summary surfaces version, contract, URA stats, and the chosen filename.
    assert "v2" in res.summary
    assert "CIT100USA" in res.summary
    assert "network_products.txt" in res.summary
    assert "G05" in res.summary and "E11" in res.summary


def test_viewer_newest_file_wins(tmp_path):
    import os
    import time
    # Older file first.
    old = tmp_path / "network_products_old.txt"
    old.write_text(_V1, encoding="utf-8")
    old_time = time.time() - 100
    os.utime(old, (old_time, old_time))
    # Newer file.
    _write_v2(tmp_path, "network_products.txt")
    res = build_network_product_file(_ctx(tmp_path))
    # Newest (the v2 file) must be chosen.
    assert "v2" in res.summary
    assert "network_products.txt" in res.summary


def test_viewer_appends_contract_check(tmp_path):
    _write_v2(tmp_path)
    (tmp_path / "network_contract_check.csv").write_text(
        "check,status,detail\n"
        "file_readable,PASS,\n"
        "contract_present,PASS,\n"
        "overall,PASS,\n", encoding="utf-8")
    res = build_network_product_file(_ctx(tmp_path))
    assert "network_contract_check.csv" in res.summary
    assert "file_readable" in res.summary


def test_viewer_skip_without_product_file(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_product_file(_ctx(tmp_path))


def test_viewer_v1_no_ura(tmp_path):
    # v1 file has no U records -> only the counts figure, contract NONE in summary.
    p = tmp_path / "network_products.txt"
    p.write_text(_V1, encoding="utf-8")
    res = build_network_product_file(_ctx(tmp_path))
    assert len(res.figures) == 1   # counts only (no URA figure)
    assert "NONE" in res.summary
