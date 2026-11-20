"""Truth on the simulator beds (card GRA, plan step G7; bars B5 and B6 of card NRF): the run-end truth lines read by kind,
the frame-free count of held Galileo chords, and the frame class of every wrong held chord and hold.

    py -3.14 scripts/grading/truth.py runend <run> [<run> ...] [--view kinds|shell]
    py -3.14 scripts/grading/truth.py cover <run> <truth bed> [--stride 10] [--order seed0|sorted] [--cover-out f.jsonl]
                                      [--cache DIR]
    py -3.14 scripts/grading/truth.py guard <off run>:<on run>:<truth bed> ... [--order seed0|sorted] [--cache DIR]
    py -3.14 scripts/grading/truth.py classes <run> <truth bed> [--stride 1] [--rule majority|any] [--order sorted|seed0]
                                      [--list N] [--out rows.jsonl]
    py -3.14 scripts/grading/truth.py --selftest

A truth bed is a simulator fixture folder (S*SIM/truth_ambiguities.csv and truth_windup.csv), given as a path or as a
name under the runs root.

runend   The engine writes three kinds of truth line into logs/main_log.log (RingFixSeams.cpp): the run's truth audit
         ("ring fix truth audit, in each epoch's frame": held link-epochs right, WRONG, without a true integer;
         decisions WRONG; branches without a wind-up row), the audit file's line ("ring fix audit, '<file>'", written
         only when network.ring_fix names an audit file) and one Truth clause per cascade family.  kinds (the
         default): each kind found, by name.  shell: byte for byte the grading shell's line (grade_c3.sh:16): the run,
         the last line containing "held link-epochs right" from its first "] " on, cut to 200 characters, and the
         epochs of products.txt; that rule takes the audit file's line when a run writes one (card GRA, B5).
cover    The frame-free count (frame_free_count.py of the grading folder, card NRF sections 103.3 and 108.4).  At every
         --stride-th epoch of the ring-fix trace (epochs in first-appearance order), every held Galileo chord is
         rebuilt on its fundamental cycle through the chain tree's targets of each band (the tree path found by a
         breadth-first search from the satellite), its error (d1, d2) against truth (the true integer plus the wind-up
         branch floor(wu(t0) + 0.5) at the arc's start, arcs from network_ionosphere.csv with a 300 s gap rule), and a
         greedy cover finds the fewest links whose pair must change by a multiple of the near-null pair (3, 4) for
         every held chord to agree with truth.  order seed0 (the legacy view): the candidate links in the order of a
         Python set under PYTHONHASHSEED=0, as the recorded covers were made (the script runs itself again under that
         seed when it is not set), so the printout and --cover-out equal frame_free_count.py's; order sorted: the
         candidates in sorted order, so a tie goes to the first link and multiple in that order whatever the seed
         (card GRA, assumption A30: two identical runs gave 545 and 510 under random seeds).  --cache keeps a cover by
         run, bed, stride, order and the sha256 of the run's ring-fix trace (card GRA, defect D6: the old cache was
         keyed by the run's tag alone).
guard    The truth and frame-free lines of grade_guard_sim.py for each pair, byte for byte under seed0: the truth
         audit of the guarded run against its control, and the cover's links summed over the sampled epochs per family.
classes  The frame class of every held chord-epoch the audit column calls wrong, at every --stride-th epoch (1: every
         epoch), by the rules the package held-signature-evidence wrote before any case was looked at
         (gates/day_program_2026-11-19/packages/held-signature-evidence/parts/sim-power-by-class.md, 09:14):
           F  frame-type: (d1, d2) a non-zero multiple of (3, 4) that the cover reduces through a tree edge (or through
              a link at least two wrong held chords pass); also (0, 0) while the audit says wrong
           I  isolated: a non-zero multiple of (3, 4) that the cover reduces only through the chord's own link, which no
              other wrong held chord passes
           O  another shape: (d1, d2) non-zero and not a multiple of (3, 4)
           U  no cycle in both bands, no truth or branch, a cover that leaves the error, or a GPS chord (not rebuilt)
         Each wrong hold (held_integrity.py's holds, graded by --rule) takes the class of the majority of its
         classified rows (F, I, O), a tie going to I, then O, then F; with none, unclassified.  Printed per run: the
         wrong chord-epochs by shape and class, the chord-epochs the rebuild finds off where the audit says right, the
         wrong holds by class, the links behind them (the cover link each hold's rows were reduced through most), and
         every isolated wrong hold with its residual (mean, one-sided share, rows, hours).
"""
import argparse
import collections
import csv
import glob
import hashlib
import json
import math
import os
import re
import subprocess
import sys

