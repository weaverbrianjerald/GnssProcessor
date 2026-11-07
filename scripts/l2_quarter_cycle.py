#!/usr/bin/env python3
"""L2C against L2W quarter-cycle alignment, read from raw RINEX 3 observation files.

    python scripts/l2_quarter_cycle.py headers <obs_dir>... --out <dir>/headers.csv
    python scripts/l2_quarter_cycle.py arcs    <obs_dir>... --out <dir>/arcs.csv [--workers N]
    python scripts/l2_quarter_cycle.py grade   --headers <csv> --arcs <csv> --out <dir>
                                               [--golden <config.json>]
    python scripts/l2_quarter_cycle.py selftest

The question.  A receiver tracks GPS L2C (L2S, L2L or L2X) and semi-codeless L2P(Y) (L2W) on
one carrier.  The two signals sit in phase quadrature at the satellite, so a receiver that
reports both raw may put the L2C phase a quarter of a cycle from the L2W phase; RINEX 3 asks
the writer to align them and to document the applied shift in SYS / PHASE SHIFT.  Whether the
files of the field network are aligned, per receiver type and firmware, is read here from the
data.

What is formed.  Per station, satellite and epoch, d = L2C - L2W in cycles, both phases as
recorded.  Geometry, clocks, troposphere, ionosphere (one frequency), wind-up and antenna
patterns cancel exactly in this difference.  What remains is an integer (the two ambiguities),
the alignment shift, the inter-signal phase bias between the two tracking modes at the
receiver and at the satellite, and noise.  Its fractional part is invariant to an integer slip
on either signal, so an arc here is broken only by a gap, a loss-of-lock flag, or a
non-integer jump of the difference itself (a half-cycle event, a realignment).  Each arc
yields one circular mean of the fraction.  A station offset is the circular mean over its
arcs, its uncertainty the arc-to-arc scatter over the square root of the number of arcs;
epochs within an arc are not independent (multipath), arcs nearly are.

The null.  A fraction is defined modulo one cycle.  Concentration at the quarter targets
{0, 1/4, 1/2, 3/4} is tested on the angle 4 * 2 * pi * frac (a Rayleigh test), and the mean
distance to the nearest quarter target is quoted against the same statistic on N uniform
fractions (expectation 1/16 cycle); the mean distance to zero against its uniform value
(1/4 cycle).  No group is centred on its own circular median before a dispersion is quoted
(that shrinks |residual| about 42 % on uniform noise); where a within-group dispersion is
given at all, the size-matched null of the same centring is given beside it.

Satellite identifiers are read through scripts/rinex_ids.py (a blank-padded "G 7" is "G07").
RINEX 2 files carry no L2C / L2W distinction and are listed, not analysed; RINEX 4 observation
files share the RINEX 3 record layout and are read.
"""
import argparse
import calendar
import cmath
import csv
import glob
import json
import math
import os
import random
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")
sys.path.insert(0, HERE)
from rinex_ids import norm_sat  # noqa: E402

L2C_CODES = ("L2S", "L2L", "L2X")
L1C_CODES = ("L1S", "L1L", "L1X")
REF = {"2": "L2W", "1": "L1C"}

MAX_GAP_S = 300.0      # a longer gap ends the arc
JUMP_TOL_CYC = 0.15    # a larger wrapped step of the difference ends the arc
MIN_ARC_EPOCHS = 20    # shorter arcs are counted and dropped


def wrap(x):
    """x reduced to [-0.5, 0.5)."""
    return x - math.floor(x + 0.5)


def circ_mean(fracs):
    """Circular mean of fractions (cycles) and the mean resultant length."""
    n = len(fracs)
    if n == 0:
        return float("nan"), 0.0
    z = sum(cmath.exp(2j * math.pi * f) for f in fracs)
    r = abs(z) / n
    return (cmath.phase(z) / (2 * math.pi) if r > 0 else 0.0), r


def wrapped_rms(fracs, centre):
    if not fracs:
        return float("nan")
    return math.sqrt(sum(wrap(f - centre) ** 2 for f in fracs) / len(fracs))


def quarter_distance(f):
    """Distance in cycles from f to the nearest of 0, 1/4, 1/2, 3/4."""
    return abs(wrap(4.0 * f)) / 4.0


# ------------------------------------------------------------------ header reading

def file_day(path):
    """(year, doy) from a RINEX 3 long name ..._R_YYYYDDDhhmm_..., else from a 'YYYY-DDD'
    directory on the path, else (0, 0)."""
    m = re.search(r"_[RSU]_(\d{4})(\d{3})\d{4}_", os.path.basename(path))
    if m:
        return int(m.group(1)), int(m.group(2))
    m = re.search(r"(\d{4})-(\d{3})", path.replace("\\", "/"))
    if m:
        return int(m.group(1)), int(m.group(2))
    return 0, 0


