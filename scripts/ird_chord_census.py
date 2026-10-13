#!/usr/bin/env python3
"""Chord census of a network run (card IRD section 8): truth-free, so it is also the field's
instrument.

Under the eq34 chain with receiver phase-bias (RPB) states, every chord's absolute float is
lambda times an integer (card IRD section 3: N~_rs = N_rs + z_r - z^s).  Under a datum without
RPB states the chords carry each receiver's fractional offset and read at the uniform null.
The census reads, per run and per hour from the grading start:

  - per band, |wrap(value_abs / lambda)| p50 of every chord (a carried arc of
    logs/network_amb_state.csv, erase rows excluded, that is not a pinned edge at that epoch:
    logs/network_phase_anchor.csv, plus logs/network_chain_edges.csv once card IRD-1 writes it);
  - the chord wide lane value_L1/lambda_1 - value_L2/lambda_2 (L1C, else L1W, paired with L2W;
    pooled), |wrap| p50 and the share within 0.15;
  - each against its size-matched null (one uniform fraction per arc, the same arc-epoch
    occupancy, 2000 draws; scripts/simnet_wire_integer_consistency.py owns the null);
  - the per-arc circular R of the wide lane over 30 min windows (cells of >= 10 epochs) against
    the R of independent uniform draws of the same sizes, and the per-epoch wide-lane jump
    (consecutive epochs of one arc, unwrapped) p50, p99 and max;
  - the receiver DCB spread (logs/network_rx_states.csv, CB GPS_DCB): the sd across stations
    at the last epoch of each hour;
  - the run-end counters (stdout.txt): eq34 re-references, erased and S-transformed carries,
    shift calls, RPB erases, the pin audit, and the emitted epochs (E records).

The negative control of IRD-0 (gate 2) reads the chord wide lane of the golden arms: within
0.02 cycle of the null in every hour (--null-tol 0.02).  The control re-registered at IRD-0r
(card IRD section 13.3; --control ird0r): p50 above 0.15 cycle and the gap to the null within
max(0.05 cycle, the null's own two-sided 99 % band) in every hour.

Usage:
    python scripts/ird_chord_census.py <run> [<run> ...] [--start-hms 00:30:15 | --start-tow T]
        [--hours 2] [--null-mc 2000] [--null-tol 0.02] [--control ird0r] [--split station]
        [--json out.json]
    python scripts/ird_chord_census.py --selftest
"""
import argparse
import json
import os
import re
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_wire_integer_consistency as WIC  # noqa: E402  (wrap and the size-matched null)

R_WINDOW_S = 1800.0
R_MIN_EPOCHS = 10
WL_PAIRS = (("L1C", "L2W"), ("L1W", "L2W"))

COUNTER_PATTERNS = {
    "reref_erase_shift": re.compile(r"Network \(Card 3/eq34\): (\d+) \(sat,\S+\) re-reference\(s\), "
                                    r"(\d+) local carry value\(s\) erased, (\d+) carry value\(s\) "
                                    r"S-transformed"),
    "shift_calls": re.compile(r"mechanism eq34_shift: (\d+) call\(s\)"),
    "rpb_erased": re.compile(r"rpb_eq34_erase: (\d+) receiver phase biases erased"),
    "pin_audit": re.compile(r"\(eq34 pin audit\) (\S+): (\w+) (?:all )?(\d+) epoch"),
}


def lam_of(sig):
    return WIC.C_LIGHT / WIC.DUAL_FREQ["G"][sig[1]]


