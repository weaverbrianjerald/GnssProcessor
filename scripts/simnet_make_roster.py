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
import hashlib
import json
import math
import re
import statistics
import sys


# --------------------------------------------------------------------------- #
# The §29.5 EMPIRICAL receiver-GF-bias model
# --------------------------------------------------------------------------- #
# docs/SPEC_level_information_budget.md §29.5 measured the FIELD's receiver
# C1C-C2W geometry-free hardware biases over the 61-station ext63 roster:
#
#     network MEAN     ~ -5 ns  ~ -1.55 m of GF code  ~ -15 slant TECU
#     spread           std ~ 21 slant TECU, CONTINENT-SCALE CLUSTERED
#                      (Europe uniformly ~ -22, southern outliers +40..+47)
#
# That non-zero mean is the whole C4 confound of the LV-6 census card: the
# rx_code_bias_datum zero-MEAN convention cannot hold it, so it lands in the
# level.  The `legacy` roster model below is zero-mean by construction (measured
# fleet mean ~ -0.05 m, full spread ~ 1 m ~ 10 TECU), so a fixture built on it
# CANNOT grade the confound -- which is why SPEC_level_authority_internal.md
# §4 G1 / §5 item 5 require a REGENERATED fixture carrying this distribution.
#
# Structure, and why it is written this way: the model is a latitude gradient
# (the "southern outliers" of §29.5) plus a continent-scale REGIONAL bloc term
# (12 cells: 3 latitude bands x 4 longitude sectors) plus per-station jitter,
# then AFFINE-PINNED so the fleet mean and population sd hit the measured
# targets EXACTLY.  Every draw is a deterministic hash of a station/region name
# -- no RNG state, so the roster stays a pure function of its arguments and the
# sim's own four RNG streams are untouched.
F1_HZ, F2_HZ = 1575.42e6, 1227.60e6
MU12 = (F1_HZ / F2_HZ) ** 2                          # 1.646944 = (f1/f2)^2
K_L1_M_PER_TECU = 40.3e16 / F1_HZ ** 2               # 0.162377 m of L1 delay per TECU
GF_M_PER_TECU = abs(1.0 - MU12) * K_L1_M_PER_TECU    # 0.105049 m of (C1C-C2W) per slant TECU
F29P5_MEAN_M = -1.55                                 # §29.5 network mean, metres of C1C-C2W
F29P5_STD_TECU = 21.0                                # §29.5 clustered spread, slant TECU


def _u01(tag):
    """Deterministic uniform(0,1) from a string: no RNG state, no seed to lose."""
    h = hashlib.blake2b(tag.encode("utf-8"), digest_size=8).digest()
    return (int.from_bytes(h, "big") + 0.5) / 2.0 ** 64


def _norm(tag):
    """Deterministic standard normal draw keyed by `tag`."""
    return statistics.NormalDist().inv_cdf(_u01(tag))


