#!/usr/bin/env python3
"""Config POSTURE check - which validated-good knobs is this config leaving OFF?

The project ships new work default-OFF + byte-identity-gated (standing invariant #6), which is
correct for new work but has no PROMOTION step: nothing ever moves from "validated good" to
"default on".  Validated knobs therefore accumulate in the OFF pile and get re-discovered by
accident, one wasted run at a time.

This reads the recommendation ledger (docs/CONFIG_promotion_ledger.json) and reports, for a
given config, every recommended knob that is off/absent AND whose validated REGIME matches
this config.  It does NOT edit anything.

*** SCOPE IS LOAD-BEARING - do not read this as "turn them all on".  The ledger's SCOPED tier
exists because knobs validated at one scale can be harmful at another: network.mw_wl
.bridge_sample_debias was validated at 50 stations and measured net-harmful at 305
(short-baseline wrong-integer rate 0.0% -> 12.5%).

REGIME FILTERING (added Jul-2026).  Each recommended knob carries a machine-readable
``regime`` predicate in the ledger, evaluated against this config.  A knob outside its
validated regime is SUPPRESSED, not reported - without this the checker over-reports (it used
to list the real-time-only ``apriori_source = brdc_rts`` against a post-processed 305-station
config).  The predicates are the same ones the loader's ``GnssConfig::logNetworkPosture_``
evaluates at run time, so the offline check and the run-time POSTURE block agree.

    python scripts/config_posture_check.py runs/guards_cont/leg.json
    python scripts/config_posture_check.py <cfg> --tier all
    python scripts/config_posture_check.py <cfg> --tier all --show-suppressed
    python scripts/config_posture_check.py --verify-loader     # ledger <-> loader coverage
"""
import argparse
import ast
import json
import os
import glob
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
LEDGER = os.path.join(_HERE, "..", "docs", "CONFIG_promotion_ledger.json")
LOADER = os.path.join(_HERE, "..", "src", "core", "config", "GnssConfigLoadNetwork.cpp")

RECOMMENDED = ("RECOMMEND_ON", "RECOMMEND_SCOPED")


# ----------------------------------------------------------------------------------
#  config access
# ----------------------------------------------------------------------------------
def norm(knob):
    """Ledger entries carry prose suffixes ('network.x (W6 slice)', 'a / b') - keep the path."""
    return knob.split(" ")[0].split("/")[0].strip().rstrip(".,;:")


def resolve(cfg, dotted):
    """-> (found, value). Dotted path into nested dicts; tolerates a flat alias."""
    cur = cfg
    for part in norm(dotted).split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            return False, None
    return True, cur


def is_off(v):
    return v is None or v is False or v == 0 or v == 0.0 or v == ""


# ----------------------------------------------------------------------------------
#  regime predicate DSL
# ----------------------------------------------------------------------------------
#  Mirrors the locals computed at the top of GnssConfig::logNetworkPosture_.  Keep the two
#  in step: if you add a predicate here, add it there (and vice versa).
_ALLOWED_NAMES = {"n_sta", "n_sys", "tri", "warm", "lane", "rt", "par", "pstations", "gps_only",
                  "gps_gal_only", "spacing_s", "on", "val", "v", "found", "True", "False", "None"}
_ALLOWED_CALLS = {"on", "val"}
_ALLOWED_NODES = (
    ast.Expression, ast.BoolOp, ast.UnaryOp, ast.BinOp, ast.Compare, ast.Call, ast.Name,
    ast.Load, ast.Constant, ast.And, ast.Or, ast.Not, ast.Eq, ast.NotEq, ast.Lt, ast.LtE,
    ast.Gt, ast.GtE, ast.In, ast.NotIn, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.Tuple,
    ast.List, ast.USub,
)


class RegimeError(ValueError):
    pass


def _check_expr(expr):
    """Reject anything that is not a pure boolean predicate over the allowed tokens."""
    try:
        tree = ast.parse(expr, mode="eval")
    except SyntaxError as e:
        raise RegimeError(f"syntax error in predicate {expr!r}: {e}")
    for node in ast.walk(tree):
        if not isinstance(node, _ALLOWED_NODES):
            raise RegimeError(f"disallowed syntax {type(node).__name__} in predicate {expr!r}")
        if isinstance(node, ast.Name) and node.id not in _ALLOWED_NAMES:
            raise RegimeError(f"unknown token {node.id!r} in predicate {expr!r}")
        if isinstance(node, ast.Call) and not (isinstance(node.func, ast.Name)
                                               and node.func.id in _ALLOWED_CALLS):
            raise RegimeError(f"only on(...)/val(...) may be called, in predicate {expr!r}")
    return tree


