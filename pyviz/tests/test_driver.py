"""Driver dispatch: default_enabled gating and the explicit-override paths.

Uses a synthetic registry of zero-CSV viewers (``required_csvs=()`` so the
``has_data`` presence check is a no-op) and a throwaway ``TraceContext`` -- the
default-folder fallback never touches disk for these.
"""
from gnssviz import TraceContext, run_viewers
from gnssviz.viewers.base import Viewer, ViewerResult


def _v(name, *, default_enabled=True):
    return Viewer(name=name, title=name, required_csvs=(),
                  build=lambda ctx: ViewerResult(figures=[], summary=name),
                  default_enabled=default_enabled)


_ON = _v("on_viewer")
_OFF = _v("off_viewer", default_enabled=False)
_REG = [_ON, _OFF]


def _names(out):
    return {v.name for v, _ in out.ran}


def test_default_run_excludes_default_disabled_viewer():
    ctx = TraceContext(None)
    out = run_viewers(ctx, registry=_REG)
    assert _names(out) == {"on_viewer"}
    # The disabled viewer is reported (not silently dropped) with a hint.
    skipped = dict(out.skipped)
    assert "off_viewer" in skipped and "off by default" in skipped["off_viewer"]


def test_explicit_only_overrides_default_disabled():
    ctx = TraceContext(None)
    out = run_viewers(ctx, only=["off_viewer"], registry=_REG)
    assert _names(out) == {"off_viewer"}
    assert not out.skipped          # explicit request -> no "off by default" note


def test_include_disabled_runs_everything():
    ctx = TraceContext(None)
    out = run_viewers(ctx, include_disabled=True, registry=_REG)
    assert _names(out) == {"on_viewer", "off_viewer"}
    assert not out.skipped


def test_residual_strip_is_off_by_default_in_the_real_registry():
    # Guards the user-facing default: the residual strip must not appear in a
    # vanilla report run, but measurement_residuals (raw) must still be enabled.
    import functools

    from gnssviz.viewers import REGISTRY
    by_name = {v.name: v for v in REGISTRY}
    assert by_name["residual_strip"].default_enabled is False
    assert by_name["measurement_residuals"].default_enabled is True

    # ...and its standardized-residual panels are bound off by default.
    build = by_name["measurement_residuals"].build
    assert isinstance(build, functools.partial)
    assert build.keywords.get("show_standardized") is False
