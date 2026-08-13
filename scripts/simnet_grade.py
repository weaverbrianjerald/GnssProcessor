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
import re
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
GPS_L5 = 1176.45e6
# RINEX code -> carrier frequency, for the W6 IF-pin transform of §4.4.  Keyed on the
# CODE observable (the B record's own key namespace), not the phase sibling.
CODE_FREQ = {"C1C": GPS_L1, "C1W": GPS_L1, "C2W": GPS_L2, "C2L": GPS_L2,
             "C2S": GPS_L2, "C5Q": GPS_L5, "C5X": GPS_L5}


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
            "bias": load_csv(os.path.join(d, "truth_bias.csv")),
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

def if_lump_by_sat(osb_truth, pin):
    """Per-satellite iono-free lump of the INJECTED OSB over the pinned pair (m).

    §4.2's `d_IF^s` term.  On the W6 arm the producer does NOT apply the .BIA, so this
    lump is absorbed by the estimated satellite CLOCK and the C-record truth is
    therefore NOT identically zero even with no injected clock error:

        truth_C(s) = [Δdt^s − Δdt_piv] − [d_IF^s − d_IF_piv]

    ⚠ THE SIGN IS MEASURED, NOT ASSUMED, AND IT IS THE OPPOSITE OF SPEC §4.2's
    WRITTEN `+`.  Regressing each satellite's settled mean C residual on its
    pivot-relative lump over the graded arm gives slope **−1.08, corr −0.78** (19
    satellites, 233 settled epochs), and removing −lump drops the per-satellite mean
    rms 1.070 → 0.906 m while removing +lump inflates it to 1.786 m.  The convention
    chain that produces it: a Bias-SINEX OSB is a quantity to SUBTRACT from the
    observation, the sim ADDS it to the observable, and the estimated clock enters the
    modeled range with the opposite sign — so the lump lands in the clock negated.
    The magnitude ≈ 1 confirms the mechanism; the 0.9 m that remains after removal is
    genuine per-satellite clock error, not convention.

    Ignoring it would charge the estimator with a ±1 m per-satellite constant that is
    the datum working exactly as designed — the invariant-#5 mistake, on the channel
    the fixture grades most sharply.  Empty on the INT arm (no injected OSB), where
    the second bracket is identically zero and the null test is recovered.
    """
    if not osb_truth or not pin or len(pin) != 2:
        return {}
    j1, j2 = pin
    f1, f2 = CODE_FREQ.get(j1), CODE_FREQ.get(j2)
    if not f1 or not f2 or f1 == f2:
        return {}
    a1 = f1 ** 2 / (f1 ** 2 - f2 ** 2)
    a2 = -f2 ** 2 / (f1 ** 2 - f2 ** 2)
    out = {}
    for (sat, sig), v in osb_truth.items():
        if sig == j1:
            out.setdefault(sat, {})["1"] = v
        elif sig == j2:
            out.setdefault(sat, {})["2"] = v
    # Negated per the measured convention documented above.
    return {s: -(a1 * d["1"] + a2 * d["2"]) for s, d in out.items()
            if "1" in d and "2" in d}


def grade_clock(epochs, s_line, sc_truth, settle_frac, if_lump=None):
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
    lump = if_lump or {}
    n_ep = len(epochs)
    start = int(n_ep * settle_frac)
    res, res_vf, contrast_res = [], [], []
    n_no_sigma, n_pivot_skipped = 0, 0

    for ep in epochs[start:]:
        tow = round(ep["tow"], 3)
        piv = pivots.get("G")
        # Truth for the pivot at this epoch (0 when no clock error is injected), plus
        # the pivot's own absorbed IF code-bias lump on the W6 arm (0 on INT).
        piv_truth = (sc_truth.get((tow, piv), 0.0) + lump.get(piv, 0.0)) if piv else 0.0

        for sat, c0 in ep["clocks"].items():
            if piv and sat == piv:
                n_pivot_skipped += 1
                continue                      # pinned to 0; nothing to grade
            t = sc_truth.get((tow, sat), 0.0) + lump.get(sat, 0.0) - piv_truth  # S[truth]
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
            ta = sc_truth.get((tow, a), 0.0) + lump.get(a, 0.0)
            tb = sc_truth.get((tow, b), 0.0) + lump.get(b, 0.0)
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


