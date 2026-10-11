#!/usr/bin/env python3
"""Card ARL: where the decorrelated partial fix stops, per L1/L2 pair.

Reads a run's logs/network_producer_ar.csv written with
network.ambiguity_resolution.decorrelated_partial (the four pair columns
wl_float_sigma_cyc, wl_cond_cyc, wl_cond_sigma_cyc, nl_cond_sigma_cyc) and sorts every
pair-epoch of the window into:

  singles fixed     both single-signal integers determined (kept on both rows);
  WL fixed only     the wide lane determined by the fixed decorrelated components
                    (conditional sigma <= --det-sigma) but not the narrow lane: the narrow
                    lane is the limiter;
  WL not fixed      the wide lane still free: the wide lane is the limiter;
  no fix epoch      the search fixed no decorrelated component that epoch;
  masked            the suspect-corner mask kept the pair out of the search (card ARL,
                    network.ambiguity_resolution.suspect_epochs: the trace's suspect_bits
                    column is non-zero);
  unpaired          the DD's partner band was not in the search for another reason (no
                    partner in the basis, or the ils_max_dim cap).

On a trace written with the mask armed, the pairs flagged in its reentry column (a corner
released by the mask at that search: the pair's first search after the release) are also
classed and graded on their own, the release edge of the mask (invariant 7).

For the second and third classes it reports the truth errors of the floats (wide lane in
wide-lane cycles, ionosphere-free part in narrow-lane cycles, (f1 e1 - f2 e2)/(f1 - f2)) and,
for the first two, whether the determined integer equals truth.  Truth integers carry each
arc's wind-up branch (simnet_producer_ar_grade.load_windup_branches).

Usage: python scripts/simnet_producer_ar_pairs.py <fixture_dir> --run <run_dir>
                                                 [--settle-h 1] [--det-sigma 0.05]
"""
import argparse
import collections
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_producer_ar_grade as g  # noqa: E402
from simnet_amb_integer import load_truth, n_at  # noqa: E402

C = 299792458.0


def pct(a, n):
    return 100.0 * a / n if n else float("nan")


