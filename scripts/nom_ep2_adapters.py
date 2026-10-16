#!/usr/bin/env python3
"""Card NOM3 step EP2: retires the epoch-form adapters a unit test was the only reason for.

    python scripts/nom_ep2_adapters.py

For each seam in ADAPTERS, the explicit form's body becomes the epoch form's body: the
explicit form's parameters become the epoch form's bindings, each initialized by the argument
the adapter forwarded (so every name keeps the object it denoted at the frozen call site);
the adapter and the explicit declaration go.  In the header the explicit declaration's
documentation is kept on the epoch form, its @param lines becoming the "former parameters, by
binding" list.  The tests call the epoch form through tests/support/NetworkTestRun.hpp.
Files keep their line ends.  Idempotent only on the base text (the script refuses a file whose
explicit form is already gone).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import nom_cpp as nc   # noqa: E402

ADAPTERS = [
    ("networkSignalIntegrity", "src/core/filter/NetworkSignalIntegrity.cpp", "include/filter/NetworkSignalIntegrity.hpp"),
    ("networkMeasurementAdmission", "src/core/filter/NetworkMeasurementAdmission.cpp", "include/filter/NetworkMeasurementAdmission.hpp"),
    ("networkHandoverStransformBeforeRows", "src/core/filter/NetworkHandoverStransform.cpp", "include/filter/NetworkHandoverStransform.hpp"),
    ("networkCarryDecide", "src/core/filter/NetworkCarryDisposition.cpp", "include/filter/NetworkCarryDisposition.hpp"),
    ("networkSolPrep", "src/core/filter/NetworkSolPrep.cpp", "include/filter/NetworkSolPrep.hpp"),
    ("networkPbHoldCloneUpdate", "src/core/filter/NetworkPbHoldClone.cpp", "include/filter/NetworkPbHoldClone.hpp"),
    ("networkPbHoldCloneHarvest", "src/core/filter/NetworkPbHoldClone.cpp", "include/filter/NetworkPbHoldClone.hpp"),
    ("networkPhaseVisibleMonitor", "src/core/filter/NetworkPhaseVisibleDiag.cpp", "include/filter/NetworkPhaseVisibleDiag.hpp"),
    ("networkLedgerEmitFold", "src/core/filter/NetworkLedgerEmit.cpp", "include/filter/NetworkLedgerEmit.hpp"),
]
OUTPUT_ROOTS = ("funnel.", "pass_a.")


def load(path):
    raw = open(os.path.join(ROOT, path), "rb").read().decode("utf-8")
    return raw.replace("\r\n", "\n"), "\r\n" in raw


def save(path, text, crlf):
    if crlf:
        text = text.replace("\n", "\r\n")
    open(os.path.join(ROOT, path), "wb").write(text.encode("utf-8"))


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


def doc_text(l):
    """A doc-comment line's text after its leading `*` and one space (alignment kept)."""
    t = l.lstrip()
    if t.startswith("*"):
        t = t[1:]
        if t.startswith(" "):
            t = t[1:]
    return t.rstrip()


EMPTY_EPOCH_SECTION = re.compile(r"\n// ---- Epoch forms \(card NOM3 step EP1[^\n]*\n(?://[^\n]*\n)*\n?"
                                 r"// ---- End of the epoch forms ----\n\n?")


def drop_empty_epoch_section(text):
    """Removes an EP1 "Epoch forms" section the retirement left with no code."""
    m = EMPTY_EPOCH_SECTION.search(text)
    if m:
        text = text[:m.start()] + "\n" + text[m.end():]
    return text


def defs(T, name):
    """[(name token, open paren, close paren, body open, body close)] of the definitions."""
    out = []
    for i, t in enumerate(T.toks):
        if t.text == name and T.k(i) == "id" and T.t(i + 1) == "(":
            j = T.match[i + 1]
            if j > 0 and T.t(j + 1) == "{" and T.t(i - 1) not in (".", "->", "return", "=", "(", ","):
                out.append((i, i + 1, j, j + 1, T.match[j + 1]))
    return out


