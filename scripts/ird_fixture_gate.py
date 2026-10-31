#!/usr/bin/env python3
"""Fixture checks of card IRD step IRD-2 (section 6 fixtures, section 11 gate "each fixture's event on
its intended tree edge").

    python scripts/ird_fixture_gate.py verify --name t1 [--fixture DIR] [--base DIR]
    python scripts/ird_fixture_gate.py verify --name rg --arm ird2_a0_rg
    python scripts/ird_fixture_gate.py gate --name t1 --arm ird2_a0_t1 [--ref ird2_a0_r3b] [--rows 12]
    python scripts/ird_fixture_gate.py --selftest

Fixture names are those of scripts/ird_make_arms.py without the `ird_` prefix: t1 .. t5 and tr (scripted
slips, config/sim/ird/ird_<name>_2h.json) and rg, rv6, rv16 (rung 3b with one station blanked, the `strip`
op of the bed in ird_make_arms.BEDS).

verify, slip fixtures: the generated fixture against rung 3b (default runs/simarl_r3b).  Every station
directory carries the same files; every file is byte-identical except the slipped station's RINEX,
truth_slips.csv and scenario_summary.json (the record of the scripted events); the RINEX differs only in
the phase field of the slipped (satellite, band) from the slip epoch on, by exactly the scripted cycles,
with the loss-of-lock digit set at the slip epoch for a flagged slip and nowhere else; truth_slips.csv holds
the scripted rows; the child configs and the manifest equal rung 3b's apart from the comment and the paths.
verify, gap fixtures: the arm's rewritten RINEX against rung 3b's: outside the window the file is
identical, inside it every satellite record of the station is blank, and the epoch headers are all kept.

gate: the arm's chain trace (logs/network_chain_edges.csv).  A slip fixture passes when the slipped arc is
a tree edge of the intended kind in every slipped band at the epoch before the event and at the event
epoch, the arc is not at the root unless the fixture is tr, and the arm's products.txt equals the
reference arm's for every epoch before the event.  A gap fixture passes when the blanked station is the
tree vertex of the intended kind (r1 for rg; a non-root vertex with child satellites for rv) at the epoch
before the first blanked epoch, with the same products condition.  The CHAIN event rows at the event
epoch and the run-end `Network (chain frame)` lines are printed.

The kinds (from the fixture's name): t1 satellite-side edge (the child is the satellite) with at least 3
satellites below it; t2 and t5 an edge in both bands with at least LARGE_SUBTREE vertices below it (the
reading of "large subtree" used to pick the targets); t3 an edge in L2W; t4 an edge whose parent is a
satellite that is itself a child of r1; tr an edge at r1 (edge_sta = r1).  t5 additionally fires at
TOW 266250 (epoch 175).
"""
import argparse
import csv
import filecmp
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ird_make_arms as arms  # noqa: E402

ROOT = arms.ROOT
TOW0, DT = 261030.0, 30.0
SAT = re.compile(r"^[GE]\d\d$")
LARGE_SUBTREE = 10
SLIP_FIXTURES = ("t1", "t2", "t3", "t4", "t5", "tr")
GAP_FIXTURES = ("rg", "rv6", "rv16")


def epoch_tow(ep):
    return TOW0 + DT * (ep - 1)


def tow_epoch(tow):
    return int(round((float(tow) - TOW0) / DT)) + 1


def is_sat(v):
    return bool(SAT.match(v))


# ----------------------------------------------------------------------------- fixture definitions
def slip_fixture(name, root=ROOT):
    """(station, slips) of a slip fixture, from its simulator config; a slip is (sat, signal, epoch,
    delta_cycles, lli)."""
    cfg = json.load(open("%s/config/sim/ird/ird_%s_2h.json" % (root, name)))
    hits = [(s["name"], s["scripted_slips"]) for s in cfg["network_stations"] if s.get("scripted_slips")]
    if len(hits) != 1:
        raise SystemExit("fixture %s: %d stations carry scripted slips, expected 1" % (name, len(hits)))
    station, slips = hits[0]
    return station, [(s["sat"], s["signal"], int(s["epoch"]), int(s["delta_cycles"]), bool(s.get("flag_lli", False)))
                     for s in slips]


