#!/usr/bin/env python3
"""Generator of card NOM3 step EP1, the seam conversion (specification section 5).

    python scripts/nom_ep1_cut.py --survey [--base REV] [--rows app,solve,emit,diag]
        [--compact] [--write-compact PATH] [--compare-compact PATH]
        [--write-renames PATH] [--compare-renames PATH]
    python scripts/nom_ep1_cut.py --stage ep1a1 [--base REV] [--root DIR] [--check]
    python scripts/nom_ep1_cut.py --stage ep1b [--rows app,solve,emit,diag]
        [--presolve-writer outputs|epoch] [--base REV] [--root DIR] [--check]
    python scripts/nom_ep1_cut.py --shared-only --rows <merged rows> [--base REV] [--root DIR]

The frozen source <B> is read from git at --base (default nom_ep1.DEFAULT_BASE); no copy is
stored and no frozen file is read from the working tree.  Every mode derives, never reads from
a row, each call site and each binding (the survey below).

Generation (--stage, --shared-only).  Every output file is the frozen file with the edits of
the transforms of specification 5.3 applied once (scripts/nom_ep1/gen.py and its gen_epoch,
gen_seams and gen_setup modules); the generator never reads its own output, so a second run
writes nothing (idempotence), and --check writes nothing and exits 1 when a file in the tree
differs from what the generator would write (this stage's manifest included when the tree has
it; an absent manifest is reported, not failed).  Files are written under --root (default this
working tree) with each file's own line ends kept.  --rows takes comma-separated row names
(app,solve,emit,diag) or `all`, in any order: the row files are taken in canonical order, so
a union of merged workers' rows writes the same files and manifest name however it is typed.
  --stage ep1a1   step EP1a-1: the types (app/NetworkRun.hpp, app/NetworkEpoch.hpp,
                  filter/NetworkRunGroups.hpp: NetworkEngineSpec and the owning
                  NetworkRunEngines), the run's construction in runNetwork with the engines'
                  constructor arguments cut into networkEngineSpec (app/NetworkRun.cpp), the
                  bracket functions, the post-fit hand-off, the third-band predicate, the
                  epoch function app/ApplicationNetworkEpoch.cpp with every call still explicit
                  (T-A, T-B, T-E), the contract test (types only), Application.hpp and CMake.
  --stage ep1b    EP1a-1 plus the seams of --rows converted (T-C, T-D, T-F): signatures,
                  Doxygen (check V7), preambles, adapters' epoch forms; the two shared files
                  regenerated for those rows.
  --shared-only   only the two shared files (the epoch function and the contract test) for
                  --rows, the union of the merged workers' rows (specification 7, merge risk 1).
  --presolve-writer epoch   the card's form for the writers before the solve
                  (`NetworkEpoch& ep`, no named outputs; author decision 1).
A run writes scripts/nom_ep1/manifest_<rows>.json under --root: the base, the rows, every
written file with its line count and digest, the frozen ranges each transform resolved, each
converted call (frozen line, old and new arguments), each seam's preamble lines, and the
specification cut's ranges; the relocation verifier reads it for pairing only.  A frozen object
missing at the base, a binding the tables cannot realize, an edit that overlaps another, or a
name the moved text still reads after its declaration is removed refuses the run and writes
nothing.

What a clean survey proves at a base (exit 0 only when every point holds):
  1. Each row's seam exists: the frozen lambda calls it as many times as the row says, its
     header declares it (as an Application member for MEMBER rows, as a free function
     otherwise), its source defines it, and the definition's parameters have the
     declaration's kinds.
  2. Each frozen parameter of each converted call site has one binding, derived from the
     argument by the scope walk of scripts/nom_cpp.py and the realization tables of
     scripts/nom_ep1/model.py: run.* for the run's names, ep.<group>.<member> for the
     lambda's locals, the output name for an epoch member written through a non-const
     parameter.  An argument outside the fold grammar is refused unless the parameter is
     kept; a parameter whose binding differs between two call sites is refused; a kept
     parameter that is equal at every site is reported.
  3. Each row's outputs are exactly the outputs its bindings write, in parameter order (the
     solve's also the outputs of the builder its replay now calls); after the solve's call
     site only the hand-off and the prepared solution are written; an observer row returns
     void and writes and keeps nothing; a run-only row reads no epoch member; an ADAPTER row
     has another caller in src/, include/ or tests/ and a DIRECT, MEMBER or RUN row has
     none.
  4. Every call of the call table (scripts/nom_cpp.py SEAM_CALL: the 99 calls of the EP1
     inventory at the EP0 base) is covered exactly once, by a row or by a declared
     non-conversion (the input stage, an EP2 block, a method call): check V8.  With --rows
     naming only some row files, coverage is printed but not required.
  5. Every realization the bindings used is backed by a declared substitution of
     scripts/nom_ep1/declared.py, and every declared entry's locators find their frozen
     object (file:line printed).  The proofs of COPY_SOURCE and VALUE_EQUAL hold at the base
     (declared.Writers: no write to a copied setup field outside networkSetup_; no writer of
     ctx.counters.n_emitted the per-epoch body reaches but the emitter, used once); each
     write found and its verdict are printed.
  6. With --compare-compact / --compare-renames, the derived bindings and renamed-argument
     list equal the EP1 design's frozen tables (its conv_compact.md and renames.txt, made by
     the inventory scripts at the EP0 base); a declaration that only moved to another line is
     listed, not failed.  With partial --rows, only the call sites those rows cover are
     compared.
  7. The engine and specification tables of scripts/nom_ep1/model.py fit the base: each
     ENGINE_CTORS entry's frozen declaration exists, default-constructed exactly when the
     table lists no arguments and otherwise with as many constructor arguments as it lists;
     each SPEC_FIELDS local is declared in runNetwork and moved by a SPEC_CUT entry.  These
     are the shapes the generator refuses, reported here first.
  8. The entries marked retired_by=REMOVE_KEY (rows, declared entries, realization tables)
     fit the base: each one's object is either present (the entry applies as written) or gone
     (the entry does not apply; listed under "Retired at this base"), never partly gone (a
     half-applied deletion fails the survey).  The tables are bound to the base first
     (nom_ep1.bind_base), so points 1 to 7 read only the entries that apply there, and the
     same tables pass at the EP0 base and at the base after the REMOVE-key deletion.

Also printed: the seams whose definitions declare or use `run`, `ep` or one of their group
outputs' names (they take the fallback names nrun / nep / <output>_out, spec 3.2), the epoch
members' frozen declarations against their transforms, and the realization entries no binding
used.  A seam in a row that is missing at the base, and a frozen call no row covers, are
reported, never a crash.
"""
import argparse
import collections
import difflib
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from nom_cpp import (DRIVER_PATH, Driver, GitTree, Ref, Tokens, calls_in, split_args)  # noqa: E402
from nom_ep1 import DEFAULT_BASE, REMOVE_KEY, ROW_MODULES, bind_base, load_rows  # noqa: E402
from nom_ep1 import model, declared  # noqa: E402
from nom_ep1.model import MEMBER, RUN, ADAPTER, DIRECT, Explicit  # noqa: E402

FREE_NAMES = ("run", "ep")


class Frozen:
    """The frozen base: the git tree, the driver, the tables bound to the base
    (nom_ep1.bind_base; fit records what that decided) and a resolver stopping at the realized
    names."""

    def __init__(self, base):
        self.base = base
        self.tree = GitTree(base)
        self.drv = Driver(self.tree)
        self.fit = bind_base(self)
        self.res = self.drv.resolver(stop=model.stop_keys())


