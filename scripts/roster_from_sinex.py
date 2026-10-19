#!/usr/bin/env python3
"""Roster coordinates from an IGS weekly SINEX.

Reads a SINEX (plain or gzip, detected from the first two bytes) and writes, for every
station of a config's network roster that the file holds, the marker coordinate of the
solution that covers the epoch.  A station the file lacks keeps the config coordinate
and is named in the report.  Nothing is propagated: a weekly SINEX has one position per
station at the middle of the week.  A file that carries velocity rows is refused unless
--epoch gives the epoch to propagate to.

The coordinates are marker (monument) coordinates, like the config's: the engine reduces
a roster coordinate to the antenna reference point itself.  No eccentricity is applied.

  python -I scripts/roster_from_sinex.py --sinex <weekly.SNX.gz> [--config <golden>.json]
        [--only NAME,NAME,...] [--min-offset-mm X]
        [--out roster.json] [--report report.txt]
        [--site-list scripts/clock_level/site_list.json --site-list-out site_list_sinex.json]
  python -I scripts/roster_from_sinex.py --selftest

The roster file written by --out has `positions`, a name to [x, y, z] map holding only the
stations to replace (all that the file holds, or those picked by --only or --min-offset-mm),
and `stations`, one record per roster station with its source, the solution number, the
formal sigma, the offset from the config and the flags.  `rapid_arm.py make --roster` reads
`positions`.  A station is matched on its four-character code and refused when the SINEX
position is more than 1 km from the config's coordinate (a reused code).

The consumer of the config (`network.closed_loop.station`) and the census sites of a site
list are handled the same way: --site-list-out writes the `sites` array of the input with
each site's `ecef` replaced, a top-level `consumer` entry and the source, so a grader that
reads the `sites` array (scripts/clock_level/msc_sites.py) can read it in place of the original.
"""
import argparse
import calendar
import datetime
import gzip
import hashlib
import json
import math
import os
import statistics as st
import sys
import tempfile

WGS84_A, WGS84_F = 6378137.0, 1.0 / 298.257223563
REUSE_LIMIT_M = 1000.0        # a SINEX position farther than this from the config's is a reused code
SHORT_SPAN_DAYS = 6.0         # a station observed for less of the week is flagged


# ----------------------------------------------------------------------------- epochs
def sinex_epoch_to_year(tok):
    """'YY:DDD:SSSSS' to a decimal year (day of year counted from zero, so DOY 129 noon is 128.5)."""
    yy, ddd, sss = (int(x) for x in tok.split(":"))
    year = 1900 + yy if yy >= 70 else 2000 + yy
    ndays = 366 if calendar.isleap(year) else 365
    return year + (ddd - 1 + sss / 86400.0) / ndays


def sinex_epoch_to_datetime(tok):
    yy, ddd, sss = (int(x) for x in tok.split(":"))
    year = 1900 + yy if yy >= 70 else 2000 + yy
    return datetime.datetime(year, 1, 1) + datetime.timedelta(days=ddd - 1, seconds=sss)


def is_epoch(tok):
    p = tok.split(":")
    return len(p) == 3 and all(x.isdigit() for x in p)


