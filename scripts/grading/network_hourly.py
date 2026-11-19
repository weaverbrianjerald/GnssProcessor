"""The hourly network bars of card NRF: held share by family and receiver mode, the GPS held share, RAIM rows, epochs and
lost satellite-samples (card GRA, plan step G2; bars B1, B2, B3 and B8).

    py -3.14 scripts/grading/network_hourly.py table <run> --xfile <list> [--hours 24] [--view legacy|exact|d24]
    py -3.14 scripts/grading/network_hourly.py compare <arm> <control> --xfile <list> [--hours 24] [--first 3]
                                               [--last 24] [--view legacy|exact]
    py -3.14 scripts/grading/network_hourly.py families <run> [<run> ...] [--hours 6]
    py -3.14 scripts/grading/network_hourly.py --selftest

A run is a bare tag, a "_rap_" folder name or a path (runs.run_dir).  --xfile names the day's combined-mode receivers
(comma-separated); every other station counts as pilot-mode.

The rules (one owner each):
  held share   rows of logs/network_ring_fix.csv in state "held" over all rows (held, decided, refused), per hour
               from the first E record of products.txt; GPS is the L1C/L2W family; Galileo is every Galileo row,
               split by the receiver's mode; the exact view adds one row per family found in the trace
  RAIM rows    rows of logs/network_raim.csv per hour; the exact view splits them by constellation
  epochs       logs/network_epoch_status.csv by disposition
  lost         every 30 min from the end of hour 1 (46 samples in 24 h), the Galileo satellites of each band-1 tree
               (logs/network_chain_edges.csv, edge_sat of the bands L1C and L1X) whose P record does not carry
               signal_integer 1: on band 1 alone (the rule of single_hourly.py and section 96) and on both signals of
               the band's family (section 96.1's rule, settled_graph.py)

The views:
  legacy  byte for byte the output of the scripts it replaces: table = single_hourly.py, compare = c4_compare.py as of
          section 118.1 (the rounded differences), families = held_by_family.py, d24 = the held, RAIM, epoch and
          withheld rows of d24_hourly.py
  exact   the same counts unrounded (two decimals), the RAIM rows by constellation, the lost samples by both rules;
          compare takes the worst hour and the mean from the unrounded shares (card GRA, defect D1)
Legacy quirks kept in the legacy view only: the last epoch of the stream is not read for the lost samples (it is read
when the next E record appears, and none follows); families counts hours from the first ring-fix row's exact time.
"""
import argparse
import ast
import collections
import re
import sys

import runs

GALILEO_ROWS = ("Galileo pilot-mode receivers", "Galileo combined-mode receivers", "Galileo all", "GPS")
D24_FAMILIES = (("G:L1C/L2W", "G", "L1C", "L2W"), ("E:L1X/L5X", "E", "L1X", "L5X"), ("E:L1C/L5Q", "E", "L1C", "L5Q"))
BAND1 = ("L1C", "L1X")


