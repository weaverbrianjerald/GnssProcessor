"""Application::networkProcessEpoch_ from the frozen lambda (card NOM3 step EP1, T-A to T-F).

epoch_function(g) returns the text of src/core/app/ApplicationNetworkEpoch.cpp and the
manifest record of the move.  The lambda body is taken whole from the frozen driver; every
change is one edit of a transform of specification 5.3 (T-B to T-F), applied once; the edited
body is then moved four columns left (T-A) under a preamble that binds the driver names the
body still reads.  Whether an epoch local keeps an alias, and which driver names the preamble
binds, is decided by counting the identifiers the edited text still reads (each resolved by the
scope walk of scripts/nom_cpp.py to its frozen declaration), never by name alone.
"""
from nom_cpp import DRIVER_PATH, calls_in, lex
from nom_ep1 import model
from nom_ep1 import templates as tpl
from nom_ep1.gen import (GenError, SOLVE, SHIFT, WIDTH, BRACKETS, CALLABLE_LOCAL, TIMER_FIELDS,
                         LAMBDA_ENUM, comment_block)
from nom_ep1.textops import Edit, lit, src, apply_edits, leading_comment, wrap_args, dedent

EMIT_SEAM = "networkEmitStore"
INPUT_STAGE_LAST = "networkCanonicalPairAdmission"


def _tok_seq(T, a, b, seq):
    """Start indices in [a, b) where the token texts of @p seq occur."""
    n = len(seq)
    return [j for j in range(a, b - n + 1) if all(T.t(j + q) == seq[q] for q in range(n))]


