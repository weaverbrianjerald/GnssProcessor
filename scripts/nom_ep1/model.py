"""Row type, realization tables and fold grammar of card NOM3 step EP1 (generator input only).

The EP1 specification (sections 2, 3 and 5.2 to 5.4) converts each per-epoch seam the frozen
lambda processSyncedEpoch calls to `R networkX(NetworkRun& run, const NetworkEpoch& ep[,
kept...][, outputs...])`.  This module states, as data, how the generator realizes a frozen
argument under the new signature:

  Seam                 one canonical-table row (spec 5.2).  It names the seam, its header and
                       source at the base, its mechanism, its outputs and kept parameters, the
                       observer flag, the worker, and how many call sites the frozen lambda has
                       (call sites are named by callee and occurrence, never by line).
  EPOCH_MEMBERS        the lambda locals that become NetworkEpoch members (spec 2.3, 5.3
                       T-B1..T-B8): frozen root L:<local> -> its member path, the transform that
                       realizes it and the member's type text and default initializer.
  EPOCH_FIELD_MOVES    the three hand-off fields that move from the solve result into
                       NetworkPostFitHandoff (spec 2.4).
  RUN_MEMBERS          the driver names that become NetworkRun members (spec 2.1, 2.2): the
                       seven references (REF, the same object) and the eleven run-owned engines
                       and groups (OWNER_MOVE).
  COPY_SOURCES         the four const driver copies of setup fields that bind to the field.
  VALUE_EQUAL_EXPRS    the expression that becomes ep.emitted.
  REMOVED_PARAMS       the std::function parameters that disappear: the third-band predicate
                       (CALLABLE) and the solve's two replay callbacks (CALLBACK_DIRECT).
  OUTPUTS              every output name a row may list, with the epoch path it writes.
  fold_problem()       the fold grammar (spec 3.2): a folded argument is a member chain with an
                       optional & or * prefix, `.get()` on a smart pointer last in a chain,
                       ep.x_emit(), a literal, a pure function of member reads
                       (PURE_FUNCTIONS), or a side-effect-free conditional over member reads;
                       anything else is refused unless the parameter is kept.
  render()             a resolved binding (nom_cpp.Ref, Copy, Cond, CallNode, Lit) as its new
                       text through the tables above, with the problems found on the way.

These tables are the generator's realization; the verifier does not read them.  It accepts a
binding change only through scripts/nom_ep1/declared.py, and the survey checks that every
realization used here is backed by a declared entry there.
"""
from dataclasses import dataclass

from nom_cpp import Ref, Copy, Cond, CallNode, Lit, Expr, Opaque

# ---------------------------------------------------------------------------------------------
# Row type
# ---------------------------------------------------------------------------------------------
DIRECT = "DIRECT"    # a free seam: its definition gains the epoch form
MEMBER = "MEMBER"    # one of the five Application member seams
ADAPTER = "ADAPTER"  # explicit form kept byte-identical (other callers), epoch form added
RUN = "RUN"          # (NetworkRun& run) only: reads no epoch member, not an observer


@dataclass(frozen=True)
class Explicit:
    """Keeps its frozen text; @p reason says why (the input stage, an EP2 block)."""
    reason: str


def EXPLICIT(reason):
    return Explicit(reason)


MECHS = (DIRECT, MEMBER, ADAPTER, RUN)
WORKERS = ("W1", "W2", "W3", "W4")


@dataclass(frozen=True)
class Seam:
    """One canonical-table row (spec 5.2).

    name:     the callee as the frozen lambda spells it.
    hpp, cpp: its declaration header and its defining source, repository-relative.
    mech:     DIRECT | MEMBER | ADAPTER | RUN | EXPLICIT(reason=...).
    outs:     the output groups or single members it writes, in parameter order (keys of
              OUTPUTS).
    kept:     frozen parameter names that stay explicit because their argument differs between
              call sites.
    observer: measure-only; its epoch form must be NetworkEpochObserver.
    worker:   W1 app, W2 pre-solve and solve, W3 emit and post-fit, W4 diagnostics.
    sites:    how many times the frozen lambda calls it (occurrences in source order)."""
    name: str
    hpp: str
    cpp: str
    mech: object
    outs: tuple = ()
    kept: tuple = ()
    observer: bool = False
    worker: str = ""
    sites: int = 1

    @property
    def form(self):
        return self.mech.lower() if isinstance(self.mech, str) else "explicit"


