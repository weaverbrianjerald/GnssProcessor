#!/usr/bin/env python3
"""Relocation verifier of card NOM3 step EP1 (gate G-R): checks V1 to V13 of the EP1 specification.

    python scripts/nom_ep1_verify.py --tree DIR [--base REV] [--rows NAME ...] [--seams A,B,...]
        [--presolve-writer {outputs,epoch}] [--manifest PATH] [--controls] [--only V1a,...]
        [--verbose]

Run it with PYTHONIOENCODING=utf-8 on a cp1252 console.  The frozen source <B> is read from
git at --base (default nom_ep1.DEFAULT_BASE), never from a working tree; the candidate is the
source tree at DIR (src/, include/, tests/, CMakeLists.txt), read from disk with CRLF folded
to LF.  Python 3 standard library only, with scripts/nom_cpp.py (lexer, statements,
declarations, scopes, resolver) and scripts/nom_epoch_lint.py (tree reader, comment-free
rendering, checks V4, V10, V11).  Exit 0 only when every check that applies passes (and,
with --controls, every control that can run fires); "n/a" is not a failure.

Independence.  The verifier reads the canonical table (scripts/nom_ep1/rows_*.py: which
seams, their mechanism, outputs, kept parameters) and the closed list of declared
substitutions (scripts/nom_ep1/declared.py: each entry's class, its realization path at the
head of its reason text, its frozen locators), and nothing the generator computes: it imports
neither scripts/nom_ep1_cut.py nor the realization tables of scripts/nom_ep1/model.py.  Every
binding is derived twice, from the frozen source (the frozen call site's argument, resolved
by the scope walk to its storage root) and from the candidate (the new call site, then the
seam's generated binding line or an adapter's forwarding argument, resolved with the seam's
parameters bound to that call site's arguments), and the two must agree in the frozen
vocabulary; a candidate path enters that vocabulary only through a declared substitution
(run.<member> through the NetworkRun construction or an OWNER_MOVE / FIELD_MOVE path;
ep.<path> through an EPOCH_MEMBER / FIELD_MOVE / ACCESSOR path or the epoch's construction
on `se`).  A copy of a const object (a const local, the knobs, a declared COPY_SOURCE field)
counts as the object.  The generator's manifest (--manifest, or each
scripts/nom_ep1/manifest_*.json of the tree whose converted seams and pre-solve form are this
run's; the manifests of other stages are listed in V8 and not paired) is read for pairing
only: its base, its converted seams and the epoch function's lines must be what the verifier
found, and each entry {"seam", "kind", "file", "frozen_line", "new_line"} under "seams" must
name the declaration or definition the verifier found itself, else V8 fails; no binding is
read from it.  The rows and the declared entries are those that apply at --base
(nom_ep1.bind_base, decided by the rows' and the entries' own locators): an entry marked
retired_by the REMOVE-key deletion applies only where the base still has its object.

The candidate is one of two shapes, found from the tree itself:
  empty   Application::runNetwork still holds the lambda processSyncedEpoch and no
          networkProcessEpoch_ exists: the frozen tree, or any tree before step EP1a-1.  It
          is verified as an empty conversion: the lambda is the epoch body (no column
          shift), the epoch locals named by declared.py's EPOCH_MEMBER entries stand for the
          epoch (V4), the spec-cut statements are still in runNetwork (V12); the checks that
          need the new types report n/a.
  epoch   Application::networkProcessEpoch_ exists (step EP1a-1 and later); its body is
          the lambda's moved four columns left, after a preamble that ends at the comment
          `// ---- End of the run's names ----`.
--rows names the row files whose seams the tree must have converted (app, solve, emit,
diag, rows_<name>, a path, or all), --seams single seams beyond them; without either no
seam is expected converted (step EP1a-1 and the empty conversion).  Every other row must
keep its frozen declaration and definition.  A converted seam is found by its definition's
first two parameters (NetworkRun&, const NetworkEpoch&); its bindings are the statements
between `// ---- From the run and the epoch` and `// ---- End of the bindings ----`.

The checks (comments and blank lines are ignored throughout, as in scripts/rt_seam_verify.py;
"character-exact" means the code text and its indentation):
  V1  Bodies.  (a) The lambda's top-level statements, in order, against the epoch body's:
      each carried character-exact (shifted), or the old side of a declared transform of
      spec 5.3 checked by rule (the epoch's construction after the canonical-pair statement,
      the dropped workspace, an epoch local's alias / sized store / result store at its
      declaration, the clock store after the three clock locals, ep.emitted after the emit
      bracket, the bracket and converted-call rewrites, the two replay closures).  Each
      carried statement and each store's initializer is then paired with its frozen text
      token by token: an `ep.<path>` read as a local's name must pair with a frozen token
      that IS that lambda local (the scope walk, so the DD-carry predict's own block-local
      `dt` is never ep.clock.dt), ep.emitted with the frozen comparison, a bracket with the
      driver's closure.  The epoch function's definition has the frozen lambda's head:
      `NetworkRun& run`, then the lambda's parameter list, its trailing return type
      (Application::LoopSignal), and after the list the lambda's specifiers but `mutable`
      (none: no noexcept).  The VALUE_EQUAL proof runs here (below).  (b) Each converted body
      below its bindings equals the frozen body, except the solve's two replay lines and
      three epilogue lines; the bindings open the body (no code between the `{` and the
      opening marker) and hold one declaration per statement, each of a frozen parameter of
      the seam that V3a compares, once (not a kept parameter, an output or a removed
      CALLBACK_DIRECT; the CALLABLE predicate only where it is a parameter).  (c) Every
      unconverted row's declaration and definition, and every adapter's explicit form,
      equal frozen (the solve's three
      epilogue lines excepted from step EP1a-1 on).  (d) runNetwork equals frozen apart from
      the spec 4.1 edits and the UNREAD_ALIAS lines (each gone line one declarator, a
      reference or a const builtin copy of a member path, and no token left in runNetwork
      names it), every declaration left in its outermost scope is read, and what those
      edits took out is where the spec puts it: the
      epoch function's one declaration in Application, with the lambda's head as in (a);
      the predicate and the two brackets as functions with the closures' parameters, return
      type, specifiers after the list (no noexcept) and body, each declaration with its
      definition's head, under a preamble that binds exactly the closures' captures (a
      reference for a by-reference capture, `const auto` for a by-value one, each resolving
      to the captured object) and otherwise only references to frozen driver objects the
      body never names; the timer scratch in NetworkRunTiming (type and initializer); the
      hand-off fields in NetworkPostFitHandoff with their frozen declarations, in frozen
      order; NetworkRunEngines with the engines in the frozen construction order, of their
      frozen types, built by one constructor with an empty body and nothing after its
      parameter list; LoopSignal.  (e) Every
      changed line of the build's inputs (a line diff against <B> of every file the base
      has under src/, include/ and tests/, every C, C++ or CMake file the candidate has
      there whatever its extension, CMakeLists.txt and, when the tree carries them,
      third_party/ and the root CMake configuration files; a C++
      file read without its comments, any other file as written, so a `/*` in CMake hides
      nothing; a new file of the step diffed against nothing, any other new file failed)
      is an #include at file scope, a forward declaration at namespace scope of a class the
      project already names, or every token of it lies in a token range another check
      verified (so code appended on a verified line is not accounted with it), or it is a
      line a check compared whole; a lone brace line only beside a line a check verified.
      No changed file holds what the reading layer reads otherwise than the compiler (a
      line splice that changes the tokens, a carriage return that ends no line, a control
      character, bytes that are not UTF-8: nom_cpp.reading_problems) beyond its frozen
      text.  No changed line is a preprocessor directive but #include, a new header's
      #pragma once, or a conditional group of #include lines, and no such group that
      enters or changes holds a frozen #include; no frozen #include is removed.  An
      #include that enters the tree names its header literally and inside the include
      directories (no `..`), and reaches a header (.hpp or .h under include/ or third_party/) the frozen tree has or
      one of the step's two new headers, a C++ standard library header, or a library
      header the frozen tree includes by that spelling: never a source, a test or an
      unread file.  CMakeLists.txt changes by the step's build-list lines alone: each new
      source named once, as a bare line of its target's list (gnss_core, gnss_tests), and
      nowhere else, so no per-source option or definition, forced include or test source
      in gnss_core enters.  The step's new source files hold only what the specification
      puts there (NetworkRun.cpp the three definitions, ApplicationNetworkEpoch.cpp the one
      epoch function, the contract test its probe, static_asserts and one TEST case), the
      hand-written engine specification test excepted (the suite checks its text; this
      check its one listing in gnss_tests and that no file includes it).  Of a struct the
      base already has (NetworkRunTiming, NetworkSolveGlueResult) only the moved or new
      members are claimed, so any other edit there (a member's default, a stray field) is
      unaccounted.
  V2  Signatures: each converted seam has one epoch-form declaration and one definition,
      (NetworkRun&, const NetworkEpoch&, kept..., outs...) as its row says (the card's
      NetworkEpoch& under --presolve-writer epoch, for a writer before the solve); kept
      parameters keep their frozen declaration; after the solve only the hand-off and the
      prepared solution are outputs.  The head around the list is the frozen form's (an
      adapter's, the explicit form's): the return type and every specifier before the name
      (so no reference return of a local or of a temporary), every specifier after the list
      (so no noexcept), and no default argument but a kept parameter's frozen one.
  V3  Bindings.  (a) At every frozen call site of a converted seam paired with its new site
      (callee and occurrence), and at the solve's two replay calls, every frozen parameter's
      binding agrees; the binding line is copy-initialized (`<declarator> = <expression>;`,
      one name), keeps the frozen declarator and carries `// written` exactly when it
      writes the epoch; a kept argument keeps its text; a removed std::function parameter
      is a declared CALLBACK_DIRECT, or the CALLABLE predicate's one declared forwarder
      (below); adapters' other callers keep their text; every binding line of a definition is
      one a call site compares.  (b) Every read of a captured driver name in the epoch body
      resolves to the frozen object, over every token V1a paired; each preamble line
      declares a captured name with the frozen declarator (a run reference member under its
      spec type, an owned engine or a moved timer vector as a
      reference of the frozen type, anything else with the frozen type text and operators,
      `const` alone addable on a reference) and binds the frozen object.  The third-band
      forwarder, in the epoch preamble and in the DD build's bindings, is exactly
      `[&X](<the closure's parameters>) -> <its return> { return
      networkIsUncarriedThirdBand(X.<path>, <the parameters>); }` with X.<path> the knobs.
      The COPY_SOURCE proof runs here (below).  (c) Each epoch member is realized at its
      local's frozen position, with the local's type and initializer (NetworkEpoch.hpp), and
      bound nowhere before it; EpochClock's members are exactly the clock store's, in its
      order, with the frozen locals' types; NetworkEpoch holds `se`, `clock`, the declared
      groups and `emitted{false}` and nothing else, in the declared layout
      (declared.EPOCH_LAYOUT), each group exactly its declared locals in their declared
      order; NetworkEpoch, EpochClock and every group are plain structs (no base clause, no
      friend, nested type, alias or static member, no member function in a group); its one
      constructor binds `se`, has an empty body and nothing after its parameter list;
      x_emit() has exactly `const` after its list, returns the frozen local's declarator,
      and its one return resolves, members read as ep.<member>, to the frozen x_emit
      initializer (ACCESSOR).  (d) The NetworkRun construction passes, for
      each reference member, a name of runNetwork's outermost scope that is the frozen
      object; NetworkRun is a plain struct whose members are the references, `engines` and
      the OWNER_MOVE groups in that declared order; each reference member is declared with
      its spec type and initialized from a constructor parameter of that same type; the
      owned members have the frozen locals' types and no default member initializer; the
      constructor builds `engines(<spec>)` and only the owned members the frozen code
      constructed with arguments, its body is empty and nothing follows its parameter list.
      (e) No body reads a free run, ep,
      nrun or nep (the solve's replay calls excepted).  (f) Folded arguments are member
      reads with an optional & or *, a final
      .get(), ep.x_emit(), literals, conditionals over those, or
      TimeUtils::continuousGpsSeconds of a member read; every binding's initializer is
      folded but the CALLABLE forwarder's (an immediately invoked lambda is a call).
  V4  Driver writes into the epoch after the solve (scripts/nom_epoch_lint.py).
  V5  Deep const: no member of NetworkEpoch, or of a project type held by value inside it,
      is a non-const reference, a pointer to non-const, a smart pointer, a std::function or
      a std::reference_wrapper; `se` is the one reference.
  V6  Order: the execution-order events (network* and call-table calls, log calls with
      level and format literal, brackets with their SB_*, returns of LoopSignal, the three
      inserted stores) of the epoch body equal the frozen lambda's, a closure's events
      counted where it is invoked; runNetwork's equal frozen runNetwork's without the lambda
      (networkEngineSpec's call declared).
  V7  Documentation: each converted declaration has a Doxygen block that carries the frozen
      @param texts and names every epoch member its bindings read.
  V8  Completeness: every call of the frozen call table is covered once, by a row or a
      declared non-conversion; every row's call count matches; the contract test lists the
      converted seams, each cast to one alias of the file whose type is the seam's epoch
      form (the frozen return type, the row's parameters), and every alias is one seam's;
      the manifest's pairing agrees.  Declared entries whose frozen object is missing at
      this base are listed.
  V9  No new overload: for every translation unit whose include closure (every tree
      file it reaches, any file type) differs from its frozen counterpart's (its own frozen
      file; ApplicationNetwork.cpp for the two units cut from runNetwork), over the names
      its code calls, every free-function declaration newly visible is a converted seam's
      epoch form (the parameter types V2 checks) or one of the step's new functions, and
      none visible at the base is gone but a converted seam's explicit form; no newly
      reached file #defines an identifier of the unit, brings in a called name by a
      using-declaration or holds a using-directive; the two cut units reach no project
      header the frozen driver did not but the two new headers, and include no library
      header it did not read.  The two headers hold only their plain structs (no base
      clause), forward declarations, static_asserts, the declarations of networkEngineSpec
      and the two brackets (each its definition's signature: return type, parameter types,
      default arguments, specifiers after the list) and the observer alias, and
      their structs define no member function but the run's and the epoch's constructors
      and x_emit() (the rest are deleted).
  V10, V11  Stage order and writer banners (scripts/nom_epoch_lint.py; banner writers not
      yet converted are skipped while the conversion is partial).  V10 includes the
      converted seams a seam calls with its own run and epoch (the solve's replay builders),
      which may read only groups written before the caller's spine line.
  V12 Spec cut: the setup statements that built the engines' constructor arguments are the
      frozen ones, character-exact and in order (in runNetwork for an empty conversion, in
      networkEngineSpec after it, a built local's declaration becoming a reference to its
      own spec field, `spec.<its name>`, or that field's initializer); they read only
      config_, nc, the knobs and their own locals, call only the allowlist, and no other
      statement entered their range; networkEngineSpec has the head of spec 2.5
      (ENGINE_SPEC_HEAD: the spec by value, nothing after the list), opens with
      `NetworkEngineSpec spec;` and ends with `return spec;`, assigns each field whole at
      most once and never a field
      a reference binding fills, and an argument field only with its frozen argument; each
      engine constructor argument of NetworkRunEngines and NetworkRun is the frozen
      argument or the spec field that holds it; each NetworkEngineSpec field has the type of
      what it holds (the replaced local's type without const, or the argument expression's:
      a static_cast's target, a frozen local's or a knob's declared type), so no argument
      is narrowed on its way to its engine.
  V13 Names: a converted seam whose frozen body declares or reads run, ep or an output's
      name takes that parameter's fallback name (nrun, nep, <output>_out); a seam that
      needs none takes run and ep; no other definition of a row takes those names.

The proofs of spec 5.6 (declared.Writers), on the frozen base and on the candidate:
COPY_SOURCE, no write to n_threads / seam_threads / carry_par / brdc_rts of the setup result
outside networkSetup_; VALUE_EQUAL, no writer of ctx.counters.n_emitted that the per-epoch
body reaches (directly or through other functions) but networkEmitStore, which the body uses
once.  Each write's object is resolved by a scope walk of its function (an alias is followed
to its initializer), so `snap.n_emitted` is another object.  A failed proof fails V3b
(COPY_SOURCE) or V1a (VALUE_EQUAL) and withdraws the substitution.

--controls builds each pre-registered negative control as a mutation of the candidate (an
overlay: one or two of its files replaced in memory, no disk copy) and runs every check on
it: the sixteen of spec 5.7 (N1 to N16), seventeen added at the review round (R1 to R17),
seventeen at the first hardening round (R18 to R34), eleven at the second (R35 to R45),
seven at the third (R46 to R52), twenty-six at the fourth (R53 to R78) and one at the EP1
review round (R79, the dead alias the review found), each R a mutation an earlier version
of this verifier accepted or (R40 to R45, R49, R50) the same mutation by another route.  A
control fires when
its named check fails (N8 V1a or V3c, N10 V1a or V3e, N16 V4 or V6, N13 V2 or V8; N4 only when
V3a and V10 both fail, R35 to R37 only when V2 and V8 both fail, R38 only when V1a and V1d
both fail, R46, R47 and R49 only when V1e and V9 both fail, R53 to R58 only when V1b and V3a
both fail).  N4's mutation keeps the trip's
adapter and gives the solve's internal call the epoch form, which binds ep.solve.plan at that
call exactly as the spec's direct conversion would (a declared deviation from the spec's
wording, not from its target).  N11 cuts the debug_r_scale warning where the base still has
it, else a synthetic warning, so it does not depend on the lane layer.  On an empty conversion the controls run in
their frozen-tree form (spec 7: "N5-N12 fire on synthetic mutations of <B>").  A control
whose target the stage cannot have yet (an unconverted seam, no epoch function) is pending;
one whose target the stage must have (its `due` rule) but the tree lacks is MISSING, which
fails the run, so a control never falls silent at a later base.  On an epoch tree the
standalone lint (scripts/nom_epoch_lint.py, with --partial while rows remain unconverted and
with this run's --presolve-writer) runs on the unmutated candidate, where it must pass, and on
N7, N16, R24 to R26, R46 to R50, R59 to R62, R64, R65, R75 and R77, which fire only when it
also fails there.  --rows takes
row names space- or comma-separated, or `all`.

Not covered here (spec 5.8): run-time behaviour (default construction then assignment,
construction and destruction order, code generation), which gate G-EP's arms cover; the
banner's prose "available from" column, which V10 replaces by each group's first write in
the epoch function.
"""
import argparse
import bisect
import collections
import difflib
import json
import pathlib
import re
import sys
import time
import traceback

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from nom_cpp import (APPLICATION_HPP, BUILTIN_TYPES, DRIVER_PATH, KEYWORDS, LAMBDA_NAME, SEAM_CALL,  # noqa: E402
                     CallNode, Cond, Copy, Driver, Expr, FileIndex, GitTree, Lit, Opaque, Ref, Resolver,
                     Scopes, Tokens, _lambda_at, calls_in, match_angle, parse_declaration,
                     parse_params, reading_problems, statements)
from nom_ep1 import DEFAULT_BASE, ROW_MODULES, bind_base, load_rows, declared  # noqa: E402
from nom_ep1.model import ADAPTER, MEMBER, RUN, Explicit  # noqa: E402  (the row type only)
import nom_epoch_lint as lint  # noqa: E402
from nom_epoch_lint import (DiskTree, Sub, cond_include_lines, pp_directive, render,  # noqa: E402
                            same_lines)

CHECKS = ("V1a", "V1b", "V1c", "V1d", "V1e", "V2", "V3a", "V3b", "V3c", "V3d", "V3e", "V3f",
          "V4", "V5", "V6", "V7", "V8", "V9", "V10", "V11", "V12", "V13")
SHIFT = 4                                   # the lambda body moves four columns left
RUN_NAMES_END = re.compile(r"//\s*-{2,}\s*End of the run's names")
NEW_FILES = ("include/app/NetworkRun.hpp", "src/core/app/NetworkRun.cpp",
             "include/app/NetworkEpoch.hpp", "src/core/app/ApplicationNetworkEpoch.cpp",
             "tests/test_network_epoch_contract.cpp", "tests/test_network_engine_spec.cpp")
CONTRACT_TEST = "tests/test_network_epoch_contract.cpp"
ENGINE_SPEC_TEST = "tests/test_network_engine_spec.cpp"   # hand-written (spec 5.9)
# The build's inputs V1e diffs against <B> (build_inputs), and those it diffs only when the
# candidate carries them (an export of src/, include/, tests/ and CMakeLists.txt does not).
BUILD_INPUTS = ("src", "include", "tests", "CMakeLists.txt")
BUILD_INPUTS_IF_PRESENT = ("third_party", "CMakeSettings.json", "CMakePresets.json", "CMakeUserPresets.json")
CPP_EXT = (".cpp", ".hpp", ".h")             # read as C++ (comments removed); any other file line by line
# A file of the candidate alone that a compiler or CMake could read (V1e fails it as new).
SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".c++", ".cppm", ".ixx", ".h", ".hh", ".hpp", ".hxx", ".h++",
              ".inl", ".ipp", ".tpp", ".tcc", ".inc", ".def", ".cmake")
# The step's build-list entries (spec 4.1): each new source, the command and target that list it.
CMAKE_ENTRIES = (("src/core/app/ApplicationNetworkEpoch.cpp", "add_library", "gnss_core"),
                 ("src/core/app/NetworkRun.cpp", "add_library", "gnss_core"),
                 (CONTRACT_TEST, "add_executable", "gnss_tests"),
                 (ENGINE_SPEC_TEST, "add_executable", "gnss_tests"))
RUN_HPP = "include/app/NetworkRun.hpp"
RUN_CPP = "src/core/app/NetworkRun.cpp"
SOLVE_CPP = "src/core/filter/NetworkSolveGlue.cpp"
SOLVE_HPP = "include/filter/NetworkSolveGlue.hpp"
GROUPS_HPP = "include/filter/NetworkRunGroups.hpp"
# The run's reference members and the frozen object each must bind (spec 2.1).
RUN_REFS = collections.OrderedDict([("ctx", "D:ctx"), ("config", "A:config_"), ("nc", "D:nc"),
                                    ("log", "D:log"), ("traces", "D:traces"), ("setup", "D:setup"),
                                    ("log_dir", "A:log_dir_")])
# Their declared types (spec 2.1): each a reference, so the run holds the frozen object itself.
RUN_REF_TYPES = {"ctx": "NetworkRunContext&", "config": "const GnssConfig&", "nc": "const NetworkConfig&",
                 "log": "const std::shared_ptr<spdlog::logger>&", "traces": "NetworkRunTraces&",
                 "setup": "NetworkSetupResult&", "log_dir": "const std::filesystem::path&"}
# The third-band forwarder's one form (CALLABLE): the predicate it calls.
PREDICATE = "networkIsUncarriedThirdBand"
# The epoch function's leading parameter (spec 4.1: networkProcessEpoch_(NetworkRun& run,
# <the lambda's parameters>)); the rest of its head is the frozen lambda's (epoch_head).
EPOCH_RUN_PARAM = "NetworkRun& run"
# networkEngineSpec's head (spec 2.5): its return type and parameter list, names included
# (the cut statements read config_, nc and ctx.knobs by these names).
ENGINE_SPEC_HEAD = ("NetworkEngineSpec",
                    "const GnssConfig& config_, const NetworkConfig& nc, const NetworkRunContext& ctx")
# Group outputs and their parameter types (spec 3.1); single-member outputs keep the frozen
# declarator.
GROUP_TYPES = {"pass_a": "EpochPassA", "funnel": "NetworkSlipFunnel", "columns": "EpochColumns",
               "assembly": "EpochAssembly", "dd_build": "EpochDdBuild",
               "handoff": "NetworkPostFitHandoff"}
# Calls the engine constructor arguments may make (V12): configuration getters, the network
# block, the VCE family parser, iterator reads of a configuration list.
SPEC_CALLS = re.compile(r"^(config_\.\w+|config_\.getNetworkConfig|NetworkCarryVce::parseComponents|"
                        r"[\w.]+\.(begin|end))$")


# ---------------------------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------------------------
class Result:
    def __init__(self):
        self.status = {}
        self.problems = collections.defaultdict(list)
        self.info = collections.defaultdict(list)

    def fail(self, chk, msg):
        self.problems[chk].append(msg)

    def note(self, chk, msg):
        self.info[chk].append(msg)

    def na(self, chk, why):
        self.status[chk] = "n/a"
        self.info[chk].append(why)

    def done(self, chk):
        if self.status.get(chk) != "n/a":
            self.status[chk] = "FAIL" if self.problems[chk] else "pass"

    def failed(self):
        return [c for c in CHECKS if self.status.get(c) == "FAIL"]


# ---------------------------------------------------------------------------------------------
# The declared substitutions, as the verifier reads them (scripts/nom_ep1/declared.py)
# ---------------------------------------------------------------------------------------------
TRANSFORMS = {"2": "WS", "3": "ALIAS", "4": "SIZED", "5": "SCALAR", "6": "CLOCK", "7": "RESULT"}


class Substitutions:
    """The closed list of spec 5.6, parsed from declared.py: each entry's realization path is
    the leading `ep.<path>:` / `run.<path>:` of its reason text."""

    def __init__(self):
        self.epoch = collections.OrderedDict()     # local -> (path, transform)
        for d in declared.declared("EPOCH_MEMBER"):
            m = re.match(r"^(ep\.[\w.]+):", d.reason)
            t = re.search(r"\(T-B(\d)\)", d.reason)
            if not m or not t:
                raise SystemExit(f"declared EPOCH_MEMBER {d.name}: no `ep.<path>:` / (T-Bn) in its reason")
            self.epoch[d.name] = (m.group(1), TRANSFORMS[t.group(1)])
        self.accessor = {}
        for d in declared.declared("ACCESSOR"):
            m = re.search(r"is (ep\.\w+\(\))", d.reason)
            self.accessor[d.name] = m.group(1) if m else None
        self.owner = collections.OrderedDict()
        for d in declared.declared("OWNER_MOVE"):
            m = re.match(r"^(run\.[\w.]+):", d.reason)
            if not m:
                raise SystemExit(f"declared OWNER_MOVE {d.name}: no `run.<path>:` in its reason")
            self.owner[d.name] = m.group(1)
        self.field_solve, self.field_timing = {}, {}
        for d in declared.declared("FIELD_MOVE"):
            fd = [w for w in d.where if type(w).__name__ == "FieldDecl"]
            if fd:
                m = re.search(r"-> \w+::([\w.]+)", d.reason)
                self.field_solve[d.name] = (fd[0].struct, m.group(1) if m else d.name)
            else:
                m = re.search(r"NetworkRunTiming::(\w+)", d.reason)
                self.field_timing[d.name] = m.group(1) if m else d.name
        self.copy_sources = [d.name for d in declared.declared("COPY_SOURCE")]
        ve = declared.declared("VALUE_EQUAL")
        self.value_equal = next((w.expr for d in ve for w in d.where if hasattr(w, "expr")), None)
        self.callable = [d.name for d in declared.declared("CALLABLE")]
        self.callbacks = [d.name for d in declared.declared("CALLBACK_DIRECT")]
        self.bracket = [d.name for d in declared.declared("BRACKET")]
        self.type_move = [d.name for d in declared.declared("TYPE_MOVE")]
        self.unread_alias = [d.name for d in declared.declared("UNREAD_ALIAS")]
        self.spec_cut = declared.declared("SPEC_CUT")
        self.paths = {v[0]: k for k, v in self.epoch.items()}
        timing_owner = next((n for n, p in self.owner.items() if p == "run.timing"), None)
        self.run_paths = {p: "D:" + n for n, p in self.owner.items()}
        for n, f in self.field_timing.items():
            if timing_owner:
                self.run_paths[f"run.timing.{f}"] = "D:" + n

    def epoch_local_of(self, path):
        """(local, rest) of the longest declared epoch path that prefixes @p path."""
        best = None
        for p, local in self.paths.items():
            if path == p or path.startswith(p + ".") or path.startswith(p + "["):
                if best is None or len(p) > len(best[0]):
                    best = (p, local)
        if best is None:
            return None, None
        return best[1], path[len(best[0]):]


SUBST = Substitutions()


# ---------------------------------------------------------------------------------------------
# Small helpers over tokens
# ---------------------------------------------------------------------------------------------
class EpSub(Sub):
    """A rendering substitution of a candidate statement that stands for frozen text (V1a):
    kind `epath` (an `ep.<path>` run rendered as the local it realizes, @p local), `value`
    (ep.emitted rendered as the VALUE_EQUAL expression), `handoff` (a result alias's
    `.handoff.` path rendered as the frozen field), `bracket` (networkSubBegin(run, ...)
    rendered as subBegin(...)).  The frozen token paired with each is checked to denote what
    the substitution claims (check_alignment)."""
    __slots__ = ("kind", "local")

    def __init__(self, a, b, text, kind, local=None, eat_before=False, eat_after=False):
        super().__init__(a, b, text, eat_before, eat_after)
        self.kind, self.local = kind, local


def _units(T, s, e, subs):
    """The tokens of [s, e) with each substitution's range replaced by the tokens of its text:
    [(text, token index or None, the substitution or None, position inside its text)]."""
    by = {x.a: x for x in subs}
    out, i = [], s
    while i < e:
        x = by.get(i)
        if x is not None:
            for k, t in enumerate(Tokens(x.text).toks):
                out.append((t.text, None, x, k))
            i = x.b
            continue
        out.append((T.t(i), i, None, 0))
        i += 1
    return out


def join_text(text):
    T = Tokens(text)
    return T.join(0, len(T))


def norm_subs(T, s, e, subs):
    """Tokens [s, e) joined with minimal spacing, each Sub's range replaced by its text."""
    by = {x.a: x for x in subs}
    out, i, prev_word = [], s, False
    while i < e:
        x = by.get(i)
        if x:
            out.append(x.text)
            i, prev_word = x.b, False
            continue
        t = T[i]
        word = t.kind in ("id", "num")
        if word and prev_word:
            out.append(" ")
        out.append(t.text)
        prev_word = word
        i += 1
    return "".join(out)


def decl_start(T, f):
    """First token of the declaration or definition @p f (its first specifier)."""
    st = getattr(f, "start", None)
    if st is not None:
        return st
    k = f.name_tok - 1
    while k >= 0:
        t = T.t(k)
        if T.k(k) == "pp" or t in (";", "{", "}") or (t == ":" and T.t(k - 1) in ("public", "private", "protected")):
            break
        k -= 1
    return k + 1


def decl_end(T, f):
    """One past the last token of @p f: its body's closing brace, or the `;` of a declaration."""
    if f.is_def:
        return f.body[1] + 1
    k = f.paren[1] + 1
    while k < T.n and T.t(k) != ";":
        k += 1
    return k + 1


# ---------------------------------------------------------------------------------------------
# Function heads (hardening round 2): what surrounds a parameter list
# ---------------------------------------------------------------------------------------------
# A head the step writes is compared whole, not only its parameter types: the return type
# and every specifier before the name (a reference return of a local, `static`, an
# attribute), each parameter's default argument, and every specifier after the parameter
# list (an exception specification, a cv or ref qualifier, a trailing return type, `=
# delete`).  A changed exception specification turns an exception the run's handler logs
# into std::terminate; a reference return of a local makes the caller copy a destroyed
# object; neither changes a parameter type or a token of a body.
def head_trail(T, f):
    """The specifiers after the parameter list of @p f (a nom_cpp Func), joined with minimal
    spacing: everything between its `)` and its body's `{` or its declaration's `;`; "" when
    there is none."""
    end = f.body[0] if f.is_def else decl_end(T, f) - 1
    return T.join(f.paren[1] + 1, end)


def head_defaults(f):
    """Each parameter's default argument of @p f (minimal spacing), None where it has none."""
    return [join_text(p.default) if p.has_default else None for p in (f.params or [])]


HEAD_PARTS = (("ret", "returns"), ("types", "takes"), ("text", "has the parameter list"),
              ("defaults", "has the default arguments"),
              ("trail", "has after its parameter list"))


def head_diff(T, f, want):
    """How the head of @p f differs from @p want, a dict of any of: ret (f.ret, every token
    before the name, the declarator's qualification excluded), types (the parameter types),
    text (the parameter list whole, names and defaults included), defaults (head_defaults)
    and trail (head_trail).  Returns one phrase per differing part."""
    got = {"ret": f.ret, "types": [lint.ptype(p) for p in (f.params or [])],
           "text": T.join(f.paren[0] + 1, f.paren[1]), "defaults": head_defaults(f),
           "trail": head_trail(T, f)}
    def shown(x):
        return "nothing" if x == "" else f"`{x}`"
    out = []
    for k, verb in HEAD_PARTS:
        if k in want and got[k] != want[k]:
            out.append(f"{verb} {shown(got[k])}, not {shown(want[k])}")
    return out


def member_trail(T, f):
    """The specifiers after the parameter list of a member function parsed by
    _member_function, up to its member initializers, its body, its `;` or its `= default` /
    `= delete` (an exception specification, `const`, a ref qualifier, an attribute)."""
    j = f["pc"] + 1
    while j < f["end"] and T.t(j) not in (":", "{", ";", "="):
        j = T.close(j) + 1 if T.t(j) in ("(", "[") else j + 1
    return T.join(f["pc"] + 1, j)


def lambda_head(T, d):
    """(parameters, specifiers, return type) of the closure declared by @p d, each joined with
    minimal spacing: the text inside its parameter list, its specifiers after it (`mutable`
    dropped: a function has no such specifier, and a by-value capture's binding carries the
    constness instead), and its trailing return type (None when it has none); or None when
    @p d does not hold a lambda with a parameter list."""
    lam = _lambda_at(T, d.init[0], d.init[1]) if d is not None and d.init else None
    if lam is None or lam[1] is None:
        return None
    _, po, bo = lam
    pc = T.close(po)
    arrow = next((k for k in range(pc + 1, bo) if T.t(k) == "->"), None)
    spec = join_text(" ".join(T.t(k) for k in range(pc + 1, arrow if arrow is not None else bo)
                              if T.t(k) != "mutable"))
    return T.join(po + 1, pc), spec, (T.join(arrow + 1, bo) if arrow is not None else None)


def alias_ret(ret):
    """A frozen declaration's return text as a function type names it: its storage and
    function specifiers (static, inline, extern, virtual, constexpr) dropped."""
    words = ret.split(" ")
    while words and words[0] in ("static", "inline", "extern", "virtual", "constexpr"):
        words = words[1:]
    return " ".join(words)


def stmt_of(stmts, tok):
    for k, (s, e) in enumerate(stmts):
        if s <= tok < e:
            return k
    return None


def leaf(name):
    return re.split(r"\.|->|::", name)[-1]


def ids_in(T, a, b):
    """Identifier tokens of [a, b) that start a name (not after `.`, `->`, `::`; not a keyword)."""
    for j in range(a, b):
        if T.k(j) == "id" and T.t(j) not in KEYWORDS and T.t(j - 1) not in (".", "->", "::"):
            yield j


def ep_path_tokens(T, j, e):
    """End token of `ep(.id)*` starting at @p j (a final `.x_emit()` included)."""
    k = j + 1
    while k + 1 < e and T.t(k) == "." and T.k(k + 1) == "id":
        if T.t(k + 2) == "(" and T.t(k + 1) == "x_emit" and T.t(k + 3) == ")":
            return k + 4
        if T.t(k + 2) == "(":
            break
        k += 2
    return k


# ---------------------------------------------------------------------------------------------
# The frozen base and the candidate
# ---------------------------------------------------------------------------------------------
class RunNetwork:
    """Application::runNetwork of a tree (any shape): its scope tree (outermost frame D) and,
    when present, the per-epoch lambda (frame L)."""

    def __init__(self, tree):
        self.tree = tree
        self.T = tree.tokens(DRIVER_PATH)
        idx = tree.index(DRIVER_PATH)
        rn = [f for f in idx.funcs if f.qual == "Application::runNetwork" and f.is_def] if idx else []
        if len(rn) != 1:
            raise SystemExit(f"nom_ep1_verify: Application::runNetwork not found in {tree.rev}")
        self.rn = rn[0]
        self.scopes = Scopes(self.T, self.rn.body[0], self.rn.body[1])
        lam = [d for d in self.scopes.root.decls if d.name == LAMBDA_NAME and d.lambda_scope]
        self.lam = lam[0] if lam else None
        self.frames = {id(self.scopes.root): "D"}
        if self.lam:
            self.lam_scope = self.lam.lambda_scope
            self.lam_close = self.lam_scope.end
            self.lam_open = self.T.match[self.lam_close]
            self.frames[id(self.lam_scope)] = "L"
        app = tree.index(APPLICATION_HPP)
        self.members = set(app.members.get("Application", ())) if app else set()

    def resolver(self, stop=()):
        return Resolver(self.scopes, self.frames, stop, self.members)

    def statements(self):
        return statements(self.T, self.rn.body[0] + 1, self.rn.body[1])

    def lambda_statements(self):
        return statements(self.T, self.lam_open + 1, self.lam_close)


class Frozen:
    """<B>: git tree, the driver (nom_cpp.Driver, the fz of declared.py's locators) and a
    resolver that follows every reference to its storage root."""

    def __init__(self, base):
        self.base = base
        self.tree = GitTree(base)
        self.drv = Driver(self.tree)
        self.fit = bind_base(self)      # the rows and declared entries that apply at this base
        self.rn = RunNetwork(self.tree)
        self.res = self.rn.resolver()
        self.lam_stmts = self.rn.lambda_statements()
        self.calls = self.drv.calls


class CandFz:
    """The candidate as declared.py's locators see it (empty conversion only)."""

    def __init__(self, tree):
        self.tree = tree
        self.drv = Driver(tree)


class Candidate:
    def __init__(self, tree):
        self.tree = tree
        self.rn = RunNetwork(tree)
        self.fn = lint.epoch_function(tree)
        if self.fn is not None:
            self.mode = "epoch"
        elif self.rn.lam is not None:
            self.mode = "empty"
        else:
            raise SystemExit("nom_ep1_verify: the candidate has neither networkProcessEpoch_ nor the "
                             "lambda processSyncedEpoch")
        if self.mode == "empty":
            self.T = self.rn.T
            self.open, self.close = self.rn.lam_open, self.rn.lam_close
            self.scopes, self.frames = self.rn.scopes, self.rn.frames
            self.members = self.rn.members
            self.shift = 0
            self.body_key = "L"
            self.preamble, self.stmts = [], self.rn.lambda_statements()
        else:
            fn = self.fn
            self.T, self.open, self.close = fn.T, fn.open, fn.close
            self.scopes, self.frames = fn.scopes, fn.frames
            self.members = self.rn.members
            self.shift = SHIFT
            self.body_key = "E"
            allst = fn.statements()
            mark = lint.raw_pos(self.T, RUN_NAMES_END, self.open, self.close)
            if mark is None:
                self.preamble, self.stmts = [], allst
                self.no_marker = True
            else:
                self.preamble = [x for x in allst if self.T[x[0]].pos < mark]
                self.stmts = [x for x in allst if self.T[x[0]].pos > mark]
                self.no_marker = False

    def resolver(self, stop=()):
        return Resolver(self.scopes, self.frames, stop, self.members)


# ---------------------------------------------------------------------------------------------
# Canonical form: a candidate binding in the frozen vocabulary (spec 5.4, 5.6)
# ---------------------------------------------------------------------------------------------
class Canon:
    """Maps a resolved candidate node to frozen roots: run.<member> through the NetworkRun
    construction (reference members) and the declared OWNER_MOVE / FIELD_MOVE paths;
    ep.<path> through the declared EPOCH_MEMBER / FIELD_MOVE / ACCESSOR paths and the CTOR
    (ep.se is the epoch function's se); E:<name> (the epoch function's top level) to L:<name>
    for a name the frozen lambda declares at its top level; B:<line>.x to B:x."""

    def __init__(self, run_refs, lam_names, resolve_key, ep_keys=("E:ep",), run_keys=("E:run",),
                 value_equal=None):
        self.run_refs = dict(run_refs)      # reference member -> its frozen object (a node)
        self.lam_names = set(lam_names)
        self.resolve_key = resolve_key      # a frozen key ('D:est') -> its storage node
        self.ep_keys, self.run_keys = set(ep_keys), set(run_keys)
        self.value_equal = value_equal      # the VALUE_EQUAL expression, None when its proof failed

    def run_path(self, chain):
        """run.<path> as its frozen object: a reference member through the NetworkRun
        construction (V3d), an owned member through its declared OWNER_MOVE / FIELD_MOVE
        path."""
        path = "run" + "".join(chain)
        best = None
        for p, root in list(SUBST.run_paths.items()) + [("run." + m, r) for m, r in self.run_refs.items()]:
            if path == p or path.startswith(p + ".") or path.startswith(p + "->") or path.startswith(p + "["):
                if best is None or len(p) > len(best[0]):
                    best = (p, root)
        if best is None:
            return Opaque(path, "no declared realization of this run path")
        root = best[1] if isinstance(best[1], (Ref, Copy)) else self.resolve_key(best[1])
        rest = tuple(_segs(path[len(best[0]):]))
        if isinstance(root, Copy):
            return Copy(root.inner.extend(rest)) if rest else root
        return root.extend(rest) if rest else root

    def ep_path(self, chain):
        path = "ep" + "".join(chain)
        for name, (struct, new) in SUBST.field_solve.items():
            p = "ep.solve." + new
            if path == p or path.startswith(p + ".") or path.startswith(p + "["):
                local = SUBST.paths.get("ep.solve")
                return Ref("L:" + (local or "sg_"), ("." + name,) + tuple(_segs(path[len(p):])))
        for local, p in SUBST.accessor.items():
            if p and path == p:
                return Ref("L:" + local)
        if path == "ep.emitted" and self.value_equal:
            return Ref("L:emitted")          # VALUE_EQUAL: the frozen comparison, stored once
        if path == "ep.se" or path.startswith("ep.se."):
            return Ref("L:se", tuple(_segs(path[len("ep.se"):])))
        local, rest = SUBST.epoch_local_of(path)
        if local is None:
            return Opaque(path, "no declared epoch member realizes this path")
        return Ref("L:" + local, tuple(_segs(rest)))

    def __call__(self, node):
        if isinstance(node, Ref):
            root = node.root
            if root in self.run_keys:
                r = self.run_path(node.chain)
                return Ref(r.root, r.chain, node.prefix) if isinstance(r, Ref) else r
            if root in self.ep_keys:
                r = self.ep_path(node.chain)
                return Ref(r.root, r.chain, node.prefix) if isinstance(r, Ref) else r
            kind, _, name = root.partition(":")
            if kind == "E":
                return Ref(("L:" if name in self.lam_names else "E:") + name, node.chain, node.prefix)
            if kind == "B":
                return Ref("B:" + name.split(".", 1)[-1], node.chain, node.prefix)
            return node
        if isinstance(node, Copy):
            return Copy(self(node.inner))
        if isinstance(node, Cond):
            return Cond(self(node.c), self(node.a), self(node.b))
        if isinstance(node, CallNode):
            return CallNode(node.fn, [self(a) for a in node.args])
        if isinstance(node, Expr):
            return Expr(node.op, [self(a) for a in node.args])
        return node


def _segs(rest):
    return re.findall(r"(?:\.|->)\w+(?:\([^)]*\))?|\[[^\]]*\]", rest)


def frozen_canon(node):
    """A frozen node with block-local line numbers dropped (B:<line>.x -> B:x)."""
    if isinstance(node, Ref):
        if node.root.startswith("B:"):
            return Ref("B:" + node.root[2:].split(".", 1)[-1], node.chain, node.prefix)
        return node
    if isinstance(node, Copy):
        return Copy(frozen_canon(node.inner))
    if isinstance(node, Cond):
        return Cond(frozen_canon(node.c), frozen_canon(node.a), frozen_canon(node.b))
    if isinstance(node, CallNode):
        return CallNode(node.fn, [frozen_canon(a) for a in node.args])
    if isinstance(node, Expr):
        return Expr(node.op, [frozen_canon(a) for a in node.args])
    return node


def is_const_root(node, const_roots):
    if not isinstance(node, Ref):
        return False
    r = node.render()
    return any(r == c or r.startswith(c + ".") for c in const_roots)


def normalize(node, const_roots):
    """copy(copy(X)) -> copy(X); copy(X) -> X for a const X (spec 5.4: a copy of a const root
    or of a declared COPY_SOURCE equals its source at every reader)."""
    if isinstance(node, Copy):
        inner = normalize(node.inner, const_roots)
        if isinstance(inner, Copy):
            inner = inner.inner
        if is_const_root(inner, const_roots):
            return inner
        return Copy(inner)
    if isinstance(node, Cond):
        return Cond(normalize(node.c, const_roots), normalize(node.a, const_roots),
                    normalize(node.b, const_roots))
    if isinstance(node, CallNode):
        return CallNode(node.fn, [normalize(a, const_roots) for a in node.args])
    if isinstance(node, Expr):
        return Expr(node.op, [normalize(a, const_roots) for a in node.args])
    return node


def show(node):
    try:
        return node.render()
    except Exception:  # noqa: BLE001
        return repr(node)


def apply_declarator(kind, node):
    """The object a parameter of frozen kind @p kind holds when bound to @p node."""
    if kind == "V":
        return Copy(node) if isinstance(node, (Ref, Copy)) else node
    return node


# ---------------------------------------------------------------------------------------------
# The verifier
# ---------------------------------------------------------------------------------------------
class Verifier:
    def __init__(self, fz, tree, rows, converted, presolve_writer="outputs", manifest=None,
                 verbose=False):
        self.fz = fz
        self.tree = tree
        self.rows = rows
        self.by_name = {r.name: r for r in rows}
        self.converted = set(converted)
        self.presolve_writer = presolve_writer
        self.manifest = manifest
        self.verbose = verbose
        self.R = Result()
        self.cand = Candidate(tree)
        self.claims = collections.defaultdict(list)     # path -> [(first line, last line)]
        self.tclaims = collections.defaultdict(list)    # path -> [(first char, end char)] of claimed tokens
        self._merged = {}
        self._line_index = {}
        self.frozen_claims = collections.defaultdict(list)
        self.realized = {}        # epoch local -> dict(transform, stmt, alias)
        self.pairs = []           # (frozen stmt, new stmt) carried by plain comparison
        self.decls = lint.DeclIndex(tree)
        self.seamdefs = lint.SeamDefs(tree)
        self.fdecls = {}          # row name -> (decl Func, defn Func) at the base
        self.run_refs = {}
        lam_names = {d.name for d in fz.rn.lam_scope.decls}
        # The two substitutions whose proofs read the whole tree (spec 5.6) hold until proofs()
        # finds otherwise: then ep.emitted no longer stands for the frozen comparison and a copy
        # of the setup field no longer equals the driver's copy of it.
        self.value_equal = SUBST.value_equal
        self.copy_ok = set(SUBST.copy_sources)
        self.canon = Canon({}, lam_names, self.frozen_root, value_equal=self.value_equal)
        self.lam_names = lam_names
        self.const_roots = self._const_roots()
        self.aligned = []         # (frozen [a, b), new [c, d), unit pairs) compared by V3b

    # -- shared facts --------------------------------------------------------------------
    def frozen_root(self, key):
        """The frozen storage object a driver name denotes (references followed): `D:nc` is
        `A:config_.getNetworkConfig()`; an Application member or an unknown key is itself."""
        kind, _, name = key.partition(":")
        if kind != "D":
            return Ref(key)
        node = self.fz.res.name_ref(name, self.fz.rn.lam_open, ())
        return node if isinstance(node, (Ref, Copy)) else Ref(key)

    def _const_roots(self):
        """Frozen roots that are const objects: the const lambda and driver locals, the knobs,
        and the declared COPY_SOURCE setup fields (proved by their write scans)."""
        out = {"D:ctx.knobs"}
        for d in self.fz.rn.lam_scope.decls:
            if d.is_const and not d.is_ptr:
                out.add("L:" + d.name)
        for d in self.fz.rn.scopes.root.decls:
            if d.is_const and not d.is_ptr and not d.is_ref:
                out.add("D:" + d.name)
        for n in SUBST.copy_sources:
            if n in self.copy_ok:
                out.add("D:setup." + n)
        return out

    def frozen_rows(self):
        T = self.fz.tree
        for r in self.rows:
            if isinstance(r.mech, Explicit):
                continue
            cls = "Application" if r.mech == MEMBER else ""
            hi, ci = T.index(r.hpp), T.index(r.cpp)
            decls = [f for f in (hi.funcs if hi else []) if f.name == r.name and (f.cls or "") == cls
                     and f.params is not None and not f.is_def]
            defs = [f for f in (ci.funcs if ci else []) if f.name == r.name and f.is_def
                    and (f.cls or "") == cls and f.params is not None]
            sites = [c for c in self.fz.calls if c.name == r.name]
            n = sites[0].nargs if sites else None
            decl = next((f for f in decls if len(f.params) == n), decls[0] if decls else None)
            defn = next((f for f in defs if decl and len(f.params) == len(decl.params)),
                        defs[0] if defs else None)
            self.fdecls[r.name] = (decl, defn)

    # -- run ---------------------------------------------------------------------------------
    def run(self, only=None):
        self.frozen_rows()
        steps = [("V1a", self.proofs), ("V8", self.v8), ("V3d", self.v3d), ("V1a", self.v1a),
                 ("V3b", self.v3b),
                 ("V3e", self.v3e), ("V4", self.v4), ("V6", self.v6), ("V1c", self.v1c),
                 ("V1d", self.v1d), ("V12", self.v12), ("V13", self.v13), ("V2", self.v2),
                 ("V1b", self.v1b), ("V3a", self.v3a), ("V3f", self.v3f), ("V3c", self.v3c),
                 ("V5", self.v5), ("V7", self.v7), ("V9", self.v9), ("V10", self.v10_11),
                 ("V1e", self.v1e)]
        for chk, fn in steps:
            try:
                fn()
            except Exception as ex:  # noqa: BLE001  (a crash is a failed check, not a pass)
                self.R.fail(chk, f"the check raised {type(ex).__name__}: {ex}")
                if self.verbose:
                    traceback.print_exc()
        for c in CHECKS:
            if only and c not in only:
                continue
            if c not in self.R.status and not self.R.problems[c] and not self.R.info[c]:
                self.R.na(c, "not run")
            self.R.done(c)
        return self.R

    @property
    def epoch(self):
        return self.cand.mode == "epoch"

    def cand_loop(self):
        """The candidate's per-epoch body for the write proofs: networkProcessEpoch_, or the
        lambda (and the runNetwork closures it calls) of an empty conversion."""
        c = self.cand
        if self.epoch:
            return declared.LoopBody(c.fn.path, c.fn.open, c.fn.close)
        T = c.rn.T
        called = {x.name for x in calls_in(T, c.rn.lam_open + 1, c.rn.lam_close)}
        spans = tuple((T.match[d.lambda_scope.end], d.lambda_scope.end) for d in c.rn.scopes.root.decls
                      if d.kind == "closure" and d.lambda_scope is not None and d.name in called)
        return declared.LoopBody(DRIVER_PATH, c.rn.lam_open, c.rn.lam_close, spans)

    def proofs(self):
        """The proof obligations of COPY_SOURCE and VALUE_EQUAL (spec 5.6; declared.Writers),
        on the frozen base and on the candidate: no write to a COPY_SOURCE setup field outside
        networkSetup_, and no writer of ctx.counters.n_emitted the per-epoch body reaches but
        the emitter, used once.  A failed proof is reported (VALUE_EQUAL under V1a, whose
        ep.emitted it licenses; COPY_SOURCE under V3b, whose preamble copies it licenses) and
        withdraws the substitution, so every binding that relied on it fails too."""
        R = self.R
        floop = declared.driver_loop(self.fz.drv)
        nloop = self.cand_loop()
        cache = self.fz.__dict__.setdefault("_proofs", {})
        for d in declared.declared("COPY_SOURCE") + declared.declared("VALUE_EQUAL"):
            chk = "V1a" if d.cls == "VALUE_EQUAL" else "V3b"
            bad = False
            for w in d.where:
                if not isinstance(w, declared.Writers):
                    continue
                if w not in cache:
                    cache[w] = w.prove(self.fz.tree, floop)
                for label, (probs, notes) in (("the base", cache[w]),
                                              ("the candidate", w.prove(self.tree, nloop))):
                    for x in probs:
                        R.fail(chk, f"{d.cls} {d.name} ({label}): {x}")
                        bad = True
            if bad and d.cls == "COPY_SOURCE":
                self.copy_ok.discard(d.name)
            if bad and d.cls == "VALUE_EQUAL":
                self.value_equal = None
        self.const_roots = self._const_roots()
        R.note("V1a", "the VALUE_EQUAL proof " + ("holds" if self.value_equal else "FAILS")
               + f" on the base and the candidate; COPY_SOURCE holds for {sorted(self.copy_ok)}")

    def claim(self, path, a, b):
        """Claim source lines [a, b] of @p path for V1e (CMakeLists.txt, and a converted body's
        edited lines, which V1b compares whole)."""
        self.claims[path].append((a, b))

    def tclaim(self, T, a, b):
        """Claim tokens [a, b) of T for V1e: a check verified exactly these tokens, so a token
        appended on one of their lines (after a definition's `}`, before a statement) is
        not claimed with them."""
        if T is not None and a < b:
            self.tclaims[T.path].append((T[a].pos, T[b - 1].end))
            self._merged.pop(T.path, None)

    def fclaim(self, path, a, b):
        self.frozen_claims[path].append((a, b))

    # ==== V8 ===========================================================================
    def v8(self):
        R, fz = self.R, self.fz
        blocks = declared.ep2_blocks(fz)
        spans = [sp for sps in blocks.values() for sp in sps]
        by_name = collections.Counter(r.name for r in self.rows)
        n_rows = n_nc = 0
        for n, c in enumerate(fz.calls, 1):
            who = []
            if any(s <= c.tok < e for s, e in spans):
                who.append("EP2 block")
            else:
                who += ["row"] * by_name.get(c.name, 0)
                who += [nc.label for nc in declared.NON_CONVERSIONS
                        if nc.kind != "EP2_BLOCK" and nc.covers(c, ())]
            if len(who) != 1:
                R.fail("V8", f"call #{n} {c.name} ({DRIVER_PATH}:{c.line}) is covered "
                             f"{len(who)} times ({', '.join(who) or 'by nothing'})")
            elif who[0] == "row":
                n_rows += 1
            else:
                n_nc += 1
        for lab, sps in blocks.items():
            if not sps:
                R.fail("V8", f"EP2 block {lab}: its anchor call is not in the frozen lambda")
        for r in self.rows:
            k = sum(1 for c in fz.calls if c.name == r.name)
            if k != r.sites:
                R.fail("V8", f"row {r.name}: the frozen lambda calls it {k} times, the row says {r.sites}")
        # Each entry spelled as the base spells its names (RENAMED), as bind_base binds it, so
        # a re-spelled entry whose object the base has is not listed as gone.
        gone = [f"{d.cls} {d.name}" for d in declared.DECLARED
                if any(not hits for _, hits in declared._respell(d, fz.fit.respelled).locate(fz))]
        R.note("V8", f"{len(fz.calls)} frozen calls: {n_rows} by rows, {n_nc} by declared "
                     "non-conversions" + (f"; declared entries with no object at this base: "
                                          f"{', '.join(gone)}" if gone else ""))
        text = self.tree.read(CONTRACT_TEST)
        if self.epoch and text is not None:
            listed = set(re.findall(r"&\s*(?:Application::)?(network\w+)\b", text))
            listed |= set(re.findall(r"NetworkEpochContractProbe::(network\w+)", text))
            want = {r for r in self.converted}
            if listed != want:
                R.fail("V8", f"{CONTRACT_TEST} lists {sorted(listed - want)} not converted, misses "
                             f"{sorted(want - listed)}")
            else:
                R.note("V8", f"the contract test lists the {len(want)} converted seams")
            self.contract_aliases()
        elif self.epoch and self.converted:
            R.fail("V8", f"{CONTRACT_TEST} is missing")

    def contract_aliases(self):
        """Each converted seam's pointer alias in the contract test is its epoch form's type:
        the frozen return type (alias_ret) and the row's parameter types (epoch_signature),
        `R (*)(...)` for a free seam and `R (Application::*)(...)` for a member seam; each
        seam is cast to exactly one alias of the file, and every alias of the file is one
        converted seam's (V8).  The static_asserts then fail to compile when the seam's type
        drifts from its row or its frozen return type, so a later tree keeps the check the
        verifier made here (an alias edited together with its seam is not a contract)."""
        R = self.R
        T = self.tree.tokens(CONTRACT_TEST)
        aliases, uses = {}, collections.defaultdict(list)
        for j in range(T.n - 3):
            if T.t(j) == "using" and T.k(j + 1) == "id" and T.t(j + 2) == "=":
                e = j + 3
                while e < T.n and T.t(e) != ";":
                    e += 1
                aliases.setdefault(T.t(j + 1), []).append(T.join(j + 3, e))
            elif T.t(j) == "static_cast" and T.t(j + 1) == "<" and T.k(j + 2) == "id" \
                    and T.t(j + 3) == ">" and T.t(j + 4) == "(" and T.t(j + 5) == "&":
                k = j + 6
                if T.t(k) == "Application" and T.t(k + 1) == "::":
                    k += 2
                uses[T.t(k)].append(T.t(j + 2))
            elif T.t(j) == "static" and T.t(j + 1) == "constexpr" and T.k(j + 2) == "id" \
                    and T.t(j + 4) == "=" and T.t(j + 5) == "&" and T.t(j + 6) == "Application" \
                    and T.t(j + 7) == "::":
                uses[T.t(j + 8)].append(T.t(j + 2))
        owner = {}
        for name in sorted(self.converted):
            r = self.by_name[name]
            mine = [a for a in uses.get(name, ()) if a in aliases]
            if len(mine) != 1:
                R.fail("V8", f"{CONTRACT_TEST}: {name} is cast to {len(mine)} aliases of the file "
                             f"({', '.join(mine) or 'none'}); one expected")
                continue
            a = mine[0]
            owner.setdefault(a, []).append(name)
            fdecl = self.fdecls[name][0]
            types, _ = self.epoch_signature(r)
            ptr = "(Application::*)" if r.mech == MEMBER else "(*)"
            want = join_text(f"{alias_ret(fdecl.ret) if fdecl is not None else '?'} {ptr}({', '.join(types)})")
            if aliases[a] != [want]:
                R.fail("V8", f"{CONTRACT_TEST}: {name}'s alias {a} is `{' / '.join(aliases[a])}`; its "
                             f"epoch form's type is `{want}` (the frozen return type, the row's parameters)")
        for a in sorted(set(aliases) - set(owner)):
            R.fail("V8", f"{CONTRACT_TEST}: the alias {a} is no converted seam's")
        for a, names in owner.items():
            if len(names) > 1:
                R.fail("V8", f"{CONTRACT_TEST}: the alias {a} serves {names}")

    # ==== V3d: the run's construction ====================================================
    def v3d(self):
        R = self.R
        if not self.epoch:
            R.na("V3d", "empty conversion: no NetworkRun")
            self.canon = Canon({}, self.lam_names, self.frozen_root, ep_keys=(), run_keys=(),
                               value_equal=self.value_equal)
            return
        rn, T = self.cand.rn, self.cand.rn.T
        run_decl = next((d for d in rn.scopes.root.decls if d.name == "run" and d.type_span
                         and T.join(*d.type_span) == "NetworkRun"), None)
        if run_decl is None or run_decl.init_kind != "(":
            R.fail("V3d", f"{DRIVER_PATH}: no `NetworkRun run(...)` in runNetwork's outermost scope")
            self.canon = Canon({}, self.lam_names, self.frozen_root, value_equal=self.value_equal)
            return
        args = list(_split_top(T, *run_decl.init))
        hdr = self.tree.index(RUN_HPP)
        ctor = None
        if hdr:
            ctor = next((f for f in hdr.funcs if f.name == "NetworkRun" and f.cls == "NetworkRun"
                         and f.params and len(f.params) == len(args)), None)
        if ctor is None:
            R.fail("V3d", f"{RUN_HPP}: no NetworkRun constructor with {len(args)} parameters")
            self.canon = Canon({}, self.lam_names, self.frozen_root, value_equal=self.value_equal)
            return
        HT = self.tree.tokens(RUN_HPP)
        inits = _mem_initializers(HT, ctor)
        pidx = {p.name: k for k, p in enumerate(ctor.params)}
        res = rn.resolver()
        for member, want in RUN_REFS.items():
            init = inits.get(member)
            if init is None or init not in pidx:
                R.fail("V3d", f"NetworkRun::{member} is not initialized from a constructor parameter")
                continue
            s, e = args[pidx[init]]
            if e != s + 1 or T.k(s) != "id":
                R.fail("V3d", f"NetworkRun::{member}: the construction passes `{T.join(s, e)}`, not a name")
                continue
            d = rn.scopes.lookup(T.t(s), s)
            if d is None and T.t(s) not in rn.members:
                R.fail("V3d", f"NetworkRun::{member}: `{T.t(s)}` is not declared before the construction")
                continue
            if d is not None and d.scope is not rn.scopes.root:
                R.fail("V3d", f"NetworkRun::{member}: `{T.t(s)}` is not declared in runNetwork's outermost scope")
                continue
            got = res.resolve(s, e)
            want_node = self.frozen_root(want)
            if show(got) != show(want_node):
                R.fail("V3d", f"NetworkRun::{member} binds {show(got)}, the frozen object is "
                              f"{show(want_node)} ({want})")
            self.run_refs[member] = self.frozen_root(want)
        R.note("V3d", f"NetworkRun run(...) at {DRIVER_PATH}:{T.line(run_decl.tok)}: "
                      f"{len(self.run_refs)} reference members bound")
        self.tclaim(T, run_decl.stmt[0], run_decl.stmt[1])
        self.canon = Canon(self.run_refs, self.lam_names, self.frozen_root, value_equal=self.value_equal)
        self.run_struct()

    def run_struct(self):
        """NetworkRun (V3d): a plain struct (no base clause, nothing but members and member
        functions) whose members are the references then the owned groups, in the spec's
        order (RUN_REFS, `engines`, then the OWNER_MOVE groups in their declared order), and
        which is claimed for V1e here; each reference member is declared with its spec type (a
        reference: the run holds the frozen object itself) and initialized from a constructor
        parameter of that same type; the owned members are the engines' owner, the emit
        engines, the lanes, the timing and the closed-loop tally, each of the frozen driver
        local's type with no default member initializer; the one constructor initializes the
        references, `engines(<spec>)` and the owned members the frozen driver constructed with
        arguments (emit, lanes: V12 checks their arguments), has an empty body, and every other
        member function is deleted."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        cp = class_parts(self.tree, RUN_HPP, "NetworkRun")
        if cp is None:
            R.fail("V3d", f"{RUN_HPP}: struct NetworkRun is missing")
            return
        froot = {d.name: d for d in fz.rn.scopes.root.decls}
        # The owned members: the engines' owner, and each group an OWNER_MOVE entry places
        # directly under the run whose frozen local the base has (the lanes go with the lane
        # layer at a REMOVE-key base).
        owned = collections.OrderedDict()
        for n, path in SUBST.owner.items():
            m = path.split(".")[1]
            if m != "engines" and n in froot:
                owned[m] = froot[n]
        want = list(RUN_REFS) + ["engines"] + list(owned)
        if list(cp["members"]) != want or cp["others"]:
            R.fail("V3d", f"NetworkRun members {list(cp['members'])}"
                          + (f" and {[o[2][:40] for o in cp['others']]}" if cp["others"] else "")
                          + f"; the spec declares {want}, in that order (the owned groups are constructed "
                          "in declaration order and destroyed in reverse)")
        self.plain_struct("V3d", cp, "NetworkRun")
        self.tclaim(cp["T"], cp["head"], cp["end"])
        for m, t in RUN_REF_TYPES.items():
            x = cp["members"].get(m)
            if x is None:
                continue
            if (x["type"] + x["ops"]).replace(" ", "") != t.replace(" ", "") or x["init"]:
                R.fail("V3d", f"NetworkRun::{m} is `{x['type']}{x['ops']}`, not the reference `{t}`")
        for m in ["engines"] + list(owned):
            x = cp["members"].get(m)
            if x is None:
                continue
            t = "NetworkRunEngines" if m == "engines" else (fT.join(*owned[m].type_span) if m in owned else None)
            if t is None or x["type"] != t or x["ops"] or x["init"]:
                R.fail("V3d", f"NetworkRun::{m} is `{x['type']}{x['ops']} {x['init']}`, not a `{t}` with no "
                              "default member initializer")
        ctors = [f for f in cp["funcs"] if not f["deleted"]]
        if len(ctors) != 1 or ctors[0]["name"] != "NetworkRun":
            R.fail("V3d", f"NetworkRun: member functions {[f['name'] for f in ctors]} besides the deleted "
                          "ones; one constructor expected")
            return
        c = ctors[0]
        params = {q.name: lint.ptype(q) for q in c["params"]}
        spec = [q.name for q in c["params"] if lint.ptype(q) == "const NetworkEngineSpec&"]
        inits = {i[0]: i for i in c["inits"]}
        if len(inits) != len(c["inits"]):
            R.fail("V3d", "NetworkRun's constructor initializes a member twice")
        for m, t in RUN_REF_TYPES.items():
            i = inits.get(m)
            if i is None or len(i[2]) != 1:
                R.fail("V3d", f"NetworkRun's constructor does not initialize {m} from one parameter")
                continue
            arg = cp["T"].join(*i[2][0])
            if params.get(arg, "").replace(" ", "") != t.replace(" ", ""):
                R.fail("V3d", f"NetworkRun::{m} is initialized from `{arg}` of type `{params.get(arg)}`, not a "
                              f"`{t}` parameter (a reference member bound to anything else dangles or rebinds)")
        allowed = set(RUN_REF_TYPES) | {"engines"} | {m for m, d in owned.items() if d.init_kind in ("(", "{")}
        extra = sorted(set(inits) - allowed)
        if extra:
            R.fail("V3d", f"NetworkRun's constructor initializes {extra}, which the frozen driver "
                          "default-constructed or the spec does not declare")
        e = inits.get("engines")
        if e is None or len(spec) != 1 or [cp["T"].join(*x) for x in e[2]] != spec:
            R.fail("V3d", f"NetworkRun's constructor does not build `engines({spec[0] if spec else 'spec'})`")
        if c["body"] is None or c["body"][1] != c["body"][0] + 1:
            R.fail("V3d", "NetworkRun's constructor body is not empty")
        if member_trail(cp["T"], c):
            R.fail("V3d", f"NetworkRun's constructor has `{member_trail(cp['T'], c)}` after its parameter "
                          "list (nothing is declared there: an engine's construction that throws reaches "
                          "the run's handler, as the frozen driver's did)")

    # ==== V1a: the lambda's lines =========================================================
    def frozen_subs(self, s, e, tf):
        """Rendering substitutions of a frozen statement: converted calls' arguments, and the
        two replay closures' invocations under T-F."""
        T = self.fz.drv.T
        subs = []
        for c in calls_in(T, s, e):
            if c.name in self.converted and c.close > c.open + 1:
                if any(x.a <= c.tok < x.b for x in subs):
                    continue
                subs.append(Sub(c.open + 1, c.close, f"<ARGS:{c.name}>", True, True))
            elif tf and c.name in SUBST.callbacks and c.nargs == 0:
                builder = "network" + c.name[0].upper() + c.name[1:]
                subs.append(Sub(c.tok, c.close + 1, f"{builder}(<ARGS:{builder}>)"))
        return _drop_nested(subs)

    def new_subs(self, s, e, stores=False):
        """Rendering substitutions of a candidate statement: converted calls' arguments, the
        bracket rewrite, ep.emitted, and the epoch paths of dropped locals back to the
        local's name (spec 5.3 T-B7, T-B9, T-E).  Every substitution but a converted call's
        arguments is an EpSub, so check_alignment can prove that the frozen token it pairs
        with denotes what it stands for."""
        T = self.cand.T
        subs = []
        for c in calls_in(T, s, e):
            if c.name in self.converted and c.close > c.open + 1:
                subs.append(Sub(c.open + 1, c.close, f"<ARGS:{c.name}>", True, True))
        if self.epoch:
            for c in calls_in(T, s, e):
                if c.name in ("networkSubBegin", "networkSubEnd") and c.nargs == 2:
                    a0 = c.args[1][0]
                    want = f"{c.name}(run, "
                    got = T.source[T[c.tok].pos:T[a0].pos]
                    if got != want:
                        self.R.fail("V1a", f"{T.path}:{c.line}: bracket call `{got}...` is not `{want}...`")
                    old = "subBegin(" if c.name == "networkSubBegin" else "subEnd("
                    subs.append(EpSub(c.tok, a0, old, "bracket", old[:-1], False, True))
            j = s
            while j < e:
                if T.t(j) == "ep" and T.k(j) == "id" and T.t(j - 1) not in (".", "->", "::"):
                    k = ep_path_tokens(T, j, e)
                    path = norm_subs(T, j, k, ())
                    if path == "ep.emitted" and T.t(k) != "=" and self.value_equal:
                        subs.append(EpSub(j, k, self.value_equal, "value"))
                    else:
                        inv = self.inverse_path(path)
                        if inv is not None:
                            subs.append(EpSub(j, k, inv[0], "epath", inv[1]))
                    j = k
                    continue
                if T.t(j) in self.result_aliases() and T.t(j + 1) == "." and T.t(j + 2) == "handoff":
                    f = T.t(j + 4) if T.t(j + 3) == "." else ""
                    if f in SUBST.field_solve:
                        subs.append(EpSub(j, j + 5, f"{T.t(j)}.{f}", "handoff", T.t(j)))
                        j += 5
                        continue
                j += 1
        return _drop_nested(subs)

    def result_aliases(self):
        return {loc for loc, (p, t) in SUBST.epoch.items() if t == "RESULT"}

    def inverse_path(self, path):
        """(the frozen text a candidate epoch path stands for, the lambda local it names), or
        None: `ep.solve.action` -> (`sg_.action`, sg_), `ep.solve.handoff.retro_flag_sats` ->
        (`sg_.retro_flag_sats`, sg_), `ep.x_emit()` -> (`x_emit`, x_emit)."""
        for name, (struct, new) in SUBST.field_solve.items():
            p = "ep.solve." + new
            if path == p or path.startswith(p + "."):
                local = SUBST.paths.get("ep.solve", "sg_")
                return local + "." + name + path[len(p):], local
        for local, p in SUBST.accessor.items():
            if path == p:
                return local, local
        local, rest = SUBST.epoch_local_of(path)
        if local is None:
            return None
        return local + rest, local

    def check_alignment(self, fs, fe, ns, ne, where, record=True):
        """The frozen tokens [fs, fe) and the candidate's [ns, ne), each with its rendering
        substitutions, paired token by token (V1a): the candidate's substitutions must stand
        for frozen tokens that denote what they claim.  An `ep.<path>` rendered as a local's
        name must pair with a frozen token that is that very local (the lambda's top-level
        declaration, found by the frozen scope walk, not a nested declaration of the same
        name: the DD-carry predict's own `dt` is not ep.clock.dt); ep.emitted must pair with
        the frozen comparison over the driver's and the lambda's own names; a bracket with the
        driver's closure; a result alias with that alias.  Records the pairs for V3b."""
        R, fz = self.R, self.fz
        fT, nT = fz.drv.T, self.cand.T
        tf = "networkSolveGlue" in self.converted
        fu = _units(fT, fs, fe, self.frozen_subs(fs, fe, tf))
        nu = _units(nT, ns, ne, self.new_subs(ns, ne))
        if [u[0] for u in fu] != [u[0] for u in nu]:
            R.fail("V1a", f"{where}: the statement's tokens do not pair with frozen "
                          f"{DRIVER_PATH}:{fT.line(fs)} one to one")
            return False
        ok = True
        lam = fz.rn.lam_scope
        root = fz.rn.scopes.root
        for (ft, fi, fsub, _), (nt, ni, nsub, k) in zip(fu, nu):
            if not isinstance(nsub, EpSub):
                continue
            if fi is None:
                R.fail("V1a", f"{where}: `{nsub.text}` pairs with frozen substituted text")
                ok = False
                continue
            name_head = (fT.k(fi) == "id" and fT.t(fi - 1) not in (".", "->", "::")
                         and fT.t(fi) not in KEYWORDS)
            if not name_head:
                continue
            d = fz.rn.scopes.lookup(fT.t(fi), fi)
            if nsub.kind in ("epath", "handoff"):
                good = k > 0 or (d is not None and d.scope is lam and d.name == nsub.local)
                why = "the lambda's top-level local " + str(nsub.local)
            elif nsub.kind == "value":
                good = d is not None and d.scope in (lam, root)
                why = "a name of the lambda's or the driver's top level"
            else:                           # bracket
                good = d is not None and d.scope is root and d.name == nsub.local
                why = "the driver's closure " + str(nsub.local)
            if not good:
                got = ("nothing" if d is None else
                       f"a declaration at {DRIVER_PATH}:{d.line}" + ("" if d.scope in (lam, root)
                                                                    else " in a nested scope"))
                R.fail("V1a", f"{where}: `{nsub.text}` stands for frozen `{fT.t(fi)}` at "
                              f"{DRIVER_PATH}:{fT.line(fi)}, which denotes {got}, not {why}")
                ok = False
        if record:
            self.aligned.append((where, fu, nu))
        return ok

    def classify(self, s, e):
        """(kind, declarations) of a frozen lambda statement for the pairing of V1a."""
        T = self.fz.drv.T
        ds = parse_declaration(T, s, e) or []
        names = [d.name for d in ds]
        if not ds:
            return "PLAIN", ds
        if any(n in SUBST.epoch for n in names):
            tr = {SUBST.epoch[n][1] for n in names if n in SUBST.epoch}
            if tr == {"CLOCK"}:
                return "PLAIN", ds
            if tr == {"WS"}:
                return "WS", ds
            return "EPOCH_DECL", ds
        if any(n in SUBST.accessor for n in names):
            return "ACCESSOR", ds
        if any(n in SUBST.callbacks for n in names):
            return "CLOSURE", ds
        if len(ds) == 1 and ds[0].is_ref and ds[0].init:
            a, b = ds[0].init
            if T.k(a) == "id" and T.t(a) in self.result_aliases() and T.t(a + 1) == ".":
                return "MEMBER_ALIAS", ds
        return "PLAIN", ds

    def v1a(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.drv.T, cand.T
        F, N = fz.lam_stmts, cand.stmts
        if self.epoch and getattr(cand, "no_marker", False):
            R.fail("V1a", f"{cand.fn.path}: no `// ---- End of the run's names ----` marker after the "
                          "epoch function's preamble")
        tf = "networkSolveGlue" in self.converted
        canon_pair = next((k for k, (s, e) in enumerate(F)
                           if any(c.name == "networkCanonicalPairAdmission" for c in calls_in(fT, s, e))), None)
        clock_last = max((k for k, (s, e) in enumerate(F)
                          for d in (parse_declaration(fT, s, e) or [])
                          if d.name in SUBST.epoch and SUBST.epoch[d.name][1] == "CLOCK"), default=None)
        emit_close = None
        emit_k = next((k for k, (s, e) in enumerate(F)
                       if any(c.name == "networkEmitStore" for c in calls_in(fT, s, e))), None)
        if emit_k is not None:
            emit_close = next((k for k in range(emit_k + 1, len(F))
                               if fT.join(*F[k]) == "subEnd(SB_EMITSTORE);"), None)
        inserts = {}
        if self.epoch:
            if canon_pair is not None:
                inserts.setdefault(canon_pair, []).append(("CTOR", "NetworkEpoch ep(se);"))
            if clock_last is not None:
                clk = [n for n, (p, t) in SUBST.epoch.items() if t == "CLOCK"]
                inserts.setdefault(clock_last, []).append(
                    ("CLOCK", "ep.clock = EpochClock{ " + ", ".join(clk) + " };"))
            if emit_close is not None and SUBST.value_equal:
                inserts.setdefault(emit_close, []).append(
                    ("EMITTED", f"ep.emitted = {SUBST.value_equal};"))
        self.inserted = {}
        j = 0
        carried = transformed = 0
        dropped = []
        for i, (s, e) in enumerate(F):
            kind, ds = self.classify(s, e)
            if not self.epoch:
                kind = "PLAIN"
            if kind == "WS":
                for d in ds:
                    self.realized[d.name] = {"transform": "WS", "stmt": None, "frozen": i,
                                             "type": fT.join(*d.type_span), "path": SUBST.epoch[d.name][0]}
                transformed += 1
            elif kind == "EPOCH_DECL":
                j = self.realize(i, s, e, ds, j)
                transformed += 1
            elif kind == "ACCESSOR":
                if j < len(N) and nT.join(*N[j]) == join_text(
                        f"const Eigen::VectorXd& {ds[0].name} = {SUBST.accessor[ds[0].name]};"):
                    self.realized[ds[0].name] = {"transform": "ACCESSOR", "stmt": j, "frozen": i}
                    j += 1
                else:
                    self.realized[ds[0].name] = {"transform": "ACCESSOR", "stmt": None, "frozen": i}
                    dropped.append((ds[0].name, j))
                transformed += 1
            elif kind == "CLOSURE" and tf:
                transformed += 1
            elif kind == "MEMBER_ALIAS":
                if j < len(N) and self.same_stmt(s, e, *N[j], quiet=True):
                    self.pairs.append((i, j))
                    self.check_alignment(s, e, *N[j], f"{nT.path}:{nT.line(N[j][0])}")
                    j += 1
                    carried += 1
                else:
                    dropped.append((ds[0].name, j))
                    transformed += 1
            else:
                k = self.match_ahead(s, e, N, j)
                if k is None:
                    R.fail("V1a", f"frozen {DRIVER_PATH}:{fT.line(s)}-{fT.line(e - 1)} is not carried: "
                                  f"`{fT.join(s, min(e, s + 12))}...`" + self.first_diff(s, e, N, j))
                else:
                    for x in range(j, k):
                        R.fail("V1a", f"{nT.path}:{nT.line(N[x][0])}: a statement the lambda does not "
                                      f"have: `{nT.join(N[x][0], min(N[x][1], N[x][0] + 12))}...`")
                    self.pairs.append((i, k))
                    self.check_alignment(s, e, *N[k], f"{nT.path}:{nT.line(N[k][0])}")
                    j = k + 1
                    carried += 1
            for tag, text in inserts.get(i, ()):
                if j < len(N) and nT.join(*N[j]) == join_text(text):
                    self.inserted[tag] = j
                    self.tclaim(nT, N[j][0], N[j][1])
                    j += 1
                else:
                    R.fail("V1a", f"the inserted `{text}` ({tag}) does not follow frozen "
                                  f"{DRIVER_PATH}:{fT.line(s)}")
                    continue
                if tag != "CTOR":
                    continue
                # T-B2: the workspace was declared before `ep` could exist; while explicit
                # calls read `ws`, its alias follows the epoch's construction.
                for local, (path, tr) in SUBST.epoch.items():
                    if tr != "WS" or j >= len(N):
                        continue
                    nd = parse_declaration(nT, *N[j]) or []
                    if (len(nd) == 1 and nd[0].name == local and nd[0].is_ref and nd[0].init
                            and nT.join(*nd[0].init) == path and not nd[0].is_const):
                        ntype = nT.join(*nd[0].type_span)
                        ftype = self.realized.get(local, {}).get("type") or ntype
                        if ntype not in ("auto", ftype):
                            R.fail("V1a", f"{nT.path}:{nT.line(N[j][0])}: the alias of {local} has "
                                          f"type `{ntype}`, the frozen local `{ftype}`")
                        self.realized.setdefault(local, {})["alias"] = j
                        self.tclaim(nT, N[j][0], N[j][1])
                        j += 1
        for x in range(j, len(N)):
            R.fail("V1a", f"{nT.path}:{nT.line(N[x][0])}: a statement the lambda does not have: "
                          f"`{nT.join(N[x][0], min(N[x][1], N[x][0] + 12))}...`")
        # A dropped declaration's name is not read afterwards (unless a nested declaration
        # of the same name shadows it, as in the frozen text).
        for name, at in dropped:
            for k in range(at, len(N)):
                s, e = N[k]
                for t in ids_in(nT, s, e):
                    if nT.t(t) == name and nT.t(t + 1) != "(":
                        d = cand.scopes.lookup(name, t)
                        if d is None or d.scope is cand.scopes.root:
                            R.fail("V1a", f"{nT.path}:{nT.line(t)}: `{name}` is read but its declaration "
                                          "was dropped")
                            break
        self.fclaim(DRIVER_PATH, fT.line(fz.rn.lam.stmt[0]), fT.line(fz.rn.lam.stmt[1] - 1))
        if self.epoch:
            fn = cand.fn
            self.epoch_head("V1a", fn.func, nT)
            self.tclaim(nT, decl_start(nT, fn.func), fn.close + 1)
        R.note("V1a", f"{len(F)} frozen top-level statements: {carried} carried "
                      f"{'(shift ' + str(cand.shift) + ')' if cand.shift else 'verbatim'}, "
                      f"{transformed} by a declared transform; {len(self.inserted)} inserted stores")

    def epoch_head(self, chk, f, T):
        """The head of Application::networkProcessEpoch_ (@p f, its declaration or its
        definition) is the frozen lambda's (spec 4.1): `NetworkRun& run`, then the lambda's
        parameter list (names and defaults included); the lambda's trailing return type, which
        the definition names `Application::<type>` when TYPE_MOVE made it a member type; and
        after the list exactly the lambda's specifiers but `mutable` (none at the base).  So
        neither `noexcept` (an escaping exception would call std::terminate instead of
        reaching Application::run's handler, which logs it and returns 1), nor a reference
        return, nor `static` / `virtual` / an attribute enters with the head the check claims
        (V1a the definition, V1d the declaration)."""
        lh = lambda_head(self.fz.rn.T, self.fz.rn.lam)
        if lh is None or lh[2] is None:
            self.R.fail(chk, f"the frozen {LAMBDA_NAME} has no parameter list or no trailing return type, "
                             "so the epoch function's head cannot be derived")
            return
        params, spec, ret = lh
        if f.is_def and ret in SUBST.type_move:
            ret = "Application::" + ret
        want = {"ret": ret, "text": join_text(f"{EPOCH_RUN_PARAM}, {params}" if params else EPOCH_RUN_PARAM),
                "trail": spec}
        what = "definition" if f.is_def else "declaration"
        if f.cls != "Application":
            self.R.fail(chk, f"{f.path}:{f.line}: the {what} of {lint.EPOCH_FN} is not Application's")
        for msg in head_diff(T, f, want):
            self.R.fail(chk, f"{f.path}:{f.line}: the {what} of Application::{lint.EPOCH_FN} {msg} (the "
                             f"frozen {LAMBDA_NAME}'s head)")

    def match_ahead(self, s, e, N, j, ahead=6):
        for k in range(j, min(len(N), j + ahead)):
            if self.same_stmt(s, e, *N[k], quiet=True):
                return k
        return None

    def first_diff(self, s, e, N, j):
        if j >= len(N):
            return " (the epoch body ended)"
        fT, nT = self.fz.drv.T, self.cand.T
        tf = "networkSolveGlue" in self.converted
        a = render(fT, s, e, self.frozen_subs(s, e, tf))
        b = render(nT, N[j][0], N[j][1], self.new_subs(N[j][0], N[j][1]))
        d = same_lines(a, b, self.cand.shift)
        if d is None:
            return ""
        k, x, y = d
        return (f"; first difference against {nT.path}:{nT.line(N[j][0])}: frozen "
                f"`{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")

    def same_stmt(self, s, e, ns, ne, quiet=False):
        fT, nT = self.fz.drv.T, self.cand.T
        tf = "networkSolveGlue" in self.converted
        a = render(fT, s, e, self.frozen_subs(s, e, tf))
        b = render(nT, ns, ne, self.new_subs(ns, ne))
        return same_lines(a, b, self.cand.shift) is None

    def realize(self, i, s, e, ds, j):
        """Pairs the frozen declaration of one or more epoch locals with its realization
        (spec 5.3 T-B3, T-B4, T-B5, T-B7); returns the next candidate statement."""
        R, fT, nT, N = self.R, self.fz.drv.T, self.cand.T, self.cand.stmts
        for d in ds:
            if d.name not in SUBST.epoch:
                R.fail("V1a", f"frozen {DRIVER_PATH}:{d.line}: `{d.name}` shares a declaration with "
                              "an epoch local but is not one")
                continue
            path, tr = SUBST.epoch[d.name]
            ftype = fT.join(*d.type_span)
            rec = {"transform": tr, "stmt": None, "alias": None, "frozen": i, "path": path,
                   "type": ftype, "init": fT.norm(*d.init) if d.init else "", "decl": d}
            self.realized[d.name] = rec
            if tr in ("SIZED", "RESULT"):
                if j >= len(N):
                    R.fail("V1a", f"{d.name} ({tr}): the epoch body ended before its store")
                    continue
                ns, ne = N[j]
                lhs = norm_subs(nT, ns, ep_path_tokens(nT, ns, ne), ())
                if nT.t(ns) != "ep" or lhs != path or nT.t(ep_path_tokens(nT, ns, ne)) != "=":
                    R.fail("V1a", f"{nT.path}:{nT.line(ns)}: expected the store `{path} = ...` of "
                                  f"{d.name} ({tr}) where frozen {DRIVER_PATH}:{d.line} declared it")
                    continue
                q = ep_path_tokens(nT, ns, ne) + 1
                rhs = norm_subs(nT, q, ne - 1, self.new_subs(q, ne - 1))
                if tr == "SIZED":
                    want = join_text(f"{fT.src(*d.type_span)}") + "(" + fT.join(*d.init) + ")"
                else:
                    want = norm_subs(fT, d.init[0], d.init[1],
                                     self.frozen_subs(d.init[0], d.init[1], "networkSolveGlue" in self.converted))
                if rhs != want:
                    R.fail("V1a", f"{nT.path}:{nT.line(ns)}: {d.name} ({tr}) stores `{rhs}`, the frozen "
                                  f"initializer is `{want}`")
                elif tr == "RESULT":
                    self.check_alignment(d.init[0], d.init[1], q, ne - 1, f"{nT.path}:{nT.line(ns)}")
                elif nT.t(ne - 2) == ")" and nT.match[ne - 2] > q:
                    self.check_alignment(d.init[0], d.init[1], nT.match[ne - 2] + 1, ne - 2,
                                         f"{nT.path}:{nT.line(ns)}")
                rec["stmt"] = j
                self.tclaim(nT, ns, ne)
                j += 1
            if j < len(N):
                ns, ne = N[j]
                nd = parse_declaration(nT, ns, ne) or []
                if len(nd) == 1 and nd[0].name == d.name and nd[0].is_ref and nd[0].init:
                    init = norm_subs(nT, nd[0].init[0], nd[0].init[1], ())
                    ntype = nT.join(*nd[0].type_span)
                    okt = ntype in ("auto", "const auto", ftype, "const " + ftype,
                                    ftype.replace("const ", "", 1))
                    if init != path or not okt or nd[0].ops != "&":
                        R.fail("V1a", f"{nT.path}:{nT.line(ns)}: the alias of {d.name} binds `{init}` as "
                                      f"`{ntype}{nd[0].ops}`; expected `{path}` as a reference of type "
                                      f"auto or {ftype}")
                    rec["alias"] = j
                    if rec["stmt"] is None:
                        rec["stmt"] = j
                    self.tclaim(nT, ns, ne)
                    j += 1
            if tr in ("ALIAS", "SCALAR") and rec["alias"] is None:
                rec["dropped_at"] = j
        return j

    # ==== V3b: captured names =============================================================
    def v3b(self):
        """Every read of a captured driver name in the epoch body against its frozen root, over
        the token pairs check_alignment recorded for each carried statement and each epoch
        store's initializer (a substituted token is proved there, so no statement is skipped),
        then the epoch function's preamble (check_preamble)."""
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.drv.T, cand.T
        fres = fz.res
        nres = cand.resolver()
        n = bad = 0
        for where, fu, nu in self.aligned:
            for (ft, a, fsub, _), (nt, b, nsub, _) in zip(fu, nu):
                if a is None or b is None:
                    continue                # substituted text: proved by check_alignment (V1a)
                if (fT.k(a) != "id" or fT.t(a) in KEYWORDS or fT.t(a - 1) in (".", "->", "::")
                        or fT.t(a + 1) == "::" or fT.t(a) != nT.t(b)):
                    continue
                d = fz.rn.scopes.lookup(fT.t(a), a)
                if d is None or d.kind == "type" or d.scope is not fz.rn.scopes.root:
                    continue
                if d.kind == "closure" or d.name in SUBST.bracket or d.name in SUBST.callable:
                    continue
                n += 1
                want = normalize(frozen_canon(fres.resolve(a, a + 1)), self.const_roots)
                got = normalize(self.canon(nres.resolve(b, b + 1)), self.const_roots)
                if show(want) != show(got):
                    bad += 1
                    if bad <= 25:
                        R.fail("V3b", f"{nT.path}:{nT.line(b)}: `{fT.t(a)}` resolves to {show(got)}; "
                                      f"the captured name is {show(want)}")
        if bad > 25:
            R.fail("V3b", f"... {bad - 25} more")
        if self.epoch:
            self.check_preamble()
        R.note("V3b", f"{n} reads of captured driver names in {len(self.aligned)} paired statements "
                      "compared with their frozen roots")

    def check_preamble(self):
        """Each line of the epoch function's preamble declares a name the frozen lambda
        captured and binds its frozen object (V3b), with the frozen declarator: a run
        reference member (spec 2.1) under its member type, an owned engine or group
        (OWNER_MOVE) and a moved timer vector (FIELD_MOVE) as a reference of the frozen type,
        anything else with the frozen declaration's type text and operators (`const` may be
        added on a reference, nothing else changes).  The third-band predicate's forwarder is
        its one declared form (check_forwarder)."""
        R, cand = self.R, self.cand
        T = cand.T
        res = cand.resolver()
        fres = self.fz.res
        fT = self.fz.rn.T
        caps = self.fz.drv.captures()
        froot = {d.name: d for d in self.fz.rn.scopes.root.decls}
        for s, e in cand.preamble:
            self.tclaim(T, s, e)
            ds = parse_declaration(T, s, e) or []
            if not ds:
                R.fail("V3b", f"{T.path}:{T.line(s)}: the preamble holds a statement that declares "
                              f"nothing: `{T.join(s, min(e, s + 10))}...`")
            if len(ds) > 1:
                R.fail("V3b", f"{T.path}:{T.line(s)}: the preamble line declares {len(ds)} names")
            for d in ds:
                fd = froot.get(d.name)
                if d.name in SUBST.callable:
                    want = fT.src(fd.stmt[0], fd.init[0] - 1) if fd and fd.init else None
                    self.check_forwarder(T, s, e, d, res, "V3b", want)
                    continue
                if d.name not in caps:
                    R.fail("V3b", f"{T.path}:{d.line}: the preamble declares `{d.name}`, which the frozen "
                                  "lambda did not capture")
                    continue
                if fd is None:
                    continue
                if d.init_kind != "=":
                    R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` is not copy-initialized (`=`)")
                ftype = fT.join(*fd.type_span)
                got_t = T.join(*d.type_span) + d.ops
                if RUN_REFS.get(d.name) == "D:" + d.name:
                    allowed = {join_text(RUN_REF_TYPES[d.name])}
                elif d.name in SUBST.owner or d.name in SUBST.field_timing:
                    allowed = {ftype + "&"}
                else:
                    allowed = {ftype + fd.ops}
                    if fd.ops.startswith("&") and not ftype.startswith("const"):
                        allowed.add(join_text("const " + ftype) + fd.ops)
                if got_t not in allowed:
                    R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` is declared `{got_t}`; the frozen "
                                  f"declaration admits {' or '.join(sorted(allowed))}")
                if fd.is_ref and not d.is_ref:
                    R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` is a copy; the frozen name is a reference")
                if not fd.is_ref and not d.is_ref and not d.is_const:
                    R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` is a non-const copy")
                if d.init:
                    got = normalize(self.canon(res.resolve(*d.init)), self.const_roots)
                    if not d.is_ref and not d.is_ptr:
                        got = normalize(Copy(got), self.const_roots)
                    want = normalize(frozen_canon(fres.name_ref(fd.name, self.fz.rn.lam_open, ())),
                                     self.const_roots)
                    if show(got) != show(want) and not (isinstance(got, Ref) and show(got) == "D:" + d.name):
                        R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` binds {show(got)}; frozen {show(want)}")

    def callable_closure(self):
        """(tokens, declaration, lambda parts) of the frozen third-band closure, or None."""
        fT = self.fz.rn.T
        fd = next((d for d in self.fz.rn.scopes.root.decls if d.name in SUBST.callable
                   and d.kind == "closure"), None)
        if fd is None:
            return None
        lam = _lambda_at(fT, fd.init[0], fd.init[1])
        return (fT, fd, lam) if lam else None

    def check_forwarder(self, T, s, e, d, res, chk, want_decl):
        """CALLABLE (spec 5.6): the binding of the third-band closure in the epoch function's
        preamble and in the DD build's bindings is its one declared form,
            <frozen declarator> = [&X](<the frozen closure's parameters>) -> <its return> {
                return networkIsUncarriedThirdBand(X.<path>, <the parameters, in order>);
            };
        where X is the one capture, by reference, and X.<path> resolves to the knobs
        (D:ctx.knobs, through the run's construction where X is the run).  @p want_decl is the
        frozen declarator text (the closure's `auto isUncarriedThirdBand`, or the DD build's
        std::function parameter); @p res resolves names at the binding."""
        R = self.R
        where = f"{T.path}:{T.line(s)}"
        fc = self.callable_closure()
        if fc is None:
            R.fail(chk, f"{where}: the frozen third-band closure was not found")
            return
        fT, fd, (fcc, fpo, fbo) = fc
        if d.kind != "closure" or d.init_kind != "=":
            R.fail(chk, f"{where}: {d.name} is not bound to a lambda")
            return
        if want_decl is None or join_text(T.src(s, d.init[0] - 1)) != join_text(want_decl):
            R.fail(chk, f"{where}: {d.name} is declared `{T.src(s, d.init[0] - 1)}`, frozen `{want_decl}`")
        a, b = d.init
        lam = _lambda_at(T, a, b)
        if not lam:
            R.fail(chk, f"{where}: {d.name}'s initializer is not a lambda")
            return
        cc, po, bo = lam
        bc = T.close(bo)
        if bc + 1 != b:
            R.fail(chk, f"{where}: {d.name}'s initializer continues after the lambda")
        if not (cc == a + 3 and T.t(a + 1) == "&" and T.k(a + 2) == "id"):
            R.fail(chk, f"{where}: {d.name}'s lambda captures `{T.join(a, cc + 1)}`, not one name by reference")
            return
        cap = T.t(a + 2)
        fpc = fT.close(fpo)
        if po is None or T.join(po, T.close(po) + 1) != fT.join(fpo, fpc + 1):
            R.fail(chk, f"{where}: {d.name}'s parameters are not the frozen closure's "
                        f"`{fT.join(fpo, fpc + 1)}`")
            return
        if T.join(T.close(po) + 1, bo) != fT.join(fpc + 1, fbo):
            R.fail(chk, f"{where}: {d.name}'s return is not the frozen closure's `{fT.join(fpc + 1, fbo)}`")
        names = [q.name for q in parse_params(fT, fpo)[0]]
        st = statements(T, bo + 1, bc)
        q = st[0][0] if len(st) == 1 else None
        if (q is None or T.t(q) != "return" or T.t(q + 1) != PREDICATE or T.t(q + 2) != "("
                or T.close(q + 2) + 2 != st[0][1] or T.t(st[0][1] - 1) != ";"):
            R.fail(chk, f"{where}: {d.name}'s body is not the one statement `return {PREDICATE}(...);`")
            return
        args = _split_top(T, q + 3, T.close(q + 2))
        if len(args) != 1 + len(names) or [T.join(*x) for x in args[1:]] != names:
            R.fail(chk, f"{where}: {d.name} forwards `{T.join(q + 3, T.close(q + 2))}`, not the knobs and "
                        f"{', '.join(names)} in order")
            return
        k0, k1 = args[0]
        if T.t(k0) != cap:
            R.fail(chk, f"{where}: {d.name} forwards `{T.join(k0, k1)}`, which is not read through its "
                        f"capture `{cap}`")
            return
        got = normalize(self.canon(res.resolve(k0, k1)), self.const_roots)
        if show(got) != "D:ctx.knobs":
            R.fail(chk, f"{where}: {d.name} forwards `{T.join(k0, k1)}`, which binds {show(got)}, not the "
                        "knobs (D:ctx.knobs)")

    # ==== V3e: fresh names ================================================================
    def v3e(self):
        R, cand = self.R, self.cand
        T = cand.T
        n = 0
        decl_toks = {d.tok for sc in cand.scopes.all for d in sc.decls}
        for j in ids_in(T, cand.open + 1, cand.close):
            name = T.t(j)
            if name not in ("run", "ep", "nrun", "nep") or j in decl_toks:
                continue
            n += 1
            d = cand.scopes.lookup(name, j)
            if self.epoch:
                ok = d is not None and (d.kind != "param" or name == "run" or name == cand.fn.run_name)
                if name in ("nrun", "nep") and (d is None or d.scope is cand.scopes.root):
                    ok = False
            else:
                ok = d is not None
            if not ok:
                R.fail("V3e", f"{T.path}:{T.line(j)}: `{name}` is read but declared nowhere in scope")
        for name in sorted(self.converted):
            if self.by_name[name].mech == ADAPTER:
                continue
            for f in self.new_defs(name):
                DT = self.tree.tokens(f.path)
                sp = lint.preamble_span(DT, f)
                lo = sp[1] if sp else f.body[0] + 1
                pnames = {p.name for p in f.params[:2]}
                sc = Scopes(DT, f.body[0], f.body[1], f.params)
                builders = {"network" + c[0].upper() + c[1:] for c in SUBST.callbacks}
                allowed = [(c.open, c.close) for c in calls_in(DT, lo, f.body[1])
                           if name == lint.SOLVE and c.name in builders]
                for j in ids_in(DT, lo, f.body[1]):
                    if any(x < j < y for x, y in allowed):
                        continue
                    if DT.t(j) in pnames:
                        d = sc.lookup(DT.t(j), j)
                        if d is not None and d.kind == "param":
                            R.fail("V3e", f"{f.path}:{DT.line(j)}: {name}'s body below its bindings reads "
                                          f"the parameter `{DT.t(j)}`")
        R.note("V3e", f"{n} uses of run / ep / nrun / nep in the epoch body; "
                      f"{len(self.converted)} converted bodies scanned")

    # ==== V4 ==============================================================================
    def v4(self):
        R, cand = self.R, self.cand
        if self.epoch:
            lt = lint.Lint(self.tree)
            lt.v4()
            for p in lt.problems["V4"]:
                R.fail("V4", p)
            for p in lt.info["V4"]:
                R.note("V4", p)
            return
        T = cand.T
        res = cand.resolver()
        solve = next((c for c in calls_in(T, cand.open + 1, cand.close) if c.name == lint.SOLVE), None)
        if solve is None:
            R.fail("V4", "the lambda has no networkSolveGlue call")
            return
        k = stmt_of(cand.stmts, solve.tok)
        solve_end = cand.stmts[k][1]
        emit = next((c.tok for c in calls_in(T, cand.open + 1, cand.close) if c.name == lint.EMIT), None)
        const_locals = {d.name for d in cand.rn.lam_scope.decls if d.is_const}

        def epath(s, e):
            try:
                node = res.resolve(s, e)
            except (ValueError, IndexError):
                return None
            if not isinstance(node, Ref) or node.prefix == "*":
                return None
            kind, _, name = node.root.partition(":")
            if kind != "L" or name in const_locals:
                return None
            chain = "".join(node.chain)
            for f, (struct, new) in SUBST.field_solve.items():
                if name == SUBST.paths.get("ep.solve") and (chain == "." + f or chain.startswith("." + f + ".")):
                    return "ep.solve." + new + chain[len("." + f):]
            if name in SUBST.epoch:
                return SUBST.epoch[name][0] + chain
            return None

        probs, writes, allowed = lint.check_v4(T, solve_end, cand.close, epath, self.decls, emit)
        for p in probs:
            R.fail("V4", p)
        R.note("V4", f"empty conversion: {len(writes)} writes into the epoch locals after the solve "
                     f"({len(allowed)} declared outputs)")

    # ==== V6: order =======================================================================
    def events(self, T, a, b, scopes, closures, inserts=None, skip_closures=True):
        """Execution-order events of tokens [a, b) (see the module docstring)."""
        ev = []
        bodies = {}
        for name, (lo, hi) in closures.items():
            bodies[name] = (lo, hi)
        skip = [v for v in bodies.values() if a <= v[0] < b] if skip_closures else []
        stmts_ins = inserts or {}
        j = a
        while j < b:
            if any(lo <= j <= hi for lo, hi in skip):
                j = next(hi for lo, hi in skip if lo <= j <= hi) + 1
                continue
            if j in stmts_ins:
                ev.append(("store", stmts_ins[j]))
            t = T.t(j)
            if t == "return" and T.t(j + 1) == "LoopSignal" and T.t(j + 2) == "::":
                ev.append(("return", T.t(j + 3)))
            if T.k(j) == "id" and T.t(j + 1) == "(" and T.t(j - 1) not in (".", "->", "::"):
                name, k = t, j
                q = j
                while T.t(q - 1) in (".", "->", "::") and T.k(q - 2) == "id":
                    name = T.t(q - 2) + T.t(q - 1) + name
                    q -= 2
                ev.extend(self.call_event(T, j, name, closures))
            elif T.k(j) == "id" and T.t(j + 1) == "(" and T.t(j - 1) in (".", "->", "::"):
                q, name = j, t
                while T.t(q - 1) in (".", "->", "::") and T.k(q - 2) == "id":
                    name = T.t(q - 2) + T.t(q - 1) + name
                    q -= 2
                if T.t(q - 1) not in (".", "->", "::"):
                    ev.extend(self.call_event(T, j, name, closures))
            j += 1
        return ev

    def call_event(self, T, j, name, closures):
        if name == LAMBDA_NAME:
            return [("call", lint.EPOCH_FN)]
        if name in ("subBegin", "subEnd", "networkSubBegin", "networkSubEnd"):
            close = T.match[j + 1]
            args = [T.join(s, e) for s, e in _split_top(T, j + 2, close)]
            return [("bracket", name.replace("network", "").replace("SubB", "subB").replace("SubE", "subE"),
                     args[-1] if args else "")]
        if name in closures:
            lo, hi = closures[name]
            return self.events(T, lo + 1, hi, None, {k: v for k, v in closures.items() if k != name},
                               skip_closures=True)
        m = re.match(r"^log->(\w+)$", name)
        if m:
            close = T.match[j + 1]
            args = _split_top(T, j + 2, close)
            level = m.group(1)
            lit_at = 0
            if level == "log" and args:
                level = "log:" + T.join(*args[0])
                lit_at = 1
            lit = ""
            if len(args) > lit_at:
                s, e = args[lit_at]
                lit = "".join(T.t(q) for q in range(s, e) if T.k(q) == "str")
            return [("log", level, lit)]
        if name == LAMBDA_NAME:
            return [("call", lint.EPOCH_FN)]
        if SEAM_CALL.match(name) or re.match(r"^network\w+$", name):
            return [("call", name)]
        return []

    def closures_in(self, T, scope):
        out = {}
        for d in scope.decls:
            if d.kind == "closure" and d.lambda_scope is not None:
                ls = d.lambda_scope
                out[d.name] = (T.match[ls.end], ls.end)
        return out

    def v6(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.drv.T, cand.T
        fclos = self.closures_in(fT, fz.rn.lam_scope)
        fclos_drv = {k: v for k, v in self.closures_in(fT, fz.rn.scopes.root).items()
                     if k in SUBST.bracket}
        finserts = {}
        if self.epoch:
            F = fz.lam_stmts
            canon_k = next((k for k, (s, e) in enumerate(F)
                            if any(c.name == "networkCanonicalPairAdmission" for c in calls_in(fT, s, e))), None)
            clock_k = max((k for k, (s, e) in enumerate(F) for d in (parse_declaration(fT, s, e) or [])
                           if SUBST.epoch.get(d.name, ("", ""))[1] == "CLOCK"), default=None)
            emit_k = next((k for k, (s, e) in enumerate(F)
                           if any(c.name == "networkEmitStore" for c in calls_in(fT, s, e))), None)
            emit_close = next((k for k in range(emit_k + 1, len(F))
                               if fT.join(*F[k]) == "subEnd(SB_EMITSTORE);"), None) if emit_k is not None else None
            for k, tag in ((canon_k, "ep"), (clock_k, "clock"), (emit_close, "emitted")):
                if k is not None and k + 1 < len(F):
                    finserts[F[k + 1][0]] = tag
                elif k is not None:
                    finserts[fz.rn.lam_close] = tag
        fev = self.events(fT, fz.rn.lam_open + 1, fz.rn.lam_close + 1, None, dict(fclos, **fclos_drv),
                          finserts)
        nclos = self.closures_in(nT, cand.scopes.root if self.epoch else cand.rn.lam_scope)
        if not self.epoch:
            nclos.update({k: v for k, v in self.closures_in(nT, cand.rn.scopes.root).items()
                          if k in SUBST.bracket})
        ninserts = {}
        if self.epoch:
            for s, e in cand.stmts:
                txt = nT.join(s, e)
                if txt == join_text("NetworkEpoch ep(se);"):
                    ninserts[s] = "ep"
                elif txt.startswith("ep.clock="):
                    ninserts[s] = "clock"
                elif txt.startswith("ep.emitted="):
                    ninserts[s] = "emitted"
            a = cand.stmts[0][0] if cand.stmts else cand.close
        else:
            a = cand.open + 1
        nev = self.events(nT, a, cand.close + 1, None, nclos, ninserts)
        fev = [e for e in fev if e[0] != "call" or e[1] not in SUBST.callbacks]
        self.compare_events("V6", "the epoch body", fev, nev)
        # runNetwork against its frozen self minus the lambda.
        fskip = {LAMBDA_NAME: (fz.rn.lam_open, fz.rn.lam_close)}
        fev2 = self.events(fT, fz.rn.rn.body[0] + 1, fz.rn.rn.body[1], None,
                           dict(self.closures_in(fT, fz.rn.scopes.root), **fskip))
        rT = cand.rn.T
        nskip = self.closures_in(rT, cand.rn.scopes.root)
        if not self.epoch and cand.rn.lam:
            nskip[LAMBDA_NAME] = (cand.rn.lam_open, cand.rn.lam_close)
        nev2 = self.events(rT, cand.rn.rn.body[0] + 1, cand.rn.rn.body[1], None, nskip)
        # The lambda's body events are counted with the epoch body; at the two call sites of
        # the lambda (frozen) or of networkProcessEpoch_ (candidate) one call event stands.
        fev2 = _expand_lambda_calls(fev2)
        nev2 = _expand_lambda_calls(nev2)
        if self.epoch:
            nev2 = [e for e in nev2 if e != ("call", "networkEngineSpec")]
        self.compare_events("V6", "runNetwork", fev2, nev2)
        R.note("V6", f"epoch body {len(nev)} events, runNetwork {len(nev2)} events")

    def compare_events(self, chk, what, fev, nev):
        if fev == nev:
            return
        k = next((k for k in range(min(len(fev), len(nev))) if fev[k] != nev[k]), min(len(fev), len(nev)))
        self.R.fail(chk, f"{what}: the order differs at event {k} of {len(fev)}/{len(nev)}: frozen "
                         f"{fev[k] if k < len(fev) else '<end>'}, here {nev[k] if k < len(nev) else '<end>'}")

    # ==== V1c: unconverted rows and adapters' explicit forms ================================
    def v1c(self):
        R = self.R
        n = 0
        for r in self.rows:
            if isinstance(r.mech, Explicit):
                continue
            fdecl, fdef = self.fdecls.get(r.name, (None, None))
            if fdecl is None or fdef is None:
                R.fail("V1c", f"{r.name}: not found at the base")
                continue
            explicit_must_stay = r.name not in self.converted or r.mech == ADAPTER
            if not explicit_must_stay:
                continue
            for f, path in ((fdecl, r.hpp), (fdef, r.cpp)):
                nf = self.find_explicit(path, r, f)
                FT, NT = self.fz.tree.tokens(path), self.tree.tokens(path)
                if nf is None:
                    R.fail("V1c", f"{r.name}: its explicit {'definition' if f.is_def else 'declaration'} "
                                  f"is missing from {path}")
                    continue
                a = render(FT, decl_start(FT, f), decl_end(FT, f))
                b = render(NT, decl_start(NT, nf), decl_end(NT, nf))
                if r.name == lint.SOLVE and f.is_def and self.epoch:
                    a, b, _ = _solve_declared(a, b, R, replay=False)
                    self.tclaim(NT, decl_start(NT, nf), decl_end(NT, nf))
                    self.fclaim(path, FT.line(decl_start(FT, f)), FT.line(decl_end(FT, f) - 1))
                d = same_lines(a, b)
                n += 1
                if d is not None:
                    k, x, y = d
                    R.fail("V1c", f"{path}:{NT.line(decl_start(NT, nf)) }: {r.name}'s explicit "
                                  f"{'definition' if f.is_def else 'declaration'} differs from frozen at code "
                                  f"line {k}: frozen `{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")
        R.note("V1c", f"{n} explicit declarations and definitions compared with frozen")

    def find_explicit(self, path, r, f):
        idx = self.tree.index(path)
        if idx is None:
            return None
        cls = "Application" if r.mech == MEMBER else ""
        cands = [g for g in idx.funcs if g.name == r.name and (g.cls or "") == cls and g.params is not None
                 and g.is_def == f.is_def and not lint.is_epoch_form(g)]
        same = [g for g in cands if [p.kind for p in g.params] == [p.kind for p in f.params]]
        return (same or cands or [None])[0]

    def new_defs(self, name):
        return self.seamdefs.find(name, True)

    # ==== V1d: runNetwork =================================================================
    def v1d(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.rn.T, cand.rn.T
        if not self.epoch:
            a = render(fT, fz.rn.rn.body[0], fz.rn.rn.body[1] + 1)
            b = render(nT, cand.rn.rn.body[0], cand.rn.rn.body[1] + 1)
            d = same_lines(a, b)
            if d is not None:
                k, x, y = d
                R.fail("V1d", f"runNetwork differs from frozen at code line {k}: frozen "
                              f"`{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")
            R.note("V1d", f"empty conversion: runNetwork's {len(a)} code lines equal frozen")
            return
        F, N = fz.rn.statements(), cand.rn.statements()
        cut = set()
        for d in SUBST.spec_cut:
            for loc, hits in d.locate(fz):
                for h in hits:
                    for k, (s, e) in enumerate(F):
                        if h.line <= fT.line(s) <= h.end:
                            cut.add(k)
        removed_names = set(SUBST.callable) | set(SUBST.bracket) | set(SUBST.type_move) | \
            set(SUBST.field_timing) | {LAMBDA_NAME, "engines"}
        traces_k = next((k for k, (s, e) in enumerate(F) if fT.join(s, e) == join_text("NetworkRunTraces traces(log_dir_);")), None)
        cand_root = {x.name for x in cand.rn.scopes.root.decls if x.kind != "param"}
        n_unread = 0
        j = 0
        for i, (s, e) in enumerate(F):
            ds = parse_declaration(fT, s, e) or []
            names = [d.name for d in ds]
            if names and all(n in SUBST.unread_alias for n in names) and not cand_root & set(names):
                # UNREAD_ALIAS: the line is gone; what it was and that nothing names it now.
                self.v1d_unread_alias(fT, s, ds)
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                n_unread += 1
                continue
            if i in cut:
                # removed, `const T& name = engine_spec.<name>;`, or (an owned engine) its run
                # reference `T& name = run.<path>;`
                if j < len(N) and len(ds) == 1:
                    nd = parse_declaration(nT, *N[j]) or []
                    ok = len(nd) == 1 and nd[0].name == ds[0].name and nd[0].is_ref and nd[0].init and (
                        nT.join(*nd[0].init) == "engine_spec." + ds[0].name
                        or (ds[0].name in SUBST.owner and nT.join(*N[j]) == join_text(
                            f"{fT.join(*ds[0].type_span)}& {ds[0].name} = {SUBST.owner[ds[0].name]};")))
                    if ok:
                        self.tclaim(nT, N[j][0], N[j][1])
                        j += 1
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            if names and any(n in SUBST.owner for n in names):
                n0 = names[0]
                want = join_text(f"{fT.join(*ds[0].type_span)}& {n0} = {SUBST.owner[n0]};")
                if j < len(N) and nT.join(*N[j]) == want:
                    self.tclaim(nT, N[j][0], N[j][1])
                    j += 1
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            if names and any(n in removed_names for n in names) or fT.t(s) == "enum" and \
                    any(n in SUBST.type_move for n in names):
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            if fT.t(s) == "return" and any(c.name == "networkTeardown" for c in calls_in(fT, s, e)):
                # The frozen run end with each group the run now owns read through the run
                # (spec 2.2: NetworkRunEndInputs{ run.engines, run.emit, ... }), derived from the
                # frozen statement so a base without the lane layer is read as it is.
                parts = []
                for q in range(s, e):
                    t = fT.t(q)
                    if fT.k(q) == "id" and fT.t(q - 1) not in (".", "->", "::") and (
                            t == "engines" or t in SUBST.owner):
                        t = "run.engines" if t == "engines" else SUBST.owner[t]
                    parts.append(t)
                want = join_text(" ".join(parts))
                if j < len(N) and nT.join(*N[j]) == want:
                    self.tclaim(nT, N[j][0], N[j][1])
                    j += 1
                else:
                    R.fail("V1d", f"the run end is not `{want}`")
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            fsubs = [Sub(c.tok, c.open + 1, f"{lint.EPOCH_FN}(run, ") for c in calls_in(fT, s, e)
                     if c.name == LAMBDA_NAME]
            a = render(fT, s, e, fsubs)
            k = None
            for q in range(j, min(len(N), j + 6)):
                if same_lines(a, render(nT, *N[q])) is None:
                    k = q
                    break
            if k is None:
                R.fail("V1d", f"frozen {DRIVER_PATH}:{fT.line(s)} is not in runNetwork: "
                              f"`{fT.join(s, min(e, s + 12))}...`")
                continue
            for q in range(j, k):
                R.fail("V1d", f"{DRIVER_PATH}:{nT.line(N[q][0])}: an undeclared statement in runNetwork: "
                              f"`{nT.join(N[q][0], min(N[q][1], N[q][0] + 12))}...`")
            if fsubs:
                self.tclaim(nT, N[k][0], N[k][1])
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
            j = k + 1
            if i == traces_k:
                for want in ("const NetworkEngineSpec engine_spec = networkEngineSpec(config_, nc, ctx);",
                             "NetworkRun run(ctx, config_, nc, log, traces, setup, log_dir_, engine_spec);"):
                    if j < len(N) and nT.join(*N[j]) == join_text(want):
                        self.tclaim(nT, N[j][0], N[j][1])
                        j += 1
                    else:
                        R.fail("V1d", f"`{want}` does not follow `NetworkRunTraces traces(log_dir_);`")
        for q in range(j, len(N)):
            R.fail("V1d", f"{DRIVER_PATH}:{nT.line(N[q][0])}: an undeclared statement in runNetwork")
        R.note("V1d", f"{len(F)} frozen runNetwork statements against {len(N)}; {n_unread} UNREAD_ALIAS "
                      "lines gone")
        self.v1d_unread_names()
        self.v1d_moved()

    def v1d_unread_alias(self, fT, s, ds):
        """UNREAD_ALIAS (declared.py): the frozen line runNetwork lost declares one name, a
        reference or a const copy of a builtin, initialized from a member path (no call, no
        operator: the line had no effect but the binding); and no token of the candidate's
        runNetwork names it outside a nested declaration of its own, so no reader lost its
        object (V1d's statement match proves the rest of runNetwork unchanged)."""
        R = self.R
        where = f"{DRIVER_PATH}:{fT.line(s)}"
        if len(ds) != 1:
            R.fail("V1d", f"{where}: an UNREAD_ALIAS line declares {len(ds)} names")
            return
        d = ds[0]
        toks = [(fT.k(q), fT.t(q)) for q in range(*d.init)] if d.init else []
        path = (d.init_kind == "=" and len(toks) >= 3 and len(toks) % 2 == 1
                and all(k == "id" and t not in KEYWORDS for k, t in toks[0::2])
                and all(t == "." for _, t in toks[1::2]))
        if not path:
            R.fail("V1d", f"{where}: UNREAD_ALIAS {d.name}'s initializer `{fT.join(*d.init) if d.init else ''}` "
                          "is not a member path (its line may have had an effect)")
        words = set(fT.join(*d.type_span).replace("&", " ").split()) - {"const"}
        if not d.is_ref and not (d.is_const and not d.is_ptr and words and words <= BUILTIN_TYPES):
            R.fail("V1d", f"{where}: UNREAD_ALIAS {d.name} is `{fT.join(*d.type_span)}`, neither a reference "
                          "nor a const builtin copy")
        rn, nT, sc = self.cand.rn, self.cand.rn.T, self.cand.rn.scopes
        for q in range(rn.rn.body[0] + 1, rn.rn.body[1]):
            if nT.k(q) != "id" or nT.t(q) != d.name or nT.t(q - 1) in (".", "->", "::") \
                    or nT.t(q + 1) == "::":
                continue
            x = sc.lookup(d.name, q)
            if x is None or x.scope is sc.root:
                R.fail("V1d", f"{DRIVER_PATH}:{nT.line(q)}: runNetwork still names {d.name}, whose line "
                              "UNREAD_ALIAS removed")

    def v1d_unread_names(self):
        """No declaration of the candidate runNetwork's outermost scope is left unread (the
        dead-alias defect INC-12 and EP0 removed, which the move of the lambda would bring
        back; the review round of 2026-10-14)."""
        rn, nT, sc = self.cand.rn, self.cand.rn.T, self.cand.rn.scopes
        used = collections.Counter()
        for q in range(rn.rn.body[0] + 1, rn.rn.body[1]):
            if nT.k(q) != "id" or nT.t(q - 1) in (".", "->", "::"):
                continue
            x = sc.lookup(nT.t(q), q)
            if x is not None and x.scope is sc.root and x.tok != q:
                used[id(x)] += 1
        dead = [x for x in sc.root.decls if x.kind != "param" and not used[id(x)]]
        for x in dead:
            self.R.fail("V1d", f"{DRIVER_PATH}:{x.line}: runNetwork declares {x.name}, which nothing in it "
                               "reads (an alias whose readers moved out is UNREAD_ALIAS)")
        self.R.note("V1d", f"every one of runNetwork's {len(sc.root.decls)} outermost declarations is read"
                    if not dead else f"{len(dead)} unread outermost declarations")

    def v1d_moved(self):
        """Where the spec 4.1 edits put what they took out of runNetwork, each against frozen,
        each region claimed for V1e: the third-band predicate and the two bracket closures as
        functions (moved_closure), the timer scratch in NetworkRunTiming, the hand-off fields
        in NetworkPostFitHandoff (FIELD_MOVE), the engines' owner NetworkRunEngines,
        LoopSignal in Application (TYPE_MOVE), and the declared header hunk of step EP1a-1
        (the epoch function's declaration; the build list is V1e's cmake_build_list).  Only
        the lines a rule checks are claimed: every other line of a struct that existed at the
        base is left to V1e's line diff, so an edit there (a member's default, a stray field)
        fails."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        froot = {d.name: d for d in fz.rn.scopes.root.decls}
        for n in SUBST.callable + SUBST.bracket:
            self.moved_closure(n, "network" + n[0].upper() + n[1:], froot)
        self.timing_fields(froot)
        self.handoff_structs()
        self.engines_struct(froot)
        # TYPE_MOVE: LoopSignal is an Application member enum with the frozen enumerators.
        for name in SUBST.type_move:
            fd = froot.get(name) or next((d for d in fz.rn.lam_scope.parent.decls if d.name == name), None)
            AT = self.tree.tokens(APPLICATION_HPP)
            hit = next((j for j in range(AT.n - 3) if AT.t(j) == "enum" and AT.t(j + 1) == "class"
                        and AT.t(j + 2) == name), None)
            if hit is None:
                R.fail("V1d", f"{APPLICATION_HPP}: no `enum class {name}`")
                continue
            e = hit
            while AT.t(e) != ";":
                e += 1
            if fd is not None and AT.join(hit, e + 1) != fT.join(*fd.stmt):
                R.fail("V1d", f"{APPLICATION_HPP}:{AT.line(hit)}: {name} is `{AT.join(hit, e + 1)}`, frozen "
                              f"`{fT.join(*fd.stmt)}`")
            self.tclaim(AT, hit, e + 1)
        # The declared header hunks of step EP1a-1: the epoch function's one declaration, its
        # head the frozen lambda's (a converted member seam's declaration is V2's).
        AT = self.tree.tokens(APPLICATION_HPP)
        eds = [f for f in self.tree.index(APPLICATION_HPP).funcs if f.name == lint.EPOCH_FN]
        if self.epoch:
            if len(eds) != 1 or eds[0].is_def:
                R.fail("V1d", f"{APPLICATION_HPP}: {len(eds)} declarations of {lint.EPOCH_FN} "
                              f"({sum(1 for f in eds if f.is_def)} defining it in the class); one "
                              "declaration expected")
            for f in eds:
                self.epoch_head("V1d", f, AT)
                self.tclaim(AT, decl_start(AT, f), decl_end(AT, f))
        for path in (GROUPS_HPP, APPLICATION_HPP, RUN_HPP, lint.EPOCH_HPP):
            T = self.tree.tokens(path)
            if T is None:
                continue
            for j in range(T.n):
                if T.t(j) == "static_assert":
                    e2 = T.close(j + 1) + 1
                    if re.search(r"NetworkRun|NetworkEpoch", T.join(j, e2)):
                        self.tclaim(T, j, e2 + 1 if T.t(e2) == ";" else e2)
                elif T.t(j) == "friend" and T.t(j + 2) == "NetworkEpochContractProbe":
                    self.tclaim(T, j, j + 4 if T.t(j + 3) == ";" else j + 3)
        # The build list (CMakeLists.txt) is cmake_build_list's, run by V1e.

    def moved_closure(self, closure, fname, froot):
        """A driver closure that became a free function (CALLABLE, BRACKET): one definition,
        the frozen closure's parameters after the one new leading parameter (the knobs, or the
        run), its return type, and a body that is a binding preamble followed by the frozen
        body character-exact.  Each preamble declaration names a frozen capture of the
        closure, with the capture's declarator kind (a reference for a by-reference capture;
        `const auto` for a by-value capture of a non-mutable lambda) and an initializer that
        resolves to the captured object; or names a frozen driver object the closure body
        never mentions (a helper alias another binding reads through, such as `timing` in
        networkSubBegin), declared as a reference to that very object.  Every capture is
        bound; nothing else is declared, so no preamble line can shadow a name the body reads
        or copy a captured reference."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        fd = froot.get(closure)
        if fd is None or fd.lambda_scope is None:
            R.fail("V1d", f"the frozen closure {closure} was not found")
            return
        ls = fd.lambda_scope
        fbo, fbc = fT.match[ls.end], ls.end
        flam = _lambda_at(fT, fd.init[0], fd.init[1])
        fbody = render(fT, fbo + 1, fbc)
        defs = find_defs(self.tree, fname)
        if len(defs) != 1:
            R.fail("V1d", f"{fname}: {len(defs)} definitions (the {closure} closure's body moves into one)")
            return
        f = defs[0]
        T = self.tree.tokens(f.path)
        where = f"{f.path}:{f.line}"
        # The signature: one leading parameter, then the closure's own.
        if flam is not None and flam[1] is not None:
            fpo = flam[1]
            fpc = fT.close(fpo)
            want = fT.join(fpo + 1, fpc)
            got = T.join(f.params[1].span[0], f.params[-1].span[1]) if len(f.params) > 1 else ""
            if got != want:
                R.fail("V1d", f"{where}: {fname}'s parameters after the first are `{got}`, the frozen "
                              f"closure's `{want}`")
            lead = lint.ptype(f.params[0]) if f.params else ""
            want_lead = "NetworkRun&" if closure in SUBST.bracket else "const NetworkKnobs&"
            if lead != want_lead:
                R.fail("V1d", f"{where}: {fname}'s first parameter is `{lead}`, not `{want_lead}`")
            fret = fT.join(fpc + 1, fbo).replace("->", "", 1) if fT.t(fpc + 1) == "->" else "void"
            nret = T.join(decl_start(T, f), f.name_tok)
            if nret != fret:
                R.fail("V1d", f"{where}: {fname} returns `{nret}`, the frozen closure `{fret}`")
            # After the parameter list: the closure's specifiers but `mutable` (none at the
            # base), so no exception specification enters with the moved body.
            lh = lambda_head(fT, fd)
            if lh is not None and head_trail(T, f) != lh[1]:
                R.fail("V1d", f"{where}: {fname} has `{head_trail(T, f)}` after its parameter list, the "
                              f"frozen closure {'`' + lh[1] + '`' if lh[1] else 'nothing'}")
            for g in find_decls(self.tree, fname):
                GT = self.tree.tokens(g.path)
                if [lint.ptype(q) for q in g.params] != [lint.ptype(q) for q in f.params]:
                    R.fail("V1d", f"{g.path}:{g.line}: {fname}'s declaration differs from its definition")
                for msg in head_diff(GT, g, {"ret": f.ret, "defaults": head_defaults(f),
                                             "trail": head_trail(T, f)}):
                    R.fail("V1d", f"{g.path}:{g.line}: {fname}'s declaration {msg} (its definition's)")
                self.tclaim(GT, decl_start(GT, g), decl_end(GT, g))
        # The preamble: the leading declarations up to the first statement of the frozen body.
        st = statements(T, f.body[0] + 1, f.body[1])
        own = {x.name for a2, b2 in statements(fT, fbo + 1, fbc)
               for x in (parse_declaration(fT, a2, b2) or [])}
        k = 0
        while k < len(st):
            ds2 = parse_declaration(T, *st[k]) or []
            if not ds2 or any(x.name in own for x in ds2):
                break
            k += 1
        lo = st[k][0] if k < len(st) else f.body[1]
        nbody = render(T, lo, f.body[1])
        shift = (fbody[0][0] - nbody[0][0]) if fbody and nbody else 0
        d = same_lines(fbody, nbody, shift)
        if d is not None:
            R.fail("V1d", f"{where}: {fname}'s body differs from the frozen {closure} closure's at "
                          f"code line {d[0]}: frozen `{d[1][1] if d[1] else '<end>'}`, here "
                          f"`{d[2][1] if d[2] else '<end>'}`")
        caps, mutable = closure_captures(fz.rn, fd)
        body_names = {fT.t(j) for j in range(fbo + 1, fbc) if fT.k(j) == "id"}
        lead_param = f.params[0].name if f.params else ""
        pmap = {lead_param: Ref("E:run") if closure in SUBST.bracket else Ref("D:ctx.knobs")}
        sc = Scopes(T, f.body[0], f.body[1], f.params)
        res = Resolver(sc, {id(sc.root): "S"}, (), set(), params=pmap)
        bound = set()
        for q in range(k):
            ds2 = parse_declaration(T, *st[q]) or []
            if len(ds2) != 1:
                R.fail("V1d", f"{f.path}:{T.line(st[q][0])}: {fname}'s binding line declares {len(ds2)} names")
                continue
            x = ds2[0]
            xt = T.join(*x.type_span) + x.ops
            if x.init_kind != "=" or x.init is None:
                R.fail("V1d", f"{f.path}:{x.line}: {fname}'s binding of {x.name} is not `T {x.name} = ...;`")
                continue
            got = normalize(self.canon(res.resolve(*x.init)), self.const_roots)
            if not x.is_ref and not x.is_ptr:
                got = normalize(Copy(got) if isinstance(got, (Ref, Copy)) else got, self.const_roots)
            if x.name in caps:
                by_ref = caps[x.name]
                if by_ref:
                    allowed = self.declarators(x.name, froot.get(x.name)) | {"auto&", "constauto&"}
                    ok_kind = x.is_ref and xt.replace(" ", "") in {t.replace(" ", "") for t in allowed}
                    kind_txt = "a reference of the frozen type (or auto&)"
                else:
                    ok_kind = (not x.is_ref and not x.is_ptr
                               and xt == ("auto" if mutable else "const auto"))
                    kind_txt = "`auto`" if mutable else "`const auto` (a non-mutable lambda's capture)"
                if not ok_kind:
                    R.fail("V1d", f"{f.path}:{x.line}: {fname} binds the capture {x.name} as `{xt}`, not "
                                  f"{kind_txt}")
                want = normalize(frozen_canon(fz.res.name_ref(x.name, fbo + 1, ())), self.const_roots)
                if show(got) != show(want):
                    R.fail("V1d", f"{f.path}:{x.line}: {fname} binds {x.name} to {show(got)}; the closure "
                                  f"captured {show(want)}")
                bound.add(x.name)
                continue
            fdd = froot.get(x.name)
            if x.name in body_names or fdd is None or not x.is_ref:
                R.fail("V1d", f"{f.path}:{x.line}: {fname}'s preamble declares `{x.name}`, which is not a "
                              f"capture of the frozen {closure} closure"
                              + (" and is a name its body reads" if x.name in body_names else ""))
                continue
            want = normalize(frozen_canon(self.frozen_root("D:" + x.name)), self.const_roots)
            allowed = self.declarators(x.name, fdd) | {"auto&"}
            if show(got) != show(want) or xt.replace(" ", "") not in {t.replace(" ", "") for t in allowed}:
                R.fail("V1d", f"{f.path}:{x.line}: {fname}'s helper `{xt} {x.name}` binds {show(got)}; a "
                              f"helper is a reference to the frozen {x.name} ({show(want)})")
        missing = sorted(set(caps) - bound)
        if missing:
            R.fail("V1d", f"{where}: {fname} does not bind the closure's captures {missing}")
        self.tclaim(T, decl_start(T, f), f.body[1] + 1)

    def declarators(self, name, fd):
        """The declarator texts (type and operators, minimal spacing) a binding of the frozen
        driver name @p name may have in a preamble (check_preamble's rule)."""
        if fd is None:
            return set()
        ftype = self.fz.rn.T.join(*fd.type_span)
        if RUN_REFS.get(name) == "D:" + name:
            return {join_text(RUN_REF_TYPES[name])}
        if name in SUBST.owner or name in SUBST.field_timing:
            return {ftype + "&"}
        out = {ftype + fd.ops}
        if fd.ops.startswith("&") and not ftype.startswith("const"):
            out.add(join_text("const " + ftype) + fd.ops)
        return out

    def timing_fields(self, froot):
        """FIELD_MOVE: the bracket scratch in NetworkRunTiming, each with the frozen type and
        initializer; only their lines are claimed (the struct existed at the base)."""
        R, fT = self.R, self.fz.rn.T
        cp = class_parts(self.tree, GROUPS_HPP, "NetworkRunTiming")
        if cp is None:
            R.fail("V1d", f"{GROUPS_HPP}: struct NetworkRunTiming is missing")
            return
        for name, member in SUBST.field_timing.items():
            fd = froot.get(name)
            m = cp["members"].get(member)
            if fd is None or m is None:
                R.fail("V1d", f"NetworkRunTiming::{member} (the frozen {name}) is missing")
                continue
            ft = fT.join(*fd.type_span)
            init = fT.join(*fd.init) if fd.init else ""
            if m["type"] != ft or m["ops"] or m["init"].replace(" ", "") not in (
                    f"{ft}({init})".replace(" ", ""), "{" + init + "}", f"({init})"):
                R.fail("V1d", f"NetworkRunTiming::{member} is `{m['type']}{m['ops']} = {m['init']}`, frozen "
                              f"`{ft} {name}({init})`")
            self.tclaim(cp["T"], m["start"], m["end"])
        # Their order is the frozen declaration order (construction among themselves).
        order = [mem for _, mem in sorted((froot[n].tok, mem) for n, mem in SUBST.field_timing.items()
                                          if n in froot)]
        got = [mem for mem in cp["members"] if mem in order]
        if got != order:
            R.fail("V1d", f"NetworkRunTiming declares the moved fields {got}; their frozen order is {order}")

    def handoff_structs(self):
        """FIELD_MOVE: NetworkSolveGlueResult keeps every frozen member but the three hand-off
        fields, which NetworkPostFitHandoff holds with their frozen declarations, in frozen
        order and nothing else; the result's new `handoff` member is the only claimed line of
        the result (its other lines are V1e's), the new struct is claimed whole."""
        R = self.R
        fres = class_parts(self.fz.tree, SOLVE_HPP, "NetworkSolveGlueResult")
        nres = class_parts(self.tree, SOLVE_HPP, "NetworkSolveGlueResult")
        if fres is None or nres is None:
            R.fail("V1d", f"{SOLVE_HPP}: struct NetworkSolveGlueResult is missing")
            return
        groups = {}
        for name, (struct, newpath) in SUBST.field_solve.items():
            grp, _, fld = newpath.partition(".")
            groups.setdefault(grp, []).append((name, fld))
            if name in nres["members"]:
                R.fail("V1d", f"NetworkSolveGlueResult still has {name} (moved to {newpath})")
            fm = fres["members"].get(name)
            if fm is not None:
                self.fclaim(SOLVE_HPP, fm["first"], fm["last"])
        for grp, fields in groups.items():
            g = nres["members"].get(grp)
            if g is None:
                R.fail("V1d", f"NetworkSolveGlueResult has no member {grp}")
                continue
            if g["ops"] or g["init"] not in ("", "{}"):
                R.fail("V1d", f"NetworkSolveGlueResult::{grp} is `{g['type']}{g['ops']} = {g['init']}`, not a "
                              "default-constructed value")
            self.tclaim(nres["T"], g["start"], g["end"])
            hp = class_parts(self.tree, SOLVE_HPP, g["type"])
            if hp is None:
                R.fail("V1d", f"{SOLVE_HPP}: struct {g['type']} (NetworkSolveGlueResult::{grp}) is missing")
                continue
            order = sorted(fields, key=lambda x: fres["members"].get(x[0], {}).get("start", 0))
            if list(hp["members"]) != [fl for _, fl in order]:
                R.fail("V1d", f"{g['type']} has members {list(hp['members'])}, expected the moved fields "
                              f"{[fl for _, fl in order]} in frozen order")
            for name, fld in order:
                fm, hm = fres["members"].get(name), hp["members"].get(fld)
                if fm is None or hm is None:
                    continue
                if (fm["type"], fm["ops"], fm["init"]) != (hm["type"], hm["ops"], hm["init"]):
                    R.fail("V1d", f"{g['type']}::{fld} is `{hm['type']}{hm['ops']} {fld}{{{hm['init']}}}`, frozen "
                                  f"NetworkSolveGlueResult::{name} `{fm['type']}{fm['ops']} {name}{{{fm['init']}}}`")
            if hp["funcs"] or hp["others"]:
                R.fail("V1d", f"{g['type']} declares more than its fields: "
                              f"{[x['text'][:50] for x in hp['funcs']] + [o[2][:50] for o in hp['others']]}")
            if hp["base"]:
                R.fail("V1d", f"{g['type']} is declared `{hp['base']}` (a base clause)")
            HT = hp["T"]
            self.tclaim(HT, hp["head"], hp["end"])

    def engines_struct(self, froot):
        """OWNER_MOVE: NetworkRunEngines owns the engines.  Its members are the engines the
        OWNER_MOVE entries place under run.engines, in the frozen construction order (so
        construction and destruction order among them are the frozen ones), each of its frozen
        declaration's type with no default member initializer; its one constructor takes the
        NetworkEngineSpec, has an empty body, and initializes exactly the engines the frozen
        code constructed with arguments (whose arguments V12 checks) plus, with empty
        parentheses, any of the default-constructed ones; every other member function is
        deleted.  The frozen struct (a const view) is replaced whole."""
        R, fT = self.R, self.fz.rn.T
        fsp = struct_span(self.fz.tree, GROUPS_HPP, "NetworkRunEngines")
        if fsp is not None:
            self.fclaim(GROUPS_HPP, *fsp)
        cp = class_parts(self.tree, GROUPS_HPP, "NetworkRunEngines")
        if cp is None:
            R.fail("V1d", f"{GROUPS_HPP}: struct NetworkRunEngines is missing")
            return
        T = cp["T"]
        eng = [(froot[n].tok, p.split(".", 2)[2], froot[n]) for n, p in SUBST.owner.items()
               if p.startswith("run.engines.") and n in froot]
        eng.sort(key=lambda x: x[0])
        want = [m for _, m, _ in eng]
        if list(cp["members"]) != want:
            R.fail("V1d", f"NetworkRunEngines members {list(cp['members'])}; the frozen construction order "
                          f"is {want}")
        for _, m, d in eng:
            x = cp["members"].get(m)
            if x is None:
                continue
            ft = fT.join(*d.type_span)
            if x["type"] != ft or x["ops"] or x["init"]:
                R.fail("V1d", f"NetworkRunEngines::{m} is `{x['type']}{x['ops']} {m}{x['init']}`, frozen "
                              f"`{ft} {d.name}` (no default member initializer)")
        if cp["others"]:
            R.fail("V1d", f"NetworkRunEngines declares {[o[2][:50] for o in cp['others']]}")
        if cp["base"]:
            R.fail("V1d", f"NetworkRunEngines is declared `{cp['base']}` (a base clause)")
        ctors = [f for f in cp["funcs"] if not f["deleted"]]
        if len(ctors) != 1 or ctors[0]["name"] != "NetworkRunEngines":
            R.fail("V1d", f"NetworkRunEngines: member functions {[f['name'] for f in ctors]} besides the "
                          "deleted ones; one constructor expected")
        else:
            c = ctors[0]
            if [lint.ptype(q) for q in c["params"]] != ["const NetworkEngineSpec&"]:
                R.fail("V1d", "NetworkRunEngines' constructor does not take one `const NetworkEngineSpec&`")
            if c["body"] is None or c["body"][1] != c["body"][0] + 1:
                R.fail("V1d", "NetworkRunEngines' constructor body is not empty")
            if member_trail(T, c):
                R.fail("V1d", f"NetworkRunEngines' constructor has `{member_trail(T, c)}` after its "
                              "parameter list (nothing is declared there, as at the frozen construction)")
            args_of = {m: d for _, m, d in eng}
            names = [i[0] for i in c["inits"]]
            if len(set(names)) != len(names) or not set(names) <= set(args_of):
                R.fail("V1d", f"NetworkRunEngines' constructor initializes {names}")
            for m, d in args_of.items():
                init = next((i for i in c["inits"] if i[0] == m), None)
                if d.init_kind in ("(", "{") and init is None:
                    R.fail("V1d", f"NetworkRunEngines' constructor does not construct {m} (frozen "
                                  f"`{fT.join(*d.stmt)[:60]}`)")
                if d.init is None and init is not None and init[2]:
                    R.fail("V1d", f"NetworkRunEngines' constructor gives the default-constructed {m} the "
                                  f"arguments `{init[3]}`")
        self.tclaim(T, cp["head"], cp["end"])

    # ==== V12: the spec cut ===============================================================
    def cut_statements(self, fzlike):
        """[(entry, [(s, e)])]: the runNetwork statements each SPEC_CUT entry locates."""
        drv = fzlike.drv
        T = drv.T
        stmts = [x for x in drv.driver_statements() if x[0] < drv.lam_decl.tok]
        out = []
        for d in SUBST.spec_cut:
            picked = []
            for loc, hits in d.locate(fzlike):
                for h in hits:
                    picked += [(s, e) for (s, e) in stmts if h.line <= T.line(s) <= h.end
                               and T.line(e - 1) <= h.end]
            out.append((d, sorted(set(picked))))
        return out

    def v12(self):
        R, fz = self.R, self.fz
        fT = fz.drv.T
        fcut = self.cut_statements(fz)
        n = sum(len(x) for _, x in fcut)
        if not self.epoch:
            cfz = CandFz(self.tree)
            nT = cfz.drv.T
            ncut = self.cut_statements(cfz)
            fst = [x for x in fz.drv.driver_statements() if x[0] < fz.drv.lam_decl.tok]
            nst = [x for x in cfz.drv.driver_statements() if x[0] < cfz.drv.lam_decl.tok]
            for (d, fs), (_, ns) in zip(fcut, ncut):
                if len(fs) != len(ns):
                    R.fail("V12", f"{d.name}: {len(ns)} cut statements here, {len(fs)} frozen")
                for (a, b), (c, e) in zip(fs, ns):
                    dd = same_lines(render(fT, a, b), render(nT, c, e))
                    if dd is not None:
                        R.fail("V12", f"{d.name}: {DRIVER_PATH}:{nT.line(c)} differs from frozen "
                                      f"{fT.line(a)}: `{dd[2][1] if dd[2] else '<end>'}`")
                # The range: no statement other than the frozen ones between its first and last.
                if fs and ns:
                    f_in = [x for x in fst if fs[0][0] <= x[0] <= fs[-1][0]]
                    n_in = [x for x in nst if ns[0][0] <= x[0] <= ns[-1][0]]
                    ftxt = [fT.join(*x) for x in f_in]
                    for x in n_in:
                        if nT.join(*x) not in ftxt:
                            R.fail("V12", f"{d.name}: {DRIVER_PATH}:{nT.line(x[0])} entered the cut range: "
                                          f"`{nT.join(x[0], min(x[1], x[0] + 10))}...`")
                res = Resolver(cfz.drv.scopes, {id(cfz.drv.scopes.root): "D"}, (),
                               RunNetwork(self.tree).members)
                locs = {x.name for d2, sts in ncut for a2, b2 in sts
                        for x in (parse_declaration(nT, a2, b2) or [])}
                for s, e in ns:
                    self.spec_tokens(nT, s, e, res, f"{d.name}", locs)
            R.note("V12", f"empty conversion: {n} cut statements in place, character-exact, and their "
                          "reads checked")
            return
        self.v12_epoch(fcut)

    def spec_tokens(self, T, s, e, res, what, locals_=()):
        """No log / traces / setup read; calls only from the allowlist; every name read is the
        configuration, nc, the knobs, a local the cut declares (@p locals_) or a frozen alias
        of one of those (V12)."""
        R = self.R
        for j in ids_in(T, s, e):
            if T.t(j) in ("log", "traces", "setup", "log_manager_") and T.t(j + 1) != "(":
                R.fail("V12", f"{what}: {T.path}:{T.line(j)} reads `{T.t(j)}`")
        if res is not None:
            for j in ids_in(T, s, e):
                if T.t(j + 1) in ("(", "::") or T.t(j) in locals_:
                    continue
                end, _ = lint._chain_end(T, j, e)
                node = res.resolve(j, end)
                inner = node.inner if isinstance(node, Copy) else node
                if not isinstance(inner, Ref):
                    continue            # a type, an enumerator, a function: not storage
                r = inner.render()
                ok = r.startswith(("A:config_", "D:ctx.knobs", "D:nc")) or \
                    any(r == "D:" + n or r.startswith("D:" + n + ".") for n in locals_)
                if r.startswith("D:") and not ok:
                    R.fail("V12", f"{what}: {T.path}:{T.line(j)} reads `{T.t(j)}` = {r}, not the "
                                  "configuration, nc, the knobs or a local of the cut")
        decl_names = {d.name for d in (parse_declaration(T, s, e) or [])}
        for c in calls_in(T, s, e):
            if c.name in decl_names:
                continue
            if not SPEC_CALLS.match(c.name):
                R.fail("V12", f"{what}: {T.path}:{c.line} calls {c.name}, not in the allowlist")

    def v12_epoch(self, fcut):
        """networkEngineSpec against the frozen cut (spec 2.5, 4.1): every frozen cut statement
        is in it, in frozen order, character-exact, except a built local's declaration, which
        becomes a reference to its own field (`T& name = spec.name;`) or its own field's
        initializer (`spec.name = <the frozen initializer>;`), and an engine construction, whose
        arguments become field initializers (checked by engine_args).  Every other statement of
        the function is `NetworkEngineSpec spec;` (first), `return spec;` (last), the one
        initializer of a field that holds an engine argument, its right side that frozen
        argument, or a frozen driver declaration repeated with its frozen text.  A field is
        assigned at most once, and a field a reference binding fills is never assigned whole,
        so no statement can re-point one field at another's value or reset it."""
        R, fz = self.R, self.fz
        fT = fz.drv.T
        idx = self.tree.index(RUN_CPP)
        defs = [g for g in (idx.funcs if idx else []) if g.name == "networkEngineSpec" and g.is_def]
        if len(defs) != 1:
            R.fail("V12", f"{RUN_CPP}: {len(defs)} definitions of networkEngineSpec (one expected)")
            if not defs:
                return
        f = defs[0]
        T = self.tree.tokens(RUN_CPP)
        # The head is the specification's (ENGINE_SPEC_HEAD): the spec by value, the
        # configuration, the network block and the context by reference to const, nothing
        # after the list (V9 holds the declaration to this definition).
        for msg in head_diff(T, f, {"ret": ENGINE_SPEC_HEAD[0], "text": join_text(ENGINE_SPEC_HEAD[1]),
                                    "trail": ""}):
            R.fail("V12", f"{RUN_CPP}:{f.line}: networkEngineSpec {msg} (spec 2.5)")
        self.tclaim(T, decl_start(T, f), f.body[1] + 1)
        body = statements(T, f.body[0] + 1, f.body[1])
        texts = [render(T, s, e) for s, e in body]
        used = [None] * len(body)
        frozen = sorted(((a, b, d) for d, sts in fcut for a, b in sts), key=lambda x: x[0])
        cursor = 0

        def verbatim(want, k):
            got = texts[k]
            if len(got) != len(want) or not got:
                return False
            shift = want[0][0] - got[0][0]
            return same_lines(want, got, shift) is None

        for a, b, d in frozen:
            ds = parse_declaration(fT, a, b) or []
            want = render(fT, a, b)
            k = next((k for k in range(cursor, len(body)) if used[k] is None and verbatim(want, k)), None)
            if k is not None:
                used[k], cursor = "cut", k + 1
                continue
            if ds and any(x.name in SUBST.owner for x in ds):
                continue          # an engine construction: engine_args checks its arguments
            if len(ds) == 1:
                nm, ftype = ds[0].name, fT.join(*ds[0].type_span)
                k = next((k for k in range(cursor, len(body)) if used[k] is None
                          and self._binds_field(T, *body[k], nm, ftype)), None)
                if k is not None:
                    used[k], cursor = "binding", k + 1
                    continue
                if ds[0].init and ds[0].init_kind == "=":
                    rhs = fT.join(*ds[0].init)
                    k = next((k for k in range(cursor, len(body)) if used[k] is None
                              and T.t(body[k][0]) == "spec" and T.t(body[k][0] + 1) == "."
                              and T.t(body[k][0] + 2) == nm and T.t(body[k][0] + 3) == "="
                              and T.join(body[k][0] + 4, body[k][1] - 1) == rhs), None)
                    if k is not None:
                        used[k], cursor = "initializer", k + 1
                        continue
            R.fail("V12", f"{d.name}: frozen {DRIVER_PATH}:{fT.line(a)} is not in networkEngineSpec "
                          f"in frozen order: `{fT.join(a, min(b, a + 12))}...`")
        fields = self.engine_args(body, T)
        drv = {x.name: x for x in fz.rn.scopes.root.decls}
        # The frame: `NetworkEngineSpec spec;` first, `return spec;` last, each once.
        opening, closing = join_text("NetworkEngineSpec spec;"), join_text("return spec;")
        btxt = [T.join(s, e) for s, e in body]
        if not body or btxt[0] != opening or btxt.count(opening) != 1:
            R.fail("V12", f"{RUN_CPP}: networkEngineSpec does not open with its one `NetworkEngineSpec spec;`")
        if not body or btxt[-1] != closing or btxt.count(closing) != 1:
            R.fail("V12", f"{RUN_CPP}: networkEngineSpec does not end with its one `return spec;`")
        # Each field is assigned whole at most once; a field a reference binding fills
        # (`T& name = spec.name;`) is never assigned whole.
        assigned = collections.Counter(T.t(s + 2) for s, e in body
                                       if T.t(s) == "spec" and T.t(s + 1) == "." and T.t(s + 3) == "=")
        for fld, cnt in sorted(assigned.items()):
            if cnt > 1:
                R.fail("V12", f"{RUN_CPP}: networkEngineSpec assigns spec.{fld} {cnt} times (once at most)")
            if fields.get(fld, ("",))[0] == "local":
                R.fail("V12", f"{RUN_CPP}: networkEngineSpec assigns spec.{fld} whole; the setup local "
                              f"{fld} fills that field through its reference binding only")
        for k, (s, e) in enumerate(body):
            if used[k] is not None:
                continue
            txt = T.join(s, e)
            if txt in (opening, closing):
                continue
            if T.t(s) == "spec" and T.t(s + 1) == "." and T.t(s + 3) == "=":
                fld = T.t(s + 2)
                kind, want = fields.get(fld, (None, None))
                if kind == "arg" and T.join(s + 4, e - 1) == want:
                    continue
                R.fail("V12", f"{RUN_CPP}:{T.line(s)}: `{T.join(s, min(e, s + 14))}...` assigns spec.{fld}"
                              + (f", which holds the frozen engine argument `{want}`, another value"
                                 if kind == "arg" else ", which no engine argument accounts for"
                                 if kind is None else ", a field its reference binding fills"))
                continue
            ds = parse_declaration(T, s, e) or []
            if len(ds) == 1 and ds[0].name in drv:
                fd = drv[ds[0].name]
                if same_lines(render(fT, *fd.stmt), texts[k],
                              render(fT, *fd.stmt)[0][0] - texts[k][0][0]) is None:
                    continue
            R.fail("V12", f"{RUN_CPP}:{T.line(s)}: networkEngineSpec holds a statement that is neither "
                          f"a frozen cut statement nor a declared form: `{T.join(s, min(e, s + 12))}...`")
        for s, e in body:
            self.spec_tokens(T, s, e, None, "networkEngineSpec")
        nf = self.spec_struct(body, T)
        R.note("V12", f"networkEngineSpec: {len(body)} statements against {len(frozen)} frozen cut "
                      f"statements; {len(fields)} engine arguments are spec fields; NetworkEngineSpec's "
                      f"{nf} field types checked")

    def spec_struct(self, body, ST):
        """NetworkEngineSpec (V12): data members only, and each field's type is the type of
        what it holds, so no argument is narrowed or converted on its way to its engine: a
        field that replaces a setup local (`T& name = spec.name;` or `spec.name = <its frozen
        initializer>;`) has the local's type without const; a field that holds one engine
        argument (`spec.F = <the frozen argument>;`) has that expression's type (the T of a
        `static_cast<T>`, a frozen local's declared type, a knob's declared type, the type a
        conditional's `T{}` branch constructs).  A field nothing assigns would reach its engine
        as its default; a built local's field has no default initializer, so it starts as the
        default-constructed local did."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        cp = class_parts(self.tree, GROUPS_HPP, "NetworkEngineSpec")
        if cp is None:
            R.fail("V12", f"{GROUPS_HPP}: struct NetworkEngineSpec is missing")
            return 0
        if cp["funcs"] or cp["others"]:
            R.fail("V12", f"NetworkEngineSpec declares more than fields: "
                          f"{[f['text'][:40] for f in cp['funcs']] + [o[2][:40] for o in cp['others']]}")
        if cp["base"]:
            R.fail("V12", f"NetworkEngineSpec is declared `{cp['base']}` (a base clause)")
        expr, bound = {}, {}
        for s, e in body:
            if ST.t(s) == "spec" and ST.t(s + 1) == "." and ST.t(s + 3) == "=":
                expr[ST.t(s + 2)] = (s + 4, e - 1)
            for d in parse_declaration(ST, s, e) or []:
                if d.is_ref and d.init and ST.t(d.init[0]) == "spec" and ST.t(d.init[0] + 1) == ".":
                    if ST.t(d.init[0] + 2) != d.name:
                        R.fail("V12", f"{RUN_CPP}:{ST.line(s)}: the setup local {d.name} is bound to "
                                      f"spec.{ST.t(d.init[0] + 2)}; a replaced local's field is its own "
                                      f"name (spec 2.2), spec.{d.name}")
                        continue
                    bound[ST.t(d.init[0] + 2)] = d.name
        froot = {d.name: d for d in fz.rn.scopes.root.decls}
        knobs = struct_members(fz.tree, "NetworkKnobs") or {}
        n = 0
        for name, m in cp["members"].items():
            n += 1
            got = m["type"] + m["ops"]
            if name in bound or (name in froot and name in expr
                                 and ST.join(*expr[name]) == fT.join(*(froot[name].init or (0, 0)))):
                local = froot.get(bound.get(name, name))
                if local is None:
                    R.fail("V12", f"NetworkEngineSpec::{name}: no frozen local {bound.get(name, name)}")
                    continue
                want = fT.join(*local.type_span)
                want = want[len("const "):] if want.startswith("const ") else want
                if got != want:
                    R.fail("V12", f"NetworkEngineSpec::{name} is `{got}`; the frozen local "
                                  f"{local.name} is `{fT.join(*local.type_span)}{local.ops}`")
                if name in bound and m["init"] not in ("", "{}"):
                    R.fail("V12", f"NetworkEngineSpec::{name} has the default `{m['init']}`; the frozen local "
                                  f"{local.name} was default-constructed")
                continue
            if name not in expr:
                R.fail("V12", f"NetworkEngineSpec::{name}: networkEngineSpec never assigns it")
                continue
            want, why = _expr_type(ST, *expr[name], froot, fT, knobs)
            if want is None:
                R.fail("V12", f"NetworkEngineSpec::{name}: the type of `{ST.join(*expr[name])}` is not "
                              f"derivable ({why})")
            elif join_text(got) != join_text(want):
                R.fail("V12", f"NetworkEngineSpec::{name} is `{got}`; it holds `{ST.join(*expr[name])}`, "
                              f"of type `{want}` ({why})")
        T = cp["T"]
        self.tclaim(T, cp["head"], cp["end"])
        return n

    @staticmethod
    def _binds_field(T, s, e, name, ftype):
        """`<the local's type>& name = spec.name;`: the frozen setup local @p name re-pointed at
        its own field (spec 2.2: each field is the local it replaced, with its name), so two
        locals cannot trade fields."""
        ds = parse_declaration(T, s, e) or []
        if len(ds) != 1 or ds[0].name != name or not ds[0].is_ref or not ds[0].init:
            return False
        a, b = ds[0].init
        if not (T.t(a) == "spec" and T.t(a + 1) == "." and T.t(a + 2) == name and b == a + 3):
            return False
        nt = T.join(*ds[0].type_span)
        return nt in (ftype, ftype.replace("const ", "", 1), "auto")

    def engine_args(self, body, ST):
        """Each frozen engine construction's arguments (the OWNER_MOVE declarations that
        construct with arguments) against its new constructor's, position by position: equal
        text, or a spec field whose initializer in networkEngineSpec is the frozen expression,
        or the frozen expression with each built local replaced by its field.  A built local
        stands for its own field only (`T& name = spec.name;`, or `spec.name = <its frozen
        initializer>;`).  Returns {field: (kind, frozen text)} of the fields the comparison
        accounted for: kind `arg` for a field whose initializer is the frozen argument (the text),
        `local` for a field a built local fills through its reference binding."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        field_expr, local_field, by_ref = {}, {}, set()
        for s, e in body:
            if ST.t(s) == "spec" and ST.t(s + 1) == "." and ST.t(s + 3) == "=":
                field_expr.setdefault(ST.t(s + 2), ST.join(s + 4, e - 1))
            for d in parse_declaration(ST, s, e) or []:
                if d.is_ref and d.init and ST.t(d.init[0]) == "spec" and ST.t(d.init[0] + 1) == "." \
                        and ST.t(d.init[0] + 2) == d.name:
                    local_field[d.name] = d.name
                    by_ref.add(d.name)
        for d in fz.rn.scopes.root.decls:      # a frozen built local whose initializer is its field's
            if d.init and d.init_kind == "=" and d.name not in local_field:
                if field_expr.get(d.name) == fT.join(*d.init):
                    local_field[d.name] = d.name
        news = {}
        for path, cls in ((GROUPS_HPP, "NetworkRunEngines"), (RUN_HPP, "NetworkRun")):
            idx = self.tree.index(path)
            T = self.tree.tokens(path)
            ctor = next((g for g in (idx.funcs if idx else []) if g.name == cls and g.cls == cls
                         and g.is_def and g.params and lint.ptype(g.params[-1]) == "const NetworkEngineSpec&"),
                        None)
            if ctor is None:
                R.fail("V12", f"{path}: no {cls} constructor taking the NetworkEngineSpec")
                continue
            for m, args in _mem_init_args(T, ctor).items():
                news[(cls, m)] = (T, args, ctor.params[-1].name)
        used = {}
        n = 0

        def local_kind(fld):
            return ("local", None) if fld in by_ref else ("init", field_expr.get(fld))

        for name, rpath in SUBST.owner.items():
            fd = next((x for x in fz.rn.scopes.root.decls if x.name == name), None)
            if fd is None or not fd.init or fd.init_kind not in ("(", "{"):
                continue
            parts = rpath.split(".")
            key = ("NetworkRunEngines", parts[2]) if parts[1] == "engines" else ("NetworkRun", parts[1])
            if key not in news:
                R.fail("V12", f"{name}: {key[0]} does not construct {key[1]}")
                continue
            T, args, sname = news[key]
            fargs = _split_top(fT, *fd.init)
            if len(fargs) != len(args):
                R.fail("V12", f"{name}: {len(fargs)} frozen constructor arguments, {len(args)} in "
                              f"{key[0]}::{key[1]}")
                continue
            for k, ((a, b), (c, e)) in enumerate(zip(fargs, args)):
                n += 1
                fa, na = fT.join(a, b), T.join(c, e)
                na = re.sub(r"\bspec\.", sname + ".", na)
                if na == fa:
                    continue
                m = re.fullmatch(re.escape(sname) + r"\.(\w+)", na)
                if m and field_expr.get(m.group(1)) == fa:
                    used[m.group(1)] = local_kind(m.group(1)) if m.group(1) in local_field else ("arg", fa)
                    continue
                if m and local_field.get(fa) == m.group(1):
                    used[m.group(1)] = local_kind(m.group(1))
                    continue
                sub = fa
                for loc, fld in local_field.items():
                    sub = re.sub(r"(?<![\w.])" + re.escape(loc) + r"\b", f"{sname}.{fld}", sub)
                if sub == na:
                    for x in local_field:
                        if re.search(r"\b" + x + r"\b", fa):
                            used[local_field[x]] = local_kind(local_field[x])
                    continue
                R.fail("V12", f"{name}: argument {k} is `{T.join(c, e)}` in {key[0]}::{key[1]}, frozen "
                              f"`{fa}`, and no field of networkEngineSpec holds it")
        return used

    # ==== V13: names ======================================================================
    def v13(self):
        R = self.R
        n = 0
        for r in self.rows:
            if isinstance(r.mech, Explicit):
                continue
            fdecl, fdef = self.fdecls.get(r.name, (None, None))
            if fdef is None:
                continue
            FT = self.fz.tree.tokens(r.cpp)
            need = _preflight(FT, fdef.body[0] + 1, fdef.body[1], set(r.outs) & set(GROUP_TYPES))
            if r.name in self.converted and r.mech != ADAPTER:
                for f in self.new_defs(r.name):
                    n += 1
                    names = [p.name for p in f.params]
                    slots = [("run", 0)] + ([("ep", 1)] if r.mech != RUN else [])
                    k0 = 2 + len(r.kept)
                    if not lint.is_card_form(f):
                        slots += [(o, k0 + q) for q, o in enumerate(r.outs) if o in GROUP_TYPES]
                    for base, pos in slots:
                        got = names[pos] if pos < len(names) else None
                        fallback = "n" + base if base in ("run", "ep") else base + "_out"
                        if base in need:
                            ok = got == fallback
                        elif need:
                            ok = got in (base, fallback)
                        else:
                            ok = got == base
                        if not ok:
                            R.fail("V13", f"{f.path}:{f.line}: {r.name}'s parameter {pos} is `{got}`; the "
                                          f"pre-flight requires `{fallback if base in need else base}` "
                                          f"(the frozen body uses {sorted(need) or 'none of run, ep, the outputs'})")
            # Every definition of the seam in the tree: a parameter named run / ep / nrun /
            # nep that the frozen definition did not have, or that collides with a name the
            # body declares, fails.
            frozen_names = {p.name for p in (fdef.params or [])}
            idx = self.tree.index(r.cpp)
            for f in (idx.funcs if idx else []):
                if f.name != r.name or not f.is_def or f.params is None:
                    continue
                if lint.is_epoch_form(f) and r.name in self.converted:
                    continue
                n += 1
                NT = self.tree.tokens(r.cpp)
                body_names = _preflight(NT, f.body[0] + 1, f.body[1], set(), declared_only=True)
                for p in f.params:
                    if p.name in ("run", "ep", "nrun", "nep") and p.name not in frozen_names:
                        why = (f"; the body declares `{p.name}` too" if p.name in body_names else "")
                        R.fail("V13", f"{r.cpp}:{f.line}: {r.name} has a parameter named `{p.name}` "
                                      f"(not a converted epoch form){why}")
        fb = sorted(r.name for r in self.rows if not isinstance(r.mech, Explicit) and r.mech != ADAPTER
                    and self.fdecls.get(r.name, (None, None))[1] is not None
                    and _preflight(self.fz.tree.tokens(r.cpp), self.fdecls[r.name][1].body[0] + 1,
                                   self.fdecls[r.name][1].body[1], set(r.outs) & set(GROUP_TYPES)))
        R.note("V13", f"{n} definitions checked; the frozen pre-flight names {len(fb)} fallback seams: "
                      + ", ".join(fb))

    # ==== V2: signatures ==================================================================
    def v2(self):
        R = self.R
        if not self.converted:
            for r in self.rows:
                if isinstance(r.mech, Explicit):
                    continue
                if self.new_defs(r.name):
                    R.fail("V2", f"{r.name} has an epoch-form definition but its rows are not converted "
                                 "(--rows)")
            if not R.problems["V2"]:
                R.na("V2", "no seam converted (--rows names none)")
            return
        for name in sorted(self.converted):
            r = self.by_name[name]
            fdecl, fdef = self.fdecls[name]
            defs = self.new_defs(name)
            hidx = self.tree.index(r.hpp)
            decls = [f for f in (hidx.funcs if hidx else []) if f.name == name and not f.is_def
                     and f.params is not None and lint.is_epoch_form(f)]
            if len(defs) != 1 or len(decls) != 1:
                R.fail("V2", f"{name}: {len(decls)} epoch-form declarations in {r.hpp}, {len(defs)} "
                             "definitions (one each expected)")
                continue
            want, presolve = self.epoch_signature(r)
            fparams = {p.name: p for p in fdecl.params}
            DT0, HT0 = self.tree.tokens(defs[0].path), self.tree.tokens(decls[0].path)
            hd = doc_before(HT0, decl_start(HT0, decls[0]))
            self.tclaim(HT0, decl_start(HT0, decls[0]), decl_end(HT0, decls[0]))
            self.tclaim(DT0, decl_start(DT0, defs[0]), defs[0].body[0] + 1)
            if r.mech != ADAPTER and fdecl is not None:
                FH = self.fz.tree.tokens(r.hpp)
                self.fclaim(r.hpp, FH.line(decl_start(FH, fdecl)), FH.line(decl_end(FH, fdecl) - 1))
            for f in (decls[0], defs[0]):
                got = [lint.ptype(p) for p in f.params]
                if got != want:
                    R.fail("V2", f"{f.path}:{f.line}: {name}({', '.join(got)}); expected ({', '.join(want)})")
            for kp in r.kept:
                p = next((q for q in decls[0].params if q.name == kp), None)
                if p is None or join_text(p.text) != join_text(fparams[kp].text):
                    R.fail("V2", f"{name}: kept parameter {kp} does not keep its frozen declaration")
            # The head around the parameter list is the frozen form's (an adapter's epoch form
            # that of the explicit form it forwards to): its return type and every specifier
            # before the name, every specifier after the list, and no default argument but a
            # kept parameter's frozen one.
            heads = ((decls[0], HT0, fdecl, r.hpp, "declaration"), (defs[0], DT0, fdef, r.cpp, "definition"))
            for f, NT, ff, fpath, what in heads:
                if ff is None:
                    R.fail("V2", f"{name}: no frozen {what} to compare its epoch form's head with")
                    continue
                FT = self.fz.tree.tokens(fpath)
                fdefaults = dict(zip([p.name for p in ff.params], head_defaults(ff)))
                wd = [fdefaults.get(p.name) if p.name in r.kept else None for p in f.params]
                for msg in head_diff(NT, f, {"ret": ff.ret, "defaults": wd, "trail": head_trail(FT, ff)}):
                    R.fail("V2", f"{f.path}:{f.line}: {name}'s epoch-form {what} {msg} (the frozen "
                                 f"{'explicit form' if r.mech == ADAPTER else what}'s)")
            if not presolve:
                bad = [o for o in r.outs if o not in ("handoff", "sol")]
                if bad:
                    R.fail("V2", f"{name}: after the solve it writes {bad}")
        R.note("V2", f"{len(self.converted)} converted seams' signatures and heads checked")

    def epoch_signature(self, r):
        """(parameter types, pre-solve) of row @p r's epoch form: (NetworkRun&[, const
        NetworkEpoch&], kept..., outputs...) as its row says, the card's NetworkEpoch& and no
        output under --presolve-writer epoch for a writer before the solve; a kept or a
        single-member output parameter keeps its frozen type (V2, and the contract test's
        aliases in V8)."""
        solve_k = next((k for k, c in enumerate(self.fz.calls) if c.name == lint.SOLVE), None)
        fdecl = self.fdecls[r.name][0]
        sites = [k for k, c in enumerate(self.fz.calls) if c.name == r.name]
        presolve = solve_k is None or not sites or sites[0] <= solve_k
        card_form = self.presolve_writer == "epoch" and r.outs and r.mech != RUN and presolve
        want = ["NetworkRun&"] if r.mech == RUN else ["NetworkRun&", "const NetworkEpoch&"]
        if card_form:
            want = ["NetworkRun&", "NetworkEpoch&"]
        fparams = {p.name: p for p in (fdecl.params if fdecl is not None else [])}
        for kp in r.kept:
            p = fparams.get(kp)
            want.append(lint.ptype(p) if p else "?")
        if not card_form:
            for o in r.outs:
                if o in GROUP_TYPES:
                    want.append(GROUP_TYPES[o] + "&")
                else:
                    p = fparams.get(o)
                    want.append(lint.ptype(p) if p else "?")
        return want, presolve

    # ==== V1b: converted bodies ===========================================================
    def v1b(self):
        R = self.R
        if not self.converted:
            R.na("V1b", "no seam converted")
            return
        n = 0
        for name in sorted(self.converted):
            r = self.by_name[name]
            if r.mech == ADAPTER:
                for f in self.new_defs(name):
                    NT = self.tree.tokens(f.path)
                    st = statements(NT, f.body[0] + 1, f.body[1])
                    self.tclaim(NT, decl_start(NT, f), f.body[1] + 1)
                    if len(st) != 1 or not self.forwarding_call(NT, *st[0], name):
                        R.fail("V1b", f"{f.path}:{f.line}: {name}'s epoch form is not one forwarding call: "
                                      f"`{NT.join(f.body[0] + 1, min(f.body[1], f.body[0] + 30))}`")
                continue
            fdecl, fdef = self.fdecls[name]
            FT = self.fz.tree.tokens(r.cpp)
            for f in self.new_defs(name):
                NT = self.tree.tokens(f.path)
                sp = lint.preamble_span(NT, f)
                if sp is None:
                    R.fail("V1b", f"{f.path}:{f.line}: {name} has no generated binding markers")
                    continue
                if not lint.preamble_leads(NT, f):
                    R.fail("V1b", f"{f.path}:{f.line}: {name}'s body holds code before its bindings: "
                                  f"`{NT.join(f.body[0] + 1, min(sp[0], f.body[0] + 13))}...` (the opening "
                                  "marker must be the first thing in the body)")
                self.tclaim(NT, decl_start(NT, f), sp[1] if sp[1] > sp[0] else f.body[0] + 1)
                self.fclaim(r.cpp, FT.line(decl_start(FT, fdef)), FT.line(fdef.body[0]))
                names = self.binding_names(r, fdef, f)
                seen = collections.Counter()
                for s2, e2 in statements(NT, *sp):
                    ds2 = parse_declaration(NT, s2, e2) or []
                    if len(ds2) != 1 or ds2[0].name not in names:
                        R.fail("V1b", f"{f.path}:{NT.line(s2)}: {name}'s bindings hold a statement that is "
                                      f"not one binding of a frozen parameter of the seam that V3a compares: "
                                      f"`{NT.join(s2, min(e2, s2 + 10))}...`")
                    else:
                        seen[ds2[0].name] += 1
                for nm in sorted(k for k, c in seen.items() if c > 1):
                    R.fail("V1b", f"{f.path}: {name}'s bindings declare {nm} {seen[nm]} times")
                a = render(FT, fdef.body[0] + 1, fdef.body[1])
                b = render(NT, sp[1], f.body[1])
                edited = []
                if name == lint.SOLVE:
                    a0 = a
                    a, b, edited = _solve_declared(a, b, self.R,
                                                   card=lint.is_card_form(f))
                d = same_lines(a, b)
                for k in edited:
                    if (d is None or k < d[0]) and k < len(b):
                        self.claim(f.path, b[k][2], b[k][2])
                        self.fclaim(r.cpp, a0[k][2], a0[k][2])
                n += 1
                if d is not None:
                    k, x, y = d
                    R.fail("V1b", f"{f.path}: {name}'s body below its bindings differs from frozen at code "
                                  f"line {k}: frozen `{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")
        R.note("V1b", f"{n} converted bodies compared with frozen")

    @staticmethod
    def binding_names(r, fdef, f):
        """The names a converted seam's bindings may declare: each frozen parameter of the
        seam that the conversion replaces by a binding line, so neither a kept parameter nor
        an output (both stay parameters of the definition @p f) nor a removed CALLBACK_DIRECT
        (the solve's replay calls its builder directly; no line binds it).  The CALLABLE
        predicate is one of them only where it is a frozen parameter (the DD build).  V3a
        compares each at every call site, so a declaration under another name (a parameter
        of another seam, a removed callback's, the predicate's in a seam that never had it)
        is a statement no check reads: its type's constructor or an immediately invoked
        lambda would run unchecked."""
        own = {p.name for p in f.params}
        return {p.name for p in fdef.params
                if p.name not in r.kept and p.name not in own
                and not (p.kind == "F" and p.name in SUBST.callbacks)}

    @staticmethod
    def forwarding_call(T, s, e, name):
        """True when the statement [s, e) is exactly `name(<arguments>);` or `return
        name(<arguments>);`: the call is the whole statement, so no comma operator, cast or
        second expression rides along with the forward (V3f checks each argument)."""
        j = s + 1 if T.t(s) == "return" else s
        if T.t(j) != name or T.t(j + 1) != "(" or T.t(e - 1) != ";":
            return False
        try:
            return T.close(j + 1) == e - 2
        except ValueError:
            return False

    # ==== V3a: bindings ===================================================================
    def v3a(self):
        R = self.R
        if not self.converted:
            R.na("V3a", "no seam converted")
            return
        cand = self.cand
        nT = cand.T
        nres = cand.resolver()
        n = 0
        for name in sorted(self.converted):
            r = self.by_name[name]
            fdecl, fdef = self.fdecls[name]
            fsites = [c for c in self.fz.calls if c.name == name]
            nsites = [c for c in calls_in(nT, cand.open + 1, cand.close) if c.name == name]
            if len(fsites) != len(nsites):
                R.fail("V3a", f"{name}: {len(fsites)} frozen call sites, {len(nsites)} here")
                continue
            defs = self.new_defs(name)
            if not defs:
                continue
            f = defs[0]
            DT = self.tree.tokens(f.path)
            for fc, nc in zip(fsites, nsites):
                actual = {}
                for k, p in enumerate(f.params):
                    if k < nc.nargs:
                        actual[p.name] = nres.resolve(*nc.args[k])
                kept_text = {p.name: nT.join(*nc.args[k]) for k, p in enumerate(f.params) if k < nc.nargs}
                n += self.compare_site(r, fdecl, fc, f, DT, actual, kept_text, f"{nT.path}:{nc.line}")
        n += self.replay_sites(nres)
        self.other_callers()
        R.note("V3a", f"{n} parameter bindings compared")

    def compare_site(self, r, fdecl, fc, f, DT, actual, kept_text, where):
        """Every frozen parameter's binding at frozen call site @p fc against the converted
        definition @p f evaluated with its parameters bound to @p actual (V3a)."""
        R, fT, fres = self.R, self.fz.drv.T, self.fz.res
        b = self.new_bindings(r, f, DT, actual)
        n = 0
        compared = set()
        for k, p in enumerate(fdecl.params):
            if k >= fc.nargs:
                continue
            if p.name in r.kept:
                ftext = fT.join(*fc.args[k])
                if kept_text.get(p.name) != ftext:
                    R.fail("V3a", f"{r.name} at {where}: kept {p.name} is `{kept_text.get(p.name)}`, frozen `{ftext}`")
                continue
            if p.kind == "F":
                self.removed_param(r, p, f, DT, actual)
                if p.name in SUBST.callable:
                    compared.add(p.name)       # its one forwarder, check_forwarder's
                continue
            n += 1
            compared.add(p.name)
            want = apply_declarator(p.kind, frozen_canon(fres.resolve(*fc.args[k])))
            if fT.join(*fc.args[k]).replace(" ", "") == (self.value_equal or "?").replace(" ", ""):
                want = apply_declarator(p.kind, Ref("L:emitted"))
            got = b.get(p.name)
            if got is None:
                R.fail("V3a", f"{r.name}: no binding line for frozen parameter {p.name}")
                continue
            node, dtext, written, form = got
            if node is None:
                R.fail("V3a", f"{r.name}: {p.name}'s binding `{dtext}` has no initializer")
                continue
            if form not in ("param", "forward", "="):
                R.fail("V3a", f"{r.name}: {p.name}'s binding `{dtext}` is initialized with `{form}`; a "
                              "binding line is `<the frozen declarator> = <expression>;`")
            if form == "multi":
                R.fail("V3a", f"{r.name}: {p.name} shares its binding statement with another name")
            if form != "forward":
                if dtext is None or join_text(dtext) != join_text(p.text):
                    R.fail("V3a", f"{r.name}: {p.name} is declared `{dtext}`, frozen `{p.text}`")
                if r.mech != ADAPTER:
                    is_out = isinstance(node, Ref) and node.root in self.canon.ep_keys and p.kind in ("W", "W*")
                    if is_out != written:
                        R.fail("V3a", f"{r.name}: the binding of {p.name} "
                                      + ("writes the epoch but is not marked `// written`" if is_out
                                         else "is marked `// written` but writes no epoch member"))
            gw = normalize(self.canon(node), self.const_roots)
            ww = normalize(want, self.const_roots)
            if show(gw) != show(ww):
                R.fail("V3a", f"{r.name} at {where}: {p.name} binds {show(gw)}, frozen {show(ww)}")
        # Every binding line is one this call site compares: a line under any other name
        # (another seam's parameter, a removed callback's, a parameter the frozen call leaves
        # to its default) declares an object no check reads.
        for nm, (_, dtext, _, form) in sorted(b.items()):
            if form not in ("param", "forward") and nm not in compared:
                R.fail("V3a", f"{r.name} at {where}: the binding line `{dtext} ...` declares {nm}, which "
                              "binds no frozen parameter this call site passes")
        return n

    def replay_sites(self, nres):
        """CALLBACK_DIRECT: the solve's replay calls the two builders directly; at each such
        call the builders' bindings, evaluated through the solve's own call site, must be the
        objects the frozen closures captured (the frozen primary call inside each closure)."""
        R = self.R
        if lint.SOLVE not in self.converted:
            return 0
        cand = self.cand
        sdefs = self.new_defs(lint.SOLVE)
        ssite = [c for c in calls_in(cand.T, cand.open + 1, cand.close) if c.name == lint.SOLVE]
        if not sdefs or not ssite:
            return 0
        sd = sdefs[0]
        ST = self.tree.tokens(sd.path)
        s_actual = {p.name: nres.resolve(*ssite[0].args[k]) for k, p in enumerate(sd.params)
                    if k < ssite[0].nargs}
        ssc = Scopes(ST, sd.body[0], sd.body[1], sd.params)
        sres = Resolver(ssc, {id(ssc.root): "S"}, (), set(), params=s_actual)
        n = 0
        for cb in SUBST.callbacks:
            builder = "network" + cb[0].upper() + cb[1:]
            r = self.by_name.get(builder)
            if r is None or builder not in self.converted:
                R.fail("V3a", f"{lint.SOLVE} is converted but {builder} is not (CALLBACK_DIRECT needs both)")
                continue
            calls = [c for c in calls_in(ST, sd.body[0] + 1, sd.body[1]) if c.name == builder]
            if not calls:
                R.fail("V3a", f"{lint.SOLVE}: the replay does not call {builder} directly")
                continue
            fdecl, _ = self.fdecls[builder]
            fc = next((c for c in self.fz.calls if c.name == builder), None)
            bdefs = self.new_defs(builder)
            if fc is None or not bdefs:
                continue
            bd = bdefs[0]
            BT = self.tree.tokens(bd.path)
            for c in calls:
                actual = {p.name: sres.resolve(*c.args[k]) for k, p in enumerate(bd.params) if k < c.nargs}
                n += self.compare_site(r, fdecl, fc, bd, BT, actual, {}, f"{sd.path}:{c.line} (replay)")
        return n

    def new_bindings(self, r, f, DT, actual):
        """{frozen parameter name: (node, declarator text, written, form)} of a converted
        definition: its binding lines (or an adapter's forwarding arguments), resolved with the
        definition's parameters bound to @p actual.  form: `param` (the name is one of the
        definition's own parameters, an output or a kept one), `forward` (an adapter's
        forwarding argument), `=` (a binding line `T name = ...;`), the initializer's bracket
        for a direct-initialized line, or `multi` for a line that declares several names; the
        declarator text is the binding line's declarator in every form (V3a compares it)."""
        out = {}
        sc = Scopes(DT, f.body[0], f.body[1], f.params)
        res = Resolver(sc, {id(sc.root): "S"}, (), set(), params=actual)
        for p in f.params[2:]:
            out[p.name] = (actual.get(p.name), p.text, True, "param")
        if r.mech == ADAPTER:
            st = statements(DT, f.body[0] + 1, f.body[1])
            fwd = next((c for c in calls_in(DT, *st[0]) if c.name == r.name), None) if st else None
            fdecl = self.fdecls[r.name][0]
            if fwd is not None:
                for k, p in enumerate(fdecl.params):
                    if k < fwd.nargs:
                        out[p.name] = (apply_declarator(p.kind, res.resolve(*fwd.args[k])), None, False,
                                       "forward")
            return out
        sp = lint.preamble_span(DT, f)
        if sp is None:
            return out
        for s, e in statements(DT, sp[0], sp[1]):
            ds = parse_declaration(DT, s, e) or []
            for d in ds:
                if not d.init:          # default-initialized: compare_site fails it, or V1b
                    out[d.name] = (None, DT.src(d.type_span[0], d.tok + 1).strip(), False, "none")
                    continue
                node = res.resolve(*d.init)
                if not d.is_ref and not d.is_ptr:
                    node = Copy(node) if isinstance(node, (Ref, Copy)) else node
                if d.init_kind == "=":
                    text = DT.src(d.type_span[0], d.init[0] - 1).strip()
                else:
                    text = DT.src(d.type_span[0], d.tok + 1).strip()
                form = "multi" if len(ds) > 1 else (d.init_kind or "?")
                line_end = DT.source.find("\n", DT[e - 1].end)
                written = "// written" in DT.source[DT[e - 1].end:line_end if line_end >= 0 else None]
                out[d.name] = (node, text, written, form)
        return out

    def removed_param(self, r, p, f, DT, actual):
        """A frozen std::function parameter the conversion removed: a declared CALLBACK_DIRECT
        (the solve's replay calls the builder directly, checked by replay_sites), or the
        CALLABLE third-band predicate, whose binding line must be its one declared forwarder
        (check_forwarder) over this call site's run."""
        if p.name in SUBST.callable:
            sp = lint.preamble_span(DT, f)
            hits = []
            for s, e in (statements(DT, *sp) if sp else []):
                for d in parse_declaration(DT, s, e) or []:
                    if d.name == p.name:
                        hits.append((s, e, d))
            if len(hits) != 1:
                self.R.fail("V3a", f"{r.name}: {len(hits)} binding lines of {p.name} (one forwarder to "
                                   f"{PREDICATE} expected)")
                return
            s, e, d = hits[0]
            sc = Scopes(DT, f.body[0], f.body[1], f.params)
            res = Resolver(sc, {id(sc.root): "S"}, (), set(), params=actual)
            self.check_forwarder(DT, s, e, d, res, "V3a", p.text)
        elif p.name not in SUBST.callbacks:
            self.R.fail("V3a", f"{r.name}: std::function parameter {p.name} removed without a declared class")

    def other_callers(self):
        """Adapters' other callers keep their frozen text (spec 3.6)."""
        names = {n for n in self.converted if self.by_name[n].mech == ADAPTER}
        for path in self.fz.tree.files("src", "tests"):
            if not path.endswith((".cpp", ".hpp")) or path == DRIVER_PATH:
                continue
            text = self.fz.tree.read(path)
            if not text or not any(n in text for n in names):
                continue
            FT, NT = self.fz.tree.tokens(path), self.tree.tokens(path)
            if NT is None:
                self.R.fail("V3a", f"{path}: missing (it calls an adapter)")
                continue
            fc = [c for c in calls_in(FT, 0, FT.n) if c.name in names]
            nc = [c for c in calls_in(NT, 0, NT.n) if c.name in names]
            fdecls = {f.name_tok for f in self.fz.tree.index(path).funcs}
            ndecls = {f.name_tok for f in self.tree.index(path).funcs}
            fc = [c for c in fc if c.tok not in fdecls]
            nc = [c for c in nc if c.tok not in ndecls]
            fn_ = [FT.join(c.tok, c.close + 1) for c in fc]
            nn_ = [NT.join(c.tok, c.close + 1) for c in nc]
            for x in fn_:
                if x not in nn_:
                    self.R.fail("V3a", f"{path}: the adapter call `{x[:80]}...` changed")

    # ==== V3f: fold grammar ===============================================================
    def v3f(self):
        R = self.R
        if not self.converted:
            R.na("V3f", "no seam converted")
            return
        n = 0
        for name in sorted(self.converted):
            r = self.by_name[name]
            for f in self.new_defs(name):
                DT = self.tree.tokens(f.path)
                sc = Scopes(DT, f.body[0], f.body[1], f.params)
                res = Resolver(sc, {id(sc.root): "S"}, ())
                spans = []
                if r.mech == ADAPTER:
                    st = statements(DT, f.body[0] + 1, f.body[1])
                    fwd = next((c for c in calls_in(DT, *st[0]) if c.name == name), None) if st else None
                    spans = list(fwd.args) if fwd else []
                else:
                    # The CALLABLE predicate's forwarder, where the seam has that parameter, is
                    # the one binding that is a lambda (check_forwarder reads it whole); any
                    # other initializer, an immediately invoked lambda included, is folded.
                    fdef = self.fdecls[name][1] if name in self.fdecls else None
                    fwd = {p.name for p in (fdef.params if fdef else ()) if p.kind == "F" and p.name in SUBST.callable}
                    sp = lint.preamble_span(DT, f)
                    for s, e in (statements(DT, *sp) if sp else []):
                        for d in parse_declaration(DT, s, e) or []:
                            if d.init and not (d.kind == "closure" and d.name in fwd):
                                spans.append(d.init)
                for s, e in spans:
                    n += 1
                    why = fold_problem(res.resolve(s, e))
                    if why:
                        R.fail("V3f", f"{f.path}:{DT.line(s)}: {name}: `{DT.join(s, e)}` is outside the fold "
                                      f"grammar: {why}")
        R.note("V3f", f"{n} folded expressions checked")

    # ==== V3c: realization ================================================================
    def v3c(self):
        R = self.R
        if not self.epoch:
            R.na("V3c", "empty conversion: every epoch local keeps its frozen declaration (V1a)")
            return
        nT, N = self.cand.T, self.cand.stmts
        hdr = self.tree.read(lint.EPOCH_HPP)
        if hdr is None:
            R.fail("V3c", f"{lint.EPOCH_HPP} does not exist")
            return
        groups = struct_members(self.tree, "NetworkEpoch")
        for local, (path, tr) in SUBST.epoch.items():
            if tr == "CLOCK":
                continue            # kept const locals; the store and EpochClock are checked below
            rec = self.realized.get(local)
            if rec is None:
                R.fail("V3c", f"{local}: not realized")
                continue
            parts = path.split(".")
            member = None
            if len(parts) == 2:
                member = groups.get(parts[1])
            elif len(parts) == 3 and parts[1] in groups:
                gtype = groups[parts[1]]["type"].replace("const ", "")
                member = struct_members(self.tree, gtype).get(parts[2])
            if member is None:
                R.fail("V3c", f"{local}: {path} is not a member path of NetworkEpoch")
                continue
            ftype = rec.get("type") or ""
            mt = member["type"]
            if tr not in ("WS",) and ftype and mt.replace("const ", "") != ftype.replace("const ", ""):
                R.fail("V3c", f"{local}: member {path} has type `{mt}`, the local `{ftype}`")
            if tr == "SCALAR":
                if member["init"] != rec.get("init"):
                    R.fail("V3c", f"{local}: member {path} default `{member['init']}`, frozen initializer "
                                  f"`{rec.get('init')}`")
            elif member["init"] not in ("", "{}") and tr not in ("CLOCK",):
                R.fail("V3c", f"{local}: member {path} has a default initializer `{member['init']}`")
            at = rec.get("stmt")
            if at is None:
                at = rec.get("dropped_at", 0)
            for k in range(0, min(at, len(N))):
                s, e = N[k]
                for j in range(s, e):
                    if nT.t(j) == "ep" and nT.t(j - 1) not in (".", "->"):
                        q = ep_path_tokens(nT, j, e)
                        p2 = norm_subs(nT, j, q, ())
                        if p2 == path or p2.startswith(path + "."):
                            R.fail("V3c", f"{nT.path}:{nT.line(j)}: {p2} is bound before its realization")
        clock = struct_members(self.tree, "EpochClock") or {}
        clk = [n for n, (p, t) in SUBST.epoch.items() if t == "CLOCK"]
        if list(clock.keys()) != clk:
            R.fail("V3c", f"EpochClock members {list(clock.keys())}, the CLOCK store's order is {clk}")
        self.epoch_type()
        R.note("V3c", f"{len(SUBST.epoch)} epoch members checked against their locals")

    def epoch_type(self):
        """NetworkEpoch holds nothing but what the declared substitutions place in it, and
        starts every epoch in the frozen state (V3c): its members are `se` (a reference to
        const), `clock`, the groups of the EPOCH_MEMBER paths and `emitted` (bool, false), in
        the declared layout (declared.EPOCH_LAYOUT, so construction and destruction order are
        declared); each group holds exactly its declared locals in their declared order and
        EpochClock the three clock values with the frozen locals' types; NetworkEpoch and each
        group is a plain struct: no base clause (a base would take an aggregate initializer's
        values, hide a member behind one of the same name, or add a function a seam call
        finds) and nothing but its data members and member functions (no friend, nested type,
        alias, static member or static_assert); the epoch's one constructor binds `se` and
        does nothing else (an empty body, so no member starts other than as its local did);
        the ACCESSOR x_emit() returns, by reference, the frozen x_emit initializer under the
        EPOCH_MEMBER mapping; every other member function is deleted, and the other structs of
        the header declare none.  NetworkEpoch and the group structs of the header are claimed
        for V1e here."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        H = lint.EPOCH_HPP
        ep = class_parts(self.tree, H, "NetworkEpoch")
        if ep is None:
            R.fail("V3c", f"{H}: struct NetworkEpoch is missing")
            return
        groups = collections.OrderedDict()
        for local, (path, tr) in SUBST.epoch.items():
            parts = path.split(".")
            if len(parts) == 3:
                groups.setdefault(parts[1], []).append((parts[2], local, tr))
            else:
                groups.setdefault(parts[1], None)
        want = {"se", "emitted"} | set(groups)
        got = set(ep["members"])
        if got != want:
            R.fail("V3c", f"NetworkEpoch members {sorted(got)}; the declared substitutions place {sorted(want)}")
        layout = [m for m in declared.EPOCH_LAYOUT if m in want]
        if set(layout) != want:
            R.fail("V3c", f"declared.EPOCH_LAYOUT does not place {sorted(want - set(layout))}")
        elif list(ep["members"]) != layout:
            R.fail("V3c", f"NetworkEpoch declares {list(ep['members'])}; the declared layout is {layout} "
                          "(the members are constructed in declaration order and destroyed in reverse)")
        self.plain_struct("V3c", ep, "NetworkEpoch")
        self.tclaim(ep["T"], ep["head"], ep["end"])
        se = ep["members"].get("se")
        if se is not None and (se["type"], se["ops"], se["init"]) != ("const SyncedEpoch", "&", ""):
            R.fail("V3c", f"NetworkEpoch::se is `{se['type']}{se['ops']}`, not `const SyncedEpoch&`")
        em = ep["members"].get("emitted")
        if em is not None and (em["type"], em["ops"], em["init"]) != ("bool", "", "false"):
            R.fail("V3c", f"NetworkEpoch::emitted is `{em['type']}{em['ops']} {{{em['init']}}}`, not `bool{{false}}`")
        lam = {d.name: d for d in fz.rn.lam_scope.decls}
        group_types = set()
        for g, fields in groups.items():
            m = ep["members"].get(g)
            if m is None or fields is None:
                continue
            if m["ops"] or m["init"] not in ("", "{}"):
                R.fail("V3c", f"NetworkEpoch::{g} is `{m['type']}{m['ops']} {m['init']}`, not a default-built value")
            gp = class_parts(self.tree, H, m["type"]) or class_parts_anywhere(self.tree, m["type"])
            if gp is None:
                R.fail("V3c", f"NetworkEpoch::{g} is a `{m['type']}`, which no header under include/ defines "
                              "under that name, so its members cannot be checked")
                continue
            group_types.add(m["type"])
            names = [f for f, _, _ in fields]
            if list(gp["members"]) != names:
                R.fail("V3c", f"{m['type']} members {list(gp['members'])}; the declared epoch locals of "
                              f"ep.{g} are {names}, in that order")
            self.plain_struct("V3c", gp, m["type"])
            live = [f["name"] for f in gp["funcs"] if not f["deleted"]]
            if live:
                R.fail("V3c", f"{m['type']} (ep.{g}) declares the member functions {live}: a group is "
                              "a plain aggregate the epoch builds by default construction")
            if gp["T"].path == H:
                self.tclaim(gp["T"], gp["head"], gp["end"])
            for fname, local, tr in fields:
                x = gp["members"].get(fname)
                if tr == "CLOCK" and x is not None and local in lam:
                    want_t = fT.join(*lam[local].type_span).replace("const ", "", 1)
                    if x["type"] != want_t or x["ops"]:
                        R.fail("V3c", f"EpochClock::{fname} is `{x['type']}{x['ops']}`, the frozen const local "
                                      f"`{fT.join(*lam[local].type_span)}`")
        ctors = [f for f in ep["funcs"] if not f["deleted"] and f["name"] == "NetworkEpoch"]
        if len(ctors) != 1:
            R.fail("V3c", f"NetworkEpoch: {len(ctors)} constructors that are not deleted (one expected)")
        else:
            c = ctors[0]
            pn = c["params"][0].name if c["params"] else ""
            if [lint.ptype(q) for q in c["params"]] != ["const SyncedEpoch&"]:
                R.fail("V3c", "NetworkEpoch's constructor does not take one `const SyncedEpoch&`")
            if [(i[0], i[3]) for i in c["inits"]] != [("se", f"se({pn})")]:
                R.fail("V3c", f"NetworkEpoch's constructor initializes {[i[3] for i in c['inits']]}; only "
                              f"`se({pn})` is declared")
            if c["body"] is None or c["body"][1] != c["body"][0] + 1:
                R.fail("V3c", "NetworkEpoch's constructor body is not empty: every member starts as its "
                              "local did, so the constructor does nothing but bind se")
            if member_trail(ep["T"], c):
                R.fail("V3c", f"NetworkEpoch's constructor has `{member_trail(ep['T'], c)}` after its "
                              "parameter list (nothing is declared there: a member's construction that "
                              "throws reaches the run's handler, as the frozen local's did)")
        acc = [f for f in ep["funcs"] if not f["deleted"] and f["name"] != "NetworkEpoch"]
        for f in acc:
            if f["name"] not in [x.replace("ep.", "").replace("()", "") for x in SUBST.accessor.values() if x]:
                R.fail("V3c", f"NetworkEpoch declares the member function {f['name']}")
        for local, path in SUBST.accessor.items():
            fname = path.replace("ep.", "").replace("()", "") if path else None
            fs = [f for f in acc if f["name"] == fname]
            if len(fs) != 1:
                R.fail("V3c", f"NetworkEpoch: {len(fs)} definitions of the accessor {fname}()")
                continue
            self.accessor_check(ep, fs[0], local)
        TT = ep["T"]
        for kind, s, e, ns, name in file_items(TT):
            if kind == "struct" and name != "NetworkEpoch" and name not in group_types:
                gp = class_parts(self.tree, H, name)
                live = [f["name"] for f in (gp["funcs"] if gp else []) if not f["deleted"]]
                if live:
                    R.fail("V3c", f"{H}: {name} declares member functions {live}")
                if gp is not None:
                    self.plain_struct("V3c", gp, name)

    def plain_struct(self, chk, cp, what):
        """A struct the step defines is a plain struct of its declared parts: no base clause (a
        base would take an aggregate initializer's values by brace elision, hide a member
        behind one of the same name, or add a member or friend function a seam call finds by
        name lookup or ADL) and nothing beyond its data members and member functions (no
        friend, nested type, alias, static member or static_assert: a hidden friend is found
        by ADL at a seam call, a nested alias changes what a member's type names)."""
        if cp is None:
            return
        T = cp["T"]
        if cp["base"]:
            self.R.fail(chk, f"{T.path}:{T.line(cp['head'])}: {what} is declared `{cp['base']}` (a base "
                             "clause); a type of the step is a plain struct")
        for a, b, txt in cp["others"]:
            self.R.fail(chk, f"{T.path}:{T.line(a)}: {what} declares `{txt[:70]}`, which is neither a data "
                             "member nor a member function")

    def accessor_check(self, ep, f, local):
        """ACCESSOR: the accessor's return type is the frozen local's declarator, it is const
        and takes nothing, and its body is one return whose expression, its NetworkEpoch
        members read as ep.<member>, resolves to the frozen initializer of @p local."""
        R, fz = self.R, self.fz
        T = ep["T"]
        d = next((x for x in fz.rn.lam_scope.decls if x.name == local), None)
        if d is None or d.init is None:
            R.fail("V3c", f"the frozen accessor local {local} was not found")
            return
        fT = fz.rn.T
        want_ret = fT.join(*d.type_span) + d.ops
        ret = T.join(f["start"], f["po"] - 1)
        if ret != want_ret or f["params"] or member_trail(T, f) != "const":
            R.fail("V3c", f"NetworkEpoch::{f['name']} is `{ret} {f['name']}(...) {member_trail(T, f)}`; the "
                          f"frozen local is `{want_ret} {local}`, so `{want_ret} {f['name']}() const` (nothing "
                          "else after the list) is declared")
        if f["body"] is None:
            R.fail("V3c", f"NetworkEpoch::{f['name']} has no body")
            return
        bo, bc = f["body"]
        st = statements(T, bo + 1, bc)
        if len(st) != 1 or T.t(st[0][0]) != "return":
            R.fail("V3c", f"NetworkEpoch::{f['name']}'s body is not one return statement")
            return
        a, b = st[0][0] + 1, st[0][1] - 1
        sc = Scopes(T, bo, bc)
        node = Resolver(sc, {id(sc.root): "S"}, (), set(ep["members"])).resolve(a, b)
        got = normalize(self.epoch_members_node(node), self.const_roots)
        want = normalize(frozen_canon(fz.res.resolve(*d.init)), self.const_roots)
        if show(got) != show(want):
            R.fail("V3c", f"NetworkEpoch::{f['name']} returns `{T.join(a, b)}` = {show(got)}; the frozen "
                          f"{local} is `{fT.join(*d.init)}` = {show(want)}")

    def epoch_members_node(self, node):
        """A node resolved inside a NetworkEpoch member function (its members as roots A:<m>)
        in the frozen vocabulary: A:<m>.<chain> is ep.<m>.<chain> through the declared paths."""
        if isinstance(node, Ref) and node.root.startswith("A:"):
            r = self.canon.ep_path(("." + node.root[2:],) + tuple(node.chain))
            return Ref(r.root, r.chain, node.prefix) if isinstance(r, Ref) else r
        if isinstance(node, Copy):
            return Copy(self.epoch_members_node(node.inner))
        if isinstance(node, Cond):
            return Cond(self.epoch_members_node(node.c), self.epoch_members_node(node.a),
                        self.epoch_members_node(node.b))
        if isinstance(node, Expr):
            return Expr(node.op, [self.epoch_members_node(x) for x in node.args])
        if isinstance(node, CallNode):
            return CallNode(node.fn, [self.epoch_members_node(x) for x in node.args])
        return node

    # ==== V5: deep const ==================================================================
    def v5(self):
        R = self.R
        if not self.epoch or self.tree.read(lint.EPOCH_HPP) is None:
            R.na("V5", "no NetworkEpoch in this tree")
            return
        seen, stack, n = set(), [("NetworkEpoch", "ep")], 0
        while stack:
            name, where = stack.pop()
            if name in seen:
                continue
            seen.add(name)
            ms = struct_members(self.tree, name)
            if ms is None:
                continue
            for b in struct_bases(self.tree, name):
                stack.append((b, f"{where}<base {b}>"))
            for m, info in ms.items():
                n += 1
                t = info["type"] + info["ops"]
                if re.search(r"\b(unique_ptr|shared_ptr|weak_ptr|function|reference_wrapper)\b", t):
                    R.fail("V5", f"{name}::{m} ({where}.{m}) is `{t}`")
                if "&" in info["ops"] and not (name == "NetworkEpoch" and m == "se" and t.startswith("const")):
                    R.fail("V5", f"{name}::{m} is a reference `{t}`")
                if "*" in info["ops"] and not info["type"].startswith("const"):
                    R.fail("V5", f"{name}::{m} is a pointer to non-const `{t}`")
                for ident in re.findall(r"\b([A-Z]\w+)\b", info["type"]):
                    if ident not in seen:
                        stack.append((ident, f"{where}.{m}"))
        R.note("V5", f"{len(seen)} types, {n} members scanned from NetworkEpoch")

    # ==== V7: documentation ===============================================================
    def v7(self):
        R = self.R
        if not self.converted:
            R.na("V7", "no seam converted")
            return
        for name in sorted(self.converted):
            r = self.by_name[name]
            fdecl, _ = self.fdecls[name]
            FT = self.fz.tree.tokens(r.hpp)
            fdoc = doc_before(FT, decl_start(FT, fdecl))
            hidx = self.tree.index(r.hpp)
            NT = self.tree.tokens(r.hpp)
            decls = [f for f in (hidx.funcs if hidx else []) if f.name == name and not f.is_def
                     and f.params and lint.is_epoch_form(f)]
            if not decls:
                continue
            ndoc = doc_before(NT, decl_start(NT, decls[0]))
            self.tclaim(NT, decl_start(NT, decls[0]), decl_end(NT, decls[0]))
            if not ndoc:
                R.fail("V7", f"{r.hpp}:{decls[0].line}: {name}'s epoch form has no Doxygen block")
                continue
            ntext = doc_words(ndoc[0])
            # An adapter's explicit declaration keeps its Doxygen (V1c), @param texts included,
            # directly above; its epoch form names the bindings.
            for pname, ptext in ([] if r.mech == ADAPTER else params_doc(fdoc[0] if fdoc else "")):
                if pname in r.kept:
                    continue
                t = doc_words(ptext)
                if t and t not in ntext:
                    R.fail("V7", f"{r.hpp}: {name}: the frozen @param {pname} text is not carried")
            for f in self.new_defs(name):
                DT = self.tree.tokens(f.path)
                for path in sorted({p for p, _ in lint.binding_reads(DT, f)}):
                    leafp = path.split(".", 1)[1]
                    if leafp not in ntext and path not in ntext:
                        R.fail("V7", f"{r.hpp}: {name}: the bound member {path} is not named in its Doxygen")
        R.note("V7", f"{len(self.converted)} converted declarations' documentation checked")

    # ==== The step's new files: what each holds ==========================================
    # The specification's alias of a measure-only seam's epoch form (spec 2.3).
    OBSERVER_ALIAS = "using NetworkEpochObserver = void (*)(NetworkRun& run, const NetworkEpoch& ep);"

    def new_header(self, h):
        """The top-level items of a new header (V9), checked against what the specification
        declares there, each accepted item claimed for V1e: #pragma once, #include lines and
        conditional groups of them (V1e's line rule); namespace GnssProcessor (and `namespace
        spdlog { class logger; }`); forward declarations; the structs (app/NetworkRun.hpp:
        NetworkRun; app/NetworkEpoch.hpp: NetworkEpoch and the types of its members it
        defines), each a plain `struct X {` (no base clause) whose contents V3c / V3d verify
        and claim; static_asserts; in app/NetworkRun.hpp the declarations of networkEngineSpec
        and the two brackets, each with its one definition's signature; in app/NetworkEpoch.hpp
        the observer alias.  A using-declaration, a typedef, a variable, a function
        definition, a nested namespace or any other struct fails, so no name a seam call or a
        member type resolves to can be redeclared there.  Returns the items."""
        R = self.R
        T = self.tree.tokens(h)
        if T is None:
            return []
        items = file_items(T)
        if h == RUN_HPP:
            structs = {"NetworkRun"}
            fdecls, alias = {"networkEngineSpec", "networkSubBegin", "networkSubEnd"}, None
        else:
            ep = class_parts(self.tree, h, "NetworkEpoch")
            structs = {"NetworkEpoch"} | {m["type"] for m in (ep["members"].values() if ep else [])}
            fdecls, alias = set(), join_text(self.OBSERVER_ALIAS)
        seen = collections.Counter()
        for kind, s, e, ns, name in items:
            where = f"{h}:{T.line(s)}"
            if kind == "pp":
                continue            # V1e's line rule: #pragma once, #include, conditional includes
            if kind in ("ns", "nsclose") and ns == () and name in ("GnssProcessor", "spdlog"):
                self.tclaim(T, s, e)
                continue
            if kind == "fwd" and (ns == ("GnssProcessor",) or (ns == ("spdlog",) and name == "logger")):
                self.tclaim(T, s, e)
                continue
            if kind == "struct" and ns == ("GnssProcessor",) and name in structs:
                seen[name] += 1
                cp = class_parts(self.tree, h, name)
                if cp is not None and cp["base"]:
                    R.fail("V9", f"{where}: struct {name} is declared `{cp['base']}`: a type of the step is "
                                 "a plain struct, so no base class adds members, conversions or functions a "
                                 "seam call or an aggregate initializer finds")
                continue            # claimed by the check that verifies its contents
            if kind == "fdecl" and ns == ("GnssProcessor",) and name in fdecls:
                dfn = find_defs(self.tree, name)
                dcl = next((f for f in self.tree.index(h).funcs if f.name == name and not f.is_def
                            and decl_start(T, f) == s), None)
                DT = self.tree.tokens(dfn[0].path) if len(dfn) == 1 else None
                if DT is None or dcl is None or \
                        [lint.ptype(q) for q in dcl.params] != [lint.ptype(q) for q in dfn[0].params] or \
                        T.join(s, dcl.name_tok) != DT.join(decl_start(DT, dfn[0]), dfn[0].name_tok) or \
                        head_diff(T, dcl, {"defaults": head_defaults(dfn[0]), "trail": head_trail(DT, dfn[0])}):
                    R.fail("V9", f"{where}: the declaration of {name} is not its one definition's signature "
                                 "(return type, parameter types, default arguments, specifiers after the list)")
                self.tclaim(T, s, e)
                continue
            if kind == "stmt" and ns == ("GnssProcessor",) and (
                    (name == "static_assert" and T.t(e - 1) == ";") or (alias and T.join(s, e) == alias)):
                self.tclaim(T, s, e)
                continue
            R.fail("V9", f"{where}: {h} declares `{T.join(s, min(e, s + 14))}...` ({kind}), which the "
                         "specification does not put there")
        for name in sorted(seen):
            if seen[name] != 1:
                R.fail("V9", f"{h}: {seen[name]} definitions of struct {name}")
        return items

    def new_sources(self):
        """The step's new source files hold exactly what the specification puts there (V1e):
        src/core/app/NetworkRun.cpp the definitions of networkEngineSpec, networkSubBegin and
        networkSubEnd (V12 and V1d verify and claim them); src/core/app/ApplicationNetworkEpoch.cpp
        the one definition of Application::networkProcessEpoch_ (V1a); the contract test its
        probe struct of member-pointer constants, its static_asserts and their aliases, and its
        one TEST case.  Around them: #include lines, conditional groups of #include lines and
        namespace GnssProcessor (the test also an anonymous namespace and its using
        directive).  A static object, a helper, an overload, an alias or a second definition
        anywhere in these files fails here, whatever line it shares."""
        R = self.R
        want = {RUN_CPP: {"networkEngineSpec", "networkSubBegin", "networkSubEnd"},
                lint.EPOCH_CPP: {lint.EPOCH_FN}}
        for path, fnames in want.items():
            T = self.tree.tokens(path)
            if T is None:
                R.fail("V1e", f"{path}: missing")
                continue
            seen = collections.Counter()
            for kind, s, e, ns, name in file_items(T):
                if kind == "pp":
                    continue        # the line rule
                if kind in ("ns", "nsclose") and ns == () and name == "GnssProcessor":
                    self.tclaim(T, s, e)
                elif kind == "fdef" and ns == ("GnssProcessor",) and name in fnames:
                    seen[name] += 1
                else:
                    R.fail("V1e", f"{path}:{T.line(s)}: `{T.join(s, min(e, s + 14))}...` ({kind}); the file "
                                  f"holds only the definitions of {', '.join(sorted(fnames))}")
            for n in sorted(fnames):
                if seen[n] != 1:
                    R.fail("V1e", f"{path}: {seen[n]} definitions of {n} (one expected)")
        T = self.tree.tokens(CONTRACT_TEST)
        if T is None:
            return
        case = join_text("TEST(NetworkEpochContract, CompiledChecks) { SUCCEED(); }")
        n_case = 0
        for kind, s, e, ns, name in file_items(T):
            txt = T.join(s, e)
            ok = False
            if kind == "pp":
                continue
            if kind in ("ns", "nsclose") and ns == () and name in ("GnssProcessor", ""):
                ok = True
            elif kind == "fwd":
                ok = True
            elif kind == "struct" and ns == ("GnssProcessor",) and name == "NetworkEpochContractProbe" \
                    and T.t(s + 2) == "{":
                body = statements(T, s + 3, T.close(s + 2))
                bad = [T.join(a, b) for a, b in body
                       if not re.fullmatch(r"using \w+=[^;{}]*;", T.join(a, b))
                       and not re.fullmatch(r"static constexpr \w+ \w+=&Application::\w+;", T.join(a, b))]
                if bad:
                    R.fail("V1e", f"{CONTRACT_TEST}: NetworkEpochContractProbe holds `{bad[0][:60]}`")
                ok = not bad
            elif ns == ("",) and (txt == join_text("using namespace GnssProcessor;")
                                  or (name == "static_assert" and T.t(e - 1) == ";")
                                  or re.fullmatch(r"using \w+=[^;{}]*;", txt)):
                ok = True
            elif ns == () and txt == case:
                n_case += 1
                ok = True
            if ok:
                self.tclaim(T, s, e)
            else:
                R.fail("V1e", f"{CONTRACT_TEST}:{T.line(s)}: `{T.join(s, min(e, s + 14))}...` ({kind}); the "
                              "contract test holds its probe, static_asserts, their aliases and one TEST case")
        if n_case != 1:
            R.fail("V1e", f"{CONTRACT_TEST}: {n_case} TEST cases (one expected)")

    # ==== V9: no new overload =============================================================
    def v9(self):
        R = self.R
        if not self.epoch:
            R.na("V9", "empty conversion: no new header")
            return
        allowed = {"networkEngineSpec", "networkSubBegin", "networkSubEnd", "x_emit", "NetworkRun",
                   "NetworkEpoch", "operator="}
        for h in (RUN_HPP, lint.EPOCH_HPP):
            idx = self.tree.index(h)
            if idx is None:
                R.fail("V9", f"{h} is missing")
                continue
            for f in idx.funcs:
                if f.name not in allowed and not f.cls:
                    R.fail("V9", f"{h}:{f.line}: declares {f.name}")
            # Member functions: only the run's and the epoch's constructors and the accessor
            # are defined (V3c and V3d check their bodies); everything else is deleted.  Every
            # struct the header defines is scanned, with a base clause or without (new_header
            # fails a base clause); a friend or a nested declaration fails V3c / V3d.
            HT = self.tree.tokens(h)
            live_ok = {"NetworkRun": {"NetworkRun"}, "NetworkEpoch": {"NetworkEpoch", "x_emit"}}
            for kind, s, e, ns, name in self.new_header(h):
                if kind != "struct":
                    continue
                cp = class_parts(self.tree, h, name)
                for f in (cp["funcs"] if cp else []):
                    if not f["deleted"] and f["name"] not in live_ok.get(name, set()):
                        R.fail("V9", f"{h}:{HT.line(f['start'])}: {name} declares the member function "
                                     f"{f['name']}")
        self.v9_translation_units()

    def v9_translation_units(self):
        """No call can resolve otherwise than at the base.  For each translation unit whose
        include closure is not its frozen counterpart's, file for file and text for text (the
        counterpart is the unit's own frozen file; for ApplicationNetworkEpoch.cpp and
        NetworkRun.cpp, which hold code cut from runNetwork, ApplicationNetwork.cpp), over the
        names its code calls: every free-function declaration the closure newly makes visible
        (any namespace, any file type the closure reaches) is a converted seam's epoch form,
        with the parameter types V2 checks, or one of the step's new functions
        (networkEngineSpec, the brackets, the moved predicate, which V9's header checks and
        V1d pin to one definition); none the frozen closure made visible is gone but a
        converted seam's explicit form; no newly reached file #defines an identifier of the
        unit, brings in a called name by a using-declaration or holds a using-directive.  The
        two units cut from runNetwork also reach no project header the frozen driver did not,
        but the step's two new headers, and include no library header the frozen driver's
        closure did not read (a library header included by a header is include_problem's).
        So a narrowed overload of a seam in any file a unit includes, an overload of a
        library name, or a dropped include that leaves another overload the better match,
        fails here."""
        R = self.R
        fzt, nt = self.fz.tree, self.tree
        newh = {RUN_HPP, lint.EPOCH_HPP}
        allowed, removed = collections.defaultdict(set), collections.defaultdict(set)
        for name in self.converted:
            r = self.by_name[name]
            allowed[name].add(tuple(self.epoch_signature(r)[0]))
            fdecl = self.fdecls.get(name, (None, None))[0]
            if r.mech != ADAPTER and fdecl is not None and fdecl.params is not None:
                removed[name].add(tuple(lint.ptype(p) for p in fdecl.params))
        new_names = {"networkEngineSpec", "networkSubBegin", "networkSubEnd"}
        new_names |= {"network" + n[0].upper() + n[1:] for n in SUBST.callable + SUBST.bracket}
        changed = self.changed_files()
        fcache = self.fz.__dict__.setdefault("_ep1_v9", {"clo": {}})
        cut = {lint.EPOCH_CPP: DRIVER_PATH, RUN_CPP: DRIVER_PATH}

        # A root the candidate does not carry (an export without third_party/) is read on
        # neither side, so the base's declarations there are not taken for removed ones.
        hidden = tuple(r + "/" for r in BUILD_INPUTS_IF_PRESENT if not nt.files(r))

        def readable(p):
            return not p.startswith(("../", "/")) and not (hidden and p.startswith(hidden))

        def visible(tree, files, name):
            out = set()
            for p in files:
                out.update(t for t, _ in lint.file_decls(tree, p)[0].get(name, ()))
            return out

        def check_unit(tu, ftu):
            """(problems, checked?) of one translation unit."""
            probs = []
            if ftu not in fcache["clo"]:
                fcache["clo"][ftu] = lint.include_closure(fzt, ftu)
            fclo, nclo = fcache["clo"][ftu], lint.include_closure(nt, tu)
            if tu == ftu and set(nclo) == set(fclo) and not (set(nclo) & changed):
                return probs, False, set(nclo)
            ncalls, nids = lint.file_calls(nt, tu)
            called = set(ncalls) | (lint.file_calls(fzt, ftu)[0] if tu == ftu else set())
            # A file both closures read with the same text declares the same on both sides, so
            # only the files that differ are indexed whole; a common file is read for a name
            # only when the two sides differ in it and its text names it.
            fset = {p for p in fclo if readable(p)}
            nset = {p for p in nclo if readable(p)}
            common = {p for p in fset & nset if p not in changed}
            for name in sorted(called - new_names):
                a, b = visible(fzt, fset - common, name), visible(nt, nset - common, name)
                if a == b:
                    continue
                c = visible(fzt, [p for p in common if name in (fzt.read(p) or "")], name)
                old, new = a | c, b | c
                for t in sorted(new - old - allowed.get(name, set())):
                    probs.append(f"{tu}: {name}({', '.join(t)}) is newly visible to a unit that calls {name}: "
                                 "an overload the step does not declare can take the call")
                for t in sorted(old - new - removed.get(name, set())):
                    probs.append(f"{tu}: {name}({', '.join(t)}), visible at the base, is not visible any more: "
                                 "the call may resolve to another overload")
            for p in sorted(set(nclo) - set(fclo) - {tu}):
                if p.startswith(("../", "/")):
                    continue
                _, macros, usings, directive, _ = lint.file_decls(nt, p)
                for m in sorted(macros & nids):
                    probs.append(f"{tu}: the newly reached {p} #defines `{m}`, an identifier of the unit")
                for u in sorted(usings & called):
                    probs.append(f"{tu}: the newly reached {p} brings in `{u}`, a name the unit calls, by a "
                                 "using-declaration")
                if directive:
                    probs.append(f"{tu}: the newly reached {p} holds a using-directive")
            if tu in cut:
                for p in sorted(set(nclo) - set(fclo) - newh - {tu}):
                    probs.append(f"{tu} reaches {p}, which {ftu} did not reach at the base (only "
                                 f"{RUN_HPP} and {lint.EPOCH_HPP} are new to the code cut from runNetwork)")
                base_libs = {(q, nm) for items in fclo.values() for _, q, nm, r in items
                             if r is None and q is not None}
                for _, q, nm, r in nclo[tu]:
                    if r is None and q is not None and (q, nm) not in base_libs:
                        probs.append(f"{tu} includes the library header {lint.include_spelling(q, nm)}, which "
                                     f"{ftu} did not read at the base")
            return probs, True, set(nclo) | {tu}

        # An overlay (a control's mutation) that edits no file a unit reads reuses the unit's
        # result on the unmutated tree.
        root, ov, structural = lint.overlay_parts(nt)
        memo = root.__dict__.setdefault("_ep1_v9_units", {})
        key = (self.fz.base, frozenset(self.converted), self.presolve_writer)
        n = 0
        for tu in sorted(p for p in nt.files("src", "tests") if p.endswith(".cpp")):
            if tu in (CONTRACT_TEST, ENGINE_SPEC_TEST):
                continue                  # V1e's new_sources holds the contract test; the suite the other
            ftu = cut.get(tu, tu)
            if fzt.read(ftu) is None:
                continue                  # a new file outside the step: V1e fails it
            hit = memo.get((key, tu))
            if hit is not None and not structural and not (hit[2] & set(ov)):
                probs, checked = hit[0], hit[1]
            else:
                probs, checked, files = check_unit(tu, ftu)
                if not ov:
                    memo[(key, tu)] = (probs, checked, files)
            n += checked
            for x in probs:
                R.fail("V9", x)
        R.note("V9", f"{n} translation units whose include closure changed checked for overloads")

    def changed_files(self):
        """The build inputs whose text differs from <B> (a new file included)."""
        if getattr(self, "_changed", None) is None:
            fzt, nt = self.fz.tree, self.tree
            self._changed = {p for p in self.build_inputs() if fzt.read(p) != nt.read(p)}
        return self._changed

    # ==== V10, V11 ========================================================================
    def v10_11(self):
        R = self.R
        if not self.epoch or self.tree.read(lint.EPOCH_HPP) is None:
            R.na("V10", "no epoch function or NetworkEpoch.hpp")
            R.na("V11", "no epoch function or NetworkEpoch.hpp")
            return
        lt = lint.Lint(self.tree)
        lt.v10()
        if self.presolve_writer == "epoch":
            for p in lt.problems["V10"]:
                R.fail("V10", p)
            for p in lt.info["V10"]:
                R.note("V10", p)
            R.na("V11", "--presolve-writer epoch: a pre-solve writer takes the whole epoch, so its "
                        "spine line names no group the banner could be checked against")
            return
        lt.v11(partial=len(self.converted) < len([r for r in self.rows if not isinstance(r.mech, Explicit)]))
        for chk in ("V10", "V11"):
            for p in lt.problems[chk]:
                R.fail(chk, p)
            for p in lt.info[chk]:
                R.note(chk, p)

    # ==== V1e: every hunk accounted for ===================================================
    def v1e(self):
        """Every changed line of the build's inputs (build_inputs: every file under src/,
        include/ and tests/ whatever its extension, CMakeLists.txt, and third_party/ and the
        root CMake configuration files when the tree carries them) is accounted for, token by
        token: each token of a changed line lies in a token range a check verified (tclaim),
        or the line is an #include at file scope (include_problem), a forward declaration at
        namespace scope of a class the project already names (textual_claim), or a line
        another check compared whole (claim; CMakeLists.txt's are cmake_build_list's); a lone
        brace line is accounted for only beside a line a check verified, never beside one
        accounted for by its text.  A C++ file is read without its comments, any other file
        as written, and no changed file holds what the reading layer reads otherwise than the
        compiler (nom_cpp.reading_problems: a line splice, a carriage return that ends no
        line, a control character, bytes that are not UTF-8) beyond its frozen
        counterpart's: the compiler splices `/* a *\\<NL>/ code /* b */` into live code
        that every check reads as one comment.  On an epoch tree the step's new
        files are diffed against nothing, so every line of them is accounted for the same way
        (their structure is checked by new_sources and, for the two headers, V9), except the
        hand-written engine specification test (spec 5.9, checked by the suite; its one
        listing is cmake_build_list's, and include_problem fails any #include of it).  Any
        other new file fails, whatever its extension: an inline file or a source included by
        name would put code into a translation unit that no check reads.  No changed line may
        be a preprocessor directive other than #include, a new header's #pragma once, or a
        conditional group that holds only #include lines (cond_include_lines): a #define, an
        #if 0 or an #else can change what the compiler builds without changing a token any
        check reads; a conditional group that enters the tree, or whose directive changes,
        holds no #include of a header the frozen file includes (`#if 0` around a frozen
        #include removes it).  No frozen #include is removed: a call could resolve to another
        overload once a declaration it found is gone."""
        R = self.R
        if self.epoch:
            self.new_sources()
        self.cmake_build_list()
        paths = self.build_inputs()
        n_files = n_lines = 0
        for path in sorted(paths):
            ft, nt = self.fz.tree.read(path), self.tree.read(path)
            if ft == nt:
                continue
            if ft is None:
                if path in NEW_FILES and self.epoch:
                    if path == ENGINE_SPEC_TEST:
                        continue        # hand-written (spec 5.9); the suite checks it, cmake_build_list
                                        # its one use and include_problem that no file includes it
                    ft = ""
                else:
                    R.fail("V1e", f"{path}: a new file outside the step's declared new files")
                    continue
            if nt is None:
                R.fail("V1e", f"{path}: deleted")
                continue
            cpp = path.endswith(CPP_EXT)
            self.reading(path, ft, nt, path.endswith(SOURCE_EXT) and not path.endswith(".cmake"))
            fl = _code_lines(ft, cpp)
            nl = _code_lines(nt, cpp)
            if [x[1] for x in fl] == [x[1] for x in nl]:
                continue
            n_files += 1
            T = self.tree.tokens(path) if cpp else None
            condl = cond_include_lines(T) if T is not None else set()
            new_incs = {lint.include_target(t) for _, t in nl if T is not None and pp_directive(t) == "include"}
            sm = difflib.SequenceMatcher(None, [x[1] for x in fl], [x[1] for x in nl], autojunk=False)
            ops = sm.get_opcodes()
            if T is not None:
                changed = {nl[q][0] for op, a0, a1, b0, b1 in ops if op != "equal" for q in range(b0, b1)}
                self.cond_groups(path, T, ft, changed)
            for op, a0, a1, b0, b1 in ops:
                if op == "equal":
                    continue
                for q in range(b0, b1):
                    ln, txt = nl[q]
                    n_lines += 1
                    d = pp_directive(txt) if T is not None else None
                    if d is not None:
                        if d == "include":
                            prob = self.include_problem(path, txt)
                            if prob:
                                R.fail("V1e", f"{path}:{ln}: `{txt.strip()[:90]}` {prob}")
                            elif not self.textual_claim(T, ln, txt):
                                R.fail("V1e", f"{path}:{ln}: `{txt.strip()[:90]}` inside a brace (an #include "
                                              "enters the tree at file scope only: inside a namespace, a class "
                                              "or a body it pastes the header's declarations there)")
                            continue
                        if ln in condl or (
                                txt.split() == ["#pragma", "once"] and path in NEW_FILES and path.endswith(".hpp")):
                            continue
                        R.fail("V1e", f"{path}:{ln}: a preprocessor directive enters the tree: "
                                      f"`{txt.strip()[:90]}` (only #include, a new header's #pragma once and "
                                      "a conditional group of #include lines are declared)")
                        continue
                    if re.match(r"^[{}]+;?$", txt.strip()) and any(
                            0 <= x < len(nl) and self.claimed(path, *nl[x], T, textual=False)
                            for x in (q - 1, q + 1)):
                        continue      # a lone brace the diff paired on the far side of a region
                    if not self.claimed(path, ln, txt, T):
                        bad = self.unclaimed_tokens(T, ln) if T is not None else []
                        R.fail("V1e", f"{path}:{ln}: an unaccounted change: `{txt.strip()[:90]}`"
                                      + (f" (unclaimed: `{' '.join(bad)[:60]}`)"
                                         if bad and len(bad) < len(txt.split()) else ""))
                for q in range(a0, a1):
                    ln, txt = fl[q]
                    if T is not None and pp_directive(txt) == "include":
                        if lint.include_target(txt) not in new_incs:
                            R.fail("V1e", f"{path}: frozen line {ln} `{txt.strip()[:90]}` removed: a declaration a "
                                          "call resolved to may no longer be visible (the step removes no #include)")
                        continue
                    if not self.fclaimed(path, ln, txt):
                        R.fail("V1e", f"{path}: frozen line {ln} removed or changed outside a declared "
                                      f"region: `{txt.strip()[:90]}`")
        R.note("V1e", f"{n_files} changed files, {n_lines} changed code lines; {len(paths)} build inputs read")

    def build_inputs(self):
        """The files the build reads, which V1e diffs against <B>: every file the frozen tree
        has under src/, include/ and tests/ (whatever its extension), every C, C++ or CMake file
        the candidate has there (an inline file, a template body, a source included by name:
        SOURCE_EXT), CMakeLists.txt, and, when the candidate carries them (a full checkout, not
        an export of the four), third_party/ and the CMake configuration files at the root.  A
        file the frozen tree lacks is new, and only the step's new files may be.  A data file
        the candidate alone has (a run's logs under tests/) is not read: no build compiles it,
        and include_problem fails any #include that would."""
        if getattr(self, "_inputs", None) is None:
            fzt, nt = self.fz.tree, self.tree

            def sources(files):
                return {p for p in files if p.endswith(SOURCE_EXT) or p.rsplit("/", 1)[-1] == lint.CMAKE}

            paths = set(fzt.files(*BUILD_INPUTS)) | sources(nt.files(*BUILD_INPUTS))
            for root in BUILD_INPUTS_IF_PRESENT:
                mine = nt.files(root)
                if mine:
                    paths |= sources(mine) | set(fzt.files(root)) | ({root} & set(mine))
            self._inputs = paths
        return self._inputs

    def include_problem(self, path, txt):
        """Why an #include line that enters the tree is not declared, or None.  The directive
        names its header literally, stays inside the include directories (no `..`, no absolute
        path) and resolves there as the compiler resolves it (a quoted name found beside the
        including file must be that same file); the file it reaches is a header (.hpp or .h
        under include/ or third_party/) the frozen tree has, or one of the step's two new
        headers, so never a source, a test (the hand-written engine specification test above
        all) or an unread file; a name the tree does not hold is a C++ standard library header
        (in angle brackets) or a library header the frozen tree already includes by that
        spelling."""
        q, name = lint.include_target(txt)
        if q is None:
            return "names its header through a macro"
        parts = name.replace("\\", "/").split("/")
        if ".." in parts or name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name):
            return "leaves the include directories (`..` or an absolute path)"
        beside = lint.resolve_include(self.tree, path, q, name) if q == '"' else None
        found = lint.resolve_include(self.tree, path, "<", name)
        if beside is not None and beside != found:
            return f"reaches {beside}, beside the including file, not a header of the include directories"
        if found is None:
            if not (q == "<" and name in lint.STD_HEADERS) and (q, name) not in self.fz_include_vocabulary():
                return ("names a header the tree does not hold, not a standard library header, and the frozen "
                        "tree never includes it by that spelling")
            return None
        if not lint.is_header_path(found):
            return f"reaches {found}, which is not a header (.hpp or .h under include/ or third_party/)"
        if self.fz.tree.read(found) is None and found not in (RUN_HPP, lint.EPOCH_HPP):
            return f"reaches {found}, a header neither the frozen tree nor the step declares"
        return None

    def fz_include_vocabulary(self):
        """{(quote, name)} of every #include directive in the frozen tree's src/, include/ and
        tests/: the library and system headers the project already reads."""
        cache = self.fz.__dict__
        if "_inc_vocab" not in cache:
            vocab = set()
            for p in self.fz.tree.files("src", "include", "tests"):
                for _, q, name in lint.file_includes(self.fz.tree, p):
                    if q is not None:
                        vocab.add((q, name))
            cache["_inc_vocab"] = vocab
        return cache["_inc_vocab"]

    def cmake_build_list(self):
        """The step's build-list edit, exactly (spec 4.1): CMakeLists.txt names each of the
        step's new sources once, as a bare source line in its target's list
        (src/core/app/ApplicationNetworkEpoch.cpp and src/core/app/NetworkRun.cpp in
        add_library(gnss_core ...), the contract test and, when it exists, the hand-written
        engine specification test in add_executable(gnss_tests ...)), and nowhere else.  Only
        those lines are claimed, so any other changed line of the file (a per-source compile
        option or definition, a forced include, a test source moved into gnss_core) is
        unaccounted in V1e.  On an empty conversion the file names none of them."""
        R = self.R
        text = self.tree.read(lint.CMAKE)
        if text is None:
            R.fail("V1e", f"{lint.CMAKE}: missing")
            return
        cmds = lint.cmake_commands(text)
        lines = text.split("\n")
        for path, cmd, target in CMAKE_ENTRIES:
            base = pathlib.PurePosixPath(path).name
            raw = [k for k, ln in enumerate(lines, 1) if base in ln]
            hits = [(name, args, a, ln) for name, args, _ in cmds for a, ln in args if base in a]
            want = 1 if self.epoch and (path != ENGINE_SPEC_TEST or self.tree.read(path) is not None) else 0
            if len(raw) != want or len(hits) != want:
                R.fail("V1e", f"{lint.CMAKE}: names {path} on {len(raw)} line(s) ({', '.join(map(str, raw[:5]))}); "
                              f"the step lists it {want} time(s), as a source of {target}")
                continue
            for name, args, a, ln in hits:
                if name != cmd or not args or args[0][0] != target or a != path or lines[ln - 1].strip() != path:
                    where = f"{name}({args[0][0] if args else ''} ...)"
                    R.fail("V1e", f"{lint.CMAKE}:{ln}: `{lines[ln - 1].strip()[:90]}` in {where}; the step "
                                  f"lists {path} as a bare source line of {cmd}({target} ...)")
                    continue
                self.claim(lint.CMAKE, ln, ln)

    def line_tokens(self, T, ln):
        """Indices of the tokens of T that lie on source line @p ln (a token spanning lines,
        a continued directive or a raw string, on each of them)."""
        idx = self._line_index.get(T.path)
        if idx is None or idx[0] is not T:
            m = collections.defaultdict(list)
            for j in range(T.n):
                first = T.line(j)
                for x in range(first, first + T.t(j).count("\n") + 1):
                    m[x].append(j)
            idx = self._line_index[T.path] = (T, m)
        return idx[1].get(ln, [])

    def merged_tclaims(self, path):
        spans = self._merged.get(path)
        if spans is None:
            spans = []
            for a, b in sorted(self.tclaims.get(path, ())):
                if spans and a <= spans[-1][1]:
                    spans[-1][1] = max(spans[-1][1], b)
                else:
                    spans.append([a, b])
            self._merged[path] = spans
        return spans

    def unclaimed_tokens(self, T, ln):
        spans = self.merged_tclaims(T.path)
        starts = [s[0] for s in spans]
        out = []
        for j in self.line_tokens(T, ln):
            k = bisect.bisect_right(starts, T[j].pos) - 1
            if k < 0 or T[j].end > spans[k][1]:
                out.append(T.t(j))
        return out

    def claimed(self, path, ln, txt, T=None, textual=True):
        """Whether changed line @p ln of @p path is accounted for: by a line claim, by token
        claims covering each of its tokens, or (@p textual) by its text alone (textual_claim).
        A lone brace's neighbour must be claimed otherwise than by its text (v1e)."""
        if textual and T is not None and self.textual_claim(T, ln, txt):
            return True
        if any(a <= ln <= b for a, b in self.claims.get(path, ())):
            return True
        return T is not None and bool(self.line_tokens(T, ln)) and not self.unclaimed_tokens(T, ln)

    def fclaimed(self, path, ln, txt):
        FT = self.fz.tree.tokens(path) if path.endswith(CPP_EXT) else None
        if FT is not None and self.textual_claim(FT, ln, txt):
            return True
        return any(a <= ln <= b for a, b in self.frozen_claims.get(path, ()))

    _FWD_DECL = re.compile(r"^(?:namespace\s+\w+\s*\{\s*)?(?:struct|class)\s+(\w+)\s*;(?:\s*\})?$")

    def textual_claim(self, T, ln, txt):
        """Line @p ln of T accounted for by its text alone: an #include at file scope (every
        brace closed; include_problem reads its target), or a forward declaration `struct X;`
        / `class X;` (or `namespace N { class X; }`) at namespace scope that names a class the
        frozen tree or one of the step's new headers already names (class_names).  Inside a
        function body, a class body or any other block such a line is code like any other:
        `struct X;` is a statement there (the whole substatement of a braceless `if`), and an
        #include inside a body pastes a header's text into it."""
        s = txt.strip()
        toks = self.line_tokens(T, ln)
        if not toks:
            return False
        depth, nonns = brace_scope(T)
        if pp_directive(s) == "include":
            return all(T.k(j) == "pp" for j in toks) and all(depth[j] == 0 for j in toks)
        m = self._FWD_DECL.match(s)
        if m is None or any(nonns[j] for j in toks):
            return False
        return m.group(1) in self.class_names()

    def class_names(self):
        """Every name the frozen tree's src/, include/ and tests/ use as a class name (`struct
        X` or `class X` before `{`, `;`, `:` or `final`), and every class a changed C++ file of
        the candidate defines (a definition V1e must account for in its turn: the step's new
        types, NetworkPostFitHandoff in a header the base has)."""
        cache = self.fz.__dict__
        if "_class_names" not in cache:
            pat = re.compile(r"\b(?:struct|class)\s+(\w+)\s*(?:final\b\s*)?[;{:]")
            names = set()
            for p in self.fz.tree.files("src", "include", "tests"):
                if p.endswith(CPP_EXT):
                    names.update(pat.findall(lint.strip_comments(self.fz.tree.read(p) or "")))
            cache["_class_names"] = names
        if getattr(self, "_class_names", None) is None:
            names = set(cache["_class_names"])
            for h in sorted(self.changed_files()):
                if h.endswith(CPP_EXT):
                    names.update(re.findall(r"\b(?:struct|class)\s+(\w+)\s*(?:final\b\s*)?[{:]",
                                            lint.strip_comments(self.tree.read(h) or "")))
            self._class_names = names
        return self._class_names

    def reading(self, path, ft, nt, cpp):
        """V1e: the changed file @p path holds nothing the reading layer reads otherwise than
        the compiler (nom_cpp.reading_problems) beyond what its frozen text holds."""
        flines = (ft or "").split("\n")
        have = collections.Counter((why, flines[ln - 1].strip()) for ln, why in reading_problems(ft or "", cpp))
        lines = nt.split("\n")
        for ln, why in reading_problems(nt, cpp):
            key = (why, lines[ln - 1].strip())
            if have[key] > 0:
                have[key] -= 1
                continue
            shown = lines[ln - 1].strip().encode("unicode_escape").decode("ascii")
            self.R.fail("V1e", f"{path}:{ln}: {why}; the checks would read the file otherwise than the "
                               f"compiler: `{shown[:90]}`")

    def cond_groups(self, path, T, ft, changed):
        """V1e: a conditional group of #include lines (lint.cond_include_groups) that enters
        the tree or whose directive changed holds no #include of a header the frozen file
        includes: `#if 0` and `#endif` around a frozen #include remove it as surely as
        deleting the line."""
        frozen = {lint.include_target(t) for _, t in _code_lines(ft or "") if pp_directive(t) == "include"}
        for directives, incs in lint.cond_include_groups(T):
            if not any(ln in changed for ln in directives):
                continue
            for ln, target in incs:
                if target in frozen:
                    self.R.fail("V1e", f"{path}:{ln}: `#include {lint.include_spelling(*target)}`, which the "
                                       f"frozen file includes, inside the conditional group at line "
                                       f"{directives[0]} that enters the tree or changes")


# ---------------------------------------------------------------------------------------------
# Helpers of the checks
# ---------------------------------------------------------------------------------------------
_BRACE_SCOPE = {}


def brace_scope(T):
    """(depth, nonns): per token of T, the number of braces open at it and how many of them
    are not a namespace's (`namespace [N[::M]] {`, `inline namespace N {`) or an
    `extern "C"` block's; a token with nonns 0 is at namespace scope."""
    hit = _BRACE_SCOPE.get(id(T))
    if hit is not None and hit[0] is T:
        return hit[1], hit[2]
    depth, nonns = [0] * T.n, [0] * T.n
    stack, other = [], 0
    for j in range(T.n):
        t, k = T.t(j), T.k(j)
        if k == "op" and t == "}" and stack:
            other -= 0 if stack.pop() else 1
        depth[j], nonns[j] = len(stack), other
        if k == "op" and t == "{":
            q = j - 1
            while q >= 0 and ((T.k(q) == "id" and T.t(q) != "namespace") or T.t(q) == "::"):
                q -= 1
            ns = T.t(q) == "namespace" or (T.k(j - 1) == "str" and T.t(j - 2) == "extern")
            stack.append(ns)
            other += 0 if ns else 1
    if len(_BRACE_SCOPE) > 256:
        _BRACE_SCOPE.clear()
    _BRACE_SCOPE[id(T)] = (T, depth, nonns)
    return depth, nonns


def _drop_nested(subs):
    subs = sorted(subs, key=lambda x: (x.a, -(x.b - x.a)))
    out, end = [], -1
    for x in subs:
        if x.a >= end:
            out.append(x)
            end = x.b
    return out


def _split_top(T, a, b):
    """Argument spans of tokens [a, b) split at top-level commas."""
    out, start, d = [], a, 0
    for j in range(a, b):
        v = T.t(j)
        if T.k(j) == "op" and v in "([{":
            d += 1
        elif T.k(j) == "op" and v in ")]}":
            d -= 1
        elif d == 0 and v == ",":
            out.append((start, j))
            start = j + 1
    if start < b:
        out.append((start, b))
    return out


def _mem_initializers(T, f):
    """{member: the single name it is initialized from} of a constructor's initializer list."""
    out = {}
    k = f.paren[1] + 1
    if T.t(k) != ":":
        return out
    k += 1
    end = f.body[0] if f.body else T.n
    while k < end:
        if T.k(k) == "id" and T.t(k + 1) in ("(", "{"):
            c = T.close(k + 1)
            inner = [T.t(q) for q in range(k + 2, c)]
            out[T.t(k)] = inner[0] if len(inner) == 1 else " ".join(inner)
            k = c + 1
            continue
        k += 1
    return out


def find_defs(tree, name):
    """Free-function definitions of @p name under src/ and include/."""
    out = []
    for p in tree.files("src", "include"):
        if not p.endswith((".cpp", ".hpp", ".h")):
            continue
        text = tree.read(p)
        if not text or name not in text:
            continue
        out += [f for f in tree.index(p).funcs if f.name == name and f.is_def and not f.cls]
    return out


def find_decls(tree, name):
    out = []
    for p in tree.files("include"):
        text = tree.read(p)
        if not text or name not in text:
            continue
        out += [f for f in tree.index(p).funcs if f.name == name and not f.is_def]
    return out


def struct_span(tree, path, name):
    """(first line, last line) of the definition of struct / class @p name in @p path."""
    T = tree.tokens(path)
    if T is None:
        return None
    for i in range(T.n - 2):
        if T.t(i) in ("struct", "class") and T.t(i + 1) == name:
            k = i + 2
            while k < T.n and T.t(k) not in ("{", ";"):
                k += 1
            if T.t(k) == "{":
                return T.line(i), T.line(T.close(k) + 1)
    return None


def _init_captures(T, closure):
    """{name: initializer text} of a closure declaration's init-captures."""
    out = {}
    a, b = closure.init
    if T.t(a) != "[":
        return out
    c = T.close(a)
    for s, e in _split_top(T, a + 1, c):
        if T.t(s + 1) == "=":
            out[T.t(s)] = T.join(s + 2, e)
    return out


def _mem_init_args(T, f):
    """{member: [argument spans]} of a constructor's initializer list."""
    out = {}
    k = f.paren[1] + 1
    if T.t(k) != ":":
        return out
    k += 1
    end = f.body[0] if f.body else T.n
    while k < end:
        if T.k(k) == "id" and T.t(k + 1) in ("(", "{"):
            c = T.close(k + 1)
            out[T.t(k)] = _split_top(T, k + 2, c)
            k = c + 1
            continue
        k += 1
    return out


def _expr_type(T, a, b, froot, fT, knobs):
    """(type text, how) of the expression [a, b) an engine argument field holds, or (None,
    why): `static_cast<T>(...)` gives T; a frozen driver local, its declared type without
    const; `ctx.knobs.X`, the knob's declared type; `c ? x : y`, the type of a `T{}` branch."""
    while T.t(a) == "(" and T.match[a] == b - 1:
        a, b = a + 1, b - 1
    if T.t(a) == "static_cast" and T.t(a + 1) == "<":
        m = match_angle(T, a + 1, b)
        if m is not None and T.t(m + 1) == "(" and T.close(m + 1) == b - 1:
            return T.join(a + 2, m), "static_cast"
    if b == a + 1 and T.k(a) == "id" and T.t(a) in froot:
        d = froot[T.t(a)]
        t = fT.join(*d.type_span)
        return (t[len("const "):] if t.startswith("const ") else t), f"the frozen local {T.t(a)}"
    if b == a + 5 and T.join(a, a + 4) == "ctx.knobs.":
        k = knobs.get(T.t(a + 4))
        if k is not None:
            return k["type"], f"the knob {T.t(a + 4)}"
    q = next((j for j in range(a, b) if T.t(j) == "?" and T.k(j) == "op"), None)
    if q is not None:
        c = next((j for j in range(q + 1, b) if T.t(j) == ":" and T.k(j) == "op"), None)
        for x, y in ((c + 1, b), (q + 1, c)) if c is not None else ():
            if T.t(y - 1) == "}" and T.t(y - 2) == "{":
                return T.join(x, y - 2), "the conditional's T{} branch"
    return None, "no rule"


def closure_captures(rn, fd):
    """({captured name: by reference}, mutable) of the frozen driver closure @p fd: its
    init-captures, and every name its body reads that is declared outside it (a `[&]` default
    or an explicit `&name` capture is by reference, `[=]` or a plain name by value)."""
    T, scopes = rn.T, rn.scopes
    ls = fd.lambda_scope
    lam = _lambda_at(T, fd.init[0], fd.init[1])
    cc, po, bo = lam
    bc = T.close(bo)
    mutable = any(T.t(j) == "mutable" for j in range((T.close(po) + 1) if po is not None else cc + 1, bo))
    items = _split_top(T, fd.init[0] + 1, cc)
    default_ref = any(T.join(a, b) == "&" for a, b in items)
    explicit = {}
    for a, b in items:
        txt = T.join(a, b)
        if txt in ("&", "="):
            continue
        k = a + 1 if T.t(a) == "&" else a
        explicit[T.t(k)] = T.t(a) == "&"
    out = {}
    for d in ls.decls:
        if d.kind == "capture":
            out[d.name] = d.is_ref
    for j in range(bo + 1, bc):
        if T.k(j) != "id" or T.t(j) in KEYWORDS or T.t(j - 1) in (".", "->", "::") or T.t(j + 1) == "::":
            continue
        d = scopes.lookup(T.t(j), j)
        if d is None or d.kind == "type" or d.name in out:
            continue
        sc = d.scope
        while sc is not None and sc is not ls:
            sc = sc.parent
        if sc is ls:
            continue                       # declared inside the closure
        out[d.name] = explicit.get(d.name, default_ref)
    return out, mutable


def _expand_lambda_calls(ev):
    return [("call", lint.EPOCH_FN) if e == ("call", LAMBDA_NAME) else e for e in ev]


def _preflight(T, a, b, outs, declared_only=False):
    """Names among run, ep and @p outs that the body [a, b) declares (any depth) or, unless
    @p declared_only, reads as a free identifier."""
    names = {"run", "ep"} | set(outs)
    found = set()
    for j in ids_in(T, a, b):
        if T.t(j) in names:
            if declared_only:
                p = T.t(j - 1)
                if p in ("&", "*", ">", ">>") or (T.k(j - 1) == "id" and p not in ("return", "case")):
                    found.add(T.t(j))
            else:
                found.add(T.t(j))
    return found


def _code_lines(text, cpp=True):
    """(line, text) of the non-blank lines of @p text: C++ comments removed when @p cpp, else
    every line as written (a `//` or `/*` in CMake is text the build reads, not a comment)."""
    out = []
    for k, ln in enumerate((lint.strip_comments(text) if cpp else text).split("\n"), 1):
        if ln.strip():
            out.append((k, ln.rstrip()))
    return out


def _solve_declared(a, b, R, replay=True, card=False):
    """The solve's declared body edits (spec 3.4): the replay's two callback lines become
    direct calls (WCF's resolves counter kept), the three epilogue lines write result.handoff."""
    fa = [ln for ln in a]
    out_a, out_b, edited = [], list(b), []
    hits = sum(1 for ln in fa if re.match(r"^result\.(clock_refloat_sats|retro_flag_sats|"
                                           r"n_arc_refloat_epoch)\b", ln[1].strip()))
    if hits != 3:
        R.fail("V1b", f"networkSolveGlue: {hits} frozen epilogue lines of the hand-off fields, 3 expected")
    for k, ln in enumerate(fa):
        t = ln[1].strip()
        if replay and t in ("buildEnsembleGauges();", "buildCarryDdPriors();"):
            builder = "network" + t[0].upper() + t[1:t.index("(")]
            args = "(run, ep)" if "Ensemble" in builder or card else "(run, ep, dd_build, funnel)"
            out_a.append([ln[0], ln[1].replace(t, builder + args + ";")] + ln[2:])
            edited.append(k)
            continue
        m = re.match(r"^result\.(clock_refloat_sats|retro_flag_sats|n_arc_refloat_epoch)\b", t)
        if m:
            out_a.append([ln[0], ln[1].replace("result.", "result.handoff.", 1)] + ln[2:])
            edited.append(k)
            continue
        out_a.append(ln)
    return out_a, out_b, edited


def struct_members(tree, name):
    """{member: {'type', 'ops', 'init', 'line'}} of struct / class @p name defined under
    include/, in declaration order; None when no definition is found."""
    pat = re.compile(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b[^;{]*\{")
    for p in [lint.EPOCH_HPP] + [x for x in tree.files("include") if x.endswith((".hpp", ".h"))]:
        text = tree.read(p)
        if not text or name not in text or not pat.search(text):
            continue
        T = tree.tokens(p)
        for i in range(T.n - 2):
            if T.t(i) in ("struct", "class") and T.t(i + 1) == name:
                k = i + 2
                while k < T.n and T.t(k) not in ("{", ";"):
                    k += 1
                if T.t(k) != "{":
                    continue
                return _members_of(T, k, T.close(k))
    return None


def struct_bases(tree, name):
    """The class names in the base clause of struct / class @p name (as struct_members finds
    it), so a scan over its members also walks what it inherits."""
    pat = re.compile(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b[^;{]*\{")
    for p in [lint.EPOCH_HPP] + [x for x in tree.files("include") if x.endswith((".hpp", ".h"))]:
        text = tree.read(p)
        if not text or name not in text or not pat.search(text):
            continue
        T = tree.tokens(p)
        for i in range(T.n - 2):
            if T.t(i) in ("struct", "class") and T.t(i + 1) == name:
                k = i + 2
                while k < T.n and T.t(k) not in ("{", ";"):
                    k += 1
                if T.t(k) != "{":
                    continue
                return [T.t(j) for j in range(i + 2, k) if T.k(j) == "id"
                        and T.t(j) not in ("public", "private", "protected", "virtual", "final")
                        and T.t(j + 1) != "::"]
    return []


def _members_of(T, a, b):
    out = collections.OrderedDict()
    i = a + 1
    while i < b:
        e = statements(T, i, b)[0][1] if i < b else b
        t = T.t(i)
        if t in ("public", "private", "protected") and T.t(i + 1) == ":":
            i += 2
            continue
        if t in ("using", "static_assert", "friend", "enum", "template", "typedef", "static") or T.k(i) == "pp":
            i = e
            continue
        if t in ("struct", "class") and any(T.t(q) == "{" for q in range(i, e)):
            i = e
            continue
        is_func = any(T.t(q) == "(" and T.k(q - 1) == "id" and T.t(q - 1) not in KEYWORDS
                      and not any(T.t(r) == "=" for r in range(i, q)) for q in range(i, e)) and \
            not any(T.t(q) == "{" and T.k(q - 1) == "id" for q in range(i, e))
        ds = None if is_func else parse_declaration(T, i, e)
        if ds and T.t(e - 1) in (";", "}"):
            for d in ds:
                if d.kind in ("var", "closure"):
                    init = T.norm(*d.init) if d.init else ""
                    if d.init_kind == "{" and not init:
                        init = "{}"
                    out[d.name] = {"type": T.join(*d.type_span), "ops": d.ops, "init": init,
                                   "line": d.line}
        i = e
    return out


def file_items(T, a=0, b=None, ns=(), funcs=None):
    """The top-level items of tokens [a, b) of a file, namespace bodies entered: a list of
    (kind, start, end, namespace path, name), kind one of
      pp        a preprocessor directive (one token);
      ns        `namespace X {` (the tokens up to its brace), then the body's items, then
      nsclose   its closing brace;
      fwd       `struct X;` / `class X;`;
      struct    `struct X ... { ... };` (name X; the tokens between X and `{` are its base
                clause, see class_parts);
      fdecl     a function declaration (FileIndex), up to its `;`;
      fdef      a function definition (FileIndex), up to its body's `}`;
      stmt      anything else, up to its `;` at bracket depth 0 (a static_assert, an alias, a
                variable), or a macro invocation with a brace body (`TEST(...) { ... }`).
    No item is skipped, so a check that accepts items by kind and name accepts nothing else."""
    b = T.n if b is None else b
    if funcs is None:
        idx = FileIndex(T)
        funcs = {}
        for f in idx.funcs:
            funcs.setdefault(decl_start(T, f), f)
    out = []
    i = a
    while i < b:
        t, k = T.t(i), T.k(i)
        if k == "pp":
            out.append(("pp", i, i + 1, ns, ""))
            i += 1
            continue
        if t == "namespace":
            j = i + 1
            while j < b and T.t(j) not in ("{", ";", "="):
                j += 1
            if T.t(j) == "{":
                c = T.close(j)
                name = T.join(i + 1, j)
                out.append(("ns", i, j + 1, ns, name))
                out += file_items(T, j + 1, c, ns + (name,), funcs)
                out.append(("nsclose", c, c + 1, ns, name))
                i = c + 1
                continue
        if t in ("struct", "class") and T.k(i + 1) == "id":
            if T.t(i + 2) == ";":
                out.append(("fwd", i, i + 3, ns, T.t(i + 1)))
                i += 3
                continue
            j = i + 2
            while j < b and T.t(j) not in ("{", ";", "("):
                j += 1
            if T.t(j) == "{":
                c = T.close(j)
                end = c + 2 if T.t(c + 1) == ";" else c + 1
                out.append(("struct", i, end, ns, T.t(i + 1)))
                i = end
                continue
        f = funcs.get(i)
        if f is not None:
            end = decl_end(T, f)
            out.append(("fdef" if f.is_def else "fdecl", i, end, ns, f.name))
            i = end
            continue
        j = i
        while j < b:
            x = T.t(j)
            if T.k(j) == "op" and x in ("(", "["):
                j = T.close(j) + 1
                continue
            if T.k(j) == "op" and x == "{":
                c = T.close(j)
                if T.t(j - 1) == ")" and T.k(i) == "id" and T.t(i + 1) == "(" and T.close(i + 1) == j - 1:
                    j = c + 1          # a macro invocation with a body: TEST(...) { ... }
                    break
                j = c + 1
                continue
            if x == ";":
                j += 1
                break
            j += 1
        out.append(("stmt", i, j, ns, T.t(i)))
        i = max(j, i + 1)
    return out


def class_parts_anywhere(tree, name):
    """class_parts of @p name in the first header under include/ that defines it."""
    pat = re.compile(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b[^;{]*\{")
    for p in [x for x in tree.files("include") if x.endswith((".hpp", ".h"))]:
        text = tree.read(p)
        if text and name in text and pat.search(text):
            cp = class_parts(tree, p, name)
            if cp is not None:
                return cp
    return None


def class_parts(tree, path, name):
    """The body of struct / class @p name defined in @p path, statement by statement:
    {'T', 'open', 'close', 'members': OrderedDict(name -> data member), 'funcs': [member
    function], 'others': [(first, end, text)], 'base': the text between the name and `{` (a
    base clause, `final`; empty for a plain `struct X {`), 'end': one past the definition's
    `;`}, or None.  A data member is {'type', 'ops', 'init', 'init_kind', 'start', 'end',
    'first', 'last'} (token range and lines of its statement); a member function is parsed by
    _member_function; `others` holds what is neither (static members, using, static_assert,
    friend, nested types)."""
    T = tree.tokens(path)
    if T is None:
        return None
    for i in range(T.n - 2):
        if T.t(i) in ("struct", "class") and T.t(i + 1) == name and T.t(i - 1) not in ("enum", "friend"):
            k = i + 2
            while k < T.n and T.t(k) not in ("{", ";", "("):
                k += 1
            if T.t(k) != "{":
                continue
            c = T.close(k)
            out = {"T": T, "head": i, "open": k, "close": c, "members": collections.OrderedDict(),
                   "funcs": [], "others": [], "base": T.join(i + 2, k),
                   "end": c + 2 if T.t(c + 1) == ";" else c + 1}
            j = k + 1
            while j < c:
                if T.t(j) in ("public", "private", "protected") and T.t(j + 1) == ":":
                    j += 2
                    continue
                e, kind = _class_stmt(T, j, c)
                if kind == "func":
                    out["funcs"].append(_member_function(T, j, e))
                elif T.t(j) in ("using", "static_assert", "friend", "enum", "template", "typedef",
                                "static", "struct", "class") or T.k(j) == "pp":
                    out["others"].append((j, e, T.join(j, e)))
                else:
                    # A reference member has no initializer: read the statement as a range
                    # declaration, which admits one.
                    ds = parse_declaration(T, j, e) or parse_declaration(T, j, e, "range") or []
                    if not ds:
                        out["others"].append((j, e, T.join(j, e)))
                    for d in ds:
                        init = T.norm(*d.init) if d.init else ""
                        if d.init_kind == "{" and not init:
                            init = "{}"
                        out["members"][d.name] = {"type": T.join(*d.type_span), "ops": d.ops,
                                                  "init": init, "init_kind": d.init_kind, "start": j,
                                                  "end": e, "first": T.line(j), "last": T.line(e - 1)}
                j = e
            return out
    return None


def _class_stmt(T, i, b):
    """(end, kind) of the class-body statement at token @p i: kind `func` for a member
    function (declared, defaulted, deleted or defined in place, a constructor's member
    initializers included), `data` otherwise (it ends at its `;`)."""
    j = i
    while j < b:
        x = T.t(j)
        if x == "operator":
            j += 1
            if T.t(j) == "(" and T.t(j + 1) == ")":
                j += 2
            while j < b and T.t(j) != "(":
                j += 1
            return _func_end(T, j, b), "func"
        if x == "(" and T.k(j - 1) == "id" and T.t(j - 1) not in KEYWORDS:
            return _func_end(T, j, b), "func"
        if x == "<" and T.k(j - 1) == "id":
            m = match_angle(T, j, b)
            j = m + 1 if m is not None else j + 1
            continue
        if x in ("=", "{", ";", ":"):
            k = j
            while k < b and T.t(k) != ";":
                k = T.close(k) + 1 if T.t(k) in ("(", "[", "{") else k + 1
            return min(k + 1, b), "data"
        if x == "[":
            j = T.close(j) + 1
            continue
        j += 1
    return b, "data"


def _func_end(T, po, b):
    """One past the member function whose parameter list opens at @p po: its `;` (a
    declaration, `= default`, `= delete`) or its body's `}` (and a `;` right after it)."""
    k = T.close(po) + 1
    while k < b:
        x = T.t(k)
        if x == ";":
            return k + 1
        if x == ":":
            k += 1
            while k < b and T.t(k) != "{":
                if T.t(k) in ("(", "{"):
                    break
                k += 1
            while k < b and T.t(k) in ("(", "{"):
                c = T.close(k)
                k = c + 1
                if T.t(k) == ",":
                    k += 1
                    while k < b and T.t(k) not in ("(", "{"):
                        k += 1
                    continue
                break
            continue
        if x == "{":
            c = T.close(k)
            return c + 2 if T.t(c + 1) == ";" else c + 1
        if x in ("(", "["):
            k = T.close(k) + 1
            continue
        k += 1
    return b


def _member_function(T, a, e):
    """A member function statement [a, e): {'name', 'start', 'end', 'po', 'pc', 'params',
    'deleted', 'const', 'inits' [(member, bracket, [argument spans], text)], 'body' (open,
    close) or None, 'text'}."""
    po = next(j for j in range(a, e) if T.t(j) == "(" and (T.k(j - 1) == "id" or T.t(j - 1) != "operator"))
    name = T.t(po - 1)
    if T.t(a) == "operator" or "operator" in [T.t(j) for j in range(a, po)]:
        q = max(j for j in range(a, po) if T.t(j) == "operator")
        name = T.join(q, po)
    pc = T.close(po)
    out = {"name": name, "start": a, "end": e, "po": po, "pc": pc,
           "params": parse_params(T, po)[0], "deleted": T.join(max(a, e - 3), e) == "=delete;",
           "const": False, "inits": [], "body": None, "text": T.join(a, e)}
    k = pc + 1
    while k < e and T.t(k) in ("const", "noexcept", "override", "final"):
        out["const"] = out["const"] or T.t(k) == "const"
        k += 1
    if T.t(k) == ":":
        k += 1
        while k < e and T.t(k) != "{" or (k < e and T.t(k) == "{" and T.k(k - 1) == "id"):
            if T.k(k) == "id" and T.t(k + 1) in ("(", "{"):
                c = T.close(k + 1)
                out["inits"].append((T.t(k), T.t(k + 1), _split_top(T, k + 2, c), T.join(k, c + 1)))
                k = c + 1
                continue
            k += 1
    if k < e and T.t(k) == "{":
        out["body"] = (k, T.close(k))
    return out


def fold_problem(node):
    """None when @p node is in the fold grammar of spec 3.2, else why not."""
    if isinstance(node, Ref):
        for k, seg in enumerate(node.chain):
            if seg.startswith("[") or ("(" in seg and seg not in (".get()", ".x_emit()")):
                return f"`{seg}` in a member chain"
            if seg in (".get()", ".x_emit()") and k != len(node.chain) - 1:
                return f"`{seg}` before the end of a member chain"
        return None
    if isinstance(node, Copy):
        return fold_problem(node.inner)
    if isinstance(node, Lit):
        return None
    if isinstance(node, Cond):
        c = node.c.args[0] if isinstance(node.c, Expr) and node.c.op == "!" else node.c
        for part in (c, node.a, node.b):
            if not isinstance(part, (Ref, Copy, Lit)):
                return "a conditional over something other than member reads and literals"
            p = fold_problem(part)
            if p:
                return p
        return None
    if isinstance(node, CallNode):
        if node.fn != "TimeUtils::continuousGpsSeconds":
            return f"a call of {node.fn}"
        for a in node.args:
            if not isinstance(a, (Ref, Copy)) or fold_problem(a):
                return "a call argument that is not a member read"
        return None
    if isinstance(node, Opaque):
        return f"not resolved ({node.why})"
    return "an operator expression"


def doc_before(T, start):
    """(text, first line) of the comment block directly above token @p start, or None."""
    src = T.source
    pos = T[start].pos
    line_start = src.rfind("\n", 0, pos) + 1
    k = line_start
    lines = []
    first = None
    while k > 0:
        prev_end = k - 1
        prev_start = src.rfind("\n", 0, prev_end) + 1
        ln = src[prev_start:prev_end].strip()
        if ln.startswith(("///", "//", "*", "/**", "/*")) or ln.endswith("*/"):
            lines.insert(0, ln)
            first = src.count("\n", 0, prev_start) + 1
            k = prev_start
            continue
        break
    return ("\n".join(lines), first) if lines else None


def doc_words(text):
    """A comment block's words: the comment markers of each line (/**, */, ///, //, a leading
    *) removed and the whitespace collapsed, so a re-wrapped paragraph compares equal."""
    out = []
    for ln in text.split("\n"):
        ln = ln.strip()
        ln = re.sub(r"^(/\*\*|/\*|///|//|\*/|\*)", "", ln)
        ln = re.sub(r"\*/$", "", ln)
        out.append(ln)
    return " ".join(" ".join(out).split())


def params_doc(text):
    """[(name, description)] of the @param entries of a Doxygen block's text."""
    t = doc_words(text)
    out = []
    for m in re.finditer(r"@param\s+(\w+)\s+(.*?)(?=@param|@return|@brief|$)", t, re.S):
        out.append((m.group(1), m.group(2).strip()))
    return out


# ---------------------------------------------------------------------------------------------
# Negative controls (spec 5.7)
# ---------------------------------------------------------------------------------------------
class Control:
    """A pre-registered negative control: @p build mutates the candidate (an overlay) and
    returns (overrides, what) or (None, why) when its target is not in the tree; @p due says
    whether the stage being verified must have that target (a seam it converts, the epoch
    function, ...).  A due control whose target is missing does not fire: it is reported as
    MISSING and fails the run, so a control cannot fall silent at a later base."""

    def __init__(self, cid, check, what, build, due=lambda v: True):
        self.cid, self.check, self.what, self.build, self.due = cid, check, what, build, due


def _replace(text, a, b, new):
    return text[:a] + new + text[b:]


def ctl_n5(v):
    c, T = v.cand, v.cand.T
    if c.mode == "empty":
        s = c.stmts[0][0]
        line_start = T.source.rfind("\n", 0, T[s].pos) + 1
        ind = T.source[line_start:T[s].pos]
        return {T.path: _replace(T.source, line_start, line_start, ind + "auto stations = setup.stations;\n")}, \
            "a by-value `auto stations = setup.stations;` opens the lambda"
    for s, e in c.preamble:
        if T.join(s, e) == join_text("auto& stations = setup.stations;"):
            return {T.path: _replace(T.source, T[s].pos, T[e - 1].end, "auto stations = run.setup.stations;")}, \
                "the preamble copies stations"
    s = c.preamble[-1][1] if c.preamble else c.open + 1
    return {T.path: _replace(T.source, T[s].pos, T[s].pos, "auto stations = run.setup.stations;\n    ")}, \
        "the preamble copies stations"


def ctl_n6(v):
    c, T = v.cand, v.cand.T

    def holds(name):
        for s, e in c.stmts:
            if any(x.name == name for x in calls_in(T, s, e)) and T.t(s) == name:
                return s, e
        return None
    a, b = holds("networkIonoDiag"), holds("networkDdFrac")
    if not a or not b:
        return None, "the two calls are not statements of the epoch body"
    ta, tb = T.source[T[a[0]].pos:T[a[1] - 1].end], T.source[T[b[0]].pos:T[b[1] - 1].end]
    src = T.source
    src = src[:T[b[0]].pos] + ta + src[T[b[1] - 1].end:]
    src = src[:T[a[0]].pos] + tb + src[T[a[1] - 1].end:]
    return {T.path: src}, "networkIonoDiag and networkDdFrac swapped"


def ctl_n7(v):
    c, T = v.cand, v.cand.T
    if c.mode == "empty":
        k = next((k for k, (s, e) in enumerate(c.stmts) if any(x.name == lint.SOLVE for x in calls_in(T, s, e))), None)
        if k is None:
            return None, "no solve"
        s = c.stmts[k + 1][1]       # after the skip-epoch return
        return {T.path: _replace(T.source, T[s - 1].end, T[s - 1].end, "\n        present.clear();")}, \
            "`present.clear();` after the solve"
    k = next((k for k, (s, e) in enumerate(c.stmts) if T.join(s, min(e, s + 5)).startswith("ep.solve=")), None)
    if k is None:
        return None, "no `ep.solve = ...` statement"
    s = c.stmts[k][1]
    return {T.path: _replace(T.source, T[s - 1].end, T[s - 1].end, "\n    ep.pass_a.present.clear();")}, \
        "`ep.pass_a.present.clear();` after the solve"


def ctl_n8(v):
    c, T = v.cand, v.cand.T
    for s, e in c.stmts:
        txt = T.join(s, e)
        if "est_index_of" in txt and "-1" in txt and ("std::vector<int>" in txt):
            for j in range(s, e):
                if T.t(j) == "-" and T.t(j + 1) == "1":
                    return {T.path: _replace(T.source, T[j].pos, T[j + 1].end, "0")}, \
                        "est_index_of sized with 0 instead of -1"
    return None, "the est_index_of sizing statement is not in the epoch body"


def ctl_n10(v):
    c, T = v.cand, v.cand.T
    for j in range(c.open, c.close):
        if T.t(j) == "dt" and T.t(j - 1) == "double" and T.t(j - 2) == "const" and T.t(j + 1) == "=" \
                and "isfinite" in T.source[T[j].pos:T[j].pos + 200].split(";")[0]:
            e = j + 2
            while T.t(e) != ";":
                e += 1
            return {T.path: _replace(T.source, T[j + 2].pos, T[e - 1].end, "ep.clock.dt")}, \
                "the DD-carry predict's shadowing dt reads ep.clock.dt"
    return None, "the shadowing dt is not in the epoch body"


def ctl_n11(v):
    """A log line inside the engine spec's statements: the debug_r_scale warning of spec 5.7
    where the base still has it (it went with the lane layer otherwise), else a synthetic
    warning, so the control does not depend on what the REMOVE-key deletion keeps.  On an
    empty conversion it enters a cut range of runNetwork (between two of its statements), on
    an epoch tree networkEngineSpec."""
    c = v.cand
    rT = c.rn.T
    warn = None
    for s, e in c.rn.statements():
        if rT.t(s) == "if" and "debug_r_scale" in rT.join(s, e) and any(x.name == "log->warn" for x in calls_in(rT, s, e)):
            warn = (s, e)
    synthetic = 'log->warn("negative control N11: a log line in the engine spec");'
    wtext = rT.source[rT[warn[0]].pos:rT[warn[1] - 1].end] if warn else synthetic
    rsrc = (rT.source[:rT[warn[0]].pos] + rT.source[rT[warn[1] - 1].end:]) if warn else rT.source
    if c.mode == "empty":
        cfz = CandFz(v.tree)
        for d, sts in v.cut_statements(cfz):
            if len(sts) >= 2 and (warn is None or sts[1][0] < warn[0]):
                at = sts[1][0]
                src = rsrc if warn is None else rT.source
                pos = rT[at].pos
                out = src[:pos] + wtext + "\n    " + src[pos:]
                if warn is not None:      # drop the warning from its own place (after the insert)
                    ws, we = rT[warn[0]].pos + len(wtext) + 5, rT[warn[1] - 1].end + len(wtext) + 5
                    out = out[:ws] + out[we:]
                return {DRIVER_PATH: out}, \
                    f"{'the debug_r_scale warning' if warn else 'a synthetic warning'} inside the {d.name} cut range"
        return None, "no cut range of two statements"
    idx = v.tree.index(RUN_CPP)
    f = next((g for g in (idx.funcs if idx else []) if g.name == "networkEngineSpec" and g.is_def), None)
    if f is None:
        return None, "networkEngineSpec does not exist yet"
    ST = v.tree.tokens(RUN_CPP)
    ret = next((s for s, e in statements(ST, f.body[0] + 1, f.body[1]) if ST.t(s) == "return"), f.body[1])
    out = {RUN_CPP: _replace(ST.source, ST[ret].pos, ST[ret].pos, wtext + "\n    ")}
    if warn:
        out[DRIVER_PATH] = rsrc
    return out, f"{'the debug_r_scale warning' if warn else 'a synthetic warning'} cut into networkEngineSpec"


def ctl_n12(v):
    r = v.by_name.get("networkFixedCycles")
    if r is None:
        return None, "no networkFixedCycles row"
    T = v.tree.tokens(r.cpp)
    idx = v.tree.index(r.cpp)
    if "networkFixedCycles" in v.converted:
        f = next((g for g in idx.funcs if g.name == "networkFixedCycles" and g.is_def and lint.is_epoch_form(g)), None)
        if f is None:
            return None, "the converted networkFixedCycles definition is missing"
        p = f.params[1]
        return {r.cpp: _replace(T.source, T[p.name_tok].pos, T[p.name_tok].end, "ep")}, \
            "networkFixedCycles defined with `ep` as its epoch parameter's name"
    f = next((g for g in idx.funcs if g.name == "networkFixedCycles" and g.is_def), None)
    p = next((q for q in f.params if q.name == "se"), None) if f else None
    if p is None:
        return None, "networkFixedCycles' definition has no `se` parameter"
    return {r.cpp: _replace(T.source, T[p.name_tok].pos, T[p.name_tok].end, "ep")}, \
        "networkFixedCycles defined with a parameter named `ep` (empty-conversion form)"


def ctl_preamble_edit(seam, pname, frm, to, what):
    """A control that rewrites one binding line of a converted seam."""
    def f(v):
        if seam not in v.converted:
            return None, f"{seam} is not converted in this tree"
        for d in v.new_defs(seam):
            DT = v.tree.tokens(d.path)
            sp = lint.preamble_span(DT, d)
            span = sp if sp else (d.body[0] + 1, d.body[1])
            for s, e in statements(DT, *span):
                txt = DT.source[DT[s].pos:DT[e - 1].end]
                if pname in txt and frm in txt:
                    return {d.path: _replace(DT.source, DT[s].pos, DT[e - 1].end, txt.replace(frm, to, 1))}, what
        return None, f"no binding of {pname} found in {seam}"
    return f


def ctl_n9(v):
    for name in sorted(v.converted):
        if v.by_name[name].mech == ADAPTER:
            continue
        for d in v.new_defs(name):
            DT = v.tree.tokens(d.path)
            sp = lint.preamble_span(DT, d)
            if not sp:
                continue
            for j in range(sp[1], d.body[1]):
                if DT.t(j) == "+" and DT.k(j) == "op":
                    return {d.path: _replace(DT.source, DT[j].pos, DT[j].end, "-")}, f"one `+` in {name}'s body made `-`"
    return None, "no converted seam body"


def ctl_n13(v):
    if "networkClockQc" not in v.converted:
        return None, "networkClockQc is not converted"
    r = v.by_name["networkClockQc"]
    out = {}
    for path in (r.hpp, r.cpp):
        T = v.tree.tokens(path)
        f = next((g for g in v.tree.index(path).funcs if g.name == "networkClockQc" and lint.is_epoch_form(g)), None)
        if f is None:
            return None, "networkClockQc's epoch form is missing"
        out[path] = _replace(T.source, T[f.paren[1]].pos, T[f.paren[1]].pos, ", EpochPassA& pass_a")
    return out, "networkClockQc given EpochPassA& as a second output"


def ctl_n14(v):
    if lint.SOLVE not in v.converted:
        return None, "networkSolveGlue is not converted"
    T = v.tree.tokens(SOLVE_CPP)
    k = T.source.find("++ctx.counters.we_culprit_withhold.resolves;")
    if k < 0:
        return None, "the WCF resolves counter line is not in the solve"
    return {SOLVE_CPP: _replace(T.source, k, k + len("++ctx.counters.we_culprit_withhold.resolves;"), ";")}, \
        "the replay's WCF resolves counter dropped"


def ctl_n15(v):
    seam = "networkHandoverStransformBeforeRows"
    if seam not in v.converted:
        return None, f"{seam} is not converted"
    for d in v.new_defs(seam):
        DT = v.tree.tokens(d.path)
        k = DT.source.find("ep.se.time", DT[d.body[0]].pos, DT[d.body[1]].pos)
        if k >= 0:
            return {d.path: _replace(DT.source, k, k + len("ep.se.time"), "ep.solve.plan")}, \
                f"{seam} binds ep.solve.plan"
    return None, f"{seam}'s epoch form does not read ep.se.time"


def ctl_n16(v):
    c, T = v.cand, v.cand.T
    if c.mode != "epoch":
        return None, "no epoch function (ep.emitted does not exist)"
    st = next(((s, e) for s, e in c.stmts if T.join(s, min(e, s + 4)).startswith("ep.emitted=")), None)
    # The counter snapshot the VALUE_EQUAL expression reads, as the base spells it.
    snap = (SUBST.value_equal or "fc_n_emitted_before").split("!=")[-1].strip()
    before = next(((s, e) for s, e in c.stmts if re.search(r"\b" + snap + r"\b", T.join(s, e))
                   and T.t(s) == "const"), None)
    if st is None or before is None:
        return None, f"the ep.emitted store or {snap} is missing"
    text = T.source[T[st[0]].pos:T[st[1] - 1].end]
    src = T.source[:T[st[0]].pos] + T.source[T[st[1] - 1].end:]
    pos = T[before[1] - 1].end
    return {T.path: src[:pos] + "\n    " + text + src[pos:]}, "ep.emitted stored before networkEmitStore"


def ctl_n4(v):
    if "networkRhsAttribTrip" not in v.converted or lint.SOLVE not in v.converted:
        return None, "networkRhsAttribTrip and the solve are not both converted"
    T = v.tree.tokens(SOLVE_CPP)
    for c in calls_in(T, 0, T.n):
        if c.name == "networkRhsAttribTrip" and c.nargs > 2:
            return {SOLVE_CPP: _replace(T.source, T[c.open + 1].pos, T[c.close].pos, "run, ep")}, \
                "the solve's internal networkRhsAttribTrip call takes the epoch form"
    return None, "the solve's internal networkRhsAttribTrip call is not found"


def _epoch_text_edit(v, path, old, new, what):
    T = v.tree.read(path)
    if T is None or T.count(old) != 1:
        return None, f"`{old[:50]}` is not in {path} once"
    return {path: T.replace(old, new, 1)}, what


def ctl_r1(v):
    """The DD-carry predict's two reads of its own TOW-only `dt` rewritten to ep.clock.dt."""
    c = v.cand
    if c.mode != "epoch":
        return None, "no epoch function"
    T = c.T
    for x in calls_in(T, c.open + 1, c.close):
        if x.name.endswith("beginEpoch") and x.args and any(T.t(j) == "dt" for j in range(*x.args[0])):
            a, b = x.args[0]
            src = T.source
            for j in sorted((j for j in range(a, b) if T.t(j) == "dt" and T.t(j - 1) != "."), reverse=True):
                src = src[:T[j].pos] + "ep.clock.dt" + src[T[j].end:]
            return {T.path: src}, "carry_driver.beginEpoch reads ep.clock.dt for its own dt"
    return None, "the DD-carry predict's beginEpoch call is not in the epoch body"


def ctl_r2(v):
    """An epoch-preamble copy narrowed: `const int pl_warmup` declared `const bool`."""
    c = v.cand
    if c.mode != "epoch":
        return None, "no epoch function"
    T = c.T
    for s, e in c.preamble:
        for d in parse_declaration(T, s, e) or []:
            if d.name == "pl_warmup" or (not d.is_ref and T.join(*d.type_span) == "const int"):
                j = next((q for q in range(*d.type_span) if T.t(q) == "int"), None)
                if j is not None:
                    return {T.path: _replace(T.source, T[j].pos, T[j].end, "bool")}, \
                        f"the preamble's `{d.name}` declared bool"
    return None, "no const int copy in the epoch preamble"


def ctl_r3(v):
    """A seam binding direct-initialized with a narrower type: `float pl_now_s(...)`."""
    seam = "networkClockQc"
    if seam not in v.converted:
        return None, f"{seam} is not converted"
    for f in v.new_defs(seam):
        DT = v.tree.tokens(f.path)
        sp = lint.preamble_span(DT, f)
        for s, e in (statements(DT, *sp) if sp else []):
            for d in parse_declaration(DT, s, e) or []:
                if not d.is_ref and d.init_kind == "=" and DT.join(*d.type_span) == "double":
                    new = f"float {d.name}({DT.src(*d.init)});"
                    return {f.path: _replace(DT.source, DT[s].pos, DT[e - 1].end, new)}, \
                        f"{seam}'s {d.name} bound as `{new}`"
    return None, f"no by-value double binding in {seam}"


def _forwarder_negated(v, path, a, b):
    T = v.tree.tokens(path)
    k = T.source.find(f"return {PREDICATE}(", T[a].pos, T[b - 1].end if b > a else None)
    if k < 0:
        return None
    return {path: T.source[:k] + f"return !{PREDICATE}(" + T.source[k + len(f"return {PREDICATE}("):]}


def ctl_r4(v):
    """The DD build's third-band forwarder negated (every third-band test inverts)."""
    seam = "networkBuildCarryDdPriors"
    if seam not in v.converted:
        return None, f"{seam} is not converted"
    for f in v.new_defs(seam):
        sp = lint.preamble_span(v.tree.tokens(f.path), f)
        ov = _forwarder_negated(v, f.path, *sp) if sp else None
        if ov:
            return ov, f"{seam}'s forwarder returns !{PREDICATE}(...)"
    return None, f"no forwarder in {seam}'s bindings"


def ctl_r5(v):
    """The epoch preamble's third-band forwarder negated (the DD build still explicit)."""
    c = v.cand
    if c.mode != "epoch" or not c.preamble:
        return None, "no epoch preamble"
    ov = _forwarder_negated(v, c.T.path, c.preamble[0][0], c.preamble[-1][1])
    return (ov, f"the epoch preamble's forwarder returns !{PREDICATE}(...)") if ov else \
        (None, "the epoch preamble holds no forwarder")


def _before_end_of_bindings(v, fname, line, what):
    defs = find_defs(v.tree, fname)
    if len(defs) != 1:
        return None, f"{fname} is not defined once"
    f = defs[0]
    T = v.tree.tokens(f.path)
    k = T.source.find("// ---- End of the bindings", T[f.body[0]].pos, T[f.body[1]].pos)
    if k < 0:
        return None, f"{fname} has no end-of-bindings marker"
    return {f.path: T.source[:k] + line + "\n    " + T.source[k:]}, what


def ctl_r6(v):
    """A predicate binding that shadows a function its body calls (systemFromLetter)."""
    if not v.epoch:
        return None, "no epoch function"
    return _before_end_of_bindings(v, PREDICATE, "const auto systemFromLetter = [](char) { return System::GPS; };",
                                   f"{PREDICATE}'s preamble shadows systemFromLetter")


def ctl_r7(v):
    """A bracket capture by reference made a copy (`auto sub_tmr0 = ...`)."""
    if not v.epoch:
        return None, "no epoch function"
    defs = find_defs(v.tree, "networkSubBegin")
    if len(defs) != 1:
        return None, "networkSubBegin is not defined once"
    f = defs[0]
    T = v.tree.tokens(f.path)
    sp = lint.preamble_span(T, f)
    for s, e in reversed(statements(T, *sp) if sp else []):
        for d in parse_declaration(T, s, e) or []:
            if d.is_ref and T.t(d.type_span[1]) == "&":
                j = d.type_span[1]
                return {f.path: _replace(T.source, T[j].pos, T[j].end, "")}, \
                    f"networkSubBegin copies the capture {d.name}"
    return None, "no reference binding in networkSubBegin"


def ctl_r8(v):
    """The accessor returns the float vector (NetworkEpoch::x_emit() is `solve.x_s`)."""
    if not v.epoch:
        return None, "no epoch function"
    cp = class_parts(v.tree, lint.EPOCH_HPP, "NetworkEpoch")
    f = next((x for x in (cp["funcs"] if cp else []) if x["name"] == "x_emit" and x["body"]), None)
    if f is None:
        return None, "NetworkEpoch has no x_emit()"
    T = cp["T"]
    bo, bc = f["body"]
    q = next((j for j in range(bc - 1, bo, -1) if T.t(j) == ":" and T.k(j) == "op"), None)
    if q is None:
        return None, "x_emit() is not a conditional"
    return {T.path: T.source[:T[bo].pos] + "{ return " + T.src(q + 1, bc - 1) + "; }" + T.source[T[bc].end:]}, \
        "NetworkEpoch::x_emit() returns its last branch"


def ctl_r9(v):
    """The epoch's constructor body starts a member off its local's state."""
    if not v.epoch:
        return None, "no epoch function"
    cp = class_parts(v.tree, lint.EPOCH_HPP, "NetworkEpoch")
    f = next((x for x in (cp["funcs"] if cp else []) if x["name"] == "NetworkEpoch" and x["body"]), None)
    if f is None:
        return None, "NetworkEpoch has no constructor body"
    T = cp["T"]
    bo, bc = f["body"]
    return {T.path: T.source[:T[bo].end] + ' funnel.carry_slipped_ambs.insert({0, "G01"}); ' + T.source[T[bc].pos:]}, \
        "NetworkEpoch's constructor inserts a phantom slip"


def ctl_r10(v):
    """A run reference member made a copy (`const GnssConfig config;`)."""
    if not v.epoch:
        return None, "no epoch function"
    cp = class_parts(v.tree, RUN_HPP, "NetworkRun")
    m = (cp["members"] if cp else {}).get("config")
    if m is None:
        return None, "NetworkRun has no config member"
    T = cp["T"]
    j = next((q for q in range(m["start"], m["end"]) if T.t(q) == "&"), None)
    if j is None:
        return None, "NetworkRun::config is not a reference"
    return {T.path: _replace(T.source, T[j].pos, T[j].end, "")}, "NetworkRun::config held by value"


def ctl_r11(v):
    """The run's constructor body logs."""
    if not v.epoch:
        return None, "no epoch function"
    cp = class_parts(v.tree, RUN_HPP, "NetworkRun")
    f = next((x for x in (cp["funcs"] if cp else []) if x["name"] == "NetworkRun" and x["body"]), None)
    if f is None:
        return None, "NetworkRun has no constructor body"
    T = cp["T"]
    bo, bc = f["body"]
    return {T.path: T.source[:T[bo].end] + ' log->info("NetworkRun built"); ' + T.source[T[bc].pos:]}, \
        "NetworkRun's constructor logs"


def ctl_r12(v):
    """Code between a converted body's `{` and its opening binding marker."""
    seam = "networkClockQc"
    if seam not in v.converted:
        return None, f"{seam} is not converted"
    for f in v.new_defs(seam):
        T = v.tree.tokens(f.path)
        out = f.params[-1].name if f.params else ""
        return {f.path: T.source[:T[f.body[0]].end] + f' {out}.retro_flag_sats.insert("G01");'
                + T.source[T[f.body[0]].end:]}, f"{seam} writes {out} before its bindings"
    return None, f"{seam}'s epoch form is missing"


def ctl_r13(v):
    """A default of a struct that existed at the base changed (NetworkSolveGlueResult::action)."""
    return _epoch_text_edit(v, SOLVE_HPP, "action{EpochAction::Proceed};", "action{EpochAction::SkipEpoch};",
                            "NetworkSolveGlueResult::action defaults to SkipEpoch")


def ctl_r14(v):
    """A specification field narrowed (NetworkEngineSpec::vce_window_epochs as float)."""
    if not v.epoch:
        return None, "no epoch function"
    cp = class_parts(v.tree, GROUPS_HPP, "NetworkEngineSpec")
    m = (cp["members"] if cp else {}).get("vce_window_epochs")
    if m is None:
        return None, "NetworkEngineSpec has no vce_window_epochs"
    T = cp["T"]
    j = next(q for q in range(m["start"], m["end"]) if T.t(q) == "double")
    return {T.path: _replace(T.source, T[j].pos, T[j].end, "float ")}, "NetworkEngineSpec::vce_window_epochs is float"


def ctl_r15(v):
    """Two engines' declarations swapped in NetworkRunEngines (construction order)."""
    if not v.epoch:
        return None, "no epoch function"
    cp = class_parts(v.tree, GROUPS_HPP, "NetworkRunEngines")
    ms = list((cp["members"] if cp else {}).values())
    if len(ms) < 4:
        return None, "NetworkRunEngines has fewer than four members"
    T = cp["T"]
    src = T.source
    lines = src.split("\n")
    a, b = ms[2]["first"] - 1, ms[3]["first"] - 1
    if ms[2]["last"] != ms[2]["first"] or ms[3]["last"] != ms[3]["first"]:
        return None, "the members span several lines"
    lines[a], lines[b] = lines[b], lines[a]
    return {T.path: "\n".join(lines)}, f"NetworkRunEngines declares {list(cp['members'])[3]} before " \
                                       f"{list(cp['members'])[2]}"


def _insert_in_seam(v, prefer, head, member, statement):
    """Insert @p statement (a format over the object path) at the top of a per-epoch seam's
    body, below its bindings: the seam @p prefer when the tree defines it, else the first row
    of the canonical table that does (the emitter excepted), so the control does not depend on
    one seam surviving a later base.  The object is `<run>.<member>` in an epoch form, or the
    explicit definition's parameter @p head."""
    names = [prefer] + [r.name for r in v.rows if r.name not in (prefer, lint.EMIT)
                        and not isinstance(r.mech, Explicit)]
    for name in names:
        defs = find_defs(v.tree, name)
        epoch_defs = [f for f in defs if lint.is_epoch_form(f)]
        for f in epoch_defs + [f for f in defs if not lint.is_epoch_form(f)]:
            if lint.is_epoch_form(f):
                obj = f"{f.params[0].name}.{member}"
            elif any(q.name == head for q in f.params):
                obj = member.replace(member.split(".")[0], head, 1)
            else:
                continue
            T = v.tree.tokens(f.path)
            sp = lint.preamble_span(T, f)
            at = T[sp[1]].pos if sp and sp[1] < f.body[1] else T[f.body[0]].end
            text = statement.format(obj)
            return {f.path: T.source[:at] + text + "\n    " + T.source[at:]}, f"{name} runs `{text}`"
    return None, "no per-epoch seam to write from"


def ctl_r16(v):
    """A second writer of ctx.counters.n_emitted inside the loop (networkIonoDiag)."""
    return _insert_in_seam(v, "networkIonoDiag", "ctx", "ctx.counters.n_emitted", "++{};")


def ctl_r17(v):
    """A write to a COPY_SOURCE setup field inside the loop (networkSdEwlAr)."""
    return _insert_in_seam(v, "networkSdEwlAr", "setup", "setup.n_threads", "{} = 1;")


# ---- Hardening round 1 (R18 to R34): each a mutation that passed the review-round verifier.
def _spec_fn(v):
    """(T, definition) of networkEngineSpec in the candidate, or (None, None)."""
    T = v.tree.tokens(RUN_CPP)
    idx = v.tree.index(RUN_CPP)
    f = next((g for g in (idx.funcs if idx else []) if g.name == "networkEngineSpec" and g.is_def), None)
    return (T, f) if f is not None else (None, None)


def _spec_bindings(v):
    """[(type text, local, field token)] of networkEngineSpec's `T& name = spec.name;` lines."""
    T, f = _spec_fn(v)
    out = []
    if f is None:
        return T, out
    for s, e in statements(T, f.body[0] + 1, f.body[1]):
        for d in parse_declaration(T, s, e) or []:
            if d.is_ref and d.init and T.t(d.init[0]) == "spec" and T.t(d.init[0] + 1) == "." \
                    and d.init[1] == d.init[0] + 3:
                out.append((T.join(*d.type_span), d.name, d.init[0] + 2))
    return T, out


def _binding_pair(bs):
    """Two reference bindings, of one type when the base has such a pair (the WL-1 and EWL
    configurations), else the first two; None when there are fewer (no networkEngineSpec: an
    empty conversion)."""
    by_type = collections.defaultdict(list)
    for b in bs:
        by_type[b[0]].append(b)
    same = next((x for x in by_type.values() if len(x) >= 2), None)
    return (same or bs)[:2] if len(same or bs) >= 2 else None


def _before_return_spec(v, text, what):
    T, f = _spec_fn(v)
    if f is None:
        return None, "no networkEngineSpec"
    last = statements(T, f.body[0] + 1, f.body[1])[-1]
    if T.join(*last) != join_text("return spec;"):
        return None, "networkEngineSpec does not end with `return spec;`"
    at = T[last[0]].pos
    return {T.path: T.source[:at] + text + " " + T.source[at:]}, what


def ctl_r18(v):
    """Two setup locals bound to each other's spec fields, and the run built from the swapped
    fields (round 1, F01)."""
    T, bs = _spec_bindings(v)
    pair = _binding_pair(bs)
    if pair is None:
        return None, ("no networkEngineSpec" if v.tree.index(RUN_CPP) is None else
                      "networkEngineSpec has fewer than two reference bindings")
    (ta, a, ja), (tb, b, jb) = pair
    src = T.source
    for j, new in sorted(((ja, b), (jb, a)), reverse=True):
        src = src[:T[j].pos] + new + src[T[j].end:]
    ov = {T.path: src}
    run = v.tree.read(RUN_HPP) or ""
    old = f"(spec.{a}, spec.{b})"
    if run.count(old) == 1:
        ov[RUN_HPP] = run.replace(old, f"(spec.{b}, spec.{a})")
    return ov, f"{a} bound to spec.{b} and {b} to spec.{a}" + (" (and the run built from them crosswise)"
                                                               if RUN_HPP in ov else "")


def ctl_r19(v):
    """A spec field its local fills re-assigned from another field before the return (F02)."""
    T, bs = _spec_bindings(v)
    pair = _binding_pair(bs)
    if pair is None:
        return None, ("no networkEngineSpec" if v.tree.index(RUN_CPP) is None else
                      "networkEngineSpec has fewer than two reference bindings")
    a, b = pair[0][1], pair[1][1]
    return _before_return_spec(v, f"spec.{a} = spec.{b};", f"spec.{a} = spec.{b}; before the return")


def ctl_r20(v):
    """A spec field its local fills reset to its default before the return (F03)."""
    T, bs = _spec_bindings(v)
    pick = next((x for x in bs if x[1] == "clk_rebase_cfg"), bs[-1] if bs else None)
    if pick is None:
        return None, ("no networkEngineSpec" if v.tree.index(RUN_CPP) is None else
                      "networkEngineSpec has no reference binding")
    return _before_return_spec(v, f"spec.{pick[1]} = {pick[0]}{{}};",
                               f"spec.{pick[1]} reset to {pick[0]}{{}} before the return")


def ctl_r21(v):
    """An adapter's epoch form forwards and, by the comma operator, empties the slip funnel
    (F08)."""
    names = ["networkCarryDecide"] + sorted(n for n in v.converted if v.by_name[n].mech == ADAPTER)
    for name in names:
        if name not in v.converted or v.by_name[name].mech != ADAPTER:
            continue
        for f in v.new_defs(name):
            T = v.tree.tokens(f.path)
            st = statements(T, f.body[0] + 1, f.body[1])
            if len(st) != 1 or T.t(st[0][1] - 1) != ";" or len(f.params) < 2:
                continue
            ep = f.params[1].name
            at = T[st[0][1] - 1].pos
            text = f", const_cast<NetworkEpoch&>({ep}).funnel.carry_slipped_ambs.clear()"
            return {f.path: T.source[:at] + text + T.source[at:]}, f"{name}'s forward also empties the funnel"
    return None, "no converted adapter"


def _ns_open(T):
    """Character position just past `namespace GnssProcessor {` in T."""
    for j in range(T.n - 2):
        if T.t(j) == "namespace" and T.t(j + 1) == "GnssProcessor" and T.t(j + 2) == "{":
            return T[j + 2].end
    return None


STATIC_INIT = ("static volatile int g_ep1_probe_sink = 0;\n"
               "static const int g_ep1_probe = []{ g_ep1_probe_sink = 42; return 1; }();\n")


def ctl_r22(v):
    """A static object with an initializer in src/core/app/NetworkRun.cpp (F1)."""
    T = v.tree.tokens(RUN_CPP) if v.epoch else None
    at = _ns_open(T) if T is not None else None
    if at is None:
        return None, f"no namespace GnssProcessor in {RUN_CPP}"
    return {T.path: T.source[:at] + "\n\n" + STATIC_INIT + T.source[at:]}, \
        "a static initializer with a side effect before networkEngineSpec"


def ctl_r23(v):
    """A static object with an initializer after the epoch function (F2)."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    at = T[v.cand.close].end
    return {T.path: T.source[:at] + "\n\n" + STATIC_INIT + T.source[at:]}, \
        "a static initializer with a side effect after the epoch function"


def _called_name(v, prefer):
    """A seam name the epoch function calls: @p prefer when it does, else its first network*
    callee."""
    T = v.cand.T
    names = [c.name for c in calls_in(T, v.cand.open + 1, v.cand.close)
             if re.fullmatch(r"network\w+", c.name)]
    return prefer if prefer in names else (names[0] if names else None)


def ctl_r24(v):
    """A better-matching overload of a seam the epoch function calls, in its translation unit,
    the first parameter spelled through an alias (F09)."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    name = _called_name(v, "networkClockQc")
    at = _ns_open(T)
    if name is None or at is None:
        return None, "the epoch function calls no network* seam"
    text = (f" using NetworkRunRef = NetworkRun; template <class... A> static void {name}(NetworkRunRef& run, "
            f"NetworkEpoch& ep, A&... a) {{ {name}(run, static_cast<const NetworkEpoch&>(ep), a...); }}")
    return {T.path: T.source[:at] + text + T.source[at:]}, f"an overload of {name} in the epoch's translation unit"


def ctl_r25(v):
    """A function-like macro of a seam's name above the epoch function (F10)."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    name = _called_name(v, "networkPostFitRaim")
    if name is None:
        return None, "the epoch function calls no network* seam"
    at = T[decl_start(T, v.cand.fn.func)].pos
    return {T.path: T.source[:at] + f"#define {name}(...) ((void)0)\n" + T.source[at:]}, \
        f"`#define {name}(...)` above the epoch function"


def ctl_r26(v):
    """The epoch function under #if 0, a compiled copy under #else (F11)."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    a, b = T[decl_start(T, v.cand.fn.func)].pos, T[v.cand.close].end
    fn = T.source[a:b]
    return {T.path: T.source[:a] + "#if 0\n" + fn + "\n#else\n" + fn + "\n#endif" + T.source[b:]}, \
        "the read copy of the epoch function under #if 0, the compiled one under #else"


def _epoch_parts(v):
    return class_parts(v.tree, lint.EPOCH_HPP, "NetworkEpoch") if v.epoch else None


def ctl_r27(v):
    """A nested type and a member alias in NetworkEpoch split ep.funnel's raw subset into two
    objects (F04)."""
    ep = _epoch_parts(v)
    m = (ep["members"] if ep else {}).get("funnel")
    if m is None:
        return None, "NetworkEpoch has no funnel member"
    T, ft = ep["T"], m["type"]
    gp = class_parts(v.tree, lint.EPOCH_HPP, ft)
    first = next(iter(gp["members"].items()), None) if gp else None
    if first is None:
        return None, f"{ft} has no member"
    name, x = first
    at = T[ep["open"]].end
    text = (f" struct {ft}X : ::GnssProcessor::{ft} {{ {x['type']}{x['ops']} {name}; }}; "
            f"using {ft} = {ft}X;")
    return {T.path: T.source[:at] + text + T.source[at:]}, f"NetworkEpoch nests {ft}X and aliases {ft} to it"


def _clock_parts(v):
    ep = _epoch_parts(v)
    m = (ep["members"] if ep else {}).get("clock")
    return (class_parts(v.tree, lint.EPOCH_HPP, m["type"]) if m else None), (m["type"] if m else None)


def ctl_r28(v):
    """EpochClock gains an aggregate base holding the same three members (F05)."""
    cp, name = _clock_parts(v)
    if cp is None:
        return None, "no EpochClock in the epoch header"
    T = cp["T"]
    body = " ".join(f"{x['type']}{x['ops']} {n}{{}};" for n, x in cp["members"].items())
    a, b = T[cp["head"]].pos, T[cp["open"]].end
    return {T.path: T.source[:a] + f"struct {name}Stamp {{ {body} }}; struct {name} : {name}Stamp {{"
            + T.source[b:]}, f"{name} derives from an aggregate {name}Stamp"


def ctl_r29(v):
    """EpochClock gains an empty base and a constructor that stores its first two members
    crosswise (F06)."""
    cp, name = _clock_parts(v)
    if cp is None or len(cp["members"]) < 2:
        return None, "no EpochClock with two members in the epoch header"
    T = cp["T"]
    ms = list(cp["members"].items())
    ps = ", ".join(f"{x['type']} p{k}" for k, (n, x) in enumerate(ms))
    inits = [f"{ms[1][0]}(p0)", f"{ms[0][0]}(p1)"] + [f"{n}(p{k})" for k, (n, x) in enumerate(ms) if k >= 2]
    ctor = f" {name}() = default; {name}({ps}) : {', '.join(inits)} {{}} "
    a, b, c = T[cp["head"]].pos, T[cp["open"]].end, T[cp["close"]].pos
    return {T.path: T.source[:a] + f"struct {name}Tag {{}}; struct {name} : {name}Tag {{" + T.source[b:c]
            + ctor + T.source[c:]}, f"{name} derives from {name}Tag and stores two members crosswise"


def ctl_r30(v):
    """A hidden friend in NetworkEpoch that ADL prefers at a seam call (F07)."""
    ep = _epoch_parts(v)
    name = _called_name(v, "networkClockQc") if ep is not None else None
    if name is None:
        return None, "no NetworkEpoch, or the epoch function calls no network* seam"
    T = ep["T"]
    at = T[ep["close"]].pos
    text = (f"    template <class Run, class... A> friend void ({name})(Run& run, NetworkEpoch& ep, A&... a) "
            f"{{ {name}(run, static_cast<const NetworkEpoch&>(ep), a...); }}\n")
    return {T.path: T.source[:at] + text + T.source[at:]}, f"a hidden friend {name} in NetworkEpoch"


def _swap_member_lines(cp, a, b, what):
    """Swap the one-line declarations of members @p a and @p b of class_parts @p cp."""
    ms = cp["members"]
    if a not in ms or b not in ms:
        return None, f"{what}: no members {a} and {b}"
    x, y = ms[a], ms[b]
    if x["first"] != x["last"] or y["first"] != y["last"]:
        return None, f"{what}: the members span several lines"
    T = cp["T"]
    lines = T.source.split("\n")
    lines[x["first"] - 1], lines[y["first"] - 1] = lines[y["first"] - 1], lines[x["first"] - 1]
    return {T.path: "\n".join(lines)}, f"{what} declares {b} before {a}"


def ctl_r31(v):
    """Two groups the run owns declared in swapped order (M20)."""
    cp = class_parts(v.tree, RUN_HPP, "NetworkRun") if v.epoch else None
    if cp is None:
        return None, "no NetworkRun"
    owned = [m for m in cp["members"] if m not in RUN_REFS]
    if len(owned) < 2:
        return None, "NetworkRun owns fewer than two groups"
    return _swap_member_lines(cp, owned[0], owned[1], "NetworkRun")


def ctl_r32(v):
    """Two NetworkEpoch members declared in swapped order (M21: ws and solve)."""
    ep = _epoch_parts(v)
    if ep is None:
        return None, "no NetworkEpoch"
    names = list(ep["members"])
    a = "ws" if "ws" in names else names[-3]
    b = names[names.index(a) + 1]
    return _swap_member_lines(ep, a, b, "NetworkEpoch")


def _swap_group(v, group):
    ep = _epoch_parts(v)
    m = (ep["members"] if ep else {}).get(group)
    if m is None:
        return None, f"NetworkEpoch has no {group}"
    gp = class_parts(v.tree, lint.EPOCH_HPP, m["type"])
    names = list(gp["members"]) if gp else []
    if len(names) < 2:
        return None, f"{m['type']} has fewer than two members"
    return _swap_member_lines(gp, names[0], names[1], m["type"])


def ctl_r33(v):
    """The first two members of the layout group declared in swapped order (M22)."""
    return _swap_group(v, "pass_a")


def ctl_r34(v):
    """The first two members of the assembly group declared in swapped order (M23)."""
    return _swap_group(v, "assembly")


# Hardening round 2: the heads around the parameter lists.
def _edit_files(v, edits):
    """Overrides from [(path, char position, char end, new text)], applied per file from the
    last edit backwards."""
    out = {}
    for path in {p for p, _, _, _ in edits}:
        text = v.tree.tokens(path).source
        for _, a, b, new in sorted((x for x in edits if x[0] == path), key=lambda x: -x[1]):
            text = text[:a] + new + text[b:]
        out[path] = text
    return out


def _ret_span(T, f):
    """Character span of @p f's return text: its first token up to the token before its
    (qualified) name."""
    q = f.name_tok
    while T.t(q - 1) == "::" and T.k(q - 2) == "id":
        q -= 2
    return T[f.start].pos, T[q - 1].end


def _seam_heads(v, name):
    """(declaration, definition) Funcs of converted seam @p name's epoch form, or None."""
    if name not in v.converted:
        return None
    r = v.by_name[name]
    hidx = v.tree.index(r.hpp)
    decls = [f for f in (hidx.funcs if hidx else []) if f.name == name and not f.is_def
             and f.params is not None and lint.is_epoch_form(f)]
    defs = v.new_defs(name)
    return (decls[0], defs[0]) if len(decls) == 1 and len(defs) == 1 else None


def _reference_return(name):
    """A converted seam's epoch form returns `const R&` (its declaration, its definition and
    its contract-test alias together), so the spine's store copies the function's destroyed
    local, or for an adapter the explicit form's destroyed temporary (D01, D06, D08)."""
    def f(v):
        heads = _seam_heads(v, name)
        if heads is None:
            return None, f"{name} is not converted"
        edits = []
        for g in heads:
            T = v.tree.tokens(g.path)
            a, b = _ret_span(T, g)
            ret = T.source[a:b]
            if ret.strip() == "void":
                return None, f"{name} returns void"
            edits.append((g.path, a, b, f"const {ret}&"))
        ct = v.tree.read(CONTRACT_TEST) or ""
        m = re.search(r"static_cast<(\w+)>\(&" + re.escape(name) + r"\)", ct)
        if m:
            k = ct.find(f"using {m.group(1)} = ")
            if k >= 0:
                a = k + len(f"using {m.group(1)} = ")
                b = ct.find(" (*)", a)
                if b > a:
                    edits.append((CONTRACT_TEST, a, b, f"const {ct[a:b]}&"))
        return _edit_files(v, edits), f"{name}'s epoch form returns `const {ret}&` (its alias with it)"
    return f


def ctl_r35(v):
    """networkSolveGlue's epoch form returns a reference to its local result (D01)."""
    return _reference_return("networkSolveGlue")(v)


def ctl_r36(v):
    """networkFixedCycles returns a reference to its local emission (D08)."""
    return _reference_return("networkFixedCycles")(v)


def ctl_r37(v):
    """networkSolPrep's adapter returns a reference to the explicit form's temporary (D06)."""
    return _reference_return("networkSolPrep")(v)


def _noexcept_heads(v, funcs, what):
    """`noexcept` after the parameter list of each (path, Func) in @p funcs."""
    edits = []
    for path, g in funcs:
        T = v.tree.tokens(path)
        at = T[g.paren[1]].end
        edits.append((path, at, at, " noexcept"))
    return _edit_files(v, edits), what


def ctl_r38(v):
    """Application::networkProcessEpoch_ declared and defined noexcept (D07)."""
    if not v.epoch:
        return None, "no epoch function"
    eds = [f for f in v.tree.index(APPLICATION_HPP).funcs if f.name == lint.EPOCH_FN and not f.is_def]
    if len(eds) != 1:
        return None, f"{len(eds)} declarations of {lint.EPOCH_FN}"
    fn = v.cand.fn.func
    return _noexcept_heads(v, [(APPLICATION_HPP, eds[0]), (fn.path, fn)],
                           f"{lint.EPOCH_FN} declared and defined noexcept")


def ctl_r39(v):
    """networkWriteStateSnapshot's epoch form declared and defined noexcept (D02)."""
    heads = _seam_heads(v, "networkWriteStateSnapshot")
    if heads is None:
        return None, "networkWriteStateSnapshot is not converted"
    return _noexcept_heads(v, [(g.path, g) for g in heads],
                           "networkWriteStateSnapshot's epoch form declared and defined noexcept")


def ctl_r40(v):
    """networkSubBegin (a moved bracket closure) declared and defined noexcept."""
    if not v.epoch:
        return None, "no epoch function"
    defs, decls = find_defs(v.tree, "networkSubBegin"), find_decls(v.tree, "networkSubBegin")
    if len(defs) != 1 or not decls:
        return None, "networkSubBegin is not defined once"
    return _noexcept_heads(v, [(g.path, g) for g in defs + decls], "networkSubBegin declared and defined noexcept")


def ctl_r41(v):
    """networkEngineSpec returns a reference to its local spec (declaration and definition)."""
    if not v.epoch:
        return None, "no epoch function"
    defs, decls = find_defs(v.tree, "networkEngineSpec"), find_decls(v.tree, "networkEngineSpec")
    if len(defs) != 1 or not decls:
        return None, "networkEngineSpec is not defined once"
    edits = []
    for g in defs + decls:
        T = v.tree.tokens(g.path)
        a, b = _ret_span(T, g)
        edits.append((g.path, a, b, f"const {T.source[a:b]}&"))
    return _edit_files(v, edits), "networkEngineSpec returns `const NetworkEngineSpec&`"


def _member_noexcept(v, path, struct, fname, what):
    cp = class_parts(v.tree, path, struct) if v.epoch else None
    fs = [f for f in (cp["funcs"] if cp else []) if f["name"] == fname and not f["deleted"]]
    if len(fs) != 1:
        return None, f"no one {struct}::{fname}"
    T, j = cp["T"], fs[0]["pc"]
    while T.t(j + 1) in ("const", "volatile", "&", "&&"):
        j += 1                          # after the cv and ref qualifiers, where noexcept goes
    at = T[j].end
    return _edit_files(v, [(path, at, at, " noexcept")]), what


def ctl_r42(v):
    """NetworkEpoch's constructor noexcept."""
    return _member_noexcept(v, lint.EPOCH_HPP, "NetworkEpoch", "NetworkEpoch", "NetworkEpoch's constructor noexcept")


def ctl_r43(v):
    """NetworkRun's constructor noexcept."""
    return _member_noexcept(v, RUN_HPP, "NetworkRun", "NetworkRun", "NetworkRun's constructor noexcept")


def ctl_r44(v):
    """NetworkRunEngines' constructor noexcept."""
    return _member_noexcept(v, GROUPS_HPP, "NetworkRunEngines", "NetworkRunEngines",
                            "NetworkRunEngines' constructor noexcept")


def ctl_r45(v):
    """NetworkEpoch::x_emit() const noexcept."""
    return _member_noexcept(v, lint.EPOCH_HPP, "NetworkEpoch", "x_emit", "x_emit() declared noexcept")


# Hardening round 3: what the epoch's translation unit reads beyond its own text, and how the
# build compiles it.
def _narrowed_overload(v, prefer="networkEmitStore", spine_only=False):
    """(seam, text) of an inline overload of a seam the epoch function calls, its epoch
    parameter narrowed from `const NetworkEpoch&` to `NetworkEpoch&`: an exact match for the
    spine's non-const `ep`, so overload resolution prefers it to the seam's epoch form.  For
    networkEmitStore it empties the per-satellite discontinuity bumps and the global datum bump
    before forwarding (fooling FA).  A converted free seam is preferred (@p spine_only: only
    one); else any network* seam the epoch function calls."""
    free = [n for n in sorted(v.converted) if v.by_name[n].mech not in (RUN, MEMBER)]
    name = prefer if prefer in free else (free[0] if free else None)
    if name is not None:
        types = list(v.epoch_signature(v.by_name[name])[0])
    elif spine_only or not v.epoch:
        return None, None
    else:
        name, types = _called_name(v, prefer), ["NetworkRun&", "const NetworkEpoch&"]
        if name is None:
            return None, None
    params = ", ".join(f"{t.replace('const NetworkEpoch&', 'NetworkEpoch&')} a{k}" for k, t in enumerate(types))
    args = ", ".join(["a0", "static_cast<const NetworkEpoch&>(a1)"] + [f"a{k}" for k in range(2, len(types))])
    body = ("a1.prep.retro_emit_sats.clear(); a1.prep.global_bump = false; "
            if name == "networkEmitStore" else "")
    text = (f"namespace GnssProcessor {{\n"
            f"inline void {name}({params}) {{ {body}{name}({args}); }}\n"
            f"}} // namespace GnssProcessor\n")
    return name, text


def _include_in_epoch_tu(v, line):
    """The epoch translation unit's text with @p line in place of the blank line before
    `namespace GnssProcessor {` (no line moves), or inserted there."""
    src = v.cand.T.source
    k = src.find("\n\nnamespace GnssProcessor {")
    if k >= 0:
        return src[:k] + "\n" + line + src[k + 1:]
    k = src.find("namespace GnssProcessor {")
    return None if k < 0 else src[:k] + line + "\n" + src[k:]


def ctl_r46(v):
    """FA: a new include/app/NetworkEpochCompat.inl holds a narrowed overload of a called seam;
    the epoch's translation unit includes it on its blank line before the namespace."""
    if not v.epoch:
        return None, "no epoch function"
    name, text = _narrowed_overload(v)
    tu = _include_in_epoch_tu(v, '#include "app/NetworkEpochCompat.inl"')
    if name is None or tu is None:
        return None, "the epoch function calls no network* seam"
    inl = "#pragma once\n#include \"app/NetworkEpoch.hpp\"\n#include \"app/NetworkRun.hpp\"\n" + text
    return {"include/app/NetworkEpochCompat.inl": inl, lint.EPOCH_CPP: tu}, \
        f"a narrowed overload of {name} in an included .inl file"


def ctl_r47(v):
    """FB: the hand-written engine specification test (listed in gnss_tests) holds a narrowed
    overload of a called seam; the epoch's translation unit includes it by a relative path."""
    if not v.epoch:
        return None, "no epoch function"
    name, text = _narrowed_overload(v)
    tu = _include_in_epoch_tu(v, f'#include "../../../{ENGINE_SPEC_TEST}"')
    if name is None or tu is None:
        return None, "the epoch function calls no network* seam"
    ov = {ENGINE_SPEC_TEST: (v.tree.read(ENGINE_SPEC_TEST) or "// The engine specification test (spec 5.9).\n")
          + "\n#include \"app/NetworkEpoch.hpp\"\n#include \"app/NetworkRun.hpp\"\n" + text,
          lint.EPOCH_CPP: tu}
    cm = v.tree.read(lint.CMAKE) or ""
    if ENGINE_SPEC_TEST not in cm:
        m = re.search(r"^([ \t]*)" + re.escape(CONTRACT_TEST) + r"[ \t]*\n", cm, re.M)
        if m is None:
            return None, f"{CONTRACT_TEST} is not listed"
        ov[lint.CMAKE] = cm[:m.end()] + f"{m.group(1)}{ENGINE_SPEC_TEST}\n" + cm[m.end():]
    return ov, f"a narrowed overload of {name} in the engine specification test, included by the epoch's unit"


def ctl_r48(v):
    """FC: a CMakeLists.txt line that names the epoch's translation unit compiles it /fp:fast."""
    if not v.epoch:
        return None, "no epoch function"
    cm = v.tree.read(lint.CMAKE)
    return {lint.CMAKE: cm.rstrip("\n") + f"\nset_source_files_properties({lint.EPOCH_CPP} PROPERTIES "
                                          "COMPILE_OPTIONS \"/fp:fast\")\n"}, \
        "a per-source /fp:fast on the epoch's translation unit"


def ctl_r49(v):
    """A narrowed overload of a converted seam in a new header under include/ (so only the
    overload rules see it), included by the epoch's translation unit."""
    if not v.epoch:
        return None, "no epoch function"
    name, text = _narrowed_overload(v, spine_only=True)
    tu = _include_in_epoch_tu(v, '#include "app/NetworkEpochCompat.hpp"')
    if name is None or tu is None:
        return None, "no converted free seam"
    hdr = "#pragma once\n#include \"app/NetworkEpoch.hpp\"\n#include \"app/NetworkRun.hpp\"\n" + text
    return {"include/app/NetworkEpochCompat.hpp": hdr, lint.EPOCH_CPP: tu}, \
        f"a narrowed overload of {name} in an included header"


def ctl_r50(v):
    """A target-wide compile definition makes one called seam's name another's (the macro
    class of R25, through the build)."""
    a = _called_name(v, "networkIonoDiag")
    b = _called_name(v, "networkDdFrac")
    if a is None or b is None or a == b:
        return None, "the epoch function calls fewer than two network* seams"
    cm = v.tree.read(lint.CMAKE)
    return {lint.CMAKE: cm.rstrip("\n") + f"\ntarget_compile_definitions(gnss_core PRIVATE {a}={b})\n"}, \
        f"`{a}={b}` defined for gnss_core"


def ctl_r51(v):
    """A CMake line hidden from a C++ reading: `/*` after a comment line, a compile option, and
    `# */` (CMake reads two comments around the option)."""
    cm = v.tree.read(lint.CMAKE)
    lines = cm.split("\n")
    k = next((i for i in range(len(lines) - 1) if lines[i].startswith("#") and lines[i + 1].startswith("#")), None)
    if k is None:
        return None, f"no two comment lines in {lint.CMAKE}"
    lines[k:k + 1] = [lines[k] + " /*", "add_compile_options(/fp:fast)", "# */"]
    return {lint.CMAKE: "\n".join(lines)}, "add_compile_options(/fp:fast) inside a C++ block comment"


def ctl_r52(v):
    """A frozen #include removed from the driver's translation unit."""
    T = v.tree.tokens(DRIVER_PATH)
    incs = [j for j in range(T.n) if T.k(j) == "pp" and pp_directive(T.t(j)) == "include"
            and lint.include_target(T.t(j))[0] == '"']
    if not incs:
        return None, f"no quoted #include in {DRIVER_PATH}"
    j = incs[-1]
    a, b = T[j].pos, T[j].end
    return {DRIVER_PATH: T.source[:a] + T.source[b:]}, f"`{T.t(j).strip()[:60]}` removed from {DRIVER_PATH}"


# Hardening round 4.  R53 to R58: a declaration in a converted seam's bindings under a name V3a
# never compares (the CALLABLE predicate's, a removed CALLBACK_DIRECT's), whose initializer or
# type's constructor does the work.  R59 to R66: code the compiler reads behind a line splice
# that every check reads as part of a comment.  R67 to R74: forward declarations and lone
# braces that V1e accounted for by their text, wherever they stood.  R75 to R77: a carriage
# return that is not half of a line end.  R78: a conditional group around a frozen #include.
def _extra_binding(v, prefer, line, what, member=False):
    """@p line (a format over the seam's run and epoch parameter names) inserted before the
    end-of-bindings marker of the converted seam @p prefer when the tree converts it, else of
    the first converted direct seam (a member seam when @p member) whose frozen parameters do
    not hold the name @p line declares: a binding no frozen parameter of the seam asks for."""
    names = [prefer] + sorted(n for n in v.converted if n != prefer)
    for name in names:
        r = v.by_name.get(name)
        if name not in v.converted or r is None or r.mech == ADAPTER or (member and r.mech != MEMBER):
            continue
        fdef = v.fdecls.get(name, (None, None))[1]
        for f in v.new_defs(name):
            text = line.format(run=f.params[0].name, ep=f.params[1].name)
            declared_name = re.search(r"(\w+)\s*(?:=|\()", text.split("[")[0])
            if fdef is not None and declared_name and any(p.name == declared_name.group(1) for p in fdef.params):
                continue
            T = v.tree.tokens(f.path)
            k = T.source.find("// ---- End of the bindings", T[f.body[0]].pos, T[f.body[1]].pos)
            if k < 0:
                continue
            return {f.path: T.source[:k] + text + "\n    " + T.source[k:]}, f"{name}'s bindings gain `{text}` ({what})"
    return None, f"no converted {'member ' if member else ''}seam with generated bindings"


def _callable_name():
    return SUBST.callable[0] if SUBST.callable else "isUncarriedThirdBand"


def ctl_r53(v):
    """U01: an immediately invoked lambda under the CALLABLE name empties the phase-leveling
    carry in a seam that never had the predicate (networkIonoDiag)."""
    return _extra_binding(v, "networkIonoDiag", "const bool " + _callable_name() +
                          " = [&{run}]() -> bool {{ {run}.ctx.carry.local_carry.clear(); return false; }}();",
                          "the carry emptied every epoch")


def ctl_r54(v):
    """U01p: the same in a member seam that takes the fallback names (networkPassB_)."""
    return _extra_binding(v, "networkPassB_", "const bool " + _callable_name() +
                          " = [&{run}]() -> bool {{ {run}.ctx.carry.local_carry.clear(); return false; }}();",
                          "the carry emptied before PASS B injects it", member=True)


def ctl_r55(v):
    """U02b: the solve's bindings declare a removed CALLBACK_DIRECT's name (no line binds it),
    an immediately invoked lambda that empties the carry; the include the replay needs added
    on a blank line."""
    if lint.SOLVE not in v.converted or not SUBST.callbacks:
        return None, f"{lint.SOLVE} is not converted"
    cb = "buildCarryDdPriors" if "buildCarryDdPriors" in SUBST.callbacks else SUBST.callbacks[0]
    ov, what = _extra_binding(v, lint.SOLVE, "const bool " + cb +
                              " = [&{run}]() -> bool {{ {run}.ctx.carry.local_carry.clear(); return true; }}();",
                              "a removed callback's name")
    if ov is None:
        return ov, what
    path, src = next(iter(ov.items()))
    inc = '#include "filter/NetworkDdPriorBuild.hpp"'
    if inc not in src:
        k = src.find('#include "math/TimeUtils.hpp"\n\n')
        if k >= 0:
            k += len('#include "math/TimeUtils.hpp"\n')
            src = src[:k] + inc + src[k:]
    return {path: src}, what


def ctl_r56(v):
    """U03: the CALLABLE name's immediately invoked lambda writes the const epoch through a
    const_cast (the solution zeroed after the emit)."""
    return _extra_binding(v, "networkIonoDiag", "const bool " + _callable_name() +
                          " = [&{ep}]() -> bool {{ const_cast<NetworkEpoch&>({ep}).solve.x_s.setZero(); "
                          "return false; }}();", "the epoch written through a const_cast")


def ctl_r57(v):
    """U04: a declaration under the CALLABLE name whose type's constructor truncates the
    solution trace, direct-initialized from a member read."""
    return _extra_binding(v, "networkIonoDiag", "std::ofstream " + _callable_name() + "({run}.traces.soln_path);",
                          "a constructor with a side effect")


def ctl_r58(v):
    """U07: a declaration under the CALLABLE name whose type adopts the product writer."""
    return _extra_binding(v, "networkIonoDiag", "std::unique_ptr<NetworkProductWriter> " + _callable_name() +
                          "({run}.setup.product_writer.get());", "the product writer deleted at the seam's end")


def _hidden(code):
    """`/* r4 *\\<NL>/ CODE /* r4 */`: one comment to a reader that splices no line, `/* r4 */
    CODE /* r4 */` to the compiler, which splices before it recognizes comments."""
    return "/* r4 *" + chr(92) + "\n/ " + code + " /* r4 */"


def _epoch_stmt(v, test):
    """(start, end) token span of the first top-level statement of the epoch function for
    which @p test(text) holds, or None."""
    c = v.cand
    T = c.T
    return next(((s, e) for s, e in c.stmts if test(T.join(s, e))), None)


def _hide_at_epoch(v, test, code, before, what):
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    st = _epoch_stmt(v, test)
    if st is None:
        return None, f"no epoch statement for {what}"
    if before:
        at = T.source.rfind("\n", 0, T[st[0]].pos) + 1
        return {T.path: T.source[:at] + "    " + _hidden(code) + "\n" + T.source[at:]}, what
    at = T[st[1] - 1].end
    return {T.path: T.source[:at] + "\n    " + _hidden(code) + T.source[at:]}, what


def ctl_r59(v):
    """m03: a hidden `n_emitted = 0;` after the ep.emitted store (the VALUE_EQUAL object)."""
    return _hide_at_epoch(v, lambda t: t.startswith("ep.emitted="), "n_emitted = 0;", False,
                          "a hidden write of n_emitted after the ep.emitted store")


def ctl_r60(v):
    """m01: a hidden `return LoopSignal::Next;` before the state snapshot."""
    return _hide_at_epoch(v, lambda t: t.startswith("networkWriteStateSnapshot("), "return LoopSignal::Next;",
                          True, "a hidden return before the state snapshot")


def ctl_r61(v):
    """m04: a hidden `return LoopSignal::Stop;` near the top of the epoch body."""
    return _hide_at_epoch(v, lambda t: t.startswith("++n_loop_epochs"), "return LoopSignal::Stop;", False,
                          "a hidden Stop after the epoch count")


def ctl_r62(v):
    """m05: a hidden throw before the state snapshot."""
    return _hide_at_epoch(v, lambda t: t.startswith("networkWriteStateSnapshot("),
                          'throw std::runtime_error("r4");', True, "a hidden throw before the state snapshot")


def ctl_r63(v):
    """m06: a hidden `return;` right below a converted seam's bindings (networkIonoDiag)."""
    names = ["networkIonoDiag"] + sorted(n for n in v.converted if n != "networkIonoDiag")
    for name in names:
        if name not in v.converted or v.by_name[name].mech == ADAPTER:
            continue
        for f in v.new_defs(name):
            T = v.tree.tokens(f.path)
            k = T.source.find("// ---- End of the bindings ----\n", T[f.body[0]].pos, T[f.body[1]].pos)
            if k < 0:
                continue
            k += len("// ---- End of the bindings ----\n")
            return {f.path: T.source[:k] + "    " + _hidden("return;") + "\n" + T.source[k:]}, \
                f"a hidden return below {name}'s bindings"
    return None, "no converted seam with generated bindings"


def ctl_r64(v):
    """m02: a hidden second call of a spine seam (networkIonoDiag)."""
    name = _called_name(v, "networkIonoDiag") if v.epoch else None
    if name is None:
        return None, "no epoch function"
    st = _epoch_stmt(v, lambda t: t.startswith(name + "("))
    if st is None:
        return None, f"no epoch statement that calls {name}"
    T = v.cand.T
    call = T.join(*st)              # the statement's tokens on one line (the epoch form, or EP1a-1's explicit call)
    return _hide_at_epoch(v, lambda t: t == call, call, False, f"a hidden second {name} call")


def ctl_r65(v):
    """m09: a hidden `return LoopSignal::Next;` before the max-duration cutoff's Stop."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    k = T.source.find("return LoopSignal::Stop;", T[v.cand.open].pos, T[v.cand.close].pos)
    if k < 0:
        return None, "the epoch function returns no LoopSignal::Stop"
    at = T.source.rfind("\n", 0, k) + 1
    ind = T.source[at:k]
    return {T.path: T.source[:at] + ind + _hidden("return LoopSignal::Next;") + "\n" + T.source[at:]}, \
        "a hidden Next before the first LoopSignal::Stop"


def ctl_r66(v):
    """m10: a hidden call in runNetwork's loop body, after the per-epoch dispatch."""
    T = v.tree.tokens(DRIVER_PATH)
    disp = "networkProcessEpoch_" if v.epoch else LAMBDA_NAME
    c = next((c for c in calls_in(T, 0, T.n) if c.name == disp and T.t(c.tok - 1) != "::"
              and T.t(c.close + 1) != "{"), None)
    if c is None:
        return None, f"runNetwork does not call {disp}"
    at = T.source.find("\n", T[c.close].end)
    code = "run.engines.est.sectionTimers();" if v.epoch else "est.sectionTimers();"
    return {T.path: T.source[:at] + "\n            " + _hidden(code) + T.source[at:]}, \
        f"a hidden call after runNetwork's {disp} dispatch"


# The scope foolings: each a site of the round-4 reproducer, else a generic one found in the
# same file, so the control does not depend on one statement surviving a later base.
_IF_BLOCK = re.compile(r"^([ \t]*)if \([^\n]*\) \{\n([ \t]+)([^\n{}]+;)\n", re.M)
_BRACELESS_IF = re.compile(r"^([ \t]*)if \([^\n{};]*\)\n([ \t]+)([^\n{}]+;)\n", re.M)
_LOCAL_DECL = re.compile(r"^([ \t]+)(?:const )?(?:double|int|bool|auto) \w+ = [^\n{}]+;\n", re.M)


def _site(v, path, anchor, pattern):
    """(text, match position, match end) of @p anchor in @p path, else the first match of the
    regular expression @p pattern there (returned as a Match), or None."""
    src = v.tree.read(path)
    if src is None:
        return None
    if anchor and src.count(anchor) == 1:
        k = src.find(anchor)
        return src, k, k + len(anchor)
    m = pattern.search(src)
    return (src, m) if m else None


def _split_block(v, path, anchor, new, one_line=False, include=False):
    """Split the block that opens just above a statement: forward declarations (or an
    #include) and lone braces close the block and open a new one, so the statements below run
    unconditionally (s01, s03, s04, s05, s07)."""
    hit = _site(v, path, anchor, _IF_BLOCK)
    if hit is None:
        return None, f"no braced if block in {path}"
    if len(hit) == 3:
        src, a, b = hit
        return {path: src[:a] + new + src[b:]}, f"a block split in {path}"
    src, m = hit
    ind, ind2 = m.group(1), m.group(2)
    head = src[m.start():m.end(2) + len(m.group(3)) + 1]
    first = ("#include <vector>" if include else "struct EpFwdA;")
    mid = (f"{ind2}{first}\n{ind}}}{{\n" if one_line
           else f"{ind2}{first}\n{ind}}}\n{ind}struct EpFwdB;\n{ind}{{\n")
    return {path: src[:m.start()] + head.split("\n", 1)[0] + "\n" + mid + head.split("\n", 1)[1] + src[m.end():]}, \
        f"a block split in {path}"


def _wrap_decl(v, path, anchor, new):
    """Wrap one local declaration in its own block between forward declarations (s02, s06):
    a later use of the name reads an outer declaration instead."""
    hit = _site(v, path, anchor, _LOCAL_DECL)
    if hit is None:
        return None, f"no local declaration in {path}"
    if len(hit) == 3:
        src, a, b = hit
        return {path: src[:a] + new + src[b:]}, f"a declaration wrapped in its own block in {path}"
    src, m = hit
    ind = m.group(1)
    return {path: src[:m.start()] + f"{ind}struct EpFwdA;\n{ind}{{\n" + m.group(0) + f"{ind}struct EpFwdB;\n{ind}}}\n"
            + src[m.end():]}, f"a declaration wrapped in its own block in {path}"


def ctl_r67(v):
    """s01: two forward declarations and two lone braces split NetworkSbasis'
    codeFamilyDiffTimeLink no-veteran block, so its reseed runs on every call."""
    return _split_block(v, "src/core/filter/NetworkSbasis.cpp",
                        "        out.jac_sats = common_sats;\n        out.rhs      = 0.0;\n",
                        "        out.jac_sats = common_sats;\n        struct EpFwdA;\n    }\n    struct EpFwdB;\n    {\n"
                        "        out.rhs      = 0.0;\n")


def ctl_r68(v):
    """s02: the SMH block's `dmax` wrapped in its own block, so the SMH log line reads the PBH
    block's `dmax`."""
    old = "            const double dmax = da.empty() ? 0.0 : *std::max_element(da.begin(), da.end());\n"
    return _wrap_decl(v, "src/core/app/NetworkClosedLoopRunEnd.cpp", old,
                      "            struct EpFwdA;\n            {\n" + old + "            struct EpFwdB;\n            }\n")


def ctl_r69(v):
    """s03: one forward declaration and a single `}{` line split the same block."""
    return _split_block(v, "src/core/filter/NetworkSbasis.cpp",
                        "        out.jac_sats = common_sats;\n        out.rhs      = 0.0;\n",
                        "        out.jac_sats = common_sats;\n        struct EpFwdA;\n    }{\n        out.rhs      = 0.0;\n",
                        one_line=True)


def ctl_r70(v):
    """s04: NetworkEstimator::solve's epoch-open guard split, so every solve throws."""
    old = ('    if (!epoch_open_) {\n        throw std::logic_error("NetworkEstimator::solve: call beginEpoch() '
           'first");\n')
    return _split_block(v, "src/core/filter/NetworkEstimator.cpp", old,
                        '    if (!epoch_open_) {\n        struct EpFwdA;\n    }\n    struct EpFwdB;\n    {\n'
                        '        throw std::logic_error("NetworkEstimator::solve: call beginEpoch() first");\n')


def ctl_r71(v):
    """s05: networkSetup_'s marker-to-ARP block split, so every station is reduced to its ARP."""
    old = "                if (marker_to_arp) {\n                    st.ecef = Coordinates::markerToArp(st.ecef, hen);\n"
    return _split_block(v, "src/core/app/ApplicationNetworkSetup.cpp", old,
                        "                if (marker_to_arp) {\n                    struct EpFwdA;\n                }\n"
                        "                struct EpFwdB;\n                {\n"
                        "                    st.ecef = Coordinates::markerToArp(st.ecef, hen);\n")


def ctl_r72(v):
    """s06: CycleSlipManager's local `is_bssd_pivot` and its if-block wrapped in their own
    block, so the soft-LLI test reads the outer std::function of that name."""
    path = "src/core/slip/CycleSlipManager.cpp"
    src = v.tree.read(path)
    a = "                    bool is_bssd_pivot = false;\n"
    b = ("                        is_bssd_pivot = (pit != pivots.end() && pit->second == sat_id);\n"
         "                    }\n")
    if src is not None and src.count(a) == 1 and src.count(b) == 1:
        src = src.replace(a, "                    struct EpFwdA;\n                    {\n" + a)
        src = src.replace(b, b + "                    struct EpFwdB;\n                    }\n")
        return {path: src}, f"a declaration and its block wrapped in their own block in {path}"
    return _wrap_decl(v, path, None, None)


def ctl_r73(v):
    """s07: the split of R69 with `#include <vector>` as the `}{` line's neighbour."""
    return _split_block(v, "src/core/filter/NetworkSbasis.cpp",
                        "        out.jac_sats = common_sats;\n        out.rhs      = 0.0;\n",
                        "        out.jac_sats = common_sats;\n#include <vector>\n    }{\n        out.rhs      = 0.0;\n",
                        one_line=True, include=True)


def ctl_r74(v):
    """s08: one forward declaration as the substatement of a braceless if, so the statement
    below it runs unconditionally (every product contract advertises hsig=pc)."""
    path = "src/core/app/ApplicationNetworkSetup.cpp"
    old = ('            if (config_.networkHSigma0PrefitCensus())\n'
           '                pending_contract.h_sigma0_source = "pc";\n')
    hit = _site(v, path, old, _BRACELESS_IF)
    if hit is None:
        return None, f"no braceless if in {path}"
    if len(hit) == 3:
        src, a, b = hit
        return {path: src[:a] + old.replace("\n", "\n                struct EpFwdA;\n", 1) + src[b:]}, \
            f"a forward declaration as a braceless if's substatement in {path}"
    src, m = hit
    k = src.find("\n", m.start()) + 1
    return {path: src[:k] + m.group(2) + "struct EpFwdA;\n" + src[k:]}, \
        f"a forward declaration as a braceless if's substatement in {path}"


def _cr_hidden(code):
    """A `//` comment ending in backslash and carriage return, then @p code on the next line:
    after the CR LF fold the reader continues the comment over @p code, MSVC does not."""
    return "    // the store above " + chr(92) + "\r\n    " + code + "\n"


def ctl_r75(v):
    """s09: after the ep.emitted store, a comment ending in backslash CR hides
    `if (!ep.emitted) return LoopSignal::Next;`."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    st = _epoch_stmt(v, lambda t: t.startswith("ep.emitted="))
    if st is None:
        return None, "no ep.emitted store"
    at = T.source.find("\n", T[st[1] - 1].end) + 1
    return {T.path: T.source[:at] + _cr_hidden("if (!ep.emitted) return LoopSignal::Next;") + T.source[at:]}, \
        "a carriage-return comment hides a return after the ep.emitted store"


def ctl_r76(v):
    """s10: in the solve, a comment ending in backslash CR hides `result.x_s *= 1.0001;`."""
    src = v.tree.read(SOLVE_CPP)
    anchor = "    result.x_s = std::move(x_s);\n"
    if src is None or src.count(anchor) != 1:
        return None, f"`{anchor.strip()}` is not in {SOLVE_CPP} once"
    k = src.find(anchor) + len(anchor)
    return {SOLVE_CPP: src[:k] + _cr_hidden("result.x_s *= 1.0001;") + src[k:]}, \
        "a carriage-return comment hides a scaling of the solve's solution"


def ctl_r77(v):
    """s11: in the epoch's unit, a comment ending in backslash CR hides a #define of a called
    seam's name (R25 by a channel no check read)."""
    if not v.epoch:
        return None, "no epoch function"
    T = v.cand.T
    name = _called_name(v, "networkPostFitRaim")
    k = T.source.find("namespace GnssProcessor {")
    if name is None or k < 0:
        return None, "no namespace or no called seam in the epoch's unit"
    hidden = "// the epoch's own unit " + chr(92) + "\r\n" + f"#define {name}(...) ((void)0)\n"
    return {T.path: T.source[:k] + hidden + T.source[k:]}, f"a carriage-return comment hides `#define {name}(...)`"


def ctl_r78(v):
    """`#if 0` and `#endif` around the driver unit's last quoted frozen #include (R52 by a
    conditional group, which V1e admitted as a group of #include lines)."""
    T = v.tree.tokens(DRIVER_PATH)
    incs = [j for j in range(T.n) if T.k(j) == "pp" and pp_directive(T.t(j)) == "include"
            and lint.include_target(T.t(j))[0] == '"']
    if not incs:
        return None, f"no quoted #include in {DRIVER_PATH}"
    j = incs[-1]
    a = T.source.rfind("\n", 0, T[j].pos) + 1
    b = T.source.find("\n", T[j].end)
    return {DRIVER_PATH: T.source[:a] + "#if 0\n" + T.source[a:b] + "\n#endif" + T.source[b:]}, \
        f"`#if 0` around `{T.t(j).strip()[:60]}` in {DRIVER_PATH}"


def ctl_r79(v):
    """The review round's defect: an UNREAD_ALIAS line left in runNetwork at its frozen place,
    where nothing reads it once the lambda has moved (so V1d's statement match accepts it and
    only the unread-declaration rule can fail it)."""
    if not v.epoch:
        return None, "no epoch function"
    fz, c = v.fz, v.cand
    fT, nT = fz.rn.T, c.rn.T
    F, N = fz.rn.statements(), c.rn.statements()
    have = {x.name for x in c.rn.scopes.root.decls}
    texts = [nT.join(s, e) for s, e in N]
    for i, (s, e) in enumerate(F):
        ds = parse_declaration(fT, s, e) or []
        if len(ds) != 1 or ds[0].name not in SUBST.unread_alias or ds[0].name in have:
            continue
        for s2, e2 in F[i + 1:]:
            nxt = fT.join(s2, e2)
            if nxt in texts:
                q = N[texts.index(nxt)][0]
                ls = nT.source.rfind("\n", 0, nT[q].pos) + 1
                line = fT.source[fT.source.rfind("\n", 0, fT[s].pos) + 1:fT[e - 1].end] + "\n"
                return {DRIVER_PATH: _replace(nT.source, ls, ls, line)}, \
                    f"`{line.strip()}` kept in runNetwork before `{nxt[:40]}`"
    return None, "no UNREAD_ALIAS line is gone from runNetwork"


def _conv(*names):
    return lambda v: all(n in v.converted for n in names)


def _epoch_only(v):
    return v.epoch


def _direct_conv(v):
    """The tree converts a seam that has generated bindings (not only adapters)."""
    return any(v.by_name[n].mech != ADAPTER for n in v.converted)


CONTROLS = (
    Control("N1", "V3a", "WCF shape: a by-value copy of we_culprits in the DD build's bindings",
            ctl_preamble_edit("networkBuildCarryDdPriors", "we_culprits", "const WithinEpochCulprits& we_culprits",
                              "WithinEpochCulprits we_culprits", "we_culprits copied"),
            _conv("networkBuildCarryDdPriors")),
    Control("N2", "V3a", "networkPassASlipScan_ binds funnel.carry_slipped_ambs",
            ctl_preamble_edit("networkPassASlipScan_", "carry_slipped_ambs", "funnel.carry_slipped_raw",
                              "funnel.carry_slipped_ambs", "the raw subset bound to the funnel"),
            _conv("networkPassASlipScan_")),
    Control("N3", "V3a", "networkSolPrep's adapter forwards ep.solve.x_s",
            ctl_preamble_edit("networkSolPrep", "x_emit", "ep.x_emit()", "ep.solve.x_s", "x_s instead of x_emit"),
            _conv("networkSolPrep")),
    Control("N4", "V3a", "the solve's internal networkRhsAttribTrip call bound to the epoch", ctl_n4,
            _conv("networkRhsAttribTrip", "networkSolveGlue")),
    Control("N5", "V3b", "a copy of a captured reference", ctl_n5),
    Control("N6", "V6", "networkIonoDiag and networkDdFrac swapped", ctl_n6),
    Control("N7", "V4", "a driver write into the epoch after the solve", ctl_n7),
    Control("N8", "V1a", "est_index_of sized with 0 instead of -1", ctl_n8),
    Control("N9", "V1b", "a one-token edit inside a converted body", ctl_n9,
            lambda v: any(v.by_name[n].mech != ADAPTER for n in v.converted)),
    Control("N10", "V1a", "the shadowing dt rewritten to ep.clock.dt", ctl_n10),
    Control("N11", "V12", "a log line cut into the engine spec", ctl_n11),
    Control("N12", "V13", "networkFixedCycles defined with `ep` as a parameter name", ctl_n12),
    Control("N13", "V2", "networkClockQc given EpochPassA& as a second output", ctl_n13, _conv("networkClockQc")),
    Control("N14", "V1b", "the replay's WCF resolves counter dropped", ctl_n14, _conv("networkSolveGlue")),
    Control("N15", "V10", "networkHandoverStransformBeforeRows binds ep.solve.plan", ctl_n15,
            _conv("networkHandoverStransformBeforeRows")),
    Control("N16", "V4", "ep.emitted stored before networkEmitStore", ctl_n16, _epoch_only),
    # The review round (each a mutation an earlier verifier accepted).
    Control("R1", "V1a", "the shadowing dt's reads rewritten to ep.clock.dt (beside N10)", ctl_r1, _epoch_only),
    Control("R2", "V3b", "an epoch-preamble copy declared with another type", ctl_r2, _epoch_only),
    Control("R3", "V3a", "a seam binding direct-initialized with a narrower type", ctl_r3, _conv("networkClockQc")),
    Control("R4", "V3a", "the DD build's third-band forwarder negated", ctl_r4,
            lambda v: "networkBuildCarryDdPriors" in v.converted and v.callable_closure() is not None),
    Control("R5", "V3b", "the epoch preamble's third-band forwarder negated", ctl_r5,
            lambda v: v.epoch and "networkBuildCarryDdPriors" not in v.converted
            and v.callable_closure() is not None),
    Control("R6", "V1d", "a predicate binding that shadows a function its body calls", ctl_r6,
            lambda v: v.epoch and v.callable_closure() is not None),
    Control("R7", "V1d", "a bracket capture by reference made a copy", ctl_r7, _epoch_only),
    Control("R8", "V3c", "NetworkEpoch::x_emit() returns the float vector", ctl_r8, _epoch_only),
    Control("R9", "V3c", "NetworkEpoch's constructor writes a member", ctl_r9, _epoch_only),
    Control("R10", "V3d", "a run reference member held by value", ctl_r10, _epoch_only),
    Control("R11", "V3d", "NetworkRun's constructor logs", ctl_r11, _epoch_only),
    Control("R12", "V1b", "code before a converted seam's bindings", ctl_r12, _conv("networkClockQc")),
    Control("R13", "V1e", "a default of a struct the base has changed", ctl_r13),
    Control("R14", "V12", "a specification field narrowed", ctl_r14, _epoch_only),
    Control("R15", "V1d", "two engines' construction order swapped", ctl_r15, _epoch_only),
    Control("R16", "V1a", "a second in-loop writer of ctx.counters.n_emitted (VALUE_EQUAL)", ctl_r16),
    Control("R17", "V3b", "a setup field written inside the loop (COPY_SOURCE)", ctl_r17),
    # Hardening round 1 (each a mutation the review-round verifier accepted).
    Control("R18", "V12", "two setup locals bound to each other's spec fields", ctl_r18, _epoch_only),
    Control("R19", "V12", "a spec field re-assigned from another field", ctl_r19, _epoch_only),
    Control("R20", "V12", "a spec field reset to its default", ctl_r20, _epoch_only),
    Control("R21", "V1b", "an adapter's forward carries a write by the comma operator", ctl_r21,
            lambda v: any(v.by_name[n].mech == ADAPTER for n in v.converted)),
    Control("R22", "V1e", "a static initializer in NetworkRun.cpp", ctl_r22, _epoch_only),
    Control("R23", "V1e", "a static initializer after the epoch function", ctl_r23, _epoch_only),
    Control("R24", "V1e", "a better-matching seam overload in the epoch's translation unit", ctl_r24,
            _epoch_only),
    Control("R25", "V1e", "a macro of a seam's name in the epoch's translation unit", ctl_r25, _epoch_only),
    Control("R26", "V1e", "the epoch function under #if 0, a compiled copy under #else", ctl_r26, _epoch_only),
    Control("R27", "V3c", "a nested type and alias in NetworkEpoch split a funnel member", ctl_r27, _epoch_only),
    Control("R28", "V3c", "EpochClock with an aggregate base of its members", ctl_r28, _epoch_only),
    Control("R29", "V3c", "EpochClock with an empty base and a crosswise constructor", ctl_r29, _epoch_only),
    Control("R30", "V3c", "a hidden friend in NetworkEpoch captures a seam call", ctl_r30, _epoch_only),
    Control("R31", "V3d", "two run-owned groups declared in swapped order", ctl_r31, _epoch_only),
    Control("R32", "V3c", "two NetworkEpoch members declared in swapped order", ctl_r32, _epoch_only),
    Control("R33", "V3c", "two members of EpochPassA declared in swapped order", ctl_r33, _epoch_only),
    Control("R34", "V3c", "two members of EpochAssembly declared in swapped order", ctl_r34, _epoch_only),
    # Hardening round 2 (R35 to R39 each a mutation the round-1 verifier accepted; R40 to R45
    # the same head edit at the other functions the step writes).
    Control("R35", "V2", "the solve's epoch form returns a reference to its local result", ctl_r35,
            _conv("networkSolveGlue")),
    Control("R36", "V2", "networkFixedCycles returns a reference to its local", ctl_r36,
            _conv("networkFixedCycles")),
    Control("R37", "V2", "an adapter's epoch form returns a reference to the explicit form's temporary",
            ctl_r37, _conv("networkSolPrep")),
    Control("R38", "V1a", "the epoch function declared and defined noexcept", ctl_r38, _epoch_only),
    Control("R39", "V2", "a converted seam declared and defined noexcept", ctl_r39,
            _conv("networkWriteStateSnapshot")),
    Control("R40", "V1d", "a moved bracket declared and defined noexcept", ctl_r40, _epoch_only),
    Control("R41", "V12", "networkEngineSpec returns a reference to its local", ctl_r41, _epoch_only),
    Control("R42", "V3c", "NetworkEpoch's constructor noexcept", ctl_r42, _epoch_only),
    Control("R43", "V3d", "NetworkRun's constructor noexcept", ctl_r43, _epoch_only),
    Control("R44", "V1d", "NetworkRunEngines' constructor noexcept", ctl_r44, _epoch_only),
    Control("R45", "V3c", "x_emit() noexcept", ctl_r45, _epoch_only),
    # Hardening round 3 (R46 to R48 each a mutation the round-2 verifier and lint accepted;
    # R49 to R52 the same channels by another route).
    Control("R46", "V1e", "a narrowed seam overload in an included .inl file", ctl_r46, _epoch_only),
    Control("R47", "V1e", "a narrowed seam overload in the engine specification test, included", ctl_r47,
            _epoch_only),
    Control("R48", "V1e", "a per-source /fp:fast on the epoch's translation unit", ctl_r48, _epoch_only),
    Control("R49", "V9", "a narrowed seam overload in an included new header", ctl_r49,
            lambda v: v.epoch and any(v.by_name[n].mech not in (RUN, MEMBER) for n in v.converted)),
    Control("R50", "V1e", "a compile definition of a called seam's name", ctl_r50),
    Control("R51", "V1e", "a CMake line inside a C++ block comment", ctl_r51),
    Control("R52", "V1e", "a frozen #include removed", ctl_r52),
    # Hardening round 4 (R53 to R77 each a mutation the round-3 verifier and lint accepted;
    # R78 the route to R52 that building the fixes found).
    Control("R53", "V1b", "a binding under the CALLABLE name empties the carry", ctl_r53, _direct_conv),
    Control("R54", "V1b", "the same in a member seam with the fallback names", ctl_r54, _conv("networkPassB_")),
    Control("R55", "V1b", "a binding under a removed callback's name empties the carry", ctl_r55,
            _conv("networkSolveGlue")),
    Control("R56", "V1b", "a binding under the CALLABLE name writes the epoch through a const_cast", ctl_r56,
            _direct_conv),
    Control("R57", "V1b", "a binding under the CALLABLE name whose constructor truncates a trace", ctl_r57,
            _direct_conv),
    Control("R58", "V1b", "a binding under the CALLABLE name that adopts the product writer", ctl_r58,
            _direct_conv),
    Control("R59", "V1e", "a write of n_emitted behind a line splice", ctl_r59, _epoch_only),
    Control("R60", "V1e", "a return before the state snapshot behind a line splice", ctl_r60, _epoch_only),
    Control("R61", "V1e", "a Stop at the top of the epoch behind a line splice", ctl_r61, _epoch_only),
    Control("R62", "V1e", "a throw behind a line splice", ctl_r62, _epoch_only),
    Control("R63", "V1e", "a return below a seam's bindings behind a line splice", ctl_r63, _direct_conv),
    Control("R64", "V1e", "a second call of a seam behind a line splice", ctl_r64, _epoch_only),
    Control("R65", "V1e", "a Next before a Stop behind a line splice", ctl_r65, _epoch_only),
    Control("R66", "V1e", "a call in runNetwork's loop behind a line splice", ctl_r66),
    Control("R67", "V1e", "forward declarations and lone braces split a block", ctl_r67),
    Control("R68", "V1e", "a declaration wrapped in its own block (a later use reads another)", ctl_r68),
    Control("R69", "V1e", "a forward declaration and a `}{` line split a block", ctl_r69),
    Control("R70", "V1e", "the estimator's epoch-open guard split", ctl_r70),
    Control("R71", "V1e", "the setup's marker-to-ARP block split", ctl_r71),
    Control("R72", "V1e", "a local and its block wrapped (a later test reads the outer name)", ctl_r72),
    Control("R73", "V1e", "an #include and a `}{` line split a block", ctl_r73),
    Control("R74", "V1e", "a forward declaration as a braceless if's substatement", ctl_r74),
    Control("R75", "V1e", "a return in the epoch behind a comment ending in a carriage return", ctl_r75,
            _epoch_only),
    Control("R76", "V1e", "a scaling of the solution behind a comment ending in a carriage return", ctl_r76),
    Control("R77", "V1e", "a #define of a called seam behind a comment ending in a carriage return", ctl_r77,
            _epoch_only),
    Control("R78", "V1e", "#if 0 around a frozen #include", ctl_r78),
    # The EP1 review round (the dead aliases the move of the lambda left in runNetwork).
    Control("R79", "V1d", "an alias only the lambda read left in runNetwork", ctl_r79, _epoch_only),
)
ALSO = {"N8": ("V3c",), "N10": ("V3e",), "N16": ("V6",), "N4": ("V10",), "N13": ("V8",),
        "R35": ("V8",), "R36": ("V8",), "R37": ("V8",), "R38": ("V1d",),
        "R46": ("V9",), "R47": ("V9",), "R49": ("V1e",),
        "R53": ("V3a",), "R54": ("V3a",), "R55": ("V3a",), "R56": ("V3a",), "R57": ("V3a",), "R58": ("V3a",)}
# Controls that fire only when every named check fails (spec 5.7: N4 fails V3a at the solve's
# internal call "and V10"; R35 to R37 edit a seam's head and its contract-test alias together,
# which V2 and V8 each catch; R38 edits the epoch function's declaration, V1d's, and its
# definition, V1a's; R46, R47 and R49 carry an overload into the epoch's translation unit, which
# the include rule or the new-file rule of V1e and the overload rule of V9 each catch; R53 to
# R58 declare a binding under a name no frozen parameter of the seam asks for, which V1b's name
# rule and V3a's every-binding-compared rule each catch).
REQUIRE_ALL = {"N4", "R35", "R36", "R37", "R38", "R46", "R47", "R49", "R53", "R54", "R55", "R56", "R57", "R58"}
# Controls the permanent lint (scripts/nom_epoch_lint.py, run standalone on the mutated tree)
# must also fail (spec 5.7: N7 "V4 (and the lint)"; N16 is a driver write the lint's V4 owns;
# R24 to R26 and R46 to R50 change what the compiler builds from the epoch's translation unit,
# which the lint's V0 owns; so do R59 to R62, R64, R65, R75 and R77, code the compiler reads in
# that unit and the lint read as a comment).
LINT_CONTROLS = ("N7", "N16", "R24", "R25", "R26", "R46", "R47", "R48", "R49", "R50",
                 "R59", "R60", "R61", "R62", "R64", "R65", "R75", "R77")


def _lint_rc(tree, partial, presolve_writer):
    """The standalone lint's exit code on @p tree (its printed lines kept for the report)."""
    lines = []
    try:
        rc = lint.run(tree, out=lines.append, partial=partial, presolve_writer=presolve_writer)
    except Exception as ex:  # noqa: BLE001
        lines.append(f"lint raised {type(ex).__name__}: {ex}")
        rc = 3
    return rc, lines


def run_controls(fz, tree, rows, converted, args, out):
    base = Verifier(fz, tree, rows, converted, args.presolve_writer)
    base.frozen_rows()
    results = []
    # The lint on the unmutated candidate: it must pass before its failure on a control means
    # anything.  A tree part way through the conversion runs it with --partial, the card's
    # pre-solve form with --presolve-writer epoch (its V11 n/a).
    partial = len(base.converted) < len([r for r in rows if not isinstance(r.mech, Explicit)])
    lint_ok = None
    if base.epoch:
        rc, lines = _lint_rc(tree, partial, args.presolve_writer)
        lint_ok = rc == 0
        out(f"  lint on the candidate{' (--partial)' if partial else ''}: "
            f"{'PASS' if lint_ok else 'FAIL (rc %d)' % rc}")
        if not lint_ok:
            for ln in lines[1:]:
                out("    " + ln)
    else:
        out("  lint on the candidate: n/a (no epoch function)")
    for ctl in CONTROLS:
        try:
            ov, what = ctl.build(base)
        except Exception as ex:  # noqa: BLE001
            ov, what = None, f"the mutation could not be built ({type(ex).__name__}: {ex})"
        if ov is None:
            try:
                due = ctl.due(base)
            except Exception:  # noqa: BLE001
                due = True
            if due:
                results.append((ctl, "MISSING", what, []))
                out(f"  {ctl.cid:4s} {ctl.check:4s} MISSING   {ctl.what}: {what} (this stage must have the "
                    "control's target)")
            else:
                results.append((ctl, "pending", what, []))
                out(f"  {ctl.cid:4s} {ctl.check:4s} pending   {ctl.what}: {what}")
            continue
        mtree = tree.overlay(ov)
        t0 = time.time()
        try:
            v = Verifier(fz, mtree, rows, converted, args.presolve_writer)
            res = v.run()
            failed = res.failed()
        except SystemExit as ex:
            failed, res = ["(the mutated tree could not be read: %s)" % ex], None
        names = (ctl.check,) + ALSO.get(ctl.cid, ())
        fired = (all if ctl.cid in REQUIRE_ALL else any)(n in failed for n in names)
        lint_note = ""
        if ctl.cid in LINT_CONTROLS and lint_ok is not None:
            lrc, llines = _lint_rc(mtree, partial, args.presolve_writer)
            lint_fired = lint_ok and lrc == 1
            fired = fired and lint_fired
            lint_note = "; lint " + ("fails" if lint_fired else
                                     f"DOES NOT FAIL (rc {lrc}{', and fails unmutated' if not lint_ok else ''})")
        results.append((ctl, "fires" if fired else "DOES NOT FIRE", what, failed))
        first = ""
        if res is not None:
            for n in names:
                if res.problems.get(n):
                    first = res.problems[n][0]
                    break
        out(f"  {ctl.cid:4s} {ctl.check:4s} {'fires    ' if fired else 'NOT FIRED'} {what}; failing: "
            f"{', '.join(failed) or 'none'}{lint_note} ({time.time() - t0:.0f} s)")
        if first:
            out(f"       {first[:160]}")
    return results


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------
def pick_rows(names):
    """The row modules named by --rows: space- or comma-separated (`app solve` or
    `app,solve`, the generator's form), `all` for every row file."""
    names = [n for x in (names or []) for n in x.split(",") if n]
    if not names:
        return []
    if "all" in names:
        return list(ROW_MODULES)
    out = []
    for n in names:
        stem = pathlib.Path(n).stem
        stem = stem if stem.startswith("rows_") else "rows_" + stem
        if stem not in ROW_MODULES:
            raise SystemExit(f"nom_ep1_verify: unknown row file {n!r} (one of {', '.join(ROW_MODULES)})")
        out.append(stem)
    return out


def check_manifest(v, path):
    """Pairing cross-check against a manifest scripts/nom_ep1_cut.py wrote (V8): its base, its
    converted seams, the frozen lambda's and the epoch function's line ranges, and each seam
    entry's frozen and new declaration / definition lines must be the ones the verifier found
    by itself.  No binding is read from it."""
    R = v.R
    name_ = pathlib.Path(path).name
    try:
        m = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as ex:
        R.fail("V8", f"manifest {name_}: unreadable ({ex})")
        return
    if not isinstance(m, dict):
        R.fail("V8", f"manifest {name_}: not a JSON object")
        return
    sha = v.fz.tree.sha
    if m.get("base_sha") and not sha.startswith(str(m["base_sha"])) and not str(m["base_sha"]).startswith(sha):
        R.fail("V8", f"manifest {name_}: written at base {m.get('base_sha')}, verified against {sha}")
    conv = set(m.get("converted") or [])
    if conv != v.converted:
        R.fail("V8", f"manifest {name_}: converts {sorted(conv - v.converted)} beyond --rows/--seams and "
                     f"not {sorted(v.converted - conv)}")
    ef = m.get("epoch_function") or {}
    fz = v.fz
    lam = [fz.drv.T.line(fz.rn.lam_open), fz.drv.T.line(fz.rn.lam_close)]
    if ef.get("frozen_lambda") and list(ef["frozen_lambda"]) != lam:
        R.fail("V8", f"manifest {name_}: frozen lambda {ef['frozen_lambda']}, the verifier found {lam}")
    if v.epoch and ef.get("function_lines"):
        T = v.cand.T
        got = [T.line(decl_start(T, v.cand.fn.func)), T.line(v.cand.close)]
        fl = list(ef["function_lines"])
        others = [f.line for f in v.tree.index(v.cand.fn.path).funcs
                  if f is not v.cand.fn.func and got[1] < f.line <= fl[1]]
        if not (got[0] <= fl[0] <= T.line(v.cand.open)) or fl[1] < got[1] or others:
            R.fail("V8", f"manifest {name_}: epoch function lines {fl}, the verifier found {got}")
    n = 0
    for e in m.get("seams", []):
        name = e.get("seam") or e.get("name")
        if not name or name not in v.converted or name not in v.by_name:
            continue
        r = v.by_name[name]
        fdecl, fdef = v.fdecls.get(name, (None, None))
        kind, path_, nl, fl = e.get("kind"), e.get("file"), e.get("new_line"), e.get("frozen_line")
        if kind == "declaration":
            idx = v.tree.index(path_)
            found = [f.line for f in (idx.funcs if idx else []) if f.name == name and not f.is_def
                     and f.params and lint.is_epoch_form(f)]
            frozen = fdecl.line if fdecl else None
        elif kind in ("definition", "adapter definition"):
            found = [f.line for f in v.new_defs(name) if f.path == path_]
            frozen = fdef.line if fdef else None
            if kind == "adapter definition":
                fl = frozen
        else:
            continue
        n += 1
        if nl is not None and nl not in found:
            R.fail("V8", f"manifest {name_}: {name} {kind} at {path_}:{nl}, the verifier found "
                         f"{', '.join(map(str, found)) or 'none'}")
        if fl is not None and frozen is not None and fl != frozen and r.mech is not None:
            R.fail("V8", f"manifest {name_}: {name} frozen {kind} line {fl}, the verifier found {frozen}")
    R.note("V8", f"manifest {name_}: {n} seam entries paired")


def this_stage_manifests(v, args):
    """The tree's manifests (scripts/nom_ep1/manifest_*.json) written for the stage being
    verified: those whose converted seams equal --rows / --seams and whose pre-solve form is
    --presolve-writer.  A tree keeps the manifests of earlier stages (step EP1a-1's beside a
    worker's), which describe other conversions; they are listed in V8 and not paired."""
    out = []
    for p in sorted((pathlib.Path(args.tree) / "scripts" / "nom_ep1").glob("manifest_*.json")):
        try:
            m = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            out.append(str(p))           # unreadable: check_manifest reports it
            continue
        if not isinstance(m, dict) or (set(m.get("converted") or []) == v.converted
                                       and m.get("presolve_writer", "outputs") == args.presolve_writer):
            out.append(str(p))
        else:
            v.R.note("V8", f"manifest {p.name}: another stage's ({len(m.get('converted') or [])} seams, "
                           f"{m.get('presolve_writer', 'outputs')}), not paired")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", required=True, help="the candidate source tree (a directory)")
    ap.add_argument("--base", default=DEFAULT_BASE, help=f"frozen revision <B> (default {DEFAULT_BASE})")
    ap.add_argument("--rows", nargs="*", default=[], help="row files the tree has converted (app solve emit "
                    "diag, or all); default none")
    ap.add_argument("--seams", default="", help="further seams the tree has converted, comma-separated "
                    "(a worker part way through its rows)")
    ap.add_argument("--presolve-writer", choices=("outputs", "epoch"), default="outputs")
    ap.add_argument("--manifest", help="the generator's manifest (pairing cross-check only)")
    ap.add_argument("--controls", action="store_true", help="also run the negative controls of spec 5.7")
    ap.add_argument("--only", help="comma-separated checks to run (default all)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    t0 = time.time()
    global SUBST
    fz = Frozen(args.base)          # binds the tables to the base (nom_ep1.bind_base)
    SUBST = Substitutions()         # the declared substitutions that apply at the base
    rows = load_rows(ROW_MODULES)
    converted = [r.name for r in load_rows(pick_rows(args.rows))]
    names = {r.name for r in rows}
    for x in filter(None, args.seams.split(",")):
        if x not in names:
            ap.error(f"--seams: {x} is not a row of the canonical table")
        if x not in converted:
            converted.append(x)
    tree = DiskTree(args.tree)
    only = set(args.only.split(",")) if args.only else None
    v = Verifier(fz, tree, rows, converted, args.presolve_writer, args.manifest, args.verbose)
    print(f"NOM3 step EP1 relocation verifier: base {args.base} ({fz.tree.sha[:10]}), tree {args.tree}")
    print(f"  candidate shape: {v.cand.mode}"
          + (f" ({v.cand.fn.path})" if v.epoch else " (the lambda still in runNetwork: an empty conversion)")
          + f"; converted rows: {', '.join(pick_rows(args.rows)) or 'none'}"
          + (f" + seams {args.seams}" if args.seams else "") + f" ({len(converted)} seams)")
    for x in fz.fit.summary():
        print(f"  {x}")
    res = v.run(only)
    mpaths = [args.manifest] if args.manifest else this_stage_manifests(v, args)
    for mp in mpaths:
        check_manifest(v, mp)
        res.done("V8")
    for c in CHECKS:
        if only and c not in only:
            continue
        st = res.status.get(c, "n/a")
        print(f"  [{st if st != 'pass' else 'pass'}] {c}: " + "; ".join(res.info[c]))
        for p in res.problems[c][:40]:
            print("      " + p)
        if len(res.problems[c]) > 40:
            print(f"      ... {len(res.problems[c]) - 40} more")
    failed = res.failed()
    print(f"verification: {'PASS' if not failed else 'FAIL (' + ', '.join(failed) + ')'} "
          f"({time.time() - t0:.0f} s)")
    rc = 1 if failed else 0
    if args.controls:
        print("\nNegative controls (spec 5.7): each must fail its named check")
        results = run_controls(fz, tree, rows, converted, args, print)
        ran = [x for x in results if x[1] != "pending"]
        nf = [x for x in ran if x[1] != "fires"]
        print(f"controls: {len(ran) - len(nf)} of {len(ran)} fire, {len(results) - len(ran)} pending"
              + (f"; NOT FIRED: {', '.join(x[0].cid for x in nf)}" if nf else ""))
        if nf:
            rc = 1
    fz.tree.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
