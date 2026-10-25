#!/usr/bin/env python3
"""simnet_clock_truth_grade.py (card SCB): the network's satellite clock against the simulator's truth.

The simulator's truth clock is the product clock (COD) plus the injected error of
`truth_satclock.csv` (absent on a quiet fixture: the error is zero). The estimate's absolute
clock is the a priori plus the wire's C record: on a COD a priori the error against truth is
    e = C + ds - err
and on the native broadcast-clock posture (card NBP, S line `sp3+brdc:`), where the wire's C is
a correction to the stamped broadcast record,
    e = (brdc + C_wire) + ds - (COD + err)
with brdc and C_wire from `logs/network_brdc_offset.csv` and COD from the CLK file. ds is the
per-satellite IF combination of the truth OSB over the pinned reference pair (GPS C1C/C2W,
Galileo C1C/C5Q): the pinned pair's bias share sits in the clock by the datum and is not error
(card SCL2's `clklevel.datum_share`).

Per epoch and constellation the median of e over the graded satellites is removed (the clock
gauge); the pivot of the S line is left ungraded. Hours count from the first emitted epoch.
Reported (card SCB gate C1): (a) the level, rms of e per hour; (b) the phase-visible part, rms
per hour of e less each satellite's mean over hours 2 to 6; (c) the per-satellite means over
hours 2 to 6, their rms across satellites and the largest three; (d) on a clock fixture the
correlation across satellites of that mean with the satellite's drawn rate (least-squares slope
of the truth over the window) and with the truth's window mean. Gate C2: the held-out
consumer's hourly 3D p50 from `logs/closed_loop_trace.csv`.

    python scripts/simnet_clock_truth_grade.py --fixture <fixture_dir> --run <run> [--run <run> ...]
        [--clk <COD CLK>] [--hours 6] [--persat out.csv] [--json out.json] [--no-ds]
    python scripts/simnet_clock_truth_grade.py --selftest
"""
import argparse
import csv
import datetime as dt
import glob
import json
import math
import os
import statistics as st
import sys
import tempfile
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_grade import parse_products  # noqa: E402

C_LIGHT = 299792458.0
DEFAULT_CLK = "C:/GnssProcessor/data/2024-129/products/COD0MGXFIN_20241290000_01D_30S_CLK.CLK"
PAIR = {"G": (1575.42e6, 1227.60e6, "C1C", "C2W"), "E": (1575.42e6, 1176.45e6, "C1C", "C5Q")}
MIN_SATS = 4
GPS_EPOCH = dt.datetime(1980, 1, 6)


def datum_share(fixture):
    """{sat: IF(reference pair) of the truth OSB, m}; the first station's truth_bias.csv (all agree)."""
    files = sorted(glob.glob(os.path.join(fixture, "*", "truth_bias.csv")))
    if not files:
        raise SystemExit("no truth_bias.csv under " + fixture)
    osb = defaultdict(dict)
    for r in csv.DictReader(open(files[0])):
        osb[r["satellite"]][r["signal"]] = float(r["osb_m"])
    ds = {}
    for sat, v in osb.items():
        pr = PAIR.get(sat[0])
        if pr and pr[2] in v and pr[3] in v:
            f1, f2 = pr[0], pr[1]
            a = f1 * f1 / (f1 * f1 - f2 * f2)
            b = f2 * f2 / (f1 * f1 - f2 * f2)
            ds[sat] = a * v[pr[2]] - b * v[pr[3]]
    return ds


def truth_error(fixture):
    """{sat: {tow: err_m}} from truth_satclock.csv, or {} on a quiet fixture."""
    files = sorted(glob.glob(os.path.join(fixture, "*", "truth_satclock.csv")))
    if not files:
        return {}
    T = defaultdict(dict)
    for r in csv.DictReader(open(files[0])):
        T[r["satellite"]][int(round(float(r["tow_s"])))] = float(r["clk_err_m"])
    return T


