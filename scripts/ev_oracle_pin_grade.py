#!/usr/bin/env python3
"""ORACLE PIN grader — gates G1..G4 of docs/SPEC_oracle_pin.md.

Every cell comes from a TRUTH grader, never a raw wire step (standing invariant
#5 / the Psychas single-realization mandate): a raw step confounds benign frame
drift with a real defect, and the whole point of this card is to read WHERE the
error went once the ambiguities are held.

    g1  OFF byte-identity   opin_null_base vs opin_null
    g2  actuation           pinned chords must read integer at pin+1
    g3  the six-column table, both windows      -> markdown + runs/opin_table.json
    g4  slipped seed        the pin FOLLOWS N(t) through a truth slip

Windows (pre-registered, §17): epochs +28..+32 and +53..+57 of the 60-epoch leg,
i.e. pin+23..+27 and pin+48..+52 with the pin at +5.  Both are expressed as TOW
ranges off each leg's own `first_tow` (leg_meta.json), so the slipped-seed legs
use the same relative windows at their own absolute epoch.

    python scripts/ev_oracle_pin_grade.py g1
    python scripts/ev_oracle_pin_grade.py g2 --leg opin_all --leg opin_l12
    python scripts/ev_oracle_pin_grade.py g3
    python scripts/ev_oracle_pin_grade.py g4
"""
import argparse
import csv
import io
import json
import os
import re
import statistics
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
REPO = os.path.dirname(_HERE)

import simnet_grade as SG                     # noqa: E402
import simnet_amb_integer as SAI              # noqa: E402

EPOCH_S = 30.0
WINDOWS = {"W1": (28, 32), "W2": (53, 57)}    # epochs relative to the resume
G2_EP = 6                                     # pin (+5) + 1

QUIET_FIXTURE = "C:/GnssProcessor/runs/simnet60_quiet_wu93_24h"
SLIP_FIXTURE = "C:/GnssProcessor/runs/simnet60_campaign_wu93"

TABLE_LEGS = [("none", "opin_null"), ("all", "opin_all"),
              ("L1C+L2W", "opin_l12"), ("L5Q", "opin_l5"),
              ("sta S21", "opin_s21"), ("sat G08", "opin_g08")]


# ------------------------------------------------------------------ utilities
def legdir(name):
    return os.path.join(REPO, "runs", f"evleg_{name}")


def meta(name):
    with open(os.path.join(legdir(name), "leg_meta.json"), encoding="utf-8") as fh:
        return json.load(fh)


def window_tows(name, key):
    """(lo, hi) GPS seconds-of-week for window `key` of leg `name`, inclusive."""
    m = meta(name)
    lo_ep, hi_ep = WINDOWS[key]
    t0 = m["first_tow"]
    return (t0 + lo_ep * EPOCH_S - 0.25, t0 + hi_ep * EPOCH_S + 0.25)


def fixture_of(name):
    return meta(name).get("opin_fixture") or QUIET_FIXTURE


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    if len(v) == 1:
        return v[0]
    i = p * (len(v) - 1) / 100.0
    lo = int(i)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


def rms(v):
    return (sum(x * x for x in v) / len(v)) ** 0.5 if v else None


def f(v, d=4):
    return "-" if v is None else f"{v:.{d}f}"


def oracle_clause(name):
    p = os.path.join(legdir(name), "stdout.txt")
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "Network (ORACLE PIN):" in line and "active epoch(s)" in line:
                return line.rstrip("\n")
    return None


# ------------------------------------------------------------------------- G1
G1_FILES = ["products.txt",
            "logs/network_dd_fractional.csv", "logs/network_zwd.csv",
            "logs/closed_loop_trace.csv", "logs/network_ionosphere.csv",
            "logs/network_rx_states.csv", "logs/network_events.csv"]

