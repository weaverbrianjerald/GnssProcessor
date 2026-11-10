#!/usr/bin/env python3
"""Readings of card IRDG (docs/refound/IRDG_floor_off_rpb_in_block_2026-11-09.md) from the run
directories of its arms (scripts/irdg_run.py).  One line per arm; every reading prints its sample
size.  Run with an interpreter that has numpy (py -3.14).

    py -3.14 scripts/irdg_grade.py ib TAG [...]         I-b per event class (simnet_chain_frame_grade.py)
    py -3.14 scripts/irdg_grade.py ibcmp A B [A B ...]  I-b of two arms on one bed, class by class
    py -3.14 scripts/irdg_grade.py sigma TAG [...]      the receiver phase bias's formal sigma by time
    py -3.14 scripts/irdg_grade.py dcb TAG [...]        the receiver DCB against truth at the last epoch
    py -3.14 scripts/irdg_grade.py fixes TAG [...]      the search's would-be fixes against truth
    py -3.14 scripts/irdg_grade.py consumer TAG [...]   the in-loop float consumer by hour
    py -3.14 scripts/irdg_grade.py heldout WIRE [...]   the nine held-out sites of a held-out wire
    py -3.14 scripts/irdg_grade.py hold TAG [...]       the would-be certification and the certifier reading
    py -3.14 scripts/irdg_grade.py event FIX CTL --tow T  one event epoch's undeclared pairs against a control
    py -3.14 scripts/irdg_grade.py identity A B         nom_td_gate.py --whole, and the lines in one run only
    py -3.14 scripts/irdg_grade.py zwd | stop | reref TAG [...]   readings without a bar: the wet delay against
                                                        truth, the stop rule's chords, the reference-free DD error
    py -3.14 scripts/irdg_grade.py summary TAG [...]    emission, rank, loop time, the key's counters
    py -3.14 scripts/irdg_grade.py root TAG [...]       the root handovers and their pins
    py -3.14 scripts/irdg_grade.py --selftest

A tag is irdg_<arm>_<bed> (scripts/irdg_run.py); the bed names the truth fixture and the grader's mode.
Slow graders keep their output under RAPID_RUNS_ROOT/_irdg_grades/ and reuse it; --fresh recomputes.
"""
import argparse
import csv
import math
import os
import re
import statistics as st
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ird_make_arms as IMA   # noqa: E402  (the beds: fixture and knobs)
import irdg_run as RUN        # noqa: E402  (the bed codes, the held-out sites)

ROOT = IMA.ROOT
RUNS = RUN.RUNS
GRADES = RUNS + "/_irdg_grades"
GOLDEN_SIM_FIXTURE = "runs/simnet60_quiet_col_24h"
SIGMA_TIMES = (0, 180, 210, 240, 300, 450, 600, 900, 1800, 3600, 7170)


def run_dir(tag):
    return "%s/_rap_%s" % (RUNS, tag)


def bed_of(tag):
    """The bed code of a tag: its last underscore field (irdg_hw_b_r3c and irdg_f0_r3c both end in it)."""
    return tag.rsplit("_", 1)[1]


def fixture_of(tag):
    """(truth fixture directory, grader mode) of a tag's bed."""
    bed = IMA.BEDS[RUN.BED[bed_of(tag)]]
    fx = bed[1] or GOLDEN_SIM_FIXTURE
    mode = "microscope" if IMA.MICROSCOPE[0] in bed[2] else "free"
    return IMA.fixture_dir(fx), mode


def cached(name, cmd, fresh):
    """The stdout of `cmd`, kept in GRADES/name and reused."""
    os.makedirs(GRADES, exist_ok=True)
    path = "%s/%s" % (GRADES, name)
    if fresh or not os.path.exists(path):
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(r.stdout)
    return open(path, encoding="utf-8").read()


# ----------------------------------------------------------------------------- I-b
IB_LINE = re.compile(r"^\s*I-b (\S+)\s+n\s+(\d+)\s+(PASS|FAIL)(.*)$")
IB_MAX = re.compile(r"(WL|E1|E2) max ([0-9.]+) > [0-9.]+ \((\d+) pairs\)")
IB_INT = re.compile(r"(\d+) undeclared integer step")


IB_STAT = re.compile(r"^\s*(\S+)\s+events\s+\d+\s+(E1|E2|WL)\s+undeclared n\s+(\d+) \|wrap\| p50 ([0-9.]+) p99 [0-9.]+ "
                     r"max ([0-9.]+)\s+integer (\d+)\s+declared n (\d+) integer (\d+)")