def read_chords(run, start, end):
    """DataFrame tow, station, sat, signal, cyc of the chords inside [start, end)."""
    p = os.path.join(run, "logs", "network_amb_state.csv")
    df = pd.read_csv(p, comment="#", usecols=["tow", "station", "sat", "signal", "value_abs_m",
                                               "event"],
                     dtype={"station": str, "sat": str, "signal": str, "event": str})
    df["tow"] = df["tow"].round(3)
    df = df[~df["event"].fillna("").str.startswith("erase")]
    df = df[(df["tow"] >= start - 1e-3) & ((end is None) | (df["tow"] < (end or 0) - 1e-3))]
    df = df[df["sat"].str.startswith("G") & df["signal"].isin(["L1C", "L1W", "L2W"])]
    pinned = []
    pa = os.path.join(run, "logs", "network_phase_anchor.csv")
    if os.path.isfile(pa):
        a = pd.read_csv(pa, dtype={"anchor_sta": str, "sat": str, "signal": str})
        pinned.append(a.rename(columns={"anchor_sta": "station"})[["tow", "station", "sat",
                                                                   "signal"]])
    ce = os.path.join(run, "logs", "network_chain_edges.csv")
    if os.path.isfile(ce):
        e = pd.read_csv(ce, comment="#", dtype={"edge_sta": str, "edge_sat": str, "band": str})
        pinned.append(e.rename(columns={"edge_sta": "station", "edge_sat": "sat",
                                        "band": "signal"})[["tow", "station", "sat", "signal"]])
    n_pinned = 0
    if pinned:
        pin = pd.concat(pinned).drop_duplicates()
        pin["tow"] = pin["tow"].round(3)
        pin["_pin"] = 1
        df = df.merge(pin, on=["tow", "station", "sat", "signal"], how="left")
        n_pinned = int(df["_pin"].notna().sum())
        df = df[df["_pin"].isna()].drop(columns="_pin")
    df["cyc"] = df["value_abs_m"] / df["signal"].map(lam_of)
    return df, n_pinned


def wide_lanes(df):
    """DataFrame tow, station, sat, l1sig, wl: L1C (else L1W) with L2W of the same arc-epoch."""
    p = df.pivot_table(index=["tow", "station", "sat"], columns="signal", values="cyc",
                       aggfunc="first")
    out = []
    taken = pd.Series(False, index=p.index)
    for l1, l2 in WL_PAIRS:
        if l1 not in p.columns or l2 not in p.columns:
            continue
        m = p[l1].notna() & p[l2].notna() & ~taken
        q = (p.loc[m, l1] - p.loc[m, l2]).rename("wl").reset_index()
        q["l1sig"] = l1
        out.append(q)
        taken |= m
    if not out:
        return pd.DataFrame(columns=["tow", "station", "sat", "wl", "l1sig"])
    return pd.concat(out, ignore_index=True)


def null_for(groups, mc, seed):
    """Size-matched null of p50 |wrap| for one uniform per group, weighted by group size:
    WIC.null_summary's {null, null_lo, null_hi, null_lo99, null_hi99}."""
    w = groups.size().values
    return WIC.null_summary(WIC.size_matched_null_draws(len(w), np.arange(len(w)), None, w,
                                                        mc=mc, seed=seed))


