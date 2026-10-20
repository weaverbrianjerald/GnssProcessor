#!/usr/bin/env python3
"""eval_speedcheck.py: consolidate and grade the _net305_gpsgal_sh_n15 duration sweep.

Walks the run dirs produced by run_chain.ps1 (1ep / 3ep / 10ep / 10min), each a
short slice of the same 305-station GPS+Galileo SH-VTEC network producer run, and
answers the two questions the sweep was built for:

  (A) Where the time goes: separates fixed startup (product load, 305-station
      RINEX parse, estimator construction) from the marginal per-epoch network
      solve, by regressing wall-time and the engine's own epoch-loop timer against
      the processed-epoch count.

  (B) Whether the implementations agree: per-epoch SH-iono field sanity (a00 mean
      VTEC and coefficient power), clock/gate/estimability health from
      network_trace.csv, the producer emit/skip ledger, and a cross-run
      prefix-determinism check: because every run shares the same leading epochs,
      epoch k must produce byte-for-byte identical a00 / clock-rms / emit counts in
      every run that reaches it. A drift there is an implementation or determinism
      bug.

Usage:  python scripts/eval_speedcheck.py [run_root]
        run_root defaults to runs/sh_n15_speedcheck
"""
from __future__ import annotations

import csv
import json
import math
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RUN_ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "runs" / "sh_n15_speedcheck"

# suffix -> nominal label + expected processed-epoch count
RUNS = [("1ep", "1-epoch", 1), ("3ep", "3-epoch", 3),
        ("10ep", "10-epoch", 10), ("10min", "10-minute", 20)]

A00_PHYSICAL_MAX = 60.0     # |mean VTEC| beyond this is treated as non-physical / divergent

# --- run.log regexes -------------------------------------------------------
RE_TIMING = re.compile(
    r"Network\[timing\]: epoch-loop wall ([\d.]+) ms over (\d+) epochs = ([\d.]+) ms/epoch")
RE_COMPLETE = re.compile(
    r"Network producer complete: (\d+) epochs emitted, (\d+) skipped "
    r"\((\d+) of those withheld by the observability gate, (\d+) by the A4[^)]*\) "
    r"— of (\d+) synced")
RE_EMIT = re.compile(
    r"Network: TOW ([\d.]+) emitted epoch (\d+) — (\d+) stations, (\d+) sat clocks, "
    r"(\d+) phase biases, (\d+) code biases .*? datum dim (\d+), clock-Δ rms ([\-\d.]+) m")
RE_A00 = re.compile(r"Network\[SH-iono\]:\s+mean VTEC \(a00\) = ([\-\d.]+) TECU over (\d+) coeffs")
# The reduced-normals diagnostic reports the (symmetric-PSD) eigenvalue
# spectrum ("ev_max"/"ev<cutoff"); older logs used the equivalent SVD wording
# ("sv_max"/"sv<cutoff"). Accept either token so historical and current logs
# both parse: eigenvalue and singular value are identical here.
RE_ESTIM = re.compile(
    r"Network\[estimability\]: TOW ([\d.]+) reduced_N_ss (\d+)x\d+, declared_dim=(\d+), "
    r"[se]v_max=([\d.eE+\-]+), cutoff=([\d.eE+\-]+), (\d+) [se]v<cutoff")


def parse_run_log(p: Path) -> dict:
    d = {"timing": None, "complete": None, "emits": [], "a00": [], "estim": []}
    if not p.is_file():
        return d
    txt = p.read_text(encoding="utf-8", errors="replace")
    m = RE_TIMING.search(txt)
    if m:
        d["timing"] = {"loop_ms": float(m[1]), "n_epochs": int(m[2]),
                       "ms_per_epoch": float(m[3])}
    m = RE_COMPLETE.search(txt)
    if m:
        d["complete"] = {"n_emitted": int(m[1]), "n_skipped": int(m[2]),
                         "n_gate_withheld": int(m[3]), "n_sigma0_withheld": int(m[4]),
                         "n_synced": int(m[5])}
    for m in RE_EMIT.finditer(txt):
        d["emits"].append({"tow": float(m[1]), "epoch": int(m[2]), "n_sta": int(m[3]),
                           "n_clk": int(m[4]), "n_pb": int(m[5]), "n_cb": int(m[6]),
                           "datum_dim": int(m[7]), "clk_rms_m": float(m[8])})
    for m in RE_A00.finditer(txt):
        d["a00"].append({"a00_tecu": float(m[1]), "n_coeff": int(m[2])})
    for m in RE_ESTIM.finditer(txt):
        d["estim"].append({"tow": float(m[1]), "reduced_N": int(m[2]),
                           "declared_dim": int(m[3]), "sv_max": float(m[4]),
                           "cutoff": float(m[5]), "n_below": int(m[6])})
    return d


