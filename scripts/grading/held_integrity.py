"""The integrity of held links (card GRA, results of 2026-11-19; card NRF's night report of 2026-11-19, section 1): the
population of long one-signed holds, the offline replay of a windowed monitor, the gap resumptions that meet a held
link, and, on a run with truth, the held segments by truth.  The managing session's scratch scripts
(gates/manager_2026-11-19/decision_split.py, onesigned_scan.py, window_monitor_replay.py) are rewritten here from
their definitions without pandas; they stay where they are.

    py -3.14 scripts/grading/held_integrity.py onesigned <run> [<run> ...] [--view legacy|exact] [--mm 8]
                                               [--share 0.90] [--min-hours 1] [--list N] [--out holds.jsonl]
    py -3.14 scripts/grading/held_integrity.py window <run> [<run> ...] [--view legacy|exact] [--windows 20,40,80]
                                               [--mm 4,5,6,8,10]
    py -3.14 scripts/grading/held_integrity.py resume <run> [<run> ...] [--list N] [--out cases.jsonl]
    py -3.14 scripts/grading/held_integrity.py truth <run> [<run> ...] [--from-s 1815] [--windows 20,40,80]
                                               [--mm 4,6,8,10] [--rule any|majority]
    py -3.14 scripts/grading/held_integrity.py --selftest

A hold (segment) is a run of consecutive held rows of one link (station, sat, sig1, sig2) of logs/network_ring_fix.csv
with constant (n1, n2), rows with no residual (resid_if_m) dropped first, a gap of more than 120 s ending it.  Its
signature (the attribution of 2026-11-19, section 4): a hold of at least --min-hours whose mean residual is at least
--mm in size with at least --share of its rows on one side (a zero residual counts on the negative side, as the
scratch scripts count it).  The residuals are kept in units of 0.01 mm (the trace writes five decimals), so every
threshold test is exact; a residual with more decimals is counted and reported.

The sub-commands:
  onesigned  legacy: byte for byte decision_split.py's printout (each run, then all runs together: holds of an hour or
             more, the decision row of each (the last decided row of the link at or before the hold's start, within
             120 s), the signature at 8 mm, the share by formal sigma, log-odds and fraction at decision, each holder
             of the signature); exact: per run and constellation the holds of an hour or more, the held hours, the
             signature at 8, 10 and 12 mm, the size quantiles of the mean residual, and every hold with the signature
             at --mm listed by start (onesigned_scan.py's population and columns, its order replaced by a fixed one)
  window     legacy: byte for byte window_monitor_replay.py's printout (a hold trips when the mean of its last W held
             residuals reaches T in size; W counts rows of the hold, the rows that exist, not epochs); exact: the same
             counts per run as well as together, the trip tested on exact sums, the windows counted in rows
  resume     every gap resumption (T0_gap arc_resume rows of logs/network_events.csv sharing station, satellite and
             epoch; class = the action) that meets a link held at that epoch (a ring-fix row of the link, state held,
             at the resumption's epoch, the resumption naming sig1 or sig2), by class: the residual at the epoch, the
             held epochs that followed with the same integers, and the first RINGFIX release or to_tree of the link at
             or after the epoch, with its cause and delay
  truth      on a run with truth (the ring-fix trace's audit column, a simulator bed): each hold graded by its rows'
             audit (--rule any: wrong when any row is wrong, right when all are right, else no truth; --rule majority:
             the class of more than half of its rows, else mixed, the scout's rule of 2026-11-19), rows before
             --from-s of the day dropped (the simulator is graded from 00:30:15); per run the holds by truth with their residual
             statistics, the signature's catch and false rate on them, and the window rule's catch, false trips and
             delay on them
"""
import argparse
import bisect
import collections
import json
import statistics
import sys

import runs

GAP_S = 120.0
SCALE = 100000          # residual units: 1e-5 m


def resid_units(s):
    """(units of 1e-5 m, exact) of a residual string; None when the field is empty or not a number."""
    s = s.strip()
    if not s:
        return None
    try:
        f = float(s)
    except ValueError:
        return None
    if f != f or f in (float("inf"), float("-inf")):
        return None
    u = round(f * SCALE)
    return u, abs(f * SCALE - u) < 1e-6


