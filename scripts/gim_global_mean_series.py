"""Grade a network run's SH a00 against the GIM's own global mean VTEC, epoch by epoch.

Rationale: every level verdict on the EXT-CB ladder so far has been quoted against
a single number, "GIM 32.24 TECU", the global mean of the 00:00 map. That is fine
for an ignition arm covering minutes. It is not fine for a 12 h arm: the global
mean VTEC itself moves through the day (about 2x between night and the afternoon
peak), so a run whose a00 rises may be tracking the ionosphere rather than
drifting. Comparing a00(t) with GIM_mean(t) separates those two readings.

What it computes
  * GIM_mean(t): the area-weighted (cos-latitude) mean VTEC of each IONEX map in the
    day's file, linearly interpolated in time. This is the same functional a00 is
    (a sun-fixed SH expansion's a00 is the global mean, since the basis is
    orthonormal under (1/4pi) integral dOmega, so Ybar00 = 1).
  * per-run a00(t) from logs/network_vtec_sh_health.csv, and the residual
    a00(t) - GIM_mean(t) with hourly statistics.

Usage
  python scripts/gim_global_mean_series.py <run_dir> [<run_dir> ...]
      [--ionex PATH] [--hours N]

Caveats worth keeping in mind when quoting the output
  * IONEX maps are themselves a product with a few-TECU error, and their global mean
    leans on the (data-free) ocean/southern-hemisphere interpolation. Treat the
    residual as a level comparison, not as truth.
  * a00 is a global mean while the network sees one hemisphere-ish footprint; the
    unsupported region is Kaula-extrapolated. A residual can therefore be real
    without any station being wrong (see docs/SPEC_level_information_budget.md §29).
"""
import argparse
import csv
import glob
import math
import os
import statistics


def load_ionex(path):
    """Return [(sod, {lat: (lon1, dlon, [vtec...])}), ...] for every TEC map."""
    maps = []
    with open(path, errors="replace") as f:
        lines = f.readlines()
    i, exp10 = 0, -1
    while i < len(lines):
        lab = lines[i][60:].strip()
        if lab == "EXPONENT":
            exp10 = int(lines[i][:60].split()[0])
        if lab == "START OF TEC MAP":
            grid, sod = {}, None
            j = i + 1
            while lines[j][60:].strip() != "END OF TEC MAP":
                lab_j = lines[j][60:].strip()
                if lab_j == "EPOCH OF CURRENT MAP":
                    p = lines[j][:60].split()
                    sod = int(p[3]) * 3600 + int(p[4]) * 60 + int(float(p[5]))
                elif lab_j == "LAT/LON1/LON2/DLON/H":
                    s = lines[j]
                    lat = float(s[2:8])
                    lon1, lon2, dlon = float(s[8:14]), float(s[14:20]), float(s[20:26])
                    nlon = int(round((lon2 - lon1) / dlon)) + 1
                    vals, k = [], j + 1
                    while len(vals) < nlon:
                        row = lines[k].rstrip()
                        vals += [int(row[x:x + 5]) for x in range(0, min(80, len(row)), 5)]
                        k += 1
                    grid[lat] = (lon1, dlon, [v * 10.0 ** exp10 for v in vals[:nlon]])
                    j = k
                    continue
                j += 1
            # A map whose EPOCH record cannot be read is dropped, not defaulted to
            # 00:00: a mis-timed map silently distorts the whole reference curve
            # (it sorts to the front and the first interpolation segment inherits the
            # wrong value).
            if grid and sod is not None:
                # The final map of a daily IONEX is the next day's 00:00 (COD writes
                # it that way), so its seconds-of-day wraps to 0. Unwrap in file
                # order, or it sorts to the front and corrupts the first
                # interpolation segment with the day-end level.
                if maps and sod <= maps[-1][0]:
                    sod += 86400
                maps.append((sod, grid))
            i = j
        i += 1
    return maps


def global_mean(grid):
    """Area-weighted (cos-lat) mean VTEC of one IONEX map, in TECU."""
    num = den = 0.0
    for lat, (_lon1, _dlon, vals) in grid.items():
        w = math.cos(math.radians(lat))
        if w <= 0.0:
            continue                       # the poles carry no area
        # The +180 and -180 columns are the same meridian: drop the duplicate.
        v = vals[:-1] if len(vals) > 1 else vals
        num += w * (sum(v) / len(v))
        den += w
    return num / den if den else float("nan")


def interp(series, sod):
    """Linear interpolation of (sod, value) pairs, clamped at both ends."""
    if not series:
        return float("nan")
    if sod <= series[0][0]:
        return series[0][1]
    if sod >= series[-1][0]:
        return series[-1][1]
    for (s0, v0), (s1, v1) in zip(series, series[1:]):
        if s0 <= sod <= s1:
            f = 0.0 if s1 == s0 else (sod - s0) / (s1 - s0)
            return v0 + f * (v1 - v0)
    return series[-1][1]


def run_a00(run_dir):
    p = os.path.join(run_dir, "logs", "network_vtec_sh_health.csv")
    out = []
    if not os.path.exists(p):
        return out
    with open(p) as f:
        for r in csv.DictReader(f):
            tow, a00 = r.get("tow"), r.get("a00_tecu")
            if not tow or not a00:
                continue               # torn last line of a live run
            try:
                out.append((float(tow), float(a00)))
            except ValueError:
                continue
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--ionex", default=None,
                    help="IONEX file (default: the day's GIM under data/2024-129)")
    ap.add_argument("--hours", type=float, default=1.0,
                    help="bucket size for the hourly table")
    args = ap.parse_args()

    ionex = args.ionex or glob.glob(
        r"C:\GnssProcessor\data\2024-129\products\*GIM.INX")[0]
    maps = load_ionex(ionex)
    series = sorted((sod, global_mean(g)) for sod, g in maps)
    print(f"GIM: {os.path.basename(ionex)} — {len(series)} maps, "
          f"global mean {min(v for _, v in series):.2f}..{max(v for _, v in series):.2f} TECU")
    print("  " + "  ".join(f"{s//3600:02d}h:{v:.1f}" for s, v in series))
    print()

    tow0 = 259200.0            # GPS TOW at 00:00 on DOY-129 (week 2313)
    for run in args.runs:
        a = run_a00(run)
        if not a:
            print(f"{os.path.basename(run):28} no data")
            continue
        rows = [(tow, v, interp(series, (tow - tow0) % 86400.0)) for tow, v in a]
        d = [v - g for _, v, g in rows]
        print(f"{os.path.basename(run):28} n={len(rows):5}  "
              f"a00-GIM: mean {sum(d)/len(d):+7.2f}  median "
              f"{statistics.median(d):+7.2f}  sd {statistics.pstdev(d):5.2f}  "
              f"[{min(d):+.1f},{max(d):+.1f}]")
        step = int(args.hours * 120)
        for h in range(0, len(rows), step):
            w = rows[h:h + step]
            if len(w) < 10:
                continue
            dv = [v - g for _, v, g in w]
            print(f"     ~{h//step:2}h  a00 {sum(v for _, v, _ in w)/len(w):6.2f}"
                  f"   GIM {sum(g for _, _, g in w)/len(w):6.2f}"
                  f"   diff {sum(dv)/len(dv):+7.2f} +-{statistics.pstdev(dv):4.2f}")


if __name__ == "__main__":
    main()
