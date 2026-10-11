#!/usr/bin/env python3
"""Truth grade of the producer AR search's lattice observatory (card AGT, step AGT-6;
network.ambiguity_resolution.trace_lattice, filter/NetworkArLattice.hpp).

The observatory keeps the search's fixed DDs as a lattice of fixed cycles.  Each search epoch it
releases the lattice arcs whose life ended (vanished, first seen, freed, or a life-stamp change),
then offers the epoch's determined DDs: added, confirmed, contradicted or not primitive.  It writes
logs/network_ar_lattice.csv (one row per epoch) and logs/network_ar_lattice_events.csv (one row
per release and per offer that is not a confirmation).  This script reads both with the search's
own trace (logs/network_producer_ar.csv: every kept DD and its integer) and the simulator's truth,
and answers the questions of the card's AGT-6 gate:

  1. **Is the bookkeeping exact?**  Per epoch: fixes = added + confirmed + contradicted +
     not_primitive and equals the trace's kept rows; rows(t) = rows(t-1) + added - dims_lost; and
     dims_lost = released_touching (one dimension per released arc that held rows), with the
     epochs where it does not hold (eliminations with no unit row) listed.
  2. **What does each verdict hold, against truth?**  Every kept DD of the trace gets its verdict
     (its event row, or confirmed when it has none) and is graded against the true DD integer
     (the arc's wind-up branch counted, as scripts/simnet_producer_ar_grade.py does).  A wrong
     integer that is CONFIRMED is the lock-in a hold would create; a wrong one ADDED is a wrong
     row entering the lattice.  Each CONTRADICTION is attributed: the offered integer wrong (a
     wrong fix, refused), or the offered integer right and the implied one wrong (a stale
     lattice: a release the life readings missed, or an earlier wrong row).
  3. **What did the releases touch?**  Totals by cause, by stamp field and by free cause; the arcs
     released again one epoch after a release while holding rows (one event read as two lives);
     with --slip-tow and --slip-arc, every release at that epoch, and whether the injected arc's
     bands were released and held rows.

Reads:  <run>/logs/network_ar_lattice.csv, <run>/logs/network_ar_lattice_events.csv,
        <run>/logs/network_producer_ar.csv, <run>/logs/network_slip_raw.csv (wind-up branches),
        <fixture>/<sta>/truth_ambiguities.csv + truth_slips.csv + truth_windup.csv
Usage:  python scripts/simnet_ar_lattice_grade.py <fixture_dir> --run <run_dir>
            [--slip-tow 261930 --slip-arc S43000SIM:G14] [--no-windup-branch] [--json]
        python scripts/simnet_ar_lattice_grade.py --selftest
"""
import argparse
import collections
import csv
import io
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at  # noqa: E402
from simnet_producer_ar_grade import branch_at, load_windup_branches  # noqa: E402

EPOCH_REL = os.path.join("logs", "network_ar_lattice.csv")
EVENTS_REL = os.path.join("logs", "network_ar_lattice_events.csv")
TRACE_REL = os.path.join("logs", "network_producer_ar.csv")

VERDICTS = ("added", "confirmed", "contradicted", "not_primitive")
CAUSES = ("vanished", "first_seen", "freed", "stamp")
STAMP_FIELDS = ("adm_generation", "adm_run_start", "has_carry", "birth_ep", "dropped_ep",
                "relevel_s", "flag_ep")
FREE_CAUSES = ("mw", "lli", "if_td", "t4_attrib", "t3_consensus", "windup", "gap_reentry",
               "gf_jump")


def rows_of(path):
    """(header index, rows) of a CSV file."""
    if not os.path.isfile(path):
        raise SystemExit("missing " + path + " (arm network.ambiguity_resolution.trace_lattice "
                         "and .trace)")
    with io.open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        return {c: i for i, c in enumerate(hdr)}, list(rd)


def bits(value, names):
    """The names of the set bits of an integer column (empty string: none)."""
    v = int(value) if value else 0
    return [n for k, n in enumerate(names) if v & (1 << k)]


