#!/usr/bin/env python3
"""Resolve `"extends"` config inheritance the way the C++ loader does.

Commit 18b4502 gave configs a base+delta inheritance format.  Every offline tool that
READS a config has to resolve it or it sees a delta - a handful of keys - and mistakes
that for the whole config.  That is not a cosmetic gap: `scripts/config_posture_check.py`
read `_golden0729_net305_3h.json` as "0 stations, 0 constellations" and reported 8 armed
knobs as unarmed, because the roster and the arm set live three files up the chain.

The single source of truth is the loader:

    src/core/config/GnssConfigLoad.cpp        loadConfigWithExtends  (:89-158)
    include/config/GnssConfigJsonHelpers.hpp  detail::deepMergeJson  (:63-74)
    tests/test_config_extends.cpp             the semantics, locked

and the rules this module mirrors, one for one:

  * `"extends"` is a string or an array of strings.  Anything else is a hard error.
  * Bases are resolved DEPTH-FIRST and applied LEFT-TO-RIGHT, so a later base wins over
    an earlier one, and the child wins over every base.
  * object vs object merges KEY BY KEY, recursively.  Everything else REPLACES.
  * ARRAYS REPLACE - they do not concatenate.  This is a safety property, not a
    convenience: `network.stations` order and count are hashed into the warm-resume
    fingerprint, so a concatenating merge would silently produce a roster that matches no
    intended regime.  Same for `processing.active_systems` and `signal_priorities`.
  * A base path resolves against the DECLARING FILE's directory first, then the process
    working directory.
  * `"extends"` is a loader directive, not a settings key - it is erased from the merged
    document, so the result looks hand-written and no section loader ever sees it.
  * Depth cap 8 (`kMaxExtendsDepth`), and a cycle is a hard error.

Two deliberate faithfulness details that look like bugs and are not:

  * The cycle set is shared across the WHOLE resolution and is never popped, exactly as
    the loader's `seen` vector is.  A diamond (two bases sharing one ancestor) is
    therefore rejected as a cycle.  That is the loader's behaviour, so this module has to
    report it too - a helper that quietly accepted a config the producer will refuse is
    worse than one that agrees.
  * JSON comments are accepted, because the loader parses with nlohmann's
    `ignore_comments=true` and several shipped configs use `//` (e.g.
    `config/_tmp_glonass_ifb_test.json`).  `json.loads` alone would reject them.

Use it instead of `json.load` wherever a script reads a config it did not write::

    from gnss_config_chain import load_config
    cfg, chain = load_config(path)      # cfg is flat; chain is the applied base order

`chain` is in APPLICATION order and matches, element for element, the `config_extends`
array in `logs/network_run_manifest.json` and the "Config INHERITANCE" line in
`logs/gnss_config.log`.  That correspondence is the check that this module and the
loader have not drifted; `--chain` prints it.

CLI::

    python scripts/gnss_config_chain.py <config>            # the merged document
    python scripts/gnss_config_chain.py <config> --chain    # just the applied base order
    python scripts/gnss_config_chain.py <config> --get network.stations
"""
import argparse
import json
import os
import sys

#: Mirrors `kMaxExtendsDepth` (GnssConfigLoad.cpp:65).  A guard against a hand-edited
#: chain, not a design limit - the deepest sensible real chain is profile -> campaign -> leg.
MAX_EXTENDS_DEPTH = 8


class ConfigChainError(ValueError):
    """A config chain the loader would also refuse: missing base, cycle, too deep, bad type.

    Raised rather than warned-and-continued on purpose.  The loader treats every one of
    these as a hard error (`tests/test_config_extends.cpp`: MissingBaseIsAHardError), and
    for the same reason: a silently-ignored missing base leaves you running the DELTA
    only - a config that looks armed and is not.
    """


# --------------------------------------------------------------------------------------
#  parsing
# --------------------------------------------------------------------------------------
def strip_json_comments(text):
    """Remove `//` and `/* */` comments, honouring string literals and escapes.

    A regex cannot do this safely: config values are full of `//` inside strings
    ("C:/GnssProcessor/...", "https://..."), and blanking one of those would corrupt a
    path instead of a comment.  This is a character scanner for that reason.  Comment
    bodies are replaced with nothing but newlines are kept, so a parse error still
    reports the line number the file actually has.
    """
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:       # an escape consumes the next char whole, so
                out.append(text[i + 1])       # a trailing \" cannot end the string early
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