# ---------------------------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------------------------
# An output is an epoch group (the writer receives the whole group; its frozen parameters bind
# `<group>.<member>`, marked written) or a single member of a group another stage owns (the
# frozen parameter itself becomes the output, with its frozen declarator and name).
OUTPUTS = {
    "pass_a": "ep.pass_a",
    "funnel": "ep.funnel",
    "columns": "ep.columns",
    "assembly": "ep.assembly",
    "dd_build": "ep.dd_build",
    "handoff": "ep.solve.handoff",
    "ws": "ep.ws",
    "est_index_of": "ep.assembly.est_index_of",
    "sol": "ep.prep.sol",
}
GROUP_OUTPUTS = ("pass_a", "funnel", "columns", "assembly", "dd_build", "handoff")
SOLVE_CALLEE = "networkSolveGlue"   # after its call site only handoff and sol may be written
POST_SOLVE_OUTPUTS = ("handoff", "sol")


def output_for(path, outs):
    """(output name, binding text) of an epoch @p path written through one of @p outs:
    ('handoff', 'handoff.clock_refloat_sats') for a group, ('ws', None) when the path is the
    output itself; (None, None) when no listed output covers the path."""
    for o in outs:
        op = OUTPUTS.get(o)
        if op is None:
            continue
        if path == op:
            return o, None
        if path.startswith(op + "."):
            return o, o + path[len(op):]
    return None, None


def default_output(path):
    """The output a writer of @p path would list if its row did not say (the group, or the
    single member when no group covers it)."""
    best = None
    for o, op in OUTPUTS.items():
        if path == op or path.startswith(op + "."):
            if best is None or len(op) < len(OUTPUTS[best]):
                best = o
    return best


# ---------------------------------------------------------------------------------------------
# The epoch: lambda locals realized as NetworkEpoch members
# ---------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class EpochMember:
    """local: the frozen lambda name; path: its realization under `ep`; transform: CTOR (the
    reference ep is built on), WS, ALIAS, SIZED, SCALAR, CLOCK, RESULT, ACCESSOR (spec 5.3);
    type, default: the member's type text and default initializer (spec 2.3; '' for none)."""
    local: str
    path: str
    transform: str
    type: str
    default: str = ""

    @property
    def key(self):
        return "L:" + self.local

    @property
    def declared_class(self):
        return {"CTOR": None, "ACCESSOR": "ACCESSOR"}.get(self.transform, "EPOCH_MEMBER")


