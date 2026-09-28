#!/usr/bin/env python3
"""fetch_sinex_subset.py: freeze a small station subset out of a SINEX/SSC.

IGS / ITRF SINEX coordinate products are tens of MB (every global station). The
test suite only needs a handful of stations, so this script copies just the
``SITE/ID``, ``SOLUTION/EPOCHS`` and ``SOLUTION/ESTIMATE`` rows for the requested
4-character codes into a small ``.snx`` suitable for committing under
``data/static/sinex/``.

``--source`` is a local path (``.snx`` or ``.snx.gz``) or an ``http(s)`` URL.
Network access is only used when a URL is given; the default workflow is to
download the big SSC once by hand, then subset it locally.

Example
-------
::

    python scripts/fetch_sinex_subset.py \\
        --source C:/dl/ITRF2020-IGS-TRF.SNX \\
        --codes JPLM CIT1 GCGO P389 \\
        --out data/static/sinex/igs20_subset.snx
"""

from __future__ import annotations

import argparse
import gzip
import io
import sys
from pathlib import Path


def _read_source(source: str) -> str:
    if source.lower().startswith(("http://", "https://")):
        import urllib.request
        with urllib.request.urlopen(source) as r:        # nosec - user-supplied
            raw = r.read()
    else:
        raw = Path(source).read_bytes()
    if source.lower().endswith(".gz") or raw[:2] == b"\x1f\x8b":
        raw = gzip.GzipFile(fileobj=io.BytesIO(raw)).read()
    return raw.decode("utf-8", errors="replace")


# Blocks kept verbatim (only the rows whose station code is requested).
_CODE_BLOCKS = ("SITE/ID", "SOLUTION/EPOCHS", "SOLUTION/ESTIMATE")


def _row_code(line: str, block: str) -> str | None:
    """Extract the 4-char station code from a data row of ``block``."""
    toks = line.split()
    if not toks:
        return None
    if block == "SOLUTION/ESTIMATE":
        # INDEX TYPE CODE PT SOLN ... -> code is token 2 (0-based).
        return toks[2].upper() if len(toks) > 2 else None
    # SITE/ID and SOLUTION/EPOCHS lead with the CODE token.
    return toks[0].upper()


def subset(text: str, codes: set[str]) -> str:
    out: list[str] = []
    block = None
    for line in text.splitlines():
        if line.startswith("%") or line.startswith("*"):
            out.append(line)            # header / file comments kept
            continue
        if line.startswith("+"):
            block = line[1:].strip()
            out.append(line)
            continue
        if line.startswith("-"):
            out.append(line)
            block = None
            continue
        if block in _CODE_BLOCKS:
            code = _row_code(line, block)
            if code is not None and code in codes:
                out.append(line)
            # non-matching data rows in code blocks are dropped
        else:
            out.append(line)            # other blocks copied as-is
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--source", required=True,
                    help="Local .snx/.snx.gz path or http(s) URL of the full SINEX/SSC.")
    ap.add_argument("--codes", nargs="+", required=True,
                    help="4-char station codes to keep (e.g. JPLM CIT1 GCGO).")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)

    codes = {c.upper()[:4] for c in args.codes}
    text = _read_source(args.source)
    sub = subset(text, codes)

    # Sanity: report which requested codes actually appear in the output.
    found = {c for c in codes if any(
        c in ln.split()[:3] for ln in sub.splitlines() if ln[:1] not in "+-%*" and ln.split())}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(sub, encoding="utf-8")
    print(f"wrote {args.out}  ({len(sub.splitlines())} lines)")
    missing = codes - found
    if missing:
        print(f"WARNING: requested codes not found in source: {sorted(missing)}",
              file=sys.stderr)
    print(f"kept codes: {sorted(found)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
