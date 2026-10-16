"""nom_ep1c_verify.py: card NOM3 step EP1c, the const tightenings.

Each tightening makes one preamble binding const (or removes a writable funnel parameter in
favour of a const read through `ep`).  Compilation proves every use of a tightened object is a
const use; this check proves no use changed meaning (EP1 specification, the EP1c paragraph):
for every member call on a tightened object the selected overload is the same function, or a
const twin with an identical body.

  T1  the binding line is const at every site.
  T2  every member named on a tightened NetworkEstimator is declared const in every overload
      (the class has no const/non-const twin), so the selected overload is the same function.
  T3  every member named on a tightened standard container is in the table below: const-only
      members select the same function; a const twin of the standard library (operator[],
      find, begin, end) has the same body with a const-qualified result.
  T4  every free call that receives a tightened object has no declaration taking it by
      non-const reference (a non-const twin would have been selected before the tightening).

Usage: python scripts/nom_ep1c_verify.py [repo]
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1])

EST = "const NetworkEstimator& est = run.engines.est;"
# (file, function, object name, binding line, kind)
SITES = [
    ("src/core/filter/NetworkDdFrac.cpp", "networkDdFrac", "est", EST, "estimator"),
    ("src/core/filter/NetworkShDevVtec.cpp", "networkShDevVtec", "est", EST, "estimator"),
    ("src/core/filter/NetworkShHealth.cpp", "networkShHealth", "est", EST, "estimator"),
    ("src/core/filter/NetworkStashStatus.cpp", "networkStashStatus", "est", EST, "estimator"),
    ("src/core/filter/NetworkFrameDiag.cpp", "networkPostfitSigma0", "est", EST, "estimator"),
    ("src/core/filter/NetworkStationBlock.cpp", "networkStationBlock", "est", EST, "estimator"),
    ("src/core/filter/NetworkRhsAttrib.cpp", "networkRhsAttribution", "est", EST, "estimator"),
    # networkStashStatus hands its estimator to the border stash, whose parameter follows it.
    ("src/core/app/NetworkBorderStash.cpp", "networkStashCarry", "est",
     "const NetworkEstimator& est, const Eigen::VectorXd& x_s,", "estimator"),
    ("src/core/filter/NetworkDdPriorBuild.cpp", "networkBuildCarryDdPriors", "carry_slipped_ambs",
     "const std::set<std::pair<int, std::string>>& carry_slipped_ambs = ep.funnel.carry_slipped_ambs;",
     "std::set"),
    ("src/core/filter/NetworkSolveStations.cpp", "networkAssemblyConsume", "modeled",
     "const std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;", "std::vector"),
]
# Standard containers: member -> "const" (one const function) or "twin" (a const twin whose
# body is the non-const one's with a const-qualified result, in the standard and in MSVC STL).
STD = {
    "std::set": {"count": "const", "size": "const", "empty": "const", "contains": "const",
                 "find": "twin", "begin": "twin", "end": "twin", "lower_bound": "twin",
                 "upper_bound": "twin", "equal_range": "twin"},
    "std::vector": {"size": "const", "empty": "const", "operator[]": "twin", "at": "twin",
                    "begin": "twin", "end": "twin", "front": "twin", "back": "twin", "data": "twin"},
}
TYPES = r"(NetworkEstimator|std::set<std::pair<int, std::string>>|std::vector<std::vector<PppObservation>>)"


def strip(text):
    """Comments and string or character literals blanked (newlines kept)."""
    blank = lambda m: re.sub(r"[^\n]", " ", m.group(0))
    out = re.sub(r"//[^\n]*|/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, out)


def body_of(text, fn):
    m = re.search(r"^\w[\w:<>]*\s+" + fn + r"\s*\((?:[^(){};]|\([^()]*\))*\)\s*\{", text, flags=re.M)
    assert m, fn
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def estimator_members():
    """name -> set of qualifiers ('const' / 'mut') over the class's member declarations."""
    src = strip((ROOT / "include/filter/NetworkEstimator.hpp").read_text(encoding="utf-8"))
    m = re.search(r"class\s+NetworkEstimator\b[^;{]*\{", src)
    i, depth, buf, decl = m.end(), 1, "", {}
    while depth:
        c = src[i]
        i += 1
        if c in "{;" and depth == 1:
            d = re.search(r"(operator\s*\S+?|~?\w+)\s*\(((?:[^()]|\([^()]*\))*)\)\s*(const)?[^()]*$",
                          " ".join(buf.split()))
            if d:
                decl.setdefault(d.group(1), set()).add("const" if d.group(3) else "mut")
            buf = ""
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 1 and c != ";":
            buf += c
    return decl


