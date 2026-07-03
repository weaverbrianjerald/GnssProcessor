"""Network run-provenance manifest viewer (summary-only).

Parses ``network_run_manifest.json`` -- the PPP-RTK NETWORK producer's
run-provenance sidecar -- and surfaces it as a prominent plain-text block in the
report.  Unlike the per-epoch trace CSVs, the manifest is **JSONL**: a small
number of newline-delimited JSON objects, one per line, each ``json.loads``-able
on its own.  Two records are expected:

  * ``startup`` -- written when the network run begins: ``written_utc``,
    ``config_path``, ``config_hash_fnv1a64`` (hex FNV-1a of the config file),
    ``build`` info, and a big ``network`` object of the run's toggles / knobs.
  * ``complete`` -- written when the run finishes: the epoch tallies
    (``n_synced``, ``n_emitted``, ``n_skipped``, ``n_gate_withheld``,
    ``n_sigma0_withheld``) and wall-clock ``loop_ms``.

This is the "what exactly was run, and how did the whole run turn out?" view --
the config hash is the reproducibility fingerprint that ties a report to the
exact config that produced it, and the completion counts are the run-level
accounting behind the per-epoch health traces.

The viewer produces **no figures** -- the report renders its ``summary`` block
verbatim inside a ``<pre>``.  It declares ``required_csvs=()`` (so the driver's
presence check is vacuous) and instead raises :class:`ViewerSkip` itself when the
manifest file is absent, matching the ``network_map`` precedent for
config-resolved / non-CSV inputs.
"""

from __future__ import annotations

import json
import os
from typing import Any, Dict, List, Optional, Tuple

from .base import Viewer, ViewerResult, ViewerSkip

MANIFEST_NAME = "network_run_manifest.json"


# --------------------------------------------------------------------------- #
def _load_records(path: str) -> List[Dict[str, Any]]:
    """Parse a JSONL manifest into a list of dict records.

    Each non-empty line is parsed independently with :func:`json.loads`; blank
    lines are ignored.  A malformed line is skipped rather than aborting the
    whole parse (a partially-flushed manifest from a crashed run still yields the
    records that were fully written).
    """
    records: List[Dict[str, Any]] = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(obj, dict):
                records.append(obj)
    return records


def _classify(records: List[Dict[str, Any]]
              ) -> Tuple[Optional[Dict[str, Any]], Optional[Dict[str, Any]]]:
    """Split records into (startup, complete).

    Prefers an explicit ``record`` discriminator field; falls back to detecting
    the record by its signature keys (``config_hash_fnv1a64`` => startup,
    ``n_synced``/``n_emitted`` => complete).  Later records of the same kind win.
    """
    startup: Optional[Dict[str, Any]] = None
    complete: Optional[Dict[str, Any]] = None
    for rec in records:
        kind = str(rec.get("record", "")).lower()
        is_startup = (kind == "startup"
                      or ("config_hash_fnv1a64" in rec or "config_path" in rec))
        is_complete = (kind == "complete"
                       or ("n_synced" in rec or "n_emitted" in rec
                           or "loop_ms" in rec))
        # An explicit discriminator overrides the heuristic ambiguity.
        if kind == "startup":
            is_complete = False
        elif kind == "complete":
            is_startup = False
        if is_startup:
            startup = rec
        if is_complete:
            complete = rec
    return startup, complete


def _fmt_val(v: Any) -> str:
    """Compact scalar rendering for the key:value block (JSON for containers)."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        # Keep it readable; strip a trailing ".0" for whole numbers.
        s = f"{v:g}"
        return s
    if isinstance(v, (dict, list)):
        return json.dumps(v, sort_keys=True)
    if v is None:
        return "null"
    return str(v)


def _kv_lines(d: Dict[str, Any], indent: int = 2, width: int = 24) -> List[str]:
    """Aligned ``key : value`` lines for a flat-ish dict, sorted by key."""
    pad = " " * indent
    out: List[str] = []
    for k in sorted(d.keys()):
        out.append(f"{pad}{str(k):<{width}} : {_fmt_val(d[k])}")
    return out


# --------------------------------------------------------------------------- #
def build_network_manifest(ctx) -> ViewerResult:
    """Build the summary-only run-manifest view (raises ViewerSkip if absent)."""
    path = os.path.join(ctx.log_dir, MANIFEST_NAME)
    if not os.path.isfile(path):
        raise ViewerSkip(f"{MANIFEST_NAME} not found in {ctx.log_dir}")

    records = _load_records(path)
    if not records:
        raise ViewerSkip(f"{MANIFEST_NAME} present but empty / unparseable")

    startup, complete = _classify(records)
    summary = _summary(startup, complete, n_records=len(records))
    return ViewerResult(figures=[], summary=summary)


def _summary(startup: Optional[Dict[str, Any]],
             complete: Optional[Dict[str, Any]],
             n_records: int) -> str:
    out: List[str] = ["=== PPP-RTK Network Run Manifest ==="]

    # --- Config hash prominently up top (the reproducibility fingerprint). ---
    cfg_hash = None
    if startup is not None:
        cfg_hash = startup.get("config_hash_fnv1a64")
    out.append(f"Config hash (FNV-1a64): {cfg_hash if cfg_hash else '<absent>'}")

    if startup is not None:
        out.append("")
        out.append("--- Startup ---")
        if startup.get("written_utc") is not None:
            out.append(f"  written_utc              : {startup['written_utc']}")
        if startup.get("config_path") is not None:
            out.append(f"  config_path              : {startup['config_path']}")
        build = startup.get("build")
        if isinstance(build, dict):
            compiled = build.get("compiled")
            out.append(f"  build.compiled           : {_fmt_val(compiled)}")
            for bk in sorted(k for k in build.keys() if k != "compiled"):
                out.append(f"  build.{str(bk):<19}: {_fmt_val(build[bk])}")
        elif build is not None:
            out.append(f"  build                    : {_fmt_val(build)}")

        network = startup.get("network")
        if isinstance(network, dict) and network:
            out.append("")
            out.append(f"  Network toggles / knobs ({len(network)}):")
            out.extend(_kv_lines(network, indent=4, width=28))
        elif network is not None:
            out.append(f"  network                  : {_fmt_val(network)}")
    else:
        out.append("")
        out.append("--- Startup ---")
        out.append("  (no startup record in manifest)")

    # --- Completion accounting. ---
    out.append("")
    out.append("--- Complete ---")
    if complete is not None:
        for key in ("n_synced", "n_emitted", "n_skipped",
                    "n_gate_withheld", "n_sigma0_withheld", "loop_ms"):
            if key in complete:
                out.append(f"  {key:<20} : {_fmt_val(complete[key])}")
        # Any extra completion keys not in the canonical list.
        extra = [k for k in complete.keys()
                 if k not in {"record", "n_synced", "n_emitted", "n_skipped",
                              "n_gate_withheld", "n_sigma0_withheld", "loop_ms"}]
        for k in sorted(extra):
            out.append(f"  {str(k):<20} : {_fmt_val(complete[k])}")
    else:
        out.append("  (no complete record — run may not have finished)")

    return "\n".join(out)


viewer = Viewer(name="network_manifest",
                title="Network run manifest (provenance & completion)",
                required_csvs=(), build=build_network_manifest)