def read_trace(run, want_decided=False, want_audit=False):
    """The held rows of each link {(station, sat, sig1, sig2): [(tow, n1, n2, units, audit)]} sorted by time
    (stable), the decided rows {link: [(tow, sigma_if, frac_if)]} when asked, and the count of inexact residuals."""
    held, dec, inexact = collections.defaultdict(list), collections.defaultdict(list), 0
    header, it = runs.raw_rows(run, "network_ring_fix.csv")
    ix = {k: i for i, k in enumerate(header)}
    itow, ista, isat, is1, is2, ist, in1, in2, ires = (ix[k] for k in ("tow", "station", "sat", "sig1", "sig2", "state",
                                                                       "n1", "n2", "resid_if_m"))
    isig, ifrac = ix.get("sigma_if"), ix.get("frac_if")
    iaud = ix.get("audit")
    for r in it:
        st = r[ist]
        if st == "held":
            ru = resid_units(r[ires])
            if ru is None:
                continue
            inexact += not ru[1]
            aud = r[iaud] if (want_audit and iaud is not None and iaud < len(r)) else ""
            held[(r[ista], r[isat], r[is1], r[is2])].append((float(r[itow]), r[in1], r[in2], ru[0], aud))
        elif want_decided and st == "decided":
            dec[(r[ista], r[isat], r[is1], r[is2])].append((float(r[itow]), num(r[isig]), num(r[ifrac])))
    for v in held.values():
        v.sort(key=lambda x: x[0])
    for v in dec.values():
        v.sort(key=lambda x: x[0])
    return held, dec, inexact


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return float("nan")


class Hold:
    """One hold: its link, integers, first and last epoch, rows and the residual sums."""
    __slots__ = ("link", "n1", "n2", "t0", "t1", "rows", "n", "sum", "npos", "audit", "sigma", "frac")

    def __init__(self, link, rows):
        self.link, self.rows = link, rows
        self.n1, self.n2 = rows[0][1], rows[0][2]
        self.t0, self.t1 = rows[0][0], max(x[0] for x in rows)
        self.n = len(rows)
        self.sum = sum(x[3] for x in rows)
        self.npos = sum(1 for x in rows if x[3] > 0)
        self.audit = None
        self.sigma = self.frac = float("nan")

    @property
    def dur(self):
        return self.t1 - self.t0

    @property
    def mean(self):
        return self.sum / self.n / SCALE

    @property
    def one(self):
        p = self.npos / self.n
        return max(p, 1 - p)

    def signature(self, mm, share, min_s):
        """The signature, tested exactly: duration, |sum| >= mm * n (in units), the one-sided share."""
        return (self.dur >= min_s and abs(self.sum) * 1000 >= round(mm * SCALE) * self.n
                and max(self.npos, self.n - self.npos) >= share * self.n - 1e-9)


def holds_of(held):
    """Every hold of a run, in the order of the sorted links and then time."""
    out = []
    for link in sorted(held):
        rows = held[link]
        start = 0
        for i in range(1, len(rows) + 1):
            if i == len(rows) or (rows[i][1], rows[i][2]) != (rows[i - 1][1], rows[i - 1][2]) \
                    or abs(rows[i][0] - rows[i - 1][0]) > GAP_S:
                out.append(Hold(link, rows[start:i]))
                start = i
    return out


def attach_decisions(holds, dec):
    """The decision row of each hold: the last decided row of its link at or before its start, within 120 s."""
    for h in holds:
        d = dec.get(h.link)
        if not d:
            continue
        i = bisect.bisect_right([x[0] for x in d], h.t0) - 1
        if i >= 0 and h.t0 - d[i][0] <= GAP_S:
            h.sigma, h.frac = d[i][1], d[i][2]


def log_odds(h):
    return (1.0 - 2.0 * abs(h.frac)) / (2.0 * h.sigma ** 2) if h.sigma == h.sigma else float("nan")


def median(xs):
    return statistics.median(xs) if xs else float("nan")


