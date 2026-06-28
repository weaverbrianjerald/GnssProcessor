"""Independent position-truth sources for the regression / A-B test suite.

A :class:`TruthSource` maps ``(station, epoch)`` to a truth ECEF coordinate,
propagated to the epoch with the published station velocity.  It is the *grader*
the harness compares a PPP / PPP-RTK solution against.

Backends
--------
``SinexTruth`` (PRIMARY)
    Parses an IGS / ITRF **SINEX** (``.snx``) or station **SSC**: the
    ``SOLUTION/ESTIMATE`` block's ``STAX/STAY/STAZ`` (+ ``VELX/VELY/VELZ``)
    rows.  Independent by construction -- a SINEX/SSC is consumed *nowhere* in
    the C++ engine, so a frame or site-displacement bug in the engine cannot
    hide inside the grader.  A single ITRF20xx/IGS20 SSC covers every IGS
    station globally; velocity propagation does the rest.

``SifTruth`` (FALLBACK + CROSS-CHECK)
    Reads an NGS **Station Info File** -- the same ``*_SIF.json`` the C++
    ``StationInfoFile`` loader consumes -- and is a faithful Python port of
    ``resolveStationCoordinate`` (linear + optional periodic + post-seismic).
    Used for stations absent from the SINEX, and as a cross-check: SINEX-vs-SIF
    agreement on the linear part validates both readers.

``CompositeTruth``
    Tries SINEX first, then SIF.

Frame / epoch caveats (see ``docs/test_suite.md``)
--------------------------------------------------
* The truth datum (ITRF20xx / IGS20) must match the frame of the precise
  products the run used.  COD/IGS final MGEX products are IGS20-aligned, so
  compare against an ITRF2020/IGS20 SSC for a mm-level result.
* SINEX gives the **secular** (regularized) position + velocity.  ``SifTruth``
  with ``with_periodic=True`` *adds* the empirical seasonal (loading) term --
  i.e. the *actual* antenna position at epoch, which is what a daily static PPP
  estimates.  The two therefore differ by the seasonal displacement
  (mm-to-cm).  For A-B diffing on a fixed date the choice cancels; for absolute
  accuracy it does not -- pick deliberately.
* The SSC coordinate is the monument reference (marker/ARP).  The engine must
  reduce its phase-center solution to the same point via the RINEX antenna
  height + ``igs20.atx`` PCO, or a constant vertical bias appears.
"""

from __future__ import annotations

import json
import math
import re
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np


# ---------------------------------------------------------------------------
# Result type
# ---------------------------------------------------------------------------

@dataclass
class TruthCoord:
    """A truth coordinate, propagated to a query epoch.

    Attributes
    ----------
    ecef
        ``np.ndarray`` shape ``(3,)`` in metres.
    epoch_year
        Decimal year the coordinate was propagated to.
    frame
        Reference frame string (e.g. ``"IGS20"`` / ``"ITRF2020"``), best-effort.
    source
        Provenance tag, e.g. ``"sinex:CIT1"`` or ``"sif:CIT100USA"``.
    sigma
        Optional ``(3,)`` 1-sigma ECEF uncertainty in metres (datum prior).
    """

    ecef: np.ndarray
    epoch_year: float
    frame: str = ""
    source: str = ""
    sigma: Optional[np.ndarray] = None


# ---------------------------------------------------------------------------
# Interface
# ---------------------------------------------------------------------------

class TruthSource(ABC):
    """Maps a station id + decimal-year epoch to a :class:`TruthCoord`."""

    @abstractmethod
    def resolve(self, station: str, decimal_year: float) -> Optional[TruthCoord]:
        """Return the truth coordinate, or ``None`` if the station is unknown."""
        raise NotImplementedError


# ---------------------------------------------------------------------------
# SINEX / SSC backend (primary)
# ---------------------------------------------------------------------------

# A SINEX SOLUTION/ESTIMATE ref-epoch token: "YY:DDD:SSSSS" (e.g. "15:001:00000").
_SNX_EPOCH_RE = re.compile(r"^\d{2}:\d{3}:\d{5}$")