FLAG_RE = re.compile(
    r"(?P<tier>[A-Za-z][A-Za-z0-9 /_+-]*\((?:T\d)\))\s*:\s*"
    r"(?P<opp>\d+)\s*opp,\s*(?P<flag>\d+)\s*flag,\s*rate\s*(?P<rate>[-\deE.+]+)"
    r"(?:.*?implied Gaussian a\s*(?P<alpha>[-\deE.+]+))?")

EMIT_RE = re.compile(
    r"Network producer complete:\s*(?P<emitted>\d+) epochs emitted,\s*(?P<skipped>\d+) skipped"
    r"\s*\((?P<gate>\d+) of those withheld by the observability gate,\s*"
    r"(?P<a4>\d+) by the A4")


def grade_qc_tiers(run_dir):
    """Grade the QC stack from the run-end flag-rate summary + the event log.

    THE DISTINCTION THAT MATTERS is opportunity count vs flag count (standing
    invariant #4).  A tier reporting "0 opp" is INERT — its wiring never even got
    a chance to look at anything — and reads in every log exactly like a tier that
    looked and found nothing.  At 3 and 5 stations EVERY tier here is 0 opp, which
    is why the 50-station variant exists.

    For a LIVE tier on a QUIET arm the grade is its false-alarm rate against the
    threshold's OWN implied Gaussian alpha, which the producer prints beside it:
      observed >> implied  the threshold is mis-calibrated for this data, or the
                           residuals are heavier-tailed than Gaussian
      observed << implied  conservative; the tier will also be slow on real faults
    """
    console = os.path.join(run_dir, "console.txt")
    if not os.path.exists(console):
        return None
    text = open(console, encoding="utf-8", errors="replace").read()

    tiers = []
    for m in FLAG_RE.finditer(text):
        opp, flag = int(m.group("opp")), int(m.group("flag"))
        alpha = float(m.group("alpha")) if m.group("alpha") else None
        tiers.append({"tier": m.group("tier").strip(), "opp": opp, "flag": flag,
                      "rate": float(m.group("rate")), "alpha": alpha,
                      "live": opp > 0})

    emit = None
    m = EMIT_RE.search(text)
    if m:
        emit = {k: int(v) for k, v in m.groupdict().items()}

    # network_events.csv: what actually fired, by tier and event name.
    events = {}
    ev_path = os.path.join(run_dir, "logs", "network_events.csv")
    rows = load_csv(ev_path)
    for r in (rows or []):
        key = (r.get("tier", "?"), r.get("event", r.get("event_type", "?")))
        events[key] = events.get(key, 0) + 1

    return {"tiers": tiers, "emit": emit, "events": events,
            "have_events_file": rows is not None}


def detect_ura_scale(run_dir):
    """Detect the scale factor the published URA carries, so vf is interpretable.

    URA = s0 * sqrt(scov(clock,clock)) with s0 = max(1.0, sigma0_EWMA)
    (NetworkSolPrep.cpp:430,443).  That EWMA is STRUCTURALLY UNAVAILABLE unless
    emit_ura + raim + phase_leveling are all armed -- the RTB-6 sentinel -- in which
    case s0 is exactly 1.0 and the URA is the pure marginal formal sigma.

    Flipping phase_leveling therefore moves the vf DENOMINATOR silently, by s0^2.
    Two arms that differ in it cannot have their vf pooled or compared, and nothing
    in the product says so.  Report it explicitly.
    """
    console = os.path.join(run_dir, "console.txt")
    if not os.path.exists(console):
        return None
    text = open(console, encoding="utf-8", errors="replace").read()
    sentinel = "RTB-6" in text
    vals = [float(m) for m in re.findall(r"\(σ̂0 ([0-9.]+)\)", text)]
    s0 = max([1.0] + vals) if vals else (1.0 if sentinel else None)
    return {"sentinel": sentinel, "sigma0_seen": vals, "s0": s0,
            "pure_formal": bool(sentinel) or (s0 == 1.0 if s0 else False)}


