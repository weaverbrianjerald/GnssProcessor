#!/usr/bin/env python3
"""Churn, L1/L2 agreement and coverage of the producer AR search's DD basis (card AGT,
docs/refound/AGT_arc_graph_shared_tree_2026-10-10.md, section 1 and section 7).

The producer AR search (filter/NetworkProducerAr) rebuilds its basis of between-station double
differences every epoch.  With `network.ambiguity_resolution.trace` armed, every DD of the basis
is written to `logs/network_producer_ar.csv` with its identity (cons, signal, sat_j, ref_sat,
sta_r, ref_sta) and, when its integer was kept, `kept` 1 and `fixed_int`.  A hold that carries
integers across epochs needs a basis whose cycles outlive the epoch, so this script measures, per
band (constellation letter plus signal):

  - DDs per epoch;
  - baselines (sta_r, ref_sta) kept from one epoch to the next, and of those the share whose
    reference satellite is unchanged;
  - DD identities kept from one epoch to the next (share of the previous epoch's DDs);
  - fixed DDs (kept 1) of one epoch still in the next epoch's basis;
  - per constellation with exactly two bands, the L1/L2 basis agreement (Jaccard of the
    (baseline, reference satellite) pairs and of the DD identities).

Medians are the upper median (element n // 2 of the sorted list), the convention of the numbers
quoted in card AGT section 7.

The cycle basis (card AGT, step AGT-5, `network.ambiguity_resolution.cycle_basis`) writes four more
columns: `cycle_kind` (partner or borrowed), `band_only`, `basis_age_ep` and `map_state` (same,
mapped, fresh_life, fresh_new: the DD's cycle against the last search's basis).  On such a trace the
churn figures are also split by kind (the kind of the DD at the earlier epoch; "/band_only" appended
for a band-only chord) and the map states are counted per band.  `--fresh-at TOW` lists the rows of
that epoch whose cycle is fresh (the slip-epoch check of card AGT section 4).

`--lattice` adds the coverage measurement: per band, the cycle-lattice dimension E - V + C of the
carried-ambiguity graph (receivers and satellites joined by one arc per carried (station,
satellite, signal); E arcs, V vertices, C components) from `logs/network_amb_state.csv` (one row
per carried arc per traced epoch; `erase:` rows skipped), beside the basis's DD count at the same
epoch, over the epochs at least `--settle-s` after the first traced one.  The carried graph
approximates the search's pool; the full-coverage basis of card AGT-5 measures it on the pool
itself.

Reads:  <run>/logs/network_producer_ar.csv, and with --lattice <run>/logs/network_amb_state.csv
Usage:  python scripts/simnet_ar_basis_churn.py <run_dir> [--lattice] [--settle-s 3600] [--json]
        python scripts/simnet_ar_basis_churn.py --selftest
"""
import argparse
import csv
import json
import os
import sys
import tempfile
from collections import defaultdict


