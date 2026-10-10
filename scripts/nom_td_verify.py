#!/usr/bin/env python3
"""Relocation verifier for the run-end reports (card NOM step TD1b, gate G-TD-R).

    python scripts/nom_td_verify.py [--verbose]

The frozen source is networkTeardown as it stood at main 067fdc1, read with `git show` (no
copy is stored).  scripts/nom_td_manifest.json, written by scripts/nom_td_cut.py, records for
every report the frozen line ranges it carries and every declared edit.  Exit 0 only when
all four checks pass:

  1. Bodies.  For every report (and networkTeardown itself), the body is taken from its home
     file by brace matching; its code lines must equal, character for character and
     indentation included, the frozen lines it declares, after the declared transforms:
       - an alias line, an inserted line or the closing `return` ("new" pieces; compared
         without leading and trailing space, since they are declared new code);
       - a frozen range with its shift (leading spaces removed: 4 for a range that sat in a
         scope the cut removed);
       - a guard: the frozen `if (COND) {` becomes `if (!COND) return 0;` (or `!(COND)`) and
         the interior moves one level out; an unwrap: a bare `{ ... }` block's interior
         moves one level out;
       - a declared substitution on one frozen line (plain text replace), or a declared
         dropped line.
     Comments and blank lines are ignored on both sides, as in rt_seam_verify.py.
  2. Coverage.  Every code line of the frozen function body is carried by exactly one
     report, or by networkTeardown, or is a declared drop (with its reason), or belongs to
     the old alias preamble that the reports' own aliases replace.  None is dropped
     silently, none is carried twice.
  3. The table.  kNetworkRunEndReports (src/core/filter/NetworkRunEnd.cpp) lists exactly the
     manifest's reports, each once, in the manifest's order.
  4. The print order.  The ordered list of (logger call, level, format literal), with the
     calls into other run-end functions and the run-manifest writes, over the table walk
     followed by networkTeardown's own lines, equals the frozen function's.  The trace close
     (the estimability RUN row and closeAll, which print nothing) is checked to be the
     walk's first action instead: the table moves it ahead of the timing report.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

MANIFEST = ROOT / "scripts" / "nom_td_manifest.json"
SIMPLE = re.compile(r"^!?[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*(?:\(\))?)*(?:\(\))?$")


def negate(cond):
    c = cond.strip()
    if SIMPLE.match(c) and not c.startswith("!"):
        return "!" + c
    return "!(" + c + ")"


def blank_literals(text):
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup in ("str", "chr", "raw"):
            out.append("".join(c if c == "\n" else " " for c in s))
        else:
            out.append(s)
    return "".join(out)


def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def function_body(code_lines, blank_lines, header_re):
    """(first body line index, last body line index) of the definition matching header_re,
    0-based into the lists; the body is strictly between the braces."""
    starts = [i for i, l in enumerate(code_lines) if re.search(header_re, l)]
    if len(starts) != 1:
        raise ValueError(f"{len(starts)} definitions match {header_re!r}")
    depth, opened, open_i = 0, False, None
    for i in range(starts[0], len(blank_lines)):
        for ch in blank_lines[i]:
            if ch == "{":
                depth += 1
                if not opened:
                    opened, open_i = True, i
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return open_i + 1, i - 1, open_i, i
    raise ValueError("unbalanced braces")


class Frozen:
    def __init__(self, rev, path):
        self.raw = [""] + git_show(rev, path).split("\n")

    def code_with_subs(self, subs):
        """Comment-stripped frozen lines (1-based) after the declared line substitutions."""
        raw = list(self.raw)
        for s in subs:
            i = s["line"]
            if s["from"] not in raw[i]:
                raise ValueError(f"substitution {s['from']!r} not on frozen line {i}")
            raw[i] = raw[i].replace(s["from"], s["to"])
        return [""] + strip_comments("\n".join(raw[1:])).split("\n")


def unshift(line, k, where):
    lead = len(line) - len(line.lstrip(" "))
    if lead < k:
        raise ValueError(f"{where}: fewer than {k} leading spaces")
    return line[k:]


def expected_lines(F, entry, problems):
    """[(kind, text, frozen_line)] the body must hold, and the frozen lines it covers."""
    code = F.code_with_subs(entry["subs"])
    drops = set(entry["drops"])
    exp, covered = [], []
    name = entry["func"]

    def take(a, b, k):
        for i in range(a, b + 1):
            if not code[i].strip():
                continue
            covered.append(i)
            if i in drops:
                continue
            exp.append(("frozen", unshift(code[i], k, f"{name} frozen {i}").rstrip(), i))

    for p in entry["pieces"]:
        if "new" in p:
            for part in strip_comments(p["new"]).split("\n"):
                if part.strip():
                    exp.append(("new", part.strip(), None))
        elif "frozen" in p:
            take(p["frozen"][0], p["frozen"][1], p["shift"])
        elif "guard" in p:
            a, b = p["guard"]
            s, e, k = p["if_line"], p["close_line"], p["shift"]
            if code[s].strip() != f"if ({p['cond']}) {{":
                problems.append(f"{name}: frozen {s} is not `if ({p['cond']}) {{`")
            if code[e].strip() != "}":
                problems.append(f"{name}: frozen {e} is not the guard's closing brace")
            if p["line"] != f"if ({negate(p['cond'])}) return 0;":
                problems.append(f"{name}: guard line {p['line']!r} is not the negation of the frozen condition")
            if any(code[i].strip() for i in range(e + 1, b + 1)):
                problems.append(f"{name}: code after the guard's closing brace")
            take(a, s - 1, k)
            covered.append(s)
            exp.append(("new", p["line"], s))
            take(s + 1, e - 1, k + 4)
            covered.append(e)
        elif "unwrap" in p:
            a, b = p["unwrap"]
            s, e, k = p["open_line"], p["close_line"], p["shift"]
            if code[s].strip() != "{" or code[e].strip() != "}":
                problems.append(f"{name}: frozen {s}/{e} are not a bare block's braces")
            take(a, s - 1, k)
            covered.append(s)
            take(s + 1, e - 1, k + 4)
            covered.append(e)
            if any(code[i].strip() for i in range(e + 1, b + 1)):
                problems.append(f"{name}: code after the block's closing brace")
    return exp, covered


def actual_lines(path, func, indent):
    text = (ROOT / path).read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
    code = strip_comments(text).split("\n")
    blank = blank_literals(strip_comments(text)).split("\n")
    hdr = (r"^\s*int " + re.escape(func) + r"\(const NetworkRunEndView& v\)"
           if func != "networkTeardown" else r"^int networkTeardown\(")
    a, b, _, _ = function_body(code, blank, hdr)
    out = []
    for i in range(a, b + 1):
        if code[i].strip():
            out.append((i + 1, unshift(code[i], indent, f"{path}:{i + 1}").rstrip()))
    return out, "\n".join(code[a:b + 1])


def compare(entry, exp, act, problems, verbose):
    name = entry["func"]
    ok = len(exp) == len(act)
    first = None
    for j in range(max(len(exp), len(act))):
        e = exp[j] if j < len(exp) else None
        a = act[j] if j < len(act) else None
        if e is None or a is None:
            first = j
            ok = False
            break
        if e[0] == "new":
            same = a[1].strip() == e[1]
        else:
            same = a[1] == e[1]
        if not same:
            first = j
            ok = False
            break
    if ok:
        if verbose:
            print(f"OK   {name}  ({sum(1 for e in exp if e[0] == 'frozen')} frozen code lines)")
        return True
    e = exp[first] if first is not None and first < len(exp) else None
    a = act[first] if first is not None and first < len(act) else None
    problems.append(f"{name} ({entry['file']}): first difference at body code line {first}:\n"
                    f"       expected ({'frozen ' + str(e[2]) if e and e[2] else 'declared'}): "
                    f"{e[1] if e else '<end>'}\n"
                    f"       found    ({entry['file']}:{a[0] if a else '<end>'}): {a[1] if a else '<end>'}")
    return False


# ---- the print order --------------------------------------------------------------------
CALL_RE = re.compile(r"\b(network[A-Z]\w*(?:RunEnd|Report|Teardown))\s*\(|"
                     r"consumer->(log\w+Summary)\s*\(|"
                     r"traces\.(appendRunManifestRecord|writeRunManifestComplete|"
                     r"writeFlagRateAndRunSummary|writeEstimabilityRunRow|closeAll)\s*\(|"
                     r"\blog->(info|warn|error|log)\s*\(")


def literal_after(text, pos):
    """The format literal at pos: adjacent string literals, joined; and the level of a
    log->log call (its first argument) when asked."""
    out = []
    for m in _CPP.finditer(text, pos):
        g = m.lastgroup
        s = m.group()
        if g == "other" and s.isspace():
            continue
        if g in ("lc", "bc"):
            continue
        if g == "str":
            out.append(s)
            continue
        break
    return "".join(out)


def events(text):
    """Ordered events of a body's code (comments already stripped)."""
    ev = []
    for m in CALL_RE.finditer(text):
        if m.group(1):
            ev.append(("call", m.group(1)))
        elif m.group(2):
            ev.append(("consumer", m.group(2)))
        elif m.group(3):
            ev.append(("traces", m.group(3)))
        else:
            level = m.group(4)
            pos = m.end()
            if level == "log":
                # the first argument, to its top-level comma
                depth, j = 0, pos
                while j < len(text):
                    ch = text[j]
                    if ch in "([{":
                        depth += 1
                    elif ch in ")]}":
                        depth -= 1
                    elif ch == "," and depth == 0:
                        break
                    j += 1
                level = "log:" + re.sub(r"\s+", " ", text[pos:j]).strip()
                pos = j + 1
            ev.append(("log", level, literal_after(text, pos)))
    return ev


