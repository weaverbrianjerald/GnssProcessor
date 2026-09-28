#!/usr/bin/env python3
"""Lane-ladder spectrometer: project the network's own float DDs onto the MW/EWL
lane ladder and grade them against the MW integers (docs/SPEC_lane_spectrometer.md §3).

Read-only instrument: it folds nothing and changes no estimate (increment S1).  A
fold-in test mixes the injected tension with the QC response (D5 flags, refloats), so
the mechanisms cannot be separated afterwards; a read-only projection gives the
attribution first, and a fold then confirms it.

Physics: why a ladder localizes the defect
------------------------------------------
Three error classes reach a float ambiguity estimate N_j (cycles):

  * dispersive (ionospheric): enters band j as  f1^2 * dI1 / (c * f_j)  cyc per metre
    of L1-equivalent DD slant delay, larger on the lower-frequency band;
  * non-dispersive metre-level (geometry / tropo / clock / orbit leakage): enters as
    dp / lambda_j = dp * f_j / c  cyc per metre, larger on the higher band;
  * per-signal cycle-structured (phase-bias / datum realization): arbitrary per
    signal, with no ladder structure at all.

A lane N_lane = N_hi - N_lo (f_hi > f_lo) compresses the first two by fixed, known
ratios while leaving the third untouched:

    lambda_lane = c / (f_hi - f_lo)
    d frac / d(DD iono I1) = f1^2/c * (1/f_hi - 1/f_lo)      [cyc/m]
    d frac / d(range-like) = (f_hi - f_lo)/c = 1/lambda_lane  [cyc/m]

so the extra-wide lanes (GPS L2/L5 lambda 5.861 m, Galileo E5b/E5a 9.768 m) are 18-40x
less sensitive to both than the per-band rungs.  The script's decisions follow from this:

  * the rung where fractions go from uniform to concentrated brackets the
    dispersive/metre-level error magnitude between that rung's threshold and the next;
  * uniform at every rung including EWL, while the measurement-side MW DD on the same
    channels is clean, identifies the cycle-structured per-signal class;
  * converting each rung's fraction to metres through that rung's own sensitivity turns
    the ladder into a hypothesis test: a single physical error gives the same metres on
    every rung, and a structureless per-signal error gives metres that fan out by the
    full sensitivity ratio of the rungs compared.  The observed ratio is judged against
    that null (~60x over lambda 0.19->19.5 m), not against a fixed threshold, since a
    fixed bar such as 2x can reject a hypothesis that explains most of the spread.

Limit: the ladder spans WL directions only.  The NL direction, where the user-visible
symptom lives, is orthogonal to every rung here.  Localization is by magnitude
bracketing plus class elimination; NL is not probed directly.

A between-station, between-satellite DD cancels the per-station and per-satellite gauge
freedoms, but not anything that varies per (station, satellite), above all the
slant-iono residual.  DD motion is therefore where an iono-representation error shows
up, which is why a network producer can churn its DDs at the cycle level while a
single-station user filter, whose satellite biases arrive fixed from a product,
converges to stable ambiguities.

Reference satellites: re-reference within a signal before joining
-----------------------------------------------------------------
`network_dd_fractional.csv` carries `ref_sat` per (constellation, signal), elected
independently per signal and per epoch.  So `dd_cyc` on L2W and on L5Q are referenced
to different satellites, and joining them directly is wrong.  This script re-references
within the signal first: inside one (epoch, cons, signal, sta_a, sta_b) group it forms
dd(sat_i) - dd(sat_j), where the reference term cancels exactly, and only then combines
across signals on common (sat_i, sat_j, sta_a, sta_b, epoch) keys.  The reference
satellite is carried as a virtual member with dd = 0 (the producer emits no row for it,
but dd(ref) is identically zero by construction); this is exact and is the largest
source of joinable pairs.  `--selftest` checks the cancellation, the join across
differing references, and that the naive join is wrong.

Arc means, not pooled samples (the R6 method)
---------------------------------------------
DD point estimates wander, so a pooled instantaneous fraction histogram measures churn,
not bias.  The graded statistic is the mean over a segmented window of >= --min-window
epoch-adjacent samples, cut at any epoch gap or any adjacent-epoch |delta| >
--split-cyc.  A rung whose median step already exceeds that threshold cannot form a
window at all: it is churn-gated (R6's third state) and gets no class verdict.  The
gate is per rung, because a single global gate would discard the EWL rungs, which are
the ones most likely to be gradeable.

Convention screens (never averaged into the main pool)
------------------------------------------------------
  * GPS lanes involving L2W / L2S are also graded on the half-cycle lattice (the
    semi-codeless half-cycle artifact is real).  Fractions are expressed in units of the
    lattice so the uniform benchmarks (0.25 / 0.20 / 0.0) are unchanged.
  * Galileo X-attribute families are reported per family (L6X/L7X sit 0.131 cyc
    off-integer as a known alignment offset, not a defect signal).

Usage
-----
    python scripts/lane_spectrometer.py <run_dir> [tail_seconds=3600] [options]

      --min-window N   epochs required to grade a window             (default 10)
      --split-cyc X    adjacent-epoch |delta| that cuts a window     (default 0.5)
      --mw / --no-mw   measurement-side MW DD from raw RINEX         (default: auto)
      --obs-glob G     RINEX glob for the MW side
      --mw-max-sta N   cap parsed RINEX stations                     (default 24)
      --min-n N        graded windows needed before a rung is judged (default 30)
      --selftest       run the algebra self-tests and exit
      --csv PATH       dump every graded window for external plotting

The MW side is on by default when a `data` directory exists under the working
directory.
"""
import cmath
import csv
import glob
import math
import os
import statistics as st
import sys
from collections import defaultdict

C = 299792458.0
F1 = 1575.42e6          # the L1 reference the iono sensitivity is expressed against

# Nominal per-band carrier frequencies, Hz.  GLONASS (FDMA) is absent by construction: its
# per-satellite frequency makes a between-satellite lane non-integer, and the producer's
# DD-fractional tracer already excludes it.
FREQ = {
    'G': {'1': 1575.42e6, '2': 1227.60e6, '5': 1176.45e6},
    'E': {'1': 1575.42e6, '5': 1176.45e6, '6': 1278.75e6, '7': 1207.14e6, '8': 1191.795e6},
    'C': {'1': 1575.42e6, '2': 1561.098e6, '5': 1176.45e6, '6': 1268.52e6,
          '7': 1207.14e6, '8': 1191.795e6},
}

# The SPEC §3.1 table rungs, marked '*' in the output so the ladder printed here (which
# enumerates every band pair the data actually carries) stays readable against the doc.
SPEC_RUNGS = {('G', '1'), ('G', '2'), ('G', '5'), ('E', '1'), ('E', '5'), ('E', '7'),
              ('G', '1', '2'), ('E', '1', '5'), ('G', '2', '5'), ('E', '7', '5')}

# A fraction distribution is effectively uniform once the underlying error exceeds ~0.4
# cycles of the rung (the SPEC table's "frac ~ uniform when |err| >~" column is this
# constant over the rung's sensitivity).  It is not a threshold on the data: it converts
# a rung into the error magnitude it can still see.
UNIFORMIZING_FRAC_CYC = 0.4

UNIFORM_MED, UNIFORM_P10, UNIFORM_R = 0.25, 0.20, 0.0


# ---------------------------------------------------------------------------
# Ladder physics
# ---------------------------------------------------------------------------
def band_props(sysc, band):
    """(lambda_m, iono_sens, range_sens) for a single band, cyc per metre."""
    f = FREQ[sysc][band]
    return C / f, F1 * F1 / (C * f), f / C


