#!/usr/bin/env python3
"""Relocation verifier (gate G-R) of card NOM3 step HV1: the harvest cut by carried kind.

    python scripts/nom_hv1_verify.py [--controls]

Reads the frozen networkHarvestLocalCarries at rows_hv1.BASE (git) and the working tree, and
proves (exit 0 only when every check passes; the authorized reduced G-R of the step):

  H2 coverage   every code line of the frozen function is claimed exactly once (carried by
                one function, replaced, or dropped); each dropped line is of its declared kind;
                every anchor resolves to exactly one statement, at its row's first statement.
  H1/H6 bodies  the working tree's orchestrator, with every harvest call inlined back (the
                bindings stripped, the declared transforms reversed: guard, unwrap, the closed
                substitution list, the extracts, the merged census renamed back for the Z
                call) reproduces the frozen function line by line (comments included, the Z
                census token by token), drops excepted.  Hence the ordered statements, log
                calls, PRM responses, event writes and container writes are equal; the
                ordered call/log vocabulary is also compared and printed.
  H3 bindings   every binding line is a frozen parameter bound to the object networkLeveling's
                frozen call passed (declarator text equal, `const` alone addable), a frozen
                alias statement verbatim, or a declared border/layout binding; every name a
                moved body reads that the frozen function declared outside the body is bound.
  H4 closure    every binding reads only names bound above it or run, ep, border, sb, i.
  H5 eval point the run-state aliases and re-evaluated loop-head lines have no writer between
                their frozen and their new evaluation point.
  H7 control    each CONTINUE_TO_RETURN line is at the station loop's own scope.
  H8 layout     each layout read names its own kind's segment; the epilogue and
                harvestBlockSpans are the declared text.
  H9 merge      the frozen L and Z census bodies are token-identical under the renaming.
  H10 (HV1b)    the four rx-CB run fields moved to the service counters, declarators equal,
                no ctx.carry spelling left (run when the move is present).
  H11 decls     one declaration per function in its home header, @brief and every @param;
                the 17-parameter declaration is gone.
  H12 (HV1c)    each response site's printed "where" names the file of its prmRespond call
                (run when the citations moved).
--controls also runs the pre-registered negative controls (each mutation must fail).
"""
import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import nom_cpp as nc  # noqa: E402
from nom_hv1 import frozen_hv1 as fh  # noqa: E402
from nom_hv1 import rows_hv1 as rows  # noqa: E402
from nom_hv1 import texts_hv1 as tx  # noqa: E402

ROOT = HERE.parent
BIND_END = "// ---- End of the bindings ----"
RUN_ONLY_OK = {"run", "ep", "border", "sb", "i", "blocks"}
ALLOW = {"std", "static_cast", "isfinite", "const", "double", "return", "bool", "auto", "Eigen",
         "size_t", "true", "false"}


class Tree:
    """The working tree, with in-memory overrides (the negative controls)."""

    def __init__(self, over=None):
        self.over = over or {}
        self.cache = {}

    def read(self, rel):
        if rel in self.over:
            return self.over[rel]
        if rel not in self.cache:
            p = ROOT / rel
            self.cache[rel] = p.read_bytes().decode("utf-8").replace("\r\n", "\n") if p.exists() else None
        return self.cache[rel]


def src_of(home):
    d, b = home.split("/")
    return f"src/core/{d}/{b}.cpp"


def hdr_of(home):
    d, b = home.split("/")
    return f"include/{d}/{b}.hpp"


def find_def(text, name):
    """(lead comment lines, signature lines, body lines) of the definition of @p name."""
    L = text.split("\n")
    for j, l in enumerate(L):
        if l and not l[0].isspace() and re.search(r"\b" + re.escape(name) + r"\(", l) \
                and not l.startswith(("//", "/*", " *")):
            k = j
            while not L[k].rstrip().endswith(("{", ";")):
                k += 1
            if L[k].rstrip().endswith(";"):
                continue
            e = next(m for m in range(k + 1, len(L)) if L[m] == "}")
            a = j
            while a > 0 and L[a - 1].startswith("//"):
                a -= 1
            return L[a:j], L[j:k + 1], L[k + 1:e]
    return None


