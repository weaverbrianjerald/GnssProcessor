#!/usr/bin/env python3
"""Arms for the multi-satellite injection check of the consumer innovation screen.

An undeclared step on several satellites at once is added to a recorded product wire
(scripts/inject_wire_step.py) and the wire is replayed by a consumer with the screen off and on.
This tool lays out the registered cells, ranks the satellites to step, writes the perturbed wires
and builds the replay arms (scripts/rapid_arm.py make).  scripts/nis_inject_grade.py grades them.

    python scripts/nis_inject_arms.py cells [--set core|ext|all] [--consumer wtzz ...]
    python scripts/nis_inject_arms.py sats --consumer wtzz [--force]
    python scripts/nis_inject_arms.py wires [--consumer wtzz ...] [--set core|ext|all]
    python scripts/nis_inject_arms.py make [--consumer ...] [--set ...] [--screen off|on|both]
                                           [--null-only] [--smoke] [--eigen-threads N] [--skip-started] [--jobs N]
    python scripts/nis_inject_arms.py tags [--consumer ...] [--set ...] [--screen ...] [--null-only]
    python scripts/nis_inject_arms.py status [--consumer ...] [--set ...]
    python scripts/nis_inject_arms.py selftest

Cells.  A cell is (consumer, class, K, step): class `pb` steps every phase-bias record of K satellites,
class `clk` the clock record of K satellites, both from TOW 270000 (hour 4 of the 6 h wires) and
persistent to the end.  Per consumer there is also the unperturbed cell (class `null`).  Every cell is
replayed twice, screen off and screen on (mode act, k 6, the default floors), as the tags
`nis_<consumer>_<class><K>_s<step>_<off|on>` under the runs root (RAPID_RUNS_ROOT, default
D:/GnssProcessorSupport/runs_nis).

  core   steps 10 and 92.409 m, K 1, 3 and 6, both classes, on the four consumers
  ext    step 2 m, same K and classes, on the primary consumer
  all    both

Consumers.  wtzz is the field wire's own held-out consumer WTZZ00DEU, arht and quin the two census sites
ARHT00ATA and QUIN00USA on the same wire (the consumer settings of scripts/clock_level/gcensus.py), szz the
sim wire's own consumer SZZ000SIM.  The wires are the baseline pair runs/_rf0hv17_{field,sim}6.

Satellites.  `sats` ranks the satellites of the unperturbed screen-off replay's closed_loop_sat_trace.csv
at TOW 270000 by elevation (highest first, ties by satellite id) and keeps the first six; the K satellites of a
cell are the first K of that list, so K 1 is inside K 3 inside K 6.  The list is written once per consumer
(sats_<consumer>.json beside the wires) and a later call only checks it, so the choice cannot follow a result.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE).replace("\\", "/")
sys.path.insert(0, HERE)
import inject_wire_step  # noqa: E402

RUNS = (os.environ.get("RAPID_RUNS_ROOT") or "D:/GnssProcessorSupport/runs_nis").replace("\\", "/").rstrip("/")
SCRATCH = (os.environ.get("NIS_SCRATCH") or "D:/GnssProcessorSupport/gates/nisclose").replace("\\", "/").rstrip("/")
BEDS = {"field": "C:/GnssProcessor/runs/_rf0hv17_field6", "sim": "C:/GnssProcessor/runs/_rf0hv17_sim6"}
T_INJ = 270000.0                                   # first stepped epoch (hour 4 of the 6 h wires)
CONSUMERS = {                                      # key -> (bed, census site or None for the wire's own consumer)
    "wtzz": ("field", None), "arht": ("field", "ARHT00ATA"), "quin": ("field", "QUIN00USA"), "szz": ("sim", None)}
PRIMARY = "wtzz"
CLASSES = ("pb", "clk")
KS = (1, 3, 6)
STEPS = {"2": 2.0, "10": 10.0, "92p409": 92.409}   # label -> metres
CORE_STEPS = ("10", "92p409")
EXT_STEPS = ("2",)
N_RANK = 6                                         # satellites kept by the ranking
SCREEN_KEY = "network.closed_loop.innovation_screen"


class Cell:
    """One (consumer, class, K, step) combination; class `null` is the unperturbed replay."""

    def __init__(self, consumer, cls, k=0, step=""):
        self.consumer, self.cls, self.k, self.step = consumer, cls, k, step

    @property
    def key(self):
        return "%s_null" % self.consumer if self.cls == "null" else "%s_%s%d_s%s" % (self.consumer, self.cls, self.k, self.step)

    def tag(self, screen):
        return "nis_%s_%s" % (self.key, screen)

    @property
    def step_m(self):
        return STEPS[self.step]

    @property
    def bed(self):
        return CONSUMERS[self.consumer][0]

    def wire(self):
        if self.cls == "null":
            return BEDS[self.bed] + "/products.txt"
        return "%s/wires/%s.txt" % (SCRATCH, self.key)

    def __repr__(self):
        return "Cell(%s)" % self.key


def cells(which="all", consumers=None, with_null=True):
    """The registered cells in a fixed order: per consumer the null, then class, K and step."""
    out = []
    for consumer in (consumers or CONSUMERS):
        if with_null:
            out.append(Cell(consumer, "null"))
        steps = []
        if which in ("core", "all"):
            steps += list(CORE_STEPS)
        if which in ("ext", "all") and consumer == PRIMARY:
            steps += list(EXT_STEPS)
        for step in steps:
            for cls in CLASSES:
                for k in KS:
                    out.append(Cell(consumer, cls, k, step))
    return out


def rank_sats(trace_csv, tow=T_INJ):
    """[(satellite, elevation_deg)] at `tow` from a closed_loop_sat_trace.csv, highest elevation first, ties by id."""
    elev = {}
    with open(trace_csv, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                if abs(float(r["tow"]) - tow) < 0.5 and r["elev_deg"] != "":
                    elev[r["sat"]] = float(r["elev_deg"])
            except ValueError:
                continue
    return sorted(elev.items(), key=lambda kv: (-kv[1], kv[0]))


def sats_path(consumer):
    return "%s/sats_%s.json" % (SCRATCH, consumer)


def load_sats(consumer):
    with open(sats_path(consumer)) as fh:
        return json.load(fh)


def sats_of(cell):
    """The K satellites of a perturbed cell: the first K of the consumer's frozen ranking."""
    return [sat for sat, _ in load_sats(cell.consumer)["ranked"][:cell.k]]