def cod_clock(path):
    """{sat: {tow: m}} from the AS records of a CLK file (GPS time)."""
    out = defaultdict(dict)
    header = True
    with open(path, errors="replace") as f:
        for line in f:
            if header:
                if "END OF HEADER" in line:
                    header = False
                continue
            if not line.startswith("AS "):
                continue
            p = line.split()
            y, mo, d, h, mi = (int(x) for x in p[2:7])
            sec = float(p[7])
            t = dt.datetime(y, mo, d, h, mi) + dt.timedelta(seconds=sec)
            delta = t - GPS_EPOCH
            tow = (delta.days % 7) * 86400 + delta.seconds + delta.microseconds * 1e-6
            out[p[1]][int(round(tow))] = float(p[9]) * C_LIGHT
    return out


def s_line(run):
    with open(os.path.join(run, "products.txt"), errors="replace") as f:
        for line in f:
            if line.startswith("S,"):
                return line.strip()
    raise SystemExit("no S line in " + run)


def pivots(s):
    return {t.split(":")[1] for t in s.split(",")
            if len(t) == 5 and t[1] == ":" and t[0] in "GEC" and t[2] in "GEC" and t[3:].isdigit()}


def native_estimate(run):
    """{sat: {tow: brdc + C_wire}} of a native arm."""
    E = defaultdict(dict)
    for r in csv.DictReader(open(os.path.join(run, "logs", "network_brdc_offset.csv"))):
        E[r["sat"]][int(round(float(r["tow"])))] = float(r["brdc_m"]) + float(r["c_wire_m"])
    return E


def error_series(run, fixture, clk_path, use_ds=True):
    """Return (E {sat: {tow: e_decommoned}}, tows, info)."""
    s = s_line(run)
    native = "sp3+brdc:" in s
    piv = pivots(s)
    ds = datum_share(fixture) if use_ds else defaultdict(float)
    T = truth_error(fixture)
    _, epochs, census = parse_products(os.path.join(run, "products.txt"))
    tows = [int(round(ep["tow"])) for ep in epochs]
    EST = native_estimate(run) if native else None
    COD = cod_clock(clk_path) if native else None
    E = defaultdict(dict)
    for ep, tow in zip(epochs, tows):
        for sat, c0 in ep["clocks"].items():
            if sat in piv or (use_ds and sat not in ds):
                continue
            if T:
                if tow not in T.get(sat, {}):
                    continue
                err = T[sat][tow]
            else:
                err = 0.0
            if native:
                if tow not in EST.get(sat, {}) or tow not in COD.get(sat, {}):
                    continue
                e = EST[sat][tow] + ds.get(sat, 0.0) - (COD[sat][tow] + err)
            else:
                e = c0 + ds.get(sat, 0.0) - err
            E[sat][tow] = e
    # the clock gauge: the per-epoch, per-constellation median over the graded satellites
    for tow in tows:
        for sysc in "GEC":
            sats = [x for x in E if x[0] == sysc and tow in E[x]]
            if len(sats) < MIN_SATS:
                for x in sats:
                    del E[x][tow]
                continue
            m = st.median(E[x][tow] for x in sats)
            for x in sats:
                E[x][tow] -= m
    info = {"native": native, "pivots": sorted(piv), "epochs": len(tows), "census": dict(census),
            "truth_present": bool(T), "ds_applied": use_ds}
    return E, tows, info, T


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")


def pearson(x, y):
    n = len(x)
    if n < 3:
        return float("nan")
    mx, my = sum(x) / n, sum(y) / n
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    if sxx <= 0 or syy <= 0:
        return float("nan")
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / math.sqrt(sxx * syy)


def slope(ts, vs):
    n = len(ts)
    if n < 3:
        return float("nan")
    mt, mv = sum(ts) / n, sum(vs) / n
    stt = sum((t - mt) ** 2 for t in ts)
    return sum((t - mt) * (v - mv) for t, v in zip(ts, vs)) / stt if stt > 0 else float("nan")


