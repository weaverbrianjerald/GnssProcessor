#!/usr/bin/env python3
"""Frame census of the eq34 chain on the float vehicle (card IRD, step IRD-2).

Three graders on the chain traces of a run (network.trace_chain_datum writes
logs/network_chain_edges.csv and logs/network_chain_frame.csv; the wire graders read
logs/network_pb_prehold.csv and products.txt):

  stop  <run>              the static-algebra stop rule (card IRD section 10)
  cert  <run> <fixture>    truth correctness of the would-be certifications (section 10) and of
                           the re-host rows
  floor <run> <fixture>    the event-free floor of the W bars (sections 9 and 10, calibration rule)

Every mode prints its sample sizes before any verdict.  --json writes the result.  --selftest runs
synthetic traces with hand-computed answers.

The frame (card IRD section 3)
------------------------------
A group is (system, band).  Each epoch holds one tree per component of the bipartite graph of
stations and satellites.  A tree edge is an arc (station, satellite) pinned at an integer target
k_e (column target_cyc); the root receiver's phase bias is pinned as well.  In that frame the
absolute ambiguity of every arc is N~ = N + z_r - z^s, z a number per vertex (an integer when the
frame is integer-recovering, a real number in the as-built chain).  For any closed walk of arcs with
coefficient c = +1 for an arc traversed station to satellite and c = -1 satellite to station (the
producer's arcgraph::treePathCycle), the z terms telescope away:

    sum c N~  =  sum c N_true

The fundamental cycle of a chord is the chord (c = +1) closed by the tree path from its satellite
back to its station.  Tree arcs sit at their targets, so the chord's true integer in the frame is

    N~_chord  =  N_true(chord) + sum over the path arcs of c (N_true(arc) - k_arc)

with N_true the simulator's slip-accumulated integer of the (station, satellite, band) plus the
producer's wind-up branch of that arc (scripts/simnet_producer_ar_grade.py: N_producer = N_sim + k,
k = round(wu_sim at the arc's first epoch)).  The truth is taken in the frame of the epoch the
chord row was written at: the tree and the targets of that epoch.

Event-free
----------
A chord-epoch is event-free in a band when, in every epoch of the trailing window (default 30 min)
that the trace covers, every arc on the chord's tree path was a tree edge with the same parent and
child and the same target_cyc, and the chord row existed.  Runs of identical tree-edge signatures
are tracked per arc over consecutive epochs of the edge trace; the path of the current epoch is
the path of every epoch of the window exactly when all its arcs have such a run reaching back to
the window start (a tree has one path between two vertices).  A satellite pair is event-free in a
band under the same rule on the pair's tree path (the path between the two satellite vertices).
By default the window must lie inside the trace (the first quiet_min of a run cannot qualify);
--short-window lets it be truncated at the first epoch of the trace.  --strict-rows also ends a run
at a TreeEdgeSlip, RootHandover, Split or Merge row on an unchanged edge (the card counts them as
events; they change neither the parent and child nor the target).

Checks that accompany the cert grade, because a wrong integer is only wrong when the formula is right
  - formula check: a chord whose tree path has three arcs closes a four-cycle, an ordinary double
    difference of absolute floats.  float_cyc + sum(c k) is then the DD float the producer forms in
    logs/network_producer_ar.csv, and the two agree to the trace's rounding whatever the float's
    quality (the tree arcs are hard pins).  The line reports the matched cycles and the largest gap.
  - float error: float_cyc - N~ (unwrapped) on chords present in the table for at least --settle-min.
    This is the float's own error against the frame truth, large when the float is far from integer.
  - frame-step check: at consecutive epochs of one chord where the true frame integer moved by one
    cycle or more, the float's step against the formula's step (the float follows a frame event when
    the two agree).

Reading choices that the card leaves open (printed again in the headers of the output)
  - event-free is evaluated per band; the joint line requires both bands;
  - the chord table row at tow t belongs to the trees of epoch t (the producer writes it after the
    solve of t; the re-host certification at t reads the table of the previous observed epoch);
  - "p50" is the median and "p90"/"p99" numpy's linear-interpolation percentiles of |wrap|, the
    fraction wrapped to [-0.5, 0.5] by simnet_wire_integer_consistency.wrapf;
  - the share clauses of the W bars (W-WL: 90 % within 0.20; W-NL: 85 % within 0.25) are reported;
    the calibration rule is stated for magnitudes, and an equivalent quantile form (p90 <= 0.20,
    p85 <= 0.25) is printed as an extension marked as such.

Usage:
    python scripts/ird_frame_census.py stop  <run> [--quiet-min 30] [--bar 0.02] [--systems G]
        [--short-window] [--strict-rows] [--json out.json]
    python scripts/ird_frame_census.py cert  <run> <fixture> [--systems G] [--no-windup-branch]
        [--max-list 20] [--settle-min 60] [--json out.json]
    python scripts/ird_frame_census.py floor <run> <fixture> [--source prehold|products]
        [--systems G] [--bands L1C,L2W] [--quiet-min 30] [--short-window] [--strict-rows]
        [--start-hms HH:MM:SS | --start-tow T] [--hours H] [--min-n 200] [--json out.json]
    python scripts/ird_frame_census.py --selftest
"""
import argparse
import csv
import json
import os
import sys
import tempfile
from collections import Counter, defaultdict, namedtuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_wire_integer_consistency as WIC   # noqa: E402  (the one owner of the wire algebra)
import simnet_producer_ar_grade as PAG          # noqa: E402  (the wind-up branch of each arc)
from simnet_amb_integer import n_at             # noqa: E402  (the slip-accumulated truth integer)

EDGES_REL = os.path.join("logs", "network_chain_edges.csv")
FRAME_REL = os.path.join("logs", "network_chain_frame.csv")
AR_REL = os.path.join("logs", "network_producer_ar.csv")
AR_AGREE_CYC = 1e-3          # formula check: the DD floats agree when the largest gap is below this
EDGE_COLUMNS = ("week", "tow", "system", "family", "band", "comp", "r1", "edge_sta", "edge_sat",
                "child", "parent", "depth", "target_m", "target_cyc", "row", "certified",
                "cert_source", "flipped", "returning")
FRAME_COLUMNS = ("week", "tow", "family", "arc_sta", "arc_sat", "float_cyc_1", "float_cyc_2",
                 "q11", "q12", "q22", "int_1", "int_2", "source", "conflict", "lattice_int")
REHOST_ROWS = ("RehostCertified", "RehostUncertified")
STRICT_BREAK_ROWS = ("TreeEdgeSlip", "RootHandover", "Split", "Merge")    # --strict-rows

EPS_S = 1e-3                 # tow tolerance (seconds)
QUIET_MIN_DEFAULT = 30.0     # minutes
STOP_BAR_CYC = 0.02          # card IRD section 10: per-band p50 |frac| of event-free chords
INT_TOL = 1e-6               # a target within this of an integer is an integer

# Card IRD section 9 (frozen bars) and the calibration rule: F <= 0.8 B stands, else the bar
# becomes 1.25 F and B is MISSED; fewer than 200 pair-epochs: B stands.
CAL_MIN_N = 200
CAL_STAND = 0.8
CAL_WIDEN = 1.25
W_BARS = (
    # (bar, channel, statistic, B)
    ("W-micro", "E1", "p50", 0.02), ("W-micro", "E1", "p99", 0.10),
    ("W-micro", "E2", "p50", 0.02), ("W-micro", "E2", "p99", 0.10),
    ("W-micro", "WL", "p50", 0.02), ("W-micro", "WL", "p99", 0.10),
    ("W-WL", "WL", "p50", 0.08),
    ("W-NL", "NL", "p50", 0.10),
)
# The share clauses as bars on a quantile: "at least 90 % within 0.20" is p90 <= 0.20.
W_SHARE_BARS = (("W-WL", "WL", 90.0, 0.20), ("W-NL", "NL", 85.0, 0.25))
CHANNEL_NAME = {"E1": "E_band1", "E2": "E_band2", "WL": "D_WL", "NL": "g (NL)"}


# --------------------------------------------------------------------------- #
# the traces
# --------------------------------------------------------------------------- #
class Edge(namedtuple("Edge", "tow system family band comp r1 sta sat child parent depth target "
                              "target_m row certified src flipped returning")):
    """One row of network_chain_edges.csv: a tree edge (arc sta-sat) of group (system, band)."""
    __slots__ = ()

    @property
    def arc(self):
        return (self.sta, self.sat)


class Chord(namedtuple("Chord", "tow system b1 b2 sta sat f1 f2 q11 q22 int1 int2 source "
                                "conflict lat1 lat2")):
    """One row of network_chain_frame.csv: a chord of both bands' trees (arc sta-sat)."""
    __slots__ = ()

    def band(self, k):
        return self.b1 if k == 1 else self.b2

    def flt(self, k):
        return self.f1 if k == 1 else self.f2

    def det(self, k):
        return self.int1 if k == 1 else self.int2

    @property
    def key(self):
        return (self.system, self.b1, self.b2, self.sta, self.sat)


def _rows(path, columns):
    """Yield (row list, column index) of a trace csv; comment lines skipped, header checked."""
    if not os.path.isfile(path):
        raise SystemExit("missing %s (network.trace_chain_datum, card IRD-1)" % path)
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(ln for ln in fh if not ln.startswith("#"))
        hdr = next(rd, None)
        if hdr is None:
            raise SystemExit("empty trace %s" % path)
        missing = [c for c in columns if c not in hdr]
        if missing:
            raise SystemExit("%s lacks columns %s" % (path, missing))
        ix = {c: i for i, c in enumerate(hdr)}
        for r in rd:
            if len(r) >= len(hdr):
                yield r, ix


def _int_or_none(s):
    s = s.strip()
    return None if s == "" else int(round(float(s)))


def read_edges(run, systems=None):
    """All rows of <run>/logs/network_chain_edges.csv (of `systems` when given) as Edge tuples."""
    out = []
    for r, ix in _rows(os.path.join(run, EDGES_REL), EDGE_COLUMNS):
        sysl = r[ix["system"]]
        if systems and sysl not in systems:
            continue
        t = r[ix["target_cyc"]].strip()
        out.append(Edge(round(float(r[ix["tow"]]), 3), sysl, r[ix["family"]], r[ix["band"]],
                        int(r[ix["comp"]]), r[ix["r1"]], r[ix["edge_sta"]], r[ix["edge_sat"]],
                        r[ix["child"]], r[ix["parent"]], int(r[ix["depth"]]),
                        float(t) if t else None, float(r[ix["target_m"]]), r[ix["row"]],
                        int(r[ix["certified"]]), r[ix["cert_source"]], int(r[ix["flipped"]]),
                        int(r[ix["returning"]])))
    return out


def read_chords(run, systems=None):
    """All rows of <run>/logs/network_chain_frame.csv (of `systems` when given) as Chord tuples."""
    out = []
    for r, ix in _rows(os.path.join(run, FRAME_REL), FRAME_COLUMNS):
        sysl, _, pair = r[ix["family"]].partition("|")
        if systems and sysl not in systems:
            continue
        b1, _, b2 = pair.partition("+")
        lat1, _, lat2 = r[ix["lattice_int"]].partition("/")
        out.append(Chord(round(float(r[ix["tow"]]), 3), sysl, b1, b2, r[ix["arc_sta"]],
                         r[ix["arc_sat"]], float(r[ix["float_cyc_1"]]), float(r[ix["float_cyc_2"]]),
                         float(r[ix["q11"]]), float(r[ix["q22"]]), _int_or_none(r[ix["int_1"]]),
                         _int_or_none(r[ix["int_2"]]), r[ix["source"]], int(r[ix["conflict"]]),
                         _int_or_none(lat1), _int_or_none(lat2)))
    return out


