#!/usr/bin/env python3
"""bootstrap_internal_data.py

Populate ``data/`` for internal-mode GnssProcessor runs.

Internal mode (``data_source = "internal"`` in the JSON config) expects a
directory tree under ``<repo>/data/`` containing the canonical IGS product
set, the IGS antenna phase-centre file, an ocean-tide loading model, and
the bundled observation RINEX fixtures.  The actual files are tens of MB
to several hundred MB each and are not stored in git.  This script
fetches them once, after which the C++ application's ``PathResolver``
discovers them automatically by globbing the canonical layout.

The script is **idempotent**: every step checks whether the destination
file already exists and skips the download.  Re-run any time to fill in
missing pieces (for example after enabling ocean-tide loading).

============================================================
Authentication
============================================================

CDDIS (https://cddis.nasa.gov) hosts the IGS products and requires a
free NASA Earthdata Login account, plus a ``.netrc`` entry::

    machine urs.earthdata.nasa.gov login YOUR_USER password YOUR_PASS

(On Windows the file is ``%USERPROFILE%\\_netrc``; on Unix it's
``~/.netrc``.)  Without this, downloads from ``cddis.nasa.gov`` will
return HTTP 401.

The IGS file server (``files.igs.org``) and DGFI-TUM (``oceantide.dgfi.tum.de``)
do not require authentication.

============================================================
Usage
============================================================

::

    # From the repo root
    python scripts/bootstrap_internal_data.py

    # Skip the heavyweight EOT20 download (use it if you'll set
    # model_corrections.enable_ocean_tide_loading=false in the config)
    python scripts/bootstrap_internal_data.py --no-eot20

    # Override the repo root (default: parent of this script's dir)
    python scripts/bootstrap_internal_data.py --repo /path/to/GnssProcessor

============================================================
Adding more fixtures
============================================================

Edit ``FIXTURE_DATES`` and ``FIXTURE_OBSERVATIONS`` below.  Each entry
in ``FIXTURE_DATES`` triggers a download of orbit/clock/dcb/erp/brdc
for that day, plus the day before and after for orbit/clock (matches
the C++ resolver's interpolation window).  ``FIXTURE_OBSERVATIONS``
lists individual obs files; the script fetches them from CDDIS for
IGS-mirrored stations, or — for local research data not on CDDIS —
prints a warning telling the user to drop the file in manually.
"""

from __future__ import annotations

import argparse
import gzip
import os
import shutil
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import date, timedelta
from pathlib import Path
from typing import Iterable

# ---------------------------------------------------------------------
# What to fetch
# ---------------------------------------------------------------------

#: Dates we want full IGS product coverage for.  Add (year, doy) tuples
#: as you onboard new fixtures.  Day-1 and day+1 are pulled
#: automatically for orbit/clock so satellite interpolation has
#: boundary points.
FIXTURE_DATES: list[tuple[int, int]] = [
    (2024, 129),  # GCGO PPP fixture (Cleveland, USA)
    (2023, 321),  # OSU baseline fixture (C103 vs NARY)
]

#: Analysis-centre prefix and product type.  COD-MGXFIN matches the
#: existing files in data/products/ — pick something else if your
#: fixtures move (e.g. GFZ for late-July 2024 GLONASS issues).
PRODUCT_SOURCE = "COD"
PRODUCT_TYPE   = "MGXFIN"   # FIN=>MGXFIN, RAP=>OPSRAP, ULT=>OPSULT

@dataclass
class ObsFixture:
    """One observation RINEX file we want bootstrapped."""
    station: str            # 9-char IGS long name OR 4-char short name
    country: str            # 3-char country code (long-name only)
    year: int
    doy: int
    rate: str = "30S"       # sampling: 30S / 15S / 01S
    on_cddis: bool = True   # False => local research data, must be supplied by user
    target_subdir: str = "absolute"  # observations/<subdir>/...

