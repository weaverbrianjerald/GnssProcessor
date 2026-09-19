#!/usr/bin/env python3
"""Propagate NGS Station Information File (SIF) coordinates to a target epoch.

Standalone, dependency-free (Python >= 3.7 standard library only) re-implementation
of GnssProcessor's ``StationInfoFile`` / ``resolveStationCoordinate`` so that any
project can turn

    (station list, target epoch, one or more SIF JSON files)

into

    (ITRF ECEF coordinates + propagated uncertainties at that epoch)

without linking the C++ engine.  A SIF is NGS's multi-year CORS/IGS solution:
per-station ITRF coordinates + velocities + seasonal (periodic) motion +
post-seismic deformation, all referred to the SIF reference frame/epoch
(typically ITRF2020 @ 2020.0).

The coordinate function evaluated here is exactly the one the C++ engine uses to
seed held-fixed network-station coordinates::

    x(t) = x_ref + v * (t - t_ref)
                 + SUM periodic(t)            (annual + semiannual seasonal terms)
                 + SUM post-seismic(t)        (log/exp relaxation per E/N/U, ENU->ECEF)

and the position covariance is propagated from the OCCUPATION 6x6
coordinate/velocity covariance::

    Cxx(t) = Cxx + dt * (Cxv + Cxv^T) + dt^2 * Cvv,   dt = t - t_ref

Port fidelity is verified numerically against the C++ engine's own output
(see ``test_sif_propagate.py``); the two agree to ~1e-9 m.

Usage
-----
    # decimal-year epoch, CSV to stdout, auto-find bundled SIFs
    python sif_propagate.py CIT100USA ANTC00CHL --epoch 2024.36

    # calendar or DOY epoch, explicit SIF files, JSON to a file
    python sif_propagate.py --sif igs20_SIF.json ncn20_SIF.json \
        --date 2024-05-08 --format json --output out.json AMC2 P041

    # stations from a file (one id per line, or a JSON array)
    python sif_propagate.py --stations-file roster.txt --doy 2024:129

    # NGS datasheet recipe: CURRENT occupation, linear only, back to 2010.0
    python sif_propagate.py CORV00USA --epoch 2010.0 --datasheet

Identifiers may be a 9-char IGS long name (``CIT100USA``), a 4-char marker
(``CIT1``, case-insensitive), or a DOMES number (``40400S201``) -- matching the
C++ ``find()`` resolution order.
"""
from __future__ import annotations

import argparse
import csv
import glob
import io
import json
import math
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# --------------------------------------------------------------------------- #
# Constants (WGS84 ellipsoid, matching GnssProcessor::Constants::Ellipsoid)     #
# --------------------------------------------------------------------------- #
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
_PI = 3.14159265358979323846  # matches the literal used in the C++ periodic term

# Provenance for the geodetic lat/lon/height columns.  WGS84 is used because it is
# exactly the ellipsoid GnssProcessor's own ECEF<->LLH uses, so the tool stays
# consistent with the engine; WGS84 vs GRS80/ITRF differ only in the last digits of
# the flattening (sub-mm in latitude/height), so WGS84 is sufficient here.
WGS84_NOTE = ("lat_deg/lon_deg/height_m are geodetic on the WGS84 ellipsoid "
              "(a=6378137.0 m, 1/f=298.257223563); matches GnssProcessor's ECEF<->LLH "
              "and is sub-mm-equivalent to GRS80/ITRF.")
_END_SENTINEL = 9999.0        # open-ended SIF "9999-..." window end
_NEAREST_WINDOW_CAP = 9000.0  # midpoint cap used when picking the nearest occupation

# --------------------------------------------------------------------------- #
# Time: SIF timestamp / calendar date -> decimal year                          #
# --------------------------------------------------------------------------- #
_CUM_DAYS = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)


def _is_leap(year: int) -> bool:
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def decimal_year_from_ymd(year: int, month: int, day: int,
                          hour: int = 0, minute: int = 0, second: int = 0) -> float:
    """Convert a calendar date/time to a decimal year (SIF convention).

    Mirrors ``decimalYearFromSif`` in the C++ loader byte-for-byte: day-of-year is
    zero-based, leap days count only after February, and any timezone (UTC/GPS)
    suffix is ignored -- the <=18 s GPS-UTC offset is far below the precision at
    which window selection or velocity propagation matters.
    """
    if year >= 9000:
        return _END_SENTINEL
    if month < 1 or month > 12:
        month = 1
    if day < 1:
        day = 1
    leap = _is_leap(year)
    doy = _CUM_DAYS[month - 1] + (day - 1) + (1 if (leap and month > 2) else 0)
    diy = 366.0 if leap else 365.0
    return year + (doy + (hour * 3600.0 + minute * 60.0 + second) / 86400.0) / diy


