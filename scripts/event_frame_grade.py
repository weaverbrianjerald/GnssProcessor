#!/usr/bin/env python3
"""Event harness: the station-frame channels (Study 2's instrument).

``scripts/event_grade.py`` grades the injected arc and the wire. This grades the
station the arc belongs to: does the event stay in the arc it was injected into,
or does the station's whole local frame move?

    python scripts/event_frame_grade.py runs/evleg_<name> --null runs/evleg_<null> \
        --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h [--json out.json]

Channels (all at epochs relative to the event tow; ``_d`` columns are event minus
null, which cancels the warm-resume seam transient the two legs share):

  lvl<band>  the injected arc's DD level error per band (L1C/L2W/L5Q), chord-median,
             from ``logs/network_dd_fractional.csv`` joined on the fixture's true
             integers. Inherits the DD trace's single reference satellite
             (SPEC_amb_iono §6.12); read the diff.
  oth        the same station's other arcs: mean |level error| over every (sat, band)
             of that station except the injected satellite. This is the frame
             reaction, the number Study 2 exists for. An event that stays in its
             arc leaves this flat; an event the station absorbs by moving its own
             local frame (clock / ZWD / iono) shows here.
  clk        the station's receiver clock error, ``network_rx_states.csv`` (kind=CLK,
             absolute metres) minus ``truth_clock.csv`` ``clock_bias_m``, reported as
             the contrast: minus the per-epoch mean over every station in the trace.
             The network-common part of a receiver-clock datum is absorbed by the
             satellite clocks and is not a station property (standing invariant #5),
             so only the contrast is gradeable.
  zwd        station ZWD error vs ``truth_tropo.csv``, metres.
  ion        the station's per-LOS slant iono error for the injected satellite:
             ``network_ionosphere.csv`` ``iono_total_m`` (the absolute column, never
             ``iono_m``, which is the correction alone) minus ``truth_atmo.csv``
             ``I_L1_m``. Carries the station's receiver-DCB offset; read the diff.
  C / P      the emitted satellite clock and L1 phase bias for the injected satellite.
  Call       the emitted satellite clock event minus null over every satellite in the
             product: its per-epoch mean is the network-common share a single-station
             event pushed into the satellite clocks (expect about -step/N for a
             station step), its rms the total wire signature.
  cons       consumer 3D ENU error from ``closed_loop_trace.csv``.
  qc         detector rows on that (station, sat) from ``network_events.csv``.

``rx_clock_step`` legs (``--step-m``, or ``step_m`` read from the leg's own event
json) additionally get the channel the kind exists for. The injected step is
subtracted from the event minus null receiver-clock difference, so:

  absorb     ``clk_raw_d / step_m``, the fraction of the injected step the
             estimated receiver clock actually took. Read the raw difference, not
             the contrast: a single station's step is 1/N network-common, so a
             60-station contrast can structurally show at most ``(1 - 1/N)`` of it.
  res        ``clk_raw_d - step_m``, what did not land in the clock, in metres.
             Everything the clock did not absorb had to go somewhere else: the
             station's other arcs (``oth_d``), its ZWD, its slant iono, or the wire.

Nothing here is a verdict. It is an instrument.
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
from simnet_amb_integer import load_truth, n_at            # noqa: E402
from event_grade import (median, leg_meta, rel_ep, fmt, truth_series,   # noqa: E402
                         zwd_error, consumer_series, wire_series, qc_events,
                         diff, EPOCH_S)


# ------------------------------------------------------------------ levels
def station_level_errors(legdir, fixture, sta, sat, t0, lo, hi, inj_delta, inj_sig):
    """{(sat, sig): {rel_ep: chord-median DD level error, cycles}} for `sta`.

    One pass over the DD trace, keeping every chord that touches `sta`.  The
    injected delta is folded into the truth of (sta, sat, inj_sig) from t0 on, so a
    perfectly responding producer reads ~0.
    """
    path = os.path.join(legdir, "logs", "network_dd_fractional.csv")
    if not os.path.isfile(path):
        return {}, "no network_dd_fractional.csv"
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
        if inj_delta and s == sta and v == sat and g == inj_sig and tow >= t0 - 0.25:
            n += inj_delta
        return n

    acc = defaultdict(lambda: defaultdict(list))
    with open(path, newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        ci = {n: i for i, n in enumerate(hdr)}
        for need in ("tow", "signal", "sat_i", "ref_sat", "sta_a", "sta_b", "dd_cyc"):
            if need not in ci:
                return {}, f"{path}: missing column {need}"
        for r in rd:
            sa, sb = r[ci["sta_a"]], r[ci["sta_b"]]
            if sta not in (sa, sb):
                continue
            sat_i, ref = r[ci["sat_i"]], r[ci["ref_sat"]]
            if ref == sat_i:
                continue
            tow = float(r[ci["tow"]])
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            sig = r[ci["signal"]]
            legs = [n_true(sa, sat_i, sig, tow), n_true(sa, ref, sig, tow),
                    n_true(sb, sat_i, sig, tow), n_true(sb, ref, sig, tow)]
            if any(v is None for v in legs):
                continue
            dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])
            e = float(r[ci["dd_cyc"]]) - dd_true
            acc[(sat_i, sig)][k].append(e if sa == sta else -e)
    return {key: {k: median(v) for k, v in per.items()} for key, per in acc.items()}, None


def other_arcs_mean_abs(lv, sat):
    """mean |level error| over every (sat', band) of the station except `sat`."""
    out = defaultdict(list)
    for (s, _sig), per in lv.items():
        if s == sat:
            continue
        for k, v in per.items():
            if v is not None:
                out[k].append(abs(v))
    return {k: (sum(v) / len(v)) for k, v in out.items() if v}


# ------------------------------------------------------------------- clock
def rx_clock_contrast(legdir, fixture, sta, t0, lo, hi):
    """Station receiver-clock error minus the per-epoch network mean, metres.

    network_rx_states.csv holds the absolute per-station clock (kind=CLK). The
    truth is per-station truth_clock.csv `clock_bias_m`. The network-common part
    of the pair is the receiver-clock datum, absorbed by the satellite clocks; the
    contrast is the only station-attributable half.
    """
    p = os.path.join(legdir, "logs", "network_rx_states.csv")
    if not os.path.isfile(p):
        return {}, {}, "no network_rx_states.csv (arm network.trace_rx_states)"
    est = defaultdict(dict)                    # tow_int -> {sta: value_m}
    with open(p, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("kind") != "CLK":
                continue
            try:
                tow = float(row["tow"])
                v = float(row["value_m"])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            est[k][row["station"]] = v
    truth_cache = {}

    def tclk(s, tow_i):
        if s not in truth_cache:
            truth_cache[s] = truth_series(fixture, s, "truth_clock.csv",
                                          "tow_s", "clock_bias_m") or {}
        return truth_cache[s].get(tow_i)

    raw, con = {}, {}
    for k, per in est.items():
        tow_i = int(round(t0 + k * EPOCH_S))
        errs = {}
        for s, v in per.items():
            t = tclk(s, tow_i)
            if t is not None:
                errs[s] = v - t
        if sta not in errs or not errs:
            continue
        mu = sum(errs.values()) / len(errs)
        raw[k] = errs[sta]
        con[k] = errs[sta] - mu
    return raw, con, None


# -------------------------------------------------------------------- iono
def los_iono_error(legdir, fixture, sta, sat, t0, lo, hi):
    """iono_total_m (absolute) minus truth_atmo I_L1_m for (sta, sat), metres."""
    p = os.path.join(legdir, "logs", "network_ionosphere.csv")
    if not os.path.isfile(p):
        return {}, "no network_ionosphere.csv (arm network.trace_ionosphere)"
    tp = os.path.join(fixture, sta, "truth_atmo.csv")
    if not os.path.isfile(tp):
        return {}, "no truth_atmo.csv"
    truth = {}
    with open(tp, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("satellite") != sat:
                continue
            try:
                truth[int(round(float(row["tow_s"])))] = float(row["I_L1_m"])
            except (KeyError, ValueError):
                continue
    out = {}
    with open(p, newline="") as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        vcol = "iono_total_m" if "iono_total_m" in cols else None
        if vcol is None:
            return {}, ("network_ionosphere.csv has no iono_total_m column — this "
                        "binary predates the §7.8 frame fix; iono_m alone is a "
                        "CORRECTION, not the slant iono")
        for row in rd:
            if row.get("station") != sta or row.get("sat") != sat:
                continue
            try:
                tow = float(row["tow"])
                v = float(row[vcol])
            except (KeyError, ValueError):
                continue
            k = rel_ep(tow, t0)
            if k < lo or k > hi:
                continue
            t = truth.get(int(round(tow)))
            if t is not None:
                out[k] = v - t
    return out, None


# ------------------------------------------------------- wire, all satellites
def wire_all_clocks(legdir, t0, lo, hi):
    """{rel_ep: {sat: emitted clock, m}} over every satellite in products.txt.

    The per-satellite `wire_series` answers what happened to that one satellite. A
    single-station event has no satellite of its own: what it does to the wire is
    a network-common shift shared by every satellite (the station's share of the
    clock datum, about 1/N), which only shows up when the whole set is read at once.
    """
    p = os.path.join(legdir, "products.txt")
    if not os.path.isfile(p):
        return {}, "no products.txt"
    out, k = defaultdict(dict), None
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
            if fs[0] == "C" and len(fs) >= 3:
                try:
                    out[k][fs[1]] = float(fs[2])
                except ValueError:
                    pass
    return dict(out), None


def wire_all_stats(e, n):
    """Per-epoch (mean, rms) of the emitted satellite clock, event - null."""
    mean, rms = {}, {}
    for k, per in e.items():
        d = [per[s] - n[k][s] for s in per if k in n and s in n[k]]
        if not d:
            continue
        mean[k] = sum(d) / len(d)
        rms[k] = math.sqrt(sum(x * x for x in d) / len(d))
    return mean, rms


# ----------------------------------------------------------------- reduce
def _rms(series, k0, k1):
    v = [x for k, x in series.items() if k0 <= k <= k1 and x is not None]
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else None


def _peak(series, k0=0):
    v = [(abs(x), k) for k, x in series.items() if k >= k0 and x is not None]
    if not v:
        return None, None
    _, k = max(v)
    return series[k], k


BANDS = ("L1C", "L2W", "L5Q")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("legdir")
    ap.add_argument("--null", required=True)
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--pre", type=int, default=5)
    ap.add_argument("--post", type=int, default=45)
    ap.add_argument("--json", default=None)
    ap.add_argument("--label", default=None, help="row label for the summary line")
    ap.add_argument("--station", default=None,
                    help="grade this station instead of the event's (required for "
                         "kinds that carry no station, e.g. anchor_handover, and for "
                         "a null-vs-null quiet contrast)")
    ap.add_argument("--sat", default=None, help="grade this satellite instead")
    ap.add_argument("--sig", default=None, help="band for the per-arc columns")
    ap.add_argument("--step-m", type=float, default=None,
                    help="injected step in metres, for a kind that steps a whole "
                         "station's range (rx_clock_step).  Default: the leg's own "
                         "event json.  Given, it is SUBTRACTED from the event - null "
                         "receiver-clock difference and the absorption is reported.")
    args = ap.parse_args()

    lo, hi = -abs(args.pre), abs(args.post)
    m = leg_meta(args.legdir)
    ev = m.get("event") or {}
    t0 = m.get("event_tow")
    if t0 is None:
        # A null leg graded against another null (the on-vs-off quiet contrast):
        # take t0 from whichever leg has an event, else the +10 convention on the
        # null's own first processed epoch, so the epoch axis matches the event legs.
        mn = leg_meta(args.null)
        t0 = mn.get("event_tow")
        if t0 is None:
            t0 = mn["first_tow"] + 10 * EPOCH_S
    kind = ev.get("kind", "null")
    sta = args.station or ev.get("station") or ""
    sat = args.sat or ev.get("sat") or ""
    sig = args.sig or ev.get("signal") or "L1C"
    if sig == "all":
        sig = "L1C"
    inj = ev.get("delta_cycles", 0.0) if kind == "slip" else 0.0
    # A station-wide range step (rx_clock_step) carries no satellite by construction,
    # since that is the event, so the arc/iono/wire columns need one named on the
    # command line, the same way anchor_handover needs --station.
    step_m = args.step_m
    if step_m is None:
        step_m = ev.get("step_m", 0.0) if kind == "rx_clock_step" else 0.0
    if not sta or not sat:
        raise SystemExit("this leg has no (station, sat) target — pass --station / --sat "
                         "(rx_clock_step is station-wide and names no satellite; "
                         "anchor_handover names no station), use --null on an event leg, "
                         "or add a _target block to leg_meta.json")

    notes = []
    lv_e, err = station_level_errors(args.legdir, args.fixture, sta, sat, t0, lo, hi,
                                     inj, sig)
    if err:
        notes.append("lvl: " + err)
    lv_n, _ = station_level_errors(args.null, args.fixture, sta, sat, t0, lo, hi, 0.0, sig)

    oth_e, oth_n = other_arcs_mean_abs(lv_e, sat), other_arcs_mean_abs(lv_n, sat)
    clk_raw_e, clk_e, err = rx_clock_contrast(args.legdir, args.fixture, sta, t0, lo, hi)
    if err:
        notes.append("clk: " + err)
    clk_raw_n, clk_n, _ = rx_clock_contrast(args.null, args.fixture, sta, t0, lo, hi)
    zwd_e, err = zwd_error(args.legdir, args.fixture, sta, t0, lo, hi)
    if err:
        notes.append("zwd: " + err)
    zwd_n, _ = zwd_error(args.null, args.fixture, sta, t0, lo, hi)
    ion_e, err = los_iono_error(args.legdir, args.fixture, sta, sat, t0, lo, hi)
    if err:
        notes.append("ion: " + err)
    ion_n, _ = los_iono_error(args.null, args.fixture, sta, sat, t0, lo, hi)
    wC_e, wP_e, err = wire_series(args.legdir, sat, sig, t0, lo, hi)
    if err:
        notes.append("wire: " + err)
    wC_n, wP_n, _ = wire_series(args.null, sat, sig, t0, lo, hi)
    con_e, gate_e, err = consumer_series(args.legdir, t0, lo, hi)
    if err:
        notes.append("cons: " + err)
    con_n, _, _ = consumer_series(args.null, t0, lo, hi)
    wall_e, err = wire_all_clocks(args.legdir, t0, lo, hi)
    if err:
        notes.append("Call: " + err)
    wall_n, _ = wire_all_clocks(args.null, t0, lo, hi)
    qc_e, _ = qc_events(args.legdir, sta, sat, t0, lo, hi)

    band_e = {b: lv_e.get((sat, b), {}) for b in BANDS}
    band_n = {b: lv_n.get((sat, b), {}) for b in BANDS}
    d_band = {b: diff(band_e[b], band_n[b]) for b in BANDS}
    d_oth, d_clk, d_zwd = diff(oth_e, oth_n), diff(clk_e, clk_n), diff(zwd_e, zwd_n)
    d_ion, d_C, d_P, d_con = (diff(ion_e, ion_n), diff(wC_e, wC_n),
                              diff(wP_e, wP_n), diff(con_e, con_n))
    d_clk_raw = diff(clk_raw_e, clk_raw_n)
    d_Call_mean, d_Call_rms = wire_all_stats(wall_e, wall_n)
    # The step channel: how much of the injected metres the estimated receiver clock
    # actually took, and what is left over to land somewhere else.
    absorb = ({k: v / step_m for k, v in d_clk_raw.items() if v is not None}
              if step_m else {})
    resid = ({k: v - step_m for k, v in d_clk_raw.items() if v is not None}
             if step_m else {})

    print(f"FRAME grade — {kind} on {sta}/{sat}/{sig}   t0 tow {t0:.0f}"
          + (f"   step {step_m:+.3f} m" if step_m else ""))
    print(f"  leg   {args.legdir}")
    print(f"  null  {args.null}")
    print()
    step_hdr = f" {'clkR_d':>8} {'absorb':>7} {'res_d':>8}" if step_m else ""
    hdr = (f"{'ep':>4} {'L1_d':>8} {'L2_d':>8} {'L5_d':>8} {'oth':>7} {'oth_d':>8} "
           f"{'clk_c':>8} {'clk_d':>8}" + step_hdr + f" {'zwd_d':>8} {'ion_d':>8} "
           f"{'C_d':>8} {'Call_m':>8} {'P_d':>8} {'cons':>7} {'cons_d':>8}  qc")
    print(hdr)
    print("-" * len(hdr))
    for k in range(lo, hi + 1):
        q = ",".join(qc_e.get(k, []))[:40]
        stepc = (f" {fmt(d_clk_raw.get(k),4,8)} {fmt(absorb.get(k),3,7)} "
                 f"{fmt(resid.get(k),4,8)}") if step_m else ""
        print(f"{k:>+4d} {fmt(d_band['L1C'].get(k),3,8)} {fmt(d_band['L2W'].get(k),3,8)} "
              f"{fmt(d_band['L5Q'].get(k),3,8)} {fmt(oth_e.get(k),3,7)} "
              f"{fmt(d_oth.get(k),3,8)} {fmt(clk_e.get(k),4,8)} {fmt(d_clk.get(k),4,8)}"
              + stepc +
              f" {fmt(d_zwd.get(k),4,8)} {fmt(d_ion.get(k),4,8)} "
              f"{fmt(d_C.get(k),4,8)} {fmt(d_Call_mean.get(k),4,8)} {fmt(d_P.get(k),4,8)} "
              f"{fmt(con_e.get(k),3,7)} {fmt(d_con.get(k),3,8)}  {q}")

    # ------------------------------------------------------------- summary
    pk_l1, pk_k = _peak(d_band["L1C"])
    row = {
        "label": args.label or os.path.basename(args.legdir),
        "kind": kind, "t0": t0, "sta": sta, "sat": sat, "sig": sig,
        "peak_lvl_d": pk_l1, "peak_lvl_ep": pk_k,
        "lvl_d_14": d_band["L1C"].get(14), "lvl_d_30": d_band["L1C"].get(30),
        "lvl_d_45": d_band["L1C"].get(45),
        "lvl2_d_45": d_band["L2W"].get(45), "lvl5_d_45": d_band["L5Q"].get(45),
        "oth_45": oth_e.get(45), "oth_d_45": d_oth.get(45),
        "oth_d_rms_14_45": _rms(d_oth, 14, 45),
        "clk_rms_14_45": _rms(clk_e, 14, 45),
        "clk_d_rms_14_45": _rms(d_clk, 14, 45),
        "clk_d_peak": _peak(d_clk)[0],
        "zwd_d_peak": _peak(d_zwd)[0], "zwd_d_peak_ep": _peak(d_zwd)[1],
        "ion_d_peak": _peak(d_ion)[0],
        "C_d_peak": _peak(d_C)[0], "P_d_peak": _peak(d_P)[0],
        "cons_45": con_e.get(45), "cons_d_45": d_con.get(45),
        "cons_d_peak": _peak(d_con)[0],
        "clk_raw_d_rms_14_45": _rms(d_clk_raw, 14, 45),
        "Call_d_rms_14_45": _rms(d_Call_rms, 14, 45),
        "Call_d_mean_45": d_Call_mean.get(45),
        "notes": notes,
    }
    if step_m:
        # The kind's OWN channel.  +1 / +2 is the question ("did the clock take it,
        # and did it take it at once?"); +0 is the epoch the step lands on, and +45
        # says whether the absorption held.
        row["step_m"] = step_m
        for k in (0, 1, 2, 5, 45):
            row[f"absorb_{k}"] = absorb.get(k)
            row[f"res_{k}"] = resid.get(k)
        row["res_rms_14_45"] = _rms(resid, 14, 45)
    print()
    print("SUMMARY " + json.dumps({k: (round(v, 6) if isinstance(v, float) else v)
                                   for k, v in row.items() if k != "notes"}))
    for n in notes:
        print("  note: " + n)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"row": row,
                       "band_d": {b: d_band[b] for b in BANDS},
                       "band": {b: band_e[b] for b in BANDS},
                       "oth": oth_e, "oth_d": d_oth,
                       "clk_contrast": clk_e, "clk_raw": clk_raw_e, "clk_d": d_clk,
                       "clk_raw_d": d_clk_raw, "absorb": absorb, "res_d": resid,
                       "Call_d_mean": d_Call_mean, "Call_d_rms": d_Call_rms,
                       "zwd": zwd_e, "zwd_d": d_zwd, "ion": ion_e, "ion_d": d_ion,
                       "C_d": d_C, "P_d": d_P, "cons": con_e, "cons_d": d_con,
                       "qc": {str(k): v for k, v in qc_e.items()}},
                      f, indent=2, default=str)
        print(f"  json: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