class Hourly:
    """Every count of one run the views print; built in one pass over each file."""

    def __init__(self, run, xs, hours):
        self.run, self.hours = run, hours
        self.t0 = runs.first_epoch(run)
        self.tot, self.held = collections.Counter(), collections.Counter()   # (row, hour)
        self.second = {}                                                      # band-1 signal -> second signal
        self._ring_fix(xs)
        self.tree = self._trees()
        self.lost_band1, self.lost_both = collections.Counter(), collections.Counter()
        self.last_band1, self.last_both = collections.Counter(), collections.Counter()   # the stream's last epoch
        self._lost()
        self.raim, self.raim_sys = collections.Counter(), collections.Counter()
        for r in runs.rows(run, "network_raim.csv"):
            h = runs.hour_of(runs.tow_key(r["tow"]), self.t0)
            self.raim[h] += 1
            self.raim_sys[(r["sat"][:1], h)] += 1
        self.status = collections.Counter()                                   # legacy: third column, file order
        self.epochs, self.withheld = collections.Counter(), collections.Counter()
        header, it = runs.raw_rows(run, "network_epoch_status.csv")
        for r in it:
            if not r:
                continue
            self.status[r[2]] += 1
            h = runs.hour_of(runs.tow_key(r[1]), self.t0)
            self.epochs[h] += 1
            self.withheld[h] += r[2] != "emitted"

    def _ring_fix(self, xs):
        for r in runs.rows(self.run, "network_ring_fix.csv"):
            h = runs.hour_of(runs.tow_key(r["tow"]), self.t0)
            sysl = r["sat"][:1]
            groups = ["%s:%s/%s" % (sysl, r["sig1"], r["sig2"])]
            if sysl == "G" and (r["sig1"], r["sig2"]) == ("L1C", "L2W"):
                groups.append("GPS")
            elif sysl == "E":
                groups.append("Galileo combined-mode receivers" if r["station"] in xs else "Galileo pilot-mode receivers")
                groups.append("Galileo all")
                self.second.setdefault(r["sig1"], r["sig2"])
            for g in groups:
                self.tot[(g, h)] += 1
                self.held[(g, h)] += r["state"] == "held"

    def samples(self):
        return {self.t0 + 1800 * k for k in range(2, 2 * self.hours)}

    def _trees(self):
        want = self.samples()
        tree = collections.defaultdict(lambda: collections.defaultdict(set))
        for r in runs.rows(self.run, "network_chain_edges.csv"):
            t = runs.tow_key(r["tow"])
            if t in want and r["system"] == "E" and r["band"] in BAND1:
                tree[t][r["band"]].add(r["edge_sat"])
        return tree

    def _lost(self):
        want = self.samples()
        epochs = runs.product_epochs(self.run)
        prev = None
        for cur in epochs:
            if prev is not None:
                self._count_lost(prev, want, self.lost_band1, self.lost_both)
            prev = cur
        if prev is not None:                       # the last epoch: read by the exact view only
            self._count_lost(prev, want, self.last_band1, self.last_both)

    def lost_exact(self, rule):
        """Lost samples of every sample epoch, the stream's last included; rule "band1" or "both"."""
        base, last = (self.lost_band1, self.last_band1) if rule == "band1" else (self.lost_both, self.last_both)
        return {b: base[b] + last[b] for b in sorted(base.keys() | last.keys())}

    def _count_lost(self, epoch, want, band1, both):
        tow, recs = epoch
        if tow not in want:
            return
        flags = {(q[1], q[2]): q[4] == "1" for q in recs if q[0] == "P" and q[1].startswith("E") and len(q) > 4}
        for band, sats in self.tree[tow].items():
            band1[band] += sum(1 for s in sats if not flags.get((s, band), False))
            s2 = self.second.get(band)
            both[band] += sum(1 for s in sats if not (flags.get((s, band), False) and flags.get((s, s2), False)))

    def share(self, row, h):
        n = self.tot[(row, h)]
        return 100.0 * self.held[(row, h)] / n if n else None

    def families(self):
        return sorted({g for g, _ in self.tot if ":" in g})


def legacy_table(H, name):
    out = [f"== {name}: held share by hour (%), hours 1 to {H.hours}"]
    for g in GALILEO_ROWS:
        out.append(f"   {g:32s} " + " ".join(f"{H.share(g, h):.0f}" if H.tot[(g, h)] else "." for h in range(1, H.hours + 1)))
    out.append("   lost satellite-samples (band-1 tree satellites not flagged, every 30 min from hour 1): "
               + ", ".join(f"{b} {n}" for b, n in sorted(H.lost_band1.items())))
    out.append(f"   epochs by disposition {dict(H.status)}; RAIM rows by hour {[H.raim[h] for h in range(1, H.hours + 1)]}")
    return "\n".join(out) + "\n"


