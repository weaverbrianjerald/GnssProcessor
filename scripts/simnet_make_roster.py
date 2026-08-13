#!/usr/bin/env python3
"""Generate a globally-distributed station roster for a synth-network sim config.

Why a generator: 50 hand-written coordinate triples is precisely where a silent
error lives.  The producer HOLDS station coordinates fixed (CORS geometry), so a
mistyped position is invisible to every downstream gate -- a held coordinate is by
definition not estimated, and nothing in the run can distinguish a 1 km error from
a real one.  Generating them removes the whole failure mode, and makes the
geometry reproducible and reviewable as a rule rather than as a table.

DISTRIBUTION.  A Fibonacci (golden-angle) lattice, which is the standard way to
spread N points near-uniformly on a sphere -- no clustering at the poles the way a
lat/lon grid gives, and no RNG, so the roster is a pure function of N.

Latitude is capped at |lat| <= ~72 deg.  GPS has a 55 deg inclination, so a polar
station sees every satellite at low elevation: it would contribute almost nothing
but noise, and (worse for a fixture) it would make the station-count trend confound
"more stations" with "worse stations".  Real IGS networks are also land-biased and
therefore clustered; even coverage is the cleaner instrument for grading QC tiers,
and it is stated here rather than left as an unexamined property of the fixture.

RECEIVER BIASES vary per station by construction.  The S-basis contract's Table-8
identity resolves the FLEET MEAN of the receiver GF code bias; a roster on which
every station carries the same bias makes the fleet mean indistinguishable from
each member, so the estimator could realize any split between them and still grade
as correct.  Values are a deterministic function of the station index (no RNG, so
the sim's own four RNG streams are untouched and byte-identity is preserved).

STATION NAMES are 9-char IGS long-name ids ending in the pseudo-country "SIM" (no
real 3-char country code is SIM, so a synthetic marker can never collide with a
real site).  Two schemes, because the original one runs out of room:

  legacy2  "S01000SIM"  -- "S" + 2-digit index + "000".  Valid only up to n = 99;
                          at n = 100 it becomes "S100000SIM", TEN characters, which
                          is no longer a RINEX long name at all.
  wide3    "S00101SIM"  -- "S" + 3-digit index + "01".  Valid to n = 999.

`--name-scheme auto` (the default) picks legacy2 at n <= 99 so every existing
roster's station ARRAY regenerates byte-identically (only the `_roster`
provenance string gains the scheme name), and wide3 above it.  The trailing "01"
rather than "00" is deliberate: the per-station RNG salt is derived from the
station NAME, and "S" + ddd + "00" would make wide3 station 300 share its name --
and therefore its entire noise realization -- with legacy2 station 30.  The 6th
character is '0' in every legacy2 name and '1' in every wide3 name, so the two
schemes cannot collide at any index.

Usage:
  python scripts/simnet_make_roster.py <template.json> <out.json> --n 50 [--consumer]
"""
import argparse
import json
import math
import re
import sys


