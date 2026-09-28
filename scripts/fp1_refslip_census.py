#!/usr/bin/env python3
"""Reference-slip fan-out census for the FP-1 field matrix.

    python scripts/fp1_refslip_census.py [--runs C:/GnssProcessor/runs]
                                         [--prefix fp1] [--json out.json]

What this measures, and why it is not one of §20's gates.

An IF-TD chain's per-sample statistic is ``dd = dOMC(sat) - dOMC(ref)``
(`IfPhaseSlipDetector.hpp:195-206`), so the chain's reference satellite has
``dd == 0`` identically, however large its own slip.  §22 closed one
consequence of that identity: a reference arc cannot exonerate, because the
pooled preview could never have flagged it (`common_band_ref_arc`).  This
census measures the other consequence, which no gate in §20 or §22 looks at:
when the reference satellite is the one that slips, its step lands at full
size on every other satellite of that chain, and the response refloats them.

Under ``if_phase_td_pairs: first`` a station runs one chain, so the exposure is
one reference arc.  Under ``all`` every distinct band pair runs its own chain
with its own reference election (§14.10.4), so a triple-band station carries
three references and the two L5-bearing chains typically elect the station's
highest L5-capable satellite, which is exactly the satellite an event study
picks as "mature, high elevation, clean".

Per leg the census reports:
  * the epochs k over which the injected satellite is each chain's reference
    (``network_if_slip.csv`` ``is_ref``): the exposure window;
  * the injected satellite's own abs_dev on each chain (expected 0.000000 on a
    chain it references: the slip is invisible where it happened);
  * every other satellite flagged at that station on that chain, with abs_dev,
    threshold and k: the fan-out;
  * how many of those became a ``T1_if_slip`` refloat versus were held back by
    a common-band verdict (``cb_kept=``) or never drained;
  * the phase-bias datum re-realizations they caused: products ``P`` rows whose
    value moves > `--pb-tol` m at the event epoch, with the phase_disc counter
    and emitted sigma on both sides (invariant #7's declaration edge), plus the
    ``R`` retro-bump and ``C`` clock_disc records;
  * the consumer's response, ``closed_loop_trace.csv`` ENU event minus null.

The off arm is printed alongside on every line (the expectation is 0), and the
G25 legs are the control: that satellite is not a chain reference at k=0, so a
fan-out there would falsify the mechanism rather than confirm it.
"""
import argparse
import csv
import json
import math
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, r"C:\GnssProcessor\scripts"):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.insert(0, _p)

from event_field_grade import consumer_series, leg_meta, read_events  # noqa: E402

EPOCH_S = 30.0

# leg key -> (station, injected sat, expected reference? )
LEGS = (("e1", "GOPE00CZE", "G24", True, "L5X -99, no LLI"),
        ("e2", "GOPE00CZE", "G24", True, "L1C +37, no LLI"),
        ("e3g24", "GOPE00CZE", "G24", True, "L1C +29 AND L5X +29"),
        ("e5", "GOPE00CZE", "G24", True, "L5X -31 WITH LLI"),
        ("e3g25", "GOPE00CZE", "G25", False, "L1C +29 AND L5X +29 (CONTROL)"))
ARMS = ("off", "pin")


def rel(tow, t0):
    return int(round((tow - t0) / EPOCH_S))


def if_rows(legdir, sta, t0):
    """[(k, pair, sat, abs_dev, thr, flagged, is_ref)] at one station."""
    p = os.path.join(legdir, "logs", "network_if_slip.csv")
    if not os.path.isfile(p):
        return []
    out = []
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in fh and csv.DictReader(fh):
            if r.get("station") != sta:
                continue
            try:
                k = rel(float(r["tow"]), t0)
            except (KeyError, ValueError):
                continue
            out.append((k, f"{r['sig1']}/{r['sig2']}", r["sat"],
                        float(r.get("abs_dev_m") or "nan"),
                        float(r.get("thr_m") or "nan"),
                        r.get("flagged") == "1", r.get("is_ref") == "1"))
    return out


