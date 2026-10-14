#!/usr/bin/env python3
"""Relocation verifier for card UOM step U3 (gate G-UR): the cut of
CycleSlipManager::detectAndRepair into section members (step U3b) and the
split of its per-satellite section S11 into a verdict and its actions (U3c).

    python scripts/uom_u3_verify.py [--verbose]

The frozen source is src/core/slip/CycleSlipManager.cpp at 5154324 (card UOM
pre-registered), read with `git show`; no copy is stored.  UNITS below declares,
for every function that now carries frozen code, its file, its definition and
its body as an ordered list of pieces:

  F(a, b, shift)        frozen lines a..b (1-based), code lines only, with
                        `shift` leading spaces removed (the body moved that
                        many columns left);
  N(text)               a declared new code line (alias preamble, guard, call,
                        return), compared without leading and trailing space;
  G(if_line, close_line, text)
                        the frozen `if (COND) {` at if_line and its `}` at
                        close_line became the early-return guard `text` (a
                        declared new line); both frozen lines are carried;
  SUBS                  per unit: (frozen line, from, to), a plain text
                        replacement on that frozen line before comparison.

MEMBERS lists the frozen declarations that became members of SlipEpochWork
(slip/CycleSlipEpochWork.hpp), DROPS the frozen code lines no body carries, each
with its reason.  Comments and blank lines are ignored in the code comparison,
as in scripts/rt_seam_verify.py and scripts/nom_td_verify.py.

Exit 0 only when every check passes:
  1. Bodies.    Each unit's body code lines equal the expected sequence,
                character for character, indentation included.
  2. Coverage.  Every code line inside a function body of the frozen file is
                carried exactly once: by a unit, a member or a declared drop.
  3. Members.   Each member declaration appears in SlipEpochWork (or, for the
                deferred-repair record, in the header) as written.
  4. Comments.  Every comment line of the frozen file appears in the new files
                at least as often (whitespace-normalized), apart from the
                declared comment edits.
  5. Order.     The ordered list of log calls (level and format literal) and
                state-changing calls of the frozen detectAndRepair equals the
                driver's, with each call into a new section replaced by that
                section's own list (recursively).
"""
import argparse
import pathlib
import re
import subprocess
import sys
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parents[1]
TREE = ROOT  # where the new files are read; --tree points it at a staging copy
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

FROZEN_REV = "5154324"
FROZEN_PATH = "src/core/slip/CycleSlipManager.cpp"
WORK_HDR = "include/slip/CycleSlipEpochWork.hpp"
NEW_FILES = [
    "src/core/slip/CycleSlipManager.cpp",
    "src/core/slip/CycleSlipDetection.cpp",
    "src/core/slip/CycleSlipAdjudication.cpp",
    "src/core/slip/CycleSlipPivot.cpp",
    "src/core/slip/CycleSlipNonMwPasses.cpp",
    "src/core/slip/CycleSlipRepair.cpp",
    WORK_HDR,
]


def F(a, b, shift=0):
    return ("F", a, b, shift)


def N(text):
    return ("N", text)


def G(if_line, close_line, text):
    return ("G", if_line, close_line, text)


CSM = "src/core/slip/CycleSlipManager.cpp"
DET = "src/core/slip/CycleSlipDetection.cpp"
ADJ = "src/core/slip/CycleSlipAdjudication.cpp"
PIV = "src/core/slip/CycleSlipPivot.cpp"
NON = "src/core/slip/CycleSlipNonMwPasses.cpp"
REP = "src/core/slip/CycleSlipRepair.cpp"

