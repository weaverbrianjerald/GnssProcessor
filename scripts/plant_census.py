#!/usr/bin/env python3
"""A 16-site in-network consumer census on a planted-coordinate wire (card PLANT).

The sim bed has one held-out consumer (SZZ000SIM, 579 km from the planted station S48000SIM).
To see how a wrong held coordinate spreads to other users, the 16 census stations of
`scripts/clock_level/simcensus6h.py` (S01, S05, ..., S57, S60) are replayed as consumers on a
wire; the paired change of each site between a planted wire and the control wire is the
quantity read (the sites are in the network, so absolute errors are optimistic).

  make  <key> <wire run dir or tag> --base <control arm.json> [--hours 2] [--jobs 3] [--sites s53,s45,...] [--exe <Release exe>]
        tags `pcs_<station>_<key>` under $RAPID_RUNS_ROOT; the replay starts with the wire
        (network.start_offset_hours 0, applied from the wire's first epoch) and runs `hours` hours.
        The base is ALWAYS the control arm, so every replay carries the same configuration and only
        the wire differs.  --sites restricts the replays to a subset of the 16 (three-letter codes,
        for a machine too loaded for 16 replays of 6 h).
  grade <control key> <key> [<key> ...] [--station S48000SIM] [--hours 2]
        per hour: median over the 16 sites of the 3D p50, then the paired change against the
        control key (median, count better / worse, the worst site with its distance from the
        planted station).
"""
import argparse
import csv
import json
import math
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # also under python -I
import rapid_arm  # noqa: E402

PICK = [f"S{i:02d}000SIM" for i in list(range(1, 58, 4)) + [60]]
RUNS = rapid_arm.RUNS


def wire_path(wire):
    d = wire if os.path.isdir(wire) else f"{RUNS}/_rap_{wire}"
    return d.replace("\\", "/") + "/products.txt"


def chosen(sites):
    """The census stations to replay: all 16, or the subset named by three-letter codes."""
    if not sites:
        return PICK
    want = {c.strip().lower() for c in sites.split(",")}
    sub = [n for n in PICK if n[:3].lower() in want]
    assert len(sub) == len(want), ("unknown census site in", sites)
    return sub


def make(a):
    base = json.load(open(a.base))
    pick = chosen(a.sites)
    sl = [s for s in base["network"]["stations"] if s["name"] in pick]
    assert len(sl) == len(pick), len(sl)
    tags = []
    for s in sl:
        tag = f"pcs_{s['name'][:3].lower()}_{a.key}"
        mk = argparse.Namespace(
            base=a.base, tag=tag, hours=a.hours, drop_station=None, rinex=None, allow_unread=None,
            set=[f"network.closed_loop.replay_products_path={wire_path(a.wire)}",
                 "network.closed_loop.pb_hold_presence_flip=true",
                 f"network.closed_loop.station.name={s['name']}",
                 f"network.closed_loop.station.rinex_path={s['rinex_path']}",
                 "network.closed_loop.station.position_ecef=" + json.dumps(s["position_ecef"]),
                 "network.start_offset_hours=0"])
        rapid_arm.cmd_make(mk)
        tags.append(tag)
    rapid_arm.cmd_run(argparse.Namespace(tags=tags, jobs=a.jobs, exe=a.exe))


def hours(tag, nh):
    p = f"{RUNS}/_rap_{tag}/logs/closed_loop_trace.csv"
    if not os.path.exists(p):
        return None
    rows = []
    for r in csv.DictReader(open(p)):
        try:
            rows.append((float(r["tow"]), float(r["pos_err_3d_m"])))
        except (ValueError, TypeError):
            continue
    if not rows:
        return None
    t0 = min(t for t, _ in rows)
    by = {h: [] for h in range(nh)}
    for t, e in rows:
        h = int((t - t0) // 3600.0)
        if h in by:
            by[h].append(e)
    return [st.median(by[h]) if by[h] else float("nan") for h in range(nh)]


def grade(a):
    base = json.load(open(a.base))
    pos = {s["name"]: s["position_ecef"] for s in base["network"]["stations"]}
    pl = pos[a.station]
    pick = chosen(a.sites)
    names = [n[:3].lower() for n in pick]
    dist = {n[:3].lower(): math.dist(pos[n], pl) / 1000.0 for n in pick}
    nh = a.hours
    ref = {n: hours(f"pcs_{n}_{a.ref}", nh) for n in names}
    ok = [n for n in names if ref[n]]
    print(f"{a.ref:12s} p50 by hour (m, median of {len(ok)} sites): "
          + " ".join(f"{st.median([ref[n][h] for n in ok]):.3f}" for h in range(nh)))
    for key in a.keys:
        K = {n: hours(f"pcs_{n}_{key}", nh) for n in names}
        ks = [n for n in names if ref[n] and K[n]]
        line = [f"{key:12s} p50 by hour: " + " ".join(f"{st.median([K[n][h] for n in ks]):.3f}" for h in range(nh))]
        for h in range(nh):
            d = {n: K[n][h] - ref[n][h] for n in ks}
            w = max(d, key=lambda n: abs(d[n]))
            line.append(f"h{h + 1} median change {1000 * st.median(d.values()):+.1f} mm, better {sum(1 for v in d.values() if v < 0)}"
                        f" / worse {sum(1 for v in d.values() if v > 0)}, largest {w.upper()}000SIM {1000 * d[w]:+.1f} mm ({dist[w]:.0f} km)")
        print("\n  ".join(line))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("key")
    m.add_argument("wire")
    m.add_argument("--base", required=True)
    m.add_argument("--hours", type=float, default=2.0)
    m.add_argument("--jobs", type=int, default=3)
    m.add_argument("--exe", required=True)
    m.add_argument("--sites")
    g = sub.add_parser("grade")
    g.add_argument("ref")
    g.add_argument("keys", nargs="+")
    g.add_argument("--base", required=True)
    g.add_argument("--station", default="S48000SIM")
    g.add_argument("--hours", type=int, default=2)
    g.add_argument("--sites")
    a = ap.parse_args()
    make(a) if a.cmd == "make" else grade(a)


if __name__ == "__main__":
    main()