EPOCH_MEMBERS = (
    EpochMember("se", "ep.se", "CTOR", "const SyncedEpoch&"),
    EpochMember("ws", "ep.ws", "WS", "EpochWorkspace"),
    EpochMember("dt", "ep.clock.dt", "CLOCK", "double", "0.0"),
    EpochMember("pl_now_s", "ep.clock.pl_now_s", "CLOCK", "double", "0.0"),
    EpochMember("carry_warm", "ep.clock.carry_warm", "CLOCK", "bool", "false"),
    EpochMember("carry_slipped_ambs", "ep.funnel.carry_slipped_ambs", "ALIAS",
                "std::set<std::pair<int, std::string>>"),
    EpochMember("carry_slipped_raw", "ep.funnel.carry_slipped_raw", "ALIAS",
                "std::set<std::pair<int, std::string>>"),
    EpochMember("modeled", "ep.pass_a.modeled", "SIZED", "std::vector<std::vector<PppObservation>>"),
    EpochMember("present", "ep.pass_a.present", "SIZED", "std::vector<bool>"),
    EpochMember("shared_phase_keys", "ep.pass_a.shared_phase_keys", "ALIAS", "std::set<std::string>"),
    EpochMember("n_dropped_sats", "ep.pass_a.n_dropped_sats", "SCALAR", "int", "0"),
    EpochMember("registered_clock_sats", "ep.columns.registered_clock_sats", "ALIAS",
                "std::vector<std::string>"),
    EpochMember("registered_code_keys", "ep.columns.registered_code_keys", "ALIAS",
                "std::vector<std::pair<std::string, std::string>>"),
    EpochMember("col_desc", "ep.columns.col_desc", "ALIAS", "std::vector<std::string>"),
    EpochMember("n_clock", "ep.columns.n_clock", "SCALAR", "int", "0"),
    EpochMember("n_pb", "ep.columns.n_pb", "SCALAR", "int", "0"),
    EpochMember("n_cb", "ep.columns.n_cb", "SCALAR", "int", "0"),
    EpochMember("n_present", "ep.columns.n_present", "SCALAR", "int", "0"),
    EpochMember("asm_res", "ep.assembly.asm_res", "SIZED", "std::vector<StationAssembly>"),
    EpochMember("est_index_of", "ep.assembly.est_index_of", "SIZED", "std::vector<int>"),
    EpochMember("total_pins", "ep.assembly.total_pins", "SCALAR", "int", "0"),
    EpochMember("sec_asm_ms_epoch", "ep.assembly.sec_asm_ms_epoch", "SCALAR", "double", "0.0"),
    EpochMember("carry_arc_amb_key", "ep.dd_build.carry_arc_amb_key", "ALIAS",
                "std::map<int, std::map<ArcKey, std::string>>"),
    EpochMember("carry_arc_amb_key2", "ep.dd_build.carry_arc_amb_key2", "ALIAS",
                "std::map<int, std::map<ArcKey, std::string>>"),
    EpochMember("sg_", "ep.solve", "RESULT", "NetworkSolveGlueResult"),
    EpochMember("ar_fixes", "ep.ar_fixes", "RESULT", "ArEpochFixes"),
    EpochMember("fce", "ep.fixed_cycles", "RESULT", "FixedCycleEmission"),
    EpochMember("sp_", "ep.prep", "RESULT", "NetworkSolPrepResult"),
    EpochMember("x_emit", "ep.x_emit()", "ACCESSOR", "const Eigen::VectorXd&"),
)
EPOCH_BY_KEY = {m.key: m for m in EPOCH_MEMBERS}

# (old path, new path): the post-fit hand-off fields (spec 2.4, FIELD_MOVE).
EPOCH_FIELD_MOVES = (
    ("ep.solve.clock_refloat_sats", "ep.solve.handoff.clock_refloat_sats"),
    ("ep.solve.retro_flag_sats", "ep.solve.handoff.retro_flag_sats"),
    ("ep.solve.n_arc_refloat_epoch", "ep.solve.handoff.n_arc_refloat_epoch"),
)

# The one expression a seam argument becomes an epoch value (VALUE_EQUAL), keyed by the frozen
# argument's text with every space removed (Tokens.norm keeps one after `!=`).
VALUE_EQUAL_EXPRS = {"ctx.counters.n_emitted!=fc_n_emitted_before": "ep.emitted"}


def value_equal(text):
    """The epoch value an argument's frozen text becomes under VALUE_EQUAL, or None."""
    return VALUE_EQUAL_EXPRS.get(text.replace(" ", ""))


# ---------------------------------------------------------------------------------------------
# The run: driver names realized as NetworkRun members
# ---------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class RunMember:
    """frozen: the root key (D: a runNetwork outermost declaration, A: an Application member);
    path: its realization under `run`; cls: REF (the run holds a reference to the same object)
    or OWNER_MOVE (the run owns it; the driver's name becomes a reference to it); type: the
    NetworkRun member's type text (spec 2.1, 2.2)."""
    frozen: str
    path: str
    cls: str
    type: str

    @property
    def name(self):
        return self.frozen.split(":", 1)[1]


