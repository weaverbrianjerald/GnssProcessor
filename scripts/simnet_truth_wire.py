#!/usr/bin/env python3
"""Truth wire (card IRD section 8): a producer's products.txt rewritten with the simulator's
truth, the positive control of the wire and consumer graders and the coverage ceiling of the
consumer's integer search.

Every record that carries a value is replaced:

    C,sat,<clk>                  the injected satellite-clock error (truth_satclock.csv, metres;
                                 0 when the fixture has none: the simulator's satellite clock is
                                 then the a-priori product's, which the C record corrects)
    B,sat,sig,<osb>              truth_bias.csv osb_m of the code signal
    P,sat,sig,<osb + lambda k>,1,1,0,gen=1,cmp=<c>
                                 truth_bias.csv osb_m of the phase signal, signal_integer 1,
                                 wide_lane_integer 1, phase_disc 0, one generation, one
                                 component; k = 0, or a per-(satellite, signal) integer gauge
                                 with --integer-gauge SEED (an integer-recovering frame, which a
                                 consumer must fix to the same integers up to that gauge)
    K,sat,<clk>                  as C
    S line                       amb_datum (field 5) = 2 (SingleReceiverChain); the cbpin=,
                                 gfpin=, sat_gf_datum= and pbhold= declarations removed (the
                                 truth carries the full OSB, no pinned combination, and no hold);
                                 the token truthwire=1 added (unknown tokens are ignorable)
    R records                    dropped (the truth has no discontinuity)

The epoch structure (E, H, G, I, U, N, F) is kept, so a Q5 replay of the truth wire sees the
template's epochs and satellite sets.  A satellite without truth OSB is dropped from every
record and counted.  With the x-corrected grade of scripts/simnet_wire_integer_consistency.py
(dual-band mode) the truth wire grades exactly 0 on every pair and band (IRD-0 gate 3).

Usage:
    python scripts/simnet_truth_wire.py --fixture runs/simarl_r1c
        --template runs/_rap_arl_r1c/products.txt --out <path> [--integer-gauge SEED] [--cmp 1]
    python scripts/simnet_truth_wire.py --selftest
"""
import argparse
import os
import random
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_wire_integer_consistency as WIC  # noqa: E402  (truth loaders, frequencies)

DROP_S_TOKENS = ("cbpin=", "gfpin=", "sat_gf_datum=", "pbhold=")
AMB_DATUM_FIELD = 4          # S,ref_rx,bias_sign,windup_id,amb_datum,...  (0-based index)
AMB_DATUM_CHAIN = "2"        # AmbDatum::SingleReceiverChain


def gauge_k(seed, sat, sig):
    """The integer gauge of one (satellite, signal): a draw in [-40, 40] keyed on the seed."""
    return random.Random("%s|%s|%s" % (seed, sat, sig)).randint(-40, 40)


def rewrite(fixture, template, out, gauge_seed=None, cmp="1"):
    """Write the truth wire; returns a census Counter."""
    stations = WIC.station_dirs(fixture)
    if not stations:
        raise SystemExit("no station with truth_bias.csv under %s" % fixture)
    osb, disagree = WIC.load_osb_truth(stations)
    if disagree:
        raise SystemExit("truth_bias.csv disagrees across stations (%d keys)" % disagree)
    sc_truth, _, _ = WIC.load_satclock_truth(stations)
    census = Counter()
    tow = None
    dropped = set()
    with open(template, encoding="utf-8", errors="replace") as fi, \
            open(out, "w", encoding="utf-8", newline="\n") as fo:
        for raw in fi:
            line = raw.rstrip("\n").rstrip("\r")
            if not line or line.startswith("#"):
                fo.write(line + "\n")
                continue
            p = line.split(",")
            rec = p[0]
            if rec == "S":
                q = [f for f in p if not f.startswith(DROP_S_TOKENS)]
                q[AMB_DATUM_FIELD] = AMB_DATUM_CHAIN
                q.append("truthwire=1")
                fo.write(",".join(q) + "\n")
            elif rec == "E":
                tow = round(float(p[2]), 3)
                fo.write(line + "\n")
            elif rec in ("C", "K"):
                sat = p[1]
                if not any(k[0] == sat for k in osb):
                    dropped.add(sat)
                    census["dropped_" + rec] += 1
                    continue
                fo.write("%s,%s,%.10f\n" % (rec, sat, sc_truth.get((tow, sat), 0.0)))
            elif rec == "B":
                key = (p[1], p[2])
                if key not in osb:
                    dropped.add(p[1])
                    census["dropped_B"] += 1
                    continue
                fo.write("B,%s,%s,%.10f\n" % (p[1], p[2], osb[key]))
            elif rec == "P":
                key = (p[1], p[2])
                if key not in osb:
                    dropped.add(p[1])
                    census["dropped_P"] += 1
                    continue
                v = osb[key]
                if gauge_seed is not None:
                    v += gauge_k(gauge_seed, p[1], p[2]) * WIC.C_LIGHT / WIC.dual_freq(p[1], p[2])
                fo.write("P,%s,%s,%.10f,1,1,0,gen=1,cmp=%s\n" % (p[1], p[2], v, cmp))
            elif rec == "R":
                census["dropped_R"] += 1
                continue
            else:
                fo.write(line + "\n")
            census[rec] += 1
    census["sats_dropped"] = len(dropped)
    return census


