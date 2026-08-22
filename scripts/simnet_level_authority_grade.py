#!/usr/bin/env python3
"""LV-6 G1 grader — the census operator applied to TRUTH.

`docs/SPEC_level_authority_internal.md` §4 G1 / §5 item 5.  The gate is

    corr(L_cs(t), censusOperator{truth}(t)) >= +0.8   settled
    offset ~ the MEDIAN of the injected biases
    |d(offset)/dt| <= 2 TECU/h                        settled

and §5 item 5 is explicit that the reference statistic is the census operator
applied to truth, "not the coverage-weighted mean — median vs mean skew".  This
script IS that operator.

WHAT THE CENSUS ACTUALLY OBSERVES (read off the code, never asserted — the
fixture's two bias-sign traps):

    cs.val = I1 - (b1-b2)/(gamma-1) - (B1-B2)/(gamma-1)      [slant, L1 metres]

    src/core/app/ApplicationNetworkAssembly.cpp:1440   i_code = -row.gf_code_m / row.gf_denom
    include/slip/NetworkRawObservables.hpp:70-71       gf_code_m = P1-P2, gf_denom = gamma-1

so an injected receiver GF bias enters at -1/(gamma-1)/K_L1 = -9.5196 SLANT TECU
per metre of b(C1C)-b(C2W), and the satellite OSB contrast enters identically.
Both are added here, so the operator lands in the census's own realization.

TRAP — THE TWO SHELLS DIFFER.  The census maps to vertical with the SH
configuration's shell (`estimation.global_vtec_sh.layer_height_km`, 450 km on
the wsh lineage; `NetworkLevelAuthority.cpp` reads `shc.layer_height_km`), while
`truth_atmo.csv`'s `obliquity` column was computed by the sim at
`iono.shell_height_km` (350 km).  Using the truth column would bias every
vertical value by several percent at low elevation.  This script recomputes the
mapping from `elev_deg` at the census's shell; pass --shell-km if the arm's SH
config differs.

Usage:
  python scripts/simnet_level_authority_grade.py <fixture_dir>          <run_dir>/logs/network_level_authority.csv [--shell-km 450] [--settle-frac 0.5]
"""

import csv, math, os, statistics, sys

F1, F2 = 1575.42e6, 1227.60e6
MU = (F1 / F2) ** 2
K_L1 = 40.3e16 / F1 ** 2
GF_TECU_PER_M = -1.0 / (MU - 1.0) / K_L1          # -9.5196 slant TECU per metre of (b1-b2)
RE_M, H_M = 6371.0e3, 450.0e3

def mapping(el_deg):
    s = (RE_M / (RE_M + H_M)) * math.cos(math.radians(el_deg))
    c = math.sqrt(max(0.0, 1.0 - s * s))
    return 1.0 / c if c > 1e-6 else None

def main(fixture, min_los=4, elev_mask=10.0):
    stations = [d for d in sorted(os.listdir(fixture))
                if os.path.isfile(os.path.join(fixture, d, "truth_atmo.csv"))]
    g, dB = {}, {}
    for s in stations:
        rows = list(csv.DictReader(open(os.path.join(fixture, s, "truth_rx_bias.csv"), newline="")))
        c = {r["code"]: float(r["value_m"]) for r in rows if r["kind"] == "code"}
        g[s] = c["C1C"] - c["C2W"]
        if not dB:
            for r in csv.DictReader(open(os.path.join(fixture, s, "truth_bias.csv"), newline="")):
                dB[(r["satellite"], r["signal"])] = float(r["osb_m"])
    dgf = {}
    for (sat, sig), v in dB.items():
        if sig == "C1C": dgf[sat] = dgf.get(sat, 0.0) + v
        if sig == "C2W": dgf[sat] = dgf.get(sat, 0.0) - v

    per_epoch = {}   # tow -> {station: (sum w*v, sum w, n)}
    for s in stations:
        gs = g[s]
        for r in csv.DictReader(open(os.path.join(fixture, s, "truth_atmo.csv"), newline="")):
            el = float(r["elev_deg"])
            if el < elev_mask: continue
            M = mapping(el)
            if M is None: continue
            sat = r["satellite"]
            if sat[0] != "G": continue                      # producer is GPS-only
            # TWO references, and the distinction IS gate G1.
            #   v_iono : the true ionosphere alone -> censusOperator{truth}.
            #            L_cs - this = what the injected biases put into the
            #            level, i.e. the C4 confound the gate is built to
            #            measure ("offset ~ the MEDIAN of the injected biases").
            #   v_obs  : truth PLUS the injected receiver GF bias and satellite
            #            OSB contrast -> what the census SHOULD observe if its
            #            conversion chain is right.  L_cs - this ~ 0 is a
            #            forward-model CLOSURE check, not a C4 grade.
            # Grading the offset against v_obs would be circular: the bias sits
            # on both sides and cancels, so the test could not fail however
            # large the confound was.
            stec = float(r["stec_total_tecu"])
            bias_tecu = GF_TECU_PER_M * (gs + dgf.get(sat, 0.0))
            w = math.sin(math.radians(el)) ** 2
            tow = round(float(r["tow_s"]), 3)
            d = per_epoch.setdefault(tow, {}).setdefault(s, [0.0, 0.0, 0.0, 0])
            d[0] += w * (stec / M); d[1] += w * ((stec + bias_tecu) / M)
            d[2] += w; d[3] += 1
    iono, obs = {}, {}
    for tow, per in per_epoch.items():
        mi = [a / w for a, b, w, n in per.values() if n >= min_los and w > 0]
        mo = [b / w for a, b, w, n in per.values() if n >= min_los and w > 0]
        if mi:
            iono[tow] = statistics.median(mi)
            obs[tow] = statistics.median(mo)
    return iono, obs

