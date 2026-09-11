#!/usr/bin/env python3
"""EVENT HARNESS — slip-DOSE legs (Study 4, docs/SPEC_event_harness.md §12).

The single-event legs price ONE event.  A dose leg prices a SCHEDULE: a converged
snapshot is resumed and a scripted sequence of true slips is injected at a chosen
rate — the campaign bed's own statistics by default — on one station, a few, or the
whole roster.  The question it answers is the one the storm beds cannot: does a
CONVERGED network stay converged under the campaign's slip rate, and if it decays,
which station channel (ZWD / receiver clock / per-LOS iono / arc levels) goes first
and which response tier (detector re-level, T4 erase) drives it?

Campaign-bed slip statistics (runs/simnet60_campaign_wu93, measured 2026-09-11):
2486 slips / 61 stations / 5.98 h = 6.8 per station-hour; 2476 of 2481 events are
SINGLE-band; |delta| 12..125 cycles (p10 26, p50 50, p90 89); 50.6 % LLI-flagged;
bands uniform within the constellation (GPS L1C/L2W/L5Q, Galileo L1C/L5Q/L7Q).
The generator samples |delta| from that empirical distribution, so a dose leg's
events are the campaign's events in kind.

    # a 2 h leg, one station, the campaign rate (Poisson, mean 6.8/h x exposure)
    python scripts/ev_dose_legs.py make --seed-run <abs seed dir> --snapshot-ep 665 \
        --epochs 240 --name dose1_s21 --stations S21000SIM --rate-mult 1.0 --rng 1 \
        --exe <abs exe> --run

    # the matched NULL (same length, no events)
    python scripts/ev_dose_legs.py make --null --seed-run ... --snapshot-ep 665 \
        --epochs 240 --name null240 --exe <abs exe> --run

    # an ATTRIBUTION twin: same schedule (same --rng), one knob changed.  NOTE:
    # `raim_attribution: false` is silently RE-ARMED by the golden's raim_within_epoch
    # (loader ~4752); the T4 starve that survives a resume is the persistence window.
    python scripts/ev_dose_legs.py make ... --rng 1 --extra '{"network":{"raim_arc_persist_epochs":1000000}}'

    # the whole roster (needs the raised injector cap, 4096 events)
    python scripts/ev_dose_legs.py make ... --stations all --rng 1

    # a density rung: the SAME events, a subset of stations, or every 2nd event
    python scripts/ev_dose_legs.py make ... --from-leg runs/evleg_doseall --stations S05000SIM,S10000SIM
    python scripts/ev_dose_legs.py make ... --from-leg runs/evleg_doseall --stations all --thin 2

Every leg is built THROUGH scripts/event_leg.py (make-leg / null), so it inherits the
validated warm-restart posture, the seed-copy rule and the half-epoch back-off; this
script only widens ``network.inject.events`` to the full schedule afterwards and
records it in ``leg_meta.json`` (``events``) and ``schedule.csv`` for the graders.
A leg is not re-runnable in place — use ``python scripts/event_leg.py rerun``.
"""
import argparse
import csv
import glob
import json
import math
import os
import random
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)
import event_leg  # noqa: E402  (reuses _run, read_snapshot_header, DEFAULT_EXE)

EPOCH_S = 30.0
CAMPAIGN_FIXTURE = os.path.join(REPO, "runs", "simnet60_campaign_wu93")
QUIET_FIXTURE = os.path.join(REPO, "runs", "simnet60_quiet_wu93_24h")
CAMPAIGN_RATE_PER_STA_H = 6.8
LLI_FRACTION = 0.506
BANDS = {"G": ["L1C", "L2W", "L5Q"], "E": ["L1C", "L5Q", "L7Q"]}