def split_top(T, a, b):
    """Source slices of the comma-separated items in tokens (a, b) (exclusive)."""
    items, start, k, angle = [], a + 1, a + 1, 0
    while k < b:
        if T.t(k) in nc.OPEN and T.k(k) == "op":
            k = T.match[k] + 1
            continue
        if T.t(k) == "<" and T.k(k - 1) == "id":   # a template argument list (no comparison occurs here)
            angle += 1
        elif T.t(k) in (">", ">>") and angle:
            angle -= len(T.t(k))
        if T.t(k) == "," and T.k(k) == "op" and angle == 0:
            items.append((start, k))
            start = k + 1
        k += 1
    if start < b:
        items.append((start, b))
    src = T.source
    return [norm(src[T.toks[s].pos:T.toks[e - 1].end]) for s, e in items]


def line_start(text, pos):
    return text.rfind("\n", 0, pos) + 1


def convert_cpp(name, path):
    text, crlf = load(path)
    T = nc.Tokens(text, path)
    ds = defs(T, name)
    expl = [d for d in ds if T.t(d[1] + 1) != "NetworkRun"]
    adap = [d for d in ds if T.t(d[1] + 1) == "NetworkRun"]
    if len(expl) != 1 or len(adap) != 1:
        sys.exit(f"{path}: {name}: {len(expl)} explicit, {len(adap)} epoch forms")
    e, a = expl[0], adap[0]
    params = split_top(T, e[1], e[2])
    # The adapter's one forwarding statement.
    k = a[3] + 1
    if T.t(k) == "return":
        k += 1
    if T.t(k) != name or T.t(k + 1) != "(":
        sys.exit(f"{path}: {name}: the adapter is not one forwarding call")
    args = split_top(T, k + 1, T.match[k + 1])
    if len(args) != len(params):
        sys.exit(f"{path}: {name}: {len(params)} parameters, {len(args)} arguments")
    body_ids = {T.toks[i].text for i in range(e[3], e[4]) if T.k(i) == "id"}
    rn = "nrun" if "run" in body_ids else "run"
    en = "nep" if "ep" in body_ids else "ep"
    sig = text[line_start(text, T.toks[a[0]].pos):T.toks[a[3]].pos].rstrip()
    if rn != "run" or en != "ep":
        sig = sig.replace("NetworkRun& run", "NetworkRun& " + rn).replace("const NetworkEpoch& ep", "const NetworkEpoch& " + en)
    binds = []
    for p, x in zip(params, args):
        p = p.split(" = ")[0]   # a default argument (the definition repeats none, by the rule)
        x = re.sub(r"\brun\b", rn, x)
        x = re.sub(r"\bep\b", en, x)
        mark = "  // written" if x.startswith(OUTPUT_ROOTS) else ""
        binds.append(f"    {p} = {x};{mark}")
    body = text[T.toks[e[3]].end:T.toks[e[4]].pos]
    new = (sig + " {\n"
           "    // ---- From the run and the epoch (card NOM3 step EP2; generated by\n"
           "    //      scripts/nom_ep2_adapters.py: the explicit form's parameters, each bound to the\n"
           "    //      argument the retired adapter forwarded). Reads bind through `ep`; writes bind\n"
           "    //      through the output parameters and are marked `// written`. ----\n"
           + "\n".join(binds) + "\n"
           "    // ---- End of the bindings ----" + body.rstrip(" ") + "}")
    # Replace the explicit definition in place, then delete the adapter (it comes later).
    es = line_start(text, T.toks[e[0]].pos)
    # the return type may start on the same line as the name (checked) or the line above
    if text[es:T.toks[e[0]].pos].strip() == "":
        es = line_start(text, es - 1)
    ee = T.toks[e[4]].end
    as_ = line_start(text, T.toks[a[0]].pos)
    ae = T.toks[a[4]].end
    if not (ee < as_):
        sys.exit(f"{path}: {name}: the adapter does not follow the explicit form")
    tail_ae = ae + (1 if text[ae:ae + 1] == "\n" else 0)
    if text[tail_ae:tail_ae + 1] == "\n":
        tail_ae += 1
    text = text[:es] + new + text[ee:as_] + text[tail_ae:]
    text = drop_empty_epoch_section(text)
    save(path, text, crlf)
    return params, args


