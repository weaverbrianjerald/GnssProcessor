#!/usr/bin/env python3
"""Warm start of a network run: resume a configuration at a chosen time from a saved state.

A test of a response to a disturbance otherwise starts from a cold network hours before the
disturbance.  This tool writes (once) a seed run of the configuration that saves the network's
state just before the chosen start, then resumes from that state and runs to the chosen end, and
checks that the start is sound.  The resume mechanics are those of ``scripts/event_leg.py``
(``write-seed`` and ``null``), whose capture and restore keys it arms.  Usage note:
``docs/onboarding/warm_start.md``.

Commands:

``run --base <config> --start HH:MM[:SS] --end HH:MM[:SS] --name <n> --exe <exe> [--gate]``
    1. The seed ``evseed_<n>`` under the runs root: reused when it exists and holds a snapshot
       copy at or before ``start - 30 s``; otherwise written from ``--base`` (every capture key,
       one snapshot copy at the epoch before ``start``, the run continued ``--margin-min``
       minutes past ``start`` so the first resumed epochs can be compared with it) and run.
    2. The leg ``evleg_<n>``: resumed from that copy, every restore key, to ``end``.
    3. ``check`` on the pair.
    Times are times of day (GPS time, as the observation and product files) on the day of the
    configuration's observations.  With ``--gate`` every engine run goes through the machine's
    run gate (``slots.py run --who <who> --priority <p>``).

``check --seed <seed dir> --leg <leg dir>``
    The checks that say the start is sound, each PASS or FAIL with its number:
    C1 the leg exited 0 and resumed warm (no refusal, no cold start);
    C2 every saved section the leg arms was restored and none refused (chain memory, station
       memory, the built-in user's filter);
    C3 at the leg's first epoch, the held links equal the seed's at the same epoch (same
       integers), counted;
    C4 at the leg's first epoch, no topology event the seed does not have (a station or
       satellite seen before the seam reads as present, not as a first sighting);
    C5 over the epochs both runs hold (the seed's margin), the products' between-satellite
       differences against the seed: median and largest per kind (reported; read against the
       bar of card WRS).
"""
import argparse
import csv
import datetime as dt
import json
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)
import event_leg as EL            # noqa: E402
import warm_resume_grade as WG    # noqa: E402

EPOCH_S = EL.EPOCH_INTERVAL_S
SLOTS = "D:/GnssProcessorSupport/gates/run_queue/slots.py"


def _tod_s(text):
    parts = [float(x) for x in text.split(":")]
    while len(parts) < 3:
        parts.append(0.0)
    return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]


def _day_start_s(first_obs_s):
    return first_obs_s - (first_obs_s % 86400.0)


def _run_engine(exe, cfg_path, rundir, gate, who, priority):
    """Run the engine in its run directory (stdout.txt, exitcode.txt), through the gate when asked."""
    runner = os.path.join(_HERE, "warm_start.py")
    if gate:
        shell = f'"{sys.executable}" "{runner}" _engine "{exe}" "{cfg_path}" "{rundir}"'
        cmd = [sys.executable, SLOTS, "run", "--who", who, "--priority", str(priority), "--shell", shell]
        print("GATE:", " ".join(cmd[3:]))
        subprocess.call(cmd)
    else:
        _engine(exe, cfg_path, rundir)
    with open(os.path.join(rundir, "exitcode.txt")) as fh:
        return int(fh.read().strip() or "1")


def _engine(exe, cfg_path, rundir):
    with open(os.path.join(rundir, "stdout.txt"), "w", encoding="utf-8", errors="replace") as out:
        rc = subprocess.call([exe, cfg_path], cwd=rundir, stdout=out, stderr=subprocess.STDOUT)
    with open(os.path.join(rundir, "exitcode.txt"), "w") as fh:
        fh.write(f"{rc}\n")
    return rc


def _snapshot_copies(seed_dir):
    """(n_emitted, week, tow, path) of every history copy in the seed, by emitted epoch."""
    out = []
    for f in os.listdir(seed_dir):
        if f.startswith("state_snapshot.txt.ep"):
            p = os.path.join(seed_dir, f)
            week, tow, n, _fp = EL.read_snapshot_header(p)
            out.append((n, week, tow, p))
    return sorted(out)