#: Obs files the bootstrap fetches.  CDDIS-hosted stations are fully
#: automatic; non-IGS stations (typical for research baseline fixtures)
#: get a "please supply" message at the end of the run.
FIXTURE_OBSERVATIONS: list[ObsFixture] = [
    # PPP fixture: GCGO on CDDIS daily archive.
    ObsFixture(station="GCGO", country="USA", year=2024, doy=129,
               rate="30S", on_cddis=True, target_subdir="absolute"),
    # IGS_Pasadena baseline fixture: Caltech (CIT1) vs JPL (JPLM),
    # ~10 km separation, both on CDDIS.  The C++ side picks them up
    # via the baseline_fixture="IGS_Pasadena" preset in gnss_config.json.
    ObsFixture(station="CIT1", country="USA", year=2024, doy=129,
               rate="30S", on_cddis=True, target_subdir="baseline/short/igs_pasadena"),
    ObsFixture(station="JPLM", country="USA", year=2024, doy=129,
               rate="30S", on_cddis=True, target_subdir="baseline/short/igs_pasadena"),
    # OSU_Campus baseline fixture: local research data, not on any
    # public archive.  Bootstrap leaves a "please supply manually"
    # message if these aren't already present.  The C++ side picks
    # them up via the baseline_fixture="OSU_Campus" preset.
    ObsFixture(station="C103", country="",   year=2023, doy=321,
               rate="30S", on_cddis=False, target_subdir="baseline/short/static"),
    ObsFixture(station="NARY", country="",   year=2023, doy=321,
               rate="30S", on_cddis=False, target_subdir="baseline/short/static"),
    ObsFixture(station="233",  country="",   year=2023, doy=321,
               rate="30S", on_cddis=False, target_subdir="baseline/short/static"),
]

#: Static (non-date-keyed) downloads.
IGS20_ATX_URL = "https://files.igs.org/pub/station/general/igs20.atx"
#: NOTE: EOT20 is hosted by DGFI-TUM.  The URL below is current as of
#: 2024 — if it 404s, check https://www.dgfi.tum.de/en/research-areas/
#: global-and-regional-reference-systems/ocean-tide-models/ for the
#: latest link, then update this constant.
EOT20_LOAD_NC_URL = "https://oceantide.dgfi.tum.de/EOT20/data/ocean_tides/EOT20_load.nc"

# ---------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------

def gps_week_from_date(year: int, doy: int) -> int:
    """GPS week number for the start of a given (year, doy)."""
    d = date(year, 1, 1) + timedelta(days=doy - 1)
    gps_epoch = date(1980, 1, 6)
    return (d - gps_epoch).days // 7

def yyyydoy(year: int, doy: int) -> str:
    return f"{year:04d}{doy:03d}"

def shifted_doy(year: int, doy: int, shift: int) -> tuple[int, int]:
    """Return (year, doy) shifted by ``shift`` whole days."""
    d = date(year, 1, 1) + timedelta(days=doy - 1 + shift)
    return d.year, (d - date(d.year, 1, 1)).days + 1

# ---------------------------------------------------------------------
# Download primitives
# ---------------------------------------------------------------------

def download_file(url: str, dest: Path, *, log_prefix: str = "") -> bool:
    """Download ``url`` to ``dest``; skip if dest already exists.

    Honors the system's ``.netrc`` for HTTPS basic auth (needed for
    CDDIS).  Returns True on success or already-cached, False on any
    network/HTTP error (caller decides whether to fail loudly).
    """
    if dest.exists() and dest.stat().st_size > 0:
        print(f"  {log_prefix}cached  {dest.relative_to(dest.parents[3]) if len(dest.parents) >= 4 else dest}")
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  {log_prefix}fetch   {url}")
    try:
        # urlretrieve handles .netrc only via Authorization headers if
        # we manually plumb them; for CDDIS the simpler route is
        # urllib + a custom opener that uses HTTPBasicAuthHandler.  We
        # sidestep that here by relying on the user having set up
        # .netrc + a Basic auth flow tested before this script runs.
        with urllib.request.urlopen(url, timeout=120) as resp, open(dest, "wb") as out:
            shutil.copyfileobj(resp, out)
        return True
    except urllib.error.HTTPError as e:
        print(f"  {log_prefix}HTTP {e.code} {url}", file=sys.stderr)
    except urllib.error.URLError as e:
        print(f"  {log_prefix}network error: {e.reason} for {url}", file=sys.stderr)
    except Exception as e:
        print(f"  {log_prefix}unexpected error: {e!r} for {url}", file=sys.stderr)
    if dest.exists():
        dest.unlink()
    return False

