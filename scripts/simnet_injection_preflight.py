#!/usr/bin/env python3
"""Injection preflight: checks whether the mechanism under test is live before
a fault is injected at it.

A synthetic event-injection bench answers "does the response mechanism work".
It can only answer that if the mechanism runs on the lineage under test. On
this codebase a large, documented set of QC mechanisms is inert by construction
under `network.carry_invariant` (Card 4), because the carry vehicle they were
hosted on no longer exists. The producer's own startup banner states this:

    "Card 4 (M1) INVARIANT CARRY ON - the DD cycle-basis z-store time-links the
     S-system-invariant double differences ..., SUPERSEDING the PhaseBias shared
     carry + phase-leveling local carry + RPB carry + eq34 pin-round/shift/reref
     hygiene for phase keys (GLONASS excluded, FDMA)."
    -- src/core/config/GnssConfigLoadNetwork.cpp:6548

Injecting a fault at a superseded mechanism produces the worst possible outcome:
the run completes, the counters read zero, and the result reads as the guard
failing to catch the fault when the guard was never wired (docs/SPEC_raim_pool_rehost.md
documents two such kills on post-fit RAIM; eq34 INC-2 is a third, 537 framed
re-hosts with byte-identical output). Standing invariant #4 covers the weaker
version of this ("0 driven / 0 folded means the wiring is inert"); the
supersession class is worse, because the counters can be non-zero and specific
while the actuation is still inert.

This checker is offline, reads only the config (flattened through its `extends`
chain exactly as the loader does), and changes nothing.

    python scripts/simnet_injection_preflight.py <producer_config.json>
    python scripts/simnet_injection_preflight.py <cfg> --target slip raim_postfit
    python scripts/simnet_injection_preflight.py <cfg> --json out.json

Exit code 0 means every requested target is live. 1 means at least one is inert
or degraded (so a CI harness does not spend hours on a dead arm). 2 is a
usage/IO error.

Scope: this reads config only. It proves a mechanism is not structurally
disabled; it does not prove it has power (whether a fault actually reaches the
statistic). Power is what the injection arms measure, and under Card 4 it is
unproven for RAIM: the local ambiguity column is free each epoch and the
time-link lives in the DD chord's prior, so tension may be absorbed locally and
never reach a residual (SPEC_raim_pool_rehost.md R2). Live here means "worth
spending compute on", not "will detect".
"""
import argparse
import json
import os
import sys

# Windows consoles default to cp1252, which cannot encode the report's warning
# glyph and would raise UnicodeEncodeError mid-report, after the verdict has
# already scrolled past. scripts/simnet_grade.py:65 carries the same guard, for
# the same reason.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gnss_config_chain import ConfigChainError, get_path, load_config  # noqa: E402

LIVE, DEGRADED, INERT = "LIVE", "DEGRADED", "INERT"


def _b(cfg, dotted, default=False):
    v = get_path(cfg, dotted, default)
    return bool(v) if v is not None else default


def _f(cfg, dotted, default=0.0):
    v = get_path(cfg, dotted, default)
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def derive(cfg):
    """Resolve the gate variables the loader/driver derive, under their real names.

    Each is anchored to the source that computes it, because these are exactly the
    derivations a reader reconstructs wrongly from the JSON alone.
    """
    d = {}
    # NetworkKnobs.cpp: carry_invariant without eq34 is a hard load
    # error (GnssConfigLoadNetwork.cpp:6129-6135), so the conjunct is defensive.
    d["eq34"] = _b(cfg, "network.phase_datum_eq34")
    d["carry_invariant"] = d["eq34"] and _b(cfg, "network.carry_invariant")
    d["carry_invariant_requested"] = _b(cfg, "network.carry_invariant")
    # ApplicationNetwork.cpp:1428: carry_warm = pl_on && (...). phase_leveling is
    # a necessary condition; the remaining conjuncts can only make it true later,
    # never true without it.
    d["phase_leveling"] = _b(cfg, "network.phase_leveling")
    d["carry_warm_possible"] = d["phase_leveling"]
    d["raim"] = _b(cfg, "network.raim")
    d["raim_within_epoch"] = _b(cfg, "network.raim_within_epoch")
    d["raim_attribution"] = _b(cfg, "network.raim_attribution")
    d["we_ci"] = _b(cfg, "network.raim_we_code_iono")
    # Loader force-DISABLES both without their prerequisites
    # (GnssConfigLoadNetwork.cpp:7428-7447).
    d["raim_ci"] = (_b(cfg, "network.raim_carry_invariant")
                    and d["carry_invariant"] and d["raim"])
    d["raim_ci_refloat"] = _b(cfg, "network.raim_carry_invariant_refloat") and d["raim_ci"]
    d["withhold_sigma0"] = _f(cfg, "network.raim_withhold_sigma0")
    d["phase_datum_prioritized"] = _b(cfg, "network.phase_datum_prioritized")
    d["mw_detect"] = _b(cfg, "network.cycle_slip.mw_detect")
    d["if_phase_td"] = _b(cfg, "network.cycle_slip.if_phase_td")
    d["turbo_edit"] = _b(cfg, "network.cycle_slip.turbo_edit")
    d["datum_station_detect"] = _b(cfg, "network.cycle_slip.datum_station_detect")
    d["trace_events"] = _b(cfg, "network.trace_events")
    d["trace_flag_rates"] = _b(cfg, "network.trace_flag_rates")
    d["emit_ura"] = _b(cfg, "network.emit_ura")
    return d


