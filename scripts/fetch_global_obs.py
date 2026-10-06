#!/usr/bin/env python3
"""Fetch RINEX-3 daily obs for a station list from CDDIS into the data tree.

Reads ``global_network_stations.json`` (from select_global_stations.py), pulls
each station's ``..._01D_30S_MO.crx.gz`` from the CDDIS daily archive over curl
with .netrc authentication, an Earthdata cookie jar, and redirect-follow, then
decompresses it to the final ``.rnx``, placed at
``data/<YYYY-DOY>/obs/<long_lower>/`` so the C++ PathResolver finds it by
station-name match.  Idempotent: an already-present ``.rnx`` is skipped.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

try:
    import hatanaka  # bundled RNXCMP 4.x: gzip + Hatanaka decompression, RINEX 2/3/4
except ImportError:
    sys.exit("fetch_global_obs.py requires the 'hatanaka' package (pip install hatanaka)")

_REPO = Path(__file__).resolve().parent.parent


def is_rinex(path: Path) -> bool:
    """True when the file's first line is a RINEX version/type header record."""
    with open(path, "rb") as f:
        return b"RINEX VERSION / TYPE" in f.readline()


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
    ap.add_argument("--root", default=str(_REPO / "data"),
                    help="data root the <YYYY-DOY>/obs tree is written under (default: the repo's data/)")
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
        obsdir = Path(args.root) / datedir / "obs" / long.lower()
        rnx = obsdir / f"{long}_R_{yd}0000_01D_{args.rate}_MO.rnx"
        if rnx.exists() and is_rinex(rnx):
            ok.append(long); print(f"  cached  {long}"); continue
        if rnx.exists():
            # A cached file that is not RINEX is an archive error page kept by an older
            # version of this script; set it aside and fetch again.
            rnx.replace(rnx.with_name(rnx.name + ".not_rinex.html"))
        obsdir.mkdir(parents=True, exist_ok=True)

        crxgz = obsdir / f"{long}_R_{yd}0000_01D_{args.rate}_MO.crx.gz"
        url = base + crxgz.name
        r = subprocess.run(["curl", "-sS", "-f", "-L", "--netrc-file", args.netrc,
                            "-c", args.cookies, "-b", args.cookies, "-o", str(crxgz), url])
        if r.returncode != 0 or not crxgz.exists() or crxgz.stat().st_size < 2000:
            failed.append((long, "download")); print(f"  FAIL dl  {long}")
            if crxgz.exists(): crxgz.unlink()
            continue

        # hatanaka handles gunzip + Hatanaka decompression in one call and
        # supports RINEX 2/3/4 (the old crx2rnx 4.0.x choked on RINEX 4).
        try:
            hatanaka.decompress_on_disk(crxgz, delete=True)   # .crx.gz -> .rnx
        except Exception as e:
            failed.append((long, f"decompress:{e}")); print(f"  FAIL cvt {long}: {str(e)[:80]}")
            if crxgz.exists(): crxgz.unlink()
            continue
        if not rnx.exists():
            failed.append((long, "decompress")); print(f"  FAIL cvt {long}: no .rnx produced"); continue
        if not is_rinex(rnx):
            # The archive answers a missing file with an HTML page, which the decompressor
            # passes through unchanged; it must never be left behind under a .rnx name.
            rnx.replace(rnx.with_name(rnx.name + ".not_rinex.html"))
            failed.append((long, "not RINEX")); print(f"  FAIL     {long}: the archive returned a non-RINEX page")
            continue
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
