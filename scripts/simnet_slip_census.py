#!/usr/bin/env python3
"""Organic-slip detection census: truth_slips (every row, not just scripted) joined
against the run's slip/RAIM response events.  The injection bench
(simnet_inject_grade.py) answers "does an injected event at a chosen magnitude trip
the guards"; this script answers, on a fixture with a natural slip population, "what
fraction of the truth slips produced any response, through which tier, at what
latency, and how many responses had no truth slip behind them" (the false-alarm
census a truth-carrying fixture makes possible without a null twin).

Classification per truth slip (earliest matching event wins):
  DETECTED   : a slip-tier event (T0_mw_detect, T1_if_slip, DATUM_SLIP) on the same
               (station,sat), or network-wide for refsat/datum events (sat only),
               within the latency window.
  RESPONDED  : no slip-tier event, but a RAIM-tier response (T2/T3/T4 event or a
               network_raim.csv refloated row) on (sat) within the window: the slip
               was not named but the network reacted.
  SILENT     : nothing fired.  Not automatically a failure: a small slip absorbed by
               a free ambiguity refloat at the next epoch's chord re-tap can be
               harmless; the channel grades (wire/integer/interaction) price that.
               The census reports; it does not verdict (invariant: report counters,
               grade channels).

Traps defended:
  * truth_slips row counts differ per station: walk every <sta>/truth_slips.csv
  * `flagged` (LLI) slips are the easy class: the table splits on it
  * refsat_reset / datum events carry no (or the pivot's) station: matched by sat
  * an event row belongs to at most one truth slip (earliest-tow greedy)

Usage:
  python scripts/simnet_slip_census.py <run_dir> <fixture_dir>
         [--latency-epochs 3] [--json out.json]
"""
import argparse
import csv
import json
import math
import os
import sys
from collections import Counter, defaultdict

SLIP_TIERS = ("T0_mw_detect", "T1_if_slip", "DATUM_SLIP")
RAIM_TIERS = ("T2_within_epoch", "T3_raim", "T4_attrib")


