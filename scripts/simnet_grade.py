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
import collections
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
GPS_L5 = 1176.45e6

# (constellation, RINEX band digit) -> carrier frequency, for the §4.4 S-transform.
# PER CONSTELLATION on purpose.  The old table was a flat signal->frequency map built
# from GPS alone; it happened to give the right numbers for Galileo C1C/C5Q only
# because E1 == L1 == 1575.42 MHz and E5a == L5 == 1176.45 MHz, and it had no entry
# at all for E5b (C7Q/C7X).  A missing entry did not refuse — it fell back to mu = 1.0
# and silently mis-projected truth on the third band, which under the geometry-free
# datum is the ONLY band carrying estimable content.  Band digit rather than full
# signal name keeps a new attribute (C1C vs C1X vs C1W) free.  Mirrors the table
# already proven in scripts/cb_structure_function.py.
FREQ_BY_BAND = {
    "G": {"1": GPS_L1, "2": GPS_L2, "5": GPS_L5},
    "E": {"1": 1575.42e6, "5": 1176.45e6, "6": 1278.75e6, "7": 1207.14e6, "8": 1191.795e6},
    "C": {"1": 1575.42e6, "2": 1561.098e6, "5": 1176.45e6, "6": 1268.52e6, "7": 1207.14e6},
    "J": {"1": GPS_L1, "2": GPS_L2, "5": GPS_L5},
}


def freq(const, signal):
    """Carrier frequency (Hz) of a RINEX observable for a constellation, or None.

    None is a REFUSAL, never a default: a caller that cannot resolve a frequency must
    count and exclude the row rather than substitute one, because every downstream
    quantity (a1/a2, mu_j) is a ratio of frequencies and a wrong one is invisible.
    """
    if not signal or len(signal) < 2:
        return None
    return FREQ_BY_BAND.get(const, {}).get(signal[1])


def wavelength(const, signal):
    """Carrier wavelength (m) of a phase observable, or None (same refusal rule)."""
    f = freq(const, signal)
    return (C_LIGHT / f) if f else None


# --------------------------------------------------------------------------- #
# Product parsing
# --------------------------------------------------------------------------- #

def parse_products(path):
    """Return (s_line dict, epochs list, census Counter).

    Each epoch: {tow, week, clocks {sat: c0_m}, ura {sat: sigma_m},
                 phase {(sat,signal): bias_m}, pb_sigma {(sat,signal): sigma_m},
                 heartbeat {...}}.

    Trailing product fields are SHAPE-dispatched, not positional (a bare int is a
    discontinuity counter; 'sigma=' / 'sigc=' / 'iode=' may appear in any order),
    and which of them exist depends on the producing toggles.  A positional parse
    calibrated on one arm reads the disc counter as a value on another, so every
    positional read below stops at the first field containing '='.

    THE CENSUS IS NOT OPTIONAL BOOKKEEPING.  Records are bucketed into per-epoch
    dicts below, so a product carrying header/epoch/S lines and NOTHING ELSE
    parses cleanly, yields the expected epoch count, and reaches every channel
    grader as a set of empty dicts that each skip silently.  The consumer channel
    then reports a flattering absolute position because the consumer applied
    nothing at all.  That is not hypothetical: on 2026-08-13 two arms of the eq34
    bisection ladder emitted zero B/C/P records, their consumer read 0.037 m, and
    a conclusion ("seed_round breaks the consumer 0.037 -> 585 m") was published
    and then retracted off exactly that number.  Counting raw record types is the
    cheapest possible defence, so it happens in the ONLY reader.
    """
    s_line, epochs, cur = None, [], None
    census = collections.Counter()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split(",")
            rec = p[0]
            # Count the RAW record letter before any dispatch, so types this parser
            # does not model stay visible instead of vanishing.  (They exist: the
            # producer emits H heartbeat records that no branch below consumes and
            # that the docstring's promised `heartbeat` key never captured.)
            census[rec] += 1
            # Orphan = a record this parser WOULD have bucketed, arriving before the
            # first E and therefore actually dropped.  An unmodelled type ahead of
            # the first epoch is not an error, so it is deliberately not counted here.
            if cur is None and rec in ("C", "U", "B", "R", "P"):
                census["_orphan"] += 1
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
    return s_line, epochs, census


