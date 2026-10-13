#!/usr/bin/env python3
"""Consumer integer grade against truth (card IRD section 8; bars K-a to K-c, IRD-0 gate 4).

Two modes, one truth convention:

  N_c(sat, sig, t) = N_int + slips(<= t) + k,   k = round(wu_sim(t0))

the consumer's integer: the simulator's slip-accumulated integer (truth_ambiguities.csv,
truth_slips.csv) plus the wind-up branch of the consumer's own arc, the simulator's accumulated
wind-up at the arc's first consumer epoch (truth_windup.csv; card ARL sections 4 and 5).  A
consumer arc starts at the first epoch a (sat, sig) has an ambiguity in
logs/closed_loop_sat_trace.csv and after any gap longer than the accumulator's 300 s horizon.
Under fractional wind-up (estimation.accumulate_phase_windup false) the branch is taken per
epoch instead.  The wire's integer offset of a pair is round(D_sp), D_sp = E^s - E^p of the
x-corrected wire (scripts/simnet_wire_integer_consistency.py dual-band mode, one owner).

fixes (logs/closed_loop_ar_fixes.csv, card IRD-1: week,tow,system,signal,sat,ref_sat,cmp,
       float_cyc,cond_sigma_cyc,fixed_int,determined,kept,z_fixed,z_total,ratio,p_s,gen_sat,
       gen_ref; signal WL for a wide-lane determination):
    a fix is correct if and only if fixed_int = (N_c^sat - N_c^ref) + round(D_sp)
    (WL: the L1 minus L2 integers and D_WL = D_1 - D_2); a pair with |wrap(D_sp)| > 0.25 is
    ungradeable (the wire's own fraction decides the rounding); settled = both arcs at least
    --settle-min old (30 min); the 95 % Wilson upper bound (z 1.96) on the wrong rate; the
    determined share of settled pairs per class (K-b); with logs/closed_loop_ar_epoch.csv the
    fixed and float 3D by hour and the time to 3D < 0.10 m (K-c).

float (the consumer's float, closed_loop_sat_trace.csv amb_m):
    ec = (amb_s - amb_ref)/lambda - (N_c^s - N_c^ref), unwrapped, per epoch and band against the
    epoch's first satellite, regressed on the wire's E_s - E_ref; IRD-0 gate 4 requires the
    slope in [0.95, 1.05] on both bands (window from --from-hms, 03:00:30 by default).

Usage:
    python scripts/consumer_ar_grade.py fixes <fixture> --run <run> [--wire <products>]
        [--station SZZ000SIM] [--settle-min 30] [--start-hms 00:30:15]
    python scripts/consumer_ar_grade.py float <fixture> --run <run> [--from-hms 03:00:30]
    python scripts/consumer_ar_grade.py --selftest
"""
import argparse
import bisect
import csv
import json
import math
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_wire_integer_consistency as WIC  # noqa: E402  (the wire algebra's one owner)
from simnet_amb_integer import load_truth, n_at  # noqa: E402

WINDUP_HORIZON_S = 300.0
WILSON_Z = 1.96
FIX_HEADER = ("week", "tow", "system", "signal", "sat", "ref_sat", "cmp", "float_cyc",
              "cond_sigma_cyc", "fixed_int", "determined", "kept", "z_fixed", "z_total",
              "ratio", "p_s", "gen_sat", "gen_ref")


def round_half_up(x):
    return int(math.floor(x + 0.5))


