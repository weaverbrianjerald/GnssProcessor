"""The eight exam_* viewers: build counts, auto-skip, graceful degradation."""
import pytest

from gnssviz import TraceContext, run_viewers
from gnssviz.backends import get_backend
from gnssviz.viewers.base import ViewerSkip

# viewer module -> (build fn name, expected figure count on the full fixture)
_VIEWERS = {
    "exam_scorecard": 0,
    "exam_position_truth": 3,
    "exam_truth_lines": 3,
    "exam_track": 1,
    "exam_map": 2,
    "exam_visibility": 1,
    "exam_iono_truth": 3,
    "exam_skyplot_slips": 1,
    "exam_slip_timeline": 1,
}
_EXAM_NAMES = set(_VIEWERS)


def _build(name, ctx):
    import importlib
    mod = importlib.import_module(f"gnssviz.viewers.{name}")
    return getattr(mod, f"build_{name}")(ctx)


@pytest.mark.parametrize("name,n_figs", _VIEWERS.items())
def test_viewer_builds_and_renders(name, n_figs, exam_fixture):
    truth, logs = exam_fixture()
    ctx = TraceContext(logs, truth_dir=truth)
    res = _build(name, ctx)
    assert len(res.figures) == n_figs, f"{name}: {len(res.figures)} != {n_figs}"
    # Every figure renders through matplotlib without exception.
    mpl = get_backend("matplotlib")
    for fig in res.figures:
        r = mpl.render(fig)
        assert mpl.to_png_bytes(r, dpi=70)
        mpl.close(r)
    # Text-only viewers still carry a summary.
    if n_figs == 0:
        assert res.summary


def test_all_exam_viewers_run_with_truth(exam_fixture):
    truth, logs = exam_fixture()
    out = run_viewers(TraceContext(logs, truth_dir=truth))
    ran = {v.name for v, _ in out.ran}
    assert _EXAM_NAMES <= ran, f"missing: {_EXAM_NAMES - ran}"
    assert not [f for f in out.failed if f[0] in _EXAM_NAMES], out.failed


def test_all_exam_viewers_autoskip_without_truth(exam_fixture):
    # Same log dir, but no truth dir bound -> every exam viewer auto-skips (R4).
    _truth, logs = exam_fixture()
    out = run_viewers(TraceContext(logs))            # no truth_dir
    ran = {v.name for v, _ in out.ran}
    skipped = {n for n, _ in out.skipped}
    assert not (_EXAM_NAMES & ran), f"exam viewer ran without truth: {_EXAM_NAMES & ran}"
    assert _EXAM_NAMES <= skipped
    # ...but the ordinary processor viewers still run (report unchanged, A4).
    assert "state" in ran and "measurement_residuals" in ran


# --------------------------------------------------------------------------- #
#  Graceful degradation
# --------------------------------------------------------------------------- #
def test_scorecard_without_verify_report(exam_fixture):
    truth, logs = exam_fixture(with_verify=False)
    res = _build("exam_scorecard", TraceContext(logs, truth_dir=truth))
    assert "unverified" in res.summary.lower()          # verdict labelled unverified
    assert "ABSENT" in res.summary


def test_scorecard_with_verify_report_shows_gates(exam_fixture):
    truth, logs = exam_fixture(with_verify=True)
    res = _build("exam_scorecard", TraceContext(logs, truth_dir=truth))
    assert "Verifier gates: PASS" in res.summary
    assert "isb" in res.summary and "zwd" in res.summary
    assert "Epoch match" in res.summary


def test_slip_timeline_with_and_without_source(exam_fixture):
    for with_source in (True, False):
        truth, logs = exam_fixture(with_source=with_source)
        res = _build("exam_slip_timeline", TraceContext(logs, truth_dir=truth))
        assert len(res.figures) == 1
        # 'scripted' class only exists via the source column.
        if with_source:
            assert "scripted" in res.summary
        else:
            assert "scripted" not in res.summary.split("by class")[1].split("\n")[0]


