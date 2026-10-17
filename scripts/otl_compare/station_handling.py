#!/usr/bin/env python3
"""How each station of the field golden roster, its consumer and the 16-site census is
served by `otl_from_sif`.

Reads the RINEX header of every station (MARKER NAME and APPROX POSITION XYZ, the two
values `Geometry::loadData` hands to `StationInfoFile::find`), runs the production lookup
(`gnss_otl_diag lookup`, which applies the 4-character prefix guard) and prints, per
station, the SIF record found and whether it carries an ocean tide loading block.  A
station without one falls to BLQ (no file is configured on the goldens), then to the EOT20
grid (radial only; the goldens set no `data_download.nc_path`), then to zero.

    python -I station_handling.py --exe <gnss_otl_diag.exe> [--eot20 <EOT20_load.nc>]
"""
import argparse
import csv
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SIF_FILES = ["C:/GnssProcessor/data/static/sif/igs20_SIF.json",
             "C:/GnssProcessor/data/static/sif/ncn20_SIF.json"]


def header(path):
    marker = approx = None
    with open(path, "r", errors="replace") as f:
        for i, line in enumerate(f):
            if "MARKER NAME" in line:
                marker = line[:60].strip()
            if "APPROX POSITION XYZ" in line:
                approx = [float(x) for x in line[:60].split()]
            if "END OF HEADER" in line or i > 400:
                break
    return marker, approx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--eot20", default=None)
    ap.add_argument("--golden", default=os.path.join(ROOT, "config", "golden", "golden_field_refound.json"))
    ap.add_argument("--sites", default="C:/GnssProcessor/runs/_msc/site_list.json")
    ap.add_argument("--work", default="D:/GnssProcessorSupport/gates/otl")
    a = ap.parse_args()
    g = json.load(open(a.golden))
    sl = json.load(open(a.sites))["sites"]
    entries = []
    for s in g["network"]["stations"]:
        entries.append(("roster", s["name"], s["rinex_path"]))
    cs = g["network"]["closed_loop"]["station"]
    entries.append(("consumer", cs["name"], cs["rinex_path"]))
    for s in sl:
        entries.append(("census", s["name"], s["path"]))
    lst = os.path.join(a.work, "lookup_list.txt")
    with open(lst, "w") as f:
        meta = []
        for role, name, path in entries:
            mk, ap_ = header(path)
            f.write(f"{ap_[0]} {ap_[1]} {ap_[2]} {mk}\n")
            meta.append((role, name, mk))
    out = os.path.join(a.work, "lookup_result.csv")
    cmd = [a.exe, "lookup"] + sum([["--sif", p] for p in SIF_FILES], []) + ["--list", lst, "--out", out]
    if a.eot20:
        cmd += ["--eot20", a.eot20]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr[-500:])
    rows = list(csv.DictReader(open(out)))
    assert len(rows) == len(meta), (len(rows), len(meta))
    n_sif = 0
    print("role      station     RINEX marker                    SIF record   OTL  M2 up mm  EOT20 M2 up mm")
    for (role, name, mk), row in zip(meta, rows):
        has = row["has_otl"] == "1"
        n_sif += has
        print(f"{role:9s} {name:11s} {mk[:30]:30s}  {row['matched_id']:11s}  {'yes' if has else 'NO ':3s}  "
              f"{row['m2_up_mm']:>8s}  {row['eot20_m2_up_mm']:>8s}")
    print(f"\n{n_sif} of {len(meta)} stations resolve to a SIF record with an OTL block")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
