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

READ-ONLY.  Nothing here writes into a run directory.
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
    ph = {r[0]: r for r in res[("median", "phase", "tracking")]["table"]}
    co = {r[0]: r for r in res[("median", "code", "tracking")]["table"]}
    lv = {r[0]: r for r in res[("level", "clk")]}
    out = []
    for h in hours:
        p, c, l = ph.get(h), co.get(h), lv.get(h)
        out.append(dict(h=h,
                        n_phase=int(p[1]) if p else 0,
                        d_phase_rms=float(p[4]) if p else float("nan"),
                        n_code=int(c[1]) if c else 0,
                        d_code_rms=float(c[4]) if c else float("nan"),
                        clk_level_p50=float(l[2]) if l else float("nan"),
                        phase_steps=int(p[5]) if p else 0,
                        code_steps=int(c[5]) if c else 0))
    return out, None


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
    ("clock time-link (Q3)", re.compile(r"CLK-ANCHOR|clock temporal anchor|cta_", re.I)),
    ("clock step test", re.compile(r"clock step test|CLK_STEP|clock_disc", re.I)),
    ("producer complete", re.compile(r"Network producer complete", re.I)),
    ("SBC", re.compile(r"Network \(SBC\)", re.I)),
    ("observability gate", re.compile(r"observability gate", re.I)),
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
        return json.load(fh), None


# ----------------------------------------------------------------------------
#  printing
# ----------------------------------------------------------------------------
def f(x, w=8, p=3):
    if x is None or (isinstance(x, float) and (math.isnan(x) or math.isinf(x))):
        return " " * (w - 1) + "-"
    return f"{x:{w}.{p}f}"


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
        print("    hour |  n_phase | D_phase m |   n_code | D_code m | clk LEVEL p50 m")
        print("    -----+----------+-----------+----------+----------+----------------")
        for r in bed["wire"]:
            print(f"    {r['h']:4d} | {r['n_phase']:8d} | {f(r['d_phase_rms'], 9)} | "
                  f"{r['n_code']:8d} | {f(r['d_code_rms'], 8)} | {f(r['clk_level_p50'], 15)}")

    print("\n  DIAGNOSTICS — NOT BARS.  Read to explain a delta, never to grade.")
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
        print("    unsignalled wire steps (wire_step_audit): "
              + json.dumps(sa.get("summary", sa))[:600])
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
    wire, werr = (None, "no arm.json / config.json in the run dir")
    if os.path.isfile(seed_cfg):
        try:
            wire, werr = wire_tables(run_dir, seed_cfg, data_root,
                                     name.lower(), n_hours)
        except Exception as exc:                               # noqa: BLE001
            wire, werr = None, f"{type(exc).__name__}: {exc}"
    bed = dict(
        name=name, run_dir=run_dir, consumer_station=station,
        consumer=ctab,
        consumer_t0=t0,
        first_subdecimetre=(dict(tow=fs["tow"], err3d=fs["err3d"],
                                 minutes=(fs["tow"] - t0) / 60.0) if fs and t0 else None),
        resets=dict(n_lines=len(hits), lines=hits[:8], n_step_proxy=nstep),
        wire=wire, wire_error=werr,
        wire_reference="the COD 30 s CLK + COD MGX OSB .BIA"
                       + (" — WHICH IS THIS BED'S TRUTH" if name == "SIM" else ""),
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
