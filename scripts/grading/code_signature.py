"""The code signature of near-null holds (card GRA, plan step G8; bar B13 of card NRF, sections 107.2, 108.3, 112.2
and 112.6): three reads on one reader of the postfit code rows and the ring-fix trace, with no awk program and no cache.

    py -3.14 scripts/grading/code_signature.py links <run> ... --doy-tow0 TOW --combined <list> [--held 0.9]
                                               [--cut 0.30] [--list 25] [--pairs A:B ...] [--with-tree]
                                               [--view legacy|exact]
    py -3.14 scripts/grading/code_signature.py arcs <run> ... [--view legacy|exact]
    py -3.14 scripts/grading/code_signature.py modes <run> [--sats E11,E18,E29] [--hours 0-8] [--audit f.json|f.txt]
                                               [--tracking <stations trace>]
    py -3.14 scripts/grading/code_signature.py --selftest

A link held one near-null step (3, 4) off carries a geometry-free phase error of 3 lambda1 - 4 lambda5 = -0.448 m, which
its free slant ionosphere absorbs; its code rows then show a geometry-free residual r(C1) - r(C5) of about +-0.448 m.
The reader: per (station, satellite, hour from TOW0), the mean postfit residual of the Galileo code rows on band-1 and
band-5 signals (logs/network_pf_rows.csv, kind code, by header name), the share of the link's ring-fix rows that are held
(logs/network_ring_fix.csv) and the tree pins of band 1 (logs/network_chain_edges.csv, a target given).

links  gf_code_signature.py (section 107.2): per link-hour held at least --held with 20 code epochs or more, the deviation
       of r(C1) - r(C5) from the median over the held links of the same satellite, tracking mode (--combined list) and
       hour (groups of four or more); over --cut a suspect; per run the histogram, by hour, by mode and the suspect
       links; --pairs compares two runs link-hour by link-hour.  legacy: the old tool's numbers, from its cache's
       rounding (each mean to 0.0001 m, the difference again to 0.0001 m, the held share to 0.001); its lists of equal
       counts follow the order of awk's arrays and of a set, which no rewrite can follow, so a comparison of those lists
       is by content (card GRA, results of 2026-11-19).  exact: unrounded means and shares, lists in a fixed order.
       --with-tree counts the band-1 tree pins of the link as held rows (section 107.2's "tree links counted").
arcs   arc_signature_spread.py (section 108.3): the same deviations, the tracking mode read from the ring-fix trace's
       first signal (X: combined), hours from the trace's first epoch's hour; consecutive hours of a link form an arc
       of two hours or more; the spread of the arcs' means.  legacy: the old rounding; exact: unrounded.
modes  mode_code_trace.py (section 112.6): the combined mode's code level against the pilot mode's per satellite and
       hour from logs/network_sat_bias_states.csv, the first held link of each satellite, the two-run audit's failing
       links by tracking (one view, the old tool's).
"""
import argparse
import collections
import csv
import json
import os
import re
import statistics
import sys

import runs

HALF = 0.5 * (4 * 0.254828049 - 3 * 0.190293672798365)
C = 299792458.0
F1, F5 = 1575.42e6, 1176.45e6
LW = C / (F1 - F5)


def r4(x):
    return float("%.4f" % x)


def r3(x):
    return float("%.3f" % x)


def code_means(run, t0):
    """{(station, sat, hour): [sum C1, n C1, sum C5, n C5]} of the Galileo code rows of the postfit trace."""
    out = {}
    path = runs.trace_path(run, "network_pf_rows.csv")
    with open(path, encoding="utf-8", errors="replace") as fh:
        ix = {k: i for i, k in enumerate(fh.readline().rstrip("\r\n").split(","))}
        itow, ista, isat, isig, ikind, ires = (ix[k] for k in ("tow", "station", "sat", "signal", "kind", "resid_m"))
        top = max(itow, ista, isat, isig, ikind, ires) + 1
        for ln in fh:
            p = ln.split(",", top)
            if p[isat][:1] != "E" or p[ikind] != "code":
                continue
            s = p[isig]
            band = 1 if s.startswith("C1") else (5 if s.startswith("C5") else 0)
            if not band:
                continue
            k = (p[ista], p[isat], int((float(p[itow]) - t0) / 3600) + 1)
            v = out.setdefault(k, [0.0, 0, 0.0, 0])
            j = 0 if band == 1 else 2
            v[j] += float(p[ires])
            v[j + 1] += 1
    return out


