#!/usr/bin/env python3
"""Rapid one-change arms for the two-bed (sim / field) bisection loop.

A fault that shows on one bed and not on the other is isolated by a series of short
arms that each change ONE thing: on the field bed an ingredient is SUBTRACTED
(observation types stripped for a station or satellite subset, a station dropped, a
knob flipped), on the sim bed an ingredient is ADDED (a bias, a relabel, a slip
process, a drift).  This tool turns a one-line spec into a runnable arm, runs a batch
in parallel, and prints one scorecard.

  make   --base <arm.json> --tag <name> [--hours H] [--set key.path=value ...]
         [--drop-station NAME ...] [--rinex "OP;key=val;..." ...]
  run    --tags a b c [--jobs 4] [--exe <gnss_processor exe>]      (Release exe only)
  score  <tag or run_dir> ... [--pf]
  draws  --op "OP;..." [--op ...] --ids G01,G02,...                (the constants a bias op draws)
  selftest

RINEX ops (GPS by default, `sys=` to change).  Common selectors:
  stations = all | A,B,C | family:Q | family:X | every:N | first:N     (default all)
  sats     = all | G03,G06 | IIF | III | L5 | NOL5                      (default all)
  consumer = 1 to apply the op to the closed-loop consumer's file too   (default 0)
  types    = comma list of 3-char patterns, `?` is a wildcard (C5? L5? ?5?)
Ops:
  strip;types=?5?                     remove the types (header too when sats=all)
  strip;types=???;sats=G30;tow_from=277200[;tow_to=278700]
                                      a strip restricted to a TIME WINDOW (GPS seconds of week,
                                      inclusive, either bound may be omitted).  The epoch's TOW is
                                      read from the `> yyyy mm dd hh mm ss` header (the files are in
                                      GPS time; the day of week comes from the date, no leap-second
                                      arithmetic).  A windowed strip NEVER edits the header type
                                      list, not even with sats=all, because the types exist outside
                                      the window; it blanks the selected fields of the records whose
                                      epoch falls inside it.  A subtraction in a time window is the
                                      field bed's equivalent of an injected event (card EV7).
  relabel;from=?5Q;to=?5X             rename observation types in the header
  bias;types=C5?;per=station|sat;sigma=3.0;seed=1      constant, metres (N(0, sigma))
  slip;types=L5?;rate=1.0;max=5;seed=1                 integer-cycle steps, rate per arc-hour
  drift;types=L5?;amp_m=0.03;period_s=5400;per=station|sat|los;seed=1   sinusoid, metres
  dropslip;types=L1?,L2?,C2?;rate=0.15;len=3;max=300;seed=1
                                      a loss of lock: the listed types are blanked for `len` epochs
                                      (rate per arc-hour) and every listed PHASE type returns with
                                      its own integer-cycle step, while the other types track through
  primaryonly;bands=12                remove every observation type of `bands` whose tracking
                                      attribute is not the station's primary one (first phase
                                      type of the band in the priority list that the file has),
                                      so that no fallback signal exists; epochs are kept
  holes;bands=12;window=20;where=all  delete a satellite's whole record at an epoch where the
                                      primary phase of one of `bands` is missing inside an arc
                                      (a good epoch within `window` epochs), so that a
                                      phase-only dropout becomes a full tracking gap;
                                      where=mid keeps only epochs with a good epoch on BOTH
                                      sides, where=edge only those with one on a single side;
                                      where=causal;keep=N deletes a partial epoch that comes
                                      before the satellite's first good epoch or more than N
                                      epochs after its last good one (a rule a filter can run)
  ecc;stations=A,B;h=2.0[;e=0.0][;n=0.0]
                                      GIVE the selected stations an antenna eccentricity (card
                                      UPO).  The vector is written into the `ANTENNA: DELTA
                                      H/E/N` record of each station's RINEX copy (the record is
                                      inserted before END OF HEADER when the file has none), AND
                                      the station's `position_ecef` in the ARM's config is moved
                                      by MINUS the same vector along the local up / east / north
                                      at the configured coordinate.  The true antenna therefore
                                      stays exactly where the simulator put it and the configured
                                      coordinate becomes a MONUMENT coordinate, which is the
                                      ingredient the sim bed lacks (every sim station is 0/0/0).
                                      `network.station_marker_to_arp` reduces it back, so ON must
                                      reproduce the un-eccentric baseline and OFF must show the
                                      defect.  Repeatable: one op per (h, e, n) group of stations.
  clone;from=C1C;to=C1W;noise=0.3;seed=7
                                      ADD an observation type `to` as a copy of `from` plus
                                      white noise N(0, noise) metres drawn per (seed, station,
                                      satellite, epoch).  The type is appended to the header
                                      (count and continuation lines rebuilt), a blank source
                                      field stays blank, and the LLI / signal-strength digits
                                      are copied.  The new column is visible to every LATER op,
                                      so `clone` followed by `bias;types=<to>` gives the second
                                      signal known receiver and satellite inter-signal biases —
                                      which is how a sim bed whose files carry ONE band-1 code
                                      gets a second one (card SCC, R9).  Put the clone FIRST.
An op on a phase type is given in metres and applied in cycles.  Every draw is keyed
on (seed, station, satellite), so the same spec gives the same file in every arm.
`draws` prints those constants without writing a file, so an injected truth can be
compared against what a wire carries.
"""
import argparse
import concurrent.futures as cf
import copy
import csv
import datetime
import fnmatch
import hashlib
import json
import math
import os
import random
import statistics as st
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
RUNS = ROOT + "/runs"
LABEL = "SYS / # / OBS TYPES"
C = 299792458.0
FREQ = {"G": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6}}
IIF = "G03 G06 G08 G09 G10 G24 G25 G26 G27 G30 G32".split()
III = "G04 G11 G14 G18 G23 G28".split()
SATSETS = {"IIF": set(IIF), "III": set(III), "L5": set(IIF + III)}


# ----------------------------------------------------------------------------- specs
def parse_op(text):
    parts = [p for p in text.split(";") if p.strip()]
    op = {"op": parts[0].strip(), "sys": "G", "stations": "all", "sats": "all", "consumer": "0"}
    for kv in parts[1:]:
        k, v = kv.split("=", 1)
        op[k.strip()] = v.strip()
    assert op["op"] in ("strip", "relabel", "bias", "slip", "drift", "holes", "dropslip",
                        "primaryonly", "clone", "ecc"), op
    assert op["op"] == "strip" or not (set(op) & {"tow_from", "tow_to"}), \
        "tow_from / tow_to are selectors of the strip op only: " + text
    if op["op"] == "ecc":
        assert "h" in op or "e" in op or "n" in op, "ecc needs at least one of h= e= n=: " + text
        assert op["stations"] != "all", \
            "ecc must name its stations (an eccentricity on every station is not an arm): " + text
    return op


# ----------------------------------------------------------------------------- geodesy
# WGS84, matching include/common/Constants.hpp and src/core/math/Coordinates.cpp
# (ecefToLlh's Bowring start + fixed-point refinement, enuToEcefMatrix's E/N/U
# columns).  Used ONLY by the `ecc` op, to move a configured coordinate by minus
# the eccentricity it writes into the header, so that the estimator's own
# markerToArp puts the station back exactly where it was.
WGS84_A, WGS84_F = 6378137.0, 1.0 / 298.257223563