class Truth:
    """The true DD integer in the producer's wind-up convention."""

    def __init__(self, fixture, run, windup_branch):
        stations = sorted(d for d in os.listdir(fixture)
                          if os.path.isdir(os.path.join(fixture, d))
                          and os.path.isfile(os.path.join(fixture, d, "truth_ambiguities.csv")))
        if not stations:
            raise SystemExit("no truth stations under " + fixture)
        self.n_int, self.slips = load_truth(fixture, stations)
        self.branches = load_windup_branches(fixture, run, stations)[0] if windup_branch else {}

    def arc(self, sta, sat, sig, tow):
        n = n_at(self.n_int, self.slips, sta, sat, sig, tow)
        return None if n is None else n + branch_at(self.branches, sta, sat, tow)

    def dd(self, sig, sat_j, ref_sat, sta_r, ref_sta, tow):
        v = [self.arc(s, k, sig, tow) for s, k in ((sta_r, sat_j), (sta_r, ref_sat),
                                                  (ref_sta, sat_j), (ref_sta, ref_sat))]
        if None in v:
            return None
        return (v[0] - v[1]) - (v[2] - v[3])


def check_epochs(run):
    """Gate part 1 from the per-epoch file alone."""
    ix, rows = rows_of(os.path.join(run, EPOCH_REL))
    out = {"epochs": len(rows), "verdicts": collections.Counter(), "released": 0,
           "released_touching": 0, "dims_lost": 0, "no_unit": 0, "causes": collections.Counter(),
           "sum_mismatch": [], "rows_mismatch": [], "dims_mismatch": [], "fixes_by_tow": {},
           "max_rows": 0, "max_len": 0, "mean_len_last": 0.0}
    prev_rows = 0
    for r in rows:
        tow = float(r[ix["tow"]])
        g = {c: int(r[ix[c]]) for c in ("rows", "fixes", "added", "confirmed", "contradicted",
                                        "not_primitive", "released", "released_touching",
                                        "dims_lost", "no_unit", "max_len")}
        for v in VERDICTS:
            out["verdicts"][v] += g[v]
        for c in CAUSES:
            out["causes"][c] += int(r[ix["rel_" + c]])
        for c in ("released", "released_touching", "dims_lost", "no_unit"):
            out[c] += g[c]
        out["fixes_by_tow"][tow] = g["fixes"]
        if sum(g[v] for v in VERDICTS) != g["fixes"]:
            out["sum_mismatch"].append(tow)
        if g["rows"] != prev_rows + g["added"] - g["dims_lost"]:
            out["rows_mismatch"].append(tow)
        if g["dims_lost"] != g["released_touching"]:
            out["dims_mismatch"].append((tow, g["dims_lost"], g["released_touching"], g["no_unit"]))
        prev_rows = g["rows"]
        out["max_rows"] = max(out["max_rows"], g["rows"])
        out["max_len"] = max(out["max_len"], g["max_len"])
        out["mean_len_last"] = float(r[ix["mean_len"]])
    return out


