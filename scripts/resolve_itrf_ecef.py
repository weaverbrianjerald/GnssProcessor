#!/usr/bin/env python3
"""Resolve ITRF2020 (IGS SSC) station ECEF coordinates at an observation epoch.

Wraps the test-suite's authoritative ``SinexTruth`` parser
(``pyviz/gnssviz/eval/truth.py``) and prints each station's velocity-propagated
ECEF.  That coordinate is the right value for two uses:

* a network station's held-fixed ``network.stations[].position_ecef``: a wrong
  fixed coordinate biases the estimated satellite clock/bias products directly;
* the truth for scoring a held-out closed-loop consumer.

It is also the ECEF-resolution step for populating the config when scaling the
network to dozens of globally distributed IGS stations.

The grader is not consumed by the C++ engine, so using it to seed config
coordinates keeps the truth independent of the solver.

``SinexTruth`` is loaded standalone (the ``gnssviz`` package ``__init__`` pulls
in pandas, which the resolver itself does not need).

Usage
-----
  python scripts/resolve_itrf_ecef.py JPLM GCGO CIT1            # default date
  python scripts/resolve_itrf_ecef.py JPLM00USA --doy 2024:129 --json
  python scripts/resolve_itrf_ecef.py GCGO --year 2024.350 --compare -2281621.6297 -1453585.1138 5756964.9518
"""
from __future__ import annotations

import argparse
import calendar
import importlib.util
import math
import sys
from pathlib import Path

import numpy as np

_REPO = Path(__file__).resolve().parent.parent
_DEFAULT_SSC = _REPO / "data" / "static" / "sinex" / "ITRF2020-IGS-TRF.SSC"


def load_sinex_truth_cls():
    """Import SinexTruth from truth.py without triggering the gnssviz package."""
    path = _REPO / "pyviz" / "gnssviz" / "eval" / "truth.py"
    spec = importlib.util.spec_from_file_location("snxtruth", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["snxtruth"] = mod          # dataclass needs the module registered
    spec.loader.exec_module(mod)
    return mod.SinexTruth


def decimal_year(year: int, doy: int) -> float:
    ndays = 366 if calendar.isleap(year) else 365
    return year + (doy - 1) / ndays


def ecef_to_enu(d, ref):
    x, y, z = ref
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    a = 6378137.0
    f = 1 / 298.257223563
    e2 = f * (2 - f)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(6):
        s = math.sin(lat)
        n = a / math.sqrt(1 - e2 * s * s)
        lat = math.atan2(z + e2 * n * s, p)
    sl, cl = math.sin(lat), math.cos(lat)
    so, co = math.sin(lon), math.cos(lon)
    dx, dy, dz = d
    return (-so * dx + co * dy,
            -sl * co * dx - sl * so * dy + cl * dz,
            cl * co * dx + cl * so * dy + sl * dz)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stations", nargs="+", help="Station codes (4- or 9-char; only the first 4 chars are used).")
    ap.add_argument("--ssc", default=str(_DEFAULT_SSC), help="ITRF/IGS SSC or SINEX file.")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--year", type=float, help="Decimal year, e.g. 2024.350.")
    g.add_argument("--doy", help="YYYY:DOY, e.g. 2024:129 (default 2024:129).")
    ap.add_argument("--json", action="store_true", help="Emit a position_ecef JSON line per station.")
    ap.add_argument("--compare", nargs=3, type=float, metavar=("X", "Y", "Z"),
                    help="Compare a single station's config ECEF against truth (ENU delta).")
    args = ap.parse_args()

    if args.year is not None:
        dy = args.year
    elif args.doy:
        yy, dd = args.doy.split(":")
        dy = decimal_year(int(yy), int(dd))
    else:
        dy = decimal_year(2024, 129)

    if not Path(args.ssc).exists():
        print(f"ERROR: SSC not found: {args.ssc}\n"
              f"  Fetch it via the test-suite (gnssviz.eval.fetch.ensure_itrf_ssc) "
              f"or from https://itrf.ign.fr/ftp/pub/itrf/itrf2020/", file=sys.stderr)
        return 2

    SinexTruth = load_sinex_truth_cls()
    truth = SinexTruth(args.ssc)

    print(f"# ITRF2020 ECEF @ decimal year {dy:.4f}   (SSC: {Path(args.ssc).name})")
    missing = []
    for stn in args.stations:
        tc = truth.resolve(stn, dy)
        if tc is None:
            missing.append(stn)
            print(f"# {stn[:4]:5}  NOT IN SSC (non-IGS / CORS — needs another source)")
            continue
        x, y, z = (float(v) for v in tc.ecef)
        sig_mm = np.array(tc.sigma) * 1e3
        if args.json:
            print(f'      "position_ecef": [ {x:.4f}, {y:.4f}, {z:.4f} ],   '
                  f'// {stn[:4]} ITRF2020@{dy:.3f} (sigma {sig_mm[0]:.0f}/{sig_mm[1]:.0f}/{sig_mm[2]:.0f} mm)')
        else:
            print(f"{stn[:4]:5}  {x:15.4f} {y:15.4f} {z:15.4f}   "
                  f"sigma(mm) {sig_mm[0]:4.1f}/{sig_mm[1]:4.1f}/{sig_mm[2]:4.1f}")
        if args.compare:
            d = np.array(args.compare) - np.array([x, y, z])
            e, n, u = ecef_to_enu(d, (x, y, z))
            print(f"       config - truth  ENU = E {e:+.3f}  N {n:+.3f}  U {u:+.3f}  m  "
                  f"(3D {np.linalg.norm(d):.3f})")

    if missing:
        print(f"# {len(missing)} station(s) not in the IGS SSC: {', '.join(s[:4] for s in missing)}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
