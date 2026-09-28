#!/usr/bin/env python3
"""Event-harness grader: one injected event, read against its null leg.

    python scripts/event_grade.py runs/evleg_<name> --null runs/evleg_<null> \
        --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h [--json out.json]

Every quantity is reported at epochs relative to t0 (the event's tow), for the
window ``--pre`` .. ``--post`` (default -10 .. +50).  Because the event leg and
the null leg resume from the same snapshot with the same config except for
``network.inject``, the difference column (event - null) cancels the warm-resume
seam transient, the bed and the posture, leaving the event's own signature.

Channels, in the order the table prints them:

  lvl        the injected arc's own level error: the chord-median DD error of
             (station, sat, signal) from ``logs/network_dd_fractional.csv``,
             joined against the fixture's true integers.  For a `slip` the
             injected delta is added to the truth from t0 on, so `lvl` reads the
             producer's residual error, not the injection itself.  The DD
             trace carries one reference satellite and its error is in every
             chord (SPEC_amb_iono §6.12); a per-arc median inherits that. Use
             the diff column, which cancels it to the extent the two legs share
             the reference (they do, except where the event moves it).
  zwd/iono   the station's ZWD error vs truth_tropo.csv and its per-LOS slant
             iono error for the injected satellite vs truth_atmo.csv.
  clk/pb     the emitted satellite clock and L1 phase bias for `sat`, from
             products.txt, minus the null leg's: the event's wire signature.
  cons       the closed-loop consumer: 3D ENU error and ar_gate from
             closed_loop_trace.csv.
  qc         detector/QC events on that (station, sat) from network_events.csv,
             printed as tier@offset.

Nothing here is a verdict. It is an instrument; the verdict is the operator's.
"""
import argparse
import bisect
import csv
import json
import math
import os
import re
import sys
from collections import defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from simnet_amb_integer import load_truth, n_at   # noqa: E402

EPOCH_S = 30.0


# ------------------------------------------------------------------ helpers
def median(v):
    if not v:
        return None
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def leg_meta(legdir):
    p = os.path.join(legdir, "leg_meta.json")
    if not os.path.isfile(p):
        raise SystemExit(f"missing {p} — was this leg made by scripts/event_leg.py?")
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def rel_ep(tow, t0):
    return int(round((tow - t0) / EPOCH_S))


def fmt(v, d=3, w=8):
    if v is None:
        return " " * (w - 1) + "-"
    if isinstance(v, str):
        return v.rjust(w)
    return f"{v:{w}.{d}f}"


# ------------------------------------------------------------------ channels
def arc_level_error(legdir, fixture, sta, sat, sig, t0, lo, hi, inj_delta):
    """Chord-median DD error (cycles) of (sta, sat, sig), per relative epoch.

    The injected delta is folded into the truth from t0 on, so a perfectly
    responding producer reads ~0 and an unresponsive one reads inj_delta.
    """
    path = os.path.join(legdir, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(path):
        return {}, "no network_dd_fractional.csv (arm network.trace_dd_fractional)"
    n_int, slips = load_truth(fixture, set())
    loaded = set()

    def ensure(s):
        if s in loaded:
            return
        loaded.add(s)
        ni, sl = load_truth(fixture, [s])
        n_int.update(ni)
        for k, v in sl.items():
            slips[k] = v

    def n_true(s, v, g, tow):
        ensure(s)
        n = n_at(n_int, slips, s, v, g, tow)
        if n is None:
            return None
        # the injected slip is part of this arc's truth from t0 on
        if inj_delta and s == sta and v == sat and g == sig and tow >= t0 - 0.25:
            n += inj_delta
        return n

    out = defaultdict(list)
    with open(path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        ci = {n: i for i, n in enumerate(hdr)}
        for need in ("tow", "signal", "sat_i", "ref_sat", "sta_a", "sta_b", "dd_cyc"):
            if need not in ci:
                return {}, f"{path}: missing column {need}"
        for r in rd:
            if r[ci["signal"]] != sig:
                continue
            sat_i, ref = r[ci["sat_i"]], r[ci["ref_sat"]]
            if sat_i != sat or ref == sat_i:
                continue
            sa, sb = r[ci["sta_a"]], r[ci["sta_b"]]
            if sta not in (sa, sb):
                continue
            tow = float(r[ci["tow"]])
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            legs = [n_true(sa, sat_i, sig, tow), n_true(sa, ref, sig, tow),
                    n_true(sb, sat_i, sig, tow), n_true(sb, ref, sig, tow)]
            if any(v is None for v in legs):
                continue
            dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])
            e = float(r[ci["dd_cyc"]]) - dd_true
            out[k].append(e if sa == sta else -e)
    return {k: median(v) for k, v in out.items()}, None


