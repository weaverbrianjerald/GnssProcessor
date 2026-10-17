"""Estimator-free characterization of GPS L5 carrier phase and C5 code from raw RINEX 3.

The instrument is the triple-carrier geometry-free and first-order-ionosphere-free
phase combination, reported in L5-equivalent metres:

    G5 = b1*PHI1 + b2*PHI2 + PHI5        (metres)

with sum(b_i) = 0 and sum(b_i * (f1/f_i)^2) = 0, scaled so the L5 coefficient is 1.
That is the combination -0.2851*PHI1 + 1.5457*PHI2 - 1.2606*PHI5 divided by its own
L5 coefficient, so an error e on L5 phase alone reads as e.

Geometry, clocks (including receiver millisecond steps), troposphere and first-order
ionosphere cancel exactly. What remains is the per-arc constant (ambiguities, phase
shifts), cycle slips, the satellite inter-frequency clock variation, inter-band antenna
phase-centre differences, higher-order ionosphere, multipath and noise.

The same coefficients applied to the three code observables give the band-5 code-bias
inconsistency (per-arc mean) and the code noise/multipath (per-arc scatter).

No estimator, no orbit, no clock product is used.

Usage:
    python scripts/ab2_l5_raw_read.py --selftest
    python scripts/ab2_l5_raw_read.py --out-dir C:/GnssProcessor/runs/_ab2_l5raw_20260920
"""

import argparse
import csv
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rinex_ids import norm_sat  # noqa: E402

# ---------------------------------------------------------------------------
# constants
# ---------------------------------------------------------------------------

C_LIGHT = 299792458.0
F1 = 1575.42e6
F2 = 1227.60e6
F5 = 1176.45e6
LAM1 = C_LIGHT / F1
LAM2 = C_LIGHT / F2
LAM5 = C_LIGHT / F5

# geometry-free + first-order-iono-free, normalized so the L5 coefficient is 1.
# g_i = (f1/f_i)^2 is the first-order ionospheric scaling of band i relative to band 1.
_G2 = (F1 / F2) ** 2
_G5 = (F1 / F5) ** 2
B2 = -(_G5 - 1.0) / (_G2 - 1.0)
B1 = -1.0 - B2
B5 = 1.0

# one-cycle slip signatures in L5-equivalent metres
STEP_L5 = B5 * LAM5
STEP_L2 = B2 * LAM2
STEP_L1 = B1 * LAM1

EPOCH_S = 30.0
ARC_GAP_S = 60.0
JUMP_THRESH_M = 0.04
INT_TOL_M = 0.02
# The L1 lattice spacing in this combination (0.0430 m) is barely wider than the
# detection threshold, so a 0.02 m tolerance would claim almost the whole line for
# some L1 multiple. The L1 class is gated at a quarter of its own step instead.
INT_TOL_L1_M = 0.010
SHORT_WIN_EP = 20   # 10 minutes at 30 s
SMOOTH_WIN_EP = 10  # 5 minutes at 30 s
MIN_ARC_EP = 10
MIN_CTRL_ARCS = 3

L5_IIF = ["G03", "G06", "G08", "G09", "G10", "G24", "G25", "G26", "G27", "G30", "G32"]
L5_III = ["G04", "G11", "G14", "G18", "G23", "G28"]
L5_SATS = sorted(L5_IIF + L5_III)
BLOCK = dict([(s, "IIF") for s in L5_IIF] + [(s, "III") for s in L5_III])

SUSPECTS = ["KIT300UZB", "BSHM00ISR", "GAMB00PYF", "ULAB00MNG", "VOIM00MDG", "JPLM00USA",
            "BOGT00COL", "YKRO00CIV", "STHL00GBR", "MAC100AUS", "LMMF00MTQ"]
# 7 named healthy stations plus 5 chosen here for a mix of the two L5 tracking modes
# (Q: Septentrio/Leica family, X: Javad/Trimble family) and of latitude band.
CONTROLS = ["GRAZ00AUT", "DJIG00DJI", "MIZU00JPN", "GCGO00USA", "MBAR00UGA", "PALM00ATA",
            "DARW00AUS", "CEBR00ESP", "STJO00CAN", "CAS100ATA", "SGOC00LKA", "POTS00DEU"]

HDR_TYPES = "SYS / # / OBS TYPES"
HDR_END = "END OF HEADER"


# ---------------------------------------------------------------------------
# RINEX 3 reader (minimal, GPS only, leading window of the day)
# ---------------------------------------------------------------------------

