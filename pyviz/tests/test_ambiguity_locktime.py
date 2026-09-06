"""Locktime reconstruction: the sigma-heuristic + detector-flag union rule.

Background (viewer docstring, measured on the exam hero run 2026-08-31): the
ambiguity trace logs POST-update sigma, so in a converged filter a slip reset
often shows NO reinflation at all -- and the few that log the raw reset sigma
collapse back within an epoch, which is exactly the bridge hold-and-restore
signature.  Resets must therefore also be forced by the detector slip / LLI
flags, without defeating the genuine bridge-hold rendering for LLI holds.
"""
import numpy as np
import pandas as pd

from gnssviz import TraceContext
from gnssviz.viewers.ambiguity_locktime import (
    _load_slip_events, _reconstruct_locktime, build_ambiguity_locktime)

_DT = 30.0


def _series(n, std=0.03):
    """(tsec, stdv): n samples every 30 s at a flat converged sigma."""
    return np.arange(n) * _DT, np.full(n, std)


# --------------------------------------------------------------------------- #
#  _reconstruct_locktime unit behaviour
# --------------------------------------------------------------------------- #
def test_slip_flag_forces_reset_without_sigma_step():
    # Converged post-update sigma shows nothing; the detector flag alone must
    # break the lock (the hero run: 38/43 slips had no visible up-step).
    t, s = _series(10)
    mask = np.zeros(10, bool)
    mask[5] = True
    lock, rt = _reconstruct_locktime(t, s, slip_mask=mask)
    assert rt[5] == 2 and lock[5] == 0.0
    assert rt[6] == 0 and lock[6] == _DT          # accumulates again
    assert lock[4] == 4 * _DT                     # untouched before the slip


def test_persisted_flags_collapse_to_one_reset():
    # MW keeps flagging for several epochs after a hit: one event, one reset.
    t, s = _series(12)
    mask = np.zeros(12, bool)
    mask[5:9] = True
    lock, rt = _reconstruct_locktime(t, s, slip_mask=mask)
    assert list(np.flatnonzero(rt == 2)) == [5]
    assert lock[8] == 3 * _DT


def test_flagged_sigma_spike_is_reset_not_bridge():
    # The 1000-sigma park + one-epoch collapse matches the bridge-restore
    # signature; with a slip flag it must be a RESET (the hero's 5 mis-held).
    t, s = _series(10)
    s[5] = 1000.0
    mask = np.zeros(10, bool)
    mask[5] = True
    lock, rt = _reconstruct_locktime(t, s, slip_mask=mask)
    assert rt[5] == 2 and lock[5] == 0.0
    assert not np.any(rt == 3)


def test_unflagged_spike_with_restore_still_bridges():
    # Regression: the soft-LLI hold-and-restore rendering survives unchanged
    # when NO detector flag coincides.
    t, s = _series(10)
    s[5] = 1000.0
    lock, rt = _reconstruct_locktime(t, s)
    assert rt[5] == 3                              # held, lock continuous
    assert not np.any(rt == 2)
    assert lock[6] == 6 * _DT


def test_lli_flag_resets_when_no_hold_signature():
    # A hard LLI reset in a converged filter: no sigma park, flag alone.
    t, s = _series(10)
    mask = np.zeros(10, bool)
    mask[5] = True
    lock, rt = _reconstruct_locktime(t, s, lli_mask=mask)
    assert rt[5] == 2 and lock[5] == 0.0


def test_lli_flag_keeps_bridge_when_hold_restores():
    # A genuine turbo_edit_bridge_lli hold: parked sigma restored next epoch.
    # LLI is SOFT evidence -- the bridge classification must win.
    t, s = _series(10)
    s[5] = 1000.0
    mask = np.zeros(10, bool)
    mask[5] = True
    lock, rt = _reconstruct_locktime(t, s, lli_mask=mask)
    assert rt[5] == 3
    assert not np.any(rt == 2)


def test_cold_start_flags_fold_into_arc_start():
    # Detector noise on the very first epochs (hero: E30/G27 at 522000): the
    # arc start already has lock 0, no extra in-arc reset.
    t, s = _series(8)
    mask = np.zeros(8, bool)
    mask[0:2] = True
    lock, rt = _reconstruct_locktime(t, s, slip_mask=mask)
    assert rt[0] == 1
    assert not np.any(rt == 2)


def test_no_masks_is_legacy_behaviour():
    t, s = _series(8)
    s[4] = 0.4                                     # up-step, no restore below
    s[5:] = 0.4
    lock_a, rt_a = _reconstruct_locktime(t, s)
    lock_b, rt_b = _reconstruct_locktime(t, s, slip_mask=None, lli_mask=None)
    assert np.array_equal(rt_a, rt_b) and np.array_equal(lock_a, lock_b)
    assert rt_a[4] == 2


# --------------------------------------------------------------------------- #
#  End-to-end through the builder with tiny fixture CSVs
# --------------------------------------------------------------------------- #
def _write_fixture(tmp_path, *, with_mw=True):
    n = 10
    week, tow0 = 2313, 522000.0
    tow = tow0 + np.arange(n) * _DT
    amb = pd.DataFrame({
        "week": week, "tow_s": tow, "key": "G10_L1C_L2W",
        "value_m": np.where(np.arange(n) >= 5, 119.9, 100.0),   # 19.9 m step
        "std_m": 0.03,                                          # converged, flat
    })
    amb.to_csv(tmp_path / "ambiguity_trace.csv", index=False)
    if with_mw:
        mw = pd.DataFrame({
            "week": week, "tow_s": tow, "satellite": "G10",
            "is_slip": [0, 0, 0, 0, 0, 1, 1, 0, 0, 0],          # persisted hit
            "is_lli": 0,
        })
        mw.to_csv(tmp_path / "mwwl_trace.csv", index=False)
    return str(tmp_path)


def test_builder_counts_flag_driven_reset(tmp_path):
    res = build_ambiguity_locktime(TraceContext(_write_fixture(tmp_path)))
    assert "In-arc resets: 1 total" in res.summary
    assert "slip-driven 1" in res.summary
    # Single-facet strip with many satellite rows: must NOT be sized by the
    # multi-facet default (1.75*1+1.5 = 3.25 in -- too short to read).
    assert all(f.height >= 8.0 for f in res.figures)


def test_builder_without_traces_sees_nothing(tmp_path):
    # No detector traces and no sigma step: the converged-filter blind spot.
    # (This is the pre-fix behaviour; kept as the documented degraded mode.)
    res = build_ambiguity_locktime(
        TraceContext(_write_fixture(tmp_path, with_mw=False)))
    assert "In-arc resets: 0 total" in res.summary


def test_load_slip_events_kinds(tmp_path):
    _write_fixture(tmp_path)
    ev = _load_slip_events(TraceContext(str(tmp_path)))
    assert set(ev.columns) >= {"satellite", "utc", "kind"}
    assert (ev["kind"] == "slip").all() and len(ev) == 2
