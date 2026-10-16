"""The seam conversion of card NOM3 step EP1 (T-D, specification 3.1 to 3.6, 5.9).

seams(g) adds to g.edits, per converted row:
  - the declaration: a DIRECT, MEMBER or RUN seam's declaration takes the epoch form in place,
    its Doxygen regenerated (check V7: the run state and the epoch members its preamble binds,
    split into read and written by the declarator, each former @param text under its binding);
    an ADAPTER keeps its explicit declaration byte-identical and gains the epoch form after it;
  - the definition: the new parameter list and the preamble, one line per frozen parameter (its
    frozen declarator, default removed, `= <binding>;`), the body below byte-identical; an
    ADAPTER's epoch form is a forwarding definition in a generated region at the end of its
    source;
  - the solve's replay (T-F): its two callback invocations become the builders' converted calls;
  - forward declarations of the new types in each seam header, the run's and the epoch's
    headers in each seam source.
contract_test(g) writes tests/test_network_epoch_contract.cpp from the same plans.
"""
import collections
import re

from nom_cpp import APPLICATION_HPP, calls_in
from nom_ep1 import model
from nom_ep1 import templates as tpl
from nom_ep1.gen import GenError, SOLVE, WIDTH, SOLVE_HPP
from nom_ep1.textops import (Edit, Lines, collapse_ws, doc_before, doc_lines, parse_doc,
                             render_doc, wrap_text, wrap_args)
from nom_ep1.model import ADAPTER, MEMBER, RUN

EPOCH_INCLUDES = ('#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)',
                  '#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)',
                  '#include "app/NetworkRunContext.hpp"',
                  '#include "app/NetworkSetupResult.hpp"',
                  '#include "rinex/MultiRinexSync.hpp"')
GROUP_WORDS = {"pass_a": "the layout", "funnel": "the slip funnel", "columns": "the shared columns",
               "assembly": "the station blocks", "dd_build": "the DD build's reverse maps",
               "handoff": "the post-fit hand-off"}


def group_words(o):
    """GROUP_WORDS[@p o] at the bound base: the DD build has one reverse map once the
    REMOVE-key deletion has taken the lane pair groups' second one."""
    w = GROUP_WORDS[o]
    if o == "dd_build" and "L:carry_arc_amb_key2" in model.ABSENT:
        w = "the DD build's reverse map"
    return w


def builders():
    """The solve's replay builders at the bound base (T-F): the closure names of
    model.SOLVE_BUILDERS."""
    return tuple(name for name, _ in model.SOLVE_BUILDERS)


def namespace_close(T):
    """Token index of the `}` closing the last `namespace GnssProcessor {` of a file."""
    js = [j for j in range(T.n - 2) if T.t(j) == "namespace" and T.t(j + 1) == "GnssProcessor"
          and T.t(j + 2) == "{"]
    if not js:
        raise GenError(f"{T.path}: no namespace GnssProcessor block")
    return T.match[js[-1] + 2]


def include_point(T):
    """Character position after the last top-level #include before the first namespace."""
    L = Lines(T.source)
    depth, best = 0, None
    for j in range(T.n):
        t = T.toks[j]
        if t.kind == "id" and t.text == "namespace":
            break
        if t.kind != "pp":
            continue
        word = t.text[1:].strip().split(None, 1)[0] if len(t.text) > 1 else ""
        if word.startswith("if"):
            depth += 1
        elif word == "endif":
            depth -= 1
        elif word == "include" and depth == 0:
            best = j
    if best is None:
        raise GenError(f"{T.path}: no #include before the namespace")
    return L.end(T.toks[best].end)


def inner_param_docs(T, f):
    """{param name: text} of `///` comments written inside a declaration's parameter list."""
    out = {}
    if not f.params:
        return out
    prev_end = T.toks[f.paren[0]].end
    for p in f.params:
        s, e = p.span
        gap = T.source[prev_end:T.toks[s].pos]
        lines = [ln.strip()[3:].strip() for ln in gap.split("\n") if ln.strip().startswith("///")]
        if lines and p.name:
            out[p.name] = " ".join(lines)
        prev_end = T.toks[e].end if e < T.n and T.t(e) == "," else T.toks[e - 1].end
    return out