import runs

SIG2 = {"L1X": "L5X", "L1C": "L5Q"}
TRUTH = re.compile(r"Network \(ring fix truth audit, in each epoch's frame\): held link-epochs right (\d+), WRONG (\d+), "
                   r"without a true integer (\d+); decisions WRONG (\d+); (\d+) branch")
AUDIT_FILE = re.compile(r"Network \(ring fix audit, '([^']*)'\): decisions right (\d+), WRONG (\d+); held link-epochs "
                        r"right (\d+), WRONG (\d+), not listed (\d+)")
CASCADE = re.compile(r"Network \(ring fix, cascade (\S+) against (\S+)\):.*Truth: held link-epochs right (\d+), WRONG (\d+);"
                     r" decisions WRONG (\d+)")


# ------------------------------------------------------------------------------------------------------------ runend
def runend_kinds(run):
    """{"truth": (right, wrong, none, decisions wrong, branches without wind-up), "audit_file": (...), "cascade":
    [(family, reference, right, wrong, decisions wrong)]}: the last line of each kind (the cascade lines all kept)."""
    out = {"cascade": []}
    path = os.path.join(runs.run_dir(run), "logs", "main_log.log")
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = TRUTH.search(ln)
        if m:
            out["truth"] = tuple(int(x) for x in m.groups())
        m = AUDIT_FILE.search(ln)
        if m:
            out["audit_file"] = (m.group(1),) + tuple(int(x) for x in m.groups()[1:])
        m = CASCADE.search(ln)
        if m:
            out["cascade"].append(m.groups()[:2] + tuple(int(x) for x in m.groups()[2:]))
    return out


def runend_shell(run, tag):
    """grade_c3.sh:16 byte for byte: tag, the last line holding "held link-epochs right" from its first "] " on (sed
    's/^.*\\] //' is greedy: from the last "] "), cut to 200 characters, and the products' epoch count."""
    last = ""
    for ln in open(os.path.join(runs.run_dir(run), "logs", "main_log.log"), encoding="utf-8", errors="replace"):
        if "held link-epochs right" in ln:
            last = ln.rstrip("\r\n")
    text = re.sub(r"^.*\] ", "", last)
    text = text.encode("utf-8")[:200].decode("utf-8", errors="ignore")
    n = sum(1 for ln in open(os.path.join(runs.run_dir(run), "products.txt"), encoding="utf-8", errors="replace")
            if ln.startswith("E,"))
    return f"{tag}: {text} | epochs {n}"


def cmd_runend(a):
    for run in a.runs:
        if a.view == "shell":                      # the shell wrote LF line ends, as bash does
            sys.stdout.flush()
            sys.stdout.buffer.write((runend_shell(run, run) + "\n").encode("utf-8"))
            sys.stdout.buffer.flush()
            continue
        k = runend_kinds(run)
        t = k.get("truth")
        print(f"== {run}: truth audit: " + (f"held link-epochs right {t[0]}, WRONG {t[1]}, without a true integer {t[2]}; "
                                            f"decisions WRONG {t[3]}; branches without a wind-up row {t[4]}"
                                            if t else "no line"))
        f = k.get("audit_file")
        print("   audit file: " + (f"'{f[0]}': decisions right {f[1]}, WRONG {f[2]}; held link-epochs right {f[3]}, "
                                   f"WRONG {f[4]}, not listed {f[5]}" if f else "no line (no audit file named)"))
        for c in k["cascade"]:
            print(f"   cascade {c[0]} against {c[1]}: held link-epochs right {c[2]}, WRONG {c[3]}; decisions WRONG {c[4]}")


