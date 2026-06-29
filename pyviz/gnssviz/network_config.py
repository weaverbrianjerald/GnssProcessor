"""Network station roster: where the worldmap gets station id / lat-lon / role.

A PPP-RTK NETWORK run identifies stations by ECEF only — positions live in the
run config JSON (``network.stations[].position_ecef``, the held-out consumer in
``network.closed_loop.station``) and, when ``network.trace_stations`` is on, in a
``network_stations.csv`` roster the producer writes alongside the other traces.

This module turns either source into a list of :class:`Station` records with
geodetic lat/lon (via :func:`gnssviz.geodesy.ecef_to_lla`) and a *role* —
``"datum"`` (the clock-datum pivot), ``"network"`` (an estimating peer), or
``"held_out"`` (the closed-loop consumer / user station, NOT in the solve).  The
roster CSV is preferred when present (it reflects the stations that actually
loaded); the config JSON is the fallback (and the only source before a run).
"""

from __future__ import annotations

import csv
import json
import os
import re
from dataclasses import dataclass
from typing import List, Optional

import numpy as np

from .geodesy import ecef_to_lla


def _loads_jsonc(text: str) -> dict:
    """Parse JSON that may carry ``//`` / ``/* */`` comments and trailing commas.

    The GnssProcessor config files are JSONC (the C++ loader strips comments);
    Python's ``json`` is strict, so strip comments outside string literals and
    drop trailing commas before re-parsing.
    """
    out: List[str] = []
    i, n = 0, len(text)
    in_str = False
    esc = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i += 2
            while i < n and text[i] not in "\r\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    stripped = re.sub(r",(\s*[}\]])", r"\1", "".join(out))
    return json.loads(stripped)

ROLE_DATUM = "datum"
ROLE_NETWORK = "network"
ROLE_HELD_OUT = "held_out"


@dataclass
class Station:
    """One network station with ECEF, geodetic lat/lon/height, and role."""
    id: str
    role: str
    x: float
    y: float
    z: float
    lat: float
    lon: float
    height: float

    @property
    def label(self) -> str:
        """Compact 4-char display label (the RINEX-3 country-stripped code)."""
        return self.id[:4] if self.id else self.id


def _mk(name: str, ecef, role: str) -> Station:
    x, y, z = (float(ecef[0]), float(ecef[1]), float(ecef[2]))
    lat, lon, h = ecef_to_lla(x, y, z)
    return Station(id=name, role=role, x=x, y=y, z=z,
                   lat=float(np.asarray(lat)), lon=float(np.asarray(lon)),
                   height=float(np.asarray(h)))


def stations_from_config(path: str) -> List[Station]:
    """Parse a network config JSON into :class:`Station` records.

    Roles are inferred structurally: a station whose name matches
    ``network.clock_datum_station`` is ``"datum"``; the rest of
    ``network.stations[]`` are ``"network"``; ``network.closed_loop.station`` (when
    the harness is enabled and carries a position) is ``"held_out"``.
    """
    with open(path, "r", encoding="utf-8") as fh:
        cfg = _loads_jsonc(fh.read())
    net = cfg.get("network", {})
    datum = net.get("clock_datum_station", "")
    out: List[Station] = []
    for s in net.get("stations", []):
        ecef = s.get("position_ecef")
        name = s.get("name", "")
        if not ecef or len(ecef) < 3:
            continue
        role = ROLE_DATUM if name == datum else ROLE_NETWORK
        out.append(_mk(name, ecef, role))
    cl = net.get("closed_loop", {})
    cs = cl.get("station") if isinstance(cl, dict) else None
    if cl.get("enable") and cs and cs.get("position_ecef"):
        out.append(_mk(cs.get("name", "USER"), cs["position_ecef"], ROLE_HELD_OUT))
    return out


def stations_from_roster_csv(path: str) -> List[Station]:
    """Parse a ``network_stations.csv`` roster (station,role,ecef_x,ecef_y,ecef_z)."""
    out: List[Station] = []
    with open(path, "r", encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            try:
                out.append(_mk(row["station"],
                               (row["ecef_x"], row["ecef_y"], row["ecef_z"]),
                               row.get("role", ROLE_NETWORK)))
            except (KeyError, ValueError):
                continue
    return out


def load_stations(roster_csv: Optional[str] = None,
                  config_json: Optional[str] = None) -> List[Station]:
    """Load stations from the roster CSV if it exists, else the config JSON.

    Returns ``[]`` when neither source is available (the caller then skips the
    geographic viewers, mirroring the missing-CSV skip the other viewers use).
    """
    if roster_csv and os.path.isfile(roster_csv) and os.path.getsize(roster_csv) > 0:
        sts = stations_from_roster_csv(roster_csv)
        if sts:
            return sts
    if config_json and os.path.isfile(config_json):
        return stations_from_config(config_json)
    return []