def parse_config_file(path):
    """Parse ONE config file (no inheritance resolution), comments tolerated."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError as e:
        raise ConfigChainError(f"cannot open config '{path}': {e}")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        # Only pay for the scan when a plain parse fails, so the common case is unchanged
        # and a genuine syntax error is not masked by the stripper.
        try:
            return json.loads(strip_json_comments(text))
        except json.JSONDecodeError as e:
            raise ConfigChainError(f"cannot parse config '{path}': {e}")


# --------------------------------------------------------------------------------------
#  merge
# --------------------------------------------------------------------------------------
def deep_merge(base, overlay):
    """Overlay onto base, in place; mirrors detail::deepMergeJson.

    object vs object recurses; EVERYTHING else replaces, arrays included.  A null overlay
    value replaces too, so a child can blank a string a base set (e.g. reset an inherited
    `state_snapshot_path` to "").
    """
    if not isinstance(base, dict) or not isinstance(overlay, dict):
        return overlay
    for k, v in overlay.items():
        if k in base and isinstance(base[k], dict) and isinstance(v, dict):
            base[k] = deep_merge(base[k], v)
        else:
            base[k] = v
    return base


# --------------------------------------------------------------------------------------
#  resolution
# --------------------------------------------------------------------------------------
def _resolve(path, chain, seen, depth):
    """Recursive worker; the direct transcription of loadConfigWithExtends."""
    if depth > MAX_EXTENDS_DEPTH:
        raise ConfigChainError(
            f"extends chain deeper than {MAX_EXTENDS_DEPTH} levels at '{path}' - refusing "
            f"to continue (a base almost certainly re-extends a descendant)")
    key = os.path.normcase(os.path.abspath(path))
    if key in seen:
        raise ConfigChainError(
            f"extends CYCLE - '{path}' is already in the chain "
            f"[{' -> '.join(seen)}]")
    seen.append(key)

    self_json = parse_config_file(path)
    if not isinstance(self_json, dict) or "extends" not in self_json:
        return self_json                       # no inheritance - verbatim, as the loader does

    raw = self_json["extends"]
    if isinstance(raw, str):
        bases = [raw]
    elif isinstance(raw, list):
        if not all(isinstance(b, str) for b in raw):
            raise ConfigChainError(f"'{path}' has a non-string entry in its extends array")
        bases = list(raw)
    else:
        raise ConfigChainError(
            f"'{path}' has an \"extends\" that is neither a string nor an array of strings")
    # A loader directive, not a settings key.
    del self_json["extends"]

    merged = {}
    for b in bases:
        # Relative to the DECLARING file first, then to the process working directory.
        # os.path.join already returns `b` unchanged when it is absolute, which is what
        # std::filesystem's operator/ does for an absolute right-hand side.
        bp = os.path.join(os.path.dirname(os.path.abspath(path)), b)
        if not os.path.exists(bp):
            bp = b
        merged = deep_merge(merged, _resolve(bp, chain, seen, depth + 1))
        chain.append(bp)
    return deep_merge(merged, self_json)        # the child always wins


def load_config(path):
    """-> (merged_config, chain).

    `chain` lists every base actually merged, in application order - empty when the config
    declares no `extends`, in which case the returned document is exactly what the file
    contains (the loader's byte-identical path).
    """
    chain = []
    return _resolve(path, chain, [], 0), chain


def load_config_flat(path):
    """`load_config` when the caller does not care about provenance."""
    return load_config(path)[0]


def get_path(cfg, dotted, default=None):
    """Dotted lookup into a merged config; `default` when any segment is missing."""
    cur = cfg
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return default
        cur = cur[part]
    return cur


# --------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Flatten a config's \"extends\" chain.")
    ap.add_argument("config")
    ap.add_argument("--chain", action="store_true",
                    help="print only the applied base order (compare against the "
                         "config_extends array in logs/network_run_manifest.json)")
    ap.add_argument("--get", metavar="DOTTED",
                    help="print one merged value instead of the whole document")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    try:
        cfg, chain = load_config(a.config)
    except ConfigChainError as e:
        print(f"config chain error: {e}", file=sys.stderr)
        return 2
    if a.chain:
        if not chain:
            print("(no extends - self-contained config)")
        for i, c in enumerate(chain):
            print(f"{i}: {c}")
        return 0
    if a.get:
        print(json.dumps(get_path(cfg, a.get), indent=2))
        return 0
    print(json.dumps(cfg, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