def gps_obs_types(path):
    """Return the ordered GPS observation-type list from the RINEX 3 header."""
    codes, n = [], None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            label = line[60:].strip()
            if label == HDR_TYPES:
                if line[0] == "G":
                    n = int(line[3:6])
                    codes = line[7:60].split()
                elif line[0] == " " and n is not None and len(codes) < n:
                    codes += line[7:60].split()
            elif label == HDR_END:
                break
    if n is not None and len(codes) != n:
        raise ValueError("GPS obs type count mismatch in %s" % path)
    return codes


def pick_types(codes, priorities):
    """First priority-list entry present in the file, per (observable letter, band)."""
    chosen = {}
    for want in priorities:
        key = (want[0], want[1])
        if key in chosen:
            continue
        if want in codes:
            chosen[key] = want
    return chosen


def read_station(path, priorities, hours, sats):
    """Read GPS phase and code for `sats` over the first `hours` of the day.

    Returns (types, data) where data[sat] is a dict of epoch-indexed arrays:
    phi1/phi2/phi5 in metres, c1/c2/c5 in metres, lli1/lli2/lli5 as ints,
    all of length nep = hours*3600/30 with NaN / -1 where absent.
    """
    codes = gps_obs_types(path)
    types = pick_types(codes, priorities)
    need = [("L", "1"), ("L", "2"), ("L", "5"), ("C", "1"), ("C", "2"), ("C", "5")]
    missing = [k for k in need if k not in types]
    if missing:
        return types, {}, missing
    idx = dict((k, codes.index(types[k])) for k in need)
    width = 3 + 16 * len(codes)
    nep = int(hours * 3600 / EPOCH_S)
    want = set(sats)

    data = {}
    for s in sats:
        data[s] = {"phi1": np.full(nep, np.nan), "phi2": np.full(nep, np.nan),
                   "phi5": np.full(nep, np.nan), "c1": np.full(nep, np.nan),
                   "c2": np.full(nep, np.nan), "c5": np.full(nep, np.nan),
                   "lli1": np.zeros(nep, dtype=np.int8), "lli2": np.zeros(nep, dtype=np.int8),
                   "lli5": np.zeros(nep, dtype=np.int8)}

    ep = -1
    with open(path, "r", errors="replace") as fh:
        in_header = True
        for line in fh:
            if in_header:
                if line[60:].strip() == HDR_END:
                    in_header = False
                continue
            if line[0] == ">":
                hh = int(line[13:15])
                mm = int(line[16:18])
                ss = float(line[18:29])
                sod = hh * 3600 + mm * 60 + ss
                if sod >= hours * 3600:
                    break
                ep = int(round(sod / EPOCH_S))
                if ep < 0 or ep >= nep or abs(sod - ep * EPOCH_S) > 1e-6:
                    ep = -1  # off-grid epoch: skip its records
                flag = int(line[31]) if line[31].isdigit() else 0
                if flag > 1:
                    nskip = int(line[32:35])
                    for _ in range(nskip):
                        next(fh, None)
                    ep = -1
                continue
            if ep < 0 or line[0] != "G":
                continue
            sat = norm_sat(line[:3])
            if sat not in want:
                continue
            pad = line.rstrip("\n").ljust(width)
            rec = data[sat]
            for key, dst, lli in (("L1", "phi1", "lli1"), ("L2", "phi2", "lli2"),
                                  ("L5", "phi5", "lli5"), ("C1", "c1", None),
                                  ("C2", "c2", None), ("C5", "c5", None)):
                k = (key[0], key[1])
                off = 3 + 16 * idx[k]
                raw = pad[off:off + 14]
                if not raw.strip():
                    continue
                val = float(raw)
                if key[0] == "L":
                    lam = {"1": LAM1, "2": LAM2, "5": LAM5}[key[1]]
                    rec[dst][ep] = val * lam
                    ch = pad[off + 14]
                    if ch.isdigit():
                        rec[lli][ep] = int(ch)
                else:
                    rec[dst][ep] = val
    return types, data, []


# ---------------------------------------------------------------------------
# the combination and the per-arc statistics
# ---------------------------------------------------------------------------

def gfif(v1, v2, v5):
    """Geometry-free, first-order-iono-free triple-band combination, L5-equivalent metres."""
    return B1 * v1 + B2 * v2 + B5 * v5


def moving_mean(x, win):
    """Centred moving mean over a window of `win` samples, NaN-aware, edge-shrinking."""
    n = len(x)
    ok = np.isfinite(x).astype(float)
    xf = np.where(np.isfinite(x), x, 0.0)
    cs = np.concatenate(([0.0], np.cumsum(xf)))
    cn = np.concatenate(([0.0], np.cumsum(ok)))
    half = win // 2
    lo = np.clip(np.arange(n) - half, 0, n)
    hi = np.clip(np.arange(n) - half + win, 0, n)
    num = cs[hi] - cs[lo]
    den = cn[hi] - cn[lo]
    out = np.full(n, np.nan)
    good = den > 0
    out[good] = num[good] / den[good]
    return out


