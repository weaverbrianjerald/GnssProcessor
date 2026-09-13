#!/usr/bin/env python3
"""FP-1 gate table — the §20 G-A..G-E bars applied to a PIN/OFF field leg matrix.

    python scripts/fp1_gate_table.py [--runs C:/GnssProcessor/runs] [--json out.json]

`scripts/event_field_grade.py` grades ONE leg against ONE null and prints
channels A-E.  The §20 gates are not that: each one compares the PIN arm's
(event - null) against the OFF arm's (event - null), with the FLOOR
|PIN-null - OFF-null| on the same channel and window as the tolerance.  This
script forms those four-leg comparisons for the whole matrix and prints the
bars, so the verdict is mechanical rather than eyeballed off ten grader dumps.

Gates, verbatim from SPEC_event_harness.md §20 (frozen 2026-09-12):
  G-A identity   which detectors fired, on what evidence, WHICH BANDS were
                 freed, and the T4 attribution-erase count at the station.  A
                 narrowing FOLLOWED by a T4_attrib/attrib_arc within +45 is a
                 wrong narrowing = FAIL.  (T4 is structurally ~0 on a
                 carry_invariant lineage — the count is reported anyway,
                 invariant #4: counters, not the absence of crashes.)
  G-B frame      station CLK / ZWD / injected-LOS iono, event - null, at
                 +1/+10/+45.  Bar: PIN |d| at the window end <= OFF |d|.
  G-C wire       injected satellite's CLK and per-signal PB, max |d| over
                 [0,+45]: PIN <= OFF.  Every OTHER satellite <= 2x the floor.
  G-D consumer   de-biased ENU, max |d3D| over [0,+50] and mean |d3D| over
                 [+30,+50].  Bar: PIN mean <= OFF mean + floor.
  G-E quiet cost PIN-null vs OFF-null: no new withhold, attrib_arc within +-3,
                 wire CLK max |d| <= 0.02 m, consumer max |d3D| <= 0.03 m.

⚠ THE G25 WINDOW TRAP (§20.1 (d)).  On the G25 legs the injected satellite
becomes the DD-trace REFERENCE at about +43, which confounds any window that
reaches past +40.  That event's rows are graded on windows ending at +40 and the
table says so — do not compare its numbers against a +45/+50 row.

⚠ THE FLOOR IS NOT FLAT IN k.  A single natural slip handled differently by the
two postures opens the two nulls at that epoch and they never re-converge, so a
scalar floor read over [0,+50] can be orders of magnitude larger than the floor
over the epochs that actually carry the event's response.  The FLOOR ONSET
section reports the first k at which each channel separates, so a gate that
passes only because the floor is wide is visible as such.
"""
import argparse
import json
import math
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, r"C:\GnssProcessor\scripts"):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.insert(0, _p)

from event_field_grade import (                      # noqa: E402
    bands_freed, consumer_series, diff_series, frame_series, leg_meta,
    max_abs, mean_abs, read_events, t4_counts, withheld)
from event_grade import read_products                # noqa: E402

EPOCH_S = 30.0
CHANNELS = ("clk", "zwd", "iono", "iono_tot")

# event key -> (station, satellite, window end, note)
MATRIX = (
    ("e1",    "GOPE00CZE", "G24", 45, 50, ""),
    ("e2",    "GOPE00CZE", "G24", 45, 50, ""),
    ("e3g24", "GOPE00CZE", "G24", 45, 50, ""),
    ("e3g25", "GOPE00CZE", "G25", 40, 40, "G25 window trap: capped at +40"),
    ("e5",    "GOPE00CZE", "G24", 45, 50, ""),
)
ARMS = ("off", "pin")


def fnum(v, d=4, w=11):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "n/a".rjust(w)
    return f"{v:{w}.{d}f}"


# ------------------------------------------------------------------ channels
def frame_diff(leg, null, sta, sat, t0):
    """{channel: {k: event-null}} for the station frame."""
    a, b = frame_series(leg, sta, sat, t0), frame_series(null, sta, sat, t0)
    out = {}
    for ch in CHANNELS:
        sa, ea = a[ch]
        sb, eb = b[ch]
        out[ch] = {} if (ea or eb) else diff_series(sa, sb)
    return out