def epoch_function(g):
    T, text, L, drv = g.T, g.text, g.L, g.drv
    lo, hi = g.lambda_body_range()
    lo_tok, hi_tok = drv.lam_open + 1, drv.lam_close
    rec = {"frozen_lambda": [T.line(drv.lam_open), T.line(drv.lam_close)], "transforms": [],
           "calls": []}
    for j in range(lo_tok, hi_tok):
        if T.k(j) == "id" and T.t(j) in ("run", "ep", "nrun", "nep") \
                and T.t(j - 1) not in (".", "->", "::"):
            raise GenError(f"{DRIVER_PATH}:{T.line(j)}: the lambda uses the name `{T.t(j)}`, "
                           "which the epoch function takes for the run or the epoch")
    E = []
    conv_spans = []

    def add(edit, frozen_line, note=""):
        E.append(edit)
        rec["transforms"].append({"tag": edit.tag, "frozen_line": frozen_line, "note": note})

    # --- T-F: the builder closures, when the solve is converted -------------------------------
    # (model.SOLVE_BUILDERS, the ones the base has: two at EP0, the DD build's alone once the
    # REMOVE-key deletion has taken the ensemble gauges)
    closures = {}
    if SOLVE in g.conv:
        for name, _ in model.SOLVE_BUILDERS:
            d = g.lam_decl(name)
            if d.kind != "closure" or d.lambda_scope is None:
                raise GenError(f"{name} is not a closure in the frozen lambda")
            inner = [c for c in drv.calls if d.lambda_scope.start <= c.tok <= d.lambda_scope.end
                     and c.name in g.conv]
            if len(inner) != 1:
                raise GenError(f"{name}: expected one converted call inside the closure, found {len(inner)}")
            closures[name] = (d, inner[0])

    def in_closure(tok):
        return any(d.lambda_scope.start <= tok <= d.lambda_scope.end for d, _ in closures.values())

    # A RESULT statement's `T x` becomes `ep.<m>`: the columns after it on that line move left.
    result_delta = {}
    for m in model.EPOCH_MEMBERS:
        if m.transform == "RESULT":
            d = g.lam_decl(m.local)
            delta = (T.toks[d.tok].end - T.toks[d.type_span[0]].pos) - len(m.path)
            result_delta[m.local] = (d, delta)

    def delta_of(c):
        for d, delta in result_delta.values():
            if d.init and d.init[0] <= c.tok < d.init[1]:
                return delta
        return 0

    # --- T-C: converted call sites ---------------------------------------------------------------
    for name, pl in g.plans.items():
        for c in pl.rr.sites:
            if in_closure(c.tok):
                continue
            args = g.call_args(pl, c)
            col = g.col_after(T, c.open) - delta_of(c)
            new = wrap_args(args, col, WIDTH + SHIFT, close="")
            a, b = T.toks[c.open].end, T.toks[c.close].pos
            add(Edit(a, b, new, f"T-C {name}"), c.line)
            conv_spans.append((a, b))
            rec["calls"].append({"seam": name, "frozen_line": c.line, "args_before": c.nargs,
                                 "args_after": len(args), "call": f"{name}({', '.join(args)})"})
            # A comment after `);` on the call's last line annotates its last argument when
            # that argument has a line of its own (spec 3.2: the snapshot call's four); it
            # moves with the arguments.  On a one-line call it annotates the call and stays.
            if T.t(c.close + 1) == ";" and c.args and T.line(c.args[-1][0]) > T.line(c.open):
                semi_end = T.toks[c.close + 1].end
                eol = L.end(semi_end) - 1
                rest = text[semi_end:eol]
                if rest.strip().startswith("//") and name in g.plans:
                    pl.comments[len(c.args) - 1].append(rest.strip()[2:].strip())
                    add(Edit(semi_end, eol, "", f"T-C comment {name}"), c.line,
                        "a trailing comment moved into the seam's preamble")

    # --- T-E: the D0b brackets ---------------------------------------------------------------------
    n_brackets = 0
    for c in calls_in(T, lo_tok, hi_tok):
        if c.name in BRACKETS:
            add(Edit(T.toks[c.tok].pos, T.toks[c.tok].end, BRACKETS[c.name], "T-E"), c.line)
            E.append(Edit(T.toks[c.open].end, T.toks[c.open].end, "run, ", "T-E"))
            n_brackets += 1
    rec["brackets"] = n_brackets

    # --- T-F: closures out, invocations become the builders' converted calls ------------------
    for name, (d, inner) in closures.items():
        s, e = d.stmt
        a, b = g.stmt_line_span(s, e, with_comment=True)
        stmt_line = L.start(T.toks[s].pos)
        add(Edit(a, b, "", f"T-F closure {name}"), T.line(s))
        pl = g.plans[inner.name]
        args = g.call_args(pl, inner)
        invs = [c for c in calls_in(T, lo_tok, hi_tok) if c.name == name and not in_closure(c.tok)]
        if not invs:
            raise GenError(f"T-F: the closure {name} is never invoked")
        for c in invs:
            add(Edit(T.toks[c.tok].pos, T.toks[c.close].end, f"{inner.name}({', '.join(args)})",
                     f"T-F call {name}"), c.line)
            rec["calls"].append({"seam": inner.name, "frozen_line": c.line, "via": name,
                                 "call": f"{inner.name}({', '.join(args)})"})
            st = g.top_stmt(c.tok)
            il = L.start(T.toks[st[0]].pos)
            if a < stmt_line:
                E.append(Edit(il, il, [src(a, stmt_line)], f"T-F comment {name}"))

    # --- VALUE_EQUAL (T-B9): the counter comparisons (four at EP0; three once the REMOVE-key
    # deletion has taken the fast clock's harvest gate) ---------------------------------------
    # (model.VALUE_EQUAL_EXPR, spelled with the base's name of the counter snapshot)
    seq = [t.text for t in lex(model.VALUE_EQUAL_EXPR)]
    sites = _tok_seq(T, lo_tok, hi_tok, seq)
    for j in sites:
        a, b = T.toks[j].pos, T.toks[j + len(seq) - 1].end
        if any(x <= a < y for x, y in conv_spans):
            continue
        add(Edit(a, b, "ep.emitted", "T-B9 EMITTED"), T.line(j))
    rec["value_equal_sites"] = [T.line(j) for j in sites]
    emit_sites = [c for c in drv.calls if c.name == EMIT_SEAM]
    if len(emit_sites) != 1:
        raise GenError(f"T-B9: expected one {EMIT_SEAM} call, found {len(emit_sites)}")
    stmts = drv.lambda_statements()
    st = g.top_stmt(emit_sites[0].tok)
    k = stmts.index(st)
    nxt = stmts[k + 1]
    if not (T.t(nxt[0]) == "subEnd" and T.t(nxt[0] + 2) == "SB_EMITSTORE"):
        raise GenError(f"T-B9: the statement after {EMIT_SEAM} is not subEnd(SB_EMITSTORE)")
    pos = L.end(T.toks[nxt[1] - 1].end)
    ind = L.indent(T.toks[nxt[0]].pos)
    add(Edit(pos, pos, [lit(f"{ind}ep.emitted = {model.VALUE_EQUAL_EXPR};\n",
                            reads=("D:ctx", "L:" + model.VALUE_EQUAL_LOCAL))], "T-B9 EMITTED store"),
        T.line(nxt[0]))

    # --- T-B: the epoch's locals (pass 1: provisional removals, to count the reads) ------------
    members = [m for m in model.EPOCH_MEMBERS if m.transform != "CTOR"]
    decl = {m.local: g.lam_decl(m.local) for m in members}
    result_members = [m for m in members if m.transform == "RESULT"]
    member_alias = {}   # alias name -> (result member, statement, field)
    for st2 in stmts:
        s, e = st2
        ds = [d for d in drv.lam_scope.decls if d.stmt == st2 and d.kind == "var"]
        if len(ds) != 1 or not ds[0].is_ref or ds[0].init is None:
            continue
        d = ds[0]
        a0, b0 = d.init
        if b0 - a0 == 3 and T.t(a0 + 1) == "." and T.k(a0 + 2) == "id":
            for m in result_members:
                if T.t(a0) == m.local:
                    member_alias[d.name] = (m, st2, T.t(a0 + 2), d)
    provisional = []
    for m in members:
        d = decl[m.local]
        s, e = d.stmt
        if m.transform in ("WS", "ALIAS", "SCALAR", "ACCESSOR"):
            provisional.append(Edit(T.toks[s].pos, T.toks[e - 1].end, "", "provisional"))
        elif m.transform == "SIZED":
            provisional.append(Edit(T.toks[s].pos, T.toks[e - 1].end,
                                    [src(T.toks[d.init[0]].pos, T.toks[d.init[1] - 1].end)], "provisional"))
    for name, (m, st2, field, d) in member_alias.items():
        provisional.append(Edit(T.toks[st2[0]].pos, T.toks[st2[1] - 1].end, "", "provisional"))
    # Multi-declarator statements appear once.
    seen, prov = set(), []
    for e in provisional:
        if (e.a, e.b) not in seen:
            seen.add((e.a, e.b))
            prov.append(e)
    reads1 = g.reads(E + prov, lo_tok, hi_tok)
    solve_site = drv.calls[g.solve_idx]
    solve_stmt_end = T.toks[g.top_stmt(solve_site.tok)[1] - 1].end

    def read_after_solve_only(key):
        js = reads1.get(key, [])
        return bool(js) and all(j >= 0 and T.toks[j].pos > solve_stmt_end for j in js)

    def alias_text(m, d, key, const_ok=True):
        ty = T.src(*d.type_span).strip()
        if m.transform in ("ALIAS", "SIZED", "SCALAR", "WS"):
            const = "const " if (const_ok and read_after_solve_only(key)) else ""
        else:
            const = ""
        return f"{const}{ty}& {d.name} = {m.path};"

    rec["epoch_members"] = []

    def drop_comment_ok(s, e):
        """The comment above a removed declaration goes with it when nothing it could also
        describe follows directly (a blank line or another comment comes next)."""
        nxt_line_start = L.end(T.toks[e - 1].end)
        nxt = text[nxt_line_start:L.end(nxt_line_start)].strip()
        return nxt == "" or nxt.startswith("//")

    def remove_stmt(s, e, tag, line, force_comment=None):
        with_c = drop_comment_ok(s, e) if force_comment is None else force_comment
        a, b = g.stmt_line_span(s, e, with_comment=with_c)
        add(Edit(a, b, "", tag), line)

    done_stmts = set()
    ctor_extra = []
    for m in members:
        d = decl[m.local]
        s, e = d.stmt
        key = "L:" + m.local
        read = bool(reads1.get(key))
        ind = L.indent(T.toks[s].pos)
        entry = {"local": m.local, "path": m.path, "transform": m.transform,
                 "frozen_line": T.line(s), "alias": False}
        if m.transform == "WS":
            remove_stmt(s, e, "T-B2 WS", T.line(s), force_comment=True)
            if read:
                ctor_extra.append(f"{T.src(*d.type_span).strip()}& {d.name} = {m.path};")
                entry["alias"] = True
        elif m.transform == "CLOCK":
            pass
        elif m.transform == "ALIAS":
            if read:
                add(Edit(T.toks[s].pos, T.toks[e - 1].end, alias_text(m, d, key), "T-B3 ALIAS"), T.line(s))
                entry["alias"] = True
            else:
                remove_stmt(s, e, "T-B3 ALIAS (unread)", T.line(s))
        elif m.transform == "SIZED":
            ty = T.src(*d.type_span).strip()
            pieces = [lit(f"{m.path} = {ty}("),
                      src(T.toks[d.init[0]].pos, T.toks[d.init[1] - 1].end), lit(");")]
            if read:
                pieces.append(lit("\n" + ind + alias_text(m, d, key)))
                entry["alias"] = True
            add(Edit(T.toks[s].pos, T.toks[e - 1].end, pieces, "T-B4 SIZED"), T.line(s))
        elif m.transform == "SCALAR":
            if (s, e) in done_stmts:
                continue
            done_stmts.add((s, e))
            group = [mm for mm in members if mm.transform == "SCALAR" and decl[mm.local].stmt == (s, e)]
            for mm in group:
                dd = decl[mm.local]
                init = T.norm(*dd.init) if dd.init else ""
                if init != mm.default:
                    raise GenError(f"T-B5: {mm.local} is initialized to {init}, the member default is {mm.default}")
            kept = [mm for mm in group if reads1.get("L:" + mm.local)]
            if kept:
                txt = ("\n" + ind).join(alias_text(mm, decl[mm.local], "L:" + mm.local) for mm in kept)
                add(Edit(T.toks[s].pos, T.toks[e - 1].end, txt, "T-B5 SCALAR"), T.line(s))
            else:
                remove_stmt(s, e, "T-B5 SCALAR (unread)", T.line(s))
            for mm in group:
                rec["epoch_members"].append({"local": mm.local, "path": mm.path, "transform": "SCALAR",
                                             "frozen_line": T.line(s), "alias": mm in kept})
            continue
        elif m.transform == "RESULT":
            ts = d.type_span[0]
            add(Edit(T.toks[ts].pos, T.toks[d.tok].end, m.path, "T-B7 RESULT"), T.line(s))
            # An unconverted call's continuation lines follow its parenthesis left.
            calls = [c for c in calls_in(T, d.init[0], d.init[1]) if c.tok == d.init[0]]
            delta = result_delta[m.local][1]
            if calls and calls[0].name not in g.plans and delta > 0:
                c = calls[0]
                for ln in range(T.line(c.open) + 1, T.line(c.close) + 1):
                    ls = _line_start_of(text, ln)
                    if text[ls:ls + delta] == " " * delta and text[ls + delta] == " ":
                        add(Edit(ls, ls + delta, "", "T-B7 RESULT alignment"), ln)
            kept_aliases = []
            for an, (mm, st2, field, ad) in member_alias.items():
                if mm is not m:
                    continue
                if reads1.get("L:" + an):
                    kept_aliases.append(an)
                    moved = [new for old, new in model.EPOCH_FIELD_MOVES
                             if old == f"{m.path}.{field}"]
                    if moved:
                        j = ad.init[0] + 2
                        add(Edit(T.toks[j].pos, T.toks[j].pos, "handoff.", "FIELD_MOVE"), T.line(st2[0]))
                else:
                    remove_stmt(st2[0], st2[1], "T-B7 member alias (unread)", T.line(st2[0]),
                                force_comment=False)
            rest = [j for j in reads1.get(key, []) if j >= 0]
            if kept_aliases or any(T.t(j + 1) != "." for j in rest):
                const = "const " if d.is_const else ""
                ty = T.src(*d.type_span).strip()
                if ty.startswith("const "):
                    ty = ty[len("const "):]
                pos = L.end(T.toks[e - 1].end)
                add(Edit(pos, pos, f"{ind}{const}{ty}& {d.name} = {m.path};\n", "T-B7 RESULT alias"),
                    T.line(s))
                entry["alias"] = True
            else:
                for j in rest:
                    add(Edit(T.toks[j].pos, T.toks[j].end, m.path, "T-B7 RESULT read"), T.line(j))
            entry["member_aliases_kept"] = kept_aliases
        elif m.transform == "ACCESSOR":
            if read:
                add(Edit(T.toks[s].pos, T.toks[e - 1].end,
                         f"const Eigen::VectorXd& {d.name} = {m.path};", "T-B8 ACCESSOR"), T.line(s))
                entry["alias"] = True
            else:
                remove_stmt(s, e, "T-B8 ACCESSOR (unread)", T.line(s), force_comment=False)
        rec["epoch_members"].append(entry)

    # CLOCK (T-B6): after the last of the three const locals.
    clock = [m for m in members if m.transform == "CLOCK"]
    last = max(clock, key=lambda m: decl[m.local].stmt[1])
    s, e = decl[last.local].stmt
    pos = L.end(T.toks[e - 1].end)
    ind = L.indent(T.toks[s].pos)
    names = [m.local for m in clock]
    add(Edit(pos, pos, [lit(f"{ind}ep.clock = EpochClock{{ {', '.join(names)} }};\n",
                            reads=tuple("L:" + n for n in names))], "T-B6 CLOCK"), T.line(s))

    # CTOR (T-B1): after the canonical-pair statement.
    se = g.lam_decl("se")
    cp = [c for c in drv.calls if c.name == INPUT_STAGE_LAST]
    if len(cp) != 1 or g.top_stmt(cp[0].tok) != se.stmt:
        raise GenError("T-B1: se is not the statement of the canonical-pair admission")
    s, e = se.stmt
    pos = L.end(T.toks[e - 1].end)
    ind = L.indent(T.toks[s].pos)
    ctor = [f"{ind}// The epoch: everything the stages below hand each other (app/NetworkEpoch.hpp).",
            f"{ind}NetworkEpoch ep(se);"] + [ind + x for x in ctor_extra]
    add(Edit(pos, pos, [lit("\n".join(ctor) + "\n", reads=("L:se",))], "T-B1 CTOR"), T.line(s))

    # --- The preamble: the driver names the edited text still reads ----------------------------
    reads2 = g.reads(E, lo_tok, hi_tok)
    needed = {k[2:] for k in reads2 if k.startswith("D:")}
    pre = build_preamble(g, needed)
    rec["preamble"] = [ln for ln, _ in pre]

    # --- Assemble ----------------------------------------------------------------------------------
    body = dedent(apply_edits(text, E, lo, hi), SHIFT)
    enum_d = g.root_decl(LAMBDA_ENUM)
    es = enum_d.stmt[0]
    ca = leading_comment(text, T.toks[es].pos)
    lead = dedent(text[ca:L.start(T.toks[es].pos)], SHIFT)
    pa, pb = _lambda_params(T, drv)
    lam_param = text[pa:pb]
    head = ("Application::LoopSignal Application::networkProcessEpoch_(NetworkRun& run,\n"
            + " " * len("Application::LoopSignal Application::networkProcessEpoch_(") + lam_param + ") {\n")
    pre_lines = comment_block("---- The run under the names the epoch's text still reads (card NOM3 "
                              "step EP1; " + tpl.GEN + "; each line the frozen runNetwork "
                              "declaration with the run's objects re-pointed).  Step EP2 removes "
                              "these lines with the blocks that read them. ----", "    ")
    for ln, _ in pre:
        pre_lines += ["    " + x for x in ln.split("\n")]
    pre_lines += ["    // ---- End of the run's names ----", ""]
    inc = include_block(g)
    out = (tpl.EPOCH_CPP_BANNER + "\n" + inc + "\nnamespace GnssProcessor {\n\n" + lead
           + "// Card NOM3 step EP1: the callable is Application::networkProcessEpoch_, over the run\n"
           + "// (app/NetworkRun.hpp) and the synced epoch.\n"
           + head + "\n".join(pre_lines) + "\n" + body + "}\n\n} // namespace GnssProcessor\n")
    first = out[:out.index(head)].count("\n") + 1
    rec["function_lines"] = [first, out.count("\n")]
    rec["preamble_count"] = len(pre)
    return out, rec, E


