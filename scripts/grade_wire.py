#!/usr/bin/env python3
"""Grade the emitted phase-bias wire, but on a datum-invariant quantity.

The producer writes the satellite phase-bias corrections it puts on the wire to
``<run>/logs/network_solution.csv`` (``kind`` = ``PB``, ``value_m`` = the emitted
bias in metres, per satellite per signal). The raw wire is datum-dependent: the
whole per-(constellation,signal) frame rides the chain datum's fractional
receiver phase bias, so every satellite in a constellation drifts together by a
few cycles per epoch. That common-mode drift is benign (it cancels in the
user's between-satellite double difference), yet a naive step grader on the raw
``value_m`` reports it as churn and mislabels a healthy frame as sick.

This grader applies the Psychas single-realization mandate: it pivot-differences
per (epoch, constellation, signal) first, subtracting the per-epoch median bias
across the satellites of that constellation+signal (the common-mode, or frame,
level), and then judges the epoch-to-epoch step of the residual. The residual
``b_s - median_s'(b_s')`` is a between-satellite (single-difference) quantity:
invariant to the common frame drift, but it still exposes a genuine per-satellite
re-level (an integer jump when the chain datum re-pins a satellite at a new
station). So the pivoted step isolates real re-levels from benign drift.

It reports the raw step alongside the pivoted step so the common-mode inflation
the pivot removes is visible directly (the "majors drift 2-4 cyc/ep" observation
collapses to about 0 after pivoting when the drift really is common-mode).

Companion to ``grade_ddfrac.py`` (which grades the internal cross-station DD).
This one grades the actual emitted product for every emitted satellite, not just
the DD-traced pairs: a complementary, wire-level instrument.

Epoch matching is tow-jitter and gap robust: samples are snapped to an inferred
epoch grid and a step is counted only between epoch-adjacent samples (never
across an arc gap).

Usage
-----
    python scripts/grade_wire.py <run_dir> [tail_seconds=3600] [--min-sats N=4]

``<run_dir>`` holds ``logs/network_solution.csv``. Cycle conversion uses a
built-in GNSS frequency table; a satellite/signal with no known wavelength is
reported in metres only. GLONASS (FDMA) is skipped for the cycle metric.
"""
import argparse
import csv
import os
import statistics as st
import sys
from collections import defaultdict

C_LIGHT = 299792458.0
SYS_NAME = {'G': 'GPS', 'E': 'Galileo', 'C': 'BeiDou', 'J': 'QZSS', 'S': 'SBAS', 'R': 'GLONASS'}

# Per-constellation carrier frequency (MHz) keyed by RINEX-3 band digit.  These
# are the standard nominal band centres; they reproduce the producer's own
# lambda_m in network_dd_fractional.csv (e.g. G/L1C -> 0.190294, E/L5Q -> 0.254828).
FREQ_MHZ = {
    'G': {'1': 1575.42, '2': 1227.60, '5': 1176.45},
    'E': {'1': 1575.42, '5': 1176.45, '6': 1278.75, '7': 1207.140, '8': 1191.795},
    'C': {'1': 1575.42, '2': 1561.098, '5': 1176.45, '6': 1268.52, '7': 1207.140, '8': 1191.795},
    'J': {'1': 1575.42, '2': 1227.60, '5': 1176.45, '6': 1278.75},
    'S': {'1': 1575.42, '5': 1176.45},
}


def band_of(signal):
    """RINEX-3 band digit from a signal code ('L1C' -> '1', '5Q' -> '5')."""
    if not signal:
        return ''
    if signal[0] in 'LCSDP' and len(signal) >= 2 and signal[1].isdigit():
        return signal[1]
    return signal[0] if signal[0].isdigit() else ''


def wavelength_m(cons, signal):
    f = FREQ_MHZ.get(cons, {}).get(band_of(signal))
    return C_LIGHT / (f * 1e6) if f else None


def infer_epoch_dt(tows):
    """Nominal epoch spacing = median gap between distinct sorted tows (jitter- and
    gap-robust; single-epoch spacing is the majority so its value is the median)."""
    u = sorted({round(t, 3) for t in tows})
    diffs = [b - a for a, b in zip(u, u[1:]) if b - a > 1e-6]
    return st.median(diffs) if diffs else 1.0


def load_pb(path):
    """Return rows [(tow, sat, cons, signal, value_m)] for kind==PB."""
    out = []
    with open(path, newline='') as f:
        rd = csv.DictReader(f)
        need = {'tow', 'sat', 'kind', 'signal', 'value_m'}
        if not need.issubset(set(rd.fieldnames or [])):
            sys.exit(f"{path} missing columns {need - set(rd.fieldnames or [])}; got {rd.fieldnames}")
        for r in rd:
            if r.get('kind') != 'PB':
                continue
            try:
                tow, val = float(r['tow']), float(r['value_m'])
            except (ValueError, TypeError):
                continue
            sat = r['sat']
            if not sat:
                continue
            out.append((tow, sat, sat[0], r['signal'], val))
    return out


