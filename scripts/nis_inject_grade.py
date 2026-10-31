#!/usr/bin/env python3
"""Grade the multi-satellite injection replays of the consumer innovation screen.

    python scripts/nis_inject_grade.py grade [--consumer wtzz ...] [--set core|ext|all]
                                             [--runs ROOT] [--json OUT] [--quiet]
    python scripts/nis_inject_grade.py validity [--consumer ...] [--set ...]     the validity test only
    python scripts/nis_inject_grade.py selftest

The replays are those laid out by scripts/nis_inject_arms.py: per consumer an unperturbed cell and
perturbed cells (class pb or clk, K satellites, a step in metres from TOW 270000), each replayed with the
screen off and on.  Run directories are `<runs root>/_rap_nis_<cell>_<off|on>`.

Measures (all against the matched unperturbed replay of the same consumer with the same screen setting,
epoch by epoch, from logs/closed_loop_trace.csv; excess = 3D error of the perturbed replay minus that of the
unperturbed one):
  peak20     the largest excess over the 40 epochs from TOW 270000 (20 min), 0 when none is positive
  recovery   the first epoch index n (0 at TOW 270000) from which the excess stays within 0.05 m (absolute) for
             10 consecutive epochs; None when it never does
  mean3h     the mean excess over the 360 epochs from TOW 270000 (3 h)
  flagged    the EXCLUDE and REFLOAT events of the screen, by key `<sat>_<signal>`, from logs/rt_engine.log;
             innocent = a flag at or after TOW 270000 on a key the step does not touch (class pb: a code
             row or a satellite not stepped; class clk: a satellite not stepped)

Validity (bar B0), read from the two screen-off sat traces (logs/closed_loop_sat_trace.csv) at TOW 270000 and
270030: every stepped satellite shows the step on a row of its own, either as an innovation that differs from
the unperturbed one by at least half the step (class pb the phase innovation; class clk the phase or the code
innovation) or as a row that the final update no longer holds (blank innovation where the unperturbed run has one:
the existing post-fit machinery peeled it), and no stepped row carries a consumer reaction token that the
unperturbed run does not carry at the same row and epoch (reset_*, carry_*, translate_*, reinflate_*,
flip_*, refloat_level), and, for class clk, clk_disc equals the unperturbed run's.  The replay recomputes the
phase-bias counter from the stepped values, which the consumer ignores on a held key; it is read as `pb_disc_recomputed`
and does not void a cell.  A cell that fails is void: reported, and in no bar.

"Identical" and "equals" compare the two logs/closed_loop_trace.csv byte for byte.

Bars (a cell that is valid; the exact text is the card's pre-registered section):
  B1 biting   screen-off peak20 above 0.10 m, or recovery above 10 epochs (or never)
  B2          every biting cell with K at most 3: screen-on peak20 at most half the screen-off one, recovery no
              longer, mean3h no larger
  B3          the unperturbed pair identical with no flagged row; a cell that does not bite: screen-on peak20 within
              0.01 m of screen-off; no innocent flag; each flagged stepped phase key that stays flagged is re-floated
              within 3 epochs of its first flag; a cell on which the screen abstained (and acted on nothing) equals
              screen-off epoch for epoch
"""
import argparse
import csv
import json
import math
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nis_inject_arms as arms  # noqa: E402

T_INJ = arms.T_INJ
DT = 30.0
W20_S = 1200.0            # 20 min
W3H_S = 10800.0           # 3 h
BAND_M = 0.05             # "back within 0.05 m"
BAND_N = 10               # ... for 10 consecutive epochs
BITE_PEAK_M = 0.10
BITE_REC_EP = 10
B3_PEAK_M = 0.01
REFLOAT_WITHIN_EP = 3
SAME_M = 5e-5             # closed_loop_trace.csv prints four decimals: equal to the last printed digit
SEEN_FRACTION = 0.5       # the step shows on a row when the innovation moved by this fraction of it

# ----------------------------------------------------------------------------- readers
LOG_TOW = re.compile(r"^\[(\d+), ([0-9.]+) \(")
EV_FLAG = re.compile(r"Innovation screen: (EXCLUDE|REFLOAT) (\S+) (\S+) w=([+-]?[0-9.]+) \(conditional sigma ([0-9.]+) m, "
                     r"innovation ([+-]?[0-9.]+) m\), flagged on (\d+) consecutive")
EV_OTHER = re.compile(r"Innovation screen: (READMIT|LOST|RELEASED) (\S+) (\S+)")
EV_ABSTAIN = re.compile(r"Innovation screen: ABSTAIN \((\w+)\), (\d+) row\(s\) flagged before it gave up;.*largest \|w\| ([0-9.]+)")
SUMMARY = re.compile(r"Consumer innovation screen \([^)]*\): epochs (\d+) / eligible rows (\d+) -> flagged (\d+) \(phase (\d+) / "
                     r"code (\d+)\) in (\d+) epoch\(s\), largest \|w\| ([0-9.]+), longest streak (\d+); abstained (\d+) "
                     r"\(too many (\d+) / too few (\d+) / numerical (\d+)\); skipped calls (\d+)")
CLOSURE = re.compile(r"exclusion episodes (\d+) -> readmitted (\d+) / re-floated (\d+) / lost (\d+) / released by an "
                     r"abstaining epoch (\d+) / still open (\d+); closure (HOLDS|BROKEN)")
