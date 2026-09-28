#!/usr/bin/env python3
"""make_golden_config.py -- generate and verify the golden network configs.

Why: the project ships new work default-off and byte-identity-gated, and the promotion
ledger (docs/CONFIG_promotion_ledger.{md,json}) records which knobs are validated and in
what regime, but nothing turned that record into an executable posture, so validated
knobs kept being re-discovered one wasted run at a time. The goldens are the standard
armed set made executable; every new arm should be a copy of a golden with only the knob
under test and the run paths changed.

What: config/golden/golden_manifest.json lists one entry per lineage:

    base         a shipped config (its "extends" chain is flattened base-first, child wins,
                 exactly as the loader does; scripts/gnss_config_chain.py)
    pins/values  knobs set on top of the base (dotted keys)
    unset        keys removed (e.g. retired hand-tunings that a promoted gate supersedes)
    exceptions   ledger recommendations that are deliberately left unarmed, each with the
                 reason (the verify step accepts exactly these and nothing else)

Generate (default) writes each golden as a flat config (no extends) with a provenance
comment, rewrites the run paths under runs/<name>/, then verifies.

Verify (--verify, also run after generate) evaluates every RECOMMEND_ON / RECOMMEND_SCOPED
ledger row against the golden with the same regime and armed_when logic the offline checker
and the loader use (scripts/config_posture_check.py). Any in-regime recommendation that is
unarmed and not in the golden's exceptions is a failure (exit 1), so a ledger row added by a
future card that the goldens do not carry fails the next verify, which is the promotion step.
It also fails if a pinned knob does not read back, or if an exception names a knob that is
armed (a stale exception).

Usage:
    python scripts/make_golden_config.py                 # generate + verify all goldens
    python scripts/make_golden_config.py --verify        # verify only (CI / session start)
    python scripts/make_golden_config.py --only golden_sim_tri_w6
    python scripts/make_golden_config.py --selftest
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import io
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
from gnss_config_chain import load_config  # noqa: E402
import config_posture_check as cpc  # noqa: E402

ROOT = os.path.abspath(os.path.join(_HERE, ".."))
MANIFEST = os.path.join(ROOT, "config", "golden", "golden_manifest.json")
LEDGER = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.json")


def _set_dotted(d, key, val):
    parts = key.split(".")
    cur = d
    for p in parts[:-1]:
        cur = cur.setdefault(p, {})
        if not isinstance(cur, dict):
            raise SystemExit(f"cannot set {key}: {p} is not an object")
    cur[parts[-1]] = val


def _unset_dotted(d, key):
    parts = key.split(".")
    cur = d
    for p in parts[:-1]:
        if not isinstance(cur, dict) or p not in cur:
            return False
        cur = cur[p]
    return cur.pop(parts[-1], None) is not None if isinstance(cur, dict) else False


#: Output-side keys whose runs/<dir>/ component is re-pointed at the golden's own run dir.
#: Input paths (rinex_path, the fixture roster, internal_data_root, replay_products_path) are
#: never rewritten: an earlier generator did, and the sim golden then failed to parse every
#: station's RINEX (it looked for the fixture under runs/<golden>/).
OUTPUT_PATH_KEYS = ("product_output_path", "state_snapshot_path")


def _rewrite_run_paths(cfg, name):
    """Output paths under runs/<something>/ become runs/<name>/ (same file name)."""
    def walk(o, key=None):
        if isinstance(o, dict):
            for k, v in o.items():
                o[k] = walk(v, k)
            return o
        if isinstance(o, list):
            return [walk(x, key) for x in o]
        if isinstance(o, str) and key in OUTPUT_PATH_KEYS and "/runs/" in o.replace("\\", "/"):
            s = o.replace("\\", "/")
            head, tail = s.split("/runs/", 1)
            rest = tail.split("/", 1)
            return f"{head}/runs/{name}/{rest[1]}" if len(rest) == 2 else f"{head}/runs/{name}"
        return o
    return walk(cfg)


def _sha(path):
    return hashlib.sha256(io.open(path, "rb").read()).hexdigest()[:16]


def generate(entry, ledger_path, when):
    base = os.path.join(ROOT, entry["base"])
    cfg, chain = load_config(base)
    if not isinstance(cfg, dict):
        raise SystemExit(f"{entry['base']} is not a config object")
    cfg.pop("extends", None)
    for k, v in (entry.get("pins") or {}).items():
        _set_dotted(cfg, k, v)
    for k, v in (entry.get("values") or {}).items():
        _set_dotted(cfg, k, v)
    for k in entry.get("unset") or []:
        _unset_dotted(cfg, k)
    cfg = _rewrite_run_paths(cfg, entry["name"])
    prov = [
        f"GOLDEN CONFIG '{entry['name']}' — GENERATED by scripts/make_golden_config.py on {when}; do not hand-edit:",
        "edit config/golden/golden_manifest.json (or the ledger) and regenerate.",
        f"base: {entry['base']} (extends chain flattened: {' -> '.join(os.path.basename(c) for c in chain) if chain else 'none'}; base sha256[:16] {_sha(base)})",
        f"ledger: docs/CONFIG_promotion_ledger.json sha256[:16] {_sha(ledger_path)}",
        entry.get("description", ""),
        "pins: " + json.dumps(entry.get("pins") or {}, ensure_ascii=False),
        "exceptions (deliberately unarmed ledger recommendations): " + json.dumps(entry.get("exceptions") or {}, ensure_ascii=False),
        "To make an arm: copy this file, change ONLY the knob under test and the run paths; keep every other line.",
    ]
    cfg = {"_comment": prov, **{k: v for k, v in cfg.items() if k != "_comment"}}
    out = os.path.join(ROOT, entry["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    io.open(out, "w", encoding="utf-8", newline="\n").write(json.dumps(cfg, indent=1, ensure_ascii=False) + "\n")
    return out


def verify(entry, ledger):
    """-> list of failure strings (empty = PASS)."""
    out = os.path.join(ROOT, entry["out"])
    if not os.path.isfile(out):
        return [f"{entry['name']}: golden file missing: {entry['out']} (run without --verify to generate)"]
    cfg, _chain = load_config(out)
    env = cpc.make_env(cfg)
    fails = []
    exceptions = entry.get("exceptions") or {}
    seen_exc = set()
    n_armed = n_gap = 0
    for k in ledger["knobs"]:
        if k.get("verdict") not in cpc.RECOMMENDED:
            continue
        in_regime, _pred, _filtered = cpc.applies(k, env)
        if not in_regime:
            continue
        is_armed, _found, _v = cpc.armed(k, cfg, env)
        name = cpc.norm(k["knob"])
        if is_armed:
            n_armed += 1
            if name in exceptions:
                fails.append(f"{entry['name']}: STALE exception — {name} is armed but listed as an exception")
            continue
        n_gap += 1
        if name in exceptions:
            seen_exc.add(name)
            continue
        fails.append(f"{entry['name']}: UNARMED in-regime {k['verdict']} — {name}: {k.get('posture_why', '')[:140]}")
    for name in exceptions:
        if name not in seen_exc and not any(cpc.norm(k["knob"]) == name for k in ledger["knobs"]):
            fails.append(f"{entry['name']}: exception names an unknown ledger knob: {name}")
    for k, v in (entry.get("pins") or {}).items():
        found, got = cpc.resolve(cfg, k)
        if not found or got != v:
            fails.append(f"{entry['name']}: pin not honoured — {k} = {got!r} (wanted {v!r})")
    for k in entry.get("unset") or []:
        found, _ = cpc.resolve(cfg, k)
        if found:
            fails.append(f"{entry['name']}: key should be unset — {k}")
    print(f"  {entry['name']}: {n_armed} recommended knobs armed in regime, {n_gap} unarmed "
          f"({len(seen_exc)} documented exceptions), {len(fails)} failure(s)")
    return fails


def selftest():
    import tempfile
    tmp = tempfile.mkdtemp()
    base = os.path.join(tmp, "base.json")
    io.open(base, "w").write(json.dumps({
        "processing": {"mode": "ppp-rtk-network", "frequency_mode": "triple", "active_systems": ["GPS"]},
        "network": {"stations": [{"name": f"S{i}"} for i in range(12)], "phase_leveling": True,
                    "product_output_path": "C:/x/runs/old/products.txt"}}))
    ledger = {"knobs": [
        {"knob": "network.foo", "verdict": "RECOMMEND_ON", "regime": "True", "posture_why": "foo"},
        {"knob": "network.bar", "verdict": "RECOMMEND_SCOPED", "regime": "tri", "posture_why": "bar"},
        {"knob": "network.thr", "verdict": "RECOMMEND_ON", "regime": "True",
         "armed_when": "0.24 <= val(\"network.thr\", 0.0) <= 0.26", "posture_why": "thr"},
    ]}
    entry = {"name": "g", "out": os.path.relpath(os.path.join(tmp, "g.json"), ROOT), "base": os.path.relpath(base, ROOT),
             "pins": {"network.foo": True, "network.thr": 0.25}, "unset": [], "exceptions": {"network.bar": "reason"}}
    lp = os.path.join(tmp, "ledger.json"); io.open(lp, "w").write(json.dumps(ledger))
    out = generate(entry, lp, "selftest")
    g = json.load(io.open(out))
    assert g["network"]["product_output_path"] == "C:/x/runs/g/products.txt", g["network"]["product_output_path"]
    assert "extends" not in g
    fails = verify(entry, ledger)
    assert fails == [], fails
    # a new ledger row the golden does not carry must FAIL
    ledger["knobs"].append({"knob": "network.new", "verdict": "RECOMMEND_ON", "regime": "True", "posture_why": "new"})
    fails = verify(entry, ledger)
    assert len(fails) == 1 and "network.new" in fails[0], fails
    # a stale exception must FAIL
    entry["exceptions"]["network.foo"] = "stale"
    ledger["knobs"].pop()
    fails = verify(entry, ledger)
    assert len(fails) == 1 and "STALE" in fails[0], fails
    print("selftest: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=MANIFEST)
    ap.add_argument("--ledger", default=LEDGER)
    ap.add_argument("--verify", action="store_true", help="verify only, do not regenerate")
    ap.add_argument("--only", help="golden name")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    if a.selftest:
        return selftest()
    man = json.load(io.open(a.manifest, encoding="utf-8"))
    ledger = json.load(io.open(a.ledger, encoding="utf-8"))
    when = _dt.date.today().isoformat()
    entries = [e for e in man["goldens"] if not a.only or e["name"] == a.only]
    if not entries:
        raise SystemExit(f"no golden named {a.only}")
    fails = []
    for e in entries:
        if not a.verify:
            out = generate(e, a.ledger, when)
            print(f"wrote {os.path.relpath(out, ROOT)}")
        fails += verify(e, ledger)
    for f in fails:
        print("FAIL:", f)
    print("golden verify:", "PASS" if not fails else f"FAIL ({len(fails)})")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