def gap_fixture(name):
    """(station, first blanked epoch, last blanked epoch) of a gap fixture, from its bed's strip op."""
    op = arms.BEDS["ird_" + name][4][0]
    kv = dict(p.split("=", 1) for p in op.split(";")[1:])
    return kv["stations"], tow_epoch(kv["tow_from"]), tow_epoch(kv["tow_to"])


# ----------------------------------------------------------------------------- RINEX
def obs_codes(lines, system="G"):
    for i, ln in enumerate(lines):
        if ln.startswith(system + " ") and "SYS / # / OBS TYPES" in ln:
            codes, n, j = ln[7:60].split(), int(ln[3:6]), i
            while len(codes) < n:
                j += 1
                codes += lines[j][7:60].split()
            return codes
    raise SystemExit("no GPS observation types in the header")


def rinex_diffs(a_text, b_text):
    """(codes, diffs, other, epochs): the differing 16-character observation fields of two GPS RINEX 3
    files with the same epochs, as (epoch, sat, code, field a, field b); `other` lists every other
    differing line."""
    A, B = a_text.split("\n"), b_text.split("\n")
    if len(A) != len(B):
        return None, [], [("line count", len(A), len(B))], 0
    codes = obs_codes(A)
    width = 3 + 16 * len(codes)
    ep, diffs, other = 0, [], []
    for la, lb in zip(A, B):
        if la.startswith(">"):
            ep += 1
        if la == lb:
            continue
        if not la.startswith(">") and la[:3] == lb[:3]:
            pa, pb = la.ljust(width), lb.ljust(width)
            for c, code in enumerate(codes):
                fa, fb = pa[3 + 16 * c:19 + 16 * c], pb[3 + 16 * c:19 + 16 * c]
                if fa != fb:
                    diffs.append((ep, la[:3], code, fa, fb))
        else:
            other.append((ep, la[:40], lb[:40]))
    return codes, diffs, other, ep


def check_slips(diffs, slips):
    """Problems (a list of strings) in the field differences `diffs` against the scripted `slips`."""
    by = {}
    for ep, sat, code, fa, fb in diffs:
        by.setdefault((sat, code), []).append((ep, fa, fb))
    bad = []
    wanted = {(s[0], s[1]): s for s in slips}
    for key in sorted(set(by) - set(wanted)):
        bad.append("unexpected differences on %s %s (%d fields)" % (key[0], key[1], len(by[key])))
    for key, (sat, sig, k, n, lli) in sorted(wanted.items()):
        rows = by.get(key)
        if not rows:
            bad.append("%s %s: no difference at all" % key)
            continue
        if min(r[0] for r in rows) != k:
            bad.append("%s %s: first difference at epoch %d, not %d" % (sat, sig, min(r[0] for r in rows), k))
        for ep, fa, fb in rows:
            try:
                if abs(float(fb[:14]) - float(fa[:14]) - n) > 0.0015:
                    bad.append("%s %s epoch %d: step %.3f, not %d" % (sat, sig, ep, float(fb[:14]) - float(fa[:14]), n))
                    break
            except ValueError:
                bad.append("%s %s epoch %d: a blank field changed" % (sat, sig, ep))
                break
        flags = [r[0] for r in rows if r[1][14:15] != r[2][14:15]]
        if lli and flags != [k]:
            bad.append("%s %s: loss-of-lock digit changed at epochs %s, expected [%d]" % (sat, sig, flags, k))
        if not lli and flags:
            bad.append("%s %s: loss-of-lock digit changed at epochs %s on an unflagged slip" % (sat, sig, flags))
    return bad


