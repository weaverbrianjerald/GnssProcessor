"""Independent position-truth sources for the regression / A-B test suite.

A :class:`TruthSource` maps ``(station, epoch)`` to a truth ECEF coordinate,
propagated to the epoch with the published station velocity.  It is the *grader*
the harness compares a PPP / PPP-RTK solution against.

Backends
--------
``SinexTruth`` (primary)
    Parses an IGS / ITRF **SINEX** (``.snx``) or station **SSC**: the
    ``SOLUTION/ESTIMATE`` block's ``STAX/STAY/STAZ`` (+ ``VELX/VELY/VELZ``)
    rows.  Independent by construction: the C++ engine does not read SINEX/SSC
    anywhere, so a frame or site-displacement bug in the engine cannot also
    be present in the grader.  A single ITRF20xx/IGS20 SSC covers every IGS
    station globally; velocity propagation does the rest.

``SifTruth`` (fallback and cross-check)
    Reads an NGS **Station Info File** (the same ``*_SIF.json`` the C++
    ``StationInfoFile`` loader consumes) and is a Python port of
    ``resolveStationCoordinate`` (linear + optional periodic + post-seismic).
    Used for stations absent from the SINEX, and as a cross-check: SINEX-vs-SIF
    agreement on the linear part validates both readers.

``SiteTruth``
    A frozen JSON site database for receivers that are in neither a SINEX nor
    a SIF.

``CompositeTruth``
    Tries each backend in order; the harness default
    (:func:`make_default_truth`) is SINEX, then SIF, then the site database.

Frame / epoch caveats (see ``docs/test_suite.md``)
--------------------------------------------------
* The truth datum (ITRF20xx / IGS20) must match the frame of the precise
  products the run used.  COD/IGS final MGEX products are IGS20-aligned, so
  compare against an ITRF2020/IGS20 SSC for a mm-level result.
* SINEX gives the **secular** (regularized) position + velocity.  ``SifTruth``
  with ``with_periodic=True`` *adds* the empirical seasonal (loading) term,
  i.e. gives the *actual* antenna position at epoch, which is what a daily
  static PPP estimates.  The two therefore differ by the seasonal displacement
  (mm-to-cm).  For A-B diffing on a fixed date the choice cancels; for absolute
  accuracy it does not, so the choice has to be made explicitly.
* The SSC coordinate is the monument reference (marker/ARP).  The engine must
  reduce its phase-center solution to the same point via the RINEX antenna
  height + ``igs20.atx`` PCO, or a constant vertical bias appears.
"""

from __future__ import annotations

import json
import math
import re
import warnings
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
    extrapolated
        True when no usable solution window covers ``epoch_year``, so the
        coordinate was propagated off a segment that had already ended (or not
        yet started).  Set by :class:`SifTruth` only; other backends leave it
        False.
    extrapolation_years
        Signed years outside the selected segment: ``t - end`` past its end,
        ``t - start`` (negative) before its start, ``0.0`` when covered.
    segment_start_year, segment_end_year
        Window of the solution segment that produced the coordinate (an open
        end reads ``9999.0``); ``None`` for backends that do not report one.
    antenna_change_in_gap
        True when an antenna change (type incl. radome, or serial number)
        starts between the segment boundary and ``epoch_year``, i.e. the
        extrapolation crosses a hardware change that typically ends a segment.
    receiver_change_in_gap
        As ``antenna_change_in_gap`` for a receiver type change.
    """

    ecef: np.ndarray
    epoch_year: float
    frame: str = ""
    source: str = ""
    sigma: Optional[np.ndarray] = None
    extrapolated: bool = False
    extrapolation_years: float = 0.0
    segment_start_year: Optional[float] = None
    segment_end_year: Optional[float] = None
    antenna_change_in_gap: bool = False
    receiver_change_in_gap: bool = False


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
    ``SOLUTION/EPOCHS`` validity windows, keyed by 4-character SINEX ``CODE``,
    point code ``PT`` (``A`` preferred) and solution number.  ITRF realizations
    are piecewise-linear across station discontinuities (several ``SOLN``s per
    station, each its own position+velocity at the SINEX reference epoch).
    :meth:`resolve` picks the ``SOLN`` whose window covers the query epoch and,
    beyond the solution span, *extrapolates off the last SOLN* (off the first
    one before it), the standard ITRF usage for current epochs, since e.g.
    ITRF2020 windows end ~2021.0.  A 9-char IGS long name (``CIT100USA``)
    resolves on its first four characters.
    """

    def __init__(self, path, frame: str = "", default_epoch: float = 2015.0):
        self.path = Path(path)
        self.default_epoch = float(default_epoch)
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