# ------------------------------------------------------------------------------------------------------------- cover
def bed_dir(bed):
    return os.path.abspath(bed) if os.path.isdir(bed) else os.path.join(runs.runs_root(), bed)


def read_truth(bed):
    truth_n, windup = {}, collections.defaultdict(dict)
    for d in glob.glob(bed_dir(bed) + "/S*SIM"):
        sta = os.path.basename(d)
        for r in csv.DictReader(open(d + "/truth_ambiguities.csv")):
            if r["satellite"].startswith("E"):
                truth_n[(sta, r["satellite"], r["signal"])] = int(r["N_int"])
        for r in csv.DictReader(open(d + "/truth_windup.csv")):
            if r["satellite"].startswith("E"):
                windup[(sta, r["satellite"])][round(float(r["tow_s"]))] = float(r["windup_cycles"])
    return truth_n, windup


def read_run(run, stride, keep_audit=False):
    """(held {t: [(sta, sat, sig1, n1, n2[, audit])]}, tree {t: {band: [(sta, sat, target)]}}, arcs {(sta, sat):
    [starts]}, wrong_g {t: [(sta, sat, sig1)]}) at the sampled epochs (frame_free_count.py's reading)."""
    held, epochs, seen = collections.defaultdict(list), [], set()
    wrong_g = collections.defaultdict(list)
    header, it = runs.raw_rows(run, "network_ring_fix.csv")
    ix = {k: i for i, k in enumerate(header)}
    itow, ista, isat, is1, ist, in1, in2 = (ix[k] for k in ("tow", "station", "sat", "sig1", "state", "n1", "n2"))
    iaud = ix.get("audit")
    for r in it:
        t = round(float(r[itow]))
        if t not in seen:
            seen.add(t)
            epochs.append(t)
        if (len(epochs) - 1) % stride or r[ist] != "held":
            continue
        if not r[isat].startswith("E"):
            if keep_audit and iaud is not None and r[iaud] == "wrong":
                wrong_g[t].append((r[ista], r[isat], r[is1]))
            continue
        row = (r[ista], r[isat], r[is1], int(r[in1]), int(r[in2]))
        held[t].append(row + ((r[iaud] if iaud is not None else ""),) if keep_audit else row)
    need = set(held)
    tree = collections.defaultdict(lambda: collections.defaultdict(list))
    header, it = runs.raw_rows(run, "network_chain_edges.csv")
    ix = {k: i for i, k in enumerate(header)}
    for r in it:
        if r[ix["system"]] != "E":
            continue
        t = round(float(r[ix["tow"]]))
        if t in need:
            tree[t][r[ix["band"]]].append((r[ix["edge_sta"]], r[ix["edge_sat"]], float(r[ix["target_cyc"]])))
    arcs, last = collections.defaultdict(list), {}
    header, it = runs.raw_rows(run, "network_ionosphere.csv")
    ix = {k: i for i, k in enumerate(header)}
    for r in it:
        if not r[ix["sat"]].startswith("E"):
            continue
        t = round(float(r[ix["tow"]]))
        key = (r[ix["station"]], r[ix["sat"]])
        if key not in last or t - last[key] > 300.0:
            arcs[key].append(t)
        last[key] = t
    return held, tree, arcs, wrong_g


def branch(arcs, windup, sta, sat, t):
    starts = [s for s in arcs.get((sta, sat), []) if s <= t]
    if not starts:
        return None
    w = windup.get((sta, sat), {})
    return math.floor(w[starts[-1]] + 0.5) if starts[-1] in w else None