# --------------------------------------------------------------------------- #
# trees, and the runs of unchanged tree edges
# --------------------------------------------------------------------------- #
class Tree:
    """One group's tree (a forest when the group has several components) at one epoch:
    child vertex -> Edge, with memoized ancestor chains."""
    __slots__ = ("nodes", "_up", "_arc")

    def __init__(self, nodes):
        self.nodes = nodes
        self._up = {}
        self._arc = None

    def ancestors(self, v):
        """[v, parent(v), ..., root], or None when the parent map loops."""
        if v in self._up:
            return self._up[v]
        chain, seen = [v], {v}
        while chain[-1] in self.nodes:
            nxt = self.nodes[chain[-1]].parent
            if nxt in seen:
                chain = None
                break
            seen.add(nxt)
            chain.append(nxt)
        self._up[v] = chain
        return chain

    def path(self, a, b):
        """The tree path from vertex a to vertex b as [(x, y, Edge), ...] (empty when a == b);
        None when a and b share no tree (different components, an unknown vertex, a parent loop)."""
        ua, ub = self.ancestors(a), self.ancestors(b)
        if ua is None or ub is None:
            return None
        pos = {v: i for i, v in enumerate(ua)}
        j = next((j for j, v in enumerate(ub) if v in pos), None)
        if j is None:
            return None
        seq = ua[:pos[ub[j]] + 1] + ub[:j][::-1]
        steps = []
        for x, y in zip(seq, seq[1:]):
            ex = self.nodes.get(x)
            if ex is not None and ex.parent == y:
                steps.append((x, y, ex))
                continue
            ey = self.nodes.get(y)
            if ey is not None and ey.parent == x:
                steps.append((x, y, ey))
                continue
            return None
        return steps

    def edge_of_arc(self, sta, sat):
        """The tree edge on arc (sta, sat), or None when the arc is not a tree edge."""
        if self._arc is None:
            self._arc = {e.arc: e for e in self.nodes.values()}
        return self._arc.get((sta, sat))

    def defects(self):
        """Counter of structural defects of the reconstructed tree."""
        out = Counter()
        for child, e in self.nodes.items():
            chain = self.ancestors(child)
            if chain is None:
                out["parent loop"] += 1
                continue
            if chain[-1] != e.r1:
                out["root differs from r1"] += 1
            if len(chain) - 1 != e.depth:
                out["depth differs from the path length"] += 1
        return out


def step_coefficient(x, edge):
    """Coefficient of the arc of `edge` traversed from vertex x: -1 satellite to station, +1
    station to satellite."""
    return -1 if x == edge.sat else 1


class Forest:
    """The tree edges of a run by group (system, band) and epoch, and the runs of unchanged
    tree-edge signatures (child, parent, target) per arc over consecutive epochs of the trace.

    With strict_rows a row of the chain-event table other than Kept on an unchanged edge (a tree-edge
    slip, a root handover, a split, a merge) also ends the run: the card counts them as events
    although they change neither the parent and child nor the target."""

    def __init__(self, edges, strict_rows=False):
        self.strict_rows = strict_rows
        self.epochs = sorted({e.tow for e in edges})
        self.index = {t: i for i, t in enumerate(self.epochs)}
        self.nodes = defaultdict(lambda: defaultdict(dict))   # group -> tow -> child -> Edge
        self.defects = Counter()
        arcs_seen = defaultdict(set)
        self.families = Counter()
        for e in edges:
            g = (e.system, e.band)
            self.families[e.family] += 1
            d = self.nodes[g][e.tow]
            if e.child in d:
                self.defects["duplicate child in a tree"] += 1
                continue
            if {e.child, e.parent} != {e.sta, e.sat}:
                self.defects["child and parent are not the edge's station and satellite"] += 1
            if e.arc in arcs_seen[(g, e.tow)]:
                self.defects["duplicate arc in a tree"] += 1
            arcs_seen[(g, e.tow)].add(e.arc)
            d[e.child] = e
        self._trees = {}
        self.since = {g: self._runs(g) for g in self.nodes}

    @staticmethod
    def _signature(e):
        return (e.child, e.parent, None if e.target is None else int(round(e.target * 1e6)))

    def _runs(self, g):
        state, since = {}, {}
        for tow in self.epochs:
            i = self.index[tow]
            for e in self.nodes[g].get(tow, {}).values():
                sig = self._signature(e)
                st = state.get(e.arc)
                broken = self.strict_rows and e.row in STRICT_BREAK_ROWS
                start = st[2] if (st is not None and st[0] == i - 1 and st[1] == sig and not broken) else tow
                state[e.arc] = (i, sig, start)
                since[(tow, e.arc)] = start
        return since

    def groups(self):
        return sorted(self.nodes)

    def tree(self, group, tow):
        """The group's Tree at `tow`, or None when the trace has no edge of the group then."""
        key = (group, tow)
        t = self._trees.get(key)
        if t is None:
            nodes = self.nodes.get(group, {}).get(tow)
            if nodes is None:
                return None
            t = self._trees[key] = Tree(nodes)
        return t

    def previous(self, tow):
        """The epoch before `tow` in the trace, or None."""
        i = self.index.get(tow)
        return self.epochs[i - 1] if i else None

    def window_start(self, tow, quiet_s, short):
        """Latest run start that still covers the trailing window of `tow`."""
        lim = tow - quiet_s
        if short:
            lim = max(lim, self.epochs[0])
        return lim + EPS_S

    def arcs_quiet(self, group, tow, arcs, quiet_s, short=False):
        """True when every arc has been a tree edge of the group with an unchanged signature
        since the start of the window ending at `tow`."""
        lim = self.window_start(tow, quiet_s, short)
        since = self.since[group]
        for a in arcs:
            s = since.get((tow, a))
            if s is None or s > lim:
                return False
        return True


def chord_runs(chords, forest):
    """{(chord key, tow): first tow of the unbroken run of consecutive epochs holding the chord's
    row}.  A row at an epoch the edge trace lacks restarts the run."""
    start, last = {}, {}
    for c in sorted(chords, key=lambda c: c.tow):
        i = forest.index.get(c.tow)
        st = last.get(c.key)
        s = st[1] if (i is not None and st is not None and st[0] == i - 1) else c.tow
        last[c.key] = (i if i is not None else -10, s)
        start[(c.key, c.tow)] = s
    return start


def chord_quiet(forest, c, k, since_c, quiet_s, short):
    """(reason or None, path) of chord row c in band k: None when the chord-epoch is event-free,
    else the first reason that excludes it."""
    group = (c.system, c.band(k))
    tree = forest.tree(group, c.tow)
    if tree is None:
        return "no tree of the band at the epoch", None
    path = tree.path(c.sat, c.sta)
    if path is None:
        return "no tree path from the satellite to the station", None
    if not short and c.tow - forest.epochs[0] < quiet_s - EPS_S:
        return "trace history shorter than the window", path
    if not forest.arcs_quiet(group, c.tow, [e.arc for _, _, e in path], quiet_s, short):
        return "a path arc changed (or was not a tree edge) in the window", path
    s = since_c.get((c.key, c.tow))
    if s is None or s > forest.window_start(c.tow, quiet_s, short):
        return "the chord row was absent in the window", path
    return None, path


def abs_wrap(x):
    """|wrap| of a scalar or an array, in cycles."""
    return np.abs(WIC.wrapf(np.asarray(x, float)))


def stats(a):
    """{n, p50, p90, p99} of an array of |wrap| values (NaN when empty)."""
    a = np.asarray(a, float)
    if a.size == 0:
        nan = float("nan")
        return {"n": 0, "p50": nan, "p90": nan, "p99": nan}
    return {"n": int(a.size), "p50": float(np.median(a)), "p90": float(np.percentile(a, 90)),
            "p99": float(np.percentile(a, 99))}


def fmt_stats(s):
    if s["n"] == 0:
        return "n %7d" % 0
    return "n %7d  |wrap| p50 %.4f  p90 %.4f" % (s["n"], s["p50"], s["p90"])


def trace_consistency(forest, chords, edges):
    """Counter of the inconsistencies the traces show on their own (empty when clean)."""
    out = Counter(forest.defects)
    for g in forest.groups():
        for tow in forest.epochs:
            tree = forest.tree(g, tow)
            if tree is not None:
                out.update(tree.defects())
    for e in edges:
        f = WIC.DUAL_FREQ.get(e.system, {}).get(e.band[1:2])
        if f and e.target is not None and abs(e.target_m / (WIC.C_LIGHT / f) - e.target) > 1e-4:
            out["target_m / lambda differs from target_cyc"] += 1
        if (e.row == "RehostCertified") != bool(e.certified):
            out["row RehostCertified and certified disagree"] += 1
        if e.target is not None and abs(e.target - round(e.target)) > INT_TOL:
            out["non-integer target_cyc"] += 1
    for c in chords:
        i = forest.index.get(c.tow)
        if i is None:
            out["chord row at an epoch the edge trace lacks"] += 1
            continue
        for k in (1, 2):
            tree = forest.tree((c.system, c.band(k)), c.tow)
            if tree is not None and tree.edge_of_arc(c.sta, c.sat) is not None:
                out["chord row whose arc is a tree edge of band %d at the same epoch" % k] += 1
    return out


# --------------------------------------------------------------------------- #
# truth
# --------------------------------------------------------------------------- #
class Truth:
    """Producer-branch truth integers: the simulator's slip-accumulated N of (station, satellite,
    band) plus the wind-up branch of the arc (scripts/simnet_producer_ar_grade.py)."""

    def __init__(self, fixture, run, windup=True):
        stations = WIC.station_dirs(fixture)
        self.n_int, self.slips, self.n_stations = WIC.load_truth_amb_series(fixture, stations)
        if not self.n_int:
            raise SystemExit("no truth_ambiguities.csv under %s" % fixture)
        self.n_slips = sum(len(v) for v in self.slips.values())
        names = sorted({sta for (sta, _sat, _sig) in self.n_int})
        if windup:
            self.branches, self.windup_info = PAG.load_windup_branches(fixture, run, names)
        else:
            self.branches, self.windup_info = {}, None

    def N(self, sta, sat, band, tow):
        """The producer's integer of the arc at `tow`, or None without a truth row."""
        n = n_at(self.n_int, self.slips, sta, sat, band, tow)
        if n is None:
            return None
        return n + PAG.branch_at(self.branches, sta, sat, tow)


