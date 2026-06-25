"""Satellite / signal / state-key identity helpers shared by the viewers.

Pure string/number utilities (no plotting, no pandas dependency beyond what the
caller passes).  Centralises the constellation-letter mapping and the
``<SAT>_<SIGNAL>`` state-key split that several MATLAB viewers re-derive inline.
"""

from __future__ import annotations

from typing import Optional, Tuple

# Leading RINEX constellation letter -> canonical system name.
_SYS_BY_LETTER = {
    "G": "GPS", "E": "Galileo", "R": "GLONASS",
    "C": "BeiDou", "J": "QZSS", "S": "SBAS", "I": "IRNSS",
}


def system_of(prn: str) -> str:
    """Map a satellite ID (e.g. ``'G03'``) to its constellation name."""
    if prn:
        return _SYS_BY_LETTER.get(str(prn)[0].upper(), "Unknown")
    return "Unknown"


def prn_num(sat: str) -> Optional[int]:
    """Numeric PRN from chars 2-3 of a satellite ID; ``None`` if unparseable."""
    try:
        return int(str(sat)[1:3])
    except (ValueError, IndexError):
        return None


def parse_key(key: str) -> Tuple[str, str]:
    """Split an ambiguity / bias state key ``'<SAT>_<SIGNAL>'`` -> (sat, signal).

    Returns ``(key, '')`` if there is no separator.
    """
    s = str(key)
    if "_" in s:
        sat, sig = s.split("_", 1)
        return sat, sig
    return s, ""
