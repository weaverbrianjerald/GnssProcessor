#!/usr/bin/env python3
"""Two measurement rounds of the GPS + Galileo against GPS-only attribution, side by side.

    python scripts/cost_attrib/compare_rounds.py <bed> <label A> <label B> [--top N]

Prints, for the sections that carry the largest share of the excess, each round's ms per epoch for the two
variants, the ratio, and the share of the total excess, and the difference of the shares in percentage points.
The tables of each round are those of nc_table.py (same arm layout, same section tiling).
"""
import os
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nc_table  # noqa: E402


def round_rows(bed, label):
    runs = nc_table.load(bed, [label])
    sec = {v: [nc_table.sections(j) for j in runs[v]] for v in runs}
    keys = {k for v in sec for s in sec[v] for k in s if k != "_loop"}
    gl = st.mean(s["_loop"] for s in sec["g"])
    el = st.mean(s["_loop"] for s in sec["ge"])
    out = {}
    for k in keys:
        gm = st.mean(s.get(k, 0.0) for s in sec["g"])
        em = st.mean(s.get(k, 0.0) for s in sec["ge"])
        out[k] = (gm, em, (em / gm) if gm > 0.05 else float("nan"), (em - gm) / (el - gl))
    return out, gl, el


def main():
    bed, la, lb = sys.argv[1], sys.argv[2], sys.argv[3]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 8
    ra, gla, ela = round_rows(bed, la)
    rb, glb, elb = round_rows(bed, lb)
    print(f"loop ms/epoch  G {gla:.0f} / {glb:.0f}   GE {ela:.0f} / {elb:.0f}   ratio {ela / gla:.2f} / {elb / glb:.2f}")
    print(f"{'section':<15}{'G ms':>8}{'GE ms':>8}{'ratio':>7}{'share':>8} |{'G ms':>8}{'GE ms':>8}{'ratio':>7}{'share':>8} | {'d share':>8}")
    for k in sorted(ra, key=lambda k: -ra[k][3])[:top]:
        a, b = ra[k], rb.get(k, (0, 0, float("nan"), 0))
        print(f"{k:<15}{a[0]:>8.1f}{a[1]:>8.1f}{a[2]:>7.2f}{100 * a[3]:>7.1f}% |{b[0]:>8.1f}{b[1]:>8.1f}{b[2]:>7.2f}{100 * b[3]:>7.1f}% | {100 * (b[3] - a[3]):>+7.1f}")


if __name__ == "__main__":
    main()