SCREEN_TOKENS = ("screen_excl", "screen_refloat", "screen_readmit", "screen_lost", "screen_released")


def num(x):
    """Float of a CSV field, None when blank."""
    return None if x is None or x == "" else float(x)


def read_err(run):
    """{tow: 3D position error (m)} of logs/closed_loop_trace.csv; towns are rounded to 1 ms."""
    out = {}
    with open(run + "/logs/closed_loop_trace.csv", newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                out[round(float(r["tow"]), 3)] = float(r["pos_err_3d_m"])
            except (ValueError, KeyError, TypeError):
                continue
    return out


def read_sat_trace(run, tows=None):
    """Rows of logs/closed_loop_sat_trace.csv: ({(tow, sat, sig): row}, [(tow, sat, sig, column, token)]).
    `rows` holds only the epochs in `tows` (all when None); the second list holds every screen token."""
    rows, toks = {}, []
    with open(run + "/logs/closed_loop_sat_trace.csv", newline="") as fh:
        for r in csv.DictReader(fh):
            tow = round(float(r["tow"]), 3)
            for col in ("ev_pb", "ev_cb"):
                for t in r[col].split(";"):
                    if t.startswith("screen_"):
                        toks.append((tow, r["sat"], r["sig"] if col == "ev_pb" else r["code_sig"], col, t))
            if tows is None or tow in tows:
                row = {k: r[k] for k in ("ev_pb", "ev_cb", "ev_clk")}
                for k in ("clk_disc", "cb_disc", "pb_disc", "innov_code_m", "innov_phase_m", "amb_var", "elev_deg"):
                    row[k] = num(r[k])
                rows[(tow, r["sat"], r["sig"])] = row
    return rows, toks


def read_screen_log(run):
    """The screen's events and run-end counters from logs/rt_engine.log.
    {'flags': [(tow, kind, sat, sig, w, sigma, innovation, streak)], 'other': [(tow, kind, sat, sig)],
     'abstain': [(tow, reason, n_flagged, max_w)], 'summary': {...}, 'closure': {...}}"""
    out = {"flags": [], "other": [], "abstain": [], "summary": None, "closure": None}
    path = run + "/logs/rt_engine.log"
    if not os.path.exists(path):
        return out
    with open(path, errors="replace") as fh:
        for line in fh:
            if "Innovation screen" not in line and "innovation screen" not in line:
                continue
            m0 = LOG_TOW.match(line)
            tow = round(float(m0.group(2)), 3) if m0 else None
            m = EV_FLAG.search(line)
            if m:
                out["flags"].append((tow, m.group(1), m.group(2), m.group(3), float(m.group(4)), float(m.group(5)),
                                     float(m.group(6)), int(m.group(7))))
                continue
            m = EV_OTHER.search(line)
            if m:
                out["other"].append((tow, m.group(1), m.group(2), m.group(3)))
                continue
            m = EV_ABSTAIN.search(line)
            if m:
                out["abstain"].append((tow, m.group(1), int(m.group(2)), float(m.group(3))))
                continue
            m = SUMMARY.search(line)
            if m:
                keys = ("epochs", "rows", "flagged", "phase", "code", "epochs_flagged", "max_w", "max_streak",
                        "abstained", "abs_many", "abs_few", "abs_num", "skipped")
                out["summary"] = dict(zip(keys, [float(x) if "." in x else int(x) for x in m.groups()]))
                continue
            m = CLOSURE.search(line)
            if m:
                keys = ("episodes", "readmitted", "refloated", "lost", "released", "open")
                out["closure"] = dict(zip(keys, map(int, m.groups()[:6])), holds=(m.group(7) == "HOLDS"))
    return out


def run_dir(runs, cell, screen):
    return "%s/_rap_%s" % (runs, cell.tag(screen))


def run_done(rd):
    p = rd + "/exitcode.txt"
    return os.path.exists(p) and open(p).read().split()[:1] == ["0"] and os.path.exists(rd + "/logs/closed_loop_trace.csv")


# ----------------------------------------------------------------------------- metrics
def excess_series(pert, null, t0=T_INJ):
    """[(tow, excess)] for the epochs from t0 on that both runs hold, in time order."""
    return [(t, pert[t] - null[t]) for t in sorted(pert) if t >= t0 - 1e-6 and t in null]


def metrics(series, t0=T_INJ):
    """peak20, recovery (epoch index or None), mean3h and the epoch counts of one excess series."""
    w20 = [e for t, e in series if t < t0 + W20_S - 1e-6]
    w3h = [e for t, e in series if t < t0 + W3H_S - 1e-6]
    ex = [e for _, e in series]
    rec = None
    for n in range(len(ex) - BAND_N + 1):
        if all(abs(x) <= BAND_M for x in ex[n:n + BAND_N]):
            rec = n
            break
    return {"peak20": max(0.0, max(w20)) if w20 else None, "recovery": rec,
            "mean3h": sum(w3h) / len(w3h) if w3h else None, "n20": len(w20), "n3h": len(w3h), "n": len(ex),
            "max_abs": max((abs(x) for x in ex), default=None)}


def hourly_p50(err):
    """Median 3D error by hour, hours counted from the first row of the trace (the binning of
    scripts/clock_level/p50.py); an hour with 30 rows or fewer is skipped."""
    if not err:
        return []
    t0 = min(err)
    by = {}
    for t in sorted(err):
        by.setdefault(int(round((t - t0) * 1000)) // 3600000, []).append(err[t])
    out = []
    for h in sorted(by):
        v = sorted(by[h])
        if len(v) > 30:
            out.append(v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2]))
    return out


