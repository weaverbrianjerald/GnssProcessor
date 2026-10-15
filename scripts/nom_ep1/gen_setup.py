"""Step EP1a-1's edits outside the epoch function (card NOM3 step EP1, T-G and specification 2).

setup(g) adds to g.edits and g.new:
  - runNetwork (specification 4.1): the run built after the traces; each run-owned engine or
    group's declaration a reference to its member (OWNER_MOVE) where the remaining text reads
    it; the engines' constructor arguments cut into networkEngineSpec (SPEC_CUT), the locals
    still read bound to the specification's fields; the third-band closure, the bracket
    closures and their timer vectors, the lambda and its enum gone; the aliases only the lambda
    read gone (UNREAD_ALIAS, the review round), no unread declaration left; the epoch source
    calls networkProcessEpoch_; the run end binds the run's groups;
  - src/core/app/NetworkRun.cpp (new): networkEngineSpec and networkSubBegin / networkSubEnd,
    each body the frozen text under a preamble that repeats the declarations it reads;
  - include/filter/NetworkRunGroups.hpp: NetworkEngineSpec, NetworkRunEngines as the owner,
    the bracket scratch in NetworkRunTiming;
  - include/app/NetworkRun.hpp and include/app/NetworkEpoch.hpp (new, specification 2.1, 2.3);
  - the post-fit hand-off in NetworkSolveGlue.{hpp,cpp} (FIELD_MOVE), the third-band predicate
    beside the DD build (CALLABLE), one knob comment in NetworkRunContext.hpp, the epoch
    function's declaration in Application.hpp, and the CMake source and test lists.
"""
import re

from nom_cpp import DRIVER_PATH, APPLICATION_HPP, LAMBDA_NAME, calls_in, split_args, parse_declaration
from nom_ep1 import model, declared
from nom_ep1 import templates as tpl
from nom_ep1.gen import (GenError, WIDTH, BRACKETS, CALLABLE_LOCAL, TIMER_FIELDS, LAMBDA_ENUM,
                         RUN_CPP, RUN_HPP, EPOCH_HPP, GROUPS_HPP, RUNCTX_HPP, SOLVE_HPP, SOLVE_CPP,
                         DDB_HPP, DDB_CPP, CONTRACT_TEST, ENGINE_SPEC_TEST, CMAKE, PREDICATE,
                         comment_block)
from nom_ep1.gen_seams import namespace_close
from nom_ep1.textops import Edit, Lines, leading_comment, collapse_ws, dedent, doc_before, removed_mask

RUN_BUILD = ("    // The run (app/NetworkRun.hpp, card NOM3 step EP1): the references every epoch reads\n"
             "    // and the engines and groups the run owns, each built here in place from the engines'\n"
             "    // constructor arguments (networkEngineSpec, a pure function of the configuration and\n"
             "    // the knobs).  The setup below configures the owned engines through references.\n"
             "    const NetworkEngineSpec engine_spec = networkEngineSpec(config_, nc, ctx);\n"
             "    NetworkRun run(ctx, config_, nc, log, traces, setup, log_dir_, engine_spec);\n")
LAMBDA_NOTE = ("    // === One synced epoch through the full producer =========================\n"
               "    // Application::networkProcessEpoch_ (ApplicationNetworkEpoch.cpp, card NOM3 step EP1)\n"
               "    // runs one synced epoch over the run built above; the epoch source below calls it.\n")
TEARDOWN_GROUPS = {"engines": "run.engines"}


def has_callable():
    """Whether the bound base has the third-band closure (CALLABLE; the REMOVE-key deletion
    takes it with the EWL lane)."""
    return any(r.cls == "CALLABLE" and r.param == CALLABLE_LOCAL for r in model.REMOVED_PARAMS)


def setup(g):
    rec = {}
    rec["run_network"] = run_network(g)
    rec["groups"] = groups_hpp(g)
    # The groups NetworkRun constructs itself (emit, and lanes while the base has the lane run).
    run_hpp = tpl.at_base("NETWORK_RUN_HPP", model.ABSENT)
    for ec in model.ENGINE_CTORS:
        if ec.owner == "run":
            run_hpp = run_hpp.replace(f"{ec.member}@@{ec.member.upper()}_INIT@@", g.member_init[ec.member])
    if "@@" in run_hpp:
        raise GenError(f"{RUN_HPP}: a member initializer of the template has no engine at the base")
    g.new[RUN_HPP] = run_hpp
    g.new[EPOCH_HPP] = tpl.at_base("NETWORK_EPOCH_HPP", model.ABSENT)
    check_epoch_template(g)
    rec["solve_handoff"] = solve_handoff(g)
    if has_callable():
        rec["predicate"] = predicate(g)
        rec["runctx_comment"] = runctx_comment(g)
    rec["application_hpp"] = application_hpp(g)
    rec["cmake"] = cmake(g)
    return rec


# ---------------------------------------------------------------------------------------------
# runNetwork
# ---------------------------------------------------------------------------------------------
def cut_statements(g):
    """[(s, e, entry name)] of the SPEC_CUT statements, in frozen order."""
    T, drv = g.T, g.drv
    out = []
    for d in declared.declared("SPEC_CUT"):
        for loc in d.where:
            names = loc.names
            for s, e in drv.driver_statements():
                if s >= drv.lam_decl.tok:
                    break
                ds = parse_declaration(T, s, e) or []
                if any(x.name in names for x in ds) or loc._assigns(T, s, e):
                    out.append((s, e, d.name))
    out.sort()
    seen, uniq = set(), []
    for s, e, n in out:
        if (s, e) not in seen:
            seen.add((s, e))
            uniq.append((s, e, n))
    if not uniq:
        raise GenError("SPEC_CUT: no statement found at the base")
    return uniq


