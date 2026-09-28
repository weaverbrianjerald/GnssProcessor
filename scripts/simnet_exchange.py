#!/usr/bin/env python3
"""Synthetic W6+SH arm: the clock, receiver-DCB and ionosphere exchange signature.

docs/SPEC_synthetic_minimum_network.md §11 item 4.

The field campaign attributed the ~1.3 m detrended wander of the emitted wire to the
estimator rather than the input products (§9.9/§9.9.1: the a-priori product frame is
exonerated, and the wander is not receiver-code-driven), but the field data cannot show
the exchange directly because every channel involved there is unknown. On this fixture
all three channels are known, so the wander can be decomposed instead of inferred.

Under the W6 code-bias datum the satellite side is pinned twice: the iono-free
combination over the pinned pair goes into the satellite clock, and each channel is
forced to zero mean over the registered set. The emitted satellite code biases have no
free fleet-common mode (their per-channel mean is 0 by construction), so a
"fleet-mean satellite DCB drift" channel would measure the datum, not the estimator.
The directions that can wander are:

  1. clk_common(t)   -- fleet-median C-record error.  On a quiet fixture with no
                        injected satellite-clock error the C record is the error, and
                        its fleet-common part is (up to a constant) minus the realized
                        datum receiver's clock error (§9.8: the common level follows
                        the datum station).                                      [m]
  2. rxdcb_common(t) -- fleet-mean receiver-DCB error.  Table-8 (contract §B.4) says
                        the fleet-mean receiver GF code bias is absorbed into the
                        ionosphere, so this is the gauge-bearing partner of a00 and
                        the channel the standing record already identifies (the
                        rx-CB carry's rolling common-mode memory is what tracks the
                        level).                                                 [m]
  3. a00_err(t)      -- estimated global mean VTEC minus the injected field's mean at
                        the same epoch.                                       [TECU]

  4. cb_rms(t)       -- rms of the per-(sat,signal) code-bias residual after the §4.4
                        transform.  Reported as a co-moving diagnostic, not a gauge:
                        it is what the pinned datum leaves free.               [m]

The pre-registered exchange coefficient is derived from the model's own coefficients.
The receiver DCB state enters the two reference-band code rows with mu1/dmu and
mu2/dmu (`StateInitializer::resolveReceiverBias`, mu1 = 1, mu2 = gamma = 1.6469,
dmu = mu1 - mu2 = -0.6469), so on the geometry-free difference P1 - P2 its signature
is exactly +1 * x_DCB.  The slant ionosphere's GF signature is (1 - gamma) * iota =
-0.6469 * iota.  A GF residual that stays satisfied while the DCB carries an error
Delta therefore needs

    Delta - 0.6469 * Delta_iota = 0   =>   Delta_iota = +1.546 * Delta   [m of L1]

and 1 m of L1 delay is 1/0.1624 = 6.16 TECU of slant, so at a fleet-typical
obliquity of ~1.2 the vertical coefficient is

    +1.546 / (0.1624 * 1.2) = +7.93 TECU per metre of rx-DCB common error.

The sign is positive: a positive receiver-DCB error is absorbed by a positive
ionosphere, because both enter the GF combination with the same effective sign once
dmu's own negative sign is carried through (this derivation is the current basis for
the sign; see also the IF-lump note in scripts/simnet_grade.py for the same
convention elsewhere in the project).  Measured slope and correlation are reported
against that number: a match is the exchange, a mismatch means the wander is entering
somewhere else and the next arm should look there.

Method notes:
  * join on TOW, never on row index: a withheld epoch simply has no E block;
  * all statistics use demeaned, settled series (a constant offset is not wander, and
    the level channel's own convergence budget is ~2 h);
  * correlations are reported on the series and on first differences.  A slow common
    trend inflates the level correlation of any two rising series; the
    first-difference correlation is what shows whether they move together epoch to
    epoch;
  * the truth a00 is the network's coverage-weighted mean vertical TEC (mean over
    the sampled stations' lines of sight of stec_total/obliquity), not a spherical
    harmonic mean; it is the reference for what this network can see, not a global
    mean.

Usage:
  python scripts/simnet_exchange.py <fixture_dir> [--products P] [--run RUNDIR]
         [--settle-frac 0.5] [--truth-stations 60] [--csv out.csv]
"""
import argparse
import csv
import json
import math
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

GPS_L1, GPS_L2 = 1575.42e6, 1227.60e6
GAMMA = (GPS_L1 / GPS_L2) ** 2                 # 1.6469
TECU_TO_L1_M = 0.1624                          # metres of L1 delay per TECU
NOMINAL_OBLIQUITY = 1.2                        # fleet-typical slant/vertical ratio
# TECU of vertical TEC per metre of receiver GF code-bias error (see the header).
EXCHANGE_TECU_PER_M = +1.0 / ((GAMMA - 1.0) * TECU_TO_L1_M * NOMINAL_OBLIQUITY)


