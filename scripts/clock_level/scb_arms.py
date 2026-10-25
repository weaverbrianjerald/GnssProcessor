#!/usr/bin/env python3
"""scb_arms.py (card SCB): the arms of the satellite-clock-error bed.

The sim golden is re-pointed to a fixture (its 61 RINEX paths and nothing else, plus the
start offset every sim arm carries) and written once per fixture as a base; every arm is
then `scripts/rapid_arm.py make` on that base with the posture's keys, so the arm record
(rapid.json) carries the base hash and the key list as for any other arm.

    python scripts/clock_level/scb_arms.py bases
    python scripts/clock_level/scb_arms.py make <g|gg|c|n> <q|k> [--hours 6] [--smoke]
    python scripts/clock_level/scb_arms.py keys <g|gg|c|n>

Fixtures: q = runs/simnet60_quiet_col_24h (the golden's), k = the clock-error sibling
(config/sim/simnet60_clk_col_24h.json). Postures (card SCB, section STATUS): g the golden as
armed; gg GPS only; c = gg + the self-contained regime on the COD a priori (card SCL2B);
n = c + the native broadcast-clock a priori of card NBP (no CLK file in its data root).
RAPID_RUNS_ROOT is set to D:/GnssProcessorSupport/runs_nom unless the environment sets it.
"""
import argparse
import copy
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, "/")
GOLDEN = ROOT + "/config/golden/golden_sim_refound.json"
FIXTURE = {
    "q": "C:/GnssProcessor/runs/simnet60_quiet_col_24h",
    "k": "D:/GnssProcessorSupport/fixtures/simnet60_clk_col_24h",
}
BASES = "D:/GnssProcessorSupport/fixtures/scb_bases"
START_OFFSET_H = 0.504167          # 00:30:15, the sim grading start (the start artifact)
NATIVE_ROOT = "C:/GnssProcessor/runs/_scl_data_nbp"   # card NBP's CLK-free root

GG = ['processing.active_systems=["GPS"]']
REGIME = GG + [
    "network.carry_coupling_clock_clone=true",
    "network.handover_stransform_dense=true",
    "network.clock_anchor_datum_keep=true",
    "network.carry_coupling_shared_history=true",
    "network.carry_coupling_shared_history_rank=1",
    "network.clock_anchor_q=5e-06",
    "network.emit_pb_hold_phase_clock=false",
    "network.emit_pb_hold_smoothed=true",
    "network.clock_level_monitor=true",       # the two monitors of the NBP arms (traces only)
    "network.phase_visible_monitor=true",
]
NATIVE = REGIME + [
    "network.apriori_source=sp3_brdc_clock",
    "network.brdc_select=ttom",
    "files.internal_data_root=" + NATIVE_ROOT,
]
POSTURE = {"g": [], "gg": GG, "c": REGIME, "n": NATIVE}


def base_path(fx):
    return f"{BASES}/golden_sim_refound_{fx}.json"


def write_bases():
    g = json.load(open(GOLDEN))
    quiet = FIXTURE["q"]
    os.makedirs(BASES, exist_ok=True)
    for fx, fdir in FIXTURE.items():
        arm = copy.deepcopy(g)
        net = arm["network"]
        n = 0
        for s in net["stations"]:
            assert s["rinex_path"].startswith(quiet + "/"), s["rinex_path"]
            s["rinex_path"] = fdir + s["rinex_path"][len(quiet):]
            n += 1
        cons = net["closed_loop"]["station"]
        assert cons["rinex_path"].startswith(quiet + "/"), cons["rinex_path"]
        cons["rinex_path"] = fdir + cons["rinex_path"][len(quiet):]
        n += 1
        assert n == 61, n
        net["start_offset_hours"] = START_OFFSET_H
        json.dump(arm, open(base_path(fx), "w"), indent=2)
        print(f"{fx}: {base_path(fx)} ({n} RINEX paths -> {fdir}, start_offset_hours {START_OFFSET_H})")


def make(posture, fx, hours, smoke):
    env = dict(os.environ)
    env.setdefault("RAPID_RUNS_ROOT", "D:/GnssProcessorSupport/runs_nom")
    tag = f"scb_{posture}_{fx}{'0' if smoke else str(int(hours))}"
    cmd = [sys.executable, ROOT + "/scripts/rapid_arm.py", "make", "--base", base_path(fx), "--tag", tag]
    if smoke:
        cmd += ["--set", "processing.max_duration_s=300", "--set", "network.max_duration_s=300"]
    else:
        cmd += ["--hours", str(hours)]
    for s in POSTURE[posture]:
        cmd += ["--set", s]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True, env=env, cwd=ROOT)
    return tag


def replay(producer_tag, wire, suffix):
    """A consumer replay of `wire` on the producer arm's own configuration (gate C3, the status
    report's T2 method): replay path set, start offset 0 so the replay begins with the wire's
    first epoch (an offset would be applied a second time by the replay driver)."""
    env = dict(os.environ)
    env.setdefault("RAPID_RUNS_ROOT", "D:/GnssProcessorSupport/runs_nom")
    base = f"{env['RAPID_RUNS_ROOT']}/_rap_{producer_tag}/arm.json"
    tag = f"{producer_tag}_{suffix}"
    cmd = [sys.executable, ROOT + "/scripts/rapid_arm.py", "make", "--base", base, "--tag", tag,
           "--set", "network.closed_loop.replay_products_path=" + wire,
           "--set", "network.start_offset_hours=0"]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True, env=env, cwd=ROOT)
    return tag


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("bases")
    m = sub.add_parser("make")
    m.add_argument("posture", choices=sorted(POSTURE))
    m.add_argument("fixture", choices=sorted(FIXTURE))
    m.add_argument("--hours", type=float, default=6.0)
    m.add_argument("--smoke", action="store_true", help="10 epochs (300 s) instead of --hours")
    r = sub.add_parser("replay")
    r.add_argument("producer_tag")
    r.add_argument("wire")
    r.add_argument("suffix")
    k = sub.add_parser("keys")
    k.add_argument("posture", choices=sorted(POSTURE))
    a = ap.parse_args()
    if a.cmd == "bases":
        write_bases()
    elif a.cmd == "make":
        make(a.posture, a.fixture, a.hours, a.smoke)
    elif a.cmd == "replay":
        replay(a.producer_tag, a.wire, a.suffix)
    else:
        print("\n".join(POSTURE[a.posture]) or "(none: the golden as armed)")


if __name__ == "__main__":
    main()