def grade(rows, min_sats):
    """Return per-(cons,signal) {'raw':[steps_m], 'piv':[steps_m], 'lam':wavelen|None}.

    raw = |Δv_s| between epoch-adjacent samples of each satellite s (the raw,
          undifferenced wire step).
    piv = |Δv_s − common(e)|, where common(e) = median over the satellites present
          in both epochs of that same Δv_s: the per-(epoch,const,signal) common
          frame drift. Subtracting the common step is the between-satellite
          (single-difference) operation done on the step, so it is datum-invariant.
          Unlike differencing the absolute biases, which span hundreds of cycles
          and whose cross-sat median jumps as the satellite set changes, it is
          immune to the absolute bias level and to set composition. A benign
          common-mode drift then cancels (piv -> 0) while a genuine per-satellite
          re-level survives. Only transitions with >= min_sats paired satellites
          contribute to piv (else the common estimate is thin).
    """
    if not rows:
        return {}, 1.0
    dt0 = infer_epoch_dt(t for t, *_ in rows)
    tow0 = min(t for t, *_ in rows)
    eidx = lambda t: round((t - tow0) / dt0)

    # (cons,signal) -> sat -> {epoch: value}
    ser = defaultdict(lambda: defaultdict(dict))
    for tow, sat, cons, sig, val in rows:
        ser[(cons, sig)][sat][eidx(tow)] = val

    res = {}
    for (cons, sig), sats in ser.items():
        # bucket epoch-adjacent per-sat steps by the starting epoch e
        trans = defaultdict(list)             # e -> [(sat, Δv_s)]
        for sat, series in sats.items():
            es = sorted(series)
            for a, b in zip(es, es[1:]):
                if b - a == 1:                # epoch-adjacent only (skip gaps)
                    trans[a].append((sat, series[b] - series[a]))
        # Raw and pivot are pooled over the same transitions (>= min_sats paired
        # sats) so the reported "common-mode removed" ratio is a clean matched
        # before/after, not raw-over-all-transitions vs pivot-over-thick-ones
        # (thin arc-edge transitions carry the largest steps and would inflate raw).
        raw_steps, piv_steps = [], []
        for e, lst in trans.items():
            if len(lst) < min_sats:
                continue
            cm = st.median(dv for _, dv in lst)       # common frame step
            for _, dv in lst:
                raw_steps.append(abs(dv))
                piv_steps.append(abs(dv - cm))
        res[(cons, sig)] = {'raw': raw_steps, 'piv': piv_steps,
                            'lam': wavelength_m(cons, sig), 'nsat': len(sats)}
    return res, dt0


def _med(x):
    return st.median(x) if x else float('nan')


def _cyc(steps_m, lam):
    return [s / lam for s in steps_m] if lam else None


def main():
    # Diffed values can carry non-ASCII in labels; force UTF-8 so the Windows
    # cp1252 console never crashes on output.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run_dir')
    ap.add_argument('tail_seconds', nargs='?', type=float, default=3600.0)
    ap.add_argument('--min-sats', type=int, default=4,
                    help='min satellites in a (epoch,const,signal) group for its pivot to count')
    a = ap.parse_args()

    path = os.path.join(a.run_dir, 'logs', 'network_solution.csv')
    if not os.path.exists(path):
        sys.exit(f"not found: {path}")
    rows = load_pb(path)
    if not rows:
        sys.exit(f"no PB (phase-bias) rows in {path}")
    tmax = max(t for t, *_ in rows)
    rows = [r for r in rows if r[0] >= tmax - a.tail_seconds]

    res, dt0 = grade(rows, a.min_sats)
    print(f"Wire phase-bias step grade: {path}")
    print(f"tail={a.tail_seconds:.0f}s  dt0={dt0:.0f}s  min_sats={a.min_sats}  "
          f"(RAW=undifferenced wire; PIVOT=per-(const,signal) between-sat SD, datum-invariant)")

    pool_raw_cyc, pool_piv_cyc = [], []
    pool_raw_m, pool_piv_m = [], []
    for (cons, sig) in sorted(res):
        d = res[(cons, sig)]
        lam = d['lam']
        raw_c, piv_c = _cyc(d['raw'], lam), _cyc(d['piv'], lam)
        unit = 'cyc/ep' if lam else 'm/ep'
        raw_v = _med(raw_c) if lam else _med(d['raw'])
        piv_v = _med(piv_c) if lam else _med(d['piv'])
        ratio = (raw_v / piv_v) if (piv_v and piv_v == piv_v and piv_v > 0) else float('nan')
        print(f"\n[{SYS_NAME.get(cons, cons)} {sig}]  sats={d['nsat']}  "
              f"raw_steps={len(d['raw'])} piv_steps={len(d['piv'])}"
              + ('' if lam else '  [no λ — metres]'))
        print(f"  RAW   wire step med = {raw_v:.3f} {unit}   (includes benign common-mode)")
        print(f"  PIVOT wire step med = {piv_v:.3f} {unit}   (datum-invariant re-level)")
        print(f"  common-mode removed by pivot: {raw_v:.3f} -> {piv_v:.3f}  (ratio {ratio:.2f}x)")
        if lam:
            pool_raw_cyc += raw_c; pool_piv_cyc += piv_c
        pool_raw_m += d['raw']; pool_piv_m += d['piv']

    if pool_raw_cyc and pool_piv_cyc:
        rv, pv = _med(pool_raw_cyc), _med(pool_piv_cyc)
        ratio = f"  (ratio {rv / pv:.2f}x)" if pv else ""
        print(f"\nPOOLED (lambda-known)  RAW step med = {rv:.3f} cyc/ep   "
              f"PIVOT step med = {pv:.3f} cyc/ep{ratio}")
    print(f"POOLED (all)     RAW step med = {_med(pool_raw_m):.4f} m/ep   "
          f"PIVOT step med = {_med(pool_piv_m):.4f} m/ep")


if __name__ == '__main__':
    main()