_SIF_TS_RE = re.compile(r"\s*(-?\d+)-(\d+)-(\d+)(?:[T ](\d+):(\d+):(\d+))?")


def decimal_year_from_sif(ts: str) -> float:
    """Parse a SIF timestamp ("YYYY-MM-DDTHH:MM:SS TZ") to a decimal year.

    Returns 0.0 for anything that does not present at least YYYY-MM-DD, matching
    the C++ ``sscanf(...) < 3`` guard.  An open-ended END ("9999-...") maps to the
    sentinel so it always compares as "in the future".
    """
    if not ts:
        return 0.0
    m = _SIF_TS_RE.match(ts)
    if not m:
        return 0.0
    y, mo, d = int(m.group(1)), int(m.group(2)), int(m.group(3))
    hh = int(m.group(4) or 0)
    mm = int(m.group(5) or 0)
    ss = int(m.group(6) or 0)
    return decimal_year_from_ymd(y, mo, d, hh, mm, ss)


def parse_epoch_argument(epoch: Optional[float],
                         date: Optional[str],
                         doy: Optional[str]) -> float:
    """Resolve the mutually-exclusive epoch CLI options to a single decimal year."""
    if epoch is not None:
        return float(epoch)
    if date is not None:
        m = _SIF_TS_RE.match(date.strip())
        if not m:
            raise ValueError(f"--date not understood: {date!r} (want YYYY-MM-DD[THH:MM:SS])")
        return decimal_year_from_sif(date)
    if doy is not None:
        try:
            yy, dd = doy.split(":")
            yy_i, dd_i = int(yy), int(dd)
        except ValueError:
            raise ValueError(f"--doy not understood: {doy!r} (want YYYY:DOY, e.g. 2024:129)")
        diy = 366.0 if _is_leap(yy_i) else 365.0
        return yy_i + (dd_i - 1) / diy
    raise ValueError("no epoch given; pass one of --epoch / --date / --doy")


# --------------------------------------------------------------------------- #
# Data model (SI units after load, matching common/StationInfo.hpp)            #
# --------------------------------------------------------------------------- #
@dataclass
class Occupation:
    start_year: float = 0.0
    end_year: float = 0.0
    coordinates: Tuple[float, float, float] = (0.0, 0.0, 0.0)   # ECEF at t_ref [m]
    coord_sigmas: Tuple[float, float, float] = (0.0, 0.0, 0.0)  # [m]
    velocity: Tuple[float, float, float] = (0.0, 0.0, 0.0)      # [m/yr]
    velocity_sigmas: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    covariance: Optional[List[List[float]]] = None              # 6x6 [X Y Z Vx Vy Vz]
    do_not_use: bool = False


@dataclass
class PeriodicFreq:
    period_yr: float = 1.0
    x_cos: float = 0.0
    x_sin: float = 0.0
    y_cos: float = 0.0
    y_sin: float = 0.0
    z_cos: float = 0.0
    z_sin: float = 0.0


@dataclass
class PeriodicBlock:
    start_year: float = 0.0
    end_year: float = 0.0
    reference_epoch_year: float = 2020.0
    freqs: List[PeriodicFreq] = field(default_factory=list)


@dataclass
class PsdComponent:
    aexp: List[float] = field(default_factory=list)
    texp: List[float] = field(default_factory=list)
    alog: List[float] = field(default_factory=list)
    tlog: List[float] = field(default_factory=list)


@dataclass
class PsdBlock:
    event_year: float = 0.0
    east: PsdComponent = field(default_factory=PsdComponent)
    north: PsdComponent = field(default_factory=PsdComponent)
    up: PsdComponent = field(default_factory=PsdComponent)


@dataclass
class Station:
    id: str = ""
    domes: str = ""
    description: str = ""
    reference_frame: str = ""
    reference_epoch_year: float = 2020.0
    source: str = ""
    occupations: List[Occupation] = field(default_factory=list)
    periodic: List[PeriodicBlock] = field(default_factory=list)
    psd: List[PsdBlock] = field(default_factory=list)


# --------------------------------------------------------------------------- #
# Small JSON accessors (mirror the jnum/jvec3/jarr helpers in the C++ loader)  #
# --------------------------------------------------------------------------- #
def _jnum(obj: dict, key: str, default: float = 0.0) -> float:
    v = obj.get(key, default)
    return float(v) if isinstance(v, (int, float)) else default


def _jstr(obj: dict, key: str) -> str:
    v = obj.get(key)
    return v if isinstance(v, str) else ""


def _jbool(obj: dict, key: str, default: bool = False) -> bool:
    v = obj.get(key, default)
    return v if isinstance(v, bool) else default