# Alias preamble lines are declared as N(...) pieces: each reconstructs one of
# the driver's former local names from the SlipEpochWork member of that name.
A = {
    "log": "const auto& log = w.log;",
    "epoch_prefix": "const auto& epoch_prefix = w.epoch_prefix;",
    "modeled_obs": "const auto& modeled_obs = w.modeled_obs;",
    "state_manager": "auto& state_manager = w.state_manager;",
    "state_manager_c": "const auto& state_manager = w.state_manager;",
    "meas_model": "const auto& meas_model = w.meas_model;",
    "epoch_now_s": "const auto& epoch_now_s = w.epoch_now_s;",
    "epoch_now_s_w": "auto& epoch_now_s = w.epoch_now_s;",
    "is_bssd_pivot": "const auto& is_bssd_pivot = w.is_bssd_pivot;",
    "is_bssd_pivot_w": "auto& is_bssd_pivot = w.is_bssd_pivot;",
    "result": "auto& result = w.result;",
    "result_c": "const auto& result = w.result;",
    "aux_events_w": "auto& aux_events = w.aux_events;",
    "slip_results": "const auto& slip_results = w.slip_results;",
    "slip_results_w": "auto& slip_results = w.slip_results;",
    "te": "const auto& te = w.te;",
    "te_w": "auto& te = w.te;",
    "te_culprits_by_sat": "const auto& te_culprits_by_sat = w.te_culprits_by_sat;",
    "te_culprits_by_sat_w": "auto& te_culprits_by_sat = w.te_culprits_by_sat;",
    "te_slips_by_sat_w": "auto& te_slips_by_sat = w.te_slips_by_sat;",
    "te_suppress_by_sat": "const auto& te_suppress_by_sat = w.te_suppress_by_sat;",
    "te_suppress_by_sat_w": "auto& te_suppress_by_sat = w.te_suppress_by_sat;",
    "soft_lli_repaired": "const auto& soft_lli_repaired = w.soft_lli_repaired;",
    "soft_lli_repaired_w": "auto& soft_lli_repaired = w.soft_lli_repaired;",
    "reset_sats_w": "auto& reset_sats = w.reset_sats;",
    "slips_by_sat": "const auto& slips_by_sat = w.slips_by_sat;",
    "slips_by_sat_w": "auto& slips_by_sat = w.slips_by_sat;",
    "all_pairs_by_sat": "const auto& all_pairs_by_sat = w.all_pairs_by_sat;",
    "all_pairs_by_sat_w": "auto& all_pairs_by_sat = w.all_pairs_by_sat;",
    "reset_keys_by_sat_w": "auto& reset_keys_by_sat = w.reset_keys_by_sat;",
    "lli_sats": "const auto& lli_sats = w.lli_sats;",
    "lli_sats_w": "auto& lli_sats = w.lli_sats;",
    "epoch_reset_amb_keys": "const auto& epoch_reset_amb_keys = w.epoch_reset_amb_keys;",
    "epoch_reset_amb_keys_w": "auto& epoch_reset_amb_keys = w.epoch_reset_amb_keys;",
    "pivot_affected_cons_sig": "const auto& pivot_affected_cons_sig = w.pivot_affected_cons_sig;",
    "pivot_affected_cons_sig_w": "auto& pivot_affected_cons_sig = w.pivot_affected_cons_sig;",
    "deferred_integer_repairs": "const auto& deferred_integer_repairs = w.deferred_integer_repairs;",
    "deferred_integer_repairs_w": "auto& deferred_integer_repairs = w.deferred_integer_repairs;",
    "deferred_resets": "const auto& deferred_resets = w.deferred_resets;",
    "deferred_resets_w": "auto& deferred_resets = w.deferred_resets;",
}


def aliases(*names):
    return [N(A[n]) for n in names]