def consumer_verdict_void_reason(census):
    """Why the consumer number must NOT be read, or None if it may be.

    B (satellite code bias), C (satellite clock) and P (satellite phase bias) are
    the three record classes a consumer actually APPLIES.  With all three at zero
    the consumer solved with an empty correction set, so its position error is a
    verdict on the consumer's own stand-alone processing — not on the producer,
    and not comparable with any arm that did emit.  A LOW number in that state is
    the dangerous one: it reads as success.

    Returns a short machine-readable reason string, or None.
    """
    if census["C"] == 0 and census["B"] == 0 and census["P"] == 0:
        return "no_bcp"
    return None


def consumer_verdict_caveats(census):
    """Record classes that are absent but not ALL absent — a partial correction set.

    Not a refusal: a clock-only product is a legitimate posture.  But it changes
    what the consumer number MEANS, and comparing it against an arm with a fuller
    record set is an unlabelled apples-to-oranges A/B, so it is named.
    """
    return [r for r in ("C", "B", "P") if census[r] == 0]


def parse_s_line(p):
    """Decode the S-basis contract line.  Fields 11+ are shape-dispatched."""
    out = {"ref_rx": p[1], "apriori_id": p[10] if len(p) > 10 else "",
           "pivots": {}, "cbpin": {}, "gfpin": {}, "extra": {}}
    for f in p[11:]:
        if "=" in f:
            k, _, v = f.partition("=")
            if k in ("cbpin", "gfpin"):
                # cbpin=<const>:<s1>-<s2>   — the IF-pinned pair (the frame the OSB
                #                             stream is internally consistent in)
                # gfpin=<const>:<s1>-<s2>   — Odijk rd 3b: the SAME pair also carries a
                #                             geometry-free datum row, so both pinned
                #                             channels' B records ARE the datum and the
                #                             GF lump sits in the ionosphere (Table 8).
                # A '+'-joined value is the per-family form (SPEC_family_gf_datum); this
                # grader does not implement the per-family transform, so it records the
                # raw value and refuses rather than grading the first family silently.
                cst, _, pair = v.partition(":")
                if "+" in pair:
                    out[k][cst] = ("MULTIFAMILY", pair)
                else:
                    a, _, b = pair.partition("-")
                    out[k][cst] = (a, b)
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
    if not osb_truth or not pin:
        return {}
    # `pin` is the whole cbpin MAP (constellation -> (s1,s2)).  It used to be a single
    # pair, taken from cbpin["G"]: on a GPS+Galileo product that applied the GPS pin
    # to Galileo satellites, whose pinned pair is a different band entirely.
    out = {}
    for (sat, sig), v in osb_truth.items():
        if not sat:
            continue
        cst = sat[0]
        pair = pin.get(cst)
        # The MULTIFAMILY sentinel IS a 2-tuple, so a bare `len(pair) != 2` test lets it
        # through and unpacks ("MULTIFAMILY", "<raw>") as if they were signal names — no
        # OSB key ever matches, so the lump silently comes out EMPTY rather than refusing.
        # Fail closed: an unresolved per-family pin must never look like "no truth here".
        if not pair or len(pair) != 2 or pair[0] == "MULTIFAMILY":
            continue
        j1, j2 = pair
        if sig == j1:
            out.setdefault(sat, {})["1"] = v
        elif sig == j2:
            out.setdefault(sat, {})["2"] = v
    lumps = {}
    for s, d in out.items():
        if "1" not in d or "2" not in d:
            continue
        j1, j2 = pin[s[0]]
        f1, f2 = freq(s[0], j1), freq(s[0], j2)
        if not f1 or not f2 or f1 == f2:
            continue
        a1 = f1 ** 2 / (f1 ** 2 - f2 ** 2)
        a2 = -f2 ** 2 / (f1 ** 2 - f2 ** 2)
        # Negated per the measured convention documented above.
        lumps[s] = -(a1 * d["1"] + a2 * d["2"])
    return lumps


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
        # The clock gauge is PER CONSTELLATION (one pinned pivot each), so the pivot
        # and its truth must be resolved per satellite.  Reading pivots["G"] alone
        # left every Galileo clock carrying its own unremoved pivot datum, and on a
        # Galileo-only product removed nothing at all.
        piv_truth_of = {}
        for cst, pv in pivots.items():
            piv_truth_of[cst] = sc_truth.get((tow, pv), 0.0) + lump.get(pv, 0.0)

        for sat, c0 in ep["clocks"].items():
            if not sat:
                continue
            cst = sat[0]
            piv = pivots.get(cst)
            if piv and sat == piv:
                n_pivot_skipped += 1
                continue                      # pinned to 0; nothing to grade
            piv_truth = piv_truth_of.get(cst, 0.0)
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
        # Contrasts are formed WITHIN a constellation only.  A G-vs-E clock difference
        # is not datum-invariant — each constellation carries its own pivot, so the
        # contrast retains the inter-system bias and would read as a residual.
        by_cst = {}
        for s in ep["clocks"]:
            if s:
                by_cst.setdefault(s[0], []).append(s)
        for cst, group in by_cst.items():
            group.sort()
            for i in range(len(group) - 1):
                a, b = group[i], group[i + 1]
                ta = sc_truth.get((tow, a), 0.0) + lump.get(a, 0.0)
                tb = sc_truth.get((tow, b), 0.0) + lump.get(b, 0.0)
                contrast_res.append((ep["clocks"][a] - ep["clocks"][b]) - (ta - tb))

    vf = sum(res_vf) / len(res_vf) if res_vf else None
    return {"residual": stats(res), "contrast": stats(contrast_res), "vf": vf,
            "n_vf": len(res_vf), "n_no_sigma": n_no_sigma,
            "pivot": dict(pivots), "n_pivot_skipped": n_pivot_skipped}


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
            if not sat:
                continue
            # Key on (constellation, signal): the between-satellite SD is only
            # integer x lambda WITHIN a constellation, and lambda itself is
            # constellation-dependent (L5 and E5a coincide, L2 and E5b do not).
            # Pooling them was harmless while only GPS emitted; on a GPS+Galileo
            # posture-C product it would difference across two datums.
            by_sig.setdefault((sat[0], sig), []).append((sat, v))
        for (cst, sig), items in by_sig.items():
            lam = wavelength(cst, sig)
            if not lam or len(items) < 2:
                continue
            items.sort()
            ref = items[0][1]
            for _, v in items[1:]:
                cyc = (v - ref) / lam
                f = cyc - round(cyc)
                fracs.setdefault(f"{cst}:{sig}", []).append(f)
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