def load_csv(path):
    if not path or not os.path.isfile(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def find_trace(run_dir, name):
    for p in (os.path.join(run_dir, "logs", name), os.path.join(run_dir, name)):
        if os.path.isfile(p):
            return p
    return None


def truth_slips(fixture_dir):
    rows = []
    for d in sorted(os.listdir(fixture_dir)):
        p = os.path.join(fixture_dir, d, "truth_slips.csv")
        if not os.path.isfile(p):
            continue
        for r in load_csv(p):
            rows.append({
                "station": d,
                "tow": float(r["tow_s"]),
                "sat": r["satellite"],
                "signal": r["signal"],
                "delta": int(float(r["delta_cycles"])),
                "flagged": (r.get("flagged") or "0").strip() in ("1", "true"),
                "elev": float(r.get("elev_deg") or "nan"),
            })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("fixture_dir")
    ap.add_argument("--latency-epochs", type=int, default=3)
    ap.add_argument("--interval-s", type=float, default=30.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    truth = truth_slips(args.fixture_dir)
    if not truth:
        sys.exit("ERROR: no truth slips found — wrong fixture, or a quiet fixture "
                 "(this census needs a natural slip population)")

    ev = load_csv(find_trace(args.run_dir, "network_events.csv"))
    if not ev:
        sys.exit("ERROR: no network_events.csv — arm network.trace_events "
                 "(absence is did-not-run, never 'no events')")
    raim = load_csv(find_trace(args.run_dir, "network_raim.csv"))

    win = args.latency_epochs * args.interval_s

    # --- index events -----------------------------------------------------------
    slip_ev = []      # (tow, sat, station, tier, event, matched-flag holder)
    raim_ev = []
    for r in ev:
        tier = r.get("tier") or ""
        base = {"tow": float(r["tow"]), "sat": r.get("sat") or "",
                "station": r.get("station") or "", "tier": tier,
                "event": r.get("event") or "", "used": False}
        if tier in SLIP_TIERS or tier.startswith("DATUM"):
            slip_ev.append(base)
        elif tier in RAIM_TIERS:
            raim_ev.append(base)
    for r in raim:
        if (r.get("refloated") or "0").strip() not in ("", "0"):
            raim_ev.append({"tow": float(r["tow"]), "sat": r.get("sat") or "",
                            "station": r.get("station") or "", "tier": "raim_refloat",
                            "event": "refloated", "used": False})

    slip_ev.sort(key=lambda e: e["tow"])
    raim_ev.sort(key=lambda e: e["tow"])

    def match(pool, ts, require_station=True):
        best = None
        for e in pool:
            if e["used"] or e["sat"] != ts["sat"]:
                continue
            if e["tow"] < ts["tow"] - 1e-3 or e["tow"] > ts["tow"] + win + 1e-3:
                continue
            if require_station and e["station"] and e["station"] != ts["station"]:
                continue
            best = e
            break
        if best:
            best["used"] = True
        return best

    results = []
    for ts in sorted(truth, key=lambda t: t["tow"]):
        hit = match(slip_ev, ts, require_station=True) or \
              match(slip_ev, ts, require_station=False)
        if hit:
            cls = "DETECTED"
        else:
            hit = match(raim_ev, ts, require_station=False)
            cls = "RESPONDED" if hit else "SILENT"
        results.append({**ts, "class": cls,
                        "tier": hit["tier"] if hit else None,
                        "latency_ep": round((hit["tow"] - ts["tow"]) / args.interval_s)
                                      if hit else None})

    # --- tables -----------------------------------------------------------------
    n = len(results)
    by_class = Counter(r["class"] for r in results)
    by_tier = Counter(r["tier"] for r in results if r["tier"])
    print(f"truth slips: {n} across {len({r['station'] for r in results})} stations "
          f"(window {args.latency_epochs} ep)")
    for c in ("DETECTED", "RESPONDED", "SILENT"):
        print(f"  {c:10s} {by_class.get(c,0):5d}  ({100.0*by_class.get(c,0)/n:.1f}%)")
    print(f"  by tier: {dict(by_tier)}")

    def split(pred, label):
        sub = [r for r in results if pred(r)]
        if not sub:
            return
        c = Counter(r["class"] for r in sub)
        print(f"  {label:28s} n={len(sub):4d}  det {100.0*c.get('DETECTED',0)/len(sub):5.1f}%  "
              f"resp {100.0*c.get('RESPONDED',0)/len(sub):5.1f}%  "
              f"silent {100.0*c.get('SILENT',0)/len(sub):5.1f}%")

    print("splits:")
    split(lambda r: r["flagged"], "LLI-flagged")
    split(lambda r: not r["flagged"], "unflagged")
    for lo, hi in ((0, 5), (5, 15), (15, 50), (50, 10**9)):
        split(lambda r, lo=lo, hi=hi: lo <= abs(r["delta"]) < hi, f"|delta| in [{lo},{hi})")
    split(lambda r: r["elev"] >= 30, "elev >= 30")
    split(lambda r: r["elev"] < 30, "elev < 30")

    silent = [r for r in results if r["class"] == "SILENT"]
    if silent:
        worst = sorted(silent, key=lambda r: -abs(r["delta"]))[:10]
        print("largest SILENT slips (delta, band, sta, sat, tow, elev, LLI):")
        for r in worst:
            print(f"    {r['delta']:+5d} cyc {r['signal']:4s} {r['station']} {r['sat']} "
                  f"tow {r['tow']:.0f} elev {r['elev']:.1f} lli={int(r['flagged'])}")

    # --- false-alarm census -----------------------------------------------------
    fa = [e for e in slip_ev if not e["used"]]
    fa_by_tier = Counter(e["tier"] for e in fa)
    print(f"slip-tier events with NO truth slip in window: {len(fa)} "
          f"(of {len(slip_ev)} total) by tier {dict(fa_by_tier)}")
    print("  (refsat/common-mode resets and ignition candidates land here by design — "
          "attribute before reading as false alarms)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"n_truth": n, "by_class": dict(by_class),
                       "by_tier": dict(by_tier),
                       "silent": [{k: r[k] for k in ("station", "sat", "signal", "tow",
                                                     "delta", "elev", "flagged")}
                                  for r in silent],
                       "false_alarms_by_tier": dict(fa_by_tier),
                       "n_slip_events": len(slip_ev)}, f, indent=1)
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()
