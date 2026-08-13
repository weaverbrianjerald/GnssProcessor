"""Card CB-Q -- floor-vs-walk split of the EMITTED satellite code-bias wire.

The CB analogue of ``q3_clock_structure_function.py``.  For each
(constellation, signal) the per-satellite emitted OSB series is differenced over
a lag L, the per-epoch-pair COMMON MODE is removed over the satellites present
at both endpoints, and the robust scale of what is left is the structure
function SF(L).  White estimation noise is FLAT in L; a random walk of rate q
grows as sqrt(q*L); a DRIFT grows as L.  Which of the three the wire actually
shows is the measurement -- and on DOY-129 it is the third, which is why this
script reports the doubling ratio rather than assuming a walk.

## Why the common-mode removal is mandatory (standing invariant #5)

The satellite code biases carry a per-(constellation, SIGNAL) sum-to-zero datum
realized over the satellites PRESENT THIS EPOCH (see
project_network_code_bias_scope).  When the tracked set changes, the realized
mean moves and EVERY satellite of that group steps by the same amount -- a
gauge re-realization, not motion.  Differencing only over the pair-common set
and removing that set's own centre makes SF(L) invariant to it by construction,
which is the same discipline Card Q3 applied to the clock.

## The IF-pin NULL CONTROL (--pin)

Each satellite's OSBs carry one deterministic pin: the ionosphere-free
combination of the selected pin pair is tied to that satellite's clock
(``NetworkSbasis::selectIfPinPair`` -- lowest band digit, then lowest signal
name, then the first distinct frequency).  That direction is fixed BY
CONSTRUCTION, so its structure function must read ~0.  It is the instrument's
own null test: a parser bug, a bad gauge removal or an epoch-key mismatch would
show up there first.  Measured on runs/agu63: SF_IF(C1C,C2W) = 0.003 cm at
30 s against 0.642 cm for the raw signals -- the pipeline reads a pinned
direction as pinned to four digits.

## What this measures, and what it does NOT

The emitted series is the FILTERED state, not an independent measurement of the
bias.  A stiff carry suppresses its own output, so a q fitted here is a LOWER
BOUND on the rate the data are pushing -- read it as "the wire already moves at
least this fast", never as "the truth moves only this fast".  Card Q3 avoided
that circularity with the phase-derived TDCP shadow; the code side has no such
independent instrument, so the bound is stated rather than hidden.  The
companion upper-side estimate comes from the prior-residual census (``cb``
family variance factor) -- see docs/SPEC_cb_carry_physical_q.md.

Usage:
  python scripts/cb_structure_function.py <run_dir_or_products.txt> [...]
        [--lags 30,60,120,300,600,1200,2400,4800,9600,19200]
        [--fit-from 600] [--min-pairs 200] [--min-sats 6]
        [--skip-hours 4] [--center median|mean]
        [--by-signal] [--pin auto|C1C,C2W|off] [--drift]
"""
import argparse
import collections
import math
import os
import statistics
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# Carrier frequencies (Hz) by constellation + RINEX band digit — enough for the
# IF combination of any pin pair this producer can select.
FREQ = {
    "G": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6},
    "E": {"1": 1575.42e6, "5": 1176.45e6, "6": 1278.75e6, "7": 1207.14e6, "8": 1191.795e6},
    "C": {"1": 1575.42e6, "2": 1561.098e6, "5": 1176.45e6, "6": 1268.52e6, "7": 1207.14e6},
    "J": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6},
}