def _jvec3(obj: dict, key: str) -> Tuple[float, float, float]:
    v = obj.get(key)
    out = [0.0, 0.0, 0.0]
    if isinstance(v, list) and len(v) >= 3:
        for k in range(3):
            if isinstance(v[k], (int, float)):
                out[k] = float(v[k])
    return (out[0], out[1], out[2])


def _jarr(obj: dict, key: str) -> List[float]:
    v = obj.get(key)
    if isinstance(v, list):
        return [float(e) for e in v if isinstance(e, (int, float))]
    return []


def _upper_trim(s: str) -> str:
    return s.strip().upper()


# --------------------------------------------------------------------------- #
# SIF registry: parse one or more SIF JSON files, resolve by identifier        #
# --------------------------------------------------------------------------- #
class SifRegistry:
    """Parses SIF JSON files and resolves per-station metadata by identifier.

    Indexing mirrors the C++ ``StationInfoFile``: full 9-char ID, 4-char prefix
    (first-wins), and DOMES number, all upper-cased.  ``find()`` tries ID, then
    DOMES, then 4-char prefix -- case-insensitive.
    """

    def __init__(self) -> None:
        self.stations: List[Station] = []
        self._by_id: Dict[str, int] = {}
        self._by_id4: Dict[str, int] = {}
        self._by_domes: Dict[str, int] = {}

    # -- loading ----------------------------------------------------------- #
    def load(self, sif_json: os.PathLike) -> int:
        """Parse one SIF JSON file, appending its stations. Returns count added."""
        path = Path(sif_json)
        with io.open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)

        meta = doc.get("METADATA", {}) if isinstance(doc.get("METADATA"), dict) else {}
        meta_epoch = _jnum(meta, "REFERENCE_EPOCH", 2020.0)
        meta_frame = _jstr(meta, "REFERENCE_FRAME")

        stations = doc.get("STATIONS")
        if not isinstance(stations, list):
            raise ValueError(f"SIF '{path}' has no STATIONS array")

        added = 0
        for sj in stations:
            st = self._parse_station(sj, meta_epoch, meta_frame, str(path))
            if st is None:
                continue
            idx = len(self.stations)
            self.stations.append(st)
            id_u = _upper_trim(st.id)
            if id_u:
                self._by_id.setdefault(id_u, idx)
                if len(id_u) >= 4:
                    self._by_id4.setdefault(id_u[:4], idx)   # first wins
            dm = _upper_trim(st.domes)
            if dm:
                self._by_domes.setdefault(dm, idx)
            added += 1
        return added

    @staticmethod
    def _parse_station(sj: dict, meta_epoch: float, meta_frame: str,
                       source: str) -> Optional[Station]:
        sid = _jstr(sj, "ID")
        if not sid:
            return None  # a station with no ID is unusable
        st = Station(
            id=sid,
            domes=_jstr(sj, "DOMES"),
            description=_jstr(sj, "DESCRIPTION"),
            reference_frame=meta_frame,
            reference_epoch_year=meta_epoch,
            source=source,
        )

        # --- Occupations (ITRF coordinate solutions) ---
        occ = sj.get("OCCUPATION")
        if isinstance(occ, list):
            for oj in occ:
                o = Occupation(
                    start_year=decimal_year_from_sif(_jstr(oj, "START")),
                    end_year=decimal_year_from_sif(_jstr(oj, "END")),
                    coordinates=_jvec3(oj, "COORDINATES"),          # m (COORDINATE_UNITS=m)
                    coord_sigmas=_jvec3(oj, "COORDINATE_SIGMAS"),   # m
                    do_not_use=_jbool(oj, "DO_NOT_USE"),
                )
                vx, vy, vz = _jvec3(oj, "VELOCITY")                 # mm/y -> m/y
                o.velocity = (vx * 1e-3, vy * 1e-3, vz * 1e-3)
                svx, svy, svz = _jvec3(oj, "VELOCITY_SIGMAS")
                o.velocity_sigmas = (svx * 1e-3, svy * 1e-3, svz * 1e-3)
                o.covariance = SifRegistry._assemble_covariance(oj, o)
                st.occupations.append(o)

        # --- Nonlinear terms: periodic (seasonal) + post-seismic ---
        nlt = sj.get("NONLINEAR_TERMS")
        if isinstance(nlt, dict):
            per = nlt.get("PERIODIC_TERMS")
            if isinstance(per, list):
                for bj in per:
                    b = PeriodicBlock(
                        start_year=decimal_year_from_sif(_jstr(bj, "START")),
                        end_year=decimal_year_from_sif(_jstr(bj, "END")),
                        reference_epoch_year=meta_epoch,  # phase ref = METADATA epoch
                    )
                    fr = bj.get("FREQUENCIES")
                    if isinstance(fr, list):
                        for fj in fr:
                            mm = 1e-3  # mm -> m (PERIODIC_COEFF_UNITS=mm)
                            b.freqs.append(PeriodicFreq(
                                period_yr=_jnum(fj, "PERIOD", 1.0),
                                x_cos=mm * _jnum(fj, "X_COS"),
                                x_sin=mm * _jnum(fj, "X_SIN"),
                                y_cos=mm * _jnum(fj, "Y_COS"),
                                y_sin=mm * _jnum(fj, "Y_SIN"),
                                z_cos=mm * _jnum(fj, "Z_COS"),
                                z_sin=mm * _jnum(fj, "Z_SIN"),
                            ))
                    st.periodic.append(b)

            psd = nlt.get("POST_SEISMIC")
            if isinstance(psd, list):
                for pj in psd:
                    pb = PsdBlock(event_year=decimal_year_from_sif(_jstr(pj, "EVENT_EPOCH")))
                    for key, comp in (("EAST", pb.east), ("NORTH", pb.north), ("UP", pb.up)):
                        cj = pj.get(key)
                        if isinstance(cj, dict):
                            comp.aexp = _jarr(cj, "AEXP")  # m, y (no conversion)
                            comp.texp = _jarr(cj, "TEXP")
                            comp.alog = _jarr(cj, "ALOG")
                            comp.tlog = _jarr(cj, "TLOG")
                    st.psd.append(pb)

        return st

    @staticmethod
    def _assemble_covariance(oj: dict, o: Occupation) -> Optional[List[List[float]]]:
        """Assemble the symmetric 6x6 [X Y Z Vx Vy Vz] covariance, or None.

        Diagonal from the (already-SI) sigma arrays; the 15 off-diagonal terms from
        6X6_COVARIANCE_MATRIX scaled mm^2 -> m^2 (1e-6) -- position, velocity, and
        cross terms all share that factor.  Requires >= 15 elements, matching C++.
        """
        c = _jarr(oj, "6X6_COVARIANCE_MATRIX")
        if len(c) < 15:
            return None
        C = [[0.0] * 6 for _ in range(6)]
        C[0][0] = o.coord_sigmas[0] ** 2
        C[1][1] = o.coord_sigmas[1] ** 2
        C[2][2] = o.coord_sigmas[2] ** 2
        C[3][3] = o.velocity_sigmas[0] ** 2
        C[4][4] = o.velocity_sigmas[1] ** 2
        C[5][5] = o.velocity_sigmas[2] ** 2
        s = 1e-6  # mm^2 -> m^2
        C[0][1] = s * c[0]; C[0][2] = s * c[1]; C[0][3] = s * c[2]
        C[0][4] = s * c[3]; C[0][5] = s * c[4]
        C[1][2] = s * c[5]; C[1][3] = s * c[6]; C[1][4] = s * c[7]
        C[1][5] = s * c[8]
        C[2][3] = s * c[9]; C[2][4] = s * c[10]; C[2][5] = s * c[11]
        C[3][4] = s * c[12]; C[3][5] = s * c[13]
        C[4][5] = s * c[14]
        for i in range(6):
            for j in range(i + 1, 6):
                C[j][i] = C[i][j]
        return C

    # -- lookup ------------------------------------------------------------ #
    def find(self, station_key: str) -> Optional[Station]:
        """Resolve a station by full ID, DOMES, then 4-char prefix (case-insensitive)."""
        k = _upper_trim(station_key)
        if not k:
            return None
        if k in self._by_id:
            return self.stations[self._by_id[k]]
        if k in self._by_domes:
            return self.stations[self._by_domes[k]]
        if len(k) >= 4 and k[:4] in self._by_id4:
            return self.stations[self._by_id4[k[:4]]]
        return None

    def __len__(self) -> int:
        return len(self.stations)


