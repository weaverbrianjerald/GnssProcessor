#!/usr/bin/env python3
"""Read `logs/network_station_block.csv` — the card-ZP per-station LOCAL-BLOCK audit.

    python scripts/zp_station_block_read.py <run_dir> [<run_dir> ...]
    python scripts/zp_station_block_read.py --epochs 1,5,20 <run_dir> ...
    python scripts/zp_station_block_read.py --station WROC00POL <run_dir>

SPEC_event_harness §24.10.  The producer writes one row per station per TRACED epoch
(the first 20 EMITTED epochs, then every 60th) describing that station's LOCAL normal
block N_rr = A_r^T W A_r + folded station priors — the matrix
NetworkEstimator::reconstructStation inverts with the shared states held at their
solved values x_s.  Columns are documented in the `#` provenance line the producer
writes above the header; this reader skips it.

What it prints, per traced epoch, across the stations present:

  n_sta                 stations audited that epoch
  rank<n                how many have a RANK-DEFICIENT local block
  cond p50/p90/max      condition number of the block over its non-null subspace
  ev1 span histogram    which local KINDS span the SMALLEST eigenpair, as a count
                        over stations of the kind-set (e.g. "amb 41, zwd+clk 15")
  zwd_info p50/p10      the ZWD column's effective / raw information ratio -- the
                        §24.9 quantity ("~1/50" was the first estimate)
  x_r_norm p50          the reconstructed local correction's norm (metres)
  x_r_zwd p50           its ZWD element (metres) -- what network_zwd.csv publishes
  zero-red code/phase   fraction of DATA rows with redundancy < 1e-6, per kind

No bars, no verdicts: this is the eyes of the card.  With more than one run
directory it also prints a side-by-side of the same quantities at each shared epoch
index, which is how the two VEHICLES (carry_invariant vs phase_leveling) are read
against each other.
"""
import argparse
import csv
import os
import sys

# Canonical kind order for the span histogram, so "zwd+clk" and "clk+zwd" are the
# same bucket whatever order the eigenvector loaded them in.
KIND_ORDER = ["clk", "zwd", "rxcb", "iono", "amb", "isb"]


def pct(vals, q):
    """Linear-interpolation percentile of a list (q in [0, 100]); None if empty."""
    v = sorted(x for x in vals if x is not None)
    if not v:
        return None
    if len(v) == 1:
        return v[0]
    f = (len(v) - 1) * q / 100.0
    lo = int(f)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (f - lo)


def fnum(x, fmt="{:.3g}"):
    return "-" if x is None else fmt.format(x)


def parse_float(s):
    if s is None or s == "":
        return None
    try:
        f = float(s)
    except ValueError:
        return None
    return f if f == f else None      # drop NaN


def span_kinds(span):
    """The KIND set of a span field, canonically ordered.

    A span is `label=share` items joined by '|'; a label is `clk`, `zwd`,
    `rxcb:<sig>`, `iono:<sat>`, `amb:<sat>:<sig>` or the `col<k>` fallback.  Only the
    leading kind token is kept, so a direction spanning four different satellites'
    ionospheres reads as one bucket, `iono`.
    """
    kinds = set()
    for item in span.split("|"):
        if not item:
            continue
        lbl = item.rsplit("=", 1)[0]
        kinds.add(lbl.split(":", 1)[0])
    known = [k for k in KIND_ORDER if k in kinds]
    other = sorted(k for k in kinds if k not in KIND_ORDER)
    return "+".join(known + other) if (known or other) else "-"


def read(run_dir):
    """-> (rows, path).  rows are dicts with the CSV's own column names."""
    path = os.path.join(run_dir, "logs", "network_station_block.csv")
    if not os.path.exists(path):
        raise SystemExit("no station-block trace at %s "
                         "(was network.trace_station_block armed?)" % path)
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    return list(csv.DictReader(lines)), path