# ----------------------------------------------------------------------------- verify
def verify_slip(name, fixture, base, root=ROOT):
    station, slips = slip_fixture(name, root)
    rn = "%s_R_20241290000_01D_30S_MO.rnx" % station
    changed = {rn, "truth_slips.csv", "scenario_summary.json"}
    problems, n_same = [], 0
    stations = sorted(d for d in os.listdir(base) if os.path.isdir("%s/%s" % (base, d)))
    for sd in stations:
        fa, fb = sorted(os.listdir("%s/%s" % (base, sd))), sorted(os.listdir("%s/%s" % (fixture, sd)))
        if fa != fb:
            problems.append("%s: file sets differ" % sd)
            continue
        for f in fa:
            if f == "logs":
                continue
            same = filecmp.cmp("%s/%s/%s" % (base, sd, f), "%s/%s/%s" % (fixture, sd, f), shallow=False)
            expect_changed = sd == station and f in changed
            if same and not expect_changed:
                n_same += 1
            elif not same and not expect_changed:
                problems.append("%s/%s differs" % (sd, f))
            elif same and expect_changed:
                problems.append("%s/%s is identical but must differ" % (sd, f))
    a = open("%s/%s/%s" % (base, station, rn), encoding="ascii", errors="replace", newline="").read()
    b = open("%s/%s/%s" % (fixture, station, rn), encoding="ascii", errors="replace", newline="").read()
    codes, diffs, other, nep = rinex_diffs(a, b)
    if other:
        problems.append("RINEX: %d differing lines other than observation fields, first %s" % (len(other), other[0]))
    problems += ["RINEX: " + p for p in check_slips(diffs, slips)]
    ts = [ln.strip().split(",") for ln in open("%s/%s/truth_slips.csv" % (fixture, station)).read().splitlines()[1:]
          if ln.strip()]
    got = sorted((r[2], r[3], tow_epoch(float(r[1])), int(r[4]), r[5] == "1") for r in ts)
    want = sorted((s[0], s[1], s[2], s[3], s[4]) for s in slips)
    if got != want:
        problems.append("truth_slips.csv rows %s, expected %s" % (got, want))
    # the child configs equal rung 3b's apart from the comment, and the slipped station's holds the scripted slips
    norm = lambda rows: sorted(json.dumps(e, sort_keys=True) for e in rows)      # noqa: E731
    want_sc = [dict(sat=s[0], signal=s[1], epoch=s[2], delta_cycles=s[3], **({"flag_lli": True} if s[4] else {}))
               for s in slips]
    for sd in stations:
        ca = json.load(open("%s/_child_%s.json" % (base, sd)))
        cb = json.load(open("%s/_child_%s.json" % (fixture, sd)))
        ca.pop("_comment", None)
        cb.pop("_comment", None)
        if sd == station:
            sc = cb.get("cycle_slips", {}).pop("scripted", None)
            if sc is None or norm(sc) != norm(want_sc):
                problems.append("_child_%s.json: scripted slips %s, expected %s" % (sd, sc, want_sc))
        if ca != cb:
            problems.append("_child_%s.json differs from rung 3b's beyond the comment and the scripted slips" % sd)
    ma = json.load(open(base + "/network_fixture.json"))["stations"]
    mb = json.load(open(fixture + "/network_fixture.json"))["stations"]
    fx = fixture.replace(os.sep, "/").rstrip("/")
    if len(ma) != len(mb) or not all(
            x["name"] == y["name"] and x["position_ecef"] == y["position_ecef"] and x["consumer"] == y["consumer"]
            and y["rinex_path"] == "%s/%s/%s_R_20241290000_01D_30S_MO.rnx" % (fx, y["name"], y["name"])
            and y["truth_dir"] == "%s/%s" % (fx, y["name"]) for x, y in zip(ma, mb)):
        problems.append("network_fixture.json differs from rung 3b's beyond the fixture path")
    print("verify %s: %d stations, %d unchanged files byte-identical to rung 3b; slipped station %s: %d RINEX fields "
          "differ (%s); %d problem(s)" % (name, len(stations), n_same, station, len(diffs),
                                          ", ".join("%s %s%+d from epoch %d" % (s[0], s[1], s[3], s[2]) for s in slips),
                                          len(problems)))
    for p in problems:
        print("  PROBLEM: " + p)
    return not problems


