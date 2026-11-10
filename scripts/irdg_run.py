#!/usr/bin/env python3
"""Arms of card IRDG (docs/record/refound/IRDG_floor_off_rpb_in_block_2026-11-09.md): the table of its arms,
made through scripts/ird_make_arms.py and scripts/rapid_arm.py.

    python scripts/irdg_run.py list [--group G ...]
    python scripts/irdg_run.py make --group G [...] [--dry-run]
    python scripts/irdg_run.py tags --group G [...]          the tags of the groups, one line (for rapid_arm.py run)
    python scripts/irdg_run.py replays --wire TAG [--dry-run]  the eight held-out replays of one held-out wire
    python scripts/irdg_run.py queue --file Q [--jobs 2]     run the arms a queue file lists, two at a time
    python scripts/irdg_run.py --selftest

A tag is irdg_<arm>_<bed>.  Arm codes: f0 the floor-off arm on the receiver phase bias's time link as built
(A1krhRsm_link), fl its floor-on variant A1krhRsm_floor, r1 the no-chain reference R1, q0 the floor-off arm
with network.phase_datum_eq34_rpb_q_m2s=0 (no link), b the floor-off arm with
network.phase_datum_eq34_rpb_in_block=true (the key of card IRDG) as its one knob, ad an arm of the A1 family
as defined since the adoption of 2026-11-10 (card IRDG section 8), which carries the key itself.  The card
graded f0, q0 and b when arm A1krhRsm was still the time-link arm; A1krhRsm_link is that arm, knob for knob,
and A1krhRsm is now arm b.  Groups:

    part1    the floor-off arm on the two beds of card IRDF, the microscope, r2 and the row fixtures with
             their controls (card IRDG section 3)
    refs     the floor-on variant, the no-chain arm and the no-link arm (the references of section 4.2)
    golden   arm G on the sim and field goldens (the off-identity pairs' golden halves; run once per exe
             with --suffix)
    key      the key on every bed of part1
    wires    the held-out wires: r1, f0 and b on r3c and the golden sim without the eight held-out sites
    adopted  the seven arms of the A1 family as now defined, 0.1 h on r3c (a smoke: the loader takes each),
             and A1krhRsm for 2 h on r3c (it reproduces the key arm irdg_bx_r3c)

A queue file holds one arm per line, "<Release exe> <tag>", and may grow while the queue runs; the line
END stops the queue once every listed arm has an exit code.  An arm that already has one is skipped; an arm
is started with an empty logs directory (scripts/ev_launch.ps1, the launcher of rapid_arm.py run).

Run directories go under RAPID_RUNS_ROOT (set it to D:/GnssProcessorSupport/runs_nom).
"""
import argparse
import concurrent.futures as cf
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE).replace(os.sep, "/")
RUNS = (os.environ.get("RAPID_RUNS_ROOT") or (ROOT + "/runs")).replace("\\", "/").rstrip("/")

KEY = "network.phase_datum_eq34_rpb_in_block=true"
NO_LINK = "network.phase_datum_eq34_rpb_q_m2s=0"
# arm code -> (arm of ird_make_arms.py, the one knob under test or None)
ARM = {"f0": ("A1krhRsm_link", None), "fl": ("A1krhRsm_floor", None), "r1": ("R1", None),
       "q0": ("A1krhRsm_link", NO_LINK), "b": ("A1krhRsm_link", KEY), "g": ("G", None)}
# The A1 family of ird_make_arms.py as defined since the adoption, and the one arm of it that runs for 2 h.
ADOPTED = ("A1", "A1k", "A1kr", "A1krh", "A1krhR", "A1krhRs", "A1krhRsm")
ADOPTED_2H = "A1krhRsm"
# bed code -> bed of ird_make_arms.py
BED = {"r3c": "r3c", "gs": "golden", "gf": "field", "mic": "microscope", "r2": "r2", "r3b": "r3b",
       "rv16": "ird_rv16", "rg": "ird_rg", "rgm": "ird_rg_mic", "t1": "ird_t1", "tr": "ird_tr",
       "t1m": "ird_t1m", "trm": "ird_trm", "ms": "ird_ms", "msr": "ird_msr", "msm": "ird_ms_mic",
       "msrm": "ird_msr_mic", "msc": "ird_ms_ctl", "msrc": "ird_msr_ctl", "mscm": "ird_ms_ctl_mic",
       "msrcm": "ird_msr_ctl_mic"}
# The beds of card IRDG section 3: the two beds of card IRDF, the hold's beds, the row fixtures, the controls.
ROW_BEDS = ["r3c", "gs", "mic", "r2", "rv16", "rg", "rgm", "t1", "tr", "t1m", "trm", "ms", "msr", "msm", "msrm"]
CONTROL_BEDS = ["r3b", "msc", "msrc", "mscm", "msrcm"]
HELD_OUT = ["S04000SIM", "S11000SIM", "S18000SIM", "S25000SIM", "S32000SIM", "S39000SIM", "S46000SIM",
            "S53000SIM"]
