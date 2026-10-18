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
exist.

Three more shapes are read, because the loader builds those paths where no literal holds them:

  3. A prefix constant joined to a literal.  A function that declares
     ``const std::string pre = "network.ambiguity_resolution.fixed_cycles.";`` and reads
     ``jval(json, pre + "fold", ...)`` reads the path ``pre`` + ``"fold"``.  The constant must be
     a dotted path ending in a dot and the join must stand in the same function.
  4. A class table.  ``network.process_noise.<class>`` is read by looking each member of the
     object up in ``kNetworkQClassTable`` (include/config/GnssConfigTypes.hpp), so the class
     names of that table are the keys of the section.
  5. A free-form table of names the caller chooses, ``logging.levels.<logger>``: any name
     below it is read once the table itself is.

A key below another free-form container the loader does not list by name (a station list) is
judged by the pieces rule above or is the caller's to skip.
"""
import glob
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lex import CPP_TOKEN, string_literals, strip_comments  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
CONFIG_SRC = ROOT + "/src/core/config"
CONFIG_HEADERS = [ROOT + "/include/config/GnssConfigTypes.hpp"]

# Sections whose members the loader finds by looking each one up in a constexpr table: the
# dotted section path and the table, whose rows start with the member name.
NAME_TABLES = {"network.process_noise": "kNetworkQClassTable"}

# Tables of names the caller chooses: any name below the path is read once the path is.
FREE_FORM_TABLES = ("logging.levels",)

# The head of a loader function: `[return type] GnssConfig::name(` at the start of a line.
_DEF = re.compile(r'^[ \t]*(?:[\w:<>,&*]+[ \t]+)+GnssConfig::(\w+)[ \t]*\(', re.M)


def _read_files(paths):
    """File name to text of each existing file of `paths`."""
    out = {}
    for path in paths:
        if os.path.isfile(path):
            with io.open(path, encoding="utf-8", errors="replace") as fh:
                out[os.path.basename(path)] = fh.read()
    return out


def _code_tokens(text):
    """The tokens of `text` that are code, as (kind, text): comments and white space dropped."""
    return [(m.lastgroup, m.group()) for m in CPP_TOKEN.finditer(text)
            if m.lastgroup not in ("lc", "bc") and not m.group().isspace()]


def _unquote(tok):
    """The contents of a string literal token."""
    return tok[tok.index('"') + 1:].removesuffix('"')


def prefix_joins(text):
    """The paths `text` builds as a prefix constant joined to a literal.

    ``pre = "network.x.";`` and then ``pre + "fold"`` give ``network.x.fold``.  Only a constant
    that is a dotted path ending in a dot counts, so a message buffer is not mistaken for one."""
    toks = _code_tokens(text)
    prefixes = {}
    for i in range(1, len(toks) - 2):
        if (toks[i][1] == "=" and toks[i - 1][0] == "other" and re.fullmatch(r"[A-Za-z_]\w*", toks[i - 1][1])
                and toks[i + 1][0] == "str" and toks[i + 2][1] == ";"):
            lit = _unquote(toks[i + 1][1])
            if re.fullmatch(r"\w+(?:\.\w+)*\.", lit):
                prefixes[toks[i - 1][1]] = lit
    joins = set()
    for i in range(len(toks) - 2):
        if toks[i][1] in prefixes and toks[i + 1][1] == "+" and toks[i + 2][0] == "str":
            joins.add(prefixes[toks[i][1]] + _unquote(toks[i + 2][1]))
    return joins


def table_names(text, table):
    """The first string of each row of the constexpr table `table` in the C++ source `text`:
    ``kT{ { { "name", ... }, { "name2", ... } } }`` gives name and name2.  A row is a brace
    group that opens on a string, inside the braces that follow the table's name."""
    toks = _code_tokens(strip_comments(text))
    start = next((i for i, t in enumerate(toks) if t[1] == table), len(toks))
    names, depth = [], 0
    for i in range(start, len(toks)):
        kind, tok = toks[i]
        if kind == "other" and tok == "{":
            depth += 1
        elif kind == "other" and tok == "}":
            depth -= 1
            if depth <= 0 and names:
                break
        elif kind == "str" and depth >= 2 and toks[i - 1][1] == "{":
            names.append(_unquote(tok))
    return names


