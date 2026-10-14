#!/usr/bin/env python3
"""Relocation verifier for card UOM step U5m (gate G-UR): the MW slip detector.

    python scripts/uom_u5m_verify.py [--verbose] [--tree DIR] [--mutate]

The frozen source is src/core/slip/MwSlipDetector.cpp and include/slip/MwSlipDetector.hpp at
f823a36 (the base of step U5m), read with `git show`; no copy is stored.  The tables below
record, for every unit of the new files, the frozen lines it carries and every declared edit.
The stage is read from the files present: U5m-b (the header split; one source file), U5m-c
(the source split by concern) or U5m-d (the giants cut into steps).

Exit 0 only when every check passes:

  1. Units.     Each unit is a run of code lines in its new file: a "block" (located where its
                expected text occurs, exactly once) or a "body" (the code lines between the
                braces of the definition its regex finds).  Pieces:
                  F(a, b, shift)  frozen lines a..b, character for character after the
                                  declared substitutions, `shift` leading spaces removed;
                  G(if, close, text)  the frozen `if (COND) {` at `if` and its `}` at `close`
                                  became the declared guard `text`; the interior shifts 4;
                  U(if, close, shift) the frozen `if (out_X) {` ... `}` (an out-parameter null
                                  check every caller satisfied) is unwrapped: both lines are
                                  dropped, the interior moves `shift` columns left;
                  N(text)         a declared new code line, compared without outer space.
                Comments and blank lines are ignored on both sides.
  2. Coverage.  Every code line of the frozen MwSlipDetector.cpp is carried exactly once: by a
                unit, a declared drop (with its reason) or file furniture (includes, the
                namespace).
  3. Header.    The frozen header's code lines, after the declared header edits and minus the
                declared header drops, equal the code lines of the new headers minus the
                declared new header lines, as multisets (whitespace-collapsed).
  4. Comments.  Every comment line of the frozen files appears in the new files at least as
                often (whitespace-collapsed), apart from the declared comment drops (the comments
                of deleted code).
  5. Logs.      For every frozen function, the ordered list of its log calls (level and format
                literal) and of the calls it makes into other detector members equals that of
                the new function of the same name, each call into a new step function replaced
                by that step's own list (recursively).

--mutate runs the checks on a scratch copy of the new files with one character of a carried
code line changed, and exits 0 only if that copy FAILS (the verifier can see a change).
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

FROZEN = "f823a36"
FCPP = "src/core/slip/MwSlipDetector.cpp"
FHPP = "include/slip/MwSlipDetector.hpp"
S = "src/core/slip/"
MW, PAIRS, STATS, ZD, BRIDGE, QUERY, RESET, TRACE = (
    S + "MwSlipDetector.cpp", S + "MwSlipDetectorPairs.cpp", S + "MwSlipDetectorStats.cpp",
    S + "MwSlipDetectorZd.cpp", S + "MwPhaseBridge.cpp", S + "MwSlipDetectorQuery.cpp",
    S + "MwSlipDetectorReset.cpp", S + "MwSlipDetectorTrace.cpp")
EPOCH, SAMPLE = S + "MwSlipDetectorEpoch.cpp", S + "MwSlipDetectorSample.cpp"
HDRS = ["include/slip/MwSlipDetector.hpp", "include/slip/MwSlipTypes.hpp",
        "include/slip/MwArcState.hpp"]
WORK_HDR = "include/slip/MwDetectorWork.hpp"


def F(a, b=None, shift=0):
    return ("F", a, a if b is None else b, shift)


def G(if_line, close_line, text):
    return ("G", if_line, close_line, text)


def U(if_line, close_line, shift=4):
    return ("U", if_line, close_line, shift)


def N(text):
    return ("N", text)


# ------------------------------------------------------------------------------------------
# Step U5m-a: dead code (frozen MwSlipDetector.cpp lines no unit carries).
# ------------------------------------------------------------------------------------------
DEAD = "SG-06: getSatelliteMwQuality has no caller (deleted with SatelliteMwQuality)"
# (first, last, reason): every code line in the range is dropped.
DROP_RANGES = [
    (131, 162, DEAD),
    (734, 746, "SG-05: findIonoFreeLabels has no caller"),
    (2090, 2090, "SG-07: epoch_clean_arc_n_ was written and never read (deleted with "
                 "getEpochCleanArcN)"),
    (2542, 2542, "SG-07: the per-epoch clear of epoch_clean_arc_n_"),
    (2992, 3005, "SG-04: a debug summary string built and discarded (its log line was "
                 "commented out)"),
]
# Comments of deleted code (frozen file, line ranges).
COMMENT_DROPS = {
    "cpp": [(2620, 2656, "SG-04: the commented-out [DIAG] coverage block and its preface"),
            (2084, 2089, "SG-07: the comment of the deleted epoch_clean_arc_n_ write"),
            (2986, 3005, "SG-04: the comment of the discarded debug summary"),
            (127, 129, "the section banner of the deleted getSatelliteMwQuality"),
            (101, 103, "the ISlipDetector banner (the contract is described in the header)"),
            (49, 51, "the Construction banner (each new file opens with its own banner)"),
            (1, 13, "the file banner (rewritten for the split files)"),
            (47, 47, "the anonymous namespace's closing comment (U5m-c: the helper is a member)")],
    "hpp": [(123, 128, "SG-06: SatelliteMwQuality's doc"),
            (130, 133, "SG-06: SatelliteMwQuality's member docs"),
            (245, 257, "SG-07: getEpochCleanArcN's doc"),
            (543, 546, "isTracingEnabled's doc (no caller; the virtual left ISlipDetector)"),
            (578, 588, "SG-06: getSatelliteMwQuality's doc"),
            (1265, 1266, "SG-07: epoch_clean_arc_n_'s doc"),
            (1886, 1898, "SG-05: findIonoFreeLabels' doc"),
            (167, 171, "SG-08: detect's @note, rewritten (no code calls it)"),
            (527, 527, "resetAll's doc: a sentence on its caller appended to the line"),
            (536, 536, "enableTracing's doc: a sentence on its callers appended to the line")],
}
# Header lines removed in U5m-a (frozen header line numbers).
HDR_DROPS = {
    3: "SG-29: the second #pragma once",
    **{n: "SG-06: SatelliteMwQuality" for n in (129, 130, 131, 132, 133, 134)},
    259: "SG-07: getEpochCleanArcN",
    529: "resetAll() override -> resetAll() (no longer an ISlipDetector virtual)",
    541: "enableTracing(...) override -> enableTracing(...) (no longer a virtual)",
    547: "isTracingEnabled: no caller on any detector type",
    589: "SG-06: getSatelliteMwQuality",
    1267: "SG-07: epoch_clean_arc_n_",
    1899: "SG-05: findIonoFreeLabels",
}
# Header lines added in U5m-a (code, whitespace-collapsed).
HDR_NEW = [
    "void resetAll();",
    "bool enableTracing(const std::filesystem::path& log_dir);",
]

# ------------------------------------------------------------------------------------------
# Step U5m-b: the header split.
# ------------------------------------------------------------------------------------------
# Rule-based header edits: (regex on the frozen code line, from, to, reason).
HDR_RULES = [
    (r"\bstatic constexpr\b", "static constexpr", "inline constexpr",
     "the constants table moved to namespace mw (slip/MwArcState.hpp)"),
    (r"^\s*struct EpochPairStatus \{", "struct EpochPairStatus {", "struct MwEpochPairStatus {",
     "namespace scope in slip/MwSlipTypes.hpp; the class keeps the name as an alias"),
    (r"^\s*struct WlLockQuality \{", "struct WlLockQuality {", "struct MwWlLockQuality {",
     "namespace scope in slip/MwSlipTypes.hpp; the class keeps the name as an alias"),
    (r"^\s*struct AcrossGapWlEvidence \{", "struct AcrossGapWlEvidence {",
     "struct MwAcrossGapWlEvidence {",
     "namespace scope in slip/MwSlipTypes.hpp; the class keeps the name as an alias"),
    (r"max_gap_seconds_\{ kDefaultMaxGapSeconds \}", "kDefaultMaxGapSeconds",
     "mw::kDefaultMaxGapSeconds", "the constant lives in namespace mw"),
]
# The class names the moved state types through namespace mw (code lines, not definitions).
MW_TYPES = ["MwArcStats", "BridgeArcStats", "DemotedArcLevel", "EstimatedBias"]
HDR_NEW_B = [
    '#include "slip/MwArcState.hpp"',
    '#include "slip/MwSlipTypes.hpp"',
    "using WlLockQuality = MwWlLockQuality;",
    "using AcrossGapWlEvidence = MwAcrossGapWlEvidence;",
    "using EpochPairStatus = MwEpochPairStatus;",
    "using MwArcZdMean = GnssProcessor::MwArcZdMean;",
    "using MwArcZdLast = GnssProcessor::MwArcZdLast;",
    "using MwBridgeMean = GnssProcessor::MwBridgeMean;",
    "using MwBridgeLast = GnssProcessor::MwBridgeLast;",
    "public:", "private:",
    # slip/MwSlipTypes.hpp
    "#pragma once", "#include <cstddef>", "#include <limits>", "#include <string>",
    "namespace GnssProcessor {", "}",
    # slip/MwArcState.hpp
    "#pragma once", "#include <array>", "#include <cstddef>", "#include <deque>",
    "#include <limits>", "#include <string>", "namespace GnssProcessor {", "namespace mw {",
    "}", "}",
]
# Header comment lines added or edited in U5m-a/b are not checked (only losses are).

# ------------------------------------------------------------------------------------------
# Step U5m-c: the source split by concern (file of every frozen definition).
# ------------------------------------------------------------------------------------------
UNIT_FILE = {
    (53, 90): MW, (92, 99): MW, (105, 125): MW, (1438, 2517): MW, (2523, 3411): MW,
    (164, 176): QUERY, (3417, 3459): QUERY, (3461, 3516): QUERY, (3518, 3559): QUERY,
    (3565, 3578): QUERY, (3779, 3815): QUERY,
    (178, 240): ZD, (242, 299): ZD, (363, 413): ZD,
    (305, 331): BRIDGE, (333, 357): BRIDGE, (415, 438): BRIDGE, (440, 618): BRIDGE,
    (624, 661): TRACE, (663, 728): TRACE,
    (748, 773): PAIRS, (775, 784): PAIRS, (786, 805): PAIRS, (807, 811): PAIRS,
    (813, 1034): PAIRS, (1040, 1084): PAIRS, (1086, 1120): PAIRS,
    (1126, 1130): STATS, (1132, 1136): STATS, (1138, 1144): STATS, (1146, 1177): STATS,
    (1179, 1184): STATS, (1186, 1189): STATS, (1191, 1195): STATS, (1197, 1226): STATS,
    (1228, 1300): STATS, (1302, 1334): STATS, (1336, 1436): STATS, (3833, 3840): STATS,
    (3842, 3939): STATS,
    (3584, 3658): RESET, (3660, 3754): RESET, (3756, 3777): RESET, (3817, 3827): RESET,
}
# mwElevationWeight left the anonymous namespace for a private static member (it is shared
# by MwSlipDetectorStats.cpp and the per-sample driver).
ELEV_UNIT = dict(name="mwElevationWeight (a private static member)", file=STATS, kind="block",
                 pieces=[F(41, 46, 4)],
                 subs={41: [("inline double mwElevationWeight(double elev_deg) {",
                             "double MwSlipDetector::mwElevationWeight(double elev_deg) {")]})
ELEV_DROPS = {30: "the anonymous namespace: its one helper became a private static member",
              47: "the anonymous namespace's closing brace"}
HDR_NEW_C = ["static double mwElevationWeight(double elev_deg);"]
# File furniture of the new source files: includes, the namespace, and the using-directive
# for the state namespace (every Mw*.cpp names the constants and arc types of namespace mw).
CPP_FURNITURE_RX = [r'#include [<"][\w/.]+[>"]', r"namespace GnssProcessor \{", r"\}",
                    r"    using namespace mw;"]

FURNITURE = {n: "includes and the namespace: each new file lists its own"
             for n in list(range(15, 27)) + [28, 3941]}

# ------------------------------------------------------------------------------------------
# Step U5m-d: the two giants as drivers over named steps.
# ------------------------------------------------------------------------------------------
# processMwSample: its 18 parameters are MwLane, MwSample and MwSdFrame (the class header);
# its out-parameters are the returned MwVerdict; its body is twelve steps over MwSampleWork
# (slip/MwDetectorWork.hpp).  detectLargeMwResets: a driver over the epoch steps sharing
# MwEpochWork; every flagged lane's results are formed by emitLaneSlip_.


def I(if_line, close_line, shift=4):
    """The frozen `if (COND) {` at if_line and its `}` at close_line: the condition moved to
    the caller (a declared new line there); the interior moves `shift` columns left."""
    return ("I", if_line, close_line, shift)


_T = {"lli_shadowed": "bool", "mw_diff": "double", "dev_hatch": "double",
      "local_sigma_scaled": "double", "local_ref": "double", "td2": "double",
      "use_local": "bool", "use_td": "bool", "dev": "double", "mode_sigma": "double",
      "detect_mode_code": "int", "thresh": "double", "sigma": "double", "is_slip": "bool",
      "is_slip_level": "bool", "epoch_s": "double"}
SA = {
    "obs": "const PppObservation& obs = w.obs;",
    "stats_key": "const std::string& stats_key = w.lane.stats_key;",
    "sig1": "const std::string& sig1 = w.lane.sig1;",
    "sig2": "const std::string& sig2 = w.lane.sig2;",
    "code1": "const std::string& code1 = w.lane.code1;",
    "code2": "const std::string& code2 = w.lane.code2;",
    "mw_m": "const double mw_m = w.sample.mw_m;",
    "mw_m_w": "double& mw_m = w.sample.mw_m;",
    "lambda_wl": "const double lambda_wl = w.sample.lambda_wl;",
    "bia_corrected": "const bool bia_corrected = w.sample.bia_corrected;",
    "bia_corrected_w": "bool& bia_corrected = w.sample.bia_corrected;",
    "sat_mw_bias": "const double sat_mw_bias = w.sample.sat_mw_bias;",
    "pivot_sat": "const std::string& pivot_sat = w.frame.pivot_sat;",
    "pivot_zd_slip_in": "const int pivot_zd_slip_in = w.frame.pivot_zd_slip;",
    "pivot_zd_known_in": "const bool pivot_zd_known_in = w.frame.pivot_zd_known;",
    "stats": "mw::MwArcStats& stats = *w.stats;",
    "verdict": "MwVerdict& verdict = w.verdict;",
    "NaN": "constexpr double NaN = std::numeric_limits<double>::quiet_NaN();",
}
for _n, _t in _T.items():
    SA[_n] = f"const {_t} {_n} = w.{_n};"
    SA[_n + "_w"] = f"{_t}& {_n} = w.{_n};"
EA = {
    "results": "std::vector<MwSlipResult>& results = w.results;",
    "logger": "const auto& logger = w.logger;",
    "sd_groups": "auto& sd_groups = w.sd_groups;",
}


def sa(*names):
    return [N("        " + SA[n]) for n in names]


def ea(*names):
    return [N("        " + EA[n]) for n in names]


def body(name, file, pieces, **kw):
    return dict(name=name, file=file, kind="body",
                find=r"MwSlipDetector::" + name + r"\s*\(", pieces=pieces, **kw)


GUARD = "a step's guard: the condition opens the step as a declared early return"
BRACE = "the guard's closing brace"
OUT = ("the out-parameter resets: MwVerdict's member initializers are these values (the "
       "evidence's bia_corrected was written and never read)")
SIG_S = ("processMwSample's 18-parameter signature: MwLane, MwSample and MwSdFrame now, the "
         "out-parameters the returned verdict")
MEMB_S = "a member of MwSampleWork (slip/MwDetectorWork.hpp)"
MEMB_E = "a member of MwEpochWork (slip/MwDetectorWork.hpp)"
CALL = ("the out-parameter locals and the 18-argument call: the declared MwLane / MwSample "
        "call above it replaces them")
PUSH = "a positional result push_back: emitLaneSlip_ forms every flagged lane's results"
LAMBDA = ("the pushReset lambda: an LLI reset is MwSlipResult{ amb_key } (the lambda's "
          "default arguments were the struct's own defaults)")
BLOCK = "a bare block's braces: the step is the scope"

DROPS_D = {
    **{n: SIG_S for n in range(1438, 1453)},
    2517: "processMwSample's closing brace (the driver's own is new)",
    **{n: OUT for n in (1458, 1459, 1460, 1461, 1462, 1464, 1465, 1466, 1467, 1468)},
    1663: GUARD, 1801: BRACE, 2135: GUARD, 2322: BRACE, 2410: GUARD, 2437: BRACE,
    3059: GUARD, 3247: BRACE,
    1938: MEMB_S, 1939: MEMB_S, 1940: MEMB_S,
    2526: MEMB_E, 2528: MEMB_E + " (bound when the driver builds it)", 2603: MEMB_E,
    **{n: LAMBDA for n in range(2556, 2561)},
    **{n: CALL for n in list(range(2748, 2758)) + list(range(2817, 2827))
       + list(range(2945, 2955)) + list(range(3267, 3283)) + list(range(3332, 3345))},
    **{n: PUSH for n in list(range(2758, 2763)) + list(range(2829, 2839))
       + list(range(2955, 2965)) + list(range(3292, 3313)) + list(range(3362, 3380))},
    3264: BLOCK, 3320: BLOCK,
    2661: None,
}
DROPS_D = {k: v for k, v in DROPS_D.items() if v}

# Out-parameter writes inside processMwSample (frozen 1438..2517), rule-based.
OUT_RULES = [
    (r"if \(out_(slip_cycles|wl_known|slip_frac|slip_wl_float)\)\s*\*out_\1(\s*)=",
     r"verdict.\1\2=", "an out-parameter write is a verdict field"),
    (r"out_evidence->", "verdict.", "an evidence field is a verdict field"),
    (r"if \(out_(slip_wl_float|evidence) && ", "if (",
     "the out-parameter's null test is gone (every caller supplied it)"),
    (r"^(\s*)\*out_slip_wl_float =", r"\1verdict.slip_wl_float =",
     "an out-parameter write is a verdict field"),
]
OUT_RANGE = (1438, 2517)

_VS = "if (const auto v = processMwSample(obs, lane, sample, MwSdFrame{})) {"
UNITS_D = [
    # --- the per-sample driver (MwSlipDetector.cpp) ---
    body("processMwSample", MW, [
        N("        MwSampleWork w{ obs, lane, sample, frame };"),
        N("        stampArc_(w);                                    // S1"),
        N("        if (w.stats->count == 0) {                       // S2 the arc's first sample"),
        N("            if (initArc_(w)) return w.verdict;"),
        N("            return std::nullopt;"),
        N("        }"),
        N("        if (resolveTdConfirm_(w)) return w.verdict;      // S3"),
        N("        tryLock_(w);                                     // S4"),
        N("        tryInhouseCommit_(w);                            // S5"),
        N("        selectStatistic_(w);                             // S6"),
        N("        gate_(w);                                        // S7"),
        N("        recordEpochEvidence_(w);                         // S8"),
        N("        if (onFlag_(w)) return w.verdict;                // S9"),
        N("        foldSample_(w);                                  // S10"),
        N("        if (checkDriftDemotion_(w)) return w.verdict;    // S11"),
        N("        if (checkLevelBackstop_(w)) return w.verdict;    // S12"),
        F(2516)],
        subs={2516: [("return false;", "return std::nullopt;")]}),
    # --- the per-sample steps (MwSlipDetectorSample.cpp) ---
    body("stampArc_", SAMPLE, [
        *sa("obs", "stats_key", "sig1", "sig2", "code1", "code2", "mw_m_w", "lambda_wl",
            "bia_corrected_w", "epoch_s_w", "lli_shadowed_w"),
        F(1470, 1477), N("        mw::MwArcStats& stats = *w.stats;"), F(1479, 1536)],
        subs={1477: [("auto& stats = mw_stats_[stats_key];", "w.stats = &mw_stats_[stats_key];")],
              1479: [("const double epoch_s =", "epoch_s =")],
              1524: [("const bool lli_shadowed =", "lli_shadowed =")]}),
    body("initArc_", SAMPLE, [
        *sa("obs", "stats_key", "sig1", "sig2", "mw_m", "lambda_wl", "bia_corrected",
            "sat_mw_bias", "pivot_sat", "stats", "epoch_s", "verdict"),
        F(1478), F(1538), I(1539, 1652, 4)]),
    body("resolveTdConfirm_", SAMPLE, [
        *sa("obs", "stats_key", "sig1", "sig2", "mw_m", "lambda_wl", "bia_corrected",
            "sat_mw_bias", "pivot_sat", "pivot_zd_slip_in", "pivot_zd_known_in", "stats",
            "epoch_s", "mw_diff_w", "verdict", "NaN"),
        F(1654), F(1656, 1662),
        N("        if (!stats.td_confirm_pending) return false;"),
        F(1664, 1756, 4), U(1757, 1762, 8), F(1763, 1800, 4),
        N("        return false;")],
        subs={1654: [("double mw_diff =", "mw_diff =")]}),
    body("tryLock_", SAMPLE, [*sa("stats_key", "lambda_wl", "stats"), F(1803, 1878)]),
    body("tryInhouseCommit_", SAMPLE, [
        *sa("stats_key", "mw_m_w", "lambda_wl", "bia_corrected_w", "stats", "epoch_s"),
        F(1880, 1902)]),
    body("selectStatistic_", SAMPLE, [
        *sa("mw_m", "stats", "mw_diff", "NaN", "dev_hatch_w", "local_sigma_scaled_w",
            "local_ref_w", "td2_w", "use_local_w", "use_td_w", "dev_w", "mode_sigma_w",
            "detect_mode_code_w"),
        F(1904, 1966)],
        subs={1928: [("const double dev_hatch =", "dev_hatch =")],
              1929: [("double local_sigma_scaled = NaN;", "local_sigma_scaled = NaN;")],
              1930: [("const double local_ref =", "local_ref =")],
              1931: [("const double td2 =", "td2 =")],
              1934: [("const bool use_local =", "use_local =")],
              1936: [("const bool use_td    =", "use_td    =")]}),
    body("gate_", SAMPLE, [
        *sa("obs", "stats_key", "sig1", "sig2", "lambda_wl", "stats", "epoch_s",
            "lli_shadowed", "dev_hatch", "dev", "mode_sigma", "thresh_w", "sigma_w",
            "is_slip_w", "is_slip_level_w"),
        F(1968, 2024)],
        subs={1968: [("const double thresh  =", "thresh  =")],
              1970: [("const double sigma   =", "sigma   =")],
              1971: [("const bool is_slip   =", "is_slip   =")],
              2023: [("const bool is_slip_level =", "is_slip_level =")]}),
    body("recordEpochEvidence_", SAMPLE, [
        *sa("obs", "stats_key", "sig1", "sig2", "mw_m", "lambda_wl", "sat_mw_bias",
            "pivot_sat", "stats", "NaN", "dev_hatch", "local_sigma_scaled", "local_ref", "dev",
            "mode_sigma", "detect_mode_code", "thresh", "sigma", "is_slip"),
        F(2026, 2133)]),
    body("onFlag_", SAMPLE, [
        *sa("obs", "stats_key", "mw_m", "lambda_wl", "bia_corrected", "pivot_zd_slip_in",
            "pivot_zd_known_in", "stats", "mw_diff", "local_ref", "use_local", "use_td", "dev",
            "mode_sigma", "detect_mode_code", "thresh", "sigma", "is_slip", "verdict"),
        N("        if (!is_slip) return false;"),
        F(2136, 2203, 4), U(2204, 2208, 8), F(2209, 2312, 4), U(2313, 2317, 8),
        F(2318, 2321, 4), N("        return false;")]),
    body("foldSample_", SAMPLE, [
        *sa("mw_m", "stats", "epoch_s", "mw_diff", "td2", "is_slip", "is_slip_level"),
        F(2324, 2395)]),
    body("checkDriftDemotion_", SAMPLE, [
        *sa("stats_key", "lambda_wl", "stats", "verdict"),
        F(2397, 2409), N("        if (!(stats.wl_locked && lambda_wl > 0.0)) return false;"),
        F(2411, 2429, 4), U(2430, 2434, 8), F(2435, 2436, 4), N("        return false;")]),
    body("checkLevelBackstop_", SAMPLE, [
        *sa("obs", "stats_key", "sig1", "sig2", "mw_m", "lambda_wl", "sat_mw_bias",
            "pivot_sat", "stats", "local_sigma_scaled", "local_ref", "use_local", "use_td",
            "sigma", "verdict"),
        F(2439, 2505), U(2506, 2510, 4), F(2511, 2514), N("        return false;")]),
    # --- the per-epoch driver (MwSlipDetector.cpp) ---
    body("detectLargeMwResets", MW, [
        N('        MwEpochWork w{ log_manager_.getLogger("mw_slip_detector") };'),
        N("        beginEpoch_(w, observations);                    // E0"),
        F(2605, 2607),
        N("            collectLaneSamples_(w, obs);                 // E1 pass 1, per satellite"),
        F(2972), F(2974, 2985), F(3007, 3008),
        N("            const std::string best_pivot = electPivot_(sat_entries);  // E2 pass 2, per group"),
        N("            if (best_pivot.empty()) continue;"),
        N("            reReferenceGroup_(w, group_key, sat_entries, best_pivot);"),
        F(3248),
        N("            const MwSdFrame frame = processPivot_(w, sat_entries, best_pivot);"),
        N("            processNonPivots_(w, sat_entries, best_pivot, frame);"),
        F(3386),
        N("        return endEpoch_(w);                             // E3")],
        sig=(2523, 2524), close=3411,
        subs={3007: [(": sd_groups) {", ": w.sd_groups) {")]}),
    # --- the per-epoch steps (MwSlipDetectorEpoch.cpp) ---
    body("beginEpoch_", EPOCH, [*ea("results"), F(2527), F(2530, 2553), F(2562, 2579)]),
    body("collectLaneSamples_", EPOCH, [
        F(2609, 2618, 4), F(2658, 2660, 4),
        N("                eraseArcsOnLli_(w, obs, if_pairs);       // the combined path's LLI policy"),
        F(2689, 2690, 4),
        N("            const std::set<std::string> if_band_pairs ="),
        N("                collectCombinedLanes_(w, obs, if_pairs);"),
        N("            collectSupplementaryLanes_(w, obs, if_band_pairs);"),
        F(2845, 2850, 4),
        N("                preserveArcsOnLli_(w, obs);              // the uncombined path's LLI policy"),
        F(2901, 2902, 4),
        N("            collectUncombinedLanes_(w, obs, uc_pairs);"),
        F(2971, 2971, 4)],
        subs={2689: [("continue;", "return;")], 2848: [("continue;", "return;")],
              2901: [("continue;", "return;")]}),
    body("eraseArcsOnLli_", EPOCH, [*ea("results", "logger"), F(2661, 2688, 12)],
         subs={2663: [("pushReset(amb_key);", "results.push_back(MwSlipResult{ amb_key });")]}),
    body("preserveArcsOnLli_", EPOCH, [*ea("results", "logger"), F(2851, 2900, 12)],
         subs={2853: [('pushReset(obs.satellite + "_" + key);',
                       'results.push_back(MwSlipResult{ obs.satellite + "_" + key });')]}),
    body("collectCombinedLanes_", EPOCH, [
        *ea("results", "logger", "sd_groups"),
        F(2692, 2757, 8),
        N("                const MwLane lane{ obs.satellite, s1, s2, c1, c2, ifp.phase_key,"),
        N("                                   stats_key };"),
        N("                const MwSample sample{ mw_raw, lambda_wl, /*bia_corrected=*/false,"),
        N("                                       /*sat_mw_bias=*/0.0 };"),
        N("                " + _VS),
        N("                    emitLaneSlip_(results, lane, *v, lambda_wl,"),
        N("                                  /*bia_corrected=*/false);"),
        F(2758, 2767, 8), N("        return if_band_pairs;")],
        subs={2764: [("wl_known, slip_cyc);", "v->wl_known, v->slip_cycles);")]}),
    body("collectSupplementaryLanes_", EPOCH, [
        *ea("results", "logger", "sd_groups"),
        F(2769, 2826, 8),
        N("                const MwLane lane{ obs.satellite, pair.phase1, pair.phase2,"),
        N("                                   pair.code1, pair.code2, {}, stats_key };"),
        N("                const MwSample sample{ mw_raw, lambda_wl, /*bia_corrected=*/false,"),
        N("                                       /*sat_mw_bias=*/0.0 };"),
        N("                " + _VS),
        F(2827, 2838, 8),
        N("                    emitLaneSlip_(results, lane, *v, lambda_wl,"),
        N("                                  /*bia_corrected=*/false);"),
        F(2839, 2844, 8)],
        subs={2841: [("wl_known, slip_cyc);", "v->wl_known, v->slip_cycles);")]}),
    body("collectUncombinedLanes_", EPOCH, [
        *ea("results", "logger", "sd_groups"),
        F(2904, 2954, 8),
        N("                const MwLane lane{ obs.satellite, pair.phase1, pair.phase2,"),
        N("                                   pair.code1, pair.code2, {}, stats_key };"),
        N("                const MwSample sample{ mw_raw, lambda_wl, /*bia_corrected=*/false,"),
        N("                                       /*sat_mw_bias=*/0.0 };"),
        N("                " + _VS),
        N("                    emitLaneSlip_(results, lane, *v, lambda_wl,"),
        N("                                  /*bia_corrected=*/false);"),
        F(2955, 2970, 8)],
        subs={2967: [("wl_known, slip_cyc);", "v->wl_known, v->slip_cycles);")]}),
    body("electPivot_", EPOCH, [F(3010, 3033, 4), N("        return best_pivot;")],
         subs={3031: [("continue;", "return {};")]}),
    body("reReferenceGroup_", EPOCH, [
        *ea("logger"), F(3035, 3058, 4),
        N("        if (prev_pivot_it == sd_pivot_sat_.end() || prev_pivot_it->second == best_pivot)"),
        N("            return;"),
        F(3060, 3246, 8)]),
    body("processPivot_", EPOCH, [
        *ea("results", "logger"), F(3250, 3263, 4), F(3265, 3274, 8),
        N("        const MwLane lane{ best_pivot, pivot_entry.pair.phase1, pivot_entry.pair.phase2,"),
        N("                           pivot_entry.pair.code1, pivot_entry.pair.code2,"),
        N("                           pivot_entry.if_phase_key, piv_key };"),
        N("        const MwSample sample{ pivot_entry.mw_bia, pivot_entry.lambda_wl,"),
        N("                               /*bia_corrected=*/false, pivot_entry.sat_mw_bias };"),
        N("        const MwSdFrame pivot_frame{ best_pivot, /*pivot_zd_slip=*/0,"),
        N("                                     /*pivot_zd_known=*/true };"),
        N("        if (const auto v = processMwSample(*pivot_entry.obs, lane, sample, pivot_frame)) {"),
        F(3275, 3291, 8),
        N("            emitLaneSlip_(results, lane, *v, pivot_entry.lambda_wl, /*bia_corrected=*/true);"),
        F(3292, 3319, 8),
        N("        return MwSdFrame{ best_pivot, pivot_zd_slip, pivot_zd_known };")],
        subs={3283: [("pivot_zd_slip  = pcyc;", "pivot_zd_slip  = v->slip_cycles;")],
              3284: [("pivot_zd_known = pknown;", "pivot_zd_known = v->wl_known;")],
              3318: [("pknown, pcyc);", "v->wl_known, v->slip_cycles);")]}),
    body("processNonPivots_", EPOCH, [
        *ea("results", "logger"),
        N("        const auto& pivot_entry   = sat_entries.at(best_pivot);"),
        N("        const int  pivot_zd_slip  = frame.pivot_zd_slip;"),
        N("        const bool pivot_zd_known = frame.pivot_zd_known;"),
        F(3322, 3336, 4),
        N("            const MwLane lane{ sat, entry.pair.phase1, entry.pair.phase2,"),
        N("                               entry.pair.code1, entry.pair.code2,"),
        N("                               entry.if_phase_key, stats_key };"),
        N("            const MwSample sample{ sd_mw, entry.lambda_wl, /*bia_corrected=*/true,"),
        N("                                   entry.sat_mw_bias };"),
        N("            if (auto v = processMwSample(*entry.obs, lane, sample, frame)) {"),
        F(3337, 3361, 4),
        N("                emitLaneSlip_(results, lane, *v, entry.lambda_wl, /*bia_corrected=*/true);"),
        F(3362, 3385, 4)],
        subs={3349: [("slip_cyc = slip_cyc + pivot_zd_slip;",
                      "v->slip_cycles = v->slip_cycles + pivot_zd_slip;")],
              3350: [("wl_known = wl_known && pivot_zd_known;",
                      "v->wl_known = v->wl_known && pivot_zd_known;")],
              3354: [("slip_wl_f = (pivot_zd_known && std::isfinite(slip_wl_f))",
                      "v->slip_wl_float = (pivot_zd_known && std::isfinite(v->slip_wl_float))")],
              3355: [("? slip_wl_f + static_cast<double>(pivot_zd_slip)",
                      "? v->slip_wl_float + static_cast<double>(pivot_zd_slip)")],
              3383: [("wl_known, slip_cyc);", "v->wl_known, v->slip_cycles);")]}),
    body("endEpoch_", EPOCH, [*ea("results"), F(3388, 3410)],
         subs={3410: [("return results;", "return std::move(results);")]}),
    body("emitLaneSlip_", EPOCH, [
        N("        auto emit = [&](const std::string& amb_key) {"),
        N("            results.push_back({ amb_key, verdict.slip_cycles, verdict.wl_known, lambda_wl,"),
        N("                                lane.sig1, lane.sig2, lane.satellite, verdict.slip_frac,"),
        N("                                verdict.slip_wl_float, verdict.evidence_ratio,"),
        N("                                verdict.arc_epochs, bia_corrected });"),
        N("        };"),
        N("        if (!lane.if_label.empty()) {             // a combined lane: its IF ambiguity"),
        N('            emit(lane.satellite + "_" + lane.if_label);'),
        N("            return;"),
        N("        }"),
        N('        emit(lane.satellite + "_" + lane.sig1);   // an uncombined lane: both signals'),
        N('        emit(lane.satellite + "_" + lane.sig2);')]),
    # --- the SD group entry, a nested type now (slip/MwDetectorWork.hpp) ---
    dict(name="SatMwEntry (was a local struct of detectLargeMwResets)", file=WORK_HDR,
         kind="block", pieces=[F(2591, 2601, 4)],
         subs={2591: [("struct SatMwEntry {", "struct MwSlipDetector::SatMwEntry {")]}),
]
# The giants whose whole-function block units the cut replaces.
CUT = {(1438, 2517), (2523, 3411)}

COMMENT_DROPS_D = {
    "cpp": [(2555, 2555, "the pushReset lambda's comment (the lambda is gone)"),
            (1454, 1457, "the evidence reset's comment: MwVerdict's member initializers replace "
                         "the reset, and the lane class is the caller's (emitLaneSlip_)"),
            *[(a, b, "the argument-name comments of a replaced call or push_back")
              for a, b in ((2753, 2762), (2822, 2838), (2950, 2964), (3275, 3282),
                           (3292, 3312), (3337, 3344), (3362, 3379))]],
    "hpp": [(106, 116, "MwSampleEvidence's doc: MwVerdict carries the evidence (its own doc)"),
            (1796, 1863, "processMwSample's doc: rewritten for the new signature; the "
                         "parameters' meanings are the field docs of MwLane, MwSample, "
                         "MwSdFrame and MwVerdict")],
}
HDR_DROPS_D = {
    **{n: "MwSampleEvidence: replaced by MwVerdict" for n in range(117, 122)},
    **{n: "processMwSample's 18-parameter declaration" for n in range(1864, 1879)},
}



# The class header's step-U5m-d block (inserted before the network-only banner): the
# per-sample value types, the working values' declarations, the two drivers' steps.
HDR_ADD_D = r'''
        // ======================================================================
        // The two drivers' steps (card UOM step U5m-d)
        //
        // detectLargeMwResets and processMwSample are ordered drivers over the
        // private steps below; the working values the steps share are declared
        // here and defined in slip/MwDetectorWork.hpp, which lists the steps.
        // ======================================================================

        /**
         * @brief The lane a sample belongs to: one satellite and one signal pair.
         *
         * The arc key is "<sat>_<sig1>_<sig2>"; on the combined path it is written
         * "<sat>_<IF label>", the same string, since the IF label is "<sig1>_<sig2>".
         */
        struct MwLane {
            std::string satellite;   ///< Satellite identifier (e.g. "G01").
            std::string sig1;        ///< First phase signal (higher frequency, e.g. "L1C").
            std::string sig2;        ///< Second phase signal (lower frequency, e.g. "L2W").
            std::string code1;       ///< RINEX code observable the MW combination consumed on
                                     ///< @c sig1, recorded so a consumer can name the satellite
                                     ///< code-bias column it carries (@ref MwArcZdMean::code1).
                                     ///< Bookkeeping only: the MW value arrives pre-computed.
            std::string code2;       ///< RINEX code observable on @c sig2 (see @c code1).
            std::string if_label;    ///< Combined (ionosphere-free) path: the IF phase label
                                     ///< ("L1C_L2W"), whose one ambiguity a flag resets.  Empty
                                     ///< on the uncombined path, where a flag resets both
                                     ///< signals' ambiguities.
            std::string stats_key;   ///< The arc key into @c mw_stats_ (see above).
        };

        /**
         * @brief One MW value of a lane, as the caller formed it.
         */
        struct MwSample {
            double mw_m{ 0.0 };            ///< MW value (meters), already bias-corrected by the
                                           ///< caller (satellite BIA; single-differenced
                                           ///< against the group pivot on a BIA arc).
            double lambda_wl{ 0.0 };       ///< Wide-lane wavelength (meters).
            bool   bia_corrected{ false }; ///< True if both the satellite (BIA) and the receiver
                                           ///< (BSSD) biases are removed: the value is near-integer
                                           ///< in WL cycles and eligible for rounding.
            double sat_mw_bias{ 0.0 };     ///< Satellite MW bias that was applied (meters, for
                                           ///< the trace).
        };

        /**
         * @brief The single-difference frame of a sample: the MW pivot of its
         *        (constellation, signal-pair) group and the pivot's own slip this pass.
         *
         * The default is the undifferenced frame: no pivot, nothing moved.
         */
        struct MwSdFrame {
            std::string pivot_sat;          ///< SD pivot satellite, recorded in the trace's
                                            ///< @c mw_pivot column; empty for an undifferenced
                                            ///< (no-BIA) arc.
            int  pivot_zd_slip{ 0 };        ///< The pivot's own ZD slip this pass (cycles; 0
                                            ///< when it is clean or the arc is undifferenced).
                                            ///< Consumed only by the locked-arc TD confirmation,
                                            ///< where non-zero marks the SD frame as moved: a new
                                            ///< pending flag is not deferred, an existing one is
                                            ///< not classified.
            bool pivot_zd_known{ true };    ///< Whether the pivot's ZD slip integer is known;
                                            ///< false likewise marks the frame moved.
        };

        /**
         * @brief What @c processMwSample reports for a flagged sample.
         *
         * The member initializers are the values a path that does not set a field reports.
         * A path with no meaningful strength leaves @c evidence_ratio NaN instead of
         * inventing one, so @c std::isfinite on it tells whether the path published one.
         * The integer and the float are in the sample's frame (single-differenced on a BIA
         * arc): the caller re-references them to the satellite's own ZD slip.
         */
        struct MwVerdict {
            int    slip_cycles{ 0 };       ///< Integer WL slip magnitude (valid when @c wl_known).
            bool   wl_known{ false };      ///< True when @c slip_cycles is reliable (only the
                                           ///< unlocked-arc path publishes one).
            double slip_frac{ 0.5 };       ///< |x - round(x)| of the float value before rounding
                                           ///< (0 = exact, up to 0.5 = worst).
            double slip_wl_float{ std::numeric_limits<double>::quiet_NaN() };
                                           ///< Float WL-step estimate (cycles) where the path has
                                           ///< one (the unlocked-arc jump, the TD-confirmation
                                           ///< level gap, the drift, the level-disagreement gap);
                                           ///< used to select candidates, never as an integer.
            double evidence_ratio{ std::numeric_limits<double>::quiet_NaN() };
                                           ///< |dev|/threshold at the firing gate.
            int    arc_epochs{ 0 };        ///< The arc's sample count before the erase (0 on the
                                           ///< init-suspicious path).
        };

        struct SatMwEntry;    ///< One satellite's BIA-corrected sample of an SD group (slip/MwDetectorWork.hpp).
        struct MwSampleWork;  ///< The working value of processMwSample's steps (slip/MwDetectorWork.hpp).
        struct MwEpochWork;   ///< The working value of detectLargeMwResets' steps (slip/MwDetectorWork.hpp).

        /**
         * @brief Process one MW sample of a lane: update the arc's statistics or flag a slip.
         *
         * The per-sample driver: twelve steps over one @c MwSampleWork, ending at the first
         * step that flags a slip.  For BIA+BSSD-corrected arcs the WL integer locks once the
         * statistical lock criterion passes (n ≥ 10).  A known integer slip ΔN_WL is reported
         * only by the unlocked-arc path, when the jump deviation is near-integer and, in the
         * TimeDifference modes, resolvable from a single sample.  Every locked-arc detection
         * (drift demotion, TD confirmation) reports an unknown integer, so the caller resets.
         *
         * @param obs     Observation (the epoch time, the elevation, tracing metadata).
         * @param lane    The satellite and signal pair (@ref MwLane).
         * @param sample  The MW value and its frame flags (@ref MwSample).
         * @param frame   The single-difference frame (@ref MwSdFrame); default undifferenced.
         * @return The verdict when a slip was flagged (the caller generates reset keys);
         *         empty otherwise.
         */
        std::optional<MwVerdict> processMwSample(const PppObservation& obs, const MwLane& lane,
                                                 const MwSample& sample, const MwSdFrame& frame);

        // --- processMwSample's steps (MwSlipDetectorSample.cpp) --------------------------

        /**
         * @brief S1: apply a stored in-house bias, reset the arc on a data gap, stamp its code
         *        channels, snapshot it for rollback, and stamp the per-epoch pair status.
         * @param w  The sample's working value; sets @c stats, @c epoch_s, @c lli_shadowed.
         */
        void stampArc_(MwSampleWork& w);

        /**
         * @brief S2: open a fresh arc with its first sample, and flag a BIA-corrected first
         *        sample whose fractional offset says a slip happened during the gap.
         * @param w  The sample's working value (an arc with no samples).
         * @return True if the first sample is flagged (the verdict is set).
         */
        bool initArc_(MwSampleWork& w);

        /**
         * @brief S3: form this sample's TD1 and resolve a pending locked-arc TD confirmation:
         *        a held level is a step (emit an unknown-integer slip and erase the arc), a
         *        returned level an outlier (keep the lock, excise it from the TD chain).
         * @param w  The sample's working value; sets @c mw_diff.
         * @return True if a confirmed step was emitted.
         */
        bool resolveTdConfirm_(MwSampleWork& w);

        /**
         * @brief S4: lock the WL integer of a BIA-corrected arc once the statistical lock
         *        criterion passes.
         * @param w  The sample's working value.
         */
        void tryLock_(MwSampleWork& w) const;

        /**
         * @brief S5: commit (or update) the in-house bias of an unbiased arc and move the
         *        sample into the corrected frame (processing.inhouse_bias_estimation only).
         * @param w  The sample's working value.
         */
        void tryInhouseCommit_(MwSampleWork& w);

        /**
         * @brief S6: the deviation and the mature spread the gate tests, per the detection
         *        reference (Hatch mean, local median, first or second time-difference).
         * @param w  The sample's working value; sets the statistic's members.
         */
        void selectStatistic_(MwSampleWork& w) const;

        /**
         * @brief S7: the adaptive threshold and the flag verdict in the active and the Hatch
         *        frame, the pair-status record and the MWDEC decision line.
         * @param w  The sample's working value; sets @c thresh, @c sigma, @c is_slip,
         *           @c is_slip_level.
         */
        void gate_(MwSampleWork& w);

        /**
         * @brief S8: the score cache, the affirmative-clean set, the cross-satellite pool
         *        and the trace row.
         * @param w  The sample's working value.
         */
        void recordEpochEvidence_(MwSampleWork& w);

        /**
         * @brief S9: a flagged sample.  A locked arc absorbs it, defers it to a TD
         *        confirmation, or (under a pivot slip) emits at once; an unlocked arc emits
         *        it, with a known integer when the jump rounds cleanly, and is erased.
         * @param w  The sample's working value.
         * @return True if a slip was emitted.
         */
        bool onFlag_(MwSampleWork& w);

        /**
         * @brief S10: fold the sample into the Welford/Hatch statistics and the windows.
         * @param w  The sample's working value.
         */
        void foldSample_(MwSampleWork& w) const;

        /**
         * @brief S11: demote a locked arc whose live mean drifted from its integer, and emit
         *        an unknown-integer slip.
         * @param w  The sample's working value.
         * @return True if the lock was demoted (a slip emitted).
         */
        bool checkDriftDemotion_(MwSampleWork& w);

        /**
         * @brief S12: restart an unlocked arc whose Hatch mean and local median disagree by
         *        more than the drift bound (an absorbed step), emitting an unknown-integer slip.
         * @param w  The sample's working value.
         * @return True if a slip was emitted.
         */
        bool checkLevelBackstop_(MwSampleWork& w);

        // --- detectLargeMwResets' steps (MwSlipDetectorEpoch.cpp) -------------------------

        /**
         * @brief E0: the per-epoch resets (snapshots, clean set, pair status), the LLI
         *        rotation, the epoch time, and the aging of demoted-level stashes.
         * @param w             The epoch's working value.
         * @param observations  The epoch's observations.
         */
        void beginEpoch_(MwEpochWork& w, const std::vector<PppObservation>& observations);

        /**
         * @brief E1: one satellite of pass 1.  Fold the phase bridge, then take the path:
         *        the combined path (IF labels present) or the uncombined path, each with its
         *        own LLI policy.
         * @param w    The epoch's working value.
         * @param obs  The satellite's observation (above the elevation mask).
         */
        void collectLaneSamples_(MwEpochWork& w, const PppObservation& obs);

        /**
         * @brief The combined path's LLI policy: schedule each IF ambiguity's reset, write the
         *        LLI trace rows, record the flagged bands, and erase the satellite's arcs.
         * @param w         The epoch's working value.
         * @param obs       The satellite's observation.
         * @param if_pairs  Its ionosphere-free phase/code label pairs.
         */
        void eraseArcsOnLli_(MwEpochWork& w, const PppObservation& obs,
                             const std::vector<IfPair>& if_pairs);

        /**
         * @brief The uncombined path's LLI policy: schedule every phase ambiguity's reset and
         *        record the flagged bands for the next epoch's LLI shadow, keeping the arcs.
         * @param w    The epoch's working value.
         * @param obs  The satellite's observation.
         */
        void preserveArcsOnLli_(MwEpochWork& w, const PppObservation& obs);

        /**
         * @brief The combined path's IF lanes: form each lane's MW; a BIA-corrected lane joins
         *        its SD group, another is processed undifferenced at once.
         * @param w         The epoch's working value.
         * @param obs       The satellite's observation.
         * @param if_pairs  Its ionosphere-free phase/code label pairs.
         * @return The band pairs ("1_2") the IF lanes cover.
         */
        std::set<std::string> collectCombinedLanes_(MwEpochWork& w, const PppObservation& obs,
                                                    const std::vector<IfPair>& if_pairs);

        /**
         * @brief The combined path's supplementary lanes: uncombined band pairs the IF labels
         *        do not cover, routed as in @c collectCombinedLanes_.
         * @param w              The epoch's working value.
         * @param obs            The satellite's observation.
         * @param if_band_pairs  The band pairs the IF lanes cover.
         */
        void collectSupplementaryLanes_(MwEpochWork& w, const PppObservation& obs,
                                        const std::set<std::string>& if_band_pairs);

        /**
         * @brief The uncombined path's lanes: one per band pair, routed as in
         *        @c collectCombinedLanes_.
         * @param w         The epoch's working value.
         * @param obs       The satellite's observation.
         * @param uc_pairs  Its viable MW pairs.
         */
        void collectUncombinedLanes_(MwEpochWork& w, const PppObservation& obs,
                                     const std::vector<UncombinedPair>& uc_pairs);

        /**
         * @brief E2a: elect an SD group's MW pivot: the highest elevation, skipping the
         *        deprioritized satellites unless every member is one.
         * @param sat_entries  The group's members, by satellite.
         * @return The pivot satellite, or empty if none can be elected.
         */
        std::string electPivot_(const std::map<std::string, SatMwEntry>& sat_entries) const;

        /**
         * @brief E2b: when the group's pivot changes, S-transform its locked SD arcs onto the
         *        new pivot (or erase them), drop the old pivot's ZD arc, and re-reference the
         *        stored in-house biases.
         * @param w            The epoch's working value.
         * @param group_key    The group ("<cons>_<sig1>_<sig2>").
         * @param sat_entries  The group's members.
         * @param best_pivot   The elected pivot.
         */
        void reReferenceGroup_(MwEpochWork& w, const std::string& group_key,
                               const std::map<std::string, SatMwEntry>& sat_entries,
                               const std::string& best_pivot);

        /**
         * @brief E2c: the pivot's own undifferenced sample; its ZD slip anchors the group's
         *        frame.
         * @param w            The epoch's working value.
         * @param sat_entries  The group's members.
         * @param best_pivot   The pivot.
         * @return The group's SD frame: the pivot and its ZD slip this pass.
         */
        MwSdFrame processPivot_(MwEpochWork& w,
                                const std::map<std::string, SatMwEntry>& sat_entries,
                                const std::string& best_pivot);

        /**
         * @brief E2d: the non-pivot members, single-differenced against the pivot; a flag's
         *        SD jump is re-referenced to the satellite's own ZD slip.
         * @param w            The epoch's working value.
         * @param sat_entries  The group's members.
         * @param best_pivot   The pivot.
         * @param frame        The group's SD frame (@c processPivot_).
         */
        void processNonPivots_(MwEpochWork& w,
                               const std::map<std::string, SatMwEntry>& sat_entries,
                               const std::string& best_pivot, const MwSdFrame& frame);

        /**
         * @brief E3: consume the pivot hints, finalize the cross-satellite σ tiers, flush the
         *        trace.
         * @param w  The epoch's working value.
         * @return The epoch's results.
         */
        std::vector<MwSlipResult> endEpoch_(MwEpochWork& w);

        /**
         * @brief Append the results of one flagged lane: one per ambiguity it resets (the IF
         *        ambiguity of a combined lane; the two signals' of an uncombined lane).
         * @param results        The epoch's results.
         * @param lane           The flagged lane.
         * @param verdict        Its verdict (re-referenced to ZD by the caller where needed).
         * @param lambda_wl      Its wide-lane wavelength (meters).
         * @param bia_corrected  The lane class the results report (see
         *                       @ref MwSlipResult::bia_corrected).
         */
        static void emitLaneSlip_(std::vector<MwSlipResult>& results, const MwLane& lane,
                                  const MwVerdict& verdict, double lambda_wl,
                                  bool bia_corrected);

        /// Unit-test seam (tests/test_mw_detector_steps.cpp): reaches the steps above.
        friend struct MwDetectorStepsTestAccess;
'''.split('\n')[1:]

# slip/MwDetectorWork.hpp; "@@SATMWENTRY@@" is where the carried SD group entry goes.
WORK_HDR_TEXT = r'''#pragma once

/**
 * @file MwDetectorWork.hpp
 * @brief @internal The working values the MW slip detector's two drivers share with
 *        their steps.
 *
 * MwSlipDetector::detectLargeMwResets (one epoch) and MwSlipDetector::processMwSample
 * (one sample) are ordered drivers over named private steps (card UOM step U5m-d):
 *
 *   detectLargeMwResets  (MwSlipDetector.cpp; the steps in MwSlipDetectorEpoch.cpp)
 *     E0  beginEpoch_                 per-epoch resets, the LLI rotation, the epoch time
 *     E1  collectLaneSamples_         per satellite: the bridge fold, then its path:
 *           eraseArcsOnLli_             combined path under LLI: resets, erase the arcs
 *           collectCombinedLanes_       combined path: the IF lanes
 *           collectSupplementaryLanes_  combined path: band pairs the IF labels miss
 *           preserveArcsOnLli_          uncombined path under LLI: resets, keep the arcs
 *           collectUncombinedLanes_     uncombined path: the band-pair lanes
 *         A BIA-corrected lane joins its SD group; any other lane is processed
 *         undifferenced at once.
 *     E2  per SD group: electPivot_, reReferenceGroup_ (a changed pivot), processPivot_
 *         (the pivot's ZD slip anchors the frame), processNonPivots_ (SD samples,
 *         re-referenced to ZD)
 *     E3  endEpoch_                   hints consumed, cross-satellite tiers, trace flush
 *   emitLaneSlip_ forms the results of every flagged lane.
 *
 *   processMwSample  (MwSlipDetector.cpp; the steps in MwSlipDetectorSample.cpp)
 *     S1  stampArc_             in-house bias, gap reset, code stamps, snapshot, pair status
 *     S2  initArc_              the arc's first sample (and its init-suspicious check)
 *     S3  resolveTdConfirm_     this sample's TD1; a pending locked-arc TD confirmation
 *     S4  tryLock_              the statistical WL lock
 *     S5  tryInhouseCommit_     the in-house bias commit
 *     S6  selectStatistic_      the deviation and spread the gate tests
 *     S7  gate_                 the threshold, the verdicts, the decision line
 *     S8  recordEpochEvidence_  score cache, clean set, cross-satellite pool, trace row
 *     S9  onFlag_               a flagged sample: absorb, defer, or emit
 *     S10 foldSample_           the Welford/Hatch fold and the windows
 *     S11 checkDriftDemotion_   a locked arc's drift
 *     S12 checkLevelBackstop_   an unlocked arc's level disagreement
 *   The sample ends at the first step that flags a slip; the driver returns the verdict.
 *
 * Each step opens with an alias block binding the names its code had as locals or
 * parameters of the former single function (`const double lambda_wl = w.sample.lambda_wl;`),
 * so the bodies read as they did; scripts/uom_u5m_verify.py proves them against f823a36.
 * A step hands a result to later steps by writing its member.
 *
 * Included by MwSlipDetector.cpp, MwSlipDetectorEpoch.cpp and MwSlipDetectorSample.cpp.
 */

#include "slip/MwSlipDetector.hpp"
#include <spdlog/spdlog.h>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

@@SATMWENTRY@@

    /**
     * @brief The working value of one @c MwSlipDetector::detectLargeMwResets call.
     *
     * Built by the driver, filled by the steps in order, discarded when the call returns.
     */
    struct MwSlipDetector::MwEpochWork {
        std::shared_ptr<spdlog::logger> logger;  ///< The "mw_slip_detector" logger.
        std::vector<MwSlipResult> results;       ///< The epoch's results, in emission order.
        // group_key -> satellite -> entry
        std::map<std::string, std::map<std::string, SatMwEntry>> sd_groups;  ///< E1 fills, E2 reads.
    };

    /**
     * @brief The working value of one @c MwSlipDetector::processMwSample call.
     *
     * The inputs, then the members each step sets, grouped by that step.  A member is
     * read only by the steps after the one that sets it.
     */
    struct MwSlipDetector::MwSampleWork {
        // --- the call's inputs ---------------------------------------------------------
        const PppObservation& obs;   ///< Observation (epoch time, elevation, tracing).
        const MwLane& lane;          ///< The lane.
        MwSample sample;             ///< The sample; a copy, since the in-house bias moves
                                     ///< @c mw_m and @c bia_corrected into the corrected frame.
        const MwSdFrame& frame;      ///< The single-difference frame.

        // --- S1 stampArc_ ----------------------------------------------------------------
        mw::MwArcStats* stats = nullptr;  ///< The arc (mw_stats_[lane.stats_key]); erased by
                                          ///< the step that emits a slip and erases the arc.
        double epoch_s = 0.0;        ///< Continuous GPS seconds of the sample.
        bool lli_shadowed = false;   ///< The first sample after an LLI on one of the bands.

        // --- S3 resolveTdConfirm_ --------------------------------------------------------
        double mw_diff = 0.0;        ///< TD1 = mw - prev_mw (re-based on an outlier excision).

        // --- S6 selectStatistic_ ---------------------------------------------------------
        double dev_hatch = 0.0;      ///< Deviation from the Hatch mean.
        double local_sigma_scaled = std::numeric_limits<double>::quiet_NaN();  ///< √2·MAD of the ring.
        double local_ref = std::numeric_limits<double>::quiet_NaN();           ///< Ring median.
        double td2 = std::numeric_limits<double>::quiet_NaN();                 ///< TD2, NaN until formed.
        bool use_local = false;      ///< The local-median reference governs.
        bool use_td = false;         ///< A time-difference reference governs.
        double dev = 0.0;             // deviation the gate tests
        double mode_sigma = 0.0;      // mature spread of the same statistic (NaN → warmup floors)
        int    detect_mode_code = 0;  // trace tag: 0=hatch, 1=local, 2=td1, 3=td2

        // --- S7 gate_ --------------------------------------------------------------------
        double thresh = 0.0;         ///< The adaptive threshold (meters).
        double sigma = 0.0;          ///< The TD1 MAD σ (meters; NaN while immature).
        bool is_slip = false;        ///< The active statistic's verdict.
        bool is_slip_level = false;  ///< The Hatch frame's verdict (guards the M2 admission).

        // --- the result ------------------------------------------------------------------
        MwVerdict verdict;           ///< Set by the step that flags.
    };

} // namespace GnssProcessor
'''.split('\n')

# The header code lines step U5m-d adds (comments stripped, whitespace collapsed).
def _code(lines):
    text = strip_comments("\n".join(lines))
    return [" ".join(l.split()) for l in text.split("\n")
            if l.strip() and "@@SATMWENTRY@@" not in l]


HDR_NEW_D = _code(HDR_ADD_D) + _code(WORK_HDR_TEXT) + ["#include <optional>"]


# ------------------------------------------------------------------------------------------
def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").lstrip("﻿").replace("\r\n", "\n")


def read(tree, path):
    return (tree / path).read_text(encoding="utf-8").lstrip("﻿").replace("\r\n", "\n")


def blank_literals(text):
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup in ("str", "chr", "raw"):
            out.append("".join(c if c == "\n" else " " for c in s))
        else:
            out.append(s)
    return "".join(out)


def code_lines(text):
    return [""] + [l.rstrip() for l in strip_comments(text).split("\n")]


def collapse(s):
    return " ".join(s.split())


def comment_lines(text, ranges=None):
    """Whitespace-collapsed comment lines; with `ranges`, only those whose line is inside."""
    out, line = [], 1
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup in ("lc", "bc"):
            parts = s[2:].split("\n") if m.lastgroup == "lc" else s[2:-2].split("\n")
            for k, part in enumerate(parts):
                t = part.strip().lstrip("/").lstrip("*").strip()
                if t and (ranges is None or any(a <= line + k <= b for a, b, *_ in ranges)):
                    out.append(collapse(t))
        line += s.count("\n")
    return out


class Frozen:
    """The frozen file; `sub_raw` and `code` carry the declared substitutions (per line, then
    the rule-based ones over `rule_range`)."""
    def __init__(self, text, subs, rules=(), rule_range=(0, 0)):
        self.raw = [""] + text.split("\n")
        raw = list(self.raw)
        self.rule_hits = []
        for i, pairs in subs.items():
            for fr, to in pairs:
                if fr not in raw[i]:
                    raise ValueError(f"substitution {fr!r} not on frozen line {i}: {raw[i]!r}")
                raw[i] = raw[i].replace(fr, to)
        for i in range(rule_range[0], rule_range[1] + 1):
            for rx, to, why in rules:
                new = re.sub(rx, to, raw[i])
                if new != raw[i]:
                    self.rule_hits.append((i, raw[i].strip(), new.strip(), why))
                    raw[i] = new
        self.sub_raw = raw
        self.code = code_lines("\n".join(raw[1:]))
        self.plain = code_lines("\n".join(self.raw[1:]))


def stage(tree):
    if (tree / WORK_HDR).exists():
        return "d"
    if (tree / PAIRS).exists():
        return "c"
    return "b"


def units_for(st):
    units, subs = [], {}
    if st in ("c", "d"):
        units.append(ELEV_UNIT)
        subs.update(ELEV_UNIT["subs"])
    else:
        units.append(dict(name="the anonymous namespace (mwElevationWeight)", file=MW,
                          kind="block", pieces=[F(30, 47)]))
    cut = CUT if st == "d" else set()
    for (a, b), f in sorted(UNIT_FILE.items()):
        if (a, b) in cut:
            continue
        units.append(dict(name=f"frozen {a}..{b}", file=f if st in ("c", "d") else MW,
                          kind="block", pieces=[F(a, b)]))
    if st == "d":
        for u in UNITS_D:
            units.append(u)
            for i, pairs in u.get("subs", {}).items():
                subs.setdefault(i, []).extend(pairs)
    return units, subs


def expected(unit, FC, drops, claims, problems):
    exp = []

    def take(a, b, k, tag="cpp"):
        for i in range(a, b + 1):
            line = FC.code[i]
            if not line.strip():
                continue
            claims.setdefault(i, []).append(unit["name"])
            if i in drops:
                continue
            lead = len(line) - len(line.lstrip(" "))
            if lead < k:
                problems.append(f"{unit['name']}: frozen {i} has fewer than {k} leading spaces")
                continue
            exp.append((line[k:], "frozen", i))

    for p in unit["pieces"]:
        if p[0] == "F":
            take(p[1], p[2], p[3])
        elif p[0] == "N":
            exp.append((strip_comments(p[1]).strip(), "new", None))
        elif p[0] == "G":
            _, s, e, text = p
            if not re.fullmatch(r"\s*if \(.*\) \{", FC.code[s]):
                problems.append(f"{unit['name']}: frozen {s} is not `if (COND) {{`")
            if FC.code[e].strip() != "}":
                problems.append(f"{unit['name']}: frozen {e} is not the guard's closing brace")
            claims.setdefault(s, []).append(unit["name"])
            claims.setdefault(e, []).append(unit["name"])
            exp.append((text, "new", s))
            take(s + 1, e - 1, 4)
        elif p[0] == "I":
            _, s, e, k = p
            if not re.fullmatch(r"\s*if \(.*\) \{", FC.code[s]):
                problems.append(f"{unit['name']}: frozen {s} is not `if (COND) {{`")
            if FC.code[e].strip() != "}":
                problems.append(f"{unit['name']}: frozen {e} is not the block's closing brace")
            claims.setdefault(s, []).append(unit["name"])
            claims.setdefault(e, []).append(unit["name"])
            take(s + 1, e - 1, k)
        elif p[0] == "U":
            _, s, e, k = p
            if not re.fullmatch(r"\s*if \(out_\w+\) \{", FC.plain[s]):
                problems.append(f"{unit['name']}: frozen {s} is not `if (out_X) {{`")
            if FC.code[e].strip() != "}":
                problems.append(f"{unit['name']}: frozen {e} is not the unwrapped block's brace")
            claims.setdefault(s, []).append(unit["name"])
            claims.setdefault(e, []).append(unit["name"])
            take(s + 1, e - 1, k)
    return exp


def match_at(exp, got, j):
    for k, (text, kind, _) in enumerate(exp):
        if j + k >= len(got):
            return False
        g = got[j + k][1] if kind == "frozen" else got[j + k][1].strip()
        if g != text:
            return False
    return True


def first_diff(exp, got, j):
    for k, (text, kind, src) in enumerate(exp):
        if j + k >= len(got):
            return k, text, src, "<end>"
        g = got[j + k][1] if kind == "frozen" else got[j + k][1].strip()
        if g != text:
            return k, text, src, f"{got[j + k][0]}: {got[j + k][1]}"
    return None


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
                    body = [(n, lines[n]) for n in range(open_i + 1, i) if lines[n].strip()]
                    return body, set(range(starts[0], open_i + 1)) | {i}
        if open_i is None and lines[i].rstrip().endswith(";"):
            raise ValueError(f"{regex!r} matched a declaration")
    raise ValueError("unbalanced braces")


# --- check 5: log calls and member calls -----------------------------------------------------
LOG_RE = re.compile(r"->\s*(trace|debug|info|warn|error|critical)\s*\(")
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def literal_after(text, pos):
    out = []
    for m in _CPP.finditer(text, pos):
        if m.lastgroup == "other" and m.group().isspace():
            continue
        if m.lastgroup == "str":
            out.append(m.group())
            continue
        break
    return "".join(out)


def events(body_text, members):
    """Ordered (log, level, literal) and (call, name) events of a comment-stripped body."""
    ev = []
    blanked = blank_literals(body_text)
    marks = []
    for m in LOG_RE.finditer(blanked):
        marks.append((m.start(), ("log", m.group(1), literal_after(body_text, m.end()))))
    for m in CALL_RE.finditer(blanked):
        if m.group(1) in members:
            marks.append((m.start(), ("call", m.group(1))))
    for _, e in sorted(marks, key=lambda x: x[0]):
        ev.append(e)
    return ev


def function_texts(text):
    """name -> comment-stripped body text, for every MwSlipDetector:: definition (and the
    file's free helpers)."""
    code = strip_comments(text)
    blank = blank_literals(code)
    out = {}
    depth, i, n = 0, 0, len(blank)
    while i < n:
        ch = blank[i]
        if ch == "{":
            # the definition head: text since the previous ; } or { at this depth
            head_start = max(blank.rfind(";", 0, i), blank.rfind("}", 0, i), blank.rfind("{", 0, i))
            head = blank[head_start + 1:i]
            mm = re.search(r"MwSlipDetector::(~?\w+)\s*\(", head) or \
                re.search(r"(\w+)\s*\([^()]*\)\s*(const\s*)?$", head.strip())
            d, j = 0, i
            while j < n:
                if blank[j] == "{":
                    d += 1
                elif blank[j] == "}":
                    d -= 1
                    if d == 0:
                        break
                j += 1
            if mm and "namespace" not in head and depth == 1:
                out[mm.group(1)] = code[i + 1:j]
                i = j + 1
                continue
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1
    return out


def run_checks(tree, verbose):
    st = stage(tree)
    print(f"stage U5m-{st}; frozen {FROZEN}: {FCPP}, {FHPP}; new files under {tree}")
    units, subs = units_for(st)
    plain = code_lines(git_show(FROZEN, FCPP))
    drops = {n: r for a, b, r in DROP_RANGES for n in range(a, b + 1) if plain[n].strip()}
    if st in ("c", "d"):
        drops.update(ELEV_DROPS)
    if st == "d":
        drops.update(DROPS_D)
        drops = {n: r for n, r in drops.items() if plain[n].strip()}
    FC = Frozen(git_show(FROZEN, FCPP), subs, OUT_RULES if st == "d" else (),
                OUT_RANGE if st == "d" else (0, 0))
    FH_text = git_show(FROZEN, FHPP)
    cpp_files = sorted({u["file"] for u in units if u["file"].endswith(".cpp")})
    files = {f: read(tree, f) for f in sorted({u["file"] for u in units})}
    fails = 0
    claims = {}
    matched = {}

    print("\n1. Units")
    for unit in units:
        problems = []
        exp = expected(unit, FC, drops, claims, problems)
        text = files[unit["file"]]
        ok = not problems
        try:
            if unit["kind"] == "body":
                got, head = locate_body(text, unit["find"])
                matched.setdefault(unit["file"], set()).update(head)
                matched[unit["file"]].update(n for n, _ in got)
                if "sig" in unit:
                    s0, s1 = unit["sig"]
                    want_sig = [FC.code[i].strip() for i in range(s0, s1 + 1) if FC.code[i].strip()]
                    nl = code_lines(text)
                    got_sig = [nl[i].strip() for i in sorted(head) if nl[i].strip()][:-1]
                    if want_sig != got_sig:
                        problems.append(f"signature differs: want {want_sig}, got {got_sig}")
                    for i in list(range(s0, s1 + 1)) + [unit["close"]]:
                        if FC.code[i].strip():
                            claims.setdefault(i, []).append(unit["name"])
                d = first_diff(exp, got, 0)
                if d is not None or len(got) != len(exp):
                    if d is None:
                        d = (len(exp), "<end of unit>", None,
                             f"{got[len(exp)][0]}: {got[len(exp)][1]}")
                    problems.append(f"line {d[0]} of the body differs\n  want {d[1]!r} "
                                    f"(frozen {d[2]})\n  got  {d[3]!r}")
            else:
                lines = code_lines(text)
                got = [(n, lines[n]) for n in range(1, len(lines)) if lines[n].strip()]
                hits = [j for j in range(len(got)) if match_at(exp, got, j)]
                if len(hits) == 1:
                    matched.setdefault(unit["file"], set()).update(
                        got[hits[0] + k][0] for k in range(len(exp)))
                if len(hits) != 1:
                    best = None
                    first = exp[0][0] if exp else ""
                    cands = [j for j in range(len(got)) if
                             (got[j][1] if exp and exp[0][1] == "frozen" else got[j][1].strip()) == first]
                    for j in cands:
                        d = first_diff(exp, got, j)
                        if d and (best is None or d[0] > best[0]):
                            best = d
                    problems.append(f"found {len(hits)} times in {unit['file']}"
                                    + (f"; best candidate differs at unit line {best[0]}:\n"
                                       f"  want {best[1]!r} (frozen {best[2]})\n  got  {best[3]!r}"
                                       if best else ""))
        except ValueError as ex:
            problems.append(str(ex))
        ok = not problems
        if verbose or not ok:
            nf = sum(1 for e in exp if e[1] == "frozen")
            nn = sum(1 for e in exp if e[1] == "new")
            print(f"  {'PASS' if ok else 'FAIL'}  {unit['name']}  "
                  f"[{pathlib.Path(unit['file']).name}: {nf} frozen, {nn} new]")
            for p in problems:
                print("        " + p.replace("\n", "\n        "))
        fails += 0 if ok else 1
    print(f"  {len(units) - fails}/{len(units)} units PASS")

    print("\n2. Coverage of the frozen MwSlipDetector.cpp")
    cov = []
    for i in range(1, len(FC.plain)):
        if not FC.plain[i].strip():
            continue
        owners = list(claims.get(i, []))
        if i in FURNITURE:
            owners.append("furniture")
        if i in drops and not owners:
            owners.append("drop")
        if len(owners) != 1:
            cov.append(f"frozen {i} claimed {len(owners)} times {owners}: {FC.plain[i].strip()!r}")
    for i in drops:
        if not FC.plain[i].strip():
            cov.append(f"declared drop {i} is not a code line")
    n_code = sum(1 for l in FC.plain[1:] if l.strip())
    print(f"  {n_code} code lines; {len(drops)} declared drops; {len(FURNITURE)} furniture; "
          f"{'PASS' if not cov else 'FAIL'}")
    for p in cov[:40]:
        print("        " + p)
    if len(cov) > 40:
        print(f"        ... {len(cov) - 40} more")

    print("\n3. Header")
    FH = code_lines(FH_text)
    hdr_drops = dict(HDR_DROPS)
    if st == "d":
        hdr_drops.update(HDR_DROPS_D)
    want = Counter()
    for i in range(1, len(FH)):
        line = FH[i]
        if not line.strip() or i in hdr_drops:
            continue
        for rx, fr, to, _ in HDR_RULES:
            if re.search(rx, line):
                line = line.replace(fr, to)
        if not re.match(r"\s*struct ", line):
            for t in MW_TYPES:
                line = re.sub(r"(?<![\w:])" + t + r"\b", "mw::" + t, line)
        want[collapse(line)] += 1
    hdrs = HDRS + ([WORK_HDR] if st == "d" else [])
    got = Counter()
    for h in hdrs:
        hl = code_lines(read(tree, h))
        carried = matched.get(h, set())   # lines a source unit carries (the SD group entry)
        for n in range(1, len(hl)):
            if hl[n].strip() and n not in carried:
                got[collapse(hl[n])] += 1
    new = Counter(collapse(x) for x in HDR_NEW + HDR_NEW_B
                  + (HDR_NEW_C if st in ("c", "d") else []) + (HDR_NEW_D if st == "d" else []))
    hdr_problems = []
    missing = want - got
    extra = (got - want) - new
    unused_new = new - (got - want)
    for line, n in sorted(missing.items()):
        hdr_problems.append(f"frozen header line not carried ({n}x): {line}")
    for line, n in sorted(extra.items()):
        hdr_problems.append(f"new header line not declared ({n}x): {line}")
    for line, n in sorted(unused_new.items()):
        hdr_problems.append(f"declared new header line not present ({n}x): {line}")
    print(f"  {sum(want.values())} frozen code lines carried, {sum(new.values())} declared new; "
          f"{'PASS' if not hdr_problems else 'FAIL'}")
    for p in hdr_problems:
        print("        " + p)

    print("\n4. Comments")
    fc_text = git_show(FROZEN, FCPP)
    frozen_c = Counter(comment_lines(fc_text)) + Counter(comment_lines(FH_text))
    cdrops = {k: list(v) + (COMMENT_DROPS_D[k] if st == "d" else [])
              for k, v in COMMENT_DROPS.items()}
    allowed = Counter(comment_lines(fc_text, cdrops["cpp"])) \
        + Counter(comment_lines(FH_text, cdrops["hpp"]))
    new_c = Counter()
    for f in cpp_files + hdrs:
        new_c.update(comment_lines(read(tree, f)))
    lost = []
    for c, n in frozen_c.items():
        short = n - new_c[c]
        if short > allowed[c]:
            lost.append((c, short - allowed[c]))
    print(f"  {sum(frozen_c.values())} frozen comment lines; {sum(allowed.values())} in declared "
          f"comment drops; {len(lost)} lost; {'PASS' if not lost else 'FAIL'}")
    for c, n in lost[:30]:
        print(f"        comment line lost ({n}x): {c[:110]}")

    print("\n5. Log calls and member calls, in order, per frozen function")
    ffun = function_texts(fc_text)
    nfun = {}
    for f in cpp_files:
        for k, v in function_texts(files[f]).items():
            if k in nfun and k != "MwSlipDetector":
                print(f"        note: {k} defined twice")
            nfun[k] = v
    members = set(ffun) | set(nfun)
    # The step functions of U5m-d: every new definition that is not a frozen one.  The
    # anonymous-namespace helper mwElevationWeight is a member now (same calls on both sides).
    steps = set(nfun) - set(ffun) - {"mwElevationWeight"}
    ev_problems = []

    def expand(name, seen=()):
        out = []
        for e in events(nfun.get(name, ""), members):
            if e[0] == "call" and e[1] in steps and e[1] not in seen:
                out += expand(e[1], seen + (e[1],))
            else:
                out.append(e)
        return out

    n_log = 0
    gone = {"getSatelliteMwQuality", "findIonoFreeLabels"}
    for name, body in ffun.items():
        if name in gone:
            continue
        fev = [e for e in events(body, members) if not (e[0] == "call" and e[1] in gone)]
        n_log += sum(1 for e in fev if e[0] == "log")
        if name not in nfun:
            ev_problems.append(f"{name}: no new definition")
            continue
        nev = expand(name)
        if fev != nev:
            k = next((k for k in range(min(len(fev), len(nev))) if fev[k] != nev[k]),
                     min(len(fev), len(nev)))
            ev_problems.append(f"{name}: differs at event {k} of {len(fev)}/{len(nev)}\n"
                               f"  frozen {fev[k] if k < len(fev) else '<end>'}\n"
                               f"  new    {nev[k] if k < len(nev) else '<end>'}")
    print(f"  {len(ffun) - len(gone)} frozen functions, {n_log} log calls; "
          f"{len(steps)} step functions expanded; {'PASS' if not ev_problems else 'FAIL'}")
    for p in ev_problems:
        print("        " + p.replace("\n", "\n        "))

    print("\n6. Undeclared code in the new source files")
    undeclared = []
    for f in cpp_files:
        lines = code_lines(files[f])
        seen = matched.get(f, set())
        for n in range(1, len(lines)):
            line = lines[n]
            if not line.strip() or n in seen:
                continue
            if any(re.fullmatch(rx, line) for rx in CPP_FURNITURE_RX):
                continue
            undeclared.append(f"{pathlib.Path(f).name}:{n}: {line.strip()[:100]}")
    print(f"  {len(cpp_files)} files; {len(undeclared)} undeclared code lines; "
          f"{'PASS' if not undeclared else 'FAIL'}")
    for p in undeclared[:30]:
        print("        " + p)

    print("\nDeclared, for review:")
    for i, old, new, why in FC.rule_hits:
        print(f"  rule   frozen {i}: {old!r} -> {new!r} ({why})")
    for i in sorted(subs):
        for fr, to in subs[i]:
            print(f"  subst  frozen {i}: {fr!r} -> {to!r}")
    for unit in units:
        for p in unit["pieces"]:
            if p[0] == "G":
                print(f"  guard  {unit['name']}: frozen {p[1]}..{p[2]} -> {p[3]!r}")
            if p[0] == "U":
                print(f"  unwrap {unit['name']}: frozen {p[1]}..{p[2]} (out-parameter check)")
    reasons = Counter(drops.values())
    for r, n in reasons.items():
        print(f"  drop   {n:3d} line(s): {r}")
    for i, r in sorted(hdr_drops.items()):
        print(f"  header drop {i}: {r}")
    for rx, fr, to, why in HDR_RULES:
        print(f"  header rule {fr!r} -> {to!r}: {why}")
    print(f"  header rule: class code lines name {', '.join(MW_TYPES)} as mw::<type>")

    total = fails + (1 if cov else 0) + (1 if hdr_problems else 0) + (1 if lost else 0) \
        + (1 if ev_problems else 0) + (1 if undeclared else 0)
    print(f"\nTOTAL: {len(units) - fails}/{len(units)} units PASS; coverage "
          f"{'PASS' if not cov else 'FAIL'}; header {'PASS' if not hdr_problems else 'FAIL'}; "
          f"comments {'PASS' if not lost else 'FAIL'}; logs {'PASS' if not ev_problems else 'FAIL'}; "
          f"undeclared code {'PASS' if not undeclared else 'FAIL'}"
          f" -> {'PASS' if total == 0 else 'FAIL'}")
    return total == 0


def mutate(verbose, src=ROOT):
    """Change one character of a carried code line in a scratch copy; the checks must fail."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        st = stage(src)
        units, _ = units_for(st)
        files = sorted({u["file"] for u in units}) + HDRS + ([WORK_HDR] if st == "d" else [])
        for f in files:
            (tmp / f).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src / f, tmp / f)
        target = tmp / (STATS if st in ("c", "d") else MW)
        text = target.read_text(encoding="utf-8")
        old = "return sigma_xsat * mwElevationWeight(elev_deg);"
        assert text.count(old) == 1, "mutation site not found once"
        target.write_text(text.replace(old, "return sigma_xsat / mwElevationWeight(elev_deg);"),
                          encoding="utf-8")
        print(f"mutation: {target.relative_to(tmp)}: '*' -> '/' in crossSatSigmaFloor's return")
        ok = run_checks(tmp, verbose)
        print("MUTATION", "DETECTED (the copy fails)" if not ok else "NOT DETECTED")
        return not ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tree", help="read the new files from this directory")
    ap.add_argument("--mutate", action="store_true")
    a = ap.parse_args()
    tree = pathlib.Path(a.tree).resolve() if a.tree else ROOT
    if a.mutate:
        return 0 if mutate(a.verbose, tree) else 1
    return 0 if run_checks(tree, a.verbose) else 1


if __name__ == "__main__":
    sys.exit(main())