class Binding:
    """One frozen parameter at one call site.  text: the argument's normalized frozen text;
    span: its token span; node: its resolved binding; rendered: the binding realized through
    the tables (None for a kept or removed parameter); out: the output it writes through, or
    None; gtext: the group member text of a group output (None for a single-member output)."""
    __slots__ = ("param", "text", "node", "disp", "value", "out", "problems", "used",
                 "rendered", "span", "gtext")

    def __init__(self, param, text, node):
        self.param, self.text, self.node = param, text, node
        self.disp = self.value = ""
        self.out = None
        self.problems = []
        self.used = set()
        self.rendered = None
        self.span = None
        self.gtext = None


class RowResult:
    def __init__(self, row):
        self.row = row
        self.sites = []
        self.decl = self.defn = None
        self.params = []
        self.bindings = []         # per site: [Binding]
        self.param_outs = []
        self.outs = []
        self.refusals = []         # argument-level: fold grammar, differing bindings
        self.problems = []         # row-level: structure, outputs, mechanism
        self.notes = []
        self.missing = []
        self.fallback = []         # (name, line) for the name pre-flight
        self.other_callers = {}


# ---------------------------------------------------------------------------------------------
# Rows
# ---------------------------------------------------------------------------------------------
def _pick_decl(funcs, nargs):
    funcs = [f for f in funcs if f.params is not None]
    for f in funcs:
        req = sum(1 for p in f.params if not p.has_default)
        if nargs is None or req <= nargs <= len(f.params):
            return f
    return funcs[0] if funcs else None


def bind(fz, row, p, s, e):
    T = fz.drv.T
    b = Binding(p, T.norm(s, e), None)
    b.span = (s, e)
    b.node = fz.res.resolve(s, e)
    if p.name in row.kept:
        b.disp = f"kept: `{b.text}`"
        b.value = ("kept", b.text)
        return b
    rem = model.REMOVED_BY_PARAM.get((row.name, p.name))
    if p.kind == "F" or rem is not None:
        if rem is None:
            b.problems.append(f"{p.name}: a std::function parameter with no declared removal")
            b.disp = f"`{b.text}`"
        else:
            n = b.node
            if not (isinstance(n, Ref) and n.root == rem.root and not n.chain and not n.prefix):
                b.problems.append(f"{p.name}: binds {n.render()}, the removal expects {rem.root}")
            b.used.add(("REMOVED", rem.seam, rem.param))
            b.disp = f"(removed: {rem.note})"
        b.value = b.disp
        return b
    if model.value_equal(b.text):
        rendered = model.value_equal(b.text)
        b.used.add(("VALUE_EQUAL", b.text))
    else:
        fp = model.fold_problem(b.node)
        if fp:
            b.problems.append(f"{p.name} = `{b.text}`: outside the fold grammar: {fp}")
        rendered, rp = model.render(b.node, b.used)
        b.problems.extend(f"{p.name} = `{b.text}`: {x}" for x in rp)
    b.disp = f"`{rendered}`"
    b.rendered = rendered
    if model.is_epoch_path(rendered):
        if row.mech == RUN:
            b.problems.append(f"{p.name}: a run-only seam reads the epoch ({rendered})")
        if p.kind in ("W", "W*"):
            body = rendered.lstrip("&*")
            o, gtext = model.output_for(body, row.outs)
            if o is None:
                b.out = "UNDECLARED:" + str(model.default_output(body))
                b.problems.append(f"{p.name}: writes {body} through a non-const parameter and no "
                                  f"output of the row covers it (the group would be "
                                  f"{model.default_output(body)})")
            elif gtext is None:
                b.out = o
                b.disp = f"**out `{o}`** (the call passes `{rendered}`)"
            else:
                b.out = o
                b.gtext = gtext
                b.disp = f"**{gtext}** (out)"
    b.value = b.disp
    return b


def preflight(fz, row, defn):
    T = fz.tree.tokens(defn.path)
    names = set(FREE_NAMES) | (set(row.outs) & set(model.GROUP_OUTPUTS))
    out = []
    for j in range(defn.paren[0], defn.body[1]):
        if T.k(j) == "id" and T.t(j) in names and T.t(j - 1) not in (".", "->", "::"):
            hit = (T.t(j), T.line(j))
            if hit not in out:
                out.append(hit)
    return out


def survey_row(fz, row, block_spans):
    rr = RowResult(row)
    drv = fz.drv
    rr.sites = [c for c in drv.calls if c.name == row.name]
    if isinstance(row.mech, Explicit):
        if not rr.sites:
            rr.missing.append("the frozen lambda does not call it")
        elif len(rr.sites) != row.sites:
            rr.problems.append(f"the frozen lambda calls it {len(rr.sites)} times; the row says {row.sites}")
        rr.notes.append(f"explicit, keeps its text: {row.mech.reason}")
        return rr
    for c in rr.sites:
        if any(s <= c.tok < e for s, e in block_spans):
            rr.problems.append(f"called inside an EP2 block ({DRIVER_PATH}:{c.line})")
    if not rr.sites:
        rr.missing.append("the frozen lambda does not call it")
    elif len(rr.sites) != row.sites:
        rr.problems.append(f"the frozen lambda calls it {len(rr.sites)} times; the row says {row.sites}")
    cls = "Application" if row.mech == MEMBER else ""
    hidx = fz.tree.index(row.hpp)
    if hidx is None:
        rr.missing.append(f"{row.hpp} does not exist")
        decls = []
    else:
        decls = [f for f in hidx.funcs if f.name == row.name and (f.cls or "") == cls]
        if not decls:
            rr.missing.append(f"{row.hpp} declares no {'member ' if cls else ''}{row.name}")
    nargs = rr.sites[0].nargs if rr.sites else None
    rr.decl = _pick_decl(decls, nargs)
    cidx = fz.tree.index(row.cpp)
    if cidx is None:
        rr.missing.append(f"{row.cpp} does not exist")
        defs = []
    else:
        defs = [f for f in cidx.funcs if f.name == row.name and f.is_def and (f.cls or "") == cls]
        if not defs:
            rr.missing.append(f"{row.cpp} defines no {row.name}")
    if rr.decl and defs:
        same = [f for f in defs if f.params is not None and len(f.params) == len(rr.decl.params)]
        rr.defn = same[0] if same else defs[0]
        dp, fp_ = rr.decl.params, rr.defn.params or []
        if len(dp) != len(fp_):
            rr.problems.append(f"declaration has {len(dp)} parameters, definition {len(fp_)}")
        for a, b in zip(dp, fp_):
            if a.kind != b.kind:
                rr.problems.append(f"parameter {a.name}: declared [{a.kind}], defined [{b.kind}]")
            elif a.name != b.name:
                rr.notes.append(f"parameter named {a.name} in the declaration, {b.name} in the definition")
    if rr.missing:
        return rr
    rr.params = rr.decl.params
    for c in rr.sites:
        if c.nargs != len(rr.params):
            unpassed = [p.name for p in rr.params[c.nargs:]]
            rr.problems.append(f"{DRIVER_PATH}:{c.line} passes {c.nargs} of {len(rr.params)} "
                               f"arguments (defaults used for {', '.join(unpassed)})")
        site = []
        for k, p in enumerate(rr.params[:c.nargs]):
            s, e = c.args[k]
            site.append(bind(fz, row, p, s, e))
        rr.bindings.append(site)
    for k, p in enumerate(rr.params):
        vals = [site[k] for site in rr.bindings if k < len(site)]
        if not vals:
            continue
        for i, b in enumerate(vals):   # vals[i] is the binding at call site i
            rr.refusals.extend(f"{DRIVER_PATH}:{rr.sites[i].line}: {x}" for x in b.problems)
        distinct = sorted({b.value for b in vals}, key=str)
        if p.name in row.kept:
            if len(vals) > 1 and len(distinct) == 1:
                rr.notes.append(f"kept parameter {p.name} has the same argument at every site")
        elif len(distinct) > 1:
            rr.refusals.append(f"parameter {p.name} binds differently at its call sites: "
                               + " / ".join(str(d) for d in distinct))
    for kp in row.kept:
        if kp not in [p.name for p in rr.params]:
            rr.problems.append(f"kept parameter {kp} is not a frozen parameter")
    if rr.bindings:
        for b in rr.bindings[0]:
            if b.out and b.out not in rr.param_outs:
                rr.param_outs.append(b.out)
    ret = (rr.decl.ret or "").split()
    if row.observer:
        if not ret or ret[-1] != "void":
            rr.problems.append(f"observer returns {' '.join(ret)}, not void")
        if row.outs or row.kept:
            rr.problems.append("observer with outputs or kept parameters")
    if row.mech == RUN and row.outs:
        rr.problems.append("run-only row with outputs")
    rr.fallback = preflight(fz, row, rr.defn)
    return rr