def osb_truth_by_key(truth):
    """Injected satellite OSB in metres, keyed (sat, RINEX code).

    Satellite-side and time-constant (it comes straight out of the shipped
    COD0MGXFIN Bias-SINEX), so it is read from one station and the others are
    ASSERTED to agree — the same determinism contract satclock_truth_by_tow uses.
    Non-empty only when the sim ran `corrections.signal_bias: true`, i.e. the W6
    arm; under the INT posture the producer removes the same .BIA the sim injected,
    so there are no B records to grade and this file is absent or all-zero.
    """
    series, disagreements = {}, 0
    for _, st in truth["stations"].items():
        for r in (st["bias"] or []):
            key = (r["satellite"], r["signal"])
            v = float(r["osb_m"])
            if key in series and abs(series[key] - v) > 1e-9:
                disagreements += 1
            series[key] = v
    return series, disagreements


def grade_code_bias(epochs, s_line, osb_truth, sig_rows, settle_frac, gf_datum=False):
    """B record: apply the producer's OWN two-stage S-transform to TRUTH (SPEC §4.4).

    The wire value is not the injected OSB — it is that OSB seen through the W6
    code-bias datum, which is two operations:

      1. IF LUMPING.  The satellite's iono-free combination over the PINNED pair is
         absorbed by the satellite clock (Odijk: the first two frequencies' code
         biases are not separately estimable), so a1*b(s,j1) + a2*b(s,j2) is
         subtracted from every channel of that satellite.  The pair is
         selectIfPinPair's LEXICAL choice, read off the S line's cbpin= token —
         NOT assumed, and not necessarily the consumer's pair
         (project_day129_pivot_cb_ratchet).
      2. ZERO-MEAN PER CHANNEL over that epoch's registered satellite set.

    Both stages are applied to the truth using THIS EPOCH's emitted satellite set,
    so a satellite rising or setting cannot masquerade as a bias error.

    Reported alongside the residual: the ESTIMATE's own per-channel mean, which the
    sum-to-zero datum should hold at 0.  It is a datum-realization check, not a
    grade — if it drifts, the residual statistics below are being measured against a
    datum the producer is not actually realizing.

    vf uses network_sat_bias_states.csv (network.trace_sat_bias_states), because the
    B record itself carries NO sigma in any configuration (§4.8).  Rows whose sigma
    is blank (a datum-pinned column, whose variance is a gauge artefact) are counted
    and excluded rather than silently treated as zero.
    """
    if not osb_truth:
        return {"armed": False}
    pair = (s_line or {}).get("cbpin", {}).get("G")
    if not pair or len(pair) != 2 or not all(pair):
        return {"armed": True, "no_pin": True}
    j1, j2 = pair
    f1, f2 = CODE_FREQ.get(j1), CODE_FREQ.get(j2)
    if not f1 or not f2 or f1 == f2:
        return {"armed": True, "bad_pin": (j1, j2)}
    a1 = f1 ** 2 / (f1 ** 2 - f2 ** 2)
    a2 = -f2 ** 2 / (f1 ** 2 - f2 ** 2)

    # sigma lookup: (tow, sat, signal) -> sigma_m ("" = pinned / no honest variance)
    sig = {}
    for r in (sig_rows or []):
        sig[(round(float(r["tow"]), 3), r["sat"], r["signal"])] = r["sigma_m"].strip()

    n_ep = len(epochs)
    res, res_vf, per_sig = [], [], {}
    est_channel_mean, n_no_truth, n_no_sigma, n_pinned = [], 0, 0, 0
    for ep in epochs[int(n_ep * settle_frac):]:
        tow = round(ep["tow"], 3)
        if not ep["code_bias"]:
            continue
        # Stage 1 — per-satellite datum projection of the TRUTH.
        #
        # Derived, not transcribed.  The unobservable directions in a satellite's code
        # biases are exactly two: the CLOCK direction e = (1,1,…,1) and the IONO
        # direction μ = (μ_1,…,μ_f) with μ_j = (f_1/f_j)².  The datum rows are the
        # iono-free row μ_IF and — in the slant posture — the geometry-free row μ_GF,
        # whose orthogonality properties are μ_IF·e = 1, μ_IF·μ = 0 (Odijk Eq. 26) and
        # μ_GF·e = 0, μ_GF·μ = 1 (Eq. 31).  So R·N = I and the projection is simply
        #
        #     t̃_j = t_j − (μ_IF·t) − μ_j·(μ_GF·t)
        #
        # with the second term present only when the GF row is folded.  Dropping it
        # recovers the single-layer posture's "subtract the IF lump" exactly, so one
        # expression serves both.  Self-check: under the slant datum this forces
        # t̃ = 0 on BOTH pinned channels, which is Odijk §5.1's "not estimable for the
        # first two frequencies" — asserted below rather than assumed.
        sats = sorted({s for (s, _) in ep["code_bias"]})
        lump, gflump, usable = {}, {}, set()
        mu2_minus_mu1 = (f1 * f1) / (f2 * f2) - 1.0
        for s in sats:
            b1, b2 = osb_truth.get((s, j1)), osb_truth.get((s, j2))
            if b1 is None or b2 is None:
                continue                      # cannot transform: pinned pair not injected
            lump[s] = a1 * b1 + a2 * b2
            gflump[s] = ((b2 - b1) / mu2_minus_mu1) if gf_datum else 0.0
            usable.add(s)
        # Stage 2 — zero-mean per channel over THIS epoch's registered set.
        by_sig = {}
        for (s, j), v in ep["code_bias"].items():
            if s not in usable or (s, j) not in osb_truth:
                n_no_truth += 1
                continue
            # mu_j for THIS channel (mu_1 = 1 by definition of the reference band).
            fj = CODE_FREQ.get(j)
            mu_j = (f1 * f1) / (fj * fj) if fj else 1.0
            t = osb_truth[(s, j)] - lump[s] - mu_j * gflump[s]
            by_sig.setdefault(j, []).append((s, v, t))
        for j, items in by_sig.items():
            if not items:
                continue
            t_mean = sum(t for _, _, t in items) / len(items)
            e_mean = sum(v for _, v, _ in items) / len(items)
            est_channel_mean.append(e_mean)
            for s, v, t in items:
                e = v - (t - t_mean)
                res.append(e)
                per_sig.setdefault(j, []).append(e)
                sv = sig.get((tow, s, j))
                if sv is None:
                    n_no_sigma += 1
                elif sv == "":
                    n_pinned += 1
                else:
                    sd = float(sv)
                    if sd > 0:
                        res_vf.append((e / sd) ** 2)
    return {"armed": True, "pin": (j1, j2), "a1": a1, "a2": a2,
            "residual": stats(res), "per_signal": {j: stats(v) for j, v in per_sig.items()},
            "vf": (sum(res_vf) / len(res_vf)) if res_vf else None, "n_vf": len(res_vf),
            "est_channel_mean": stats(est_channel_mean), "n_no_truth": n_no_truth,
            "n_no_sigma": n_no_sigma, "n_pinned": n_pinned}


