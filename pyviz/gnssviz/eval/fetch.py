"""Auto-download of station-coordinate truth files for the test suite.

The primary source is the **ITRF2020 IGS combined SSC** from IGN
(`itrf.ign.fr`): a 2.4 MB SINEX-format file with position + velocity for every
IGS station, **no authentication**, so it auto-downloads cleanly for any IGS
station the harness processes.  It is consumed by :class:`SinexTruth`.

Why not CDDIS?  CDDIS (which hosts the IGS *weekly* SINEX) sits behind NASA
Earthdata OAuth; a plain ``urllib`` GET returns the HTML login page, not the
file.  A documented Earthdata-aware path is provided (:func:`cddis_weekly_sinex_url`)
for users who have a ``.netrc`` + cookie flow, but the default truth source is
the no-auth IGN SSC, which is sufficient (it covers every IGS station and
propagates by velocity to any epoch).

The full ITRF2020 SINEX *with* the variance-covariance matrix is 3.3 GB and is
deliberately **not** used -- the SSC carries the position+velocity we need.
"""

from __future__ import annotations

import gzip
import io
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import List, Optional

#: ITRF2020 IGS combined station-coordinate SSC (SINEX format, ~2.4 MB, no auth).
ITRF2020_IGS_SSC_URL = (
    "https://itrf.ign.fr/ftp/pub/itrf/itrf2020/ITRF2020-IGS-TRF.SSC"
)
ITRF2020_IGS_SSC_NAME = "ITRF2020-IGS-TRF.SSC"

#: Extensions the harness treats as SINEX/SSC truth files.
SINEX_GLOBS = ("*.snx", "*.SNX", "*.ssc", "*.SSC")


def find_sinex_files(sinex_dir) -> List[Path]:
    """All SINEX/SSC truth files in ``sinex_dir`` (sorted, de-duplicated)."""
    d = Path(sinex_dir)
    if not d.is_dir():
        return []
    import os
    seen, out = set(), []
    for pat in SINEX_GLOBS:
        for p in sorted(d.glob(pat)):
            # Key on the normalized resolved path so the .snx/.SNX globs don't
            # double-list one file on Windows, yet genuinely case-distinct files
            # survive on POSIX.
            key = os.path.normcase(str(p.resolve()))
            if key not in seen:
                seen.add(key)
                out.append(p)
    return out


def ensure_itrf_ssc(sinex_dir, *, url: str = ITRF2020_IGS_SSC_URL,
                    timeout: int = 180, force: bool = False,
                    quiet: bool = False) -> Optional[Path]:
    """Ensure the ITRF2020 IGS SSC is present in ``sinex_dir``; download if not.

    Idempotent: returns the cached path immediately if the file already exists
    (and ``force`` is False).  Returns ``None`` on any download error (the caller
    then degrades to whatever truth is already on disk -- e.g. SIF).

    Transparently gunzips if the URL serves ``.gz``.
    """
    d = Path(sinex_dir)
    d.mkdir(parents=True, exist_ok=True)
    dest = d / ITRF2020_IGS_SSC_NAME
    if dest.is_file() and dest.stat().st_size > 0 and not force:
        if not quiet:
            print(f"[sinex] cached {dest}")
        return dest
    if not quiet:
        print(f"[sinex] fetching {url}")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            raw = resp.read()
        if url.lower().endswith(".gz") or raw[:2] == b"\x1f\x8b":
            raw = gzip.GzipFile(fileobj=io.BytesIO(raw)).read()
        # An HTTP 200 can still carry an empty body or a captive-portal / error
        # HTML page (CDDIS does exactly this on auth failure).  Writing that as
        # the SSC would silently degrade every truth lookup, so require the
        # SINEX magic before accepting it.
        if not raw or raw.lstrip()[:5] != b"%=SNX":
            print(f"[sinex] rejected non-SINEX response (empty or not %=SNX) "
                  f"from {url}", file=sys.stderr)
            return None
        dest.write_bytes(raw)
        if not quiet:
            print(f"[sinex] saved {dest} ({dest.stat().st_size/1e6:.2f} MB)")
        return dest
    except urllib.error.HTTPError as e:
        print(f"[sinex] HTTP {e.code} for {url}", file=sys.stderr)
    except urllib.error.URLError as e:
        print(f"[sinex] network error: {e.reason} for {url}", file=sys.stderr)
    except Exception as e:  # noqa: BLE001 - best-effort, never fatal
        print(f"[sinex] unexpected error: {e!r}", file=sys.stderr)
    if dest.exists() and dest.stat().st_size == 0:
        dest.unlink()
    return None


def gps_week(year: int, doy: int) -> int:
    """GPS week number for a (year, doy) -- the CDDIS products subdirectory."""
    from datetime import date, timedelta
    d = date(year, 1, 1) + timedelta(days=doy - 1)
    return (d - date(1980, 1, 6)).days // 7


def cddis_products_week_dir(year: int, doy: int) -> str:
    """CDDIS products directory for the GPS week of a date (Earthdata auth req'd).

    Provided for users who want the operational IGS *weekly* SINEX combination
    instead of the cumulative SSC.  The default truth path uses the no-auth IGN
    SSC, because a plain ``urllib`` GET against CDDIS returns the Earthdata login
    page -- fetching from here needs a ``.netrc`` + cookie-jar opener (or the C++
    ``DataDownloader``).  The weekly file is named
    ``IGS0OPSSNX_<yyyyddd>0000_07D_07D_SOL.SNX.gz`` for the week's day-0.
    """
    return f"https://cddis.nasa.gov/archive/gnss/products/{gps_week(year, doy)}/"