GROUPS = {
    "part1": [("f0", b) for b in ROW_BEDS + CONTROL_BEDS],
    "refs": [("fl", "r3c"), ("fl", "gs"), ("r1", "r3c"), ("r1", "gs"), ("q0", "r3c"), ("q0", "gs"), ("q0", "rg")],
    "golden": [("g", "gs"), ("g", "gf")],
    "key": [("b", b) for b in ROW_BEDS + CONTROL_BEDS],
    "wires": [(a, b) for b in ("r3c", "gs") for a in ("r1", "f0", "b")],
    "adopted": [],                               # made by adopted_rows(): arms by their own names, not by a code
}


def tag_of(arm, bed, suffix=""):
    return "irdg_%s%s_%s" % (arm, suffix, bed)


def wire_tag(arm, bed):
    return "irdg_hw_%s_%s" % (arm, bed)


def make_cmd(arm, bed, tag, drop=()):
    name, knob = ARM[arm]
    cmd = [sys.executable, HERE + "/ird_make_arms.py", "make", "--arm", name, "--bed", BED[bed], "--tag", tag]
    if knob:
        cmd += ["--knob", knob]
    for st in drop:
        cmd += ["--drop-station", st]
    return cmd


def adopted_rows():
    """The A1 family as defined since the adoption, on r3c: each arm for 0.1 h (tag irdg_ads_<arm>_r3c: the
    loader takes the arm and its block columns fold) and A1krhRsm for 2 h (irdg_ad_a1krhrsm_r3c: the arm
    card IRDG graded as its key arm)."""
    out = []
    for name, hours in [(a, 0.1) for a in ADOPTED] + [(ADOPTED_2H, 2.0)]:
        t = "irdg_ad%s_%s_r3c" % ("s" if hours < 1.0 else "", name.lower())
        out.append((t, [sys.executable, HERE + "/ird_make_arms.py", "make", "--arm", name, "--bed", "r3c",
                        "--tag", t, "--hours", str(hours)]))
    return out


def rows(groups, suffix=""):
    out = []
    for g in groups:
        if g == "adopted":
            out += adopted_rows()
        for arm, bed in GROUPS[g]:
            if g == "wires":
                out.append((wire_tag(arm, bed), make_cmd(arm, bed, wire_tag(arm, bed), HELD_OUT)))
            else:
                t = tag_of(arm, bed, suffix)
                out.append((t, make_cmd(arm, bed, t)))
    return out


def replay_rows(wire):
    """The eight replays of a held-out wire: its arm with the consumer moved to a held-out site and the
    products read from the wire (card IRDF section 4.1).  The site's file and coordinate come from the
    full arm of the same bed (irdg_f0_<bed>), which holds every station."""
    bed = wire.rsplit("_", 1)[1]
    full = json.load(open("%s/_rap_%s/arm.json" % (RUNS, tag_of("f0", bed))))
    sta = {s["name"]: s for s in full["network"]["stations"]}
    out = []
    for name in HELD_OUT:
        t = "irdg_hr_%s_%s" % (name[:3].lower(), wire[len("irdg_hw_"):])
        s = sta[name]
        cmd = [sys.executable, HERE + "/rapid_arm.py", "make", "--base", "%s/_rap_%s/arm.json" % (RUNS, wire),
               "--tag", t,
               "--set", "network.closed_loop.replay_products_path=%s/_rap_%s/products.txt" % (RUNS, wire),
               "--set", "network.closed_loop.station.name=%s" % name,
               "--set", "network.closed_loop.station.rinex_path=%s" % s["rinex_path"],
               "--set", "network.closed_loop.station.position_ecef=%s" % json.dumps(s["position_ecef"]),
               "--set", "network.start_offset_hours=0"]
        out.append((t, cmd))
    return out


def run_rows(rws, dry):
    for t, cmd in rws:
        if os.path.exists("%s/_rap_%s/arm.json" % (RUNS, t)):
            print("exists  " + t)
            continue
        if dry:
            print("would make " + t + ": " + " ".join(cmd[2:]))
            continue
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(("made    " if r.returncode == 0 else "FAILED  ") + t)
        if r.returncode != 0:
            print(r.stdout[-600:])
            return 1
    return 0


def read_queue(path):
    """The queue file's arms, in order, and whether its END line is there."""
    arms, end = [], False
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if line == "END":
                end = True
            elif line and not line.startswith("#"):
                exe, tag = line.rsplit(None, 1)
                arms.append((exe.replace("\\", "/"), tag))
    return arms, end


