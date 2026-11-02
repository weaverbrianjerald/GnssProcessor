#!/usr/bin/env python3
"""scenario_matrix.py (card SMX): the standing fault-scenario matrix of the network producer.

Every fault the network must survive (a slip on one or more signals, a storm of slips, a
single-signal loss of lock, a satellite or station outage, a satellite or receiver clock
step, an anchor handover, an arc erase, a broadcast health flip, a moved station, a
drifting satellite clock) is one ROW.  A row is one injected event on a converged seed,
60 epochs (240 for the storm), graded EVENT MINUS a matched NULL leg resumed from the same
snapshot, on datum-invariant quantities, against a bar frozen in ``scripts/smx/matrix.json``
before the first run (card SMX, docs/refound/SMX_scenario_matrix_2026-10-24.md).  Every row
runs on each POSTURE of the clock level where they differ.  The matrix runs on any Release
exe, the way the G5 identity gate does, and prints one table per exe.

    python scripts/scenario_matrix.py seed   <posture> [--hours 6] [--smoke] [--exe EXE] [--launch]
    python scripts/scenario_matrix.py make   --label L --exe EXE [--rows r1,r2] [--postures g,c] [--extra JSON]
    python scripts/scenario_matrix.py run    --label L [--jobs 3]
    python scripts/scenario_matrix.py grade  --label L
    python scripts/scenario_matrix.py report --label L [--control L2]

A control is the same matrix made under a second label with the same exe; ``report
--control`` compares every measured value of the two and must read IDENTICAL (the producer
is deterministic; a difference is a defect of the harness or of the exe, never noise).

``make --extra JSON`` (or ``--extra @file``) arms knobs for the whole label: the object is
deep-merged into the config of every leg of the label, event and null (a leg inherits its
posture's extra from the seed, so only the new object is applied), and into the fresh arms
of the drift row over the posture's extra.  It is recorded in plan.json as ``extra``; with
no ``--extra`` the plan and every leg are exactly what they were before the option existed.

Postures (the clock level of the goldens, card SCB's table):
  g   the sim golden as armed (GPS + Galileo; Q3 anchor on the COD a priori, phase-clock hold)
  c   GPS only + the self-contained regime of card SCL2B (clock clone, shared history, no hold)
  n   c + the native broadcast-clock a priori of card NBP (no CLK file in its data root)
  gx  g + network.sat_health "exclude" (the reader of a health flip; fingerprinted, own seed)
  f   the field golden (no truth; seeds only in version 1, no rows run on it)

Seeds are written under RAPID_RUNS_ROOT (default D:/GnssProcessorSupport/runs_nom) as
``evseed_smx_<posture>``; legs as ``evleg_smx_<label>_<row>_<posture>`` (nulls ``..._null60``
and ``..._null240``); the drift row's fresh arms as ``smx_<label>_drift_<q|k>_<posture>``;
the plan, the grades and the report under ``D:/GnssProcessorSupport/gates/smx/<label>/``.
"""
import argparse
import concurrent.futures as cf
import copy
import csv
import hashlib
import json
import math
import os
import random
import re
import statistics as st
import subprocess
import sys
import time
from collections import defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
ROOT = os.path.dirname(_HERE).replace("\\", "/")
RUNS = (os.environ.get("RAPID_RUNS_ROOT") or "D:/GnssProcessorSupport/runs_nom").replace("\\", "/").rstrip("/")
GATES = "D:/GnssProcessorSupport/gates/smx"
MATRIX = ROOT + "/scripts/smx/matrix.json"

SIM_GOLDEN = ROOT + "/config/golden/golden_sim_refound.json"
FIELD_GOLDEN = ROOT + "/config/golden/golden_field_refound.json"
QUIET = "C:/GnssProcessor/runs/simnet60_quiet_col_24h"
NATIVE_ROOT = "C:/GnssProcessor/runs/_scl_data_nbp"      # card NBP's CLK-free data root
START_OFFSET_H = 0.504167                                # 00:30:15, the sim grading start
EPOCH_S = 30.0
CONSUMER = "SZZ000SIM"

REGIME_NET = {
    "carry_coupling_clock_clone": True,
    "handover_stransform_dense": True,
    "clock_anchor_datum_keep": True,
    "carry_coupling_shared_history": True,
    "carry_coupling_shared_history_rank": 1,
    "clock_anchor_q": 5e-06,
    "emit_pb_hold_phase_clock": False,
    "emit_pb_hold_smoothed": True,
    "clock_level_monitor": True,
    "phase_visible_monitor": True,
}
# posture -> (base golden, overrides, bed); card SCB's scripts/clock_level/scb_arms.py keys
POSTURES = {
    "g": (SIM_GOLDEN, {}, "sim"),
    "c": (SIM_GOLDEN, {"processing": {"active_systems": ["GPS"]}, "network": REGIME_NET}, "sim"),
    "n": (SIM_GOLDEN, {"processing": {"active_systems": ["GPS"]},
                       "files": {"internal_data_root": NATIVE_ROOT},
                       "network": dict(REGIME_NET, apriori_source="sp3_brdc_clock", brdc_select="ttom")},
          "sim"),
    "gx": (SIM_GOLDEN, {"network": {"sat_health": "exclude"}}, "sim"),
    "f": (FIELD_GOLDEN, {}, "field"),
}