RUN_MEMBERS = (
    RunMember("D:ctx", "run.ctx", "REF", "NetworkRunContext&"),
    RunMember("A:config_", "run.config", "REF", "const GnssConfig&"),
    RunMember("D:nc", "run.nc", "REF", "const NetworkConfig&"),
    RunMember("D:log", "run.log", "REF", "const std::shared_ptr<spdlog::logger>&"),
    RunMember("D:traces", "run.traces", "REF", "NetworkRunTraces&"),
    RunMember("D:setup", "run.setup", "REF", "NetworkSetupResult&"),
    RunMember("A:log_dir_", "run.log_dir", "REF", "const std::filesystem::path&"),
    RunMember("D:est", "run.engines.est", "OWNER_MOVE", "NetworkEstimator"),
    RunMember("D:carry_driver", "run.engines.carry_driver", "OWNER_MOVE",
              "NetworkInvariantCarryDriver"),
    RunMember("D:carry_vce_est", "run.engines.carry_vce", "OWNER_MOVE", "NetworkCarryVce"),
    RunMember("D:prires_est", "run.engines.prires", "OWNER_MOVE", "NetworkPriorResidual"),
    RunMember("D:slip_qc", "run.engines.slip_qc", "OWNER_MOVE", "SlipQcChannel"),
    RunMember("D:iono_deweight", "run.engines.iono_deweight", "OWNER_MOVE",
              "NetworkIonoDeweightGate"),
    RunMember("D:fc_ppp_utils", "run.engines.fc_ppp_utils", "OWNER_MOVE",
              "std::unique_ptr<PppUtils>"),
    RunMember("D:emit_engines", "run.emit", "OWNER_MOVE", "NetworkEmitEngines"),
    RunMember("D:lane_run", "run.lanes", "OWNER_MOVE", "NetworkLaneRun"),
    RunMember("D:timing", "run.timing", "OWNER_MOVE", "NetworkRunTiming"),
    RunMember("D:cl_tally", "run.closed_loop", "OWNER_MOVE", "ClosedLoopTally"),
)
RUN_BY_KEY = {m.frozen: m for m in RUN_MEMBERS}

# Const driver copies of setup fields bound to the field (COPY_SOURCE): driver name -> the
# field the copy was taken from.  A by-value copy of anything else is refused, except a copy
# of a member of the const knobs (run.ctx.knobs.*, `const NetworkKnobs knobs` in
# NetworkRunContext), which equals its source at every reader.
COPY_SOURCES = {"n_threads": "setup.n_threads", "seam_threads": "setup.seam_threads",
                "carry_par": "setup.carry_par", "brdc_rts": "setup.brdc_rts"}
CONST_RUN_PREFIXES = ("run.ctx.knobs.",)


def copy_source_paths():
    return {"run." + v for v in COPY_SOURCES.values()}


# ---------------------------------------------------------------------------------------------
# Removed std::function parameters
# ---------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Removed:
    """A std::function parameter that the conversion removes: the frozen argument's root,
    the declared class that accepts it, and the text the survey prints for it."""
    seam: str
    param: str
    root: str
    cls: str
    note: str
    builder: str = ""   # CALLBACK_DIRECT: the seam the replay now calls directly


REMOVED_PARAMS = (
    Removed("networkBuildCarryDdPriors", "isUncarriedThirdBand", "D:isUncarriedThirdBand",
            "CALLABLE", "preamble lambda over networkIsUncarriedThirdBand"),
    Removed("networkSolveGlue", "buildEnsembleGauges", "L:buildEnsembleGauges",
            "CALLBACK_DIRECT", "direct call in the T2 replay", "networkBuildEnsembleGauges"),
    Removed("networkSolveGlue", "buildCarryDdPriors", "L:buildCarryDdPriors",
            "CALLBACK_DIRECT", "direct call in the T2 replay", "networkBuildCarryDdPriors"),
)
REMOVED_BY_PARAM = {(r.seam, r.param): r for r in REMOVED_PARAMS}


# ---------------------------------------------------------------------------------------------
# Fold grammar and rendering
# ---------------------------------------------------------------------------------------------
# Pure functions of member reads a folded argument may call (they read their argument only).
PURE_FUNCTIONS = {"TimeUtils::continuousGpsSeconds": "the continuous GPS seconds of a GpsTime; "
                  "reads its argument only (math/TimeUtils)"}