class Tree:
    """One band's tree at one epoch; the breadth-first search from each satellite is kept (frame_free_count.py's search
    stops at the station, but the predecessors it assigns up to there are those of the full search)."""

    def __init__(self, edges):
        self.adj = collections.defaultdict(list)
        for s, k, tg in edges:
            self.adj[("S", s)].append((("K", k), tg))
            self.adj[("K", k)].append((("S", s), tg))
        self.prev = {}

    def cycle(self, sta, sat):
        start, goal = ("K", sat), ("S", sta)
        if start not in self.prev:
            prev = {start: None}
            q = collections.deque([start])
            while q:
                u = q.popleft()
                for v, tg in self.adj[u]:
                    if v not in prev:
                        prev[v] = (u, tg)
                        q.append(v)
            self.prev[start] = prev
        prev = self.prev[start]
        if goal not in prev:
            return None
        path, v = [], goal
        while prev[v] is not None:
            u, tg = prev[v]
            path.append((v[1] if v[0] == "S" else u[1], v[1] if v[0] == "K" else u[1], tg))
            v = u
        path.reverse()
        out, c = [(sta, sat, 1, None)], -1
        for s, k, tg in path:
            out.append((s, k, c, tg))
            c = -c
        return out


def chord_errors(rows, sig1, e1, e2, truth_n, windup, arcs, t):
    """{chord: (d1, d2, kk or None, {link: coef})} for the held chords of one family at one epoch; kk is the multiple of
    (3, 4) (0 for (0, 0)); a chord with no cycle in both bands, no truth or no branch is left out."""
    s2 = SIG2[sig1]
    out = {}
    for row in rows:
        sta, sat, s, n1, n2 = row[:5]
        if s != sig1:
            continue
        cy1, cy2 = e1.cycle(sta, sat), e2.cycle(sta, sat)
        if cy1 is None or cy2 is None or [x[:3] for x in cy1] != [x[:3] for x in cy2]:
            continue
        f1 = f2 = 0.0
        ok = True
        for (ls, lk, cf, tg1), (_, _, _, tg2) in zip(cy1, cy2):
            k = branch(arcs, windup, ls, lk, t)
            if k is None or (ls, lk, sig1) not in truth_n or (ls, lk, s2) not in truth_n:
                ok = False
                break
            f1 += cf * (truth_n[(ls, lk, sig1)] + k) - (cf * tg1 if tg1 is not None else 0.0)
            f2 += cf * (truth_n[(ls, lk, s2)] + k) - (cf * tg2 if tg2 is not None else 0.0)
        if not ok:
            continue
        d1, d2 = n1 - round(f1), n2 - round(f2)
        kk = d1 // 3 if (d1 % 3 == 0 and d2 == 4 * (d1 // 3)) else None
        if kk is None and (d1, d2) == (0, 0):
            kk = 0
        out[(sta, sat)] = (d1, d2, kk, {(ls, lk): cf for ls, lk, cf, _ in cy1})
    return out


def greedy(chords, order):
    """The greedy cover of frame_free_count.py over chords {c: (kk, {link: coef})}: (steps [(link, multiple)],
    residual {c: kk left}, used {c: [links that changed it]})."""
    r = {c: k for c, (k, _) in chords.items()}
    through = collections.defaultdict(list)
    for c, (_, links) in chords.items():
        for l, cf in links.items():
            through[l].append((c, cf))
    steps, used = [], collections.defaultdict(list)
    while any(r.values()):
        best = None
        cand = {l for c, v in r.items() if v for l in chords[c][1]}
        if order == "sorted":
            cand = sorted(cand)
        for l in cand:
            for m in (1, -1, 2, -2):
                score = sum(abs(r[c]) - abs(r[c] - cf * m) for c, cf in through[l])
                if best is None or score > best[0]:
                    best = (score, l, m)
        if best is None or best[0] <= 0:
            break
        _, l, m = best
        for c, cf in through[l]:
            r[c] -= cf * m
            used[c].append(l)
        steps.append((l, m))
    return steps, r, used


def need_seed0(order):
    """Run again under PYTHONHASHSEED=0 when the seed0 order is asked for and the seed is not 0; returns the exit
    code to leave with, or None to go on."""
    if order != "seed0" or os.environ.get("PYTHONHASHSEED") == "0":
        return None
    env = dict(os.environ, PYTHONHASHSEED="0")
    p = subprocess.run([sys.executable] + sys.argv, env=env)
    return p.returncode


