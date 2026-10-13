#!/usr/bin/env python3
"""Wire integer consistency: is the producer's emitted (satellite clock + phase bias)
integer-consistent between satellites, measured directly against the simulator's truth
with no consumer in the loop?

Motivation
----------
``scripts/consumer_float_truth_join.py`` shows that the closed-loop consumer's float
ambiguities on the quiet 24 h truth bed are precise within an arc (EWL 0.015-0.026 cyc,
WL 0.055-0.085, NL 0.053), but that each arc's between-satellite SD ambiguity sits at a
fractional offset uniformly distributed on the circle (p50 |frac| ~ 0.25 = the uniform
null), except GPS EWL, which is partially concentrated.  That is the consumer's view of
the wire; this script measures the wire itself.

The quantity and its sign (all four conventions traced in source, then verified)
--------------------------------------------------------------------------------
Simulator (``src/sim/main.cpp``, the ``row.values`` block):

    P                  = range + c*dt_r - c*sat_clk           (forwardPseudorange)
    sat_clk            = eph.clock(sat, em) + clk_err_m / c   (clk_err_m = 0 unless
                                                               scenario.sat_clock armed)
    L_b * lambda_b     = P + tropo + m7 + ant + bias_phase + rxb_phase + isb
                         - I_b + windup_m + lambda_b*N + np + np_scint + mp_phase

so the satellite-dependent content of a phase observation, over and above the a-priori
CLK product both sides share, is

    -clk_err_m(s,t)  +  osb_phase(s,b)                       ... (OBS)

``osb_phase`` is ``truth_bias.csv``'s ``osb_m`` on the phase RINEX code.  The sim writes
a row per (sat, signal) for both the code and the phase observable of every band
(``record(band.code, bias_code, ...)`` and ``record(band.phase, bias_phase, ...)``), and
the two differ, e.g. G03 C1C = +2.116295 m vs G03 L1C = -0.017022 m.  ``osb_m`` is the
total injected bias (``product_m`` + ``synth_m``), which is what the writer's own comment
says the estimator must reproduce.

Consumer, or any other user of the wire:

    satellite clock bias  = product_clk + C_record / c        (SsrCorrectionStore::
                            applyClockToBias: "Clock correction in seconds; ADD to
                            broadcast clock bias"; GnssCorrections.cpp)
    modeled range         = ... - c * (product_clk + C/c)  =>  contributes  -C
    phase bias term       = + kSsrPhaseBiasSign * P_record,  kSsrPhaseBiasSign = +1.0
                            (MeasurementModel.cpp; docs/network_ambiguity_
                            resolution_design.md "bias added to clean model,
                            identical producer and consumer")

so the satellite-dependent content of the model is

    -C(s,t)  +  P(s,b,t)                                     ... (MODEL)

and the wire error a user's phase ambiguity has to absorb is (OBS) - (MODEL):

    W(s,b,t) = [C(s,t) - clk_true(s,t)] - [P(s,b,t) - osb_phase(s,b)]
             = dC(s,t) - dP(s,b,t)                            <- primary ("minus")

The literal form of the task statement, E = [C + P] - [clk_true + osb_true] = dC + dP,
has the other relative sign; it is computed and tabulated alongside as "plus".  Step 5
decides between the two empirically against the consumer's own float ambiguities and
prints the answer as a verdict.

No IF lump is added to the clock truth.  ``simnet_grade.grade_clock`` grades the C record
against ``sc_truth + if_lump`` because the producer does not apply the .BIA, so the
iono-free lump of the injected code OSB over the pinned pair is absorbed by the estimated
satellite clock.  That lump is a real, per-satellite, band-common offset in metres seen
from the phase side, and whether the emitted phase bias gives it back is what this script
measures; folding it into the truth would remove the quantity under test.  It is
reported separately (a leading candidate for the range-like term r_s of the shape model),
and a lump-removed variant of the clock channel is tabulated as a diagnostic.

Wrapping
--------
The producer pins each (sat, signal) phase bias at an anchor receiver's own absolute
ambiguity estimate, so P carries -lambda * N_anchor: the raw SD error is tens of cycles of
gauge integer.  The wrapped fraction f = e - round(e) is the fixability quantity, as in
consumer_float_truth_join.  Every wrapped statistic is quoted against its own
size-matched uniform null (|f| p50 0.25; P(<0.15) 0.30; P(<0.25) 0.50; R ~ sqrt(pi/4n)).

Usage
-----
    python scripts/simnet_wire_integer_consistency.py <fixture_dir> --run <run_dir>
        [--settle-h 12] [--out <dir>] [--json <path>] [--shape-decim 4]
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from collections import Counter, defaultdict

import numpy as np

_SCRIPTS = os.path.dirname(os.path.abspath(__file__))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

import simnet_grade as SG                      # noqa: E402  product parser + truth semantics
import consumer_float_truth_join as CJ         # noqa: E402  wrap/stat_block/lane algebra

# The true integer is time-dependent on a slipped bed: N(t) = N_int + the sum of the
# truth_slips.csv delta_cycles with tow_s <= t.  simnet_amb_integer's accumulator is
# reused rather than copied; simnet_dd_reref and simnet_t4_census share the same code
# path.
from simnet_amb_integer import load_truth as load_truth_amb, n_at   # noqa: E402

C_LIGHT = CJ.C_LIGHT
FREQ, BANDS, LANES, CONST_NAME = CJ.FREQ, CJ.BANDS, CJ.LANES, CJ.CONST_NAME
wrap, stat_block, nl_from = CJ.wrap, CJ.stat_block, CJ.nl_from

# gamma_b = (f_L1 / f_b)^2 -- the first-order ionospheric scaling, L1-referenced.
GAMMA = {c: {b: (FREQ[c]["L1C"] / FREQ[c][b]) ** 2 for b in BANDS[c]} for c in BANDS}
LAM = {c: {b: C_LIGHT / FREQ[c][b] for b in BANDS[c]} for c in BANDS}


# --------------------------------------------------------------------------- #
# truth
# --------------------------------------------------------------------------- #
def station_dirs(fixture):
    out = []
    for name in sorted(os.listdir(fixture)):
        d = os.path.join(fixture, name)
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "truth_bias.csv")):
            out.append((name, d))
    return out


def load_osb_truth(stations):
    """(sat, RINEX signal) -> injected OSB in metres, asserted identical at every station.

    Satellite-side and time-constant by construction (SG.osb_truth_by_key's contract).
    Both the code row (C1C) and the phase row (L1C) of each band are present and differ.
    """
    series, disagree = {}, 0
    for _name, d in stations:
        with open(os.path.join(d, "truth_bias.csv"), encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                key = (r["satellite"], r["signal"])
                v = float(r["osb_m"])
                if key in series and abs(series[key] - v) > 1e-9:
                    disagree += 1
                series[key] = v
    return series, disagree


def load_satclock_truth(stations):
    """(tow, sat) -> injected satellite-clock error in metres; empty when not armed.

    The sim writes truth_satclock.csv only when sat_clk_err.active() (src/sim/main.cpp).
    An absent file means zero injected satellite-clock error, and the report states
    that instead of defaulting silently.
    """
    series, disagree, files = {}, 0, 0
    for _name, d in stations:
        p = os.path.join(d, "truth_satclock.csv")
        if not os.path.isfile(p):
            continue
        files += 1
        with open(p, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                key = (round(float(r["tow_s"]), 3), r["satellite"])
                v = float(r["clk_err_m"])
                if key in series and abs(series[key] - v) > 1e-9:
                    disagree += 1
                series[key] = v
    return series, disagree, files


def load_truth_amb_series(fixture, stations):
    """(n_int, slips, n_stations): the slip-accumulated truth integer, not a static one.

    ``n_int[(sta, sat, sig)]`` is the pre-slip base and ``slips[(sta, sat, sig)]`` the
    sorted [(tow_s, delta_cycles)] list; the value at an epoch is
    ``n_at(n_int, slips, sta, sat, sig, tow)``.  Both come from
    ``simnet_amb_integer.load_truth``, so the tree has a single accumulator.

    Two adaptations to that loader's contract: it takes bare station names where this
    script carries (name, dir) pairs, and it raises SystemExit on a missing truth file.
    Only the stations that have a truth_ambiguities.csv are handed to it, so a station
    without one is skipped rather than aborting the run.
    """
    names = [name for name, d in stations
             if os.path.isfile(os.path.join(d, "truth_ambiguities.csv"))]
    if not names:
        return {}, {}, 0
    n_int, slips = load_truth_amb(fixture, names)
    return n_int, slips, len(names)


def load_phase_anchor(run):
    """(tow, sat, signal) -> anchoring station, from logs/network_phase_anchor.csv.

    Header: week,tow,cons,signal,sat,anchor_sta; one row per emitted P record.
    """
    p = os.path.join(run, "logs", "network_phase_anchor.csv")
    if not os.path.isfile(p):
        return None
    out = {}
    with open(p, encoding="utf-8") as fh:
        hdr = fh.readline().strip().split(",")
        if hdr != ["week", "tow", "cons", "signal", "sat", "anchor_sta"]:
            raise SystemExit("unexpected network_phase_anchor header: %s" % hdr)
        for line in fh:
            q = line.rstrip("\n").split(",")
            if len(q) >= 6:
                out[(round(float(q[1]), 3), q[4], q[3])] = q[5]
    return out


def load_events(run):
    """tier -> list of rows from logs/network_events.csv."""
    p = os.path.join(run, "logs", "network_events.csv")
    by_tier = defaultdict(list)
    if not os.path.isfile(p):
        return by_tier
    with open(p, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            by_tier[r["tier"]].append(r)
    return by_tier


# --------------------------------------------------------------------------- #
# the wire error
# --------------------------------------------------------------------------- #
def build_wire(epochs, osb, sc_truth, lump):
    """Per epoch: dC{sat}, dCl{sat} (lump-removed), dP{(sat,sig)}, sig{(sat,sig)}."""
    out, n_no_osb = [], Counter()
    for ep in epochs:
        tow = round(ep["tow"], 3)
        dC, dCl = {}, {}
        for s, c0 in ep["clocks"].items():
            if not s:
                continue
            dC[s] = c0 - sc_truth.get((tow, s), 0.0)
            dCl[s] = dC[s] - lump.get(s, 0.0)
        dP, sig = {}, {}
        for (s, g), v in ep["phase"].items():
            if not s:
                continue
            t = osb.get((s, g))
            if t is None:
                n_no_osb[(s, g)] += 1
                continue
            dP[(s, g)] = v - t
            sg = ep["pb_sigma"].get((s, g))
            if sg is not None:
                sig[(s, g)] = sg
        out.append({"tow": tow, "dC": dC, "dCl": dCl, "dP": dP, "sig": sig})
    return out, n_no_osb


def wire_sd(wire, mode="minus", clock_key="dC", fixed_ref=None):
    """Between-satellite SD of the wire error, per (epoch, constellation).

    mode "minus": W = dC - dP  (the quantity a consumer's phase ambiguity absorbs)
    mode "plus" : E = dC + dP  (the task statement's literal form)

    Reference rule (as in consumer_float_truth_join.compute): one satellite per
    (epoch, constellation), chosen among those carrying a clock and all three bands' P
    records as the one with the smallest sum of emitted phase-bias sigma (the wire's
    analogue of the consumer's sum-of-float-std; ties break on the satellite ID).  The
    lanes are defined only with a single reference across bands.  `fixed_ref` pins one
    satellite instead, when that satellite is a candidate at the epoch.

    -> per_ep[tow][const] = {"ref": sat, "e": {sat: {band: e_cycles}}}
    """
    sgn = -1.0 if mode == "minus" else +1.0
    per_ep, refs = {}, defaultdict(Counter)
    for w in wire:
        row = {}
        for const, bands in BANDS.items():
            cand = [s for s in w["dC"]
                    if s.startswith(const) and all((s, b) in w["dP"] for b in bands)]
            if len(cand) < 2:
                continue
            if fixed_ref and fixed_ref.get(const) in cand:
                ref = fixed_ref[const]
            else:
                ref = min(cand, key=lambda s: (sum(w["sig"].get((s, b), 9e9)
                                                   for b in bands), s))
            refs[const][ref] += 1
            E = {s: {b: (w[clock_key][s] + sgn * w["dP"][(s, b)]) for b in bands}
                 for s in cand}
            e = {s: {b: (E[s][b] - E[ref][b]) / LAM[const][b] for b in bands}
                 for s in cand if s != ref}
            row[const] = {"ref": ref, "e": e}
        per_ep[w["tow"]] = row
    return per_ep, refs


def band_lane_records(per_ep):
    """(const, band) -> [(tow, sat, e_cyc)] and (const, lane) -> [(tow, sat, e_cyc)]."""
    band_e, lane_e = defaultdict(list), defaultdict(list)
    for t, row in per_ep.items():
        for const, d in row.items():
            fr = FREQ[const]
            for s, per_b in d["e"].items():
                for b, v in per_b.items():
                    band_e[(const, b)].append((t, s, v))
                for lane, (bhi, blo) in LANES[const].items():
                    e1, e2 = per_b[bhi], per_b[blo]
                    if lane == "NL":
                        lane_e[(const, "NL_raw")].append(
                            (t, s, float(nl_from(e1, e2, fr[bhi], fr[blo]))))
                        lane_e[(const, "NL")].append(
                            (t, s, float(nl_from(float(wrap(e1)), float(wrap(e2)),
                                                 fr[bhi], fr[blo]))))
                    else:
                        lane_e[(const, lane)].append((t, s, e1 - e2))
    return band_e, lane_e


# --------------------------------------------------------------------------- #
# shape decomposition
# --------------------------------------------------------------------------- #
class ShapeFitter:
    """dE_b = r + gamma_b*i + lambda_b*n_b + eps_b, fitted on the wrapped values.

    Objective (task statement):  min over r,i in [-3,3]^2 of
        sum_b [ lambda_b * wrap( f_b - (r + gamma_b*i)/lambda_b ) ]^2      [m^2]
    where f_b = wrap(dE_b / lambda_b).  The objective depends only on the wrapped
    fractions, since the free integers absorb any whole cycle.

    Two solvers, cross-checked against each other:

    * CELL (default, exact): every interior stationary point of the objective is the
      unconstrained LS solution of some integer triple n, and every reachable n is
      generated by an (n1, n2) pair, so enumerating (n1, n2) over the box-implied range
      (67 x 69 = 4623 candidates for both GPS and Galileo) and evaluating the true
      objective at each induced (r, i) finds the interior optimum exactly, with no
      discretization.
    * GRID (validation): the literal 1 cm grid over [-3,3]^2 (601 x 601 = 361 201
      points), run on a random subsample and compared cell-for-cell.

    Identifiability.  With 3 bands the model has 3 observations, 2 real parameters and
    3 free integers (one degree of freedom), and the box contains a few hundred integer
    cells.  A near-zero residual under the 2-parameter model is therefore not evidence of
    a range/iono shape: a uniformly random fraction triple produces one too.  Every
    share below is quoted against a size-matched Monte-Carlo null drawn from independent
    U(-0.5, 0.5) fractions and run through the identical solver, and the gap to that null
    is the result.  The falsifiable statements are the 1-parameter fits (2 dof each) and
    the time behaviour of the fitted r_s, i_s.
    """

    def __init__(self, const, box=3.0, step=0.01):
        self.const = const
        self.bands = BANDS[const]
        self.lam = np.array([LAM[const][b] for b in self.bands])
        self.gam = np.array([GAMMA[const][b] for b in self.bands])
        self.box = box
        # cell-enumeration offsets: n_b - round(dE_b/lam_b) spans +-(box*(1+gamma_b))/lam_b
        k1 = int(math.ceil(box * (1.0 + self.gam[0]) / self.lam[0])) + 1
        k2 = int(math.ceil(box * (1.0 + self.gam[1]) / self.lam[1])) + 1
        a, b = np.meshgrid(np.arange(-k1, k1 + 1), np.arange(-k2, k2 + 1), indexing="ij")
        self.k1 = a.ravel().astype(float)
        self.k2 = b.ravel().astype(float)
        self.n_cand = self.k1.size
        # 1-D grids for the constrained 1-parameter models
        self.rgrid = np.arange(-box, box + 0.5 * step, step)
        # literal 2-D grid (validation only)
        self._grid = None
        self._step = step

    # -- objective ---------------------------------------------------------
    def cost(self, f, r, i):
        """f: (N,3) fractions; r,i: (N,) or (N,M) -> (…) cost in m^2 and residual cyc."""
        u = (r[..., None] + i[..., None] * self.gam) / self.lam       # (..., 3) cycles
        res = wrap(f[..., None, :] - u) if u.ndim == 3 else wrap(f - u)
        return np.sum((res * self.lam) ** 2, axis=-1), res

    def fit_full(self, f):
        """(N,3) -> r, i, cost_m2, res_cyc (N,3).  Exact interior optimum (cell solver)."""
        N = f.shape[0]
        l1, l2 = self.lam[0], self.lam[1]
        g1, g2 = self.gam[0], self.gam[1]
        best_c = np.full(N, np.inf)
        best_r = np.zeros(N)
        best_i = np.zeros(N)
        # chunk the candidate list so the (N, M, 3) working array stays ~4M elements
        CH = max(1, int(4.0e6 / max(N * 3, 1)))
        for j0 in range(0, self.n_cand, CH):
            k1 = self.k1[j0:j0 + CH][None, :]
            k2 = self.k2[j0:j0 + CH][None, :]
            a1 = l1 * (f[:, 0:1] - k1)          # (N, M) residual-free target, band 1
            a2 = l2 * (f[:, 1:2] - k2)
            ii = (a2 - a1) / (g2 - g1)
            rr = a1 - g1 * ii
            c, _ = self.cost(f, rr, ii)         # (N, M)
            bad = (np.abs(rr) > self.box) | (np.abs(ii) > self.box)
            c = np.where(bad, np.inf, c)
            j = np.argmin(c, axis=1)
            cm = c[np.arange(N), j]
            upd = cm < best_c
            best_c = np.where(upd, cm, best_c)
            best_r = np.where(upd, rr[np.arange(N), j], best_r)
            best_i = np.where(upd, ii[np.arange(N), j], best_i)
        _, res = self.cost(f, best_r, best_i)
        return best_r, best_i, best_c, res

    def fit_1p(self, f, which):
        """1-parameter constrained fit on the 1 cm grid + a 1 mm local refinement."""
        g = self.rgrid
        N = f.shape[0]
        best_v = np.zeros(N)
        best_c = np.full(N, np.inf)
        CH = max(1, int(4.0e6 / max(N * 3, 1)))
        for j0 in range(0, g.size, CH):
            gg = g[j0:j0 + CH][None, :]
            r = gg if which == "r" else np.zeros_like(gg)
            i = gg if which == "i" else np.zeros_like(gg)
            c, _ = self.cost(f, np.broadcast_to(r, (N, gg.shape[1])).copy(),
                             np.broadcast_to(i, (N, gg.shape[1])).copy())
            j = np.argmin(c, axis=1)
            cm = c[np.arange(N), j]
            upd = cm < best_c
            best_c = np.where(upd, cm, best_c)
            best_v = np.where(upd, g[j0:j0 + CH][j], best_v)
        off = np.arange(-10, 11) * 0.001
        v = best_v[:, None] + off[None, :]
        r = v if which == "r" else np.zeros_like(v)
        i = v if which == "i" else np.zeros_like(v)
        c, _ = self.cost(f, r, i)
        j = np.argmin(c, axis=1)
        best_v = v[np.arange(N), j]
        best_c = c[np.arange(N), j]
        z = np.zeros(N)
        _, res = self.cost(f, best_v if which == "r" else z,
                           best_v if which == "i" else z)
        return best_v, best_c, res

    def fit_null(self, f):
        z = np.zeros(f.shape[0])
        c, res = self.cost(f, z, z)
        return c, res

    def grid_full(self, f):
        """Literal 1 cm grid over [-3,3]^2: the validation path (slow)."""
        if self._grid is None:
            g = np.arange(-self.box, self.box + 0.5 * self._step, self._step)
            R, I = np.meshgrid(g, g, indexing="ij")
            R, I = R.ravel(), I.ravel()
            # (G, 3) cycles of (r + gamma_b*i)/lambda_b, built once rather than per sample
            self._grid = (R, I, (R[:, None] + I[:, None] * self.gam) / self.lam)
        R, I, u = self._grid
        out_r, out_i, out_c = [], [], []
        for k in range(f.shape[0]):
            c = np.sum((wrap(f[k][None, :] - u) * self.lam) ** 2, axis=1)
            j = int(np.argmin(c))
            out_r.append(R[j]); out_i.append(I[j]); out_c.append(c[j])
        return np.array(out_r), np.array(out_i), np.array(out_c)


def res_rms_cyc(res_cyc):
    return np.sqrt(np.mean(res_cyc ** 2, axis=-1))


def linear_shape(d, gam):
    """Integer-free shape decomposition of d (N, 3) metres onto [1, gamma].

    Used wherever the integers have already been removed: by time-differencing within
    an arc (they are arc-constant, so they cancel exactly) or by the anchor-gauge unwrap.
    3 observations, 2 parameters, one degree of freedom and no free integers, so unlike
    the wrapped per-(s,t) fit this one is identifiable and falsifiable: R^2 against the
    null (no shape at all) is meaningful.

    -> {model: {"p": params, "res": (N,3), "ss": scalar SS of residuals}}, ss_tot
    """
    gam = np.asarray(gam, float)
    one = np.ones_like(gam)
    A = np.stack([one, gam], axis=1)                       # (3, 2)
    out = {}
    p = d @ np.linalg.pinv(A).T                            # (N, 2)
    out["full (r,i)"] = {"p": p, "res": d - p @ A.T}
    pr = (d @ one) / float(one @ one)
    out["range-only (r)"] = {"p": pr, "res": d - pr[:, None] * one}
    pi = (d @ gam) / float(gam @ gam)
    out["iono-only (i)"] = {"p": pi, "res": d - pi[:, None] * gam}
    out["null (0,0)"] = {"p": None, "res": d}
    for v in out.values():
        v["ss"] = float(np.sum(v["res"] ** 2))
    return out, float(np.sum(d ** 2))


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def win(recs, tow0, lo_h, hi_h):
    lo, hi = tow0 + lo_h * 3600.0, tow0 + hi_h * 3600.0
    return np.array([e for (t, _s, e) in recs if lo <= t < hi], dtype=float)


def circ_R(x):
    """Resultant length of the wrapped-cycle sample x (concentration, not location)."""
    x = np.asarray(x, float)
    x = x[np.isfinite(x)]
    if x.size == 0:
        return float("nan"), float("nan"), 0
    z = np.mean(np.exp(2j * np.pi * x))
    return float(abs(z)), float(np.angle(z) / (2 * np.pi)), int(x.size)


def circ_std_cyc(x):
    R, _, n = circ_R(x)
    if not np.isfinite(R) or R <= 0 or n < 2:
        return float("nan")
    return math.sqrt(max(-2.0 * math.log(R), 0.0)) / (2.0 * math.pi)


def circ_corr(a, b):
    """Jammalamadaka circular correlation of two wrapped-cycle samples."""
    a = 2 * np.pi * np.asarray(a, float)
    b = 2 * np.pi * np.asarray(b, float)
    ma = np.angle(np.mean(np.exp(1j * a)))
    mb = np.angle(np.mean(np.exp(1j * b)))
    sa, sb = np.sin(a - ma), np.sin(b - mb)
    den = math.sqrt(float(np.sum(sa ** 2) * np.sum(sb ** 2)))
    return float(np.sum(sa * sb) / den) if den > 0 else float("nan")


def pctl(a, q):
    a = np.asarray(a, float)
    a = a[np.isfinite(a)]
    return float(np.percentile(a, q)) if a.size else float("nan")


# --------------------------------------------------------------------------- #
# dual-band mode (card IRD section 8)
# --------------------------------------------------------------------------- #
# The three-band path above selects nothing on a dual-band product (every candidate
# needs L5).  The dual-band mode grades one band pair, all satellite pairs, with the
# code-implied ionosphere offset x the consumer adopts:
#
#   Wp_b = dC - dP_b,  Wc_b = dC - dB_b          (wire errors, metres; dX = emitted - truth)
#   x    = (Wc_1 - Wc_2) / (mu_1 - mu_2)          (mu_b = (f_1 / f_b)^2, band 1 first)
#   E_b  = (Wp_b + mu_b x) / lambda_b             (what the consumer's float absorbs, cycles)
#   r    = Wc_1 - mu_1 x                          (the range-like code term; 0 when the IF code
#                                                  lump sits in the clock)
#
# Between satellites s and p of one system: D_b = E_b^s - E_b^p; D_WL = D_1 - D_2 (the
# consumer's wide lane); g = D_1 + f_2/(f_1 - f_2) (D_WL - round(D_WL)) (the ionosphere-free
# part given the wide lane, NL cycles; 3.529 for L1/L2); the Melbourne-Wubbena wide lane MW,
# equal to D_WL - (r^s - r^p)/lambda_WL.  Every statistic is |wrap| over all pair-epochs,
# quoted against a size-matched null: one uniform fraction per unit (a satellite with its
# hold generations), the same statistic over the same occupancy.
DUAL_FREQ = {"G": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6},
             "E": {"1": 1575.42e6, "5": 1176.45e6, "7": 1207.14e6, "6": 1278.75e6,
                   "8": 1191.795e6}}
DUAL_CHANNELS = ("E1", "E2", "WL", "NL", "MW")
PREHOLD_HEADER = ("week", "tow", "sat", "signal", "est_pb_m", "est_pb_sigma_m", "est_clk_m",
                  "phase_visible_m", "emit_pb_m", "gen", "role", "comp", "r1", "attach_sta",
                  "depth")


def dual_freq(sat, sig):
    """Carrier frequency (Hz) of a RINEX phase or code signal of satellite `sat`."""
    return DUAL_FREQ[sat[0]][sig[1]]


def code_of(phase_sig):
    """The code signal paired with a phase signal (L1C -> C1C, L2W -> C2W)."""
    return "C" + phase_sig[1:]


def wrapf(v):
    """Fraction in [-0.5, 0.5] (array or scalar)."""
    return v - np.round(v)


def weighted_quantile(vals, w, q):
    """The q-quantile of `vals` with integer or real weights `w` (the lower sample at the
    cumulative weight q; a plain sample quantile when every weight is 1)."""
    vals, w = np.asarray(vals, float), np.asarray(w, float)
    if vals.size == 0:
        return float("nan")
    o = np.argsort(vals)
    c = np.cumsum(w[o])
    return float(vals[o][min(np.searchsorted(c, q * c[-1]), vals.size - 1)])


def size_matched_null(n_units, ia, ib, w, mc=2000, seed=20261013):
    """The size-matched null of the p50 |wrap| statistic (card IRD section 8).

    One uniform fraction per unit; a record is a unit (ib None) or a pair of units
    (ia, ib), with occupancy weights w (pair-epochs or arc-epochs per record).  Returns
    (mean, p2.5, p97.5) of the weighted p50 of |wrap(u_a - u_b)| (or |u_a|) over mc draws."""
    rng = np.random.default_rng(seed)
    ia = np.asarray(ia, int)
    ib = None if ib is None else np.asarray(ib, int)
    w = np.asarray(w, float)
    if ia.size == 0:
        return float("nan"), float("nan"), float("nan")
    out = np.empty(mc)
    for k in range(mc):
        u = rng.uniform(-0.5, 0.5, n_units)
        v = np.abs(u[ia]) if ib is None else np.abs(wrapf(u[ia] - u[ib]))
        out[k] = weighted_quantile(v, w, 0.5)
    return float(out.mean()), float(np.percentile(out, 2.5)), float(np.percentile(out, 97.5))


def _tokens(fields):
    """key=value tokens of a product record's tail."""
    out = {}
    for f in fields:
        if "=" in f:
            k, v = f.split("=", 1)
            out[k] = v
    return out


def parse_products_tokens(path):
    """(s_fields, epochs, census) from a products.txt, keeping the P record tokens.

    Each epoch: {tow, C {sat: m}, B {(sat, sig): m}, P {(sat, sig): m},
    attr {(sat, sig): {"si": signal_integer, "gen": .., "cmp": .., "genc": ..}}}.
    The record census guard of simnet_grade.parse_products is kept: a product with no C,
    B or P record is refused rather than graded as an empty (perfect-looking) wire."""
    s_fields, epochs, cur = None, [], None
    census = Counter()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n").rstrip("\r")
            if not line or line.startswith("#"):
                continue
            p = line.split(",")
            census[p[0]] += 1
            if p[0] == "S":
                s_fields = p
            elif p[0] == "E":
                cur = {"tow": round(float(p[2]), 3), "C": {}, "B": {}, "P": {}, "attr": {}}
                epochs.append(cur)
            elif cur is None:
                continue
            elif p[0] == "C":
                cur["C"][p[1]] = float(p[2])
            elif p[0] == "B":
                cur["B"][(p[1], p[2])] = float(p[3])
            elif p[0] == "P":
                key = (p[1], p[2])
                cur["P"][key] = float(p[3])
                tok = _tokens(p[4:])
                cur["attr"][key] = {"si": int(p[4]) if len(p) > 4 and p[4].lstrip("-").isdigit()
                                    else None,
                                    "gen": tok.get("gen"), "cmp": tok.get("cmp"),
                                    "genc": tok.get("genc")}
    if census["C"] == 0 or census["B"] == 0 or census["P"] == 0:
        raise SystemExit("products %s carries no C, B or P record (census %s): refused, an empty "
                         "wire would grade as perfect" % (path, dict(census)))
    return s_fields, epochs, census


def load_prehold(path, product_epochs):
    """Epochs in parse_products_tokens' shape from logs/network_pb_prehold.csv (card IRD
    section 7): P = est_pb_m and C = est_clk_m, sampled immediately before the phase-bias
    hold; B (the code biases, which the hold does not touch) from the products at the same
    epoch.  attr carries gen, role, comp, r1, attach_sta and depth."""
    by_tow = {ep["tow"]: ep for ep in product_epochs}
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(l for l in fh if not l.startswith("#"))
        hdr = tuple(next(rd))
        if hdr[:len(PREHOLD_HEADER)] != PREHOLD_HEADER:
            raise SystemExit("unexpected network_pb_prehold.csv header: %s" % (hdr,))
        ix = {c: i for i, c in enumerate(hdr)}
        for r in rd:
            if len(r) < len(PREHOLD_HEADER):
                continue
            tow = round(float(r[ix["tow"]]), 3)
            ep = out.get(tow)
            if ep is None:
                src = by_tow.get(tow)
                ep = {"tow": tow, "C": {}, "B": dict(src["B"]) if src else {}, "P": {},
                      "attr": {}}
                out[tow] = ep
            sat, sig = r[ix["sat"]], r[ix["signal"]]
            ep["P"][(sat, sig)] = float(r[ix["est_pb_m"]])
            if r[ix["est_clk_m"]] != "":
                ep["C"][sat] = float(r[ix["est_clk_m"]])
            ep["attr"][(sat, sig)] = {"si": None, "gen": r[ix["gen"]], "cmp": r[ix["comp"]],
                                      "genc": None, "role": r[ix["role"]],
                                      "depth": r[ix["depth"]], "r1": r[ix["r1"]],
                                      "attach_sta": r[ix["attach_sta"]]}
    return [out[t] for t in sorted(out)]


def dual_sat_values(ep, osb, sc_truth, bands, xcorr=True):
    """sat -> {E1, E2, MWs, r, x, unit attrs} for every satellite of the epoch with a clock,
    both codes and both phases of `bands` and their truth.  E_b is x-corrected unless
    xcorr is False (then raw Wp_b / lambda_b, the reading that misgrades an integer wire)."""
    b1, b2 = bands
    out = {}
    for s, c0 in ep["C"].items():
        if not s or s[0] not in DUAL_FREQ:
            continue
        ck = [(s, code_of(b1)), (s, code_of(b2))]
        pk = [(s, b1), (s, b2)]
        if any(k not in ep["B"] or k not in osb for k in ck) or \
           any(k not in ep["P"] or k not in osb for k in pk):
            continue
        f1, f2 = dual_freq(s, b1), dual_freq(s, b2)
        l1, l2 = C_LIGHT / f1, C_LIGHT / f2
        mu1, mu2 = 1.0, (f1 / f2) ** 2
        dC = c0 - sc_truth.get((ep["tow"], s), 0.0)
        wc1 = dC - (ep["B"][ck[0]] - osb[ck[0]])
        wc2 = dC - (ep["B"][ck[1]] - osb[ck[1]])
        w1 = dC - (ep["P"][pk[0]] - osb[pk[0]])
        w2 = dC - (ep["P"][pk[1]] - osb[pk[1]])
        x = (wc1 - wc2) / (mu1 - mu2)
        r = wc1 - mu1 * x
        if xcorr:
            e1, e2 = (w1 + mu1 * x) / l1, (w2 + mu2 * x) / l2
        else:
            e1, e2 = w1 / l1, w2 / l2
        lwl = C_LIGHT / (f1 - f2)
        mw = ((f1 * w1 - f2 * w2) / (f1 - f2) - (f1 * wc1 + f2 * wc2) / (f1 + f2)) / lwl
        a1, a2 = ep["attr"].get(pk[0], {}), ep["attr"].get(pk[1], {})
        out[s] = {"E1": e1, "E2": e2, "MWs": mw, "r": r, "x": x,
                  "knl": f2 / (f1 - f2), "unit": (s, a1.get("gen"), a2.get("gen")),
                  "a1": a1, "a2": a2}
    return out


def dual_pair_channels(a, b):
    """The five channels of the pair (s, p) = (a, b), unwrapped, in cycles (NL in NL cycles)."""
    d1, d2 = a["E1"] - b["E1"], a["E2"] - b["E2"]
    dwl = d1 - d2
    return {"E1": d1, "E2": d2, "WL": dwl, "NL": d1 + a["knl"] * (dwl - round(dwl)),
            "MW": a["MWs"] - b["MWs"]}


def _split_label(kind, a, b):
    """A pair's label for --split (card IRD section 8)."""
    if kind == "comp":
        ca, cb = a["a1"].get("cmp"), b["a1"].get("cmp")
        if not ca or not cb:
            return "na"
        return "same:%s" % ca if ca == cb else "cross"
    if kind == "depth":
        da, db = a["a1"].get("depth"), b["a1"].get("depth")
        try:
            d = max(int(da), int(db))
        except (TypeError, ValueError):
            return "na"
        return "d%d" % d if d < 5 else "d5+"
    if kind == "gen":
        ga, gb = a["a1"].get("genc"), b["a1"].get("genc")
        if ga or gb:
            return "genc:%s/%s" % tuple(sorted((ga or "-", gb or "-")))
        ga, gb = a["a1"].get("gen"), b["a1"].get("gen")
        if ga is None or gb is None:
            return "na"
        return "first" if str(ga) == "1" and str(gb) == "1" else "later"
    if kind == "role":
        ra, rb = a["a1"].get("role"), b["a1"].get("role")
        if not ra or not rb:
            return "na"
        return "/".join(sorted((ra, rb)))
    raise ValueError(kind)


def dual_grade(epochs, osb, sc_truth, bands=("L1C", "L2W"), systems=("G",), start_tow=None,
               end_tow=None, xcorr=True, splits=(), mc=2000, seed=20261013):
    """Hourly all-pairs grade of the dual-band wire.  Returns
    {hour: {"n": .., "units": .., "rabs_p50": .., channel: {p50, p99, in15, in20, in25,
    null, null_lo, null_hi, gap}, "splits": {kind: {label: {channel: {n, p50}}}}}}."""
    if start_tow is None:
        start_tow = epochs[0]["tow"]
    recs = defaultdict(lambda: {c: [] for c in DUAL_CHANNELS})
    occ = defaultdict(Counter)
    rabs = defaultdict(list)
    spl = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for ep in epochs:
        t = ep["tow"]
        if t < start_tow - 1e-3 or (end_tow is not None and t >= end_tow - 1e-3):
            continue
        h = int((t - start_tow) // 3600.0)
        vals = dual_sat_values(ep, osb, sc_truth, bands, xcorr)
        for s, v in vals.items():
            rabs[h].append(abs(v["r"]))
        for sysl in systems:
            sats = sorted(s for s in vals if s[0] == sysl)
            for i in range(len(sats)):
                a = vals[sats[i]]
                for j in range(i + 1, len(sats)):
                    b = vals[sats[j]]
                    ch = dual_pair_channels(a, b)
                    for c in DUAL_CHANNELS:
                        recs[h][c].append(ch[c])
                    occ[h][(a["unit"], b["unit"])] += 1
                    for kind in splits:
                        lab = _split_label(kind, a, b)
                        for c in DUAL_CHANNELS:
                            spl[h][kind + "|" + lab][c].append(ch[c])
    out = {}
    for h in sorted(recs):
        pairs = list(occ[h].items())
        units = sorted({u for (ua, ub), _ in pairs for u in (ua, ub)}, key=str)
        ix = {u: k for k, u in enumerate(units)}
        ia = [ix[ua] for (ua, ub), _ in pairs]
        ib = [ix[ub] for (ua, ub), _ in pairs]
        w = [n for _, n in pairs]
        null, lo, hi = size_matched_null(len(units), ia, ib, w, mc=mc, seed=seed + h)
        row = {"n": len(recs[h]["E1"]), "units": len(units),
               "rabs_p50": float(np.median(rabs[h])) if rabs[h] else float("nan")}
        for c in DUAL_CHANNELS:
            v = np.abs(wrapf(np.asarray(recs[h][c])))
            p50 = float(np.median(v)) if v.size else float("nan")
            row[c] = {"p50": p50, "p99": float(np.percentile(v, 99)) if v.size else float("nan"),
                      "in15": float(np.mean(v <= 0.15)) if v.size else float("nan"),
                      "in20": float(np.mean(v <= 0.20)) if v.size else float("nan"),
                      "in25": float(np.mean(v <= 0.25)) if v.size else float("nan"),
                      "null": null, "null_lo": lo, "null_hi": hi, "gap": p50 - null}
        sp = {}
        for key, chans in spl[h].items():
            kind, lab = key.split("|", 1)
            sp.setdefault(kind, {})[lab] = {
                c: {"n": len(chans[c]),
                    "p50": float(np.median(np.abs(wrapf(np.asarray(chans[c])))))}
                for c in DUAL_CHANNELS}
        row["splits"] = sp
        out[h] = row
    return out


def dual_print(res, label, null_tol=None, channels=("E1", "E2", "WL", "NL", "MW")):
    """Print the hourly grade; with null_tol, the negative-control verdict per hour and
    channel (|p50 - null| <= null_tol and n > 0).  Returns the list of failing cells."""
    fails = []
    print("== %s" % label)
    print("   h      n units  |r|p50_m  " + "  ".join("%-27s" % c for c in channels))
    for h, row in sorted(res.items()):
        cells = []
        for c in channels:
            d = row[c]
            cells.append("%.3f p99 %.2f gap %+.3f" % (d["p50"], d["p99"], d["gap"]))
            if null_tol is not None and (row["n"] <= 0 or not abs(d["gap"]) <= null_tol):
                fails.append((label, h, c, d["p50"], d["null"], d["gap"], row["n"]))
        print("   h%-3d %7d %5d  %8.4f  " % (h, row["n"], row["units"], row["rabs_p50"])
              + "  ".join("%-27s" % x for x in cells))
        print("        null %.3f [%.3f, %.3f]; within 0.15/0.20/0.25 (WL): %.2f / %.2f / %.2f"
              % (row["E1"]["null"], row["E1"]["null_lo"], row["E1"]["null_hi"],
                 row["WL"]["in15"], row["WL"]["in20"], row["WL"]["in25"]))
        for kind, labs in sorted(row.get("splits", {}).items()):
            for lab, chans in sorted(labs.items()):
                print("        split %-6s %-22s " % (kind, lab)
                      + "  ".join("%s n %d p50 %.3f" % (c, chans[c]["n"], chans[c]["p50"])
                                  for c in channels))
    if null_tol is not None:
        print("   negative control (|p50 - null| <= %.3f, n > 0 every hour): %s"
              % (null_tol, "PASS" if not fails else "FAIL in %d cell(s)" % len(fails)))
        for f in fails:
            print("     FAIL %s h%d %s: p50 %.4f null %.4f gap %+.4f n %d" % f)
    return fails


def start_from_hms(first_tow, hms):
    """TOW of the clock time `hms` (HH:MM:SS) on the day of `first_tow`."""
    hh, mm, ss = (float(x) for x in hms.split(":"))
    return math.floor(first_tow / 86400.0) * 86400.0 + hh * 3600.0 + mm * 60.0 + ss


def dual_load(fixture, run, source="products"):
    """(osb, sc_truth, epochs, census) for the dual-band grade of `run` against `fixture`."""
    stations = station_dirs(fixture)
    if not stations:
        raise SystemExit("no station with truth_bias.csv under %s" % fixture)
    osb, _ = load_osb_truth(stations)
    sc_truth, _, _ = load_satclock_truth(stations)
    prod = run if run.endswith(".txt") else os.path.join(run, "products.txt")
    _, epochs, census = parse_products_tokens(prod)
    if source == "prehold":
        ph = os.path.join(run, "logs", "network_pb_prehold.csv")
        if not os.path.isfile(ph):
            raise SystemExit("missing %s (network.trace_pb_prehold, card IRD-1)" % ph)
        epochs = load_prehold(ph, epochs)
    return osb, sc_truth, epochs, census


def dual_main(args):
    bands = tuple(args.bands.split(","))
    if len(bands) != 2:
        raise SystemExit("--bands takes exactly two phase signals, e.g. L1C,L2W")
    run = os.path.abspath(args.run)
    osb, sc_truth, epochs, census = dual_load(os.path.abspath(args.fixture), run, args.source)
    t0 = epochs[0]["tow"]
    start = args.start_tow if args.start_tow is not None else (
        start_from_hms(t0, args.start_hms) if args.start_hms else t0)
    end = start + args.hours * 3600.0 if args.hours else None
    splits = tuple(s for s in (args.split or "").split(",") if s)
    res = dual_grade(epochs, osb, sc_truth, bands, tuple(args.systems.split(",")), start, end,
                     xcorr=args.xcorr, splits=splits, mc=args.null_mc)
    chans = ["E1", "E2"] + (["WL", "MW"] if args.wl else []) + (["NL"] if args.nl else [])
    print("dual-band wire grade: run %s, source %s, bands %s, x-correction %s, start TOW %.3f%s"
          % (run, args.source, "/".join(bands), "on" if args.xcorr else "OFF (raw W)", start,
             ", %.2f h" % args.hours if args.hours else ""))
    print("record census: %s" % ", ".join("%s=%d" % kv for kv in sorted(census.items())))
    fails = dual_print(res, os.path.basename(run.rstrip("/\\")), args.null_tol, chans)
    if args.events:
        import simnet_chain_frame_grade as FG          # one owner of the event steps
        FG.print_event_steps(FG.event_steps(epochs, osb, sc_truth, bands,
                                            FG.load_chain_events(run), xcorr=args.xcorr))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"run": run, "source": args.source, "bands": bands, "start_tow": start,
                       "hours": {str(h): v for h, v in res.items()},
                       "null_fail": [list(f) for f in fails]}, fh, indent=1, default=float)
    return 1 if (args.null_tol is not None and fails) else 0


def dual_selftest():
    """Synthetic truth and products: a truth wire grades 0, an integer gauge grades 0 wrapped,
    a planted fraction is read back, the x correction removes an ionosphere-shaped code and
    phase error that raw W misgrades, MW differs from D_WL by exactly r / lambda_WL, the NL
    formula, the size-matched null, the census guard, the pre-hold reader and the hour bins."""
    import tempfile
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    f1, f2 = DUAL_FREQ["G"]["1"], DUAL_FREQ["G"]["2"]
    l1, l2 = C_LIGHT / f1, C_LIGHT / f2
    mu2 = (f1 / f2) ** 2
    sats = ["G%02d" % k for k in range(1, 9)]
    truth = {}
    for k, s in enumerate(sats):
        truth[(s, "C1C")] = 1.0 + 0.1 * k
        truth[(s, "C2W")] = -0.7 + 0.13 * k
        truth[(s, "L1C")] = 0.031 * k
        truth[(s, "L2W")] = -0.047 * k
    with tempfile.TemporaryDirectory() as td:
        fx = os.path.join(td, "fx")
        for sta in ("S01000SIM", "S02000SIM"):
            os.makedirs(os.path.join(fx, sta))
            with open(os.path.join(fx, sta, "truth_bias.csv"), "w", newline="") as fh:
                fh.write("satellite,signal,osb_ns,osb_m,product_m,synth_m,source\n")
                for (s, g), v in truth.items():
                    fh.write("%s,%s,0,%.9f,%.9f,0,product\n" % (s, g, v, v))

        def write_products(path, edit, n_ep=240, gen_of=lambda s, t: 1):
            with open(path, "w", newline="") as fh:
                fh.write("# gnss_network_product v2\n")
                fh.write("S,S01000SIM,1,nominal-wu93,2,0,1,0,0.05,1,x,G:G01\n")
                for e in range(n_ep):
                    t = 261030.0 + 30.0 * e
                    fh.write("E,2313,%.4f,0,0,0,0\n" % t)
                    for s in sats:
                        v = edit(s, t)
                        fh.write("C,%s,%.12f\n" % (s, v["C"]))
                        for g in ("C1C", "C2W"):
                            fh.write("B,%s,%s,%.12f\n" % (s, g, v[g]))
                        for g in ("L1C", "L2W"):
                            fh.write("P,%s,%s,%.12f,1,0,gen=%d,cmp=1\n" % (s, g, v[g], gen_of(s, t)))

        def truth_vals(s, t):
            return {"C": 0.0, "C1C": truth[(s, "C1C")], "C2W": truth[(s, "C2W")],
                    "L1C": truth[(s, "L1C")], "L2W": truth[(s, "L2W")]}

        def grade(edit, **kw):
            p = os.path.join(td, "products.txt")
            write_products(p, edit, **{k: v for k, v in kw.items() if k in ("n_ep", "gen_of")})
            osb, sc, eps, _ = dual_load(fx, p)
            res = dual_grade(eps, osb, sc, mc=200,
                             **{k: v for k, v in kw.items() if k in ("xcorr", "start_tow")})
            return osb, sc, eps, res

        _, _, eps, res = grade(truth_vals)
        chk(sorted(res) == [0, 1], "240 epochs at 30 s from 00:30:30 fall in hours 0 and 1")
        chk(all(res[h][c]["p50"] == 0.0 and res[h][c]["p99"] < 1e-9 for h in res
                for c in DUAL_CHANNELS), "a truth wire grades 0 on every channel")
        chk(res[0]["n"] == 120 * 28, "all 28 pairs of 8 satellites in each of 120 epochs (n printed)")

        def gauge(s, t):
            v = truth_vals(s, t)
            k = sats.index(s)
            v["L1C"] += l1 * (3 * k - 7)
            v["L2W"] += l2 * (-5 * k + 2)
            v["C"] += 0.0
            return v
        _, _, _, res = grade(gauge)
        chk(all(res[h][c]["p99"] < 1e-6 for h in res for c in DUAL_CHANNELS),
            "an integer gauge on P grades 0 wrapped on every channel")

        def planted(s, t):
            v = truth_vals(s, t)
            if s == "G02":
                v["L1C"] += 0.3 * l1
            return v
        osb, sc, eps, res = grade(planted)
        vals = dual_sat_values(eps[0], osb, sc, ("L1C", "L2W"))
        chk(abs(vals["G02"]["E1"] + 0.3) < 1e-9 and abs(vals["G01"]["E1"]) < 1e-9,
            "a +0.3 cycle P on G02 L1C reads E_L1 = -0.3 (W = dC - dP)")
        chk(abs(dual_pair_channels(vals["G02"], vals["G01"])["NL"]
                - (-0.3 + (f2 / (f1 - f2)) * (-0.3 - round(-0.3)))) < 1e-9,
            "g = D1 + f2/(f1-f2) (D_WL - round(D_WL)) on the planted pair")
        chk(abs(f2 / (f1 - f2) - 3.5294) < 1e-4, "f2/(f1-f2) = 3.529 for L1/L2")

        def iono_shaped(s, t):
            v = truth_vals(s, t)
            a = 0.37 * (1 + sats.index(s))
            v["C1C"] += a
            v["C2W"] += mu2 * a
            v["L1C"] -= a
            v["L2W"] -= mu2 * a
            return v
        osb, sc, eps, res = grade(iono_shaped)
        chk(all(res[h][c]["p99"] < 1e-6 for h in res for c in ("E1", "E2", "WL", "NL", "MW")),
            "an ionosphere-shaped code+phase wire error grades 0 with the x correction")
        _, _, _, res_raw = grade(iono_shaped, xcorr=False)
        chk(res_raw[0]["E1"]["p50"] > 0.05, "the same error misgrades on raw W (p50 %.3f)"
            % res_raw[0]["E1"]["p50"])

        def ranged(s, t):
            v = truth_vals(s, t)
            if s == "G03":
                v["C1C"] += 0.2
                v["C2W"] += 0.2
            return v
        osb, sc, eps, _ = grade(ranged)
        vals = dual_sat_values(eps[0], osb, sc, ("L1C", "L2W"))
        ch = dual_pair_channels(vals["G03"], vals["G01"])
        chk(abs(vals["G03"]["r"] + 0.2) < 1e-9 and abs(ch["WL"]) < 1e-9
            and abs(ch["MW"] - (ch["WL"] - (vals["G03"]["r"] - vals["G01"]["r"])
                                / (C_LIGHT / (f1 - f2)))) < 1e-9,
            "a range-like code error: r = -0.2 m, D_WL 0, MW = D_WL - dr / lambda_WL")

        _, _, _, res = grade(truth_vals, start_tow=261030.0 + 1800.0)
        chk(sorted(res) == [0, 1] and res[0]["n"] == 120 * 28 and res[1]["n"] == 60 * 28,
            "a later start re-bins the hours (120 then 60 epochs)")

        nm, lo, hi = size_matched_null(400, list(range(400)), None, [1] * 400, mc=400)
        chk(abs(nm - 0.25) < 0.01 and lo < 0.25 < hi,
            "null of 400 independent units: mean %.3f, band [%.3f, %.3f]" % (nm, lo, hi))
        nm8, lo8, hi8 = size_matched_null(8, [0, 0, 1], [1, 2, 2], [5, 5, 5], mc=400)
        chk(hi8 - lo8 > hi - lo, "few units give a wider null band (%.3f against %.3f)"
            % (hi8 - lo8, hi - lo))
        chk(weighted_quantile([0.1, 0.2, 0.3], [1, 1, 10], 0.5) == 0.3,
            "the weighted median follows the occupancy")

        p = os.path.join(td, "empty.txt")
        with open(p, "w") as fh:
            fh.write("S,X,1,w,0,0,1,0,0.05,1,x\nE,2313,1.0,0,0,0,0\nH,2313,1.0,ok,1,0,-\n")
        try:
            parse_products_tokens(p)
            chk(False, "the census guard refuses a wire with no C, B or P record")
        except SystemExit:
            chk(True, "the census guard refuses a wire with no C, B or P record")

        p = os.path.join(td, "products.txt")
        write_products(p, planted, n_ep=4)
        os.makedirs(os.path.join(td, "logs"), exist_ok=True)
        with open(os.path.join(td, "logs", "network_pb_prehold.csv"), "w", newline="") as fh:
            fh.write(",".join(PREHOLD_HEADER) + "\n")
            for e in range(4):
                t = 261030.0 + 30.0 * e
                for s in sats:
                    for g in ("L1C", "L2W"):
                        fh.write("2313,%.3f,%s,%s,%.12f,0.01,0.0,0,0,1,%s,%d,S01000SIM,S01000SIM,%d\n"
                                 % (t, s, g, truth[(s, g)], "tree" if s < "G05" else "chord_only",
                                    1 if s < "G07" else 2, sats.index(s) % 3))
        osb, sc, eps, _ = dual_load(fx, td, source="prehold")
        res = dual_grade(eps, osb, sc, mc=50, splits=("comp", "role", "depth", "gen"))
        chk(len(eps) == 4 and res[0]["E1"]["p99"] < 1e-9,
            "the pre-hold reader takes P from est_pb_m (the planted product fraction is gone)")
        chk(set(res[0]["splits"]["comp"]) == {"same:1", "same:2", "cross"}
            and res[0]["splits"]["comp"]["cross"]["E1"]["n"] == 4 * 12,
            "the comp split separates same-component and cross-component pairs")
        chk("chord_only/tree" in res[0]["splits"]["role"], "the role split labels pairs")
    print("dual-band selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture", nargs="?")
    ap.add_argument("--run")
    ap.add_argument("--selftest", action="store_true",
                    help="run the dual-band mode's selftest (card IRD) and exit")
    ap.add_argument("--bands", default=None,
                    help="dual-band mode (card IRD): two phase signals, e.g. L1C,L2W")
    ap.add_argument("--xcorr", dest="xcorr", action="store_true", default=True,
                    help="dual-band: apply the code-implied ionosphere offset x (default)")
    ap.add_argument("--no-xcorr", dest="xcorr", action="store_false",
                    help="dual-band: grade raw W instead (misgrades an integer wire)")
    ap.add_argument("--wl", action="store_true", help="dual-band: report D_WL and MW")
    ap.add_argument("--nl", action="store_true",
                    help="dual-band: report g = D1 + f2/(f1-f2)(D_WL - round(D_WL))")
    ap.add_argument("--source", choices=("products", "prehold"), default="products",
                    help="dual-band: products.txt, or logs/network_pb_prehold.csv (card IRD-1)")
    ap.add_argument("--split", default="", help="dual-band: comma list of comp,depth,gen,role")
    ap.add_argument("--events", action="store_true",
                    help="dual-band: pair steps at chain event epochs (simnet_chain_frame_grade)")
    ap.add_argument("--systems", default="G", help="dual-band: systems graded (default G)")
    ap.add_argument("--start-tow", type=float, default=None, help="dual-band: grading start TOW")
    ap.add_argument("--start-hms", default=None,
                    help="dual-band: grading start as HH:MM:SS on the run's day (e.g. 00:30:15)")
    ap.add_argument("--hours", type=float, default=None, help="dual-band: graded span (h)")
    ap.add_argument("--null-mc", type=int, default=2000, help="dual-band: null draws")
    ap.add_argument("--null-tol", type=float, default=None,
                    help="dual-band: print the negative-control verdict |p50 - null| <= tol")
    ap.add_argument("--settle-h", type=float, default=12.0)
    ap.add_argument("--out", default=None, help="output dir (default <run>/artrack)")
    ap.add_argument("--json", default=None, help="explicit json path")
    ap.add_argument("--shape-decim", type=int, default=4,
                    help="keep every Nth epoch for the shape fit (default 4 = 120 s)")
    ap.add_argument("--grid-check", type=int, default=200,
                    help="samples per window re-fitted on the literal 1 cm grid")
    ap.add_argument("--seed", type=int, default=20240315)
    ap.add_argument("--json-dual", dest="json_dual", default=None,
                    help="dual-band: write the hourly grade as JSON")
    args = ap.parse_args()
    if args.selftest:
        return dual_selftest()
    if not args.fixture or not args.run:
        ap.error("the fixture and --run are required (or --selftest)")
    if args.bands:
        args.json = args.json_dual or args.json
        return dual_main(args)

    rng = np.random.default_rng(args.seed)
    fixture = os.path.abspath(args.fixture)
    run = os.path.abspath(args.run)
    out_dir = os.path.abspath(args.out) if args.out else os.path.join(run, "artrack")
    os.makedirs(out_dir, exist_ok=True)

    L = []
    P = L.append
    J = {"fixture": fixture, "run": run, "settle_h": args.settle_h}

    # ---- inputs ------------------------------------------------------------
    stations = station_dirs(fixture)
    osb, osb_disagree = load_osb_truth(stations)
    sc_truth, sc_disagree, sc_files = load_satclock_truth(stations)
    tn_int, tslips, n_amb_sta = load_truth_amb_series(fixture, stations)
    n_slip = sum(len(v) for v in tslips.values())
    s_line, epochs, census = SG.parse_products(os.path.join(run, "products.txt"))
    lump = SG.if_lump_by_sat(osb, s_line.get("cbpin", {}))
    wire, n_no_osb = build_wire(epochs, osb, sc_truth, lump)
    anchors = load_phase_anchor(run)
    events = load_events(run)

    tows = [w["tow"] for w in wire]
    tow0, tow1 = tows[0], tows[-1]
    span_h = (tow1 - tow0) / 3600.0
    settle = args.settle_h
    end_h = math.ceil(span_h) + 1e-6
    WINDOWS = [("h0-h%g" % settle, (0.0, settle)),
               ("h%g-h%.0f" % (settle, span_h), (settle, end_h))]

    P("=" * 128)
    P("WIRE INTEGER CONSISTENCY  --  the emitted (clock + phase bias) against injected truth,")
    P("                              no consumer in the loop")
    P("=" * 128)
    P("run      : %s" % run)
    P("fixture  : %s  (%d stations with truth)" % (fixture, len(stations)))
    P("product  : %d epochs, tow %.0f .. %.0f (%.2f h); record census %s"
      % (len(epochs), tow0, tow1, span_h,
         ", ".join("%s=%d" % kv for kv in sorted(census.items())
                   if not kv[0].startswith("#"))))
    P("S line   : ref_rx=%s  pivots=%s  cbpin=%s  gfpin=%s  isb_datum=%s"
      % (s_line["ref_rx"], s_line["pivots"], s_line["cbpin"], s_line["gfpin"],
         s_line["isb_datum"]))
    P("           apriori=%s" % s_line["apriori_id"])
    P("")
    P("TRUTH AND SIGN CONVENTIONS ACTUALLY USED")
    P("  satellite CLOCK truth : truth_satclock.csv found in %d/%d station dirs"
      % (sc_files, len(stations)))
    if sc_files == 0:
        P("                          => INJECTED SATELLITE-CLOCK ERROR IS ZERO (the sim writes")
        P("                          that file only when sat_clk_err.active(), src/sim/main.cpp")
        P("                          :3052), so clk_true(s,t) == 0 and dC == the C record.")
    else:
        P("                          (%d cross-station disagreements)" % sc_disagree)
    P("  satellite OSB truth   : truth_bias.csv 'osb_m' = product_m + synth_m (the writer's")
    P("                          own comment: osb_m records the TOTAL -- 'that is what the")
    P("                          estimator must reproduce'), keyed on the PHASE RINEX code")
    P("                          (L1C/L2W/L5Q/L7Q); %d keys, %d cross-station disagreements."
      % (len(osb), osb_disagree))
    P("  applied in the sim as : L_b*lam = P + ... + bias_phase + rxb_phase + isb - I_b")
    P("                          + windup + lam*N + noise   (src/sim/main.cpp:2580)")
    P("                          with P = range + c*dt_r - c*sat_clk, so a satellite clock")
    P("                          error enters the observable NEGATED.")
    P("  consumer applies      : clock += C/c ADDED to the clock bias -> -C in the model")
    P("                          (SsrCorrectionStore::applyClockToBias doc + GnssCorrections")
    P("                          .cpp:346);  phase bias +1.0 * P_record  (kSsrPhaseBiasSign")
    P("                          = +1.0, MeasurementModel.cpp:1609/1624).")
    P("  => PRIMARY quantity   : W = dC - dP ('minus'), dC = C - clk_true, dP = P - osb_true.")
    P("     The task's literal E = dC + dP ('plus') is tabulated beside it; step 5 decides.")
    P("  receiver PHASE bias   : truth_rx_bias.csv carries only kind=code rows on this")
    P("                          fixture, so rxb_phase == 0 and an anchor handover moves the")
    P("                          gauge by an INTEGER only (no receiver-bias step).")
    P("  truth AMBIGUITY       : the true integer is TIME-DEPENDENT and is evaluated PER")
    P("                          EPOCH -- N(sta,sat,sig,t) = N_int + SUM(delta_cycles :")
    P("                          tow_s <= t) over truth_slips.csv, through")
    P("                          simnet_amb_integer.n_at (the accumulator simnet_dd_reref")
    P("                          and simnet_t4_census already use, not a second copy).")
    P("  truth_slips rows      : %d over %d stations, touching %d of %d (sta,sat,signal)"
      % (n_slip, n_amb_sta, len(tslips), len(tn_int)))
    P("                          keys = %.1f%%.  A QUIET bed (0 rows) collapses N(t) back to"
      % (100.0 * len(tslips) / max(len(tn_int), 1)))
    P("                          the static N_int, so steps 4/4a/4b reproduce the old")
    P("                          numbers there exactly; on a SLIPPED bed every one of them")
    P("                          read a stale integer after the first slip of the key.")
    if n_no_osb:
        P("  *** %d (sat,signal) P records had NO injected OSB truth: %s"
          % (sum(n_no_osb.values()), sorted(n_no_osb)[:6]))
    P("")
    P("  IF-LUMP (NOT folded into the clock truth -- see module docstring):")
    P("    the producer does not apply the .BIA, so the iono-free CODE-OSB lump over the")
    P("    cbpin pair is absorbed by the estimated clock.  Per-satellite, band-common, in")
    P("    metres -- i.e. exactly the range-like term r_s of the shape model below.")
    if lump:
        lv = np.array(sorted(lump.values()))
        P("    %d satellites, lump %.3f .. %.3f m, p50|.| %.3f m"
          % (len(lump), lv[0], lv[-1], float(np.median(np.abs(lv)))))
    else:
        P("    EMPTY (no injected code OSB, or the cbpin pair could not be resolved).")
    J["conventions"] = {
        "satclock_truth_files": sc_files, "satclock_zero": sc_files == 0,
        "osb_keys": len(osb), "osb_disagreements": osb_disagree,
        "truth_slip_rows": n_slip,
        "truth_amb_stations": n_amb_sta,
        "truth_amb_keys": len(tn_int),
        "truth_amb_keys_with_slips": len(tslips),
        "truth_integer": "N_int + sum(delta_cycles: tow_s <= t)  [simnet_amb_integer.n_at]",
        "primary": "W = dC - dP (dC = C - clk_true, dP = P - osb_phase_true)",
        "if_lump_n": len(lump),
        "if_lump_p50_abs": (float(np.median(np.abs(list(lump.values()))))
                            if lump else None),
        "cbpin": {k: list(v) for k, v in s_line.get("cbpin", {}).items()},
        "pivots": dict(s_line.get("pivots", {}))}
    P("")

    # ---- 1/2: per-band and lane distributions ------------------------------
    variants = {}
    for mode in ("minus", "plus"):
        per_ep, refs = wire_sd(wire, mode=mode)
        be, le = band_lane_records(per_ep)
        variants[mode] = {"per_ep": per_ep, "refs": refs, "band": be, "lane": le}
    per_ep_nl, _ = wire_sd(wire, mode="minus", clock_key="dCl")
    be_nl, _le_nl = band_lane_records(per_ep_nl)

    # consumer side, recomputed through consumer_float_truth_join's own code path
    cons_station = CJ.find_station(run, None)
    ctruthN, _ctruthLam, _c_slip = CJ.load_truth(fixture, cons_station)
    cper_epoch, _cweek = CJ.load_float(run)
    cres = CJ.compute(cper_epoch, ctruthN)

    P("=" * 128)
    P("STEP 1/2  --  BETWEEN-SATELLITE SD OF THE WIRE ERROR, in CYCLES of the named channel")
    P("  reference: one satellite per (epoch, constellation) = min SUM of emitted phase-bias")
    P("  sigma among satellites carrying a clock AND all three bands' P records (the wire's")
    P("  analogue of consumer_float_truth_join's min sum-of-float-std rule).")
    P("=" * 128)
    for wname, (lo, hi) in WINDOWS:
        P("")
        P("---- WINDOW %s ------------------------------------------------------------" % wname)
        P(CJ.hdr_line())
        P("-" * 128)
        wj = {}
        for const in ("G", "E"):
            for b in BANDS[const]:
                st = stat_block(win(variants["minus"]["band"][(const, b)], tow0, lo, hi))
                lbl = "WIRE %s %s" % (CONST_NAME[const], b)
                P(CJ.fmt_stat(lbl, st))
                wj[lbl] = st
            for lane in ("WL", "EWL", "NL", "NL_raw"):
                recs = variants["minus"]["lane"].get((const, lane))
                if not recs:
                    continue
                bhi, blo = LANES[const]["NL" if lane.startswith("NL") else lane]
                st = stat_block(win(recs, tow0, lo, hi), wrappable=(lane != "NL_raw"))
                nm = {"WL": "WL", "EWL": "EWL", "NL": "NL|int-removed",
                      "NL_raw": "NL|raw(gauge in)"}[lane]
                lbl = "WIRE %s %s(%s-%s)" % (CONST_NAME[const], nm, bhi, blo)
                P(CJ.fmt_stat(lbl, st))
                wj[lbl] = st
            P("-" * 128)
        P("uniform null                   -         -         -         -         -"
          " |   0.2500   0.2887   0.3000   0.5000 |  R_null    ---       ---")
        J.setdefault("wire_sd", {})[wname] = wj

    lo, hi = WINDOWS[-1][1]
    P("")
    P("---- the SAME table for the 'plus' convention E = dC + dP (settled window) ----")
    P(CJ.hdr_line())
    pj = {}
    for const in ("G", "E"):
        for b in BANDS[const]:
            st = stat_block(win(variants["plus"]["band"][(const, b)], tow0, lo, hi))
            lbl = "WIRE+ %s %s" % (CONST_NAME[const], b)
            P(CJ.fmt_stat(lbl, st))
            pj[lbl] = st
        for lane in ("WL", "EWL", "NL"):
            recs = variants["plus"]["lane"].get((const, lane))
            if recs:
                bhi, blo = LANES[const][lane]
                st = stat_block(win(recs, tow0, lo, hi))
                lbl = "WIRE+ %s %s(%s-%s)" % (CONST_NAME[const], lane, bhi, blo)
                P(CJ.fmt_stat(lbl, st))
                pj[lbl] = st
    J["wire_sd_plus_settled"] = pj

    P("")
    P("---- IF-LUMP-REMOVED clock diagnostic (dC - lump instead of dC), settled window ----")
    P(CJ.hdr_line())
    nj = {}
    for const in ("G", "E"):
        for b in BANDS[const]:
            st = stat_block(win(be_nl[(const, b)], tow0, lo, hi))
            lbl = "WIRE-lump %s %s" % (CONST_NAME[const], b)
            P(CJ.fmt_stat(lbl, st))
            nj[lbl] = st
    J["wire_sd_lumpremoved_settled"] = nj

    # ---- side by side with the consumer -----------------------------------
    P("")
    P("=" * 128)
    P("WIRE vs CONSUMER, SIDE BY SIDE  (p50 |wrapped fraction|; uniform null 0.2500)")
    P("  consumer numbers are recomputed here through consumer_float_truth_join's OWN")
    P("  compute()/stat_block(), so the columns differ only in what is being measured.")
    P("=" * 128)
    P("%-26s | %-33s | %-33s" % ("channel", "WIRE (dC - dP)", "CONSUMER float vs truth"))
    P("%-26s | %8s %8s %8s %7s | %8s %8s %8s %7s"
      % ("", "p50|fr|", "<0.15", "<0.25", "R", "p50|fr|", "<0.15", "<0.25", "R"))
    P("-" * 128)
    sbs = {}
    for wname, (lo_, hi_) in WINDOWS:
        P("-- %s --" % wname)
        for const in ("G", "E"):
            chans = [(b, variants["minus"]["band"][(const, b)],
                      cres["band_e"][(const, b)]) for b in BANDS[const]]
            chans += [(l, variants["minus"]["lane"][(const, l)],
                       cres["lane_e"][(const, l)]) for l in ("WL", "EWL", "NL")]
            for name, wrec, crec in chans:
                a = stat_block(win(wrec, tow0, lo_, hi_))
                b_ = stat_block(win(crec, cres["tow0"], lo_, hi_))
                if not a.get("n") or not b_.get("n"):
                    continue
                lbl = "%s %s" % (CONST_NAME[const], name)
                P("%-26s | %8.4f %8.4f %8.4f %7.4f | %8.4f %8.4f %8.4f %7.4f"
                  % (lbl, a["frac_p50"], a["frac_lt_015"], a["frac_lt_025"], a["R"],
                     b_["frac_p50"], b_["frac_lt_015"], b_["frac_lt_025"], b_["R"]))
                sbs.setdefault(wname, {})[lbl] = {
                    "wire": {k: a[k] for k in ("n", "frac_p50", "frac_lt_015",
                                               "frac_lt_025", "R", "R_null", "mu")},
                    "consumer": {k: b_[k] for k in ("n", "frac_p50", "frac_lt_015",
                                                    "frac_lt_025", "R", "R_null", "mu")}}
    J["side_by_side"] = sbs

    # ---- 1c: the undifferenced (ZD) channels -------------------------------
    P("")
    P("=" * 128)
    P("STEP 1c  --  the UNDIFFERENCED channels: is each emitted quantity an INTEGER number")
    P("             of cycles away from its own truth?")
    P("  dP/lam  = (P_record - osb_true)/lambda : the emitted phase bias against truth.  If")
    P("            the gauge is exactly an anchor receiver's INTEGER ambiguity (rxb_phase is")
    P("            zero on this fixture) this must sit ON an integer, and the wrapped part is")
    P("            the anchor's own FLOAT error -- the whole non-integer budget of the wire.")
    P("  dC/lam  = the emitted clock against truth, in cycles of the same band (it is NOT")
    P("            integer-natured -- shown only for scale).")
    P("  W/lam   = (dC - dP)/lambda : what a consumer's ZD ambiguity absorbs.  A ZD statistic")
    P("            still carries the CLOCK's per-satellite datum, which a between-satellite SD")
    P("            removes; it is the ZD non-integer content, not a fixability claim.")
    P("=" * 128)
    P("%-24s %8s %10s %10s %10s %10s %8s"
      % ("channel", "n", "p50|frac|", "<0.15", "<0.25", "R", "p50|.|[m]"))
    zd_j = {}
    lo_t = tow0 + settle * 3600.0
    for const in ("G", "E"):
        for b in BANDS[const]:
            lam = LAM[const][b]
            dp, dc, ww = [], [], []
            for w in wire:
                if w["tow"] < lo_t:
                    continue
                for (s, g), v in w["dP"].items():
                    if g != b or not s.startswith(const) or s not in w["dC"]:
                        continue
                    dp.append(v / lam)
                    dc.append(w["dC"][s] / lam)
                    ww.append((w["dC"][s] - v) / lam)
            for nm, arr in (("dP/lam", dp), ("dC/lam", dc), ("W/lam", ww)):
                a = np.asarray(arr, float)
                if a.size == 0:
                    continue
                f = np.abs(wrap(a))
                R, _mu, n = circ_R(a)
                P("%-24s %8d %10.4f %10.4f %10.4f %10.4f %8.3f"
                  % ("%s %s %s" % (CONST_NAME[const], b, nm), n,
                     float(np.median(f)), float(np.mean(f < 0.15)),
                     float(np.mean(f < 0.25)), R,
                     float(np.median(np.abs(a))) * lam))
                zd_j["%s %s %s" % (CONST_NAME[const], b, nm)] = {
                    "n": n, "frac_p50": float(np.median(f)),
                    "frac_lt_015": float(np.mean(f < 0.15)),
                    "frac_lt_025": float(np.mean(f < 0.25)), "R": R,
                    "R_null": math.sqrt(math.pi / (4.0 * n)),
                    "p50_abs_m": float(np.median(np.abs(a))) * lam}
        P("-" * 128)
    P("uniform null                    -     0.2500     0.3000     0.5000   ~R_null        -")
    J["zd_channels_settled"] = zd_j

    # ---- 3: shape decomposition -------------------------------------------
    P("")
    P("=" * 128)
    P("STEP 3  --  SHAPE of the wire's SD error across the three bands of a constellation")
    P("  model   dE_b = r + gamma_b*i + lambda_b*n_b + eps_b,  gamma_b = (f_L1/f_b)^2")
    P("  fitted on the WRAPPED fractions (the free integers make the fit a function of them")
    P("  alone), minimising sum_b [lambda_b*wrap(f_b - (r+gamma_b*i)/lambda_b)]^2 over the")
    P("  box r,i in [-3,3] m.  Solver: exact cell enumeration (every interior stationary")
    P("  point is the LS solution of some integer triple); cross-checked against the literal")
    P("  1 cm grid on a random subsample.")
    P("  *** IDENTIFIABILITY: 3 obs - 2 params - 3 free integers = ONE degree of freedom, and")
    P("  the box holds a few hundred integer cells, so the 2-parameter model fits almost")
    P("  ANY fraction triple.  Every share is therefore quoted against a size-matched")
    P("  MONTE-CARLO null of independent U(-0.5,0.5) fractions through the same solver; the")
    P("  GAP is the finding, never the raw share.  The 1-parameter fits carry 2 dof each and")
    P("  are the falsifiable ones.")
    P("=" * 128)
    P("  gamma:  %s" % "  ".join("%s %s=%.4f" % (CONST_NAME[c], b, GAMMA[c][b])
                                 for c in ("G", "E") for b in BANDS[c]))
    fitters = {c: ShapeFitter(c) for c in ("G", "E")}
    P("  cell candidates per sample: %s"
      % ", ".join("%s=%d" % (CONST_NAME[c], fitters[c].n_cand) for c in ("G", "E")))
    P("")

    shape_j = {}
    shape_series = {}          # (const, sat) -> [(tow, r, i, rms_cyc)]
    for wname, (lo_, hi_) in WINDOWS:
        lo_t, hi_t = tow0 + lo_ * 3600.0, tow0 + hi_ * 3600.0
        P("---- WINDOW %s ----" % wname)
        P("%-9s %-22s %7s %10s %10s %10s %10s %10s"
          % ("const", "model", "n", "res p50", "res p90", "<0.05cyc", "null<0.05", "GAP"))
        for const in ("G", "E"):
            F = fitters[const]
            keys, rows = [], []
            for k, (t, row) in enumerate(sorted(variants["minus"]["per_ep"].items())):
                if not (lo_t <= t < hi_t) or (k % max(args.shape_decim, 1)):
                    continue
                if const not in row:
                    continue
                for s, per_b in row[const]["e"].items():
                    keys.append((t, s))
                    rows.append([float(wrap(per_b[b])) for b in BANDS[const]])
            if len(rows) < 10:
                continue
            f = np.asarray(rows, float)
            fn = rng.uniform(-0.5, 0.5, size=f.shape)      # size-matched uniform null

            out = {}
            for tag, fun in (("full (r,i)", "full"), ("range-only (r)", "r"),
                             ("iono-only (i)", "i"), ("null (0,0)", "null")):
                if fun == "full":
                    r, i, c, res = F.fit_full(f)
                    rn, ino, cn, resn = F.fit_full(fn)
                elif fun == "null":
                    c, res = F.fit_null(f)
                    cn, resn = F.fit_null(fn)
                    r = i = np.zeros(f.shape[0])
                    rn = ino = np.zeros(f.shape[0])
                else:
                    v, c, res = F.fit_1p(f, fun)
                    vn, cn, resn = F.fit_1p(fn, fun)
                    r = v if fun == "r" else np.zeros(f.shape[0])
                    i = v if fun == "i" else np.zeros(f.shape[0])
                    rn = vn if fun == "r" else np.zeros(f.shape[0])
                    ino = vn if fun == "i" else np.zeros(f.shape[0])
                rms = res_rms_cyc(res)
                rmsn = res_rms_cyc(resn)
                sh = float(np.mean(rms < 0.05))
                shn = float(np.mean(rmsn < 0.05))
                P("%-9s %-22s %7d %10.4f %10.4f %10.4f %10.4f %+10.4f"
                  % (CONST_NAME[const], tag, f.shape[0], pctl(rms, 50), pctl(rms, 90),
                     sh, shn, sh - shn))
                out[tag] = {"n": int(f.shape[0]), "res_p50_cyc": pctl(rms, 50),
                            "res_p90_cyc": pctl(rms, 90), "share_lt_005": sh,
                            "null_share_lt_005": shn, "gap": sh - shn,
                            "abs_r_p50": pctl(np.abs(r), 50),
                            "abs_r_p90": pctl(np.abs(r), 90),
                            "abs_i_p50": pctl(np.abs(i), 50),
                            "abs_i_p90": pctl(np.abs(i), 90),
                            "null_abs_r_p50": pctl(np.abs(rn), 50),
                            "null_abs_i_p50": pctl(np.abs(ino), 50)}
                if fun == "full":
                    full_r, full_i, full_rms = r, i, rms
            P("%-9s %-22s   |r| p50 %.3f m  p90 %.3f m   |i| p50 %.3f m  p90 %.3f m"
              % ("", "  full fit magnitudes", pctl(np.abs(full_r), 50),
                 pctl(np.abs(full_r), 90), pctl(np.abs(full_i), 50),
                 pctl(np.abs(full_i), 90)))
            P("%-9s %-22s   |r| p50 %.3f m   |i| p50 %.3f m   (uniform-null fit magnitudes)"
              % ("", "", out["full (r,i)"]["null_abs_r_p50"],
                 out["full (r,i)"]["null_abs_i_p50"]))
            shape_j.setdefault(wname, {})[CONST_NAME[const]] = out

            # literal-grid cross-check on a random subsample
            if args.grid_check > 0 and f.shape[0] > 0:
                m = min(args.grid_check, f.shape[0])
                pick = rng.choice(f.shape[0], size=m, replace=False)
                _gr, _gi, gc = F.grid_full(f[pick])
                cell_cost = np.array([np.sum((wrap(f[p] - (full_r[p] + full_i[p] * F.gam)
                                                   / F.lam) * F.lam) ** 2)
                                      for p in pick])
                worse = float(np.mean(gc < cell_cost - 1e-9))
                P("%-9s grid check: %d samples, literal 1 cm grid beats the exact cell"
                  % ("", m))
                P("%-9s solver on %.4f of them (expect ~0; a nonzero share means the box"
                  % ("", worse))
                P("%-9s boundary bound the optimum); median |cost_grid - cost_cell| = %.3e m^2"
                  % ("", float(np.median(np.abs(gc - cell_cost)))))
                shape_j[wname][CONST_NAME[const]]["_grid_check"] = {
                    "n": int(m), "share_grid_better": worse,
                    "median_abs_cost_diff_m2": float(np.median(np.abs(gc - cell_cost)))}

            # time series of the full fit, for the step-6 overlay
            if wname == WINDOWS[-1][0] or True:
                for (t, s), rr, ii, rm in zip(keys, full_r, full_i, full_rms):
                    shape_series.setdefault((const, s), []).append((t, rr, ii, rm))
        P("")
    J["shape"] = shape_j

    # -- time behaviour of the fitted r_s, i_s -------------------------------
    P("---- time behaviour of the fitted (r_s, i_s), settled window ----")
    P("  per satellite: consecutive-sample |dr|, |di| (decimated at %d x 30 s) and the"
      % args.shape_decim)
    P("  within-tenure std between that satellite's PBGAUGE / PHASE_ANCHOR events, plus the")
    P("  step across those events.  NOTE r_s, i_s are SD quantities: they carry the")
    P("  reference satellite's own error too." % ())
    ev_by_sat = defaultdict(set)
    for r in events.get("PBGAUGE", []):
        ev_by_sat[r["sat"].split("_")[0]].add(round(float(r["tow"]), 3))
    for r in events.get("PHASE_ANCHOR", []):
        ev_by_sat[r["sat"]].add(round(float(r["tow"]), 3))
    lo_t = tow0 + settle * 3600.0
    tb = {}
    P("%-9s %8s %10s %10s %10s %10s %10s %10s"
      % ("const", "n_sat", "|dr| p50", "|di| p50", "tenure sd r", "tenure sd i",
         "|step r|", "|step i|"))
    for const in ("G", "E"):
        dr, di, tsr, tsi, spr, spi, nsat = [], [], [], [], [], [], 0
        for (c, s), ser in sorted(shape_series.items()):
            if c != const:
                continue
            ser = [x for x in ser if x[0] >= lo_t]
            if len(ser) < 5:
                continue
            nsat += 1
            ser.sort()
            t = np.array([x[0] for x in ser])
            r = np.array([x[1] for x in ser])
            i = np.array([x[2] for x in ser])
            dr.extend(np.abs(np.diff(r)))
            di.extend(np.abs(np.diff(i)))
            marks = sorted(x for x in ev_by_sat.get(s, ()) if x >= lo_t)
            edges = [0] + [int(np.searchsorted(t, m)) for m in marks] + [len(t)]
            segs = []
            for a, b in zip(edges[:-1], edges[1:]):
                if b - a >= 4:
                    segs.append((r[a:b], i[a:b]))
            for rr, ii in segs:
                tsr.append(float(np.std(rr)))
                tsi.append(float(np.std(ii)))
            for k in range(len(segs) - 1):
                spr.append(abs(float(np.mean(segs[k + 1][0]) - np.mean(segs[k][0]))))
                spi.append(abs(float(np.mean(segs[k + 1][1]) - np.mean(segs[k][1]))))
        if nsat:
            P("%-9s %8d %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f"
              % (CONST_NAME[const], nsat, pctl(dr, 50), pctl(di, 50), pctl(tsr, 50),
                 pctl(tsi, 50), pctl(spr, 50), pctl(spi, 50)))
            tb[CONST_NAME[const]] = {
                "n_sat": nsat, "dr_p50": pctl(dr, 50), "di_p50": pctl(di, 50),
                "tenure_std_r_p50": pctl(tsr, 50), "tenure_std_i_p50": pctl(tsi, 50),
                "step_r_p50": pctl(spr, 50), "step_i_p50": pctl(spi, 50),
                "n_tenures": len(tsr), "n_steps": len(spr)}
    J["shape_time"] = tb

    # ---- 3b: integer-free shape decomposition on the time difference -------
    P("")
    P("=" * 128)
    P("STEP 3b  --  the SAME shape question, made IDENTIFIABLE: consecutive-epoch DIFFERENCE")
    P("  The step-3 nulls above show the wrapped 2-parameter fit is vacuous (a uniform")
    P("  fraction triple fits it too), so the shape of the LEVEL is not identifiable at three")
    P("  bands.  Its DYNAMICS are: the gauge integers are arc-constant, so a 30 s difference")
    P("  of the SD error in METRES cancels them EXACTLY, leaving 3 observations, 2 parameters")
    P("  and one degree of freedom with no integers at all.")
    P("    d(dE_b) = dr + gamma_b*di + d(eps_b)")
    P("  R^2 = 1 - SS_res/SS_total is then a real variance decomposition: how much of the")
    P("  wire's SD-error MOTION is band-common (range-like), gamma-scaled (iono-like), both,")
    P("  or independent per band.  One FIXED reference per constellation per window; samples")
    P("  are consecutive 30 s pairs with no PBGAUGE/PHASE_ANCHOR mark on the satellite or the")
    P("  reference, and |d(dE_b)| < lambda_b/2 on every band (an integer jump would otherwise")
    P("  enter as a huge 'independent' residual -- the excluded share is reported).")
    P("=" * 128)
    fixed_by_window = {}
    td_j = {}
    for wname, (lo_, hi_) in WINDOWS:
        lo_t, hi_t = tow0 + lo_ * 3600.0, tow0 + hi_ * 3600.0
        cnt = defaultdict(Counter)
        for t, row in variants["minus"]["per_ep"].items():
            if lo_t <= t < hi_t:
                for const, d in row.items():
                    cnt[const][d["ref"]] += 1
                    for s in d["e"]:
                        cnt[const][s] += 1
        fixed = {c: cnt[c].most_common(1)[0][0] for c in cnt if cnt[c]}
        pe_fixed, _ = wire_sd(wire, mode="minus", fixed_ref=fixed)
        fixed_by_window[wname] = (fixed, pe_fixed)
        P("-- %s   fixed reference: %s --"
          % (wname, ", ".join("%s=%s" % (CONST_NAME[c], fixed[c]) for c in sorted(fixed))))
        P("%-9s %-18s %8s %11s %11s %9s %11s"
          % ("const", "model", "n", "res rms[m]", "res rms[cyc]", "R^2", "|param| p50"))
        for const in ("G", "E"):
            if const not in fixed:
                continue
            lam = np.array([LAM[const][b] for b in BANDS[const]])
            gam = np.array([GAMMA[const][b] for b in BANDS[const]])
            ser = defaultdict(list)
            for t, row in pe_fixed.items():
                if not (lo_t <= t < hi_t) or const not in row:
                    continue
                if row[const]["ref"] != fixed[const]:
                    continue
                for s, per_b in row[const]["e"].items():
                    ser[s].append((t, np.array([per_b[b] * LAM[const][b]
                                                for b in BANDS[const]])))
            marks_ref = set(x for x in ev_by_sat.get(fixed[const], ()))
            rows, n_jump = [], 0
            for s, pts in ser.items():
                pts.sort()
                mk = marks_ref | set(ev_by_sat.get(s, ()))
                for k in range(1, len(pts)):
                    t0, v0 = pts[k - 1]
                    t1, v1 = pts[k]
                    if abs(t1 - t0 - 30.0) > 1.0 or t1 in mk or t0 in mk:
                        continue
                    d = v1 - v0
                    if np.any(np.abs(d) > 0.5 * lam):
                        n_jump += 1
                        continue
                    rows.append(d)
            if len(rows) < 20:
                continue
            D = np.asarray(rows)
            models, ss_tot = linear_shape(D, gam)
            for tag, m in models.items():
                res = m["res"]
                rms_m = float(np.sqrt(np.mean(np.sum(res ** 2, axis=1) / 3.0)))
                rms_c = float(np.sqrt(np.mean(np.mean((res / lam) ** 2, axis=1))))
                r2 = 1.0 - m["ss"] / ss_tot
                pp = m["p"]
                if pp is None:
                    pmag = float("nan")
                elif pp.ndim == 2:
                    pmag = float(np.median(np.abs(pp[:, 0])))
                else:
                    pmag = float(np.median(np.abs(pp)))
                P("%-9s %-18s %8d %11.5f %11.5f %9.4f %11.5f"
                  % (CONST_NAME[const], tag, D.shape[0], rms_m, rms_c, r2, pmag))
                td_j.setdefault(wname, {}).setdefault(CONST_NAME[const], {})[tag] = {
                    "n": int(D.shape[0]), "res_rms_m": rms_m, "res_rms_cyc": rms_c,
                    "r2": float(r2), "param_abs_p50_m": pmag}
            full = models["full (r,i)"]["p"]
            P("%-9s %-18s   |dr| p50 %.5f m  p90 %.5f m   |di| p50 %.5f m  p90 %.5f m"
              % ("", "  full params", pctl(np.abs(full[:, 0]), 50),
                 pctl(np.abs(full[:, 0]), 90), pctl(np.abs(full[:, 1]), 50),
                 pctl(np.abs(full[:, 1]), 90)))
            P("%-9s %-18s   excluded as integer jumps: %d (%.4f of candidate pairs)"
              % ("", "", n_jump, n_jump / max(n_jump + D.shape[0], 1)))
            td_j[wname][CONST_NAME[const]]["_n_integer_jump_excluded"] = n_jump
        P("")
    J["shape_time_differenced"] = td_j

    # ---- 4: anchor-gauge unwrap -------------------------------------------
    P("")
    P("=" * 128)
    P("STEP 4  --  ANCHOR-GAUGE UNWRAP")
    P("  logs/network_phase_anchor.csv gives the anchoring station of every (tow, sat,")
    P("  signal) directly (header week,tow,cons,signal,sat,anchor_sta -- one row per emitted")
    P("  P record).  If the producer's SD non-integer offset is exactly the anchor's own")
    P("  float error at pin time, then removing lambda_b*(N_anchor(s),s,b - N_anchor(ref),")
    P("  ref,b) -- both integers taken from the slip-accumulated truth N(t) at THIS epoch --")
    P("  must leave a SMALL residue.  The sign of the gauge in the emitted bias is not")
    P("  assumed: both are evaluated and the one that collapses is reported.")
    P("=" * 128)
    anch_j = {}
    if anchors is None:
        P("  network_phase_anchor.csv ABSENT -- step 4 skipped.")
    else:
        for wname, (lo_, hi_) in WINDOWS:
            lo_t, hi_t = tow0 + lo_ * 3600.0, tow0 + hi_ * 3600.0
            P("-- %s --" % wname)
            P("%-16s %8s %10s %10s %10s %10s %10s %9s"
              % ("channel", "n", "p50|e|cyc", "-gauge p50", "+gauge p50",
                 "-g<0.2cyc", "-g p50 [m]", "no_anchor"))
            for const in ("G", "E"):
                for b in BANDS[const]:
                    raw, um, up, miss = [], [], [], 0
                    for t, row in variants["minus"]["per_ep"].items():
                        if not (lo_t <= t < hi_t) or const not in row:
                            continue
                        ref = row[const]["ref"]
                        ar = anchors.get((t, ref, b))
                        Nr = n_at(tn_int, tslips, ar, ref, b, t) if ar else None
                        if Nr is None:
                            miss += len(row[const]["e"])
                            continue
                        for s, per_b in row[const]["e"].items():
                            a = anchors.get((t, s, b))
                            Ns = n_at(tn_int, tslips, a, s, b, t) if a else None
                            if Ns is None:
                                miss += 1
                                continue
                            g = float(Ns - Nr)
                            e = per_b[b]
                            raw.append(e)
                            um.append(e - g)
                            up.append(e + g)
                    if not raw:
                        continue
                    am = np.abs(np.asarray(um))
                    P("%-16s %8d %10.4f %10.4f %10.4f %10.4f %10.4f %9d"
                      % ("%s %s" % (CONST_NAME[const], b), len(raw),
                         float(np.median(np.abs(wrap(raw)))),
                         float(np.median(am)), float(np.median(np.abs(up))),
                         float(np.mean(am < 0.2)),
                         float(np.median(am)) * LAM[const][b], miss))
                    anch_j.setdefault(wname, {})["%s %s" % (CONST_NAME[const], b)] = {
                        "n": len(raw), "raw_frac_p50": float(np.median(np.abs(wrap(raw)))),
                        "minus_gauge_p50_cyc": float(np.median(am)),
                        "plus_gauge_p50_cyc": float(np.median(np.abs(up))),
                        "minus_gauge_lt_02": float(np.mean(am < 0.2)),
                        "minus_gauge_p50_m": float(np.median(am)) * LAM[const][b],
                        "n_no_anchor": miss}
            P("")
        # how often does the anchor of s differ from the anchor of the reference?
        same, diff, n_st = 0, 0, Counter()
        for t, row in variants["minus"]["per_ep"].items():
            for const, d in row.items():
                for b in BANDS[const]:
                    ar = anchors.get((t, d["ref"], b))
                    for s in d["e"]:
                        a = anchors.get((t, s, b))
                        if a and ar:
                            n_st[a] += 1
                            if a == ar:
                                same += 1
                            else:
                                diff += 1
        P("  anchor of the satellite == anchor of the reference in %.4f of chords"
          % (same / max(same + diff, 1)))
        P("  distinct anchoring stations seen: %d  (top: %s)"
          % (len(n_st), ", ".join("%s:%d" % kv for kv in n_st.most_common(5))))
        anch_j["anchor_same_as_ref_share"] = same / max(same + diff, 1)
        anch_j["n_anchor_stations"] = len(n_st)
    J["anchor_unwrap"] = anch_j

    # ---- 4a: gauge identification -----------------------------------------
    P("")
    P("=" * 128)
    P("STEP 4a  --  GAUGE IDENTIFICATION: whose integer does the emitted phase bias carry?")
    P("  Step 4 removes the CURRENT anchor's truth integer.  If the producer's sticky")
    P("  handover keeps the frame of an EARLIER anchor (953 continuous vs 82 cold handovers")
    P("  on this run), that is the wrong station.  So instead of assuming, INVERT: the")
    P("  implied gauge integer is n = round(-dP/lambda) (or +), and we ask which of the 61")
    P("  stations' truth actually holds that integer for this (sat, band) AT THIS EPOCH.")
    P("  A gauge that is any receiver's integer ambiguity must hit one of them.")
    P("  The N -> stations index is itself TIME-DEPENDENT -- a slip moves a station from")
    P("  one N bucket to another -- so it is rebuilt per epoch and memoised by tow.  On a")
    P("  quiet bed every epoch's index is the one the old static build produced.")
    P("=" * 128)
    byN_cache = {}                                     # tow -> {(sat,sig): {N: [stations]}}

    def byN_at(tow):
        """(sat,sig) -> N -> [stations], every N the slip-accumulated truth at `tow`."""
        idx = byN_cache.get(tow)
        if idx is None:
            idx = defaultdict(lambda: defaultdict(list))
            for (st, sat, sig) in tn_int:
                idx[(sat, sig)][n_at(tn_int, tslips, st, sat, sig, tow)].append(st)
            byN_cache[tow] = idx
        return idx

    gid = {}
    DEC = 20
    for const in ("G", "E"):
        for b in BANDS[const]:
            lam = LAM[const][b]
            tot = near = hit_anchor = hit_any = 0
            who = Counter()
            span = []
            for k, w in enumerate(wire):
                if k % DEC or w["tow"] < lo_t:
                    continue
                idx = byN_at(w["tow"])                 # the truth N buckets at THIS epoch
                for (s, g), v in w["dP"].items():
                    if g != b or not s.startswith(const):
                        continue
                    tot += 1
                    q = v / lam
                    if abs(float(wrap(q))) < 0.25:
                        near += 1
                    for sgn in (-1.0, +1.0):
                        n = int(round(sgn * q))
                        sts = idx[(s, b)].get(n)
                        if sts:
                            hit_any += 1
                            who.update(sts)
                            a = anchors.get((w["tow"], s, b)) if anchors else None
                            if a in sts:
                                hit_anchor += 1
                            break
                    span.append(q)
            if not tot:
                continue
            sp = np.asarray(span)
            P("  %-12s n=%-7d |dP/lam| p50 %8.2f cyc (range %+.0f..%+.0f) | near-integer"
              % ("%s %s" % (CONST_NAME[const], b), tot, float(np.median(np.abs(sp))),
                 sp.min(), sp.max()))
            P("  %-12s   share |wrap| < 0.25 : %.4f   implied integer == SOME station's"
              % ("", near / tot))
            P("  %-12s   truth N : %.4f   == the CURRENT anchor's : %.4f"
              % ("", hit_any / tot, hit_anchor / tot))
            if who:
                P("  %-12s   top matching stations: %s"
                  % ("", ", ".join("%s:%d" % kv for kv in who.most_common(4))))
            gid["%s %s" % (CONST_NAME[const], b)] = {
                "n": tot, "share_near_integer": near / tot,
                "share_integer_is_some_station_N": hit_any / tot,
                "share_integer_is_current_anchor_N": hit_anchor / tot,
                "abs_dP_over_lam_p50_cyc": float(np.median(np.abs(sp))),
                "top_stations": dict(who.most_common(5))}
    J["gauge_identification"] = gid

    # ---- 4c: same-anchor vs cross-anchor chords ---------------------------
    if anchors is not None:
        P("")
        P("=" * 128)
        P("STEP 4c  --  the MECHANISM test: SAME-ANCHOR vs CROSS-ANCHOR chords")
        P("  network.phase_datum_prioritized pins each (sat, signal) phase bias at its OWN")
        P("  highest-coverage receiver, so two satellites in one SD chord are usually gauged")
        P("  at DIFFERENT stations and each contributes that station's own float ambiguity")
        P("  error.  When the SAME station anchors both, everything station-specific cancels")
        P("  and the chord is the wire's internal between-satellite consistency alone.  Split")
        P("  the identical population on that one bit and compare.")
        P("=" * 128)
        P("%-14s | %-31s | %-31s"
          % ("channel", "SAME anchor as the reference", "DIFFERENT anchor"))
        P("  *** the SAME-anchor population is small and CORRELATED (few satellites, long")
        P("  runs of consecutive epochs), so n overstates the effective sample size and a")
        P("  large R at a large |mu| is a COMMON NON-INTEGER OFFSET, not integer content.")
        P("  The distinct (satellite, tenure) count is printed beside it.")
        P("%-14s | %6s %4s %7s %7s %7s %7s | %6s %7s %7s %7s"
          % ("", "n", "nsat", "p50|fr|", "<0.15", "R", "mu", "n", "p50|fr|", "<0.15", "R"))
        aab = {}
        lo_t3 = tow0 + settle * 3600.0
        for const in ("G", "E"):
            for b in BANDS[const]:
                same_v, diff_v = [], []
                same_sats = set()
                for t, row in variants["minus"]["per_ep"].items():
                    if t < lo_t3 or const not in row:
                        continue
                    ref = row[const]["ref"]
                    ar = anchors.get((t, ref, b))
                    if ar is None:
                        continue
                    for s, per_b in row[const]["e"].items():
                        a = anchors.get((t, s, b))
                        if a is None:
                            continue
                        if a == ar:
                            same_v.append(per_b[b])
                            same_sats.add((s, ref, a))
                        else:
                            diff_v.append(per_b[b])
                cells = []
                for arr in (same_v, diff_v):
                    if len(arr) < 20:
                        cells.append(None)
                        continue
                    f = np.abs(wrap(arr))
                    R, mu, n = circ_R(arr)
                    cells.append({"n": n, "frac_p50": float(np.median(f)),
                                  "frac_lt_015": float(np.mean(f < 0.15)), "R": R,
                                  "mu": mu, "R_null": math.sqrt(math.pi / (4.0 * n))})
                if cells[0] is None or cells[1] is None:
                    continue
                cells[0]["n_distinct_sat_ref_anchor"] = len(same_sats)
                P("%-14s | %6d %4d %7.4f %7.4f %7.4f %+7.4f | %6d %7.4f %7.4f %7.4f"
                  % ("%s %s" % (CONST_NAME[const], b),
                     cells[0]["n"], len(same_sats), cells[0]["frac_p50"],
                     cells[0]["frac_lt_015"], cells[0]["R"], cells[0]["mu"],
                     cells[1]["n"], cells[1]["frac_p50"], cells[1]["frac_lt_015"],
                     cells[1]["R"]))
                aab["%s %s" % (CONST_NAME[const], b)] = {
                    "same_anchor": cells[0], "diff_anchor": cells[1]}
        P("  uniform null: p50|fr| 0.2500, <0.15 0.3000, R ~ sqrt(pi/4n)")
        J["anchor_match_ab"] = aab

    # ---- 4b: shape of the anchor-unwrapped level (integer-free) ------------
    if anchors is not None:
        settled_name = WINDOWS[-1][0]
        pick_minus = None
        if anch_j.get(settled_name):
            mm = np.mean([v["minus_gauge_p50_cyc"] for v in anch_j[settled_name].values()])
            pp = np.mean([v["plus_gauge_p50_cyc"] for v in anch_j[settled_name].values()])
            pick_minus = mm <= pp
        P("")
        P("=" * 128)
        P("STEP 4b  --  SHAPE of the ANCHOR-UNWRAPPED LEVEL (integer-free, identifiable)")
        P("  Once the anchor gauge integers are removed with truth there are NO free integers")
        P("  left, so the level's own shape becomes a 1-dof regression exactly like 3b:")
        P("    dE_b - lambda_b*gauge_b = r + gamma_b*i + eps_b")
        P("  This is the version of step 3 that can actually be falsified.")
        P("  gauge sign selected empirically: %s"
          % ("-(N_s - N_ref)" if pick_minus else "+(N_s - N_ref)"))
        P("=" * 128)
        P("%-9s %-18s %8s %11s %11s %9s %11s"
          % ("const", "model", "n", "res rms[m]", "res rms[cyc]", "R^2", "|param| p50"))
        ul_j = {}
        for wname, (lo_, hi_) in WINDOWS:
            lo_t, hi_t = tow0 + lo_ * 3600.0, tow0 + hi_ * 3600.0
            for const in ("G", "E"):
                lam = np.array([LAM[const][b] for b in BANDS[const]])
                gam = np.array([GAMMA[const][b] for b in BANDS[const]])
                rows = []
                for t, row in variants["minus"]["per_ep"].items():
                    if not (lo_t <= t < hi_t) or const not in row:
                        continue
                    ref = row[const]["ref"]
                    gref = {}
                    ok = True
                    for b in BANDS[const]:
                        a = anchors.get((t, ref, b))
                        N = n_at(tn_int, tslips, a, ref, b, t) if a else None
                        if N is None:
                            ok = False
                            break
                        gref[b] = N
                    if not ok:
                        continue
                    for s, per_b in row[const]["e"].items():
                        v, ok2 = [], True
                        for b in BANDS[const]:
                            a = anchors.get((t, s, b))
                            N = n_at(tn_int, tslips, a, s, b, t) if a else None
                            if N is None:
                                ok2 = False
                                break
                            g = float(N - gref[b]) * (1.0 if pick_minus else -1.0)
                            v.append((per_b[b] - g) * LAM[const][b])
                        if ok2:
                            rows.append(v)
                if len(rows) < 20:
                    continue
                D = np.asarray(rows, float)
                models, ss_tot = linear_shape(D, gam)
                P("-- %s %s (n=%d, |level| p50 %.4f m) --"
                  % (wname, CONST_NAME[const], D.shape[0],
                     float(np.median(np.abs(D)))))
                for tag, m in models.items():
                    res = m["res"]
                    rms_m = float(np.sqrt(np.mean(np.sum(res ** 2, axis=1) / 3.0)))
                    rms_c = float(np.sqrt(np.mean(np.mean((res / lam) ** 2, axis=1))))
                    r2 = 1.0 - m["ss"] / ss_tot
                    pp2 = m["p"]
                    if pp2 is None:
                        pmag = float("nan")
                    elif pp2.ndim == 2:
                        pmag = float(np.median(np.abs(pp2[:, 0])))
                    else:
                        pmag = float(np.median(np.abs(pp2)))
                    P("%-9s %-18s %8d %11.5f %11.5f %9.4f %11.5f"
                      % (CONST_NAME[const], tag, D.shape[0], rms_m, rms_c, r2, pmag))
                    ul_j.setdefault(wname, {}).setdefault(
                        CONST_NAME[const], {})[tag] = {
                            "n": int(D.shape[0]), "res_rms_m": rms_m,
                            "res_rms_cyc": rms_c, "r2": float(r2),
                            "param_abs_p50_m": pmag}
                full = models["full (r,i)"]["p"]
                P("%-9s %-18s   |r| p50 %.4f m p90 %.4f m   |i| p50 %.4f m p90 %.4f m"
                  % ("", "  full params", pctl(np.abs(full[:, 0]), 50),
                     pctl(np.abs(full[:, 0]), 90), pctl(np.abs(full[:, 1]), 50),
                     pctl(np.abs(full[:, 1]), 90)))
                # per-band residual in cycles after the full fit (what a fixer sees)
                rc = models["full (r,i)"]["res"] / lam
                P("%-9s %-18s   per-band residual after (r,i): %s"
                  % ("", "", "  ".join("%s p50|.| %.4f cyc" % (b, pctl(np.abs(rc[:, k]), 50))
                                       for k, b in enumerate(BANDS[const]))))
        J["unwrapped_level_shape"] = ul_j

    # ---- 5: consistency with the consumer ---------------------------------
    P("")
    P("=" * 128)
    P("STEP 5  --  DOES THE CONSUMER INHERIT THE WIRE'S OFFSET?  (and which sign is right)")
    P("  Both sides are re-differenced against the SAME reference -- the CONSUMER's own")
    P("  per-epoch rule (min sum-of-float-std among satellites present in the consumer's")
    P("  float AND truth AND the wire on every band) -- so the join is exact.")
    P("  consumer per-(s,b,t) SD residue  e_c = (v_s - v_ref)/lam - (N_s - N_ref)")
    P("  wire     per-(s,b,t) SD residue  e_w = (W_s - W_ref)/lam,  W = dC - dP ('minus')")
    P("                                or  (E_s - E_ref)/lam,  E = dC + dP ('plus')")
    P("  wrap(e_c - e_w) ~ 0 => the consumer inherits the wire EXACTLY and the convention is")
    P("  right; wrap(e_c + e_w) ~ 0 => the relative sign is inverted; both uniform => the")
    P("  consumer's offset is its OWN, not the wire's.")
    P("=" * 128)
    Wmap = {}
    for wsel, sgn in (("minus", -1.0), ("plus", +1.0)):
        m = {}
        for w in wire:
            d = {}
            for (s, b), v in w["dP"].items():
                if s in w["dC"]:
                    d[(s, b)] = w["dC"][s] + sgn * v
            m[w["tow"]] = d
        Wmap[wsel] = m

    join = defaultdict(lambda: defaultdict(list))
    n_ep_joined = 0
    for ctow, ep in sorted(cper_epoch.items()):
        t = round(ctow, 3)
        if t not in Wmap["minus"]:
            continue
        wm, wp = Wmap["minus"][t], Wmap["plus"][t]
        used = False
        for const, bands in BANDS.items():
            cand = [s for s in {a for (a, _g) in ep if a.startswith(const)}
                    if all((s, b) in ep and (s, b) in ctruthN and (s, b) in wm
                           for b in bands)]
            if len(cand) < 2:
                continue
            ref = min(cand, key=lambda s: (sum(ep[(s, b)][1] for b in bands), s))
            used = True
            for s in cand:
                if s == ref:
                    continue
                for b in bands:
                    lam = LAM[const][b]
                    ec = ((ep[(s, b)][0] - ep[(ref, b)][0]) / lam
                          - (ctruthN[(s, b)] - ctruthN[(ref, b)]))
                    ew_m = (wm[(s, b)] - wm[(ref, b)]) / lam
                    ew_p = (wp[(s, b)] - wp[(ref, b)]) / lam
                    join[(const, b)]["t"].append(t)
                    join[(const, b)]["ec"].append(ec)
                    join[(const, b)]["ewm"].append(ew_m)
                    join[(const, b)]["ewp"].append(ew_p)
                    join[(const, b)]["sat"].append(s)
        n_ep_joined += int(used)
    P("  joined %d epochs" % n_ep_joined)
    P("")
    jj = {}
    for wname, (lo_, hi_) in WINDOWS:
        lo_t, hi_t = tow0 + lo_ * 3600.0, tow0 + hi_ * 3600.0
        P("-- %s --" % wname)
        P("%-14s %7s | %8s %8s | %8s %8s %8s | %8s %8s | %7s"
          % ("channel", "n", "p50|ec|", "p50|ew|", "p50|c-w|", "R(c-w)", "R_null",
             "p50|c+w|", "R(c+w)", "circorr"))
        for const in ("G", "E"):
            for b in BANDS[const]:
                d = join.get((const, b))
                if not d:
                    continue
                t = np.asarray(d["t"])
                m = (t >= lo_t) & (t < hi_t)
                if m.sum() < 10:
                    continue
                ec = wrap(np.asarray(d["ec"])[m])
                ew = wrap(np.asarray(d["ewm"])[m])
                ewp = wrap(np.asarray(d["ewp"])[m])
                dm = wrap(ec - ew)
                dp = wrap(ec + ew)
                Rm, mum, n = circ_R(dm)
                Rp, _mup, _ = circ_R(dp)
                Rnull = math.sqrt(math.pi / (4.0 * n))
                cc = circ_corr(ec, ew)
                P("%-14s %7d | %8.4f %8.4f | %8.4f %8.4f %8.4f | %8.4f %8.4f | %+7.4f"
                  % ("%s %s" % (CONST_NAME[const], b), n,
                     float(np.median(np.abs(ec))), float(np.median(np.abs(ew))),
                     float(np.median(np.abs(dm))), Rm, Rnull,
                     float(np.median(np.abs(dp))), Rp, cc))
                jj.setdefault(wname, {})["%s %s" % (CONST_NAME[const], b)] = {
                    "n": n, "p50_abs_ec": float(np.median(np.abs(ec))),
                    "p50_abs_ew_minus": float(np.median(np.abs(ew))),
                    "p50_abs_diff_minus": float(np.median(np.abs(dm))),
                    "R_diff_minus": Rm, "mu_diff_minus": mum, "R_null": Rnull,
                    "p50_abs_sum": float(np.median(np.abs(dp))), "R_sum": Rp,
                    "circ_corr": cc,
                    "p50_abs_ew_plus": float(np.median(np.abs(ewp)))}
        P("")
    P("---- the SAME join, UNWRAPPED (the decisive test for the GAUGE INTEGER) ----")
    P("  e_c and e_w each carry the producer's gauge integer -- tens of cycles.  If the")
    P("  consumer inherits the wire, e_c - e_w is the consumer's OWN float error and must be")
    P("  SMALL and UNWRAPPED-small (<< 1 cycle); the wrapped test above cannot see that")
    P("  because a ~0.3 cyc float error already fills the circle.  If instead the integers")
    P("  disagree, |e_c - e_w| is of order the gauge itself.")
    P("%-14s %7s | %10s %10s %10s %9s %9s %9s"
      % ("channel", "n", "p50|c-w|", "p90|c-w|", "std", "<0.5cyc", "<1cyc", "<3cyc"))
    unw = {}
    lo_t2, hi_t2 = tow0 + WINDOWS[-1][1][0] * 3600.0, tow0 + WINDOWS[-1][1][1] * 3600.0
    for const in ("G", "E"):
        for b in BANDS[const]:
            d = join.get((const, b))
            if not d:
                continue
            t = np.asarray(d["t"])
            m = (t >= lo_t2) & (t < hi_t2)
            if m.sum() < 10:
                continue
            du = np.asarray(d["ec"])[m] - np.asarray(d["ewm"])[m]
            ad = np.abs(du)
            P("%-14s %7d | %10.4f %10.4f %10.4f %9.4f %9.4f %9.4f"
              % ("%s %s" % (CONST_NAME[const], b), int(m.sum()),
                 float(np.median(ad)), pctl(ad, 90), float(np.std(du)),
                 float(np.mean(ad < 0.5)), float(np.mean(ad < 1.0)),
                 float(np.mean(ad < 3.0))))
            unw["%s %s" % (CONST_NAME[const], b)] = {
                "n": int(m.sum()), "p50_abs": float(np.median(ad)),
                "p90_abs": pctl(ad, 90), "std": float(np.std(du)),
                "lt_0p5": float(np.mean(ad < 0.5)), "lt_1": float(np.mean(ad < 1.0)),
                "lt_3": float(np.mean(ad < 3.0))}
    J["consumer_join_unwrapped_settled"] = unw
    P("")
    P("---- UNWRAPPED REGRESSION of the consumer's SD error on the wire's (settled) ----")
    P("  The decisive consistency + sign test.  e_c = a*e_w + b: if the consumer inherits the")
    P("  wire, a ~ +1 with a high Pearson r on the MINUS convention and a NEGATIVE r on the")
    P("  PLUS one.  The scatter about the line is the consumer's OWN float error -- it is what")
    P("  makes the wrapped per-epoch test above read as null, and it is not evidence against")
    P("  inheritance.")
    P("%-14s %7s | %7s %7s %9s | %7s %7s | %9s %9s %9s"
      % ("channel", "n", "slope-", "r-", "resid sd", "slope+", "r+",
         "sd(e_c)", "sd(e_w)", "sd(-dP)"))
    reg = {}
    for const in ("G", "E"):
        for b in BANDS[const]:
            d = join.get((const, b))
            if not d:
                continue
            t = np.asarray(d["t"])
            m = (t >= lo_t2) & (t < hi_t2)
            if m.sum() < 30:
                continue
            ec = np.asarray(d["ec"])[m]
            ew = np.asarray(d["ewm"])[m]
            ewp = np.asarray(d["ewp"])[m]

            def _fit(x, y):
                A = np.vstack([x, np.ones_like(x)]).T
                c = np.linalg.lstsq(A, y, rcond=None)[0]
                return float(c[0]), float(np.corrcoef(x, y)[0, 1]), \
                    float(np.std(y - (c[0] * x + c[1])))
            sm, rm2, sd = _fit(ew, ec)
            sp2, rp2, _ = _fit(ewp, ec)
            P("%-14s %7d | %+7.3f %+7.3f %9.2f | %+7.3f %+7.3f | %9.2f %9.2f %9.2f"
              % ("%s %s" % (CONST_NAME[const], b), int(m.sum()), sm, rm2, sd,
                 sp2, rp2, float(np.std(ec)), float(np.std(ew)), float(np.std(ewp))))
            reg["%s %s" % (CONST_NAME[const], b)] = {
                "n": int(m.sum()), "slope_minus": sm, "pearson_minus": rm2,
                "resid_sd_cyc": sd, "slope_plus": sp2, "pearson_plus": rp2,
                "sd_ec": float(np.std(ec)), "sd_ew_minus": float(np.std(ew))}
    J["consumer_regression_settled"] = reg
    P("")
    P("---- ARC-MEAN join (the consumer's own float error averages down within an arc) ----")
    P("  arcs are runs of one (sat, band) chord with 30 s spacing and no |de| > 0.5 cyc step;")
    P("  arcs shorter than 40 epochs are dropped.  |wrap(mean e_c) - wrap(mean e_w)| is the")
    P("  arc-level version of the test above.")
    P("%-14s %7s | %10s %10s %10s %10s %10s"
      % ("channel", "n_arcs", "p50|c-w|", "R(c-w)", "R_null", "mu(c-w)", "p50|c+w|"))
    arcj = {}
    for const in ("G", "E"):
        for b in BANDS[const]:
            d = join.get((const, b))
            if not d:
                continue
            t = np.asarray(d["t"])
            m = (t >= lo_t2) & (t < hi_t2)
            per = defaultdict(list)
            sats = np.asarray(d["sat"])
            ec = np.asarray(d["ec"])
            ew = np.asarray(d["ewm"])
            for idx in np.where(m)[0]:
                per[sats[idx]].append((t[idx], ec[idx], ew[idx]))
            dm, dp2 = [], []
            for s, pts in per.items():
                pts.sort()
                arc = []
                for k, (tt, a, bb) in enumerate(pts):
                    if arc and (tt - pts[k - 1][0] > 90.0
                                or abs(a - pts[k - 1][1]) > 0.5):
                        if len(arc) >= 40:
                            aa = np.array(arc)
                            dm.append(float(wrap(aa[:, 0].mean() - aa[:, 1].mean())))
                            dp2.append(float(wrap(aa[:, 0].mean() + aa[:, 1].mean())))
                        arc = []
                    arc.append((a, bb))
                if len(arc) >= 40:
                    aa = np.array(arc)
                    dm.append(float(wrap(aa[:, 0].mean() - aa[:, 1].mean())))
                    dp2.append(float(wrap(aa[:, 0].mean() + aa[:, 1].mean())))
            if len(dm) < 4:
                continue
            # Signed values go into circ_R: |x| would confine every sample to a
            # half-circle and inflate the resultant.
            R, mu, n = circ_R(dm)
            Rp3, _mp, _ = circ_R(dp2)
            P("%-14s %7d | %10.4f %10.4f %10.4f %10.4f %10.4f"
              % ("%s %s" % (CONST_NAME[const], b), len(dm),
                 float(np.median(np.abs(dm))), R,
                 math.sqrt(math.pi / (4.0 * n)), mu,
                 float(np.median(np.abs(dp2)))))
            arcj["%s %s" % (CONST_NAME[const], b)] = {
                "n_arcs": len(dm), "p50_abs_diff": float(np.median(np.abs(dm))),
                "R_diff": R, "mu_diff": mu, "R_sum": Rp3,
                "R_null": math.sqrt(math.pi / (4.0 * n)),
                "p50_abs_sum": float(np.median(np.abs(dp2)))}
    J["consumer_join_arcmean_settled"] = arcj
    P("")

    # verdict: from the unwrapped regression, not the wrapped test
    settled = WINDOWS[-1][0]
    if reg:
        sm = float(np.mean([v["slope_minus"] for v in reg.values()]))
        rm2 = float(np.mean([v["pearson_minus"] for v in reg.values()]))
        rp2 = float(np.mean([v["pearson_plus"] for v in reg.values()]))
        sd = float(np.mean([v["resid_sd_cyc"] for v in reg.values()]))
        if rm2 > 0.6 and rm2 > abs(rp2):
            verdict = ("CONSUMER INHERITS THE WIRE, and the MINUS convention "
                       "(W = dC - dP) is the right one")
        elif rp2 > 0.6 and rp2 > abs(rm2):
            verdict = "SIGN INVERTED -- the PLUS convention (E = dC + dP) is the right one"
        else:
            verdict = "NEITHER -- the consumer's error is not the wire's"
        P("  VERDICT: %s" % verdict)
        P("    unwrapped regression, mean over bands: slope %+0.3f, Pearson r %+0.3f on the"
          % (sm, rm2))
        P("    minus convention vs %+0.3f on the plus one; residual sd %.1f cyc = the"
          % (rp2, sd))
        P("    CONSUMER's own float error, which is what makes the WRAPPED per-epoch test")
        P("    read as null (a >1-cycle scatter fills the circle regardless of inheritance).")
        if jj.get(settled):
            P("    wrapped per-epoch: mean R(c-w) = %.4f vs R_null %.4f"
              % (float(np.mean([v["R_diff_minus"] for v in jj[settled].values()])),
                 float(np.mean([v["R_null"] for v in jj[settled].values()]))))
        jj["_verdict"] = {"text": verdict, "slope_minus_mean": sm,
                          "pearson_minus_mean": rm2, "pearson_plus_mean": rp2,
                          "resid_sd_cyc_mean": sd}
    J["consumer_join"] = jj

    # ---- 6: time structure -------------------------------------------------
    P("")
    P("=" * 128)
    P("STEP 6  --  TIME STRUCTURE of the wire's wrapped SD fraction")
    P("  ONE FIXED reference per constellation per window (the most-emitted satellite), so")
    P("  every (sat, band) chord is one continuous series.  A tenure boundary is any")
    P("  PBGAUGE / PHASE_ANCHOR event on THAT satellite or on the reference (the reference's")
    P("  own gauge change moves every chord).  'within' = circular std about the tenure's own")
    P("  circular mean, in cycles (a bias gauge holds it near 0; a wander drives it toward")
    P("  the uniform value 0.34).  'step' = |wrapped difference of adjacent tenure means|.")
    P("=" * 128)
    ts_j = {}
    for wname, (lo_, hi_) in WINDOWS:
        lo_t, hi_t = tow0 + lo_ * 3600.0, tow0 + hi_ * 3600.0
        fixed, pe = fixed_by_window[wname]
        P("-- %s   fixed reference: %s --"
          % (wname, ", ".join("%s=%s" % (CONST_NAME[c], fixed[c]) for c in sorted(fixed))))
        P("%-16s %8s %9s %10s %11s %11s %10s %10s"
          % ("channel", "n_ten", "len[ep]", "within p50", "within p90", "|step| p50",
             "|step| p90", "n_steps"))
        for const in ("G", "E"):
            if const not in fixed:
                continue
            for b in BANDS[const]:
                ser = defaultdict(list)
                for t, row in pe.items():
                    if not (lo_t <= t < hi_t) or const not in row:
                        continue
                    if row[const]["ref"] != fixed[const]:
                        continue
                    for s, per_b in row[const]["e"].items():
                        ser[s].append((t, float(wrap(per_b[b]))))
                wi, st, ln = [], [], []
                for s, pts in ser.items():
                    pts.sort()
                    t = np.array([x[0] for x in pts])
                    v = np.array([x[1] for x in pts])
                    marks = sorted(set(x for x in ev_by_sat.get(s, ())
                                       if lo_t <= x < hi_t)
                                   | set(x for x in ev_by_sat.get(fixed[const], ())
                                         if lo_t <= x < hi_t))
                    edges = ([0] + [int(np.searchsorted(t, mk)) for mk in marks]
                             + [len(t)])
                    edges = sorted(set(edges))
                    means = []
                    for a, bb in zip(edges[:-1], edges[1:]):
                        # split further on data gaps > 90 s (a rise/set arc break)
                        if bb - a < 10:
                            continue
                        seg_t, seg_v = t[a:bb], v[a:bb]
                        brk = [0] + list(np.where(np.diff(seg_t) > 90.0)[0] + 1) + [len(seg_t)]
                        for p0, p1 in zip(brk[:-1], brk[1:]):
                            if p1 - p0 < 10:
                                continue
                            sv = seg_v[p0:p1]
                            wi.append(circ_std_cyc(sv))
                            ln.append(p1 - p0)
                            R, mu, _ = circ_R(sv)
                            means.append(mu)
                    for k in range(len(means) - 1):
                        st.append(abs(float(wrap(means[k + 1] - means[k]))))
                if wi:
                    P("%-16s %8d %9.0f %10.4f %11.4f %11.4f %10.4f %10d"
                      % ("%s %s" % (CONST_NAME[const], b), len(wi), pctl(ln, 50),
                         pctl(wi, 50), pctl(wi, 90), pctl(st, 50), pctl(st, 90), len(st)))
                    ts_j.setdefault(wname, {})["%s %s" % (CONST_NAME[const], b)] = {
                        "n_tenures": len(wi), "len_p50_ep": pctl(ln, 50),
                        "within_p50": pctl(wi, 50), "within_p90": pctl(wi, 90),
                        "step_p50": pctl(st, 50), "step_p90": pctl(st, 90),
                        "n_steps": len(st)}
        P("  (uniform reference: circular std of a U(-0.5,0.5) sample = 0.3401 cyc)")
        P("")
    J["time_structure"] = ts_j

    # hourly convergence view
    P("---- hourly p50 |wrapped SD fraction| on the wire (uniform null 0.25) ----")
    chans = []
    for const in ("G", "E"):
        chans += [("%s_%s" % (const, b), variants["minus"]["band"][(const, b)])
                  for b in BANDS[const]]
        chans += [("%s_%s" % (const, l), variants["minus"]["lane"][(const, l)])
                  for l in ("WL", "EWL", "NL")]
    P("%-3s " % "h" + " ".join("%9s" % c[0] for c in chans))
    hourly = {}
    for h in range(int(math.ceil(span_h)) + 1):
        cells, row = [], {}
        for name, recs in chans:
            a = win(recs, tow0, h, h + 1)
            if a.size > 1:
                v = float(np.median(np.abs(wrap(a))))
                cells.append("%9.4f" % v)
                row[name] = v
            else:
                cells.append("%9s" % "-")
        if row:
            P("%-3d " % h + " ".join(cells))
            hourly[h] = row
    J["hourly_frac_p50"] = hourly
    P("")

    # event census + reference occupancy
    P("---- producer events seen (network_events.csv tiers) ----")
    for tier in sorted(events):
        evs = Counter(r["event"] for r in events[tier])
        P("  %-14s %5d   %s" % (tier, len(events[tier]),
                                ", ".join("%s=%d" % kv for kv in evs.most_common())))
    J["events"] = {k: len(v) for k, v in events.items()}
    P("")
    P("---- wire SD reference occupancy ----")
    for const in ("G", "E"):
        cc = variants["minus"]["refs"][const]
        if cc:
            P("  %-8s %s" % (CONST_NAME[const],
                             ", ".join("%s:%d" % kv for kv in cc.most_common(8))))
            J.setdefault("ref_occupancy", {})[CONST_NAME[const]] = dict(cc)
    P("")

    txt = os.path.join(out_dir, "wire_integer_consistency.txt")
    jsn = args.json or os.path.join(out_dir, "wire_integer_consistency.json")
    with open(txt, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    with open(jsn, "w", encoding="utf-8") as fh:
        json.dump(J, fh, indent=1, default=float)
    print("\n".join(L))
    print("wrote %s" % txt)
    print("wrote %s" % jsn)
    return 0


if __name__ == "__main__":
    sys.exit(main())