def _region(lat, lon):
    """Continent-scale cell id: 3 latitude bands x 4 longitude sectors."""
    band = 0 if lat >= 20.0 else (1 if lat > -20.0 else 2)
    sector = int((lon + 180.0) // 90.0) % 4
    return band * 4 + sector


def f29p5_gf(points, names, bloc_tecu, bloc_sd_tecu, outlier_frac,
             outlier_lo_tecu, outlier_hi_tecu):
    """Per-station GF code bias g = b(C1C) - b(C2W), §29.5-shaped, in METRES.

    The shape is taken literally from the measurement, not from a Gaussian fit:
    a dominant continental BLOC near `bloc_tecu` (§29.5: "Europe uniformly
    ~ -22") plus a small POSITIVE SOUTHERN TAIL at +40..+47.  That skew is
    load-bearing for LV-6 G1 -- the gate grades the census MEDIAN, and
    SPEC_level_authority_internal §5 item 5 says in as many words that the
    reference statistic is the median "not the coverage-weighted mean -- median
    vs mean skew".  A symmetric draw pinned to the same mean and sd would make
    that distinction untestable, so the bloc/tail anchors are held and the
    resulting mean/sd are REPORTED rather than pinned.

    Bloc dispersion is split between a continent-scale REGIONAL term (12 cells:
    3 latitude bands x 4 longitude sectors) and per-station jitter, in
    quadrature.  Every draw is a deterministic hash of a region/station name --
    no RNG state, so the roster stays a pure function of its arguments.
    """
    n = len(names)
    n_out = max(1, int(round(outlier_frac * n))) if outlier_frac > 0.0 else 0
    # The tail is SOUTHERN (§29.5 "southern outliers"), so it is the n_out
    # lowest-latitude stations -- a geographic rule, not a draw.
    south_idx = set(sorted(range(n), key=lambda i: (points[i][0], names[i]))[:n_out])
    sd_reg = bloc_sd_tecu * math.sqrt(0.6)      # regional / station split, in quadrature
    sd_sta = bloc_sd_tecu * math.sqrt(0.4)
    out = []
    for i, ((lat, lon), nm) in enumerate(zip(points, names)):
        if i in south_idx:
            u = _u01("f29p5/outlier/" + nm)
            t = outlier_lo_tecu + (outlier_hi_tecu - outlier_lo_tecu) * u
        else:
            t = (bloc_tecu
                 + sd_reg * _norm("f29p5/region/%d" % _region(lat, lon))
                 + sd_sta * _norm("f29p5/station/" + nm))
        out.append(t * GF_M_PER_TECU)
    return out


def f29p5_split(g, mode):
    """Split a GF bias g into (b_C1C, b_C2W) metres.

    `gf_pure` (default) puts the ENTIRE injected bias in the geometry-free
    direction: the ionosphere-free lump a1*b1 + a2*b2 (a1 = mu/(mu-1),
    a2 = -1/(mu-1) -- the same IF_A1/IF_A2 scripts/simnet_grade.py uses) is
    identically ZERO, so the receiver-CLOCK truth channel is untouched and every
    other graded channel keeps its meaning.  `half` splits +-g/2 (smaller
    absolute codes, but it moves the rx-clock channel by 2.046*g).
    """
    if mode == "half":
        return 0.5 * g, -0.5 * g
    return -g / (MU12 - 1.0), -g * MU12 / (MU12 - 1.0)


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
    ap.add_argument("--rx-bias-model", choices=("legacy", "f29p5"), default="legacy",
                    help="receiver code-bias distribution.  legacy (default) = the "
                         "deterministic sin/cos draw every existing roster carries "
                         "(fleet-mean GF ~ 0, spread ~ 1 m) -- byte-identical.  f29p5 = "
                         "the §29.5 EMPIRICAL field distribution (mean -1.55 m of "
                         "C1C-C2W, sd 21 slant TECU, continent-clustered, positive "
                         "southern tail), which is what LV-6 G1 requires "
                         "(SPEC_level_authority_internal SS4/§5 item 5)")
    ap.add_argument("--rx-bias-bloc-tecu", type=float, default=-22.0,
                    help="f29p5 continental bloc level, slant TECU (default -22 = "
                         "§29.5's 'Europe uniformly ~ -22')")
    ap.add_argument("--rx-bias-bloc-sd-tecu", type=float, default=6.0,
                    help="f29p5 within-bloc dispersion, slant TECU (default 8.4: with "
                         "the default tail this reproduces §29.5's sd ~ 21 over the "
                         "whole roster; measured 22.3 at n = 50)")
    ap.add_argument("--rx-bias-outlier-frac", type=float, default=0.12,
                    help="fraction of the roster placed in the positive SOUTHERN tail "
                         "(default 0.12; with the default anchors this reproduces "
                         "§29.5's fleet mean ~ -15 TECU / -1.55 m -- measured "
                         "-15.3 TECU / -1.61 m at n = 50)")
    ap.add_argument("--rx-bias-outlier-tecu", type=float, nargs=2, default=(40.0, 47.0),
                    metavar=("LO", "HI"),
                    help="f29p5 southern-outlier band, slant TECU (default 40 47 = §29.5)")
    ap.add_argument("--rx-bias-split", choices=("gf_pure", "half"), default="gf_pure",
                    help="how the GF bias is split across C1C/C2W; gf_pure (default) "
                         "leaves the ionosphere-free lump identically zero")
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

    pts = fibonacci_sphere(args.n, args.lat_cap)
    names = [station_name(i) for i in range(args.n)]
    # §29.5 model (opt-in).  Computed for the WHOLE roster at once because the
    # affine pin is a fleet statistic -- a per-station formula cannot hit a fleet
    # mean, and the fleet mean is the entire point of the model.
    gf_f29p5 = None
    if args.rx_bias_model == "f29p5":
        gf_f29p5 = f29p5_gf(pts, names, args.rx_bias_bloc_tecu,
                            args.rx_bias_bloc_sd_tecu, args.rx_bias_outlier_frac,
                            args.rx_bias_outlier_tecu[0], args.rx_bias_outlier_tecu[1])

    roster = []
    for i, (lat, lon) in enumerate(pts):
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
        codes = [{"code": "C1C", "value_m": round(c1, 4)},
                 {"code": "C2W", "value_m": round(c2, 4)},
                 {"code": "C5Q", "value_m": round(c5, 4)}]
        if gf_f29p5 is not None:
            b1, b2 = f29p5_split(gf_f29p5[i], args.rx_bias_split)
            # L1/L2 GF only: §29.5 measured a C1C-C2W statistic and says nothing
            # about band 5, so no C5Q row is emitted rather than inventing one.
            # A triple-band fixture on this model needs the model extended first.
            codes = [{"code": "C1C", "value_m": round(b1, 6)},
                     {"code": "C2W", "value_m": round(b2, 6)}]
        roster.append({
            "name": names[i],
            "lat_deg": round(lat, 4), "lon_deg": round(lon, 4), "h_m": round(h, 1),
            "rx_code_bias": codes,
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
        if gf_f29p5 is not None:
            # The consumer is HELD OUT of the solve, so it contributes nothing to
            # the fleet mean the pin fixes; it still needs a bias from the same
            # population or the closed-loop station is the one clean receiver in a
            # biased world.  Keyed off its own name, pinned to the fleet mean + 1 sd
            # of the solve roster is wrong (it would be a fleet member); use the
            # model's own draw at its coordinates instead.
            g_c = f29p5_gf([(28.5, -16.5)], ["SZZ000SIM"], args.rx_bias_bloc_tecu,
                            args.rx_bias_bloc_sd_tecu, 0.0, 0.0, 0.0)[0]
            b1, b2 = f29p5_split(g_c, args.rx_bias_split)
            roster[-1]["rx_code_bias"] = [{"code": "C1C", "value_m": round(b1, 6)},
                                          {"code": "C2W", "value_m": round(b2, 6)}]

    cfg["network_stations"] = roster
    cfg["_roster"] = (f"GENERATED by scripts/simnet_make_roster.py --n {args.n}"
                      f"{' --consumer' if args.consumer else ''} "
                      f"(Fibonacci lattice, |lat| <= {args.lat_cap} deg, "
                      f"name-scheme {scheme}, rx-bias-model {args.rx_bias_model}"
                      + (f" bloc {args.rx_bias_bloc_tecu} +- {args.rx_bias_bloc_sd_tecu} TECU,"
                         f" tail {args.rx_bias_outlier_frac} at"
                         f" {args.rx_bias_outlier_tecu[0]}..{args.rx_bias_outlier_tecu[1]} TECU,"
                         f" split {args.rx_bias_split}"
                         if args.rx_bias_model == "f29p5" else "")
                      + "). Do not hand-edit — regenerate.")

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
    if args.rx_bias_model == "f29p5":
        solve_gf = [st["rx_code_bias"][0]["value_m"] - st["rx_code_bias"][1]["value_m"]
                    for st in roster if not st.get("consumer")]
        t = [v / GF_M_PER_TECU for v in solve_gf]
        m = sum(t) / len(t)
        med = statistics.median(t)
        print(f"  f29p5 SOLVE roster (n={len(t)}): mean {m:+.2f} TECU "
              f"({m * GF_M_PER_TECU:+.4f} m), MEDIAN {med:+.2f} TECU "
              f"({med * GF_M_PER_TECU:+.4f} m), sd {statistics.pstdev(t):.2f} TECU, "
              f"min {min(t):+.1f} / max {max(t):+.1f} TECU")
        print(f"       §29.5 targets: mean {F29P5_MEAN_M / GF_M_PER_TECU:+.2f} TECU "
              f"({F29P5_MEAN_M:+.4f} m), sd {F29P5_STD_TECU:.2f} TECU, "
              f"bloc ~ -22, southern outliers +40..+47")
        south = sorted(((st["lat_deg"],
                         (st["rx_code_bias"][0]["value_m"]
                          - st["rx_code_bias"][1]["value_m"]) / GF_M_PER_TECU)
                        for st in roster if not st.get("consumer")))[:4]
        print("       most-southern 4 (lat, TECU): "
              + ", ".join(f"({a:+.1f}, {b:+.1f})" for a, b in south))
    return 0


if __name__ == "__main__":
    sys.exit(main())