def arg_parts(g, opener):
    """[(code start, code end, leading inline comments, leading line comments)] of the
    arguments of the bracket at token @p opener."""
    T, text = g.T, g.text
    out = []
    prev = T.toks[opener].end
    for s, e in split_args(T, opener):
        gap = text[prev:T.toks[s].pos]
        inline = re.findall(r"/\*.*?\*/", gap, re.S)
        line = [m.strip()[2:].strip() for m in re.findall(r"//[^\n]*", gap)]
        out.append((s, e, inline, line))
        prev = T.toks[e].end if T.t(e) == "," else T.toks[e - 1].end
    return out


def member_inits(g):
    """For every engine of model.ENGINE_CTORS: the initializer text of its member in
    NetworkRunEngines / NetworkRun, and the specification assignments its arguments need."""
    T, text = g.T, g.text
    field_locals = model.SPEC_FIELD_BY_LOCAL
    inits, assigns = {}, {}
    for ec in model.ENGINE_CTORS:
        d = g.root_decl(ec.local)
        prefix = "s." if ec.owner == "engines" else "spec."
        if d.init is None:
            if ec.arg_fields:
                raise GenError(f"{ec.local}: default-constructed at the base, the table lists arguments")
            inits[ec.member] = f"{ec.member}()"
            continue
        opener = d.init[0] - 1
        close = {"(": ")", "{": "}"}[d.init_kind] if d.init_kind in ("(", "{") else None
        if close is None:
            raise GenError(f"{ec.local}: not direct-initialized at the base")
        parts = arg_parts(g, opener)
        if len(parts) != len(ec.arg_fields):
            raise GenError(f"{ec.local}: {len(parts)} constructor arguments at the base, the table lists "
                           f"{len(ec.arg_fields)}")
        args, need = [], []
        for k, (s, e, inline, line) in enumerate(parts):
            keys = {g.key_at(j) for j in range(s, e)} - {None}
            locals_ = {k2[2:] for k2 in keys if k2.startswith("D:")}
            members = {k2 for k2 in keys if k2.startswith("A:")}
            fld = ec.arg_fields[k]
            if not members and locals_ <= set(field_locals):
                # Literal or field locals only: stays in the initializer, field locals rewritten.
                code = text[T.toks[s].pos:T.toks[e - 1].end]
                for j in sorted(range(s, e), reverse=True):
                    kk = g.key_at(j)
                    if kk and kk[2:] in field_locals:
                        a, b = T.toks[j].pos - T.toks[s].pos, T.toks[j].end - T.toks[s].pos
                        code = code[:a] + prefix + field_locals[kk[2:]].name + code[b:]
                if fld is not None and code != prefix + fld:
                    raise GenError(f"{ec.local}: argument {k + 1} is `{code}`, the table names field {fld}")
                args.append("".join(inline) + " ".join(code.split()))
            else:
                if fld is None:
                    raise GenError(f"{ec.local}: argument {k + 1} reads {sorted(locals_ | members)}, "
                                   "which no specification field holds")
                args.append("".join(inline) + prefix + fld)
                need.append((fld, s, e, line))
        bra = "(" if d.init_kind == "(" else "{ "
        ket = ")" if d.init_kind == "(" else " }"
        inits[ec.member] = (ec.member, bra, args, ket)
        assigns[ec.local] = need
    return inits, assigns


def fmt_init(item, col):
    if isinstance(item, str):
        return item
    member, bra, args, ket = item
    one = member + bra + ", ".join(args) + ket
    if col + len(one) <= WIDTH:
        return one
    from nom_ep1.textops import wrap_args
    inner_col = col + len(member) + len(bra)
    return member + bra + wrap_args(args, inner_col, WIDTH, close=ket)