def truth_series(fixture, sta, fname, tcol, vcol, filt=None):
    p = os.path.join(fixture, sta, fname)
    if not os.path.isfile(p):
        return None
    out = {}
    with open(p, newline="") as f:
        for row in csv.DictReader(f):
            if filt and not filt(row):
                continue
            try:
                out[int(round(float(row[tcol])))] = float(row[vcol])
            except (KeyError, ValueError):
                continue
    return out


def zwd_error(legdir, fixture, sta, t0, lo, hi):
    """Estimated ZWD (network_zwd.csv) minus truth_tropo.csv, metres."""
    p = os.path.join(legdir, "logs", "network_zwd.csv")
    if not os.path.isfile(p):
        return {}, "no network_zwd.csv (arm network.trace_zwd)"
    truth = truth_series(fixture, sta, "truth_tropo.csv", "tow_s", "zwd_m")
    if truth is None:
        return {}, "no truth_tropo.csv"
    out = {}
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        scol = next((c for c in cols if c in ("station", "sta")), None)
        vcol = next((c for c in cols if "zwd" in c.lower() and "sig" not in c.lower()),
                    None)
        if not scol or not vcol:
            return {}, f"network_zwd.csv: no station/zwd columns in {cols}"
        for row in rd:
            if row[scol] != sta:
                continue
            tow = float(row["tow"])
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            t = truth.get(int(round(tow)))
            if t is None:
                continue
            try:
                out[k] = float(row[vcol]) - t
            except ValueError:
                continue
    return out, None


def consumer_series(legdir, t0, lo, hi):
    """3D ENU error (m) and ar_gate from closed_loop_trace.csv."""
    p = os.path.join(legdir, "logs", "closed_loop_trace.csv")
    if not os.path.isfile(p):
        return {}, {}, "no closed_loop_trace.csv"
    enu, gate = {}, {}
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        ec = [c for c in ("east_m", "err_e_m", "e_err_m", "de_m") if c in cols]
        nc = [c for c in ("north_m", "err_n_m", "n_err_m", "dn_m") if c in cols]
        uc = [c for c in ("up_m", "err_u_m", "u_err_m", "du_m") if c in cols]
        d3 = [c for c in ("pos_err_3d_m", "err_3d_m", "e3d_m") if c in cols]
        gc = [c for c in ("ar_gate", "gate", "ar_state") if c in cols]
        for row in rd:
            try:
                tow = float(row["tow"])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            try:
                if d3:
                    enu[k] = float(row[d3[0]])
                elif ec and nc and uc:
                    enu[k] = math.sqrt(float(row[ec[0]]) ** 2 + float(row[nc[0]]) ** 2
                                       + float(row[uc[0]]) ** 2)
            except ValueError:
                pass
            if gc:
                gate[k] = row[gc[0]]
    return enu, gate, None


_PROD_C = re.compile(r"^C\s+(\S+)\s+([-\d.eE+]+)")
_PROD_P = re.compile(r"^P\s+(\S+)\s+(\S+)\s+([-\d.eE+]+)")


