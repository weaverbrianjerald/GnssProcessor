#!/usr/bin/env python3
"""Warm-resume seam grader (card RS-2): grades a null leg against its own seed's traces.

    python scripts/ev_seam_grade.py runs/evleg_rs_null_nocarry \
        --seed runs/evseed_field6h [--json out.json]

The seed run is continuous through the leg's whole window (the leg resumes from a
snapshot the seed dropped mid-run), so the seed's traces at the matched tows are
the reference a warm resume is supposed to reproduce. Everything here is
leg-minus-seed at the same tow; nothing is truth and nothing is a verdict beyond
the pre-registered pass/fail rows of ``docs/SPEC_event_harness.md`` §21.3.

Epoch axis ``k = 1 + (tow - first_tow)/30`` with ``first_tow`` from the leg's
``leg_meta.json``, so ``+1`` is the leg's first processed epoch, the one the
rhs-attrib flight recorder fires on.

Channels
  STATUS   ``logs/network_epoch_status.csv``: disposition, ``clock_std_m``,
           ``sigma0_pf_code``, ``sigma0_pf``, ``n_d5_flags``, ``n_d5_retaps``,
           ``n_chord_births``, ``n_slip_refloats``.
  FRAME    ``logs/network_vtec_sh.csv`` a₀₀ = the (layer 0, n 0, m 0, trig C) row's
           ``value_tecu`` (§Amendment 4: a sun-fixed a₀₀ is the global mean VTEC,
           the only SH coefficient that is frame-meaningful on its own).
  ZWD      ``logs/network_zwd.csv`` per station (``zwd_m``), |leg − seed| over the
           stations present in both.
  WIRE     ``products.txt``: satellite clock as the datum-invariant contrast (each
           arm's per-epoch median over satellites removed first, then differenced,
           since a raw clock step confounds benign frame drift with a real defect,
           standing invariant #5), W6 per-signal code bias (B rows) and per-signal
           phase bias (P rows) as raw |Δ| (both are datum-pinned by the
           S-record's ``cbpin`` / ``amb_datum``).
  DISC     the emitter's declared discontinuities in the window: the ``R`` record
           (B1 per-satellite bump) and, when ``network.serialize_disc_counters`` is
           armed, the optional trailing counter field of each C/B/P row. A silent
           |ΔPB| > 5 cm on > 10 % of keys is invariant #7's failure; a declared one
           is the emitter doing its job.
  CONSUMER ``logs/closed_loop_trace.csv`` ``pos_err_3d_m``: reported, never gated
           (the closed-loop consumer is not snapshotted; its own cold start
           dominates a 30-minute window, §18.14).
  RHS      ``logs/rhs_attrib/attrib_<first_tow>.csv`` when the recorder tripped:
           the total rows' ``norm_total`` per class, the first-epoch discharge
           attribution.
"""
import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from event_grade import read_products      # noqa: E402  (the one C/P products parser)

EPOCH_S = 30.0

# Epoch-status columns lifted verbatim (leg and seed both).
STATUS_COLS = ("disposition", "clock_std_m", "sigma0_pf_code", "sigma0_pf",
               "n_d5_flags", "n_d5_retaps", "n_chord_births", "n_slip_refloats")


# ------------------------------------------------------------------ utilities
def fnum(v, d=3, w=10):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "NA".rjust(w)
    return f"{v:{w}.{d}f}"


def fint(v, w=6):
    return "NA".rjust(w) if v is None else f"{int(v):{w}d}"


def pct(vals, q):
    """Simple order-statistic percentile (q in [0,100]); None on an empty list."""
    if not vals:
        return None
    s = sorted(vals)
    if len(s) == 1:
        return s[0]
    i = (len(s) - 1) * (q / 100.0)
    lo = int(math.floor(i))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (i - lo)