def fallback_text(hits):
    """`ep` line 780 (34 uses), ... : the first line of each name and its count."""
    first, count = {}, collections.Counter()
    for a, b in hits:
        first.setdefault(a, b)
        count[a] += 1
    return ", ".join(f"`{a}` line {first[a]} ({count[a]} uses)" for a in first)


def finish_outputs(results, drv):
    """Derived outputs in parameter order; a removed callback adds its builder's outputs."""
    by_name = {rr.row.name: rr for rr in results}
    solve_idx = next((k for k, c in enumerate(drv.calls) if c.name == model.SOLVE_CALLEE), None)
    for rr in results:
        rr.outs = list(rr.param_outs)
        for b in (rr.bindings[0] if rr.bindings else ()):
            rem = model.REMOVED_BY_PARAM.get((rr.row.name, b.param.name))
            if rem is not None and rem.builder and rem.builder in by_name:
                for o in by_name[rem.builder].param_outs:
                    if o not in rr.outs:
                        rr.outs.append(o)
        if rr.missing or isinstance(rr.row.mech, Explicit):
            continue
        if tuple(rr.outs) != tuple(rr.row.outs):
            rr.problems.append(f"outputs derived {tuple(rr.outs)}, the row says {tuple(rr.row.outs)}")
        if solve_idx is not None and rr.sites:
            first = drv.calls.index(rr.sites[0])
            if first > solve_idx:
                bad = [o for o in rr.outs if o not in model.POST_SOLVE_OUTPUTS]
                if bad:
                    rr.problems.append(f"writes {bad} after the solve (only "
                                       f"{model.POST_SOLVE_OUTPUTS} may be)")


def other_callers(fz, results):
    """Calls of each row's seam outside the frozen lambda in src/, include/ and tests/."""
    names = {rr.row.name for rr in results}
    drv = fz.drv
    found = collections.defaultdict(collections.Counter)
    for path in fz.tree.files("src", "include", "tests"):
        if not path.endswith((".cpp", ".hpp", ".h")):
            continue
        text = fz.tree.read(path)
        if not text or not any(n in text for n in names):
            continue
        T = fz.tree.tokens(path)
        idx = fz.tree.index(path)
        decl_toks = {f.name_tok for f in idx.funcs}
        for c in calls_in(T, 0, T.n):
            leaf = re.split(r"\.|->|::", c.name)[-1]
            if leaf not in names or "." in c.name or "->" in c.name:
                continue
            if c.tok in decl_toks:
                continue
            if path == DRIVER_PATH and drv.lam_open < c.tok < drv.lam_close:
                continue
            found[leaf][path] += 1
    for rr in results:
        rr.other_callers = dict(found.get(rr.row.name, {}))
        if rr.missing or isinstance(rr.row.mech, Explicit):
            continue
        if rr.row.mech == ADAPTER and not rr.other_callers:
            rr.problems.append("an ADAPTER row whose seam has no other caller (DIRECT would do)")
        if rr.row.mech in (DIRECT, MEMBER, RUN) and rr.other_callers:
            rr.problems.append("other callers " + ", ".join(f"{p} ({n})" for p, n in rr.other_callers.items())
                               + f": a {rr.row.mech} row must be an ADAPTER")


# ---------------------------------------------------------------------------------------------
# Coverage, declared entries, realization tables
# ---------------------------------------------------------------------------------------------
def coverage(fz, rows):
    drv = fz.drv
    blocks = declared.ep2_blocks(fz)
    by_name = collections.defaultdict(list)
    for r in rows:
        by_name[r.name].append(r)
    out = []
    for n, c in enumerate(drv.calls, 1):
        who = []
        blk = next((lab for lab, spans in blocks.items() if any(s <= c.tok < e for s, e in spans)), None)
        if blk:
            who.append(("EP2_BLOCK", blk))
        else:
            for r in by_name.get(c.name, ()):
                who.append(("ROW", f"{r.worker} {r.form}"))
            for nc in declared.NON_CONVERSIONS:
                if nc.kind != "EP2_BLOCK" and nc.covers(c, ()):
                    who.append((nc.kind, nc.label))
        out.append((n, c, who))
    return out, blocks


def locate_declared(fz):
    rows = []
    for d in declared.declared():
        hits = d.locate(fz)
        rows.append((d, hits))
    return rows


def engine_table_checks(fz):
    """Problems of model.ENGINE_CTORS and model.SPEC_FIELDS at the base: each engine's frozen
    declaration exists in runNetwork's outermost scope, is default-constructed when the table
    lists no arguments, and otherwise is direct-initialized with as many arguments as the
    table lists; each field that replaces a setup local names a local declared there and
    moved by a SPEC_CUT entry."""
    drv = fz.drv
    T = drv.T
    root = {d.name: d for d in drv.scopes.root.decls}
    out = []
    for ec in model.ENGINE_CTORS:
        d = root.get(ec.local)
        if d is None:
            out.append(f"engine {ec.local} ({ec.owner}.{ec.member}): not declared in runNetwork at this base")
            continue
        if d.init is None:
            if ec.arg_fields:
                out.append(f"engine {ec.local}: default-constructed at the base, the table lists "
                           f"{len(ec.arg_fields)} arguments")
            continue
        if d.init_kind not in ("(", "{"):
            out.append(f"engine {ec.local}: not direct-initialized at the base")
            continue
        n = len(split_args(T, d.init[0] - 1))
        if n != len(ec.arg_fields):
            out.append(f"engine {ec.local}: {n} constructor arguments at the base, the table lists "
                       f"{len(ec.arg_fields)}")
    cut = set()
    for e in declared.declared("SPEC_CUT"):
        for loc in e.where:
            cut |= set(getattr(loc, "names", ()))
    for f in model.SPEC_FIELDS:
        if not f.local:
            continue
        if f.local not in root:
            out.append(f"specification field {f.name}: its setup local {f.local} is not declared in "
                       "runNetwork at this base")
        elif f.local not in cut:
            out.append(f"specification field {f.name}: its setup local {f.local} is in no SPEC_CUT entry")
    return out


