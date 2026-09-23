#!/usr/bin/env python3
"""gal_grade.py -- card GAL (GPS and Galileo on the network path) grading instruments.

READ-ONLY.  Reads one run directory (products.txt and logs/) and, on the simulator bed,
the fixture's truth files reached through the RINEX paths of the run's arm.json.
Every section states the file it read.

Sections (each prints only when its inputs exist):

  WIRE     satellites per epoch with a clock (C) record, per constellation, and per
           (constellation, signal) the satellites per epoch with a code-bias (B) and a
           phase-bias (P) record; epochs where a constellation has fewer clock
           satellites than its median are counted.
  GROSS    full-weight gross post-fit CODE rows (information weight >= 1e-2, the
           definition of the manager's gross.py) above 3 m and above 10 m, per
           constellation and per (constellation, signal).
  ISB      the receiver inter-system bias: every station's series from
           logs/network_rx_states.csv (kind ISB, key = system; a free per-epoch local
           column on the producer) and the consumer's from logs/closed_loop_isb.csv
           (a Kalman state), per hour.
  ISBTRUTH (simulator) the consumer's inter-system bias against the value the truth
           and the run's own datum predict:
               Y = ISB_true + IF_rx(Gal) - IF_rx(GPS) - (m_E(t) - m_G(t))
           where IF_rx is the consumer's receiver ionosphere-free code bias on each
           constellation's pair (truth_rx_bias.csv), and m_c(t) is the per-epoch median
           over constellation c's wire satellites of
               E_s(t) = -C_s(t) + IF(B_s(t) - OSB_s)
           (the clock plus ionosphere-free code-bias error of the wire against the
           truth, the quantity a consumer's receiver clock absorbs per constellation).
  INJECT   (a split-family emulation made with rapid_arm.py) the wire's inter-family
           code differences against the injected per-satellite constants:
               D1_s(t) = B_s(C1X) - B_s(C1C),   D5_s(t) = B_s(C5X) - B_s(C5Q)
           graded three ways, each after removing the per-epoch median over satellites
           (the family common mode, which the data cannot reach):
             raw      D1 - b1 and D5 - b5 (the injected constants themselves);
             datum    against the constants transformed into the datum the run
                      realized (RF-0d: C5X pinned through its geometry-free combination
                      against C1C at the pair's target, so D5 = 0 and
                      D1 = b1 + (a2/a1) b5);
             IF       a1 D1 + a2 D5 against a1 b1 + a2 b5, the one combination no
                      geometry-free datum choice can move.
           The inter-family geometry-free difference (b5 - b1) is observed by no
           station (the two families share no receiver, and the slant ionosphere is
           free per line of sight), so the 'raw' grade carries it whatever datum the
           producer chooses; the 'datum' and 'IF' grades do not.  The FROZEN-SPLIT
           test (card SID section 12): per satellite, the error at the end of hour 1,
           3 and 6 beside the walk of the combined-family receivers' inter-system bias
           and Galileo receiver DCB over hours 2 to 6.

Usage:
    python scripts/gal_grade.py <run_dir> [--sections wire,gross,isb,isbtruth,inject]
"""
import argparse
import csv
import json
import math
import os
import re
import statistics as st
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

C_LIGHT = 299792458.0
FREQ = {"G": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6},
        "E": {"1": 1575.42e6, "5": 1176.45e6, "7": 1207.14e6}}
T0 = 259200.0   # DOY-129 00:00:00 GPS seconds of week (the hour bins of wtzz.py)


def hour_of(tow):
    return int(round((tow - T0) * 1000)) // 3600000


def if_weights(c, sig1, sig2):
    f1 = FREQ[c][sig1[1]]
    f2 = FREQ[c][sig2[1]]
    den = f1 * f1 - f2 * f2
    return f1 * f1 / den, -f2 * f2 / den


def med(v):
    return st.median(v) if v else float("nan")


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")


def fmt(x, p=4):
    return "-" if x is None or (isinstance(x, float) and math.isnan(x)) else f"{x:.{p}f}"


