"""Report assembly: offline inline plotly, aspect_equal, and the exam CLI."""
import math

import numpy as np

from gnssviz.backends import get_backend
from gnssviz.figures import Axes, Figure, Line
from gnssviz.report import build_html_report
from gnssviz.viewers.base import ViewerResult


# --------------------------------------------------------------------------- #
#  Offline interactive mode (SPEC §4.5 / R2)
# --------------------------------------------------------------------------- #
def _one_section():
    ax = Axes(traces=[Line(x=[1, 2, 3], y=[1.0, 2.0, 3.0])], x_is_datetime=False)
    return [("Demo", ViewerResult(figures=[Figure(name="d", axes=[ax])]))]


def test_plotly_inline_is_offline():
    html = build_html_report(_one_section(), get_backend("plotly"), plotly_js="inline")
    assert "cdn.plot.ly" not in html                     # no CDN substring at all
    assert "text/javascript" in html                     # runtime embedded inline
    assert "inline (offline)" in html                    # meta note surfaced


def test_plotly_cdn_default_uses_cdn():
    html = build_html_report(_one_section(), get_backend("plotly"), plotly_js="cdn")
    assert "cdn.plot.ly" in html                          # CDN script tag present


def test_matplotlib_report_has_no_js():
    html = build_html_report(_one_section(), get_backend("matplotlib"))
    assert "plot.ly" not in html and "cdn.plot" not in html


# --------------------------------------------------------------------------- #
#  aspect_equal primitive renders in both backends (SPEC §4.3)
# --------------------------------------------------------------------------- #
def _aspect_fig():
    ax = Axes(traces=[Line(x=[0.0, 1.0, 2.0], y=[0.0, 1.0, 0.5])],
              x_is_datetime=False, aspect_equal=True)
    return Figure(name="track", axes=[ax], width=5, height=5)


def test_aspect_equal_matplotlib():
    mpl = get_backend("matplotlib")
    r = mpl.render(_aspect_fig())
    try:
        assert r.axes[0].get_aspect() == 1.0             # 'equal' -> ratio 1
        assert mpl.to_png_bytes(r, dpi=70)
    finally:
        mpl.close(r)


def test_aspect_equal_plotly():
    r = get_backend("plotly").render(_aspect_fig())
    lay = r.layout
    assert lay.yaxis.scaleanchor == "x"                  # y locked to x scale
    assert lay.yaxis.scaleratio == 1.0


def _rgb_image_fig(**kw):
    from gnssviz.figures import RgbImage
    rgb = np.zeros((4, 6, 3), np.uint8)
    rgb[0, 0] = (255, 0, 0)                  # top-left red -> north-west corner
    ax = Axes(traces=[RgbImage(rgb=rgb, extent=(176.0, 185.0, -23.0, -12.0),
                               zorder=0, **kw)],
              x_is_datetime=False, aspect_equal=True,
              xlim=(176.0, 185.0), ylim=(-23.0, -12.0))
    return Figure(name="basemap", axes=[ax], width=5, height=5)


def test_rgb_image_matplotlib():
    mpl = get_backend("matplotlib")
    r = mpl.render(_rgb_image_fig())
    try:
        assert r.axes[0].images                          # an AxesImage was added
        im = r.axes[0].images[0]
        assert im.origin == "upper"                      # row 0 = north (top)
        # Georeferenced at the data extent (must match the plotly placement).
        assert tuple(float(v) for v in im.get_extent()) == (176.0, 185.0, -23.0, -12.0)
        assert mpl.to_png_bytes(r, dpi=60)
    finally:
        mpl.close(r)


def test_rgb_image_plotly_is_offline_data_uri():
    # No pre-encoded bytes -> stdlib PNG re-encode, embedded as a data URI.
    r = get_backend("plotly").render(_rgb_image_fig())
    imgs = r.layout.images
    assert len(imgs) == 1
    src = imgs[0].source
    assert src.startswith("data:image/png;base64,")       # offline, no URL
    assert imgs[0].layer == "below"                        # basemap under traces
    assert imgs[0].sizing == "stretch"
    assert float(imgs[0].x) == 176.0 and float(imgs[0].y) == -12.0   # top-left anchor
    # Size spans the full extent (must match matplotlib's get_extent georeferencing).
    assert float(imgs[0].sizex) == 9.0 and float(imgs[0].sizey) == 11.0


def test_rgb_image_plotly_embeds_encoded_bytes_verbatim():
    # A pre-encoded JPEG is embedded as-is (small HTML), not re-encoded to PNG.
    r = get_backend("plotly").render(_rgb_image_fig(encoded=b"\xff\xd8JPEGDATA",
                                                    mime="image/jpeg"))
    src = r.layout.images[0].source
    assert src.startswith("data:image/jpeg;base64,")
    import base64
    assert base64.b64decode(src.split(",", 1)[1]) == b"\xff\xd8JPEGDATA"


def test_polygon_and_facecolor_both_backends():
    # The basemap primitives: a filled Polygon (land) on a facecolor axes (ocean).
    from gnssviz.figures import Polygon
    ax = Axes(traces=[Polygon(x=[0.0, 1.0, 1.0, 0.0], y=[0.0, 0.0, 1.0, 1.0],
                              fill=(0.9, 0.9, 0.8), edge=(0.4, 0.4, 0.4))],
              x_is_datetime=False, aspect_equal=True, facecolor=(0.8, 0.9, 0.96))
    fig = Figure(name="map", axes=[ax], width=4, height=4)
    mpl = get_backend("matplotlib")
    r = mpl.render(fig)
    try:
        assert mpl.to_png_bytes(r, dpi=60)
    finally:
        mpl.close(r)
    rp = get_backend("plotly").render(fig)
    assert rp.layout.plot_bgcolor is not None                 # facecolor -> plot bg
    assert any(getattr(t, "fill", None) == "toself" for t in rp.data)  # land filled


