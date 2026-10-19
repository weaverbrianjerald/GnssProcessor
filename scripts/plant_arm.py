#!/usr/bin/env python3
"""Plant a known error in ONE network station's held coordinate (card PLANT).

A network station's `position_ecef` is the coordinate the producer holds.  The RINEX
file and the simulator's truth are left as they are, so the planted offset is exactly a
wrong held coordinate: `held = true + offset`, the offset given in the local East /
North / Up frame at the station (the same WGS84 frame the estimator's own ENU uses).

  make --base <golden.json> --tag <name> --station S48000SIM --east 1.0 [--north 0] [--up 0]
       [--hours 2] [--set key.path=value ...]
  make --base <golden.json> --tag <name> --control          (no offset: the same-exe control)

`make` builds the arm through `rapid_arm.py make` (run paths, hours, --set keys checked
against the loader) and then edits the one array element that rapid_arm cannot set.  It
refuses to finish unless the arm differs from the base in nothing but that station's
`position_ecef` and the run paths, and records the planted vector in `rapid.json`.

The arm directory is `$RAPID_RUNS_ROOT/_rap_<tag>` (rapid_arm's convention), so
`rapid_arm.py run --tags <tag>` and `rapid_arm.py score <tag>` work unchanged.
"""
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # also under python -I
import rapid_arm  # noqa: E402  (RUNS is read from RAPID_RUNS_ROOT at import)

# keys under which an arm legitimately differs from its base: the run paths and length
RUN_PATH_KEYS = ("network.product_output_path", "network.max_duration_s", "processing.max_duration_s")


def leaves(node, prefix=""):
    """Flatten a JSON tree to {dotted path: value}; list elements are indexed."""
    out = {}
    if isinstance(node, dict):
        for k, v in node.items():
            out.update(leaves(v, f"{prefix}.{k}" if prefix else k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            out.update(leaves(v, f"{prefix}[{i}]"))
    else:
        out[prefix] = node
    return out


def structural_diff(base, arm):
    """Paths whose value differs, appears or vanishes between two configs."""
    a, b = leaves(base), leaves(arm)
    return sorted(k for k in set(a) | set(b) if a.get(k, "<absent>") != b.get(k, "<absent>"))


def planted_position(true_ecef, east, north, up):
    """true + offset, the offset converted from local ENU at the station."""
    lat, lon, _ = rapid_arm.ecef_to_llh(*true_ecef)
    d = rapid_arm.enu_to_ecef((east, north, up), lat, lon)
    return [round(true_ecef[i] + d[i], 6) for i in range(3)]


def cmd_make(a):
    base = json.load(open(a.base))
    control = bool(a.control)
    east, north, up = (0.0, 0.0, 0.0) if control else (a.east, a.north, a.up)
    mk = argparse.Namespace(base=a.base, tag=a.tag, hours=a.hours, set=a.set or [], drop_station=None,
                            rinex=None, allow_unread=None)
    rapid_arm.cmd_make(mk)
    rd = f"{rapid_arm.RUNS}/_rap_{a.tag}"
    arm = json.load(open(rd + "/arm.json"))
    stations = arm["network"]["stations"]
    idx = [i for i, s in enumerate(stations) if s["name"] == a.station]
    assert len(idx) == 1, f"station {a.station} is not in the roster exactly once"
    node = stations[idx[0]]
    true_ecef = list(node["position_ecef"])
    held = true_ecef if control else planted_position(true_ecef, east, north, up)
    node["position_ecef"] = held
    json.dump(arm, open(rd + "/arm.json", "w"), indent=2)

    # the arm differs from its base in the station's position and the run paths only
    dif = structural_diff(base, arm)
    pos_paths = {f"network.stations[{idx[0]}].position_ecef[{k}]" for k in range(3)}
    allowed = pos_paths | set(RUN_PATH_KEYS) | {kv.split("=", 1)[0] for kv in (a.set or [])}
    stray = [p for p in dif if p not in allowed]
    assert not stray, f"arm differs from the base outside the plant and run paths: {stray}"
    moved = [p for p in dif if p in pos_paths]
    assert control == (not moved), "the control must not move the coordinate, an arm must"
    prov = json.load(open(rd + "/rapid.json"))
    prov["plant"] = {"station": a.station, "control": control, "east_m": east, "north_m": north, "up_m": up,
                     "true_ecef": true_ecef, "held_ecef": held,
                     "offset_ecef": [round(held[i] - true_ecef[i], 6) for i in range(3)],
                     "differs_from_base": dif}
    json.dump(prov, open(rd + "/rapid.json", "w"), indent=2)
    print(f"{a.tag}: {a.station} " + ("control (held = true)" if control else
          f"held = true + (E {east:g}, N {north:g}, U {up:g}) m; |offset| {math.dist(held, true_ecef):.6f} m")
          + f"; paths differing from the base: {len(dif)} ({', '.join(dif)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--base", required=True)
    m.add_argument("--tag", required=True)
    m.add_argument("--station", required=True)
    m.add_argument("--east", type=float, default=0.0)
    m.add_argument("--north", type=float, default=0.0)
    m.add_argument("--up", type=float, default=0.0)
    m.add_argument("--control", action="store_true")
    m.add_argument("--hours", type=float)
    m.add_argument("--set", action="append")
    a = ap.parse_args()
    assert a.cmd == "make"
    assert a.control or (a.east or a.north or a.up), "give an offset (--east / --north / --up) or --control"
    cmd_make(a)


if __name__ == "__main__":
    main()
