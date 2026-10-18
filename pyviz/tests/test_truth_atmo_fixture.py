"""The ``truth_atmo.csv`` test fixture keeps the simulator's own header.

``conftest.exam_fixture`` writes a stand-in for the GnssSimulator answer key.
When the stand-in drifts from the real file (a column missing, as the
``dstec_turb_tecu`` column once was), the exam viewers are tested against a
schema the simulator never writes.  The header is compared with the literal in
``src/sim/main.cpp`` whenever that source is present.
"""
import os
import re

import pandas as pd
import pytest

_SIM_MAIN = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "..", "src", "sim", "main.cpp")


def _simulator_header():
    """Header of truth_atmo.csv as the simulator writes it, or None."""
    if not os.path.isfile(_SIM_MAIN):
        return None
    with open(_SIM_MAIN, "r", encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r'atmo_csv\s*<<\s*((?:"[^"\n]*"\s*)+)', src)
    if not m:
        return None
    pieces = re.findall(r'"([^"\n]*)"', m.group(1))
    return "".join(pieces).replace("\\n", "")


def test_fixture_header_matches_simulator(exam_fixture):
    want = _simulator_header()
    if want is None:
        pytest.skip("src/sim/main.cpp not available next to pyviz")
    truth, _ = exam_fixture()
    got = pd.read_csv(os.path.join(truth, "truth_atmo.csv"), nrows=0)
    assert ",".join(got.columns) == want


def test_fixture_rows_have_one_value_per_column(exam_fixture):
    truth, _ = exam_fixture()
    path = os.path.join(truth, "truth_atmo.csv")
    with open(path, "r", encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    width = len(lines[0].split(","))
    assert width == 21
    assert all(len(ln.split(",")) == width for ln in lines[1:])
    df = pd.read_csv(path)
    assert "dstec_turb_tecu" in df.columns
    assert (df["dstec_turb_tecu"] == 0.0).all()