def spec_cut_stays(fz, hits):
    """Driver statements inside a SPEC_CUT entry's overall span that the entry leaves in place."""
    if not hits:
        return []
    T = fz.drv.T
    lo, hi = min(h.line for h in hits), max(h.end for h in hits)
    out = []
    for s, e in fz.drv.driver_statements():
        a, b = T.line(s), T.line(e - 1)
        if lo <= a <= hi and not any(h.line <= a <= h.end for h in hits):
            out.append((a, b, T.norm(s, min(e, s + 8))))
    return out


def _shape_problem(m, d, ty, want, init):
    """Why the frozen declaration @p d cannot be realized by @p m's transform ('' when it can)."""
    def const_of(t):
        return "const" + (" " if t[:1].isalnum() else "") + t
    if m.transform == "CLOCK":          # the const local stays; ep.clock stores a copy (T-B6)
        if ty != const_of(want):
            return f"type {ty}, expected the const local {const_of(want)}"
        return "" if d.is_const and d.init_kind == "=" else "not a const local with an initializer"
    if m.transform == "RESULT":         # `[const] T x = seam(...)`: the driver's store (T-B7)
        if ty not in (want, const_of(want)):
            return f"type {ty}, member {want}"
        return "" if d.init_kind == "=" and "(" in init else "not initialized by a call"
    if ty != want:
        return f"type {ty}, member {want}"
    if m.transform in ("ALIAS", "WS"):   # default-constructed (T-B2, T-B3)
        return f"initialized ({init}); the transform expects default construction" if d.init else ""
    if m.transform == "SIZED":          # constructed with arguments (T-B4)
        return "" if d.init_kind == "(" else "not constructed with arguments"
    if m.transform == "SCALAR":         # initializer equal to the member default (T-B5)
        return "" if init == m.default else f"initializer {init}, member default {m.default}"
    if m.transform in ("ACCESSOR", "CTOR"):   # a reference bound once (T-B8, T-B1)
        return "" if d.is_ref and d.init_kind == "=" else "not a reference with an initializer"
    return f"unknown transform {m.transform}"


def epoch_member_checks(fz):
    """Each realized lambda local's frozen declaration against its transform (the shape the
    verifier's check V3c relies on): exactly one top-level declaration, the member's type
    text, and the initializer form the transform rewrites."""
    T = fz.drv.T
    out = []
    for m in model.EPOCH_MEMBERS:
        ds = [d for d in fz.drv.lam_scope.decls if d.name == m.local]
        if len(ds) != 1:
            out.append((m, None, f"{len(ds)} top-level declarations"))
            continue
        d = ds[0]
        ty = T.join(*d.type_span) + d.ops
        mt = Tokens(m.type)
        want = mt.join(0, len(mt))
        init = T.norm(*d.init) if d.init else ""
        out.append((m, d, _shape_problem(m, d, ty, want, init)))
    return out


def realization_report(results):
    used = set()
    for rr in results:
        for site in rr.bindings:
            for b in site:
                used |= b.used
    problems, unused = [], []
    for k in sorted(x for x in used if isinstance(x, str)):
        if k in model.EPOCH_BY_KEY:
            m = model.EPOCH_BY_KEY[k]
            cls = m.declared_class
            if cls and not declared.find(cls, m.local):
                problems.append(f"{k} realized as {m.path} with no declared {cls} entry")
        elif k in model.RUN_BY_KEY:
            m = model.RUN_BY_KEY[k]
            if m.cls == "OWNER_MOVE" and not declared.find("OWNER_MOVE", m.name):
                problems.append(f"{k} realized as {m.path} with no declared OWNER_MOVE entry")
    for x in used:
        if isinstance(x, tuple):
            kind = x[0]
            if kind == "FIELD_MOVE" and not declared.find("FIELD_MOVE", x[1].split(".")[-1]):
                problems.append(f"field move of {x[1]} with no declared entry")
            if kind == "COPY_SOURCE" and not declared.find("COPY_SOURCE", x[1].split(".")[-1]):
                problems.append(f"copy source {x[1]} with no declared entry")
            if kind == "VALUE_EQUAL" and not declared.find("VALUE_EQUAL", "emitted"):
                problems.append("VALUE_EQUAL used with no declared entry")
            if kind == "REMOVED":
                rem = model.REMOVED_BY_PARAM[(x[1], x[2])]
                if not declared.find(rem.cls, x[2]):
                    problems.append(f"removal of {x[1]}({x[2]}) with no declared {rem.cls} entry")
    for m in model.EPOCH_MEMBERS:
        if m.key not in used:
            unused.append(f"{m.key} -> {m.path}")
    for m in model.RUN_MEMBERS:
        if m.frozen not in used:
            unused.append(f"{m.frozen} -> {m.path}")
    # The reverse direction: a declared binding-class entry with no realization in model.py.
    for d in declared.declared():
        if d.cls in ("EPOCH_MEMBER", "ACCESSOR") and "L:" + d.name not in model.EPOCH_BY_KEY:
            problems.append(f"declared {d.cls} {d.name} has no realization in model.py")
        if d.cls == "OWNER_MOVE" and "D:" + d.name not in model.RUN_BY_KEY:
            problems.append(f"declared OWNER_MOVE {d.name} has no realization in model.py")
        if d.cls == "COPY_SOURCE" and d.name not in model.COPY_SOURCES:
            problems.append(f"declared COPY_SOURCE {d.name} has no realization in model.py")
        if d.cls in ("CALLABLE", "CALLBACK_DIRECT") and not any(
                r.param == d.name and r.cls == d.cls for r in model.REMOVED_PARAMS):
            problems.append(f"declared {d.cls} {d.name} has no removal in model.py")
    return problems, unused


# ---------------------------------------------------------------------------------------------
# The design's frozen tables: compact bindings and renamed arguments
# ---------------------------------------------------------------------------------------------
COMPACT_HEAD = ["| # | seam (declared in) | form | parameter = binding [frozen kind] |",
                "|---|---|---|---|"]


def compact_rows(fz, results):
    drv = fz.drv
    lines = []
    for rr in results:
        if rr.missing or not rr.bindings:
            continue
        hdr = rr.decl.path[len("include/"):] if rr.decl.path.startswith("include/") else rr.decl.path
        for c, site in zip(rr.sites, rr.bindings):
            n = drv.calls.index(c) + 1
            cells = "; ".join(f"{b.param.name} [{b.param.kind}] = {b.disp.replace('|', chr(92) + '|')}"
                              for b in site)
            lines.append((n, f"| {n} | `{rr.row.name}` ({hdr}:{rr.decl.line}) | {rr.row.form} | {cells} |"))
    return [ln for _, ln in sorted(lines)]


_COMPACT_RE = re.compile(r"^\| (\d+) \| `(\w+)` \(([^:]+):(\d+)\) \| (\w+) \| (.*) \|$")