def verify_gap(name, arm, base_rinex_dir):
    station, e0, e1 = gap_fixture(name)
    rn = "%s_R_20241290000_01D_30S_MO.rnx" % station
    # read as rapid_arm.py reads (universal newlines): the rewritten file has LF line ends throughout
    a = open("%s/%s/%s" % (base_rinex_dir, station, rn), encoding="ascii", errors="replace").read()
    b = open("%s/rinex/%s" % (arm, rn), encoding="ascii", errors="replace").read()
    A, B = a.split("\n"), b.split("\n")
    problems = []
    if len(A) != len(B):
        problems.append("line count %d against %d" % (len(A), len(B)))
    ep, n_blank = 0, 0
    for la, lb in zip(A, B):
        if la.startswith(">"):
            ep += 1
        inside = e0 <= ep <= e1
        if la[:1] == "G" and not la.startswith(">") and ep:
            # the op rewrites the GPS records only (the producer reads GPS only on these beds) and drops their
            # trailing blanks; every other line is untouched
            if inside:
                if lb.rstrip() != la[:3]:
                    problems.append("epoch %d: %s is not blank" % (ep, la[:3]))
                else:
                    n_blank += 1
            elif la.rstrip() != lb.rstrip():
                problems.append("epoch %d: %s differs outside the window" % (ep, la[:3]))
        elif la != lb:
            problems.append("epoch %d: a line other than a GPS record differs (%s)" % (ep, la[:3]))
    print("verify %s: station %s, GPS records blanked in epochs %d..%d (TOW %d..%d, %d epochs = %.0f min): %d records; "
          "every other GPS record identical but for trailing blanks, every other line identical; file has %d epochs; %d problem(s)"
          % (name, station, e0, e1, epoch_tow(e0), epoch_tow(e1), e1 - e0 + 1, (e1 - e0 + 1) * DT / 60.0,
             n_blank, ep, len(problems)))
    for p in problems[:10]:
        print("  PROBLEM: " + p)
    return not problems and ep == 240


# ----------------------------------------------------------------------------- the chain trace
def read_trees(path):
    """trees[(band, epoch)] = {child: (parent, depth, edge_sta, edge_sat, row)} and roots[(band, epoch)]."""
    trees, roots = {}, {}
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            k = (r["band"], tow_epoch(r["tow"]))
            trees.setdefault(k, {})[r["child"]] = (r["parent"], int(r["depth"]), r["edge_sta"], r["edge_sat"], r["row"])
            roots[k] = r["r1"]
    return trees, roots


def below(tree, v):
    """The descendants of vertex v in a tree {child: (parent, ...)}."""
    kids = {}
    for c, rec in tree.items():
        kids.setdefault(rec[0], []).append(c)
    out, stack = [], list(kids.get(v, []))
    while stack:
        x = stack.pop()
        out.append(x)
        stack.extend(kids.get(x, []))
    return out


def edge_of(tree, sta, sat):
    """(child, parent, depth) of the arc (sta, sat) when it is a tree edge, else None."""
    for c in (sat, sta):
        rec = tree.get(c)
        if rec and rec[2] == sta and rec[3] == sat:
            return c, rec[0], rec[1]
    return None


def arc_state(trees, roots, band, ep, sta, sat):
    tree = trees.get((band, ep), {})
    e = edge_of(tree, sta, sat)
    if e is None:
        return None
    sub = below(tree, e[0])
    n_sat = sum(1 for x in sub if is_sat(x))
    parent_rec = tree.get(e[1])
    return {"child": e[0], "parent": e[1], "depth": e[2], "sub_sat": n_sat, "sub_sta": len(sub) - n_sat,
            "parent_in_r1": bool(parent_rec and is_sat(e[1]) and parent_rec[0] == roots[(band, ep)]),
            "at_root": sta == roots[(band, ep)], "row": tree[e[0]][4]}


def judge_slip(name, states, k):
    """The kind conditions of fixture `name`; `states[(band, ep)]` is arc_state() or None.  Returns a list of
    (condition, passed)."""
    out = [("tree edge in %s at epoch %d" % key, st is not None) for key, st in sorted(states.items())]
    if any(st is None for st in states.values()):
        return out
    st = list(states.values())
    out.append(("the same edge (child, parent) in every slipped band at both epochs",
                len({(s["child"], s["parent"]) for s in st}) == 1))
    if name == "tr":
        out.append(("an edge at r1 itself (edge_sta = r1)", all(s["at_root"] for s in st)))
    else:
        out.append(("not an arc at r1", not any(s["at_root"] for s in st)))
    if name == "t1":
        out.append(("satellite-side: the child is a satellite", all(is_sat(s["child"]) for s in st)))
        out.append(("at least 3 satellites below the child (%s)" % ",".join(str(s["sub_sat"]) for s in st),
                    all(s["sub_sat"] >= 3 for s in st)))
    if name in ("t2", "t5"):
        out.append(("at least %d vertices below the child (%s)"
                    % (LARGE_SUBTREE, ",".join(str(s["sub_sat"] + s["sub_sta"]) for s in st)),
                    all(s["sub_sat"] + s["sub_sta"] >= LARGE_SUBTREE for s in st)))
    if name == "t4":
        out.append(("r1-adjacent: the parent satellite %s is a child of r1" % st[0]["parent"],
                    all(s["parent_in_r1"] for s in st)))
    return out