def wire_diff(leg, null, sat, t0, post):
    """(injected-sat CLK/PB maxima, worst other) over k in [0, post]."""
    lo_t = t0
    hi_t = t0 + post * EPOCH_S + 1.0
    A = read_products(os.path.join(leg, "products.txt"), int(lo_t) - 1, int(hi_t))
    B = read_products(os.path.join(null, "products.txt"), int(lo_t) - 1, int(hi_t))
    tows = sorted(set(A) & set(B))
    clk, pb, other = None, {}, (None, None, None)
    for t in tows:
        k = int(round((t - t0) / EPOCH_S))
        if not (0 <= k <= post):
            continue
        for key, va in A[t].items():
            vb = B[t].get(key)
            if vb is None:
                continue
            d = abs(va - vb)
            if key[0] == "C" and key[1] == sat:
                clk = d if clk is None else max(clk, d)
            elif key[0] == "P" and key[1] == sat:
                pb[key[2]] = max(pb.get(key[2], 0.0), d)
            elif other[0] is None or d > other[0]:
                other = (d, "/".join(key), k)
    return {"clk": clk, "pb": pb, "pb_max": (max(pb.values()) if pb else None),
            "other": {"v": other[0], "key": other[1], "at": other[2]},
            "n_epochs": len(tows)}


def consumer_diff(leg, null, t0, post, lo_mean=30):
    a, ea = consumer_series(leg, t0)
    b, eb = consumer_series(null, t0)
    if ea or eb or not a or not b:
        return {"err": ea or eb or "no rows"}
    d3 = {}
    for k in a:
        if k not in b or not (0 <= k <= post):
            continue
        de, dn, du = (a[k][0] - b[k][0], a[k][1] - b[k][1], a[k][2] - b[k][2])
        d3[k] = math.sqrt(de * de + dn * dn + du * du)
    if not d3:
        return {"err": "no common epochs"}
    mx = max((v, k) for k, v in d3.items())
    seg = [v for k, v in d3.items() if lo_mean <= k <= post]
    return {"max3d": mx[0], "max3d_at": mx[1],
            "mean3d": (sum(seg) / len(seg)) if seg else None,
            "n": len(d3), "n_mean": len(seg), "series": d3}


def onset(d, tol=0.0):
    """First k >= 0 at which |value| exceeds tol (None = never)."""
    for k in sorted(x for x in d if x >= 0):
        if abs(d[k]) > tol:
            return k
    return None


_DRAIN = {("T1_if_slip", "if_slip"), ("T0_mw_detect", "mw_wl_slip"),
          ("T0_mw_detect", "lli_slip"), ("T0_mw_detect", "band_scope")}


def collateral(legdir, nulldir, sta, injected_sat, t0, lo=0, hi=5):
    """Drain rows at the injected STATION on OTHER satellites, event - null.

    §20's G-A inspects only the (station, satellite) that was dosed, so it is
    structurally blind to a response that lands on the station's OTHER arcs.
    That is not a hypothetical: an IF-TD chain's statistic is
    dOMC(sat) - dOMC(ref), so a slip on the chain's REFERENCE satellite is
    identically 0 on its own row and appears at full size on EVERY other
    satellite of that chain — the fan-out this census counts.
    """
    out = {}
    for tag, d in (("leg", legdir), ("null", nulldir)):
        rows = read_events(d)
        c = {}
        if rows:
            for r in rows:
                if r.get("station") != sta or r.get("sat") == injected_sat:
                    continue
                if (r.get("tier"), r.get("event")) not in _DRAIN:
                    continue
                if r.get("action") != "refloat":
                    continue
                try:
                    k = int(round((float(r["tow"]) - t0) / EPOCH_S))
                except (KeyError, ValueError):
                    continue
                if lo <= k <= hi:
                    c.setdefault(r.get("sat"), []).append(
                        (k, r.get("tier"), r.get("detail", "")[:60]))
        out[tag] = c
    net = {s: v for s, v in out["leg"].items() if s not in out["null"]}
    return out, net