def grade(fixture, run, slip_tow=None, slip_arc=None, windup_branch=True, out=sys.stdout):
    """Grade one run; print the report and return it as a dict."""
    P = lambda *a: print(*a, file=out)  # noqa: E731
    ep = check_epochs(run)
    truth = Truth(fixture, run, windup_branch)

    # Offer events, keyed like the trace's rows.
    eix, events = rows_of(os.path.join(run, EVENTS_REL))
    offer_ev = {}
    releases = []
    for r in events:
        tow = float(r[eix["tow"]])
        if r[eix["event"]] == "release":
            releases.append({"tow": tow, "signal": r[eix["signal"]], "sat": r[eix["sat_j"]],
                             "sta": r[eix["sta_r"]], "cause": r[eix["cause"]],
                             "rows_touched": int(r[eix["rows_touched"]]),
                             "rows_dropped": int(r[eix["rows_dropped"]]),
                             "change": bits(r[eix["change_bits"]], STAMP_FIELDS),
                             "free": bits(r[eix["free_cause"]], FREE_CAUSES)})
            continue
        key = (tow, r[eix["signal"]], r[eix["sat_j"]], r[eix["ref_sat"]], r[eix["sta_r"]],
               r[eix["ref_sta"]])
        offer_ev[key] = (r[eix["event"]], int(r[eix["value"]]),
                         int(r[eix["implied"]]) if r[eix["implied"]] else None)

    # Every kept DD of the search's trace, with its verdict and its truth.
    tix, trows = rows_of(os.path.join(run, TRACE_REL))
    matrix = {v: collections.Counter() for v in VERDICTS}
    contra = collections.Counter()
    wrong = []
    kept_by_tow = collections.Counter()
    value_mismatch = 0
    matched = set()
    for r in trows:
        if r[tix["kept"]] != "1":
            continue
        tow = float(r[tix["tow"]])
        kept_by_tow[tow] += 1
        key = (tow, r[tix["signal"]], r[tix["sat_j"]], r[tix["ref_sat"]], r[tix["sta_r"]],
               r[tix["ref_sta"]])
        fixed = int(round(float(r[tix["fixed_int"]])))
        verdict, value, implied = offer_ev.get(key, ("confirmed", fixed, None))
        if key in offer_ev:
            matched.add(key)
            if value != fixed:
                value_mismatch += 1
        t = truth.dd(key[1], key[2], key[3], key[4], key[5], tow)
        if t is None:
            matrix[verdict]["no_truth"] += 1
            continue
        ok = fixed == t
        matrix[verdict]["correct" if ok else "wrong"] += 1
        if verdict == "contradicted":
            if not ok:
                contra["wrong fix (offered integer wrong)"] += 1
            elif implied is not None and implied != t:
                contra["stale lattice (offered right, implied wrong)"] += 1
            else:
                contra["implied right too (inconsistent)"] += 1
        if not ok:
            wrong.append({"tow": tow, "verdict": verdict, "signal": key[1], "sat_j": key[2],
                          "ref_sat": key[3], "sta_r": key[4], "ref_sta": key[5],
                          "fixed": fixed, "true": t, "implied": implied})
    unmatched_events = len(set(offer_ev) - matched)
    fixes_vs_trace = sum(1 for tow, n in ep["fixes_by_tow"].items() if n != kept_by_tow.get(tow, 0))

    # A release of an arc the epoch after its last release: the fix of the new life entered the
    # lattice in between, and the life reading ended it again (two stamp steps of one event).
    last_rel = {}
    rereleased = 0
    for x in releases:
        k = (x["sta"], x["sat"], x["signal"])
        if x["rows_touched"] > 0 and k in last_rel and 0.0 < x["tow"] - last_rel[k] <= 30.5:
            rereleased += 1
        last_rel[k] = x["tow"]
    rel_by = collections.Counter(x["cause"] for x in releases)
    touch_by = collections.Counter(x["cause"] for x in releases if x["rows_touched"] > 0)
    field_by = collections.Counter(f for x in releases if x["rows_touched"] > 0 for f in x["change"])
    free_by = collections.Counter(f for x in releases if x["rows_touched"] > 0 for f in x["free"])

    P("Lattice observatory grade: run %s" % run)
    P("  1. bookkeeping: %d epoch(s); verdict sums %s; rows recursion %s; fixes = the trace's kept "
      "rows %s; event values = the trace's integers %s; offer events with no trace row %d"
      % (ep["epochs"], "EXACT" if not ep["sum_mismatch"] else "MISMATCH %d" % len(ep["sum_mismatch"]),
         "EXACT" if not ep["rows_mismatch"] else "MISMATCH %d" % len(ep["rows_mismatch"]),
         "EXACT" if fixes_vs_trace == 0 else "MISMATCH in %d epoch(s)" % fixes_vs_trace,
         "EXACT" if value_mismatch == 0 else "MISMATCH %d" % value_mismatch, unmatched_events))
    P("     dimensions lost %d = released arcs that held rows %d: %s (eliminations with no unit "
      "row %d)" % (ep["dims_lost"], ep["released_touching"],
                   "EQUAL in every epoch" if not ep["dims_mismatch"]
                   else "DIFFER in %d epoch(s), first %s" % (len(ep["dims_mismatch"]), ep["dims_mismatch"][0]),
                   ep["no_unit"]))
    P("     lattice: at most %d rows; longest row %d arcs; mean row length at the end %.3f"
      % (ep["max_rows"], ep["max_len"], ep["mean_len_last"]))
    P("  2. offers against truth (verdict: correct / wrong / no truth):")
    for v in VERDICTS:
        m = matrix[v]
        P("     %-14s %8d / %4d / %d" % (v, m["correct"], m["wrong"], m["no_truth"]))
    P("     lock-in (a wrong integer confirmed): %d; wrong rows added: %d"
      % (matrix["confirmed"]["wrong"], matrix["added"]["wrong"]))
    if contra:
        P("     contradictions: " + "; ".join("%s %d" % kv for kv in sorted(contra.items())))
    for w in wrong[:12]:
        P("     wrong: %s" % w)
    P("  3. releases: %d (%s); %d held rows (%s); by stamp field [%s]; by free cause [%s]"
      % (len(releases), ", ".join("%s %d" % (c, rel_by[c]) for c in CAUSES),
         sum(touch_by.values()), ", ".join("%s %d" % (c, touch_by[c]) for c in CAUSES),
         ", ".join("%s %d" % kv for kv in sorted(field_by.items())) or "none",
         ", ".join("%s %d" % kv for kv in sorted(free_by.items())) or "none"))
    P("     re-released one epoch after a release, holding rows again: %d" % rereleased)
    slip = None
    if slip_tow is not None:
        at = [x for x in releases if abs(x["tow"] - slip_tow) < 0.5]
        P("     at TOW %.1f: %d release(s)" % (slip_tow, len(at)))
        for x in at:
            P("       %s %s %s: %s, rows held %d, dropped %d, stamp [%s], free [%s]"
              % (x["sta"], x["sat"], x["signal"], x["cause"], x["rows_touched"], x["rows_dropped"],
                 ",".join(x["change"]), ",".join(x["free"])))
        slip = {"tow": slip_tow, "releases": at}
        if slip_arc:
            sta, sat = slip_arc.split(":")
            on = [x for x in at if x["sta"] == sta and x["sat"] == sat]
            off = [x for x in at if not (x["sta"] == sta and x["sat"] == sat)
                   and x["cause"] in ("freed", "stamp") and x["rows_touched"] > 0]
            later = [x for x in releases if x["sta"] == sta and x["sat"] == sat and x["tow"] > slip_tow]
            P("     injected arc %s: released bands at the slip epoch [%s], rows held %d; other "
              "arcs freed or stamped at that epoch that held rows: %d; later releases of the arc %d"
              % (slip_arc, ",".join(sorted(x["signal"] for x in on)),
                 sum(x["rows_touched"] for x in on), len(off), len(later)))
            slip.update({"arc": slip_arc, "bands": sorted(x["signal"] for x in on),
                         "rows_held": sum(x["rows_touched"] for x in on),
                         "other_held_releases": len(off)})
    return {"epochs": ep["epochs"], "verdicts": dict(ep["verdicts"]),
            "bookkeeping": {"sum_mismatch": len(ep["sum_mismatch"]),
                            "rows_mismatch": len(ep["rows_mismatch"]),
                            "fixes_vs_trace": fixes_vs_trace, "value_mismatch": value_mismatch,
                            "unmatched_events": unmatched_events},
            "dims_lost": ep["dims_lost"], "released_touching": ep["released_touching"],
            "dims_mismatch_epochs": len(ep["dims_mismatch"]), "no_unit": ep["no_unit"],
            "max_rows": ep["max_rows"], "max_len": ep["max_len"],
            "matrix": {v: dict(matrix[v]) for v in VERDICTS},
            "lock_in": matrix["confirmed"]["wrong"], "wrong_added": matrix["added"]["wrong"],
            "contradictions": dict(contra), "wrong": wrong,
            "releases": dict(rel_by), "releases_held": dict(touch_by),
            "by_field": dict(field_by), "by_free": dict(free_by), "rereleased": rereleased,
            "slip": slip}


