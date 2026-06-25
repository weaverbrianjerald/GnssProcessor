"""Categorical colour palettes shared by the viewers (matplotlib-free).

Two needs recur across the MATLAB suite:
  * one canonical colour per constellation (stacked counts, DOP lines, sat-status);
  * a large set of visually-distinct colours indexed by PRN (per-satellite line /
    scatter series), the role MATLAB filled with ``distinguishable_colors``.

Kept dependency-light (only ``colorsys`` from the stdlib) so the data layer never
imports a plotting library.
"""

from __future__ import annotations

import colorsys
from typing import Dict, List, Sequence, Tuple

from .identifiers import prn_num

RGB = Tuple[float, float, float]

# Canonical per-constellation colours (match dops / satellite-status viewers).
SYSTEM_COLORS: Dict[str, RGB] = {
    "GPS": (0.15, 0.45, 0.85),       # blue
    "Galileo": (0.20, 0.65, 0.30),   # green
    "GLONASS": (0.85, 0.20, 0.20),   # red
    "BeiDou": (0.55, 0.30, 0.75),    # purple
    "QZSS": (0.95, 0.55, 0.10),      # orange
    "SBAS": (0.50, 0.50, 0.50),      # gray
    "IRNSS": (0.30, 0.70, 0.70),     # teal
    "Unknown": (0.40, 0.40, 0.40),
}

# Canonical render order.
SYSTEM_ORDER: Sequence[str] = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS",
                               "SBAS", "IRNSS")


def system_color(system: str) -> RGB:
    return SYSTEM_COLORS.get(str(system), SYSTEM_COLORS["Unknown"])


def distinguishable_colors(n: int) -> List[RGB]:
    """``n`` visually-distinct RGB colours via a golden-angle HSV walk.

    Deterministic (same ``n`` -> same colours) and well-separated in hue, with
    saturation / value dithered so neighbours differ in more than hue alone.
    """
    out: List[RGB] = []
    h = 0.0
    golden = 0.6180339887498949
    for i in range(max(int(n), 0)):
        h = (h + golden) % 1.0
        s = 0.55 + 0.30 * ((i % 3) / 2.0)
        v = 0.95 - 0.20 * (i % 2)
        out.append(colorsys.hsv_to_rgb(h, s, v))
    return out


def color_by_prn(sats: Sequence[str]) -> Dict[str, RGB]:
    """Stable colour per satellite, indexed by numeric PRN where possible.

    Satellites are sorted by PRN so a given PRN gets a consistent colour within a
    figure; unparseable IDs fall back to appearance order.
    """
    uniq = list(dict.fromkeys(str(s) for s in sats))

    def key(s):
        p = prn_num(s)
        return (0, p) if p is not None else (1, uniq.index(s))
    ordered = sorted(uniq, key=key)
    palette = distinguishable_colors(len(ordered))
    return {s: palette[i] for i, s in enumerate(ordered)}
