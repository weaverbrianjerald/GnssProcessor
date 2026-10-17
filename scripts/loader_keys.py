#!/usr/bin/env python3
"""Which config keys the C++ loader reads.

    python scripts/loader_keys.py KEY [KEY ...]       say whether the loader reads each dotted key
    python scripts/loader_keys.py config CONFIG.json  list the keys of a config that the loader never reads
    python scripts/loader_keys.py selftest

The loader ignores a key it does not know without a word, so an arm that sets a misspelled
or deleted key runs as its base and the comparison measures nothing.  Every tool that builds
arms (scripts/rapid_arm.py, scripts/prm_ab.py, scripts/ird_make_arms.py) asks this module
before it writes one, and refuses the keys that are not read.

The check reads the loader's source (src/core/config/*.cpp) and never runs it.  The loader
reads a key in two shapes:

  1. By its whole dotted path, as in ``jval(json, "network.raim_k_sigma", ...)``.  The string
     literal then holds the path.
  2. By name below a section object it already holds, as in ``cl.value("enable", false)``
     after ``jsection(nw, "closed_loop")``, or ``mn.value("code_std_m", 0.6)`` after
     ``jsection(json, "filter.measurement_noise")``.  The path is then spread over several
     literals of one function.

A dotted key is judged read when its whole path is a string literal anywhere in the loader, or
when one loader function spells it in pieces: the pieces, joined with dots, tile the path
(``filter.measurement_noise`` + ``code_std_m``).  The section may be left out of the pieces
when the function's file or own name says it reads below it (``loadNetworkClosedLoop_`` in
GnssConfigLoadNetwork.cpp for ``network``).  The second rule judges names, not the object they
are read from, so a real leaf name under the wrong section passes.  It refuses what
happened in practice: a typo, a knob deleted from the loader and a section that does not
exist.  Keys of free-form containers (a station list, a signal priority table, a logger level
table) are the caller's to skip.
"""
import glob
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lex import string_literals, strip_comments  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
CONFIG_SRC = ROOT + "/src/core/config"

# The head of a loader function: `[return type] GnssConfig::name(` at the start of a line.
_DEF = re.compile(r'^[ \t]*(?:[\w:<>,&*]+[ \t]+)+GnssConfig::(\w+)[ \t]*\(', re.M)


class LoaderKeys:
    """The string literals of a loader, whole and per function.  Build it from the source text
    of the loader files (a dict of file name to text) or, by default, from src/core/config."""

    def __init__(self, sources=None):
        if sources is None:
            sources = {}
            for path in sorted(glob.glob(CONFIG_SRC + "/*.cpp")):
                with io.open(path, encoding="utf-8", errors="replace") as fh:
                    sources[os.path.basename(path)] = fh.read()
        self.literals = set()
        self.functions = []                 # (file name, function name, set of literals)
        self._by_leaf = {}                  # last name of a literal -> indexes into functions
        self._memo = {}
        for fname, text in sources.items():
            text = strip_comments(text)
            heads = [(m.start(), m.group(1)) for m in _DEF.finditer(text)]
            bounds = [h[0] for h in heads] + [len(text)]
            if not heads:
                heads, bounds = [(0, "")], [0, len(text)]
            for k, (pos, func) in enumerate(heads):
                lits = set(string_literals(text[pos:bounds[k + 1]]))
                self.functions.append((fname, func, lits))
                self.literals |= lits
                for lit in lits:
                    self._by_leaf.setdefault(lit.rsplit(".", 1)[-1], set()).add(len(self.functions) - 1)

    @staticmethod
    def _named_for(section, fname, func):
        """Whether a function's file or own name says it reads below `section`
        (loadNetworkClosedLoop_ in GnssConfigLoadNetwork.cpp for `network`)."""
        flat = section.replace("_", "").lower()
        return flat in fname.lower() or flat in func.lower()

    @staticmethod
    def _tiles(segs, lits):
        """Whether consecutive segments joined with dots, each join a string literal of the
        function, cover all of `segs`."""
        ok = [True] + [False] * len(segs)
        for i in range(1, len(segs) + 1):
            ok[i] = any(ok[j] and ".".join(segs[j:i]) in lits for j in range(i))
        return ok[len(segs)]

    def is_read(self, key):
        """Whether the loader reads the dotted `key` (see the module header for the rule)."""
        if key not in self._memo:
            self._memo[key] = self._is_read(key)
        return self._memo[key]

    def _is_read(self, key):
        if key in self.literals:
            return True
        segs = key.split(".")
        if len(segs) < 2:
            return False
        for k in self._by_leaf.get(segs[-1], ()):
            fname, func, lits = self.functions[k]
            if self._tiles(segs, lits) or (self._named_for(segs[0], fname, func)
                                           and self._tiles(segs[1:], lits)):
                return True
        return False

    def unread(self, keys):
        """The keys of `keys` (dotted paths) the loader does not read, sorted."""
        return sorted({k for k in keys if not self.is_read(k)})


