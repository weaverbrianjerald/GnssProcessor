#!/usr/bin/env python3
"""SYNTHETIC MINIMUM NETWORK — the truth grader (docs/SPEC_synthetic_minimum_network.md §4).

The first ABSOLUTE grade the network producer has ever had.  Every other grade in
this repo is internal (self-consistency, datum invariance, cross-instrument
agreement) and is satisfiable by a confidently wrong estimator; this one compares
the emitted wire against injected truth.

THE METHOD, AND WHY IT IS NOT THE OBVIOUS ONE (SPEC §4.1)
---------------------------------------------------------
The obvious move -- difference the ESTIMATE into a between-satellite contrast so
the datum cancels -- silently breaks the variance factor.  For a contrast,

    sigma_e^2  !=  sigma_s^2 + sigma_s'^2

because the two estimates share the datum and are strongly correlated.  The
marginal sigma the producer publishes therefore OVERSTATES the contrast sigma and
biases vf DOWNWARD -- it makes a mis-calibrated estimator look calibrated.  Since
the whole point of gate G-A is to ask "is the field code-bias vf ~ 10 the
estimator or the data?", a downward-biased vf answers it backwards.

So this grader transforms TRUTH INTO THE PRODUCER'S REALIZED BASIS instead:

    e_s  = estimate_s - S[truth]_s
    vf   = mean( e_s^2 / sigma_s^2 )      sigma_s = marginal, datum-conditioned

Both sides then live in the same basis and sigma_s is the marginal variance of
exactly that quantity.  The realized datum (pivot, cbpin pair, reference receiver)
is READ off the S line -- never assumed.  The datum-free contrast form is retained
as a CROSS-CHECK: it must agree, and a disagreement means the datum was misread.

TRAPS DEFENDED AGAINST (SPEC §8) -- each has produced a plausible wrong answer:
  * join on tow, never on row index (truth files have different row counts:
    some are written before the epoch's <5-satellite `continue`, some after)
  * stop the positional parse at the first field containing '=' (trailing product
    fields are shape-dispatched, not positional, and config-dependent)
  * blank means "did not run", never 0
  * the datum pivot has no U record by construction, so it is absent from the vf
    population -- that is not a bug and must not be counted as missing coverage
  * truth_ambiguities.csv holds the PRE-SLIP integer; the true value at epoch k is
    N_int + sum of truth_slips deltas up to k
  * truth_atmo stec_* columns are ALREADY SLANT -- multiplying by the published
    obliquity gives a wrong value that still cancels in the iono-free combination
  * the S line's apriori_id can name the WRONG DAY: when the product day-edge guard
    loads the neighbouring day, getProductPath returns one of several files.  Never
    key truth on it; it is reported, not trusted.

Usage:
    python scripts/simnet_grade.py <fixture_dir> [--run <run_dir>] [--json <out>]

  <fixture_dir>  the synth-network output (holds network_fixture.json and the
                 per-station truth directories)
  --run          the producer run directory (default <fixture_dir>/run); its
                 logs/ holds network_zwd.csv and closed_loop_trace.csv
"""
import argparse
import csv
import json
import math
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

C_LIGHT = 299792458.0
GPS_L1 = 1575.42e6
GPS_L2 = 1227.60e6
# Iono-free code combination over the L1/L2 pair: a1 + a2 == 1 exactly.
IF_A1 = GPS_L1 ** 2 / (GPS_L1 ** 2 - GPS_L2 ** 2)
IF_A2 = -GPS_L2 ** 2 / (GPS_L1 ** 2 - GPS_L2 ** 2)
WAVELENGTH = {"L1C": C_LIGHT / GPS_L1, "L2W": C_LIGHT / GPS_L2}


# --------------------------------------------------------------------------- #
# Product parsing
# --------------------------------------------------------------------------- #