def read_header(path):
    """The header fields this analysis needs, as a dict.  Reads until END OF HEADER."""
    h = {"file": path.replace("\\", "/"), "version": "", "pgm": "", "run_by": "",
         "rec_type": "", "rec_fw": "", "ant_type": "", "marker": "",
         "obs_types": {}, "shifts": []}
    pending = None
    rec = None
    with open(path, "r", encoding="latin-1", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            label = line[60:].strip()
            if label == "RINEX VERSION / TYPE":
                h["version"] = line[0:9].strip()
            elif label == "PGM / RUN BY / DATE":
                h["pgm"] = line[0:20].strip()
                h["run_by"] = line[20:40].strip()
            elif label == "MARKER NAME":
                h["marker"] = line[0:60].strip()
            elif label == "REC # / TYPE / VERS":
                h["rec_type"] = line[20:40].strip()
                h["rec_fw"] = line[40:60].strip()
            elif label == "ANT # / TYPE":
                h["ant_type"] = line[20:40].strip()
            elif label == "SYS / # / OBS TYPES":
                if line[0] != " ":
                    pending = line[0]
                    h["obs_types"][pending] = []
                if pending is None:
                    continue
                for k in range(13):
                    t = line[7 + 4 * k:10 + 4 * k].strip()
                    if t:
                        h["obs_types"][pending].append(t)
            elif label == "SYS / PHASE SHIFT":
                if line[:18].strip():
                    nsat = line[16:18].strip()
                    rec = {"sys": line[0], "code": line[2:5].strip(), "value": line[6:14].strip(),
                           "nsat": int(nsat) if nsat else 0, "sats": []}
                    h["shifts"].append(rec)
                if rec is not None and rec["nsat"]:
                    for k in range(10):
                        s = line[19 + 4 * k:22 + 4 * k]
                        if s.strip():
                            rec["sats"].append(norm_sat(s))
            elif label == "END OF HEADER":
                break
    return h


def shift_text(h, sysc="G"):
    parts = []
    for r in h["shifts"]:
        if r["sys"] != sysc:
            continue
        t = "%s=%s" % (r["code"], r["value"] if r["value"] else "blank")
        if r["nsat"]:
            t += "[%d:%s]" % (r["nsat"], ",".join(r["sats"]))
        parts.append(t)
    return ";".join(parts)


HEADER_FIELDS = ["year", "doy", "station", "version", "pgm", "run_by", "rec_type", "rec_fw",
                 "ant_type", "gps_phase_types", "has_L2W", "l2c_codes", "l1c_codes", "gps_shifts"]


def station_of(path):
    base = os.path.basename(path)
    m = re.match(r"([A-Za-z0-9]{9})_", base)
    if m:
        return m.group(1).upper()
    return os.path.basename(os.path.dirname(path)).upper()


def header_row(path):
    h = read_header(path)
    y, d = file_day(path)
    g = h["obs_types"].get("G", [])
    phases = [t for t in g if t.startswith("L")]
    return {"year": y, "doy": d, "station": station_of(path), "version": h["version"],
            "pgm": h["pgm"], "run_by": h["run_by"], "rec_type": h["rec_type"],
            "rec_fw": h["rec_fw"], "ant_type": h["ant_type"],
            "gps_phase_types": " ".join(phases), "has_L2W": int("L2W" in g),
            "l2c_codes": " ".join(c for c in L2C_CODES if c in g),
            "l1c_codes": " ".join(c for c in L1C_CODES if c in g),
            "gps_shifts": shift_text(h, "G")}


def list_files(obs_dirs):
    files = []
    for d in obs_dirs:
        files += glob.glob(os.path.join(d, "*", "*.rnx")) + glob.glob(os.path.join(d, "*", "*.*o")) \
            + glob.glob(os.path.join(d, "*.rnx"))
    return sorted(set(f.replace("\\", "/") for f in files))


def cmd_headers(args):
    files = list_files(args.obs_dirs)
    rows = [header_row(f) for f in files]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=HEADER_FIELDS)
        w.writeheader()
        w.writerows(rows)
    n3 = sum(1 for r in rows if r["version"][:1] in ("3", "4"))
    both = sum(1 for r in rows if r["has_L2W"] and r["l2c_codes"])
    print("%d files, %d RINEX 3 or 4, %d with L2W and an L2C code -> %s" % (len(rows), n3, both, args.out))
    return 0


# --------------------------------------------------------------------- arc reading

