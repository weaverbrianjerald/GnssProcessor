#!/usr/bin/env python3
"""Rapid one-change arms for the two-bed (sim / field) bisection loop.

A fault that shows on one bed and not on the other is isolated by a series of short
arms that each change ONE thing: on the field bed an ingredient is SUBTRACTED
(observation types stripped for a station or satellite subset, a station dropped, a
knob flipped), on the sim bed an ingredient is ADDED (a bias, a relabel, a slip
process, a drift).  This tool turns a one-line spec into a runnable arm, runs a batch
in parallel, and prints one scorecard.

  make   --base <arm.json> --tag <name> [--hours H] [--set key.path=value ...]
         [--drop-station NAME ...] [--rinex "OP;key=val;..." ...]
  run    --tags a b c [--jobs 4] [--exe <gnss_processor exe>]      (Release exe only)
  score  <tag or run_dir> ... [--pf]
  selftest

RINEX ops (GPS by default, `sys=` to change).  Common selectors:
  stations = all | A,B,C | family:Q | family:X | every:N | first:N     (default all)
  sats     = all | G03,G06 | IIF | III | L5 | NOL5                      (default all)
  consumer = 1 to apply the op to the closed-loop consumer's file too   (default 0)
  types    = comma list of 3-char patterns, `?` is a wildcard (C5? L5? ?5?)
Ops:
  strip;types=?5?                     remove the types (header too when sats=all)
  relabel;from=?5Q;to=?5X             rename observation types in the header
  bias;types=C5?;per=station|sat;sigma=3.0;seed=1      constant, metres (N(0, sigma))
  slip;types=L5?;rate=1.0;max=5;seed=1                 integer-cycle steps, rate per arc-hour
  drift;types=L5?;amp_m=0.03;period_s=5400;per=station|sat|los;seed=1   sinusoid, metres
  dropslip;types=L1?,L2?,C2?;rate=0.15;len=3;max=300;seed=1
                                      a loss of lock: the listed types are blanked for `len` epochs
                                      (rate per arc-hour) and every listed PHASE type returns with
                                      its own integer-cycle step, while the other types track through
  holes;bands=12;window=20;where=all  delete a satellite's whole record at an epoch where the
                                      primary phase of one of `bands` is missing inside an arc
                                      (a good epoch within `window` epochs), so that a
                                      phase-only dropout becomes a full tracking gap;
                                      where=mid keeps only epochs with a good epoch on BOTH
                                      sides, where=edge only those with one on a single side
An op on a phase type is given in metres and applied in cycles.  Every draw is keyed
on (seed, station, satellite), so the same spec gives the same file in every arm.
"""
import argparse
import concurrent.futures as cf
import copy
import csv
import fnmatch
import hashlib
import json
import math
import os
import random
import statistics as st
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
RUNS = ROOT + "/runs"
LABEL = "SYS / # / OBS TYPES"
C = 299792458.0
FREQ = {"G": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6}}
IIF = "G03 G06 G08 G09 G10 G24 G25 G26 G27 G30 G32".split()
III = "G04 G11 G14 G18 G23 G28".split()
SATSETS = {"IIF": set(IIF), "III": set(III), "L5": set(IIF + III)}


# ----------------------------------------------------------------------------- specs
def parse_op(text):
    parts = [p for p in text.split(";") if p.strip()]
    op = {"op": parts[0].strip(), "sys": "G", "stations": "all", "sats": "all", "consumer": "0"}
    for kv in parts[1:]:
        k, v = kv.split("=", 1)
        op[k.strip()] = v.strip()
    assert op["op"] in ("strip", "relabel", "bias", "slip", "drift", "holes", "dropslip"), op
    return op


def match_types(codes, patterns):
    pats = [p.strip() for p in patterns.split(",")]
    return [k for k, c in enumerate(codes) if any(fnmatch.fnmatchcase(c, p) for p in pats)]


def sat_selected(sat, sel):
    if sel == "all":
        return True
    if sel == "NOL5":
        return sat not in SATSETS["L5"]
    if sel in SATSETS:
        return sat in SATSETS[sel]
    return sat in sel.split(",")