def frame_integer(forest, truth, system, band, sta, sat, tow):
    """(N~, why): the true integer of arc (sta, sat) in the frame of epoch `tow` (module docstring).
    N~ is None with a reason when the arc has no tree path, a cycle arc has no truth or a target is
    not an integer.  A tree edge of the epoch reads its own target ("tree edge")."""
    tree = forest.tree((system, band), tow)
    if tree is None:
        return None, "no tree of the band at the epoch"
    e0 = tree.edge_of_arc(sta, sat)
    if e0 is not None:
        if e0.target is None or abs(e0.target - round(e0.target)) > INT_TOL:
            return None, "non-integer target on the cycle"
        return int(round(e0.target)), "tree edge"
    path = tree.path(sat, sta)
    if path is None:
        return None, "no tree path from the satellite to the station"
    n = truth.N(sta, sat, band, tow)
    if n is None:
        return None, "no truth for the chord"
    total = float(n)
    for x, _y, e in path:
        m = truth.N(e.sta, e.sat, band, tow)
        if m is None:
            return None, "no truth for a path arc"
        if e.target is None:
            return None, "non-integer target on the cycle"
        total += step_coefficient(x, e) * (m - e.target)
    r = round(total)
    if abs(total - r) > INT_TOL:
        return None, "non-integer target on the cycle"
    return int(r), "ok"


# --------------------------------------------------------------------------- #
# mode 1: stop
# --------------------------------------------------------------------------- #
def stop_census(forest, chords, quiet_s, short=False):
    """Per band: |wrap(float)| of every chord row, of the event-free chord-epochs and of those
    event-free in both bands, with the exclusion reasons."""
    since_c = chord_runs(chords, forest)
    acc = defaultdict(lambda: {"all": [], "quiet": [], "joint": [], "why": Counter()})
    for c in chords:
        why = {k: chord_quiet(forest, c, k, since_c, quiet_s, short)[0] for k in (1, 2)}
        for k in (1, 2):
            band = c.band(k)
            v = float(abs_wrap(c.flt(k)))
            a = acc[band]
            a["all"].append(v)
            a["why"][why[k] or "event-free"] += 1
            if why[k] is None:
                a["quiet"].append(v)
                if why[3 - k] is None:
                    a["joint"].append(v)
    return {band: {"all": np.array(a["all"]), "quiet": np.array(a["quiet"]),
                   "joint": np.array(a["joint"]), "why": a["why"]}
            for band, a in sorted(acc.items())}


def stop_verdict(bands, bar):
    """(verdict, per-band rows): PASS when every band's event-free p50 <= bar, FAIL when a band
    exceeds it, NOT GRADEABLE when a band has no event-free chord-epoch and none exceeds."""
    rows, fail, ungraded = [], False, False
    for band, a in bands.items():
        s = stats(a["quiet"])
        if s["n"] == 0:
            ungraded = True
            rows.append((band, s, "NOT GRADEABLE (n = 0)"))
        elif s["p50"] <= bar:
            rows.append((band, s, "p50 %.4f <= %.4f: ok" % (s["p50"], bar)))
        else:
            fail = True
            rows.append((band, s, "p50 %.4f > %.4f: FAIL" % (s["p50"], bar)))
    verdict = "FAIL" if fail else ("NOT GRADEABLE" if (ungraded or not rows) else "PASS")
    return verdict, rows


def run_stop(args):
    run = os.path.abspath(args.run)
    systems = tuple(args.systems.split(","))
    quiet_s = args.quiet_min * 60.0
    edges = read_edges(run, systems)
    chords = read_chords(run, systems)
    if not edges or not chords:
        raise SystemExit("no chain edge or chord row of systems %s in %s" % (systems, run))
    forest = Forest(edges, args.strict_rows)
    print("== static-algebra stop rule (card IRD section 10)")
    print("run %s" % run)
    print("edge trace : %d tree-edge rows, %d epochs (TOW %.3f .. %.3f), groups %s, families %s"
          % (len(edges), len(forest.epochs), forest.epochs[0], forest.epochs[-1],
             " ".join("%s/%s" % g for g in forest.groups()), ",".join(sorted(forest.families))))
    ftows = sorted({c.tow for c in chords})
    print("chord trace: %d chord rows, %d epochs (TOW %.3f .. %.3f)"
          % (len(chords), len(ftows), ftows[0], ftows[-1]))
    print("window     : %.1f min, %s%s" % (args.quiet_min,
          "truncated at the first epoch of the trace (--short-window)" if args.short_window
          else "the whole window must lie inside the trace",
          "; slip, root handover, split and merge rows end a run (--strict-rows)" if args.strict_rows else ""))
    cons = trace_consistency(forest, chords, edges)
    print_consistency(cons)
    bands = stop_census(forest, chords, quiet_s, args.short_window)
    print("n (before any verdict): chord rows %d" % len(chords))
    for band, a in bands.items():
        n_all, n_q = len(a["all"]), len(a["quiet"])
        print("  %s: event-free chord-epochs %d of %d (%.1f %%); event-free in both bands %d"
              % (band, n_q, n_all, 100.0 * n_q / max(n_all, 1), len(a["joint"])))
        for why, n in a["why"].most_common():
            if why != "event-free":
                print("      excluded %7d  %s" % (n, why))
    print("|wrap(float_cyc)| of the chord floats, cycles:")
    result = {"mode": "stop", "run": run, "quiet_min": args.quiet_min, "short_window": args.short_window,
              "strict_rows": args.strict_rows, "bar": args.bar, "systems": systems, "n_chord_rows": len(chords),
              "inconsistencies": dict(cons), "bands": {}}
    for band, a in bands.items():
        s_all, s_q, s_j = stats(a["all"]), stats(a["quiet"]), stats(a["joint"])
        print("  %-4s all chords          %s" % (band, fmt_stats(s_all)))
        print("  %-4s event-free          %s   (%.1f %% of the chord-epochs)"
              % (band, fmt_stats(s_q), 100.0 * s_q["n"] / max(s_all["n"], 1)))
        print("  %-4s event-free, both bands %s" % (band, fmt_stats(s_j)))
        result["bands"][band] = {"all": s_all, "event_free": s_q, "event_free_both_bands": s_j,
                                 "share_event_free": s_q["n"] / max(s_all["n"], 1),
                                 "excluded": dict(a["why"])}
    verdict, rows = stop_verdict(bands, args.bar)
    print("verdict (per-band p50 |frac| of the event-free chords <= %.3f cycle; the rule is stated for "
          "the microscope bed):" % args.bar)
    for band, s, text in rows:
        print("  %-4s n %d: %s" % (band, s["n"], text))
    print("STOP RULE: %s%s" % (verdict, "  (the card stops and attributes the cause)" if verdict == "FAIL" else ""))
    result["verdict"] = verdict
    return result, {"PASS": 0, "FAIL": 1}.get(verdict, 2)


def print_consistency(cons):
    if not cons:
        print("trace consistency: clean (tree reconstruction, depths, roots, chord/tree-edge disjointness)")
        return
    print("trace consistency: %d kind(s) of inconsistency" % len(cons))
    for what, n in cons.most_common():
        print("  %7d  %s" % (n, what))


# --------------------------------------------------------------------------- #
# mode 2: cert
# --------------------------------------------------------------------------- #
def read_ar_floats(run):
    """{(tow, signal, sat_j, ref_sat, sta_r, ref_sta): float_cyc} of the producer's double-difference
    trace (network.ambiguity_resolution.trace), or None when the run carries none."""
    path = os.path.join(run, AR_REL)
    if not os.path.isfile(path):
        return None
    out = {}
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.reader(ln for ln in fh if not ln.startswith("#"))
        hdr = next(rd, None)
        need = ("tow", "signal", "sat_j", "ref_sat", "sta_r", "ref_sta", "float_cyc")
        if hdr is None or any(c not in hdr for c in need):
            return None
        ix = {c: i for i, c in enumerate(hdr)}
        for r in rd:
            if len(r) >= len(hdr):
                out[(round(float(r[ix["tow"]]), 3), r[ix["signal"]], r[ix["sat_j"]], r[ix["ref_sat"]],
                     r[ix["sta_r"]], r[ix["ref_sta"]])] = float(r[ix["float_cyc"]])
    return out


def ar_crosscheck(forest, chords, ar):
    """Formula check on real traces: for chords whose tree path has three arcs (a four-cycle, an
    ordinary double difference), float_cyc + sum(c k) against the producer's DD float of the same
    quadruple.  Returns {n_cycles, matched, max_abs, p50_abs}."""
    n3, diffs = 0, []
    for c in chords:
        for k in (1, 2):
            band = c.band(k)
            tree = forest.tree((c.system, band), c.tow)
            path = tree.path(c.sat, c.sta) if tree is not None else None
            if path is None or len(path) != 3:
                continue
            n3 += 1
            f_sta, ref_sat, r_sta = path[0][1], path[1][1], path[2][1]
            a = ar.get((c.tow, band, c.sat, ref_sat, c.sta, f_sta)) if r_sta == c.sta else None
            if a is None or any(e.target is None for _x, _y, e in path):
                continue
            p = sum(step_coefficient(x, e) * e.target for x, _y, e in path)
            diffs.append(c.flt(k) + p - a)
    d = np.abs(np.array(diffs))
    return {"n_cycles": n3, "matched": int(d.size), "max_abs": float(d.max()) if d.size else None,
            "p50_abs": float(np.median(d)) if d.size else None}


def classify_integer(iv, v, why):
    """'correct', 'wrong' or 'ungradeable: <why>' for the determined integer iv of a chord against
    the frame truth (v, why) of frame_integer."""
    if v is None:
        return "ungradeable: " + why
    if why == "tree edge":
        return "ungradeable: the chord's arc is a tree edge at that epoch"
    return "correct" if iv == v else "wrong"


def cert_grade(forest, truth, chords, max_list=20, settle_s=3600.0):
    """Per band: the would-be certifications of the chord table against truth.

    Two checks of the truth formula need no determined integer and are reported beside the grade:
      settled     float_cyc - N~ (unwrapped) on chords present in the table for at least settle_s;
      steps       (change of N~, change of float_cyc) between consecutive epochs of one chord where the
                  true frame integer moved by one cycle or more (an event moved the frame): the float
                  follows the formula's frame step when the second changes as the first does."""
    since_c = chord_runs(chords, forest)
    out = {}
    settled, steps, prev = defaultdict(list), defaultdict(list), {}
    for c in sorted(chords, key=lambda c: c.tow):
        i = forest.index.get(c.tow)
        for k in (1, 2):
            band = c.band(k)
            o = out.setdefault(band, {"rows": 0, "det": 0, "correct": 0, "wrong": 0,
                                      "ungradeable": Counter(), "wrong_list": [],
                                      "det_arcs": set(), "wrong_arcs": set(),
                                      "by_source": defaultdict(Counter)})
            o["rows"] += 1
            v, why = frame_integer(forest, truth, c.system, band, c.sta, c.sat, c.tow)
            if v is not None and why != "tree edge":
                f = c.flt(k)
                if c.tow - since_c.get((c.key, c.tow), c.tow) >= settle_s - EPS_S:
                    settled[band].append(f - v)
                p = prev.get((c.key, band))
                if p is not None and i is not None and p[0] == i - 1 and abs(v - p[1]) >= 1:
                    steps[band].append((v - p[1], f - p[2]))
                prev[(c.key, band)] = (i, v, f)
            iv = c.det(k)
            if iv is None:
                continue
            status = classify_integer(iv, v, why)
            o["det"] += 1
            o["det_arcs"].add((c.sta, c.sat))
            o["by_source"][c.source][status.split(":")[0]] += 1
            if status == "correct":
                o["correct"] += 1
            elif status == "wrong":
                o["wrong"] += 1
                o["wrong_arcs"].add((c.sta, c.sat))
                if len(o["wrong_list"]) < max_list:
                    o["wrong_list"].append({"tow": c.tow, "sta": c.sta, "sat": c.sat, "band": band,
                                            "int": iv, "truth": v, "float": c.flt(k),
                                            "source": c.source, "conflict": c.conflict})
            else:
                o["ungradeable"][status[len("ungradeable: "):]] += 1
    for band, o in out.items():
        o["settled"] = np.array(settled[band])
        o["steps"] = np.array(steps[band]).reshape(-1, 2)
    return out