# ----------------------------------------------------------------------------- parser
class Sinex:
    """The blocks of a SINEX that a roster needs: the header line, SOLUTION/EPOCHS and the
    STA* / VEL* rows of SOLUTION/ESTIMATE, keyed by (code, point, solution number)."""

    def __init__(self, path):
        self.path = path
        raw = open(path, "rb").read()
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.size = len(raw)
        text = (gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw).decode("latin1")
        self.header = ""
        self.windows = {}         # (code, pt, soln) -> (start_tok, end_tok)
        self.rows = {}            # (code, pt, soln) -> {"pos": [3], "std": [3], "vel": [3], "epoch": tok}
        self.n_rows_skipped = 0
        block = None
        for line in text.splitlines():
            if line.startswith("%=SNX"):
                self.header = line.strip()
            if line.startswith("+"):
                block = line[1:].split()[0].upper() if line[1:].strip() else None
                continue
            if line.startswith("-"):
                block = None
                continue
            if not line.strip() or line.startswith("*"):
                continue
            if block == "SOLUTION/EPOCHS":
                self._epoch_row(line)
            elif block == "SOLUTION/ESTIMATE":
                self._estimate_row(line)

    def _epoch_row(self, line):
        t = line.split()                    # CODE PT SOLN T START END MEAN
        if len(t) < 6 or not t[2].isdigit() or not is_epoch(t[4]) or not is_epoch(t[5]):
            self.n_rows_skipped += 1
            return
        self.windows[(t[0].upper(), t[1].upper(), int(t[2]))] = (t[4], t[5])

    def _estimate_row(self, line):
        t = line.split()                    # INDEX TYPE CODE PT SOLN EPOCH UNIT S VALUE STD
        if len(t) < 10 or t[1][:3] not in ("STA", "VEL"):
            return
        axis = "XYZ".find(t[1][3:4])
        if len(t[1]) != 4 or axis < 0 or not t[4].isdigit() or not is_epoch(t[5]):
            self.n_rows_skipped += 1
            return
        try:
            val, std = float(t[-2].replace("D", "E")), float(t[-1].replace("D", "E"))
        except ValueError:
            self.n_rows_skipped += 1
            return
        rec = self.rows.setdefault((t[2].upper(), t[3].upper(), int(t[4])),
                                   {"pos": [None] * 3, "std": [None] * 3, "vel": [None] * 3, "epoch": t[5]})
        if t[1].startswith("STA"):
            rec["pos"][axis], rec["std"][axis] = val, std
        else:
            rec["vel"][axis] = val

    def has_velocity(self):
        return any(None not in r["vel"] for r in self.rows.values())

    def reference_epochs(self):
        return sorted({r["epoch"] for r in self.rows.values()})

    def pick(self, code, year):
        """The solution of `code` that covers `year`: ((code, pt, soln), record, window, note) or None.
        Point A is preferred; a window that contains the epoch beats the nearest one (note says so)."""
        keys = [k for k in self.rows if k[0] == code and None not in self.rows[k]["pos"]]
        if not keys:
            return None
        pts = sorted({k[1] for k in keys})
        pt = "A" if "A" in pts else pts[0]
        keys = sorted(k for k in keys if k[1] == pt)

        def dist(k):
            w = self.windows.get(k)
            if w is None:
                return 0.0
            a, b = sinex_epoch_to_year(w[0]), sinex_epoch_to_year(w[1])
            return 0.0 if a <= year <= b else min(abs(year - a), abs(year - b))

        best = min(keys, key=lambda k: (dist(k), k[2]))
        note = "" if dist(best) == 0.0 else "window does not contain the epoch"
        if len(keys) > 1:
            note = (note + "; " if note else "") + f"soln {best[2]} of {len(keys)}"
        return best, self.rows[best], self.windows.get(best), note


# ----------------------------------------------------------------------------- geodesy
def ecef_to_enu(d, ref):
    """The vector d (ECEF, metres) in the local east, north, up frame at ref."""
    x, y, z = ref
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    e2 = WGS84_F * (2 - WGS84_F)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(6):
        s = math.sin(lat)
        n = WGS84_A / math.sqrt(1 - e2 * s * s)
        lat = math.atan2(z + e2 * n * s, p)
    sl, cl, so, co = math.sin(lat), math.cos(lat), math.sin(lon), math.cos(lon)
    return (-so * d[0] + co * d[1],
            -sl * co * d[0] - sl * so * d[1] + cl * d[2],
            cl * co * d[0] + cl * so * d[1] + sl * d[2])