# ----------------------------------------------------------------------------------------------------- onesigned
def decision_table(long_holds, label, mm, share):
    out = []
    sus = [h for h in long_holds if h.signature(mm, share, 0)]
    have = [h for h in long_holds if h.sigma == h.sigma]
    out.append(f"== {label}: holds of an hour or more {len(long_holds)}, with a decision row {len(have)}, suspects "
               f"{len(sus)} (with a decision row {sum(1 for h in have if h.signature(mm, share, 0))})")
    for sysname in sorted({h.link[1][0] for h in have}):
        sub = [h for h in have if h.link[1][0] == sysname]
        out.append(f"   {sysname}: holds {len(sub)}, suspects {sum(1 for h in sub if h.signature(mm, share, 0))}")

        def row(s):
            k = sum(1 for h in s if h.signature(mm, share, 0))
            return len(s), k, 100.0 * k / len(s)
        for a, b in [(0.0, 0.05), (0.05, 0.08), (0.08, 0.10), (0.10, 0.12), (0.12, 0.15), (0.15, 9.0)]:
            s = [h for h in sub if a < h.sigma <= b]
            if s:
                n, k, p = row(s)
                out.append(f"      sigma_if at decision in ({a:.2f}, {b:.2f}]: holds {n:5d}  suspects {k:3d}  share "
                           f"{p:5.1f} %  median |mean residual| {1000 * median([abs(h.mean) for h in s]):.2f} mm")
        for a, b in [(0, 20), (20, 30), (30, 60), (60, 1e9)]:
            s = [h for h in sub if a < log_odds(h) <= b]
            if s:
                n, k, p = row(s)
                out.append(f"      log-odds at decision in ({a}, {b if b < 1e8 else 'inf'}]: holds {n:5d}  suspects "
                           f"{k:3d}  share {p:5.1f} %")
        for a, b in [(0.0, 0.05), (0.05, 0.10), (0.10, 0.15), (0.15, 0.21)]:
            s = [h for h in sub if a < abs(h.frac) <= b]
            if s:
                n, k, p = row(s)
                out.append(f"      |frac_if| at decision in ({a:.2f}, {b:.2f}]: holds {n:5d}  suspects {k:3d}  share "
                           f"{p:5.1f} %")
    return out


def load_holds(run, want_decided=False, want_audit=False):
    held, dec, inexact = read_trace(run, want_decided, want_audit)
    hs = holds_of(held)
    if want_decided:
        attach_decisions(hs, dec)
    return hs, inexact


def label_of(run):
    return run if run.startswith("_rap_") else "_rap_" + run