def products_blocks(path):
    blocks, cur = [], None
    with open(path, newline="") as fh:
        for ln in fh:
            if ln.startswith("H,"):
                cur = [ln]
                blocks.append(cur)
            elif cur is not None and not ln.startswith("#"):
                cur.append(ln)
    return ["".join(b) for b in blocks]


def first_difference(a_path, b_path):
    a, b = products_blocks(a_path), products_blocks(b_path)
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i + 1, len(a), len(b)
    return (None if len(a) == len(b) else min(len(a), len(b)) + 1), len(a), len(b)


def event_rows(events_csv, tow):
    """(CHAIN rows, tier:event counts of every other row) of network_events.csv at `tow`."""
    chain, other = [], {}
    with open(events_csv, newline="") as fh:
        for r in csv.DictReader(fh):
            if abs(float(r["tow"]) - tow) < 1e-3:
                if r["tier"] == "CHAIN":
                    chain.append(r)
                else:
                    other["%s:%s" % (r["tier"], r["event"])] = other.get("%s:%s" % (r["tier"], r["event"]), 0) + 1
    return chain, other


def run_end_lines(arm):
    p = arm + "/stdout.txt"
    if not os.path.exists(p):
        return []
    out = []
    for ln in open(p, errors="replace"):
        if "Network (chain frame)" in ln or "Network[run-end] the eq34 chain frame" in ln:
            out.append(ln.split("] ", 3)[-1].rstrip())
    return out


def resolve(tag):
    return tag if os.path.isdir(tag) else "%s/_rap_%s" % (arms.RUNS, tag)


def print_events(arm, tow, label, max_rows):
    """The CHAIN rows of network_events.csv at `tow`, counted by kind and listed (at most `max_rows`), and the
    other tiers' counts."""
    rows, other = event_rows(arm + "/logs/network_events.csv", tow)
    print("  CHAIN event rows at %s (TOW %d): %d" % (label, tow, len(rows)))
    by = {}
    for r in rows:
        kv = dict(p.split("=", 1) for p in r["detail"].split() if "=" in p)
        key = "%s row=%s cause=%s" % (r["event"], kv.get("row", "-"), kv.get("cause", kv.get("slip_cause", "-")))
        by[key] = by.get(key, 0) + 1
    for key, n in sorted(by.items()):
        print("    %3d x %s" % (n, key))
    for r in rows[:max_rows]:
        print("    %s %s sat=%s sta=%s %s | %s" % (r["tier"], r["event"], r["sat"], r["station"], r["action"], r["detail"]))
    if len(rows) > max_rows:
        print("    ... %d more row(s); --rows N prints more" % (len(rows) - max_rows))
    print("  other event rows at TOW %d: %s" % (tow, ", ".join("%s x%d" % kv for kv in sorted(other.items())) or "none"))