def wire_series(legdir, sat, sig, t0, lo, hi):
    """Emitted satellite clock (m) and phase bias (m) for `sat`, per relative epoch.

    products.txt is the epoch-blocked serialisation the consumer replays; the
    epoch header carries the tow, and C / P lines carry the clock and per-signal
    phase bias.  Parsed permissively: the format has grown fields and this
    grader only needs (tow, sat, value).
    """
    p = os.path.join(legdir, "products.txt")
    if not os.path.isfile(p):
        return {}, {}, "no products.txt"
    clk, pb = {}, {}
    k = None
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            fs = line.rstrip().split(",")
            if fs[0] == "E" and len(fs) >= 3:
                try:
                    k = rel_ep(float(fs[2]), t0)
                except ValueError:
                    k = None
                continue
            if k is None or k < lo or k > hi:
                continue
            if fs[0] == "C" and len(fs) >= 3 and fs[1] == sat:
                try:
                    clk[k] = float(fs[2])
                except ValueError:
                    pass
            elif fs[0] == "P" and len(fs) >= 4 and fs[1] == sat and fs[2] == sig:
                try:
                    pb[k] = float(fs[3])
                except ValueError:
                    pass
    return clk, pb, None


def qc_events(legdir, sta, sat, t0, lo, hi):
    """network_events.csv rows touching (sta, sat) -> {rel_ep: [tier/event, ...]}."""
    p = os.path.join(legdir, "logs", "network_events.csv")
    if not os.path.isfile(p):
        return {}, "no network_events.csv"
    out = defaultdict(list)
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        for row in rd:
            if row.get("sat") not in ("", sat) or row.get("station") not in ("", sta):
                continue
            if row.get("sat") == "" and row.get("station") == "":
                continue          # epoch-level row: not attributable to this arc
            try:
                k = rel_ep(float(row["tow"]), t0)
            except (KeyError, ValueError):
                continue
            if lo <= k <= hi:
                out[k].append(f"{row.get('tier','?')}/{row.get('event','?')}")
    return out, None



# ------------------------------------------------------------------ seam mode
def read_products(path, lo_tow=None, hi_tow=None):
    """products.txt -> {tow_int: {('C', sat): v, ('P', sat, sig): v}}."""
    out = {}
    tow = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            fs = line.rstrip().split(",")
            if fs[0] == "E" and len(fs) >= 3:
                try:
                    t = float(fs[2])
                except ValueError:
                    tow = None
                    continue
                tow = int(round(t))
                if (lo_tow is not None and tow < lo_tow) or \
                   (hi_tow is not None and tow > hi_tow):
                    tow = None
                elif tow not in out:
                    out[tow] = {}
                continue
            if tow is None:
                continue
            if fs[0] == "C" and len(fs) >= 3:
                try:
                    out[tow][("C", fs[1])] = float(fs[2])
                except ValueError:
                    pass
            elif fs[0] == "P" and len(fs) >= 4:
                try:
                    out[tow][("P", fs[1], fs[2])] = float(fs[3])
                except ValueError:
                    pass
    return out