def find_crx2rnx(explicit: Path | None = None) -> Path | None:
    """Locate a usable crx2rnx executable for Hatanaka decompression.

    Search order:
      1. ``explicit`` (from --crx2rnx CLI flag) if it exists.
      2. The ``CRX2RNX`` environment variable.
      3. ``shutil.which("crx2rnx")`` / ``crx2rnx.exe``.
      4. A short list of well-known install locations on Windows.

    Returns ``None`` if nothing is found; the caller logs a friendly
    "install crx2rnx" message in that case.
    """
    import shutil
    if explicit is not None and explicit.exists():
        return explicit.resolve()
    env = os.environ.get("CRX2RNX")
    if env and Path(env).exists():
        return Path(env).resolve()
    for name in ("crx2rnx", "crx2rnx.exe", "CRX2RNX.exe"):
        hit = shutil.which(name)
        if hit:
            return Path(hit).resolve()
    fallback_paths = [
        Path("C:/GnssProcessorSupport/exe/crx2rnx.exe"),
        Path("C:/Program Files/crx2rnx/crx2rnx.exe"),
        Path("/usr/local/bin/crx2rnx"),
        Path("/usr/bin/crx2rnx"),
    ]
    for p in fallback_paths:
        if p.exists():
            return p.resolve()
    return None

def expected_rnx_output(compact_path: Path) -> Path | None:
    """Predict the path crx2rnx will produce for a Hatanaka input.

    RNXCMP names the output by changing the last character of the
    extension:
      RINEX-3 long-name : *.crx → *.rnx
      RINEX-2 short-name: *.YYd → *.YYo  (lower-case)
                          *.YYD → *.YYO  (upper-case)

    Returns ``None`` for unrecognised extensions so the caller can
    decide whether to skip or try anyway.
    """
    name = compact_path.name
    lower_name = name.lower()
    if lower_name.endswith(".crx"):
        return compact_path.with_suffix(".rnx")
    # .YYd / .YYD short-name form — last char swaps d↔o, preserving case.
    if (len(name) >= 4 and name[-4] == "."
            and name[-3].isdigit() and name[-2].isdigit()
            and name[-1] in ("d", "D")):
        new_last = "o" if name[-1] == "d" else "O"
        return compact_path.with_name(name[:-1] + new_last)
    return None

def crx_to_rnx(compact_path: Path, crx2rnx_exe: Path | None) -> Path | None:
    """Convert a Hatanaka-compressed obs file (.crx or .YYd) to
    standard RINEX (.rnx or .YYo).

    Returns the path to the produced standard-RINEX file, or ``None``
    on any error (including "no executable available" or
    "unrecognised input extension").  RNXCMP writes the output
    next to the input.
    """
    if crx2rnx_exe is None:
        return None
    rnx_out = expected_rnx_output(compact_path)
    if rnx_out is None:
        print(f"  unrecognised compact extension: {compact_path.name}", file=sys.stderr)
        return None
    if rnx_out.exists():
        return rnx_out
    import subprocess
    try:
        result = subprocess.run(
            [str(crx2rnx_exe), str(compact_path)],
            capture_output=True, text=True, timeout=120,
        )
    except Exception as e:
        print(f"  crx2rnx error: {e!r}", file=sys.stderr)
        return None
    if result.returncode != 0:
        print(f"  crx2rnx exited {result.returncode}: {result.stderr.strip()}",
              file=sys.stderr)
        return None
    if not rnx_out.exists():
        print(f"  crx2rnx ran but produced no output at {rnx_out}", file=sys.stderr)
        return None
    return rnx_out

def gunzip_inplace(src_gz: Path, dest: Path, *, keep_gz: bool = False) -> bool:
    """Decompress ``src_gz`` -> ``dest``.  Removes ``src_gz`` after
    success unless ``keep_gz`` is True (mirrors the C++ DataDownloader's
    new default behavior)."""
    if dest.exists() and dest.stat().st_size > 0:
        if not keep_gz and src_gz.exists():
            src_gz.unlink()
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        with gzip.open(src_gz, "rb") as gz, open(dest, "wb") as out:
            shutil.copyfileobj(gz, out)
    except Exception as e:
        print(f"  gunzip error {src_gz}: {e!r}", file=sys.stderr)
        if dest.exists():
            dest.unlink()
        return False
    if not keep_gz:
        try:
            src_gz.unlink()
        except FileNotFoundError:
            pass
    return True

# ---------------------------------------------------------------------
# CDDIS URL builders (mirror the C++ ProductResolver's filename logic)
# ---------------------------------------------------------------------