def station_selected(name, idx, codes, sel):
    if sel == "all":
        return True
    if sel.startswith("family:"):
        return ("L5" + sel[7:]) in codes
    if sel.startswith("every:"):
        return idx % int(sel[6:]) == 0
    if sel.startswith("first:"):
        return idx < int(sel[6:])
    return name in sel.split(",")


def draw(seed, *key):
    return random.Random("|".join([str(seed)] + [str(k) for k in key]))


# ----------------------------------------------------------------------------- RINEX
def read_header(lines, system):
    """-> (index of first record line, codes of `system`, (first, last+1) of its type block)."""
    i, codes, blk = 0, None, None
    while i < len(lines):
        ln = lines[i]
        if ln[60:].strip() == LABEL and ln[:1] == system:
            n, a, codes = int(ln[3:6]), i, []
            while len(codes) < n:
                codes += lines[i][7:60].split()
                i += 1
            blk = (a, i)
            continue
        i += 1
        if ln[60:].strip() == "END OF HEADER":
            break
    return i, codes, blk


def type_block(system, codes):
    out = []
    for a in range(0, len(codes), 13):
        head = f"{system}  {len(codes):3d}" if a == 0 else "      "
        out.append((head + "".join(f" {c}" for c in codes[a:a + 13])).ljust(60) + LABEL)
    return out


def apply_holes(lines, i0, codes, system, op):
    """Delete satellite records where a primary phase is missing inside an arc; fix the epoch counts."""
    prio, prim = op.get("_prio") or [], []
    for b in op.get("bands", "12"):
        cand = [t for t in prio if t[0] == "L" and t[1] == b and t in codes] or \
            sorted(t for t in codes if t[0] == "L" and t[1] == b)
        if not cand:
            return lines, 0
        prim.append(codes.index(cand[0]))
    win, width = int(op.get("window", "20")), 3 + 16 * len(codes)
    pres, owner, ep, j, head = {}, {}, -1, i0, None
    while j < len(lines):
        ln = lines[j]
        if ln.startswith(">"):
            ep, head = ep + 1, j
            flag = int(ln[31]) if len(ln) > 31 and ln[31].isdigit() else 0
            j += 1 + (int(ln[32:35]) if flag > 1 else 0)
            continue
        if ln[:1] == system and len(ln) > 3:
            q = ln.ljust(width)
            pres.setdefault(ln[:3], []).append((ep, all(q[3 + 16 * k: 17 + 16 * k].strip() for k in prim), j))
            owner[j] = head
        j += 1
    kill, where = set(), op.get("where", "all")
    for v in pres.values():
        good = [e for e, ok, _ in v if ok]
        for e, ok, j in v:
            if ok or not good:
                continue
            before = any(0 < e - g <= win for g in good)
            after = any(0 < g - e <= win for g in good)
            if (where == "all" and (before or after)) or (where == "mid" and before and after) \
                    or (where == "edge" and (before != after)):
                kill.add(j)
    nk = {}
    for j in kill:
        nk[owner[j]] = nk.get(owner[j], 0) + 1
    out = []
    for j, ln in enumerate(lines):
        if j in kill:
            continue
        if j in nk:
            ln = ln[:32] + f"{int(ln[32:35]) - nk[j]:3d}" + ln[35:]
        out.append(ln)
    return out, len(kill)