# ----------------------------------------------------------------------------- resolve
def resolve(snx, name, held, year, target_year):
    """One station record: the SINEX marker position for `name` against the held position.
    `target_year` is None for no propagation, else the decimal year to propagate to."""
    code = name[:4].upper()
    rec = {"name": name, "source": "config", "position_ecef": [round(v, 4) for v in held], "flags": []}
    got = snx.pick(code, year)
    if got is None:
        rec["flags"].append("absent from the file")
        return rec
    key, row, win, note = got
    pos = list(row["pos"])
    if target_year is not None and None not in row["vel"]:
        dt = target_year - sinex_epoch_to_year(row["epoch"])
        pos = [p + v * dt for p, v in zip(pos, row["vel"])]
    d = [p - h for p, h in zip(pos, held)]
    if math.sqrt(sum(c * c for c in d)) > REUSE_LIMIT_M:
        rec["flags"].append(f"refused: {math.sqrt(sum(c * c for c in d)):.1f} m from the config (reused code)")
        return rec
    e, n, u = ecef_to_enu(d, held)
    rec.update({"source": "sinex", "position_ecef": [round(v, 4) for v in pos], "pt": key[1], "soln": key[2],
                "sigma_mm": round(1000.0 * math.sqrt(sum(s * s for s in row["std"])), 2),
                "offset_enu_mm": [round(1000.0 * e, 2), round(1000.0 * n, 2), round(1000.0 * u, 2)],
                "offset_3d_mm": round(1000.0 * math.sqrt(sum(c * c for c in d)), 2)})
    if win is not None:
        span = (sinex_epoch_to_datetime(win[1]) - sinex_epoch_to_datetime(win[0])).total_seconds() / 86400.0
        rec["span_days"] = round(span, 2)
        if span < SHORT_SPAN_DAYS:
            rec["flags"].append(f"short data span {span:.1f} d")
    if note:
        rec["flags"].append(note)
    return rec


def load_config_roster(path):
    cfg = json.load(open(path, encoding="utf-8"))
    net = cfg["network"]
    stations = [(s["name"], [float(v) for v in s["position_ecef"]]) for s in net["stations"]]
    cons = net.get("closed_loop", {}).get("station")
    consumer = (cons["name"], [float(v) for v in cons["position_ecef"]]) if cons and "position_ecef" in cons else None
    return stations, consumer


# ----------------------------------------------------------------------------- report
def stats(vals):
    v = sorted(vals)
    if not v:
        return "n 0"
    k = 0.9 * (len(v) - 1)
    p90 = v[int(k)] + (k - int(k)) * (v[min(int(k) + 1, len(v) - 1)] - v[int(k)])
    return f"n {len(v)}  median {st.median(v):.1f}  p90 {p90:.1f}  max {v[-1]:.1f}"


def render_report(snx, year, recs, replaced, consumer_rec, sites_recs):
    L = [f"Roster from SINEX: {os.path.basename(snx.path)}  ({snx.size} bytes, sha256 {snx.sha256})",
         f"Header: {snx.header}",
         f"Reference epoch of the coordinate rows: {', '.join(snx.reference_epochs())}  (decimal year {year:.5f})",
         "Offsets are SINEX minus config, millimetres, local east, north, up at the config position; 3D is their length.",
         "Marker coordinates on both sides; no eccentricity applied; no propagation.", ""]
    L.append(f"{'station':10s} {'source':7s} {'replaced':8s} {'soln':>4s} {'sig':>5s} {'E':>8s} {'N':>8s} {'U':>8s} {'3D':>8s}  flags")

    def row(r, flag_replaced):
        if r["source"] == "sinex":
            e, n, u = r["offset_enu_mm"]
            return (f"{r['name']:10s} {r['source']:7s} {flag_replaced:8s} {r['soln']:4d} {r['sigma_mm']:5.1f} "
                    f"{e:8.1f} {n:8.1f} {u:8.1f} {r['offset_3d_mm']:8.1f}  {'; '.join(r['flags'])}")
        return f"{r['name']:10s} {r['source']:7s} {flag_replaced:8s} {'':>4s} {'':>5s} {'':>8s} {'':>8s} {'':>8s} {'':>8s}  {'; '.join(r['flags'])}"

    for r in recs:
        L.append(row(r, "yes" if r["name"] in replaced else "no"))
    present = [r for r in recs if r["source"] == "sinex"]
    absent = [r["name"] for r in recs if r["source"] != "sinex"]
    L += ["", f"Roster stations: {len(recs)}; in the file {len(present)}; replaced {len(replaced)}; "
          f"kept at the config coordinate {len(recs) - len(replaced)}; absent or refused {len(absent)}"
          + (f" ({', '.join(absent)})" if absent else ""),
          "3D offset, all present: " + stats([r["offset_3d_mm"] for r in present]),
          "3D offset, replaced:    " + stats([r["offset_3d_mm"] for r in present if r["name"] in replaced])]
    if consumer_rec:
        L += ["", "Consumer " + row(consumer_rec, "truth")]
    if sites_recs:
        L += ["", "Site list (census):"] + ["  " + row(r, "truth") for r in sites_recs]
        pr = [r for r in sites_recs if r["source"] == "sinex"]
        L.append(f"  3D offset, {len(pr)} of {len(sites_recs)} sites present: " + stats([r["offset_3d_mm"] for r in pr]))
    return "\n".join(L) + "\n"