class Check:
    def __init__(self, tree, fz):
        self.t, self.fz = tree, fz
        self.fails = []
        self.notes = []
        self.rowmap = {r[0]: r for r in fz.table if r[1]}

    def fail(self, check, msg):
        self.fails.append((check, msg))

    # ---- the definitions ----
    def defn(self, name):
        row = self.rowmap[name]
        text = self.t.read(src_of(row[1]))
        d = find_def(text or "", name)
        if d is None:
            self.fail("H1", f"{name}: no definition in {src_of(row[1])}")
            return [], [], [], []
        lead, sig, body = d
        if BIND_END not in [b.strip() for b in body]:
            self.fail("H1", f"{name}: no end-of-bindings marker")
            return lead, sig, [], body
        k = [b.strip() for b in body].index(BIND_END)
        return lead, sig, body[:k], body[k + 1:]

    # ---- H1/H6: reconstruction ----
    def reverse(self, name, zcall=False):
        """The frozen-equivalent lines of function @p name: [(text, tag)]."""
        row = self.rowmap[name]
        f, home, shape, pieces, ex = row
        lead, sig, pre, body = self.defn(name)
        lines = [b for b in body]
        if f == "networkHarvestBorderBegin":
            if not lines or lines[0].strip() != "HarvestBorder border;" or lines[-1].strip() != "return border;":
                self.fail("H1", f"{f}: the declared inserts are not first and last")
            lines = lines[1:-1]
        if f == "networkHarvestStationBlock":
            n = len(tx.EPILOGUE)
            if not lines or lines[0].strip() != "StationHarvestBlock sb;" or lines[-n:] != tx.EPILOGUE:
                self.fail("H8", f"{f}: the declared insert or the layout epilogue differs")
            lines = lines[1:-n]
        # the closed substitution list, reversed (each new text exactly as often as declared)
        subs = list(ex.get("subs", {}).values())
        for old, new in set(subs):
            self.sub_class(f, old, new)
            want = subs.count((old, new))
            got = sum(l.count(new) for l in lines)
            if got != want:
                self.fail("H1", f"{f}: substitution {new!r} occurs {got} times, declared {want}")
            lines = [l.replace(new, old) for l in lines]
        if shape == "guard":
            k = next((j for j, l in enumerate(lines) if fh.is_code(l)), None)
            m = re.fullmatch(r"(\s*)if \(!(.*)\) return;", lines[k] if k is not None else "")
            if not m:
                self.fail("H1", f"{f}: the guard is not `if (!C) return;`")
            else:
                c = m.group(2)
                if c.startswith("(") and c.endswith(")") and self.balanced(c[1:-1]):
                    c = c[1:-1]
                lines[k] = f"{m.group(1)}if ({c}) {{"
                lines.append("}")
        elif shape == "unwrap":
            k = next(j for j, l in enumerate(lines) if fh.is_code(l))
            lines = lines[:k] + ["{"] + lines[k:] + ["}"]
        out = []
        calls = {c.strip(): fn for a, b, fn, c in ex.get("extract", [])}
        for l in lines:
            s = l.strip()
            if s in calls:
                out += self.reverse(calls[s], zcall="zold" in s)
            else:
                out.append((l, "line"))
        if f == "networkHarvestColumn":
            out = [(l, "line") for l in lead] + out
        if zcall:
            ren = {v: k for k, v in ex.get("merge", {}).get("rename", {}).items()} or \
                  {v: k for k, v in self.rowmap["networkResumeCouplingCensusRow"][4]["merge"]["rename"].items()}
            pat = re.compile(r"\b(" + "|".join(map(re.escape, ren)) + r")\b")
            out = [(pat.sub(lambda m: ren[m.group(1)], l), "twin") for l, _ in out]
        return out

    @staticmethod
    def balanced(s):
        d = 0
        for ch in s:
            d += ch == "("
            d -= ch == ")"
            if d < 0:
                return False
        return d == 0

    def sub_class(self, f, old, new):
        """H1/H7/H8: every substitution belongs to a declared class."""
        if (old, new) == rows.CALL_COL:
            return
        m = re.fullmatch(r"&?([irczb]blk)(_cols)?_of_sta\[i\]|([irc]blk)_row0_of_sta\[i\]", old)
        if m:
            kind = m.group(1) or m.group(3)
            seg = rows.LAYOUT_SEG.get(kind)
            if m.group(2):
                ok = new == "&blocks[i].cols"
            elif m.group(3):
                ok = new == f"blocks[i].rowOf(BlockSegment::{seg})"
            else:
                ok = new == f"blocks[i].segmentBlock(BlockSegment::{seg})"
            if not ok:
                self.fail("H8", f"{f}: layout read {old!r} -> {new!r} names another segment")
            return
        if (old, new) == ("continue;", "return sb;") and f == "networkHarvestStationBlock":
            return
        if f == "networkHarvestBorderBegin" and (old, new) in [
                ("std::map<Eigen::Index, CoupledShared> col2desc;",
                 "std::map<Eigen::Index, CoupledShared>& col2desc = border.col2desc;"),
                ("Eigen::MatrixXd cov_df;", "Eigen::MatrixXd& cov_df = border.cov_df;"),
                ("ClockCloneHarvest ccl_h;", "ClockCloneHarvest& ccl_h = border.ccl_h;"),
                ("SharedHistoryHarvest shx_h = ", "border.shx_h = ")]:
            return
        if f == "networkResumeCouplingCensusRow" and (old, new) == ("old->second.coupling", "restored.coupling"):
            return
        if f == "networkHarvestAmbiguityStation" and "birth_ep" in old + new or "admission age gate" in old + new:
            return      # D11, a comment edit (the reconstruction compares it as a comment line)
        fm = re.fullmatch(r"ctx\.carry\.(\w+)", old)
        if fm and fm.group(1) in rows.FIELD_MOVE and new == f"ctx.counters.{fm.group(1)}":
            return
        self.fail("H1", f"{f}: undeclared substitution class {old!r} -> {new!r}")

    def orchestrator(self):
        lead, sig, pre, body = self.defn(rows.FUNC_NAME)
        out, seen = [], []
        inserts = set(rows.ORCH_INSERTS)
        for l in body:
            s = l.strip()
            if s not in inserts:
                out.append((l, "line"))
                continue
            seen.append(s)
            if s == "HarvestBorder border = networkHarvestBorderBegin(run, ep);":
                out += self.reverse("networkHarvestBorderBegin") + self.reverse("networkHarvestColumn")
            elif s == "blocks[i] = networkHarvestStationBlock(run, ep, border, i);":
                out += self.reverse("networkHarvestStationBlock")
            elif s.startswith("network"):
                out += self.reverse(s.split("(")[0])
        out.append(("}", "line"))          # the function's close (a frozen line)
        if seen != rows.ORCH_INSERTS:
            self.fail("H6", "the orchestrator's calls differ from the declared order")
        return out

    def h1_h6(self):
        fz = self.fz
        drops = {n for r in fz.table for (a, b) in r[4].get("drops", {}) for n in range(a, b + 1)}
        (aa, ab), = [k for k, v in fz.replaced.items() if v == "alias"]
        ZW = self.rowmap["networkResumeCouplingCensusRow"][4]["merge"]["twin"]
        frozen = [(fz.L[n - 1], n) for n in range(ab + 1, fz.func[1] + 1)
                  if n not in drops and fz.L[n - 1].strip()]
        rec = [(l, t) for l, t in self.orchestrator() if l.strip()]
        i = j = 0
        while i < len(rec) and j < len(frozen):
            if rec[i][1] == "twin":
                a = i
                while i < len(rec) and rec[i][1] == "twin":
                    i += 1
                b = j
                while j < len(frozen) and ZW[0] <= frozen[j][1] <= ZW[1]:
                    j += 1
                ta = [t.text for t in nc.lex("\n".join(l for l, _ in rec[a:i]))]
                tb = [t.text for t in nc.lex("\n".join(l for l, _ in frozen[b:j]))]
                if ta != tb or b == j:
                    self.fail("H9", f"the Z census call does not reproduce frozen {ZW}")
                continue
            if rec[i][0].strip() != frozen[j][0].strip():
                self.fail("H1", f"frozen line {frozen[j][1]} {frozen[j][0].strip()[:60]!r} "
                                f"!= {rec[i][0].strip()[:60]!r}")
                return
            i += 1
            j += 1
        if i != len(rec) or j != len(frozen):
            self.fail("H1", f"length: reconstructed {len(rec)} lines, frozen {len(frozen)}")
        self.vocab(rec, frozen)
        self.notes.append(f"H1/H6: {len(frozen)} frozen non-blank lines reproduced in order")

    def vocab(self, rec, frozen):
        pat = re.compile(r"(\b(?:log|lg)->\w+|spdlog::\w+|prmRespond\(|\bemit\w*\(|"
                         r"network\w+Note\w*\(|traces\.\w+\(|\.push_back\(|\.erase\()")
        a = [m.group(0) for l, _ in rec for m in pat.finditer(l)]
        b = [m.group(0) for l, _ in frozen for m in pat.finditer(l)]
        if a != b:
            self.fail("H6", "the ordered log/event/write vocabulary differs")
        self.notes.append(f"H6: {len(b)} ordered log calls, PRM responses, event and container writes equal")

    # ---- H3/H4: bindings ----
    def call_bindings(self):
        """The frozen call's argument per parameter, rooted at run/ep through networkLeveling."""
        text = fh.git_show(self.fz.rev, "src/core/filter/NetworkLeveling.cpp")
        env = {}
        for m in re.finditer(r"^\s+(?:const\s+)?[\w:<>,\s&*]+?[&*]?\s*(\w+)\s*=\s*([^;]+);", text, re.M):
            env[m.group(1)] = " ".join(m.group(2).split())
        call = re.search(rows.FUNC_NAME + r"\(([^;]*)\);", text).group(1)
        args = [" ".join(a.split()) for a in call.split(",")]
        if len(args) != len(self.fz.params):
            self.fail("H3", "the frozen call's argument count differs from the parameters")
        out = {}
        for p, a in zip(self.fz.params, args):
            for _ in range(5):
                a = re.sub(r"^(\w+)", lambda m: env.get(m.group(1), m.group(1)), a)
            out[p.name] = (p.text.strip(), a)
        return out

    def h3_h4(self, aliases):
        cb = self.call_bindings()
        alias_lines = {n: [x for x in t] for n, t in aliases}
        alias_text = set(x.rstrip() for _, t in aliases for x in t)
        fz = self.fz
        sc = nc.Scopes(fz.T, fz.open, fz.close, fz.params)
        nbind = 0
        for f, row in self.rowmap.items():
            lead, sig, pre, body = self.defn(f)
            kept = row[4].get("kept", ())
            kept_names = {re.split(r"[\s*&]+", k.strip())[-1] for k in kept} | {"run", "ep"}
            bound = set()
            k = 0
            pre = [p for p in pre if not p.strip().startswith("// ---- From") and
                   not p.strip().startswith("//      scripts/nom_hv1_cut.py")]
            while k < len(pre):
                line = pre[k].rstrip()
                if line.strip().startswith("//"):
                    if line not in alias_text:
                        self.fail("H3", f"{f}: comment line not from the alias block: {line.strip()[:50]!r}")
                    k += 1
                    continue
                m = re.match(r"\s*(.*?)\s*\b(\w+)\s*=\s*(.*);(\s*//.*)?$", line)
                m0 = re.match(r"\s*(?:const\s+)?[\w:<>]+\s*&?\s+(\w+)\s*=", line)
                name = m.group(2) if m else (m0.group(1) if m0 else None)
                if name in alias_lines and line == [x for x in alias_lines[name] if not x.strip().startswith("//")][0]:
                    code = [x for x in alias_lines[name] if not x.strip().startswith("//")]
                    if pre[k:k + len(code)] != code:
                        self.fail("H3", f"{f}: alias {name} not verbatim")
                    rhs = " ".join(c for c in code)[len(code[0].split("=")[0]) + 1:]
                    k += len(code)
                elif m and name in cb:
                    decl, rhs = m.group(1) + " " + name, m.group(3)
                    fdecl, frhs = cb[name]
                    if decl != fdecl and decl != "const " + fdecl:
                        self.fail("H3", f"{f}: {name} declared {decl!r}, frozen {fdecl!r}")
                    want = frhs if not frhs.endswith(".get()") else frhs
                    if rhs != want:
                        self.fail("H3", f"{f}: {name} bound to {rhs!r}, the frozen call passed {frhs!r}")
                    if row[4].get("run_only") and rhs.startswith("ep."):
                        self.fail("H3", f"{f}: a run-only function binds the epoch")
                    k += 1
                elif line.strip() in rows.BORDER_BIND.values() and "border" in kept_names:
                    rhs = line.split("=")[1]
                    k += 1
                elif line.strip() in rows.LAYOUT_BIND.values() and "sb" in kept_names:
                    rhs = line.split("=", 1)[1]
                    if name in rows.RE_EVALUATED:
                        sb_row = self.rowmap["networkHarvestStationBlock"]
                        a, b, _ = sb_row[3][0]
                        if line.strip() not in [x.strip() for x in fz.L[a - 1:b]]:
                            self.fail("H5", f"{f}: re-evaluated {name} is not the loop head's text")
                    k += 1
                else:
                    self.fail("H3", f"{f}: undeclared binding {line.strip()[:70]!r}")
                    k += 1
                    continue
                # H4: an initializer that reads another binding's name reads it bound above
                tk = nc.lex(rhs)
                rd = {t.text for q, t in enumerate(tk) if t.kind == "id"
                      and not (q > 0 and tk[q - 1].text in (".", "->", "::"))}
                names = {n for n, _ in aliases} | {p[0] for p in rows.PARAMS} | set(rows.LAYOUT_BIND)
                missing = {x for x in rd if x in names and x not in bound and x not in kept_names}
                if missing:
                    self.fail("H4", f"{f}: {name} reads {sorted(missing)} before they are bound")
                bound.add(name)
                nbind += 1
            # every name the body reads that the frozen function declared outside the body
            self.free_names(f, row, sc, bound | kept_names)
        self.notes.append(f"H3/H4: {nbind} binding lines checked")

    def free_names(self, f, row, sc, bound):
        fz, T = self.fz, self.fz.T
        if f == rows.FUNC_NAME:
            return
        drops = {n for (a, b) in row[4].get("drops", {}) for n in range(a, b + 1)}
        ext = {n for a, b, _, _ in row[4].get("extract", []) for n in range(a, b + 1)}
        own = {n for a, b, _ in row[3] for n in range(a, b + 1)} - drops - ext
        # names a declared substitution replaces (layout reads, the lambda, the census's
        # restored entry) are not read by the new body
        subbed = {}
        for n, (old, new) in row[4].get("subs", {}).items():
            subbed.setdefault(n, set()).update(t.text for t in nc.lex(old) if t.kind == "id")
        if f == "networkHarvestColumn":
            own |= {n for (a, b) in row[4]["drops"] for n in range(a, b + 1)}
        for i in range(T.n):
            ln = T.line(i)
            if ln not in own or T.k(i) != "id" or (i > 0 and T.t(i - 1) in (".", "->", "::")):
                continue
            d = sc.lookup(T.t(i), i)
            if d is None or d.line in own or T.t(i) in subbed.get(ln, ()):
                continue
            if T.t(i) not in bound:
                self.fail("H3", f"{f}: reads {T.t(i)} (frozen line {d.line}) with no binding")

    # ---- H5, H7, H9, H11, H10, H12 ----
    def h5(self):
        fz = self.fz
        body = "\n".join(fz.L[fz.func[0] - 1:fz.func[1]])
        w = re.findall(r"pl_prev_now_s\s*=[^=]", body)
        z = re.findall(r"(?:--|\+\+)\s*ctx\.carry\.zwd_seam_ep_left|zwd_seam_ep_left\s*(?:[-+]?=[^=])", body)
        n = re.findall(r"n_loop_epochs\s*(?:[-+]?=[^=]|\+\+|--)", body)
        if len(w) != 1 or len(z) != 1 or n:
            self.fail("H5", f"writers: pl_prev_now_s {len(w)}, zwd_seam_ep_left {len(z)}, n_loop_epochs {len(n)}")
        # the one stamp follows the station loop in the orchestrator; the readers of the
        # run-state aliases are the ambiguity station (before it) and the ZWD functions (the
        # decrement is inside the seam step, after its own evaluation)
        for f, names in (("networkHarvestAmbiguityStation", ("amb_state_dt_ep_s",)),
                         ("networkZwdSeamWindowStep", ("zwd_seam_live",))):
            pre = self.defn(f)[2]
            for nm in names:
                if not any(nm in p for p in pre):
                    self.fail("H5", f"{f}: {nm} not bound at entry")
        for f in self.rowmap:
            if f in ("networkHarvestAmbiguityStation",):
                continue
            pre = "\n".join(self.defn(f)[2])
            if "amb_state_dt_ep_s" in pre:
                self.fail("H5", f"{f} evaluates amb_state_dt_ep_s after the stamp")
        self.notes.append("H5: one stamp, one decrement, no n_loop_epochs writer; readers placed")

    def h7(self):
        fz, T = self.fz, self.fz.T
        row = self.rowmap["networkHarvestStationBlock"]
        loop = row[3][0][0] - 1
        if not fh.code_text(fz.L[loop - 1]).startswith("for (size_t i = 0; i < stations.size(); ++i)"):
            self.fail("H7", "the station loop does not precede the station block")
        o = next(i for i in range(T.n) if T.line(i) == loop and T.t(i) == "{")
        tops = nc.statements(T, o + 1, T.close(o))
        n_ok = 0
        for n, (old, new) in row[4]["subs"].items():
            if old != "continue;":
                continue
            c = next((i for i in range(T.n) if T.line(i) == n and T.t(i) == "continue"), None)
            st = next(((s, e) for s, e in tops if s <= c <= e), None) if c is not None else None
            inner = st and any(T.t(j) in ("for", "while", "do") for j in range(st[0], c))
            if st is None or inner:
                self.fail("H7", f"frozen line {n}: `continue` not at the station loop's scope")
            else:
                n_ok += 1
        self.notes.append(f"H7: {n_ok} continue-to-return statements at the station loop's scope")

    def h9(self):
        fz = self.fz
        r = self.rowmap["networkResumeCouplingCensusRow"]
        (a, b, _), = r[3]
        za, zb = r[4]["merge"]["twin"]
        ren = r[4]["merge"]["rename"]
        tl = [t.text for t in nc.lex("\n".join(fz.L[a - 1:b]))]
        tz = [ren.get(t.text, t.text) for t in nc.lex("\n".join(fz.L[za - 1:zb]))]
        if tl != tz:
            self.fail("H9", "the frozen L and Z census bodies differ under the renaming")
        self.notes.append(f"H9: L {a}-{b} and Z {za}-{zb} token-identical under the renaming ({len(tl)} tokens)")

    def h11(self):
        decls = 0
        for f, row in self.rowmap.items():
            run_only = row[4].get("run_only")
            hdr = "include/app/NetworkRunContext.hpp" if run_only else hdr_of(row[1])
            text = self.t.read(hdr) or ""
            hits = [m.start() for m in re.finditer(r"\b" + re.escape(f) + r"\(", text)]
            dl = [h for h in hits if text[h:text.find(";", h) + 1].count("{") == 0 and
                  text.rfind("\n", 0, h) >= 0 and not text[text.rfind("\n", 0, h):h].strip().startswith(("//", "*", "///"))]
            if len(dl) != 1:
                self.fail("H11", f"{f}: {len(dl)} declarations in {hdr}")
                continue
            h = dl[0]
            close = text.find(";", h)
            params = re.findall(r"(\w+)\s*[,)]", text[h:close + 1].split("(", 1)[1])
            doc = text[text.rfind("*/", 0, h) - 2000:h] if "*/" in text[max(0, h - 300):h] else \
                text[max(0, h - 1500):h]
            if "@brief" not in doc:
                self.fail("H11", f"{f}: no @brief")
            for p in params:
                if f"@param {p}" not in doc:
                    self.fail("H11", f"{f}: no @param {p}")
            decls += 1
        ctxh = self.t.read("include/app/NetworkRunContext.hpp")
        if re.search(rows.FUNC_NAME + r"\(NetworkRunContext&", ctxh):
            self.fail("H11", "the 17-parameter declaration remains")
        cm = self.t.read("CMakeLists.txt")
        if "tests/test_network_harvest_layout.cpp" not in cm:
            self.fail("H11", "the layout test is not in CMakeLists.txt")
        if tx.SPANS_DEF not in self.t.read(src_of(rows.SPINE)):
            self.fail("H8", "harvestBlockSpans is not the declared definition")
        self.notes.append(f"H11: {decls} declarations, each with @brief and every @param")
        # H8: a kind that reads the station layout names its own segment only
        own_seg = {"networkHarvestSlantIonoBlock": "Iono", "networkHarvestRxClock": "RxClock",
                   "networkHarvestRxCbBlock": "RxCodeBias", "networkHarvestZwd": "Zwd"}
        for f, seg in own_seg.items():
            body = "\n".join(self.defn(f)[3])
            other = set(re.findall(r"BlockSegment::(\w+)", body)) - {seg}
            if other:
                self.fail("H8", f"{f} reads the segment(s) {sorted(other)}")

    def h10(self):
        ctxh = self.t.read("include/app/NetworkRunContext.hpp")
        if "struct NetworkServiceCounters" not in ctxh:
            return
        a = ctxh.index("struct NetworkServiceCounters")
        o = ctxh.index("{", a)
        d, e = 0, o
        for e in range(o, len(ctxh)):
            d += (ctxh[e] == "{") - (ctxh[e] == "}")
            if d == 0:
                break
        sc = ctxh[a:e]
        moved = [f for f in rows.FIELD_MOVE if re.search(r"\b" + f + r"\b", sc)]
        if not moved:
            self.notes.append("H10: not applied (HV1b pending)")
            return
        base = fh.git_show(self.fz.rev, "include/app/NetworkRunContext.hpp")
        for f in rows.FIELD_MOVE:
            bl = [l.strip() for l in base.split("\n") if re.search(r"\b" + f + r"\b", l) and
                  not l.strip().startswith("//")]
            nl = [l.strip() for l in sc.split("\n") if re.search(r"\b" + f + r"\b", l) and
                  not l.strip().startswith("//")]
            if sorted(bl) != sorted(nl):
                self.fail("H10", f"{f}: declarator or initializer differs ({nl} vs {bl})")
        import subprocess
        g = subprocess.run(["git", "-C", str(ROOT), "grep", "-n", "-E",
                            r"carry\.(" + "|".join(rows.FIELD_MOVE) + r")\b", "--", "src", "include",
                            "tests"], capture_output=True, text=True).stdout
        over = [r for r in self.t.over if re.search(r"carry\.(" + "|".join(rows.FIELD_MOVE) + r")\b",
                                                   self.t.over[r] or "")]
        if g.strip() or over:
            self.fail("H10", "a ctx.carry spelling of a moved field remains")
        self.notes.append("H10: four rx-CB run fields moved, declarators equal, no carry spelling left")

    def h12(self):
        text = self.t.read("src/core/filter/NetworkResponses.cpp")
        if "NetworkLocalCarryHarvest.cpp" in text:
            self.notes.append("H12: not applied (HV1c pending)")
            return
        for site, file in rows.CITATIONS.items():
            m = re.search(r"\{R::" + site + r",[^{}]*?\"([^\"]+)\"\s*,\s*\"[^\"]*\"\s*\}", text, re.S)
            where = m.group(1) if m else ""
            if not where.startswith(file):
                self.fail("H12", f"{site}: printed where {where!r}, expected {file}")
                continue
            home = next(r[1] for r in self.fz.table if r[1] and r[1].endswith(file[:-4]))
            if f"ResponseSite::{site}" not in (self.t.read(src_of(home)) or ""):
                self.fail("H12", f"{site}: {file} holds no prmRespond of the site")
        self.notes.append(f"H12: {len(rows.CITATIONS)} printed citations name their site's file")

    def run_all(self, aliases):
        total, counts, fails = self.fz.coverage()
        for w, m in fails:
            self.fail("H2", f"{w}: {m}")
        self.notes.append(f"H2: {total} code lines claimed once; {dict(sorted(counts.items()))}")
        for step in (self.h1_h6, lambda: self.h3_h4(aliases), self.h5, self.h7, self.h9,
                     self.h11, self.h10, self.h12):
            try:
                step()
            except Exception as e:  # a malformed tree is a failure, not a crash
                self.fail("H*", f"{getattr(step, '__name__', 'check')}: {type(e).__name__}: {e}")
        return self.fails