def q(v, p):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir")
    ap.add_argument("--run", required=True)
    ap.add_argument("--settle-h", type=float, default=1.0)
    ap.add_argument("--det-sigma", type=float, default=0.05)
    a = ap.parse_args()

    stations = sorted(d for d in os.listdir(a.fixture_dir)
                      if os.path.isfile(os.path.join(a.fixture_dir, d, "truth_ambiguities.csv")))
    n_int, slips = load_truth(a.fixture_dir, stations)
    br, _ = g.load_windup_branches(a.fixture_dir, a.run, stations)

    def N(sta, sat, sig, t):
        v = n_at(n_int, slips, sta, sat, sig, t)
        return None if v is None else v + g.branch_at(br, sta, sat, t)

    pairs = collections.defaultdict(dict)   # (tow, key) -> {signal: row}
    tow0 = None
    with open(os.path.join(a.run, "logs", "network_producer_ar.csv"), newline="") as f:
        rd = csv.DictReader(f)
        if "wl_cond_sigma_cyc" not in rd.fieldnames:
            raise SystemExit("trace has no pair columns: arm "
                             "network.ambiguity_resolution.decorrelated_partial")
        has_mask = "suspect_bits" in rd.fieldnames
        for r in rd:
            t = float(r["tow"])
            tow0 = t if tow0 is None else tow0
            if t - tow0 < a.settle_h * 3600.0:
                continue
            key = (r["cons"], r["sat_j"], r["ref_sat"], r["sta_r"], r["ref_sta"])
            pairs[(t, key)][r["signal"]] = r

    def masked(r):
        return has_mask and r.get("suspect_bits", "0") not in ("", "0")

    cls = collections.Counter()
    err = {"wl_only": {"wl": [], "nl": []}, "wl_free": {"wl": [], "nl": [], "wl_fsig": []}}
    ok = collections.Counter()
    re_cls = collections.Counter()   # the mask's re-entered pairs (first search after release)
    re_ok = collections.Counter()
    re_wrong = []
    for (t, key), rows in pairs.items():
        if len(rows) != 2:
            for r in rows.values():
                cls["masked" if masked(r) else "unpaired"] += 1
            continue
        (s1, r1), (s2, r2) = sorted(rows.items(), key=lambda kv: float(kv[1]["lambda_m"]))
        l1, l2 = float(r1["lambda_m"]), float(r2["lambda_m"])
        f1, f2 = C / l1, C / l2
        cons, sat_j, ref_sat, sta_r, ref_sta = key

        def dd_true(sig):
            v = [N(sta_r, sat_j, sig, t), N(sta_r, ref_sat, sig, t),
                 N(ref_sta, sat_j, sig, t), N(ref_sta, ref_sat, sig, t)]
            return None if None in v else (v[0] - v[1]) - (v[2] - v[3])

        T1, T2 = dd_true(s1), dd_true(s2)
        if T1 is None or T2 is None:
            cls["no truth"] += 1
            continue
        e1, e2 = float(r1["float_cyc"]) - T1, float(r2["float_cyc"]) - T2
        e_wl, e_nl = e1 - e2, (f1 * e1 - f2 * e2) / (f1 - f2)
        reentry = has_mask and r1.get("reentry") == "1" and r2.get("reentry") == "1"
        if r1["kept"] == "1" and r2["kept"] == "1":
            cls["singles fixed"] += 1
            good = int(r1["fixed_int"]) == T1 and int(r2["fixed_int"]) == T2
            ok["singles fixed"] += int(good)
            if reentry:
                re_cls["singles fixed"] += 1
                re_ok["singles fixed"] += int(good)
                if not good:
                    re_wrong.append((t, key, "singles", r1["fixed_int"], r2["fixed_int"], T1, T2))
            continue
        if masked(r1) or masked(r2):
            cls["masked"] += 1
            continue
        if r1["wl_float_sigma_cyc"] == "":
            cls["unpaired"] += 1
            continue
        if r1["wl_cond_sigma_cyc"] == "":
            cls["no fix epoch"] += 1
            if reentry:
                re_cls["no fix epoch"] += 1
            continue
        if float(r1["wl_cond_sigma_cyc"]) <= a.det_sigma:
            cls["WL fixed only"] += 1
            good = round(float(r1["wl_cond_cyc"])) == T1 - T2
            ok["WL fixed only"] += int(good)
            if reentry:
                re_cls["WL fixed only"] += 1
                re_ok["WL fixed only"] += int(good)
                if not good:
                    re_wrong.append((t, key, "WL", r1["wl_cond_cyc"], "", T1 - T2, ""))
            err["wl_only"]["wl"].append(abs(e_wl))
            err["wl_only"]["nl"].append(abs(e_nl))
        else:
            cls["WL not fixed"] += 1
            if reentry:
                re_cls["WL not fixed"] += 1
            err["wl_free"]["wl"].append(abs(e_wl))
            err["wl_free"]["nl"].append(abs(e_nl))
            err["wl_free"]["wl_fsig"].append(float(r1["wl_float_sigma_cyc"]))

    n = sum(cls.values())
    print("PAIR CLASSES  run=%s  settled >= h%g  determined: conditional sigma <= %.3f cycle"
          % (a.run, a.settle_h, a.det_sigma))
    for c in ("singles fixed", "WL fixed only", "WL not fixed", "no fix epoch", "masked",
              "unpaired", "no truth"):
        if cls[c] or c in ("singles fixed", "WL fixed only", "WL not fixed"):
            extra = ""
            if c in ok:
                extra = "   integer correct %d of %d" % (ok[c], cls[c])
            print("  %-15s %7d  %5.1f %%%s" % (c, cls[c], pct(cls[c], n), extra))
    for k, label in (("wl_only", "WL fixed only"), ("wl_free", "WL not fixed")):
        E = err[k]
        if not E["wl"]:
            continue
        print("  %s: float truth error  WL |e| p50 %.3f p90 %.3f (within 0.25: %.1f %%)   "
              "NL |e| p50 %.3f p90 %.3f (within 0.25: %.1f %%)"
              % (label, q(E["wl"], .5), q(E["wl"], .9), pct(sum(x < .25 for x in E["wl"]), len(E["wl"])),
                 q(E["nl"], .5), q(E["nl"], .9), pct(sum(x < .25 for x in E["nl"]), len(E["nl"]))))
        if "wl_fsig" in E:
            print("  %s: WL formal sigma before the fix p50 %.3f p90 %.3f cycle"
                  % (label, q(E["wl_fsig"], .5), q(E["wl_fsig"], .9)))
    if has_mask:
        n_re = sum(re_cls.values())
        print("RE-ENTRY (suspect-corner mask release; the pair's first search after it): %d pair(s)"
              % n_re)
        for c in ("singles fixed", "WL fixed only", "WL not fixed", "no fix epoch"):
            extra = ""
            if c in ("singles fixed", "WL fixed only"):
                extra = "   integer correct %d of %d" % (re_ok[c], re_cls[c])
            print("  %-15s %7d%s" % (c, re_cls[c], extra))
        for w in sorted(re_wrong):
            print("  WRONG at re-entry: tow %.1f %s %s determined %s %s truth %s %s" % w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