def _member_chain_problem(ref):
    for k, seg in enumerate(ref.chain):
        if seg.startswith("[") or ("(" in seg and seg != ".get()"):
            return f"`{seg}` in a member chain (only member reads and a final .get())"
        if seg == ".get()" and k != len(ref.chain) - 1:
            return "`.get()` before the end of a member chain"
    return None


def fold_problem(node):
    """None when @p node is in the fold grammar, else the reason it is not."""
    if isinstance(node, Ref):
        return _member_chain_problem(node)
    if isinstance(node, Copy):
        return fold_problem(node.inner)
    if isinstance(node, Lit):
        return None
    if isinstance(node, Cond):
        c = node.c
        if isinstance(c, Expr) and c.op == "!" and len(c.args) == 1:
            c = c.args[0]
        if not isinstance(c, (Ref, Copy)) or (isinstance(c, Ref) and c.prefix):
            return "a conditional whose condition is not a member read"
        for part in (c, node.a, node.b):
            if not isinstance(part, (Ref, Copy, Lit)):
                return "a conditional whose branch is not a member read or a literal"
            p = fold_problem(part)
            if p:
                return p
        return None
    if isinstance(node, CallNode):
        if node.fn not in PURE_FUNCTIONS:
            return f"a call of {node.fn} (not a listed pure function)"
        for a in node.args:
            if not isinstance(a, (Ref, Copy, Lit)):
                return f"an argument of {node.fn} that is not a member read"
            p = fold_problem(a)
            if p:
                return p
        return None
    if isinstance(node, Opaque):
        return f"not resolved ({node.why})"
    return f"an operator expression ({node.render()})"


def realize_root(key):
    if key in EPOCH_BY_KEY:
        return EPOCH_BY_KEY[key].path
    if key in RUN_BY_KEY:
        return RUN_BY_KEY[key].path
    return None


def _field_moved(path):
    for old, new in EPOCH_FIELD_MOVES:
        if path == old or path.startswith(old + ".") or path.startswith(old + "->") \
                or path.startswith(old + "["):
            return new + path[len(old):], (old, new)
    return path, None


def render(node, used=None):
    """(text, problems) of @p node realized through the tables.  @p used, a set, collects the
    table entries used: root keys, ('FIELD_MOVE', old), ('COPY_SOURCE', path)."""
    problems = []
    used = used if used is not None else set()

    def r(n):
        if isinstance(n, Ref):
            base = realize_root(n.root)
            if base is None:
                problems.append(f"no realization for {n.root}")
                base = "<" + n.root + ">"
            else:
                used.add(n.root)
            text, moved = _field_moved(base + "".join(n.chain))
            if moved:
                used.add(("FIELD_MOVE", moved[0]))
            return n.prefix + text
        if isinstance(n, Copy):
            inner = r(n.inner)
            if inner.startswith(CONST_RUN_PREFIXES):
                return inner
            if inner in copy_source_paths():
                used.add(("COPY_SOURCE", inner))
                return inner
            problems.append(f"a by-value copy of {inner}, which is neither const nor a declared copy source")
            return "copy(" + inner + ")"
        if isinstance(n, Cond):
            return f"{r(n.c)}?{r(n.a)}:{r(n.b)}"
        if isinstance(n, CallNode):
            return f"{n.fn}(" + ",".join(r(a) for a in n.args) + ")"
        if isinstance(n, Lit):
            return n.text
        if isinstance(n, Expr):
            if len(n.args) == 1:
                return n.op + r(n.args[0])
            return r(n.args[0]) + n.op + r(n.args[1])
        if isinstance(n, Opaque):
            problems.append(f"unresolved: {n.text} ({n.why})")
            return "<" + n.text + ">"
        problems.append(f"unknown node {n!r}")
        return "?"

    return r(node), problems


def is_epoch_path(text):
    t = text.lstrip("&*")
    return t == "ep" or t.startswith("ep.")


def stop_keys():
    """The root keys at which the resolver ends its walk: every realized name."""
    return set(EPOCH_BY_KEY) | set(RUN_BY_KEY)