def seam_report(legdir, refdir, cm=0.01):
    """Per-epoch max |Delta| between a null leg's wire and a continuous run's.

    This is the harness's own control: it measures what the warm-resume seam
    costs, in the same units the event legs are graded in.  Because both legs of
    an event A/B carry this transient identically, the event grader's diff
    columns cancel it: this report is what justifies that claim.
    """
    m = leg_meta(legdir)
    first = int(round(m["first_tow"]))
    last = first + int(m["epochs"]) * int(EPOCH_S)
    a = read_products(os.path.join(legdir, "products.txt"), first, last)
    b = read_products(os.path.join(refdir, "products.txt"), first, last)
    tows = sorted(set(a) & set(b))
    if not tows:
        raise SystemExit("no overlapping epochs between the leg and the reference run")
    print(f"SEAM TRANSIENT — {legdir}  vs continuous {refdir}")
    print(f"  {len(tows)} overlapping epochs from tow {tows[0]} "
          f"(leg epoch +0) to {tows[-1]}")
    print()
    print(f"{'ep':>4} {'n_C':>5} {'maxdC_m':>10} {'rmsdC_m':>10} "
          f"{'n_P':>5} {'maxdP_m':>10} {'rmsdP_m':>10}")
    print("-" * 64)
    first_ok = None
    rows = []
    for i, t in enumerate(tows):
        dc, dp = [], []
        for k, v in a[t].items():
            if k in b[t]:
                (dc if k[0] == "C" else dp).append(v - b[t][k])
        mc = max((abs(x) for x in dc), default=None)
        mp = max((abs(x) for x in dp), default=None)
        rc = (sum(x * x for x in dc) / len(dc)) ** 0.5 if dc else None
        rp = (sum(x * x for x in dp) / len(dp)) ** 0.5 if dp else None
        rows.append((i, len(dc), mc, rc, len(dp), mp, rp))
        print(f"{i:>4} {len(dc):>5} {fmt(mc,4,10)} {fmt(rc,4,10)} "
              f"{len(dp):>5} {fmt(mp,4,10)} {fmt(rp,4,10)}")
        if first_ok is None and mc is not None and mp is not None \
           and mc < cm and mp < cm:
            # require it to stay below from here on
            first_ok = i
        elif first_ok is not None and ((mc or 0) >= cm or (mp or 0) >= cm):
            first_ok = None
    print()
    if first_ok is None:
        print(f"  wire never settles below {cm*100:.0f} cm on BOTH channels in this window.")
    else:
        print(f"  both channels stay below {cm*100:.0f} cm from leg epoch +{first_ok}.")
    mc0 = rows[0][2]
    mp0 = rows[0][5]
    print(f"  seam epoch (+0): max|dC| {fmt(mc0,4,0).strip()} m, "
          f"max|dP| {fmt(mp0,4,0).strip()} m")
    return rows, first_ok


# ------------------------------------------------------------------ main
def diff(a, b):
    """a - b on the intersection; None where either is missing."""
    return {k: (a[k] - b[k]) for k in a if k in b and a[k] is not None
            and b[k] is not None}