# -------------------------------------------------------------------- report
def grade(runs, prefix, json_out=None):
    def leg(name):
        return os.path.join(runs, f"evleg_{prefix}_{name}")

    nulls = {a: leg(f"null_{a}") for a in ARMS}
    for p in nulls.values():
        if not os.path.isdir(p):
            raise SystemExit(f"FATAL: missing null leg {p}")
    t0 = float(leg_meta(leg("e1_off"))["event_tow"])
    res = {"runs": runs, "prefix": prefix, "event_tow": t0, "events": {}}

    # ------------------------------------------------------------ G-E floor
    print("=" * 92)
    print(f"FP-1 GATE TABLE   runs={runs}   event_tow={t0!r}")
    print("=" * 92)
    print("\nG-E  QUIET COST / FLOOR — null_pin MINUS null_off (no event anywhere)")
    print("-" * 92)
    fl_fr = frame_diff(nulls["pin"], nulls["off"], "GOPE00CZE", "G24", t0)
    fl_wi = wire_diff(nulls["pin"], nulls["off"], "G24", t0, 50)
    fl_co = consumer_diff(nulls["pin"], nulls["off"], t0, 50)
    floor = {ch: max_abs(fl_fr[ch], 0, 50)[0] for ch in CHANNELS}
    floor["wire_clk"] = fl_wi["clk"]
    floor["wire_pb"] = fl_wi["pb_max"]
    floor["wire_other"] = fl_wi["other"]["v"]
    floor["cons_max"] = fl_co.get("max3d")
    floor["cons_mean"] = fl_co.get("mean3d")
    wo, wn = withheld(nulls["pin"]), withheld(nulls["off"])
    ts_p, tn_p = t4_counts(read_events(nulls["pin"]), "GOPE00CZE", t0, 0, 45)
    ts_o, tn_o = t4_counts(read_events(nulls["off"]), "GOPE00CZE", t0, 0, 45)
    print(f"  withheld   PIN {wo}")
    print(f"             OFF {wn}")
    print(f"  attrib_arc network-wide  PIN {tn_p}  OFF {tn_o}  (d {tn_p - tn_o:+d}) "
          f"| at GOPE PIN {ts_p} OFF {ts_o}")
    print(f"  {'channel':<14}{'max|d| [0,50]':>16}{'onset k':>10}   bar")
    for ch in CHANNELS:
        print(f"  {ch:<14}{fnum(floor[ch], 4, 16)}{str(onset(fl_fr[ch])):>10}")
    print(f"  {'wire CLK G24':<14}{fnum(fl_wi['clk'], 4, 16)}{'':>10}   <= 0.02 m  "
          f"{'PASS' if (fl_wi['clk'] or 0) <= 0.02 else 'FAIL'}")
    print(f"  {'wire worst':<14}{fnum(fl_wi['other']['v'], 4, 16)}"
          f"{('+%s' % fl_wi['other']['at']):>10}   ({fl_wi['other']['key']})")
    print(f"  {'consumer max':<14}{fnum(fl_co.get('max3d'), 4, 16)}"
          f"{('+%s' % fl_co.get('max3d_at')):>10}   <= 0.03 m  "
          f"{'PASS' if (fl_co.get('max3d') or 9) <= 0.03 else 'FAIL'}")
    print(f"  {'consumer onset':<14}{'':>16}{str(onset(fl_co.get('series', {}), 1e-9)):>10}")
    ge = {
        "withheld_equal": (wo or {}).get("n_gate_withheld") == (wn or {}).get("n_gate_withheld"),
        "attrib_within_3": abs(tn_p - tn_o) <= 3,
        "wire_clk_ok": (fl_wi["clk"] or 0) <= 0.02,
        "cons_ok": (fl_co.get("max3d") or 9) <= 0.03,
    }
    ge["PASS"] = all(ge.values())
    print(f"  G-E: {'PASS' if ge['PASS'] else 'FAIL'}  {ge}")
    res["floor"] = {k: v for k, v in floor.items()}
    res["G-E"] = ge

    # ------------------------------------------------------------- per event
    for ev, sta, sat, kb, kd, note in MATRIX:
        d = {"station": sta, "sat": sat, "k_frame": kb, "k_cons": kd, "note": note}
        print("\n" + "=" * 92)
        print(f"EVENT {ev}   station {sta}  sat {sat}  windows: frame/wire [0,+{kb}] "
              f"consumer [0,+{kd}]" + (f"   ** {note}" if note else ""))
        print("=" * 92)
        arms = {}
        ok = True
        for a in ARMS:
            lp = leg(f"{ev}_{a}")
            if not os.path.isdir(lp) or not os.path.isfile(os.path.join(lp, "exitcode.txt")):
                print(f"  {a.upper()}: leg missing ({lp})")
                ok = False
                continue
            rows = read_events(lp)
            fr, vd, dr = bands_freed(rows, sta, sat, t0, -1, 5)
            ts, tn = t4_counts(rows, sta, t0, 0, 45)
            nrows = read_events(nulls[a])
            nts, ntn = t4_counts(nrows, sta, t0, 0, 45)
            arms[a] = {
                "leg": lp,
                "bands": sorted(fr),
                "verdicts": [(k, act, dd["reason"], sorted(dd["implicated"]),
                              sorted(dd["exonerated"]), dd["basis"])
                             for k, act, dd in vd],
                "t4_sta": ts - nts, "t4_net": tn - ntn,
                "withheld": withheld(lp),
                "frame": frame_diff(lp, nulls[a], sta, sat, t0),
                "wire": wire_diff(lp, nulls[a], sat, t0, kb),
                "cons": consumer_diff(lp, nulls[a], t0, kd),
            }
        if not ok:
            d["incomplete"] = True
            res["events"][ev] = d
            continue

        # ---- G-A
        print("\n  G-A IDENTITY")
        for a in ARMS:
            A = arms[a]
            print(f"    {a.upper():<4} bands freed: "
                  f"{'+'.join(A['bands']) if A['bands'] else '(none)'}"
                  f"   T4 at {sta}: {A['t4_sta']:+d}  network: {A['t4_net']:+d}")
            for k, act, reason, imp, exo, basis in A["verdicts"]:
                print(f"         k={k:+d} {act} reason={reason or '-'} "
                      f"implicated={'+'.join(imp) or '-'} "
                      f"exonerated={'+'.join(exo) or '-'}")
                print(f"            basis={basis}")
        ga_narrow_fail = any(arms[a]["bands"] and arms[a]["t4_sta"] > 0 for a in ARMS)
        print(f"    wrong-narrowing tripwire (narrowing + T4 at station within +45): "
              f"{'FIRED = FAIL' if ga_narrow_fail else 'silent'}")

        print(f"    COLLATERAL at {sta} on satellites OTHER than {sat}, "
              f"k in [0,+5] (event - null)")
        coll = {}
        for a in ARMS:
            both, net = collateral(arms[a]["leg"], nulls[a], sta, sat, t0)
            coll[a] = {"net_sats": sorted(net),
                       "net_rows": sum(len(v) for v in net.values())}
            if net:
                for s in sorted(net):
                    print(f"      {a.upper()}  {s}: "
                          + "; ".join(f"k={k:+d} {t} {dd}" for k, t, dd in net[s]))
            else:
                print(f"      {a.upper()}  (none)")
        d["collateral"] = coll

        # ---- G-B
        print(f"\n  G-B STATION FRAME (event - null, m) — bar: PIN |d| at +{kb} <= OFF")
        print(f"    {'channel':<10}{'OFF @+%d' % kb:>13}{'PIN @+%d' % kb:>13}"
              f"{'OFF max':>12}{'PIN max':>12}{'floor':>12}   bar")
        gb = {}
        for ch in CHANNELS:
            vo = arms["off"]["frame"][ch].get(kb)
            vp = arms["pin"]["frame"][ch].get(kb)
            mo = max_abs(arms["off"]["frame"][ch], 0, kb)[0]
            mp = max_abs(arms["pin"]["frame"][ch], 0, kb)[0]
            f = floor[ch]
            bar = None
            if vo is not None and vp is not None:
                bar = abs(vp) <= abs(vo) + (f or 0.0)
                gb[ch] = bar
            print(f"    {ch:<10}{fnum(vo, 4, 13)}{fnum(vp, 4, 13)}{fnum(mo, 4, 12)}"
                  f"{fnum(mp, 4, 12)}{fnum(f, 4, 12)}   "
                  f"{'-' if bar is None else ('PASS' if bar else 'FAIL')}")
        t4_bar = arms["pin"]["t4_sta"] <= arms["off"]["t4_sta"]
        print(f"    attribution-erase count PIN {arms['pin']['t4_sta']:+d} <= OFF "
              f"{arms['off']['t4_sta']:+d}: {'PASS' if t4_bar else 'FAIL'}")
        gb["t4"] = t4_bar
        gb["PASS"] = all(gb.values())

        # ---- G-C
        print(f"\n  G-C WIRE (event - null, max |d| over [0,+{kb}]) — bar: PIN <= OFF; "
              f"others <= 2x floor ({fnum(2 * (floor['wire_other'] or 0), 3, 1).strip()} m)")
        wo_, wp_ = arms["off"]["wire"], arms["pin"]["wire"]
        print(f"    {'quantity':<16}{'OFF':>13}{'PIN':>13}   bar")
        gc = {}
        gc["clk"] = (wp_["clk"] or 0) <= (wo_["clk"] or 0) + (floor["wire_clk"] or 0)
        print(f"    {sat + ' CLK':<16}{fnum(wo_['clk'], 5, 13)}{fnum(wp_['clk'], 5, 13)}"
              f"   {'PASS' if gc['clk'] else 'FAIL'}")
        for sig in sorted(set(wo_["pb"]) | set(wp_["pb"])):
            a_, b_ = wo_["pb"].get(sig), wp_["pb"].get(sig)
            okk = (b_ or 0) <= (a_ or 0) + (floor["wire_pb"] or 0)
            gc[f"pb_{sig}"] = okk
            print(f"    {sat + ' PB ' + sig:<16}{fnum(a_, 5, 13)}{fnum(b_, 5, 13)}"
                  f"   {'PASS' if okk else 'FAIL'}")
        lim = 2 * (floor["wire_other"] or 0)
        for a in ARMS:
            w = arms[a]["wire"]["other"]
            okk = (w["v"] or 0) <= lim
            gc[f"other_{a}"] = okk
            print(f"    {'worst other ' + a.upper():<16}{fnum(w['v'], 4, 13)}"
                  f"{('@+%s %s' % (w['at'], w['key'])):>20}  "
                  f"{'PASS' if okk else 'FAIL'}")
        gc["PASS"] = all(gc.values())

        # ---- G-D
        print(f"\n  G-D CONSUMER de-biased (event - null) — bar: PIN mean <= OFF mean "
              f"+ floor ({fnum(floor['cons_mean'], 4, 1).strip()} m)")
        co_, cp_ = arms["off"]["cons"], arms["pin"]["cons"]
        gd = {}
        if "err" in co_ or "err" in cp_:
            print(f"    n/a  OFF:{co_.get('err', '-')}  PIN:{cp_.get('err', '-')}")
            gd["PASS"] = False
        else:
            gd["mean"] = (cp_["mean3d"] <= co_["mean3d"] + (floor["cons_mean"] or 0))
            print(f"    {'quantity':<24}{'OFF':>13}{'PIN':>13}   bar")
            print(f"    {'max |d3D| [0,+%d]' % kd:<24}{fnum(co_['max3d'], 5, 13)}"
                  f"{fnum(cp_['max3d'], 5, 13)}   (at +{co_['max3d_at']} / "
                  f"+{cp_['max3d_at']})")
            print(f"    {'mean |d3D| [+30,+%d]' % kd:<24}{fnum(co_['mean3d'], 5, 13)}"
                  f"{fnum(cp_['mean3d'], 5, 13)}   "
                  f"{'PASS' if gd['mean'] else 'FAIL'}")
            gd["PASS"] = all(v for k, v in gd.items() if k != "PASS")

        d["G-A"] = {"off": arms["off"]["bands"], "pin": arms["pin"]["bands"],
                    "verdict_off": arms["off"]["verdicts"],
                    "verdict_pin": arms["pin"]["verdicts"],
                    "t4_off": arms["off"]["t4_sta"], "t4_pin": arms["pin"]["t4_sta"],
                    "tripwire_fired": ga_narrow_fail}
        d["G-B"], d["G-C"], d["G-D"] = gb, gc, gd
        d["wire"] = {a: {k: v for k, v in arms[a]["wire"].items() if k != "pb"}
                     for a in ARMS}
        d["cons"] = {a: {k: v for k, v in arms[a]["cons"].items() if k != "series"}
                     for a in ARMS}
        print(f"\n  EVENT {ev}: G-A {'FAIL' if ga_narrow_fail else 'pass'} | "
              f"G-B {'PASS' if gb['PASS'] else 'FAIL'} | "
              f"G-C {'PASS' if gc['PASS'] else 'FAIL'} | "
              f"G-D {'PASS' if gd['PASS'] else 'FAIL'}")
        res["events"][ev] = d

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
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    grade(a.runs, a.prefix, a.json)


if __name__ == "__main__":
    main()
