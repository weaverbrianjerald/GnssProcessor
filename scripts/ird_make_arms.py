#!/usr/bin/env python3
"""Arms of card IRD (sections 6 and 11): the golden pinned by sha256, plus the bed, plus the
bundle, plus at most one knob under test, made through scripts/rapid_arm.py.

    python scripts/ird_make_arms.py list
    python scripts/ird_make_arms.py check                       which arms the loader can run
    python scripts/ird_make_arms.py make --arm A0 --bed r3c --tag ird2_a0_r3c [--hours 2]
        [--knob key=value] [--drop-station NAME ...] [--dry-run]
    python scripts/ird_make_arms.py --selftest

The base of every arm is config/golden/golden_{sim,field}_refound.json at main 3524acb, read
with `git show` and refused unless its sha256 is the pinned one (the working tree may hold
another card's golden edits; card IRD's base-drift rule re-bases at most once, before IRD-2).
A sim bed re-points every station's and the consumer's RINEX at the bed's fixture; the golden
sim bed keeps the 24 h fixture and starts at 00:30:15 (start_offset_hours 0.504167).  The base
file is written to runs/_ird_bases/, rapid_arm.py makes the arm from it, and the arm's
rapid.json gains an "ird" block (golden revision and sha256, bed, arm, knob sets).

An arm is refused when it sets a key the config loader does not read (a quoted literal in
src/core/config/*.cpp): the loader ignores an unknown key without a word, so such an arm would
silently be its base.  The IRD-1 knobs (R1's traces) are therefore refused until IRD-1 lands.
"""
import argparse
import glob
import hashlib
import io
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace(os.sep, "/")
RUNS = ROOT + "/runs"
GOLDEN_REV = "3524acb"
GOLDEN = {
    "sim": ("config/golden/golden_sim_refound.json",
            "9080e7ecaf57200e8c18de1bd638d339ea1407931362e6e072282e240c69e57e"),
    "field": ("config/golden/golden_field_refound.json",
              "cb08c38210b634637bc1b0cd8c517224b36e5b896102f845e7534a81811481fc"),
}
SIM_START_H = "0.504167"

# Card IRD section 6: bundle A0 (existing keys), the ARL section 13 slip set, the search.
CHAIN_ON = ["network.phase_datum_eq34=true", "network.phase_datum_eq34_carry_shift=true",
            "network.carry_rebase_complete=true"]
HELD_OFF = ["network.phase_datum_prioritized=false", "network.phase_datum_held_basis=false",
            "network.phase_datum_held_mark_cold=false",
            "network.phase_datum_held_cold_release=false",
            "network.phase_datum_handover_continuous=false",
            "network.phase_anchor_carry_reref=false", "network.phase_datum_gate_drop_erase=false",
            "network.phase_datum_min_stations=1"]
FLOAT_REPIN_OFF = ["network.phase_datum_eq34_seed_round=false",
                   "network.phase_datum_eq34_seed_round_rehost_frame=false",
                   "network.phase_datum_time_linked=false", "network.eq34_fresh_pin_ramp_ep=0",
                   "network.eq34_fresh_pin_sigma_scale=1.0"]
SLIP_SET = ["network.cycle_slip.gf_arc_jump=true", "network.cycle_slip.gf_arc_jump_k=6",
            "network.cycle_slip.relevel_free_epoch=true", "network.cycle_slip.if_phase_td=true",
            "network.cycle_slip.if_phase_td_reseed_on_flag=true",
            "network.ambiguity_resolution.suspect_epochs=10"]
SEARCH_LOG = ["network.ambiguity_resolution.%s=true" % k for k in
              ("enable", "tree_basis", "decorrelated_partial", "block_search", "cycle_basis",
               "trace", "trace_lattice")]
IRD_TRACES = ["network.trace_chain_datum=true", "network.trace_pb_prehold=true",
              "network.trace_rpb_states=true"]
BUNDLE_A0 = CHAIN_ON + HELD_OFF + FLOAT_REPIN_OFF + SLIP_SET + SEARCH_LOG
MICROSCOPE = ["network.sim_zero_iono_sigma_m=0.001"]