def grade_code_bias(epochs, s_line, osb_truth, sig_rows, settle_frac, gf_datum=False,
                    gf_forced=False):
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

    # Which constellations actually emit B records?  The datum is realized PER
    # CONSTELLATION (its own IF pin, its own sum-to-zero channels, its own GF datum),
    # so every quantity below is resolved per constellation.  Reading cbpin["G"] alone
    # refused a Galileo-only product outright and, worse, applied the GPS pair and the
    # GPS frequencies to Galileo satellites on a mixed product.
    consts = sorted({s[0] for (s, _) in osb_truth if s})
    emitted = sorted({s[0] for ep in epochs for (s, _) in ep["code_bias"] if s})
    consts = [c for c in consts if c in emitted] or emitted

    cbpin = (s_line or {}).get("cbpin", {})
    gfpin = (s_line or {}).get("gfpin", {})

    # Which receiver channel FAMILIES does each constellation actually emit?  Mirrors the
    # producer's NetworkSbasis::channelFamilyOf: 'X' is the combined I+Q correlator output
    # and a physically distinct bias; every discrete component is one family.  Deliberately
    # NOT keyed on the raw attribute character — the Galileo primary family is C1C/C5Q/C7Q,
    # attributes C, Q, Q.
    def _family(sig):
        return "X" if (len(sig) > 2 and sig[2] == "X") else "P"

    emitted_fams = {}
    for ep in epochs:
        for (s, j) in ep["code_bias"]:
            if s and j:
                emitted_fams.setdefault(s[0], set()).add(_family(j))

    datum, refused = {}, {}
    for c in consts:
        pair = cbpin.get(c)
        if not pair or len(pair) != 2 or not all(pair):
            refused[c] = "no_pin"
            continue
        j1, j2 = pair
        if j1 == "MULTIFAMILY":
            refused[c] = "multifamily"
            continue
        f1, f2 = freq(c, j1), freq(c, j2)
        if not f1 or not f2 or f1 == f2:
            refused[c] = f"bad_pin:{j1},{j2}"
            continue
        gf = gfpin.get(c)
        # Per-family GF datum (SPEC_family_gf_datum §5).  `gfpin=<c>:<p1>-<p2>+<s1>-<s2>`
        # declares the PRIMARY family's pair and each partitioned SECONDARY family's pair.
        # Removal is asymmetric and that asymmetry IS the card: the primary family loses
        # its IF direction (the clock absorbed it) AND its GF direction; a secondary family
        # loses ONLY its GF direction, because its IF combination is the estimable
        # inter-family ISC and is graded, not projected away.
        fam_gf = {}
        if gf and gf[0] == "MULTIFAMILY":
            ok = True
            for blk in gf[1].split("+"):
                a, _, b = blk.partition("-")
                fa, fb = freq(c, a), freq(c, b)
                if not a or not b or not fa or not fb or fa == fb:
                    ok = False
                    break
                fam_gf[_family(a)] = {
                    "pair": (a, b), "f1": fa,
                    "mu2_minus_mu1": (fa * fa) / (fb * fb) - 1.0,
                }
            if not ok or _family(j1) not in fam_gf:
                refused[c] = f"bad_multifamily_gfpin:{gf[1]}"
                continue
        # THE SCREEN §D.4 DESCRIBES, and the one that would have caught the 2026-08-12
        # `_pc305_gg_tri_slant_20m` posture.  selectIfPinPair resolves EVERY satellite of a
        # Q+X roster to the primary pair, deterministically and unanimously, so a producer
        # with no family awareness emits a confident single-family `gfpin=E:C1C-C5Q` on a
        # two-family product — and grading the X channels through the Q family's transform
        # is exactly the silent mis-framing the token exists to prevent.  Refuse instead:
        # more than one emitted family REQUIRES a per-family declaration.
        if len(emitted_fams.get(c, set())) > 1 and not fam_gf:
            refused[c] = ("multifamily_emitted_no_token:"
                          f"{','.join(sorted(emitted_fams[c]))}")
            continue
        datum[c] = {
            "pin": (j1, j2), "f1": f1, "f2": f2,
            "a1": f1 ** 2 / (f1 ** 2 - f2 ** 2),
            "a2": -f2 ** 2 / (f1 ** 2 - f2 ** 2),
            "mu2_minus_mu1": (f1 * f1) / (f2 * f2) - 1.0,
            "fam_gf": fam_gf,          # {} = single-family product (the legacy path)
            "primary_fam": _family(j1),
            # Per-constellation GF datum, in strict precedence order:
            #   explicit --gf-datum (gf_forced) > gfpin= on the S line > fallback.
            # The fallback (gf_datum with no wire token) covers products written before
            # the token existed, where the only evidence is a console scrape.
            "gf": True if gf_forced else (bool(gf) if gfpin else bool(gf_datum)),
        }
    if not datum:
        return {"armed": True, "no_pin": True, "refused": refused,
                "consts": consts, "declared": sorted(cbpin)}

    # sigma lookup: (tow, sat, signal) -> sigma_m ("" = pinned / no honest variance)
    sig = {}
    for r in (sig_rows or []):
        sig[(round(float(r["tow"]), 3), r["sat"], r["signal"])] = r["sigma_m"].strip()

    n_ep = len(epochs)
    res, res_vf, per_sig = [], [], {}
    est_channel_mean, n_no_truth, n_no_sigma, n_pinned = [], 0, 0, 0
    n_no_freq = 0
    # §5.3: the self-check the transform's own comment promises ("asserted below rather
    # than assumed") and never performed.  Under the GF datum a pinned channel's projected
    # truth must be identically 0 — Odijk §5.1's "not estimable for the first two
    # frequencies", realized as a datum.  It is the one identity that catches a WRONG
    # FAMILY PAIRING silently: mis-assign a family and the pinned channels stop vanishing.
    n_pin_residual = 0
    max_pin_residual = 0.0
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
        gf_fam_lump = {}                  # sat -> {family: GF lump in that family's frame}
        pin_check = {}                    # (sat, secondary family) -> {signal: (t, fam_gf)}
        for s in sats:
            d = datum.get(s[0] if s else "")
            if not d:
                continue                      # constellation refused above
            j1, j2 = d["pin"]
            b1, b2 = osb_truth.get((s, j1)), osb_truth.get((s, j2))
            if b1 is None or b2 is None:
                continue                      # cannot transform: pinned pair not injected
            lump[s] = d["a1"] * b1 + d["a2"] * b2
            gflump[s] = ((b2 - b1) / d["mu2_minus_mu1"]) if d["gf"] else 0.0
            # Per-family GF lumps: each family's own pinned pair, in its OWN mu convention
            # (mu is pair-relative, exactly as the producer computes it).  A family whose
            # pinned pair is not present in truth contributes nothing and its channels fall
            # back to the primary lump below, where the missing-truth counter catches them.
            if d["fam_gf"]:
                fl = {}
                for fam, fg in d["fam_gf"].items():
                    p1, p2 = fg["pair"]
                    v1, v2 = osb_truth.get((s, p1)), osb_truth.get((s, p2))
                    if v1 is None or v2 is None:
                        continue
                    fl[fam] = (v2 - v1) / fg["mu2_minus_mu1"]
                gf_fam_lump[s] = fl
            usable.add(s)
        # Stage 2 — zero-mean per (CONSTELLATION, channel) over THIS epoch's registered
        # set.  That is the producer's own datum key: NetworkSbasis builds the
        # sum-to-zero rows on constellationOf(sat)+"|"+signal.  Keying on the signal
        # alone pooled G and E wherever they share a code name (C1C, C5Q on posture C)
        # into a mean the producer never formed.
        by_sig = {}
        for (s, j), v in ep["code_bias"].items():
            if s not in usable or (s, j) not in osb_truth:
                n_no_truth += 1
                continue
            d = datum[s[0]]
            # mu_j for THIS channel (mu_1 = 1 by definition of the reference band).
            # An unresolvable frequency is EXCLUDED and counted — never defaulted.  The
            # old `mu_j = 1.0` fallback silently mis-projected truth on exactly the
            # bands it could not name (E5b C7Q/C7X), which under the GF datum are the
            # only bands carrying estimable content.
            fj = freq(s[0], j)
            if not fj:
                n_no_freq += 1
                continue
            fam = _family(j)
            fg = d["fam_gf"].get(fam) if d["fam_gf"] else None
            if fg is not None and fam != d["primary_fam"]:
                # SECONDARY family: remove ONLY this family's geometry-free direction, in
                # its own mu convention.  The IF direction is deliberately KEPT — it is the
                # per-satellite inter-family ISC, a real estimated quantity graded against
                # injected truth.  Subtracting the primary family's IF lump here would
                # project away exactly the content this card makes estimable.
                fl = gf_fam_lump.get(s, {}).get(fam)
                if fl is None:
                    n_no_truth += 1
                    continue
                mu_j = (fg["f1"] * fg["f1"]) / (fj * fj)
                t = osb_truth[(s, j)] - mu_j * fl
            else:
                # PRIMARY family (or a single-family product): the clock absorbed the IF
                # direction and the ionosphere absorbed the GF one, so both come out.
                mu_j = (d["f1"] * d["f1"]) / (fj * fj)
                gl = gflump[s]
                if fg is not None:
                    gl = gf_fam_lump.get(s, {}).get(fam, gflump[s])
                t = osb_truth[(s, j)] - lump[s] - mu_j * gl
            # Pinned-channel identity — and it is DIFFERENT per family, which is exactly
            # the thing worth asserting.  The PRIMARY family has both its IF and its GF
            # direction removed, so each pinned channel projects to identically 0 (Odijk
            # §5.1).  A SECONDARY family has only its GF direction removed, so its pinned
            # channels do NOT vanish — they still carry the inter-family ISC.  What must
            # vanish there is the GF COMBINATION of the pair.  Checking |t| = 0 on a
            # secondary channel would be asserting the over-pin this card exists to avoid.
            if d["gf"]:
                if fg is None or fam == d["primary_fam"]:
                    if j in d["pin"] and abs(t) > 1e-9:
                        n_pin_residual += 1
                        max_pin_residual = max(max_pin_residual, abs(t))
                elif j in fg["pair"]:
                    pin_check.setdefault((s, fam), {})[j] = (t, fg)
            by_sig.setdefault((s[0], j), []).append((s, v, t))
        # Secondary-family identity, evaluated once both pinned channels are projected:
        # GF(t) over the family's own pair must be 0, while the pair's IF combination is
        # left alone (it is the graded ISC).
        for (s_, fam_), got in pin_check.items():
            if len(got) != 2:
                continue
            fg = next(iter(got.values()))[1]
            p1, p2 = fg["pair"]
            if p1 not in got or p2 not in got:
                continue
            gf_t = (got[p2][0] - got[p1][0]) / fg["mu2_minus_mu1"]
            if abs(gf_t) > 1e-9:
                n_pin_residual += 1
                max_pin_residual = max(max_pin_residual, abs(gf_t))
        for (cst, j), items in by_sig.items():
            if not items:
                continue
            t_mean = sum(t for _, _, t in items) / len(items)
            e_mean = sum(v for _, v, _ in items) / len(items)
            est_channel_mean.append(e_mean)
            for s, v, t in items:
                e = v - (t - t_mean)
                res.append(e)
                per_sig.setdefault(f"{cst}:{j}", []).append(e)
                sv = sig.get((tow, s, j))
                if sv is None:
                    n_no_sigma += 1
                elif sv == "":
                    n_pinned += 1
                else:
                    sd = float(sv)
                    if sd > 0:
                        res_vf.append((e / sd) ** 2)
    return {"armed": True, "datum": datum, "refused": refused, "consts": consts,
            "residual": stats(res), "per_signal": {j: stats(v) for j, v in per_sig.items()},
            "vf": (sum(res_vf) / len(res_vf)) if res_vf else None, "n_vf": len(res_vf),
            "est_channel_mean": stats(est_channel_mean), "n_no_truth": n_no_truth,
            "n_no_sigma": n_no_sigma, "n_pinned": n_pinned, "n_no_freq": n_no_freq,
            "n_pin_residual": n_pin_residual, "max_pin_residual": max_pin_residual}


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
    s_line, epochs, census = parse_products(prod)
    sc_truth, sc_disagree = satclock_truth_by_tow(truth)
    void_reason = consumer_verdict_void_reason(census)

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
        print(f"  realized datum: pivot {s_line['pivots']}  ref_rx {s_line['ref_rx']}")
        print(f"                  cbpin {s_line['cbpin'] or '(none declared)'}"
              f"  gfpin {s_line['gfpin'] or '(none — single-layer code-bias datum)'}")
        print(f"  apriori_id   : {s_line['apriori_id']}")
        print( "                 (NOT trusted as a date key — the product day-edge guard")
        print( "                  loads the neighbouring day and this names only one file)")
    else:
        print("  realized datum: *** NO S LINE *** — arm network.serialize_sbasis_contract.")
        print("                  Without it the datum must be ASSUMED, which is exactly")
        print("                  the invariant-#5 mistake this grader exists to avoid.")

    # The census runs FIRST, ahead of every channel, because it decides whether the
    # channels below are grading a product or an empty file.  Every per-channel skip
    # downstream is silent by design (an epoch with no B records just `continue`s), so
    # without this block a zero-record product is indistinguishable from a clean one.
    print("\n-- Product record census (read BEFORE any number below) " + "-" * 20)
    print(f"  S {census['S']:>7}   E {census['E']:>7}   C {census['C']:>7}   "
          f"U {census['U']:>7}")
    print(f"  B {census['B']:>7}   P {census['P']:>7}   R {census['R']:>7}")
    unmodelled = sorted(k for k in census
                        if k not in ("S", "E", "C", "U", "B", "R", "P", "_orphan"))
    if unmodelled:
        print(f"  not modelled by this parser: "
              f"{', '.join(f'{k} {census[k]}' for k in unmodelled)}")
    if census["_orphan"]:
        print(f"  *** {census['_orphan']} C/U/B/R/P record(s) appear BEFORE the first E record")
        print( "      and were DROPPED.  Every count above is a lower bound and every")
        print( "      channel population below is short by an unknown amount.")
    if void_reason == "no_bcp":
        print( "  ZERO B/C/P — the product carries no satellite code bias, no clock and no")
        print( "  phase bias.  It is header/epoch/S records only.  Nothing downstream of the")
        print( "  producer can APPLY anything, so the consumer verdict is VOID (refused below)")
        print( "  and every channel grade is a statement about an empty population.")
    else:
        missing = consumer_verdict_caveats(census)
        if missing:
            print(f"  ⚠ absent record class(es): {', '.join(missing)} — the correction set is")
            print( "    PARTIAL, so the consumer number is not comparable with an arm that")
            print( "    emitted them.  Label any A/B that spans this difference.")

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
    if_lump = if_lump_by_sat(osb_truth, (s_line or {}).get("cbpin", {}))
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
    # GF-datum detection, in precedence order: explicit flag > gfpin= on the S line >
    # console scrape > OFF.  The wire token is the only source that travels WITH an
    # archived product; console.txt is written by the run-arm PowerShell wrappers, not
    # by the producer, so it is absent for any product graded away from its run dir.
    gf_src = None
    gf_datum = args.gf_datum
    if gf_datum:
        gf_src = "--gf-datum flag"
    elif (s_line or {}).get("gfpin"):
        gf_datum, gf_src = True, "gfpin= on the S line"
    else:
        con = os.path.join(run_dir, "console.txt")
        if os.path.exists(con):
            gf_datum = "CB-GF-DATUM" in open(con, encoding="utf-8", errors="replace").read()
            if gf_datum:
                gf_src = "console.txt scrape (no gfpin= on the wire)"
    cb = grade_code_bias(epochs, s_line, osb_truth, sig_rows, args.settle_frac, gf_datum,
                         gf_forced=bool(args.gf_datum))
    print("\n-- B record (satellite code bias), truth through the W6 S-transform " + "-" * 8)
    if not cb.get("armed"):
        print("  NOT ARMED: no injected satellite OSB (truth_bias.csv empty/absent).")
        print("  This is the INT posture — the producer removes the same .BIA the sim")
        print("  injected, so no B records exist and the code-bias channel is untestable.")
        print("  Re-synthesize with corrections.signal_bias = true for the W6 arm (§2.2).")
    elif cb.get("no_pin"):
        print(f"  injected OSB present for constellation(s) {','.join(cb.get('consts') or [])} "
              f"but no usable cbpin= on the S line, so the realized IF-pin pair is unknown")
        print("  and the transform cannot be applied.  Refused per constellation: "
              + str(cb.get("refused")))
        print(f"  The S line declares cbpin for: {cb.get('declared') or '(nothing)'}")
        print("  do NOT assume (C1C,C2W).  If the S line exists but carries no cbpin= at")
        print("  all, network.serialize_sbasis_contract is NOT the missing knob — check")
        print("  that the producer build has the unconditional §1.5 declaration record")
        print("  (before 2026-08-13 the fill was gated on network.emit_clock_disc, which")
        print("  defaults false and is absent from the whole ft63 -> pc305 field chain,")
        print("  so every field posture-C product declared no pin).")
    else:
        if osb_disagree:
            print(f"  *** {osb_disagree} (sat,signal) OSB truth DISAGREEMENTS across stations")
        for c, d in sorted(cb["datum"].items()):
            print(f"  [{c}] IF pin  {d['pin'][0]}/{d['pin'][1]}  "
                  f"(a1 {d['a1']:+.4f}, a2 {d['a2']:+.4f}) — read off the S line, not assumed"
                  + ("   + GF datum" if d["gf"] else ""))
        if cb.get("refused"):
            print(f"  *** constellation(s) NOT graded (no usable datum on the wire): "
                  f"{cb['refused']}")
        if gf_datum:
            print(f"  GF pin  ARMED (slant posture; source: {gf_src}) — truth is projected")
            print( "          along BOTH the clock and iono directions, so the pinned pair's")
            print( "          estimable content is identically zero and only j >= 3 carries")
            print( "          content.  The geometry-free lump sits in the ionosphere")
            print( "          (Odijk Table 8), which this product does NOT emit.")
        if cb.get("n_pin_residual"):
            print(f"  *** {cb['n_pin_residual']} PINNED-channel row(s) did NOT project to "
                  f"zero (max |t| = {cb['max_pin_residual']:.3e} m).  Under the GF datum a")
            print("      pinned channel IS the datum, so its projected truth must vanish "
                  "identically.  A")
            print("      nonzero residual means the transform paired a channel with the "
                  "WRONG family or the")
            print("      wrong pin — the grade below is measured against a frame the "
                  "producer did not realize.")
        if cb.get("n_no_freq"):
            print(f"  *** {cb['n_no_freq']} row(s) EXCLUDED: no carrier frequency known for the")
            print( "      (constellation, band) — extend FREQ_BY_BAND.  These are excluded, not")
            print( "      defaulted: a wrong mu_j is invisible in the residual.")
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
    if void_reason == "no_bcp":
        # REFUSED, not caveated.  The numbers are withheld on purpose: a reader who
        # can see them will quote them, and the failure mode is that they look GOOD.
        # 2026-08-13: an arm in this exact state read 0.037 m and a published verdict
        # was built on it before the record census was run.
        print("  *** REFUSED — the product emitted ZERO B/C/P records.")
        print("      The consumer applied no clock, no code bias and no phase bias, so")
        print("      closed_loop_trace.csv measures its own stand-alone processing, not")
        print("      this producer.  A LOW number here is the dangerous one: it reads as")
        print("      success and is achieved by applying nothing.")
        print("      Numbers withheld deliberately.  Fix the producer until B/C/P are")
        print("      non-zero, then re-grade; do not compare this arm against any other.")
        if con is not None:
            con = dict(con)
            con["refused"] = void_reason
            for k in ("err3d", "up"):
                con.pop(k, None)
    elif con is None:
        print("  no closed_loop_trace.csv")
    else:
        missing = consumer_verdict_caveats(census)
        if missing:
            print(f"  ⚠ PARTIAL correction set (no {', '.join(missing)} records) — "
                  f"comparable only")
            print( "    against an arm with the same absent classes.")
        print(f"  3D error  {fmt(con['err3d'], ' m')}")
        print(f"  up        {fmt(con['up'], ' m')}")
        print(f"  epochs {con['n_epochs']}, AR accepted {con['ar_success']}")

    print("\n" + "=" * 78)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"clock": clk, "phase": ph, "code_bias": cb, "rx_states": rx,
                       "zwd": zwd, "consumer": con, "table8": t8, "s_line": s_line,
                       "n_epochs": len(epochs),
                       # Machine-readable so a batch harness can filter voided arms out
                       # of a comparison table instead of averaging them into it.
                       "product_census": dict(census),
                       "consumer_void_reason": void_reason,
                       "consumer_absent_classes": consumer_verdict_caveats(census)},
                      fh, indent=2, default=str)
        print(f"wrote {args.json}")
    # A voided consumer verdict is an INPUT defect, not a grading opinion, so it moves
    # the exit code — this is the second condition in the file to do so, after a missing
    # products.txt.  A batch harness that ignores stdout still cannot bank the number.
    return 3 if void_reason else 0


if __name__ == "__main__":
    sys.exit(main())
