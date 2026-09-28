#!/usr/bin/env python3
"""R-CL, the clock-level read: does the ramp reset at a basis handover, or run through it?

Card: docs/refound/RF-2_held_basis_2026-09-15.md (RF-2b item c).

    python scripts/clock_level_read.py <run_dir> [<run_dir> ...] [--window 10] [--json out.json]

The question, and why it is the one that matters. RF-2 held each satellite's phase-bias
datum as a constant and the field's within-arc phase wire crossed every bar (D_phase
0.22-0.90x RF-0, from 9-115 m under RF-1). But the consumer kept degrading, and the
clock level against COD ramped monotonically: 0.70 m at RF-0 to 1.22 / 1.41 / 1.50 /
1.67 / 2.23 / 2.29 m per hour. Two readings, and they point at different cards:

  * the level resets at handovers => the held value carries the basis station's own bias
    at election. Each new tenure stamps in a fresh bias and the clock inherits it; the
    ramp is a staircase of tenures, not a drift. Lever: the ledger screen (RF-2b's
    composed rank) and a higher `phase_anchor_min_age_ep`;
  * the level runs through handovers => the clock has lost the level datum that the
    churning phase bias used to supply. Before RF-2 the datum moved constantly, and that
    motion was (accidentally) re-referencing the clock level every few epochs; freezing
    it took the level anchor away. Lever: the clock needs its own level datum, a
    per-constellation clock level row, or the Q3 anchor in a gauge-free mode.

The statistic. Per satellite, per `--window` epochs: the median of
(emitted clock - COD clock) in metres, de-meaned across the satellites of that window so
the network-common clock datum (a free gauge that is never graded raw, standing invariant
#5) cancels. What is left is the per-satellite level relative to the constellation, which
is datum-invariant and is exactly what a consumer sees.

The join. Beside each satellite's level series, that satellite's basis tenure from the
event log: PBGAUGE / STRANSFORM rows give the handover epochs, the incoming station and
whether the handover was continuous or cold. The test is then a paired comparison:

  |level(t+) - level(t-)| at handover epochs   vs   |level(t+) - level(t-)| at matched
                                                    non-handover epoch boundaries

A reset shows as a step concentrated at handovers (the ratio >> 1); a drift shows as the
same step size either side (ratio ~ 1) with a nonzero mean slope through them. The
non-handover control is size-matched per satellite, because a satellite with many
handovers also has many epochs.

Also reported: the Q3 clock-anchor decouples (`Q3` / anchor trip rows in the event log),
because an anchor trip is itself a level event and must not be read as a handover's.
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

C_LIGHT = 299792458.0


# ----------------------------------------------------------------- COD clock
def civil_to_gps_seconds(y, mo, d, h, mi, s):
    """Calendar UTC -> continuous GPS seconds (the CLK 'AS' record stamp)."""
    a = (14 - mo) // 12
    yy = y + 4800 - a
    mm = mo + 12 * a - 3
    jdn = d + (153 * mm + 2) // 5 + 365 * yy + yy // 4 - yy // 100 + yy // 400 - 32045
    gps_epoch_jdn = 2444245          # 1980-01-06
    days = jdn - gps_epoch_jdn
    return days * 86400.0 + h * 3600.0 + mi * 60.0 + s


def parse_clk(paths, systems=("G",)):
    """RINEX CLK 'AS' records -> {sat: {t_gps_s(int): bias_s}} (ProductParser's own rule)."""
    clk = defaultdict(dict)
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if not line.startswith("AS "):
                    continue
                f = line.split()
                if len(f) < 10 or f[1][0] not in systems:
                    continue
                t = civil_to_gps_seconds(int(f[2]), int(f[3]), int(f[4]),
                                         int(f[5]), int(f[6]), float(f[7]))
                clk[f[1]][int(round(t))] = float(f[9])
    return clk


_sorted_keys: dict = {}


def clk_lookup(clk, sat, t):
    """Exact node, else linear inside a 65 s bracket (GnssCorrections::interpolateClock)."""
    d = clk.get(sat)
    if not d:
        return None
    v = d.get(t)
    if v is not None:
        return v
    keys = _sorted_keys.get(sat)
    if keys is None:
        keys = sorted(d.keys())
        _sorted_keys[sat] = keys
    lo, hi = 0, len(keys)
    while lo < hi:
        mid = (lo + hi) // 2
        if keys[mid] < t:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0 or lo >= len(keys):
        return None
    t0, t1 = keys[lo - 1], keys[lo]
    if t1 - t0 > 65:
        return None
    w = (t - t0) / (t1 - t0)
    return d[t0] * (1.0 - w) + d[t1] * w


def resolve_cod(run_dir, data_root=None):
    cfg_path = os.path.join(run_dir, "arm.json")
    if not os.path.isfile(cfg_path):
        cfg_path = os.path.join(run_dir, "smoke.json")
    cfg = {}
    if os.path.isfile(cfg_path):
        with open(cfg_path, encoding="utf-8") as fh:
            cfg = json.load(fh)
    root = data_root or cfg.get("files", {}).get("internal_data_root") \
        or r"C:/GnssProcessor/data"
    dl = cfg.get("data_download", {})
    year, doy = int(dl.get("year", 2024)), int(dl.get("doy", 129))
    src, typ = dl.get("product_source", "COD"), dl.get("product_type", "FIN")
    paths = []
    for d in (doy - 1, doy, doy + 1):
        paths += sorted(glob.glob(os.path.join(
            root, "%d-%03d" % (year, d), "products",
            "%s0MGX%s_*_30S_CLK.CLK" % (src, typ))))
    return paths


# ------------------------------------------------------------- the run's wire
def read_clock_series(run_dir):
    """{sat: {tow: emitted clock (m)}} from logs/network_solution.csv CLK rows."""
    p = os.path.join(run_dir, "logs", "network_solution.csv")
    out = defaultdict(dict)
    week = None
    if not os.path.isfile(p):
        return out, None
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(ln for ln in fh if not ln.startswith("#")):
            if r.get("kind") != "CLK":
                continue
            try:
                out[r["sat"]][float(r["tow"])] = float(r["value_m"])
                week = int(r["week"])
            except (TypeError, ValueError, KeyError):
                continue
    return out, week


def read_events(run_dir):
    """Handover epochs per satellite, plus the Q3 anchor trips, from logs/network_events.csv."""
    p = os.path.join(run_dir, "logs", "network_events.csv")
    hand = defaultdict(list)     # sat -> [(tow, station, cold, amb)]
    anchor = defaultdict(list)   # sat -> [tow] (Q3 clock-anchor decouple / trip)
    if not os.path.isfile(p):
        return hand, anchor
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(ln for ln in fh if not ln.startswith("#")):
            tier, ev = r.get("tier", ""), r.get("event", "")
            amb, sta = r.get("sat", ""), r.get("station", "")
            sat = amb.split("_")[0] if amb else ""
            try:
                tow = float(r.get("tow", ""))
            except (TypeError, ValueError):
                continue
            if tier in ("PBGAUGE", "STRANSFORM") and ("handover" in ev or "sts_" in ev):
                cold = ("cold" in ev) or ("re_realize" in (r.get("action") or ""))
                if sat:
                    hand[sat].append((tow, sta, cold, amb))
            elif tier in ("Q3", "CLKANCHOR") or "anchor" in ev.lower():
                if sat:
                    anchor[sat].append(tow)
    return hand, anchor


# ------------------------------------------------------------------ the read
def med(xs):
    if not xs:
        return float("nan")
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def analyse(run_dir, window, data_root=None):
    wire, week = read_clock_series(run_dir)
    if not wire or week is None:
        return {"run": run_dir, "error": "no CLK rows in logs/network_solution.csv"}
    clk = parse_clk(resolve_cod(run_dir, data_root))
    if not clk:
        return {"run": run_dir, "error": "no COD CLK product resolved"}
    hand, anchor = read_events(run_dir)

    # Per epoch: the raw (emitted - COD) residual per satellite, then de-meaned across
    # that epoch's satellites.  The network-common clock datum is a free gauge; grading
    # it raw would confound a benign frame move with a per-satellite level error
    # (standing invariant #5).
    tows = sorted({t for s in wire.values() for t in s})
    lev = defaultdict(dict)          # sat -> {tow: de-meaned level (m)}
    for tow in tows:
        t_abs = int(round(week * 604800.0 + tow))
        row = {}
        for sat, series in wire.items():
            v = series.get(tow)
            if v is None:
                continue
            c = clk_lookup(clk, sat, t_abs)
            if c is None:
                continue
            row[sat] = v - c * C_LIGHT
        if len(row) < 4:
            continue
        m = med(list(row.values()))
        for sat, r in row.items():
            lev[sat][tow] = r - m

    # Window medians, then the step between consecutive windows.
    t0 = tows[0]
    steps = defaultdict(list)        # sat -> [(tow_boundary, |step|)]
    wmed = {}
    for sat, series in lev.items():
        by_w = defaultdict(list)
        for tow, v in series.items():
            by_w[int((tow - t0) // (30.0 * window))].append(v)
        ws = sorted(by_w)
        wmed[sat] = {w: med(by_w[w]) for w in ws}
        for a, b in zip(ws, ws[1:]):
            if b - a != 1:
                continue
            steps[sat].append((t0 + b * 30.0 * window,
                               abs(wmed[sat][b] - wmed[sat][a])))

    # Split each satellite's window boundaries into handover and control, size-matched
    # per satellite (a satellite with many handovers also has many epochs).
    at_h, at_c, at_h_cold, at_h_cont = [], [], [], []
    per_sat = {}
    for sat, sl in steps.items():
        hts = [h[0] for h in hand.get(sat, [])]
        colds = {h[0] for h in hand.get(sat, []) if h[2]}
        hh, cc = [], []
        for btow, v in sl:
            near = [t for t in hts if abs(t - btow) <= 30.0 * window]
            if near:
                hh.append(v)
                (at_h_cold if any(t in colds for t in near) else at_h_cont).append(v)
            else:
                cc.append(v)
        at_h += hh
        at_c += cc
        if hh and cc:
            per_sat[sat] = {"n_h": len(hh), "n_c": len(cc),
                            "med_h": med(hh), "med_c": med(cc),
                            "n_handovers": len(hts)}
    # The through-handover drift: the net level change over the run, per satellite.
    drift = {}
    for sat, wm in wmed.items():
        ws = sorted(wm)
        if len(ws) >= 4:
            drift[sat] = wm[ws[-1]] - wm[ws[0]]

    mh, mc = med(at_h), med(at_c)
    return {
        "run": run_dir, "window_ep": window,
        "n_sats": len(lev), "n_windows": sum(len(v) for v in wmed.values()),
        "step_med_at_handover_m": mh,
        "step_med_control_m": mc,
        "ratio": (mh / mc) if (mc and not math.isnan(mc) and mc > 0) else float("nan"),
        "n_at_handover": len(at_h), "n_control": len(at_c),
        "step_med_at_cold_m": med(at_h_cold), "n_cold": len(at_h_cold),
        "step_med_at_continuous_m": med(at_h_cont), "n_cont": len(at_h_cont),
        "net_drift_med_m": med(list(drift.values())),
        "net_drift_p90_m": (sorted(abs(v) for v in drift.values())
                            [int(0.9 * (len(drift) - 1))] if drift else float("nan")),
        "n_handovers_total": sum(len(v) for v in hand.values()),
        "hours": (tows[-1] - tows[0]) / 3600.0 if len(tows) > 1 else 0.0,
        "handovers_per_h": (sum(len(v) for v in hand.values())
                            / max((tows[-1] - tows[0]) / 3600.0, 1e-9)),
        "n_anchor_trips": sum(len(v) for v in anchor.values()),
        "per_sat": per_sat,
    }


def verdict(r):
    """The card's two readings, decided on the ratio alone.

    The verdict deliberately does not use `net_drift_med_m`.  That statistic is
    arc-confounded: a satellite that sets and rises re-enters with a legitimately
    different ambiguity-absorbed level, so the net change over a run measures arc
    turnover as much as datum drift, measured identically (−20.1 m vs −21.4 m median)
    on the RF-0 baseline and the RF-2 arm, which is exactly what a statistic that cannot
    discriminate looks like.  It is printed because it bounds the channel, not because it
    decides anything.

    The discriminating pair is the |level step| at a handover boundary against the same
    run's non-handover boundaries, plus the handover rate.  A reset shows as a step
    concentrated at handovers (ratio >> 1); a level that moves anyway shows the same step
    either side (ratio ~ 1), and then the arm's level cost is the rate, not the event.
    """
    if "error" in r:
        return r["error"]
    ratio = r["ratio"]
    if math.isnan(ratio) or r["n_control"] < 20:
        return "too few control boundaries to separate a reset from a drift"
    if ratio >= 2.0:
        return ("RESETS at handovers (|step| ratio %.2f, %d handovers) => the held value "
                "carries the basis station's own bias at election; the levers are the "
                "ledger screen and a higher phase_anchor_min_age_ep"
                % (ratio, r["n_handovers_total"]))
    if ratio <= 1.5:
        return ("RUNS THROUGH handovers (|step| ratio %.2f — a handover boundary is not "
                "distinguishable from any other) => the clock LEVEL moves on its own, so "
                "the arm's level cost is the handover RATE (%d in this run), not the "
                "event; the clock needs its OWN level datum (a per-constellation clock "
                "level row, or the anchor gauge-free)"
                % (ratio, r["n_handovers_total"]))
    return ("MIXED (|step| ratio %.2f over %d handovers) — handovers contribute but do "
            "not dominate; read the per-satellite split before choosing a lever"
            % (ratio, r["n_handovers_total"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--window", type=int, default=10, help="epochs per level window")
    ap.add_argument("--data-root", default=None)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    out = []
    for run in a.runs:
        r = analyse(run, a.window, a.data_root)
        out.append(r)
        print("== %s" % run)
        if "error" in r:
            print("   ERROR: %s" % r["error"])
            continue
        print("   %d satellites, %d level windows of %d epochs; %d handovers, %d anchor trips"
              % (r["n_sats"], r["n_windows"], r["window_ep"],
                 r["n_handovers_total"], r["n_anchor_trips"]))
        print("   |level step| median AT HANDOVER %8.4f m (n=%d)   CONTROL %8.4f m (n=%d)"
              "   ratio %.2f" % (r["step_med_at_handover_m"], r["n_at_handover"],
                                 r["step_med_control_m"], r["n_control"], r["ratio"]))
        print("      of the handover boundaries: cold %8.4f m (n=%d), continuous %8.4f m (n=%d)"
              % (r["step_med_at_cold_m"], r["n_cold"],
                 r["step_med_at_continuous_m"], r["n_cont"]))
        print("   net level change over the run (⚠ ARC-CONFOUNDED — bounds the channel, "
              "decides nothing): median %+.4f m, |p90| %.4f m"
              % (r["net_drift_med_m"], r["net_drift_p90_m"]))
        print("   handover RATE %.1f / h over %.2f h"
              % (r["handovers_per_h"], r["hours"]))
        print("   VERDICT: %s" % verdict(r))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2)
        print("\n[json] %s" % a.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