# name -> (knob list on top of the base, what it is)
ARMS = {
    "R1": (SLIP_SET + SEARCH_LOG + IRD_TRACES, "reference: golden + slip set + search + traces"),
    "A0": (SLIP_SET + SEARCH_LOG + IRD_TRACES + BUNDLE_A0, "R1 + bundle A0"),
    "A0h": (SLIP_SET + SEARCH_LOG + IRD_TRACES + BUNDLE_A0
            + ["network.emit_pb_hold=false", "network.emit_pb_hold_phase_clock=false"],
            "A0 with both emission holds off"),
    "A0f": (SLIP_SET + SEARCH_LOG + IRD_TRACES + BUNDLE_A0
            + ["network.phase_datum_eq34_rpb_prior_sigma_m=0"], "A0 with the RPB floor off"),
    "eq34": (["network.phase_datum_eq34=true", "network.phase_datum_eq34_carry_shift=true"],
             "card PRM's eq34 posture (scripts/prm_ab.py), the census arms"),
    "eq34c": (["network.phase_datum_eq34=true", "network.phase_datum_eq34_carry_shift=true",
               "network.carry_rebase_complete=true"], "card PRM's eq34c posture"),
}
# name -> (golden, fixture dir or None, extra knobs, what it is)
BEDS = {
    "golden": ("sim", None, ["network.start_offset_hours=" + SIM_START_H],
               "the golden sim bed, graded from 00:30:15"),
    "microscope": ("sim", "runs/simarl_r1c", MICROSCOPE,
                   "ARL r1c (receiver phase biases) with zero-ionosphere rows"),
    "r1c": ("sim", "runs/simarl_r1c", [], "ARL rung 1c"),
    "r2": ("sim", "runs/simarl_r2", [], "ARL rung 2 (ionosphere)"),
    "r3b": ("sim", "runs/simarl_r3b", [], "ARL rung 3b (the slip-ladder control)"),
    "r3c": ("sim", "runs/simarl_r3c", [], "ARL rung 3c (troposphere, phase colour)"),
    "field": ("field", None, [], "the field golden (no ON arm before IRD-6, invariant 1)"),
}


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def golden_bytes(which, rev=GOLDEN_REV):
    """The golden at `rev`, refused unless its sha256 is the pinned one."""
    path, pin = GOLDEN[which]
    b = subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (rev, path)], check=True,
                       stdout=subprocess.PIPE).stdout
    if sha256_bytes(b) != pin:
        raise SystemExit("golden %s at %s has sha256 %s, not the pinned %s"
                         % (path, rev, sha256_bytes(b), pin))
    return b


def repoint(cfg, fixture):
    """Point every station's and the consumer's RINEX at `fixture` (one file per station dir)."""
    net = cfg["network"]
    nodes = list(net["stations"])
    cons = net.get("closed_loop", {}).get("station")
    if cons:
        nodes.append(cons)
    for node in nodes:
        files = sorted(glob.glob("%s/%s/%s_R_*.rnx" % (fixture, node["name"], node["name"])))
        if len(files) != 1:
            raise SystemExit("bed %s: %d RINEX files for %s" % (fixture, len(files), node["name"]))
        node["rinex_path"] = files[0].replace(os.sep, "/")
    return cfg


def loader_keys():
    src = "".join(io.open(f, encoding="utf-8", errors="replace").read()
                  for f in glob.glob(ROOT + "/src/core/config/*.cpp"))
    return src


def unread(sets, src=None):
    """The keys of `sets` the loader does not read."""
    src = loader_keys() if src is None else src
    return sorted({kv.split("=", 1)[0] for kv in sets
                   if '"%s"' % kv.split("=", 1)[0] not in src})


def compose(arm, bed, knob=None):
    """The --set list of an arm on a bed: the bed's knobs, the arm's, then the knob under test.
    A key may appear once only, except that the knob under test overrides the arm (it is the
    one change)."""
    sets = list(BEDS[bed][2]) + list(ARMS[arm][0])
    keys = [kv.split("=", 1)[0] for kv in sets]
    dup = sorted({k for k in keys if keys.count(k) > 1})
    if dup:
        # The same key with the same value from two lists (the slip set inside the bundle) is
        # one setting; a conflicting value is a composition error.
        vals = {}
        for kv in sets:
            k, v = kv.split("=", 1)
            if k in vals and vals[k] != v:
                raise SystemExit("arm %s on bed %s sets %s twice with different values" % (arm, bed, k))
            vals[k] = v
        sets = ["%s=%s" % kv for kv in vals.items()]
    if knob:
        k = knob.split("=", 1)[0]
        sets = [kv for kv in sets if kv.split("=", 1)[0] != k] + [knob]
    return sets


def make(a):
    which, fixture, _, _ = BEDS[a.bed]
    sets = compose(a.arm, a.bed, a.knob)
    missing = unread(sets)
    if missing:
        raise SystemExit("refused: the loader does not read %s (an arm setting them would "
                         "silently be its base)" % ", ".join(missing))
    if which == "field":
        raise SystemExit("refused: no new field arm before IRD-6 has passed (card IRD sections 6 "
                         "and 11, invariant 1); the field census arms already exist")
    base = json.loads(golden_bytes(which))
    if fixture:
        base = repoint(base, ROOT + "/" + fixture)
    base_path = "%s/_ird_bases/%s_%s.json" % (RUNS, a.bed, GOLDEN_REV)
    if a.dry_run:
        print("arm %s on bed %s: base %s (golden %s sha256 %s)" % (a.arm, a.bed, base_path,
                                                                    GOLDEN_REV, GOLDEN[which][1]))
        for kv in sets:
            print("  --set " + kv)
        return 0
    os.makedirs(RUNS + "/_ird_bases", exist_ok=True)
    with open(base_path, "w", encoding="utf-8") as fh:
        json.dump(base, fh, indent=1)
    cmd = [sys.executable, ROOT + "/scripts/rapid_arm.py", "make", "--base", base_path,
           "--tag", a.tag, "--hours", str(a.hours)]
    for kv in sets:
        cmd += ["--set", kv]
    for st in a.drop_station or []:
        cmd += ["--drop-station", st]
    subprocess.run(cmd, check=True)
    rj = "%s/_rap_%s/rapid.json" % (RUNS, a.tag)
    prov = json.load(open(rj))
    prov["ird"] = {"golden_rev": GOLDEN_REV, "golden_path": GOLDEN[which][0],
                   "golden_sha256": GOLDEN[which][1], "bed": a.bed, "fixture": fixture,
                   "arm": a.arm, "knob_under_test": a.knob, "sets": sets}
    with open(rj, "w") as fh:
        json.dump(prov, fh, indent=2)
    return 0


