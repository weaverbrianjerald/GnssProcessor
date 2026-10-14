#!/usr/bin/env python3
"""Relocation verifier for card UOM step U2: the KalmanFilter organized by concern.

    python scripts/uom_u2_verify.py [--verbose] [--tree DIR]

The frozen source is src/core/filter/KalmanFilter.cpp at 5154324 (card UOM pre-registered),
read with `git show` (no copy is stored).  Comments and blank lines are ignored on both sides
(the lexer of scripts/rt_seam_verify.py), so a rewritten comment never fails the check; every
other character of a carried line is compared, indentation included.

The manifest below records the whole step against that one frozen file: U2a's deletions
(DROPS), U2b's relocation (one unit per moved item), U2c's stages (alias blocks, published
results, and the braces and guards the driver replaces, as DROPS), U2d's detector table (the
loops it rewrites, REWRITTEN, and the statements it carries, with their SUBS).  U2c's
generator (KalmanPostFitQc.cpp was written from this manifest) and this check read one list.

A unit is one item of a new file (a function, a struct, a constant) written as an ordered
list of pieces:

  ("frozen", a, b, shift)   the frozen code lines a..b, `shift` leading spaces removed, after
                            the declared substitutions of SUBS (applied to the frozen text
                            before its comments are stripped);
  ("new", text)             one declared new code line (compared without its indentation and
                            its trailing comment);
  ("alias", [names])        one line per name binding the name the code had as a local of
  ("alias_rw", [names])     the single function to the stage's work value, `... name =
                            w.name;` (the binding check: the alias and the member are the
                            same name; alias_rw is the writable form);
  ("publish", [names])      one line per name, `w.name = name;`, handing a stage's local to
                            the later stages;
  ("comment", text)         text the generator writes and the check ignores.

Exit 0 only when every check passes:

  1. Units.  Each unit is found in its declared file by its first code lines, which occur
     once in that file and in no other file of the set; its code lines equal the pieces one
     for one; and they close the item (balanced braces, the last line ends the item).
  2. Coverage.  Every code line of the frozen file is carried by exactly one unit, or lies in
     SCAFFOLD (the includes and namespace lines each new file writes for itself), or is a
     declared drop (DROPS) or a declared rewrite (REWRITTEN), each with its reason.  None is
     dropped silently, none carried twice.
  3. Nothing added.  Outside its units a new .cpp file holds only #include lines, namespace
     lines and their closing braces and `using kalman::<name>;` declarations; a header also
     holds declarations that end in `;` without a body, and the types and tables of
     DECLARED_TYPES.
  4. Literals.  The string literals (the log formats among them) over the code of the new
     files are exactly those of the carried lines, the declared new lines and the declared
     types; and every literal of a rewritten line still occurs in the new files, so a
     rewrite loses no log text.

A carried line is compared as text: the check proves where each line now lives and that it
is unchanged.  The includes of each new file are new, and the compiler proves them
sufficient; the gate (scripts/uom_gate.py) proves the output unchanged, and for U2d, where
the four detector loops are rewritten as one function over a table, it is the proof that the
arithmetic is the same.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

FROZEN_REV = "5154324"
FROZEN_PATH = "src/core/filter/KalmanFilter.cpp"
F = "src/core/filter/"
H = "include/filter/"
FILES = [F + "KalmanFilter.cpp", F + "KalmanKernels.cpp", F + "KalmanResidualQc.cpp",
         F + "KalmanPostFitQc.cpp", F + "KalmanOutlierResponse.cpp",
         H + "KalmanKernels.hpp", H + "KalmanRowIdentity.hpp", H + "KalmanPostFitQc.hpp"]

# Frozen lines each new file replaces with its own: the includes, `namespace GnssProcessor {`,
# the anonymous namespace's braces, the namespace close.
SCAFFOLD = [(11, 25), (27, 27), (33, 33), (206, 206), (3240, 3240)]

# Frozen lines deleted on purpose, with the reason.
DROPS = [
    ((492, 591), "U2a: KalmanFilter::predictImu has no caller (the Grep tool and a grep over "
                 "src, include, tests and python find only its definition and declaration)"),
    ((1746, 1746), "U2a: the local epoch_tag of validatePostFitResiduals is never read"),
    ((1937, 1937), "U2c: the group-detector block's opening brace (the block is detectPivotOffsets_ "
                   "and detectNonPivotSlips_ now)"),
    ((2496, 2496), "U2c: the non-pivot block's opening brace (its interior is detectNonPivotSlips_)"),
    ((2640, 2640), "U2c: the non-pivot block's closing brace"),
    ((2642, 2642), "U2c: the E2 guard `if (any_pivot_slip) {`, now the driver's "
                   "`if (pivot_offset || nonpivot_slip)`"),
    ((2648, 2649), "U2c: the E2 guard's and the group-detector block's closing braces"),
    ((2657, 2657), "U2c: the Huber block's opening brace (its interior is queueHuberInflations_)"),
    ((2741, 2741), "U2c: the E3 guard `if (huber_triggered) {`, now the driver's "
                   "`if (queueHuberInflations_(w))`"),
    ((2749, 2750), "U2c: the E3 guard's and the Huber block's closing braces"),
    ((2789, 2790), "U2c: the locals is_outlier_found and is_complete_reset are members of "
                   "PostFitQcWork, initialized false"),
]

# Frozen lines rewritten on purpose: every code line of a range that no unit carries is
# replaced by the declared new code of the units, with the reason.  A rewrite may not lose a
# string literal: each literal of a rewritten line must still occur in the new files.
REWRITTEN = [
    ((1938, 2476), "U2d: the four coherent-offset detector loops, with their local constants, "
                   "the Group type, the group maps, the code grouping loop and the lambdas, are "
                   "one detector over kalman::kPivotDetectors: testCoherentOffset (the four "
                   "criteria, the loops' expressions in their order), testPivotGroup_ (one row "
                   "on one group), groupPivotRows_ (one grouping for both observables) and the "
                   "members they call; the statements they carry are units"),
    ((2594, 2598), "U2d: the non-pivot detector's band join is a call to joinBands"),
]

# Declared substitutions on single frozen lines: (line, old text, new text).
SUBS = [
    (199, "    std::pair<std::string, std::string> splitBssdSatId(",
          "    inline std::pair<std::string, std::string> splitBssdSatId("),
    # U2c: the lambda medianInPlace is a file-scope function, as both group stages call it.
    (2038, "auto medianInPlace = [](std::vector<double>& v) -> double {",
           "double medianInPlace(std::vector<double>& v) {"),
    (2045, "};", "}"),
    # U2c: E1 and E4 sit in the driver, which reads the work value by its members.
    (1757, "if (num_obs == 0) {", "if (w.num_obs == 0) {"),
    (1758, "klogger->warn(", "w.klogger->warn("),
    (1758, ", epoch_prefix);", ", w.epoch_prefix);"),
    (3110, "return { is_outlier_found, is_complete_reset };",
           "return { w.is_outlier_found, w.is_complete_reset };"),
    # U2c: the non-pivot stage's own flag is named for what it records.
    (2638, "any_pivot_slip = true;", "any_nonpivot_slip = true;"),
    # U2d: the phase grouping loop serves both observables (groupPivotRows_).
    (2012, "sig[0] != 'L'", "sig[0] != obs_type"),
    (2015, "row_atx_substituted(", "rowAtxSubstituted_("),
    (2024, "groups[{ pivot, sig }]", "rows.per_signal[{ pivot, sig }]"),
    (2029, "pivot_pools[pivot]", "rows.pooled[pivot]"),
    (2033, "pivot_pool_bands[pivot].insert(band);", "pp.bands.insert(band);"),
    # U2d: the handover-exclusion band's constants are declared beside the table.
    (2060, "HANDOVER_EXCLUDE_MAD_K", "kHandoverExcludeMadK"),
    (2061, "HANDOVER_EXCLUDE_FLOOR_M", "kHandoverExcludeFloorM"),
    # U2d: the wavelength floor reads the row's tight-cluster ratio and the group's signal,
    # and answers instead of continuing the loop.
    (2126, "TIGHT_CLUSTER_MAD_RATIO", "spec.tight_cluster_mad_ratio"),
    (2129, "key.second.substr(0, 2)", "signal.substr(0, 2)"),
    (2140, "pivot_sat, key.second,", "pivot_sat, signal,"),
    (2141, "TIGHT_CLUSTER_MAD_RATIO", "spec.tight_cluster_mad_ratio"),
    (2142, "continue;", "return true;"),
    # U2d: the four report lines read the group by one name and the signal by its parameter.
    (2203, "key.second /* signal */,", "signal,"),
    (2285, "pp.above,", "g.above,"),
    (2406, "key.second /* signal */,", "signal,"),
    (2463, "pp.above,", "g.above,"),
    # U2d: the lambda is the member recordHandoverExcludes_, called with the group's statistics.
    (2216, "recordHandoverExcludes(g, med, mad,", "recordHandoverExcludes_(w, g, t.med, t.mad,"),
]

# Declarations a header may hold outside the units (new types and tables, no function
# bodies): header -> regexes of the declaration's first code line.
DECLARED_TYPES = {
    H + "KalmanPostFitQc.hpp": [r"^struct KalmanFilter::PostFitQcWork \{$",
                                r"^struct (PivotDetectorSpec|PivotRowGroup|PivotRowGroups|"
                                r"CoherentOffset) \{$",
                                r"^inline constexpr PivotDetectorSpec kPivotDetectors\[\] = \{$"],
}


def fz(a, b=None, shift=0):
    return ("frozen", a, a if b is None else b, shift)


UNITS = [
    ("bandWavelengthForSat", H + "KalmanRowIdentity.hpp", [fz(42, 80)]),
    ("splitBssdSatId", H + "KalmanRowIdentity.hpp", [fz(199, 204)]),
    ("SparseHRow", H + "KalmanKernels.hpp", [fz(95, 98)]),
    ("kHSparseThreshold", F + "KalmanKernels.cpp", [fz(93)]),
    ("buildSparseH", F + "KalmanKernels.cpp", [fz(101, 116)]),
    ("sparseHP", F + "KalmanKernels.cpp", [fz(119, 132)]),
    ("sparseHPHt", F + "KalmanKernels.cpp", [fz(135, 152)]),
    ("sparseKH", F + "KalmanKernels.cpp", [fz(156, 169)]),
    ("sparseHv", F + "KalmanKernels.cpp", [fz(172, 186)]),
    ("spliceAndPropagate", F + "KalmanKernels.cpp", [fz(597, 634)]),
    ("josephPseudoUpdate", F + "KalmanKernels.cpp", [fz(3211, 3228)]),
    ("modeLabel", F + "KalmanFilter.cpp", [fz(188, 190)]),
    ("KalmanFilter()", F + "KalmanFilter.cpp", [fz(299, 385)]),
    ("~KalmanFilter()", F + "KalmanFilter.cpp", [fz(387)]),
    ("predict", F + "KalmanFilter.cpp", [fz(393, 486)]),
    ("predictAided", F + "KalmanFilter.cpp", [fz(636, 664)]),
    ("update", F + "KalmanFilter.cpp", [fz(670, 1060)]),
    ("updateFromPrior", F + "KalmanFilter.cpp", [fz(1066, 1172)]),
    ("takeRequestedPivotHandovers", F + "KalmanFilter.cpp", [fz(3113, 3117)]),
    ("takeRequestedPivotHandoverExcludeSats", F + "KalmanFilter.cpp", [fz(3119, 3124)]),
    ("takeRequestedPivotHandoverIonoReinflateSats", F + "KalmanFilter.cpp", [fz(3126, 3131)]),
    ("beginEpoch", F + "KalmanFilter.cpp", [fz(3133, 3154)]),
    ("endEpoch", F + "KalmanFilter.cpp", [fz(3156, 3171)]),
    ("logKalmanFilterTimings", F + "KalmanFilter.cpp", [fz(3173, 3205)]),
    ("applyPseudoObservation", F + "KalmanFilter.cpp", [fz(3230, 3238)]),
    ("computeResidualQc_", F + "KalmanResidualQc.cpp", [fz(1609, 1641)]),
    ("residualQcKernel", F + "KalmanResidualQc.cpp", [fz(1643, 1736)]),
    # U2c: validatePostFitResiduals as an ordered driver over named stages.
    ("medianInPlace", F + "KalmanPostFitQc.cpp", [
        ("comment", "    /**"),
        ("comment", "     * @brief Median of @p v, reordering it (nth_element); an even count averages"),
        ("comment", "     *        the two middle values."),
        ("comment", "     *"),
        ("comment", "     * Shared by the coherent-offset group detectors (card UOM step U2c made the"),
        ("comment", "     * lambda of the single function a file-scope function)."),
        ("comment", "     *"),
        ("comment", "     * @param v Values; reordered in place.  Must not be empty."),
        ("comment", "     * @return The median."),
        ("comment", "     */"),
        fz(2038, 2045, 8)]),
    ("validatePostFitResiduals", F + "KalmanPostFitQc.cpp", [
        fz(1742, 1743),
        ("comment", ""),
        ("comment", "        // The post-fit outlier screen as an ordered list of stages over one working"),
        ("comment", "        // value (include/filter/KalmanPostFitQc.hpp).  Four exits: E1 no rows; E2 a"),
        ("comment", "        // group detector rejected rows (counted as a bad residual, and returns before"),
        ("comment", "        // the complete-reset backstop); E3 the Huber pass queued R targets (not"),
        ("comment", "        // counted, before the backstop); E4 the dispatch, the backstop and the clean"),
        ("comment", "        // commit ran."),
        ("new", "        PostFitQcWork w = beginPostFitQc_(measurement, x, P);              // S0"),
        ("comment", "        // E1: nothing to screen."),
        fz(1757, 1760),
        ("comment", ""),
        ("new", "        reportPostFitResiduals_(w);                                          // S1"),
        ("new", "        reportStatisticalQc_(w);                                             // S2"),
        ("comment", ""),
        ("comment", "        // S3: the group detectors; both always run, and any rejection is E2."),
        ("new", "        const bool pivot_offset  = detectPivotOffsets_(w);"),
        ("new", "        const bool nonpivot_slip = detectNonPivotSlips_(w);"),
        ("new", "        if (pivot_offset || nonpivot_slip) return finishGroupRejection_(w);  // E2"),
        ("comment", ""),
        ("new", "        if (queueHuberInflations_(w)) return finishHuberRebuild_(w);        // S4, E3"),
        ("comment", ""),
        ("new", "        selectGateRow_(w);                                                   // S5"),
        ("new", "        dispatchWorstRow_(w);                                                // S6"),
        ("new", "        completeResetBackstop_(w);                                           // S7"),
        ("new", "        logPostFitResult_(w);                                                // S8"),
        ("new", "        recordCleanCommit_(w);                                               // S9"),
        ("comment", "        // E4."),
        fz(3110, 3111)]),
    ("beginPostFitQc_", F + "KalmanPostFitQc.cpp", [
        ("new", "    KalmanFilter::PostFitQcWork KalmanFilter::beginPostFitQc_("),
        ("new", "        EpochMeasurement& measurement, Eigen::VectorXd& x, Eigen::MatrixXd& P) {"),
        ("comment", ""),
        fz(1745, 1756),
        ("comment", ""),
        fz(1761, 1775),
        ("comment", ""),
        ("new", "        return PostFitQcWork{ measurement, x, P, klogger, epoch_prefix, threshold,"),
        ("new", "                              bad_meas_percent, res, vres, sres, num_obs,"),
        ("new", "                              total_obs_count, meta_n };"),
        ("new", "    }")]),
    ("reportPostFitResiduals_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::reportPostFitResiduals_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "P", "klogger", "epoch_prefix", "threshold", "bad_meas_percent",
                   "res", "vres", "sres", "num_obs", "meta_n"]),
        ("comment", ""),
        fz(1777, 1863),
        ("comment", ""),
        ("publish", ["max_idx", "max_sres", "residual_summary_level", "sx", "sy", "sz"]),
        ("new", "    }")]),
    ("reportStatisticalQc_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::reportStatisticalQc_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "klogger", "epoch_prefix", "sres", "num_obs",
                   "residual_summary_level", "sx", "sy", "sz"]),
        ("comment", ""),
        fz(1865, 1905),
        ("new", "    }")]),
    ("detectNonPivotSlips_", F + "KalmanPostFitQc.cpp", [
        ("new", "    bool KalmanFilter::detectNonPivotSlips_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "P", "klogger", "epoch_prefix", "threshold", "res", "sres",
                   "meta_n"]),
        ("comment", ""),
        fz(2478, 2495, 4),
        ("new", "        bool any_nonpivot_slip = false;"),
        fz(2497, 2593, 8),
        ("new", "            const std::string bands_str = joinBands(e.bands);"),
        fz(2599, 2639, 8),
        ("comment", ""),
        ("new", "        return any_nonpivot_slip;"),
        ("new", "    }")]),
    ("finishGroupRejection_", F + "KalmanPostFitQc.cpp", [
        ("new", "    std::pair<bool, bool> KalmanFilter::finishGroupRejection_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "klogger", "epoch_prefix"]),
        fz(2643, 2647, 8),
        ("new", "    }")]),
    ("queueHuberInflations_", F + "KalmanPostFitQc.cpp", [
        ("new", "    bool KalmanFilter::queueHuberInflations_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "P", "klogger", "epoch_prefix", "res", "sres", "num_obs"]),
        ("comment", ""),
        fz(2651, 2656),
        fz(2658, 2739, 4),
        ("comment", ""),
        ("new", "        return huber_triggered;"),
        ("new", "    }")]),
    ("finishHuberRebuild_", F + "KalmanPostFitQc.cpp", [
        ("new", "    std::pair<bool, bool> KalmanFilter::finishHuberRebuild_(PostFitQcWork& w) {"),
        ("alias", ["klogger", "epoch_prefix"]),
        fz(2742, 2748, 8),
        ("new", "    }")]),
    ("selectGateRow_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::selectGateRow_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "sres", "threshold", "max_idx", "max_sres"]),
        ("comment", ""),
        fz(2752, 2786),
        ("comment", ""),
        ("publish", ["gate_stat", "gate_idx", "gate_crit"]),
        ("new", "    }")]),
    ("dispatchWorstRow_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::dispatchWorstRow_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "klogger", "epoch_prefix", "threshold", "res", "vres", "sres",
                   "max_sres", "gate_stat", "gate_crit"]),
        ("alias_rw", ["is_outlier_found"]),
        ("comment", ""),
        fz(2788, 2788),
        fz(2792, 2800),
        ("comment", "            // MW tiebreaker for competing phase outliers (chooseOutlierRow_)."),
        ("new", "            const size_t chosen_idx = chooseOutlierRow_(w);"),
        ("comment", ""),
        fz(2884, 2902),
        ("comment", "                // The sub-cycle floor, the reset and the correlated rows on the"),
        ("comment", "                // satellite's other phase signals (respondToPhaseOutlier_)."),
        ("new", "                respondToPhaseOutlier_(w, chosen_idx, outlier_sat, outlier_sig, outlier_res);"),
        fz(2991, 3005),
        ("new", "    }")]),
    ("chooseOutlierRow_", F + "KalmanPostFitQc.cpp", [
        ("new", "    size_t KalmanFilter::chooseOutlierRow_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "klogger", "epoch_prefix", "threshold", "sres", "meta_n",
                   "max_idx", "max_sres", "gate_idx"]),
        ("comment", ""),
        fz(2801, 2883, 4),
        ("comment", ""),
        ("new", "        return chosen_idx;"),
        ("new", "    }")]),
    ("respondToPhaseOutlier_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::respondToPhaseOutlier_(PostFitQcWork& w, size_t chosen_idx,"),
        ("new", "        const std::string& outlier_sat, const std::string& outlier_sig, double outlier_res) {"),
        ("alias", ["measurement", "x", "P", "klogger", "epoch_prefix", "threshold", "res", "sres",
                   "meta_n"]),
        ("comment", ""),
        fz(2903, 2990, 8),
        ("new", "    }")]),
    ("completeResetBackstop_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::completeResetBackstop_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "P", "klogger", "epoch_prefix", "bad_meas_percent",
                   "total_obs_count"]),
        ("alias_rw", ["is_outlier_found", "is_complete_reset"]),
        ("comment", ""),
        fz(3007, 3021),
        ("new", "    }")]),
    ("logPostFitResult_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::logPostFitResult_(PostFitQcWork& w) {"),
        ("alias", ["klogger", "epoch_prefix", "is_outlier_found", "is_complete_reset"]),
        ("comment", ""),
        fz(3023, 3026),
        ("new", "    }")]),
    ("recordCleanCommit_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::recordCleanCommit_(PostFitQcWork& w) {"),
        ("alias", ["measurement", "klogger", "epoch_prefix", "res", "meta_n", "is_outlier_found",
                   "is_complete_reset"]),
        ("comment", ""),
        fz(3028, 3045),
        ("comment", "            // Stuck-code-bias scan (queueStuckCodeBiasResets_)."),
        ("new", "            queueStuckCodeBiasResets_(w);"),
        ("comment", ""),
        fz(3094, 3108),
        ("new", "    }")]),
    ("queueStuckCodeBiasResets_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::queueStuckCodeBiasResets_(PostFitQcWork& w) {"),
        ("alias", ["klogger", "epoch_prefix"]),
        ("comment", ""),
        fz(3046, 3093, 4),
        ("new", "    }")]),
    ("isUnsetVariance", F + "KalmanOutlierResponse.cpp", [fz(194, 196)]),
    ("seedCovarianceDiagonals", F + "KalmanOutlierResponse.cpp", [fz(212, 293)]),
    ("handlePhaseOutlier", F + "KalmanOutlierResponse.cpp", [fz(1178, 1317)]),
    ("handleBaselinePhaseOutlier", F + "KalmanOutlierResponse.cpp", [fz(1322, 1411)]),
    ("handleCodeOutlier", F + "KalmanOutlierResponse.cpp", [fz(1413, 1464)]),
    ("resetFilterCovariance", F + "KalmanOutlierResponse.cpp", [fz(1466, 1603)]),
    # U2d: the four coherent-offset pivot detectors as one detector over kalman::kPivotDetectors.
    ("testCoherentOffset", F + "KalmanPostFitQc.cpp", [
        ("new", "    CoherentOffset testCoherentOffset(const PivotDetectorSpec& spec, const PivotRowGroup& g) {"),
        ("new", "        CoherentOffset t;"),
        ("new", "        t.n = g.indices.size();"),
        ("new", "        if (t.n < spec.min_rows) return t;"),
        ("new", "        if (spec.pooled && g.bands.size() < spec.min_bands) return t;"),
        ("new", "        t.frac = static_cast<double>(g.above) / static_cast<double>(t.n);"),
        ("new", "        if (t.frac < spec.above_fraction_min) return t;"),
        ("new", "        std::vector<double> r_copy = g.residuals;"),
        ("new", "        t.med = medianInPlace(r_copy);"),
        ("new", "        if (std::abs(t.med) < spec.median_floor_m) return t;"),
        ("new", "        std::vector<double> abs_dev;"),
        ("new", "        abs_dev.reserve(g.residuals.size());"),
        ("new", "        for (double v : g.residuals) abs_dev.push_back(std::abs(v - t.med));"),
        ("new", "        t.mad = medianInPlace(abs_dev);"),
        ("new", "        t.mad_ratio = t.mad / std::abs(t.med);"),
        ("new", "        if (t.mad_ratio > spec.mad_ratio_max) return t;"),
        ("new", "        t.coherent = true;"),
        ("new", "        return t;"),
        ("new", "    }")]),
    ("excludeGroupRows", F + "KalmanPostFitQc.cpp", [
        ("new", "    void excludeGroupRows(EpochMeasurement& measurement, const PivotRowGroup& g) {"),
        fz(2223, 2228, 8),
        ("new", "    }")]),
    ("joinBands", F + "KalmanPostFitQc.cpp", [
        ("new", "    std::string joinBands(const std::set<std::string>& bands_set) {"),
        fz(2273, 2277, 8),
        ("new", "        return bands_str;"),
        ("new", "    }")]),
    ("detectPivotOffsets_", F + "KalmanPostFitQc.cpp", [
        ("new", "    bool KalmanFilter::detectPivotOffsets_(PostFitQcWork& w) {"),
        ("new", "        const PivotRowGroups phase_rows = groupPivotRows_(w, 'L');"),
        ("new", "        const PivotRowGroups code_rows  = groupPivotRows_(w, 'C');"),
        fz(2088, 2088, 4),
        ("new", "        for (const PivotDetectorSpec& spec : kPivotDetectors) {"),
        ("new", "            const PivotRowGroups& rows = (spec.obs_type == 'L') ? phase_rows : code_rows;"),
        ("new", "            if (spec.pooled) {"),
        ("new", "                for (const auto& [pivot_sat, g] : rows.pooled)"),
        ("new", "                    if (testPivotGroup_(w, spec, pivot_sat, std::string(), g)) any_pivot_slip = true;"),
        ("new", "            } else {"),
        ("new", "                for (const auto& [key, g] : rows.per_signal)"),
        ("new", "                    if (testPivotGroup_(w, spec, key.first, key.second, g)) any_pivot_slip = true;"),
        ("new", "            }"),
        ("new", "        }"),
        ("new", "        return any_pivot_slip;"),
        ("new", "    }")]),
    ("groupPivotRows_", F + "KalmanPostFitQc.cpp", [
        ("new", "    PivotRowGroups KalmanFilter::groupPivotRows_(const PostFitQcWork& w, char obs_type) const {"),
        ("alias", ["measurement", "threshold", "res", "sres", "meta_n"]),
        ("new", "        PivotRowGroups rows;"),
        fz(2009, 2012, 4),
        ("new", "            if (obs_type == 'C' && sig.find('_') != std::string::npos) continue;"),
        fz(2013, 2034, 4),
        ("new", "        return rows;"),
        ("new", "    }")]),
    ("rowAtxSubstituted_", F + "KalmanPostFitQc.cpp", [
        ("new", "    bool KalmanFilter::rowAtxSubstituted_(const std::string& sat, const std::string& pivot_sat,"),
        ("new", "                                          const std::string& signal) const {"),
        fz(1995, 2006, 8),
        ("new", "    }")]),
    ("testPivotGroup_", F + "KalmanPostFitQc.cpp", [
        ("new", "    bool KalmanFilter::testPivotGroup_(PostFitQcWork& w, const PivotDetectorSpec& spec,"),
        ("new", "                                       const std::string& pivot_sat, const std::string& signal,"),
        ("new", "                                       const PivotRowGroup& g) {"),
        fz(2150, 2150, 8),
        ("new", "        if (spec.pooled && requested_pivot_handovers_.count(cons)) return false;"),
        ("new", "        const CoherentOffset t = testCoherentOffset(spec, g);"),
        ("new", "        if (!t.coherent) return false;"),
        ("new", "        if (spec.wavelength_floor && belowPivotWavelengthFloor_(w, spec, pivot_sat, signal, t))"),
        ("new", "            return false;"),
        ("new", "        logPivotDetection_(w, spec, cons, pivot_sat, signal, g, t);"),
        fz(2215, 2216, 8),
        ("new", "        if (spec.reinflate_iono) recordIonoReinflate_(w, g, cons);"),
        ("new", "        excludeGroupRows(w.measurement, g);"),
        ("new", "        return true;"),
        ("new", "    }")]),
    ("belowPivotWavelengthFloor_", F + "KalmanPostFitQc.cpp", [
        ("new", "    bool KalmanFilter::belowPivotWavelengthFloor_(const PostFitQcWork& w,"),
        ("new", "                                                  const PivotDetectorSpec& spec,"),
        ("new", "                                                  const std::string& pivot_sat,"),
        ("new", "                                                  const std::string& signal,"),
        ("new", "                                                  const CoherentOffset& t) const {"),
        ("alias", ["klogger", "epoch_prefix"]),
        ("new", "        const double med = t.med;"),
        ("new", "        const double mad_ratio = t.mad_ratio;"),
        fz(1965, 1965, 4),
        fz(2126, 2145, 8),
        ("new", "        return false;"),
        ("new", "    }")]),
    ("pivotSlipConfidence_", F + "KalmanPostFitQc.cpp", [
        ("new", "    std::string KalmanFilter::pivotSlipConfidence_(const PostFitQcWork& w,"),
        ("new", "                                                   const PivotRowGroup& g) const {"),
        ("alias", ["measurement"]),
        fz(2166, 2197, 8),
        ("new", "        return confidence_tag;"),
        ("new", "    }")]),
    ("logPivotDetection_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::logPivotDetection_(const PostFitQcWork& w, const PivotDetectorSpec& spec,"),
        ("new", "                                          const std::string& cons, const std::string& pivot_sat,"),
        ("new", "                                          const std::string& signal, const PivotRowGroup& g,"),
        ("new", "                                          const CoherentOffset& t) const {"),
        ("alias", ["klogger", "epoch_prefix"]),
        ("new", "        const size_t n = t.n;"),
        ("new", "        const double med = t.med;"),
        ("new", "        const double mad = t.mad;"),
        ("new", "        const double mad_ratio = t.mad_ratio;"),
        ("new", "        const double frac = t.frac;"),
        ("new", "        if (spec.obs_type == 'L' && !spec.pooled) {"),
        ("new", "            const std::string confidence_tag = pivotSlipConfidence_(w, g);"),
        fz(2199, 2204, 4),
        ("new", "        } else if (spec.obs_type == 'L') {"),
        ("new", "            const auto& bands_set = g.bands;"),
        ("new", "            const std::string bands_str = joinBands(bands_set);"),
        fz(2279, 2285, 4),
        ("new", "        } else if (!spec.pooled) {"),
        fz(2401, 2407, 8),
        ("new", "        } else {"),
        ("new", "            const auto& bands_set = g.bands;"),
        ("new", "            const std::string bands_str = joinBands(bands_set);"),
        fz(2457, 2463, 8),
        ("new", "        }"),
        ("new", "    }")]),
    ("recordHandoverExcludes_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::recordHandoverExcludes_(const PostFitQcWork& w, const PivotRowGroup& grp,"),
        ("new", "                                               double med, double mad,"),
        ("new", "                                               const std::string& pivot_sat,"),
        ("new", "                                               const std::string& cons) {"),
        ("alias", ["measurement"]),
        fz(2058, 2067, 8),
        ("new", "    }")]),
    ("recordIonoReinflate_", F + "KalmanPostFitQc.cpp", [
        ("new", "    void KalmanFilter::recordIonoReinflate_(const PostFitQcWork& w, const PivotRowGroup& grp,"),
        ("new", "                                            const std::string& cons) {"),
        ("alias", ["measurement"]),
        fz(2080, 2085, 8),
        ("new", "    }")]),
]

ALLOWED_OUTSIDE = re.compile(r"^(#include\s.*|#pragma once|namespace \w+ \{|namespace \{|"
                             r"\}\s*|using kalman::\w+;)$")
ALIAS = re.compile(r"^(?:const\s+)?[\w:]+(?:<[^<>]*>)?\s*&?\s+(\w+)\s*=\s*w\.(\w+);$")


def blank_literals(text):
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        out.append("".join(c if c == "\n" else " " for c in s)
                   if m.lastgroup in ("str", "chr", "raw") else s)
    return "".join(out)


def literals(code_line):
    return [m.group() for m in _CPP.finditer(code_line) if m.lastgroup in ("str", "raw")]


class Source:
    """A file as comment-stripped code lines (1-based), and the same with literals blanked."""

    def __init__(self, text):
        text = text.lstrip("\ufeff").replace("\r\n", "\n")
        self.text = [""] + text.split("\n")
        self.code = [""] + strip_comments(text).split("\n")
        self.blank = [""] + blank_literals(strip_comments(text)).split("\n")

    def extent(self, i):
        """Last line of the declaration that starts at line i: the line of its first `;`
        when no `{` comes first, else the line that closes the first `{`."""
        depth, opened = 0, False
        for j in range(i, len(self.blank)):
            for ch in self.blank[j]:
                if ch == ";" and not opened:
                    return j
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
                    if opened and depth == 0:
                        return j
        raise ValueError(f"unbalanced item at line {i}")


def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8")


def in_ranges(i, ranges):
    return any(a <= i <= b for a, b in ranges)


def dropped(i):
    return any(a <= i <= b for (a, b), _ in DROPS)


def rewritten(i):
    return any(a <= i <= b for (a, b), _ in REWRITTEN)


def frozen_code_with_subs(raw, problems):
    """The frozen code lines (1-based) after the declared substitutions, which are applied to
    the raw text first, so a substitution may span a comment."""
    lines = [""] + raw.lstrip("﻿").replace("\r\n", "\n").split("\n")
    for ln, old, new in SUBS:
        if old not in lines[ln]:
            problems.append(f"substitution {old!r} not on frozen line {ln}")
            continue
        lines[ln] = lines[ln].replace(old, new)
    return [""] + strip_comments("\n".join(lines[1:])).split("\n")


def expected(code, pieces, claimed, problems, name, lits):
    """[(kind, text)] of the code lines a unit must hold, in order; adds the literals of the
    carried and the declared new lines to lits."""
    exp = []
    for p in pieces:
        kind = p[0]
        if kind == "frozen":
            _, a, b, shift = p
            for i in range(a, b + 1):
                if not code[i].strip():
                    continue
                claimed[i] += 1
                if dropped(i):
                    continue
                if code[i][:shift].strip():
                    problems.append(f"{name}: frozen line {i} has fewer than {shift} leading spaces")
                exp.append(("frozen", code[i][shift:].rstrip()))
                lits.update(literals(code[i]))
        elif kind == "new":
            text = strip_comments(p[1]).strip()
            exp.append(("new", text))
            lits.update(literals(text))
        elif kind in ("alias", "alias_rw", "publish"):
            exp.extend(("publish" if kind == "publish" else "alias", n) for n in p[1])
        elif kind != "comment":
            problems.append(f"{name}: unknown piece {kind}")
    return exp


def matches(kind, want, line):
    s = line.strip()
    if kind == "frozen":
        return line.rstrip() == want
    if kind == "new":
        return s == want
    if kind == "alias":
        m = ALIAS.match(s)
        return bool(m) and m.group(1) == want and m.group(2) == want
    return s == f"w.{want} = {want};"


def find_starts(src, exp, has_body):
    """Lines of src where the unit's first (up to three) code lines match in order.  A
    declaration without a body (a prototype) is not a match for a unit that has one."""
    rows = [i for i in range(1, len(src.code)) if src.code[i].strip()]
    k = min(3, len(exp))
    out = []
    for n, i in enumerate(rows):
        if n + k > len(rows):
            break
        if all(matches(exp[j][0], exp[j][1], src.code[rows[n + j]]) for j in range(k)):
            if has_body and "{" not in "".join(src.blank[i:src.extent(i) + 1]):
                continue
            out.append(i)
    return out


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tree", default=str(ROOT),
                    help="directory holding the new files (default: this checkout)")
    a = ap.parse_args()
    tree = pathlib.Path(a.tree)

    problems = []
    raw = git_show(FROZEN_REV, FROZEN_PATH)
    frozen = Source(raw)
    code = frozen_code_with_subs(raw, problems)
    new = {p: Source((tree / p).read_text(encoding="utf-8")) for p in FILES}
    claimed = collections.Counter()
    covered = {p: set() for p in FILES}
    lit_expected = collections.Counter()

    # 1. Units.
    n_pass = 0
    for name, path, pieces in UNITS:
        exp = expected(code, pieces, claimed, problems, name, lit_expected)
        if not exp:
            problems.append(f"{name}: no code lines")
            continue
        has_body = any("{" in t for kind, t in exp if kind in ("frozen", "new"))
        src = new[path]
        starts = find_starts(src, exp, has_body)
        others = [p for p in FILES if p != path and find_starts(new[p], exp, has_body)]
        if len(starts) != 1 or others:
            problems.append(f"{name}: first line found {len(starts)} times in {path}"
                            + (f", also in {others}" if others else ""))
            continue
        s = starts[0]
        rows = [i for i in range(s, len(src.code)) if src.code[i].strip()][:len(exp)]
        e = rows[-1]
        covered[path].update(range(s, e + 1))
        bad = next((k for k, (kind, want) in enumerate(exp)
                    if k >= len(rows) or not matches(kind, want, src.code[rows[k]])), None)
        if bad is not None:
            got = src.code[rows[bad]].rstrip() if bad < len(rows) else "<end of file>"
            problems.append(f"{name}: differs at code line {bad + 1} of {len(exp)} "
                            f"({path}:{rows[bad] if bad < len(rows) else '?'}):\n"
                            f"      expected ({exp[bad][0]}): {exp[bad][1]}\n"
                            f"      found          : {got}")
            continue
        body = "".join(src.blank[s:e + 1])
        if body.count("{") != body.count("}") or not src.code[e].rstrip().endswith(("}", ";")):
            problems.append(f"{name}: the carried lines do not close the item at {path}:{e}")
            continue
        n_pass += 1
        if a.verbose:
            print(f"PASS {name}: {len(exp)} code lines at {path}:{s}-{e}")

    # 2. Coverage.
    lit_rewritten = collections.Counter()
    for i in range(1, len(frozen.code)):
        if not frozen.code[i].strip():
            continue
        if in_ranges(i, SCAFFOLD):
            claimed[i] += 1
        if claimed[i] == 0 and rewritten(i):
            lit_rewritten.update(literals(code[i]))
        elif claimed[i] == 0 and not dropped(i):
            problems.append(f"coverage: frozen line {i} is carried by no unit: {frozen.code[i].strip()}")
        elif claimed[i] > 1:
            problems.append(f"coverage: frozen line {i} is carried {claimed[i]} times")

    # 3. Nothing added.
    for p in FILES:
        src = new[p]
        types = DECLARED_TYPES.get(p, [])
        i = 1
        while i < len(src.code):
            line = src.code[i].strip()
            if not line or i in covered[p] or ALLOWED_OUTSIDE.match(line):
                i += 1
                continue
            if p.endswith(".hpp"):
                e = src.extent(i)
                body = "".join(src.blank[i:e + 1])
                if any(re.match(t, line) for t in types):
                    for j in range(i, e + 1):
                        lit_expected.update(literals(src.code[j]))
                    i = e + 1
                    continue
                if "{" not in body and body.rstrip().endswith(";"):
                    i = e + 1
                    continue
            problems.append(f"added: {p}:{i}: {line}")
            i += 1

    # 4. Literals: the new files hold exactly the carried, declared-new and declared-type
    #    literals, and a rewrite loses no literal (each still occurs in the new files).
    lit_found = collections.Counter()
    for p in FILES:
        for line in new[p].code[1:]:
            if line.strip().startswith("#include"):
                continue
            lit_found.update(literals(line))
    if lit_expected != lit_found:
        problems.append(f"literals: only expected {dict(lit_expected - lit_found)}, "
                        f"only found {dict(lit_found - lit_expected)}")
    lost = sorted(t for t in lit_rewritten if t not in lit_found)
    if lost:
        problems.append(f"literals of rewritten lines found nowhere in the new files: {lost}")

    for p in problems:
        print("FAIL", p)
    n_lines = sum(1 for i in range(1, len(frozen.code)) if frozen.code[i].strip())
    n_drop = sum(1 for i in range(1, len(frozen.code)) if frozen.code[i].strip() and dropped(i))
    n_rew = sum(1 for i in range(1, len(frozen.code))
                if frozen.code[i].strip() and claimed[i] == 0 and rewritten(i))
    print(f"units {n_pass}/{len(UNITS)} PASS; frozen code lines {n_lines}: {n_drop} dropped "
          f"({len(DROPS)} declared drops), {n_rew} rewritten ({len(REWRITTEN)} declared "
          f"rewrites); substitutions {len(SUBS)}; literals {sum(lit_found.values())}; "
          f"problems {len(problems)}")
    print("RESULT", "PASS" if not problems else "FAIL")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
