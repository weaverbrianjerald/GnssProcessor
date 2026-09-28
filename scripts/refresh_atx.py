#!/usr/bin/env python
"""Keep the IGS ANTEX (igs20.atx) antenna model up to date.

The IGS ``igs20.atx`` at files.igs.org is a single, continuously-updated file:
new satellites get phase-center calibrations as they are commissioned, PRN<->SVN
reassignments are tracked, and occasional PCO/PV corrections land. A stale ATX
silently mis-models recently-changed satellites (wrong block, or a fallback to a
neighbouring frequency). This script refreshes the local copy.

It is the single source of truth for "is my ATX current": run it by hand, wire it
into a scheduled task, or let the engine invoke it at run start (config toggle
``data_download.atx_auto_refresh``). Design points:

  * Conditional fetch: sends ``If-Modified-Since`` from the local file's mtime,
    so a 304 means "already current" and nothing downloads.
  * Validated, atomic, reversible replace: the download lands in a temp file,
    is parse-checked as real ANTEX, the previous file is backed up, then the temp
    is atomically renamed into place. A truncated/HTML-error download never
    clobbers a good ATX.
  * Best-effort: any network/validation failure leaves the existing file intact
    and exits non-zero; the engine treats that as non-fatal and proceeds with the
    local copy.
  * Provenance: reports the ATX's own embedded ``week NNNN`` changelog marker
    (more reliable than file mtime, which a copy/paste bumps without changing
    content) before and after.

Exit codes: 0 = current or successfully updated; 2 = update available
(``--check-only``); 1 = error (network/validation/IO).

Examples
--------
    python refresh_atx.py --check-only
    python refresh_atx.py --atx C:/GnssProcessorSupport/products/atx/igs20.atx
    python refresh_atx.py --max-age-days 7        # skip the network if <7 days old
    python refresh_atx.py --force
"""
from __future__ import annotations

import argparse
import email.utils
import os
import pathlib
import re
import sys
import tempfile
import time
import urllib.error
import urllib.request

IGS20_ATX_URL = "https://files.igs.org/pub/station/general/igs20.atx"
GPS_EPOCH = 315964800  # 1980-01-06 00:00:00 UTC, unix seconds
DEFAULT_ATX = pathlib.Path(__file__).resolve().parent.parent / "data" / "static" / "atx" / "igs20.atx"
_UA = {"User-Agent": "GnssProcessor-refresh_atx/1.0"}
_WEEK_RE = re.compile(rb"\bweek\s+(\d{3,4})\b")


def gps_week_to_date(week: int) -> str:
    return time.strftime("%Y-%m-%d", time.gmtime(GPS_EPOCH + week * 7 * 86400))


def embedded_week(data: bytes) -> int | None:
    """Highest ``week NNNN`` marker among ANTEX COMMENT lines (its changelog)."""
    weeks = [int(m.group(1)) for line in data.splitlines()
             if b"COMMENT" in line[60:] for m in _WEEK_RE.finditer(line)]
    return max(weeks) if weeks else None


def looks_like_antex(data: bytes) -> str | None:
    """Return an error string if ``data`` is not a plausible ANTEX file, else None."""
    if len(data) < 10 * 1024 * 1024:
        return f"suspiciously small ({len(data)} bytes; expected tens of MB)"
    head = data[:200]
    if b"ANTEX VERSION" not in head:
        return "first line is not 'ANTEX VERSION / SYST' (got an error page?)"
    # The header COMMENT/changelog block (all the 'week NNNN' lines) can run for
    # many KB before the first antenna, so scan the whole payload, not a window.
    if b"START OF ANTENNA" not in data:
        return "no 'START OF ANTENNA' block found (got an error page?)"
    return None


def http_date(ts: float) -> str:
    return email.utils.formatdate(ts, usegmt=True)


def server_last_modified(url: str, timeout: int) -> float | None:
    """HEAD the URL and return the server Last-Modified as unix seconds (or None)."""
    req = urllib.request.Request(url, method="HEAD", headers=_UA)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        lm = resp.headers.get("Last-Modified")
    if not lm:
        return None
    parsed = email.utils.parsedate_to_datetime(lm)
    return parsed.timestamp()