def compare_compact(mine, path, partial=False):
    want = pathlib.Path(path).read_text(encoding="utf-8").splitlines()
    parse = lambda ls: {m.group(1): m.groups() for m in map(_COMPACT_RE.match, ls) if m}  # noqa: E731
    a, b = parse(mine), parse(want)
    if partial:
        b = {k: v for k, v in b.items() if k in a}
    diffs, moved = [], []
    for k in sorted(set(a) | set(b), key=int):
        x, y = a.get(k), b.get(k)
        if x is None or y is None:
            diffs.append(f"#{k}: {'missing here' if x is None else 'not in ' + path}")
            continue
        if (x[1], x[2], x[4], x[5]) != (y[1], y[2], y[4], y[5]):
            diffs.append(f"#{k} {x[1]}:")
            for d in difflib.ndiff(y[5].split("; "), x[5].split("; ")):
                if d[:1] in "+-?":
                    diffs.append("    " + d)
            if (x[1], x[2], x[4]) != (y[1], y[2], y[4]):
                diffs.append(f"    seam/header/form here {x[1]} {x[2]} {x[4]}, table {y[1]} {y[2]} {y[4]}")
        elif x[3] != y[3]:
            moved.append(f"#{k} {x[1]}: {x[2]}:{y[3]} -> {x[3]}")
    return diffs, moved, len(a), len(b)


def renames(fz, results):
    """(total arguments, [seam: param <- frozen text]) over the converted call sites."""
    drv = fz.drv
    items, total = [], 0
    for rr in results:
        if rr.missing:
            continue
        for c, site in zip(rr.sites, rr.bindings):
            for b in site:
                total += 1
                ids = re.findall(r"[A-Za-z_]\w*", b.text)
                leaf = ids[-1] if ids else ""
                if model.value_equal(b.text):
                    leaf = model.value_equal(b.text).split(".")[-1]
                if leaf != b.param.name and b.text not in ("true", "false"):
                    items.append((drv.calls.index(c), f"{rr.row.name}: {b.param.name} <- {b.text}"))
    return total, [t for _, t in sorted(items, key=lambda x: x[0])]


def compare_renames(total, items, path):
    lines = pathlib.Path(path).read_text(encoding="utf-8").splitlines()
    m = re.match(r"args (\d+) bound under another name (\d+)", lines[0]) if lines else None
    want = [ln.strip() for ln in lines[1:] if ln.strip()]
    diffs = []
    if not m or int(m.group(1)) != total or int(m.group(2)) != len(items):
        diffs.append(f"header: here args {total} renamed {len(items)}; table {lines[0] if lines else '(empty)'}")
    for d in difflib.unified_diff(want, items, "table", "survey", lineterm="", n=0):
        diffs.append(d)
    return diffs


def row_names(arg):
    """The row modules --rows names, in the canonical order of ROW_MODULES (so a union of
    merged workers' rows gives the same files and manifest name in whichever order it is
    typed): comma-separated short names (app,solve,emit,diag), `rows_` names, or `all`;
    every row file when --rows is not given."""
    if not arg:
        return ROW_MODULES
    want = set()
    for n in filter(None, (x.strip() for x in arg.split(","))):
        if n == "all":
            return ROW_MODULES
        stem = n if n.startswith("rows_") else "rows_" + n
        if stem not in ROW_MODULES:
            raise SystemExit(f"nom_ep1_cut: unknown row file {n!r} (one of {', '.join(ROW_MODULES)}, or all)")
        want.add(stem)
    return tuple(m for m in ROW_MODULES if m in want)


