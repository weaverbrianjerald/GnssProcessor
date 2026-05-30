#!/usr/bin/env python3
"""Standalone Python port of vmf3.m used to generate reference values
for the C++ Vmf3 unit test.

Reads coefficients directly from the generated header
include/atmosphere/Vmf3Coefficients.hpp (so the C++ test is verified
against the exact same constants its release build will see), and
evaluates VMF3 for a fixed (lat, lon, doy, el, a_h, a_w) tuple.

Usage:
    python scripts/vmf3_reference.py
"""

from __future__ import annotations
import math
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "include" / "atmosphere" / "Vmf3Coefficients.hpp"

NAMES = ["anm_bh", "anm_bw", "anm_ch", "anm_cw",
         "bnm_bh", "bnm_bw", "bnm_ch", "bnm_cw"]


def load_arrays() -> dict[str, list[list[float]]]:
    text = HEADER.read_text(encoding="utf-8")
    arrs: dict[str, list[list[float]]] = {}
    for name in NAMES:
        # Match the constexpr block: `constexpr double NAME[91][5] = { ... };`
        m = re.search(
            rf"constexpr\s+double\s+{name}\s*\[\d+\]\s*\[\d+\]\s*=\s*\{{(.*?)\}};",
            text, re.DOTALL)
        if not m:
            raise RuntimeError(f"{name}: not found in {HEADER}")
        body = m.group(1)
        # Each row is `{ a, b, c, d, e }` possibly followed by a `// n=..., m=...` comment.
        rows = re.findall(r"\{([^}]+)\}", body)
        if len(rows) != 91:
            raise RuntimeError(f"{name}: expected 91 rows, got {len(rows)}")
        parsed: list[list[float]] = []
        for r in rows:
            vals = [float(v) for v in r.split(",")]
            if len(vals) != 5:
                raise RuntimeError(f"{name}: expected 5 cols, got {len(vals)}")
            parsed.append(vals)
        arrs[name] = parsed
    return arrs


def vmf3(lat: float, lon: float, doy: float, el: float, ah: float, aw: float,
         arrs: dict[str, list[list[float]]]) -> tuple[float, float]:
    """Port of vmf3.m with the Legendre+SH+seasonal+Marini chain."""
    NMAX = 12
    polDist = math.pi / 2 - lat
    x = math.sin(polDist) * math.cos(lon)
    y = math.sin(polDist) * math.sin(lon)
    z = math.cos(polDist)

    # MATLAB-style (1-based) indexing using lists of lists.
    DIM = NMAX + 2
    V = [[0.0] * DIM for _ in range(DIM)]
    W = [[0.0] * DIM for _ in range(DIM)]
    V[1][1] = 1.0
    V[2][1] = z * V[1][1]
    for n in range(2, NMAX + 1):
        V[n + 1][1] = ((2 * n - 1) * z * V[n][1] - (n - 1) * V[n - 1][1]) / n
    for m in range(1, NMAX + 1):
        V[m + 1][m + 1] = (2 * m - 1) * (x * V[m][m] - y * W[m][m])
        W[m + 1][m + 1] = (2 * m - 1) * (x * W[m][m] + y * V[m][m])
        if m < NMAX:
            V[m + 2][m + 1] = (2 * m + 1) * z * V[m + 1][m + 1]
            W[m + 2][m + 1] = (2 * m + 1) * z * W[m + 1][m + 1]
        for n in range(m + 2, NMAX + 1):
            V[n + 1][m + 1] = ((2 * n - 1) * z * V[n][m + 1] - (n + m - 1) * V[n - 1][m + 1]) / (n - m)
            W[n + 1][m + 1] = ((2 * n - 1) * z * W[n][m + 1] - (n + m - 1) * W[n - 1][m + 1]) / (n - m)

    bh = [0.0] * 5
    bw = [0.0] * 5
    ch = [0.0] * 5
    cw = [0.0] * 5
    idx = 0
    for n in range(NMAX + 1):
        for m in range(n + 1):
            Vnm = V[n + 1][m + 1]
            Wnm = W[n + 1][m + 1]
            for k in range(5):
                bh[k] += arrs["anm_bh"][idx][k] * Vnm + arrs["bnm_bh"][idx][k] * Wnm
                bw[k] += arrs["anm_bw"][idx][k] * Vnm + arrs["bnm_bw"][idx][k] * Wnm
                ch[k] += arrs["anm_ch"][idx][k] * Vnm + arrs["bnm_ch"][idx][k] * Wnm
                cw[k] += arrs["anm_cw"][idx][k] * Vnm + arrs["bnm_cw"][idx][k] * Wnm
            idx += 1

    omega = 2 * math.pi * doy / 365.25
    c1 = math.cos(omega); s1 = math.sin(omega)
    c2 = math.cos(2 * omega); s2 = math.sin(2 * omega)
    bh_v = bh[0] + bh[1] * c1 + bh[2] * s1 + bh[3] * c2 + bh[4] * s2
    bw_v = bw[0] + bw[1] * c1 + bw[2] * s1 + bw[3] * c2 + bw[4] * s2
    ch_v = ch[0] + ch[1] * c1 + ch[2] * s1 + ch[3] * c2 + ch[4] * s2
    cw_v = cw[0] + cw[1] * c1 + cw[2] * s1 + cw[3] * c2 + cw[4] * s2

    se = math.sin(el)
    mfh = (1 + ah / (1 + bh_v / (1 + ch_v))) / (se + ah / (se + bh_v / (se + ch_v)))
    mfw = (1 + aw / (1 + bw_v / (1 + cw_v))) / (se + aw / (se + bw_v / (se + cw_v)))
    return mfh, mfw


def main() -> int:
    arrs = load_arrays()
    # Test inputs — bake these into the C++ unit test.
    lat = 0.5            # rad (~28.6°N)
    lon = 1.0            # rad (~57.3°E)
    doy = 100.0
    el = math.radians(30.0)
    ah = 0.00125
    aw = 0.00060

    mfh, mfw = vmf3(lat, lon, doy, el, ah, aw, arrs)
    print(f"lat={lat} rad, lon={lon} rad, doy={doy}, el=30deg")
    print(f"a_h={ah}, a_w={aw}")
    print(f"mf_d = {mfh!r}")
    print(f"mf_w = {mfw!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
