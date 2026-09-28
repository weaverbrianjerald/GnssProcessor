#!/usr/bin/env python3
"""bootstrap_internal_data.py

Populate ``data/`` for internal-mode GnssProcessor runs.

Internal mode (``data_source = "internal"`` in the JSON config) expects a
directory tree under ``<repo>/data/`` containing the canonical IGS product
set, the IGS antenna phase-centre file, an ocean-tide loading model, the
GPT3 grid, and the bundled observation RINEX fixtures.  The files are tens
of MB to several hundred MB each and are not stored in git.  This script
fetches them once; the C++ application's ``PathResolver`` then discovers
them by globbing the canonical date-first layout.

The script is idempotent: every step checks whether the destination file
already exists and skips the download if so.  Re-running it fills in
missing pieces (for example after enabling ocean-tide loading).

============================================================
Authentication
============================================================

CDDIS (https://cddis.nasa.gov) hosts the IGS products and requires a
free NASA Earthdata Login account, plus a ``.netrc`` entry::

    machine urs.earthdata.nasa.gov login YOUR_USER password YOUR_PASS

(On Windows the file is ``%USERPROFILE%\\_netrc``; on Unix it is
``~/.netrc``.)  Without it, downloads from ``cddis.nasa.gov`` return
HTTP 401.  Note that ``download_file`` below opens URLs with plain
urllib, which does not read ``.netrc`` itself (see its docstring).

The IGS file server (``files.igs.org``) and DGFI-TUM (``oceantide.dgfi.tum.de``)
do not require authentication.

============================================================
Usage
============================================================

::

    # From the repo root
    python scripts/bootstrap_internal_data.py

    # Skip the large EOT20 download (for configs that set
    # model_corrections.enable_ocean_tide_loading=false)
    python scripts/bootstrap_internal_data.py --no-eot20

    # Override the repo root (default: parent of this script's dir)
    python scripts/bootstrap_internal_data.py --repo /path/to/GnssProcessor

============================================================
Adding more fixtures
============================================================

Edit ``FIXTURE_DATES`` and ``FIXTURE_OBSERVATIONS`` below.  Each entry
in ``FIXTURE_DATES`` triggers a download of orbit/clock/dcb/erp/brdc
for that day, plus the day before and after for orbit/clock (the C++
resolver's interpolation window).  ``FIXTURE_OBSERVATIONS`` lists
individual obs files; the script fetches them from CDDIS for
IGS-mirrored stations, and for local research data not on CDDIS it
prints a message at the end asking for the file to be supplied manually.
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

try:
    import hatanaka  # bundled RNXCMP 4.x: gzip + Hatanaka decompression, RINEX 2/3/4
    HAVE_HATANAKA = True
except ImportError:
    hatanaka = None
    HAVE_HATANAKA = False

# ---------------------------------------------------------------------
# What to fetch
# ---------------------------------------------------------------------

#: Dates that get full IGS product coverage.  A new fixture adds its
#: (year, doy) tuple here.  Day-1 and day+1 are pulled automatically
#: for orbit/clock so satellite interpolation has boundary points.
FIXTURE_DATES: list[tuple[int, int]] = [
    (2024, 129),  # GCGO PPP fixture (Cleveland, USA) and IGS_Pasadena baseline
    (2023, 321),  # OSU baseline fixtures (C103, NARY, 233)
]

#: Analysis-centre prefix and product type.  COD-MGXFIN matches the
#: existing files in data/<YYYY-DOY>/products/; other fixtures may need
#: another source (e.g. GFZ for late-July 2024 GLONASS issues).
PRODUCT_SOURCE = "COD"
PRODUCT_TYPE   = "MGXFIN"   # FIN=>MGXFIN, RAP=>OPSRAP, ULT=>OPSULT

@dataclass
class ObsFixture:
    """One observation RINEX file to bootstrap."""
    station: str            # 9-char IGS long name or 4-char short name
    country: str            # 3-char country code (used only to build the long name)
    year: int
    doy: int
    rate: str = "30S"       # sampling: 30S / 15S / 01S
    on_cddis: bool = True   # False => local research data, must be supplied by user

#: Obs files the bootstrap fetches.  CDDIS-hosted stations are fully
#: automatic; non-IGS stations (typical for research baseline fixtures)
#: get a "please supply" message at the end of the run.
#:
#: Files are placed under @c <repo>/data/<YYYY-DOY>/obs/<station_lower>/
#: per the unified date-first layout; the C++ ``PathResolver`` picks
#: them up by station-name match within that subtree.
FIXTURE_OBSERVATIONS: list[ObsFixture] = [
    # PPP fixture: GCGO on CDDIS daily archive.
    ObsFixture(station="GCGO", country="USA", year=2024, doy=129,
               rate="30S", on_cddis=True),
    # IGS_Pasadena baseline fixture: Caltech (CIT1) vs JPL (JPLM),
    # ~10 km separation, both on CDDIS.  The C++ side picks them up
    # via the baseline_fixture="IGS_Pasadena" preset in gnss_config.json.
    ObsFixture(station="CIT1", country="USA", year=2024, doy=129,
               rate="30S", on_cddis=True),
    ObsFixture(station="JPLM", country="USA", year=2024, doy=129,
               rate="30S", on_cddis=True),
    # OSU_Campus baseline fixture: local research data, not on any
    # public archive.  Bootstrap prints a "please supply manually"
    # message if these are not already present.  The C++ side picks
    # them up via the baseline_fixture="OSU_Campus" preset (NARY + 233).
    ObsFixture(station="C103", country="",   year=2023, doy=321,
               rate="30S", on_cddis=False),
    ObsFixture(station="NARY", country="",   year=2023, doy=321,
               rate="30S", on_cddis=False),
    ObsFixture(station="233",  country="",   year=2023, doy=321,
               rate="30S", on_cddis=False),
]

#: Static (non-date-keyed) downloads.
IGS20_ATX_URL = "https://files.igs.org/pub/station/general/igs20.atx"
#: EOT20 is hosted by DGFI-TUM.  The URL below was current in 2024; if
#: it returns 404, the current link is listed at
#: https://www.dgfi.tum.de/en/research-areas/
#: global-and-regional-reference-systems/ocean-tide-models/ and this
#: constant needs updating.
EOT20_LOAD_NC_URL = "https://oceantide.dgfi.tum.de/EOT20/data/ocean_tides/EOT20_load.nc"
#: GPT3 5° empirical pressure/temperature/mapping-function grid
#: (Landskron & Boehm 2018).  Used by the troposphere a priori when
#: ``receiver.troposphere_apriori_source = "gpt3"``.
GPT3_5_GRID_URL = "https://vmf.geo.tuwien.ac.at/codes/gpt3_5.grd"

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
    """Download ``url`` to ``dest``; skip if dest already exists and is non-empty.

    Uses plain ``urllib.request.urlopen`` with no authentication handler;
    urllib does not read ``.netrc``, so a server that requires Basic auth
    (CDDIS) answers with an HTTP error and the call returns False.
    Returns True on success or when already cached, False on any
    network/HTTP error, after removing a partial ``dest`` (the caller
    decides whether to fail).
    """
    if dest.exists() and dest.stat().st_size > 0:
        print(f"  {log_prefix}cached  {dest.relative_to(dest.parents[3]) if len(dest.parents) >= 4 else dest}")
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  {log_prefix}fetch   {url}")
    try:
        # urllib uses .netrc credentials only if they are passed in as
        # Authorization headers or through an opener with
        # HTTPBasicAuthHandler.  Neither is set up here: the request is
        # sent without credentials.
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
      4. A short list of well-known install locations (Windows and Unix).

    Returns ``None`` if nothing is found; the caller then prints an
    install hint.
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
    # .YYd / .YYD short-name form: the last char swaps d↔o, preserving case.
    if (len(name) >= 4 and name[-4] == "."
            and name[-3].isdigit() and name[-2].isdigit()
            and name[-1] in ("d", "D")):
        new_last = "o" if name[-1] == "d" else "O"
        return compact_path.with_name(name[:-1] + new_last)
    return None

def crx_to_rnx(compact_path: Path, crx2rnx_exe: Path | None = None) -> Path | None:
    """Convert a Hatanaka-compressed obs file (.crx or .YYd) to
    standard RINEX (.rnx or .YYo).

    The primary engine is the ``hatanaka`` library (bundled RNXCMP 4.x, which
    handles RINEX 2/3/4; the standalone crx2rnx 4.0.x rejects RINEX 4).  Falls
    back to an external ``crx2rnx`` binary only when hatanaka is unavailable.
    Returns the produced standard-RINEX path, or ``None`` on any error.  The
    output is written next to the input.
    """
    rnx_out = expected_rnx_output(compact_path)
    if rnx_out is None:
        print(f"  unrecognised compact extension: {compact_path.name}", file=sys.stderr)
        return None
    if rnx_out.exists():
        return rnx_out
    if HAVE_HATANAKA:
        try:
            produced = Path(hatanaka.decompress_on_disk(compact_path))
        except Exception as e:
            print(f"  hatanaka decompress error: {e!r}", file=sys.stderr)
            return None
        if produced != rnx_out and produced.exists():
            produced.replace(rnx_out)
        return rnx_out if rnx_out.exists() else None
    # Legacy fallback: external crx2rnx executable (RINEX 2/3 only on 4.0.x).
    if crx2rnx_exe is None:
        return None
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
    success unless ``keep_gz`` is True (as the C++ DataDownloader does by
    default).  An existing non-empty ``dest`` counts as success."""
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
    """RINEX-3 long-name Hatanaka URL: SSSS00CCC_R_YYYYDOY...crx.gz"""
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