def same_trace(run_a, run_b):
    """True when the two runs' logs/closed_loop_trace.csv are the same bytes."""
    with open(run_a + "/logs/closed_loop_trace.csv", "rb") as fa, open(run_b + "/logs/closed_loop_trace.csv", "rb") as fb:
        return fa.read() == fb.read()


def max_diff(a, b, lo=None):
    """Largest |a - b| over the epochs both runs hold (from `lo` on when given), None when they share none."""
    d = [abs(a[t] - b[t]) for t in a if t in b and (lo is None or t >= lo - 1e-6)]
    return max(d) if d else None


# ----------------------------------------------------------------------------- validity (B0)
def tokens(field):
    return set() if field in ("", "none") else set(field.split(";"))


def validity(cls, sats, step_m, pert_rows, null_rows, t0=T_INJ):
    """Bar B0 from the two screen-off sat traces.  Returns (valid, reasons, info)."""
    reasons, info = [], {"pb_disc_recomputed": False, "shown": {}}
    cols = ("innov_phase_m",) if cls == "pb" else ("innov_phase_m", "innov_code_m")
    for sat in sats:
        shown = None
        n_rows = 0
        for t in (t0, t0 + DT):
            for (tt, s, sig), nrow in null_rows.items():
                if tt != t or s != sat:
                    continue
                n_rows += 1
                prow = pert_rows.get((tt, s, sig))
                if prow is None:
                    continue
                for c in ("ev_pb", "ev_cb", "ev_clk"):
                    extra = tokens(prow[c]) - tokens(nrow[c])
                    if extra:
                        reasons.append("reaction %s %s %s=%s at %.0f" % (s, sig, c, ";".join(sorted(extra)), tt))
                if cls == "clk" and prow["clk_disc"] != nrow["clk_disc"]:
                    reasons.append("clk_disc %s at %.0f: %s against %s" % (s, tt, prow["clk_disc"], nrow["clk_disc"]))
                if cls == "pb" and prow["pb_disc"] != nrow["pb_disc"]:
                    info["pb_disc_recomputed"] = True
                for c in cols:
                    a, b = prow[c], nrow[c]
                    if a is not None and b is not None and abs(a - b) >= SEEN_FRACTION * step_m:
                        shown = "visible"
                    elif a is None and b is not None and shown is None:
                        shown = "peeled"
        info["shown"][sat] = shown
        if n_rows == 0:
            reasons.append("%s has no row at TOW %.0f or %.0f" % (sat, t0, t0 + DT))
        elif shown is None:
            reasons.append("%s shows no step on any row at TOW %.0f or %.0f" % (sat, t0, t0 + DT))
    return (not reasons), sorted(set(reasons)), info


# ----------------------------------------------------------------------------- the screen's flags
def is_stepped_key(cls, sats, sat, sig):
    """A key the step touches: class pb the phase rows of the stepped satellites, class clk all their rows."""
    return sat in sats and (cls == "clk" or sig.startswith("L"))


def flag_summary(log, cls, sats, t0=T_INJ):
    """Flag counts by key, the innocent ones, the persistence rule and the abstention, from one screen log."""
    flags = [f for f in log["flags"] if f[0] is not None]
    after = [f for f in flags if f[0] >= t0 - 1e-6]
    by_key = {}
    for f in flags:
        by_key["%s_%s" % (f[2], f[3])] = by_key.get("%s_%s" % (f[2], f[3]), 0) + 1
    innocent = sorted({"%s_%s" % (f[2], f[3]) for f in after if not is_stepped_key(cls, sats, f[2], f[3])})
    first, refloat, late = {}, {}, []
    for f in after:
        key = (f[2], f[3])
        if key[1].startswith("L") and key[0] in sats:
            first.setdefault(key, f[0])
            if f[1] == "REFLOAT":
                refloat.setdefault(key, f[0])
    for key, t1 in sorted(first.items()):
        # a key flagged on 3 consecutive epochs is re-floated at the third: a gap or a readmission ends the episode
        gone = [o[0] for o in log["other"] if (o[2], o[3]) == key and o[0] is not None and o[0] >= t1 - 1e-6]
        t_r = refloat.get(key)
        if t_r is not None:
            if (t_r - t1) / DT > REFLOAT_WITHIN_EP + 1e-6:
                late.append("%s_%s: first flag %.0f, re-float %.0f" % (key[0], key[1], t1, t_r))
        elif not gone or (min(gone) - t1) / DT >= REFLOAT_WITHIN_EP - 1e-6:
            late.append("%s_%s: first flag %.0f, no re-float (episode %s)" % (key[0], key[1], t1,
                        "open" if not gone else "ended at %.0f" % min(gone)))
    abstain = [a for a in log["abstain"] if a[0] is not None and a[0] >= t0 - 1e-6]
    acted = any(f[1] in ("EXCLUDE", "REFLOAT") for f in after)
    return {"n_flags": len(flags), "n_flags_after": len(after), "by_key": by_key, "innocent": innocent,
            "stepped_phase_keys_flagged": len(first), "stepped_phase_keys_refloated": len(refloat), "refloat_late": late,
            "n_abstain": len(abstain), "abstain_reasons": sorted({a[1] for a in abstain}),
            "n_refloat": sum(1 for f in after if f[1] == "REFLOAT"), "acted": acted,
            "first_flag_tow": min((f[0] for f in after), default=None)}