UNITS = [
    # --- CycleSlipManager.cpp: kept in place, or the two key helpers made
    #     private static members (their bodies unchanged).
    dict(file=CSM, def_re=r"^std::string CycleSlipManager::satIdFromAmbiguityKey\(",
         pieces=[F(23, 24)]),
    dict(file=CSM, def_re=r"^std::string CycleSlipManager::signalFromAmbiguityKey\(",
         pieces=[F(29, 30)]),
    dict(file=CSM, def_re=r"^CycleSlipManager::CycleSlipManager\(", pieces=[F(42, 65)]),
    dict(file=CSM, def_re=r"CycleSlipManager::getResolvedWlAmbiguities\(", pieces=[F(73, 73)]),
    dict(file=CSM, def_re=r"CycleSlipManager::getResolvedWlFractions\(", pieces=[F(77, 77)]),
    dict(file=CSM, def_re=r"^CycleSlipManager::getResolvedWlQuality\(", pieces=[F(82, 83)]),
    dict(file=CSM, def_re=r"CycleSlipManager::resetSatellite\(", pieces=[F(87, 93)]),
    dict(file=CSM, def_re=r"CycleSlipManager::resetSatelliteSignal\(", pieces=[F(98, 99)]),
    dict(file=CSM, def_re=r"CycleSlipManager::setCorrections\(", pieces=[F(108, 110)]),
    dict(file=CSM, def_re=r"CycleSlipManager::enableMwTracing\(", pieces=[F(114, 114)]),
    dict(file=CSM, def_re=r"CycleSlipManager::enableTurboEditTracing\(", pieces=[F(118, 118)]),
    dict(file=CSM, def_re=r"CycleSlipManager::setSampleInterval\(", pieces=[F(122, 128)]),
    dict(file=CSM, def_re=r"CycleSlipManager::recordAmbiguitySnapshot\(", pieces=[F(1807, 1807)]),
    dict(file=CSM, def_re=r"CycleSlipManager::getHeldPendingBridgeKeys\(", pieces=[F(1811, 1811)]),
    dict(file=CSM, def_re=r"CycleSlipManager::resetSoftLliBridge\(", pieces=[F(1815, 1815)]),
    # --- CycleSlipRepair.cpp and CycleSlipAdjudication.cpp: moved, unchanged.
    dict(file=REP, def_re=r"^void CycleSlipManager::resetAmbiguityBssdAware\(", pieces=[F(143, 258)]),
    dict(file=REP, def_re=r"^bool CycleSlipManager::repairAmbiguityBssdAware\(", pieces=[F(273, 343)]),
    dict(file=ADJ, def_re=r"CycleSlipManager::computeCulpritSlipIntegers\(", pieces=[F(349, 400)]),
    dict(file=ADJ, def_re=r"CycleSlipManager::identifySlippedSignals\(", pieces=[F(410, 453)]),
    # --- The driver.
    dict(file=CSM, def_re=r"^CycleSlipEpochResult CycleSlipManager::detectAndRepair\(",
         section=True, pieces=[
             N("SlipEpochWork w(epoch_prefix, modeled_obs, state_manager, meas_model,"),
             N('log_manager_.getLogger("cycle_slip_manager"));'),
             N("beginSlipEpoch(w);"),
             N("registerSoftLliGapHolds(w);"),
             N("runAuxiliaryDetectors(w);"),
             N("detectMwSlips(w);"),
             N("resolveTurboEdit(w);"),
             N("resolveSoftLliHolds(w);"),
             N("groupMwPairsBySatellite(w);"),
             F(702, 702, 4),
             N("collectMwMandates(w);"),
             N("scanPivotCulprits(w);"),
             F(828, 828, 4),
             N("handleSatelliteMandates(w, sat_id, amb_keys);"),
             F(1192, 1192, 4),
             N("repairAcrossSlippedPivotInPlace(w);"),
             N("forcePivotHandoverAndApplyDeferred(w);"),
             N("logMwEpochSummary(w);"),
             F(1639, 1639, 4),
             N("applyTurboEditGfOnlyResolutions(w);"),
             N("applyAuxiliarySlipEvents(w);"),
             N("return finishSlipEpoch(w);"),
         ],
         subs=[(702, "slip_results", "w.slip_results"),
               (828, "reset_keys_by_sat", "w.reset_keys_by_sat")]),
    # --- S0 and S17 (CycleSlipManager.cpp).
    dict(file=CSM, def_re=r"^void CycleSlipManager::beginSlipEpoch\(", section=True, pieces=[
        *aliases("modeled_obs", "meas_model", "epoch_now_s_w", "is_bssd_pivot_w"),
        F(468, 473), F(475, 490)],
        subs=[(484, "const SoftLliBridge::IsBssdPivotFn is_bssd_pivot =", "is_bssd_pivot =")]),
    dict(file=CSM, def_re=r"^CycleSlipEpochResult CycleSlipManager::finishSlipEpoch\(", section=True,
         pieces=[*aliases("result", "epoch_now_s"), F(1660, 1670)]),
    # --- S1, S2, S4 to S8 (CycleSlipDetection.cpp).
    dict(file=DET, def_re=r"^void CycleSlipManager::registerSoftLliGapHolds\(", section=True, pieces=[
        *aliases("epoch_prefix", "modeled_obs", "state_manager_c", "epoch_now_s", "is_bssd_pivot"),
        G(502, 505, "if (!turbo_ || !soft_lli_.enabled()) return;"), F(503, 504, 4)]),
    dict(file=DET, def_re=r"^void CycleSlipManager::runAuxiliaryDetectors\(", section=True, pieces=[
        *aliases("modeled_obs", "aux_events_w"), F(507, 514), F(516, 522)]),
    dict(file=DET, def_re=r"^void CycleSlipManager::detectMwSlips\(", section=True, pieces=[
        *aliases("modeled_obs", "slip_results_w"), F(556, 558, 4)],
        subs=[(556, "const std::vector<MwSlipResult> slip_results =", "slip_results =")]),
    dict(file=DET, def_re=r"^void CycleSlipManager::resolveTurboEdit\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "modeled_obs", "epoch_now_s", "is_bssd_pivot", "slip_results",
                 "te_w", "te_culprits_by_sat_w", "te_slips_by_sat_w", "te_suppress_by_sat_w"),
        G(560, 602, "if (!turbo_) return;"), F(561, 601, 8)]),
    dict(file=DET, def_re=r"^void CycleSlipManager::resolveSoftLliHolds\(", section=True, pieces=[
        *aliases("epoch_prefix", "modeled_obs", "state_manager", "meas_model", "epoch_now_s", "te",
                 "result", "soft_lli_repaired_w", "reset_sats_w"),
        G(611, 629, "if (!turbo_ || soft_lli_.empty()) return;"), F(612, 628, 8)]),
    dict(file=DET, def_re=r"^void CycleSlipManager::groupMwPairsBySatellite\(", section=True, pieces=[
        *aliases("slip_results", "slips_by_sat_w", "all_pairs_by_sat_w"),
        F(631, 642, 4), F(644, 660, 4), F(662, 700, 4)]),
    dict(file=DET, def_re=r"^void CycleSlipManager::collectMwMandates\(", section=True, pieces=[
        *aliases("slip_results", "reset_keys_by_sat_w", "lli_sats_w"), F(703, 704, 8), F(715, 725, 8)]),
    # --- S9, S12, S13 (CycleSlipPivot.cpp).
    dict(file=PIV, def_re=r"^void CycleSlipManager::scanPivotCulprits\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "meas_model", "te_culprits_by_sat", "te_suppress_by_sat",
                 "slips_by_sat", "all_pairs_by_sat_w", "pivot_affected_cons_sig_w"),
        F(736, 762, 8), G(764, 811, "if (!config_.enableBetweenSatelliteSd()) return;"), F(765, 810, 12)]),
    dict(file=PIV, def_re=r"^bool CycleSlipManager::confidentlyCleanOnSignal\(", section=True, pieces=[
        *aliases("lli_sats", "slips_by_sat", "all_pairs_by_sat"), F(1237, 1255, 12)]),
    dict(file=PIV, def_re=r"^void CycleSlipManager::repairAcrossSlippedPivotInPlace\(", section=True,
         pieces=[
             *aliases("meas_model", "pivot_affected_cons_sig_w", "deferred_integer_repairs_w",
                      "deferred_resets_w"),
             G(1258, 1467, "if (!config_.enableBetweenSatelliteSd() || deferred_integer_repairs.empty()) return;"),
             F(1259, 1267, 12),
             N("if (repairConstellationInPlace(w, cons, pivots_now))"),
             F(1432, 1432, 8),
             F(1434, 1466, 12)]),
    dict(file=PIV, def_re=r"^bool CycleSlipManager::repairConstellationInPlace\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "modeled_obs", "state_manager", "meas_model", "result",
                 "epoch_reset_amb_keys", "pivot_affected_cons_sig", "deferred_integer_repairs",
                 "deferred_resets"),
        F(1268, 1431, 16), F(1433, 1433, 16), N("return true;")],
        subs=[(1269, "continue;", "return false;"), (1292, "continue;", "return false;"),
              (1377, "confidentlyCleanOnSignal(sat, sig)", "confidentlyCleanOnSignal(w, sat, sig)")]),
    dict(file=PIV, def_re=r"^std::set<std::string> CycleSlipManager::handoverExcludeSet\(", section=True,
         pieces=[*aliases("te", "te_culprits_by_sat", "slips_by_sat", "lli_sats"),
                 F(1507, 1531, 12), N("return exclude_sats;")]),
    dict(file=PIV, def_re=r"^void CycleSlipManager::forcePivotHandoverAndApplyDeferred\(", section=True,
         pieces=[
             *aliases("log", "epoch_prefix", "modeled_obs", "state_manager", "meas_model",
                      "pivot_affected_cons_sig", "deferred_integer_repairs", "deferred_resets"),
             F(1469, 1489, 8),
             G(1490, 1631, "if (deferred_integer_repairs.empty() && deferred_resets.empty()) return;"),
             F(1491, 1506, 12),
             N("std::set<std::string> exclude_sats = handoverExcludeSet(w);"),
             F(1532, 1630, 12)]),
    # --- S11 (CycleSlipAdjudication.cpp): the verdict, then the one action it
    #     selects (step U3c).
    dict(file=ADJ, def_re=r"^void CycleSlipManager::handleSatelliteMandates\(", section=True, pieces=[
        N("const SatelliteSlipVerdict verdict = adjudicateSatellite(w, sat_id);"),
        N("switch (verdict.action) {"),
        N("case SatelliteSlipAction::SkipBridgeRepaired:"),
        N("case SatelliteSlipAction::SkipWlSettlePending:"),
        N("return;"),
        N("case SatelliteSlipAction::RepairCulprits:"),
        N("applyCulpritVerdict(w, sat_id, amb_keys, verdict);"),
        N("return;"),
        N("case SatelliteSlipAction::SuppressMandates:"),
        N("applySuppressionVerdict(w, sat_id, amb_keys, verdict);"),
        N("return;"),
        N("case SatelliteSlipAction::FullReset:"),
        N("resetEveryMandate(w, sat_id, amb_keys, verdict);"),
        N("holdOrResetLliArcs(w, sat_id, amb_keys, verdict);"),
        N("reinflateSatelliteStates(w, sat_id);"),
        N("return;"),
        N("}")]),
    dict(file=ADJ, def_re=r"^SatelliteSlipVerdict CycleSlipManager::adjudicateSatellite\(", section=True,
         pieces=[
             *aliases("log", "epoch_prefix", "te", "te_culprits_by_sat", "te_slips_by_sat_w",
                      "te_suppress_by_sat", "soft_lli_repaired", "slips_by_sat", "all_pairs_by_sat_w",
                      "lli_sats"),
             F(829, 872, 12),
             N("SatelliteSlipVerdict verdict;"),
             N("verdict.lli = lli_sat;"),
             N("auto& culprit_signals = verdict.culprit_signals;"),
             N("auto& per_signal_slips = verdict.known_integers;"),
             F(875, 930, 12),
             N("verdict.n_slipped_pairs = has_pair_slips ? slips_it->second.size() : 0;"),
             N("verdict.n_mature_pairs = sat_all_pairs.size();"),
             N("if (!culprit_signals.empty()) {"),
             N("verdict.action = SatelliteSlipAction::RepairCulprits;"),
             N("verdict.source = culprits_from_te ? CulpritSource::TurboEdit"),
             N(": CulpritSource::CrossConsistency;"),
             N("auto& adjudicated = verdict.adjudicated_signals;"),
             F(1006, 1017, 16),
             N("return verdict;"),
             N("}"),
             F(1060, 1062, 12),
             N("verdict.action = SatelliteSlipAction::SuppressMandates;"),
             N("verdict.suppressed_signals = tce->second;"),
             N("return verdict;"),
             F(1094, 1095, 12),
             N("verdict.action = SatelliteSlipAction::FullReset;"),
             N("return verdict;")],
         subs=[(833, "continue;", "return SatelliteSlipVerdict{ SatelliteSlipAction::SkipBridgeRepaired };"),
               (844, "continue;", "return SatelliteSlipVerdict{ SatelliteSlipAction::SkipWlSettlePending };"),
               (927, "te_suppress_by_sat.erase(tce_c);", "verdict.suppression_withdrawn = true;"),
               (1006, "std::set<std::string> adjudicated = culprit_signals;", "adjudicated = culprit_signals;"),
               (1060, "if (!lli_sat) {", "if (!lli_sat && !verdict.suppression_withdrawn) {")]),
    dict(file=ADJ, def_re=r"^void CycleSlipManager::applyCulpritVerdict\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "modeled_obs", "state_manager", "meas_model", "result",
                 "reset_sats_w", "epoch_reset_amb_keys_w", "pivot_affected_cons_sig",
                 "deferred_integer_repairs_w", "deferred_resets_w"),
        N("const auto& culprit_signals = verdict.culprit_signals;"),
        N("const auto& per_signal_slips = verdict.known_integers;"),
        N("const auto& adjudicated = verdict.adjudicated_signals;"),
        F(933, 985, 20), F(1018, 1030, 20), F(1032, 1032, 20)]),
    dict(file=ADJ, def_re=r"^void CycleSlipManager::applySuppressionVerdict\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "modeled_obs", "state_manager", "meas_model", "result",
                 "reset_sats_w", "epoch_reset_amb_keys_w"),
        F(1063, 1092, 20)],
        subs=[(1067, "tce->second.count(signal)", "verdict.suppressed_signals.count(signal)")]),
    dict(file=ADJ, def_re=r"^void CycleSlipManager::resetEveryMandate\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "modeled_obs", "state_manager", "meas_model", "result",
                 "reset_sats_w", "epoch_reset_amb_keys_w"),
        F(1097, 1110, 12)],
        subs=[(1100, "has_pair_slips ? slips_it->second.size() : 0,", "verdict.n_slipped_pairs,"),
              (1101, "sat_all_pairs.size(), amb_keys.size());", "verdict.n_mature_pairs, amb_keys.size());")]),
    dict(file=ADJ, def_re=r"^void CycleSlipManager::holdOrResetLliArcs\(", section=True, pieces=[
        *aliases("epoch_prefix", "meas_model", "epoch_now_s"),
        N("const bool lli_sat = verdict.lli;"),
        G(1131, 1145, "if (!lli_sat || !turbo_) return;"), F(1132, 1144, 16)]),
    dict(file=ADJ, def_re=r"^void CycleSlipManager::reinflateSatelliteStates\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "state_manager", "meas_model"),
        F(1163, 1163, 12), G(1164, 1191, "if (!sr_cfg.enabled) return;"), F(1165, 1190, 16)]),
    # --- S14 (CycleSlipAdjudication.cpp).
    dict(file=ADJ, def_re=r"^void CycleSlipManager::logMwEpochSummary\(", section=True, pieces=[
        *aliases("log", "epoch_prefix", "result_c"), F(1633, 1638, 8)]),
    # --- S15 and S16 (CycleSlipNonMwPasses.cpp): the pass's guard moved from
    #     the driver into the pass; the driver's call became the pass itself.
    dict(file=NON, def_re=r"^void CycleSlipManager::applyTurboEditGfOnlyResolutions\(", section=False,
         pieces=[
             *aliases("te"), N("auto& handled_sats = w.reset_sats;"),
             *aliases("modeled_obs", "state_manager", "meas_model", "epoch_prefix", "result"),
             G(1649, 1652, "if (!turbo_) return;"), F(1737, 1797)]),
    dict(file=NON, def_re=r"^void CycleSlipManager::applyAuxiliarySlipEvents\(", section=False,
         pieces=[
             N("const auto& events = w.aux_events;"), N("auto& handled_sats = w.reset_sats;"),
             *aliases("modeled_obs", "state_manager", "meas_model", "epoch_prefix", "result"),
             G(1655, 1658, "if (events.empty()) return;"), F(1686, 1721)]),
]