def selftest():
    """A two-station, three-satellite fixture: a held DD, a release on a slipped arc, its re-fix
    in the new life (added), a confirmation, a wrong confirmed integer (lock-in), and a
    contradiction attributed to a stale lattice."""
    ok = True

    def chk(c, what):
        nonlocal ok
        print(("  ok   " if c else "  FAIL ") + what)
        ok = ok and c

    with tempfile.TemporaryDirectory() as d:
        fx, run = os.path.join(d, "fx"), os.path.join(d, "run")
        os.makedirs(os.path.join(run, "logs"))
        N = {("SA", "G01"): 10, ("SA", "G02"): 20, ("SA", "G03"): 33,
             ("SB", "G01"): 5, ("SB", "G02"): 7, ("SB", "G03"): 1}
        for sta in ("SA", "SB"):
            os.makedirs(os.path.join(fx, sta))
            with open(os.path.join(fx, sta, "truth_ambiguities.csv"), "w") as f:
                f.write("satellite,signal,N_int\n")
                for (s, k), n in N.items():
                    if s == sta:
                        f.write("%s,L1C,%d\n" % (k, n))
        with open(os.path.join(fx, "SA", "truth_slips.csv"), "w") as f:
            f.write("tow_s,satellite,signal,delta_cycles\n100.0,G02,L1C,3\n")
        # DD(SA,SB;G02,G01) true: (20-10)-(7-5) = 8, after the slip 11; DD(SA,SB;G03,G01) = 23-(-4) = 27.
        hdr = ("week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,sigma_cyc,"
               "in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n")
        trace = [(70, "G02", 8), (70, "G03", 27), (100, "G02", 11), (100, "G03", 27),
                 (130, "G02", 12), (160, "G03", 27)]
        with open(os.path.join(run, TRACE_REL), "w") as f:
            f.write(hdr)
            for tow, j, v in trace:
                f.write("1,%.1f,G,L1C,%s,G01,SA,SB,0.19,0,0.01,1,%d,1,5,1,2,2,2\n" % (tow, j, v))
        with open(os.path.join(run, EPOCH_REL), "w") as f:
            f.write("week,tow,rows,arcs,fixes,added,confirmed,contradicted,not_primitive,released,"
                    "released_touching,dims_lost,no_unit,rel_vanished,rel_first_seen,rel_freed,"
                    "rel_stamp,max_len,mean_len\n")
            f.write("1,70.0,2,6,2,2,0,0,0,0,0,0,0,0,0,0,0,4,4.000\n")
            f.write("1,100.0,2,6,2,1,1,0,0,1,1,1,0,0,0,1,0,4,4.000\n")
            f.write("1,130.0,2,6,1,0,0,1,0,0,0,0,0,0,0,0,0,4,4.000\n")
            f.write("1,160.0,2,6,1,0,1,0,0,0,0,0,0,0,0,0,0,4,4.000\n")
        with open(os.path.join(run, EVENTS_REL), "w") as f:
            f.write("week,tow,event,cons,signal,sat_j,ref_sat,sta_r,ref_sta,value,implied,len,cause,"
                    "rows_touched,rows_dropped,change_bits,free_cause\n")
            f.write("1,70.0,added,G,L1C,G02,G01,SA,SB,8,,4,,,,,\n")
            f.write("1,70.0,added,G,L1C,G03,G01,SA,SB,27,,4,,,,,\n")
            f.write("1,100.0,release,G,L1C,G02,,SA,,,,,freed,1,1,32,128\n")
            f.write("1,100.0,added,G,L1C,G02,G01,SA,SB,11,,4,,,,,\n")
            f.write("1,130.0,contradicted,G,L1C,G02,G01,SA,SB,12,11,,,,,,\n")
        r = grade(fx, run, slip_tow=100.0, slip_arc="SA:G02", windup_branch=False, out=io.StringIO())
        chk(r["bookkeeping"] == {"sum_mismatch": 0, "rows_mismatch": 0, "fixes_vs_trace": 0,
                                 "value_mismatch": 0, "unmatched_events": 0}, "bookkeeping exact")
        chk(r["dims_lost"] == 1 and r["released_touching"] == 1 and r["dims_mismatch_epochs"] == 0,
            "one dimension per released arc that held rows")
        chk(r["matrix"]["added"] == {"correct": 3}, "three correct rows added (one after the release)")
        chk(r["matrix"]["confirmed"] == {"correct": 2}, "two correct confirmations")
        chk(r["contradictions"] == {"wrong fix (offered integer wrong)": 1},
            "the contradiction is a wrong fix (12 against 11)")
        chk(r["lock_in"] == 0, "no lock-in")
        chk(r["slip"]["bands"] == ["L1C"] and r["slip"]["rows_held"] == 1, "the slip epoch releases the arc")
        chk(r["by_field"] == {"relevel_s": 1} and r["by_free"] == {"gf_jump": 1}, "attribution bits")
        # A stale lattice: the slip unreleased, the right new integer contradicts the old row.
        with open(os.path.join(run, EVENTS_REL), "w") as f:
            f.write("week,tow,event,cons,signal,sat_j,ref_sat,sta_r,ref_sta,value,implied,len,cause,"
                    "rows_touched,rows_dropped,change_bits,free_cause\n")
            f.write("1,70.0,added,G,L1C,G02,G01,SA,SB,8,,4,,,,,\n")
            f.write("1,70.0,added,G,L1C,G03,G01,SA,SB,27,,4,,,,,\n")
            f.write("1,100.0,contradicted,G,L1C,G02,G01,SA,SB,11,8,,,,,,\n")
            f.write("1,130.0,contradicted,G,L1C,G02,G01,SA,SB,12,8,,,,,,\n")
        r2 = grade(fx, run, windup_branch=False, out=io.StringIO())
        chk(r2["contradictions"].get("stale lattice (offered right, implied wrong)") == 1,
            "a right integer against a stale row reads as a stale lattice")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("fixture", nargs="?")
    ap.add_argument("--run")
    ap.add_argument("--slip-tow", type=float)
    ap.add_argument("--slip-arc", help="STATION:SAT of the injected slip")
    ap.add_argument("--no-windup-branch", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.fixture or not a.run:
        ap.error("fixture and --run are required")
    r = grade(a.fixture, a.run, a.slip_tow, a.slip_arc, not a.no_windup_branch)
    if a.json:
        print(json.dumps(r, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
