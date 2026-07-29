#!/usr/bin/env python3
"""SPEC_day129_frame_guards Phase-2 validation-gate verdict.

Grades the ARMED leg (runs/day129_guards/leg01) against the spec's Phase-2
gates:

  (ii)  ON-path: the CB-excursion detector FIRED on the known excursion
        epochs and n_reinit / bumps-landed are non-zero (0 = inert = FAIL).
  (iii) The pivot-CB step at the 20:47:00 collapse (tow 334020) is BOUNDED:
        re-tracked within ~10 epochs instead of tau = 3.8 h, and the pivot's
        total IF(C1W,C2W) shows no multi-hour offset (reference: +49 m step,
        still +42.7 m at end of day).
  (iv)  Grading is datum-invariant by construction: between-satellite
        deviation of total_IF = CLK + 2.545728*CB(L1) - 1.545728*CB(L2) from
        the per-epoch constellation median (handoff section 3 methodology,
        same statistic scripts/chain_stability.py grades).

Plus the observability reads this leg exists to produce:
  - SH health gate (Phase 2.2): engage/release events, per-epoch out-of-band
    count, emissions withheld.
  - A4 sigma0 ceiling: withheld epochs.
  - RHS localization (section 4.3): keyed post-fit robust-z at each onset vs
    the key's own pre-onset baseline (which stations / satellites / signals
    carried the misfit) from network_pf_keyed.csv.

Usage:
    python scripts/p2_gate_verdict.py --legdir runs/day129_guards/leg01
        [--pivot G01] [--events 328890 334020]
"""
import argparse
import csv
import math
import os
import re
import statistics
import sys
from collections import defaultdict

# The producer's log lines carry sigma-hat / Delta glyphs; a cp1252 console
# must not kill the verdict over them.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DAY_ANCHOR_TOW = 259200.0
EPOCH_S = 30.0
C1 = 2.545728
C2 = 1.545728
L1_CODES = ("C1W", "C1C", "C1X", "C1P")
L2_CODES = ("C2W", "C2C", "C2X", "C2P", "C2L", "C2S")


def hms(tow):
    s = int(round(tow - DAY_ANCHOR_TOW))
    return f"{s // 3600:02d}:{(s % 3600) // 60:02d}:{s % 60:02d}"


def robust(vals):
    if not vals:
        return 0.0, 0.0
    med = statistics.median(vals)
    mad = statistics.median([abs(v - med) for v in vals])
    return med, 1.4826 * mad


# ---------------------------------------------------------------- loaders ---
def load_total_if(path):
    """network_solution.csv -> ordered [(tow, {sat: total_if})]."""
    clk = defaultdict(dict)
    cb = defaultdict(lambda: defaultdict(dict))
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                tow, sat, kind, val = (float(r["tow"]), r["sat"], r["kind"],
                                       float(r["value_m"]))
            except (ValueError, KeyError, TypeError):
                continue
            if kind == "CLK":
                clk[tow][sat] = val
            elif kind == "CB":
                cb[tow][sat][r["signal"]] = val
    epochs = []
    for tow in sorted(clk):
        tot = {}
        for sat, c in clk[tow].items():
            sigs = cb[tow].get(sat, {})
            l1 = next((sigs[s] for s in L1_CODES if s in sigs), None)
            l2 = next((sigs[s] for s in L2_CODES if s in sigs), None)
            if l1 is None or l2 is None:
                continue
            tot[sat] = c + C1 * l1 - C2 * l2
        if len(tot) >= 8:
            epochs.append((tow, tot))
    return epochs


def deviation_series(epochs, sat):
    """[(tow, dev_from_constellation_median)] for one satellite."""
    out = []
    for tow, tot in epochs:
        if sat not in tot:
            continue
        med = statistics.median(tot.values())
        out.append((tow, tot[sat] - med))
    return out


