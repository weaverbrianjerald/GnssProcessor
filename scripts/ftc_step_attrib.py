#!/usr/bin/env python3
"""ftc_step_attrib.py: trace-only attribution of consumer phase-bias translations.

Reads an existing closed-loop run directory and classifies every cell-epoch on
which the consumer's pb_step_translate (PBT) ledger translated the float
ambiguity, against what the applied corrections did and against the
producer's own event stream.

Measure-only: reads CSV/text traces, writes nothing back, runs no estimator.

Sign convention (from the source; --sign prints the note with line numbers
that may have drifted since):
    SsrCorrectionStore::applyClockToBias            -> (c0+...)/c  [s]
    GnssCorrections::getSatelliteClockCorrection    clock_corr += applyClockToBias(...)
    PppModel::computeBaseModeledValue               modeled = range - sat_clock*c - ...
    MeasurementModel::buildJacobianRow              kSsrPhaseBiasSign = +1,
                                                    kSsrCodeBiasSign  = +1,
                                                    predicted = modeled + ...
                                                    + ssr_bias_term (+ amb)
  => phase prediction carries  (-clk_m + pb_m + A)
     code  prediction carries  (-clk_m + cb_m)
  which is PBT-AUDIT section 4's "phase correction = -clock + PB, code
  correction = -clock + CB".

Usage:
    python scripts/ftc_step_attrib.py <run_dir> [--eps 0.005]

<run_dir> holds logs/closed_loop_sat_trace.csv, logs/network_events.csv,
logs/network_phase_anchor.csv and (optionally) products.txt.
"""

import argparse
import glob
import json
import re
import math
import os
import sys
from collections import Counter, defaultdict

import numpy as np
import pandas as pd

# --------------------------------------------------------------------------
# Producer event tier vocabulary.  Tiers that are pure per-epoch telemetry
# (thousands of rows, one per screened cell) would make "some event coincided"
# vacuously true, so they are held out of attribution and reported separately.
# --------------------------------------------------------------------------
TIER_STRANSFORM = ("STRANSFORM",)
TIER_PHASE_ANCHOR = ("PHASE_ANCHOR",)
TIER_PBGAUGE = ("PBGAUGE",)
TIER_CLK_ANCHOR = ("CLK_ANCHOR",)
TIER_READMIT = ("READMIT",)
TIER_SLIP = ("T0_gap", "T0_mw_detect", "T0_band_scope",
             "T1_if_slip", "T2_within_epoch", "T4_attrib")
TIER_OTHER = ("CB_DATUM", "CLK_BASIS", "PIVOT", "RETRO", "GATE",
              "ADMISSION", "TOPOLOGY", "REDUNDANCY", "DATUMGRAPH")
TIER_TELEMETRY = ("MDB", "PRIORRES", "CONGRUENCE", "PREFIT", "AMB_LEVEL")

# Single-label priority, in the order the measurement request lists them.
PRIORITY = ["F-STRANSFORM", "F-PHASE_ANCHOR", "F-PBGAUGE", "F-CLK_ANCHOR",
            "F-READMIT", "F-SLIP", "F-ANCHORCHANGE", "F-other", "F-none"]

SIGN_NOTE = """\
SIGN CONVENTION (verified from source)
  SsrCorrectionStore.cpp:666   applyClockToBias(c0,..) = (c0 + c1*dt + ..)/c  [seconds]
  GnssCorrections.cpp:344      clock_corr += applyClockToBias(ssr->c0, ..)
  PppModel.cpp:1490            modeled = geom.range - (geom.sat_clock * c) - ...
  MeasurementModel.cpp:1610    kSsrCodeBiasSign = +1.0 ; kSsrPhaseBiasSign = +1.0
  MeasurementModel.cpp:1641    predicted = modeled + ... + ssr_bias_term  (+ amb_term, phase)
  => phase prediction contribution  = -clk_m + pb_m + A
     code  prediction contribution  = -clk_m + cb_m
  Matches PBT-AUDIT section 4 ("phase correction = -clock + PB, code = -clock + CB").
  Trace columns clk_m / cb_m / pb_m are the APPLIED values, harvested with the
  model's own staleness gating (RealTimeEngineSsrDiag.cpp:233-283).
"""


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def q(x, p):
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    return float(np.quantile(x, p)) if x.size else float("nan")


def fmt(v, n=4):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "-"
    return f"{v:.{n}f}"


def hdr(title):
    return "\n" + "=" * 78 + "\n" + title + "\n" + "=" * 78


def sub(title):
    return "\n--- " + title + " " + "-" * max(0, 72 - len(title))