def parse_products(path):
    """Return (s_line dict, epochs list).

    Each epoch: {tow, week, clocks {sat: c0_m}, ura {sat: sigma_m},
                 phase {(sat,signal): bias_m}, pb_sigma {(sat,signal): sigma_m},
                 heartbeat {...}}.

    Trailing product fields are SHAPE-dispatched, not positional (a bare int is a
    discontinuity counter; 'sigma=' / 'sigc=' / 'iode=' may appear in any order),
    and which of them exist depends on the producing toggles.  A positional parse
    calibrated on one arm reads the disc counter as a value on another, so every
    positional read below stops at the first field containing '='.
    """
    s_line, epochs, cur = None, [], None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split(",")
            rec = p[0]
            if rec == "S":
                s_line = parse_s_line(p)
            elif rec == "E":
                cur = {"week": int(p[1]), "tow": float(p[2]),
                       "datum_changed": p[6] if len(p) > 6 else "0",
                       "clocks": {}, "ura": {}, "phase": {}, "pb_sigma": {},
                       "code_bias": {}, "retro": []}
                epochs.append(cur)
            elif cur is None:
                continue                       # records before the first E
            elif rec == "C":
                cur["clocks"][p[1]] = float(p[2])
            elif rec == "U":
                cur["ura"][p[1]] = float(p[2])
            elif rec == "B":
                cur["code_bias"][(p[1], p[2])] = float(p[3])
            elif rec == "R":
                cur["retro"].extend(x for x in p[1:] if x)
            elif rec == "P":
                key = (p[1], p[2])
                cur["phase"][key] = float(p[3])
                for f in p[4:]:
                    if f.startswith("sigma="):
                        cur["pb_sigma"][key] = float(f[6:])
    return s_line, epochs


def parse_s_line(p):
    """Decode the S-basis contract line.  Fields 11+ are shape-dispatched."""
    out = {"ref_rx": p[1], "apriori_id": p[10] if len(p) > 10 else "",
           "pivots": {}, "cbpin": {}, "extra": {}}
    for f in p[11:]:
        if "=" in f:
            k, _, v = f.partition("=")
            if k == "cbpin":
                # cbpin=<const>:<s1>-<s2>
                cst, _, pair = v.partition(":")
                a, _, b = pair.partition("-")
                out["cbpin"][cst] = (a, b)
            else:
                out["extra"][k] = v
        elif ":" in f:
            cst, _, sat = f.partition(":")
            out["pivots"][cst] = sat
    return out


# --------------------------------------------------------------------------- #
# Truth loading
# --------------------------------------------------------------------------- #