class LoaderKeys:
    """The string literals of a loader, whole and per function.  Build it from the source text
    of the loader files (a dict of file name to text) or, by default, from src/core/config.
    `headers` (file name to text) holds the class tables of NAME_TABLES; it defaults to the
    checkout's header with the default sources and to none with sources given."""

    def __init__(self, sources=None, headers=None):
        if headers is None:
            headers = _read_files(CONFIG_HEADERS) if sources is None else {}
        if sources is None:
            sources = _read_files(sorted(glob.glob(CONFIG_SRC + "/*.cpp")))
        self.literals = set()
        self.functions = []                 # (file name, function name, set of literals)
        self._by_leaf = {}                  # last name of a literal -> indexes into functions
        self._memo = {}
        self.table_members = {sect: {n for text in headers.values() for n in table_names(text, tbl)}
                              for sect, tbl in NAME_TABLES.items()}
        for fname, text in sources.items():
            text = strip_comments(text)
            heads = [(m.start(), m.group(1)) for m in _DEF.finditer(text)]
            # The text before the first member function holds the file's own helpers (a
            # function in an anonymous namespace), whose literals are loader reads too.
            chunks = [(0, heads[0][0] if heads else len(text), "")]
            chunks += [(pos, (heads[k + 1][0] if k + 1 < len(heads) else len(text)), func)
                       for k, (pos, func) in enumerate(heads)]
            for lo, hi, func in chunks:
                lits = set(string_literals(text[lo:hi])) | prefix_joins(text[lo:hi])
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
        for sect, members in self.table_members.items():
            if key.startswith(sect + ".") and key[len(sect) + 1:] in members and self.is_read(sect):
                return True
        for table in FREE_FORM_TABLES:
            if key.startswith(table + ".") and self.is_read(table):
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


def refuse_unread(keys, loader=None, allow=()):
    """Exit with a message naming the dotted `keys` that the loader does not read, and do
    nothing when all of them are read.  A caller passes the keys it is about to set.  A key
    named in `allow` is let through: the escape for a key the loader reads by a route this
    check cannot see (the tool offers it as --allow-unread)."""
    allowed = set(allow or ())
    missing = [k for k in (loader or default()).unread(keys) if k not in allowed]
    if missing:
        raise SystemExit("refused: no read of %s was found in the config loader (src/core/config). "
                         "A key the loader ignores makes the arm its base, so the comparison "
                         "measures nothing. The usual causes are a misspelled key, a key deleted "
                         "from the loader and a key of a branch this checkout does not have. If the "
                         "loader reads the key by a route this check cannot see (a path it builds "
                         "at run time), name it with --allow-unread KEY."
                         % ", ".join(missing))


# The keys the 24 arms of the network gate set (G5) set, as their rapid.json records list them.
# They include the fixed-cycle keys, which the loader reads through a prefix constant.  The
# selftest checks them even where the run directories are not on disk.
_G5_ARM_KEYS = """
estimation.global_vtec_sh.rep_error_d5_keep estimation.global_vtec_sh.rep_error_demean
estimation.global_vtec_sh.rep_error_tau_s network.ambiguity_resolution.block_search
network.ambiguity_resolution.cycle_basis network.ambiguity_resolution.decorrelated_partial
network.ambiguity_resolution.fixed_cycles.emit_measurement_only
network.ambiguity_resolution.fixed_cycles.fold network.ambiguity_resolution.fixed_cycles.source
network.ambiguity_resolution.fixed_cycles.trace network.ambiguity_resolution.suspect_epochs
network.ambiguity_resolution.trace_lattice network.ambiguity_resolution.tree_basis
network.block_baarda network.carry_invariant_innov_corroborate network.carry_invariant_innov_gate
network.carry_invariant_innov_retap_only network.carry_vce_components network.clock_monitor
network.clock_monitor_couple_raim network.clock_monitor_jump_max_m network.congruence_screen
network.cycle_slip.anchor_slip_handover network.cycle_slip.gf_arc_jump
network.cycle_slip.gf_arc_jump_k network.cycle_slip.if_phase_td
network.cycle_slip.if_phase_td_reseed_on_flag network.datum_graph_census
network.epoch_decimation_n network.inject network.ledger_anchor network.mdb
network.phase_datum_gate_drop_erase network.phase_datum_held_basis
network.phase_datum_prioritized network.phase_datum_sticky network.raim_min_stations
network.raim_sigma0_floor network.raim_sigma0_trigger network.raim_withhold_free_erase
network.raim_withhold_sigma0 network.rank_gate_tol network.redundancy_census_stride
network.rhs_attrib_n_ep network.rhs_attrib_on_trip network.rhs_attrib_tow
network.start_offset_hours network.state_snapshot_carry network.state_snapshot_carry_lin
network.state_snapshot_datum_capture network.state_snapshot_emit_capture
network.state_snapshot_interval_s network.state_snapshot_path network.state_snapshot_require
network.state_snapshot_rxcb_lin_capture network.state_snapshot_signal_capture
network.state_snapshot_station_capture network.trace_clock_full network.trace_postfit_keyed
network.trace_postfit_rows network.trace_postfit_sigma0 network.trace_prefit_census
network.trace_sh_spectrum network.trace_sh_spectrum_stride network.trace_station_block
network.trace_topology network.ura_retro_bump
""".split()