# Backwards-compatible alias for tooling outside this script that
# imports the old name; it points at the v3 builder.
cddis_obs_url = cddis_obs_url_v3

# ---------------------------------------------------------------------
# Layout helpers (these must match PathResolver / ProductResolver)
# ---------------------------------------------------------------------

def date_dir_name(year: int, doy: int) -> str:
    """Canonical "YYYY-DOY" date-dir name (matches the C++ side)."""
    return f"{year:04d}-{doy:03d}"

def products_dir(data_root: Path, year: int, doy: int) -> Path:
    return data_root / date_dir_name(year, doy) / "products"

def cache_dir(data_root: Path, year: int, doy: int) -> Path:
    return data_root / "cache" / date_dir_name(year, doy)

def obs_dir(data_root: Path, year: int, doy: int, station_long: str) -> Path:
    return data_root / date_dir_name(year, doy) / "obs" / station_long.lower()

def static_dir(data_root: Path, kind: str) -> Path:
    return data_root / "static" / kind

# ---------------------------------------------------------------------
# Top-level fetch routines
# ---------------------------------------------------------------------

def fetch_products_for_date(year: int, doy: int, data_root: Path) -> None:
    """Pull SP3/CLK (3-day window), BIA/ERP (single day), and BRDC into
    the date-first layout under @c <data_root>/<YYYY-DOY>/products/.

    Each file goes under its own day's directory, so the day before and
    after get their own date dirs for SP3/CLK.  Raw .gz downloads land in
    @c <data_root>/cache/<YYYY-DOY>/ to match @c ProductResolver; the .gz
    is removed by @c gunzip_inplace once the decompressed product is in
    place.
    """
    days_3 = [shifted_doy(year, doy, s) for s in (-1, 0, 1)]
    days_1 = [(year, doy)]

    plan: list[tuple[str, list[tuple[int, int]]]] = [
        ("orb", days_3),
        ("clk", days_3),
        ("dcb", days_1),
        ("erp", days_1),
    ]
    for tag, dates in plan:
        for (y, d) in dates:
            url, fname = cddis_product_url(tag, y, d)
            ready = products_dir(data_root, y, d) / fname[:-3]   # strip .gz
            if ready.exists():
                print(f"  cached  {ready.relative_to(data_root)}")
                continue
            zip_path = cache_dir(data_root, y, d) / fname
            if not download_file(url, zip_path):
                continue
            gunzip_inplace(zip_path, ready)

    for (y, d) in days_1:
        url, fname = cddis_brdc_url(y, d)
        ready = products_dir(data_root, y, d) / fname[:-3]
        if ready.exists():
            print(f"  cached  {ready.relative_to(data_root)}")
            continue
        zip_path = cache_dir(data_root, y, d) / fname
        if not download_file(url, zip_path):
            continue
        gunzip_inplace(zip_path, ready)

