"""Real-time dashboard: change-token logic and live HTML assembly."""
import os

import pytest

from gnssviz import TraceContext
from gnssviz.realtime import LiveDashboard
from gnssviz.trace_paths import default_log_dir


def test_data_token_changes_on_modification(tmp_path):
    csv = tmp_path / "state_trace.csv"
    csv.write_text("week,tow_s\n1,2\n")
    dash = LiveDashboard(TraceContext(str(tmp_path)), backend=None)
    t1 = dash.data_token()
    # rewrite with a later mtime
    os.utime(csv, (os.path.getmtime(csv) + 10, os.path.getmtime(csv) + 10))
    assert dash.data_token() != t1


def test_empty_dir_token_is_stable(tmp_path):
    dash = LiveDashboard(TraceContext(str(tmp_path)), backend=None)
    assert dash.data_token() == dash.data_token()


_LOG_DIR = default_log_dir()
_has_dops = os.path.isfile(os.path.join(_LOG_DIR, "dop_trace.csv")) and \
    os.path.getsize(os.path.join(_LOG_DIR, "dop_trace.csv")) > 0


@pytest.mark.skipif(not _has_dops, reason="no dop_trace.csv to render")
def test_live_page_embeds_poller_and_figs():
    from gnssviz.backends import get_backend
    dash = LiveDashboard(TraceContext(_LOG_DIR), get_backend("plotly"),
                         only=["dops"], poll_interval=4.0)
    page = dash.live_page()
    assert "setInterval" in page and "fetch('update'" in page   # in-place poller
    assert "Plotly.react" in page                                # in-place update path
    assert "id='dops__0'" in page                                # stable figure id


@pytest.mark.skipif(not _has_dops, reason="no dop_trace.csv to render")
def test_update_payload_shape_and_per_viewer_cache():
    from gnssviz.backends import get_backend
    dash = LiveDashboard(TraceContext(_LOG_DIR), get_backend("plotly"), only=["dops"])
    u = dash.update_payload()
    assert u["ids"] and u["figs"] and "token" in u
    assert all(set(f) == {"id", "payload"} for f in u["figs"])
    # Unchanged data -> the viewer's section is served from cache (same object).
    s1 = dash._build()[0]
    s2 = dash._build()[0]
    assert s1 is s2
