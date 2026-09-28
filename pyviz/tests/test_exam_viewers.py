"""The exam_* viewers: build counts, auto-skip, graceful degradation."""
import pytest

from gnssviz import TraceContext, run_viewers
from gnssviz.backends import get_backend
from gnssviz.figures import TextBox
from gnssviz.viewers.base import ViewerSkip

# viewer module -> (build fn name, expected figure count on the full fixture)
_VIEWERS = {
    "exam_scorecard": 0,
    "exam_spp_reference": 6,      # 7 minus the opt-in A/B ablation figure
    "exam_static_window": 2,
    "exam_position_truth": 3,
    "exam_truth_lines": 3,
    "exam_track": 3,
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
    from gnssviz.viewers.exam_map import _C_LAND
    regional = res.figures[1].axes[0].traces
    assert any(isinstance(t, RgbImage) for t in regional)
    # No land-fill polygon: the imagery replaces the coastline basemap.  (The
    # error-region polygon is a separate feature that may also be present, so
    # this checks the fill colour rather than the type alone.)
    assert not any(isinstance(t, Polygon) and tuple(t.fill) == tuple(_C_LAND)
                   for t in regional)
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


def test_track_error_bounds_need_the_off_diagonals(exam_fixture):
    # The trajectory corridor and per-epoch ellipses come from the full covariance.
    # Without cov_xy/xz/yz an orientation cannot be recovered, and rotating a
    # diagonal-only ECEF covariance would manufacture a bearing that is an
    # artifact of the rotation, so the panels must be dropped, not faked.
    truth, logs = exam_fixture(state_cov=True)
    res = _build("exam_track", TraceContext(logs, truth_dir=truth))
    names = [f.name for f in res.figures]
    assert "Track error bounds" in names and "Track-relative error bounds" in names

    truth2, logs2 = exam_fixture(state_cov=False)
    res2 = _build("exam_track", TraceContext(logs2, truth_dir=truth2))
    assert [f.name for f in res2.figures] == ["Plan-view track"]


def test_exam_map_error_region_needs_the_off_diagonals(exam_fixture):
    # The filled 95% region on the imagery panel comes from the full covariance;
    # without cov_xy/xz/yz it must be omitted (an orientation cannot be recovered
    # from variances alone), leaving the panel as the plain context map it was.
    from gnssviz.figures import Polygon as _Poly
    from gnssviz.viewers.exam_map import _C_REGION

    def region_of(**kw):
        truth, logs = exam_fixture(**kw)
        res = _build("exam_map", TraceContext(logs, truth_dir=truth))
        return [t for t in res.figures[1].axes[0].traces
                if isinstance(t, _Poly) and tuple(t.fill) == tuple(_C_REGION)]

    got = region_of(state_cov=True)
    assert len(got) == 1, "error region missing with full covariance"
    assert got[0].label and "95%" in got[0].label and "×" in got[0].label
    assert not region_of(state_cov=False), "region drawn without the off-diagonals"


def test_exam_map_zoom_is_proportional_to_the_track(exam_fixture):
    # The regional panel used a fixed 3-degree pad (~333 km), which framed the
    # track inside a box several times its own size.  The window must now scale
    # with the track so the trajectory stays the subject of the panel.
    truth, logs = exam_fixture()
    res = _build("exam_map", TraceContext(logs, truth_dir=truth))
    ax = res.figures[1].axes[0]
    w = ax.xlim[1] - ax.xlim[0]
    h = ax.ylim[1] - ax.ylim[0]
    assert w < 1.0 and h < 1.0, (ax.xlim, ax.ylim)   # fixture track is tiny


def test_whole_arc_storm_suppresses_the_spatial_overlay(exam_fixture):
    # A storm covering the whole record is recoloured over every epoch, in a line
    # heavier than the track beneath it, so the overlay hides the trajectory it
    # annotates and advertises a legend colour with nothing to contrast against.
    # The spatial viewers must drop it and say so in prose instead.
    from gnssviz.figures import Line as _Line, TextBox as _TextBox
    from gnssviz.viewers import exam_common as _ec

    assert _ec.storm_overlay_is_informative([True, False, True])       # partial: draw
    assert not _ec.storm_overlay_is_informative([True] * 20)           # whole arc: skip
    assert not _ec.storm_overlay_is_informative([])                    # nothing to draw
    assert not _ec.storm_overlay_is_informative([False, False])

    def traces_of(name, **kw):
        truth, logs = exam_fixture(**kw)
        res = _build(name, TraceContext(logs, truth_dir=truth))
        idx = 1 if name == "exam_map" else 0
        return res.figures[idx].axes[0].traces

    # The fixture's storm covers the middle third -> informative, overlay drawn.
    for name, lbl in (("exam_track", "storm"), ("exam_map", "storm window")):
        tr = traces_of(name)
        assert any(isinstance(t, _Line) and t.label == lbl for t in tr), name
        assert not any(isinstance(t, _TextBox) and "entire arc" in t.text for t in tr), name

    # storm_whole_arc mirrors the hero (100% of epochs) -> overlay suppressed.
    for name, lbl in (("exam_track", "storm"), ("exam_map", "storm window")):
        tr = traces_of(name, storm_whole_arc=True)
        assert not any(isinstance(t, _Line) and t.label == lbl for t in tr), name
        assert any(isinstance(t, _TextBox) and "entire arc" in t.text for t in tr), name


def test_static_track_branch(exam_fixture):
    truth, logs = exam_fixture(kinematic=False)
    res = _build("exam_track", TraceContext(logs, truth_dir=truth))
    assert len(res.figures) == 1
    assert "Static" in res.figures[0].axes[0].title


# --------------------------------------------------------------------------- #
#  Part-B SPP reference (truth-only viewer)
# --------------------------------------------------------------------------- #
def _textboxes(res):
    return [t.text for f in res.figures for a in f.axes for t in a.traces
            if isinstance(t, TextBox)]


def test_spp_reference_stats_come_from_the_csv(exam_fixture):
    # The grading bands are keyed to truth_spp.csv, so the read-out must be
    # computed from it -- the ablation means especially.
    truth, logs = exam_fixture()
    res = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    assert "Klobuchar" in res.summary and "+10.75" in res.summary
    assert "-1.46" in res.summary and "-0.49" in res.summary and "+0.37" in res.summary
    # Leg 1 is vertical-dominated, leg 2 is not (the Part-B teaching point).
    assert "signed Up median" in res.summary
    assert "1441" not in res.summary          # no numbers leaked from the real hero run


def test_spp_reference_survives_missing_summary_json(exam_fixture):
    # truth_spp.csv alone must still draw every panel; only the model ledger goes.
    truth, logs = exam_fixture(spp_summary=False)
    res = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    assert len(res.figures) == 6
    assert "truth_spp_summary.json absent" in res.summary
    assert "Estimator" not in res.summary


def test_spp_reference_sigma_panel_needs_the_sigma_columns(exam_fixture):
    # The formal-sigma panel is the Part-B stochastic-model reference; it must
    # appear when truth_spp.csv carries the sigma columns and be dropped (not
    # faked) when an older file does not.
    truth, logs = exam_fixture()
    res = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    assert any(f.name == "Formal sigma vs error" for f in res.figures)
    assert "err/σ3D" in res.summary
    # Leg 1's covariance is optimistic (ratio >> 1), leg 2's matches its errors (~1).
    rows = [l for l in res.summary.split("\n") if l.strip().startswith(("leg 1", "leg 2"))]
    assert rows, res.summary

    import csv as _csv
    import os
    p = os.path.join(truth, "truth_spp.csv")
    with open(p, newline="", encoding="utf-8") as fh:
        rd = list(_csv.reader(fh))
    keep = [i for i, c in enumerate(rd[0]) if "_sig" not in c]
    with open(p, "w", newline="", encoding="utf-8") as fh:
        _csv.writer(fh, lineterminator="\n").writerows([[r[i] for i in keep] for r in rd])
    res2 = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    assert not any(f.name == "Formal sigma vs error" for f in res2.figures)
    assert len(res2.figures) == 5
    assert "err/σ3D" not in res2.summary


def test_static_window_needs_a_disclosed_window(exam_fixture):
    # The viewer keys off scenario_summary.static_segment; an instance with no
    # station-keeping prefix has nothing to draw and must skip rather than fail.
    import json
    import os
    truth, logs = exam_fixture()
    p = os.path.join(truth, "scenario_summary.json")
    with open(p, encoding="utf-8") as fh:
        summ = json.load(fh)
    summ.pop("static_segment")
    with open(p, "w", encoding="utf-8") as fh:
        json.dump(summ, fh)
    with pytest.raises(ViewerSkip):
        _build("exam_static_window", TraceContext(logs, truth_dir=truth))


def test_static_window_separates_bias_from_noise(exam_fixture):
    # A static solve's error is the mean of the per-epoch errors (window truth is
    # one point); the panel reports that mean next to the spread about it, so a
    # bias-limited leg is visibly not helped by averaging.
    truth, logs = exam_fixture()
    res = _build("exam_static_window", TraceContext(logs, truth_dir=truth))
    assert [f.name for f in res.figures] == ["Static window: per-epoch vs static fix",
                                             "Static window: horizontal scatter"]
    assert "static 3D" in res.summary and "scatter" in res.summary
    # Both SPP legs and the PPP trace are collapsed over the window.
    for lbl in ("leg 1", "leg 2", "PPP"):
        assert lbl in res.summary
    # The PPP row must carry its caveat: it is a kinematic mean, not a static solve.
    # (Match a fragment that cannot straddle the read-out's line wrapping.)
    assert "KINEMATIC filter's window mean" in res.summary
    # Per-epoch panel is log-scaled (three solutions spanning decades)...
    assert res.figures[0].axes[0].yscale == "log"
    # ...with a linear companion so the window's convergence is not
    # log-compressed; leg 1 (decades above the rest) is dropped from it.
    axl = res.figures[0].axes[1]
    assert axl.yscale == "linear"
    labels = [t.label for t in axl.traces if getattr(t, "label", None)]
    assert any("PPP" in lbl for lbl in labels)
    assert not any("leg 1" in lbl for lbl in labels)
    # The dotted static-fix overlays are removed: they read as data series and
    # caused confusion.  Static values live in the read-out box, the summary
    # table, and the scatter-panel stars.
    from gnssviz.figures import HLine
    for ax in (res.figures[0].axes[0], axl):
        assert not any(isinstance(t, HLine) for t in ax.traces)
    # The linear panel is scaled near the static fixes, not the first
    # convergence epochs.
    import math
    assert axl.ylim is not None and math.isfinite(axl.ylim[1])


def test_static_window_truly_static_ppp_row(exam_fixture):
    # A state_trace_static.csv beside the main trace (the window rerun with
    # filter.process_noise.position_q = 0) becomes a fourth solution whose fix
    # is the final epoch's error: a true fixed point, not a window mean.
    import os
    import shutil
    truth, logs = exam_fixture()
    shutil.copyfile(os.path.join(logs, "state_trace.csv"),
                    os.path.join(logs, "state_trace_static.csv"))
    res = _build("exam_static_window", TraceContext(logs, truth_dir=truth))
    assert "PPP-static" in res.summary and "FINAL epoch" in res.summary
    labels = [t.label for f in res.figures for ax in f.axes
              for t in ax.traces if getattr(t, "label", None)]
    assert any("PPP-static (true fixed point) per epoch" in lbl for lbl in labels)


def test_spp_reference_sigma_panel_is_a_correlated_scatter(exam_fixture):
    # The panel is a per-leg scatter of realised error against formal sigma on
    # log-log axes (not two time series), carrying a 1:1 reference and each
    # leg's correlation.
    from gnssviz.figures import Scatter
    truth, logs = exam_fixture()
    res = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    fig = next(f for f in res.figures if f.name == "Formal sigma vs error")
    ax = fig.axes[0]
    assert (ax.xscale, ax.yscale) == ("log", "log")
    assert ax.x_is_datetime is False
    pts = [t for t in ax.traces if isinstance(t, Scatter)]
    assert len(pts) == 2, "one cloud per leg"
    # Every cloud is labelled with its own correlation, and the 1:1 line is drawn.
    assert all("r_log" in (t.label or "") and "ρ" in (t.label or "") for t in pts)
    assert any("1:1" in (getattr(t, "label", "") or "") for t in ax.traces)


def test_spp_reference_crowded_panels_are_opt_in(exam_fixture):
    # A/B ablations and the leg1−leg2 clock difference are off by default
    # (dashboard de-crowding) but must come back when asked for, and their
    # numbers must survive in the read-out either way.
    from gnssviz.viewers.exam_spp_reference import build_exam_spp_reference as B
    truth, logs = exam_fixture()
    ctx = TraceContext(logs, truth_dir=truth)

    off = B(ctx)
    assert not any(f.name == "A/B ablations" for f in off.figures)
    assert "Paired A/B ablations" in off.summary          # table kept
    assert "leg 1 − leg 2 c·dt median" in off.summary     # median kept
    clock = next(f for f in off.figures if f.name == "Receiver clock")
    # The panel de-trends per leg (the simulated truth clock drifts, so a median-only
    # baseline leaves a km-scale ramp that swamps the ~20 m of real structure).
    assert len(clock.axes) == 1 and "linear trend removed" in clock.axes[0].title

    on = B(ctx, show_ablations=True, show_clock_difference=True)
    assert any(f.name == "A/B ablations" for f in on.figures)
    assert len(on.figures) == len(off.figures) + 1
    clock_on = next(f for f in on.figures if f.name == "Receiver clock")
    assert len(clock_on.axes) == 2


def test_spp_reference_flags_a_stale_summary(exam_fixture, tmp_path):
    # A summary written from a different (e.g. half-finished) CSV must be called
    # out rather than silently quoted next to contradicting panels.
    import json
    import os
    truth, logs = exam_fixture()
    p = os.path.join(truth, "truth_spp_summary.json")
    with open(p, encoding="utf-8") as fh:
        js = json.load(fh)
    js["n_epochs"] = 1441                      # CSV holds 6
    with open(p, "w", encoding="utf-8") as fh:
        json.dump(js, fh)
    res = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    assert "STALE" in res.summary


def test_spp_reference_excludes_partial_hour_from_the_ladder(exam_fixture):
    # A stub bucket (arc ending mid-hour) is a 1-sample "hourly mean"; quoting it
    # in the range turned leg 2's flat ladder into a spurious 2x spread.
    import os
    truth, logs = exam_fixture()
    rows = ["week,tow_s,l1_n,l1_pdop,l1_cdt_m,l1_e_m,l1_n_m,l1_u_m,l1_3d_m,"
            "l1_noklob_3d_m,l1_notgd_3d_m,l1_prec_3d_m,"
            "if_n,if_pdop,if_cdt_m,if_e_m,if_n_m,if_u_m,if_3d_m,if_noosb_3d_m"]
    # 120 epochs in hour 1, then one epoch that lands in hour 2 carrying a wild value.
    for i in range(121):
        tow = 3600.0 + i * 30.0
        v = 3.0 if i < 120 else 50.0
        rows.append(f"2313,{tow:.3f},9,1.4,29995.5,-1.5,0.8,{v:.4f},{v:.4f},"
                    f"{v:.4f},{v:.4f},{v:.4f},10,1.3,29974.4,-0.7,-1.2,0.9,{v:.4f},{v:.4f}")
    with open(os.path.join(truth, "truth_spp.csv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(rows) + "\n")
    res = _build("exam_spp_reference", TraceContext(logs, truth_dir=truth))
    ladder = [t for t in _textboxes(res) if "→" in t]
    assert ladder, "hourly ladder textbox missing"
    assert "partial — excluded" in ladder[0]
    assert "50.0" not in ladder[0]             # the stub hour never sets the range


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