def run_network(g):
    T, text, L, drv = g.T, g.text, g.L, g.drv
    E = g.edits[DRIVER_PATH]
    rn_lo, rn_hi = drv.rn.body[0] + 1, drv.rn.body[1]
    lam_s, lam_e = drv.lam_decl.stmt
    rec = {"frozen": [T.line(drv.rn.body[0]), T.line(drv.rn.body[1])], "edits": []}

    def add(edit, line, cls):
        E.append(edit)
        rec["edits"].append({"class": cls, "tag": edit.tag, "frozen_line": line})

    for j in range(rn_lo, rn_hi):
        if lam_s <= j < lam_e:
            continue
        if T.k(j) == "id" and T.t(j) in ("run", "engine_spec") and T.t(j - 1) not in (".", "->", "::"):
            raise GenError(f"{DRIVER_PATH}:{T.line(j)}: runNetwork uses the name `{T.t(j)}`")

    # The lambda and its enum (TYPE_MOVE, relocation).
    enum_d = g.root_decl(LAMBDA_ENUM)
    a = leading_comment(text, T.toks[enum_d.stmt[0]].pos)
    b = L.end(T.toks[lam_e - 1].end)
    if enum_d.stmt[1] > lam_s:
        raise GenError("the LoopSignal enum does not precede the lambda")
    add(Edit(a, b, LAMBDA_NOTE, "T-A the lambda and LoopSignal"), T.line(enum_d.stmt[0]), "TYPE_MOVE")

    # The epoch source's calls.
    for c in calls_in(T, rn_lo, rn_hi):
        if c.name == LAMBDA_NAME and not (lam_s <= c.tok < lam_e):
            add(Edit(T.toks[c.tok].pos, T.toks[c.tok].end, "networkProcessEpoch_", "call rewrite"),
                c.line, "relocation")
            E.append(Edit(T.toks[c.open].end, T.toks[c.open].end, "run, ", "call rewrite"))

    # SPEC_CUT statements (with their comments, which move into networkEngineSpec).
    cut = cut_statements(g)
    cut_set = {(s, e) for s, e, _ in cut}
    g.cut = cut
    inits, assigns = member_inits(g)
    g.member_init = {m: fmt_init(v, 10 + len("")) for m, v in inits.items()}
    g.engine_inits = inits
    g.spec_assigns = assigns
    for s, e, n in cut:   # provisional: the reads below decide where each comment goes
        E.append(Edit(T.toks[s].pos, T.toks[e - 1].end, "", "provisional"))

    # The third-band closure (CALLABLE, while the base has it), the bracket closures and their
    # timer vectors.
    for name in [*([CALLABLE_LOCAL] if has_callable() else []), *BRACKETS, *TIMER_FIELDS]:
        d = g.root_decl(name)
        s, e = d.stmt
        a, b = g.stmt_line_span(s, e, with_comment=True)
        add(Edit(a, b, "", f"remove {name}"), T.line(s),
            "CALLABLE" if name == CALLABLE_LOCAL else ("BRACKET" if name in BRACKETS else "FIELD_MOVE"))

    # The run end's view of the engines.
    td = [d for d in drv.scopes.root.decls if d.name == "engines"]
    if len(td) != 1:
        raise GenError("the run end's `engines` view was not found")
    s, e = td[0].stmt
    a, b = g.stmt_line_span(s, e, with_comment=False)
    add(Edit(a, b, "", "run end: the engines view"), T.line(s), "OWNER_MOVE")
    ri = [j for j in range(rn_lo, rn_hi) if T.t(j) == "NetworkRunEndInputs" and T.t(j + 1) == "{"]
    if len(ri) != 1:
        raise GenError("the run end's NetworkRunEndInputs binding was not found")
    o = ri[0] + 1
    for j in range(o + 1, T.match[o]):
        if T.k(j) != "id" or T.t(j - 1) in (".", "->", "::"):
            continue
        n = T.t(j)
        rm = model.RUN_BY_KEY.get("D:" + n)
        new = TEARDOWN_GROUPS.get(n) or (rm.path if rm else None)
        if new is None:
            raise GenError(f"the run end binds {n}, which is not a run group")
        add(Edit(T.toks[j].pos, T.toks[j].end, new, "run end binding"), T.line(j), "OWNER_MOVE")

    # The run's construction, after the traces, and its header.
    tr = g.root_decl("traces")
    pos = L.end(T.toks[tr.stmt[1] - 1].end)
    add(Edit(pos, pos, RUN_BUILD, "the run's construction"), T.line(tr.stmt[0]), "new lines")
    anchor = '#include "app/Application.hpp"\n'
    k = text.find(anchor)
    if k < 0:
        raise GenError(f"{DRIVER_PATH} does not include app/Application.hpp")
    k += len(anchor)
    add(Edit(k, k, '#include "app/NetworkRun.hpp"     // NetworkRun, networkEngineSpec '
                   '(card NOM3 step EP1)\n', "include"), T.line(0), "include")

    # What the remaining text still reads decides each OWNER_MOVE and field-local line.
    owner_pending = []
    for rm in model.RUN_MEMBERS:
        if rm.cls != "OWNER_MOVE":
            continue
        d = g.root_decl(rm.name)
        if (d.stmt[0], d.stmt[1]) in cut_set:
            continue
        owner_pending.append((rm, d))
        E.append(Edit(T.toks[d.stmt[0]].pos, T.toks[d.stmt[1] - 1].end, "", "provisional"))
    reads = g.reads(E, rn_lo, rn_hi)
    E[:] = [x for x in E if x.tag != "provisional"]
    run_tok = tr.stmt[1]
    for rm, d in owner_pending:
        s, e = d.stmt
        n = rm.name
        if reads.get("D:" + n):
            if s < run_tok:
                raise GenError(f"OWNER_MOVE {n}: declared before the run's construction and still read")
            ty = rm.type + "&"
            add(Edit(T.toks[s].pos, T.toks[e - 1].end, f"{ty} {n} = {rm.path};", f"OWNER_MOVE {n}"),
                T.line(s), "OWNER_MOVE")
        else:
            a, b = g.stmt_line_span(s, e, with_comment=s < run_tok)
            add(Edit(a, b, "", f"OWNER_MOVE {n} (unread)"), T.line(s), "OWNER_MOVE")
    # The cut statements.  A statement's comment stays in runNetwork when the statement
    # declares a name runNetwork still reads (its reference line takes the statement's
    # place), and a run of consecutive cut statements keeps its first comment when the run
    # declares such a name or ends right before a run-owned engine's declaration (the
    # comment introduces that engine).  Every other comment moves with its statement into
    # networkEngineSpec.
    stmts = [se for se in drv.driver_statements() if se[0] < lam_s]
    pos_of = {se: k for k, se in enumerate(stmts)}
    runs = []
    for s, e, n in cut:
        if runs and pos_of[(s, e)] == pos_of[(runs[-1][-1][0], runs[-1][-1][1])] + 1:
            runs[-1].append((s, e, n))
        else:
            runs.append([(s, e, n)])
    owner_decls = {(d.stmt[0], d.stmt[1]) for rm, d in owner_pending}

    def still_read(s, e):
        return [d for d in (parse_declaration(T, s, e) or []) if reads.get("D:" + d.name)]

    kept = set()
    for run in runs:
        last = pos_of[(run[-1][0], run[-1][1])]
        before_owner = last + 1 < len(stmts) and stmts[last + 1] in owner_decls
        any_read = any(still_read(s, e) for s, e, _ in run)
        for k, (s, e, n) in enumerate(run):
            if still_read(s, e) or (k == 0 and (any_read or before_owner)):
                kept.add((s, e))
    g.cut_comment_kept = kept
    for s, e, n in cut:
        a, b = g.stmt_line_span(s, e, with_comment=(s, e) not in kept)
        add(Edit(a, b, "", f"SPEC_CUT {n}"), T.line(s), "SPEC_CUT")
        lines = []
        for d in still_read(s, e):
            rm = model.RUN_BY_KEY.get("D:" + d.name)
            f = model.SPEC_FIELD_BY_LOCAL.get(d.name)
            if rm is not None:
                lines.append(f"    {rm.type}& {d.name} = {rm.path};")
            elif f is not None:
                lines.append("    // Built by networkEngineSpec (app/NetworkRun.hpp, card NOM3 step EP1).")
                lines.append(f"    const {f.type}& {d.name} = engine_spec.{f.name};")
            else:
                raise GenError(f"SPEC_CUT moves {d.name} ({DRIVER_PATH}:{d.line}), which runNetwork "
                               "still reads")
        if lines:
            at = L.start(T.toks[s].pos) if (s, e) in kept else a
            add(Edit(at, at, "\n".join(lines) + "\n", "SPEC_CUT reference"), T.line(s), "SPEC_CUT")
    # Removed names no remaining text may read.
    for name in [CALLABLE_LOCAL, *BRACKETS, *TIMER_FIELDS, "engines"]:
        if reads.get("D:" + name):
            raise GenError(f"runNetwork still reads {name} after EP1 removes it")
    rec["unread_aliases"] = unread_aliases(g, E, rn_lo, rn_hi, add)
    g.new[RUN_CPP] = run_cpp(g)
    return rec