#: A 4-char prefix match is accepted only within this 3D distance [m] of the
#: caller's approximate position (when one is supplied).  A real rename keeps
#: the monument (``STHL00GBR`` -> ``STHL00SHN``, 0 km); a code reused elsewhere
#: does not (``GAMB00PYF`` vs ``GAMB00USA``, 7,688 km).
SIF_MAX_PREFIX_DISTANCE_M = 10000.0

#: Slack [yr] around a segment boundary when looking for an antenna / receiver
#: change in an extrapolation gap: the hardware log and the coordinate segment
#: rarely break at the same second (CHUR00CAN: receiver 23:43, segment 23:59:30).
SIF_GAP_TOLERANCE_YR = 2.0 / 365.25

# An ECEF vector shorter than this is a placeholder (zeros / missing), not a position.
_MIN_ECEF_NORM_M = 1.0e6


class SifPrefixRejectedWarning(UserWarning):
    """A 4-char prefix candidate was refused (distance, or an unconfirmed long key)."""


class SifExtrapolationWarning(UserWarning):
    """A SIF coordinate was propagated outside every usable occupation window."""


def _usable_ecef(v: Optional[np.ndarray]) -> bool:
    """True for a finite ECEF vector of plausible length (norm > 1e6 m)."""
    if v is None:
        return False
    v = np.asarray(v, dtype=float)
    return (v.shape == (3,) and bool(np.all(np.isfinite(v)))
            and float(np.linalg.norm(v)) > _MIN_ECEF_NORM_M)