def grade_rx_states(run_dir, truth, settle_frac):
    """network_rx_states.csv: the receiver DCB, graded as a CONTRAST (SPEC §4.5).

    The estimator carries ONE receiver differential code bias per (station,
    constellation) — key "GPS_DCB" — while the fixture injects a bias per RINEX
    code, so the comparable truth is the injected GF difference d(C1C) - d(C2W).

    Its LEVEL is not gradeable and must not be reported as an error: the Table-8
    identity says the fleet-mean receiver GF bias is absorbed into the ionosphere
    (contract §B.4), so the absolute per-station DCB is datum-bearing.  What IS
    estimable is the BETWEEN-STATION contrast, so that is what carries a verdict
    here; the fleet-mean offset is reported separately as the absorbed gauge, which
    is the quantity the iono channel should show with the opposite sign.
    """
    rows = load_csv(os.path.join(run_dir, "logs", "network_rx_states.csv"))
    if not rows:
        return None
    tt = {}
    for name, st in truth["stations"].items():
        code = {r["code"]: float(r["value_m"])
                for r in (st["rx_bias"] or []) if r["kind"] == "code"}
        if "C1C" in code and "C2W" in code:
            tt[name] = code["C1C"] - code["C2W"]
    # Receiver CLOCK: truth is truth_clock.csv's clock_bias_m per (station, tow).  Like
    # the DCB, only the BETWEEN-STATION contrast is estimable — the network clock datum
    # fixes the common part — so the same contrast/level split applies.
    tclk = {}
    for name, st in truth["stations"].items():
        for r in (load_csv(os.path.join(
                [s for s in truth["manifest"]["stations"]
                 if s["name"] == name][0]["truth_dir"], "truth_clock.csv")) or []):
            try:
                tclk[(name, round(float(r["tow_s"]), 3))] = float(r["clock_bias_m"])
            except (KeyError, ValueError):
                continue
    cb = [r for r in rows if r["kind"] == "CB"]
    clk = [r for r in rows if r["kind"] == "CLK"]
    clk_contrast = []
    if tclk and clk:
        ctows = sorted({round(float(r["tow"]), 3) for r in clk})
        ccut = ctows[int(len(ctows) * settle_frac)] if ctows else 0.0
        per_tow = {}
        for r in clk:
            t = round(float(r["tow"]), 3)
            if t < ccut or (r["station"], t) not in tclk:
                continue
            per_tow.setdefault(t, []).append(
                (r["station"], float(r["value_m"]) - tclk[(r["station"], t)]))
        for t, items in per_tow.items():
            if len(items) < 2:
                continue
            items.sort()
            base = items[0][1]
            clk_contrast.extend(v - base for _, v in items[1:])
    if not cb:
        return {"n_cb": 0, "n_clk": len(clk), "clk_contrast": stats(clk_contrast)}
    tows = sorted({round(float(r["tow"]), 3) for r in cb})
    cut = tows[int(len(tows) * settle_frac)] if tows else 0.0
    by_tow = {}
    for r in cb:
        t = round(float(r["tow"]), 3)
        if t < cut or r["station"] not in tt:
            continue
        by_tow.setdefault(t, []).append((r["station"], float(r["value_m"])))
    contrast, level, sd_used = [], [], []
    for t, items in by_tow.items():
        if len(items) < 2:
            continue
        items.sort()
        # Contrast against the first station in the epoch's set: removes the fleet
        # gauge without needing to know how the producer realized it.
        r0, v0 = items[0]
        for name, v in items[1:]:
            contrast.append((v - v0) - (tt[name] - tt[r0]))
        level.append(sum(v - tt[n] for n, v in items) / len(items))
    for r in cb:
        s = r.get("std_m", "").strip()
        if s:
            sd_used.append(float(s))
    return {"n_cb": len(cb), "n_clk": len(clk), "contrast": stats(contrast),
            "level": stats(level), "sigma": stats(sd_used),
            "clk_contrast": stats(clk_contrast), "n_stations_truth": len(tt)}


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
    ap.add_argument("--gf-datum", dest="gf_datum", action="store_true",
                    help="the run folded the per-satellite GEOMETRY-FREE code-bias pin "
                         "(network.code_bias_gf_datum, the Odijk slant posture).  "
                         "Auto-detected from the run console when not given; the truth "
                         "transform is WRONG for that posture without it, because the "
                         "datum removes a second direction from every channel.")
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

    osb_truth, osb_disagree = osb_truth_by_key(truth)
    if_lump = if_lump_by_sat(osb_truth, (s_line or {}).get("cbpin", {}).get("G"))
    clk = grade_clock(epochs, s_line, sc_truth, args.settle_frac, if_lump)
    print("\n-- C record (satellite clock), truth in the producer's basis " + "-" * 16)
    print(f"  pivot {clk['pivot']} excluded ({clk['n_pivot_skipped']} rows, pinned to 0 by the datum)")
    if if_lump:
        vals = sorted(if_lump.values())
        print(f"  W6 arm: truth includes the ABSORBED IF code-bias lump on {len(if_lump)} "
              f"sats ({vals[0]:+.3f} .. {vals[-1]:+.3f} m, sign MEASURED as −d_IF:")
        print( "          slope −1.08 / corr −0.78 on this arm) — the producer does not apply")
        print( "          the .BIA, so that lump IS in the emitted clock by construction")
    elif osb_truth:
        print("  ⚠ injected OSB present but no cbpin on the S line: the absorbed IF lump")
        print("    CANNOT be removed from the truth, so this residual carries a per-satellite")
        print("    constant of order 1 m that is the datum, not an error.  Arm")
        print("    network.serialize_sbasis_contract before reading the numbers below.")
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
    us = detect_ura_scale(run_dir)
    if us and us["s0"] is not None:
        if us["pure_formal"]:
            print("  σ denominator: URA = pure marginal formal σ (s0 = 1.000) — "
                  "comparable across arms")
        else:
            print(f"  ⚠ σ denominator: URA carries s0 = {us['s0']:.3f}, so it is the formal σ "
                  f"INFLATED {us['s0']:.3f}x.")
            print(f"    This vf is DEFLATED by s0^2 = {us['s0']**2:.2f} vs the pure-formal-σ "
                  f"measure (≈ {clk['vf']*us['s0']**2:.4g} if vf were computed on it)."
                  if clk["vf"] else "")
            print("    phase_leveling makes the σ̂0 EWMA available and moves this scale "
                  "SILENTLY — never pool vf across arms that differ in it.")

    ph = grade_phase(epochs, args.settle_frac)
    print("\n-- P record (phase bias), between-satellite SD, fractional cycles " + "-" * 11)
    if not ph:
        print("  (no population)")
    for sig, s in sorted(ph.items()):
        print(f"  {sig}: {fmt(s, ' cyc')}")

    sig_rows = load_csv(os.path.join(run_dir, "logs", "network_sat_bias_states.csv"))
    gf_datum = args.gf_datum
    if not gf_datum:
        con = os.path.join(run_dir, "console.txt")
        if os.path.exists(con):
            gf_datum = "CB-GF-DATUM" in open(con, encoding="utf-8", errors="replace").read()
    cb = grade_code_bias(epochs, s_line, osb_truth, sig_rows, args.settle_frac, gf_datum)
    print("\n-- B record (satellite code bias), truth through the W6 S-transform " + "-" * 8)
    if not cb.get("armed"):
        print("  NOT ARMED: no injected satellite OSB (truth_bias.csv empty/absent).")
        print("  This is the INT posture — the producer removes the same .BIA the sim")
        print("  injected, so no B records exist and the code-bias channel is untestable.")
        print("  Re-synthesize with corrections.signal_bias = true for the W6 arm (§2.2).")
    elif cb.get("no_pin"):
        print("  injected OSB present but the S line carries NO cbpin= token, so the")
        print("  realized IF-pin pair is unknown and the transform cannot be applied.")
        print("  Arm network.serialize_sbasis_contract; do NOT assume (C1C,C2W).")
    elif cb.get("bad_pin"):
        print(f"  cbpin names {cb['bad_pin']}, whose frequencies are unknown to this "
              f"grader — extend CODE_FREQ.")
    else:
        if osb_disagree:
            print(f"  *** {osb_disagree} (sat,signal) OSB truth DISAGREEMENTS across stations")
        print(f"  IF pin  {cb['pin'][0]}/{cb['pin'][1]}  (a1 {cb['a1']:+.4f}, a2 {cb['a2']:+.4f}) "
              f"— read off the S line, not assumed")
        if gf_datum:
            print( "  GF pin  ARMED (slant posture) — truth is projected along BOTH the")
            print( "          clock and iono directions, so the pinned pair's estimable")
            print( "          content is identically zero and only j >= 3 carries content")
        print(f"  residual  {fmt(cb['residual'], ' m')}")
        for j, s in sorted(cb["per_signal"].items()):
            print(f"    {j}: {fmt(s, ' m')}")
        em = cb["est_channel_mean"]
        if em:
            print(f"  datum check: emitted per-channel mean {em['mean']:+.2e} m "
                  f"(rms {em['rms']:.2e}) — the sum-to-zero datum should hold this at 0")
        if cb["vf"] is not None:
            verdict = ("CALIBRATED" if 0.5 <= cb["vf"] <= 2.0 else
                       "OPTIMISTIC (sigma too small)" if cb["vf"] > 2.0 else
                       "CONSERVATIVE (sigma too large)")
            print(f"  vf        {cb['vf']:.3f}  over {cb['n_vf']} rows   => {verdict}")
            print( "            (from network_sat_bias_states.csv — the B record has no σ)")
        else:
            print("  vf        (no σ channel — arm network.trace_sat_bias_states)")
        if cb["n_pinned"]:
            print(f"  {cb['n_pinned']} rows sat on a datum-PINNED column (σ withheld as a "
                  f"gauge artefact, excluded from vf)")
        if cb["n_no_truth"]:
            print(f"  {cb['n_no_truth']} emitted (sat,signal) had no injected truth "
                  f"(or an untransformable pin) and were skipped")

    rx = grade_rx_states(run_dir, truth, args.settle_frac)
    print("\n-- Receiver states (DCB contrast; level is the absorbed gauge) " + "-" * 14)
    if rx is None:
        print("  no network_rx_states.csv — arm network.trace_rx_states")
    elif not rx.get("n_cb"):
        print(f"  {rx['n_clk']} clock rows but ZERO receiver code-bias rows — "
              f"estimate_ifb is off, so the receiver GF lump rides inside the slant iono")
    else:
        print(f"  {rx['n_cb']} DCB rows / {rx['n_clk']} clock rows, "
              f"{rx['n_stations_truth']} stations with injected truth")
        print(f"  contrast  {fmt(rx['contrast'], ' m')}   <- the estimable part, GRADED")
        print(f"  level     {fmt(rx['level'], ' m')}   <- absorbed into the ionosphere "
              f"(Table-8), NOT an error")
        print(f"  formal σ  {fmt(rx['sigma'], ' m')}")
    if rx and rx.get("clk_contrast") and rx["clk_contrast"]["n"]:
        print(f"  rx clock  {fmt(rx['clk_contrast'], ' m')}   <- between-station "
              f"contrast vs truth_clock.csv (the common part is the network clock datum)")

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

    qc = grade_qc_tiers(run_dir)
    print("\n-- QC tiers " + "-" * 64)
    if qc is None:
        print("  no console.txt in the run dir — cannot read the flag-rate summary")
    else:
        if qc["emit"]:
            e = qc["emit"]
            print(f"  epochs: {e['emitted']} emitted, {e['skipped']} skipped "
                  f"({e['gate']} observability-gate withheld, {e['a4']} A4 σ̂0 ceiling)")
        if not qc["tiers"]:
            print("  no flag-rate summary — arm network.trace_flag_rates")
        else:
            print(f"  {'tier':<26}{'opp':>10}{'flag':>7}{'obs rate':>12}"
                  f"{'implied α':>12}  verdict")
            n_inert = 0
            for t in qc["tiers"]:
                if not t["live"]:
                    n_inert += 1
                    v = "INERT — 0 opportunities, says NOTHING"
                elif t["alpha"] is None:
                    v = "live (absolute bound, no α)"
                elif t["flag"] == 0:
                    v = "live, silent (quiet arm: expected)"
                elif t["rate"] > 10 * t["alpha"]:
                    v = f"live, fires {t['rate']/t['alpha']:.0f}x its own implied α"
                else:
                    v = "live, within its implied α"
                a = f"{t['alpha']:.2e}" if t["alpha"] is not None else "-"
                print(f"  {t['tier']:<26}{t['opp']:>10}{t['flag']:>7}"
                      f"{t['rate']:>12.3e}{a:>12}  {v}")
            print(f"  => {len(qc['tiers']) - n_inert} of {len(qc['tiers'])} tiers LIVE, "
                  f"{n_inert} inert")
        if qc["events"]:
            print("  events fired:")
            for (tier, ev), n in sorted(qc["events"].items(), key=lambda kv: -kv[1])[:12]:
                print(f"    {tier:<22} {ev:<28} {n}")
        elif qc["have_events_file"]:
            print("  network_events.csv present and EMPTY (no events fired)")
        else:
            print("  no network_events.csv — arm network.trace_events")

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
            json.dump({"clock": clk, "phase": ph, "code_bias": cb, "rx_states": rx,
                       "zwd": zwd, "consumer": con, "table8": t8, "s_line": s_line,
                       "n_epochs": len(epochs)}, fh, indent=2, default=str)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