def ring_counts(run, t0):
    """({(station, sat, hour): [held rows, rows]} of the Galileo ring-fix rows, {station: mode from its last band-1
    first signal}, the trace's first TOW)."""
    out, mode, first = {}, {}, None
    header, it = runs.raw_rows(run, "network_ring_fix.csv")
    ix = {k: i for i, k in enumerate(header)}
    itow, ista, isat, is1, ist = (ix[k] for k in ("tow", "station", "sat", "sig1", "state"))
    for r in it:
        if first is None:
            first = float(r[itow])
        if not r[isat].startswith("E"):
            continue
        k = (r[ista], r[isat], int((float(r[itow]) - t0) / 3600) + 1)
        v = out.setdefault(k, [0, 0])
        v[1] += 1
        v[0] += r[ist] == "held"
        if r[is1].startswith("L1"):
            mode[r[ista]] = "X" if r[is1].endswith("X") else "P"
    return out, mode, first


def first_tow(run):
    header, it = runs.raw_rows(run, "network_ring_fix.csv")
    ix = header.index("tow")
    for r in it:
        return float(r[ix])


def tree_pins(run, t0):
    """{(station, sat, hour): pinned band-1 Galileo tree rows} (a target given)."""
    out = collections.Counter()
    header, it = runs.raw_rows(run, "network_chain_edges.csv")
    ix = {k: i for i, k in enumerate(header)}
    for r in it:
        if r[ix["system"]] == "E" and r[ix["band"]] in ("L1C", "L1X") and r[ix["target_cyc"]] != "":
            out[(r[ix["edge_sta"]], r[ix["edge_sat"]], int((float(r[ix["tow"]]) - t0) / 3600) + 1)] += 1
    return out


def link_table(run, t0, legacy, with_tree):
    """{(station, sat, hour): (gf mean m, held share, code epochs)}; legacy: the old cache's rounding."""
    cm = code_means(run, t0)
    rc, _, _ = ring_counts(run, t0)
    pins = tree_pins(run, t0) if with_tree else {}
    out = {}
    for k, (s1, n1, s5, n5) in cm.items():
        if not n1 or not n5:
            continue
        if legacy:
            g = r4(r4(s1 / n1) - r4(s5 / n5))
        else:
            g = s1 / n1 - s5 / n5
        if with_tree:
            c, n = rc.get(k, (0, 0))
            p = pins.get(k, 0)
            f = (c + p) / (n + p) if n + p else 0.0
        else:
            c, n = rc.get(k, (0, 0))
            f = c / n if n else 0.0
        out[k] = (g, r3(f) if legacy else f, min(n1, n5))
    return out


def deviations(tab, held, mode_of):
    groups = collections.defaultdict(list)
    for (s, t, h), (g, f, n) in tab.items():
        if f >= held and n >= 20:
            groups[(t, mode_of(s), h)].append((s, g))
    dev = {}
    for (t, m, h), v in groups.items():
        if len(v) < 4:
            continue
        med = statistics.median(g for _, g in v)
        for s, g in v:
            dev[(s, t, h)] = g - med
    return dev