# Frozen declarations that became members of SlipEpochWork ("work") or a
# namespace-scope type of its header ("header").
MEMBERS = {
    467: ("work", "CycleSlipEpochResult result;"),
    474: ("work", "double epoch_now_s{ 0.0 };"),
    515: ("work", "std::vector<SlipEvent> aux_events;"),
    525: ("work", "std::set<std::string> reset_sats;"),
    529: ("work", "std::set<std::string> soft_lli_repaired;"),
    537: ("work", "TurboEditResolver::EpochResolution te;"),
    538: ("work", "std::map<std::string, std::set<std::string>> te_culprits_by_sat;"),
    539: ("work", "std::map<std::string, std::map<std::string, int>> te_slips_by_sat;"),
    554: ("work", "std::map<std::string, std::set<std::string>> te_suppress_by_sat;"),
    556: ("work", "std::vector<MwSlipResult> slip_results;"),
    643: ("work", "std::map<std::string, std::vector<MwSlipResult>> slips_by_sat;"),
    661: ("work", "std::map<std::string, std::vector<MwSlipResult>> all_pairs_by_sat;"),
    705: ("work", "std::map<std::string, std::set<std::string>> reset_keys_by_sat;"),
    714: ("work", "std::set<std::string> lli_sats;"),
    734: ("work", "std::set<std::string> epoch_reset_amb_keys;"),
    763: ("work", "std::set<std::pair<std::string, std::string>> pivot_affected_cons_sig;"),
    816: ("header", "struct DeferredIntegerRepair {"),
    817: ("header", "std::string sat_id;"),
    818: ("header", "std::string signal;"),
    819: ("header", "int dN_carrier_cycles;"),
    820: ("header", "};"),
    821: ("work", "std::vector<DeferredIntegerRepair> deferred_integer_repairs;"),
    825: ("work", "std::vector<std::pair<std::string, std::string>> deferred_resets;"),
}
# Line 556 is both a member (the declaration) and carried by S4 (the
# initializer became an assignment); MEMBERS documents the type, the unit
# carries the line.
MEMBER_ALSO_CARRIED = {556}