# ------------------------------------------------------------------ helpers
def campaign_sizes(fixture=CAMPAIGN_FIXTURE):
    """The campaign bed's empirical |delta_cycles| population (one entry per slip)."""
    vals = []
    for f in glob.glob(os.path.join(fixture, "S*", "truth_slips.csv")):
        with open(f, newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    vals.append(abs(int(round(float(r["delta_cycles"])))))
                except (KeyError, ValueError):
                    pass
    if not vals:
        raise SystemExit(f"no truth_slips.csv under {fixture} — cannot sample slip sizes")
    return vals


def elevations(fixture, sta, lo_tow, hi_tow):
    """{tow_int: {sat: elev_deg}} from the station's truth_atmo.csv, window only."""
    p = os.path.join(fixture, sta, "truth_atmo.csv")
    if not os.path.isfile(p):
        raise SystemExit(f"missing {p}")
    out = {}
    with open(p, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                t = float(r["tow_s"])
            except (KeyError, ValueError):
                continue
            if t < lo_tow - 1 or t > hi_tow + 1:
                continue
            out.setdefault(int(round(t)), {})[r["satellite"]] = float(r["elev_deg"])
    return out


def poisson(rng, lam):
    """Knuth's method — fine for the means this harness uses (< ~100)."""
    if lam <= 0:
        return 0
    L, k, p = math.exp(-lam), 0, 1.0
    while True:
        k += 1
        p *= rng.random()
        if p <= L:
            return k - 1


def schedule(stations, fixture, first_tow, epochs, first_ep, last_margin, rate_mult,
             count_per_station, rng, min_elev, min_gap_ep, sizes):
    """Sample the slip schedule.  Returns a list of event dicts (with an extra 'ep')."""
    lo_ep, hi_ep = first_ep, epochs - last_margin
    if hi_ep < lo_ep:
        raise SystemExit(f"exposure window empty: epochs {epochs}, first-ep {first_ep}, "
                         f"last-margin {last_margin}")
    hours = (hi_ep - lo_ep + 1) * EPOCH_S / 3600.0
    lo_tow, hi_tow = first_tow + lo_ep * EPOCH_S, first_tow + hi_ep * EPOCH_S
    events = []
    for sta in stations:
        if count_per_station is not None:
            n = count_per_station
        else:
            n = max(1, poisson(rng, CAMPAIGN_RATE_PER_STA_H * hours * rate_mult))
        el = elevations(fixture, sta, lo_tow, hi_tow)
        used, mine, tries = [], [], 0
        while len(mine) < n and tries < 5000:
            tries += 1
            ep = rng.randint(lo_ep, hi_ep)
            tow = first_tow + ep * EPOCH_S
            cands = sorted(s for s, e in el.get(int(round(tow)), {}).items()
                           if e >= min_elev and s[0] in BANDS)
            if not cands:
                continue
            sat = rng.choice(cands)
            if any(s == sat and abs(e - ep) < min_gap_ep for s, e in used):
                continue
            sig = rng.choice(BANDS[sat[0]])
            mag = rng.choice(sizes)
            sign = rng.choice((-1, 1))
            mine.append({"kind": "slip", "station": sta, "sat": sat, "signal": sig,
                         "tow": tow, "delta_cycles": float(sign * mag),
                         "flag_lli": rng.random() < LLI_FRACTION, "ep": ep})
            used.append((sat, ep))
        if len(mine) < n:
            print(f"WARNING: {sta}: only {len(mine)} of {n} events placed "
                  f"(elevation {min_elev} deg, gap {min_gap_ep} ep)")
        events.extend(mine)
    events.sort(key=lambda e: (e["tow"], e["station"], e["sat"]))
    return events


def deep_merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            deep_merge(dst[k], v)
        else:
            dst[k] = v


def station_names(seed_run):
    with open(os.path.join(seed_run, "seed.json"), encoding="utf-8") as fh:
        cfg = json.load(fh)
    return [s["name"] for s in cfg.get("network", {}).get("stations", [])]


# --------------------------------------------------------------------- make
def cmd_make(a):
    seed_run = a.seed_run if os.path.isabs(a.seed_run) else os.path.join(REPO, a.seed_run)
    snap = os.path.join(seed_run, f"state_snapshot.txt.ep{a.snapshot_ep:04d}")
    if not os.path.isfile(snap):
        raise SystemExit(f"missing {snap}")
    week, tow, n_emitted, fingerprint = event_leg.read_snapshot_header(snap)
    first_tow = tow + EPOCH_S
    legdir = os.path.join(REPO, "runs", f"evleg_{a.name}")
    if os.path.isdir(legdir) and os.path.isfile(os.path.join(legdir, "leg.json")):
        raise SystemExit(f"{legdir} exists — pick another --name or delete it")

    rng = random.Random(a.rng)
    events = []
    if a.from_leg and not a.null:
        # A SUBSET of an existing leg's schedule: same events, fewer stations
        # (--stations) and/or every --thin-th event.  This is how a density ladder
        # keeps the events themselves identical across rungs.
        src = a.from_leg if os.path.isabs(a.from_leg) else os.path.join(REPO, a.from_leg)
        with open(os.path.join(src, "leg_meta.json"), encoding="utf-8") as fh:
            src_meta = json.load(fh)
        if abs(src_meta["first_tow"] - first_tow) > 0.5:
            raise SystemExit("--from-leg must resume from the same snapshot (first_tow differs)")
        events = list(src_meta.get("events", []))
        if a.stations.lower() != "all":
            keep = {s.strip() for s in a.stations.split(",") if s.strip()}
            events = [e for e in events if e["station"] in keep]
        events.sort(key=lambda e: (e["tow"], e["station"], e["sat"]))
        if a.thin > 1:
            events = events[::a.thin]
        events = [e for e in events if e["ep"] <= a.epochs - a.last_margin]
        if not events:
            raise SystemExit("subset schedule is empty")
    elif not a.null:
        roster = station_names(seed_run)
        if a.stations.lower() == "all":
            stations = roster
        else:
            stations = [s.strip() for s in a.stations.split(",") if s.strip()]
            bad = [s for s in stations if s not in roster]
            if bad:
                raise SystemExit(f"not in the seed roster: {bad}")
        fixture = a.fixture if os.path.isabs(a.fixture) else os.path.join(REPO, a.fixture)
        events = schedule(stations, fixture, first_tow, a.epochs, a.first_ep,
                          a.last_margin, a.rate_mult, a.count_per_station, rng,
                          a.min_elev, a.min_gap_ep, campaign_sizes())
        if not events:
            raise SystemExit("schedule is empty")

    cmd = [sys.executable, os.path.join(_HERE, "event_leg.py")]
    if a.null:
        cmd += ["null"]
    else:
        first = {k: v for k, v in events[0].items() if k != "ep"}
        cmd += ["make-leg", "--at-ep", str(events[0]["ep"]), "--event", json.dumps(first)]
    cmd += ["--seed-run", seed_run, "--snapshot-ep", str(a.snapshot_ep),
            "--epochs", str(a.epochs), "--name", a.name, "--exe", a.exe]
    subprocess.check_call(cmd)

    cfg_path = os.path.join(legdir, "leg.json")
    with open(cfg_path, encoding="utf-8") as fh:
        cfg = json.load(fh)
    net = cfg.setdefault("network", {})
    if events:
        net["inject"] = {"enable": True,
                         "events": [{k: v for k, v in e.items() if k != "ep"} for e in events]}
    extra = json.loads(a.extra) if a.extra else {}
    if extra:
        deep_merge(cfg, extra)
    with open(cfg_path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)

    meta_path = os.path.join(legdir, "leg_meta.json")
    with open(meta_path, encoding="utf-8") as fh:
        meta = json.load(fh)
    meta["events"] = events
    meta["dose"] = {"stations": (a.stations if not a.null else None), "rate_mult": a.rate_mult,
                    "count_per_station": a.count_per_station, "rng": a.rng,
                    "first_ep": a.first_ep, "last_margin": a.last_margin,
                    "min_elev": a.min_elev, "min_gap_ep": a.min_gap_ep,
                    "fixture": a.fixture, "n_events": len(events),
                    "n_stations": len({e["station"] for e in events}),
                    "from_leg": a.from_leg or None, "thin": a.thin}
    meta["extra"] = extra
    with open(meta_path, "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)

    with open(os.path.join(legdir, "schedule.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["ep", "tow", "station", "sat", "signal", "delta_cycles", "flag_lli"])
        for e in events:
            w.writerow([e["ep"], f"{e['tow']:.3f}", e["station"], e["sat"], e["signal"],
                        f"{e['delta_cycles']:.0f}", int(e["flag_lli"])])

    per_sta = {}
    for e in events:
        per_sta[e["station"]] = per_sta.get(e["station"], 0) + 1
    print(f"DOSE leg {legdir}")
    print(f"  events {len(events)} on {len(per_sta)} station(s); per-station "
          f"min/max {min(per_sta.values()) if per_sta else 0}/"
          f"{max(per_sta.values()) if per_sta else 0}; rng {a.rng}; extra {extra or '-'}")
    for e in events[:12]:
        print(f"  +{e['ep']:4d}  {e['station']} {e['sat']} {e['signal']} "
              f"{e['delta_cycles']:+.0f} cyc lli={int(e['flag_lli'])}")
    if len(events) > 12:
        print(f"  ... ({len(events) - 12} more; see schedule.csv)")
    if a.run:
        return event_leg._run(a.exe, cfg_path, legdir)
    return 0


def cmd_run(a):
    legdir = a.legdir if os.path.isabs(a.legdir) else os.path.join(REPO, a.legdir)
    if os.path.isfile(os.path.join(legdir, "products.txt")):
        raise SystemExit(f"{legdir} already ran — use `python scripts/event_leg.py rerun`")
    return event_leg._run(a.exe, os.path.join(legdir, "leg.json"), legdir)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make", help="build (and optionally run) a dose leg or its null")
    m.add_argument("--seed-run", required=True)
    m.add_argument("--snapshot-ep", type=int, required=True)
    m.add_argument("--epochs", type=int, default=240)
    m.add_argument("--name", required=True)
    m.add_argument("--null", action="store_true", help="no events (the matched control)")
    m.add_argument("--stations", default="S21000SIM", help="comma list or 'all'")
    m.add_argument("--rate-mult", type=float, default=1.0,
                   help="multiple of the campaign rate (6.8 slips / station-hour)")
    m.add_argument("--count-per-station", type=int, default=None,
                   help="fixed count instead of Poisson")
    m.add_argument("--rng", type=int, default=1, help="schedule seed (same seed = same schedule)")
    m.add_argument("--first-ep", type=int, default=10, help="first event no earlier than this")
    m.add_argument("--last-margin", type=int, default=30,
                   help="no event in the last N epochs (every slip gets an aftermath)")
    m.add_argument("--min-elev", type=float, default=15.0)
    m.add_argument("--min-gap-ep", type=int, default=20,
                   help="no two events on the same (station, sat) closer than this")
    m.add_argument("--fixture", default=QUIET_FIXTURE, help="truth dir the seed ran on")
    m.add_argument("--extra", default="", help="JSON overrides merged into leg.json")
    m.add_argument("--from-leg", default="",
                   help="take the schedule from this leg (same snapshot) instead of sampling; "
                        "--stations filters it, --thin keeps every N-th event")
    m.add_argument("--thin", type=int, default=1)
    m.add_argument("--exe", default=event_leg.DEFAULT_EXE)
    m.add_argument("--run", action="store_true")
    m.set_defaults(fn=cmd_make)
    r = sub.add_parser("run", help="run a made leg (first run only)")
    r.add_argument("legdir")
    r.add_argument("--exe", default=event_leg.DEFAULT_EXE)
    r.set_defaults(fn=cmd_run)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