# ------------------------------------------------------------------------------ helpers
def deep_merge(dst, src):
    """Recursive dict merge; arrays and scalars replace (the config-inheritance rule)."""
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            deep_merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def posture_extra(posture):
    """The --extra object a seed of this posture carries over its golden."""
    _, over, bed = POSTURES[posture]
    extra = copy.deepcopy(over)
    if bed == "sim":
        deep_merge(extra, {"network": {"start_offset_hours": START_OFFSET_H}})
    return extra


def seed_dir(posture, smoke=False):
    return f"{RUNS}/evseed_smx_{posture}{'_smoke' if smoke else ''}"


def default_exe():
    return ROOT + "/build/x64-Release/gnss_processor.exe"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_matrix():
    return json.load(open(MATRIX, encoding="utf-8"))


def gate_dir(label):
    d = f"{GATES}/{label}"
    os.makedirs(d, exist_ok=True)
    return d


def exitcode(d):
    p = d + "/exitcode.txt"
    if not os.path.exists(p):
        return None, None
    f = open(p).read().split()
    return (f[0] if f else "?"), (float(f[1]) if len(f) > 1 else None)


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else 0.0


def white(diffs):
    """White-noise equivalent of a set of first differences: sqrt(mean(d^2)/2)."""
    return math.sqrt(sum(d * d for d in diffs) / (2.0 * len(diffs))) if diffs else 0.0


# ------------------------------------------------------------------------------ seed
def cmd_seed(a):
    base, _, _ = POSTURES[a.posture]
    name = f"smx_{a.posture}{'_smoke' if a.smoke else ''}"
    hours = 10 * EPOCH_S / 3600.0 if a.smoke else a.hours
    cmd = [sys.executable, ROOT + "/scripts/event_leg.py", "write-seed", "--name", name,
           "--base", base, "--hours", repr(hours), "--interval-ep", str(a.interval_ep),
           "--exe", a.exe, "--runs-root", RUNS, "--extra", json.dumps(posture_extra(a.posture))]
    subprocess.run(cmd, check=True, cwd=ROOT)
    rundir = seed_dir(a.posture, a.smoke)
    if a.launch:
        launch(a.exe, rundir + "/seed.json", rundir)
        print(f"{name}: exit {exitcode(rundir)[0]}")