def ecef_to_llh(x, y, z):
    e2 = WGS84_F * (2.0 - WGS84_F)
    p = math.hypot(x, y)
    lon = math.atan2(y, x)
    if p < 1e-12:
        lat = math.copysign(math.pi / 2.0, z)
        nrad = WGS84_A / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
        return lat, lon, abs(z) - nrad * (1.0 - e2)
    lat, h = math.atan2(z, p * (1.0 - e2)), 0.0
    for _ in range(10):
        nrad = WGS84_A / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - nrad
        denom = p * (1.0 - e2 * (nrad / (nrad + h)))
        if abs(denom) < 1e-15:
            break
        nxt = math.atan2(z, denom)
        if abs(nxt - lat) < 1e-13:
            lat = nxt
            break
        lat = nxt
    return lat, lon, h


def enu_to_ecef(enu, lat, lon):
    """ENU offset -> ECEF offset, with the local frame at (lat, lon)."""
    se, ce, sl, cl = math.sin(lon), math.cos(lon), math.sin(lat), math.cos(lat)
    e, n, u = enu
    return (-se * e - sl * ce * n + cl * ce * u,
            ce * e - sl * se * n + cl * se * u,
            cl * n + sl * u)


def ecc_hen(op):
    """(H, E, N) of an `ecc` op, in RINEX header order."""
    return (float(op.get("h", 0.0)), float(op.get("e", 0.0)), float(op.get("n", 0.0)))


def lower_to_marker(pos, hen):
    """Move an ARP coordinate DOWN by `hen` in the local frame: the monument it came from.

    The single-shot inverse of Coordinates::markerToArp evaluated at the ARP rather
    than at the marker, which closes to O(|ecc|^2 / R) — 6e-7 m for a 2 m tower, four
    orders below anything graded here.  This is deliberately the literal "minus the
    same vector at the configured coordinate" the card asks for.
    """
    lat, lon, _ = ecef_to_llh(*pos)
    d = enu_to_ecef((hen[1], hen[2], hen[0]), lat, lon)
    return [pos[0] - d[0], pos[1] - d[1], pos[2] - d[2]]


def apply_ecc_header(lines, hen):
    """Write `hen` into the ANTENNA: DELTA H/E/N record, inserting one when absent."""
    rec = ("%14.4f%14.4f%14.4f" % hen).ljust(60) + "ANTENNA: DELTA H/E/N"
    for k, ln in enumerate(lines):
        if ln[60:].strip() == "ANTENNA: DELTA H/E/N":
            return lines[:k] + [rec] + lines[k + 1:]
        if ln[60:].strip() == "END OF HEADER":
            return lines[:k] + [rec] + lines[k:]
    raise AssertionError("no END OF HEADER in the RINEX file")


def op_window(op):
    """(tow_from, tow_to) of a windowed op, or None when it has no window.

    Bounds are GPS seconds of week and INCLUSIVE; either may be omitted.  `None`
    (no window at all) is kept distinct from an open-ended window so that the
    un-windowed code path stays byte-for-byte what it was.
    """
    if "tow_from" not in op and "tow_to" not in op:
        return None
    return (float(op.get("tow_from", "-inf")), float(op.get("tow_to", "inf")))


def epoch_tow(fields):
    """GPS seconds of week of a RINEX 3 epoch header, from its `yyyy mm dd hh mm ss` fields.

    The observation files are in GPS time, so the seconds of week are just the day of
    week (Sunday = 0) times 86400 plus the seconds of day: no leap-second arithmetic.
    """
    d = datetime.date(int(fields[0]), int(fields[1]), int(fields[2]))
    sod = int(fields[3]) * 3600 + int(fields[4]) * 60 + float(fields[5])
    return ((d.weekday() + 1) % 7) * 86400.0 + sod


def match_types(codes, patterns):
    pats = [p.strip() for p in patterns.split(",")]
    return [k for k, c in enumerate(codes) if any(fnmatch.fnmatchcase(c, p) for p in pats)]


def norm_sat(sat):
    """Canonical satellite id for a RINEX record (card RXP).

    A RINEX 3 record may write its number blank-padded — "G 7" for "G07" — and 16
    of the 310 observation files of 2024-129 do.  Every id this tool is COMPARED
    against (the `sats=` selectors, IIF / III, the draw keys) is zero-padded, so a
    raw slice silently skips those records: an op asked for on G03 never reaches a
    station that writes "G 3".  Only comparisons and dictionary keys are
    normalized; the record's own three bytes are written back untouched, because
    re-padding the file would make the arm a second change.
    """
    return sat[:1] + "0" + sat[2:3] if len(sat) >= 3 and sat[1] == " " else sat


def sat_selected(sat, sel):
    sat = norm_sat(sat)
    if sel == "all":
        return True
    if sel == "NOL5":
        return sat not in SATSETS["L5"]
    if sel in SATSETS:
        return sat in SATSETS[sel]
    return sat in sel.split(",")


def station_selected(name, idx, codes, sel):
    if sel == "all":
        return True
    if sel.startswith("family:"):
        return ("L5" + sel[7:]) in codes
    if sel.startswith("every:"):
        return idx % int(sel[6:]) == 0
    if sel.startswith("first:"):
        return idx < int(sel[6:])
    return name in sel.split(",")


def draw(seed, *key):
    return random.Random("|".join([str(seed)] + [str(k) for k in key]))


# ----------------------------------------------------------------------------- RINEX
def read_header(lines, system):
    """-> (index of first record line, codes of `system`, (first, last+1) of its type block)."""
    i, codes, blk = 0, None, None
    while i < len(lines):
        ln = lines[i]
        if ln[60:].strip() == LABEL and ln[:1] == system:
            n, a, codes = int(ln[3:6]), i, []
            while len(codes) < n:
                codes += lines[i][7:60].split()
                i += 1
            blk = (a, i)
            continue
        i += 1
        if ln[60:].strip() == "END OF HEADER":
            break
    return i, codes, blk


def type_block(system, codes):
    out = []
    for a in range(0, len(codes), 13):
        head = f"{system}  {len(codes):3d}" if a == 0 else "      "
        out.append((head + "".join(f" {c}" for c in codes[a:a + 13])).ljust(60) + LABEL)
    return out


def apply_holes(lines, i0, codes, system, op):
    """Delete satellite records where a primary phase is missing inside an arc; fix the epoch counts."""
    prio, prim = op.get("_prio") or [], []
    for b in op.get("bands", "12"):
        cand = [t for t in prio if t[0] == "L" and t[1] == b and t in codes] or \
            sorted(t for t in codes if t[0] == "L" and t[1] == b)
        if not cand:
            return lines, 0
        prim.append(codes.index(cand[0]))
    win, width = int(op.get("window", "20")), 3 + 16 * len(codes)
    pres, owner, ep, j, head = {}, {}, -1, i0, None
    while j < len(lines):
        ln = lines[j]
        if ln.startswith(">"):
            ep, head = ep + 1, j
            flag = int(ln[31]) if len(ln) > 31 and ln[31].isdigit() else 0
            j += 1 + (int(ln[32:35]) if flag > 1 else 0)
            continue
        if ln[:1] == system and len(ln) > 3:
            q = ln.ljust(width)
            pres.setdefault(norm_sat(ln[:3]), []).append(
                (ep, all(q[3 + 16 * k: 17 + 16 * k].strip() for k in prim), j))
            owner[j] = head
        j += 1
    kill, where = set(), op.get("where", "all")
    for v in pres.values():
        if where == "causal":
            last, keep_n = None, int(op.get("keep", "3"))
            for e, ok, j in v:
                if ok:
                    last = e
                elif last is None or e - last > keep_n:
                    kill.add(j)
            continue
        good = [e for e, ok, _ in v if ok]
        for e, ok, j in v:
            if ok or not good:
                continue
            before = any(0 < e - g <= win for g in good)
            after = any(0 < g - e <= win for g in good)
            if (where == "all" and (before or after)) or (where == "mid" and before and after) \
                    or (where == "edge" and (before != after)):
                kill.add(j)
    nk = {}
    for j in kill:
        nk[owner[j]] = nk.get(owner[j], 0) + 1
    out = []
    for j, ln in enumerate(lines):
        if j in kill:
            continue
        if j in nk:
            ln = ln[:32] + f"{int(ln[32:35]) - nk[j]:3d}" + ln[35:]
        out.append(ln)
    return out, len(kill)