def make_env(cfg):
    """Regime variables, derived from the config exactly as the loader derives them."""
    net = cfg.get("network", {}) or {}
    proc = cfg.get("processing", {}) or {}
    freq = str(proc.get("frequency_mode", proc.get("signal_mode", "dual")) or "").lower()
    mw = net.get("mw_wl", {}) or {}
    lc = net.get("lane_carry", {}) or {}

    def on(path):
        found, v = resolve(cfg, path)
        return found and not is_off(v)

    def val(path, default=0):
        """Resolved config value, or the C++ CODE default the caller supplies.  An absent key
        is not zero - it is whatever the loader would have used, so the caller must say."""
        found, v = resolve(cfg, path)
        return v if found else default

    systems = proc.get("active_systems", []) or []
    return {
        "n_sta": len(net.get("stations", []) or []),
        "n_sys": len(systems),
        "tri":   freq in ("triple", "all"),
        "warm":  bool(net.get("state_snapshot_path", "")),
        "lane":  bool(mw.get("ewl_enable") or mw.get("enable") or lc.get("enable")),
        # service_mode IS the real-time service producer, so it counts as the RT posture:
        # knobs scoped "post-processing only" must stay silent there and RT-only knobs must
        # stay silent everywhere else.  Mirrors the loader's `rt`.
        "rt":    (str(cfg.get("app_mode", "post")).lower() in ("realtime", "real_time", "rt")
                  or bool(net.get("service_mode"))),
        # The parallel family is SILENTLY inert at or below the threshold.  NOTE the
        # default: network.parallel_stations is the one default-TRUE knob in this family
        # (GnssConfigLoadNetwork.cpp:521-522), so an ABSENT key means ON - reading it with a
        # plain truthiness test would invert every parallel predicate.  Same for the
        # threshold, whose code default is 10 (:523-524), not the harness convention of 8.
        "pstations": bool(net.get("parallel_stations", True)),
        "par":   bool(net.get("parallel_stations", True)) and
                 len(net.get("stations", []) or []) > int(net.get("parallel_min_stations", 10) or 0),
        # n_sys == 1 does NOT mean GPS-only - active_systems is a list of NAMES, so a count
        # admits GPS+BeiDou where the ledger says GPS or GPS+Galileo only.
        "gps_only": len(systems) == 1 and str(systems[0]) == "GPS",
        "gps_gal_only": bool(systems) and set(map(str, systems)) <= {"GPS", "Galileo"},
        # Config-determinable epoch spacing; 0 when gnss_rate_s is 0 (native RINEX rate).
        "spacing_s": float(proc.get("gnss_rate_s", 0.0) or 0.0)
                     * float(net.get("epoch_decimation_n", 1) or 1),
        "on":    on,
        "val":   val,
    }


def evaluate(expr, env, extra=None):
    _check_expr(expr)
    ns = dict(env)
    if extra:
        ns.update(extra)
    return bool(eval(compile(ast.parse(expr, mode="eval"), "<regime>", "eval"),  # noqa: S307
                     {"__builtins__": {}}, ns))


def applies(knob, env):
    """-> (applies?, predicate_text, filtered?).  A knob with no regime always applies."""
    expr = knob.get("regime")
    if not expr:
        return True, None, False
    return evaluate(expr, env), expr, True


def armed(knob, cfg, env):
    """-> (armed?, found, value).  ``armed_when`` handles non-boolean knobs (thresholds,
    counts, strings) whose recommended state is a VALUE, not merely truthiness."""
    found, v = resolve(cfg, knob["knob"])
    expr = knob.get("armed_when")
    if expr:
        try:
            return evaluate(expr, env, {"v": v, "found": found}), found, v
        except (RegimeError, TypeError):
            pass
    return (found and not is_off(v)), found, v


# ----------------------------------------------------------------------------------
#  loader <-> ledger coverage cross-check
# ----------------------------------------------------------------------------------
def loader_knobs(path=LOADER):
    """Knob paths named inside GnssConfig::logNetworkPosture_ (items table + bundles)."""
    src = open(path, encoding="utf-8", errors="replace").read()
    start = src.find("void GnssConfig::logNetworkPosture_")
    if start < 0:
        raise SystemExit(f"logNetworkPosture_ not found in {path}")
    end = src.find("\n    void GnssConfig::", start + 10)
    body = src[start:end if end > 0 else len(src)]
    out = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        head = lit.split(" ")[0].rstrip(".,;:")
        if re.fullmatch(r"(network|estimation|processing|filter)\.[A-Za-z0-9_.]+", head):
            out.add(head)
    return out