# ----------------------------------------------------------------------------- main
def config_label(path):
    """The config path as recorded in the outputs: relative to the repository when it lies inside it."""
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    full = os.path.abspath(path)
    inside = os.path.commonpath([repo, full]) == repo if os.path.splitdrive(repo)[0] == os.path.splitdrive(full)[0] else False
    return (os.path.relpath(full, repo) if inside else full).replace("\\", "/")


def run(a):
    snx = Sinex(a.sinex)
    eps = snx.reference_epochs()
    if len(eps) != 1:
        sys.exit(f"the coordinate rows carry {len(eps)} reference epochs ({eps[:4]}...); one is expected for a weekly file")
    year = sinex_epoch_to_year(eps[0])
    target = None
    if snx.has_velocity():
        if a.epoch is None:
            sys.exit("the file carries velocity rows: give --epoch (decimal year) to propagate to")
        target = a.epoch
    elif a.epoch is not None:
        sys.exit("--epoch given but the file has no velocity rows; nothing to propagate")
    stations, consumer = load_config_roster(a.config)
    recs = [resolve(snx, n, p, year, target) for n, p in stations]
    present = {r["name"] for r in recs if r["source"] == "sinex"}
    replaced = set(present)
    if a.only:
        names = [x.strip() for x in a.only.split(",") if x.strip()]
        unknown = [x for x in names if x not in {n for n, _ in stations}]
        if unknown:
            sys.exit("--only names not in the config roster: " + ", ".join(unknown))
        missing = [x for x in names if x not in present]
        if missing:
            sys.exit("--only names the file does not hold: " + ", ".join(missing))
        replaced = set(names)
    if a.min_offset_mm is not None:
        replaced &= {r["name"] for r in recs if r["source"] == "sinex" and r["offset_3d_mm"] >= a.min_offset_mm}
    consumer_rec = resolve(snx, consumer[0], consumer[1], year, target) if consumer else None
    sites_recs, site_doc = [], None
    if a.site_list:
        site_doc = json.load(open(a.site_list, encoding="utf-8"))
        sites_recs = [resolve(snx, s["name"], [float(v) for v in s["ecef"]], year, target) for s in site_doc["sites"]]

    report = render_report(snx, year, recs, replaced, consumer_rec, sites_recs)
    print(report, end="")
    if a.report:
        open(a.report, "w", encoding="utf-8", newline="\n").write(report)
    src = {"file": os.path.basename(snx.path), "sha256": snx.sha256, "header": snx.header,
           "reference_epoch": eps[0], "decimal_year": round(year, 5), "propagated_to": target,
           "config": config_label(a.config)}
    if a.out:
        out = {"source": src, "positions": {r["name"]: r["position_ecef"] for r in recs if r["name"] in replaced},
               "stations": recs, "consumer": consumer_rec}
        json.dump(out, open(a.out, "w", encoding="utf-8", newline="\n"), indent=1)
        print(f"wrote {a.out}: {len(out['positions'])} positions")
    if a.site_list_out:
        if site_doc is None:
            sys.exit("--site-list-out needs --site-list")
        doc = {"_source": {**src, "site_list": os.path.basename(a.site_list),
                           "note": "each site's ecef is the SINEX marker coordinate; a site the file lacks keeps its "
                                   "original ecef and is listed in `kept`"},
               "kept": [r["name"] for r in sites_recs if r["source"] != "sinex"],
               "sites": json.loads(json.dumps(site_doc["sites"]))}
        for s, r in zip(doc["sites"], sites_recs):
            s["ecef"] = r["position_ecef"]
        if consumer_rec:
            doc["consumer"] = {"name": consumer_rec["name"], "ecef": consumer_rec["position_ecef"],
                               "source": consumer_rec["source"]}
        json.dump(doc, open(a.site_list_out, "w", encoding="utf-8", newline="\n"), indent=1)
        print(f"wrote {a.site_list_out}: {len(doc['sites'])} sites, kept {len(doc['kept'])}")