def group_epochs(rows):
    """-> [(key, [row, ...]), ...] in first-appearance (time) order."""
    out, idx = [], {}
    for r in rows:
        k = (r["week"], r["tow"])
        if k not in idx:
            idx[k] = len(out)
            out.append((k, []))
        out[idx[k]][1].append(r)
    return out


def epoch_stats(rs):
    """Everything the card reads off one traced epoch's station rows."""
    n_local = [int(r["n_local"]) for r in rs]
    rank = [int(r["rank"]) for r in rs]
    cond = [parse_float(r["cond"]) for r in rs]
    ratio = [parse_float(r["zwd_info_ratio"]) for r in rs]
    xrn = [parse_float(r["x_r_norm"]) for r in rs]
    xrz = [parse_float(r["x_r_zwd"]) for r in rs]

    hist = {}
    for r in rs:
        k = span_kinds(r["ev1_span"])
        hist[k] = hist.get(k, 0) + 1

    zc = sum(int(r["zero_red_code"]) for r in rs)
    zp = sum(int(r["zero_red_phase"]) for r in rs)
    nc = sum(int(r["n_rows_code"]) for r in rs)
    np_ = sum(int(r["n_rows_phase"]) for r in rs)
    nr = sum(int(r["n_rows"]) for r in rs)
    return {
        "n_sta": len(rs),
        "deficient": sum(1 for a, b in zip(rank, n_local) if a < b),
        "n_local_p50": pct(n_local, 50),
        "cond50": pct(cond, 50), "cond90": pct(cond, 90),
        "condmax": max([c for c in cond if c is not None], default=None),
        "hist": hist,
        "ratio50": pct(ratio, 50), "ratio10": pct(ratio, 10),
        "xrn50": pct(xrn, 50), "xrz50": pct(xrz, 50),
        "zero_code": (zc / nc if nc else None), "zero_phase": (zp / np_ if np_ else None),
        "zero_all": ((zc + zp) / nr if nr else None),
        "n_rows": nr, "n_rows_code": nc, "n_rows_phase": np_,
    }


def hist_str(hist, top=4):
    items = sorted(hist.items(), key=lambda kv: (-kv[1], kv[0]))
    s = ", ".join("%s %d" % (k, v) for k, v in items[:top])
    if len(items) > top:
        s += ", +%d more" % (len(items) - top)
    return s


HDR = ("%-4s %-12s %5s %6s %10s %10s %10s %9s %9s %9s %9s %7s %7s"
       % ("ep", "tow", "n_sta", "rank<n", "cond p50", "cond p90", "cond max",
          "zwdR p50", "zwdR p10", "|x_r| p50", "x_r_zwd", "zr_cod", "zr_pha"))


def line(i, key, st):
    return ("%-4d %-12s %5d %6d %10s %10s %10s %9s %9s %9s %9s %7s %7s"
            % (i, key[1], st["n_sta"], st["deficient"],
               fnum(st["cond50"], "{:.2e}"), fnum(st["cond90"], "{:.2e}"),
               fnum(st["condmax"], "{:.2e}"),
               fnum(st["ratio50"], "{:.2e}"), fnum(st["ratio10"], "{:.2e}"),
               fnum(st["xrn50"], "{:.3g}"), fnum(st["xrz50"], "{:.3g}"),
               fnum(st["zero_code"], "{:.3f}"), fnum(st["zero_phase"], "{:.3f}")))