def cmd_onesigned(a):
    min_s = a.min_hours * 3600.0
    every, out, inexact, listed = [], [], 0, []
    for run in a.runs:
        hs, ne = load_holds(run, want_decided=a.view == "legacy")
        inexact += ne
        long_holds = sorted([h for h in hs if h.dur >= min_s], key=lambda h: h.t0)
        if a.view == "legacy":
            every += long_holds
            out += decision_table(long_holds, label_of(run), a.mm, a.share)
            for h in sorted([h for h in long_holds if h.signature(a.mm, a.share, 0)], key=lambda h: -h.dur):
                out.append(f"     SUSPECT {h.link[0]} {h.link[1]} n1={h.n1} n2={h.n2} hours {(h.t0 % 86400) / 3600:5.2f}-"
                           f"{(h.t1 % 86400) / 3600:5.2f} mean {1000 * h.mean:+6.1f} mm one-sign {h.one:.2f} | decided "
                           f"sigma_if {h.sigma:.3f} frac {h.frac:.3f} log-odds {log_odds(h):.1f}")
            continue
        out.append(f"== {label_of(run)}: holds {len(hs)}, of {a.min_hours:g} h or more {len(long_holds)}")
        for sysname in sorted({h.link[1][0] for h in long_holds}):
            sub = [h for h in long_holds if h.link[1][0] == sysname]
            cells = []
            for mm in sorted({8.0, 10.0, 12.0, a.mm}):
                s = [h for h in sub if h.signature(mm, a.share, 0)]
                cells.append(f"signature at {mm:g} mm {len(s)} ({sum(h.dur for h in s) / 3600:.1f} h)")
            out.append(f"   {sysname}: holds {len(sub)} ({sum(h.dur for h in sub) / 3600:.1f} held hours); " + "; ".join(cells))
        q = sorted(abs(h.mean) for h in long_holds)
        if q:
            out.append("   |mean residual| of these holds: median %.2f mm, 90 %% %.2f mm, 99 %% %.2f mm" % tuple(
                1000 * quantile(q, p) for p in (0.5, 0.9, 0.99)))
        sus = [h for h in long_holds if h.signature(a.mm, a.share, 0)]
        sus.sort(key=lambda h: (h.t0, h.link))
        for h in sus[: a.list]:
            out.append(f"     {h.link[0]} {h.link[1]} {h.link[2]}/{h.link[3]} n1={h.n1} n2={h.n2} TOW {h.t0:.0f}..{h.t1:.0f} "
                       f"(hours of day {(h.t0 % 86400) / 3600:5.2f}-{(h.t1 % 86400) / 3600:5.2f}) rows {h.n} mean "
                       f"{1000 * h.mean:+.2f} mm one-sided {h.one:.2f}")
        listed += [dict(run=label_of(run), station=h.link[0], sat=h.link[1], sig1=h.link[2], sig2=h.link[3], n1=h.n1,
                        n2=h.n2, t0=h.t0, t1=h.t1, n=h.n, mean=h.mean, one=h.one) for h in sus]
    if a.view == "legacy":
        out += decision_table(every, "ALL RUNS TOGETHER", a.mm, a.share)
    elif len(a.runs) > 1:
        out.append(f"== all runs: holds with the signature at {a.mm:g} mm: {len(listed)}")
    if inexact:
        out.append(f"   residuals with more than five decimals (rounded to 0.01 mm): {inexact}")
    sys.stdout.write("\n".join(out) + "\n")
    if a.out:
        with open(a.out, "w") as fh:
            for x in listed:
                fh.write(json.dumps(x) + "\n")


def quantile(sorted_xs, p):
    """Linear-interpolation quantile (pandas' default) of a sorted list."""
    k = (len(sorted_xs) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(sorted_xs) - 1)
    return sorted_xs[lo] + (sorted_xs[hi] - sorted_xs[lo]) * (k - lo)


# -------------------------------------------------------------------------------------------------------- window
def first_trip(h, w, t_units):
    """The epoch of the first row at which the mean of the hold's last w rows reaches t (exact), or None."""
    if h.n < w:
        return None
    rs = h.rows
    s = sum(x[3] for x in rs[:w])
    lim = t_units * w
    if abs(s) >= lim:
        return rs[w - 1][0]
    for i in range(w, len(rs)):
        s += rs[i][3] - rs[i - w][3]
        if abs(s) >= lim:
            return rs[i][0]
    return None


def replay(hs, windows, mms, sus_of):
    """{(w, mm): counts} of one run: holds, suspects, caught, other tripped, other, the delays of the caught."""
    res = {}
    sus = [sus_of(h) for h in hs]
    for w in windows:
        for mm in mms:
            t_units = round(mm * SCALE / 1000)
            c = dict(holds=len(hs), suspects=sum(sus), caught=0, other_tripped=0, other=len(hs) - sum(sus), delays=[])
            for h, s in zip(hs, sus):
                t = first_trip(h, w, t_units)
                if t is None:
                    continue
                if s:
                    c["caught"] += 1
                    c["delays"].append((t - h.t0) / 60.0)
                else:
                    c["other_tripped"] += 1
            res[(w, mm)] = c
    return res