# --------------------------------------------------------------------------- #
# Geodesy (mirror math/Coordinates.cpp)                                        #
# --------------------------------------------------------------------------- #
def ecef_to_llh(x: float, y: float, z: float,
                a: float = WGS84_A, f: float = WGS84_F) -> Tuple[float, float, float]:
    """ECEF -> (lat_rad, lon_rad, h_m), Bowring init + fixed-point iteration."""
    e2 = f * (2.0 - f)
    p = math.sqrt(x * x + y * y)
    lon = math.atan2(y, x)
    if p < 1e-12:
        lat = _PI / 2.0 if z >= 0.0 else -_PI / 2.0
        sin_lat = math.sin(lat)
        N = a / math.sqrt(1.0 - e2 * sin_lat * sin_lat)
        h = abs(z) - N * (1.0 - e2)
        return lat, lon, h
    lat = math.atan2(z, p * (1.0 - e2))
    h = 0.0
    for _ in range(10):
        sin_lat = math.sin(lat)
        N = a / math.sqrt(1.0 - e2 * sin_lat * sin_lat)
        h = p / math.cos(lat) - N
        denom = p * (1.0 - e2 * (N / (N + h)))
        if abs(denom) < 1e-15:
            break
        lat_next = math.atan2(z, denom)
        if abs(lat_next - lat) < 1e-13:
            lat = lat_next
            break
        lat = lat_next
    return lat, lon, h


