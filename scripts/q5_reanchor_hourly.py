#!/usr/bin/env python3
"""Re-anchor an already-run Q5 consumer-replay arm onto a different truth ECEF.

Phase 0 of ``docs/SPEC_day129_frame_guards.md`` graded consumer NRMG00NCL against
the RINEX header ``APPROX POSITION XYZ``.  That coordinate is stale by ~12 years
of plate motion (0.632 m; see ``scripts/q5_make_replay_arm.py``), so the absolute
levels in the Phase-0 / Q5 tables carried it as a constant bias.  This tool
replaces the anchor **without re-running anything**:

``logs/closed_loop_trace.csv`` records, per epoch, the position error as a local
ENU triple at the OLD truth.  The ENU rotation is orthonormal, so the estimated
ECEF position is recoverable exactly::

    pos = truth_old + R(llh_old) @ [east, north, up]

and the run can then be re-graded against any other truth.  The only loss is the
trace's own 0.1 mm print quantization, four orders of magnitude below the shift
being corrected -- and ``--selfcheck`` measures it rather than assuming it
(re-anchoring onto the OLD truth must reproduce the C++-written
``consumer_replay_hourly.csv``).

Only the anchor changes
-----------------------
The statistics are ports of ``Application::runConsumerReplay``
(``src/core/app/ApplicationConsumerReplay.cpp``): ``HourBand::quantile``'s linear
interpolation, same column order, same 4-decimal print.

Band MEMBERSHIP is taken from the run's own ``consumer_replay_hourly.csv`` ``n``
column rather than recomputed from the timestamps.  The driver's index is
``(int)((now_s - hour0_s) / 3600.0)`` on a ``tow`` that carries sub-microsecond
residue from the calendar->GPS conversion, so an epoch landing exactly on an hour
boundary falls to the low side about half the time: the DOY-129 arms band as
121/119/120 repeating, not 120/120/120.  That is a one-epoch-in-120 effect that
changes no conclusion, but recomputing the boundaries here would mix a banding
change into a truth change.  The bands are contiguous and time-ordered, so the
``n`` sequence determines membership exactly.  Without that file (or if the
counts do not sum to the trace length) the tool falls back to timestamp banding
and says so.

Output goes to ``logs/consumer_replay_hourly_itrf.csv`` -- a SIBLING of the C++
file, never an overwrite, so the run's own artifact stays intact.

Usage
-----
  # every arm, ITRF2020 truth from the frozen SSC, with the null-transform check
  python scripts/q5_reanchor_hourly.py runs/q5/arm* --selfcheck

  # one arm against an explicit coordinate
  python scripts/q5_reanchor_hourly.py runs/q5/armA --truth -5743538.2169 1380503.7952 -2397895.8332

  # compare the old and new hourly medians side by side
  python scripts/q5_reanchor_hourly.py runs/q5/armM_q1em4 --diff
"""
from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
_DEFAULT_SSC = _REPO / "data" / "static" / "sinex" / "ITRF2020-IGS-TRF.SSC"

# WGS84, matching Constants::Ellipsoid::WGS84 (the values Coordinates.cpp uses).
_A = 6378137.0
_F = 1.0 / 298.257223563
_E2 = _F * (2.0 - _F)


# --------------------------------------------------------------------------
# Geodesy -- verbatim ports of math/Coordinates.cpp
# --------------------------------------------------------------------------