# ---------------------------------------------------------------------------------------------
# Survey
# ---------------------------------------------------------------------------------------------
def survey(args):
    names = row_names(args.rows)
    fz = Frozen(args.base)          # binds the tables to the base first (point 8)
    rows = load_rows(names)
    drv, T = fz.drv, fz.drv.T
    problems = list(fz.fit.problems)

    print(f"NOM3 step EP1 survey at {args.base} ({fz.tree.sha[:10]})")
    print(f"  frozen driver {DRIVER_PATH}: runNetwork lines {T.line(drv.rn.body[0])}-"
          f"{T.line(drv.rn.body[1])}, lambda {T.line(drv.lam_open)}-{T.line(drv.lam_close)}")
    net = [c for c in drv.calls if c.name.startswith("network")]
    print(f"  call table: {len(drv.calls)} calls, {len(net)} network* with "
          f"{sum(c.nargs for c in net)} arguments")

    # Row table sanity.
    seen = collections.Counter(r.name for r in rows)
    for n, k in seen.items():
        if k > 1:
            problems.append(f"row {n} appears {k} times")
    for r in rows:
        if r.mech not in model.MECHS and not isinstance(r.mech, Explicit):
            problems.append(f"row {r.name}: mechanism {r.mech} is not one of {model.MECHS}")
        if r.worker not in model.WORKERS:
            problems.append(f"row {r.name}: worker {r.worker!r}")
        for o in r.outs:
            if o not in model.OUTPUTS:
                problems.append(f"row {r.name}: output {o} is not an epoch output")

    blocks = declared.ep2_blocks(fz)
    block_spans = [sp for spans in blocks.values() for sp in spans]
    results = [survey_row(fz, r, block_spans) for r in rows]
    finish_outputs(results, drv)
    other_callers(fz, results)

    by_worker = collections.Counter(r.worker for r in rows)
    by_mech = collections.Counter(r.mech for r in rows)
    conv = [rr for rr in results if not rr.missing and not isinstance(rr.row.mech, Explicit)]
    conv_sites = sum(len(rr.sites) for rr in conv)
    conv_args = sum(len(site) for rr in results for site in rr.bindings)
    new_args = sum((1 if rr.row.mech == RUN else 2) + len(rr.row.kept) + len(rr.row.outs)
                   for rr in conv for _ in rr.sites)
    print(f"  rows: {len(rows)} (" + ", ".join(f"{w} {by_worker[w]}" for w in model.WORKERS) + "); "
          + ", ".join(f"{m} {by_mech[m]}" for m in model.MECHS)
          + (f", EXPLICIT {sum(1 for r in rows if isinstance(r.mech, Explicit))}"
             if any(isinstance(r.mech, Explicit) for r in rows) else "")
          + f"; {sum(1 for r in rows if r.observer)} observers")
    print(f"  converted call sites: {conv_sites}, carrying {conv_args} arguments today and "
          f"{new_args} after the conversion")

    # What the base decided about the entries the REMOVE-key deletion retires (point 8).
    print(f"\n## Retired at this base ({REMOVE_KEY})\n")
    fit_lines = fz.fit.lines()
    print("  none: every object the deletion retires is present" if not fit_lines
          else "\n".join("  " + x for x in fit_lines))
    for x in fz.fit.problems:
        print(f"  HALF APPLIED: {x}")

    # Per-row table.
    print("\n## Rows against the base\n")
    print("| # | line | seam | form | worker | args | outputs | kept | declared | defined | fallback names |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for rr in sorted(results, key=lambda r: drv.calls.index(r.sites[0]) if r.sites else 10 ** 6):
        r = rr.row
        if rr.missing:
            print(f"| - | - | `{r.name}` | {r.form} | {r.worker} | | | | MISSING: {'; '.join(rr.missing)} | | |")
            continue
        if isinstance(r.mech, Explicit):
            for c in rr.sites:
                print(f"| {drv.calls.index(c) + 1} | {c.line} | `{r.name}` | explicit | {r.worker} | "
                      f"{c.nargs} | - | - | (keeps its text) | | |")
            continue
        first = {}
        for a, b in rr.fallback:
            first.setdefault(a, b)
        for c in rr.sites:
            n = drv.calls.index(c) + 1
            fb = (", ".join(f"{a}@{b}" for a, b in first.items())
                  if rr.fallback and r.mech != ADAPTER else "-")
            print(f"| {n} | {c.line} | `{r.name}` | {r.form}{' (observer)' if r.observer else ''} | "
                  f"{r.worker} | {c.nargs} | {', '.join(rr.outs) or '-'} | {', '.join(r.kept) or '-'} | "
                  f"{rr.decl.path}:{rr.decl.line} | {rr.defn.path}:{rr.defn.line} | {fb} |")

    # Coverage (V8).
    cov, _ = coverage(fz, rows)
    unc = [(n, c) for n, c, who in cov if not who]
    dbl = [(n, c, who) for n, c, who in cov if len(who) > 1]
    kinds = collections.Counter(who[0][0] for n, c, who in cov if len(who) == 1)
    print("\n## Coverage of the call table (V8)\n")
    print(f"  {len(cov)} calls: rows {kinds['ROW']}, input stage {kinds['INPUT']}, EP2 blocks "
          f"{kinds['EP2_BLOCK']}, method calls {kinds['METHOD']}; uncovered {len(unc)}; "
          f"covered twice {len(dbl)}")
    for lab, spans in blocks.items():
        for s, e in spans:
            inside = [c.name for c in drv.calls if s <= c.tok < e]
            print(f"  EP2 block, {lab}: {DRIVER_PATH}:{T.line(s)}-{T.line(e - 1)}, {len(inside)} calls: "
                  + ", ".join(inside))
        if not spans:
            print(f"  EP2 block, {lab}: MISSING at this base (its anchor callee is not called)")
            problems.append(f"EP2 block {lab}: anchor not found")
    partial = set(names) != set(ROW_MODULES)
    for n, c in unc:
        msg = (f"call #{n} {c.name} ({DRIVER_PATH}:{c.line}) is covered by no row and no "
               "declared non-conversion")
        if partial:
            print(f"  (partial rows) {msg}")
        else:
            problems.append(msg)
    for n, c, who in dbl:
        problems.append(f"call #{n} {c.name} ({DRIVER_PATH}:{c.line}) is covered {len(who)} times: {who}")

    # Declared substitutions.
    print("\n## Declared substitutions (spec 5.6), located at the base\n")
    decl_rows = locate_declared(fz)
    loop = declared.driver_loop(drv)
    counts = collections.Counter(d.cls for d, _ in decl_rows)
    print("  " + ", ".join(f"{c} {counts[c]}" for c in declared.CLASSES))
    for d, hits in decl_rows:
        parts = []
        for loc, hs in hits:
            if not hs:
                parts.append(f"{type(loc).__name__}: MISSING")
                problems.append(f"declared {d.cls} {d.name}: {type(loc).__name__} finds nothing")
                continue
            if len(hs) > 4 and d.cls != "SPEC_CUT":
                parts.append(f"{type(loc).__name__}: {len(hs)} hits, {hs[0].path}:{hs[0].line} .. "
                             f"{hs[-1].path}:{hs[-1].line}")
            else:
                parts.append(f"{type(loc).__name__}: " + ", ".join(
                    f"{h.path}:{h.line}{'-' + str(h.end) if h.end != h.line else ''}"
                    + (f" [{h.note}]" if (type(loc).__name__ == "Writers"
                                          or "initializer" in h.note) else "")
                    for h in hs))
            if type(loc).__name__ == "DriverDecl":
                for h in hs:
                    if "(expected" in h.note:
                        problems.append(f"declared {d.cls} {d.name}: {h.note}")
        print(f"  {d.cls:15s} {d.name}: " + "; ".join(parts))
        if d.cls == "SPEC_CUT":
            for a, b, head in spec_cut_stays(fz, [h for _, hs in hits for h in hs]):
                print(f"  {'':15s}   stays: {a}-{b} `{head}`")
        if d.cls == "BRACKET":
            n_sites = sum(len(hs) for loc, hs in hits if type(loc).__name__ == "CallSitesOf")
            print(f"  {'':15s}   {n_sites} call sites in the lambda")
        for loc in d.where:
            if isinstance(loc, declared.Writers):
                wp, wn = loc.prove(fz.tree, loop)
                for x in wn:
                    print(f"  {'':15s}   proof: {x}")
                for x in wp:
                    print(f"  {'':15s}   proof FAILS: {x}")
                    problems.append(f"declared {d.cls} {d.name}: {x}")

    # The engine and specification tables against the base (the generator's member_inits and
    # spec_body refuse the same shapes; the survey reports them first).
    print("\n## Engines and specification fields (model.py) against the base\n")
    for x in engine_table_checks(fz):
        print(f"  {x}")
        problems.append(x)
    print(f"  {len(model.ENGINE_CTORS)} engines, {len(model.SPEC_FIELDS)} specification fields checked")

    # Non-conversions located.
    print("\n## Declared non-conversions (the calls each covers in the coverage above)\n")
    for nc in declared.NON_CONVERSIONS:
        if nc.kind == "EP2_BLOCK":
            hit = [c for n, c, who in cov if ("EP2_BLOCK", nc.label) in who]
        else:
            hit = [c for n, c, who in cov if not any(w[0] == "EP2_BLOCK" for w in who)
                   and nc.covers(c, ())]
        lines = sorted({c.line for c in hit})
        print(f"  {nc.kind:9s} {nc.label}: {nc.match} ({len(hit)} calls"
              + (f", lines {lines[0]}..{lines[-1]}" if lines else "") + f"): {nc.reason}")
        if not hit:
            print(f"  {'':9s}   (nothing matches at this base)")

    # Realization tables.
    print("\n## Realization tables (model.py) against the declared entries\n")
    rprob, unused = realization_report(results)
    problems.extend(rprob)
    print(f"  realization entries no binding used: {', '.join(unused) if unused else 'none'}")
    em = epoch_member_checks(fz)
    for m, d, prob in em:
        if prob:
            problems.append(f"epoch member {m.local} ({m.transform}): {prob}")
    print(f"  epoch members checked against their transforms: {len(em)}, "
          f"{sum(1 for _, _, p in em if p)} disagree")
    for m in model.RUN_MEMBERS:
        kind, name = m.frozen.split(":", 1)
        found = (any(d.name == name for d in drv.scopes.root.decls) if kind == "D"
                 else name in drv.members)
        if not found:
            print(f"  run member {m.frozen} -> {m.path}: not declared at this base")

    # Name pre-flight.
    print("\n## Name pre-flight (spec 3.2): seams whose frozen definitions declare or use "
          "`run`, `ep` or a group output's name\n")
    fb = [rr for rr in results if rr.fallback and rr.row.mech != ADAPTER]
    for rr in fb:
        print(f"  {rr.row.name} ({rr.defn.path}): {fallback_text(rr.fallback)}")
    print(f"  {len(fb)} seams take the fallback names nrun / nep / <output>_out")
    for rr in results:
        if rr.fallback and rr.row.mech == ADAPTER:
            print(f"  (adapter {rr.row.name}: its explicit body, untouched, uses "
                  f"{fallback_text(rr.fallback)}; the epoch form is a new forwarding body)")

    # Renamed bindings.
    total, items = renames(fz, results)
    print(f"\n## Arguments bound under another name\n\n  args {total} bound under another name {len(items)}")
    for t in items:
        print("     " + t)

    # Refusals and problems.
    refusals = [(rr.row.name, x) for rr in results for x in rr.refusals]
    print("\n## Refusals\n")
    print("  none" if not refusals else "\n".join(f"  {n}: {x}" for n, x in refusals))
    row_problems = [(rr.row.name, x) for rr in results for x in rr.problems]
    missing = [(rr.row.name, x) for rr in results for x in rr.missing]
    print("\n## Rows missing at the base\n")
    print("  none" if not missing else "\n".join(f"  {n}: {x}" for n, x in missing))
    notes = [(rr.row.name, x) for rr in results for x in rr.notes]
    if notes:
        print("\n## Notes\n")
        print("\n".join(f"  {n}: {x}" for n, x in notes))

    comp = compact_rows(fz, results)
    if args.compact:
        print("\n## Bindings (the design's conv_compact.md form)\n")
        print("\n".join(COMPACT_HEAD + comp))
    if args.write_compact:
        pathlib.Path(args.write_compact).write_text("\n".join(COMPACT_HEAD + comp) + "\n", encoding="utf-8")
    if args.write_renames:
        pathlib.Path(args.write_renames).write_text(
            f"args {total} bound under another name {len(items)}\n" + "".join(f"   {t}\n" for t in items),
            encoding="utf-8")
    print("\n## Comparisons\n")
    if args.compare_compact:
        diffs, moved, nm, nw = compare_compact(comp, args.compare_compact, partial)
        print(f"  bindings against {args.compare_compact}: {nm} rows here, {nw} there, "
              + ("equal" if not diffs else f"{len(diffs)} difference lines"))
        for d in diffs:
            print("    " + d)
            problems.append(f"compact: {d}")
        for m in moved:
            print(f"    declaration moved (bindings equal): {m}")
    if args.compare_renames and partial:
        print("  renamed arguments: not compared (partial rows)")
    elif args.compare_renames:
        diffs = compare_renames(total, items, args.compare_renames)
        print(f"  renamed arguments against {args.compare_renames}: "
              + ("equal" if not diffs else f"{len(diffs)} difference lines"))
        for d in diffs:
            print("    " + d)
            problems.append(f"renames: {d}")
    if not args.compare_compact and not args.compare_renames:
        print("  (none requested)")

    all_problems = ([f"{n}: {x}" for n, x in row_problems] + [f"{n}: MISSING {x}" for n, x in missing]
                    + problems)
    print("\n## Problems\n")
    print("  none" if not all_problems else "\n".join("  " + p for p in all_problems))
    fz.tree.close()
    bad = bool(refusals or all_problems)
    print(f"\nsurvey: {'FAIL' if bad else 'PASS'} ({len(refusals)} refusals, {len(all_problems)} problems)")
    return 1 if bad else 0


# ---------------------------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------------------------
def manifest_name(args, rows_names):
    """manifest_<rows>.json: ep1a1, or the row files' short names joined by '_', with a
    shared_ prefix for --shared-only and an _epoch suffix for --presolve-writer epoch."""
    if args.stage == "ep1a1" and not args.shared_only:
        name = "ep1a1"
    else:
        name = "_".join(n[len("rows_"):] for n in rows_names)
        if args.shared_only:
            name = "shared_" + name
    if args.presolve_writer == "epoch":
        name += "_epoch"
    return f"manifest_{name}.json"


def locate_new_lines(outputs, epoch_cpp, epoch_rec, seam_rec):
    """Adds to the manifest records the line each object has in the generated text: each
    converted call in the epoch function (frozen order), each converted declaration and
    definition (and the line after a definition's preamble), each adapter's epoch form."""
    lines = outputs[epoch_cpp].split("\n")
    k = epoch_rec["function_lines"][0] - 1
    for c in sorted(epoch_rec["calls"], key=lambda c: c["frozen_line"]):
        needle = c["seam"] + "("
        while k < len(lines) and needle not in lines[k]:
            k += 1
        c["new_line"] = k + 1 if k < len(lines) else None
        k += 1
    for r in seam_rec:
        text = outputs.get(r["file"])
        if text is None:
            continue
        pat = re.compile(r"\b" + re.escape(r["seam"]) + r"\(NetworkRun& n?run\b")
        m = pat.search(text)
        r["new_line"] = text.count("\n", 0, m.start()) + 1 if m else None
        if m and r["kind"] == "definition":
            end = text.find("// ---- End of the bindings ----", m.end())
            r["body_first_new_line"] = text.count("\n", 0, end) + 2 if end >= 0 else None


def generate(args):
    """--stage / --shared-only: the generated tree, written under --root (see the banner)."""
    from nom_ep1 import gen as gen_mod, gen_epoch, gen_seams, gen_setup, textops
    from nom_ep1 import docs as docs_mod
    root = pathlib.Path(args.root).resolve()
    stage = "ep1b" if args.shared_only else args.stage
    names = row_names(args.rows)
    fz = Frozen(args.base)          # binds the tables to the base (nom_ep1.bind_base)
    rows = [] if stage == "ep1a1" else load_rows(names)
    print(f"NOM3 step EP1 generator at {args.base} ({fz.tree.sha[:10]}): stage {stage}"
          + (" (shared files only)" if args.shared_only else "")
          + f", rows {', '.join(names) if rows else '(none)'}, pre-solve writers "
          f"{args.presolve_writer}; target {root}")
    for x in fz.fit.summary():      # in full: the survey, and the manifest's retired_at_base
        print(f"  {x}")
    if fz.fit.problems:
        print("generator: the REMOVE-key deletion is half applied at this base; nothing written")
        for x in fz.fit.problems:
            print("  " + x)
        fz.tree.close()
        return 1
    blocks = declared.ep2_blocks(fz)
    block_spans = [sp for spans in blocks.values() for sp in spans]
    results = [survey_row(fz, r, block_spans) for r in rows]
    finish_outputs(results, fz.drv)
    other_callers(fz, results)
    bad = [f"{rr.row.name}: {x}" for rr in results for x in rr.missing + rr.problems + rr.refusals]
    if bad:
        print("generator: the survey refuses these rows; nothing written")
        for x in bad:
            print("  " + x)
        fz.tree.close()
        return 1
    target_files = {p for p in (gen_mod.ENGINE_SPEC_TEST,) if (root / p).exists()}
    try:
        g = gen_mod.Gen(fz, results, args.presolve_writer, stage, target_files)
        epoch_text, epoch_rec, _ = gen_epoch.epoch_function(g)
        seam_rec = gen_seams.seams(g)
        setup_rec = gen_setup.setup(g)
        contract = gen_seams.contract_test(g)
        outputs = {}
        for path, edits in sorted(g.edits.items()):
            frozen = fz.tree.read(path)
            if frozen is None:
                raise gen_mod.GenError(f"{path} does not exist at the base")
            outputs[path] = textops.apply_edits(frozen, edits)
        outputs.update(g.new)
        outputs[gen_mod.EPOCH_CPP] = epoch_text
        outputs[gen_mod.CONTRACT_TEST] = contract
        # The comment rewrites (nom_ep1/docs.py), last, on the finished text; the epoch
        # function's lines are then re-read from that text.
        try:
            doc_skipped = []
            doc_rec = docs_mod.apply(outputs, fz.tree.read, set(g.plans), g.problems,
                                     deletion_landed=bool(model.ABSENT), skipped=doc_skipped)
        except ValueError as ex:
            raise gen_mod.GenError(str(ex))
        head = "Application::LoopSignal Application::networkProcessEpoch_("
        etext = outputs[gen_mod.EPOCH_CPP]
        epoch_rec["function_lines"] = [etext[:etext.index(head)].count("\n") + 1, etext.count("\n")]
    except (gen_mod.GenError, textops.EditError) as ex:
        print(f"generator: REFUSED: {ex}")
        fz.tree.close()
        return 1
    except (KeyError, IndexError, ValueError, AttributeError, TypeError):
        # A frozen shape the generator does not recognize at this base (the REMOVE-key
        # deletion, a moved seam): reported with its trace, nothing written.
        import traceback
        print("generator: REFUSED: the base has a shape the generator does not recognize; "
              "nothing written.  Trace:")
        traceback.print_exc(file=sys.stdout)
        fz.tree.close()
        return 1
    locate_new_lines(outputs, gen_mod.EPOCH_CPP, epoch_rec, seam_rec)
    # The stale-spelling census (nom_ep1/docs.py STALE) over the whole generated tree, before
    # --shared-only narrows what is written: the docs pass's worklist, never a failure.
    texts = {p: fz.tree.read(p) for p in fz.tree.files("src/", "include/", "tests/")}
    texts.update(outputs)
    stale = docs_mod.census(texts)
    if args.shared_only:
        outputs = {k: v for k, v in outputs.items() if k in (gen_mod.EPOCH_CPP, gen_mod.CONTRACT_TEST)}
    driver_bytes = textops.read_target(root, DRIVER_PATH)
    default_eol = textops.detect_eol(driver_bytes) if driver_bytes else "\n"
    files = {}
    changed = 0
    for path in sorted(outputs):
        text = outputs[path]
        old = textops.read_target(root, path)
        eol = textops.detect_eol(old) if old else default_eol
        status = textops.write_target(root, path, text, eol, check=args.check)
        changed += status != "unchanged"
        frozen = fz.tree.read(path)
        files[path] = {"kind": "new" if frozen is None else "edited",
                       "lines": text.count("\n"),
                       "frozen_lines": None if frozen is None else frozen.count("\n"),
                       "edits": len(g.edits.get(path, ())), "doc_hunks": doc_rec.get(path, 0),
                       "sha256_lf": gen_mod._sha(text)}
        print(f"  {status:9s} {path} ({files[path]['kind']}, {files[path]['lines']} lines"
              + (f", frozen {files[path]['frozen_lines']}" if frozen is not None else "") + ")")
    ef = epoch_rec["function_lines"]
    print(f"  epoch function: lines {ef[0]}-{ef[1]} of {gen_mod.EPOCH_CPP} "
          f"({ef[1] - ef[0] + 1} lines; preamble {epoch_rec['preamble_count']} names; frozen lambda "
          f"{epoch_rec['frozen_lambda'][0]}-{epoch_rec['frozen_lambda'][1]})")
    print(f"  doc hunks (nom_ep1/docs.py): {sum(doc_rec.values())} applied in {len(doc_rec)} files"
          + (f"; {len(doc_skipped)} written for the base after the REMOVE-key deletion, not applied "
             "at this one" if doc_skipped else ""))
    by_label = collections.Counter(x[0] for x in stale)
    print(f"  stale-spelling census (the docs pass's worklist, nom_ep1/docs.py STALE): {len(stale)} lines"
          + "".join(f"; {n} {label}" for label, n in by_label.items()))
    for label, path, ln, text in stale:
        print(f"    {path}:{ln}: {text[:110]}")
    for x in g.problems:
        print(f"  problem: {x}")
    manifest = {
        "tool": "scripts/nom_ep1_cut.py", "base": args.base, "base_sha": fz.tree.sha,
        "stage": stage, "shared_only": bool(args.shared_only), "rows": list(names) if rows else [],
        "presolve_writer": args.presolve_writer,
        "converted": sorted(g.plans), "files": files,
        "epoch_function": epoch_rec, "seams": seam_rec, "setup": setup_rec,
        "spec_cut": [[fz.drv.T.line(s), fz.drv.T.line(e - 1), n] for s, e, n in getattr(g, "cut", [])],
        "problems": g.problems,
        "doc_hunks": doc_rec,
        "doc_hunks_after_deletion_only": doc_skipped,
        "stale_spellings": [[label, path, ln] for label, path, ln, _ in stale],
    }
    if fz.fit.lines():
        # The entries the REMOVE-key deletion retired at this base (absent at the EP0 base,
        # where every object exists).
        manifest["retired_at_base"] = fz.fit.lines()
    mpath = pathlib.Path(args.manifest) if args.manifest else \
        root / "scripts" / "nom_ep1" / manifest_name(args, names)
    mtext = json.dumps(manifest, indent=1, ensure_ascii=False) + "\n"
    mold = textops.read_target(mpath.parent, mpath.name)
    meol = textops.detect_eol(mold) if mold else "\n"
    if args.check and mold is None:
        # A tree that never ran this stage (a merged tree checked with --shared-only, an
        # EP1a-1 tree checked for a worker's rows) has no manifest of it: nothing to compare,
        # and nothing the verifier would pair.
        mstatus = "absent"
    else:
        mstatus = textops.write_target(mpath.parent, mpath.name, mtext, meol, check=args.check)
    print(f"  {mstatus:9s} manifest {mpath}")
    fz.tree.close()
    if args.check and (changed or mstatus == "differs"):
        print(f"generator --check: {changed} file(s)"
              + (" and the manifest" if mstatus == "differs" else "") + " differ from the generated text")
        return 1
    print(f"generator: {'checked' if args.check else 'done'} ({changed} file(s) "
          f"{'differ' if args.check else 'written'}, {len(outputs) - changed} unchanged)")
    return 1 if g.problems and not args.allow_problems else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--survey", action="store_true", help="resolve every row against the base")
    ap.add_argument("--stage", choices=("ep1a1", "ep1b"),
                    help="generate: ep1a1 (types, run construction, epoch function, every call "
                         "explicit) or ep1b (also the rows' seams converted)")
    ap.add_argument("--shared-only", action="store_true",
                    help="regenerate only the two shared files (the epoch function, the contract "
                         "test) for --rows")
    ap.add_argument("--presolve-writer", choices=("outputs", "epoch"), default="outputs",
                    help="pre-solve writers take named group outputs (default) or the card's "
                         "NetworkEpoch&")
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[1]),
                    help="the tree to write (default: this repository's working tree)")
    ap.add_argument("--check", action="store_true",
                    help="write nothing; exit 1 when a generated file differs from the tree's")
    ap.add_argument("--manifest", metavar="PATH",
                    help="manifest path (default <root>/scripts/nom_ep1/manifest_<rows>.json)")
    ap.add_argument("--allow-problems", action="store_true",
                    help="exit 0 although the generator reported a problem (a doc hunk not found)")
    ap.add_argument("--base", default=DEFAULT_BASE, help=f"frozen revision <B> (default {DEFAULT_BASE})")
    ap.add_argument("--rows", help="comma-separated row files to load (app,solve,emit,diag, or all; default all)")
    ap.add_argument("--compact", action="store_true", help="print the bindings in the design's compact form")
    ap.add_argument("--write-compact", metavar="PATH", help="write the compact bindings table to PATH")
    ap.add_argument("--compare-compact", metavar="PATH", help="compare the bindings with a compact table")
    ap.add_argument("--write-renames", metavar="PATH", help="write the renamed-argument list to PATH")
    ap.add_argument("--compare-renames", metavar="PATH", help="compare the renamed-argument list with PATH")
    args = ap.parse_args(argv)
    modes = sum(bool(x) for x in (args.survey, args.stage, args.shared_only))
    if modes != 1:
        ap.error("give exactly one of --survey, --stage {ep1a1,ep1b}, --shared-only")
    if args.survey:
        return survey(args)
    return generate(args)


if __name__ == "__main__":
    sys.exit(main())