def _injected_median_vertical(fixture, elev_mask):
    """Median over stations of that station's injected GF bias expressed the way
    the census sees it: slant TECU divided by the per-LOS mapping, averaged over
    the station's own elevation distribution with the census's sin^2 weights.

    This is the number G1's offset test is compared against.  It is NOT simply
    median(bias)/mean(M): the mapping average is taken per station first, matching
    the operator's tier-1-then-tier-2 order."""
    stations = [d for d in sorted(os.listdir(fixture))
                if os.path.isfile(os.path.join(fixture, d, "truth_atmo.csv"))]
    per = []
    for s in stations:
        rows = list(csv.DictReader(open(os.path.join(fixture, s, "truth_rx_bias.csv"), newline="")))
        c = {r["code"]: float(r["value_m"]) for r in rows if r["kind"] == "code"}
        b = GF_TECU_PER_M * (c["C1C"] - c["C2W"])
        sw = swm = 0.0
        for r in csv.DictReader(open(os.path.join(fixture, s, "truth_atmo.csv"), newline="")):
            el = float(r["elev_deg"])
            if el < elev_mask or r["satellite"][0] != "G": continue
            M = mapping(el)
            if M is None: continue
            w = math.sin(math.radians(el)) ** 2
            sw += w; swm += w / M
        if sw > 0: per.append(b * swm / sw)
    return statistics.median(per) if per else float("nan")


def _exact_from_los_dump(fixture, los_csv, min_los):
    """censusOperator{truth} on the census's OWN sampling.

    The default path re-derives a LOS set from truth_atmo.csv (every row above
    the mask).  That is NOT the set the census reduced: the census keeps only
    LOS whose CS-GF state is MATURE (n >= min_n) and that appear in this epoch's
    modeled rows.  A reference built on a different set carries a different
    mapping distribution, which is exactly the sampling error SPEC §5.2 killed
    the planetary-mean grade for -- in miniature.

    With network.level_authority_los_dump armed the census publishes its own
    (station, sat, ipp, mapping, weight) rows, so the reference is evaluated on
    the identical LOS with the identical weights and the identical mapping.
    Only the FIELD differs: truth STEC instead of the observed CS-GF value.
    """
    truth = {}   # (station, sat, tow) -> stec_total_tecu
    stations = [d for d in sorted(os.listdir(fixture))
                if os.path.isfile(os.path.join(fixture, d, "truth_atmo.csv"))]
    for st in stations:
        for r in csv.DictReader(open(os.path.join(fixture, st, "truth_atmo.csv"), newline="")):
            truth[(st, r["satellite"], round(float(r["tow_s"]), 3))] = float(r["stec_total_tecu"])
    per, miss, hit = {}, 0, 0
    for r in csv.DictReader(open(los_csv, newline="")):
        tow = round(float(r["tow"]), 3)
        k = (r["station"], r["sat"], tow)
        if k not in truth:
            miss += 1
            continue
        hit += 1
        M = float(r["mapping"]); w = float(r["weight"])
        d = per.setdefault(tow, {}).setdefault(r["station"], [0.0, 0.0, 0])
        d[0] += w * (truth[k] / M); d[1] += w; d[2] += 1
    out = {}
    for tow, sta in per.items():
        means = [a / b for a, b, n in sta.values() if n >= min_los and b > 0]
        if means: out[tow] = statistics.median(means)
    return out, hit, miss