def lane_props(sysc, b_hi, b_lo):
    """(lambda_m, |iono_sens|, |range_sens|) for the lane N_hi - N_lo, cyc per metre.

    b_hi must be the higher-frequency band; the caller orders them.  The iono
    sensitivity is negative (the lower band absorbs more delay); the ladder reads the
    magnitude, so the magnitude is returned."""
    f_hi, f_lo = FREQ[sysc][b_hi], FREQ[sysc][b_lo]
    return (C / (f_hi - f_lo),
            abs(F1 * F1 / C * (1.0 / f_hi - 1.0 / f_lo)),
            abs((f_hi - f_lo) / C))


def rung_props(rung):
    """rung is ('G','2') for a per-band rung or ('G','2','5') for a lane."""
    return band_props(*rung) if len(rung) == 2 else lane_props(*rung)


def rung_label(rung):
    if len(rung) == 2:
        return '%s L%s' % (rung[0], rung[1])
    lam = rung_props(rung)[0]
    kind = 'EWL' if lam > 3.0 else ('WL' if lam > 0.5 else 'NL')
    return '%s %s(%s,%s)' % (rung[0], kind, rung[1], rung[2])


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------
def frac_stats(vals, lattice=1.0):
    """Fractional-lattice statistics of `vals` (cycles), reported in lattice units.

    Dividing by the lattice before taking the fraction means the uniform benchmarks
    (0.25 / 0.20 / 0) hold for the half-cycle screen too, so the two screens are
    directly comparable instead of needing separate baselines."""
    n = len(vals)
    if n == 0:
        return None
    f = [(v / lattice) - round(v / lattice) for v in vals]
    af = sorted(abs(x) for x in f)
    R = abs(sum(cmath.exp(2j * math.pi * x) for x in f)) / n
    return {
        'n': n,
        'mean': sum(af) / n,
        'med': af[n // 2],
        'p10': sum(1 for x in af if x < 0.10) / n,
        'p15': sum(1 for x in af if x < 0.15) / n,
        'R': R,
        'deciles': [af[min(n - 1, int(k * n / 10))] for k in range(1, 10)],
    }


def verdict(s, min_n, churn_gated=False):
    """Per-rung verdict: churn-gated / thin / UNIFORM / partial / CONCENTRATED.

    Conservative in both directions: `partial` is a real answer (the error sits near
    this rung's threshold), `thin` declines to classify a rung the run did not measure,
    and `churn-gated` declines to classify a rung where no window can form."""
    if churn_gated:
        return 'churn-gated'
    if s is None or s['n'] < min_n:
        return 'thin'
    if s['med'] > 0.225 and s['R'] < 0.10:
        return 'UNIFORM'
    if s['med'] < 0.15 or s['R'] > 0.30:
        return 'CONCENTRATED'
    return 'partial'


def explained(ratio, null):
    """How much of a structureless-error null a single-magnitude hypothesis explains.

    A per-signal error with no lane structure produces implied-metre columns that fan out
    by the rungs' sensitivity ratio (`null`).  A single physical error produces a ratio
    of 1.0.  The score 1 - log(observed)/log(null) places both hypotheses on one scale,
    so that 2.5x across a 60x null reads as ~78 % agreement rather than failing a fixed
    absolute bar."""
    if not (null > 1.0):
        return 0.0
    return max(0.0, 1.0 - math.log(max(ratio, 1.0 + 1e-9)) / math.log(null))


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------
def _f(x, default=None):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def infer_epoch_dt(tows):
    """Nominal epoch spacing = median gap between distinct sorted tows.

    Robust to the ~1e-5 s product-tow jitter and to multi-epoch gaps.  Under
    `trace_dd_fractional_epoch_stride` this is the strided spacing, which is what
    "epoch-adjacent" means for a strided trace."""
    u = sorted({round(t, 3) for t in tows})
    d = [b - a for a, b in zip(u, u[1:]) if b - a > 1e-6]
    return st.median(d) if d else 1.0


def load_ddfrac(run, tail_s, ctr):
    """-> (tail_rows, dt0, t0, tmax) with floats already parsed."""
    p = os.path.join(run, 'logs', 'network_dd_fractional.csv')
    if not os.path.exists(p):
        sys.exit('not found: %s\n(enable network.trace_dd_fractional and re-run)' % p)
    out = []
    with open(p) as fh:
        for r in csv.DictReader(fh):
            ctr['rows_read'] += 1
            tow, dd = _f(r.get('tow')), _f(r.get('dd_cyc'))
            lam = _f(r.get('lambda_m'), 0.0)
            # dd_cyc / frac_cyc are written as exactly 0 when lambda is unknown.  That is
            # an in-band sentinel, so a grader that does not filter it reports spurious
            # perfect integers.
            if tow is None or dd is None or not lam or lam <= 0.0:
                ctr['rows_unusable'] += 1
                continue
            out.append({'tow': tow, 'cons': r['cons'], 'signal': r['signal'],
                        'sat': r['sat_i'], 'ref': r['ref_sat'],
                        'a': r['sta_a'], 'b': r['sta_b'], 'dd': dd,
                        'sig': _f(r.get('dd_sigma_cyc'), -1.0)})
    if not out:
        sys.exit('no usable rows in %s' % p)
    tmax = max(r['tow'] for r in out)
    tail = [r for r in out if r['tow'] >= tmax - tail_s]
    ctr['rows_tail'] = len(tail)
    return tail, infer_epoch_dt(r['tow'] for r in tail), min(r['tow'] for r in tail), tmax


def load_anchors(run):
    """(cons,signal,sat) -> frozenset of stations that gauge-pin that freedom.

    Measured, and it changes what the SPEC §3.3 partition can mean: under the eq34 chain
    datum (the graded lineage's posture) the pins are spanning-tree edges, so a satellite
    is pinned at every station incident to it in the tree (up to 17 stations per
    (cons, signal, satellite) at 50 stations).  The map is one-to-many, so "the anchor
    station" is not well defined and the spec's binary split is generalized here.

    Two satellites are same-anchor on a signal if their pin sets intersect, and
    cross-anchor if the sets are disjoint.  That keeps the question the partition asks
    (does the datum realize these two satellites through a shared station or through
    unrelated ones?) and reduces to the spec's case: on the legacy single-pivot datum
    every set is {pivot}, every pair is same-anchor, and the partition clears the
    election by construction, as the SPEC says.

    No DD is ever `pinned_local`, because the DD-fractional trace skips pinned
    ambiguities: a station in a satellite's pin set never appears as that satellite's
    DD member on that signal.  Sets are unioned over the window (the sticky tree
    re-elects often, so an epoch-exact join would break the arcs up for no gain)."""
    p = os.path.join(run, 'logs', 'network_phase_anchor.csv')
    if not os.path.exists(p):
        return None
    seen = defaultdict(set)
    with open(p) as fh:
        for r in csv.DictReader(fh):
            seen[(r['cons'], r['signal'], r['sat'])].add(r['anchor_sta'])
    return {k: frozenset(v) for k, v in seen.items()}


def load_baselines(run):
    """station -> ECEF, for the baseline-bucket partition."""
    p = os.path.join(run, 'logs', 'network_stations.csv')
    if not os.path.exists(p):
        return None
    out = {}
    with open(p) as fh:
        for r in csv.DictReader(fh):
            x, y, z = _f(r.get('ecef_x')), _f(r.get('ecef_y')), _f(r.get('ecef_z'))
            if None not in (x, y, z):
                out[r['station']] = (x, y, z)
    return out or None


def baseline_bucket(ecef, a, b):
    if not ecef or a not in ecef or b not in ecef:
        return '?'
    d = math.dist(ecef[a], ecef[b]) / 1000.0
    return '<400' if d < 400 else ('400-1500' if d < 1500 else '>1500')


# ---------------------------------------------------------------------------
# Step 1: re-reference within a signal, then join across signals
# ---------------------------------------------------------------------------
def reref(rows, dt0, t0, ctr):
    """-> ({(epoch,cons,signal,a,b): {sat: dd_cyc}}, same-keyed sigma map).

    Only inside one of these groups does the per-(cons,signal) reference term cancel,
    which is why every later difference is taken here and never across signals."""
    grp = defaultdict(dict)
    sig = defaultdict(dict)
    refs = defaultdict(set)
    for r in rows:
        e = round((r['tow'] - t0) / dt0)
        k = (e, r['cons'], r['signal'], r['a'], r['b'])
        grp[k][r['sat']] = r['dd']
        # dd_sigma_cyc is -1 when the joint conditional covariance was unavailable that
        # epoch (another in-band sentinel); it is not propagated as a variance.
        if r['sig'] is not None and r['sig'] > 0.0:
            sig[k][r['sat']] = r['sig']
        refs[k].add(r['ref'])
    for k, rs in refs.items():
        if len(rs) != 1:
            # Two references inside one group would break the cancellation.  The producer
            # elects one per (cons,signal) per epoch, so this should stay empty; it is
            # checked rather than assumed because an unnoticed violation would corrupt
            # every difference formed in the group.  Such groups are dropped and counted.
            ctr['dropped_ref_mismatch'] += 1
            grp.pop(k, None)
            continue
        ref = next(iter(rs))
        grp[k].setdefault(ref, 0.0)   # the reference is a member, at dd == 0
        sig[k].setdefault(ref, 0.0)   # ... and carries no formal sigma of its own
    ctr['reref_groups'] = len(grp)
    return grp, sig


def pair_series(grp, sig, ctr):
    """-> ({key: {epoch: ddr_cyc}}, {key: indicative_sigma_cyc}) for sat_i < sat_j.

    ddr = dd(sat_i) - dd(sat_j): the between-station, between-satellite DD on one signal
    with the reference satellite algebraically removed.  The sigma is indicative only:
    the trace carries each DD's own formal sigma but not the cross-covariance between two
    DDs sharing a reference, so sqrt(sA^2+sB^2) over-counts the shared term."""
    out = defaultdict(dict)
    sigs = defaultdict(list)
    for (e, cons, s, a, b), sats in grp.items():
        names = sorted(sats)
        sg = sig.get((e, cons, s, a, b), {})
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                si, sj = names[i], names[j]
                key = (cons, s, si, sj, a, b)
                out[key][e] = sats[si] - sats[sj]
                if si in sg and sj in sg:
                    sigs[key].append(math.hypot(sg[si], sg[sj]))
                ctr['pairs_rereferenced'] += 1
    return out, {k: st.median(v) for k, v in sigs.items() if v}


def lane_series(ps, ps_sig, ctr, extra_rungs=True):
    """-> ({rung: {key: {epoch: cyc}}}, {rung: {key: indicative_sigma}}).

    Per-band rungs are the re-referenced DDs as they stand; lane rungs are the
    cross-signal difference N_hi - N_lo on common (sat pair, station pair, epoch) keys.
    `family` in the key is the tracking-attribute pair (the Galileo X-family screen)."""
    per_band = defaultdict(dict)
    for key, series in ps.items():
        cons, sig = key[0], key[1]
        if len(sig) < 2 or sig[0] != 'L' or sig[1] not in FREQ.get(cons, {}):
            ctr['dropped_unknown_band'] += 1
            continue
        per_band[(cons, sig[1])][key] = series

    rungs = defaultdict(dict)
    rung_sig = defaultdict(dict)
    for (cons, band), keyed in per_band.items():
        rung = (cons, band)
        for key, series in keyed.items():
            _, sig, si, sj, a, b = key
            nk = (cons, sig[2:] or '-', sig, '', si, sj, a, b)
            rungs[rung][nk] = series
            if key in ps_sig:
                rung_sig[rung][nk] = ps_sig[key]

    bands = defaultdict(set)
    for (cons, band) in per_band:
        bands[cons].add(band)
    for cons, bs in bands.items():
        for b_hi in sorted(bs):
            for b_lo in sorted(bs):
                if b_hi == b_lo or FREQ[cons][b_hi] <= FREQ[cons][b_lo]:
                    continue
                rung = (cons, b_hi, b_lo)
                if not extra_rungs and rung not in SPEC_RUNGS:
                    continue
                hi_keyed, lo_keyed = per_band[(cons, b_hi)], per_band[(cons, b_lo)]
                # Index the low side by the geometry key so the join is a lookup, not a
                # quadratic scan: the same (sat pair, station pair) may carry several
                # tracking channels on each band.
                lo_by_geom = defaultdict(list)
                for key, series in lo_keyed.items():
                    lo_by_geom[key[2:]].append((key[1], series, key))
                for hkey, hser in hi_keyed.items():
                    geom = hkey[2:]
                    for lsig, lser, lkey in lo_by_geom.get(geom, ()):
                        hsig = hkey[1]
                        common = hser.keys() & lser.keys()
                        if not common:
                            continue
                        fam = (hsig[2:] or '-') + '/' + (lsig[2:] or '-')
                        nk = (cons, fam, hsig, lsig) + geom
                        rungs[rung][nk] = {e: hser[e] - lser[e] for e in common}
                        hk_full = (cons, hsig) + geom
                        if hk_full in ps_sig and lkey in ps_sig:
                            rung_sig[rung][nk] = math.hypot(ps_sig[hk_full], ps_sig[lkey])
                        ctr['lane_keys_joined'] += 1
                        ctr['lane_samples_joined'] += len(rungs[rung][nk])
    return rungs, rung_sig


# ---------------------------------------------------------------------------
# Step 2: segment into windows and grade the arc means
# ---------------------------------------------------------------------------
def windows(series, min_window, split_cyc):
    """-> maximal epoch-adjacent runs with no jump.

    Two cutting rules, both applied: an epoch index gap (the trace is strided and the
    producer skips epochs, so consecutive samples are not consecutive epochs) and an
    adjacent-epoch |delta| over `split_cyc` (a refloat or a network re-level moves a DD
    by O(1) cycle; averaging across it produces a fraction that never existed)."""
    out, cur = [], []
    for e in sorted(series):
        v = series[e]
        if cur and (e - cur[-1][0] != 1 or abs(v - cur[-1][1]) > split_cyc):
            out.append(cur)
            cur = []
        cur.append((e, v))
    if cur:
        out.append(cur)
    return out


def step_stats(keyed):
    """Adjacent-epoch |delta| over every series of one rung (the churn read).

    Gradeability evidence and a ladder measurement in its own right: if a rung's median
    step already exceeds the window-splitting threshold, no window can form and that
    rung is churn-gated (SPEC §3.4's last row, per rung)."""
    steps = []
    for series in keyed.values():
        es = sorted(series)
        steps += [abs(series[b] - series[a]) for a, b in zip(es, es[1:]) if b - a == 1]
    if not steps:
        return None
    steps.sort()
    n = len(steps)
    return {'n': n, 'med': steps[n // 2], 'p90': steps[int(.9 * n)], 'all': steps}


def grade(rungs, min_window, split_cyc, ctr):
    """-> ({rung: [record]}, {rung: step_stats}): arc means plus the churn read."""
    graded = defaultdict(list)
    steps = {}
    wlen = []
    for rung, keyed in rungs.items():
        steps[rung] = step_stats(keyed)
        for key, series in keyed.items():
            cons, fam, s_hi, s_lo, si, sj, a, b = key
            for w in windows(series, min_window, split_cyc):
                wlen.append(len(w))
                if len(w) < min_window:
                    ctr['windows_too_short'] += 1
                    continue
                vals = [v for _, v in w]
                ctr['windows_graded'] += 1
                graded[rung].append({
                    'cons': cons, 'fam': fam, 's_hi': s_hi, 's_lo': s_lo,
                    'sat_i': si, 'sat_j': sj, 'a': a, 'b': b,
                    'n': len(w), 'e0': w[0][0], 'e1': w[-1][0],
                    'mean': sum(vals) / len(vals),
                    'sd': st.pstdev(vals) if len(vals) > 1 else 0.0,
                    'pooled': vals,
                })
    ctr['window_lengths'] = wlen
    return graded, steps


# ---------------------------------------------------------------------------
# Step 3: measurement side, DD of the raw MW statistic (the trusted reference)
# ---------------------------------------------------------------------------
def mw_reference(obs_glob, sod_lo, sod_hi, want_sta, max_sta, log=print):
    """-> {(cons, sig_hi, sig_lo, station, sat): {sod: mw_cyc}} from raw RINEX.

    Receiver and satellite MW biases cancel exactly in the between-station DD of the same
    satellite pair, so the DD is a pure integer x lambda_lane against decimetre averaged
    noise: ground truth independent of the estimator.  Parsing is delegated to the census
    parser (mw_census_allpairs.parse_station_tow) so there is one RINEX MW implementation
    in the tree."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        from mw_census_allpairs import parse_station_tow
    except ImportError as exc:                      # pragma: no cover - import guard
        log('  MW side unavailable: %s' % exc)
        return {}
    paths = sorted(glob.glob(obs_glob))
    if want_sta:
        paths = [p for p in paths
                 if os.path.basename(os.path.dirname(p)).upper() in want_sta]
    if max_sta and len(paths) > max_sta:
        paths = paths[:max_sta]
    out = {}
    for p in paths:
        name = os.path.basename(os.path.dirname(p)).upper()
        try:
            ser = parse_station_tow(p, sod_lo, sod_hi)
        except Exception as exc:                     # pragma: no cover - parser guard
            log('  MW parse failed for %s: %s' % (name, exc))
            continue
        for (sat, (sysc, pair)), samples in ser.items():
            hi, lo = pair.split('/')                # 'L7Q/L5Q', higher frequency first
            out[(sysc, hi, lo, name, sat)] = dict(samples)
    log('  MW side: %d station file(s) parsed, %d (channel,station,sat) series'
        % (len(paths), len(out)))
    return out


def mw_dd(mw, cons, s_hi, s_lo, si, sj, a, b, sods):
    """DD of the MW statistic over the same channels/geometry/epochs -> [cyc].

    The four legs must share an epoch or it is not a DD: receiver clock and satellite
    bias only cancel between simultaneous samples."""
    ka = mw.get((cons, s_hi, s_lo, a, si)), mw.get((cons, s_hi, s_lo, a, sj))
    kb = mw.get((cons, s_hi, s_lo, b, si)), mw.get((cons, s_hi, s_lo, b, sj))
    if any(x is None for x in ka + kb):
        return []
    out = []
    for s in sods:
        try:
            out.append((ka[0][s] - ka[1][s]) - (kb[0][s] - kb[1][s]))
        except KeyError:
            continue
    return out


def _mad(vals, centre):
    a = sorted(abs(v - centre) for v in vals)
    return 1.4826 * a[len(a) // 2] if a else float('nan')


def report_mw(rungs, mw, dt0, t0, min_ep, ctr):
    """State-vs-MW per-arc bias: the unwrapped error gauge, which needs no windows.

    This instrument still works on a churn-dominated producer.  The arc-mean method
    needs >= min_window epoch-adjacent samples, which a DD wandering cycles per epoch
    never supplies.  Here the MW statistic supplies a per-epoch truth, so each epoch is
    its own comparison:  d(e) = state_lane(e) - MW_lane(e).  That decomposes into a
    per-arc constant (the model bias) plus epoch-to-epoch motion (the churn), so the
    median of d over an arc is a bias estimate robust to the churn that breaks up the
    windows, and the scaled MAD of d measures the churn on the same arc.  Neither needs
    contiguity.

    The disagreement then splits into a whole-lane-cycle part and a sub-cycle part, and
    they are different findings.  The integer part says the state DD is not on the same
    lattice member as the truth (SPEC §3.4's "instrument first" row), which is the
    symptom.  Only the sub-cycle part is a model-error magnitude that converts to
    metres, which is why the class test below uses it."""
    summary = []
    for rung in sorted(rungs, key=lambda r: rung_props(r)[0]):
        if len(rung) != 3:
            continue            # the MW statistic is a lane; a single band has none
        lam, si, sr = rung_props(rung)
        bias, churn, mwfrac, nep = [], [], [], []
        attempted = 0
        for key, series in rungs[rung].items():
            cons, fam, s_hi, s_lo, si_, sj_, a, b = key
            attempted += 1
            pairs = []
            for e in sorted(series):
                v = mw_dd(mw, cons, s_hi, s_lo, si_, sj_, a, b,
                          [((e * dt0 + t0) % 86400.0)])
                if v:
                    pairs.append((series[e], v[0]))
            if len(pairs) < min_ep:
                continue
            d = [s - m for s, m in pairs]
            med = sorted(d)[len(d) // 2]
            bias.append(med)
            churn.append(_mad(d, med))
            mwfrac += [m - round(m) for _, m in pairs]
            nep.append(len(pairs))
        ctr['mw_arcs_attempted'] += attempted
        ctr['mw_arcs_coregistered'] += len(bias)
        if not bias:
            print(' %-14s no co-registered MW arcs (of %d state arcs) - channel-pair '
                  'join loss' % (rung_label(rung), attempted))
            continue
        n = len(bias)
        whole = sorted(abs(round(x)) for x in bias)
        fr = sorted(abs(x - round(x)) for x in bias)
        ch = sorted(churn)
        print(' %-14s arcs %d of %d state arcs co-registered  (median %d paired epochs)'
              % (rung_label(rung), n, attempted, sorted(nep)[len(nep) // 2]))
        print(fmt_stats('  MW DD itself (truth)', frac_stats(mwfrac),
                        'circR -> 1 means the MW integer is trustworthy'))
        print('    per-arc bias, WHOLE-cycle part : med %.0f  p90 %.0f  max %.0f cyc '
              '(%.1f%% of arcs non-zero)'
              % (whole[n // 2], whole[int(.9 * n)], whole[-1],
                 100.0 * sum(1 for x in whole if x) / n))
        print('    per-arc bias, SUB-cycle part   : med %.3f  p90 %.3f cyc  '
              '=> %.3f m dispersive / %.3f m range-like'
              % (fr[n // 2], fr[int(.9 * n)], fr[n // 2] / si, fr[n // 2] / sr))
        print('    per-arc churn (1.4826*MAD of d): med %.3f  p90 %.3f cyc  '
              '=> %.3f m dispersive / %.3f m range-like'
              % (ch[n // 2], ch[int(.9 * n)], ch[n // 2] / si, ch[n // 2] / sr))
        if lam < 3.0:
            print('    NOTE WL rung: POPULATION statistics only - the measured 2-4 % '
                  'systematic wrong-by-one floor plateaus near 0.96 (not 1/sqrt(n)) and '
                  'is invisible to variance gates.  Never read this per arc.')
        summary.append((rung, n, whole[n // 2], fr[n // 2], ch[n // 2], si, sr))

    if len(summary) >= 3:
        print('\n  -- CROSS-RUNG CONSISTENCY (the class test) --')
        print('   A single physical DISPERSIVE delay gives the SAME metres in disp_m on')
        print('   every rung; a single non-dispersive metre-level error gives the same in')
        print('   rng_m.  A per-signal CYCLE-STRUCTURED error matches NEITHER: its metres')
        print('   fan out by the full SENSITIVITY ratio of the rungs, which is the null')
        print('   each hypothesis below is scored against.')
        print('  %-14s %6s %8s %9s %9s %9s %9s %9s %9s' %
              ('rung', 'arcs', 'whole', 'frac_cyc', 'f_disp_m', 'f_rng_m',
               'churn_cyc', 'c_disp_m', 'c_rng_m'))
        for rung, n, wc, fc, cc, si, sr in summary:
            print('  %-14s %6d %8.0f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f%s'
                  % (rung_label(rung), n, wc, fc, fc / si, fc / sr, cc, cc / si, cc / sr,
                     '  UNIF' if fc >= 0.225 else ''))
        if any(s[3] >= 0.225 for s in summary):
            print('   UNIF = that rung\'s sub-cycle bias sits AT the uniform value (0.25 '
                  'cyc), so its implied metres are a FLOOR, not a measurement: the rung '
                  'is saturated and its fan-out is not evidence of a class.')
        nd = max(s[5] for s in summary) / min(s[5] for s in summary)
        nr = max(s[6] for s in summary) / min(s[6] for s in summary)
        for name, col in (('SUB-CYCLE BIAS', 3), ('CHURN', 4)):
            d = [s[col] / s[5] for s in summary]
            r = [s[col] / s[6] for s in summary]
            rd = max(d) / min(d) if min(d) > 0 else float('inf')
            rr = max(r) / min(r) if min(r) > 0 else float('inf')
            ed, er = explained(rd, nd), explained(rr, nr)
            print('  %s: dispersive %.3f-%.3f m (%.2fx of a %.0fx null => explains '
                  '%.0f%%)   range-like %.3f-%.3f m (%.2fx of %.0fx => %.0f%%)'
                  % (name, min(d), max(d), rd, nd, 100 * ed,
                     min(r), max(r), rr, nr, 100 * er))
            best, be = ('dispersive', ed) if ed >= er else ('range-like', er)
            vals = d if best == 'dispersive' else r
            if be >= 0.7:
                print('    => predominantly ONE %s error of ~%.2f m (explains %.0f%% of '
                      'the null): real lane structure, NOT per-signal cycle structure.'
                      % (best, st.median(vals), 100 * be))
            elif be >= 0.4:
                print('    => PARTIAL %s structure (%.0f%%): a real physical component '
                      'plus something without lane structure.' % (best, 100 * be))
            else:
                print('    => no lane structure (best explains %.0f%%): the per-signal '
                      'cycle-structured signature.' % (100 * be))
        wmax = max(s[2] for s in summary)
        if wmax >= 1:
            print('  SPEC 3.4 ROW 5 APPLIES - the state-vs-MW offset carries WHOLE lane '
                  'cycles (median up to %.0f): read that as INSTRUMENT-FIRST (channel '
                  'identity, half-cycle, ref_sat join) and then as the A2 integer-datum '
                  'lump, BEFORE any physics claim from the sub-cycle columns.' % wmax)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def fmt_stats(tag, s, extra=''):
    if s is None:
        return '  %-26s          (no samples)' % tag
    return ('  %-26s n=%6d  med|f|=%.3f  mean|f|=%.3f  P(<.1)=%.3f  circR=%.3f  %s'
            % (tag, s['n'], s['med'], s['mean'], s['p10'], s['R'], extra))


def print_ladder(rungs_present):
    print('\n== SENSITIVITY LADDER (computed from the nominal frequencies) ==')
    print('  %-14s %9s %11s %11s %10s %10s' %
          ('rung', 'lambda_m', 'iono cyc/m', 'rng cyc/m', 'iono err_m', 'rng err_m'))
    for rung in sorted(rungs_present, key=lambda r: rung_props(r)[0]):
        lam, si, sr = rung_props(rung)
        print('  %-14s %9.3f %11.3f %11.3f %10.3f %10.3f %s' %
              (rung_label(rung), lam, si, sr,
               UNIFORMIZING_FRAC_CYC / si, UNIFORMIZING_FRAC_CYC / sr,
               '*' if rung in SPEC_RUNGS else ''))
    print('  err_m = %.1f cyc / sensitivity: the error magnitude at which that rung\'s'
          % UNIFORMIZING_FRAC_CYC)
    print('  fractions go effectively uniform.  * = a rung of the SPEC 3.1 table.')


def print_churn(steps, split_cyc):
    print('\n== CHURN (adjacent-epoch |delta| per rung) ==')
    print('   a rung whose MEDIAN step exceeds the %.2f cyc split threshold cannot form '
          'a window at' % split_cyc)
    print('   all: that is R6\'s third state (churn-gated), PER RUNG, not a clean result.')
    print('  %-14s %7s %9s %9s %10s %9s %9s' %
          ('rung', 'n', 'med_cyc', 'p90_cyc', 'frac>split', 'disp_m', 'rng_m'))
    pool = []
    for rung in sorted(steps, key=lambda r: rung_props(r)[0]):
        s = steps[rung]
        if not s:
            continue
        lam, si, sr = rung_props(rung)
        over = sum(1 for x in s['all'] if x > split_cyc) / s['n']
        print('  %-14s %7d %9.3f %9.3f %10.3f %9.3f %9.3f%s' %
              (rung_label(rung), s['n'], s['med'], s['p90'], over,
               s['med'] / si, s['med'] / sr,
               '   <-- CHURN-GATED' if s['med'] > split_cyc else ''))
        if s['n'] >= 100:
            pool.append((rung, s['med'] / si, s['med'] / sr))
    # The churn is itself a ladder measurement and needs no MW side.  It is scored
    # against the structureless null, in the same way as the MW class test.
    for tag, sub in (('SPEC-table rungs', [x for x in pool if x[0] in SPEC_RUNGS]),
                     ('all well-sampled', pool)):
        if len(sub) < 3:
            continue
        d = [x[1] for x in sub]
        r = [x[2] for x in sub]
        sd = [rung_props(x[0])[1] for x in sub]
        srr = [rung_props(x[0])[2] for x in sub]
        rd = max(d) / min(d) if min(d) > 0 else float('inf')
        rr = max(r) / min(r) if min(r) > 0 else float('inf')
        nd, nr = max(sd) / min(sd), max(srr) / min(srr)
        ed, er = explained(rd, nd), explained(rr, nr)
        print('  CHURN as ONE error, %-16s [%d rungs]:' % (tag, len(sub)))
        print('    dispersive %.3f-%.3f m  ratio %.2fx of a %.0fx null => explains %.0f%%'
              % (min(d), max(d), rd, nd, 100 * ed))
        print('    range-like %.3f-%.3f m  ratio %.2fx of a %.0fx null => explains %.0f%%'
              % (min(r), max(r), rr, nr, 100 * er))
        best, be = ('dispersive', ed) if ed >= er else ('range-like', er)
        vals = d if best == 'dispersive' else r
        if be >= 0.7:
            print('    => the epoch-to-epoch churn IS predominantly ONE %s error of '
                  '~%.2f m: it scales down the ladder as physics predicts (%.0f%% of the '
                  'null), so it is NOT per-signal cycle structure.'
                  % (best, st.median(vals), 100 * be))
        elif be >= 0.4:
            print('    => PARTIAL %s structure (%.0f%%): a real physical component plus '
                  'something without lane structure.' % (best, 100 * be))
        else:
            print('    => no lane structure (best explains %.0f%%): the per-signal '
                  'cycle-structured signature.' % (100 * be))


def half_cycle_screened(rec, cons):
    """GPS L2W / L2S participation -> the half-cycle lattice screen applies."""
    return cons == 'G' and any(s in ('L2W', 'L2S') for s in (rec['s_hi'], rec['s_lo']))


def anchor_class(anchors, cons, rec):
    """same / cross / unknown for one graded DD record (pin-set intersection).

    A lane record spans two signals and therefore two pin-set pairs; it counts as `same`
    only when both signals share a pinning station between the two satellites, because a
    disjoint realization on either signal contaminates the lane."""
    got = []
    for s in [x for x in (rec['s_hi'], rec['s_lo']) if x]:
        ai = anchors.get((cons, s, rec['sat_i']))
        aj = anchors.get((cons, s, rec['sat_j']))
        if ai is None or aj is None:
            return 'unknown'
        got.append(bool(ai & aj))
    return 'same' if all(got) else 'cross'


def print_matrix_read(rv, min_n, steps, split_cyc, ctr):
    """Map the per-rung verdicts onto a SPEC 3.4 row.  No row => not a verdict."""
    gradeable = [v for v in rv.values() if v not in ('churn-gated', 'thin')]
    live = [s for s in steps.values() if s]
    if live and not gradeable:
        med_all = st.median([s['med'] for s in live])
        gf = ctr['windows_graded'] / max(1, ctr['windows_graded']
                                        + ctr['windows_too_short'])
        print('  ROW 6 (LAST ROW) - CHURN-GATED THROUGHOUT: median adjacent-epoch DD '
              'step %.3f cyc vs a %.2f cyc split threshold; only %.2f%% of windows '
              'reached the grading minimum, and NO rung class survived.'
              % (med_all, split_cyc, 100 * gf))
        print('  Consequence per SPEC 3.4: D5/refloat load remains the gate and the '
              'window-length distribution IS the evidence.  DO NOT force a defect-class '
              'verdict - the pooled fractions measure churn, not bias.')
        return

    def cls(pred):
        vs = [v for r, v in rv.items() if pred(r) and v not in ('thin', 'churn-gated')]
        if not vs:
            return None
        if all(v == 'UNIFORM' for v in vs):
            return 'UNIFORM'
        if all(v == 'CONCENTRATED' for v in vs):
            return 'CONCENTRATED'
        return 'MIXED'

    def gated(pred):
        g = [r for r, v in rv.items() if pred(r) and v == 'churn-gated']
        return ('  [%d CHURN-GATED: %s]' % (len(g), ', '.join(rung_label(r) for r in g))
                if g else '')

    def is_band(r):
        return len(r) == 2

    def is_wl(r):
        return len(r) == 3 and 0.5 < rung_props(r)[0] <= 3.0

    def is_ewl(r):
        return len(r) == 3 and rung_props(r)[0] > 3.0

    band, wl, ewl = cls(is_band), cls(is_wl), cls(is_ewl)
    print('  per-band rungs : %s%s' % (band, gated(is_band)))
    print('  WL rungs       : %s%s' % (wl, gated(is_wl)))
    print('  EWL rungs      : %s%s' % (ewl, gated(is_ewl)))

    if band == 'UNIFORM' and wl == 'CONCENTRATED':
        row = ('ROW 1 - dispersive error in the 0.06-0.27 m window: IONO REPRESENTATION '
               'CONFIRMED DOMINANT.  Magnitude gauge = frac_WL/1.49 m.  Next purchase: '
               'Iono-2\'s owed >=3 h A/B, with this grader as its acceptance instrument.')
    elif ewl in ('CONCENTRATED', 'MIXED') and band != 'CONCENTRATED' \
            and wl != 'CONCENTRATED':
        row = ('ROW 2 - the per-band and WL rungs are uniform-or-ungradeable while the '
               'EWL rungs concentrate => dispersive error at SH-FIELD scale.  Read the '
               'bracket off the two rungs either side of the transition, and cross-check '
               'with the free-iono discriminator before building.')
    elif ewl == 'UNIFORM' and 'CONCENTRATED' not in (band, wl):
        row = ('ROW 3 - ALL rungs uniform including EWL => CYCLE-STRUCTURED PER-SIGNAL '
               'DEFECT (given a clean measurement-side MW DD).  Read the anchor '
               'partition: cross >> same convicts the anchor-election datum split; '
               'otherwise escalate to the eq34/datum card.')
    elif None in (band, wl, ewl):
        row = ('PARTIAL ROW - a rung class was not measured.  State the row for the '
               'classes that WERE measured and name the gated ones; do not force a '
               'verdict on the missing class.')
    else:
        row = ('NO CLEAN ROW - the rung classes disagree without matching a matrix '
               'pattern.  Report the per-rung table and treat the bracketing as partial.')
    print('  ' + row)
    print('  (A run without a matrix row is not a verdict - SPEC 9.3.  Attach the '
          'window-length distribution.)')


def dump_csv(path, graded):
    with open(path, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['rung', 'cons', 'family', 'sig_hi', 'sig_lo', 'sat_i', 'sat_j',
                    'sta_a', 'sta_b', 'n_ep', 'e0', 'e1', 'arc_mean_cyc', 'arc_sd_cyc',
                    'frac_cyc'])
        for rung, recs in graded.items():
            for r in recs:
                w.writerow([rung_label(rung), r['cons'], r['fam'], r['s_hi'], r['s_lo'],
                            r['sat_i'], r['sat_j'], r['a'], r['b'], r['n'],
                            r['e0'], r['e1'], '%.6f' % r['mean'], '%.6f' % r['sd'],
                            '%.6f' % (r['mean'] - round(r['mean']))])


# ---------------------------------------------------------------------------
def main(argv):
    if '--selftest' in argv:
        return selftest()
    if len(argv) < 2:
        sys.exit('usage: python scripts/lane_spectrometer.py <run_dir> [tail_seconds] '
                 '[options]   (--selftest for the algebra checks)')
    run = argv[1]
    pos = [a for a in argv[2:] if not a.startswith('--')]
    tail_s = float(pos[0]) if pos else 3600.0

    def opt(name, default, cast=float):
        return cast(argv[argv.index(name) + 1]) if name in argv else default

    min_window = int(opt('--min-window', 10, int))
    split_cyc = opt('--split-cyc', 0.5)
    min_n = int(opt('--min-n', 30, int))
    mw_max_sta = int(opt('--mw-max-sta', 24, int))
    csv_out = opt('--csv', None, str)

    ctr = defaultdict(int)
    rows, dt0, t0, tmax = load_ddfrac(run, tail_s, ctr)
    anchors = load_anchors(run)
    ecef = load_baselines(run)

    print('== LANE-LADDER SPECTROMETER (SPEC_lane_spectrometer 3) ==')
    print('run            : %s' % run)
    print('tail window    : %.0f s ending tow %.1f  (dt0 %.0f s, %d graded epochs)'
          % (tail_s, tmax, dt0, len({round((r['tow'] - t0) / dt0) for r in rows})))
    print('arc grading    : min_window %d epochs, split at |delta| > %.2f cyc'
          % (min_window, split_cyc))
    if anchors:
        pn = sorted(len(v) for v in anchors.values())
        print('anchor trace   : %d (cons,signal,sat) freedoms, pin-set size med %d max %d'
              '  (>1 => the eq34 chain datum pins TREE EDGES, so the map is one-to-many '
              'and the partition uses pin-set intersection)'
              % (len(pn), pn[len(pn) // 2], pn[-1]))
    else:
        print('anchor trace   : ABSENT (anchor partition skipped - arm '
              'network.trace_phase_anchor)')
    print('station ECEF   : %s' % ('%d stations' % len(ecef) if ecef else 'ABSENT'))

    grp, grp_sig = reref(rows, dt0, t0, ctr)
    ps, ps_sig = pair_series(grp, grp_sig, ctr)
    rungs, rung_sig = lane_series(ps, ps_sig, ctr)
    graded, steps = grade(rungs, min_window, split_cyc, ctr)

    print_ladder(set(rungs))
    print_churn(steps, split_cyc)

    print('\n== PER-RUNG STATISTICS (ARC MEANS - the graded statistic) ==')
    print('   uniform benchmark: med|f| %.3f  P(<.1) %.3f  circR %.3f'
          % (UNIFORM_MED, UNIFORM_P10, UNIFORM_R))
    rung_verdict = {}
    for rung in sorted(graded, key=lambda r: rung_props(r)[0]):
        recs = graded[rung]
        main_pool = [r for r in recs if not half_cycle_screened(r, rung[0])]
        screened = [r for r in recs if half_cycle_screened(r, rung[0])]
        ctr['dropped_screen'] += len(screened)
        s = frac_stats([r['mean'] for r in main_pool])
        pooled = frac_stats([v for r in main_pool for v in r['pooled']])
        cg = bool(steps.get(rung) and steps[rung]['med'] > split_cyc)
        v = verdict(s, min_n, cg)
        rung_verdict[rung] = v
        lam, si, sr = rung_props(rung)
        print('\n %-14s lambda %8.3f m   VERDICT: %s' % (rung_label(rung), lam, v))
        print(fmt_stats('arc means', s, 'windows=%d' % len(main_pool)))
        print(fmt_stats('pooled instantaneous', pooled, '(churn, not bias)'))
        if s:
            print('    deciles |f| : ' + ' '.join('%.3f' % d for d in s['deciles']))
            keys = [(r['cons'], r['fam'], r['s_hi'], r['s_lo'], r['sat_i'], r['sat_j'],
                     r['a'], r['b']) for r in main_pool]
            sg = [rung_sig[rung][k] for k in keys if k in rung_sig.get(rung, {})]
            if sg:
                print('    indicative lane sigma: med %.3f cyc over %d/%d arcs '
                      '(sqrt(sA^2+sB^2); cross-covariance is NOT in the trace)'
                      % (st.median(sg), len(sg), len(main_pool)))
            print('    implied |err|: %.3f m dispersive / %.3f m range-like '
                  '(med|f| / sensitivity; wraps at +-%.2f m)'
                  % (s['med'] / si, s['med'] / sr, 0.5 / si))
        fams = defaultdict(list)
        for r in main_pool:
            fams[r['fam']].append(r['mean'])
        if len(fams) > 1:
            for fam in sorted(fams):
                print(fmt_stats('  family %s' % fam, frac_stats(fams[fam])))
        if screened:
            print(fmt_stats('  L2W/L2S mod 1.0',
                            frac_stats([r['mean'] for r in screened])))
            print(fmt_stats('  L2W/L2S mod 0.5',
                            frac_stats([r['mean'] for r in screened], lattice=0.5),
                            '(half-cycle lattice, units of 0.5 cyc)'))

    print('\n== PARTITION: baseline bucket ==')
    for rung in sorted(graded, key=lambda r: rung_props(r)[0]):
        buckets = defaultdict(list)
        for r in graded[rung]:
            if not half_cycle_screened(r, rung[0]):
                buckets[baseline_bucket(ecef, r['a'], r['b'])].append(r['mean'])
        if buckets:
            print(' %-14s' % rung_label(rung))
            for bk in sorted(buckets):
                print(fmt_stats('  %s km' % bk, frac_stats(buckets[bk])))

    if anchors:
        print('\n== PARTITION: anchor (same-anchor vs cross-anchor sat pairs) ==')
        print('   the open blocker predicts CROSS-anchor pairs non-integer while '
              'same-anchor pairs stay clean')
        for rung in sorted(graded, key=lambda r: rung_props(r)[0]):
            parts = defaultdict(list)
            for r in graded[rung]:
                if not half_cycle_screened(r, rung[0]):
                    parts[anchor_class(anchors, rung[0], r)].append(r['mean'])
            if parts:
                print(' %-14s' % rung_label(rung))
                for k in ('same', 'cross', 'unknown'):
                    if parts.get(k):
                        print(fmt_stats('  %s-anchor' % k, frac_stats(parts[k])))

    do_mw = '--mw' in argv or ('--no-mw' not in argv and os.path.isdir('data'))
    if do_mw:
        print('\n== MEASUREMENT SIDE: DD of the raw MW statistic (ground truth) ==')
        obs_glob = opt('--obs-glob',
                       'C:/GnssProcessor/data/2024-129/obs/*/*_30S_MO.rnx', str)
        # From every state arc, not just the window-graded ones: under a churn-dominated
        # producer `graded` can be empty while the per-epoch MW gauge is fully available.
        want_sta = {k[6] for keyed in rungs.values() for k in keyed} | \
                   {k[7] for keyed in rungs.values() for k in keyed}
        sod_lo, sod_hi = (tmax - tail_s) % 86400.0, tmax % 86400.0
        print('  glob %s  sod window [%.0f, %.0f]' % (obs_glob, sod_lo, sod_hi))
        mw = mw_reference(obs_glob, sod_lo, sod_hi, want_sta, mw_max_sta)
        if mw:
            report_mw(rungs, mw, dt0, t0, max(3, min_window // 2), ctr)
        else:
            print('  no MW series recovered - the state-vs-MW gauge is UNAVAILABLE (a '
                  'missing instrument, not a clean result)')
    else:
        print('\n== MEASUREMENT SIDE: skipped (--no-mw) ==')

    print('\n== DENOMINATORS ==')
    wl = ctr.pop('window_lengths', [])
    for k in sorted(ctr):
        print('  %-24s %d' % (k, ctr[k]))
    if wl:
        wl.sort()
        print('  window lengths (epochs): n=%d  min=%d  p10=%d  med=%d  p90=%d  max=%d'
              % (len(wl), wl[0], wl[int(.1 * len(wl))], wl[len(wl) // 2],
                 wl[int(.9 * len(wl))], wl[-1]))
        print('  gradeable (>= %d ep): %d of %d (%.1f%%)'
              % (min_window, sum(1 for x in wl if x >= min_window), len(wl),
                 100.0 * sum(1 for x in wl if x >= min_window) / len(wl)))

    print('\n== SPEC 3.4 DECISION-MATRIX READ ==')
    ctr['windows_graded'] = ctr['windows_graded']
    print_matrix_read(rung_verdict, min_n, steps, split_cyc, ctr)

    if csv_out:
        dump_csv(csv_out, graded)
        print('\nwindows dumped to %s' % csv_out)
    return 0


# ---------------------------------------------------------------------------
# Self-tests: the ref_sat algebra and the cross-signal join keying (9.2 / 10)
# ---------------------------------------------------------------------------
def selftest():
    fails = []

    def chk(name, cond, detail=''):
        print('  %-52s %s %s' % (name, 'ok' if cond else 'FAIL', detail))
        if not cond:
            fails.append(name)

    print('== lane_spectrometer selftest ==')

    table = {('G', '1'): (0.190, 5.26, 5.26), ('G', '2'): (0.244, 6.74, 4.10),
             ('G', '5'): (0.255, 7.04, 3.92), ('G', '1', '2'): (0.862, 1.49, 1.16),
             ('E', '1', '5'): (0.751, 1.78, 1.33),
             ('G', '2', '5'): (5.861, 0.293, 0.171),
             ('E', '7', '5'): (9.768, 0.179, 0.102)}
    for rung, want in table.items():
        got = rung_props(rung)
        chk('ladder %s' % rung_label(rung),
            all(abs(a - b) <= 0.006 * max(abs(b), 1e-3) for a, b in zip(got, want)),
            '%.4f/%.4f/%.4f' % got)

    ctr = defaultdict(int)
    truth = {'G02': 3.25, 'G05': -1.75, 'G11': 0.5}
    REF = 'G09'
    rows = [{'tow': 100.0, 'cons': 'G', 'signal': 'L1C', 'sat': s, 'ref': REF,
             'a': 'AAAA', 'b': 'BBBB', 'dd': v, 'sig': 0.1} for s, v in truth.items()]
    grp, gsig = reref(rows, 1.0, 100.0, ctr)
    ps, _ = pair_series(grp, gsig, ctr)
    ok = all(abs(ser[0] - (truth.get(k[2], 0.0) - truth.get(k[3], 0.0))) < 1e-12
             for k, ser in ps.items())
    chk('ref_sat cancels in the within-signal difference', ok)
    chk('reference satellite is carried as a virtual dd=0 member',
        any(REF in (k[2], k[3]) for k in ps))
    chk('pair count = C(n+1,2) with the virtual ref', len(ps) == 6, 'got %d' % len(ps))

    # Two signals with different ref_sat must still join correctly.
    ctr2 = defaultdict(int)
    N = {('G02', 'L2W'): 12.0, ('G05', 'L2W'): -3.0, ('G09', 'L2W'): 7.0,
         ('G02', 'L5Q'): 4.0, ('G05', 'L5Q'): -9.0, ('G09', 'L5Q'): 2.0}
    rows2 = []
    for sig, ref in (('L2W', 'G09'), ('L5Q', 'G02')):
        for sat in ('G02', 'G05', 'G09'):
            if sat == ref:
                continue                              # producer emits no ref row
            rows2.append({'tow': 100.0, 'cons': 'G', 'signal': sig, 'sat': sat,
                          'ref': ref, 'a': 'AAAA', 'b': 'BBBB',
                          'dd': N[(sat, sig)] - N[(ref, sig)], 'sig': 0.1})
    grp2, gsig2 = reref(rows2, 1.0, 100.0, ctr2)
    ps2, ps2_sig = pair_series(grp2, gsig2, ctr2)
    lanes, _ = lane_series(ps2, ps2_sig, ctr2)
    lane = lanes.get(('G', '2', '5'))
    chk('cross-signal lane rung built from mismatched refs', bool(lane))
    ok = bool(lane)
    for key, ser in (lane or {}).items():
        _, fam, s_hi, s_lo, si_, sj_, a, b = key
        exp = ((N[(si_, s_hi)] - N[(sj_, s_hi)]) - (N[(si_, s_lo)] - N[(sj_, s_lo)]))
        if abs(ser[0] - exp) > 1e-12:
            ok = False
            print('     mismatch %s: got %.6f want %.6f' % (str(key), ser[0], exp))
    chk('lane value = exact integer DD despite differing ref_sat', ok)
    chk('lane keys joined = C(3,2) sat pairs', ctr2['lane_keys_joined'] == 3,
        'got %d' % ctr2['lane_keys_joined'])
    naive = ((N[('G05', 'L2W')] - N[('G09', 'L2W')])
             - (N[('G05', 'L5Q')] - N[('G02', 'L5Q')]))
    true5 = ((N[('G05', 'L2W')] - N[('G05', 'L5Q')])
             - (N[('G09', 'L2W')] - N[('G09', 'L5Q')]))
    chk('naive cross-signal join IS wrong (trap is real)', abs(naive - true5) > 0.5,
        'naive %.1f vs true %.1f' % (naive, true5))

    # epochs 0,1,2 | gap (3 missing) | 4,5 | 5->6 jumps 5 cyc | 6,7
    ser = {0: 0.10, 1: 0.11, 2: 0.12, 4: 0.13, 5: 0.14, 6: 5.14, 7: 5.15}
    chk('segmentation cuts at epoch gap and at |delta|>split',
        [len(x) for x in windows(ser, 3, 0.5)] == [3, 2, 2],
        str([len(x) for x in windows(ser, 3, 0.5)]))

    s1 = frac_stats([0.25, 0.75, 1.25], lattice=0.5)
    chk('half-cycle screen maps 0.25 cyc to |f|=0.5 lattice units',
        abs(s1['med'] - 0.5) < 1e-12, 'med %.3f' % s1['med'])
    chk('integer lattice statistics',
        abs(frac_stats([0.05, -0.05, 0.10])['med'] - 0.05) < 1e-12)

    import random
    random.seed(7)
    uni = [random.uniform(-0.5, 0.5) for _ in range(4000)]
    con = [random.gauss(0, 0.03) for _ in range(4000)]
    chk('verdict(uniform sample) == UNIFORM', verdict(frac_stats(uni), 30) == 'UNIFORM')
    chk('verdict(tight sample) == CONCENTRATED',
        verdict(frac_stats(con), 30) == 'CONCENTRATED')
    chk('verdict(thin sample) == thin', verdict(frac_stats(con[:5]), 30) == 'thin')
    chk('verdict(churn-gated) short-circuits',
        verdict(frac_stats(con), 30, True) == 'churn-gated')

    # The null-scored class test: a perfect single magnitude explains 100 %, a ratio equal
    # to the null explains 0 %, and 2.5x across a 60x null explains ~78 %, a case that a
    # fixed 2x bar would reject.
    chk('explained(1.0, 60) == 1', abs(explained(1.0, 60.0) - 1.0) < 1e-9)
    chk('explained(60, 60) == 0', abs(explained(60.0, 60.0)) < 1e-9)
    chk('explained(2.5, 60) ~ 0.78', abs(explained(2.5, 60.0) - 0.776) < 0.01,
        '%.3f' % explained(2.5, 60.0))
    chk('explained is monotone', explained(2.0, 60.0) > explained(8.0, 60.0))

    anc = {('G', 'L1C', 'G01'): frozenset({'AAAA', 'BBBB'}),
           ('G', 'L1C', 'G02'): frozenset({'BBBB', 'CCCC'}),
           ('G', 'L1C', 'G03'): frozenset({'DDDD'})}

    def rec(i, j):
        return {'s_hi': 'L1C', 's_lo': '', 'sat_i': i, 'sat_j': j}

    chk('anchor: intersecting pin sets are same-anchor',
        anchor_class(anc, 'G', rec('G01', 'G02')) == 'same')
    chk('anchor: disjoint pin sets are cross-anchor',
        anchor_class(anc, 'G', rec('G01', 'G03')) == 'cross')
    chk('anchor: a missing satellite is unknown, never guessed',
        anchor_class(anc, 'G', rec('G01', 'G99')) == 'unknown')

    print('\n%s (%d check(s) failed)' % ('SELFTEST FAILED' if fails else 'selftest OK',
                                         len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
