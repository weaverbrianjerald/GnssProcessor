#!/usr/bin/env python3
"""Config POSTURE check — which validated-good knobs is this config leaving OFF?

The project ships new work default-OFF + byte-identity-gated (standing invariant #6), which is
correct for new work but has no PROMOTION step: nothing ever moves from "validated good" to
"default on".  Validated knobs therefore accumulate in the OFF pile and get re-discovered by
accident, one wasted run at a time.

This reads the recommendation ledger (docs/CONFIG_promotion_ledger.json) and reports, for a
given config, every RECOMMEND_ON knob that is off/absent and every RECOMMEND_SCOPED knob whose
scope plausibly applies.  It does NOT edit anything.

⚠️ SCOPE IS LOAD-BEARING — do not read this as "turn them all on".  The ledger's SCOPED tier
exists because knobs validated at one scale can be harmful at another: network.mw_wl
.bridge_sample_debias was validated at 50 stations and measured net-harmful at 305
(short-baseline wrong-integer rate 0.0% -> 12.5%).

    python scripts/config_posture_check.py runs/guards_cont/leg.json
    python scripts/config_posture_check.py <cfg> --tier all
"""
import argparse
import json
import os
import sys

LEDGER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "docs", "CONFIG_promotion_ledger.json")


def norm(knob):
    """Ledger entries carry prose suffixes ('network.x (W6 slice)', 'a / b') — keep the path."""
    return knob.split(" ")[0].split("/")[0].strip().rstrip(".,;:")


def resolve(cfg, dotted):
    """-> (found, value). Dotted path into nested dicts; tolerates a flat alias."""
    cur = cfg
    for part in norm(dotted).split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            return False, None
    return True, cur


def is_off(v):
    return v is None or v is False or v == 0 or v == 0.0 or v == ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("--ledger", default=LEDGER)
    ap.add_argument("--tier", default="on", choices=("on", "scoped", "all"))
    a = ap.parse_args()

    led = json.load(open(a.ledger, encoding="utf-8"))
    cfg = json.load(open(a.config, encoding="utf-8"))
    nsta = len(cfg.get("network", {}).get("stations", []))
    print(f"# posture check: {a.config}")
    print(f"#   {nsta} stations, mode={cfg.get('processing',{}).get('mode','?')}, "
          f"systems={cfg.get('processing',{}).get('active_systems','?')}, "
          f"freq={cfg.get('processing',{}).get('frequency_mode','?')}")
    print(f"#   ledger: {len(led['knobs'])} knobs "
          f"({sum(1 for k in led['knobs'] if k['verdict']=='RECOMMEND_ON')} ON, "
          f"{sum(1 for k in led['knobs'] if k['verdict']=='RECOMMEND_SCOPED')} SCOPED)")

    want = {"on": ("RECOMMEND_ON",),
            "scoped": ("RECOMMEND_SCOPED",),
            "all": ("RECOMMEND_ON", "RECOMMEND_SCOPED")}[a.tier]
    n_gap = 0
    for tier in want:
        rows = [k for k in led["knobs"] if k["verdict"] == tier]
        gaps = []
        for k in rows:
            found, v = resolve(cfg, k["knob"])
            if not found or is_off(v):
                gaps.append((k, found, v))
        print(f"\n=== {tier}: {len(gaps)} of {len(rows)} not armed ===")
        for k, found, v in gaps:
            n_gap += 1
            state = "ABSENT (uses code default)" if not found else f"= {v!r}"
            print(f"  {k['knob']}  {state}")
            print(f"      default at {k['default_at']}")
            print(f"      scope: {k['scope']}")
            if k.get("interlock"):
                print(f"      interlock: {k['interlock']}")
    print(f"\n{n_gap} gap(s).  Read the scope before arming — see the ledger's SCOPED tier "
          f"and docs/CONFIG_promotion_ledger.md.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