def selftest():
    import tempfile
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    sats = ["G01", "G02", "G03", "G04"]
    truth = {(s, g): 0.1 * k + 0.01 * j for k, s in enumerate(sats)
             for j, g in enumerate(("C1C", "C2W", "L1C", "L2W"))}
    with tempfile.TemporaryDirectory() as td:
        fx = os.path.join(td, "fx")
        os.makedirs(os.path.join(fx, "S01000SIM"))
        with open(os.path.join(fx, "S01000SIM", "truth_bias.csv"), "w", newline="") as fh:
            fh.write("satellite,signal,osb_ns,osb_m,product_m,synth_m,source\n")
            for (s, g), v in truth.items():
                fh.write("%s,%s,0,%.9f,%.9f,0,product\n" % (s, g, v, v))
        tpl = os.path.join(td, "template.txt")
        with open(tpl, "w", newline="") as fh:
            fh.write("# gnss_network_product v2\n")
            fh.write("S,S01000SIM,1,nominal-wu93,0,0,1,0,0.05,1,x,G:G01,cbjump=1,"
                     "cbpin=G:C1C-C2W,sat_gf_datum=broadcast_tgd,pbhold=2\n")
            for e in range(120):
                fh.write("E,2313,%.7f,0,0,0,0\n" % (261030.0 + 30 * e + 1e-5))
                for s in sats + ["G99"]:
                    fh.write("C,%s,%.4f\nU,%s,0.2\n" % (s, 3.0 + e * 0.01, s))
                    for g in ("C1C", "C2W"):
                        fh.write("B,%s,%s,%.4f\n" % (s, g, -3.0))
                    for g in ("L1C", "L2W"):
                        fh.write("P,%s,%s,%.4f,1,0,gen=%d\n" % (s, g, -5.0, 1 + e // 60))
                fh.write("H,2313,%.3f,ok,3.0,5,-\n" % (261030.0 + 30 * e))
                if e == 7:
                    fh.write("R,G02\n")
        out = os.path.join(td, "truth.txt")
        c = rewrite(fx, tpl, out)
        lines = open(out).read().splitlines()
        s_line = next(l for l in lines if l.startswith("S,")).split(",")
        chk(s_line[AMB_DATUM_FIELD] == "2" and "truthwire=1" in s_line
            and not any(f.startswith(DROP_S_TOKENS) for f in s_line),
            "S line: amb_datum 2, pinned-combination and hold declarations removed")
        chk(c["sats_dropped"] == 1 and c["dropped_R"] == 1 and c["E"] == 120 and c["H"] == 120,
            "G99 (no truth) dropped, the R record dropped, the epoch structure kept")
        chk(all(float(l.split(",")[2]) == 0.0 for l in lines if l.startswith("C,")),
            "C = 0 without truth_satclock.csv")
        chk(all(l.endswith("gen=1,cmp=1") and l.split(",")[4] == "1" for l in lines
                if l.startswith("P,")), "P: signal_integer 1, one generation, cmp=1")
        osb, sc, eps, _ = WIC.dual_load(fx, out)
        res = WIC.dual_grade(eps, osb, sc, mc=50)
        chk(all(res[h][ch]["p99"] < 1e-6 for h in res for ch in WIC.DUAL_CHANNELS),
            "the truth wire grades 0 on every pair and channel")
        out2 = os.path.join(td, "truth_g.txt")
        rewrite(fx, tpl, out2, gauge_seed=7)
        osb, sc, eps2, _ = WIC.dual_load(fx, out2)
        res2 = WIC.dual_grade(eps2, osb, sc, mc=50)
        v = WIC.dual_sat_values(eps2[0], osb, sc, ("L1C", "L2W"))
        chk(any(abs(v[s]["E1"]) > 0.5 for s in v)
            and all(res2[h][ch]["p99"] < 1e-6 for h in res2 for ch in WIC.DUAL_CHANNELS),
            "with an integer gauge the raw offsets are integers and the wrapped grade is 0")
    print("truth-wire selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture")
    ap.add_argument("--template")
    ap.add_argument("--out")
    ap.add_argument("--integer-gauge", type=int, default=None,
                    help="add lambda * k(seed, sat, sig) to every P (an integer frame)")
    ap.add_argument("--cmp", default="1")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not (a.fixture and a.template and a.out):
        ap.error("--fixture, --template and --out are required (or --selftest)")
    c = rewrite(os.path.abspath(a.fixture), a.template, a.out, a.integer_gauge, a.cmp)
    print("truth wire %s from %s: %s" % (a.out, a.template,
                                          ", ".join("%s=%d" % kv for kv in sorted(c.items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