def cmd_window(a):
    windows = [int(x) for x in a.windows.split(",")]
    mms = [float(x) for x in a.mm.split(",")]
    out, tot, per_run = [], {}, []
    for run in a.runs:
        hs, _ = load_holds(run)
        sus_of = lambda h: h.signature(8.0, 0.90, 3600.0)  # noqa: E731
        out.append(f"== {label_of(run)}: holds {len(hs)} (of an hour or more {sum(1 for h in hs if h.dur >= 3600)}), "
                   f"suspects {sum(1 for h in hs if sus_of(h))}")
        res = replay(hs, windows, mms, sus_of)
        per_run.append((label_of(run), res))
        for k, v in res.items():
            t = tot.setdefault(k, dict(holds=0, suspects=0, caught=0, other_tripped=0, other=0, delays=[]))
            for f in ("holds", "suspects", "caught", "other_tripped", "other"):
                t[f] += v[f]
            if v["delays"]:
                t["delays"].append(median(v["delays"]))
    out.append("== ALL RUNS TOGETHER: window (epochs of 30 s), threshold (mm): suspects caught / all suspects; other holds "
               "tripped / all other holds (share); median minutes from the hold start to the trip for the caught suspects "
               "(median over runs)")
    for (w, mm), t in sorted(tot.items()):
        share = 100.0 * t["other_tripped"] / max(1, t["other"])
        d = median(t["delays"])
        out.append(f"   W={w:3d} ({w // 2:2d} min)  T={mm:4.1f} mm: caught {t['caught']:3d}/{t['suspects']:3d}; other holds "
                   f"tripped {t['other_tripped']:6d}/{t['other']:6d} ({share:5.2f} %); median delay {d:6.1f} min")
    if a.view == "exact":
        out.append("== per run (trips tested on exact sums; W counts rows of the hold): window, threshold: caught / "
                   "suspects, other holds tripped, median delay of the caught (min)")
        for label, res in per_run:
            out.append(f"   {label}: " + "; ".join(
                f"W={w} T={mm:g}: {v['caught']}/{v['suspects']}, {v['other_tripped']}, {median(v['delays']):.1f}"
                for (w, mm), v in sorted(res.items())))
    sys.stdout.write("\n".join(out) + "\n")


# -------------------------------------------------------------------------------------------------------- resume
def resumptions(run):
    """{(station, sat, epoch key): {"class": action, "signals": set, "details": [...]}} of the T0_gap arc_resume rows,
    and the RINGFIX release / to_tree events {(station, sat): [(epoch key, kind, cause, detail)]}."""
    res, ends = {}, collections.defaultdict(list)
    _, it = runs.raw_rows(run, "network_events.csv")
    for r in it:
        if len(r) < 10:
            continue
        if r[2] == "T0_gap" and r[3] == "arc_resume":
            k = (r[5], r[4], runs.tow_key(r[1]))
            x = res.setdefault(k, {"class": set(), "signals": set(), "details": []})
            x["class"].add(r[8])
            x["signals"].add(r[9].split(";")[0])
            x["details"].append(r[9])
        elif r[2] == "RINGFIX" and r[3] in ("release", "to_tree"):
            ends[(r[5], r[4])].append((runs.tow_key(r[1]), r[3], r[8], r[9]))
    for v in ends.values():
        v.sort(key=lambda x: x[0])
    return res, ends


def detail_sigs(detail):
    d = dict(t.split("=", 1) for t in detail.split() if "=" in t)
    return d.get("sig1"), d.get("sig2")


def resume_cases(run):
    """(resumptions, cases): every resumption meeting a link held at its epoch, one case per (resumption, link)."""
    res, ends = resumptions(run)
    held, _, _ = read_trace(run)
    by_sta_sat = collections.defaultdict(list)
    for link in held:
        by_sta_sat[link[:2]].append(link)
    index = {link: {runs.tow_key(x[0]): i for i, x in enumerate(rows)} for link, rows in held.items()}
    cases = []
    for (sta, sat, t), x in sorted(res.items(), key=lambda kv: (kv[0][2], kv[0][0], kv[0][1])):
        for link in sorted(by_sta_sat.get((sta, sat), [])):
            if not (x["signals"] & {link[2], link[3]}):
                continue
            i = index[link].get(t)
            if i is None:
                continue
            rows = held[link]
            j = i
            while j + 1 < len(rows) and (rows[j + 1][1], rows[j + 1][2]) == (rows[i][1], rows[i][2]) \
                    and rows[j + 1][0] - rows[j][0] <= GAP_S:
                j += 1
            e = [y for y in ends.get((sta, sat), []) if y[0] >= t and detail_sigs(y[3]) == (link[2], link[3])]
            end = e[0] if e else None
            cases.append(dict(run=label_of(run), station=sta, sat=sat, tow=t, cls="+".join(sorted(x["class"])),
                              signals=sorted(x["signals"]), sig1=link[2], sig2=link[3], n1=rows[i][1], n2=rows[i][2],
                              resid_m=rows[i][3] / SCALE, held_rows=j - i + 1,
                              max_resid_m=max(abs(y[3]) for y in rows[i:j + 1]) / SCALE,
                              end=None if end is None else dict(kind=end[1], cause=end[2], delay_s=end[0] - t),
                              details=x["details"]))
    return res, cases