def enu_to_ecef_matrix(lat_rad: float, lon_rad: float) -> List[List[float]]:
    """3x3 rotation whose columns are the E, N, U unit vectors in ECEF."""
    slat, clat = math.sin(lat_rad), math.cos(lat_rad)
    slon, clon = math.sin(lon_rad), math.cos(lon_rad)
    # Row-major; columns are [E | N | U].
    return [
        [-slon, -slat * clon, clat * clon],
        [clon, -slat * slon, clat * slon],
        [0.0, clat, slat],
    ]


def _mat3_vec(m: List[List[float]], v: Sequence[float]) -> Tuple[float, float, float]:
    return (
        m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
        m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
    )


def _mat3_mul(a: List[List[float]], b: List[List[float]]) -> List[List[float]]:
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _mat3_transpose(a: List[List[float]]) -> List[List[float]]:
    return [[a[j][i] for j in range(3)] for i in range(3)]


# --------------------------------------------------------------------------- #
# The coordinate function: resolve one station at a target epoch               #
# --------------------------------------------------------------------------- #
@dataclass
class ResolvedCoord:
    valid: bool = False
    ecef: Tuple[float, float, float] = (0.0, 0.0, 0.0)   # [m]
    covariance: Optional[List[List[float]]] = None       # propagated 3x3 position cov [m^2]
    has_covariance: bool = False
    in_window: bool = False                              # epoch inside the occupation window?
    window: Tuple[float, float] = (0.0, 0.0)             # (start_year, end_year) of the used occupation
    occupation_mode: str = "covering"                    # selection mode actually applied


OCCUPATION_MODES = ("covering", "latest")


def _select_occupation(station: Station, year: float,
                       mode: str = "covering") -> Optional[Occupation]:
    """Pick the occupation to evaluate at ``year``.

    ``mode="covering"`` (default, and the only mode the C++ engine implements):
    the occupation whose window covers ``year`` (start<=year<end, not flagged),
    else the non-flagged occupation whose window midpoint is nearest.

    ``mode="latest"``: the last non-flagged occupation by window start -- the
    *current* solution for the mark.  This is the NGS datasheet convention: a
    published NAD83(2011) epoch-2010.00 coordinate is the CURRENT occupation
    propagated linearly back to 2010.0, not the occupation that was live in 2010.
    Returns None when the station has no usable occupation.
    """
    if mode == "latest":
        best: Optional[Occupation] = None
        for o in station.occupations:
            if o.do_not_use:
                continue
            if best is None or o.start_year > best.start_year:
                best = o
        return best
    if mode != "covering":
        raise ValueError(f"unknown occupation mode {mode!r}; want one of {OCCUPATION_MODES}")
    best = None
    best_dist = 1e30
    for o in station.occupations:
        if o.do_not_use:
            continue
        if o.start_year <= year < o.end_year:
            return o
        mid = 0.5 * (o.start_year + min(o.end_year, _NEAREST_WINDOW_CAP))
        dist = abs(year - mid)
        if dist < best_dist:
            best_dist = dist
            best = o
    return best


def _eval_psd_component(comp: PsdComponent, dt: float) -> float:
    d = 0.0
    for a, t in zip(comp.alog, comp.tlog):
        if t > 0.0:
            d += a * math.log(1.0 + dt / t)
    for a, t in zip(comp.aexp, comp.texp):
        if t > 0.0:
            d += a * (1.0 - math.exp(-dt / t))
    return d


