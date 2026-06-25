"""End-to-end smoke test against the real trace folder (skips if absent).

Exercises the full path: driver -> viewers -> matplotlib backend -> PNG bytes,
and asserts the HTML report assembles. Numerical fidelity vs MATLAB is verified
by eye on the rendered figures; this guards against import/shape/regression breaks.
"""
import os

import pytest

from gnssviz import TraceContext, run_viewers
from gnssviz.backends import get_backend
from gnssviz.report import build_html_report
from gnssviz.trace_paths import default_log_dir

_LOG_DIR = default_log_dir()
_has_logs = os.path.isdir(_LOG_DIR) and any(
    os.path.getsize(os.path.join(_LOG_DIR, f)) > 0
    for f in ("state_trace.csv", "dop_trace.csv", "satellite_status_trace.csv")
    if os.path.isfile(os.path.join(_LOG_DIR, f))
)

pytestmark = pytest.mark.skipif(not _has_logs, reason=f"no trace CSVs in {_LOG_DIR}")


def test_pipeline_renders_and_reports():
    ctx = TraceContext(_LOG_DIR)
    out = run_viewers(ctx)
    assert not out.failed, f"viewers failed: {out.failed}"
    assert out.ran, "no viewers ran (no matching CSVs?)"

    backend = get_backend("matplotlib")
    for _viewer, res in out.ran:
        for fig in res.figures:
            rendered = backend.render(fig)
            png = backend.to_png_bytes(rendered)
            backend.close(rendered)
            assert png[:8] == b"\x89PNG\r\n\x1a\n"        # valid PNG header

    html = build_html_report([(v.title, r) for (v, r) in out.ran], backend,
                             log_dir=ctx.log_dir, skipped=out.skipped)
    assert "<html>" in html and "data:image/png;base64," in html