def _sinex_epoch_to_year(token: str) -> Optional[float]:
    """Decode a SINEX ``YY:DDD:SSSSS`` epoch token to a decimal year.

    Returns ``None`` for the ``00:000:00000`` "no epoch" sentinel.  The 2-digit
    year follows the SINEX pivot: ``00-50 -> 2000-2050``, ``51-99 -> 1951-1999``.
    """
    yy, ddd, sssss = (int(p) for p in token.split(":"))
    if yy == 0 and ddd == 0 and sssss == 0:
        return None
    year = 2000 + yy if yy <= 50 else 1900 + yy
    leap = (year % 4 == 0 and (year % 100 != 0 or year % 400 == 0))
    diy = 366.0 if leap else 365.0
    # SINEX day-of-year is 1-based (001 = Jan 1).
    return year + ((ddd - 1) + sssss / 86400.0) / diy


@dataclass
class _SnxSoln:
    """One piecewise-linear solution segment for a station."""
    pos: List[Optional[float]] = field(default_factory=lambda: [None, None, None])
    vel: List[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    pos_std: List[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    epoch: Optional[float] = None       # ref epoch of the STA* rows
    start_year: Optional[float] = None  # validity window (SOLUTION/EPOCHS)
    end_year: Optional[float] = None


_AXIS = {"X": 0, "Y": 1, "Z": 2}


class SinexTruth(TruthSource):
    """Truth from an IGS / ITRF SINEX or SSC.

    Parses the ``SOLUTION/ESTIMATE`` ``STA*``/``VEL*`` parameters and the
    ``SOLUTION/EPOCHS`` validity windows, keyed by 4-character SINEX ``CODE`` and
    solution number.  ITRF realizations are piecewise-linear across station
    discontinuities (several ``SOLN``s per station, each its own
    position+velocity at the SINEX reference epoch).  :meth:`resolve` picks the
    ``SOLN`` whose window covers the query epoch and *extrapolates off the last
    SOLN beyond the solution span* -- the standard ITRF usage for current
    epochs, since e.g. ITRF2020 windows end ~2021.0.  A 9-char IGS long name
    (``CIT100USA``) resolves on its first four characters.
    """

    def __init__(self, path, frame: str = "", default_epoch: float = 2015.0):
        self.path = Path(path)
        self.default_epoch = float(default_epoch)
        # code -> {soln_int: _SnxSoln}
        # code -> {pt: {soln_int: _SnxSoln}}
        self._stations: Dict[str, Dict[str, Dict[int, _SnxSoln]]] = {}
        self.frame = frame
        self._parse()
        if not self.frame:   # fall back to the datum in the filename
            m = re.search(r"(ITRF\d{4}|IGS\d{2}|IGB\d{2})", self.path.name.upper())
            if m:
                self.frame = m.group(1)

    # -- parsing -----------------------------------------------------------
    def _parse(self) -> None:
        # SINEX/SSC are 7-bit ASCII; latin1 avoids decode errors on stray bytes.
        text = self.path.read_text(encoding="latin1", errors="replace")
        if not self.frame:
            self.frame = self._sniff_frame(text)
        block = None
        for raw in text.splitlines():
            if raw.startswith("+"):
                tail = raw[1:].strip()
                block = tail.split()[0].upper() if tail else None
                continue
            if raw.startswith("-"):
                block = None
                continue
            if not raw.strip() or raw.startswith("*"):
                continue
            if block == "SOLUTION/EPOCHS":
                self._parse_epoch_row(raw)
            elif block == "SOLUTION/ESTIMATE":
                self._parse_estimate_row(raw)

    @staticmethod
    def _sniff_frame(text: str) -> str:
        """Best-effort frame from header comments (ITRFyyyy / IGSnn)."""
        head = text[:4000].upper()
        m = re.search(r"(ITRF\d{4}|IGS\d{2}|IGB\d{2})", head)
        return m.group(1) if m else ""

    def _soln(self, code: str, pt: str, soln: int) -> _SnxSoln:
        return (self._stations.setdefault(code, {})
                              .setdefault(pt, {})
                              .setdefault(soln, _SnxSoln()))

    def _parse_epoch_row(self, line: str) -> None:
        # CODE PT SOLN T START END MEAN
        toks = line.split()
        if len(toks) < 6:
            return
        code = toks[0].upper()
        pt = toks[1].upper()
        try:
            soln = int(toks[2])
        except ValueError:
            return
        s = self._soln(code, pt, soln)
        if _SNX_EPOCH_RE.match(toks[4]):
            s.start_year = _sinex_epoch_to_year(toks[4])
        if _SNX_EPOCH_RE.match(toks[5]):
            s.end_year = _sinex_epoch_to_year(toks[5])

    def _parse_estimate_row(self, line: str) -> None:
        # INDEX TYPE CODE PT SOLN REF_EPOCH UNIT S VALUE STD
        toks = line.split()
        if len(toks) < 8:
            return
        ptype = toks[1].upper()           # STAX / VELX / ...
        if len(ptype) != 4 or ptype[:3] not in ("STA", "VEL"):
            return
        axis = _AXIS.get(ptype[3])
        if axis is None:
            return
        code = toks[2].upper()
        pt = toks[3].upper()
        try:
            soln = int(toks[4])
        except ValueError:
            soln = 1
        # ref-epoch is the token in YY:DDD:SSSSS form; value/std are the last two.
        epoch_tok = next((t for t in toks if _SNX_EPOCH_RE.match(t)), None)
        try:
            value = float(toks[-2].replace("D", "E").replace("d", "e"))
            std = float(toks[-1].replace("D", "E").replace("d", "e"))
        except ValueError:
            return
        s = self._soln(code, pt, soln)
        if ptype.startswith("STA"):
            s.pos[axis] = value
            s.pos_std[axis] = std
            if epoch_tok is not None and s.epoch is None:
                s.epoch = _sinex_epoch_to_year(epoch_tok)
        else:  # VEL*
            s.vel[axis] = value

    @staticmethod
    def _select_soln(solns: Dict[int, _SnxSoln], year: float) -> Optional[_SnxSoln]:
        complete = {k: v for k, v in solns.items()
                    if all(c is not None for c in v.pos)}
        if not complete:
            return None
        # 1) the SOLN whose window covers the epoch
        for k in sorted(complete):
            v = complete[k]
            if (v.start_year is not None and v.end_year is not None
                    and v.start_year <= year < v.end_year):
                return v
        # 2) future extrapolation -> latest-ending SOLN; past -> earliest-starting
        ends = [(v.end_year, k) for k, v in complete.items() if v.end_year is not None]
        starts = [(v.start_year, k) for k, v in complete.items()
                  if v.start_year is not None]
        if ends and year >= max(e for e, _ in ends):
            return complete[max(ends)[1]]
        if starts and year < min(s for s, _ in starts):
            return complete[min(starts)[1]]
        # 3) no usable windows -> highest SOLN number (most recent)
        return complete[max(complete)]

    # -- interface ---------------------------------------------------------
    def resolve(self, station: str, decimal_year: float) -> Optional[TruthCoord]:
        code = station.strip().upper()[:4]
        pts = self._stations.get(code)
        if not pts:
            return None
        # Prefer the primary monument point 'A'; else the first PT present.
        pt = "A" if "A" in pts else sorted(pts)[0]
        s = self._select_soln(pts[pt], decimal_year)
        if s is None:
            return None
        e0 = s.epoch if s.epoch is not None else self.default_epoch
        dt = decimal_year - e0
        pos = np.array([s.pos[0], s.pos[1], s.pos[2]], dtype=float)
        ecef = pos + np.array(s.vel, dtype=float) * dt
        return TruthCoord(ecef=ecef, epoch_year=decimal_year, frame=self.frame,
                          source=f"sinex:{code}", sigma=np.array(s.pos_std, dtype=float))

    def stations(self) -> List[str]:
        return sorted(self._stations.keys())


# ---------------------------------------------------------------------------
# SIF backend (fallback + cross-check) -- port of resolveStationCoordinate
# ---------------------------------------------------------------------------

_MM = 1.0e-3  # mm -> m


def _decimal_year_from_sif(s: str) -> float:
    """Port of ``decimalYearFromSif``: ``"YYYY-MM-DDTHH:MM:SS TZ"`` -> decimal yr.

    Timezone suffix is ignored (<=18 s GPS-UTC offset is far below the precision
    that matters here).  An open-ended END (year >= 9000) maps to ``9999.0``.
    """
    m = re.match(r"\s*(\d+)-(\d+)-(\d+)(?:T(\d+):(\d+):(\d+))?", s)
    if not m:
        return 0.0
    Y, M, D = int(m.group(1)), int(m.group(2)), int(m.group(3))
    h = int(m.group(4) or 0); mi = int(m.group(5) or 0); se = int(m.group(6) or 0)
    if Y >= 9000:
        return 9999.0
    if M < 1 or M > 12:
        M = 1
    if D < 1:
        D = 1
    cum = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)
    leap = (Y % 4 == 0 and (Y % 100 != 0 or Y % 400 == 0))
    doy = cum[M - 1] + (D - 1) + (1 if (leap and M > 2) else 0)
    diy = 366.0 if leap else 365.0
    return Y + (doy + (h * 3600.0 + mi * 60.0 + se) / 86400.0) / diy


def _enu_delta_to_ecef(e: float, n: float, u: float,
                       lat_rad: float, lon_rad: float) -> np.ndarray:
    """Local ENU offset -> ECEF offset (transpose of the ENU rotation)."""
    sl, cl = math.sin(lat_rad), math.cos(lat_rad)
    so, co = math.sin(lon_rad), math.cos(lon_rad)
    dx = -so * e - sl * co * n + cl * co * u
    dy = co * e - sl * so * n + cl * so * u
    dz = cl * n + sl * u
    return np.array([dx, dy, dz], dtype=float)


@dataclass
class _SifOccupation:
    start_year: float
    end_year: float
    coords: np.ndarray      # m, at the SIF reference epoch
    velocity: np.ndarray    # m/yr
    coord_sigmas: np.ndarray
    do_not_use: bool


@dataclass
class _SifPeriodicFreq:
    period_yr: float
    cos: np.ndarray  # (x,y,z) m
    sin: np.ndarray


@dataclass
class _SifPeriodicBlock:
    start_year: float
    end_year: float
    ref_epoch: float
    freqs: List[_SifPeriodicFreq]


@dataclass
class _SifPsdComp:
    aexp: List[float]
    texp: List[float]
    alog: List[float]
    tlog: List[float]


@dataclass
class _SifPsdBlock:
    event_year: float
    east: _SifPsdComp
    north: _SifPsdComp
    up: _SifPsdComp


@dataclass
class _SifStation:
    sid: str
    domes: str
    frame: str
    ref_epoch: float
    occupations: List[_SifOccupation]
    periodic: List[_SifPeriodicBlock]
    psd: List[_SifPsdBlock]


def _ecef_to_llh_rad(ecef: np.ndarray) -> Tuple[float, float]:
    """WGS84 ECEF -> (lat_rad, lon_rad); height not needed for the ENU rotation."""
    a = 6378137.0
    f = 1.0 / 298.257223563
    e2 = f * (2.0 - f)
    x, y, z = float(ecef[0]), float(ecef[1]), float(ecef[2])
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1.0 - e2))
    for _ in range(6):
        sl = math.sin(lat)
        n = a / math.sqrt(1.0 - e2 * sl * sl)
        h = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1.0 - e2 * n / (n + h)))
    return lat, lon


