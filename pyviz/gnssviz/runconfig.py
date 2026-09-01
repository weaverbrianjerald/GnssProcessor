"""Run-config (JSON-with-comments) access for report plumbing.

The processor's config files are nlohmann-style JSON that freely carry ``//``
and ``/* */`` comments (e.g. ``config/sbf_replay_covl145.json``), so a plain
``json.loads`` refuses most of them.  This module gives the report layer a
tolerant, dependency-free reader plus the two lookups it actually needs:

* the **surveyed reference position** (``receiver.reference_position_ecef``) —
  what the :mod:`~gnssviz.viewers.rt_scorecard` viewer grades the solution
  against, and
* the **app mode** (``app_mode == "real_time"``) — what flips the report title.

The comment stripper is a faithful port of ``scripts/gnss_config_chain.py``'s
``strip_json_comments`` (string-aware character scanner — a regex would corrupt
the ``//`` inside path/URL string values).  Deliberately NOT handled here:
config *inheritance* chains (a leg config naming a parent).  Resolving those is
``scripts/gnss_config_chain.py``'s job; this reader sees one file only, and a
caller holding a chained leg should pass the resolved/base config instead.
"""

from __future__ import annotations

import json
import os
from typing import Optional, Tuple


def strip_json_comments(text: str) -> str:
    """Remove ``//`` and ``/* */`` comments, honouring string literals/escapes.

    Comment bodies are dropped but their newlines kept, so a subsequent parse
    error still reports the line number the file actually has.
    """
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:       # escape consumes the next char whole
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")          # keep line numbers honest
                i += 1
            i = min(i + 2, n)
            continue
        out.append(c)
        i += 1
    return "".join(out)


def load_run_config(path: Optional[str]) -> Optional[dict]:
    """Parse one processor config JSON (comments tolerated).

    ``None``-safe: a ``None``/missing/unreadable/unparseable path returns
    ``None`` rather than raising — report plumbing degrades to "no config"
    exactly like a missing trace CSV degrades to a skipped viewer.
    """
    if not path or not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        # Only pay for the scan when the plain parse fails (common case unchanged).
        try:
            return json.loads(strip_json_comments(text))
        except json.JSONDecodeError:
            return None


def ref_ecef_from_config(cfg: Optional[dict]) -> Optional[Tuple[float, float, float]]:
    """``receiver.reference_position_ecef`` as ``(x, y, z)`` metres, or ``None``.

    Returns ``None`` unless all three components are present and finite — a
    partially-filled block must not silently grade against a garbage point.
    """
    if not isinstance(cfg, dict):
        return None
    ref = (cfg.get("receiver") or {}).get("reference_position_ecef")
    if not isinstance(ref, dict):
        return None
    try:
        xyz = tuple(float(ref[k]) for k in ("x", "y", "z"))
    except (KeyError, TypeError, ValueError):
        return None
    if not all(v == v and abs(v) != float("inf") for v in xyz):   # finite check
        return None
    if all(v == 0.0 for v in xyz):                                # unset sentinel
        return None
    return xyz


def is_realtime_config(cfg: Optional[dict]) -> bool:
    """``True`` iff the config declares ``app_mode: "real_time"``."""
    return isinstance(cfg, dict) and str(cfg.get("app_mode", "")).strip().lower() == "real_time"