def fetch_observations(data_root: Path,
                       crx2rnx_exe: Path | None) -> tuple[list[str], list[str]]:
    """Fetch each obs fixture; return (manual_supply_list, crx_pending_list).

    Files land at @c <data_root>/<YYYY-DOY>/obs/<station_lower>/<file>
    so @c PathResolver finds them by station-name match.

    ``manual_supply_list`` covers fixtures with @c on_cddis=False (local
    research data that has to be copied in by hand).
    ``crx_pending_list`` covers fixtures where the CDDIS download
    succeeded but no conversion produced standard RINEX (no decompressor
    available, or the conversion failed).  The compact file is staged
    but not converted, and the C++ processor does not read it until it
    is converted by hand or a decompressor is installed and the script
    re-run.
    """
    manual: list[str] = []
    crx_pending: list[str] = []
    for fix in FIXTURE_OBSERVATIONS:
        # Long-name station identifier (SSSS00CCC) when on_cddis;
        # otherwise the station name as given (obs_dir lower-cases the
        # directory name; PathResolver matches case-insensitively on
        # substring).
        if fix.on_cddis:
            station_long = (fix.station + ("00" + fix.country
                            if len(fix.station) == 4 else "")).upper()
        else:
            station_long = fix.station.upper()

        target_dir = obs_dir(data_root, fix.year, fix.doy, station_long)
        target_dir.mkdir(parents=True, exist_ok=True)

        if not fix.on_cddis:
            # Detect both long-name (.rnx) and RINEX-2 short-name (.YYo) forms.
            yy = f"{fix.year % 100:02d}"
            short_name = f"{fix.station.upper()}{fix.doy:03d}0.{yy}o"
            long_name  = f"{station_long}_R_{yyyydoy(fix.year, fix.doy)}0000_01D_{fix.rate}_MO.rnx"
            short_path = target_dir / short_name
            long_path  = target_dir / long_name
            if short_path.exists() or long_path.exists():
                hit = short_path if short_path.exists() else long_path
                print(f"  cached  {hit.relative_to(data_root)}")
            else:
                manual.append(
                    f"{fix.station} {fix.year}-{fix.doy:03d} -> "
                    f"{target_dir.relative_to(data_root)}/"
                )
            continue

        # CDDIS-hosted IGS station.  CDDIS publishes daily obs in two
        # naming conventions per station: the modern RINEX-3 long name
        # (SSSS00CCC_R_..._01D_30S_MO.crx.gz) and/or the legacy
        # RINEX-2 short name (ssssdddh.YYd.gz).  Some stations have
        # both, some only one.  v3 is tried first, with a fall back to
        # v2 on any download or conversion failure (typically HTTP 404).
        cdir = cache_dir(data_root, fix.year, fix.doy)
        candidates = [
            ("v3", *cddis_obs_url_v3(fix)),
            ("v2", *cddis_obs_url_v2(fix)),
        ]

        # If either candidate's standard-RINEX output is already in
        # the target observations dir, treat that as a cached hit.
        already_have = False
        for tag, _url, fname in candidates:
            compact_in_cache = cdir / fname[:-3]                # strip .gz
            rnx_for_candidate = expected_rnx_output(compact_in_cache)
            if rnx_for_candidate is None:
                continue
            target_rnx = target_dir / rnx_for_candidate.name
            if target_rnx.exists():
                print(f"  cached  {target_rnx.relative_to(data_root)}")
                already_have = True
                break
        if already_have:
            continue

        # Try each candidate in order and stop at the first one that
        # downloads and converts; the rest are alternative spellings,
        # and redundant copies are not needed.
        produced_rnx = None
        last_compact = None
        for tag, url, fname in candidates:
            compact_path = cdir / fname[:-3]                     # strip .gz
            if not compact_path.exists():
                zip_path = cdir / fname
                if not download_file(url, zip_path,
                                     log_prefix=f"({tag}) "):
                    continue
                if not gunzip_inplace(zip_path, compact_path):
                    continue
            last_compact = compact_path
            rnx_in_cache = crx_to_rnx(compact_path, crx2rnx_exe)
            if rnx_in_cache is None:
                # No decompressor, a failed conversion, or an
                # unrecognised extension: last_compact keeps this file
                # for the post-run summary; try the next candidate.
                continue
            produced_rnx = rnx_in_cache
            break

        if produced_rnx is None:
            if last_compact is not None:
                # Downloaded, but no conversion produced standard RINEX.
                target_rnx_basename = (
                    expected_rnx_output(last_compact).name
                    if expected_rnx_output(last_compact) is not None
                    else last_compact.stem + ".rnx"
                )
                target_rnx = target_dir / target_rnx_basename
                crx_pending.append(
                    f"{last_compact.relative_to(data_root)} -> "
                    f"{target_rnx.relative_to(data_root)}"
                )
                print(f"  staged  {last_compact.relative_to(data_root)} "
                      f"(crx2rnx unavailable — will need manual conversion)")
            else:
                print(f"  !! no CDDIS spelling worked for {fix.station} "
                      f"{fix.year}-{fix.doy:03d} (tried v3 long-name and v2 short-name)",
                      file=sys.stderr)
            continue

        # Move into the per-station obs dir, where PathResolver picks
        # it up on the next run by station-name match.
        target_rnx = target_dir / produced_rnx.name
        try:
            shutil.move(str(produced_rnx), str(target_rnx))
        except Exception as e:
            print(f"  move failed {produced_rnx} -> {target_rnx}: {e!r}",
                  file=sys.stderr)
            continue
        print(f"  rnx     {target_rnx.relative_to(data_root)}")
    return manual, crx_pending