def unread_aliases(g, E, rn_lo, rn_hi, add):
    """UNREAD_ALIAS (declared.py): each listed runNetwork alias that nothing reads once the
    other edits are made loses its line (its trailing comment with it; a comment block above
    it is the docs table's, docs.py).  Then no declaration of runNetwork's outermost scope may
    stay unread: one that does is refused by name, so the list is closed at every base (the
    dead-alias defect INC-12 and EP0 removed, which EP1's move of the lambda would otherwise
    bring back).  Returns the names removed and the candidates kept (still read)."""
    T = g.T
    reads = g.reads(E, rn_lo, rn_hi)
    removed = removed_mask(T, E)
    out = {"removed": [], "kept": []}
    for d in declared.declared("UNREAD_ALIAS"):
        rd = g.root_decl(d.name)
        if removed[rd.tok]:
            raise GenError(f"UNREAD_ALIAS {d.name}: another edit already removes its line")
        if reads.get("D:" + d.name):
            out["kept"].append(d.name)
            continue
        s, e = rd.stmt
        ds = parse_declaration(T, s, e) or []
        if len(ds) != 1 or ds[0].init is None:
            raise GenError(f"UNREAD_ALIAS {d.name} ({DRIVER_PATH}:{rd.line}) is not a single declarator "
                           "with an initializer")
        a, b = g.stmt_line_span(s, e, with_comment=False)
        add(Edit(a, b, "", f"UNREAD_ALIAS {d.name}"), T.line(s), "UNREAD_ALIAS")
        out["removed"].append(d.name)
    reads = g.reads(E, rn_lo, rn_hi)
    removed = removed_mask(T, E)
    dead = [rd for rd in g.drv.scopes.root.decls
            if rd.kind != "param" and not removed[rd.tok] and not reads.get("D:" + rd.name)]
    if dead:
        raise GenError("runNetwork would keep declarations nothing reads after the cut: "
                       + ", ".join(f"{rd.name} ({DRIVER_PATH}:{rd.line})" for rd in dead)
                       + "; declare each UNREAD_ALIAS in declared.py (a side-effect-free alias), or "
                       "find the reader the generator missed")
    return out


def _is_engine_ctor(g, s, e):
    ds = parse_declaration(g.T, s, e) or []
    return any(d.name in model.ENGINE_BY_LOCAL for d in ds)


# ---------------------------------------------------------------------------------------------
# NetworkRun.cpp
# ---------------------------------------------------------------------------------------------
def spec_body(g):
    """The statements of networkEngineSpec and the preamble they need."""
    T, text, L = g.T, g.text, g.L
    field_locals = model.SPEC_FIELD_BY_LOCAL
    out = []
    reads = set()
    initialized_fields = set()
    for s, e, n in g.cut:
        ds = parse_declaration(T, s, e) or []
        engine = [d for d in ds if d.name in model.ENGINE_BY_LOCAL]
        ca = leading_comment(text, T.toks[s].pos)
        ls = L.start(T.toks[s].pos)
        le = L.end(T.toks[e - 1].end)
        if engine:
            d = engine[0]
            ec = model.ENGINE_BY_LOCAL[d.name]
            need = g.spec_assigns.get(d.name, [])
            if need:
                owner = "NetworkRunEngines" if ec.owner == "engines" else "NetworkRun"
                out.append(f"    // {d.name}'s constructor arguments ({owner}::{ec.member} is built "
                           "from these fields).\n")
            for fld, a0, b0, cm in need:
                for c in cm:
                    out.append(f"    // {c}\n")
                code = text[T.toks[a0].pos:T.toks[b0 - 1].end]
                out.append(f"    spec.{fld} = {code};\n")
                for j in range(a0, b0):
                    k = g.key_at(j)
                    if k:
                        reads.add(k)
            continue
        comment = "" if (s, e) in g.cut_comment_kept else text[ca:ls]
        body = text[ls:le]
        fl = [d for d in ds if d.name in field_locals]
        if fl:
            if len(ds) != 1:
                raise GenError(f"SPEC_CUT: {fl[0].name} is one of several declarators")
            d = fl[0]
            f = field_locals[d.name]
            ts = T.toks[d.type_span[0]].pos
            if d.init is None:
                new = text[ls:ts] + f"{T.src(*d.type_span).strip()}& {d.name} = spec.{f.name};" \
                    + text[T.toks[e - 1].end:le]
            else:
                new = text[ls:ts] + f"spec.{f.name}" + text[T.toks[d.tok].end:le]
                initialized_fields.add(d.name)
                for j in range(*d.init):
                    k = g.key_at(j)
                    if k:
                        reads.add(k)
            out.append(comment + new)
            continue
        out.append(comment + body)
        for j in range(s, e):
            k = g.key_at(j)
            if k:
                reads.add(k)
    # A field local initialized by assignment and read later needs its name back.
    later = {k[2:] for k in reads if k.startswith("D:")} & initialized_fields
    if later:
        raise GenError(f"SPEC_CUT: {sorted(later)} are read after their field assignment")
    cut_names = set()
    for s, e, n in g.cut:
        for d in parse_declaration(T, s, e) or []:
            cut_names.add(d.name)
    pre_names = {k[2:] for k in reads if k.startswith("D:")} - cut_names - {"ctx", "nc"}
    members = {k[2:] for k in reads if k.startswith("A:")} - {"config_"}
    if members:
        raise GenError(f"SPEC_CUT: the cut statements read Application members {sorted(members)}")
    for bad in ("log", "traces", "setup"):
        if bad in pre_names:
            raise GenError(f"SPEC_CUT: the cut statements read {bad} (check V12)")
    pre = []
    queue, seen = list(pre_names), set()
    while queue:
        n = queue.pop()
        if n in seen:
            continue
        seen.add(n)
        d = g.root_decl(n)
        if not (d.is_ref or d.is_const) or d.init is None:
            raise GenError(f"networkEngineSpec would repeat {n}, which is not a const or reference "
                           "declaration with an initializer")
        s, e = d.stmt
        pre.append((d.tok, T.src(s, e)))
        for j in range(*d.init):
            k = g.key_at(j)
            if k and k.startswith("D:") and k[2:] not in ("ctx", "nc"):
                if k[2:] in ("log", "traces", "setup"):
                    raise GenError(f"networkEngineSpec's preamble line {n} reads {k[2:]}")
                queue.append(k[2:])
            if k and k.startswith("A:") and k != "A:config_":
                raise GenError(f"networkEngineSpec's preamble line {n} reads {k}")
    pre.sort()
    return [p for _, p in pre], "".join(out)