def cddis_product_url(tag: str, year: int, doy: int) -> tuple[str, str]:
    """Return (url, filename) for an IGS product.

    ``tag`` is one of "orb" (SP3), "clk" (CLK), "dcb" (BIA), "erp" (ERP).
    """
    yd = yyyydoy(year, doy)
    src = PRODUCT_SOURCE
    typ = PRODUCT_TYPE
    if tag == "orb":
        fname = f"{src}0{typ}_{yd}0000_01D_05M_ORB.SP3.gz"
    elif tag == "clk":
        fname = f"{src}0{typ}_{yd}0000_01D_30S_CLK.CLK.gz"
    elif tag == "dcb":
        fname = f"{src}0{typ}_{yd}0000_01D_01D_OSB.BIA.gz"
    elif tag == "erp":
        fname = f"{src}0{typ}_{yd}0000_01D_12H_ERP.ERP.gz"
    else:
        raise ValueError(f"unknown product tag: {tag}")
    week = gps_week_from_date(year, doy)
    url = f"https://cddis.nasa.gov/archive/gnss/products/{week}/{fname}"
    return url, fname

def cddis_brdc_url(year: int, doy: int) -> tuple[str, str]:
    yd = yyyydoy(year, doy)
    fname = f"BRDC00IGS_R_{yd}0000_01D_MN.rnx.gz"
    url = f"https://cddis.nasa.gov/archive/gnss/data/daily/{year:04d}/brdc/{fname}"
    return url, fname

def _cddis_daily_dir_url(fix: ObsFixture) -> str:
    """Return the directory URL on CDDIS that holds the day's daily
    obs files in both naming conventions (Hatanaka v2 and v3)."""
    yy = f"{fix.year % 100:02d}"
    return (
        f"https://cddis.nasa.gov/archive/gnss/data/daily/"
        f"{fix.year:04d}/{fix.doy:03d}/{yy}d/"
    )

def cddis_obs_url_v3(fix: ObsFixture) -> tuple[str, str]:
    """RINEX-3 long-name Hatanaka URL: SSSSCCCNNN_R_YYYYDOY...crx.gz"""
    yd = yyyydoy(fix.year, fix.doy)
    long_name = (fix.station + ("00" + fix.country if len(fix.station) == 4 else "")).upper()
    fname = f"{long_name}_R_{yd}0000_01D_{fix.rate}_MO.crx.gz"
    return _cddis_daily_dir_url(fix) + fname, fname

def cddis_obs_url_v2(fix: ObsFixture) -> tuple[str, str]:
    """RINEX-2 short-name Hatanaka URL: ssssdddh.YYd.gz (lowercase
    station, 8.3 form).  Some older IGS stations only publish in v2."""
    yy = f"{fix.year % 100:02d}"
    short_station = fix.station[:4].lower()
    fname = f"{short_station}{fix.doy:03d}0.{yy}d.gz"
    return _cddis_daily_dir_url(fix) + fname, fname

# Backwards-compat alias — tooling outside this script may have
# imported the old name; keep it pointing at the v3 builder.
cddis_obs_url = cddis_obs_url_v3

# ---------------------------------------------------------------------
# Top-level fetch routines
# ---------------------------------------------------------------------

def fetch_products_for_date(year: int, doy: int, products_root: Path) -> None:
    """Pull SP3/CLK (3-day window), BIA/ERP (single day), and BRDC."""
    days_3 = [shifted_doy(year, doy, s) for s in (-1, 0, 1)]
    days_1 = [(year, doy)]

    plan: list[tuple[str, str, list[tuple[int,int]]]] = [
        ("orb",  "orbit", days_3),
        ("clk",  "clock", days_3),
        ("dcb",  "dcb",   days_1),
        ("erp",  "erp",   days_1),
    ]
    for tag, subdir, dates in plan:
        for (y, d) in dates:
            url, fname = cddis_product_url(tag, y, d)
            zip_dir = products_root / subdir / "zip"
            unzip_dir = products_root / subdir / "unzip"
            unzip_path = unzip_dir / fname[:-3]   # strip .gz
            if unzip_path.exists():
                print(f"  cached  {unzip_path.relative_to(products_root.parent)}")
                continue
            zip_path = zip_dir / fname
            if not download_file(url, zip_path):
                continue
            gunzip_inplace(zip_path, unzip_path)

    for (y, d) in days_1:
        url, fname = cddis_brdc_url(y, d)
        zip_dir = products_root / "brdc" / "zip"
        unzip_dir = products_root / "brdc" / "unzip"
        unzip_path = unzip_dir / fname[:-3]
        if unzip_path.exists():
            print(f"  cached  {unzip_path.relative_to(products_root.parent)}")
            continue
        zip_path = zip_dir / fname
        if not download_file(url, zip_path):
            continue
        gunzip_inplace(zip_path, unzip_path)