class SifTruth(TruthSource):
    """Truth from one or more NGS Station Info Files (``*_SIF.json``).

    Faithful port of the C++ ``resolveStationCoordinate``: linear term
    ``coords + velocity*(t - ref_epoch)`` plus optional periodic (seasonal) and
    post-seismic deformation.  ``ref_epoch`` is the file-level
    ``METADATA.REFERENCE_EPOCH`` (2020.0 for IGS20), to which *all* occupation
    ``COORDINATES`` are referenced.
    """

    def __init__(self, paths, with_periodic: bool = True, with_psd: bool = True):
        if isinstance(paths, (str, Path)):
            paths = [paths]
        self.with_periodic = with_periodic
        self.with_psd = with_psd
        self._by_id: Dict[str, _SifStation] = {}
        self._by_id4: Dict[str, _SifStation] = {}
        self._by_domes: Dict[str, _SifStation] = {}
        for p in paths:
            self._load(Path(p))

    # -- loading -----------------------------------------------------------
    def _load(self, path: Path) -> None:
        doc = json.loads(path.read_text(encoding="utf-8-sig"))
        meta = doc.get("METADATA", {}) or {}
        meta_epoch = float(meta.get("REFERENCE_EPOCH", 2020.0))
        meta_frame = str(meta.get("REFERENCE_FRAME", "") or "")
        for sj in doc.get("STATIONS", []) or []:
            sid = str(sj.get("ID", "") or "").strip()
            if not sid:
                continue
            st = _SifStation(
                sid=sid.upper(),
                domes=str(sj.get("DOMES", "") or "").strip().upper(),
                frame=meta_frame,
                ref_epoch=meta_epoch,
                occupations=self._read_occupations(sj),
                periodic=self._read_periodic(sj, meta_epoch) if self.with_periodic else [],
                psd=self._read_psd(sj) if self.with_psd else [],
            )
            self._by_id.setdefault(st.sid, st)
            if len(st.sid) >= 4:
                self._by_id4.setdefault(st.sid[:4], st)
            if st.domes:
                self._by_domes.setdefault(st.domes, st)

    @staticmethod
    def _vec3(j, key) -> np.ndarray:
        v = j.get(key)
        if isinstance(v, list) and len(v) >= 3:
            return np.array([float(v[0]), float(v[1]), float(v[2])], dtype=float)
        return np.zeros(3, dtype=float)

    def _read_occupations(self, sj) -> List[_SifOccupation]:
        out: List[_SifOccupation] = []
        for oj in sj.get("OCCUPATION", []) or []:
            out.append(_SifOccupation(
                start_year=_decimal_year_from_sif(str(oj.get("START", ""))),
                end_year=_decimal_year_from_sif(str(oj.get("END", ""))),
                coords=self._vec3(oj, "COORDINATES"),
                velocity=self._vec3(oj, "VELOCITY") * _MM,  # mm/yr -> m/yr
                coord_sigmas=self._vec3(oj, "COORDINATE_SIGMAS"),
                do_not_use=bool(oj.get("DO_NOT_USE", False)),
            ))
        return out

    def _read_periodic(self, sj, meta_epoch) -> List[_SifPeriodicBlock]:
        nlt = sj.get("NONLINEAR_TERMS", {}) or {}
        out: List[_SifPeriodicBlock] = []
        for bj in nlt.get("PERIODIC_TERMS", []) or []:
            freqs: List[_SifPeriodicFreq] = []
            for fj in bj.get("FREQUENCIES", []) or []:
                freqs.append(_SifPeriodicFreq(
                    period_yr=float(fj.get("PERIOD", 1.0) or 1.0),
                    cos=_MM * np.array([float(fj.get("X_COS", 0.0)),
                                        float(fj.get("Y_COS", 0.0)),
                                        float(fj.get("Z_COS", 0.0))]),
                    sin=_MM * np.array([float(fj.get("X_SIN", 0.0)),
                                        float(fj.get("Y_SIN", 0.0)),
                                        float(fj.get("Z_SIN", 0.0))]),
                ))
            out.append(_SifPeriodicBlock(
                start_year=_decimal_year_from_sif(str(bj.get("START", ""))),
                end_year=_decimal_year_from_sif(str(bj.get("END", ""))),
                ref_epoch=meta_epoch,
                freqs=freqs,
            ))
        return out

    def _read_psd(self, sj) -> List[_SifPsdBlock]:
        nlt = sj.get("NONLINEAR_TERMS", {}) or {}
        out: List[_SifPsdBlock] = []

        def comp(cj) -> _SifPsdComp:
            cj = cj or {}
            flt = lambda k: [float(x) for x in (cj.get(k, []) or [])]
            return _SifPsdComp(aexp=flt("AEXP"), texp=flt("TEXP"),
                               alog=flt("ALOG"), tlog=flt("TLOG"))

        for pj in nlt.get("POST_SEISMIC", []) or []:
            out.append(_SifPsdBlock(
                event_year=_decimal_year_from_sif(str(pj.get("EVENT_EPOCH", ""))),
                east=comp(pj.get("EAST")),
                north=comp(pj.get("NORTH")),
                up=comp(pj.get("UP")),
            ))
        return out

    # -- resolution --------------------------------------------------------
    def _find(self, station: str) -> Optional[_SifStation]:
        k = station.strip().upper()
        if not k:
            return None
        if k in self._by_id:
            return self._by_id[k]
        if len(k) >= 4 and k[:4] in self._by_id4:
            return self._by_id4[k[:4]]
        return self._by_domes.get(k)

    @staticmethod
    def _select_occupation(st: _SifStation, year: float) -> Optional[_SifOccupation]:
        best, best_dist = None, float("inf")
        for o in st.occupations:
            if o.do_not_use:
                continue
            if o.start_year <= year < o.end_year:
                return o
            mid = 0.5 * (o.start_year + min(o.end_year, 9000.0))
            dist = abs(year - mid)
            if dist < best_dist:
                best, best_dist = o, dist
        return best

    def resolve(self, station: str, decimal_year: float) -> Optional[TruthCoord]:
        st = self._find(station)
        if st is None:
            return None
        occ = self._select_occupation(st, decimal_year)
        if occ is None:
            return None

        dt = decimal_year - st.ref_epoch
        ecef = occ.coords + occ.velocity * dt

        if self.with_periodic:
            for b in st.periodic:
                if decimal_year < b.start_year or decimal_year >= b.end_year:
                    continue
                dT = decimal_year - b.ref_epoch
                for f in b.freqs:
                    if f.period_yr == 0.0:
                        continue
                    w = 2.0 * math.pi / f.period_yr
                    ecef = ecef + f.cos * math.cos(w * dT) + f.sin * math.sin(w * dT)

        if self.with_psd and st.psd:
            def eval_comp(c: _SifPsdComp, dT: float) -> float:
                d = 0.0
                for a, t in zip(c.alog, c.tlog):
                    if t > 0.0:
                        d += a * math.log(1.0 + dT / t)
                for a, t in zip(c.aexp, c.texp):
                    if t > 0.0:
                        d += a * (1.0 - math.exp(-dT / t))
                return d

            enu = np.zeros(3, dtype=float)
            for pb in st.psd:
                dT = decimal_year - pb.event_year
                if dT <= 0.0:
                    continue
                enu[0] += eval_comp(pb.east, dT)
                enu[1] += eval_comp(pb.north, dT)
                enu[2] += eval_comp(pb.up, dT)
            if float(enu @ enu) > 0.0:
                lat, lon = _ecef_to_llh_rad(ecef)
                ecef = ecef + _enu_delta_to_ecef(enu[0], enu[1], enu[2], lat, lon)

        sigma = occ.coord_sigmas if float(occ.coord_sigmas @ occ.coord_sigmas) > 0 else None
        return TruthCoord(ecef=ecef, epoch_year=decimal_year, frame=st.frame,
                          source=f"sif:{st.sid}", sigma=sigma)

    def stations(self) -> List[str]:
        return sorted(self._by_id.keys())