ARC_FIELDS = ["year", "doy", "station", "sat", "pair", "t0", "t1", "n", "frac", "rms",
              "n_half", "break_reason"]


def epoch_seconds(line):
    """Seconds since 1980-01-06 of a RINEX 3 epoch record, or None."""
    try:
        y = int(line[2:6]); mo = int(line[7:9]); d = int(line[10:12])
        hh = int(line[13:15]); mi = int(line[16:18]); s = float(line[18:29])
    except ValueError:
        return None
    return calendar.timegm((y, mo, d, hh, mi, 0)) - 315964800 + s


class Arc:
    __slots__ = ("t0", "t1", "d", "n_half", "reason")

    def __init__(self, t, reason):
        self.t0 = t
        self.t1 = t
        self.d = []
        self.n_half = 0
        self.reason = reason


def flush(arc, key, meta, out, counts):
    if arc is None:
        return
    if len(arc.d) < MIN_ARC_EPOCHS:
        counts["short_arcs"] += 1
        return
    mean, r = circ_mean(arc.d)
    out.append({"year": meta[0], "doy": meta[1], "station": meta[2], "sat": key[0],
                "pair": key[1], "t0": "%.0f" % arc.t0, "t1": "%.0f" % arc.t1, "n": len(arc.d),
                "frac": "%.5f" % mean, "rms": "%.5f" % wrapped_rms(arc.d, mean),
                "n_half": arc.n_half, "break_reason": arc.reason})


def read_arcs(path):
    """Per-arc rows of every (satellite, L2C-L2W pair) and (satellite, L1C-L1C/A pair) of
    one RINEX 3 file, and a dict of counts."""
    counts = defaultdict(int)
    h = read_header(path)
    y, doy = file_day(path)
    meta = (y, doy, station_of(path))
    if h["version"][:1] not in ("3", "4"):
        counts["rinex2_files"] += 1
        return [], counts
    g = h["obs_types"].get("G", [])
    pairs = []   # (pair name, index of the L2C-type code, index of the reference code)
    for band, ref in REF.items():
        if ref not in g:
            continue
        for code in (L2C_CODES if band == "2" else L1C_CODES):
            if code in g:
                pairs.append(("%s-%s" % (code, ref), g.index(code), g.index(ref)))
    if not pairs:
        counts["files_without_pair"] += 1
        return [], counts
    arcs = {}
    last = {}
    out = []
    with open(path, "r", encoding="latin-1", errors="replace") as fh:
        in_header = True
        for raw in fh:
            if in_header:
                if raw[60:73] == "END OF HEADER":
                    in_header = False
                continue
            if raw[0] != ">":
                continue
            flag = raw[31:32]
            try:
                nsat = int(raw[32:35])
            except ValueError:
                nsat = 0
            if flag not in ("0", "1"):
                for _ in range(nsat):
                    next(fh, "")
                continue
            t = epoch_seconds(raw)
            counts["epochs"] += 1
            for _ in range(nsat):
                rec = next(fh, "")
                if rec[0:1] != "G":
                    continue
                sat = norm_sat(rec[0:3])
                for pair, ic, ir in pairs:
                    a = 3 + 16 * ic
                    b = 3 + 16 * ir
                    vc = rec[a:a + 14].strip()
                    vr = rec[b:b + 14].strip()
                    if not vc or not vr:
                        continue
                    try:
                        d = float(vc) - float(vr)
                    except ValueError:
                        continue
                    if d == 0.0:
                        # the two phases agree to the printed millicycle: real data at a
                        # zero offset, kept; the per-file share is the duplicate-column check
                        counts["zero_difference"] += 1
                    lc = rec[a + 14:a + 15]
                    lr = rec[b + 14:b + 15]
                    lli_c = int(lc) if lc.strip() else 0
                    lli_r = int(lr) if lr.strip() else 0
                    counts["sat_epochs"] += 1
                    key = (sat, pair)
                    arc = arcs.get(key)
                    reason = None
                    if arc is None:
                        reason = "start"
                    elif t - arc.t1 > MAX_GAP_S:
                        reason = "gap"
                    elif (lli_c | lli_r) & 1:
                        reason = "lli"
                    elif abs(wrap(d - last[key])) > JUMP_TOL_CYC:
                        reason = "jump"
                    if reason is not None:
                        counts["break_" + reason] += 1
                        flush(arc, key, meta, out, counts)
                        arc = Arc(t, reason)
                        arcs[key] = arc
                    last[key] = d
                    if (lli_c | lli_r) & 2:
                        arc.n_half += 1
                        counts["half_cycle_epochs"] += 1
                        continue
                    arc.d.append(d)
                    arc.t1 = t
    for key, arc in arcs.items():
        flush(arc, key, meta, out, counts)
    counts["arcs"] += len(out)
    return out, counts