def fibonacci_sphere(n, lat_cap_deg=72.0):
    """n near-uniform points on the sphere, |lat| <= lat_cap_deg, no RNG."""
    golden = math.pi * (3.0 - math.sqrt(5.0))          # golden angle, radians
    s_cap = math.sin(math.radians(lat_cap_deg))
    pts = []
    for i in range(n):
        # Map the index onto sin(lat) inside the cap so the cap does not merely
        # truncate the lattice (which would pile points onto the cap edge).
        s = -s_cap + 2.0 * s_cap * (i + 0.5) / n
        lat = math.degrees(math.asin(s))
        lon = math.degrees((i * golden) % (2.0 * math.pi))
        if lon > 180.0:
            lon -= 360.0
        pts.append((lat, lon))
    return pts


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("template")
    ap.add_argument("out")
    ap.add_argument("--n", type=int, default=50, help="solve stations (default 50)")
    ap.add_argument("--consumer", action="store_true",
                    help="append a held-out consumer station")
    ap.add_argument("--lat-cap", type=float, default=72.0)
    ap.add_argument("--name-scheme", choices=("auto", "legacy2", "wide3"), default="auto",
                    help="9-char marker scheme (see module docstring); auto = legacy2 "
                         "at n <= 99, wide3 above")
    args = ap.parse_args()

    scheme = args.name_scheme
    if scheme == "auto":
        scheme = "legacy2" if args.n <= 99 else "wide3"
    if scheme == "legacy2" and args.n > 99:
        print("ERROR: name-scheme legacy2 overflows 9 characters at n > 99 "
              f"(n = {args.n} would emit 'S{args.n:02d}000SIM'); use wide3.",
              file=sys.stderr)
        return 2
    if args.n > 999:
        print(f"ERROR: n = {args.n} exceeds the 3-digit index (max 999).", file=sys.stderr)
        return 2

    def station_name(i):
        """9-char marker for solve station i (0-based)."""
        return (f"S{i + 1:02d}000SIM" if scheme == "legacy2" else f"S{i + 1:03d}01SIM")

    src = open(args.template, encoding="utf-8").read()
    cfg = json.loads(re.sub(r"^\s*//.*$", "", src, flags=re.M))

    roster = []
    for i, (lat, lon) in enumerate(fibonacci_sphere(args.n, args.lat_cap)):
        # Height: a deterministic spread 0..2000 m so the hydrostatic delay (and
        # therefore the tropo a priori) is not identical across the roster.
        h = 50.0 + 1950.0 * ((i * 37) % 100) / 99.0
        # Receiver code biases: deterministic, distinct per station, non-zero fleet
        # mean.  Two incommensurate periods so C1C and C2W do not co-vary.
        c1 = 0.55 * math.sin(2.0 * math.pi * (i * 7 % 23) / 23.0)
        c2 = 0.48 * math.cos(2.0 * math.pi * (i * 11 % 29) / 29.0)
        # Third-band receiver bias.  INERT on a two-band fixture — the sim applies a
        # receiver bias only to a code it actually emits — so this is byte-safe for
        # every existing roster, and it means a triple-frequency fixture does not
        # silently carry a ZERO receiver bias on the one band whose code bias Odijk
        # says is the only estimable one under a slant ionosphere (Eq. 33).  A third
        # incommensurate period so C5Q co-varies with neither C1C nor C2W.
        c5 = 0.52 * math.sin(2.0 * math.pi * (i * 13 % 31) / 31.0)
        roster.append({
            "name": station_name(i),
            "lat_deg": round(lat, 4), "lon_deg": round(lon, 4), "h_m": round(h, 1),
            "rx_code_bias": [{"code": "C1C", "value_m": round(c1, 4)},
                             {"code": "C2W", "value_m": round(c2, 4)},
                             {"code": "C5Q", "value_m": round(c5, 4)}],
        })

    if args.consumer:
        # Held out of the solve.  Placed at a mid-latitude point offset from every
        # lattice node so it is not co-located with a solve station (a consumer
        # sitting on top of a network station is not an independent test).
        roster.append({
            "name": "SZZ000SIM", "lat_deg": 28.5, "lon_deg": -16.5, "h_m": 600.0,
            "consumer": True,
            "rx_code_bias": [{"code": "C1C", "value_m": -0.37},
                             {"code": "C2W", "value_m": 0.41}],
        })

    cfg["network_stations"] = roster
    cfg["_roster"] = (f"GENERATED by scripts/simnet_make_roster.py --n {args.n}"
                      f"{' --consumer' if args.consumer else ''} "
                      f"(Fibonacci lattice, |lat| <= {args.lat_cap} deg, "
                      f"name-scheme {scheme}). Do not hand-edit — regenerate.")

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
        fh.write("\n")

    lats = [s["lat_deg"] for s in roster]
    lons = [s["lon_deg"] for s in roster]
    print(f"wrote {args.out}: {args.n} solve"
          f"{' + 1 consumer' if args.consumer else ''}, name-scheme {scheme}"
          f" ({roster[0]['name']} .. {roster[args.n - 1]['name']})")
    print(f"  lat {min(lats):+.1f} .. {max(lats):+.1f}   lon {min(lons):+.1f} .. {max(lons):+.1f}")
    gf = [s["rx_code_bias"][0]["value_m"] - s["rx_code_bias"][1]["value_m"] for s in roster]
    print(f"  rx GF bias: fleet mean {sum(gf)/len(gf):+.4f} m, spread {max(gf)-min(gf):.4f} m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