def loader_default_true(config_src_dir=None):
    """Knob paths whose jval default in the loader is `true`.

    THE TRAP THIS EXISTS FOR: on(path) is a truthiness test and returns False for an ABSENT
    key, so using on() on a default-TRUE knob inverts the predicate.  It bit
    network.parallel_stations (5 predicates) and network.carry_invariant_emit_rebase (which
    silently suppressed the emit_pb_filter_gate_v2 recommendation - the single knob the field
    record calls the live promotion gap).  Those must use val(path, True) instead.
    """
    d = config_src_dir or os.path.join(_HERE, "..", "src", "core", "config")
    out = set()
    for path in glob.glob(os.path.join(d, "*.cpp")):
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r'jval<bool>\s*\(\s*json,\s*"([^"]+)"\s*,\s*"[^"]*"\s*,\s*true\s*\)',
                             src):
            out.add(m.group(1))
    # struct-literal defaults the jval regex cannot see (nlohmann .value("x", true) form)
    for path in glob.glob(os.path.join(d, "*.cpp")):
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r'(\w+)\.value\(\s*"([^"]+)"\s*,\s*true\s*\)', src):
            out.add("network.closed_loop." + m.group(2))
    return out


def verify_loader(led, loader_path=LOADER):
    # regime "False" marks a recommended knob that is deliberately NOT a loader posture row
    # (e.g. network.product_flush_per_epoch, which service_mode force-arms in the only regime
    # that wants it, so the check could never fire).  Those are exempt from loader coverage,
    # but the posture_why must record the reason.
    want = {norm(k["knob"]) for k in led["knobs"]
            if k["verdict"] in RECOMMENDED and str(k.get("regime", "")).strip() != "False"}
    have = loader_knobs(loader_path)
    missing = sorted(want - have)
    extra = sorted(have - want)
    print(f"# loader coverage: {len(want & have)}/{len(want)} recommended knobs are named in "
          f"logNetworkPosture_")
    for k in missing:
        row = next(r for r in led["knobs"] if norm(r["knob"]) == k)
        print(f"  MISSING from loader  [{row['verdict'][10:]:6s}] {k}")
    # Not a gap: the bundles name companion knobs (clock_runaway_check, mw_wl.ewl_enable, ...)
    # whose own ledger verdict is not RECOMMEND_*.  That is the point of a coupled bundle.
    for k in extra:
        print(f"  named in loader as a bundle member only   {k}")
    exempt = sorted(norm(k["knob"]) for k in led["knobs"]
                    if k["verdict"] in RECOMMENDED and str(k.get("regime", "")).strip() == "False")
    for k in exempt:
        row = next(r for r in led["knobs"] if norm(r["knob"]) == k)
        print(f"  exempt (not a posture row)  {k}: {row.get('posture_why', '')[:90]}")
    no_regime = sorted(norm(k["knob"]) for k in led["knobs"]
                       if k["verdict"] in RECOMMENDED and not k.get("regime"))
    for k in no_regime:
        print(f"  no regime predicate (reports unconditionally)  {k}")
    bad = []
    dflt_true = loader_default_true()
    for k in led["knobs"]:
        for field in ("regime", "armed_when"):
            if not k.get(field):
                continue
            try:
                _check_expr(k[field])
            except RegimeError as e:
                bad.append(f"  BAD {field}: {e}")
            for ref in re.findall(r'on\("([^"]+)"\)', k[field]):
                if ref in dflt_true:
                    bad.append(f"  INVERTED {field} on {norm(k['knob'])}: on(\"{ref}\") is "
                               f"False for an ABSENT key but that knob's loader default is "
                               f"TRUE - use val(\"{ref}\", True)")
    print("\n".join(bad))
    n = len(missing) + len(no_regime) + len(bad)
    print(f"\n{n} coverage gap(s).")
    return 1 if n else 0