def ecef_to_llh(ecef):
    """Port of ``Coordinates::ecefToLlh`` (Bowring seed + <=10 fixed-point steps).

    Ported rather than reused from ``gnssviz.geodesy`` so the null-transform
    self-check compares against the SAME iteration the C++ ran; a different but
    equally valid solver would leave a residue the check could not distinguish
    from a real defect.
    """
    x, y, z = (float(v) for v in ecef)
    p = math.hypot(x, y)
    lon = math.atan2(y, x)
    if p < 1e-12:
        lat = math.copysign(math.pi / 2.0, z if z != 0.0 else 1.0)
        n = _A / math.sqrt(1.0 - _E2 * math.sin(lat) ** 2)
        return lat, lon, abs(z) - n * (1.0 - _E2)
    lat = math.atan2(z, p * (1.0 - _E2))
    h = 0.0
    for _ in range(10):
        n = _A / math.sqrt(1.0 - _E2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        denom = p * (1.0 - _E2 * (n / (n + h)))
        if abs(denom) < 1e-15:
            break
        lat_next = math.atan2(z, denom)
        if abs(lat_next - lat) < 1e-13:
            lat = lat_next
            break
        lat = lat_next
    return lat, lon, h


def enu_to_ecef_matrix(lat, lon):
    """Port of ``Coordinates::enuToEcefMatrix`` -- columns are E, N, U in ECEF."""
    sl, cl = math.sin(lat), math.cos(lat)
    so, co = math.sin(lon), math.cos(lon)
    return ((-so, -sl * co, cl * co),
            (co, -sl * so, cl * so),
            (0.0, cl, sl))


def enu_to_ecef(enu, lat, lon):
    r = enu_to_ecef_matrix(lat, lon)
    return tuple(sum(r[i][k] * enu[k] for k in range(3)) for i in range(3))


def ecef_to_enu(vec, lat, lon):
    r = enu_to_ecef_matrix(lat, lon)
    return tuple(sum(r[k][i] * vec[k] for k in range(3)) for i in range(3))


# --------------------------------------------------------------------------
# Hour bands -- port of ApplicationConsumerReplay.cpp's HourBand
# --------------------------------------------------------------------------

def _quantile(sorted_vals, q):
    """Port of ``HourBand::quantile`` (linear interpolation on a sorted vector)."""
    if not sorted_vals:
        return 0.0
    pos = q * (len(sorted_vals) - 1)
    lo, hi = math.floor(pos), math.ceil(pos)
    frac = pos - lo
    return sorted_vals[int(lo)] * (1.0 - frac) + sorted_vals[int(hi)] * frac


_COLS = ["hour", "n", "err3d_mean_m", "err3d_median_m", "err3d_p95_m",
         "err3d_max_m", "up_mean_m", "up_min_m", "up_max_m"]


def band_rows(epochs):
    """Aggregate ``[(hour, err3d, up)]`` into the C++ hourly-CSV row order."""
    by_hour = {}
    for hour, err3d, up in epochs:
        e3, ups = by_hour.setdefault(hour, ([], []))
        e3.append(err3d)
        ups.append(up)
    out = []
    for hour in sorted(by_hour):
        e3, ups = by_hour[hour]
        s3 = sorted(e3)
        out.append({
            "hour": hour,
            "n": len(e3),
            "err3d_mean_m": sum(e3) / len(e3),
            "err3d_median_m": _quantile(s3, 0.5),
            "err3d_p95_m": _quantile(s3, 0.95),
            "err3d_max_m": max(e3),
            "up_mean_m": sum(ups) / len(ups),
            "up_min_m": min(ups),
            "up_max_m": max(ups),
        })
    return out


def hours_from_counts(cpp_rows, n_epochs):
    """Per-epoch hour index rebuilt from the run's own band counts.

    Returns ``None`` when the counts are absent or do not tile the trace, so the
    caller can fall back to timestamp banding instead of grading a mismatch.
    """
    if not cpp_rows:
        return None
    counts = [(int(r["hour"]), int(r["n"])) for r in cpp_rows]
    if sum(n for _, n in counts) != n_epochs:
        return None
    hours = []
    for hour, n in counts:
        hours.extend([hour] * n)
    return hours


def hours_from_time(rows):
    """Fallback: the driver's own ``(int)((now_s - hour0_s) / 3600.0)``."""
    t0 = rows[0][0]
    return [int((now_s - t0) / 3600.0) for now_s, _, _, _ in rows]


# --------------------------------------------------------------------------
# Arm I/O
# --------------------------------------------------------------------------

def read_arm_truth(armdir: Path):
    """The truth the run was ACTUALLY graded against, from its own config.

    Read from the arm's config rather than assumed, so an arm generated before
    the truth upgrade and one generated after both re-anchor correctly.
    """
    cfg = json.loads((armdir / "config.json").read_text())
    stn = cfg["network"]["closed_loop"]["station"]
    return [float(v) for v in stn["position_ecef"]], stn.get("name", "?")


def read_trace(armdir: Path):
    """``[(continuous_gps_s, east, north, up)]`` from ``closed_loop_trace.csv``."""
    path = armdir / "logs" / "closed_loop_trace.csv"
    rows = []
    with path.open(newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append((float(r["week"]) * 604800.0 + float(r["tow"]),
                         float(r["east_m"]), float(r["north_m"]), float(r["up_m"])))
    return rows


def read_cpp_hourly(armdir: Path):
    path = armdir / "logs" / "consumer_replay_hourly.csv"
    if not path.exists():
        return None
    with path.open(newline="") as fh:
        return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(fh)]


def reanchor(rows, hours, truth_old, truth_new):
    """Re-express each epoch's error against ``truth_new``."""
    lat_o, lon_o, _ = ecef_to_llh(truth_old)
    lat_n, lon_n, _ = ecef_to_llh(truth_new)
    out = []
    for (_, e, n, u), hour in zip(rows, hours):
        d_ecef = enu_to_ecef((e, n, u), lat_o, lon_o)
        # pos - truth_new = (truth_old - truth_new) + (pos - truth_old)
        d = tuple(truth_old[i] - truth_new[i] + d_ecef[i] for i in range(3))
        enu = ecef_to_enu(d, lat_n, lon_n)
        out.append((hour, math.sqrt(sum(c * c for c in d)), enu[2]))
    return out


# --------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("armdirs", nargs="+", help="Q5 arm directories (each holding config.json + logs/).")
    ap.add_argument("--truth", nargs=3, type=float, metavar=("X", "Y", "Z"),
                    help="Explicit new truth ECEF; default = the SSC value for the arm's station.")
    ap.add_argument("--ssc", default=str(_DEFAULT_SSC), help="ITRF/IGS SSC or SINEX file.")
    ap.add_argument("--doy", default="2024:129", help="YYYY:DOY for the velocity propagation.")
    ap.add_argument("--out", default="consumer_replay_hourly_itrf.csv",
                    help="Output CSV name inside each arm's logs/ (never the C++ file).")
    ap.add_argument("--selfcheck", action="store_true",
                    help="Null transform: re-anchor onto the OLD truth and diff against the "
                         "C++-written consumer_replay_hourly.csv.")
    ap.add_argument("--tol", type=float, default=1.0e-3,
                    help="Self-check ceiling in metres (default 1e-3 = the trace's own "
                         "0.1 mm ENU print quantization).")
    ap.add_argument("--diff", action="store_true",
                    help="Print the per-hour old-vs-new median/mean-up comparison.")
    args = ap.parse_args()

    truth_new = list(args.truth) if args.truth else None
    resolver = None
    if truth_new is None:
        if not Path(args.ssc).exists():
            print(f"ERROR: SSC not found: {args.ssc} (or pass --truth X Y Z).", file=sys.stderr)
            return 2
        # Load SinexTruth standalone, exactly as scripts/resolve_itrf_ecef.py does
        # (the gnssviz package __init__ pulls in pandas, which is not needed here).
        spec = importlib.util.spec_from_file_location(
            "snxtruth", _REPO / "pyviz" / "gnssviz" / "eval" / "truth.py")
        mod = importlib.util.module_from_spec(spec)
        sys.modules["snxtruth"] = mod
        spec.loader.exec_module(mod)
        yy, dd = (int(p) for p in args.doy.split(":"))
        ndays = 366 if (yy % 4 == 0 and (yy % 100 != 0 or yy % 400 == 0)) else 365
        resolver = (mod.SinexTruth(args.ssc), yy + (dd - 1) / ndays)

    rc = 0
    for d in args.armdirs:
        armdir = Path(d)
        if not (armdir / "logs" / "closed_loop_trace.csv").exists():
            print(f"{armdir}: no logs/closed_loop_trace.csv -- skipped", file=sys.stderr)
            rc = 1
            continue
        truth_old, station = read_arm_truth(armdir)

        if truth_new is None:
            tc = resolver[0].resolve(station, resolver[1])
            if tc is None:
                print(f"{armdir}: station {station} not in the SSC -- pass --truth.", file=sys.stderr)
                rc = 1
                continue
            new = [float(v) for v in tc.ecef]
            frame = tc.frame or "SSC"
        else:
            new, frame = truth_new, "explicit"

        rows = read_trace(armdir)
        cpp = read_cpp_hourly(armdir)
        hours = hours_from_counts(cpp, len(rows))
        banding = "run's own band counts"
        if hours is None:
            hours = hours_from_time(rows)
            banding = "RECOMPUTED from timestamps (no usable hourly CSV)"

        shift = [new[i] - truth_old[i] for i in range(3)]
        lat_o, lon_o, _ = ecef_to_llh(truth_old)
        se, sn, su = ecef_to_enu(tuple(shift), lat_o, lon_o)
        print(f"{armdir.name}: {station}  {len(rows)} epochs  truth {frame}  "
              f"shift E {se:+.4f} N {sn:+.4f} U {su:+.4f} m (3D {math.dist(new, truth_old):.4f})")
        print(f"   banding: {banding}")

        if args.selfcheck:
            if cpp is None:
                print("   selfcheck: no consumer_replay_hourly.csv to compare against", file=sys.stderr)
                rc = 1
            else:
                null = band_rows(reanchor(rows, hours, truth_old, truth_old))
                if len(null) != len(cpp):
                    print(f"   selfcheck FAIL: {len(null)} bands vs {len(cpp)} in the C++ file",
                          file=sys.stderr)
                    rc = 1
                else:
                    worst, worst_col = 0.0, ""
                    for got, want in zip(null, cpp):
                        for c in _COLS:
                            dv = abs(got[c] - want[c])
                            if dv > worst:
                                worst, worst_col = dv, c
                    ok = worst <= args.tol
                    print(f"   selfcheck {'PASS' if ok else 'FAIL'}: null transform reproduces "
                          f"the C++ bands to {worst:.2e} m (worst column {worst_col})")
                    if not ok:
                        rc = 1

        bands = band_rows(reanchor(rows, hours, truth_old, new))
        outp = armdir / "logs" / args.out
        with outp.open("w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=_COLS, lineterminator="\n")
            w.writeheader()
            for b in bands:
                w.writerow({c: (b[c] if c in ("hour", "n") else f"{b[c]:.4f}") for c in _COLS})
        print(f"   wrote {outp}")

        if args.diff and cpp:
            old_by_hour = {int(r["hour"]): r for r in cpp}
            print("   hour     n   med3d_old  med3d_new     d      up_old    up_new")
            for b in bands:
                o = old_by_hour.get(b["hour"])
                if o is None:
                    continue
                print(f"   {b['hour']:>4}  {b['n']:>4}  {o['err3d_median_m']:>9.3f}  "
                      f"{b['err3d_median_m']:>9.3f}  {b['err3d_median_m'] - o['err3d_median_m']:>+6.3f}  "
                      f"{o['up_mean_m']:>8.3f}  {b['up_mean_m']:>8.3f}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
