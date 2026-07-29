"""Parser for the PPP-RTK network product file (``network_products*.txt``).

This is the Python analogue of :class:`NetworkProductReader` in
``src/core/products/NetworkProductIO.cpp`` — a dependency-free reader for the
line-oriented, comma-separated text product the network producer emits (one file
per run, ``network.product_output_path``).  It re-derives the *exact* grammar
from that C++ writer/reader so a viewer can decode a file the producer wrote
without re-running the C++ side.

Grammar (see ``NetworkProductIO.cpp`` ``writeHeader_`` / ``write``)
------------------------------------------------------------------
* Banner comment ``# gnss_network_product v2`` (or ``v1``).  Lines starting with
  ``#`` are comments and are skipped **before** tag dispatch.  Trailing ``\r``
  (CRLF files) is stripped.
* ``E,week,tow,iod_ssr,provider_id,solution_id,datum_changed[,datum_consts]``
  starts a new epoch.  Field 8 ``datum_consts`` is a *concatenated* constellation
  string (e.g. ``"GE"``, NOT comma-separated), present only when the affected set
  is non-empty; it applies to the current epoch.
* ``G,week,tow`` — optional per-epoch wall-clock time-of-generation, only stamped
  runs emit it (carried onto the epoch, not a data row).
* ``R,sat[,sat...]`` — retro-bump satellite set, written right after ``E`` only
  when non-empty; applies to the current epoch.
* ``C,sat,clock_c0_m[,clock_c1_m_s]`` — field 4 (drift) only when != 0.
* ``B,sat,signal,code_bias_m``.
* ``P,sat,signal,bias_m,signal_integer,wide_lane_integer``.
* ``U,sat,clock_ura_m`` — only under ``network.emit_ura``.  **Datum-pivot
  satellites get NO U line by design** (their pin variance is bogus), so an
  absent URA for a satellite is expected, not data loss.
* ``S,ref_rx,bias_sign,windup_id,amb_datum,isb_datum,emit_cb,cb_datum,``
  ``disc_thresh_m,emit_clock_disc,apriori_product_id[,<const>:<pivot_sat>...]``
  — a *v2-only* header record carrying the S-basis contract (a real ``S`` record,
  not a ``#`` comment).  The C++ reader guards on ``f.size() >= 11``; the enums
  are serialized as ints; the trailing entries are ``<const>:<pivot_sat>`` pairs.
  A v1 file has no ``S`` line, so ``contract`` is ``None``.
* Numbers are shortest-round-trip floats (``fmt "{}"``) — ``float()`` handles all.
* Unknown tags are skipped and counted.

Note the v2 writer is *deferred* (the file opens at the FIRST emitted epoch), so a
zero-epoch run leaves no file at all.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional

import pandas as pd

# Enum decode tables (mirror SbasisDeclaration in the C++ headers).  Values are
# the ``static_cast<int>`` serialization written by ``writeHeader_``.
_AMB_DATUM = {0: "none", 1: "pivot", 2: "reference"}
_ISB_DATUM = {0: "none", 1: "reference", 2: "per_system"}
_CODE_BIAS_DATUM = {0: "none", 1: "sum_to_zero", 2: "if_pin", 3: "reference"}


@dataclass
class ProductContract:
    """Decoded S-basis contract (the v2 ``S`` header record)."""
    reference_receiver_id: str = ""
    bias_sign: float = 0.0
    windup_yaw_model_id: str = ""
    amb_datum: int = 0
    isb_datum: int = 0
    emit_code_bias: bool = False
    code_bias_datum: int = 0
    disc_threshold_m: float = 0.0
    emit_clock_disc: bool = False
    apriori_product_id: str = ""
    clock_pivot_sat: Dict[str, str] = field(default_factory=dict)

    @property
    def amb_datum_name(self) -> str:
        return _AMB_DATUM.get(self.amb_datum, str(self.amb_datum))

    @property
    def isb_datum_name(self) -> str:
        return _ISB_DATUM.get(self.isb_datum, str(self.isb_datum))

    @property
    def code_bias_datum_name(self) -> str:
        return _CODE_BIAS_DATUM.get(self.code_bias_datum, str(self.code_bias_datum))


@dataclass
class ProductFile:
    """Parsed network product file.

    Attributes
    ----------
    header_version:
        ``"v2"`` / ``"v1"`` from the banner comment (``None`` if no recognizable
        banner was seen — the reader still parses records).
    contract:
        Decoded S-basis contract, or ``None`` for a v1 / pre-B6 file.
    rows:
        Long-form records.  Each dict has keys ``week, tow, sat, record, signal,
        value`` plus the per-epoch flags carried onto every row of that epoch:
        ``datum_changed`` (bool), ``datum_consts`` (str, e.g. ``"GE"``),
        ``retro`` (bool — the row's satellite is in the epoch's R set).  ``record``
        is the tag letter (``C``/``B``/``P``/``U``); ``signal`` is ``""`` for
        clock (``C``) and URA (``U``) records; ``value`` is the primary numeric
        (clock_c0_m / code_bias_m / phase bias_m / clock_ura_m).  For a ``C`` row
        the optional drift is carried in ``clock_c1_m`` (``NaN`` when absent) and a
        ``has_c1`` flag; ``P`` rows carry ``signal_integer`` / ``wide_lane_integer``.
    n_epochs:
        Count of ``E`` records seen.
    unknown_tags:
        ``{tag: count}`` for lines whose leading field was not a known tag.
    malformed:
        Count of records dropped because a required field was missing / non-numeric
        (mirrors the C++ reader's ``*ok = false`` paths).
    """
    header_version: Optional[str] = None
    contract: Optional[ProductContract] = None
    rows: List[dict] = field(default_factory=list)
    n_epochs: int = 0
    unknown_tags: Dict[str, int] = field(default_factory=dict)
    malformed: int = 0

    def to_frame(self) -> pd.DataFrame:
        """Long table as a DataFrame (empty with the canonical columns if no rows)."""
        cols = ["week", "tow", "sat", "record", "signal", "value",
                "clock_c1_m", "has_c1", "signal_integer", "wide_lane_integer",
                "datum_changed", "datum_consts", "retro"]
        if not self.rows:
            return pd.DataFrame(columns=cols)
        return pd.DataFrame(self.rows, columns=cols)


def _to_float(s: str) -> float:
    """``float()`` a field; raises ``ValueError`` on a bad token (caller catches)."""
    return float(s)


def parse_product_file(path: str) -> ProductFile:
    """Parse a ``network_products*.txt`` file into a :class:`ProductFile`.

    Robust to CRLF and to the optional / deferred fields exactly as the C++ reader
    is.  Malformed records are counted and skipped rather than raising, so a
    partially-written file (e.g. a run still in progress) still yields the epochs
    that did parse.
    """
    pf = ProductFile()

    # Per-epoch carry state (applied to every data row of the current epoch).
    cur_week: Optional[int] = None
    cur_tow: Optional[float] = None
    cur_datum_changed = False
    cur_datum_consts = ""
    cur_retro: set = set()
    # Rows of the current epoch, so a late R record (written right after E, but we
    # tolerate any position) can back-fill the retro flag.
    epoch_row_start = 0
    have_epoch = False

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if line.endswith("\r"):
                line = line[:-1]
            if not line:
                continue
            if line[0] == "#":
                low = line.lower()
                if "gnss_network_product v2" in low:
                    pf.header_version = "v2"
                elif "gnss_network_product v1" in low:
                    pf.header_version = "v1"
                continue

            f = line.split(",")
            if not f or not f[0]:
                continue
            tag = f[0]

            try:
                if tag == "E":
                    if len(f) < 6:
                        pf.malformed += 1
                        continue
                    # New epoch: reset carry state.
                    cur_week = int(f[1])
                    cur_tow = _to_float(f[2])
                    cur_datum_changed = (len(f) > 6) and (int(f[6]) != 0)
                    cur_datum_consts = f[7] if len(f) > 7 else ""
                    cur_retro = set()
                    epoch_row_start = len(pf.rows)
                    have_epoch = True
                    pf.n_epochs += 1
                elif tag == "G":
                    # Per-epoch generation time — carried on the epoch, not a data
                    # row.  (Only stamped runs emit it.)  Nothing to store here for
                    # the long table.
                    if not have_epoch or len(f) < 3:
                        pf.malformed += 1
                    continue
                elif tag == "R":
                    if not have_epoch:
                        pf.malformed += 1
                        continue
                    for k in range(1, len(f)):
                        if f[k]:
                            cur_retro.add(f[k])
                    # Back-fill retro flag onto rows already emitted this epoch.
                    for r in pf.rows[epoch_row_start:]:
                        r["retro"] = r["sat"] in cur_retro
                elif tag == "C":
                    if not have_epoch or len(f) < 3:
                        pf.malformed += 1
                        continue
                    # RTF-2 (Card 4): after the c0 value, trailing C-record fields are
                    # either POSITIONAL numerics (RTA-4 disc-counter VALUES: c1 then the
                    # clock disc) or self-describing key=value tokens (brdc_rts:
                    # iode=/oiod=/oepoch=<week>:<tow>).  Mirror the C++ reader's '='-guard
                    # (NetworkProductIO.cpp:149-156) so a brdc_rts line
                    # "C,G05,<c0>,iode=39" does NOT try to float("iode=39") as the
                    # positional c1 and drop the whole clock row.
                    c1 = float("nan")
                    has_c1 = False
                    clock_iode = None
                    orbit_iod_ssr = None
                    orbit_epoch = None
                    pos_seen = 0
                    for tok in f[3:]:
                        if not tok:
                            continue
                        if "=" in tok:
                            key, _, val = tok.partition("=")
                            try:
                                if key == "iode":
                                    clock_iode = int(val)
                                elif key == "oiod":
                                    orbit_iod_ssr = int(val)
                                elif key == "oepoch":
                                    orbit_epoch = val   # "<week>:<tow>"
                            except ValueError:
                                pass                    # tolerate a malformed echo token
                        else:
                            if pos_seen == 0:
                                c1 = _to_float(tok)     # RTA-4 positional c1 (clock drift)
                                has_c1 = True
                            # pos_seen == 1 would be the positional clock-disc VALUE; the
                            # reader tracks disc via the datum/retro flags, not on the C row.
                            pos_seen += 1
                    pf.rows.append(_row(cur_week, cur_tow, f[1], "C", "",
                                        _to_float(f[2]), cur_datum_changed,
                                        cur_datum_consts, cur_retro,
                                        clock_c1_m=c1, has_c1=has_c1,
                                        clock_iode=clock_iode,
                                        orbit_iod_ssr=orbit_iod_ssr,
                                        orbit_epoch=orbit_epoch))
                elif tag == "B":
                    if not have_epoch or len(f) < 4:
                        pf.malformed += 1
                        continue
                    pf.rows.append(_row(cur_week, cur_tow, f[1], "B", f[2],
                                        _to_float(f[3]), cur_datum_changed,
                                        cur_datum_consts, cur_retro))
                elif tag == "P":
                    if not have_epoch or len(f) < 6:
                        pf.malformed += 1
                        continue
                    pf.rows.append(_row(cur_week, cur_tow, f[1], "P", f[2],
                                        _to_float(f[3]), cur_datum_changed,
                                        cur_datum_consts, cur_retro,
                                        signal_integer=(int(f[4]) != 0),
                                        wide_lane_integer=int(f[5])))
                elif tag == "U":
                    if not have_epoch or len(f) < 3:
                        pf.malformed += 1
                        continue
                    pf.rows.append(_row(cur_week, cur_tow, f[1], "U", "",
                                        _to_float(f[2]), cur_datum_changed,
                                        cur_datum_consts, cur_retro))
                elif tag == "S":
                    # v2 header contract record.  Reader guard mirrors the C++
                    # f.size() >= 11 check; a short S line is flagged + skipped.
                    if len(f) >= 11:
                        pf.contract = _parse_contract(f)
                    else:
                        pf.malformed += 1
                else:
                    pf.unknown_tags[tag] = pf.unknown_tags.get(tag, 0) + 1
            except (ValueError, IndexError):
                # Malformed numeric / missing field — flag and skip, like the C++
                # reader's catch(std::exception).
                pf.malformed += 1
                continue

    return pf


def _row(week, tow, sat, record, signal, value, datum_changed, datum_consts,
         retro_set, *, clock_c1_m=float("nan"), has_c1=False,
         signal_integer=None, wide_lane_integer=None,
         clock_iode=None, orbit_iod_ssr=None, orbit_epoch=None) -> dict:
    return {
        "week": week,
        "tow": tow,
        "sat": sat,
        "record": record,
        "signal": signal,
        "value": value,
        "clock_c1_m": clock_c1_m,
        "has_c1": has_c1,
        "signal_integer": signal_integer,
        "wide_lane_integer": wide_lane_integer,
        # RTF-2 (Card 4/5): broadcast eph version the brdc_rts C0 is referenced to,
        # and the orbit-coherence echo.  None on every non-brdc_rts (sp3-mode) row.
        "clock_iode": clock_iode,
        "orbit_iod_ssr": orbit_iod_ssr,
        "orbit_epoch": orbit_epoch,
        "datum_changed": datum_changed,
        "datum_consts": datum_consts,
        "retro": sat in retro_set,
    }


def _parse_contract(f: List[str]) -> ProductContract:
    """Decode an ``S`` record's fields into a :class:`ProductContract`.

    Caller guarantees ``len(f) >= 11``.  Trailing ``<const>:<pivot_sat>`` entries
    (fields 11+) are colon-split into the pivot map.
    """
    c = ProductContract(
        reference_receiver_id=f[1],
        bias_sign=float(f[2]),
        windup_yaw_model_id=f[3],
        amb_datum=int(f[4]),
        isb_datum=int(f[5]),
        emit_code_bias=(int(f[6]) != 0),
        code_bias_datum=int(f[7]),
        disc_threshold_m=float(f[8]),
        emit_clock_disc=(int(f[9]) != 0),
        apriori_product_id=f[10],
    )
    for k in range(11, len(f)):
        entry = f[k]
        colon = entry.find(":")
        if colon != -1:
            c.clock_pivot_sat[entry[:colon]] = entry[colon + 1:]
    return c