def load(path):
    """products.txt -> (path, series, disc, cbpin)

    series[(const, signal)][sat][tow] = code_bias_m
    disc[sat] = sorted tows at which a discontinuity was SIGNALLED (per-record
                ``code_disc`` bump or an R retro-mark).
    """
    if os.path.isdir(path):
        cand = [os.path.join(path, "products.txt"),
                os.path.join(path, "logs", "products.txt")]
        path = next((c for c in cand if os.path.exists(c)), cand[0])
    series = collections.defaultdict(lambda: collections.defaultdict(dict))
    disc = collections.defaultdict(list)
    cbpin = {}
    tow = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            p = line.rstrip("\n").split(",")
            k = p[0]
            if k == "E":
                # Snap to the millisecond: the emitted TOW is a double that
                # round-trips as 259229.99999809265, so a bare float key makes
                # every `t + L` lookup miss and the whole SF reads zero pairs.
                tow = round(float(p[2]), 3)
            elif k == "B" and tow is not None and len(p) >= 4:
                try:
                    v = float(p[3])
                except ValueError:
                    continue
                sat, sig = p[1], p[2]
                series[(sat[0], sig)][sat][tow] = v
                if len(p) >= 5 and p[4] not in ("", "0"):
                    disc[sat].append(tow)
            elif k == "R" and tow is not None:
                for s in p[1:]:
                    if s:
                        disc[s].append(tow)
            elif k == "S":
                for tok in p[1:]:
                    if tok.startswith("cbpin="):          # cbpin=G:C1C-C2L
                        body = tok[len("cbpin="):]
                        if ":" in body and "-" in body:
                            c, pair = body.split(":", 1)
                            a, b = pair.split("-", 1)
                            cbpin[c] = (a, b)
    for s in disc:
        disc[s] = sorted(set(disc[s]))
    return path, series, disc, cbpin


def pin_pair(const, signals):
    """Reproduce NetworkSbasis::selectIfPinPair on the signals actually emitted.

    Strict total order: band digit, THEN full signal name.  First = lowest;
    second = the first with a DIFFERENT frequency.  (The non-stable-sort bug
    that made this order-dependent is documented in
    project_network_code_bias_scope.)
    """
    f = FREQ.get(const, {})
    ordered = sorted(signals, key=lambda s: (s[1], s))
    if not ordered:
        return None
    first = ordered[0]
    f1 = f.get(first[1])
    for s in ordered[1:]:
        f2 = f.get(s[1])
        if f1 and f2 and abs(f1 - f2) > 1.0:
            return (first, s)
    return None


def spans_disc(marks, t0, t1):
    """True when a signalled discontinuity falls in (t0, t1]."""
    for m in marks:
        if t0 < m <= t1:
            return True
    return False