def rewrite_rinex(src, dst, ops, station, hours=None):
    """Apply `ops` (already selected for this station) to one RINEX 3 file; returns the records deleted by `holes`."""
    lines = open(src, "r", encoding="ascii", errors="replace").read().split("\n")
    # `ecc` is a HEADER op: it rewrites (or inserts) the ANTENNA: DELTA H/E/N
    # record and touches no observation.  Applied before read_header so the line
    # indices below are those of the final header.
    for op in ops:
        if op["op"] == "ecc":
            lines = apply_ecc_header(lines, ecc_hen(op))
    system = ops[0]["sys"]
    i, codes, blk = read_header(lines, system)
    assert codes, (src, "no type block for", system)
    n_holes = 0
    for op in ops:
        if op["op"] == "holes":
            lines, n = apply_holes(lines, i, codes, system, op)
            n_holes += n
    # A CLONE appends a new observation type, so every later step works against the
    # WIDENED type list: a `bias;types=C1W` that follows a `clone;to=C1W` has to find
    # the column the clone created, which is why the clone is parsed first and why
    # match_types reads `work` below.  With no clone op `work` IS `codes`, so every
    # index, the header block and every record line are byte-for-byte what they were.
    clones = []
    for op in ops:
        if op["op"] != "clone":
            continue
        src_code, dst_code = op["from"], op["to"]
        assert src_code in codes, (src, "clone source type not in the file", src_code)
        assert dst_code not in codes, (src, "clone target type already exists", dst_code)
        assert dst_code not in [c["to"] for c in clones], (src, "cloned twice", dst_code)
        clones.append({"to": dst_code, "src": codes.index(src_code),
                       "noise": float(op.get("noise", 0.0)), "seed": op.get("seed", "1")})
    work = codes + [c["to"] for c in clones]
    new_codes, drop = list(work), set()
    for op in ops:
        # A WINDOWED strip never touches the header: the types exist outside the window.
        if op["op"] == "strip" and op["sats"] == "all" and op_window(op) is None:
            drop |= set(match_types(work, op["types"]))
        if op["op"] == "primaryonly":
            prio = op.get("_prio") or []
            for b in op.get("bands", "12"):
                cand = [t for t in prio if t[0] == "L" and t[1] == b and t in work] or \
                    sorted(t for t in work if t[0] == "L" and t[1] == b)
                if cand:
                    drop |= {k for k, c in enumerate(work) if c[1] == b and c[2] != cand[0][2]}
        if op["op"] == "relabel":
            for k in match_types(work, op["from"]):
                new_codes[k] = "".join(t if t != "?" else c for t, c in zip(op["to"], work[k]))
    keep = [k for k in range(len(work)) if k not in drop]
    out = lines[:blk[0]] + type_block(system, [new_codes[k] for k in keep]) + lines[blk[1]:i]
    width = 3 + 16 * len(codes)
    rec_ops = [op for op in ops if op["op"] not in ("relabel", "holes", "primaryonly", "clone", "ecc")
               and not (op["op"] == "strip" and op["sats"] == "all" and op_window(op) is None)]
    for op in rec_ops:
        op["_idx"] = match_types(work, op["types"])
        op["_win"] = op_window(op)
    state, sod, tow = {}, 0.0, None
    while i < len(lines):
        ln = lines[i]
        if ln.startswith(">"):
            f = ln[1:].split()
            sod = int(f[3]) * 3600 + int(f[4]) * 60 + float(f[5])
            tow = epoch_tow(f)
            if hours is not None and sod > hours * 3600.0 + 1.0:
                break
            out.append(ln)
            flag = int(ln[31]) if len(ln) > 31 and ln[31].isdigit() else 0
            i += 1
            if flag > 1:
                n = int(ln[32:35])
                out += lines[i:i + n]
                i += n
            continue
        if ln[:1] == system and len(ln) > 3:
            # Card RXP: compare and key on the canonical id, write back ln[:3].
            sat = norm_sat(ln[:3])
            p = ln.ljust(width)
            fld = [p[3 + 16 * k: 19 + 16 * k] for k in range(len(codes))]
            # The cloned columns, formed from the RAW source field before any record
            # op touches it, so `clone` then `bias;types=<to>` injects the bias once
            # and the source column keeps its own value.  A blank source stays blank
            # (the clone inherits the tracking gaps of the signal it copies), and the
            # LLI / signal-strength digits are carried over verbatim.
            for c in clones:
                f = fld[c["src"]]
                if not f[:14].strip():
                    fld.append(" " * 16)
                    continue
                d = 0.0
                if c["noise"] > 0.0:
                    d = draw(c["seed"], "clone", c["to"], station, sat,
                             "%.3f" % sod).gauss(0.0, c["noise"])
                fld.append(f"{float(f[:14]) + d:14.3f}" + f[14:16])
            for n_op, op in enumerate(rec_ops):
                if not sat_selected(sat, op["sats"]):
                    continue
                w = op["_win"]
                if w is not None and not (w[0] <= tow <= w[1]):
                    continue
                if op["op"] == "dropslip":
                    dropslip(op, n_op, station, sat, work, fld, sod, state, system)
                    continue
                for k in op["_idx"]:
                    if not fld[k][:14].strip():
                        continue
                    if op["op"] == "strip":
                        fld[k] = " " * 16
                        continue
                    d = delta_m(op, n_op, station, sat, work[k], sod, state)
                    if work[k][0] == "L":
                        d *= FREQ[system][work[k][1]] / C
                    fld[k] = f"{float(fld[k][:14]) + d:14.3f}" + fld[k][14:16]
            ln = (p[:3] + "".join(fld[k] for k in keep)).rstrip()
        out.append(ln)
        i += 1
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "w", encoding="ascii", newline="\n").write("\n".join(out) + ("\n" if out[-1] else ""))
    return n_holes


def dropslip(op, n_op, station, sat, codes, fld, sod, state, system):
    """A loss of lock on the listed types with a slip on each listed phase at the return."""
    st = state.setdefault(("ds", n_op, sat), {"left": 0, "cyc": {}, "last": None, "n": 0,
                                              "rng": draw(op.get("seed", "1"), n_op, "ds", station, sat)})
    rng = st["rng"]
    if st["left"] == 0 and st["last"] is not None and sod - st["last"] < 61.0:
        if rng.random() < 1.0 - math.exp(-float(op.get("rate", 0.15)) * (sod - st["last"]) / 3600.0):
            st["left"] = int(op.get("len", 3))
    st["last"] = sod
    if st["left"] > 0:
        for k in op["_idx"]:
            fld[k] = " " * 16
        st["left"] -= 1
        if st["left"] == 0:
            st["n"] += 1
            for k in op["_idx"]:
                if codes[k][0] == "L":
                    step = rng.randint(1, int(op.get("max", 300)))
                    st["cyc"][k] = st["cyc"].get(k, 0) + (step if rng.random() < 0.5 else -step)
        return
    for k, cyc in st["cyc"].items():
        if fld[k][:14].strip():
            fld[k] = f"{float(fld[k][:14]) + cyc:14.3f}" + fld[k][14:16]


