#!/usr/bin/env python3
"""Relocation verifier for card UOM step U5t (gate G-UR): TurboEdit and the IF
phase detector.

    python scripts/uom_u5t_verify.py [--verbose] [--tree DIR] [--mutate]

The frozen sources are read at f823a36 (the step's base) with `git show`; no copy
is stored.  Each source (TE: src/core/slip/TurboEditResolver.cpp, IF:
src/core/slip/IfPhaseSlipDetector.cpp) declares where every line of its frozen
functions went:

  UNITS    a function of the new files whose body is an ordered list of pieces:
             F(a, b, shift)   frozen lines a..b (code lines only), character for
                              character after the unit's substitutions, with
                              `shift` leading spaces removed;
             N(text)          a declared new code line (an alias preamble, a call,
                              a return), compared without surrounding space;
             G(if, close, t)  the frozen `if (COND) {` at `if` and its `}` at
                              `close` became the declared line `t` (a guard or a
                              one-line call); both frozen lines are carried;
           with `subs`: (frozen line, from, to), a plain text replacement on that
           line before comparison (each one listed for review at the end);
  REGIONS  frozen lines a..b carried unchanged as one contiguous run of code
           lines of a new file (functions the step did not touch);
  MEMBERS  frozen declarations that became members of a working value (or of a
           type in a header), with the member text;
  DROPS    frozen code lines no new body carries, each with its reason.

Exit 0 only when every check passes:
  1. Units.      Each unit's body code lines equal its pieces.
  2. Regions.    Each region is found once, contiguous, in its file.
  3. Coverage.   Every code line inside a function of the frozen file is carried
                 exactly once (unit, region, member or drop).
  4. Members.    Each member text is declared in its struct, and names the same
                 variable as the frozen declaration (same type too, unless the
                 type change is declared).
  5. Comments.   Every comment line of the frozen file is in the new files at
                 least as often (whitespace and comment markers normalized),
                 apart from the declared comment edits.
  6. Log calls.  The multiset of (level, format literal) of every log call of the
                 frozen file equals that of the new files, apart from the
                 declared log deletions and literal edits.
  7. Order.      The ordered list of log calls and state-changing calls of each
                 frozen driver equals the new driver's, with every call into a new
                 step replaced by that step's own list (recursively) and every
                 frozen lambda replaced by its body at its call sites.
  8. Aliases.    Every alias line of a unit (`... = w.x;`, `... = row.x;`) names a
                 variable the unit body uses.
  9. Headers.    The frozen class header's code lines appear in the new header in
                 order, apart from the declared header removals.
With --mutate the checks run a second time on a copy of the new files in which one
character of a moved body is changed; that run must FAIL.
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

FROZEN_REV = "f823a36"


def F(a, b=None, shift=0):
    return ("F", a, a if b is None else b, shift)


def N(text):
    return ("N", text)


def G(if_line, close_line, text):
    return ("G", if_line, close_line, text)


def R(a, b, reason):
    """Declared drops for every code line of frozen a..b (blank lines skipped)."""
    return {i: ("range", reason) for i in range(a, b + 1)}


# =============================================================================
# TurboEdit (step U5t-b: resolve cut into steps over EpochWork / RowWork)
# =============================================================================
S = "src/core/slip/"
TE_MAIN = S + "TurboEditResolver.cpp"
TE_CHAIN = S + "TurboEditChain.cpp"
TE_ADJ = S + "TurboEditAdjudication.cpp"
TE_BRIDGE = S + "TurboEditBridge.cpp"
TE_WORK = "include/slip/TurboEditEpochWork.hpp"
TE_HPP = "include/slip/TurboEditResolver.hpp"

# TE_UNITS: every function of the new TurboEdit files that carries frozen code.
TE_UNITS = [
    dict(file=TE_MAIN, def_re=r'^TurboEditResolver::EpochResolution TurboEditResolver::resolve\(', pieces=[
        N('EpochWork w(mw_results, mw_clean_pairs, observations, wl_gap_evidence,'),
        N('log_manager_.getLogger("turbo_edit_resolver"));'),
        N('adviseCoarseRate(w, interval_s);'),
        N('stampEpoch(w);'),
        N('indexMwFlags(w);'),
        N('collectChainRows(w);'),
        N('assessIonosphere(w);'),
        N('for (const auto& cached : w.cache)'),
        N('adjudicateRow(w, cached);'),
        N('reconcileStagedIntegers(w);'),
        N('withdrawContradictedVerdicts(w);'),
        N('logEpochSummary(w);'),
        N('finishEpoch(w);'),
        N('return std::move(w.out);'),
    ]),
    dict(file=TE_MAIN, def_re=r'^void TurboEditResolver::adviseCoarseRate\(', pieces=[
        N('const auto& log = w.log;'),
        N('auto& out = w.out;'),
        F(381, 397, 0),
    ]),
    dict(file=TE_MAIN, def_re=r'^void TurboEditResolver::stampEpoch\(', pieces=[
        N('const auto& observations = w.observations;'),
        N('auto& epoch_now_s = w.epoch_now_s;'),
        F(399, 399, 0),
        F(401, 406, 0),
    ]),
    dict(file=TE_MAIN, def_re=r'^void TurboEditResolver::logEpochSummary\(', pieces=[
        N('const auto& log = w.log;'),
        N('const auto& out = w.out;'),
        F(1466, 1475, 0),
    ]),
    dict(file=TE_MAIN, def_re=r'^void TurboEditResolver::finishEpoch\(', pieces=[
        N('const auto& epoch_now_s = w.epoch_now_s;'),
        F(1477, 1492, 0),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::indexMwFlags\(', pieces=[
        N('const auto& mw_results = w.mw_results;'),
        N('auto& mw_by_pair = w.mw_by_pair;'),
        F(408, 408, 0),
        F(418, 427, 0),
    ], subs=[
        (420, 'satOf(sr)', 'turbo_edit::satOf(sr)'),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::collectChainRows\(', pieces=[
        N('const auto& observations = w.observations;'),
        N('const auto& cache = w.cache;'),
        N('auto& pe_by_key = w.pe_by_key;'),
        F(429, 429, 0),
        F(446, 451, 0),
        N('trackSignalContinuity(obs, epoch_s);'),
        F(469, 469, 0),
        N('collectPairRow(w, obs, pr, epoch_s);'),
        F(661, 663, 0),
        F(695, 696, 0),
        F(698, 698, 0),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::trackSignalContinuity\(', pieces=[
        F(452, 455, 4),
        F(456, 467, 4),
    ], subs=[
        (461, 'sig_last_seen_s_.find(skey)', 'sig_runs_.find(skey)'),
        (462, 'ls == sig_last_seen_s_.end()', 'ls == sig_runs_.end()'),
        (463, 'ls->second < prev_epoch_s_', 'ls->second.last_seen_s < prev_epoch_s_'),
        (464, 'sig_continuous_since_s_[skey] = epoch_s;', 'sig_runs_[skey].continuous_since_s = epoch_s;'),
        (466, 'sig_last_seen_s_[skey] = epoch_s;', 'sig_runs_[skey].last_seen_s = epoch_s;'),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::collectPairRow\(', pieces=[
        N('auto& cache = w.cache;'),
        N('auto& cons_td2 = w.cons_td2;'),
        F(470, 478, 8),
        N('restartArcOnBreak(arc, epoch_s);'),
        F(563, 599, 8),
        G(600, 616, 'if (!cached_bridged) rollWarmUpChain(arc, gf, epoch_s);'),
        F(617, 625, 8),
        N('expireBridgeStash(arc, epoch_s);'),
        F(648, 660, 8),
    ], subs=[
        (471, 'continue;', 'return;'),
        (474, 'continue;', 'return;'),
        (617, 'continue;', 'return;'),
        (659, 'gfElevWeight(obs.elevation)', 'turbo_edit::gfElevWeight(obs.elevation)'),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::restartArcOnBreak\(', pieces=[
        F(479, 561, 8),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::rollWarmUpChain\(', pieces=[
        F(601, 615, 16),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::expireBridgeStash\(', pieces=[
        F(626, 636, 8),
        F(638, 645, 12),
    ]),
    dict(file=TE_CHAIN, def_re=r'^void TurboEditResolver::assessIonosphere\(', pieces=[
        N('const auto& cons_td2 = w.cons_td2;'),
        N('auto& ivi_by_cons = w.ivi_by_cons;'),
        N('auto& out = w.out;'),
        F(664, 664, 0),
        F(666, 679, 0),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::adjudicateRow\(', pieces=[
        N('RowWork row(cached, gf_arcs_[cached.key]);'),
        N('openRow(w, row);'),
        N('rerouteLateAcrossGap(w, row);'),
        N('if (row.pe.bridged) adjudicateBridged(w, row);'),
        N('else if (row.flagged) adjudicateFlagged(w, row);'),
        N('traceRow(row);'),
        N('advanceArc(w, row);'),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::openRow\(', pieces=[
        N('const auto& ivi_by_cons = w.ivi_by_cons;'),
        N('const auto& mw_by_pair = w.mw_by_pair;'),
        N('const auto& pe = row.pe;'),
        N('const auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('auto& ivi = row.ivi;'),
        N('auto& iono_stable_cons = row.iono_stable_cons;'),
        N('auto& split_gate_m = row.split_gate_m;'),
        N('auto& sigma_trace = row.sigma_trace;'),
        N('auto& mwit = row.mwit;'),
        N('auto& mw_flagged = row.mw_flagged;'),
        N('auto& gf_flagged = row.gf_flagged;'),
        N('auto& flagged = row.flagged;'),
        F(756, 771, 4),
    ], subs=[
        (757, 'const double ivi =', 'ivi ='),
        (758, 'const bool iono_stable_cons =', 'iono_stable_cons ='),
        (765, 'const double split_gate_m =', 'split_gate_m ='),
        (766, 'double sigma_trace =', 'sigma_trace ='),
        (768, 'auto mwit =', 'mwit ='),
        (769, 'const bool mw_flagged =', 'mw_flagged ='),
        (770, 'const bool gf_flagged =', 'gf_flagged ='),
        (771, 'const bool flagged    =', 'flagged    ='),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::stageInteger\(', pieces=[
        N('auto& cand = w.cand;'),
        N('auto& conflict_sats = w.conflict_sats;'),
        F(830, 833, 4),
        F(835, 838, 8),
        F(841, 843, 8),
    ], subs=[
        (838, 'sigmap[sig] = dN;', 'sigmap[sig] = StagedInteger{ dN, lam, fr };'),
        (841, 'it->second != dN', 'it->second.dN != dN'),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::adjudicateFlagged\(', pieces=[
        N('const auto& log = w.log;'),
        N('const auto& mw_clean_pairs = w.mw_clean_pairs;'),
        N('auto& out = w.out;'),
        N('const auto& pe = row.pe;'),
        N('const auto& sat = row.sat;'),
        N('const auto& mwit = row.mwit;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        N('const auto& iono_stable_cons = row.iono_stable_cons;'),
        N('const auto& sigma_trace = row.sigma_trace;'),
        N('const auto& split_gate_m = row.split_gate_m;'),
        N('auto& did_slip = row.did_slip;'),
        N('auto& dN1 = row.dN1;'),
        N('auto& dN2 = row.dN2;'),
        N('auto& frac2 = row.frac2;'),
        N('auto& dN_wl_trace = row.dN_wl_trace;'),
        N('auto& wl_known_trace = row.wl_known_trace;'),
        N('auto& action = row.action;'),
        F(1169, 1191, 8),
        N('const bool code_event = classifyCodeEvent(w, row, dN_wl, wl_known, denom);'),
        F(1266, 1327, 8),
    ], subs=[
        (1315, 'stage(pe.sig1,', 'stageInteger(w, sat, pe.sig1,'),
        (1316, 'stage(pe.sig2,', 'stageInteger(w, sat, pe.sig2,'),
    ]),
    dict(file=TE_ADJ, def_re=r'^bool TurboEditResolver::classifyCodeEvent\(', pieces=[
        N('const auto& log = w.log;'),
        N('auto& out = w.out;'),
        N('const auto& pe = row.pe;'),
        N('const auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        N('const auto& iono_stable_cons = row.iono_stable_cons;'),
        N('auto& dN1 = row.dN1;'),
        N('auto& dN2 = row.dN2;'),
        N('auto& frac2 = row.frac2;'),
        N('auto& action = row.action;'),
        F(1192, 1264, 8),
        N('return code_event;'),
    ], subs=[
        (1219, '!pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2)) {', '!turbo_edit::pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2)) {'),
        (1245, 'stage(pe.sig1,', 'stageInteger(w, sat, pe.sig1,'),
        (1246, 'stage(pe.sig2,', 'stageInteger(w, sat, pe.sig2,'),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::traceRow\(', pieces=[
        N('const auto& pe = row.pe;'),
        N('const auto& sat = row.sat;'),
        N('const auto& ivi = row.ivi;'),
        N('const auto& iono_stable_cons = row.iono_stable_cons;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        N('const auto& gf_flagged = row.gf_flagged;'),
        N('const auto& dN_wl_trace = row.dN_wl_trace;'),
        N('const auto& wl_known_trace = row.wl_known_trace;'),
        N('const auto& dN1 = row.dN1;'),
        N('const auto& dN2 = row.dN2;'),
        N('const auto& frac2 = row.frac2;'),
        N('const auto& action = row.action;'),
        N('const auto& sigma_trace = row.sigma_trace;'),
        N('const auto& split_gate_m = row.split_gate_m;'),
        F(1330, 1330, 4),
        G(1331, 1355, 'if (!trace_file_.is_open()) return;'),
        F(1332, 1354, 8),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::advanceArc\(', pieces=[
        N('const auto& epoch_now_s = w.epoch_now_s;'),
        N('const auto& pe = row.pe;'),
        N('auto& arc = row.arc;'),
        N('const auto& did_slip = row.did_slip;'),
        F(1357, 1407, 4),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::reconcileStagedIntegers\(', pieces=[
        N('const auto& cand = w.cand;'),
        N('const auto& cand_moot = w.cand_moot;'),
        N('const auto& conflict_sats = w.conflict_sats;'),
        N('auto& out = w.out;'),
        F(1410, 1439, 0),
    ], subs=[
        (1416, 'for (const auto& [sig, dN] : sigmap) {', 'for (const auto& [sig, staged] : sigmap) {'),
        (1417, 'if (dN == 0) {', 'if (staged.dN == 0) {'),
        (1434, 'r.dN        = dN;', 'r.dN        = staged.dN;'),
        (1435, 'r.lambda    = cand_lambda.at(sat).at(sig);', 'r.lambda    = staged.lambda;'),
        (1436, 'r.frac      = cand_frac.at(sat).at(sig);', 'r.frac      = staged.frac;'),
    ]),
    dict(file=TE_ADJ, def_re=r'^void TurboEditResolver::withdrawContradictedVerdicts\(', pieces=[
        N('auto& out = w.out;'),
        F(1441, 1464, 0),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^std::set<std::string> TurboEditResolver::affirmedZeroSignals\(', pieces=[
        N('const auto& pe_by_key = w.pe_by_key;'),
        F(719, 750, 4),
    ], subs=[
        (741, 'sig_continuous_since_s_.find(prefix + q.sig1)', 'sig_runs_.find(prefix + q.sig1)'),
        (742, 'sig_continuous_since_s_.find(prefix + q.sig2)', 'sig_runs_.find(prefix + q.sig2)'),
        (743, 'c1 == sig_continuous_since_s_.end()', 'c1 == sig_runs_.end()'),
        (744, 'c1->second > ref_epoch_s', 'c1->second.continuous_since_s > ref_epoch_s'),
        (745, 'c2 == sig_continuous_since_s_.end()', 'c2 == sig_runs_.end()'),
        (746, 'c2->second > ref_epoch_s', 'c2->second.continuous_since_s > ref_epoch_s'),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^void TurboEditResolver::rerouteLateAcrossGap\(', pieces=[
        N('const auto& log = w.log;'),
        N('const auto& epoch_now_s = w.epoch_now_s;'),
        N('auto& out = w.out;'),
        N('auto& pe = row.pe;'),
        N('auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        F(773, 821, 4),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^void TurboEditResolver::adjudicateBridged\(', pieces=[
        N('auto& out = w.out;'),
        N('const auto& pe = row.pe;'),
        N('auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('const auto& mwit = row.mwit;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        N('const auto& iono_stable_cons = row.iono_stable_cons;'),
        N('auto& dN_wl_trace = row.dN_wl_trace;'),
        N('auto& wl_known_trace = row.wl_known_trace;'),
        N('auto& action = row.action;'),
        F(847, 854, 8),
        G(855, 1167, 'if (!(mw_flagged || arc.wl_settle_pending)) return;'),
        F(856, 865, 12),
        G(866, 1148, 'if (trusted) actioned = adjudicateTrustedBridge(w, row);'),
        F(1149, 1166, 12),
    ], subs=[
        (863, '!pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2) &&', '!turbo_edit::pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2) &&'),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^bool TurboEditResolver::adjudicateTrustedBridge\(', pieces=[
        N('const auto& log = w.log;'),
        N('auto& out = w.out;'),
        N('const auto& pe = row.pe;'),
        N('const auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('const auto& mwit = row.mwit;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        N('auto& sigma_trace = row.sigma_trace;'),
        N('auto& did_slip = row.did_slip;'),
        N('const auto& dN_wl_trace = row.dN_wl_trace;'),
        N('auto& action = row.action;'),
        N('bool actioned = false;'),
        F(867, 953, 16),
        N('BridgeSolve solve;'),
        N('collectBridgeWlCandidates(w, row, solve);'),
        N('selectBridgeCandidate(w, row, solve, denom, zero_gate);'),
        N('actioned = applyBridgeSolve(w, row, solve);'),
        F(1147, 1147, 16),
        N('return actioned;'),
    ], subs=[
        (891, 'stage(pe.sig1,', 'stageInteger(w, sat, pe.sig1,'),
        (892, 'stage(pe.sig2,', 'stageInteger(w, sat, pe.sig2,'),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^void TurboEditResolver::collectBridgeWlCandidates\(', pieces=[
        N('const auto& log = w.log;'),
        N('const auto& wl_gap_evidence = w.wl_gap_evidence;'),
        N('const auto& pe = row.pe;'),
        N('const auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('const auto& mwit = row.mwit;'),
        N('const auto& mw_flagged = row.mw_flagged;'),
        N('auto& k_cands = solve.k_cands;'),
        N('auto& k_published = solve.k_published;'),
        N('auto& k_settled = solve.k_settled;'),
        F(957, 994, 20),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^void TurboEditResolver::selectBridgeCandidate\(', pieces=[
        N('const auto& log = w.log;'),
        N('const auto& pe = row.pe;'),
        N('const auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('const auto& k_cands = solve.k_cands;'),
        N('auto& best_k = solve.best_k;'),
        N('auto& best_dN2 = solve.best_dN2;'),
        N('auto& best_frac = solve.best_frac;'),
        N('auto& n_acceptable = solve.n_acceptable;'),
        N('auto& n_sibling_pruned = solve.n_sibling_pruned;'),
        F(995, 1000, 20),
        F(1005, 1047, 20),
    ], subs=[
        (1000, 'affirmedZeroSignals(sat, pe.key, arc.bridge_epoch_s);', 'affirmedZeroSignals(w, sat, pe.key, arc.bridge_epoch_s);'),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^bool TurboEditResolver::applyBridgeSolve\(', pieces=[
        N('const auto& log = w.log;'),
        N('auto& out = w.out;'),
        N('auto& cand_moot = w.cand_moot;'),
        N('const auto& pe = row.pe;'),
        N('const auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('auto& did_slip = row.did_slip;'),
        N('auto& dN1 = row.dN1;'),
        N('auto& dN2 = row.dN2;'),
        N('auto& frac2 = row.frac2;'),
        N('auto& dN_wl_trace = row.dN_wl_trace;'),
        N('auto& action = row.action;'),
        N('const auto& k_cands = solve.k_cands;'),
        N('const auto& k_published = solve.k_published;'),
        N('const auto& k_settled = solve.k_settled;'),
        N('const auto& best_k = solve.best_k;'),
        N('const auto& best_dN2 = solve.best_dN2;'),
        N('const auto& best_frac = solve.best_frac;'),
        N('const auto& n_acceptable = solve.n_acceptable;'),
        N('bool actioned = false;'),
        F(1048, 1093, 20),
        N('actioned = deferBridgeAdjudication(w, row, n_acceptable);'),
        F(1135, 1146, 20),
        N('return actioned;'),
    ], subs=[
        (1061, 'stage(sig, dN, lam, frac2);', 'stageInteger(w, sat, sig, dN, lam, frac2);'),
        (1070, 'sig_continuous_since_s_.find(sigkey)', 'sig_runs_.find(sigkey)'),
        (1073, '(cs != sig_continuous_since_s_.end() &&', '(cs != sig_runs_.end() &&'),
        (1074, 'cs->second <= arc.bridge_epoch_s', 'cs->second.continuous_since_s <= arc.bridge_epoch_s'),
    ]),
    dict(file=TE_BRIDGE, def_re=r'^bool TurboEditResolver::deferBridgeAdjudication\(', pieces=[
        N('const auto& log = w.log;'),
        N('const auto& epoch_now_s = w.epoch_now_s;'),
        N('auto& out = w.out;'),
        N('const auto& pe = row.pe;'),
        N('auto& arc = row.arc;'),
        N('const auto& sat = row.sat;'),
        N('auto& did_slip = row.did_slip;'),
        N('auto& action = row.action;'),
        N('bool actioned = false;'),
        F(1094, 1134, 24),
        N('return actioned;'),
    ]),
    dict(file=TE_WORK, def_re=r'^inline std::string satOf\(', pieces=[
        F(26, 28, 0),
    ]),
    dict(file=TE_WORK, def_re=r'^inline double gfElevWeight\(', pieces=[
        F(37, 41, 0),
    ]),
    dict(file=TE_WORK, def_re=r'^inline bool pairHasLockLoss\(', pieces=[
        F(50, 54, 0),
    ]),
]

TE_REGIONS = [
    # Construction, tracing, the GF combination helpers and the statistics:
    # unchanged, except the elevation weight's new namespace.
    dict(file=TE_MAIN, a=58, b=365,
         subs=[(290, "gfElevWeight(elev_deg)", "turbo_edit::gfElevWeight(elev_deg)")]),
    # The external resets (resetAll deleted in step U5t-a).
    dict(file=TE_MAIN, a=1497, b=1537),
]

# Frozen declarations that became members: (struct, member text).  `struct` is
# the name in `struct TurboEditResolver::<name> {` of TE_WORK.
TE_MEMBERS = {
    379: ("EpochWork", "EpochResolution out;"),
    400: ("EpochWork", "double epoch_now_s = 0.0;"),
    409: ("MwFlag", None),            # the struct's own opening line
    410: ("MwFlag", "int    dN_wl{ 0 };"),
    411: ("MwFlag", "bool   known{ false };"),
    415: ("MwFlag", "double wl_float{ std::numeric_limits<double>::quiet_NaN() };"),
    416: ("MwFlag", None),            # its closing `};`
    417: ("EpochWork", "std::map<std::string, MwFlag> mw_by_pair;"),
    430: ("PairEpoch", None),
    431: ("PairEpoch", "const PppObservation* obs;"),
    432: ("PairEpoch", "std::string key, sig1, sig2;"),
    433: ("PairEpoch", "double gf{ 0.0 }, td2{ 0.0 }, lambda1{ 0.0 }, lambda2{ 0.0 };"),
    434: ("PairEpoch", "double thresh{ std::numeric_limits<double>::quiet_NaN() };"),
    435: ("PairEpoch", "double thresh_unfloored{ std::numeric_limits<double>::quiet_NaN() };"),
    436: ("PairEpoch", "bool   gf_exceeds{ false };"),
    440: ("PairEpoch", "bool   bridged{ false };"),
    441: ("PairEpoch", "double gap_s{ 0.0 };"),
    442: ("PairEpoch", None),
    443: ("EpochWork", "std::vector<PairEpoch> cache;"),
    444: ("EpochWork", "std::map<char, std::vector<double>> cons_td2;"),
    665: ("EpochWork", "std::map<char, double> ivi_by_cons;"),
    683: ("EpochWork", "std::map<std::string, std::map<std::string, StagedInteger>> cand;"),
    692: ("EpochWork", "std::map<std::string, std::set<std::string>> cand_moot;"),
    693: ("EpochWork", "std::set<std::string> conflict_sats;"),
    697: ("EpochWork", "std::map<std::string, const PairEpoch*> pe_by_key;"),
    754: ("RowWork", "GfArc& arc;"),
    755: ("RowWork", "const std::string& sat;"),
    757: ("RowWork", "double ivi{ 0.0 };"),
    758: ("RowWork", "bool   iono_stable_cons{ true };"),
    765: ("RowWork", "double split_gate_m{ 0.0 };"),
    766: ("RowWork", "double sigma_trace{ 0.0 };"),
    768: ("RowWork", "std::map<std::string, MwFlag>::const_iterator mwit;"),
    769: ("RowWork", "bool   mw_flagged{ false };"),
    770: ("RowWork", "bool   gf_flagged{ false };"),
    771: ("RowWork", "bool   flagged{ false };"),
    823: ("RowWork", "bool did_slip = false;"),
    824: ("RowWork", "int  dN1 = 0, dN2 = 0;"),
    825: ("RowWork", "double frac2 = 0.0;"),
    826: ("RowWork", "int  dN_wl_trace = 0;"),
    827: ("RowWork", "bool wl_known_trace = true;"),
    828: ("RowWork", 'const char* action = "none";'),
    954: ("BridgeSolve", "std::vector<int> k_cands;"),
    955: ("BridgeSolve", "bool k_published = false;"),
    956: ("BridgeSolve", "bool k_settled   = false;"),
    1001: ("BridgeSolve", "int    best_k = 0, best_dN2 = 0;"),
    1002: ("BridgeSolve", "double best_frac = 1.0;"),
    1003: ("BridgeSolve", "int    n_acceptable = 0;"),
    1004: ("BridgeSolve", "int    n_sibling_pruned = 0;"),
}
# Declarations whose type the move spells differently (the variable is the same).
TE_TYPE_CHANGES = {
    683: "the three staging maps merged: the value type is StagedInteger {dN, lambda, frac}",
    754: "`auto&` bound by the RowWork constructor spells its type, GfArc&",
    768: "`auto` (a map iterator) spelled as the member's type, a const_iterator",
    757: "`const` dropped: openRow (A0) assigns the member once",
    758: "`const` dropped: openRow (A0) assigns the member once",
    765: "`const` dropped: openRow (A0) assigns the member once",
    769: "`const` dropped: openRow (A0) assigns the member once",
    770: "`const` dropped: openRow (A0) assigns the member once",
    771: "`const` dropped: openRow (A0) assigns the member once",
}
# Declarations carried by a unit as an assignment (substituted) and also members.
TE_MEMBER_ALSO_CARRIED = {757, 758, 765, 766, 768, 769, 770, 771}

TE_DROPS = {
    # The anonymous namespace's helpers became inline functions of namespace
    # turbo_edit in TE_WORK (their bodies are units there).
    25: "satOf signature: now `inline std::string satOf(` in slip/TurboEditEpochWork.hpp",
    29: "satOf closing brace (see 25)",
    36: "gfElevWeight signature: now inline in slip/TurboEditEpochWork.hpp",
    42: "gfElevWeight closing brace (see 36)",
    48: "pairHasLockLoss signature: now inline in slip/TurboEditEpochWork.hpp",
    49: "pairHasLockLoss signature, second line (see 48)",
    55: "pairHasLockLoss closing brace (see 48)",
    378: "the module logger is fetched once by the driver into EpochWork::log",
    684: "cand_lambda merged into cand (StagedInteger::lambda)",
    685: "cand_frac merged into cand (StagedInteger::frac)",
    716: "the affirmedZeroSignals lambda became a member function (its signature)",
    717: "the lambda's parameter list (see 716)",
    718: "the lambda's parameter list (see 716)",
    751: "the lambda's closing `};` (see 716)",
    753: "the pass-2 loop: the driver's `for (const auto& cached : w.cache) adjudicateRow(w, cached);`",
    834: "the stage lambda became the static member function stageInteger",
    839: "cand_lambda merged: StagedInteger{ dN, lam, fr } on line 838",
    840: "cand_frac merged: StagedInteger{ dN, lam, fr } on line 838",
    844: "the stage lambda's closing `};` (see 834)",
    846: "the row dispatch: `if (row.pe.bridged) adjudicateBridged(w, row);` in adjudicateRow",
    1168: "the row dispatch: `else if (row.flagged) adjudicateFlagged(w, row);` in adjudicateRow",
    1328: "the closing brace of the flagged branch (see 1168)",
    637: "the bare block became the function expireBridgeStash",
    646: "the bare block's closing brace (see 637)",
    1408: "the pass-2 loop's closing brace (see 753)",
    1494: "the driver returns the work's resolution: `return std::move(w.out);`",
    **R(1540, 1548, "resetAll deleted in step U5t-a (no caller)"),
}

TE_COMMENT_EDITS = {
    "for trace": "the RowWork member docs of dN1/dN2 and frac2 (two lines)",
    "@brief Implementation of the MW+GF joint cycle-slip repair resolver.":
        "file banner rewritten: it now lists the steps and the files that hold them",
    "Organization:": "file banner (see @brief)",
    "- Construction / tracing": "file banner (see @brief)",
    "- GF combination helpers": "file banner (see @brief)",
    "- Statistics / threshold / IVI": "file banner (see @brief)",
    "- resolve(): two-pass (collect TD2 → assess IVI → flag + joint solve)": "file banner (see @brief)",
    "- External reset": "file banner (see @brief)",
    "namespace": "the anonymous namespace's closing comment (its helpers moved to namespace turbo_edit)",
    "resolve(): two-pass joint solve": "section banner now `resolve(): the per-epoch driver`",
    "by value: the late-flag re-route below mutates the row":
        "the RowWork::pe doc (`The row, by value: the late across-gap re-route (A1) rewrites ...`)",
}

# Calls whose order the event check follows, besides the log calls and the steps.
TE_ACTIONS = {
    "computeAdaptiveThreshold", "effectiveSigma", "splitCapable", "computeMadSigma",
    "computeIvi", "finalizeXsatSigmas", "collectPhasePairs", "computeGfMeters",
    "getWavelength", "xsatGroupKey", "gfElevWeight", "pairHasLockLoss", "satOf", "arcKey",
    "affirmedZeroSignals", "stageInteger", "continuousGpsSeconds", "push_back", "insert",
    "erase", "flush", "dispatch",
}

# Frozen header lines the step removed (normalized code text -> reason).
TE_HEADER_REMOVED = {
    "void setCorrections(const GnssCorrections* corrections) { corrections_ = corrections; }":
        "U5t-a: setCorrections stores nothing; corrections_ was never read",
    "void resetAll();": "U5t-a: resetAll had no caller",
    "const GnssCorrections* corrections_{ nullptr };": "U5t-a: never read",
    "std::map<std::string, double> sig_last_seen_s_;":
        "U5t-b: the two continuity maps merged into sig_runs_ (SignalRun)",
    "std::map<std::string, double> sig_continuous_since_s_;": "U5t-b: see sig_last_seen_s_",
}

# =============================================================================
# IF phase detector (step U5t-c: processDoubleDifferences' values and steps; the
# staged user path in one bannered file)
# =============================================================================
IF_MAIN = S + "IfPhaseSlipDetector.cpp"
IF_STAGED = S + "IfPhaseStagedUserPath.cpp"
IF_WORK = "include/slip/IfPhaseChainWork.hpp"
IF_HPP = "include/slip/IfPhaseSlipDetector.hpp"

# IF_UNITS: every function of the new IF detector files that carries frozen code.
IF_UNITS = [
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::logSlipRecord\(', pieces=[
        F(77, 78, 0),
        F(80, 85, 4),
    ], subs=[
        (81, 'slip_reason={} pos_sigma={:.3f}{}",', 'slip_reason={} pos_sigma={:.3f}",'),
        (85, 'position_sigma_3d, pos_gated ? " [pos_gated]" : "");', 'position_sigma_3d);'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    IfPhaseSlipDetector::groupByConstellation\(', pieces=[
        F(222, 229, 0),
        N('groupCombinedIfRow(obs, phase_if_key, result);'),
        F(247, 247, 0),
        N('groupUncombinedRows(obs, result);'),
        F(320, 323, 0),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::groupUncombinedRows\(', pieces=[
        F(248, 319, 8),
    ], subs=[
        (258, 'continue;', 'return;'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    IfPhaseSlipDetector::ChainResult IfPhaseSlipDetector::processDoubleDifferences\(', pieces=[
        N('ChainWork w(chain, cs, log_manager_.getLogger("if_slip_detector"));'),
        N('const auto& sys_str = chain.sys_str;'),
        N('auto& run_ref_arm = plan.run_ref_arm;'),
        N('auto& inherit = plan.inherit;'),
        F(624, 642, 0),
        N('indexChainSatellites(w);'),
        F(649, 650, 0),
        N('rebaseOnReferenceChange(w);'),
        N('formChainDoubleDifferences(w);'),
        N('applyGeometryCorrection(w);'),
        F(698, 717, 0),
        F(798, 798, 0),
        F(892, 908, 0),
        N('inheritReferenceVerdict(w, inherit);'),
        F(921, 927, 0),
        G(928, 968, 'if (!defer_flagging_) flagNonReferenceOutliers(w);'),
        F(970, 970, 0),
    ], subs=[
        (649, 'if (!ref_data) return {};', 'if (!w.ref_data) return std::move(w.result);'),
        (700, 'return delta_grad;', 'return std::move(w.result);'),
        (798, 'if (run_ref_arm)', 'if (run_ref_arm) runReferenceArm(w);'),
        (970, 'return delta_grad;', 'return std::move(w.result);'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::indexChainSatellites\(', pieces=[
        N('const auto& sat_vec = w.chain.sat_vec;'),
        N('const auto& cs = w.cs;'),
        N('auto& sat_map = w.sat_map;'),
        N('auto& ref_data = w.ref_data;'),
        F(643, 643, 4),
        F(645, 647, 4),
        F(648, 648, 4),
    ], subs=[
        (648, 'const SatData* ref_data =', 'ref_data ='),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::rebaseOnReferenceChange\(', pieces=[
        N('const auto& logger = w.logger;'),
        N('const auto& sys_str = w.chain.sys_str;'),
        N('const auto& grad_phi_if = w.chain.grad_phi_if;'),
        N('const auto& ref_changed = w.chain.ref_changed;'),
        N('auto& cs = w.cs;'),
        F(651, 665, 0),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::formChainDoubleDifferences\(', pieces=[
        N('const auto& grad_phi_if = w.chain.grad_phi_if;'),
        N('const auto& cs = w.cs;'),
        N('auto& delta_grad = w.result.delta_grad;'),
        N('auto& all_finite = w.all_finite;'),
        F(667, 667, 0),
        N('formDoubleDifferences(grad_phi_if, cs, delta_grad, all_finite);'),
        F(679, 680, 0),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::formDoubleDifferences\(', pieces=[
        F(670, 678, 0),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::runReferenceArm\(', pieces=[
        N('const auto& sys_str = w.chain.sys_str;'),
        N('const auto& lambda_if = w.chain.lambda_if;'),
        N('auto& cs = w.cs;'),
        N('const auto& sat_map = w.sat_map;'),
        N('const auto& ref_data = w.ref_data;'),
        N('const auto& delta_grad = w.result.delta_grad;'),
        N('auto& outcome = w.result.outcome;'),
        F(804, 883, 4),
        F(885, 887, 8),
        F(889, 889, 4),
    ], subs=[
        (878, 'applyRefFire(median_dd', 'applyRefFire(w, median_dd'),
        (885, 'out_outcome->', 'outcome.'),
        (886, 'out_outcome->', 'outcome.'),
        (887, 'out_outcome->', 'outcome.'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::applyRefFire\(', pieces=[
        N('const auto& logger = w.logger;'),
        N('const auto& sys_str = w.chain.sys_str;'),
        N('const auto& grad_phi_if = w.chain.grad_phi_if;'),
        N('const auto& lambda_if = w.chain.lambda_if;'),
        N('const auto& position_sigma_3d = w.chain.position_sigma_3d;'),
        N('auto& cs = w.cs;'),
        N('const auto& ref_data = w.ref_data;'),
        N('auto& delta_grad = w.result.delta_grad;'),
        N('auto& reset_keys = w.result.reset_keys;'),
        N('auto& all_finite = w.all_finite;'),
        F(727, 786, 4),
        N('formDoubleDifferences(grad_phi_if, cs, delta_grad, all_finite);'),
    ], subs=[
        (749, 'writeTraceRecord(*ref_data, sys_str, 0.0, median_dd,', 'logSlipRecord(*ref_data, sys_str, 0.0, median_dd,'),
        (750, 'cs.ref_satellite, false, lambda_if, true, "ref_slip",', 'cs.ref_satellite, lambda_if, "ref_slip",'),
        (751, 'position_sigma_3d, false);', 'position_sigma_3d);'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::inheritReferenceVerdict\(', pieces=[
        N('const auto& sys_str = w.chain.sys_str;'),
        N('const auto& lambda_if = w.chain.lambda_if;'),
        N('const auto& cs = w.cs;'),
        N('const auto& delta_grad = w.result.delta_grad;'),
        F(909, 920, 4),
    ], subs=[
        (919, 'applyRefFire(median_dd', 'applyRefFire(w, median_dd'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    std::vector<std::string> IfPhaseSlipDetector::detect\(', pieces=[
        F(983, 983, 0),
        F(985, 995, 0),
        N('const bool pos_gated = gateEpoch(position_sigma_3d, model_nuisance_m);'),
        F(1011, 1110, 0),
        N('const ChainEpoch chain{ sys_str, sat_vec, grad_phi_if, lambda_if, ref_changed,'),
        N('delta_pos, position_sigma_3d };'),
        F(1111, 1114, 0),
        N('ChainResult result = processDoubleDifferences(chain, cs,'),
        N('RefArmPlan{ first_pair_chain || run_extra_own_arm,'),
        N('inherit });'),
        N('delta_grad = std::move(result.delta_grad);'),
        N('reset_keys.insert(reset_keys.end(), result.reset_keys.begin(),'),
        N('result.reset_keys.end());'),
        F(1121, 1129, 0),
        N('publishChainJumps(chain, cs, delta_grad, first_pair_chain, pos_gated);'),
        N('storeChainGradients(chain, cs, reset_keys);'),
        F(1209, 1216, 0),
    ], subs=[
        (1121, '!outcome.satellite.empty()', '!result.outcome.satellite.empty()'),
        (1122, '= outcome;', '= result.outcome;'),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::publishChainJumps\(', pieces=[
        N('const auto& sys_str = chain.sys_str;'),
        N('const auto& sat_vec = chain.sat_vec;'),
        N('const auto& lambda_if = chain.lambda_if;'),
        F(1130, 1136, 4),
        F(1156, 1189, 4),
    ]),
    dict(file=IF_MAIN, def_re=r'^    void IfPhaseSlipDetector::storeChainGradients\(', pieces=[
        N('const auto& grad_phi_if = chain.grad_phi_if;'),
        F(1191, 1193, 4),
        F(1195, 1201, 4),
        F(1206, 1207, 4),
    ]),
    dict(file=IF_STAGED, def_re=r'^    bool IfPhaseSlipDetector::gateEpoch\(', pieces=[
        F(984, 984, 0),
        F(996, 1009, 0),
        N('return pos_gated;'),
    ]),
    dict(file=IF_STAGED, def_re=r'^    void IfPhaseSlipDetector::groupCombinedIfRow\(', pieces=[
        F(230, 246, 8),
    ], subs=[
        (232, 'continue;', 'return;'),
        (236, 'continue;', 'return;'),
        (239, 'continue;', 'return;'),
        (242, 'continue;', 'return;'),
    ]),
    dict(file=IF_STAGED, def_re=r'^    void IfPhaseSlipDetector::applyGeometryCorrection\(', pieces=[
        N('const auto& delta_pos = w.chain.delta_pos;'),
        N('const auto& ref_data = w.ref_data;'),
        N('const auto& sat_map = w.sat_map;'),
        N('auto& delta_grad = w.result.delta_grad;'),
        F(682, 696, 0),
    ]),
    dict(file=IF_STAGED, def_re=r'^    void IfPhaseSlipDetector::flagNonReferenceOutliers\(', pieces=[
        N('const auto& logger = w.logger;'),
        N('const auto& sys_str = w.chain.sys_str;'),
        N('const auto& grad_phi_if = w.chain.grad_phi_if;'),
        N('const auto& lambda_if = w.chain.lambda_if;'),
        N('const auto& position_sigma_3d = w.chain.position_sigma_3d;'),
        N('const auto& cs = w.cs;'),
        N('const auto& sat_map = w.sat_map;'),
        N('auto& delta_grad = w.result.delta_grad;'),
        N('auto& reset_keys = w.result.reset_keys;'),
        N('const auto& all_finite = w.all_finite;'),
        F(929, 967, 0),
    ], subs=[
        (943, 'writeTraceRecord(', 'logSlipRecord('),
        (945, 'cs.ref_satellite, ref_changed, lambda_if,', 'cs.ref_satellite, lambda_if,'),
        (946, 'true, "non_ref_outlier", position_sigma_3d, false);', '"non_ref_outlier", position_sigma_3d);'),
        (961, 'writeTraceRecord(', 'logSlipRecord('),
        (964, 'dd, cs.ref_satellite, ref_changed, lambda_if,', 'dd, cs.ref_satellite, lambda_if,'),
        (965, 'true, "fixed_threshold", position_sigma_3d, false);', '"fixed_threshold", position_sigma_3d);'),
    ]),
]


IF_REGIONS = [
    dict(file=IF_MAIN, a=30, b=37),        # the constructor
    dict(file=IF_MAIN, a=114, b=160),      # the signal helpers and computeMedian
    dict(file=IF_STAGED, a=162, b=213),    # checkOutliers (staged)
    dict(file=IF_MAIN, a=326, b=602),      # selectReference and the hardened reference gate
    # The external resets and re-seeds (prev_los erases deleted in step U5t-a).
    dict(file=IF_MAIN, a=1223, b=1229),
    dict(file=IF_MAIN, a=1231, b=1253),
    dict(file=IF_MAIN, a=1255, b=1305),
]

IF_MEMBERS = {
    644: ("ChainWork", "std::map<std::string, const SatData*> sat_map;"),
    648: ("ChainWork", "const SatData* ref_data = nullptr;"),
    668: ("ChainResult", "std::map<std::string, double> delta_grad;"),
    669: ("ChainWork", "bool all_finite = true;"),
}
IF_TYPE_CHANGES = {}
IF_MEMBER_ALSO_CARRIED = {648}

IF_DROPS = {
    # Step U5t-a (map SG-19).
    **R(40, 66, "U5t-a: enableTracing had no caller (the trace file could not be opened)"),
    79: "U5t-a: every remaining caller of the slip log line passes is_slip true",
    **R(86, 94, "U5t-a: the non-slip debug branch, reached only from the deleted trace loop"),
    96: "U5t-a: the trace file (never opened) is deleted",
    **R(98, 107, "U5t-a: the trace file (never opened) is deleted"),
    **R(1138, 1154, "U5t-a: the trace loop of detect(), guarded by the never-open trace file"),
    1194: "U5t-a: ConstellationState::prev_los was never read",
    **R(1202, 1205, "U5t-a: prev_los (never read)"),
    1230: "U5t-a: prev_los (never read)",
    1254: "U5t-a: prev_los (never read)",
    **R(1308, 1311, "U5t-a: resetAll had no caller"),
    **R(1316, 1319, "U5t-a: getLastIfJump had no caller"),
    # Step U5t-c.
    622: "the module logger is fetched once by the driver into ChainWork::logger",
    725: "the applyRefFire lambda became a member function (its signature)",
    726: "the lambda's parameter list (see 725)",
    796: "the lambda's closing `};` (see 725)",
    **R(787, 795, "the double-difference loop of the rebase: identical to frozen 670-678 "
                  "(checked), now the shared formDoubleDifferences"),
    803: "the bare block of the reference arm became runReferenceArm",
    890: "the bare block's closing brace (see 803)",
    884: "the outcome is always recorded in ChainResult::outcome; detect keeps it for the "
         "first chain only, the one chain that passed a non-null out_outcome",
    888: "the closing brace of that test (see 884)",
    1115: "the outcome is returned in ChainResult::outcome",
    **R(1116, 1120, "the twelve-argument call became "
                    "processDoubleDifferences(chain, cs, RefArmPlan{...})"),
}
# Frozen code lines dropped because an identical run is carried elsewhere:
# (dropped a, b, carried a, b), compared without leading space.
IF_DUPLICATES = [(787, 795, 670, 678)]

IF_COMMENT_EDITS = {
    "- Construction / tracing": "file banner: `- Construction / the slip log line`",
    "- Statistics (median, MAD outlier check)": "file banner: `- Statistics (median)`",
    "- External reset": "file banner: `- External reset and re-seed`",
    "Construction / tracing": "section banner: `Construction / the slip log line`",
    "Statistics (median, MAD outlier check)": "section banner: the MAD outlier check is staged",
    "Write trace records for non-slip satellites": "U5t-a: the trace loop is deleted",
    "- Double-difference processing": "file banner: now names the driver and its steps",
    "if (!defer_flagging_)": "the closing brace's trailing comment: the block became the call "
                             "`if (!defer_flagging_) flagNonReferenceOutliers(w);`",
    "out_outcome=": "an argument comment of the twelve-argument call (out_outcome is now "
                    "ChainResult::outcome)",
}

# (level, substring of the frozen format literal) -> None (deleted) or
# (level, substring of the new literal) (edited).
IF_LOG_EDITS = {
    ("warn", "Failed to create log directory"): None,
    ("warn", "Could not open IF slip trace file"): None,
    ("info", "IF slip tracing enabled"): None,
    ("debug", "reason={} pos_sigma"): None,
    ("debug", "All IF phase slip state cleared"): None,
    ("info", "slip_reason={} pos_sigma={:.3f}{}"): ("info", 'slip_reason={} pos_sigma={:.3f}"'),
}

IF_ACTIONS = {
    "push_back", "erase", "clear", "computeMedian", "updateRefDispersion", "evaluateRefGate",
    "selectReference", "groupByConstellation", "logSlipRecord", "checkOutliers",
}

IF_HEADER_REMOVED = {
    "#include <filesystem>": "U5t-a: no trace file",
    "#include <fstream>": "U5t-a: no trace file",
    "void resetAll();": "U5t-a: no caller",
    "IfJumpInfo getLastIfJump(const std::string& satellite) const;": "U5t-a: no caller",
    "bool enableTracing(const std::filesystem::path& log_dir);": "U5t-a: no caller",
    "bool isTracingEnabled() const { return trace_file_.is_open(); }": "U5t-a: no trace file",
    "std::map<std::string, Eigen::Vector3d> prev_los;": "U5t-a: never read",
    "void writeTraceRecord(const SatData& sd, const std::string& sys_str,": "U5t-a: now logSlipRecord",
    "const std::string& ref_sat, bool ref_changed,": "U5t-a: logSlipRecord drops ref_changed",
    "double lambda_if, bool is_slip,": "U5t-a: logSlipRecord drops is_slip",
    "double position_sigma_3d, bool pos_gated) const;": "U5t-a: logSlipRecord drops pos_gated",
    "mutable std::ofstream trace_file_;": "U5t-a: no trace file",
    "std::map<std::string, double> processDoubleDifferences(": "U5t-c: the new signature",
    "const std::vector<SatData>& sat_vec,": "U5t-c: now ChainEpoch::sat_vec",
    "const std::string& sys_str,": "U5t-c: now ChainEpoch::sys_str",
    "ConstellationState& cs,": "U5t-c: the new signature",
    "const std::map<std::string, double>& grad_phi_if,": "U5t-c: now ChainEpoch::grad_phi_if",
    "double lambda_if,": "U5t-c: now ChainEpoch::lambda_if",
    "bool ref_changed,": "U5t-c: now ChainEpoch::ref_changed",
    "double position_sigma_3d,": "U5t-c: now ChainEpoch::position_sigma_3d",
    "const Eigen::Vector3d& delta_pos,": "U5t-c: now ChainEpoch::delta_pos",
    "std::vector<std::string>& reset_keys,": "U5t-c: now ChainResult::reset_keys",
    "bool run_ref_arm = true,": "U5t-c: now RefArmPlan::run_ref_arm",
    "RefArmOutcome* out_outcome = nullptr,": "U5t-c: now ChainResult::outcome",
    "const RefArmOutcome* inherit = nullptr);": "U5t-c: now RefArmPlan::inherit",
}

SOURCES = {
    "TE": dict(
        path="src/core/slip/TurboEditResolver.cpp",
        header=TE_HPP,
        new_files=[TE_MAIN, TE_CHAIN, TE_ADJ, TE_BRIDGE, TE_WORK, TE_HPP],
        work_header=TE_WORK, work_prefix="TurboEditResolver::",
        units=TE_UNITS, regions=TE_REGIONS, members=TE_MEMBERS,
        type_changes=TE_TYPE_CHANGES, also_carried=TE_MEMBER_ALSO_CARRIED,
        drops=TE_DROPS, comment_edits=TE_COMMENT_EDITS,
        log_edits={}, header_removed=TE_HEADER_REMOVED,
        drivers=[dict(name="resolve", body=(378, 1494))],
        lambdas={"affirmedZeroSignals": (716, 751, "leaf"), "stage": (834, 844, "leaf")},
        renames={"stage": "stageInteger"},
        leaf={"stageInteger", "affirmedZeroSignals", "satOf", "gfElevWeight", "pairHasLockLoss"},
        frozen_expand={},
        actions=TE_ACTIONS),
}
SOURCES["IF"] = dict(
    path="src/core/slip/IfPhaseSlipDetector.cpp",
    header=IF_HPP,
    new_files=[IF_MAIN, IF_STAGED, IF_WORK, IF_HPP],
    work_header=IF_WORK, work_prefix="IfPhaseSlipDetector::",
    units=IF_UNITS, regions=IF_REGIONS, members=IF_MEMBERS,
    type_changes=IF_TYPE_CHANGES, also_carried=IF_MEMBER_ALSO_CARRIED,
    drops=IF_DROPS, duplicates=IF_DUPLICATES, comment_edits=IF_COMMENT_EDITS,
    log_edits=IF_LOG_EDITS, header_removed=IF_HEADER_REMOVED,
    drivers=[dict(name="detect", body=(983, 1216))],
    lambdas={"applyRefFire": (725, 796, "expand")},
    renames={"writeTraceRecord": "logSlipRecord"},
    leaf={"logSlipRecord", "groupByConstellation", "groupUncombinedRows", "groupCombinedIfRow",
          "checkOutliers"},
    frozen_expand={"processDoubleDifferences": (622, 970)},
    event_skip=[(1137, 1155), (1194, 1194)],
    actions=IF_ACTIONS)


# -----------------------------------------------------------------------------
TREE = ROOT


def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def read_new(path):
    return (TREE / path).read_text(encoding="utf-8").replace("\r\n", "\n")


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
            t = m.group()[2:].lstrip("/!").lstrip("<").strip()
            if t:
                out.append(t)
        elif m.lastgroup == "bc":
            for part in m.group()[2:-2].split("\n"):
                t = part.strip().lstrip("*!").lstrip("<").strip()
                if t:
                    out.append(t)
    return out


def norm(s):
    return re.sub(r"\s+", " ", s.strip())


def _strip_nested(s, open_, close_):
    pat = re.compile(re.escape(open_) + r"[^" + re.escape(open_ + close_) + r"]*" + re.escape(close_))
    prev = None
    while prev != s:
        prev, s = s, pat.sub("", s)
    return s


def decl_core(s):
    """A declaration without const, initializer or brace-init: its type and names."""
    s = norm(s).rstrip(";").strip()
    s = _strip_nested(s, "{", "}")
    s = _strip_nested(s, "(", ")")
    s = re.sub(r"\s*=[^,]*", "", s)
    return re.sub(r"^const ", "", s).strip()


def decl_names(s):
    """The declared variable names of a one-line declaration."""
    core = _strip_nested(decl_core(s), "<", ">")
    names = []
    for p in core.split(","):
        m = re.search(r"([A-Za-z_]\w*)\s*$", p.strip())
        if m:
            names.append(m.group(1))
    return names


def unshift(line, k, where):
    if k > 0:
        if line[:k] != " " * k:
            raise ValueError(f"{where}: fewer than {k} leading spaces")
        return line[k:]
    return " " * (-k) + line


class Frozen:
    def __init__(self, path):
        self.text = git_show(FROZEN_REV, path)
        self.raw = [""] + self.text.split("\n")
        self.code = [""] + strip_comments(self.text).split("\n")
        self.blank = [""] + blank_literals(strip_comments(self.text)).split("\n")

    def function_bodies(self):
        """1-based (first, last) body lines of every block at namespace depth 2."""
        bodies, depth, i = [], 0, 1
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
    text = read_new(path)
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


# --- log calls and the event list ---------------------------------------------
LOG_RE = re.compile(r"->(info|warn|error|debug|critical|trace)\s*\(")
EV_RE = re.compile(r"->(info|warn|error|debug|critical|trace)\s*\(|\b([A-Za-z_]\w*)\s*\(")


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


def log_calls(text):
    return [(m.group(1), literal_after(text, m.end())) for m in LOG_RE.finditer(text)]


def raw_events(text, names, actions, renames):
    ev = []
    for m in EV_RE.finditer(text):
        if m.group(1):
            ev.append(("log", m.group(1), literal_after(text, m.end())))
        else:
            n = renames.get(m.group(2), m.group(2))
            if n in actions or n in names:
                ev.append(("call", n))
    return ev


def alias_lint(unit, body, problems):
    """Every alias `X = w.Y;` / `X = row.Y;` / `X = solve.Y;` / `X = c.Y;` is used."""
    lines = body.split("\n")
    for i, l in enumerate(lines):
        m = re.match(r"^\s*(?:const )?auto& (\w+) = (?:w|row|solve|chain|plan)\.[\w.]+;\s*$", l)
        if not m:
            continue
        rest = "\n".join(lines[:i] + lines[i + 1:])
        if not re.search(r"\b" + re.escape(m.group(1)) + r"\b", rest):
            problems.append(f"{unit['def_re']} ({unit['file']}): alias `{m.group(1)}` is never used")


# -----------------------------------------------------------------------------
def check_source(key, src, verbose, problems, summary):
    Fz = Frozen(src["path"])
    cover, bodies, n_ok = {}, {}, 0
    section_names = set()
    for u in src["units"]:
        try:
            exp, covered = expected(Fz, u, problems)
            act, body = actual(u["file"], u["def_re"])
        except ValueError as e:
            problems.append(f"{key}: {e}")
            continue
        name = re.search(r"(\w+)\\\($", u["def_re"]).group(1)
        u["name"] = name
        bodies[name] = body
        section_names.add(name)
        alias_lint(u, body, problems)
        for i in covered:
            cover.setdefault(i, []).append(name)
        if compare(u, exp, act, problems):
            n_ok += 1
            if verbose:
                print(f"OK   {key} {name:32s} {u['file']}  "
                      f"({sum(1 for e in exp if e[0] == 'frozen')} frozen, "
                      f"{sum(1 for e in exp if e[0] == 'new')} declared code lines)")

    # 2. Regions.
    n_reg = 0
    for r in src["regions"]:
        subs = {}
        for line, a, b in r.get("subs", []):
            subs.setdefault(line, []).append((a, b))
        want, idx = [], []
        for i in range(r["a"], r["b"] + 1):
            line = Fz.code[i]
            if not line.strip():
                continue
            for x, y in subs.get(i, []):
                if x not in line:
                    problems.append(f"{key}: region substitution {x!r} not on frozen line {i}")
                line = line.replace(x, y)
            want.append(line.rstrip())
            idx.append(i)
        have = [l.rstrip() for l in strip_comments(read_new(r["file"])).split("\n") if l.strip()]
        hits = [j for j in range(len(have) - len(want) + 1) if have[j:j + len(want)] == want]
        if len(hits) != 1:
            problems.append(f"{key}: region frozen {r['a']}..{r['b']} found {len(hits)} times in {r['file']}")
        else:
            n_reg += 1
            for i in idx:
                cover.setdefault(i, []).append("region")

    # 3. Coverage.
    for i in src["members"]:
        if i not in src["also_carried"]:
            cover.setdefault(i, []).append("member")
    for i, why in src["drops"].items():
        if not Fz.code[i].strip():
            if not isinstance(why, tuple):
                problems.append(f"{key}: declared drop {i} is not a code line")
            continue
        cover.setdefault(i, []).append("drop")
    n_code = 0
    for a, b in Fz.function_bodies():
        for i in range(a, b + 1):
            if not Fz.code[i].strip():
                continue
            n_code += 1
            if i not in cover:
                problems.append(f"{key}: frozen line {i} is carried by nothing: {Fz.code[i].strip()[:90]}")
    for da, db, ca, cb in src.get("duplicates", []):
        d = [Fz.code[i].strip() for i in range(da, db + 1) if Fz.code[i].strip()]
        c = [Fz.code[i].strip() for i in range(ca, cb + 1) if Fz.code[i].strip()]
        if d != c:
            problems.append(f"{key}: frozen {da}..{db} is not identical to frozen {ca}..{cb}")
        if any(i not in src["drops"] for i in range(da, db + 1) if Fz.code[i].strip()):
            problems.append(f"{key}: duplicate {da}..{db} is not declared dropped")
    twice = {i: w for i, w in cover.items() if len(w) > 1}
    for i, w in sorted(twice.items()):
        problems.append(f"{key}: frozen line {i} is carried twice ({', '.join(w)})")

    # 4. Members.
    wtext = strip_comments(read_new(src["work_header"]))
    wblank = blank_literals(wtext).split("\n")
    wlines = wtext.split("\n")
    structs = {}
    for i, l in enumerate(wlines):
        m = re.match(r"^struct " + re.escape(src["work_prefix"]) + r"(\w+)\b", l)
        if m:
            o, c = brace_close(wblank, i)
            structs[m.group(1)] = {norm(x) for x in wlines[o + 1:c] if x.strip()}
    n_mem = 0
    for i, (struct, text) in src["members"].items():
        if struct not in structs:
            problems.append(f"{key}: struct {src['work_prefix']}{struct} not found in {src['work_header']}")
            continue
        if text is None:
            fl = norm(Fz.code[i])
            if not (fl.startswith("struct ") or fl == "};"):
                problems.append(f"{key}: frozen {i} declared as a struct line but is `{fl}`")
            n_mem += 1
            continue
        if norm(text) not in structs[struct]:
            problems.append(f"{key}: member of frozen {i} not declared as `{text}` in {struct}")
            continue
        if i in src["type_changes"]:
            if decl_names(Fz.code[i]) != decl_names(text):
                problems.append(f"{key}: member for frozen {i} names {decl_names(text)}, "
                                f"the frozen line {decl_names(Fz.code[i])}")
        elif decl_core(Fz.code[i]) != decl_core(text):
            problems.append(f"{key}: member text for frozen {i} does not match the frozen declaration "
                            f"({decl_core(Fz.code[i])!r} against {decl_core(text)!r})")
        n_mem += 1

    # 5. Comments.
    frozen_c = Counter(norm(c) for c in comment_lines(Fz.text))
    new_c = Counter()
    for f in src["new_files"]:
        new_c.update(norm(c) for c in comment_lines(read_new(f)))
    edits = {norm(k) for k in src["comment_edits"]}
    lost = [(c, n - new_c[c]) for c, n in frozen_c.items() if new_c[c] < n and c not in edits]
    for c, n in lost:
        problems.append(f"{key}: comment line lost ({n}x): {c[:100]}")

    # 6. Log calls (multiset).
    flog = Counter(log_calls(strip_comments(Fz.text)))
    nlog = Counter()
    for f in src["new_files"]:
        nlog.update(log_calls(strip_comments(read_new(f))))
    for (lvl, sub), change in src["log_edits"].items():
        hits = [k_ for k_ in flog if k_[0] == lvl and sub in k_[1]]
        if len(hits) != 1:
            problems.append(f"{key}: declared log edit ({lvl}, {sub!r}) matches {len(hits)} frozen calls")
            continue
        flog[hits[0]] -= 1
        if change is not None:      # literal edited: (level, substring of the new literal)
            new_hits = [k_ for k_ in nlog if k_[0] == change[0] and change[1] in k_[1]]
            if len(new_hits) != 1:
                problems.append(f"{key}: declared new literal {change!r} matches {len(new_hits)} new calls")
                continue
            flog[new_hits[0]] += 1
    flog = +flog
    if flog != nlog:
        for k_ in sorted(set(flog) | set(nlog)):
            if flog[k_] != nlog[k_]:
                problems.append(f"{key}: log call {k_[0]} {k_[1][:80]} frozen x{flog[k_]}, new x{nlog[k_]}")

    # 7. Event order.
    actions, renames, leaf = src["actions"], src["renames"], src["leaf"]
    expand_names = section_names - leaf

    def frozen_events(a, b):
        text_lines = Fz.code[a:b + 1]
        lam_spans = [(s, e) for n_, (s, e, _) in src["lambdas"].items() if a <= s <= b]
        lam_spans += [(s, e) for s, e in src.get("event_skip", []) if a <= s <= b]
        keep = []
        for k_, line in enumerate(text_lines, start=a):
            if any(s <= k_ <= e for s, e in lam_spans):
                continue
            keep.append(line)
        ev = []
        for e_ in raw_events("\n".join(keep), set(src["lambdas"]) | set(src["frozen_expand"]), actions, renames):
            n_ = e_[1] if e_[0] == "call" else None
            if n_ in src["lambdas"] and src["lambdas"][n_][2] == "expand":
                s, e, _ = src["lambdas"][n_]
                ev += raw_events("\n".join(Fz.code[s + 1:e]), set(), actions, renames)
            elif n_ in src["frozen_expand"]:
                ev += frozen_events(*src["frozen_expand"][n_])
            else:
                ev.append(e_)
        return ev

    def expand(name, depth=0):
        out = []
        for e_ in raw_events(bodies.get(name, ""), expand_names, actions, renames):
            if e_[0] == "call" and e_[1] in expand_names and e_[1] != name and depth < 20:
                out += expand(e_[1], depth + 1)
            else:
                out.append(e_)
        return out

    n_ev = 0
    for d in src["drivers"]:
        fev = frozen_events(*d["body"])
        nev = expand(d["name"])
        n_ev += len(fev)
        if fev != nev:
            k_ = next((k_ for k_ in range(min(len(fev), len(nev))) if fev[k_] != nev[k_]),
                      min(len(fev), len(nev)))
            problems.append(f"{key}: {d['name']}: event order differs at event {k_} of {len(fev)}/{len(nev)}:\n"
                            f"       frozen: {fev[k_] if k_ < len(fev) else '<end>'}\n"
                            f"       new   : {nev[k_] if k_ < len(nev) else '<end>'}")

    # 9. Header.
    fh = [norm(l) for l in strip_comments(git_show(FROZEN_REV, src["header"])).split("\n") if l.strip()]
    nh = [norm(l) for l in strip_comments(read_new(src["header"])).split("\n") if l.strip()]
    removed = {norm(k_) for k_ in src["header_removed"]}
    j = 0
    for l in fh:
        if l in removed:
            continue
        while j < len(nh) and nh[j] != l:
            j += 1
        if j == len(nh):
            problems.append(f"{key}: frozen header line not kept in order: {l[:100]}")
            break
        j += 1

    summary.append(
        f"{key}: {n_ok} of {len(src['units'])} units verbatim in code, {n_reg} of {len(src['regions'])} "
        f"regions; {n_code} frozen body code lines, {n_mem} members, {len(src['drops'])} declared "
        f"drops, {len(twice)} carried twice; {sum(frozen_c.values())} frozen comment lines, "
        f"{len(lost)} lost; {sum(nlog.values())} log calls; event order over "
        f"{', '.join(d['name'] for d in src['drivers'])}: {n_ev} events")
    return [(u, p) for u in src["units"] for p in u["pieces"]]


def run_checks(verbose):
    problems, summary = [], []
    for key, src in SOURCES.items():
        check_source(key, src, verbose, problems, summary)
    return problems, summary


def mutate_tree(key):
    """Copy every new file to a temporary tree and change one character of a
    frozen-carried code line of the first unit (with an F piece) of source `key`."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="u5t_mut_"))
    for src in SOURCES.values():
        for f in src["new_files"]:
            dst = tmp / f
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(TREE / f, dst)
    src = SOURCES[key]
    u = next(u for u in src["units"] if any(p[0] == "F" for p in u["pieces"]))
    p = tmp / u["file"]
    lines = p.read_text(encoding="utf-8").split("\n")
    code = strip_comments("\n".join(lines)).split("\n")
    start = next(i for i, l in enumerate(code) if re.search(u["def_re"], l))
    blank = blank_literals("\n".join(code)).split("\n")
    o, c = brace_close(blank, start)
    for i in range(o + 1, c):
        m = re.search(r"[<>+\-*/]", code[i])
        if m and not re.match(r"^\s*(?:const )?auto&", code[i]):
            ch = {"<": ">", ">": "<", "+": "-", "-": "+", "*": "/", "/": "*"}[m.group()]
            lines[i] = lines[i][:m.start()] + ch + lines[i][m.start() + 1:]
            print(f"mutation ({key}): {u['file']}:{i + 1}: `{m.group()}` -> `{ch}`")
            break
    p.write_text("\n".join(lines), encoding="utf-8")
    return tmp


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tree", help="read the new files from this directory (a staging copy)")
    ap.add_argument("--mutate", action="store_true",
                    help="also run on a copy with one changed character; that run must fail")
    args = ap.parse_args()
    global TREE
    if args.tree:
        TREE = pathlib.Path(args.tree).resolve()
    problems, summary = run_checks(args.verbose)
    for p in problems:
        print("FAIL " + p)
    print("\nDeclared, for review:")
    for key, src in SOURCES.items():
        for u in src["units"]:
            for p in u["pieces"]:
                if p[0] == "G":
                    print(f"  guard  {key} {u.get('name', '?'):30s} frozen {p[1]}/{p[2]} -> `{p[3]}`")
            for line, a, b in u.get("subs", []):
                print(f"  subst  {key} {u.get('name', '?'):30s} frozen {line}: `{a}` -> `{b}`")
        for r in src["regions"]:
            for line, a, b in r.get("subs", []):
                print(f"  subst  {key} region {r['a']}..{r['b']:<16} frozen {line}: `{a}` -> `{b}`")
        shown = set()
        for i, r in sorted(src["drops"].items()):
            if isinstance(r, tuple):
                if r[1] in shown:
                    continue
                shown.add(r[1])
                lines = sorted(j for j, q in src["drops"].items() if q == r)
                print(f"  drop   {key} frozen {lines[0]}..{lines[-1]}: {r[1]}")
            else:
                print(f"  drop   {key} frozen {i}: {r}")
        for da, db, ca, cb in src.get("duplicates", []):
            print(f"  dup    {key} frozen {da}..{db} identical to frozen {ca}..{cb}")
        for s_, e_ in src.get("event_skip", []):
            print(f"  events {key} frozen {s_}..{e_} not in the frozen event list (deleted code)")
        for i, r in sorted(src["type_changes"].items()):
            print(f"  type   {key} frozen {i}: {r}")
        for c, r in src["comment_edits"].items():
            print(f"  comment edit {key}: {c[:60]} ({r})")
        for k_, v in src["log_edits"].items():
            print(f"  log    {key} {k_[0]} {k_[1][:60]} -> {'deleted' if v is None else v[1][:60]}")
        for k_, v in src["header_removed"].items():
            print(f"  header {key} removed `{k_[:60]}`: {v}")
    print()
    for s in summary:
        print(s)
    ok = not problems
    if args.mutate:
        saved = TREE
        for key in SOURCES:
            TREE = mutate_tree(key)
            mp, _ = run_checks(False)
            TREE = saved
            print(f"mutation run ({key}): {len(mp)} problem(s) -> "
                  f"{'FAIL (as required)' if mp else 'PASS (the mutation was NOT caught)'}")
            for p in mp[:2]:
                print("   " + p.split("\n")[0])
            if not mp:
                ok = False
    print("RESULT", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
