"""The two-run audit, bar F4 of card NRF (card GRA, plan step G3, done 2026-11-19 as a second implementation): two
runs' integers compared against each other without truth, written from the audit's definition (card NRF section 107,
bar 5; `scripts/nrf_two_run_audit.py`, which stays where it is).

    py -3.14 scripts/grading/two_run.py <run A> <run B> [--states held,decided,would_hold] [--view legacy|bylink]
                                        [--json out.json] [--share 0.5] [--min-loops 3] [--list N]
    py -3.14 scripts/grading/two_run.py --selftest

The definition.  At every epoch common to both runs and on each band (constellation and signal), the links whose
integers both runs know are compared: a ring-fix link in one of --states (logs/network_ring_fix.csv: n1 on the first
signal, n2 on the second) or a tree link of the chain with an integer pin (logs/network_chain_edges.csv, target_cyc;
the tree pin wins over a ring-fix row of the same link).  The two runs' frames differ by one potential per receiver and
one per satellite, so on the graph of common links D = N_A - N_B must equal x_receiver - y_satellite.  An error both
runs share passes (the audit's known limit: a pair that agrees is not thereby right).

The views:
  legacy  byte for byte nrf_two_run_audit.py without its wind-up options (its printout and its --json file).  A
          breadth-first walk from the first vertex of each component (vertices in the order of the sorted common links)
          assigns the potentials over a spanning tree and checks every other link; each check is one independent loop
          and a failing check is a loop the runs disagree on.  The count of failing checks depends on the walk: one
          link the runs disagree on fails a check for every loop the walk closes through it, and a loop that the walk
          closes through its tree path rather than through the link itself fails too, so the count measures the
          link's place in the spanning tree as much as the disagreement (card GRA, B4 (b); the managing session's
          attribution of 2026-11-19, section 3: 1 502 failing checks per band were two links).
  bylink  walk-free, the count decision 4 of the night report asks for: at each epoch and band where the walk fails,
          every four-link loop (r1, s1, r2, s2) with all four links common closes to
          D(r1,s1) - D(r1,s2) - D(r2,s1) + D(r2,s2) = 0 when the runs differ by a frame only; a link that fails more
          than --share of at least --min-loops of its loops is a disagreeing link that epoch (a correct link fails only
          the loops through a disagreeing one).  Where the walk passes, every loop closes, so no other epoch can hold a
          disagreeing link.  Printed per band: epochs, link-epochs, the walk's failing checks split into those whose
          loop (the tree path plus the closing link) holds a disagreeing link (as the closing link or on the tree path)
          and those that hold none (their closing links listed: a disagreement the rule does not attribute to one link, for
          instance a link with fewer than --min-loops loops), the disagreeing link-epochs and each disagreeing link (epochs, TOW span, hours from
          the first epoch and of the day, closure mode, failing share, kinds), in the line format of the five-day
          read's two_run_loops.py; then the distinct links per constellation, both bands together.

A run is a bare tag, a "_rap_" name or a path (runs.run_dir; the legacy printout names the run as
nrf_two_run_audit.py did, so give the same form to compare a printout).  Epoch times are kept to the millisecond, as
the original keeps them.
"""
import argparse
import collections
import csv
import json
import os
import statistics
import sys

import runs


def links_of(run, states):
    """{tow: {band: {(station, sat): (integer, kind)}}} of one run; kind "rf" (a ring-fix link) or "tree" (a pin)."""
    out = collections.defaultdict(lambda: collections.defaultdict(dict))
    path = runs.trace_path(run, "network_ring_fix.csv")
    if os.path.exists(path):
        with open(path, newline="") as fh:
            rd = csv.reader(fh)
            ix = {k: i for i, k in enumerate(next(rd))}
            itow, ista, isat, is1, is2, ist, in1, in2 = (ix[k] for k in ("tow", "station", "sat", "sig1", "sig2",
                                                                         "state", "n1", "n2"))
            for r in rd:
                if r[ist] not in states or r[in1] == "" or r[in2] == "":
                    continue
                at = out[round(float(r[itow]), 3)]
                link = (r[ista], r[isat])
                at[r[isat][:1] + "|" + r[is1]][link] = (int(r[in1]), "rf")
                at[r[isat][:1] + "|" + r[is2]][link] = (int(r[in2]), "rf")
    with open(runs.trace_path(run, "network_chain_edges.csv"), newline="") as fh:
        rd = csv.reader(fh)
        ix = {k: i for i, k in enumerate(next(rd))}
        itow, isys, iband, ista, isat, icyc = (ix[k] for k in ("tow", "system", "band", "edge_sta", "edge_sat",
                                                               "target_cyc"))
        for r in rd:
            t = r[icyc].strip()
            if not t:
                continue
            v = float(t)
            if abs(v - round(v)) > 1e-6:
                continue
            out[round(float(r[itow]), 3)][r[isys] + "|" + r[iband]][(r[ista], r[isat])] = (int(round(v)), "tree")
    return out