def classify(g, pl):
    """(run written, run read, epoch read, outputs) of @p pl's bindings, for the Doxygen."""
    run_w, run_r, ep_r = [], [], []
    outs = collections.OrderedDict()
    for k, dp, b in g.bindings_for(pl):
        text, note = g.binding_text(pl, k, b, for_adapter=True)
        if note in ("kept", "CALLABLE", "CALLBACK_DIRECT"):
            continue
        if note == "output" or (note == "written" and not pl.epoch_writable):
            o = b.out
            outs.setdefault(o, []).append((dp.name, b.gtext or model.OUTPUTS[o]))
            continue
        item = f"{text} ({dp.name})"
        if "ep." in text and not text.lstrip("&*").startswith("run."):
            ep_r.append(item + (" [written]" if note == "written" else ""))
        elif dp.kind in ("W", "W*"):
            run_w.append(item)
        else:
            run_r.append(item)
    return run_w, run_r, ep_r, outs


def decl_doc(g, pl, old_lines, inner, adapter=False):
    """The Doxygen lines of a converted declaration (check V7)."""
    row, rr = pl.row, pl.rr
    head, params, tail = parse_doc(old_lines)
    for n, t in inner.items():
        params.setdefault(n, t)
    fixed = {}
    if row.name == SOLVE:
        head, tail, fixed = list(tpl.SOLVE_DOC), list(tpl.SOLVE_RETURN), dict(tpl.SOLVE_PARAMS)
    width = WIDTH - 8
    lines = []
    if adapter:
        callers = ", ".join(sorted(rr.other_callers)) or "none found"
        lines += wrap_text(f"@brief Epoch form of {row.name} (card NOM3 step EP1; {tpl.GEN}): one "
                           "call forwarding to the explicit form above, with the bindings of the "
                           f"frozen call site.  The explicit form stays for its other callers ({callers}).",
                           width, "", "       ")
    else:
        lines += head or [f"@brief {row.name}."]
        lines += [""] + wrap_text(
            f"Epoch form (card NOM3 step EP1; {tpl.GEN}): the run and the epoch replace the explicit "
            "parameters; the definition binds each former parameter in its preamble, under its "
            "former name.", width)
    run_w, run_r, ep_r, outs = classify(g, pl)
    lines.append("")
    t = fixed.get("run") or "The run (app/NetworkRun.hpp)."
    if run_w:
        t += "  Written through non-const bindings: " + ", ".join(run_w) + "."
    if run_r:
        t += "  Read: " + ", ".join(run_r) + "."
    lines += wrap_text(t, width, "@param run ", "           ")
    if row.mech != RUN:
        t = fixed.get("ep") or ("The epoch (app/NetworkEpoch.hpp), written through its members "
                                "(--presolve-writer epoch)." if pl.epoch_writable
                                else "The epoch (app/NetworkEpoch.hpp), read only.")
        if ep_r:
            t += "  Bindings: " + ", ".join(ep_r) + "."
        lines += wrap_text(t, width, "@param ep ", "          ")
    for k in g.kept_index(pl):
        n = rr.decl.params[k].name
        lines += wrap_text(params.get(n, "") + "  (Kept explicit: its argument differs between "
                           "call sites.)", width, f"@param {n} ", "    ")
    for o in pl.outs:
        if o in tpl.GROUP_TYPES:
            members = ", ".join(f"{gt} ({n})" for n, gt in outs.get(o, []))
            words = group_words(o)[0].upper() + group_words(o)[1:]
            t = fixed.get(o) or f"[in/out] {words} ({model.OUTPUTS[o]}), written: {members}."
            lines += wrap_text(t, width, f"@param {o} ", "    ")
        else:
            k = g.single_out_index(pl, o)
            n = rr.decl.params[k].name
            t = fixed.get(o) or f"[in/out] {model.OUTPUTS[o]}.  {params.get(n, '')}"
            lines += wrap_text(t, width, f"@param {n} ", "    ")
    former = []
    for k, dp, b in g.bindings_for(pl):
        text, note = g.binding_text(pl, k, b, for_adapter=True)
        if note == "kept":
            continue
        if note in ("CALLABLE", "CALLBACK_DIRECT"):
            rem = model.REMOVED_BY_PARAM[(row.name, dp.name)]
            text = f"(removed: {rem.note})"
        elif note == "output":
            text = f"the output parameter itself ({model.OUTPUTS.get(b.out, '')} at the spine line)"
        elif text is None:
            text = model.OUTPUTS.get(b.out, "")
        body = params.get(dp.name, "")
        former += wrap_text(f"- @p {dp.name} = {text}" + (f": {body}" if body else "."), width,
                            "  ", "    ")
    if former:
        lines += ["", "The former parameters, by binding:"] + former
    if tail:
        lines += [""] + tail
    elif adapter and pl.ret and pl.ret != "void":
        lines += ["", "@return As the explicit form."]
    return lines