def parse_ib_stats(text):
    """(class, channel) -> {n, p50, max, integer, declared, declared_integer} of the undeclared pairs."""
    out = {}
    for line in text.splitlines():
        m = IB_STAT.match(line)
        if m:
            out[(m.group(1), m.group(2))] = {"n": int(m.group(3)), "p50": float(m.group(4)), "max": float(m.group(5)),
                                             "integer": int(m.group(6)), "declared": int(m.group(7)),
                                             "declared_integer": int(m.group(8))}
    return out


def parse_ib(text):
    """class -> {n, verdict, max: {channel: (max, pairs)}, integer}."""
    out = {}
    for line in text.splitlines():
        m = IB_LINE.match(line)
        if not m:
            continue
        rest = m.group(4)
        mi = IB_INT.search(rest)
        out[m.group(1)] = {"n": int(m.group(2)), "verdict": m.group(3),
                           "max": {c: (float(v), int(n)) for c, v, n in IB_MAX.findall(rest)},
                           "integer": int(mi.group(1)) if mi else 0}
    return out


def cmd_ib(a):
    for tag in a.tags:
        fx, mode = fixture_of(tag)
        text = cached("ib_%s.txt" % tag,
                      [sys.executable, HERE + "/simnet_chain_frame_grade.py", fx, "--run", run_dir(tag),
                       "--source", "prehold", "--mode", mode], a.fresh)
        rows = parse_ib(text)
        if not rows:
            print("%-18s NO I-b LINE (%s)" % (tag, text.strip().splitlines()[-1][:120] if text.strip() else "empty"))
            continue
        stats = parse_ib_stats(text)
        ch = "E1" if mode == "microscope" else "WL"   # the bar's channel
        for cls in sorted(rows):
            if a.classes and cls not in a.classes:
                continue
            r = rows[cls]
            s = stats.get((cls, ch), {})
            mx = "; ".join("%s max %.3f, %d pairs" % (c, v, n) for c, (v, n) in sorted(r["max"].items())) or "within the bar"
            print("%-18s %-5s %-13s n %3d  %-4s  integer %5d  %s p50 %.4f (undeclared pairs %d; declared %d)  %s%s"
                  % (tag, mode[:5], cls, r["n"], r["verdict"], r["integer"], ch, s.get("p50", float("nan")),
                     s.get("n", 0), s.get("declared", 0), mx,
                     "  (reported, not graded)" if cls == "non_event" else ""))
    return 0


def ib_rows(tag, fresh=False):
    """(parse_ib, parse_ib_stats, mode) of a tag's cached frame grade."""
    fx, mode = fixture_of(tag)
    text = cached("ib_%s.txt" % tag,
                  [sys.executable, HERE + "/simnet_chain_frame_grade.py", fx, "--run", run_dir(tag),
                   "--source", "prehold", "--mode", mode], fresh)
    return parse_ib(text), parse_ib_stats(text), mode


def cmd_ibcmp(a):
    """I-b of two arms on one bed, class by class: the integer steps and the pairs over the bar's channel
    (card IRDG bar G7: the second arm no higher than the first)."""
    for i in range(0, len(a.tags) - 1, 2):
        ta, tb = a.tags[i], a.tags[i + 1]
        (ra, sa, mode), (rb, sb, _) = ib_rows(ta, a.fresh), ib_rows(tb, a.fresh)
        ch = "E1" if mode == "microscope" else "WL"
        for cls in sorted(set(ra) | set(rb)):
            if a.classes and cls not in a.classes:
                continue
            xa, xb = ra.get(cls), rb.get(cls)
            if not xa or not xb:
                print("%-14s %-13s only in %s" % (bed_of(ta), cls, ta if xa else tb))
                continue
            pa, pb = xa["max"].get(ch, (0.0, 0)), xb["max"].get(ch, (0.0, 0))
            worse = xb["integer"] > xa["integer"] or pb[1] > pa[1]
            print("%-6s %-5s %-13s n %3d/%3d  integer %5d -> %5d  %s pairs over the bar %5d -> %5d (max %.3f -> %.3f, p50 %.4f -> %.4f)  %s/%s  %s"
                  % (bed_of(ta), mode[:5], cls, xa["n"], xb["n"], xa["integer"], xb["integer"], ch, pa[1], pb[1],
                     pa[0] if pa[1] else sa.get((cls, ch), {}).get("max", 0.0),
                     pb[0] if pb[1] else sb.get((cls, ch), {}).get("max", 0.0),
                     sa.get((cls, ch), {}).get("p50", float("nan")), sb.get((cls, ch), {}).get("p50", float("nan")),
                     xa["verdict"], xb["verdict"], "HIGHER" if worse else "no higher"))
    return 0