def launch(exe, cfg, rundir):
    """The harness launcher (cwd = run dir, so ./logs cannot collide; writes exitcode.txt)."""
    return subprocess.call(["pwsh", "-NoProfile", "-File", ROOT + "/scripts/ev_launch.ps1",
                            "-Exe", exe, "-Config", cfg, "-RunDir", rundir],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ------------------------------------------------------------------------------ targets
def snapshot_ep(sdir, index):
    """The n_emitted suffix of the seed's index-th history snapshot (0-based, in order)."""
    eps = sorted(int(f[len("state_snapshot.txt.ep"):]) for f in os.listdir(sdir)
                 if re.fullmatch(r"state_snapshot\.txt\.ep\d{4}", f))
    if index >= len(eps):
        raise SystemExit(f"{sdir}: {len(eps)} snapshots, index {index} asked")
    return eps[index]


def snapshot_tow(sdir, ep):
    import event_leg
    week, tow, _, _ = event_leg.read_snapshot_header(f"{sdir}/state_snapshot.txt.ep{ep:04d}")
    return week, tow


def gps_pivots(sdir, tow_max):
    """GPS clock pivots of the seed up to tow_max: the S line's and every PIVOT event's."""
    piv = set()
    p = sdir + "/products.txt"
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("S,"):
                for tok in line.strip().split(","):
                    if tok.startswith("G:"):
                        piv.add(tok[2:])
                break
    last = None
    ev = sdir + "/logs/network_events.csv"
    if os.path.exists(ev):
        for r in csv.DictReader(open(ev, newline="")):
            if r.get("tier") != "PIVOT" or not r.get("sat", "").startswith("G"):
                continue
            try:
                if float(r["tow"]) <= tow_max + 0.5:
                    last = r["sat"]
            except ValueError:
                pass
    if last:
        piv.add(last)
    return piv


def anchor_station(sdir, cons, signal, sat, tow_max):
    """The station anchoring (cons, signal, sat) at the last trace row at or before tow_max."""
    p = sdir + "/logs/network_phase_anchor.csv"
    best = None
    for r in csv.DictReader(open(p, newline="")):
        if r["cons"] == cons and r["signal"] == signal and r["sat"] == sat:
            try:
                if float(r["tow"]) <= tow_max + 0.5:
                    best = r["anchor_sta"]
            except ValueError:
                pass
    return best


def resolve_targets(m, posture):
    sdir = seed_dir(posture)
    ep = snapshot_ep(sdir, m["seed_snapshot_index"])
    _, tow = snapshot_tow(sdir, ep)
    sat = "G25" if "G25" not in gps_pivots(sdir, tow) else "G12"
    anc = anchor_station(sdir, "G", "L2W", sat, tow)
    return {"$STA": m["station"], "$SAT": sat, "$ANCHOR_L2W": anc or m["station"]}, tow, ep


def substitute(obj, sub):
    if isinstance(obj, dict):
        return {substitute(k, sub): substitute(v, sub) for k, v in obj.items()}
    if isinstance(obj, list):
        return [substitute(v, sub) for v in obj]
    if isinstance(obj, str):
        return sub.get(obj, obj)
    return obj


def storm_schedule(spec, stations, first_tow, epochs):
    """The ev_dose_legs.py form: Poisson per station at the campaign rate, GPS only."""
    rng = random.Random(spec["rng"])
    lo_ep, hi_ep = spec["first_ep"], epochs - spec["last_margin"]
    hours = (hi_ep - lo_ep + 1) * EPOCH_S / 3600.0
    lo_tow, hi_tow = first_tow + lo_ep * EPOCH_S, first_tow + hi_ep * EPOCH_S
    events = []
    for sta in stations:
        el = defaultdict(dict)
        for r in csv.DictReader(open(f"{QUIET}/{sta}/truth_atmo.csv", newline="")):
            t = float(r["tow_s"])
            if lo_tow - 1 <= t <= hi_tow + 1 and r["satellite"].startswith("G"):
                el[int(round(t))][r["satellite"]] = float(r["elev_deg"])
        lam = spec["rate_per_sta_h"] * hours
        n, L, p = 0, math.exp(-lam), 1.0          # Knuth's Poisson draw, as ev_dose_legs.py
        while True:
            p *= rng.random()
            if p <= L:
                break
            n += 1
        n = max(1, n)
        used, mine, tries = [], [], 0
        while len(mine) < n and tries < 5000:
            tries += 1
            ep = rng.randint(lo_ep, hi_ep)
            tow = first_tow + ep * EPOCH_S
            cands = sorted(s for s, e in el.get(int(round(tow)), {}).items() if e >= spec["min_elev"])
            if not cands:
                continue
            sat = rng.choice(cands)
            if any(s == sat and abs(e - ep) < spec["min_gap_ep"] for s, e in used):
                continue
            mine.append({"kind": "slip", "station": sta, "sat": sat,
                         "signal": rng.choice(spec["bands"]),
                         "tow": tow, "delta_cycles": float(rng.choice((-1, 1))
                                                           * rng.randint(spec["delta_min"], spec["delta_max"])),
                         "flag_lli": rng.random() < spec["lli_p"]})
            used.append((sat, ep))
        events.extend(mine)
    events.sort(key=lambda e: (e["tow"], e["station"], e["sat"], e["signal"]))
    return events


# ------------------------------------------------------------------------------ make
def drift_arm(label, posture, fx, fixture, hours, exe, extra=None):
    """A fresh arm of the drift row: the posture on one fixture, from 00:30:15, `hours` long."""
    base, over, _ = POSTURES[posture]
    arm = json.load(open(base, encoding="utf-8"))
    deep_merge(arm, over)
    if extra:
        deep_merge(arm, extra)    # the label's --extra, over the posture's
    net = arm["network"]
    n = 0
    for s in net["stations"] + [net["closed_loop"]["station"]]:
        assert s["rinex_path"].startswith(QUIET + "/"), s["rinex_path"]
        s["rinex_path"] = fixture + s["rinex_path"][len(QUIET):]
        n += 1
    assert n == 61, n
    net["start_offset_hours"] = START_OFFSET_H
    for sec in ("processing", "network"):
        if "max_duration_s" in arm.get(sec, {}) or sec == "processing":
            arm.setdefault(sec, {})["max_duration_s"] = hours * 3600.0
    rd = f"{RUNS}/smx_{label}_drift_{fx}_{posture}"
    os.makedirs(rd + "/logs", exist_ok=True)
    net["product_output_path"] = rd + "/products.txt"
    json.dump(arm, open(rd + "/arm.json", "w"), indent=2)
    return {"name": os.path.basename(rd), "dir": rd, "config": rd + "/arm.json"}


def make_leg(label, posture, row_id, events, epochs, at_ep, exe, snap_ep, extra=None):
    name = f"smx_{label}_{row_id}_{posture}"
    cmd = [sys.executable, ROOT + "/scripts/event_leg.py", "null" if events is None else "make-leg",
           "--seed-run", seed_dir(posture), "--snapshot-ep", str(snap_ep),
           "--epochs", str(epochs), "--name", name, "--at-ep", str(at_ep), "--exe", exe,
           "--runs-root", RUNS]
    if extra:
        # The leg inherits the seed's config (the posture's extra with it); event_leg
        # deep-merges this object last, so it overrides anything the seed carries.
        cmd += ["--extra", json.dumps(extra)]
    if events is not None:
        # Through a file: a storm schedule is too long for a Windows command line.
        os.makedirs(f"{RUNS}/smx_events", exist_ok=True)
        ef = f"{RUNS}/smx_events/{name}.json"
        json.dump(events, open(ef, "w"), indent=1)
        cmd += ["--event", "@" + ef]
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"make-leg failed for {name}:\n{out.stdout}\n{out.stderr}")
    d = f"{RUNS}/evleg_{name}"
    return {"name": name, "dir": d, "config": d + "/leg.json"}


def parse_extra(text):
    """The --extra object of `make`: a JSON object (or @file holding one); {} when absent."""
    if not text:
        return {}
    try:
        extra = json.loads(open(text[1:], encoding="utf-8").read() if text.startswith("@") else text)
    except (ValueError, OSError) as e:
        raise SystemExit(f"FATAL: --extra is not valid JSON: {e}")
    if not isinstance(extra, dict):
        raise SystemExit("FATAL: --extra must be a JSON object")
    return extra


