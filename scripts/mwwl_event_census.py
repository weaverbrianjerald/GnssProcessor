"""Estimator-free lane-information census across an EVENT WINDOW (DOY-129 19:26 detonation).

Settles one question that needs NO estimator run: when the network's SH-VTEC field
detonates, is the MW/widelane cascade's *information* also disturbed (⇒ the cascade is a
victim of the same insult) or is it intact (⇒ the cascade holds information the estimator
loses, and is a candidate conditioner)?

The cascade has two information paths with OPPOSITE exposure to an ionospheric/SH fault:

  MW(1,2), MW(2,5)   Melbourne-Wubbena — IONO-FREE by construction, code-noise-limited.
                     MW(2,5) (EWL, lam 5.861 m) is single-epoch fixable (sigma ~0.04 cyc);
                     MW(1,2) (WL, lam 0.862 m) needs n ~ 25-50 ep (sigma ~0.5-0.7/sqrt(n)).
  GF_b bridge        lam12*(L1-L2) - lam25*(L2-L5), phase-only, PRECISE and FAST but
                     IONO-CONTAMINATED: z_WL = N12 - 0.5064 * I1_dd[m].  The shipped
                     cascade de-biases it with the network's OWN slant-iono estimate,
                     so it inherits whatever the SH field is doing.

This tool measures both paths straight from raw RINEX, per epoch, over a window, together
with the DATA-side disturbance metrics (phase TEC rate, code-minus-phase iono residual =
what the code-only SH field actually eats, LLI, MW jumps).

Usage
    python scripts/mwwl_event_census.py parse   --start 18:00:00 --end 21:30:00
    python scripts/mwwl_event_census.py analyze --start 18:00:00 --end 21:30:00

`parse` caches per-station arrays under the scratchpad; `analyze` re-reads the cache, so
window sub-slices and metric changes cost seconds, not a re-parse of 9 GB.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pickle
import statistics as st
import sys
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor

C = 299792458.0
F1, F2, F5 = 1575.42e6, 1227.60e6, 1176.45e6
LAM1, LAM2, LAM5 = C / F1, C / F2, C / F5
LAM12 = C / (F1 - F2)                 # 0.8619 m  widelane
LAM25 = C / (F2 - F5)                 # 5.861 m   extra-widelane
GF_SCALE = (F1 * F1) / (F2 * F2) - 1.0   # 0.64694 : P2-P1 = GF_SCALE * I1[m]
IONO_CYC_PER_M = 0.5064               # bridge sensitivity, WL cycles per m of DD I1
TECU_M_L1 = 0.16240                   # 1 TECU at L1 in metres

DATA_ROOT = r"C:\GnssProcessor\data\2024-129\obs"
ROSTER_JSON = r"C:\GnssProcessor\runs\guards_cont\leg.json"
CACHE_DIR = os.path.join(
    os.environ.get("TEMP", r"C:\Temp"),
    "claude", "C--GnssProcessor", "13e1dc21-707d-4ecd-bc5e-02b9c8f443ca",
    "scratchpad", "mwwl_census_cache")

# same-channel code/phase preference per band (estimator uses GPS "WCSLXPYMIQ")
CHAN_PREF = {"1": "WCSLX", "2": "WLSXC", "5": "QXI"}


# ----------------------------------------------------------------- parsing


def _hms(s: str) -> float:
    h, m, sec = s.split(":")
    return int(h) * 3600 + int(m) * 60 + float(sec)


def parse_header(fh):
    """-> ({sys: [obs types]}, approx_xyz or None); file left after END OF HEADER."""
    types, cur, pos = {}, None, None
    for line in fh:
        if "APPROX POSITION XYZ" in line:
            try:
                pos = tuple(float(line[i:i + 14]) for i in (0, 14, 28))
            except ValueError:
                pos = None
        if "SYS / # / OBS TYPES" in line:
            if line[0].strip():
                cur = line[0]
                types[cur] = line[7:60].split()
            elif cur:
                types[cur] += line[7:60].split()
        if "END OF HEADER" in line:
            break
    return types, pos


def pick_channel(tl, band):
    """-> (code_idx, phase_idx, chan) for the first channel carrying BOTH C and L."""
    for ch in CHAN_PREF[band]:
        c, l = "C" + band + ch, "L" + band + ch
        if c in tl and l in tl:
            return tl.index(c), tl.index(l), ch
    return None


def parse_station(args):
    """Worker: parse one RINEX over [t0, t1] -> compact per-(sat, epoch) record."""
    path, name, t0, t1, step = args
    try:
        fh = open(path, "r", errors="replace")
    except OSError:
        return name, None
    with fh:
        types, pos = parse_header(fh)
        tl = types.get("G")
        if not tl:
            return name, None
        b1, b2, b5 = (pick_channel(tl, "1"), pick_channel(tl, "2"), pick_channel(tl, "5"))
        if not b1 or not b2:
            return name, None

        n_ep = int(round((t1 - t0) / step)) + 1
        # sat -> arrays indexed by epoch slot
        rec = defaultdict(lambda: {
            "mw12": [None] * n_ep, "mw25": [None] * n_ep, "gfb": [None] * n_ep,
            "icode": [None] * n_ep, "iphase": [None] * n_ep, "lli": [0] * n_ep,
        })
        l5_epochs = 0
        slot, in_win = -1, False
        for line in fh:
            if line.startswith(">"):
                try:
                    hh, mm = int(line[13:15]), int(line[16:18])
                    ss = float(line[19:29])
                except ValueError:
                    slot, in_win = -1, False
                    continue
                tod = hh * 3600 + mm * 60 + ss
                if tod > t1 + 0.5:
                    break
                if tod < t0 - 0.5:
                    slot, in_win = -1, False
                    continue
                k = (tod - t0) / step
                slot = int(round(k))
                in_win = abs(k - slot) < 0.02 and 0 <= slot < n_ep
                continue
            if not in_win or not line.startswith("G"):
                continue
            sat = line[0:3]

            def fld(i):
                s = line[3 + 16 * i: 3 + 16 * i + 14].strip()
                if not s:
                    return None
                try:
                    v = float(s)
                except ValueError:
                    return None
                return v if v != 0.0 else None

            def lli(i):
                c = line[3 + 16 * i + 14: 3 + 16 * i + 15]
                try:
                    return int(c) if c.strip() else 0
                except ValueError:
                    return 0

            p1, l1 = fld(b1[0]), fld(b1[1])
            p2, l2 = fld(b2[0]), fld(b2[1])
            if None in (p1, l1, p2, l2):
                continue
            r = rec[sat]
            r["mw12"][slot] = (l1 - l2) - (F1 * p1 + F2 * p2) / ((F1 + F2) * LAM12)
            r["icode"][slot] = (p2 - p1) / GF_SCALE           # slant I1 from CODE  [m]
            r["iphase"][slot] = -(LAM1 * l1 - LAM2 * l2) / GF_SCALE   # I1 from PHASE + const
            r["lli"][slot] = (lli(b1[1]) & 1) | (lli(b2[1]) & 1)
            if b5:
                p5, l5 = fld(b5[0]), fld(b5[1])
                if None not in (p5, l5):
                    r["mw25"][slot] = (l2 - l5) - (F2 * p2 + F5 * p5) / ((F2 + F5) * LAM25)
                    r["gfb"][slot] = LAM12 * (l1 - l2) - LAM25 * (l2 - l5)
                    l5_epochs += 1

    if not rec:
        return name, None
    return name, {
        "pos": pos,
        "chan": (b1[2], b2[2], b5[2] if b5 else None),
        "l5_declared": b5 is not None,
        "l5_epochs": l5_epochs,
        "sats": {s: dict(v) for s, v in rec.items()},
    }


def roster():
    cfg = json.load(open(ROSTER_JSON, encoding="utf-8"))
    return [s["name"].upper() for s in cfg["network"]["stations"]]


def cmd_parse(a):
    names = roster()
    jobs = []
    for n in names:
        d = os.path.join(DATA_ROOT, n.lower())
        hits = [f for f in os.listdir(d)] if os.path.isdir(d) else []
        hits = [f for f in hits if f.endswith("_30S_MO.rnx")]
        if hits:
            jobs.append((os.path.join(d, hits[0]), n, _hms(a.start), _hms(a.end), a.step))
    print(f"roster {len(names)} stations, {len(jobs)} RINEX found; window "
          f"{a.start}..{a.end} @ {a.step:.0f}s  ({len(jobs)} workers over {a.jobs} procs)")
    os.makedirs(CACHE_DIR, exist_ok=True)
    out, done = {}, 0
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for name, d in ex.map(parse_station, jobs, chunksize=1):
            done += 1
            if d:
                out[name] = d
            if done % 25 == 0:
                print(f"  ... {done}/{len(jobs)}", flush=True)
    tag = f"{a.start.replace(':','')}_{a.end.replace(':','')}"
    p = os.path.join(CACHE_DIR, f"census_{tag}.pkl")
    with open(p, "wb") as fh:
        pickle.dump({"window": (a.start, a.end, a.step), "sta": out}, fh, 4)
    l5 = [n for n, d in out.items() if d["l5_epochs"] > 0]
    print(f"parsed {len(out)} stations -> {p}")
    print(f"L5 DECLARED in header: {sum(1 for d in out.values() if d['l5_declared'])}; "
          f"L5 DATA present: {len(l5)}")
    return 0


# ---------------------------------------------------------------- analysis


def frac_stats(vals):
    if not vals:
        return None
    fx = [v - round(v) for v in vals]
    n = len(fx)
    af = sorted(abs(x) for x in fx)
    R = math.hypot(sum(math.cos(2 * math.pi * x) for x in fx) / n,
                   sum(math.sin(2 * math.pi * x) for x in fx) / n)
    return {"n": n, "med": af[n // 2], "p15": sum(1 for x in af if x < 0.15) / n, "R": R}


def trailing_median(series, slot, w):
    lo = max(0, slot - w + 1)
    vals = [v for v in series[lo:slot + 1] if v is not None]
    return st.median(vals) if len(vals) >= max(3, w // 2) else None


def dd_population(per_sta, ref_sta, ref_sat):
    """star-basis DD over {sta: {sat: value}} -> list of DD values."""
    out = []
    base = per_sta.get(ref_sta)
    if not base or ref_sat not in base:
        return out
    for sta, sats in per_sta.items():
        if sta == ref_sta or ref_sat not in sats:
            continue
        for sat, v in sats.items():
            if sat == ref_sat or sat not in base:
                continue
            out.append((v - sats[ref_sat]) - (base[sat] - base[ref_sat]))
    return out


def pick_refs(per_sta):
    cnt = defaultdict(int)
    for sats in per_sta.values():
        for s in sats:
            cnt[s] += 1
    if not cnt:
        return None, None
    ref_sat = max(cnt, key=cnt.get)
    cands = [s for s, v in per_sta.items() if ref_sat in v]
    if not cands:
        return None, None
    return max(cands, key=lambda s: len(per_sta[s])), ref_sat


def cmd_analyze(a):
    tag = f"{a.start.replace(':','')}_{a.end.replace(':','')}"
    p = os.path.join(CACHE_DIR, f"census_{tag}.pkl")
    blob = pickle.load(open(p, "rb"))
    sta, (s0, s1, step) = blob["sta"], blob["window"]
    t0 = _hms(s0)
    n_ep = int(round((_hms(s1) - t0) / step)) + 1
    print(f"# window {s0}..{s1} step {step:.0f}s  n_ep={n_ep}  stations={len(sta)}")

    # channel groups (DD discipline: never mix channels)
    g12 = defaultdict(list)
    g25 = defaultdict(list)
    for n, d in sta.items():
        c1, c2, c5 = d["chan"]
        g12[(c1, c2)].append(n)
        if c5 and d["l5_epochs"] > 0:
            g25[(c2, c5)].append(n)
    grp12 = max(g12.values(), key=len)
    grp25 = max(g25.values(), key=len) if g25 else []
    print(f"# DD group WL(1,2): {len(grp12)} sta  {max(g12, key=lambda k: len(g12[k]))}")
    print(f"# DD group EWL(2,5): {len(grp25)} sta  "
          f"{max(g25, key=lambda k: len(g25[k])) if g25 else '-'}")
    print(f"# L5 data stations: {sum(1 for d in sta.values() if d['l5_epochs'] > 0)}"
          f" / {len(sta)}")

    # per-arc leveling constant for code-minus-phase iono (whole window median)
    level = {}
    for n, d in sta.items():
        for s, r in d["sats"].items():
            dif = [c - ph for c, ph in zip(r["icode"], r["iphase"])
                   if c is not None and ph is not None]
            if len(dif) >= 10:
                level[(n, s)] = st.median(dif)

    cols = ("tow_hms n_los n_l5 dI_med dI_p99 icres_med icres_p99 lli mwjump "
            "wl_n wl_med wl_R ewl_n ewl_med ewl_R brg_n brg_med brg_R "
            "brgiono_med brgiono_p99").split()
    print(",".join(cols))
    rows = []
    for e in range(n_ep):
        tod = t0 + e * step
        hms = f"{int(tod)//3600:02d}:{(int(tod)%3600)//60:02d}:{int(tod)%60:02d}"
        n_los = n_l5 = lli = mwjump = 0
        dI, icres = [], []
        m12 = defaultdict(dict)
        m25 = defaultdict(dict)
        gfb = defaultdict(dict)
        for n, d in sta.items():
            in12 = n in grp12
            in25 = n in grp25
            for s, r in d["sats"].items():
                if r["mw12"][e] is None:
                    continue
                n_los += 1
                lli += r["lli"][e]
                if e > 0 and r["iphase"][e - 1] is not None:
                    dI.append(abs(r["iphase"][e] - r["iphase"][e - 1]))
                if e > 0 and r["mw12"][e - 1] is not None:
                    if abs(r["mw12"][e] - r["mw12"][e - 1]) > 1.0:
                        mwjump += 1
                lv = level.get((n, s))
                if lv is not None:
                    icres.append(abs(r["icode"][e] - r["iphase"][e] - lv))
                if in12:
                    v = trailing_median(r["mw12"], e, a.win)
                    if v is not None:
                        m12[n][s] = v
                if r["mw25"][e] is not None:
                    n_l5 += 1
                    if in25:
                        v = trailing_median(r["mw25"], e, a.win)
                        g = trailing_median(r["gfb"], e, a.win)
                        if v is not None and g is not None:
                            m25[n][s] = v
                            gfb[n][s] = g

        def q(v, f):
            if not v:
                return float("nan")
            v = sorted(v)
            return v[min(len(v) - 1, int(f * len(v)))]

        r_wl = r_ewl = r_brg = None
        bi = []
        rs, rt = pick_refs(m12)
        if rs:
            r_wl = frac_stats(dd_population(m12, rs, rt))
        rs, rt = pick_refs(m25)
        if rs:
            dd25 = dd_population(m25, rs, rt)
            r_ewl = frac_stats(dd25)
            ddg = dd_population(gfb, rs, rt)
            if len(ddg) == len(dd25):
                z = []
                for g, e25 in zip(ddg, dd25):
                    if abs(e25 - round(e25)) > 0.20:      # FFR gate on the EWL integer
                        continue
                    zz = (g + LAM25 * round(e25)) / LAM12
                    z.append(zz)
                    bi.append(abs(-(zz - round(zz)) / IONO_CYC_PER_M))
                r_brg = frac_stats(z)

        def fmt(d, k):
            return f"{d[k]:.4f}" if d else "nan"

        row = [hms, n_los, n_l5,
               f"{q(dI,0.5):.4f}", f"{q(dI,0.99):.4f}",
               f"{q(icres,0.5):.3f}", f"{q(icres,0.99):.3f}", lli, mwjump,
               r_wl["n"] if r_wl else 0, fmt(r_wl, "med"), fmt(r_wl, "R"),
               r_ewl["n"] if r_ewl else 0, fmt(r_ewl, "med"), fmt(r_ewl, "R"),
               r_brg["n"] if r_brg else 0, fmt(r_brg, "med"), fmt(r_brg, "R"),
               f"{q(bi,0.5):.3f}", f"{q(bi,0.99):.3f}"]
        rows.append(row)
        print(",".join(str(x) for x in row))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as fh:
            fh.write(",".join(cols) + "\n")
            for r in rows:
                fh.write(",".join(str(x) for x in r) + "\n")
        print(f"# wrote {a.out}", file=sys.stderr)
    return 0


# ------------------------------------------------------- bridge oracle

BUCKETS = [("<100km", 0, 100), ("100-400km", 100, 400), ("400-1500km", 400, 1500),
           ("1.5-4Mm", 1500, 4000), ("4-12Mm", 4000, 12000)]


def cmd_oracle(a):
    """Baseline-bucketed EWL->WL bridge oracle, graded against a long-arc MW(1,2) truth.

    Answers SPEC_ewl_wl_cascade_card §11 Q2 (is the bridge's iono term local?) and, by
    de-biasing with a phase-levelled CODE iono of known accuracy, what accuracy the
    de-bias input must have for the bridge to deliver an integer.
    """
    tag = f"{a.start.replace(':','')}_{a.end.replace(':','')}"
    blob = pickle.load(open(os.path.join(CACHE_DIR, f"census_{tag}.pkl"), "rb"))
    sta, (s0, s1, step) = blob["sta"], blob["window"]
    t0 = _hms(s0)
    e_lo = int(round((_hms(a.win_start) - t0) / step))
    e_hi = int(round((_hms(a.win_end) - t0) / step))
    print(f"# oracle window {a.win_start}..{a.win_end}  (slots {e_lo}..{e_hi})  "
          f"n={a.n} trailing epochs, truth = whole-window MW(1,2) median")

    # keep only stations with L5 data, grouped by (c2,c5) channel tuple
    pool = {n: d for n, d in sta.items() if d["chan"][2] and d["l5_epochs"] > 0 and d["pos"]}
    grp = defaultdict(list)
    for n, d in pool.items():
        grp[(d["chan"][1], d["chan"][2])].append(n)
    names = max(grp.values(), key=len)
    print(f"# channel group {max(grp, key=lambda k: len(grp[k]))}: {len(names)} stations")

    # --- per-(sta,sat) truth WL from the whole window; drop slipped/short arcs
    truth, lev = {}, {}
    for n in names:
        for s, r in sta[n]["sats"].items():
            v = [x for x in r["mw12"] if x is not None]
            if len(v) < a.truth_min:
                continue
            m = st.median(v)
            mad = st.median(sorted(abs(x - m) for x in v))
            if mad > 0.6:                       # slipped / multi-arc -> unusable as truth
                continue
            truth[(n, s)] = m
            dif = [c - p for c, p in zip(r["icode"], r["iphase"])
                   if c is not None and p is not None]
            if len(dif) >= 20:
                lev[(n, s)] = st.median(dif)

    def dist_km(x, y):
        return math.dist(pool[x]["pos"], pool[y]["pos"]) / 1e3

    rng = __import__("random").Random(129)
    pairs = defaultdict(list)
    for label, lo, hi in BUCKETS:
        cand = []
        for i, x in enumerate(names):
            for y in names[i + 1:]:
                d = dist_km(x, y)
                if lo <= d < hi:
                    cand.append((x, y, d))
        rng.shuffle(cand)
        pairs[label] = cand[:a.max_pairs]

    gated = {}
    hdr = (f"{'bucket':<12}{'pairs':>6}{'DDs':>7}{'|DDiono|med':>12}"
           f"{'BRG med|f|':>11}{'BRG R':>7}{'BRGhit':>8}"
           f"{'DBG med|f|':>11}{'DBG R':>7}{'DBGhit':>8}{'MW10 R':>8}{'MW10hit':>9}")
    print(hdr)
    for label, _, _ in BUCKETS:
        plist = pairs[label]
        if not plist:
            continue
        brg, dbg, mw10, ionos = [], [], [], []
        hit_b = hit_d = hit_m = tot = 0
        for x, y, d in plist:
            sx, sy = sta[x]["sats"], sta[y]["sats"]
            common = [s for s in sx if s in sy and (x, s) in truth and (y, s) in truth
                      and (x, s) in lev and (y, s) in lev]
            if len(common) < 2:
                continue
            ref = common[0]
            for s in common[1:]:
                def tm(stn, sat, key):
                    return trailing_median(sta[stn]["sats"][sat][key], e_hi, a.n)

                def dd(key):
                    v = [tm(x, s, key), tm(x, ref, key), tm(y, s, key), tm(y, ref, key)]
                    return None if None in v else (v[0] - v[1]) - (v[2] - v[3])

                e25, gfb = dd("mw25"), dd("gfb")
                if e25 is None or gfb is None or abs(e25 - round(e25)) > 0.20:
                    continue
                n_truth = ((truth[(x, s)] - truth[(x, ref)])
                           - (truth[(y, s)] - truth[(y, ref)]))
                if abs(n_truth - round(n_truth)) > 0.25:      # truth itself not integer
                    continue
                n_truth = round(n_truth)
                # phase-levelled CODE iono (accuracy ~ code noise / sqrt(arc))
                ip = []
                for stn, sat in ((x, s), (x, ref), (y, s), (y, ref)):
                    v = trailing_median(sta[stn]["sats"][sat]["iphase"], e_hi, a.n)
                    ip.append(None if v is None else v + lev[(stn, sat)])
                if None in ip:
                    continue
                i_dd = (ip[0] - ip[1]) - (ip[2] - ip[3])
                z = (gfb + LAM25 * round(e25)) / LAM12
                zd = z + IONO_CYC_PER_M * i_dd
                m12 = dd("mw12")
                if m12 is None:
                    continue
                tot += 1
                ionos.append(abs(i_dd))
                brg.append(z - n_truth)
                dbg.append(zd - n_truth)
                mw10.append(m12 - n_truth)
                hit_b += (round(z) == n_truth)
                hit_d += (round(zd) == n_truth)
                hit_m += (round(m12) == n_truth)
        if not tot:
            print(f"{label:<12}{len(plist):>6}{0:>7}")
            continue
        fb, fd, fm = frac_stats(brg), frac_stats(dbg), frac_stats(mw10)
        print(f"{label:<12}{len(plist):>6}{tot:>7}{st.median(ionos):>12.2f}"
              f"{fb['med']:>11.3f}{fb['R']:>7.3f}{hit_b/tot:>8.3f}"
              f"{fd['med']:>11.3f}{fd['R']:>7.3f}{hit_d/tot:>8.3f}"
              f"{fm['R']:>8.3f}{hit_m/tot:>9.3f}")
        gated[label] = (brg, dbg, mw10)
    print("# BRG = raw bridge; DBG = bridge de-biased with phase-levelled CODE iono;")
    print("# MW10 = plain MW(1,2) trailing median. hit = fraction landing on the truth "
          "integer. uniform: med|f|=0.25 R=0.00 hit=0.50/(2*0.5)")

    # ---- the shipped GATE applied: what actually reaches the stiff carry ----
    print(f"\n# --- shipped FFR gate |z-round(z)| < {a.frac_gate} applied "
          f"(network.mw_wl.wl_fix_frac_cyc) ---")
    print(f"{'bucket':<12}{'BRG pass%':>10}{'BRG WRONG%':>12}{'DBG pass%':>10}"
          f"{'DBG WRONG%':>12}{'MW pass%':>10}{'MW WRONG%':>11}")
    for label, _, _ in BUCKETS:
        if label not in gated:
            continue
        cells = []
        for err in gated[label]:
            passed = [e for e in err if abs(e - round(e)) < a.frac_gate]
            if not passed:
                cells += [float("nan"), float("nan")]
                continue
            wrong = sum(1 for e in passed if round(e) != 0)
            cells += [100.0 * len(passed) / len(err), 100.0 * wrong / len(passed)]
        print(f"{label:<12}" + "".join(f"{c:>10.1f}{'':>2}" if i % 2 == 0 else f"{c:>10.1f}{'':>1}"
                                       for i, c in enumerate(cells)))
    print("# WRONG% = of the decisions the gate ADMITS, the fraction that are NOT the truth "
          "integer.\n# These are the ones that would be written into the stiff qz carry "
          "(standing invariant #1).")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("parse", "analyze", "oracle"):
        s = sub.add_parser(c)
        s.add_argument("--start", default="18:00:00")
        s.add_argument("--end", default="21:30:00")
        s.add_argument("--step", type=float, default=30.0)
        if c == "parse":
            s.add_argument("--jobs", type=int, default=8)
        elif c == "analyze":
            s.add_argument("--win", type=int, default=10,
                           help="trailing arc-median window in epochs (detector proxy)")
            s.add_argument("--out", default="")
        else:
            s.add_argument("--win-start", default="19:26:00")
            s.add_argument("--win-end", default="19:31:00")
            s.add_argument("--n", type=int, default=10)
            s.add_argument("--truth-min", type=int, default=200)
            s.add_argument("--max-pairs", type=int, default=400)
            s.add_argument("--frac-gate", type=float, default=0.15,
                           help="shipped network.mw_wl.wl_fix_frac_cyc")
    a = ap.parse_args()
    return {"parse": cmd_parse, "analyze": cmd_analyze, "oracle": cmd_oracle}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
