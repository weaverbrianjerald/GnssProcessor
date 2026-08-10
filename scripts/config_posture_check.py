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

INHERITANCE.  The config is flattened through its ``"extends"`` chain before any regime
variable is derived (scripts/gnss_config_chain.py, which mirrors the loader).  Reading the
child file alone is not a near-miss, it inverts the answer: ``_golden0729_net305_3h.json`` is
a four-key delta whose roster and arm set live three files up, and unflattened it reported
"0 station(s), 0 constellation(s)" and eight ARMED knobs as gaps.  A checker that
false-positives on every inheriting config teaches people to ignore it, which reinstates
exactly the promotion gap the ledger exists to close.

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
if _HERE not in sys.path:                       # importable however the script is invoked
    sys.path.insert(0, _HERE)
from gnss_config_chain import load_config, ConfigChainError  # noqa: E402

LEDGER = os.path.join(_HERE, "..", "docs", "CONFIG_promotion_ledger.json")
# logNetworkPosture_ was split out of GnssConfigLoadNetwork.cpp into its own TU
# (GnssConfigNetworkPosture.cpp) when the network posture check landed.  Both files still
# NAME the function, but only this one DEFINES it - and every scraper below anchors on the
# definition, so pointing at the old file made --verify-loader die with "not found".
LOADER = os.path.join(_HERE, "..", "src", "core", "config", "GnssConfigNetworkPosture.cpp")

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
#  warm-resume detection
# ----------------------------------------------------------------------------------
def snapshot_state(cfg):
    """-> (warm, why).  Is this config a RESUMED leg, or the SEED of a fresh lineage?

    ``network.state_snapshot_path`` alone does not answer that.  The knob is WRITE-side in
    the forward pass (ApplicationNetwork.cpp:914-918 sets the write target unconditionally);
    a resume is only ATTEMPTED, and only succeeds, when a readable snapshot already exists
    there.  Every lineage therefore begins with a leg that has the path set and no file -
    a cold seed that a path-only test mislabels as a resume.

    The mislabel is not cosmetic.  `warm` gates which ledger rows are in regime
    (``state_snapshot_require`` applies only when warm; ``code_bias_disc_jump_test`` only
    when NOT warm), and it drives the CHAIN annotation - which on a seed tells you that
    arming a fingerprinted knob "SILENTLY cold-starts" the leg, when a fresh lineage is
    precisely where the standard armed set says to arm it.

    DIVERGENCE, deliberate: the loader's own posture readout tests the path only
    (GnssConfigLoadNetwork.cpp:3838, ``warm = !nc.state_snapshot_path.empty()``), so it
    labels a fresh seed "WARM-RESUME start".  runs/golden3h_net305 is that case on record -
    it logged WARM-RESUME and then wrote six snapshots without ever resuming one.  This
    check is the stricter of the two; where they differ, the loader is over-reporting.

    Ambiguity resolves toward WARM on purpose.  A false COLD would suppress the
    ``state_snapshot_require`` ON-tier row on a genuinely resumed leg, and that row is the
    one guarding invariant #4's worst failure - a silent cold start that reads as a clean
    resume.  A false WARM only over-reports.
    """
    found, p = resolve(cfg, "network.state_snapshot_path")
    p = (p or "").strip() if isinstance(p, str) else ""
    if not found or not p:
        return False, "no network.state_snapshot_path (cold by construction)"
    if os.path.isabs(p):
        if os.path.exists(p):
            return True, f"snapshot present at {p}"
        return False, f"state_snapshot_path set but NO snapshot at {p} - this is a fresh " \
                      f"lineage SEED, not a resumed leg"
    # A relative path is resolved by the driver against log_dir_, which is "logs" relative
    # to the process working directory (Application.hpp:844) - i.e. it depends on where the
    # exe will be launched, which is not knowable from the config.  Probe both readings;
    # if neither hits, we cannot prove it is a seed, so fall to the safe side.
    for cand in (os.path.join("logs", p), p):
        if os.path.exists(cand):
            return True, f"snapshot present at {cand} (relative path, cwd-resolved)"
    return True, (f"relative state_snapshot_path {p!r} and no snapshot under ./logs or . - "
                  f"the driver resolves it against its own launch directory, so this cannot "
                  f"be decided offline; assuming WARM (the over-reporting side)")


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
        # NOT a path-set test - see snapshot_state() for why a seed leg has the path set
        # and is still cold.
        "warm":  snapshot_state(cfg)[0],
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
def _posture_body(path):
    """Source text of the whole posture TU, from its first function to EOF.

    THE TRAP THIS EXISTS FOR: the items table used to live INSIDE logNetworkPosture_, so both
    scrapers anchored on that function and read to the next `void GnssConfig::`.  The table was
    later split into appendNetworkPostureItemsOn_ / appendNetworkPostureItemsScoped_, which sit
    ABOVE logNetworkPosture_ in the file - so the old anchor started reading AFTER every row and
    found zero of them, reporting all 65 recommended knobs as "MISSING from loader" (i.e. a
    total-coverage-loss readout that looks exactly like a catastrophic ledger regression).

    Anchoring on the FIRST posture function instead of a named one keeps this working across
    further decomposition.  It deliberately starts at the first function rather than at byte 0:
    the file-header Doxygen quotes knob paths in prose, and counting those as loader coverage
    would silently mask real gaps in the forward check.
    """
    src = open(path, encoding="utf-8", errors="replace").read()
    starts = [m.start() for m in
              re.finditer(r"void GnssConfig::(?:appendNetworkPostureItems\w*_|logNetworkPosture_)",
                          src)]
    if not starts:
        raise SystemExit(f"no GnssConfig posture function found in {path}")
    return src[min(starts):]