def fetch_static_assets(data_root: Path, *, want_eot20: bool) -> None:
    """Place date-independent assets under @c <data_root>/static/."""
    atx_path = static_dir(data_root, "atx") / "igs20.atx"
    if not atx_path.exists():
        download_file(IGS20_ATX_URL, atx_path)
    else:
        print(f"  cached  {atx_path.relative_to(data_root)}")

    if want_eot20:
        nc_path = static_dir(data_root, "nc") / "EOT20_load.nc"
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
            print(f"  cached  {nc_path.relative_to(data_root)}")

    gpt3_path = static_dir(data_root, "gpt3") / "gpt3_5.grd"
    if not gpt3_path.exists():
        ok = download_file(GPT3_5_GRID_URL, gpt3_path)
        if not ok:
            print(
                "  !! gpt3_5.grd download failed.  Either leave "
                "receiver.troposphere_apriori_source = \"standard_atmosphere\" "
                "(default) or update GPT3_5_GRID_URL in this script "
                "(check https://vmf.geo.tuwien.ac.at/codes/ for the current link).",
                file=sys.stderr,
            )
    else:
        print(f"  cached  {gpt3_path.relative_to(data_root)}")

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

    data_root = repo / "data"

    print(f"Bootstrap target: {data_root} (date-first layout)")

    print("\n[1/3] IGS products (SP3, CLK, BIA, ERP, BRDC) per fixture date")
    for (year, doy) in FIXTURE_DATES:
        print(f"  date {year}-{doy:03d}")
        fetch_products_for_date(year, doy, data_root)

    print("\n[2/3] Observation RINEX fixtures")
    crx2rnx_exe = find_crx2rnx(args.crx2rnx)
    if HAVE_HATANAKA:
        print(f"  decompressor: hatanaka (bundled RNXCMP {hatanaka.rnxcmp_version})")
    elif crx2rnx_exe is None:
        print("  (no decompressor located — Hatanaka .crx files will be staged "
              "but not converted.  Install the hatanaka package "
              "(pip install hatanaka) — it bundles a RINEX-4-capable RNXCMP — "
              "or place a recent crx2rnx on PATH and re-run.)")
    else:
        print(f"  crx2rnx: {crx2rnx_exe} (legacy fallback — prefer 'pip install hatanaka' for RINEX 4)")
    manual, crx_pending = fetch_observations(data_root, crx2rnx_exe)

    print("\n[3/3] Static assets (igs20.atx, EOT20_load.nc, gpt3_5.grd)")
    fetch_static_assets(data_root, want_eot20=not args.no_eot20)

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