def bracket_fn(g, name):
    """networkSubBegin / networkSubEnd: the closure body under its preamble."""
    T, text, L = g.T, g.text, g.L
    d = g.root_decl(name)
    lam = d.lambda_scope
    cc = T.match[lam.start]
    po, pc = cc + 1, T.close(cc + 1)
    bo, bc = T.match[lam.end], lam.end
    params = collapse_ws(T.src(po + 1, pc))
    needed = set()
    for j in range(bo + 1, bc):
        k = g.key_at(j)
        if k and k.startswith("D:"):
            needed.add(k[2:])
    lines = {}
    queue = list(needed)
    while queue:
        n = queue.pop()
        if n in lines:
            continue
        dd = g.root_decl(n)
        rm = model.RUN_BY_KEY.get("D:" + n)
        if rm is not None:
            ty = rm.type if rm.type.endswith("&") else rm.type + "&"
            lines[n] = (dd.tok, f"{ty} {n} = {rm.path};")
        elif n in TIMER_FIELDS:
            lines[n] = (dd.tok, f"auto& {n} = timing.{n};")
            queue.append("timing")
        else:
            raise GenError(f"{name} reads {n}, which the run does not hold")
    body = text[L.end(T.toks[bo].end):L.start(T.toks[bc].pos)]
    body = dedent(body, 4)
    head = f"void {BRACKETS[name]}(NetworkRun& run, {params}) {{\n"
    pre = ["    // ---- From the run (card NOM3 step EP1; " + tpl.GEN + "): the names the",
           "    //      closure captured, re-pointed at the run. ----"]
    pre += ["    " + ln for _, ln in sorted(lines.values())]
    pre += ["    // ---- End of the bindings ----"]
    return head + "\n".join(pre) + "\n" + body + "}\n"


def run_cpp(g):
    T, text, L = g.T, g.text, g.L
    pre, body = spec_body(g)
    out = [tpl.NETWORK_RUN_CPP_HEAD]
    out.append("NetworkEngineSpec networkEngineSpec(const GnssConfig& config_, const NetworkConfig& nc,\n"
               "                                    const NetworkRunContext& ctx) {\n")
    out.append("    NetworkEngineSpec spec;\n")
    out.append("    // ---- The runNetwork declarations the moved statements read, character for character\n"
               "    //      (card NOM3 step EP1; " + tpl.GEN + "). ----\n")
    out += [f"    {p}\n" for p in pre]
    out.append("    // ---- End of the declarations ----\n\n")
    out.append(body)
    out.append("    return spec;\n}\n\n")
    # The D0b bracket comment block (it documented the removed closures and timer vectors).
    first = g.root_decl(TIMER_FIELDS[0])
    ca = leading_comment(text, T.toks[first.stmt[0]].pos)
    block = text[ca:L.start(T.toks[first.stmt[0]].pos)]
    out.append(dedent(block, 4))
    out.append(bracket_fn(g, "subBegin"))
    out.append("\n")
    out.append(bracket_fn(g, "subEnd"))
    out.append("\n} // namespace GnssProcessor\n")
    return "".join(out)


# ---------------------------------------------------------------------------------------------
# NetworkRunGroups.hpp, NetworkEpoch.hpp
# ---------------------------------------------------------------------------------------------
# The headers NetworkRunGroups.hpp needs once it owns the engines and holds their
# specification, each with the object that needs it: None, every base; a model.py root key,
# only while the base has that object (the REMOVE-key deletion takes the deweight gate, its
# configuration and the fast tier's wavelength provider, and so their headers).
GROUPS_INCLUDES = [
    ('#include "config/GnssConfigTypes.hpp"             // IonoConstraintDeweightConfig (card NOM3 step EP1)',
     "D:idw"),
    ('#include "common/PppUtils.hpp"                    // PppUtils, the fast tier\'s wavelength provider',
     "D:fc_ppp_utils"),
    ('#include "filter/NetworkCarryVce.hpp"             // NetworkCarryVce', None),
    ('#include "filter/NetworkEstimator.hpp"            // NetworkEstimator', None),
    ('#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver (+ ::Config)', None),
    ('#include "filter/NetworkIonoDeweightGate.hpp"     // NetworkIonoDeweightGate', "D:iono_deweight"),
    ('#include "filter/NetworkPriorResidual.hpp"        // NetworkPriorResidual', None),
    ('#include "slip/NetworkRawObservables.hpp"         // SlipQcChannel', None),
]
# The standard headers, likewise: std::unique_ptr (the fast tier's provider), std::set (the
# deweight gate's station list), std::string (the families), the static_asserts.
GROUPS_SYS_INCLUDES = [("memory", "D:fc_ppp_utils"), ("set", "D:iono_deweight"), ("string", None),
                       ("type_traits", None)]
BANNER_OLD = " *   - @ref NetworkRunEngines: a const view of the run's estimation engines (the run end);\n"
BANNER_NEW = (" *   - @ref NetworkRunEngines: the run's estimation engines, owned since card NOM3 step EP1\n"
              " *     (built in place inside NetworkRun from a @ref NetworkEngineSpec);\n")