def ref_windows(rows, sat):
    """{pair: [(k_lo, k_hi), ...]} where `sat` is that chain's reference."""
    by = {}
    for k, pair, s, _d, _t, _f, isref in rows:
        if s == sat and isref:
            by.setdefault(pair, []).append(k)
    out = {}
    for pair, ks in by.items():
        ks.sort()
        runs = []
        for k in ks:
            if runs and k == runs[-1][1] + 1:
                runs[-1][1] = k
            else:
                runs.append([k, k])
        out[pair] = [tuple(r) for r in runs]
    return out


def products_at(path, tow):
    """{key: (value, disc, sigma)} for one epoch, plus the S/R/C records."""
    vals, extra = {}, {"R": [], "S": None}
    cur = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            f = line.rstrip().split(",")
            if not f or not f[0]:
                continue
            if f[0] == "S" and extra["S"] is None:
                extra["S"] = line.rstrip()
            if f[0] == "E" and len(f) >= 3:
                try:
                    cur = int(round(float(f[2])))
                except ValueError:
                    cur = None
                continue
            if cur != tow:
                continue
            if f[0] == "R":
                extra["R"].append(line.rstrip())
            elif f[0] == "C" and len(f) >= 3:
                disc = f[4] if len(f) > 4 and f[4].isdigit() else None
                vals[("C", f[1])] = (float(f[2]), disc, None)
            elif f[0] == "P" and len(f) >= 4:
                disc = f[6] if len(f) > 6 and f[6].isdigit() else None
                sig = next((x.split("=")[1] for x in f if x.startswith("sigma=")),
                           None)
                vals[("P", f[1], f[2])] = (float(f[3]), disc, sig)
    return vals, extra