def wilson_upper(wrong, n, z=WILSON_Z):
    """The upper end of the Wilson score interval of wrong / n (two-sided, z 1.96)."""
    if n <= 0:
        return float("nan")
    p = wrong / n
    den = 1.0 + z * z / n
    return (p + z * z / (2 * n) + z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / den


def consumer_station(run, default="SZZ000SIM"):
    p = os.path.join(run, "arm.json")
    try:
        return json.load(open(p))["network"]["closed_loop"]["station"]["name"]
    except (OSError, KeyError, ValueError):
        return default


def accumulated_windup(run):
    p = os.path.join(run, "arm.json")
    try:
        return bool(json.load(open(p)).get("estimation", {}).get("accumulate_phase_windup", True))
    except (OSError, ValueError):
        return True


def load_consumer_trace(run):
    """{(tow, sat, sig): amb_m} and {sat: sorted tows} from logs/closed_loop_sat_trace.csv."""
    amb, tows = {}, defaultdict(set)
    p = os.path.join(run, "logs", "closed_loop_sat_trace.csv")
    with open(p, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(l for l in fh if not l.startswith("#")):
            t = round(float(r["tow"]), 3)
            tows[r["sat"]].add(t)
            if r.get("amb_m", "") != "":
                amb[(t, r["sat"], r["sig"])] = float(r["amb_m"])
    return amb, {s: sorted(v) for s, v in tows.items()}


class Branches:
    """The consumer's wind-up branch per (sat, tow) and its arc starts."""

    def __init__(self, fixture, station, sat_tows, accumulated=True):
        self.acc = accumulated
        self.wu = defaultdict(dict)
        p = os.path.join(fixture, station, "truth_windup.csv")
        if os.path.isfile(p):
            with open(p, newline="") as fh:
                for r in csv.DictReader(fh):
                    self.wu[r["satellite"]][round(float(r["tow_s"]), 3)] = float(r["windup_cycles"])
        self.arcs = {}
        for sat, ts in sat_tows.items():
            starts = [0] + [i for i in range(1, len(ts)) if ts[i] - ts[i - 1] > WINDUP_HORIZON_S]
            self.arcs[sat] = [ts[i] for i in starts]

    def arc_start(self, sat, tow):
        st = self.arcs.get(sat, [])
        i = bisect.bisect_right(st, tow + 1e-6) - 1
        return st[i] if i >= 0 else None

    def k(self, sat, tow):
        t0 = tow if not self.acc else self.arc_start(sat, tow)
        w0 = self.wu.get(sat, {}).get(t0) if t0 is not None else None
        return round_half_up(w0) if w0 is not None else 0


def truth_n(n_int, slips, br, station, sat, sig, tow):
    base = n_at(n_int, slips, station, sat, sig, tow)
    return None if base is None else base + br.k(sat, tow)


def wire_values(fixture, wire_path, bands):
    """{tow: {sat: dual_sat_values row}} of the x-corrected wire."""
    osb, sc, eps, _ = WIC.dual_load(fixture, wire_path)
    return {ep["tow"]: WIC.dual_sat_values(ep, osb, sc, bands) for ep in eps}


# ----------------------------------------------------------------------------- float
def grade_float(fixture, run, from_tow=None, bands=("L1C", "L2W"), station=None):
    station = station or consumer_station(run)
    amb, sat_tows = load_consumer_trace(run)
    br = Branches(fixture, station, sat_tows, accumulated_windup(run))
    n_int, slips = load_truth(fixture, [station])
    wire = wire_values(fixture, os.path.join(run, "products.txt"), bands)
    if from_tow is None:
        from_tow = min(wire)
    acc = defaultdict(list)
    by_t = defaultdict(dict)
    for (t, s, g), v in amb.items():
        by_t[t][(s, g)] = v
    for t in sorted(by_t):
        if t < from_tow - 1e-3 or t not in wire:
            continue
        d, w = by_t[t], wire[t]
        for bi, g in enumerate(bands):
            lam = WIC.C_LIGHT / WIC.dual_freq("G", g)
            sats = sorted(s for (s, gg) in d if gg == g and s in w
                          and truth_n(n_int, slips, br, station, s, g, t) is not None)
            if len(sats) < 2:
                continue
            ref = sats[0]
            nref = truth_n(n_int, slips, br, station, ref, g, t)
            for s in sats[1:]:
                ec = (d[(s, g)] - d[(ref, g)]) / lam - (truth_n(n_int, slips, br, station, s, g, t) - nref)
                key = "E%d" % (bi + 1)
                acc[(g, "c")].append(ec)
                acc[(g, "E")].append(w[s][key] - w[ref][key])
    out = {}
    for g in bands:
        c, e = np.asarray(acc[(g, "c")]), np.asarray(acc[(g, "E")])
        if c.size < 3:
            out[g] = {"n": int(c.size)}
            continue
        res = c - e
        out[g] = {"n": int(c.size), "slope": float(np.polyfit(e, c, 1)[0]),
                  "r": float(np.corrcoef(e, c)[0, 1]), "resid_sd": float(res.std()),
                  "resid_wrap_p50": float(np.median(np.abs(WIC.wrapf(res))))}
    return out


# ----------------------------------------------------------------------------- fixes
def load_fixes(run):
    p = os.path.join(run, "logs", "closed_loop_ar_fixes.csv")
    with open(p, encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(l for l in fh if not l.startswith("#"))
        hdr = tuple(next(rd))
        if hdr[:len(FIX_HEADER)] != FIX_HEADER:
            raise SystemExit("unexpected closed_loop_ar_fixes.csv header: %s" % (hdr,))
        return [dict(zip(hdr, r)) for r in rd if len(r) >= len(FIX_HEADER)]


def grade_fixes(fixture, run, wire_path=None, bands=("L1C", "L2W"), station=None,
                settle_min=30.0, start_tow=None):
    station = station or consumer_station(run)
    _, sat_tows = load_consumer_trace(run)
    br = Branches(fixture, station, sat_tows, accumulated_windup(run))
    n_int, slips = load_truth(fixture, [station])
    wire = wire_values(fixture, wire_path or os.path.join(run, "products.txt"), bands)
    rows = load_fixes(run)
    if start_tow is None:
        start_tow = min(wire) if wire else 0.0
    cls = defaultdict(lambda: {"rows": 0, "settled": 0, "determined": 0, "graded": 0,
                               "wrong": 0, "ungradeable": 0, "no_truth": 0, "wrong_list": []})
    for r in rows:
        t = round(float(r["tow"]), 3)
        sig, s, p = r["signal"], r["sat"], r["ref_sat"]
        c = cls[sig]
        c["rows"] += 1
        a_s, a_p = br.arc_start(s, t), br.arc_start(p, t)
        if a_s is None or a_p is None or min(t - a_s, t - a_p) < settle_min * 60.0:
            continue
        c["settled"] += 1
        if not (r["determined"] == "1" and r["kept"] == "1"):
            continue
        c["determined"] += 1
        w = wire.get(t, {})
        if s not in w or p not in w:
            c["no_truth"] += 1
            continue
        if sig == "WL":
            nt = [truth_n(n_int, slips, br, station, x, g, t) for x in (s, p) for g in bands]
            if None in nt:
                c["no_truth"] += 1
                continue
            dn = (nt[0] - nt[1]) - (nt[2] - nt[3])
            d = (w[s]["E1"] - w[p]["E1"]) - (w[s]["E2"] - w[p]["E2"])
        else:
            if sig not in bands:
                continue
            key = "E%d" % (bands.index(sig) + 1)
            ns, np_ = (truth_n(n_int, slips, br, station, x, sig, t) for x in (s, p))
            if ns is None or np_ is None:
                c["no_truth"] += 1
                continue
            dn, d = ns - np_, w[s][key] - w[p][key]
        if abs(WIC.wrapf(d)) > 0.25:
            c["ungradeable"] += 1
            continue
        c["graded"] += 1
        if int(r["fixed_int"]) != dn + round(d):
            c["wrong"] += 1
            if len(c["wrong_list"]) < 20:
                c["wrong_list"].append((t, s, p, int(r["fixed_int"]), dn + round(d), round(d, 3)))
    for c in cls.values():
        c["wilson_upper"] = wilson_upper(c["wrong"], c["graded"])
        c["determined_share"] = c["determined"] / c["settled"] if c["settled"] else float("nan")
    out = {"station": station, "classes": dict(cls)}
    ep = os.path.join(run, "logs", "closed_loop_ar_epoch.csv")
    if os.path.isfile(ep):
        out["position"] = position_grade(ep, start_tow)
    return out


def position_grade(path, start_tow):
    """Fixed and float 3D by hour, the first epoch below 0.10 m and the first sustained 5 cm
    (10 consecutive epochs), for the conditioned and the float position (bar K-c)."""
    hours = defaultdict(lambda: {"fix": [], "flt": []})
    first = {"fix": None, "flt": None}
    run5 = {"fix": 0, "flt": 0}
    sus5 = {"fix": None, "flt": None}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(l for l in fh if not l.startswith("#")):
            t = float(r["tow"])
            if t < start_tow - 1e-3:
                continue
            h = int((t - start_tow) // 3600)
            for k, pre in (("fix", "fix_"), ("flt", "float_")):
                try:
                    e3 = math.sqrt(sum(float(r[pre + a]) ** 2 for a in "enu"))
                except (ValueError, KeyError):
                    continue
                hours[h][k].append(e3)
                if first[k] is None and e3 < 0.10:
                    first[k] = t - start_tow
                run5[k] = run5[k] + 1 if e3 < 0.05 else 0
                if sus5[k] is None and run5[k] >= 10:
                    sus5[k] = t - start_tow
    return {"hours": {h: {k: float(np.median(v)) if v else float("nan") for k, v in d.items()}
                      for h, d in sorted(hours.items())},
            "first_below_0p10_s": first, "sustained_5cm_s": sus5}


def print_fixes(res):
    print("consumer integer grade at %s (Wilson z %.2f)" % (res["station"], WILSON_Z))
    for sig, c in sorted(res["classes"].items()):
        print("  %-4s rows %7d settled %7d determined+kept %6d (share %.3f)  graded %6d  "
              "wrong %d  ungradeable %d  no-truth %d  Wilson upper %.2e"
              % (sig, c["rows"], c["settled"], c["determined"], c["determined_share"],
                 c["graded"], c["wrong"], c["ungradeable"], c["no_truth"], c["wilson_upper"]))
        for w in c["wrong_list"]:
            print("       WRONG tow %.1f %s-%s fixed %d true %d (wire D %.3f)" % w)
    if "position" in res:
        pg = res["position"]
        for h, d in pg["hours"].items():
            print("  h%d 3D p50 fixed %.4f float %.4f m" % (h, d["fix"], d["flt"]))
        print("  first below 0.10 m: fixed %s s, float %s s; sustained 5 cm: fixed %s s, float %s s"
              % (pg["first_below_0p10_s"]["fix"], pg["first_below_0p10_s"]["flt"],
                 pg["sustained_5cm_s"]["fix"], pg["sustained_5cm_s"]["flt"]))


# ----------------------------------------------------------------------------- selftest
def selftest():
    import tempfile
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    chk(abs(wilson_upper(0, 1000) - 3.8415 / 1003.8415) < 1e-6,
        "Wilson upper of 0 of 1000 = z^2/(n + z^2) = %.5f" % wilson_upper(0, 1000))
    chk(wilson_upper(0, 12900) < 3e-4 < wilson_upper(0, 12700),
        "0 wrong needs about 12 800 graded integers for a bound below 3e-4")
    f1, f2 = WIC.DUAL_FREQ["G"]["1"], WIC.DUAL_FREQ["G"]["2"]
    lam = {"L1C": WIC.C_LIGHT / f1, "L2W": WIC.C_LIGHT / f2}
    sats = ["G01", "G02", "G03", "G04"]
    N = {(s, g): 10 * k + (3 if g == "L2W" else 0) for k, s in enumerate(sats) for g in lam}
    gauge = {(s, g): (k * 7 - 9) * (1 if g == "L1C" else 2) for k, s in enumerate(sats) for g in lam}
    with tempfile.TemporaryDirectory() as td:
        fx = os.path.join(td, "fx")
        st = "SZZ000SIM"
        os.makedirs(os.path.join(fx, st))
        with open(os.path.join(fx, st, "truth_bias.csv"), "w", newline="") as fh:
            fh.write("satellite,signal,osb_ns,osb_m,product_m,synth_m,source\n")
            for s in sats:
                for g in ("C1C", "C2W", "L1C", "L2W"):
                    fh.write("%s,%s,0,0.0,0.0,0,product\n" % (s, g))
        with open(os.path.join(fx, st, "truth_ambiguities.csv"), "w", newline="") as fh:
            fh.write("satellite,signal,N_int,wavelength_m\n")
            for (s, g), n in N.items():
                fh.write("%s,%s,%d,%.9f\n" % (s, g, n, lam[g]))
        with open(os.path.join(fx, st, "truth_slips.csv"), "w", newline="") as fh:
            fh.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
            fh.write("2313,262830.000,G04,L1C,5,0,40\n")
        t0 = 261030.0
        tows = [t0 + 30 * e for e in range(240)]
        with open(os.path.join(fx, st, "truth_windup.csv"), "w", newline="") as fh:
            fh.write("week,tow_s,satellite,windup_cycles\n")
            for t in tows:
                for s in sats:
                    fh.write("2313,%.3f,%s,%.6f\n" % (t, s, -0.7 if s == "G03" else 0.1))
        run = os.path.join(td, "run")
        os.makedirs(os.path.join(run, "logs"))
        with open(os.path.join(run, "products.txt"), "w", newline="") as fh:
            fh.write("S,X,1,w,2,0,1,0,0.05,1,x\n")
            for t in tows:
                fh.write("E,2313,%.3f,0,0,0,0\n" % t)
                for s in sats:
                    fh.write("C,%s,0.0\nB,%s,C1C,0.0\nB,%s,C2W,0.0\n" % (s, s, s))
                    for g in lam:
                        fh.write("P,%s,%s,%.10f,1,1,0,gen=1\n" % (s, g, gauge[(s, g)] * lam[g]))
        # The consumer: float = lambda (N_c + D) with N_c = N + slips + branch, D = E = -gauge.
        with open(os.path.join(run, "logs", "closed_loop_sat_trace.csv"), "w", newline="") as fh:
            fh.write("week,tow,sat,sig,amb_m\n")
            for t in tows:
                for s in sats:
                    for g in lam:
                        n = N[(s, g)] + (5 if (s, g) == ("G04", "L1C") and t >= 262830 else 0) \
                            + (-1 if s == "G03" else 0)
                        fh.write("2313,%.3f,%s,%s,%.10f\n" % (t, s, g, lam[g] * (n - gauge[(s, g)])))
        res = grade_float(fx, run, from_tow=t0)
        chk(all(abs(res[g]["slope"] - 1.0) < 1e-6 and res[g]["resid_sd"] < 1e-6 for g in lam),
            "float: a consumer at N_c + D regresses on the wire's D with slope 1 (wind-up branch, "
            "slip and integer gauge accounted)")
        with open(os.path.join(run, "logs", "closed_loop_ar_fixes.csv"), "w", newline="") as fh:
            fh.write(",".join(FIX_HEADER) + "\n")
            for t in tows:
                for s in sats[1:]:
                    for g in lam:
                        n = lambda x: (N[(x, g)] + (5 if (x, g) == ("G04", "L1C") and t >= 262830
                                                    else 0) + (-1 if x == "G03" else 0)
                                       - gauge[(x, g)])
                        fixed = n(s) - n("G01")
                        if s == "G02" and g == "L2W" and t == tows[-1]:
                            fixed += 1                                # one planted wrong fix
                        fh.write("2313,%.3f,G,%s,%s,G01,1,0,0.01,%d,1,1,0,0,9,1,1,1\n"
                                 % (t, g, s, fixed))
                    nwl = lambda x: (N[(x, "L1C")] - N[(x, "L2W")]
                                     + (5 if x == "G04" and t >= 262830 else 0)
                                     - gauge[(x, "L1C")] + gauge[(x, "L2W")])
                    fh.write("2313,%.3f,G,WL,%s,G01,1,0,0.01,%d,1,1,0,0,9,1,1,1\n"
                             % (t, s, nwl(s) - nwl("G01")))
        res = grade_fixes(fx, run, settle_min=30.0)
        c1, c2, cw = (res["classes"][k] for k in ("L1C", "L2W", "WL"))
        chk(c1["settled"] == 3 * 180 and c1["graded"] == 540 and c1["wrong"] == 0,
            "fixes: 540 settled L1C fixes graded (arc age >= 30 min), none wrong across the "
            "G04 slip and G03's wind-up branch")
        chk(c2["wrong"] == 1 and c2["wrong_list"][0][1] == "G02",
            "fixes: the one planted wrong L2W fix is found")
        chk(cw["graded"] == 540 and cw["wrong"] == 0, "fixes: WL determinations graded on D_1 - D_2")
    print("consumer AR grade selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", choices=("fixes", "float"))
    ap.add_argument("fixture", nargs="?")
    ap.add_argument("--run")
    ap.add_argument("--wire", default=None, help="fixes: the wire the consumer read "
                    "(default <run>/products.txt; a truth wire for a Q5 replay)")
    ap.add_argument("--station", default=None)
    ap.add_argument("--bands", default="L1C,L2W")
    ap.add_argument("--settle-min", type=float, default=30.0)
    ap.add_argument("--start-hms", default=None)
    ap.add_argument("--from-hms", default="03:00:30", help="float: window start (HH:MM:SS)")
    ap.add_argument("--json", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not (a.mode and a.fixture and a.run):
        ap.error("mode, fixture and --run are required (or --selftest)")
    fixture, run = os.path.abspath(a.fixture), os.path.abspath(a.run)
    bands = tuple(a.bands.split(","))
    _, eps, _ = WIC.parse_products_tokens(os.path.join(run, "products.txt"))
    t_first = eps[0]["tow"]
    if a.mode == "float":
        start = WIC.start_from_hms(t_first, a.from_hms)
        res = grade_float(fixture, run, start, bands, a.station)
        print("consumer float against the wire's E_b, %s, from TOW %.1f" % (run, start))
        for g, d in res.items():
            if "slope" not in d:
                print("  %s n %d: too few samples" % (g, d["n"]))
                continue
            verdict = "PASS" if 0.95 <= d["slope"] <= 1.05 else "FAIL"
            print("  %s n %d slope %.4f r %.4f residual sd %.3f cycle |wrap(resid)| p50 %.3f  "
                  "[0.95, 1.05]: %s" % (g, d["n"], d["slope"], d["r"], d["resid_sd"],
                                        d["resid_wrap_p50"], verdict))
    else:
        start = WIC.start_from_hms(t_first, a.start_hms) if a.start_hms else None
        res = grade_fixes(fixture, run, a.wire, bands, a.station, a.settle_min, start)
        print_fixes(res)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
