#!/usr/bin/env python3
"""RF-0 — the two instruments of the network re-founding, and NOTHING else.

    python scripts/refound_grade.py --field-run runs/_rf0_field6 --sim-run runs/_rf0_sim6 \
        [--hours 6] [--json runs/_rf0_baseline.json]

docs/refound/NETWORK_REFOUNDING_2026-09-14.md names exactly two bars, and this script
is the only thing that reports them:

  1. THE COLD CONSUMER, per hour.  p50 and p90 of the held-out closed-loop consumer's
     3D error, plus the first epoch at which 3D < 0.1 m HOLDS for 10 consecutive
     epochs, plus resets.  Field: WTZZ00DEU against its ITRF reference; sim:
     SZZ000SIM against truth.  Both read the SAME file and the SAME column —
     `logs/closed_loop_trace.csv` / `pos_err_3d_m` — because that column is already
     the error against `network.closed_loop.station.position_ecef`, and on the sim
     bed that configured position IS the truth position (network_fixture.json /
     scenario_summary.json static_segment.position_ecef).  Read exactly as
     scripts/pv_consumer_read.py reads it, hour-binned from the CONSUMER's own first
     epoch (a consumer that starts late must not be graded on a shifted clock).

  2. THE WIRE AGAINST A REFERENCE, per hour.  scripts/wire_vs_code_census.py's
     within-arc D_phase / D_code rms and the clock LEVEL p50, on BOTH beds against
     the COD 30 s CLK + COD MGX OSB .BIA.

     ⚠ ON THE SIM BED THAT REFERENCE IS TRUTH, NOT A SECOND OPINION.  The fixture
     injects the precise COD CLK product as the satellite clock (there is no
     `truth_satclock.csv` in runs/simnet60_quiet_wu93_24h ⇒ no injected satellite
     clock error; scripts/simnet_grade.py:454-466 states the same contract) and the
     COD MGX OSB .BIA verbatim as the satellite code bias (`truth_bias.csv`
     source=product, osb_ns matching the .BIA to the digit: G03 C1C 7.0592 ns, C2W
     12.8975 ns; scenario_summary.json signal_bias.file).  So one statistic covers
     both beds and on the sim it is an ABSOLUTE grade.

Everything else printed below is a DIAGNOSTIC — read to explain a delta, never a
bar.  Each such block says so in its own header.

The producer is never re-run and no solve artifact is touched; the only files written
are the optional --json summary and wire_step_audit.py's own
`<run>/_rf0_step_audit.json`.
"""
import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys

import numpy as np

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wire_vs_code_census as wvc          # noqa: E402  (path set above)

EPOCH_S = 30.0
SUBDM_M = 0.1
SUBDM_HOLD = 10


# ----------------------------------------------------------------------------
#  instrument 1 — the cold consumer
# ----------------------------------------------------------------------------
def read_consumer(run_dir):
    """-> list of {tow, err3d, e, n, u} in file order, numeric rows only."""
    p = os.path.join(run_dir, "logs", "closed_loop_trace.csv")
    rows = []
    if not os.path.isfile(p):
        return rows
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append(dict(tow=float(r["tow"]), err3d=float(r["pos_err_3d_m"]),
                                 e=float(r["east_m"]), n=float(r["north_m"]),
                                 u=float(r["up_m"])))
            except (TypeError, ValueError, KeyError):
                continue
    return rows


