#!/usr/bin/env python3
"""Chain-frame grade (card IRD section 8, bar I-b): the truth SD-potential step of every
satellite pair at every chain event epoch, split into declared and undeclared pairs.

The SD potential of a pair (s, p) on band b is D_b = E_b^s - E_b^p, with E_b the x-corrected
wire error of scripts/simnet_wire_integer_consistency.py's dual-band mode (one owner of the
wire algebra): what a consumer's single difference absorbs on top of its integer.  An event
that keeps the frame leaves every D unchanged; a cut release or a re-host on a wrong integer
steps the moved side by an integer, which the producer must declare; an estimate-based shift
steps it by a fraction, which no declaration can repair.

At each epoch t that carries CHAIN rows in logs/network_events.csv (card IRD-1: tier CHAIN,
event one of rehost, cut_release, root_handover, returning, slip_deseat, merge, split, plus
declared_keys rows whose `sat` column and `sig=<signal>` detail token name a declared key; a
declared_keys row without sig= declares every signal of the satellite), the step of every pair
present at t and at the previous graded epoch is

    step = D(t) - D(t_prev)        (unwrapped, cycles; WL = step_1 - step_2)

and a pair is declared when either of its keys is declared at t.  Bar I-b (frozen):

    undeclared pairs: |wrap(step)| <= 0.02 cycle per band (microscope) or 0.05 on the wide
    lane (elsewhere); every integer step (round(step) != 0) declared; every event class
    reaches n >= 1 on its own fixture (an unexercised class reads FAILED).

An epoch with several classes counts for each of them.

Usage:
    python scripts/simnet_chain_frame_grade.py <fixture> --run <run> [--source prehold|products]
        [--bands L1C,L2W] [--mode microscope|free] [--require-class rehost,root_handover]
        [--json out.json]
    python scripts/simnet_chain_frame_grade.py --selftest
"""
import argparse
import csv
import json
import os
import sys
from collections import Counter, defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_wire_integer_consistency as WIC  # noqa: E402  (the wire algebra's one owner)

EVENT_CLASSES = ("rehost", "cut_release", "root_handover", "returning", "slip_deseat",
                 "merge", "split")
BAR_BAND_MICRO = 0.02
BAR_WL = 0.05