def abstaining(fs):
    """The screen abstained at some epoch from the injection on and acted on nothing."""
    return fs["n_abstain"] > 0 and not fs["acted"]


# ----------------------------------------------------------------------------- one cell
def grade_cell(runs, cell, nulls):
    """All readings of one perturbed cell.  `nulls` = {screen: (run dir, err series)} of the same consumer."""
    sats = arms.sats_of(cell)
    rec = {"cell": cell.key, "consumer": cell.consumer, "cls": cell.cls, "k": cell.k, "step": cell.step_m, "sats": sats}
    rd = {s: run_dir(runs, cell, s) for s in ("off", "on")}
    missing = [s for s in rd if not run_done(rd[s])]
    if missing:
        rec["incomplete"] = missing
        return rec
    err = {s: read_err(rd[s]) for s in rd}
    # the replays hold the same history until the injection: a difference before it voids the comparison
    pre = {s: max_diff({t: v for t, v in err[s].items() if t < T_INJ - 1e-6}, nulls[s][1]) for s in rd}
    rec["pre_identical"] = all(v is not None and v < SAME_M for v in pre.values())
    for s in rd:
        rec[s] = metrics(excess_series(err[s], nulls[s][1]))
    # validity from the screen-off traces
    win = {round(T_INJ + d, 3) for d in (0.0, DT)}
    prow, _ = read_sat_trace(rd["off"], win)
    nrow, _ = read_sat_trace(nulls["off"][0], win)
    valid, reasons, info = validity(cell.cls, sats, cell.step_m, prow, nrow)
    if not rec["pre_identical"]:
        valid, reasons = False, reasons + ["history before the injection differs from the unperturbed replay"]
    rec["valid"], rec["void_reasons"], rec["pb_disc_recomputed"], rec["shown"] = valid, reasons, info["pb_disc_recomputed"], info["shown"]
    at0 = [(k, r) for k, r in nrow.items() if k[0] == T_INJ and k[1] in sats]
    rec["stepped_phase_rows"] = len(at0)
    rec["stepped_code_rows"] = sum(1 for k, r in at0 if r["innov_code_m"] is not None)
    rec["p50"] = {s: hourly_p50(err[s]) for s in rd}
    rec["p50"]["null_off"] = hourly_p50(nulls["off"][1])
    # bar B1
    off = rec["off"]
    rec["biting"] = bool(off["peak20"] is not None and (off["peak20"] > BITE_PEAK_M or off["recovery"] is None or off["recovery"] > BITE_REC_EP))
    # the screen's flags
    log = read_screen_log(rd["on"])
    fs = flag_summary(log, cell.cls, sats)
    rec["flags"], rec["closure"], rec["screen_summary"] = fs, log["closure"], log["summary"]
    _, toks = read_sat_trace(rd["on"], set())
    rec["trace_tokens"] = {t: sum(1 for x in toks if x[4] == t) for t in SCREEN_TOKENS}
    rec["tokens_match_log"] = fs["n_flags"] == rec["trace_tokens"]["screen_excl"] + rec["trace_tokens"]["screen_refloat"]
    rec["abstaining"] = abstaining(fs)
    rec["on_equals_off"] = same_trace(rd["on"], rd["off"])
    # bars
    on = rec["on"]
    b2 = None
    if rec["valid"] and rec["biting"] and cell.k <= 3:
        rec_ok = (off["recovery"] is None and True) or (on["recovery"] is not None and (off["recovery"] is None or on["recovery"] <= off["recovery"]))
        b2 = {"peak": on["peak20"] <= 0.5 * off["peak20"] + 1e-12, "recovery": bool(rec_ok), "mean": on["mean3h"] <= off["mean3h"] + 1e-12}
        b2["met"] = all(b2.values())
    rec["b2"] = b2
    b3 = {}
    if rec["valid"]:
        if not rec["biting"]:
            b3["peak_within_0.01"] = abs(on["peak20"] - off["peak20"]) <= B3_PEAK_M + 1e-12
        b3["no_innocent_flag"] = not fs["innocent"]
        b3["refloat_within_3"] = not fs["refloat_late"]
        if rec["abstaining"]:
            b3["abstaining_equals_off"] = rec["on_equals_off"]
        b3["met"] = all(b3.values())
    rec["b3"] = b3
    return rec


def grade_null(runs, cell):
    """The unperturbed pair of one consumer: identical epoch for epoch, nothing flagged.  Returns (record, nulls)."""
    rd = {s: run_dir(runs, cell, s) for s in ("off", "on")}
    rec = {"cell": cell.key, "consumer": cell.consumer}
    missing = [s for s in rd if not run_done(rd[s])]
    if missing:
        rec["incomplete"] = missing
        return rec, None
    err = {s: read_err(rd[s]) for s in rd}
    d = max_diff(err["on"], err["off"])
    log = read_screen_log(rd["on"])
    rec["max_abs_difference_m"] = d
    rec["epochs"] = len(err["on"])
    rec["identical"] = same_trace(rd["on"], rd["off"])
    rec["n_flags"] = len(log["flags"])
    rec["screen_summary"], rec["closure"] = log["summary"], log["closure"]
    rec["met"] = bool(rec["identical"] and rec["n_flags"] == 0)
    return rec, {s: (rd[s], err[s]) for s in rd}


