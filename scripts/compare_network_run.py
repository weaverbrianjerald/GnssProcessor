#!/usr/bin/env python3
"""compare_network_run.py

Field-aware byte-identity harness for the ApplicationNetwork.cpp decomposition
series (see docs/network_decomposition_plan.md, INC 0).

The decomposition moves ~7,300 lines of ``runNetwork()`` verbatim across
translation units. Correctness is proven not by unit tests (none reach
``ApplicationNetwork.cpp`` internals) but by proving every increment produces
**byte-identical** output to the frozen pre-refactor baseline on a golden-run
matrix. This tool is that gate.

Design constraints (from the plan, verifier-hardened):

* **NOT vacuously passable.** A golden records its exact artifact set (relative
  paths + expected-non-empty flags + QC data-row counts). ``compare`` FAILS on
  any missing OR extra file, and on any QC trace that the golden had rows in but
  the candidate does not. An empty/degenerate golden fails at ``capture`` time.
* **Field-aware masks, frozen.** The only permitted differences are wall-clock /
  build-stamp fields, masked at column (CSV) or key (JSON) granularity — never by
  excluding a whole artifact. The strongest full-precision artifact (the state
  snapshot) is masked field-level, never file-level. The mask list is discovered
  empirically (``selfcheck`` = run the binary twice, self-compare) and then
  FROZEN. After freeze, a newly *needed* mask mid-series is a red flag that
  behavior changed — not a fixture to update.

The known wall-clock writers (verified against the tree, INC 0) are seeded in
``DEFAULT_MASKSPEC`` below with their source anchors.

Commands
--------
* ``capture  <run_dir> --out <golden_manifest.json>`` — record the golden's
  artifact set, per-file normalized SHA-256, CSV row counts, and manifest
  counters. Asserts non-vacuous.
* ``compare  <golden_dir> <cand_dir>`` — the increment gate. Exit 0 iff the
  normalized artifact sets are identical. Can also take ``--manifest`` instead of
  a golden dir.
* ``selfcheck <dir_a> <dir_b>`` — twice-run self-compare for empirical mask
  discovery. Reports every normalized diff, flagging which are already covered by
  the frozen maskspec. Informational (exit 0 unless ``--strict``).
* ``dump-maskspec [--out spec.json]`` — emit the default frozen maskspec for
  review/editing.

Usage
-----
::

    # INC 0: capture a golden from the pre-refactor binary's run output
    python scripts/compare_network_run.py capture runs/inc0/net305_smoke \\
        --out goldens/net305_smoke.manifest.json

    # INC 0: prove a config is run-to-run deterministic before freezing it
    python scripts/compare_network_run.py selfcheck runs/inc0/net305_smoke_a \\
        runs/inc0/net305_smoke_b

    # INC N: gate an increment against the frozen golden
    python scripts/compare_network_run.py compare goldens/net305_smoke \\
        runs/incN/net305_smoke && echo "byte-identical"

Exit codes: 0 = pass; 1 = a real diff / missing / extra / vacuous; 2 = usage error.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# ----------------------------------------------------------------------------
# Frozen mask specification. Every entry cites the source writer verified at
# INC 0. Extend ONLY via the selfcheck -> review -> freeze workflow; a mask added
# to make a mid-series diff disappear is masking a behavior change.
# ----------------------------------------------------------------------------
DEFAULT_MASKSPEC: dict = {
    # JSON/JSONL artifacts: masked keys are blanked before compare. Dotted paths
    # index into nested objects (e.g. "build.compiled").
    "json_key_masks": {
        # network_run_manifest.json (ApplicationNetwork.cpp:1611/1614/7071)
        "network_run_manifest.json": ["written_utc", "build.compiled", "loop_ms"],
    },
    # CSV artifacts: masked columns (by header name) are blanked before compare.
    "csv_col_masks": {
        # network_run_summary.csv (ApplicationNetwork.cpp:7243, steady_clock)
        "network_run_summary.csv": ["loop_ms", "ms_per_epoch"],
        # network_service_health.csv (ApplicationNetwork.cpp:2437-2438)
        "network_service_health.csv": ["ms_per_epoch_window", "last_emit_age_s"],
    },
    # State snapshot file (path is config-driven: network.state_snapshot_path).
    # Matched by basename substring; M-line wall_utc masked field-level
    # (NetworkStateSnapshot.cpp:118-120; ApplicationNetwork.cpp:6939).
    "snapshot_name_hint": "snapshot",
    "snapshot_json_key_masks": ["wall_utc"],
    # Product file (network.product_output_path): G-line generation_time is a
    # wall-clock stamp ONLY under network.emit_generation_time
    # (NetworkProductIO.cpp:437-439; ApplicationNetwork.cpp:5151). Matched by
    # suffix; the generation_time token on a "G " record line is canonicalized.
    "product_name_hints": [".ssr", ".rtcm", ".prod", "product"],
    "product_genline_prefixes": ["G "],
    # spdlog text logs (LogManager.cpp:363 pattern "[%E] ..."): the leading
    # bracketed timestamp is canonicalized. run.log is the stdout capture; the
    # golden run-wrapper may already dash-mask it — the same regex normalizes both.
    "log_ts_regex": r"^\[[^\]]*\]",
    "log_ts_token": "[TS]",
    # .log content is bulky and, under parallel station regions, can carry
    # nondeterministic line ordering from shared module loggers. Default: logs are
    # SET-checked (presence/absence) but not content-compared. --include-logs turns
    # on ts-masked content compare (use on serial goldens only).
    "log_content_default_off": True,
    # run.log (stdout capture) IS content-compared by default, ts-masked, plus any
    # timing summary lines matching these regexes are dropped.
    "runlog_name": "run.log",
    # Whole-line drops: lines that are purely wall-clock timing summaries.
    "runlog_drop_regexes": [
        r"loop_ms",
        r"\bms/epoch\b",
        r"elapsed",
    ],
    # Field-level: canonicalize decimal-seconds duration tokens ("... in 0.718 s")
    # in run.log while leaving the rest of the line (station counts, etc.) intact,
    # so ingestion timing varies freely but a real content drift still trips.
    "runlog_dur_regex": r"\d+\.\d+\s*s\b",
    "runlog_dur_token": "<T>s",
    # wall.txt holds the process exit code ("exit=N"); compared verbatim.
}

LOG_SUFFIX = ".log"
CSV_SUFFIX = ".csv"


# ----------------------------------------------------------------------------
# Normalization
# ----------------------------------------------------------------------------
def _read_lines(path: str) -> List[str]:
    with io.open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        text = fh.read()
    # Canonicalize line endings so CRLF/LF differences never register as diffs.
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def _del_dotted(obj: dict, dotted: str) -> None:
    parts = dotted.split(".")
    cur = obj
    for p in parts[:-1]:
        if not isinstance(cur, dict) or p not in cur:
            return
        cur = cur[p]
    if isinstance(cur, dict):
        cur.pop(parts[-1], None)


def _normalize_jsonl(lines: List[str], key_masks: List[str]) -> List[str]:
    out = []
    for ln in lines:
        s = ln.strip()
        if not s:
            out.append("")
            continue
        try:
            obj = json.loads(s)
        except json.JSONDecodeError:
            out.append(ln)  # not JSON — compare verbatim
            continue
        if isinstance(obj, dict):
            for k in key_masks:
                _del_dotted(obj, k)
        out.append(json.dumps(obj, sort_keys=True, separators=(",", ":")))
    return out


def _normalize_csv(lines: List[str], col_masks: List[str]) -> List[str]:
    if not lines:
        return lines
    reader = list(csv.reader(lines))
    if not reader:
        return lines
    header = reader[0]
    mask_idx = {i for i, h in enumerate(header) if h in col_masks}
    if not mask_idx:
        return lines  # nothing to mask -> verbatim (cheaper, exact)
    out_buf = io.StringIO()
    writer = csv.writer(out_buf, lineterminator="\n")
    for r, row in enumerate(reader):
        if r == 0:
            writer.writerow(row)
            continue
        row = list(row)
        for i in mask_idx:
            if i < len(row):
                row[i] = "<MASK>"
        writer.writerow(row)
    return out_buf.getvalue().rstrip("\n").split("\n")


def _normalize_log(lines: List[str], spec: dict, is_runlog: bool) -> List[str]:
    ts = re.compile(spec["log_ts_regex"])
    token = spec["log_ts_token"]
    drops = [re.compile(p) for p in (spec["runlog_drop_regexes"] if is_runlog else [])]
    dur = re.compile(spec["runlog_dur_regex"]) if is_runlog else None
    dur_tok = spec.get("runlog_dur_token", "<T>s")
    out = []
    for ln in lines:
        if any(d.search(ln) for d in drops):
            continue
        ln = ts.sub(token, ln, count=1)
        if dur is not None:
            ln = dur.sub(dur_tok, ln)
        out.append(ln)
    return out


def classify(relpath: str, spec: dict) -> str:
    """Return artifact class: 'jsonl-manifest', 'csv', 'snapshot', 'product',
    'log', 'runlog', or 'verbatim'."""
    base = os.path.basename(relpath)
    if base == spec["runlog_name"]:
        return "runlog"
    if base in spec["json_key_masks"]:
        return "jsonl-manifest"
    if spec["snapshot_name_hint"] and spec["snapshot_name_hint"] in base.lower():
        return "snapshot"
    low = base.lower()
    if any(h in low for h in spec["product_name_hints"]):
        return "product"
    if base.endswith(CSV_SUFFIX):
        return "csv"
    if base.endswith(LOG_SUFFIX):
        return "log"
    return "verbatim"


def normalize_file(path: str, relpath: str, spec: dict) -> List[str]:
    kind = classify(relpath, spec)
    lines = _read_lines(path)
    base = os.path.basename(relpath)
    if kind == "jsonl-manifest":
        return _normalize_jsonl(lines, spec["json_key_masks"].get(base, []))
    if kind == "snapshot":
        return _normalize_jsonl(lines, spec["snapshot_json_key_masks"])
    if kind == "csv":
        return _normalize_csv(lines, spec["csv_col_masks"].get(base, []))
    if kind == "product":
        return _normalize_product(lines, spec)
    if kind == "log":
        return _normalize_log(lines, spec, is_runlog=False)
    if kind == "runlog":
        return _normalize_log(lines, spec, is_runlog=True)
    return lines  # verbatim


def _normalize_product(lines: List[str], spec: dict) -> List[str]:
    prefixes = spec["product_genline_prefixes"]
    out = []
    for ln in lines:
        if any(ln.startswith(p) for p in prefixes):
            # Canonicalize the whole generation-time line; format is
            # producer-specific, so the safest field-level mask is to blank the
            # numeric week/tow tail while keeping the record's identity prefix.
            out.append(re.sub(r"[-+]?\d[\d.]*", "<N>", ln))
        else:
            out.append(ln)
    return out


def csv_data_rows(path: str) -> Optional[int]:
    if not path.endswith(CSV_SUFFIX):
        return None
    n = len(_read_lines(path))
    return max(0, n - 1)  # minus header


# ----------------------------------------------------------------------------
# Directory model
# ----------------------------------------------------------------------------
def list_artifacts(root: str) -> List[str]:
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out.append(rel)
    return sorted(out)


def norm_sha(path: str, relpath: str, spec: dict) -> str:
    lines = normalize_file(path, relpath, spec)
    h = hashlib.sha256()
    h.update(("\n".join(lines)).encode("utf-8"))
    return h.hexdigest()


@dataclass
class Result:
    ok: bool = True
    missing: List[str] = field(default_factory=list)   # in golden, not candidate
    extra: List[str] = field(default_factory=list)      # in candidate, not golden
    content_diffs: List[Tuple[str, str]] = field(default_factory=list)  # (rel, detail)
    rowcount_fails: List[str] = field(default_factory=list)
    vacuous: List[str] = field(default_factory=list)


# ----------------------------------------------------------------------------
# capture / compare / selfcheck
# ----------------------------------------------------------------------------
def _manifest_counters(root: str) -> dict:
    """Pull the deterministic counters from network_run_manifest.json's complete
    record (n_synced/n_emitted/n_skipped/... ) — the strongest byte-identity
    signal outside the CSVs."""
    for rel in list_artifacts(root):
        if os.path.basename(rel) == "network_run_manifest.json":
            for ln in _read_lines(os.path.join(root, rel)):
                try:
                    obj = json.loads(ln)
                except json.JSONDecodeError:
                    continue
                if isinstance(obj, dict) and obj.get("record") == "complete":
                    return {k: v for k, v in obj.items() if k != "loop_ms"}
    return {}


def cmd_capture(args) -> int:
    spec = load_maskspec(args.maskspec)
    root = args.run_dir
    arts = list_artifacts(root)
    if not arts:
        print(f"FATAL: {root} has no artifacts — golden is vacuous.", file=sys.stderr)
        return 1
    entries = {}
    nonempty_csv = 0
    for rel in arts:
        full = os.path.join(root, rel)
        rows = csv_data_rows(full)
        entries[rel] = {
            "bytes": os.path.getsize(full),
            "norm_sha256": norm_sha(full, rel, spec),
            "csv_rows": rows,
            "class": classify(rel, spec),
        }
        if rel.endswith(CSV_SUFFIX) and os.path.basename(rel).startswith("network_") and (rows or 0) > 0:
            nonempty_csv += 1
    if nonempty_csv == 0:
        print("FATAL: no non-empty network_*.csv trace in golden — vacuous capture.", file=sys.stderr)
        return 1
    manifest = {
        "schema": "compare_network_run/1",
        "run_dir": os.path.abspath(root),
        "maskspec_sha256": maskspec_sha(spec),
        "counters": _manifest_counters(root),
        "artifacts": entries,
    }
    out = args.out or (root.rstrip("/\\") + ".manifest.json")
    with io.open(out, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
    print(f"captured {len(arts)} artifacts ({nonempty_csv} non-empty network CSV traces) -> {out}")
    return 0


def _compare_dirs(golden: str, cand: str, spec: dict, include_logs: bool) -> Result:
    res = Result()
    g_arts = list_artifacts(golden)
    c_arts = list_artifacts(cand)
    g_set, c_set = set(g_arts), set(c_arts)
    res.missing = sorted(g_set - c_set)
    res.extra = sorted(c_set - g_set)
    if res.missing or res.extra:
        res.ok = False
    for rel in sorted(g_set & c_set):
        base = os.path.basename(rel)
        is_log = rel.endswith(LOG_SUFFIX) and base != spec["runlog_name"]
        if is_log and (spec["log_content_default_off"] and not include_logs):
            continue  # set-checked only
        g_full, c_full = os.path.join(golden, rel), os.path.join(cand, rel)
        g_lines = normalize_file(g_full, rel, spec)
        c_lines = normalize_file(c_full, rel, spec)
        if g_lines != c_lines:
            res.ok = False
            res.content_diffs.append((rel, _first_diff(g_lines, c_lines)))
        # QC non-vacuous + exact row-count guard
        if rel.endswith(CSV_SUFFIX):
            gr, cr = csv_data_rows(g_full), csv_data_rows(c_full)
            if (gr or 0) > 0 and gr != cr:
                res.ok = False
                res.rowcount_fails.append(f"{rel}: golden {gr} rows, candidate {cr}")
    return res


def _first_diff(a: List[str], b: List[str]) -> str:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return (f"first diff at line {i + 1}:\n"
                    f"    golden: {a[i][:200]}\n"
                    f"    cand  : {b[i][:200]}")
    if len(a) != len(b):
        longer, side = (a, "golden") if len(a) > len(b) else (b, "cand")
        return f"length differs: golden={len(a)} cand={len(b)}; first extra ({side}) line {n+1}: {longer[n][:200]}"
    return "diff (unlocated)"


def cmd_compare(args) -> int:
    spec = load_maskspec(args.maskspec)
    if args.manifest:
        return _compare_vs_manifest(args.manifest, args.candidate, spec, args.include_logs)
    res = _compare_dirs(args.golden, args.candidate, spec, args.include_logs)
    return _report(res, args.max_diffs)


def _compare_vs_manifest(manifest_path: str, cand: str, spec: dict, include_logs: bool) -> int:
    with io.open(manifest_path, "r", encoding="utf-8") as fh:
        man = json.load(fh)
    if man.get("maskspec_sha256") != maskspec_sha(spec):
        print("FATAL: maskspec changed since this golden manifest was captured — "
              "goldens are READ-ONLY; do not re-freeze mid-series.", file=sys.stderr)
        return 1
    res = Result()
    c_arts = set(list_artifacts(cand))
    g_arts = set(man["artifacts"].keys())
    res.missing = sorted(g_arts - c_arts)
    res.extra = sorted(c_arts - g_arts)
    if res.missing or res.extra:
        res.ok = False
    for rel, meta in man["artifacts"].items():
        if rel not in c_arts:
            continue
        base = os.path.basename(rel)
        is_log = rel.endswith(LOG_SUFFIX) and base != spec["runlog_name"]
        if is_log and (spec["log_content_default_off"] and not include_logs):
            continue
        c_full = os.path.join(cand, rel)
        if norm_sha(c_full, rel, spec) != meta["norm_sha256"]:
            res.ok = False
            res.content_diffs.append((rel, "normalized SHA-256 mismatch vs golden manifest"))
        if rel.endswith(CSV_SUFFIX) and (meta.get("csv_rows") or 0) > 0:
            cr = csv_data_rows(c_full)
            if cr != meta["csv_rows"]:
                res.ok = False
                res.rowcount_fails.append(f"{rel}: golden {meta['csv_rows']} rows, candidate {cr}")
    return _report(res, 40)


def cmd_selfcheck(args) -> int:
    """Twice-run self-compare: report every normalized diff as a mask candidate.
    A config that is not run-to-run identical (outside the frozen masks) must be
    fixed or dropped before it can serve as a golden."""
    spec = load_maskspec(args.maskspec)
    res = _compare_dirs(args.dir_a, args.dir_b, spec, include_logs=args.include_logs)
    print("=== selfcheck (twice-run determinism / empirical mask discovery) ===")
    if res.missing or res.extra:
        print(f"  artifact-set differs: {len(res.missing)} missing, {len(res.extra)} extra "
              "(nondeterministic file set — investigate before use as golden)")
        for m in res.missing[:20]:
            print(f"    only in A: {m}")
        for e in res.extra[:20]:
            print(f"    only in B: {e}")
    if not res.content_diffs and not res.rowcount_fails and not res.missing and not res.extra:
        print("  CLEAN: run-to-run byte-identical under the frozen maskspec. Safe as a golden.")
        return 0
    print(f"  {len(res.content_diffs)} artifact(s) differ run-to-run (MASK CANDIDATES — "
          "each is either a missed wall-clock field to add to the frozen maskspec, "
          "or genuine nondeterminism that disqualifies this config):")
    for rel, detail in res.content_diffs[:args.max_diffs]:
        print(f"\n  [{rel}]  (class={classify(rel, spec)})")
        for line in detail.splitlines():
            print(f"    {line}")
    for rc in res.rowcount_fails:
        print(f"  row-count nondeterminism: {rc}")
    return 1 if args.strict else 0


def _report(res: Result, max_diffs: int) -> int:
    if res.ok:
        print("PASS: normalized artifact sets are byte-identical.")
        return 0
    print("FAIL: byte-identity gate tripped.")
    if res.missing:
        print(f"\n  {len(res.missing)} MISSING artifact(s) (in golden, absent from candidate):")
        for m in res.missing[:max_diffs]:
            print(f"    - {m}")
    if res.extra:
        print(f"\n  {len(res.extra)} EXTRA artifact(s) (in candidate, not in golden):")
        for e in res.extra[:max_diffs]:
            print(f"    + {e}")
    if res.rowcount_fails:
        print(f"\n  {len(res.rowcount_fails)} QC row-count mismatch(es):")
        for rc in res.rowcount_fails[:max_diffs]:
            print(f"    ! {rc}")
    if res.content_diffs:
        print(f"\n  {len(res.content_diffs)} content diff(s):")
        for rel, detail in res.content_diffs[:max_diffs]:
            print(f"\n  [{rel}]")
            for line in detail.splitlines():
                print(f"    {line}")
        if len(res.content_diffs) > max_diffs:
            print(f"\n  ... and {len(res.content_diffs) - max_diffs} more (raise --max-diffs).")
    print("\n  Per the plan: any diff HALTS the increment and bisects — never patch forward, "
          "never regenerate goldens or masks to make it pass.")
    return 1


# ----------------------------------------------------------------------------
# maskspec plumbing
# ----------------------------------------------------------------------------
def load_maskspec(path: Optional[str]) -> dict:
    if not path:
        return DEFAULT_MASKSPEC
    with io.open(path, "r", encoding="utf-8") as fh:
        user = json.load(fh)
    spec = json.loads(json.dumps(DEFAULT_MASKSPEC))  # deep copy
    spec.update(user)
    return spec


def maskspec_sha(spec: dict) -> str:
    return hashlib.sha256(json.dumps(spec, sort_keys=True).encode("utf-8")).hexdigest()[:16]


def cmd_dump_maskspec(args) -> int:
    out = args.out
    text = json.dumps(DEFAULT_MASKSPEC, indent=2, sort_keys=True)
    if out:
        with io.open(out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        print(f"wrote default maskspec (sha {maskspec_sha(DEFAULT_MASKSPEC)}) -> {out}")
    else:
        print(text)
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    # Diffed lines can carry non-ASCII (σ, °, ⁻¹) that the Windows cp1252 console
    # cannot encode; force UTF-8 output so reporting never crashes on content.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pc = sub.add_parser("capture", help="record a golden's artifact set + normalized hashes")
    pc.add_argument("run_dir")
    pc.add_argument("--out", help="output manifest path (default: <run_dir>.manifest.json)")
    pc.add_argument("--maskspec", help="frozen maskspec JSON (default: built-in)")
    pc.set_defaults(func=cmd_capture)

    pp = sub.add_parser("compare", help="gate a candidate run against a golden (dir or --manifest)")
    pp.add_argument("golden", nargs="?", help="golden run dir")
    pp.add_argument("candidate", help="candidate run dir")
    pp.add_argument("--manifest", help="compare against a captured golden manifest instead of a dir")
    pp.add_argument("--maskspec", help="frozen maskspec JSON (default: built-in)")
    pp.add_argument("--include-logs", action="store_true", help="also content-compare .log files (ts-masked; serial goldens only)")
    pp.add_argument("--max-diffs", type=int, default=40)
    pp.set_defaults(func=cmd_compare)

    ps = sub.add_parser("selfcheck", help="twice-run self-compare for empirical mask discovery")
    ps.add_argument("dir_a")
    ps.add_argument("dir_b")
    ps.add_argument("--maskspec", help="frozen maskspec JSON (default: built-in)")
    ps.add_argument("--include-logs", action="store_true")
    ps.add_argument("--strict", action="store_true", help="exit nonzero if any diff found")
    ps.add_argument("--max-diffs", type=int, default=40)
    ps.set_defaults(func=cmd_selfcheck)

    pd = sub.add_parser("dump-maskspec", help="emit the default frozen maskspec")
    pd.add_argument("--out", help="write to file instead of stdout")
    pd.set_defaults(func=cmd_dump_maskspec)

    args = p.parse_args(argv)
    if args.cmd == "compare" and not args.manifest and not args.golden:
        p.error("compare needs a golden dir (or --manifest)")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
