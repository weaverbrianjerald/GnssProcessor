#!/usr/bin/env python3
"""Card CE / s24.17 leg 2: the HOUR-BY-HOUR grade of a long cold arm.

    python scripts/ce_hourly_grade.py --arms c10,pvb6 [--hours 6] [--tow0 259200]
        [--runs C:/GnssProcessor/runs] [--prefix _cm_] [--force] [--json out.json]

scripts/cm_grade.py grades ONE window (the last hour of a 2 h arm).  The 2 h numbers are
ignition by construction — the level channel's convergence budget is ~2 h
(SPEC_level_information_budget s14) — so a posture claim needs the same reads repeated per
hour on a longer arm.  This wrapper does exactly that and nothing else: it IMPORTS cm_grade
and calls its own `per_station_median` / `pct` for (i) and (ii), so the definitions cannot
drift from the card's, and it drives `wire_vs_code_census.py` once per hour for (iii) into a
per-hour output directory (cm_grade's `census()` caches under one fixed name per arm, which
would silently return hour 1's numbers for every later window).

Per arm per hour h (k = 120*(h-1)+1 .. 120*h, tow = tow0 + 30*(k-1)):
  (i)   network_pf_keyed.csv kind STA: per-station median rms_code over the hour, then
        p50 / p90 / max across stations.  Guarded exactly as cm_grade guards it: an arm whose
        post-fit pass pooled zero code rows reports an EMPTY POOL, never a pass.
  (ii)  network_zwd.csv: per-station median zwd_m; the count with |ZWD| > 0.5 m and the worst.
  (iii) wire_vs_code_census.py --window-tow/--window-n: declared within-arc rms of D_phase and
        D_code, and the raw clock contrast LEVEL p50.
  consumer: p50 / p90 of pos_err_3d_m over the hour from logs/closed_loop_trace.csv.
  avail:    emitted / withheld from network_epoch_status.csv over the hour.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cm_grade as CG                                        # noqa: E402  (path set above)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable or "python"


def census_window(armdir, runs_root, t_lo, n_ep, tag, force=False):
    """wire_vs_code_census.py over ONE window, into its own out dir (so hours cannot collide)."""
    label = os.path.basename(armdir.rstrip("/\\"))
    out_dir = os.path.join(armdir, f"census_{tag}")
    summ = os.path.join(out_dir, f"win_{label}_summary.json")
    proj = os.path.dirname(runs_root.rstrip("/\\"))
    rel = os.path.relpath(armdir, proj).replace("\\", "/")
    seed_json = os.path.join(armdir, "seed.json")
    if not os.path.isfile(seed_json):
        for cand in ("arm.json", "leg.json", "smoke.json"):
            if os.path.isfile(os.path.join(armdir, cand)):
                import shutil
                shutil.copyfile(os.path.join(armdir, cand), seed_json)
                break
    if force or not os.path.isfile(summ):
        os.makedirs(out_dir, exist_ok=True)
        cmd = [PY, os.path.join(REPO, "scripts", "wire_vs_code_census.py"), "--project", proj,
               "--seed", rel, "--run", rel, "--window-run", rel,
               "--window-tow", str(int(t_lo)), "--window-n", str(int(n_ep)), "--out", out_dir]
        with open(os.path.join(out_dir, "census_stdout.txt"), "w", encoding="utf-8", errors="replace") as fh:
            rc = subprocess.call(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=REPO)
        if rc != 0 or not os.path.isfile(summ):
            print(f"  !! census failed for {label} {tag} (rc {rc}); see {out_dir}/census_stdout.txt")
            return None
    with open(summ, encoding="utf-8") as fh:
        S = json.load(fh)

    def tab(method, kind, tag_, col):
        t = S.get(str((method, kind, tag_)), {}).get("table")
        return float(t[0][col]) if t else float("nan")

    def level(kind, col):
        t = S.get(str(("level", kind)))
        return float(t[0][col]) if t else float("nan")

    return dict(phase_decl_rms=tab("median", "phase", "declared", 4),
                code_decl_rms=tab("median", "code", "declared", 4),
                clk_level_p50=level("clk", 2),
                n_arcs=S.get(str(("median", "phase", "declared")), {}).get("n_arcs"))


def consumer_window(armdir, t_lo, t_hi):
    p = os.path.join(armdir, "logs", "closed_loop_trace.csv")
    if not os.path.isfile(p):
        return None
    import csv
    v = []
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                t, e = float(r["tow"]), float(r["pos_err_3d_m"])
            except (KeyError, ValueError, TypeError):
                continue
            if t_lo - 1e-3 <= t <= t_hi + 1e-3 and np.isfinite(e):
                v.append(e)
    if not v:
        return None
    a = np.array(v)
    return dict(n=int(a.size), p50=float(np.percentile(a, 50)), p90=float(np.percentile(a, 90)), max=float(a.max()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default="C:/GnssProcessor/runs")
    ap.add_argument("--prefix", default="_cm_")
    ap.add_argument("--arms", required=True)
    ap.add_argument("--hours", type=int, default=6)
    ap.add_argument("--tow0", type=float, default=259200.0)
    ap.add_argument("--epochs-per-hour", type=int, default=120)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    out = {}
    print(f"HOUR-BY-HOUR grade — {a.epochs_per_hour} epochs per hour, k = 1 at tow {a.tow0:.0f}")
    print(f"{'arm':<8} {'h':>2} {'k window':>11} {'(i) p50':>8} {'p90':>7} {'max':>7} {'(ii) n>0.5':>10} "
          f"{'|Z| p50':>8} {'(iii) Dph':>9} {'Dcode':>7} {'clkLVL':>7} {'cons p50':>8} {'p90':>7} {'emit':>5} {'wh':>4}")
    for arm in [s for s in a.arms.split(",") if s]:
        armdir = os.path.join(a.runs, f"{a.prefix}{arm}")
        rows = []
        C = CG.run_end_counters(CG.read_lines(os.path.join(armdir, "stdout.txt")))
        rows_line = C.get("rows")
        code_pooled = None if rows_line is None else int(rows_line[1])
        for h in range(1, a.hours + 1):
            k_lo = a.epochs_per_hour * (h - 1) + 1
            k_hi = a.epochs_per_hour * h
            t_lo = a.tow0 + 30.0 * (k_lo - 1)
            t_hi = a.tow0 + 30.0 * (k_hi - 1)
            logs = os.path.join(armdir, "logs")
            med, tows = CG.per_station_median(os.path.join(logs, "network_pf_keyed.csv"), "STA", "key",
                                              ["rms_code", "s0_code", "rms_phase"], t_lo, t_hi)
            if med and code_pooled == 0:
                med = None                       # EMPTY POOL, not a residual (cm_grade's guard)
            i = None
            if med and med["rms_code"]:
                rc = np.array(list(med["rms_code"].values()))
                i = dict(n_sta=int(rc.size), n_ep=len(tows), p50=CG.pct(rc, 50), p90=CG.pct(rc, 90),
                         max=float(rc.max()), phase_p50=CG.pct(list(med["rms_phase"].values()), 50),
                         worst=sorted(med["rms_code"].items(), key=lambda kv: -kv[1])[:3])
            medz, _ = CG.per_station_median(os.path.join(logs, "network_zwd.csv"), None, "station",
                                            ["zwd_m"], t_lo, t_hi)
            ii = None
            if medz and medz["zwd_m"]:
                z = medz["zwd_m"]
                az = np.abs(np.array(list(z.values())))
                ii = dict(n_sta=len(z), n_over_0p5=int((az > 0.5).sum()), abs_p50=CG.pct(az, 50),
                          abs_max=float(az.max()), worst=sorted(z.items(), key=lambda kv: -abs(kv[1]))[:3])
            iii = census_window(armdir, a.runs, t_lo, a.epochs_per_hour, f"h{h}", a.force)
            cons = consumer_window(armdir, t_lo, t_hi)
            whole, win = CG.epoch_status(os.path.join(logs, "network_epoch_status.csv"), t_lo, t_hi)
            emitted = dict(win).get("emitted", 0)
            withheld = sum(v for k, v in dict(win).items() if k != "emitted")

            def f(x, n=3):
                return "-" if x is None or (isinstance(x, float) and not np.isfinite(x)) else f"{x:.{n}f}"
            print(f"{arm:<8} {h:>2} {str(k_lo)+'..'+str(k_hi):>11} "
                  f"{f(i and i['p50']):>8} {f(i and i['p90']):>7} {f(i and i['max']):>7} "
                  f"{(str(ii['n_over_0p5']) if ii else '-'):>10} {f(ii and ii['abs_p50']):>8} "
                  f"{f(iii and iii['phase_decl_rms']):>9} {f(iii and iii['code_decl_rms']):>7} "
                  f"{f(iii and iii['clk_level_p50']):>7} "
                  f"{f(cons and cons['p50']):>8} {f(cons and cons['p90']):>7} {emitted:>5} {withheld:>4}")
            rows.append(dict(hour=h, k_lo=k_lo, k_hi=k_hi, t_lo=t_lo, t_hi=t_hi, i=i, ii=ii, iii=iii,
                             consumer=cons, emitted=emitted, withheld=withheld))
        out[arm] = rows
        print()
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=1, default=float)
        print(f"json -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