# ---------------------------------------------------------------------------
# Composite (SINEX primary, SIF fallback)
# ---------------------------------------------------------------------------

class CompositeTruth(TruthSource):
    """Try each backend in order; return the first hit.  ``None`` if all miss."""

    def __init__(self, sources: Sequence[TruthSource]):
        self.sources = list(sources)

    def resolve(self, station: str, decimal_year: float) -> Optional[TruthCoord]:
        for src in self.sources:
            tc = src.resolve(station, decimal_year)
            if tc is not None:
                return tc
        return None


def make_default_truth(sinex_paths: Optional[Sequence] = None,
                       sif_paths: Optional[Sequence] = None,
                       sif_kwargs: Optional[dict] = None) -> CompositeTruth:
    """Build the harness default: SINEX/SSC primary, SIF fallback.

    Any SINEX path that does not exist is silently skipped, so the composite
    degrades to SIF-only until a real SSC is dropped into
    ``data/static/sinex/``.

    Parameters
    ----------
    sinex_paths, sif_paths
        Iterables of file paths.  ``None`` -> empty.
    sif_kwargs
        Forwarded to :class:`SifTruth` (e.g. ``with_periodic``, ``with_psd``).
    """
    sources: List[TruthSource] = []
    for p in (sinex_paths or []):
        if Path(p).is_file():
            snx = SinexTruth(p)
            if snx.stations():      # a file that parsed to 0 stations is useless
                sources.append(snx)
    sif_existing = [p for p in (sif_paths or []) if Path(p).is_file()]
    if sif_existing:
        sources.append(SifTruth(sif_existing, **(sif_kwargs or {})))
    return CompositeTruth(sources)