_LINE_STARTS = {}


def _line_start_of(text, ln):
    """Character index of the first character of 1-based line @p ln of @p text."""
    key = id(text)
    if key not in _LINE_STARTS:
        starts = [0]
        for k, ch in enumerate(text):
            if ch == "\n":
                starts.append(k + 1)
        _LINE_STARTS[key] = starts
    return _LINE_STARTS[key][ln - 1]


def _lambda_params(T, drv):
    """Token span of the frozen lambda's parameter list (inside the parentheses)."""
    cc = T.match[drv.lam_scope.start]
    po = cc + 1
    if T.t(po) != "(":
        raise GenError("the frozen lambda has no parameter list")
    return T.toks[po + 1].pos, T.toks[T.close(po) - 1].end


def include_block(g):
    """The frozen driver's #include lines, with the run's and the epoch's headers added after
    app/Application.hpp."""
    T, L = g.T, g.L
    pp = [j for j in range(T.n) if T.k(j) == "pp"]
    first = next(j for j in pp if T.t(j).startswith("#include"))
    ns = next(j for j in range(T.n) if T.t(j) == "namespace" and T.t(j + 1) == "GnssProcessor")
    block = g.text[L.start(T.toks[first].pos):L.start(T.toks[ns].pos)].rstrip("\n") + "\n"
    anchor = '#include "app/Application.hpp"\n'
    if anchor not in block:
        raise GenError("the driver does not include app/Application.hpp")
    return block.replace(anchor, anchor + '#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)\n'
                         '#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)\n', 1)