def rehost_grade(forest, truth, chords, edges, max_list=20):
    """The re-host rows (RehostCertified, RehostUncertified) against the previous observed epoch's
    chord table and truth: the table's integer for the new arc and band (determined or not, right or
    wrong), and the row's own target against the arc's true integer in the incumbent frame."""
    chord_at = {(c.tow, c.system, c.sta, c.sat): c for c in chords}
    per = defaultdict(lambda: {"n": 0, "table": Counter(), "int": Counter(), "step": Counter(),
                               "cert_n": 0, "cert_int": Counter(), "incons": Counter()})
    wrong_cert, steps = [], Counter()
    for e in edges:
        if e.row not in REHOST_ROWS:
            continue
        o = per[(e.row, e.band)]
        o["n"] += 1
        pt = forest.previous(e.tow)
        c = chord_at.get((pt, e.system, e.sta, e.sat)) if pt is not None else None
        k = None if c is None else (1 if e.band == c.b1 else (2 if e.band == c.b2 else None))
        zeta, why = (None, "no previous epoch") if pt is None else \
            frame_integer(forest, truth, e.system, e.band, e.sta, e.sat, pt)
        if c is None or k is None:
            o["table"]["no row"] += 1
            if e.certified:
                o["incons"]["certified without a previous chord row"] += 1
        elif c.det(k) is None:
            o["table"]["row, no integer"] += 1
            if e.certified:
                o["incons"]["certified without a determined integer"] += 1
        else:
            o["table"]["determined"] += 1
            status = classify_integer(c.det(k), zeta, why).split(":")[0]
            o["int"][status] += 1
            if e.certified:
                o["cert_n"] += 1
                o["cert_int"][status] += 1
                if c.conflict:
                    o["incons"]["certified with a conflict flag"] += 1
                if e.target is not None and abs(e.target - c.det(k)) > INT_TOL:
                    o["incons"]["certified target differs from the table's integer"] += 1
                if status == "wrong" and len(wrong_cert) < max_list:
                    wrong_cert.append({"tow": e.tow, "sta": e.sta, "sat": e.sat, "band": e.band,
                                       "int": c.det(k), "truth": zeta, "target": e.target})
        if zeta is None or e.target is None or why == "tree edge":
            o["step"]["ungradeable"] += 1
        else:
            j = int(round(e.target - zeta))
            steps[j] += 1
            o["step"]["0 (frame kept)" if j == 0 else "integer step j != 0"] += 1
    return dict(per), wrong_cert, steps


def run_cert(args):
    run = os.path.abspath(args.run)
    fixture = os.path.abspath(args.fixture)
    systems = tuple(args.systems.split(","))
    edges = read_edges(run, systems)
    chords = read_chords(run, systems)
    if not edges or not chords:
        raise SystemExit("no chain edge or chord row of systems %s in %s" % (systems, run))
    forest = Forest(edges)
    truth = Truth(fixture, run, windup=not args.no_windup_branch)
    print("== would-be certification against truth (card IRD section 10)")
    print("run %s\nfixture %s" % (run, fixture))
    print("truth      : %d (station, satellite, band) integers over %d stations, %d scripted slips; "
          "wind-up branch %s" % (len(truth.n_int), truth.n_stations, truth.n_slips,
          "off (--no-windup-branch)" if truth.windup_info is None else
          "%d arcs, %d on a shifted branch, %d without wind-up truth"
          % (truth.windup_info["arcs"], truth.windup_info["arcs_shifted"],
             truth.windup_info["arcs_no_truth"])))
    print("traces     : %d tree-edge rows over %d epochs; %d chord rows over %d epochs"
          % (len(edges), len(forest.epochs), len(chords), len({c.tow for c in chords})))
    cons = trace_consistency(forest, chords, edges)
    print_consistency(cons)
    result = {"mode": "cert", "run": run, "fixture": fixture, "systems": systems,
              "n_chord_rows": len(chords), "inconsistencies": dict(cons), "bands": {}}
    ar = read_ar_floats(run)
    if ar is None:
        print("formula check: no %s in the run (network.ambiguity_resolution.trace); skipped" % AR_REL)
        result["formula_check"] = None
    else:
        fc = ar_crosscheck(forest, chords, ar)
        result["formula_check"] = fc
        if fc["matched"]:
            agree = fc["max_abs"] < AR_AGREE_CYC
            print("formula check against the producer's double differences (%s): %d four-cycle chords "
                  "(three-arc paths), %d matched; |float_cyc + sum(c k) - DD float| max %.6f, p50 %.6f "
                  "cycle: %s" % (AR_REL, fc["n_cycles"], fc["matched"], fc["max_abs"], fc["p50_abs"],
                                 "AGREE" if agree else "MISMATCH, the grade below is unreliable"))
        else:
            print("formula check: no four-cycle chord matched a row of %s (%d three-arc chords); skipped"
                  % (AR_REL, fc["n_cycles"]))
    res = cert_grade(forest, truth, chords, args.max_list, args.settle_min * 60.0)
    print("n (before any verdict): chord rows %d" % len(chords))
    total_wrong = 0
    for band, o in res.items():
        grad = o["correct"] + o["wrong"]
        ung = sum(o["ungradeable"].values())
        total_wrong += o["wrong"]
        print("band %s: chord rows %d, determined %d (%.2f %% of the rows; %d distinct arcs)"
              % (band, o["rows"], o["det"], 100.0 * o["det"] / max(o["rows"], 1), len(o["det_arcs"])))
        print("   gradeable %d: correct %d, wrong %d (%d distinct arcs); ungradeable %d%s"
              % (grad, o["correct"], o["wrong"], len(o["wrong_arcs"]), ung,
                 "" if not ung else " (" + "; ".join("%s: %d" % kv for kv in o["ungradeable"].items()) + ")"))
        for src, cnt in sorted(o["by_source"].items()):
            print("   source %-8s %s" % (src, ", ".join("%s %d" % kv for kv in sorted(cnt.items()))))
        for w in o["wrong_list"]:
            print("   WRONG tow %.3f  %s/%s  %s  int %+d  truth %+d  (float %.3f, source %s, conflict %d)"
                  % (w["tow"], w["sta"], w["sat"], w["band"], w["int"], w["truth"], w["float"],
                     w["source"], w["conflict"]))
        if o["wrong"] > len(o["wrong_list"]):
            print("   ... %d more wrong not listed" % (o["wrong"] - len(o["wrong_list"])))
        st, sp = o["settled"], o["steps"]
        if st.size:
            ae = np.abs(st)
            print("   float error against the frame truth, chords present for >= %.0f min (float_cyc - N~, "
                  "unwrapped): n %d, |e| p50 %.3f p90 %.3f, within 0.5 cycle %.1f %%"
                  % (args.settle_min, st.size, np.median(ae), np.percentile(ae, 90), 100.0 * np.mean(ae < 0.5)))
        else:
            print("   float error: no gradeable row on a chord present for >= %.0f min" % args.settle_min)
        if sp.shape[0]:
            dd = np.abs(sp[:, 1] - sp[:, 0])
            print("   frame-step check: %d chord-epochs where the true frame integer moved by >= 1 cycle: "
                  "median |true step| %.1f, median |float step - true step| %.3f, within 0.5 cycle %.1f %%"
                  % (sp.shape[0], np.median(np.abs(sp[:, 0])), np.median(dd), 100.0 * np.mean(dd < 0.5)))
        else:
            print("   frame-step check: no chord-epoch where the true frame integer moved by >= 1 cycle")
        result["bands"][band] = {"rows": o["rows"], "determined": o["det"], "correct": o["correct"],
                                 "wrong": o["wrong"], "ungradeable": dict(o["ungradeable"]),
                                 "share_determined": o["det"] / max(o["rows"], 1),
                                 "wrong_list": o["wrong_list"],
                                 "float_error_n": int(st.size),
                                 "float_error_abs_p50": float(np.median(np.abs(st))) if st.size else None,
                                 "frame_step_n": int(sp.shape[0]),
                                 "frame_step_abs_error_p50": float(np.median(np.abs(sp[:, 1] - sp[:, 0])))
                                 if sp.shape[0] else None}
    per, wrong_cert, steps = rehost_grade(forest, truth, chords, edges, args.max_list)
    n_re = sum(o["n"] for o in per.values())
    print("re-host rows (edge rows RehostCertified / RehostUncertified; the previous observed epoch's "
          "chord table): %d" % n_re)
    result["rehosts"] = {"n": n_re, "groups": {}}
    n_cert_wrong = 0
    for (row, band), o in sorted(per.items()):
        print("  %-17s %s: n %d; previous chord table: %s"
              % (row, band, o["n"], ", ".join("%s %d" % kv for kv in sorted(o["table"].items()))))
        print("      its integer against truth: %s; the pin's target against the incumbent-frame integer: %s"
              % (", ".join("%s %d" % kv for kv in sorted(o["int"].items())) or "none determined",
                 ", ".join("%s %d" % kv for kv in sorted(o["step"].items())) or "none"))
        if o["cert_n"]:
            print("      certified rows %d: integer %s"
                  % (o["cert_n"], ", ".join("%s %d" % kv for kv in sorted(o["cert_int"].items()))))
        for what, n in sorted(o["incons"].items()):
            print("      INCONSISTENT %d: %s" % (n, what))
        n_cert_wrong += o["cert_int"]["wrong"]
        result["rehosts"]["groups"]["%s/%s" % (row, band)] = {
            "n": o["n"], "table": dict(o["table"]), "int": dict(o["int"]), "step": dict(o["step"]),
            "certified": o["cert_n"], "certified_int": dict(o["cert_int"]), "inconsistent": dict(o["incons"])}
    if n_re == 0:
        print("  no re-host row in the trace")
    if steps:
        print("  integer step j of the pin against the incumbent frame (target - truth): %s"
              % ", ".join("%+d: %d" % kv for kv in sorted(steps.items())))
    for w in wrong_cert:
        print("  WRONG CERTIFIED RE-HOST tow %.3f  %s/%s  %s  certified int %+d  truth %+d  target %s"
              % (w["tow"], w["sta"], w["sat"], w["band"], w["int"], w["truth"], w["target"]))
    result["rehosts"]["wrong_certified"] = n_cert_wrong
    result["rehosts"]["integer_step_histogram"] = {str(k): v for k, v in sorted(steps.items())}
    graded = sum(o["correct"] + o["wrong"] for o in res.values())
    verdict = "PASS" if (graded and not total_wrong and not n_cert_wrong) else \
        ("NOT GRADEABLE" if not graded else "FAIL")
    print("would-be certification: %d gradeable determined integer(s), %d wrong; %d wrong certified re-host(s)"
          % (graded, total_wrong, n_cert_wrong))
    print("WOULD-BE CERTIFICATION: %s (bar: 0 wrong against truth)" % verdict)
    result["verdict"] = verdict
    return result, {"PASS": 0, "FAIL": 1}.get(verdict, 2)