def convert_hpp(name, path, params, args):
    text, crlf = load(path)
    T = nc.Tokens(text, path)
    # Declarations (not definitions): name ( ... ) ;
    decls = []
    for i, t in enumerate(T.toks):
        if t.text == name and T.k(i) == "id" and T.t(i + 1) == "(":
            j = T.match[i + 1]
            if j > 0 and T.t(j + 1) == ";":
                decls.append((i, i + 1, j))
    expl = [d for d in decls if T.t(d[1] + 1) != "NetworkRun"]
    epf = [d for d in decls if T.t(d[1] + 1) == "NetworkRun"]
    if len(expl) != 1 or len(epf) != 1:
        sys.exit(f"{path}: {name}: {len(expl)} explicit, {len(epf)} epoch declarations")

    def doc_before(i):
        ls = line_start(text, T.toks[i].pos)
        if text[ls:T.toks[i].pos].strip() == "":
            pass
        end = text.rfind("*/", 0, ls)
        between = text[end + 2:ls] if end >= 0 else "x"
        # the return type may sit on the name's line; the doc ends right above it
        if end < 0 or between.strip() != "":
            return None
        start = text.rfind("/**", 0, end)
        return start, end + 2

    def decl_span(d):
        ls = line_start(text, T.toks[d[0]].pos)
        return ls, T.toks[d[2] + 1].end

    xd = doc_before(expl[0][0])
    ed = doc_before(epf[0][0])
    if xd is None or ed is None:
        sys.exit(f"{path}: {name}: a declaration without its doc comment")
    xdoc = text[xd[0]:xd[1]]
    edoc = text[ed[0]:ed[1]]
    ind = text[line_start(text, xd[0]):xd[0]]
    # Parse the explicit doc: description lines and @param name -> text.
    lines = [doc_text(l) for l in xdoc[3:-2].split("\n")]
    desc, pdoc, ret, cur = [], {}, [], None
    for l in lines:
        t = l.strip()
        m = re.match(r"@param\s+(\w+)\s*(.*)", t)
        if m:
            cur = m.group(1)
            pdoc[cur] = [m.group(2)]
            continue
        if t.startswith("@return"):
            cur = "@return"
            ret.append(l)
            continue
        if t.startswith("@") and not t.startswith("@brief"):
            cur = None
        if cur == "@return":
            ret.append(l)
        elif cur:
            pdoc[cur].append(t)
        else:
            desc.append(l)
    while desc and desc[-1].strip() == "":
        desc.pop()
    while desc and desc[0].strip() == "":
        desc.pop(0)
    # The epoch form's own @param run / @param ep lines.
    elines = [doc_text(l) for l in edoc[3:-2].split("\n")]
    keep, on = [], False
    for l in elines:
        t = l.strip()
        if t.startswith("@param run") or t.startswith("@param ep"):
            on = True
        elif not t or t.startswith("The former parameters") or t.startswith("- @p") or t.startswith("@"):
            on = False
        if on:
            keep.append(l)
    out = ["/**"]
    out += [(" * " + l).rstrip() for l in desc]
    out += [" *", " * Epoch form (card NOM3 step EP1); the explicit form and its forwarding adapter",
            " * retired in card NOM3 step EP2 (the unit tests call this form through",
            " * tests/support/NetworkTestRun.hpp).", " *"]
    out += [" * " + l for l in keep]
    out += [" *", " * The former parameters, by binding:"]
    for p, x in zip(params, args):
        pname = re.findall(r"\w+", p.split(" = ")[0])[-1]
        d = " ".join(s for s in pdoc.get(pname, []) if s).strip()
        out.append(f" *   - @p {pname} = {x}" + (f": {d}" if d else "."))
    if ret:
        out += [" *"] + [(" * " + l).rstrip() for l in ret]
    out.append(" */")
    newdoc = ("\n" + ind).join(out)
    xs, xe = decl_span(expl[0])
    # Delete the explicit doc + declaration (and one following blank line), rewrite the epoch doc.
    pieces = [(ed[0], ed[1], newdoc), (xd[0] - len(ind), xe, None)]
    if xe < len(text) and text[xe:xe + 1] == "\n":
        pieces[1] = (xd[0] - len(ind), xe + 1 + (1 if text[xe + 1:xe + 2] == "\n" else 0), None)
    for s, e, rep in sorted(pieces, key=lambda p: -p[0]):
        text = text[:s] + (rep or "") + text[e:]
    save(path, text, crlf)


def main():
    for name, cpp, hpp in ADAPTERS:
        params, args = convert_cpp(name, cpp)
        convert_hpp(name, hpp, params, args)
        print(f"retired: {name} ({len(params)} parameters)")


if __name__ == "__main__":
    main()