def consumer_resets(run_dir, rows, step_m=1.0):
    """(reset lines from the producer's own voice, |d3D| > step_m step proxy).

    closed_loop_trace.csv has no reset column, so the count is taken from stdout
    exactly as scripts/pv_consumer_read.py takes it, and the step proxy is reported
    beside it and LABELLED a proxy.  A zero that rests only on the absence of a
    channel says so.
    """
    hits = []
    p = os.path.join(run_dir, "stdout.txt")
    if os.path.isfile(p):
        pat = re.compile(r"closed-loop|consumer", re.I)
        rst = re.compile(r"\breset\b|re-?init|restart|re-?seed", re.I)
        with open(p, encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                if pat.search(ln) and rst.search(ln):
                    hits.append(ln.strip()[:160])
    steps = sum(1 for a, b in zip(rows, rows[1:])
                if abs(b["err3d"] - a["err3d"]) > step_m)
    return hits, steps


def first_subdecimetre(rows, thresh=SUBDM_M, hold=SUBDM_HOLD):
    """tow of the first epoch that opens `hold` CONSECUTIVE epochs all < thresh."""
    run = 0
    start = None
    for r in rows:
        if r["err3d"] < thresh:
            if run == 0:
                start = r
            run += 1
            if run >= hold:
                return start
        else:
            run = 0
    return None


def consumer_table(rows, n_hours):
    """per-hour [{h, n, p50, p90, max}] binned from the CONSUMER's own first epoch."""
    if not rows:
        return [], None
    t0 = rows[0]["tow"]
    out = []
    for h in range(n_hours):
        a = np.array([r["err3d"] for r in rows
                      if h * 3600.0 <= r["tow"] - t0 < (h + 1) * 3600.0], dtype=float)
        out.append(dict(h=h, n=int(a.size),
                        p50=float(np.percentile(a, 50)) if a.size else float("nan"),
                        p90=float(np.percentile(a, 90)) if a.size else float("nan"),
                        max=float(a.max()) if a.size else float("nan")))
    return out, t0


# ----------------------------------------------------------------------------
#  instrument 2 — the wire
# ----------------------------------------------------------------------------
def wire_tables(run_dir, seed_cfg_path, data_root, label, n_hours):
    """per-hour within-arc D_phase / D_code rms and the clock LEVEL p50.

    Delegates to scripts/wire_vs_code_census.py (the s24.5 instrument) so the
    statistic, the common-mode estimator and every field offset are ITS source of
    truth, not a second implementation of them here.
    """
    clk_paths, bia_path, tag, _ = wvc.resolve_code_products(seed_cfg_path, data_root)
    if not clk_paths:
        return None, f"no CODE CLK resolved for {tag}"
    if not bia_path:
        return None, f"no CODE OSB .BIA resolved for {tag}"
    clk, n_clk = wvc.parse_clk(clk_paths)
    osb, sigs = wvc.parse_bia(bia_path, tag)
    print(f"\n  [wire reference] {n_clk} GPS AS records for {len(clk)} satellites; "
          f"{len(osb)} GPS OSB entries valid on {tag}; signals {sorted(sigs)}")
    hours = list(range(n_hours))
    res = wvc.run_census(run_dir, clk, osb, label, hours=hours, out_dir=None)
    if res is None:
        return None, "census returned nothing"
    # pooled_table rows: (h, n, p50, p95, rms, n_steps);  level_table: (h, n, p50, p95)
    def tab(kind, tag):
        return {r[0]: r for r in res[("median", kind, tag)]["table"]}
    ph, co, ck = tab("phase", "tracking"), tab("code", "tracking"), tab("clk", "tracking")
    phd, cod, ckd = tab("phase", "declared"), tab("code", "declared"), tab("clk", "declared")
    lv = {r[0]: r for r in res[("level", "clk")]}
    out = []
    for h in hours:
        p, c, k, l = ph.get(h), co.get(h), ck.get(h), lv.get(h)
        pd_, cd_, kd_ = phd.get(h), cod.get(h), ckd.get(h)
        out.append(dict(h=h,
                        n_phase=int(p[1]) if p else 0,
                        d_phase_rms=float(p[4]) if p else float("nan"),
                        n_code=int(c[1]) if c else 0,
                        d_code_rms=float(c[4]) if c else float("nan"),
                        d_clk_rms=float(k[4]) if k else float("nan"),
                        clk_level_p50=float(l[2]) if l else float("nan"),
                        # the SAME statistic re-segmented at the producer's OWN declared
                        # discontinuities — what a consumer that honours the M4 contract
                        # actually integrates across.
                        d_phase_rms_declared=float(pd_[4]) if pd_ else float("nan"),
                        d_code_rms_declared=float(cd_[4]) if cd_ else float("nan"),
                        d_clk_rms_declared=float(kd_[4]) if kd_ else float("nan"),
                        d_phase_p50=float(p[2]) if p else float("nan"),
                        phase_steps=int(p[5]) if p else 0,
                        code_steps=int(c[5]) if c else 0))
    # the tail: which arcs carry the pooled phase sum of squares
    rows = res[("median", "phase", "tracking")]["rows"]
    tot = sum(r["rms"] ** 2 * r["n"] for r in rows) or float("nan")
    worst = sorted(rows, key=lambda r: -r["rms"] * r["rms"] * r["n"])[:8]
    tail = dict(n_arcs=len(rows),
                top8_share=sum(r["rms"] ** 2 * r["n"] for r in worst) / tot,
                worst=[dict(sat=r["sat"], sig=r["sig"], n=r["n"], rms=r["rms"],
                            max_abs=r["max_abs"], nstep=r["nstep"],
                            max_step=r["max_step"]) for r in worst])
    return dict(hours=out, phase_tail=tail), None


# ----------------------------------------------------------------------------
#  diagnostics (NOT BARS)
# ----------------------------------------------------------------------------
def code_rms_table(run_dir, n_hours):
    """DIAGNOSTIC.  Per-hour p50 / max of the per-STATION post-fit code rms
    (network_pf_keyed.csv, kind=STA, rms_code) — the s24.8 (i) read."""
    p = os.path.join(run_dir, "logs", "network_pf_keyed.csv")
    if not os.path.isfile(p):
        return None
    buckets = {h: [] for h in range(n_hours)}
    t0 = None
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if (r.get("kind") or "").strip() != "STA":
                continue
            try:
                t = float(r["tow"]); v = float(r["rms_code"])
            except (TypeError, ValueError):
                continue
            if t0 is None:
                t0 = t
            h = int((t - t0) // 3600)
            if 0 <= h < n_hours:
                buckets[h].append(v)
    out = []
    for h in range(n_hours):
        a = np.array(buckets[h], dtype=float)
        out.append(dict(h=h, n=int(a.size),
                        p50=float(np.percentile(a, 50)) if a.size else float("nan"),
                        max=float(a.max()) if a.size else float("nan")))
    return out


def zwd_table(run_dir, n_hours, bar_m=0.5):
    """DIAGNOSTIC.  Per hour, how many DISTINCT stations hold |ZWD| > bar_m
    (network_zwd.csv) — an unphysical zenith wet delay is the s24.9 tell."""
    p = os.path.join(run_dir, "logs", "network_zwd.csv")
    if not os.path.isfile(p):
        return None
    bad = {h: set() for h in range(n_hours)}
    allsta = set()
    t0 = None
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                t = float(r["tow"]); v = float(r["zwd_m"])
            except (TypeError, ValueError):
                continue
            if t0 is None:
                t0 = t
            allsta.add(r["station"])
            h = int((t - t0) // 3600)
            if 0 <= h < n_hours and abs(v) > bar_m:
                bad[h].add(r["station"])
    return [dict(h=h, n_bad=len(bad[h]), n_sta=len(allsta),
                 worst=sorted(bad[h])[:6]) for h in range(n_hours)]


Q3_PATTERNS = [
    ("producer complete", re.compile(r"Network producer complete")),
    ("Q3 clock time-link", re.compile(r"Card Q3 / guarded temporal clock anchor")),
    ("wire contract / step marks", re.compile(r"Card CLK-R / clock emission re-base")),
    ("leveling vehicle", re.compile(r"leveling-carry retirement census|relevel_free_epoch|"
                                    r"free-epoch")),
    ("phase datum", re.compile(r"phase[- ]datum family|PHASE_ANCHOR|phase anchor")),
    ("iono posture", re.compile(r"Network \(W6\): GIM loaded|SH-iono|estimate_iono_sh is OFF")),
    ("SBC", re.compile(r"Network \(SBC\): (first completion|[0-9])")),
    ("observability gate", re.compile(r"observability gate at TOW")),
]


def run_end_lines(run_dir, max_per=4):
    """DIAGNOSTIC.  The producer's own run-end voice for the armed families."""
    p = os.path.join(run_dir, "stdout.txt")
    out = {}
    if not os.path.isfile(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    for name, pat in Q3_PATTERNS:
        hits = [ln.strip() for ln in lines if pat.search(ln)]
        out[name] = dict(n=len(hits), tail=[h[-400:] for h in hits[-max_per:]])
    return out


def step_audit(runs_root, arm, tow0, python_exe=None):
    """DIAGNOSTIC.  scripts/wire_step_audit.py — the unsignalled-step population."""
    out_json = os.path.join(runs_root, arm, "_rf0_step_audit.json")
    cmd = [python_exe or sys.executable, os.path.join(HERE, "wire_step_audit.py"),
           "--runs", runs_root, "--arms", arm, "--tow0", str(tow0), "--json", out_json]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    except Exception as exc:                                   # noqa: BLE001
        return None, f"wire_step_audit failed to start: {exc}"
    if not os.path.isfile(out_json):
        return None, f"wire_step_audit wrote no json (rc={p.returncode}): {p.stderr[-300:]}"
    with open(out_json, encoding="utf-8") as fh:
        doc = json.load(fh)
    return doc.get(arm, doc), None


# ----------------------------------------------------------------------------
#  printing
# ----------------------------------------------------------------------------
def f(x, w=8, p=3):
    if x is None or (isinstance(x, float) and (math.isnan(x) or math.isinf(x))):
        return " " * (w - 1) + "-"
    return f"{x:{w}.{p}f}"


def if_dispersion(run_dir, t0_tow=259200.0, lo=(3600.0, 7200.0), hi=(18000.0, 21600.0)):
    """Per-satellite dispersion of the IONOSPHERE-FREE correction's drift (a DIAGNOSTIC).

    The quantity a consumer actually applies per satellite is
    ``IF_corr = -clock + a1*b(C1C) + a2*b(C2W)``.  Its NETWORK-COMMON drift is a datum and
    cancels in the consumer's own clock; what a consumer's phase cannot absorb is the
    PER-SATELLITE departure from that common drift.  So this reports the common drift
    between two windows (default hour 2 and hour 6) and the spread of the per-satellite
    deviations around it.

    Introduced by RF-0e, where it separated arms the two bars could not: across RF-0 (GIM),
    RF-0c, RF-0d and RF-0e's two level-datum arms the p90 ran 0.300 / 0.552 / 1.527 /
    1.148 / 1.022 m and tracked the field consumer monotonically, while the common drift
    (-1.2 / -8.1 / -17.2 / -17.1 / -16.9 m) did not.

    Returns None when the product carries no satellite with a clock and both pinned-pair
    code biases spanning both windows.
    """
    f1, f2 = 1575.42e6, 1227.60e6
    den = f1 * f1 - f2 * f2
    a1, a2 = f1 * f1 / den, -f2 * f2 / den
    path = os.path.join(run_dir, "products.txt")
    if not os.path.exists(path):
        return None
    per = {}
    tow = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln.startswith("#"):
                continue
            f = ln.rstrip("\n").split(",")
            if f[0] == "E":
                tow = float(f[2]) if len(f) > 2 else None
            elif f[0] == "C" and tow is not None and len(f) > 2:
                try:
                    per.setdefault(f[1], {}).setdefault(tow, [None, {}])[0] = float(f[2])
                except ValueError:
                    pass
            elif f[0] == "B" and tow is not None and len(f) > 3:
                try:
                    per.setdefault(f[1], {}).setdefault(tow, [None, {}])[1][f[2]] = float(f[3])
                except ValueError:
                    pass

    def win(series, w):
        v = [x for t, x in series if w[0] <= t - t0_tow < w[1]]
        return float(np.mean(v)) if v else None

    drifts = []
    for sat, byt in per.items():
        series = []
        for t in sorted(byt):
            clk, b = byt[t]
            if clk is None or "C1C" not in b or "C2W" not in b:
                continue
            series.append((t, -clk + a1 * b["C1C"] + a2 * b["C2W"]))
        lo_v, hi_v = win(series, lo), win(series, hi)
        if lo_v is not None and hi_v is not None:
            drifts.append((sat, hi_v - lo_v))
    if len(drifts) < 3:
        return None
    common = float(np.median([d for _, d in drifts]))
    dev = np.array([abs(d - common) for _, d in drifts])
    worst = sorted(drifts, key=lambda r: -abs(r[1] - common))[:5]
    return dict(n=len(drifts), common_drift_m=common,
                p50=float(np.percentile(dev, 50)), p90=float(np.percentile(dev, 90)),
                max=float(dev.max()),
                worst=[dict(sat=s, dev_m=d - common) for s, d in worst])


def print_bed(name, bed):
    print("\n" + "=" * 96)
    print(f"  BED: {name}     run {bed['run_dir']}")
    print("=" * 96)

    print("\n  INSTRUMENT 1 — THE COLD CONSUMER (a bar).  "
          f"{bed['consumer_station']} 3D, per hour from the consumer's first epoch.")
    print("    hour |     n |   p50 m |   p90 m |   max m")
    print("    -----+-------+---------+---------+---------")
    for r in bed["consumer"]:
        print(f"    {r['h']:4d} | {r['n']:5d} | {f(r['p50'])} | {f(r['p90'])} | {f(r['max'])}")
    fs = bed["first_subdecimetre"]
    if fs:
        print(f"    first 3D < {SUBDM_M} m held {SUBDM_HOLD} consecutive epochs: "
              f"tow {fs['tow']:.0f}  (+{fs['minutes']:.1f} min from the consumer's first epoch)")
    else:
        print(f"    first 3D < {SUBDM_M} m held {SUBDM_HOLD} consecutive epochs: NEVER in this window")
    print(f"    resets: {bed['resets']['n_lines']} reset/re-init line(s) in the producer's voice"
          f"   |   step proxy (|d3D| > 1 m): {bed['resets']['n_step_proxy']}"
          + ("   [the trace has no reset column — a zero here rests on the absence of a channel]"
             if bed["resets"]["n_lines"] == 0 else ""))

    print("\n  INSTRUMENT 2 — THE WIRE vs "
          f"{bed['wire_reference']} (a bar).  Within-arc D rms and the clock LEVEL, per hour.")
    if bed["wire"] is None:
        print("    (unavailable: " + str(bed["wire_error"]) + ")")
    else:
        print("         |     TRACKING arcs (a whole pass)      |  re-segmented at the "
              "producer's DECLARED  |")
        print("    hour |  n_phase  D_phase   D_code    D_clk  |  D_phase   D_code    "
              "D_clk  | clk LEVEL p50")
        print("    -----+--------------------------------------+-------------------------"
              "-------+--------------")
        for r in bed["wire"]:
            print(f"    {r['h']:4d} | {r['n_phase']:8d} {f(r['d_phase_rms'], 9)}"
                  f"{f(r['d_code_rms'], 9)}{f(r['d_clk_rms'], 9)}  |"
                  f"{f(r['d_phase_rms_declared'], 9)}{f(r['d_code_rms_declared'], 9)}"
                  f"{f(r['d_clk_rms_declared'], 9)}  |{f(r['clk_level_p50'], 14)}")
        t = bed["wire_tail"]
        print(f"    phase tail: {t['top8_share'] * 100:.1f} % of the pooled within-arc "
              f"sum of squares sits in 8 of {t['n_arcs']} tracking arcs —")
        for w in t["worst"][:5]:
            print(f"      {w['sat']} {w['sig']:4s} n={w['n']:4d}  rms {w['rms']:8.3f} m  "
                  f"max|D| {w['max_abs']:8.3f}  steps>0.25 {w['nstep']:3d}  "
                  f"max step {w['max_step']:8.3f} m")

    print("\n  DIAGNOSTICS — NOT BARS.  Read to explain a delta, never to grade.")
    ifd = bed.get("if_dispersion")
    if ifd:
        print("    per-satellite IF-correction dispersion (RF-0e; -clk + a1*b(C1C) + a2*b(C2W),")
        print("      hour 2 -> hour 6).  The COMMON drift is a datum and cancels in the consumer's")
        print("      own clock; the PER-SATELLITE deviation is what its phase cannot absorb:")
        print(f"      n={ifd['n']}  common drift {ifd['common_drift_m']:+.3f} m  |  "
              f"deviation p50 {ifd['p50']:.3f}  p90 {ifd['p90']:.3f}  max {ifd['max']:.3f} m")
        print("      worst: " + "  ".join(f"{w['sat']}{w['dev_m']:+.2f}" for w in ifd["worst"]))
    if bed["code_rms"]:
        print("    per-station post-fit code rms (network_pf_keyed STA):")
        print("      hour |     n |   p50 m |   max m")
        for r in bed["code_rms"]:
            print(f"      {r['h']:4d} | {r['n']:5d} | {f(r['p50'])} | {f(r['max'])}")
    if bed["zwd"]:
        print("    stations with |ZWD| > 0.5 m (network_zwd):")
        for r in bed["zwd"]:
            print(f"      hour {r['h']}: {r['n_bad']} of {r['n_sta']}"
                  + (("   " + " ".join(r["worst"])) if r["worst"] else ""))
    sa = bed.get("step_audit")
    if sa:
        print("    unsignalled wire steps (wire_step_audit; sig = the per-key disc counter "
              "incremented at that epoch):")
        cnt = sa.get("counts", {})
        rows = sa.get("rows", {})
        for kind in ("CLK", "PB", "CB"):
            if kind not in cnt:
                continue
            bits = []
            for bar in ("0.5", "2.0", "10.0"):
                b = cnt[kind].get(bar, {})
                bits.append(f">{bar} m: {b.get('true', 0)} sig / {b.get('false', 0)} UNSIG")
            print(f"      {kind:3s} ({rows.get(kind, 0)} rows)  " + "   ".join(bits))
        hr = sa.get("hourly", {})
        if hr:
            print("      per hour, UNSIGNALLED > 2 m: " + "  ".join(
                f"h{h}={hr[h].get(k, {}).get('2.0', {}).get('false', 0)}({k})"
                for h in sorted(hr, key=lambda x: int(x)) for k in ("CLK", "PB")))
    elif bed.get("step_audit_error"):
        print("    unsignalled wire steps: " + bed["step_audit_error"])
    for k, v in (bed.get("run_end") or {}).items():
        if v["n"]:
            print(f"    [{k}] {v['n']} line(s); last:")
            for t in v["tail"]:
                print("       " + t)


# ----------------------------------------------------------------------------
def grade_bed(name, run_dir, station, n_hours, runs_root, data_root, do_steps):
    rows = read_consumer(run_dir)
    ctab, t0 = consumer_table(rows, n_hours)
    fs = first_subdecimetre(rows)
    hits, nstep = consumer_resets(run_dir, rows)
    seed_cfg = os.path.join(run_dir, "arm.json")
    if not os.path.isfile(seed_cfg):
        seed_cfg = os.path.join(run_dir, "config.json")
    wire, werr, tail = None, "no arm.json / config.json in the run dir", None
    if os.path.isfile(seed_cfg):
        try:
            got, werr = wire_tables(run_dir, seed_cfg, data_root, name.lower(), n_hours)
            if got:
                wire, tail = got["hours"], got["phase_tail"]
        except Exception as exc:                               # noqa: BLE001
            wire, werr = None, f"{type(exc).__name__}: {exc}"
    bed = dict(
        name=name, run_dir=run_dir, consumer_station=station,
        consumer=ctab,
        consumer_t0=t0,
        first_subdecimetre=(dict(tow=fs["tow"], err3d=fs["err3d"],
                                 minutes=(fs["tow"] - t0) / 60.0) if fs and t0 else None),
        resets=dict(n_lines=len(hits), lines=hits[:8], n_step_proxy=nstep),
        wire=wire, wire_error=werr, wire_tail=tail,
        wire_reference="the COD 30 s CLK + COD MGX OSB .BIA"
                       + (" — WHICH IS THIS BED'S TRUTH" if name == "SIM" else ""),
        if_dispersion=if_dispersion(run_dir),
        code_rms=code_rms_table(run_dir, n_hours),
        zwd=zwd_table(run_dir, n_hours),
        run_end=run_end_lines(run_dir),
    )
    if do_steps:
        arm = os.path.basename(os.path.normpath(run_dir))
        tow0 = rows[0]["tow"] if rows else 259200.0
        sa, serr = step_audit(runs_root, arm, tow0)
        bed["step_audit"], bed["step_audit_error"] = sa, serr
    return bed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--field-run", default="C:/GnssProcessor/runs/_rf0_field6")
    ap.add_argument("--sim-run", default="C:/GnssProcessor/runs/_rf0_sim6")
    ap.add_argument("--field-station", default="WTZZ00DEU")
    ap.add_argument("--sim-station", default="SZZ000SIM")
    ap.add_argument("--hours", type=int, default=6)
    ap.add_argument("--runs-root", default="C:/GnssProcessor/runs")
    ap.add_argument("--data-root", default=None,
                    help="override files.internal_data_root when resolving the CODE products")
    ap.add_argument("--no-step-audit", action="store_true")
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    beds = []
    for nm, rd, st in (("FIELD", a.field_run, a.field_station),
                       ("SIM", a.sim_run, a.sim_station)):
        if not os.path.isdir(rd):
            print(f"!! {nm}: {rd} is not a directory — skipped")
            continue
        beds.append(grade_bed(nm, rd, st, a.hours, a.runs_root, a.data_root,
                              not a.no_step_audit))

    for bed in beds:
        print_bed(bed["name"], bed)

    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(dict(card="RF-0",
                           doc="docs/refound/NETWORK_REFOUNDING_2026-09-14.md",
                           bars=["cold consumer 3D per hour",
                                 "wire vs reference per hour"],
                           hours=a.hours, beds=beds), fh, indent=1, default=str)
        print(f"\nwrote {a.json}")


if __name__ == "__main__":
    main()