def first_below(series, thresh, lo, hi):
    """First relative epoch >= 0 at which |value| stays below `thresh` to `hi`."""
    for k in range(0, hi + 1):
        if k not in series:
            continue
        if all(abs(series[j]) < thresh for j in range(k, hi + 1) if j in series):
            return k
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("legdir")
    ap.add_argument("--null", help="the no-event control leg (event grading)")
    ap.add_argument("--fixture", help="simulator fixture (truth files)")
    ap.add_argument("--seam-ref", default=None,
                    help="CONTINUOUS run dir: grade this leg's wire against it "
                         "(the harness's own seam-transient control) instead of "
                         "grading an event")
    ap.add_argument("--settle-m", type=float, default=0.01,
                    help="--seam-ref settle threshold in metres (default 0.01)")
    ap.add_argument("--pre", type=int, default=10)
    ap.add_argument("--post", type=int, default=50)
    ap.add_argument("--signal", default=None,
                    help="grade the lvl channel on THIS band instead of the "
                         "event's own (the per-band pass of a multi-band leg); "
                         "on a multi-event leg it also selects the event whose "
                         "delta_cycles is folded into the truth")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    if args.seam_ref:
        seam_report(args.legdir, args.seam_ref, args.settle_m)
        return 0
    if not args.null or not args.fixture:
        raise SystemExit("--null and --fixture are required for event grading")

    lo, hi = -abs(args.pre), abs(args.post)
    m = leg_meta(args.legdir)
    ev = m.get("event") or {}
    # A leg may carry several events at one tow (Study 3's all-band injected
    # slip writes one per band).  The lvl channel is per-signal, so grade one of
    # them: --signal names which, and the matching event supplies the injected
    # delta that gets folded into the truth.  Without --signal the first event
    # is graded, matching a single-event leg.
    ev_all = ev if isinstance(ev, list) else [ev]
    if isinstance(ev, list):
        ev = {}
        if args.signal:
            for e in ev_all:
                if (e or {}).get("signal") == args.signal:
                    ev = e
                    break
        if not ev:
            ev = ev_all[0] if ev_all else {}
    t0 = m.get("event_tow")
    if t0 is None:
        raise SystemExit("this leg has no event (it IS a null leg) — grade an event leg")
    kind = ev.get("kind", "?")
    sta, sat = ev.get("station", ""), ev.get("sat", "")
    sig = args.signal or ev.get("signal") or "L1C"
    if sig == "all":
        sig = "L1C"
    # An arc_erase band scope is a ";"-separated list (Study 1 rung 2); the lvl
    # channel is per-signal, so grade the first band of the scope and say so.
    if ";" in sig or "," in sig:
        scope = [t for t in sig.replace(",", ";").split(";") if t]
        sig = scope[0]
    inj = ev.get("delta_cycles", 0.0) if kind == "slip" else 0.0

    notes = []
    if len(ev_all) > 1:
        notes.append(f"leg carries {len(ev_all)} events at t0 "
                     f"({', '.join(str((e or {}).get('signal') or (e or {}).get('kind')) for e in ev_all)})"
                     f" — lvl graded on {sig}")
    if ";" in (ev.get("signal") or "") or "," in (ev.get("signal") or ""):
        notes = [f"band scope {ev['signal']} — lvl graded on {sig} "
                 f"(the other bands need a separate pass)"]
    res = {"leg": os.path.abspath(args.legdir), "null": os.path.abspath(args.null),
           "event": ev, "t0": t0, "window": [lo, hi]}

    def both(fn, *a, **kw):
        e = fn(args.legdir, *a, **kw)
        n = fn(args.null, *a, **kw)
        return e, n

    # (i) the injected arc's level error
    lvl_e, err = arc_level_error(args.legdir, args.fixture, sta, sat, sig, t0, lo, hi, inj)
    if err:
        notes.append("lvl: " + err)
    lvl_n, _ = arc_level_error(args.null, args.fixture, sta, sat, sig, t0, lo, hi, 0.0)

    # (ii) station ZWD error
    zwd_e, err = zwd_error(args.legdir, args.fixture, sta, t0, lo, hi) if sta else ({}, None)
    if err:
        notes.append("zwd: " + err)
    zwd_n, _ = zwd_error(args.null, args.fixture, sta, t0, lo, hi) if sta else ({}, None)

    # (iii) the wire
    clk_e, pb_e, err = wire_series(args.legdir, sat, sig, t0, lo, hi)
    if err:
        notes.append("wire: " + err)
    clk_n, pb_n, _ = wire_series(args.null, sat, sig, t0, lo, hi)

    # (iv) the consumer
    con_e, gate_e, err = consumer_series(args.legdir, t0, lo, hi)
    if err:
        notes.append("consumer: " + err)
    con_n, gate_n, _ = consumer_series(args.null, t0, lo, hi)

    # (v) QC response
    qc_e, err = qc_events(args.legdir, sta, sat, t0, lo, hi)
    if err:
        notes.append("qc: " + err)
    qc_n, _ = qc_events(args.null, sta, sat, t0, lo, hi)

    d_clk, d_pb, d_con = diff(clk_e, clk_n), diff(pb_e, pb_n), diff(con_e, con_n)
    d_lvl, d_zwd = diff(lvl_e, lvl_n), diff(zwd_e, zwd_n)

    # ---------------------------------------------------------------- table
    print(f"EVENT HARNESS grade — {kind}"
          + (f" {ev.get('delta_cycles')} cyc" if kind == "slip" else "")
          + (f" {ev.get('step_m')} m" if kind == "sat_clock_step" else "")
          + (f" {ev.get('duration_s')} s" if kind == "outage" else "")
          + (f" ({ev.get('mode')})" if kind == "anchor_handover" else ""))
    print(f"  leg   {args.legdir}")
    print(f"  null  {args.null}")
    print(f"  t0    tow {t0:.1f}   target  sta={sta or '-'} sat={sat or '-'} sig={sig}")
    print()
    hdr = (f"{'ep':>4} {'lvl_cyc':>9} {'lvl_d':>9} {'zwd_m':>9} {'zwd_d':>9} "
           f"{'clk_d_m':>9} {'pb_d_m':>9} {'cons_m':>8} {'cons_d':>9} {'gate':>5}  qc")
    print(hdr)
    print("-" * len(hdr))
    for k in range(lo, hi + 1):
        q = ",".join(qc_e.get(k, []))[:46]
        if q and qc_n.get(k):
            q += "  [null:" + ",".join(qc_n[k])[:20] + "]"
        row = (f"{k:>+4d} {fmt(lvl_e.get(k),3,9)} {fmt(d_lvl.get(k),3,9)} "
               f"{fmt(zwd_e.get(k),4,9)} {fmt(d_zwd.get(k),4,9)} "
               f"{fmt(d_clk.get(k),4,9)} {fmt(d_pb.get(k),4,9)} "
               f"{fmt(con_e.get(k),3,8)} {fmt(d_con.get(k),3,9)} "
               f"{str(gate_e.get(k,'-')):>5}  {q}")
        print(row)

    # ---------------------------------------------------------------- summary
    def peak(s, k0=0):
        vals = [(abs(v), k) for k, v in s.items() if k >= k0 and v is not None]
        if not vals:
            return None, None
        a, k = max(vals)
        return s[k], k

    lvl0 = lvl_e.get(0)
    lvl_pk, lvl_pk_k = peak(lvl_e)
    conv = first_below(lvl_e, 0.5, lo, hi) if lvl_e else None
    clk_pk, clk_pk_k = peak(d_clk)
    pb_pk, pb_pk_k = peak(d_pb)
    con_pk, con_pk_k = peak(d_con)
    zwd_pk, zwd_pk_k = peak(d_zwd)
    first_qc = min((k for k in qc_e if k >= 0 and qc_e[k]), default=None)

    print()
    parts = [f"{kind}"]
    if kind == "slip":
        parts.append(f"of {ev.get('delta_cycles')} cyc on {sta}/{sat}/{sig}")
    elif kind == "outage":
        parts.append(f"of {ev.get('duration_s')} s on {sta}/{sat}")
    elif kind == "sat_clock_step":
        parts.append(f"of {ev.get('step_m')} m on {sat}")
    elif kind == "anchor_handover":
        parts.append(f"({ev.get('mode')}) on {sat}")
    else:
        parts.append(f"on {sta}/{sat}")
    s = "SUMMARY: " + " ".join(parts) + f" at t0 (tow {t0:.0f}): "
    s += (f"QC responded at {first_qc:+d} ({','.join(qc_e[first_qc])}); "
          if first_qc is not None else "NO QC event on this arc in the window; ")
    s += (f"arc level error {lvl0:+.3f} cyc at +0, peak {lvl_pk:+.3f} at {lvl_pk_k:+d}"
          if lvl0 is not None and lvl_pk is not None else "arc level error unavailable")
    s += (f", |e| < 0.5 cyc from {conv:+d}" if conv is not None else ", never settles < 0.5 cyc")
    if zwd_pk is not None:
        s += f"; station ZWD error (event-null) peak {zwd_pk*1000:+.1f} mm at {zwd_pk_k:+d}"
    if clk_pk is not None:
        s += f"; wire C step peak {clk_pk:+.4f} m at {clk_pk_k:+d}"
    if pb_pk is not None:
        s += f", P({sig}) {pb_pk:+.4f} m at {pb_pk_k:+d}"
    if con_pk is not None:
        s += f"; consumer 3D (event-null) peak {con_pk:+.3f} m at {con_pk_k:+d}"
    else:
        s += "; consumer unavailable"
    s += "."
    print(s)
    for n in notes:
        print("  note: " + n)

    res.update({
        "lvl": lvl_e, "lvl_null": lvl_n, "lvl_diff": d_lvl,
        "zwd": zwd_e, "zwd_diff": d_zwd,
        "clk_diff": d_clk, "pb_diff": d_pb,
        "cons": con_e, "cons_diff": d_con, "gate": gate_e,
        "qc": {str(k): v for k, v in qc_e.items()},
        "qc_null": {str(k): v for k, v in qc_n.items()},
        "summary": s, "notes": notes,
    })
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=2, default=str)
        print(f"  json: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