def gate(name, arm_tag, ref_tag, quiet=False, max_rows=12):
    arm, ref = resolve(arm_tag), resolve(ref_tag)
    trees, roots = read_trees(arm + "/logs/network_chain_edges.csv")
    results = []
    if name in SLIP_FIXTURES:
        station, slips = slip_fixture(name)
        sat = slips[0][0]
        k = slips[0][2]
        bands = sorted({s[1] for s in slips})
        states = {(b, ep): arc_state(trees, roots, b, ep, station, sat) for b in bands for ep in (k - 1, k)}
        what = "%s %s %s epoch %d (TOW %d)" % (station, sat, " ".join("%s%+d%s" % (s[1], s[3], " LLI" if s[4] else "")
                                                                     for s in slips), k, epoch_tow(k))
        results += judge_slip(name, states, k)
        pre = {(b, ep): arc_state(trees, roots, b, ep, station, sat) for b in bands for ep in range(k - 20, k)}
        results.append(("tree edge in every slipped band over the 20 epochs before the event",
                        all(v is not None for v in pre.values())))
        detail = ["%s epoch %d: %s" % (b, ep, ("child %s parent %s depth %d, %d satellites + %d stations below, row %s"
                                               % (s["child"], s["parent"], s["depth"], s["sub_sat"], s["sub_sta"], s["row"]))
                                      if s else "not a tree edge") for (b, ep), s in sorted(states.items())]
        event_tow = epoch_tow(k)
    else:
        station, e0, e1 = gap_fixture(name)
        k = e0
        bands = ["L1C", "L2W"]
        what = "%s blanked epochs %d..%d (TOW %d..%d, %d min)" % (station, e0, e1, epoch_tow(e0), epoch_tow(e1),
                                                                   (e1 - e0 + 1) // 2)
        detail = []
        for b in bands:
            for ep in (e0 - 1, e0, e1 + 1):
                tree = trees.get((b, ep), {})
                kids = [c for c, rec in tree.items() if rec[0] == station]
                rec = tree.get(station)
                is_root = roots.get((b, ep)) == station
                detail.append("%s epoch %d: %s%s, %d child(ren) %s" % (
                    b, ep, "r1" if is_root else ("vertex, parent %s depth %d" % (rec[0], rec[1]) if rec else "not in the tree"),
                    "" if not is_root else " (root)", len(kids), ",".join(sorted(kids))))
                if ep == e0 - 1:
                    if name == "rg":
                        results.append(("%s epoch %d: %s is r1 with child satellites" % (b, ep, station),
                                        is_root and bool(kids)))
                    else:
                        results.append(("%s epoch %d: %s is a non-root tree vertex with child satellites" % (b, ep, station),
                                        bool(rec) and not is_root and bool(kids) and all(is_sat(c) for c in kids)))
        event_tow = epoch_tow(e0)
    diff = first_difference(arm + "/products.txt", ref + "/products.txt")
    results.append(("products.txt equals the reference's for every epoch before %d (first difference: %s; blocks %d against %d)"
                    % (k, diff[0], diff[1], diff[2]), diff[0] is None or diff[0] >= k))
    ok = all(p for _, p in results)
    if not quiet:
        print("gate %s  arm %s  reference %s\n  event: %s" % (name, arm_tag, ref_tag, what))
        for d in detail:
            print("  " + d)
        for c, p in results:
            print("  %s  %s" % ("PASS" if p else "FAIL", c))
        print_events(arm, event_tow, "the event epoch", max_rows)
        if name in GAP_FIXTURES:
            print_events(arm, epoch_tow(e1 + 1), "the first epoch after the gap", max_rows)
        for ln in run_end_lines(arm):
            print("  " + ln)
        print("  GATE %s: %s" % (name, "PASS" if ok else "FAIL"))
    return ok


# ----------------------------------------------------------------------------- selftest
def selftest():
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    chk(epoch_tow(1) == 261030.0 and epoch_tow(175) == 266250.0 and tow_epoch(266250.0) == 175,
        "epoch 1 is TOW 261030 and epoch 175 is TOW 266250")
    # a toy tree: r1 R; satellites G01 (child of R, parent of S2), G02 (child of R); S2 child of G01 with child G03;
    # S3 child of G02 with children G04, G05 and G06 below it
    def rec(parent, depth, sta, sat):
        return (parent, depth, sta, sat, "Kept")
    tree = {"G01": rec("R", 1, "R", "G01"), "G02": rec("R", 1, "R", "G02"), "S2": rec("G01", 2, "S2", "G01"),
            "G03": rec("S2", 3, "S2", "G03"), "S3": rec("G02", 2, "S3", "G02"), "G04": rec("S3", 3, "S3", "G04"),
            "G05": rec("S3", 3, "S3", "G05"), "S4": rec("G04", 4, "S4", "G04")}
    trees = {("L1C", 5): tree, ("L1C", 6): tree, ("L2W", 5): tree, ("L2W", 6): tree}
    roots = {k: "R" for k in trees}
    chk(edge_of(tree, "S3", "G04") == ("G04", "S3", 3) and edge_of(tree, "S3", "G02") == ("S3", "G02", 2)
        and edge_of(tree, "S4", "G01") is None, "an arc is a tree edge with the child on either side")
    chk(sorted(below(tree, "S3")) == ["G04", "G05", "S4"] and below(tree, "G03") == [], "the descendants of a vertex")
    s = arc_state(trees, roots, "L1C", 5, "S3", "G04")
    chk(s["sub_sat"] == 0 and s["sub_sta"] == 1 and not s["parent_in_r1"] and not s["at_root"],
        "arc_state: a satellite-side edge with one station below")
    s = arc_state(trees, roots, "L1C", 5, "S3", "G02")
    chk(s["sub_sat"] == 2 and s["sub_sta"] == 1 and s["parent_in_r1"], "arc_state: S3's edge to G02 is r1-adjacent")
    st = {(b, ep): arc_state(trees, roots, b, ep, "S3", "G02") for b in ("L1C", "L2W") for ep in (5, 6)}
    chk(all(p for _, p in judge_slip("t4", st, 6)), "t4 holds on an r1-adjacent edge in both bands")
    chk(not all(p for _, p in judge_slip("t1", st, 6)) and not all(p for _, p in judge_slip("tr", st, 6)),
        "t1 (satellite-side) and tr (at r1) fail on it")
    st1 = {("L1C", ep): arc_state(trees, roots, "L1C", ep, "S3", "G04") for ep in (5, 6)}
    chk(not all(p for _, p in judge_slip("t1", st1, 6)), "t1 fails with fewer than 3 satellites below the child")
    st1 = {("L1C", ep): arc_state(trees, roots, "L1C", ep, "R", "G01") for ep in (5, 6)}
    chk(all(p for _, p in judge_slip("tr", st1, 6)) and not all(p for _, p in judge_slip("t1", st1, 6)),
        "tr holds on an edge at r1")
    st1 = {("L1C", 5): arc_state(trees, roots, "L1C", 5, "S3", "G04"), ("L1C", 6): None}
    chk(not all(p for _, p in judge_slip("t3", st1, 6)), "an arc that is not a tree edge at the event epoch fails")
    with tempfile.TemporaryDirectory() as td:
        a, b = td + "/a.txt", td + "/b.txt"
        blk = lambda n, x: "H,%d\nE,%d\nC,G05,%s\n" % (n, n, x)       # noqa: E731
        open(a, "w").write("# c\n" + blk(1, 1) + blk(2, 2) + blk(3, 3))
        open(b, "w").write("# c\n" + blk(1, 1) + blk(2, 2) + blk(3, 9))
        chk(first_difference(a, a)[0] is None and first_difference(a, b)[0] == 3, "products: the first differing epoch")
        open(b, "w").write("# c\n" + blk(1, 1) + blk(2, 2))
        chk(first_difference(a, b)[0] == 3, "products: a shorter file differs where it ends")
    hdr = "G    4 C1C L1C C2W L2W                                      SYS / # / OBS TYPES\n"
    def fld(v, flag=" "):
        return "%14.3f%s " % (v, flag)
    def rnx(l1, l2flag):
        out = [hdr.rstrip("\n")]
        for ep in range(1, 5):
            out.append("> 2024 05 08 00 30 %02d.0000000  0  1" % (30 + ep))
            out.append("G09" + fld(2.0e7) + fld(100.0 + (l1 if ep >= 3 else 0.0)) + fld(2.1e7) + fld(80.0, l2flag if ep == 3 else " "))
        return "\n".join(out) + "\n"
    codes, diffs, other, n = rinex_diffs(rnx(0.0, " "), rnx(2.0, " "))
    chk(codes == ["C1C", "L1C", "C2W", "L2W"] and n == 4 and not other and len(diffs) == 2,
        "rinex_diffs finds the two changed phase fields and no other line")
    chk(check_slips(diffs, [("G09", "L1C", 3, 2, False)]) == [], "check_slips accepts the scripted slip")
    chk(check_slips(diffs, [("G09", "L1C", 3, 1, False)]) != [] and check_slips(diffs, [("G09", "L1C", 4, 2, False)]) != [],
        "check_slips rejects another size and another epoch")
    chk(check_slips(diffs, [("G09", "L2W", 3, 2, False)]) != [] and check_slips(diffs, []) != [],
        "check_slips rejects a slip on the wrong band and an unscripted difference")
    codes, diffs2, other, n = rinex_diffs(rnx(2.0, " "), rnx(2.0, "1"))
    chk(check_slips(diffs2, [("G09", "L2W", 3, 0, True)]) == [] and check_slips(diffs2, [("G09", "L2W", 3, 0, False)]) != [],
        "check_slips takes a loss-of-lock digit at the slip epoch for a flagged slip only")
    chk(arms.BEDS["ird_rg"][4] and gap_fixture("rg") == ("S13000SIM", 152, 171) and gap_fixture("rv6")[2] == 163
        and gap_fixture("rv16")[2] == 183, "gap windows read back from the beds' strip ops")
    # --- the committed fixture configs (card IRD section 6)
    shapes = {"t1": [("L1C", 1, False)], "t2": [("L1C", 4, False), ("L2W", 5, False)], "t3": [("L2W", 1, True)],
              "t4": [("L1C", 2, False), ("L2W", 2, False)], "t5": [("L1C", 4, False), ("L2W", 5, False)],
              "tr": [("L1C", 1, False)]}
    base2h = json.load(open(ROOT + "/config/sim/arl/arl_r3b_2h.json"))
    basepr = open(ROOT + "/config/sim/arl/arl_r3b_producer.json").read()
    good = True
    for name in SLIP_FIXTURES:
        station, slips = slip_fixture(name)
        cfg = json.load(open("%s/config/sim/ird/ird_%s_2h.json" % (ROOT, name)))
        rest = json.loads(json.dumps(cfg))
        for s in rest["network_stations"]:
            s.pop("scripted_slips", None)
        ref = json.loads(json.dumps(base2h))
        for c in (rest, ref):
            c.pop("_comment")
        k = slips[0][2]
        desc = arms.BEDS["ird_" + name][3]
        prod = open("%s/config/sim/ird/ird_%s_producer.json" % (ROOT, name)).read()
        want_pr = json.loads(basepr.replace("C:/GnssProcessor/runs/simarl_r3b/", "%s/simird_%s/" % (arms.FIXTURES_ROOT, name)))
        got_pr = json.loads(prod)
        for c in (want_pr, got_pr):
            c.pop("_comment")
        good &= (rest == ref and sorted((s[1], s[3], s[4]) for s in slips) == sorted(shapes[name])
                 and len({(s[0], s[2]) for s in slips}) == 1 and want_pr == got_pr
                 and (station == "S13000SIM") == (name == "tr")
                 and (k == 175 if name == "t5" else 61 <= k <= 180) and 1 <= k - 20 and k + 40 <= 240
                 and station in desc and slips[0][0] in desc and str(k) in desc
                 and arms.BEDS["ird_" + name][1] == "%s/simird_%s" % (arms.FIXTURES_ROOT, name))
    chk(good, "the six slip configs are rung 3b plus one station's scripted slips of the card's shapes, in the card's epoch "
        "window (t5 at epoch 175), the producer files rung 3b's re-pointed, and each bed names its station, satellite and epoch")
    print("ird_fixture_gate selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    v = sub.add_parser("verify")
    v.add_argument("--name", required=True, choices=SLIP_FIXTURES + GAP_FIXTURES)
    v.add_argument("--fixture", help="slip fixture directory (default FIXTURES_ROOT/simird_<name>)")
    v.add_argument("--base", default=ROOT + "/runs/simarl_r3b")
    v.add_argument("--arm", help="gap fixture: the arm whose rinex/ holds the blanked file")
    g = sub.add_parser("gate")
    g.add_argument("--name", required=True, choices=SLIP_FIXTURES + GAP_FIXTURES)
    g.add_argument("--arm", required=True)
    g.add_argument("--ref", default="ird2_a0_r3b")
    g.add_argument("--rows", type=int, default=12, help="CHAIN rows to print in full (the counts by kind are always printed)")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd == "verify":
        if a.name in SLIP_FIXTURES:
            return 0 if verify_slip(a.name, a.fixture or "%s/simird_%s" % (arms.FIXTURES_ROOT, a.name), a.base) else 1
        if not a.arm:
            ap.error("a gap fixture needs --arm")
        return 0 if verify_gap(a.name, resolve(a.arm), a.base) else 1
    if a.cmd == "gate":
        return 0 if gate(a.name, a.arm, a.ref, max_rows=a.rows) else 1
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