def cmd_sats(a):
    trace = "%s/_rap_nis_%s_null_off/logs/closed_loop_sat_trace.csv" % (RUNS, a.consumer)
    ranked = rank_sats(trace)
    if len(ranked) < N_RANK:
        sys.exit("only %d satellites at TOW %s in %s" % (len(ranked), T_INJ, trace))
    rec = {"consumer": a.consumer, "tow": T_INJ, "source": trace, "ranked": [[s, e] for s, e in ranked[:N_RANK]],
           "all": [[s, e] for s, e in ranked]}
    p = sats_path(a.consumer)
    if os.path.exists(p) and not a.force:
        old = load_sats(a.consumer)
        same = old["ranked"] == rec["ranked"]
        print("%s: frozen list kept (%s the trace now read)" % (a.consumer, "equal to" if same else "DIFFERENT FROM"))
        if not same:
            sys.exit(1)
    else:
        os.makedirs(SCRATCH, exist_ok=True)
        with open(p, "w") as fh:
            json.dump(rec, fh, indent=1)
        print("%s: wrote %s" % (a.consumer, p))
    for i, (s, e) in enumerate(rec["ranked"], 1):
        print("  %d  %s  %.3f deg" % (i, s, e))


def cmd_cells(a):
    for c in cells(a.set, a.consumer):
        print("%-22s %-5s K%d step %s m" % (c.key, c.cls, c.k, STEPS.get(c.step, "-")))


def cmd_wires(a):
    os.makedirs("%s/wires" % SCRATCH, exist_ok=True)
    for c in cells(a.set, a.consumer, with_null=False):
        dst = c.wire()
        sats = sats_of(c)
        counts, n_ep = inject_wire_step.inject_file(BEDS[c.bed] + "/products.txt", dst, c.cls, sats, c.step_m, T_INJ)
        print("%-22s %s %s m on %s: %d records, %d epochs" % (c.key, c.cls, c.step_m, ",".join(sats), sum(counts.values()), n_ep))