def cmd_run(a):
    root = EL.runs_root(a)
    cfg, _ = EL._flatten(a.base)
    first_obs = EL.fixture_first_obs_s(cfg)
    day0 = _day_start_s(first_obs)
    base_off_h = float((cfg.get("network") or {}).get("start_offset_hours", 0.0) or 0.0)
    base_first_s = first_obs + base_off_h * 3600.0 - (EL.HALF_EPOCH_BACKOFF_S if base_off_h else 0.0)
    start_s = day0 + _tod_s(a.start)
    end_s = day0 + _tod_s(a.end)
    if end_s <= start_s:
        raise SystemExit("FATAL: --end must be after --start")
    if start_s - EPOCH_S <= base_first_s:
        raise SystemExit("FATAL: --start must be after the configuration's own first epoch")
    want_tow = (start_s - EPOCH_S) % 604800.0

    seed_dir = os.path.join(root, f"evseed_{a.name}")
    copies = _snapshot_copies(seed_dir) if os.path.isdir(seed_dir) else []
    pick = [c for c in copies if c[2] <= want_tow + 1e-3]
    if not pick or abs(pick[-1][2] - want_tow) > 1e-3:
        # Write the seed: one history copy at the epoch before the start (emitted epoch 1 + n).
        n = int(round((start_s - EPOCH_S - base_first_s) / EPOCH_S))
        hours = (start_s - base_first_s) / 3600.0 + a.margin_min / 60.0
        args = [sys.executable, os.path.join(_HERE, "event_leg.py"), "write-seed", "--name", a.name, "--base", a.base,
                "--hours", f"{hours:.6f}", "--interval-ep", str(n), "--exe", a.exe, "--runs-root", root]
        if a.seed_extra:
            args += ["--extra", a.seed_extra]
        print("SEED:", " ".join(args[2:]))
        if subprocess.call(args) != 0:
            raise SystemExit("FATAL: write-seed failed")
        rc = _run_engine(a.exe, os.path.join(seed_dir, "seed.json"), seed_dir, a.gate, a.who, a.priority)
        if rc != 0:
            raise SystemExit(f"FATAL: the seed run exited {rc}; see {seed_dir}/stdout.txt")
        copies = _snapshot_copies(seed_dir)
        pick = [c for c in copies if c[2] <= want_tow + 1e-3]
    if not pick:
        raise SystemExit(f"FATAL: no snapshot copy at or before TOW {want_tow:.0f} in {seed_dir}")
    n_emit, _w, tow, path = pick[-1]
    if abs(tow - want_tow) > 1e-3:
        print(f"WARNING: the nearest snapshot copy is at TOW {tow:.0f}, not {want_tow:.0f} (an epoch gap in the "
              "seed): the leg starts at the epoch after it")
    epochs = int(round((end_s - (start_s if abs(tow - want_tow) <= 1e-3 else tow + EPOCH_S)) / EPOCH_S))
    args = [sys.executable, os.path.join(_HERE, "event_leg.py"), "null", "--seed-run", seed_dir, "--snapshot-ep",
            str(n_emit), "--epochs", str(epochs), "--name", a.name, "--exe", a.exe, "--runs-root", root]
    # The wide-lane detector's arc statistics do not cross the seam; their cold reference fires en masse at the
    # second resumed epoch, so the leg holds the wide-lane drain diagnostic-only for a few epochs (card WRS).
    extra = json.loads(a.leg_extra) if a.leg_extra else {}
    if a.slip_warmup_ep > 0:
        extra.setdefault("network", {}).setdefault("resume_slip_warmup_ep", a.slip_warmup_ep)
    if extra:
        args += ["--extra", json.dumps(extra)]
    print("LEG:", " ".join(args[2:]))
    if subprocess.call(args) != 0:
        raise SystemExit("FATAL: making the leg failed")
    leg_dir = os.path.join(root, f"evleg_{a.name}")
    rc = _run_engine(a.exe, os.path.join(leg_dir, "leg.json"), leg_dir, a.gate, a.who, a.priority)
    print(f"leg exit code {rc}")
    return check(seed_dir, leg_dir)