# ------------------------------------------------------------------------------------------------------------ links
def cmd_links(a):
    xs = set(open(a.combined).read().strip().split(","))
    mode_of = lambda s: "X" if s in xs else "P"  # noqa: E731
    legacy = a.view == "legacy"
    devs = {}
    for tag in a.runs:
        d = deviations(link_table(tag, a.doy_tow0, legacy, a.with_tree), a.held, mode_of)
        if not legacy:
            d = dict(sorted(d.items()))
        devs[tag] = d
        sus = {k: v for k, v in d.items() if abs(v) > a.cut}
        hist = collections.Counter(min(9, int(abs(v) / 0.05)) for v in d.values())
        byh = collections.Counter(k[2] for k in sus)
        bym = collections.Counter(mode_of(k[0]) for k in sus)
        near = sum(1 for v in d.values() if 0.35 <= abs(v) <= 0.60)
        links = collections.Counter((k[0], k[1]) for k in sus)
        ranked = links.most_common(a.list) if legacy else sorted(links.items(), key=lambda kv: (-kv[1], kv[0]))[: a.list]
        print(f"== {tag}: {len(d)} held link-hours graded; deviation over {a.cut} m: {len(sus)} on {len(links)} links "
              f"(0.35 to 0.60 m, the signature's band: {near}); by mode " + ", ".join(f"{m} {c}" for m, c in sorted(bym.items())))
        print("   |deviation| histogram (0.05 m bins, last bin 0.45 m and over): " + " ".join(str(hist[i]) for i in range(10)))
        print("   suspects by hour: " + str([byh[h] for h in range(1, 25)]))
        print("   suspect links (link-hours): " + ", ".join(f"{s}-{t}({mode_of(s)}) {c}" for (s, t), c in ranked))
    for p in a.pairs:
        ta, tb = p.split(":")
        da, db = devs[ta], devs[tb]
        common = sorted(set(da) & set(db))
        c = collections.Counter()
        ex = collections.defaultdict(list)
        for k in common:
            sa, sb = abs(da[k]) > a.cut, abs(db[k]) > a.cut
            cls = {(True, True): "both", (True, False): ta + " only", (False, True): tb + " only",
                   (False, False): "neither"}[(sa, sb)]
            c[cls] += 1
            if cls != "neither" and len(ex[cls]) < 10:
                ex[cls].append((k, round(da[k], 2), round(db[k], 2)))
        print(f"== {ta} against {tb}: {len(common)} common held link-hours; suspect in " + ", ".join(
            f"{k} {v}" for k, v in sorted(c.items())))
        for cls, v in ex.items():
            print(f"   {cls}: " + "; ".join(f"{k[0]}-{k[1]} h{k[2]} {x:+} {y:+}" for k, x, y in v))
        disc = [(k, da[k] - db[k], da[k], db[k]) for k in common if abs(da[k] - db[k]) > 0.25]
        wrong = collections.Counter(ta if abs(x) > abs(y) else tb for _, _, x, y in disc)
        dl = collections.Counter((k[0], k[1]) for k, *_ in disc)
        dh = collections.Counter(k[2] for k, *_ in disc)
        dd = sorted(abs(d) for _, d, _, _ in disc)
        print(f"   the runs differ by more than 0.25 m on {len(disc)} link-hours ({len(dl)} links; |difference| median "
              f"{dd[len(dd) // 2] if dd else float('nan'):.2f} m); the larger deviation in " + ", ".join(
                  f"{k} {v}" for k, v in sorted(wrong.items())))
        print("      by hour: " + str([dh[h] for h in range(1, 25)]))
        ranked = sorted(dl.items(), key=lambda kv: (-kv[1], kv[0]))[: a.list]
        print("      links: " + ", ".join(f"{s}-{t}({mode_of(s)}) {n}" for (s, t), n in ranked))
    return 0