# ----------------------------------------------------------------------------- formal sigma
def rpb_sigma_p50(tag, times=SIGMA_TIMES):
    """time offset -> (p50 of sigma_m over unpinned receiver columns, n)."""
    by_t = {}
    with open(run_dir(tag) + "/logs/network_rpb_states.csv", newline="") as fh:
        for r in csv.DictReader(fh):
            if r["pinned"] != "0" or not r["sigma_m"]:
                continue
            try:
                by_t.setdefault(float(r["tow"]), []).append(float(r["sigma_m"]))
            except ValueError:
                pass
    if not by_t:
        return {}
    t0 = min(by_t)
    return {dt: (st.median(by_t[t0 + dt]), len(by_t[t0 + dt])) for dt in times if (t0 + dt) in by_t}


def cmd_sigma(a):
    print("%-18s %s" % ("t, s", " ".join("%8d" % t for t in SIGMA_TIMES)))
    for tag in a.tags:
        s = rpb_sigma_p50(tag)
        print("%-18s %s   (n %s)" % (tag, " ".join(("%8.3f" % s[t][0]) if t in s else "       -" for t in SIGMA_TIMES),
                                     s[max(s)][1] if s else 0))
    return 0


# ----------------------------------------------------------------------------- DCB against truth
def truth_dcb(fixture):
    """station -> true C2W less C1C (m), from each station's truth_rx_bias.csv."""
    out = {}
    for name in sorted(os.listdir(fixture)):
        p = "%s/%s/truth_rx_bias.csv" % (fixture, name)
        if not os.path.exists(p):
            continue
        v = {r["code"]: float(r["value_m"]) for r in csv.DictReader(open(p, newline="")) if r["kind"] == "code"}
        if "C2W" in v and "C1C" in v:
            out[name] = v["C2W"] - v["C1C"]
    return out


def dcb_error(tag):
    """(sd of estimate + truth across receivers at the last epoch, sd of the estimate, sd of the truth,
    correlation of estimate and truth, n)."""
    last = {}
    with open(run_dir(tag) + "/logs/network_rx_states.csv", newline="") as fh:
        for r in csv.DictReader(fh):
            if r["kind"] == "CB" and r["key"] == "GPS_DCB":
                last[r["station"]] = (float(r["tow"]), float(r["value_m"]))
    tmax = max(t for t, _ in last.values())
    truth = truth_dcb(fixture_of(tag)[0])
    names = sorted(n for n, (t, _) in last.items() if t == tmax and n in truth)
    est = [last[n][1] for n in names]
    tru = [truth[n] for n in names]
    err = [e + t for e, t in zip(est, tru)]
    corr = float("nan")
    if len(names) > 2 and st.pstdev(est) > 0 and st.pstdev(tru) > 0:
        me, mt = st.mean(est), st.mean(tru)
        corr = sum((e - me) * (t - mt) for e, t in zip(est, tru)) / (len(names) * st.pstdev(est) * st.pstdev(tru))
    return st.stdev(err), st.stdev(est), st.stdev(tru), corr, len(names)


def cmd_dcb(a):
    for tag in a.tags:
        e, s_est, s_tru, corr, n = dcb_error(tag)
        print("%-18s DCB error sd %.3f m  (estimate sd %.3f, truth sd %.3f, corr %+.3f, n %d)"
              % (tag, e, s_est, s_tru, corr, n))
    return 0


# ----------------------------------------------------------------------------- the search
def search_fixes(tag, fresh=False):
    """(kept, wrong, ceiling %) of the search's would-be fixes, window h0_12 (a 2 h run is all of it)."""
    text = cached("fixes_%s.txt" % tag,
                  [sys.executable, HERE + "/simnet_producer_ar_grade.py", fixture_of(tag)[0], "--run", run_dir(tag)],
                  fresh)
    at = text.find("---- h0_12 window")
    if at < 0:
        return None
    text = text[at:]
    kept = re.search(r"CORRECT-FIX RATE among kept: (\d+)/(\d+)", text)
    ceil = re.search(r"FIXABILITY CEILING.*= +([0-9.]+)%", text)
    wrong = re.search(r"WRONG FIXES \((\d+)\)", text)
    if not kept:
        return None
    n_kept, n_ok = int(kept.group(2)), int(kept.group(1))
    return n_kept, (int(wrong.group(1)) if wrong else n_kept - n_ok), float(ceil.group(1)) if ceil else float("nan")