def compute_cover(run, bed, stride, order):
    """(per_epoch [(t, sig1, chords, wrong, n)], cover_rows [(t, sig1, sta, sat, m)], chosen Counter, shapes Counter)."""
    truth_n, windup = read_truth(bed)
    held, tree, arcs, _ = read_run(run, stride)
    per_epoch, cover_rows, chosen, shapes = [], [], collections.Counter(), collections.Counter()
    for t in sorted(held):
        for sig1 in sorted({h[2] for h in held[t]}):
            e1, e2 = Tree(tree[t][sig1]), Tree(tree[t][SIG2[sig1]])
            ce = chord_errors(held[t], sig1, e1, e2, truth_n, windup, arcs, t)
            for d1, d2, kk, _ in ce.values():
                if (d1, d2) != (0, 0):
                    shapes[(d1, d2)] += 1
            chords = {c: (kk, links) for c, (_, _, kk, links) in ce.items() if kk is not None}
            wrong0 = sum(1 for k, _ in chords.values() if k)
            steps, _, _ = greedy(chords, order)
            for l, m in steps:
                chosen[l] += 1
                cover_rows.append((t, sig1, l[0], l[1], m))
            per_epoch.append((t, sig1, len(chords), wrong0, len(steps)))
    return per_epoch, cover_rows, chosen, shapes


def cover_cached(run, bed, stride, order, cache):
    """The cover rows of a run, from --cache when a file of the same run, bed, stride, order and ring-fix sha256
    exists; the printout's other parts are not cached (None)."""
    if not cache:
        return compute_cover(run, bed, stride, order)
    h = hashlib.sha256()
    with open(runs.trace_path(run, "network_ring_fix.csv"), "rb") as fh:
        for b in iter(lambda: fh.read(1 << 22), b""):
            h.update(b)
    name = f"cover_{order}_{os.path.basename(runs.run_dir(run))}_{os.path.basename(bed_dir(bed))}_s{stride}_" \
           f"{h.hexdigest()[:16]}.jsonl"
    path = os.path.join(cache, name)
    if os.path.isfile(path):
        rows = [tuple(json.loads(ln)) for ln in open(path)]
        return None, rows, None, None
    res = compute_cover(run, bed, stride, order)
    os.makedirs(cache, exist_ok=True)
    with open(path, "w") as fh:
        for row in res[1]:
            fh.write(json.dumps(row) + "\n")
    return res


def cmd_cover(a):
    code = need_seed0(a.order)
    if code is not None:
        return code
    per_epoch, cover_rows, chosen, shapes = compute_cover(a.run, a.bed, a.stride, a.order)
    if a.cover_out:
        with open(a.cover_out, "w") as fh:
            for row in cover_rows:
                fh.write(json.dumps(row) + "\n")
    out = [f"== {a.run} (every {a.stride}th epoch; truth {a.bed})"]
    if shapes:
        out.append("   held errors by shape (d1, d2): " + ", ".join(f"{k} {v}" for k, v in shapes.most_common(6)))
    for f in sorted({p[1] for p in per_epoch}):
        P = [p for p in per_epoch if p[1] == f]
        ww = [p for p in P if p[3]]
        out.append(f"   {f}: {len(P)} sampled epochs; held chords per epoch {sum(p[2] for p in P) / len(P):.0f}; epochs "
                   f"with a wrong held chord {len(ww)}; wrong held chords per such epoch: mean "
                   f"{sum(p[3] for p in ww) / max(1, len(ww)):.1f}, max {max([p[3] for p in ww] or [0])}; links a cycle "
                   f"off (greedy cover) per such epoch: mean {sum(p[4] for p in ww) / max(1, len(ww)):.2f}, max "
                   f"{max([p[4] for p in ww] or [0])}; distribution {sorted(collections.Counter(p[4] for p in ww).items())}")
    out.append("   links chosen most often: " + ", ".join(f"{l[0]}-{l[1]} {v}" for l, v in chosen.most_common(8)))
    sys.stdout.write("\n".join(out) + "\n")
    return 0