def selftest():
    def row(i, typ, code, soln, val, std="1.0e-03"):
        return f"{i:6d} {typ}   {code}  A {soln:4d} 24:129:43200 m    2 {val:21.14e} {std}"
    body = ["%=SNX 2.02 IGN 24:144:26732 IGN 24:126:00000 24:133:00000 C  9 2 S E", "+SOLUTION/EPOCHS",
            " AAAA  A    1 P 24:126:00000 24:132:86370 24:129:43200",
            " BBBB  A    1 P 24:126:00000 24:131:00000 24:129:43200",
            " BBBB  A    2 P 24:131:00000 24:133:00000 24:129:43200", "-SOLUTION/EPOCHS", "+SOLUTION/ESTIMATE"]
    truth = {"AAAA": (4075579.1224, 931853.4440, 4801569.2330), "BBBB": (-2493304.9000, -4655214.7000, 3565497.6000)}
    i = 1
    for code, p in truth.items():
        for ax, v in zip("XYZ", p):
            body.append(row(i, f"STA{ax}", code, 1, v))
            i += 1
    body.append("-SOLUTION/ESTIMATE")
    with tempfile.TemporaryDirectory() as td:
        for name, wrap in (("a.snx", open), ("a.snx.gz", gzip.open)):
            path = os.path.join(td, name)
            with wrap(path, "wb") as fh:
                fh.write(("\n".join(body) + "\n").encode("latin1"))
            s = Sinex(path)
            assert s.reference_epochs() == ["24:129:43200"] and not s.has_velocity(), name
            held = [truth["AAAA"][0] + 0.010, truth["AAAA"][1], truth["AAAA"][2]]
            r = resolve(s, "AAAA00DEU", held, sinex_epoch_to_year("24:129:43200"), None)
            assert r["source"] == "sinex" and abs(r["offset_3d_mm"] - 10.0) < 0.2 and not r["flags"], r
            far = resolve(s, "AAAA00DEU", [held[0] + 5000.0, held[1], held[2]], 2024.35, None)
            assert far["source"] == "config" and "refused" in far["flags"][0], far
            assert resolve(s, "CCCC00XXX", held, 2024.35, None)["flags"] == ["absent from the file"]
            b = resolve(s, "BBBB00XXX", list(truth["BBBB"]), 2024.35109, None)
            assert b["soln"] == 1 and any("short data span" in f for f in b["flags"]), b
    assert abs(sinex_epoch_to_year("24:129:43200") - 2024.35109) < 1e-5
    e, n, u = ecef_to_enu((0.0, 0.0, 1.0), (6378137.0, 0.0, 0.0))
    assert abs(e) < 1e-9 and abs(n - 1.0) < 1e-9 and abs(u) < 1e-9
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sinex", help="weekly SINEX, .SNX or .SNX.gz")
    ap.add_argument("--config", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config", "golden",
                                                     "golden_field_refound.json"),
                    help="the config whose roster and consumer are matched (default: the field golden)")
    ap.add_argument("--only", help="replace only these stations (comma list of 9-character names)")
    ap.add_argument("--min-offset-mm", type=float, help="replace only stations whose 3D offset is at least this")
    ap.add_argument("--epoch", type=float, help="decimal year to propagate to (only for a file with velocity rows)")
    ap.add_argument("--out", help="roster JSON to write")
    ap.add_argument("--report", help="text report to write")
    ap.add_argument("--site-list", help="a site list (the census sites) to carry through")
    ap.add_argument("--site-list-out", help="the site list with SINEX coordinates")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.sinex:
        ap.error("--sinex is required")
    run(a)


if __name__ == "__main__":
    main()