def run_one(exe, tag):
    rd = "%s/_rap_%s" % (RUNS, tag)
    assert "x64-Release" in exe and os.path.exists(exe), "a Release exe is required: " + exe
    assert tag.startswith("irdg_") and os.path.exists(rd + "/arm.json"), "not an arm of this card: " + tag
    logs = rd + "/logs"
    if os.path.isdir(logs) and not os.path.islink(logs):
        shutil.rmtree(logs)                      # a re-run never reads the traces of a stopped one
    t0 = time.time()
    subprocess.run(["pwsh", "-NoProfile", "-File", HERE + "/ev_launch.ps1", "-Exe", exe,
                    "-Config", rd + "/arm.json", "-RunDir", rd], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ex = open(rd + "/exitcode.txt").read().split() if os.path.exists(rd + "/exitcode.txt") else ["?"]
    return tag, ex[0], time.time() - t0


def run_queue(path, jobs):
    """Run the queue's arms `jobs` at a time until its END line is read and nothing is left."""
    started = set()
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        live = {}
        while True:
            arms, end = read_queue(path)
            for exe, tag in arms:
                if len(live) >= jobs:
                    break
                if tag in started or os.path.exists("%s/_rap_%s/exitcode.txt" % (RUNS, tag)):
                    continue
                if not os.path.exists("%s/_rap_%s/arm.json" % (RUNS, tag)):
                    continue                     # not made yet: looked at again on the next pass
                started.add(tag)
                live[pool.submit(run_one, exe, tag)] = tag
            for f in [f for f in live if f.done()]:
                tag, ex, dt = f.result()
                print("%s  %s: exit %s, %.0f s" % (time.strftime("%H:%M:%S"), tag, ex, dt), flush=True)
                del live[f]
            pending = [t for _, t in arms if t not in started
                       and not os.path.exists("%s/_rap_%s/exitcode.txt" % (RUNS, t))]
            if end and not live and not pending:
                return 0
            time.sleep(5)


def selftest():
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    all_tags = [t for g in GROUPS for t, _ in rows([g])]
    chk(len(all_tags) == len(set(all_tags)), "every tag is made once (%d tags)" % len(all_tags))
    chk(all(t.startswith("irdg_") for t in all_tags), "every tag carries the card's prefix")
    chk([b for a, b in GROUPS["part1"]] == [b for a, b in GROUPS["key"]]
        and {a for a, _ in GROUPS["part1"]} == {"f0"} and {a for a, _ in GROUPS["key"]} == {"b"},
        "the key runs on the beds of part 1, bed for bed")
    c = make_cmd("b", "r3c", "x")
    chk(c[c.index("--arm") + 1] == "A1krhRsm_link" and c[c.index("--knob") + 1] == KEY,
        "the key arm is the floor-off time-link arm with the key as its one knob")
    c = make_cmd("q0", "rg", "x")
    chk(c[c.index("--arm") + 1] == "A1krhRsm_link" and c[c.index("--knob") + 1] == NO_LINK
        and c[c.index("--bed") + 1] == "ird_rg",
        "the no-link arm sets the time link's q to 0 on the time-link arm (the loader refuses q 0 with the key)")
    import ird_make_arms
    chk(all(ird_make_arms.compose(ARM["b"][0], BED[b], knob=KEY) == ird_make_arms.compose(ADOPTED_2H, BED[b])
            for _, b in GROUPS["key"]),
        "on every bed of the card, arm A1krhRsm as now defined is the key arm b, knob for knob")
    ad = adopted_rows()
    chk(len(ad) == len(ADOPTED) + 1 and [cmd[cmd.index("--hours") + 1] for _, cmd in ad] == ["0.1"] * len(ADOPTED) + ["2.0"]
        and ad[-1][0] == "irdg_ad_a1krhrsm_r3c" and len({t for t, _ in ad}) == len(ad)
        and all("--knob" not in cmd for _, cmd in ad),
        "the adopted group smokes each arm of the A1 family and runs A1krhRsm for 2 h, no knob on any")
    w = rows(["wires"])
    chk(len(w) == 6 and all(cmd.count("--drop-station") == 8 for _, cmd in w),
        "six held-out wires, each without the eight held-out sites")
    chk(tag_of("g", "gs", "base") == "irdg_gbase_gs", "a suffix names the exe of a golden half")
    print("irdg_run selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("list", "make", "tags"):
        p = sub.add_parser(name)
        p.add_argument("--group", action="append", choices=sorted(GROUPS))
        p.add_argument("--suffix", default="", help="appended to the arm code (base, cand: the exe of a pair)")
        p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("queue")
    p.add_argument("--file", required=True)
    p.add_argument("--jobs", type=int, default=2)
    p = sub.add_parser("replays")
    p.add_argument("--wire", required=True)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--tags", action="store_true", help="print the replays' tags only")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd == "queue":
        return run_queue(a.file, a.jobs)
    if a.cmd == "replays":
        rws = replay_rows(a.wire)
        if a.tags:
            print(" ".join(t for t, _ in rws))
            return 0
        return run_rows(rws, a.dry_run)
    if a.cmd in ("list", "make", "tags"):
        rws = rows(a.group or sorted(GROUPS), a.suffix)
        if a.cmd == "tags":
            print(" ".join(t for t, _ in rws))
            return 0
        if a.cmd == "list":
            for t, cmd in rws:
                print("%-22s %s" % (t, " ".join(cmd[3:])))
            return 0
        return run_rows(rws, a.dry_run)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