DROPS = {
    104: "notifyPivotForcedHandover deleted in step U3a (no caller; card UOM map F-D9)",
    466: "the module logger is fetched once by the driver into SlipEpochWork::log",
    484: None,  # carried by beginSlipEpoch (substituted); listed to keep the table complete
    555: "the bare block scoped slip_results, which is now a SlipEpochWork member",
    1235: "the lambda became the member function confidentlyCleanOnSignal",
    1236: "the lambda's parameter list became the member function's",
    1256: "the lambda's closing `};`",
    1640: "the bare block's closing brace (see 555)",
    1650: "the driver's call became the pass itself (applyTurboEditGfOnlyResolutions(w))",
    1651: "the call's continuation line",
    873: "the culprit set is the verdict's culprit_signals (step U3c)",
    874: "the culprit integers are the verdict's known_integers (step U3c)",
    931: "the culprit branch is the action RepairCulprits selected by the verdict (step U3c)",
    932: "a bare block with no declaration of its own",
    1033: "the `continue` that ended the culprit branch: applyCulpritVerdict returns",
    1034: "the bare block's closing brace (see 932)",
    1035: "the culprit branch's closing brace (see 931)",
    1093: "the `continue` that ended the suppression branch: applySuppressionVerdict returns",
    1656: "the driver's call became the pass itself (applyAuxiliarySlipEvents(w))",
    1657: "the call's continuation line",
    1686: None,
    1737: None,
}
DROPS = {k: v for k, v in DROPS.items() if v}