def fwd_lines(types, indent, after_decl=False):
    """The forward declarations of the new types a seam header's epoch forms name.  Before a
    Doxygen block they end with a blank line; after a declaration (an adapter's insertion,
    which opens with its own blank line) they start with one."""
    out = []
    for t in types:
        home = tpl.GROUP_HOME.get(t, {"NetworkRun": "app/NetworkRun.hpp",
                                      "NetworkEpoch": "app/NetworkEpoch.hpp"}.get(t, ""))
        out.append(f"{indent}struct {t};".ljust(len(indent) + 32) + f"// {home} (card NOM3 step EP1)")
    return ("\n" + "\n".join(out) + "\n") if after_decl else ("\n".join(out) + "\n\n")


def seams(g):
    rec = []
    by_hpp = collections.defaultdict(list)
    by_cpp = collections.defaultdict(list)
    for name, pl in g.plans.items():
        by_hpp[pl.rr.decl.path].append(pl)
        by_cpp[pl.rr.defn.path].append(pl)
    # Headers.
    for path, pls in by_hpp.items():
        T = g.tree.tokens(path)
        text, L = T.source, Lines(T.source)
        types = ["NetworkRun", "NetworkEpoch"]
        # Where each declaration's edit begins; the forward declarations go before the
        # earliest, as the first edit at that position (an adapter inserts after its explicit
        # declaration, a converted declaration replaces its Doxygen block).
        starts = []
        for pl in pls:
            for o in pl.outs:
                ty = tpl.GROUP_TYPES.get(o)
                if ty and ty not in types:
                    types.append(ty)
            f = pl.rr.decl
            start = T.toks[f.start].pos
            if pl.row.mech == ADAPTER:
                j = f.paren[1] + 1
                while T.t(j) != ";":
                    j += 1
                starts.append((L.end(T.toks[j].end), L.indent(start), True))
            else:
                db = doc_before(text, start)
                starts.append((db[0] if db else L.start(start), L.indent(start), False))
        if path != APPLICATION_HPP:
            at, indent, after_decl = min(starts)
            g.edits[path].append(Edit(at, at, fwd_lines(types, indent, after_decl),
                                      "T-D forward declarations"))
        for pl in pls:
            f = pl.rr.decl
            start = T.toks[f.start].pos
            indent = L.indent(start)
            db = doc_before(text, start)
            old = doc_lines(text[db[0]:db[1]]) if db else []
            inner = inner_param_docs(T, f)
            params = g.new_params(pl, "decl")
            if pl.row.mech == ADAPTER:
                j = f.paren[1] + 1
                while T.t(j) != ";":
                    j += 1
                pos = L.end(T.toks[j].end)
                ret = collapse_ws(T.src(f.start, f.name_tok))
                head = f"{indent}{ret} {pl.row.name}("
                doc = decl_doc(g, pl, [], {}, adapter=True)
                decl = head + wrap_args(params, len(head), WIDTH, close=");") + "\n"
                g.edits[path].append(Edit(pos, pos, "\n" + render_doc(doc, indent) + decl,
                                          f"T-D adapter declaration {pl.row.name}"))
            else:
                a = db[0] if db else L.start(start)
                b = db[1] if db else L.start(start)
                doc = decl_doc(g, pl, old, inner)
                g.edits[path].append(Edit(a, b, render_doc(doc, indent), f"T-D doc {pl.row.name}"))
                col = g.col_after(T, f.paren[0], text)
                g.edits[path].append(Edit(T.toks[f.paren[0]].end, T.toks[f.paren[1]].pos,
                                          wrap_args(params, col, WIDTH, close=""),
                                          f"T-D signature {pl.row.name}"))
            rec.append({"seam": pl.row.name, "file": path, "frozen_line": f.line, "kind": "declaration",
                        "form": pl.row.form, "params": params})
        if any(pl.row.name == SOLVE for pl in pls):
            old, new = tpl.SOLVE_BANNERS.get(builders(), (None, None))
            k = text.find(old) if old else -1
            if k < 0:
                g.problems.append(f"{SOLVE_HPP}: the banner's callback sentence was not found (not rewritten)")
            else:
                g.edits[path].append(Edit(k, k + len(old), new, "T-F banner"))
    # Sources.
    for path, pls in by_cpp.items():
        T = g.tree.tokens(path)
        text, L = T.source, Lines(T.source)
        adapters = []
        for pl in pls:
            f = pl.rr.defn
            if pl.row.mech == ADAPTER:
                adapters.append(pl)
                continue
            params = g.new_params(pl, "def")
            col = g.col_after(T, f.paren[0], text)
            g.edits[path].append(Edit(T.toks[f.paren[0]].end, T.toks[f.paren[1]].pos,
                                      wrap_args(params, col, WIDTH, close=""),
                                      f"T-D definition {pl.row.name}"))
            bo = f.body[0]
            ins = L.end(T.toks[bo].end)
            if T.line(bo + 1) > T.line(bo):
                indent = L.indent(T.toks[bo + 1].pos)
            else:
                indent = L.indent(T.toks[f.start].pos) + "    "
            lines = g.preamble_lines(pl, indent)
            g.edits[path].append(Edit(ins, ins, "\n".join(lines) + "\n", f"T-D preamble {pl.row.name}"))
            rec.append({"seam": pl.row.name, "file": path, "frozen_line": f.line, "kind": "definition",
                        "body_frozen": [T.line(f.body[0]), T.line(f.body[1])],
                        "preamble": [f"{d} = {b};" for d, b, _ in pl.preamble],
                        "fallback": sorted(pl.fallback)})
            if pl.row.name == SOLVE:
                replay(g, pl, T, path)
        if adapters:
            close = namespace_close(T)
            pos = L.start(T.toks[close].pos)
            parts = ["// ---- Epoch forms (card NOM3 step EP1; " + tpl.GEN + "): one forwarding",
                     "//      call each, with the bindings of the frozen call site; the explicit forms",
                     "//      above stay for their other callers. ----", ""]
            for pl in adapters:
                f = pl.rr.defn
                ret = collapse_ws(T.src(f.start, f.name_tok))
                head = f"{ret} {pl.row.name}("
                parts.append(head + wrap_args(g.new_params(pl, "decl"), len(head), WIDTH, close=") {"))
                args = []
                for k, dp, b in g.bindings_for(pl):
                    t, note = g.binding_text(pl, k, b, for_adapter=True)
                    if t is None:
                        raise GenError(f"{pl.row.name}: adapter parameter {dp.name} has no forwarding argument")
                    args.append(t)
                call = ("return " if pl.ret != "void" else "") + pl.row.name + "("
                parts.append("    " + call + wrap_args(args, 4 + len(call), WIDTH, close=");"))
                parts.append("}")
                parts.append("")
                rec.append({"seam": pl.row.name, "file": path, "kind": "adapter definition",
                            "forward": f"{pl.row.name}({', '.join(args)})"})
            parts += ["// ---- End of the epoch forms ----", "", ""]
            g.edits[path].append(Edit(pos, pos, "\n".join(parts), "T-D adapters"))
        # Includes.
        want = list(EPOCH_INCLUDES)
        if any(pl.row.name == SOLVE for pl in pls):
            # T-F: the replay calls its builders directly, so the solve's source includes their
            # header (the frozen solve reached them only through the callbacks).
            for h in sorted({g.plans[model.REMOVED_BY_PARAM[(SOLVE, n)].builder].rr.decl.path
                             for n in builders()}):
                want.append(f'#include "{h[len("include/"):]}"   // the replay\'s builders (T-F)')
        have = set(re.findall(r'#include\s+"([^"]+)"', text))
        add = [ln for ln in want if re.search(r'"([^"]+)"', ln).group(1) not in have]
        if add:
            p = include_point(T)
            g.edits[path].append(Edit(p, p, "\n".join(add) + "\n", "T-D includes"))
    return rec