def loader_knobs(path=LOADER):
    """Knob paths named in the posture TU (items table + bundles)."""
    body = _posture_body(path)
    out = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        head = lit.split(" ")[0].rstrip(".,;:")
        if re.fullmatch(r"(network|estimation|processing|filter)\.[A-Za-z0-9_.]+", head):
            out.add(head)
    return out


def loader_item_rows(path=LOADER):
    """-> {knob: tier} for the ITEMS TABLE rows of logNetworkPosture_ (not bundle members).

    ``loader_knobs`` scrapes every knob-shaped string literal in the function, so it cannot
    tell a table ROW from a knob merely NAMED in a bundle's member list.  That distinction
    is what makes the reverse coverage direction checkable: a row the loader counts and the
    ledger has no entry for is a silent tally divergence between the run-time POSTURE block
    and this script - which is exactly how network.trace_phase_anchor hid, showing up in the
    forward check as an innocuous "bundle member only".

    The row shape is `{ <applies>, <on>, Tier::X, <fp>, "knob", "why" }`, so anchoring on
    `Tier::X, <bool>, "knob"` picks out rows and nothing else.
    """
    body = _posture_body(path)
    return {knob: tier for tier, knob in
            re.findall(r'Tier::(ON|SCOPED)\s*,\s*(?:true|false)\s*,\s*"([^"]+)"', body)}


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
    # REVERSE direction: a loader items-table ROW with no RECOMMEND_* ledger entry.  The
    # loader counts it in its POSTURE tally and this script cannot, so the two readouts
    # disagree by exactly that row on every config where it is in regime - a divergence
    # with no other symptom.  Distinct from a bundle member, which is named but not counted.
    rows = loader_item_rows(loader_path)
    orphan_rows = sorted(k for k in rows if k not in want)
    for k in orphan_rows:
        print(f"  MISSING from ledger  [{rows[k]:6s}] {k} — a loader POSTURE ROW with no "
              f"RECOMMEND_* entry; this script's tally will read one short of the run's")
    # Not a gap: the bundles name companion knobs (clock_runaway_check, mw_wl.ewl_enable, ...)
    # whose own ledger verdict is not RECOMMEND_*.  That is the point of a coupled bundle.
    for k in extra:
        if k in orphan_rows:
            continue                      # already reported above, as the stronger finding
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
    n = len(missing) + len(no_regime) + len(bad) + len(orphan_rows)
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

    # Flatten "extends" FIRST, exactly as the loader does before any section loader runs -
    # every regime variable below is derived from the merged document, never the delta.
    try:
        cfg, chain = load_config(a.config)
    except ConfigChainError as e:
        print(f"config chain error: {e}", file=sys.stderr)
        return 2
    if not isinstance(cfg, dict):
        # config/test_suite/ also holds bare station-roster ARRAYS
        # (global_network_stations*.json).  Say so instead of dying in make_env with an
        # AttributeError three frames down.
        print(f"not a config: {a.config} parses as a top-level "
              f"{type(cfg).__name__}, not an object.  (Roster files such as "
              f"global_network_stations.json are data, not configs - point this at the "
              f"config that REFERENCES the roster.)", file=sys.stderr)
        return 2
    env = make_env(cfg)
    warm, warm_why = snapshot_state(cfg)
    print(f"# posture check: {a.config}")
    if chain:
        print(f"#   extends: {' -> '.join(os.path.basename(c) for c in chain)}  "
              f"({len(chain)} base(s), merged base-first, child wins)")
    print(f"#   regime: {env['n_sta']} station(s), {env['n_sys']} constellation(s), "
          f"{cfg.get('processing',{}).get('frequency_mode','?')} frequency mode, "
          f"{'WARM-RESUME' if env['warm'] else 'cold'} start, "
          f"{'lane/WL armed' if env['lane'] else 'no lane layer'}, "
          f"{'REAL-TIME' if env['rt'] else 'post-processed'}, "
          f"mode={cfg.get('processing',{}).get('mode','?')}")
    print(f"#   chain state: {warm_why}")
    print(f"#   ledger: {len(led['knobs'])} knobs "
          f"({sum(1 for k in led['knobs'] if k['verdict']=='RECOMMEND_ON')} ON, "
          f"{sum(1 for k in led['knobs'] if k['verdict']=='RECOMMEND_SCOPED')} SCOPED)"
          f"{'  [regime filtering DISABLED]' if a.no_regime else ''}")

    want = {"on": ("RECOMMEND_ON",),
            "scoped": ("RECOMMEND_SCOPED",),
            "all": RECOMMENDED}[a.tier]
    n_gap = n_suppressed = n_chain = 0
    tallies = []
    for tier in want:
        rows = [k for k in led["knobs"] if k["verdict"] == tier]
        gaps, suppressed, n_armed = [], [], 0
        for k in rows:
            in_regime, pred, filtered = (True, None, False) if a.no_regime else applies(k, env)
            is_armed, found, v = armed(k, cfg, env)
            # Classify BEFORE the armed short-circuit: an armed IN-REGIME row is what the
            # loader's `on_armed` counts, and without counting it here there is no
            # denominator to compare the two readouts on.  Only NOT-armed out-of-regime
            # rows are worth listing under --show-suppressed, so that list is unchanged.
            if not in_regime:
                if not is_armed:
                    suppressed.append((k, found, v, pred))
                continue
            if is_armed:
                n_armed += 1
                continue
            gaps.append((k, found, v, pred))
        n_suppressed += len(suppressed)
        tallies.append((tier, n_armed, n_armed + len(gaps)))
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
    # The one line that is directly comparable, term for term, against the run-time
    # "=== Network POSTURE: ON tier X/Y armed, SCOPED tier A/B armed" in
    # logs/gnss_config.log.  Same regime filter, same armed tests, so a mismatch means
    # this checker and GnssConfig::logNetworkPosture_ have drifted - which is the only
    # cheap way to notice that they have.
    if tallies:
        print("\n" + ", ".join(f"{t[10:]} tier {a_}/{n} armed" for t, a_, n in tallies)
              + "   (armed/in-regime - compare against the run's "
                "'Network POSTURE: ... armed' line)")
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
