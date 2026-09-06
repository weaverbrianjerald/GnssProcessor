#!/usr/bin/env python3
"""TRUTH-FREE reference re-referencing of the DD fractional histogram (SPEC_amb_iono_split_overcount §6.13).

The companion `simnet_dd_reref.py` grades the sim against `truth_ambiguities.csv`.  The FIELD has no truth, so
the only observable is the trace's own `frac_cyc` = dd_cyc - round(dd_cyc), which lives on the unit circle.
That is enough: the producer forms every chord of a (constellation, signal) against ONE reference satellite
(filter/NetworkDdFrac.cpp: the lexically-lowest satellite present at >= 2 stations), so within one
(epoch, cons, signal, sta_a, sta_b) group

    f_i = frac( s_i - s_ref )      s = between-station SD ambiguity error of a satellite

and -s_ref is COMMON to the whole group.  Remove it circularly and what remains is per-satellite.

⚠ THE NULL IS THE WHOLE POINT.  Subtracting a group's OWN circular median from its members shrinks |residual|
even when the fractions are independent uniform noise — a group of 5 has 5 chances to sit near its own median.
Every ref-free number below is therefore printed beside a size-matched Monte-Carlo null (`--null-draws`), and
the verdict is the GAP, never the raw value.  Three independent readouts, each with its own null:

  as-traced   med|f_i|, share |f| <= 0.1              what every field DD grade has reported
  resultant   R = |mean exp(2*pi*i*f)| per group      concentration; null E[R] ~ 0.886/sqrt(n)
  ref-free    med|u_i|, u = wrap(f_i - c_hat)         c_hat = circular median of the group
  closure     wrap(c_ab + c_bc + c_ca) over triangles the STRUCTURE test (see below)

CLOSURE is a SECONDARY diagnostic and its null is subtle.  A DD is per-station additive by construction, so a
triangle closes for any FIXED satellite whether or not the reference matters; since c_hat is a circular median
it IS one of those data points, and a triangle closes whenever the median lands on the same satellite in all
three pairs.  Its null therefore draws the DD structure WITHOUT a reference term (iid s per satellite-station,
same group sizes, same estimator) — observed AT that null means the closure is the median-agreement artifact.
The verdict rests on R and on the ref-free GAP, both of which are free of this confound.

Also reported: the reference-satellite census per hour (the field's reference ROTATES as satellites rise and
set, unlike the sim's all-day G01/E02) and the as-traced grade split by whether the reference switched in the
preceding K epochs.

Reads:  <run>/logs/network_dd_fractional.csv only.  No fixture, no truth, no products.
Usage:  python scripts/dd_frac_reref.py <run_dir> [--signal L1C] [--stride 4] [--cons G]
        python scripts/dd_frac_reref.py --selftest
"""
import argparse
import collections
import csv
import io
import math
import os
import random
import statistics as st
import sys
import tempfile

NEAR = 0.1          # "near an integer" band, cycles (share |.| <= NEAR)
MIN_GROUP = 3       # a group needs >= 3 satellites before a circular median means anything


def wrap(x):
    """Map a cycle count to (-0.5, +0.5]."""
    return x - round(x)


def circ_median(vals):
    """The value minimising sum_i |wrap(f_i - c)|.  Attained at a data point; O(n^2), n = group size."""
    best, bestcost = vals[0], None
    for c in vals:
        cost = 0.0
        for v in vals:
            cost += abs(wrap(v - c))
        if bestcost is None or cost < bestcost:
            best, bestcost = c, cost
    return best


def resultant(vals):
    """|mean exp(2*pi*i*f)| — 1 = perfectly concentrated, ~0.886/sqrt(n) under uniform noise."""
    c = sum(math.cos(2 * math.pi * v) for v in vals) / len(vals)
    s = sum(math.sin(2 * math.pi * v) for v in vals) / len(vals)
    return math.hypot(c, s)


def med(x):
    return st.median(abs(v) for v in x) if x else float("nan")


def near(x):
    return 100.0 * sum(1 for v in x if abs(v) <= NEAR) / len(x) if x else float("nan")