def arm_args(c, screen, hours=None, tag_prefix="", eigen_threads=None):
    """The rapid_arm.py make argument list of one replay arm.  `eigen_threads` sets processing.eigen_threads
    (the golden arm has 20; a replay is single-threaded work, and on a loaded box 20 threads waiting at a
    barrier make it an order of magnitude slower; the results do not depend on it, see the card)."""
    bed, site = CONSUMERS[c.consumer]
    args = ["make", "--base", BEDS[bed] + "/arm.json", "--tag", tag_prefix + c.tag(screen)]
    if hours:
        args += ["--hours", repr(hours)]
    args += ["--set", "network.closed_loop.replay_products_path=" + c.wire(), "--set", "network.trace_consumer_sat=true"]
    if site:
        sys.path.insert(0, HERE + "/clock_level")
        import msc_sites   # the census site list
        s = next(x for x in msc_sites.load() if x["name"] == site)
        args += ["--set", "network.closed_loop.station.name=" + s["name"],
                 "--set", "network.closed_loop.station.rinex_path=" + s["path"],
                 "--set", "network.closed_loop.station.position_ecef=" + json.dumps(s["ecef"])]
    if eigen_threads is not None:
        args += ["--set", "processing.eigen_threads=%d" % eigen_threads]
    if screen == "on":
        args += ["--set", SCREEN_KEY + "=true"]
    return args


def cmd_make(a):
    env = dict(os.environ, RAPID_RUNS_ROOT=RUNS)
    screens = ("off", "on") if a.screen == "both" else (a.screen,)
    todo = cells("core" if a.null_only else a.set, a.consumer, with_null=True)
    if a.null_only:
        todo = [c for c in todo if c.cls == "null"]
    jobs = []
    for c in todo:
        for screen in screens:
            tag = ("smoke_" if a.smoke else "") + c.tag(screen)
            if a.skip_started and os.path.isdir("%s/_rap_%s/logs" % (RUNS, tag)):
                continue            # a replay has started or finished in this directory: its arm is not rewritten
            jobs.append((c, screen))

    def one(job):
        c, screen = job
        cmd = [sys.executable, "-I", ROOT + "/scripts/rapid_arm.py"] + arm_args(
            c, screen, hours=(0.083333 if a.smoke else None), tag_prefix=("smoke_" if a.smoke else ""), eigen_threads=a.eigen_threads)
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=ROOT)
        if r.returncode:
            sys.exit("make failed for %s %s:\n%s%s" % (c.key, screen, r.stdout, r.stderr))
        return r.stdout.strip()

    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        for line in pool.map(one, jobs):
            print(line)


def selected(a):
    todo = cells("core" if a.null_only else a.set, a.consumer, with_null=True)
    if a.null_only:
        todo = [c for c in todo if c.cls == "null"]
    screens = ("off", "on") if a.screen == "both" else (a.screen,)
    return [(c, s) for c in todo for s in screens]


def cmd_tags(a):
    print(" ".join(c.tag(s) for c, s in selected(a)))


def cmd_status(a):
    for c, s in selected(a):
        d = "%s/_rap_%s" % (RUNS, c.tag(s))
        ex = open(d + "/exitcode.txt").read().split() if os.path.exists(d + "/exitcode.txt") else ["-", "-"]
        print("%-34s exit %s wall %s s" % (c.tag(s), ex[0], ex[1] if len(ex) > 1 else "-"))