def resolve_station_coordinate(station: Station, decimal_year: float,
                               with_periodic: bool = True,
                               with_psd: bool = True,
                               occupation: str = "covering") -> ResolvedCoord:
    """Evaluate a station's SIF coordinate function at ``decimal_year``.

    With the default ``occupation="covering"`` this is a faithful port of
    ``GnssProcessor::resolveStationCoordinate``.  ``occupation="latest"`` instead
    evaluates the CURRENT (last-starting, non-flagged) occupation -- the NGS
    datasheet convention for a published legacy-epoch coordinate.  Everything
    else (linear term, periodic/PSD toggles, covariance propagation, dt measured
    from the SIF reference epoch) is identical and applies to whichever
    occupation was selected; periodic and post-seismic terms are keyed on the
    EPOCH, not the occupation, so "latest" does not imply "no periodic" --
    pass ``with_periodic=False`` for that.

    Returns a ``ResolvedCoord`` whose ``valid`` is False when the station has no
    usable occupation (the caller should then fall back to its own a-priori).
    """
    out = ResolvedCoord()
    out.occupation_mode = occupation
    occ = _select_occupation(station, decimal_year, occupation)
    if occ is None:
        return out

    dt = decimal_year - station.reference_epoch_year

    # Linear term: reference coordinate + velocity * (t - t_ref).
    x = occ.coordinates[0] + occ.velocity[0] * dt
    y = occ.coordinates[1] + occ.velocity[1] * dt
    z = occ.coordinates[2] + occ.velocity[2] * dt

    # Periodic (seasonal) terms over the block whose window covers the epoch.
    if with_periodic:
        for b in station.periodic:
            if decimal_year < b.start_year or decimal_year >= b.end_year:
                continue
            dT = decimal_year - b.reference_epoch_year
            for fr in b.freqs:
                if fr.period_yr == 0.0:
                    continue
                w = 2.0 * _PI / fr.period_yr
                c, sn = math.cos(w * dT), math.sin(w * dT)
                x += fr.x_cos * c + fr.x_sin * sn
                y += fr.y_cos * c + fr.y_sin * sn
                z += fr.z_cos * c + fr.z_sin * sn

    # Post-seismic deformation: log/exp relaxation per ENU component, ENU->ECEF.
    if with_psd and station.psd:
        e = n = u = 0.0
        for pb in station.psd:
            dT = decimal_year - pb.event_year
            if dT <= 0.0:
                continue  # event has not happened yet at this epoch
            e += _eval_psd_component(pb.east, dT)
            n += _eval_psd_component(pb.north, dT)
            u += _eval_psd_component(pb.up, dT)
        if (e * e + n * n + u * u) > 0.0:
            lat, lon, _h = ecef_to_llh(x, y, z)
            R = enu_to_ecef_matrix(lat, lon)
            dx, dy, dz = _mat3_vec(R, (e, n, u))
            x += dx
            y += dy
            z += dz

    out.ecef = (x, y, z)

    # Propagate the position covariance to the epoch:
    #   Cxx(t) = Cxx + dt*(Cxv + Cxv^T) + dt^2 * Cvv.
    if occ.covariance is not None:
        C = occ.covariance
        cov = [[0.0, 0.0, 0.0] for _ in range(3)]
        for i in range(3):
            for j in range(3):
                cxx = C[i][j]
                cxv = C[i][3 + j]
                cvx = C[3 + i][j]      # = Cxv^T
                cvv = C[3 + i][3 + j]
                cov[i][j] = cxx + dt * (cxv + cvx) + dt * dt * cvv
        out.covariance = cov
        out.has_covariance = True

    out.in_window = occ.start_year <= decimal_year < occ.end_year
    out.window = (occ.start_year, occ.end_year)
    out.valid = True
    return out


# --------------------------------------------------------------------------- #
# Uncertainty helpers                                                          #
# --------------------------------------------------------------------------- #
def ecef_sigmas(cov: List[List[float]]) -> Tuple[float, float, float]:
    """sqrt of the ECEF variance diagonal [m]."""
    return (math.sqrt(max(cov[0][0], 0.0)),
            math.sqrt(max(cov[1][1], 0.0)),
            math.sqrt(max(cov[2][2], 0.0)))


def enu_covariance(cov: List[List[float]], lat_rad: float, lon_rad: float) -> List[List[float]]:
    """Rotate an ECEF 3x3 covariance into the local ENU frame: C_enu = R^T C R."""
    R = enu_to_ecef_matrix(lat_rad, lon_rad)
    return _mat3_mul(_mat3_transpose(R), _mat3_mul(cov, R))


def enu_sigmas(cov: List[List[float]], lat_rad: float, lon_rad: float) -> Tuple[float, float, float]:
    """East/North/Up sigmas [m] from an ECEF covariance."""
    c = enu_covariance(cov, lat_rad, lon_rad)
    return (math.sqrt(max(c[0][0], 0.0)),
            math.sqrt(max(c[1][1], 0.0)),
            math.sqrt(max(c[2][2], 0.0)))