def cmd_resume(a):
    out, cases_all = [], []
    tally_all = collections.Counter()
    for run in a.runs:
        res, cases = resume_cases(run)
        tally = collections.Counter(c["cls"] for c in cases)
        tally_all.update(tally)
        out.append(f"== {label_of(run)}: gap resumptions {len(res)}; meeting a link held at that epoch {len(cases)}: "
                   + ", ".join(f"{k} {v}" for k, v in sorted(tally.items())))
        for cls in sorted(tally):
            cc = [c for c in cases if c["cls"] == cls]
            r = sorted(abs(c["resid_m"]) for c in cc)
            ends_c = collections.Counter("none" if c["end"] is None else f"{c['end']['kind']} {c['end']['cause']}"
                                         for c in cc)
            out.append(f"   {cls}: {len(cc)}; |residual| at the epoch median {1000 * median(r):.1f} mm, max "
                       f"{max(r):.3f} m; held rows that followed median {median([c['held_rows'] for c in cc]):g}; "
                       f"ended by " + ", ".join(f"{k} {v}" for k, v in sorted(ends_c.items())))
        for c in [c for c in cases if not c["cls"].startswith("clean")][: a.list]:
            end = c["end"]
            out.append(f"      {c['tow']} {c['station']}-{c['sat']} {c['cls']} on {','.join(c['signals'])}: held "
                       f"({c['n1']}, {c['n2']}) residual {c['resid_m']:+.3f} m, {c['held_rows']} held row(s), max "
                       f"|residual| {c['max_resid_m']:.3f} m; "
                       + ("held to the end" if end is None else
                          f"{end['kind']} {end['cause']} after {end['delay_s']} s"))
        cases_all += cases
    if len(a.runs) > 1:
        out.append(f"== all runs: meeting a held link {len(cases_all)}: "
                   + ", ".join(f"{k} {v}" for k, v in sorted(tally_all.items())))
    sys.stdout.write("\n".join(out) + "\n")
    if a.out:
        with open(a.out, "w") as fh:
            for c in cases_all:
                fh.write(json.dumps(c) + "\n")


# --------------------------------------------------------------------------------------------------------- truth
def truth_holds(run, from_s, rule="any"):
    """The holds of a run with truth, rows before from_s of the day dropped, each graded by its rows' audit: rule
    "any", wrong when any row is wrong, right when every row is right, else none; rule "majority", the class (wrong,
    right, none for an empty audit) of more than half of its rows, else mixed."""
    held, _, _ = read_trace(run, want_audit=True)
    for link in list(held):
        held[link] = [x for x in held[link] if x[0] % 86400 >= from_s]
    hs = holds_of({k: v for k, v in held.items() if v})
    for h in hs:
        if rule == "majority":
            c = collections.Counter(x[4] or "none" for x in h.rows)
            top, k = c.most_common(1)[0]
            h.audit = top if 2 * k > h.n else "mixed"
            continue
        aud = {x[4] for x in h.rows}
        h.audit = "wrong" if "wrong" in aud else ("right" if aud == {"right"} else "none")
    return hs