def cmd_guard(a):
    code = need_seed0(a.order)
    if code is not None:
        return code
    for spec in a.pairs:
        off, on, bed = spec.split(":")
        k_off, k_on = runend_kinds(off).get("truth"), runend_kinds(on).get("truth")
        fam = {}
        for tag in (off, on):
            fam[tag] = collections.Counter()
            for t, sig1, sta, sat, m in cover_cached(tag, bed, 10, a.order, a.cache)[1]:
                fam[tag][sig1] += abs(m)
        out = [f"== {on} against {off} (truth {bed})"]
        out.append(f"   truth audit: held link-epochs WRONG {k_on[1] if k_on else None} against {k_off[1] if k_off else None}; "
              f"right {k_on[0] if k_on else None} against {k_off[0] if k_off else None}; decisions WRONG "
              f"{k_on[3] if k_on else None} against {k_off[3] if k_off else None}")
        ca, cb = fam[off], fam[on]
        out.append("   frame-free cover (links a cycle off, summed over the sampled epochs): " + ", ".join(
            f"{f} {cb.get(f, 0)} against {ca.get(f, 0)}" for f in sorted(set(ca) | set(cb) | {"L1C", "L1X"})))
        sys.stdout.flush()                       # LF line ends, as the recorded grades file has them
        sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()
    return 0


# ----------------------------------------------------------------------------------------------------------- classes
def classify(run, bed, stride, order):
    """(rows, rebuilt_off_right): one record per held chord-epoch the audit calls wrong at the sampled epochs:
    (t, sta, sat, sig1, d1, d2, class, cover links, private), private true for a frame-type row reduced only through
    links no other held chord passes (the cover cannot tell such a link's error from the chord's own), and the count of
    chord-epochs the rebuild finds off where the audit says right."""
    truth_n, windup = read_truth(bed)
    held, tree, arcs, wrong_g = read_run(run, stride, keep_audit=True)
    rows, off_right = [], 0
    for t in sorted(held):
        for sig1 in sorted({h[2] for h in held[t]}):
            e1, e2 = Tree(tree[t][sig1]), Tree(tree[t][SIG2[sig1]])
            fam_rows = [h for h in held[t] if h[2] == sig1]
            audit = {(h[0], h[1]): h[5] for h in fam_rows}
            ce = chord_errors(fam_rows, sig1, e1, e2, truth_n, windup, arcs, t)
            chords = {c: (kk, links) for c, (_, _, kk, links) in ce.items() if kk is not None}
            _, resid, used = greedy(chords, order)
            wrong_links, all_links = collections.Counter(), collections.Counter()
            for c, (kk, links) in chords.items():
                for l in links:
                    all_links[l] += 1
                    if audit.get(c) == "wrong":
                        wrong_links[l] += 1
            for c, x in ce.items():
                if audit.get(c) != "wrong" and (x[0], x[1]) != (0, 0):
                    off_right += 1
            for (sta, sat), aud in audit.items():
                if aud != "wrong":
                    continue
                c = (sta, sat)
                if c not in ce:
                    rows.append((t, sta, sat, sig1, None, None, "U", [], False))
                    continue
                d1, d2, kk, links = ce[c]
                if (d1, d2) == (0, 0):
                    cls = "F"
                elif kk is None:
                    cls = "O"
                elif resid.get(c):
                    cls = "U"
                else:
                    ls = set(used.get(c, []))
                    cls = "F" if any(l != c or wrong_links[l] >= 2 for l in ls) else "I"
                ls = sorted(set(used.get(c, [])))
                private = cls == "F" and bool(ls) and all(all_links[l] == 1 for l in ls)
                rows.append((t, sta, sat, sig1, d1, d2, cls, ls, private))
    for t, lst in wrong_g.items():
        for sta, sat, sig1 in lst:
            rows.append((t, sta, sat, sig1, None, None, "U", [], False))
    return rows, off_right


def hold_class(classes):
    """The class of the majority of a hold's classified rows (F, I, O), a tie going to I, then O, then F."""
    c = collections.Counter(x for x in classes if x in ("F", "I", "O"))
    if not c:
        return "unclassified"
    top = max(c.values())
    for k in ("I", "O", "F"):
        if c[k] == top:
            return k


