"""Two runs' outputs compared (card GRA, plan step G6; bar B12 of card NRF: the off-identity comparisons of sections
112.1 F1, 116.1 H1 and 118.1 C1 to C3), and the member records of a phase-family stream.

    py -3.14 scripts/grading/compare_runs.py compare <run A> <run B> [--hours H] [--view identity|legacy]
    py -3.14 scripts/grading/compare_runs.py members <run> [<run> ...] [--view legend|awk|check]
    py -3.14 scripts/grading/compare_runs.py --selftest

compare
  legacy    byte for byte the grading folder's compare_runs.py: products.txt without its comment lines and without the
            Galileo P records on L1X or L5X and B records on C1X or C5X, whatever the run declares (card GRA, defect D4);
            every logs/*.csv present in both runs, line by line (network_stations.csv without a trailing phase_tracking
            column); the held-out user's traces (closed_loop_*) reported; --hours cuts both at the same epoch.
  identity  (the default) whole files first: products.txt and every logs/*.csv of either run, by sha256; a trace in
            one run only is reported.  A differing trace is compared again without the wall-time columns of WALL_TIME
            (named once here), and called identical but for them when that settles it.  A differing products.txt: the
            comment lines compared on their own (the legend), the member records each run's own legend declares
            ("# phase families (network.phase_families): E:L1X=L1C ...") set aside and each checked against its
            identity's record of the same epoch, then the rest compared record by record; the records left in one run
            only are counted by type and signal, with those that repeat another signal's record of the same band,
            satellite and epoch named (a copy the legend does not declare).
members
  legend    (the default) the P records the run's legend declares as members, each against its identity's record of
            the same epoch (value, flags, counter and tokens), with the records of a member signal the legend does not
            declare counted apart, and their equality with the same band's identity
  awk       byte for byte grade_n46.sh:24-30 (the awk over P and B records on L1X, L5X, C1X and C5X against L1C, L5Q,
            C1C and C5Q, the last record of each epoch, satellite and signal kept)
  check     byte for byte check_member_records.py (P records of L1C and L5Q against L1X and L5X in the same epoch)
"""
import argparse
import collections
import hashlib
import os
import re
import sys

import runs

MEMBER_P = ("L1X", "L5X")
MEMBER_B = ("C1X", "C5X")
WALL_TIME = {"network_datum_graph.csv": ("ms",), "network_run_summary.csv": ("loop_ms", "ms_per_epoch"),
             "network_service_health.csv": ("ms_per_epoch_window",)}
LEGEND = re.compile(r"^# phase families \(network\.phase_families\): (.*?);")


# ---------------------------------------------------------------------------------------------------------- legacy
def legacy_products(path, hours):
    kept, members, t0 = [], 0, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            if line.startswith("E,"):
                tow = float(line.split(",")[2])
                t0 = tow if t0 is None else t0
                if hours is not None and tow - t0 >= hours * 3600.0:
                    break
            q = line.split(",", 3)
            if len(q) > 2 and q[1].startswith("E") and ((q[0] == "P" and q[2] in MEMBER_P)
                                                        or (q[0] == "B" and q[2] in MEMBER_B)):
                members += 1
                continue
            kept.append(line)
    return kept, members