def cmd_fixes(a):
    for tag in a.tags:
        r = search_fixes(tag, a.fresh)
        print("%-18s %s" % (tag, "no search trace" if r is None
                            else "search kept %d, wrong %d, ceiling %.1f %%" % r))
    return 0


# ----------------------------------------------------------------------------- consumers
def hourly_p50(path, col="pos_err_3d_m"):
    """hour index -> (p50 of the 3D error, n) from a closed-loop trace."""
    hrs = {}
    rows = [r for r in csv.DictReader(open(path, newline="")) if r.get("tow") and r.get(col)]
    if not rows:
        return {}
    t0 = min(float(r["tow"]) for r in rows)
    for r in rows:
        try:
            hrs.setdefault(int((float(r["tow"]) - t0) // 3600), []).append(float(r[col]))
        except ValueError:
            pass
    return {h: (st.median(v), len(v)) for h, v in sorted(hrs.items())}


def cmd_consumer(a):
    for tag in a.tags:
        h = hourly_p50(run_dir(tag) + "/logs/closed_loop_trace.csv")
        print("%-18s float consumer 3D p50 by hour, m: %s"
              % (tag, ", ".join("%.3f (n %d)" % h[k] for k in sorted(h))))
    return 0


def heldout_sites(wire):
    """site -> [p50 hour 1, p50 hour 2] at the eight replayed sites and the wire's own consumer."""
    out = {}
    for name in RUN.HELD_OUT:
        t = "irdg_hr_%s_%s" % (name[:3].lower(), wire[len("irdg_hw_"):])
        p = run_dir(t) + "/logs/consumer_replay_hourly.csv"
        if not os.path.exists(p):
            continue
        rows = {int(r["hour"]): float(r["err3d_median_m"]) for r in csv.DictReader(open(p, newline=""))}
        if 0 in rows and 1 in rows:
            out[name] = [rows[0], rows[1]]
    h = hourly_p50(run_dir(wire) + "/logs/closed_loop_trace.csv")
    if 0 in h and 1 in h:
        out["SZZ000SIM"] = [h[0][0], h[1][0]]
    return out


def cmd_heldout(a):
    ref = heldout_sites(a.ref) if a.ref else None
    for wire in a.tags:
        s = heldout_sites(wire)
        if not s:
            print("%-18s no held-out site yet" % wire)
            continue
        med = [st.median(v[k] for v in s.values()) for k in (0, 1)]
        line = "%-18s %d sites, median of site p50 h1 %.3f, h2 %.3f m" % (wire, len(s), med[0], med[1])
        if ref and wire != a.ref:
            rmed = [st.median(v[k] for v in ref.values()) for k in (0, 1)]
            common = sorted(set(s) & set(ref))
            worst = [max(((s[n][k] - ref[n][k]), n) for n in common) for k in (0, 1)]
            line += ("; against %s: %+.1f %%, %+.1f %%; largest worse %+.3f m (%s), %+.3f m (%s)"
                     % (a.ref, 100.0 * (med[0] / rmed[0] - 1.0), 100.0 * (med[1] / rmed[1] - 1.0),
                        worst[0][0], worst[0][1][:3], worst[1][0], worst[1][1][:3]))
        print(line)
        if a.sites:
            for n in sorted(s):
                print("    %-10s %.3f %.3f" % (n, s[n][0], s[n][1]))
    return 0


# ----------------------------------------------------------------------------- readings without a bar
def zwd_error(tag):
    """(p50, p90, p99, max, n) of |estimated wet delay - truth| over all receivers and epochs after hour 1."""
    fx = fixture_of(tag)[0]
    truth = {}

    def truth_of(name):
        if name not in truth:
            p = "%s/%s/truth_tropo.csv" % (fx, name)
            truth[name] = ({round(float(r["tow_s"]), 3): float(r["zwd_m"]) for r in csv.DictReader(open(p, newline=""))}
                           if os.path.exists(p) else {})
        return truth[name]

    rows = list(csv.DictReader(open(run_dir(tag) + "/logs/network_zwd.csv", newline="")))
    t0 = min(float(r["tow"]) for r in rows)
    err = []
    for r in rows:
        t = float(r["tow"])
        tz = truth_of(r["station"]).get(round(t, 3))
        if t >= t0 + 3600.0 and tz is not None:
            err.append(abs(float(r["zwd_m"]) - tz))
    err.sort()
    if not err:
        return None
    q = lambda f: err[min(len(err) - 1, int(f * len(err)))]
    return q(0.50), q(0.90), q(0.99), err[-1], len(err)


def cmd_zwd(a):
    for tag in a.tags:
        r = zwd_error(tag)
        print("%-18s %s" % (tag, "no wet-delay truth" if r is None else
                            "wet delay error after hour 1: p50 %.3f, p90 %.3f, p99 %.3f, max %.2f m (n %d)" % r))
    return 0


def cmd_stop(a):
    """The stop rule's event-free chords (ird_frame_census.py stop): per-band p50 of |wrap|."""
    for tag in a.tags:
        text = cached("stop_%s.txt" % tag, [sys.executable, HERE + "/ird_frame_census.py", "stop", run_dir(tag)], a.fresh)
        v = re.findall(r"^\s+(L\w+)\s+n (\d+): p50 ([0-9.]+) [<>]=? ?[0-9.]+: (PASS|FAIL)", text, flags=re.M)
        print("%-18s event-free chords: %s" % (tag, "; ".join("%s p50 %s (n %s) %s" % (b, p, n, w) for b, n, p, w in v)
                                                or text.strip().splitlines()[-1][:200]))
    return 0


def cmd_reref(a):
    """The reference-free double-difference error by hour (simnet_dd_reref.py, L1C)."""
    for tag in a.tags:
        text = cached("reref_%s.txt" % tag,
                      [sys.executable, HERE + "/simnet_dd_reref.py", fixture_of(tag)[0], run_dir(tag), "--stride", "1"],
                      a.fresh)
        rows = re.findall(r"^\s*(\d+)\s+G\s+(\d+)\s+([0-9.]+)\s+[0-9.]+ \|\s+[0-9.]+ \|\s+([0-9.]+)\s+([0-9.]+)", text, flags=re.M)
        print("%-18s reference-free DD error, L1C, by hour: %s"
              % (tag, "; ".join("h%d %s cycle, %s %% rounding to 0 (n %s; as traced %s)" % (int(h) + 1, rf, r0, n, tr)
                                for h, n, tr, rf, r0 in rows) or "no row"))
    return 0


# ----------------------------------------------------------------------------- off identity
STAMP = re.compile(r"^\[[^\]]*\] ")


def line_bag(path, mask=None):
    """The file's lines as a multiset, each with its leading time stamp and `mask` matches removed."""
    from collections import Counter
    bag = Counter()
    if not os.path.exists(path):
        return bag
    for line in open(path, encoding="utf-8", errors="replace"):
        line = STAMP.sub("", line.rstrip("\n"))
        if mask:
            line = mask.sub("<masked>", line)
        bag[line] += 1
    return bag


def cmd_identity(a):
    """scripts/nom_td_gate.py --whole on a pair (base exe, candidate with the key absent), its part
    verdicts, and the log and manifest lines that are in one run only (order ignored, stamps removed)."""
    ta, tb = a.tags[0], a.tags[1]
    text = cached("identity_%s__%s.txt" % (ta, tb),
                  [sys.executable, HERE + "/nom_td_gate.py", run_dir(ta), run_dir(tb), "--whole", "--show", "0"],
                  a.fresh)
    csvs = [ln for ln in text.splitlines() if ln.startswith(("  products.txt", "  logs/", "  CSVs outside", "  products.txt in full"))]
    n_ident = sum(1 for ln in csvs if "IDENTICAL" in ln or "none in either run" in ln)
    print("%s vs %s: products and trace CSVs %d of %d identical%s"
          % (ta, tb, n_ident, len(csvs), "" if n_ident == len(csvs) else "  DIFFER: "
             + "; ".join(ln.split()[0] for ln in csvs if "IDENTICAL" not in ln and "none in either" not in ln)))
    for ln in text.splitlines():
        if ln.startswith(("  arm.json", "  network_run_manifest", "  stdout.txt whole", "  main_log whole", "  rt_engine.log",
                          "  exit code", "RESULT")):
            print("    " + ln.strip()[:a.width])
    # The log: run-specific text masked (paths, the exe's name), timing lines left out.
    mask = re.compile(r"(_rap_irdg_[A-Za-z0-9_]+|gnss_processor_[A-Za-z0-9_]+\.exe)")
    timing = re.compile(r"\(D0b?\)|obs backlog|\bwall\b|ms/epoch|[0-9] ms\b|\bms total\b|elapsed|per epoch|timing|thread"
                        r"|RINEX ingestion: parsed")
    A, B = line_bag(run_dir(ta) + "/stdout.txt", mask), line_bag(run_dir(tb) + "/stdout.txt", mask)
    only_a = [ln for ln in (A - B).elements() if not timing.search(ln)]
    only_b = [ln for ln in (B - A).elements() if not timing.search(ln)]
    print("    stdout.txt (timing lines left out): %d line(s) only in %s, %d only in %s" % (len(only_a), ta, len(only_b), tb))
    if a.show:
        for side, lines in (("-", only_a), ("+", only_b)):
            for line in lines[:a.show]:
                print("      %s %s" % (side, line[:a.width]))
    # The manifest: record by record, the paths whose values differ.
    import json
    volatile = ("written_utc", "config_path", "config_hash_fnv1a64", "build", "loop_ms", "ms_per_epoch", "wall_s",
                "product_output_path")

    def load(tag):
        p = run_dir(tag) + "/logs/network_run_manifest.json"
        return [json.loads(ln) for ln in open(p, encoding="utf-8") if ln.strip()] if os.path.exists(p) else []

    def diff(x, y, path, out):
        if isinstance(x, dict) and isinstance(y, dict):
            for k in sorted(set(x) | set(y)):
                if k in volatile or k.endswith("_ms"):
                    continue
                if k not in x or k not in y:
                    out.append("%s.%s only in %s" % (path, k, ta if k in x else tb))
                else:
                    diff(x[k], y[k], path + "." + k, out)
        elif isinstance(x, list) and isinstance(y, list):
            if len(x) != len(y):
                key = next((k for k in ("site", "name", "kind") if x and isinstance(x[0], dict) and k in x[0]), None)
                if key:
                    nx, ny = [e.get(key) for e in x], [e.get(key) for e in y]
                    out.append("%s: %d against %d entries; only in %s: %s; only in %s: %s"
                               % (path, len(x), len(y), ta, [n for n in nx if n not in ny], tb, [n for n in ny if n not in nx]))
                    ymap = {e.get(key): e for e in y}
                    for e in x:
                        if e.get(key) in ymap:
                            diff(e, ymap[e.get(key)], "%s[%s]" % (path, e.get(key)), out)
                else:
                    out.append("%s: %d against %d entries" % (path, len(x), len(y)))
            else:
                for i, (p, q) in enumerate(zip(x, y)):
                    label = p.get("site", p.get("name", p.get("kind", i))) if isinstance(p, dict) else i
                    diff(p, q, "%s[%s]" % (path, label), out)
        elif x != y:
            out.append("%s: %r against %r" % (path, x, y))

    ra, rb = load(ta), load(tb)
    out = []
    if len(ra) != len(rb):
        out.append("manifest: %d against %d records" % (len(ra), len(rb)))
    for x, y in zip(ra, rb):
        diff(x, y, x.get("record", "?"), out)
    print("    run manifest: %d record(s), %d differing value(s) outside the run-specific fields" % (len(ra), len(out)))
    if a.show:
        for line in out[:max(a.show, 40)]:
            print("      " + line[:a.width])
    return 0


# ----------------------------------------------------------------------------- one event epoch against a control
def pair_steps(tag, bands=("L1C", "L2W")):
    """tow -> {(sat, sat): {"E1", "E2", "WL"} one-epoch step of the pair's SD potential on the pre-hold wire},
    and the run's CHAIN events (simnet_chain_frame_grade.py owns both)."""
    import simnet_chain_frame_grade as CFG
    WIC = CFG.WIC
    rd = run_dir(tag)
    osb, sc, eps, _ = WIC.dual_load(os.path.abspath(fixture_of(tag)[0]), os.path.abspath(rd), "prehold")
    out, prev = {}, None
    for ep in eps:
        vals = WIC.dual_sat_values(ep, osb, sc, bands, True)
        if prev is not None:
            sats = sorted(s for s in vals if s[0] == "G" and s in prev)
            row = {}
            for i in range(len(sats)):
                for j in range(i + 1, len(sats)):
                    now = WIC.dual_pair_channels(vals[sats[i]], vals[sats[j]])
                    was = WIC.dual_pair_channels(prev[sats[i]], prev[sats[j]])
                    s1, s2 = now["E1"] - was["E1"], now["E2"] - was["E2"]
                    row[(sats[i], sats[j])] = {"E1": s1, "E2": s2, "WL": s1 - s2}
            out[ep["tow"]] = row
        prev = vals
    return out, CFG.load_chain_events(rd), bands


def wrap(x):
    return abs(x - round(x))


def cmd_event(a):
    """The undeclared pairs of one event epoch: their step, the same step less the control's, and the level
    the same pairs reach at the no-event epochs of the prior 10 min (cards IRDD and IRDE's attribution)."""
    fix, ctl = a.tags[0], a.tags[1]
    steps, events, bands = pair_steps(fix)
    csteps = pair_steps(ctl)[0]
    mode = fixture_of(fix)[1]
    bar_ch, bar = ("E1", 0.02) if mode == "microscope" else ("WL", 0.05)
    for tow in a.tow:
        ev = events.get(round(tow, 3)) or {"classes": {}, "declared": set()}
        decl = ev["declared"]

        def declared(s, ch):
            sigs = {"E1": (bands[0],), "E2": (bands[1],), "WL": bands}[ch]
            return any((s, g) in decl or (s, None) in decl for g in sigs)

        row, crow = steps.get(tow, {}), csteps.get(tow, {})
        und = {ch: [p for p in row if not declared(p[0], ch) and not declared(p[1], ch)] for ch in ("E1", "E2", "WL")}
        quiet = [t for t in sorted(steps) if tow - 600.0 <= t < tow and not (events.get(round(t, 3)) or {}).get("classes")]
        print("%s at TOW %.0f (%s; bar %s %.2f; control %s): classes %s, %d declared key(s)"
              % (fix, tow, mode, bar_ch, bar, ctl, dict(ev["classes"]) or "none", len(decl)))
        for ch in ("E1", "E2", "WL"):
            ps = und[ch]
            if not ps:
                print("    %s  no undeclared pair" % ch)
                continue
            raw = [row[p][ch] for p in ps]
            diff = [row[p][ch] - crow[p][ch] for p in ps if p in crow]
            qmax = [max(wrap(steps[t][p][ch]) for p in ps if p in steps[t]) for t in quiet
                    if any(p in steps[t] for p in ps)]
            print("    %s  n %3d  step: max |wrap| %.3f, over the bar %3d, integer %3d | less the control: n %3d, "
                  "p50 %.3f, max %.3f, integer %3d | quiet epochs (%d): p50 of the per-epoch max %.3f, largest %.3f"
                  % (ch, len(ps), max(wrap(x) for x in raw), sum(1 for x in raw if wrap(x) > (bar if ch == bar_ch else 1.0)),
                     sum(1 for x in raw if round(x) != 0), len(diff),
                     st.median(abs(x) for x in diff) if diff else float("nan"),
                     max((abs(x) for x in diff), default=float("nan")),
                     sum(1 for x in diff if round(x) != 0), len(qmax),
                     st.median(qmax) if qmax else float("nan"), max(qmax, default=float("nan"))))
    return 0


# ----------------------------------------------------------------------------- the hold's two commands
def cmd_hold(a):
    """The release conditions of the hold of card IRDC section 7.3: ird_frame_census.py cert and nees."""
    for tag in a.tags:
        fx = fixture_of(tag)[0]
        out = []
        for mode in ("cert", "nees"):
            text = cached("%s_%s.txt" % (mode, tag),
                          [sys.executable, HERE + "/ird_frame_census.py", mode, run_dir(tag), fx], a.fresh)
            keep = [ln.strip() for ln in text.splitlines()
                    if ln.startswith(("would-be certification:", "WOULD-BE CERTIFICATION", "CERTIFIER ON HONEST FLOATS"))
                    or "determined" in ln and ln.lstrip().startswith("band ")]
            out += keep or ["%s: no verdict line (%s)" % (mode, text.strip().splitlines()[-1][:160] if text.strip() else "empty")]
        for ln in out:
            print("%-18s %s" % (tag, ln[:a.width]))
    return 0


# ----------------------------------------------------------------------------- summary and root
def cmd_summary(a):
    for tag in a.tags:
        rd = run_dir(tag)
        ex = open(rd + "/exitcode.txt").read().split() if os.path.exists(rd + "/exitcode.txt") else ["?", "?"]
        row = next(csv.DictReader(open(rd + "/logs/network_run_summary.csv", newline="")), {}) \
            if os.path.exists(rd + "/logs/network_run_summary.csv") else {}
        print("%-18s exit %s, emitted %s, skipped %s (gate %s, sigma0 %s), blocks dropped %s, %s ms/epoch"
              % (tag, ex[0], row.get("n_emitted", "?"), row.get("n_skipped", "?"), row.get("n_gate_withheld", "?"),
                 row.get("n_sigma0_withheld", "?"), row.get("n_sta_pd_dropped", "?"), row.get("ms_per_epoch", "?")))
        if not a.lines:
            continue
        pats = [re.compile(p) for p in a.lines]
        for line in open(rd + "/stdout.txt", encoding="utf-8", errors="replace"):
            if any(p.search(line) for p in pats):
                print("    " + line.split("] ", 3)[-1].rstrip()[:a.width])
    return 0


def cmd_root(a):
    for tag in a.tags:
        n = 0
        with open(run_dir(tag) + "/logs/network_events.csv", newline="") as fh:
            for r in csv.reader(fh):
                if len(r) > 3 and r[2] == "CHAIN" and r[3] == "root_handover":
                    n += 1
                    print("%-18s %s" % (tag, ",".join(r)[:a.width]))
        if n == 0:
            print("%-18s no root_handover row" % tag)
    return 0


def selftest():
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    text = ("  I-b kept           n   51  FAIL: WL max 0.500 > 0.05 (4828 pairs); 5836 undeclared integer step(s)\n"
            "  I-b split          n    1  PASS\n"
            "  I-b root_handover  n    1  FAIL: E1 max 0.023 > 0.02 (4 pairs)\n")
    r = parse_ib(text)
    chk(r["kept"] == {"n": 51, "verdict": "FAIL", "max": {"WL": (0.5, 4828)}, "integer": 5836},
        "a failed class: its count, the channel over the bar, the pairs and the integer steps")
    chk(r["split"]["verdict"] == "PASS" and r["split"]["integer"] == 0 and not r["split"]["max"],
        "a passed class has no channel over the bar and no integer step")
    chk(r["root_handover"]["max"] == {"E1": (0.023, 4)} and r["root_handover"]["integer"] == 0,
        "a fractional miss with no integer step")
    chk(bed_of("irdg_f0_r3c") == "r3c" and bed_of("irdg_hw_b_gs") == "gs", "the bed is the tag's last field")
    chk(fixture_of("irdg_f0_mic")[1] == "microscope" and fixture_of("irdg_f0_rgm")[1] == "microscope"
        and fixture_of("irdg_f0_r3c")[1] == "free" and fixture_of("irdg_f0_gs")[0].endswith(GOLDEN_SIM_FIXTURE),
        "the microscope beds are graded per band, the others on the wide lane; the golden bed has its fixture")
    print("irdg_grade selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")   # a console code page that lacks a log line's character
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    cmds = {"ib": cmd_ib, "ibcmp": cmd_ibcmp, "sigma": cmd_sigma, "dcb": cmd_dcb, "fixes": cmd_fixes, "consumer": cmd_consumer,
            "heldout": cmd_heldout, "hold": cmd_hold, "event": cmd_event, "identity": cmd_identity,
            "zwd": cmd_zwd, "stop": cmd_stop, "reref": cmd_reref, "summary": cmd_summary, "root": cmd_root}
    for name in cmds:
        p = sub.add_parser(name)
        p.add_argument("tags", nargs="+")
        p.add_argument("--fresh", action="store_true")
        p.add_argument("--width", type=int, default=400)
        if name == "heldout":
            p.add_argument("--ref", help="the reference wire (the no-chain wire of the bed)")
            p.add_argument("--sites", action="store_true")
        if name == "summary":
            p.add_argument("--lines", action="append", help="also print the stdout lines matching this regex")
        if name in ("ib", "ibcmp"):
            p.add_argument("--classes", nargs="*", default=[], help="print these event classes only")
        if name == "identity":
            p.add_argument("--show", type=int, default=0, help="print up to this many differing lines per side")
        if name == "event":
            p.add_argument("--tow", type=float, action="append", required=True,
                           help="the event epoch (GPS seconds of week); the two tags are the fixture arm and its control")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd in cmds:
        return cmds[a.cmd](a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
