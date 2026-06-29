"""Minimal IONEX (global ionosphere map) reader -- pure-Python, no deps.

The ionosphere pierce-point viewer can draw a faint *reference* VTEC field
behind the network's own pierce-point estimates: the CODE global IONEX the run
already consumes (``data/<YYYY-DOY>/products/*GIM.INX``).  This parser reads that
file into hourly gridded VTEC maps so the viewer can ``pcolormesh`` the field a
station's estimates are compared against.

Only what the viewer needs is parsed: the lat/lon grid and the per-epoch TEC
maps (RMS maps are skipped).  Values are scaled by the header ``EXPONENT`` into
TECU; the ``9999`` no-data sentinel becomes ``NaN``.  Tested against the CODE
``COD0OPSFIN_..._GIM.INX`` convention (LAT 87.5..-87.5/-2.5, LON -180..180/5,
exponent -1).
"""

from __future__ import annotations

import datetime as _dt
from dataclasses import dataclass, field
from typing import List, Optional

import numpy as np


@dataclass
class IonexMap:
    """One epoch's vertical TEC grid (TECU), shaped ``(n_lat, n_lon)``."""
    epoch: _dt.datetime
    vtec: np.ndarray


@dataclass
class IonexData:
    """Parsed IONEX: the lat/lon axes plus the ordered per-epoch TEC maps."""
    lats: np.ndarray = field(default_factory=lambda: np.array([]))
    lons: np.ndarray = field(default_factory=lambda: np.array([]))
    maps: List[IonexMap] = field(default_factory=list)
    height_km: float = 450.0
    base_radius_km: float = 6371.0

    @property
    def n_maps(self) -> int:
        return len(self.maps)


def _axis(v1: float, v2: float, dv: float) -> np.ndarray:
    n = int(round((v2 - v1) / dv)) + 1
    return v1 + dv * np.arange(n)


def read_ionex(path: str) -> IonexData:
    """Parse an IONEX file into :class:`IonexData` (TEC maps only).

    Raises ``OSError`` if the file cannot be opened and ``ValueError`` if the
    header is missing the grid definition.
    """
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()

    lat1 = lat2 = dlat = lon1 = lon2 = dlon = None
    exponent = -1
    height = 450.0
    base_radius = 6371.0
    i = 0
    n = len(lines)

    # --- header ---
    while i < n:
        line = lines[i]
        label = line[60:].strip()
        if label == "EXPONENT":
            exponent = int(line[:60].split()[0])
        elif label == "HGT1 / HGT2 / DHGT":
            height = float(line[:60].split()[0])
        elif label == "BASE RADIUS":
            base_radius = float(line[:60].split()[0])
        elif label == "LAT1 / LAT2 / DLAT":
            lat1, lat2, dlat = (float(x) for x in line[:60].split()[:3])
        elif label == "LON1 / LON2 / DLON":
            lon1, lon2, dlon = (float(x) for x in line[:60].split()[:3])
        elif label == "END OF HEADER":
            i += 1
            break
        i += 1

    if None in (lat1, lat2, dlat, lon1, lon2, dlon):
        raise ValueError(f"IONEX {path}: missing grid definition in header")

    lats = _axis(lat1, lat2, dlat)
    lons = _axis(lon1, lon2, dlon)
    n_lat, n_lon = lats.size, lons.size
    scale = 10.0 ** exponent

    data = IonexData(lats=lats, lons=lons, height_km=height,
                     base_radius_km=base_radius)

    # --- TEC maps ---
    while i < n:
        label = lines[i][60:].strip()
        if label == "START OF TEC MAP":
            i += 1
            epoch = None
            grid = np.full((n_lat, n_lon), np.nan)
            while i < n:
                lab = lines[i][60:].strip()
                if lab == "END OF TEC MAP":
                    i += 1
                    break
                if lab == "EPOCH OF CURRENT MAP":
                    p = [int(x) for x in lines[i][:60].split()[:6]]
                    epoch = _dt.datetime(p[0], p[1], p[2], p[3], p[4], p[5])
                    i += 1
                elif lab.startswith("LAT/LON1/LON2/DLON"):
                    # Fixed-width record (2X,5F6.1): the LAT/LON1/.. fields touch
                    # (e.g. "  87.5-180.0 ...") so column-slice the LAT field
                    # rather than whitespace-split.
                    lat_val = float(lines[i][2:8])
                    row = int(round((lat_val - lat1) / dlat))
                    i += 1
                    # The band's n_lon values follow on the next lines as I5
                    # fixed-width fields (column-sliced so packed/negative values
                    # parse unambiguously); read exactly n_lon, then fall back to
                    # the outer record loop.
                    vals: List[int] = []
                    while i < n and len(vals) < n_lon:
                        ln = lines[i].rstrip("\n")
                        for c in range(0, len(ln), 5):
                            tok = ln[c:c + 5].strip()
                            if tok:
                                vals.append(int(tok))
                        i += 1
                    arr = np.array(vals[:n_lon], dtype=float)
                    arr[arr == 9999] = np.nan
                    if 0 <= row < n_lat:
                        grid[row, :] = arr * scale
                else:
                    i += 1
            if epoch is not None:
                data.maps.append(IonexMap(epoch=epoch, vtec=grid))
        else:
            i += 1

    return data


def nearest_map(data: IonexData, when: _dt.datetime) -> Optional[IonexMap]:
    """Return the IONEX map whose epoch is closest to ``when`` (or ``None``)."""
    if not data.maps:
        return None
    return min(data.maps, key=lambda m: abs((m.epoch - when).total_seconds()))