def replay(g, pl, T, path):
    """T-F in the solve: the replay's two callback invocations become the builders' calls."""
    f = pl.rr.defn
    for name in builders():
        rem = model.REMOVED_BY_PARAM.get((SOLVE, name))
        bpl = g.plans[rem.builder]
        args = [pl.run_name, pl.ep_name]
        if not pl.epoch_writable:
            for o in bpl.outs:
                if o not in pl.outs:
                    raise GenError(f"T-F: {rem.builder} writes {o}, which {SOLVE} does not output")
                args.append(pl.out_name[o])
        cs = [c for c in calls_in(T, f.body[0], f.body[1]) if c.name == name]
        if len(cs) != 1:
            raise GenError(f"T-F: {SOLVE} invokes {name} {len(cs)} times")
        c = cs[0]
        g.edits[path].append(Edit(T.toks[c.tok].pos, T.toks[c.close].end,
                                  f"{rem.builder}({', '.join(args)})", f"T-F replay {name}"))


def contract_test(g):
    """The text of tests/test_network_epoch_contract.cpp (spec 2.7, 5.9)."""
    heads = sorted({pl.rr.decl.path[len("include/"):] for pl in g.plans.values()
                    if pl.rr.decl.path.startswith("include/") and pl.rr.decl.path != APPLICATION_HPP})
    out = [tpl.CONTRACT_HEAD]
    out.append('#include "app/Application.hpp"')
    out.append('#include "app/NetworkEpoch.hpp"')
    out.append('#include "app/NetworkRun.hpp"')
    out.append('#include "filter/NetworkRunGroups.hpp"')
    out.append('#include "filter/NetworkRunTypes.hpp"')
    out.append('#include "rinex/MultiRinexSync.hpp"')
    for h in heads:
        out.append(f'#include "{h}"')
    out += ["", "#include <gtest/gtest.h>", "", "#include <type_traits>", "", "namespace GnssProcessor {", ""]
    members = [pl for pl in g.plans.values() if pl.row.mech == MEMBER]
    out.append("/// Reads the private member seams' types (a friend of Application): each member's address")
    out.append("/// converts only to the pointer type its row states.")
    out.append("struct NetworkEpochContractProbe {")
    for pl in sorted(members, key=lambda p: g.drv.calls.index(p.rr.sites[0])):
        sig = ", ".join(g.signature_types(pl))
        alias = "Sig" + pl.row.name[len("network"):].rstrip("_")
        out.append(f"    using {alias} = {pl.ret} (Application::*)({sig});")
        out.append(f"    static constexpr {alias} {pl.row.name} = &Application::{pl.row.name};")
    if not members:
        out.append("    // No member seam is converted at this stage.")
    out.append("};")
    out += ["", "} // namespace GnssProcessor", "", "namespace {", "", "using namespace GnssProcessor;", ""]
    out += ["// Specification 2.7: the run, its engines and the epoch are neither copyable nor movable,",
            "// and an epoch cannot be built on a temporary.",
            "static_assert(!std::is_copy_constructible_v<NetworkRun> && !std::is_move_constructible_v<NetworkRun>);",
            "static_assert(!std::is_copy_assignable_v<NetworkRun> && !std::is_move_assignable_v<NetworkRun>);",
            "static_assert(!std::is_copy_constructible_v<NetworkRunEngines>",
            "              && !std::is_move_constructible_v<NetworkRunEngines>);",
            "static_assert(!std::is_copy_constructible_v<NetworkEpoch> && !std::is_move_constructible_v<NetworkEpoch>);",
            "static_assert(!std::is_copy_assignable_v<NetworkEpoch> && !std::is_move_assignable_v<NetworkEpoch>);",
            "static_assert(!std::is_constructible_v<NetworkEpoch, SyncedEpoch&&>);",
            "static_assert(std::is_constructible_v<NetworkEpoch, const SyncedEpoch&>);", ""]
    free = sorted((pl for pl in g.plans.values() if pl.row.mech != MEMBER),
                  key=lambda p: g.drv.calls.index(p.rr.sites[0]))
    if free:
        out += ["// Specification 5.9: every converted free seam's parameter list, from its row.  The cast",
                "// selects the overload with exactly that type (an adapter's explicit form does not",
                "// match), so a seam whose signature drifts from its row does not compile."]
    for pl in free:
        alias = "Sig" + pl.row.name[len("network"):]
        out.append(f"using {alias} = {pl.ret} (*)({', '.join(g.signature_types(pl))});"
                   f"   // {pl.row.worker} {pl.row.form}")
        out.append(f"static_assert(std::is_same_v<decltype(static_cast<{alias}>(&{pl.row.name})), {alias}>);")
    obs = [pl for pl in free if pl.row.observer]
    if obs:
        out += ["", "// Every measure-only row has the observer form."]
        for pl in obs:
            out.append(f"static_assert(std::is_same_v<decltype(static_cast<NetworkEpochObserver>"
                       f"(&{pl.row.name})), NetworkEpochObserver>);")
    out += ["", "} // namespace", "",
            "TEST(NetworkEpochContract, CompiledChecks) {",
            "    // Every check of this file is a static_assert; the case records that it built "
            f"({len(g.plans)} converted seams).",
            "    SUCCEED();",
            "}", ""]
    return "\n".join(out)