# --------------------------------------------------------------------------
# loaders
# --------------------------------------------------------------------------
def load_trace(run_dir, eps):
    p = os.path.join(run_dir, "logs", "closed_loop_sat_trace.csv")
    d = pd.read_csv(p)
    d = d.sort_values(["sat", "sig", "tow"], kind="mergesort").reset_index(drop=True)
    g = d.groupby(["sat", "sig"], sort=False)
    d["prev_tow"] = g["tow"].shift(1)
    d["d_pb"] = g["pb_m"].diff()
    d["d_clk"] = g["clk_m"].diff()
    d["d_cb"] = g["cb_m"].diff()
    d["prev_pb"] = g["pb_m"].shift(1)
    d["prev_amb"] = g["amb_m"].shift(1)
    d["gap_s"] = d["tow"] - d["prev_tow"]

    n_first = int(d["prev_tow"].isna().sum())
    n_gap = int(((d["gap_s"].notna()) & (d["gap_s"] != 30.0)).sum())
    d["pair30"] = d["gap_s"] == 30.0

    # ---- common-mode removal ------------------------------------------------
    # clock is a per-satellite quantity repeated on the sat's signal rows, so the
    # per-epoch median is taken over distinct satellites tracked at both epochs.
    pr = d[d["pair30"]]
    sat_clk = pr.drop_duplicates(subset=["tow", "sat"])[["tow", "sat", "d_clk"]]
    c_clk = sat_clk.groupby("tow")["d_clk"].median().rename("c_clk")
    n_clk = sat_clk.groupby("tow")["d_clk"].size().rename("n_clk_sats")
    c_pb = pr.groupby(["tow", "sig"])["d_pb"].median().rename("c_pb")
    n_pb = pr.groupby(["tow", "sig"])["d_pb"].size().rename("n_pb_sats")
    c_cb = pr.groupby(["tow", "code_sig"])["d_cb"].median().rename("c_cb")

    d = d.merge(c_clk, left_on="tow", right_index=True, how="left")
    d = d.merge(n_clk, left_on="tow", right_index=True, how="left")
    d = d.merge(c_pb, left_on=["tow", "sig"], right_index=True, how="left")
    d = d.merge(n_pb, left_on=["tow", "sig"], right_index=True, how="left")
    d = d.merge(c_cb, left_on=["tow", "code_sig"], right_index=True, how="left")

    d["d_clk_dc"] = d["d_clk"] - d["c_clk"]
    d["d_pb_dc"] = d["d_pb"] - d["c_pb"]
    d["d_cb_dc"] = d["d_cb"] - d["c_cb"]
    d["d_phi"] = -d["d_clk_dc"] + d["d_pb_dc"]
    d["d_rho"] = -d["d_clk_dc"] + d["d_cb_dc"]
    # robustness variant: remove the common mode of the whole phase correction
    # (-d_clk + d_pb) with a single per-signal median
    d["d_phi_raw"] = -d["d_clk"] + d["d_pb"]
    c_phi = pr.assign(v=(-pr["d_clk"] + pr["d_pb"])).groupby(
        ["tow", "sig"])["v"].median().rename("c_phi")
    d = d.merge(c_phi, left_on=["tow", "sig"], right_index=True, how="left")
    d["d_phi_alt"] = d["d_phi_raw"] - d["c_phi"]

    tow0 = float(d["tow"].min())
    d["hour"] = ((d["tow"] - tow0) // 3600.0).astype(int) + 1

    # run length: how many consecutive 30 s epochs this cell has been translating
    # (a deterministic frame move is isolated; ordinary drift past the dead band
    # produces runs).
    d["translated"] = d["ev_pb"].astype(str).str.startswith("translate")
    runlen = np.zeros(len(d), dtype=int)
    prev_key, prev_tow_v, cur = None, None, 0
    for i, (k, tw, tv) in enumerate(zip(zip(d["sat"], d["sig"]), d["tow"], d["translated"])):
        if k != prev_key or prev_tow_v is None or tw != prev_tow_v + 30.0:
            cur = 0
        cur = cur + 1 if tv else 0
        runlen[i] = cur
        prev_key, prev_tow_v = k, tw
    d["run_len"] = runlen
    g2 = d.groupby(["sat", "sig"], sort=False)
    prev_tr = g2["translated"].shift(1).fillna(False).astype(bool)
    next_tr = g2["translated"].shift(-1).fillna(False).astype(bool)
    prev_ok = g2["tow"].shift(1) == d["tow"] - 30.0
    next_ok = g2["tow"].shift(-1) == d["tow"] + 30.0
    d["nbr_tr"] = (prev_tr & prev_ok) | (next_tr & next_ok)
    d["isolated"] = d["translated"] & ~d["nbr_tr"]
    d["tr_kind"] = np.where(d["ev_pb"].astype(str) == "translate_disc", "declared",
                            np.where(d["ev_pb"].astype(str) == "translate_undeclared",
                                     "undeclared", ""))
    return d, dict(n_first=n_first, n_gap=n_gap, tow0=tow0,
                   n_epochs=int(d["tow"].nunique()), n_rows=len(d))


def load_events(run_dir):
    p = os.path.join(run_dir, "logs", "network_events.csv")
    e = pd.read_csv(p, dtype=str)
    e["tow"] = e["tow"].astype(float).map(snap)
    satf = e["sat"].fillna("")
    e["sat_base"] = satf.str.split("_").str[0]
    e["sat_sig"] = satf.str.split("_").str[1].fillna("")
    e["station"] = e["station"].fillna("")
    return e


def load_anchor(run_dir):
    p = os.path.join(run_dir, "logs", "network_phase_anchor.csv")
    a = pd.read_csv(p)
    return {(r.sat, r.signal, snap(r.tow)): r.anchor_sta
            for r in a.itertuples(index=False)}


def load_wire(run_dir):
    """Parse products.txt P records -> {(sat, signal, tow): bias_m}."""
    p = os.path.join(run_dir, "products.txt")
    if not os.path.exists(p):
        return {}
    wire = {}
    tow = None
    with open(p, "r", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            if line[0] == "E":
                parts = line.rstrip("\n").split(",")
                if len(parts) > 2:
                    try:
                        tow = round(float(parts[2]), 3)
                    except ValueError:
                        tow = None
            elif line[0] == "P" and tow is not None:
                parts = line.rstrip("\n").split(",")
                if len(parts) > 3:
                    try:
                        wire[(parts[1], parts[2], tow)] = float(parts[3])
                    except ValueError:
                        pass
    return wire


def snap(t):
    """Epoch tow to a canonical key.  products.txt E records carry float noise
    (259229.99999809265) while the CSV traces carry 259230.000."""
    return round(float(t), 3)


# --------------------------------------------------------------------------
# event index
# --------------------------------------------------------------------------
class EventIndex:
    def __init__(self, e):
        self.sat_tow = defaultdict(set)       # (sat, tow) -> {tier}
        self.satsig_tow = defaultdict(set)    # (sat, sig, tow) -> {tier}
        self.slip_sta = defaultdict(set)      # (sat, station, tow) -> {slip tier}
        self.sta_tier = defaultdict(set)      # (sat, station, tow) -> {any tier}
        for r in e.itertuples(index=False):
            key = (r.sat_base, r.tow)
            self.sat_tow[key].add(r.tier)
            if r.sat_sig:
                self.satsig_tow[(r.sat_base, r.sat_sig, r.tow)].add(r.tier)
            if r.station:
                self.sta_tier[(r.sat_base, r.station, r.tow)].add(r.tier)
                if r.tier in TIER_SLIP:
                    self.slip_sta[(r.sat_base, r.station, r.tow)].add(r.tier)

    def has(self, sat, tow, tiers):
        return bool(self.sat_tow.get((sat, tow), frozenset()) & set(tiers))

    def has_sig(self, sat, sig, tow, tiers):
        return bool(self.satsig_tow.get((sat, sig, tow), frozenset()) & set(tiers))

    def slip_on(self, sat, stations, tows):
        for st in stations:
            if not st:
                continue
            for t in tows:
                if self.slip_sta.get((sat, st, t)):
                    return True
        return False


def label_row(r, idx, anchor, multi=False):
    """Return the priority label (and, with multi=True, the full label set)."""
    sat, sig, tow = r.sat, r.sig, snap(r.tow)
    prev_tow = snap(tow - 30.0)
    a_now = anchor.get((sat, sig, tow))
    a_prev = anchor.get((sat, sig, prev_tow))
    labs = set()
    if idx.has(sat, tow, TIER_STRANSFORM):
        labs.add("F-STRANSFORM")
    if idx.has(sat, tow, TIER_PHASE_ANCHOR):
        labs.add("F-PHASE_ANCHOR")
    if idx.has(sat, tow, TIER_PBGAUGE):
        labs.add("F-PBGAUGE")
    if idx.has(sat, tow, TIER_CLK_ANCHOR):
        labs.add("F-CLK_ANCHOR")
    if idx.has(sat, tow, TIER_READMIT):
        labs.add("F-READMIT")
    if idx.slip_on(sat, (a_now, a_prev), (tow, prev_tow)):
        labs.add("F-SLIP")
    if (a_now is not None and a_prev is not None and a_now != a_prev
            and "F-STRANSFORM" not in labs):
        labs.add("F-ANCHORCHANGE")
    if idx.has(sat, tow, TIER_OTHER):
        labs.add("F-other")
    lab = "F-none"
    for cand in PRIORITY:
        if cand in labs:
            lab = cand
            break
    return (lab, labs) if multi else lab


# --------------------------------------------------------------------------
# Q1 classification
# --------------------------------------------------------------------------
def classify(d, eps):
    """C / R / F partition over translated cell-epochs (|d_pb| >= eps by ledger)."""
    cls = np.full(len(d), "", dtype=object)
    common = d["d_pb_dc"].abs() < eps
    flat = d["d_phi"].abs() < eps
    cls[common.values] = "C"
    cls[(~common & flat).values] = "R"
    cls[(~common & ~flat).values] = "F"
    return pd.Series(cls, index=d.index)


def class_table(t, eps, title, out):
    out.append(sub(title))
    out.append(f"{'class':<16} {'n':>6} {'sum|d_pb|':>10} {'sum|d_pb_dc|':>12} "
               f"{'p50':>8} {'p90':>8} {'max':>9}   (|d_pb_dc|, m)")
    order = ["C", "R"] + [x for x in PRIORITY if (t["cls"] == "F").any()]
    seen = []
    for c in order:
        if c in ("C", "R"):
            s = t[t["cls"] == c]
        else:
            s = t[(t["cls"] == "F") & (t["flabel"] == c)]
        if c in seen:
            continue
        seen.append(c)
        if len(s) == 0:
            continue
        out.append(f"{c:<16} {len(s):>6} {s['d_pb'].abs().sum():>10.3f} "
                   f"{s['d_pb_dc'].abs().sum():>12.3f} "
                   f"{q(s['d_pb_dc'].abs(),.5):>8.4f} {q(s['d_pb_dc'].abs(),.9):>8.4f} "
                   f"{s['d_pb_dc'].abs().max():>9.4f}")
    f_all = t[t["cls"] == "F"]
    out.append(f"{'F (all)':<16} {len(f_all):>6} {f_all['d_pb'].abs().sum():>10.3f} "
               f"{f_all['d_pb_dc'].abs().sum():>12.3f} "
               f"{q(f_all['d_pb_dc'].abs(),.5):>8.4f} {q(f_all['d_pb_dc'].abs(),.9):>8.4f} "
               f"{f_all['d_pb_dc'].abs().max() if len(f_all) else float('nan'):>9.4f}")
    out.append(f"{'TOTAL':<16} {len(t):>6} {t['d_pb'].abs().sum():>10.3f} "
               f"{t['d_pb_dc'].abs().sum():>12.3f}")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
RAW_NOTE = """\
WHERE A RAW (PRE-FILTER) PHASE BIAS WOULD LIVE -- candidate audit
  The emit path writes the emitted phase bias `npb.bias_m` in exactly three places
  (grep `bias_m =` over EmitStore.cpp / SolPrep.cpp):
    EmitStore.cpp:243   reads npb.bias_m into the re-base entry (not a write)
    EmitStore.cpp:256   emit RE-BASE write-back      -- inside `if (emit_rebase_on)`
    EmitStore.cpp:295   PB emit FILTER write-back    -- inside `if (emit_pb_filter_on)`
  Every other phase-bias site on the emit path writes `sigma_m` only
  (SolPrep.cpp:538/567/592/1308 -- emitted-sigma, SD-sigma, combined sigc).

  ApplicationNetwork.cpp:919  emit_rebase_on   = carry_invariant && carry_invariant_emit_rebase
  ApplicationNetwork.cpp:927  emit_pb_filter_on= emit_rebase_on  && emit_pb_filter

  Candidate on-disk sources, each resolved against those line numbers:
    products.txt P records        POST-filter by construction (the wire).
    network_solution.csv kind=PB  POST-filter: written at EmitStore.cpp:871-873,
                                  i.e. AFTER the :295 write-back, from the same `sol`.
    network_sat_bias_states.csv   CODE bias only -- SolPrep.cpp:1367 iterates
                                  `s.code_bias_m`; there is no phase row in the file.
    network_amb_state.csv         per (station, satellite, signal) CARRIED ambiguity
                                  (value_abs_m), not the satellite phase bias.
    network_rx_states.csv         receiver states only (kind in {CLK, CB}).
    NetworkPbEmitFilter           exposes Counters + a sigma_step telemetry seam only
                                  (include/products/NetworkPbEmitFilter.hpp:216-251,314);
                                  it opens NO CSV stream.
  => No pre-filter phase bias is written to disk by any armed trace.
"""


def raw_mode(run, out):
    """Locate a raw pre-filter PB in the run, or show that none is written (H-FILT read)."""
    out.append(hdr(f"H-FILT raw pre-filter PB read -- {os.path.basename(run)}"))
    out.append(f"run_dir : {run}")
    out.append("")
    out.append(RAW_NOTE)

    out.append(hdr("1. WAS THE PB EMIT FILTER ACTUALLY RUNNING?"))
    out.append("The filter is double-gated.  ApplicationNetwork.cpp:919/927:")
    out.append("    emit_rebase_on    = carry_invariant && carry_invariant_emit_rebase")
    out.append("    emit_pb_filter_on = emit_rebase_on  && emit_pb_filter")
    out.append("Four INDEPENDENT checks, read out of this run directory:")
    out.append("")

    # (i) config echo
    cfg = {}
    for src, path in (("arm.json", os.path.join(run, "arm.json")),
                      ("manifest", os.path.join(run, "logs", "network_run_manifest.json"))):
        if not os.path.exists(path):
            continue
        blobs = []
        with open(path, "r", errors="replace") as f:
            txt = f.read()
        for chunk in txt.splitlines():
            chunk = chunk.strip()
            if not chunk:
                continue
            try:
                blobs.append(json.loads(chunk))
            except Exception:
                pass
        if not blobs:
            try:
                blobs = [json.loads(txt)]
            except Exception:
                blobs = []
        for b in blobs:
            n = b.get("network", {}) if isinstance(b, dict) else {}
            for k in ("carry_invariant", "carry_invariant_emit_rebase", "emit_pb_filter",
                      "emit_pb_filter_emit_sigma", "emit_pb_filter_window_epochs",
                      "emit_pb_filter_q", "emit_pb_filter_meas_sigma_m",
                      "emit_pb_filter_jump_k", "emit_pb_filter_gate_v2",
                      "ambiguity_vehicle"):
                if k in n and (src, k) not in cfg:
                    cfg[(src, k)] = n[k]
    out.append("(i) config echo (arm.json and the run manifest):")
    for (src, k), v in sorted(cfg.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        out.append(f"      {src:<10} network.{k:<32} = {v}")
    ci = cfg.get(("manifest", "carry_invariant"), cfg.get(("arm.json", "carry_invariant")))
    pf = cfg.get(("manifest", "emit_pb_filter"), cfg.get(("arm.json", "emit_pb_filter")))
    derived = bool(ci) and bool(pf)
    out.append(f"      => emit_pb_filter_on = {derived}")

    # (ii) run-end telemetry lines (unconditional inside their if-blocks)
    ml = os.path.join(run, "logs", "main_log.log")
    counts = {}
    if os.path.exists(ml):
        with open(ml, "r", errors="replace") as f:
            body = f.read()
        for tag in ("emit re-base", "continuity filter", "GATE v2"):
            counts[tag] = body.count(tag)
    out.append("")
    out.append("(ii) run-end telemetry (Teardown.cpp:5002 / :5021 / :5035 -- each is")
    out.append("     UNCONDITIONAL inside `if (emit_rebase_on)` / `if (emit_pb_filter_on)`):")
    for tag, n in counts.items():
        out.append(f"      lines matching {tag!r:<22} in main_log.log : {n}")

    # (iii) the emitted-sigma stamp
    pth = os.path.join(run, "products.txt")
    nf = Counter()
    nsig = 0
    if os.path.exists(pth):
        with open(pth, "r", errors="replace") as f:
            for line in f:
                if line[:2] == "P,":
                    parts = line.rstrip("\n").split(",")
                    nf[len(parts)] += 1
                    if any(p.startswith("sigma=") for p in parts):
                        nsig += 1
    out.append("")
    out.append("(iii) the filter's posterior sigma stamp.  emit_pb_filter_emit_sigma is armed,")
    out.append("      so an ACTIVE filter stamps `sigma=<m>` onto every P record")
    out.append("      (EmitStore.cpp:296-297).  Observed P-record field counts:")
    out.append(f"      {dict(nf)}   records carrying a sigma= token: {nsig}")

    # (iv) the loader's own inertness warning
    gc = os.path.join(run, "logs", "gnss_config.log")
    warn = []
    if os.path.exists(gc):
        with open(gc, "r", errors="replace") as f:
            for line in f:
                if "emit_pb_filter" in line and "INERT" in line:
                    warn.append(line.strip())
    out.append("")
    out.append("(iv) the loader's own warn-INERT line "
               "(GnssConfigLoadNetwork.cpp:9869), in logs/gnss_config.log:")
    for w in warn[:2]:
        out.append(f"      {w[:150]}")
    if not warn:
        out.append("      (none found)")

    out.append(hdr("2. VERDICT"))
    active = derived and counts.get("continuity filter", 0) > 0
    out.append(f"PB emit filter ACTIVE on this run: {active}")
    out.append("")
    if not active:
        out.append("H-FILT cannot be tested on this run, and is FALSIFIED AT ITS PREMISE:")
        out.append("the smoothing mechanism it names was NOT RUNNING.  `network.emit_pb_filter:")
        out.append("true` is inert here because `network.carry_invariant` is false -- the")
        out.append("RF-0hv posture runs ambiguity_vehicle=float_kf, and the filter only exists")
        out.append("inside the invariant carry's re-based emission frame.")
        out.append("")
        out.append("CONSEQUENCE for the FTC read: with both :256 and :295 gated off, NOTHING")
        out.append("writes npb.bias_m between the solve and products.txt.  The wire phase bias")
        out.append("IS the estimator's own per-epoch emitted value -- unsmoothed and un-re-based.")
        out.append("So the drift-shaped F-none runs and the F-CLK_ANCHOR steps measured in the")
        out.append("main read are the ESTIMATOR's behaviour, not an emit-filter artifact.")
        out.append("Measurements (a)-(f) are not run: there is no raw/wire pair to compare.")
        out.append("")
        out.append("TO ACTUALLY TEST H-FILT one of the following is needed:")
        out.append("  - a run with network.carry_invariant armed (so the filter is live), or")
        out.append("  - a new trace stream emitting Entry.value_m BEFORE EmitStore.cpp:295")
        out.append("    (a C++ change, out of scope for a trace-only read).")

    out.append(hdr("3. WHAT AN ARMED FILTER WOULD DO (from source + this config)"))
    out.append("Reference numbers for whoever re-runs this with carry_invariant armed.")
    out.append("The filter is a per-key scalar random-walk KF, NOT a 15-epoch boxcar:")
    out.append("  window_epochs is the exponential-forgetting HALF-LIFE of the sigma_step")
    out.append("  ESTIMATOR only (NetworkPbEmitFilter.hpp:201), not the smoothing length.")
    try:
        qv = float(cfg.get(("manifest", "emit_pb_filter_q"), 3.0e-7))
        rs = float(cfg.get(("manifest", "emit_pb_filter_meas_sigma_m"), 0.05))
        jk = float(cfg.get(("manifest", "emit_pb_filter_jump_k"), 4.0))
        dt_ = 30.0
        Q = qv * dt_
        R = rs * rs
        S = 0.5 * (Q + math.sqrt(Q * Q + 4.0 * Q * R))     # steady-state P_pred
        K = S / (S + R)
        tau = 1.0 / (-math.log(1.0 - K)) if 0 < K < 1 else float("nan")
        out.append(f"  q = {qv:g} m^2/s, dt = 30 s  -> Q = {Q:.3e} m^2 "
                   f"(per-epoch drift sigma {math.sqrt(Q)*1000:.1f} mm)")
        out.append(f"  R = meas_sigma^2 = {R:.3e} m^2  (meas_sigma = {rs} m)")
        out.append(f"  steady-state gain K = {K:.4f}  -> the wire closes {100*K:.1f} % of the")
        out.append(f"     raw-vs-wire offset per epoch; 1/e time constant = {tau:.1f} epochs "
                   f"({tau*30:.0f} s)")
        out.append(f"  gate_v2 threshold = k*sqrt(Ppred+R) = {jk:g} * {math.sqrt(S+R):.4f} "
                   f"= {jk*math.sqrt(S+R):.4f} m")
        out.append(f"     => any raw step above {jk*math.sqrt(S+R):.3f} m is PASSED THROUGH RAW")
        out.append("        (Cause::kInnovation reset), so the filter can only manufacture a")
        out.append(f"        ramp out of steps BELOW that, and its lag is self-limiting at "
                   f"~{jk*math.sqrt(S+R):.2f} m.")
        out.append("  PREDICTED SIGNATURE on an armed run: within a smoothed run the wire's")
        out.append(f"     per-epoch step should decay geometrically by {1.0-K:.4f} per epoch.")
        out.append("     That ratio, measured on the wire alone, is a sufficient test -- it")
        out.append("     does not need the raw series.")
    except Exception as ex:                                  # pragma: no cover
        out.append(f"  (could not derive: {ex})")
    return 0


# --------------------------------------------------------------------------
# R1 / R2 / R3: whether the wire's PB movement harms a float consumer
# --------------------------------------------------------------------------
F1_HZ = 1575.42e6
F2_HZ = 1227.60e6
IF_A = F1_HZ ** 2 / (F1_HZ ** 2 - F2_HZ ** 2)          # 2.545728
IF_B = F2_HZ ** 2 / (F1_HZ ** 2 - F2_HZ ** 2)          # 1.545728
MU21 = F1_HZ ** 2 / F2_HZ ** 2                          # 1.646944  (exact IF null)
C_MS = 299792458.0
LAM1 = C_MS / F1_HZ                                     # 0.190293 m
LAM2 = C_MS / F2_HZ                                     # 0.244210 m
CYC_R = LAM2 / LAM1                                     # 1.283333 = f1/f2
LAM_NL = C_MS / (F1_HZ + F2_HZ)                         # 0.106953 m
# An equal-cycle step (N cycles on both bands) has d2/d1 = lam2/lam1 and leaks
# exactly N * lam_NL into the IF combination:  a*lam1 - b*lam2 == lam_NL.

# The R3 grade per constellation.  The graded pair is the constellation's dual
# pair (GPS L1 / L2, Galileo E1 / E5a) with its own ionosphere-free weights; the
# common mode is removed per (epoch, constellation), because each constellation's
# clock common mode is its own gauge.  Values: (band digit of the second signal,
# f1, f2).  (card GAL)
R3_PAIRS = {"G": (2, F1_HZ, F2_HZ), "E": (5, 1575.42e6, 1176.45e6)}


def band_of(sig):
    """RINEX phase obs code -> band digit ('L1W'->1, 'L2W'->2)."""
    return int(sig[1]) if len(sig) >= 2 and sig[1].isdigit() else 0


def build_pivot(d, eps):
    """(sat, tow) table carrying both bands' 30 s steps, plus IF / GF."""
    d = d.copy()
    d["band"] = d["sig"].map(band_of)
    p = d[d["pair30"] & d["band"].isin((1, 2))]
    keep = ["tow", "sat", "hour", "sig", "d_pb", "d_pb_dc", "d_clk", "d_clk_dc",
            "translated", "cls", "flabel", "run_len", "isolated"]
    keep = [c for c in keep if c in p.columns]
    b1 = p[p["band"] == 1][keep].add_suffix("_1")
    b2 = p[p["band"] == 2][keep].add_suffix("_2")
    m = b1.merge(b2, left_on=["tow_1", "sat_1"], right_on=["tow_2", "sat_2"],
                 how="inner")
    m = m.rename(columns={"tow_1": "tow", "sat_1": "sat", "hour_1": "hour"})
    m["d_IF_raw"] = IF_A * m["d_pb_1"] - IF_B * m["d_pb_2"]
    m["d_GF_raw"] = m["d_pb_1"] - m["d_pb_2"]
    # de-common each by the per-epoch median over the satellites tracked at both
    # epochs (the receiver-clock direction).
    for c in ("d_IF", "d_GF"):
        med = m.groupby("tow")[c + "_raw"].median().rename(c + "_med")
        m = m.merge(med, left_on="tow", right_index=True, how="left")
        m[c] = m[c + "_raw"] - m[c + "_med"]
    # band-first variant (remove each band's common mode first, then combine),
    # for robustness
    m["d_IF_bf"] = IF_A * m["d_pb_dc_1"] - IF_B * m["d_pb_dc_2"]
    m["d_GF_bf"] = m["d_pb_dc_1"] - m["d_pb_dc_2"]
    m["tr_any"] = m["translated_1"] | m["translated_2"]
    return m


def shape_of(row, eps, dc=False):
    d1 = row.d_pb_dc_1 if dc else row.d_pb_1
    d2 = row.d_pb_dc_2 if dc else row.d_pb_2
    s1, s2 = abs(d1) >= eps, abs(d2) >= eps
    if s1 != s2:
        return "single-band"
    if not s1 and not s2:
        return "sub-eps"
    r = d2 / d1 if d1 != 0.0 else float("inf")
    if abs(r - MU21) <= 0.25:
        return "iono-like"
    if abs(r - 1.0) <= 0.15:
        return "range-like"
    if abs(r - CYC_R) <= 0.10:
        return "cycle-like"
    return "other"


def r1_section(m, eps, out, bed):
    out.append(hdr(f"R1  IF / GF DECOMPOSITION OF THE WIRE'S PB MOVEMENT -- {bed}"))
    out.append(f"bands confirmed from the sig labels: "
               f"{sorted(set(m['sig_1']))} (band 1) / {sorted(set(m['sig_2']))} (band 2)")
    out.append(f"a = f1^2/(f1^2-f2^2) = {IF_A:.6f}   b = f2^2/(f1^2-f2^2) = {IF_B:.6f}   "
               f"a-b = {IF_A-IF_B:.6f}")
    out.append(f"mu = (f1/f2)^2 = {MU21:.6f}; an iono-like pair (d2 = mu*d1) gives "
               f"d_IF = d1*(a - b*mu) = {IF_A - IF_B*MU21:.2e}  (exact null)")
    out.append(f"dual-band satellite-epochs with a 30 s pair: {len(m)}")
    out.append("")
    out.append(f"{'window':<12} {'n':>6} {'sum|d_IF|':>11} {'p50':>8} {'p90':>8} {'max':>9} "
               f"{'sum|d_GF|':>11} {'p50':>8} {'p90':>8} {'max':>9}")
    for nm, s in [("ALL hours", m), ("hours 2-6", m[m["hour"] >= 2]), ("hour 1", m[m["hour"] == 1])]:
        out.append(f"{nm:<12} {len(s):>6} {s['d_IF'].abs().sum():>11.3f} "
                   f"{q(s['d_IF'].abs(),.5):>8.4f} {q(s['d_IF'].abs(),.9):>8.4f} "
                   f"{s['d_IF'].abs().max():>9.4f} {s['d_GF'].abs().sum():>11.3f} "
                   f"{q(s['d_GF'].abs(),.5):>8.4f} {q(s['d_GF'].abs(),.9):>8.4f} "
                   f"{s['d_GF'].abs().max():>9.4f}")
    out.append("   robustness (de-common each BAND first, then combine):")
    for nm, s in [("ALL hours", m), ("hours 2-6", m[m["hour"] >= 2])]:
        out.append(f"      {nm:<12} sum|d_IF_bf| = {s['d_IF_bf'].abs().sum():>10.3f}  "
                   f"sum|d_GF_bf| = {s['d_GF_bf'].abs().sum():>10.3f}")

    for lab, sel in [("ALL translated sat-epochs", m[m["tr_any"]]),
                     ("hours 2-6, translated", m[m["tr_any"] & (m["hour"] >= 2)]),
                     ("class F-none (either band)",
                      m[m["tr_any"] & ((m["flabel_1"] == "F-none") | (m["flabel_2"] == "F-none"))]),
                     ("class F-CLK_ANCHOR (either band)",
                      m[m["tr_any"] & ((m["flabel_1"] == "F-CLK_ANCHOR")
                                       | (m["flabel_2"] == "F-CLK_ANCHOR"))])]:
        out.append(sub(f"SHAPE SPLIT -- {lab}  (n={len(sel)})"))
        if len(sel) == 0:
            out.append("   (empty)")
            continue
        sh = pd.Series([shape_of(r, eps) for r in sel.itertuples(index=False)],
                       index=sel.index)
        out.append(f"{'shape':<14} {'n':>6} {'share':>8} {'sum|d_IF|':>11} "
                   f"{'IF share':>9} {'p50|d_IF|':>10} {'sum|d_pb1|+|d_pb2|':>19}")
        tot_if = sel["d_IF"].abs().sum()
        for nm in ("iono-like", "range-like", "cycle-like", "single-band", "other", "sub-eps"):
            s = sel[sh == nm]
            if len(s) == 0:
                continue
            out.append(f"{nm:<14} {len(s):>6} {100.0*len(s)/len(sel):>7.1f}% "
                       f"{s['d_IF'].abs().sum():>11.3f} "
                       f"{100.0*s['d_IF'].abs().sum()/max(tot_if,1e-12):>8.1f}% "
                       f"{q(s['d_IF'].abs(),.5):>10.4f} "
                       f"{(s['d_pb_1'].abs()+s['d_pb_2'].abs()).sum():>19.3f}")
        out.append(f"{'TOTAL':<14} {len(sel):>6} {100.0:>7.1f}% {tot_if:>11.3f}")
        shd = pd.Series([shape_of(r, eps, dc=True) for r in sel.itertuples(index=False)],
                        index=sel.index)
        out.append("   same split on DE-COMMONED band steps: "
                   + ", ".join(f"{k} {v}" for k, v in shd.value_counts().items()))


def r1b_section(m, eps, out, bed):
    """The harm-relevant quantity under the stated theory, from the wire alone.

    Theory: a wire phase-correction error is absorbed by (i) the receiver clock
    when common to all satellites, and (ii) the float ambiguity when constant
    over an arc.  What reaches position is therefore the IF correction with the
    per-epoch common mode and the per-arc mean removed.  Cumulating the
    common-mode-removed per-epoch IF steps inside an arc (arcs break at gaps
    over 120 s) and removing the arc mean gives that quantity, with no truth
    needed.
    """
    out.append(sub("R1b  WITHIN-ARC IF EXCURSION (de-commoned, de-meaned per arc)"))
    rows = []
    for sat, g in m.sort_values(["sat", "tow"], kind="mergesort").groupby("sat"):
        tw = g["tow"].to_numpy(float)
        dv = g["d_IF"].to_numpy(float)
        hr = g["hour"].to_numpy(int)
        arc = 0
        cum = 0.0
        cur_t, cur_v, cur_h = [], [], []
        for i in range(len(tw)):
            if i > 0 and tw[i] - tw[i - 1] > 120.0:
                if cur_v:
                    rows.append((sat, arc, np.array(cur_v), np.array(cur_h)))
                arc += 1
                cum = 0.0
                cur_t, cur_v, cur_h = [], [], []
            cum += dv[i]
            cur_t.append(tw[i]); cur_v.append(cum); cur_h.append(hr[i])
        if cur_v:
            rows.append((sat, arc, np.array(cur_v), np.array(cur_h)))
    out.append(f"{'window':<12} {'arcs':>6} {'p50 len':>8} {'p50 RMS':>10} {'p90 RMS':>10} "
               f"{'max RMS':>10} {'p50 p2p':>9} {'max p2p':>9}  (m, IF level)")
    for nm, hmin in [("ALL hours", 1), ("hours 2-6", 2)]:
        rm, pp, ln = [], [], []
        for sat, arc, v, h in rows:
            sel = v[h >= hmin]
            if len(sel) < 3:
                continue
            c = sel - sel.mean()
            rm.append(float(np.sqrt((c ** 2).mean())))
            pp.append(float(c.max() - c.min()))
            ln.append(len(sel))
        if not rm:
            continue
        out.append(f"{nm:<12} {len(rm):>6} {q(ln,.5):>8.0f} {q(rm,.5):>10.4f} "
                   f"{q(rm,.9):>10.4f} {max(rm):>10.4f} {q(pp,.5):>9.4f} "
                   f"{max(pp):>9.4f}")
    out.append("   RMS  = within-arc RMS of the cumulated de-commoned IF correction,")
    out.append("          after removing the arc mean -- i.e. what neither the receiver")
    out.append("          clock nor the float ambiguity can absorb.")
    out.append("   p2p  = the same series' peak-to-peak inside the arc.")
    out.append("   ARTIFACT: this measures the wire's MOVEMENT, not its error against")
    out.append("   truth -- movement TOWARD truth would be beneficial, and this cannot")
    out.append("   tell the two apart.  Cumulating also accumulates the de-commoning")
    out.append("   residual (median-based, so it is not exactly zero-mean).")

    # ---- the equal-cycle signature -------------------------------------
    sel = m[m["tr_any"]]
    sh = pd.Series([shape_of(r, eps) for r in sel.itertuples(index=False)], index=sel.index)
    cyc = sel[sh == "cycle-like"]
    out.append(sub("EQUAL-CYCLE signature (d2/d1 = lam2/lam1 = f1/f2)"))
    out.append(f"   lam1 = {LAM1:.6f} m, lam2 = {LAM2:.6f} m, lam2/lam1 = {CYC_R:.6f}")
    out.append(f"   a*lam1 - b*lam2 = {IF_A*LAM1 - IF_B*LAM2:.6f} m = lam_NL "
               f"({LAM_NL:.6f} m)  -> an N-cycle EQUAL step on both bands leaks")
    out.append(f"   exactly N * {LAM_NL*100:.2f} cm into the IF correction.")
    out.append(f"   cycle-like translated sat-epochs: {len(cyc)} of {len(sel)} "
               f"({100.0*len(cyc)/max(len(sel),1):.1f} %), "
               f"sum|d_IF| = {cyc['d_IF'].abs().sum():.3f} m "
               f"({100.0*cyc['d_IF'].abs().sum()/max(sel['d_IF'].abs().sum(),1e-12):.1f} % "
               f"of the translated IF mass)")
    if len(cyc):
        n1 = (cyc["d_pb_1"] / LAM1)
        frac = (n1 - n1.round()).abs()
        out.append(f"   band-1 step in CYCLES: p50 |N1| = {q(n1.abs(),.5):.3f}, "
                   f"p90 = {q(n1.abs(),.9):.3f}, max = {n1.abs().max():.3f}")
        out.append(f"   distance to the nearest INTEGER cycle: p50 = {q(frac,.5):.3f}, "
                   f"p90 = {q(frac,.9):.3f}; share within 0.1 cyc = "
                   f"{100.0*float((frac<0.1).mean()):.1f} % "
                   f"(a uniform null gives 20 %)")
        out.append(f"   implied IF leak N*lam_NL vs measured |d_IF|: p50 ratio = "
                   f"{q((cyc['d_IF'].abs()/(n1.abs()*LAM_NL).replace(0,np.nan)).dropna(),.5):.3f} "
                   f"(1.0 = the equal-cycle model explains the IF leak exactly)")

    # ratio spectrum of the 'other' shape
    oth = sel[sh == "other"]
    if len(oth):
        r = (oth["d_pb_2"] / oth["d_pb_1"]).replace([np.inf, -np.inf], np.nan).dropna()
        out.append(sub(f"ratio d_pb2/d_pb1 inside the 'other' shape (n={len(r)})"))
        bins = [(-1e9, -0.5, "< -0.5"), (-0.5, 0.0, "-0.5..0"), (0.0, 0.5, "0..0.5"),
                (0.5, 0.85, "0.5..0.85"), (1.15, 1.40, "1.15..1.40"),
                (1.90, 3.0, "1.90..3.0"), (3.0, 1e9, "> 3.0")]
        for lo, hi, nm in bins:
            k = int(((r >= lo) & (r < hi)).sum())
            if k:
                out.append(f"   {nm:<12} {k:>5}  ({100.0*k/len(r):5.1f} %)")
        out.append(f"   p50 ratio {q(r,.5):.3f} ; iono-like target {MU21:.3f} ; "
                   f"range-like target 1.000")


def find_runs(d):
    """Maximal runs of consecutive 30 s translating epochs per (sat, sig) cell."""
    runs = []
    cur = None
    for r in d.itertuples(index=False):
        key = (r.sat, r.sig)
        if cur is not None and (cur["key"] != key
                                or r.tow != cur["last_tow"] + 30.0
                                or not r.translated):
            runs.append(cur)
            cur = None
        if r.translated:
            if cur is None:
                cur = {"key": key, "start": r.tow, "hour": r.hour,
                       "steps": [], "labels": []}
            cur["steps"].append(float(r.d_pb))
            cur["labels"].append(r.flabel if isinstance(r.flabel, str) else "")
            cur["last_tow"] = r.tow
    if cur is not None:
        runs.append(cur)
    return runs


def r2_section(d, e, idx, anchor, eps, out, bed, seed=0):
    out.append(hdr(f"R2  RUN ANATOMY -- {bed}"))
    runs = find_runs(d[d["pair30"] | d["translated"]].sort_values(
        ["sat", "sig", "tow"], kind="mergesort"))
    fn_runs = [r for r in runs
               if sum(1 for l in r["labels"] if l == "F-none") > len(r["steps"]) / 2.0]
    out.append(f"translating runs: {len(runs)} total, {len(fn_runs)} majority-F-none "
               f"(covering {sum(len(r['steps']) for r in fn_runs)} cell-epochs)")
    out.append("")
    out.append(f"{'population':<22} {'n runs':>7} {'p50 len':>8} {'p90 len':>8} {'max':>5} "
               f"{'p50 |excursion|':>16} {'p50 sum|step|':>14}")
    for nm, rr in [("all runs", runs), ("majority F-none", fn_runs)]:
        if not rr:
            continue
        L = [len(r["steps"]) for r in rr]
        ex = [abs(sum(r["steps"])) for r in rr]
        sa = [sum(abs(s) for s in r["steps"]) for r in rr]
        out.append(f"{nm:<22} {len(rr):>7} {q(L,.5):>8.0f} {q(L,.9):>8.0f} {max(L):>5} "
                   f"{q(ex,.5):>16.4f} {q(sa,.5):>14.4f}")

    out.append(sub("geometric relaxation test (rho = |step_{i+1}| / |step_i|)"))
    out.append("A geometric relaxation has a CONSTANT rho < 1 within a run; a random walk "
               "does not.")
    out.append(f"{'population':<22} {'runs>=4':>8} {'p50 rho':>9} {'p90 rho':>9} "
               f"{'p50 IQR(rho)':>13} {'monotone':>9} {'directed':>9}")
    for nm, rr in [("all runs", runs), ("majority F-none", fn_runs)]:
        sel = [r for r in rr if len(r["steps"]) >= 4]
        if not sel:
            continue
        med_rho, iqr_rho = [], []
        for r in sel:
            rat = [abs(r["steps"][i + 1]) / abs(r["steps"][i])
                   for i in range(len(r["steps"]) - 1) if abs(r["steps"][i]) > 1e-12]
            if len(rat) >= 2:
                med_rho.append(float(np.median(rat)))
                iqr_rho.append(float(np.quantile(rat, .75) - np.quantile(rat, .25)))
        mono = sum(1 for r in sel
                   if all(s > 0 for s in r["steps"]) or all(s < 0 for s in r["steps"]))
        drct = [abs(sum(r["steps"])) / max(sum(abs(s) for s in r["steps"]), 1e-12)
                for r in sel]
        out.append(f"{nm:<22} {len(sel):>8} {q(med_rho,.5):>9.3f} {q(med_rho,.9):>9.3f} "
                   f"{q(iqr_rho,.5):>13.3f} {100.0*mono/len(sel):>8.1f}% "
                   f"{q(drct,.5):>8.3f}")
    out.append("   'monotone' = every step in the run has the same sign.")
    out.append("   'directed' = p50 of |sum(step)| / sum|step|  (1.0 = pure ramp, "
               "~0 = oscillation).")
    out.append(f"   REFERENCE: a pure geometric relaxation gives rho = const and IQR ~ 0; "
               f"an uncorrelated random walk gives p50 rho ~ {0.707:.3f} with a wide IQR.")

    # ---- run-start attribution with an hour-matched null --------------------
    classes = {"STRANSFORM": TIER_STRANSFORM, "PHASE_ANCHOR": TIER_PHASE_ANCHOR,
               "PBGAUGE": TIER_PBGAUGE, "CLK_ANCHOR": TIER_CLK_ANCHOR,
               "READMIT": TIER_READMIT, "SLIP": TIER_SLIP,
               "GATE": ("GATE",), "ADMISSION": ("ADMISSION",), "TOPOLOGY": ("TOPOLOGY",)}

    def probe(sat, sig, tow, tiers, anchor_only):
        ts = set(tiers)
        for k in range(0, 4):                       # [start-3, start] epochs
            t = snap(tow - 30.0 * k)
            if anchor_only:
                a_now = anchor.get((sat, sig, t)) or anchor.get((sat, sig, snap(tow)))
                if a_now and (idx.slip_sta.get((sat, a_now, t), frozenset()) & ts):
                    return True
                if a_now and any(r for r in idx.sta_tier.get((sat, a_now, t), frozenset())
                                 if r in ts):
                    return True
            else:
                if idx.sat_tow.get((sat, t), frozenset()) & ts:
                    return True
        return False

    starts = [(r["key"][0], r["key"][1], r["start"], r["hour"]) for r in fn_runs]
    pool = d[d["pair30"]].copy()
    run_cells = set()
    for r in fn_runs:
        run_cells.add((r["key"][0], r["key"][1], snap(r["start"])))
    pool = pool[[(s, g, snap(t)) not in run_cells
                 for s, g, t in zip(pool["sat"], pool["sig"], pool["tow"])]]
    rng = np.random.default_rng(seed)
    hours = Counter(h for _, _, _, h in starts)
    draws = []
    n_draw = 5000
    for h, cnt in hours.items():
        sub_pool = pool[pool["hour"] == h]
        k = int(round(n_draw * cnt / max(len(starts), 1)))
        if len(sub_pool) == 0 or k == 0:
            continue
        ii = rng.integers(0, len(sub_pool), size=k)
        for i in ii:
            rr = sub_pool.iloc[int(i)]
            draws.append((rr["sat"], rr["sig"], float(rr["tow"]), int(rr["hour"])))
    out.append(sub(f"RUN-START attribution, window [start-3, start] "
                   f"(n_runs={len(starts)}, null n={len(draws)}, seed={seed})"))
    out.append(f"{'class':<14} {'ANY station':>12} {'null':>8} {'lift':>7}   "
               f"{'ANCHOR station':>15} {'null':>8} {'lift':>7}")
    for nm, tiers in classes.items():
        ro = np.mean([probe(s, g, t, tiers, False) for s, g, t, _ in starts]) if starts else 0.0
        rn = np.mean([probe(s, g, t, tiers, False) for s, g, t, _ in draws]) if draws else 0.0
        ao = np.mean([probe(s, g, t, tiers, True) for s, g, t, _ in starts]) if starts else 0.0
        an = np.mean([probe(s, g, t, tiers, True) for s, g, t, _ in draws]) if draws else 0.0
        lo = (ro / rn) if rn > 0 else float("nan")
        la = (ao / an) if an > 0 else float("nan")
        out.append(f"{nm:<14} {100.0*ro:>11.2f}% {100.0*rn:>7.2f}% "
                   f"{(fmt(lo,2) if math.isfinite(lo) else '-'):>7}   "
                   f"{100.0*ao:>14.2f}% {100.0*an:>7.2f}% "
                   f"{(fmt(la,2) if math.isfinite(la) else '-'):>7}")
    # handover proximity
    hv = TIER_STRANSFORM + TIER_PBGAUGE + TIER_PHASE_ANCHOR

    def near_handover(sat, tow, n_ep=20):
        for k in range(0, n_ep + 1):
            if idx.sat_tow.get((sat, snap(tow - 30.0 * k)), frozenset()) & set(hv):
                return True
        return False
    fo = np.mean([near_handover(s, t) for s, _, t, _ in starts]) if starts else 0.0
    fn_ = np.mean([near_handover(s, t) for s, _, t, _ in draws]) if draws else 0.0
    out.append(f"   runs starting within 20 epochs AFTER a handover of that satellite: "
               f"{100.0*fo:.2f}%  (null {100.0*fn_:.2f}%, lift "
               f"{(fo/fn_ if fn_>0 else float('nan')):.2f})")



def load_wire_series(run):
    """products.txt -> (clk[(sat,tow)], pb[(sat,sig,tow)])."""
    path = os.path.join(run, "products.txt")
    clk, pb = {}, {}
    tow = None
    if not os.path.exists(path):
        return clk, pb
    with open(path, "r", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            c = line[0]
            if c == "E":
                pr = line.rstrip().split(",")
                if len(pr) > 2:
                    try:
                        tow = round(float(pr[2]), 3)
                    except ValueError:
                        tow = None
            elif tow is None:
                continue
            elif c == "C":
                pr = line.rstrip().split(",")
                if len(pr) > 2:
                    try:
                        clk[(pr[1], tow)] = float(pr[2])
                    except ValueError:
                        pass
            elif c == "P":
                pr = line.rstrip().split(",")
                if len(pr) > 3:
                    try:
                        pb[(pr[1], pr[2], tow)] = float(pr[3])
                    except ValueError:
                        pass
    return clk, pb


def load_truth_bias_sim(run):
    """Sim fixture truth: <fixture>/<STA>/truth_bias.csv (satellite,signal,...,osb_m).

    Satellite-side and time-constant by construction: the simulator (the
    truth_bias.csv writer in src/sim/main.cpp) records each (sat, signal) once, at
    first sighting, and the file carries no time column.
    The fixture is named only implicitly, by the absolute RINEX paths in arm.json.
    """
    fx = None
    ap = os.path.join(run, "arm.json")
    if os.path.exists(ap):
        with open(ap, "r", errors="replace") as f:
            txt = f.read()
        for mm in re.finditer(r'"rinex_path"\s*:\s*"([^"]+)"', txt):
            st = os.path.dirname(mm.group(1).replace("/", os.sep))
            cand = os.path.join(st, "truth_bias.csv")
            if os.path.exists(cand):
                fx = cand
                break
    if fx is None:
        return None, None, "no truth_bias.csv reachable from arm.json rinex paths"
    t = pd.read_csv(fx)
    osb = {(str(r.satellite), str(r.signal)): float(r.osb_m)
           for r in t.itertuples(index=False)}
    return osb, fx, f"sim fixture truth_bias.csv ({len(osb)} (sat,signal) rows)"


def load_osb_bia(run, systems=("G",)):
    """Field truth: the COD MGX OSB .BIA, parsed the same way as
    scripts/wire_vs_code_census.py:parse_bia (ProductParser column offsets, ns->m).
    `systems`: the constellation letters kept, default GPS alone (card GAL)."""
    ap = os.path.join(run, "arm.json")
    root = year = doy = None
    src, typ = "COD", "FIN"
    if os.path.exists(ap):
        try:
            a = json.load(open(ap, "r", errors="replace"))
        except Exception:
            a = {}
        root = (a.get("files", {}) or {}).get("internal_data_root")
        dd = a.get("data_download", {}) or {}
        year, doy = dd.get("year"), dd.get("doy")
        src = dd.get("product_source", "COD")
        typ = dd.get("product_type", "FIN")
    if not (root and year and doy):
        return None, None, "arm.json lacks internal_data_root / year / doy"
    pat = os.path.join(root.replace("/", os.sep), f"{year}-{doy:03d}", "products",
                       f"{src}0MGX{typ}_*_OSB.BIA")
    hits = sorted(glob.glob(pat))
    if not hits:
        return None, None, f"no OSB .BIA at {pat}"
    path = hits[0]
    osb = {}
    in_block = False
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith("+BIAS/SOLUTION"):
                in_block = True
                continue
            if line.startswith("-BIAS/SOLUTION"):
                break
            if not in_block or not line.startswith("OSB ") or len(line) < 70:
                continue
            prn = line[10:13].strip()
            station = line[14:23].strip()
            obs1 = line[24:28].strip()
            obs2 = line[29:33].strip()
            start = line[34:48].strip()
            end = line[49:63].strip()
            unit = line[64:68].strip()
            bias = line[69:90].strip()
            if station or not prn or prn[0] not in systems or unit != "ns":
                continue
            sig = obs2 if obs2 else obs1
            try:
                t0 = tuple(int(x) for x in start.split(":"))
                t1 = tuple(int(x) for x in end.split(":"))
            except ValueError:
                continue
            if not (t0 <= (int(year), int(doy), 1) <= t1):
                continue
            osb[(prn, sig)] = float(bias) * C_MS * 1e-9
    return osb, path, f"{src}/{typ} OSB .BIA ({len(osb)} (sat,signal) rows)"


def arc_rms(series, hmin, tow0, gap_s=120.0):
    """series[key][tow] -> within-arc, arc-mean-removed RMS list + d/dt RMS."""
    rms, dif, n_arc, npt = [], [], 0, 0

    def flush(cur):
        nonlocal n_arc, npt
        if len(cur) >= 3:
            v = np.asarray(cur, float)
            rms.append(float(np.sqrt(((v - v.mean()) ** 2).mean())))
            dif.extend(np.diff(v).tolist())
            n_arc += 1
            npt += len(v)

    for key, byt in series.items():
        tw = sorted(byt)
        cur = []
        for i, t in enumerate(tw):
            in_win = ((t - tow0) // 3600.0) + 1 >= hmin
            brk = (i > 0 and t - tw[i - 1] > gap_s) or not in_win
            if brk:
                flush(cur)
                cur = []
            if in_win:
                cur.append(byt[t])
        flush(cur)
    d_rms = float(np.sqrt(np.mean(np.asarray(dif) ** 2))) if dif else float("nan")
    return rms, d_rms, n_arc, npt


def r3_section(run, d, eps, out, bed, truth_path, systems=("G",)):
    """R3: IF-level wire-vs-truth grade of the clock, the phase bias, and their sum.

    One grade per constellation in `systems` (default GPS alone, whose output is
    unchanged from the single-system version); with more than one, the section
    header names the constellation (card GAL)."""
    for sy in systems:
        lab = bed if len(systems) == 1 else f"{bed} [system {sy}]"
        r3_one(run, d, eps, out, lab, truth_path, sy)
    return 0


def r3_one(run, d, eps, out, bed, truth_path, sy="G"):
    """R3 for one constellation letter `sy` (the single-system grade, run per constellation)."""
    band2, f1_hz, f2_hz = R3_PAIRS.get(sy, R3_PAIRS["G"])
    if_a = f1_hz ** 2 / (f1_hz ** 2 - f2_hz ** 2)
    if_b = f2_hz ** 2 / (f1_hz ** 2 - f2_hz ** 2)
    out.append(hdr(f"R3  TRUTH GRADE AT THE IF LEVEL -- {bed}"))
    osb, src, note = load_truth_bias_sim(run)
    if osb is None:
        osb, src, note = load_osb_bia(run, (sy,))
    out.append(f"truth source: {note}")
    if osb is None:
        out.append("R3 NOT RUN.")
        return 0
    out.append(f"truth path  : {src}")
    out.append("")
    out.append("CONVENTION (matches scripts/wire_vs_code_census.py:272-285):")
    out.append("    e_clk = -clk_wire + clk_truth = -c0        (per satellite)")
    out.append("    e_pb  = pb_wire - pb_truth    = pb - osb   (per sat, signal)")
    out.append("    e_phi = e_clk + e_pb                       (= raw['phase'], :284)")
    out.append("IF: e_pbIF = a*(pb1-o1) - b*(pb2-o2); e_phiIF = e_clk + e_pbIF")
    out.append("    (a-b = 1, so the clock enters the IF correction unscaled).")
    out.append("NO truth_satclock.csv exists anywhere on this machine => the injected")
    out.append("satellite-clock error is ZERO and the wire C record IS the clock error in")
    out.append("the producer's frame (simnet_grade.py:1314-1315; "
               "simnet_wire_integer_consistency.py:591-596).")
    out.append("truth PB is CONSTANT in time (no time column; src/sim/main.cpp:2125-2131),")
    out.append("so every movement of e_pb is wire movement.")

    wclk, wpb = load_wire_series(run)
    # this constellation's records only (a no-op on a single-system wire; card GAL)
    wclk = {k: v for k, v in wclk.items() if k[0][:1] == sy}
    wpb = {k: v for k, v in wpb.items() if k[0][:1] == sy}
    if not wclk or not wpb:
        out.append("R3 NOT RUN -- products.txt carried no C/P records.")
        return 0
    osb_sigs = {k[1] for k in osb}
    b1s = sorted({s for (_, s, _) in wpb if band_of(s) == 1})
    b2s = sorted({s for (_, s, _) in wpb if band_of(s) == band2})
    # Prefer the signal the consumer applies (the field wire emits L1C
    # and L1W while the consumer tracks L1W; grading L1C would measure a channel
    # the consumer never reads).
    csig = set(d["sig"].unique())
    s1 = (next((x for x in b1s if x in osb_sigs and x in csig), None)
          or next((x for x in b1s if x in osb_sigs), None))
    s2 = (next((x for x in b2s if x in osb_sigs and x in csig), None)
          or next((x for x in b2s if x in osb_sigs), None))
    out.append("")
    out.append(f"wire bands {b1s} / {b2s}; consumer tracks {sorted(csig)}; "
               f"truth signals {sorted(osb_sigs)}")
    out.append(f"graded pair = ({s1}, {s2})  [consumer's own signals where available]")
    if not (s1 and s2):
        out.append("R3 NOT RUN -- no band pair common to the wire and the truth.")
        return 0

    by_tow = defaultdict(list)
    for (sat, t) in wclk:
        by_tow[t].append(sat)
    tow0 = min(by_tow)
    tracked = set(zip(d["sat"], d["tow"].map(snap)))
    series = {k: defaultdict(dict) for k in ("e_clk", "e_pb", "e_phi", "e_sum")}
    series_c = {k: defaultdict(dict) for k in series}
    n_miss = Counter()
    for t, sats in by_tow.items():
        row = {}
        for sat in sats:
            p1 = wpb.get((sat, s1, t))
            p2 = wpb.get((sat, s2, t))
            o1 = osb.get((sat, s1))
            o2 = osb.get((sat, s2))
            if p1 is None or p2 is None:
                n_miss["wire_pb"] += 1
                continue
            if o1 is None or o2 is None:
                n_miss["truth_osb"] += 1
                continue
            e_clk = -wclk[(sat, t)]
            e_pb = if_a * (p1 - o1) - if_b * (p2 - o2)
            row[sat] = (e_clk, e_pb, e_clk + e_pb)
        if len(row) < 3:
            continue
        med = [float(np.median([v[i] for v in row.values()])) for i in range(3)]
        for sat, v in row.items():
            for i, k in enumerate(("e_clk", "e_pb", "e_phi")):
                series[k][sat][t] = v[i] - med[i]
                if (sat, t) in tracked:
                    series_c[k][sat][t] = v[i] - med[i]
            # linearity control: the sum of the two channels with their common
            # modes removed separately (the median is not linear, so this need
            # not equal e_phi with its common mode removed).
            series["e_sum"][sat][t] = (v[0] - med[0]) + (v[1] - med[1])
            if (sat, t) in tracked:
                series_c["e_sum"][sat][t] = (v[0] - med[0]) + (v[1] - med[1])
    # ---- DIFFMEDIAN common-mode control ---------------------------------
    # A per-epoch median of levels is exposed to a rotating satellite set: when
    # the set changes, the median jumps and injects a spurious common step into
    # every satellite.  common_mode() in scripts/wire_vs_code_census.py
    # pre-registers a second estimator that is immune to this: integrate the
    # per-epoch median of the satellite-wise differences.  The same recipe is
    # used here.
    series_d = {k: defaultdict(dict) for k in ("e_clk", "e_pb", "e_phi")}
    series_dc = {k: defaultdict(dict) for k in series_d}
    raws = {k: defaultdict(dict) for k in series_d}
    for t, sats in by_tow.items():
        for sat in sats:
            p1 = wpb.get((sat, s1, t)); p2 = wpb.get((sat, s2, t))
            o1 = osb.get((sat, s1)); o2 = osb.get((sat, s2))
            if p1 is None or p2 is None or o1 is None or o2 is None:
                continue
            ec = -wclk[(sat, t)]
            ep = if_a * (p1 - o1) - if_b * (p2 - o2)
            raws["e_clk"][sat][t] = ec
            raws["e_pb"][sat][t] = ep
            raws["e_phi"][sat][t] = ec + ep
    all_t = sorted(by_tow)
    for k in series_d:
        cm = 0.0
        prev = None
        for t in all_t:
            cur = {sat: raws[k][sat][t] for sat in raws[k] if t in raws[k][sat]}
            if prev is not None:
                both = [cur[s_] - prev[s_] for s_ in cur if s_ in prev]
                if both:
                    cm += float(np.median(both))
            for sat, v in cur.items():
                series_d[k][sat][t] = v - cm
                if (sat, t) in tracked:
                    series_dc[k][sat][t] = v - cm
            prev = cur

    out.append(f"graded satellite-epochs: {sum(len(v) for v in series['e_phi'].values())}"
               f" (consumer-tracked subset "
               f"{sum(len(v) for v in series_c['e_phi'].values())}); "
               f"skipped: {dict(n_miss)}")

    for lab, ser in (("ALL wire satellites", series),
                     ("consumer-TRACKED satellites only", series_c),
                     ("ALL sats, DIFFMEDIAN de-common", series_d),
                     ("consumer-TRACKED, DIFFMEDIAN de-common", series_dc)):
        out.append(sub(f"{lab} -- within-arc RMS after removing the arc mean (m, IF)"))
        out.append(f"{'window':<12} {'channel':<8} {'arcs':>5} {'pts':>7} {'p50 RMS':>9} "
                   f"{'p90 RMS':>9} {'max RMS':>9} {'RMS d/dt':>10}")
        for nm, hmin in (("ALL hours", 1), ("hours 2-6", 2)):
            for k in ("e_phi", "e_clk", "e_pb", "e_sum"):
                if k not in ser:
                    continue
                rms, drms, na, npt = arc_rms(ser[k], hmin, tow0)
                if not rms:
                    continue
                out.append(f"{nm:<12} {k:<8} {na:>5} {npt:>7} {q(rms,.5):>9.4f} "
                           f"{q(rms,.9):>9.4f} {max(rms):>9.4f} {drms:>10.5f}")
        xs, ys = [], []
        for sat in ser["e_pb"]:
            tw = sorted(ser["e_pb"][sat])
            for i in range(1, len(tw)):
                if tw[i] - tw[i - 1] > 120.0:
                    continue
                if ((tw[i] - tow0) // 3600.0) + 1 < 2:
                    continue
                ys.append(ser["e_pb"][sat][tw[i]] - ser["e_pb"][sat][tw[i - 1]])
                xs.append(ser["e_clk"][sat][tw[i]] - ser["e_clk"][sat][tw[i - 1]])
        if len(xs) > 3:
            r = float(np.corrcoef(xs, ys)[0, 1])
            sl = float(np.linalg.lstsq(np.vstack([xs, np.ones_like(xs)]).T,
                                       np.asarray(ys), rcond=None)[0][0])
            out.append(f"   corr( d(e_pb), d(e_clk) ) hours 2-6: r = {r:+.4f}, "
                       f"slope = {sl:+.4f}, n = {len(xs)}")
            out.append("   (r ~ -1, slope ~ -1 would mean the PB tracks the clock so the")
            out.append("    phase correction is preserved -- a pure redistribution.)")

    out.append(sub("VERDICT: full phase correction vs the clock alone"))
    out.append("'clock alone' = e_clk with the PB frozen at its arc-first value; after the")
    out.append("per-arc de-mean a frozen PB is a constant, so that IS the e_clk row above.")
    for lab, ser in (("ALL wire satellites", series),
                     ("consumer-TRACKED only", series_c),
                     ("ALL sats, DIFFMEDIAN", series_d),
                     ("consumer-TRACKED, DIFFMEDIAN", series_dc)):
        for nm, hmin in (("ALL hours", 1), ("hours 2-6", 2)):
            a_, _, _, _ = arc_rms(ser["e_phi"], hmin, tow0)
            b_, _, _, _ = arc_rms(ser["e_clk"], hmin, tow0)
            if not a_ or not b_:
                continue
            ra, rb = q(a_, .5), q(b_, .5)
            out.append(f"   {lab:<24} {nm:<10} p50 RMS e_phi = {ra:.4f} m  vs  "
                       f"e_clk(PB frozen) = {rb:.4f} m  -> "
                       f"{'PB MOVEMENT HURTS' if ra > rb else 'PB MOVEMENT HELPS'} "
                       f"({ra/max(rb,1e-12):.2f}x)")
    return 0

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--r3", action="store_true",
                    help="R1/R2/R3: is the wire's PB movement harmful, and what shape")
    ap.add_argument("--truth", default=None,
                    help="R3: path to the simulator truth CSV (see --r3 output)")
    ap.add_argument("--raw", action="store_true",
                    help="H-FILT mode: locate the raw pre-filter PB, or prove it absent")
    ap.add_argument("--eps", type=float, default=0.005)
    ap.add_argument("--eps2", type=float, default=0.02,
                    help="second threshold for the repeated key tables")
    ap.add_argument("--sign", action="store_true", help="print the sign note and exit")
    ap.add_argument("--systems", default="G",
                    help="card GAL: comma list of constellation letters the R3 grade runs "
                         "for, one section each (default G, the pre-card output)")
    args = ap.parse_args()
    if args.sign:
        print(SIGN_NOTE)
        return 0
    if not args.run_dir:
        ap.error("run_dir is required (omit it only with --sign)")
    if args.raw:
        out = []
        rc = raw_mode(args.run_dir.rstrip("\\/"), out)
        print("\n".join(out))
        return rc
    if args.r3:
        run = args.run_dir.rstrip("\\/")
        bed = os.path.basename(run)
        eps = args.eps
        out = []
        out.append(hdr(f"R1/R2/R3 harm-shape read -- {bed}"))
        out.append(f"run_dir : {run}\neps     : {eps} m")
        d, meta = load_trace(run, eps)
        e = load_events(run)
        anchor = load_anchor(run)
        idx = EventIndex(e)
        # annotate every 30 s-paired cell with its Q1 class + priority F label
        d["cls"] = ""
        d["flabel"] = ""
        tmask = d["translated"] & d["pair30"]
        tr = d[tmask]
        cls = classify(tr, eps)
        labs = [label_row(r, idx, anchor) for r in tr.itertuples(index=False)]
        d.loc[tmask, "cls"] = cls.values
        d.loc[tmask, "flabel"] = np.where(cls.values == "F", labs, "")
        m = build_pivot(d, eps)
        r1_section(m, eps, out, bed)
        r1b_section(m, eps, out, bed)
        r2_section(d, e, idx, anchor, eps, out, bed)
        r3_section(run, d, eps, out, bed, args.truth,
                   tuple(s.strip() for s in args.systems.split(",") if s.strip()))
        print("\n".join(out))
        return 0

    run = args.run_dir.rstrip("\\/")
    out = []
    out.append(hdr(f"FTC step attribution -- {os.path.basename(run)}"))
    out.append(f"run_dir : {run}")
    out.append(f"eps     : {args.eps} m   (repeat tables at {args.eps2} m)")
    out.append("")
    out.append(SIGN_NOTE)

    d, meta = load_trace(run, args.eps)
    e = load_events(run)
    anchor = load_anchor(run)
    wire = load_wire(run)
    idx = EventIndex(e)

    # ---------------- 0. inventory + sanity ---------------------------------
    out.append(hdr("0. INVENTORY AND SANITY"))
    out.append(f"consumer cell-epoch rows      : {meta['n_rows']}")
    out.append(f"consumer epochs               : {meta['n_epochs']} "
               f"(tow {d['tow'].min():.0f}..{d['tow'].max():.0f})")
    out.append(f"first sightings (no pair)     : {meta['n_first']}")
    out.append(f"non-30 s cell gaps (skipped)  : {meta['n_gap']}")
    out.append(f"usable 30 s consecutive pairs : {int(d['pair30'].sum())}")
    out.append(f"signals                       : {sorted(d['sig'].unique())}")
    out.append(f"satellites                    : {d['sat'].nunique()}")
    out.append("")
    out.append("ev_pb vocabulary:")
    for k, v in sorted(Counter(d["ev_pb"].astype(str)).items(),
                       key=lambda kv: (-kv[1], kv[0])):
        out.append(f"   {k:<24} {v}")
    out.append("")
    out.append("producer event tier x event (network_events.csv):")
    for (t_, ev), n in sorted(Counter(zip(e["tier"], e["event"])).items()):
        mark = "   " if t_ not in TIER_TELEMETRY else " * "
        out.append(f"{mark}{t_:<16} {ev:<24} {n}")
    out.append("   (* = per-cell telemetry tier, held OUT of F attribution)")

    tr = d[d["translated"] & d["pair30"]].copy()
    tr_any = d[d["translated"]]
    out.append("")
    out.append(f"translated cell-epochs        : {len(tr_any)} "
               f"(declared {(tr_any['tr_kind']=='declared').sum()} / "
               f"undeclared {(tr_any['tr_kind']=='undeclared').sum()})")
    out.append(f"   of which 30 s-paired       : {len(tr)} "
               f"(gap-spanning translations: {len(tr_any)-len(tr)})")
    out.append(f"sum|d_pb| over translated     : {tr_any['d_pb'].abs().sum():.3f} m "
               f"(max |d_pb| {tr_any['d_pb'].abs().max():.3f} m)")
    out.append("SANITY: compare the three numbers above with the run-end "
               "'pb step translate (PBT)' line in logs/main_log.log.")
    out.append(f"per-epoch common-mode support: n_clk_sats p50="
               f"{q(d['n_clk_sats'],.5):.0f}, n_pb_sats(per signal) p50="
               f"{q(d['n_pb_sats'],.5):.0f}")

    # ---- attribution reachability (invariant #4: a zero count is meaningful
    #      only if events could have landed on a consumer cell)
    out.append(sub("attribution reachability (how many events CAN land on a consumer cell)"))
    cells = set(zip(d["sat"], d["sig"], d["tow"].map(snap)))
    dp = d[d["pair30"]]
    cells_p = set(zip(dp["sat"], dp["sig"], dp["tow"].map(snap)))
    sats_c = set(d["sat"].unique())
    for nm, tiers in [("STRANSFORM", TIER_STRANSFORM), ("PHASE_ANCHOR", TIER_PHASE_ANCHOR),
                      ("PBGAUGE", TIER_PBGAUGE), ("CLK_ANCHOR", TIER_CLK_ANCHOR),
                      ("READMIT", TIER_READMIT), ("SLIP(T0/T1/T2/T4)", TIER_SLIP),
                      ("other(structural)", TIER_OTHER)]:
        s = e[e["tier"].isin(tiers)]
        n_all = len(s)
        n_sat = int(s["sat_base"].isin(sats_c).sum())
        n_hit = sum(1 for r in s.itertuples(index=False)
                    if any((r.sat_base, sg, snap(r.tow)) in cells for sg in d["sig"].unique()))
        n_pair = sum(1 for r in s.itertuples(index=False)
                     if any((r.sat_base, sg, snap(r.tow)) in cells_p for sg in d["sig"].unique()))
        out.append(f"   {nm:<20} rows={n_all:>6}  sat in roster={n_sat:>6}  "
                   f"TRACKED at that tow={n_hit:>6}  and 30 s-PAIRED={n_pair:>6}")
    # anchor changes
    ach = 0
    ach_hit = 0
    ach_with_sts = 0
    a_keys = sorted(anchor.keys())
    for (sat, sg, tw) in a_keys:
        prev = anchor.get((sat, sg, tw - 30.0))
        if prev is None or prev == anchor[(sat, sg, tw)]:
            continue
        ach += 1
        if (sat, sg, snap(tw)) in cells:
            ach_hit += 1
            if idx.has(sat, snap(tw), TIER_STRANSFORM):
                ach_with_sts += 1
    out.append(f"   {'ANCHOR CHANGE':<20} 30 s anchor_sta changes={ach:>6}  "
               f"on a tracked cell={ach_hit:>6}  of those WITH a STRANSFORM row="
               f"{ach_with_sts:>6}")
    out.append("   (F-ANCHORCHANGE counts only changes WITHOUT a STRANSFORM row, "
               "so 'on a tracked cell' minus 'WITH a STRANSFORM' is its ceiling)")

    # ---------------- 1. Q1 classification ----------------------------------
    for eps in (args.eps, args.eps2):
        tr["cls"] = classify(tr, eps)
        labs_all = [label_row(r, idx, anchor, multi=True) for r in tr.itertuples(index=False)]
        tr["flabel"] = [l[0] for l in labs_all]
        tr["flabset"] = [l[1] for l in labs_all]
        tr.loc[tr["cls"] != "F", "flabel"] = ""

        out.append(hdr(f"1. Q1 CLASSIFICATION OF TRANSLATED CELL-EPOCHS  (eps={eps} m)"))
        out.append("C = |d_pb_dc| < eps (PB step was common-mode only)")
        out.append("R = |d_pb_dc| >= eps and |d_phi| < eps  (redistribution: clock moved "
                   "with PB, total phase correction did not step)")
        out.append("F = |d_pb_dc| >= eps and |d_phi| >= eps (genuine phase-correction step)")
        out.append(f"F priority order: {' > '.join(PRIORITY)}")
        class_table(tr, eps, "ALL HOURS", out)
        class_table(tr[tr["hour"] >= 2], eps, "HOURS 2-6 (settled)", out)
        class_table(tr[tr["hour"] == 1], eps, "HOUR 1 (ignition)", out)

        out.append(sub("by declared / undeclared (all hours)"))
        out.append(f"{'class':<16} {'decl n':>7} {'decl sum|d_pb|':>15} "
                   f"{'undecl n':>9} {'undecl sum|d_pb|':>17}")
        for c in ["C", "R"] + [x for x in PRIORITY]:
            s = tr[tr["cls"] == c] if c in ("C", "R") else \
                tr[(tr["cls"] == "F") & (tr["flabel"] == c)]
            if len(s) == 0:
                continue
            a = s[s["tr_kind"] == "declared"]
            b = s[s["tr_kind"] == "undeclared"]
            out.append(f"{c:<16} {len(a):>7} {a['d_pb'].abs().sum():>15.3f} "
                       f"{len(b):>9} {b['d_pb'].abs().sum():>17.3f}")

        out.append(sub("by hour (count / sum|d_pb| m)"))
        hours = sorted(tr["hour"].unique())
        out.append("class            " + "".join(f"{('h%d' % h):>18}" for h in hours))
        for c in ["C", "R"] + [x for x in PRIORITY]:
            s = tr[tr["cls"] == c] if c in ("C", "R") else \
                tr[(tr["cls"] == "F") & (tr["flabel"] == c)]
            if len(s) == 0:
                continue
            cells = []
            for h in hours:
                ss = s[s["hour"] == h]
                cells.append(f"{len(ss):>8d}/{ss['d_pb'].abs().sum():>9.3f}")
            out.append(f"{c:<16} " + "".join(f"{x:>18}" for x in cells))

        # raw multi-label counts
        out.append(sub("F raw MULTI-label counts (a cell-epoch can carry several)"))
        mc = Counter()
        for s in tr.loc[tr["cls"] == "F", "flabset"]:
            if s:
                for x in s:
                    mc[x] += 1
            else:
                mc["F-none"] += 1
        nF = int((tr["cls"] == "F").sum())
        for k in PRIORITY:
            if mc.get(k):
                out.append(f"   {k:<18} {mc[k]:>6}   ({100.0*mc[k]/max(nF,1):5.1f} % of F)")
        out.append(f"   F total {nF}")

        # ---------------- null / lift ---------------------------------------
        out.append(sub(f"NULL base-rate control (eps={eps})"))
        nt = d[(~d["translated"]) & d["pair30"]].copy()
        nlabs = [label_row(r, idx, anchor, multi=True)[1]
                 for r in nt.itertuples(index=False)]
        out.append(f"{'class':<18} {'obs rate':>10} {'chance':>10} {'lift':>8} "
                   f"{'n_obs':>7} {'n_null':>8}   (obs = translated cell-epochs)")
        for k in PRIORITY[:-1]:
            nobs = sum(1 for s in tr.loc[tr["cls"] != "", "flabset"] if k in s)
            nnul = sum(1 for s in nlabs if k in s)
            ro = nobs / max(len(tr), 1)
            rn = nnul / max(len(nt), 1)
            lift = (ro / rn) if rn > 0 else float("inf")
            out.append(f"   {k:<15} {ro:>10.4f} {rn:>10.4f} "
                       f"{(fmt(lift,2) if math.isfinite(lift) else 'inf'):>8} "
                       f"{nobs:>7} {nnul:>8}")
        out.append(f"   (translated n={len(tr)}, non-translated n={len(nt)})")

        if eps == args.eps:
            tr_main = tr.copy()

    tr = tr_main
    eps = args.eps
    tr["cls"] = classify(tr, eps)

    # ---------------- 2. Q2 -------------------------------------------------
    out.append(hdr("2. Q2  DOES THE PRODUCER ALREADY KNOW THE SIGNED STEP?"))
    out.append("STRANSFORM detail carries basis= and prev=; delta_event = basis - prev.")
    out.append("The wire step is products.txt P-record d_pb for the SAME (sat, signal, tow).")
    st = e[e["tier"] == "STRANSFORM"].copy()
    if len(st):
        def getkv(s, k):
            for tok in str(s).split(";"):
                if tok.startswith(k + "="):
                    try:
                        return float(tok.split("=", 1)[1])
                    except ValueError:
                        return float("nan")
            return float("nan")
        st["basis"] = st["detail"].map(lambda s: getkv(s, "basis"))
        st["prev"] = st["detail"].map(lambda s: getkv(s, "prev"))
        st["marked"] = st["detail"].map(lambda s: getkv(s, "marked"))
        st["delta_event"] = st["basis"] - st["prev"]
        rows = []
        for r in st.itertuples(index=False):
            if not r.sat_sig:
                continue
            k_now = (r.sat_base, r.sat_sig, snap(r.tow))
            k_prev = (r.sat_base, r.sat_sig, snap(r.tow - 30.0))
            if k_now in wire and k_prev in wire:
                dw = wire[k_now] - wire[k_prev]
                rows.append((r.event, r.action, r.sat_base, r.sat_sig, r.tow,
                             r.delta_event, dw, r.marked))
        w = pd.DataFrame(rows, columns=["event", "action", "sat", "sig", "tow",
                                        "delta_event", "d_pb_wire", "marked"])
        out.append(f"STRANSFORM rows: {len(st)}; with a wire pair at tow and tow-30: {len(w)}")
        if len(w):
            out.append("")
            out.append("(a) THE WIRE STEP the handover actually produced, |d_pb_wire| (m):")
            out.append(f"{'event':<20} {'action':<13} {'n':>5} {'p50':>9} {'p90':>10} "
                       f"{'max':>10} {'n<eps':>7} {'%<eps':>7}")
            for (evn, act), s in w.groupby(["event", "action"]):
                v = s["d_pb_wire"].abs()
                n0 = int((v < eps).sum())
                out.append(f"{evn:<20} {act:<13} {len(s):>5} {q(v,.5):>9.4f} "
                           f"{q(v,.9):>10.4f} {v.max():>10.4f} {n0:>7} "
                           f"{100.0*n0/max(len(s),1):>6.1f}%")
            v = w["d_pb_wire"].abs()
            out.append(f"{'ALL':<20} {'':<13} {len(w):>5} {q(v,.5):>9.4f} "
                       f"{q(v,.9):>10.4f} {v.max():>10.4f} "
                       f"{int((v<eps).sum()):>7} {100.0*(v<eps).mean():>6.1f}%")
            out.append("")
            out.append("(b) DOES THE EVENT'S OWN NUMBER PREDICT THAT STEP?  "
                       "residual |d_pb_wire - delta_event| (m):")
            out.append(f"{'event':<20} {'action':<13} {'n':>5} {'|res| p50':>10} "
                       f"{'p90':>10} {'max':>10} {'p50|delta_ev|':>14}")
            for (evn, act), s in w.groupby(["event", "action"]):
                res = (s["d_pb_wire"] - s["delta_event"]).abs()
                out.append(f"{evn:<20} {act:<13} {len(s):>5} {q(res,.5):>10.4f} "
                           f"{q(res,.9):>10.4f} {res.max():>10.4f} "
                           f"{q(s['delta_event'].abs(),.5):>14.4f}")
            res_all = (w["d_pb_wire"] - w["delta_event"]).abs()
            out.append(f"{'ALL':<20} {'':<13} {len(w):>5} {q(res_all,.5):>10.4f} "
                       f"{q(res_all,.9):>10.4f} {res_all.max():>10.4f} "
                       f"{q(w['delta_event'].abs(),.5):>14.4f}")
            out.append("   BASELINE for (b): predicting ZERO gives residual "
                       f"p50 {q(w['d_pb_wire'].abs(),.5):.4f} m, "
                       f"p90 {q(w['d_pb_wire'].abs(),.9):.4f} m.  A delta_event that "
                       "beats that carries information; one that does not, does not.")
            rm = (w["d_pb_wire"] + w["delta_event"]).abs()
            out.append(f"   sign check: p50 |dw - de| = {q(res_all,.5):.4f} m ; "
                       f"p50 |dw + de| = {q(rm,.5):.4f} m ; "
                       f"p50 |dw| = {q(w['d_pb_wire'].abs(),.5):.4f} m")
            out.append("")
            out.append("(c) field contents of the STRANSFORM detail (basis/prev):")
            for evn, s in st.groupby("event"):
                out.append(f"   {evn:<22} n={len(s):>4} basis p50={q(s['basis'],.5):>10.4f} "
                           f"|basis| max={s['basis'].abs().max():>9.4f}  "
                           f"prev p50={q(s['prev'],.5):>10.4f}  "
                           f"frac(prev==0)={float((s['prev']==0).mean()):.2f}  "
                           f"marked p50={q(s['marked'],.5):.0f}")
            out.append("")
            out.append("(d) the handovers that DID step the wire (|d_pb_wire| >= eps):")
            big_w = w[w["d_pb_wire"].abs() >= eps]
            for (evn, act), s in big_w.groupby(["event", "action"]):
                out.append(f"   {evn:<20} {act:<13} n={len(s):>4} "
                           f"p50|dw|={q(s['d_pb_wire'].abs(),.5):>9.4f} "
                           f"max|dw|={s['d_pb_wire'].abs().max():>9.4f} "
                           f"p50|res|={q((s['d_pb_wire']-s['delta_event']).abs(),.5):>9.4f}")
        # PBGAUGE
        pg = e[e["tier"] == "PBGAUGE"].copy()
        rows = []
        for r in pg.itertuples(index=False):
            if not r.sat_sig:
                continue
            k_now = (r.sat_base, r.sat_sig, snap(r.tow))
            k_prev = (r.sat_base, r.sat_sig, snap(r.tow - 30.0))
            if k_now in wire and k_prev in wire:
                rows.append((r.event, wire[k_now] - wire[k_prev],
                             float(r.statistic) if str(r.statistic) not in ("nan", "") else float("nan")))
        pgw = pd.DataFrame(rows, columns=["event", "d_pb_wire", "statistic"])
        out.append("")
        out.append(f"PBGAUGE (statistic = the new basis value, no prev field): n={len(pgw)}")
        if len(pgw):
            for evn, s in pgw.groupby("event"):
                out.append(f"   {evn:<22} n={len(s):>4} p50|d_pb_wire|="
                           f"{q(s['d_pb_wire'].abs(),.5):.4f} "
                           f"n|dw|<eps={int((s['d_pb_wire'].abs()<eps).sum())}")
        # PHASE_ANCHOR carries no value column
        pa = e[e["tier"] == "PHASE_ANCHOR"]
        out.append(f"PHASE_ANCHOR: {len(pa)} rows, statistic column all-NaN="
                   f"{bool(pa['statistic'].isna().all())} -> carries NO signed value.")
    else:
        out.append("no STRANSFORM rows")

    # ---------------- 3. Q3 -------------------------------------------------
    out.append(hdr("3. Q3  SIZE SPECTRUM"))
    labs_all = [label_row(r, idx, anchor, multi=True) for r in tr.itertuples(index=False)]
    tr["flabel"] = [l[0] for l in labs_all]
    tr.loc[tr["cls"] != "F", "flabel"] = ""
    bins = [(0.005, 0.02, "0.5-2 cm"), (0.02, 0.05, "2-5 cm"), (0.05, 0.10, "5-10 cm"),
            (0.10, 0.25, "10-25 cm"), (0.25, 1.00, "25-100 cm"), (1.00, 1e9, ">100 cm")]
    for name, s in [("F-none", tr[(tr["cls"] == "F") & (tr["flabel"] == "F-none")]),
                    ("R", tr[tr["cls"] == "R"]),
                    ("F (all)", tr[tr["cls"] == "F"])]:
        out.append(sub(f"{name}  (n={len(s)}, sum|d_pb_dc|={s['d_pb_dc'].abs().sum():.3f} m)"))
        out.append(f"{'bin':<12} {'n':>6} {'sum|d_pb_dc|':>13} {'sum|d_pb|':>11} {'share n':>9}")
        v = s["d_pb_dc"].abs()
        for lo, hi, nm in bins:
            m = (v >= lo) & (v < hi)
            out.append(f"{nm:<12} {int(m.sum()):>6} {s.loc[m,'d_pb_dc'].abs().sum():>13.3f} "
                       f"{s.loc[m,'d_pb'].abs().sum():>11.3f} "
                       f"{(100.0*m.sum()/max(len(s),1)):>8.1f}%")

    # --- is F-none isolated (frame-move shaped) or a run (drift-shaped)? ------
    out.append(sub("ISOLATION: a deterministic frame move is ISOLATED; drift past "
                   "the 5 mm dead band RUNS"))
    out.append(f"{'class':<16} {'n':>6} {'isolated':>9} {'iso %':>7} "
               f"{'p50 run_len':>12} {'p90 run_len':>12} {'max':>5}")
    for c in ["C", "R"] + PRIORITY:
        s = tr[tr["cls"] == c] if c in ("C", "R") else \
            tr[(tr["cls"] == "F") & (tr["flabel"] == c)]
        if len(s) == 0:
            continue
        out.append(f"{c:<16} {len(s):>6} {int(s['isolated'].sum()):>9} "
                   f"{100.0*s['isolated'].mean():>6.1f}% {q(s['run_len'],.5):>12.0f} "
                   f"{q(s['run_len'],.9):>12.0f} {int(s['run_len'].max()):>5}")
    out.append(f"{'ALL translated':<16} {len(tr):>6} {int(tr['isolated'].sum()):>9} "
               f"{100.0*tr['isolated'].mean():>6.1f}%")

    # --- does any event (telemetry included) coincide with an F-none cell? ----
    out.append(sub("F-none: coincident tiers INCLUDING the held-out telemetry tiers"))
    fnone = tr[(tr["cls"] == "F") & (tr["flabel"] == "F-none")]
    tc = Counter()
    n_any = 0
    for r in fnone.itertuples(index=False):
        tiers = idx.sat_tow.get((r.sat, snap(r.tow)), frozenset())
        if tiers:
            n_any += 1
        for t_ in tiers:
            tc[t_] += 1
    out.append(f"   F-none cells with ANY same-tow event for that satellite: "
               f"{n_any}/{len(fnone)} ({100.0*n_any/max(len(fnone),1):.1f} %)")
    n_slip_any = sum(1 for r in fnone.itertuples(index=False)
                     if idx.sat_tow.get((r.sat, snap(r.tow)), frozenset()) & set(TIER_SLIP))
    nt_all = d[(~d["translated"]) & d["pair30"]]
    n_slip_null = sum(1 for r in nt_all.itertuples(index=False)
                      if idx.sat_tow.get((r.sat, snap(r.tow)), frozenset()) & set(TIER_SLIP))
    ro = n_slip_any / max(len(fnone), 1)
    rn = n_slip_null / max(len(nt_all), 1)
    out.append(f"   F-none with a slip event for that satellite at ANY station "
               f"(not just its anchor): {n_slip_any} ({100.0*ro:.1f} %) vs a "
               f"non-translated base rate of {100.0*rn:.1f} %  ->  lift "
               f"{(ro/rn if rn>0 else float('nan')):.2f}")
    for t_, n in sorted(tc.items(), key=lambda kv: -kv[1]):
        out.append(f"      {t_:<18} {n:>6}"
                   + ("   (telemetry tier)" if t_ in TIER_TELEMETRY else ""))

    # --- epoch-level (satellite-agnostic) datum events -----------------------
    # A network-wide datum move (for example several satellites decoupling from
    # the clock anchor in one epoch) changes the emitted PB of every satellite,
    # but the producer names only the satellites it acted on.  A per-satellite
    # key therefore under-attributes; this satellite-agnostic view is the upper
    # bound.
    out.append(sub("EPOCH-level datum co-incidence (sat-agnostic upper bound)"))
    epoch_tiers = defaultdict(set)
    for r in e.itertuples(index=False):
        epoch_tiers[r.tow].add(r.tier)
    groups = [("STRANSFORM/PBGAUGE/PHASE_ANCHOR", TIER_STRANSFORM + TIER_PBGAUGE + TIER_PHASE_ANCHOR),
              ("CLK_ANCHOR", TIER_CLK_ANCHOR),
              ("PIVOT/CLK_BASIS/CB_DATUM/RETRO", ("PIVOT", "CLK_BASIS", "CB_DATUM", "RETRO")),
              ("ANY of the above", TIER_STRANSFORM + TIER_PBGAUGE + TIER_PHASE_ANCHOR
               + TIER_CLK_ANCHOR + ("PIVOT", "CLK_BASIS", "CB_DATUM", "RETRO"))]
    out.append(f"{'epoch carries':<34} {'F-none n':>9} {'%':>7} {'sum|d_pb|':>10} "
               f"{'null %':>8} {'lift':>6}")
    for nm, tiers in groups:
        ts = set(tiers)
        m = fnone["tow"].map(lambda t: bool(epoch_tiers.get(snap(t), frozenset()) & ts))
        mn = nt_all["tow"].map(lambda t: bool(epoch_tiers.get(snap(t), frozenset()) & ts))
        ro, rn = float(m.mean()) if len(m) else 0.0, float(mn.mean()) if len(mn) else 0.0
        out.append(f"{nm:<34} {int(m.sum()):>9} {100.0*ro:>6.1f}% "
                   f"{fnone.loc[m,'d_pb'].abs().sum():>10.3f} {100.0*rn:>7.1f}% "
                   f"{(ro/rn if rn>0 else float('nan')):>6.2f}")
    out.append("   NOTE: an epoch-level hit is NOT attribution -- it is the ceiling a "
               "sat-agnostic declaration could reach.")

    out.append(sub("20 largest F-none steps"))
    fn = tr[(tr["cls"] == "F") & (tr["flabel"] == "F-none")].copy()
    fn = fn.reindex(fn["d_pb_dc"].abs().sort_values(ascending=False).index).head(20)
    out.append(f"{'tow':>10} {'sat':<4} {'sig':<5} {'h':>2} {'d_pb':>9} {'d_pb_dc':>9} "
               f"{'d_clk_dc':>9} {'d_cb':>9} {'d_phi':>9} {'anchor prev -> now':<26} "
               f"{'kind':<11} events +/-2ep")
    for r in fn.itertuples(index=False):
        tow = float(r.tow)
        a_p = anchor.get((r.sat, r.sig, tow - 30.0), "-")
        a_n = anchor.get((r.sat, r.sig, tow), "-")
        near = set()
        for dt in (-60.0, -30.0, 0.0, 30.0, 60.0):
            for t_ in idx.sat_tow.get((r.sat, tow + dt), ()):
                if t_ not in TIER_TELEMETRY:
                    near.add(f"{t_}{int(dt/30):+d}")
            for st_ in {a_p, a_n}:
                for t_ in idx.slip_sta.get((r.sat, st_, tow + dt), ()):
                    near.add(f"{t_}@anchor{int(dt/30):+d}")
        out.append(f"{tow:>10.0f} {r.sat:<4} {r.sig:<5} {r.hour:>2} {r.d_pb:>9.4f} "
                   f"{r.d_pb_dc:>9.4f} {r.d_clk_dc:>9.4f} {r.d_cb:>9.4f} {r.d_phi:>9.4f} "
                   f"{(str(a_p)+' -> '+str(a_n)):<26} {r.tr_kind:<11} "
                   f"{','.join(sorted(near)) if near else '(none)'}")

    # ---------------- 4. Q4 -------------------------------------------------
    out.append(hdr("4. Q4  REGRESSION OF d_pb_dc ON d_clk_dc"))
    out.append("slope ~ +1 => redistribution dominates (PB tracks the clock);"
               " slope ~ 0 => PB-only steps.")
    out.append(f"{'population':<34} {'n':>7} {'pearson r':>10} {'slope':>9} "
               f"{'intcpt':>9} {'r^2':>7}")

    def reg(name, s):
        x = s["d_clk_dc"].to_numpy(float)
        y = s["d_pb_dc"].to_numpy(float)
        m = np.isfinite(x) & np.isfinite(y)
        x, y = x[m], y[m]
        if x.size < 3 or np.allclose(x, x[0]):
            out.append(f"{name:<34} {x.size:>7} {'-':>10} {'-':>9} {'-':>9} {'-':>7}")
            return
        r = float(np.corrcoef(x, y)[0, 1])
        A = np.vstack([x, np.ones_like(x)]).T
        sl, ic = np.linalg.lstsq(A, y, rcond=None)[0]
        out.append(f"{name:<34} {x.size:>7} {r:>10.4f} {sl:>9.4f} {ic:>9.5f} {r*r:>7.4f}")

    big = d[d["pair30"] & (d["d_pb_dc"].abs() >= eps)]
    reg("ALL cell-epochs |d_pb_dc|>=eps", big)
    reg("  hours 2-6", big[big["hour"] >= 2])
    reg("  hour 1", big[big["hour"] == 1])
    reg("  translated only", big[big["translated"]])
    reg("  translated, hours 2-6", big[big["translated"] & (big["hour"] >= 2)])
    reg("  non-translated", big[~big["translated"]])
    reg("EVERY 30 s pair (no |d| cut)", d[d["pair30"]])
    for sg in sorted(d["sig"].unique()):
        reg(f"  |d_pb_dc|>=eps, sig={sg}", big[big["sig"] == sg])

    # ---------------- 5. Q5 -------------------------------------------------
    out.append(hdr("5. Q5  SHARES OF THE TRANSLATED sum|d_pb|"))
    tot = tr["d_pb"].abs().sum()
    decl = tr[(tr["cls"] == "F") &
              (tr["flabel"].isin(["F-STRANSFORM", "F-PHASE_ANCHOR", "F-PBGAUGE"]))]
    # multi-label variant: any frame tier present, regardless of priority winner
    frame_multi = tr[(tr["cls"] == "F") & pd.Series(
        [bool(s & {"F-STRANSFORM", "F-PHASE_ANCHOR", "F-PBGAUGE"})
         for s in [label_row(r, idx, anchor, multi=True)[1]
                   for r in tr.itertuples(index=False)]], index=tr.index)]
    unwar = tr[(tr["cls"] == "R") |
               ((tr["cls"] == "F") & (tr["flabel"] == "F-none") &
                (tr["d_pb_dc"].abs() < 0.02))]
    out.append(f"total translated sum|d_pb|                     : {tot:>10.3f} m  (n={len(tr)})")
    out.append(f"declared-frame-only translator would cover     : {decl['d_pb'].abs().sum():>10.3f} m "
               f"({100.0*decl['d_pb'].abs().sum()/max(tot,1e-12):5.1f} %)  n={len(decl)}"
               "   [priority label in {F-STRANSFORM,F-PHASE_ANCHOR,F-PBGAUGE}]")
    out.append(f"   same, ANY frame tier coincident (multi)     : {frame_multi['d_pb'].abs().sum():>10.3f} m "
               f"({100.0*frame_multi['d_pb'].abs().sum()/max(tot,1e-12):5.1f} %)  n={len(frame_multi)}")
    out.append(f"PBT-AUDIT 'unwarranted' (R + F-none < 2 cm)    : {unwar['d_pb'].abs().sum():>10.3f} m "
               f"({100.0*unwar['d_pb'].abs().sum()/max(tot,1e-12):5.1f} %)  n={len(unwar)}")
    rem = tot - decl["d_pb"].abs().sum() - unwar["d_pb"].abs().sum()
    out.append(f"remainder                                     : {rem:>10.3f} m "
               f"({100.0*rem/max(tot,1e-12):5.1f} %)")
    out.append("")
    out.append("   breakdown of the remainder by priority label:")
    covered = set(["F-STRANSFORM", "F-PHASE_ANCHOR", "F-PBGAUGE"])
    for c in ["C"] + PRIORITY:
        if c in covered:
            continue
        s = tr[tr["cls"] == c] if c == "C" else \
            tr[(tr["cls"] == "F") & (tr["flabel"] == c)]
        if c == "F-none":
            s = s[s["d_pb_dc"].abs() >= 0.02]
        if len(s) == 0:
            continue
        out.append(f"      {c:<18} n={len(s):>5}  sum|d_pb|={s['d_pb'].abs().sum():>9.3f} m "
                   f"({100.0*s['d_pb'].abs().sum()/max(tot,1e-12):5.1f} %)")

    # ---------------- 6. robustness -----------------------------------------
    out.append(hdr("6. ROBUSTNESS / INSTRUMENT NOTES"))
    a = tr["d_phi"].abs() < eps
    b = tr["d_phi_alt"].abs() < eps
    out.append(f"de-common variant: 'flat phase correction' agreement between "
               f"d_phi = -(d_clk-med d_clk)+(d_pb-med d_pb) and "
               f"d_phi_alt = (-d_clk+d_pb) - med(-d_clk+d_pb): "
               f"{int((a==b).sum())}/{len(tr)} ({100.0*(a==b).mean():.1f} %)")
    out.append(f"R class under d_phi_alt instead of d_phi: "
               f"{int(((tr['d_pb_dc'].abs()>=eps) & b).sum())} "
               f"(vs {int(((tr['d_pb_dc'].abs()>=eps) & a).sum())} under d_phi)")
    out.append(f"median per-epoch |c_pb| (per signal): "
               f"{q(d.drop_duplicates(['tow','sig'])['c_pb'].abs(),.5):.5f} m ; "
               f"|c_clk|: {q(d.drop_duplicates(['tow'])['c_clk'].abs(),.5):.5f} m")
    out.append(f"p90 per-epoch |c_pb|: "
               f"{q(d.drop_duplicates(['tow','sig'])['c_pb'].abs(),.9):.5f} m ; "
               f"|c_clk|: {q(d.drop_duplicates(['tow'])['c_clk'].abs(),.9):.5f} m")
    out.append(f"wire records parsed from products.txt: {len(wire)}")
    if wire:
        chk = []
        for r in d.sample(min(4000, len(d)), random_state=0).itertuples(index=False):
            k = (r.sat, r.sig, float(r.tow))
            if k in wire:
                chk.append(abs(wire[k] - r.pb_m))
        if chk:
            out.append(f"applied pb_m vs wire P-record, max |diff| over "
                       f"{len(chk)} sampled cells: {max(chk):.3e} m "
                       f"(0 => the consumer applies the same-epoch wire, no lag)")

    txt = "\n".join(out)
    print(txt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