def cmd_make(a):
    m = load_matrix()
    extra = parse_extra(a.extra)
    exe = a.exe.replace("\\", "/")
    assert "x64-Release" in exe and os.path.exists(exe), "a Release exe is required: " + exe
    rows = [r for r in m["rows"] if not a.rows or r["id"] in a.rows.split(",")]
    only = set(a.postures.split(",")) if a.postures else None
    postures = [p for p in m["postures"] if only is None or p in only]

    def postures_of(r):
        """The matrix's postures for every row, plus the row's own extra ones (filtered)."""
        return postures + [p for p in r.get("extra_postures", []) if only is None or p in only]
    plan = {"label": a.label, "exe": exe, "exe_sha256": sha256(exe), "matrix_version": m["version"],
            "matrix_sha256": sha256(MATRIX), "made": time.strftime("%Y-%m-%d %H:%M:%S"),
            "postures": postures, "targets": {}, "legs": [], "cells": []}
    if extra:
        plan["extra"] = extra
    old_plan = gate_dir(a.label) + "/plan.json"
    if a.append and os.path.exists(old_plan):
        # Add postures or rows to a label (a seed that finished later): same exe, same matrix.
        prev = json.load(open(old_plan))
        if prev["exe_sha256"] != plan["exe_sha256"] or prev["matrix_sha256"] != plan["matrix_sha256"]:
            raise SystemExit("FATAL: --append needs the label's exe and matrix unchanged")
        if prev.get("extra", {}) != extra:
            raise SystemExit("FATAL: --append needs the label's --extra unchanged")
        plan.update({k: prev[k] for k in ("targets", "legs", "cells", "made")})
        plan["postures"] = sorted(set(prev["postures"]) | set(postures))
    needed = set()
    for r in rows:
        needed |= set(postures_of(r))
    for p in sorted(needed):
        sub, tow, ep = resolve_targets(m, p)
        plan["targets"][p] = dict(sub, snapshot_tow=tow, snapshot_ep=ep)
        print(f"posture {p}: snapshot tow {tow}, targets {sub}")
    made = {(l["posture"], l["row"]): l for l in plan["legs"]}
    have = {(c["row"], c["posture"]) for c in plan["cells"]}

    def leg(p, row_id, events, epochs):
        key = (p, row_id)
        if key not in made:
            rec = make_leg(a.label, p, row_id, events, epochs, m["at_ep"], exe,
                           plan["targets"][p]["snapshot_ep"], extra)
            rec.update({"posture": p, "row": row_id, "epochs": epochs,
                        "role": "null" if events is None else "event"})
            plan["legs"].append(rec)
            made[key] = rec
        return made[key]

    for r in rows:
        for p in postures_of(r):
            if (r["id"], p) in have:
                continue
            sub = plan["targets"][p]
            if r.get("drift"):
                d = m["drift"]
                ev = drift_arm(a.label, p, "k", d["fixture_event"], d["hours"], exe, extra)
                nu = drift_arm(a.label, p, "q", d["fixture_null"], d["hours"], exe, extra)
                for rec, role in ((ev, "event"), (nu, "null")):
                    rec.update({"posture": p, "row": r["id"], "epochs": None, "role": role})
                    plan["legs"].append(rec)
                plan["cells"].append({"row": r["id"], "posture": p, "event": ev["name"], "null": nu["name"],
                                      "drift": True})
                continue
            if "storm" in r:
                epochs = m["storm_epochs"]
                null = leg(p, "null240", None, epochs)
                tow = sub["snapshot_tow"]
                stations = [s["name"] for s in json.load(open(seed_dir(p) + "/seed.json"))["network"]["stations"]]
                events = storm_schedule(r["storm"], stations, tow + EPOCH_S, epochs)
            else:
                epochs = m["leg_epochs"]
                null = leg(p, "null60", None, epochs)
                events = substitute(r["events"], sub)
            ev = leg(p, r["id"], events, epochs)
            cell = {"row": r["id"], "posture": p, "event": ev["name"], "null": null["name"],
                    "n_events": len(events)}
            if "expect_clk_m" in r:
                cell["expect_clk_m"] = substitute(r["expect_clk_m"], sub)
            if "plant_m" in r:
                cell["plant_m"] = r["plant_m"]
            plan["cells"].append(cell)
    gd = gate_dir(a.label)
    json.dump(plan, open(gd + "/plan.json", "w"), indent=2)
    print(f"{len(plan['legs'])} legs, {len(plan['cells'])} cells -> {gd}/plan.json")