def load_csv(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return list(csv.DictReader(fh))


def load_truth(fixture_dir):
    man_path = os.path.join(fixture_dir, "network_fixture.json")
    with open(man_path, encoding="utf-8") as fh:
        man = json.load(fh)

    truth = {"manifest": man, "stations": {}}
    for st in man["stations"]:
        d = st["truth_dir"]
        rec = {
            "name": st["name"],
            "consumer": st.get("consumer", False),
            "ecef": st["position_ecef"],
            "tropo": load_csv(os.path.join(d, "truth_tropo.csv")),
            "atmo": load_csv(os.path.join(d, "truth_atmo.csv")),
            "amb": load_csv(os.path.join(d, "truth_ambiguities.csv")),
            "slips": load_csv(os.path.join(d, "truth_slips.csv")),
            "rx_bias": load_csv(os.path.join(d, "truth_rx_bias.csv")),
            "satclock": load_csv(os.path.join(d, "truth_satclock.csv")),
        }
        truth["stations"][st["name"]] = rec
    return truth


def satclock_truth_by_tow(truth):
    """Injected satellite-clock error, metres of clock offset, keyed (tow, sat).

    Satellite-side and therefore identical at every station BY CONSTRUCTION (the
    determinism contract in SatClockError.hpp).  We read it from one station and
    ASSERT the others agree -- if they ever do not, a satellite-side term has been
    made station-dependent, which grades as an estimator defect.
    """
    series, disagreements = {}, 0
    for name, st in truth["stations"].items():
        rows = st["satclock"]
        if not rows:
            continue
        for r in rows:
            key = (round(float(r["tow_s"]), 3), r["satellite"])
            v = float(r["clk_err_m"])
            if key in series and abs(series[key] - v) > 1e-9:
                disagreements += 1
            series[key] = v
    return series, disagreements


# --------------------------------------------------------------------------- #
# Statistics
# --------------------------------------------------------------------------- #

def stats(vals):
    if not vals:
        return None
    n = len(vals)
    m = sum(vals) / n
    var = sum((v - m) ** 2 for v in vals) / n if n > 1 else 0.0
    a = sorted(abs(v) for v in vals)
    rms = math.sqrt(sum(v * v for v in vals) / n)
    return {"n": n, "mean": m, "std": math.sqrt(var), "rms": rms,
            "p50_abs": a[n // 2], "max_abs": a[-1]}


def fmt(s, unit=""):
    if s is None:
        return "        (no population)"
    return (f"n={s['n']:<6d} mean={s['mean']:+.4f}{unit}  rms={s['rms']:.4f}{unit}  "
            f"p50|.|={s['p50_abs']:.4f}{unit}  max|.|={s['max_abs']:.4f}{unit}")


# --------------------------------------------------------------------------- #
# Channel graders
# --------------------------------------------------------------------------- #

def grade_clock(epochs, s_line, sc_truth, settle_frac):
    """C record: transform truth into the producer's basis, then compare.

    Wire meaning (contract section B.1):
        dt~^s = [dt^s + d_IF^s] - [dt_pivot] - A_c
    With the sim injecting the precise CLK product -- the SAME object the producer
    names as its a-priori -- the truth correction is exactly the INJECTED error,
    and the basis transform is "subtract the realized pivot's injected error".
    The common-mode accumulator A_c drops out of that difference too.

    With no injected error the truth is identically zero for every satellite: a
    valid null test, and the one this fixture runs at G-A.
    """
    pivots = (s_line or {}).get("pivots", {})
    n_ep = len(epochs)
    start = int(n_ep * settle_frac)
    res, res_vf, contrast_res = [], [], []
    n_no_sigma, n_pivot_skipped = 0, 0

    for ep in epochs[start:]:
        tow = round(ep["tow"], 3)
        piv = pivots.get("G")
        # Truth for the pivot at this epoch (0 when no clock error is injected).
        piv_truth = sc_truth.get((tow, piv), 0.0) if piv else 0.0

        for sat, c0 in ep["clocks"].items():
            if piv and sat == piv:
                n_pivot_skipped += 1
                continue                      # pinned to 0; nothing to grade
            t = sc_truth.get((tow, sat), 0.0) - piv_truth   # S[truth]
            e = c0 - t
            res.append(e)
            sig = ep["ura"].get(sat)
            if sig is None or not (sig > 0):
                n_no_sigma += 1               # blank means "not emitted", never 0
            else:
                res_vf.append((e / sig) ** 2)

        # Datum-free cross-check: every between-satellite contrast must reproduce
        # the same residual without using the pivot at all.  Disagreement here is
        # the signature of a MISREAD datum, which is the failure this catches.
        sats = sorted(ep["clocks"])
        for i in range(len(sats) - 1):
            a, b = sats[i], sats[i + 1]
            ta = sc_truth.get((tow, a), 0.0)
            tb = sc_truth.get((tow, b), 0.0)
            contrast_res.append((ep["clocks"][a] - ep["clocks"][b]) - (ta - tb))

    vf = sum(res_vf) / len(res_vf) if res_vf else None
    return {"residual": stats(res), "contrast": stats(contrast_res), "vf": vf,
            "n_vf": len(res_vf), "n_no_sigma": n_no_sigma,
            "pivot": pivots.get("G"), "n_pivot_skipped": n_pivot_skipped}


def grade_phase(epochs, settle_frac):
    """P record: the between-satellite single difference must be integer x lambda.

    In the INT posture (no satellite phase bias injected) the estimable content of
    a P-record SD is exactly -lambda * (N_pivot_station difference), so its residual
    modulo one cycle is the sharp test.  Grading the metre value directly would be
    the invariant-#5 mistake: the datum receiver's ambiguity rides inside the bias.
    """
    n_ep = len(epochs)
    fracs = {}
    for ep in epochs[int(n_ep * settle_frac):]:
        by_sig = {}
        for (sat, sig), v in ep["phase"].items():
            by_sig.setdefault(sig, []).append((sat, v))
        for sig, items in by_sig.items():
            lam = WAVELENGTH.get(sig)
            if not lam or len(items) < 2:
                continue
            items.sort()
            ref = items[0][1]
            for _, v in items[1:]:
                cyc = (v - ref) / lam
                f = cyc - round(cyc)
                fracs.setdefault(sig, []).append(f)
    return {sig: stats(v) for sig, v in fracs.items()}


def grade_zwd(run_dir, truth, settle_frac):
    """ZWD: not gauge-bearing, so grade it ABSOLUTELY, with the one clean vf.

    network_zwd.csv is the only per-station state channel emitted with a
    datum-conditioned formal sigma (zwd_m = x_lin + x_r, std_m = sqrt(conditional
    local variance)), which makes it the second opinion on the code-bias vf that
    the C/P/B channels cannot supply as cleanly.
    """
    path = os.path.join(run_dir, "logs", "network_zwd.csv")
    rows = load_csv(path)
    if not rows:
        return None

    # Truth ZWD per (station, tow).  JOIN ON TOW -- the truth writers and the
    # RINEX epoch loop have different row counts by construction.
    tt = {}
    for name, st in truth["stations"].items():
        for r in (st["tropo"] or []):
            tt[(name, round(float(r["tow_s"]), 3))] = float(r["zwd_m"])

    n = len(rows)
    res, vfs, missing = [], [], 0
    for r in rows[int(n * settle_frac):]:
        key = (r["station"], round(float(r["tow"]), 3))
        if key not in tt:
            missing += 1
            continue
        e = float(r["zwd_m"]) - tt[key]
        res.append(e)
        sd = r.get("std_m", "").strip()
        if sd:                                 # blank = did not run, never 0
            s = float(sd)
            if s > 0:
                vfs.append((e / s) ** 2)
    return {"residual": stats(res), "vf": (sum(vfs) / len(vfs)) if vfs else None,
            "n_vf": len(vfs), "missing_truth": missing}


def grade_consumer(run_dir, settle_frac):
    """closed_loop_trace.csv already logs the ERROR vector against declared truth.

    For a synthetic station with zero antenna eccentricity the declared ECEF is
    exact truth, so this is the only fully datum-free ABSOLUTE producer output.
    It is AR-confounded, so it is an end-to-end verdict, not a channel diagnostic.
    """
    rows = load_csv(os.path.join(run_dir, "logs", "closed_loop_trace.csv"))
    if not rows:
        return None
    n = len(rows)
    e3 = [float(r["pos_err_3d_m"]) for r in rows[int(n * settle_frac):]
          if r.get("pos_err_3d_m", "").strip()]
    up = [float(r["up_m"]) for r in rows[int(n * settle_frac):]
          if r.get("up_m", "").strip()]
    ar = sum(1 for r in rows if r.get("ar_success", "0").strip() in ("1", "true"))
    return {"err3d": stats(e3), "up": stats(up), "n_epochs": n, "ar_success": ar}


def grade_iono_table8(truth):
    """The Table-8 identity's PRECONDITION check (SPEC section 4.5).

    The headline system-level unit test is

        mean_s( iono~_r^s - iono_r^s )  ==  dbar_r,GF - mean_s( d_GF^s )

    whose right-hand side is the FLEET-MEAN receiver GF code bias.  It is only a
    test at all if the injected receiver biases are (a) non-zero and (b) DIFFERENT
    across the roster -- otherwise the fleet mean is indistinguishable from its
    members and the estimator could realize any split between them and still grade
    as correct.  This reports whether the fixture actually satisfies that, because
    a degenerate fixture would otherwise pass silently.
    """
    per_station = {}
    for name, st in truth["stations"].items():
        rows = st["rx_bias"] or []
        code = {r["code"]: float(r["value_m"]) for r in rows if r["kind"] == "code"}
        if not code:
            continue
        # GF (geometry-free) receiver bias over the graded L1/L2 pair.
        c1, c2 = code.get("C1C"), code.get("C2W")
        if c1 is None or c2 is None:
            continue
        per_station[name] = {"gf": c1 - c2,
                             "if": IF_A1 * c1 + IF_A2 * c2}
    if not per_station:
        return {"armed": False}
    gfs = [v["gf"] for v in per_station.values()]
    return {"armed": True, "per_station": per_station,
            "fleet_mean_gf": sum(gfs) / len(gfs),
            "spread_gf": max(gfs) - min(gfs),
            "degenerate": (max(gfs) - min(gfs)) < 1e-9}


# --------------------------------------------------------------------------- #
# Report
# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir")
    ap.add_argument("--run", default=None, help="producer run dir (default <fixture>/run)")
    ap.add_argument("--products", default=None, help="products.txt (default <fixture>/products.txt)")
    ap.add_argument("--settle-frac", type=float, default=0.5,
                    help="discard this leading fraction of epochs as ignition "
                         "(default 0.5; the level channel needs ~2 h and a short "
                         "arc will otherwise grade the transient)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    fx = args.fixture_dir
    run_dir = args.run or os.path.join(fx, "run")
    prod = args.products or os.path.join(fx, "products.txt")

    if not os.path.exists(prod):
        print(f"FAIL: no product file at {prod}")
        return 2

    truth = load_truth(fx)
    s_line, epochs = parse_products(prod)
    sc_truth, sc_disagree = satclock_truth_by_tow(truth)

    print("=" * 78)
    print("SYNTHETIC MINIMUM NETWORK — truth grade")
    print("=" * 78)
    man = truth["manifest"]
    solve = [s["name"] for s in man["stations"] if not s.get("consumer")]
    cons = [s["name"] for s in man["stations"] if s.get("consumer")]
    print(f"  fixture      : {fx}")
    print(f"  stations     : {len(solve)} solve {solve} + consumer {cons or '(none)'}")
    print(f"  epochs       : {len(epochs)} emitted of {man['n_epochs']} synthesized "
          f"@ {man['interval_s']} s")
    print(f"  settle       : discarding first {args.settle_frac:.0%} as ignition")
    if s_line:
        print(f"  realized datum: pivot {s_line['pivots']}  ref_rx {s_line['ref_rx']}"
              f"  cbpin {s_line['cbpin'] or '(none — no CB channel)'}")
        print(f"  apriori_id   : {s_line['apriori_id']}")
        print( "                 (NOT trusted as a date key — the product day-edge guard")
        print( "                  loads the neighbouring day and this names only one file)")
    else:
        print("  realized datum: *** NO S LINE *** — arm network.serialize_sbasis_contract.")
        print("                  Without it the datum must be ASSUMED, which is exactly")
        print("                  the invariant-#5 mistake this grader exists to avoid.")

    print(f"\n  injected sat-clock error : "
          f"{'ARMED (' + str(len(sc_truth)) + ' rows)' if sc_truth else 'none — C-channel truth is identically 0 (null test)'}")
    if sc_disagree:
        print(f"  *** {sc_disagree} (tow,sat) satellite-clock truth DISAGREEMENTS across stations —")
        print(f"      a satellite-side term has been made station-dependent.")

    t8 = grade_iono_table8(truth)
    print("\n-- Table-8 precondition (receiver GF code bias) " + "-" * 30)
    if not t8["armed"]:
        print("  NOT ARMED: no receiver code biases injected.  The fleet-mean identity")
        print("  degenerates to 0 == 0 and proves nothing (SPEC §4.5).")
    elif t8["degenerate"]:
        print("  DEGENERATE: every station carries the SAME GF bias, so the fleet mean is")
        print("  indistinguishable from its members.  Vary them across the roster.")
    else:
        print(f"  armed, fleet-mean GF = {t8['fleet_mean_gf']:+.4f} m, "
              f"across-roster spread = {t8['spread_gf']:.4f} m")
        for n, v in sorted(t8["per_station"].items()):
            print(f"    {n}: GF {v['gf']:+.4f} m   IF {v['if']:+.4f} m")

    clk = grade_clock(epochs, s_line, sc_truth, args.settle_frac)
    print("\n-- C record (satellite clock), truth in the producer's basis " + "-" * 16)
    print(f"  pivot {clk['pivot']} excluded ({clk['n_pivot_skipped']} rows, pinned to 0 by the datum)")
    print(f"  residual  {fmt(clk['residual'], ' m')}")
    print(f"  contrast  {fmt(clk['contrast'], ' m')}   <- datum-free cross-check")
    if clk["vf"] is not None:
        verdict = ("CALIBRATED" if 0.5 <= clk["vf"] <= 2.0 else
                   "OPTIMISTIC (sigma too small)" if clk["vf"] > 2.0 else
                   "CONSERVATIVE (sigma too large)")
        print(f"  vf        {clk['vf']:.3f}  over {clk['n_vf']} rows   => {verdict}")
    else:
        print("  vf        (no URA emitted — arm network.emit_ura)")
    if clk["n_no_sigma"]:
        print(f"  {clk['n_no_sigma']} clock rows had no URA (blank = not emitted, never 0)")

    ph = grade_phase(epochs, args.settle_frac)
    print("\n-- P record (phase bias), between-satellite SD, fractional cycles " + "-" * 11)
    if not ph:
        print("  (no population)")
    for sig, s in sorted(ph.items()):
        print(f"  {sig}: {fmt(s, ' cyc')}")

    zwd = grade_zwd(run_dir, truth, args.settle_frac)
    print("\n-- ZWD (absolute; the cleanest available vf) " + "-" * 32)
    if zwd is None:
        print("  no network_zwd.csv — arm network.trace_zwd")
    else:
        print(f"  residual  {fmt(zwd['residual'], ' m')}")
        if zwd["vf"] is not None:
            print(f"  vf        {zwd['vf']:.3f}  over {zwd['n_vf']} rows")
        if zwd["missing_truth"]:
            print(f"  {zwd['missing_truth']} rows had no truth match (joined on tow)")

    con = grade_consumer(run_dir, args.settle_frac)
    print("\n-- Consumer position (datum-free, absolute, AR-confounded) " + "-" * 18)
    if con is None:
        print("  no closed_loop_trace.csv")
    else:
        print(f"  3D error  {fmt(con['err3d'], ' m')}")
        print(f"  up        {fmt(con['up'], ' m')}")
        print(f"  epochs {con['n_epochs']}, AR accepted {con['ar_success']}")

    print("\n" + "=" * 78)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"clock": clk, "phase": ph, "zwd": zwd, "consumer": con,
                       "table8": t8, "s_line": s_line,
                       "n_epochs": len(epochs)}, fh, indent=2, default=str)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