def classify_step(d):
    """Classify a GFIF step: which single band's integer slip explains it.

    Returns (cls, ncyc, resid_m). cls in {l5_int, l2_int, l1_int, noninteger}.

    Candidates are ranked by cycle count first and residual second: 6 L1 cycles match
    1 L5 cycle to 3.4 mm, which is inside the phase noise, so a residual-first rule
    would relabel most single L5 slips as six-cycle L1 slips.
    """
    best = ("noninteger", 0, abs(d))
    cands = []
    for cls, step, tol in (("l5_int", STEP_L5, INT_TOL_M), ("l2_int", STEP_L2, INT_TOL_M),
                           ("l1_int", STEP_L1, INT_TOL_L1_M)):
        n = int(round(d / step))
        if n == 0:
            continue
        resid = abs(d - n * step)
        if resid <= tol:
            cands.append((abs(n), resid, cls, n))
    if cands:
        cands.sort(key=lambda t: (t[0], t[1]))
        _, resid, cls, n = cands[0]
        best = (cls, n, resid)
    return best


def find_arcs(mask, ep_index):
    """Contiguous runs of `mask` split where the epoch spacing exceeds ARC_GAP_S."""
    arcs, cur = [], []
    for i in np.flatnonzero(mask):
        if cur and (ep_index[i] - ep_index[cur[-1]]) * EPOCH_S > ARC_GAP_S:
            arcs.append(np.array(cur))
            cur = []
        cur.append(i)
    if cur:
        arcs.append(np.array(cur))
    return [a for a in arcs if len(a) >= MIN_ARC_EP]


def analyse_arc(idx, g5, gcode, lli1, lli2, lli5):
    """Jump detection, re-levelling and the arc statistics. Returns (stats, jumps, series)."""
    x = g5[idx]
    dt = np.diff(idx) * EPOCH_S
    d = np.diff(x)
    jumps = []
    level = np.zeros(len(x))
    for k in np.flatnonzero(np.abs(d) > JUMP_THRESH_M):
        cls, ncyc, resid = classify_step(d[k])
        jumps.append({"ep": int(idx[k + 1]), "dt_s": float(dt[k]), "step_m": float(d[k]),
                      "cls": cls, "ncyc": int(ncyc), "resid_m": float(resid),
                      "lli1": int(lli1[idx[k + 1]] & 1), "lli2": int(lli2[idx[k + 1]] & 1),
                      "lli5": int(lli5[idx[k + 1]] & 1)})
        level[k + 1:] += d[k]
    xr = x - level

    keep = np.abs(d) <= JUMP_THRESH_M
    td = d[keep] if keep.any() else np.array([])
    noise_rms = float(np.sqrt(np.mean(td ** 2))) if len(td) else float("nan")

    short = xr - moving_mean(xr, SHORT_WIN_EP)
    short_rms = float(np.sqrt(np.nanmean(short ** 2)))
    sm = moving_mean(xr, SMOOTH_WIN_EP)
    drift_range = float(np.nanmax(sm) - np.nanmin(sm))

    cg = gcode[idx]
    fin = np.isfinite(cg)
    code_mean = float(np.mean(cg[fin])) if fin.any() else float("nan")
    code_rms = float(np.std(cg[fin])) if fin.sum() > 1 else float("nan")

    stats = {"n_ep": len(x), "ep0": int(idx[0]), "ep1": int(idx[-1]),
             "n_jump": len(jumps),
             "n_l5": sum(1 for j in jumps if j["cls"] == "l5_int"),
             "n_l2": sum(1 for j in jumps if j["cls"] == "l2_int"),
             "n_l1": sum(1 for j in jumps if j["cls"] == "l1_int"),
             "n_non": sum(1 for j in jumps if j["cls"] == "noninteger"),
             "n_l5_lli": sum(1 for j in jumps if j["cls"] == "l5_int" and j["lli5"]),
             "n_lli_any": sum(1 for j in jumps if j["lli1"] or j["lli2"] or j["lli5"]),
             "noise_rms_m": noise_rms, "short_rms_m": short_rms,
             "drift_range_m": drift_range, "code_mean_m": code_mean, "code_rms_m": code_rms}
    return stats, jumps, xr, sm


# ---------------------------------------------------------------------------
# self test
# ---------------------------------------------------------------------------