def parse_trace(p: Path) -> list[dict]:
    rows = []
    if not p.is_file():
        return rows
    with p.open(newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "tow": float(r["tow"]),
                    "n_present": int(r["n_present"]),
                    "declared_dim": int(r["declared_dim"]),
                    "gate_ok": int(r["gate_ok"]),
                    "solved": int(r["solved"]),
                    "rms_clock_m": float(r["rms_clock_m"]),
                    "max_clock_m": float(r["max_clock_m"]),
                })
            except (KeyError, ValueError):
                pass
    return rows


def parse_vtec_sh(p: Path) -> dict:
    """tow -> {(n,m,trig): value}. Also returns per-tow a00/max|c|/L2."""
    by_epoch = {}
    if not p.is_file():
        return {}
    with p.open(newline="") as f:
        for r in csv.DictReader(f):
            try:
                tow = float(r["tow"]); n = int(r["n"]); m = int(r["m"])
                val = float(r["value_tecu"])
            except (KeyError, ValueError):
                continue
            by_epoch.setdefault(tow, {})[(n, m, r["trig"])] = val
    out = {}
    for tow, c in by_epoch.items():
        fin = {k: v for k, v in c.items() if math.isfinite(v)}
        a00 = c.get((0, 0, "C"), float("nan"))
        if fin:
            amax_k = max(fin, key=lambda k: abs(fin[k]))
            amax = fin[amax_k]
            l2 = math.sqrt(sum(v * v for v in fin.values()))
        else:
            amax_k, amax, l2 = None, float("nan"), float("nan")
        out[tow] = {"a00": a00, "max_abs": abs(amax), "argmax": amax_k, "l2": l2,
                    "n_nan": len(c) - len(fin), "n_coeff": len(c)}
    return out


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    runs = {}
    for suf, label, nep in RUNS:
        rd = RUN_ROOT / suf
        wall_txt = (rd / "wall.txt").read_text(encoding="utf-8").strip() if (rd / "wall.txt").is_file() else ""
        mwall = re.search(r"wall_s=([\d.]+)", wall_txt)
        mexit = re.search(r"exit=(\d+)", wall_txt)
        runs[suf] = {
            "label": label, "expected_epochs": nep,
            "wall_s": float(mwall[1]) if mwall else None,
            "exit": int(mexit[1]) if mexit else None,
            "log": parse_run_log(rd / "run.log"),
            "trace": parse_trace(rd / "logs" / "network_trace.csv"),
            "vtec": parse_vtec_sh(rd / "logs" / "network_vtec_sh.csv"),
        }

    # ================= (A) timing / scaling =================
    print("=" * 92)
    print("(A)  TIMING  — fixed startup vs marginal per-epoch network solve")
    print("=" * 92)
    print(f"  {'run':<8}{'exit':>5}{'epochs':>8}{'wall_s':>9}{'loop_s':>9}"
          f"{'startup_s':>11}{'ms/epoch(loop)':>16}")
    pts = []  # (n_epochs, wall_s, loop_s)
    for suf, _, _ in RUNS:
        r = runs[suf]; t = r["log"]["timing"]
        loop_s = (t["loop_ms"] / 1000.0) if t else float("nan")
        nep = t["n_epochs"] if t else (r["log"]["complete"] or {}).get("n_emitted", 0)
        startup = (r["wall_s"] - loop_s) if (r["wall_s"] and t) else float("nan")
        mspe = t["ms_per_epoch"] if t else float("nan")
        print(f"  {suf:<8}{r['exit']!s:>5}{nep:>8}{r['wall_s']!s:>9}"
              f"{loop_s:>9.1f}{startup:>11.1f}{mspe:>16.1f}")
        if t and r["wall_s"]:
            pts.append((nep, r["wall_s"], loop_s))

    # least-squares fit wall = a + b*N  and  loop = c + d*N
    def linfit(xs, ys):
        n = len(xs); sx = sum(xs); sy = sum(ys)
        sxx = sum(x * x for x in xs); sxy = sum(x * y for x, y in zip(xs, ys))
        den = n * sxx - sx * sx
        if abs(den) < 1e-12:
            return float("nan"), float("nan")
        b = (n * sxy - sx * sy) / den
        a = (sy - b * sx) / n
        return a, b
    if len(pts) >= 2:
        Ns = [p[0] for p in pts]
        a_w, b_w = linfit(Ns, [p[1] for p in pts])
        a_l, b_l = linfit(Ns, [p[2] for p in pts])
        print(f"\n  wall(N) ≈ {a_w:6.1f} s startup + {b_w:5.2f} s/epoch")
        print(f"  loop(N) ≈ {a_l:6.1f} s          + {b_l:5.2f} s/epoch   (engine timer, solve only)")
        print(f"  => fixed 305-sta startup ≈ {a_w:.0f} s; steady-state solve ≈ {b_l:.1f} s/epoch")
        # cold-start premium: epoch-1 loop vs steady slope
        r1 = runs["1ep"]["log"]["timing"]
        if r1:
            print(f"  => epoch-1 (cold) loop = {r1['loop_ms']/1000:.1f} s  "
                  f"vs steady {b_l:.1f} s/epoch  (cold-start premium ≈ {r1['loop_ms']/1000 - b_l:+.1f} s)")

    # ================= (B) correctness =================
    print("\n" + "=" * 92)
    print("(B)  CORRECTNESS  — per-epoch health of the LONGEST run (10min / 20 epochs)")
    print("=" * 92)
    r = runs["10min"]
    comp = r["log"]["complete"] or {}
    print(f"  producer ledger: emitted={comp.get('n_emitted')}  skipped={comp.get('n_skipped')}  "
          f"gate_withheld={comp.get('n_gate_withheld')}  sigma0_withheld={comp.get('n_sigma0_withheld')}  "
          f"synced={comp.get('n_synced')}")
    print(f"\n  {'tow':>9}{'n_pres':>7}{'dim':>5}{'gate':>5}{'solv':>5}"
          f"{'rms_clk_m':>11}{'max_clk_m':>11}{'a00_TECU':>10}{'max|c|':>9}{'L2':>9}{'nan':>5}")
    trace_by_tow = {row["tow"]: row for row in r["trace"]}
    for tow in sorted(set(list(trace_by_tow) + list(r["vtec"]))):
        tr = trace_by_tow.get(tow, {})
        v = r["vtec"].get(tow, {})
        print(f"  {tow:>9.0f}{tr.get('n_present',''):>7}{tr.get('declared_dim',''):>5}"
              f"{tr.get('gate_ok',''):>5}{tr.get('solved',''):>5}"
              f"{tr.get('rms_clock_m',float('nan')):>11.3f}{tr.get('max_clock_m',float('nan')):>11.3f}"
              f"{v.get('a00',float('nan')):>10.2f}{v.get('max_abs',float('nan')):>9.2f}"
              f"{v.get('l2',float('nan')):>9.2f}{v.get('n_nan',''):>5}")

    # SH power spectrum @ last epoch of the longest run
    if r["vtec"]:
        last = max(r["vtec"])
        # need raw coeffs for per-degree; re-read
        raw = {}
        p = RUN_ROOT / "10min" / "logs" / "network_vtec_sh.csv"
        with p.open(newline="") as f:
            for row in csv.DictReader(f):
                if float(row["tow"]) == last:
                    try:
                        raw.setdefault(int(row["n"]), []).append(float(row["value_tecu"]))
                    except ValueError:
                        pass
        print(f"\n  SH per-degree RMS power @ last epoch tow={last:.0f} (physical: Kaula-like decay):")
        Nmax = max(raw) if raw else 0
        for n in range(Nmax + 1):
            vals = [x for x in raw.get(n, []) if math.isfinite(x)]
            rmsn = math.sqrt(sum(x * x for x in vals) / len(vals)) if vals else 0.0
            bar = "#" * min(60, int(rmsn))
            print(f"    n={n:2d}: rms={rmsn:8.2f} TECU  {bar}")

    # ================= (C) cross-run prefix determinism =================
    print("\n" + "=" * 92)
    print("(C)  DETERMINISM  — epoch k must be identical in every run that reaches it")
    print("=" * 92)
    # Build per-run tow->(a00, clk_rms, n_emit-per-epoch stats) from the log emits + vtec
    def per_run_epoch_map(suf):
        r = runs[suf]
        # a00 from run.log (ordered per emitted epoch), paired with emit tow
        emap = {}
        # emit lines and a00 lines are interleaved 1:1 per epoch in order
        for e, a in zip(r["log"]["emits"], r["log"]["a00"]):
            emap[e["tow"]] = {"a00_log": a["a00_tecu"], "clk_rms": e["clk_rms_m"],
                              "n_sta": e["n_sta"], "n_clk": e["n_clk"],
                              "n_pb": e["n_pb"], "n_cb": e["n_cb"], "dim": e["datum_dim"]}
        # merge trace clock rms + vtec a00
        for row in r["trace"]:
            emap.setdefault(row["tow"], {})["clk_rms_trace"] = row["rms_clock_m"]
        for tow, v in r["vtec"].items():
            emap.setdefault(tow, {})["a00_vtec"] = v["a00"]
        return emap
    maps = {suf: per_run_epoch_map(suf) for suf, _, _ in RUNS}
    ref = "10min"
    print(f"  reference = {ref}; comparing each shorter run's shared epochs against it")
    print(f"  {'tow':>9}  {'field':<12}{'1ep':>14}{'3ep':>14}{'10ep':>14}{'10min':>14}  max|Δ|")
    fields = [("a00_log", "a00(TECU)"), ("clk_rms", "clkRMS(m)"),
              ("n_sta", "n_sta"), ("n_cb", "n_codebias"), ("dim", "datum_dim")]
    all_tows = sorted(maps[ref].keys())
    max_drift = 0.0
    mism = 0
    for tow in all_tows:
        for key, fname in fields:
            vals = []
            present = []
            for suf, _, _ in RUNS:
                x = maps[suf].get(tow, {}).get(key)
                vals.append(x); present.append(x)
            fin = [x for x in present if isinstance(x, (int, float)) and math.isfinite(x)]
            if len(fin) < 2:
                continue
            drift = max(fin) - min(fin)
            if abs(drift) > 1e-9:
                mism += 1
                max_drift = max(max_drift, abs(drift))
                def fmt(x):
                    return f"{x:14.4f}" if isinstance(x, (int, float)) else f"{'-':>14}"
                print(f"  {tow:>9.0f}  {fname:<12}" + "".join(fmt(v) for v in vals) + f"  {drift:.2e}")
    if mism == 0:
        print("  ✔ every shared epoch is bit-identical across all runs "
              "(a00, clock-RMS, station/bias counts, datum dim) — deterministic.")
    else:
        print(f"  �“ {mism} field/epoch cells DIFFER across runs (max |Δ|={max_drift:.3e}) — "
              "non-deterministic prefix (implementation concern).")

    # ================= verdict flags =================
    print("\n" + "=" * 92)
    print("VERDICT FLAGS")
    print("=" * 92)
    r = runs["10min"]
    a00s = [v["a00"] for v in r["vtec"].values() if math.isfinite(v["a00"])]
    a00_bad = [a for a in a00s if abs(a) > A00_PHYSICAL_MAX]
    print(f"  [{'FAIL' if a00_bad else ' OK '}] SH-iono a00 physical  "
          f"(|a00|<= {A00_PHYSICAL_MAX:.0f} TECU): "
          f"range [{min(a00s):.1f}, {max(a00s):.1f}] TECU over {len(a00s)} epochs"
          + (f"  — {len(a00_bad)} epoch(s) non-physical" if a00_bad else ""))
    gate_all = all(row["gate_ok"] == 1 for row in r["trace"]) if r["trace"] else False
    solv_all = all(row["solved"] == 1 for row in r["trace"]) if r["trace"] else False
    print(f"  [{' OK ' if gate_all else 'WARN'}] estimability gate: "
          f"gate_ok==1 for all {len(r['trace'])} epochs = {gate_all}")
    print(f"  [{' OK ' if solv_all else 'WARN'}] solve success: solved==1 all epochs = {solv_all}")
    if r["trace"]:
        rms0 = r["trace"][0]["rms_clock_m"]; rmsL = r["trace"][-1]["rms_clock_m"]
        trend = "shrinking" if rmsL < rms0 else "NOT shrinking"
        print(f"  [ -- ] clock-Δ RMS trend: {rms0:.2f} m (ep1) -> {rmsL:.2f} m (ep{len(r['trace'])})  [{trend}]")
    dims = {row["declared_dim"] for row in r["trace"]}
    print(f"  [ -- ] declared_dim set across epochs: {sorted(dims)}  "
          f"(2 = GPS+Galileo clock-datum nullspaces, expected)")

    # dump json
    out = RUN_ROOT / "eval_summary.json"
    dump = {}
    for suf, _, _ in RUNS:
        r = runs[suf]
        dump[suf] = {"label": r["label"], "exit": r["exit"], "wall_s": r["wall_s"],
                     "timing": r["log"]["timing"], "complete": r["log"]["complete"],
                     "n_trace_epochs": len(r["trace"]),
                     "a00_range": ([min(v["a00"] for v in r["vtec"].values() if math.isfinite(v["a00"])),
                                    max(v["a00"] for v in r["vtec"].values() if math.isfinite(v["a00"]))]
                                   if r["vtec"] else None)}
    out.write_text(json.dumps(dump, indent=2), encoding="utf-8")
    print(f"\nWrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