# Comment lines of the frozen file deliberately edited (normalized text).
COMMENT_EDITS = {
    "TurboEdit (MW+GF joint repair) resolution for this epoch.  Declared at":
        "the working set is a SlipEpochWork member now, not a function-scope local",
    "function scope because it is consumed in two places: the MW per-satellite":
        "(same sentence)",
}

# Calls that change state or order observable output; with the log calls they
# form the event list of check 5.
ACTIONS = {
    "resetAmbiguityBssdAware", "repairAmbiguityBssdAware", "computeCulpritSlipIntegers",
    "identifySlippedSignals", "applyAuxiliarySlipEvents", "applyTurboEditGfOnlyResolutions",
    "detectLargeMwResets", "resolve", "registerGapHolds", "registerHold", "resolveHolds", "detect",
    "resetSatellite", "requestForcedHandover", "handlePivotHandover", "setStateVector",
    "setCovariance", "markAmbiguityPendingReseed", "resetAmbiguity", "reinflateIonosphereVariance",
    "reinflateIonosphereRateVariance", "reinflateSatBiasVariancesForSat", "advanceEpoch",
    "getActiveArcKeys", "getEpochCleanPairs", "getAcrossGapWlEvidence",
}
# Frozen lambdas whose bodies run at their call site, not at their definition.
FROZEN_LAMBDAS = {"confidentlyCleanOnSignal": (1235, 1256)}


# ---------------------------------------------------------------------------
def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def blank_literals(text):
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup in ("str", "chr", "raw"):
            out.append("".join(c if c == "\n" else " " for c in s))
        else:
            out.append(s)
    return "".join(out)


def brace_close(blank_lines, start):
    """(open line, close line) of the first brace block opening at or after start."""
    depth, opened, open_i = 0, False, None
    for i in range(start, len(blank_lines)):
        for ch in blank_lines[i]:
            if ch == "{":
                depth += 1
                if not opened:
                    opened, open_i = True, i
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return open_i, i
    raise ValueError("unbalanced braces")


def comment_lines(text):
    """Normalized comment lines: the text of every // and /* */ line."""
    out = []
    for m in _CPP.finditer(text):
        if m.lastgroup == "lc":
            t = m.group()[2:].lstrip("/").strip()
            if t:
                out.append(t)
        elif m.lastgroup == "bc":
            for part in m.group()[2:-2].split("\n"):
                t = part.strip().lstrip("*").strip()
                if t:
                    out.append(t)
    return out


def norm(s):
    return re.sub(r"\s+", " ", s.strip())


def decl_core(s):
    """A declaration without const, initializer or brace-init: its type and name."""
    s = norm(s).rstrip(";").strip()
    s = re.sub(r"\s*=.*$", "", s)
    s = re.sub(r"\{[^{}]*\}$", "", s).strip()
    return re.sub(r"^const ", "", s)