# A run-end counter line is one that carries a count and no wall-clock figure.
# Timing, paths and the per-epoch section timers are the ONLY legal differences
# between two byte-identical solves; everything else must match character for
# character (memory `feedback_stale_trace_dir_fake_ab`: an A/B that compares a
# stale trace directory reads as byte-identity and is worth nothing, so the file
# mtimes are asserted to post-date the launch as well).
_TIMEY = re.compile(r"\bms\b|\bwall\b|elapsed|\(D0b\)|\(T2b\)|per epoch|section timers"
                    r"|µs|\bsec\b|\d+\.\d+\s*s\b|\bin \d+\s*s\b", re.I)


def _counter_lines(path, legname):
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            l = re.sub(r"^\[[^\]]*\]\s*", "", line.rstrip("\n"))
            if _TIMEY.search(l):
                continue
            l = l.replace(legname, "<LEG>")
            if re.search(r"\d", l):
                out.append(l)
    return out


def cmd_g1(args):
    a, b = "opin_null_base", "opin_null"
    ok = True
    print("== G1  OFF byte-identity  (opin_null_base = UNMODIFIED exe) ==")
    import hashlib
    launch = min(os.path.getmtime(os.path.join(legdir(x), "leg.json")) for x in (a, b))
    for rel in G1_FILES:
        pa, pb = os.path.join(legdir(a), rel), os.path.join(legdir(b), rel)
        if not (os.path.isfile(pa) and os.path.isfile(pb)):
            print(f"  {rel:40s} MISSING"); ok = False; continue
        ha = hashlib.sha256(open(pa, "rb").read()).hexdigest()
        hb = hashlib.sha256(open(pb, "rb").read()).hexdigest()
        fresh = os.path.getmtime(pa) > launch and os.path.getmtime(pb) > launch
        same = ha == hb
        ok = ok and same and fresh
        print(f"  {rel:40s} {'IDENTICAL' if same else 'DIFFER':10s} "
              f"{'fresh' if fresh else '*** STALE ***':14s} {os.path.getsize(pa):>10d} B  "
              f"{ha[:16]}")
    ca = _counter_lines(os.path.join(legdir(a), "stdout.txt"), a)
    cb = _counter_lines(os.path.join(legdir(b), "stdout.txt"), b)
    ndiff = sum(1 for x, y in zip(ca, cb) if x != y) + abs(len(ca) - len(cb))
    print(f"  run-end/counter log lines               "
          f"{'IDENTICAL' if ndiff == 0 else f'{ndiff} DIFFER':10s} "
          f"({len(ca)} lines compared, timing/paths excluded)")
    if ndiff:
        ok = False
        for x, y in zip(ca, cb):
            if x != y:
                print(f"    A: {x[:150]}\n    B: {y[:150]}")
                break
    cl = oracle_clause(b)
    print(f"  ORACLE PIN clause on the null leg       "
          f"{'ABSENT (correct)' if cl is None else '*** PRESENT ***'}")
    if cl is not None:
        ok = False
    print(f"\nG1: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


# ------------------------------------------------------- truth / trace joins
def load_truth_amb(fixture, stations):
    """(sta, sat, sig) -> (N_int, [(tow_s, delta)]) from the fixture."""
    n_int, slips = {}, {}
    for sta in stations:
        p = os.path.join(fixture, sta, "truth_ambiguities.csv")
        if not os.path.isfile(p):
            continue
        with open(p, newline="") as fh:
            for r in csv.DictReader(fh):
                n_int[(sta, r["satellite"], r["signal"])] = int(round(float(r["N_int"])))
        p = os.path.join(fixture, sta, "truth_slips.csv")
        if os.path.isfile(p):
            with open(p, newline="") as fh:
                for r in csv.DictReader(fh):
                    slips.setdefault((sta, r["satellite"], r["signal"]), []).append(
                        (float(r["tow_s"]), int(round(float(r["delta_cycles"])))))
    for v in slips.values():
        v.sort()
    return n_int, slips


def n_true(n_int, slips, sta, sat, sig, tow):
    base = n_int.get((sta, sat, sig))
    if base is None:
        return None
    for t, d in slips.get((sta, sat, sig), ()):
        if t <= tow + 1e-6:
            base += d
    return base


def dd_rows(name, lo, hi):
    """DD trace rows inside [lo, hi], as dicts."""
    p = os.path.join(legdir(name), "logs", "network_dd_fractional.csv")
    out = []
    with open(p, newline="") as fh:
        for r in csv.DictReader(fh):
            t = float(r["tow"])
            if lo <= t <= hi:
                out.append(r)
    return out


def pinned_set(name, at_tow=None):
    """(sta, sat, sig) PINNED at `at_tow` (default: at the end of the leg).

    Reconstructed from the producer's own per-arc ENTRY (`pinned`) and EXIT
    (`released`) rows in `network_events.csv` — what the producer DID, not what
    the scope asked for.  The difference is exactly the no-anchor /
    no-station-term population the run-end clause counts.

    ⚠ The ONSET rows alone are NOT the membership: arcs join as satellites rise
    (7 828 at onset vs 9 381 at the run end on the first quiet arm), so a grade
    built on them reads a working pin as a failure.  Both edges are needed, which
    is why both are emitted.
    """
    p = os.path.join(legdir(name), "logs", "network_events.csv")
    out = set()
    if not os.path.isfile(p):
        return out
    with open(p, newline="") as fh:
        for r in csv.DictReader(fh):
            if r.get("event") != "oracle_pin":
                continue
            act = r.get("action")
            if act not in ("pinned", "released"):
                continue
            try:
                if at_tow is not None and float(r["tow"]) > at_tow + 0.25:
                    break                       # rows are in epoch order
            except (KeyError, ValueError):
                continue
            m = re.search(r"signal=([^;]+)", r.get("detail") or "")
            if not m:
                continue
            k = (r.get("station"), r.get("sat"), m.group(1))
            if act == "pinned":
                out.add(k)
            else:
                out.discard(k)
    return out


# ------------------------------------------------------------------------- G2
def dd_tows(name):
    """Distinct tows present in the leg's DD trace, ascending."""
    p = os.path.join(legdir(name), "logs", "network_dd_fractional.csv")
    seen, prev = [], None
    with open(p, newline="") as fh:
        fh.readline()
        for line in fh:
            i = line.find(",")
            j = line.find(",", i + 1)
            t = line[i + 1:j]
            if t != prev:
                prev = t
                seen.append(float(t))
    return sorted(set(seen))


def cmd_g2(args):
    names = args.leg or [n for _c, n in TABLE_LEGS if n != "opin_null"]
    ok = True
    miss = []
    print("== G2  actuation — pinned chords must read the true integer DD ==")
    print("   A chord counts as PINNED when all FOUR of its legs (both stations x "
          "sat_i/ref_sat)\n   are in the leg's own pinned set (read from the onset "
          "event rows, i.e. what the\n   producer DID, not what the scope asked for).")
    print("   ⚠ The read epoch is the first EMITTED one at or after pin+1, not pin+1 "
          "itself:\n     yanking the ambiguities to truth is a metre-class prefit "
          "inconsistency, so the\n     standing A4 σ̅0 withhold ceiling withholds the "
          "onset epochs while the rest of\n     the state moves.  The withheld count is "
          "printed per leg — it is a RESULT, not a\n     grading convenience.\n")
    print(f"{'leg':14s} {'read@':>7s} {'wh':>3s} {'sig':5s} {'pop':>6s} {'p50':>9s} "
          f"{'p90':>9s} {'max':>9s} {'<0.01':>7s}   unpinned p50/p90/max (pop)")
    for name in names:
        m = meta(name)
        t_want = m["first_tow"] + (args.at_ep if args.at_ep is not None
                                   else G2_EP) * EPOCH_S
        tows = dd_tows(name)
        later = [x for x in tows if x >= t_want - 0.25]
        if not later:
            print(f"{name:14s} *** no DD epoch at or after pin+1 ***")
            ok = False
            continue
        t = later[0]
        withheld = sum(1 for k in range(G2_EP, int((t - m["first_tow"]) / EPOCH_S))
                       if abs(m["first_tow"] + k * EPOCH_S - t) > 0.25)
        rel = int(round((t - m["first_tow"]) / EPOCH_S)) - 5      # epochs after the pin
        rows = dd_rows(name, t - 0.25, t + 0.25)
        fx = fixture_of(name)
        stas = sorted({r["sta_a"] for r in rows} | {r["sta_b"] for r in rows})
        n_int, slips = load_truth_amb(fx, stas)
        pins = pinned_set(name, t)
        per = {}
        for r in rows:
            sig, sat, ref = r["signal"], r["sat_i"], r["ref_sat"]
            sa, sb = r["sta_a"], r["sta_b"]
            legs = [n_true(n_int, slips, sa, sat, sig, t),
                    n_true(n_int, slips, sa, ref, sig, t),
                    n_true(n_int, slips, sb, sat, sig, t),
                    n_true(n_int, slips, sb, ref, sig, t)]
            if any(v is None for v in legs):
                continue
            dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])
            e = abs(float(r["dd_cyc"]) - dd_true)
            four = [(sa, sat, sig), (sa, ref, sig), (sb, sat, sig), (sb, ref, sig)]
            key = "pinned" if all(k in pins for k in four) else "free"
            per.setdefault((sig, key), []).append(e)
        for sig in sorted({k[0] for k in per}):
            p = per.get((sig, "pinned"), [])
            q = per.get((sig, "free"), [])
            frac = (sum(1 for x in p if x < 0.01) / len(p)) if p else None
            if p and frac is not None and frac < 0.95:
                ok = False
                miss.append(f"{name}/{sig} {100*frac:.1f}% (max {max(p):.4f} cyc)")
            print(f"{name:14s} {('+'+str(rel)):>7s} {withheld:3d} {sig:5s} "
                  f"{len(p):6d} {f(pct(p,50)):>9s} "
                  f"{f(pct(p,90)):>9s} {f(max(p) if p else None):>9s} "
                  f"{('-' if frac is None else f'{100*frac:.1f}%'):>7s}   "
                  f"{f(pct(q,50)):>8s} {f(pct(q,90)):>8s} "
                  f"{f(max(q) if q else None):>8s} ({len(q)})")
    print(f"\nG2: {'PASS' if ok else 'FAIL'}  "
          "(bar: >= 95 % of pinned chords under 0.01 cyc)")
    for x in miss:
        print(f"     under the bar: {x}")
    return 0 if ok else 1


