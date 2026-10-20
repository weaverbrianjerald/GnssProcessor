#!/usr/bin/env python3
"""Attribution table: GPS + Galileo (ge) against GPS only (g), per section, from nc.json files.

    python scripts/cost_attrib/nc_table.py <bed> <label> [<label> ...] [--out FILE] [--vs NUM DEN]

<bed> is f (field) or s (sim).  Runs are the arm directories
$RAPID_RUNS_ROOT/_rap_nc_<label>_<bed>_<variant>_<n> that carry an nc.json (nc_parse.py); the variants are
those of abba.sh (ge, g, on).  The sections tile the epoch-loop wall: the assembly wall, reduce, solve,
cov-inverse and cov-consumers of the run-end timing report, then its sub-blocks (net of the sections already
counted) and the unattributed rest.  Per section: mean ms per epoch for each variant (spread = max - min over
the runs), the ratio NUM / DEN, the excess (NUM - DEN) in points of the total ratio (excess / DEN total) and
its share of the total excess.  --vs NUM DEN names the two variants (default ge g; for the sparse normals
check ge on).  A variant may carry the label it comes from, variant@label (for two executables run under two
labels: --vs ge@new ge@old); without it the variant is read from every label given.
"""
import glob
import json
import os
import statistics as st
import sys

RUNS = os.environ.get("RAPID_RUNS_ROOT", "D:/GnssProcessorSupport/runs_nom")
D0 = ["assembly-wall", "reduce", "solve", "cov-inverse", "cov-consumers"]


def load(bed, labels, va="ge", vb="g"):
    runs = {"ge": [], "g": []}
    for key, spec in (("ge", va), ("g", vb)):
        var, _, only = spec.partition("@")
        for lab in ([only] if only else labels):
            for d in sorted(glob.glob(f"{RUNS}/_rap_nc_{lab}_{bed}_{var}_*")):
                p = d + "/nc.json"
                if os.path.exists(p):
                    j = json.load(open(p))
                    if "loop_ms" in j:
                        runs[key].append(j)
    return runs


def sections(j):
    ep = j["epochs"]
    out = {}
    for k in D0:
        out[k] = j["d0"][k]["ms"] / ep
    for k, v in j["d0b"].items():
        if k != "TOTAL":
            out[k] = v["net"] / ep
    out["_loop"] = j["loop_ms"] / ep
    return out


def main():
    a = sys.argv[1:]
    out = None
    va, vb = "ge", "g"
    if "--out" in a:
        i = a.index("--out")
        out = a[i + 1]
        a = a[:i] + a[i + 2:]
    if "--vs" in a:           # --vs <numerator variant> <denominator variant>, e.g. --vs ge on
        i = a.index("--vs")
        va, vb = a[i + 1], a[i + 2]
        a = a[:i] + a[i + 3:]
    bed, labels = a[0], a[1:]
    runs = load(bed, labels, va, vb)
    if not runs["ge"] or not runs["g"]:
        sys.exit("no runs")
    S = {v: [sections(j) for j in runs[v]] for v in runs}
    keys = sorted({k for v in S for s in S[v] for k in s if k != "_loop"},
                  key=lambda k: -st.mean(s.get(k, 0.0) for s in S["ge"]))
    L = []
    w = L.append
    for v in ("g", "ge"):
        rs = runs[v]
        nm = vb if v == "g" else va
        loops = [r["ms_epoch"] for r in rs]
        cpu = [float(r["timing"]["cpu_user_s"]) + float(r["timing"]["cpu_kernel_s"]) for r in rs if "timing" in r]
        oth = [float(r["timing"]["other_procs_mean"]) for r in rs if "timing" in r]
        w(f"{nm:>3}: {len(rs)} runs, loop ms/epoch " + " ".join(f"{x:.0f}" for x in loops)
          + f" (mean {st.mean(loops):.0f}, spread {max(loops)-min(loops):.0f})"
          + (f"; process CPU s " + " ".join(f"{x:.0f}" for x in cpu) if cpu else "")
          + (f"; other processes' CPU (logical procs) " + " ".join(f"{x:.1f}" for x in oth) if oth else ""))
    gl = st.mean(s["_loop"] for s in S["g"]); el = st.mean(s["_loop"] for s in S["ge"])
    w(f"wall ratio (numerator/denominator) = {el/gl:.2f}")
    gc = [float(r["timing"]["cpu_user_s"]) + float(r["timing"]["cpu_kernel_s"]) for r in runs["g"] if "timing" in r]
    ec = [float(r["timing"]["cpu_user_s"]) + float(r["timing"]["cpu_kernel_s"]) for r in runs["ge"] if "timing" in r]
    if gc and ec:
        w(f"CPU ratio (numerator/denominator) = {st.mean(ec)/st.mean(gc):.2f}")
    w("")
    cw = max(12, len(va) + 8, len(vb) + 8)               # column width of the two variants' means
    w(f"{'section':<15}{vb + ' ms/ep':>{cw}}{'(spread)':>9}{va + ' ms/ep':>{cw}}{'(spread)':>9}{'ratio':>7}{'excess':>9}{'pts':>7}{'share':>8}")
    tot_ex = el - gl
    rows = []
    for k in keys:
        gv = [s.get(k, 0.0) for s in S["g"]]; ev = [s.get(k, 0.0) for s in S["ge"]]
        gm, em = st.mean(gv), st.mean(ev)
        rows.append((k, gm, max(gv) - min(gv), em, max(ev) - min(ev), em / gm if gm > 0.05 else float("nan"),
                     em - gm, (em - gm) / gl, (em - gm) / tot_ex))
    for r in sorted(rows, key=lambda r: -r[6]):
        w(f"{r[0]:<15}{r[1]:>{cw}.1f}{r[2]:>9.1f}{r[3]:>{cw}.1f}{r[4]:>9.1f}{r[5]:>7.2f}{r[6]:>9.1f}{r[7]:>7.2f}{100*r[8]:>7.1f}%")
    w(f"{'TOTAL':<15}{gl:>{cw}.1f}{'':>9}{el:>{cw}.1f}{'':>9}{el/gl:>7.2f}{tot_ex:>9.1f}{tot_ex/gl:>7.2f}{100.0:>7.1f}%")
    txt = "\n".join(L)
    print(txt)
    if out:
        open(out, "w").write(txt + "\n")


if __name__ == "__main__":
    main()