# --------------------------------------------------------------------------- #

def median(xs):
    s = sorted(xs)
    n = len(s)
    if not n:
        return None
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def stats(xs):
    n = len(xs)
    if n < 2:
        return {"n": n, "mean": mean(xs) or 0.0, "std": 0.0, "range": 0.0}
    m = sum(xs) / n
    return {"n": n, "mean": m,
            "std": math.sqrt(sum((x - m) ** 2 for x in xs) / n),
            "range": max(xs) - min(xs)}


def pearson(xs, ys):
    n = min(len(xs), len(ys))
    if n < 3:
        return float("nan")
    xs, ys = xs[:n], ys[:n]
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in ys)
    if sxx <= 0 or syy <= 0:
        return float("nan")
    return sxy / math.sqrt(sxx * syy)


def ols_slope(xs, ys):
    """Slope of ys on xs (least squares through the demeaned data)."""
    n = min(len(xs), len(ys))
    if n < 3:
        return float("nan")
    xs, ys = xs[:n], ys[:n]
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((a - mx) ** 2 for a in xs)
    if sxx <= 0:
        return float("nan")
    return sum((a - mx) * (b - my) for a, b in zip(xs, ys)) / sxx


def load_csv(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return list(csv.DictReader(fh))


# --------------------------------------------------------------------------- #
# Channel extraction
# --------------------------------------------------------------------------- #

def parse_products(path, osb_truth, pin):
    """-> {tow: {"clk": median C, "cb_rms": rms transformed CB residual}}.

    The CB residual applies §4.4's transform (per-satellite IF lump over the pinned
    pair, then zero mean per channel over the epoch's registered set) to the truth
    before differencing, the same way scripts/simnet_grade.py does.  With no injected
    OSB (the INT posture) the CB part is absent.
    """
    out, tow, clk, cb = {}, None, [], {}
    j1, j2 = pin if pin else (None, None)
    a1 = a2 = None
    if j1 and j2:
        f1 = GPS_L1 if j1[1] == "1" else (GPS_L2 if j1[1] == "2" else None)
        f2 = GPS_L1 if j2[1] == "1" else (GPS_L2 if j2[1] == "2" else None)
        if f1 and f2 and f1 != f2:
            a1 = f1 ** 2 / (f1 ** 2 - f2 ** 2)
            a2 = -f2 ** 2 / (f1 ** 2 - f2 ** 2)

    def commit():
        if tow is None:
            return
        rec = {}
        if clk:
            rec["clk"] = median(clk)
        if cb and a1 is not None:
            resid = []
            by_sig = {}
            for (s, j), v in cb.items():
                b1, b2 = osb_truth.get((s, j1)), osb_truth.get((s, j2))
                t = osb_truth.get((s, j))
                if b1 is None or b2 is None or t is None:
                    continue
                by_sig.setdefault(j, []).append((v, t - (a1 * b1 + a2 * b2)))
            for j, items in by_sig.items():
                tm = sum(t for _, t in items) / len(items)
                resid.extend(v - (t - tm) for v, t in items)
            if resid:
                rec["cb_rms"] = math.sqrt(sum(r * r for r in resid) / len(resid))
        if rec:
            out[tow] = rec

    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("E,"):
                commit()
                p = line.strip().split(",")
                try:
                    tow = round(float(p[2]), 3)
                except (IndexError, ValueError):
                    tow = None
                clk, cb = [], {}
            elif tow is None:
                continue
            elif line.startswith("C,"):
                p = line.strip().split(",")
                if len(p) >= 3 and "=" not in p[2]:
                    try:
                        v = float(p[2])
                    except ValueError:
                        continue
                    if math.isfinite(v):
                        clk.append(v)
            elif line.startswith("B,"):
                p = line.strip().split(",")
                if len(p) >= 4 and "=" not in p[3]:
                    try:
                        cb[(p[1], p[2])] = float(p[3])
                    except ValueError:
                        pass
    commit()
    return out


def parse_cbpin(path):
    """Read cbpin=<const>:<s1>-<s2> off the S line.  Never assume the pair."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line.startswith("S,"):
                continue
            for f in line.strip().split(",")[11:]:
                if f.startswith("cbpin="):
                    _, _, v = f.partition("=")
                    _, _, pair = v.partition(":")
                    a, _, b = pair.partition("-")
                    if a and b:
                        return (a, b)
            return None
    return None


def rxdcb_series(run_dir, truth_gf):
    """-> {tow: fleet-mean (estimated rx DCB - injected GF truth)} in metres."""
    rows = load_csv(os.path.join(run_dir, "logs", "network_rx_states.csv"))
    if not rows:
        return {}
    acc = {}
    for r in rows:
        if r.get("kind") != "CB":
            continue
        st = r["station"]
        if st not in truth_gf:
            continue
        try:
            t = round(float(r["tow"]), 3)
            v = float(r["value_m"])
        except ValueError:
            continue
        acc.setdefault(t, []).append(v - truth_gf[st])
    return {t: mean(v) for t, v in acc.items() if v}


def a00_series(run_dir):
    """-> ({tow: a00_tecu}, {tow: a00_std_tecu}) from network_vtec_sh_health.csv."""
    rows = load_csv(os.path.join(run_dir, "logs", "network_vtec_sh_health.csv"))
    if not rows:
        return {}, {}
    val, sig = {}, {}
    for r in rows:
        try:
            t = round(float(r["tow"]), 3)
            val[t] = float(r["a00_tecu"])
            sig[t] = float(r["a00_std_tecu"])
        except (KeyError, ValueError):
            continue
    return val, sig


def truth_vtec_series(manifest, n_sample):
    """-> {tow: coverage-weighted mean vertical TECU} from truth_atmo.csv.

    Sampled over up to n_sample solve stations by stride (a Fibonacci roster's index
    is monotone in latitude, so a prefix would be a latitude band, not a network).
    """
    solve = [s for s in manifest["stations"] if not s.get("consumer")]
    if not solve:
        return {}, 0
    step = max(1, len(solve) // max(1, n_sample))
    picked = solve[::step][:n_sample]
    acc = {}
    for s in picked:
        p = os.path.join(s["truth_dir"], "truth_atmo.csv")
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8", errors="replace") as fh:
            for r in csv.DictReader(fh):
                try:
                    t = round(float(r["tow_s"]), 3)
                    ob = float(r["obliquity"])
                    st = float(r["stec_total_tecu"])
                except (KeyError, ValueError):
                    continue
                if ob > 0:
                    acc.setdefault(t, []).append(st / ob)
    return {t: mean(v) for t, v in acc.items() if v}, len(picked)


# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir")
    ap.add_argument("--products", default=None)
    ap.add_argument("--run", default=None)
    ap.add_argument("--settle-frac", type=float, default=0.5)
    ap.add_argument("--truth-stations", type=int, default=60,
                    help="stations sampled by stride for the truth VTEC mean (default 60)")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    fx = args.fixture_dir
    run_dir = args.run or os.path.join(fx, "run")
    prod = args.products or os.path.join(fx, "products.txt")
    if not os.path.exists(prod):
        print(f"FAIL: no product file at {prod}")
        return 2
    with open(os.path.join(fx, "network_fixture.json"), encoding="utf-8") as fh:
        man = json.load(fh)

    # Truth: injected satellite OSB (satellite-side, time-constant) + receiver GF.
    osb_truth, truth_gf = {}, {}
    for s in man["stations"]:
        for r in (load_csv(os.path.join(s["truth_dir"], "truth_bias.csv")) or []):
            osb_truth[(r["satellite"], r["signal"])] = float(r["osb_m"])
        code = {r["code"]: float(r["value_m"])
                for r in (load_csv(os.path.join(s["truth_dir"], "truth_rx_bias.csv")) or [])
                if r["kind"] == "code"}
        if "C1C" in code and "C2W" in code:
            truth_gf[s["name"]] = code["C1C"] - code["C2W"]

    pin = parse_cbpin(prod)
    prods = parse_products(prod, osb_truth, pin)
    rx = rxdcb_series(run_dir, truth_gf)
    a00, a00_sd = a00_series(run_dir)
    tv, n_picked = truth_vtec_series(man, args.truth_stations)

    tows = sorted(prods)
    if not tows:
        print("FAIL: no emitted epochs in the product file")
        return 2
    cut = tows[0] + args.settle_frac * (tows[-1] - tows[0])
    keep = [t for t in tows if t >= cut]

    ch = {}
    ch["clk_common"] = {t: prods[t]["clk"] for t in keep if "clk" in prods[t]}
    if any("cb_rms" in prods[t] for t in keep):
        ch["cb_rms"] = {t: prods[t]["cb_rms"] for t in keep if "cb_rms" in prods[t]}
    if rx:
        ch["rxdcb_common"] = {t: rx[t] for t in keep if t in rx}
    if a00 and tv:
        ch["a00_err"] = {t: a00[t] - tv[t] for t in keep if t in a00 and t in tv}

    print("=" * 78)
    print("W6+SH EXCHANGE SIGNATURE — clock <-> receiver DCB <-> ionosphere")
    print("=" * 78)
    print(f"  fixture   : {fx}")
    print(f"  emitted   : {len(tows)} epochs, grading the settled {len(keep)} "
          f"(discarded first {args.settle_frac:.0%} as ignition)")
    print(f"  IF pin    : {pin if pin else '(no cbpin on the S line — CB channel off)'}")
    print(f"  truth VTEC: coverage-weighted mean over {n_picked} stride-sampled stations"
          if tv else "  truth VTEC: UNAVAILABLE (no truth_atmo.csv) — a00 channel off")
    if not a00:
        print("  a00       : UNAVAILABLE — arm estimation.global_vtec_sh.trace")
    if not rx:
        print("  rx DCB    : UNAVAILABLE — arm network.trace_rx_states")

    units = {"clk_common": "m", "cb_rms": "m", "rxdcb_common": "m", "a00_err": "TECU"}
    print("\n-- channels (settled, demeaned statistics) " + "-" * 34)
    demeaned = {}
    for name, series in ch.items():
        xs = [series[t] for t in sorted(series)]
        s = stats(xs)
        u = units[name]
        print(f"  {name:<14} n={s['n']:<5d} mean={s['mean']:+10.4f} {u:<5s} "
              f"std={s['std']:.4f} {u:<5s} range={s['range']:.4f} {u}")
        m = s["mean"]
        demeaned[name] = {t: series[t] - m for t in series}

    names = [n for n in ("clk_common", "rxdcb_common", "a00_err", "cb_rms") if n in demeaned]
    if len(names) >= 2:
        print("\n-- correlation matrix (upper: series; lower: first differences) " + "-" * 13)
        print("                 " + "".join(f"{n:>15s}" for n in names))
        for i, a in enumerate(names):
            row = f"  {a:<15s}"
            for j, b in enumerate(names):
                if i == j:
                    row += f"{'1':>15s}"
                    continue
                joint = sorted(set(demeaned[a]) & set(demeaned[b]))
                xa = [demeaned[a][t] for t in joint]
                xb = [demeaned[b][t] for t in joint]
                if i < j:
                    r = pearson(xa, xb)
                else:
                    r = pearson([xa[k + 1] - xa[k] for k in range(len(xa) - 1)],
                                [xb[k + 1] - xb[k] for k in range(len(xb) - 1)])
                row += f"{r:>+15.3f}"
            print(row)
        print(f"  (joint settled epochs per pair vary with channel availability)")

    if "rxdcb_common" in demeaned and "a00_err" in demeaned:
        joint = sorted(set(demeaned["rxdcb_common"]) & set(demeaned["a00_err"]))
        xs = [demeaned["rxdcb_common"][t] for t in joint]
        ys = [demeaned["a00_err"][t] for t in joint]
        sl = ols_slope(xs, ys)
        r = pearson(xs, ys)
        fsl = ols_slope([xs[k + 1] - xs[k] for k in range(len(xs) - 1)],
                        [ys[k + 1] - ys[k] for k in range(len(ys) - 1)])
        print("\n-- the pre-registered exchange coefficient " + "-" * 34)
        print(f"  predicted  {EXCHANGE_TECU_PER_M:+.2f} TECU per metre of rx-DCB common error")
        print(f"             (1/((gamma-1) * {TECU_TO_L1_M} m/TECU * obliquity "
              f"{NOMINAL_OBLIQUITY}), gamma = {GAMMA:.4f}; POSITIVE — derived from the")
        print( "              model's own DCB coefficients mu1/dmu, mu2/dmu, not asserted)")
        print(f"  measured   {sl:+.2f} TECU/m  (corr {r:+.3f}, {len(joint)} epochs)")
        print(f"  first-diff {fsl:+.2f} TECU/m")
        if math.isfinite(sl) and EXCHANGE_TECU_PER_M:
            ratio = sl / EXCHANGE_TECU_PER_M
            note = ("MATCHES the ideal-absorption prediction" if 0.7 <= ratio <= 1.4 else
                    "WRONG SIGN — the two channels are not exchanging as modeled"
                    if ratio < 0 else
                    f"same sign, {ratio:.2f}x the IDEAL coefficient — the exchange is "
                    "real but partial (the iono is also held by its own data and prior)")
            print(f"  ratio measured/predicted = {ratio:+.2f}  => {note}")

    if args.csv:
        allt = sorted(set().union(*[set(v) for v in ch.values()])) if ch else []
        with open(args.csv, "w", encoding="utf-8", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["tow"] + names + ["a00_std_tecu"])
            for t in allt:
                w.writerow([f"{t:.3f}"]
                           + [f"{ch[n][t]:.6f}" if t in ch[n] else "" for n in names]
                           + [f"{a00_sd[t]:.4f}" if t in a00_sd else ""])
        print(f"\nwrote {args.csv}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"channels": {n: stats([v for _, v in sorted(ch[n].items())])
                                    for n in names},
                       "pin": pin, "n_settled": len(keep),
                       "exchange_predicted_tecu_per_m": EXCHANGE_TECU_PER_M}, fh, indent=2)
        print(f"wrote {args.json}")
    print("\n" + "=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