# ------------------------------------------------------------------ readers
def read_products(run):
    """products.txt -> {tow: {"C": {sat: c0}, "B": {(sat,sig): b}, "P": {(sat,sig): p}}}."""
    path = os.path.join(run, "products.txt")
    eps = {}
    cur = None
    if not os.path.exists(path):
        return eps, path
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line or line[0] == "#":
                continue
            f = line.rstrip("\r\n").split(",")
            tag = f[0]
            if tag == "E" and len(f) > 2:
                tow = round(float(f[2]) / 30.0) * 30.0
                cur = eps.setdefault(tow, {"C": {}, "B": {}, "P": {}})
            elif cur is None:
                continue
            elif tag == "C" and len(f) > 2:
                cur["C"][f[1]] = float(f[2])
            elif tag == "B" and len(f) > 3:
                cur["B"][(f[1], f[2])] = float(f[3])
            elif tag == "P" and len(f) > 3:
                cur["P"][(f[1], f[2])] = float(f[3])
    return eps, path


def fixture_dirs(run):
    """Station name -> fixture directory, from the arm's RINEX paths (the ORIGINAL fixture
    directory when a rapid_arm op rewrote the file into the arm's rinex/ folder)."""
    ap = os.path.join(run, "arm.json")
    out = {}
    if not os.path.exists(ap):
        return out
    arm = json.load(open(ap, encoding="utf-8"))
    net = arm.get("network", {})
    entries = list(net.get("stations", []))
    cons = net.get("closed_loop", {}).get("station")
    if cons:
        entries.append(cons)
    rap = os.path.join(run, "rapid.json")
    base_paths = {}
    if os.path.exists(rap):
        try:
            base = json.load(open(json.load(open(rap))["base"], encoding="utf-8"))
            bnet = base.get("network", {})
            for s in bnet.get("stations", []) + [bnet.get("closed_loop", {}).get("station") or {}]:
                if s.get("name"):
                    base_paths[s["name"]] = s.get("rinex_path", "")
        except Exception:
            pass
    for s in entries:
        name = s.get("name")
        p = base_paths.get(name) or s.get("rinex_path", "")
        d = os.path.dirname(p.replace("/", os.sep))
        if os.path.exists(os.path.join(d, "truth_bias.csv")):
            out[name] = d
    return out