def cmd_classes(a):
    code = need_seed0(a.order)
    if code is not None:
        return code
    import held_integrity as hi
    rows, off_right = classify(a.run, a.bed, a.stride, a.order)
    hs = hi.truth_holds(a.run, 0.0, a.rule)
    where = {}
    for h in hs:
        for x in h.rows:
            where[(h.link[0], h.link[1], h.link[2], round(x[0]))] = h
    per_hold = collections.defaultdict(list)
    unplaced = 0
    for t, sta, sat, sig1, d1, d2, cls, links, private in rows:
        h = where.get((sta, sat, sig1, t))
        if h is None:
            unplaced += 1
            continue
        per_hold[id(h)].append((cls, links, private))
    wrong = [h for h in hs if h.audit == "wrong"]
    out = [f"== {a.run} (truth {a.bed}; every {a.stride}th epoch; cover order {a.order}; holds by the rule '{a.rule}')"]
    shp = collections.Counter((r[4], r[5]) for r in rows if r[4] is not None)
    n_priv = sum(1 for r in rows if r[8])
    out.append(f"   held chord-epochs the audit calls wrong at the sampled epochs: {len(rows)}; by shape (d1, d2): "
               + ", ".join(f"{k} {v}" for k, v in shp.most_common()) + "; by class: "
               + ", ".join(f"{k} {v}" for k, v in sorted(collections.Counter(r[6] for r in rows).items())))
    out.append(f"   frame-type chord-epochs reduced only through links no other held chord passes (frame or isolated, not "
               f"told apart): {n_priv}; chord-epochs the rebuild finds off where the audit says right: {off_right}; wrong "
               f"chord-epochs in no hold: {unplaced}")
    hc = {}
    for h in wrong:
        rr = per_hold.get(id(h), [])
        hc[id(h)] = hold_class([c for c, _, _ in rr])
    tally = collections.Counter(hc.values())
    flag_i = sum(1 for h in wrong if any(c == "I" for c, _, _ in per_hold.get(id(h), [])))
    flag_o = sum(1 for h in wrong if any(c == "O" for c, _, _ in per_hold.get(id(h), [])))
    priv = sum(1 for h in wrong if hc[id(h)] == "F" and all(p for c, _, p in per_hold.get(id(h), []) if c == "F"))
    out.append(f"   wrong holds: {len(wrong)}; by class: " + ", ".join(f"{k} {v}" for k, v in sorted(tally.items()))
               + f" (with an isolated row {flag_i}, with another shape {flag_o}; frame-type holds whose every frame-type "
               f"row was reduced only through links no other held chord passes {priv})")
    behind = collections.defaultdict(list)
    for h in wrong:
        cnt = collections.Counter(l for _, ls, _ in per_hold.get(id(h), []) for l in ls)
        if cnt:
            top = max(cnt.values())
            link = min(l for l, v in cnt.items() if v == top)
            behind[(link, "own link" if link == h.link[:2] else "tree edge or shared link")].append(h)
    out.append(f"   links behind the wrong holds (the cover link a hold's rows were reduced through most): {len(behind)}; "
               + ", ".join(f"{l[0]}-{l[1]} ({kind}) {len(v)}" for (l, kind), v in
                           sorted(behind.items(), key=lambda kv: (-len(kv[1]), kv[0]))[: a.list]))
    iso = sorted([h for h in wrong if hc[id(h)] == "I"], key=lambda h: (h.t0, h.link))
    out.append(f"   isolated wrong holds: {len(iso)}")
    for h in iso[: a.list]:
        r = sorted(abs(x[3]) for x in h.rows)
        out.append(f"      {h.link[0]} {h.link[1]} {h.link[2]}/{h.link[3]} ({h.n1}, {h.n2}) TOW {h.t0:.0f}..{h.t1:.0f} "
                   f"({h.dur / 3600:.2f} h, {h.n} rows, wrong {sum(1 for x in h.rows if x[4] == 'wrong')}): mean residual "
                   f"{1000 * h.mean:+.2f} mm, one-sided {h.one:.2f}, |residual| median "
                   f"{1000 * r[len(r) // 2] / hi.SCALE:.2f} mm, signature at 8 mm "
                   f"{'yes' if h.signature(8.0, 0.90, 3600.0) else 'no'}")
    sys.stdout.write("\n".join(out) + "\n")
    if a.out:
        with open(a.out, "w") as fh:
            for r in rows:
                fh.write(json.dumps(list(r[:7]) + [[list(x) for x in r[7]], r[8]]) + "\n")
    return 0