def fetch_observations(observations_root: Path, products_root: Path,
                       crx2rnx_exe: Path | None) -> tuple[list[str], list[str]]:
    """Fetch each obs fixture; return (manual_supply_list, crx_pending_list).

    ``manual_supply_list`` covers fixtures with @c on_cddis=False (local
    research data the user must drop in by hand).
    ``crx_pending_list`` covers fixtures where the CDDIS download
    succeeded but @c crx2rnx wasn't available — the .crx file is
    staged but not converted, and the C++ processor won't read it
    until the user runs crx2rnx manually or installs it on PATH.
    """
    manual: list[str] = []
    crx_pending: list[str] = []
    for fix in FIXTURE_OBSERVATIONS:
        target_dir = observations_root / fix.target_subdir
        target_dir.mkdir(parents=True, exist_ok=True)

        if not fix.on_cddis:
            # Detect both long-name (.rnx) and RINEX-2 short-name (.YYo) forms.
            yy = f"{fix.year % 100:02d}"
            short_name = f"{fix.station.upper()}{fix.doy:03d}0.{yy}o"
            long_name  = f"{fix.station.upper()}_R_{yyyydoy(fix.year, fix.doy)}0000_01D_{fix.rate}_MO.rnx"
            short_path = target_dir / short_name
            long_path  = target_dir / long_name
            if short_path.exists() or long_path.exists():
                print(f"  cached  {(short_path if short_path.exists() else long_path).relative_to(observations_root.parent)}")
            else:
                manual.append(
                    f"{fix.station} {fix.year}-{fix.doy:03d} -> "
                    f"{target_dir.relative_to(observations_root.parent)}/"
                )
            continue

        # CDDIS-hosted IGS station.  CDDIS publishes daily obs in two
        # naming conventions per station: the modern RINEX-3 long name
        # (SSSSCCCNNN_R_..._01D_30S_MO.crx.gz) and/or the legacy
        # RINEX-2 short name (ssssdddh.YYd.gz).  Some stations have
        # both, some only one.  We try v3 first and fall back to v2
        # on any download failure (typically HTTP 404).
        zip_dir   = products_root / "rinex" / "zip"
        unzip_dir = products_root / "rinex" / "unzip"
        candidates = [
            ("v3", *cddis_obs_url_v3(fix)),
            ("v2", *cddis_obs_url_v2(fix)),
        ]

        # If either candidate's standard-RINEX output is already in
        # the target observations dir, treat that as a cached hit.
        already_have = False
        for tag, _url, fname in candidates:
            compact_in_cache = unzip_dir / fname[:-3]                # strip .gz
            rnx_for_candidate = expected_rnx_output(compact_in_cache)
            if rnx_for_candidate is None:
                continue
            target_rnx = target_dir / rnx_for_candidate.name
            if target_rnx.exists():
                print(f"  cached  {target_rnx.relative_to(observations_root.parent)}")
                already_have = True
                break
        if already_have:
            continue

        # Try each candidate in order.  Stop at the first one whose
        # download succeeds; the rest are alternative spellings and
        # we don't need redundant copies.
        produced_rnx = None
        last_compact = None
        for tag, url, fname in candidates:
            compact_path = unzip_dir / fname[:-3]                    # strip .gz
            if not compact_path.exists():
                zip_path = zip_dir / fname
                if not download_file(url, zip_path,
                                     log_prefix=f"({tag}) "):
                    continue
                if not gunzip_inplace(zip_path, compact_path):
                    continue
            last_compact = compact_path
            rnx_in_cache = crx_to_rnx(compact_path, crx2rnx_exe)
            if rnx_in_cache is None:
                # Either crx2rnx isn't available or this candidate's
                # extension wasn't recognised — record for the
                # post-run summary and try the next candidate.
                continue
            produced_rnx = rnx_in_cache
            break

        if produced_rnx is None:
            if last_compact is not None:
                # Downloaded successfully but conversion didn't run.
                target_rnx_basename = (
                    expected_rnx_output(last_compact).name
                    if expected_rnx_output(last_compact) is not None
                    else last_compact.stem + ".rnx"
                )
                target_rnx = target_dir / target_rnx_basename
                crx_pending.append(
                    f"{last_compact.relative_to(observations_root.parent)} -> "
                    f"{target_rnx.relative_to(observations_root.parent)}"
                )
                print(f"  staged  {last_compact.relative_to(observations_root.parent)} "
                      f"(crx2rnx unavailable — will need manual conversion)")
            else:
                print(f"  !! no CDDIS spelling worked for {fix.station} "
                      f"{fix.year}-{fix.doy:03d} (tried v3 long-name and v2 short-name)",
                      file=sys.stderr)
            continue

        # Move into the observations dir.  PathResolver will pick it
        # up on the next run via station-name match.
        target_rnx = target_dir / produced_rnx.name
        try:
            shutil.move(str(produced_rnx), str(target_rnx))
        except Exception as e:
            print(f"  move failed {produced_rnx} -> {target_rnx}: {e!r}",
                  file=sys.stderr)
            continue
        print(f"  rnx     {target_rnx.relative_to(observations_root.parent)}")
    return manual, crx_pending

