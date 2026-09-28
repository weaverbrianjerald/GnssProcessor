#!/usr/bin/env python3
"""Build the fault arm of a slip-injection bench from an already-synthesized null arm.

    null fixture  --(this script)-->  fault sim config + injection manifest
                                      --(gnss_simulator synth-network)--> fault fixture

Why the null arm comes first, and is an input:

A scripted slip fires with form A (an exact 1-based epoch) only if that satellite
is visible at that station at that epoch; a form-A event that never fires is a
hard run-end failure (`unfinished()`), and on a globally-distributed roster most
(station, satellite) pairs are below the horizon at any given epoch.  Rather than
guess, this reads the null arm's own `truth_visibility.csv` (which satellites
were up, per epoch) and `truth_ambiguities.csv` (which (sat, signal) pairs that
station actually tracks) and plans every event inside that intersection.  The
null arm therefore does double duty: false-alarm calibration and the visibility
oracle.

Scripted slips are documented to touch no RNG stream other than `slip_accum`,
the LLI OR, and their own counters, so the fault fixture is identical to the
null fixture everywhere except at the injected (station, sat, signal, epoch).
That is what makes a differential grade legitimate; §9.6 of
SPEC_synthetic_minimum_network requires G-B classes to be graded differentially
against the null arm, since a recovery-floor number is not meaningful unless the
null arm's own drift across the same window is accounted for.

The magnitude ladder: events are laid out as a ladder (default +/-1, 2, 3, 5, 10,
50 cycles) spread across distinct stations/satellites/epochs, so one fault run
measures a detection floor (the empirical MDB the bench exists to produce)
instead of a single yes/no.  Thresholds stay field-derived; this measures where
they land, it does not tune them.

    python scripts/simnet_inject_arms.py runs/simnet24_inject_null \\
           config/sim/simnet24_inject.json config/sim/simnet24_inject_fault.json
    python scripts/simnet_inject_arms.py <null_fixture> <base_sim> <out_sim> \\
           --ladder 1 2 3 5 10 50 --repeat-per-rung 2 --seed 7 --signal-mode per-band

`--signal-mode`:
  per-band  (default) each event hits one (sat,signal): the single-band slip a
            geometry-free / MW detector is built to see.
  mw-blind  each event hits the same delta on both bands of a pair, which cancels
            in the Melbourne-Wubbena wide-lane and is invisible to mw_detect.
            §9.7.6 measured exactly this null space; arming
            network.cycle_slip.if_phase_td is what catches it.  Use this mode to
            reproduce that discriminator on demand.
"""
import argparse
import csv
import json
import os
import random
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# Band pairing used by --signal-mode mw-blind.  Keyed by constellation letter; the
# pair is the two signals a wide-lane is normally formed from.
MW_PAIR = {"G": ("L1C", "L2W"), "E": ("L1C", "L5Q")}


def load_manifest(fixture_dir):
    p = os.path.join(fixture_dir, "network_fixture.json")
    if not os.path.exists(p):
        raise SystemExit(f"missing {p} — is that a synth-network output dir?")
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def station_visibility(fixture_dir, station):
    """-> {epoch_1based: set(sat)} from truth_visibility.csv.

    Epoch index is the ROW index + 1, matching the simulator's `epoch_counter`,
    which is what a form-A trigger compares against.  Rows are written once per
    synthesized epoch before any solvability skip, so the mapping is dense.
    """
    p = os.path.join(fixture_dir, station, "truth_visibility.csv")
    if not os.path.exists(p):
        return {}
    out = {}
    with open(p, encoding="utf-8") as fh:
        for i, r in enumerate(csv.DictReader(fh), start=1):
            sats = (r.get("sat_list") or "").replace(";", " ").replace(",", " ").split()
            out[i] = set(s for s in sats if s)
    return out