def _worker(path):
    try:
        return path, read_arcs(path)
    except Exception as exc:  # one bad file must not end the sweep
        return path, ([], {"error:%s" % exc: 1})


def cmd_arcs(args):
    files = list_files(args.obs_dirs)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    total = defaultdict(int)
    rows = []
    per_file = []
    keys = ("epochs", "sat_epochs", "arcs", "short_arcs", "zero_difference", "half_cycle_epochs",
            "break_gap", "break_lli", "break_jump")

    def take(path, out, counts):
        rows.extend(out)
        for k, v in counts.items():
            total[k] += v
        y, doy = file_day(path)
        per_file.append({"year": y, "doy": doy, "station": station_of(path),
                         **{k: counts.get(k, 0) for k in keys}})

    if args.workers > 1:
        from multiprocessing import Pool
        with Pool(args.workers) as pool:
            for path, (out, counts) in pool.imap_unordered(_worker, files, chunksize=1):
                take(path, out, counts)
    else:
        for path in files:
            _, (out, counts) = _worker(path)
            take(path, out, counts)
    per_file.sort(key=lambda r: (r["year"], r["doy"], r["station"]))
    files_csv = os.path.splitext(args.out)[0] + "_files.csv"
    with open(files_csv, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["year", "doy", "station"] + list(keys))
        w.writeheader()
        w.writerows(per_file)
    rows.sort(key=lambda r: (r["year"], r["doy"], r["station"], r["pair"], r["sat"], int(r["t0"])))
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=ARC_FIELDS)
        w.writeheader()
        w.writerows(rows)
    print("%d files -> %d arcs -> %s (per-file counts: %s)" % (len(files), len(rows), args.out, files_csv))
    for k in sorted(total):
        print("  %-24s %d" % (k, total[k]))
    return 0


# ------------------------------------------------------------------------- grading

def rayleigh_p(n, rbar):
    """Rayleigh test p-value for mean resultant length rbar of n angles (uniform null)."""
    if n == 0:
        return float("nan")
    z = n * rbar * rbar
    p = math.exp(-z) * (1 + (2 * z - z * z) / (4 * n)
                        - (24 * z - 132 * z ** 2 + 76 * z ** 3 - 9 * z ** 4) / (288 * n * n))
    return max(0.0, min(1.0, p))


def null_quarter(n, draws, rng):
    """Size-matched null of the mean distance to the nearest quarter target: the mean and
    the 1st percentile over `draws` sets of n uniform fractions."""
    vals = []
    for _ in range(draws):
        vals.append(sum(quarter_distance(rng.random()) for _ in range(n)) / n)
    vals.sort()
    return sum(vals) / len(vals), vals[max(0, int(0.01 * len(vals)) - 1)]


def null_median_centred(n, draws, rng):
    """Size-matched null of a group centred on its own circular median: the mean
    |residual| after subtracting the circular median of n uniform fractions."""
    vals = []
    for _ in range(draws):
        f = [rng.random() for _ in range(n)]
        best = None
        for c in f:
            v = sum(abs(wrap(x - c)) for x in f) / n
            if best is None or v < best:
                best = v
        vals.append(best)
    return sum(vals) / len(vals)


def station_stats(fracs):
    """Offset, uncertainty and the null-aware statistics of one station's arc fractions."""
    n = len(fracs)
    mu, r = circ_mean(fracs)
    rms = wrapped_rms(fracs, mu)
    se = rms / math.sqrt(n) if n else float("nan")
    target = round(4 * mu) / 4.0
    if target == -0.5:
        target = 0.5
    _, r4 = circ_mean([4 * f for f in fracs])
    return {"n_arcs": n, "offset": mu, "rms": rms, "se": se, "target": target,
            "gap": wrap(mu - target), "rbar": r, "rbar4": r4, "p4": rayleigh_p(n, r4),
            "q_dist": sum(quarter_distance(f) for f in fracs) / n if n else float("nan"),
            "zero_dist": sum(abs(wrap(f)) for f in fracs) / n if n else float("nan")}


def golden_stations(path):
    if not path:
        return set()
    with open(path, encoding="utf-8") as fh:
        cfg = json.load(fh)
    names = set()
    net = cfg.get("network", {})
    for st in net.get("stations", []):
        p = st.get("rinex_path", "")
        if p:
            names.add(station_of(p))
    cl = net.get("closed_loop", {}).get("station", {})
    if cl.get("rinex_path"):
        names.add(station_of(cl["rinex_path"]))
    return names