# ----------------------------------------------------------------------------- verdict
def verdict(nulls, graded):
    """The pre-registered rule over every complete cell."""
    valid = [r for r in graded if r.get("valid")]
    biting = [r for r in valid if r["biting"]]
    b2_cells = [r for r in biting if r["k"] <= 3]
    misses = []
    misses += ["unperturbed pair %s: %s" % (n["cell"], "not identical" if not n.get("identical") else "%d row(s) flagged" % n["n_flags"])
               for n in nulls if not n.get("met")]
    misses += ["B2 %s" % r["cell"] for r in b2_cells if not r["b2"]["met"]]
    misses += ["B3 %s: %s" % (r["cell"], ", ".join(k for k, v in r["b3"].items() if k != "met" and not v)) for r in valid if not r["b3"]["met"]]
    if not biting:
        return "KEEP_OFF", "no valid cell bites", misses
    if misses:
        return "KEEP_OFF", "a bar is missed", misses
    if not b2_cells:
        return "KEEP_OFF", "the valid cells that bite all have K above 3, where the screen abstains by design", misses
    return "RECOMMEND_ARM_COLD", "every valid biting cell with K at most 3 meets B2 and every valid cell meets B3", misses


# ----------------------------------------------------------------------------- report
def fmt(x, nd=3):
    return "-" if x is None else ("%.*f" % (nd, x))


def report(nulls, graded, quiet=False):
    lines = []
    w = lines.append
    w("UNPERTURBED PAIRS (screen off against on)")
    for n in nulls:
        if "incomplete" in n:
            w("  %-12s incomplete: %s" % (n["cell"], ",".join(n["incomplete"])))
            continue
        w("  %-12s epochs %d  max |difference| %s m  flagged rows %d  %s" % (
            n["cell"], n["epochs"], fmt(n["max_abs_difference_m"], 5), n["n_flags"], "identical" if n["met"] else "NOT MET"))
    w("")
    w("CELLS  (valid = bar B0; bite = B1: off peak20 > 0.10 m or recovery > 10 epochs; pk20 m, rec epochs, mean3h m; "
      "flags = EXCLUDE+REFLOAT rows, inn = innocent keys)")
    w("%-20s %-3s %2s %7s %-5s %-6s | %6s %5s %7s | %6s %5s %7s | %5s %3s %4s %-9s %s" % (
        "cell", "cls", "K", "step", "valid", "bite", "off pk", "rec", "mean", "on pk", "rec", "mean", "flags", "inn", "refl", "abstain", "B2 B3"))
    for r in graded:
        if "incomplete" in r:
            w("%-20s incomplete: %s" % (r["cell"], ",".join(r["incomplete"])))
            continue
        off, on, fs = r["off"], r["on"], r["flags"]
        ab = "yes" if r["abstaining"] else ("part" if fs["n_abstain"] else "no")
        b2 = "-" if r["b2"] is None else ("ok" if r["b2"]["met"] else "MISS")
        b3 = "-" if not r["b3"] else ("ok" if r["b3"]["met"] else "MISS")
        w("%-20s %-3s %2d %7s %-5s %-6s | %6s %5s %7s | %6s %5s %7s | %5d %3d %4d %-9s %s %s" % (
            r["cell"], r["cls"], r["k"], r["step"], "yes" if r["valid"] else "VOID", "yes" if r["biting"] else "no",
            fmt(off["peak20"]), "-" if off["recovery"] is None else off["recovery"], fmt(off["mean3h"], 4),
            fmt(on["peak20"]), "-" if on["recovery"] is None else on["recovery"], fmt(on["mean3h"], 4),
            fs["n_flags_after"], len(fs["innocent"]), fs["n_refloat"], ab, b2, b3))
    w("")
    void = [r for r in graded if "incomplete" not in r and not r["valid"]]
    w("VOID CELLS: %d of %d complete" % (len(void), sum(1 for r in graded if "incomplete" not in r)))
    for r in void:
        w("  %s: %s" % (r["cell"], "; ".join(r["void_reasons"])))
    recomputed = sum(1 for r in graded if r.get("pb_disc_recomputed"))
    w("pb_disc recomputed by the replay in %d pb cells" % recomputed)
    done = [r for r in graded if "incomplete" not in r]
    miss = [r["cell"] for r in done if not r["tokens_match_log"]]
    w("log flags equal the sat-trace screen tokens in %d of %d cells%s" % (len(done) - len(miss), len(done), (": not in " + ", ".join(miss)) if miss else ""))
    w("")
    w("FLAGS BY KEY (screen on, whole run)")
    for r in graded:
        if "incomplete" in r or not r["flags"]["by_key"]:
            continue
        w("  %-20s %s%s%s" % (r["cell"], " ".join("%s:%d" % kv for kv in sorted(r["flags"]["by_key"].items())),
                              ("  INNOCENT " + ",".join(r["flags"]["innocent"])) if r["flags"]["innocent"] else "",
                              ("  LATE " + "; ".join(r["flags"]["refloat_late"])) if r["flags"]["refloat_late"] else ""))
    v, why, misses = verdict(nulls, graded)
    w("")
    w("VERDICT: %s (%s)" % (v, why))
    for m in misses:
        w("  miss: " + m)
    return "\n".join(lines), {"verdict": v, "reason": why, "misses": misses}