def cmd_list(_):
    for name, (sets, what) in ARMS.items():
        print("%-6s %-55s %d knob(s)" % (name, what, len(sets)))
    for name, (which, fx, extra, what) in BEDS.items():
        print("bed %-11s %-6s %-20s %s%s" % (name, which, fx or "-", what,
                                             " (+%s)" % ", ".join(extra) if extra else ""))
    return 0


def cmd_check(_):
    src = loader_keys()
    for name in ARMS:
        miss = unread(compose(name, "golden"), src)
        print("%-6s %s" % (name, "makeable" if not miss else "refused until the loader reads "
                           + ", ".join(miss)))
    return 0


def selftest():
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    s = compose("A0", "golden")
    keys = [kv.split("=", 1)[0] for kv in s]
    chk(len(keys) == len(set(keys)), "A0 on the golden bed sets each key once (%d keys)" % len(keys))
    chk("network.start_offset_hours=" + SIM_START_H in s, "the golden sim bed starts at 00:30:15")
    chk(set(BUNDLE_A0) <= set(s) and set(SLIP_SET) <= set(s), "A0 carries the bundle and the slip set")
    r1 = compose("R1", "r3c")
    chk(not any(kv.startswith("network.phase_datum_eq34") for kv in r1),
        "R1 carries no chain key (it isolates the datum)")
    f = compose("A0f", "microscope", knob="network.phase_datum_eq34_rpb_q_m2s=0")
    chk("network.sim_zero_iono_sigma_m=0.001" in f and f[-1] == "network.phase_datum_eq34_rpb_q_m2s=0",
        "the microscope bed adds the zero-ionosphere rows; the knob under test is last")
    o = compose("A0", "r2", knob="network.phase_datum_min_stations=2")
    chk(o.count("network.phase_datum_min_stations=2") == 1
        and "network.phase_datum_min_stations=1" not in o, "the knob under test overrides the arm")
    src = '"network.phase_datum_eq34" "network.cycle_slip.gf_arc_jump"'
    chk(unread(["network.phase_datum_eq34=true", "network.trace_chain_datum=true"], src)
        == ["network.trace_chain_datum"], "a key the loader does not read is refused")
    try:
        ARMS["_bad"] = (["network.phase_datum_eq34=true", "network.phase_datum_eq34=false"], "")
        compose("_bad", "golden")
        chk(False, "a conflicting duplicate is refused")
    except SystemExit:
        chk(True, "a conflicting duplicate is refused")
    finally:
        ARMS.pop("_bad", None)
    chk(all(len(v[1]) == 64 for v in GOLDEN.values()), "both goldens pinned by a full sha256")
    try:
        b = golden_bytes("sim")
        chk(sha256_bytes(b) == GOLDEN["sim"][1], "the sim golden at %s matches its pin" % GOLDEN_REV)
    except (subprocess.CalledProcessError, OSError) as e:
        chk(False, "git show of the golden at %s: %s" % (GOLDEN_REV, e))
    try:
        GOLDEN["_x"] = (GOLDEN["sim"][0], "0" * 64)
        golden_bytes("_x")
        chk(False, "a golden that does not match its pin is refused")
    except SystemExit:
        chk(True, "a golden that does not match its pin is refused")
    finally:
        GOLDEN.pop("_x", None)
    print("ird_make_arms selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("list")
    sub.add_parser("check")
    m = sub.add_parser("make")
    m.add_argument("--arm", required=True, choices=sorted(ARMS))
    m.add_argument("--bed", required=True, choices=sorted(BEDS))
    m.add_argument("--tag", required=True)
    m.add_argument("--hours", type=float, default=2.0)
    m.add_argument("--knob", default=None, help="the one knob under test, key=value")
    m.add_argument("--drop-station", action="append")
    m.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd == "list":
        return cmd_list(a)
    if a.cmd == "check":
        return cmd_check(a)
    if a.cmd == "make":
        return make(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