_DEFAULT = None


def default():
    """The loader of this checkout, read once per process."""
    global _DEFAULT
    if _DEFAULT is None:
        _DEFAULT = LoaderKeys()
    return _DEFAULT


def is_read(key):
    """Whether this checkout's loader reads the dotted `key`."""
    return default().is_read(key)


def unread(keys):
    """The dotted `keys` this checkout's loader does not read."""
    return default().unread(keys)


def leaf_paths(node, prefix=""):
    """Every dotted path of a config object down to its leaves.  An empty object and a list are
    leaves: what is inside a list is not a key."""
    for k, v in node.items():
        p = prefix + k
        if isinstance(v, dict) and v:
            yield from leaf_paths(v, p + ".")
        else:
            yield p


def refuse_unread(keys, loader=None):
    """Exit with a message naming the dotted `keys` that the loader does not read, and do
    nothing when all of them are read.  A caller passes the keys it is about to set."""
    missing = (loader or default()).unread(keys)
    if missing:
        raise SystemExit("refused: the config loader does not read %s. An arm that sets a key the "
                         "loader ignores runs as its base and measures nothing; run the tool from "
                         "the checkout whose loader reads it, or fix the spelling."
                         % ", ".join(missing))


def selftest():
    n = 0

    def chk(cond, what):
        nonlocal n
        n += 1
        assert cond, what

    # The three reading shapes, on a small loader.
    src = {"GnssConfigLoadNetwork.cpp": r'''
        void GnssConfig::loadNetworkRaim_(const nlohmann::json& json) {
            // "network.commented_out" is not read: the comment is not code
            raim_ = jval<bool>(json, "network.raim", "network_raim", false);
            const auto& dj = jsection(json, "network.sd_ewl");
            sd_.enable = jval<bool>(json, "network.sd_ewl.enable", "", false);
        }
        void GnssConfig::loadNetworkClosedLoop_(const nlohmann::json& nw) {
            if (nw.contains("closed_loop")) {
                const auto& cl = nw["closed_loop"];
                clc.enable = cl.value("enable", false);
                const auto& sj = cl["station"];
                clc.station.name = sj.value("name", std::string{});
                char q = '"';
            }
            max_duration_s_ = nw.value("max_duration_s", 0.0);
        }
        void GnssConfig::loadOther_(const nlohmann::json& json) {
            const auto& mc = json["model_corrections"];
            e_ = mc.value("enable_troposphere", true);
            other_ = jval<double>(json, "processing.max_duration_s", "max_duration_s", 0.0);
            tail_ = jval<double>(nw, "raim.sigma", "", 0.0);
        }
    '''}
    lk = LoaderKeys(src)
    chk(lk.is_read("network.raim"), "a whole dotted literal")
    chk(lk.is_read("network_raim"), "the flat legacy spelling")
    chk(lk.is_read("network.sd_ewl.enable"), "a whole dotted literal three deep")
    chk(lk.is_read("network.closed_loop.enable"), "a leaf read below a section object")
    chk(lk.is_read("network.closed_loop.station.name"), "a leaf read two objects below the section")
    chk(lk.is_read("network.max_duration_s"), "a leaf read below the section object, two deep")
    chk(lk.is_read("model_corrections.enable_troposphere"), "a section the function names")
    chk(lk.is_read("processing.max_duration_s"), "a dotted literal of another section")
    chk(lk.is_read("network.raim.sigma"), "a dotted tail read below a section object")
    chk(not lk.is_read("network.commented_out"), "a name that only a comment spells")
    chk(not lk.is_read("network.raim_typo"), "a misspelled leaf")
    chk(not lk.is_read("network.closed_loop.enabel"), "a misspelled leaf below a section object")
    chk(not lk.is_read("network.closed_loop.nosuch.name"), "a middle segment the loader never spells")
    chk(not lk.is_read("nosuch.raim"), "a section that does not exist")
    chk(not lk.is_read("network"), "a bare section name is not a key")
    chk(lk.unread(["network.raim", "network.gone", "network.gone"]) == ["network.gone"],
        "unread lists each missing key once")
    chk(sorted(leaf_paths({"a": {"b": 1, "c": {"d": [1, 2], "e": {}}}, "f": "x"}))
        == ["a.b", "a.c.d", "a.c.e", "f"], "leaf paths stop at lists and empty objects")
    try:
        refuse_unread(["network.gone"], loader=lk)
        chk(False, "a missing key must be refused")
    except SystemExit as ex:
        chk("network.gone" in str(ex), "the refusal names the key")

    # The loader of this checkout, on the keys the network golden configs set.  Every key must
    # be read except the free-form logger level table, the comment key, and two keys the
    # goldens carry that the loader never reads (they run as nothing: the census is armed by
    # network.redundancy_census_stride, the station floor by network.parallel_min_stations).
    inert = {"_comment", "network.min_stations", "network.redundancy_census"}
    for name in ("golden_field_refound.json", "golden_sim_refound.json"):
        with io.open(ROOT + "/config/golden/" + name, encoding="utf-8") as fh:
            cfg = json.load(fh)
        miss = [k for k in default().unread(leaf_paths(cfg))
                if k not in inert and not k.startswith("logging.levels.")]
        chk(not miss, "%s: the loader reads every key the golden sets, not %s" % (name, miss))
    # The closed-loop consumer station and the signal priority table are read below their
    # section object, as the census scripts and the arm tools set them.
    for k in ("network.closed_loop.station.name", "network.closed_loop.station.rinex_path",
              "network.closed_loop.station.position_ecef", "network.closed_loop.replay_products_path",
              "network.start_offset_hours", "processing.signal_priorities.GPS",
              "filter.measurement_noise.code_std_m"):
        chk(is_read(k), "read in this checkout: " + k)
    # Keys deleted from the loader stay refused.
    for k in ("network.emit_pb_filter", "network.sh_health_gate", "network.mw_wl.ewl_enable",
              "network.lane_carry.enable", "network.carry_invariant_honesty"):
        chk(not is_read(k), "a deleted key is refused: " + k)
    print("loader_keys selftest: %d/%d PASS" % (n, n))


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    if argv[0] == "selftest":
        selftest()
        return 0
    if argv[0] == "config":
        if len(argv) != 2:
            print("usage: loader_keys.py config CONFIG.json")
            return 2
        with io.open(argv[1], encoding="utf-8") as fh:
            cfg = json.load(fh)
        missing = unread(leaf_paths(cfg))
        for k in missing:
            print("unread  " + k)
        print("%d key(s) the loader does not read" % len(missing))
        return 1 if missing else 0
    bad = 0
    for k in argv:
        r = is_read(k)
        bad += not r
        print("%-6s %s" % ("read" if r else "UNREAD", k))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