def _detail_tokens(detail):
    out = {}
    for tok in (detail or "").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def load_chain_events(run):
    """{tow: {"classes": Counter(event), "declared": {(sat, sig or None)}}} from the CHAIN rows
    of <run>/logs/network_events.csv; empty when the file or the rows are absent."""
    p = os.path.join(run, "logs", "network_events.csv")
    out = {}
    if not os.path.isfile(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(l for l in fh if not l.startswith("#")):
            if r.get("tier") != "CHAIN":
                continue
            tow = round(float(r["tow"]), 3)
            ev = out.setdefault(tow, {"classes": Counter(), "declared": set()})
            if r["event"] == "declared_keys":
                ev["declared"].add((r["sat"], _detail_tokens(r.get("detail")).get("sig")))
            else:
                ev["classes"][r["event"]] += 1
    return out


def _declared(decl, sat, sig):
    return (sat, sig) in decl or (sat, None) in decl


def event_steps(epochs, osb, sc_truth, bands, chain_events, xcorr=True, systems=("G",)):
    """Per event class: {"events": n epochs, channel: {"und": [steps], "dec": [steps]}}."""
    b1, b2 = bands
    res = defaultdict(lambda: {"events": 0, **{c: {"und": [], "dec": []}
                                               for c in ("E1", "E2", "WL")}})
    prev_vals = None
    for ep in epochs:
        vals = WIC.dual_sat_values(ep, osb, sc_truth, bands, xcorr)
        ev = chain_events.get(ep["tow"])
        if ev and prev_vals is not None and ev["classes"]:
            for cls in ev["classes"]:
                res[cls]["events"] += 1
            for sysl in systems:
                sats = sorted(s for s in vals if s[0] == sysl and s in prev_vals)
                for i in range(len(sats)):
                    for j in range(i + 1, len(sats)):
                        s, p = sats[i], sats[j]
                        now = WIC.dual_pair_channels(vals[s], vals[p])
                        was = WIC.dual_pair_channels(prev_vals[s], prev_vals[p])
                        st1, st2 = now["E1"] - was["E1"], now["E2"] - was["E2"]
                        steps = {"E1": st1, "E2": st2, "WL": st1 - st2}
                        dec = {"E1": _declared(ev["declared"], s, b1)
                               or _declared(ev["declared"], p, b1),
                               "E2": _declared(ev["declared"], s, b2)
                               or _declared(ev["declared"], p, b2)}
                        dec["WL"] = dec["E1"] or dec["E2"]
                        for cls in ev["classes"]:
                            for c in ("E1", "E2", "WL"):
                                res[cls][c]["dec" if dec[c] else "und"].append(steps[c])
        prev_vals = vals
    return dict(res)


def verdict(res, mode="free", require=()):
    """Bar I-b per class: (rows, failed).  mode "microscope" grades E1 and E2 at 0.02 cycle and
    the wide lane at 0.05; "free" grades the wide lane at 0.05 (the bands are reported)."""
    rows, failed = [], False
    graded = (("E1", BAR_BAND_MICRO), ("E2", BAR_BAND_MICRO), ("WL", BAR_WL)) \
        if mode == "microscope" else (("WL", BAR_WL),)
    for cls in sorted(set(res) | set(require)):
        r = res.get(cls)
        if r is None or r["events"] == 0:
            rows.append((cls, 0, "FAILED (class not exercised)" if cls in require else "n = 0"))
            failed |= cls in require
            continue
        bad = []
        n_int_und = 0
        for c in ("E1", "E2", "WL"):
            und = np.asarray(r[c]["und"])
            n_int_und += int(np.sum(np.round(und) != 0)) if und.size else 0
        for c, bar in graded:
            und = np.abs(WIC.wrapf(np.asarray(r[c]["und"])))
            if und.size and float(und.max()) > bar:
                bad.append("%s max %.3f > %.2f (%d pairs)" % (c, float(und.max()), bar,
                                                               int(np.sum(und > bar))))
        if n_int_und:
            bad.append("%d undeclared integer step(s)" % n_int_und)
        rows.append((cls, r["events"], "PASS" if not bad else "FAIL: " + "; ".join(bad)))
        failed |= bool(bad)
    return rows, failed


def print_event_steps(res, mode="free", require=()):
    """Print the per-class step table and bar I-b's verdict; returns True when it fails."""
    print("chain-frame steps at event epochs (D(t) - D(t_prev), cycles; mode %s)" % mode)
    if not res and not require:
        print("  no CHAIN rows: nothing to grade (network.trace_chain_datum, card IRD-1)")
        return False
    for cls in sorted(res):
        r = res[cls]
        for c in ("E1", "E2", "WL"):
            und = np.asarray(r[c]["und"])
            dec = np.asarray(r[c]["dec"])
            a = np.abs(WIC.wrapf(und)) if und.size else und
            print("  %-14s events %4d  %-2s  undeclared n %6d |wrap| p50 %s p99 %s max %s"
                  "  integer %d   declared n %d integer %d"
                  % (cls, r["events"], c, und.size,
                     "%.4f" % np.median(a) if a.size else "-",
                     "%.4f" % np.percentile(a, 99) if a.size else "-",
                     "%.4f" % a.max() if a.size else "-",
                     int(np.sum(np.round(und) != 0)) if und.size else 0, dec.size,
                     int(np.sum(np.round(dec) != 0)) if dec.size else 0))
    rows, failed = verdict(res, mode, require)
    for cls, n, v in rows:
        print("  I-b %-14s n %4d  %s" % (cls, n, v))
    return failed


def selftest():
    import tempfile
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    f1, f2 = WIC.DUAL_FREQ["G"]["1"], WIC.DUAL_FREQ["G"]["2"]
    l1, l2 = WIC.C_LIGHT / f1, WIC.C_LIGHT / f2
    sats = ["G%02d" % k for k in range(1, 7)]
    truth = {(s, g): 0.01 * k for k, s in enumerate(sats) for g in ("C1C", "C2W", "L1C", "L2W")}
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "fx", "S01000SIM"))
        with open(os.path.join(td, "fx", "S01000SIM", "truth_bias.csv"), "w", newline="") as fh:
            fh.write("satellite,signal,osb_ns,osb_m,product_m,synth_m,source\n")
            for (s, g), v in truth.items():
                fh.write("%s,%s,0,%.9f,%.9f,0,product\n" % (s, g, v, v))
        run = os.path.join(td, "run")
        os.makedirs(os.path.join(run, "logs"))
        t_ev = 261030.0 + 30.0 * 5
        with open(os.path.join(run, "products.txt"), "w", newline="") as fh:
            fh.write("S,S01000SIM,1,w,2,0,1,0,0.05,1,x\n")
            for e in range(10):
                t = 261030.0 + 30.0 * e
                fh.write("E,2313,%.3f,0,0,0,0\n" % t)
                for s in sats:
                    fh.write("C,%s,0.0\n" % s)
                    for g in ("C1C", "C2W"):
                        fh.write("B,%s,%s,%.9f\n" % (s, g, truth[(s, g)]))
                    p1, p2 = truth[(s, "L1C")], truth[(s, "L2W")]
                    if t >= t_ev and s == "G03":
                        p1 += l1                       # declared +1 cycle on L1
                    if t >= t_ev and s == "G04":
                        p1 += 0.3 * l1                 # undeclared fraction (a shift)
                    if t >= t_ev + 60 and s == "G05":
                        p2 += 2 * l2                   # undeclared integer step on L2
                    fh.write("P,%s,L1C,%.9f,1,0,gen=1\nP,%s,L2W,%.9f,1,0,gen=1\n" % (s, p1, s, p2))
        with open(os.path.join(run, "logs", "network_events.csv"), "w", newline="") as fh:
            fh.write("week,tow,tier,event,sat,station,statistic,threshold,action,detail\n")
            fh.write("2313,%.3f,CHAIN,rehost,G03,S01000SIM,,,,sig=L1C edge=S01000SIM/G03\n" % t_ev)
            fh.write("2313,%.3f,CHAIN,declared_keys,G03,,,,,sig=L1C\n" % t_ev)
            fh.write("2313,%.3f,CHAIN,root_handover,,S01000SIM,,,,\n" % (t_ev + 60))
            fh.write("2313,%.3f,PB_HOLD,gen_start,G01,,,,first,sig=L1C\n" % t_ev)
        osb, sc, eps, _ = WIC.dual_load(os.path.join(td, "fx"), run)
        ev = load_chain_events(run)
        chk(set(ev) == {t_ev, t_ev + 60} and ev[t_ev]["declared"] == {("G03", "L1C")},
            "CHAIN rows read (classes and declared keys); other tiers ignored")
        res = event_steps(eps, osb, sc, ("L1C", "L2W"), ev)
        chk(res["rehost"]["events"] == 1 and res["root_handover"]["events"] == 1,
            "one rehost and one root-handover epoch")
        dec = np.asarray(res["rehost"]["E1"]["dec"])
        chk(dec.size == 5 and int(np.sum(np.abs(np.abs(dec) - 1.0) < 1e-6)) == 4
            and int(np.sum(np.abs(np.abs(dec) - 0.7) < 1e-6)) == 1,
            "G03's declared +1 L1 step: 5 declared pairs, 4 of size 1 and G03-G04 of 0.7")
        und = np.abs(WIC.wrapf(np.asarray(res["rehost"]["E1"]["und"])))
        chk(und.size == 10 and abs(und.max() - 0.3) < 1e-6 and int(np.sum(und > 0.02)) == 4,
            "G04's undeclared 0.3 cycle shift reads on its 4 undeclared pairs")
        rows, failed = verdict(res, "microscope")
        chk(failed and any(r[0] == "rehost" and "E1 max 0.300" in r[2] for r in rows),
            "bar I-b fails the fractional step (microscope)")
        und2 = np.asarray(res["root_handover"]["E2"]["und"])
        chk(int(np.sum(np.round(und2) != 0)) == 5,
            "G05's undeclared 2-cycle L2 step is an undeclared integer step on 5 pairs")
        rows, failed = verdict(res, "free", require=("rehost", "merge"))
        chk(any(r[0] == "merge" and r[2].startswith("FAILED") for r in rows),
            "a required class with n = 0 reads FAILED")
        chk(any(r[0] == "root_handover" and "undeclared integer" in r[2] for r in rows),
            "free mode still fails an undeclared integer step")
        empty = event_steps(eps, osb, sc, ("L1C", "L2W"), {})
        chk(empty == {} and not verdict(empty)[1], "no CHAIN rows: nothing graded, nothing failed")
    print("chain-frame selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture", nargs="?")
    ap.add_argument("--run")
    ap.add_argument("--source", choices=("products", "prehold"), default="prehold")
    ap.add_argument("--bands", default="L1C,L2W")
    ap.add_argument("--mode", choices=("microscope", "free"), default="free")
    ap.add_argument("--require-class", default="",
                    help="comma list of event classes that must reach n >= 1 (bar I-b)")
    ap.add_argument("--no-xcorr", dest="xcorr", action="store_false", default=True)
    ap.add_argument("--json", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.fixture or not a.run:
        ap.error("the fixture and --run are required (or --selftest)")
    osb, sc, eps, _ = WIC.dual_load(os.path.abspath(a.fixture), os.path.abspath(a.run), a.source)
    ev = load_chain_events(a.run)
    require = tuple(x for x in a.require_class.split(",") if x)
    res = event_steps(eps, osb, sc, tuple(a.bands.split(",")), ev, a.xcorr)
    print("run %s, source %s, %d epochs, %d CHAIN event epochs" % (a.run, a.source, len(eps), len(ev)))
    failed = print_event_steps(res, a.mode, require)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({"run": a.run, "verdict": verdict(res, a.mode, require)[0]}, fh, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