HOLDS_OLD = (" * Application::runNetwork holds one of each group for the whole run and hands a seam the\n"
             " * group it reads instead of one parameter per member:\n")
HOLDS_NEW = (" * The run (app/NetworkRun.hpp, card NOM3 step EP1) owns one of each group for the whole\n"
             " * run, and a seam reads the group it needs instead of one parameter per member:\n")
LOOP_OLD = "just before the estimator is built and the warm resume runs"
LOOP_NEW = "just before the estimator is configured and the warm resume runs"
TIMING_FIELDS = (
    "    std::vector<SubBlockTimer> sec_sub_ep0 = std::vector<SubBlockTimer>(SB_COUNT);  "
    "///< D0b: the epoch-top copy of sub_blocks the readout differences against (card NOM3 step EP1).\n"
    "    std::vector<std::chrono::steady_clock::time_point> sub_wall0 =\n"
    "        std::vector<std::chrono::steady_clock::time_point>(SB_COUNT);              "
    "///< networkSubBegin's wall stamp per sub-block.\n"
    "    std::vector<NetworkNormalEquations::SectionTimers> sub_tmr0 =\n"
    "        std::vector<NetworkNormalEquations::SectionTimers>(SB_COUNT);              "
    "///< networkSubBegin's section-timer stamp per sub-block.\n")


def _replace_text(g, path, text, old, new, tag, required=True):
    k = text.find(old)
    if k < 0:
        if required:
            raise GenError(f"{path}: the text to edit was not found ({tag})")
        g.problems.append(f"{path}: the text to edit was not found ({tag}); left unchanged")
        return
    if text.find(old, k + 1) >= 0:
        raise GenError(f"{path}: the text to edit occurs twice ({tag})")
    g.edits[path].append(Edit(k, k + len(old), new, tag))


def groups_hpp(g):
    T = g.tree.tokens(GROUPS_HPP)
    if T is None:
        raise GenError(f"{GROUPS_HPP} does not exist at the base")
    text, L = T.source, Lines(T.source)
    _replace_text(g, GROUPS_HPP, text, BANNER_OLD, BANNER_NEW, "banner: the engines", required=False)
    _replace_text(g, GROUPS_HPP, text, HOLDS_OLD, HOLDS_NEW, "banner: the owner", required=False)
    _replace_text(g, GROUPS_HPP, text, LOOP_OLD, LOOP_NEW, "loop_begin comment")
    # Includes after the last quoted include.
    incs = [j for j in range(T.n) if T.k(j) == "pp" and T.t(j).startswith('#include "')]
    pos = L.end(T.toks[incs[-1]].end)
    have = set(re.findall(r'#include\s+"([^"]+)"', text))
    add = [x for x, key in GROUPS_INCLUDES if key not in model.ABSENT
           and re.search(r'"([^"]+)"', x).group(1) not in have]
    sys_incs = [j for j in range(T.n) if T.k(j) == "pp" and T.t(j).startswith("#include <")]
    have_sys = {T.t(j) for j in sys_incs}
    extra_sys = [f"#include <{h}>" for h, key in GROUPS_SYS_INCLUDES
                 if key not in model.ABSENT and f"#include <{h}>" not in have_sys]
    g.edits[GROUPS_HPP].append(Edit(pos, pos, "\n".join(add) + "\n", "includes"))
    if extra_sys:
        p2 = L.end(T.toks[sys_incs[-1]].end)
        g.edits[GROUPS_HPP].append(Edit(p2, p2, "\n".join(extra_sys) + "\n", "includes"))
    # NetworkRunEngines: the owner, and NetworkEngineSpec before it.
    js = [j for j in range(T.n - 2) if T.t(j) == "struct" and T.t(j + 1) == "NetworkRunEngines"
          and T.t(j + 2) == "{"]
    if len(js) != 1:
        raise GenError(f"{GROUPS_HPP}: struct NetworkRunEngines not found")
    j = js[0]
    close = T.match[j + 2]
    if T.t(close + 1) != ";":
        raise GenError(f"{GROUPS_HPP}: NetworkRunEngines does not end with `}};`")
    db = doc_before(text, T.toks[j].pos)
    a = db[0] if db else L.start(T.toks[j].pos)
    b = L.end(T.toks[close + 1].end)
    g.edits[GROUPS_HPP].append(Edit(a, b, engine_spec_struct() + "\n" + engines_struct(g), "OWNER_MOVE engines"))
    # NetworkRunTiming: the bracket scratch after sub_blocks.
    ks = [k for k in range(T.n - 1) if T.t(k) == "sub_blocks" and T.t(k + 1) == "="]
    if len(ks) != 1:
        raise GenError(f"{GROUPS_HPP}: NetworkRunTiming::sub_blocks not found")
    p3 = L.end(T.toks[ks[0]].end)
    g.edits[GROUPS_HPP].append(Edit(p3, p3, TIMING_FIELDS, "FIELD_MOVE timers"))
    return {"engines_struct_frozen": [T.line(j), T.line(close)]}


def engine_spec_struct():
    rows = []
    for f in model.SPEC_FIELDS:
        decl = f.name + (f"{{{f.init}}}" if f.init else "") + ";"
        rows.append((f.type, decl, f.doc))
    w1 = max(len(r[0]) for r in rows) + 1
    w2 = max(len(r[1]) for r in rows) + 1
    out = ["/// The constructor arguments of the run's engines and groups (card NOM3 step EP1), computed",
           "/// once by networkEngineSpec (app/NetworkRun.hpp) from the configuration and the knobs.  A",
           "/// field is either the setup local it replaced, with that local's name and type, or one",
           "/// engine constructor argument the setup computed inline.",
           "struct NetworkEngineSpec {"]
    for t, d, doc in rows:
        out.append("    " + t.ljust(w1) + d.ljust(w2) + "///< " + doc)
    out.append("};")
    return "\n".join(out) + "\n"