def _decimal_year_from_sif(s: str) -> float:
    """Port of ``decimalYearFromSif``: ``"YYYY-MM-DDTHH:MM:SS TZ"`` -> decimal yr.

    Timezone suffix is ignored (<=18 s GPS-UTC offset is far below the precision
    that matters here).  An open-ended end (year >= 9000) maps to ``9999.0``.
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
    coords: np.ndarray      # m, at ref_epoch
    velocity: np.ndarray    # m/yr
    coord_sigmas: np.ndarray
    do_not_use: bool
    ref_epoch: float        # OCCUPATION.REFERENCE_EPOCH, else METADATA.REFERENCE_EPOCH


@dataclass
class _SifEquipWindow:
    """One ANTENNA or RECEIVER history window (only what the gap test reads)."""
    start_year: float
    type: str               # trimmed; antenna TYPE includes the radome
    serial: str             # trimmed


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
    ref_epoch: float                  # METADATA.REFERENCE_EPOCH (occupation fallback)
    occupations: List[_SifOccupation]
    periodic: List[_SifPeriodicBlock]
    psd: List[_SifPsdBlock]
    approx: Optional[np.ndarray] = None                       # APPROXIMATE_COORDINATES, m
    antennas: List[_SifEquipWindow] = field(default_factory=list)   # sorted by START
    receivers: List[_SifEquipWindow] = field(default_factory=list)  # sorted by START


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

    Port of the C++ ``resolveStationCoordinate``: linear term
    ``coords + velocity*(t - ref_epoch)`` plus optional periodic (seasonal) and
    post-seismic deformation.  ``ref_epoch`` is the selected occupation's own
    ``OCCUPATION.REFERENCE_EPOCH``, falling back to the file-level
    ``METADATA.REFERENCE_EPOCH`` (2020.0 for IGS20) when the record has none.
    Almost every occupation sits at 2020.0, but a few NCN records
    (``AZAZ00USA``, ``AZOR00USA``, ``AZPL00USA``, ``FLSI00USA``) are at 2010.0;
    propagating those from 2020.0 would misplace them by ten years of velocity
    (11-15 cm).  The periodic terms keep the file-level epoch as their phase
    reference.

    Lookup (:meth:`_find`) tries the full 9-char ID, then the DOMES number, then
    the 4-char station prefix.  An exact ID or DOMES match is returned as is.  A
    prefix match is only a *candidate*: with an approximate position it must lie
    within ``SIF_MAX_PREFIX_DISTANCE_M`` of the candidate's reference position;
    without one, only a bare 4-char key (``"CIT1"``) is accepted.  A longer key
    that is neither an exact ID nor a DOMES says more than the code (monument,
    country), and nothing confirms the candidate agrees: ``GAMB00PYF`` is absent,
    and its prefix names ``GAMB00USA``, 7,688 km away.  Every refusal warns
    (:class:`SifPrefixRejectedWarning`).

    Occupation selection: the non-flagged window covering the epoch, else the
    non-flagged window with the nearest midpoint.  The second case extrapolates
    a closed segment past its end (or before its start); the coordinate is still
    returned unchanged, but :class:`TruthCoord` then carries
    ``extrapolated=True``, the years outside the segment and whether an antenna
    or receiver change starts in that gap, and :meth:`resolve` warns
    (:class:`SifExtrapolationWarning`).

    The periodic (seasonal) term is one block's displacement chosen by the epoch
    alone (:meth:`_select_periodic`, the NGS M-PAGES v1.2 rule): the block whose
    window covers the epoch, else the first block before the first window, the last
    block after the last, and the block that ended last inside a gap.  The sinusoids
    continue past the window on the block's own phase reference, so the term does
    not drop to zero where the linear term goes on; the other cases warn with
    :class:`SifExtrapolationWarning`.
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
                occupations=self._read_occupations(sj, meta_epoch),
                periodic=self._read_periodic(sj, meta_epoch) if self.with_periodic else [],
                psd=self._read_psd(sj) if self.with_psd else [],
                approx=self._vec3(sj, "APPROXIMATE_COORDINATES"),  # zeros if absent
                antennas=self._read_equipment(sj, "ANTENNA"),
                receivers=self._read_equipment(sj, "RECEIVER"),
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

    def _read_occupations(self, sj, meta_epoch: float) -> List[_SifOccupation]:
        out: List[_SifOccupation] = []
        for oj in sj.get("OCCUPATION", []) or []:
            # Each occupation's COORDINATES are at its own REFERENCE_EPOCH; only a
            # record without a numeric one inherits the file-level epoch.
            ep = oj.get("REFERENCE_EPOCH")
            ref_epoch = (float(ep) if isinstance(ep, (int, float))
                         and not isinstance(ep, bool) else meta_epoch)
            out.append(_SifOccupation(
                start_year=_decimal_year_from_sif(str(oj.get("START", ""))),
                end_year=_decimal_year_from_sif(str(oj.get("END", ""))),
                coords=self._vec3(oj, "COORDINATES"),
                velocity=self._vec3(oj, "VELOCITY") * _MM,  # mm/yr -> m/yr
                coord_sigmas=self._vec3(oj, "COORDINATE_SIGMAS"),
                do_not_use=bool(oj.get("DO_NOT_USE", False)),
                ref_epoch=ref_epoch,
            ))
        return out

    @staticmethod
    def _read_equipment(sj, key: str) -> List[_SifEquipWindow]:
        """``ANTENNA`` / ``RECEIVER`` history, stably sorted by START."""
        out = [_SifEquipWindow(
                   start_year=_decimal_year_from_sif(str(wj.get("START", ""))),
                   type=str(wj.get("TYPE", "") or "").strip(),
                   serial=str(wj.get("SERIAL_NUMBER", "") or "").strip())
               for wj in (sj.get(key, []) or [])]
        out.sort(key=lambda w: w.start_year)
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
    @staticmethod
    def _reference_position(st: _SifStation) -> Optional[np.ndarray]:
        """Position a prefix candidate is distance-checked against.

        ``APPROXIMATE_COORDINATES`` if usable, else the first non-flagged
        occupation's ``COORDINATES``, else the first occupation's; ``None`` when
        none of them is a usable ECEF.
        """
        if _usable_ecef(st.approx):
            return st.approx
        for o in st.occupations:
            if not o.do_not_use:
                if _usable_ecef(o.coords):
                    return o.coords
                break
        if st.occupations and _usable_ecef(st.occupations[0].coords):
            return st.occupations[0].coords
        return None

    def _find(self, station: str, approx_ecef: Optional[Sequence[float]] = None,
              max_prefix_distance_m: float = SIF_MAX_PREFIX_DISTANCE_M, *,
              stacklevel: int = 2) -> Optional[_SifStation]:
        """Look a station up: exact ID, then DOMES, then a checked 4-char prefix.

        Parameters
        ----------
        station
            9-char ID, DOMES number or 4-char code; case-insensitive, trimmed.
        approx_ecef
            Optional approximate ECEF [m] of the station the caller means.  When
            usable (exactly three finite components, norm > 1e6 m) a prefix
            candidate is accepted only within ``max_prefix_distance_m`` of its
            reference position.  Without it, a prefix candidate is accepted only
            for a bare 4-char key.  Exact ID and DOMES matches ignore it.
        max_prefix_distance_m
            Acceptance radius for a prefix candidate [m].
        stacklevel
            ``warnings.warn`` stack level of a refusal, counted from here: 2
            names the direct caller, 3 the caller's caller (:meth:`resolve`
            passes 3 so the warning names the same line as its own
            :class:`SifExtrapolationWarning`).

        Returns
        -------
        The station record, or ``None`` (unknown, or the prefix candidate was
        refused with a :class:`SifPrefixRejectedWarning`).
        """
        k = station.strip().upper()
        if not k:
            return None
        if k in self._by_id:
            return self._by_id[k]
        if k in self._by_domes:
            return self._by_domes[k]
        if len(k) < 4 or k[:4] not in self._by_id4:
            return None
        cand = self._by_id4[k[:4]]
        if _usable_ecef(approx_ecef):
            ref = self._reference_position(cand)
            if ref is None:
                warnings.warn(
                    f"SIF: prefix candidate {cand.sid} for '{k}' rejected: the "
                    f"candidate has no usable reference position to check "
                    f"against the supplied approximate position",
                    SifPrefixRejectedWarning, stacklevel=stacklevel)
                return None
            dist = float(np.linalg.norm(np.asarray(approx_ecef, dtype=float) - ref))
            if dist <= max_prefix_distance_m:
                return cand
            warnings.warn(
                f"SIF: prefix candidate {cand.sid} for '{k}' rejected: "
                f"{dist:.1f} m from the supplied approximate position "
                f"(limit {max_prefix_distance_m:.1f} m)",
                SifPrefixRejectedWarning, stacklevel=stacklevel)
            return None
        if len(k) == 4:
            return cand   # a bare 4-char code carries nothing to contradict the match
        warnings.warn(
            f"SIF: prefix candidate {cand.sid} for '{k}' rejected: '{k}' is not an "
            f"exact ID or DOMES and no approximate position was supplied to "
            f"confirm the 4-char match",
            SifPrefixRejectedWarning, stacklevel=stacklevel)
        return None

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

    @staticmethod
    def _change_in_gap(windows: List[_SifEquipWindow], lo: float, hi: float,
                       compare_serial: bool) -> bool:
        """True if a window after the first starts in ``[lo, hi]`` with a new
        TYPE (or, when ``compare_serial``, a new SERIAL_NUMBER) relative to the
        window before it.  A re-log that changes neither does not count."""
        for prev, w in zip(windows, windows[1:]):
            if lo <= w.start_year <= hi and (
                    w.type != prev.type or (compare_serial and w.serial != prev.serial)):
                return True
        return False

    @classmethod
    def _extrapolation(cls, st: _SifStation, occ: _SifOccupation,
                       year: float) -> Tuple[bool, float, bool, bool]:
        """``(extrapolated, extrapolation_years, antenna_change, receiver_change)``.

        The selected occupation covers ``year`` unless no non-flagged window
        does, so a non-covering selection is exactly the extrapolated case.  The
        gap runs from the segment boundary (widened by ``SIF_GAP_TOLERANCE_YR``)
        to the query epoch.
        """
        if occ.start_year <= year < occ.end_year:
            return False, 0.0, False, False
        if year >= occ.end_year:
            years = year - occ.end_year
            lo, hi = occ.end_year - SIF_GAP_TOLERANCE_YR, year
        else:
            years = year - occ.start_year
            lo, hi = year, occ.start_year + SIF_GAP_TOLERANCE_YR
        return (True, years,
                cls._change_in_gap(st.antennas, lo, hi, compare_serial=True),
                cls._change_in_gap(st.receivers, lo, hi, compare_serial=False))

    @staticmethod
    def _select_periodic(st: "_SifStation", year: float
                         ) -> Tuple[Optional["_SifPeriodicBlock"], str]:
        """The periodic block that supplies the seasonal term at ``year``, and the case.

        Port of ``selectPeriodicBlock`` in ``StationInfoFile.cpp`` (the NGS M-PAGES
        v1.2 rule).  Blocks are taken in start order (file order for equal starts).
        The first block whose window covers ``year`` (start <= year < end) wins, so
        overlapping windows are not summed.  With none covering: before the first
        window the first block (``"before_first"``), at or after the end of the last
        window the last block (``"after_last"``), inside a gap the last block, in
        start order, that ended at or before ``year`` (``"in_gap"``).  A station
        without a periodic block returns ``(None, "not_evaluated")``.
        """
        if not st.periodic:
            return None, "not_evaluated"
        ordered = sorted(st.periodic, key=lambda b: b.start_year)  # stable
        last_ended = None
        for b in ordered:
            if b.start_year <= year < b.end_year:
                return b, "covered"
            if b.end_year <= year:
                last_ended = b
        if year < ordered[0].start_year:
            return ordered[0], "before_first"
        if year >= ordered[-1].end_year:
            return ordered[-1], "after_last"
        return last_ended, "in_gap"

    def resolve(self, station: str, decimal_year: float, *,
                approx_ecef: Optional[Sequence[float]] = None) -> Optional[TruthCoord]:
        """Return the SIF coordinate of ``station`` at ``decimal_year``.

        ``approx_ecef`` (keyword-only, optional) lets a 4-char prefix match be
        confirmed by distance; see :meth:`_find`.  An epoch outside every usable
        occupation window still resolves, flagged ``extrapolated`` on the
        result and warned with a :class:`SifExtrapolationWarning`.
        """
        st = self._find(station, approx_ecef, stacklevel=3)
        if st is None:
            return None
        occ = self._select_occupation(st, decimal_year)
        if occ is None:
            return None

        extrapolated, extra_yr, ant_chg, rcv_chg = self._extrapolation(
            st, occ, decimal_year)
        if extrapolated:
            warnings.warn(
                f"SIF: {st.sid} at {decimal_year:.4f} is outside every usable "
                f"occupation window; extrapolating segment "
                f"[{occ.start_year:.4f}, {occ.end_year:.4f}) by {extra_yr:+.3f} yr "
                f"(antenna change in gap: {'yes' if ant_chg else 'no'}, "
                f"receiver change in gap: {'yes' if rcv_chg else 'no'})",
                SifExtrapolationWarning, stacklevel=2)

        dt = decimal_year - occ.ref_epoch
        ecef = occ.coords + occ.velocity * dt

        if self.with_periodic:
            blk, reach = self._select_periodic(st, decimal_year)
            if blk is not None:
                if reach != "covered":
                    warnings.warn(
                        f"SIF: {st.sid} at {decimal_year:.4f} is outside every periodic "
                        f"window ({reach.replace('_', ' ')}); continuing the seasonal term "
                        f"of block [{blk.start_year:.4f}, {blk.end_year:.4f})",
                        SifExtrapolationWarning, stacklevel=2)
                dT = decimal_year - blk.ref_epoch
                for f in blk.freqs:
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
                          source=f"sif:{st.sid}", sigma=sigma,
                          extrapolated=extrapolated, extrapolation_years=extra_yr,
                          segment_start_year=occ.start_year,
                          segment_end_year=occ.end_year,
                          antenna_change_in_gap=ant_chg,
                          receiver_change_in_gap=rcv_chg)

    def stations(self) -> List[str]:
        return sorted(self._by_id.keys())


# ---------------------------------------------------------------------------
# Site backend -- a frozen "site database" of externally-established coordinates
# ---------------------------------------------------------------------------

@dataclass
class _Site:
    ecef: np.ndarray            # m, at ref_epoch
    velocity: np.ndarray        # m/yr (zeros if unknown)
    ref_epoch: float            # decimal year the ecef is referenced to
    frame: str
    source: str                 # provenance tag (survey method / product set)
    sigma: Optional[np.ndarray] # (3,) 1-sigma ECEF, or None


class SiteTruth(TruthSource):
    """Truth from a frozen JSON *site database* of fixed-station coordinates.

    For receivers that are **not** IGS/CORS stations (so absent from any SINEX
    or SIF), e.g. a rooftop research receiver fed to the real-time engine, the
    truth coordinate is established once *out of band* (a long static-PPP
    survey-in, an official survey, or a published monument value) and frozen
    into a small JSON file that the engine does not read.  That keeps the
    grader independent in the same sense as :class:`SinexTruth`: a frame or
    site-displacement bug in the engine cannot also be present in the truth.

    File schema (``data/static/sites/*.json``)::

        {
          "frame": "ITRF2020",            // file-level default
          "ref_epoch": 2026.40,           // file-level default (decimal year)
          "sites": {
            "COVL": {
              "ecef_m": [x, y, z],        // required
              "velocity_m_yr": [vx,vy,vz],// optional (default 0 -> no propagation)
              "ref_epoch": 2026.40,       // optional (overrides file default)
              "sigma_m": [sx, sy, sz],    // optional 1-sigma
              "frame": "ITRF2020",        // optional (overrides file default)
              "source": "static PPP survey-in (COD MGX final), 2026-145",
              "aliases": ["COVL00USA"]    // optional extra lookup keys
            }
          }
        }

    Lookup is by exact key, then by any declared alias, then by 4-char prefix
    (so ``COVL00USA`` resolves ``COVL``).  ``resolve`` propagates the coordinate
    to the query epoch with the (usually zero) site velocity, mirroring
    :class:`SinexTruth`.
    """

    def __init__(self, paths):
        if isinstance(paths, (str, Path)):
            paths = [paths]
        self._sites: Dict[str, _Site] = {}        # canonical name -> _Site
        self._alias: Dict[str, str] = {}          # alias/prefix -> canonical
        for p in paths:
            self._load(Path(p))

    def _load(self, path: Path) -> None:
        doc = json.loads(path.read_text(encoding="utf-8-sig"))
        file_frame = str(doc.get("frame", "") or "")
        file_epoch = float(doc.get("ref_epoch", 2020.0))
        for name, sj in (doc.get("sites", {}) or {}).items():
            ecef = self._vec3(sj, "ecef_m")
            if ecef is None:
                continue                          # a site without a position is skipped
            vel = self._vec3(sj, "velocity_m_yr")
            if vel is None:
                vel = np.zeros(3, dtype=float)
            sig = self._vec3(sj, "sigma_m")
            site = _Site(
                ecef=ecef, velocity=vel,
                ref_epoch=float(sj.get("ref_epoch", file_epoch)),
                frame=str(sj.get("frame", file_frame) or file_frame),
                source=str(sj.get("source", "") or ""),
                sigma=sig,
            )
            key = name.strip().upper()
            self._sites.setdefault(key, site)
            if len(key) >= 4:
                self._alias.setdefault(key[:4], key)
            for a in sj.get("aliases", []) or []:
                ak = str(a).strip().upper()
                if ak:
                    self._alias.setdefault(ak, key)
                    if len(ak) >= 4:
                        self._alias.setdefault(ak[:4], key)

    @staticmethod
    def _vec3(j, key) -> Optional[np.ndarray]:
        v = j.get(key)
        if isinstance(v, list) and len(v) >= 3:
            return np.array([float(v[0]), float(v[1]), float(v[2])], dtype=float)
        return None

    def _find(self, station: str) -> Optional[_Site]:
        k = station.strip().upper()
        if not k:
            return None
        if k in self._sites:
            return self._sites[k]
        if k in self._alias:
            return self._sites[self._alias[k]]
        if len(k) >= 4 and k[:4] in self._alias:
            return self._sites[self._alias[k[:4]]]
        return None

    def resolve(self, station: str, decimal_year: float) -> Optional[TruthCoord]:
        s = self._find(station)
        if s is None:
            return None
        dt = decimal_year - s.ref_epoch
        ecef = s.ecef + s.velocity * dt
        return TruthCoord(ecef=ecef, epoch_year=decimal_year, frame=s.frame,
                          source=f"site:{station.strip().upper()}"
                                 + (f" ({s.source})" if s.source else ""),
                          sigma=s.sigma)

    def stations(self) -> List[str]:
        return sorted(self._sites.keys())


# ---------------------------------------------------------------------------
# Composite (SINEX primary, then SIF, then the site database)
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
                       sif_kwargs: Optional[dict] = None,
                       site_paths: Optional[Sequence] = None) -> CompositeTruth:
    """Build the harness default: SINEX/SSC primary, SIF, then site database.

    Any path that does not exist is skipped without a message, as is a SINEX or
    site file that parses to 0 stations, so the composite falls back (e.g. to
    SIF-only until a real SSC is placed in ``data/static/sinex/``).  Order is
    SINEX -> SIF -> :class:`SiteTruth`; the site database is last because its
    members (non-IGS stations) are disjoint from the global SSC/SIF coverage, so
    the precedence has no effect in practice, and keeping the published-network
    sources first is the safe default.

    Parameters
    ----------
    sinex_paths, sif_paths, site_paths
        Iterables of file paths.  ``None`` -> empty.
    sif_kwargs
        Forwarded to :class:`SifTruth` (e.g. ``with_periodic``, ``with_psd``).
    """
    sources: List[TruthSource] = []
    for p in (sinex_paths or []):
        if Path(p).is_file():
            snx = SinexTruth(p)
            if snx.stations():      # a file that parsed to 0 stations is skipped
                sources.append(snx)
    sif_existing = [p for p in (sif_paths or []) if Path(p).is_file()]
    if sif_existing:
        sources.append(SifTruth(sif_existing, **(sif_kwargs or {})))
    site_existing = [p for p in (site_paths or []) if Path(p).is_file()]
    if site_existing:
        site = SiteTruth(site_existing)
        if site.stations():
            sources.append(site)
    return CompositeTruth(sources)