def table_rows(path):
    text = (ROOT / path).read_text(encoding="utf-8")
    body = text[text.index("kNetworkRunEndReports = {{"):]
    return re.findall(r'\{"((?:[^"\\]|\\.)*)",\s*"([^"]+)",\s*(\w+)\}', body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    m = json.loads(MANIFEST.read_text(encoding="utf-8"))
    F = Frozen(m["frozen"]["rev"], m["frozen"]["path"])
    fcode = [""] + strip_comments("\n".join(F.raw[1:])).split("\n")
    fblank = [""] + blank_literals(strip_comments("\n".join(F.raw[1:]))).split("\n")
    a0, b0, _, _ = function_body(fcode[1:], fblank[1:], r"^int networkTeardown\(")
    body_first, body_last = a0 + 1, b0 + 1          # 1-based frozen body lines

    problems = []
    cover = {}
    funcs = m["reports"] + [m["teardown"]]
    n_ok = 0
    acts = {}
    for entry in funcs:
        try:
            exp, covered = expected_lines(F, entry, problems)
            act, text = actual_lines(entry["file"], entry["func"], entry["indent"])
        except ValueError as e:
            problems.append(f"{entry['func']}: {e}")
            continue
        acts[entry["func"]] = text
        for i in covered:
            cover.setdefault(i, []).append(entry["func"])
        if compare(entry, exp, act, problems, args.verbose):
            n_ok += 1

    # 2. Coverage.
    pre_a, pre_b = m["preamble"]
    gdrops = {int(k): v for k, v in m["global_drops"].items()}
    for i in gdrops:
        if not fcode[i].strip():
            problems.append(f"declared drop {i} is not a code line")
        cover.setdefault(i, []).append("drop")
    for i in range(pre_a, pre_b + 1):
        if fcode[i].strip():
            cover.setdefault(i, []).append("preamble")
    missing = [i for i in range(body_first, body_last + 1) if fcode[i].strip() and i not in cover]
    twice = {i: w for i, w in cover.items() if len(w) > 1}
    outside = [i for i in cover if not (body_first <= i <= body_last)]
    for i in missing:
        problems.append(f"frozen line {i} is carried by no report: {fcode[i].strip()[:90]}")
    for i, w in sorted(twice.items()):
        problems.append(f"frozen line {i} is carried twice ({', '.join(w)})")
    for i in outside:
        problems.append(f"frozen line {i} is outside the function body")
    n_code = sum(1 for i in range(body_first, body_last + 1) if fcode[i].strip())

    # 3. The table.  A row added after the cut (filter/NetworkRunEnd.hpp, "Adding a report")
    #    has no frozen body and is not in the manifest: it is named below, not failed, and
    #    the cut's own rows must still appear in the manifest's order with their homes.
    rows = table_rows(m["table"])
    want = [e["func"] for e in m["reports"]]
    post_cut = [r[2] for r in rows if r[2] not in set(want)]
    rows = [r for r in rows if r[2] in set(want)]
    names = [r[2] for r in rows]
    if names != want:
        problems.append(f"the table lists {len(names)} rows; the manifest {len(want)} reports, "
                        f"and they differ (first at {next((k for k in range(min(len(names), len(want))) if names[k] != want[k]), min(len(names), len(want)))})")
    for r, e in zip(rows, m["reports"]):
        if r[1] != e["file"].replace("src/core/", ""):
            problems.append(f"table row {r[2]}: home {r[1]} is not {e['file']}")

    # 4. The print order.
    inverse = {v: k for k, v in m["renames"].items()}
    fev = events("\n".join(fcode[body_first:body_last + 1]))
    wev = []
    for fn in names:
        ev = events(acts.get(fn, ""))
        wev += [(e[0], inverse.get(e[1], e[1])) + e[2:] for e in ev]
    wev += events(acts.get("networkTeardown", ""))
    silent = [("traces", "writeEstimabilityRunRow"), ("traces", "closeAll")]
    if wev[:2] != silent:
        problems.append(f"the walk does not open with the trace close: {wev[:2]}")
    fev_cmp = [e for e in fev if e not in silent]
    wev_cmp = [e for e in wev if e not in silent]
    if fev_cmp != wev_cmp:
        k = next((k for k in range(min(len(fev_cmp), len(wev_cmp))) if fev_cmp[k] != wev_cmp[k]),
                 min(len(fev_cmp), len(wev_cmp)))
        problems.append(f"print order differs at event {k} of {len(fev_cmp)}/{len(wev_cmp)}:\n"
                        f"       frozen: {fev_cmp[k] if k < len(fev_cmp) else '<end>'}\n"
                        f"       walk  : {wev_cmp[k] if k < len(wev_cmp) else '<end>'}")
    n_log = sum(1 for e in fev if e[0] == "log")
    n_call = sum(1 for e in fev if e[0] != "log")

    for p in problems:
        print("FAIL " + p)
    if post_cut:
        print(f"table rows added after the cut (not verified here): {', '.join(post_cut)}")
    print(f"\n{n_ok} of {len(funcs)} bodies verbatim in code ({len(m['reports'])} reports + "
          f"networkTeardown); {n_code} frozen code lines, {len(missing)} uncovered, "
          f"{len(twice)} carried twice; table {len(names)} rows; print order {len(fev_cmp)} "
          f"events ({n_log} log calls, {n_call} calls and manifest writes) "
          f"{'equal' if fev_cmp == wev_cmp else 'DIFFERENT'}.")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