def cmd_truth(a):
    windows = [int(x) for x in a.windows.split(",")]
    mms = [float(x) for x in a.mm.split(",")]
    out = []
    for run in a.runs:
        hs = truth_holds(run, a.from_s, a.rule)
        by = collections.defaultdict(list)
        for h in hs:
            by[h.audit].append(h)
        out.append(f"== {label_of(run)}: holds from {a.from_s:g} s of the day {len(hs)}, graded by the rule "
                   f"'{a.rule}': "
                   + ", ".join(f"{k} {len(v)}" for k, v in sorted(by.items())))
        for k, v in sorted(by.items()):
            m = sorted(abs(h.mean) for h in v)
            ones = [h.one for h in v]
            out.append(f"   {k}: holds {len(v)}, rows {sum(h.n for h in v)}, held hours {sum(h.dur for h in v) / 3600:.1f};"
                       f" |mean residual| median {1000 * median(m):.2f} mm, 90 % {1000 * quantile(m, 0.9):.2f} mm, max "
                       f"{1000 * m[-1]:.2f} mm; one-sided share median {median(ones):.2f}; of an hour or more "
                       f"{sum(1 for h in v if h.dur >= 3600)}, with the signature at 8 mm "
                       f"{sum(1 for h in v if h.signature(8.0, 0.90, 3600.0))}")
        rows_by = collections.defaultdict(list)
        for h in hs:
            for x in h.rows:
                rows_by[x[4] or "none"].append(abs(x[3]))
        out.append("   |residual| of held rows by their own audit, median: " + ", ".join(
            f"{k} {1000 * median(v) / SCALE:.2f} mm ({len(v)} rows)" for k, v in sorted(rows_by.items())))
        out.append("   window rule against truth (W rows, T mm): tripped / holds by truth (wrong, right, no truth); median "
                   "delay of the wrong holds' trips (min)")
        for w in windows:
            for mm in mms:
                t_units = round(mm * SCALE / 1000)
                trips = collections.Counter()
                delays = []
                for h in hs:
                    t = first_trip(h, w, t_units)
                    if t is not None:
                        trips[h.audit] += 1
                        if h.audit == "wrong":
                            delays.append((t - h.t0) / 60.0)
                out.append(f"      W={w} T={mm:g}: " + ", ".join(f"{k} {trips[k]}/{len(by.get(k, []))}"
                                                            for k in sorted(by))
                           + f"; delay {median(delays):.1f}")
        wrong = sorted(by.get("wrong", []), key=lambda h: (h.t0, h.link))
        for h in wrong[: a.list]:
            nw = sum(1 for x in h.rows if x[4] == "wrong")
            out.append(f"      wrong: {h.link[0]} {h.link[1]} {h.link[2]}/{h.link[3]} ({h.n1}, {h.n2}) TOW {h.t0:.0f}..{h.t1:.0f}"
                       f" rows {h.n} (wrong {nw}) mean {1000 * h.mean:+.2f} mm one-sided {h.one:.2f}")
    sys.stdout.write("\n".join(out) + "\n")