def engines_struct(g):
    T = g.T
    inits = []
    for ec in model.ENGINE_CTORS:
        if ec.owner != "engines":
            continue
        inits.append(fmt_init(g.engine_inits[ec.member], 10))
    out = ["/// The run's estimation engines, owned (card NOM3 step EP1; card NOM2 step TD1a made this name",
           "/// a const view, so every run-end report's `engines.X` reads unchanged).  Built in place inside",
           "/// NetworkRun from a NetworkEngineSpec; setup then configures the members through references",
           "/// (slip-QC parameters, corroborators, release tracking, estimator settings, warm resume).",
           "struct NetworkRunEngines {",
           "    explicit NetworkRunEngines(const NetworkEngineSpec& s)",
           "        : " + ",\n          ".join(inits) + " {}",
           "    NetworkRunEngines(const NetworkRunEngines&) = delete;",
           "    NetworkRunEngines& operator=(const NetworkRunEngines&) = delete;",
           ""]
    rows = []
    for ec in model.ENGINE_CTORS:
        if ec.owner != "engines":
            continue
        d = g.root_decl(ec.local)
        rows.append((T.src(*d.type_span).strip(), ec.member + ";", model.ENGINE_MEMBER_DOCS[ec.member]))
    w1 = max(len(r[0]) for r in rows) + 1
    w2 = max(len(r[1]) for r in rows) + 1
    for t, d, doc in rows:
        if t == "SlipQcChannel":
            out.append(f"    /// {doc}")
            out.append("    " + t.ljust(w1) + d)
        else:
            out.append("    " + t.ljust(w1) + d.ljust(w2) + "///< " + doc)
    out.append("};")
    out.append("static_assert(!std::is_copy_constructible_v<NetworkRunEngines> "
               "&& !std::is_move_constructible_v<NetworkRunEngines>);")
    return "\n".join(out) + "\n"


def check_epoch_template(g):
    """Every epoch member of model.EPOCH_MEMBERS appears in NetworkEpoch.hpp with its type and
    default initializer (the realization the verifier's V3c checks), in the text written at
    this base."""
    text = g.new[EPOCH_HPP]
    flat = re.sub(r"\s+", " ", text)
    for m in model.EPOCH_MEMBERS:
        if m.transform in ("CTOR", "ACCESSOR", "RESULT", "WS"):
            continue
        leaf = m.path.split(".")[-1]
        ty = re.sub(r"\s+", " ", m.type)
        want = f"{ty} {leaf}" + (f"{{{m.default}}}" if m.default else "") + ";"
        if want.replace(" ", "") not in flat.replace(" ", ""):
            raise GenError(f"NetworkEpoch.hpp: member {m.path} is not `{want}`")


# ---------------------------------------------------------------------------------------------
# The hand-off, the predicate, the knob comment, Application.hpp, CMake
# ---------------------------------------------------------------------------------------------
def solve_handoff(g):
    T = g.tree.tokens(SOLVE_HPP)
    text, L = T.source, Lines(T.source)
    js = [j for j in range(T.n - 2) if T.t(j) == "struct" and T.t(j + 1) == "NetworkSolveGlueResult"
          and T.t(j + 2) == "{"]
    if len(js) != 1:
        raise GenError(f"{SOLVE_HPP}: struct NetworkSolveGlueResult not found")
    j = js[0]
    db = doc_before(text, T.toks[j].pos)
    a = db[0] if db else L.start(T.toks[j].pos)
    g.edits[SOLVE_HPP].append(Edit(a, a, tpl.POSTFIT_HANDOFF, "FIELD_MOVE hand-off struct"))
    close = T.match[j + 2]
    fields = [old.split(".")[-1] for old, new in model.EPOCH_FIELD_MOVES]
    lines = []
    for f in fields:
        ks = [k for k in range(j, close) if T.t(k) == f and T.t(k + 1) in (";", "{")]
        if len(ks) != 1:
            raise GenError(f"{SOLVE_HPP}: field {f} not found in NetworkSolveGlueResult")
        lines.append((L.start(T.toks[ks[0]].pos), L.end(T.toks[ks[0]].end)))
    lines.sort()
    for k, (a, b) in enumerate(lines):
        g.edits[SOLVE_HPP].append(Edit(a, b, tpl.HANDOFF_MEMBER if k == 0 else "",
                                       "FIELD_MOVE hand-off fields"))
    # The solve's epilogue writes result.handoff.<field>.
    d = [x for x in declared.declared("FIELD_MOVE") if x.name in fields]
    n = 0
    for x in d:
        for loc in x.where:
            if type(loc).__name__ != "FieldWrites":
                continue
            Ts = g.tree.tokens(SOLVE_CPP)
            idx = g.tree.index(SOLVE_CPP)
            f = [ff for ff in idx.funcs if ff.name == "networkSolveGlue" and ff.is_def][0]
            for k in range(f.body[0], f.body[1] - 3):
                if Ts.t(k) == "result" and Ts.t(k + 1) == "." and Ts.t(k + 2) == x.name \
                        and Ts.t(k + 3) == "=" and Ts.t(k - 1) not in (".", "->"):
                    p = Ts.toks[k + 2].pos
                    g.edits[SOLVE_CPP].append(Edit(p, p, "handoff.", "FIELD_MOVE epilogue"))
                    n += 1
    if n != len(fields):
        raise GenError(f"{SOLVE_CPP}: {n} hand-off writes found, expected {len(fields)}")
    return {"fields": fields, "epilogue_writes": n}