# ------------------------------------------------------------------------------------------------------------- arcs
def cmd_arcs(a):
    legacy = a.view == "legacy"
    for tag in a.runs:
        t0 = int(first_tow(tag) // 3600) * 3600
        rc, mode, _ = ring_counts(tag, t0)
        cm = code_means(tag, t0)
        groups = collections.defaultdict(list)
        for k, (s1, n1, s5, n5) in cm.items():
            if not n1 or not n5:
                continue
            c, n = rc.get(k, (0, 0))
            if n and c / n >= 0.9 and min(n1, n5) >= 20:
                g = (r4(s1 / n1) - r4(s5 / n5)) if legacy else (s1 / n1 - s5 / n5)
                groups[(k[1], mode.get(k[0], "P"), k[2])].append((k[0], g))
        dev = {}
        for (t, m, h), v in groups.items():
            if len(v) < 4:
                continue
            med = statistics.median(g for _, g in v)
            for s, g in v:
                dev[(s, t, h)] = g - med
        by_link = collections.defaultdict(list)
        for (s, t, h), d in dev.items():
            by_link[(s, t)].append((h, d))
        arcs = []
        for v in by_link.values():
            v.sort()
            cur = [v[0]]
            for h, d in v[1:]:
                if h == cur[-1][0] + 1:
                    cur.append((h, d))
                else:
                    if len(cur) >= 2:
                        arcs.append(sum(x for _, x in cur) / len(cur))
                    cur = [(h, d)]
            if len(cur) >= 2:
                arcs.append(sum(x for _, x in cur) / len(cur))
        s = sorted(abs(x) for x in arcs)
        if not s:
            print(f"== {tag}: no arcs")
            continue
        q = lambda p: s[min(len(s) - 1, int(p * len(s)))]  # noqa: E731
        print(f"== {tag}: {len(s)} held link-arcs (from {len(dev)} centred link-hours); |arc mean| median {q(0.5):.3f} m, "
              f"90 % {q(0.9):.3f}, 95 % {q(0.95):.3f}, 99 % {q(0.99):.3f}; beyond 0.15 m {sum(x > 0.15 for x in s)} "
              f"({100.0 * sum(x > 0.15 for x in s) / len(s):.1f} %), at or beyond half a step ({HALF:.3f} m) "
              f"{sum(x >= HALF for x in s)} ({100.0 * sum(x >= HALF for x in s) / len(s):.1f} %)")
    return 0


# ------------------------------------------------------------------------------------------------------------ modes
def cmd_modes(a):
    h0, h1 = (int(x) for x in a.hours.split("-"))
    sats = a.sats.split(",")
    cb = collections.defaultdict(dict)
    t0 = None
    header, it = runs.raw_rows(a.run, "network_sat_bias_states.csv")
    ix = {k: i for i, k in enumerate(header)}
    for r in it:
        if not r[ix["sat"]].startswith("E") or r[ix["signal"]] not in ("C1C", "C1X", "C5Q", "C5X"):
            continue
        tow = float(r[ix["tow"]])
        if t0 is None:
            t0 = tow
        cb[tow][(r[ix["sat"]], r[ix["signal"]])] = (float(r[ix["code_bias_m"]]), float(r[ix["sigma_m"]]))
    first_w = {}
    hourly = collections.defaultdict(lambda: collections.defaultdict(list))
    spread = collections.defaultdict(list)
    for tow in sorted(cb):
        h = int((tow - t0) // 3600)
        rec = cb[tow]
        w = {}
        for sat in {k[0] for k in rec}:
            try:
                d1 = rec[(sat, "C1X")][0] - rec[(sat, "C1C")][0]
                d5 = rec[(sat, "C5X")][0] - rec[(sat, "C5Q")][0]
            except KeyError:
                continue
            w[sat] = ((F1 * d1 + F5 * d5) / (F1 + F5) / LW, rec[(sat, "C1X")][1], rec[(sat, "C5X")][1])
        if len(w) < 5:
            continue
        m = statistics.median(v[0] for v in w.values())
        for sat, (x, s1, s5) in w.items():
            xc = x - m
            first_w.setdefault(sat, xc)
            if sat in sats:
                hourly[sat][h].append((xc, s1, s5))
        if h0 <= h < h1 + 16:
            spread[h].append(statistics.median(abs(v[0] - m) for v in w.values()))
    print(f"{a.run}: the combined mode's code level against the pilot mode's, wide-lane cycles, centred over the satellites")
    print("  satellite: per hour, W (change since its first epoch) [sigma C1X, C5X in m]")
    for sat in sats:
        cells = []
        for h in range(h0, h1 + 1):
            v = hourly[sat].get(h)
            if not v:
                cells.append(f"h{h + 1}: -")
                continue
            x = statistics.median(z[0] for z in v)
            cells.append(f"h{h + 1}: {x:+.2f} ({x - first_w[sat]:+.2f}) [{statistics.median(z[1] for z in v):.2f},"
                         f"{statistics.median(z[2] for z in v):.2f}]")
        print(f"  {sat}  " + "  ".join(cells))
    print("  median |W| over the satellites by hour: "
          + " ".join(f"h{h + 1} {statistics.median(spread[h]):.3f}" for h in sorted(spread)))
    track = {}
    with open(a.tracking, newline="") as f:
        for r in csv.DictReader(f):
            track[r["station"]] = "combined" if ">" in r.get("phase_tracking", "") else "pilot"
    first = {}
    for r in runs.rows(a.run, "network_ring_fix.csv"):
        if r["state"] != "held" or not r["sat"].startswith("E") or r["sat"] in first:
            continue
        first[r["sat"]] = (float(r["tow"]), r["station"])
    print("  first held link per satellite (hour, station, tracking):")
    by_mode = collections.Counter()
    for sat in sorted(first):
        tow, sta = first[sat]
        by_mode[track.get(sta, "?")] += 1
        mark = "  <" if sat in sats else ""
        print(f"    {sat} h{int((tow - t0) // 3600) + 1} {sta} {track.get(sta, '?')}{mark}")
    print("  first held at:", dict(by_mode))
    if a.audit:
        fails, fail_h = collections.Counter(), collections.Counter()
        if a.audit.endswith(".json"):
            for x in json.load(open(a.audit, encoding="utf-8"))["failures"]:
                if x["band"] == "E|L1C":
                    fails[(x["station"], x["sat"])] += 1
                    fail_h[int((x["tow"] - t0) // 3600) + 1] += 1
        else:
            for line in open(a.audit, encoding="utf-8", errors="replace"):
                m = re.match(r"\s+FAIL tow (\d+) E\|L1C (\S+) (E\d\d):", line)
                if m:
                    fails[(m.group(2), m.group(3))] += 1
        if fail_h:
            print("  failing E1 checks by hour: " + " ".join(f"h{h}:{n}" for h, n in sorted(fail_h.items())))
        print("  the two-run audit's failing Galileo links (E1 checks): station (tracking), satellite (first held at)")
        agg = collections.Counter()
        for (sta, sat), n in fails.most_common():
            fm = track.get(first[sat][1], "?") if sat in first else "?"
            agg[(track.get(sta, "?"), fm)] += n
            print(f"    {sta} ({track.get(sta, '?')}) {sat} (first held at a {fm} receiver): {n}")
        print("  failing checks by (link tracking, satellite first held at):", dict(agg))
    return 0


# ---------------------------------------------------------------------------------------------------------- selftest
def selftest():
    import io
    import contextlib
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g8_signature.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        tab = link_table("sig", 0.0, False, False)
        check(len(tab) == 5 and abs(tab[("S5", "E01", 1)][0] - 0.45) < 1e-9, "five link-hours, the fifth 0.45 m off")
        check(tab[("S2", "E01", 1)][1:] == (1.0, 20) and tab[("S1", "E01", 1)][1] == 0.0, "held shares and code epochs")
        d = deviations(tab, 0.9, lambda s: "P")
        check(abs(d[("S5", "E01", 1)] - 0.45) < 1e-9 and abs(d[("S2", "E01", 1)]) < 1e-9 and ("S1", "E01", 1) not in d,
              "the deviation from the median of the held links; a link not held left out")
        tl = link_table("sig", 0.0, True, False)
        check(tl[("S5", "E01", 1)][0] == 0.45, "the legacy rounding to 0.0001 m")
        tt = link_table("sig", 0.0, False, True)
        check(tt[("S1", "E01", 1)][1] == 0.5, "the tree pins counted as held rows with --with-tree")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            cmd_links(build_parser().parse_args(["links", "sig", "--doy-tow0", "0", "--combined",
                                                 os.path.join(root, "x.txt"), "--view", "exact"]))
        out = buf.getvalue()
        check("4 held link-hours graded; deviation over 0.3 m: 1 on 1 links" in out and "S5-E01(P) 1" in out,
              "the links view finds the one suspect")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("code_signature.py")
    return 0 if ok else 1


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    li = sub.add_parser("links")
    li.add_argument("runs", nargs="+")
    li.add_argument("--doy-tow0", type=float, required=True)
    li.add_argument("--combined", required=True)
    li.add_argument("--held", type=float, default=0.9)
    li.add_argument("--cut", type=float, default=0.30)
    li.add_argument("--list", type=int, default=25)
    li.add_argument("--pairs", nargs="*", default=[])
    li.add_argument("--with-tree", action="store_true")
    li.add_argument("--view", choices=("legacy", "exact"), default="legacy")
    ar = sub.add_parser("arcs")
    ar.add_argument("runs", nargs="+")
    ar.add_argument("--view", choices=("legacy", "exact"), default="legacy")
    mo = sub.add_parser("modes")
    mo.add_argument("run")
    mo.add_argument("--sats", default="E11,E18,E29")
    mo.add_argument("--hours", default="0-8")
    mo.add_argument("--audit", default=None)
    mo.add_argument("--tracking", default=os.path.join(runs.runs_root(), "_rap_n43_smoke", "logs", "network_stations.csv"))
    return ap


def main():
    if "--selftest" in sys.argv:
        return selftest()
    a = build_parser().parse_args()
    return {"links": cmd_links, "arcs": cmd_arcs, "modes": cmd_modes}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