def rewrite_rinex(src, dst, ops, station, hours=None):
    """Apply `ops` (already selected for this station) to one RINEX 3 file; returns the records deleted by `holes`."""
    lines = open(src, "r", encoding="ascii", errors="replace").read().split("\n")
    system = ops[0]["sys"]
    i, codes, blk = read_header(lines, system)
    assert codes, (src, "no type block for", system)
    n_holes = 0
    for op in ops:
        if op["op"] == "holes":
            lines, n = apply_holes(lines, i, codes, system, op)
            n_holes += n
    new_codes, drop = list(codes), set()
    for op in ops:
        if op["op"] == "strip" and op["sats"] == "all":
            drop |= set(match_types(codes, op["types"]))
        if op["op"] == "relabel":
            for k in match_types(codes, op["from"]):
                new_codes[k] = "".join(t if t != "?" else c for t, c in zip(op["to"], codes[k]))
    keep = [k for k in range(len(codes)) if k not in drop]
    out = lines[:blk[0]] + type_block(system, [new_codes[k] for k in keep]) + lines[blk[1]:i]
    width = 3 + 16 * len(codes)
    rec_ops = [op for op in ops if op["op"] not in ("relabel", "holes")
               and not (op["op"] == "strip" and op["sats"] == "all")]
    for op in rec_ops:
        op["_idx"] = match_types(codes, op["types"])
    state, sod = {}, 0.0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith(">"):
            f = ln[1:].split()
            sod = int(f[3]) * 3600 + int(f[4]) * 60 + float(f[5])
            if hours is not None and sod > hours * 3600.0 + 1.0:
                break
            out.append(ln)
            flag = int(ln[31]) if len(ln) > 31 and ln[31].isdigit() else 0
            i += 1
            if flag > 1:
                n = int(ln[32:35])
                out += lines[i:i + n]
                i += n
            continue
        if ln[:1] == system and len(ln) > 3:
            sat = ln[:3]
            p = ln.ljust(width)
            fld = [p[3 + 16 * k: 19 + 16 * k] for k in range(len(codes))]
            for n_op, op in enumerate(rec_ops):
                if not sat_selected(sat, op["sats"]):
                    continue
                if op["op"] == "dropslip":
                    dropslip(op, n_op, station, sat, codes, fld, sod, state, system)
                    continue
                for k in op["_idx"]:
                    if not fld[k][:14].strip():
                        continue
                    if op["op"] == "strip":
                        fld[k] = " " * 16
                        continue
                    d = delta_m(op, n_op, station, sat, codes[k], sod, state)
                    if codes[k][0] == "L":
                        d *= FREQ[system][codes[k][1]] / C
                    fld[k] = f"{float(fld[k][:14]) + d:14.3f}" + fld[k][14:16]
            ln = (p[:3] + "".join(fld[k] for k in keep)).rstrip()
        out.append(ln)
        i += 1
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "w", encoding="ascii", newline="\n").write("\n".join(out) + ("\n" if out[-1] else ""))
    return n_holes


def dropslip(op, n_op, station, sat, codes, fld, sod, state, system):
    """A loss of lock on the listed types with a slip on each listed phase at the return."""
    st = state.setdefault(("ds", n_op, sat), {"left": 0, "cyc": {}, "last": None, "n": 0,
                                              "rng": draw(op.get("seed", "1"), n_op, "ds", station, sat)})
    rng = st["rng"]
    if st["left"] == 0 and st["last"] is not None and sod - st["last"] < 61.0:
        if rng.random() < 1.0 - math.exp(-float(op.get("rate", 0.15)) * (sod - st["last"]) / 3600.0):
            st["left"] = int(op.get("len", 3))
    st["last"] = sod
    if st["left"] > 0:
        for k in op["_idx"]:
            fld[k] = " " * 16
        st["left"] -= 1
        if st["left"] == 0:
            st["n"] += 1
            for k in op["_idx"]:
                if codes[k][0] == "L":
                    step = rng.randint(1, int(op.get("max", 300)))
                    st["cyc"][k] = st["cyc"].get(k, 0) + (step if rng.random() < 0.5 else -step)
        return
    for k, cyc in st["cyc"].items():
        if fld[k][:14].strip():
            fld[k] = f"{float(fld[k][:14]) + cyc:14.3f}" + fld[k][14:16]