def delta_m(op, n_op, station, sat, code, sod, state):
    seed, kind = op.get("seed", "1"), op["op"]
    if kind == "bias":
        key = station if op.get("per", "station") == "station" else sat
        return draw(seed, n_op, "bias", key).gauss(0.0, float(op.get("sigma", 1.0)))
    if kind == "drift":
        per = op.get("per", "los")
        key = {"station": (station,), "sat": (sat,), "los": (station, sat)}[per]
        ph = draw(seed, n_op, "drift", *key).uniform(0.0, 2.0 * math.pi)
        return float(op.get("amp_m", 0.03)) * math.sin(2.0 * math.pi * sod / float(op.get("period_s", 5400.0)) + ph)
    if kind == "slip":                     # cumulative integer cycles per (station, sat, type), Poisson in time
        key = (n_op, station, sat, code)
        last, cyc, rng = state.get(key, (None, 0, None))
        if rng is None:
            rng = draw(seed, n_op, "slip", station, sat, code)
        if last is not None:
            dt_h = max(0.0, sod - last) / 3600.0
            if rng.random() < 1.0 - math.exp(-float(op.get("rate", 1.0)) * dt_h):
                step = rng.randint(1, int(op.get("max", 5)))
                cyc += step if rng.random() < 0.5 else -step
        state[key] = (sod, cyc, rng)
        return cyc * C / FREQ[op["sys"]][code[1]]      # metres; converted back to cycles by the caller
    raise ValueError(kind)


# ----------------------------------------------------------------------------- make
def set_key(cfg, dotted, value):
    node, keys = cfg, dotted.split(".")
    for k in keys[:-1]:
        node = node.setdefault(k, {})
    try:
        node[keys[-1]] = json.loads(value)
    except ValueError:
        node[keys[-1]] = value


def cmd_make(a):
    base = json.load(open(a.base))
    arm, rd = copy.deepcopy(base), f"{RUNS}/_rap_{a.tag}"
    net = arm["network"]
    for name in a.drop_station or []:
        n0 = len(net["stations"])
        net["stations"] = [s for s in net["stations"] if s["name"] != name]
        assert len(net["stations"]) == n0 - 1, f"station {name} not in the roster"
    ops = [parse_op(t) for t in (a.rinex or [])]
    for op in ops:
        op["_prio"] = base.get("processing", {}).get("signal_priorities", {}).get("GPS", [])
    touched, n_holes, moved = [], 0, []
    if ops:
        files = [(k, s["name"], s["rinex_path"], s) for k, s in enumerate(net["stations"])]
        cons = net.get("closed_loop", {}).get("station")
        if cons:
            files.append((-1, cons["name"], cons["rinex_path"], cons))
        for idx, name, path, node in files:
            _, codes, _ = read_header(open(path, "r", encoding="ascii", errors="replace").read().split("\n"), "G")
            mine = []
            for op in ops:
                if idx == -1:
                    ok = op["consumer"] == "1" and (op["stations"] == "all" or op["stations"].startswith(("family:",))
                                                    and station_selected(name, 0, codes, op["stations"])
                                                    or name in op["stations"].split(","))
                else:
                    ok = station_selected(name, idx, codes, op["stations"])
                if ok:
                    mine.append(dict(op))
            if mine:
                dst = f"{rd}/rinex/{os.path.basename(path)}"
                n_holes += rewrite_rinex(path, dst, mine, name, hours=(a.hours + 0.05) if a.hours else None)
                node["rinex_path"] = dst
                touched.append(name)
                # `ecc` also LOWERS the configured coordinate by the vector it wrote
                # into the header, so the true antenna stays where it was and the
                # config value becomes a monument coordinate (card UPO, U5).  Done
                # here rather than in rewrite_rinex because it edits the ARM, not
                # the RINEX.  A station may carry at most one ecc op: two would
                # compose two rotations and neither header could hold both.
                eccs = [op for op in mine if op["op"] == "ecc"]
                assert len(eccs) < 2, (name, "more than one ecc op selects this station")
                for op in eccs:
                    hen = ecc_hen(op)
                    assert "position_ecef" in node, (name, "no position_ecef to lower")
                    node["position_ecef"] = lower_to_marker(node["position_ecef"], hen)
                    moved.append((name, hen))
    for kv in a.set or []:
        k, v = kv.split("=", 1)
        set_key(arm, k, v)
    if a.hours:
        for sec in ("processing", "network"):
            if "max_duration_s" in arm.get(sec, {}):
                arm[sec]["max_duration_s"] = a.hours * 3600.0
    net["product_output_path"] = rd + "/products.txt"
    os.makedirs(rd, exist_ok=True)
    json.dump(arm, open(rd + "/arm.json", "w"), indent=2)
    prov = {"base": a.base, "base_sha256": hashlib.sha256(open(a.base, "rb").read()).hexdigest(), "tag": a.tag,
            "hours": a.hours, "set": a.set or [], "drop_station": a.drop_station or [], "rinex": a.rinex or [],
            "rinex_touched": touched, "holes_records_deleted": n_holes,
            "ecc_moved": [{"station": s, "hen": list(h)} for s, h in moved]}
    json.dump(prov, open(rd + "/rapid.json", "w"), indent=2)
    print(f"{a.tag}: {len(net['stations'])} stations, {len(touched)} RINEX file(s) rewritten"
          + (f", {n_holes} record(s) deleted by holes" if n_holes else "")
          + (", ecc " + " ".join("%s H%.3f/E%.3f/N%.3f" % (s, *h) for s, h in moved) if moved else "")
          + f", -> {rd}")


# ----------------------------------------------------------------------------- run
def resolve(tag):
    return tag if os.path.isdir(tag) else f"{RUNS}/_rap_{tag}"