def exact_table(H, name):
    hs = range(1, H.hours + 1)
    out = [f"== {name}: held share by hour (%, exact), hours 1 to {H.hours} from TOW {H.t0}"]
    for g in GALILEO_ROWS + tuple(H.families()):
        out.append(f"   {g:32s} " + " ".join(f"{H.share(g, h):.2f}" if H.tot[(g, h)] else "." for h in hs))
    out.append("   lost satellite-samples, band 1 flagged: " + ", ".join(f"{b} {n}" for b, n in H.lost_exact("band1").items())
               + "; both signals flagged (section 96.1): " + ", ".join(f"{b} {n}" for b, n in H.lost_exact("both").items()))
    out.append(f"   epochs by disposition {dict(H.status)}")
    out.append(f"   RAIM rows by hour {[H.raim[h] for h in hs]} (total {sum(H.raim[h] for h in hs)})")
    for s in sorted({s for s, _ in H.raim_sys}):
        out.append(f"   RAIM rows {s} by hour {[H.raim_sys[(s, h)] for h in hs]} (total {sum(H.raim_sys[(s, h)] for h in hs)})")
    return "\n".join(out) + "\n"


def d24_rows(H, name):
    hs = range(1, H.hours + 1)
    out = ["== %s by hour (hour 1 from TOW %d)" % (name, H.t0)]
    for f, *_ in D24_FAMILIES:
        out.append("   %-10s held %%    %s" % (f, " ".join("%.0f" % H.share(f, h) if H.tot[(f, h)] else "-" for h in hs)))
    out.append("   RAIM rows    %s" % " ".join(str(H.raim[h]) for h in hs))
    out.append("   epochs       %s" % " ".join(str(H.epochs[h]) for h in hs))
    out.append("   withheld     %s" % " ".join(str(H.withheld[h]) for h in hs))
    return "\n".join(out) + "\n"


def _read_legacy(text):
    """c4_compare.py's reader of a single_hourly.py table (section 118.1's version), kept for the legacy view."""
    out = {"name": "?", "rows": {}, "lost": {}, "raim": [], "epochs": {}}
    for ln in text.splitlines(True):
        m = re.match(r"== (\S+):", ln)
        if m:
            out["name"] = m.group(1)
        m = re.match(r"\s+(Galileo pilot-mode receivers|Galileo combined-mode receivers|Galileo all|GPS)\s+([\d ]+)$", ln)
        if m:
            out["rows"][m.group(1)] = [int(x) for x in m.group(2).split()]
        if "lost satellite-samples" in ln:
            for fam, n in re.findall(r"(L\w+) (\d+)", ln.split("):", 1)[1]):
                out["lost"][fam] = int(n)
        if "RAIM rows by hour" in ln:
            out["raim"] = ast.literal_eval(ln.split("RAIM rows by hour", 1)[1].strip())
            out["epochs"] = ast.literal_eval(ln.split("disposition", 1)[1].split(";")[0].strip())
    return out


def legacy_compare(text_a, text_b, first, last):
    A, B = _read_legacy(text_a), _read_legacy(text_b)
    hs = range(first, last + 1)
    out = [f"== {A['name']} against {B['name']} (hours {first} to {last})"]
    for row in ("GPS", "Galileo all", "Galileo pilot-mode receivers", "Galileo combined-mode receivers"):
        x, y = A["rows"].get(row), B["rows"].get(row)
        if not x or not y:
            out.append(f"   {row}: missing")
            continue
        d = [(x[h - 1] - y[h - 1], h) for h in hs]
        worst = min(d)
        mean = sum(v for v, _ in d) / len(d)
        out.append(f"   {row}: worst {worst[0]:+d} points (hour {worst[1]}: {x[worst[1] - 1]} against {y[worst[1] - 1]} %), "
                   f"mean {mean:+.2f} ({sum(x[h - 1] for h in hs) / len(d):.2f} against {sum(y[h - 1] for h in hs) / len(d):.2f} %)")
    diff = [(h + 1, x, y) for h, (x, y) in enumerate(zip(A["raim"], B["raim"])) if x != y]
    out.append(f"   RAIM rows: {sum(A['raim'])} against {sum(B['raim'])}; hours that differ: "
               + (", ".join(f"{h}: {x} against {y}" for h, x, y in diff) or "none"))
    out.append(f"   epochs: {A['epochs']} against {B['epochs']}")
    la, lb = sum(A["lost"].values()), sum(B["lost"].values())
    out.append(f"   lost satellite-samples: {la} ({A['lost']}) against {lb} ({B['lost']}); ratio {la / lb if lb else float('nan'):.3f}")
    return "\n".join(out) + "\n"