def delta_m(op, n_op, station, sat, code, sod, state):
    seed, kind = op.get("seed", "1"), op["op"]
    if kind == "bias":
        key = station if op.get("per", "station") == "station" else sat
        return draw(seed, n_op, "bias", key).gauss(0.0, float(op.get("sigma", 1.0)))
    if kind == "drift":
        per = op.get("per", "los")
        key = {"station": (station,), "sat": (sat,), "los": (station, sat)}[per]
        ph = draw(seed, n_op, "drift", *key).uniform(0.0, 2.0 * math.pi)
        return float(op.get("amp_m", 0.03)) * math.sin(2.0 * math.pi * sod / float(op.get("period_s", 5400.0)) + ph)
    if kind == "slip":                     # cumulative integer cycles per (station, sat, type), Poisson in time
        key = (n_op, station, sat, code)
        last, cyc, rng = state.get(key, (None, 0, None))
        if rng is None:
            rng = draw(seed, n_op, "slip", station, sat, code)
        if last is not None:
            dt_h = max(0.0, sod - last) / 3600.0
            if rng.random() < 1.0 - math.exp(-float(op.get("rate", 1.0)) * dt_h):
                step = rng.randint(1, int(op.get("max", 5)))
                cyc += step if rng.random() < 0.5 else -step
        state[key] = (sod, cyc, rng)
        return cyc * C / FREQ[op["sys"]][code[1]]      # metres; converted back to cycles by the caller
    raise ValueError(kind)


# ----------------------------------------------------------------------------- make
def set_key(cfg, dotted, value):
    node, keys = cfg, dotted.split(".")
    for k in keys[:-1]:
        node = node.setdefault(k, {})
    try:
        node[keys[-1]] = json.loads(value)
    except ValueError:
        node[keys[-1]] = value


def cmd_make(a):
    base = json.load(open(a.base))
    arm, rd = copy.deepcopy(base), f"{RUNS}/_rap_{a.tag}"
    net = arm["network"]
    for name in a.drop_station or []:
        n0 = len(net["stations"])
        net["stations"] = [s for s in net["stations"] if s["name"] != name]
        assert len(net["stations"]) == n0 - 1, f"station {name} not in the roster"
    ops = [parse_op(t) for t in (a.rinex or [])]
    for op in ops:
        op["_prio"] = base.get("processing", {}).get("signal_priorities", {}).get("GPS", [])
    touched, n_holes = [], 0
    if ops:
        files = [(k, s["name"], s["rinex_path"], s) for k, s in enumerate(net["stations"])]
        cons = net.get("closed_loop", {}).get("station")
        if cons:
            files.append((-1, cons["name"], cons["rinex_path"], cons))
        for idx, name, path, node in files:
            _, codes, _ = read_header(open(path, "r", encoding="ascii", errors="replace").read().split("\n"), "G")
            mine = []
            for op in ops:
                if idx == -1:
                    ok = op["consumer"] == "1" and (op["stations"] == "all" or op["stations"].startswith(("family:",))
                                                    and station_selected(name, 0, codes, op["stations"])
                                                    or name in op["stations"].split(","))
                else:
                    ok = station_selected(name, idx, codes, op["stations"])
                if ok:
                    mine.append(dict(op))
            if mine:
                dst = f"{rd}/rinex/{os.path.basename(path)}"
                n_holes += rewrite_rinex(path, dst, mine, name, hours=(a.hours + 0.05) if a.hours else None)
                node["rinex_path"] = dst
                touched.append(name)
    for kv in a.set or []:
        k, v = kv.split("=", 1)
        set_key(arm, k, v)
    if a.hours:
        for sec in ("processing", "network"):
            if "max_duration_s" in arm.get(sec, {}):
                arm[sec]["max_duration_s"] = a.hours * 3600.0
    net["product_output_path"] = rd + "/products.txt"
    os.makedirs(rd, exist_ok=True)
    json.dump(arm, open(rd + "/arm.json", "w"), indent=2)
    prov = {"base": a.base, "base_sha256": hashlib.sha256(open(a.base, "rb").read()).hexdigest(), "tag": a.tag,
            "hours": a.hours, "set": a.set or [], "drop_station": a.drop_station or [], "rinex": a.rinex or [],
            "rinex_touched": touched, "holes_records_deleted": n_holes}
    json.dump(prov, open(rd + "/rapid.json", "w"), indent=2)
    print(f"{a.tag}: {len(net['stations'])} stations, {len(touched)} RINEX file(s) rewritten"
          + (f", {n_holes} record(s) deleted by holes" if n_holes else "") + f", -> {rd}")


# ----------------------------------------------------------------------------- run
def resolve(tag):
    return tag if os.path.isdir(tag) else f"{RUNS}/_rap_{tag}"