def assess(d):
    """-> ordered list of {target, state, why, fix} records."""
    out = []

    def add(target, state, why, fix=""):
        out.append({"target": target, "state": state, "why": why, "fix": fix})

    # ---- slip detection ----------------------------------------------------------
    # NetworkSlipQc.cpp:160-162 / :464-465: neither carry_invariant nor carry_warm
    # gates detection; they only select the action target. So *_detections
    # increments on a Card-4 lineage while *_refloats stays 0, and that asymmetry
    # is deliberate.
    det = [k for k in ("mw_detect", "if_phase_td", "turbo_edit") if d[k]]
    if not det:
        add("slip_detect", INERT,
            "no network slip detector armed (mw_detect / if_phase_td / turbo_edit all off) "
            "- an injected slip has nothing to trip",
            "arm network.cycle_slip.mw_detect and/or .if_phase_td")
    else:
        add("slip_detect", LIVE,
            f"detectors armed: {', '.join(det)}; detection is NOT gated by "
            f"carry_invariant/carry_warm (NetworkSlipQc.cpp:160-162)")

    # The legacy refloat action is carry_warm-gated even though detection is not.
    if det:
        if d["carry_warm_possible"]:
            add("slip_refloat_legacy", LIVE,
                "phase_leveling armed, so carry_warm can become true and "
                "markAmbiguityPendingReseed runs")
        elif d["carry_invariant"]:
            add("slip_refloat_legacy", DEGRADED,
                "carry_warm is false (no phase_leveling): *_refloats stays 0 by "
                "construction.  Under carry_invariant the C3c funnel "
                "(carry_slipped_ambs) is the live actuator instead - grade THAT, "
                "not the refloat counters",
                "grade run-end *_detections + the funnel, and do not read *_refloats "
                "as a failure")
        else:
            add("slip_refloat_legacy", INERT,
                "no phase_leveling and no carry_invariant: nothing consumes a detection",
                "arm network.phase_leveling, or carry_invariant for the funnel path")

    if d["datum_station_detect"]:
        # NetworkKnobs.cpp: && !phase_datum_prioritized, and eq34 implies
        # a no-pivot datum.
        add("slip_datum_station", INERT if d["eq34"] else LIVE,
            "inert under eq34 / any no-pivot datum (NetworkKnobs.cpp)"
            if d["eq34"] else "armed and reachable")

    # ---- post-fit RAIM (T3/T3c, + T4) --------------------------------------------
    if not d["raim"]:
        add("raim_postfit", INERT, "network.raim is off", "arm network.raim")
    elif d["carry_invariant"] and not d["raim_ci"]:
        # The documented double kill.
        add("raim_postfit", INERT,
            "carry_invariant lineage without raim_carry_invariant: the pool (pl_amb) is "
            "empty because its only fill site is gated `&& !carry_invariant` "
            "(ApplicationNetworkAssembly.cpp:2585-2600), AND the seam needs carry_warm. "
            "Post-fit RAIM screens NOTHING - this is the two-kill defect",
            "arm network.raim_carry_invariant (re-hosts the pool onto the carry-neutral "
            "qc_amb census)")
    elif d["carry_invariant"] and d["raim_ci"]:
        add("raim_postfit", LIVE,
            "pool re-hosted onto qc_amb (NetworkRaim.cpp:99-101); seam gate widened to "
            "`raim_on && (carry_warm || raim_ci_on)`")
    elif d["carry_warm_possible"]:
        add("raim_postfit", LIVE, "legacy posture: phase_leveling fills pl_amb and "
                                  "carry_warm opens the seam")
    else:
        add("raim_postfit", INERT,
            "no phase_leveling: carry_warm is false so the seam is never called "
            "(ApplicationNetwork.cpp:2100)",
            "arm network.phase_leveling")

    if d["raim"] and d["raim_ci"] and not d["raim_ci_refloat"]:
        add("raim_refloat", DEGRADED,
            "screen is measure-only: flags reach NO carry consumer.  Detection is "
            "gradeable, RESPONSE is not",
            "arm network.raim_carry_invariant_refloat for the response half (field gate 3)")
    elif d["raim_ci_refloat"]:
        add("raim_refloat", LIVE,
            "actuator armed: flags queue into raim_refloat_pending and drain into "
            "carry_slipped_ambs at the next epoch head")

    # ---- within-epoch RAIM (T2) --------------------------------------------------
    if d["raim_within_epoch"]:
        if not d["carry_warm_possible"]:
            add("raim_within_epoch", INERT,
                "gated `within_epoch_armed && carry_warm` (NetworkSolveGlue.cpp:1102) and "
                "carry_warm needs phase_leveling.  NOT re-hosted by the pool card - this "
                "is the standing open gap",
                "arm network.phase_leveling (the decouple is a sanctioned open fix)")
        elif d["carry_invariant"]:
            add("raim_within_epoch", DEGRADED,
                "seam runs, but its ambiguity/phase pool is still pl_amb "
                "(NetworkSolveGlue.cpp:1211) which carry_invariant empties; only the "
                "WE-CI code/iono rows can populate" if d["we_ci"] else
                "seam runs but the pool is pl_amb, empty under carry_invariant, and "
                "raim_we_code_iono is off so no other rows enter",
                "" if d["we_ci"] else "arm network.raim_we_code_iono for code/iono rows")
        else:
            add("raim_within_epoch", LIVE, "phase_leveling fills pl_amb; seam gate open")

    if d["we_ci"] and not d["carry_warm_possible"]:
        add("raim_we_code_iono", INERT,
            "inherits the same carry_warm bracket, so the code/iono pools never "
            "populate (run-end we_ci_s0_*_max stays 0.00)",
            "arm network.phase_leveling")
    elif d["we_ci"]:
        add("raim_we_code_iono", LIVE, "code + iono pools populate; own triggers apply")

    # ---- the A4 withhold, which does not fire where the spec implies --------------
    if d["withhold_sigma0"] > 0:
        if not (d["raim_within_epoch"] and d["carry_warm_possible"]):
            add("a4_withhold", INERT,
                f"raim_withhold_sigma0={d['withhold_sigma0']} but A4 consumes sigma0_adapt, "
                "computed INSIDE `within_epoch_armed && carry_warm` "
                "(NetworkSolveGlue.cpp:1102).  It cannot fire on this posture regardless "
                "of the post-fit screen - so 'does A4 start firing' is not a question this "
                "arm can answer",
                "arm network.raim_within_epoch + network.phase_leveling, or accept that "
                "the emission-affecting surface here reduces to the URA EWMA")
        else:
            add("a4_withhold", LIVE,
                f"armed at {d['withhold_sigma0']}: epochs CAN be withheld - this arm is "
                "emission-affecting, grade it as such")

    # ---- emission side-effects of filling the pool -------------------------------
    if d["raim_ci"] and d["emit_ura"]:
        add("ura_ewma", LIVE,
            "filling the pool makes sigma0 real, and it folds into ura_sigma0_ewma "
            "(NetworkRaim.cpp:276-278) with a one-epoch lag: emitted URA WILL move. "
            "A 'measure-only' arm is not emission-neutral")

    # eq34 hygiene knobs superseded by Card 4 are handled by eq34_hygiene(): they are
    # confounders, not injection targets, so they get their own report block and do
    # not move the exit code.
    return out


