"""Build-time tests for the network run-manifest viewer (L15, pyviz half).

The manifest is JSONL (one JSON object per line), not a trace CSV, so these
fixtures write the two records as newline-delimited JSON.  Figures are never
produced (summary-only viewer); the tests assert the summary content and the
:class:`ViewerSkip` paths.  Backend-free / no rendering, like the other network
viewer tests.
"""
import json

import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_manifest import (MANIFEST_NAME,
                                              build_network_manifest)

_CONFIG_HASH = "a1b2c3d4e5f60789"


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _startup_record():
    return {
        "record": "startup",
        "written_utc": "2026-07-02T12:00:00Z",
        "config_path": "C:/GnssProcessor/config/net.json",
        "config_hash_fnv1a64": _CONFIG_HASH,
        "build": {"compiled": "2026-07-01T22:14:03", "type": "Release"},
        "network": {
            "estimate_glonass": True,
            "clock_model": "allan2",
            "sigma0_gate": 4.0,
            "estimate_code_bias": False,
        },
    }


def _complete_record():
    return {
        "record": "complete",
        "n_synced": 120,
        "n_emitted": 111,
        "n_skipped": 3,
        "n_gate_withheld": 4,
        "n_sigma0_withheld": 2,
        "loop_ms": 48213.5,
    }


def _write_manifest(tmp_path, records):
    text = "\n".join(json.dumps(r) for r in records) + "\n"
    (tmp_path / MANIFEST_NAME).write_text(text, encoding="utf-8")


def test_manifest_both_records(tmp_path):
    _write_manifest(tmp_path, [_startup_record(), _complete_record()])
    res = build_network_manifest(_ctx(tmp_path))
    # Summary-only: no figures.
    assert res.figures == []
    # The config hash must appear prominently.
    assert _CONFIG_HASH in res.summary
    # Startup toggles surfaced.
    assert "estimate_glonass" in res.summary
    assert "clock_model" in res.summary
    # Completion counts surfaced.
    assert "n_synced" in res.summary
    assert "n_emitted" in res.summary
    assert "loop_ms" in res.summary


def test_manifest_absent_skips(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_manifest(_ctx(tmp_path))


def test_manifest_startup_only_parses(tmp_path):
    # Only the startup line: still parses, still shows the hash + toggles.
    _write_manifest(tmp_path, [_startup_record()])
    res = build_network_manifest(_ctx(tmp_path))
    assert res.figures == []
    assert _CONFIG_HASH in res.summary
    assert "clock_model" in res.summary
    # No complete record -> the viewer says so rather than crashing.
    assert "no complete record" in res.summary.lower()


def test_manifest_empty_file_skips(tmp_path):
    # Present but empty (no parseable records) -> ViewerSkip, not a crash.
    (tmp_path / MANIFEST_NAME).write_text("\n   \n", encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_network_manifest(_ctx(tmp_path))


def test_manifest_blank_lines_and_bad_json_tolerated(tmp_path):
    # Interleaved blank lines and one malformed line: the good records survive.
    lines = [
        json.dumps(_startup_record()),
        "",
        "{ this is not valid json ",
        json.dumps(_complete_record()),
    ]
    (tmp_path / MANIFEST_NAME).write_text("\n".join(lines) + "\n", encoding="utf-8")
    res = build_network_manifest(_ctx(tmp_path))
    assert _CONFIG_HASH in res.summary
    assert "n_emitted" in res.summary


def test_manifest_viewer_registration_shape():
    from gnssviz.viewers.network_manifest import viewer
    assert viewer.name == "network_manifest"
    assert viewer.required_csvs == ()
    assert viewer.build is build_network_manifest