def unshift(line, k, where):
    if k > 0:
        if line[:k] != " " * k:
            raise ValueError(f"{where}: fewer than {k} leading spaces")
        return line[k:]
    return " " * (-k) + line


class Frozen:
    def __init__(self):
        self.text = git_show(FROZEN_REV, FROZEN_PATH)
        self.raw = [""] + self.text.split("\n")
        self.code = [""] + strip_comments(self.text).split("\n")
        self.blank = [""] + blank_literals(strip_comments(self.text)).split("\n")

    def function_bodies(self):
        """1-based (first, last) body lines of every function at namespace depth."""
        bodies, depth = [], 0
        i = 1
        while i < len(self.blank):
            line = self.blank[i]
            for ch in line:
                if ch == "{":
                    depth += 1
                    if depth == 2:
                        o, c = brace_close(self.blank, i)
                        bodies.append((o + 1, c - 1))
                        depth = 1
                        i = c
                        break
                elif ch == "}":
                    depth -= 1
            i += 1
        return bodies


def expected(Fz, unit, problems):
    subs = {}
    for line, a, b in unit.get("subs", []):
        subs.setdefault(line, []).append((a, b))
    exp, covered = [], []
    for p in unit["pieces"]:
        if p[0] == "N":
            exp.append(("new", p[1].strip(), None))
        elif p[0] == "G":
            _, s, e, text = p
            if not re.match(r"^if \(.*\) \{$", Fz.code[s].strip()):
                problems.append(f"{unit['def_re']}: frozen {s} is not an `if (...) {{` line")
            if Fz.code[e].strip() != "}":
                problems.append(f"{unit['def_re']}: frozen {e} is not the guard's closing brace")
            covered += [s, e]
            exp.append(("new", text, s))
        else:
            _, a, b, k = p
            for i in range(a, b + 1):
                line = Fz.code[i]
                if not line.strip():
                    continue
                for x, y in subs.get(i, []):
                    if x not in line:
                        problems.append(f"substitution {x!r} not on frozen line {i}")
                    line = line.replace(x, y)
                covered.append(i)
                exp.append(("frozen", unshift(line, k, f"frozen {i}").rstrip(), i))
    return exp, covered


def actual(path, def_re):
    text = (TREE / path).read_text(encoding="utf-8").replace("\r\n", "\n")
    code = strip_comments(text).split("\n")
    blank = blank_literals(strip_comments(text)).split("\n")
    starts = [i for i, l in enumerate(code) if re.search(def_re, l)]
    if len(starts) != 1:
        raise ValueError(f"{len(starts)} definitions match {def_re!r} in {path}")
    o, c = brace_close(blank, starts[0])
    lines = [(i + 1, code[i].rstrip()) for i in range(o + 1, c) if code[i].strip()]
    return lines, "\n".join(code[o + 1:c])


def compare(unit, exp, act, problems):
    for j in range(max(len(exp), len(act))):
        e = exp[j] if j < len(exp) else None
        a = act[j] if j < len(act) else None
        if e is None or a is None:
            ok = False
        elif e[0] == "new":
            ok = norm(a[1]) == norm(e[1])
        else:
            ok = a[1] == e[1]
        if not ok:
            problems.append(
                f"{unit['def_re']} ({unit['file']}): first difference at body code line {j}:\n"
                f"       expected ({'frozen ' + str(e[2]) if e and e[0] == 'frozen' else 'declared'}): "
                f"{e[1] if e else '<end>'}\n"
                f"       found    ({unit['file']}:{a[0] if a else '<end>'}): {a[1] if a else '<end>'}")
            return False
    return True


# --- check 5: the event list ------------------------------------------------
EV_RE = re.compile(r"\blog->(info|warn|error|debug)\s*\(|\b([A-Za-z_]\w*)\s*\(")


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