def test_truth_lines_gps_only_skips_isb(exam_fixture):
    # No Galileo_m column -> the ISB figure is dropped, ZWD + clock remain.
    truth, logs = exam_fixture(galileo=False)
    res = _build("exam_truth_lines", TraceContext(logs, truth_dir=truth))
    assert len(res.figures) == 2
    assert not any("ISB" in f.name for f in res.figures)


def test_iono_and_skyplot_skip_without_atmo(exam_fixture):
    truth, logs = exam_fixture(atmo=False)
    ctx = TraceContext(logs, truth_dir=truth)
    for name in ("exam_iono_truth", "exam_skyplot_slips"):
        with pytest.raises(ViewerSkip):
            _build(name, ctx)


def test_skyplot_without_slips_still_draws_tracks(exam_fixture):
    truth, logs = exam_fixture(slips=False)
    res = _build("exam_skyplot_slips", TraceContext(logs, truth_dir=truth))
    assert len(res.figures) == 1
    assert "absent" in res.summary.lower()


def test_exam_map_uses_satellite_imagery(exam_fixture):
    # With the vendored asset present, the regional panel draws an RgbImage
    # basemap (not the filled-land Polygon) + an imagery attribution chip.
    from gnssviz import basemap
    from gnssviz.figures import Polygon, RgbImage, TextBox
    if not basemap.has_basemap():
        pytest.skip("vendored basemap asset absent")
    truth, logs = exam_fixture()
    res = _build("exam_map", TraceContext(logs, truth_dir=truth))
    regional = res.figures[1].axes[0].traces
    assert any(isinstance(t, RgbImage) for t in regional)
    assert not any(isinstance(t, Polygon) for t in regional)
    assert any(isinstance(t, TextBox) and "Esri" in t.text for t in regional)


def test_exam_map_falls_back_to_coastline_without_imagery(exam_fixture, monkeypatch):
    # No imagery -> the regional panel degrades to the filled land/ocean basemap
    # (or a graticule), and never an RgbImage.  World panel is unaffected.
    import gnssviz.viewers.exam_map as em
    from gnssviz.figures import Polygon, RgbImage
    monkeypatch.setattr(em, "load_basemap", lambda: None)
    truth, logs = exam_fixture()
    res = _build("exam_map", TraceContext(logs, truth_dir=truth))
    assert len(res.figures) == 2
    regional = res.figures[1].axes[0].traces
    assert not any(isinstance(t, RgbImage) for t in regional)
    assert any(isinstance(t, Polygon) for t in regional)      # coastline fill present
    # Still renders through matplotlib.
    mpl = get_backend("matplotlib")
    r = mpl.render(res.figures[1])
    assert mpl.to_png_bytes(r, dpi=60)
    mpl.close(r)


def test_static_track_branch(exam_fixture):
    truth, logs = exam_fixture(kinematic=False)
    res = _build("exam_track", TraceContext(logs, truth_dir=truth))
    assert len(res.figures) == 1
    assert "Static" in res.figures[0].axes[0].title


def test_slip_timeline_burst_label_no_signal_column():
    # Regression: on the no-'signal' path a single-key groupby yields 1-tuples in
    # pandas 2.2+, which must not leak into the burst label as "('G12',)".
    import pandas as pd
    from gnssviz.io import add_utc
    from gnssviz.viewers.exam_slip_timeline import _find_bursts
    df = pd.DataFrame({"week": [2313] * 3, "tow_s": [302400.0, 302430.0, 302460.0],
                       "satellite": ["G12"] * 3, "delta_cycles": [-5, -5, -5],
                       "flagged": [1, 1, 1]})                      # no 'signal' column
    df = add_utc(df)
    bursts, available = _find_bursts(df, 30.0)
    assert available and len(bursts) == 1
    assert bursts[0]["sat"] == "G12"                               # not "('G12',)"