def station_signals(fixture_dir, station):
    """-> {sat: [signal, ...]} from truth_ambiguities.csv (the pairs actually tracked)."""
    p = os.path.join(fixture_dir, station, "truth_ambiguities.csv")
    if not os.path.exists(p):
        return {}
    out = {}
    with open(p, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            out.setdefault(r["satellite"], []).append(r["signal"])
    for k in out:
        out[k] = sorted(set(out[k]))
    return out


def plan_events(fixture_dir, stations, n_epochs, ladder, repeat_per_rung, seed,
                signal_mode, epoch_lo_frac, epoch_hi_frac, constellations=None,
                elev_window=None):
    """Deterministically choose (station, sat, signal, epoch, delta) tuples.

    Constraints enforced here rather than discovered at synthesis time:
      * the satellite is visible at that station at that exact epoch;
      * the (sat, signal) pair is one the station actually tracks;
      * the epoch sits inside [lo, hi) of the arc, away from both the ignition
        transient and the run end (a slip at the last epoch has no post-event
        window to be detected in, which would grade as a miss);
      * at most one event per (station, sat, signal) so magnitudes never overlap;
      * no two events share both an epoch and a satellite, so an attribution tier
        is never asked to separate two simultaneous faults on one satellite.
    """
    rng = random.Random(seed)
    lo, hi = int(n_epochs * epoch_lo_frac), int(n_epochs * epoch_hi_frac)
    if hi - lo < 4:
        raise SystemExit(f"epoch window [{lo},{hi}) too narrow — need a longer arc")

    cache = {s: (station_visibility(fixture_dir, s), station_signals(fixture_dir, s))
             for s in stations}
    usable = [s for s in stations if cache[s][0] and cache[s][1]]
    if not usable:
        raise SystemExit("no station has both truth_visibility.csv and "
                         "truth_ambiguities.csv — synthesize the null arm first")

    rungs = [(d * sgn) for d in ladder for sgn in (+1, -1)][:len(ladder) * 2]
    want = [r for r in rungs for _ in range(repeat_per_rung)]

    events, taken_key, taken_sat_epoch = [], set(), set()
    attempts = 0
    for delta in want:
        placed = False
        while attempts < 20000 and not placed:
            attempts += 1
            sta = rng.choice(usable)
            vis, sigs = cache[sta]
            epoch = rng.randrange(lo, hi)
            up = sorted(vis.get(epoch, set()) & set(sigs.keys()))
            if constellations:
                up = [s for s in up if s[:1] in constellations]
            if not up:
                continue
            sat = rng.choice(up)
            if (sat, epoch) in taken_sat_epoch:
                continue
            avail = sigs[sat]
            if signal_mode == "mw-blind":
                pair = MW_PAIR.get(sat[0])
                if not pair or not all(p in avail for p in pair):
                    continue
                chosen = list(pair)
            else:
                chosen = [rng.choice(avail)]
            if any((sta, sat, c) in taken_key for c in chosen):
                continue
            for c in chosen:
                taken_key.add((sta, sat, c))
            taken_sat_epoch.add((sat, epoch))
            events.append({"station": sta, "sat": sat, "signals": chosen,
                           "epoch": epoch, "delta_cycles": delta,
                           "mode": signal_mode})
            # elev_window: the planner still picks a (station, sat, signal) it has
            # verified the station tracks, but hands the firing decision to the
            # simulator's own Form-B elevation trigger instead of pinning an epoch.
            # The chosen epoch becomes not_before_epoch, so the event still lands
            # inside the requested arc window.
            if elev_window:
                ev = events[-1]
                ev["elev_lo_deg"], ev["elev_hi_deg"] = elev_window
                ev["not_before_epoch"] = epoch
                ev.pop("epoch")
            placed = True
        if not placed:
            print(f"  ! could not place a delta={delta:+d} event "
                  f"(exhausted {attempts} attempts) — ladder will be short")
    return events


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("null_fixture", help="synthesized NULL arm dir (the visibility oracle)")
    ap.add_argument("base_sim", help="sim config the null arm was built from")
    ap.add_argument("out_sim", help="fault sim config to write")
    ap.add_argument("--manifest", default=None,
                    help="injection manifest JSON (default <out_sim>.injection.json)")
    ap.add_argument("--ladder", type=int, nargs="*", default=[1, 2, 3, 5, 10, 50],
                    help="slip magnitudes in cycles; each is injected at both signs")
    ap.add_argument("--repeat-per-rung", type=int, default=1,
                    help="events per (magnitude, sign) — raise for a tighter detection rate")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--constellations", nargs="*", default=None, metavar="LETTER",
                    help="only inject on these system letters, e.g. --constellations G. "
                         "THE FIXTURE IS NOT THE PRODUCER: the sim writes GPS+Galileo, "
                         "and a GPS-only producer config silently ignores every E row, so "
                         "a Galileo injection lands in data nothing looks at and scores as "
                         "a detection MISS.  Measured: 10 of 24 events wasted that way on "
                         "the first bench run.  Check the producer's POSTURE line "
                         "(\"N constellation(s)\") or its products.txt before planning.")
    ap.add_argument("--signal-mode", choices=["per-band", "mw-blind"], default="per-band")
    ap.add_argument("--elev-window", type=float, nargs=2, metavar=("LO", "HI"),
                    default=None,
                    help="emit form-B (elevation-window) events instead of form-A "
                         "exact-epoch ones: the slip fires the first time the "
                         "satellite is inside [LO,HI) at that station, at or after "
                         "the planned epoch.  REQUIRED to exercise the IF-TD "
                         "REFERENCE gate, which screens the highest-elevation "
                         "satellite per constellation (measured p50 61.7 deg) while "
                         "the random slip model is capped at max_elev_deg=45 — zero "
                         "overlap, so a reference-satellite slip is otherwise "
                         "unreachable on this bench by construction "
                         "(SPEC_amb_iono_split_overcount SS6.14.5).  Try 60 90.")
    ap.add_argument("--epoch-lo-frac", type=float, default=0.35,
                    help="earliest injection as a fraction of the arc (default 0.35 — "
                         "past ignition; a slip inside convergence grades the transient)")
    ap.add_argument("--epoch-hi-frac", type=float, default=0.85,
                    help="latest injection (default 0.85 — leaves a post-event window "
                         "for the detection to happen in)")
    ap.add_argument("--flag-lli", action="store_true",
                    help="also set the receiver LLI bit (tests the LLI corroboration "
                         "path; OFF by default so the detector must find it unaided)")
    args = ap.parse_args()

    man = load_manifest(args.null_fixture)
    stations = [s["name"] for s in man["stations"] if not s.get("consumer")]
    n_epochs = int(man["n_epochs"])

    with open(args.base_sim, encoding="utf-8") as fh:
        sim = json.load(fh)

    events = plan_events(args.null_fixture, stations, n_epochs, args.ladder,
                         args.repeat_per_rung, args.seed, args.signal_mode,
                         args.epoch_lo_frac, args.epoch_hi_frac,
                         set(args.constellations) if args.constellations else None,
                         tuple(args.elev_window) if args.elev_window else None)
    if not events:
        raise SystemExit("planned zero events — nothing to inject")

    # Attach per-station scripted_slips.  The parent passes this array through into the
    # child's cycle_slips.scripted verbatim (SimConfig.cpp:897-919) and the existing
    # top-level parser validates it there, so no validation is duplicated here.
    by_station = {}
    for ev in events:
        for sig in ev["signals"]:
            entry = {"sat": ev["sat"], "signal": sig,
                     "delta_cycles": ev["delta_cycles"], "flag_lli": bool(args.flag_lli)}
            if "epoch" in ev:                       # form A: an exact epoch
                entry["epoch"] = ev["epoch"]
            else:                                   # form B: an elevation window
                entry["elev_lo_deg"]     = ev["elev_lo_deg"]
                entry["elev_hi_deg"]     = ev["elev_hi_deg"]
                entry["not_before_epoch"] = ev["not_before_epoch"]
            by_station.setdefault(ev["station"], []).append(entry)
    n_attached = 0
    for st in sim.get("network_stations", []):
        if st["name"] in by_station:
            st["scripted_slips"] = by_station[st["name"]]
            n_attached += len(st["scripted_slips"])
    if n_attached != sum(len(v) for v in by_station.values()):
        raise SystemExit("roster/manifest station-name mismatch — the fault arm would "
                         "silently inject fewer events than planned")

    sim["_injection"] = (
        f"FAULT ARM — {len(events)} scripted slip event(s), {n_attached} (sat,signal) "
        f"jump(s), planned by scripts/simnet_inject_arms.py --seed {args.seed} "
        f"--signal-mode {args.signal_mode} against {args.null_fixture}. "
        f"Grade DIFFERENTIALLY against the null arm.")
    with open(args.out_sim, "w", encoding="utf-8") as fh:
        json.dump(sim, fh, indent=2)

    mpath = args.manifest or (args.out_sim + ".injection.json")
    with open(mpath, "w", encoding="utf-8") as fh:
        json.dump({"null_fixture": args.null_fixture, "base_sim": args.base_sim,
                   "out_sim": args.out_sim, "seed": args.seed,
                   "signal_mode": args.signal_mode, "ladder": args.ladder,
                   "constellations": args.constellations,
                   "repeat_per_rung": args.repeat_per_rung, "flag_lli": bool(args.flag_lli),
                   "n_epochs": n_epochs, "interval_s": man.get("interval_s"),
                   "events": events}, fh, indent=2)

    print(f"wrote {args.out_sim}")
    print(f"wrote {mpath}")
    print(f"  {len(events)} event(s) over {len(by_station)} station(s), "
          f"{n_attached} (sat,signal) jump(s), mode={args.signal_mode}")
    hist = {}
    for e in events:
        hist[abs(e["delta_cycles"])] = hist.get(abs(e["delta_cycles"]), 0) + 1
    print(f"  ladder: " + "  ".join(f"|{k}|x{v}" for k, v in sorted(hist.items())))
    ep = sorted(e.get("epoch", e.get("not_before_epoch")) for e in events)
    if args.elev_window:
        print(f"  form B: elevation window [{args.elev_window[0]:.0f},"
              f"{args.elev_window[1]:.0f}) deg, not before epochs {ep[0]}..{ep[-1]} "
              f"of {n_epochs}")
        print("  ! form B fires on the FIRST visit to the window — verify every event "
              "actually fired (the simulator treats an unfired scripted slip as a "
              "run-end failure) and that the fixture's arcs REACH that elevation")
    else:
        print(f"  epochs {ep[0]}..{ep[-1]} of {n_epochs}")
    import collections as _c
    print(f"  constellations: "
          + "  ".join(f"{k}x{v}" for k, v in
                      sorted(_c.Counter(e["sat"][0] for e in events).items())))
    if not args.constellations:
        print("  ! --constellations not given: verify the PRODUCER solves every system")
        print("    above, or those events land in data it never reads and score as misses")
    print("\n  NEXT:")
    print(f"    gnss_simulator.exe synth-network <fault_dir> --sim-config {args.out_sim}")
    print(f"    (then a matched producer run on each arm, and simnet_inject_grade.py)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