def report_local(atx: pathlib.Path) -> int | None:
    if not atx.exists():
        print(f"  local   {atx} : MISSING")
        return None
    data = atx.read_bytes()
    wk = embedded_week(data)
    age_days = (time.time() - atx.stat().st_mtime) / 86400.0
    wk_str = f"week {wk} (~{gps_week_to_date(wk)})" if wk else "week unknown"
    print(f"  local   {atx}")
    print(f"          {len(data):,} bytes | changelog tops out at {wk_str} | file mtime {age_days:.0f} d ago")
    return wk


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--atx", type=pathlib.Path, default=DEFAULT_ATX,
                    help=f"local ATX path to refresh (default: {DEFAULT_ATX})")
    ap.add_argument("--url", default=IGS20_ATX_URL, help="source URL")
    ap.add_argument("--check-only", action="store_true",
                    help="report whether an update is available; do not download/replace")
    ap.add_argument("--force", action="store_true",
                    help="download and replace even if the server reports not-modified")
    ap.add_argument("--max-age-days", type=float, default=0.0,
                    help="if the local file is younger than this, skip the network entirely "
                         "(for the per-run engine hook; 0 = always check)")
    ap.add_argument("--timeout", type=int, default=120, help="HTTP timeout (s)")
    ap.add_argument("--quiet", action="store_true", help="only warn/error output")
    args = ap.parse_args(argv)

    def say(*a):
        if not args.quiet:
            print(*a)

    atx: pathlib.Path = args.atx
    say(f"=== refresh_atx: {args.url} ===")
    local_wk = report_local(atx)

    # Throttle: the engine passes --max-age-days so it does not hit the network
    # every single run. A freshly-written file is assumed current.
    if args.max_age_days > 0 and atx.exists():
        age_days = (time.time() - atx.stat().st_mtime) / 86400.0
        if age_days < args.max_age_days and not args.force:
            say(f"  skip    local file is {age_days:.1f} d old (< --max-age-days "
                f"{args.max_age_days}); not checking server")
            return 0

    # --- check-only: cheap HEAD, compare Last-Modified to local mtime ----------
    if args.check_only:
        try:
            slm = server_last_modified(args.url, args.timeout)
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as e:
            print(f"  ERROR   could not reach server: {e}", file=sys.stderr)
            return 1
        if slm is None:
            say("  server  Last-Modified not advertised; cannot compare cheaply "
                "(run without --check-only to do a conditional GET)")
            return 0
        say(f"  server  Last-Modified {http_date(slm)}")
        if atx.exists() and slm <= atx.stat().st_mtime + 1:
            say("  result  UP TO DATE (server not newer than local)")
            return 0
        say("  result  UPDATE AVAILABLE (server is newer than local)")
        return 2

    # --- refresh: conditional GET, validate, backup, atomic replace ------------
    headers = dict(_UA)
    if atx.exists() and not args.force:
        headers["If-Modified-Since"] = http_date(atx.stat().st_mtime)
    req = urllib.request.Request(args.url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            data = resp.read()
    except urllib.error.HTTPError as e:
        if e.code == 304:
            say("  result  UP TO DATE (304 Not Modified)")
            return 0
        print(f"  ERROR   HTTP {e.code} fetching {args.url}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError) as e:
        print(f"  ERROR   network error: {getattr(e, 'reason', e)} (kept local file)",
              file=sys.stderr)
        return 1

    bad = looks_like_antex(data)
    if bad:
        print(f"  ERROR   download rejected: {bad} (kept local file)", file=sys.stderr)
        return 1

    new_wk = embedded_week(data)
    if local_wk is not None and new_wk is not None and new_wk < local_wk:
        print(f"  WARN    server changelog (week {new_wk}) is OLDER than local "
              f"(week {local_wk}); refusing to downgrade (use --force to override)",
              file=sys.stderr)
        if not args.force:
            return 1

    atx.parent.mkdir(parents=True, exist_ok=True)
    # Atomic, reversible replace.
    fd, tmp_name = tempfile.mkstemp(dir=str(atx.parent), prefix=".igs20.", suffix=".tmp")
    tmp = pathlib.Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        if atx.exists():
            backup = atx.with_suffix(atx.suffix + f".week{local_wk or 'NA'}.bak")
            try:
                if backup.exists():
                    backup.unlink()
                os.replace(atx, backup)
                say(f"  backup  previous -> {backup.name}")
            except OSError as e:
                print(f"  WARN    could not back up previous ATX: {e}", file=sys.stderr)
        os.replace(tmp, atx)
    except OSError as e:
        print(f"  ERROR   could not install new ATX: {e}", file=sys.stderr)
        if tmp.exists():
            tmp.unlink()
        return 1

    wk_str = f"week {new_wk} (~{gps_week_to_date(new_wk)})" if new_wk else "week unknown"
    delta = (f" [{local_wk} -> {new_wk}]" if (local_wk and new_wk and new_wk != local_wk)
             else "")
    say(f"  result  UPDATED -> {atx} | now at {wk_str}{delta} | {len(data):,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