def circ_R_cells(wl, start):
    """Per (arc, 30 min window) with >= R_MIN_EPOCHS epochs: (R, n)."""
    wl = wl.assign(win=((wl["tow"] - start) // R_WINDOW_S).astype(int))
    z = np.exp(2j * np.pi * wl["wl"].values)
    wl = wl.assign(zr=z.real, zi=z.imag)
    g = wl.groupby(["station", "sat", "win"]).agg(zr=("zr", "mean"), zi=("zi", "mean"),
                                                  n=("zr", "size"))
    g = g[g["n"] >= R_MIN_EPOCHS]
    return np.hypot(g["zr"].values, g["zi"].values), g["n"].values


def null_R(sizes, mc=200, seed=7):
    """p50 of the R of independent uniform draws with the same cell sizes."""
    rng = np.random.default_rng(seed)
    out = []
    for _ in range(mc):
        rs = [abs(np.mean(np.exp(2j * np.pi * rng.uniform(size=int(n))))) for n in sizes]
        out.append(np.median(rs) if rs else float("nan"))
    return float(np.mean(out))


def wl_jumps(wl):
    """|wl(t) - wl(t_prev)| over consecutive epochs of one arc (unwrapped, cycles)."""
    wl = wl.sort_values(["station", "sat", "tow"])
    dt = wl.groupby(["station", "sat"])["tow"].diff()
    cad = float(np.median(dt.dropna())) if dt.notna().any() else 30.0
    d = wl.groupby(["station", "sat"])["wl"].diff().abs()
    return d[(dt > 0) & (dt <= 1.5 * cad)].values


def dcb_spread(run, start, end):
    """[(hour, tow, n stations, sd, min, max)] of the GPS_DCB at the last epoch of each hour."""
    p = os.path.join(run, "logs", "network_rx_states.csv")
    if not os.path.isfile(p):
        return []
    rx = pd.read_csv(p, comment="#", dtype={"station": str, "kind": str, "key": str})
    rx = rx[(rx["kind"] == "CB") & (rx["key"] == "GPS_DCB")]
    rx = rx[(rx["tow"] >= start - 1e-3) & ((end is None) | (rx["tow"] < (end or 0) - 1e-3))]
    out = []
    rx = rx.assign(h=((rx["tow"] - start) // 3600).astype(int))
    for h, g in rx.groupby("h"):
        t = g["tow"].max()
        v = g[g["tow"] == t]["value_m"].values
        out.append((int(h), float(t), int(v.size), float(np.std(v, ddof=1)) if v.size > 1 else
                    float("nan"), float(v.min()), float(v.max())))
    return out


def run_counters(run):
    """Run-end counters from stdout.txt (or logs/main_log.log) and the emitted epoch count."""
    out = {}
    for name in ("stdout.txt", os.path.join("logs", "main_log.log")):
        p = os.path.join(run, name)
        if not os.path.isfile(p):
            continue
        with open(p, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = COUNTER_PATTERNS["reref_erase_shift"].search(line)
                if m:
                    out["reref"], out["erased"], out["stransformed"] = (int(x) for x in m.groups())
                m = COUNTER_PATTERNS["shift_calls"].search(line)
                if m:
                    out["shift_calls"] = int(m.group(1))
                m = COUNTER_PATTERNS["rpb_erased"].search(line)
                if m:
                    out["rpb_erased"] = int(m.group(1))
                m = COUNTER_PATTERNS["pin_audit"].search(line)
                if m:
                    out.setdefault("pin_audit", {})[m.group(1)] = "%s %s" % (m.group(2), m.group(3))
        break
    prod = os.path.join(run, "products.txt")
    if os.path.isfile(prod):
        with open(prod, encoding="utf-8", errors="replace") as fh:
            out["emitted_epochs"] = sum(1 for l in fh if l.startswith("E,"))
    return out


def first_tow(run):
    with open(os.path.join(run, "products.txt"), encoding="utf-8", errors="replace") as fh:
        for l in fh:
            if l.startswith("E,"):
                return round(float(l.split(",")[2]), 3)
    raise SystemExit("no E record in %s/products.txt" % run)


def census(run, start, end, mc=2000, split=()):
    df, n_pinned = read_chords(run, start, end)
    df = df.assign(h=((df["tow"] - start) // 3600).astype(int))
    wl = wide_lanes(df)
    wl = wl.assign(h=((wl["tow"] - start) // 3600).astype(int))
    res = {"run": run, "start_tow": start, "pinned_rows_excluded": n_pinned, "hours": {}}
    for h in sorted(set(df["h"]) | set(wl["h"])):
        row = {}
        g = wl[wl["h"] == h]
        if len(g):
            v = np.abs(WIC.wrapf(g["wl"].values))
            nl = null_for(g.groupby(["station", "sat"]), mc, 100 + h)
            row["WL"] = {"n": int(v.size), "arcs": int(g.groupby(["station", "sat"]).ngroups),
                         "p50": float(np.median(v)), "in15": float(np.mean(v <= 0.15)),
                         "gap": float(np.median(v)) - nl["null"], **nl}
            for l1 in ("L1C", "L1W"):
                gg = g[g["l1sig"] == l1]
                if len(gg):
                    row["WL_" + l1] = {"n": int(len(gg)),
                                       "p50": float(np.median(np.abs(WIC.wrapf(gg["wl"].values))))}
        for sig in ("L1C", "L1W", "L2W"):
            g = df[(df["h"] == h) & (df["signal"] == sig)]
            if len(g):
                v = np.abs(WIC.wrapf(g["cyc"].values))
                nm = null_for(g.groupby(["station", "sat"]), min(mc, 500), 200 + h)["null"]
                row[sig] = {"n": int(v.size), "p50": float(np.median(v)), "null": nm,
                            "gap": float(np.median(v)) - nm}
        if "station" in split and len(wl[wl["h"] == h]):
            g = wl[wl["h"] == h]
            row["station"] = {s: float(np.median(np.abs(WIC.wrapf(x["wl"].values))))
                              for s, x in g.groupby("station")}
        res["hours"][int(h)] = row
    if len(wl):
        R, n = circ_R_cells(wl, start)
        res["circ_R"] = {"cells": int(R.size), "p50": float(np.median(R)) if R.size else
                         float("nan"), "null_p50": null_R(n) if R.size else float("nan")}
        j = wl_jumps(wl)
        res["wl_jump"] = {"n": int(j.size), "p50": float(np.median(j)) if j.size else float("nan"),
                          "p99": float(np.percentile(j, 99)) if j.size else float("nan"),
                          "max": float(j.max()) if j.size else float("nan"),
                          "over_0p5": float(np.mean(j > 0.5)) if j.size else float("nan")}
    res["dcb"] = dcb_spread(run, start, end)
    res["counters"] = run_counters(run)
    return res


def print_census(res, null_tol=None, control=None):
    """Print the census; with null_tol the IRD-0 verdict on the chord wide lane, with control
    "ird0r" the re-registered one (card IRD section 13.3).  Returns the failing cells."""
    fails = []
    cfails, ccells = [], []
    label = os.path.basename(res["run"].rstrip("/\\"))
    print("== %s (start TOW %.3f; %d pinned rows excluded)"
          % (os.path.basename(res["run"].rstrip("/\\")), res["start_tow"], res["pinned_rows_excluded"]))
    for h, row in sorted(res["hours"].items()):
        w = row.get("WL")
        if w:
            print("   h%d chord WL n %d arcs %d p50 %.3f in0.15 %.2f null %.3f [%.3f, %.3f] gap %+.3f"
                  % (h, w["n"], w["arcs"], w["p50"], w["in15"], w["null"], w["null_lo"],
                     w["null_hi"], w["gap"])
                  + "".join("  (%s n %d p50 %.3f)" % (k, row[k]["n"], row[k]["p50"])
                            for k in ("WL_L1C", "WL_L1W") if k in row))
        if control == "ird0r":
            if w:
                ok, tlo, thi, why = WIC.control_ird0r(w["p50"], w["null"], w["null_lo99"],
                                                      w["null_hi99"], w["n"])
                ccells.append((label, h, "chordWL", w["p50"], w["null"], w["gap"], tlo, thi,
                               w["n"], "PASS" if ok else "FAIL: " + why))
            else:
                ok = False
                ccells.append((label, h, "chordWL", float("nan"), float("nan"), float("nan"),
                               float("nan"), float("nan"), 0, "FAIL: n = 0"))
            if not ok:
                cfails.append(ccells[-1])
        if null_tol is not None and (not w or w["n"] <= 0 or not abs(w["gap"]) <= null_tol):
            fails.append((os.path.basename(res["run"].rstrip("/\\")), h, "chordWL",
                          w["p50"] if w else float("nan"), w["null"] if w else float("nan"),
                          w["gap"] if w else float("nan"), w["n"] if w else 0))
        print("      bands: " + "  ".join("%s n %d p50 %.3f gap %+.3f" % (s, row[s]["n"], row[s]["p50"],
                                                                         row[s]["gap"])
                                           for s in ("L1C", "L1W", "L2W") if s in row))
        if "station" in row:
            st = sorted(row["station"].items(), key=lambda kv: kv[1])
            print("      per-station WL p50: lowest %s, highest %s"
                  % (", ".join("%s %.3f" % kv for kv in st[:3]),
                     ", ".join("%s %.3f" % kv for kv in st[-3:])))
    if "circ_R" in res:
        print("   per-arc circular R (30 min, WL): cells %d p50 %.3f (independent-uniform null p50 %.3f)"
              % (res["circ_R"]["cells"], res["circ_R"]["p50"], res["circ_R"]["null_p50"]))
        j = res["wl_jump"]
        print("   per-epoch WL jump: n %d p50 %.4f p99 %.3f max %.2f share > 0.5 cycle %.4f"
              % (j["n"], j["p50"], j["p99"], j["max"], j["over_0p5"]))
    for h, t, n, sd, lo, hi in res["dcb"]:
        print("   receiver DCB h%d (TOW %.0f, %d stations): sd %.3f m, range %.2f to %.2f m"
              % (h, t, n, sd, lo, hi))
    c = res["counters"]
    print("   counters: " + ", ".join("%s %s" % (k, c[k]) for k in sorted(c)))
    if null_tol is not None:
        print("   negative control, chord WL (|p50 - null| <= %.3f, n > 0 every hour): %s"
              % (null_tol, "PASS" if not fails else "FAIL in %d hour(s)" % len(fails)))
        for f in fails:
            print("     FAIL %s h%d %s: p50 %.4f null %.4f gap %+.4f n %d" % f)
    if control == "ird0r":
        print("   re-registered control, chord WL (card IRD 13.3: p50 > %.2f and gap within "
              "max(%.2f, null 99 %% band); %d cell(s)): %s"
              % (WIC.CONTROL_FAR_CYC, WIC.CONTROL_GAP_FLOOR_CYC, len(ccells),
                 "PASS" if not cfails else "FAIL in %d cell(s)" % len(cfails)))
        for c in ccells:
            print("     %s h%d %s: p50 %.4f null %.4f gap %+.4f tol [-%.4f, +%.4f] n %d  %s" % c)
    return fails + cfails


def selftest():
    import tempfile
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    rng = np.random.default_rng(3)
    l1, l2 = lam_of("L1C"), lam_of("L2W")
    with tempfile.TemporaryDirectory() as td:
        def make(run, integer):
            os.makedirs(os.path.join(run, "logs"))
            t0 = 261030.0
            rows, rx = [], []
            stas = ["S%02d000SIM" % k for k in range(1, 11)]
            sats = ["G%02d" % k for k in range(1, 9)]
            off = {(s, g): (0.0 if integer else rng.uniform(-0.5, 0.5))
                   for s in stas for g in ("L1C", "L2W", "L1W")}
            for e in range(240):
                t = t0 + 30 * e
                for i, st in enumerate(stas):
                    l1sig = "L1W" if i >= 8 else "L1C"
                    for k, sa in enumerate(sats):
                        n1, n2 = 3 * k - i, 7 - k + 2 * i
                        f1 = off[(st, l1sig)] + 0.3 * k * (not integer) + rng.normal(0, 0.005)
                        f2 = off[(st, "L2W")] + rng.normal(0, 0.005)
                        rows.append("2313,%.3f,%s,%s,%s,%.9f,0.01,0,0,0,1,%d,0,1,,"
                                    % (t, st, sa, l1sig, (n1 + f1) * l1, e))
                        rows.append("2313,%.3f,%s,%s,L2W,%.9f,0.01,0,0,0,1,%d,0,1,,"
                                    % (t, st, sa, (n2 + f2) * l2, e))
                    rx.append("2313,%.3f,%s,CB,GPS_DCB,%.4f,0.1" % (t, st, 0.5 * i))
                rows.append("2313,%.3f,S01000SIM,G01,L1C,999.0,0.01,0,0,0,1,1,0,1,,erase:gap_prune" % t)
            with open(os.path.join(run, "logs", "network_amb_state.csv"), "w") as fh:
                fh.write("# network_amb_state.csv - a comment line\n")
                fh.write("week,tow,station,sat,signal,value_abs_m,var_m2,var_first_m2,var_birth_m2,"
                         "fold_var_m2,n_folds,age_ep,age_since_relevel_ep,folded,route,event\n")
                fh.write("\n".join(rows) + "\n")
            with open(os.path.join(run, "logs", "network_phase_anchor.csv"), "w") as fh:
                fh.write("week,tow,cons,signal,sat,anchor_sta\n")
                for e in range(240):
                    fh.write("2313,%.3f,G,L1C,G02,S01000SIM\n" % (t0 + 30 * e))
            with open(os.path.join(run, "logs", "network_rx_states.csv"), "w") as fh:
                fh.write("week,tow,station,kind,key,value_m,std_m\n" + "\n".join(rx) + "\n")
            with open(os.path.join(run, "products.txt"), "w") as fh:
                for e in range(240):
                    fh.write("E,2313,%.3f,0,0,0,0\n" % (t0 + 30 * e))
            with open(os.path.join(run, "stdout.txt"), "w", encoding="utf-8") as fh:
                fh.write("[x] Network (Card 3/eq34): 44 (sat,σ) re-reference(s), 470 local carry "
                         "value(s) erased, 4906 carry value(s) S-transformed (v2-shift), ...\n")
                fh.write("[x] mechanism eq34_shift: 77 call(s); not moved: ...\n")
                fh.write("[x] rx_phase_bias handover -> erase_and_rebirth at rpb_eq34_erase: 62 "
                         "receiver phase biases erased (armed)\n")
                fh.write("[x] Network (eq34 pin audit) G|L1C: COMPLETE all 241 epoch(s) - declared\n")
            return run

        ri = make(os.path.join(td, "int"), True)
        rr = make(os.path.join(td, "rand"), False)
        a = census(ri, 261030.0, None, mc=200, split=("station",))
        b = census(rr, 261030.0, None, mc=200)
        chk(a["pinned_rows_excluded"] == 240, "the anchor's 240 pinned rows are excluded")
        chk(a["hours"][0]["WL"]["n"] == 120 * 80 - 120 and a["hours"][0]["WL"]["p50"] < 0.02,
            "integer chords: WL p50 %.4f, n %d (the erase rows and the pinned L1C excluded)"
            % (a["hours"][0]["WL"]["p50"], a["hours"][0]["WL"]["n"]))
        chk("WL_L1W" in a["hours"][0] and a["hours"][0]["WL_L1W"]["n"] == 120 * 16,
            "L1W arcs pair with L2W where L1C is absent")
        chk(abs(b["hours"][0]["WL"]["gap"]) < 0.06 and b["hours"][0]["WL"]["p50"] > 0.15,
            "random chords read near the null (p50 %.3f, null %.3f)"
            % (b["hours"][0]["WL"]["p50"], b["hours"][0]["WL"]["null"]))
        chk(a["circ_R"]["p50"] > 0.99 and a["circ_R"]["null_p50"] < 0.3,
            "per-arc circular R: stable arcs %.3f against the independent-uniform null %.3f"
            % (a["circ_R"]["p50"], a["circ_R"]["null_p50"]))
        chk(a["wl_jump"]["p99"] < 0.05 and a["wl_jump"]["n"] > 0, "per-epoch WL jumps read")
        chk(abs(a["dcb"][0][3] - np.std([0.5 * i for i in range(10)], ddof=1)) < 1e-6,
            "receiver DCB sd across stations at the hour's last epoch")
        c = a["counters"]
        chk(c.get("reref") == 44 and c.get("erased") == 470 and c.get("stransformed") == 4906
            and c.get("shift_calls") == 77 and c.get("rpb_erased") == 62
            and c.get("emitted_epochs") == 240 and c["pin_audit"]["G|L1C"] == "COMPLETE 241",
            "run-end counters parsed")
        fails = print_census(b, null_tol=1e-6)
        chk(len(fails) == 2, "a tolerance of 1e-6 fails both hours (the verdict is wired)")
        w = b["hours"][0]["WL"]
        chk(w["null_lo99"] <= w["null_lo"] < w["null"] < w["null_hi"] <= w["null_hi99"],
            "the chord null carries its 99 % band")
        chk(print_census(b, control="ird0r") == [],
            "random chords pass the re-registered control in both hours")
        chk(len(print_census(a, control="ird0r")) == 2,
            "integer chords fail the re-registered control in both hours")
    print("chord-census selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--start-hms", default=None)
    ap.add_argument("--start-tow", type=float, default=None)
    ap.add_argument("--hours", type=float, default=None)
    ap.add_argument("--null-mc", type=int, default=2000)
    ap.add_argument("--null-tol", type=float, default=None)
    ap.add_argument("--control", choices=("ird0r",), default=None,
                    help="the re-registered negative control of card IRD section 13.3")
    ap.add_argument("--split", default="")
    ap.add_argument("--json", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.runs:
        ap.error("at least one run directory (or --selftest)")
    allres, allfails = [], []
    for run in a.runs:
        t0 = first_tow(run)
        start = a.start_tow if a.start_tow is not None else (
            WIC.start_from_hms(t0, a.start_hms) if a.start_hms else t0)
        end = start + a.hours * 3600.0 if a.hours else None
        res = census(run, start, end, a.null_mc, tuple(x for x in a.split.split(",") if x))
        allfails += print_census(res, a.null_tol, a.control)
        allres.append(res)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(allres, fh, indent=1, default=float)
    return 1 if ((a.null_tol is not None or a.control) and allfails) else 0


if __name__ == "__main__":
    sys.exit(main())