# --------------------------------------------------------------------------- #
# Record assembly + output                                                     #
# --------------------------------------------------------------------------- #
def build_record(key: str, station: Optional[Station], resolved: Optional[ResolvedCoord],
                 decimal_year: float, with_covariance: bool = True) -> dict:
    """Assemble a flat, serialisable result record for one requested station."""
    rec: dict = {
        "requested": key,
        "found": station is not None,
        "valid": bool(resolved and resolved.valid),
        "id": station.id if station else "",
        "domes": station.domes if station else "",
        "description": station.description if station else "",
        "reference_frame": station.reference_frame if station else "",
        "reference_epoch_year": station.reference_epoch_year if station else None,
        "epoch_year": decimal_year,
        "source": station.source if station else "",
    }
    if resolved and resolved.valid:
        x, y, z = resolved.ecef
        lat, lon, h = ecef_to_llh(x, y, z)
        rec.update({
            "x": x, "y": y, "z": z,
            "lat_deg": math.degrees(lat), "lon_deg": math.degrees(lon), "height_m": h,
            "in_window": resolved.in_window,
            "window_start": resolved.window[0],
            "window_end": (None if resolved.window[1] >= _NEAREST_WINDOW_CAP else resolved.window[1]),
            "has_covariance": resolved.has_covariance,
            "occupation_mode": resolved.occupation_mode,
        })
        if resolved.has_covariance and resolved.covariance is not None:
            sx, sy, sz = ecef_sigmas(resolved.covariance)
            se, sn, su = enu_sigmas(resolved.covariance, lat, lon)
            rec.update({
                "sigma_x": sx, "sigma_y": sy, "sigma_z": sz,
                "sigma_e": se, "sigma_n": sn, "sigma_u": su,
            })
            if with_covariance:
                rec["covariance_ecef"] = resolved.covariance
        else:
            rec.update({"sigma_x": None, "sigma_y": None, "sigma_z": None,
                        "sigma_e": None, "sigma_n": None, "sigma_u": None})
    return rec


_CSV_COLUMNS = [
    "requested", "found", "valid", "id", "domes", "description",
    "reference_frame", "epoch_year",
    "x", "y", "z", "lat_deg", "lon_deg", "height_m",
    "sigma_x", "sigma_y", "sigma_z", "sigma_e", "sigma_n", "sigma_u",
    "in_window", "window_start", "window_end", "source",
    # Appended LAST so the historical column order is untouched for existing
    # consumers; default runs report "covering".
    "occupation_mode",
]


# Fixed decimal places per float column.  Angles need ~11 places (~1 micrometre at
# the Earth's surface); metric columns get 6 (micrometre) -- both far below any GNSS
# precision, so nothing meaningful is ever truncated.  A blanket format cannot serve
# both: 6 places on a degree is only ~0.1 m.
_CSV_DECIMALS = {
    "lat_deg": 11, "lon_deg": 11,
    "height_m": 6, "x": 6, "y": 6, "z": 6,
    "sigma_x": 6, "sigma_y": 6, "sigma_z": 6,
    "sigma_e": 6, "sigma_n": 6, "sigma_u": 6,
    "epoch_year": 6, "reference_epoch_year": 6,
    "window_start": 6, "window_end": 6,
}
_CSV_DEFAULT_DECIMALS = 6


def _fmt_csv(column: str, value) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.{_CSV_DECIMALS.get(column, _CSV_DEFAULT_DECIMALS)}f}"
    return str(value)


def write_csv(records: List[dict], stream) -> None:
    w = csv.writer(stream, lineterminator="\n")
    w.writerow(_CSV_COLUMNS)
    for rec in records:
        w.writerow([_fmt_csv(col, rec.get(col)) for col in _CSV_COLUMNS])


def write_json(records: List[dict], stream, epoch_year: float, sif_paths: List[str]) -> None:
    payload = {
        "epoch_year": epoch_year,
        "sif_files": sif_paths,
        "geodetic_note": WGS84_NOTE,
        "stations": records,
    }
    json.dump(payload, stream, indent=2)
    stream.write("\n")


# --------------------------------------------------------------------------- #
# SIF file discovery                                                           #
# --------------------------------------------------------------------------- #
def default_sif_paths() -> List[str]:
    """Locate bundled SIF files without an explicit --sif.

    Order: $SIF_PATHS (os.pathsep-separated), then the first non-empty match of
    these, searched under the script directory, its parents, and the CWD:

        <root>/sif/*.json               (standalone project layout)
        <root>/data/static/sif/*.json   (GnssProcessor tree layout)

    Returns [] if none are found.
    """
    env = os.environ.get("SIF_PATHS")
    if env:
        return [p for p in env.split(os.pathsep) if p.strip()]
    here = Path(__file__).resolve().parent
    roots = [here, here.parent, here.parent.parent, here.parent.parent.parent, Path.cwd()]
    subdirs = (("sif",), ("data", "static", "sif"))
    for root in roots:
        for sub in subdirs:
            cand = sorted(glob.glob(str(root.joinpath(*sub) / "*.json")))
            if cand:
                return cand
    return []