def build_preamble(g, needed):
    """[(line, key)] of the epoch function's preamble for the driver names @p needed, closed
    over the names their re-declarations read, in frozen declaration order."""
    T = g.T
    lines = {}
    queue = list(needed)
    seen = set()
    while queue:
        n = queue.pop()
        if n in seen:
            continue
        seen.add(n)
        d = g.root_decl(n)
        deps = set()
        if n in BRACKETS or n == LAMBDA_ENUM:
            raise GenError(f"the epoch function still reads {n}, which EP1 removes")
        if n == CALLABLE_LOCAL:
            params, ret, names = g.callable_signature()
            line = (f"auto {n} = [&ctx]({params}){ret} {{\n"
                    f"    return networkIsUncarriedThirdBand(ctx.knobs, {', '.join(names)});\n"
                    "};")
            deps.add("ctx")
        elif "D:" + n in model.RUN_BY_KEY:
            rm = model.RUN_BY_KEY["D:" + n]
            ty = rm.type if rm.type.endswith("&") else rm.type + "&"
            line = f"{ty} {n} = {rm.path};"
        elif n in TIMER_FIELDS:
            line = f"{T.src(*d.type_span).strip()}& {n} = timing.{n};"
            deps.add("timing")
        else:
            if not (d.is_ref or d.is_const):
                raise GenError(f"the epoch function reads {n}, a non-const by-value runNetwork local "
                               "(re-declaring it per epoch would lose its value)")
            ds = [x for x in g.drv.scopes.root.decls if x.stmt == d.stmt]
            if len(ds) != 1:
                raise GenError(f"{n} is one of several declarators of one statement")
            s, e = d.stmt
            line = " ".join(T.src(s, e).split()) if "\n" in T.src(s, e) else T.src(s, e)
            for j in range(*d.init):
                k = g.key_at(j)
                if k and k.startswith("D:"):
                    deps.add(k[2:])
        lines[n] = (d.tok, line)
        queue.extend(deps - seen)
    return [(line, "D:" + n) for n, (tok, line) in sorted(lines.items(), key=lambda kv: kv[1][0])]