# --------------------------------------------------------------------------- #
# mode 3: floor
# --------------------------------------------------------------------------- #
def calibrate(floor, bar, n, min_n=CAL_MIN_N):
    """Card IRD section 9, calibration rule: (verdict, bar after calibration, text)."""
    if not n >= min_n:
        return "STANDS", bar, "n %d < %d: B stands" % (n, min_n)
    if floor <= CAL_STAND * bar:
        return "STANDS", bar, "F %.4f <= %.1f B (%.4f)" % (floor, CAL_STAND, CAL_STAND * bar)
    return "MISSED", CAL_WIDEN * floor, "F %.4f > %.1f B (%.4f): bar becomes %.2f F = %.4f" % (
        floor, CAL_STAND, CAL_STAND * bar, CAL_WIDEN, CAL_WIDEN * floor)


def floor_census(forest, epochs, osb, sc_truth, bands, systems, quiet_s, short=False,
                 start=None, end=None):
    """Wire statistics of the satellite pairs lying in one tree of a band, split by event-free.

    For each wire epoch and each pair (s, p) of one system: the pair's tree path in each band's tree
    (the path between the satellite vertices); event-free per band as in module docstring.  E_band1
    needs band 1 event-free, E_band2 band 2, and D_WL and g both.  Returns {channel: {"floor":
    |wrap| array of the event-free pairs, "all": every pair of the system}, counters}."""
    b1, b2 = bands
    floor = {c: [] for c in ("E1", "E2", "WL", "NL")}
    every = {c: [] for c in ("E1", "E2", "WL", "NL")}
    cnt = Counter()
    for ep in epochs:
        t = ep["tow"]
        if start is not None and t < start - EPS_S:
            continue
        if end is not None and t >= end - EPS_S:
            continue
        cnt["wire epochs graded"] += 1
        if t not in forest.index:
            cnt["wire epochs absent from the edge trace"] += 1
            continue
        vals = WIC.dual_sat_values(ep, osb, sc_truth, bands, True)
        for sysl in systems:
            sats = sorted(s for s in vals if s[0] == sysl)
            groups = ((sysl, b1), (sysl, b2))
            trees = [forest.tree(g, t) for g in groups]
            for i in range(len(sats)):
                for j in range(i + 1, len(sats)):
                    s, p = sats[i], sats[j]
                    ch = WIC.dual_pair_channels(vals[s], vals[p])
                    cnt["pair-epochs"] += 1
                    quiet = []
                    for g, tr in zip(groups, trees):
                        path = tr.path(s, p) if tr is not None else None
                        if path is not None:
                            cnt["in one tree, band %s" % g[1]] += 1
                        ok = path is not None and forest.arcs_quiet(
                            g, t, [e.arc for _, _, e in path], quiet_s, short)
                        quiet.append(ok)
                        cnt["event-free, band %s" % g[1]] += int(ok)
                    cnt["event-free, both bands"] += int(quiet[0] and quiet[1])
                    for c in ("E1", "E2", "WL", "NL"):
                        v = float(abs_wrap(ch[c]))
                        every[c].append(v)
                        if (c == "E1" and quiet[0]) or (c == "E2" and quiet[1]) or \
                           (c in ("WL", "NL") and quiet[0] and quiet[1]):
                            floor[c].append(v)
    return {c: {"floor": np.array(floor[c]), "all": np.array(every[c])} for c in floor}, cnt


def floor_rows(res, min_n=CAL_MIN_N):
    """The calibration table: [(bar, channel, statistic, B, F, n, verdict, new bar, text)]."""
    rows = []
    for bar, ch, stat, b in W_BARS:
        a = res[ch]["floor"]
        n = int(a.size)
        f = float(np.median(a)) if (stat == "p50" and n) else \
            (float(np.percentile(a, 99)) if n else float("nan"))
        if n:
            v, nb, text = calibrate(f, b, n, min_n)
        else:
            v, nb, text = "STANDS", b, "n 0 < %d: B stands" % min_n
        rows.append((bar, ch, stat, b, f, n, v, nb, text))
    for bar, ch, q, b in W_SHARE_BARS:
        a = res[ch]["floor"]
        n = int(a.size)
        f = float(np.percentile(a, q)) if n else float("nan")
        if n:
            v, nb, text = calibrate(f, b, n, min_n)
        else:
            v, nb, text = "STANDS", b, "n 0 < %d: B stands" % min_n
        rows.append((bar + " share (extension)", ch, "p%d" % q, b, f, n, v, nb, text))
    return rows


def run_floor(args):
    run = os.path.abspath(args.run)
    fixture = os.path.abspath(args.fixture)
    systems = tuple(args.systems.split(","))
    bands = tuple(args.bands.split(","))
    if len(bands) != 2:
        raise SystemExit("--bands takes exactly two phase signals, e.g. L1C,L2W")
    quiet_s = args.quiet_min * 60.0
    edges = read_edges(run, systems)
    if not edges:
        raise SystemExit("no chain edge row of systems %s in %s" % (systems, run))
    forest = Forest(edges, args.strict_rows)
    osb, sc_truth, epochs, census = WIC.dual_load(fixture, run, args.source)
    t0 = epochs[0]["tow"]
    start = args.start_tow if args.start_tow is not None else (
        WIC.start_from_hms(t0, args.start_hms) if args.start_hms else None)
    end = start + args.hours * 3600.0 if (args.hours and start is not None) else (
        t0 + args.hours * 3600.0 if args.hours else None)
    print("== event-free floor of the W bars (card IRD sections 9 and 10)")
    print("run %s\nfixture %s\nwire source %s, bands %s, systems %s" % (run, fixture, args.source,
          "/".join(bands), ",".join(systems)))
    print("edge trace : %d tree-edge rows, %d epochs (TOW %.3f .. %.3f); wire: %d epochs"
          % (len(edges), len(forest.epochs), forest.epochs[0], forest.epochs[-1], len(epochs)))
    print("window     : %.1f min, %s%s; graded from TOW %s%s"
          % (args.quiet_min, "truncated at the first epoch of the trace" if args.short_window
             else "the whole window must lie inside the trace",
             "; slip, root handover, split and merge rows end a run" if args.strict_rows else "",
             "%.3f" % start if start is not None else "the first wire epoch",
             " for %.2f h" % args.hours if args.hours else ""))
    cons = trace_consistency(forest, [], edges)
    print_consistency(cons)
    res, cnt = floor_census(forest, epochs, osb, sc_truth, bands, systems, quiet_s,
                            args.short_window, start, end)
    print("n (before any verdict):")
    for k in ("wire epochs graded", "wire epochs absent from the edge trace", "pair-epochs"):
        print("  %-42s %d" % (k, cnt[k]))
    for b in bands:
        print("  %-42s %d" % ("pair-epochs in one tree, band %s" % b, cnt["in one tree, band %s" % b]))
        print("  %-42s %d" % ("event-free pair-epochs, band %s" % b, cnt["event-free, band %s" % b]))
    print("  %-42s %d" % ("event-free pair-epochs, both bands", cnt["event-free, both bands"]))
    print("|wrap| of the wire statistics (cycles; g in NL cycles):")
    out = {"mode": "floor", "run": run, "fixture": fixture, "source": args.source, "bands": bands,
           "systems": systems, "quiet_min": args.quiet_min, "short_window": args.short_window,
           "strict_rows": args.strict_rows, "counts": dict(cnt),
           "inconsistencies": dict(cons), "channels": {}}
    for c in ("E1", "E2", "WL", "NL"):
        f, a = res[c]["floor"], res[c]["all"]
        sf, sa = stats(f), stats(a)
        extra = ""
        if c == "WL" and f.size:
            extra = "  within 0.20: %.3f" % float(np.mean(f <= 0.20))
        if c == "NL" and f.size:
            extra = "  within 0.25: %.3f" % float(np.mean(f <= 0.25))
        print("  %-8s event-free %s  p99 %.4f%s" % (CHANNEL_NAME[c], fmt_stats(sf),
              sf["p99"] if sf["n"] else float("nan"), extra))
        print("  %-8s all pairs  %s  p99 %.4f   (context)" % ("", fmt_stats(sa),
              sa["p99"] if sa["n"] else float("nan")))
        out["channels"][c] = {"event_free": sf, "all_pairs": sa,
                              "within_0.20": float(np.mean(f <= 0.20)) if (c == "WL" and f.size) else None,
                              "within_0.25": float(np.mean(f <= 0.25)) if (c == "NL" and f.size) else None}
    print("calibration (card IRD section 9: with n >= %d, F <= %.1f B stands, else the bar becomes %.2f F "
          "and B is MISSED; with n < %d, B stands):" % (args.min_n, CAL_STAND, CAL_WIDEN, args.min_n))
    print("  W-micro applies to r1c (zero-ionosphere rows, settled hour); W-WL and W-NL to r2, r3c and the "
          "golden bed")
    rows = floor_rows(res, args.min_n)
    out["calibration"] = []
    for bar, ch, stat, b, f, n, v, nb, text in rows:
        print("  %-22s %-8s %-3s  B %.3f  F %s  n %7d  %-7s %s"
              % (bar, CHANNEL_NAME[ch], stat, b, "%.4f" % f if n else "   -  ", n, v, text))
        out["calibration"].append({"bar": bar, "channel": ch, "stat": stat, "B": b,
                                   "F": f if n else None, "n": n, "verdict": v,
                                   "bar_after": nb})
    return out, 0


# --------------------------------------------------------------------------- #
# selftest
# --------------------------------------------------------------------------- #
class DictTruth:
    """Truth integers from a dict {(sta, sat, band): int} plus {(sta, sat): branch} (tests)."""

    def __init__(self, ints, branch=None):
        self.ints, self.branch = ints, branch or {}

    def N(self, sta, sat, band, tow):
        n = self.ints.get((sta, sat, band))
        return None if n is None else n + self.branch.get((sta, sat), 0)


# The synthetic world: stations A (the root) and B with truth, station C without; satellites
# G01 to G03; bands L1C and L2W; 12 epochs at 30 s.  Both bands' trees: G01, G02 and G03 children of
# A, B a child of G01, C a child of G02.  Chords: (B, G02), (B, G03) and, for two epochs, (C, G01).
# Events: the band-1 target of (A, G03) moves 0 -> 1 at epoch 6; at epoch 8 G03 is re-hosted onto B in
# band 1 (a certified row, target 9) and the chord (B, G03) is a tree edge from then on.
A, B, C = "S01000SIM", "S02000SIM", "S03000SIM"
SATS = ("G01", "G02", "G03")
L1, L2 = "L1C", "L2W"
EPOCH_T0, EPOCH_DT, EPOCH_N = 261030.0, 30.0, 12
TRUTH_N = {(A, "G01", L1): 10, (A, "G02", L1): 20, (A, "G03", L1): 30,
           (B, "G01", L1): 11, (B, "G02", L1): 25, (B, "G03", L1): 40,
           (A, "G01", L2): -30, (A, "G02", L2): -5, (A, "G03", L2): 17,
           (B, "G01", L2): -29, (B, "G02", L2): 8, (B, "G03", L2): -4}
BRANCH = {(B, "G02"): 1}          # the arc whose wind-up starts at +0.7 cycle (k = round(0.7) = 1)
TARGET_L2 = {(A, "G01"): 0, (A, "G02"): 4, (A, "G03"): 5, (B, "G01"): -3, (C, "G02"): 7}
TREE_SPEC = (("G01", A, A, "G01", 1), ("G02", A, A, "G02", 1), ("G03", A, A, "G03", 1),
             (B, "G01", B, "G01", 2), (C, "G02", C, "G02", 2))      # child, parent, sta, sat, depth