def report(run_dir, want):
    rows, path = read(run_dir)
    eps = group_epochs(rows)
    print("=" * 118)
    print("%s   (%d rows, %d traced epoch(s))" % (path, len(rows), len(eps)))
    print("=" * 118)
    if not eps:
        print("  EMPTY -- the trace was armed but audited nothing; read the run-end "
              "'card ZP station block' line.")
        return {}
    print(HDR)
    stats = {}
    for i, (key, rs) in enumerate(eps, start=1):
        st = epoch_stats(rs)
        stats[i] = (key, st)
        if want and i not in want:
            continue
        print(line(i, key, st))
    print()
    print("  smallest-eigenpair span, over stations (kind sets):")
    for i, (key, st) in stats.items():
        if want and i not in want:
            continue
        print("    ep %-3d %-12s  %s" % (i, key[1], hist_str(st["hist"])))
    print()
    first = stats[min(stats)][1]
    last = stats[max(stats)][1]
    print("  rows per traced epoch: %d total (%d code + %d phase) at ep 1; "
          "%d (%d + %d) at ep %d"
          % (first["n_rows"], first["n_rows_code"], first["n_rows_phase"],
             last["n_rows"], last["n_rows_code"], last["n_rows_phase"], max(stats)))
    print("  median n_local: %s (ep 1) -> %s (ep %d)"
          % (fnum(first["n_local_p50"], "{:.0f}"), fnum(last["n_local_p50"], "{:.0f}"),
             max(stats)))
    print()
    return stats


def side_by_side(all_stats, names, want):
    idxs = sorted(set.intersection(*[set(s) for s in all_stats])) if all_stats else []
    if len(all_stats) < 2 or not idxs:
        return
    print("=" * 118)
    print("SIDE BY SIDE  " + "  vs  ".join(names))
    print("=" * 118)
    fields = [("rank<n / n_sta", lambda s: "%d/%d" % (s["deficient"], s["n_sta"])),
              ("cond p50", lambda s: fnum(s["cond50"], "{:.2e}")),
              ("cond max", lambda s: fnum(s["condmax"], "{:.2e}")),
              ("zwd_info_ratio p50", lambda s: fnum(s["ratio50"], "{:.2e}")),
              ("zwd_info_ratio p10", lambda s: fnum(s["ratio10"], "{:.2e}")),
              ("|x_r| p50 (m)", lambda s: fnum(s["xrn50"], "{:.4g}")),
              ("x_r_zwd p50 (m)", lambda s: fnum(s["xrz50"], "{:.4g}")),
              ("zero-red code frac", lambda s: fnum(s["zero_code"], "{:.3f}")),
              ("zero-red phase frac", lambda s: fnum(s["zero_phase"], "{:.3f}")),
              ("zero-red all frac", lambda s: fnum(s["zero_all"], "{:.3f}")),
              ("ev1 span (top)", lambda s: hist_str(s["hist"], top=2))]
    for i in idxs:
        if want and i not in want:
            continue
        print("-- traced epoch %d  (tow %s) " % (i, all_stats[0][i][0][1]) + "-" * 60)
        for label, fn in fields:
            cells = "".join("%-34s" % fn(st[i][1]) for st in all_stats)
            print("   %-20s %s" % (label, cells))
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dirs", nargs="+")
    ap.add_argument("--epochs", default="",
                    help="comma-separated 1-based TRACED-epoch indices to print "
                         "(default: all)")
    ap.add_argument("--station", default="",
                    help="also dump every traced row of this station verbatim")
    a = ap.parse_args()
    want = set(int(x) for x in a.epochs.split(",") if x.strip()) if a.epochs else set()

    all_stats, names = [], []
    for d in a.run_dirs:
        st = report(d, want)
        if st:
            all_stats.append(st)
            names.append(os.path.basename(os.path.normpath(d)))
    side_by_side(all_stats, names, want)

    if a.station:
        for d in a.run_dirs:
            rows, _ = read(d)
            print("=" * 118)
            print("%s -- %s" % (os.path.basename(os.path.normpath(d)), a.station))
            for r in rows:
                if r["station"] != a.station:
                    continue
                print("  tow %s n_local %s rank %s cond %s | ev1 %s [%s] | "
                      "zwd eff %s raw %s ratio %s | x_r %s zwd %s | zr %s/%s of %s"
                      % (r["tow"], r["n_local"], r["rank"], r["cond"], r["ev1_val"],
                         r["ev1_span"], r["zwd_info_eff"], r["zwd_info_raw"],
                         r["zwd_info_ratio"], r["x_r_norm"], r["x_r_zwd"],
                         r["zero_red_code"], r["zero_red_phase"], r["n_rows"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