def f_or_none(row, col):
    try:
        v = float(row[col])
    except (KeyError, TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def itow(t):
    return int(round(float(t)))


# --------------------------------------------------------------- leg metadata
def leg_meta(legdir):
    p = os.path.join(legdir, "leg_meta.json")
    if not os.path.isfile(p):
        raise SystemExit(f"FATAL: no leg_meta.json in {legdir}")
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


# ------------------------------------------------------------- trace readers
def read_status(rundir):
    """{tow_int: {col: value}} from network_epoch_status.csv."""
    p = os.path.join(rundir, "logs", "network_epoch_status.csv")
    out = {}
    if not os.path.isfile(p):
        return out
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                t = itow(r["tow"])
            except (KeyError, TypeError, ValueError):
                continue
            rec = {"disposition": r.get("disposition", "")}
            for c in STATUS_COLS[1:]:
                rec[c] = f_or_none(r, c)
            out[t] = rec
    return out


def read_a00(rundir):
    """{tow_int: a00_tecu}, the (layer 0, n 0, m 0, trig C) row of network_vtec_sh.csv."""
    p = os.path.join(rundir, "logs", "network_vtec_sh.csv")
    out = {}
    if not os.path.isfile(p):
        return out
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if (r.get("layer") != "0" or r.get("n") != "0" or r.get("m") != "0"
                    or r.get("trig") != "C"):
                continue
            try:
                out[itow(r["tow"])] = float(r["value_tecu"])
            except (KeyError, TypeError, ValueError):
                continue
    return out


def _zwd_value_column(fieldnames):
    """Find the ZWD value column by name (the trace has grown columns over time)."""
    if not fieldnames:
        return None
    for cand in ("zwd_m", "zwd", "value_m", "zwd_total_m"):
        if cand in fieldnames:
            return cand
    for c in fieldnames:                       # last resort: first zwd-ish numeric
        if c.lower().startswith("zwd"):
            return c
    return None


def read_zwd(rundir):
    """{tow_int: {station: zwd_m}} from network_zwd.csv."""
    p = os.path.join(rundir, "logs", "network_zwd.csv")
    out = defaultdict(dict)
    if not os.path.isfile(p):
        return {}
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.DictReader(fh)
        col = _zwd_value_column(rd.fieldnames)
        if col is None:
            return {}
        for r in rd:
            v = f_or_none(r, col)
            sta = r.get("station")
            if v is None or not sta:
                continue
            try:
                out[itow(r["tow"])][sta] = v
            except (KeyError, TypeError, ValueError):
                continue
    return dict(out)


def read_consumer(rundir):
    """{tow_int: pos_err_3d_m} from closed_loop_trace.csv."""
    p = os.path.join(rundir, "logs", "closed_loop_trace.csv")
    out = {}
    if not os.path.isfile(p):
        return out
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            v = f_or_none(r, "pos_err_3d_m")
            if v is None:
                continue
            try:
                out[itow(r["tow"])] = v
            except (KeyError, TypeError, ValueError):
                continue
    return out


def read_code_bias_and_disc(path, lo=None, hi=None):
    """products.txt -> ({tow: {(sat,signal): bias_m}}, {tow: {...declarations...}}).

    ``event_grade.read_products`` is the project's single products parser but covers
    only the C (clock) and P (phase-bias) records; the B (code-bias) record and the
    declaration channels are read here in a second pass rather than by forking it.

    Declarations per epoch: ``R`` is the B1 per-satellite discontinuity-bump record,
    and the optional trailing counter field of a C/B/P row (RTA-4,
    ``network.serialize_disc_counters``; absent on a lineage that does not arm it,
    in which case ``ctr_*`` stay empty and only the R record is available). A field
    containing '=' is a self-describing key=value token, never a positional number:
    the positional parse stops at the first one, matching what the writer documents.
    """
    bias = {}
    decl = {}
    tow = None
    if not os.path.isfile(path):
        return bias, decl
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line or line[0] == "#":
                continue
            fs = line.rstrip("\n").split(",")
            tag = fs[0]
            if tag == "E" and len(fs) >= 3:
                try:
                    t = itow(fs[2])
                except ValueError:
                    tow = None
                    continue
                tow = t
                if (lo is not None and t < lo) or (hi is not None and t > hi):
                    tow = None
                else:
                    bias.setdefault(t, {})
                    decl.setdefault(t, {"R": set(), "ctr_clk": {}, "ctr_cb": {},
                                        "ctr_pb": {}})
                continue
            if tow is None:
                continue
            pos = []
            for f in fs:
                if "=" in f:
                    break
                pos.append(f)
            if tag == "B" and len(pos) >= 4:
                try:
                    bias[tow][(pos[1], pos[2])] = float(pos[3])
                except ValueError:
                    pass
                if len(pos) >= 5:                       # code-bias counter (RTA-4)
                    try:
                        decl[tow]["ctr_cb"][(pos[1], pos[2])] = int(pos[4])
                    except ValueError:
                        pass
            elif tag == "P" and len(pos) >= 7:          # phase-bias counter (RTA-4)
                try:
                    decl[tow]["ctr_pb"][(pos[1], pos[2])] = int(pos[6])
                except ValueError:
                    pass
            elif tag == "C" and len(pos) >= 5:          # clock counter, field 5 (RTA-4)
                try:
                    decl[tow]["ctr_clk"][pos[1]] = int(pos[4])
                except ValueError:
                    pass
            elif tag == "R" and len(pos) >= 2:
                decl[tow]["R"].update(pos[1:])
    return bias, decl


# ------------------------------------------------------------------- channels
def clock_contrast(a_epoch, b_epoch):
    """|Δ| per satellite after removing each arm's own per-epoch median clock.

    The satellite-clock wire carries an arbitrary per-epoch datum; a raw step
    confounds benign frame drift with a real defect (standing invariant #5). The
    median over the satellites common to both arms is removed from each arm first,
    giving the datum-free contrast the ledger grades on.
    """
    sats = sorted({k[1] for k in a_epoch if k[0] == "C"} &
                  {k[1] for k in b_epoch if k[0] == "C"})
    if len(sats) < 2:
        return []
    va = [a_epoch[("C", s)] for s in sats]
    vb = [b_epoch[("C", s)] for s in sats]
    ma, mb = pct(va, 50), pct(vb, 50)
    return [abs((a - ma) - (b - mb)) for a, b in zip(va, vb)]


def pb_deltas(a_epoch, b_epoch):
    """{(sat,signal): |Δ bias_m|} over the phase-bias keys present in both arms."""
    out = {}
    for k, v in a_epoch.items():
        if k[0] == "P" and k in b_epoch:
            out[(k[1], k[2])] = abs(v - b_epoch[k])
    return out


def pb_desat(a_epoch, b_epoch):
    """|ΔPB| after removing each satellite's own median ΔPB, the de-referenced view.

    A branch re-realization at the seam moves a satellite's whole phase-bias column by a
    common amount (its ambiguity datum lands elsewhere), so the raw |ΔPB| reads that one
    re-realization on every signal of that satellite from then on. Removing the
    per-satellite median leaves only the part that is not a per-satellite datum; a
    satellite with a single signal in common drops out, since its residual is 0 by
    construction and would dilute the quantile.
    """
    per = {}
    for k, v in a_epoch.items():
        if k[0] == "P" and k in b_epoch:
            per.setdefault(k[1], []).append(v - b_epoch[k])
    out = []
    for _sat, vals in per.items():
        if len(vals) < 2:
            continue
        m = pct(vals, 50)
        out += [abs(x - m) for x in vals]
    return out


# ---------------------------------------------------------------------- rhs
_SHGATE_RE = None


def read_shgate_a00(rundir):
    """{tow_int: a00_tecu} as the SH health gate itself read it, from main_log.log.

    Not the same number as ``network_vtec_sh.csv``'s (0,0,C) row: the gate
    evaluates its own candidate a₀₀ while the trace records the epoch's stored
    coefficient, and on a torn first epoch the two can differ by roughly a factor
    of two at the same tow. §21.3's G1 bar was quoted from the gate line, so both
    are reported and neither is silently substituted for the other.
    """
    global _SHGATE_RE
    if _SHGATE_RE is None:
        import re
        _SHGATE_RE = re.compile(
            r"SH gate\).*?ENGAGED at TOW\s+([0-9.]+)\s*.*?a00\s+(-?[0-9.eE+]+)\s*TECU")
    p = os.path.join(rundir, "logs", "main_log.log")
    out = {}
    if not os.path.isfile(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = _SHGATE_RE.search(ln)
            if m:
                try:
                    out[itow(m.group(1))] = float(m.group(2))
                except ValueError:
                    pass
    return out


def read_rhs_total(rundir, tow):
    """[(class, norm_total, c00_l1, c00_l2)] from the TOTAL rows, descending."""
    p = os.path.join(rundir, "logs", "rhs_attrib", f"attrib_{int(tow)}.csv")
    if not os.path.isfile(p):
        return None
    rows = []
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if r.get("kind") != "TOTAL":
                continue
            rows.append((r.get("class", ""), f_or_none(r, "norm_total"),
                         f_or_none(r, "c00_l1"), f_or_none(r, "c00_l2")))
    rows.sort(key=lambda x: (x[1] is None, -(x[1] or 0.0)))
    return rows


# --------------------------------------------------------------------- grade
def grade(legdir, seeddir):
    meta = leg_meta(legdir)
    first = itow(meta["first_tow"])
    nep = int(meta["epochs"])
    tows = [first + i * int(EPOCH_S) for i in range(nep)]
    kof = {t: i + 1 for i, t in enumerate(tows)}

    leg = {"status": read_status(legdir), "a00": read_a00(legdir),
           "zwd": read_zwd(legdir), "cons": read_consumer(legdir)}
    seed = {"status": read_status(seeddir), "a00": read_a00(seeddir),
            "zwd": read_zwd(seeddir), "cons": read_consumer(seeddir)}

    lp = os.path.join(legdir, "products.txt")
    sp = os.path.join(seeddir, "products.txt")
    lo, hi = tows[0], tows[-1]
    leg_cp = read_products(lp, lo, hi) if os.path.isfile(lp) else {}
    seed_cp = read_products(sp, lo, hi) if os.path.isfile(sp) else {}
    leg_cb, leg_decl = read_code_bias_and_disc(lp, lo, hi)
    seed_cb, _ = read_code_bias_and_disc(sp, lo, hi)
    have_wire = bool(leg_cp) and bool(seed_cp)

    rows = []
    for t in tows:
        k = kof[t]
        ls = leg["status"].get(t, {})
        ss = seed["status"].get(t, {})
        r = {"k": k, "tow": t, "disposition": ls.get("disposition"),
             "a00_leg": leg["a00"].get(t), "a00_seed": seed["a00"].get(t)}
        r["d_a00"] = (None if r["a00_leg"] is None or r["a00_seed"] is None
                      else r["a00_leg"] - r["a00_seed"])
        for c in STATUS_COLS[1:]:
            r[c + "_leg"] = ls.get(c)
            r[c + "_seed"] = ss.get(c)

        lz, sz = leg["zwd"].get(t, {}), seed["zwd"].get(t, {})
        dz = [abs(lz[s] - sz[s]) for s in lz if s in sz]
        r["zwd_n"] = len(dz)
        r["zwd_p50"], r["zwd_p95"] = pct(dz, 50), pct(dz, 95)

        if have_wire and t in leg_cp and t in seed_cp:
            dc = clock_contrast(leg_cp[t], seed_cp[t])
            r["clk_n"] = len(dc)
            r["clk_p50"], r["clk_p95"] = pct(dc, 50), pct(dc, 95)
            db = [abs(v - seed_cb[t][kk]) for kk, v in leg_cb.get(t, {}).items()
                  if t in seed_cb and kk in seed_cb[t]]
            r["cb_n"] = len(db)
            r["cb_p50"], r["cb_p95"] = pct(db, 50), pct(db, 95)
            dpb = pb_deltas(leg_cp[t], seed_cp[t])
            vals = list(dpb.values())
            r["pb_n"] = len(vals)
            r["pb_p50"], r["pb_p95"] = pct(vals, 50), pct(vals, 95)
            r["pb_over"] = sum(1 for v in vals if v > 0.05)
            r["pb_over_frac"] = (r["pb_over"] / len(vals)) if vals else None
            ds = pb_desat(leg_cp[t], seed_cp[t])
            r["pb_desat_n"] = len(ds)
            r["pb_desat_p50"], r["pb_desat_p95"] = pct(ds, 50), pct(ds, 95)
            d = leg_decl.get(t, {})
            r["decl_R"] = sorted(d.get("R", ()))
            r["decl_pb_ctr"] = len(d.get("ctr_pb", {}))
        else:
            for c in ("clk_n", "clk_p50", "clk_p95", "cb_n", "cb_p50", "cb_p95",
                      "pb_n", "pb_p50", "pb_p95", "pb_over", "pb_over_frac",
                      "pb_desat_n", "pb_desat_p50", "pb_desat_p95", "decl_pb_ctr"):
                r[c] = None
            r["decl_R"] = []

        r["pos_leg"] = leg["cons"].get(t)
        r["pos_seed"] = seed["cons"].get(t)
        rows.append(r)
    return meta, rows, leg_decl, have_wire


# ---------------------------------------------------------------------- gates
def gates(rows, legdir, meta, have_wire):
    """The §21.3 bars, frozen at pre-registration.  PASS/FAIL only, no editorial."""
    out = {}
    first = itow(meta["first_tow"])
    n = len(rows)
    emitted = [r for r in rows if (r["disposition"] or "") == "emitted"]
    withheld = [r for r in rows if (r["disposition"] or "") != "emitted"]
    out["n_epochs"] = n
    out["n_emitted"] = len(emitted)
    out["n_withheld"] = len(withheld)
    out["withheld_k"] = [r["k"] for r in withheld]
    out["dispositions"] = sorted({(r["disposition"] or "?") for r in rows})

    # The L0 control (gate G1): the failure must reproduce on this exe.
    rhs = read_rhs_total(legdir, first)
    out["rhs_total"] = ([{"class": c, "norm_total": v, "c00_l1": a, "c00_l2": b}
                         for c, v, a, b in rhs] if rhs else None)
    dd_fold = None
    if rhs:
        for c, v, _a, _b in rhs:
            if c == "dd_fold":
                dd_fold = v
    a00_1 = rows[0]["a00_leg"] if rows else None
    gate_a00 = read_shgate_a00(legdir).get(first)
    out["G1"] = {
        "withheld_all": len(withheld) == n,
        "a00_p1": a00_1, "a00_p1_lt_-5000": (a00_1 is not None and a00_1 < -5000.0),
        "a00_p1_shgate_log": gate_a00,
        "a00_p1_shgate_lt_-5000": (gate_a00 is not None and gate_a00 < -5000.0),
        "dd_fold_total": dd_fold, "dd_fold_gt_1e7": (dd_fold is not None and dd_fold > 1e7),
    }
    out["G1"]["verdict"] = ("PASS" if (out["G1"]["withheld_all"]
                                       and out["G1"]["a00_p1_lt_-5000"]
                                       and out["G1"]["dd_fold_gt_1e7"]) else "FAIL")

    # Availability (gate G2).
    bad_k = [k for k in out["withheld_k"] if not (1 <= k <= 5)]
    out["G2"] = {"emitted": len(emitted), "need": 55, "withheld_outside_1_5": bad_k,
                 "verdict": "PASS" if (len(emitted) >= 55 and not bad_k) else "FAIL"}

    # Frame consistency (gate G3), checked at every epoch from +5 on.
    g3 = {"a00_worst": None, "a00_worst_k": None, "clkstd_worst_ratio": None,
          "clkstd_worst_k": None, "s0code_worst_ratio": None, "s0code_worst_k": None,
          "n_missing": 0}
    for r in rows:
        if r["k"] < 5:
            continue
        if r["d_a00"] is None:
            g3["n_missing"] += 1
        else:
            v = abs(r["d_a00"])
            if g3["a00_worst"] is None or v > g3["a00_worst"]:
                g3["a00_worst"], g3["a00_worst_k"] = v, r["k"]
        for key, tag in (("clock_std_m", "clkstd"), ("sigma0_pf_code", "s0code")):
            a, b = r[key + "_leg"], r[key + "_seed"]
            if a is None or b is None or b <= 0.0:
                continue
            ratio = a / b
            if g3[tag + "_worst_ratio"] is None or ratio > g3[tag + "_worst_ratio"]:
                g3[tag + "_worst_ratio"], g3[tag + "_worst_k"] = ratio, r["k"]
    g3["verdict"] = "PASS" if (
        g3["a00_worst"] is not None and g3["a00_worst"] <= 5.0
        and g3["clkstd_worst_ratio"] is not None and g3["clkstd_worst_ratio"] <= 2.0
        and g3["s0code_worst_ratio"] is not None and g3["s0code_worst_ratio"] <= 2.0
    ) else "FAIL"
    out["G3"] = g3

    # Branch cost (gate G4): births summed over +5..+60.
    bl = sum((r["n_chord_births_leg"] or 0.0) for r in rows if r["k"] >= 5)
    bs = sum((r["n_chord_births_seed"] or 0.0) for r in rows if r["k"] >= 5)
    out["G4"] = {"births_leg_5_end": bl, "births_seed_5_end": bs,
                 "ratio": (bl / bs) if bs > 0 else None,
                 "d5_flags_p1": rows[0]["n_d5_flags_leg"] if rows else None,
                 "verdict": "PASS" if (bl <= 5.0 * bs or (bs == 0.0 and bl == 0.0))
                            else "FAIL"}

    # Gate G5 needs the pooled raw |Δ| samples, not a percentile-of-percentiles
    # (the per-epoch table keeps only percentiles). ``pooled_g5`` rebuilds them and
    # ``g5_verdict`` scores them; the caller installs the result under "G5".
    out["G5"] = None
    return out


def pooled_g5(legdir, seeddir, meta, rows):
    """Pooled |Δ| samples from +5 on: the exact distributions G5's bars read."""
    first = itow(meta["first_tow"])
    nep = int(meta["epochs"])
    tows = [first + i * int(EPOCH_S) for i in range(nep)]
    lo, hi = tows[0], tows[-1]
    lp = os.path.join(legdir, "products.txt")
    sp = os.path.join(seeddir, "products.txt")
    if not (os.path.isfile(lp) and os.path.isfile(sp)):
        return None
    leg_cp, seed_cp = read_products(lp, lo, hi), read_products(sp, lo, hi)
    leg_cb, leg_decl = read_code_bias_and_disc(lp, lo, hi)
    seed_cb, _ = read_code_bias_and_disc(sp, lo, hi)
    lz, sz = read_zwd(legdir), read_zwd(seeddir)
    clk, cb, zwd = [], [], []
    pb_bad_epochs = []
    declared_pb = 0
    for i, t in enumerate(tows):
        k = i + 1
        if k < 5:
            continue
        if t in leg_cp and t in seed_cp:
            clk += clock_contrast(leg_cp[t], seed_cp[t])
            dpb = pb_deltas(leg_cp[t], seed_cp[t])
            vals = list(dpb.values())
            if vals:
                over = sum(1 for v in vals if v > 0.05)
                if over / len(vals) > 0.10:
                    pb_bad_epochs.append({"k": k, "over": over, "n": len(vals),
                                          "frac": over / len(vals),
                                          "declared_R": sorted(
                                              leg_decl.get(t, {}).get("R", ()))})
        if t in leg_cb and t in seed_cb:
            cb += [abs(v - seed_cb[t][kk]) for kk, v in leg_cb[t].items()
                   if kk in seed_cb[t]]
        a, b = lz.get(t, {}), sz.get(t, {})
        zwd += [abs(a[s] - b[s]) for s in a if s in b]
        declared_pb += len(leg_decl.get(t, {}).get("R", ()))
    return {"clk": clk, "cb": cb, "zwd": zwd, "pb_bad_epochs": pb_bad_epochs,
            "declared_pb_bumps": declared_pb}


def seed_yardsticks(seeddir, tows):
    """The seed's own epoch-to-epoch step over the leg window: the calibration scale.

    A leg-minus-seed bar is only meaningful read against how far the continuous
    run's own wire moves between consecutive epochs. Measured on exactly the
    leg's tow window so the comparison is time-matched (the wire's step rate is not
    constant through the day). Returns p50/p95 per channel, in metres.
    """
    lo, hi = tows[0], tows[-1]
    sp = os.path.join(seeddir, "products.txt")
    out = {}
    if os.path.isfile(sp):
        P = read_products(sp, lo, hi)
        B, _ = read_code_bias_and_disc(sp, lo, hi)
        ts = sorted(P)
        clk, pb, cb = [], [], []
        for i in range(1, len(ts)):
            a, b = P[ts[i]], P[ts[i - 1]]
            clk += clock_contrast(a, b)
            pb += [abs(v - b[k]) for k, v in a.items() if k[0] == "P" and k in b]
            ba, bb = B.get(ts[i], {}), B.get(ts[i - 1], {})
            cb += [abs(v - bb[k]) for k, v in ba.items() if k in bb]
        out["clk"] = (len(clk), pct(clk, 50), pct(clk, 95))
        out["pb"] = (len(pb), pct(pb, 50), pct(pb, 95))
        out["cb"] = (len(cb), pct(cb, 50), pct(cb, 95))
    z = read_zwd(seeddir)
    zt = [t for t in sorted(z) if lo <= t <= hi]
    zd = []
    for i in range(1, len(zt)):
        a, b = z[zt[i]], z[zt[i - 1]]
        zd += [abs(a[s] - b[s]) for s in a if s in b]
    out["zwd"] = (len(zd), pct(zd, 50), pct(zd, 95))
    return out


def g5_verdict(pool):
    if pool is None:
        return {"verdict": "NA (leg emitted nothing)"}
    g = {
        "clk_n": len(pool["clk"]), "clk_p50": pct(pool["clk"], 50),
        "clk_p95": pct(pool["clk"], 95),
        "cb_n": len(pool["cb"]), "cb_p50": pct(pool["cb"], 50),
        "cb_p95": pct(pool["cb"], 95),
        "zwd_n": len(pool["zwd"]), "zwd_p50": pct(pool["zwd"], 50),
        "zwd_p95": pct(pool["zwd"], 95),
        "pb_bad_epochs": pool["pb_bad_epochs"],
        "declared_pb_bumps": pool["declared_pb_bumps"],
    }
    ok = (g["clk_p50"] is not None and g["clk_p50"] <= 0.05
          and g["clk_p95"] is not None and g["clk_p95"] <= 0.20
          and g["cb_p50"] is not None and g["cb_p50"] <= 0.02
          and g["zwd_p50"] is not None and g["zwd_p50"] <= 0.005
          and g["zwd_p95"] is not None and g["zwd_p95"] <= 0.020
          and not g["pb_bad_epochs"])
    g["verdict"] = "PASS" if ok else "FAIL"
    return g


# ---------------------------------------------------------------------- print
HDR = (f"{'k':>3} {'tow':>8} {'disp':<18} {'a00_leg':>11} {'a00_seed':>10} "
       f"{'d_a00':>10} {'clkstd_L':>10} {'clkstd_S':>9} {'s0cod_L':>10} "
       f"{'s0cod_S':>9} {'d5f':>5} {'brt_L':>6} {'brt_S':>6} "
       f"{'zwdp50':>8} {'zwdp95':>8} {'clkp50':>8} {'clkp95':>8} {'cbp50':>8} "
       f"{'pbp50':>8} {'pbp95':>8} {'pb>5c':>6} {'pos_L':>8} {'pos_S':>8}")


def print_table(rows):
    print(HDR)
    print("-" * len(HDR))
    for r in rows:
        print(f"{r['k']:>3} {r['tow']:>8} {(r['disposition'] or '?'):<18} "
              f"{fnum(r['a00_leg'], 2, 11)} {fnum(r['a00_seed'], 2, 10)} "
              f"{fnum(r['d_a00'], 2, 10)} "
              f"{fnum(r['clock_std_m_leg'], 3, 10)} {fnum(r['clock_std_m_seed'], 3, 9)} "
              f"{fnum(r['sigma0_pf_code_leg'], 3, 10)} {fnum(r['sigma0_pf_code_seed'], 3, 9)} "
              f"{fint(r['n_d5_flags_leg'], 5)} "
              f"{fint(r['n_chord_births_leg'], 6)} {fint(r['n_chord_births_seed'], 6)} "
              f"{fnum(r['zwd_p50'], 4, 8)} {fnum(r['zwd_p95'], 4, 8)} "
              f"{fnum(r['clk_p50'], 4, 8)} {fnum(r['clk_p95'], 4, 8)} "
              f"{fnum(r['cb_p50'], 4, 8)} "
              f"{fnum(r['pb_p50'], 4, 8)} {fnum(r['pb_p95'], 4, 8)} "
              f"{fint(r['pb_over'], 6)} "
              f"{fnum(r['pos_leg'], 3, 8)} {fnum(r['pos_seed'], 3, 8)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("legdir")
    ap.add_argument("--seed", required=True,
                    help="the CONTINUOUS seed run the leg resumed from")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    repo = os.path.dirname(_HERE)
    legdir = args.legdir if os.path.isabs(args.legdir) else os.path.join(repo, args.legdir)
    seeddir = args.seed if os.path.isabs(args.seed) else os.path.join(repo, args.seed)
    for d in (legdir, seeddir):
        if not os.path.isdir(d):
            raise SystemExit(f"FATAL: not a directory: {d}")

    meta, rows, leg_decl, have_wire = grade(legdir, seeddir)
    g = gates(rows, legdir, meta, have_wire)
    pool = pooled_g5(legdir, seeddir, meta, rows) if have_wire else None
    g["G5"] = g5_verdict(pool)

    print(f"SEAM GRADE — leg {legdir}")
    print(f"            seed {seeddir}")
    print(f"  first_tow {meta['first_tow']}  epochs {meta['epochs']}  "
          f"fingerprint {meta.get('fingerprint')}")
    print(f"  leg EXTRA {json.dumps(meta.get('extra', {}))}")
    print()
    print_table(rows)
    print()
    print("SUMMARY")
    print(f"  epochs {g['n_epochs']}   emitted {g['n_emitted']}   "
          f"withheld {g['n_withheld']}   dispositions {g['dispositions']}")
    if g["n_withheld"]:
        print(f"  withheld at k = {g['withheld_k']}")
    if g["rhs_total"]:
        print()
        print(f"  rhs-attrib TOTAL at +1 (tow {itow(meta['first_tow'])}):")
        print(f"    {'class':<22}{'norm_total':>16}{'c00_l1':>14}{'c00_l2':>14}")
        for e in g["rhs_total"]:
            print(f"    {e['class']:<22}{(('%.4g' % e['norm_total']) if e['norm_total'] is not None else 'NA'):>16}"
                  f"{(('%.4f' % e['c00_l1']) if e['c00_l1'] is not None else 'NA'):>14}"
                  f"{(('%.4f' % e['c00_l2']) if e['c00_l2'] is not None else 'NA'):>14}")
    else:
        print("  rhs-attrib: no attrib file at +1 (the recorder did not trip)")
    print()
    print("GATES (bars frozen in SPEC_event_harness §21.3)")
    G1 = g["G1"]
    print(f"  G1 control     {G1['verdict']:<5} withheld_all={G1['withheld_all']}  "
          f"a00(+1)={fnum(G1['a00_p1'], 2, 0).strip()} (<-5000: {G1['a00_p1_lt_-5000']})  "
          f"dd_fold={('%.4g' % G1['dd_fold_total']) if G1['dd_fold_total'] is not None else 'NA'}"
          f" (>1e7: {G1['dd_fold_gt_1e7']})")
    print(f"                       cross-ref: the SH GATE's own a00(+1) = "
          f"{fnum(G1['a00_p1_shgate_log'], 2, 0).strip()} TECU "
          f"(<-5000: {G1['a00_p1_shgate_lt_-5000']}) — §21.1 quoted THIS reading")
    G2 = g["G2"]
    print(f"  G2 availability{G2['verdict']:>6} emitted {G2['emitted']}/{g['n_epochs']} "
          f"(need >= 55)   withheld outside +1..+5: {G2['withheld_outside_1_5'] or 'none'}")
    G3 = g["G3"]
    print(f"  G3 frame       {G3['verdict']:<5} worst |d_a00| from +5 = "
          f"{fnum(G3['a00_worst'], 3, 0).strip()} TECU at k+{G3['a00_worst_k']} (bar 5.0); "
          f"clock_std ratio {fnum(G3['clkstd_worst_ratio'], 3, 0).strip()} at k+{G3['clkstd_worst_k']} (bar 2.0); "
          f"sigma0_pf_code ratio {fnum(G3['s0code_worst_ratio'], 3, 0).strip()} at k+{G3['s0code_worst_k']} (bar 2.0)")
    G4 = g["G4"]
    print(f"  G4 branch cost {G4['verdict']:<5} births +5..end leg {G4['births_leg_5_end']:.0f} "
          f"vs seed {G4['births_seed_5_end']:.0f}  ratio "
          f"{fnum(G4['ratio'], 2, 0).strip()} (bar 5.0);  D5 flags at +1 = "
          f"{fint(G4['d5_flags_p1'], 0).strip()}")
    G5 = g["G5"]
    if G5.get("verdict", "").startswith("NA"):
        print(f"  G5 wire+ZWD    {G5['verdict']}")
    else:
        print(f"  G5 wire+ZWD    {G5['verdict']:<5} clk-contrast p50 "
              f"{fnum(G5['clk_p50'], 4, 0).strip()} / p95 {fnum(G5['clk_p95'], 4, 0).strip()} "
              f"(bars 0.05 / 0.20 m, n={G5['clk_n']});  code-bias p50 "
              f"{fnum(G5['cb_p50'], 4, 0).strip()} (bar 0.02 m, n={G5['cb_n']});  ZWD p50 "
              f"{fnum(G5['zwd_p50'], 4, 0).strip()} / p95 {fnum(G5['zwd_p95'], 4, 0).strip()} "
              f"(bars 0.005 / 0.020 m, n={G5['zwd_n']})")
        print(f"                       PB epochs with >10% of keys over 5 cm: "
              f"{len(G5['pb_bad_epochs'])}"
              + ("" if not G5["pb_bad_epochs"] else
                 "  -> " + ", ".join(f"k+{e['k']}:{e['over']}/{e['n']}"
                                     f"({e['frac']*100:.0f}%)"
                                     f"{' declared=' + '+'.join(e['declared_R']) if e['declared_R'] else ' SILENT'}"
                                     for e in G5["pb_bad_epochs"][:12])))
        print(f"                       emitter-DECLARED discontinuity bumps (R records) "
              f"in +5..end: {G5['declared_pb_bumps']}")
    # a00 at the last epoch vs the seed's (readout G7, an L4 readout, always printed).
    last = rows[-1]
    print(f"  G7 readout     a00(+{last['k']}) leg {fnum(last['a00_leg'], 2, 0).strip()} "
          f"vs seed {fnum(last['a00_seed'], 2, 0).strip()}  d = "
          f"{fnum(last['d_a00'], 2, 0).strip()} TECU  (within 5: "
          f"{last['d_a00'] is not None and abs(last['d_a00']) <= 5.0})")
    g["G7"] = {"k": last["k"], "a00_leg": last["a00_leg"], "a00_seed": last["a00_seed"],
               "d_a00": last["d_a00"],
               "within_5": (last["d_a00"] is not None and abs(last["d_a00"]) <= 5.0)}
    cons = [r for r in rows if r["pos_leg"] is not None and r["pos_seed"] is not None
            and r["k"] >= 5]
    if cons:
        pl = pct([r["pos_leg"] for r in cons], 50)
        ps = pct([r["pos_seed"] for r in cons], 50)
        print(f"  CONSUMER       (report only) pos_err_3d p50 from +5: leg "
              f"{fnum(pl, 3, 0).strip()} m vs seed {fnum(ps, 3, 0).strip()} m "
              f"over {len(cons)} epochs")
        g["consumer"] = {"n": len(cons), "leg_p50": pl, "seed_p50": ps}
    else:
        print("  CONSUMER       (report only) no overlapping closed-loop rows")
        g["consumer"] = None

    # Per-epoch detail a seam A/B is read on, beside the gates (RS-2 §21.5). Everything
    # here is reported, never scored.
    print()
    print("SEAM DETAIL (RS-2)")
    at = {r["k"]: r for r in rows}
    decl = [(r["k"], r["decl_R"]) for r in rows if r["decl_R"]]
    if decl:
        print(f"  emitter R records (per-satellite discontinuity bump): "
              + ", ".join(f"k+{k}: {len(s)} sat" for k, s in decl[:12])
              + (f"  (+{len(decl) - 12} more)" if len(decl) > 12 else ""))
        print(f"    first R at k+{decl[0][0]} names: {' '.join(decl[0][1])}")
    else:
        print("  emitter R records: none in the window")
    print("  chord births k+1..+5:  "
          + "  ".join(f"+{k}: {fint(at[k]['n_chord_births_leg'], 0).strip()}"
                      for k in (1, 2, 3, 4, 5) if k in at)
          + "    (seed at the same tows: "
          + " ".join(f"+{k}: {fint(at[k]['n_chord_births_seed'], 0).strip()}"
                     for k in (1, 2, 3, 4, 5) if k in at) + ")")
    print("  D5 flags / re-taps:    "
          + "  ".join(f"+{k}: {fint(at[k]['n_d5_flags_leg'], 0).strip()}"
                      f" / {fint(at[k]['n_d5_retaps_leg'], 0).strip()}"
                      for k in (1, 2) if k in at))
    for k in (2, 60):
        if k in at:
            print(f"  |dPB| vs seed at k+{k}: raw p50 "
                  f"{fnum(at[k]['pb_p50'], 4, 0).strip()} m; PER-SAT-median-removed p50 "
                  f"{fnum(at[k]['pb_desat_p50'], 4, 0).strip()} / p95 "
                  f"{fnum(at[k]['pb_desat_p95'], 4, 0).strip()} m "
                  f"(n={at[k]['pb_desat_n']})")
    for k in (5, 60):
        if k in at:
            print(f"  clock contrast at k+{k}: p50 {fnum(at[k]['clk_p50'], 4, 0).strip()} "
                  f"/ p95 {fnum(at[k]['clk_p95'], 4, 0).strip()} m")
    yard = seed_yardsticks(seeddir, [r["tow"] for r in rows])
    print()
    print("SEED YARDSTICKS — the CONTINUOUS run's OWN epoch-to-epoch step over this window")
    for tag, name in (("clk", "clock contrast (datum-free)"), ("cb", "W6 code bias"),
                      ("pb", "phase bias"), ("zwd", "station ZWD")):
        if tag in yard and yard[tag][1] is not None:
            n, p50, p95 = yard[tag]
            print(f"  {name:<28} p50 {p50:.5f}  p95 {p95:.5f}  m   (n={n})")
    g["seam_detail"] = {"decl_R": [(k, s) for k, s in decl],
                        "yardsticks": yard}

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"leg": legdir, "seed": seeddir, "meta": meta,
                       "rows": rows, "gates": g}, fh, indent=2, default=str)
        print(f"\n  JSON -> {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