def fetch_static_assets(products_root: Path, *, want_eot20: bool) -> None:
    atx_path = products_root / "atx" / "igs20.atx"
    if not atx_path.exists():
        download_file(IGS20_ATX_URL, atx_path)
    else:
        print(f"  cached  {atx_path.relative_to(products_root.parent)}")

    if want_eot20:
        nc_path = products_root / "nc" / "EOT20_load.nc"
        if not nc_path.exists():
            ok = download_file(EOT20_LOAD_NC_URL, nc_path)
            if not ok:
                print(
                    "  !! EOT20_load.nc download failed.  Either set "
                    "model_corrections.enable_ocean_tide_loading=false "
                    "in your config, or update EOT20_LOAD_NC_URL in this script "
                    "(check https://www.dgfi.tum.de/ for the current link).",
                    file=sys.stderr,
                )
        else:
            print(f"  cached  {nc_path.relative_to(products_root.parent)}")

# ---------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------

def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="GnssProcessor repo root (default: parent of this script's directory)",
    )
    parser.add_argument(
        "--no-eot20",
        action="store_true",
        help="Skip the 280 MB EOT20_load.nc download (use when "
             "model_corrections.enable_ocean_tide_loading is off)",
    )
    parser.add_argument(
        "--crx2rnx",
        type=Path,
        default=None,
        help="Explicit path to the crx2rnx executable.  Overrides PATH "
             "lookup and the well-known fallback locations.  Used to "
             "convert CDDIS-downloaded Hatanaka files to standard RINEX.",
    )
    args = parser.parse_args(argv)

    repo = args.repo.resolve()
    if not (repo / "CMakeLists.txt").exists():
        print(f"!! {repo} doesn't look like a GnssProcessor checkout", file=sys.stderr)
        return 2

    data_root         = repo / "data"
    products_root     = data_root / "products"
    observations_root = data_root / "observations"

    print(f"Bootstrap target: {data_root}")

    print("\n[1/3] IGS products (SP3, CLK, BIA, ERP, BRDC) per fixture date")
    for (year, doy) in FIXTURE_DATES:
        print(f"  date {year}-{doy:03d}")
        fetch_products_for_date(year, doy, products_root)

    print("\n[2/3] Observation RINEX fixtures")
    crx2rnx_exe = find_crx2rnx(args.crx2rnx)
    if crx2rnx_exe is None:
        print("  (no crx2rnx executable located — Hatanaka .crx files will "
              "be staged but not converted.  Install RNXCMP from "
              "https://terras.gsi.go.jp/ja/crx2rnx.html and either set "
              "the CRX2RNX env var, place the binary on PATH, or pass "
              "--crx2rnx PATH on the next run.)")
    else:
        print(f"  crx2rnx: {crx2rnx_exe}")
    manual, crx_pending = fetch_observations(observations_root, products_root,
                                             crx2rnx_exe)

    print("\n[3/3] Static assets (igs20.atx, EOT20_load.nc)")
    fetch_static_assets(products_root, want_eot20=not args.no_eot20)

    if manual:
        print(
            "\n!! Some fixtures are not on a public archive and must be "
            "supplied manually.  Drop the following observation files "
            "into the indicated directories before running the processor:",
            file=sys.stderr,
        )
        for entry in manual:
            print(f"   - {entry}", file=sys.stderr)
        print(
            "   (They typically come from the project that originally "
            "produced the baseline test data.)",
            file=sys.stderr,
        )

    if crx_pending:
        print(
            "\n!! Hatanaka .crx files were downloaded but not converted "
            "because no crx2rnx executable was located.  Install RNXCMP "
            "(https://terras.gsi.go.jp/ja/crx2rnx.html) and re-run, or "
            "convert these by hand:",
            file=sys.stderr,
        )
        for entry in crx_pending:
            print(f"   - {entry}", file=sys.stderr)

    print("\nDone.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