def legacy_csv(path, hours, drop_last_col=False):
    out, t0 = [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\n")
        cols = header.split(",")
        drop = drop_last_col and cols and cols[-1] == "phase_tracking"
        out.append(",".join(cols[:-1]) if drop else header)
        ti = cols.index("tow") if "tow" in cols else None
        for line in f:
            line = line.rstrip("\n")
            if ti is not None and hours is not None:
                try:
                    tow = float(line.split(",")[ti])
                    t0 = tow if t0 is None else t0
                    if tow - t0 >= hours * 3600.0:
                        break
                except (ValueError, IndexError):
                    pass
            out.append(",".join(line.split(",")[:-1]) if drop else line)
    return out


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x[:160], y[:160]
    if len(a) != len(b):
        return min(len(a), len(b)), f"(length {len(a)})", f"(length {len(b)})"
    return None


def compare_legacy(ta, tb, hours):
    da, db = runs.run_dir(ta), runs.run_dir(tb)
    pa, ma = legacy_products(os.path.join(da, "products.txt"), hours)
    pb, mb = legacy_products(os.path.join(db, "products.txt"), hours)
    d = first_diff(pa, pb)
    print(f"products.txt without member records: {'IDENTICAL' if d is None else 'DIFFER at line %d' % d[0]} "
          f"({len(pa)} lines; member records {ta} {ma}, {tb} {mb})")
    if d:
        print(f"    {ta}: {d[1]}\n    {tb}: {d[2]}")
    names = sorted(set(csv_names(da)) & set(csv_names(db)))
    for n in names:
        held_out = n.startswith("closed_loop")
        a = legacy_csv(os.path.join(da, "logs", n), hours, drop_last_col=(n == "network_stations.csv"))
        b = legacy_csv(os.path.join(db, "logs", n), hours, drop_last_col=(n == "network_stations.csv"))
        d = first_diff(a, b)
        tag = " (the held-out user: reported)" if held_out else ""
        if d is None:
            print(f"{n}: IDENTICAL ({len(a)} lines){tag}")
        else:
            print(f"{n}: DIFFER at line {d[0]}{tag}\n    {ta}: {d[1]}\n    {tb}: {d[2]}")


def csv_names(d):
    p = os.path.join(d, "logs")
    return [n for n in os.listdir(p) if n.endswith(".csv")] if os.path.isdir(p) else []


# -------------------------------------------------------------------------------------------------------- identity
def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def legend_members(path):
    """{(constellation, member signal): identity signal} declared by a products file's legend line, and the line."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if not ln.startswith("#"):
                if ln.startswith("E,"):
                    break
                continue
            m = LEGEND.match(ln)
            if m:
                out = {}
                for tok in m.group(1).split():
                    sy, pair = tok.split(":", 1)
                    mem, ident = pair.split("=", 1)
                    out[(sy, mem)] = ident
                return out, ln.rstrip("\r\n")
    return {}, ""


def epochs_of(path, hours):
    """[(epoch line, [records])] and the comment lines of a products file, cut at --hours from the first epoch."""
    eps, comments, t0 = [], [], None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            ln = ln.rstrip("\r\n")
            if ln.startswith("#"):
                comments.append(ln)
                continue
            if ln.startswith("E,"):
                tow = float(ln.split(",")[2])
                t0 = tow if t0 is None else t0
                if hours is not None and tow - t0 >= hours * 3600.0:
                    break
                eps.append((ln, []))
            elif eps:
                eps[-1][1].append(ln)
    return eps, comments


def split_members(recs, members):
    """(kept records, member records set aside, members unequal to their identity) of one epoch."""
    by = {}
    for r in recs:
        q = r.split(",", 3)
        if len(q) > 2:
            by[(q[0], q[1], q[2])] = q[3] if len(q) > 3 else ""
    kept, aside, bad = [], 0, 0
    for r in recs:
        q = r.split(",", 3)
        if len(q) > 2 and q[0] == "P" and (q[1][:1], q[2]) in members:
            aside += 1
            if by.get(("P", q[1], members[(q[1][:1], q[2])])) != (q[3] if len(q) > 3 else ""):
                bad += 1
            continue
        kept.append(r)
    return kept, aside, bad


def copies_of(rec, by):
    """The signal of another record of the same type, satellite and band in the same epoch whose fields equal rec's."""
    q = rec.split(",", 3)
    if len(q) < 4:
        return None
    for (t, sat, sig), rest in by.items():
        if t == q[0] and sat == q[1] and sig != q[2] and sig[1:2] == q[2][1:2] and rest == q[3]:
            return sig
    return None


def compare_products(pa, pb, ta, tb, hours):
    out = []
    ma, la = legend_members(pa)
    mb, lb = legend_members(pb)
    ea, ca = epochs_of(pa, hours)
    eb, cb = epochs_of(pb, hours)
    out.append(f"   legend: {'the same' if ca == cb else 'differs'}; {ta} declares "
               f"{' '.join(f'{s}:{m}={i}' for (s, m), i in sorted(ma.items())) or 'no member'}, {tb} declares "
               f"{' '.join(f'{s}:{m}={i}' for (s, m), i in sorted(mb.items())) or 'no member'}")
    if len(ea) != len(eb) or any(x[0] != y[0] for x, y in zip(ea, eb)):
        out.append(f"   epochs differ: {len(ea)} against {len(eb)}" + next(
            (f"; first at {x[0]} against {y[0]}" for x, y in zip(ea, eb) if x[0] != y[0]), ""))
    aside = {ta: [0, 0], tb: [0, 0]}
    only = {ta: collections.Counter(), tb: collections.Counter()}
    copy = {ta: collections.Counter(), tb: collections.Counter()}
    first = None
    for (la_, ra), (lb_, rb) in zip(ea, eb):
        ka, na, ba = split_members(ra, ma)
        kb, nb, bb = split_members(rb, mb)
        aside[ta][0] += na
        aside[ta][1] += ba
        aside[tb][0] += nb
        aside[tb][1] += bb
        if ka == kb:
            continue
        if first is None:
            d = first_diff(ka, kb)
            first = (la_, d)
        sa, sb = collections.Counter(ka), collections.Counter(kb)
        for tag, extra, recs in ((ta, sa - sb, ra), (tb, sb - sa, rb)):
            by = {}
            for r in recs:
                q = r.split(",", 3)
                if len(q) > 3:
                    by[(q[0], q[1], q[2])] = q[3]
            for r, k in extra.items():
                q = r.split(",", 3)
                key = (q[0], q[1][:1], q[2] if len(q) > 2 else "")
                only[tag][key] += k
                c = copies_of(r, by)
                if c:
                    copy[tag][key + (c,)] += k
    out.append(f"   member records set aside by each run's legend: {ta} {aside[ta][0]} ({aside[ta][1]} unequal to their "
               f"identity), {tb} {aside[tb][0]} ({aside[tb][1]} unequal)")
    if first is None:
        out.append("   the other records: identical")
        return out
    out.append(f"   the other records: DIFFER, first in the epoch {first[0]} at its record {first[1][0]}: {first[1][1]} "
               f"against {first[1][2]}")
    for tag in (ta, tb):
        if only[tag]:
            out.append(f"   records in {tag} only, by type, constellation and signal: " + ", ".join(
                f"{t} {s} {g} {n}" for (t, s, g), n in sorted(only[tag].items())))
        if copy[tag]:
            out.append(f"      of them repeating the same epoch's record of another signal of the band (a copy no "
                       f"legend declares): " + ", ".join(f"{t} {s} {g} = {c} {n}" for (t, s, g, c), n in
                                                        sorted(copy[tag].items())))
    return out


def csv_rows(path, drop_cols, hours):
    with open(path, encoding="utf-8", errors="replace") as fh:
        header = fh.readline().rstrip("\r\n").split(",")
        keep = [i for i, h in enumerate(header) if h not in drop_cols]
        ti = header.index("tow") if "tow" in header else None
        out, t0 = [[header[i] for i in keep]], None
        for ln in fh:
            q = ln.rstrip("\r\n").split(",")
            if ti is not None and hours is not None:
                try:
                    tow = float(q[ti])
                    t0 = tow if t0 is None else t0
                    if tow - t0 >= hours * 3600.0:
                        break
                except (ValueError, IndexError):
                    pass
            out.append([q[i] for i in keep if i < len(q)])
    return out


def compare_identity(ta, tb, hours):
    da, db = runs.run_dir(ta), runs.run_dir(tb)
    out = [f"== {ta} against {tb}: whole files first" + (f" (cut at {hours:g} h from the first epoch)" if hours else "")]
    pa, pb = os.path.join(da, "products.txt"), os.path.join(db, "products.txt")
    same_p = hours is None and sha(pa) == sha(pb)
    out.append(f"products.txt: {'identical (sha256)' if same_p else 'compared by record'}")
    if not same_p:
        out += compare_products(pa, pb, ta, tb, hours)
    na, nb = set(csv_names(da)), set(csv_names(db))
    tally = collections.Counter()
    for n in sorted(na | nb):
        if n not in nb or n not in na:
            out.append(f"logs/{n}: only in {ta if n in na else tb}")
            tally["in one run only"] += 1
            continue
        a, b = os.path.join(da, "logs", n), os.path.join(db, "logs", n)
        user = " (the held-out user's trace)" if n.startswith("closed_loop") else ""
        if hours is None and sha(a) == sha(b):
            tally["identical"] += 1
            continue
        wall = WALL_TIME.get(n, ())
        ra, rb = csv_rows(a, wall, hours), csv_rows(b, wall, hours)
        if ra == rb:
            if wall and hours is None:
                out.append(f"logs/{n}: identical but for the wall-time columns ({', '.join(wall)}){user}")
                tally["identical but for wall time"] += 1
            else:
                tally["identical"] += 1
            continue
        d = first_diff([",".join(x) for x in ra], [",".join(x) for x in rb])
        out.append(f"logs/{n}: DIFFER at line {d[0]}{user}" + (f" (wall-time columns {', '.join(wall)} left out)"
                                                                if wall else "") + f"\n    {ta}: {d[1]}\n    {tb}: {d[2]}")
        tally["differ"] += 1
    out.append(f"traces: {len(na | nb)}; " + ", ".join(f"{k} {v}" for k, v in sorted(tally.items())))
    return out


def cmd_compare(a):
    if a.view == "legacy":
        compare_legacy(a.a, a.b, a.hours)
    else:
        sys.stdout.write("\n".join(compare_identity(a.a, a.b, a.hours)) + "\n")
    return 0


# --------------------------------------------------------------------------------------------------------- members
def members_awk(run):
    """grade_n46.sh:24-30: the last record of each (epoch, satellite, signal) of P and B records of Galileo; every
    member key (L1X, L5X, C1X, C5X) against its identity's (L1C, L5Q, C1C, C5Q) record after the first three fields."""
    ident = {"L1X": "L1C", "L5X": "L5Q", "C1X": "C1C", "C5X": "C5Q"}
    v, ep = {}, ""
    with open(os.path.join(runs.run_dir(run), "products.txt"), encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            ln = ln.rstrip("\n")
            q = ln.split(",")
            if q[0] == "E":
                ep = q[2] if len(q) > 2 else ""
            if q[0] in ("P", "B") and len(q) > 1 and q[1].startswith("E"):
                v[(ep, q[1], q[2] if len(q) > 2 else "")] = ln
    n = bad = 0
    for (e, sat, s), line in v.items():
        if s not in ident:
            continue
        n += 1
        other = v.get((e, sat, ident[s]), "")
        a = re.sub(r"^[PB],[^,]*,[^,]*,", "", line, count=1)
        b = re.sub(r"^[PB],[^,]*,[^,]*,", "", other, count=1)
        bad += a != b
    return f"member records {n}, unequal to their identity {bad}\n"


def members_check(run):
    """check_member_records.py: P records of L1C and L5Q against L1X and L5X of the same epoch."""
    pairs = (("L1C", "L1X"), ("L5Q", "L5X"))
    recs, epochs, t, note = collections.defaultdict(dict), 0, None, ""
    with open(os.path.join(runs.run_dir(run), "products.txt"), encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln.startswith("# phase families"):
                note = ln.strip()
            if ln.startswith("E,"):
                t = ln.split(",")[2]
                epochs += 1
            elif ln.startswith("P,") and t is not None:
                p = ln.rstrip("\n").split(",")
                if p[1].startswith("E"):
                    recs[(t, p[1])][p[2]] = p[3:]
    n = bad = orphan = 0
    for k, d in recs.items():
        for ident, mem in pairs:
            if ident in d:
                n += 1
                if d.get(mem) != d[ident]:
                    bad += 1
            elif mem in d:
                orphan += 1
    return (f"== {run}: {epochs} epochs; {n} identity records, {bad} without an equal member record, {orphan} member "
            f"records without an identity; legend note: {'present' if note else 'absent'}")


def members_legend(run):
    path = os.path.join(runs.run_dir(run), "products.txt")
    members, line = legend_members(path)
    eps, _ = epochs_of(path, None)
    n = bad = orphan = 0
    undeclared = collections.Counter()
    for _, recs in eps:
        by = {}
        for r in recs:
            q = r.split(",", 3)
            if len(q) > 2:
                by[(q[0], q[1], q[2])] = q[3] if len(q) > 3 else ""
        for (t, sat, sig), rest in by.items():
            if t == "P" and (sat[:1], sig) in members:
                n += 1
                ident = by.get(("P", sat, members[(sat[:1], sig)]))
                if ident is None:
                    orphan += 1
                elif ident != rest:
                    bad += 1
            elif sat.startswith("E") and sig[2:3] == "X" and sig[1:2] in "15" and t in ("P", "B"):
                ident = next((s for (tt, ss, s) in by if tt == t and ss == sat and s != sig and s[1:2] == sig[1:2]), None)
                undeclared[(t, sig, "equal to " + ident if ident and by[(t, sat, ident)] == rest else
                            ("unequal to " + ident if ident else "alone"))] += 1
    out = [f"== {run}: legend {'declares ' + ' '.join(f'{s}:{m}={i}' for (s, m), i in sorted(members.items())) if members else 'absent'}; "
           f"declared member records {n}, unequal to their identity {bad}, without an identity {orphan}"]
    if undeclared:
        out.append("   records of a Galileo X signal of band 1 or 5 the legend does not declare: " + ", ".join(
            f"{t} {s} {k} {v}" for (t, s, k), v in sorted(undeclared.items())))
    return "\n".join(out)


def cmd_members(a):
    for run in a.runs:
        if a.view == "awk":                     # awk's printf wrote LF line ends
            sys.stdout.flush()
            sys.stdout.buffer.write(members_awk(run).encode("utf-8"))
            sys.stdout.buffer.flush()
        elif a.view == "check":
            print(members_check(run))
        else:
            print(members_legend(run))
    return 0


# -------------------------------------------------------------------------------------------------------- selftest
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

    def capture(fn, *args):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            fn(*args)
        return buf.getvalue()

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g6_compare.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        out = "\n".join(compare_identity("fam", "sep", None))
        check("member records set aside by each run's legend: fam 2 (0 unequal to their identity), sep 0" in out,
              "the declared P members set aside, each equal to its identity")
        check("records in fam only, by type, constellation and signal: B E C1X 1" in out
              and "B E C1X = C1C 1" in out, "an undeclared B C1X record reported, with its identity copy named")
        check("logs/network_run_summary.csv: identical but for the wall-time columns (loop_ms, ms_per_epoch)" in out,
              "a trace identical but for its wall-time columns")
        check("logs/network_extra.csv: only in fam" in out, "a trace in one run only")
        out2 = "\n".join(compare_identity("sep", "sep2", None))
        check("products.txt: identical (sha256)" in out2 and "traces: 2; identical 2" in out2, "identical runs")
        leg = capture(compare_legacy, "fam", "sep", None)
        check("products.txt without member records: IDENTICAL" in leg and "member records fam 3, sep 0" in leg
              and "network_extra.csv" not in leg, "the legacy view sets aside B C1X too and skips a one-run trace")
        check(members_awk("fam") == "member records 3, unequal to their identity 0\n", "the awk view")
        check("declared member records 2, unequal to their identity 0" in members_legend("fam")
              and "B C1X equal to C1C 1" in members_legend("fam"), "the legend view")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("compare_runs.py")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--hours", type=float, default=None)
    c.add_argument("--view", choices=("identity", "legacy"), default="identity")
    m = sub.add_parser("members")
    m.add_argument("runs", nargs="+")
    m.add_argument("--view", choices=("legend", "awk", "check"), default="legend")
    a = ap.parse_args()
    return {"compare": cmd_compare, "members": cmd_members}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