# The key names of the fixed-cycle block, as the loader's helper for it reads them.
_FIXED_CYCLE_LEAVES = ("fold", "source", "sigma_cyc", "unit_alpha", "gate_alpha", "max_iter",
                       "max_shift_m", "persist_ep", "lapse_ep", "trace", "emit_measurement_only")


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

    # The three shapes with no whole literal: a prefix constant joined to a literal, a class
    # table and a free-form table, on a small loader and a small header.
    src3 = {"GnssConfigLoadNetwork.cpp": r'''
        namespace {
        void loadFixedCycleKnobs(const nlohmann::json& json) {
            const std::string pre = "network.ambiguity_resolution.fixed_cycles.";
            const std::string sect = "network.sect";
            auto refuse = [&](const std::string& key) { throw std::runtime_error(pre + key + " bad"); };
            fold = jval<std::string>(json, pre + "fold", "", "off");
            iter = jval<int>(json, pre + "max_iter", "", 3);
            odd = jval<int>(json, sect + "tail", "", 3);
        }
        }
        bool GnssConfig::loadNetworkProcessNoise_(const nlohmann::json& json) {
            const auto& pn = jsection(json, "network.process_noise", "network_process_noise");
            for (const NetworkQDesc& d : kNetworkQClassTable) v = pn.value(d.name, d.fallback);
        }
        void GnssConfig::loadLogging_(const nlohmann::json& json) {
            const auto& lg = jsection(json, "logging");
            if (lg.contains("levels")) {}
        }
    '''}
    hdr3 = {"GnssConfigTypes.hpp": r'''
        /// "decoy" is in a comment
        inline constexpr std::array<NetworkQDesc, 2> kNetworkQClassTable{ {
            { "sat_clock", "filter.process_noise.sat_clock_q", "sat_clock_q", 1e-2, "m^2/s", "always" },
            { "vtec_sh",   "estimation.global_vtec_sh.q_per_degree", "q", 1.0e-2, "TECU^2/s", "gate" },
        } };
        inline constexpr int kAfter = 3;
    '''}
    chk(table_names(hdr3["GnssConfigTypes.hpp"], "kNetworkQClassTable") == ["sat_clock", "vtec_sh"],
        "the class names are the first string of each table row, and only those")
    chk(table_names("int x;", "kNetworkQClassTable") == [], "a missing table has no names")
    lk3 = LoaderKeys(src3, hdr3)
    for k in ("network.ambiguity_resolution.fixed_cycles.fold",
              "network.ambiguity_resolution.fixed_cycles.max_iter",
              "network.process_noise.sat_clock", "network.process_noise.vtec_sh",
              "logging.levels.kalman_filter", "logging.levels.any_logger_name"):
        chk(lk3.is_read(k), "read by a prefix constant, a class table or a free-form table: " + k)
    for k in ("network.ambiguity_resolution.fixed_cycles.nosuch",
              "network.ambiguity_resolution.fixed_cycles.max_iterx",
              "network.secttail", "network.process_noise.nosuch", "network.process_noise.iono",
              "network.process_noise.decoy", "network.process_noise.kAfter", "logging.nosuch",
              "logging.levelsx.kalman_filter", "network.nosuch.levels.kalman_filter"):
        chk(not lk3.is_read(k), "refused though a table or prefix shape is near it: " + k)
    lk4 = LoaderKeys({"GnssConfigLoadNetwork.cpp": 'x = jval(json, "network.raim", "", false);'}, hdr3)
    chk(not lk4.is_read("network.process_noise.sat_clock") and not lk4.is_read("logging.levels.a"),
        "a table member or a logger name is read only when the loader reads the table's section")
    chk(prefix_joins('pre = "network.x."; a = pre + "y"; b = pre + z; pre2 = "no dot"; c = pre2 + "w";')
        == {"network.x.y"}, "only a dotted prefix constant joined to a literal gives a path")
    try:
        refuse_unread(["network.raim", "network.gone"], loader=lk, allow=["network.gone"])
        chk(True, "an allowed key passes")
    except SystemExit:
        chk(False, "an allowed key must pass")
    try:
        refuse_unread(["network.gone", "network.also_gone"], loader=lk, allow=["network.gone"])
        chk(False, "a key that is not allowed must still be refused")
    except SystemExit as ex:
        chk("network.also_gone" in str(ex) and "network.gone " not in str(ex) and "--allow-unread" in str(ex),
            "the refusal names the key that is not allowed and says how to allow one")

    # The loader of this checkout, on the keys the network golden configs set.  Every key must
    # be read except the comment key and two keys the goldens carry that the loader never reads
    # (they run as nothing: the census is armed by network.redundancy_census_stride, the
    # station floor by network.parallel_min_stations).  The logger level table is read.
    inert = {"_comment", "network.min_stations", "network.redundancy_census"}
    for name in ("golden_field_refound.json", "golden_sim_refound.json"):
        with io.open(ROOT + "/config/golden/" + name, encoding="utf-8") as fh:
            cfg = json.load(fh)
        miss = [k for k in default().unread(leaf_paths(cfg)) if k not in inert]
        chk(not miss, "%s: the loader reads every key the golden sets, not %s" % (name, miss))
    # The closed-loop consumer station and the signal priority table are read below their
    # section object, as the census scripts and the arm tools set them.
    for k in ("network.closed_loop.station.name", "network.closed_loop.station.rinex_path",
              "network.closed_loop.station.position_ecef", "network.closed_loop.replay_products_path",
              "network.start_offset_hours", "processing.signal_priorities.GPS",
              "filter.measurement_noise.code_std_m"):
        chk(is_read(k), "read in this checkout: " + k)
    # The keys the loader reads without a whole literal: the fixed-cycle block (a prefix
    # constant), the process-noise classes (a class table) and the logger levels (a free-form
    # table).
    for leaf in _FIXED_CYCLE_LEAVES:
        chk(is_read("network.ambiguity_resolution.fixed_cycles." + leaf), "fixed_cycles." + leaf)
    classes = [nm for text in _read_files(CONFIG_HEADERS).values()
               for nm in table_names(text, NAME_TABLES["network.process_noise"])]
    chk(len(classes) >= 2 and "sat_clock" in classes, "the class table of this checkout is found")
    for nm in classes:
        chk(is_read("network.process_noise." + nm), "process_noise." + nm)
    for k in ("logging.levels.kalman_filter", "logging.levels.any_logger_name"):
        chk(is_read(k), "read in this checkout: " + k)
    # Near-misses of those shapes stay refused, and so do keys deleted from the loader.
    for k in ("network.ambiguity_resolution.fixed_cycles.nosuch", "network.process_noise.nosuch",
              "network.process_noise.iono", "logging.nosuch", "network.fast_clock.enable",
              "network.emit_pb_filter", "network.sh_health_gate", "network.mw_wl.ewl_enable",
              "network.lane_carry.enable", "network.carry_invariant_honesty"):
        chk(not is_read(k), "refused in this checkout: " + k)
    # Every key the arms of the network gate set (G5) set.  The list above holds them; the
    # records are read too where the run directories are on disk, so a new arm is covered.
    arm_keys = set(_G5_ARM_KEYS)
    records = sorted(glob.glob(os.environ.get("RAPID_RUNS_ROOT", "D:/GnssProcessorSupport/runs_nom")
                               + "/_rap_g5_n6base_*/rapid.json"))
    for rec in records:
        with io.open(rec, encoding="utf-8") as fh:
            arm_keys |= {kv.split("=", 1)[0] for kv in json.load(fh).get("set", [])}
    miss = default().unread(arm_keys)
    chk(not miss, "the loader reads every key of the G5 arms (%d keys, %d records), not %s"
        % (len(arm_keys), len(records), miss))
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