# ------------------------------------------------------------ the G3 channels
def iono_row(name, lo, hi):
    """Slant iono per LOS vs truth_atmo.csv I_L1_m (ALREADY slant).

    The join simnet_iono_interaction.py uses: network_ionosphere.csv's
    `iono_total_m` (= x_lin + x_r, the ABSOLUTE slant estimate — never `iono_m`,
    which is the correction alone) against (tow, station, sat).
    """
    p = os.path.join(legdir(name), "logs", "network_ionosphere.csv")
    if not os.path.isfile(p):
        return None
    fx = fixture_of(name)
    keep, seen_sta = {}, set()
    with open(p, newline="") as fh:
        rd = csv.DictReader(fh)
        if "iono_total_m" not in (rd.fieldnames or []):
            return None
        for r in rd:
            t = float(r["tow"])
            if not (lo <= t <= hi):
                continue
            keep[(int(round(t)), r["station"], r["sat"])] = float(r["iono_total_m"])
            seen_sta.add(r["station"])
    if not keep:
        return None
    err = []
    for sta in sorted(seen_sta):
        a = os.path.join(fx, sta, "truth_atmo.csv")
        if not os.path.isfile(a):
            continue
        with open(a, newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    k = (int(round(float(r["tow_s"]))), sta, r["satellite"])
                except (KeyError, ValueError):
                    continue
                if k in keep:
                    err.append(keep[k] - float(r["I_L1_m"]))
    if not err:
        return None
    ae = [abs(e) for e in err]
    return {"n": len(err), "rms_m": rms(err), "p50_m": pct(ae, 50),
            "p90_m": pct(ae, 90), "bias_m": statistics.mean(err)}


def zwd_row(name, lo, hi):
    p = os.path.join(legdir(name), "logs", "network_zwd.csv")
    if not os.path.isfile(p):
        return None
    fx = fixture_of(name)
    keep, stas = {}, set()
    with open(p, newline="") as fh:
        for r in csv.DictReader(fh):
            t = float(r["tow"])
            if lo <= t <= hi:
                keep[(int(round(t)), r["station"])] = float(r["zwd_m"])
                stas.add(r["station"])
    if not keep:
        return None
    err = []
    for sta in sorted(stas):
        a = os.path.join(fx, sta, "truth_tropo.csv")
        if not os.path.isfile(a):
            continue
        with open(a, newline="") as fh:
            for r in csv.DictReader(fh):
                k = (int(round(float(r["tow_s"]))), sta)
                if k in keep:
                    err.append(keep[k] - float(r["zwd_m"]))
    if not err:
        return None
    ae = [abs(e) for e in err]
    return {"n": len(err), "rms_m": rms(err), "p50_m": pct(ae, 50),
            "p90_m": pct(ae, 90)}


def consumer_row(name, lo, hi):
    p = os.path.join(legdir(name), "logs", "closed_loop_trace.csv")
    if not os.path.isfile(p):
        return None
    e3, up = [], []
    with open(p, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                t = float(r["tow"])
            except (KeyError, ValueError):
                continue
            if not (lo <= t <= hi):
                continue
            if (r.get("pos_err_3d_m") or "").strip():
                e3.append(float(r["pos_err_3d_m"]))
            if (r.get("up_m") or "").strip():
                up.append(abs(float(r["up_m"])))
    if not e3:
        return None
    return {"n": len(e3), "p50_3d_m": pct(e3, 50), "p90_3d_m": pct(e3, 90),
            "p50_up_m": pct(up, 50)}


def sg_rows(name, lo, hi):
    """The simnet_grade channels (sat clock, code bias, rx states) in-window.

    ⚠ `network_fixture.json` stores `truth_dir` RELATIVE to the MAIN checkout
    (`runs/<fixture>/<STA>`), so `SG.load_truth` silently returns EMPTY truth when
    the cwd is a worktree — and `grade_rx_states` then reports `n_stations_truth
    = 0` with every contrast None, which reads exactly like a channel the run did
    not emit.  chdir to the checkout the fixture lives under for the duration.
    """
    d = legdir(name)
    fx = fixture_of(name)
    fx_root = os.path.dirname(os.path.dirname(os.path.abspath(fx)))
    cwd = os.getcwd()
    SG.set_tow_window(lo, hi)
    os.chdir(fx_root)
    try:
        truth = SG.load_truth(fx)
        s_line, epochs, _census = SG.parse_products(os.path.join(d, "products.txt"))
        epochs = [e for e in epochs if lo <= float(e["tow"]) <= hi]
        sc_truth, _ = SG.satclock_truth_by_tow(truth)
        osb_truth, _ = SG.osb_truth_by_key(truth)
        if_lump = SG.if_lump_by_sat(osb_truth, (s_line or {}).get("cbpin", {}))
        s2z = bool(s_line) and s_line.get("isb_datum") == "1"
        # settle_frac 0: the WINDOW is the settling choice.
        clk = SG.grade_clock(epochs, s_line, sc_truth, 0.0, if_lump, s2z=s2z)
        sig_rows = SG.load_csv(os.path.join(d, "logs", "network_sat_bias_states.csv"))
        cb = SG.grade_code_bias(epochs, s_line, osb_truth, sig_rows, 0.0)
        rx = SG.grade_rx_states(d, truth, 0.0)
    finally:
        os.chdir(cwd)
        SG.set_tow_window(None, None)
    return {"clk": clk, "cb": cb, "rx": rx, "n_epochs": len(epochs)}


def amb_row(name, lo, hi):
    """REF-FREE lane / band grade (SPEC_amb_iono §6.12: the DD trace has ONE
    reference satellite and it is in every chord, so a raw DD grade reports that
    satellite's own error)."""
    ns = argparse.Namespace(
        fixture_dir=fixture_of(name), run=legdir(name), last_frac=1.0,
        sigma_ratio=False, by_age=False, by_age_minlen=200, iono_join=False,
        stride=4, gal_lanes=False, ref_free=True, check_lambda=False,
        warmup=0, tow_min=lo, tow_max=hi)
    buf = io.StringIO()
    try:
        res = SAI.grade(ns, out=buf)
    except SystemExit as e:
        return None, f"{e}"
    finally:
        SAI.set_tow_window(None, None)
    return res, buf.getvalue()


def _dig(d, *path, default=None):
    for k in path:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return d


def cmd_g3(args):
    out = {"windows": {}, "legs": {}}
    for wkey in WINDOWS:
        cells = {}
        for col, name in TABLE_LEGS:
            lo, hi = window_tows(name, wkey)
            cells[col] = {
                "leg": name, "tow": [lo, hi],
                "iono": iono_row(name, lo, hi),
                "zwd": zwd_row(name, lo, hi),
                "consumer": consumer_row(name, lo, hi),
            }
            try:
                cells[col]["sg"] = sg_rows(name, lo, hi)
            except Exception as ex:                       # noqa: BLE001
                cells[col]["sg"] = {"error": repr(ex)}
            res, txt = amb_row(name, lo, hi)
            cells[col]["amb"] = res if isinstance(res, dict) else None
            cells[col]["amb_text"] = txt if not isinstance(res, dict) else None
        out["windows"][wkey] = cells
    for _c, name in TABLE_LEGS:
        out["legs"][name] = {"clause": oracle_clause(name), "meta": meta(name)}
    jp = os.path.join(REPO, "runs", "opin_table.json")
    with open(jp, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1, default=str)
    render_table(out)
    print(f"\nJSON: {jp}")
    return 0


def _amb(cell, block, key, idx):
    """One field of simnet_amb_integer's stats() tuple
    (n, med e, med |e|, p90 |e|, %<=0.10 cyc, %<=0.25 cyc, % round(e)==0)."""
    v = _dig(cell, "amb", block, key)
    if not isinstance(v, (list, tuple)) or len(v) <= idx:
        return None
    return v[idx]


def _rowvals(cells, fn):
    return [fn(cells[c]) for c, _n in TABLE_LEGS]


def render_table(out):
    hdr = [c for c, _n in TABLE_LEGS]
    for wkey, cells in out["windows"].items():
        lo, hi = cells["none"]["tow"]
        ep = WINDOWS[wkey]
        print(f"\n### window {wkey} — epochs +{ep[0]}..+{ep[1]} "
              f"(pin+{ep[0]-5}..+{ep[1]-5}), tow {lo:.0f}..{hi:.0f}\n")
        print("| row | " + " | ".join(hdr) + " |")
        print("|---" * (len(hdr) + 1) + "|")

        def line(label, fn, d=4):
            vals = []
            for c in hdr:
                v = fn(cells[c])
                vals.append("-" if v is None else (v if isinstance(v, str) else f"{v:.{d}f}"))
            print(f"| {label} | " + " | ".join(vals) + " |")

        line("1 slant iono rms (m)", lambda c: _dig(c, "iono", "rms_m"))
        line("1 slant iono p50 \\|e\\| (m)", lambda c: _dig(c, "iono", "p50_m"))
        line("1 slant iono p90 \\|e\\| (m)", lambda c: _dig(c, "iono", "p90_m"))
        line("2 ZWD rms (m)", lambda c: _dig(c, "zwd", "rms_m"))
        line("2 ZWD p90 \\|e\\| (m)", lambda c: _dig(c, "zwd", "p90_m"))
        line("3 rx DCB contrast rms (m)",
             lambda c: _dig(c, "sg", "rx", "contrast", "rms"))
        line("3 rx clock contrast rms (m)",
             lambda c: _dig(c, "sg", "rx", "clk_contrast", "rms"))
        line("4 sat clock rms (m)", lambda c: _dig(c, "sg", "clk", "residual", "rms"))
        line("4 sat clock p50 \\|e\\| (m)",
             lambda c: _dig(c, "sg", "clk", "residual", "p50_abs"))
        line("4 sat clock SD contrast rms (m)",
             lambda c: _dig(c, "sg", "clk", "contrast", "rms"))
        line("5 code bias rms (m)", lambda c: _dig(c, "sg", "cb", "residual", "rms"))
        # simnet_amb_integer.stats() tuple:
        #   (n, med e, med |e|, p90 |e|, %<=0.10, %<=0.25, % round(e)==0)
        for sig in ("L1C", "L2W", "L5Q"):
            line(f"5 DD {sig} med \\|e\\| (cyc, ref-free)",
                 lambda c, g=sig: _amb(c, "rf_all", g, 2))
            line(f"5 DD {sig} at own integer (%)",
                 lambda c, g=sig: _amb(c, "rf_all", g, 6), d=1)
        for lane in ("WL(1,2)", "EWL(2,5)"):
            line(f"5 {lane} med \\|e\\| (cyc, ref-free)",
                 lambda c, g=lane: _amb(c, "rf_lane_all", g, 2))
            line(f"5 {lane} at own integer (%)",
                 lambda c, g=lane: _amb(c, "rf_lane_all", g, 6), d=1)
        line("6 consumer 3D p50 (m)", lambda c: _dig(c, "consumer", "p50_3d_m"))
        line("6 consumer Up p50 (m)", lambda c: _dig(c, "consumer", "p50_up_m"))
        line("n LOS / sta-ep / epochs",
             lambda c: f"{_dig(c,'iono','n') or 0}/{_dig(c,'zwd','n') or 0}/"
                       f"{_dig(c,'sg','n_epochs') or 0}")


# ------------------------------------------------------------------------- G4
def cmd_g4(args):
    name, ref_leg = "opin_slip_all", "opin_slip_null"
    m = meta(name)
    t_pin = m["event_tow"]
    t_end = m["first_tow"] + 60 * EPOCH_S
    fx = fixture_of(name)
    pins = pinned_set(name)   # end-of-leg membership; per-slip membership below
    print("== G4  slipped seed — the pin must FOLLOW N(t) through a truth slip ==")
    print(f"   pin tow {t_pin:.0f}, leg ends {t_end:.0f}, {len(pins)} arc(s) pinned at onset\n")

    # truth slips landing on a PINNED arc after the pin
    hits = []
    for sta, sat, sig in sorted(pins):
        p = os.path.join(fx, sta, "truth_slips.csv")
        if not os.path.isfile(p):
            continue
        with open(p, newline="") as fh:
            for r in csv.DictReader(fh):
                if r["satellite"] != sat or r["signal"] != sig:
                    continue
                t = float(r["tow_s"])
                if t_pin < t <= t_end:
                    hits.append((t, sta, sat, sig, int(round(float(r["delta_cycles"]))),
                                 r.get("flagged")))
    hits.sort()
    print(f"   truth slips on pinned arcs after the pin: {len(hits)}")
    for h in hits[:40]:
        print(f"     tow {h[0]:.0f}  {h[1]} {h[2]}/{h[3]}  d={h[4]:+d} cyc  flagged={h[5]}")
    if len(hits) > 40:
        print(f"     ... and {len(hits)-40} more")

    # DD error on chords touching a slipped pinned arc, at slip+1 and slip+5
    stas = sorted({h[1] for h in hits})
    n_int, slips = load_truth_amb(fx, sorted(set(stas) | {s for s, _a, _b in
                                                          [(x[0], 0, 0) for x in []]}))
    ok = len(hits) >= 1
    print(f"\n   DD error on chords carrying a slipped pinned arc "
          f"(|dd_cyc - DD(N_true)|, cycles)")
    print(f"   {'offset':>8s} {'pop':>6s} {'p50':>9s} {'p90':>9s} {'max':>9s} {'<0.01':>7s}")
    pins_at = {}
    for off in (1, 5):
        pool = []
        for t_slip, sta, sat, sig, _d, _fl in hits:
            t = t_slip + off * EPOCH_S
            if t > t_end:
                continue
            rows = [r for r in dd_rows(name, t - 0.25, t + 0.25)
                    if r["signal"] == sig
                    and (r["sat_i"] == sat or r["ref_sat"] == sat)
                    and (r["sta_a"] == sta or r["sta_b"] == sta)]
            need = {s for r in rows for s in (r["sta_a"], r["sta_b"])}
            miss = [s for s in need if (s, sat, sig) not in n_int]
            if miss:
                ni2, sl2 = load_truth_amb(fx, miss)
                n_int.update(ni2); slips.update(sl2)
            if t not in pins_at:
                pins_at[t] = pinned_set(name, t)
            pt = pins_at[t]
            for r in rows:
                sa, sb, ref = r["sta_a"], r["sta_b"], r["ref_sat"]
                four = [(sa, r["sat_i"], sig), (sa, ref, sig),
                        (sb, r["sat_i"], sig), (sb, ref, sig)]
                if not all(k in pt for k in four):
                    continue
                legs = [n_true(n_int, slips, k[0], k[1], sig, t) for k in four]
                if any(v is None for v in legs):
                    continue
                dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])
                pool.append(abs(float(r["dd_cyc"]) - dd_true))
        frac = (sum(1 for x in pool if x < 0.01) / len(pool)) if pool else None
        if pool and frac < 0.95:
            ok = False
        print(f"   slip+{off:<3d} {len(pool):6d} {f(pct(pool,50)):>9s} "
              f"{f(pct(pool,90)):>9s} {f(max(pool) if pool else None):>9s} "
              f"{('-' if frac is None else f'{100*frac:.1f}%'):>7s}")

    print("\n   slipped-bed none vs all (window W2):")
    print(f"   {'channel':22s} {'none':>12s} {'all':>12s}")
    lo, hi = window_tows(name, "W2")
    for label, fn, key in (("ZWD rms (m)", zwd_row, "rms_m"),
                           ("ZWD p90 |e| (m)", zwd_row, "p90_m"),
                           ("slant iono rms (m)", iono_row, "rms_m"),
                           ("consumer 3D p50 (m)", consumer_row, "p50_3d_m"),
                           ("consumer Up p50 (m)", consumer_row, "p50_up_m")):
        a = fn(ref_leg, *window_tows(ref_leg, "W2"))
        b = fn(name, lo, hi)
        print(f"   {label:22s} {f(_dig(a, key)):>12s} {f(_dig(b, key)):>12s}")
    print(f"\n   run-end clause: {oracle_clause(name)}")
    print(f"\nG4: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("g1").set_defaults(func=cmd_g1)
    p2 = sub.add_parser("g2")
    p2.add_argument("--leg", action="append")
    # The pre-registered read is pin+1 (epoch +6).  --at-ep re-reads the SAME gate
    # at a settled epoch, which is how the `all` arm's onset transient is separated
    # from a genuine actuation failure: the gate is the same, the epoch is named.
    p2.add_argument("--at-ep", type=int, default=None)
    p2.set_defaults(func=cmd_g2)
    sub.add_parser("g3").set_defaults(func=cmd_g3)
    sub.add_parser("g4").set_defaults(func=cmd_g4)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