# ------------------------------------------------------------------------------ run
def cmd_run(a):
    plan = json.load(open(gate_dir(a.label) + "/plan.json"))
    exe = plan["exe"]
    if sha256(exe) != plan["exe_sha256"]:
        raise SystemExit(f"FATAL: {exe} changed since make (sha256); re-make the label")
    todo = [l for l in plan["legs"] if a.force or exitcode(l["dir"])[0] is None]
    # Nulls first: an event leg is graded against its null, and a long null should not wait.
    todo.sort(key=lambda l: (l["role"] != "null", -(l["epochs"] or 240)))

    def one(l):
        t0 = time.time()
        if l["config"].endswith("leg.json") and a.force:
            subprocess.call([sys.executable, ROOT + "/scripts/event_leg.py", "rerun", l["dir"], "--exe", exe],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            launch(exe, l["config"], l["dir"])
        return l["name"], exitcode(l["dir"])[0], time.time() - t0

    print(f"{len(todo)} of {len(plan['legs'])} legs to run, {a.jobs} at a time, exe {exe}")
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for name, ex, dt in pool.map(one, todo):
            print(f"{name}: exit {ex}, {dt:.0f} s", flush=True)


# ------------------------------------------------------------------------------ readers
def read_wire(d):
    """products.txt -> {tow: {"C": {sat: m}, "P": {(sat, sig): (m, gen)}}}."""
    out, tow = {}, None
    p = d + "/products.txt"
    if not os.path.exists(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            fs = line.rstrip("\n").split(",")
            if fs[0] == "E" and len(fs) >= 3:
                tow = int(round(float(fs[2])))
                out.setdefault(tow, {"C": {}, "P": {}})
            elif tow is None:
                continue
            elif fs[0] == "C" and len(fs) >= 3:
                out[tow]["C"][fs[1]] = float(fs[2])
            elif fs[0] == "P" and len(fs) >= 4:
                gen = next((t[4:] for t in fs[4:] if t.startswith("gen=")), None)
                out[tow]["P"][(fs[1], fs[2])] = (float(fs[3]), gen)
    return out


def read_csv_by_tow(path, key_cols, val_col):
    out = defaultdict(dict)
    if not os.path.exists(path):
        return out
    for r in csv.DictReader(open(path, newline="")):
        try:
            out[int(round(float(r["tow"])))][tuple(r[c] for c in key_cols)] = float(r[val_col])
        except (KeyError, ValueError):
            pass
    return out


def read_cons(d):
    out = {}
    p = d + "/logs/closed_loop_trace.csv"
    if not os.path.exists(p):
        return out
    for r in csv.DictReader(open(p, newline="")):
        try:
            out[int(round(float(r["tow"])))] = (float(r["east_m"]), float(r["north_m"]), float(r["up_m"]))
        except (KeyError, ValueError):
            pass
    return out


_TRUTH_ZWD = {}


def truth_zwd(sta):
    if sta not in _TRUTH_ZWD:
        t = {}
        p = f"{QUIET}/{sta}/truth_tropo.csv"
        if os.path.exists(p):
            for r in csv.DictReader(open(p, newline="")):
                t[int(round(float(r["tow_s"])))] = float(r["zwd_m"])
        _TRUTH_ZWD[sta] = t
    return _TRUTH_ZWD[sta]


def decommon(vals, group):
    """{key: v} -> {key: v - median of its group}; group(key) names the group."""
    g = defaultdict(list)
    for k, v in vals.items():
        g[group(k)].append(v)
    med = {k: st.median(v) for k, v in g.items()}
    return {k: v - med[group(k)] for k, v in vals.items()}


def injected_rows(d):
    p = d + "/logs/network_events.csv"
    n = 0
    if os.path.exists(p):
        for r in csv.DictReader(open(p, newline="")):
            if r.get("tier") == "INJECT" and r.get("action") == "injected":
                n += 1
    return n


def unread_health(d):
    p = d + "/stdout.txt"
    return os.path.exists(p) and "UNREAD on this posture" in open(p, encoding="utf-8", errors="replace").read()


# ------------------------------------------------------------------------------ grade
def grade_cell(m, cell, legs):
    """Event minus null for one (row, posture) cell, every channel, against its bar."""
    ev, nu = legs[cell["event"]], legs[cell["null"]]
    meta = json.load(open(ev["dir"] + "/leg_meta.json"))
    t0 = min(int(round(e["tow"])) for e in meta["events"])
    out = {"row": cell["row"], "posture": cell["posture"], "event": ev["name"], "null": nu["name"],
           "exit_event": exitcode(ev["dir"])[0], "exit_null": exitcode(nu["dir"])[0],
           "wall_event_s": exitcode(ev["dir"])[1], "t0": t0, "injected_rows": injected_rows(ev["dir"])}
    if cell["row"] == "health":
        out["unread"] = unread_health(ev["dir"])
    W_e, W_n = read_wire(ev["dir"]), read_wire(nu["dir"])
    post = sorted(t for t in W_n if t >= t0)
    pre = sorted(t for t in W_n if t < t0 and t in W_e)
    out["post_epochs"] = len(post)
    ch = {}

    # emit: post-event epochs the null emitted and the event leg did not
    ch["emit"] = {"E": float(sum(1 for t in post if t not in W_e)), "S": 0.0}
    common = [t for t in post if t in W_e]
    # pre-event determinism: both legs are byte-identical before the event
    pre_max = 0.0
    for t in pre:
        for s in set(W_e[t]["C"]) & set(W_n[t]["C"]):
            pre_max = max(pre_max, abs(W_e[t]["C"][s] - W_n[t]["C"][s]))
    out["pre_max_clk_mm"] = 1000.0 * pre_max

    # clk
    exp = cell.get("expect_clk_m", {})
    e_clk, prev, dif = 0.0, None, []
    for t in common:
        d = {s: W_e[t]["C"][s] - W_n[t]["C"][s] - exp.get(s, 0.0)
             for s in set(W_e[t]["C"]) & set(W_n[t]["C"])}
        if len(d) >= 3:
            r = decommon(d, lambda s: s[0])
            e_clk = max(e_clk, rms(list(r.values())))
    for t in post:
        c = decommon(W_n[t]["C"], lambda s: s[0])
        if prev is not None:
            dif += [c[s] - prev[s] for s in c if s in prev]
        prev = c
    ch["clk"] = {"E": 1000.0 * e_clk, "S": 1000.0 * white(dif)}

    # pb (same hold generation in both legs)
    e_pb, prev, dif, regen = 0.0, None, [], set()
    for t in common:
        d = {}
        for k in set(W_e[t]["P"]) & set(W_n[t]["P"]):
            (ve, ge), (vn, gn) = W_e[t]["P"][k], W_n[t]["P"][k]
            if ge == gn:
                d[k] = ve - vn
            else:
                regen.add(k)
        if len(d) >= 3:
            r = decommon(d, lambda k: (k[0][0], k[1]))
            e_pb = max(e_pb, rms(list(r.values())))
    for t in post:
        c = decommon({k: v for k, (v, _) in W_n[t]["P"].items()}, lambda k: (k[0][0], k[1]))
        if prev is not None:
            dif += [c[k] - prev[k] for k in c if k in prev]
        prev = c
    ch["pb"] = {"E": 1000.0 * e_pb, "S": 1000.0 * white(dif)}
    out["pb_regenerated"] = len(regen)

    # consumer
    C_e, C_n = read_cons(ev["dir"]), read_cons(nu["dir"])
    cpost = [t for t in sorted(C_n) if t >= t0 and t in C_e]
    dv = [tuple(a - b for a, b in zip(C_e[t], C_n[t])) for t in cpost]
    peak = max((math.sqrt(sum(x * x for x in v)) for v in dv), default=0.0)
    tail = dv[-m["plateau_epochs"]:]
    plat = math.sqrt(sum((sum(v[i] for v in tail) / len(tail)) ** 2 for i in range(3))) if tail else 0.0
    nd = [tuple(a - b for a, b in zip(C_n[t2], C_n[t1])) for t1, t2 in zip(cpost, cpost[1:])]
    s_cons = math.sqrt(sum(sum(x * x for x in v) for v in nd) / (2.0 * len(nd))) if nd else 0.0
    ch["cons_peak"] = {"E": 1000.0 * peak, "S": 1000.0 * s_cons}
    ch["cons_plat"] = {"E": 1000.0 * plat, "S": 1000.0 * s_cons}

    # ZWD error growth against truth
    Z_e = read_csv_by_tow(ev["dir"] + "/logs/network_zwd.csv", ["station"], "zwd_m")
    Z_n = read_csv_by_tow(nu["dir"] + "/logs/network_zwd.csv", ["station"], "zwd_m")
    e_z, where, dif, prev = -1e9, None, [], None
    for t in sorted(Z_n):
        if t < t0:
            continue
        if t in Z_e:
            for k in set(Z_e[t]) & set(Z_n[t]):
                tz = truth_zwd(k[0]).get(t)
                if tz is None:
                    continue
                g = abs(Z_e[t][k] - tz) - abs(Z_n[t][k] - tz)
                if g > e_z:
                    e_z, where = g, k[0]
        if prev is not None:
            dif += [Z_n[t][k] - Z_n[prev][k] for k in Z_n[t] if k in Z_n[prev]]
        prev = t
    ch["zwd"] = {"E": 1000.0 * max(e_z, 0.0), "S": 1000.0 * white(dif)}
    out["zwd_worst_station"] = where

    # slant ionosphere (de-commoned per station and epoch)
    I_e = read_csv_by_tow(ev["dir"] + "/logs/network_ionosphere.csv", ["station", "sat"], "iono_total_m")
    I_n = read_csv_by_tow(nu["dir"] + "/logs/network_ionosphere.csv", ["station", "sat"], "iono_total_m")
    e_i, where, dif, prev = 0.0, None, [], None
    for t in sorted(I_n):
        if t < t0:
            continue
        c = decommon(I_n[t], lambda k: k[0])
        if t in I_e:
            d = {k: I_e[t][k] - I_n[t][k] for k in set(I_e[t]) & set(I_n[t])}
            for k, v in decommon(d, lambda k: k[0]).items():
                if abs(v) > e_i:
                    e_i, where = abs(v), "/".join(k)
        if prev is not None:
            dif += [c[k] - prev[k] for k in c if k in prev]
        prev = c
    ch["iono"] = {"E": 1000.0 * e_i, "S": 1000.0 * white(dif)}
    out["iono_worst_los"] = where

    # bars
    ref = m["plant_reference"]
    pm = cell.get("plant_m", 0.0)
    refs = {"clk": ref["clk_mm_per_m"] * pm, "pb": ref["pb_mm_per_m"] * pm,
            "cons_peak": ref["cons_peak_mm_per_m"] * pm, "cons_plat": ref["cons_plat_mm_per_m"] * pm}
    ok = out["exit_event"] == "0" and out["exit_null"] == "0" and out["injected_rows"] > 0
    worst = None
    for k, c in ch.items():
        spec = m["channels"][k]
        c["bar"] = max(spec["factor"] * c["S"], spec["floor_mm"]) + refs.get(k, 0.0)
        c["pass"] = c["E"] <= c["bar"] + 1e-9
        ok = ok and c["pass"]
        ratio = c["E"] / c["bar"] if c["bar"] > 0 else (math.inf if c["E"] > 0 else 0.0)
        if worst is None or ratio > worst[1]:
            worst = (k, ratio)
    out["channels"] = ch
    out["pass"] = ok
    out["worst"] = worst[0]
    # Report-only: an event with no effect on any channel may have hit nothing the producer
    # used (a target arc already out of the solve); the cell then tests nothing.
    out["no_effect"] = all(c["E"] == 0.0 for c in ch.values())
    return out


def grade_drift(m, cell, legs):
    import simnet_clock_truth_grade as scg
    d = m["drift"]
    ev, nu = legs[cell["event"]], legs[cell["null"]]
    out = {"row": cell["row"], "posture": cell["posture"], "event": ev["name"], "null": nu["name"],
           "exit_event": exitcode(ev["dir"])[0], "exit_null": exitcode(nu["dir"])[0],
           "wall_event_s": exitcode(ev["dir"])[1], "injected_rows": None}
    h = d["graded_hour"]
    ge, _ = scg.grade(ev["dir"], d["fixture_event"], scg.DEFAULT_CLK, d["hours"])
    gn, _ = scg.grade(nu["dir"], d["fixture_null"], scg.DEFAULT_CLK, d["hours"])
    lev_e = ge["hours"][h - 1]["level_rms_m"]
    lev_n = gn["hours"][h - 1]["level_rms_m"]
    ch = {"clk_truth": {"E": 1000.0 * (lev_e - lev_n), "S": 1000.0 * lev_n}}
    ce, cn = read_cons(ev["dir"]), read_cons(nu["dir"])
    tn = sorted(cn)
    t_lo = tn[0] + (h - 1) * 3600.0
    hrs_n = [t for t in tn if t_lo <= t < t_lo + 3600.0]
    nd = [tuple(a - b for a, b in zip(cn[t2], cn[t1])) for t1, t2 in zip(hrs_n, hrs_n[1:])]
    s_cons = math.sqrt(sum(sum(x * x for x in v) for v in nd) / (2.0 * len(nd))) if nd else 0.0
    p50e, p50n = ge["consumer_p50_m"][h - 1], gn["consumer_p50_m"][h - 1]
    ch["cons_p50"] = {"E": 1000.0 * (p50e - p50n), "S": 1000.0 * s_cons}
    out["clk_truth_h"] = {"event_m": lev_e, "null_m": lev_n}
    out["cons_p50_h"] = {"event_m": p50e, "null_m": p50n}
    ok = out["exit_event"] == "0" and out["exit_null"] == "0"
    worst = None
    for k, c in ch.items():
        spec = d["channels"][k]
        c["bar"] = max(spec["factor"] * c["S"], spec["floor_mm"])
        c["pass"] = c["E"] <= c["bar"] + 1e-9
        ok = ok and c["pass"]
        ratio = c["E"] / c["bar"] if c["bar"] > 0 else 0.0
        if worst is None or ratio > worst[1]:
            worst = (k, ratio)
    out["channels"] = ch
    out["pass"] = ok
    out["worst"] = worst[0]
    return out


def cmd_grade(a):
    m = load_matrix()
    gd = gate_dir(a.label)
    plan = json.load(open(gd + "/plan.json"))
    legs = {l["name"]: l for l in plan["legs"]}
    res = []
    for cell in plan["cells"]:
        try:
            r = grade_drift(m, cell, legs) if cell.get("drift") else grade_cell(m, cell, legs)
        except Exception as e:  # noqa: BLE001 - a cell that cannot be graded is reported, not skipped
            r = {"row": cell["row"], "posture": cell["posture"], "pass": False, "error": repr(e),
                 "event": cell["event"], "null": cell["null"], "channels": {}}
        res.append(r)
        print(f"{r['row']:>10} {r['posture']:>3}: {'PASS' if r['pass'] else 'FAIL'}"
              + (f"  ({r['error']})" if "error" in r else f"  worst {r.get('worst')}"), flush=True)
    json.dump({"label": a.label, "exe": plan["exe"], "exe_sha256": plan["exe_sha256"],
               "matrix_sha256": plan["matrix_sha256"], "cells": res}, open(gd + "/grades.json", "w"), indent=2)
    print(f"-> {gd}/grades.json")


# ------------------------------------------------------------------------------ report
CH_ORDER = ["clk", "pb", "cons_peak", "cons_plat", "zwd", "iono", "emit", "clk_truth", "cons_p50"]


def cell_text(r):
    if "error" in r:
        return "ERROR"
    if not r["channels"]:
        return "?"
    k = r["worst"]
    c = r["channels"][k]
    tag = "PASS" if r["pass"] else "FAIL"
    if r.get("exit_event") not in (None, "0"):
        tag = f"FAIL(exit {r['exit_event']})"
    note = " UNREAD" if r.get("unread") else (" NO-EFFECT" if r.get("no_effect") else "")
    return f"{tag}{note} {k} {c['E']:.1f}/{c['bar']:.1f}"


def cmd_report(a):
    m = load_matrix()
    gd = gate_dir(a.label)
    G = json.load(open(gd + "/grades.json"))
    cells = {(r["row"], r["posture"]): r for r in G["cells"]}
    postures = []
    for r in G["cells"]:
        if r["posture"] not in postures:
            postures.append(r["posture"])
    rows = [r["id"] for r in m["rows"] if any(k[0] == r["id"] for k in cells)]
    L = [f"# Scenario matrix `{a.label}`", "",
         f"exe `{G['exe']}` (sha256 `{G['exe_sha256'][:16]}`), matrix sha256 `{G['matrix_sha256'][:16]}`.",
         "Cell: verdict, the channel nearest its bar, measured / bar (mm; emit in epochs).", "",
         "| row | " + " | ".join(postures) + " |", "|---|" + "---|" * len(postures)]
    for rid in rows:
        L.append(f"| {rid} | " + " | ".join(cell_text(cells[(rid, p)]) if (rid, p) in cells else "" for p in postures) + " |")
    n_pass = sum(1 for r in G["cells"] if r["pass"])
    L += ["", f"{n_pass} of {len(G['cells'])} cells pass.", "", "## Channels (measured / bar, mm)", "",
          "| row | posture | " + " | ".join(CH_ORDER) + " | notes |", "|---|---|" + "---|" * (len(CH_ORDER) + 1)]
    for rid in rows:
        for p in postures:
            r = cells.get((rid, p))
            if not r:
                continue
            vals = []
            for k in CH_ORDER:
                c = r["channels"].get(k)
                vals.append("" if c is None else (f"{c['E']:.1f}/{c['bar']:.1f}" + ("" if c["pass"] else " X")))
            notes = []
            if "error" in r:
                notes.append(r["error"][:60])
            if r.get("pre_max_clk_mm"):
                notes.append(f"pre-event diff {r['pre_max_clk_mm']:.3f} mm")
            if r.get("pb_regenerated"):
                notes.append(f"pb regenerated {r['pb_regenerated']}")
            if r.get("zwd_worst_station"):
                notes.append(f"zwd {r['zwd_worst_station']}")
            if r.get("unread"):
                notes.append("unread")
            if r.get("no_effect"):
                notes.append("NO EFFECT on any channel: the target may be out of the solve")
            if r.get("iono_worst_los") and not r["channels"]["iono"]["pass"]:
                notes.append(f"iono {r['iono_worst_los']}")
            L.append(f"| {rid} | {p} | " + " | ".join(vals) + " | " + "; ".join(notes) + " |")
    if a.control:
        C = json.load(open(gate_dir(a.control) + "/grades.json"))
        cc = {(r["row"], r["posture"]): r for r in C["cells"]}
        diffs = []
        for k, r in cells.items():
            o = cc.get(k)
            if o is None:
                continue
            for ch, c in r["channels"].items():
                oc = o["channels"].get(ch)
                if oc is None or oc["E"] != c["E"] or oc["S"] != c["S"]:
                    diffs.append(f"{k[0]}/{k[1]}/{ch}")
        shared = sum(1 for k in cells if k in cc)
        L += ["", f"## Control `{a.control}`", "",
              (f"IDENTICAL: {shared} shared cells, every measured value and spread equal."
               if not diffs else f"DIFFERENT in {len(diffs)} channel(s) of {shared} shared cells: "
               + ", ".join(diffs[:30]))]
    text = "\n".join(L) + "\n"
    open(gd + "/report.md", "w", encoding="utf-8").write(text)
    print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("seed", help="write (and with --launch, run) one posture's seed")
    s.add_argument("posture", choices=sorted(POSTURES))
    s.add_argument("--hours", type=float, default=6.0)
    s.add_argument("--interval-ep", type=int, default=60)
    s.add_argument("--smoke", action="store_true", help="10 epochs instead of --hours")
    s.add_argument("--exe", default=default_exe())
    s.add_argument("--launch", action="store_true")
    s.set_defaults(func=cmd_seed)
    k = sub.add_parser("make", help="generate every leg of the matrix for one exe")
    k.add_argument("--label", required=True)
    k.add_argument("--exe", required=True)
    k.add_argument("--rows", default="", help="comma list of row ids (default: every row)")
    k.add_argument("--postures", default="", help="comma list (default: the matrix's)")
    k.add_argument("--append", action="store_true", help="add to the label's existing plan")
    k.add_argument("--extra", default="",
                   help="JSON object (or @file) deep-merged into every leg of the label, event and "
                        "null alike (arms knobs the seeds predate); recorded in plan.json")
    k.set_defaults(func=cmd_make)
    r = sub.add_parser("run", help="run the label's legs not yet run")
    r.add_argument("--label", required=True)
    r.add_argument("--jobs", type=int, default=3)
    r.add_argument("--force", action="store_true", help="re-run every leg (restores each snapshot first)")
    r.set_defaults(func=cmd_run)
    g = sub.add_parser("grade", help="grade every cell, event minus null, against its bar")
    g.add_argument("--label", required=True)
    g.set_defaults(func=cmd_grade)
    p = sub.add_parser("report", help="the table per exe; --control compares a second label")
    p.add_argument("--label", required=True)
    p.add_argument("--control", default="")
    p.set_defaults(func=cmd_report)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