def cmd_grade(args):
    rng = random.Random(20261106)
    with open(args.headers, newline="") as fh:
        headers = list(csv.DictReader(fh))
    with open(args.arcs, newline="") as fh:
        arcs = list(csv.DictReader(fh))
    golden = golden_stations(args.golden)
    hdr = {(int(r["year"]), int(r["doy"]), r["station"]): r for r in headers}

    # per (day, station, pair): the arc fractions
    by_station = defaultdict(list)
    for a in arcs:
        by_station[(int(a["year"]), int(a["doy"]), a["station"], a["pair"])].append(float(a["frac"]))

    os.makedirs(args.out, exist_ok=True)
    station_rows = []
    for key in sorted(by_station):
        y, doy, sta, pair = key
        st = station_stats(by_station[key])
        h = hdr.get((y, doy, sta), {})
        code = pair.split("-")[0]
        rec_shift = ""
        for part in (h.get("gps_shifts", "") or "").split(";"):
            if part.startswith(code + "="):
                rec_shift = part[len(code) + 1:]
        row = {"year": y, "doy": doy, "station": sta, "golden": int(sta in golden), "pair": pair,
               "rec_type": h.get("rec_type", ""), "rec_fw": h.get("rec_fw", ""),
               "pgm": h.get("pgm", ""), "ant_type": h.get("ant_type", ""),
               "header_shift": rec_shift if rec_shift else "none",
               "n_arcs": st["n_arcs"], "offset_cyc": "%.4f" % st["offset"], "se_cyc": "%.4f" % st["se"],
               "arc_rms_cyc": "%.4f" % st["rms"], "target": "%.2f" % st["target"],
               "gap_cyc": "%.4f" % st["gap"], "rbar4": "%.4f" % st["rbar4"], "p_rayleigh4": "%.2e" % st["p4"],
               "q_dist": "%.4f" % st["q_dist"], "zero_dist": "%.4f" % st["zero_dist"]}
        station_rows.append(row)
    sf = ["year", "doy", "station", "golden", "pair", "rec_type", "rec_fw", "pgm", "ant_type",
          "header_shift", "n_arcs", "offset_cyc", "se_cyc", "arc_rms_cyc", "target", "gap_cyc",
          "rbar4", "p_rayleigh4", "q_dist", "zero_dist"]
    with open(os.path.join(args.out, "stations.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=sf)
        w.writeheader()
        w.writerows(station_rows)

    # per receiver type (and firmware, and converter): every station-day pooled
    def group_table(keyfn, name, pairs_filter):
        groups = defaultdict(list)
        for r in station_rows:
            if not pairs_filter(r["pair"]):
                continue
            groups[keyfn(r)].append(r)
        lines = []
        for gk in sorted(groups, key=lambda k: (-len(groups[k]), k)):
            rows = groups[gk]
            offs = [float(r["offset_cyc"]) for r in rows]
            narcs = sum(int(r["n_arcs"]) for r in rows)
            mu, _ = circ_mean(offs)
            spread = max(abs(wrap(o - mu)) for o in offs)
            at = defaultdict(int)
            for o in offs:
                t = round(4 * o) / 4.0
                if t == -0.5:
                    t = 0.5
                at["%.2f" % t if abs(wrap(o - t)) <= 0.05 else "none"] += 1
            stations = sorted(set(r["station"] for r in rows))
            lines.append({"group": " | ".join(gk), "n_station_days": len(rows),
                          "n_stations": len(stations), "n_arcs": narcs,
                          "offset_cyc": "%.4f" % mu, "max_dev_cyc": "%.4f" % spread,
                          "at_0": at["0.00"], "at_+0.25": at["0.25"], "at_-0.25": at["-0.25"],
                          "at_0.5": at["0.50"], "at_none": at["none"],
                          "pairs": " ".join(sorted(set(r["pair"] for r in rows))),
                          "header_shifts": " ".join(sorted(set(r["header_shift"] for r in rows)))})
        gf = ["group", "n_station_days", "n_stations", "n_arcs", "offset_cyc", "max_dev_cyc",
              "at_0", "at_+0.25", "at_-0.25", "at_0.5", "at_none", "pairs", "header_shifts"]
        with open(os.path.join(args.out, name), "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=gf)
            w.writeheader()
            w.writerows(lines)
        return lines

    def is_l2(p):
        return p.endswith("-L2W")

    def is_l1(p):
        return p.endswith("-L1C")

    group_table(lambda r: (r["rec_type"],), "receivers_l2.csv", is_l2)
    group_table(lambda r: (r["rec_type"], r["rec_fw"]), "receivers_fw_l2.csv", is_l2)
    group_table(lambda r: (r["rec_type"], r["pgm"]), "receivers_pgm_l2.csv", is_l2)
    group_table(lambda r: (r["rec_type"],), "receivers_l1.csv", is_l1)

    # the pooled null-aware statistics, L2 pairs, all arcs
    l2 = [float(a["frac"]) for a in arcs if is_l2(a["pair"])]
    n = len(l2)
    with open(os.path.join(args.out, "pooled.txt"), "w") as fh:
        if n:
            pooled = station_stats(l2)
            nn = min(n, 5000)
            null_mean, null_p01 = null_quarter(nn, 400, rng)
            fh.write("L2C-L2W arcs: %d\n" % n)
            fh.write("mean distance to the nearest quarter target: %.4f cyc (uniform null %.4f; "
                     "size-matched null of %d uniform fractions: mean %.4f, 1st percentile %.4f)\n"
                     % (pooled["q_dist"], 1 / 16.0, nn, null_mean, null_p01))
            fh.write("mean distance to zero: %.4f cyc (uniform null 0.2500)\n" % pooled["zero_dist"])
            fh.write("Rayleigh on 4*angle: Rbar4 %.4f, p %.2e; on the angle: Rbar %.4f\n"
                     % (pooled["rbar4"], pooled["p4"], pooled["rbar"]))
            nm = min(n, 2000)
            fh.write("median-centred |residual| null for n=%d uniform fractions: %.4f cyc "
                     "(0.2500 uncentred)\n" % (nm, null_median_centred(nm, 50, rng)))
    detail_outputs(args.out, arcs, station_rows, golden, headers, is_l2)
    print("wrote %s: stations.csv (%d rows), receivers_*.csv, pooled.txt, satellites.csv, "
          "repeatability.csv, golden_coverage.csv, arc_histogram.txt" % (args.out, len(station_rows)))
    return 0


def detail_outputs(out_dir, arcs, station_rows, golden, headers, is_l2):
    """The finer structure behind the station table: where the arc fractions lie (and
    where the arcs that a non-integer jump opened lie), the per-satellite residual after
    each station-day's own offset is removed, the day-to-day repeatability of a station,
    and the golden roster's coverage."""
    from collections import Counter

    def band(f):
        if abs(f) < 0.05:
            return "|f|<0.05"
        if abs(f) < 0.15:
            return "0.05<=|f|<0.15"
        if abs(wrap(f - 0.25)) < 0.05 or abs(wrap(f + 0.25)) < 0.05:
            return "within 0.05 of +-0.25"
        if abs(wrap(f - 0.5)) < 0.05:
            return "within 0.05 of 0.5"
        return "elsewhere"

    offset_of = {(r["year"], r["doy"], r["station"], r["pair"]): float(r["offset_cyc"]) for r in station_rows}
    rec_of = {(int(h["year"]), int(h["doy"]), h["station"]): h["rec_type"] for h in headers}
    l2 = [a for a in arcs if is_l2(a["pair"])]
    hist = Counter(band(float(a["frac"])) for a in l2)
    by_reason = defaultdict(Counter)
    half_by_rec = Counter()
    arcs_by_rec = Counter()
    for a in l2:
        f = float(a["frac"])
        by_reason[a["break_reason"]][band(f)] += 1
        rec = rec_of.get((int(a["year"]), int(a["doy"]), a["station"]), "")
        arcs_by_rec[rec] += 1
        if abs(wrap(f - 0.5)) < 0.05:
            half_by_rec[rec] += 1
    with open(os.path.join(out_dir, "arc_histogram.txt"), "w") as fh:
        fh.write("L2C-L2W arcs by fraction band (%d arcs)\n" % len(l2))
        for k in ("|f|<0.05", "0.05<=|f|<0.15", "within 0.05 of +-0.25", "within 0.05 of 0.5", "elsewhere"):
            fh.write("  %-24s %7d  (%.2f %%)\n" % (k, hist[k], 100.0 * hist[k] / max(1, len(l2))))
        fh.write("\nby the reason that opened the arc\n")
        for reason in ("start", "gap", "lli", "jump"):
            c = by_reason[reason]
            tot = sum(c.values())
            fh.write("  %-6s %7d: %s\n" % (reason, tot, ", ".join("%s %d" % (k, c[k]) for k in
                     ("|f|<0.05", "0.05<=|f|<0.15", "within 0.05 of +-0.25", "within 0.05 of 0.5", "elsewhere"))))
        fh.write("\narcs within 0.05 of 0.5 cycle (a half-cycle event) by receiver type\n")
        for rec, n in sorted(arcs_by_rec.items(), key=lambda kv: -kv[1]):
            fh.write("  %-22s %6d of %6d arcs\n" % (rec, half_by_rec[rec], n))

    # per-satellite residual structure, after the station-day's own offset is removed
    per_sat = defaultdict(list)
    per_rec_sat = defaultdict(list)
    for a in l2:
        key = (int(a["year"]), int(a["doy"]), a["station"], a["pair"])
        res = wrap(float(a["frac"]) - offset_of[key])
        if abs(res) > 0.15:
            continue   # a half-cycle or elsewhere arc is not a bias sample
        per_sat[a["sat"]].append(res)
        per_rec_sat[(rec_of.get(key[:3], ""), a["sat"])].append(res)
    with open(os.path.join(out_dir, "satellites.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["sat", "n_arcs", "mean_residual_cyc", "se_cyc", "rms_cyc"])
        for sat in sorted(per_sat):
            v = per_sat[sat]
            mu, _ = circ_mean(v)
            rms = wrapped_rms(v, mu)
            w.writerow([sat, len(v), "%.4f" % mu, "%.4f" % (rms / math.sqrt(len(v))), "%.4f" % rms])
    with open(os.path.join(out_dir, "satellites_by_receiver.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["rec_type", "sat", "n_arcs", "mean_residual_cyc", "se_cyc"])
        for key in sorted(per_rec_sat):
            v = per_rec_sat[key]
            if len(v) < 10:
                continue
            mu, _ = circ_mean(v)
            w.writerow([key[0], key[1], len(v), "%.4f" % mu, "%.4f" % (wrapped_rms(v, mu) / math.sqrt(len(v)))])

    # day-to-day repeatability of a station's offset
    by_sta = defaultdict(dict)
    for r in station_rows:
        if is_l2(r["pair"]):
            by_sta[(r["station"], r["pair"], r["rec_type"])][r["doy"]] = float(r["offset_cyc"])
    with open(os.path.join(out_dir, "repeatability.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["station", "pair", "rec_type", "n_days", "offsets_by_day", "max_spread_cyc"])
        for key in sorted(by_sta):
            days = by_sta[key]
            if len(days) < 2:
                continue
            vals = list(days.values())
            spread = max(abs(wrap(a - b)) for a in vals for b in vals)
            w.writerow([key[0], key[1], key[2], len(days),
                        " ".join("%s:%.4f" % (d, days[d]) for d in sorted(days)), "%.4f" % spread])

    # the golden roster: which of its stations hold both signals, under which receiver
    covered = {r["station"]: r for r in station_rows if is_l2(r["pair"]) and r["doy"] == 129}
    with open(os.path.join(out_dir, "golden_coverage.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["station", "has_pair_129", "pair", "rec_type", "offset_cyc", "n_arcs", "header_shift"])
        for sta in sorted(golden):
            r = covered.get(sta)
            h = rec_of.get((2024, 129, sta), "")
            if r:
                w.writerow([sta, 1, r["pair"], r["rec_type"], r["offset_cyc"], r["n_arcs"], r["header_shift"]])
            else:
                w.writerow([sta, 0, "", h, "", "", ""])


# ------------------------------------------------------------------------ selftest

def _synthetic_file(path):
    """A small RINEX 3 file: G07 (blank-padded 'G 7') with L2L = L2W + 7.25 cycles, G12 with
    L2S = L2W + 3.00 for 25 epochs then + 3.50 (a half-cycle event), G20 with L2W only."""
    hdr = [
        "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE",
        "selftest            none                20240508 000000 UTC PGM / RUN BY / DATE",
        "TEST                                                        MARKER NAME",
        "123                 SEPT POLARX5        5.3.2               REC # / TYPE / VERS",
        "456                 TRM57971.00     NONE                    ANT # / TYPE",
        "G    6 C1C L1C C2W L2W L2L L2S                              SYS / # / OBS TYPES",
        "G L1C                                                       SYS / PHASE SHIFT",
        "G L2W                                                       SYS / PHASE SHIFT",
        "G L2L -0.25000                                              SYS / PHASE SHIFT",
        "G L2S  0.25000  12 G01 G02 G03 G04 G05 G06 G07 G08 G09 G10  SYS / PHASE SHIFT",
        "                   G11 G 2                                  SYS / PHASE SHIFT",
        "                                                            END OF HEADER",
    ]
    def rec(sat, vals):
        # one 16-column field per observation type: F14.3, LLI, SNR; a missing value is blank
        return sat + "".join(("%14.3f  " % v) if v is not None else " " * 16 for v in vals)

    lines = list(hdr)
    for k in range(50):
        t = 30 * k
        lines.append("> 2024 05 08 00 %02d%11.7f  0  3" % (t // 60, t % 60))
        w = 100000000.0 + 1234.5 * k
        s = 3.0 if k < 25 else 3.5
        #                    C1C          L1C  C2W          L2W  L2L        L2S
        lines.append(rec("G 7", [20000000.0, w, 20000000.0, w, w + 7.25, None]))
        lines.append(rec("G12", [21000000.0, w, 21000000.0, w, None, w + s]))
        lines.append(rec("G20", [22000000.0, w, 22000000.0, w, None, None]))
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def selftest():
    import tempfile
    n = 0

    def chk(cond, what):
        nonlocal n
        n += 1
        assert cond, what

    chk(abs(wrap(0.75) - (-0.25)) < 1e-12 and abs(wrap(-0.3) - (-0.3)) < 1e-12, "wrap")
    chk(abs(quarter_distance(0.27) - 0.02) < 1e-9 and abs(quarter_distance(0.9) - 0.1) < 1e-9,
        "quarter distance")
    mu, r = circ_mean([0.49, -0.49])
    chk(abs(abs(mu) - 0.5) < 1e-9 and r > 0.99, "circular mean across the wrap")
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(d + "/sta/test00xxx")
        path = d + "/sta/test00xxx/TEST00XXX_R_20241290000_01D_30S_MO.rnx"
        _synthetic_file(path)
        h = read_header(path)
        chk(h["rec_type"] == "SEPT POLARX5" and h["rec_fw"] == "5.3.2", "receiver fields")
        chk(h["obs_types"]["G"] == ["C1C", "L1C", "C2W", "L2W", "L2L", "L2S"], "obs types")
        sh = {r["code"]: r for r in h["shifts"] if r["sys"] == "G"}
        chk(sh["L2W"]["value"] == "" and sh["L2L"]["value"] == "-0.25000", "shift values")
        chk(sh["L2S"]["nsat"] == 12 and sh["L2S"]["sats"][-1] == "G02" and len(sh["L2S"]["sats"]) == 12,
            "a per-satellite list over a continuation record, blank-padded id normalised")
        row = header_row(path)
        chk(row["gps_shifts"].startswith("L1C=blank;L2W=blank;L2L=-0.25000;L2S=0.25000[12:"),
            "shift text %s" % row["gps_shifts"])
        chk(row["l2c_codes"] == "L2S L2L" and row["has_L2W"] == 1, "L2 code inventory")
        out, counts = read_arcs(path)
        got = {(r["sat"], r["pair"]): r for r in out}
        chk(("G07", "L2L-L2W") in got, "blank-padded G 7 read as G07: %s" % sorted(got))
        chk(abs(float(got[("G07", "L2L-L2W")]["frac"]) - 0.25) < 1e-6
            and got[("G07", "L2L-L2W")]["n"] == 50, "G07 L2L-L2W fraction 0.25 over 50 epochs")
        g12 = sorted((r for r in out if r["sat"] == "G12"), key=lambda r: int(r["t0"]))
        chk(len(g12) == 2 and g12[1]["break_reason"] == "jump", "half-cycle event splits G12")
        chk(abs(float(g12[0]["frac"])) < 1e-6 and abs(abs(float(g12[1]["frac"])) - 0.5) < 1e-6,
            "G12 fractions 0 then 0.5")
        chk(not any(r["sat"] == "G20" for r in out), "no pair without L2C")
        chk(counts["epochs"] == 50, "50 epochs read")
        st = station_stats([0.251, 0.249, 0.252, 0.248])
        chk(abs(st["target"] - 0.25) < 1e-9 and abs(st["gap"]) < 0.002 and st["p4"] < 0.05, "station stats")
        st = station_stats([0.0, 0.25, 0.5, -0.25])
        chk(st["rbar4"] > 0.999 and st["rbar"] < 1e-9, "four-fold concentration with no mean")
    print("l2_quarter_cycle selftest: %d/%d PASS" % (n, n))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("headers")
    p.add_argument("obs_dirs", nargs="+")
    p.add_argument("--out", required=True)
    p = sub.add_parser("arcs")
    p.add_argument("obs_dirs", nargs="+")
    p.add_argument("--out", required=True)
    p.add_argument("--workers", type=int, default=max(1, min(16, (os.cpu_count() or 2) - 2)))
    p = sub.add_parser("grade")
    p.add_argument("--headers", required=True)
    p.add_argument("--arcs", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--golden", default="")
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "headers":
        return cmd_headers(args)
    if args.cmd == "arcs":
        return cmd_arcs(args)
    if args.cmd == "grade":
        return cmd_grade(args)
    if args.cmd == "selftest":
        return selftest()
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