class Null:
    """Size-matched Monte-Carlo null: uniform fractions, same group sizes, same estimator."""

    def __init__(self, draws=400, seed=20260906):
        self.draws = draws
        self.rng = random.Random(seed)
        self.cache = {}

    def of(self, n):
        """(E med|u|, E R, E med|f|) for a uniform group of size n."""
        if n in self.cache:
            return self.cache[n]
        u, r, f = [], [], []
        for _ in range(self.draws):
            v = [self.rng.random() - 0.5 for _ in range(n)]
            c = circ_median(v)
            u.extend(wrap(x - c) for x in v)
            f.extend(v)
            r.append(resultant(v))
        out = (med(u), st.mean(r), med(f))
        self.cache[n] = out
        return out

    def closure(self, triangles):
        """The DD-STRUCTURE null for the triangle closure.

        ⚠ An independent-uniform null is WRONG here.  A DD is per-station additive BY CONSTRUCTION —
        f_i(a,b) = [s_i(a) - s_i(b)] - [s_ref(a) - s_ref(b)] — so for a FIXED satellite the triangle
        closes exactly, with or without a reference effect.  c_hat is a circular median, i.e. one of
        those data points, so a triangle closes whenever the median lands on the SAME satellite in all
        three pairs.  That confound has nothing to do with the reference, and a null that ignores it
        makes any trace look structured.

        So this null draws the alternative with the DD structure and NO reference contribution: iid
        uniform s per (satellite, station) over three stations, groups built from the same satellite
        pool with the observed sizes, c_hat by the same circular median.  Observed closure at this null
        = the closure is the median-agreement artifact; observed BELOW it = structure the DD
        construction alone does not explain."""
        out = []
        for (n1, n2, n3) in triangles:
            npool = max(n1, n2, n3)
            sa = [self.rng.random() - 0.5 for _ in range(npool)]
            sb = [self.rng.random() - 0.5 for _ in range(npool)]
            sc = [self.rng.random() - 0.5 for _ in range(npool)]
            tot = 0.0
            for (n, x, y) in ((n1, sa, sb), (n2, sb, sc), (n3, sc, sa)):
                tot += circ_median([wrap(x[i] - y[i]) for i in range(n)])
            out.append(wrap(tot))
        return med(out)