def census(runs, prefix, pb_tol, json_out=None):
    res = {}
    print("=" * 96)
    print("FP-1 REFERENCE-SLIP FAN-OUT CENSUS")
    print("  An IF-TD sample's statistic is dOMC(sat) - dOMC(ref), so the chain's")
    print("  REFERENCE has dd == 0 identically: a slip THERE is invisible on its own")
    print("  row and full-size on every other satellite of that chain.")
    print("=" * 96)

    for key, sta, sat, expect_ref, what in LEGS:
        print("\n" + "=" * 96)
        print(f"LEG {key}   station {sta}   injected {sat}   {what}"
              + ("" if expect_ref else "   <-- CONTROL: not a chain reference at k=0"))
        print("=" * 96)
        d = {"station": sta, "sat": sat, "desc": what, "arms": {}}
        for arm in ARMS:
            leg = os.path.join(runs, f"evleg_{prefix}_{key}_{arm}")
            null = os.path.join(runs, f"evleg_{prefix}_null_{arm}")
            if not os.path.isfile(os.path.join(leg, "exitcode.txt")):
                print(f"\n  {arm.upper()}: leg missing")
                continue
            t0 = float(leg_meta(leg)["event_tow"])
            a = {}
            rows = if_rows(leg, sta, t0)
            print(f"\n  ---- {arm.upper()} arm ({os.path.basename(leg)}) ----")

            # 1. reference windows
            rw = ref_windows(rows, sat)
            a["ref_windows"] = {p: v for p, v in rw.items()}
            if rw:
                for pair in sorted(rw):
                    spans = " ".join(f"[{lo:+d}..{hi:+d}]" for lo, hi in rw[pair])
                    print(f"    {sat} IS the reference of chain {pair:<10} at k {spans}")
            else:
                print(f"    {sat} is the reference of NO chain at this station")

            # 2. the injected satellite's own rows at k=0
            own = [(p, dv, th, fl, ir) for k, p, s, dv, th, fl, ir in rows
                   if s == sat and k == 0]
            a["own_at_k0"] = [(p, dv, th, fl, ir) for p, dv, th, fl, ir in own]
            print(f"    {sat}'s OWN rows at k=0:")
            for p, dv, th, fl, ir in sorted(own):
                print(f"      {p:<10} abs_dev={dv:12.6f} thr={th:8.4f} "
                      f"flagged={int(fl)} is_ref={int(ir)}"
                      + ("   <-- slip invisible here" if ir else ""))
            if not own:
                print("      (none)")

            # 3. the fan-out: other satellites flagged at this station, k in [0,+5]
            fan = [(k, p, s, dv, th) for k, p, s, dv, th, fl, ir in rows
                   if fl and s != sat and 0 <= k <= 5]
            fan.sort()
            a["fanout_flags"] = fan
            print(f"    FAN-OUT — other satellites FLAGGED at {sta}, k in [0,+5]: "
                  f"{len(fan)}")
            for k, p, s, dv, th in fan:
                print(f"      k={k:+d} {s} {p:<10} abs_dev={dv:12.4f} thr={th:8.4f}"
                      f"  ({dv / th:.0f}x)")

            # 4. flagged -> refloat, or held by a common-band verdict
            ev = read_events(leg) or []
            drains, kept = [], []
            for r in ev:
                if r.get("tier") != "T1_if_slip" or r.get("station") != sta:
                    continue
                if r.get("sat") == sat:
                    continue
                try:
                    k = rel(float(r["tow"]), t0)
                except (KeyError, ValueError):
                    continue
                if not (0 <= k <= 5):
                    continue
                det = r.get("detail", "")
                if r.get("action") == "refloat":
                    drains.append((k, r.get("sat"), det))
                if "cb_kept=" in det:
                    kept.append((k, r.get("sat"), det))
            a["refloats"] = drains
            a["cb_kept"] = kept
            print(f"    -> became a T1_if_slip REFLOAT: {len(drains)}   "
                  f"held back by a common-band keep (cb_kept=): {len(kept)}")
            for k, s, det in drains:
                print(f"      k={k:+d} {s} refloat {det}")
            for k, s, det in kept:
                print(f"      k={k:+d} {s} cb_kept  {det}")

            # 5. product re-realizations at the event epoch
            tw = int(round(t0))
            pa, xa = products_at(os.path.join(leg, "products.txt"), tw)
            pb, xb = products_at(os.path.join(null, "products.txt"), tw)
            moved = []
            for kk, (va, da, sa) in pa.items():
                vb = pb.get(kk)
                if vb is None:
                    continue
                if abs(va - vb[0]) > pb_tol:
                    moved.append((abs(va - vb[0]), kk, da, vb[1], sa, vb[2]))
            moved.sort(reverse=True)
            a["moved"] = [(round(v, 4), "/".join(kk), da, dn, sa, sn)
                          for v, kk, da, dn, sa, sn in moved]
            print(f"    PRODUCT re-realizations at k=0 (|d| > {pb_tol} m): {len(moved)}")
            for v, kk, da, dn, sa, sn in moved:
                tag = "INJECTED SAT" if len(kk) > 1 and kk[1] == sat else "COLLATERAL"
                print(f"      {'/'.join(kk):<14} |d|={v:10.4f}  disc {dn}->{da}  "
                      f"sigma {sn}->{sa}   [{tag}]")
            a["R_records"] = {"leg": xa["R"], "null": xb["R"]}
            if xa["R"] or xb["R"]:
                print(f"      R (retro bump) records: leg {xa['R']} | null {xb['R']}")

            # 6. consumer
            ca, ea = consumer_series(leg, t0)
            cb, eb = consumer_series(null, t0)
            if ea or eb or not ca or not cb:
                print(f"    CONSUMER: n/a ({ea or eb})")
            else:
                d3 = {}
                for k in ca:
                    if k in cb and 0 <= k <= 50:
                        d3[k] = math.sqrt(sum((ca[k][i] - cb[k][i]) ** 2
                                              for i in range(3)))
                if d3:
                    mx = max((v, k) for k, v in d3.items())
                    seg = [v for k, v in d3.items() if 30 <= k <= 50]
                    a["consumer"] = {"at0": d3.get(0), "at5": d3.get(5),
                                     "max": mx[0], "max_at": mx[1],
                                     "mean30_50": sum(seg) / len(seg) if seg else None}
                    print(f"    CONSUMER |d3D| event-null:  k=0 {d3.get(0, 0):.4f}  "
                          f"k=+5 {d3.get(5, 0):.4f}  max {mx[0]:.4f} @+{mx[1]}  "
                          f"mean[+30,+50] {(sum(seg) / len(seg)) if seg else 0:.4f} m")
            d["arms"][arm] = a
        res[key] = d

    if json_out:
        with open(json_out, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1, default=str)
        print(f"\njson -> {json_out}")
    return res


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default=r"C:\GnssProcessor\runs")
    ap.add_argument("--prefix", default="fp1")
    ap.add_argument("--pb-tol", type=float, default=0.5)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    census(a.runs, a.prefix, a.pb_tol, a.json)


if __name__ == "__main__":
    main()