def selftest():
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("%-46s %s %s" % (name, "PASS" if cond else "FAIL", detail))
        if not cond:
            ok = False

    # coefficients
    check("sum(b) == 0", abs(B1 + B2 + B5) < 1e-12, "%.17g" % (B1 + B2 + B5))
    ion = B1 * 1.0 + B2 * _G2 + B5 * _G5
    check("sum(b*(f1/fi)^2) == 0", abs(ion) < 1e-12, "%.3e" % ion)
    # the scale quoted in the card: dividing (-0.2851, 1.5457, -1.2606) by -1.2606
    check("b1 matches -0.2851/-1.2606", abs(B1 - 0.2851 / 1.2606) < 5e-4, "b1=%.6f" % B1)
    check("b2 matches 1.5457/-1.2606", abs(B2 + 1.5457 / 1.2606) < 5e-4, "b2=%.6f" % B2)
    check("L5 one-cycle step 0.2548 m", abs(STEP_L5 - 0.2548) < 5e-5, "%.6f m" % STEP_L5)
    check("L2 one-cycle step 0.2994 m", abs(abs(STEP_L2) - 0.2994) < 5e-5, "%.6f m" % STEP_L2)
    check("L1 one-cycle step 0.0430 m", abs(STEP_L1 - 0.0430) < 5e-5, "%.6f m" % STEP_L1)

    # exact cancellation of an arbitrary geometry term and an arbitrary 1/f^2 term.
    # The 1e-9 m absolute bar is not attainable in float64 at full pseudorange
    # magnitude: at |geo| = 2.4e7 m the double rounding floor of a three-term sum with
    # O(1) coefficients is about 4*eps*|geo| = 2e-8 m. The bar is therefore checked as
    # 1e-9 m at |geo| <= 1e6 m, and as the float64 floor at full magnitude.
    rng = np.random.default_rng(20260920)
    tec = rng.uniform(-80.0, 80.0, 500)
    i1 = 40.3e16 * tec / F1 ** 2  # up to ~13 m of L1 first-order ionosphere

    def combo_resid(geo, sign):
        return gfif(geo + sign * i1, geo + sign * i1 * _G2, geo + sign * i1 * _G5)

    small = rng.uniform(-1.0e6, 1.0e6, 500)
    r_small = np.max(np.abs(combo_resid(small, -1.0)))
    check("cancels geometry + 1/f^2 to 1e-9 m", r_small < 1e-9, "max %.3e m (|geo|<=1e6 m)" % r_small)
    full = rng.uniform(2.0e7, 2.6e7, 500)
    r_full = np.max(np.abs(combo_resid(full, -1.0)))
    floor = 4.0 * np.finfo(float).eps * np.max(full)
    check("at 2.4e7 m: at the float64 rounding floor", r_full < floor,
          "max %.3e m vs floor %.3e m" % (r_full, floor))
    r_code = np.max(np.abs(combo_resid(small, +1.0)))
    check("same on codes (iono sign flipped)", r_code < 1e-9, "max %.3e m" % r_code)

    # synthetic arc: one L5 cycle slip, one L2 cycle slip, a slow drift, white noise
    n = 400
    ep = np.arange(n)
    sigma = 0.004
    drift_amp = 0.10
    drift = drift_amp * (ep / (n - 1.0))
    noise = rng.normal(0.0, sigma, n)
    truth = drift + noise
    truth[150:] += STEP_L5 * 1.0      # +1 cycle on L5 alone
    truth[280:] += STEP_L2 * -2.0     # -2 cycles on L2 alone
    g5 = np.full(720, np.nan)
    g5[:n] = truth
    gcode = np.full(720, np.nan)
    code_bias, code_sigma = -0.62, 0.55
    gcode[:n] = code_bias + rng.normal(0.0, code_sigma, n)
    lli = np.zeros(720, dtype=np.int8)
    lli[150] = 1
    st, jm, xr, sm = analyse_arc(ep, g5, gcode, lli, lli * 0, lli)

    check("finds exactly 2 jumps", st["n_jump"] == 2, str([j["cls"] for j in jm]))
    if len(jm) == 2:
        check("jump 1 = L5, n=+1, at ep 150",
              jm[0]["cls"] == "l5_int" and jm[0]["ncyc"] == 1 and jm[0]["ep"] == 150,
              "%s n=%d ep=%d" % (jm[0]["cls"], jm[0]["ncyc"], jm[0]["ep"]))
        check("jump 1 carries the L5 LLI bit", jm[0]["lli5"] == 1)
        check("jump 2 = L2, n=-2, at ep 280",
              jm[1]["cls"] == "l2_int" and jm[1]["ncyc"] == -2 and jm[1]["ep"] == 280,
              "%s n=%d ep=%d" % (jm[1]["cls"], jm[1]["ncyc"], jm[1]["ep"]))
        check("jump 2 has no LLI bit", jm[1]["lli5"] == 0)
    check("re-levelled series is continuous",
          np.max(np.abs(np.diff(xr))) < 10 * sigma,
          "max |d| %.4f m" % np.max(np.abs(np.diff(xr))))
    exp_td = sigma * math.sqrt(2.0)
    check("30 s TD RMS recovers sqrt(2)*sigma",
          abs(st["noise_rms_m"] - exp_td) < 0.25 * exp_td,
          "%.4f vs %.4f m" % (st["noise_rms_m"], exp_td))
    check("short-term RMS near sigma", abs(st["short_rms_m"] - sigma) < 0.4 * sigma,
          "%.4f vs %.4f m" % (st["short_rms_m"], sigma))
    check("drift range recovers %.2f m" % drift_amp,
          abs(st["drift_range_m"] - drift_amp) < 0.25 * drift_amp,
          "%.4f m" % st["drift_range_m"])
    check("code-combination mean recovers the bias",
          abs(st["code_mean_m"] - code_bias) < 0.15,
          "%.3f vs %.3f m" % (st["code_mean_m"], code_bias))
    check("code-combination scatter recovers sigma",
          abs(st["code_rms_m"] - code_sigma) < 0.2 * code_sigma,
          "%.3f vs %.3f m" % (st["code_rms_m"], code_sigma))

    # a clean arc must produce no jumps
    clean = np.full(720, np.nan)
    clean[:n] = rng.normal(0.0, sigma, n)
    st2, jm2, _, _ = analyse_arc(ep, clean, gcode, lli * 0, lli * 0, lli * 0)
    check("clean arc yields zero jumps", st2["n_jump"] == 0, str(st2["n_jump"]))

    # a single L1 cycle slip must not be read as L5 or L2, and 1 L5 cycle must not be
    # stolen by the six-L1-cycle alias
    c5a, _, _ = classify_step(STEP_L5)
    c1a, n1a, _ = classify_step(STEP_L1)
    c2a, n2a, _ = classify_step(3.0 * STEP_L2)
    check("1 L5 cycle beats the 6 x L1 alias", c5a == "l5_int", c5a)
    check("1 L1 cycle classifies as l1_int", c1a == "l1_int" and n1a == 1, "%s n=%d" % (c1a, n1a))
    check("3 L2 cycles classify as l2_int", c2a == "l2_int" and n2a == 3, "%s n=%d" % (c2a, n2a))
    check("0.10 m step is non-integer", classify_step(0.10)[0] == "noninteger",
          classify_step(0.10)[0])

    print("\nSELFTEST %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

def lon_deg(ecef):
    return math.degrees(math.atan2(ecef[1], ecef[0]))


def load_arm(path):
    import json
    with open(path) as fh:
        arm = json.load(fh)
    stations = arm["network"]["stations"]
    prio = arm["processing"]["signal_priorities"]["GPS"]
    return stations, prio


def process(stations, prio, hours, tag, out):
    """Read and analyse a list of stations. Fills out['arcs'], out['jumps'], out['series']."""
    for st in stations:
        name, path = st["name"], st["rinex_path"]
        if not os.path.exists(path):
            print("  %-10s MISSING %s" % (name, path))
            continue
        types, data, missing = read_station(path, prio, hours, L5_SATS)
        if missing:
            print("  %-10s no %s in header" % (name, missing))
            continue
        nep = int(hours * 3600 / EPOCH_S)
        ep_all = np.arange(nep)
        narc = 0
        n12 = n125 = n_drop_ev = 0
        for sat in L5_SATS:
            rec = data[sat]
            g5 = gfif(rec["phi1"], rec["phi2"], rec["phi5"])
            gc = gfif(rec["c1"], rec["c2"], rec["c5"])
            mask = np.isfinite(g5)
            # band-5 specific data loss: L1 and L2 tracked, L5 not
            m12 = np.isfinite(rec["phi1"]) & np.isfinite(rec["phi2"])
            n12 += int(m12.sum())
            n125 += int((m12 & np.isfinite(rec["phi5"])).sum())
            lost = m12 & ~np.isfinite(rec["phi5"])
            n_drop_ev += int(np.sum(lost[1:] & ~lost[:-1] & m12[:-1]))
            if mask.sum() < MIN_ARC_EP:
                continue
            for idx in find_arcs(mask, ep_all):
                stats, jumps, xr, sm = analyse_arc(idx, g5, gc, rec["lli1"], rec["lli2"],
                                                   rec["lli5"])
                stats.update({"tag": tag, "station": name, "sat": sat,
                              "block": BLOCK[sat], "l5_type": types[("L", "5")],
                              "l1_type": types[("L", "1")], "l2_type": types[("L", "2")]})
                aid = len(out["arcs"])
                stats["arc_id"] = aid
                out["arcs"].append(stats)
                for j in jumps:
                    j.update({"tag": tag, "station": name, "sat": sat, "arc_id": aid})
                    out["jumps"].append(j)
                out["series"].append({"arc_id": aid, "tag": tag, "station": name, "sat": sat,
                                      "idx": idx, "sm": sm, "raw": xr})
                narc += 1
        out["cover"][name] = {"n_ep_l12": n12, "n_ep_l125": n125,
                              "l5_dropout_pct": 100.0 * (1.0 - n125 / max(n12, 1)),
                              "n_l5_dropout_ev": n_drop_ev}
        print("  %-10s L1=%s L2=%s L5=%s arcs=%d L5-dropout=%.1f%%"
              % (name, types[("L", "1")], types[("L", "2")], types[("L", "5")], narc,
                 100.0 * (1.0 - n125 / max(n12, 1))))
    return out


def sat_common(out, controls, nep):
    """Across-control-station median of the mean-removed smoothed series, per satellite."""
    common = {}
    for sat in L5_SATS:
        rows = []
        for s in out["series"]:
            if s["station"] not in controls or s["sat"] != sat:
                continue
            v = np.full(nep, np.nan)
            sm = s["sm"]
            fin = np.isfinite(sm)
            if fin.sum() < MIN_ARC_EP:
                continue
            v[s["idx"][fin]] = sm[fin] - np.mean(sm[fin])
            rows.append(v)
        if len(rows) < MIN_CTRL_ARCS:
            continue
        m = np.vstack(rows)
        cnt = np.sum(np.isfinite(m), axis=0)
        with np.errstate(invalid="ignore"):
            med = np.nanmedian(m, axis=0)
        med[cnt < MIN_CTRL_ARCS] = np.nan
        common[sat] = med
    return common


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--arm", default="C:/GnssProcessor/runs/_ab2_field6_T3/arm.json")
    ap.add_argument("--sim-arm", default="C:/GnssProcessor/runs/_ab2_sim6_T3/arm.json")
    ap.add_argument("--sim-n", type=int, default=3)
    ap.add_argument("--hours", type=float, default=6.0)
    ap.add_argument("--pf-rows", default="")
    ap.add_argument("--out-dir", default="C:/GnssProcessor/runs/_ab2_l5raw_20260920")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    nep = int(args.hours * 3600 / EPOCH_S)
    os.makedirs(args.out_dir, exist_ok=True)
    out = {"arcs": [], "jumps": [], "series": [], "cover": {}}

    stations, prio = load_arm(args.arm)
    by_name = dict((s["name"], s) for s in stations)
    wanted = [n for n in SUSPECTS + CONTROLS]
    missing_cfg = [n for n in wanted if n not in by_name]
    if missing_cfg:
        raise SystemExit("not in the arm roster: %s" % missing_cfg)
    print("FIELD")
    process([by_name[n] for n in wanted], prio, args.hours, "field", out)

    print("SIM")
    sim_st, sim_prio = load_arm(args.sim_arm)
    process(sim_st[:args.sim_n], sim_prio, args.hours, "sim", out)
    sim_names = [s["name"] for s in sim_st[:args.sim_n]]

    # satellite-common term from the field control stations
    common = sat_common(out, set(CONTROLS), nep)
    sim_common = sat_common(out, set(sim_names), nep)

    # excess per arc
    for s in out["series"]:
        ref = sim_common if s["tag"] == "sim" else common
        cm = ref.get(s["sat"])
        arc = out["arcs"][s["arc_id"]]
        arc["excess_rms_m"] = float("nan")
        arc["excess_range_m"] = float("nan")
        if cm is None:
            continue
        sm = s["sm"]
        c = cm[s["idx"]]
        fin = np.isfinite(sm) & np.isfinite(c)
        if fin.sum() < MIN_ARC_EP:
            continue
        e = sm[fin] - c[fin]
        e = e - np.mean(e)
        arc["excess_rms_m"] = float(np.sqrt(np.mean(e ** 2)))
        arc["excess_range_m"] = float(np.max(e) - np.min(e))
        # per-hour excess for the time-structure table
        hrs = (s["idx"][fin] * EPOCH_S) / 3600.0
        arc["excess_by_hour"] = [(int(h), float(v)) for h, v in zip(hrs.astype(int), e)]

    # ---- per-arc CSV
    arc_cols = ["tag", "station", "sat", "block", "arc_id", "l1_type", "l2_type", "l5_type",
                "ep0", "ep1", "n_ep", "n_jump", "n_l5", "n_l2", "n_l1", "n_non", "n_l5_lli",
                "n_lli_any", "noise_rms_m", "short_rms_m", "drift_range_m", "excess_rms_m",
                "excess_range_m", "code_mean_m", "code_rms_m"]
    with open(os.path.join(args.out_dir, "per_arc.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, arc_cols, extrasaction="ignore")
        w.writeheader()
        for a in out["arcs"]:
            w.writerow(a)

    # ---- jumps CSV
    jump_cols = ["tag", "station", "sat", "arc_id", "ep", "hour", "dt_s", "step_m", "cls",
                 "ncyc", "resid_m", "lli1", "lli2", "lli5"]
    with open(os.path.join(args.out_dir, "jumps.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, jump_cols, extrasaction="ignore")
        w.writeheader()
        for j in out["jumps"]:
            j["hour"] = int(j["ep"] * EPOCH_S // 3600)
            w.writerow(j)

    # ---- satellite-common CSV
    with open(os.path.join(args.out_dir, "sat_common.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["tag", "sat", "block", "n_ep", "range_m", "rms_m", "n_ctrl_arcs"])
        for tag, ref, pool in (("field", common, set(CONTROLS)), ("sim", sim_common, set(sim_names))):
            for sat in sorted(ref):
                v = ref[sat]
                fin = np.isfinite(v)
                nctrl = len(set(s["station"] for s in out["series"]
                                if s["sat"] == sat and s["station"] in pool))
                w.writerow([tag, sat, BLOCK[sat], int(fin.sum()),
                            "%.4f" % (np.max(v[fin]) - np.min(v[fin])) if fin.any() else "",
                            "%.4f" % np.sqrt(np.mean(v[fin] ** 2)) if fin.any() else "",
                            nctrl])

    # ---- elevation join (optional)
    elev = {}
    if args.pf_rows and os.path.exists(args.pf_rows):
        import pandas as pd
        keep = set(SUSPECTS + CONTROLS)
        tow0 = 259200.0
        for chunk in pd.read_csv(args.pf_rows, usecols=["tow", "station", "sat", "elev_deg"],
                                 chunksize=2000000):
            chunk = chunk[chunk["station"].isin(keep)]
            if chunk.empty:
                continue
            for tow, sta, sat, el in chunk.itertuples(index=False):
                ep = int(round((tow - tow0) / EPOCH_S))
                if 0 <= ep < nep:
                    elev[(sta, sat, ep)] = el

    # ---- satellite reference for the code combination: the across-control median of the
    # per-arc code mean, so a station's residual is its own (receiver) band-5 code bias.
    sat_code_ref = {}
    for tag, pool in (("field", set(CONTROLS)), ("sim", set(sim_names))):
        for sat in L5_SATS:
            v = [a["code_mean_m"] for a in out["arcs"]
                 if a["tag"] == tag and a["station"] in pool and a["sat"] == sat
                 and np.isfinite(a["code_mean_m"])]
            if len(v) >= MIN_CTRL_ARCS:
                sat_code_ref[(tag, sat)] = float(np.median(v))

    # ---- per-station CSV
    per_station_rows = []
    groups = {}
    for a in out["arcs"]:
        groups.setdefault((a["tag"], a["station"]), []).append(a)
    ser_by_sta = {}
    for s in out["series"]:
        ser_by_sta.setdefault(s["station"], []).append(s)

    for (tag, sta), arcs in sorted(groups.items()):
        n_ep = sum(a["n_ep"] for a in arcs)
        w = np.array([a["n_ep"] for a in arcs], dtype=float)

        def wmean(key):
            v = np.array([a.get(key, np.nan) for a in arcs], dtype=float)
            f = np.isfinite(v)
            return float(np.sum(v[f] * w[f]) / np.sum(w[f])) if f.any() else float("nan")

        def wrms(key):
            v = np.array([a.get(key, np.nan) for a in arcs], dtype=float)
            f = np.isfinite(v)
            return float(np.sqrt(np.sum(w[f] * v[f] ** 2) / np.sum(w[f]))) if f.any() else float("nan")

        pos = by_name[sta]["position_ecef"] if sta in by_name else [1.0, 0.0, 0.0]
        row = {
            "tag": tag,
            "class": ("suspect" if sta in SUSPECTS else ("control" if sta in CONTROLS else "sim")),
            "station": sta,
            "l5_type": arcs[0]["l5_type"], "l1_type": arcs[0]["l1_type"],
            "l2_type": arcs[0]["l2_type"],
            "n_arc": len(arcs), "n_ep": n_ep,
            "lon_deg": round(lon_deg(pos), 2),
            "n_l5_slip": sum(a["n_l5"] for a in arcs),
            "n_l5_slip_lli": sum(a["n_l5_lli"] for a in arcs),
            "n_l2_slip": sum(a["n_l2"] for a in arcs),
            "n_l1_slip": sum(a["n_l1"] for a in arcs),
            "n_noninteger": sum(a["n_non"] for a in arcs),
            "jump_rate_k": round(1000.0 * sum(a["n_jump"] for a in arcs) / max(n_ep, 1), 2),
            "l5_slip_rate_k": round(1000.0 * sum(a["n_l5"] for a in arcs) / max(n_ep, 1), 2),
            "non_rate_k": round(1000.0 * sum(a["n_non"] for a in arcs) / max(n_ep, 1), 2),
            "noise_rms_m": wrms("noise_rms_m"),
            "short_rms_m": wrms("short_rms_m"),
            "drift_range_m": wmean("drift_range_m"),
            "drift_range_max_m": float(np.nanmax([a["drift_range_m"] for a in arcs])),
            "excess_rms_m": wrms("excess_rms_m"),
            "code_mean_m": wmean("code_mean_m"),
            "code_rms_m": wrms("code_rms_m"),
        }
        rel = [a["code_mean_m"] - sat_code_ref[(tag, a["sat"])] for a in arcs
               if (tag, a["sat"]) in sat_code_ref and np.isfinite(a["code_mean_m"])]
        row["code_mean_rel_m"] = float(np.median(rel)) if rel else float("nan")
        row["code_spread_rel_m"] = float(np.std(rel)) if len(rel) > 1 else float("nan")
        row.update(out["cover"].get(sta, {}))
        # elevation-binned excess
        if elev:
            bins = {"15_30": [], "30_60": [], "60_90": []}
            for s in ser_by_sta.get(sta, []):
                a = out["arcs"][s["arc_id"]]
                if not np.isfinite(a.get("excess_rms_m", np.nan)):
                    continue
                ref = sim_common if s["tag"] == "sim" else common
                cm = ref.get(s["sat"])
                if cm is None:
                    continue
                sm, c = s["sm"], cm[s["idx"]]
                fin = np.isfinite(sm) & np.isfinite(c)
                e = sm[fin] - c[fin]
                e = e - np.mean(e)
                for ep_i, ev in zip(s["idx"][fin], e):
                    el = elev.get((sta, s["sat"], int(ep_i)))
                    if el is None:
                        continue
                    b = "15_30" if el < 30 else ("30_60" if el < 60 else "60_90")
                    bins[b].append(ev)
            for b, vals in bins.items():
                row["excess_rms_el" + b] = (float(np.sqrt(np.mean(np.array(vals) ** 2)))
                                            if vals else float("nan"))
        per_station_rows.append(row)

    ps_cols = list(per_station_rows[0].keys())
    with open(os.path.join(args.out_dir, "per_station.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, ps_cols, extrasaction="ignore")
        w.writeheader()
        for r in per_station_rows:
            w.writerow(r)

    # ---- hourly table for the suspects
    hourly_path = os.path.join(args.out_dir, "per_hour.csv")
    with open(hourly_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["station", "hour_ut", "hour_lst", "n_ep", "excess_rms_m", "n_jump", "n_l5"])
        for sta in SUSPECTS + CONTROLS:
            pos = by_name[sta]["position_ecef"]
            lst0 = lon_deg(pos) / 15.0
            buckets = {}
            for s in ser_by_sta.get(sta, []):
                a = out["arcs"][s["arc_id"]]
                for h, v in a.get("excess_by_hour", []):
                    buckets.setdefault(h, []).append(v)
            jh = {}
            for j in out["jumps"]:
                if j["station"] == sta:
                    h = int(j["ep"] * EPOCH_S // 3600)
                    jh.setdefault(h, [0, 0])
                    jh[h][0] += 1
                    if j["cls"] == "l5_int":
                        jh[h][1] += 1
            for h in range(int(args.hours)):
                vals = np.array(buckets.get(h, []))
                w.writerow([sta, h, round((h + lst0) % 24, 1), len(vals),
                            "%.4f" % np.sqrt(np.mean(vals ** 2)) if len(vals) else "",
                            jh.get(h, [0, 0])[0], jh.get(h, [0, 0])[1]])

    print("\nwrote %s" % args.out_dir)
    print("coefficients: b1=%.10f b2=%.10f b5=%.10f" % (B1, B2, B5))
    print("steps (L5-equivalent m/cycle): L5=%.6f L2=%.6f L1=%.6f" % (STEP_L5, STEP_L2, STEP_L1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