def exact_compare(A, B, na, nb, first, last):
    hs = range(first, last + 1)
    out = [f"== {na} against {nb} (hours {first} to {last}, exact shares)"]
    for row in ("GPS", "Galileo all", "Galileo pilot-mode receivers", "Galileo combined-mode receivers"):
        d = [(A.share(row, h) - B.share(row, h), h) for h in hs if A.tot[(row, h)] and B.tot[(row, h)]]
        if not d:
            out.append(f"   {row}: missing")
            continue
        v, h = min(d)
        mean = sum(x for x, _ in d) / len(d)
        ma = sum(A.share(row, k) for _, k in d) / len(d)
        mb = sum(B.share(row, k) for _, k in d) / len(d)
        out.append(f"   {row}: worst {v:+.2f} points (hour {h}: {A.share(row, h):.2f} against {B.share(row, h):.2f} %), "
                   f"mean {mean:+.2f} ({ma:.2f} against {mb:.2f} %)")
    ra, rb = [A.raim[h] for h in range(1, A.hours + 1)], [B.raim[h] for h in range(1, B.hours + 1)]
    diff = [(h + 1, x, y) for h, (x, y) in enumerate(zip(ra, rb)) if x != y]
    out.append(f"   RAIM rows: {sum(ra)} against {sum(rb)}; hours that differ: "
               + (", ".join(f"{h}: {x} against {y}" for h, x, y in diff) or "none"))
    for s in sorted({s for s, _ in A.raim_sys} | {s for s, _ in B.raim_sys}):
        out.append(f"   RAIM rows {s}: {sum(A.raim_sys[(s, h)] for h in range(1, A.hours + 1))} against "
                   f"{sum(B.raim_sys[(s, h)] for h in range(1, B.hours + 1))}")
    out.append(f"   epochs: {dict(A.status)} against {dict(B.status)}")
    for label, rule in (("band 1 flagged", "band1"), ("both signals flagged", "both")):
        la, lb = A.lost_exact(rule), B.lost_exact(rule)
        sa, sb = sum(la.values()), sum(lb.values())
        out.append(f"   lost satellite-samples, {label}: {sa} ({la}) against {sb} ({lb}); "
                   f"ratio {sa / sb if sb else float('nan'):.3f}")
    return "\n".join(out) + "\n"