def _grep(path, needles):
    hits = {k: [] for k in needles}
    if not os.path.exists(path):
        return hits
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            for k, n in needles.items():
                if n in line:
                    hits[k].append(line.strip())
    return hits


def _events_at(path, tow, tier):
    out = set()
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            if r["tier"] == tier and abs(float(r["tow"]) - tow) < 1e-3:
                out.add((r["event"], r["sat"], r["station"], r["detail"]))
    return out


def check(seed_dir, leg_dir):
    res = []
    ok_all = True

    def verdict(name, ok, text):
        nonlocal ok_all
        ok_all &= ok
        res.append(f"{name} {'PASS' if ok else 'FAIL'}: {text}")

    rc_path = os.path.join(leg_dir, "exitcode.txt")
    rc = int(open(rc_path).read().strip()) if os.path.exists(rc_path) else -1
    out = os.path.join(leg_dir, "stdout.txt")
    h = _grep(out, {
        "refused": "NOT warm-resumable",
        "net_ok": "warm resume restored",
        "user_ok": "RT state snapshot: WARM RESUME applied",
        "cold": "COLD START",
        "chain_ok": "the chain memory section restored at the resume",
        "chain_bad": "chain memory section is REFUSED",
        "chain_none": "carries NO chain memory",
        "stmem_ok": "the station memory section restored at the resume",
        "stmem_bad": "station memory section is REFUSED",
        "stmem_none": "carries NO station memory",
        "user_refused": "staged snapshot DISCARDED",
    })
    verdict("C1", rc == 0 and bool(h["net_ok"]) and not h["refused"] and not h["cold"],
            f"exit code {rc}; warm resume lines {len(h['net_ok'])}; refusal lines {len(h['refused'])}; cold-start "
            f"lines {len(h['cold'])}")
    leg_cfg = json.load(open(os.path.join(leg_dir, "leg.json"), encoding="utf-8"))
    net = leg_cfg.get("network", {})
    parts = []
    good = True
    if net.get("resume_chain_restore"):
        good &= bool(h["chain_ok"]) and not h["chain_bad"] and not h["chain_none"]
        parts.append(f"chain memory restored {len(h['chain_ok'])}, refused {len(h['chain_bad'])}, absent "
                     f"{len(h['chain_none'])}")
    if net.get("resume_station_memory_restore"):
        good &= bool(h["stmem_ok"]) and not h["stmem_bad"] and not h["stmem_none"]
        parts.append(f"station memory restored {len(h['stmem_ok'])}, refused {len(h['stmem_bad'])}, absent "
                     f"{len(h['stmem_none'])}")
    if (leg_cfg.get("realtime") or {}).get("state_snapshot_path"):
        good &= bool(h["user_ok"]) and not h["user_refused"]
        parts.append(f"built-in user restored {len(h['user_ok'])}, refused {len(h['user_refused'])}")
    verdict("C2", good, "; ".join(parts))

    PL = WG.read_products(os.path.join(leg_dir, "products.txt"))
    if not PL:
        verdict("C3", False, "the leg wrote no products")
        print("\n".join(res))
        return 1
    t0 = sorted(PL)[0]
    PU = WG.read_products(os.path.join(seed_dir, "products.txt"), t_min=t0)
    HL = WG.read_held(os.path.join(leg_dir, "logs", "network_ring_fix.csv"), t0).get(t0, {})
    HU = WG.read_held(os.path.join(seed_dir, "logs", "network_ring_fix.csv"), t0).get(t0, {})
    if t0 not in PU:
        res.append(f"C3 n/a: the seed has no epoch at TOW {t0:.0f} (no margin past the snapshot)")
    else:
        same = sum(1 for k, v in HU.items() if k in HL and HL[k][:2] == v[:2])
        other = sum(1 for k, v in HU.items() if k in HL and HL[k][:2] != v[:2])
        u_only = sum(1 for k in HU if k not in HL)
        l_only = sum(1 for k in HL if k not in HU)
        verdict("C3", other == 0 and u_only == 0 and l_only == 0,
                f"held links at TOW {t0:.0f}: seed {len(HU)}, leg {len(HL)}; same integers {same}, other integers "
                f"{other}, seed only {u_only}, leg only {l_only}")
        ev = os.path.join("logs", "network_events.csv")
        el = _events_at(os.path.join(leg_dir, ev), t0, "TOPOLOGY")
        eu = _events_at(os.path.join(seed_dir, ev), t0, "TOPOLOGY")
        verdict("C4", not (el - eu), f"topology events at TOW {t0:.0f}: leg {len(el)}, seed {len(eu)}, in the leg "
                                     f"only {len(el - eu)}")
        common = [t for t in sorted(PL) if t in PU]
        lines = []
        for kind, label in (("C", "clocks"), ("B", "code biases"), ("P", "phase biases")):
            meds, mx = [], 0.0
            for t in common:
                v = []
                for key in set(PL[t]) | set(PU[t]):
                    if key[0] != kind:
                        continue
                    ml, mu = PL[t].get(key, {}), PU[t].get(key, {})
                    sats = [s for s in ml if s in mu]
                    if len(sats) < 2:
                        continue
                    d = [ml[s] - mu[s] for s in sats]
                    m = sorted(d)[len(d) // 2]
                    r = [abs(x - m) for x in d]
                    if kind == "P":
                        lam = WG.LAMBDA.get(key[2])
                        if lam:
                            r = [abs(WG.wrap(x - m, lam)) for x in d]
                    v += r
                if v:
                    meds.append(sorted(v)[len(v) // 2])
                    mx = max(mx, max(v))
            if meds:
                lines.append(f"{label} median {sorted(meds)[len(meds) // 2] * 1e3:.2f} mm, largest {mx * 1e3:.1f} mm")
        res.append(f"C5 (reported) over {len(common)} common epoch(s), between-satellite differences against the seed"
                   f" (phase biases wrapped to the wavelength): " + "; ".join(lines))
    print("\n".join(res))
    print("start SOUND" if ok_all else "start NOT SOUND: see the FAIL lines")
    return 0 if ok_all else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="seed if needed, resume at --start, run to --end, check")
    r.add_argument("--base", required=True, help="the configuration (a reference file or an arm's arm.json)")
    r.add_argument("--start", required=True, help="time of day of the first resumed epoch, HH:MM[:SS]")
    r.add_argument("--end", required=True, help="time of day of the last epoch, HH:MM[:SS]")
    r.add_argument("--name", required=True, help="names evseed_<name> and evleg_<name> under the runs root")
    r.add_argument("--exe", required=True)
    r.add_argument("--margin-min", type=float, default=10.0,
                   help="minutes the seed runs past --start, for the seam checks (default 10)")
    r.add_argument("--seed-extra", default="", help="JSON deep-merged into the seed configuration")
    r.add_argument("--leg-extra", default="", help="JSON deep-merged into the leg configuration")
    r.add_argument("--slip-warmup-ep", type=int, default=5,
                   help="network.resume_slip_warmup_ep of the leg (default 5; 0 leaves it unset)")
    r.add_argument("--runs-root", default=None)
    r.add_argument("--gate", action="store_true", help="run every engine through the machine's run gate")
    r.add_argument("--who", default="warm-start")
    r.add_argument("--priority", type=int, default=5)
    r.set_defaults(func=lambda a: cmd_run(a))
    c = sub.add_parser("check", help="the soundness checks of a leg against its seed")
    c.add_argument("--seed", required=True)
    c.add_argument("--leg", required=True)
    c.set_defaults(func=lambda a: check(a.seed, a.leg))
    e = sub.add_parser("_engine", help=argparse.SUPPRESS)
    e.add_argument("exe")
    e.add_argument("cfg")
    e.add_argument("rundir")
    e.set_defaults(func=lambda a: _engine(a.exe, a.cfg, a.rundir))
    a = ap.parse_args()
    sys.exit(a.func(a) or 0)


if __name__ == "__main__":
    main()
