#!/usr/bin/env python3
"""Relocation verifier for card UOM step U4 (gate G-UR): MeasurementModel split by concern.

    python scripts/uom_u4_verify.py [--verbose]

The frozen source is src/core/measurement/MeasurementModel.cpp and
include/measurement/MeasurementModel.hpp at 5154324 (card UOM pre-registered), read with
`git show` (no copy is stored).  The manifest below records, for every unit of the new files,
the frozen lines it carries and every declared edit.  Exit 0 only when all three checks pass:

  1. Units.  Each unit is located in its new file: a "block" by the first code line of its
     frozen text (the run of code lines from there must equal the unit), a "body" by its
     definition (the code lines between its braces must equal the unit).  Pieces:
       F(a, b, shift)  frozen lines a..b, character for character after the declared
                       substitutions and drops, with `shift` leading spaces removed;
       G(if, close, line)  a guard: the frozen `if (COND) {` ... `}` becomes `line`, which must
                       be `if (!COND) return;` (or the negation of a negated name), and the
                       interior moves one level out (shift 4);
       W(a, b, n)      frozen lines a..b after substitution equal the next n new lines with
                       all whitespace runs collapsed (a declaration rejoined on one line);
       H(a, b, shift)  as F, from the frozen header;
       N(text)         declared new code (an alias preamble, a call, a return), compared
                       without surrounding space.
     Comments and blank lines are ignored on both sides (rt_seam_verify.strip_comments); a
     body unit with `sig` also compares its signature lines and closing brace.
  2. Coverage.  Every code line of the frozen MeasurementModel.cpp is carried by exactly one
     unit, or is a declared drop (with its reason), or is file furniture (the includes).
  3. Header.  The frozen header's code lines appear in the new header in the same order,
     apart from the declared header edits, and the data members are the same names in the
     same order (their order is the construction order).
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

FROZEN = "5154324"
CPP = "src/core/measurement/MeasurementModel.cpp"
HPP = "include/measurement/MeasurementModel.hpp"
D = "src/core/measurement/"
MM, ST, RW, VA, DD, NW = (D + "MeasurementModel.cpp", D + "MeasurementModelStates.cpp",
                          D + "MeasurementModelRow.cpp", D + "MeasurementModelVariance.cpp",
                          D + "MeasurementModelDd.cpp", D + "MeasurementModelNetwork.cpp")


def F(a, b=None, shift=0):
    return ("F", a, a if b is None else b, shift)


def H(a, b=None, shift=0):
    return ("H", a, a if b is None else b, shift)


def G(if_line, close_line, line):
    return ("G", if_line, close_line, line)


def W(a, b, n):
    return ("W", a, b, n)


def N(text):
    return ("N", text)


RET_FALSE = ("return em;", "return false;")
NEED = "a field of RequiredStates (the pre-pass's working value)"
DDB = "a field of DdBuild (the DD build's working value)"
EXIT = "the single exit in buildDdMeasurement resets the flag"
BLOCK = "a bare block's brace: the stage is the scope"

# Frozen MeasurementModel.cpp line -> [(from, to)] (plain text replace), applied before the shift.
SUBS = {
    298: [("const bool is_uncombined", "need.is_uncombined")],
    299: [("const bool nlwl", "need.nlwl")],
    306: [("const bool pc_on", "need.pc_on")],
    506: [("const bool bssd_active", "need.bssd_active")],
    820: [("(required_ambiguities,", "(need.required_ambiguities,")],
    **{n: [("continue;", "return false;")] for n in (1947, 1953, 1959, 1965, 1971)},
    1984: [("update.R_v(row) = ", "double variance = ")],
    **{n: [("update.R_v(row)", "variance")] for n in (2135, 2165, 2172, 2190)},
    2417: [("auto klog =", "dd.klog =")],
    2422: [("const bool diag =", "dd.diag =")],
    2439: [("{ first_dd_call = false; return em; }", "return;")],
    2497: [("struct SharedInfo {", "struct MeasurementModel::DdSharedSat {")],
    2515: [("SharedInfo info;", "DdSharedSat info;")],
    2589: [("const std::map<std::string, std::string> pivots =", "pivots =")],
    2625: [("struct DdRow {", "struct MeasurementModel::DdRow {")],
    **{n: [RET_FALSE] for n in (2573, 2600, 2861, 2991, 2995, 3018, 3028)},
    **{n: [("return em;", "return;")] for n in (3089, 3093)},
}
DROPS = {
    302: NEED, 303: NEED, 307: NEED, 314: NEED, 521: NEED,
    1857: "epoch_tag: computed and never read (step U4a)",
    2245: "epoch_tag: computed and never read (step U4a)",
    2452: DDB, 2504: DDB, 2646: DDB, 2997: DDB,
    **{n: EXIT for n in (2572, 2599, 2860, 2990, 2994, 3017, 3027, 3088, 3092)},
    2660: BLOCK, 2716: BLOCK, 2887: BLOCK, 2938: BLOCK,
}
FURNITURE = {n: "includes: each new file lists its own" for n in range(23, 39)}

# Frozen header line -> [(from, to)]; dropped header lines; header lines carried by units.
HSUBS = {
    138: [("AbsorbedReason reason) {", "AbsorbedReason reason);")],
    419: [(") const {", ") const;")],
    511: [("mutable std::map", "std::map")],
}
HDROPS = {
    6: "the second #pragma once (step U4a)",
    389: "getNlWlReferenceBands: no caller (step U4a)", 390: "getNlWlReferenceBands",
    391: "getNlWlReferenceBands",
    140: "the inline body's brace: defined in MeasurementModelNetwork.cpp",
    424: "the inline body's brace: defined in MeasurementModelNetwork.cpp",
}

ST_PRE = {
    "is_uncombined": "const bool is_uncombined = need.is_uncombined;",
    "nlwl": "const bool nlwl = need.nlwl;",
    "pc_on": "const bool pc_on = need.pc_on;",
    "bssd": "const bool bssd_active = need.bssd_active;",
}
DD_LOG = [N("const auto& klog = dd.klog;"), N("const bool diag = dd.diag;")]

UNITS = [
    # --- MeasurementModel.cpp ---
    dict(name="MeasurementModel.cpp head (namespace, modeLabel, construction, setters, aiding)",
         file=MM, kind="block", pieces=[F(40, 51), F(74, 103), F(122, 170)]),
    dict(name="hasValidGeometry (the row loop's geometry gate)", file=MM, kind="body",
         find=r"MeasurementModel::hasValidGeometry\s*\(",
         pieces=[F(1942, 1972, 4), N("return true;")]),
    dict(name="computeMeasurementUpdate (single receiver)", file=MM, kind="block",
         pieces=[F(1850, 1941), N("if (!hasValidGeometry(obs, epoch_prefix)) continue;"),
                 F(1973, 1983), N("update.R_v(row) = rowVariance(obs, ref_type);"),
                 F(2192, 2238)]),
    dict(name="validateMeasurementUpdate, BSSD forwarders, namespace close", file=MM,
         kind="block", pieces=[F(2244, 2332), F(3100)]),
    # --- MeasurementModelStates.cpp ---
    dict(name="substitutedAmbStdOverride", file=ST, kind="block", pieces=[F(59, 72)]),
    dict(name="ensureRequiredStates (driver)", file=ST, kind="body",
         find=r"MeasurementModel::ensureRequiredStates\s*\(", sig=(296, 297), close=1206,
         pieces=[N("RequiredStates need;"), W(298, 298, 1), W(299, 300, 1), W(306, 306, 1),
                 W(506, 506, 1),
                 N("collectRequiredStates(observations, need);"),
                 N("addHoldProtectedKeys(need);"),
                 N("collectNlWlKeys(need);"),
                 N("ensureIonosphereStates(observations, epoch_prefix, need);"),
                 N("ensurePhaseColourStates(need);"),
                 N("preseedColdReceiverClock(observations, epoch_prefix);"),
                 N("computePivotSeeds(observations, need);"),
                 N("ensurePerBandAmbiguities(observations, epoch_prefix, need);"),
                 N("ensureNlWlAmbiguities(observations, epoch_prefix, need);"),
                 F(815, 821),
                 N("warnAmbiguityBandCount(epoch_prefix, need);"),
                 N("markUnresolvableAmbiguities(observations, epoch_prefix, need);"),
                 N("ensureReceiverCodeBiasStates(observations, epoch_prefix);"),
                 N("ensureSatCodeBiasStates(observations, epoch_prefix, need);")]),
    dict(name="collectRequiredStates", file=ST, kind="body",
         find=r"MeasurementModel::collectRequiredStates\s*\(",
         pieces=[N(ST_PRE["is_uncombined"]), N(ST_PRE["nlwl"]), N(ST_PRE["pc_on"]),
                 N("auto& required_ambiguities = need.required_ambiguities;"),
                 N("auto& required_iono_sats = need.required_iono_sats;"),
                 N("auto& required_pc_sats = need.required_pc_sats;"),
                 N("auto& nlwl_phase_signals = need.nlwl_phase_signals;"),
                 F(316, 365)]),
    dict(name="addHoldProtectedKeys", file=ST, kind="body",
         find=r"MeasurementModel::addHoldProtectedKeys\s*\(",
         pieces=[N(ST_PRE["is_uncombined"]),
                 N("auto& required_ambiguities = need.required_ambiguities;"),
                 N("auto& required_iono_sats = need.required_iono_sats;"),
                 F(367, 383)]),
    dict(name="collectNlWlKeys", file=ST, kind="body",
         find=r"MeasurementModel::collectNlWlKeys\s*\(",
         pieces=[N(ST_PRE["nlwl"]),
                 N("const auto& nlwl_phase_signals = need.nlwl_phase_signals;"),
                 N("auto& required_ambiguities = need.required_ambiguities;"),
                 F(385, 391), G(392, 403, "if (!nlwl) return;")]),
    dict(name="ensureIonosphereStates", file=ST, kind="body",
         find=r"MeasurementModel::ensureIonosphereStates\s*\(",
         pieces=[N(ST_PRE["is_uncombined"]),
                 N("const auto& required_iono_sats = need.required_iono_sats;"),
                 G(406, 469, "if (!is_uncombined) return;")]),
    dict(name="ensurePhaseColourStates", file=ST, kind="body",
         find=r"MeasurementModel::ensurePhaseColourStates\s*\(",
         pieces=[N(ST_PRE["pc_on"]),
                 N("const auto& required_pc_sats = need.required_pc_sats;"),
                 G(476, 487, "if (!pc_on) return;")]),
    dict(name="preseedColdReceiverClock", file=ST, kind="body",
         find=r"MeasurementModel::preseedColdReceiverClock\s*\(", pieces=[F(489, 503)]),
    dict(name="computePivotSeeds", file=ST, kind="body",
         find=r"MeasurementModel::computePivotSeeds\s*\(",
         pieces=[N(ST_PRE["nlwl"]), N(ST_PRE["bssd"]),
                 N("const auto& nlwl_phase_signals = need.nlwl_phase_signals;"),
                 N("auto& pivot_abs_seeds = need.pivot_abs_seeds;"),
                 F(507, 583)]),
    dict(name="ensurePerBandAmbiguities", file=ST, kind="body",
         find=r"MeasurementModel::ensurePerBandAmbiguities\s*\(",
         pieces=[N(ST_PRE["nlwl"]), N(ST_PRE["bssd"]),
                 N("const auto& required_ambiguities = need.required_ambiguities;"),
                 N("const auto& pivot_abs_seeds = need.pivot_abs_seeds;"),
                 G(586, 681, "if (nlwl) return;")]),
    dict(name="ensureNlWlAmbiguities", file=ST, kind="body",
         find=r"MeasurementModel::ensureNlWlAmbiguities\s*\(",
         pieces=[N(ST_PRE["nlwl"]), N(ST_PRE["bssd"]),
                 N("const auto& nlwl_phase_signals = need.nlwl_phase_signals;"),
                 N("const auto& required_ambiguities = need.required_ambiguities;"),
                 N("const auto& pivot_abs_seeds = need.pivot_abs_seeds;"),
                 G(689, 813, "if (!nlwl) return;")]),
    dict(name="warnAmbiguityBandCount", file=ST, kind="body",
         find=r"MeasurementModel::warnAmbiguityBandCount\s*\(",
         pieces=[N(ST_PRE["nlwl"]),
                 N("const auto& required_ambiguities = need.required_ambiguities;"),
                 G(832, 867, "if (nlwl) return;")]),
    dict(name="markUnresolvableAmbiguities", file=ST, kind="body",
         find=r"MeasurementModel::markUnresolvableAmbiguities\s*\(",
         pieces=[N(ST_PRE["is_uncombined"]), N(ST_PRE["nlwl"]),
                 N("const auto& required_ambiguities = need.required_ambiguities;"),
                 F(869, 883), G(884, 940, "if (nlwl) return;")]),
    dict(name="ensureReceiverCodeBiasStates", file=ST, kind="body",
         find=r"MeasurementModel::ensureReceiverCodeBiasStates\s*\(", pieces=[F(942, 978)]),
    dict(name="ensureSatCodeBiasStates", file=ST, kind="body",
         find=r"MeasurementModel::ensureSatCodeBiasStates\s*\(",
         pieces=[N(ST_PRE["is_uncombined"]), F(980, 1205)]),
    # --- MeasurementModelRow.cpp ---
    dict(name="coverage checks (ifCombHasFullBia .. ifCombHasFullRxAtx)", file=RW,
         kind="block", pieces=[F(176, 290)]),
    dict(name="isUsableMeasurement, buildJacobianRow", file=RW, kind="block",
         pieces=[F(1391, 1846)]),
    # --- MeasurementModelVariance.cpp ---
    dict(name="rowVariance (the row-variance block of the row loop)", file=VA, kind="body",
         find=r"MeasurementModel::rowVariance\s*\(",
         pieces=[F(1984, 1984, 12), F(1986, 2191, 12), N("return variance;")]),
    # --- MeasurementModelNetwork.cpp ---
    dict(name="carrierFrequency", file=NW, kind="block", pieces=[F(105, 120)]),
    dict(name="ensureNetworkSharedStates", file=NW, kind="block", pieces=[F(1212, 1385)]),
    dict(name="noteAbsorbedSharedState (header inline body)", file=NW, kind="body",
         find=r"MeasurementModel::noteAbsorbedSharedState\s*\(", pieces=[H(139, 139, 4)]),
    dict(name="phaseBiasAdmitted (header inline body)", file=NW, kind="body",
         find=r"MeasurementModel::phaseBiasAdmitted\s*\(", pieces=[H(420, 423, 4)]),
    # --- MeasurementModelDd.cpp ---
    dict(name="DD key and name helpers (anonymous namespace)", file=DD, kind="block",
         pieces=[F(2347, 2385)]),
    dict(name="DdSharedSat (was the local SharedInfo)", file=DD, kind="block",
         pieces=[F(2497, 2503, 4)]),
    dict(name="DdRow (was local)", file=DD, kind="block", pieces=[F(2625, 2645, 4)]),
    dict(name="computeMeasurementUpdate (DD overload)", file=DD, kind="block",
         pieces=[F(2387, 2397)]),
    dict(name="buildDdMeasurement (driver)", file=DD, kind="body",
         find=r"MeasurementModel::buildDdMeasurement\s*\(", sig=(2399, 2403), close=3098,
         pieces=[F(2405, 2415), N("DdBuild dd;"), F(2417, 2422),
                 N("ddRunStages(rov_modeled, ref_modeled, current_state, excluded_measurements, dd, em);"),
                 F(3096, 3097)]),
    dict(name="ddRunStages", file=DD, kind="body", find=r"MeasurementModel::ddRunStages\s*\(",
         pieces=[N("ddLogInputDiagnostic(rov_modeled, ref_modeled, dd);"), F(2439, 2440),
                 N("if (!ddSharedSignals(rov_modeled, ref_modeled, dd)) return;"),
                 N("if (!ddSelectPivots(dd)) return;"),
                 N("ddRowsFromBrsd(excluded_measurements, dd);"),
                 N("ddCollapseIonoFree(dd);"),
                 N("if (!ddHasRows(dd)) return;"),
                 N("ddPruneStaleAmbiguities(excluded_measurements, dd);"),
                 N("ddNonIntegerKeys(dd);"),
                 N("ddEnsureAmbiguities(dd);"),
                 N("if (!ddInnovations(current_state, dd)) return;"),
                 N("ddAssemble(dd, em);")]),
    dict(name="ddLogInputDiagnostic", file=DD, kind="body",
         find=r"MeasurementModel::ddLogInputDiagnostic\s*\(",
         pieces=DD_LOG + [G(2423, 2438, "if (!diag) return;")]),
    dict(name="ddSharedSignals", file=DD, kind="body",
         find=r"MeasurementModel::ddSharedSignals\s*\(",
         pieces=DD_LOG + [N("auto& rov_by_sat = dd.rov_by_sat;"),
                          N("auto& ref_by_sat = dd.ref_by_sat;"),
                          N("auto& by_cons = dd.by_cons;"),
                          F(2447, 2496), F(2505, 2574), N("return true;")]),
    dict(name="ddSelectPivots (state write 1 of 3)", file=DD, kind="body",
         find=r"MeasurementModel::ddSelectPivots\s*\(",
         pieces=DD_LOG + [N("const auto& by_cons = dd.by_cons;"),
                          N("auto& pivots = dd.pivots;"),
                          F(2576, 2601), N("return true;")]),
    dict(name="ddRowsFromBrsd", file=DD, kind="body",
         find=r"MeasurementModel::ddRowsFromBrsd\s*\(",
         pieces=[N("const auto& rov_by_sat = dd.rov_by_sat;"),
                 N("const auto& by_cons = dd.by_cons;"),
                 N("const auto& pivots = dd.pivots;"),
                 N("auto& rows = dd.rows;"),
                 F(2661, 2715, 4)]),
    dict(name="ddCollapseIonoFree", file=DD, kind="body",
         find=r"MeasurementModel::ddCollapseIonoFree\s*\(",
         pieces=DD_LOG + [N("auto& rows = dd.rows;"), F(2718, 2856)]),
    dict(name="ddHasRows", file=DD, kind="body", find=r"MeasurementModel::ddHasRows\s*\(",
         pieces=DD_LOG + [N("const auto& rows = dd.rows;"), F(2858, 2862),
                          N("return true;")]),
    dict(name="ddPruneStaleAmbiguities (state write 2 of 3)", file=DD, kind="body",
         find=r"MeasurementModel::ddPruneStaleAmbiguities\s*\(",
         pieces=[N("const auto& pivots = dd.pivots;"), N("const auto& rows = dd.rows;"),
                 F(2888, 2937, 4)]),
    dict(name="ddNonIntegerKeys", file=DD, kind="body",
         find=r"MeasurementModel::ddNonIntegerKeys\s*\(",
         pieces=[N("const auto& rows = dd.rows;"), F(2940, 2955)]),
    dict(name="ddEnsureAmbiguities (state write 3 of 3)", file=DD, kind="body",
         find=r"MeasurementModel::ddEnsureAmbiguities\s*\(",
         pieces=[N("auto& rows = dd.rows;"), F(2442, 2445),
                 G(2958, 2979, "if (code_only) return;")]),
    dict(name="ddInnovations", file=DD, kind="body",
         find=r"MeasurementModel::ddInnovations\s*\(",
         pieces=DD_LOG + [N("auto& rows = dd.rows;"), N("auto& kept = dd.kept;"),
                          N("const bool code_only = config_.getBaselineConfig().code_only;"),
                          F(2981, 3029), N("return true;")]),
    dict(name="ddAssemble", file=DD, kind="body", find=r"MeasurementModel::ddAssemble\s*\(",
         pieces=[N("const auto& klog = dd.klog;"), N("const auto& rows = dd.rows;"),
                 N("const auto& kept = dd.kept;"),
                 N("const Eigen::Index m = static_cast<Eigen::Index>(kept.size());"),
                 F(3031, 3094)]),
]


# ------------------------------------------------------------------------------------------
def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def blank_literals(text):
    """Text with string and character literals blanked (newlines kept): braces inside a
    format string do not count for brace matching."""
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup in ("str", "chr", "raw"):
            out.append("".join(c if c == "\n" else " " for c in s))
        else:
            out.append(s)
    return "".join(out)


def code_lines(text):
    """1-based list of comment-stripped, right-stripped lines."""
    return [""] + [l.rstrip() for l in strip_comments(text).split("\n")]


def collapse(s):
    return " ".join(s.split())


def negate(cond):
    c = cond.strip()
    if c.startswith("!") and re.fullmatch(r"![A-Za-z_]\w*", c):
        return c[1:]
    if re.fullmatch(r"[A-Za-z_]\w*", c):
        return "!" + c
    return "!(" + c + ")"


class Frozen:
    def __init__(self, text, subs):
        self.raw = [""] + text.split("\n")
        raw = list(self.raw)
        for i, pairs in subs.items():
            for fr, to in pairs:
                if fr not in raw[i]:
                    raise ValueError(f"substitution {fr!r} not on frozen line {i}: {raw[i]!r}")
                raw[i] = raw[i].replace(fr, to)
        self.code = code_lines("\n".join(raw[1:]))
        self.plain = code_lines("\n".join(self.raw[1:]))


def expected(unit, FC, FH, drops, claims, problems):
    """[(text, kind)] the unit must hold; records the frozen lines it claims."""
    exp = []

    def take(src, a, b, k, tag):
        for i in range(a, b + 1):
            line = src.code[i]
            if not line.strip():
                continue
            claims.setdefault((tag, i), []).append(unit["name"])
            if tag == "cpp" and i in drops:
                continue
            lead = len(line) - len(line.lstrip(" "))
            if lead < k:
                problems.append(f"{unit['name']}: frozen {tag} {i} has fewer than {k} leading spaces")
                continue
            exp.append((line[k:], "frozen"))

    for p in unit["pieces"]:
        if p[0] == "F":
            take(FC, p[1], p[2], p[3], "cpp")
        elif p[0] == "H":
            take(FH, p[1], p[2], p[3], "hpp")
        elif p[0] == "N":
            exp.append((p[1].strip(), "new"))
        elif p[0] == "G":
            _, s, e, line = p
            m = re.fullmatch(r"\s*if \((.*)\) \{", FC.code[s])
            if not m:
                problems.append(f"{unit['name']}: frozen {s} is not `if (COND) {{`")
                continue
            if FC.code[e].strip() != "}":
                problems.append(f"{unit['name']}: frozen {e} is not the guard's closing brace")
            if line != f"if ({negate(m.group(1))}) return;":
                problems.append(f"{unit['name']}: guard {line!r} is not the negation of "
                                f"`if ({m.group(1)})`")
            claims.setdefault(("cpp", s), []).append(unit["name"])
            claims.setdefault(("cpp", e), []).append(unit["name"])
            exp.append((line, "new"))
            take(FC, s + 1, e - 1, 4, "cpp")
        elif p[0] == "W":
            _, a, b, n = p
            joined = []
            for i in range(a, b + 1):
                if FC.code[i].strip():
                    claims.setdefault(("cpp", i), []).append(unit["name"])
                    joined.append(FC.code[i])
            exp.append((collapse(" ".join(joined)), ("ws", n)))
    return exp


def compare(unit, exp, got, problems):
    """Compare the expected pieces with the new code lines (blank lines removed)."""
    j = 0
    for text, kind in exp:
        if isinstance(kind, tuple):
            n = kind[1]
            seg = got[j:j + n]
            if collapse(" ".join(seg)) != text:
                problems.append(f"{unit['name']}: rejoined line differs\n  want {text!r}\n  got  {seg!r}")
                return False
            j += n
            continue
        if j >= len(got):
            problems.append(f"{unit['name']}: new code ends before {text!r}")
            return False
        g = got[j] if kind == "frozen" else got[j].strip()
        if g != text:
            problems.append(f"{unit['name']}: line {j + 1} of the unit differs\n"
                            f"  want {text!r}\n  got  {got[j]!r}")
            return False
        j += 1
    if unit["kind"] == "body" and j != len(got):
        problems.append(f"{unit['name']}: {len(got) - j} undeclared line(s) in the body, "
                        f"first {got[j]!r}")
        return False
    return True


def locate_body(text, regex):
    lines = code_lines(text)
    blank = [""] + blank_literals(strip_comments(text)).split("\n")
    starts = [i for i in range(1, len(lines)) if re.search(regex, lines[i])]
    if len(starts) != 1:
        raise ValueError(f"{len(starts)} definitions match {regex!r}")
    depth, open_i = 0, None
    for i in range(starts[0], len(blank)):
        for ch in blank[i]:
            if ch == "{":
                depth += 1
                if open_i is None:
                    open_i = i
            elif ch == "}":
                depth -= 1
                if open_i is not None and depth == 0:
                    body = [l for l in lines[open_i + 1:i] if l.strip()]
                    return starts[0], open_i, i, body, lines
        if open_i is None and lines[i].rstrip().endswith(";"):
            raise ValueError(f"{regex!r} matched a declaration")
    raise ValueError("unbalanced braces")


def data_members(lines):
    # A declaration: a type (ending in a word, '>', '*' or '&'), space, a name ending in '_',
    # an optional initializer, ';'.  A statement in an inline body has no type before the
    # name and does not match.
    pat = re.compile(r"^(?!\s*(?:return|friend|using|typedef)\b)\s*[\w:<][\w:<>,\s\*&]*?"
                     r"[\w>\*&]\s+([A-Za-z_]\w*_)\s*(\{[^}]*\}|=[^;]*)?;\s*$")
    return [m.group(1) for l in lines if (m := pat.match(l))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--new-root", default=str(ROOT),
                    help="read the new files under this root (a candidate tree; default: "
                         "this checkout); the frozen source is always read from this checkout")
    a = ap.parse_args()
    new_root = pathlib.Path(a.new_root)

    FC = Frozen(git_show(FROZEN, CPP), SUBS)
    FH = Frozen(git_show(FROZEN, HPP), HSUBS)
    files = {}
    problems_total, fails = 0, 0
    claims = {}

    print(f"frozen {FROZEN}: {CPP} ({len(FC.raw) - 1} lines), {HPP} ({len(FH.raw) - 1} lines)")
    print("\n1. Units")
    for unit in UNITS:
        problems = []
        exp = expected(unit, FC, FH, DROPS, claims, problems)
        text = files.setdefault(unit["file"], (new_root / unit["file"]).read_text(encoding="utf-8")
                                .replace("\r\n", "\n"))
        ok = not problems
        try:
            if unit["kind"] == "body":
                start, open_i, close_i, body, lines = locate_body(text, unit["find"])
                if "sig" in unit:
                    s, e = unit["sig"]
                    want = [l for l in FC.code[s:e + 1] if l.strip()]
                    got = [l for l in lines[start:open_i + 1] if l.strip()]
                    if want != got:
                        problems.append(f"{unit['name']}: signature differs\n  want {want}\n  got  {got}")
                    claims.setdefault(("cpp", unit["close"]), []).append(unit["name"])
                    for i in range(s, e + 1):
                        if FC.code[i].strip():
                            claims.setdefault(("cpp", i), []).append(unit["name"])
                    if lines[close_i].strip() != FC.code[unit["close"]].strip():
                        problems.append(f"{unit['name']}: closing line differs")
                ok = compare(unit, exp, body, problems) and not problems
            else:
                lines = [l for l in code_lines(text)[1:] if l.strip()]
                first = exp[0][0]
                hits = [i for i, l in enumerate(lines) if l == first]
                if len(hits) != 1:
                    problems.append(f"{unit['name']}: first line {first!r} found {len(hits)} times")
                    ok = False
                else:
                    ok = compare(unit, exp, lines[hits[0]:hits[0] + len(exp) + 8], problems) \
                        and not problems
        except ValueError as ex:
            problems.append(f"{unit['name']}: {ex}")
            ok = False
        n_frozen = sum(1 for _, k in exp if k == "frozen")
        n_new = sum(1 for _, k in exp if k == "new")
        print(f"  {'PASS' if ok else 'FAIL'}  {unit['name']}  "
              f"[{pathlib.Path(unit['file']).name}: {n_frozen} frozen, {n_new} new]")
        for p in problems:
            print("        " + p.replace("\n", "\n        "))
        problems_total += len(problems)
        fails += 0 if ok else 1

    print("\n2. Coverage of the frozen MeasurementModel.cpp")
    cov_problems = []
    for i in range(1, len(FC.plain)):
        if not FC.plain[i].strip():
            continue
        owners = claims.get(("cpp", i), [])
        if i in FURNITURE:
            owners = owners + ["furniture"]
        elif i in DROPS and not owners:
            owners = ["drop"]
        if len(owners) != 1:
            cov_problems.append(f"frozen {i} claimed {len(owners)} times {owners}: {FC.plain[i].strip()!r}")
    for i in DROPS:
        if not FC.plain[i].strip():
            cov_problems.append(f"declared drop {i} is not a code line")
    n_code = sum(1 for l in FC.plain[1:] if l.strip())
    print(f"  {n_code} code lines; {len(DROPS)} declared drops; {len(FURNITURE)} furniture; "
          f"{'PASS' if not cov_problems else 'FAIL'}")
    for p in cov_problems:
        print("        " + p)

    print("\n3. Header")
    new_h = (new_root / HPP).read_text(encoding="utf-8").replace("\r\n", "\n")
    new_lines = [l for l in code_lines(new_h)[1:] if l.strip()]
    moved = {i for (tag, i) in claims if tag == "hpp"}
    want = [FH.code[i] for i in range(1, len(FH.code))
            if FH.code[i].strip() and i not in HDROPS and i not in moved]
    j, missing = 0, None
    for w in want:
        while j < len(new_lines) and new_lines[j] != w:
            j += 1
        if j == len(new_lines):
            missing = w
            break
        j += 1
    hdr_problems = []
    if missing is not None:
        hdr_problems.append(f"frozen header line not found in order: {missing!r}")
    old_members = data_members([FH.code[i] for i in range(1, len(FH.code)) if i not in HDROPS])
    new_members = data_members(new_lines)
    if old_members != new_members:
        hdr_problems.append(f"data members differ:\n  frozen {old_members}\n  new    {new_members}")
    print(f"  {len(want)} frozen code lines in order; {len(new_members)} data members in "
          f"construction order; {'PASS' if not hdr_problems else 'FAIL'}")
    for p in hdr_problems:
        print("        " + p.replace("\n", "\n        "))

    print("\nDeclared substitutions (frozen line: from -> to)")
    for i in sorted(SUBS):
        for fr, to in SUBS[i]:
            print(f"  cpp {i}: {fr!r} -> {to!r}")
    for i in sorted(HSUBS):
        for fr, to in HSUBS[i]:
            print(f"  hpp {i}: {fr!r} -> {to!r}")
    print("Declared drops")
    for i in sorted(DROPS):
        print(f"  cpp {i}: {FC.plain[i].strip()!r}  ({DROPS[i]})")
    for i in sorted(HDROPS):
        print(f"  hpp {i}: {FH.plain[i].strip()!r}  ({HDROPS[i]})")
    if a.verbose:
        print("Guards")
        for unit in UNITS:
            for p in unit["pieces"]:
                if p[0] == "G":
                    print(f"  {unit['name']}: frozen {p[1]}..{p[2]} -> {p[3]!r}")

    total_fail = fails + (1 if cov_problems else 0) + (1 if hdr_problems else 0)
    print(f"\nTOTAL: {len(UNITS) - fails}/{len(UNITS)} units PASS; coverage "
          f"{'PASS' if not cov_problems else 'FAIL'}; header {'PASS' if not hdr_problems else 'FAIL'}"
          f" -> {'PASS' if total_fail == 0 else 'FAIL'}")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