def md_cells(graded):
    """Markdown rows of the complete cells, for a card table."""
    out = ["| cell | valid | off: peak20 m / recovery / mean3h m | on: peak20 m / recovery / mean3h m | flagged rows (innocent) | refloats | B2 | B3 |",
           "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for r in graded:
        if "incomplete" in r:
            out.append("| %s | incomplete | | | | | | |" % r["cell"])
            continue
        off, on, fs = r["off"], r["on"], r["flags"]
        rc = lambda m: "never" if m["recovery"] is None else str(m["recovery"])
        out.append("| %s | %s | %s / %s / %s | %s / %s / %s | %d (%d) | %d | %s | %s |" % (
            r["cell"], "yes" if r["valid"] else "VOID", fmt(off["peak20"]), rc(off), fmt(off["mean3h"], 4),
            fmt(on["peak20"]), rc(on), fmt(on["mean3h"], 4), fs["n_flags_after"], len(fs["innocent"]), fs["n_refloat"],
            "-" if r["b2"] is None else ("met" if r["b2"]["met"] else "MISSED"),
            "-" if not r["b3"] else ("met" if r["b3"]["met"] else "MISSED")))
    return "\n".join(out)


def cmd_grade(a):
    runs = a.runs.rstrip("/")
    nulls, graded, null_runs = [], [], {}
    for c in arms.cells(a.set, a.consumer):
        if c.cls == "null":
            rec, nr = grade_null(runs, c)
            nulls.append(rec)
            null_runs[c.consumer] = nr
        elif null_runs.get(c.consumer):
            graded.append(grade_cell(runs, c, null_runs[c.consumer]))
        else:
            graded.append({"cell": c.key, "incomplete": ["null"]})
    text, v = report(nulls, graded)
    if not a.quiet:
        print(text)
    if a.md:
        with open(a.md, "w") as fh:
            fh.write(md_cells(graded) + "\n")
    if a.json:
        with open(a.json, "w") as fh:
            json.dump({"nulls": nulls, "cells": graded, "verdict": v}, fh, indent=1, default=str)


def cmd_validity(a):
    runs = a.runs.rstrip("/")
    for c in arms.cells(a.set, a.consumer):
        if c.cls == "null":
            continue
        rd, rn = run_dir(runs, c, "off"), run_dir(runs, arms.Cell(c.consumer, "null"), "off")
        if not (run_done(rd) and run_done(rn)):
            print("%-20s incomplete" % c.key)
            continue
        win = {round(T_INJ + d, 3) for d in (0.0, DT)}
        valid, reasons, info = validity(c.cls, arms.sats_of(c), c.step_m, read_sat_trace(rd, win)[0], read_sat_trace(rn, win)[0])
        print("%-20s %s%s  shown %s%s" % (c.key, "valid" if valid else "VOID ", "  pb_disc recomputed" if info["pb_disc_recomputed"] else "",
                                          " ".join("%s:%s" % kv for kv in info["shown"].items()),
                                          "" if valid else "  | " + "; ".join(reasons)))


# ----------------------------------------------------------------------------- selftest
def selftest():
    # metrics: a transient of 4 epochs (0.5 to 0.1 m), then within the band from the fifth epoch on
    ser = [(T_INJ + DT * i, e) for i, e in enumerate([0.5, 0.4, 0.2, 0.1] + [0.01] * 400)]
    m = metrics(ser)
    assert abs(m["peak20"] - 0.5) < 1e-12 and m["recovery"] == 4 and m["n20"] == 40 and m["n3h"] == 360, m
    assert abs(m["mean3h"] - (0.5 + 0.4 + 0.2 + 0.1 + 356 * 0.01) / 360) < 1e-12
    # a series that is better than the null has no positive peak; one that never recovers has recovery None
    m = metrics([(T_INJ + DT * i, -0.2) for i in range(60)])
    assert m["peak20"] == 0.0 and m["recovery"] is None and m["mean3h"] < 0
    # recovery needs 10 consecutive epochs inside the band, and counts from the first of them
    ex = [0.3, 0.01, 0.01, 0.01, 0.01, 0.2] + [0.0] * 20
    assert metrics([(T_INJ + DT * i, e) for i, e in enumerate(ex)])["recovery"] == 6
    assert metrics([(T_INJ + DT * i, 0.0) for i in range(30)])["recovery"] == 0
    assert metrics([(T_INJ + DT * i, 0.0) for i in range(9)])["recovery"] is None     # fewer than 10 epochs
    # excess pairs epochs by time and starts at the injection
    e = excess_series({T_INJ - 30: 1.0, T_INJ: 2.0, T_INJ + 30: 3.0}, {T_INJ - 30: 1.0, T_INJ: 1.5, T_INJ + 60: 0.0})
    assert e == [(T_INJ, 0.5)], e
    assert max_diff({1.0: 1.0, 2.0: 5.0}, {1.0: 1.0, 2.0: 5.00004}) < SAME_M and max_diff({1.0: 1}, {2.0: 1}) is None

    # validity: a step that shows as an innovation, a peeled row, a reaction token, a re-declared clock, a hidden step
    def row(ip=None, ic=None, pb=2, clk=0, ev=("none", "none", "none")):
        return {"ev_pb": ev[0], "ev_cb": ev[1], "ev_clk": ev[2], "clk_disc": clk, "cb_disc": 0, "pb_disc": pb,
                "innov_code_m": ic, "innov_phase_m": ip, "amb_var": 0.1, "elev_deg": 50.0}
    null = {}
    for t in (T_INJ, T_INJ + DT):
        for sat in ("G01", "E02"):
            for sig in ("L1W", "L2W"):
                null[(t, sat, sig)] = row(0.01, 0.02)
    pert = {k: dict(v) for k, v in null.items()}
    for t in (T_INJ, T_INJ + DT):                                 # pb 10 m on G01: visible on L1W, peeled on L2W
        pert[(t, "G01", "L1W")].update(innov_phase_m=-9.99, pb_disc=3)
        pert[(t, "G01", "L2W")].update(innov_phase_m=None, pb_disc=3)
        pert[(t, "E02", "L1W")].update(innov_phase_m=-9.5)
    ok, why, info = validity("pb", ["G01", "E02"], 10.0, pert, null)
    assert ok and not why and info["pb_disc_recomputed"] and info["shown"] == {"G01": "visible", "E02": "visible"}, (why, info)
    bad = {k: dict(v) for k, v in pert.items()}
    bad[(T_INJ, "G01", "L1W")]["ev_pb"] = "reset_disc"
    ok, why, _ = validity("pb", ["G01", "E02"], 10.0, bad, null)
    assert not ok and any("reaction G01 L1W ev_pb=reset_disc" in x for x in why), why
    same_tok = {k: dict(v) for k, v in pert.items()}                   # a token the unperturbed run carries too is not a reaction
    null_tok = {k: dict(v) for k, v in null.items()}
    same_tok[(T_INJ, "G01", "L1W")]["ev_cb"] = null_tok[(T_INJ, "G01", "L1W")]["ev_cb"] = "reinflate_disc"
    assert validity("pb", ["G01", "E02"], 10.0, same_tok, null_tok)[0]
    hidden = {k: dict(v) for k, v in pert.items()}
    for t in (T_INJ, T_INJ + DT):
        hidden[(t, "E02", "L1W")]["innov_phase_m"] = 0.012
    ok, why, _ = validity("pb", ["G01", "E02"], 10.0, hidden, null)
    assert not ok and any(x.startswith("E02 shows no step") for x in why), why
    clk = {k: dict(v) for k, v in null.items()}
    for t in (T_INJ, T_INJ + DT):
        clk[(t, "G01", "L1W")].update(innov_phase_m=8.0, innov_code_m=7.9)
        clk[(t, "G01", "L2W")].update(innov_code_m=-8.1)               # the code innovation alone shows it on L2W
    assert validity("clk", ["G01"], 10.0, clk, null)[0]
    clk[(T_INJ, "G01", "L1W")]["clk_disc"] = 1
    ok, why, _ = validity("clk", ["G01"], 10.0, clk, null)
    assert not ok and any(x.startswith("clk_disc G01") for x in why), why
    assert not validity("pb", ["G77"], 10.0, pert, null)[0]            # a satellite with no row

    # the screen's flags: parse real log lines, keys, innocent flags and the persistence rule
    lines = [
        "[2313, 270000.000 (2024-05-08 03:00:00.000)] [rt_engine] [info] [RT] Innovation screen: EXCLUDE G01 L1W w=-268.4 (conditional sigma 0.344 m, innovation -65.261 m), flagged on 1 consecutive epoch(s)",
        "[2313, 270000.000 (2024-05-08 03:00:00.000)] [rt_engine] [info] [RT] Innovation screen: EXCLUDE G01 L2W w=-100.0 (conditional sigma 0.4 m, innovation -9.0 m), flagged on 1 consecutive epoch(s)",
        "[2313, 270030.000 (2024-05-08 03:00:30.000)] [rt_engine] [info] [RT] Innovation screen: EXCLUDE G01 L1W w=-268.5 (conditional sigma 0.344 m, innovation -66.4 m), flagged on 2 consecutive epoch(s)",
        "[2313, 270030.000 (2024-05-08 03:00:30.000)] [rt_engine] [info] [RT] Innovation screen: READMIT G01 L2W passes again after 1 flagged epoch(s)",
        "[2313, 270060.000 (2024-05-08 03:01:00.000)] [rt_engine] [info] [RT] Innovation screen: REFLOAT G01 L1W w=-268.6 (conditional sigma 0.344 m, innovation -66.8 m), flagged on 3 consecutive epoch(s): the ambiguity is re-floated and the row returns to the update",
        "[2313, 270060.000 (2024-05-08 03:01:00.000)] [rt_engine] [info] [RT] Innovation screen: re-floated G01 L1W at std 1000.000 m (ok)",
        "[2313, 270090.000 (2024-05-08 03:01:30.000)] [rt_engine] [info] [RT] Innovation screen: EXCLUDE G09 C1W w=+7.0 (conditional sigma 1.1 m, innovation +9.0 m), flagged on 1 consecutive epoch(s)",
        "[2313, 270120.000 (2024-05-08 03:02:00.000)] [rt_engine] [info] [RT] Innovation screen: ABSTAIN (too_many_rows_flagged), 16 row(s) flagged before it gave up; the epoch goes to the update unscreened, largest |w| 120.5",
        "[2313, 270120.000 (2024-05-08 03:02:00.000)] [rt_engine] [info] [RT] Innovation screen: RELEASED G09 C1W (the screen abstained, the row is in the update; 1 flagged epoch(s))",
        "[2313, 280770.000 (2024-05-08 05:59:30.000)] [rt_engine] [info] [RT] Consumer innovation screen (end of replay, mode 'act', rows 'all', k 6.00, max fraction 0.25, re-float after 3 ep): epochs 720 / eligible rows 40952 -> flagged 5 (phase 4 / code 1) in 4 epoch(s), largest |w| 268.6, longest streak 3; abstained 1 (too many 1 / too few 0 / numerical 0); skipped calls 0",
        "[2313, 280770.000 (2024-05-08 05:59:30.000)] [rt_engine] [info] [RT] Consumer innovation screen (end of replay): exclusion episodes 3 -> readmitted 1 / re-floated 1 / lost 0 / released by an abstaining epoch 1 / still open 0; closure HOLDS",
    ]
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(td + "/logs")
        with open(td + "/logs/rt_engine.log", "w") as fh:
            fh.write("\n".join(lines) + "\n")
        log = read_screen_log(td)
    assert len(log["flags"]) == 5 and len(log["other"]) == 2 and len(log["abstain"]) == 1, log
    assert log["flags"][0][:4] == (270000.0, "EXCLUDE", "G01", "L1W") and abs(log["flags"][0][6] + 65.261) < 1e-9
    assert log["summary"]["flagged"] == 5 and log["summary"]["abs_many"] == 1 and log["closure"]["holds"] and log["closure"]["open"] == 0
    fs = flag_summary(log, "pb", ["G01"])
    assert fs["by_key"] == {"G01_L1W": 3, "G01_L2W": 1, "G09_C1W": 1}, fs["by_key"]
    assert fs["innocent"] == ["G09_C1W"]                                # a satellite that was not stepped
    assert fs["stepped_phase_keys_flagged"] == 2 and fs["stepped_phase_keys_refloated"] == 1
    assert fs["refloat_late"] == [], fs["refloat_late"]                 # L1W re-floated at the third epoch, L2W readmitted at the second
    assert fs["n_abstain"] == 1 and fs["acted"] and not abstaining(fs)
    assert flag_summary(log, "clk", ["G01", "G09"])["innocent"] == []   # a clock step touches the code rows of its satellites
    assert flag_summary(log, "pb", ["G01", "G09"])["innocent"] == ["G09_C1W"]   # a phase-bias step does not
    late = {"flags": [(270000.0, "EXCLUDE", "G01", "L1W", -9.0, 0.3, -9.0, 1)] +
                     [(270000.0 + DT * i, "EXCLUDE", "G01", "L1W", -9.0, 0.3, -9.0, i + 1) for i in (1, 2, 3)],
            "other": [], "abstain": [], "summary": None, "closure": None}
    fs = flag_summary(late, "pb", ["G01"])
    assert len(fs["refloat_late"]) == 1 and "no re-float" in fs["refloat_late"][0], fs["refloat_late"]   # flagged and never re-floated
    ab = {"flags": [], "other": [], "abstain": [(270000.0, "too_many_rows_flagged", 16, 99.0)], "summary": None, "closure": None}
    assert abstaining(flag_summary(ab, "clk", ["G01"]))
    assert math.isclose(SEEN_FRACTION * 10.0, 5.0)

    # the verdict: no biting cell, a clean win, and a miss
    def cellrec(name, k, valid, biting, b2met, b3met):
        return {"cell": name, "k": k, "valid": valid, "biting": biting,
                "b2": None if not (valid and biting and k <= 3) else {"met": b2met},
                "b3": {"met": b3met, "x": b3met} if valid else {}}
    ok_null = [{"cell": "a_null", "met": True, "identical": True, "n_flags": 0}]
    assert verdict(ok_null, [cellrec("c1", 1, True, False, None, True)])[0] == "KEEP_OFF"
    assert verdict(ok_null, [cellrec("c1", 1, False, True, None, None)])[0] == "KEEP_OFF"            # void cells do not count
    assert verdict(ok_null, [cellrec("c1", 1, True, True, True, True), cellrec("c2", 6, True, True, None, True)])[0] == "RECOMMEND_ARM_COLD"
    assert verdict(ok_null, [cellrec("c1", 1, True, True, False, True)])[0] == "KEEP_OFF"           # B2 miss
    assert verdict(ok_null, [cellrec("c1", 1, True, True, True, False)])[0] == "KEEP_OFF"           # B3 miss
    assert verdict(ok_null, [cellrec("c6", 6, True, True, None, True)])[1].startswith("the valid cells that bite all have K above 3")
    assert verdict([{"cell": "a_null", "met": False, "identical": False, "n_flags": 0}], [cellrec("c1", 1, True, True, True, True)])[0] == "KEEP_OFF"
    print("nis_inject_grade selftest: ok")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("grade", cmd_grade), ("validity", cmd_validity)):
        p = sub.add_parser(name)
        p.add_argument("--consumer", nargs="+", choices=list(arms.CONSUMERS))
        p.add_argument("--set", choices=("core", "ext", "all"), default="all")
        p.add_argument("--runs", default=arms.RUNS)
        if name == "grade":
            p.add_argument("--json")
            p.add_argument("--md", help="write the cell table as markdown rows to this file")
            p.add_argument("--quiet", action="store_true")
        p.set_defaults(fn=fn)
    p = sub.add_parser("selftest")
    p.set_defaults(fn=lambda a: selftest())
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