def regrade(run, sig="L1C", cons_filter=None, stride=1, null_draws=400, switch_k=20, out=sys.stdout):
    """Stream the trace and report the as-traced / ref-free / closure grades against their nulls."""
    path = os.path.join(run, "logs", "network_dd_fractional.csv")
    f = io.open(path, newline="", encoding="utf-8", errors="replace")
    rd = csv.reader(f)
    hdr = next(rd)
    ix = {c: hdr.index(c) for c in ("tow", "cons", "signal", "sat_i", "ref_sat",
                                    "sta_a", "sta_b", "dd_cyc", "frac_cyc")}
    nul = Null(draws=null_draws)
    tow0 = None
    last = None
    k = -1
    group = collections.defaultdict(dict)          # (cons, a, b) -> {sat: f}
    H = collections.defaultdict(lambda: {"f": [], "u": [], "R": [], "nf": [], "nu": [], "nR": [],
                                         "ref": collections.Counter(), "n": []})
    CL = collections.defaultdict(lambda: {"c": [], "sizes": []})   # cons -> triangle closures
    refseen = {}                                   # cons -> (ref, epoch index of the last switch)
    switches = collections.Counter()
    byswitch = {"fresh": [], "settled": []}        # as-traced |f| split on reference age
    nrows = 0

    def flush(hour, kk):
        # per-group grades
        cpair = collections.defaultdict(dict)      # cons -> (a,b) -> c_hat
        for (cons, a, b), d in group.items():
            if len(d) < MIN_GROUP:
                continue
            vals = list(d.values())
            n = len(vals)
            c = circ_median(vals)
            cpair[cons][(a, b)] = (c, n)
            h = H[(hour, cons)]
            nu, nR, nf = nul.of(n)
            h["n"].append(n)
            h["nf"].append(nf)
            h["nu"].append(nu)
            h["nR"].append(nR)
            h["R"].append(resultant(vals))
            for v in vals:
                h["f"].append(v)
                h["u"].append(wrap(v - c))
        # triangle closure: c_ab + c_bc + c_ca == 0 (mod 1) iff c is a per-STATION difference
        for cons, pm in cpair.items():
            adj = collections.defaultdict(dict)
            for (a, b), (c, n) in pm.items():
                adj[a][b] = (c, n)
                adj[b][a] = (-c, n)
            done = set()
            for a in adj:
                for b in adj[a]:
                    for c_ in adj.get(b, {}):
                        if c_ == a or c_ not in adj.get(a, {}):
                            continue
                        tri = tuple(sorted((a, b, c_)))
                        if tri in done:
                            continue
                        done.add(tri)
                        cab, n1 = adj[a][b]
                        cbc, n2 = adj[b][c_]
                        cca, n3 = adj[c_][a]
                        CL[cons]["c"].append(wrap(cab + cbc + cca))
                        CL[cons]["sizes"].append((n1, n2, n3))
                        if len(CL[cons]["c"]) >= 20000:
                            break

    for row in rd:
        tow = float(row[ix["tow"]])
        if tow != last:
            if last is not None and k % stride == 0:
                flush(int((last - tow0) // 3600), k)
            if last is not None:
                group.clear()
            k += 1
            last = tow
            if tow0 is None:
                tow0 = tow
        if row[ix["signal"]] != sig:
            continue
        cons = row[ix["cons"]]
        if cons_filter and cons != cons_filter:
            continue
        nrows += 1
        ref = row[ix["ref_sat"]]
        hour = int((tow - tow0) // 3600)
        H[(hour, cons)]["ref"][ref] += 1
        prev = refseen.get(cons)
        if prev is None or prev[0] != ref:
            if prev is not None:
                switches[cons] += 1
            refseen[cons] = (ref, k)
        fr = float(row[ix["frac_cyc"]])
        if fr == 0.0 and float(row[ix["dd_cyc"]]) == 0.0:
            continue                                # lambda unknown -> both columns zeroed
        byswitch["fresh" if (k - refseen[cons][1]) < switch_k else "settled"].append(fr)
        if k % stride == 0:
            group[(cons, row[ix["sta_a"]], row[ix["sta_b"]])][row[ix["sat_i"]]] = fr
    if last is not None and k % stride == 0:
        flush(int((last - tow0) // 3600), k)
    f.close()

    def P(*a):
        print(*a, file=out)

    P(f"run {run}")
    P(f"signal {sig}{'  cons ' + cons_filter if cons_filter else ''}, stride {stride}, "
      f"groups need >= {MIN_GROUP} sats, near-band |.| <= {NEAR} cyc, null {null_draws} draws")
    P("")
    P(f"{'h':>2} {'cn':>2} {'n':>7} {'grp':>5} | {'as-traced':>9} {'null':>6} {'near%':>6} | "
      f"{'R':>5} {'null':>5} | {'ref-free':>8} {'null':>6} {'gap':>6} {'near%':>6} | ref (rows)")
    tot = collections.defaultdict(lambda: {"f": [], "u": [], "R": [], "nf": [], "nu": [], "nR": []})
    for (h, cons) in sorted(H):
        d = H[(h, cons)]
        if not d["f"]:
            continue
        for key in ("f", "u", "R", "nf", "nu", "nR"):
            tot[cons][key].extend(d[key])
        refs = " ".join(f"{s}:{n}" for s, n in d["ref"].most_common(2))
        gap = med(d["u"]) - st.mean(d["nu"])
        P(f"{h:2d} {cons:>2} {len(d['f']):7d} {len(d['R']):5d} | {med(d['f']):9.3f} "
          f"{st.mean(d['nf']):6.3f} {near(d['f']):6.1f} | {st.mean(d['R']):5.3f} {st.mean(d['nR']):5.3f} | "
          f"{med(d['u']):8.3f} {st.mean(d['nu']):6.3f} {gap:+6.3f} {near(d['u']):6.1f} | {refs}")
    P("")
    P("FULL RUN")
    for cons in sorted(tot):
        d = tot[cons]
        P(f"  {cons}: as-traced med|f| {med(d['f']):.3f} (null {st.mean(d['nf']):.3f}), near {near(d['f']):.1f} % | "
          f"R {st.mean(d['R']):.3f} (null {st.mean(d['nR']):.3f}) | "
          f"ref-free med|u| {med(d['u']):.3f} (null {st.mean(d['nu']):.3f}, "
          f"gap {med(d['u']) - st.mean(d['nu']):+.3f}), near {near(d['u']):.1f} %")
    P("")
    P("TRIANGLE CLOSURE of the per-group common component  (c_ab + c_bc + c_ca; 0 iff c is per-STATION)")
    for cons in sorted(CL):
        c = CL[cons]["c"]
        if not c:
            continue
        nullc = nul.closure(CL[cons]["sizes"])
        P(f"  {cons}: n {len(c):6d}  med|closure| {med(c):.3f}  null {nullc:.3f}  "
          f"near {near(c):.1f} %  (uniform would be 0.250)")
    P("")
    P("REFERENCE CENSUS")
    for cons in sorted(switches | collections.Counter({c: 0 for c in tot})):
        seen = collections.Counter()
        for (h, cn), d in H.items():
            if cn == cons:
                seen.update(d["ref"])
        P(f"  {cons}: {switches[cons]} reference switches over the run; "
          f"{len(seen)} distinct references; top {', '.join(f'{s}({n})' for s, n in seen.most_common(4))}")
    P(f"\nAS-TRACED |f| split on reference AGE (< {switch_k} epochs since the reference last switched):")
    for nm in ("fresh", "settled"):
        x = byswitch[nm]
        P(f"  {nm:>8}: n {len(x):8d}  med|f| {med(x):.3f}  near {near(x):.1f} %")
    return {"H": H, "tot": tot, "CL": CL, "switches": switches, "byswitch": byswitch, "nrows": nrows}


def selftest():
    """Two beds, one trace each, graded through the real path.

    BED A (reference-dominated): every group's fractions are a common -s_ref plus a small per-satellite term,
    and -s_ref is built as a per-STATION difference so the triangles must close.  Expect: as-traced ~ the
    reference's own spread, ref-free FAR below its null, closure ~ 0.
    BED B (pure uniform noise): as-traced, ref-free and closure must all sit AT their nulls — this is the
    guard that makes a BED-A win meaningful rather than a median-subtraction artifact.
    """
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"  {'PASS' if cond else 'FAIL'} {name} {detail}")

    stations = [f"S{i:03d}" for i in range(6)]
    sats = [f"G{i:02d}" for i in range(2, 9)]
    lam = 0.190293672798

    def write(path, mode, seed):
        rng = random.Random(seed)
        os.makedirs(os.path.join(path, "logs"))
        rho = {s: rng.random() - 0.5 for s in stations}          # the reference sat's error per station
        per = {s: (rng.random() - 0.5) * 0.06 for s in sats}     # small per-satellite term
        with open(os.path.join(path, "logs", "network_dd_fractional.csv"), "w") as fd:
            fd.write("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc,"
                     "dd_sigma_cyc,sigma_carry_aware\n")
            for k in range(10):
                tow = 30.0 * k
                for i, a in enumerate(stations):
                    for b in stations[i + 1:]:
                        for sat in sats:
                            if mode == "ref":
                                fr = wrap((per[sat] - per["G02"]) - (rho[a] - rho[b]))
                            else:
                                fr = rng.random() - 0.5
                            dd = 17 + fr
                            fd.write(f"2313,{tow:.3f},G,L1C,{sat},G02,{a},{b},{dd * lam:.4f},"
                                     f"{lam:.6f},{dd:.4f},{fr:.4f},0.01,0\n")

    td = tempfile.mkdtemp(prefix="ddfrac_")
    for mode in ("ref", "unif"):
        run = os.path.join(td, mode)
        write(run, mode, seed=7 if mode == "ref" else 11)
        buf = io.StringIO()
        r = regrade(run, sig="L1C", stride=1, null_draws=120, out=buf)
        d = r["tot"]["G"]
        m_f, m_u, n_u = med(d["f"]), med(d["u"]), st.mean(d["nu"])
        cl = med(r["CL"]["G"]["c"])
        print(f"  [{mode}] as-traced {m_f:.3f}  ref-free {m_u:.3f} (null {n_u:.3f})  closure {cl:.3f}")
        if mode == "ref":
            # the common part dominates: as-traced is large, ref-free collapses to the per-sat term (<= 0.06),
            # and the triangles close because c_hat IS a per-station difference
            chk("ref bed: as-traced carries the reference", m_f > 0.15, f"{m_f:.3f} > 0.15")
            chk("ref bed: ref-free collapses", m_u < 0.05 and m_u < 0.5 * n_u, f"{m_u:.3f} vs null {n_u:.3f}")
            chk("ref bed: triangles close", cl < 0.02, f"{cl:.3f} < 0.02")
            chk("ref bed: near% rises", near(d["u"]) > 90 > near(d["f"]),
                f"{near(d['f']):.0f} -> {near(d['u']):.0f} %")
        else:
            # uniform: every readout must sit AT its null, and the closure must NOT close
            chk("unif bed: as-traced at the null", abs(m_f - 0.25) < 0.03, f"{m_f:.3f} ~ 0.250")
            chk("unif bed: ref-free NO better than its null", m_u > 0.7 * n_u, f"{m_u:.3f} vs null {n_u:.3f}")
            chk("unif bed: the null is BELOW 0.25 (the artifact this control exists for)",
                n_u < 0.235, f"null {n_u:.3f} < 0.235")
            nullcl = med([wrap(sum(circ_median([wrap(random.Random(90000 + i * 7 + j).random()
                                                 - random.Random(91000 + i * 7 + j).random())
                                                for _ in range(4)]) for j in range(3)))
                          for i in range(200)])
            chk("unif bed: closure is NOT better than the DD-structure null",
                cl > 0.6 * nullcl, f"{cl:.3f} vs structure-null {nullcl:.3f}")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--signal", default="L1C")
    ap.add_argument("--cons", default=None, help="restrict to one constellation (G / E / C ...)")
    ap.add_argument("--stride", type=int, default=1, help="grade every Nth traced epoch")
    ap.add_argument("--null-draws", type=int, default=400)
    ap.add_argument("--switch-k", type=int, default=20,
                    help="epochs after a reference switch that count as 'fresh'")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("run_dir is required (or --selftest)")
    regrade(a.run_dir, sig=a.signal, cons_filter=a.cons, stride=max(1, a.stride),
            null_draws=a.null_draws, switch_k=a.switch_k)
    return 0


if __name__ == "__main__":
    sys.exit(main())