def sf_at_lag(groups, disc, L, min_sats, center):
    """Robust structure function at lag L over {label: {sat: {tow: value}}}."""
    d = []
    for bysat in groups:
        tset = {t for s in bysat.values() for t in s}
        for t in sorted(tset):
            if (t + L) not in tset:
                continue
            row = [s[t + L] - s[t] for sat, s in bysat.items()
                   if t in s and (t + L) in s
                   and not spans_disc(disc.get(sat, ()), t, t + L)]
            n = len(row)
            if n < min_sats:
                continue
            c = statistics.median(row) if center == "median" else sum(row) / n
            # Removing the group's own centre costs one degree of freedom; undo
            # that shrinkage so SF estimates the PER-SATELLITE dispersion.
            g = math.sqrt(n / (n - 1.0))
            d.extend(abs((v - c) * g) for v in row)
    if not d:
        return None, 0
    d.sort()
    return 1.4826 * d[len(d) // 2], len(d)


def report(groups, disc, a, title, note=""):
    print(f"\n=== {title} ===" + (f"  [{note}]" if note else ""))
    print(" lag L[s]   SF robust [cm]    x per L-doubling      pairs")
    res, prev = {}, None
    for L in (int(x) for x in a.lags.split(",")):
        sf, n = sf_at_lag(groups, disc, L, a.min_sats, a.center)
        if sf is None or n < a.min_pairs:
            print(f" {L:6d}    (only {n} pairs -- skipped)")
            prev = None
            continue
        res[L] = sf
        # The DIAGNOSTIC column: 1.0 = white floor, 1.41 = random walk, 2.0 = drift.
        rat = ""
        if prev and prev[1] > 0:
            rat = f"{(sf / prev[1]) ** (1.0 / math.log2(L / prev[0])):.2f}"
        prev = (L, sf)
        print(f" {L:6d}    {sf * 100:9.3f}      {rat:>10s}          {n:9d}")
    lags = sorted(res)
    if len(lags) < 2:
        print("  insufficient lags")
        return None
    L0, L1 = lags[0], lags[-1]
    slope = math.log(res[L1] / res[L0]) / math.log(L1 / L0)
    shape = ("FLOOR (white)" if slope < 0.25 else
             "RANDOM WALK" if slope < 0.75 else
             "DRIFT / long-correlation" if slope >= 0.75 else "mixed")
    print(f"\n  log-log slope over {L0}..{L1} s = {slope:.3f}  =>  {shape}"
          f"   (0 = white, 0.5 = walk, 1 = drift)")
    fit = [L for L in lags if L >= a.fit_from]
    if len(fit) >= 2:
        q = (res[fit[-1]] ** 2 - res[fit[0]] ** 2) / (fit[-1] - fit[0])
        if q > 0:
            print(f"  walk-equivalent q over {fit[0]}..{fit[-1]} s: {q:.3e} m^2/s "
                  f"-> sqrt(q*30 s) = {math.sqrt(q * 30) * 1000:.2f} mm/30 s"
                  f"   [{q / 1e-10:.3g}x the armed sat_code_bias_q = 1e-10]")
            if slope > 0.75:
                print("  ^ NOT a walk: with slope ~1 this number is lag-dependent by "
                      "construction; quote it only as an ORDER, against the stated lags.")
    print(f"  per-epoch (30 s) scale: {res[L0] * 100 / max(1.0, L0 / 30.0):.3f} cm "
          f"equivalent; armed q gives sqrt(1e-10*30) = 0.055 mm")
    return res


def drift_table(series, a, tmin):
    """Per-satellite linear drift of the common-mode-removed series, by signal."""
    print("\n=== per-satellite LINEAR DRIFT of the common-removed series ===")
    print(f"{'const/signal':>13} {'nsat':>5} {'|drift| med':>12} {'p90':>9} "
          f"{'R^2 med':>8} {'resid med':>10}")
    print(f"{'':>13} {'':>5} {'[cm/30 s]':>12} {'[cm/30s]':>9} {'':>8} {'[cm]':>10}")
    for key in sorted(series):
        bysat = {s: {t: v for t, v in d.items() if t >= tmin}
                 for s, d in series[key].items()}
        bysat = {s: d for s, d in bysat.items() if len(d) > 50}
        if len(bysat) < a.min_sats:
            continue
        med = {}
        for t in sorted({t for d in bysat.values() for t in d}):
            vals = [d[t] for d in bysat.values() if t in d]
            if len(vals) >= a.min_sats:
                med[t] = statistics.median(vals)
        sl, r2s, res = [], [], []
        for d in bysat.values():
            pts = [(t, v - med[t]) for t, v in d.items() if t in med]
            if len(pts) < 50:
                continue
            n = len(pts)
            mt = sum(p[0] for p in pts) / n
            mv = sum(p[1] for p in pts) / n
            sxx = sum((p[0] - mt) ** 2 for p in pts)
            sxy = sum((p[0] - mt) * (p[1] - mv) for p in pts)
            syy = sum((p[1] - mv) ** 2 for p in pts)
            if sxx <= 0 or syy <= 0:
                continue
            b = sxy / sxx
            sl.append(abs(b) * 30.0 * 100.0)
            r2s.append(sxy * sxy / (sxx * syy))
            res.append(math.sqrt(max(0.0, (syy - b * sxy) / n)) * 100.0)
        if len(sl) < 3:
            continue
        sl.sort(); r2s.sort(); res.sort()
        n = len(sl)
        p90 = sl[min(n - 1, int(0.9 * n))]
        print(f"{key[0] + '/' + key[1]:>13} {n:5d} {sl[n // 2]:12.3f} {p90:9.3f} "
              f"{r2s[n // 2]:8.3f} {res[n // 2]:10.2f}")
    print("  R^2 is of a straight line through the common-removed series; the "
          "residual column is what WANDERS about that line.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+", help="run dir(s) containing products.txt, or the file")
    ap.add_argument("--lags", default="30,60,120,300,600,1200,2400,4800,9600,19200")
    ap.add_argument("--fit-from", type=float, default=600.0)
    ap.add_argument("--min-pairs", type=int, default=200)
    ap.add_argument("--min-sats", type=int, default=6,
                    help="minimum pair-common satellites for the gauge removal to be meaningful")
    ap.add_argument("--skip-hours", type=float, default=4.0,
                    help="drop the first N hours; the CB carry's measured step response is "
                         "tau ~ 3.8 h, so a 3 h run is ENTIRELY ignition")
    ap.add_argument("--center", choices=("median", "mean"), default="median")
    ap.add_argument("--by-signal", action="store_true")
    ap.add_argument("--pin", default="auto",
                    help="'auto' (S-record cbpin, else the selectIfPinPair rule), "
                         "'off', or an explicit 'C1C,C2W'")
    ap.add_argument("--drift", action="store_true")
    a = ap.parse_args()

    for run in a.runs:
        path, series, disc, cbpin = load(run)
        if not series:
            print(f"{path}: no B records", file=sys.stderr)
            continue
        t0 = min(t for k in series for s in series[k].values() for t in s)
        cut = t0 + a.skip_hours * 3600.0
        for k in list(series):
            for sat in list(series[k]):
                series[k][sat] = {t: v for t, v in series[k][sat].items() if t >= cut}
                if not series[k][sat]:
                    del series[k][sat]
            if not series[k]:
                del series[k]
        if not series:
            print(f"{path}: --skip-hours {a.skip_hours} left nothing", file=sys.stderr)
            continue
        tows = sorted({t for k in series for s in series[k].values() for t in s})
        print(f"\n{path}")
        print(f"  {len(tows)} epoch(s) {tows[0]:.0f}..{tows[-1]:.0f} "
              f"({(tows[-1] - tows[0]) / 3600.0:.2f} h after --skip-hours {a.skip_hours}), "
              f"{sum(len(v) for v in series.values())} (sat,signal) series, "
              f"{sum(len(v) for v in disc.values())} signalled break(s)")

        report([series[k] for k in series], disc, a, "ALL (const,signal) pooled")
        if a.by_signal:
            for k in sorted(series):
                report([series[k]], disc, a, f"{k[0]} / {k[1]}",
                       f"{len(series[k])} sats")

        # --- the IF-pin null control -------------------------------------
        if a.pin != "off":
            for const in sorted({k[0] for k in series}):
                sigs = [k[1] for k in series if k[0] == const]
                if a.pin != "auto":
                    pp = tuple(a.pin.split(","))
                elif const in cbpin:
                    pp = cbpin[const]
                else:
                    pp = pin_pair(const, sigs)
                if not pp or pp[0] not in sigs or pp[1] not in sigs:
                    print(f"\n=== {const}: IF-pin control unavailable "
                          f"(pair {pp} not emitted) ===")
                    continue
                f = FREQ.get(const, {})
                f1, f2 = f.get(pp[0][1]), f.get(pp[1][1])
                g1 = f1 * f1 / (f1 * f1 - f2 * f2)
                g2 = -f2 * f2 / (f1 * f1 - f2 * f2)
                A, B = series[(const, pp[0])], series[(const, pp[1])]
                ifc, gfc = {}, {}
                for sat in set(A) & set(B):
                    a_, b_ = A[sat], B[sat]
                    common = set(a_) & set(b_)
                    if common:
                        ifc[sat] = {t: g1 * a_[t] + g2 * b_[t] for t in common}
                        gfc[sat] = {t: a_[t] - b_[t] for t in common}
                src = "S-record cbpin" if const in cbpin and a.pin == "auto" else \
                      ("explicit" if a.pin != "auto" else "selectIfPinPair rule")
                report([ifc], disc, a,
                       f"{const}: IF{pp} — PINNED DIRECTION (null control)",
                       f"{src}; must read ~0")
                report([gfc], disc, a, f"{const}: GF{pp} — the DCB direction",
                       "iono-confounded")

        if a.drift:
            drift_table(series, a, cut)
    return 0


if __name__ == "__main__":
    sys.exit(main())
