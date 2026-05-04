#!/usr/bin/env python3
"""migrate_data_layout.py

One-shot migrator from the legacy *flat* GNSS data layout to the
unified *date-first* layout used by ``PathResolver`` and
``ProductResolver`` after the layout-unification refactor.

============================================================
Layouts
============================================================

Legacy flat layout (input)::

    <root>/
      products/
        orbit/{zip,unzip}/         *.SP3, *.SP3.gz
        clock/{zip,unzip}/         *.CLK, *.CLK.gz
        dcb/{zip,unzip}/           *.BIA, *.BIA.gz
        erp/{zip,unzip}/           *.ERP, *.ERP.gz
        brdc/{zip,unzip}/          BRDC*_MN.rnx, .gz
        rinex/{zip,unzip}/         observation files (.rnx, .crx, etc.)
        atx/                       *.atx
        nc/                        *.nc
      observations/
        absolute/                  *.rnx
        baseline/short/<exp>/      per-experiment obs files

Some sites also use a top-level ``orbit/clock/...`` (no ``products/``
intermediate) — the script detects either form.

Date-first layout (output)::

    <root>/
      <YYYY-DOY>/
        products/   *.SP3 *.CLK *.BIA *.ERP *_MN.rnx
        obs/<station_lower>/   *.rnx
      static/
        atx/*.atx
        nc/*.nc
      cache/<YYYY-DOY>/   raw .gz / .crx (production cache; optional)

============================================================
Usage
============================================================

::

    # Migrate the bundled data/ tree (in-place).
    python scripts/migrate_data_layout.py C:/GnssProcessor/data

    # Also fine for the production cache the user keeps elsewhere.
    python scripts/migrate_data_layout.py C:/GnssProcessorSupport/products

    # Preview without touching anything.
    python scripts/migrate_data_layout.py --dry-run C:/GnssProcessor/data_production

The script is idempotent: re-running on an already-migrated root is
a no-op (everything's "already at destination").  Files whose
filename can't be parsed for a YYYYDOY (e.g. README.md) are left
alone with a one-line note.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from datetime import date, timedelta
from pathlib import Path

# ---------------------------------------------------------------------
# Date-extraction helpers (mirror PathResolver::extractYyyyDoy)
# ---------------------------------------------------------------------

# NOTE on the long-name regex: only a non-digit on the LEFT side is
# required, matching PathResolver::extractYyyyDoy.  The trailing
# characters in IGS long-names are typically more digits (the HHMM
# field), so a (?!\d) lookahead would reject every product file.
_RE_LONGNAME_YYYYDOY = re.compile(r"(?<![0-9])(\d{4})(\d{3})")
# RINEX-2 short name station prefix can contain underscores in some
# local fixtures (e.g. "233_3210.23o"); accept any 4-char prefix
# matching the SSSSDDDH.YYO shape.
_RE_RINEX2_SHORTNAME = re.compile(r"^.{4}(\d{3})\d\.(\d{2})[odODnNgG]$")

def _strip_compress_suffix(name: str) -> str:
    """Strip a single compression suffix (.gz / .Z) for regex matching.

    Migration needs to extract dates and stations from raw downloads
    too (e.g. "cit11290.24d.gz") — the regex shapes are easier to
    write against the bare RINEX/IGS filename.
    """
    lower = name.lower()
    if lower.endswith(".gz"):
        return name[:-3]
    if lower.endswith(".z"):
        return name[:-2]
    return name

def extract_yyyydoy(name: str) -> tuple[int, int] | None:
    """Pull (year, doy) out of a RINEX or IGS-product filename.

    Returns ``None`` if no plausible date is encoded.  Mirrors the C++
    ``extractYyyyDoy`` recogniser so migration and runtime agree on
    what counts as a date-keyed file.  A trailing @c .gz / @c .Z is
    stripped before matching so raw cache downloads parse the same as
    their decompressed counterparts.
    """
    bare = _strip_compress_suffix(name)
    # RINEX-2 short name first (YYYY/DOY straddle the dot).
    m = _RE_RINEX2_SHORTNAME.match(bare)
    if m:
        doy = int(m.group(1))
        yy  = int(m.group(2))
        year = 2000 + yy  # pre-2000 fixtures aren't expected
        if 1 <= doy <= 366:
            return year, doy
    # Long-name scan: first 7-digit window that parses as YYYYDOY.
    for m in _RE_LONGNAME_YYYYDOY.finditer(bare):
        year = int(m.group(1))
        doy  = int(m.group(2))
        if 1990 <= year <= 2100 and 1 <= doy <= 366:
            return year, doy
    return None

def date_dir_name(year: int, doy: int) -> str:
    return f"{year:04d}-{doy:03d}"

# ---------------------------------------------------------------------
# Station-name extraction for obs files
# ---------------------------------------------------------------------

_RE_OBS_LONGNAME = re.compile(r"^([A-Za-z0-9]{9})_R_\d{11}_", re.IGNORECASE)
# RINEX-2 short name: SSSSDDDH.YYO, where the SSSS station ID can
# contain underscores in some local fixtures (e.g. "233_3210.23o").
_RE_OBS_SHORTNAME = re.compile(r"^([A-Za-z0-9_]{4})\d{3}\d\.\d{2}[odODnNgG]$",
                               re.IGNORECASE)

def extract_station(name: str) -> str | None:
    """Pull the station identifier (lower-cased) from an obs filename.

    Returns the 9-char IGS long-name (SSSSCCCNNN) when available, the
    4-char short-name station ID otherwise, or @c None when the name
    doesn't match any known RINEX naming convention.  A trailing
    @c .gz / @c .Z is stripped first so raw cache downloads work.
    """
    bare = _strip_compress_suffix(name)
    m = _RE_OBS_LONGNAME.match(bare)
    if m:
        return m.group(1).lower()
    m = _RE_OBS_SHORTNAME.match(bare)
    if m:
        return m.group(1).lower()
    return None

# ---------------------------------------------------------------------
# Move primitives
# ---------------------------------------------------------------------

class Mover:
    def __init__(self, dry_run: bool, root: Path):
        self.dry_run = dry_run
        self.root = root
        self.moved = 0
        self.skipped = 0
        self.untouched = 0

    def _rel(self, p: Path) -> str:
        try:
            return str(p.relative_to(self.root))
        except ValueError:
            return str(p)

    def move(self, src: Path, dst: Path) -> None:
        if not src.exists():
            return
        if dst.exists():
            # Idempotency: leave the destination alone, drop the source
            # if it's a duplicate (same size).  Don't silently overwrite —
            # if sizes differ the user wants to know.
            if src.resolve() == dst.resolve():
                self.untouched += 1
                return
            if dst.stat().st_size == src.stat().st_size:
                if self.dry_run:
                    print(f"  dup-skip  {self._rel(src)}  (already at {self._rel(dst)})")
                else:
                    src.unlink()
                self.skipped += 1
                return
            print(f"  !! conflict: {self._rel(src)} ({src.stat().st_size} B) "
                  f"vs {self._rel(dst)} ({dst.stat().st_size} B); leaving source",
                  file=sys.stderr)
            self.skipped += 1
            return
        action = "would move" if self.dry_run else "move"
        print(f"  {action}  {self._rel(src)}  ->  {self._rel(dst)}")
        if not self.dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(src), str(dst))
        self.moved += 1

# ---------------------------------------------------------------------
# Migration steps
# ---------------------------------------------------------------------

# Subdirs at the legacy layout's top level that are known to hold
# date-keyed product files.  Includes some non-canonical aliases the
# user may have created on the side (e.g. "bia" / "clk" / "sp3" /
# "rnx").  Anything not in this list is left for migrate_observations
# or migrate_static to handle.
_DATE_KEYED_PRODUCT_SUBDIRS = (
    "orbit", "clock", "dcb", "erp", "brdc",
    "sp3", "clk", "bia", "rnx",  # non-canonical aliases
)
_OBS_PRODUCT_SUBDIRS = ("rinex",)

# File extensions that denote decompressed products.  Used to classify
# each file under a legacy product subdir as either "ready" (move to
# <YYYY-DOY>/products/) or "raw" (move to cache/<YYYY-DOY>/).
_PRODUCT_READY_EXT = (".sp3", ".clk", ".bia", ".bsx", ".erp", ".rnx")

def _candidate_product_root(root: Path) -> list[Path]:
    """Return the list of plausible "flat product roots" inside <root>."""
    cands = [root, root / "products"]
    return [p for p in cands if p.is_dir()]

def _classify_product_target(root: Path, entry: Path) -> Path | None:
    """Decide where a legacy product file belongs in the date-first tree.

    Returns the absolute destination path or @c None if the file
    can't be classified (no YYYYDOY in the name).  The classification
    rule:
      - decompressed product extension (@c .SP3, @c .CLK, @c .BIA,
        @c .BSX, @c .ERP, @c .rnx) → @c <root>/<YYYY-DOY>/products/
      - everything else (.gz, .Z, raw .crx) → @c <root>/cache/<YYYY-DOY>/
    """
    name = entry.name
    yd = extract_yyyydoy(name)
    if yd is None:
        return None
    ddir = date_dir_name(*yd)
    name_lower = name.lower()
    is_ready = (
        not (name_lower.endswith(".gz") or name_lower.endswith(".z"))
        and entry.suffix.lower() in _PRODUCT_READY_EXT
    )
    if is_ready:
        return root / ddir / "products" / name
    return root / "cache" / ddir / name

def migrate_products(root: Path, mover: Mover) -> None:
    """Move date-keyed product files into <root>/<YYYY-DOY>/products/
    (decompressed) or <root>/cache/<YYYY-DOY>/ (raw .gz / .crx).

    Recursively walks every legacy product subdir under @c <root> and
    @c <root>/products/ so files in zip/, unzip/, or other ad-hoc
    layouts (@c bia/, @c clk/, @c sp3/, @c rnx/) all get placed
    correctly.  Files seen more than once via overlapping scan roots
    are deduped on resolved-path.
    """
    seen: set[Path] = set()
    for prod_root in _candidate_product_root(root):
        for sub in _DATE_KEYED_PRODUCT_SUBDIRS:
            src_dir = prod_root / sub
            if not src_dir.is_dir():
                continue
            for entry in src_dir.rglob("*"):
                if not entry.is_file():
                    continue
                rp = entry.resolve()
                if rp in seen:
                    continue
                seen.add(rp)
                dst = _classify_product_target(root, entry)
                if dst is None:
                    continue
                mover.move(entry, dst)

def migrate_observations(root: Path, mover: Mover) -> None:
    """Move obs files into <root>/<YYYY-DOY>/obs/<station>/.

    Sources searched (deduped by resolved-path):
      - <root>/observations/**
      - <root>/products/rinex/{zip,unzip}/

    Specifically NOT scanned: <root>/rinex/, where users may keep
    ad-hoc debug subsets (e.g. trimmed 1-hour RINEX for fast
    profiling).  Touching that would break explicit observation_path
    overrides pointing into it.  rglob is used so we pick up nested
    fixture subdirs (@c absolute/, @c baseline/short/...).
    """
    obs_search_roots: list[Path] = []
    if (root / "observations").is_dir():
        obs_search_roots.append(root / "observations")
    # Add any <prod_root>/rinex/ that looks like a canonical product
    # cache (has zip/ or unzip/ subdirs) — that signature distinguishes
    # the bootstrap-produced layout from a user-curated subset like
    # <root>/rinex/debug/ that we must not touch.
    for prod_root in _candidate_product_root(root):
        legacy_rinex = prod_root / "rinex"
        if (legacy_rinex / "zip").is_dir() or (legacy_rinex / "unzip").is_dir():
            obs_search_roots.append(legacy_rinex)

    seen: set[Path] = set()
    for src_root in obs_search_roots:
        for entry in src_root.rglob("*"):
            if not entry.is_file():
                continue
            rp = entry.resolve()
            if rp in seen:
                continue
            seen.add(rp)
            name = entry.name
            ext_lower = entry.suffix.lower()
            if ext_lower in (".md", ".txt"):
                continue
            yd = extract_yyyydoy(name)
            station = extract_station(name)
            if yd is None or station is None:
                continue

            ddir = date_dir_name(*yd)
            name_lower = name.lower()
            if name_lower.endswith(".crx.gz") or name_lower.endswith(".crx") \
                    or name_lower.endswith(".gz") or name_lower.endswith(".z"):
                # Raw / Hatanaka downloads → cache/<YYYY-DOY>/.
                dst = root / "cache" / ddir / name
            else:
                dst = root / ddir / "obs" / station / name
            mover.move(entry, dst)

def migrate_static(root: Path, mover: Mover) -> None:
    """Move atx and ocean-loading files into <root>/static/."""
    for prod_root in _candidate_product_root(root):
        atx_src = prod_root / "atx"
        if atx_src.is_dir():
            for entry in atx_src.iterdir():
                if entry.is_file() and entry.suffix.lower() == ".atx":
                    mover.move(entry, root / "static" / "atx" / entry.name)
        nc_src = prod_root / "nc"
        if nc_src.is_dir():
            for entry in nc_src.iterdir():
                if entry.is_file() and entry.suffix.lower() == ".nc":
                    mover.move(entry, root / "static" / "nc" / entry.name)

def prune_empty(root: Path, mover: Mover) -> None:
    """Remove now-empty legacy directories.  Skipped on dry-run.

    A README.md that lives at the top of @c <root>/products/ or
    @c <root>/observations/ would otherwise keep those legacy roots
    permanently non-empty.  We move such README files (and any other
    @c .md / @c .txt files whose only purpose was documenting the old
    layout) to @c <backup-name>.legacy alongside the now-empty parent
    so the user sees them at the top level — they were docs, not data.
    """
    if mover.dry_run:
        return

    # Step 1: relocate top-of-tree README/.txt notes that are part of
    # the legacy layout's docstring scaffolding.
    for legacy_top in (root / "products", root / "observations"):
        if not legacy_top.is_dir():
            continue
        for entry in legacy_top.iterdir():
            if entry.is_file() and entry.suffix.lower() in (".md", ".txt"):
                # Suffix the destination so multiple legacy roots'
                # README.md don't collide.
                dst = root / f"{legacy_top.name}_README{entry.suffix}.legacy"
                if not dst.exists():
                    print(f"  preserve  {entry.relative_to(root)}  ->  {dst.relative_to(root)}")
                    shutil.move(str(entry), str(dst))
                else:
                    # Already preserved on a prior run; drop the duplicate.
                    entry.unlink()

    # Step 2: rmdir bottom-up.  We collect every directory under the
    # legacy tops (and any flat-root product subdirs that are siblings
    # to the new layout, e.g. data_production/orbit/), sort by depth
    # (deepest first), and rmdir each.  rmdir refuses to delete
    # non-empty dirs, so a hand-curated leftover is silently left
    # alone.
    legacy_dirs: list[Path] = []
    legacy_tops: list[Path] = [root / "products", root / "observations"]
    # Also prune flat-root product subdirs (e.g. <root>/orbit/,
    # <root>/clock/, <root>/atx/, <root>/nc/, <root>/rinex/zip/, ...).
    # We only walk subdirs we know belong to the legacy layout — never
    # the new <YYYY-DOY>/, static/, or cache/ trees.
    for sub in _DATE_KEYED_PRODUCT_SUBDIRS + _OBS_PRODUCT_SUBDIRS + ("atx", "nc"):
        d = root / sub
        if d.is_dir():
            legacy_tops.append(d)
    for legacy_top in legacy_tops:
        if legacy_top.is_dir():
            legacy_dirs.extend(p for p in legacy_top.rglob("*") if p.is_dir())
            legacy_dirs.append(legacy_top)
    legacy_dirs.sort(key=lambda p: -len(p.parts))
    for d in legacy_dirs:
        try:
            d.rmdir()
        except OSError:
            pass

# ---------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("root", type=Path,
                    help="Data root to migrate (in-place).")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would happen without touching the filesystem.")
    args = ap.parse_args(argv)

    root = args.root.resolve()
    if not root.is_dir():
        print(f"!! {root} is not a directory", file=sys.stderr)
        return 2

    print(f"Migrating: {root}")
    if args.dry_run:
        print("(dry-run: no files will be moved)")

    mover = Mover(dry_run=args.dry_run, root=root)
    print("\n[1/3] Date-keyed products")
    migrate_products(root, mover)
    print("\n[2/3] Observation RINEX")
    migrate_observations(root, mover)
    print("\n[3/3] Static assets (atx, nc)")
    migrate_static(root, mover)
    print("\n[cleanup] Pruning empty legacy directories")
    prune_empty(root, mover)

    print()
    print(f"Done.  moved={mover.moved}  skipped={mover.skipped}  "
          f"already-at-dest={mover.untouched}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