def raw_events(text, names):
    ev = []
    for m in EV_RE.finditer(text):
        if m.group(1):
            ev.append(("log", m.group(1), literal_after(text, m.end())))
        elif m.group(2) in ACTIONS or m.group(2) in names:
            ev.append(("call", m.group(2)))
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tree", help="read the new files from this directory (a staging copy)")
    args = ap.parse_args()
    global TREE
    if args.tree:
        TREE = pathlib.Path(args.tree).resolve()
    Fz = Frozen()
    problems = []
    cover = {}
    bodies = {}
    n_ok = 0
    for u in UNITS:
        try:
            exp, covered = expected(Fz, u, problems)
            act, body = actual(u["file"], u["def_re"])
        except ValueError as e:
            problems.append(str(e))
            continue
        name = re.search(r"(\w+)\\\($", u["def_re"]).group(1)
        u["name"] = name
        bodies[name] = body
        for i in covered:
            cover.setdefault(i, []).append(name)
        if compare(u, exp, act, problems):
            n_ok += 1
            if args.verbose:
                print(f"OK   {name:36s} {u['file']}  "
                      f"({sum(1 for e in exp if e[0] == 'frozen')} frozen, "
                      f"{sum(1 for e in exp if e[0] == 'new')} declared code lines)")

    # 2. Coverage over every frozen function body.
    for i, (where, _) in MEMBERS.items():
        if i not in MEMBER_ALSO_CARRIED:
            cover.setdefault(i, []).append("member")
    for i in DROPS:
        if not Fz.code[i].strip():
            problems.append(f"declared drop {i} is not a code line")
        cover.setdefault(i, []).append("drop")
    n_code = 0
    for a, b in Fz.function_bodies():
        for i in range(a, b + 1):
            if not Fz.code[i].strip():
                continue
            n_code += 1
            if i not in cover:
                problems.append(f"frozen line {i} is carried by nothing: {Fz.code[i].strip()[:90]}")
    twice = {i: w for i, w in cover.items() if len(w) > 1}
    for i, w in sorted(twice.items()):
        problems.append(f"frozen line {i} is carried twice ({', '.join(w)})")

    # 3. Members.
    htext = (TREE / WORK_HDR).read_text(encoding="utf-8").replace("\r\n", "\n")
    hcode = strip_comments(htext)
    hblank = blank_literals(hcode).split("\n")
    hlines = hcode.split("\n")
    m = [i for i, l in enumerate(hlines) if re.match(r"^struct SlipEpochWork\b", l)]
    work = set()
    if len(m) == 1:
        o, c = brace_close(hblank, m[0])
        work = {norm(l) for l in hlines[o:c + 1] if l.strip()}
    else:
        problems.append("struct SlipEpochWork not found once in " + WORK_HDR)
    header = {norm(l) for l in hlines if l.strip()}
    for i, (where, text) in MEMBERS.items():
        pool = work if where == "work" else header
        if norm(text) not in pool:
            problems.append(f"member of frozen {i} not declared as `{text}` in {WORK_HDR}")
        if decl_core(Fz.code[i]) != decl_core(text):
            problems.append(f"member text for frozen {i} does not match the frozen declaration "
                            f"({decl_core(Fz.code[i])!r} against {decl_core(text)!r})")

    # 4. Comments.
    frozen_c = Counter(norm(c) for c in comment_lines(Fz.text))
    new_c = Counter()
    for f in NEW_FILES:
        new_c.update(norm(c) for c in comment_lines((TREE / f).read_text(encoding="utf-8")))
    edits = {norm(k) for k in COMMENT_EDITS}
    lost = []
    for c, n in frozen_c.items():
        if new_c[c] < n and c not in edits:
            lost.append((c, n - new_c[c]))
    for c, n in lost:
        problems.append(f"comment line lost ({n}x): {c[:100]}")

    # 5. The event list.
    names = {u["name"] for u in UNITS if u.get("section")}
    fbody_a, fbody_b = 466, 1670
    ftext_lines = Fz.code[fbody_a:fbody_b + 1]
    la, lb = FROZEN_LAMBDAS["confidentlyCleanOnSignal"]
    lam_text = "\n".join(Fz.code[la + 2:lb])
    pre = "\n".join(Fz.code[fbody_a:la])
    post = "\n".join(Fz.code[lb + 1:fbody_b + 1])
    fev = []
    for e in raw_events(pre, set()) + [("LAMBDA",)] + raw_events(post, {"confidentlyCleanOnSignal"}):
        if e == ("LAMBDA",):
            continue
        if e == ("call", "confidentlyCleanOnSignal"):
            fev += raw_events(lam_text, set())
        else:
            fev.append(e)

    def expand(name, depth=0):
        out = []
        for e in raw_events(bodies.get(name, ""), names):
            if e[0] == "call" and e[1] in names and e[1] != name:
                out += expand(e[1], depth + 1)
            else:
                out.append(e)
        return out

    nev = expand("detectAndRepair")
    if fev != nev:
        k = next((k for k in range(min(len(fev), len(nev))) if fev[k] != nev[k]), min(len(fev), len(nev)))
        problems.append(f"event order differs at event {k} of {len(fev)}/{len(nev)}:\n"
                        f"       frozen: {fev[k] if k < len(fev) else '<end>'}\n"
                        f"       new   : {nev[k] if k < len(nev) else '<end>'}")

    for p in problems:
        print("FAIL " + p)
    print("\nDeclared, for review:")
    for u in UNITS:
        for p in u["pieces"]:
            if p[0] == "G":
                print(f"  guard  {u.get('name', '?'):34s} frozen {p[1]}: `{Fz.code[p[1]].strip()}` -> `{p[3]}`")
        for line, a, b in u.get("subs", []):
            print(f"  subst  {u.get('name', '?'):34s} frozen {line}: `{a}` -> `{b}`")
    for i, r in sorted(DROPS.items()):
        print(f"  drop   frozen {i}: {r}")
    for c, r in COMMENT_EDITS.items():
        print(f"  comment edit: {c[:60]}... ({r})")
    n_log = sum(1 for e in fev if e[0] == "log")
    print(f"\n{n_ok} of {len(UNITS)} bodies verbatim in code; {n_code} frozen body code lines, "
          f"{sum(1 for v in cover.values() if 'member' in v)} became members, {len(DROPS)} declared drops, "
          f"{len(twice)} carried twice; {sum(frozen_c.values())} frozen comment lines, {len(lost)} lost; "
          f"event order {len(fev)} events ({n_log} log calls) "
          f"{'equal' if fev == nev else 'DIFFERENT'}.")
    print("RESULT", "PASS" if not problems else "FAIL")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