def load_station_keys(positional: List[str], stations_file: Optional[str]) -> List[str]:
    keys = list(positional)
    if stations_file:
        text = Path(stations_file).read_text(encoding="utf-8").strip()
        parsed = None
        if text.startswith("["):
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                parsed = None
        if isinstance(parsed, list):
            keys.extend(str(k).strip() for k in parsed if str(k).strip())
        else:
            for line in text.splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    keys.extend(line.split())
    # De-duplicate, preserving order.
    seen = set()
    out = []
    for k in keys:
        if k not in seen:
            seen.add(k)
            out.append(k)
    return out


# --------------------------------------------------------------------------- #
# CLI                                                                          #
# --------------------------------------------------------------------------- #
def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("stations", nargs="*",
                    help="Station identifiers (9-char ID, 4-char marker, or DOMES).")
    ap.add_argument("--stations-file",
                    help="File of identifiers: whitespace/newline list, or a JSON array.")
    ap.add_argument("--sif", nargs="+", metavar="PATH",
                    help="SIF JSON file(s). Default: $SIF_PATHS or bundled data/static/sif/*.json.")

    g = ap.add_mutually_exclusive_group()
    g.add_argument("--epoch", type=float, metavar="YEAR",
                   help="Target epoch as a decimal year, e.g. 2024.36.")
    g.add_argument("--date", metavar="YYYY-MM-DD[THH:MM:SS]",
                   help="Target epoch as a calendar date.")
    g.add_argument("--doy", metavar="YYYY:DOY", help="Target epoch as year:day-of-year.")

    ap.add_argument("--format", choices=("csv", "json"), default="csv",
                    help="Output format (default: csv).")
    ap.add_argument("--output", "-o", help="Write to this file instead of stdout.")
    ap.add_argument("--no-periodic", action="store_true",
                    help="Disable seasonal (periodic) terms.")
    ap.add_argument("--no-psd", action="store_true",
                    help="Disable post-seismic deformation terms.")
    ap.add_argument("--no-covariance", action="store_true",
                    help="Omit the full 3x3 covariance matrix from JSON output (sigmas still shown).")
    ap.add_argument("--occupation", choices=OCCUPATION_MODES, default="covering",
                    help="Occupation selection: 'covering' (default; the window containing the "
                         "epoch, else nearest -- what the C++ engine does) or 'latest' (the "
                         "current occupation, propagated back; the NGS datasheet convention).")
    ap.add_argument("--datasheet", action="store_true",
                    help="Preset reproducing NGS datasheet coordinates: --occupation latest "
                         "plus --no-periodic. Post-seismic terms stay ON (pass --no-psd to "
                         "drop them); the published value still needs the epoch-matched "
                         "ITRF2020->NAD83(2011) transformation applied afterwards.")
    args = ap.parse_args(argv)

    occupation_mode = "latest" if args.datasheet else args.occupation
    no_periodic = args.no_periodic or args.datasheet

    # Epoch.
    try:
        decimal_year = parse_epoch_argument(args.epoch, args.date, args.doy)
    except ValueError as e:
        ap.error(str(e))
        return 2

    # Station keys.
    keys = load_station_keys(args.stations, args.stations_file)
    if not keys:
        ap.error("no stations given (positional args or --stations-file)")
        return 2

    # SIF files.
    sif_paths = args.sif if args.sif else default_sif_paths()
    if not sif_paths:
        ap.error("no SIF files found; pass --sif PATH [PATH ...] or set $SIF_PATHS")
        return 2

    registry = SifRegistry()
    for p in sif_paths:
        if not Path(p).exists():
            print(f"ERROR: SIF not found: {p}", file=sys.stderr)
            return 2
        n = registry.load(p)
        print(f"# loaded {n} stations from {p}", file=sys.stderr)
    print(f"# {WGS84_NOTE}", file=sys.stderr)

    # Resolve.
    records: List[dict] = []
    missing: List[str] = []
    for key in keys:
        st = registry.find(key)
        if st is None:
            missing.append(key)
            records.append(build_record(key, None, None, decimal_year,
                                        with_covariance=not args.no_covariance))
            continue
        rc = resolve_station_coordinate(
            st, decimal_year,
            with_periodic=not no_periodic,
            with_psd=not args.no_psd,
            occupation=occupation_mode,
        )
        records.append(build_record(key, st, rc, decimal_year,
                                    with_covariance=not args.no_covariance))

    # Output.
    if args.output:
        out_stream = io.open(args.output, "w", encoding="utf-8", newline="")
        close = True
    else:
        out_stream = sys.stdout
        close = False
    try:
        if args.format == "json":
            write_json(records, out_stream, decimal_year, [str(p) for p in sif_paths])
        else:
            write_csv(records, out_stream)
    finally:
        if close:
            out_stream.close()

    if missing:
        print(f"# WARNING: {len(missing)} station(s) not found in the SIF(s): "
              f"{', '.join(missing)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