def read_truth_bias(d):
    osb = {}
    with open(os.path.join(d, "truth_bias.csv"), encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            osb[(r["satellite"], r["signal"])] = float(r["osb_m"])
    return osb


def read_truth_rx(d):
    out = {}
    p = os.path.join(d, "truth_rx_bias.csv")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                if r["kind"] == "code":
                    out[r["code"]] = float(r["value_m"])
    return out


def read_truth_scalar(d, key):
    p = os.path.join(d, "truth_scalars.csv")
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as fh:
        for r in csv.reader(fh):
            if r and r[0] == key:
                return float(r[1])
    return None


def consumer_pairs(run):
    """The consumer's (code1, code2) per constellation, from closed_loop_sat_trace.csv."""
    p = os.path.join(run, "logs", "closed_loop_sat_trace.csv")
    by = defaultdict(set)
    if not os.path.exists(p):
        return {}
    with open(p, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            cs = r.get("code_sig", "")
            if cs and len(cs) == 3:
                by[r["sat"][:1]].add(cs)
    out = {}
    for c, sigs in by.items():
        s1 = sorted(s for s in sigs if s[1] == "1")
        s2 = sorted(s for s in sigs if s[1] != "1")
        if s1 and s2:
            out[c] = (s1[0], s2[0])
    return out


# ------------------------------------------------------------------ sections
def section_wire(run, eps, path):
    print(f"\n== WIRE COVERAGE  ({path}) ==")
    if not eps:
        print("no epochs")
        return
    per_c = defaultdict(list)
    per_b = defaultdict(list)
    per_p = defaultdict(list)
    sats_c = defaultdict(set)
    for tow in sorted(eps):
        e = eps[tow]
        cc = defaultdict(set)
        for s in e["C"]:
            cc[s[0]].add(s)
            sats_c[s[0]].add(s)
        for c in set(cc) | set(per_c):
            per_c[c].append(len(cc.get(c, ())))
        bb = defaultdict(set)
        for (s, g) in e["B"]:
            bb[(s[0], g)].add(s)
        for k in set(bb) | set(per_b):
            per_b[k].append(len(bb.get(k, ())))
        pp = defaultdict(set)
        for (s, g) in e["P"]:
            pp[(s[0], g)].add(s)
        for k in set(pp) | set(per_p):
            per_p[k].append(len(pp.get(k, ())))
    print(f"epochs {len(eps)}")
    for c in sorted(per_c):
        v = per_c[c]
        m = med(v)
        print(f"  {c}: clock satellites per epoch median {m:.1f} min {min(v)} max {max(v)}; "
              f"distinct satellites {len(sats_c[c])} [{' '.join(sorted(sats_c[c]))}]")
    for name, d in (("B", per_b), ("P", per_p)):
        for k in sorted(d):
            v = d[k]
            print(f"  {name} {k[0]}:{k[1]}  satellites per epoch median {med(v):.1f} "
                  f"min {min(v)} max {max(v)}  records {sum(v)}")


def section_gross(run):
    p = os.path.join(run, "logs", "network_pf_rows.csv")
    print(f"\n== GROSS CODE ROWS at full weight (w >= 1e-2)  ({p}) ==")
    if not os.path.exists(p):
        print("absent")
        return
    by_c = defaultdict(lambda: [0, 0, 0])
    by_cs = defaultdict(lambda: [0, 0, 0])
    with open(p, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            if r["kind"] != "code":
                continue
            try:
                v = abs(float(r["resid_m"]))
                w = float(r["w"])
            except (ValueError, TypeError):
                continue
            if w < 1e-2:
                continue
            for acc in (by_c[r["sat"][:1]], by_cs[(r["sat"][:1], r["signal"])]):
                acc[0] += 1
                acc[1] += v > 3.0
                acc[2] += v > 10.0
    for c in sorted(by_c):
        a = by_c[c]
        print(f"  {c}: rows above 3 m {a[1]}, above 10 m {a[2]} (of {a[0]} full-weight code rows)")
    for k in sorted(by_cs):
        a = by_cs[k]
        print(f"    {k[0]}:{k[1]}  above 3 m {a[1]}, above 10 m {a[2]} (of {a[0]})")


def read_rx_isb(run):
    p = os.path.join(run, "logs", "network_rx_states.csv")
    out = defaultdict(lambda: defaultdict(list))   # station -> hour -> [value]
    series = defaultdict(dict)                    # station -> tow -> value
    if not os.path.exists(p):
        return out, series, p
    with open(p, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            if r["kind"] != "ISB":
                continue
            t = float(r["tow"])
            v = float(r["value_m"])
            out[r["station"]][hour_of(t)].append(v)
            series[r["station"]][t] = v
    return out, series, p


def read_cl_isb(run):
    p = os.path.join(run, "logs", "closed_loop_isb.csv")
    out = defaultdict(list)
    series = {}
    if not os.path.exists(p):
        return out, series, p
    with open(p, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            t = float(r["tow"])
            out[hour_of(t)].append(float(r["isb_m"]))
            series[t] = (float(r["isb_m"]), float(r["isb_std_m"]))
    return out, series, p


def section_isb(run):
    by_sta, _, p = read_rx_isb(run)
    print(f"\n== INTER-SYSTEM BIAS, producer stations  ({p}) ==")
    if not by_sta:
        print("no ISB rows")
    else:
        hours = sorted({h for d in by_sta.values() for h in d})
        print(f"stations with an ISB row: {len(by_sta)}")
        print("  hour  n_sta  median_over_stations_of_hourly_median_m  p10  p90")
        for h in hours:
            vals = [med(d[h]) for d in by_sta.values() if d.get(h)]
            vals.sort()
            p10 = vals[int(0.1 * (len(vals) - 1))] if vals else float("nan")
            p90 = vals[int(0.9 * (len(vals) - 1))] if vals else float("nan")
            print(f"  {h:4d} {len(vals):6d} {fmt(med(vals)):>12} {fmt(p10):>10} {fmt(p90):>10}")
    cl, _, q = read_cl_isb(run)
    print(f"\n== INTER-SYSTEM BIAS, consumer  ({q}) ==")
    if not cl:
        print("absent")
    else:
        print("  hour  n  median_m  first  last")
        for h in sorted(cl):
            v = cl[h]
            print(f"  {h:4d} {len(v):4d} {fmt(med(v)):>9} {fmt(v[0]):>8} {fmt(v[-1]):>8}")


def section_isbtruth(run, eps):
    fx = fixture_dirs(run)
    arm = json.load(open(os.path.join(run, "arm.json"), encoding="utf-8"))
    cons = (arm.get("network", {}).get("closed_loop", {}).get("station") or {}).get("name")
    print(f"\n== CONSUMER INTER-SYSTEM BIAS AGAINST TRUTH (simulator) ==")
    if not cons or cons not in fx:
        print("no simulator fixture reachable for the consumer")
        return
    d = fx[cons]
    osb = read_truth_bias(d)
    rxb = read_truth_rx(d)
    isb_true = read_truth_scalar(d, "isb_gps_gal_m")
    pairs = consumer_pairs(run)
    inj = injected_constants(run, consumer=True)
    print(f"truth: {os.path.join(d, 'truth_bias.csv')}, truth_rx_bias.csv, truth_scalars.csv "
          f"(isb_gps_gal_m = {isb_true}); consumer code pairs {pairs}")
    if "G" not in pairs or "E" not in pairs or isb_true is None:
        print("pairs or truth missing")
        return
    # Truth of a relabelled / biased signal: the product OSB of the signal it was
    # relabelled FROM plus the injected constant.
    def truth_osb(sat, sig):
        src = inj.get("relabel_from", {}).get(sig, sig)
        v = osb.get((sat, src))
        if v is None:
            return None
        return v + inj.get("sat", {}).get((sat, sig), 0.0)
    a = {c: if_weights(c, *pairs[c]) for c in ("G", "E")}
    def rx(sig):
        src = inj.get("relabel_from", {}).get(sig, sig)
        return rxb.get(src, 0.0)
    if_rx = {c: a[c][0] * rx(pairs[c][0]) + a[c][1] * rx(pairs[c][1]) for c in ("G", "E")}
    m_by_t = {}
    for tow, e in eps.items():
        m = {}
        for c in ("G", "E"):
            s1, s2 = pairs[c]
            vals = []
            for sat, c0 in e["C"].items():
                if sat[0] != c:
                    continue
                b1 = e["B"].get((sat, s1)); b2 = e["B"].get((sat, s2))
                o1 = truth_osb(sat, s1); o2 = truth_osb(sat, s2)
                if None in (b1, b2, o1, o2):
                    continue
                vals.append(-c0 + a[c][0] * (b1 - o1) + a[c][1] * (b2 - o2))
            if vals:
                m[c] = med(vals)
        if "G" in m and "E" in m:
            m_by_t[tow] = m
    _, cl, q = read_cl_isb(run)
    print(f"IF_rx(G {pairs['G']}) = {if_rx['G']:+.4f} m, IF_rx(E {pairs['E']}) = {if_rx['E']:+.4f} m; "
          f"consumer ISB from {q}")
    print("  hour   n  median(Y_est)  median(Y_pred)  median(Y_est-Y_pred)  p90|diff|  median(m_E-m_G)")
    by_h = defaultdict(list)
    for tow, (y, s) in sorted(cl.items()):
        m = m_by_t.get(round(tow / 30.0) * 30.0)
        if not m:
            continue
        pred = isb_true + if_rx["E"] - if_rx["G"] - (m["E"] - m["G"])
        by_h[hour_of(tow)].append((y, pred, m["E"] - m["G"]))
    for h in sorted(by_h):
        v = by_h[h]
        diffs = sorted(abs(y - p) for y, p, _ in v)
        print(f"  {h:4d} {len(v):4d} {med([y for y, _, _ in v]):14.4f} {med([p for _, p, _ in v]):15.4f} "
              f"{med([y - p for y, p, _ in v]):21.4f} {diffs[int(0.9 * (len(diffs) - 1))]:10.4f} "
              f"{med([dm for _, _, dm in v]):16.4f}")
    # The same prediction with the datum taken over the CONSUMER'S OWN satellites (the
    # ones in closed_loop_sat_trace.csv that epoch, a plain mean per constellation): the
    # consumer's receiver clock and inter-system bias absorb its own satellites' errors,
    # not the network median, and the two differ by the per-satellite scatter of E_s.
    trk = defaultdict(set)
    p = os.path.join(run, "logs", "closed_loop_sat_trace.csv")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                trk[round(float(r["tow"]) / 30.0) * 30.0].add(r["sat"])
    by_h2 = defaultdict(list)
    sd_by_h = defaultdict(list)
    for tow, (y, s) in sorted(cl.items()):
        t = round(tow / 30.0) * 30.0
        e = eps.get(t)
        if not e or not trk.get(t):
            continue
        mm = {}
        for c in ("G", "E"):
            s1, s2 = pairs[c]
            vals = []
            for sat in trk[t]:
                if sat[0] != c or sat not in e["C"]:
                    continue
                b1 = e["B"].get((sat, s1)); b2 = e["B"].get((sat, s2))
                o1 = truth_osb(sat, s1); o2 = truth_osb(sat, s2)
                if None in (b1, b2, o1, o2):
                    continue
                vals.append(-e["C"][sat] + a[c][0] * (b1 - o1) + a[c][1] * (b2 - o2))
            if len(vals) >= 2:
                mm[c] = sum(vals) / len(vals)
                sd_by_h[(hour_of(tow), c)].append(st.stdev(vals))
        if "G" in mm and "E" in mm:
            by_h2[hour_of(tow)].append(y - (isb_true + if_rx["E"] - if_rx["G"] - (mm["E"] - mm["G"])))
    print("  with the datum over the consumer's own satellites: hour, median(Y_est - Y_pred_tracked), "
          "sd over its satellites of E_s (GPS / Galileo, m):")
    for h in sorted(by_h2):
        print(f"  {h:4d} {med(by_h2[h]):+10.4f}    {med(sd_by_h.get((h, 'G'), [])):.3f} / "
              f"{med(sd_by_h.get((h, 'E'), [])):.3f}")


# ------------------------------------------------------------------ injected constants
def injected_constants(run, consumer=False):
    """From rapid.json: the relabel map and the per-satellite bias constants, reproduced with
    rapid_arm.py's own draw and its per-FILE record-op index (the station group's ops, or the
    consumer's when consumer=True)."""
    rp = os.path.join(run, "rapid.json")
    out = {"relabel_from": {}, "sat": {}, "specs": []}
    if not os.path.exists(rp):
        return out
    import rapid_arm as ra
    spec = json.load(open(rp, encoding="utf-8"))
    ops = [ra.parse_op(t) for t in spec.get("rinex", [])]
    if consumer:
        ops = [o for o in ops if o["consumer"] == "1"]
    else:
        ops = [o for o in ops if o["consumer"] != "1"]
    rec = [o for o in ops if o["op"] not in ("relabel", "holes", "primaryonly", "clone", "ecc")
           and not (o["op"] == "strip" and o["sats"] == "all" and ra.op_window(o) is None)]
    for o in ops:
        if o["op"] == "relabel":
            out["relabel_from"][o["to"]] = o["from"]
    sats = {}
    for n_op, o in enumerate(rec):
        if o["op"] != "bias" or o.get("per") != "sat":
            continue
        out["specs"].append((o.get("types"), o.get("seed"), n_op, float(o.get("sigma", 1.0))))
        for k in range(1, 37):
            sat = f"{o['sys']}{k:02d}"
            sats[(sat, o["types"])] = ra.draw(o.get("seed", "1"), n_op, "bias", sat).gauss(
                0.0, float(o.get("sigma", 1.0)))
    out["sat"] = sats
    return out


def section_inject(run, eps):
    inj = injected_constants(run)
    print(f"\n== INJECTED INTER-FAMILY CONSTANTS AGAINST THE WIRE  (products.txt, rapid.json) ==")
    if not inj["specs"]:
        print("no per-satellite bias op in rapid.json")
        return
    print(f"injected: {inj['specs']} (types, seed, per-file op index, sigma m); relabel map "
          f"{inj['relabel_from']}")
    b = inj["sat"]
    a1, a2 = if_weights("E", "C1C", "C5Q")
    rows = defaultdict(dict)   # tow -> sat -> (D1, D5)
    for tow, e in eps.items():
        for sat in e["C"]:
            if sat[0] != "E":
                continue
            v1 = e["B"].get((sat, "C1X")); v1c = e["B"].get((sat, "C1C"))
            v5 = e["B"].get((sat, "C5X")); v5q = e["B"].get((sat, "C5Q"))
            if None in (v1, v1c, v5, v5q):
                continue
            rows[tow][sat] = (v1 - v1c, v5 - v5q)
    if not rows:
        print("no epoch with both families' B records on a Galileo satellite")
        return
    grades = {"raw C1X": [], "raw C5X": [], "datum C1X": [], "datum C5X": [], "IF": []}
    per_h = defaultdict(lambda: defaultdict(list))
    per_sat = defaultdict(lambda: defaultdict(list))  # sat -> hour -> (e1_datum, e5_raw, eIF)
    for tow in sorted(rows):
        r = rows[tow]
        e = {}
        for sat, (d1, d5) in r.items():
            b1 = b.get((sat, "C1X"), 0.0)
            b5 = b.get((sat, "C5X"), 0.0)
            e[sat] = {"raw C1X": d1 - b1, "raw C5X": d5 - b5,
                      "datum C1X": d1 - (b1 + (a2 / a1) * b5), "datum C5X": d5,
                      "IF": (a1 * d1 + a2 * d5) - (a1 * b1 + a2 * b5)}
        if len(e) < 3:
            continue
        h = hour_of(tow)
        for k in grades:
            cm = med([v[k] for v in e.values()])
            for sat, v in e.items():
                per_h[k][h].append(v[k] - cm)
                per_sat[sat][h].append((k, v[k] - cm))
    print("  common mode removed per epoch (median over satellites); rms by hour (m):")
    hours = sorted({h for k in per_h for h in per_h[k]})
    print("  grade        " + " ".join(f"h{h}" .rjust(8) for h in hours) + "   from-h3")
    for k in grades:
        late = [x for h in hours if h >= 2 for x in per_h[k].get(h, [])]
        print(f"  {k:<12} " + " ".join(f"{rms(per_h[k].get(h, [])):8.4f}" for h in hours)
              + f"   {rms(late):8.4f}")
    # the FROZEN-SPLIT test
    print("\n  FROZEN-SPLIT test: per satellite, the de-commoned error (datum C1X / raw C5X / IF) "
          "averaged over hour 1, hour 3 and the last hour (m):")
    last = max(hours)
    for sat in sorted(per_sat):
        line = []
        for h in (0, 2, last):
            vals = per_sat[sat].get(h, [])
            if not vals:
                line.append("   -      -      -   ")
                continue
            mv = defaultdict(list)
            for k, v in vals:
                mv[k].append(v)
            line.append(f"{med(mv['datum C1X']):+.3f} {med(mv['raw C5X']):+.3f} {med(mv['IF']):+.3f}")
        print(f"   {sat}: h1 [{line[0]}]  h3 [{line[1]}]  h{last + 1} [{line[2]}]")
    # the receivers' walk (combined family = the stations whose files were relabelled)
    _, ser, p = read_rx_isb(run)
    fam = set(json.load(open(os.path.join(run, "rapid.json"))).get("rinex_touched", []))
    walks = []
    for sta, s in ser.items():
        if sta not in fam:
            continue
        vv = [v for t, v in s.items() if hour_of(t) >= 1]
        if vv:
            walks.append(max(vv) - min(vv))
    if walks:
        print(f"\n  receiver ISB walk (max - min over hours 2 to the end) at the relabelled stations "
              f"({p}): median {med(walks):.4f} m, max {max(walks):.4f} m over {len(walks)} stations")


def section_cbtruth(run, eps):
    """(simulator) The satellite code biases against truth_bias.csv in the REALIZED datum.

    The realized datum of a satellite's pinned pair is its ionosphere-free pin and its
    geometry-free pin (c*T_GD for GPS, c*BGD(E1, E5a) for Galileo), both anchored at the
    satellite's first sighting and CARRIED afterwards (code_datum_time_linked and
    sat_gf_datum_time_linked).  Transformed into that datum, the truth of the pinned pair
    is a per-satellite CONSTANT (the pair carries no estimable content, Odijk section
    5.1), so the grade is the variation of (B - OSB) about its per-satellite mean over the
    satellite's emitted arc: what the carried datum let move.  Gaps above 120 s split an arc."""
    fx = fixture_dirs(run)
    print("\n== SATELLITE CODE BIASES AGAINST TRUTH IN THE REALIZED DATUM (simulator) ==")
    if not fx:
        print("no simulator fixture reachable")
        return
    d = sorted(fx.values())[0]
    osb = read_truth_bias(d)
    inj = injected_constants(run)
    def truth_osb(sat, sig):
        src = inj.get("relabel_from", {}).get(sig, sig)
        v = osb.get((sat, src))
        return None if v is None else v + inj.get("sat", {}).get((sat, sig), 0.0)
    ser = defaultdict(lambda: defaultdict(list))   # (const, sig) -> sat -> [(tow, err)]
    for tow in sorted(eps):
        for (sat, sig), b in eps[tow]["B"].items():
            o = truth_osb(sat, sig)
            if o is None:
                continue
            ser[(sat[0], sig)][sat].append((tow, b - o))
    print(f"truth {os.path.join(d, 'truth_bias.csv')} (relabelled signals take the product OSB of "
          f"their source plus the injected constant)")
    print("  const:signal  arcs  rms(B-OSB about the per-arc mean) hours 1 / 2-6 (m)   "
          "spread of the per-satellite means (sd, m: the datum, not an error)")
    for k in sorted(ser):
        dev = {1: [], 2: []}
        means = []
        for sat, v in ser[k].items():
            arcs, cur = [], [v[0]]
            for a, bb in zip(v, v[1:]):
                if bb[0] - a[0] > 120.0:
                    arcs.append(cur); cur = []
                cur.append(bb)
            arcs.append(cur)
            for arc in arcs:
                if len(arc) < 3:
                    continue
                m = sum(e for _, e in arc) / len(arc)
                means.append(m)
                for t, e in arc:
                    dev[1 if hour_of(t) == 0 else 2].append(e - m)
        print(f"  {k[0]}:{k[1]:<4} {len(means):5d}  {fmt(rms(dev[1]))} / {fmt(rms(dev[2]))}"
              f"   {fmt(st.stdev(means) if len(means) > 1 else float('nan'))}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--sections", default="wire,gross,isb,isbtruth,cbtruth,inject")
    a = ap.parse_args()
    run = a.run.rstrip("/\\")
    secs = set(s.strip() for s in a.sections.split(","))
    eps, path = read_products(run)
    print(f"run {run}")
    if "wire" in secs:
        section_wire(run, eps, path)
    if "gross" in secs:
        section_gross(run)
    if "isb" in secs:
        section_isb(run)
    if "isbtruth" in secs:
        section_isbtruth(run, eps)
    if "cbtruth" in secs:
        section_cbtruth(run, eps)
    if "inject" in secs:
        section_inject(run, eps)


if __name__ == "__main__":
    main()