def cmd_run(a):
    exe = a.exe.replace("\\", "/")
    assert "x64-Release" in exe and os.path.exists(exe), "a Release exe is required: " + exe

    def one(tag):
        rd = resolve(tag)
        t0 = time.time()
        subprocess.run(["pwsh", "-NoProfile", "-File", ROOT + "/scripts/ev_launch.ps1", "-Exe", exe,
                        "-Config", rd + "/arm.json", "-RunDir", rd], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ex = open(rd + "/exitcode.txt").read().split() if os.path.exists(rd + "/exitcode.txt") else ["?"]
        return tag, ex[0], time.time() - t0

    with cf.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for tag, ex, dt in pool.map(one, a.tags):
            print(f"{tag}: exit {ex}, {dt:.0f} s")


# ----------------------------------------------------------------------------- score
EVENTS = ("T4_attrib", "T1_if_slip", "T0_band_scope", "CLK_ANCHOR", "STRANSFORM", "T0_mw_detect")


def score_one(rd, pf=False):
    out = {"tag": os.path.basename(rd).replace("_rap_", "")}
    ex = open(rd + "/exitcode.txt").read().split() if os.path.exists(rd + "/exitcode.txt") else ["?", "?"]
    out["exit"], out["wall_s"] = ex[0], ex[1] if len(ex) > 1 else "?"
    p = rd + "/logs/closed_loop_trace.csv"
    hrs = {}
    if os.path.exists(p):
        rows = [r for r in csv.DictReader(open(p)) if r.get("tow") and r.get("pos_err_3d_m") is not None]
        t0 = min(float(r["tow"]) for r in rows) if rows else 0.0
        for r in rows:                      # a run still in flight has a partial last row
            try:
                hrs.setdefault(int((float(r["tow"]) - t0) // 3600), []).append(float(r["pos_err_3d_m"]))
            except (ValueError, TypeError):
                pass
    out["p50"] = " ".join(f"{st.median(v):.2f}" for _, v in sorted(hrs.items()))
    out["subdm"] = sum(1 for v in hrs.values() for e in v if e < 0.10)
    cnt = {k: 0 for k in EVENTS}
    cnt["if_slip"] = cnt["if_slip_b5"] = 0
    p = rd + "/logs/network_events.csv"
    if os.path.exists(p):
        for r in csv.reader(open(p)):
            if len(r) < 4:
                continue
            if r[2] in cnt:
                cnt[r[2]] += 1
            if r[2] == "T1_if_slip" and r[3] == "if_slip":
                cnt["if_slip"] += 1
                cnt["if_slip_b5"] += ("L5" in r[-1])
    out.update(cnt)
    if pf and os.path.exists(rd + "/logs/network_pf_rows.csv"):
        import numpy as np
        import pandas as pd
        df = pd.read_csv(rd + "/logs/network_pf_rows.csv", usecols=["tow", "station", "sat", "signal", "kind", "resid_m"],
                         dtype={"station": "category", "sat": "category", "signal": "category", "kind": "category"})
        df = df[(df["kind"] == "phase") & (df["tow"] >= df["tow"].min() + 3600.0)]
        df["band"] = df["signal"].astype(str).str[1]
        r = df["resid_m"] - df.groupby(["station", "sat", "signal"], observed=True)["resid_m"].transform("mean")
        rms = (r * r).groupby([df["band"], df["station"]], observed=True).mean().pow(0.5)
        out["pf_med_mm"] = " ".join(f"L{b}:{1000.0 * float(np.median(rms[b].dropna())):.1f}" for b in sorted(rms.index.levels[0]))
    return out


def cmd_score(a):
    rows = [score_one(resolve(t), a.pf) for t in a.tags]
    cols = ["tag", "exit", "wall_s", "p50", "subdm", "T4_attrib", "if_slip", "if_slip_b5", "T0_band_scope",
            "CLK_ANCHOR", "STRANSFORM", "T0_mw_detect"] + (["pf_med_mm"] if a.pf else [])
    w = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in cols}
    print("  ".join(c.ljust(w[c]) for c in cols))
    for r in rows:
        print("  ".join(str(r.get(c, "")).ljust(w[c]) for c in cols))


# ----------------------------------------------------------------------------- draws
def cmd_draws(a):
    """Print the constants the `bias` ops of a spec list draw, without writing a file.

    The draw key of a bias includes the op's INDEX in the per-file record-op list, so
    a spec cannot be graded in isolation: the same `bias;per=sat;seed=12` draws
    different numbers depending on what precedes it.  The whole list is therefore
    given here exactly as `make --rinex` takes it, in the same order, and the indices
    are rebuilt the way rewrite_rinex builds them (a clone, a relabel, a holes, an
    ecc, a primaryonly and a whole-file strip are structural and take no index).
    """
    ops = [parse_op(t) for t in a.op]
    rec_ops = [op for op in ops if op["op"] not in ("relabel", "holes", "primaryonly", "clone", "ecc")
               and not (op["op"] == "strip" and op["sats"] == "all" and op_window(op) is None)]
    ids = [x.strip() for x in a.ids.split(",") if x.strip()]
    n_printed = 0
    for n_op, op in enumerate(rec_ops):
        if op["op"] != "bias":
            continue
        per, seed = op.get("per", "station"), op.get("seed", "1")
        sigma = float(op.get("sigma", 1.0))
        print(f"# op[{n_op}] bias;types={op.get('types', '')};per={per};sigma={sigma};seed={seed}"
              f"  (metres, key = {per})")
        for x in ids:
            key = x if per == "station" else norm_sat(x)
            print(f"{x} {draw(seed, n_op, 'bias', key).gauss(0.0, sigma):+.6f}")
        n_printed += 1
    if not n_printed:
        print("# no bias op in the spec list: nothing is drawn")


# ----------------------------------------------------------------------------- selftest
def cmd_selftest(_):
    hdr = ["     3.04           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE",
           "G    6 C1C C2W C5Q L1C L2W L5Q                              SYS / # / OBS TYPES",
           "E    2 C1C L1C                                              SYS / # / OBS TYPES",
           "                                                            END OF HEADER"]

    def rec(sat, vals):
        return sat + "".join(f"{v:14.3f} 7" for v in vals)

    body = []
    for e in range(4):
        body.append(f"> 2024 05 08 00 {e:02d} 00.0000000  0  3")
        body += [rec("G03", [2.0e7 + e, 2.0e7 + e, 2.0e7 + e, 1.0e8, 0.8e8, 0.7e8]),
                 rec("G05", [2.1e7, 2.1e7, 2.1e7, 1.1e8, 0.9e8, 0.8e8]), rec("E01", [2.2e7, 1.2e8])]
    d = tempfile.mkdtemp()
    src = d + "/a.rnx"
    open(src, "w", newline="\n").write("\n".join(hdr + body) + "\n")
    n = 0

    def chk(cond, what):
        nonlocal n
        n += 1
        assert cond, what

    # 1: strip band 5 everywhere removes the types from the header and the records
    rewrite_rinex(src, d + "/b.rnx", [parse_op("strip;types=?5?")], "AAAA")
    b = open(d + "/b.rnx").read().split("\n")
    chk(b[1].startswith("G    4 C1C C2W L1C L2W"), "header strip")
    chk(b[5] == rec("G03", [2.0e7, 2.0e7, 1.0e8, 0.8e8]).rstrip(), "record strip")
    chk(b[7] == body[3], "other system untouched")
    # 2: strip for a satellite subset blanks the fields and keeps the header
    rewrite_rinex(src, d + "/c.rnx", [parse_op("strip;types=?5?;sats=G03")], "AAAA")
    c = open(d + "/c.rnx").read().split("\n")
    chk(c[1] == hdr[1], "header kept")
    chk(c[5][3 + 32:3 + 48].strip() == "" and c[6] == body[2], "G03 blanked, G05 kept")
    # 3: a code bias is constant per station, identical across calls, and leaves the flags
    rewrite_rinex(src, d + "/e.rnx", [parse_op("bias;types=C5?;per=station;sigma=3;seed=7")], "AAAA")
    rewrite_rinex(src, d + "/f.rnx", [parse_op("bias;types=C5?;per=station;sigma=3;seed=7")], "AAAA")
    e = open(d + "/e.rnx").read().split("\n")
    chk(open(d + "/e.rnx").read() == open(d + "/f.rnx").read(), "deterministic")
    b3 = float(e[5][3 + 32:3 + 46]) - 2.0e7
    b5 = float(e[6][3 + 32:3 + 46]) - 2.1e7
    chk(abs(b3 - b5) < 1e-3 and abs(b3) > 1e-3 and e[5][3 + 46:3 + 48] == " 7", "per-station constant")
    chk(e[5][3:3 + 32] == body[1][3:3 + 32], "other types untouched")
    # 4: relabel renames the types only
    rewrite_rinex(src, d + "/g.rnx", [parse_op("relabel;from=?5Q;to=?5X")], "AAAA")
    g = open(d + "/g.rnx").read().split("\n")
    chk("C5X" in g[1] and "L5X" in g[1] and "C5Q" not in g[1] and g[5:] == (hdr + body)[5:] + [""], "relabel")
    # 5: a slip process adds whole cycles to the phase, cumulatively
    rewrite_rinex(src, d + "/h.rnx", [parse_op("slip;types=L5?;rate=3600;max=1;seed=2")], "AAAA")
    h = open(d + "/h.rnx").read().split("\n")
    steps = [float(h[5 + 4 * k][3 + 80:3 + 94]) - 0.7e8 for k in range(4)]
    chk(all(abs(s - round(s)) < 1e-3 for s in steps) and steps[0] == 0.0 and any(abs(s) >= 1 for s in steps[1:]),
        f"integer slips {steps}")
    # 6: truncation
    rewrite_rinex(src, d + "/i.rnx", [parse_op("strip;types=?5?")], "AAAA", hours=1.5 / 60.0)
    chk(sum(1 for x in open(d + "/i.rnx").read().split("\n") if x.startswith(">")) == 2, "truncate at the hour mark")
    # 7: holes: a phase-only dropout inside an arc deletes the satellite's record and fixes the count
    hb = list(hdr + body)
    full = rec("G03", [2.0e7 + 1, 2.0e7 + 1, 2.0e7 + 1, 1.0e8, 0.8e8, 0.7e8])
    hb[4 + 5] = full[:3 + 48] + " " * 16 + full[3 + 64:]          # epoch 2: G03 without its L1C
    open(d + "/j.rnx", "w", newline="\n").write("\n".join(hb) + "\n")
    nh = rewrite_rinex(d + "/j.rnx", d + "/k.rnx",
                       [dict(parse_op("holes;bands=12;window=3"), _prio=["L1C", "L2W"])], "AAAA")
    k = open(d + "/k.rnx").read().split("\n")
    chk(nh == 1 and k[8].startswith("> 2024 05 08 00 01") and k[8][32:35] == "  2" and not k[9].startswith("G03"),
        f"holes {nh} {k[8]!r}")
    chk(sum(1 for x in k if x.startswith("G03")) == 3 and sum(1 for x in k if x.startswith("G05")) == 4, "holes scope")
    n_mid = rewrite_rinex(d + "/j.rnx", d + "/k2.rnx",
                          [dict(parse_op("holes;bands=12;window=3;where=mid"), _prio=["L1C", "L2W"])], "AAAA")
    n_edge = rewrite_rinex(d + "/j.rnx", d + "/k3.rnx",
                           [dict(parse_op("holes;bands=12;window=3;where=edge"), _prio=["L1C", "L2W"])], "AAAA")
    chk(n_mid == 1 and n_edge == 0, f"holes where: mid {n_mid} edge {n_edge}")
    n_c0 = rewrite_rinex(d + "/j.rnx", d + "/k4.rnx",
                         [dict(parse_op("holes;bands=12;where=causal;keep=0"), _prio=["L1C", "L2W"])], "AAAA")
    n_c3 = rewrite_rinex(d + "/j.rnx", d + "/k5.rnx",
                         [dict(parse_op("holes;bands=12;where=causal;keep=3"), _prio=["L1C", "L2W"])], "AAAA")
    chk(n_c0 == 1 and n_c3 == 0, f"holes causal: keep=0 {n_c0} keep=3 {n_c3}")
    # 9: primaryonly keeps one tracking attribute per band
    hp = list(hdr)
    hp[1] = "G    8 C1C C1W C2W C2L L1C L1W L2W L2L                      SYS / # / OBS TYPES"
    open(d + "/q.rnx", "w", newline="\n").write("\n".join(hp + ["> 2024 05 08 00 00 00.0000000  0  1",
                                                 rec("G03", [1, 2, 3, 4, 5, 6, 7, 8])]) + "\n")
    rewrite_rinex(d + "/q.rnx", d + "/r.rnx", [dict(parse_op("primaryonly;bands=12"), _prio=["L1W", "L1C", "L2W", "L2L"])], "AAAA")
    r = open(d + "/r.rnx").read().split("\n")
    chk(r[1].startswith("G    4 C1W C2W L1W L2W") and r[5] == rec("G03", [2, 3, 6, 7]).rstrip(), f"primaryonly {r[1]!r}")
    # 8: dropslip: the listed types vanish for `len` epochs and the phases return stepped by whole cycles
    long_body = []
    for e in range(12):
        long_body.append(f"> 2024 05 08 00 {e:02d} 00.0000000  0  1")
        long_body.append(rec("G03", [2.0e7, 2.0e7, 2.0e7, 1.0e8, 0.8e8, 0.7e8]))
    open(d + "/m.rnx", "w", newline="\n").write("\n".join(hdr + long_body) + "\n")
    rewrite_rinex(d + "/m.rnx", d + "/n.rnx", [parse_op("dropslip;types=L1?,L2?,C2?;rate=40;len=2;max=9;seed=5")], "AAAA")
    g = [x for x in open(d + "/n.rnx").read().split("\n") if x.startswith("G03")]
    blank = [k for k, x in enumerate(g) if not x.ljust(99)[3 + 48:3 + 62].strip()]
    chk(len(blank) >= 2 and all(x.ljust(99)[3:3 + 14].strip() and x.ljust(99)[3 + 80:3 + 94].strip() for x in g),
        f"dropslip keeps C1C and L5Q, blanks L1C at {blank}")
    first = next(k for k in range(blank[0] + 1, len(g)) if g[k].ljust(99)[3 + 48:3 + 62].strip())
    after = [float(g[first][3 + 48:3 + 62]) - 1.0e8]
    chk(after and abs(after[0]) >= 1 and abs(after[0] - round(after[0])) < 1e-3, f"dropslip integer step {after}")
    # 10: card RXP — a blank-padded number ("G 3") is SELECTED as G03 and keyed as
    #     G03, while the record's own three bytes are written back untouched.
    pad_b, bln_b = [], []
    for e in range(4):
        ep = f"> 2024 05 08 00 {e:02d} 00.0000000  0  2"
        vals3 = [2.0e7 + e, 2.0e7 + e, 2.0e7 + e, 1.0e8, 0.8e8, 0.7e8]
        vals17 = [2.1e7, 2.1e7, 2.1e7, 1.1e8, 0.9e8, 0.8e8]
        pad_b += [ep, rec("G03", vals3), rec("G17", vals17)]
        bln_b += [ep, rec("G 3", vals3), rec("G17", vals17)]
    open(d + "/s_pad.rnx", "w", newline="\n").write("\n".join(hdr + pad_b) + "\n")
    open(d + "/s_bln.rnx", "w", newline="\n").write("\n".join(hdr + bln_b) + "\n")
    rewrite_rinex(d + "/s_bln.rnx", d + "/t_bln.rnx", [parse_op("strip;types=?5?;sats=G03")], "AAAA")
    t = open(d + "/t_bln.rnx").read().split("\n")
    chk(t[5][:3] == "G 3", f"blank id written back verbatim {t[5][:3]!r}")
    chk(t[5][3 + 32:3 + 46].strip() == "" and t[5][3 + 80:3 + 94].strip() == "",
        "blank-padded G 3 selected by sats=G03")
    chk(t[6] == bln_b[2].rstrip(), "G17 untouched")
    # The draw key is the canonical id, so the same satellite gets the same slips
    # whichever way its file writes the number.
    rewrite_rinex(d + "/s_pad.rnx", d + "/u_pad.rnx", [parse_op("slip;types=L5?;sats=G03;rate=3600;max=3;seed=4")], "AAAA")
    rewrite_rinex(d + "/s_bln.rnx", d + "/u_bln.rnx", [parse_op("slip;types=L5?;sats=G03;rate=3600;max=3;seed=4")], "AAAA")
    up = [x[3:] for x in open(d + "/u_pad.rnx").read().split("\n")]
    ub = [x[3:] for x in open(d + "/u_bln.rnx").read().split("\n")]
    steps = [float(ub[5 + 3 * k][80:94]) - 0.7e8 for k in range(4)]
    chk(up == ub and any(abs(s) >= 1 for s in steps), f"blank-padded draw matches the padded twin {steps}")
    # 11: card EV7 — a windowed strip acts at BOTH edges of the window and nowhere else,
    #     keeps the header even with sats=all, and is a no-op on the un-windowed path.
    #     The bed epochs are 2024-05-08 00:00 .. 00:03, a Wednesday, so TOW 259200 + 60*e.
    chk(abs(epoch_tow(["2024", "05", "08", "05", "22", "00.0000000"]) - 278520.0) < 1e-6,
        "epoch_tow of 2024-129 05:22:00 is TOW 278520")
    rewrite_rinex(src, d + "/w.rnx", [parse_op("strip;types=?5?;tow_from=259260;tow_to=259320")], "AAAA")
    w = open(d + "/w.rnx").read().split("\n")
    chk(w[1] == hdr[1], "windowed strip keeps the header even with sats=all")
    g5 = [k for k, x in enumerate(w) if x.startswith("G03")]
    blanked = [w[k].ljust(99)[3 + 32:3 + 46].strip() == "" for k in g5]
    chk(blanked == [False, True, True, False], f"window honoured at both edges {blanked}")
    chk(w[g5[0]] == body[1].rstrip() and w[g5[3]] == body[13].rstrip(), "outside the window untouched")
    chk(all(w[k].ljust(99)[3 + 80:3 + 94].strip() == "" for k in g5[1:3])
        and all(x.ljust(99)[3:3 + 14].strip() for x in w if x.startswith("G03")), "windowed strip hits every ?5? type")
    chk([w[k] for k, x in enumerate(w) if x.startswith("E01")] == [body[3 + 4 * j].rstrip() for j in range(4)],
        "other system untouched by a windowed strip")
    # an open-ended window, a satellite subset, and the no-window twin byte-for-byte as before
    rewrite_rinex(src, d + "/w2.rnx", [parse_op("strip;types=?5?;sats=G03;tow_from=259320")], "AAAA")
    w2 = open(d + "/w2.rnx").read().split("\n")
    g5b = [k for k, x in enumerate(w2) if x.startswith("G03")]
    chk([w2[k].ljust(99)[3 + 32:3 + 46].strip() == "" for k in g5b] == [False, False, True, True],
        "tow_from alone is open-ended to the right")
    chk([w2[k] for k, x in enumerate(w2) if x.startswith("G05")] == [body[2 + 4 * j].rstrip() for j in range(4)],
        "another satellite untouched inside the window")
    rewrite_rinex(src, d + "/w3.rnx", [parse_op("strip;types=?5?")], "AAAA")
    chk(open(d + "/w3.rnx").read() == open(d + "/b.rnx").read(), "no window: byte-for-byte the old path")
    # 12: card SCC — `clone` adds an observation type as a noisy copy, the header count
    #     and its continuation lines are rebuilt, a blank source stays blank, the
    #     LLI / signal-strength digits are copied, and a LATER op sees the new column.
    rewrite_rinex(src, d + "/x.rnx", [parse_op("clone;from=C1C;to=C1W;noise=0.3;seed=7")], "AAAA")
    x = open(d + "/x.rnx").read().split("\n")
    chk(x[1].startswith("G    7 C1C C2W C5Q L1C L2W L5Q C1W"), f"clone header {x[1]!r}")
    chk(x[2] == hdr[2], "the cloned system's block did not displace another system's")
    g3 = x[5].ljust(3 + 16 * 7)
    chk(len(x[5].rstrip()) == 3 + 16 * 7, f"clone appends one 16-char field {len(x[5].rstrip())}")
    off, dv = 3 + 16 * 6, float(g3[3 + 16 * 6:3 + 16 * 6 + 14]) - 2.0e7
    chk(abs(dv) > 1e-4 and abs(dv) < 3.0, f"clone value is the source plus noise {dv}")
    chk(g3[off + 14:off + 16] == " 7", "clone copies the LLI / signal-strength digits")
    chk(x[5][3:3 + 16] == body[1][3:3 + 16], "the clone SOURCE column is untouched")
    # deterministic, and keyed on the epoch (so the noise is white, not a constant)
    rewrite_rinex(src, d + "/x2.rnx", [parse_op("clone;from=C1C;to=C1W;noise=0.3;seed=7")], "AAAA")
    chk(open(d + "/x2.rnx").read() == open(d + "/x.rnx").read(), "clone deterministic")
    per_ep = [float(x[5 + 4 * k].ljust(3 + 16 * 7)[3 + 16 * 6:3 + 16 * 6 + 14]) - (2.0e7 + k)
              for k in range(4)]
    chk(len(set(round(v, 6) for v in per_ep)) == 4, f"clone noise is per epoch {per_ep}")
    rewrite_rinex(src, d + "/x3.rnx", [parse_op("clone;from=C1C;to=C1W;noise=0.3;seed=8")], "AAAA")
    chk(open(d + "/x3.rnx").read() != open(d + "/x.rnx").read(), "clone seed changes the draw")
    # a blank source stays blank
    src_gap = [ln for ln in hdr + body]
    src_gap[5] = body[1][:3] + " " * 16 + body[1][3 + 16:]
    open(d + "/y.rnx", "w", newline="\n").write("\n".join(src_gap) + "\n")
    rewrite_rinex(d + "/y.rnx", d + "/y2.rnx", [parse_op("clone;from=C1C;to=C1W;noise=0.3;seed=7")], "AAAA")
    y = open(d + "/y2.rnx").read().split("\n")
    chk(y[5].ljust(3 + 16 * 7)[3 + 16 * 6:3 + 16 * 6 + 14].strip() == "", "blank source stays blank")
    chk(y[6].ljust(3 + 16 * 7)[3 + 16 * 6:3 + 16 * 6 + 14].strip() != "", "the next record still clones")
    # a LATER op acts on the cloned column, and only on it
    two = [parse_op("clone;from=C1C;to=C1W;noise=0;seed=7"),
           parse_op("bias;types=C1W;per=sat;sigma=0.5;seed=12")]
    rewrite_rinex(src, d + "/z.rnx", two, "AAAA")
    z = open(d + "/z.rnx").read().split("\n")
    inj = [float(z[5 + 4 * k].ljust(3 + 16 * 7)[3 + 16 * 6:3 + 16 * 6 + 14]) - (2.0e7 + k)
           for k in range(4)]
    chk(len(set(round(v, 6) for v in inj)) == 1 and abs(inj[0]) > 1e-4,
        f"a bias on the clone is one constant per satellite {inj}")
    chk(z[5][3:3 + 16] == body[1][3:3 + 16], "the bias did not reach the source column")
    # `draws` reproduces that constant from the spec alone — the op INDEX included.
    # The tolerance is the FILE's: RINEX writes F14.3, so an injected truth read back
    # out of the file is quantized to the millimetre.
    got = draw("12", 0, "bias", "G03").gauss(0.0, 0.5)
    chk(abs(got - inj[0]) < 1e-3, f"draws key matches the injected constant {got} vs {inj[0]}")
    # the clone is not a record op, so it does not shift a following op's index
    chk([op["op"] for op in two if op["op"] not in ("relabel", "holes", "primaryonly", "clone")]
        == ["bias"], "clone takes no record-op index")
    # header continuation: 13 types per line, so a 14th opens a second line
    hc = list(hdr)
    hc[1] = ("G   13 C1C C2W C5Q L1C L2W L5Q C1X C1L C2L C5X L1X L1L L2L").ljust(60) + LABEL
    open(d + "/c13.rnx", "w", newline="\n").write("\n".join(
        hc + ["> 2024 05 08 00 00 00.0000000  0  1", rec("G03", list(range(1, 14)))]) + "\n")
    rewrite_rinex(d + "/c13.rnx", d + "/c14.rnx", [parse_op("clone;from=C1C;to=C1W;noise=0;seed=7")], "AAAA")
    c14 = open(d + "/c14.rnx").read().split("\n")
    chk(c14[1].startswith("G   14 C1C") and c14[2].startswith("       C1W")
        and c14[2][60:].strip() == LABEL, f"continuation line {c14[1]!r} / {c14[2]!r}")
    # 13: card UPO — `ecc` writes the eccentricity into ANTENNA: DELTA H/E/N (inserting
    #     the record when the file has none), leaves every observation untouched, and
    #     LOWERS the configured coordinate by the same vector, so that the estimator's
    #     own marker -> ARP reduction returns it to where it started.
    he = list(hdr)
    he.insert(1, "        0.0000        0.0000        0.0000                  ANTENNA: DELTA H/E/N")
    open(d + "/ec0.rnx", "w", newline="\n").write("\n".join(he + body) + "\n")
    rewrite_rinex(d + "/ec0.rnx", d + "/ec1.rnx", [parse_op("ecc;stations=AAAA;h=2.0;e=0.25;n=-0.5")], "AAAA")
    e1 = open(d + "/ec1.rnx").read().split("\n")
    dl = [x for x in e1 if x[60:].strip() == "ANTENNA: DELTA H/E/N"]
    chk(len(dl) == 1 and dl[0][:42] == "%14.4f%14.4f%14.4f" % (2.0, 0.25, -0.5), f"ecc header {dl!r}")
    chk([x for x in e1 if x.startswith(("G03", "G05", "E01"))] == [b for b in body if b[:1] != ">"],
        "ecc leaves every observation record untouched")
    chk([x for x in e1 if x.startswith(">")] == [b for b in body if b[:1] == ">"],
        "ecc leaves every epoch header untouched")
    # the record is INSERTED, in the header, when the file carries none
    rewrite_rinex(src, d + "/ec2.rnx", [parse_op("ecc;stations=AAAA;h=1.5")], "AAAA")
    e2 = open(d + "/ec2.rnx").read().split("\n")
    k_rec = [k for k, x in enumerate(e2) if x[60:].strip() == "ANTENNA: DELTA H/E/N"]
    k_end = [k for k, x in enumerate(e2) if x[60:].strip() == "END OF HEADER"]
    chk(len(k_rec) == 1 and k_rec[0] < k_end[0], f"ecc inserts the record in the header {k_rec}")
    chk(e2[k_rec[0]][:42] == "%14.4f%14.4f%14.4f" % (1.5, 0.0, 0.0), f"ecc default e/n zero {e2[k_rec[0]]!r}")
    chk(len(e2) == len(hdr + body) + 2, f"ecc adds exactly one line {len(e2)}")
    # the configured coordinate goes DOWN by h, and the estimator's markerToArp
    # (the same rotation, opposite sign, at the lowered point) returns it to within
    # the O(|ecc|^2/R) closure of a single-shot inverse
    pos = [4194423.4920, 1162703.0403, 4647245.6154]          # GRAZ00AUT
    hen = (1.9640, 0.0, 0.0)
    mk = lower_to_marker(pos, hen)
    lat, lon, h0 = ecef_to_llh(*pos)
    _, _, h1 = ecef_to_llh(*mk)
    chk(abs((h0 - h1) - hen[0]) < 1e-6, f"ecc lowers the coordinate by h {h0 - h1}")
    la, lo, _ = ecef_to_llh(*mk)
    dd = enu_to_ecef((hen[1], hen[2], hen[0]), la, lo)         # markerToArp, at the marker
    back = [mk[k] + dd[k] for k in range(3)]
    chk(max(abs(back[k] - pos[k]) for k in range(3)) < 1e-5,
        f"markerToArp of the lowered coordinate returns the original {back} vs {pos}")
    # a three-component vector lands on up / east / north, in that header order
    mk3 = lower_to_marker(pos, (2.0, 0.25, -0.5))
    la3, lo3, _ = ecef_to_llh(*pos)
    se, ce, sl, cl = math.sin(lo3), math.cos(lo3), math.sin(la3), math.cos(la3)
    dv = (pos[0] - mk3[0], pos[1] - mk3[1], pos[2] - mk3[2])
    enu = (-se * dv[0] + ce * dv[1],
           -sl * ce * dv[0] - sl * se * dv[1] + cl * dv[2],
           cl * ce * dv[0] + cl * se * dv[1] + sl * dv[2])
    chk(abs(enu[2] - 2.0) < 1e-6 and abs(enu[0] - 0.25) < 1e-6 and abs(enu[1] + 0.5) < 1e-6,
        f"ecc uses RINEX (H, E, N) order {enu}")
    # an ecc op is structural: it takes no record-op index and does not shift a bias
    chk([op["op"] for op in [parse_op("ecc;stations=AAAA;h=1"), parse_op("bias;types=C1C;sigma=1;seed=3")]
         if op["op"] not in ("relabel", "holes", "primaryonly", "clone", "ecc")] == ["bias"],
        "ecc takes no record-op index")
    try:
        parse_op("ecc;h=1.0")
        chk(False, "ecc with stations=all must be refused")
    except AssertionError as ex:
        chk("ecc must name its stations" in str(ex), f"ecc stations=all refused {ex}")
    print(f"rapid_arm selftest: {n}/{n} PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--base", required=True)
    m.add_argument("--tag", required=True)
    m.add_argument("--hours", type=float)
    m.add_argument("--set", action="append")
    m.add_argument("--drop-station", action="append")
    m.add_argument("--rinex", action="append")
    m.set_defaults(fn=cmd_make)
    r = sub.add_parser("run")
    r.add_argument("--tags", nargs="+", required=True)
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--exe", default=ROOT + "/build/x64-Release/gnss_processor.exe")
    r.set_defaults(fn=cmd_run)
    s = sub.add_parser("score")
    s.add_argument("tags", nargs="+")
    s.add_argument("--pf", action="store_true", help="add the median-station post-fit phase RMS per band (slow)")
    s.set_defaults(fn=cmd_score)
    dw = sub.add_parser("draws")
    dw.add_argument("--op", action="append", required=True,
                    help="an op spec, repeatable, in the SAME order make --rinex takes them")
    dw.add_argument("--ids", required=True,
                    help="comma list of the draw keys to print (satellites for per=sat, stations for per=station)")
    dw.set_defaults(fn=cmd_draws)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