def walk(a, b):
    """The legacy walk over one band at one epoch (a, b: {(station, sat): (integer, kind)}): (checks, failures,
    kinds, loops), failures as (link, D, the potentials' difference), loops the links of each failing check's loop."""
    common = sorted(k for k in a if k in b)
    kinds = collections.Counter((a[k][1], b[k][1]) for k in common)
    adj = collections.defaultdict(list)
    for link in common:
        d = a[link][0] - b[link][0]
        adj["R:" + link[0]].append(("S:" + link[1], d, link))
        adj["S:" + link[1]].append(("R:" + link[0], -d, link))
    pot, parent, used, checks, fails, loops = {}, {}, set(), 0, [], []
    for root in adj:
        if root in pot:
            continue
        pot[root], parent[root] = 0, None
        queue = collections.deque([root])
        while queue:
            v = queue.popleft()
            for w, d, link in adj[v]:
                if w not in pot:
                    pot[w] = pot[v] - d
                    parent[w] = (v, link)
                    used.add(link)
                    queue.append(w)
                elif link not in used:
                    used.add(link)
                    checks += 1
                    if pot[v] - pot[w] != d:
                        fails.append((link, d, pot[v] - pot[w]))
                        loops.append(tree_path(parent, v, w))
    return checks, fails, kinds, loops


def tree_path(parent, v, w):
    """The links of the spanning tree's path between vertices v and w."""
    up = {}
    x, path_v = v, []
    while x is not None:
        up[x] = len(path_v)
        p = parent[x]
        if p is None:
            break
        path_v.append(p[1])
        x = p[0]
    x, path_w = w, []
    while x not in up:
        p = parent[x]
        path_w.append(p[1])
        x = p[0]
    return path_v[:up[x]] + path_w


def loop_failures(links, min_loops, share):
    """{link: (closure mode, failing share, loops)} of the links failing more than share of at least min_loops of
    their four-link loops; links {(station, sat): D}.  For receivers r and r2 with common satellites S, a loop
    (r, s, r2, s2) closes to e(s) - e(s2), e(s) = D(r, s) - D(r2, s): for (r, s) the closure is e(s) - e(s2), for
    (r2, s) its negative (the closure written from that link, as two_run_loops.py writes it)."""
    by_sta = collections.defaultdict(dict)
    for (r, s), d in links.items():
        by_sta[r][s] = d
    stas = sorted(by_sta)
    n_loops, n_fail, values = collections.Counter(), collections.Counter(), collections.defaultdict(collections.Counter)
    for i, r in enumerate(stas):
        for r2 in stas[i + 1:]:
            e = {s: d - by_sta[r2][s] for s, d in by_sta[r].items() if s in by_sta[r2]}
            if len(e) < 2:
                continue
            cnt = collections.Counter(e.values())
            for s, es in e.items():
                n_loops[(r, s)] += len(e) - 1
                n_loops[(r2, s)] += len(e) - 1
                for v, k in cnt.items():
                    if v != es:
                        n_fail[(r, s)] += k
                        n_fail[(r2, s)] += k
                        values[(r, s)][es - v] += k
                        values[(r2, s)][v - es] += k
    out = {}
    for link, n in n_loops.items():
        nb = n_fail[link]
        if n >= min_loops and nb > share * n:
            top = max(values[link].values())
            out[link] = (min(v for v, k in values[link].items() if k == top), nb / n, n)
    return out


def audit(run_a, run_b, states, bylink=False, min_loops=3, share=0.5):
    """Per band: the legacy figures, and with bylink the disagreeing links and the split of the walk's failures."""
    A, B = links_of(run_a, states), links_of(run_b, states)
    per_band = collections.defaultdict(lambda: {"epochs": 0, "checks": 0, "failing": 0, "kinds": collections.Counter(),
                                                "dropped": 0, "no_anchor": 0, "anchor_disagree": 0,
                                                "link_epochs": 0, "on_link": 0, "on_tree": 0, "none": 0,
                                                "none_links": collections.Counter(),
                                                "bad": collections.defaultdict(list)})
    failures = []
    for tow in sorted(set(A) & set(B)):
        for band in sorted(set(A[tow]) & set(B[tow])):
            la, lb = A[tow][band], B[tow][band]
            checks, fails, kinds, loops = walk(la, lb)
            pb = per_band[band]
            pb["epochs"] += 1
            pb["checks"] += checks
            pb["failing"] += len(fails)
            pb["kinds"].update(kinds)
            for link, d, expect in fails:
                failures.append({"tow": tow, "band": band, "station": link[0], "sat": link[1], "d": d,
                                 "potentials": expect})
            if not bylink:
                continue
            pb["link_epochs"] += sum(kinds.values())
            if not fails:
                continue
            common = {k: la[k][0] - lb[k][0] for k in la if k in lb}
            bad = loop_failures(common, min_loops, share)
            for k, (mode, sh, n) in bad.items():
                pb["bad"][k].append((tow, mode, sh, la[k][1] + "/" + lb[k][1]))
            for (link, _, _), path in zip(fails, loops):
                if link in bad:
                    pb["on_link"] += 1
                elif any(x in bad for x in path):
                    pb["on_tree"] += 1
                else:
                    pb["none"] += 1
                    pb["none_links"][link] += 1
    first = min(min(A, default=0), min(B, default=0))
    return per_band, failures, first