def predicate(g):
    """networkIsUncarriedThirdBand beside the DD build (CALLABLE)."""
    T, text, L = g.T, g.text, g.L
    d = g.callable_decl()
    lam = d.lambda_scope
    params, ret, names = g.callable_signature()
    caps = g.callable_captures()
    bo, bc = T.match[lam.end], lam.end
    body = dedent(text[L.end(T.toks[bo].end):L.start(T.toks[bc].pos)], 4)
    s = d.stmt[0]
    ca = leading_comment(text, T.toks[s].pos)
    moved = dedent(text[ca:L.start(T.toks[s].pos)], 4)
    pre = comment_block("---- The closure's init-captures as const locals (card NOM3 step EP1; "
                        + tpl.GEN + "; the closure was runNetwork's isUncarriedThirdBand, whose "
                        "non-mutable captures were const). ----", "    ")
    for name, knob, line in caps:
        cm = _capture_comment(g, name)
        pre.append(f"    const auto {name} = {knob};" + (f"   // {cm}" if cm else ""))
    pre.append("    // ---- End of the bindings ----")
    rett = ret.replace("->", "").strip() or "bool"
    fn = (moved + f"{rett} {PREDICATE}(const NetworkKnobs& knobs, {params}) {{\n"
          + "\n".join(pre) + "\n" + body + "}\n\n")
    Tc = g.tree.tokens(DDB_CPP)
    close = namespace_close(Tc)
    pos = Lines(Tc.source).start(Tc.toks[close].pos)
    g.edits[DDB_CPP].append(Edit(pos, pos, fn, "CALLABLE definition"))
    Th = g.tree.tokens(DDB_HPP)
    close = namespace_close(Th)
    pos = Lines(Th.source).start(Th.toks[close].pos)
    doc = ["/**",
           " * @brief Whether (constellation @p cons, signal @p signal) is the EWL pair's uncarried band,",
           " *        left out of the DD carry while the EWL lane is armed (card M5 WL-2; UWL-1 reads the",
           " *        per-constellation lane-tier table).",
           " *",
           " * Formerly runNetwork's closure isUncarriedThirdBand, moved beside its one consumer (card",
           " * NOM3 step EP1, " + tpl.GEN + "); the body is unchanged.  Its three captures are knobs.",
           " * @param knobs  NetworkKnobs::uwl_on, uwl_gal_top and uwl_dual_on.",
           " * @param cons   The constellation letter.",
           " * @param signal A carried signal (\"L5Q\") or a band probe (\"L<digit>\").",
           " * @return True for the uncarried band.",
           " */"]
    decl = ("struct NetworkKnobs;   // app/NetworkRunContext.hpp\n\n" + "\n".join(doc) + "\n"
            + f"{rett} {PREDICATE}(const NetworkKnobs& knobs, {params});\n\n")
    g.edits[DDB_HPP].append(Edit(pos, pos, decl, "CALLABLE declaration"))
    return {"captures": [c[:2] for c in caps], "frozen_line": T.line(s)}


def _capture_comment(g, name):
    """The line comment that follows init-capture @p name on its source line, if any."""
    T, text, L = g.T, g.text, g.L
    d = g.callable_decl()
    for c in d.lambda_scope.decls:
        if c.kind == "capture" and c.name == name:
            end = T.toks[c.init[1] - 1].end
            rest = text[end:L.end(end)]
            k = rest.find("//")
            return rest[k + 2:].strip() if k >= 0 else ""
    return ""


def runctx_comment(g):
    T = g.tree.tokens(RUNCTX_HPP)
    old = "(isUncarriedThirdBand, which networkBuildCarryDdPriors calls)"
    new = f"({PREDICATE} in filter/NetworkDdPriorBuild, which networkBuildCarryDdPriors calls)"
    _replace_text(g, RUNCTX_HPP, T.source, old, new, "knob comment", required=False)
    return {"done": any(e.tag == "knob comment" for e in g.edits[RUNCTX_HPP])}


def application_hpp(g):
    T = g.tree.tokens(APPLICATION_HPP)
    text, L = T.source, Lines(T.source)
    m = re.search(r"^enum class EpochAction;[^\n]*\n", text, re.M)
    if not m:
        raise GenError(f"{APPLICATION_HPP}: the EpochAction forward declaration was not found")
    fwd = "".join(f"{d}".ljust(29) + f"// {c}\n" for d, c in tpl.APP_FWD)
    g.edits[APPLICATION_HPP].append(Edit(m.end(), m.end(), fwd, "forward declarations"))
    idx = g.tree.index(APPLICATION_HPP)
    rn = [f for f in idx.funcs if f.name == "runNetwork" and (f.cls or "") == "Application"]
    if len(rn) != 1:
        raise GenError(f"{APPLICATION_HPP}: Application::runNetwork's declaration was not found")
    j = rn[0].paren[1] + 1
    while T.t(j) != ";":
        j += 1
    pos = L.end(T.toks[j].end)
    g.edits[APPLICATION_HPP].append(Edit(pos, pos, tpl.at_base("APP_PROCESS_EPOCH", model.ABSENT),
                                         "networkProcessEpoch_"))
    return {"after_line": T.line(j)}


def cmake(g):
    data = g.tree.read(CMAKE)
    if data is None:
        raise GenError("CMakeLists.txt does not exist at the base")
    srcs = ["src/core/app/ApplicationNetworkEpoch.cpp", RUN_CPP]
    tests = [CONTRACT_TEST]
    if ENGINE_SPEC_TEST in g.target_files:
        tests.append(ENGINE_SPEC_TEST)
    m = re.search(r"^([ \t]*)src/core/app/ApplicationNetwork\.cpp[ \t]*\n", data, re.M)
    if not m:
        raise GenError("CMakeLists.txt: src/core/app/ApplicationNetwork.cpp is not listed")
    ind = m.group(1)
    g.edits[CMAKE].append(Edit(m.end(), m.end(), "".join(f"{ind}{s}\n" for s in srcs), "sources"))
    m2 = re.search(r"^([ \t]*)tests/test_network_knobs_epoch_driver\.cpp[ \t]*\n", data, re.M)
    if not m2:
        ms = list(re.finditer(r"^([ \t]*)tests/test_[\w]+\.cpp[ \t]*\n", data, re.M))
        if not ms:
            raise GenError("CMakeLists.txt: no tests/test_*.cpp line")
        m2 = ms[-1]
    ind = m2.group(1)
    g.edits[CMAKE].append(Edit(m2.end(), m2.end(), "".join(f"{ind}{t}\n" for t in tests), "tests"))
    return {"sources": srcs, "tests": tests}