def grade(run, fixture, clk_path, hours, use_ds=True):
    E, tows, info, T = error_series(run, fixture, clk_path, use_ds)
    t0 = tows[0]
    hour_of = lambda tow: int((tow - t0) // 3600)
    win = lambda tow: 1 <= hour_of(tow) <= 5          # hours 2 to 6
    out = {"run": run, "info": info, "hours": []}
    # per-satellite means over hours 2 to 6
    mean_s = {}
    for sat, series in E.items():
        v = [e for tow, e in series.items() if win(tow)]
        if len(v) >= 20:
            mean_s[sat] = sum(v) / len(v)
    for h in range(int(hours)):
        lev, pv, sats = [], [], set()
        for sat, series in E.items():
            for tow, e in series.items():
                if hour_of(tow) == h:
                    lev.append(e)
                    sats.add(sat)
                    if sat in mean_s:
                        pv.append(e - mean_s[sat])
        out["hours"].append({"hour": h + 1, "n_sat": len(sats), "n": len(lev),
                             "level_rms_m": rms(lev), "phase_visible_rms_m": rms(pv)})
    means = sorted(mean_s.items(), key=lambda kv: -abs(kv[1]))
    out["persat_mean"] = {"rms_m": rms(list(mean_s.values())), "n_sat": len(mean_s),
                          "largest": [(s, round(m, 4)) for s, m in means[:3]]}
    if T:
        rates, tmeans, errs = [], [], []
        for sat, m in mean_s.items():
            ts = sorted(t for t in T.get(sat, {}) if win(t))
            if len(ts) < 20:
                continue
            vs = [T[sat][t] for t in ts]
            rates.append(slope(ts, vs))
            tmeans.append(sum(vs) / len(vs))
            errs.append(m)
        out["rate_corr"] = {"n_sat": len(errs), "corr_mean_vs_rate": pearson(errs, rates),
                            "corr_mean_vs_truth_mean": pearson(errs, tmeans),
                            "rate_rms_mps": rms(rates), "truth_mean_rms_m": rms(tmeans)}
    out["consumer_p50_m"] = consumer(run, hours)
    return out, E


def consumer(run, hours):
    p = os.path.join(run, "logs", "closed_loop_trace.csv")
    if not os.path.exists(p):
        return []
    rows = list(csv.DictReader(open(p)))
    if not rows:
        return []
    t0 = float(rows[0]["tow"])
    by = defaultdict(list)
    for r in rows:
        by[int((float(r["tow"]) - t0) // 3600)].append(float(r["pos_err_3d_m"]))
    return [round(st.median(by[h]), 4) if by.get(h) else None for h in range(int(hours))]


def report(res):
    info = res["info"]
    print(f"== {res['run']}  ({'native broadcast a priori' if info['native'] else 'COD a priori'}; "
          f"pivots {','.join(info['pivots'])}; {info['epochs']} epochs; truth error "
          f"{'present' if info['truth_present'] else 'absent (quiet)'}; ds {'applied' if info['ds_applied'] else 'not applied'})")
    print("  hour  n_sat   level_rms_m  phase_visible_rms_m  consumer_p50_m")
    cons = res["consumer_p50_m"]
    for h in res["hours"]:
        c = cons[h["hour"] - 1] if cons and len(cons) >= h["hour"] else None
        print(f"  {h['hour']:>4}  {h['n_sat']:>5}   {h['level_rms_m']:11.4f}  {h['phase_visible_rms_m']:19.4f}  "
              f"{'' if c is None else f'{c:.4f}':>14}")
    pm = res["persat_mean"]
    print(f"  per-satellite mean over hours 2 to 6: rms {pm['rms_m']:.4f} m over {pm['n_sat']} satellites; "
          f"largest {pm['largest']}")
    if "rate_corr" in res:
        rc = res["rate_corr"]
        print(f"  rate test ({rc['n_sat']} sats): corr(mean error, drawn rate) {rc['corr_mean_vs_rate']:+.3f}; "
              f"corr(mean error, truth window mean) {rc['corr_mean_vs_truth_mean']:+.3f}; "
              f"truth rate rms {rc['rate_rms_mps']:.2e} m/s, truth window mean rms {rc['truth_mean_rms_m']:.3f} m")


def selftest():
    """A fabricated fixture and wire: the sign convention, the gauge removal and the truth subtraction."""
    d = tempfile.mkdtemp(prefix="sctg_")
    sdir = os.path.join(d, "S01000SIM")
    os.makedirs(sdir)
    sats = ["G01", "G02", "G03", "G04", "G05"]
    ds = {"G01": 0.10, "G02": -0.20, "G03": 0.30, "G04": 0.0, "G05": 0.05}
    f1, f2 = PAIR["G"][0], PAIR["G"][1]
    a = f1 * f1 / (f1 * f1 - f2 * f2)
    b = f2 * f2 / (f1 * f1 - f2 * f2)
    with open(os.path.join(sdir, "truth_bias.csv"), "w") as f:
        f.write("satellite,signal,osb_m\n")
        for s in sats:
            # choose osb(C2W) = 0 so IF = a * osb(C1C) = ds
            f.write(f"{s},C1C,{ds[s] / a}\n{s},C2W,0.0\n")
    tows = [259230, 259260]
    err = {s: {t: 0.01 * (i + 1) * (k + 1) for k, t in enumerate(tows)} for i, s in enumerate(sats)}
    with open(os.path.join(sdir, "truth_satclock.csv"), "w") as f:
        f.write("week,tow_s,satellite,clk_err_m,clk_err_s\n")
        for s in sats:
            for t in tows:
                f.write(f"2313,{t}.000,{s},{err[s][t]},{err[s][t] / C_LIGHT}\n")
    run = os.path.join(d, "run")
    os.makedirs(os.path.join(run, "logs"))
    common = 5.0   # a gauge offset common to every satellite at epoch 2
    with open(os.path.join(run, "products.txt"), "w") as f:
        f.write("# gnss_network_product v2\nS,S30000SIM,1,nominal-wu93,0,0,1,0,0.05,1,COD.SP3+COD.CLK,G:G01,cbjump=1\n")
        for k, t in enumerate(tows):
            f.write(f"E,2313,{t}.000,0\n")
            for s in sats:
                c = -ds[s] + err[s][t] + (common if k == 1 else 0.0)   # a perfect estimate up to the gauge
                f.write(f"C,{s},{c}\n")
    E, tws, info, T = error_series(run, d, DEFAULT_CLK)
    assert info["pivots"] == ["G01"] and "G01" not in E, info
    worst = max(abs(e) for s in E for e in E[s].values())
    assert worst < 1e-9, worst
    # a satellite off by +0.02 m at both epochs reads +0.02 after the median removal (4 graded, median of 0,0,0,0.02)
    with open(os.path.join(run, "products.txt"), "a") as f:
        pass
    print(f"selftest PASS: pivot excluded, ds sign as clklevel, truth subtracted, gauge removed (worst {worst:.1e} m)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture")
    ap.add_argument("--run", action="append", default=[])
    ap.add_argument("--clk", default=DEFAULT_CLK)
    ap.add_argument("--hours", type=float, default=6.0)
    ap.add_argument("--persat")
    ap.add_argument("--json")
    ap.add_argument("--no-ds", action="store_true", help="omit the datum share (a sign diagnostic)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    if not a.fixture or not a.run:
        ap.error("--fixture and at least one --run are required")
    results = []
    for run in a.run:
        res, E = grade(run, a.fixture, a.clk, a.hours, use_ds=not a.no_ds)
        report(res)
        results.append(res)
        if a.persat:
            with open(a.persat if len(a.run) == 1 else a.persat.replace(".csv", f"_{os.path.basename(run)}.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["sat", "tow", "e_m"])
                for sat in sorted(E):
                    for tow in sorted(E[sat]):
                        w.writerow([sat, tow, f"{E[sat][tow]:.5f}"])
    if a.json:
        json.dump(results, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