def _tow(k):
    return EPOCH_T0 + EPOCH_DT * k


def _world_edges():
    """The Edge rows of the synthetic world, epoch by epoch."""
    edges = []
    for k in range(EPOCH_N):
        for band in (L1, L2):
            lam = WIC.C_LIGHT / WIC.DUAL_FREQ["G"][band[1]]
            tg = {(A, "G01"): 3, (A, "G02"): -2, (A, "G03"): 0 if k < 6 else 1, (B, "G01"): 1,
                  (C, "G02"): 2} if band == L1 else TARGET_L2
            for child, parent, sta, sat, depth in TREE_SPEC:
                row, cert, src = ("Birth" if k == 0 else "Kept"), 0, "none"
                t = float(tg[(sta, sat)])
                if band == L1 and sat == "G03" and k >= 8:
                    child, parent, sta, depth, t = "G03", B, B, 3, 9.0
                    if k == 8:
                        row, cert, src = "RehostCertified", 1, "pair"
                edges.append(Edge(_tow(k), "G", "signal", band, 0, A, sta, sat, child, parent, depth, t,
                                  round(t * lam, 6), row, cert, src, 0, 0))
    return edges


def _frac(k):
    """Planted fraction of a chord row the test expects to be event-free: (-1)^k 0.001 (k + 1)."""
    return (-1) ** k * 0.001 * (k + 1)


def _frame_truth(sat, band, k):
    """The true frame integer of the chord (B, sat) at epoch k (hand-computed, see the tests)."""
    if band == L2:
        return 14 if sat == "G02" else -20
    return 1 if sat == "G02" else (7 if k < 6 else 8)


def _world_chords():
    """Chord rows: (B, G02) at epochs 0 to 11 except 9; (B, G03) at 0 to 7; (C, G01) at 6 and 7.
    A float is the true frame integer plus a planted fraction on the rows the full-window rule keeps
    (0.4 elsewhere, so that a leak shows).  Determined integers: (B, G02) at 6, 7, 11 and (B, G03)
    at 5, 6, 7 (band 1: 1 and 7, 7, 9; band 2: 14 and -20), (C, G01) both bands 5."""
    keep = {(B, "G02", L1): set(range(4, 9)), (B, "G02", L2): set(range(4, 9)),
            (B, "G03", L1): {4, 5}, (B, "G03", L2): set(range(4, 8))}
    chords = []
    for sat, ks in (("G02", [k for k in range(12) if k != 9]), ("G03", range(8))):
        for k in ks:
            f1 = _frame_truth(sat, L1, k) + (_frac(k) if k in keep[(B, sat, L1)] else 0.4)
            f2 = _frame_truth(sat, L2, k) + (_frac(k) if k in keep[(B, sat, L2)] else 0.4)
            i1 = i2 = None
            if sat == "G02" and k in (6, 7, 11):
                i1, i2 = 1, (14 if k != 11 else None)
            if sat == "G03" and k in (5, 6, 7):
                i1, i2 = {5: 7, 6: 7, 7: 9}[k], -20
            chords.append(Chord(_tow(k), "G", L1, L2, B, sat, f1, f2, 0.1, 0.1, i1, i2,
                                "pair" if (i1 is not None or i2 is not None) else "none", 0, None, None))
    for k in (6, 7):
        chords.append(Chord(_tow(k), "G", L1, L2, C, "G01", 5.4, 5.4, 0.1, 0.1, 5, 5, "pair", 0, None, None))
    return chords


def _write_world(td):
    """Write the synthetic fixture and run under `td`; returns (fixture, run)."""
    fx, run = os.path.join(td, "fx"), os.path.join(td, "run")
    os.makedirs(os.path.join(run, "logs"))
    osb = {}
    for i, s in enumerate(SATS):
        osb[(s, "C1C")], osb[(s, "C2W")] = 1.0 + 0.1 * i, -0.7 + 0.13 * i
        osb[(s, "L1C")], osb[(s, "L2W")] = 0.031 * i, -0.047 * i
    for sta in (A, B):
        d = os.path.join(fx, sta)
        os.makedirs(d)
        with open(os.path.join(d, "truth_bias.csv"), "w", newline="") as fh:
            fh.write("satellite,signal,osb_ns,osb_m,product_m,synth_m,source\n")
            for (s, g), v in osb.items():
                fh.write("%s,%s,0,%.9f,%.9f,0,product\n" % (s, g, v, v))
        with open(os.path.join(d, "truth_ambiguities.csv"), "w", newline="") as fh:
            fh.write("satellite,signal,N_int,wavelength_m\n")
            for (st, s, band), v in sorted(TRUTH_N.items()):
                if st == sta:
                    fh.write("%s,%s,%d,0.19\n" % (s, band, v))
        with open(os.path.join(d, "truth_windup.csv"), "w", newline="") as fh:
            fh.write("week,tow_s,satellite,windup_cycles\n")
            for k in range(EPOCH_N):
                for s in SATS:
                    fh.write("2313,%.3f,%s,%.3f\n" % (_tow(k), s, 0.7 if (sta, s) in BRANCH else 0.1))
    with open(os.path.join(run, "logs", "network_slip_raw.csv"), "w", newline="") as fh:
        fh.write("week,tow,station,sat,sig1,sig2,elev_deg\n")
        for k in range(EPOCH_N):
            for sta in (A, B):
                for s in SATS:
                    fh.write("2313,%.3f,%s,%s,L1C,L2W,30.0\n" % (_tow(k), sta, s))
    with open(os.path.join(run, EDGES_REL), "w", newline="") as fh:
        fh.write("week,tow,system,family,band,comp,r1,edge_sta,edge_sat,child,parent,depth,target_m,"
                 "target_cyc,prev_target_m,pinmove_m,known,legacy_cause,row,row_armed,certified,"
                 "cert_source,flipped,returning,life_stamp\n")
        for e in _world_edges():
            fh.write("2313,%.3f,%s,%s,%s,%d,%s,%s,%s,%s,%s,%d,%.6f,%.6f,,0.000000,1,same_edge,%s,0,%d,%s,"
                     "%d,%d,x\n" % (e.tow, e.system, e.family, e.band, e.comp, e.r1, e.sta, e.sat, e.child,
                                    e.parent, e.depth, e.target_m, e.target, e.row, e.certified, e.src,
                                    e.flipped, e.returning))
    with open(os.path.join(run, FRAME_REL), "w", newline="") as fh:
        fh.write("week,tow,family,arc_sta,arc_sat,float_cyc_1,float_cyc_2,q11,q12,q22,int_1,int_2,"
                 "source,conflict,lattice_int\n")
        for c in _world_chords():
            fh.write("2313,%.3f,G|L1C+L2W,%s,%s,%.6f,%.6f,%.6e,0,%.6e,%s,%s,%s,%d,/\n"
                     % (c.tow, c.sta, c.sat, c.f1, c.f2, c.q11, c.q22,
                        "" if c.int1 is None else c.int1, "" if c.int2 is None else c.int2,
                        c.source, c.conflict))
    # The producer's DD trace: the double difference of (B, G02) against the reference pair (A, G01) at
    # epochs 4 to 8 is float_cyc + sum(c k) = float_cyc + 4 in band 1 (-2 + 3 - 1 with the signs of the
    # path G02 -> A -> G01 -> B: +2, +3, -1) and float_cyc - 1 in band 2 (-4, 0, +3).
    with open(os.path.join(run, AR_REL), "w", newline="") as fh:
        fh.write("week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,sigma_cyc\n")
        for c in _world_chords():
            if c.sat == "G02" and c.sta == B and 4 <= round((c.tow - EPOCH_T0) / EPOCH_DT) <= 8:
                for band, f, p in ((L1, c.f1, 4.0), (L2, c.f2, -1.0)):
                    fh.write("2313,%.3f,G,%s,G02,G01,%s,%s,0.19,%.6f,0.2\n" % (c.tow, band, B, A, f + p))
    # The wire: truth, plus G02 L1C +0.01 cycle at every epoch and G03 L1C +0.4 cycle at epochs 6 to 9.
    l1 = WIC.C_LIGHT / WIC.DUAL_FREQ["G"]["1"]
    rows, prehold = [], []
    for k in range(EPOCH_N):
        rows.append("E,2313,%.4f,0,0,0,0" % _tow(k))
        for s in SATS:
            rows.append("C,%s,0.0" % s)
            for g in ("C1C", "C2W"):
                rows.append("B,%s,%s,%.12f" % (s, g, osb[(s, g)]))
            p1 = osb[(s, "L1C")] + (0.01 * l1 if s == "G02" else 0.0) + \
                (0.4 * l1 if (s == "G03" and 6 <= k <= 9) else 0.0)
            p2 = osb[(s, "L2W")]
            rows.append("P,%s,L1C,%.12f,1,0,gen=1,cmp=1" % (s, p1))
            rows.append("P,%s,L2W,%.12f,1,0,gen=1,cmp=1" % (s, p2))
            for g, p in (("L1C", p1), ("L2W", p2)):
                prehold.append("2313,%.3f,%s,%s,%.12f,0.01,0.0,0,0,1,tree,0,%s,%s,1"
                               % (_tow(k), s, g, p, A, A))
    with open(os.path.join(run, "products.txt"), "w", newline="") as fh:
        fh.write("# gnss_network_product v2\nS,%s,1,nominal-wu93,2,0,1,0,0.05,1,x,G:G01\n" % A)
        fh.write("\n".join(rows) + "\n")
    with open(os.path.join(run, "logs", "network_pb_prehold.csv"), "w", newline="") as fh:
        fh.write(",".join(WIC.PREHOLD_HEADER) + "\n" + "\n".join(prehold) + "\n")
    return fx, run


def _captured(fn, *a):
    """(result, printed text) of fn(*a) with stdout captured."""
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        res = fn(*a)
    return res, buf.getvalue()