# ---------------------------------------------------------------------------------------------------------- selftest
def selftest():
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g7_truth.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        k = runend_kinds("bed_run")
        check(k.get("truth") == (100, 7, 3, 2, 0), "the truth audit line by kind")
        check(k.get("audit_file") == ("a.csv", 5, 1, 90, 2, 4), "the audit file's line by kind")
        check(len(k["cascade"]) == 1 and k["cascade"][0][2:] == (40, 1, 0), "the cascade's Truth clause")
        check(runend_shell("bed_run", "x").startswith("x: Network (ring fix audit, 'a.csv')"),
              "the shell rule takes the last line holding the phrase: the audit file's")
        # a three-station, two-satellite bed: the tree S1-E01, S1-E02, S2-E01, S3-E01; chords S2-E02 and S3-E02.
        per_epoch, cover_rows, chosen, shapes = compute_cover("bed_run", "bed", 1, "sorted")
        check([p[3] for p in per_epoch] == [0, 2], "the first epoch right, the second with two wrong chords")
        check(cover_rows == [(30, "L1C", "S1AAAASIM", "E02", -1)], "one tree edge a near-null step off covers both chords")
        check(shapes == collections.Counter({(3, 4): 2}), "both wrong chords of shape (3, 4)")
        rows, off_right = classify("bed_run", "bed", 1, "sorted")
        check(sorted((r[1], r[6], r[8]) for r in rows) == [("S2BBBBSIM", "F", False), ("S3CCCCSIM", "F", False)]
              and off_right == 0, "the two wrong chords are frame-type, through a shared tree edge")
        rows2, _ = classify("bed_iso", "bed", 1, "sorted")
        check([(r[1], r[2], r[6]) for r in rows2] == [("S2BBBBSIM", "E02", "I")], "a chord alone off is isolated")
        check(hold_class(["F", "I"]) == "I" and hold_class(["F", "F", "O"]) == "F" and hold_class(["U"]) == "unclassified",
              "a hold's class: the majority, ties to I then O then F")
    chords = {("a", "x"): (1, {("a", "x"): 1, ("t", "x"): -1}), ("b", "x"): (1, {("b", "x"): 1, ("t", "x"): -1})}
    steps, resid, used = greedy(chords, "sorted")
    check(steps == [(("t", "x"), -1)] and not any(resid.values()), "the greedy cover takes the shared link")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("truth.py")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("runend")
    r.add_argument("runs", nargs="+")
    r.add_argument("--view", choices=("kinds", "shell"), default="kinds")
    c = sub.add_parser("cover")
    c.add_argument("run")
    c.add_argument("bed")
    c.add_argument("--stride", type=int, default=10)
    c.add_argument("--order", choices=("seed0", "sorted"), default="seed0")
    c.add_argument("--cover-out", default="")
    g = sub.add_parser("guard")
    g.add_argument("pairs", nargs="+")
    g.add_argument("--order", choices=("seed0", "sorted"), default="seed0")
    g.add_argument("--cache", default="")
    k = sub.add_parser("classes")
    k.add_argument("run")
    k.add_argument("bed")
    k.add_argument("--stride", type=int, default=1)
    k.add_argument("--order", choices=("sorted", "seed0"), default="sorted")
    k.add_argument("--rule", choices=("majority", "any"), default="majority")
    k.add_argument("--list", type=int, default=30)
    k.add_argument("--out", default="")
    a = ap.parse_args()
    return {"runend": cmd_runend, "cover": cmd_cover, "guard": cmd_guard, "classes": cmd_classes}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