def cmd_run(a):
    exe = a.exe.replace("\\", "/")
    assert "x64-Release" in exe and os.path.exists(exe), "a Release exe is required: " + exe

    def one(tag):
        rd = resolve(tag)
        t0 = time.time()
        subprocess.run(["pwsh", "-NoProfile", "-File", ROOT + "/scripts/ev_launch.ps1", "-Exe", exe,
                        "-Config", rd + "/arm.json", "-RunDir", rd], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ex = open(rd + "/exitcode.txt").read().split() if os.path.exists(rd + "/exitcode.txt") else ["?"]
        return tag, ex[0], time.time() - t0

    with cf.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for tag, ex, dt in pool.map(one, a.tags):
            print(f"{tag}: exit {ex}, {dt:.0f} s")


# ----------------------------------------------------------------------------- score
EVENTS = ("T4_attrib", "T1_if_slip", "T0_band_scope", "CLK_ANCHOR", "STRANSFORM", "T0_mw_detect")


def score_one(rd, pf=False):
    out = {"tag": os.path.basename(rd).replace("_rap_", "")}
    ex = open(rd + "/exitcode.txt").read().split() if os.path.exists(rd + "/exitcode.txt") else ["?", "?"]
    out["exit"], out["wall_s"] = ex[0], ex[1] if len(ex) > 1 else "?"
    p = rd + "/logs/closed_loop_trace.csv"
    hrs = {}
    if os.path.exists(p):
        rows = [r for r in csv.DictReader(open(p)) if r.get("tow") and r.get("pos_err_3d_m") is not None]
        t0 = min(float(r["tow"]) for r in rows) if rows else 0.0
        for r in rows:                      # a run still in flight has a partial last row
            try:
                hrs.setdefault(int((float(r["tow"]) - t0) // 3600), []).append(float(r["pos_err_3d_m"]))
            except (ValueError, TypeError):
                pass
    out["p50"] = " ".join(f"{st.median(v):.2f}" for _, v in sorted(hrs.items()))
    out["subdm"] = sum(1 for v in hrs.values() for e in v if e < 0.10)
    cnt = {k: 0 for k in EVENTS}
    cnt["if_slip"] = cnt["if_slip_b5"] = 0
    p = rd + "/logs/network_events.csv"
    if os.path.exists(p):
        for r in csv.reader(open(p)):
            if len(r) < 4:
                continue
            if r[2] in cnt:
                cnt[r[2]] += 1
            if r[2] == "T1_if_slip" and r[3] == "if_slip":
                cnt["if_slip"] += 1
                cnt["if_slip_b5"] += ("L5" in r[-1])
    out.update(cnt)
    if pf and os.path.exists(rd + "/logs/network_pf_rows.csv"):
        import numpy as np
        import pandas as pd
        df = pd.read_csv(rd + "/logs/network_pf_rows.csv", usecols=["tow", "station", "sat", "signal", "kind", "resid_m"],
                         dtype={"station": "category", "sat": "category", "signal": "category", "kind": "category"})
        df = df[(df["kind"] == "phase") & (df["tow"] >= df["tow"].min() + 3600.0)]
        df["band"] = df["signal"].astype(str).str[1]
        r = df["resid_m"] - df.groupby(["station", "sat", "signal"], observed=True)["resid_m"].transform("mean")
        rms = (r * r).groupby([df["band"], df["station"]], observed=True).mean().pow(0.5)
        out["pf_med_mm"] = " ".join(f"L{b}:{1000.0 * float(np.median(rms[b].dropna())):.1f}" for b in sorted(rms.index.levels[0]))
    return out


def cmd_score(a):
    rows = [score_one(resolve(t), a.pf) for t in a.tags]
    cols = ["tag", "exit", "wall_s", "p50", "subdm", "T4_attrib", "if_slip", "if_slip_b5", "T0_band_scope",
            "CLK_ANCHOR", "STRANSFORM", "T0_mw_detect"] + (["pf_med_mm"] if a.pf else [])
    w = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in cols}
    print("  ".join(c.ljust(w[c]) for c in cols))
    for r in rows:
        print("  ".join(str(r.get(c, "")).ljust(w[c]) for c in cols))


# ----------------------------------------------------------------------------- selftest
def cmd_selftest(_):
    hdr = ["     3.04           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE",
           "G    6 C1C C2W C5Q L1C L2W L5Q                              SYS / # / OBS TYPES",
           "E    2 C1C L1C                                              SYS / # / OBS TYPES",
           "                                                            END OF HEADER"]

    def rec(sat, vals):
        return sat + "".join(f"{v:14.3f} 7" for v in vals)

    body = []
    for e in range(4):
        body.append(f"> 2024 05 08 00 {e:02d} 00.0000000  0  3")
        body += [rec("G03", [2.0e7 + e, 2.0e7 + e, 2.0e7 + e, 1.0e8, 0.8e8, 0.7e8]),
                 rec("G05", [2.1e7, 2.1e7, 2.1e7, 1.1e8, 0.9e8, 0.8e8]), rec("E01", [2.2e7, 1.2e8])]
    d = tempfile.mkdtemp()
    src = d + "/a.rnx"
    open(src, "w", newline="\n").write("\n".join(hdr + body) + "\n")
    n = 0

    def chk(cond, what):
        nonlocal n
        n += 1
        assert cond, what

    # 1: strip band 5 everywhere removes the types from the header and the records
    rewrite_rinex(src, d + "/b.rnx", [parse_op("strip;types=?5?")], "AAAA")
    b = open(d + "/b.rnx").read().split("\n")
    chk(b[1].startswith("G    4 C1C C2W L1C L2W"), "header strip")
    chk(b[5] == rec("G03", [2.0e7, 2.0e7, 1.0e8, 0.8e8]).rstrip(), "record strip")
    chk(b[7] == body[3], "other system untouched")
    # 2: strip for a satellite subset blanks the fields and keeps the header
    rewrite_rinex(src, d + "/c.rnx", [parse_op("strip;types=?5?;sats=G03")], "AAAA")
    c = open(d + "/c.rnx").read().split("\n")
    chk(c[1] == hdr[1], "header kept")
    chk(c[5][3 + 32:3 + 48].strip() == "" and c[6] == body[2], "G03 blanked, G05 kept")
    # 3: a code bias is constant per station, identical across calls, and leaves the flags
    rewrite_rinex(src, d + "/e.rnx", [parse_op("bias;types=C5?;per=station;sigma=3;seed=7")], "AAAA")
    rewrite_rinex(src, d + "/f.rnx", [parse_op("bias;types=C5?;per=station;sigma=3;seed=7")], "AAAA")
    e = open(d + "/e.rnx").read().split("\n")
    chk(open(d + "/e.rnx").read() == open(d + "/f.rnx").read(), "deterministic")
    b3 = float(e[5][3 + 32:3 + 46]) - 2.0e7
    b5 = float(e[6][3 + 32:3 + 46]) - 2.1e7
    chk(abs(b3 - b5) < 1e-3 and abs(b3) > 1e-3 and e[5][3 + 46:3 + 48] == " 7", "per-station constant")
    chk(e[5][3:3 + 32] == body[1][3:3 + 32], "other types untouched")
    # 4: relabel renames the types only
    rewrite_rinex(src, d + "/g.rnx", [parse_op("relabel;from=?5Q;to=?5X")], "AAAA")
    g = open(d + "/g.rnx").read().split("\n")
    chk("C5X" in g[1] and "L5X" in g[1] and "C5Q" not in g[1] and g[5:] == (hdr + body)[5:] + [""], "relabel")
    # 5: a slip process adds whole cycles to the phase, cumulatively
    rewrite_rinex(src, d + "/h.rnx", [parse_op("slip;types=L5?;rate=3600;max=1;seed=2")], "AAAA")
    h = open(d + "/h.rnx").read().split("\n")
    steps = [float(h[5 + 4 * k][3 + 80:3 + 94]) - 0.7e8 for k in range(4)]
    chk(all(abs(s - round(s)) < 1e-3 for s in steps) and steps[0] == 0.0 and any(abs(s) >= 1 for s in steps[1:]),
        f"integer slips {steps}")
    # 6: truncation
    rewrite_rinex(src, d + "/i.rnx", [parse_op("strip;types=?5?")], "AAAA", hours=1.5 / 60.0)
    chk(sum(1 for x in open(d + "/i.rnx").read().split("\n") if x.startswith(">")) == 2, "truncate at the hour mark")
    # 7: holes: a phase-only dropout inside an arc deletes the satellite's record and fixes the count
    hb = list(hdr + body)
    full = rec("G03", [2.0e7 + 1, 2.0e7 + 1, 2.0e7 + 1, 1.0e8, 0.8e8, 0.7e8])
    hb[4 + 5] = full[:3 + 48] + " " * 16 + full[3 + 64:]          # epoch 2: G03 without its L1C
    open(d + "/j.rnx", "w", newline="\n").write("\n".join(hb) + "\n")
    nh = rewrite_rinex(d + "/j.rnx", d + "/k.rnx",
                       [dict(parse_op("holes;bands=12;window=3"), _prio=["L1C", "L2W"])], "AAAA")
    k = open(d + "/k.rnx").read().split("\n")
    chk(nh == 1 and k[8].startswith("> 2024 05 08 00 01") and k[8][32:35] == "  2" and not k[9].startswith("G03"),
        f"holes {nh} {k[8]!r}")
    chk(sum(1 for x in k if x.startswith("G03")) == 3 and sum(1 for x in k if x.startswith("G05")) == 4, "holes scope")
    n_mid = rewrite_rinex(d + "/j.rnx", d + "/k2.rnx",
                          [dict(parse_op("holes;bands=12;window=3;where=mid"), _prio=["L1C", "L2W"])], "AAAA")
    n_edge = rewrite_rinex(d + "/j.rnx", d + "/k3.rnx",
                           [dict(parse_op("holes;bands=12;window=3;where=edge"), _prio=["L1C", "L2W"])], "AAAA")
    chk(n_mid == 1 and n_edge == 0, f"holes where: mid {n_mid} edge {n_edge}")
    # 8: dropslip: the listed types vanish for `len` epochs and the phases return stepped by whole cycles
    long_body = []
    for e in range(12):
        long_body.append(f"> 2024 05 08 00 {e:02d} 00.0000000  0  1")
        long_body.append(rec("G03", [2.0e7, 2.0e7, 2.0e7, 1.0e8, 0.8e8, 0.7e8]))
    open(d + "/m.rnx", "w", newline="\n").write("\n".join(hdr + long_body) + "\n")
    rewrite_rinex(d + "/m.rnx", d + "/n.rnx", [parse_op("dropslip;types=L1?,L2?,C2?;rate=40;len=2;max=9;seed=5")], "AAAA")
    g = [x for x in open(d + "/n.rnx").read().split("\n") if x.startswith("G03")]
    blank = [k for k, x in enumerate(g) if not x.ljust(99)[3 + 48:3 + 62].strip()]
    chk(len(blank) >= 2 and all(x.ljust(99)[3:3 + 14].strip() and x.ljust(99)[3 + 80:3 + 94].strip() for x in g),
        f"dropslip keeps C1C and L5Q, blanks L1C at {blank}")
    first = next(k for k in range(blank[0] + 1, len(g)) if g[k].ljust(99)[3 + 48:3 + 62].strip())
    after = [float(g[first][3 + 48:3 + 62]) - 1.0e8]
    chk(after and abs(after[0]) >= 1 and abs(after[0] - round(after[0])) < 1e-3, f"dropslip integer step {after}")
    print(f"rapid_arm selftest: {n}/{n} PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--base", required=True)
    m.add_argument("--tag", required=True)
    m.add_argument("--hours", type=float)
    m.add_argument("--set", action="append")
    m.add_argument("--drop-station", action="append")
    m.add_argument("--rinex", action="append")
    m.set_defaults(fn=cmd_make)
    r = sub.add_parser("run")
    r.add_argument("--tags", nargs="+", required=True)
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--exe", default=ROOT + "/build/x64-Release/gnss_processor.exe")
    r.set_defaults(fn=cmd_run)
    s = sub.add_parser("score")
    s.add_argument("tags", nargs="+")
    s.add_argument("--pf", action="store_true", help="add the median-station post-fit phase RMS per band (slow)")
    s.set_defaults(fn=cmd_score)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