def upper_median(values):
    """Element n // 2 of the sorted values; NaN when empty."""
    v = sorted(values)
    return v[len(v) // 2] if v else float("nan")


def _rows(path):
    """DictReader over a trace, skipping '#' banner lines."""
    with open(path, newline="") as fh:
        yield from csv.DictReader(line for line in fh if not line.startswith("#"))


def read_basis(run_dir):
    """Per epoch (in file order) and band: baselines -> reference satellite, DD identities,
    the fixed DDs with their integers, and on a cycle-basis trace each DD's kind and map state."""
    epochs = defaultdict(lambda: defaultdict(lambda: {"bl": {}, "dd": set(), "fixed": {},
                                                      "kind": {}, "map": {}}))
    order = []
    for row in _rows(os.path.join(run_dir, "logs", "network_producer_ar.csv")):
        tow = float(row["tow"])
        if not order or order[-1] != tow:
            order.append(tow)
        e = epochs[tow][row["cons"] + row["signal"]]
        e["bl"][(row["sta_r"], row["ref_sta"])] = row["ref_sat"]
        dd = (row["sat_j"], row["ref_sat"], row["sta_r"], row["ref_sta"])
        e["dd"].add(dd)
        if row["kept"] == "1":
            e["fixed"][dd] = int(row["fixed_int"])
        if row.get("cycle_kind"):
            e["kind"][dd] = row["cycle_kind"] + ("/band_only" if row.get("band_only") == "1" else "")
            e["map"][dd] = row.get("map_state", "")
    return epochs, order


def churn_by_kind(epochs, order, band):
    """One band's churn split by the kind of the DD at the earlier epoch (cycle-basis traces)."""
    keep, fix_keep, n_dd = defaultdict(list), defaultdict(list), defaultdict(list)
    for i, t in enumerate(order):
        e = epochs[t][band]
        per = defaultdict(int)
        for dd in e["dd"]:
            per[e["kind"].get(dd, "")] += 1
        for k, n in per.items():
            n_dd[k].append(n)
        if i == 0:
            continue
        p = epochs[order[i - 1]][band]
        prev_by = defaultdict(list)
        for dd in p["dd"]:
            prev_by[p["kind"].get(dd, "")].append(dd)
        for k, dds in prev_by.items():
            keep[k].append(sum(1 for dd in dds if dd in e["dd"]) / len(dds))
            fixed = [dd for dd in dds if dd in p["fixed"]]
            if fixed:
                fix_keep[k].append(sum(1 for dd in fixed if dd in e["dd"]) / len(fixed))
    return {k: {"dd_per_epoch_p50": upper_median(n_dd[k]),
                "dd_identity_kept_p50": upper_median(keep[k]),
                "dd_identity_kept_min": min(keep[k]) if keep[k] else float("nan"),
                "fixed_in_next_basis_p50": upper_median(fix_keep[k]),
                "fixed_in_next_basis_min": min(fix_keep[k]) if fix_keep[k] else float("nan")}
            for k in sorted(n_dd) if k}


def map_states(epochs, order, band):
    """One band's map-state counts over the run (cycle-basis traces)."""
    counts = defaultdict(int)
    for t in order:
        for state in epochs[t][band]["map"].values():
            counts[state] += 1
    return dict(sorted(counts.items()))


def fresh_at(epochs, tow):
    """The rows of epoch `tow` whose cycle is fresh, per band: (sat_j, ref_sat, sta_r, ref_sta, state)."""
    out = {}
    for band, e in sorted(epochs.get(tow, {}).items()):
        rows = sorted(dd + (s,) for dd, s in e["map"].items() if s.startswith("fresh"))
        if rows:
            out[band] = rows
    return out


def dd_arcs(dd):
    """The four (station, satellite) arcs of a DD identity (sat_j, ref_sat, sta_r, ref_sta)."""
    sat_j, ref_sat, sta_r, ref_sta = dd
    return {(sta_r, sat_j), (sta_r, ref_sat), (ref_sta, sat_j), (ref_sta, ref_sat)}


def arcs_of(dds):
    """Every arc some DD of a basis runs through."""
    out = set()
    for dd in dds:
        out |= dd_arcs(dd)
    return out


def churn(epochs, order):
    """The per-band churn figures and the per-constellation L1/L2 agreement."""
    bands = sorted({b for t in order for b in epochs[t]})
    out = {"epochs": len(order), "bands": {}, "pairs": {}}
    for b in bands:
        n_dd, bl_keep, ref_keep, dd_keep, fix_keep = [], [], [], [], []
        dd_keep_live, fix_keep_live = [], []
        for i, t in enumerate(order):
            e = epochs[t][b]
            n_dd.append(len(e["dd"]))
            if i == 0:
                continue
            p = epochs[order[i - 1]][b]
            if not p["bl"] or not e["bl"]:
                continue
            same_bl = set(p["bl"]) & set(e["bl"])
            bl_keep.append(len(same_bl) / len(p["bl"]))
            ref_keep.append(sum(1 for x in same_bl if p["bl"][x] == e["bl"][x]) / max(1, len(same_bl)))
            dd_keep.append(len(p["dd"] & e["dd"]) / max(1, len(p["dd"])))
            if p["fixed"]:
                fix_keep.append(sum(1 for k in p["fixed"] if k in e["dd"]) / len(p["fixed"]))
            # The same two figures over the DDs whose four arcs are still in the next basis: a
            # DD through an arc that left the network (a setting or departing satellite) cannot be
            # in any basis, so only these measure the basis's own churn.
            live = arcs_of(e["dd"])
            p_live = [dd for dd in p["dd"] if dd_arcs(dd) <= live]
            if p_live:
                dd_keep_live.append(sum(1 for dd in p_live if dd in e["dd"]) / len(p_live))
            f_live = [dd for dd in p["fixed"] if dd_arcs(dd) <= live]
            if f_live:
                fix_keep_live.append(sum(1 for dd in f_live if dd in e["dd"]) / len(f_live))
        out["bands"][b] = {
            "dd_per_epoch_p50": upper_median(n_dd),
            "baseline_kept_p50": upper_median(bl_keep),
            "baseline_kept_min": min(bl_keep) if bl_keep else float("nan"),
            "ref_sat_kept_p50": upper_median(ref_keep),
            "dd_identity_kept_p50": upper_median(dd_keep),
            "dd_identity_kept_min": min(dd_keep) if dd_keep else float("nan"),
            "fixed_in_next_basis_p50": upper_median(fix_keep),
            "fixed_in_next_basis_min": min(fix_keep) if fix_keep else float("nan"),
            "dd_identity_kept_live_p50": upper_median(dd_keep_live),
            "dd_identity_kept_live_min": min(dd_keep_live) if dd_keep_live else float("nan"),
            "fixed_in_next_basis_live_p50": upper_median(fix_keep_live),
            "fixed_in_next_basis_live_min": min(fix_keep_live) if fix_keep_live else float("nan"),
        }
        if any(epochs[t][b]["kind"] for t in order):
            out["bands"][b]["by_kind"] = churn_by_kind(epochs, order, b)
            out["bands"][b]["map_states"] = map_states(epochs, order, b)
    for cons in sorted({b[0] for b in bands}):
        pair = [b for b in bands if b[0] == cons]
        if len(pair) != 2:
            continue
        agree_bl, agree_dd = [], []
        for t in order:
            a, c = epochs[t][pair[0]], epochs[t][pair[1]]
            if not a["bl"] or not c["bl"]:
                continue
            ua, uc = set(a["bl"].items()), set(c["bl"].items())
            agree_bl.append(len(ua & uc) / max(1, len(ua | uc)))
            agree_dd.append(len(a["dd"] & c["dd"]) / max(1, len(a["dd"] | c["dd"])))
        out["pairs"][cons] = {
            "bands": pair,
            "baseline_jaccard_p50": upper_median(agree_bl),
            "baseline_jaccard_min": min(agree_bl) if agree_bl else float("nan"),
            "dd_jaccard_p50": upper_median(agree_dd),
            "dd_jaccard_min": min(agree_dd) if agree_dd else float("nan"),
        }
    return out


def cycle_dimension(arcs):
    """E - V + C of the bipartite graph of (station, satellite) arcs; returns (E, V, C)."""
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for sta, sat in arcs:
        ra, rb = find("R" + sta), find("S" + sat)
        if ra != rb:
            parent[ra] = rb
    n_comp = len({find(v) for v in parent})
    return len(arcs), len(parent), n_comp


def lattice(run_dir, epochs, settle_s):
    """Per band, over the settled epochs: carried arcs, E - V + C and the basis DD count."""
    arcs = defaultdict(set)
    for row in _rows(os.path.join(run_dir, "logs", "network_amb_state.csv")):
        if row["event"].startswith("erase"):
            continue
        arcs[(float(row["tow"]), row["sat"][0] + row["signal"])].add((row["station"], row["sat"]))
    if not arcs:
        return {}
    t0 = min(t for t, _ in arcs)
    per_band = defaultdict(list)
    for (tow, band), e in arcs.items():
        if tow - t0 < settle_s:
            continue
        n_e, n_v, n_c = cycle_dimension(e)
        dim = n_e - n_v + n_c
        basis = len(epochs[tow][band]["dd"]) if tow in epochs else 0
        per_band[band].append((n_e, dim, basis))
    return {b: {"settled_epochs": len(v),
                "arcs_p50": upper_median([x[0] for x in v]),
                "lattice_dim_p50": upper_median([x[1] for x in v]),
                "basis_dd_p50": upper_median([x[2] for x in v]),
                "coverage_p50": upper_median([x[2] / x[1] for x in v if x[1]])}
            for b, v in sorted(per_band.items())}


def report(res):
    print(f"epochs {res['churn']['epochs']}")
    for b, s in res["churn"]["bands"].items():
        print(f"{b}: DDs/epoch p50 {s['dd_per_epoch_p50']}"
              f"  baseline kept p50 {s['baseline_kept_p50']:.3f} min {s['baseline_kept_min']:.3f}"
              f"  ref-sat kept p50 {s['ref_sat_kept_p50']:.3f}"
              f"  DD identity kept p50 {s['dd_identity_kept_p50']:.3f} min {s['dd_identity_kept_min']:.3f}"
              f"  fixed in next basis p50 {s['fixed_in_next_basis_p50']:.3f}"
              f" min {s['fixed_in_next_basis_min']:.3f}")
        print(f"  {b} over DDs whose arcs survive: DD identity kept p50 {s['dd_identity_kept_live_p50']:.3f}"
              f" min {s['dd_identity_kept_live_min']:.3f}  fixed in next basis p50"
              f" {s['fixed_in_next_basis_live_p50']:.3f} min {s['fixed_in_next_basis_live_min']:.3f}")
        for k, ks in s.get("by_kind", {}).items():
            print(f"  {b} {k}: DDs/epoch p50 {ks['dd_per_epoch_p50']}"
                  f"  DD identity kept p50 {ks['dd_identity_kept_p50']:.3f} min {ks['dd_identity_kept_min']:.3f}"
                  f"  fixed in next basis p50 {ks['fixed_in_next_basis_p50']:.3f}"
                  f" min {ks['fixed_in_next_basis_min']:.3f}")
        if s.get("map_states"):
            print(f"  {b} map states: " + ", ".join(f"{k} {n}" for k, n in s["map_states"].items()))
    for cons, s in res["churn"]["pairs"].items():
        print(f"{cons} {'/'.join(s['bands'])}: (baseline, ref sat) Jaccard p50 {s['baseline_jaccard_p50']:.3f}"
              f" min {s['baseline_jaccard_min']:.3f}; DD Jaccard p50 {s['dd_jaccard_p50']:.3f}"
              f" min {s['dd_jaccard_min']:.3f}")
    for b, s in res.get("lattice", {}).items():
        print(f"{b}: settled epochs {s['settled_epochs']}  carried arcs p50 {s['arcs_p50']}"
              f"  E-V+C p50 {s['lattice_dim_p50']}  basis DDs p50 {s['basis_dd_p50']}"
              f"  coverage p50 {s['coverage_p50']:.3f}")
    if "fresh_at" in res:
        fa = res["fresh_at"]
        print(f"fresh rows at TOW {fa['tow']}: " + ("none" if not fa["rows"] else ""))
        for b, rows in fa["rows"].items():
            for sat_j, ref_sat, sta_r, ref_sta, state in rows:
                print(f"  {b} {sta_r}/{sat_j} vs {ref_sta}/{ref_sat}: {state}")


def measure(run_dir, with_lattice, settle_s, fresh_tow=None):
    epochs, order = read_basis(run_dir)
    res = {"run": run_dir, "churn": churn(epochs, order)}
    if with_lattice:
        res["lattice"] = lattice(run_dir, epochs, settle_s)
    if fresh_tow is not None:
        res["fresh_at"] = {"tow": fresh_tow, "rows": fresh_at(epochs, fresh_tow)}
    return res


def selftest():
    """Two epochs, one band pair, a four-station triangle-plus-tail graph."""
    hdr = "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,kept,fixed_int\n"
    body = []
    for sig in ("L1C", "L2W"):
        # Epoch 1: two DDs on baseline (B, A) against G01, one fixed.
        body.append(f"1,0.0,G,{sig},G02,G01,B,A,1,3\n")
        body.append(f"1,0.0,G,{sig},G03,G01,B,A,0,0\n")
    for sig in ("L1C", "L2W"):
        # Epoch 2: the fixed DD survives, the other is replaced; L2W also changes reference.
        body.append(f"1,30.0,G,{sig},G02,G01,B,A,0,0\n")
        ref = "G01" if sig == "L1C" else "G04"
        body.append(f"1,30.0,G,{sig},G05,{ref},B,A,0,0\n")
    amb_hdr = "# banner\nweek,tow,station,sat,signal,event\n"
    amb = []
    for tow in (0.0, 30.0):
        # Stations A, B, C and satellites G01, G02: arcs A-G01, A-G02, B-G01, B-G02, C-G01
        # (E 5, V 5, C 1, so E - V + C = 1), plus an erase row that must be skipped.
        for sta, sat in (("A", "G01"), ("A", "G02"), ("B", "G01"), ("B", "G02"), ("C", "G01")):
            amb.append(f"1,{tow},{sta},{sat},L1C,\n")
        amb.append(f"1,{tow},C,G02,L1C,erase:gap_prune\n")
    ok = True
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "logs"))
        with open(os.path.join(d, "logs", "network_producer_ar.csv"), "w", newline="") as fh:
            fh.write(hdr + "".join(body))
        with open(os.path.join(d, "logs", "network_amb_state.csv"), "w", newline="") as fh:
            fh.write(amb_hdr + "".join(amb))
        res = measure(d, True, 0.0)
    l1, l2 = res["churn"]["bands"]["GL1C"], res["churn"]["bands"]["GL2W"]
    checks = [
        ("epochs", res["churn"]["epochs"] == 2),
        ("dd per epoch", l1["dd_per_epoch_p50"] == 2),
        ("dd identity kept 1/2", abs(l1["dd_identity_kept_p50"] - 0.5) < 1e-12),
        ("fixed in next basis 1/1", l1["fixed_in_next_basis_p50"] == 1.0),
        ("ref sat kept L1C", l1["ref_sat_kept_p50"] == 1.0),
        # The baseline map keeps the last row's reference, so L2W's (B, A) reads G04 at epoch 2.
        ("ref sat changed L2W", l2["ref_sat_kept_p50"] == 0.0),
        ("baseline jaccard min 0", res["churn"]["pairs"]["G"]["baseline_jaccard_min"] == 0.0),
        ("dd jaccard 1/3", abs(res["churn"]["pairs"]["G"]["dd_jaccard_min"] - 1.0 / 3.0) < 1e-12),
        ("lattice dim 1", res["lattice"]["GL1C"]["lattice_dim_p50"] == 1),
        # Epoch 2 keeps every arc of (G02, G01, B, A) but none of G03 at B: the live figures
        # count only the surviving DD.
        ("live identity kept 1/1", l1["dd_identity_kept_live_p50"] == 1.0),
        ("lattice arcs 5", res["lattice"]["GL1C"]["arcs_p50"] == 5),
    ]
    # A cycle-basis trace (card AGT): the churn split by kind, the map states, the fresh rows.
    cb_hdr = "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,kept,fixed_int,cycle_kind,band_only,basis_age_ep,map_state"
    cb_rows = ["1,0.0,G,L1C,G02,G01,B,A,1,3,partner,0,0,fresh_new",
               "1,0.0,G,L1C,G03,G01,C,B,1,5,borrowed,0,0,fresh_new",
               "1,30.0,G,L1C,G02,G01,B,A,0,0,partner,0,1,same",
               "1,30.0,G,L1C,G03,G02,C,A,0,0,borrowed,1,0,fresh_life"]
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "logs"))
        with open(os.path.join(d, "logs", "network_producer_ar.csv"), "w", newline="") as fh:
            fh.write("".join(r + chr(10) for r in [cb_hdr] + cb_rows))
        cb = measure(d, False, 0.0, fresh_tow=30.0)
    by = cb["churn"]["bands"]["GL1C"]["by_kind"]
    checks += [
        ("kind partner kept", by["partner"]["dd_identity_kept_p50"] == 1.0
         and by["partner"]["fixed_in_next_basis_p50"] == 1.0),
        ("kind borrowed replaced", by["borrowed"]["dd_identity_kept_p50"] == 0.0
         and by["borrowed"]["fixed_in_next_basis_p50"] == 0.0),
        ("kind band-only counted", by["borrowed/band_only"]["dd_per_epoch_p50"] == 1),
        ("map states", cb["churn"]["bands"]["GL1C"]["map_states"]
         == {"fresh_life": 1, "fresh_new": 2, "same": 1}),
        ("fresh at 30", cb["fresh_at"]["rows"] == {"GL1C": [("G03", "G02", "C", "A", "fresh_life")]}),
    ]
    for name, good in checks:
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} {name}")
    print("selftest " + ("PASS" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--lattice", action="store_true",
                    help="also measure E - V + C of the carried-ambiguity graph")
    ap.add_argument("--settle-s", type=float, default=3600.0,
                    help="lattice: skip the epochs before first traced epoch + this (default 3600)")
    ap.add_argument("--fresh-at", type=float, default=None,
                    help="cycle-basis traces: list the rows of this epoch (TOW) whose cycle is fresh")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("run_dir is required")
    res = measure(a.run_dir, a.lattice, a.settle_s, a.fresh_at)
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        report(res)
    return 0


if __name__ == "__main__":
    sys.exit(main())