def selftest():
    ok = [True]

    def chk(cond, msg):
        print("  %s  %s" % ("ok  " if cond else "FAIL", msg))
        ok[0] &= bool(cond)

    near = lambda a, b, tol=1e-9: abs(a - b) <= tol            # noqa: E731
    edges = _world_edges()
    forest = Forest(edges)
    truth = DictTruth(TRUTH_N, BRANCH)
    quiet_s = 120.0

    # -- the tree, its path and the cycle signs ----------------------------------------------
    tr = forest.tree(("G", L1), _tow(0))
    path = tr.path("G02", B)
    chk([(x, y) for x, y, _ in path] == [("G02", A), (A, "G01"), ("G01", B)]
        and [step_coefficient(x, e) for x, _y, e in path] == [-1, 1, -1],
        "path G02 -> B: via A and G01, coefficients -1 (sat to station), +1 (station to sat), -1")
    chk([e.arc for _, _, e in path] == [(A, "G02"), (A, "G01"), (B, "G01")],
        "the path's arcs, from the satellite back to the station")
    chk(tr.path("G01", "G01") == [] and tr.path("G01", "ZZZ") is None
        and len(tr.path(B, C)) == 4 and tr.defects() == Counter(),
        "empty path to itself, None to an unknown vertex, B to C through G01, A, G02; no tree defect")
    mk = lambda child, parent: Edge(0.0, "G", "signal", L1, 0, "R", "R", child, child, parent, 1, 0.0,  # noqa: E731
                                    0.0, "Kept", 0, "none", 0, 0)
    loop = Tree({"X": mk("X", "Y"), "Y": mk("Y", "X")})
    chk(loop.path("X", "Y") is None and loop.defects()["parent loop"] == 2,
        "a parent loop is a defect and has no path")

    # -- the truth formula --------------------------------------------------------------------
    v, why = frame_integer(forest, truth, "G", L1, B, "G02", _tow(0))
    chk(v == 1 and why == "ok",
        "chord (B, G02) band 1: N(B,G02)+1 - (20+2) + (10-3) - (11-1) = 26 - 22 + 7 - 10 = 1 (got %s)" % v)
    chk(frame_integer(forest, truth, "G", L1, B, "G03", _tow(0))[0] == 7
        and frame_integer(forest, truth, "G", L1, B, "G03", _tow(6))[0] == 8,
        "chord (B, G03) band 1: 40 - 30 + 7 - 10 = 7, and 8 after the target of (A, G03) moves 0 -> 1")
    chk(frame_integer(forest, truth, "G", L2, B, "G02", _tow(0))[0] == 14
        and frame_integer(forest, truth, "G", L2, B, "G03", _tow(0))[0] == -20,
        "band 2: (B, G02) 9 + 9 - 30 + 26 = 14 and (B, G03) -4 - 12 - 30 + 26 = -20")
    chk(frame_integer(forest, truth, "G", L1, C, "G01", _tow(6)) == (None, "no truth for the chord"),
        "a station without truth is ungradeable")
    chk(frame_integer(forest, truth, "G", L1, A, "G01", _tow(0)) == (3, "tree edge"),
        "a tree edge's frame integer is its target")
    chk(frame_integer(forest, DictTruth(TRUTH_N), "G", L1, B, "G02", _tow(0))[0] == 0,
        "without the wind-up branch the same chord reads 0 (the branch is +1)")

    # -- event-free runs -----------------------------------------------------------------------
    g1 = ("G", L1)
    arcs_g03 = [(A, "G03"), (A, "G01"), (B, "G01")]
    chk(forest.since[g1][(_tow(5), (A, "G03"))] == _tow(0) and forest.since[g1][(_tow(6), (A, "G03"))] == _tow(6),
        "the target change at epoch 6 restarts the run of (A, G03)")
    chk(forest.arcs_quiet(g1, _tow(5), arcs_g03, quiet_s) and not forest.arcs_quiet(g1, _tow(3), arcs_g03, quiet_s)
        and not forest.arcs_quiet(g1, _tow(6), arcs_g03, quiet_s)
        and not forest.arcs_quiet(g1, _tow(9), arcs_g03, quiet_s)
        and forest.arcs_quiet(g1, _tow(3), arcs_g03, quiet_s, short=True),
        "path quiet at epoch 5; not at 3 (short history; quiet when truncated), not at 6 (event), not at 9 "
        "((A, G03) no longer a tree edge)")
    flip = Edge(_tow(0), "G", "signal", L1, 0, A, A, "G01", A, "G01", 1, 3.0, 0.0, "Kept", 0, "none", 0, 0)
    f2 = Forest([flip, flip._replace(tow=_tow(1)), flip._replace(tow=_tow(2), child="G01", parent=A),
                 flip._replace(tow=_tow(3))])
    chk([f2.since[g1][(_tow(k), (A, "G01"))] for k in range(4)] == [_tow(0), _tow(0), _tow(2), _tow(3)],
        "a flipped parent-child relation restarts the run, and flipping back restarts it again")
    gap = Forest([flip, flip._replace(tow=_tow(1)), flip._replace(tow=_tow(3)),
                  flip._replace(tow=_tow(2), system="E", band="L1X")])
    chk(gap.since[g1][(_tow(1), (A, "G01"))] == _tow(0) and gap.since[g1][(_tow(3), (A, "G01"))] == _tow(3),
        "an epoch of the trace without the arc breaks its run")
    slip_rows = [flip._replace(tow=_tow(k), row="TreeEdgeSlip" if k == 2 else "Kept") for k in range(4)]
    chk([Forest(slip_rows).since[g1][(_tow(k), (A, "G01"))] for k in range(4)] == [_tow(0)] * 4
        and [Forest(slip_rows, strict_rows=True).since[g1][(_tow(k), (A, "G01"))] for k in range(4)]
        == [_tow(0), _tow(0), _tow(2), _tow(2)],
        "a tree-edge slip row on an unchanged edge ends the run only with --strict-rows")

    # -- stop ----------------------------------------------------------------------------------
    chords = _world_chords()
    res = stop_census(forest, chords, quiet_s)
    s1, s2 = stats(res[L1]["quiet"]), stats(res[L2]["quiet"])
    chk(s1["n"] == 7 and s2["n"] == 9 and len(res[L1]["all"]) == 21 and len(res[L2]["all"]) == 21,
        "event-free chord-epochs: band 1 7 of 21, band 2 9 of 21 (n %d, %d)" % (s1["n"], s2["n"]))
    chk(near(s1["p50"], 0.006) and near(s1["p90"], 0.0084) and near(s2["p50"], 0.007),
        "planted fractions read back: band 1 p50 0.006, p90 0.0084; band 2 p50 0.007")
    w1, w2 = res[L1]["why"], res[L2]["why"]
    chk(len(res[L1]["joint"]) == 7 and len(res[L2]["joint"]) == 7,
        "event-free in both bands: 7 chord-epochs")
    chk(dict(w1) == {"event-free": 7, "trace history shorter than the window": 8,
                     "a path arc changed (or was not a tree edge) in the window": 2,
                     "the chord row was absent in the window": 4}
        and dict(w2) == {"event-free": 9, "trace history shorter than the window": 8,
                         "the chord row was absent in the window": 4},
        "exclusion reasons: epochs 0 to 3 lack history (8 rows), (B, G03) epochs 6 and 7 saw the band-1 "
        "target change (2), (B, G02) epochs 10 and 11 and (C, G01) lack an unbroken chord run (4)")
    chk(float(res[L1]["all"].max()) > 0.39 and stats(res[L1]["all"])["n"] == 21,
        "all chords as context include the planted 0.4 rows")
    chk(stop_verdict(res, 0.02)[0] == "PASS" and stop_verdict(res, 0.0065)[0] == "FAIL"
        and stop_verdict({}, 0.02)[0] == "NOT GRADEABLE",
        "verdict: p50 <= 0.02 passes; a bar of 0.0065 fails band 2; no data is not gradeable")
    sh = stop_census(forest, chords, quiet_s, short=True)
    chk(len(sh[L1]["quiet"]) == 15 and len(sh[L2]["quiet"]) == 17,
        "--short-window: truncated at the first epoch, band 1 15 and band 2 17 event-free chord-epochs")
    chk(near(stats(np.array([0.1, 0.2, 0.3, 0.4]))["p50"], 0.25)
        and near(stats(abs_wrap(np.array([0.5, 0.97])))["p50"], 0.265)
        and near(float(abs_wrap(-22.52)), 0.48),
        "median arithmetic; |wrap(0.97)| = 0.03, |wrap(0.5)| = 0.5, |wrap(-22.52)| = 0.48")

    # -- cert -----------------------------------------------------------------------------------
    cg = cert_grade(forest, truth, chords, settle_s=quiet_s)
    c1, c2 = cg[L1], cg[L2]
    chk(c1["rows"] == 21 and c1["det"] == 8 and c1["correct"] == 4 and c1["wrong"] == 2
        and sum(c1["ungradeable"].values()) == 2,
        "band 1: 21 rows, 8 determined, 4 correct, 2 wrong, 2 ungradeable (no truth)")
    chk(c2["det"] == 7 and c2["correct"] == 5 and c2["wrong"] == 0 and sum(c2["ungradeable"].values()) == 2,
        "band 2: 7 determined, 5 correct, 0 wrong, 2 ungradeable")
    chk([(w["tow"], w["int"], w["truth"]) for w in c1["wrong_list"]] == [(_tow(6), 7, 8), (_tow(7), 9, 8)],
        "the wrong list names the epoch, the integer and the truth (7 and 9 against 8 after the frame step)")
    chk(c1["settled"].size == 9 and c2["settled"].size == 9 and float(np.max(np.abs(c1["settled"]))) < 0.5,
        "float error: 9 rows per band on chords present for 2 min, floats minus truth within the planted "
        "fractions (max %.3f)" % float(np.max(np.abs(c1["settled"]))))
    chk(c1["steps"].shape == (1, 2) and near(c1["steps"][0, 0], 1.0) and near(c1["steps"][0, 1], 1.406, 1e-6)
        and c2["steps"].shape == (0, 2),
        "frame-step check: the band-1 truth of (B, G03) moves 7 -> 8 at epoch 6 and the float follows by "
        "1.406 (planted 0.4 and -0.006); band 2 has no step")
    cg0 = cert_grade(forest, DictTruth(TRUTH_N), chords, settle_s=quiet_s)
    chk(cg0[L1]["wrong"] > cg[L1]["wrong"] and cg0[L2]["wrong"] > 0,
        "without the wind-up branch the (B, G02) integers read wrong")

    # -- re-hosts, by category (extra rows on top of the world) -------------------------------
    def rh(tow, band, sta, sat, row, cert, target):
        return Edge(tow, "G", "signal", band, 0, A, sta, sat, sat, sta, 2, target, 0.0, row, cert,
                    "pair" if cert else "none", 0, 0)
    ex = [rh(_tow(8), L1, B, "G02", "RehostCertified", 1, 1.0),         # right integer, target kept
          rh(_tow(8), L1, B, "G03", "RehostCertified", 1, 9.0),         # certified 9, truth 8: wrong
          rh(_tow(8), L1, C, "G01", "RehostUncertified", 0, 0.0),       # determined, no truth
          rh(_tow(8), L1, C, "G03", "RehostUncertified", 0, 0.0),       # no previous row
          rh(_tow(3), L1, B, "G02", "RehostUncertified", 0, 0.0),       # previous row, no integer; j = -1
          rh(_tow(3), L2, B, "G02", "RehostCertified", 1, 14.0)]        # certified without an integer
    per, wrong_cert, steps = rehost_grade(forest, truth, chords, ex)
    a = per[("RehostCertified", L1)]
    chk(a["n"] == 2 and a["table"]["determined"] == 2 and a["int"]["correct"] == 1 and a["int"]["wrong"] == 1
        and a["cert_n"] == 2 and a["cert_int"]["wrong"] == 1,
        "certified re-hosts band 1: 2 rows, 2 determined, 1 right, 1 wrong")
    chk(len(wrong_cert) == 1 and wrong_cert[0]["sat"] == "G03" and wrong_cert[0]["int"] == 9
        and wrong_cert[0]["truth"] == 8, "the wrong certified re-host names its integer 9 and truth 8")
    b = per[("RehostUncertified", L1)]
    chk(b["n"] == 3 and b["table"]["determined"] == 1 and b["table"]["no row"] == 1
        and b["table"]["row, no integer"] == 1 and b["int"]["ungradeable"] == 1,
        "uncertified re-hosts band 1: 3 rows (determined but ungradeable, no row, row without integer)")
    chk(steps[0] == 2 and steps[1] == 1 and steps[-1] == 1 and a["step"]["integer step j != 0"] == 1,
        "pin targets against the incumbent frame: j = 0 twice (targets 1 and 14), +1 (target 9), -1 (target 0)")
    d = per[("RehostCertified", L2)]
    chk(d["incons"]["certified without a determined integer"] == 1,
        "a certified row without a determined previous integer is reported as inconsistent")

    # -- calibration arithmetic ------------------------------------------------------------------
    chk(calibrate(0.015, 0.02, 300)[0] == "STANDS" and calibrate(0.017, 0.02, 300)[0] == "MISSED"
        and near(calibrate(0.017, 0.02, 300)[1], 0.02125) and calibrate(0.5, 0.02, 199)[0] == "STANDS"
        and calibrate(0.016, 0.02, 200)[0] == "STANDS",
        "calibration: F 0.015 stands, F 0.017 is MISSED and the bar becomes 0.02125, n < 200 stands, "
        "F = 0.8 B stands")

    # -- the traces on disk: loaders, consistency, the three modes ------------------------------
    with tempfile.TemporaryDirectory() as td:
        fx, run = _write_world(td)
        e2, c2_ = read_edges(run), read_chords(run)
        chk(e2 == edges and len(c2_) == len(chords) and c2_[0].tow == chords[0].tow,
            "the loaders read back the written world (%d edge rows, %d chord rows)" % (len(e2), len(c2_)))
        cons = trace_consistency(Forest(e2), c2_, e2)
        chk(not cons, "the synthetic traces are consistent (%s)" % dict(cons))
        bad = Chord(_tow(2), "G", L1, L2, A, "G01", 0.0, 0.0, 0.1, 0.1, None, None, "none", 0, None, None)
        cons = trace_consistency(Forest(e2), c2_ + [bad], e2)
        chk(cons["chord row whose arc is a tree edge of band 1 at the same epoch"] == 1,
            "a chord row on a tree edge is reported")
        tilted = [e._replace(depth=e.depth + 1) if i == 0 else e for i, e in enumerate(e2)]
        chk(trace_consistency(Forest(tilted), [], tilted)["depth differs from the path length"] == 1,
            "a wrong depth is reported")

        fcx = ar_crosscheck(Forest(e2), c2_, read_ar_floats(run))
        chk(fcx["n_cycles"] == 42 and fcx["matched"] == 10 and fcx["max_abs"] < 1e-5,
            "formula check: 42 four-cycle chord rows, 10 matched in the producer's DD trace, equal to 1e-6 "
            "(max %.1e)" % fcx["max_abs"])
        ar_path = os.path.join(run, AR_REL)
        with open(ar_path, encoding="utf-8") as fh:
            ar_lines = fh.read().splitlines()
        cols = ar_lines[1].split(",")
        cols[9] = "%.6f" % (float(cols[9]) + 0.25)
        with open(ar_path, "w", newline="") as fh:
            fh.write("\n".join([ar_lines[0], ",".join(cols)] + ar_lines[2:]) + "\n")
        fcx = ar_crosscheck(Forest(e2), c2_, read_ar_floats(run))
        chk(near(fcx["max_abs"], 0.25, 1e-6) and fcx["matched"] == 10,
            "formula check: a DD float 0.25 cycle off the chord algebra is the largest gap (%.3f)" % fcx["max_abs"])
        os.remove(ar_path)
        chk(read_ar_floats(run) is None and not os.path.isfile(ar_path),
            "a run without a DD trace skips the formula check")
        with open(ar_path, "w", newline="") as fh:
            fh.write("\n".join(ar_lines) + "\n")

        ns = argparse.Namespace(run=run, systems="G", quiet_min=2.0, short_window=False, strict_rows=False,
                                bar=0.02, json=None)
        (r, rc), text = _captured(run_stop, ns)
        chk(rc == 0 and r["bands"][L1]["event_free"]["n"] == 7 and r["bands"][L2]["event_free"]["n"] == 9
            and r["verdict"] == "PASS", "stop on disk: n 7 and 9, PASS")
        chk(0 <= text.index("n (before any verdict)") < text.index("STOP RULE:"),
            "stop prints its sample sizes before the verdict")
        ns.bar = 0.0065
        chk(_captured(run_stop, ns)[0][1] == 1, "stop on disk with a bar below the planted fractions fails (exit 1)")

        ns = argparse.Namespace(run=run, fixture=fx, systems="G", no_windup_branch=False, max_list=20,
                                settle_min=2.0, json=None)
        (r, rc), text = _captured(run_cert, ns)
        chk(rc == 1 and r["bands"][L1]["wrong"] == 2 and r["bands"][L2]["wrong"] == 0
            and r["rehosts"]["wrong_certified"] == 1 and r["bands"][L1]["determined"] == 8
            and r["bands"][L1]["float_error_n"] == 9 and r["bands"][L1]["frame_step_n"] == 1
            and r["formula_check"]["matched"] == 10 and r["formula_check"]["max_abs"] < 1e-5,
            "cert on disk: wrong 2 and 0, the wrong certified re-host found, float-error, frame-step and "
            "formula checks (exit 1)")
        chk("AGREE" in text and text.index("formula check") < text.index("n (before any verdict)"),
            "the formula check is printed, and agrees, on the synthetic traces")
        chk(0 <= text.index("n (before any verdict)") < text.index("WOULD-BE CERTIFICATION:")
            and "WRONG tow" in text and "WRONG CERTIFIED RE-HOST" in text,
            "cert prints its sample sizes before the verdict and lists the wrong integers")
        ns.no_windup_branch = True
        chk(_captured(run_cert, ns)[0][0]["bands"][L2]["wrong"] > 0,
            "cert on disk without the wind-up branch grades wrong")

        knl = WIC.DUAL_FREQ["G"]["2"] / (WIC.DUAL_FREQ["G"]["1"] - WIC.DUAL_FREQ["G"]["2"])
        for src in ("prehold", "products"):
            ns = argparse.Namespace(run=run, fixture=fx, source=src, systems="G", bands="L1C,L2W",
                                    quiet_min=2.0, short_window=False, strict_rows=False, start_tow=None,
                                    start_hms=None, hours=None, min_n=200, json=None)
            (r, rc), text = _captured(run_floor, ns)
            ch = r["channels"]
            chk(ch["E1"]["event_free"]["n"] == 12 and ch["E2"]["event_free"]["n"] == 24
                and ch["WL"]["event_free"]["n"] == 12 and ch["NL"]["event_free"]["n"] == 12,
                "floor (%s): event-free pair-epochs E1 12, E2 24, WL 12, NL 12" % src)
            chk(near(ch["E1"]["event_free"]["p50"], 0.01, 1e-6) and near(ch["E1"]["event_free"]["p99"], 0.01, 1e-6)
                and ch["E2"]["event_free"]["p99"] < 1e-6 and ch["E1"]["all_pairs"]["p99"] > 0.3,
                "floor (%s): E1 p50 = p99 = 0.01 (the 0.4 planted in the event epochs is excluded; all pairs "
                "p99 %.2f)" % (src, ch["E1"]["all_pairs"]["p99"]))
            chk(near(ch["NL"]["event_free"]["p50"], 0.01 * (1 + knl), 1e-6)
                and all(c["verdict"] == "STANDS" for c in r["calibration"]),
                "floor (%s): g = D1 + f2/(f1-f2) (D_WL - round(D_WL)) = %.5f; n < 200, every bar stands"
                % (src, 0.01 * (1 + knl)))
            chk(0 <= text.index("n (before any verdict)") < text.index("calibration"),
                "floor (%s) prints its sample sizes before the calibration verdict" % src)
        ns.start_tow, ns.hours = _tow(4), 0.05
        (r, rc), _text = _captured(run_floor, ns)
        chk(r["counts"]["wire epochs graded"] == 6 and r["counts"]["pair-epochs"] == 18,
            "floor: --start-tow and --hours keep the epochs 4 to 9 (6 wire epochs, 18 pair-epochs)")

        out_json = os.path.join(td, "stop.json")
        argv, sys.argv = sys.argv, ["ird_frame_census.py", "stop", run, "--quiet-min", "2", "--json", out_json]
        try:
            rc = _captured(main)[0]
        finally:
            sys.argv = argv
        with open(out_json, encoding="utf-8") as fh:
            js = json.load(fh)
        chk(rc == 0 and js["verdict"] == "PASS" and js["bands"][L1]["event_free"]["n"] == 7,
            "the command line runs a mode and --json writes its result")
    print("frame census selftest: %s" % ("PASS" if ok[0] else "FAIL"))
    return 0 if ok[0] else 1


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="run the synthetic-trace selftest and exit")
    sub = ap.add_subparsers(dest="mode")

    def common(p, fixture):
        p.add_argument("run", help="run directory holding logs/network_chain_edges.csv")
        if fixture:
            p.add_argument("fixture", help="simulator truth tree (<fixture>/<sta>/truth_*.csv)")
        p.add_argument("--systems", default="G", help="comma list of systems graded (default G)")
        p.add_argument("--json", default=None, help="write the result as JSON")

    def window(p):
        p.add_argument("--quiet-min", type=float, default=QUIET_MIN_DEFAULT,
                       help="event-free window in minutes (default 30)")
        p.add_argument("--short-window", action="store_true",
                       help="let the window be truncated at the first epoch of the trace")
        p.add_argument("--strict-rows", action="store_true",
                       help="a TreeEdgeSlip, RootHandover, Split or Merge row on an unchanged tree edge also "
                            "ends its run (the brief's test is the parent, child and target only)")

    p = sub.add_parser("stop", help="static-algebra stop rule (card IRD section 10)")
    common(p, False)
    window(p)
    p.add_argument("--bar", type=float, default=STOP_BAR_CYC, help="per-band p50 bar in cycles (default 0.02)")
    p = sub.add_parser("cert", help="truth correctness of the would-be certifications and re-hosts")
    common(p, True)
    p.add_argument("--no-windup-branch", action="store_true",
                   help="grade against the simulator's raw integers (no per-arc wind-up branch)")
    p.add_argument("--max-list", type=int, default=20, help="wrong integers listed (default 20)")
    p.add_argument("--settle-min", type=float, default=60.0,
                   help="chord age (minutes in the table) from which a float enters the formula check "
                        "(default 60)")
    p = sub.add_parser("floor", help="event-free floor of the W bars (calibration rule)")
    common(p, True)
    p.add_argument("--source", choices=("prehold", "products"), default="prehold",
                   help="wire source: logs/network_pb_prehold.csv (default) or products.txt")
    p.add_argument("--bands", default="L1C,L2W", help="the two phase signals (default L1C,L2W)")
    window(p)
    p.add_argument("--start-tow", type=float, default=None, help="grading start TOW")
    p.add_argument("--start-hms", default=None, help="grading start HH:MM:SS on the run's day")
    p.add_argument("--hours", type=float, default=None, help="graded span (h)")
    p.add_argument("--min-n", type=int, default=CAL_MIN_N, help="calibration needs this many pair-epochs")
    return ap


def main():
    ap = build_parser()
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.mode is None:
        ap.error("a mode (stop, cert, floor) or --selftest is required")
    fn = {"stop": run_stop, "cert": run_cert, "floor": run_floor}[args.mode]
    result, rc = fn(args)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1, default=lambda o: float(o) if isinstance(o, np.floating)
                      else (int(o) if isinstance(o, np.integer) else str(o)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