# ----------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config", nargs="?")
    ap.add_argument("--ledger", default=LEDGER)
    ap.add_argument("--tier", default="on", choices=("on", "scoped", "all"))
    ap.add_argument("--show-suppressed", action="store_true",
                    help="also list knobs held back because their regime does not match")
    ap.add_argument("--no-regime", action="store_true",
                    help="disable regime filtering (the pre-Jul-2026 over-reporting behaviour)")
    ap.add_argument("--verify-loader", action="store_true",
                    help="cross-check ledger coverage against GnssConfig::logNetworkPosture_")
    a = ap.parse_args()

    # The ledger's evidence/interlock prose carries non-cp1252 characters, and a Windows
    # console defaults to cp1252 - without this the report dies with UnicodeEncodeError
    # part-way through, silently truncating the gap list.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    led = json.load(open(a.ledger, encoding="utf-8"))
    if a.verify_loader:
        return verify_loader(led)
    if not a.config:
        ap.error("a config path is required (or use --verify-loader)")

    cfg = json.load(open(a.config, encoding="utf-8"))
    env = make_env(cfg)
    print(f"# posture check: {a.config}")
    print(f"#   regime: {env['n_sta']} station(s), {env['n_sys']} constellation(s), "
          f"{cfg.get('processing',{}).get('frequency_mode','?')} frequency mode, "
          f"{'WARM-RESUME' if env['warm'] else 'cold'} start, "
          f"{'lane/WL armed' if env['lane'] else 'no lane layer'}, "
          f"{'REAL-TIME' if env['rt'] else 'post-processed'}, "
          f"mode={cfg.get('processing',{}).get('mode','?')}")
    print(f"#   ledger: {len(led['knobs'])} knobs "
          f"({sum(1 for k in led['knobs'] if k['verdict']=='RECOMMEND_ON')} ON, "
          f"{sum(1 for k in led['knobs'] if k['verdict']=='RECOMMEND_SCOPED')} SCOPED)"
          f"{'  [regime filtering DISABLED]' if a.no_regime else ''}")

    want = {"on": ("RECOMMEND_ON",),
            "scoped": ("RECOMMEND_SCOPED",),
            "all": RECOMMENDED}[a.tier]
    n_gap = n_suppressed = n_chain = 0
    for tier in want:
        rows = [k for k in led["knobs"] if k["verdict"] == tier]
        gaps, suppressed = [], []
        for k in rows:
            in_regime, pred, filtered = (True, None, False) if a.no_regime else applies(k, env)
            is_armed, found, v = armed(k, cfg, env)
            if is_armed:
                continue
            (gaps if in_regime else suppressed).append((k, found, v, pred))
        n_suppressed += len(suppressed)
        print(f"\n=== {tier}: {len(gaps)} of {len(rows)} not armed and IN REGIME"
              f"{f' ({len(suppressed)} suppressed as out-of-regime)' if suppressed else ''} ===")
        for k, found, v, pred in gaps:
            n_gap += 1
            state = "ABSENT (uses code default)" if not found else f"= {v!r}"
            print(f"  {k['knob']}  {state}")
            if k.get("posture_why"):
                print(f"      why: {k['posture_why']}")
            # CHAIN axis - mirrors the loader.  On a warm resume a fingerprinted knob cannot
            # be adopted mid-lineage, so the recommendation is NOT actionable on this config.
            if env["warm"] and k.get("fingerprinted"):
                n_chain += 1
                require = resolve(cfg, "network.state_snapshot_require")[1]
                print(f"      ^ CHAIN: fingerprinted - arming it on this resumed leg "
                      f"{'hard-aborts (state_snapshot_require on)' if require else 'SILENTLY cold-starts (state_snapshot_require is FALSE)'}"
                      f".  Arm it in a FRESH lineage (new seed), not here.")
            print(f"      default at {k['default_at']}")
            print(f"      scope: {k['scope']}")
            if k.get("interlock"):
                print(f"      interlock: {k['interlock']}")
        if a.show_suppressed:
            for k, found, v, pred in suppressed:
                print(f"  [out of regime] {k['knob']}  (needs: {pred})")
    print(f"\n{n_gap} in-regime gap(s); {n_suppressed} suppressed as out-of-regime"
          f"{' (--show-suppressed to list)' if n_suppressed and not a.show_suppressed else ''}."
          f"\nRead the scope before arming - see the ledger's SCOPED tier and "
          f"docs/CONFIG_promotion_ledger.md.")
    if n_chain:
        require = resolve(cfg, "network.state_snapshot_require")[1]
        print(f"\n*** CHAIN: {n_chain} fingerprinted gap(s) on a warm-resume config, "
              f"not armable on this leg - they need a fresh lineage."
              + ("  network.state_snapshot_require is FALSE, which is exactly the setting that "
                 "makes a mistaken flip fail SILENTLY (it reads as a clean resume).  Arm "
                 "state_snapshot_require first." if not require else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