def _grade(fx, run_csv, shell_km, settle_frac, min_los, elev_mask, los_csv=None):
    global H_M
    H_M = shell_km * 1000.0
    truth, obsref = main(fx, min_los=min_los, elev_mask=elev_mask)
    inj = _injected_median_vertical(fx, elev_mask)
    exact = None
    if los_csv and os.path.isfile(los_csv):
        exact, hit, miss = _exact_from_los_dump(fx, los_csv, min_los)
        print(f"[exact sampling] LOS dump joined: {hit} rows matched, {miss} unmatched "
              f"({100.0 * hit / max(1, hit + miss):.1f}% matched) over {len(exact)} epochs")
        if exact:
            truth = exact
        else:
            print("[exact sampling] no epoch qualified — falling back to the re-derived set")
            exact = None
    print("REFERENCE              " + ("censusOperator{truth} on the CENSUS'S OWN LOS set "
                                        "(exact sampling)" if exact else
                                        "censusOperator{truth} re-derived from truth_atmo "
                                        "(APPROXIMATE sampling: all LOS above the mask, not "
                                        "the mature-CS-GF subset the census reduced)"))
    fleet = {}
    for r in csv.DictReader(open(run_csv, newline="")):
        if r["kind"] == "FLEET":
            fleet[round(float(r["tow"]), 3)] = float(r["level_tecu"])
    tows = sorted(set(truth) & set(fleet))
    if not tows:
        sys.exit("no joint epochs between the fixture truth and the census trace")
    cut = tows[int(len(tows) * settle_frac)] if settle_frac > 0 else tows[0]
    tows = [t for t in tows if t >= cut]
    x = [truth[t] for t in tows]
    y = [fleet[t] for t in tows]
    z = [obsref[t] for t in tows]
    mx, my = sum(x) / len(x), sum(y) / len(y)
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    corr = sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else float("nan")
    off = [b - a for a, b in zip(x, y)]
    om = sum(off) / len(off)
    n = len(tows)
    tm = sum(tows) / n
    slope = (sum((t - tm) * (o - om) for t, o in zip(tows, off))
             / sum((t - tm) ** 2 for t in tows)) if n > 2 else float("nan")
    print(f"settled epochs         {n}  ({tows[0]:.0f} .. {tows[-1]:.0f} TOW, "
          f"settle-frac {settle_frac}, shell {shell_km:.0f} km)")
    print(f"censusOperator{{truth}}  mean {mx:+.3f}  sd {statistics.pstdev(x):.3f} TECU")
    print(f"L_cs (run)             mean {my:+.3f}  sd {statistics.pstdev(y):.3f} TECU")
    print(f"corr(L_cs, truth-op)   {corr:+.4f}          [G1: >= +0.8; STOP if < +0.5]")
    print(f"offset L_cs - truth-op mean {om:+.3f}  sd {statistics.pstdev(off):.3f} TECU"
          f"   [G1: ~ {inj:+.3f}, the injected median in VERTICAL TECU]")
    clo = [b - c for c, b in zip(z, y)]
    cm = sum(clo) / len(clo)
    print(f"closure L_cs - obs-op  mean {cm:+.3f}  sd {statistics.pstdev(clo):.3f} TECU"
          f"   [wiring check, ~0 expected; NOT the C4 grade]")
    print(f"d(offset)/dt           {slope * 3600.0:+.3f} TECU/h   [G1: |.| <= 2]")
    verdict = ("PASS" if (corr >= 0.8 and abs(slope * 3600.0) <= 2.0)
               else ("STOP-RULE" if corr < 0.5 else "FAIL"))
    print(f"G1 VERDICT             {verdict}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir")
    ap.add_argument("level_authority_csv",
                    help="<run_dir>/logs/network_level_authority.csv")
    ap.add_argument("--shell-km", type=float, default=450.0,
                    help="the CENSUS's shell = estimation.global_vtec_sh.layer_height_km "
                         "(default 450 = the wsh lineage).  NOT the sim's "
                         "iono.shell_height_km.")
    ap.add_argument("--settle-frac", type=float, default=0.5,
                    help="discard this leading fraction as ignition (default 0.5; the "
                         "level channel needs ~2 h, so never grade a short arc)")
    ap.add_argument("--min-los", type=int, default=4,
                    help="match network.level_authority_min_los (default 4)")
    ap.add_argument("--elev-mask", type=float, default=10.0,
                    help="match processing.elevation_mask_deg (default 10)")
    ap.add_argument("--los-dump", default=None,
                    help="<run_dir>/logs/network_level_authority_los.csv from a run with "
                         "network.level_authority_los_dump armed.  When given, the reference "
                         "is evaluated on the census's OWN LOS set, weights and mappings "
                         "instead of a re-derived all-visible set (SPEC §5.2's \"SAME\").")
    a = ap.parse_args()
    if a.los_dump is None:
        cand = os.path.join(os.path.dirname(a.level_authority_csv),
                            "network_level_authority_los.csv")
        if os.path.isfile(cand):
            a.los_dump = cand
    sys.exit(_grade(a.fixture_dir, a.level_authority_csv, a.shell_km,
                    a.settle_frac, a.min_los, a.elev_mask, a.los_dump))