def free_callees(body, name):
    """Names of the calls that receive `name` as a whole argument."""
    out = set()
    for m in re.finditer(r"\b" + name + r"\b", body):
        if re.match(r"\s*(\.|->|\[)", body[m.end():]) or re.search(r"(\.|->|::)\s*$", body[:m.start()]):
            continue
        depth, j = 0, m.start() - 1
        while j >= 0:
            c = body[j]
            if c in ")]}":
                depth += 1
            elif c in "([{":
                if depth == 0:
                    break
                depth -= 1
            elif c == ";" and depth == 0:
                j = -1
                break
            j -= 1
        if j >= 0 and body[j] == "(":
            f = re.search(r"([A-Za-z_]\w*)\s*$", body[:j])
            if f and f.group(1) not in ("if", "while", "for", "switch", "return", "sizeof"):
                out.add(f.group(1))
    return out


def main():
    fails, rows = [], []
    est_decl = estimator_members()
    decl_text = "\n".join(strip(p.read_text(encoding="utf-8", errors="replace"))
                          for d in ("include", "src") for p in (ROOT / d).rglob("*.[hc]pp"))
    for path, fn, name, binding, kind in SITES:
        raw = (ROOT / path).read_text(encoding="utf-8")
        if binding not in raw:
            fails.append(f"T1 {path}:{fn}: binding not const: {binding}")
            continue
        body = body_of(strip(raw), fn)
        members = set(re.findall(r"\b" + name + r"\s*(?:\.|->)\s*(\w+)", body))
        if re.search(r"\b" + name + r"\s*\[", body):
            members.add("operator[]")
        if re.search(r":\s*" + name + r"\s*\)", body):
            members |= {"begin", "end"}
        for mem in sorted(members):
            if kind == "estimator":
                q = est_decl.get(mem)
                ok = q == {"const"}
                rows.append(f"  {fn:26s} est.{mem:32s} {'const only' if ok else q}")
                if not ok:
                    fails.append(f"T2 {fn}: est.{mem} declared {q}")
            else:
                q = STD[kind].get(mem)
                rows.append(f"  {fn:26s} {name}.{mem} {q or 'NOT IN TABLE'}")
                if not q:
                    fails.append(f"T3 {fn}: {name}.{mem} not in the {kind} table")
        for callee in sorted(free_callees(body, name)):
            sigs = re.findall(r"\b" + callee + r"\s*\(([^;{]*?)\)\s*(?:const\s*)?[;{]", decl_text)
            mut = [s for s in sigs if re.search(r"(?<!const )\b" + TYPES + r"\s*&", s)]
            rows.append(f"  {fn:26s} {callee}({name}) "
                        f"{'no non-const overload' if not mut else 'NON-CONST OVERLOAD'}")
            if mut:
                fails.append(f"T4 {fn}: {callee} has a non-const-reference declaration")
    print("nom_ep1c_verify:", ROOT)
    print("\n".join(rows))
    for f in fails:
        print("  [FAIL]", f)
    print("nom_ep1c_verify:", "FAIL" if fails else f"PASS ({len(SITES)} sites, {len(rows)} uses)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