def aliases_of(fz):
    sys.argv_backup = None
    import nom_hv1_cut as cut
    return cut.alias_statements(fz)


def run(over=None, verbose=True, fz=None):
    fz = fz or fh.load()
    c = Check(Tree(over), fz)
    fails = c.run_all(aliases_of(fz))
    if verbose:
        for n in c.notes:
            print("ok  ", n)
        for k, m in fails:
            print("FAIL", k, m)
    return fails


# ---- negative controls ------------------------------------------------------------------------
def controls():
    t = Tree()

    def mut(rel, old, new, count=1):
        text = t.read(rel)
        if text.count(old) < 1:
            raise SystemExit(f"control anchor missing in {rel}: {old[:60]!r}")
        return {rel: text.replace(old, new, count)}

    S, LV = "src/core/app/NetworkLocalCarryHarvest.cpp", "src/core/filter/NetworkLevelingCarry.cpp"
    Z, IO = "src/core/filter/NetworkZwdCarry.cpp", "src/core/filter/NetworkSlantIonoCarry.cpp"
    CH, RS = "src/core/filter/NetworkChainDatum.cpp", "src/core/filter/NetworkResponses.cpp"
    CTX, RC = "include/app/NetworkRunContext.hpp", "src/core/filter/NetworkRxCodeBiasCarry.cpp"
    cases = [
        ("C1 receiver clock before the ionosphere", "H6",
         mut(S, "        networkHarvestSlantIonoBlock(run, ep, border, blocks);\n        networkHarvestRxClock(run, ep, border, blocks);",
             "        networkHarvestRxClock(run, ep, border, blocks);\n        networkHarvestSlantIonoBlock(run, ep, border, blocks);")),
        ("C3 ionosphere reads the receiver-clock segment", "H8",
         mut(IO, "blocks[i].segmentBlock(BlockSegment::Iono)", "blocks[i].segmentBlock(BlockSegment::RxClock)")),
        ("C4 stamp before the station loop", "H1",
         mut(S, "        ctx.carry.pl_prev_now_s = pl_now_s;\n", "")
         | {}),
        ("C5 a per-ambiguity continue turned into return", "H1",
         mut(LV, "if (!std::isfinite(value_abs)) continue;", "if (!std::isfinite(value_abs)) return;")),
        ("C6 x_s bound as a copy", "H3",
         mut(LV, "const Eigen::VectorXd& x_s = ep.solve.x_s;", "const Eigen::VectorXd x_s = ep.solve.x_s;")),
        ("C7 pl_now_s bound to ep.clock.dt", "H3",
         mut(Z, "double pl_now_s = ep.clock.pl_now_s;", "double pl_now_s = ep.clock.dt;")),
        ("C9 the ZWD prune's PRM response dropped", "H1",
         mut(Z, "prmRespond(ctx.counters.prm_resp, ResponseSite::ZwdGapPrune);", ";")),
        ("C10 a guard's negation lost", "H1",
         mut(CH, "if (!(eq34_rpb_q > 0.0)) return;", "if (eq34_rpb_q > 0.0) return;")),
        ("C11 an alias line edited", "H3",
         mut(LV, "const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;", "const double pl_max_gap_s = ctx.knobs.pl_q_amb;")),
        ("C12 the Z call passes the L entry", "H1",
         mut(Z, "networkResumeCouplingCensusRow(run, zold->second, coupling);",
             "networkResumeCouplingCensusRow(run, old->second, coupling);")),
        ("C15 the column without its regression declaration", "H1",
         mut(S, "    NetworkNormalEquations::StationRegression reg;\n", "")),
        ("C18 a binding line removed", "H3",
         mut(IO, "    const std::vector<int>& est_index_of = ep.assembly.est_index_of;\n", "")),
    ]
    # C4 needs the stamp re-inserted before the loop
    c4 = cases[2][2][S]
    cases[2] = (cases[2][0], cases[2][1], {S: c4.replace(
        "        for (size_t i = 0; i < stations.size(); ++i) {\n            blocks[i]",
        "        ctx.carry.pl_prev_now_s = pl_now_s;\n        for (size_t i = 0; i < stations.size(); ++i) {\n            blocks[i]")})
    if "long long rx_cb_injected_total = 0, rx_cb_innov_skips_total = 0," in t.read(CTX) and \
            "ctx.counters.rx_cb_injected_total" in t.read(RC):
        cases.append(("C13 a moved counter with another initializer", "H10",
                      mut(CTX, "long long rx_cb_injected_total = 0, rx_cb_innov_skips_total = 0,",
                          "long long rx_cb_injected_total = 1, rx_cb_innov_skips_total = 0,")))
    if "NetworkLocalCarryHarvest.cpp" not in t.read(RS):
        cases.append(("C14 a citation naming the wrong file", "H12",
                      mut(RS, '"NetworkZwdCarry.cpp harvest prune"', '"NetworkRxClockCarry.cpp harvest prune"')))
    fz = fh.load()
    ok = True
    for name, want, over in cases:
        fails = run(over, verbose=False, fz=fz)
        kinds = {k for k, _ in fails}
        hit = want in kinds
        ok &= hit
        print(f"{'ok  ' if hit else 'MISS'} control {name}: "
              f"{'fails ' + want if hit else 'did not fail ' + want} {sorted(kinds)}")
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--controls", action="store_true")
    args = ap.parse_args(argv)
    fails = run()
    rc = 1 if fails else 0
    if args.controls:
        rc = rc or (0 if controls() else 1)
    print("nom_hv1_verify:", "PASS" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