def legacy_report(ra, rb, states, per_band, failures, n_list=20):
    out = ["== two-run audit: %s  vs  %s  (states %s%s%s)" % (ra, rb, states, "", "")]
    tot_c = tot_f = 0
    for band, pb in sorted(per_band.items()):
        k = pb["kinds"]
        out.append("   %-7s epochs %4d  common links: rf-rf %7d  rf-tree %6d  tree-rf %6d  tree-tree %7d   checks %7d  "
                   "failing %d  (links dropped %d)" % (band, pb["epochs"], k[("rf", "rf")], k[("rf", "tree")],
                                                       k[("tree", "rf")], k[("tree", "tree")], pb["checks"],
                                                       pb["failing"], pb["dropped"]))
        tot_c += pb["checks"]
        tot_f += pb["failing"]
    out.append("   total checks %d, failing %d" % (tot_c, tot_f))
    sizes = collections.Counter((f["band"], f["d"] - f["potentials"]) for f in failures)
    if sizes:
        out.append("   failing closures by band and size: " + ", ".join(
            "%s %+d: %d" % (b, z, n) for (b, z), n in sorted(sizes.items())))
    for f in failures[:n_list]:
        out.append("   FAIL tow %.0f %s %s %s: D %d against the potentials' %d" % (
            f["tow"], f["band"], f["station"], f["sat"], f["d"], f["potentials"]))
    return out, tot_f


def legacy_json(path, ra, rb, states, per_band, failures):
    with open(path, "w") as fh:
        json.dump({"run_a": ra, "run_b": rb, "states": states,
                   "bands": {b: {"epochs": v["epochs"], "checks": v["checks"], "failing": v["failing"],
                                 "dropped": v["dropped"], "no_anchor": v["no_anchor"],
                                 "anchor_disagree": v["anchor_disagree"],
                                 "kinds": {"%s-%s" % kk: n for kk, n in v["kinds"].items()}}
                             for b, v in per_band.items()},
                   "failures": failures}, fh, indent=1)


def bylink_report(ra, rb, states, per_band, first, min_loops, share):
    day0 = first // 86400 * 86400
    out = ["== two-run audit by distinct link (walk-free): %s  vs  %s  (states %s; a link disagrees at an epoch when it "
           "fails more than %g of at least %d four-link loops)" % (ra, rb, states, share, min_loops)]
    distinct = collections.defaultdict(set)
    for band, pb in sorted(per_band.items()):
        bad = pb["bad"]
        out.append(f"   {band}: epochs {pb['epochs']}, link-epochs {pb['link_epochs']}, walk failing checks "
                   f"{pb['failing']} (on a loop holding a disagreeing link: as the closing link {pb['on_link']}, on the "
                   f"tree path {pb['on_tree']}; holding none {pb['none']}), disagreeing link-epochs "
                   f"{sum(len(v) for v in bad.values())}; disagreeing links {len(bad)}")
        if pb["none_links"]:
            out.append("      failing checks on a loop holding no link the rule names, by closing link: " + ", ".join(
                f"{a}-{b} {n}" for (a, b), n in sorted(pb["none_links"].items(), key=lambda kv: (-kv[1], kv[0]))[:12])
                + (" (and others)" if len(pb["none_links"]) > 12 else ""))
        for k in sorted(bad, key=lambda x: (-len(bad[x]), x)):
            v = bad[k]
            ts = [x[0] for x in v]
            distinct[band.split("|")[0]].add(k)
            out.append(f"      {k[0]}-{k[1]}: {len(v)} epochs, TOW {ts[0]:.0f}..{ts[-1]:.0f}, hours "
                       f"{(ts[0] - first) / 3600 + 1:.3f}..{(ts[-1] - first) / 3600 + 1:.3f} (of day "
                       f"{(ts[0] - day0) / 3600:.3f}..{(ts[-1] - day0) / 3600:.3f}), closure mode "
                       f"{dict(collections.Counter(x[1] for x in v))}, failing share median "
                       f"{statistics.median([x[2] for x in v]):.2f}, kinds {dict(collections.Counter(x[3] for x in v))}")
    systems = sorted({b.split("|")[0] for b in per_band})
    out.append("   distinct disagreeing links per constellation (its bands together): " + "; ".join(
        f"{s} {len(distinct[s])}" + (f" ({', '.join(a + '-' + b for a, b in sorted(distinct[s]))})" if distinct[s] else "")
        for s in systems))
    return out


