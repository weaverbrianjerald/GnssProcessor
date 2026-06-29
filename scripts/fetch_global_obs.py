#!/usr/bin/env python3
"""Fetch RINEX-3 daily obs for a station list from CDDIS into the data tree.

Reads ``global_network_stations.json`` (from select_global_stations.py), pulls
each station's ``..._01D_30S_MO.crx.gz`` from the CDDIS daily archive (curl +
.netrc + Earthdata cookie jar + redirect-follow — the working CDDIS recipe),
gunzips, and runs ``crx2rnx`` to the final ``.rnx``, placed at
``data/<YYYY-DOY>/obs/<long_lower>/`` so the C++ PathResolver finds it by
station-name match.  Idempotent: an already-present ``.rnx`` is skipped.
"""
from __future__ import annotations

import argparse
import gzip
import json
import shutil
import subprocess
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("stations_json")
    ap.add_argument("--year", type=int, default=2024)
    ap.add_argument("--doy", type=int, default=129)
    ap.add_argument("--rate", default="30S")
    ap.add_argument("--netrc", default=str(Path.home() / ".netrc"))
    ap.add_argument("--cookies", default=str(_REPO / "data" / "cache" / "_cddis_cookies.txt"))
    ap.add_argument("--crx2rnx", default="C:/GnssProcessorSupport/exe/crx2rnx.exe")
    ap.add_argument("--limit", type=int, default=0, help="process only the first N (0=all)")
    args = ap.parse_args()

    stations = json.loads(Path(args.stations_json).read_text())
    if args.limit:
        stations = stations[:args.limit]
    Path(args.cookies).parent.mkdir(parents=True, exist_ok=True)

    datedir = f"{args.year:04d}-{args.doy:03d}"
    yd = f"{args.year:04d}{args.doy:03d}"
    yy = f"{args.year % 100:02d}"
    base = f"https://cddis.nasa.gov/archive/gnss/data/daily/{args.year}/{args.doy:03d}/{yy}d/"

    ok, failed = [], []
    for st in stations:
        long = st["long"]
        obsdir = _REPO / "data" / datedir / "obs" / long.lower()
        rnx = obsdir / f"{long}_R_{yd}0000_01D_{args.rate}_MO.rnx"
        if rnx.exists():
            ok.append(long); print(f"  cached  {long}"); continue
        obsdir.mkdir(parents=True, exist_ok=True)

        crxgz = obsdir / f"{long}_R_{yd}0000_01D_{args.rate}_MO.crx.gz"
        url = base + crxgz.name
        r = subprocess.run(["curl", "-sS", "-L", "--netrc-file", args.netrc,
                            "-c", args.cookies, "-b", args.cookies, "-o", str(crxgz), url])
        if r.returncode != 0 or not crxgz.exists() or crxgz.stat().st_size < 2000:
            failed.append((long, "download")); print(f"  FAIL dl  {long}")
            if crxgz.exists(): crxgz.unlink()
            continue

        crx = obsdir / f"{long}_R_{yd}0000_01D_{args.rate}_MO.crx"
        try:
            with gzip.open(crxgz, "rb") as f, open(crx, "wb") as g:
                shutil.copyfileobj(f, g)
        except Exception as e:
            failed.append((long, f"gunzip:{e}")); print(f"  FAIL gz  {long}"); continue
        crxgz.unlink()

        r2 = subprocess.run([args.crx2rnx, "-f", str(crx)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r2.returncode != 0 or not rnx.exists():
            failed.append((long, "crx2rnx")); print(f"  FAIL crx {long}: {r2.stderr.decode()[:80]}")
            continue
        crx.unlink()
        ok.append(long); print(f"  OK      {long}")

    print(f"\n=== {len(ok)} ok, {len(failed)} failed ===")
    for lf in failed:
        print("  FAILED", lf)
    # Drop the failures from the station list so config generation only uses fetched obs.
    if failed:
        good = [s for s in stations if s["long"] in set(ok)]
        Path(args.stations_json).write_text(json.dumps(good, indent=2))
        print(f"(rewrote {args.stations_json} to the {len(good)} fetched stations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