def load_events(path):
    if not os.path.isfile(path):
        return []
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def grep_log(log_path, pattern):
    rx = re.compile(pattern)
    hits = []
    if os.path.isfile(log_path):
        with open(log_path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if rx.search(line):
                    hits.append(line.rstrip())
    return hits


# ----------------------------------------------------------------- verdict --
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--legdir", required=True)
    ap.add_argument("--pivot", default="G01")
    ap.add_argument("--events", nargs="*", type=float, default=[328890.0, 334020.0],
                    help="onset TOWs to grade (default: 19:21:30 and 20:47:00)")
    args = ap.parse_args()
    legdir = os.path.abspath(args.legdir)
    logs = os.path.join(legdir, "logs")
    main_log = os.path.join(logs, "main_log.log")

    print(f"=== Phase-2 gate verdict — {legdir} ===\n")
    verdict = {}

    # --- run health ------------------------------------------------------
    n_err = len(grep_log(main_log, r"\[(error|critical)\]"))
    print(f"[run] error/critical log lines: {n_err}")
    for line in grep_log(main_log, r"producer complete"):
        print(f"[run] {line.split(']')[-1].strip()}")

    # --- epoch dispositions ----------------------------------------------
    es_path = os.path.join(logs, "network_epoch_status.csv")
    disp = defaultdict(int)
    es_rows = []
    if os.path.isfile(es_path):
        with open(es_path, newline="") as fh:
            es_rows = list(csv.DictReader(fh))
        for r in es_rows:
            disp[r.get("disposition", "?")] += 1
    print(f"[run] dispositions: {dict(disp)}\n")

    # --- gate (ii): detector ON-path -------------------------------------
    print("--- gate (ii): CB-excursion detector ON-path ---")
    p21 = grep_log(main_log, r"Phase 2\.1 CB-excursion guard")
    for line in p21:
        print("  " + line.split("] ", 3)[-1][:170])
    m = re.search(r"(\d+) trip\(s\), (\d+) code-bias carry reinit\(s\), (\d+) "
                  r"discontinuity bump\(s\)", " ".join(p21))
    trips = reinits = bumps = -1
    if m:
        trips, reinits, bumps = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    cbexc = [r for r in load_events(os.path.join(logs, "network_events.csv"))
             if r.get("tier") == "CB_EXC"]
    if cbexc:
        tows = sorted({float(r["tow"]) for r in cbexc if r.get("tow")})
        eras = ", ".join(f"{t:.0f} ({hms(t)})" for t in tows[:12])
        print(f"  CB_EXC events: {len(cbexc)} at {eras}"
              + (" ..." if len(tows) > 12 else ""))
    verdict["ii"] = "PASS" if (trips > 0 and reinits > 0 and bumps > 0) else "FAIL"
    print(f"  => gate (ii): {verdict['ii']}  (trips={trips} reinits={reinits} "
          f"bumps={bumps}; 0 anywhere = inert = FAIL)\n")

    # --- gate (iii): pivot-CB re-track at each event ----------------------
    print(f"--- gate (iii): {args.pivot} total-IF re-track (datum-invariant) ---")
    sol_path = os.path.join(logs, "network_solution.csv")
    epochs = load_total_if(sol_path) if os.path.isfile(sol_path) else []
    if not epochs:
        print("  network_solution.csv missing/empty — cannot grade")
        verdict["iii"] = "NO-DATA"
    else:
        dev = dict(deviation_series(epochs, args.pivot))
        towlist = sorted(dev)
        eod = towlist[-1]
        overall = "PASS"
        for onset in args.events:
            pre = [dev[t] for t in towlist if onset - 900.0 <= t < onset - EPOCH_S]
            if len(pre) < 5:
                print(f"  {hms(onset)}: <5 pre-onset epochs in leg — skipped")
                continue
            pre_med, pre_sig = robust(pre)
            band = max(2.0, 3.0 * pre_sig)
            post = [(t, dev[t]) for t in towlist if t >= onset]
            peak_t, peak = max(post[:40] or [(onset, 0.0)],
                               key=lambda kv: abs(kv[1] - pre_med))
            # re-track: first post-onset tow with |dev-pre| <= band sustained 5 ep
            rt_ep = None
            run = 0
            for i, (t, d) in enumerate(post):
                run = run + 1 if abs(d - pre_med) <= band else 0
                if run >= 5:
                    rt_ep = i - 3   # first epoch of the sustained run
                    break
            at = {}
            for label, t_q in (("+10ep", onset + 10 * EPOCH_S),
                               ("+1h", onset + 3600.0), ("EOD", eod)):
                near = min(towlist, key=lambda t: abs(t - t_q))
                at[label] = dev[near] - pre_med
            rt_txt = f"{rt_ep} ep" if rt_ep is not None else "NEVER"
            print(f"  {hms(onset)}: pre {pre_med:+.2f} m (sig {pre_sig:.2f}); "
                  f"peak {peak - pre_med:+.2f} m @ {hms(peak_t)}; "
                  f"re-track {rt_txt} (band +/-{band:.2f} m); "
                  f"offset +10ep {at['+10ep']:+.2f} / +1h {at['+1h']:+.2f} / "
                  f"EOD {at['EOD']:+.2f} m")
            if rt_ep is None or rt_ep > 20 or abs(at["EOD"]) > 5.0:
                overall = "FAIL"
        # worst satellite at EOD for context (gate-iv statistic, all sats)
        last_tow, last_tot = epochs[-1]
        med = statistics.median(last_tot.values())
        worst = max(last_tot, key=lambda s: abs(last_tot[s] - med))
        print(f"  EOD worst-sat deviation: {worst} {last_tot[worst] - med:+.2f} m "
              f"over {len(last_tot)} sats")
        verdict["iii"] = overall
        print(f"  => gate (iii): {overall}  (spec: re-track ~10 ep, no multi-hour "
              f"offset; reference damage was +49 m with tau=3.8 h)\n")

    # --- Phase 2.2 SH gate read ------------------------------------------
    print("--- Phase 2.2: SH health gate ---")
    for line in grep_log(main_log, r"Phase 2\.2 SH health gate"):
        print("  " + line.split("] ", 3)[-1][:170])
    shev = [r for r in load_events(os.path.join(logs, "network_events.csv"))
            if r.get("tier") == "SH_GATE" and r.get("event") in ("engage", "release")]
    for r in shev[:16]:
        print(f"  SH_GATE {r.get('event'):8s} tow {float(r['tow']):.0f} "
              f"({hms(float(r['tow']))}) a00={r.get('statistic')}")
    print(f"  sh_gate_withheld epochs (status rows): {disp.get('sh_gate_withheld', 0)}\n")

    # --- A4 read ----------------------------------------------------------
    print("--- A4 sigma0 ceiling ---")
    print(f"  sigma0_withheld epochs: {disp.get('sigma0_withheld', 0)}")
    s0 = [(float(r["tow"]), float(r["sigma0"])) for r in es_rows
          if r.get("sigma0") not in (None, "", "0.0000")]
    if s0:
        mx = max(s0, key=lambda kv: kv[1])
        print(f"  max sigma0_adapt {mx[1]:.2f} at tow {mx[0]:.0f} ({hms(mx[0])}) "
              f"(ceiling 6.0)\n")

    # --- section 4.3 RHS keyed read at each onset -------------------------
    print("--- RHS localization (network_pf_keyed.csv robust-z at onset) ---")
    pk_path = os.path.join(logs, "network_pf_keyed.csv")
    if not os.path.isfile(pk_path):
        print("  (keyed trace absent)")
    else:
        series = defaultdict(dict)   # (kind,key) -> {tow: s0_code}
        with open(pk_path, newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    series[(r["kind"], r["key"])][float(r["tow"])] = float(r["s0_code"])
                except (ValueError, KeyError, TypeError):
                    continue
        for onset in args.events:
            print(f"  onset {hms(onset)}:")
            scored = []
            for (kind, key), sv in series.items():
                tow_on = min(sv, key=lambda t: abs(t - onset))
                if abs(tow_on - onset) > EPOCH_S:
                    continue
                base = [v for t, v in sv.items() if t < onset - EPOCH_S]
                if len(base) < 10:
                    continue
                med, sig = robust(base)
                z = (sv[tow_on] - med) / sig if sig > 1e-6 else 0.0
                scored.append((z, kind, key, sv[tow_on], med))
            for z, kind, key, v, med in sorted(scored, reverse=True)[:8]:
                print(f"    z={z:8.1f}  {kind:3s} {key:10s} s0_code {med:.2f} -> {v:.2f} m")
        print()

    print("=== summary ===")
    for g in ("ii", "iii"):
        print(f"  gate ({g}): {verdict.get(g, '?')}")
    print("  gate (iv): datum-invariant by construction (between-sat total-IF)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