def selftest():
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    # B's frame differs by +2 at receiver R1 and -1 at satellite S2: no failure.  Then one link of B is off by one.
    a = {("R1", "S1"): (10, "rf"), ("R1", "S2"): (20, "tree"), ("R2", "S1"): (30, "rf"), ("R2", "S2"): (40, "rf")}
    shift = {"R1": 2, "R2": 0, "S1": 0, "S2": -1}
    b = {k: (v[0] - (shift[k[0]] - shift[k[1]]), v[1]) for k, v in a.items()}
    c1, f1, _, _ = walk(a, b)
    check(c1 == 1 and not f1, "a frame difference passes")
    b2 = dict(b)
    b2[("R2", "S2")] = (b[("R2", "S2")][0] + 1, "rf")
    c2, f2, _, l2 = walk(a, b2)
    check(c2 == 1 and len(f2) == 1, "one link off by one fails one check")
    check(sorted(l2[0] + [f2[0][0]]) == sorted(a), "the failing check's loop is the four links")
    # A 4 x 4 grid with one disagreeing link.  On the walk's spanning tree it fails all nine checks, each closing
    # through another link with the disagreeing one on its tree path; off the tree the same disagreement fails one.
    # The loop rule names the one link either way.
    a3 = {(f"R{i}", f"S{j}"): (10 * i + j, "rf") for i in range(4) for j in range(4)}
    for off in (("R0", "S0"), ("R3", "S3")):
        b3 = dict(a3)
        b3[off] = (a3[off][0] - 1, "rf")
        c3, f3, _, l3 = walk(a3, b3)
        if off == ("R0", "S0"):
            check(c3 == 9 and len(f3) == 9, "a disagreeing link on the walk's tree fails nine of nine checks")
            check(all(f[0] != off and off in p for f, p in zip(f3, l3)),
                  "each failing check closes through another link, the disagreeing one on its tree path")
        else:
            check(c3 == 9 and len(f3) == 1 and f3[0][0] == off, "the same disagreement off the tree fails one check")
        bad = loop_failures({k: a3[k][0] - b3[k][0] for k in a3}, 3, 0.5)
        check(set(bad) == {off} and bad[off][0] == 1 and bad[off][1] == 1.0 and bad[off][2] == 9,
              f"the loop rule names {off}, closure +1, failing all nine loops")
    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g3_two_run.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        pb, fails, first = audit("a", "b", {"held", "decided", "would_hold"}, bylink=True)
        check(first == 172800.0, "the first epoch")
        check(pb["E|L1C"]["epochs"] == 2 and pb["E|L1C"]["failing"] == 4, "the fixture's walk fails four checks")
        check(pb["E|L1C"]["kinds"][("tree", "tree")] == 2, "a tree pin wins over a ring-fix row")
        check(set(pb["E|L1C"]["bad"]) == {("AAAA00XXX", "E01")} and pb["E|L1C"]["on_tree"] == 4,
              "the fixture's one disagreeing link carries all four failures on the tree path")
        check(pb["E|L5Q"]["failing"] == 0 and not pb["E|L5Q"]["bad"], "the second band agrees")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("two_run.py")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_a")
    ap.add_argument("run_b")
    ap.add_argument("--states", default="held,decided,would_hold")
    ap.add_argument("--view", choices=("legacy", "bylink"), default="legacy")
    ap.add_argument("--json", default="")
    ap.add_argument("--share", type=float, default=0.5)
    ap.add_argument("--min-loops", type=int, default=3)
    ap.add_argument("--list", type=int, default=20)
    a = ap.parse_args()
    ra, rb = runs.run_dir(a.run_a), runs.run_dir(a.run_b)
    by = a.view == "bylink"
    per_band, failures, first = audit(ra, rb, set(a.states.split(",")), by, a.min_loops, a.share)
    if by:
        out = bylink_report(ra, rb, a.states, per_band, first, a.min_loops, a.share)
        tot_f = sum(len(pb["bad"]) for pb in per_band.values())
    else:
        out, tot_f = legacy_report(ra, rb, a.states, per_band, failures, a.list)
        if a.json:
            legacy_json(a.json, ra, rb, a.states, per_band, failures)
    sys.stdout.write("\n".join(out) + "\n")
    return 0 if tot_f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