# ------------------------------------------------------------------------------------------------------ selftest
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

    check(resid_units("-0.01436") == (-1436, True), "a residual in units of 0.01 mm")
    check(resid_units("") is None and resid_units("nan") is None, "an empty or non-number residual is dropped")
    check(resid_units("0.000001")[1] is False, "a residual with a sixth decimal is flagged")
    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g6_held.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        hs, inexact = load_holds("net", want_decided=True)
        check(inexact == 0, "no inexact residual in the fixture")
        by = {(h.link[0], h.link[1], h.t0): h for h in hs}
        check(len(hs) == 6, "six holds: an integer change and a gap over 120 s split two links")
        a = by.get(("AAAA00XXX", "E04", 30.0))
        check(a is not None and a.n == 130 and a.dur == 3870.0 and a.signature(8.0, 0.90, 3600.0),
              "a one-signed hold of 64.5 min with the signature")
        check(a is not None and abs(a.sigma - 0.12) < 1e-12 and abs(log_odds(a) - (1 - 0.2) / (2 * 0.0144)) < 1e-9,
              "its decision row, 30 s before the hold")
        check(a is not None and abs(a.mean - 0.009) < 1e-15, "the exact mean")
        check(a is not None and first_trip(a, 40, 800) == 1200.0, "the 40-row window reaches 8 mm at its 40th row")
        check(a is not None and first_trip(a, 40, 1000) is None, "and never reaches 10 mm")
        b = by.get(("AAAA00XXX", "E05", 30.0))
        check(b is not None and b.sum == 0 and b.dur >= 3600 and not b.signature(8.0, 0.90, 3600.0),
              "a long hold of mixed sign, residuals cancelling, has no signature")
        c = by.get(("BBBB00YYY", "E04", 30.0))
        check(c is not None and c.dur < 3600 and c.signature(8.0, 0.90, 0.0), "one-signed but short")
        d = by.get(("BBBB00YYY", "E04", 1830.0))
        check(d is not None and d.n1 == "11", "the integer change starts a new hold")
        check(("CCCC00ZZZ", "E09", 30.0) in by and ("CCCC00ZZZ", "E09", 3000.0) in by, "a gap over 120 s ends a hold")
        res, cases = resume_cases("net")
        check(set(res) == {("AAAA00XXX", "E04", 600), ("BBBB00YYY", "E04", 1830), ("CCCC00ZZZ", "E09", 2400)},
              "three resumptions")
        check(res[("AAAA00XXX", "E04", 600)]["signals"] == {"L1C", "L5Q"} and
              res[("AAAA00XXX", "E04", 600)]["class"] == {"clean_bridged"}, "one resumption from its two signal rows")
        cs = {(c["station"], c["tow"]): c for c in cases}
        check(set(cs) == {("AAAA00XXX", 600), ("BBBB00YYY", 1830)}, "two meet a held link; one meets none")
        x = cs.get(("BBBB00YYY", 1830))
        check(x is not None and x["cls"] == "shifted_fresh" and x["held_rows"] == 20 and abs(x["resid_m"] - 0.001) < 1e-12
              and x["end"] == {"kind": "release", "cause": "monitor", "delay_s": 30},
              "the shifted resumption, its folded rows and the release that followed")
        th = {(h.link[0], h.link[1], h.t0): h.audit for h in truth_holds("net", 0.0)}
        check(th.get(("AAAA00XXX", "E04", 30.0)) == "wrong" and th.get(("AAAA00XXX", "E05", 30.0)) == "right"
              and th.get(("CCCC00ZZZ", "E09", 30.0)) == "none", "holds graded by their rows' audit")
        tm = {(h.link[0], h.link[1], h.t0): h.audit for h in truth_holds("net", 0.0, "majority")}
        check(tm.get(("AAAA00XXX", "E04", 30.0)) == "wrong" and tm.get(("CCCC00ZZZ", "E09", 30.0)) == "none",
              "the majority rule")
        th2 = truth_holds("net", 1815.0)
        check(min(h.t0 for h in th2) >= 1815.0, "rows before 00:30:15 dropped")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("held_integrity.py")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    o = sub.add_parser("onesigned")
    o.add_argument("runs", nargs="+")
    o.add_argument("--view", choices=("legacy", "exact"), default="legacy")
    o.add_argument("--mm", type=float, default=8.0)
    o.add_argument("--share", type=float, default=0.90)
    o.add_argument("--min-hours", type=float, default=1.0)
    o.add_argument("--list", type=int, default=50)
    o.add_argument("--out", default="")
    w = sub.add_parser("window")
    w.add_argument("runs", nargs="+")
    w.add_argument("--view", choices=("legacy", "exact"), default="legacy")
    w.add_argument("--windows", default="20,40,80")
    w.add_argument("--mm", default="4,5,6,8,10")
    r = sub.add_parser("resume")
    r.add_argument("runs", nargs="+")
    r.add_argument("--list", type=int, default=20)
    r.add_argument("--out", default="")
    t = sub.add_parser("truth")
    t.add_argument("runs", nargs="+")
    t.add_argument("--from-s", type=float, default=1815.0)
    t.add_argument("--windows", default="20,40,80")
    t.add_argument("--mm", default="4,6,8,10")
    t.add_argument("--list", type=int, default=20)
    t.add_argument("--rule", choices=("any", "majority"), default="any")
    a = ap.parse_args()
    {"onesigned": cmd_onesigned, "window": cmd_window, "resume": cmd_resume, "truth": cmd_truth}[a.cmd](a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