# --------------------------------------------------------------------------- #
#  run_report.py exam CLI end-to-end
# --------------------------------------------------------------------------- #
def test_cli_exam_report_has_sections_and_banner(exam_fixture, tmp_path):
    import run_report
    truth, logs = exam_fixture()
    out = tmp_path / "dashboard.html"
    rc = run_report.main([logs, "--truth-dir", truth, "-o", str(out),
                          "--backend", "matplotlib"])
    assert rc == 0
    html = out.read_text(encoding="utf-8")
    assert "GnssSimulator exam run report" in html       # title flipped (§4.1)
    assert "Exam scorecard" in html                      # exam section present
    assert "Position error vs truth" in html
    assert "banner pass" in html and "VERIFIER PASS" in html   # banner (§4.4)


def test_plotly_log_axis_limits_are_exponents(tmp_path):
    # Plotly expresses a log axis's `range` in exponents; matplotlib uses data units.
    # Viewers speak data units, so the backend must convert: passing 35 straight
    # through renders the axis top at 10^35 (observed as a 1e32 y-limit on the
    # station-keeping panel, whose data tops out near 10 m).
    from gnssviz.backends import get_backend
    from gnssviz.figures import Axes, Figure, Line
    ax = Axes(title="log", xlabel="x", ylabel="y", yscale="log", ylim=(0.2, 35.0),
              x_is_datetime=False,
              traces=[Line(x=[0, 1, 2], y=[0.5, 5.0, 20.0])])
    gofig = get_backend("plotly").render(Figure(name="log", axes=[ax], width=6, height=3))
    rng = gofig.layout.yaxis.range
    assert rng is not None
    # log10(0.2) = -0.699, log10(35) = 1.544, not the raw 0.2/35.
    assert abs(rng[0] - math.log10(0.2)) < 1e-9, rng
    assert abs(rng[1] - math.log10(35.0)) < 1e-9, rng


def test_plotly_linear_axis_limits_are_untouched(tmp_path):
    from gnssviz.backends import get_backend
    from gnssviz.figures import Axes, Figure, Line
    ax = Axes(title="lin", yscale="linear", ylim=(-4.0, 9.0), x_is_datetime=False,
              traces=[Line(x=[0, 1], y=[0.0, 1.0])])
    gofig = get_backend("plotly").render(Figure(name="lin", axes=[ax], width=6, height=3))
    assert list(gofig.layout.yaxis.range) == [-4.0, 9.0]


def test_exam_dashboard_drops_the_crowding_viewers(exam_fixture, tmp_path):
    # Dashboard composition: a fixed set of viewers that duplicate a panel above
    # them, or are internal QC strips, is off by default on the exam dashboard
    # only.
    import run_report
    truth, logs = exam_fixture()
    out = tmp_path / "dashboard.html"
    assert run_report.main([logs, "--truth-dir", truth, "-o", str(out),
                            "--backend", "matplotlib"]) == 0
    html = out.read_text(encoding="utf-8")
    for title in ("State / position", "Measurement residuals by signal",
                  "Plan-view track vs truth", "Ocean tidal loading",
                  "DOPs &amp; satellite counts", "Float ambiguities",
                  "Slip-detector gate strip", "Satellite signal-usage strip"):
        assert title not in html, f"{title!r} should be off by default"
    # The panels they were duplicating are still there.
    assert "Position error vs truth" in html and "Satellite visibility" in html


def test_exam_dashboard_off_list_is_overridable(exam_fixture, tmp_path):
    # --only names a viewer explicitly; --all opts everything back in. Neither
    # may silently lose to the default off-list.
    import run_report
    truth, logs = exam_fixture()
    only = tmp_path / "only.html"
    assert run_report.main([logs, "--truth-dir", truth, "-o", str(only),
                            "--backend", "matplotlib", "--only", "state"]) == 0
    assert "State / position" in only.read_text(encoding="utf-8")

    every = tmp_path / "all.html"
    assert run_report.main([logs, "--truth-dir", truth, "-o", str(every),
                            "--backend", "matplotlib", "--all"]) == 0
    assert "Plan-view track vs truth" in every.read_text(encoding="utf-8")


def test_plain_report_keeps_the_general_purpose_viewers(exam_fixture, tmp_path):
    # The off-list is an EXAM-dashboard composition choice; the ordinary report
    # must be unaffected.
    import run_report
    _truth, logs = exam_fixture()
    out = tmp_path / "plain.html"
    assert run_report.main([logs, "-o", str(out), "--backend", "matplotlib"]) == 0
    assert "State / position" in out.read_text(encoding="utf-8")


def test_cli_without_truth_skips_exam_viewers(exam_fixture, tmp_path):
    import run_report
    _truth, logs = exam_fixture()
    out = tmp_path / "plain.html"
    rc = run_report.main([logs, "-o", str(out), "--backend", "matplotlib"])
    assert rc == 0
    html = out.read_text(encoding="utf-8")
    assert "GnssProcessor run report" in html            # default title
    assert "GnssSimulator exam run report" not in html
    # exam viewers appear under "Skipped viewers", not as sections.
    assert "Skipped viewers" in html
    assert "exam_scorecard" in html                      # listed as skipped


def test_cli_inline_plotly_offline(exam_fixture, tmp_path):
    import run_report
    truth, logs = exam_fixture()
    out = tmp_path / "dash_plotly.html"
    rc = run_report.main([logs, "--truth-dir", truth, "-o", str(out),
                          "--backend", "plotly", "--plotly-js", "inline"])
    assert rc == 0
    assert "cdn.plot.ly" not in out.read_text(encoding="utf-8")