def eq34_hygiene(cfg, d):
    """Armed-but-superseded eq34 hygiene knobs (reported separately: they are not
    injection targets, they are confounders that cost wall-clock time for nothing)."""
    checks = [
        ("network.phase_datum_eq34_pin_round",
         "NetworkChainDatum.cpp:136-137 `&& !carry_invariant`"),
        ("network.phase_datum_eq34_carry_shift",
         "NetworkKnobs.cpp `&& !k.carry_invariant`"),
        ("network.phase_datum_eq34_rpb_q_m2s",
         "NetworkKnobs.cpp ternary forces q to 0.0"),
        ("network.phase_datum_eq34_seed_round_rehost_frame",
         "apply path transforms local_carry/rpb_carry/prev_carry, all empty under "
         "carry_invariant: measured 537 FRAMED re-hosts with byte-identical output "
         "and +419 s wall for nothing"),
    ]
    out = []
    if not d["carry_invariant"]:
        return out
    for knob, why in checks:
        v = get_path(cfg, knob)
        if v not in (None, False, 0, 0.0):
            out.append({"knob": knob, "value": v, "why": why})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", help="producer config (flattened through its extends chain)")
    ap.add_argument("--target", nargs="*", default=None,
                    help="only these targets decide the exit code (default: all reported)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    try:
        cfg, chain = load_config(args.config)
    except (ConfigChainError, OSError, ValueError) as e:
        print(f"FAIL: {e}")
        return 2

    d = derive(cfg)
    rows = assess(d)
    hyg = eq34_hygiene(cfg, d)

    print("=" * 78)
    print("INJECTION PRE-FLIGHT — is the mechanism alive on this lineage?")
    print("=" * 78)
    print(f"  config       : {args.config}")
    if chain:
        print(f"  extends chain: {' -> '.join(os.path.basename(c) for c in chain)}")
    print(f"  posture      : eq34={d['eq34']}  carry_invariant={d['carry_invariant']}  "
          f"phase_leveling={d['phase_leveling']}")
    print(f"                 raim={d['raim']}  raim_ci={d['raim_ci']}  "
          f"raim_ci_refloat={d['raim_ci_refloat']}  within_epoch={d['raim_within_epoch']}")
    if d["carry_invariant_requested"] and not d["eq34"]:
        print("  *** carry_invariant is set WITHOUT phase_datum_eq34 — the loader treats")
        print("      that as a HARD LOAD ERROR (GnssConfigLoadNetwork.cpp:6129-6135).")
        print("      This config does not start; every verdict below is moot.")

    print("\n-- Targets " + "-" * 65)
    print(f"  {'target':<24}{'state':<10}why")
    bad = []
    for r in rows:
        mark = {LIVE: " ", DEGRADED: "!", INERT: "*"}[r["state"]]
        print(f" {mark}{r['target']:<24}{r['state']:<10}{r['why']}")
        if r["fix"]:
            print(f"  {'':<24}{'':<10}FIX: {r['fix']}")
        if r["state"] != LIVE:
            if args.target is None or r["target"] in args.target:
                bad.append(r)

    if hyg:
        print("\n-- Armed but SUPERSEDED by carry_invariant " + "-" * 34)
        print("  These are not injection targets; they are confounders that emit")
        print("  authoritative-looking telemetry while actuating nothing.")
        for h in hyg:
            print(f"  * {h['knob']} = {h['value']}")
            print(f"      {h['why']}")

    if args.target:
        missing = [t for t in args.target if not any(r["target"] == t for r in rows)]
        if missing:
            print(f"\n  *** requested target(s) not assessed: {', '.join(missing)}")
            bad.append({"target": ",".join(missing), "state": INERT, "why": "unknown target"})

    print("\n-- Verdict " + "-" * 65)
    if bad:
        print(f"  NOT CLEARED — {len(bad)} target(s) not LIVE:")
        for r in bad:
            print(f"    {r['target']}: {r['state']}")
        print("  Injecting here would produce zero counters that read as a DETECTION")
        print("  FAILURE rather than as dead wiring.  Fix the posture or pick another")
        print("  target before spending run time.")
    else:
        print("  CLEARED for injection (structurally).  Note this proves the mechanism is")
        print("  not DISABLED; it does not prove it has POWER — that is what the fault")
        print("  arms measure, and under Card 4 RAIM's power is explicitly unproven (R2).")

    if not d["trace_events"] or not d["trace_flag_rates"]:
        print("\n  ⚠ OBSERVABILITY: an injection bench grades on run-end counters.")
        if not d["trace_events"]:
            print("    network.trace_events is OFF — no network_events.csv, so no")
            print("    per-injection attribution is possible.")
        if not d["trace_flag_rates"]:
            print("    network.trace_flag_rates is OFF — no network_flag_rates.csv, so")
            print("    the null arm cannot produce an empirical alpha denominator.")

    print("=" * 78)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"config": args.config, "chain": chain, "derived": d,
                       "targets": rows, "superseded_armed": hyg,
                       "cleared": not bad}, fh, indent=2, default=str)
        print(f"wrote {args.json}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