def legacy_families(names, hours):
    """held_by_family.py: held rows per family and hour from the first ring-fix row's exact time."""
    def count(run):
        by = collections.defaultdict(lambda: [0] * hours)
        t0 = None
        for r in runs.rows(run, "network_ring_fix.csv"):
            tow = float(r["tow"])
            if t0 is None:
                t0 = tow
            if r["state"] != "held":
                continue
            h = int((tow - t0) // 3600)
            if 0 <= h < hours:
                by[f"{r['sat'][0]}:{r['sig1']}/{r['sig2']}"][h] += 1
        return by
    counted = {t: count(t) for t in names}
    out = []
    for fam in sorted({f for b in counted.values() for f in b}):
        out.append(f"{fam}: held link-epochs by hour, total, ratio to {names[0]}")
        base = sum(counted[names[0]].get(fam, [0])) or 1
        for t in names:
            v = counted[t].get(fam, [0] * hours)
            out.append(f"  {t:14s} " + " ".join(f"{x:7d}" for x in v) + f" | {sum(v):8d}  {sum(v) / base:.4f}")
    return "\n".join(out) + "\n"


def selftest():
    import os
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g2_network.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        xs = runs.name_list(os.path.join(root, "x.txt"))
        A, B = Hourly("arm", xs, 2), Hourly("ctl", xs, 2)
        check(A.t0 == 172800, "first epoch of a time that is not a whole second")
        check(A.held[("GPS", 1)] == 2 and A.tot[("GPS", 1)] == 3, "GPS counts held rows over all rows (decided not held)")
        check(A.tot[("Galileo combined-mode receivers", 1)] == 2 and A.tot[("Galileo pilot-mode receivers", 1)] == 1,
              "Galileo rows split by the list")
        check(A.tot[("E:L7Q/L5Q", 1)] == 1 and A.tot[("Galileo all", 1)] == 3, "a third-band family has its own row")
        check(A.raim_sys[("E", 2)] == 1 and A.raim_sys[("G", 2)] == 1 and A.raim[2] == 2, "RAIM rows by constellation")
        # the sample at 176400: E05 flagged on L1C only, E09 not at all, E04 on both -> band 1: 1, both: 2
        check(A.lost_band1["L1C"] == 1 and A.lost_both["L1C"] == 2, "lost samples by both rules")
        check(legacy_table(A, "arm").splitlines()[4].endswith("67 50"), "the legacy GPS row rounds to whole percent")
        # GPS hour 1: 2 of 3 held against 5 of 6; the rounded tables differ by 16 points, the shares by 16.67
        e = exact_compare(A, B, "arm", "ctl", 1, 2)
        check("GPS: worst -16.67 points (hour 1: 66.67 against 83.33 %)" in e, "exact compare from the shares")
        lc = legacy_compare(legacy_table(A, "arm"), legacy_table(B, "ctl"), 1, 2)
        check("GPS: worst -16 points (hour 1: 67 against 83 %)" in lc, "legacy compare from the rounded tables")
        check("RAIM rows E: 1 against 0" in e, "exact compare RAIM rows by constellation")
        f = legacy_families(["arm", "ctl"], 2)
        check("G:L1C/L2W: held link-epochs by hour, total, ratio to arm" in f, "families header")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("network_hourly.py")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("table")
    t.add_argument("run")
    t.add_argument("--xfile", required=True)
    t.add_argument("--hours", type=int, default=24)
    t.add_argument("--view", choices=("legacy", "exact", "d24"), default="legacy")
    c = sub.add_parser("compare")
    c.add_argument("arm")
    c.add_argument("ctl")
    c.add_argument("--xfile", required=True)
    c.add_argument("--hours", type=int, default=24)
    c.add_argument("--first", type=int, default=3)
    c.add_argument("--last", type=int, default=24)
    c.add_argument("--view", choices=("legacy", "exact"), default="exact")
    f = sub.add_parser("families")
    f.add_argument("runs", nargs="+")
    f.add_argument("--hours", type=int, default=6)
    a = ap.parse_args()
    if a.cmd == "families":
        sys.stdout.write(legacy_families(a.runs, a.hours))
        return 0
    xs = runs.name_list(a.xfile)
    if a.cmd == "table":
        H = Hourly(a.run, xs, a.hours)
        sys.stdout.write({"legacy": legacy_table, "exact": exact_table, "d24": d24_rows}[a.view](H, a.run))
        return 0
    A, B = Hourly(a.arm, xs, a.hours), Hourly(a.ctl, xs, a.hours)
    if a.view == "legacy":
        sys.stdout.write(legacy_compare(legacy_table(A, a.arm), legacy_table(B, a.ctl), a.first, a.last))
    else:
        sys.stdout.write(exact_compare(A, B, a.arm, a.ctl, a.first, a.last))
    return 0


if __name__ == "__main__":
    sys.exit(main())