def selftest():
    # the registered matrix: 4 nulls + 4 x 12 core + 12 ext; two replays each
    core = cells("core")
    ext = cells("ext")
    allc = cells("all")
    n_null = sum(1 for c in allc if c.cls == "null")
    assert n_null == 4, n_null
    assert sum(1 for c in core if c.cls != "null") == 4 * 2 * 2 * 3, len(core)
    assert sum(1 for c in ext if c.cls != "null") == 1 * 1 * 2 * 3 and all(c.step == "2" for c in ext if c.cls != "null")
    assert len(allc) == 4 + 48 + 6 and len({c.key for c in allc}) == len(allc)
    assert len({c.tag(s) for c in allc for s in ("off", "on")}) == 2 * len(allc)       # 116 replays
    assert [c.key for c in cells("core", ["wtzz"])][:3] == ["wtzz_null", "wtzz_pb1_s10", "wtzz_pb3_s10"]
    assert Cell("wtzz", "clk", 6, "92p409").tag("on") == "nis_wtzz_clk6_s92p409_on"
    assert Cell("szz", "pb", 3, "10").step_m == 10.0 and Cell("szz", "null").bed == "sim" and Cell("arht", "pb", 1, "10").bed == "field"
    # ranking: highest elevation first, ties by id, one entry per satellite, only the epoch asked for
    with tempfile.TemporaryDirectory() as td:
        p = td + "/sat.csv"
        with open(p, "w") as fh:
            fh.write("week,tow,sat,sig,elev_deg\n")
            for tow in ("269970.000", "270000.000"):
                for sat, sig, el in (("G05", "L1W", 30.0), ("G05", "L2W", 30.0), ("E24", "L1X", 70.1), ("G24", "L1W", 70.1),
                                     ("G15", "L1W", 53.0), ("E31", "L1X", 53.2), ("E09", "L1X", 42.4), ("G22", "L1W", 42.0),
                                     ("G10", "L1W", 17.0), ("G11", "L1W", 90.0 if tow == "269970.000" else 12.0)):
                    fh.write("2313,%s,%s,%s,%s\n" % (tow, sat, sig, el))
        r = rank_sats(p)
        assert [s for s, _ in r] == ["E24", "G24", "E31", "G15", "E09", "G22", "G05", "G10", "G11"], r
        assert rank_sats(p, 269970.0)[0][0] == "G11"
        # nesting: K 1 inside K 3 inside K 6 by construction of sats_of
        global SCRATCH
        saved, SCRATCH = SCRATCH, td.replace("\\", "/")
        try:
            with open(sats_path("wtzz"), "w") as fh:
                json.dump({"ranked": [[s, e] for s, e in r[:N_RANK]]}, fh)
            k1, k3, k6 = (sats_of(Cell("wtzz", "pb", k, "10")) for k in KS)
            assert k1 == ["E24"] and k3 == ["E24", "G24", "E31"] and k6[:3] == k3 and len(k6) == 6 and set(k1) <= set(k3) <= set(k6)
        finally:
            SCRATCH = saved
    # the replay arm: the perturbed wire, the trace, the consumer's own settings, the screen only when on
    a_off = arm_args(Cell("wtzz", "pb", 3, "10"), "off")
    a_on = arm_args(Cell("wtzz", "pb", 3, "10"), "on")
    assert a_off[:3] == ["make", "--base", BEDS["field"] + "/arm.json"] and "nis_wtzz_pb3_s10_off" in a_off
    assert "network.closed_loop.replay_products_path=%s/wires/wtzz_pb3_s10.txt" % SCRATCH in a_off
    assert "network.trace_consumer_sat=true" in a_off and SCREEN_KEY + "=true" not in a_off and SCREEN_KEY + "=true" in a_on
    assert not any(x.startswith("network.closed_loop.station") for x in a_off)
    assert "processing.eigen_threads=1" in arm_args(Cell("wtzz", "pb", 3, "10"), "on", eigen_threads=1)
    assert not any(x.startswith("processing.eigen_threads") for x in a_off)
    a_null = arm_args(Cell("szz", "null"), "off", hours=0.083333, tag_prefix="smoke_")
    assert a_null[2] == BEDS["sim"] + "/arm.json" and "smoke_nis_szz_null_off" in a_null and "--hours" in a_null
    assert "network.closed_loop.replay_products_path=%s/products.txt" % BEDS["sim"] in a_null
    print("nis_inject_arms selftest: ok")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, screen=False):
        p.add_argument("--consumer", nargs="+", choices=list(CONSUMERS))
        p.add_argument("--set", choices=("core", "ext", "all"), default="all")
        if screen:
            p.add_argument("--screen", choices=("off", "on", "both"), default="both")
            p.add_argument("--null-only", action="store_true", help="only the unperturbed cells")

    p = sub.add_parser("cells"); common(p); p.set_defaults(fn=cmd_cells)
    p = sub.add_parser("sats"); p.add_argument("--consumer", required=True, choices=list(CONSUMERS)); p.add_argument("--force", action="store_true"); p.set_defaults(fn=cmd_sats)
    p = sub.add_parser("wires"); common(p); p.set_defaults(fn=cmd_wires)
    p = sub.add_parser("make"); common(p, True)
    p.add_argument("--smoke", action="store_true", help="10 epochs, tags prefixed smoke_")
    p.add_argument("--eigen-threads", type=int, help="processing.eigen_threads of the arms (default: the wire's own arm)")
    p.add_argument("--skip-started", action="store_true", help="leave an arm alone whose run directory already has a logs/ directory")
    p.add_argument("--jobs", type=int, default=1, help="arms made at once")
    p.set_defaults(fn=cmd_make)
    p = sub.add_parser("tags"); common(p, True); p.set_defaults(fn=cmd_tags)
    p = sub.add_parser("status"); common(p, True); p.set_defaults(fn=cmd_status)
    p = sub.add_parser("selftest"); p.set_defaults(fn=lambda a: selftest())
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
